/*-------------------------------------------------------------------------
 *
 * alignedalloc.c
 *	  用于实现 palloc_aligned 的分配器函数
 *
 * 这并非一个完备的内存上下文类型，因为无法
 * 创建此类型的 MemoryContext。这里的代码只是为了让
 * pfree()、repalloc() 等操作正确作用于由
 * palloc_aligned() 分配的内存区块。
 *
 * Portions Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/alignedalloc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memdebug.h"
#include "utils/memutils_memorychunk.h"

/*
 * AlignedAllocFree
*		释放已分配的内存；内存从其所属上下文移除。
*/
void
AlignedAllocFree(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;

	VALGRIND_MAKE_MEM_DEFINED(chunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(chunk));

	/* 取得最初的（未对齐）已分配指针 */
	unaligned = MemoryChunkGetBlock(chunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* 检测是否有人涂写区块内的未用空间 */
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 GetMemoryChunkContext(unaligned)->name, chunk);
#endif

	pfree(unaligned);
}

/*
 * AlignedAllocRealloc
 *		修改区块的已分配大小，返回的指针可能不同，
 *		其内存地址对齐边界与最初申请的相同。'pointer'
 *		的内容将被复制进返回的指针，直至 'size'。多余
 *		的内存将不做初始化。
 *
 */
void *
AlignedAllocRealloc(void *pointer, Size size)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	Size		alignto;
	void	   *unaligned;
	MemoryContext ctx;
	Size		old_size;
	void	   *newptr;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	alignto = MemoryChunkGetValue(redirchunk);
	unaligned = MemoryChunkGetBlock(redirchunk);

	/* 健全性检查这是 2 的幂 */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * 确定最初分配的大小。我们无法把它确定得
	 * 分精确，因为 GetMemoryChunkSpace() 返回的
	 * 是分配所耗的总空间，对 aset 这类上下文
	 * 还包括向上取整到 2 的幂。不过，该值只是
	 * 用来把旧数据 memcpy() 进新分配，所以我们
	 * 只需关注不越过最初分配内存的末尾读取。
	 * 缺点是可能复制多于所需的字节，但那只是
	 * 徒劳而已。我们可以安全地减去为对齐指针
	 * 而额外申请的字节。还必须减去未对齐指针
	 * 的 MemoryChunk 所占的空间，因为
	 * GetMemoryChunkSpace 本应已把它计入。这假定一切
	 * 上下文类型都以 MemoryChunk 为区块头部。
	 */
	old_size = GetMemoryChunkSpace(unaligned) -
		PallocAlignedExtraBytes(alignto) - sizeof(MemoryChunk);

#ifdef MEMORY_CONTEXT_CHECKING
	/* 检查 GetMemoryChunkSpace 返回值是否合乎实际 */
	Assert(old_size >= redirchunk->requested_size);
#endif

	ctx = GetMemoryChunkContext(unaligned);
	newptr = MemoryContextAllocAligned(ctx, size, alignto, 0);

	/*
	 * 我们可能 memcpy 到越过最初申请大小末尾的地方，
	 * 因此须把整个分配标记为已定义。
	 */
	VALGRIND_MAKE_MEM_DEFINED(pointer, old_size);
	memcpy(newptr, pointer, Min(size, old_size));
	pfree(unaligned);

	return newptr;
}

/*
 * AlignedAllocGetChunkContext
 *		返回 'pointer' 所属的 MemoryContext。
 */
MemoryContext
AlignedAllocGetChunkContext(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	MemoryContext cxt;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	Assert(!MemoryChunkIsExternal(redirchunk));

	cxt = GetMemoryChunkContext(MemoryChunkGetBlock(redirchunk));

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return cxt;
}

/*
 * AlignedAllocGetChunkSpace
 *		给定一个已分配的区块，确定其占用的总空间
 *		（包括全部内存分配开销）。
 */
Size
AlignedAllocGetChunkSpace(void *pointer)
{
	MemoryChunk *redirchunk = PointerGetMemoryChunk(pointer);
	void	   *unaligned;
	Size		space;

	VALGRIND_MAKE_MEM_DEFINED(redirchunk, sizeof(MemoryChunk));

	unaligned = MemoryChunkGetBlock(redirchunk);
	space = GetMemoryChunkSpace(unaligned);

	VALGRIND_MAKE_MEM_NOACCESS(redirchunk, sizeof(MemoryChunk));

	return space;
}
