#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tileam.h"
#include "access/tile_visi.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_tile.h"
#include "utils/dispatchcat.h"
#include "cdb/cdbcatalogfunc.h"
#include "cdb/cdbvars.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "storage/predicate.h"
#include "storage/objectfilerw.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

typedef struct TidBlockkey {
    uint32 blockid;
    TileKey key;
} TidBlockkey;

static void set_page(TileDmlDesc dmlDesc);

static void tile_init_scan(TileScanDesc scan);

static List *tile_get_visi(Relation visiRel, Snapshot snapshot);

static bool tile_get_page(TileScanDesc desc, BLOCKMOVE page_move);

static TileKey tid_get_blockkey(ItemPointer tid);
static void MoveAfterToNewPage(TileDmlDesc desc);
static void FinishTransForCurrentBlock(TileDmlDesc desc);

void
release_tile_dml_state(Relation rel) {
    if (rel->tileDmlDesc) {
        tile_release_buf(rel->tileDmlDesc->oldBuffer);
        tile_release_buf(rel->tileDmlDesc->newBuffer);
        pfree(rel->tileDmlDesc);
        rel->tileDmlDesc = NULL;
    }

    if (rel->tileFetchDesc) {
        if (rel->tileFetchDesc->bufferShouldFree)
            tile_release_buf(rel->tileFetchDesc->buffer);
        pfree(rel->tileFetchDesc);
        rel->tileFetchDesc = NULL;
    }
}

TileBuf *
tile_init_buf(void) {
    TileBuf *buf;

    buf = MemoryContextAllocZero(CacheMemoryContext, sizeof(TileBuf));

    buf->bufStartPtr = MemoryContextAlloc(CacheMemoryContext, TILE_BLOCK_SIZE);
    buf->bufSize = 0;
    return buf;
}

static TileKey
tile_new_blockkey(void) {
    FullTransactionId fullxid = GetCurrentFullTransactionId();
    uint64 tseq;
    TileKey key;

    tseq = GpIdentity.segindex + 1;
    tseq = tseq << 48;
    tseq += TransactionGetSequence(true);

    key.tid = fullxid.value;
    key.seq = tseq;

    return key;
}

TileBuf *
tile_init_buf_with_tid(void) {
    TileBuf *buf;

    buf = MemoryContextAllocZero(CacheMemoryContext, sizeof(TileBuf));

    buf->blockid = 0;
    buf->bufStartPtr = MemoryContextAlloc(CacheMemoryContext, TILE_BLOCK_SIZE);
    buf->bufSize = 0;
    buf->key = tile_new_blockkey();

    return buf;
}

static void
tile_reset_buf(TileBuf *buf) {
    buf->blockid = 0;
    buf->bufSize = 0;
    buf->key = tile_new_blockkey();
}

void
tile_release_buf(TileBuf *tileBuffer) {
    if (tileBuffer == NULL)
        return;

    if (tileBuffer->bufStartPtr)
        pfree(tileBuffer->bufStartPtr);

    pfree(tileBuffer);
}


static void
tile_read_buf(Relation relation, ItemPointerData tid, TileKey key,
                TileBuf *buf) {
    char *block_name;
    char *bucket_name;

    // find the target old block
    block_name = GetBlockNameFromKey(key);
    bucket_name = TileMakeBucketPath(relation->rd_node);
    buf->blockid = tile_tid_get_blockid(tid);
    buf->key = key;
    buf->bufSize = S3GetObject2(s3Client, bucket_name, block_name, buf->bufStartPtr);
    Assert(buf->bufSize > 0);
    Assert(buf->bufSize <= TILE_BLOCK_SIZE);
    pfree(block_name);
    pfree(bucket_name);
}

void
tile_insert(TileDmlDesc dmlDesc, MinimalTuple minimalTuple) {
    if (dmlDesc->newBuffer->bufSize + minimalTuple->t_len > TILE_BLOCK_SIZE) {
        set_page(dmlDesc);
    }

    memcpy(dmlDesc->newBuffer->bufStartPtr + dmlDesc->newBuffer->bufSize,
        minimalTuple, minimalTuple->t_len);
    dmlDesc->newBuffer->bufSize += minimalTuple->t_len;
    dmlDesc->newBufferTupNum++;
}

static HeapTuple
make_visibility_tuple(Relation metaRel, uint32 pageSize, uint32 tupleNum, TileKey key) {
    TupleDesc meta_tuple_desc = RelationGetDescr(metaRel);
    bool *nulls = palloc(meta_tuple_desc->natts * sizeof(bool));
    Datum *values = palloc(meta_tuple_desc->natts * sizeof(Datum));
    HeapTuple meta_tuple;
    NameData page_name;
    char *page_name_str = GetBlockNameFromKey(key);

    namestrcpy(&page_name, page_name_str);
    pfree(page_name_str);

    MemSet(nulls, false, sizeof(bool) * meta_tuple_desc->natts);
    values[0] = UInt32GetDatum(pageSize);
    values[1] = NameGetDatum(&page_name);
    values[2] = UInt32GetDatum(tupleNum);

    meta_tuple = heap_form_tuple(meta_tuple_desc, values, nulls);

    return meta_tuple;
}

static void
set_page(TileDmlDesc dmlDesc) {
    TM_FailureData tmfd;
    LockTupleMode lockmode;
    S3ObjKey s3_obj_key;
    S3Obj s3_obj;

    if (myClusterId != 0) {
        BlockDesc2 *blockDesc2;
        blockDesc2 = makeNode(BlockDesc2);

        if (blockkey_is_valid(dmlDesc->oldBuffer->key)) {
            blockDesc2->blockid = dmlDesc->oldBuffer->blockid;
        }

        if (blockkey_is_valid(dmlDesc->newBuffer->key)) {
            blockDesc2->block_size = dmlDesc->newBuffer->bufSize;
            blockDesc2->block_tuple_num = dmlDesc->newBufferTupNum;
            blockDesc2->newKey = dmlDesc->newBuffer->key;
        }

        dmlDesc->visibilityInfo = lappend(dmlDesc->visibilityInfo, blockDesc2);
    } else {
        HeapTuple block_list_tuple;

        block_list_tuple = make_visibility_tuple(dmlDesc->visibilityRel,
                                                 dmlDesc->newBuffer->bufSize,
                                                 dmlDesc->newBufferTupNum,
                                                 dmlDesc->newBuffer->key);

        if (!blockkey_is_valid(dmlDesc->oldBuffer->key))
            heap_insert(dmlDesc->visibilityRel, block_list_tuple, GetCurrentCommandId(true),
                        0, NULL);
        else {
            ItemPointerData heapTid;

            heapTid = blockid_to_heaptid(dmlDesc->oldBuffer->blockid);
            heap_update(dmlDesc->visibilityRel, &heapTid, block_list_tuple,
                        GetCurrentCommandId(true), NULL, true, &tmfd, &lockmode);
        }

        heap_freetuple(block_list_tuple);
    }

    s3_obj_key.bucketName = TileMakeBucketPath(dmlDesc->mainRel->rd_node);
    s3_obj_key.objectName = GetBlockNameFromKey(dmlDesc->newBuffer->key);
    s3_obj.data = dmlDesc->newBuffer->bufStartPtr;
    s3_obj.size = dmlDesc->newBuffer->bufSize;
    S3PutObject(s3Client, s3_obj_key, s3_obj);
    pfree(s3_obj_key.bucketName);
    pfree(s3_obj_key.objectName);

    tile_reset_buf(dmlDesc->newBuffer);
    dmlDesc->newBufferTupNum = 0;
}

void
tile_insert_block_list_notify(char *message) {
    BlockListNode *blockListNode;

    if (!message)
        return;

    blockListNode = stringToNode(message);
    cc_send_modify_tabble(blockListNode);
}

void
tile_insert_block_list_cs(BlockListNode *blockListNode) {
    Oid blockListRelid;
    Relation blockListRel;
    ListCell *cell;
    TM_FailureData tmfd;
    LockTupleMode lockmode;

    blockListRelid = PgTileGetVisiRelId(blockListNode->relid);
    blockListRel = table_open(blockListRelid, RowExclusiveLock);

    foreach(cell, blockListNode->blockListInfo) {
        BlockDesc2 *blockDesc2;
        blockDesc2 = lfirst(cell);
        ItemPointerData oldTid;

        oldTid = blockid_to_heaptid(blockDesc2->blockid);

        if (ItemPointerIsValid(&oldTid) &&
            blockkey_is_valid(blockDesc2->newKey)) {
            HeapTuple block_list_tuple;

            block_list_tuple = make_visibility_tuple(blockListRel,
                                                     blockDesc2->block_size,
                                                     blockDesc2->block_tuple_num,
                                                     blockDesc2->newKey);
            heap_update(blockListRel, &oldTid, block_list_tuple,
                        GetCurrentCommandId(true), NULL, true, &tmfd, &lockmode);
            heap_freetuple(block_list_tuple);
        } else if (ItemPointerIsValid(&oldTid)) {
            heap_delete(blockListRel, &oldTid, GetCurrentCommandId(true),
                        NULL, true, &tmfd, false);
        } else if (blockkey_is_valid(blockDesc2->newKey)) {
            HeapTuple block_list_tuple;

            block_list_tuple = make_visibility_tuple(blockListRel,
                                                     blockDesc2->block_size,
                                                     blockDesc2->block_tuple_num,
                                                     blockDesc2->newKey);
            heap_insert(blockListRel, block_list_tuple, GetCurrentCommandId(true),
                        0, NULL);
            heap_freetuple(block_list_tuple);
        }
    }

    table_close(blockListRel, RowExclusiveLock);
}

TM_Result
tile_delete(TileDmlDesc dmlDesc, ItemPointer tid) {
    TileKey key;
    uint32 targetTupleSeq;
    MinimalTuple tup;

    key = tid_get_blockkey(tid);

    // deal with the oldBuf
    if (blockkey_is_valid(dmlDesc->oldBuffer->key)) {
        if (!blockkey_equal(key, dmlDesc->oldBuffer->key)) {
            // need finish the current old buf
            FinishTransForCurrentBlock(dmlDesc);
            tile_read_buf(dmlDesc->mainRel, *tid, key, dmlDesc->oldBuffer);
            dmlDesc->oldBufferCurPtrDiff = 0;
            dmlDesc->oldBufferCurPtrTupNum = 1;
        }
    } else {
        tile_read_buf(dmlDesc->mainRel, *tid, key, dmlDesc->oldBuffer);
        dmlDesc->oldBufferCurPtrDiff = 0;
        dmlDesc->oldBufferCurPtrTupNum = 1;
    }

    targetTupleSeq = tile_tid_get_seq(tid);

    if (targetTupleSeq == dmlDesc->oldBufferCurPtrTupNum - 1)
        return TM_Deleted;

    while (dmlDesc->oldBufferCurPtrTupNum < targetTupleSeq) {
        tup = (MinimalTuple) (dmlDesc->oldBuffer->bufStartPtr + dmlDesc->
                                 oldBufferCurPtrDiff);
        memcpy(dmlDesc->newBuffer->bufStartPtr + dmlDesc->newBuffer->bufSize, tup,
               tup->t_len);
        dmlDesc->newBuffer->bufSize += tup->t_len;
        dmlDesc->newBufferTupNum++;
        dmlDesc->oldBufferCurPtrDiff += tup->t_len;
        dmlDesc->oldBufferCurPtrTupNum++;
    }

    dmlDesc->oldBufferCurPtrDiff +=
        ((MinimalTuple) (dmlDesc->oldBuffer->bufStartPtr + dmlDesc->oldBufferCurPtrDiff))->t_len;
    dmlDesc->oldBufferCurPtrTupNum++;

    return TM_Ok;
}

static void
FinishTransForCurrentBlock(TileDmlDesc desc) {
    TM_FailureData tmfd;

    if (desc->oldBuffer && desc->oldBuffer->bufSize != 0)
        MoveAfterToNewPage(desc);

    if (desc->newBuffer->bufSize > 0) {
        // insert a new block for the remaining available data
        set_page(desc);
    } else if (desc->oldBuffer->bufSize > 0) {
        if (!IS_CATALOG_SERVER()) {
            BlockDesc2 *blockDesc2;
            blockDesc2 = makeNode(BlockDesc2);

            blockDesc2->blockid = desc->oldBuffer->blockid;
            desc->visibilityInfo = lappend(desc->visibilityInfo, blockDesc2);
        }
        // the whole block is not valid anymore, delete the aux tuple from the heap
        else {
            ItemPointerData heapTid;

            heapTid = blockid_to_heaptid(desc->oldBuffer->blockid);
            heap_delete(desc->visibilityRel, &heapTid, GetCurrentCommandId(true),
                        NULL, true, &tmfd, false);
        }
    }
}

static void
MoveAfterToNewPage(TileDmlDesc desc) {
    MinimalTuple mtuple;

    while (desc->oldBufferCurPtrDiff < desc->oldBuffer->bufSize) {
        mtuple = (MinimalTuple) (desc->oldBuffer->bufStartPtr + desc->
                                 oldBufferCurPtrDiff);
        memcpy(desc->newBuffer->bufStartPtr + desc->newBuffer->bufSize, mtuple,
               mtuple->t_len);
        desc->newBuffer->bufSize += mtuple->t_len;
        desc->newBufferTupNum++;
        desc->oldBufferCurPtrDiff += mtuple->t_len;
        desc->oldBufferCurPtrTupNum++;
    }
}

static TileKey
tid_get_blockkey(ItemPointer tid) {
    TileKey key;

    key.tid = tid->xid;
    key.seq = tid->seq;

    return key;
}

TM_Result
tile_update(TileDmlDesc dmlDesc, ItemPointer otid, MinimalTuple newTuple) {
    TM_Result result;

    result = tile_delete(dmlDesc, otid);
    if (result == TM_Deleted)
        return TM_Updated;

    memcpy(dmlDesc->newBuffer->bufStartPtr + dmlDesc->newBuffer->bufSize, newTuple,
        newTuple->t_len);
    dmlDesc->newBuffer->bufSize += newTuple->t_len;
    dmlDesc->newBufferTupNum++;

    return TM_Ok;
}

void
tile_update_finish(TileDmlDesc dmlDesc) {
    FinishTransForCurrentBlock(dmlDesc);

    if (myClusterId != 0) {
        BlockListNode *blockListNode;

        blockListNode = makeNode(BlockListNode);
        blockListNode->relid = RelationGetRelid(dmlDesc->mainRel);
        blockListNode->blockListInfo = dmlDesc->visibilityInfo;

        if (Gp_role == GP_ROLE_EXECUTE) {
            char *message;

            message = nodeToString(blockListNode);
            NotifyMyFrontEnd(CDB_NOTIFY_TILE, message, MyProcPid);
            pq_flush();
            pfree(message);
        } else {
            cc_send_modify_tabble(blockListNode);
        }
    } else
        table_close(dmlDesc->visibilityRel, RowExclusiveLock);
}

bool
tile_fetch(Relation rel, TileFetchDesc desc, ItemPointer tid, Snapshot snapshot,
           TupleTableSlot *slot) {
    TileKey key;
    MinimalTuple mtuple;


    key = tid_get_blockkey(tid);


    if (desc->buffer && blockkey_equal(key, desc->buffer->key)) {
    } else if (rel->tileDmlDesc && rel->tileDmlDesc->newBuffer &&
               (blockkey_equal(key, rel->tileDmlDesc->newBuffer->key) ||
                !blockkey_is_valid(key))) {
        if (desc->buffer && desc->bufferShouldFree)
            tile_release_buf(desc->buffer);

        desc->buffer = rel->tileDmlDesc->newBuffer;
        desc->bufferCurPtrTupNum = 1;
        desc->bufferCurPtrDiff = 0;
        desc->bufferShouldFree = false;
    } else {
        if (desc->buffer == NULL || !desc->bufferShouldFree) {
            desc->buffer = tile_init_buf();
        }
        tile_read_buf(desc->mainRel, *tid, key, desc->buffer);
        desc->bufferCurPtrTupNum = 1;
        desc->bufferCurPtrDiff = 0;
    }

    if (tile_tid_get_seq(tid) < desc->bufferCurPtrTupNum) {
        desc->bufferCurPtrTupNum = 1;
        desc->bufferCurPtrDiff = 0;
    }

    while (desc->bufferCurPtrDiff < desc->buffer->bufSize &&
           desc->bufferCurPtrTupNum < tile_tid_get_seq(tid)) {
        mtuple = (MinimalTuple) (desc->buffer->bufStartPtr + desc->bufferCurPtrDiff);
        desc->bufferCurPtrDiff += mtuple->t_len;
        desc->bufferCurPtrTupNum++;
    }

    if (desc->bufferCurPtrTupNum != tile_tid_get_seq(tid)) {
        return false;
    }

    mtuple = (MinimalTuple) (desc->buffer->bufStartPtr + desc->bufferCurPtrDiff);
    ExecForceStoreMinimalTuple(mtuple, slot, false);
    slot->tts_tableOid = RelationGetRelid(desc->mainRel);
    slot->tts_tid = *tid;

    return true;
}


void
tile_fetch_finish(TileFetchDesc desc) {
    if (desc->visibilityRel)
        table_close(desc->visibilityRel, AccessShareLock);
}

void
tile_scan_prepare_dispatch(Relation rel, Snapshot snapshot) {
    Oid blockListRelid;
    Relation blockListRel;
    MemoryContext oldCtx;

    if (!CatCollectorActive())
        return;

    oldCtx = MemoryContextSwitchTo(catCollectCtx);

    blockListRelid = PgTileGetVisiRelId(RelationGetRelid(rel));
    blockListRel = table_open(blockListRelid, AccessShareLock);
    tile_get_visi(blockListRel, snapshot);

    table_close(blockListRel, AccessShareLock);

    MemoryContextSwitchTo(oldCtx);
}


TableScanDesc
tile_beginscan(Relation relation, Snapshot snapshot,
               int nkeys, ScanKey key,
               ParallelTableScanDesc parallel_scan,
               uint32 flags) {
    TileScanDesc scan;

    RelationIncrementReferenceCount(relation);

    scan = (TileScanDesc) palloc0(sizeof(TileScanDescData));
    scan->rs_base.rs_rd = relation;
    scan->rs_base.rs_snapshot = snapshot;
    scan->rs_base.rs_nkeys = nkeys;
    scan->rs_base.rs_flags = flags;
    scan->rs_base.rs_parallel = parallel_scan;

    if (!(snapshot && IsMVCCSnapshot(snapshot)))
        scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

    if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN)) {
        /*
         * Ensure a missing snapshot is noticed reliably, even if the
         * isolation mode means predicate locking isn't performed (and
         * therefore the snapshot isn't used here).
         */
        Assert(snapshot);
        PredicateLockRelation(relation, snapshot);
    }

    /*
     * we do this here instead of in initscan() because heap_rescan also calls
     * initscan() and we don't want to allocate memory again
     */
    if (nkeys > 0)
        scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
    else
        scan->rs_base.rs_key = NULL;

    tile_init_scan(scan);

    scan->visiInfo = tile_get_visi(scan->visiRel, snapshot);

    return (TableScanDesc) scan;
}

static void
tile_init_scan(TileScanDesc scan) {
    Oid visiRelOid;

    scan->buffer = palloc(TILE_BLOCK_SIZE);
    scan->bufferPointer = scan->buffer;
    scan->bufferLen = 0;
    scan->curPageIdx = 0;

    visiRelOid = PgTileGetVisiRelId(scan->rs_base.rs_rd->rd_id);
    scan->visiRel = table_open(visiRelOid, AccessShareLock);

    scan->visiInfo = NIL;
    scan->bufTupleLenArr = palloc(TILE_BLOCK_SIZE);
    scan->bufTupleLenArrPtr = scan->bufTupleLenArr;
    scan->bufTupleNum = 0;

    scan->tuple = NULL;
    scan->blockid = 0;
    scan->seq = 0;
    if (scan->scanCtx)
        MemoryContextDelete(scan->scanCtx);
    scan->scanCtx = AllocSetContextCreate(CurrentMemoryContext,
                                          "TileScanContext",
                                          ALLOCSET_DEFAULT_SIZES);
}

static List *
tile_get_visi(Relation visiRel, Snapshot snapshot) {
    Datum values[Visi_natt];
    BlockDesc *block_desc;
    List *visiInfo = NIL;

    SysScanDesc sysScan;
    HeapTuple sysTuple;

    /* Need a cutoff xmin for HeapTupleSatisfiesVacuum */
    sysScan = systable_beginscan(visiRel, InvalidOid, false, snapshot, 0, NULL);
    while ((sysTuple = systable_getnext(sysScan)) != NULL) {
        bool isNull;

        values[Visi_file_size -1] =
            heap_getattr(sysTuple, Visi_file_size,
                         RelationGetDescr(visiRel), &isNull);
        values[Visi_file_path -1] =
            heap_getattr(sysTuple, Visi_file_path,
                         RelationGetDescr(visiRel), &isNull);
        values[Visi_tuple_num -1] =
            heap_getattr(sysTuple, Visi_tuple_num,
                         RelationGetDescr(visiRel), &isNull);

        block_desc = makeNode(BlockDesc);
        block_desc->block_size = DatumGetUInt32(values[Visi_file_size -1]);
        strcpy(block_desc->block_name,
               DatumGetName(values[Visi_file_path -1])->data);
        block_desc->block_tuple_num = DatumGetUInt32(values[Visi_tuple_num -1]);;
        block_desc->blockid = heaptid_to_blockid(sysTuple->t_self);
        visiInfo = lappend(visiInfo, block_desc);
    }
    systable_endscan(sysScan);

    return visiInfo;
}

void
tile_endscan(TableScanDesc sscan) {
    TileScanDesc desc = (TileScanDesc) sscan;
    RelationDecrementReferenceCount(desc->rs_base.rs_rd);

    table_close(desc->visiRel, AccessShareLock);
    if (IS_CATALOG_SERVER())
        list_free_deep(desc->visiInfo);

    pfree(desc->buffer);
    pfree(desc->bufTupleLenArr);
    if (desc->scanCtx) {
        MemoryContextDelete(desc->scanCtx);
    }
}

void
tile_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
            bool allow_strat, bool allow_sync, bool allow_pagemode) {
    // reset
    TileScanDescData *oscan = (TileScanDescData *) sscan;
    oscan->bufferPointer = oscan->buffer;
    oscan->bufferLen = 0;
    oscan->curPageIdx = 0;
    oscan->bufTupleLenArrPtr = oscan->bufTupleLenArr;
    oscan->bufTupleNum = 0;
}

bool
tile_getnextslot(TableScanDesc sscan, ScanDirection direction,
                 TupleTableSlot *slot) {
    // note: seq2 starting from 1
    TileScanDesc desc = (TileScanDesc) sscan;
    MinimalTuple mtuple;
    uint32 tuple_len;

    if (desc->tuple)
        pfree(desc->tuple);
    desc->tuple = NULL;


    if (list_length(desc->visiInfo) == 0) {
        return false;
    }

    if (ScanDirectionIsForward(direction)) {
        if (desc->bufferLen == 0) {
            // the initial state
            // get the first block, and read from the scratch
            if (!tile_get_page(desc, NOMOVE))
                return false;
        }

        if (desc->seq == 0) {
            // previously we've stepped backward out of the bound, now we should start from the scratch
            // the ptr should be in the correct position
            desc->seq = 1;
            Assert(
                desc->bufferPointer == desc->buffer && desc->bufTupleLenArrPtr == desc->
                bufTupleLenArr);
        } else if (desc->seq == desc->bufTupleNum) {
            // the last tuple has already been read; namely, now point to the last tuple
            // the cur block has been used up, now fetch a newer block and read from the scratch
            Assert(
                desc->bufTupleLenArrPtr - desc->bufTupleLenArr == sizeof(mtuple->t_len) *
                (desc->bufTupleNum - 1));
            if (!tile_get_page(desc, FORWARD)) {
                // should mark the cur block's cursor out of the boundary,
                // otherwise, it would be wrong if the user want to fetch backward
                memcpy(&tuple_len, desc->bufTupleLenArrPtr, sizeof(mtuple->t_len));
                desc->bufferPointer += tuple_len;
                desc->seq += 1;
                desc->bufTupleLenArrPtr += sizeof(mtuple->t_len);
                return false;
            }

            desc->seq += 1;
        } else {
            // the cur block is available to provide the desired tuple, read the next tuple
            Assert(
                desc->bufTupleLenArrPtr - desc->bufTupleLenArr != desc->bufTupleNum *
                sizeof(mtuple->t_len)
                && desc->seq < desc->bufTupleNum);
            memcpy(&tuple_len, desc->bufTupleLenArrPtr, sizeof(mtuple->t_len));
            desc->bufferPointer += tuple_len;
            desc->seq += 1;
            desc->bufTupleLenArrPtr += sizeof(mtuple->t_len);
        }
    } else if (ScanDirectionIsBackward(direction)) {
        Assert(desc->bufferLen != 0);
        if (desc->bufferPointer == desc->buffer) {
            // step back to the former block
            if (!tile_get_page(desc, BACKWARD)) {
                //reset the seq2, the next time we know it should start from the scratch
                desc->seq = 0;
                return false;
            }
            // point to the tail
            desc->bufTupleLenArrPtr =
                    desc->bufTupleLenArr + sizeof(mtuple->t_len) * desc->bufTupleNum;
            desc->bufferPointer = desc->buffer + desc->bufferLen;
            desc->seq = desc->bufTupleNum + 1;
        }
        // step back one tuple
        Assert(
            desc->bufTupleLenArr != desc->bufTupleLenArrPtr && desc->bufferPointer != desc
            ->buffer &&
            desc->seq > 1);
        desc->bufTupleLenArrPtr -= sizeof(mtuple->t_len);
        memcpy(&tuple_len, desc->bufTupleLenArrPtr, sizeof(mtuple->t_len));
        desc->bufferPointer -= tuple_len;
        desc->seq -= 1;
    }

    mtuple = (MinimalTuple) desc->bufferPointer;
    desc->tuple = MemoryContextAlloc(desc->scanCtx, mtuple->t_len);
    memcpy(desc->tuple, desc->bufferPointer, mtuple->t_len);
    ExecStoreMinimalTuple((MinimalTuple) desc->tuple, slot, false);
    slot->tts_tid = blockid_seq_get_tile_tid(desc->blockid, desc->seq, desc->key);

    return true;
}

static void getblock_internal(TileScanDesc scanDesc) {
    char *bucketPath;
    uint32 count;
    BlockDesc *blockDesc;
    MinimalTuple mtuple;

    Assert(scanDesc->curPageIdx < list_length(scanDesc->visiInfo) &&
        scanDesc->curPageIdx >= 0);
    blockDesc = (BlockDesc *) list_nth(scanDesc->visiInfo, scanDesc->curPageIdx);

    scanDesc->cur_buf_block_vacuum_status = blockDesc->block_htsv_result;
    scanDesc->blockid = blockDesc->blockid;
    scanDesc->key = GetBlockKeyFromBlockName(blockDesc->block_name);
    scanDesc->seq = 0;

    scanDesc->bufTupleLenArrPtr = scanDesc->bufTupleLenArr;
    scanDesc->bufTupleNum = blockDesc->block_tuple_num;

    bucketPath = TileMakeBucketPath(scanDesc->rs_base.rs_rd->rd_node);
    scanDesc->bufferLen = S3GetObject2(s3Client, bucketPath, blockDesc->block_name,
                                       scanDesc->buffer);
    Assert(scanDesc->bufferLen == blockDesc->block_size);
    pfree(bucketPath);
    scanDesc->bufferPointer = scanDesc->buffer;

    count = 0;
    while (count < blockDesc->block_tuple_num) {
        Assert(scanDesc->bufferPointer - scanDesc->buffer < TILE_BLOCK_SIZE);
        mtuple = (MinimalTuple) (scanDesc->bufferPointer);
        memcpy(scanDesc->bufTupleLenArrPtr, &mtuple->t_len, sizeof(mtuple->t_len));
        scanDesc->bufTupleLenArrPtr += sizeof(mtuple->t_len);
        count += 1;
        scanDesc->bufferPointer += mtuple->t_len;
    }
    scanDesc->bufferPointer = scanDesc->buffer;
    scanDesc->bufTupleLenArrPtr = scanDesc->bufTupleLenArr;
}

static bool
tile_get_page(TileScanDesc desc, BLOCKMOVE page_move) {
    switch (page_move) {
        case FORWARD: {
            if (desc->curPageIdx == list_length(desc->visiInfo) - 1) {
                return false;
            }
            desc->curPageIdx += 1;
        }
        break;
        case BACKWARD: {
            if (desc->curPageIdx == 0) {
                // there is no former blocks
                return false;
            }
            desc->curPageIdx -= 1;
        }
        break;
        default:
            break;
    }
    getblock_internal(desc);

    return true;
}

bool tile_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
                                  BufferAccessStrategy bstrategy) {
    TileScanDesc desc = (TileScanDesc) scan;
    desc->curPageIdx = blockno;
    getblock_internal(desc);

    return true;
}

bool tile_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
                                  double *liverows, double *deadrows,
                                  TupleTableSlot *slot) {
    TileScanDesc desc = (TileScanDesc) scan;
    MinimalTuple mtuple;
    bool sample_it = false;


    if (desc->bufferLen == 0 || desc->bufferPointer - desc->buffer >= desc->bufferLen) {
        // no tuple left yet
        return false;
    }

    mtuple = (MinimalTuple) desc->bufferPointer;
    ExecStoreMinimalTuple(mtuple, slot, false);
    slot->tts_tid = blockid_seq_get_tile_tid(desc->blockid, desc->seq, desc->key);
    desc->seq++;

    switch (desc->cur_buf_block_vacuum_status) {
        case HEAPTUPLE_LIVE:
            sample_it = true;
            *liverows += 1;
            break;
        case HEAPTUPLE_DEAD:
        case HEAPTUPLE_RECENTLY_DEAD:
            *deadrows += 1;
            break;
        case HEAPTUPLE_INSERT_IN_PROGRESS:
            sample_it = true;
            *liverows += 1;
            break;
        case HEAPTUPLE_DELETE_IN_PROGRESS:
            *deadrows += 1;
            break;
        default:
            elog(ERROR, "unexpected TupleSatisfiesVacuum result");
            break;
    }


    // move forward in order to serve the next request
    desc->bufferPointer += mtuple->t_len;
    desc->seq++;

    if (sample_it)
        return true;

    return false;
}

void
tile_clear_table(RelFileNode rd_node) {
    S3Objs s3_objs;
    ListCell *lc;
    S3ObjKey s3_obj_key;

    s3_obj_key.bucketName = TileMakeBucketPath(rd_node);
    s3_objs = S3DeleteObjects(s3Client, s3_obj_key.bucketName);

    foreach(lc, s3_objs.objPathList)
    {
        s3_obj_key.objectName = lfirst(lc);
        S3DeleteObject(s3Client, s3_obj_key.objectName);
        pfree(s3_obj_key.objectName);
    }

	pfree(s3_obj_key.bucketName);
}

void
tile_clear_db(Oid spcNode, Oid dbNode)
{
    char *prefix;
    S3Objs s3_objs;
    ListCell *lc;

    prefix = TileMakeDbPrefix(spcNode, dbNode);
    s3_objs = S3DeleteObjects(s3Client, prefix);
    pfree(prefix);

    foreach(lc, s3_objs.objPathList)
    {
        char *object_name;

        object_name = lfirst(lc);
        S3DeleteObject(s3Client, object_name);
        pfree(object_name);
    }
}

void
CreateBuckets(RelFileNode node) {
    // char *bucketPath;
    //
    // bucketPath = TileMakeBucketPath(node);
    // S3CreateBucket(s3Client, bucketPath);
    // pfree(bucketPath);
}

void
tile_access_release(Relation relation) {
    if (relation->tileDmlDesc) {
        tile_update_finish(relation->tileDmlDesc);
        tile_release_buf(relation->tileDmlDesc->oldBuffer);
        tile_release_buf(relation->tileDmlDesc->newBuffer);
        pfree(relation->tileDmlDesc);
        relation->tileDmlDesc = NULL;
    }
    if (relation->tileFetchDesc) {
        tile_fetch_finish(relation->tileFetchDesc);
        if (relation->tileFetchDesc->bufferShouldFree)
            tile_release_buf(relation->tileFetchDesc->buffer);

        pfree(relation->tileFetchDesc);
        relation->tileFetchDesc = NULL;
    }
}
