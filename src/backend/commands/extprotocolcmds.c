/*-------------------------------------------------------------------------
 *
 * extprotocolcmds.c
 *
 *	  Routines for external protocol-manipulation commands
 *
 * Portions Copyright (c) 2011, Greenplum/EMC.
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	    src/backend/commands/extprotocolcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/oid_dispatch.h"
#include "catalog/pg_extprotocol.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extprotocolcmds.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbdisp_query.h"

/*
 * Drop PROTOCOL by OID. This is the guts of deletion.
 * This is called to clean up dependencies.
 */
void
RemoveExtProtocolById(Oid protOid)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	/*
	 * Search pg_extprotocol.
	 */
	rel = table_open(ExtprotocolRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_extprotocol_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(protOid));
	scan = systable_beginscan(rel, ExtprotocolOidIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(rel, &tup->t_self);
		found = true;
	}
	systable_endscan(scan);

	if (!found)
		elog(ERROR, "protocol %u could not be found", protOid);

	table_close(rel, NoLock);
}
