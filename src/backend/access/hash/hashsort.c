/*-------------------------------------------------------------------------
 *
 * hashsort.c
 *		为插入新建的 hash 索引而对元组排序。
 *
 * 构建非常大的 hash 索引时，我们按桶号预先排序元组，
 * 以改善对索引的访问局部性，从而避免颠簸。我们使用
 * tuplesort.c 把给定的索引元组排成顺序。
 *
 * 注意：若低估了表中的行数，索引构建期间
 * 可能发生桶分裂。那时，对每个可能的掩码后
 * 哈希码值，我们会插入到两个或更多桶中。
 * 不过这不是大问题，因为我们仍会有充足的
 * 访问局部性。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "utils/tuplesort.h"


/*
 * 假脱机/排序阶段的状态记录。
 */
struct HSpool
{
	Tuplesortstate *sortstate;	/* tuplesort.c 的状态数据 */
	Relation	index;

	/*
	 * 我们先按哈希键所属的桶排序，再按哈希值本身
	 * 排序，以优化向 hash 页的插入。下面的掩码在
	 * _hash_hashkey2bucket 中用来确定给定哈希键
	 * 所属的桶。
	 */
	uint32		high_mask;
	uint32		low_mask;
	uint32		max_buckets;
};


/*
 * 创建并初始化假脱机结构
 */
HSpool *
_h_spoolinit(Relation heap, Relation index, uint32 num_buckets)
{
	HSpool	   *hspool = (HSpool *) palloc0(sizeof(HSpool));

	hspool->index = index;

	/*
	 * 确定哈希码值的位掩码。由于索引中目前有
	 * num_buckets 个桶，合适的掩码可按如下方式
	 * 计算。
	 *
	 * 注意：此哈希掩码计算应与
	 * _hash_init_metabuffer 中的类似计算保持同步。
	 */
	hspool->high_mask = pg_nextpower2_32(num_buckets + 1) - 1;
	hspool->low_mask = (hspool->high_mask >> 1);
	hspool->max_buckets = num_buckets - 1;

	/*
	 * 排序区按 maintenance_work_mem 而非 work_mem
	 * 设定大小，以加快索引创建。这应当没问题，因为
	 * 单个后端无法并行运行多个索引创建。
	 */
	hspool->sortstate = tuplesort_begin_index_hash(heap,
												   index,
												   hspool->high_mask,
												   hspool->low_mask,
												   hspool->max_buckets,
												   maintenance_work_mem,
												   NULL,
												   TUPLESORT_NONE);

	return hspool;
}

/*
 * 清理假脱机结构及其子结构。
 */
void
_h_spooldestroy(HSpool *hspool)
{
	tuplesort_end(hspool->sortstate);
	pfree(hspool);
}

/*
 * 把一条索引项假脱机写入排序文件。
 */
void
_h_spool(HSpool *hspool, ItemPointer self, Datum *values, bool *isnull)
{
	tuplesort_putindextuplevalues(hspool->sortstate, hspool->index,
								  self, values, isnull);
}

/*
 * 对由连续调用 _h_spool 装入的假脱机，
 * 创建整个索引。
 */
void
_h_indexbuild(HSpool *hspool, Relation heapRel)
{
	IndexTuple	itup;
	int64		tups_done = 0;
#ifdef USE_ASSERT_CHECKING
	uint32		hashkey = 0;
#endif

	tuplesort_performsort(hspool->sortstate);

	while ((itup = tuplesort_getindextuple(hspool->sortstate, true)) != NULL)
	{
		/*
		 * 从技术上说，哈希键是否按排序顺序出现
		 * 并不关键，因为这种排序只用来提高访问
		 * 局部性，作为一种性能优化。不过，用断言
		 * 测试 tuplesort.c 对 hash 索引元组排序的
		 * 处理，仍然是个好主意。
		 */
#ifdef USE_ASSERT_CHECKING
		uint32		lasthashkey = hashkey;

		hashkey = _hash_hashkey2bucket(_hash_get_indextuple_hashkey(itup),
									   hspool->max_buckets, hspool->high_mask,
									   hspool->low_mask);
		Assert(hashkey >= lasthashkey);
#endif

		/* 元组已按 hashkey 排序，因此把 'sorted' 作为 true 传入 */
		_hash_doinsert(hspool->index, itup, heapRel, true);

		/* 允许插入阶段被中断，并跟踪进度 */
		CHECK_FOR_INTERRUPTS();

		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
									 ++tups_done);
	}
}
