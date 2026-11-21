/*-------------------------------------------------------------------------
 *
 * memnodes.h
 *	POSTGRES 内存上下文节点定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/memnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMNODES_H
#define MEMNODES_H

#include "nodes/nodes.h"

/*
 * MemoryContextCounters
 *	MemoryContextStats 收集用的汇总状态。
 *
 * 本结构中的计数器偏向 AllocSet；若将来有基于根本不同
 * 思路的上下文类型,可能需要更多或不同的计数器。届时
 * 一种 API 设想是只打印非零计数器,但目前仍按 AllocSet
 * 历史上使用的格式汇总。
 * 见上。
 */
typedef struct MemoryContextCounters
{
	Size		nblocks;		/* malloc 块总数 */
	Size		freechunks;		/* 空闲块总数 */
	Size		totalspace;		/* 向 malloc 请求的总字节 */
	Size		freespace;		/* totalspace 中未用部分 */
} MemoryContextCounters;

/*
 * MemoryContext
 *	发生内存分配的逻辑上下文。
 *
 * MemoryContext 本身是可有多种实现的抽象类型。
 * 见下。
 * MemoryContextMethods 中的函数指针定义一种具体实现
 * ——用 C++ 术语即虚函数表。
 * 见上。
 *
 * 作为内存上下文实际实现的节点类型必须以与
 * MemoryContextData 相同的字段开头。
 *
 * 注意:由于历史原因,typedef MemoryContext 是指向
 * 上下文结构的指针,而非结构类型本身。
 */

typedef void (*MemoryStatsPrintFunc) (MemoryContext context, void *passthru,
									  const char *stats_string,
									  bool print_to_stderr);

typedef struct MemoryContextMethods
{
	void	   *(*alloc) (MemoryContext context, Size size);
	/* call this free_p in case someone #define's free() */
	void		(*free_p) (void *pointer);
	void	   *(*realloc) (void *pointer, Size size);
	void		(*reset) (MemoryContext context);
	void		(*delete_context) (MemoryContext context);
	MemoryContext (*get_chunk_context) (void *pointer);
	Size		(*get_chunk_space) (void *pointer);
	bool		(*is_empty) (MemoryContext context);
	void		(*stats) (MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
	void		(*check) (MemoryContext context);
#endif
} MemoryContextMethods;


typedef struct MemoryContextData
{
	pg_node_attr(abstract)		/* 无此类型的节点 */

	NodeTag		type;			/* 标识上下文的确切种类 */
	/* 这两字段放这里以减少对齐浪费: */
	bool		isReset;		/* T=自上次复位后未再分配 */
	bool		allowInCritSection; /* 允许在临界区 palloc */
	Size		mem_allocated;	/* 跟踪为本上下文分配的内存 */
	const MemoryContextMethods *methods;	/* 虚函数表 */
	MemoryContext parent;		/* 无父则为 NULL(顶层上下文) */
	MemoryContext firstchild;	/* 子链表头 */
	MemoryContext prevchild;	/* 同父的前一子 */
	MemoryContext nextchild;	/* 同父的下一子 */
	const char *name;			/* 上下文名(仅调试) */
	const char *ident;			/* 上下文 ID(若有,仅调试) */
	MemoryContextCallback *reset_cbs;	/* 复位/删除回调列表 */
} MemoryContextData;

/* utils/palloc.h contains typedef struct MemoryContextData *MemoryContext */


/*
 * MemoryContextIsValid
 *	内存上下文有效则为真。
 *
 * 把新上下文类型加入本宏接受的集合。
 */
#define MemoryContextIsValid(context) \
	((context) != NULL && \
	 (IsA((context), AllocSetContext) || \
	  IsA((context), SlabContext) || \
	  IsA((context), GenerationContext)))

#endif							/* MEMNODES_H */
