#include "postgres.h"

#include "fmgr.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/htup_details.h"
#include "access/tileam.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_type.h"
#include "cdb/cdbsreh.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/objectfilerw.h"
#include "utils/builtins.h"
#include "utils/dispatchcat.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/pickcat.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include <unistd.h>


static CdbCatalogNode *initCatalog;
static MemoryContext initCatalogContext = NULL;

static void PickE(Oid id);
static void PickR(Oid id);
static void PickA(Oid id);
static void PickC(Oid id);
static void PickD(Oid id);
static void PickBase(Oid typeId);

static void TouchExpr(Expr *expr);
static void PickPartition(Relation relation);

static void
TouchSysCache(int cacheId, Datum key1)
{
	HeapTuple tuple;

	tuple = SearchSysCache1(cacheId, key1);
	if (tuple)
		ReleaseSysCache(tuple);
}

void
PickFunctionCall(FmgrInfo *fmgrInfo, short nFuncArgs, NullableDatum *funcArgs)
{
	HeapTuple		system_tuple;
	Form_pg_proc	pg_proc;
	MemoryContext	oldCtx;

	if (!DataDispatcherActive())
		return;

	oldCtx = MemoryContextSwitchTo(dataDispatchCtx);
	system_tuple = SearchSysCache1(PROCOID, fmgrInfo->fn_oid);
	pg_proc = (Form_pg_proc) GETSTRUCT(system_tuple);
	TouchSysCache(LANGOID, pg_proc->prolang);

	// Test if the function is a trigger function
	if (pg_proc->prolang > FirstGenbkiObjectId /* pl ? */)
		dataDispatcher->hasPlOrTigger = true;
	// Pick the catalog of sql function
	else if (pg_proc->prolang == SQLlanguageId)
	{
		init_sql_fcache(fmgrInfo, InvalidOid, true);
		fmgrInfo->fn_extra = NULL;
	}
	// Pick the catalog of next value function
	else if (strcmp(pg_proc->proname.data, "nextval") == 0 && !funcArgs[0].isnull)
		PickRelationOid(funcArgs[0].value);

	ReleaseSysCache(system_tuple);
	MemoryContextSwitchTo(oldCtx);
}

static void
TouchSysCache4(int cacheId, Datum key1, Datum key2, Datum key3, Datum key4)
{
	HeapTuple tuple;

	tuple = SearchSysCache4(cacheId, key1, key2, key3, key4);
	if (tuple)
		ReleaseSysCache(tuple);
}

static void
PickAmProc(Oid id)
{
	FmgrInfo finfo;
	HeapTuple heapTuple;

	heapTuple = SearchSysCache1(PROCOID, id);
	if (((Form_pg_proc) GETSTRUCT(heapTuple))->prolang == SQLlanguageId)
	{
		fmgr_info(id, &finfo);
		init_sql_fcache(&finfo, InvalidOid, true);
	}
	ReleaseSysCache(heapTuple);
}

static void
PickNamespaceOid(Oid id)
{
	TouchSysCache(NAMESPACEOID, id);
}

static void
PickAttr(Form_pg_attribute pg_attribute)
{
	PickType(pg_attribute->atttypid);

	if (OidIsValid(pg_attribute->attcollation))
		TouchSysCache(COLLOID,pg_attribute->attcollation);
}

static void
PickInOut(Oid typeId)
{
	HeapTuple heapTuple;

	heapTuple = SearchSysCache1(TYPEOID, typeId);
	if (!heapTuple)
		return;

	TouchSysCache(PROCOID, ((Form_pg_type) GETSTRUCT(heapTuple))->typinput);
	TouchSysCache(PROCOID, ((Form_pg_type) GETSTRUCT(heapTuple))->typoutput);

	ReleaseSysCache(heapTuple);
}


void
PickModifyRelation(ResultRelInfo *info)
{
	if (!DataDispatcherActive())
		return;

	if (info->ri_PartitionCheck && info->ri_RootResultRelInfo == NULL)
	{
		Oid			father;

		TouchExpr((Expr *) info->ri_PartitionCheck);

		// Get the partition parent of the table
		father = get_partition_parent_noerror(RelationGetRelid(info->ri_RelationDesc));
		if (OidIsValid(father))
		{
			Relation parentRelation;
			PartitionKey key;

			parentRelation = table_open(father, AccessShareLock);

			key = RelationRetrievePartitionKey(parentRelation);
			for (int i = 0; i < key->partnatts; ++i)
				PickFunctionCall(&key->partsupfunc[i], 0, NULL);

			table_close(parentRelation, AccessShareLock);
		}
	}

	PickRelation(info->ri_RelationDesc);
	table_scan_prepare_dispatch(info->ri_RelationDesc, NULL);
}

void
PickType(Oid typeId)
{
	Form_pg_type pg_type;
	HeapTuple heapTuple;

	if (!DataDispatcherActive())
		return;

	heapTuple = SearchSysCache1(TYPEOID, typeId);
	if (!heapTuple)
		return;

	pg_type = (Form_pg_type) GETSTRUCT(heapTuple);


	if(pg_type->typtype == TYPTYPE_DOMAIN)
		PickD(typeId);
	else if (pg_type->typtype == TYPTYPE_RANGE)
		PickR(typeId);
	else if (pg_type->typtype == TYPTYPE_ENUM)
		PickE(typeId);
	else if (pg_type->typtype == TYPTYPE_COMPOSITE)
		PickC(typeId);
	else if (type_is_array(typeId))
		PickA(typeId);
	else
		PickBase(typeId);

	PickInOut(typeId);

	ReleaseSysCache(heapTuple);
}

static void
PickD(Oid id)
{
	TypeCacheEntry *type_cache_entry;
	int flags = 0;

	flags |= TYPECACHE_RANGE_INFO ;
	flags |= TYPECACHE_DOMAIN_BASE_INFO;
	type_cache_entry = lookup_type_cache(id, flags);
	PickType(type_cache_entry->domainBaseType);
}

static void
PickC(Oid id)
{
	TypeCacheEntry *type_cache_entry;
	int flags = 0;

	flags |= TYPECACHE_TUPDESC;
	type_cache_entry = lookup_type_cache(id, flags);

	for (int i = 0; i < type_cache_entry->tupDesc->natts; ++i)
		PickType(TupleDescAttr(type_cache_entry->tupDesc, i)->atttypid);
}

static void
PickA(Oid id)
{
	TypeCacheEntry *type_cache_entry;
	int flags = 0;

	flags |= TYPECACHE_RANGE_INFO;
	flags |= TYPECACHE_DOMAIN_BASE_INFO;

	type_cache_entry = lookup_type_cache(id, flags);
	PickType(type_cache_entry->typelem);
}

static void
PickR(Oid id)
{
	int flags = 0;

	flags |= TYPECACHE_RANGE_INFO;
	flags |= TYPECACHE_DOMAIN_BASE_INFO;
	lookup_type_cache(id, flags);
}

static void
PickE(Oid id)
{
	TypeCacheEntry *type_cache_entry = lookup_type_cache(id, TYPECACHE_EQ_OPR);

	load_enum_cache_data(type_cache_entry);
}

static void
PickBase(Oid typeId)
{
	int flags = 0;

	flags |= TYPECACHE_CMP_PROC_FINFO;
	flags |= TYPECACHE_EQ_OPR_FINFO;

	lookup_type_cache(typeId, flags);
}

void
PickCommon(void)
{
	TouchSysCache(NAMESPACENAME, CStringGetDatum("public"));
	TouchSysCache(NAMESPACENAME, CStringGetDatum("pg_catalog"));

	/*
	 * Pick unknown type
	 */
	TouchSysCache(TYPEOID, 705);
}

void
PickCommon2(CdbCatalogNode *catalog)
{
	catalog->full_xid = dataDispatcher->fullXid;
	catalog->seq = dataDispatcher->seq;
	GetTempNamespaceState(&catalog->namespace_1, &catalog->namespace_2);
	catalog->CatalogServerId = CatalogServerId;
}

static void
TouchExpr(Expr *expr)
{
	if (!expr)
		return;

	expr = expression_planner(expr);

	if (IsA(expr, List))
		expr = make_ands_explicit((List *) expr);

	ExecInitExpr(expr, NULL);
}

static void
TouchConstraint(Relation relation)
{
	int i;

	if (!relation->rd_att->constr)
		return;

	for (i = 0; i < relation->rd_att->constr->num_check; i++)
		TouchExpr(stringToNode(relation->rd_att->constr->check[i].ccbin));

	if (relation->rd_att->constr->has_generated_stored)
	{
		TupleDesc	tupdesc = RelationGetDescr(relation);
		int natts = tupdesc->natts;

		for (i = 0; i < natts; i++)
			TouchExpr((Expr *) build_column_default(relation, i + 1));
	}
}

static void
PickPartition(Relation relation)
{
	int idxPart;

	// Pick all the patition columns
	pg_get_partkeydef_columns(RelationGetRelid(relation), true);
	for (idxPart = 0; idxPart < RelationGetPartitionDesc(relation)->nparts; idxPart++)
	{
		Oid subPart;
		Relation childRel;

		subPart = RelationGetPartitionDesc(relation)->oids[idxPart];
		childRel = relation_open(subPart, AccessShareLock);
		PickRelation(childRel);
		relation_close(childRel, AccessShareLock);
	}
}

void
PickSortColumn(Oid orderingOp)
{
	Oid			operatorFamily = 0;
	Oid			leftType = 0;

	CatCList   *catlist;
	int			i;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(orderingOp));
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* must be btree */
		if (aform->amopmethod != BTREE_AM_OID)
			continue;

		if (aform->amopstrategy == BTLessStrategyNumber ||
			aform->amopstrategy == BTGreaterStrategyNumber)
		{
			/* Found it ... should have consistent input types */
			if (aform->amoplefttype == aform->amoprighttype)
			{
				/* Found a suitable opfamily, return info */
				operatorFamily = aform->amopfamily;
				leftType = aform->amoplefttype;
				break;
			}
		}
	}
	ReleaseSysCacheList(catlist);

	TouchSysCache4(AMPROCNUM, operatorFamily, leftType, leftType, BTSORTSUPPORT_PROC);
	TouchSysCache4(AMPROCNUM, operatorFamily, leftType, leftType, BTORDER_PROC);
}

void
PickRelation(Relation relation)
{
	if (!(relation && DataDispatcherActive()))
		return;

	if (relation->trigdesc)
		dataDispatcher->hasPlOrTigger = true;

	// Pick the constraint
	TouchConstraint(relation);
	TouchExpr((Expr *) RelationGetPartitionQual(relation));

	if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		PickPartition(relation);

	// Pick the relation row type
	PickType(relation->rd_rel->reltype);
	TouchSysCache(SEQRELID, RelationGetRelid(relation));

	// If this is a tile table, call it initialization function
	if (RelationIsTile(relation))
		tile_access_initialization(relation);
}

void
PickRelationOid(Oid relationId)
{
	Relation	relation;

	if (!(OidIsValid(relationId) && DataDispatcherActive()))
		return;

	relation = relation_open(relationId, AccessShareLock);

	PickRelation(relation);

	relation_close(relation, AccessShareLock);
}

static void
PickGpSegInfo(void)
{
	int			total_dbs = 0;
	GpSegConfigEntry *entrys = NULL;
	entrys = readGpSegConfigFromCatalog(&total_dbs);
}

void
PickCurrentRole(void)
{
	TouchSysCache(AUTHOID, GetUserId());
}

static void
PickCurrentDatabase(void)
{
	TouchSysCache(DATABASEOID, MyDatabaseId);
}


void
PickSomeExtraData(HeapTuple heapTuple)
{
	if (heapTuple->t_data == NULL)
		return;

	switch (heapTuple->t_tableOid)
	{
		case RelationRelationId:
			PickNamespaceOid(((Form_pg_class) GETSTRUCT(heapTuple))->relnamespace);
		break;
		case AccessMethodProcedureRelationId:
			PickAmProc(((Form_pg_amproc) GETSTRUCT(heapTuple))->amproc);
		break;
		case TypeRelationId:
			PickType(((Form_pg_type) GETSTRUCT(heapTuple))->oid);
		break;
		case AttributeRelationId:
			PickAttr((Form_pg_attribute) GETSTRUCT(heapTuple));
		break;
		case PartitionedRelationId:
		{
			Form_pg_partitioned_table pgPartition;
			HeapTuple tuple;

			pgPartition = (Form_pg_partitioned_table) GETSTRUCT(heapTuple);
			tuple = SearchSysCache1(PARTRELID, pgPartition->partrelid);
			memcpy(heapTuple->t_data, tuple->t_data, Min(heapTuple->t_len, tuple->t_len));
			ReleaseSysCache(tuple);
			break;
		}
		default:
			break;
	}
}


void
PickBaseCatalog(void)
{
	MemoryContext oldCtx;
	CdbCatalogNode *catalogNode;

	initCatalogContext = AllocSetContextCreate(TopMemoryContext,
											   "initCatalogContext",
											   ALLOCSET_DEFAULT_SIZES);

	if (!DataDispatcherActive())
		return;

	PickGpSegInfo();
	PickCurrentRole();
	PickCurrentDatabase();

	oldCtx = MemoryContextSwitchTo(initCatalogContext);

	catalogNode = makeNode(CdbCatalogNode);
	{
		HASH_SEQ_STATUS status;
		CatalogTableHtValue	*ht_value;

		hash_seq_init(&status, dataDispatcher->tableDataHt);
		while ((ht_value = hash_seq_search(&status)))
		{
			CatalogTableNode *tableNode = makeNode(CatalogTableNode);

			tableNode->relId = ht_value->relId;
			tableNode->tupleDataSize = ht_value->stringInfo.len;
			tableNode->tupleData = palloc(tableNode->tupleDataSize);
			memcpy(tableNode->tupleData, ht_value->stringInfo.data,
				   tableNode->tupleDataSize);

			catalogNode->tableList = lappend(catalogNode->tableList, tableNode);
		}
	}

	catalogNode->s3Url = s3_url;

	MemoryContextSwitchTo(oldCtx);

	initCatalog = catalogNode;
}

void
FillBaseCatalog(CdbCatalogNode *catalogNode)
{
	initCatalog = catalogNode;
}

Node *
GetBaseCatalog(void)
{
	return (Node*) initCatalog;
}
