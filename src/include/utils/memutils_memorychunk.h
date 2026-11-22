/*-------------------------------------------------------------------------
 *
 * memutils_memorychunk.h
 *	  在此定义结构体 MemoryChunk，供 MemoryContext
 *	  实现用作其所分配内存块的头。
 *
 * MemoryChunk 是轻量头，MemoryContext 可用它保存
 * 回指该块所在 block 的引用，另有 30 位存放
 * 其他值，例如所分配块的大小。
 * 见上。
 *
 * 虽各 MemoryContexts 都用 MemoryChunks，未来实现
 * 也可自定块头方案。唯一要求是头以 8 字节值
 * 结尾，且其最低 3 位设为该上下文的
 * MemoryContextMethodID。
 * 见上。
 *
 * 默认 MemoryChunk 为 8 字节；但当定义了
 * MEMORY_CONTEXT_CHECKING 时因额外 requested_size
 * 字段变为 16 字节。MemoryContext 可随意使用该
 * 字段，但意在用于仅在 MEMORY_CONTEXT_CHECKING
 * 构建中做的额外检查。
 *
 * MemoryChunk 含名为 'hdrmask' 的 uint64 字段。该
 * 字段编码 4 段信息。自 'hdrmask' 最低有效位起，
 * 位空间保留如下：
 *
 * 1.	3 位表示 MemoryContextMethodID，由
 *		MEMORY_CONTEXT_METHODID_MASK
 * 2.	1 位表示"外部"块（见下）
 * 3.	30 位留给 MemoryContext 任意使用。多数
 *		MemoryContext 想在此存放块大小。
 *		见上。
 * 4.	30 位：须从块地址减去多少字节才得到
 *		存放该块的 block 地址。
 *
 * 有时（例如分配变大）上面字段 3、4 不够存
 * 该块所需之值。此时 MemoryContext 可调用
 * MemoryChunkSetHdrMaskExternal() 将块标为
 * 外部。之后字段 3、4 不可再供
 * MemoryContext 使用，须由其自定取得
 * block 引用的方法。
 * 见上。
 *
 * 接口：
 *
 * MemoryChunkSetHdrMask:
 *		用于设置非外部 MemoryChunk。
 *
 * MemoryChunkSetHdrMaskExternal:
 *		用于设置外部管理的 MemoryChunk。
 *
 * MemoryChunkIsExternal:
 *		判断给定 MemoryChunk 是否外部管理，即
 *		是否对其调用过 MemoryChunkSetHdrMaskExternal()。
 *
 * MemoryChunkGetValue:
 *		对非外部块，返回调用 MemoryChunkSetHdrMask()
 *		时所存的 30 位值。
 *
 * MemoryChunkGetBlock:
 *		对非外部块，返回调用 MemoryChunkSetHdrMask()
 *		时所存的 block 指针。
 *
 * 亦导出：
 *		MEMORYCHUNK_MAX_VALUE
 *		MEMORYCHUNK_MAX_BLOCKOFFSET
 *		PointerGetMemoryChunk
 *		MemoryChunkGetPointer
 *
 * Portions Copyright (c) 2022-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/memutils_memorychunk.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MEMUTILS_MEMORYCHUNK_H
#define MEMUTILS_MEMORYCHUNK_H

#include "utils/memutils_internal.h"

 /*
  * MemoryContexts 可在 value 字段存放的最大允许值。
  * 须为 2 的幂减 1。
  */
#define MEMORYCHUNK_MAX_VALUE			UINT64CONST(0x3FFFFFFF)

/*
 * MemoryChunk 相对存放它的 block 的最大字节偏移。
 * 须为 2 的幂减 1。
 */
#define MEMORYCHUNK_MAX_BLOCKOFFSET		UINT64CONST(0x3FFFFFFF)

/*  定义 hdrmask 各段的最低有效基 0 位  */
#define MEMORYCHUNK_EXTERNAL_BASEBIT	MEMORY_CONTEXT_METHODID_BITS
#define MEMORYCHUNK_VALUE_BASEBIT		(MEMORYCHUNK_EXTERNAL_BASEBIT + 1)
#define MEMORYCHUNK_BLOCKOFFSET_BASEBIT	(MEMORYCHUNK_VALUE_BASEBIT + 30)

/*
 * 存入外部块空闲位的魔数。须屏蔽用于存放
 * MemoryContextMethodID 与外部位的那些位。
 * 见上。
 */
#define MEMORYCHUNK_MAGIC		(UINT64CONST(0xB1A8DB858EB6EFBA) >> \
								 MEMORYCHUNK_VALUE_BASEBIT << \
								 MEMORYCHUNK_VALUE_BASEBIT)

typedef struct MemoryChunk
{
#ifdef MEMORY_CONTEXT_CHECKING
	Size		requested_size;
#endif

	/*  存放块细节的位域  */
	uint64		hdrmask;		/*  须居末  */
} MemoryChunk;

/*  由指针取得 MemoryChunk  */
#define PointerGetMemoryChunk(p) \
	((MemoryChunk *) ((char *) (p) - sizeof(MemoryChunk)))
/*  由 MemoryChunk 取得指针  */
#define MemoryChunkGetPointer(c) \
	((void *) ((char *) (c) + sizeof(MemoryChunk)))

/*  私有宏，使下方内联函数更简洁  */
#define HdrMaskIsExternal(hdrmask) \
	((hdrmask) & (((uint64) 1) << MEMORYCHUNK_EXTERNAL_BASEBIT))
#define HdrMaskGetValue(hdrmask) \
	(((hdrmask) >> MEMORYCHUNK_VALUE_BASEBIT) & MEMORYCHUNK_MAX_VALUE)

/*
 * 此处应已用尽全部位，故编译器多半会优化掉
 * & MEMORYCHUNK_MAX_BLOCKOFFSET。
 */
#define HdrMaskBlockOffset(hdrmask) \
	(((hdrmask) >> MEMORYCHUNK_BLOCKOFFSET_BASEBIT) & MEMORYCHUNK_MAX_BLOCKOFFSET)

/*  仅外部块：检查魔数是否匹配  */
#define HdrMaskCheckMagic(hdrmask) \
	(MEMORYCHUNK_MAGIC == \
	 ((hdrmask) >> MEMORYCHUNK_VALUE_BASEBIT << MEMORYCHUNK_VALUE_BASEBIT))
/*
 * MemoryChunkSetHdrMask
 *		把给定 block、chunk_size 与 methodid 存入
 *		MemoryChunk.
 *
 * block 与 chunk 间的字节数必须 <=
 * MEMORYCHUNK_MAX_BLOCKOFFSET.
 * value 必须 <= MEMORYCHUNK_MAX_VALUE。
 */
static inline void
MemoryChunkSetHdrMask(MemoryChunk *chunk, void *block,
					  Size value, MemoryContextMethodID methodid)
{
	Size		blockoffset = (char *) chunk - (char *) block;

	Assert((char *) chunk >= (char *) block);
	Assert(blockoffset <= MEMORYCHUNK_MAX_BLOCKOFFSET);
	Assert(value <= MEMORYCHUNK_MAX_VALUE);
	Assert((int) methodid <= MEMORY_CONTEXT_METHODID_MASK);

	chunk->hdrmask = (((uint64) blockoffset) << MEMORYCHUNK_BLOCKOFFSET_BASEBIT) |
		(((uint64) value) << MEMORYCHUNK_VALUE_BASEBIT) |
		methodid;
}

/*
 * MemoryChunkSetHdrMaskExternal
 *		将 chunk 标为外部管理块。此处只记录
 *		MemoryContextMethodID 并置外部块位。
 */
static inline void
MemoryChunkSetHdrMaskExternal(MemoryChunk *chunk,
							  MemoryContextMethodID methodid)
{
	Assert((int) methodid <= MEMORY_CONTEXT_METHODID_MASK);

	chunk->hdrmask = MEMORYCHUNK_MAGIC | (((uint64) 1) << MEMORYCHUNK_EXTERNAL_BASEBIT) |
		methodid;
}

/*
 * MemoryChunkIsExternal
 *		若 chunk 标为外部则返回 true。
 */
static inline bool
MemoryChunkIsExternal(MemoryChunk *chunk)
{
	/*
	 * 外部块应始终在 hdrmask 高位存放
	 * MEMORYCHUNK_MAGIC，检查未被踩踏。
	 */
	Assert(!HdrMaskIsExternal(chunk->hdrmask) ||
		   HdrMaskCheckMagic(chunk->hdrmask));

	return HdrMaskIsExternal(chunk->hdrmask);
}

/*
 * MemoryChunkGetValue
 *		对非外部块，返回 MemoryChunkSetHdrMask
 *		所设 value 字段。
 */
static inline Size
MemoryChunkGetValue(MemoryChunk *chunk)
{
	Assert(!HdrMaskIsExternal(chunk->hdrmask));

	return HdrMaskGetValue(chunk->hdrmask);
}

/*
 * MemoryChunkGetBlock
 *		对非外部块，返回 MemoryChunkSetHdrMask
 *		所设 block 指针。
 */
static inline void *
MemoryChunkGetBlock(MemoryChunk *chunk)
{
	Assert(!HdrMaskIsExternal(chunk->hdrmask));

	return (void *) ((char *) chunk - HdrMaskBlockOffset(chunk->hdrmask));
}

/*  清理全部内部定义  */
#undef MEMORYCHUNK_EXTERNAL_BASEBIT
#undef MEMORYCHUNK_VALUE_BASEBIT
#undef MEMORYCHUNK_BLOCKOFFSET_BASEBIT
#undef MEMORYCHUNK_MAGIC
#undef HdrMaskIsExternal
#undef HdrMaskGetValue
#undef HdrMaskBlockOffset
#undef HdrMaskCheckMagic

#endif							/* MEMUTILS_MEMORYCHUNK_H */
