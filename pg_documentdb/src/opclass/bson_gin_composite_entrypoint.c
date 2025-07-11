/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/opclass/bson_gin_composite_entrypoint.c
 *
 *
 * Gin operator implementations of BSON for a composite index
 * See also: https://www.postgresql.org/docs/current/gin-extensibility.html
 *
 *-------------------------------------------------------------------------
 */


 #include <postgres.h>
 #include <fmgr.h>
 #include <miscadmin.h>
 #include <access/reloptions.h>
 #include <executor/executor.h>
 #include <utils/builtins.h>
 #include <utils/typcache.h>
 #include <utils/lsyscache.h>
 #include <utils/syscache.h>
 #include <utils/timestamp.h>
 #include <utils/array.h>
 #include <parser/parse_coerce.h>
 #include <catalog/pg_type.h>
 #include <funcapi.h>
 #include <lib/stringinfo.h>
 #include <nodes/pathnodes.h>

 #include "io/bson_core.h"
 #include "aggregation/bson_query_common.h"
 #include "opclass/bson_gin_common.h"
 #include "opclass/bson_gin_private.h"
 #include "opclass/bson_gin_index_mgmt.h"
 #include "opclass/bson_gin_index_term.h"
 #include "opclass/bson_gin_index_types_core.h"
 #include "query/bson_compare.h"
 #include "utils/documentdb_errors.h"
 #include "metadata/metadata_cache.h"
 #include "collation/collation.h"
 #include "opclass/bson_gin_composite_scan.h"
 #include "opclass/bson_gin_composite_private.h"


/* --------------------------------------------------------- */
/* Top level exports */
/* --------------------------------------------------------- */
PG_FUNCTION_INFO_V1(gin_bson_composite_path_extract_value);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_extract_query);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_compare_partial);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_consistent);
PG_FUNCTION_INFO_V1(gin_bson_composite_path_options);
PG_FUNCTION_INFO_V1(gin_bson_get_composite_path_generated_terms);
PG_FUNCTION_INFO_V1(gin_bson_composite_ordering_transform);

extern bool EnableCollation;
extern bool EnableNewCompositeIndexOpclass;

extern bool RumHasMultiKeyPaths;

static void ValidateCompositePathSpec(const char *prefix);
static Size FillCompositePathSpec(const char *prefix, void *buffer);
static Datum * GenerateCompositeTermsCore(pgbson *doc,
										  BsonGinCompositePathOptions *options,
										  int32_t *nentries);
static int32_t GetIndexPathsFromOptions(BsonGinCompositePathOptions *options,
										const char **indexPaths);
static void ParseBoundsForCompositeOperator(pgbsonelement *singleElement, const
											char **indexPaths, int32_t numPaths,
											VariableIndexBounds *variableBounds);
static bytea * BuildTermForBounds(CompositeQueryRunData *runData,
								  IndexTermCreateMetadata *singlePathMetadata,
								  IndexTermCreateMetadata *compositeMetadata,
								  bool *partialMatch);
static void ParseCompositeQuerySpec(pgbson *querySpec, pgbsonelement *singleElement,
									bool *isMultiKey, bool *isOrderedScan);
static int32_t RunCompareOnBounds(CompositeIndexBounds *bounds, const
								  bson_value_t *compareValue,
								  bool hasEqualityPrefix, bool *priorMatchesEquality);


inline static IndexTermCreateMetadata
GetSinglePathTermCreateMetadata(void *options, int32_t numPaths)
{
	IndexTermCreateMetadata singlePathMetadata = GetIndexTermMetadata(options);
	singlePathMetadata.indexTermSizeLimit = (singlePathMetadata.indexTermSizeLimit /
											 numPaths) - 4;
	return singlePathMetadata;
}


inline static IndexTermCreateMetadata
GetCompositeIndexTermMetadata(void *options)
{
	IndexTermCreateMetadata compositeMetadata = GetIndexTermMetadata(options);
	compositeMetadata.indexTermSizeLimit = -1;
	return compositeMetadata;
}


/*
 * gin_bson_composite_path_extract_value is run on the insert/update path and collects the terms
 * that will be indexed for indexes for a single path definition. the method provides the bson document as an input, and
 * can return as many terms as is necessary (1:N).
 * For more details see documentation on the 'extractValue' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_extract_value(PG_FUNCTION_ARGS)
{
	pgbson *bson = PG_GETARG_PGBSON_PACKED(0);
	int32_t *nentries = (int32_t *) PG_GETARG_POINTER(1);
	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	Datum *indexEntries = GenerateCompositeTermsCore(bson, options, nentries);
	PG_RETURN_POINTER(indexEntries);
}


/*
 * gin_bson_composite_path_extract_query is run on the query path when a predicate could be pushed
 * to the index. The predicate and the "strategy" based on the operator is passed down.
 * In the operator class, the OPERATOR index maps to the strategy index presented here.
 * The method then returns a set of terms that are valid for that predicate and strategy.
 * For more details see documentation on the 'extractQuery' method in the GIN extensibility.
 * TODO: Today this recurses through the given document fully. We would need to implement
 * something that recurses down 1 level of objects & arrays for a given path unless it's a wildcard
 * index.
 */
Datum
gin_bson_composite_path_extract_query(PG_FUNCTION_ARGS)
{
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	pgbson *query = PG_GETARG_PGBSON(0);
	int32 *nentries = (int32 *) PG_GETARG_POINTER(1);
	bool **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	if (!PG_HAS_OPCLASS_OPTIONS())
	{
		ereport(ERROR, (errmsg("Index does not have options")));
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	/* We need to handle this case for amcostestimate - let
	 * compare partial and consistent handle failures.
	 */
	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);
	IndexTermCreateMetadata singlePathMetadata = GetSinglePathTermCreateMetadata(options,
																				 numPaths);
	IndexTermCreateMetadata compositeMetadata = GetCompositeIndexTermMetadata(options);


	if (strategy == BSON_INDEX_STRATEGY_IS_MULTIKEY)
	{
		/* Consider only the root multi-key term */
		*nentries = 1;
		Datum *result = palloc(sizeof(Datum));
		result[0] = GenerateRootMultiKeyTerm(&compositeMetadata);
		PG_RETURN_POINTER(result);
	}

	VariableIndexBounds variableBounds = { 0 };

	CompositeQueryMetaInfo *metaInfo =
		(CompositeQueryMetaInfo *) palloc0(sizeof(CompositeQueryMetaInfo));
	CompositeQueryRunData *runData = (CompositeQueryRunData *) palloc0(
		sizeof(CompositeQueryRunData));
	runData->metaInfo = metaInfo;
	runData->numIndexPaths = numPaths;

	/* Default to assuming array paths (we can do better if told otherwise) */
	bool hasArrayPaths = true;
	bool isOrderedScan = false;

	/* Round 1, collect fixed index bounds and collect variable index bounds */
	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		/* Could be for cost estimate or regular index
		 * in this path, just treat it as valid. let
		 * compare partial and consistent handle errors.
		 */

		pgbsonelement singleElement;
		PgbsonToSinglePgbsonElement(query, &singleElement);

		ParseOperatorStrategy(indexPaths, numPaths, &singleElement, strategy,
							  &variableBounds);
	}
	else
	{
		pgbsonelement singleElement;
		ParseCompositeQuerySpec(query, &singleElement, &hasArrayPaths, &isOrderedScan);
		ParseBoundsForCompositeOperator(&singleElement, indexPaths, numPaths,
										&variableBounds);
	}


	/* First thing to check: Optimization - if no arrays and there are bounds with 1 bound
	 * add it to the global bounds
	 * If we don't have arrays, and there's exactly 1 boundary,
	 * We can apply it to the global bounds, and skip this key
	 */
	if (!hasArrayPaths)
	{
		MergeSingleVariableBounds(&variableBounds, runData);
	}
	else if (isOrderedScan)
	{
		PickVariableBoundsForOrderedScan(&variableBounds, runData);
	}

	/* Tally up the total variable bound counts - this is the permutation of all variable terms
	 * e.g. if we have { "a": { "$in": [ 1, 2, 3 ]}} && { "b": { "$in": [ 4, 5 ] } }
	 * That would generate 6 possible terms.
	 * Similarly if we have
	 * { "a": { "$in": [ 1, 2, 3 ]}} && { "a": { "$ngt": 2 } }
	 * even though we can simplify it statically, we choose to permute and generate 6 terms
	 * with each of the boundaries.
	 */
	int32_t totalPathTerms = 1;

	/* These are the scan keys to validate in consistent checks */
	runData->metaInfo->numScanKeys = list_length(variableBounds.variableBoundsList);
	PathScanTermMap pathScanTermMap[INDEX_MAX_KEYS] = { 0 };
	bool hasMultipleScanKeysPerPath = false;
	if (runData->metaInfo->numScanKeys > 0)
	{
		runData->metaInfo->scanKeyMap = palloc0(sizeof(PathScanKeyMap) *
												runData->metaInfo->numScanKeys);

		/* First pass - aggregate per path */
		ListCell *cell;
		foreach(cell, variableBounds.variableBoundsList)
		{
			CompositeIndexBoundsSet *set =
				(CompositeIndexBoundsSet *) lfirst(cell);

			if (set->numBounds == 0)
			{
				/* If one scanKey is unsatisfiable then the query is not satisfiable */
				totalPathTerms = 0;
			}

			/* Add the index to the current key */
			pathScanTermMap[set->indexAttribute].scanKeyIndexList =
				lappend_int(pathScanTermMap[set->indexAttribute].scanKeyIndexList,
							foreach_current_index(cell));
			pathScanTermMap[set->indexAttribute].numTermsPerPath += set->numBounds;
		}

		/* Second phase, calculate total term count */
		for (int i = 0; i < numPaths; i++)
		{
			if (pathScanTermMap[i].numTermsPerPath > 0)
			{
				/* Check if any paths have multiple keys */
				hasMultipleScanKeysPerPath = hasMultipleScanKeysPerPath ||
											 (list_length(
												  pathScanTermMap[i].scanKeyIndexList) >
											  1);
				totalPathTerms = totalPathTerms * pathScanTermMap[i].numTermsPerPath;
			}
		}
	}

	runData->metaInfo->hasMultipleScanKeysPerPath = hasMultipleScanKeysPerPath;
	*nentries = totalPathTerms;
	*partialmatch = (bool *) palloc0(sizeof(bool) * (totalPathTerms + 1));
	*extra_data = palloc0(sizeof(Pointer) * (totalPathTerms + 1));
	Pointer *extraDataArray = *extra_data;
	Datum *entries = (Datum *) palloc(sizeof(Datum) * (totalPathTerms + 1));

	if (variableBounds.variableBoundsList == NIL)
	{
		bytea *term = BuildTermForBounds(runData, &singlePathMetadata, &compositeMetadata,
										 &(*partialmatch)[0]);
		extraDataArray[0] = (Pointer) runData;
		entries[0] = PointerGetDatum(term);
	}
	else
	{
		for (int i = 0; i < totalPathTerms; i++)
		{
			/* for each of the terms to generate, walk *one* of each CompositePathSet */
			int currentTerm = i;

			/* First create a copy of rundata */
			CompositeQueryRunData *runDataCopy = (CompositeQueryRunData *) palloc0(
				sizeof(CompositeQueryRunData));
			memcpy(runDataCopy, runData, sizeof(CompositeQueryRunData));

			UpdateRunDataForVariableBounds(runDataCopy, pathScanTermMap, &variableBounds,
										   currentTerm);
			bytea *term = BuildTermForBounds(runDataCopy, &singlePathMetadata,
											 &compositeMetadata,
											 &(*partialmatch)[i]);

			extraDataArray[i] = (Pointer) runDataCopy;
			entries[i] = PointerGetDatum(term);
		}
	}

	if (runData->metaInfo->hasTruncation && !isOrderedScan)
	{
		*nentries = totalPathTerms + 1;
		metaInfo->truncationTermIndex = totalPathTerms;
		entries[totalPathTerms] = GenerateRootTruncatedTerm(&compositeMetadata);
		(*partialmatch)[totalPathTerms] = false;
		extraDataArray[totalPathTerms] = NULL;  /* no extra data for the truncated term */
	}

	PG_RETURN_POINTER(entries);
}


/*
 * gin_bson_composite_path_compare_partial is run on the query path when extract_query requests a partial
 * match on the index. Each index term that has a partial match (with the lower bound as a
 * starting point) will be an input to this method. compare_partial will return '0' if the term
 * is a match, '-1' if the term is not a match but enumeration should continue, and '1' if
 * enumeration should stop. Note that enumeration may happen multiple times - this sorted enumeration
 * happens once per GIN page so there may be several sequences of [-1, 0]* -> 1 per query.
 * The strategy passed in will map to the index of the Operator on the OPERATOR class definition
 * For more details see documentation on the 'comparePartial' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_compare_partial(PG_FUNCTION_ARGS)
{
	/* 0 will be the value we passed in for the extract query */
	/* bytea *queryValue = PG_GETARG_BYTEA_PP(0); */

	/* 1 is the value in the index we want to compare against. */
	bytea *compareValue = PG_GETARG_BYTEA_PP(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	Pointer extraData = PG_GETARG_POINTER(3);

	CompositeQueryRunData *runData = (CompositeQueryRunData *) extraData;

	BsonIndexTerm compareTerm[INDEX_MAX_KEYS] = { 0 };
	int32_t numTerms = InitializeCompositeIndexTerm(compareValue, compareTerm);

	if (numTerms != runData->numIndexPaths)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Number of terms in the index term (%d) does not match "
							   "the number of index paths (%d)",
							   numTerms, runData->numIndexPaths)));
	}

	if (strategy == BSON_INDEX_STRATEGY_DOLLAR_ORDERBY)
	{
		/* use order by key to signal truncation status of ordering */
		/* TODO(Orderby): Support ordering on subsequent keys */
		for (int i = 0; i < runData->numIndexPaths; i++)
		{
			if (compareTerm[i].isIndexTermTruncated)
			{
				PG_RETURN_INT32(-1);
			}
		}

		PG_RETURN_INT32(1);
	}

	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Composite index does not support strategy %d",
							   strategy)));
	}

	bool priorMatchesEquality = true;
	bool hasEqualityPrefix = true;
	for (int32_t compareIndex = 0; compareIndex < runData->numIndexPaths; compareIndex++)
	{
		hasEqualityPrefix = hasEqualityPrefix && priorMatchesEquality;
		const bson_value_t *compareValue = &compareTerm[compareIndex].element.bsonValue;
		int32_t compareInBounds = RunCompareOnBounds(
			&runData->indexBounds[compareIndex],
			compareValue,
			hasEqualityPrefix,
			&priorMatchesEquality);
		if (compareInBounds != 0)
		{
			PG_RETURN_INT32(compareInBounds);
		}

		if (runData->indexBounds[compareIndex].indexRecheckFunctions != NIL)
		{
			ListCell *recheckFuncs;
			foreach(recheckFuncs,
					runData->indexBounds[compareIndex].indexRecheckFunctions)
			{
				IndexRecheckArgs *recheckStrategy = lfirst(recheckFuncs);
				if (!IsValidRecheckForIndexValue(&compareTerm[compareIndex],
												 recheckStrategy))
				{
					PG_RETURN_INT32(-1);
				}
			}
		}
	}

	PG_RETURN_INT32(0);
}


/*
 * When running compare_partial, we first check if the current term matches
 * based purely on the lower and upper bounds.
 * Returns 0 if true, -1/1 if we need to bail.
 * If we do have a match, further checks can be made for scenarios like
 * Index rechecks.
 */
static int32_t
RunCompareOnBounds(CompositeIndexBounds *bounds, const bson_value_t *compareValue,
				   bool hasEqualityPrefix, bool *priorMatchesEquality)
{
	if (bounds->isEqualityBound)
	{
		/* We have an equality on a term - if not equal - we can bail */
		bool isComparisonValid = false;
		int32_t compareBounds = CompareBsonValueAndType(
			compareValue,
			&bounds->lowerBound.processedBoundValue,
			&isComparisonValid);

		/* If we're an equality and we're less than the lower bound, this
		 * is an order by situation, and we need to keep searching.
		 */
		if (compareBounds < 0)
		{
			return -1;
		}
		else if (compareBounds > 0)
		{
			/* Stop the search */
			return hasEqualityPrefix ? 1 : -1;
		}

		return 0;
	}

	*priorMatchesEquality = false;
	if (bounds->lowerBound.bound.value_type != BSON_TYPE_EOD)
	{
		bool isComparisonValid = false;
		int32_t compareBounds = CompareBsonValueAndType(
			compareValue,
			&bounds->lowerBound.processedBoundValue,
			&isComparisonValid);
		if (!isComparisonValid)
		{
			return -1;
		}

		if (compareBounds == 0)
		{
			if (!bounds->lowerBound.isBoundInclusive &&
				!bounds->lowerBound.isProcessedValueTruncated)
			{
				return -1;
			}
		}
		else if (compareBounds < 0)
		{
			/* compareValue < lowerBound, not a match */
			return -1;
		}
	}

	if (bounds->upperBound.bound.value_type != BSON_TYPE_EOD)
	{
		bool isComparisonValid = false;
		int32_t compareBounds = CompareBsonValueAndType(
			compareValue,
			&bounds->upperBound.processedBoundValue,
			&isComparisonValid);
		if (!isComparisonValid)
		{
			return -1;
		}

		if (compareBounds == 0)
		{
			if (!bounds->upperBound.isBoundInclusive &&
				!bounds->upperBound.isProcessedValueTruncated)
			{
				return -1;
			}
		}
		else if (compareBounds > 0)
		{
			/* Can stop searching */
			return hasEqualityPrefix ? 1 : -1;
		}
	}

	return 0;
}


/*
 * gin_bson_composite_path_consistent validates whether a given match on a key
 * can be used to satisfy a query. given an array of queryKeys and
 * an array of 'check' that indicates whether that queryKey matched
 * exactly for the check. it allows for the gin index to do a full
 * runtime check for partial matches (recheck) or to accept that the term was a
 * hit for the query.
 * For more details see documentation on the 'consistent' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_consistent(PG_FUNCTION_ARGS)
{
	bool *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* int32_t numKeys = (int32_t) PG_GETARG_INT32(3); */
	Pointer *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool *recheck = (bool *) PG_GETARG_POINTER(5);       /* out param. */
	/* Datum *queryKeys = (Datum *) PG_GETARG_POINTER(6); */

	if (strategy == BSON_INDEX_STRATEGY_IS_MULTIKEY)
	{
		*recheck = false;
		PG_RETURN_BOOL(check[0]);
	}

	if (strategy != BSON_INDEX_STRATEGY_COMPOSITE_QUERY)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Composite index does not support strategy %d",
							   strategy)));
	}

	CompositeQueryRunData *runData = (CompositeQueryRunData *) extra_data[0];

	/* If operators specifically required runtime recheck honor it */
	*recheck = runData->metaInfo->requiresRuntimeRecheck;

	if (runData->metaInfo->hasTruncation &&
		check[runData->metaInfo->truncationTermIndex])
	{
		*recheck = true;
	}

	if (!runData->metaInfo->hasMultipleScanKeysPerPath &&
		!runData->metaInfo->hasTruncation)
	{
		/* No truncation and each path has exactly 1 scan key to it
		 * At this point, any matching entry matches the top level query
		 * so we can just return early.
		 */
		PG_RETURN_BOOL(true);
	}

	if (runData->metaInfo->numScanKeys == 0)
	{
		/* No scan keys, so we can just return true */
		PG_RETURN_BOOL(check[0]);
	}

	/* Walk the scan keys and ensure every one is matched */
	bool innerResult = runData->metaInfo->numScanKeys > 0;
	for (int i = 0; i < runData->metaInfo->numScanKeys && innerResult; i++)
	{
		if (list_length(runData->metaInfo->scanKeyMap[i].scanIndices) == 0)
		{
			/* unsatisfiable key */
			innerResult = false;
			break;
		}

		bool keyMatched = false;
		ListCell *scanCell;
		foreach(scanCell, runData->metaInfo->scanKeyMap[i].scanIndices)
		{
			int32_t scanTerm = lfirst_int(scanCell);
			if (check[scanTerm])
			{
				keyMatched = true;
				break;
			}
		}

		if (!keyMatched)
		{
			innerResult = false;
		}
	}

	PG_RETURN_BOOL(innerResult);
}


/*
 * gin_bson_get_composite_path_generated_terms is an internal utility function that allows to retrieve
 * the set of terms that *would* be inserted in the index for a given document for a single
 * path index option specification.
 * The function gets a document, path, and if it's a wildcard, and sets up the index structures
 * to call 'generateTerms' and returns it as a SETOF records.
 *
 * gin_bson_get_composite_path_generated_terms(
 *      document bson,
 *      pathSpec text,
 *      termLength int)
 *
 */
Datum
gin_bson_get_composite_path_generated_terms(PG_FUNCTION_ARGS)
{
	FuncCallContext *functionContext;
	GenerateTermsContext *context;

	bool addMetadata = PG_GETARG_BOOL(3);
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		pgbson *document = PG_GETARG_PGBSON(0);
		char *pathSpec = text_to_cstring(PG_GETARG_TEXT_P(1));
		int32_t truncationLimit = PG_GETARG_INT32(2);

		functionContext = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(functionContext->multi_call_memory_ctx);

		Size fieldSize = FillCompositePathSpec(pathSpec, NULL);
		BsonGinCompositePathOptions *options = palloc0(
			sizeof(BsonGinCompositePathOptions) + fieldSize);
		options->base.indexTermTruncateLimit = truncationLimit;
		options->base.type = IndexOptionsType_Composite;
		options->base.version = IndexOptionsVersion_V0;
		options->compositePathSpec = sizeof(BsonGinCompositePathOptions);

		FillCompositePathSpec(
			pathSpec,
			((char *) options) + sizeof(BsonGinCompositePathOptions));

		context = (GenerateTermsContext *) palloc0(sizeof(GenerateTermsContext));
		context->terms.entries = GenerateCompositeTermsCore(document, options,
															&context->totalTermCount);
		context->index = 0;
		MemoryContextSwitchTo(oldcontext);
		functionContext->user_fctx = (void *) context;
	}

	functionContext = SRF_PERCALL_SETUP();
	context = (GenerateTermsContext *) functionContext->user_fctx;

	if (context->index < context->totalTermCount)
	{
		Datum next = context->terms.entries[context->index++];
		BsonIndexTerm term[INDEX_MAX_KEYS] = { 0 };
		bytea *serializedTerm = DatumGetByteaPP(next);
		int32_t numKeys = InitializeCompositeIndexTerm(serializedTerm, term);

		/* By default we only print out the index term. If addMetadata is set, then we
		 * also append the bson metadata for the index term to the final output.
		 * This includes things like whether or not the term is truncated
		 */
		pgbson_writer writer;
		PgbsonWriterInit(&writer);

		if (!IsSerializedIndexTermComposite(serializedTerm))
		{
			PgbsonWriterAppendValue(&writer, term[0].element.path,
									term[0].element.pathLength,
									&term[0].element.bsonValue);
			if (addMetadata)
			{
				PgbsonWriterAppendBool(&writer, "t", 1, term[0].isIndexTermTruncated);
			}
		}
		else
		{
			/* If this is a single path index term, we just return the value */
			pgbson_array_writer arrayWriter;
			PgbsonWriterStartArray(&writer, "$", 1, &arrayWriter);
			for (int i = 0; i < numKeys; i++)
			{
				if (!addMetadata)
				{
					/* If we don't add metadata, we just return the term */
					PgbsonArrayWriterWriteValue(&arrayWriter, &term[i].element.bsonValue);
				}
				else
				{
					pgbson_writer termWriter;
					PgbsonArrayWriterStartDocument(&arrayWriter, &termWriter);
					PgbsonWriterAppendValue(&termWriter, term[i].element.path,
											term[i].element.pathLength,
											&term[i].element.bsonValue);
					PgbsonWriterAppendBool(&termWriter, "t", 1,
										   term[i].isIndexTermTruncated);
					PgbsonArrayWriterEndDocument(&arrayWriter, &termWriter);
				}
			}

			PgbsonWriterEndArray(&writer, &arrayWriter);
		}

		SRF_RETURN_NEXT(functionContext, PointerGetDatum(PgbsonWriterGetPgbson(&writer)));
	}

	SRF_RETURN_DONE(functionContext);
}


Datum
gin_bson_composite_ordering_transform(PG_FUNCTION_ARGS)
{
	bytea *compareValue = PG_GETARG_BYTEA_PP(0);
	pgbson *queryValue = PG_GETARG_PGBSON_PACKED(1);

	/* StrategyNumber strategy = PG_GETARG_UINT16(2); */
	Datum currentKey = PG_GETARG_DATUM(3);

	if (currentKey != (Datum) 0)
	{
		pgbson *currentOrdering = DatumGetPgBsonPacked(currentKey);
		pfree(currentOrdering);
	}

	pgbsonelement sortElement;
	if (!TryGetSinglePgbsonElementFromPgbson(queryValue, &sortElement))
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg(
							"Invalid query value for ordering transform - only 1 path is supported")));
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	/* We need to handle this case for amcostestimate - let
	 * compare partial and consistent handle failures.
	 */
	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);

	/* Match the order by column to the index path */
	int orderbyIndexPath = -1;
	for (int i = 0; i < numPaths; i++)
	{
		if (strcmp(sortElement.path, indexPaths[i]) == 0)
		{
			orderbyIndexPath = i;
			break;
		}
	}

	if (orderbyIndexPath < 0)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Order by path '%s' does not match any index path",
							   sortElement.path)));
	}

	/* For ordering we only support 1 column
	 * TODO(Orderby) fix this.
	 */
	BsonIndexTerm compareTerm[INDEX_MAX_KEYS] = { 0 };
	int32_t numPathsInIndex = InitializeCompositeIndexTerm(compareValue, compareTerm);
	if (numPathsInIndex != numPaths)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR),
						errmsg("Number of terms in the index term (%d) does not match "
							   "the number of index paths (%d)",
							   numPathsInIndex, numPaths)));
	}

	/* Match the runtime format of order by */
	pgbson_writer writer;
	PgbsonWriterInit(&writer);
	PgbsonWriterAppendValue(&writer, sortElement.path, sortElement.pathLength,
							&compareTerm[orderbyIndexPath].element.bsonValue);
	PG_FREE_IF_COPY(compareValue, 0);
	PG_RETURN_POINTER(PgbsonWriterGetPgbson(&writer));
}


/*
 * gin_bson_composite_path_options sets up the option specification for single field indexes
 * This initializes the structure that is used by the Index AM to process user specified
 * options on how to handle documents with the index.
 * For single field indexes we only need to track the path being indexed, and whether or not
 * it's a wildcard.
 * usage is as: using gin(document bson_gin_single_path_ops(path='a.b',iswildcard=true))
 * For more details see documentation on the 'options' method in the GIN extensibility.
 */
Datum
gin_bson_composite_path_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(BsonGinCompositePathOptions));

	/* add an option that has a default value of single path and accepts *one* value
	 *  This is used later to key off whether it's a single path or multi-key wildcard index options */
	add_local_int_reloption(relopts, "optionsType",
							"The type of the options struct.",
							IndexOptionsType_Composite,  /* default value */
							IndexOptionsType_Composite,  /* min */
							IndexOptionsType_Composite,  /* max */
							offsetof(BsonGinCompositePathOptions, base.type));
	add_local_string_reloption(relopts, "pathspec",
							   "Composite path array for the index",
							   NULL, &ValidateCompositePathSpec, &FillCompositePathSpec,
							   offsetof(BsonGinCompositePathOptions, compositePathSpec));
	add_local_int_reloption(relopts, "tl",
							"The index term size limit for truncation.",
							-1,  /* default value */
							-1,  /* min */
							INT32_MAX,  /* max */
							offsetof(BsonGinCompositePathOptions,
									 base.indexTermTruncateLimit));
	add_local_int_reloption(relopts, "v",
							"The version of the options struct.",
							IndexOptionsVersion_V0,          /* default value */
							IndexOptionsVersion_V0,          /* min */
							IndexOptionsVersion_V1,          /* max */
							offsetof(BsonGinCompositePathOptions, base.version));

	PG_RETURN_VOID();
}


static bool
IsBsonDollarNinArrayContainsArrays(const bson_value_t *bsonValue)
{
	bson_iter_t iter;
	BsonValueInitIterator(bsonValue, &iter);
	while (bson_iter_next(&iter))
	{
		if (BSON_ITER_HOLDS_ARRAY(&iter))
		{
			/* If we have an array, we cannot push down the $nin */
			return true;
		}
	}

	return false;
}


int32_t
GetCompositeOpClassColumnNumber(const char *currentPath, void *contextOptions)
{
	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) contextOptions;

	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);
	for (int32_t i = 0; i < numPaths; i++)
	{
		if (strcmp(currentPath, indexPaths[i]) == 0)
		{
			return i;
		}
	}

	return -1;
}


IndexTraverseOption
GetCompositePathIndexTraverseOption(BsonIndexStrategy strategy, void *contextOptions,
									const char *currentPath,
									uint32_t currentPathLength,
									const bson_value_t *bsonValue,
									int32_t *compositeIndexCol)
{
	if (!EnableNewCompositeIndexOpclass)
	{
		return IndexTraverse_Invalid;
	}

	if (bsonValue->value_type == BSON_TYPE_ARRAY)
	{
		/*
		 * For queries targetting arrays, the following operators cannot be served by the index:
		 * These are because negation operators like $nin, $not, $ne cannot detect these in the index
		 * since we don't index the raw array value.
		 */
		if (strategy == BSON_INDEX_STRATEGY_DOLLAR_NOT_IN)
		{
			/*
			 * Need to check if the array has an array terms. if it does
			 * we can't push down.
			 */
			if (IsBsonDollarNinArrayContainsArrays(bsonValue))
			{
				return IndexTraverse_Invalid;
			}
		}
		else if (IsNegationStrategy(strategy))
		{
			return IndexTraverse_Invalid;
		}
	}

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) contextOptions;

	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);
	for (int32_t i = 0; i < numPaths; i++)
	{
		if (strcmp(currentPath, indexPaths[i]) == 0)
		{
			*compositeIndexCol = i;
			return IndexTraverse_Match;
		}
	}

	return IndexTraverse_Invalid;
}


bool
GetEqualityRangePredicatesForIndexPath(IndexPath *indexPath, void *options,
									   bool equalityPrefixes[INDEX_MAX_KEYS],
									   bool nonEqualityPrefixes[INDEX_MAX_KEYS])
{
	/*
	 * We're a multi-key index, or order by on the nth column.
	 */
	ListCell *cell;
	foreach(cell, indexPath->indexclauses)
	{
		IndexClause *indexClause = (IndexClause *) lfirst(cell);
		ListCell *iclauseCell;
		foreach(iclauseCell, indexClause->indexquals)
		{
			RestrictInfo *qual = (RestrictInfo *) lfirst(iclauseCell);
			if (IsA(qual->clause, OpExpr))
			{
				OpExpr *expr = (OpExpr *) qual->clause;
				Expr *queryVal = lsecond(expr->args);
				if (!IsA(queryVal, Const))
				{
					/* If the query value is not a constant, we can't push down */
					return false;
				}

				Const *queryConst = (Const *) queryVal;
				pgbson *queryBson = DatumGetPgBson(queryConst->constvalue);

				pgbsonelement queryElement;
				PgbsonToSinglePgbsonElement(queryBson, &queryElement);

				const MongoIndexOperatorInfo *info =
					GetMongoIndexOperatorByPostgresOperatorId(expr->opno);

				if (info->indexStrategy == BSON_INDEX_STRATEGY_INVALID)
				{
					/* This could be a full scan with $range, check on that */
					DollarRangeParams rangeParams = { 0 };
					InitializeQueryDollarRange(&queryElement, &rangeParams);
					if (rangeParams.isFullScan)
					{
						/* This is neither equality nor inequality */
						continue;
					}
				}

				int32_t filterColumn = -1;
				GetCompositePathIndexTraverseOption(
					info->indexStrategy,
					options,
					queryElement.path,
					queryElement.pathLength,
					&queryElement.bsonValue,
					&filterColumn);

				if (filterColumn < 0 || filterColumn >= INDEX_MAX_KEYS)
				{
					return false;
				}

				switch (info->indexStrategy)
				{
					case BSON_INDEX_STRATEGY_DOLLAR_EQUAL:
					{
						equalityPrefixes[filterColumn] = true;
						break;
					}

					case BSON_INDEX_STRATEGY_DOLLAR_RANGE:
					{
						DollarRangeParams rangeParams = { 0 };
						InitializeQueryDollarRange(&queryElement, &rangeParams);
						if (!rangeParams.isFullScan)
						{
							nonEqualityPrefixes[filterColumn] = true;
						}
						break;
					}

					default:
					{
						/* Track the filters as being a non-equality (range predicate) */
						nonEqualityPrefixes[filterColumn] = true;
						break;
					}
				}
			}
			else
			{
				return false;
			}
		}
	}

	return true;
}


char *
SerializeBoundsStringForExplain(bytea *entry, void *extraData, PG_FUNCTION_ARGS)
{
	CompositeQueryRunData *runData = (CompositeQueryRunData *) extraData;

	BsonGinCompositePathOptions *options =
		(BsonGinCompositePathOptions *) PG_GET_OPCLASS_OPTIONS();

	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	int numPaths = GetIndexPathsFromOptions(
		options,
		indexPaths);
	if (numPaths != runData->numIndexPaths)
	{
		return "";
	}

	StringInfo s = makeStringInfo();
	appendStringInfoString(s, "[");
	for (int i = 0; i < runData->numIndexPaths; i++)
	{
		if (i > 0)
		{
			appendStringInfoString(s, ", ");
		}

		appendStringInfo(s, "\"%s\": %s",
						 indexPaths[i],
						 runData->indexBounds[i].lowerBound.isBoundInclusive ? "[" : "(");
		if (runData->indexBounds[i].lowerBound.bound.value_type == BSON_TYPE_EOD ||
			runData->indexBounds[i].lowerBound.bound.value_type == BSON_TYPE_MINKEY)
		{
			appendStringInfoString(s, "MinKey");
		}
		else
		{
			appendStringInfo(s, "%s", BsonValueToJsonForLogging(
								 &runData->indexBounds[i].lowerBound.bound));
		}

		appendStringInfo(s, ", ");

		if (runData->indexBounds[i].upperBound.bound.value_type == BSON_TYPE_EOD ||
			runData->indexBounds[i].upperBound.bound.value_type == BSON_TYPE_MAXKEY)
		{
			appendStringInfoString(s, "MaxKey");
		}
		else
		{
			appendStringInfo(s, "%s", BsonValueToJsonForLogging(
								 &runData->indexBounds[i].upperBound.bound));
		}

		appendStringInfo(s, "%s",
						 runData->indexBounds[i].upperBound.isBoundInclusive ? "]" : ")");
	}
	appendStringInfoString(s, "]");

	return s->data;
}


void
ModifyScanKeysForCompositeScan(ScanKey scankey, int nscankeys, ScanKey targetScanKey,
							   bool hasArrayKeys, bool hasOrderBys)
{
	pgbson_writer querySpecWriter;
	PgbsonWriterInit(&querySpecWriter);

	pgbson_array_writer queryWriter;
	PgbsonWriterStartArray(&querySpecWriter, "q", 1, &queryWriter);

	for (int i = 0; i < nscankeys; i++)
	{
		Datum scanKeyArg = scankey[i].sk_argument;
		BsonIndexStrategy strategy = scankey[i].sk_strategy;
		pgbson *secondBson = DatumGetPgBson(scanKeyArg);

		pgbson_writer clauseWriter;
		PgbsonArrayWriterStartDocument(&queryWriter, &clauseWriter);
		PgbsonWriterAppendInt32(&clauseWriter, "op", 2,
								strategy);
		PgbsonWriterConcat(&clauseWriter, secondBson);
		PgbsonArrayWriterEndDocument(&queryWriter, &clauseWriter);
	}

	PgbsonWriterEndArray(&querySpecWriter, &queryWriter);
	PgbsonWriterAppendBool(&querySpecWriter, "m", 1, hasArrayKeys);
	PgbsonWriterAppendBool(&querySpecWriter, "or", 2, hasOrderBys);

	Datum finalDatum = PointerGetDatum(
		PgbsonWriterGetPgbson(&querySpecWriter));

	/* Now update all the scan keys */
	if (nscankeys > 0)
	{
		memcpy(targetScanKey, scankey, sizeof(ScanKeyData));
	}
	else
	{
		memset(targetScanKey, 0, sizeof(ScanKeyData));
		targetScanKey->sk_attno = 1;
	}

	targetScanKey->sk_argument = finalDatum;
	targetScanKey->sk_strategy = BSON_INDEX_STRATEGY_COMPOSITE_QUERY;
}


Datum
BuildCompositeOrderByScanKeyArgument(bytea *options)
{
	const char *indexPaths[INDEX_MAX_KEYS] = { 0 };

	GetIndexPathsFromOptions(
		(BsonGinCompositePathOptions *) options,
		indexPaths);

	pgbsonelement sortElement = { 0 };
	sortElement.path = indexPaths[0];
	sortElement.pathLength = strlen(indexPaths[0]);
	sortElement.bsonValue.value_type = BSON_TYPE_INT32;
	sortElement.bsonValue.value.v_int32 = 1;  /* Default value for order by */

	return PointerGetDatum(PgbsonElementToPgbson(&sortElement));
}


static void
ParseCompositeQuerySpec(pgbson *querySpec, pgbsonelement *singleElement,
						bool *isMultiKey, bool *isOrderBy)
{
	bson_iter_t queryIter;
	PgbsonInitIterator(querySpec, &queryIter);

	/* Default assumption is that it's multi-key unless otherwise specified */
	*isMultiKey = true;
	*isOrderBy = false;
	while (bson_iter_next(&queryIter))
	{
		const char *key = bson_iter_key(&queryIter);
		if (strcmp(key, "q") == 0)
		{
			singleElement->path = key;
			singleElement->pathLength = 1;
			singleElement->bsonValue = *bson_iter_value(&queryIter);
		}
		else if (strcmp(key, "m") == 0)
		{
			*isMultiKey = bson_iter_bool(&queryIter);
		}
		else if (strcmp(key, "or") == 0)
		{
			*isOrderBy = bson_iter_bool(&queryIter);
		}
		else
		{
			ereport(ERROR, (errmsg("Unknown key for composite query %s", key)));
		}
	}
}


/* --------------------------------------------------------- */
/* Private helper methods */
/* --------------------------------------------------------- */


/*
 * Callback that validates a user provided wildcard projection prefix
 * This is called on CREATE INDEX when a specific wildcard projection is provided.
 * We do minimal sanity validation here and instead use the Fill method to do final validation.
 */
static void
ValidateCompositePathSpec(const char *prefix)
{
	if (prefix == NULL)
	{
		/* validate can be called with the default value NULL. */
		return;
	}

	int32_t stringLength = strlen(prefix);
	if (stringLength < 3)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"at least one filter path must be specified")));
	}
}


/*
 * Callback that updates the single path data into the serialized,
 * post-processed options structure - this is used later in term generation
 * through PG_GET_OPCLASS_OPTIONS().
 * This is called on CREATE INDEX to set up the serialized structure.
 * This function is called twice
 * - once with buffer being NULL (to get alloc size)
 * - once again with the buffer that should be serialized.
 * Here we parse the jsonified path options to build a serialized path
 * structure that is more efficiently parsed during term generation.
 */
static Size
FillCompositePathSpec(const char *prefix, void *buffer)
{
	if (prefix == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
							"at least one filter path must be specified")));
	}

	pgbson *bson = PgbsonInitFromJson(prefix);
	uint32_t pathCount = 0;
	bson_iter_t bsonIterator;

	/* serialized length - start with the total term count. */
	uint32_t totalSize = sizeof(uint32_t);
	PgbsonInitIterator(bson, &bsonIterator);
	while (bson_iter_next(&bsonIterator))
	{
		if (!BSON_ITER_HOLDS_UTF8(&bsonIterator))
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"filter must have a valid string path")));
		}

		uint32_t pathLength;
		bson_iter_utf8(&bsonIterator, &pathLength);
		if (pathLength == 0)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_BADVALUE), errmsg(
								"filter must have a valid path")));
		}

		pathCount++;

		/* add the prefixed path length */
		totalSize += sizeof(uint32_t);

		/* add the path size */
		totalSize += pathLength;

		/* Add the null terminator */
		totalSize += 1;
	}

	if (buffer != NULL)
	{
		PgbsonInitIterator(bson, &bsonIterator);
		char *bufferPtr = (char *) buffer;
		*((uint32_t *) bufferPtr) = pathCount;
		bufferPtr += sizeof(uint32_t);

		while (bson_iter_next(&bsonIterator))
		{
			uint32_t pathLength;
			const char *path = bson_iter_utf8(&bsonIterator, &pathLength);

			/* add the prefixed path length */
			*((uint32_t *) bufferPtr) = pathLength;
			bufferPtr += sizeof(uint32_t);

			/* add the serialized string */
			memcpy(bufferPtr, path, pathLength);
			bufferPtr += pathLength;

			*bufferPtr = 0;
			bufferPtr++;
		}
	}

	return totalSize;
}


static Datum *
GenerateCompositeTermsCore(pgbson *bson, BsonGinCompositePathOptions *options,
						   int32_t *nentries)
{
	uint32_t pathCount;
	const char *pathSpecBytes;
	Get_Index_Path_Option(options, compositePathSpec, pathSpecBytes, pathCount);

	Datum **entries = palloc(sizeof(Datum *) * pathCount);
	int32_t *entryCounts = palloc0(sizeof(int32_t) * pathCount);

	uint32_t totalTermCount = 1;
	for (uint32_t i = 0; i < pathCount; i++)
	{
		uint32_t indexPathLength = *(uint32_t *) pathSpecBytes;
		const char *indexPath = pathSpecBytes + sizeof(uint32_t);
		pathSpecBytes += indexPathLength + sizeof(uint32_t) + 1;

		Size requiredSize = FillSinglePathSpec(indexPath, NULL);

		GenerateTermsContext context = { 0 };
		BsonGinSinglePathOptions *singlePathOptions = palloc(
			sizeof(BsonGinSinglePathOptions) + requiredSize + 1);
		singlePathOptions->base.type = IndexOptionsType_SinglePath;
		singlePathOptions->base.version = IndexOptionsVersion_V0;

		/* The truncation limit will be divided by the numPaths */
		context.termMetadata = GetSinglePathTermCreateMetadata(options,
															   (int32_t) pathCount);
		singlePathOptions->base.indexTermTruncateLimit =
			context.termMetadata.indexTermSizeLimit;
		singlePathOptions->isWildcard = false;
		singlePathOptions->generateNotFoundTerm = true;
		singlePathOptions->path = sizeof(BsonGinSinglePathOptions);

		FillSinglePathSpec(indexPath, ((char *) singlePathOptions) +
						   sizeof(BsonGinSinglePathOptions));

		context.options = (void *) singlePathOptions;
		context.traverseOptionsFunc = &GetSinglePathIndexTraverseOption;
		context.generatePathBasedUndefinedTerms = true;
		context.skipGeneratedPathUndefinedTermOnLiteralNull = true;
		context.termMetadata = GetIndexTermMetadata(singlePathOptions);
		context.skipGenerateTopLevelArrayTerm = true;

		bool addRootTerm = false;
		GenerateTerms(bson, &context, addRootTerm);

		entries[i] = context.terms.entries;
		entryCounts[i] = context.totalTermCount;

		/* We will have at least 1 term */
		totalTermCount = totalTermCount * context.totalTermCount;
		pfree(singlePathOptions);
	}

	/* Now that we have the per term counts, generate the overall terms */
	/* Add an additional one in case we need a truncated term */
	Datum *indexEntries = palloc0(sizeof(Datum) * (totalTermCount + 3));

	bool hasTruncation = false;
	IndexTermCreateMetadata overallMetadata = GetCompositeIndexTermMetadata(options);

	bytea *compositeDatums[INDEX_MAX_KEYS] = { 0 };
	for (uint32_t i = 0; i < totalTermCount; i++)
	{
		int termIndex = i;
		for (uint32_t j = 0; j < pathCount; j++)
		{
			int32_t currentIndex = termIndex % entryCounts[j];
			termIndex = termIndex / entryCounts[j];
			Datum term = entries[j][currentIndex];

			BsonIndexTerm indexTerm;
			InitializeBsonIndexTerm(DatumGetByteaPP(term), &indexTerm);

			if (indexTerm.isIndexTermTruncated)
			{
				hasTruncation = true;
			}

			compositeDatums[j] = DatumGetByteaPP(term);
		}

		BsonCompressableIndexTermSerialized serializedTerm =
			SerializeCompositeBsonIndexTermWithCompression(compositeDatums, pathCount);
		if (serializedTerm.isIndexTermTruncated)
		{
			hasTruncation = true;
		}

		indexEntries[i] = serializedTerm.indexTermDatum;
	}

	if (totalTermCount > 1)
	{
		/*
		 * TODO: This term is only needed in the case of parallel build
		 * See if we can eliminate this.
		 */
		RumHasMultiKeyPaths = true;
		indexEntries[totalTermCount] = GenerateRootMultiKeyTerm(&overallMetadata);
		totalTermCount++;
	}

	if (hasTruncation)
	{
		indexEntries[totalTermCount] = GenerateRootTruncatedTerm(&overallMetadata);
		totalTermCount++;
	}

	*nentries = totalTermCount;
	return indexEntries;
}


static int32_t
GetIndexPathsFromOptions(BsonGinCompositePathOptions *options,
						 const char **indexPaths)
{
	uint32_t pathCount;
	const char *pathSpecBytes;
	Get_Index_Path_Option(options, compositePathSpec, pathSpecBytes, pathCount);

	for (uint32_t i = 0; i < pathCount; i++)
	{
		uint32_t indexPathLength = *(uint32_t *) pathSpecBytes;
		const char *indexPath = pathSpecBytes + sizeof(uint32_t);
		pathSpecBytes += indexPathLength + sizeof(uint32_t) + 1;

		indexPaths[i] = indexPath;
	}

	return (int32_t) pathCount;
}


static void
ParseBoundsForCompositeOperator(pgbsonelement *singleElement, const char **indexPaths,
								int32_t numPaths, VariableIndexBounds *variableBounds)
{
	if (singleElement->bsonValue.value_type != BSON_TYPE_ARRAY)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
							"extract query for composite expecting a single array value: not %s",
							BsonTypeName(singleElement->bsonValue.value_type))));
	}

	bson_iter_t arrayIter;
	BsonValueInitIterator(&singleElement->bsonValue, &arrayIter);
	while (bson_iter_next(&arrayIter))
	{
		const bson_value_t *value = bson_iter_value(&arrayIter);
		if (value->value_type != BSON_TYPE_DOCUMENT)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"extract query composite expecting a single document value: %s",
								BsonValueToJsonForLogging(&singleElement->bsonValue))));
		}

		bson_iter_t queryOpIter;

		BsonValueInitIterator(value, &queryOpIter);
		BsonIndexStrategy queryStrategy = BSON_INDEX_STRATEGY_INVALID;
		pgbsonelement queryElement = { 0 };
		while (bson_iter_next(&queryOpIter))
		{
			const char *key = bson_iter_key(&queryOpIter);
			if (strcmp(key, "op") == 0)
			{
				queryStrategy = (BsonIndexStrategy) bson_iter_int32(&queryOpIter);
			}
			else
			{
				queryElement.path = key;
				queryElement.pathLength = strlen(key);
				queryElement.bsonValue = *bson_iter_value(&queryOpIter);
			}
		}

		if (queryStrategy == BSON_INDEX_STRATEGY_INVALID ||
			queryElement.pathLength == 0 ||
			queryElement.bsonValue.value_type == BSON_TYPE_EOD)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_INTERNALERROR), errmsg(
								"extract query composite expecting a valid operator and value: op=%d, value=%s",
								queryStrategy, BsonValueToJsonForLogging(value))));
		}

		ParseOperatorStrategy(indexPaths, numPaths, &queryElement, queryStrategy,
							  variableBounds);
	}
}


static bytea *
BuildTermForBounds(CompositeQueryRunData *runData,
				   IndexTermCreateMetadata *singlePathMetadata,
				   IndexTermCreateMetadata *compositeMetadata,
				   bool *partialMatch)
{
	/* For the next phase, process each term and handle truncation */
	bool hasTruncation = UpdateBoundsForTruncation(
		runData->indexBounds, runData->numIndexPaths,
		singlePathMetadata);
	runData->metaInfo->hasTruncation = runData->metaInfo->hasTruncation ||
									   hasTruncation;

	bool hasInequalityMatch = false;
	bytea *lowerBoundTerm = BuildLowerBoundTermFromIndexBounds(runData,
															   compositeMetadata,
															   &hasInequalityMatch);
	*partialMatch = hasInequalityMatch;
	return lowerBoundTerm;
}
