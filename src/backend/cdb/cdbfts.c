/*-------------------------------------------------------------------------
 *
 * cdbfts.c
 *	  Provides fault tolerance service routines for mpp.
 *
 * Portions Copyright (c) 2003-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/cdb/cdbfts.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "utils/memutils.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbdisp_query.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "libpq/libpq-be.h"
#include "commands/dbcommands.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"

#include "executor/spi.h"

#include "postmaster/fts.h"

#include "utils/fmgroids.h"
#include "catalog/pg_authid.h"

/* segment id for the master */
#define COORDINATOR_SEGMENT_ID -1

volatile FtsProbeInfo *ftsProbeInfo = NULL;	/* Probe process updates this structure */

extern volatile pid_t *shmFtsProbePID;

/*
 * get fts share memory size
 */
int
FtsShmemSize(void)
{
	return MAXALIGN(sizeof(FtsControlBlock));
}

void
FtsShmemInit(void)
{
	bool		found;
	FtsControlBlock *shared;

	shared = (FtsControlBlock *) ShmemInitStruct("Fault Tolerance manager", sizeof(FtsControlBlock), &found);
	if (!shared)
		elog(FATAL, "FTS: could not initialize fault tolerance manager share memory");

	/* Initialize shared memory area */
	ftsProbeInfo = &shared->fts_probe_info;
	shmFtsProbePID = &shared->fts_probe_pid;
	*shmFtsProbePID = 0;

	if (!IsUnderPostmaster)
		shared->fts_probe_info.status_version = 0;
}

/* see src/backend/fts/README */
void
FtsNotifyProber(void)
{
	Assert(Gp_role == GP_ROLE_DISPATCH);
	int32			initial_started;
	int32			started;
	int32			done;

	/* Ignore if we don't have a FTS probe process, like a standby QD in a mirrored cluster. */
	if (FtsProbePID() == 0)
		return;

	if (am_ftsprobe)
		return;

	SpinLockAcquire(&ftsProbeInfo->lock);
	initial_started = ftsProbeInfo->start_count;
	SpinLockRelease(&ftsProbeInfo->lock);

	SendPostmasterSignal(PMSIGNAL_WAKEN_FTS);

	SIMPLE_FAULT_INJECTOR("ftsNotify_before");

	/* Wait for a new fts probe to start. */
	for (;;)
	{
		SpinLockAcquire(&ftsProbeInfo->lock);
		started = ftsProbeInfo->start_count;
		SpinLockRelease(&ftsProbeInfo->lock);

		if (started != initial_started)
			break;

		CHECK_FOR_INTERRUPTS();
		pg_usleep(50000);
	}

	/* Wait until current probe in progress is completed */
	for (;;)
	{
		SpinLockAcquire(&ftsProbeInfo->lock);
		done = ftsProbeInfo->done_count;
		SpinLockRelease(&ftsProbeInfo->lock);

		if (done - started >= 0)
			break;

		CHECK_FOR_INTERRUPTS();
		pg_usleep(50000);
	}

}

/*
 * Test-Connection: This is called from the threaded context inside the
 * dispatcher: ONLY CALL THREADSAFE FUNCTIONS -- elog() is NOT threadsafe.
 */
bool
FtsIsSegmentDown(CdbComponentDatabaseInfo *dBInfo)
{
	return false;

	/* coordinator is always reported as alive */
	if (dBInfo->config->segindex == COORDINATOR_SEGMENT_ID)
		return false;

	return FTS_STATUS_IS_DOWN(ftsProbeInfo->status[dBInfo->config->dbid]);
}

/*
 * Check if any segment DB is down.
 *
 * returns true if any segment DB is down.
 */
bool
FtsTestSegmentDBIsDown(SegmentDatabaseDescriptor **segdbDesc, int size)
{
	int			i = 0;

	for (i = 0; i < size; i++)
	{
		CdbComponentDatabaseInfo *segInfo = segdbDesc[i]->segment_database_info;

		elog(DEBUG2, "FtsTestSegmentDBIsDown: looking for real fault on segment dbid %d", (int) segInfo->config->dbid);

		if (FtsIsSegmentDown(segInfo))
		{
			ereport(LOG, (errmsg_internal("FTS: found fault with segment dbid %d. "
										  "Reconfiguration is in progress", (int) segInfo->config->dbid)));
			return true;
		}
	}

	return false;
}

uint8
getFtsVersion(void)
{

	return 0;

	return ftsProbeInfo->status_version;
}
