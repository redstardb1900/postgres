/*-------------------------------------------------------------------------
 *
 * tidbitmap.h
 *	PostgreSQL 元组标识 (TID) 位图包
 *
 * 本模块提供在精神上类似 Bitmapset 的位图结构，
 * 但专为存储元组标识 (TID) 即 ItemPointer 集合而改。
 * 特别照顾 ItemPointer 划分为 BlockNumber 与
 * OffsetNumber。又因希望能在内存中存很大的元组集，
 * 支持"有损"存储:不再记住页上各元组偏移,只记
 * 某页需要访问
 * 这一
 * 事实。
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * src/include/nodes/tidbitmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIDBITMAP_H
#define TIDBITMAP_H

#include "storage/itemptr.h"
#include "utils/dsa.h"


/*
 * 实际位图表示对 tidbitmap.c 私有。调用方可对其做
 * IsA(x, TIDBitmap)，除此无他。
 */
typedef struct TIDBitmap TIDBitmap;

/* 同理,TBMIterator 也私有 */
typedef struct TBMIterator TBMIterator;
typedef struct TBMSharedIterator TBMSharedIterator;

/* tbm_iterate 的结果结构 */
typedef struct TBMIterateResult
{
	BlockNumber blockno;		/* 含元组的页号 */
	int			ntuples;		/* -1 表示有损结果 */
	bool		recheck;		/* 元组是否应重检？ */
	/* 注意:ntuples < 0 则 recheck 恒真 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} TBMIterateResult;

/* nodes/tidbitmap.c 中的函数原型 */

extern TIDBitmap *tbm_create(long maxbytes, dsa_area *dsa);
extern void tbm_free(TIDBitmap *tbm);
extern void tbm_free_shared_area(dsa_area *dsa, dsa_pointer dp);

extern void tbm_add_tuples(TIDBitmap *tbm,
						   const ItemPointer tids, int ntids,
						   bool recheck);
extern void tbm_add_page(TIDBitmap *tbm, BlockNumber pageno);

extern void tbm_union(TIDBitmap *a, const TIDBitmap *b);
extern void tbm_intersect(TIDBitmap *a, const TIDBitmap *b);

extern bool tbm_is_empty(const TIDBitmap *tbm);

extern TBMIterator *tbm_begin_iterate(TIDBitmap *tbm);
extern dsa_pointer tbm_prepare_shared_iterate(TIDBitmap *tbm);
extern TBMIterateResult *tbm_iterate(TBMIterator *iterator);
extern TBMIterateResult *tbm_shared_iterate(TBMSharedIterator *iterator);
extern void tbm_end_iterate(TBMIterator *iterator);
extern void tbm_end_shared_iterate(TBMSharedIterator *iterator);
extern TBMSharedIterator *tbm_attach_shared_iterate(dsa_area *dsa,
													dsa_pointer dp);
extern long tbm_calculate_entries(double maxbytes);

#endif							/* TIDBITMAP_H */
