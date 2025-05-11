#ifndef DISPATCHCAT_H
#define DISPATCHCAT_H

#include "access/htup.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "tcop/dest.h"
#include "utils/hsearch.h"

typedef struct CatalogTableNode
{
	NodeTag type;

	Oid		relId;
	int		tupleDataSize;
	char   *tupleData;
} CatalogTableNode;

typedef struct CatalogTableHtValue
{
	Oid				relId;
	StringInfoData	stringInfo;
} CatalogTableHtValue;

typedef struct DataDispatcher
{
	HTAB   *tableDataHt;
	HTAB   *memoryHeapKeyHt;
	uint64	fullXid;
	uint64	seq;
	bool	hasPlOrTigger;
} DataDispatcher;

#define MAX_CACHE_LEVEL 5

typedef struct CacheNode
{
	List *tuples;
} CacheNode;

typedef struct CacheGetter
{
	int			cur;
	CacheNode  *tuplesList[MAX_CACHE_LEVEL];
} CacheGetter;

typedef struct BlockListNode
{
	NodeTag	type;
	Oid		relid;
	Oid		relfilenode;
	List   *blockListInfo;
} BlockListNode;

typedef struct AuxNode
{
	NodeTag	type;
	List   *tableList;
} AuxNode;

typedef struct MemoryHeapKey
{
	Oid				relId;
	BlockIdData		ip_blkid;
	OffsetNumber	ip_posid;
}
/* If compiler understands packed and aligned pragmas, use those */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
	pg_attribute_packed()
	pg_attribute_aligned(2)
#endif
MemoryHeapKey;

typedef struct CdbTableHtValue
{
	Oid		relId;
	int		tupleDataSize;
	char   *tupleData;
} CdbTableHtValue;

typedef struct CdbCatalogNode
{
	NodeTag	type;

	uint64	full_xid;
	uint64	seq;
	Oid 	namespace_1;
	Oid 	namespace_2;
	char   *CatalogServerId;
	List   *tableList;
	char   *s3Url;
} CdbCatalogNode;

typedef struct CdbCatalogAuxNode
{
	NodeTag			type;
	char			completionTag[COMPLETION_TAG_BUFSIZE];
	CdbCatalogNode *catalog;
	AuxNode		   *aux;
	PlannedStmt	   *plan;
} CdbCatalogAuxNode;

extern DataDispatcher *dataDispatcher;

typedef struct MemoryHeapData
{
	HTAB   *memTableHash;
	uint64	fullxid;
} MemoryHeapData;

extern MemoryContext	dataDispatchCtx;
extern bool				isInTrigger;
extern MemoryContext	memoryHeapContext;
extern CdbCatalogAuxNode   *cdbCatAuxNode;

extern void DataDispatcherInit(void);
extern void BeginCacheGet(CacheNode *cache);
extern void EndCacheGet(void);
extern void CdbCopyCacheTuples(CacheNode *cacheNode);
extern bool DataDispatcherActive(void);
extern bool CacheGetterActive(void);
extern void DataDispatcherClear(void);
extern HeapTuple CdbGetTuple(HeapTuple heapTuple);
extern AuxNode *GetAuxNode(void);
extern CdbCatalogNode *GetCatalogNode(void);
extern AuxNode **GetAuxNodeArray(int gangSize);

extern void FreeCacheTuples(CacheNode *cacheNode);
extern HeapTuple TupleDataGetNext(char *tuple_data, int *curIndex, int len);

#endif // DISPATCHCAT_H
