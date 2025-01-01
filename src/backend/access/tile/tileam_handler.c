#include "postgres.h"

#include "access/heapam.h"
#include "access/tileam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "cdb/cdbvars.h"
#include "storage/objectfilerw.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/pg_tile.h"
#include "utils/dispatchcat.h"
#include "commands/vacuum.h"

void *s3Client = NULL;

static TileDmlDesc getDmlDesc(Relation relation);
static TileFetchDesc get_fetch_descriptor(Relation relation);

void tile_access_initialization(Relation relation)
{
	if (CatCollectorActive())
	{
		FullTransactionId fullXid = GetCurrentFullTransactionId();
		catCollector->fullXid = fullXid.value;
		catCollector->seq = TransactionGetSequence(false);
	}
}

void s3_init(void)
{
	s3Client = S3InitAccess();
}

void s3_destroy(void)
{
	if (s3Client)
		S3DestroyAccess(s3Client);
}


static const TupleTableSlotOps *
tile_slot_callbacks(Relation relation)
{
	return &TTSOpsMinimalTuple;
}

static Size
tile_parallelscan_estimate(Relation rel)
{
	elog(ERROR, "tile_parallelscan_estimate has not been supported");
}

static Size
tile_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	elog(ERROR, "tile_parallelscan_initialize has not been supported");
}

static void
tile_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	elog(ERROR, "tile_parallelscan_reinitialize has not been supported");
}

static IndexFetchTableData *
tile_index_fetch_begin(Relation rel)
{
	elog(ERROR, "tile_index_fetch has not been supported");
}

static void
tile_index_fetch_reset(IndexFetchTableData *scan)
{
	elog(ERROR, "tile_index_fetch has not been supported");
}

static void
tile_index_fetch_end(IndexFetchTableData *scan)
{
	elog(ERROR, "tile_index_fetch has not been supported");
}

static bool
tile_index_fetch_tuple(struct IndexFetchTableData *scan,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	return false;
}

static void
tileam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	TileDmlDesc dmlDesc;
	MinimalTuple tup;
	bool		free = true;

	dmlDesc = getDmlDesc(relation);
	tup = ExecFetchSlotMinimalTuple(slot, &free);
	tile_insert(dmlDesc, tup);

	ItemPointerSet(&slot->tts_tid, 0, dmlDesc->newBufferTupNum);
	slot->tts_tid.xid = dmlDesc->newBuffer->key.tid;
	slot->tts_tid.seq = dmlDesc->newBuffer->key.seq;

	if (free)
		pfree(tup);
}

static TileDmlDesc
getDmlDesc(Relation relation)
{
	if (!relation->tileDmlDesc)
	{
		relation->tileDmlDesc = MemoryContextAllocZero(CacheMemoryContext,
													   sizeof(TileDmlDescData));
		relation->tileDmlDesc->mainRel = relation;
		if (myClusterId != 0)
			relation->tileDmlDesc->visibilityRel = NULL;
		else {
			Oid visibilityRelid = PgTileGetVisiRelId(RelationGetRelid(relation));
			relation->tileDmlDesc->visibilityRel = table_open(visibilityRelid,
															  RowExclusiveLock);
		}

		relation->tileDmlDesc->oldBuffer = tile_init_buf();
		relation->tileDmlDesc->oldBufferCurPtrDiff = 0;
		relation->tileDmlDesc->oldBufferCurPtrTupNum = 0; //invalid

		relation->tileDmlDesc->newBuffer = tile_init_buf_with_tid();
		relation->tileDmlDesc->newBufferTupNum = 0; // invalid, no data
		relation->tileDmlDesc->visibilityInfo = NIL;
	}

	return relation->tileDmlDesc;
}


static void
tileam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								  CommandId cid, int options,
								  BulkInsertState bistate, uint32 specToken)
{
	elog(ERROR, "tileam_tuple_insert_speculative has not been supported");
}

static void
tileam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
									uint32 specToken, bool succeeded)
{
	elog(ERROR, "tileam_tuple_complete_speculative has not been supported");
}

static void
tileam_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
					  CommandId cid, int options, BulkInsertState bistate)
{
	TileDmlDesc	desc;
	MinimalTuple	mtuple;
	bool			shouldFree = false;
	int				i;


	desc = getDmlDesc(relation);

	for (i = 0; i < ntuples; i++)
	{
		mtuple = ExecFetchSlotMinimalTuple(slots[i], &shouldFree);
		tile_insert(desc, mtuple);
		ItemPointerSet(&slots[i]->tts_tid, 0, desc->newBufferTupNum);
		slots[i]->tts_tid.xid = desc->newBuffer->key.tid;
		slots[i]->tts_tid.seq = desc->newBuffer->key.seq;
	}

	if (shouldFree)
		pfree(mtuple);
}

static TM_Result
tileam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
	return tile_delete(getDmlDesc(relation), tid);
}

static TM_Result
tileam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, bool *update_indexes)
{
	bool free;
	MinimalTuple tup = ExecFetchSlotMinimalTuple(slot, &free);
	TileDmlDesc dmlDesc = getDmlDesc(relation);

	tile_update(dmlDesc, otid, tup);
	ItemPointerSet(&slot->tts_tid, 0, dmlDesc->newBufferTupNum);
	slot->tts_tid.xid = dmlDesc->newBuffer->key.tid;
	slot->tts_tid.seq = dmlDesc->newBuffer->key.seq;

	if (free)
		pfree(tup);

	return TM_Ok;
}

static TileFetchDesc
get_fetch_descriptor(Relation relation)
{
	if (relation->tileFetchDesc == NULL)
	{
		relation->tileFetchDesc = MemoryContextAllocZero(CacheMemoryContext,
													sizeof(TileFetchDescData));
		relation->tileFetchDesc->mainRel = relation;
		if (myClusterId == 0) {
			Oid blockListRelid = PgTileGetVisiRelId(RelationGetRelid(relation));
			relation->tileFetchDesc->visibilityRel = table_open(blockListRelid,
																AccessShareLock);
		}
		relation->tileFetchDesc->buffer = NULL;
		relation->tileFetchDesc->bufferCurPtrDiff = 0;
		relation->tileFetchDesc->bufferCurPtrTupNum = 0;
	}

	return relation->tileFetchDesc;
}

static TM_Result
tileam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	TileFetchDesc desc;

	tmfd->traversed = false;

	desc = get_fetch_descriptor(relation);

	tile_fetch(relation, desc, tid, snapshot, slot);

	return TM_Ok;
}

static void
tileam_finish_bulk_insert(Relation relation, int options)
{

}

static bool
tileam_fetch_row_version(Relation relation, ItemPointer tid, Snapshot snapshot,
						 TupleTableSlot *slot)
{
	TileFetchDesc desc;

	desc = get_fetch_descriptor(relation);

	return tile_fetch(relation, desc, tid, snapshot, slot);
}


static void
tileam_relation_set_new_filenode(Relation rel,
								 const RelFileNode *newrnode,
								 char persistence,
								 TransactionId *freezeXid,
								 MultiXactId *minmulti)

{

}

static uint64
tileam_relation_size(Relation rel, ForkNumber forkNumber)
{
	BlockNumber relpages;
	double tuples;

	/*
	 * FIXME_CLOUD: use real data to get relation size
	 */

	/* it has storage, ok to call the smgr */
	relpages = rel->rd_rel->relpages;

	if (relpages == 0)
	{
		relpages = 10;
		tuples = relpages * 400;
	}
	else
	{
		tuples = rel->rd_rel->reltuples < 400 ? 400 : rel->rd_rel->reltuples;
	}

	return (tuples * 100);
}

static bool
tileam_relation_needs_toast_table(Relation rel)
{
	return false;
}

static void
tileam_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	BlockNumber relpages;

	/* it has storage, ok to call the smgr */
	relpages = rel->rd_rel->relpages;

	if (relpages == 0)
	{
		relpages = 10;
		*tuples = relpages * 400;
	}
	else
	{
		*tuples = rel->rd_rel->reltuples < 400 ? 400 : rel->rd_rel->reltuples;
	}

	*pages = relpages;
	*allvisfrac = 1;
}

static void
tileam_vacuum(Relation onerel, struct VacuumParams *params,
              BufferAccessStrategy bstrategy)
{
	LOCKMODE	lmode = (params->options & VACOPT_FULL) ?
	                    AccessExclusiveLock : ShareUpdateExclusiveLock;
	Oid blockListRelid = PgTileGetVisiRelId(RelationGetRelid(onerel));
	Relation blockListRel = table_open(blockListRelid, lmode);
	heap_vacuum_rel(blockListRel, params, bstrategy);
	relation_close(blockListRel, lmode);
}

static void
tileam_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
	// we cannot use link to find the latest tid
}


static bool
tileam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerIsValid(tid);
}

static void
tile_nontransactional_truncate(Relation rel)
{
	Oid blockListRelid;
	Relation blockListRel;

	blockListRelid = PgTileGetVisiRelId(rel->rd_id);
	blockListRel = table_open(blockListRelid, ExclusiveLock);

	heap_truncate_one_rel(blockListRel);
	table_close(blockListRel, ExclusiveLock);
	tile_clear_table(rel->rd_node);
}


static const TableAmRoutine tile_methods = {
		.type = T_TableAmRoutine,

		.slot_callbacks = tile_slot_callbacks,

		.dml_init = tile_access_initialization,
		.dml_finish = tile_access_release,

		.scan_prepare_dispatch = tile_scan_prepare_dispatch,
		.scan_begin = tile_beginscan,
		.scan_end = tile_endscan,
		.scan_rescan = tile_rescan,
		.scan_getnextslot = tile_getnextslot,
		.scan_analyze_next_block = tile_scan_analyze_next_block,
		.scan_analyze_next_tuple = tile_scan_analyze_next_tuple,

		.parallelscan_estimate = tile_parallelscan_estimate,
		.parallelscan_initialize = tile_parallelscan_initialize,
		.parallelscan_reinitialize = tile_parallelscan_reinitialize,


		.index_fetch_begin = tile_index_fetch_begin,
		.index_fetch_reset = tile_index_fetch_reset,
		.index_fetch_end = tile_index_fetch_end,
		.index_fetch_tuple = tile_index_fetch_tuple,

		.tuple_insert = tileam_tuple_insert,
		.tuple_insert_speculative = tileam_tuple_insert_speculative,
		.tuple_complete_speculative = tileam_tuple_complete_speculative,
		.multi_insert = tileam_multi_insert,
		.tuple_delete = tileam_tuple_delete,
		.tuple_update = tileam_tuple_update,
		.tuple_lock = tileam_tuple_lock,
		.finish_bulk_insert = tileam_finish_bulk_insert,

		.tuple_fetch_row_version = tileam_fetch_row_version,
		.tuple_get_latest_tid = tileam_get_latest_tid,
		.tuple_tid_valid = tileam_tuple_tid_valid,


		.relation_set_new_filenode = tileam_relation_set_new_filenode,
		.relation_nontransactional_truncate = tile_nontransactional_truncate,

		.relation_size = tileam_relation_size,
		.relation_needs_toast_table = tileam_relation_needs_toast_table,

		.relation_estimate_size = tileam_estimate_rel_size,

		.relation_vacuum = tileam_vacuum
};

const TableAmRoutine *
GetTileamTableAmRoutine(void)
{
	return &tile_methods;
}

Datum
tile_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&tile_methods);
}