#include "postgres.h"

#include "access/tileam.h"

bool
blockkey_is_valid(TileKey key)
{
	if (key.tid != 0)
		return true;

	if (key.seq != 0)
		return true;

	return false;
}

bool
blockkey_equal(TileKey key1, TileKey key2)
{
	if (key1.tid != key2.tid)
		return false;

	if (key1.seq != key2.seq)
		return false;

	return true;
}

uint32
tile_tid_get_seq(ItemPointer tid)
{
	uint32 seq;

	seq = tid->ip_blkid.bi_lo & 0x000f;
	seq = seq << 16;
	seq += tid->ip_posid;

	return seq;
}

uint32
tile_tid_get_blockid(ItemPointerData tid)
{
	uint32 blockid;

	blockid = tid.ip_blkid.bi_hi;
	blockid = blockid << 12;
	blockid += (tid.ip_blkid.bi_lo & 0xfff0) >> 4;

	return blockid;
};

ItemPointerData
blockid_seq_get_tile_tid(uint32 blockid, uint32 seq, TileKey key)
{
	ItemPointerData tid;

	tid.ip_blkid.bi_hi = (blockid & 0x0ffff000) >> 12;
	tid.ip_blkid.bi_lo = (blockid & 0x00000fff) << 4;
	tid.ip_blkid.bi_lo += (seq & 0x000f0000) >> 16;

	tid.ip_posid = seq & 0x0000ffff;

	tid.xid = key.tid;
	tid.seq = key.seq;

	return tid;
}

ItemPointerData
blockid_to_heaptid(uint32 blockid)
{
	ItemPointerData tid;

	tid.ip_posid = blockid & 0x000000ff;
	tid.ip_blkid.bi_lo = blockid & 0x00ffff00;
	tid.ip_blkid.bi_hi = blockid & 0xff000000;

	return tid;
}

uint32
heaptid_to_blockid(ItemPointerData tid)
{
	uint32 blockid;

	blockid = tid.ip_blkid.bi_hi & 0x000f;
	blockid = blockid << 16;
	blockid += tid.ip_blkid.bi_lo;
	blockid = blockid << 8;
	blockid += tid.ip_posid;

	return blockid;
}

static char *
bytes_to_string(uint8 *byteData, uint32 byteDataSize)
{
	char *string = palloc0((byteDataSize * 2) + 1);;
	char *stringPtr = string;
	uint8 *bytePtr = byteData;

	while (bytePtr < byteData + byteDataSize)
	{
		sprintf(stringPtr, "%02x", *bytePtr);
		stringPtr += 2;
		bytePtr++;
	}

	return string;
}

static ByteKey *
string_to_bytes(char *string, uint32 *byteSize)
{
	char tmp[3];
	char *stringPtr = string;
	uint8 *bytePtr;
	ByteKey *byte_key;

	*byteSize = strlen(string) / 2;
	byte_key = palloc(*byteSize + sizeof(uint64));
	byte_key->size = *byteSize;
	bytePtr = byte_key->data;

	while (bytePtr < byte_key->data + *byteSize)
	{
		tmp[0] = *stringPtr;
		tmp[1] = *(stringPtr + 1);
		tmp[2] = 0;
		*bytePtr = strtol(tmp, NULL, 16);
		bytePtr++;
		stringPtr += 2;
	}

	return byte_key;
}

char *
GetBlockNameFromKey(TileKey key)
{
	char *block_name_str = NULL;
	uint8 buf[TILE_KEY_SIZE];
	uint8 *cur;
	uint64 net;

	cur = buf;

	net = pg_hton64(key.tid);
	memcpy(cur, &net, sizeof(net));
	cur += sizeof(net);

	net = pg_hton64(key.seq);
	memcpy(cur, &net, sizeof(net));
	cur += sizeof(net);

	block_name_str = bytes_to_string(buf, TILE_KEY_SIZE);

	return block_name_str;
}

TileKey
GetBlockKeyFromBlockName(char *blockname)
{
	ByteKey *byte_key;
	uint32 key_size;
	TileKey key;
	uint8 *cur;
	uint64 net;

	byte_key = string_to_bytes(blockname, &key_size);
	Assert(key_size == TILE_KEY_SIZE);

	cur = byte_key->data;
	memcpy(&net, cur, sizeof(net));
	key.tid = pg_ntoh64(net);
	cur += sizeof(net);

	memcpy(&net, cur, sizeof(net));
	key.seq = pg_ntoh64(net);
	cur += sizeof(net);

	pfree(byte_key);

	return key;
}

#define KEY_RELFILENNODE_LEN 12

char *
TileMakeBucketPath(RelFileNode relFileNode)
{
	uint8 buf[KEY_RELFILENNODE_LEN];
	char *bucketPath= NULL;
	uint8 *cur = buf;

	*(uint32 *)cur = pg_hton32(relFileNode.spcNode);
	cur += 4;
	*(uint32 *)cur = pg_hton32(relFileNode.dbNode);
	cur += 4;
	*(uint32 *)cur = pg_hton32(relFileNode.relNode);
	cur += 4;

	bucketPath = bytes_to_string(buf, KEY_RELFILENNODE_LEN);

	return bucketPath;
}

#define KEY_DB_PREFIX_LEN 8
char *
TileMakeDbPrefix(Oid spcNode, Oid dbNode)
{
	uint8 buf[KEY_DB_PREFIX_LEN];
	char *bucketPath= NULL;
	uint8 *cur = buf;

	*(uint32 *)cur = pg_hton32(spcNode);
	cur += 4;
	*(uint32 *)cur = pg_hton32(dbNode);
	cur += 4;

	bucketPath = bytes_to_string(buf, KEY_DB_PREFIX_LEN);

	return bucketPath;
}