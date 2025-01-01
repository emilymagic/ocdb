#include "postgres.h"

#include "access/relscan.h"
#include "access/skey.h"
#include "access/memoryheapam.h"
#include "access/valid.h"
#include "catalog/namespace.h"
#include "utils/dispatchcat.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbvars.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"
#include "utils/rel.h"
static MemoryHeapData memoryHeapData = {0};
static MemoryHeapData *memHeapData = &memoryHeapData;
MemoryContext memoryHeapContext = NULL;

char *initCatalogBuf = NULL;
int initCatalogBufSize = 0;

static void MemoryHeapDataSetInternal(CdbCatalogNode *catalogNode, AuxNode *auxNode);

static void
MemoryHeapStorageInit(void)
{
	HASHCTL hashctl;
	MemoryContext oldCtx;
	oldCtx = MemoryContextSwitchTo(memoryHeapContext);

	MemSet(&hashctl, 0, sizeof(hashctl));
	hashctl.keysize = sizeof(Oid);
	hashctl.entrysize = sizeof(CdbTableHtValue);
	hashctl.hcxt = memoryHeapContext;
	memHeapData->memTableHash = hash_create("memory heap table", 128, &hashctl,
											HASH_CONTEXT | HASH_ELEM | HASH_BLOBS);

	MemoryContextSwitchTo(oldCtx);
}

void
MemoryHeapStorageReset(void)
{
	if (!memoryHeapContext)
		memoryHeapContext = AllocSetContextCreate(TopMemoryContext,
												  "memory heap context",
												  ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(memoryHeapContext);

	cdbCatAuxNode = NULL;

	memHeapData->memTableHash = NULL;
	memHeapData->fullxid = 0;
	MemoryHeapStorageInit();
}

void
MemoryHeapDataSet1(CdbCatalogAuxNode *catAux)
{
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(memoryHeapContext);

	MemoryHeapDataSetInternal(catAux->catalog, catAux->aux);
	if (!IS_CATALOG_SERVER() && IS_QUERY_DISPATCHER())
		cdbCatAuxNode = catAux;

	MemoryContextSwitchTo(oldCtx);
}

void
MemoryHeapDataSet2(const char *catBuffer, int catBufferSize,
				   const char *auxBuffer, int auxBufferSize)
{
	MemoryContext	oldCtx;
	CdbCatalogNode *cat = NULL;
	AuxNode		   *aux = NULL;

	oldCtx = MemoryContextSwitchTo(memoryHeapContext);

	if (catBuffer)
		cat = (CdbCatalogNode *) deserializeNode(catBuffer, catBufferSize);
	if (auxBuffer)
		aux = (AuxNode *) deserializeNode(auxBuffer, auxBufferSize);

	MemoryHeapDataSetInternal(cat, aux);
	if (!IS_CATALOG_SERVER() && IS_QUERY_DISPATCHER())
	{
		cdbCatAuxNode = (CdbCatalogAuxNode *) makeNode(CdbCatalogNode);
		cdbCatAuxNode->catalog = cat;
		cdbCatAuxNode->aux = aux;
	}

	MemoryContextSwitchTo(oldCtx);
}

static void
MemoryHeapSetTableData(CatalogTableNode *tableNode)
{
	CdbTableHtValue *relHtValue;
	bool		exist;

	relHtValue = hash_search(memHeapData->memTableHash, &tableNode->relId,
							 HASH_ENTER, &exist);
	if (exist)
		elog(ERROR, "Table %u already exist in memory table.", tableNode->relId);

	relHtValue->tupleDataSize = tableNode->tupleDataSize;
	relHtValue->tupleData = tableNode->tupleData;
}

static void
MemoryHeapDataSetInternal(CdbCatalogNode *catalogNode, AuxNode *auxNode)
{
	MemoryContext oldCtx;

	if (auxNode == NULL && catalogNode == NULL)
		return;

	oldCtx = MemoryContextSwitchTo(memoryHeapContext);

	if (catalogNode)
	{
		ListCell   *lc;

		/* Fill transaction */
		memHeapData->fullxid = catalogNode->full_xid;
		SetTempNamespaceState(catalogNode->namespace_1,
							  catalogNode->namespace_2);

		/* Fill data to memory table */
		foreach(lc, catalogNode->tableList)
			MemoryHeapSetTableData(lfirst(lc));
	}

	if (auxNode)
	{
		ListCell   *lc;

		/* Fill data to memory table */
		foreach(lc, auxNode->tableList)
			MemoryHeapSetTableData(lfirst(lc));
	}

	MemoryContextSwitchTo(oldCtx);
}

char *
memoryTableGetData(Oid relid, int *size)
{
	CdbTableHtValue	*tableHtValue;
	bool		exist;

	tableHtValue = hash_search(memHeapData->memTableHash, &relid, HASH_FIND, &exist);

	if (!exist)
	{
		*size = 0;
		return NULL;
	}

	*size = tableHtValue->tupleDataSize;
	return tableHtValue->tupleData;
}


TableScanDesc
MemoryHeapBeginScan(Relation relation, int nkeys, ScanKey key)
{
	MemoryHeapDesc scan;

	scan = (MemoryHeapDesc) palloc0(sizeof(MemoryHeapDescData));
	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_nkeys = nkeys;

	if (key != NULL && nkeys > 0)
	{
		scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));
	}

	scan->tupleData = memoryTableGetData(RelationGetRelid(scan->rs_base.rs_rd),
										 &scan->tupleDataSize);
	scan->curIndex = 0;

	return (TableScanDesc) scan;
}

void
MemoryHeapEndScan(TableScanDesc sscan)
{
	pfree(sscan);
}

bool
MemoryHeapGetNextSlot(TableScanDesc sscan, TupleTableSlot *slot)
{
	MemoryHeapDesc	scan = (MemoryHeapDesc) sscan;
	HeapTuple		tuple;
	bool			testResult = true;

	while ((tuple = TupleDataGetNext(scan->tupleData, &scan->curIndex,
					scan->tupleDataSize)) != NULL)
	{


		if (scan->rs_base.rs_key != NULL)
			HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
						scan->rs_base.rs_nkeys, scan->rs_base.rs_key, testResult);

		if (testResult)
			break;
	}

	if (tuple)
	{
		ExecStoreHeapTuple(tuple, slot, false);
		return true;
	}

	ExecClearTuple(slot);
	return false;
}

bool
MemoryHeapActive(void)
{
	return memHeapData->memTableHash != NULL;
}

uint64
MemoryHeapGetFullXid(void)
{
	return memHeapData->fullxid;
}
