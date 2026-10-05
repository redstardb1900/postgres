/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for mapping BufferTags to buffer indexes.
 *
 * Note: the routines in this file do no locking of their own.  The caller
 * must hold a suitable lock on the appropriate BufMappingLock, as specified
 * in the comments.  We can't do the locking inside these functions because
 * in most cases the caller needs to adjust the buffer header contents
 * before the lock is released (see notes in README).
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

/* 缓冲区查找散列表的项 */
typedef struct
{
	BufferTag	key;			/* 磁盘页的标签 */
	int			id;				/* 关联的缓冲区标识 */
} BufferLookupEnt;

static HTAB *SharedBufHash;


/*
 * 估计映射散列表所需的空间。
 *		size 是想要的散列表大小（可能大于 NBuffers）。
 */
Size
BufTableShmemSize(int size)
{
	return hash_estimate_size(size, sizeof(BufferLookupEnt));
}

/*
 * 初始化用于映射缓冲区的共享内存散列表。
 *		size 是想要的散列表大小（可能大于 NBuffers）。
 */
void
InitBufTable(int size)
{
	HASHCTL		info;

	/* 假定目前还不需要加锁 */

	/* BufferTag 映射到 Buffer */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(BufferLookupEnt);
	info.num_partitions = NUM_BUFFER_PARTITIONS;

	SharedBufHash = ShmemInitHash("Shared Buffer Lookup Table",
								  size, size,
								  &info,
								  HASH_ELEM | HASH_BLOBS | HASH_PARTITION);
}

/*
 * BufTableHashCode
 *		计算与 BufferTag 关联的散列码。
 *
 * 这必须连同标签一起传给查找、插入、删除例程。
 * 我们这样做是因为调用者需要知道散列码，
 * 以便确定要锁住哪个缓冲区分区，而且我们不想
 * 把散列计算做两遍（hash_any 有点慢）。
 */
uint32
BufTableHashCode(BufferTag *tagPtr)
{
	return get_hash_value(SharedBufHash, (void *) tagPtr);
}

/*
 * BufTableLookup
 *		查找给定的 BufferTag；返回缓冲区标识，未找到则返回 -1。
 *
 * 调用者必须至少持有该标签所在分区的 BufMappingLock 共享锁。
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_FIND,
									NULL);

	if (!result)
		return -1;

	return result->id;
}

/*
 * BufTableInsert
 *		为给定标签和缓冲区标识插入散列表项，
 *		除非该标签已有项。
 *
 * 插入成功时返回 -1。若已存在冲突项，
 * 则返回该项里的缓冲区标识。
 *
 * 调用者必须持有该标签所在分区的 BufMappingLock 排他锁。
 */
int
BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{
	BufferLookupEnt *result;
	bool		found;

	Assert(buf_id >= 0);		/* -1 保留给不在表中的情形 */
	Assert(tagPtr->blockNum != P_NEW);	/* 无效标签 */

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_ENTER,
									&found);

	if (found)					/* 发现表里已经有东西 */
		return result->id;

	result->id = buf_id;

	return -1;
}

/*
 * BufTableDelete
 *		删除给定标签的散列表项（该项必须存在）。
 *
 * 调用者必须持有该标签所在分区的 BufMappingLock 排他锁。
 */
void
BufTableDelete(BufferTag *tagPtr, uint32 hashcode)
{
	BufferLookupEnt *result;

	result = (BufferLookupEnt *)
		hash_search_with_hash_value(SharedBufHash,
									tagPtr,
									hashcode,
									HASH_REMOVE,
									NULL);

	if (!result)				/* 不应当发生 */
		elog(ERROR, "shared buffer hash table corrupted");
}
