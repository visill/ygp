/*
* execProjection.c
*/

#include "postgres.h"

#include "nodes/execnodes.h"
#include "access/tableam.h"
#include "executor/executor.h"

#include "access/table.h"
#include "utils/relcache.h"

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void
ExecOpenProjections(ResultRelInfo *resultRelInfo)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *prjoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	PrjInfo **prjInfoArray;


	resultRelInfo->ri_NumProjection = 0;

	// /* fast path if no indexes */
	// if (!RelationGetForm(resultRelation))
	// 	return;

	/*
	 * Get cached list of index OIDs
	 */
	prjoidlist = RelationGetPrjList(resultRelation);
	len = list_length(prjoidlist);
	if (len == 0)
		return;

	/*
	 * allocate space for result arrays
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));

	resultRelInfo->ri_NumProjection = len;
	resultRelInfo->ri_PrjRelationDescs = relationDescs;


	prjInfoArray = (PrjInfo **) palloc(len * sizeof(PrjInfo *));

	resultRelInfo->ri_ProjectionRelationInfo = prjInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info. We
	 * acquire RowExclusiveLock, signifying we will update the index.
	 *
	 * Note: we do this even if the index is not indisready; it's not worth
	 * the trouble to optimize for the case where it isn't.
	 */
	i = 0;
	foreach(l, prjoidlist)
	{
		Oid			prjOid = lfirst_oid(l);
		Relation	prjDesc;
		PrjInfo  *pji;

		prjDesc = table_open(prjOid, RowExclusiveLock);
		/* extract index key information from the index's pg_index info */
		pji = BuildPrjInfo(prjDesc);

		prjInfoArray[i] = pji;

		relationDescs[i] = prjDesc;
		i++;
	}

	list_free(prjoidlist);
}



/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseProjection(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr prjDescs;

	numIndices = resultRelInfo->ri_NumProjection;
	prjDescs = resultRelInfo->ri_PrjRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (prjDescs[i] == NULL)
			continue;			/* shouldn't happen? */

		/* Drop lock acquired by ExecOpenIndices */
		table_close(prjDescs[i], RowExclusiveLock);
	}
}


/* ----------------
 *		FormIndexDatum
 *			Construct values[] and isnull[] arrays for a new index tuple.
 *
 *	indexInfo		Info about the index
 *	slot			Heap tuple for which we must prepare an index entry
 *	estate			executor state for evaluating any index expressions
 *	values			Array of index Datums (output area)
 *	isnull			Array of is-null indicators (output area)
 *
 * When there are no index expressions, estate may be NULL.  Otherwise it
 * must be supplied, *and* the ecxt_scantuple slot of its per-tuple expr
 * context must point to the heap tuple passed in.
 *
 * Notice we don't actually call index_form_tuple() here; we just prepare
 * its input arrays values[] and isnull[].  This is because the index AM
 * may wish to alter the data before storage.
 * ----------------
 */
void
FormProjectionDatum(struct PrjInfo *prjInfo,
			   TupleTableSlot *slot,
			   struct EState *estate,
			   Datum *values,
			   bool *isnull)
{
	int			i;

	for (i = 0; i < prjInfo->pji_NumPrjAttrs; i++)
	{
		int			keycol = prjInfo->pji_PrjAttrNumbers[i];
		Datum		iDatum;
		bool		isNull;

		if (keycol < 0)
			iDatum = slot_getsysattr(slot, keycol, &isNull);
		else if (keycol != 0)
		{
			/*
			 * Plain index column; get the value we need directly from the
			 * heap tuple.
			 */
			iDatum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/*
			 * Projection expression --- need to evaluate it.
			 */
		}
		values[i] = iDatum;
		isnull[i] = isNull;
	}
}


List *
ExecInsertProjectionTuples(TupleTableSlot *slot, EState *estate) 
{
	List	   *result = NIL;
	ResultRelInfo *resultRelInfo;
	int			i;
	int			numProjections;
	RelationPtr relationDescs;
	Relation	heapRelation;
	ExprContext *econtext;
	TupleTableSlot *tts;

	// ExecCopySlot(tts, slot);

	/*
	 * Get information from the result relation info structure.
	 */
	resultRelInfo = estate->es_result_relation_info;
	numProjections = resultRelInfo->ri_NumProjection;
	relationDescs = resultRelInfo->ri_PrjRelationDescs;
	heapRelation = resultRelInfo->ri_RelationDesc;

	// es_result_relation_info->

	/* Sanity check: slot must belong to the same rel as the resultRelInfo. */
	Assert(slot->tts_tableOid == RelationGetRelid(heapRelation));

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numProjections; i++)
	{

		HeapTuple tuple;
        Datum		values[INDEX_MAX_KEYS];
        bool		isnull[INDEX_MAX_KEYS];
        
		Relation	prjRelation = relationDescs[i];
		TupleDesc tupDesc;
		TupleTableSlot* prjslot;
		PrjInfo  *pjInfo;
		bool		applyNoDupErr;
		bool		satisfiesConstraint;

		if (prjRelation == NULL)
			continue;

		tupDesc = RelationGetDescr(prjRelation);
		prjslot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsVirtual);
        
		/*
		 * FormProjectionDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the projection.
		 */
		FormProjectionDatum(pjInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		tuple = heap_form_tuple(tupDesc, values, isnull);

		ExecStoreHeapTuple(tuple, prjslot, true /* do pfree tuple */);

        // !! reduce tuple, does it satify local prj?

		(void)simple_table_tuple_insert_check_location(prjRelation, prjslot);
	}

	return result;
}