/*-------------------------------------------------------------------------
 *
 * rewriteheap.h
 *	  堆重写支持函数的声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/access/rewriteheap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITE_HEAP_H
#define REWRITE_HEAP_H

#include "access/htup.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* 结构定义私有于 rewriteheap.c */
typedef struct RewriteStateData *RewriteState;

extern RewriteState begin_heap_rewrite(Relation old_heap, Relation new_heap,
									   TransactionId oldest_xmin, TransactionId freeze_xid,
									   MultiXactId cutoff_multi);
extern void end_heap_rewrite(RewriteState state);
extern void rewrite_heap_tuple(RewriteState state, HeapTuple old_tuple,
							   HeapTuple new_tuple);
extern bool rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple);

/*
 * 单条逻辑重写映射的磁盘数据格式。
 */
typedef struct LogicalRewriteMappingData
{
	RelFileLocator old_locator;
	RelFileLocator new_locator;
	ItemPointerData old_tid;
	ItemPointerData new_tid;
} LogicalRewriteMappingData;

/* ---
 * 文件名由下列以连字符分隔的成分
 * 组成：
 * 1) 数据库 oid，共享关系为 InvalidOid
 * 2) 该关系的 oid
 * 3) 重写开始时 LSN 的高 32 位
 * 4) 重写开始时 LSN 的低 32 位
 * 5) 为其做映射的 xid
 * 6) 执行该映射的事务的 xid
 * ---
 */
#define LOGICAL_REWRITE_FORMAT "map-%x-%x-%X_%X-%x-%x"
extern void CheckPointLogicalRewriteHeap(void);

#endif							/* REWRITE_HEAP_H */
