#ifndef TILEAM_H
#define TILEAM_H

#include "access/tableam.h"
#include "access/heapam.h"

#define TILE_BLOCK_SIZE (16*1024*1024)
#define TILE_PATH_SIZE 40
#define TILE_KEY_SIZE 16

typedef struct TileKey
{
	uint64 tid;
	uint64 seq;
} TileKey;

typedef enum
{
	TILE_INSERT,
	TILE_DELETE,
	TILE_UPDATE
} DMLType;

typedef enum
{
	FORWARD,
	NOMOVE,
	BACKWARD
} BLOCKMOVE;

typedef struct TileBuf
{
	uint32 blockid;
	TileKey key;
	char *bufStartPtr;  //starting point
	uint32 bufSize;
} TileBuf;

typedef struct TileDmlDescData
{
	Relation mainRel;
	Relation visibilityRel;

	TileBuf *oldBuffer;
	uint32 oldBufferCurPtrTupNum; //0: invalid, starting from 1; used in copying data from old block to new block
	uint32 oldBufferCurPtrDiff;

	TileBuf *newBuffer;
	uint32 newBufferTupNum;  // only used in insertdesc. in order to cal tid
	List *visibilityInfo;
} TileDmlDescData;

typedef TileDmlDescData *TileDmlDesc;

typedef struct TileFetchDescData
{
	Relation mainRel;
	Relation visibilityRel;
	TileBuf *buffer;
	bool	bufferShouldFree;
	uint32 bufferCurPtrTupNum;
	uint32 bufferCurPtrDiff;  //current point
} TileFetchDescData;

typedef TileFetchDescData *TileFetchDesc;

typedef struct TileDmlState
{
	TileDmlDesc dmlDesc;
	TileFetchDesc fetchDesc;
} TileDmlState;

typedef struct BlockDesc
{
	NodeTag type;
	uint32 block_size;
	uint32 block_tuple_num;
	uint32 blockid;
	char block_name[TILE_KEY_SIZE * 2 + 1];
	HTSV_Result block_htsv_result;
} BlockDesc;

typedef struct BlockDesc2
{
	NodeTag type;

	uint32			blockid;
	TileKey	newKey;
	uint32			block_size;
	uint32			block_tuple_num;
} BlockDesc2;

/*
 * Scan desc
 */
typedef struct TileScanDescData
{
	TableScanDescData rs_base;
	char *buffer;
	char *bufferPointer;
	uint32 bufferLen;
	Relation visiRel;
	List *visiInfo;
	uint32 curPageIdx; // the next blockno to be read, starting from zero
	HTSV_Result cur_buf_block_vacuum_status;
	char *bufTupleLenArr;
	char *bufTupleLenArrPtr;
	uint32 bufTupleNum;
	uint32 blockid;
	TileKey key;
	uint32 seq;
	char *tuple;
	MemoryContext scanCtx;
} TileScanDescData;


typedef TileScanDescData *TileScanDesc;

extern void *s3Client;

extern void tile_scan_prepare_dispatch(Relation rel, Snapshot snapshot);
extern TableScanDesc tile_beginscan(Relation relation, Snapshot snapshot,
									  int nkeys, ScanKey key,
									  ParallelTableScanDesc parallel_scan,
									  uint32 flags);
extern void tile_endscan(TableScanDesc sscan);
extern void tile_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
						  bool allow_strat, bool allow_sync,
						  bool allow_pagemode);
extern bool tile_getnextslot(TableScanDesc sscan, ScanDirection direction,
							   TupleTableSlot *slot);

extern 	bool tile_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
											   BufferAccessStrategy bstrategy);

extern 	bool tile_scan_analyze_next_tuple(TableScanDesc scan,
                                                  TransactionId OldestXmin,
                                                  double *liverows,
                                                  double *deadrows,
                                                  TupleTableSlot *slot);

typedef struct BlockListNode BlockListNode;

extern void tile_insert(TileDmlDesc dmlDesc, MinimalTuple minimalTuple);
extern void tile_insert_block_list_notify(char *message);
extern void tile_insert_block_list_cs(BlockListNode *blockListNode);

extern TM_Result tile_delete(TileDmlDesc dmlDesc, ItemPointer tid);


extern TM_Result tile_update(TileDmlDesc dmlDesc, ItemPointer otid,
							   MinimalTuple newTuple);
extern void tile_update_finish(TileDmlDesc dmlDesc);


extern bool tile_fetch(Relation rel, TileFetchDesc desc, ItemPointer tid,
						 Snapshot snapshot, TupleTableSlot *slot);
extern void tile_fetch_finish(TileFetchDesc desc);


extern void tile_clear_table(RelFileNode rd_node);
extern void tile_clear_db(Oid spcNode, Oid dbNode);
extern void tile_access_initialization(Relation relation);
extern void tile_access_release(Relation relation);
extern void s3_init(void);
extern void s3_destroy(void);

extern void release_tile_dml_state(Relation rel);

extern void tile_release_buf(TileBuf *tileBuffer);
extern TileBuf *tile_init_buf(void);
extern TileBuf *tile_init_buf_with_tid(void);
extern void CreateBuckets(RelFileNode node);

/*
 * Utils
 */
typedef struct ByteKey
{
	uint64 size;
	uint8 data[0];
} ByteKey;

extern bool blockkey_is_valid(TileKey key);
extern bool blockkey_equal(TileKey key1, TileKey key2);
extern uint32 tile_tid_get_seq(ItemPointer tid);
extern uint32 tile_tid_get_blockid(ItemPointerData tid);
extern ItemPointerData blockid_seq_get_tile_tid(uint32 blockid, uint32 seq,
												 TileKey key);
extern ItemPointerData blockid_to_heaptid(uint32 blockid);
extern uint32 heaptid_to_blockid(ItemPointerData tid);
extern char *GetBlockNameFromKey(TileKey key);
extern TileKey GetBlockKeyFromBlockName(char *blockname);
extern char *TileMakeBucketPath(RelFileNode relFileNode);
extern char *TileMakeDbPrefix(Oid spcNode, Oid dbNode);

#endif //TILEAM_H
