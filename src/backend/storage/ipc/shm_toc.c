/*-------------------------------------------------------------------------
 *
 * shm_toc.c
 *	  共享内存段目录
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/ipc/shm_toc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/atomics.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"

typedef struct shm_toc_entry
{
	uint64		key;			/* 任意标识符 */
	Size		offset;			/* 自 TOC 起点起的偏移量，以字节计 */
} shm_toc_entry;

struct shm_toc
{
	uint64		toc_magic;		/* 标识本 TOC 的魔数 */
	slock_t		toc_mutex;		/* 用于互斥的自旋锁 */
	Size		toc_total_bytes;	/* 本 TOC 管理的字节数 */
	Size		toc_allocated_bytes;	/* 其中已分配的字节数 */
	uint32		toc_nentry;		/* TOC 中的条目数 */
	shm_toc_entry toc_entry[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * 用目录初始化一片共享内存区域。
 */
shm_toc *
shm_toc_create(uint64 magic, void *address, Size nbytes)
{
	shm_toc    *toc = (shm_toc *) address;

	Assert(nbytes > offsetof(shm_toc, toc_entry));
	toc->toc_magic = magic;
	SpinLockInit(&toc->toc_mutex);

	/*
	 * shm_toc_allocate() 中的对齐代码假定起始值
	 * 已按缓冲区对齐。
	 */
	toc->toc_total_bytes = BUFFERALIGN_DOWN(nbytes);
	toc->toc_allocated_bytes = 0;
	toc->toc_nentry = 0;

	return toc;
}

/*
 * 挂接到已有目录。若目标地址处的魔数与预期不符，
 * 返回 NULL。
 */
shm_toc *
shm_toc_attach(uint64 magic, void *address)
{
	shm_toc    *toc = (shm_toc *) address;

	if (toc->toc_magic != magic)
		return NULL;

	Assert(toc->toc_total_bytes >= toc->toc_allocated_bytes);
	Assert(toc->toc_total_bytes > offsetof(shm_toc, toc_entry));

	return toc;
}

/*
 * 从由目录管理的段中分配共享内存。
 *
 * 这不是一个完备的分配器；没有办法释放内存。
 * 它只是把单个物理共享内存段划分成可用于不同目的的
 * 逻辑块。
 *
 * 我们从段的末尾向前分配，这样 TOC 条目可以从段的
 * 起点向后增长。
 */
void *
shm_toc_allocate(shm_toc *toc, Size nbytes)
{
	volatile shm_toc *vtoc = toc;
	Size		total_bytes;
	Size		allocated_bytes;
	Size		nentry;
	Size		toc_bytes;

	/*
	 * 确保请求对齐良好。XXX：MAXALIGN 不够，
	 * 因为原子操作可能需要更宽的对齐。我们没有
	 * 一个能使原子操作安全的最小对齐的正式定义，但
	 * BUFFERALIGN 应当够用。
	 */
	nbytes = BUFFERALIGN(nbytes);

	SpinLockAcquire(&toc->toc_mutex);

	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	toc_bytes = offsetof(shm_toc, toc_entry) + nentry * sizeof(shm_toc_entry)
		+ allocated_bytes;

	/* 检查内存耗尽与溢出。 */
	if (toc_bytes + nbytes > total_bytes || toc_bytes + nbytes < toc_bytes)
	{
		SpinLockRelease(&toc->toc_mutex);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	}
	vtoc->toc_allocated_bytes += nbytes;

	SpinLockRelease(&toc->toc_mutex);

	return ((char *) toc) + (total_bytes - allocated_bytes - nbytes);
}

/*
 * 返回仍然可以分配的字节数。
 */
Size
shm_toc_freespace(shm_toc *toc)
{
	volatile shm_toc *vtoc = toc;
	Size		total_bytes;
	Size		allocated_bytes;
	Size		nentry;
	Size		toc_bytes;

	SpinLockAcquire(&toc->toc_mutex);
	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	SpinLockRelease(&toc->toc_mutex);

	toc_bytes = offsetof(shm_toc, toc_entry) + nentry * sizeof(shm_toc_entry);
	Assert(allocated_bytes + BUFFERALIGN(toc_bytes) <= total_bytes);
	return total_bytes - (allocated_bytes + BUFFERALIGN(toc_bytes));
}

/*
 * 插入一条 TOC 条目。
 *
 * 这里的想法是：建立共享内存段的进程用本函数
 * 登记段内数据结构的地址。每个数据结构用一个
 * 64 位键标识，该键假定是众所周知或可发现的
 * 整数。访问该共享内存段的其他进程可以把同一个键
 * 传给 shm_toc_lookup()，以发现这些数据结构的
 * 地址。
 *
 * 由于共享内存段在不同后端中可能映射到不同地址，
 * 我们存储相对指针而不是绝对指针。
 *
 * 这在键很多时扩展性不好。希望没有必要；
 * 若事实证明有必要，我们可能得在这里提供
 * 更精巧的数据结构。但这里真正的用意只是让
 * 映射动态共享内存的人能够找到完成引导所需的
 * 最少指针。若你把大量东西放进 TOC，
 * 那就是用错了。
 */
void
shm_toc_insert(shm_toc *toc, uint64 key, void *address)
{
	volatile shm_toc *vtoc = toc;
	Size		total_bytes;
	Size		allocated_bytes;
	Size		nentry;
	Size		toc_bytes;
	Size		offset;

	/* 把指针改成相对形式。 */
	Assert(address > (void *) toc);
	offset = ((char *) address) - (char *) toc;

	SpinLockAcquire(&toc->toc_mutex);

	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	toc_bytes = offsetof(shm_toc, toc_entry) + nentry * sizeof(shm_toc_entry)
		+ allocated_bytes;

	/* 检查内存耗尽与溢出。 */
	if (toc_bytes + sizeof(shm_toc_entry) > total_bytes ||
		toc_bytes + sizeof(shm_toc_entry) < toc_bytes ||
		nentry >= PG_UINT32_MAX)
	{
		SpinLockRelease(&toc->toc_mutex);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	}

	Assert(offset < total_bytes);
	vtoc->toc_entry[nentry].key = key;
	vtoc->toc_entry[nentry].offset = offset;

	/*
	 * 在填好条目之后、更新条目数之前
	 * 放置写屏障，这样不加锁读取 TOC
	 * 也是安全的。
	 */
	pg_write_barrier();

	vtoc->toc_nentry++;

	SpinLockRelease(&toc->toc_mutex);
}

/*
 * 查找一条 TOC 条目。
 *
 * 若找不到该键，noError 为真时返回 NULL，否则
 * 抛出 elog(ERROR)。
 *
 * 与本文件中的其他函数不同，本操作不加锁；
 * 它只用屏障。即使加锁，对并发的损害大概也不大，
 * 但一组工作进程很可能会在差不多同一时间各自从
 * 同一个 TOC 读取一系列条目，因此避免加锁
 * 似乎有些价值。
 */
void *
shm_toc_lookup(shm_toc *toc, uint64 key, bool noError)
{
	uint32		nentry;
	uint32		i;

	/*
	 * 在检查任何条目之前先读取条目数。我们假定
	 * 读取 uint32 是原子的。
	 */
	nentry = toc->toc_nentry;
	pg_read_barrier();

	/* 现在搜索匹配的条目。 */
	for (i = 0; i < nentry; ++i)
	{
		if (toc->toc_entry[i].key == key)
			return ((char *) toc) + toc->toc_entry[i].offset;
	}

	/* 没有找到匹配的条目。 */
	if (!noError)
		elog(ERROR, "could not find key " UINT64_FORMAT " in shm TOC at %p",
			 key, toc);
	return NULL;
}

/*
 * 估计存储一个 TOC 及其依赖的数据结构
 * 需要多少共享内存。
 */
Size
shm_toc_estimate(shm_toc_estimator *e)
{
	Size		sz;

	sz = offsetof(shm_toc, toc_entry);
	sz = add_size(sz, mul_size(e->number_of_keys, sizeof(shm_toc_entry)));
	sz = add_size(sz, e->space_for_chunks);

	return BUFFERALIGN(sz);
}
