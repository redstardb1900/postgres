/*-------------------------------------------------------------------------
 *
 * memutils.h
 *	  本文件含内存分配实用函数的声明。这些函数
 *	  尚不足以广泛到放进 utils/palloc.h，但仍属
 *	  内存管理子系统 API 的一部分。
 *	见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMUTILS_H
#define MEMUTILS_H

#include "nodes/memnodes.h"


/*
 * MaxAllocSize, MaxAllocHugeSize
 *		分配大小的准任意上限。
 *
 * 注:
 *		不保证更小的分配一定成功，但更大的请求会
 *		被直接拒绝。
 *
 * palloc() 强制 MaxAllocSize，选为与 TOAST 下
 * varlena 对象的限制大小对应。见 postgres.h 中
 * VARSIZE_4B() 及相关宏。许多数据类型假定任何
 * 可分配大小都能用 varlena 头表示。该限制也使
 * 调用方可用 int 变量做分配下标或长度。小心避开
 * 这些风险的调用方可用 MemoryContextAllocHuge()
 * 访问更高上限。两上限都允许代码假定可把分配
 * 大小加倍而无溢出。
 */
#define MaxAllocSize	((Size) 0x3fffffff) /* 1 GB - 1 */

#define AllocSizeIsValid(size)	((Size) (size) <= MaxAllocSize)

/* 必须小于 SIZE_MAX */
#define MaxAllocHugeSize	(SIZE_MAX / 2)

#define InvalidAllocSize	SIZE_MAX

#define AllocHugeSizeIsValid(size)	((Size) (size) <= MaxAllocHugeSize)


/*
 * 标准顶层内存上下文。
 *
 * 仅 TopMemoryContext/ErrorContext 由 MemoryContextInit()
 * 自身初始化。
 */
extern PGDLLIMPORT MemoryContext TopMemoryContext;
extern PGDLLIMPORT MemoryContext ErrorContext;
extern PGDLLIMPORT MemoryContext PostmasterContext;
extern PGDLLIMPORT MemoryContext CacheMemoryContext;
extern PGDLLIMPORT MemoryContext MessageContext;
extern PGDLLIMPORT MemoryContext TopTransactionContext;
extern PGDLLIMPORT MemoryContext CurTransactionContext;

/* 到活动 portal 内存上下文的瞬时链接： */
extern PGDLLIMPORT MemoryContext PortalContext;

/* 向后兼容宏 */
#define MemoryContextResetAndDeleteChildren(ctx) MemoryContextReset(ctx)


/*
 * mcxt.c 中与内存上下文类型无关的函数
 */
extern void MemoryContextInit(void);
extern void MemoryContextReset(MemoryContext context);
extern void MemoryContextDelete(MemoryContext context);
extern void MemoryContextResetOnly(MemoryContext context);
extern void MemoryContextResetChildren(MemoryContext context);
extern void MemoryContextDeleteChildren(MemoryContext context);
extern void MemoryContextSetIdentifier(MemoryContext context, const char *id);
extern void MemoryContextSetParent(MemoryContext context,
								   MemoryContext new_parent);
extern MemoryContext GetMemoryChunkContext(void *pointer);
extern Size GetMemoryChunkSpace(void *pointer);
extern MemoryContext MemoryContextGetParent(MemoryContext context);
extern bool MemoryContextIsEmpty(MemoryContext context);
extern Size MemoryContextMemAllocated(MemoryContext context, bool recurse);
extern void MemoryContextStats(MemoryContext context);
extern void MemoryContextStatsDetail(MemoryContext context, int max_children,
									 bool print_to_stderr);
extern void MemoryContextAllowInCriticalSection(MemoryContext context,
												bool allow);

#ifdef MEMORY_CONTEXT_CHECKING
extern void MemoryContextCheck(MemoryContext context);
#endif

/* 拷贝并赋值上下文 ID 的便捷宏……但注意双重求值 */
#define MemoryContextCopyAndSetIdentifier(cxt, id) \
	MemoryContextSetIdentifier(cxt, MemoryContextStrdup(cxt, id))

extern void HandleLogMemoryContextInterrupt(void);
extern void ProcessLogMemoryContextInterrupt(void);

/*
 * 特定内存上下文类型的函数
 */

/* aset.c */
extern MemoryContext AllocSetContextCreateInternal(MemoryContext parent,
												   const char *name,
												   Size minContextSize,
												   Size initBlockSize,
												   Size maxBlockSize);

/*
 * 此包装宏用于检查把非常量字符串用作上下文名；
 * 那已不再支持。（若要提供可变标识符，用
 * MemoryContextSetIdentifier。）
 */
#ifdef HAVE__BUILTIN_CONSTANT_P
#define AllocSetContextCreate(parent, name, ...) \
	(StaticAssertExpr(__builtin_constant_p(name), \
					  "memory context names must be constant strings"), \
	 AllocSetContextCreateInternal(parent, name, __VA_ARGS__))
#else
#define AllocSetContextCreate \
	AllocSetContextCreateInternal
#endif

/* slab.c */
extern MemoryContext SlabContextCreate(MemoryContext parent,
									   const char *name,
									   Size blockSize,
									   Size chunkSize);

/* generation.c */
extern MemoryContext GenerationContextCreate(MemoryContext parent,
											 const char *name,
											 Size minContextSize,
											 Size initBlockSize,
											 Size maxBlockSize);

/*
 * 推荐的默认分配参数，适于可能容纳较多数据的
 * "普通"上下文。
 */
#define ALLOCSET_DEFAULT_MINSIZE   0
#define ALLOCSET_DEFAULT_INITSIZE  (8 * 1024)
#define ALLOCSET_DEFAULT_MAXSIZE   (8 * 1024 * 1024)
#define ALLOCSET_DEFAULT_SIZES \
	ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE

/*
 * 推荐给从不预期含很多数据的"小"上下文的分配
 * 参数（例如容纳查询计划的上下文）。
 */
#define ALLOCSET_SMALL_MINSIZE	 0
#define ALLOCSET_SMALL_INITSIZE  (1 * 1024)
#define ALLOCSET_SMALL_MAXSIZE	 (8 * 1024)
#define ALLOCSET_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE

/*
 * 推荐给应从小开始、但有时可能变大的上下文的
 * 分配参数。
 */
#define ALLOCSET_START_SMALL_SIZES \
	ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE


/*
 * AllocSet 上下文中，请求必定单独分配（从而有
 * 恒定分配开销）的阈值。很少调用方关心，但
 * tuplesort/tuplestore 需要知道。
 * 见上。
 */
#define ALLOCSET_SEPARATE_THRESHOLD  8192

#define SLAB_DEFAULT_BLOCK_SIZE		(8 * 1024)
#define SLAB_LARGE_BLOCK_SIZE		(8 * 1024 * 1024)

#endif							/* MEMUTILS_H */
