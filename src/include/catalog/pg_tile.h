#ifndef PG_TILE_H
#define PG_TILE_H

#include "catalog/genbki.h"
#include "catalog/pg_tile_d.h"
#include "utils/relcache.h"

CATALOG(pg_tile,6017,TileRelationId)
{
	Oid	mainrelid;
	Oid	visirelid;
} FormData_pg_tile;

typedef FormData_pg_tile *Form_pg_tile;

extern void PgTileInsert(Oid mainRelid, Oid visiRelid);
extern void CreateTileVisiTable(Relation main_rel);
extern Oid PgTileGetVisiRelId(Oid mainRelId);
#endif // PG_TILE_H
