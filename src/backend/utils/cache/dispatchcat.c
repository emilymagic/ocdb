#include "postgres.h"

#include "access/tileam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbsrlz.h"
#include "utils/pickcat.h"
#include "utils/dispatchcat.h"
#include "utils/inval.h"
#include "utils/memutils.h"

MemoryContext	dataDispatchCtx = NULL;
DataDispatcher   *dataDispatcher = NULL;
static CacheGetter		cacheGetterData;
static CacheGetter	   *cacheGetter = NULL;
CdbCatalogAuxNode   *cdbCatAuxNode = NULL;
bool			isInTrigger = false;

static void  TestAndCreateCtx(void);

void
DataDispatcherInit(void)
{
	MemoryContext oldCtx;

	if (!IS_CATALOG_SERVER() || IsBootstrapProcessingMode())
		return;

	if (dataDispatcher)
		return;

	oldCtx = MemoryContextSwitchTo(dataDispatchCtx);

	dataDispatcher = palloc0(sizeof(DataDispatcher));

	/*
	 * Create table data hash
	 */
	{
		HASHCTL tableDataHc;

		MemSet(&tableDataHc, 0, sizeof(tableDataHc));
		tableDataHc.hcxt = dataDispatchCtx;
		tableDataHc.keysize = sizeof(Oid);
		tableDataHc.entrysize = sizeof(CatalogTableHtValue);
		dataDispatcher->tableDataHt = hash_create("table data", 128, &tableDataHc,
												HASH_CONTEXT | HASH_ELEM | HASH_BLOBS);
	}

	/*
	 * Create catalog tuple hash key to check duplicate
	 */
	{
		HASHCTL tupleKeyHc;

		MemSet(&tupleKeyHc, 0, sizeof(tupleKeyHc));
		tupleKeyHc.hcxt = dataDispatchCtx;
		tupleKeyHc.keysize = sizeof(MemoryHeapKey);
		tupleKeyHc.entrysize = sizeof(MemoryHeapKey);
		dataDispatcher->memoryHeapKeyHt = hash_create("tuple key", 128, &tupleKeyHc,
											   HASH_CONTEXT | HASH_ELEM | HASH_BLOBS);
	}

	dataDispatcher->seq = FirstItemPointerSeq;
	dataDispatcher->hasPlOrTigger = false;

	MemoryContextSwitchTo(oldCtx);
}

void
BeginCacheGet(CacheNode *cache)
{
	if (!IS_CATALOG_SERVER() || IsBootstrapProcessingMode())
		return;

	cacheGetter->cur++;
	cacheGetter->tuplesList[cacheGetter->cur] = cache;
}

void
FreeCacheTuples(CacheNode *cacheNode)
{
	ListCell *lc;

	foreach(lc, cacheNode->tuples)
	{
		HeapTuple tuple;

		tuple = lfirst(lc);

		pfree(tuple->t_data);
		pfree(tuple);
	}

	list_free(cacheNode->tuples);
}

void
EndCacheGet(void)
{
	if (!IS_CATALOG_SERVER() || IsBootstrapProcessingMode())
		return;

	cacheGetter->cur--;
}

static void
CdbGetCache(HeapTuple heapTuple)
{
	MemoryContext	oldCtx;
	HeapTuple		tuple;

	oldCtx = MemoryContextSwitchTo(CacheMemoryContext);

	tuple = palloc(sizeof(HeapTupleData));
	tuple->t_len = heapTuple->t_len;;
	tuple->t_self = heapTuple->t_self;
	tuple->t_tableOid = heapTuple->t_tableOid;
	tuple->t_data = palloc(heapTuple->t_len);
	memcpy(tuple->t_data, heapTuple->t_data, heapTuple->t_len);

	for (int i = 0; i <= cacheGetter->cur; ++i)
		cacheGetter->tuplesList[i]->tuples =
				lappend(cacheGetter->tuplesList[i]->tuples, tuple);

	MemoryContextSwitchTo(oldCtx);
}

static void
AddTupleToStringInfo(StringInfo curInfo, HeapTuple heapTuple)
{
	appendBinaryStringInfo(curInfo, (char *) heapTuple, sizeof(HeapTupleData));
	appendBinaryStringInfo(curInfo, (char *) heapTuple->t_data, heapTuple->t_len);
}

HeapTuple
TupleDataGetNext(char *tuple_data, int *curIndex, int len)
{
	HeapTuple	tuple = NULL;

	if (*curIndex < len)
	{
		tuple = (HeapTuple) (tuple_data + *curIndex);
		tuple->t_data = (HeapTupleHeader) (((char *) tuple) + sizeof(HeapTupleData));
		*curIndex += (sizeof(HeapTupleData) + tuple->t_len);
		Assert(*curIndex <= len);
	}

	return tuple;
}

static void
CdbGetTupleInternal(HeapTuple heapTuple)
{
	MemoryContext	oldCtx;
	MemoryHeapKey	memory_heap_key;
	CatalogTableHtValue	   *ht_value;
	bool			active;

	oldCtx = MemoryContextSwitchTo(dataDispatchCtx);

	memory_heap_key.relId = heapTuple->t_tableOid;
	memory_heap_key.ip_blkid = heapTuple->t_self.ip_blkid;
	memory_heap_key.ip_posid = heapTuple->t_self.ip_posid;
	hash_search(dataDispatcher->memoryHeapKeyHt, &memory_heap_key, HASH_ENTER, &active);
	if (active)
	{
		MemoryContextSwitchTo(oldCtx);
		return;
	}

	ht_value = hash_search(dataDispatcher->tableDataHt, &memory_heap_key.relId, HASH_ENTER,
						   &active);
	if (!active)
		initStringInfo(&ht_value->stringInfo);

	AddTupleToStringInfo(&ht_value->stringInfo, heapTuple);

	MemoryContextSwitchTo(oldCtx);
}

HeapTuple
CdbGetTuple(HeapTuple heapTuple)
{
	if (!heapTuple)
		return heapTuple;

	if (CacheGetterActive())
		CdbGetCache(heapTuple);
	if (DataDispatcherActive())
		CdbGetTupleInternal(heapTuple);

	return heapTuple;
}

void
CdbCopyCacheTuples(CacheNode *cacheNode)
{
	MemoryContext oldCtx;

	if (!cacheNode)
		return;

	if (CacheGetterActive())
	{
		oldCtx = MemoryContextSwitchTo(CacheMemoryContext);
		for (int i = 0; i <= cacheGetter->cur; ++i)
		{
			CacheNode *curNode;
			ListCell *lc;

			curNode = cacheGetter->tuplesList[i];
			foreach(lc, cacheNode->tuples)
			{
				HeapTuple tuple;

				tuple = lfirst(lc);
				curNode->tuples = lappend(curNode->tuples, tuple);
			}
		}
		MemoryContextSwitchTo(oldCtx);
	}

	if (DataDispatcherActive())
	{
		ListCell	 *lc;

		foreach(lc, cacheNode->tuples)
		{
			HeapTuple cacheTuple;

			cacheTuple = lfirst(lc);
			CdbGetTupleInternal(cacheTuple);
		}
	}

}


bool
DataDispatcherActive(void)
{
	return (dataDispatcher != NULL);
}

bool
CacheGetterActive(void)
{
	return (cacheGetter && cacheGetter->cur >= 0);
}

void
DataDispatcherClear(void)
{
	if (!IS_CATALOG_SERVER() || IsBootstrapProcessingMode())
		return;

	TestAndCreateCtx();

	dataDispatcher = NULL;
	cacheGetter = &cacheGetterData;
	cacheGetter->cur = -1;
	isInTrigger = false;
}

static CdbCatalogNode *
GetCatalogNodeFromDispatcher(void)
{
	CdbCatalogNode *catalog = makeNode(CdbCatalogNode);

	if (!DataDispatcherActive())
		return catalog;

	PickCommon();
	PickCommon2(catalog);

	{
		HASH_SEQ_STATUS hash_seq;
		CatalogTableHtValue	*ht_value;

		hash_seq_init(&hash_seq, dataDispatcher->tableDataHt);
		while ((ht_value = hash_seq_search(&hash_seq)))
		{
			HeapTuple	tuple;
			int			curIndex;
			int			len;

			if (ht_value->relId >= FirstNormalObjectId)
				continue;

			curIndex = 0;
			len = ht_value->stringInfo.len;
			while ((tuple = TupleDataGetNext(ht_value->stringInfo.data, &curIndex,len)))
				PickSomeExtraData(tuple);
		}
	}

	{
		HASH_SEQ_STATUS status;
		CatalogTableHtValue	*ht_value;

		hash_seq_init(&status, dataDispatcher->tableDataHt);
		while ((ht_value = hash_seq_search(&status)))
		{
			CatalogTableNode *tableNode;

			if (ht_value->relId >= FirstNormalObjectId)
				continue;

			tableNode= makeNode(CatalogTableNode);
			tableNode->relId = ht_value->relId;
			tableNode->tupleData = ht_value->stringInfo.data;
			tableNode->tupleDataSize = ht_value->stringInfo.len;

			catalog->tableList = lappend(catalog->tableList, tableNode);
		}
	}

	return catalog;
}


CdbCatalogNode *
GetCatalogNode(void)
{
	if (cdbCatAuxNode)
		return cdbCatAuxNode->catalog;
	else
		return GetCatalogNodeFromDispatcher();
}

AuxNode *
GetAuxNode(void)
{
	AuxNode *node;

	if (cdbCatAuxNode)
		return cdbCatAuxNode->aux;

	node = makeNode(AuxNode);
	if (DataDispatcherActive())
	{
		HASH_SEQ_STATUS status;
		CatalogTableHtValue	*ht_value;

		hash_seq_init(&status, dataDispatcher->tableDataHt);
		while ((ht_value = hash_seq_search(&status)))
		{
			CatalogTableNode *tableNode;

			if (ht_value->relId < FirstNormalObjectId)
				continue;

			tableNode= makeNode(CatalogTableNode);
			tableNode->relId = ht_value->relId;
			tableNode->tupleData = ht_value->stringInfo.data;
			tableNode->tupleDataSize = ht_value->stringInfo.len;

			node->tableList = lappend(node->tableList, tableNode);
		}
	}

	return node;
}

AuxNode **
GetAuxNodeArray(int gangSize)
{
	AuxNode	   *auxNode;
	AuxNode	  **auxNodeArray;
	int			i;
	ListCell   *lc;

	auxNodeArray = palloc(sizeof(AuxNode *) * gangSize);

	for (i = 0; i < gangSize; ++i)
		auxNodeArray[i] = makeNode(AuxNode);

	auxNode = GetAuxNode();

	if (!auxNode)
		return auxNodeArray;

	foreach(lc, auxNode->tableList)
	{
		CatalogTableNode   *tableNode;
		HeapTuple			tuple;
		int					curIndex;
		StringInfoData	   *stringInfoArray;
		uint32				segNo;

		stringInfoArray = palloc(sizeof(StringInfoData) * gangSize);
		for (i = 0; i < gangSize; ++i)
			initStringInfo(&stringInfoArray[i]);

		tableNode = lfirst(lc);

		segNo = 0;
		curIndex = 0;
		while ((tuple = TupleDataGetNext(tableNode->tupleData, &curIndex,
				tableNode->tupleDataSize)) != NULL)
		{
			AddTupleToStringInfo(&stringInfoArray[segNo++%gangSize], tuple);
		}

		for (i = 0; i < gangSize; ++i)
		{
			CatalogTableNode *tableNodeSeg;

			tableNodeSeg = makeNode(CatalogTableNode);
			tableNodeSeg->relId = tableNode->relId;
			tableNodeSeg->tupleData = stringInfoArray[i].data;
			tableNodeSeg->tupleDataSize = stringInfoArray[i].len;

			auxNodeArray[i]->tableList = lappend(auxNodeArray[i]->tableList,
												 tableNodeSeg);
		}
	}

	return auxNodeArray;
}

static void
TestAndCreateCtx(void)
{
	if (dataDispatchCtx)
		MemoryContextReset(dataDispatchCtx);
	else
		dataDispatchCtx = AllocSetContextCreate(TopMemoryContext, "Data Dispatcher",
											  ALLOCSET_DEFAULT_SIZES);
}

