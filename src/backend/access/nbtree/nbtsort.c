/*-------------------------------------------------------------------------
 *
 * nbtsort.c
 *		按已排序输入顺序装入叶子页，完成 B-树的排序构建。
 *
 * 说明（NOTES）
 *
 * 我们用 tuplesort.c 把给定的索引元组排成顺序。
 * 然后按序扫描这些索引元组，并为每一层构建 B-树页。
 * 源元组装入叶子层的页。
 * 每当某一层的一页被填满，就向其父层加入指向它的链接
 * （必要时开始一个新的父层）。完成之后，
 * 写出每一层的最后一页，并把它加入
 * 其父层。当某一层只剩一页时，它必定是
 * 根——可以把它挂到 B-树元数据页上，构建即告完成。
 *
 * 把页完全塞满并不明智，因为那样*任何*
 * 插入都会引起分裂（而且不只是叶子页；分裂的需要
 * 会一直向上级联）。B-树的稳态负载
 * 因子通常估计为 70%。我们选择把叶子页
 * 装到用户可控的 fillfactor（默认 90%），而上层页
 * 始终装到 70%。这样密度仍然合理（键的大小合理时，
 * 上层页并不多），又不会在早期插入时冒大量
 * 级联分裂的风险。
 *
 * 从前，正在构建的索引页放在共享缓冲区里，但
 * 那并无价值（因为其他后端当时对它们还没有兴趣），
 * 而且会给 CHECKPOINT 带来加锁问题，因为上层
 * 页会长时间被排他锁定。现在我们只在
 * 本地内存中构建这些页，完成后再用 smgrwrite 或 smgrextend
 * 把它们写出。构建结束后首次使用时，还需要重新读入共享缓冲区，
 * 才能用到它们。
 *
 * 这段代码完全不关心空闲空间映射（FSM）。调用者负责
 * 初始化它。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"		/* pgrminclude ignore */
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"


/* 并行状态共享用的魔数 */
#define PARALLEL_KEY_BTREE_SHARED		UINT64CONST(0xA000000000000001)
#define PARALLEL_KEY_TUPLESORT			UINT64CONST(0xA000000000000002)
#define PARALLEL_KEY_TUPLESORT_SPOOL2	UINT64CONST(0xA000000000000003)
#define PARALLEL_KEY_QUERY_TEXT			UINT64CONST(0xA000000000000004)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xA000000000000005)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xA000000000000006)

/*
 * DISABLE_LEADER_PARTICIPATION 禁止领导进程参与
 * 并行索引构建。这作为调试辅助可能有用。
#undef DISABLE_LEADER_PARTICIPATION
 */

/*
 * 假脱机/排序阶段的状态。（注意，由于带有
 * 死元组的唯一性检查有特殊需要，我们可能有
 * 两份这样的结构。）
 */
typedef struct BTSpool
{
	Tuplesortstate *sortstate;	/* tuplesort.c 的状态数据 */
	Relation	heap;
	Relation	index;
	bool		isunique;
	bool		nulls_not_distinct;
} BTSpool;

/*
 * 并行执行的索引构建所用的状态。它分配在
 * 动态共享内存段中。注意另有一条独立的 tuplesort TOC
 * 项，归 tuplesort.c 私有，但由本模块代为分配。
 */
typedef struct BTShared
{
	/*
	 * 这些字段在排序期间不会被修改。它们主要是为了
	 * 方便工作进程创建与领导进程所用相对应的
	 * BTSpool 状态。
	 */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isunique;
	bool		nulls_not_distinct;
	bool		isconcurrent;
	int			scantuplesortstates;

	/*
	 * workersdonecv 用来监视工作进程的进度。全体并行
	 * 参与者都必须表明自己已经完成，领导进程才能使用
	 * 工作进程在扫描期间维护的可变状态（也才能
	 * 继续调用 tuplesort_performsort()）。
	 */
	ConditionVariable workersdonecv;

	/*
	 * mutex 保护 heapdesc 之前的所有字段。
	 *
	 * 这些字段含有 B-树索引构建所关心的状态信息，
	 * 索引并行构建时也必须同样有效。
	 */
	slock_t		mutex;

	/*
	 * 由工作进程维护、并在并行扫描结束时汇报给
	 * 领导进程的可变状态。
	 *
	 * nparticipantsdone 是已完成的工作进程数。
	 *
	 * reltuples 是输入堆元组的总数。
	 *
	 * havedead 表示构建期间是否遇到 RECENTLY_DEAD
	 * 元组。
	 *
	 * indtuples 是最终进入索引的元组总数。
	 *
	 * brokenhotchain 表示是否有工作进程在构建期间
	 * 发现断裂的 HOT 链。
	 */
	int			nparticipantsdone;
	double		reltuples;
	bool		havedead;
	double		indtuples;
	bool		brokenhotchain;

	/*
	 * 后面跟着 ParallelTableScanDescData 数据。不能直接嵌在这里，
	 * 因为并行表扫描描述符接口的实现可能需要
	 * 更强的对齐。
	 */
} BTShared;

/*
 * 返回指向 BTShared 并行表扫描的指针。
 *
 * 关于为何使用 BUFFERALIGN 而不是仅仅 MAXALIGN，
 * 参见 shm_toc_allocate。
 */
#define ParallelTableScanFromBTShared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(BTShared)))

/*
 * 并行索引构建中领导进程的状态。
 */
typedef struct BTLeader
{
	/* 并行上下文本身 */
	ParallelContext *pcxt;

	/*
	 * nparticipanttuplesorts 是成功启动的工作进程的准确数目，
	 * 若领导进程作为工作进程参与，则再加一（只有
	 * DISABLE_LEADER_PARTICIPATION 构建才会避免领导进程
	 * 作为工作进程参与）。
	 */
	int			nparticipanttuplesorts;

	/*
	 * 领导进程用来方便访问共享状态的指针（领导进程避免做 TOC
	 * 查找）。
	 *
	 * btshared 是整个构建的共享状态。sharedsort 是
	 * 传给每个进程 tuplesort 的、由 tuplesort 管理的共享状态。
	 * sharedsort2 是对应的 btspool2 共享状态，仅在
	 * 构建唯一索引时使用。snapshot 是扫描使用的快照，当且仅当
	 * 需要 MVCC 快照时才有。
	 */
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} BTLeader;

/*
 * btbuild 及其回调的工作状态。
 *
 * 使用并行 CREATE INDEX 时，每个参与者都有一份
 * BTBuildState。
 */
typedef struct BTBuildState
{
	bool		isunique;
	bool		nulls_not_distinct;
	bool		havedead;
	Relation	heap;
	BTSpool    *spool;

	/*
	 * 仅当索引是唯一索引时才需要 spool2。死元组放进
	 * spool2 而不是 spool，以避免唯一性检查。
	 */
	BTSpool    *spool2;
	double		indtuples;

	/*
	 * 仅在执行并行索引构建时，且仅在领导进程中，才有 btleader。
	 * （实际上只有领导进程持有 BTBuildState。不过工作进程
	 * 有自己的 spool 和 spool2。）
	 */
	BTLeader   *btleader;
} BTBuildState;

/*
 * 正在构建的 B-树页的状态记录。每一个活动的
 * 树层都有一份。
 */
typedef struct BTPageState
{
	Page		btps_page;		/* 构建页用的工作区 */
	BlockNumber btps_blkno;		/* 写出本页时使用的块号 */
	IndexTuple	btps_lowkey;	/* 本页的严格下界枢轴元组 */
	OffsetNumber btps_lastoff;	/* 已装入的最后一项偏移 */
	Size		btps_lastextra; /* 最后一项额外的倒排列表空间 */
	uint32		btps_level;		/* 树层（0 = 叶子） */
	Size		btps_full;		/* "full" 表示空闲空间少于此值 */
	struct BTPageState *btps_next;	/* 指向父层的链接，若有 */
} BTPageState;

/*
 * 索引写出阶段的总体状态记录。
 */
typedef struct BTWriteState
{
	Relation	heap;
	Relation	index;
	BTScanInsert inskey;		/* 通用插入扫描键 */
	bool		btws_use_wal;	/* 把页转储到 WAL？ */
	BlockNumber btws_pages_alloced; /* 已分配的页数 */
	BlockNumber btws_pages_written; /* 已写出的页数 */
	Page		btws_zeropage;	/* 填充零用的工作区 */
} BTWriteState;


static double _bt_spools_heapscan(Relation heap, Relation index,
								  BTBuildState *buildstate, IndexInfo *indexInfo);
static void _bt_spooldestroy(BTSpool *btspool);
static void _bt_spool(BTSpool *btspool, ItemPointer self,
					  Datum *values, bool *isnull);
static void _bt_leafbuild(BTSpool *btspool, BTSpool *btspool2);
static void _bt_build_callback(Relation index, ItemPointer tid, Datum *values,
							   bool *isnull, bool tupleIsAlive, void *state);
static Page _bt_blnewpage(uint32 level);
static BTPageState *_bt_pagestate(BTWriteState *wstate, uint32 level);
static void _bt_slideleft(Page rightmostpage);
static void _bt_sortaddtup(Page page, Size itemsize,
						   IndexTuple itup, OffsetNumber itup_off,
						   bool newfirstdataitem);
static void _bt_buildadd(BTWriteState *wstate, BTPageState *state,
						 IndexTuple itup, Size truncextra);
static void _bt_sort_dedup_finish_pending(BTWriteState *wstate,
										  BTPageState *state,
										  BTDedupState dstate);
static void _bt_uppershutdown(BTWriteState *wstate, BTPageState *state);
static void _bt_load(BTWriteState *wstate,
					 BTSpool *btspool, BTSpool *btspool2);
static void _bt_begin_parallel(BTBuildState *buildstate, bool isconcurrent,
							   int request);
static void _bt_end_parallel(BTLeader *btleader);
static Size _bt_parallel_estimate_shared(Relation heap, Snapshot snapshot);
static double _bt_parallel_heapscan(BTBuildState *buildstate,
									bool *brokenhotchain);
static void _bt_leader_participate_as_worker(BTBuildState *buildstate);
static void _bt_parallel_scan_and_sort(BTSpool *btspool, BTSpool *btspool2,
									   BTShared *btshared, Sharedsort *sharedsort,
									   Sharedsort *sharedsort2, int sortmem,
									   bool progress);


/*
 *	btbuild() -- 构建一个新的 B-树索引。
 */
IndexBuildResult *
btbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BTBuildState buildstate;
	double		reltuples;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif							/* BTREE_BUILD_STATS */

	buildstate.isunique = indexInfo->ii_Unique;
	buildstate.nulls_not_distinct = indexInfo->ii_NullsNotDistinct;
	buildstate.havedead = false;
	buildstate.heap = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;
	buildstate.btleader = NULL;

	/*
	 * 对于任何索引关系，我们预期只被调用一次。若并非如此，
	 * 那就是大麻烦了。
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	reltuples = _bt_spools_heapscan(heap, index, &buildstate, indexInfo);

	/*
	 * 按以下步骤完成构建：(1) 完成假脱机文件的排序，(2)
	 * 把排好序的元组插入 B-树页，(3) 构建上层。
	 * 最后，可能还需要结束对并行的使用。
	 */
	_bt_leafbuild(buildstate.spool, buildstate.spool2);
	_bt_spooldestroy(buildstate.spool);
	if (buildstate.spool2)
		_bt_spooldestroy(buildstate.spool2);
	if (buildstate.btleader)
		_bt_end_parallel(buildstate.btleader);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	return result;
}

/*
 * 创建并初始化一个或两个假脱机结构，存入调用者的
 * buildstate 参数。也可能填写索引构建要用的 indexInfo
 * 字段。
 *
 * 扫描堆，可能并行进行，用 IndexTuples 填充假脱机。本例程
 * 封装了管理并行的全部方面。调用者只需在用完 spool/spool2
 * 之后，于并行情形下调用 _bt_end_parallel()。
 *
 * 返回扫描到的堆元组总数。
 */
static double
_bt_spools_heapscan(Relation heap, Relation index, BTBuildState *buildstate,
					IndexInfo *indexInfo)
{
	BTSpool    *btspool = (BTSpool *) palloc0(sizeof(BTSpool));
	SortCoordinate coordinate = NULL;
	double		reltuples = 0;

	/*
	 * 排序区按 maintenance_work_mem 而非 work_mem 设定大小，
	 * 以加快索引创建。这应当没问题，因为单个后端无法
	 * 并行运行多个索引创建（另见下文关于并行与
	 * maintenance_work_mem 的说明）。
	 */
	btspool->heap = heap;
	btspool->index = index;
	btspool->isunique = indexInfo->ii_Unique;
	btspool->nulls_not_distinct = indexInfo->ii_NullsNotDistinct;

	/* 存为主假脱机 */
	buildstate->spool = btspool;

	/* 报告表扫描阶段已开始 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN);

	/* 在需要时尝试启动并行工作进程扫描 */
	if (indexInfo->ii_ParallelWorkers > 0)
		_bt_begin_parallel(buildstate, indexInfo->ii_Concurrent,
						   indexInfo->ii_ParallelWorkers);

	/*
	 * 若请求了并行构建，且至少成功启动了一个工作进程，
	 * 则建立协调状态
	 */
	if (buildstate->btleader)
	{
		coordinate = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
		coordinate->isWorker = false;
		coordinate->nParticipants =
			buildstate->btleader->nparticipanttuplesorts;
		coordinate->sharedsort = buildstate->btleader->sharedsort;
	}

	/*
	 * 开始串行/领导进程的 tuplesort。
	 *
	 * 涉及并行时，领导进程得到与串行排序相同份额的
	 * maintenance_work_mem（一旦返回，它大体上被当成
	 * 串行排序来对待）。不过并行工作进程的
	 * Tuplesortstate 只会得到 maintenance_work_mem
	 * 的一小部分。
	 *
	 * 我们依赖领导进程 Tuplesortstate 的生存期几乎不与
	 * 任何工作进程 Tuplesortstate 的生存期重叠。也许有一点
	 * 小的重叠，但这没关系，因为这里依赖领导进程的
	 * Tuplesortstate 只分配少量、固定大小的内存。
	 * 当调用者调用它的 tuplesort_performsort()，并且
	 * 很可能要用掉大量内存时，所有工作进程必须已经
	 * 释放其 Tuplesortstate 持有的几乎全部内存
	 * （它们也即将完全退出）。总体效果是，
	 * maintenance_work_mem 始终表示一次 CREATE INDEX
	 * 操作用掉的内存量的绝对高水位，无论是否使用并行，
	 * 也不论任何其他因素。
	 */
	buildstate->spool->sortstate =
		tuplesort_begin_index_btree(heap, index, buildstate->isunique,
									buildstate->nulls_not_distinct,
									maintenance_work_mem, coordinate,
									TUPLESORT_NONE);

	/*
	 * 若在构建唯一索引，把死元组放进第二个假脱机，
	 * 使它们不参与唯一性检查。我们预期第二个假脱机（用于
	 * 死元组）不会装得很满，因此只给它 work_mem。
	 */
	if (indexInfo->ii_Unique)
	{
		BTSpool    *btspool2 = (BTSpool *) palloc0(sizeof(BTSpool));
		SortCoordinate coordinate2 = NULL;

		/* 初始化次假脱机 */
		btspool2->heap = heap;
		btspool2->index = index;
		btspool2->isunique = false;
		/* 存为次假脱机 */
		buildstate->spool2 = btspool2;

		if (buildstate->btleader)
		{
			/*
			 * 建立传给 tuplesort_begin_index_btree() 的
			 * 非私有状态，说明并行排序在高层上的
			 * 基本协调方式。
			 */
			coordinate2 = (SortCoordinate) palloc0(sizeof(SortCoordinateData));
			coordinate2->isWorker = false;
			coordinate2->nParticipants =
				buildstate->btleader->nparticipanttuplesorts;
			coordinate2->sharedsort = buildstate->btleader->sharedsort2;
		}

		/*
		 * 我们预期第二个（用于死元组）不会装得很满，
		 * 因此只给它 work_mem
		 */
		buildstate->spool2->sortstate =
			tuplesort_begin_index_btree(heap, index, false, false, work_mem,
										coordinate2, TUPLESORT_NONE);
	}

	/* 用串行或并行堆扫描填充假脱机 */
	if (!buildstate->btleader)
		reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
										   _bt_build_callback, (void *) buildstate,
										   NULL);
	else
		reltuples = _bt_parallel_heapscan(buildstate,
										  &indexInfo->ii_BrokenHotChain);

	/*
	 * 为下一阶段设置进度目标。重置由
	 * table_index_build_scan 设置的块号值
	 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE
		};
		const int64 progress_vals[] = {
			buildstate->indtuples,
			0, 0
		};

		pgstat_progress_update_multi_param(3, progress_index, progress_vals);
	}

	/* 好了，全部堆元组都已假脱机 */
	if (buildstate->spool2 && !buildstate->havedead)
	{
		/* 结果并不需要 spool2 */
		_bt_spooldestroy(buildstate->spool2);
		buildstate->spool2 = NULL;
	}

	return reltuples;
}

/*
 * 清理假脱机结构及其子结构。
 */
static void
_bt_spooldestroy(BTSpool *btspool)
{
	tuplesort_end(btspool->sortstate);
	pfree(btspool);
}

/*
 * 把一条索引项假脱机写入排序文件。
 */
static void
_bt_spool(BTSpool *btspool, ItemPointer self, Datum *values, bool *isnull)
{
	tuplesort_putindextuplevalues(btspool->sortstate, btspool->index,
								  self, values, isnull);
}

/*
 * 对由连续调用 _bt_spool 装入的假脱机，
 * 创建整棵 B-树。
 */
static void
_bt_leafbuild(BTSpool *btspool, BTSpool *btspool2)
{
	BTWriteState wstate;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	/* 执行排序 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
	{
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	wstate.heap = btspool->heap;
	wstate.index = btspool->index;
	wstate.inskey = _bt_mkscankey(wstate.index, NULL);
	/* 没有元数据页时 _bt_mkscankey() 不会设置 allequalimage */
	wstate.inskey->allequalimage = _bt_allequalimage(wstate.index, true);
	wstate.btws_use_wal = RelationNeedsWAL(wstate.index);

	/* 预留元数据页 */
	wstate.btws_pages_alloced = BTREE_METAPAGE + 1;
	wstate.btws_pages_written = 0;
	wstate.btws_zeropage = NULL;	/* 需要时再分配 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_LEAF_LOAD);
	_bt_load(&wstate, btspool, btspool2);
}

/*
 * table_index_build_scan 的逐元组回调
 */
static void
_bt_build_callback(Relation index,
				   ItemPointer tid,
				   Datum *values,
				   bool *isnull,
				   bool tupleIsAlive,
				   void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;

	/*
	 * 把索引元组插入合适的假脱机文件，供后续
	 * 处理
	 */
	if (tupleIsAlive || buildstate->spool2 == NULL)
		_bt_spool(buildstate->spool, tid, values, isnull);
	else
	{
		/* 死元组放入 spool2 */
		buildstate->havedead = true;
		_bt_spool(buildstate->spool2, tid, values, isnull);
	}

	buildstate->indtuples += 1;
}

/*
 * 为一张新的、空白的、未链接到任何兄弟的 B-树页分配工作区。
 */
static Page
_bt_blnewpage(uint32 level)
{
	Page		page;
	BTPageOpaque opaque;

	page = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);

	/* 把页清零并设置标准页头信息 */
	_bt_pageinit(page, BLCKSZ);

	/* 初始化 BT 不透明状态 */
	opaque = BTPageGetOpaque(page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo_level = level;
	opaque->btpo_flags = (level > 0) ? 0 : BTP_LEAF;
	opaque->btpo_cycleid = 0;

	/* 让 P_HIKEY 行指针看起来已分配 */
	((PageHeader) page)->pd_lower += sizeof(ItemIdData);

	return page;
}

/*
 * 写出一张已完成的 B-树页，并释放工作存储。
 */
static void
_bt_blwritepage(BTWriteState *wstate, Page page, BlockNumber blkno)
{
	/* XLOG 相关 */
	if (wstate->btws_use_wal)
	{
		/* 这里使用 XLOG_FPI 记录类型 */
		log_newpage(&wstate->index->rd_locator, MAIN_FORKNUM, blkno, page, true);
	}

	/*
	 * 若必须非顺序地写页，就用零填上中间的空间，
	 * 直到回头覆盖。在标准 Unix 文件系统上这在逻辑上
	 * 并非必要（未写过的空间读出来反正也是零），
	 * 但应当有助于避免碎片。不过这些哑页
	 * 并不写入 WAL。
	 */
	while (blkno > wstate->btws_pages_written)
	{
		if (!wstate->btws_zeropage)
			wstate->btws_zeropage = (Page) palloc_aligned(BLCKSZ,
														  PG_IO_ALIGN_SIZE,
														  MCXT_ALLOC_ZERO);
		/* 全零页不设置校验和 */
		smgrextend(RelationGetSmgr(wstate->index), MAIN_FORKNUM,
				   wstate->btws_pages_written++,
				   wstate->btws_zeropage,
				   true);
	}

	PageSetChecksumInplace(page, blkno);

	/*
	 * 现在写出该页。不必让 smgr 为这次写安排 fsync；
	 * 我们将在结束构建之前自己来做。
	 */
	if (blkno == wstate->btws_pages_written)
	{
		/* 正在扩展文件... */
		smgrextend(RelationGetSmgr(wstate->index), MAIN_FORKNUM, blkno,
				   page, true);
		wstate->btws_pages_written++;
	}
	else
	{
		/* 覆盖先前填零的块 */
		smgrwrite(RelationGetSmgr(wstate->index), MAIN_FORKNUM, blkno,
				  page, true);
	}

	pfree(page);
}

/*
 * 分配并初始化一个新的 BTPageState。返回的结构
 * 可供 _bt_buildadd 立即使用。
 */
static BTPageState *
_bt_pagestate(BTWriteState *wstate, uint32 level)
{
	BTPageState *state = (BTPageState *) palloc0(sizeof(BTPageState));

	/* 为该层创建初始页 */
	state->btps_page = _bt_blnewpage(level);

	/* 并给它分配一个页位置 */
	state->btps_blkno = wstate->btws_pages_alloced++;

	state->btps_lowkey = NULL;
	/* 初始化 lastoff，使第一项进入 P_FIRSTKEY */
	state->btps_lastoff = P_HIKEY;
	state->btps_lastextra = 0;
	state->btps_level = level;
	/* 按层设置 "full" 阈值。见文件开头的说明。 */
	if (level > 0)
		state->btps_full = (BLCKSZ * (100 - BTREE_NONLEAF_FILLFACTOR) / 100);
	else
		state->btps_full = BTGetTargetPageFreeSpace(wstate->index);

	/* 尚无父层 */
	state->btps_next = NULL;

	return state;
}

/*
 * 把页上的 ItemIds 数组往回滑动一槽（从 P_FIRSTKEY
 * 滑到 P_HIKEY，覆盖 P_HIKEY）。
 *
 * _bt_blnewpage() 让 P_HIKEY 行指针看起来已分配，但
 * 该层的最右页不应当有高键。既然已经明确
 * 本页是最右页，就去掉不再需要的空
 * P_HIKEY 行指针空间。
 */
static void
_bt_slideleft(Page rightmostpage)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	ItemId		previi;

	maxoff = PageGetMaxOffsetNumber(rightmostpage);
	Assert(maxoff >= P_FIRSTKEY);
	previi = PageGetItemId(rightmostpage, P_HIKEY);
	for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemId		thisii = PageGetItemId(rightmostpage, off);

		*previi = *thisii;
		previi = thisii;
	}
	((PageHeader) rightmostpage)->pd_lower -= sizeof(ItemIdData);
}

/*
 * 向正在构建的页加入一项。
 *
 * 这与 nbtinsert.c 的 _bt_pgaddtup() 非常相似，但这个变体
 * 会直接报错。
 *
 * 注意，nbtsort.c 中的调用者此时还不知道该页是否会成为
 * 最右页。调用者始终假定偏移 P_FIRSTKEY 是第一个数据键。
 * 若该页最终是本层的最右页，则通过调用
 * _bt_slideleft() 来修正。
 */
static void
_bt_sortaddtup(Page page,
			   Size itemsize,
			   IndexTuple itup,
			   OffsetNumber itup_off,
			   bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (PageAddItem(page, (Item) itup, itemsize, itup_off,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to the index page");
}

/*----------
 * 把排序输出中的一项加入磁盘页（或加入由排序输出
 * 形成的倒排列表项）。
 *
 * 必须小心遵守 nbtsearch.c 的页布局约定：
 * - 最右页的数据项从 P_HIKEY 开始，而不是从 P_FIRSTKEY。
 * - 在非叶页上，第一项的键部分不必
 *	 存储，只应存储链接。
 *
 * 正在构建的叶子页看起来像这样：
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * +-------------+------------------------------------+
 * |			 | itemN ...                          |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *
 * 把它与 bufpage.h 中的图对比；注意 linp 与
 * item 并不对齐。这是因为我们把 linp0 留作
 * 指向 "高键" 项的占位指针；当我们
 * 填满该页后，会让 linp0 指向 itemN 并清除
 * linpN。另一方面，若发现这是最后一页（最右页），
 * 就不动这些 item，而是把 linp 数组滑过去。若
 * 高键需要截断，则删掉偏移 1，并插入
 * 截断后的高键到偏移 1。
 *
 * 'last' 指针表示加入该页的最后一个偏移。
 *
 * 'truncextra' 是 itup 里倒排列表的大小，若有的话。
 * 这条信息留到下一次调用这里时使用。那时在决定
 * 结束本页之前，考虑截掉页上
 * 倒排列表的影响也许有好处。倒排列表
 * 往往相对较大，所以值得费心计入：
 * 截掉将成为高键的那个元组的倒排列表
 * 能省下多少空间（这也许是让该页接近
 * 目标空闲空间的唯一办法）。注意，这只用于
 * 按 fillfactor 衡量的软限制，而不是
 * 关键的硬限制。
 *----------
 */
static void
_bt_buildadd(BTWriteState *wstate, BTPageState *state, IndexTuple itup,
			 Size truncextra)
{
	Page		npage;
	BlockNumber nblkno;
	OffsetNumber last_off;
	Size		last_truncextra;
	Size		pgspc;
	Size		itupsz;
	bool		isleaf;

	/*
	 * 这里适合在索引创建的 B-树装入阶段
	 * 检查取消中断。
	 */
	CHECK_FOR_INTERRUPTS();

	npage = state->btps_page;
	nblkno = state->btps_blkno;
	last_off = state->btps_lastoff;
	last_truncextra = state->btps_lastextra;
	state->btps_lastextra = truncextra;

	pgspc = PageGetFreeSpace(npage);
	itupsz = IndexTupleSize(itup);
	itupsz = MAXALIGN(itupsz);
	/* 由于后缀截断，叶子情形的规则略有不同 */
	isleaf = (state->btps_level == 0);

	/*
	 * 检查新项究竟能否放进当前层的
	 * 一张 B-树页。
	 *
	 * 每个新建的索引都会把堆 TID 当作键空间的一部分，
	 * 因此新高键偶尔必须在 _bt_truncate() 内部
	 * 追加一个堆 TID。这可能让新的枢轴元组比它所派生自的
	 * 原来那个 firstright 元组大一到两个 MAXALIGN()
	 * 对齐单位。v4 处理该问题的办法，是把叶子层上
	 * 允许插入的元组大小上限减小同样的一小截。
	 * 在叶子层强制执行新的 v4+ 上限，在内部层沿用旧上限，
	 * 因为枢轴元组可能需要用到
	 * 这块预留空间。这在内部页上
	 * 不应当失败。
	 */
	if (unlikely(itupsz > BTMaxItemSize(npage)))
		_bt_check_third_page(wstate->index, wstate->heap, isleaf, npage,
							 itup);

	/*
	 * 检查当前页能否放下新项，并在该页是叶子页时为后缀截断
	 * 追加堆 TID 留出空间。
	 *
	 * 可以保证，在结束一张叶子页时至少能放下 2 个非枢轴元组
	 * 再加一个带堆 TID 的高键，因为我们依赖
	 * _bt_check_third_page() 拒绝过大的非枢轴元组。在
	 * 内部页上，由于内部页元组上限更大，总能放下 3 个枢轴元组
	 * （含页高键）。
	 *
	 * 大多数时候，页所谓 "full" 只是说按 fillfactor 的软限制
	 * 已被超过。不过，在开始新页之前，每一页上始终必须留下
	 * 至少两项再加一个高键。若还没有达到最少项数，
	 * 就忽略 fillfactor，继续插入这张 "full" 的当前页。
	 * （注意，施加软限制时，我们有意假定后缀截断既不会
	 * 放大也不会缩小新高键，除非最后那个元组带有
	 * 倒排列表。）
	 */
	Assert(last_truncextra == 0 || isleaf);
	if (pgspc < itupsz + (isleaf ? MAXALIGN(sizeof(ItemPointerData)) : 0) ||
		(pgspc + last_truncextra < state->btps_full && last_off > P_FIRSTKEY))
	{
		/*
		 * 结束该页并把它写出。
		 */
		Page		opage = npage;
		BlockNumber oblkno = nblkno;
		ItemId		ii;
		ItemId		hii;
		IndexTuple	oitup;

		/* 创建同层的新页 */
		npage = _bt_blnewpage(state->btps_level);

		/* 并给它分配一个页位置 */
		nblkno = wstate->btws_pages_alloced++;

		/*
		 * 我们把页上的最后一项复制到新页，然后
		 * 重新安排旧页，使 "最后一项" 成为它的高键，
		 * 而不再是真正的数据项。页上最好已经至少有
		 * 两项，否则该页就不会有有用的
		 * 数据。
		 */
		Assert(last_off > P_FIRSTKEY);
		ii = PageGetItemId(opage, last_off);
		oitup = (IndexTuple) PageGetItem(opage, ii);
		_bt_sortaddtup(npage, ItemIdGetLength(ii), oitup, P_FIRSTKEY,
					   !isleaf);

		/*
		 * 把 'last' 移到 opage 上的高键位置。_bt_blnewpage()
		 * 在最初创建 opage 时已为空行指针分配了空间，
		 * 因此这里只是重新安排页上已分配的空间，并初始化
		 * 高键行指针。（实际上，叶子页还必须把 oitup 换成
		 * oitup 的截断版本，该版本有时比 oitup 更大，但增幅
		 * 从不超过追加一个堆 TID 所需的
		 * 空间。）
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ItemIdSetUnused(ii);	/* 冗余 */
		((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

		if (isleaf)
		{
			IndexTuple	lastleft;
			IndexTuple	truncated;

			/*
			 * 从叶子层的高键上截掉任何不需要的属性。
			 * 只在叶子层这样做，因为内部页中的下行链接
			 * 要么是负无穷项，要么其内容复制自下一层。
			 * 另见：
			 * _bt_split()。
			 *
			 * 我们并不试图偏置分裂点的选择，以使
			 * _bt_truncate() 更有可能截掉更多属性，
			 * 而 _bt_split() 里选用的分裂点则仔细得多。
			 * 即便如此，这里传给 _bt_truncate() 的 lastleft
			 * 和 firstright 元组，在使用去重时至少不会完全
			 * 相等，除非有一大组重复项（另外，唯一索引的
			 * 构建通常很少或没有 spool2 重复项）。分裂点
			 * 位于两个不相等的元组之间时，_bt_truncate()
			 * 会避免在新高键里放入堆 TID，
			 * 这是后缀截断最重要的
			 * 好处。
			 *
			 * 直接用新的截断高键覆盖旧项。
			 * oitup 已经位于元组空间的物理开头，因此这应当
			 * 直接复用现有的元组空间。
			 */
			ii = PageGetItemId(opage, OffsetNumberPrev(last_off));
			lastleft = (IndexTuple) PageGetItem(opage, ii);

			Assert(IndexTupleSize(oitup) > last_truncextra);
			truncated = _bt_truncate(wstate->index, lastleft, oitup,
									 wstate->inskey);
			if (!PageIndexTupleOverwrite(opage, P_HIKEY, (Item) truncated,
										 IndexTupleSize(truncated)))
				elog(ERROR, "failed to add high key to the index page");
			pfree(truncated);

			/* oitup 应继续指向该页的高键 */
			hii = PageGetItemId(opage, P_HIKEY);
			oitup = (IndexTuple) PageGetItem(opage, hii);
		}

		/*
		 * 用低键把旧页链接进它的父页。若还没有父页，
		 * 就必须创建一个；这会增加一个新的 B-树层。
		 */
		if (state->btps_next == NULL)
			state->btps_next = _bt_pagestate(wstate, state->btps_level + 1);

		Assert((BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) <=
				IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
				BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) > 0) ||
			   P_LEFTMOST(BTPageGetOpaque(opage)));
		Assert(BTreeTupleGetNAtts(state->btps_lowkey, wstate->index) == 0 ||
			   !P_LEFTMOST(BTPageGetOpaque(opage)));
		BTreeTupleSetDownLink(state->btps_lowkey, oblkno);
		_bt_buildadd(wstate, state->btps_next, state->btps_lowkey, 0);
		pfree(state->btps_lowkey);

		/*
		 * 保存旧页高键的一份副本。它同时也是新页的
		 * 低键。
		 */
		state->btps_lowkey = CopyIndexTuple(oitup);

		/*
		 * 设置两页的兄弟链接。
		 */
		{
			BTPageOpaque oopaque = BTPageGetOpaque(opage);
			BTPageOpaque nopaque = BTPageGetOpaque(npage);

			oopaque->btpo_next = nblkno;
			nopaque->btpo_prev = oblkno;
			nopaque->btpo_next = P_NONE;	/* 冗余 */
		}

		/*
		 * 写出旧页。我们再也不需要碰它，因此也可以
		 * 释放 opage 工作区。
		 */
		_bt_blwritepage(wstate, opage, oblkno);

		/*
		 * 重置 last_off，使它指向新页
		 */
		last_off = P_FIRSTKEY;
	}

	/*
	 * 到这里，要么原来的页仍是当前页，要么新建了一页
	 * 并成为当前页。无论哪种情况，当前页都一定有空间
	 * 放下新项。
	 *
	 * 若新项是它所在页的第一项，它也必定是整个这一层的
	 * 第一项。在同层的后续页上，页的低键会由上面的代码
	 * 从上一页复制过来。这里则改为生成一个负无穷
	 * 低键。
	 */
	if (last_off == P_HIKEY)
	{
		Assert(state->btps_lowkey == NULL);
		state->btps_lowkey = palloc0(sizeof(IndexTupleData));
		state->btps_lowkey->t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(state->btps_lowkey, 0, false);
	}

	/*
	 * 把新项加入当前页。
	 */
	last_off = OffsetNumberNext(last_off);
	_bt_sortaddtup(npage, itupsz, itup, last_off,
				   !isleaf && last_off == P_FIRSTKEY);

	state->btps_page = npage;
	state->btps_blkno = nblkno;
	state->btps_lastoff = last_off;
}

/*
 * 定稿待定的倒排列表元组，并把它加入索引。最终元组
 * 基于保存的基元组，以及保存的堆 TIDs。
 *
 * 这几乎与 _bt_dedup_finish_pending() 相同，但它用
 * _bt_buildadd() 加入新元组。
 */
static void
_bt_sort_dedup_finish_pending(BTWriteState *wstate, BTPageState *state,
							  BTDedupState dstate)
{
	Assert(dstate->nitems > 0);

	if (dstate->nitems == 1)
		_bt_buildadd(wstate, state, dstate->base, 0);
	else
	{
		IndexTuple	postingtuple;
		Size		truncextra;

		/* 构造带倒排列表的元组 */
		postingtuple = _bt_form_posting(dstate->base,
										dstate->htids,
										dstate->nhtids);
		/* 计算倒排列表开销 */
		truncextra = IndexTupleSize(postingtuple) -
			BTreeTupleGetPostingOffset(postingtuple);

		_bt_buildadd(wstate, state, postingtuple, truncextra);
		pfree(postingtuple);
	}

	dstate->nmaxitems = 0;
	dstate->nhtids = 0;
	dstate->nitems = 0;
	dstate->phystupsize = 0;
}

/*
 * 把已完成的 B-树写完。
 */
static void
_bt_uppershutdown(BTWriteState *wstate, BTPageState *state)
{
	BTPageState *s;
	BlockNumber rootblkno = P_NONE;
	uint32		rootlevel = 0;
	Page		metapage;

	/*
	 * 这个循环的每一轮完成树的又一层。
	 */
	for (s = state; s != NULL; s = s->btps_next)
	{
		BlockNumber blkno;
		BTPageOpaque opaque;

		blkno = s->btps_blkno;
		opaque = BTPageGetOpaque(s->btps_page);

		/*
		 * 必须把本层的最后一页链接到某处。
		 *
		 * 若已在顶层，它就是根，因此把它挂到元数据页上。
		 * 否则，用它的低键在父页中为它加一项。
		 * 这可能导致父层的最后一页分裂，但那不是问题——
		 * 我们还没有处理到那一页。
		 */
		if (s->btps_next == NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			rootblkno = blkno;
			rootlevel = s->btps_level;
		}
		else
		{
			Assert((BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) <=
					IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
					BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) > 0) ||
				   P_LEFTMOST(opaque));
			Assert(BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) == 0 ||
				   !P_LEFTMOST(opaque));
			BTreeTupleSetDownLink(s->btps_lowkey, blkno);
			_bt_buildadd(wstate, s->btps_next, s->btps_lowkey, 0);
			pfree(s->btps_lowkey);
			s->btps_lowkey = NULL;
		}

		/*
		 * 这是最右页，因此需要把 ItemId 数组往回
		 * 滑动一槽。然后就可以把该页转储出去。
		 */
		_bt_slideleft(s->btps_page);
		_bt_blwritepage(wstate, s->btps_page, s->btps_blkno);
		s->btps_page = NULL;	/* writepage 已释放工作区 */
	}

	/*
	 * 作为该过程的最后一步，构造元数据页并让它指向
	 * 新根（除非完全没有数据，那时它被设为指向 "P_NONE"）。
	 * 通过在元数据页中填入有效的魔数，把索引变为
	 * "valid" 状态。
	 */
	metapage = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
	_bt_initmetapage(metapage, rootblkno, rootlevel,
					 wstate->inskey->allequalimage);
	_bt_blwritepage(wstate, metapage, BTREE_METAPAGE);
}

/*
 * 按正确的排序顺序从 tuplesort 读取元组，并把它们装入
 * B-树叶子。
 */
static void
_bt_load(BTWriteState *wstate, BTSpool *btspool, BTSpool *btspool2)
{
	BTPageState *state = NULL;
	bool		merge = (btspool2 != NULL);
	IndexTuple	itup,
				itup2 = NULL;
	bool		load1;
	TupleDesc	tupdes = RelationGetDescr(wstate->index);
	int			i,
				keysz = IndexRelationGetNumberOfKeyAttributes(wstate->index);
	SortSupport sortKeys;
	int64		tuples_done = 0;
	bool		deduplicate;

	deduplicate = wstate->inskey->allequalimage && !btspool->isunique &&
		BTGetDeduplicateItems(wstate->index);

	if (merge)
	{
		/*
		 * 还存在另一个用于死元组的 BTSpool。现在必须合并
		 * btspool 与 btspool2。
		 */

		/* 合并的准备工作 */
		itup = tuplesort_getindextuple(btspool->sortstate, true);
		itup2 = tuplesort_getindextuple(btspool2->sortstate, true);

		/* 为每一列准备 SortSupport 数据 */
		sortKeys = (SortSupport) palloc0(keysz * sizeof(SortSupportData));

		for (i = 0; i < keysz; i++)
		{
			SortSupport sortKey = sortKeys + i;
			ScanKey		scanKey = wstate->inskey->scankeys + i;
			int16		strategy;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = scanKey->sk_collation;
			sortKey->ssup_nulls_first =
				(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
			sortKey->ssup_attno = scanKey->sk_attno;
			/* 这里不支持缩写 */
			sortKey->abbreviate = false;

			Assert(sortKey->ssup_attno != 0);

			strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
				BTGreaterStrategyNumber : BTLessStrategyNumber;

			PrepareSortSupportFromIndexRel(wstate->index, strategy, sortKey);
		}

		for (;;)
		{
			load1 = true;		/* 接下来装入 BTSpool？ */
			if (itup2 == NULL)
			{
				if (itup == NULL)
					break;
			}
			else if (itup != NULL)
			{
				int32		compare = 0;

				for (i = 1; i <= keysz; i++)
				{
					SortSupport entry;
					Datum		attrDatum1,
								attrDatum2;
					bool		isNull1,
								isNull2;

					entry = sortKeys + i - 1;
					attrDatum1 = index_getattr(itup, i, tupdes, &isNull1);
					attrDatum2 = index_getattr(itup2, i, tupdes, &isNull2);

					compare = ApplySortComparator(attrDatum1, isNull1,
												  attrDatum2, isNull2,
												  entry);
					if (compare > 0)
					{
						load1 = false;
						break;
					}
					else if (compare < 0)
						break;
				}

				/*
				 * 若键值相等，则按 ItemPointer 排序。这对 B-树索引
				 * 是必需的，因为堆 TID 被当作隐式的最后一个键属性，
				 * 以保证索引中的所有键在物理上
				 * 唯一。
				 */
				if (compare == 0)
				{
					compare = ItemPointerCompare(&itup->t_tid, &itup2->t_tid);
					Assert(compare != 0);
					if (compare > 0)
						load1 = false;
				}
			}
			else
				load1 = false;

			/* 见到第一个元组时，创建第一张索引页 */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			if (load1)
			{
				_bt_buildadd(wstate, state, itup, 0);
				itup = tuplesort_getindextuple(btspool->sortstate, true);
			}
			else
			{
				_bt_buildadd(wstate, state, itup2, 0);
				itup2 = tuplesort_getindextuple(btspool2->sortstate, true);
			}

			/* 报告进度 */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
		pfree(sortKeys);
	}
	else if (deduplicate)
	{
		/* 不必合并，去重成倒排列表 */
		BTDedupState dstate;

		dstate = (BTDedupState) palloc(sizeof(BTDedupStateData));
		dstate->deduplicate = true; /* 未使用 */
		dstate->nmaxitems = 0;	/* 未使用 */
		dstate->maxpostingsize = 0; /* 稍后设置 */
		/* 当前待定倒排列表基元组的元数据 */
		dstate->base = NULL;
		dstate->baseoff = InvalidOffsetNumber;	/* 未使用 */
		dstate->basetupsize = 0;
		/* 当前待定倒排列表 TIDs 的元数据 */
		dstate->htids = NULL;
		dstate->nhtids = 0;
		dstate->nitems = 0;
		dstate->phystupsize = 0;	/* 未使用 */
		dstate->nintervals = 0; /* 未使用 */

		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* 见到第一个元组时，创建第一张索引页 */
			if (state == NULL)
			{
				state = _bt_pagestate(wstate, 0);

				/*
				 * 把倒排列表元组的大小限制为我们希望留在页上的
				 * 空间的 1/10，再加上最后一项行指针所占的空间。
				 * 这等于 fillfactor 为 90 时，希望留在每张叶子页上的
				 * 空间，从而在碰巧存在大量重复项时，空间利用率
				 * 能够接近 fillfactor%。（这会使更高的叶子
				 * fillfactor 设置，在构建含许多重复项的索引时失效，
				 * 但用少数非常大的元组把叶子页塞满，
				 * 看起来并不是
				 * 一个有用的
				 * 目标。）
				 */
				dstate->maxpostingsize = MAXALIGN_DOWN((BLCKSZ * 10 / 100)) -
					sizeof(ItemIdData);
				Assert(dstate->maxpostingsize <= BTMaxItemSize(state->btps_page) &&
					   dstate->maxpostingsize <= INDEX_SIZE_MASK);
				dstate->htids = palloc(dstate->maxpostingsize);

				/* 用 itup 的副本开始新的待定倒排列表 */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}
			else if (_bt_keep_natts_fast(wstate->index, dstate->base,
										 itup) > keysz &&
					 _bt_dedup_save_htid(dstate, itup))
			{
				/*
				 * 元组等于待定倒排列表的基元组。来自 itup 的堆
				 * TID 已保存在状态中。
				 */
			}
			else
			{
				/*
				 * 元组不等于待定倒排列表元组，或者
				 * _bt_dedup_save_htid() 选择不把当前项合并进
				 * 待定倒排列表。
				 */
				_bt_sort_dedup_finish_pending(wstate, state, dstate);
				pfree(dstate->base);

				/* 用 itup 的副本开始新的待定倒排列表 */
				_bt_dedup_start_pending(dstate, CopyIndexTuple(itup),
										InvalidOffsetNumber);
			}

			/* 报告进度 */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}

		if (state)
		{
			/*
			 * 处理最后一项（当 tuplesort 返回了一个或多个元组时，
			 * 必定存在最后一项）
			 */
			_bt_sort_dedup_finish_pending(wstate, state, dstate);
			pfree(dstate->base);
			pfree(dstate->htids);
		}

		pfree(dstate);
	}
	else
	{
		/* 合并与去重都不需要 */
		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* 见到第一个元组时，创建第一张索引页 */
			if (state == NULL)
				state = _bt_pagestate(wstate, 0);

			_bt_buildadd(wstate, state, itup, 0);

			/* 报告进度 */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
	}

	/* 结束最后几页并写出元数据页 */
	_bt_uppershutdown(wstate, state);

	/*
	 * 即使已经把索引页写入 WAL，仍必须 fsync 索引文件。
	 * 由于我们在共享缓冲区之外构建，构建期间发生的
	 * CHECKPOINT 无法把先前写出的数据刷到磁盘（它甚至
	 * 不知道该索引存在）。此后若崩溃，会从该检查点重放 WAL，
	 * 因此不会重放我们更早的 WAL 记录。若这里不对那些页
	 * 做 fsync，崩溃发生时它们可能仍然
	 * 不在磁盘上。
	 */
	if (wstate->btws_use_wal)
		smgrimmedsync(RelationGetSmgr(wstate->index), MAIN_FORKNUM);
}

/*
 * 创建并行上下文，并为领导进程启动工作进程。
 *
 * buildstate 参数应当已经初始化（假脱机中的 tuplesort
 * 状态除外，它稍后可能根据这里最初建立的共享状态
 * 来创建）。
 *
 * isconcurrent 表示该操作是否为 CREATE INDEX CONCURRENTLY。
 *
 * request 是要启动的并行工作进程的目标数目。
 *
 * 设置 buildstate 的 BTLeader。调用者必须在索引构建的
 * 最末尾把它传给 _bt_end_parallel()，以关闭并行模式。
 * 若连一个工作进程都无法启动，则不会设置它，
 * 调用者应当继续做串行索引构建。
 */
static void
_bt_begin_parallel(BTBuildState *buildstate, bool isconcurrent, int request)
{
	ParallelContext *pcxt;
	int			scantuplesortstates;
	Snapshot	snapshot;
	Size		estbtshared;
	Size		estsort;
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	BTSpool    *btspool = buildstate->spool;
	BTLeader   *btleader = (BTLeader *) palloc0(sizeof(BTLeader));
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	bool		leaderparticipates = true;
	int			querylen;

#ifdef DISABLE_LEADER_PARTICIPATION
	leaderparticipates = false;
#endif

	/*
	 * 进入并行模式，并创建 B-树索引并行构建的
	 * 上下文
	 */
	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("postgres", "_bt_parallel_build_main",
								 request);

	scantuplesortstates = leaderparticipates ? request + 1 : request;

	/*
	 * 准备扫描基础关系。在普通索引构建中，我们使用
	 * SnapshotAny，因为必须取出全部元组并自行做时间
	 * 资格检查（因为必须为 RECENTLY_DEAD 元组建索引）。在
	 * 并发构建中，我们取一个常规 MVCC 快照，并索引
	 * 按照该快照仍然存活的内容。
	 */
	if (!isconcurrent)
		snapshot = SnapshotAny;
	else
		snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * 估算我们自己的 PARALLEL_KEY_BTREE_SHARED 工作区，以及
	 * PARALLEL_KEY_TUPLESORT tuplesort 工作区的大小
	 */
	estbtshared = _bt_parallel_estimate_shared(btspool->heap, snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, estbtshared);
	estsort = tuplesort_estimate_shared(scantuplesortstates);
	shm_toc_estimate_chunk(&pcxt->estimator, estsort);

	/*
	 * 唯一索引情形需要第二个假脱机，因此可能还要为它
	 * 再算上一块共享工作区——PARALLEL_KEY_TUPLESORT_SPOOL2
	 */
	if (!btspool->isunique)
		shm_toc_estimate_keys(&pcxt->estimator, 2);
	else
	{
		shm_toc_estimate_chunk(&pcxt->estimator, estsort);
		shm_toc_estimate_keys(&pcxt->estimator, 3);
	}

	/*
	 * 估算 WalUsage 与 BufferUsage 的空间——PARALLEL_KEY_WAL_USAGE
	 * 和 PARALLEL_KEY_BUFFER_USAGE。
	 *
	 * 若没有已加载的扩展关心这些，本来可以跳过。我们
	 * 无法知道是否有人在查看 pgWalUsage 或 pgBufferUsage，
	 * 因此无条件地做这件事。
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 最后，估算 PARALLEL_KEY_QUERY_TEXT 的空间 */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* 让编译器安静 */

	/* 各方都已申请空间，现在创建 DSM */
	InitializeParallelDSM(pcxt);

	/* 若没有可用的 DSM 段，则退出（改为串行构建） */
	if (pcxt->seg == NULL)
	{
		if (IsMVCCSnapshot(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	/* 存入已预留空间的共享构建状态 */
	btshared = (BTShared *) shm_toc_allocate(pcxt->toc, estbtshared);
	/* 初始化不可变状态 */
	btshared->heaprelid = RelationGetRelid(btspool->heap);
	btshared->indexrelid = RelationGetRelid(btspool->index);
	btshared->isunique = btspool->isunique;
	btshared->nulls_not_distinct = btspool->nulls_not_distinct;
	btshared->isconcurrent = isconcurrent;
	btshared->scantuplesortstates = scantuplesortstates;
	ConditionVariableInit(&btshared->workersdonecv);
	SpinLockInit(&btshared->mutex);
	/* 初始化可变状态 */
	btshared->nparticipantsdone = 0;
	btshared->reltuples = 0.0;
	btshared->havedead = false;
	btshared->indtuples = 0.0;
	btshared->brokenhotchain = false;
	table_parallelscan_initialize(btspool->heap,
								  ParallelTableScanFromBTShared(btshared),
								  snapshot);

	/*
	 * 存入已预留空间的、tuplesort 私有的共享状态。
	 * 然后用 tuplesort 例程初始化不透明状态。
	 */
	sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
	tuplesort_initialize_shared(sharedsort, scantuplesortstates,
								pcxt->seg);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BTREE_SHARED, btshared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT, sharedsort);

	/* 唯一索引还需要第二个假脱机及其共享状态 */
	if (!btspool->isunique)
		sharedsort2 = NULL;
	else
	{
		/*
		 * 存入额外的、tuplesort 私有的共享状态，
		 * 其空间已经预留。然后用 tuplesort 例程初始化
		 * 不透明状态。
		 */
		sharedsort2 = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
		tuplesort_initialize_shared(sharedsort2, scantuplesortstates,
									pcxt->seg);

		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT_SPOOL2, sharedsort2);
	}

	/* 为工作进程存放查询字符串 */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, sharedquery);
	}

	/*
	 * 为每个工作进程的 WalUsage 和 BufferUsage 分配空间；
	 * 不必初始化。
	 */
	walusage = shm_toc_allocate(pcxt->toc,
								mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, walusage);
	bufferusage = shm_toc_allocate(pcxt->toc,
								   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, bufferusage);

	/* 启动工作进程，并为领导进程/调用者保存状态 */
	LaunchParallelWorkers(pcxt);
	btleader->pcxt = pcxt;
	btleader->nparticipanttuplesorts = pcxt->nworkers_launched;
	if (leaderparticipates)
		btleader->nparticipanttuplesorts++;
	btleader->btshared = btshared;
	btleader->sharedsort = sharedsort;
	btleader->sharedsort2 = sharedsort2;
	btleader->snapshot = snapshot;
	btleader->walusage = walusage;
	btleader->bufferusage = bufferusage;

	/* 若没有工作进程成功启动，则退出（改为串行构建） */
	if (pcxt->nworkers_launched == 0)
	{
		_bt_end_parallel(btleader);
		return;
	}

	/* 既然确定将进行并行构建，保存领导进程状态 */
	buildstate->btleader = btleader;

	/* 我们自己加入堆扫描 */
	if (leaderparticipates)
		_bt_leader_participate_as_worker(buildstate);

	/*
	 * 返回时，调用者需要等待所有已启动的工作进程。确保
	 * 未能启动的情形不会永远挂起。
	 */
	WaitForParallelWorkersToAttach(pcxt);
}

/*
 * 关闭工作进程，销毁并行上下文，并结束并行模式。
 */
static void
_bt_end_parallel(BTLeader *btleader)
{
	int			i;

	/* 关闭工作进程 */
	WaitForParallelWorkersToFinish(btleader->pcxt);

	/*
	 * 接下来累计 WAL 用量。（这必须等工作进程结束，
	 * 否则得到的数据可能不完整。）
	 */
	for (i = 0; i < btleader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&btleader->bufferusage[i], &btleader->walusage[i]);

	/* 若使用了 MVCC 快照，释放最后一次引用 */
	if (IsMVCCSnapshot(btleader->snapshot))
		UnregisterSnapshot(btleader->snapshot);
	DestroyParallelContext(btleader->pcxt);
	ExitParallelMode();
}

/*
 * 返回并行 B-树索引构建所需的共享内存大小，依据其
 * 并行扫描将使用的快照来计算。
 */
static Size
_bt_parallel_estimate_shared(Relation heap, Snapshot snapshot)
{
	/* 关于为何使用 BUFFERALIGN，参见 shm_toc_allocate */
	return add_size(BUFFERALIGN(sizeof(BTShared)),
					table_parallelscan_estimate(heap, snapshot));
}

/*
 * 在领导进程中等待堆扫描结束。
 *
 * 调用时，由 _bt_begin_parallel() 启动的并行堆扫描
 * 已在工作进程中进行（若领导进程作为工作进程参与，
 * 我们到达这里时工作进程应当正要结束）。
 *
 * 填写 ambuild 统计所需的字段，并让调用者设置
 * 表示某个工作进程遇到断裂 HOT 链的字段。
 *
 * 返回扫描到的堆元组总数。
 */
static double
_bt_parallel_heapscan(BTBuildState *buildstate, bool *brokenhotchain)
{
	BTShared   *btshared = buildstate->btleader->btshared;
	int			nparticipanttuplesorts;
	double		reltuples;

	nparticipanttuplesorts = buildstate->btleader->nparticipanttuplesorts;
	for (;;)
	{
		SpinLockAcquire(&btshared->mutex);
		if (btshared->nparticipantsdone == nparticipanttuplesorts)
		{
			buildstate->havedead = btshared->havedead;
			buildstate->indtuples = btshared->indtuples;
			*brokenhotchain = btshared->brokenhotchain;
			reltuples = btshared->reltuples;
			SpinLockRelease(&btshared->mutex);
			break;
		}
		SpinLockRelease(&btshared->mutex);

		ConditionVariableSleep(&btshared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}

	ConditionVariableCancelSleep();

	return reltuples;
}

/*
 * 在领导进程中，作为并行工作进程参与。
 */
static void
_bt_leader_participate_as_worker(BTBuildState *buildstate)
{
	BTLeader   *btleader = buildstate->btleader;
	BTSpool    *leaderworker;
	BTSpool    *leaderworker2;
	int			sortmem;

	/* 分配内存并初始化私有假脱机 */
	leaderworker = (BTSpool *) palloc0(sizeof(BTSpool));
	leaderworker->heap = buildstate->spool->heap;
	leaderworker->index = buildstate->spool->index;
	leaderworker->isunique = buildstate->spool->isunique;
	leaderworker->nulls_not_distinct = buildstate->spool->nulls_not_distinct;

	/* 若需要则初始化第二个假脱机 */
	if (!btleader->btshared->isunique)
		leaderworker2 = NULL;
	else
	{
		/* 为工作进程自己的私有次假脱机分配内存 */
		leaderworker2 = (BTSpool *) palloc0(sizeof(BTSpool));

		/* 初始化工作进程自己的次假脱机 */
		leaderworker2->heap = leaderworker->heap;
		leaderworker2->index = leaderworker->index;
		leaderworker2->isunique = false;
	}

	/*
	 * 分配 maintenance_work_mem 时不妨使用可靠的数字
	 * （若请求数目的工作进程并未全部启动，这个数会
	 * 比其他工作进程的略高一些）。
	 */
	sortmem = maintenance_work_mem / btleader->nparticipanttuplesorts;

	/* 执行所有参与者共用的工作 */
	_bt_parallel_scan_and_sort(leaderworker, leaderworker2, btleader->btshared,
							   btleader->sharedsort, btleader->sharedsort2,
							   sortmem, true);

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Leader Partial Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */
}

/*
 * 在已启动的并行进程中执行工作。
 */
void
_bt_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	char	   *sharedquery;
	BTSpool    *btspool;
	BTSpool    *btspool2;
	BTShared   *btshared;
	Sharedsort *sharedsort;
	Sharedsort *sharedsort2;
	Relation	heapRel;
	Relation	indexRel;
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
	int			sortmem;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif							/* BTREE_BUILD_STATS */

	/*
	 * 能够设置到并行工作进程上的状态标志只有
	 * PROC_IN_SAFE_IC。
	 */
	Assert((MyProc->statusFlags == 0) ||
		   (MyProc->statusFlags == PROC_IN_SAFE_IC));

	/* 先为各个工作进程设置 debug_query_string */
	sharedquery = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;

	/* 报告来自领导进程的查询字符串 */
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* 查找 nbtree 共享状态 */
	btshared = shm_toc_lookup(toc, PARALLEL_KEY_BTREE_SHARED, false);

	/* 用 index.c 已知会取得的锁模式打开关系 */
	if (!btshared->isconcurrent)
	{
		heapLockmode = ShareLock;
		indexLockmode = AccessExclusiveLock;
	}
	else
	{
		heapLockmode = ShareUpdateExclusiveLock;
		indexLockmode = RowExclusiveLock;
	}

	/* 在工作进程内打开关系 */
	heapRel = table_open(btshared->heaprelid, heapLockmode);
	indexRel = index_open(btshared->indexrelid, indexLockmode);

	/* 初始化工作进程自己的假脱机 */
	btspool = (BTSpool *) palloc0(sizeof(BTSpool));
	btspool->heap = heapRel;
	btspool->index = indexRel;
	btspool->isunique = btshared->isunique;
	btspool->nulls_not_distinct = btshared->nulls_not_distinct;

	/* 查找 tuplesort.c 私有的共享状态 */
	sharedsort = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT, false);
	tuplesort_attach_shared(sharedsort, seg);
	if (!btshared->isunique)
	{
		btspool2 = NULL;
		sharedsort2 = NULL;
	}
	else
	{
		/* 为工作进程自己的私有次假脱机分配内存 */
		btspool2 = (BTSpool *) palloc0(sizeof(BTSpool));

		/* 初始化工作进程自己的次假脱机 */
		btspool2->heap = btspool->heap;
		btspool2->index = btspool->index;
		btspool2->isunique = false;
		/* 查找 tuplesort.c 私有的共享状态 */
		sharedsort2 = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT_SPOOL2, false);
		tuplesort_attach_shared(sharedsort2, seg);
	}

	/* 准备在并行执行期间跟踪缓冲区用量 */
	InstrStartParallelQuery();

	/* 对假脱机排序，可能还包括 spool2 */
	sortmem = maintenance_work_mem / btshared->scantuplesortstates;
	_bt_parallel_scan_and_sort(btspool, btspool2, btshared, sharedsort,
							   sharedsort2, sortmem, false);

	/* 报告并行执行期间的 WAL/缓冲区用量 */
	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Worker Partial Spool) STATISTICS");
		ResetUsage();
	}
#endif							/* BTREE_BUILD_STATS */

	index_close(indexRel, indexLockmode);
	table_close(heapRel, heapLockmode);
}

/*
 * 执行并行排序中一个工作进程承担的部分。
 *
 * 这为传入的 btspool 生成一个 tuplesort；若还需要第二个
 * btspool（即唯一索引构建），则再生成第二个 tuplesort
 * 状态。调用时，假脱机的其他字段应当都已设置好。
 *
 * sortmem 是每个工作进程使用的工作内存量，
 * 以 KB 为单位。
 *
 * 返回时工作进程已经做完，只需释放资源。
 */
static void
_bt_parallel_scan_and_sort(BTSpool *btspool, BTSpool *btspool2,
						   BTShared *btshared, Sharedsort *sharedsort,
						   Sharedsort *sharedsort2, int sortmem, bool progress)
{
	SortCoordinate coordinate;
	BTBuildState buildstate;
	TableScanDesc scan;
	double		reltuples;
	IndexInfo  *indexInfo;

	/* 初始化本地 tuplesort 协调状态 */
	coordinate = palloc0(sizeof(SortCoordinateData));
	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = sharedsort;

	/* 开始 "部分" tuplesort */
	btspool->sortstate = tuplesort_begin_index_btree(btspool->heap,
													 btspool->index,
													 btspool->isunique,
													 btspool->nulls_not_distinct,
													 sortmem, coordinate,
													 TUPLESORT_NONE);

	/*
	 * 与串行情形一样，可能还有第二个假脱机。若有，就需要
	 * 第二个专用的 spool2 部分 tuplesort。
	 */
	if (btspool2)
	{
		SortCoordinate coordinate2;

		/*
		 * 我们预期第二个（用于死元组）不会装得很满，
		 * 因此只给它 work_mem（除非对该工作进程而言 sortmem
		 * 更小）。工作进程通常允许独立分配
		 * work_mem。
		 */
		coordinate2 = palloc0(sizeof(SortCoordinateData));
		coordinate2->isWorker = true;
		coordinate2->nParticipants = -1;
		coordinate2->sharedsort = sharedsort2;
		btspool2->sortstate =
			tuplesort_begin_index_btree(btspool->heap, btspool->index, false, false,
										Min(sortmem, work_mem), coordinate2,
										false);
	}

	/* 为 _bt_build_callback() 填好 buildstate */
	buildstate.isunique = btshared->isunique;
	buildstate.nulls_not_distinct = btshared->nulls_not_distinct;
	buildstate.havedead = false;
	buildstate.heap = btspool->heap;
	buildstate.spool = btspool;
	buildstate.spool2 = btspool2;
	buildstate.indtuples = 0;
	buildstate.btleader = NULL;

	/* 加入并行扫描 */
	indexInfo = BuildIndexInfo(btspool->index);
	indexInfo->ii_Concurrent = btshared->isconcurrent;
	scan = table_beginscan_parallel(btspool->heap,
									ParallelTableScanFromBTShared(btshared));
	reltuples = table_index_build_scan(btspool->heap, btspool->index, indexInfo,
									   true, progress, _bt_build_callback,
									   (void *) &buildstate, scan);

	/* 执行本工作进程承担的那部分排序 */
	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2)
	{
		if (progress)
			pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
										 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	/*
	 * 完成。记录 ambuild 统计信息，以及是否遇到断裂的
	 * HOT 链。
	 */
	SpinLockAcquire(&btshared->mutex);
	btshared->nparticipantsdone++;
	btshared->reltuples += reltuples;
	if (buildstate.havedead)
		btshared->havedead = true;
	btshared->indtuples += buildstate.indtuples;
	if (indexInfo->ii_BrokenHotChain)
		btshared->brokenhotchain = true;
	SpinLockRelease(&btshared->mutex);

	/* 通知领导进程 */
	ConditionVariableSignal(&btshared->workersdonecv);

	/* 可以立即结束 tuplesort */
	tuplesort_end(btspool->sortstate);
	if (btspool2)
		tuplesort_end(btspool2->sortstate);
}
