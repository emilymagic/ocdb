#ifndef TILEAM_H
#define TILEAM_H

#include "access/heapam.h"
#include "access/tableam.h"

#define TILE_BLOCK_SIZE (16*1024*1024)
#define TILE_PATH_SIZE 40
#define TILE_KEY_SIZE 16

typedef struct TileKey
{
	uint64 tid;
	uint64 seq;
} TileKey;

typedef struct TileBuf
{
	uint32 blockid;
	TileKey key;
	char *bufStartPtr;  //starting point
	uint32 bufSize;
} TileBuf;

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

typedef struct BlockListNode BlockListNode;

extern void tile_insert_block_list_notify(char *message);
extern void tile_insert_block_list_cs(BlockListNode *blockListNode);


extern void tile_clear_table(RelFileNode rd_node);
extern void tile_clear_db(Oid spcNode, Oid dbNode);
extern void tile_access_initialization(Relation relation);
extern void s3_init(void);
extern void s3_destroy(void);

extern void release_tile_dml_state(Relation rel);

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
