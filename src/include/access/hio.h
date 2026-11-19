/*-------------------------------------------------------------------------
 *
 * hio.h
 *	POSTGRES 堆访问方法的输入/输出定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/*
 * 批量插入状态 --- heapam.c 和 hio.c 私有
 *
 * 若 current_buf 非 InvalidBuffer，则我们持有该缓冲区的
 * 额外钉住。
 *
 * typedef（BulkInsertStateData *BulkInsertState）见 heapam.h
 */
typedef struct BulkInsertStateData
{
	BufferAccessStrategy strategy;	/* 自用 BULKWRITE 策略对象 */
	Buffer		current_buf;	/* 当前插入目标页 */

	/*
	 * 批量扩展的状态。
	 *
	 * last_free..next_free 是上次扩展时未用的其他页面。
	 * 它们到我们使用时可能已被占用，
	 * 所以仍需重新检查。
	 *
	 * XXX：这些最终大概应改放在 RelationData 中，
	 * 紧邻 targetblock。
	 *
	 * already_extended_by 是本批量插入已扩展的页数。
	 * 若我们已经扩展了大量页面，
	 * 则今后扩展时我们可以更激进。
	 */
	BlockNumber next_free;
	BlockNumber last_free;
	uint32		already_extended_by;
} BulkInsertStateData;


extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
								 HeapTuple tuple, bool token);
extern Buffer RelationGetBufferForTuple(Relation relation, Size len,
										Buffer otherBuffer, int options,
										BulkInsertStateData *bistate,
										Buffer *vmbuffer, Buffer *vmbuffer_other,
										int num_pages);

#endif							/* HIO_H */
