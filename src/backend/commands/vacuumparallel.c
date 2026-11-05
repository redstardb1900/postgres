/*-------------------------------------------------------------------------
 *
 * vacuumparallel.c
 *	  Support routines for parallel vacuum execution.
 *
 * This file contains routines that are intended to support setting up, using,
 * and tearing down a ParallelVacuumState.
 *
 * In a parallel vacuum, we perform both index bulk deletion and index cleanup
 * with parallel worker processes.  Individual indexes are processed by one
 * vacuum process.  ParallelVacuumState contains shared information as well as
 * the memory space for storing dead items allocated in the DSM segment.  We
 * launch parallel worker processes at the start of parallel index
 * bulk-deletion and index cleanup and once all indexes are processed, the
 * parallel worker processes exit.  Each time we process indexes in parallel,
 * the parallel context is re-initialized so that the same DSM can be used for
 * multiple passes of index bulk-deletion and index cleanup.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuumparallel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * 并行清理的 DSM 键。与其他并行执行代码不同，由于我们无需担心
 * DSM 键与 plan_node_id 冲突，因此可以使用小整数。
 * use small integers.
 */
#define PARALLEL_VACUUM_KEY_SHARED			1
#define PARALLEL_VACUUM_KEY_DEAD_ITEMS		2
#define PARALLEL_VACUUM_KEY_QUERY_TEXT		3
#define PARALLEL_VACUUM_KEY_BUFFER_USAGE	4
#define PARALLEL_VACUUM_KEY_WAL_USAGE		5
#define PARALLEL_VACUUM_KEY_INDEX_STATS		6

/*
 * 并行工作进程之间的共享信息。因此它分配在 DSM 段中。
 * segment.
 */
typedef struct PVShared
{
	/*
	 * 目标表 relid 和日志级别（用于 VACUUM VERBOSE 期间启动并行
	 * 工作进程的消息）。这些字段在并行清理期间不会被修改。
	 * the parallel vacuum.
	 */
	Oid			relid;
	int			elevel;

	/*
	 * 索引清理和清除的共同字段。
	 *
	 * reltuples 是输入堆元组的总数。在索引清理情况下，我们设置为旧的活跃
	 * 元组数，在索引清除情况下设置为新的活跃元组数。
	 * 
	 *如果 reltuples 是估计值，则 estimated_count 为 true。
	 * （请注意，在此情况下 reltuples 可能为 -1，表示我们不知道。）
	 * reltuples could be -1 in this case, indicating we have no idea.)
	 */
	double		reltuples;
	bool		estimated_count;

	/*
	 * 在单进程清理中，除了堆扫描的内存之外，索引清理或清除期间可能会
	 * 消耗更多内存。在并行清理中，由于单个清理工作进程可以消耗相当于
	 * maintenance_work_mem 的内存，因此为每个工作进程设置的新的
	 * maintenance_work_mem 要确保并行操作不会比单进程清理消耗更多内存。
	 * worker is set such that the parallel operation doesn't consume more
	 * memory than single process vacuum.
	 */
	int			maintenance_work_mem_worker;

	/*
	 * 每个工作进程的缓冲区访问策略环应包含的缓冲区数量。
	 * contain.
	 */
	int			ring_nbuffers;

	/*
	 * 共享清理成本余额。在并行清理期间，VacuumSharedCostBalance 指向此值，
	 * 它累积每个并行清理工作进程的余额。
	 * balance of each parallel vacuum worker.
	 */
	pg_atomic_uint32 cost_balance;

	/*
	 * 活跃并行工作进程的数量。此值用于计算工作进程在进入基于成本的延迟
	 * 睡眠前所需的清理成本余额最低阈值。
	 * cost-based delay.
	 */
	pg_atomic_uint32 active_nworkers;

	/* 清理和清除的计数器 */
	pg_atomic_uint32 idx;
} PVShared;

/* 并行索引清理或清除期间使用的状态 */
typedef enum PVIndVacStatus
{
	PARALLEL_INDVAC_STATUS_INITIAL = 0,
	PARALLEL_INDVAC_STATUS_NEED_BULKDELETE,
	PARALLEL_INDVAC_STATUS_NEED_CLEANUP,
	PARALLEL_INDVAC_STATUS_COMPLETED
} PVIndVacStatus;

/*
 * 用于并行清理的索引的索引清理统计信息结构体。其中包括并行索引清理
 * 的状态以及索引统计信息。
 */
typedef struct PVIndStats
{
	/*
	 * 以下两个字段由 leader 进程在执行并行索引清理或并行索引清除之前设置。
	 * 这些字段并不是在整个 VACUUM 操作期间保持不变，它们仅在单个并行索引
	 * 清理和清除期间固定。
	 * 
	 *如果 leader 和工作进程都可以处理索引，则 parallel_workers_can_process
	 * 为 true，否则只有 leader 可以处理。
	 * process the index, otherwise only leader can process it.
	 */
	PVIndVacStatus status;
	bool		parallel_workers_can_process;

	/*
	 * 单个工作进程或 leader 存储索引清理或清除的结果。
	 * cleanup.
	 */
	bool		istat_updated;	/* 统计信息是否已更新？ */
	IndexBulkDeleteResult istat;
} PVIndStats;

/*
 * 维护并行清理状态的结构体。typedef 出现在 vacuum.h 中。
 */
struct ParallelVacuumState
{
	/* 工作进程为 NULL */
	ParallelContext *pcxt;

	/* 父堆关系 */
	Relation	heaprel;

	/* 目标索引 */
	Relation   *indrels;
	int			nindexes;

	/* 并行清理工作进程间的共享信息 */
	PVShared   *shared;

	/*
	 * 并行清理工作进程间的共享索引统计信息。数组元素为每个索引分配，
	 * 即使那些并行索引清理不安全或不值得的索引（例如
	 * will_parallel_vacuum[] 为 false）。在并行清理期间，每个索引的
	 * IndexBulkDeleteResult 保存在 DSM 中，并在并行清理结束时复制到本地内存。
	 * IndexBulkDeleteResult of each index is kept in DSM and is copied into
	 * local memory at the end of parallel vacuum.
	 */
	PVIndStats *indstats;

	/* 并行清理工作进程间的共享死元组空间 */
	VacDeadItems *dead_items;

	/* 指向 DSM 中的缓冲区使用区域 */
	BufferUsage *buffer_usage;

	/* 指向 DSM 中的 WAL 使用区域 */
	WalUsage   *wal_usage;

	/*
	 * 如果索引完全不适合任何并行处理的目标，则为 false。
	 * 例如，索引可能小于 min_parallel_index_scan_size 的截断值。
	 * min_parallel_index_scan_size cutoff.
	 */
	bool	   *will_parallel_vacuum;

	/*
	 * 分别支持并行索引批量删除和并行索引清除的索引数量。
	 * parallel index cleanup respectively.
	 */
	int			nindexes_parallel_bulkdel;
	int			nindexes_parallel_cleanup;
	int			nindexes_parallel_condcleanup;

	/* leader 进程使用的缓冲区访问策略 */
	BufferAccessStrategy bstrategy;

	/*
	 * 错误报告状态。错误回调仅在并行索引清理期间为工作进程设置。
	 * processes during parallel index vacuum.
	 */
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;
	PVIndVacStatus status;
};

static int	parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
											bool *will_parallel_vacuum);
static void parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
												bool vacuum);
static void parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
											  PVIndStats *indstats);
static bool parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
												   bool vacuum);
static void parallel_vacuum_error_callback(void *arg);

/*
 * 尝试进入并行模式并创建并行上下文。然后初始化共享内存状态。
 * 
 *成功时返回并行清理状态。否则返回 NULL。
 * On success, return parallel vacuum state.  Otherwise return NULL.
 */
ParallelVacuumState *
parallel_vacuum_init(Relation rel, Relation *indrels, int nindexes,
					 int nrequested_workers, int max_items,
					 int elevel, BufferAccessStrategy bstrategy)
{
	ParallelVacuumState *pvs;
	ParallelContext *pcxt;
	PVShared   *shared;
	VacDeadItems *dead_items;
	PVIndStats *indstats;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	bool	   *will_parallel_vacuum;
	Size		est_indstats_len;
	Size		est_shared_len;
	Size		est_dead_items_len;
	int			nindexes_mwm = 0;
	int			parallel_workers = 0;
	int			querylen;

	/*
	 * 必须请求并行清理，且关系上必须有索引。
	 * relation
	 */
	Assert(nrequested_workers >= 0);
	Assert(nindexes > 0);

	/*
	 * 计算要启动的并行清理工作进程数量。
	 */
	will_parallel_vacuum = (bool *) palloc0(sizeof(bool) * nindexes);
	parallel_workers = parallel_vacuum_compute_workers(indrels, nindexes,
													   nrequested_workers,
													   will_parallel_vacuum);
	if (parallel_workers <= 0)
	{
		/* 无法并行执行清理 -- 返回 NULL */
		pfree(will_parallel_vacuum);
		return NULL;
	}

	pvs = (ParallelVacuumState *) palloc0(sizeof(ParallelVacuumState));
	pvs->indrels = indrels;
	pvs->nindexes = nindexes;
	pvs->will_parallel_vacuum = will_parallel_vacuum;
	pvs->bstrategy = bstrategy;
	pvs->heaprel = rel;

	EnterParallelMode();
	pcxt = CreateParallelContext("postgres", "parallel_vacuum_main",
								 parallel_workers);
	Assert(pcxt->nworkers > 0);
	pvs->pcxt = pcxt;

	/* 估算索引清理统计信息的大小 -- PARALLEL_VACUUM_KEY_INDEX_STATS */
	est_indstats_len = mul_size(sizeof(PVIndStats), nindexes);
	shm_toc_estimate_chunk(&pcxt->estimator, est_indstats_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 估算共享信息的大小 -- PARALLEL_VACUUM_KEY_SHARED */
	est_shared_len = sizeof(PVShared);
	shm_toc_estimate_chunk(&pcxt->estimator, est_shared_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 估算 dead_items 的大小 -- PARALLEL_VACUUM_KEY_DEAD_ITEMS */
	est_dead_items_len = vac_max_items_to_alloc_size(max_items);
	shm_toc_estimate_chunk(&pcxt->estimator, est_dead_items_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * 估算 BufferUsage 和 WalUsage 的空间 --
	 * PARALLEL_VACUUM_KEY_BUFFER_USAGE 和 PARALLEL_VACUUM_KEY_WAL_USAGE。
	 *
	 * 如果没有加载关心这些的扩展，我们可以跳过。但我们无法知道是否有
	 * 人在查看 pgBufferUsage 或 pgWalUsage，因此无条件执行。
	 * pgWalUsage, so do it unconditionally.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 最后，估算 PARALLEL_VACUUM_KEY_QUERY_TEXT 的空间 */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* 让编译器安静 */

	InitializeParallelDSM(pcxt);

	/* 准备索引清理统计信息 */
	indstats = (PVIndStats *) shm_toc_allocate(pcxt->toc, est_indstats_len);
	MemSet(indstats, 0, est_indstats_len);
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/*
		 * Cleanup 选项应该是禁用、始终并行执行或条件并行执行。
		 * parallel or conditionally performing in parallel.
		 */
		Assert(((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) ||
			   ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0));
		Assert(vacoptions <= VACUUM_OPTION_MAX_VALID_VALUE);

		if (!will_parallel_vacuum[i])
			continue;

		if (indrel->rd_indam->amusemaintenanceworkmem)
			nindexes_mwm++;

		/*
		 * 记录每个阶段支持并行操作的索引数量。
		 * each phase.
		 */
		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			pvs->nindexes_parallel_bulkdel++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0)
			pvs->nindexes_parallel_cleanup++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0)
			pvs->nindexes_parallel_condcleanup++;
	}
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_INDEX_STATS, indstats);
	pvs->indstats = indstats;

	/* 准备共享信息 */
	shared = (PVShared *) shm_toc_allocate(pcxt->toc, est_shared_len);
	MemSet(shared, 0, est_shared_len);
	shared->relid = RelationGetRelid(rel);
	shared->elevel = elevel;
	shared->maintenance_work_mem_worker =
		(nindexes_mwm > 0) ?
		maintenance_work_mem / Min(parallel_workers, nindexes_mwm) :
		maintenance_work_mem;

	/* 为所有工作进程使用相同的缓冲区大小 */
	shared->ring_nbuffers = GetAccessStrategyBufferCount(bstrategy);

	pg_atomic_init_u32(&(shared->cost_balance), 0);
	pg_atomic_init_u32(&(shared->active_nworkers), 0);
	pg_atomic_init_u32(&(shared->idx), 0);

	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_SHARED, shared);
	pvs->shared = shared;

	/* 准备 dead_items 空间 */
	dead_items = (VacDeadItems *) shm_toc_allocate(pcxt->toc,
												   est_dead_items_len);
	dead_items->max_items = max_items;
	dead_items->num_items = 0;
	MemSet(dead_items->items, 0, sizeof(ItemPointerData) * max_items);
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_DEAD_ITEMS, dead_items);
	pvs->dead_items = dead_items;

	/*
	 * 为每个工作进程的 BufferUsage 和 WalUsage 分配空间；无需初始化。
	 * initialize
	 */
	buffer_usage = shm_toc_allocate(pcxt->toc,
									mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, buffer_usage);
	pvs->buffer_usage = buffer_usage;
	wal_usage = shm_toc_allocate(pcxt->toc,
								 mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_WAL_USAGE, wal_usage);
	pvs->wal_usage = wal_usage;

	/* 为工作进程存储查询字符串 */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		sharedquery[querylen] = '\0';
		shm_toc_insert(pcxt->toc,
					   PARALLEL_VACUUM_KEY_QUERY_TEXT, sharedquery);
	}

	/* 成功 -- 返回并行清理状态 */
	return pvs;
}

/*
 * 销毁并行上下文，并结束并行模式。
 *
 * 由于并行模式下不允许写入，因此将更新后的索引统计信息从 DSM 复制到
 * 本地内存，然后使用它来更新索引统计信息。有人可能认为我们可以退出并行
 * 模式，更新索引统计信息，然后销毁并行上下文，但那样不安全
 * （参见 ExitParallelMode）。
 * context, but that won't be safe (see ExitParallelMode).
 */
void
parallel_vacuum_end(ParallelVacuumState *pvs, IndexBulkDeleteResult **istats)
{
	Assert(!IsParallelWorker());

	/* 复制更新后的统计信息 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->istat_updated)
		{
			istats[i] = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
			memcpy(istats[i], &indstats->istat, sizeof(IndexBulkDeleteResult));
		}
		else
			istats[i] = NULL;
	}

	DestroyParallelContext(pvs->pcxt);
	ExitParallelMode();

	pfree(pvs->will_parallel_vacuum);
	pfree(pvs);
}

/* 返回死元组空间 */
VacDeadItems *
parallel_vacuum_get_dead_items(ParallelVacuumState *pvs)
{
	return pvs->dead_items;
}

/*
 * 使用并行工作进程执行并行索引批量删除。
 */
void
parallel_vacuum_bulkdel_all_indexes(ParallelVacuumState *pvs, long num_table_tuples,
									int num_index_scans)
{
	Assert(!IsParallelWorker());

	/*
	 * 我们只能提供 num_heap_tuples 的近似值，至少目前如此。
	 * for now.
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = true;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, true);
}

/*
 * 使用并行工作进程执行并行索引清除。
 */
void
parallel_vacuum_cleanup_all_indexes(ParallelVacuumState *pvs, long num_table_tuples,
									int num_index_scans, bool estimated_count)
{
	Assert(!IsParallelWorker());

	/*
	 * 我们可以提供更好的存活元组总数估计
	 * （我们假设索引对此比名义上的活跃元组数更感兴趣）。
	 * nominally live tuples).
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = estimated_count;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, false);
}

/*
 * 计算要请求的并行工作进程数量。索引清理和索引清除都可以使用并行
 * 工作进程执行。只有当索引的大小大于
 * min_parallel_index_scan_size 时才有资格进行并行清理，因为为非常小的
 * 索引调用工作进程会损害性能。
 * 
 *nrequested 是用户请求的并行工作进程数。如果 nrequested 为 0，
 * 我们根据 nindexes 计算并行度，即支持并行清理的索引数量。此函数还会
 * 设置 will_parallel_vacuum 以记住参与并行清理的索引。
 * the number of indexes that support parallel vacuum.  This function also
 * sets will_parallel_vacuum to remember indexes that participate in parallel
 * vacuum.
 */
static int
parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
								bool *will_parallel_vacuum)
{
	int			nindexes_parallel = 0;
	int			nindexes_parallel_bulkdel = 0;
	int			nindexes_parallel_cleanup = 0;
	int			parallel_workers;

	/*
	 * 我们不允许在独立后端中执行并行操作，也不允许在禁用并行时执行。
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/*
	 * 计算可以参与并行清理的索引数量。
	 */
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/* 跳过不适合作为并行索引清理目标的索引 */
		if (vacoptions == VACUUM_OPTION_NO_PARALLEL ||
			RelationGetNumberOfBlocks(indrel) < min_parallel_index_scan_size)
			continue;

		will_parallel_vacuum[i] = true;

		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			nindexes_parallel_bulkdel++;
		if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0) ||
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
			nindexes_parallel_cleanup++;
	}

	nindexes_parallel = Max(nindexes_parallel_bulkdel,
							nindexes_parallel_cleanup);

	/* leader 进程获取一个索引 */
	nindexes_parallel--;

	/* 没有索引支持并行清理 */
	if (nindexes_parallel <= 0)
		return 0;

	/* 计算并行度 */
	parallel_workers = (nrequested > 0) ?
		Min(nrequested, nindexes_parallel) : nindexes_parallel;

	/* 由 max_parallel_maintenance_workers 设上限 */
	parallel_workers = Min(parallel_workers, max_parallel_maintenance_workers);

	return parallel_workers;
}

/*
 * 使用并行工作进程执行索引清理或索引清除。此函数必须由并行清理
 * leader 进程使用。
 */
static void
parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
									bool vacuum)
{
	int			nworkers;
	PVIndVacStatus new_status;

	Assert(!IsParallelWorker());

	if (vacuum)
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_BULKDELETE;

		/* 确定要启动的并行工作进程数 */
		nworkers = pvs->nindexes_parallel_bulkdel;
	}
	else
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_CLEANUP;

		/* 确定要启动的并行工作进程数 */
		nworkers = pvs->nindexes_parallel_cleanup;

		/* 如果是第一次调用，添加条件并行感知的索引 */
		if (num_index_scans == 0)
			nworkers += pvs->nindexes_parallel_condcleanup;
	}

	/* leader 进程将参与 */
	nworkers--;

	/*
	 * 可能出现并行上下文初始化的工作进程数少于当前阶段需要单独工作进程的
	 * 索引数的情况，所以我们需要考虑这种情况。参见
	 * parallel_vacuum_compute_workers()。
	 * parallel_vacuum_compute_workers().
	 */
	nworkers = Min(nworkers, pvs->pcxt->nworkers);

	/*
	 * 设置索引清理状态并标记并行清理工作进程是否可以处理它。
	 * process it.
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		Assert(indstats->status == PARALLEL_INDVAC_STATUS_INITIAL);
		indstats->status = new_status;
		indstats->parallel_workers_can_process =
			(pvs->will_parallel_vacuum[i] &&
			 parallel_vacuum_index_is_parallel_safe(pvs->indrels[i],
													num_index_scans,
													vacuum));
	}

	/* 重置并行索引处理计数器 */
	pg_atomic_write_u32(&(pvs->shared->idx), 0);

	/* 设置共享的基于成本的清理延迟并启动工作进程 */
	if (nworkers > 0)
	{
		/* 重新初始化并行上下文以重新启动并行工作进程 */
		if (num_index_scans > 0)
			ReinitializeParallelDSM(pvs->pcxt);

		/*
		 * 为清理延迟设置共享成本余额和活跃工作进程数。我们需要在启动工作
		 * 进程之前完成此操作，否则它们可能看不到这些参数的最新值。
		 * otherwise, they might not see the updated values for these
		 * parameters.
		 */
		pg_atomic_write_u32(&(pvs->shared->cost_balance), VacuumCostBalance);
		pg_atomic_write_u32(&(pvs->shared->active_nworkers), 0);

		/*
		 * 工作进程数量在批量删除和清除阶段之间可能不同。
		 * phase.
		 */
		ReinitializeParallelWorkers(pvs->pcxt, nworkers);

		LaunchParallelWorkers(pvs->pcxt);

		if (pvs->pcxt->nworkers_launched > 0)
		{
			/*
			 * 重置 leader 后端的本地成本值，因为我们已经累计了堆的剩余余额。
			 * already accumulated the remaining balance of heap.
			 */
			VacuumCostBalance = 0;
			VacuumCostBalanceLocal = 0;

			/* 为 leader 后端启用共享成本余额 */
			VacuumSharedCostBalance = &(pvs->shared->cost_balance);
			VacuumActiveNWorkers = &(pvs->shared->active_nworkers);
		}

		if (vacuum)
			ereport(pvs->shared->elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index vacuuming (planned: %d)",
									 "launched %d parallel vacuum workers for index vacuuming (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
		else
			ereport(pvs->shared->elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index cleanup (planned: %d)",
									 "launched %d parallel vacuum workers for index cleanup (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
	}

	/* 清理只能由 leader 进程处理的索引 */
	parallel_vacuum_process_unsafe_indexes(pvs);

	/*
	 * 作为并行工作进程加入。在没有启动工作进程的情况下，leader 独自清理
	 * 所有并行安全的索引。
	 */
	parallel_vacuum_process_safe_indexes(pvs);

	/*
	 * 然后，累积缓冲区和 WAL 的使用情况。（这必须等待工作进程完成，
	 * 否则可能得到不完整的数据。）
	 */
	if (nworkers > 0)
	{
		/* 等待所有清理工作进程完成 */
		WaitForParallelWorkersToFinish(pvs->pcxt);

		for (int i = 0; i < pvs->pcxt->nworkers_launched; i++)
			InstrAccumParallelQuery(&pvs->buffer_usage[i], &pvs->wal_usage[i]);
	}

	/*
	 * 将所有索引状态重置为初始状态（同时检查我们已清理了所有索引）。
	 * vacuumed all indexes).
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->status != PARALLEL_INDVAC_STATUS_COMPLETED)
			elog(ERROR, "parallel index vacuum on index \"%s\" is not completed",
				 RelationGetRelationName(pvs->indrels[i]));

		indstats->status = PARALLEL_INDVAC_STATUS_INITIAL;
	}

	/*
	 * 将共享余额值传递到堆扫描并禁用共享成本计算
	 */
	if (VacuumSharedCostBalance)
	{
		VacuumCostBalance = pg_atomic_read_u32(VacuumSharedCostBalance);
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;
	}
}

/*
 * leader 进程和并行清理工作进程使用的索引清理/清除例程，
 * 用于并行清理索引。
 */
static void
parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs)
{
	/*
	 * 如果能够启动任何工作进程，则增加活跃工作进程计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	/* 循环直到所有索引都被清理 */
	for (;;)
	{
		int			idx;
		PVIndStats *indstats;

		/* 获取要处理的索引编号 */
		idx = pg_atomic_fetch_add_u32(&(pvs->shared->idx), 1);

		/* 所有索引处理完毕？ */
		if (idx >= pvs->nindexes)
			break;

		indstats = &(pvs->indstats[idx]);

		/*
		 * 跳过对工作进程不安全或不适合作为并行索引清理目标的索引进行清理
		 * （这些索引由 leader 在 parallel_vacuum_process_unsafe_indexes() 中
		 * 清理）。
		 */
		if (!indstats->parallel_workers_can_process)
			continue;

		/* 执行索引的清理或清除 */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[idx], indstats);
	}

	/*
	 * 我们已完成索引清理，因此减少活跃工作进程计数。
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * 在 leader 进程中执行索引的并行清理。
 *
 * 处理非并行安全的索引的索引清理（或索引清除）。对于特定索引，这可能
 * 因为诸如当前是否正在执行索引清除等细节而有所不同。
 * 
 *还清理那些低于 parallel_vacuum_compute_workers() 强制的大小截断值的
 * 较小索引。
 * enforced by parallel_vacuum_compute_workers().
 */
static void
parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs)
{
	Assert(!IsParallelWorker());

	/*
	 * 如果能够启动任何工作进程，则增加活跃工作进程计数。
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		/* 跳过对工作进程安全的索引 */
		if (indstats->parallel_workers_can_process)
			continue;

		/* 执行索引的清理或清除 */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[i], indstats);
	}

	/*
	 * 我们已完成索引清理，因此减少活跃工作进程计数。
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * 由 leader 进程或某个工作进程清理或清除索引。清理索引后，
 * 此函数将 ambulkdelete 和 amvacuumcleanup 返回的索引统计信息复制到
 * DSM 段。
 * segment.
 */
static void
parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
								  PVIndStats *indstats)
{
	IndexBulkDeleteResult *istat = NULL;
	IndexBulkDeleteResult *istat_res;
	IndexVacuumInfo ivinfo;

	/*
	 * 如果已有人更新了相应的批量删除结果，则更新指向它的指针。
	 * has already updated it
	 */
	if (indstats->istat_updated)
		istat = &(indstats->istat);

	ivinfo.index = indrel;
	ivinfo.heaprel = pvs->heaprel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.message_level = DEBUG2;
	ivinfo.estimated_count = pvs->shared->estimated_count;
	ivinfo.num_heap_tuples = pvs->shared->reltuples;
	ivinfo.strategy = pvs->bstrategy;

	/* 更新错误回溯信息 */
	pvs->indname = pstrdup(RelationGetRelationName(indrel));
	pvs->status = indstats->status;

	switch (indstats->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			istat_res = vac_bulkdel_one_index(&ivinfo, istat, pvs->dead_items);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			istat_res = vac_cleanup_one_index(&ivinfo, istat);
			break;
		default:
			elog(ERROR, "unexpected parallel vacuum index status %d for index \"%s\"",
				 indstats->status,
				 RelationGetRelationName(indrel));
	}

	/*
	 * 把从 ambulkdelete 和 amvacuumcleanup 返回的索引批量     
	 * 删除结果复制到 DSM 段（若这是第一周期），因为      
	 * 它们是在本地分配的，下一个周期可能由另一个不    
	 * 同的清理进程来清理它。复制结果通常只在索引      
	 * 第一次被清理时发生。对任何额外的清理轮次，我   
	 * 们直接指向 DSM 段上的结果并把它传给清理索引的  
	 * API，让工作进程能够直接更新它。                       
	 *
	 * 由于所有清理工作进程在不同的槽位写入批量删除  
	 * 结果，所以可以不加锁写入。 
	 */
	if (!indstats->istat_updated && istat_res != NULL)
	{
		memcpy(&(indstats->istat), istat_res, sizeof(IndexBulkDeleteResult));
		indstats->istat_updated = true;

		/* 释放本地分配的批量删除结果 */
		pfree(istat_res);
	}

	/*
	 * 将状态更新为已完成。无需在此锁定，因为每个工作进程处理不同的索引。
	 * touches different indexes.
	 */
	indstats->status = PARALLEL_INDVAC_STATUS_COMPLETED;

	/* 重置错误回溯信息 */
	pvs->status = PARALLEL_INDVAC_STATUS_COMPLETED;
	pfree(pvs->indname);
	pvs->indname = NULL;
}

/*
 * 如果给定索引无法参与下一次并行索引清理或并行索引清除的执行，
 * 则返回 false。
 */
static bool
parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
									   bool vacuum)
{
	uint8		vacoptions;

	vacoptions = indrel->rd_indam->amparallelvacuumoptions;

	/* 在并行清理情况下，检查是否支持并行批量删除 */
	if (vacuum)
		return ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0);

	/* 如果索引不支持并行清除，则不安全 */
	if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0))
		return false;

	/*
	 * 如果索引有条件地支持并行清除，但我们已经处理了该索引（批量删除），
	 * 则不安全。我们这样做是为了避免在并行索引清除不需要扫描索引时调用
	 * 工作进程。请参阅选项 VACUUM_OPTION_PARALLEL_COND_CLEANUP 的注释以
	 * 了解索引何时有条件地支持并行清除。
	 * VACUUM_OPTION_PARALLEL_COND_CLEANUP to know when indexes support
	 * parallel cleanup conditionally.
	 */
	if (num_index_scans > 0 &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
		return false;

	return true;
}

/*
 * 在已启动的并行进程中执行工作。
 *
 * 由于并行清理工作进程仅执行索引清理或索引清除，因此无需报告进度
 * 信息。
 */
void
parallel_vacuum_main(dsm_segment *seg, shm_toc *toc)
{
	ParallelVacuumState pvs;
	Relation	rel;
	Relation   *indrels;
	PVIndStats *indstats;
	PVShared   *shared;
	VacDeadItems *dead_items;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	int			nindexes;
	char	   *sharedquery;
	ErrorContextCallback errcallback;

	/*
	 * 并行清理工作进程必须只有 PROC_IN_VACUUM 标志，因为目前我们不支持
	 * 自动清理的并行清理。
	 */
	Assert(MyProc->statusFlags == PROC_IN_VACUUM);

	elog(DEBUG1, "starting parallel vacuum worker");

	shared = (PVShared *) shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_SHARED, false);

	/* 为各个工作进程设置 debug_query_string */
	sharedquery = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/*
	 * 打开表。锁模式与 leader 进程相同。这是可以的，因为锁模式在并行
	 * 工作进程之间不会冲突。
	 * workers.
	 */
	rel = table_open(shared->relid, ShareUpdateExclusiveLock);

	/*
	 * 打开所有索引。indrels 按 OID 顺序排序，这应与 leader 的排序相匹配。
	 * matched to the leader's one.
	 */
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	Assert(nindexes > 0);

	if (shared->maintenance_work_mem_worker > 0)
		maintenance_work_mem = shared->maintenance_work_mem_worker;

	/* 设置索引统计信息 */
	indstats = (PVIndStats *) shm_toc_lookup(toc,
											 PARALLEL_VACUUM_KEY_INDEX_STATS,
											 false);

	/* 设置 dead_items 空间 */
	dead_items = (VacDeadItems *) shm_toc_lookup(toc,
												 PARALLEL_VACUUM_KEY_DEAD_ITEMS,
												 false);

	/* 设置基于成本的清理延迟 */
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	VacuumPageHit = 0;
	VacuumPageMiss = 0;
	VacuumPageDirty = 0;
	VacuumCostBalanceLocal = 0;
	VacuumSharedCostBalance = &(shared->cost_balance);
	VacuumActiveNWorkers = &(shared->active_nworkers);

	/* 设置并行清理状态 */
	pvs.indrels = indrels;
	pvs.nindexes = nindexes;
	pvs.indstats = indstats;
	pvs.shared = shared;
	pvs.dead_items = dead_items;
	pvs.relnamespace = get_namespace_name(RelationGetNamespace(rel));
	pvs.relname = pstrdup(RelationGetRelationName(rel));
	pvs.heaprel = rel;

	/* 这些字段将在索引清理或清除期间填充 */
	pvs.indname = NULL;
	pvs.status = PARALLEL_INDVAC_STATUS_INITIAL;

	/* 每个并行 VACUUM 工作进程获取自己的访问策略。 */
	pvs.bstrategy = GetAccessStrategyWithSize(BAS_VACUUM,
											  shared->ring_nbuffers * (BLCKSZ / 1024));

	/* 为 ereport() 设置错误回溯支持 */
	errcallback.callback = parallel_vacuum_error_callback;
	errcallback.arg = &pvs;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 准备在并行执行期间跟踪缓冲区使用情况 */
	InstrStartParallelQuery();

	/* 处理索引以执行清理/清除 */
	parallel_vacuum_process_safe_indexes(&pvs);

	/* 报告并行执行期间的缓冲区/WAL 使用情况 */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* 弹出错误上下文栈 */
	error_context_stack = errcallback.previous;

	vac_close_indexes(nindexes, indrels, RowExclusiveLock);
	table_close(rel, ShareUpdateExclusiveLock);
	FreeAccessStrategy(pvs.bstrategy);
}

/*
 * 并行索引清理期间发生错误时的错误上下文回调。错误上下文消息应与
 * lazy 清理错误上下文中设置的消息匹配。如果更改此函数，请同时更改
 * vacuum_error_callback()。
 * as well.
 */
static void
parallel_vacuum_error_callback(void *arg)
{
	ParallelVacuumState *errinfo = arg;

	switch (errinfo->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_INITIAL:
		case PARALLEL_INDVAC_STATUS_COMPLETED:
		default:
			return;
	}
}
