#ifndef MEMORYHEAPAM_H
#define MEMORYHEAPAM_H

#include "executor/tuptable.h"

typedef struct MemoryHeapDescData
{
	TableScanDescData rs_base;

	int			tupleDataSize;
	char	   *tupleData;
	int			curIndex;
} MemoryHeapDescData;

typedef MemoryHeapDescData *MemoryHeapDesc;
typedef struct CdbCatalogAuxNode CdbCatalogAuxNode;
typedef struct CdbCatalogNode CdbCatalogNode;
typedef struct AuxNode AuxNode;

extern void MemoryHeapStorageReset(void);
extern void MemoryHeapDataSetCat(CdbCatalogNode *catalog);
extern void MemoryHeapDataSetAux(AuxNode *aux);
extern void MemoryHeapDataSet1(CdbCatalogAuxNode *catAux);
extern void MemoryHeapDataSet2(const char *catBuffer, int catBufferSize,
						 const char *auxBuffer, int auxBufferSize);
extern char *memoryTableGetData(Oid relid, int *size);

extern TableScanDesc MemoryHeapBeginScan(Relation relation, int nkeys, ScanKey key);
extern void MemoryHeapEndScan(TableScanDesc sscan);
extern bool MemoryHeapGetNextSlot(TableScanDesc sscan, TupleTableSlot *slot);
extern bool MemoryHeapActive(void);

extern char *initCatalogBuf;
extern int initCatalogBufSize;

extern uint64 MemoryHeapGetFullXid(void);

#endif //MEMORYHEAPAM_H
