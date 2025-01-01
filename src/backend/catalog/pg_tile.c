#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tile.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

void
CreateTileVisiTable(Relation main_rel)
{
	TupleDesc	tupdesc;

	char visiRelName[NAMEDATALEN];
	Oid visiRelId = InvalidOid;
	ObjectAddress srcObj;
	ObjectAddress dstObj;
	Oid			relNameSpace;

	tupdesc = CreateTemplateTupleDesc(3);

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "filesize", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "filepath", NAMEOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "tupnum", INT4OID, -1, 0);

	snprintf(visiRelName, sizeof(visiRelName),
			 "%s_%u", "visi", RelationGetRelid(main_rel));

	if (RelationUsesLocalBuffers(main_rel))
		relNameSpace = GetTempToastNamespace();
	else
		relNameSpace = PG_AOSEGMENT_NAMESPACE;

	visiRelId = heap_create_with_catalog(visiRelName,
										 relNameSpace,
										 main_rel->rd_rel->reltablespace,
										 InvalidOid,
										 InvalidOid,
										 InvalidOid,
										 main_rel->rd_rel->relowner,
										 HEAP_TABLE_AM_OID,
										 tupdesc,
										 NIL,
										 RELKIND_RELATION,
										 RELPERSISTENCE_PERMANENT,
										 false,
										 false,
										 ONCOMMIT_NOOP,
										 (Datum) 0,
										 /* use_user_acl */ false,
										 true,
										 true,
										 InvalidOid,
										 NULL);

	CommandCounterIncrement();

	PgTileInsert(RelationGetRelid(main_rel), visiRelId);

	srcObj.classId = RelationRelationId;
	srcObj.objectId = RelationGetRelid(main_rel);
	srcObj.objectSubId = 0;
	dstObj.classId = RelationRelationId;
	dstObj.objectId = visiRelId;
	dstObj.objectSubId = 0;

	recordDependencyOn(&dstObj, &srcObj, DEPENDENCY_INTERNAL);

	CommandCounterIncrement();
}

Oid
PgTileGetVisiRelId(Oid mainRelId)
{
	Relation	pgTile;
	ScanKeyData scanKeys[1];
	SysScanDesc sysScan;
	HeapTuple	tup;
	bool is_null;
	Oid visiRelId;

	pgTile = table_open(TileRelationId, AccessShareLock);

	ScanKeyInit(&scanKeys[0],
				Anum_pg_tile_mainrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(mainRelId));

	sysScan = systable_beginscan(pgTile, TileMainrelidIndexId, true, NULL, 1, scanKeys);
	tup = systable_getnext(sysScan);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("PgTile tuple missed for relation \"%s\"", get_rel_name(mainRelId))));


	visiRelId = DatumGetObjectId(heap_getattr(tup,Anum_pg_tile_visirelid,
											  RelationGetDescr(pgTile), &is_null));
	Assert(!is_null);

	systable_endscan(sysScan);
	table_close(pgTile, AccessShareLock);

	return visiRelId;
}

void
PgTileInsert(Oid mainRelid, Oid visiRelid)
{
	Relation tileRel;
	HeapTuple	tup = NULL;
	Datum	   values[Natts_pg_tile];
	bool	   nulls[Natts_pg_tile];

	tileRel = table_open(TileRelationId, RowExclusiveLock);

	values[Anum_pg_tile_mainrelid - 1] = ObjectIdGetDatum(mainRelid);
	values[Anum_pg_tile_visirelid - 1] = ObjectIdGetDatum(visiRelid);
	MemSet(nulls, false, sizeof(nulls));

	tup = heap_form_tuple(RelationGetDescr(tileRel), values, nulls);
	CatalogTupleInsert(tileRel, tup);
	table_close(tileRel, NoLock);
}
