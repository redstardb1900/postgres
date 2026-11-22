/*-------------------------------------------------------------------------
 *
 * memutils_internal.h
 *	  供内部使用的内存分配实用函数声明。
 *	见上。
 *
 *
 * Portions Copyright (c) 2022-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils_internal.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MEMUTILS_INTERNAL_H
#define MEMUTILS_INTERNAL_H

#include "utils/memutils.h"

/* 这些函数为 AllocSet 上下文实现 MemoryContext API。 */
extern void *AllocSetAlloc(MemoryContext context, Size size);
extern void AllocSetFree(void *pointer);
extern void *AllocSetRealloc(void *pointer, Size size);
extern void AllocSetReset(MemoryContext context);
extern void AllocSetDelete(MemoryContext context);
extern MemoryContext AllocSetGetChunkContext(void *pointer);
extern Size AllocSetGetChunkSpace(void *pointer);
extern bool AllocSetIsEmpty(MemoryContext context);
extern void AllocSetStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void AllocSetCheck(MemoryContext context);
#endif

/* 这些函数为 Generation 上下文实现 MemoryContext API。 */
extern void *GenerationAlloc(MemoryContext context, Size size);
extern void GenerationFree(void *pointer);
extern void *GenerationRealloc(void *pointer, Size size);
extern void GenerationReset(MemoryContext context);
extern void GenerationDelete(MemoryContext context);
extern MemoryContext GenerationGetChunkContext(void *pointer);
extern Size GenerationGetChunkSpace(void *pointer);
extern bool GenerationIsEmpty(MemoryContext context);
extern void GenerationStats(MemoryContext context,
							MemoryStatsPrintFunc printfunc, void *passthru,
							MemoryContextCounters *totals,
							bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void GenerationCheck(MemoryContext context);
#endif


/* 这些函数为 Slab 上下文实现 MemoryContext API。 */
extern void *SlabAlloc(MemoryContext context, Size size);
extern void SlabFree(void *pointer);
extern void *SlabRealloc(void *pointer, Size size);
extern void SlabReset(MemoryContext context);
extern void SlabDelete(MemoryContext context);
extern MemoryContext SlabGetChunkContext(void *pointer);
extern Size SlabGetChunkSpace(void *pointer);
extern bool SlabIsEmpty(MemoryContext context);
extern void SlabStats(MemoryContext context,
					  MemoryStatsPrintFunc printfunc, void *passthru,
					  MemoryContextCounters *totals,
					  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
extern void SlabCheck(MemoryContext context);
#endif

/*
 * 这些函数支持 palloc_aligned() 的实现，并非完整
 * MemoryContext 类型的一部分。
 */
extern void AlignedAllocFree(void *pointer);
extern void *AlignedAllocRealloc(void *pointer, Size size);
extern MemoryContext AlignedAllocGetChunkContext(void *pointer);
extern Size AlignedAllocGetChunkSpace(void *pointer);

/*
 * 为确保能把指针对齐到 alignto，额外需请求多少
 * 字节。因 palloc 指针已按 MAXIMUM_ALIGNOF 对齐，
 * 可减去该量。还须确保有足够空间放重定向
 * MemoryChunk。
 */
#define PallocAlignedExtraBytes(alignto) \
	((alignto) + (sizeof(MemoryChunk) - MAXIMUM_ALIGNOF))

/*
 * MemoryContextMethodID
 *		每种 MemoryContext 实现的唯一标识，表示
 *		mcxt_methods[] 数组下标。见 mcxt.c。
 *
 * 为稳健检错，确保 MemoryContextMethodID 对
 * MEMORY_CONTEXT_METHODID_MASK 的每种可能位模式
 * 都有值，并在 mcxt_methods[] 中为未用 ID 做哑项。
 * 我们也尽量避免把垃圾数据中常见、或会误匹配
 * malloc 而非 palloc 块的位模式用作有效 ID。
 * （多数 malloc 实现无法区分，但 glibc 恰把标志
 * 位存在我们放 MemoryContextMethodID 的同一位置，
 * 故对其可预测可能值。）
 */
typedef enum MemoryContextMethodID
{
	MCTX_UNUSED1_ID,			/* 000 见于从未用过的内存 */
	MCTX_UNUSED2_ID,			/* glibc malloc'd chunks usually match 001 */
	MCTX_UNUSED3_ID,			/* glibc malloc'd chunks > 128kB match 010 */
	MCTX_ASET_ID,
	MCTX_GENERATION_ID,
	MCTX_SLAB_ID,
	MCTX_ALIGNED_REDIRECT_ID,
	MCTX_UNUSED4_ID				/* 111 见于 wipe_mem 内存 */
} MemoryContextMethodID;

/*
 * 8 字节内存块头可用来编码 MemoryContextMethodID
 * 的位数。
 */
#define MEMORY_CONTEXT_METHODID_BITS 3
#define MEMORY_CONTEXT_METHODID_MASK \
	((((uint64) 1) << MEMORY_CONTEXT_METHODID_BITS) - 1)

/*
 * 本例程处理内存上下文创建中与类型无关部分。
 * 意在由特定类型的创建例程调用，别处勿用。
 * 见上。
 */
extern void MemoryContextCreate(MemoryContext node,
								NodeTag tag,
								MemoryContextMethodID method_id,
								MemoryContext parent,
								const char *name);

#endif							/* MEMUTILS_INTERNAL_H */
