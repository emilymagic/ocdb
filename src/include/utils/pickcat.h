#ifndef PICKCAT_H
#define PICKCAT_H

#include "access/heapam.h"
#include "nodes/execnodes.h"
#include "utils/dispatchcat.h"


extern void PickSomeExtraData(HeapTuple heapTuple);
extern void PickRelation(Relation relation);
extern void PickRelationOid(Oid relationId);
extern void PickModifyRelation(ResultRelInfo *info);
extern void PickType(Oid typeId);
extern void PickFunctionCall(FmgrInfo *fmgrInfo, short	nFuncArgs, NullableDatum *funcArgs);
extern void PickCommon(void);
extern void PickCommon2(CdbCatalogNode *catalog);
extern void PickSortColumn(Oid orderingOp);
extern void PickCurrentRole(void);
extern void PickBaseCatalog(void);
extern Node *GetBaseCatalog(void);
extern void FillBaseCatalog(CdbCatalogNode *catalogNode);
#endif // PICKCAT_H
