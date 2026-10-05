/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  并发（"惰性"）清理。
 *
 * 清理占用的主要空间，是存放待从索引中移除的死 TIDs
 * 数组。我们希望即使用有限的内存，也能清理
 * 最大的关系。为此，我们对一次能够跟踪的
 * TID 数量设置上限。
 *
 * 我们最多愿意使用 maintenance_work_mem（或者
 * autovacuum_work_mem）的内存来跟踪死 TID。起初
 * 按该大小分配一个 TID 数组，上限还取决于
 * 表大小（该限制确保不会为清理小表而无谓分配巨大区域）。
 * 若数组即将溢出，就必须调用
 * lazy_vacuum 来清理索引（并清理我们已经剪枝的页面）。
 * 这样就能释放专用于存放死 TID 的内存。
 *
 * 实践中 VACUUM 往往能走完对目标
 * 堆关系的初始扫描，而不会耗尽存放 TID 的空间。这意味着
 * 初始扫描结束后只需要调用一次 lazy_vacuum，
 * 即可完成。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/vacuumlazy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/timestamp.h"


/*
 * 空间/时间权衡参数：这些需要做成可由用户调节的吗？
 *
 * 要考虑截断该关系，我们希望至少有
 * REL_TRUNCATE_MINIMUM 或 (relsize / REL_TRUNCATE_FRACTION)（取
 * 较小者）那么多可能可释放的页。
 */
#define REL_TRUNCATE_MINIMUM	1000
#define REL_TRUNCATE_FRACTION	16

/*
 * 截断加锁启发式的计时参数。
 *
 * 这些没有作为可由用户调节的 GUC 暴露出来，因为看起来
 * 改进的潜力还不足以抵消
 * 支持它们的代价。
 */
#define VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL		20	/* 毫秒 */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL		50	/* 毫秒 */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT			5000	/* 毫秒 */

/*
 * 控制是否把绕过索引清理和堆
 * 清理当作一种优化的阈值
 */
#define BYPASS_THRESHOLD_PAGES	0.02	/* 即 rel_pages 的 2% */

/*
 * 每再扫描 4GB 的页，就做一次故障安全检查。
 * （注意这里有意保持为 2 的幂，通常是 2^19。）
 */
#define FAILSAFE_EVERY_PAGES \
	((BlockNumber) (((uint64) 4 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * 当表没有索引时，大约每 8GB 清理一次 FSM
 * （不会精确，因为我们只在处理完含有可移除元组的堆页之后
 * 才清理 FSM）。有索引时忽略这一点，
 * 并在每一趟索引/堆清理之后清理 FSM。
 */
#define VACUUM_FSM_EVERY_PAGES \
	((BlockNumber) (((uint64) 8 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * 在考虑跳过可见性映射中标记为干净的页之前，
 * 我们必须已经见过至少这么多干净页。
 */
#define SKIP_PAGES_THRESHOLD	((BlockNumber) 32)

/*
 * 惰性清理反向截断扫描的预取窗口大小。
 * 必须是 2 的幂。
 */
#define PREFETCH_SIZE			((BlockNumber) 32)

/*
 * 检查我们是否处于并行清理中的宏。若为真，则处于
 * 并行模式，且 DSM 段已初始化。
 */
#define ParallelVacuumIsActive(vacrel) ((vacrel)->pvs != NULL)

/* 报告错误上下文的清理阶段。 */
typedef enum
{
	VACUUM_ERRCB_PHASE_UNKNOWN,
	VACUUM_ERRCB_PHASE_SCAN_HEAP,
	VACUUM_ERRCB_PHASE_VACUUM_INDEX,
	VACUUM_ERRCB_PHASE_VACUUM_HEAP,
	VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
	VACUUM_ERRCB_PHASE_TRUNCATE
} VacErrPhase;

typedef struct LVRelState
{
	/* 目标堆关系及其索引 */
	Relation	rel;
	Relation   *indrels;
	int			nindexes;

	/* 缓冲区访问策略与并行清理状态 */
	BufferAccessStrategy bstrategy;
	ParallelVacuumState *pvs;

	/* 积极 VACUUM？（必须把 relfrozenxid 设为 >= FreezeLimit） */
	bool		aggressive;
	/* 用可见性映射来跳过？（由 DISABLE_PAGE_SKIPPING 禁用） */
	bool		skipwithvm;
	/* 考虑索引清理绕过优化？ */
	bool		consider_bypass_optimization;

	/* 正在做索引清理、索引善后、关系截断？ */
	bool		do_index_vacuuming;
	bool		do_index_cleanup;
	bool		do_rel_truncate;

	/* VACUUM 操作用于冻结和剪枝的截止值 */
	struct VacuumCutoffs cutoffs;
	GlobalVisState *vistest;
	/* 跟踪现存最旧的 XID/MXID，以便设置 relfrozenxid/relminmxid */
	TransactionId NewRelfrozenXid;
	MultiXactId NewRelminMxid;
	bool		skippedallvis;

	/* 错误报告状态 */
	char	   *dbname;
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;		/* 当前索引名 */
	BlockNumber blkno;			/* 仅用于堆操作 */
	OffsetNumber offnum;		/* 仅用于堆操作 */
	VacErrPhase phase;
	bool		verbose;		/* VACUUM VERBOSE？ */

	/*
	 * dead_items 存放其索引元组将由索引
	 * 清理删除的 TIDs。每个 TID 指向堆页上一个 LP_DEAD 行指针，
	 * 该堆页已由 lazy_scan_prune 处理。lazy_vacuum_heap_rel
	 * 也需要它，后者在第二次堆扫描中把同样的 LP_DEAD 行指针
	 * 标为 LP_UNUSED。
	 */
	VacDeadItems *dead_items;	/* 我们将删除其索引元组的 TIDs */
	BlockNumber rel_pages;		/* 总页数 */
	BlockNumber scanned_pages;	/* 已检查页数（未经由 VM 跳过） */
	BlockNumber removed_pages;	/* 关系截断移除的页数 */
	BlockNumber frozen_pages;	/* 含有新冻结元组的页数 */
	BlockNumber lpdead_item_pages;	/* 含有 LP_DEAD 项的页数 */
	BlockNumber missed_dead_pages;	/* 含有遗漏死元组的页数 */
	BlockNumber nonempty_pages; /* 实际上是最后一个非空页 + 1 */

	/* 我们为表输出的统计信息 */
	double		new_rel_tuples; /* 新估计的元组总数 */
	double		new_live_tuples;	/* 新估计的活元组总数 */
	/* 索引 AM 输出的统计信息 */
	IndexBulkDeleteResult **indstats;

	/* 测量计数器 */
	int			num_index_scans;
	/* 以下计数器仅针对 scanned_pages */
	int64		tuples_deleted; /* 从表中删除的数目 */
	int64		tuples_frozen;	/* 新冻结的数目 */
	int64		lpdead_items;	/* 从索引中删除的数目 */
	int64		live_tuples;	/* 剩余的活元组数 */
	int64		recently_dead_tuples;	/* 已死但尚不可移除的数目 */
	int64		missed_dead_tuples; /* 可移除但未移除的数目 */
} LVRelState;

/*
 * lazy_scan_prune() 返回的状态
 */
typedef struct LVPagePruneState
{
	bool		hastup;			/* 此页使关系截断不安全？ */
	bool		has_lpdead_items;	/* 包括已有的 LP_DEAD 项 */

	/*
	 * 该状态描述剪枝和冻结之后应为该页设置的
	 * 正确 VM 位。all_visible 蕴含 !has_lpdead_items，但除非
	 * all_visible 也设为真，否则不要信任 all_frozen 的结果。
	 */
	bool		all_visible;	/* 每一项对所有事务都可见？ */
	bool		all_frozen;		/* 前提是 all_visible 也为真 */
	TransactionId visibility_cutoff_xid;	/* 用于恢复冲突 */
} LVPagePruneState;

/* 用于保存和恢复清理错误信息的结构体。 */
typedef struct LVSavedErrInfo
{
	BlockNumber blkno;
	OffsetNumber offnum;
	VacErrPhase phase;
} LVSavedErrInfo;


/* 非导出函数原型 */
static void lazy_scan_heap(LVRelState *vacrel);
static BlockNumber lazy_scan_skip(LVRelState *vacrel, Buffer *vmbuffer,
								  BlockNumber next_block,
								  bool *next_unskippable_allvis,
								  bool *skipping_current_range);
static bool lazy_scan_new_or_empty(LVRelState *vacrel, Buffer buf,
								   BlockNumber blkno, Page page,
								   bool sharelock, Buffer vmbuffer);
static void lazy_scan_prune(LVRelState *vacrel, Buffer buf,
							BlockNumber blkno, Page page,
							LVPagePruneState *prunestate);
static bool lazy_scan_noprune(LVRelState *vacrel, Buffer buf,
							  BlockNumber blkno, Page page,
							  bool *hastup, bool *recordfreespace);
static void lazy_vacuum(LVRelState *vacrel);
static bool lazy_vacuum_all_indexes(LVRelState *vacrel);
static void lazy_vacuum_heap_rel(LVRelState *vacrel);
static int	lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno,
								  Buffer buffer, int index, Buffer vmbuffer);
static bool lazy_check_wraparound_failsafe(LVRelState *vacrel);
static void lazy_cleanup_all_indexes(LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_vacuum_one_index(Relation indrel,
													IndexBulkDeleteResult *istat,
													double reltuples,
													LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_cleanup_one_index(Relation indrel,
													 IndexBulkDeleteResult *istat,
													 double reltuples,
													 bool estimated_count,
													 LVRelState *vacrel);
static bool should_attempt_truncation(LVRelState *vacrel);
static void lazy_truncate_heap(LVRelState *vacrel);
static BlockNumber count_nondeletable_pages(LVRelState *vacrel,
											bool *lock_waiter_detected);
static void dead_items_alloc(LVRelState *vacrel, int nworkers);
static void dead_items_cleanup(LVRelState *vacrel);
static bool heap_page_is_all_visible(LVRelState *vacrel, Buffer buf,
									 TransactionId *visibility_cutoff_xid, bool *all_frozen);
static void update_relstats_all_indexes(LVRelState *vacrel);
static void vacuum_error_callback(void *arg);
static void update_vacuum_error_info(LVRelState *vacrel,
									 LVSavedErrInfo *saved_vacrel,
									 int phase, BlockNumber blkno,
									 OffsetNumber offnum);
static void restore_vacuum_error_info(LVRelState *vacrel,
									  const LVSavedErrInfo *saved_vacrel);


/*
 *	heap_vacuum_rel() -- 对一个堆关系执行 VACUUM
 *
 *		本例程先做好准备，然后调用 lazy_scan_heap，
 *		几乎所有工作都在那里进行。调用返回后，通过管理关系截断
 *		并更新该关系的 pg_class
 *		项来收尾。（也会更新任何需要更新的索引的 pg_class 项。）
 *
 *		进入时，我们已经建立了事务，并打开
 *		且锁定了该关系。
 */
void
heap_vacuum_rel(Relation rel, VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	LVRelState *vacrel;
	bool		verbose,
				instrument,
				skipwithvm,
				frozenxid_updated,
				minmulti_updated;
	BlockNumber orig_rel_pages,
				new_rel_pages,
				new_rel_allvisible;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	PgStat_Counter startreadtime = 0,
				startwritetime = 0;
	WalUsage	startwalusage = pgWalUsage;
	BufferUsage startbufferusage = pgBufferUsage;
	ErrorContextCallback errcallback;
	char	  **indnames = NULL;

	verbose = (params->options & VACOPT_VERBOSE) != 0;
	instrument = (verbose || (IsAutoVacuumWorkerProcess() &&
							  params->log_min_duration >= 0));
	if (instrument)
	{
		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}
	}

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
								  RelationGetRelid(rel));

	/*
	 * 先为 ereport() 设置错误回溯支持。想法是设置
	 * 一个错误上下文回调，以便在清理期间的任何
	 * 错误上显示附加信息。在清理的不同阶段，我们更新
	 * 该状态，使错误上下文回调始终显示当前
	 * 信息。
	 *
	 * 把堆关系的名字复制到本地内存，也是为了错误报告。
	 * 不能总是放心地假设我们能取得每个
	 * 关系的名字。lazy_scan_heap 中的代码始终使用
	 * 这些临时副本会更方便。
	 */
	vacrel = (LVRelState *) palloc0(sizeof(LVRelState));
	vacrel->dbname = get_database_name(MyDatabaseId);
	vacrel->relnamespace = get_namespace_name(RelationGetNamespace(rel));
	vacrel->relname = pstrdup(RelationGetRelationName(rel));
	vacrel->indname = NULL;
	vacrel->phase = VACUUM_ERRCB_PHASE_UNKNOWN;
	vacrel->verbose = verbose;
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 建立关于 rel 及其索引的高层信息 */
	vacrel->rel = rel;
	vac_open_indexes(vacrel->rel, RowExclusiveLock, &vacrel->nindexes,
					 &vacrel->indrels);
	vacrel->bstrategy = bstrategy;
	if (instrument && vacrel->nindexes > 0)
	{
		/* 复制测量所用的索引名（不是为了错误报告） */
		indnames = palloc(sizeof(char *) * vacrel->nindexes);
		for (int i = 0; i < vacrel->nindexes; i++)
			indnames[i] = pstrdup(RelationGetRelationName(vacrel->indrels[i]));
	}

	/*
	 * index_cleanup 参数要么禁用索引清理和索引善后，要么
	 * 在我们本来会采用索引绕过
	 * 优化时强制继续。默认是 'auto'，把最终决定
	 * 留给 lazy_vacuum()。
	 *
	 * truncate 参数允许用户避免尝试关系截断，
	 * 但不能强制截断发生。
	 */
	Assert(params->index_cleanup != VACOPTVALUE_UNSPECIFIED);
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED &&
		   params->truncate != VACOPTVALUE_AUTO);

	/*
	 * 虽然在调用本函数之前 VacuumFailSafeActive 会被重置为 false，
	 * 由于递归调用，这里仍需要再重置一次。
	 */
	VacuumFailsafeActive = false;
	vacrel->consider_bypass_optimization = true;
	vacrel->do_index_vacuuming = true;
	vacrel->do_index_cleanup = true;
	vacrel->do_rel_truncate = (params->truncate != VACOPTVALUE_DISABLED);
	if (params->index_cleanup == VACOPTVALUE_DISABLED)
	{
		/* 预先强制禁用索引清理 */
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
	}
	else if (params->index_cleanup == VACOPTVALUE_ENABLED)
	{
		/* 强制进行索引清理。注意故障安全仍可绕过。 */
		vacrel->consider_bypass_optimization = false;
	}
	else
	{
		/* 默认/auto，所有决定都动态做出 */
		Assert(params->index_cleanup == VACOPTVALUE_AUTO);
	}

	/* 显式初始化页计数器（以求整齐） */
	vacrel->scanned_pages = 0;
	vacrel->removed_pages = 0;
	vacrel->frozen_pages = 0;
	vacrel->lpdead_item_pages = 0;
	vacrel->missed_dead_pages = 0;
	vacrel->nonempty_pages = 0;
	/* dead_items_alloc 稍后才分配 vacrel->dead_items */

	/* 分配/初始化输出统计状态 */
	vacrel->new_rel_tuples = 0;
	vacrel->new_live_tuples = 0;
	vacrel->indstats = (IndexBulkDeleteResult **)
		palloc0(vacrel->nindexes * sizeof(IndexBulkDeleteResult *));

	/* 初始化其余计数器（以求整齐） */
	vacrel->num_index_scans = 0;
	vacrel->tuples_deleted = 0;
	vacrel->tuples_frozen = 0;
	vacrel->lpdead_items = 0;
	vacrel->live_tuples = 0;
	vacrel->recently_dead_tuples = 0;
	vacrel->missed_dead_tuples = 0;

	/*
	 * 取得截止值，以判定哪些已删除元组视为 DEAD，
	 * 而不只是 RECENTLY_DEAD，以及要冻结哪些 XIDs/MXIDs。然后确定
	 * 我们将在 lazy_scan_heap 中扫描的块范围。必须
	 * 按这个顺序进行，以确保 OldestXmin 截止字段
	 * 作为我们将实际扫描的页中所存 XID 的上界
	 * （NewRelfrozenXid 跟踪绝不能漏掉未冻结的 XID）。
	 *
	 * 接着取得 vistest，这是 heap_page_prune 使用的相关截止值。
	 * 我们预期 vistest 总会让 heap_page_prune 移除任何 xmax
	 * < OldestXmin 的已删除元组。lazy_scan_prune 绝不能
	 * 搞混一个元组该冻结还是该移除。（将来
	 * 我们也许想让 lazy_scan_prune 不时重新计算 vistest，
	 * 以增加它能剪掉的死元组数量。）
	 */
	vacrel->aggressive = vacuum_get_cutoffs(rel, params, &vacrel->cutoffs);
	vacrel->rel_pages = orig_rel_pages = RelationGetNumberOfBlocks(rel);
	vacrel->vistest = GlobalVisTestFor(rel);
	/* 初始化用于跟踪现存最旧 XID/MXID 的状态 */
	vacrel->NewRelfrozenXid = vacrel->cutoffs.OldestXmin;
	vacrel->NewRelminMxid = vacrel->cutoffs.OldestMxact;
	vacrel->skippedallvis = false;
	skipwithvm = true;
	if (params->options & VACOPT_DISABLE_PAGE_SKIPPING)
	{
		/*
		 * 强制进入积极模式，并禁用借助
		 * 可见性映射跳过块（即使那些已设为全冻结的块）
		 */
		vacrel->aggressive = true;
		skipwithvm = false;
	}

	vacrel->skipwithvm = skipwithvm;

	if (verbose)
	{
		if (vacrel->aggressive)
			ereport(INFO,
					(errmsg("aggressively vacuuming \"%s.%s.%s\"",
							vacrel->dbname, vacrel->relnamespace,
							vacrel->relname)));
		else
			ereport(INFO,
					(errmsg("vacuuming \"%s.%s.%s\"",
							vacrel->dbname, vacrel->relnamespace,
							vacrel->relname)));
	}

	/*
	 * 用 dead_items_alloc 分配 dead_items 数组内存。这会在分配
	 * dead_items 所用共享内存时处理
	 * 并行 VACUUM 的初始化。（但先做一次故障安全预检，
	 * 以确保当 relfrozenxid
	 * 已经危险地过旧时，根本不会尝试并行 VACUUM。）
	 */
	lazy_check_wraparound_failsafe(vacrel);
	dead_items_alloc(vacrel, params->nworkers);

	/*
	 * 调用 lazy_scan_heap，完成所需的堆剪枝、索引
	 * 清理和堆清理（以及相关处理）
	 */
	lazy_scan_heap(vacrel);

	/*
	 * 释放 dead_items_alloc 管理的资源。必要时
	 * 顺带结束并行模式。
	 */
	dead_items_cleanup(vacrel);
	Assert(!IsInParallelMode());

	/*
	 * 在适当处更新 rel 每个索引的 pg_class 项。
	 *
	 * 与稍后更新 rel 自身的 pg_class 项不同，这一步并非关键。
	 * 它只维护规划器使用的 relpages/reltuples 统计信息。
	 */
	if (vacrel->do_index_cleanup)
		update_relstats_all_indexes(vacrel);

	/* rel 的索引已处理完 */
	vac_close_indexes(vacrel->nindexes, vacrel->indrels, NoLock);

	/* 可选地截断 rel */
	if (should_attempt_truncation(vacrel))
		lazy_truncate_heap(vacrel);

	/* 弹出错误上下文栈 */
	error_context_stack = errcallback.previous;

	/* 报告我们现在正在做最终善后 */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/*
	 * 准备更新 rel 的 pg_class 项。
	 *
	 * 积极 VACUUMs 必须始终能把 relfrozenxid 推进到
	 * >= FreezeLimit 的值，并把 relminmxid 推进到 >= MultiXactCutoff 的值。
	 * 非积极 VACUUM 可以把它们推进任意量，也可以完全不推进。
	 */
	Assert(vacrel->NewRelfrozenXid == vacrel->cutoffs.OldestXmin ||
		   TransactionIdPrecedesOrEquals(vacrel->aggressive ? vacrel->cutoffs.FreezeLimit :
										 vacrel->cutoffs.relfrozenxid,
										 vacrel->NewRelfrozenXid));
	Assert(vacrel->NewRelminMxid == vacrel->cutoffs.OldestMxact ||
		   MultiXactIdPrecedesOrEquals(vacrel->aggressive ? vacrel->cutoffs.MultiXactCutoff :
									   vacrel->cutoffs.relminmxid,
									   vacrel->NewRelminMxid));
	if (vacrel->skippedallvis)
	{
		/*
		 * 在选择跳过一段全可见页范围的非积极 VACUUM 中，
		 * 必须保留原来的 relfrozenxid。跟踪新
		 * 值的状态会漏掉我们跳过的页中未冻结的 XIDs。
		 */
		Assert(!vacrel->aggressive);
		vacrel->NewRelfrozenXid = InvalidTransactionId;
		vacrel->NewRelminMxid = InvalidMultiXactId;
	}

	/*
	 * 为安全起见，把 relallvisible 钳制为不超过我们将设置的
	 * pg_class.relpages
	 */
	new_rel_pages = vacrel->rel_pages;	/* 在可能的关系截断之后 */
	visibilitymap_count(rel, &new_rel_allvisible, NULL);
	if (new_rel_allvisible > new_rel_pages)
		new_rel_allvisible = new_rel_pages;

	/*
	 * 现在真正更新 rel 的 pg_class 项。
	 *
	 * 原则上 new_live_tuples 可能是 -1，表示我们（仍然）
	 * 不知道元组数。实践中这不会发生，因为我们
	 * 会扫描每一页未被可见性映射跳过的页。
	 */
	vac_update_relstats(rel, new_rel_pages, vacrel->new_live_tuples,
						new_rel_allvisible, vacrel->nindexes > 0,
						vacrel->NewRelfrozenXid, vacrel->NewRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);

	/*
	 * 也把结果报告给累积统计系统。
	 *
	 * 有意不把由于 VACUUM 绕过索引清理和堆清理
	 * 而仍留在表中的 LP_DEAD 项告诉统计系统。
	 * ANALYZE 会把剩余的 LP_DEAD 项视为死"元组"。
	 * 宁可不要过快再次清理，这似乎是个好主意，
	 * 在故障安全阻止了大量堆
	 * 清理的情况下尤其如此。
	 */
	pgstat_report_vacuum(RelationGetRelid(rel),
						 rel->rd_rel->relisshared,
						 Max(vacrel->new_live_tuples, 0),
						 vacrel->recently_dead_tuples +
						 vacrel->missed_dead_tuples);
	pgstat_progress_end_command();

	if (instrument)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (verbose || params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			long		secs_dur;
			int			usecs_dur;
			WalUsage	walusage;
			BufferUsage bufferusage;
			StringInfoData buf;
			char	   *msgfmt;
			int32		diff;
			double		read_rate = 0,
						write_rate = 0;

			TimestampDifference(starttime, endtime, &secs_dur, &usecs_dur);
			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &startwalusage);
			memset(&bufferusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufferusage, &pgBufferUsage, &startbufferusage);

			initStringInfo(&buf);
			if (verbose)
			{
				/*
				 * 积极程度已在更早的专用
				 * VACUUM VERBOSE ereport 中报告过
				 */
				Assert(!params->is_wraparound);
				msgfmt = _("finished vacuuming \"%s.%s.%s\": index scans: %d\n");
			}
			else if (params->is_wraparound)
			{
				/*
				 * VACUUM 有可能同时是 is_wraparound
				 * 和 !aggressive，但这只是边角情况 -- is_wraparound
				 * 蕴含 aggressive。尽管如此，仍为该边角
				 * 情况产生不同的输出，以防万一。
				 */
				if (vacrel->aggressive)
					msgfmt = _("automatic aggressive vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
			}
			else
			{
				if (vacrel->aggressive)
					msgfmt = _("automatic aggressive vacuum of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum of table \"%s.%s.%s\": index scans: %d\n");
			}
			appendStringInfo(&buf, msgfmt,
							 vacrel->dbname,
							 vacrel->relnamespace,
							 vacrel->relname,
							 vacrel->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u scanned (%.2f%% of total)\n"),
							 vacrel->removed_pages,
							 new_rel_pages,
							 vacrel->scanned_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->scanned_pages / orig_rel_pages);
			appendStringInfo(&buf,
							 _("tuples: %lld removed, %lld remain, %lld are dead but not yet removable\n"),
							 (long long) vacrel->tuples_deleted,
							 (long long) vacrel->new_rel_tuples,
							 (long long) vacrel->recently_dead_tuples);
			if (vacrel->missed_dead_tuples > 0)
				appendStringInfo(&buf,
								 _("tuples missed: %lld dead from %u pages not removed due to cleanup lock contention\n"),
								 (long long) vacrel->missed_dead_tuples,
								 vacrel->missed_dead_pages);
			diff = (int32) (ReadNextTransactionId() -
							vacrel->cutoffs.OldestXmin);
			appendStringInfo(&buf,
							 _("removable cutoff: %u, which was %d XIDs old when operation ended\n"),
							 vacrel->cutoffs.OldestXmin, diff);
			if (frozenxid_updated)
			{
				diff = (int32) (vacrel->NewRelfrozenXid -
								vacrel->cutoffs.relfrozenxid);
				appendStringInfo(&buf,
								 _("new relfrozenxid: %u, which is %d XIDs ahead of previous value\n"),
								 vacrel->NewRelfrozenXid, diff);
			}
			if (minmulti_updated)
			{
				diff = (int32) (vacrel->NewRelminMxid -
								vacrel->cutoffs.relminmxid);
				appendStringInfo(&buf,
								 _("new relminmxid: %u, which is %d MXIDs ahead of previous value\n"),
								 vacrel->NewRelminMxid, diff);
			}
			appendStringInfo(&buf, _("frozen: %u pages from table (%.2f%% of total) had %lld tuples frozen\n"),
							 vacrel->frozen_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->frozen_pages / orig_rel_pages,
							 (long long) vacrel->tuples_frozen);
			if (vacrel->do_index_vacuuming)
			{
				if (vacrel->nindexes == 0 || vacrel->num_index_scans == 0)
					appendStringInfoString(&buf, _("index scan not needed: "));
				else
					appendStringInfoString(&buf, _("index scan needed: "));

				msgfmt = _("%u pages from table (%.2f%% of total) had %lld dead item identifiers removed\n");
			}
			else
			{
				if (!VacuumFailsafeActive)
					appendStringInfoString(&buf, _("index scan bypassed: "));
				else
					appendStringInfoString(&buf, _("index scan bypassed by failsafe: "));

				msgfmt = _("%u pages from table (%.2f%% of total) have %lld dead item identifiers\n");
			}
			appendStringInfo(&buf, msgfmt,
							 vacrel->lpdead_item_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->lpdead_item_pages / orig_rel_pages,
							 (long long) vacrel->lpdead_items);
			for (int i = 0; i < vacrel->nindexes; i++)
			{
				IndexBulkDeleteResult *istat = vacrel->indstats[i];

				if (!istat)
					continue;

				appendStringInfo(&buf,
								 _("index \"%s\": pages: %u in total, %u newly deleted, %u currently deleted, %u reusable\n"),
								 indnames[i],
								 istat->num_pages,
								 istat->pages_newly_deleted,
								 istat->pages_deleted,
								 istat->pages_free);
			}
			if (track_io_timing)
			{
				double		read_ms = (double) (pgStatBlockReadTime - startreadtime) / 1000;
				double		write_ms = (double) (pgStatBlockWriteTime - startwritetime) / 1000;

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			if (secs_dur > 0 || usecs_dur > 0)
			{
				read_rate = (double) BLCKSZ * (bufferusage.shared_blks_read + bufferusage.local_blks_read) /
					(1024 * 1024) / (secs_dur + usecs_dur / 1000000.0);
				write_rate = (double) BLCKSZ * (bufferusage.shared_blks_dirtied + bufferusage.local_blks_dirtied) /
					(1024 * 1024) / (secs_dur + usecs_dur / 1000000.0);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf,
							 _("buffer usage: %lld hits, %lld misses, %lld dirtied\n"),
							 (long long) (bufferusage.shared_blks_hit + bufferusage.local_blks_hit),
							 (long long) (bufferusage.shared_blks_read + bufferusage.local_blks_read),
							 (long long) (bufferusage.shared_blks_dirtied + bufferusage.local_blks_dirtied));
			appendStringInfo(&buf,
							 _("WAL usage: %lld records, %lld full page images, %llu bytes\n"),
							 (long long) walusage.wal_records,
							 (long long) walusage.wal_fpi,
							 (unsigned long long) walusage.wal_bytes);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(verbose ? INFO : LOG,
					(errmsg_internal("%s", buf.data)));
			pfree(buf.data);
		}
	}

	/* 清理索引统计信息和索引名 */
	for (int i = 0; i < vacrel->nindexes; i++)
	{
		if (vacrel->indstats[i])
			pfree(vacrel->indstats[i]);

		if (instrument)
			pfree(indnames[i]);
	}
}

/*
 *	lazy_scan_heap() -- VACUUM 的主力函数
 *
 *		本例程剪枝堆中的每一页，并考虑是否需要
 *		冻结仍带存储的剩余元组（不包括可用
 *		可见性映射跳过的页）。也会对 FSM 和可见性映射
 *		做相关维护。这些步骤都发生在对
 *		目标堆关系的初始扫描期间。
 *
 *		也会调用 lazy_vacuum_all_indexes 来清理索引，这主要
 *		是删除指向剪枝后留在堆页上的 LP_DEAD 项的
 *		索引元组。更早的堆初始扫描
 *		已经收集了需要移除其索引元组的 TIDs。
 *
 *		最后调用 lazy_vacuum_heap_rel 来清理堆页，这
 *		主要是把 LP_DEAD 项（来自收集到的 TID 数组）
 *		标为 LP_UNUSED。这必须在第二次、也是最后一次堆扫描中
 *		进行，以保持所有索引 AM 依赖的基本不变量：任何
 *		现存索引元组都绝不能包含指向堆中
 *		LP_UNUSED 行指针的 TID。我们必须禁止过早
 *		回收行指针，以免索引扫描在回收之后立刻
 *		搞混哪个 TID 指向哪个元组。
 *		（实际上，当目标堆关系恰好
 *		没有索引时这不是问题，因此我们可以安全地把单遍策略
 *		当作优化来用）。
 *
 *		实践中我们常常有足够空间放下所有 TID，因此
 *		在对堆的初始扫描完全结束后，不需要
 *		多次调用 lazy_vacuum。否则情况会稍微
 *		复杂一些：我们的堆"初始扫描"只适用于在需要调用
 *		lazy_vacuum 之前已经剪枝的那些页，而堆的
 *		"最终扫描"也只清理同样这些堆页。
 *		然而，每次调用 lazy_vacuum 都会完整处理索引，
 *		这使内存在
 *		不足时索引处理非常低效。
 */
static void
lazy_scan_heap(LVRelState *vacrel)
{
	BlockNumber rel_pages = vacrel->rel_pages,
				blkno,
				next_unskippable_block,
				next_fsm_block_to_vacuum = 0;
	VacDeadItems *dead_items = vacrel->dead_items;
	Buffer		vmbuffer = InvalidBuffer;
	bool		next_unskippable_allvis,
				skipping_current_range;
	const int	initprog_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_TOTAL_HEAP_BLKS,
		PROGRESS_VACUUM_MAX_DEAD_TUPLES
	};
	int64		initprog_val[3];

	/* 报告我们正在扫描堆，并通告总块数 */
	initprog_val[0] = PROGRESS_VACUUM_PHASE_SCAN_HEAP;
	initprog_val[1] = rel_pages;
	initprog_val[2] = dead_items->max_items;
	pgstat_progress_update_multi_param(3, initprog_index, initprog_val);

	/* 用可见性映射建立初始的可跳过块范围 */
	next_unskippable_block = lazy_scan_skip(vacrel, &vmbuffer, 0,
											&next_unskippable_allvis,
											&skipping_current_range);
	for (blkno = 0; blkno < rel_pages; blkno++)
	{
		Buffer		buf;
		Page		page;
		bool		all_visible_according_to_vm;
		LVPagePruneState prunestate;

		if (blkno == next_unskippable_block)
		{
			/*
			 * 无法安全跳过此页。必须扫描该页。但
			 * 先确定该页之后的下一个可跳过范围。
			 */
			all_visible_according_to_vm = next_unskippable_allvis;
			next_unskippable_block = lazy_scan_skip(vacrel, &vmbuffer,
													blkno + 1,
													&next_unskippable_allvis,
													&skipping_current_range);

			Assert(next_unskippable_block >= blkno + 1);
		}
		else
		{
			/* 最后一页总是扫描（可能需要设置 nonempty_pages） */
			Assert(blkno < rel_pages - 1);

			if (skipping_current_range)
				continue;

			/* 当前范围太小，不值得跳过 -- 直接扫描该页 */
			all_visible_according_to_vm = true;
		}

		vacrel->scanned_pages++;

		/* 报告该块已扫描，并更新错误回溯信息 */
		pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);
		update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_SCAN_HEAP,
								 blkno, InvalidOffsetNumber);

		vacuum_delay_point();

		/*
		 * 定期检查回卷故障安全是否应触发。
		 *
		 * lazy_vacuum_all_indexes() 里有类似检查，但
		 * relfrozenxid 可能在我们到达
		 * 那一点之前就开始显得危险地过旧。本检查也为
		 * 单遍策略，以及 index_cleanup
		 * 参数设为 'off' 的两遍策略提供故障安全覆盖。
		 */
		if (vacrel->scanned_pages % FAILSAFE_EVERY_PAGES == 0)
			lazy_check_wraparound_failsafe(vacrel);

		/*
		 * 考虑我们是否肯定已有足够空间处理该页上的
		 * TIDs。若接近用尽 dead_items TID 的
		 * 可用空间，就先暂停并做一轮清理，再处理
		 * 此页。
		 */
		Assert(dead_items->max_items >= MaxHeapTuplesPerPage);
		if (dead_items->max_items - dead_items->num_items < MaxHeapTuplesPerPage)
		{
			/*
			 * 开始索引清理之前，释放我们可能
			 * 在可见性映射页上持有的任何钉住。这对
			 * 正确性并非必要，但我们仍然这样做，以避免在一次
			 * 耗时很长且无关的操作期间一直钉住。
			 */
			if (BufferIsValid(vmbuffer))
			{
				ReleaseBuffer(vmbuffer);
				vmbuffer = InvalidBuffer;
			}

			/* 执行一轮索引清理和堆清理 */
			vacrel->consider_bypass_optimization = false;
			lazy_vacuum(vacrel);

			/*
			 * 清理空闲空间映射，使新释放的空间在
			 * 上层 FSM 页上可见。注意我们尚未处理 blkno。
			 */
			FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum,
									blkno);
			next_fsm_block_to_vacuum = blkno;

			/* 报告我们再次开始扫描堆 */
			pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
										 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
		}

		/*
		 * 钉住可见性映射页，以防需要把该页标为
		 * 全可见。多数情况下这非常便宜，因为我们
		 * 反正已经钉住了正确的页。
		 */
		visibilitymap_pin(vacrel->rel, blkno, &vmbuffer);

		/*
		 * 我们需要缓冲区清理锁，以便在 lazy_scan_prune 中剪枝 HOT 链并
		 * 对该页做碎片整理。但若不能立刻取得
		 * 清理锁，也许可以退而求其次，用 lazy_scan_noprune
		 * 做缩减处理。
		 */
		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 vacrel->bstrategy);
		page = BufferGetPage(buf);
		if (!ConditionalLockBufferForCleanup(buf))
		{
			bool		hastup,
						recordfreespace;

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			/* 在调用 lazy_scan_noprune 之前检查新页或空页 */
			if (lazy_scan_new_or_empty(vacrel, buf, blkno, page, true,
									   vmbuffer))
			{
				/* 已按新页/空页处理（锁和钉住已释放） */
				continue;
			}

			/* 在 dead_items 数组中收集 LP_DEAD 项，并清点元组 */
			if (lazy_scan_noprune(vacrel, buf, blkno, page, &hastup,
								  &recordfreespace))
			{
				Size		freespace = 0;

				/*
				 * 已成功处理该页（没有清理锁）-- 只需
				 * 执行关系截断和 FSM 步骤，很像
				 * lazy_scan_prune 的情形。不过不必费力去匹配它的
				 * 可见性映射设置步骤。
				 */
				if (hastup)
					vacrel->nonempty_pages = blkno + 1;
				if (recordfreespace)
					freespace = PageGetHeapFreeSpace(page);
				UnlockReleaseBuffer(buf);
				if (recordfreespace)
					RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
				continue;
			}

			/*
			 * lazy_scan_noprune 无法完成全部所需处理。等待
			 * 清理锁，然后按通常方式调用 lazy_scan_prune。
			 */
			Assert(vacrel->aggressive);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBufferForCleanup(buf);
		}

		/* 在调用 lazy_scan_prune 之前检查新页或空页 */
		if (lazy_scan_new_or_empty(vacrel, buf, blkno, page, false, vmbuffer))
		{
			/* 已按新页/空页处理（锁和钉住已释放） */
			continue;
		}

		/*
		 * 剪枝、冻结并清点元组。
		 *
		 * 把该页上剩余 LP_DEAD 行指针的细节累积到
		 * dead_items 数组中。这包括我们自己
		 * 剪枝得到的 LP_DEAD 行指针，以及更早时候
		 * 剪枝留下的已有 LP_DEAD 行指针。也会考虑冻结仍带存储的
		 * 剩余项的元组头中的 XIDs。
		 */
		lazy_scan_prune(vacrel, buf, blkno, page, &prunestate);

		Assert(!prunestate.all_visible || !prunestate.has_lpdead_items);

		/* 记住最后一个含有不可移除元组的页的位置 */
		if (prunestate.hastup)
			vacrel->nonempty_pages = blkno + 1;

		if (vacrel->nindexes == 0)
		{
			/*
			 * 现在考虑在使用单遍策略时，是否需要
			 * 一次一页地做堆清理。
			 *
			 * 单遍策略永远不会调用 lazy_vacuum()。这里执行的
			 * 步骤可以看作一次 lazy_vacuum()
			 * 调用在单遍策略下的对应物。
			 */
			if (prunestate.has_lpdead_items)
			{
				Size		freespace;

				lazy_vacuum_heap_page(vacrel, blkno, buf, 0, vmbuffer);

				/* 忘掉我们刚刚清理掉的 LP_DEAD 项 */
				dead_items->num_items = 0;

				/*
				 * 定期清理 FSM，使新释放的
				 * 空间在上层 FSM 页上可见。注意我们尚未
				 * 对 blkno 做 FSM 处理。
				 */
				if (blkno - next_fsm_block_to_vacuum >= VACUUM_FSM_EVERY_PAGES)
				{
					FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum,
											blkno);
					next_fsm_block_to_vacuum = blkno;
				}

				/*
				 * 现在对 blkno 做 FSM 处理，然后转到下一
				 * 页。
				 *
				 * 我们对 lazy_vacuum_heap_page() 的调用会考虑是否
				 * 可以独立于 lazy_scan_prune() 设置 all_visible/all_frozen。
				 * 注意 prunestate 已被
				 * lazy_vacuum_heap_page() 调用作废。
				 */
				freespace = PageGetHeapFreeSpace(page);

				UnlockReleaseBuffer(buf);
				RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
				continue;
			}

			/*
			 * 没有调用 lazy_vacuum_heap_page()，因为剪枝
			 * 没有遇到/产生任何需要
			 * 清理的 LP_DEAD 项。剪枝状态没有作废，因此继续
			 * 由 prunestate 驱动的可见性映射和 FSM 步骤（就像
			 * 两遍策略那样）。
			 */
			Assert(dead_items->num_items == 0);
		}

		/*
		 * 根据来自 VM 的信息
		 * （截至上次 lazy_scan_skip() 调用）以及 prunestate，处理可见性映射位的设置
		 */
		if (!all_visible_according_to_vm && prunestate.all_visible)
		{
			uint8		flags = VISIBILITYMAP_ALL_VISIBLE;

			if (prunestate.all_frozen)
			{
				Assert(!TransactionIdIsValid(prunestate.visibility_cutoff_xid));
				flags |= VISIBILITYMAP_ALL_FROZEN;
			}

			/*
			 * 可见性映射页被置位
			 * 而页级位却清除，这种情况绝不应当发生，但反过来是允许的
			 * （若未启用校验和）。无论如何都把两个位都设上，
			 * 以便重新同步。
			 *
			 * 注意：若堆页是全可见的但 VM 位未设置，
			 * 我们不需要把堆页标脏。但是，若启用了校验和，
			 * 我们确实需要确保堆页
			 * 在传给 visibilitymap_set() 之前被标脏，因为它
			 * 可能会被记入日志。鉴于这种情况只应在
			 * 崩溃后的罕见情形中发生，不值得优化。
			 */
			PageSetAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, prunestate.visibility_cutoff_xid,
							  flags);
		}

		/*
		 * 自 PostgreSQL 9.2 起，若
		 * 页级位是清除的，可见性映射位就绝不应当被设置。不过，该位有可能
		 * 在调用 lazy_scan_skip() 之后被清除，所以在断定 VM 损坏之前，我们必须
		 * 在持有缓冲区锁的情况下重新检查。
		 */
		else if (all_visible_according_to_vm && !PageIsAllVisible(page) &&
				 visibilitymap_get_status(vacrel->rel, blkno, &vmbuffer) != 0)
		{
			elog(WARNING, "page is not marked all-visible but visibility map bit is set in relation \"%s\" page %u",
				 vacrel->relname, blkno);
			visibilitymap_clear(vacrel->rel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * GetOldestNonRemovableTransactionId()
		 * 返回的值有可能回退，因此我们看到似乎尚未对
		 * 所有人都可见的元组，而 PD_ALL_VISIBLE 已经设置，这并不
		 * 算错。真正安全的
		 * xmin 值从不回退，但
		 * GetOldestNonRemovableTransactionId() 是保守的，有时
		 * 返回一个不必要地偏小的值，所以若我们看到这种
		 * 矛盾，那只是说明我们认为尚未
		 * 对所有人可见的元组实际上已经可见，而 PD_ALL_VISIBLE 标志
		 * 是正确的。
		 *
		 * 不过，在设置了 PD_ALL_VISIBLE 的页上
		 * 绝不应当有 LP_DEAD 项。
		 */
		else if (prunestate.has_lpdead_items && PageIsAllVisible(page))
		{
			elog(WARNING, "page containing LP_DEAD items is marked as all-visible in relation \"%s\" page %u",
				 vacrel->relname, blkno);
			PageClearAllVisible(page);
			MarkBufferDirty(buf);
			visibilitymap_clear(vacrel->rel, blkno, vmbuffer,
								VISIBILITYMAP_VALID_BITS);
		}

		/*
		 * 若全可见页已经全冻结但尚未这样标记，
		 * 就把它标为全冻结。注意只有 all_visible
		 * 为真时 all_frozen 才有效，所以必须检查 prunestate 的两个字段。
		 */
		else if (all_visible_according_to_vm && prunestate.all_visible &&
				 prunestate.all_frozen &&
				 !VM_ALL_FROZEN(vacrel->rel, blkno, &vmbuffer))
		{
			/*
			 * 不要把 all_visible_according_to_vm 当作
			 * 页级 PD_ALL_VISIBLE 位已设置的代理，因为它可能已经
			 * 过时 -- 即使 prunestate 中设置了 all_visible
			 */
			if (!PageIsAllVisible(page))
			{
				PageSetAllVisible(page);
				MarkBufferDirty(buf);
			}

			/*
			 * 在 VM 中把该页设为全冻结（以及全可见）。
			 *
			 * 我们可以把 InvalidTransactionId 作为 visibility_cutoff_xid 传入，
			 * 因为在冻结该页元组时已经记入了足以使一切
			 * 对 REDO 都安全的 snapshotConflictHorizon。
			 */
			Assert(!TransactionIdIsValid(prunestate.visibility_cutoff_xid));
			visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, InvalidTransactionId,
							  VISIBILITYMAP_ALL_VISIBLE |
							  VISIBILITYMAP_ALL_FROZEN);
		}

		/*
		 * 该块的最后步骤：放下清理锁，把空闲空间记入
		 * FSM
		 */
		if (prunestate.has_lpdead_items && vacrel->do_index_vacuuming)
		{
			/*
			 * 等到 lazy_vacuum_heap_rel() 再保存空闲空间。这
			 * 不只是省下一些开销；它还让我们能记录
			 * lazy_vacuum_heap_page() 在可以截断
			 * 该页行指针数组时
			 * 将额外腾出的空闲空间。
			 *
			 * 注意：我们并非 100% 确定真的会调用
			 * lazy_vacuum_heap_rel() -- lazy_vacuum() 仍可能选择跳过
			 * 索引清理（因此也必须跳过堆清理）。这被
			 * 认为可以接受，因为它只发生在紧急情况下，或
			 * 空闲空间本来就很少时。（此外，一旦放弃
			 * 索引清理，我们就开始
			 * 把空闲空间记入 FSM。）
			 *
			 * 注意：单遍（无索引）情形只有在剪枝期间
			 * 没有 LP_DEAD 项时，才应当走到这里。
			 */
			Assert(vacrel->nindexes > 0);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Size		freespace = PageGetHeapFreeSpace(page);

			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		}
	}

	vacrel->blkno = InvalidBlockNumber;
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/* 报告现在一切都已扫描 */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);

	/* 现在可以计算 pg_class.reltuples 的新值 */
	vacrel->new_live_tuples = vac_estimate_reltuples(vacrel->rel, rel_pages,
													 vacrel->scanned_pages,
													 vacrel->live_tuples);

	/*
	 * 同时计算幸存的堆项总数。在
	 * new_live_tuples 为 -1 这种（不太可能的）情形下，把它当作零。
	 */
	vacrel->new_rel_tuples =
		Max(vacrel->new_live_tuples, 0) + vacrel->recently_dead_tuples +
		vacrel->missed_dead_tuples;

	/*
	 * 做索引清理（调用每个索引的 ambulkdelete 例程），然后做
	 * 相关的堆清理
	 */
	if (dead_items->num_items > 0)
		lazy_vacuum(vacrel);

	/*
	 * 清理空闲空间映射的剩余部分。无论是否
	 * 有索引，也无论是否绕过了索引清理，都必须这样做。
	 */
	if (blkno > next_fsm_block_to_vacuum)
		FreeSpaceMapVacuumRange(vacrel->rel, next_fsm_block_to_vacuum, blkno);

	/* 报告所有块都已清理 */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* 做最终的索引善后（调用每个索引的 amvacuumcleanup 例程） */
	if (vacrel->nindexes > 0 && vacrel->do_index_cleanup)
		lazy_cleanup_all_indexes(vacrel);
}

/*
 *	lazy_scan_skip() -- 用可见性映射建立可跳过块的范围。
 *
 * 每次需要用可见性映射建立新的
 * 可跳过块范围时，lazy_scan_heap() 都会调用到这里。调用方传入下一个
 * 待处理的块。我们为该范围返回 next_unskippable_block。若没有
 * 可跳过的块，就只返回调用方的 next_block。返回块的全可见
 * 状态也在 *next_unskippable_allvis 中为调用方设置。
 * 该块通常不会是全可见的（因为它不可跳过），但在
 * 积极 VACUUMs 期间（以及某些边角情况中）可以是。
 *
 * 设置 *skipping_current_range，表示调用方是否应跳过该范围。
 * 代价和收益驱动我们的决定。非常小的范围不会被跳过。
 *
 * 注意：我们对哪些块可以跳过的判断可能立刻过时。
 * 不过，若调用方"错过"了一个全可见或全冻结标记
 * 被并发清除的页，那也没关系。重要的是调用方扫描所有
 * 其元组可能含有 XIDs < OldestXmin 或 MXIDs < OldestMxact 的页。
 * （实际上，非积极 VACUUM 可以选择跳过含有
 * 更旧 XID/MXID 的全可见页。当真正做出
 * 跳过这样一段范围的选择时，这里会设置 vacrel->skippedallvis 标志，从而使一切安全。）
 */
static BlockNumber
lazy_scan_skip(LVRelState *vacrel, Buffer *vmbuffer, BlockNumber next_block,
			   bool *next_unskippable_allvis, bool *skipping_current_range)
{
	BlockNumber rel_pages = vacrel->rel_pages,
				next_unskippable_block = next_block,
				nskippable_blocks = 0;
	bool		skipsallvis = false;

	*next_unskippable_allvis = true;
	while (next_unskippable_block < rel_pages)
	{
		uint8		mapbits = visibilitymap_get_status(vacrel->rel,
													   next_unskippable_block,
													   vmbuffer);

		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) == 0)
		{
			Assert((mapbits & VISIBILITYMAP_ALL_FROZEN) == 0);
			*next_unskippable_allvis = false;
			break;
		}

		/*
		 * 调用方必须扫描最后一页，以判断它是否有元组
		 * （调用方必须有机会设置 vacrel->nonempty_pages）。
		 * 这条规则避免 lazy_truncate_heap() 对 rel 取得访问排他
		 * 锁，只因为最后一页上有元组就去尝试一次反正会失败的截断
		 * （附近其他页上很可能
		 * 也有元组，但那些可以跳过）。
		 *
		 * 实现方式是始终把最后一块视为跳过不安全。
		 */
		if (next_unskippable_block == rel_pages - 1)
			break;

		/* DISABLE_PAGE_SKIPPING 使一切跳过都不安全 */
		if (!vacrel->skipwithvm)
			break;

		/*
		 * 积极 VACUUM 的调用方不能仅仅因为页是
		 * 全可见的就跳过它们。它们仍可跳过全冻结页，那些页不可能
		 * 含有 < OldestXmin 的 XIDs（现在尚未冻结的 XID）。
		 */
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) == 0)
		{
			if (vacrel->aggressive)
				break;

			/*
			 * 在非积极情形下，全可见块可以安全跳过。但
			 * 要记住最终范围包含这样一块，供以后使用。
			 */
			skipsallvis = true;
		}

		vacuum_delay_point();
		next_unskippable_block++;
		nskippable_blocks++;
	}

	/*
	 * 我们只跳过至少有 SKIP_PAGES_THRESHOLD 个连续
	 * 页的范围。因为我们在顺序读取，OS 应当在为我们
	 * 做预读，所以偶尔跳过一页没有好处。
	 * 跳过这样的范围甚至可能妨碍顺序检测。
	 *
	 * 这项测试也使非积极 VACUUMs 期间 relfrozenxid 的推进更频繁。
	 * 若该范围含有任何全可见页，那么
	 * 跳过会使更新 relfrozenxid 变得不安全，这是真正的缺点。
	 */
	if (nskippable_blocks < SKIP_PAGES_THRESHOLD)
		*skipping_current_range = false;
	else
	{
		*skipping_current_range = true;
		if (skipsallvis)
			vacrel->skippedallvis = true;
	}

	return next_unskippable_block;
}

/*
 *	lazy_scan_new_or_empty() -- lazy_scan_heap() 对新页/空页的处理。
 *
 * 在调用 lazy_scan_prune 或 lazy_scan_noprune 之前，必须调用这里
 * 来处理新页和空页，因为它们没有准备好对付
 * 新页或空页。
 *
 * 有必要把新页当作特殊情况，因为用空页维护
 * 可见性映射和 FSM 的规则略有
 * 不同（不过新页可以在关系截断期间被截掉）。
 *
 * 空页其实不是特殊情况 -- 它们只是
 * 没有已分配元组的堆页（甚至包括 LP_UNUSED 项）。你也许
 * 会奇怪为什么这里仍然要处理它们。这只是因为
 * 堆关系扩展期间硬崩溃这一边角情况
 * 才有必要。若我们有朝一日让关系扩展对崩溃安全，那么
 * 这里就不再需要处理空页（就此而言，
 * 也不再需要处理新页）。
 *
 * 调用方必须至少持有共享锁。那种情况下我们可能需要升级
 * 该锁，所以必须用 'sharelock' 参数
 * 指明调用方持有的锁类型。
 *
 * 在常见情形下返回假，调用方应继续调用
 * lazy_scan_prune（或 lazy_scan_noprune）。否则返回真，表示
 * lazy_scan_heap 已处理完该页，并代调用方
 * 释放了锁。
 */
static bool
lazy_scan_new_or_empty(LVRelState *vacrel, Buffer buf, BlockNumber blkno,
					   Page page, bool sharelock, Buffer vmbuffer)
{
	Size		freespace;

	if (PageIsNew(page))
	{
		/*
		 * 全零页可能残留下来：要么某个后端把关系
		 * 扩展了一页，但在新初始化的
		 * 页写出之前崩溃了；要么在批量扩展该关系
		 * （这会在关系尾部创建若干空页）
		 * 之后把它们记入 FSM。
		 *
		 * 注意我们不把该页记入 visibilitymap。这样做的
		 * 缺点是后续清理会反复访问此页，
		 * 否则在提升后的备库上我们将永远发现不了这块空间。
		 * 反复检查的害处通常不应太严重。这些
		 * 空间通常总会在某个时候被用掉，否则
		 * 就不会有常规清理。
		 *
		 * 确保这些页在 FSM 中，以便它们能被重用。
		 * 做法是测试该页是否记录了任何空间。若
		 * 没有，就记入。我们在释放堆页上的锁之后才这样做，
		 * 毕竟 FSM 是近似的。
		 */
		UnlockReleaseBuffer(buf);

		if (GetRecordedFreeSpace(vacrel->rel, blkno) == 0)
		{
			freespace = BLCKSZ - SizeOfPageHeaderData;

			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		}

		return true;
	}

	if (PageIsEmpty(page))
	{
		/*
		 * 调用方似乎总能在空页上取得清理
		 * 锁。但不要冒险 -- 升级到
		 * 排他锁（不过仍然不需要清理锁）。
		 */
		if (sharelock)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			if (!PageIsEmpty(page))
			{
				/* 页不是新页或空页 -- 暂时保留锁和钉住 */
				return false;
			}
		}
		else
		{
			/* 已经持有完整的清理锁（这已经绰绰有余） */
		}

		/*
		 * 与新页不同，空页总是被设为全可见和
		 * 全冻结。
		 */
		if (!PageIsAllVisible(page))
		{
			START_CRIT_SECTION();

			/* 在写 WAL 记录之前把缓冲区标脏 */
			MarkBufferDirty(buf);

			/*
			 * 另一个后端可能已经扩展了堆，
			 * 初始化了该页，然后由于
			 * ERROR 而未能把该页写入 WAL。因为堆扩展不记入 WAL，恢复
			 * 可能会重放我们把该页设为全可见的记录，并
			 * 发现该页尚未初始化，这会导致 PANIC。
			 * 为防止这种情况，检查该页先前是否
			 * 已记入 WAL，若没有，现在就记。
			 */
			if (RelationNeedsWAL(vacrel->rel) &&
				PageGetLSN(page) == InvalidXLogRecPtr)
				log_newpage_buffer(buf, true);

			PageSetAllVisible(page);
			visibilitymap_set(vacrel->rel, blkno, buf, InvalidXLogRecPtr,
							  vmbuffer, InvalidTransactionId,
							  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
			END_CRIT_SECTION();
		}

		freespace = PageGetHeapFreeSpace(page);
		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		return true;
	}

	/* 页不是新页或空页 -- 保留锁和钉住 */
	return false;
}

/*
 *	lazy_scan_prune() -- lazy_scan_heap() 的剪枝与冻结。
 *
 * 调用方必须钉住该缓冲区并持有缓冲区清理锁。
 *
 * PostgreSQL 14 之前，有极罕见的情况允许 heap_page_prune()
 * 与我们的 HeapTupleSatisfiesVacuum() 调用在
 * 一个元组是否应视为 DEAD 上意见不一。这发生在某个
 * 插入事务并发中止时（在我们的 heap_page_prune()
 * 调用之后、HeapTupleSatisfiesVacuum() 调用之前）。当时有相当多
 * 的复杂性，只为了处理对 VACUUM 而言已是 DEAD、
 * 但剪枝之后仍留有存储的元组。
 *
 * 我们现在的做法是在检测到该竞态时
 * 重新开始剪枝。这让 heap_page_prune() 能够剪掉
 * 现已中止的事务所插入的元组。这有些粗略，但它保证
 * 进入 dead_items 数组的任何项都是简单的 LP_DEAD
 * 行指针，并且每一个仍带元组存储的剩余项
 * 都被当作冻结候选。
 */
static void
lazy_scan_prune(LVRelState *vacrel,
				Buffer buf,
				BlockNumber blkno,
				Page page,
				LVPagePruneState *prunestate)
{
	Relation	rel = vacrel->rel;
	OffsetNumber offnum,
				maxoff;
	ItemId		itemid;
	HeapTupleData tuple;
	HTSV_Result res;
	int			tuples_deleted,
				tuples_frozen,
				lpdead_items,
				live_tuples,
				recently_dead_tuples;
	int			nnewlpdead;
	HeapPageFreeze pagefrz;
	int64		fpi_before = pgWalUsage.wal_fpi;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];
	HeapTupleFreeze frozen[MaxHeapTuplesPerPage];

	Assert(BufferGetBlockNumber(buf) == blkno);

	/*
	 * heap_page_prune 中的行指针数组截断之后，maxoff 可能会减小。
	 * 我们可以安全地忽略这一点，因为回收的
	 * 空间在下面看起来仍会像 LP_UNUSED 项。
	 */
	maxoff = PageGetMaxOffsetNumber(page);

retry:

	/* 初始化（或重置）页级状态 */
	pagefrz.freeze_required = false;
	pagefrz.FreezePageRelfrozenXid = vacrel->NewRelfrozenXid;
	pagefrz.FreezePageRelminMxid = vacrel->NewRelminMxid;
	pagefrz.NoFreezePageRelfrozenXid = vacrel->NewRelfrozenXid;
	pagefrz.NoFreezePageRelminMxid = vacrel->NewRelminMxid;
	tuples_deleted = 0;
	tuples_frozen = 0;
	lpdead_items = 0;
	live_tuples = 0;
	recently_dead_tuples = 0;

	/*
	 * 剪枝此页上的所有 HOT 更新链。
	 *
	 * 我们把剪枝步骤移除的元组计为 tuples_deleted。它的
	 * 最终值可以理解为已从该表
	 * 删除的元组数。不应把它和 lpdead_items 搞混；
	 * lpdead_items 的最终值可以理解为已从索引
	 * 删除的元组数。
	 */
	tuples_deleted = heap_page_prune(rel, buf, vacrel->cutoffs.OldestXmin,
									 vacrel->vistest,
									 InvalidTransactionId, 0, &nnewlpdead,
									 &vacrel->offnum);

	/*
	 * 现在扫描该页以收集 LP_DEAD 项，并检查仍带存储的
	 * 剩余元组中需要冻结的元组
	 */
	prunestate->hastup = false;
	prunestate->has_lpdead_items = false;
	prunestate->all_visible = true;
	prunestate->all_frozen = true;
	prunestate->visibility_cutoff_xid = InvalidTransactionId;

	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		bool		totally_frozen;

		/*
		 * 设置偏移号，以便在处理此元组时若发生
		 * 任何错误，可以把它一起显示出来。
		 */
		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsUsed(itemid))
			continue;

		/* 不得触碰重定向项 */
		if (ItemIdIsRedirected(itemid))
		{
			/* 此页使关系截断不安全 */
			prunestate->hastup = true;
			continue;
		}

		if (ItemIdIsDead(itemid))
		{
			/*
			 * 有意不为 LP_DEAD 项设置 hastup。我们做一个
			 * 宽松假设：这里遇到的任何 LP_DEAD 项稍后都会
			 * 变成 LP_UNUSED，且发生在到达 count_nondeletable_pages
			 * 之前。若不做这个假设，关系截断
			 * 最多只会每隔一次 VACUUM 发生。此外，无论
			 * LP_DEAD 项如何处理（在这里处理，或稍后处理），VACUUM
			 * 都必须把 hastup/nonempty_pages 当作临时值。
			 *
			 * 也有意把清除 all_visible 推迟到
			 * 返回 lazy_scan_heap 调用方的前一刻，下文会完整解释。
			 * （这是另一个有用的情形：预期任何
			 * LP_DEAD 项都会在本次 VACUUM 期间变成 LP_UNUSED。）
			 */
			deadoffsets[lpdead_items++] = offnum;
			continue;
		}

		Assert(ItemIdIsNormal(itemid));

		ItemPointerSet(&(tuple.t_self), blkno, offnum);
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(rel);

		/*
		 * DEAD 元组几乎总是被 heap_page_prune() 剪成 LP_DEAD 行指针，
		 * 但自 heap_page_prune() 查看以来，元组状态有可能已经
		 * 改变。这里通过重新开始来处理。
		 * （完整解释见函数顶部的注释。）
		 */
		res = HeapTupleSatisfiesVacuum(&tuple, vacrel->cutoffs.OldestXmin,
									   buf);

		if (unlikely(res == HEAPTUPLE_DEAD))
			goto retry;

		/*
		 * 在本块中把元组计为活元组的标准需要
		 * 与 analyze.c 的 acquire_sample_rows() 一致，否则 VACUUM
		 * 和 ANALYZE 可能产生差异极大的 reltuples 值，例如
		 * 有许多最近死亡元组时。
		 *
		 * 这里的逻辑比 acquire_sample_rows() 简单一些，因为
		 * VACUUM 不能在事务块内运行，这使某些情形
		 * 不可能发生（例如同一事务中进行中的插入）。
		 *
		 * 我们也区别对待 LP_DEAD 项（它们是这里可能见到的
		 * 最接近 DEAD 元组的东西）：我们假设它们会在
		 * VACUUM 结束前变成 LP_UNUSED。这种差别只是
		 * 表面的。最终 VACUUM 在 DEAD
		 * 项上实际上与 ANALYZE 一致。VACUUM 不会记住 LP_DEAD 项，但这只是
		 * 因为它们在 VACUUM 结束时不应当被留下。
		 * （绕过索引清理的情形会违反这一乐观
		 * 假设，但其总体影响应当可以忽略。）
		 */
		switch (res)
		{
			case HEAPTUPLE_LIVE:

				/*
				 * 把它计为活的。这不仅自然，而且也是
				 * acquire_sample_rows() 的做法。
				 */
				live_tuples++;

				/*
				 * 该元组是否肯定对所有事务可见？
				 *
				 * 注意：和逐元组提示位一样，若插入者
				 * 异步提交，我们不能设置
				 * PD_ALL_VISIBLE 标志。详见 SetHintBits。因此要检查
				 * 该元组的提示位表明 xmin 已提交。
				 */
				if (prunestate->all_visible)
				{
					TransactionId xmin;

					if (!HeapTupleHeaderXminCommitted(tuple.t_data))
					{
						prunestate->all_visible = false;
						break;
					}

					/*
					 * 插入者肯定已提交。但它是否足够旧，
					 * 以至于所有人都把它看作已提交？
					 */
					xmin = HeapTupleHeaderGetXmin(tuple.t_data);
					if (!TransactionIdPrecedes(xmin,
											   vacrel->cutoffs.OldestXmin))
					{
						prunestate->all_visible = false;
						break;
					}

					/* 跟踪该页上最新的 xmin。 */
					if (TransactionIdFollows(xmin, prunestate->visibility_cutoff_xid) &&
						TransactionIdIsNormal(xmin))
						prunestate->visibility_cutoff_xid = xmin;
				}
				break;
			case HEAPTUPLE_RECENTLY_DEAD:

				/*
				 * 若元组是最近死亡的，则不得把它从
				 * 关系中移除。（我们只移除剪枝得到的
				 * LP_DEAD 项。）
				 */
				recently_dead_tuples++;
				prunestate->all_visible = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 我们不把这些行计为活的，因为我们预期
				 * 插入事务会在提交时更新计数器，并且
				 * 我们假设那只会在我们报告
				 * 结果之后发生。这个假设不太牢靠，但这正是
				 * acquire_sample_rows() 的做法，所以保持一致。
				 */
				prunestate->all_visible = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				/* 这是并发清理期间的预期情形 */
				prunestate->all_visible = false;

				/*
				 * 把这样的行计为活的。同上，我们假设删除
				 * 事务会在我们报告之后提交并更新
				 * 计数器。
				 */
				live_tuples++;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		prunestate->hastup = true;	/* 此页使关系截断不安全 */

		/* 带存储的元组 -- 考虑是否需要冻结 */
		if (heap_prepare_freeze_tuple(tuple.t_data, &vacrel->cutoffs, &pagefrz,
									  &frozen[tuples_frozen], &totally_frozen))
		{
			/* 保存准备好的冻结计划，供以后使用 */
			frozen[tuples_frozen++].offset = offnum;
		}

		/*
		 * 若任何元组既不是已经完全冻结，也没有资格
		 * 变成完全冻结（按其冻结计划），那么该页
		 * 以后肯定无法在可见性映射中设为全冻结
		 */
		if (!totally_frozen)
			prunestate->all_frozen = false;
	}

	/*
	 * 现在该页上的每一项都被分成：要么是稍后需要
	 * 在索引中清理的 LP_DEAD 项，要么是留下并需要
	 * 现在考虑冻结的 LP_NORMAL 元组（LP_UNUSED 和
	 * LP_REDIRECT 项也会留下，但我们对它们没有进一步兴趣）。
	 */
	vacrel->offnum = InvalidOffsetNumber;

	/*
	 * 当 heap_prepare_freeze_tuple 表明至少存在一个
	 * 早于 FreezeLimit/MultiXactCutoff 的 XID/MXID 时，冻结该页。若剪枝
	 * 产生了 FPI，且这样做意味着我们随后把该页
	 * 设为全冻结，也要冻结（也许要到最后一次堆扫描才发生）。
	 */
	if (pagefrz.freeze_required || tuples_frozen == 0 ||
		(prunestate->all_visible && prunestate->all_frozen &&
		 fpi_before != pgWalUsage.wal_fpi))
	{
		/*
		 * 我们正在冻结该页。最终的 NewRelfrozenXid 不需要
		 * 受那些马上就要被冻结的 XIDs 影响。
		 */
		vacrel->NewRelfrozenXid = pagefrz.FreezePageRelfrozenXid;
		vacrel->NewRelminMxid = pagefrz.FreezePageRelminMxid;

		if (tuples_frozen == 0)
		{
			/*
			 * 我们没有要执行的冻结计划，因此走冻结路径
			 * 没有额外代价。这就是选择它的原因。这一点
			 * 在该页此刻只含有完全
			 * 冻结元组时很重要（也许只是在剪枝之后）。
			 * 调用方仍可在 VM 中把这样的页标为全冻结，
			 * 即使这里没有任何元组是新冻结的（注意
			 * "no freeze" 路径从不把页设为全冻结）。
			 *
			 * 我们在这里从不增加 frozen_pages 测量计数器，
			 * 因为它只统计含有新冻结元组的页
			 * （不要把它和 VM 中新设为全冻结的页搞混）。
			 */
		}
		else
		{
			TransactionId snapshotConflictHorizon;

			vacrel->frozen_pages++;

			/*
			 * 当整页有资格在我们处理完之后
			 * 在 VM 中变成全冻结时，我们可以用 visibility_cutoff_xid 作为冲突
			 * 截止值。否则我们通过从 OldestXmin
			 * 回退来生成一个保守的截止值。
			 */
			if (prunestate->all_visible && prunestate->all_frozen)
			{
				/* 现在设置 VM 时不必再用同一个截止值 */
				snapshotConflictHorizon = prunestate->visibility_cutoff_xid;
				prunestate->visibility_cutoff_xid = InvalidTransactionId;
			}
			else
			{
				/* 在使用 hot_standby_feedback 时避免虚假冲突 */
				snapshotConflictHorizon = vacrel->cutoffs.OldestXmin;
				TransactionIdRetreat(snapshotConflictHorizon);
			}

			/* 把该页的所有冻结计划作为一次原子动作执行 */
			heap_freeze_execute_prepared(vacrel->rel, buf,
										 snapshotConflictHorizon,
										 frozen, tuples_frozen);
		}
	}
	else
	{
		/*
		 * 该页需要 "no freeze" 处理。它也许可以在
		 * 可见性映射中设为全可见，但绝不能设为全冻结。
		 */
		vacrel->NewRelfrozenXid = pagefrz.NoFreezePageRelfrozenXid;
		vacrel->NewRelminMxid = pagefrz.NoFreezePageRelminMxid;
		prunestate->all_frozen = false;
		tuples_frozen = 0;		/* 避免测量中的计数错误 */
	}

	/*
	 * VACUUM 会在对堆的第二次扫描期间调用 heap_page_is_all_visible()，
	 * 以确定该页的 all_visible 和 all_frozen -- 这是
	 * 本函数逻辑的一个专用版本。现在
	 * 剪枝和冻结已经完成，用断言确保我们与
	 * heap_page_is_all_visible() 完全一致。
	 */
#ifdef USE_ASSERT_CHECKING
	/* 注意当 !all_visible 时 all_frozen 的值无关紧要 */
	if (prunestate->all_visible && lpdead_items == 0)
	{
		TransactionId cutoff;
		bool		all_frozen;

		if (!heap_page_is_all_visible(vacrel, buf, &cutoff, &all_frozen))
			Assert(false);

		Assert(!TransactionIdIsValid(cutoff) ||
			   cutoff == prunestate->visibility_cutoff_xid);
	}
#endif

	/*
	 * 现在把该页上 LP_DEAD 项的细节保存到 vacrel 中
	 */
	if (lpdead_items > 0)
	{
		VacDeadItems *dead_items = vacrel->dead_items;
		ItemPointerData tmp;

		vacrel->lpdead_item_pages++;
		prunestate->has_lpdead_items = true;

		ItemPointerSetBlockNumber(&tmp, blkno);

		for (int i = 0; i < lpdead_items; i++)
		{
			ItemPointerSetOffsetNumber(&tmp, deadoffsets[i]);
			dead_items->items[dead_items->num_items++] = tmp;
		}

		Assert(dead_items->num_items <= dead_items->max_items);
		pgstat_progress_update_param(PROGRESS_VACUUM_NUM_DEAD_TUPLES,
									 dead_items->num_items);

		/*
		 * 早先在 all_visible 中忽略 LP_DEAD 项比较方便，
		 * 这样是否冻结该页的选择就不会受
		 * LP_DEAD 项的短期存在影响。这些 LP_DEAD 项
		 * 实际上被假设为即将变成的 LP_UNUSED 项。只要本次 VACUUM
		 * 做到这一点，由哪一次堆扫描（初始扫描或最终扫描）
		 * 最终把该页设为全冻结并不重要。
		 *
		 * 冻结已经定案，现在清除 all_visible。它应当
		 * 反映当前状态，正如调用方所预期的。
		 */
		prunestate->all_visible = false;
	}

	/* 最后，把页内计数加到整个 VACUUM 的计数上 */
	vacrel->tuples_deleted += tuples_deleted;
	vacrel->tuples_frozen += tuples_frozen;
	vacrel->lpdead_items += lpdead_items;
	vacrel->live_tuples += live_tuples;
	vacrel->recently_dead_tuples += recently_dead_tuples;
}

/*
 *	lazy_scan_noprune() -- 不做剪枝或冻结的 lazy_scan_prune()
 *
 * 调用方只需钉住缓冲区并持有共享锁，不像
 * lazy_scan_prune 那样需要完整的清理锁。这里虽然不做
 * 剪枝，但更早的机会性剪枝
 * 操作很有可能留下了 LP_DEAD 项。我们至少会把任何这样的项
 * 收集到 dead_items 数组中，以便从索引中移除。
 *
 * 对积极 VACUUM 的调用方，我们可能返回假，表示需要完整的
 * 清理锁，以便由 lazy_scan_prune 处理。这只在
 * 积极 VACUUM 需要冻结该页上一个或多个
 * 元组中的某些元组 XIDs 时才有必要。对非积极
 * 调用方，我们总是返回真。
 *
 * hastup 返回标志的解释见 lazy_scan_prune。
 * recordfreespace 标志指示调用方是否应对该页做
 * 常规的 FSM 处理。
 */
static bool
lazy_scan_noprune(LVRelState *vacrel,
				  Buffer buf,
				  BlockNumber blkno,
				  Page page,
				  bool *hastup,
				  bool *recordfreespace)
{
	OffsetNumber offnum,
				maxoff;
	int			lpdead_items,
				live_tuples,
				recently_dead_tuples,
				missed_dead_tuples;
	HeapTupleHeader tupleheader;
	TransactionId NoFreezePageRelfrozenXid = vacrel->NewRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid = vacrel->NewRelminMxid;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];

	Assert(BufferGetBlockNumber(buf) == blkno);

	*hastup = false;			/* 暂时如此 */
	*recordfreespace = false;	/* 暂时如此 */

	lpdead_items = 0;
	live_tuples = 0;
	recently_dead_tuples = 0;
	missed_dead_tuples = 0;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		HeapTupleData tuple;

		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsUsed(itemid))
			continue;

		if (ItemIdIsRedirected(itemid))
		{
			*hastup = true;
			continue;
		}

		if (ItemIdIsDead(itemid))
		{
			/*
			 * 这里有意不设置 hastup=true。解释见
			 * lazy_scan_prune 中的同一处。
			 */
			deadoffsets[lpdead_items++] = offnum;
			continue;
		}

		*hastup = true;			/* 此页阻止关系截断 */
		tupleheader = (HeapTupleHeader) PageGetItem(page, itemid);
		if (heap_tuple_should_freeze(tupleheader, &vacrel->cutoffs,
									 &NoFreezePageRelfrozenXid,
									 &NoFreezePageRelminMxid))
		{
			/* 元组的 XID < FreezeLimit（或 MXID < MultiXactCutoff） */
			if (vacrel->aggressive)
			{
				/*
				 * 积极 VACUUMs 必须始终能把该关系的
				 * relfrozenxid 推进到 >= FreezeLimit 的值（并且能把
				 * 该关系的 relminmxid 推进到 >= MultiXactCutoff 的值）。
				 * 正在进行的积极 VACUUM 做不到这一点，
				 * 除非它现在就能冻结此元组中的一个 XID（或 MXID）。
				 *
				 * 唯一安全的选择是让调用方用 lazy_scan_prune
				 * 处理此页。调用方也许不得不等待
				 * 一会儿才能取得清理锁，但这是没办法的。
				 */
				vacrel->offnum = InvalidOffsetNumber;
				return false;
			}

			/*
			 * 非积极 VACUUMs 没有义务推进
			 * relfrozenxid（哪怕一个 XID）。这里我们可以宽松得多。
			 *
			 * 目前我们总是直接接受一个更旧的最终 relfrozenxid
			 * 和/或 relminmxid 值。我们从不让调用方等待或
			 * 多做一点工作，即使这样做很可能有意义。
			 */
		}

		ItemPointerSet(&(tuple.t_self), blkno, offnum);
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(vacrel->rel);

		switch (HeapTupleSatisfiesVacuum(&tuple, vacrel->cutoffs.OldestXmin,
										 buf))
		{
			case HEAPTUPLE_DELETE_IN_PROGRESS:
			case HEAPTUPLE_LIVE:

				/*
				 * 两种情形都计为活的，就像 lazy_scan_prune 那样
				 */
				live_tuples++;

				break;
			case HEAPTUPLE_DEAD:

				/*
				 * 有一些对剪枝有用的工作，由于
				 * 未能取得清理锁而不会完成。
				 */
				missed_dead_tuples++;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:

				/*
				 * 计入 recently_dead_tuples，就像 lazy_scan_prune 那样
				 */
				recently_dead_tuples++;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 不要把这些行计为活的，就像 lazy_scan_prune 那样
				 */
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}

	vacrel->offnum = InvalidOffsetNumber;

	/*
	 * 到这里我们可以确定，调用方可以把此特定页的冻结和剪枝
	 * 推迟到下一次 VACUUM。现在记住它的细节。
	 * （lazy_scan_prune 期望一块干净的初始状态，所以我们必须最后做这件事。）
	 */
	vacrel->NewRelfrozenXid = NoFreezePageRelfrozenXid;
	vacrel->NewRelminMxid = NoFreezePageRelminMxid;

	/* 把该页上找到的任何 LP_DEAD 项保存到 dead_items 数组中 */
	if (vacrel->nindexes == 0)
	{
		/* 使用单遍策略（因为表没有索引） */
		if (lpdead_items > 0)
		{
			/*
			 * 对单遍
			 * 策略 VACUUM 无法取得清理锁、并且结果
			 * 有一个或多个 LP_DEAD 项这一边角情况，做权宜处理：只把这些 LP_DEAD
			 * 项改计为 missed_dead_tuples。（这有点不够诚实，
			 * 但也好过永远维护专门的堆清理代码，
			 * 因为好处微乎其微。）
			 */
			*hastup = true;
			missed_dead_tuples += lpdead_items;
		}

		*recordfreespace = true;
	}
	else if (lpdead_items == 0)
	{
		/*
		 * 以后不会清理此页，所以现在把该页的空闲空间记入
		 * FSM
		 */
		*recordfreespace = true;
	}
	else
	{
		VacDeadItems *dead_items = vacrel->dead_items;
		ItemPointerData tmp;

		/*
		 * 该页有 LP_DEAD 项，因此索引中残留的任何引用/TIDs
		 * 都会在索引清理期间被删除（然后在堆中标为
		 * LP_UNUSED）
		 */
		vacrel->lpdead_item_pages++;

		ItemPointerSetBlockNumber(&tmp, blkno);

		for (int i = 0; i < lpdead_items; i++)
		{
			ItemPointerSetOffsetNumber(&tmp, deadoffsets[i]);
			dead_items->items[dead_items->num_items++] = tmp;
		}

		Assert(dead_items->num_items <= dead_items->max_items);
		pgstat_progress_update_param(PROGRESS_VACUUM_NUM_DEAD_TUPLES,
									 dead_items->num_items);

		vacrel->lpdead_items += lpdead_items;

		/*
		 * 假设我们将在对堆的最终扫描期间
		 * 继续清理此堆页。在那之前不要记录空闲空间。
		 */
		*recordfreespace = false;
	}

	/*
	 * 最后，把相关的页内计数加到整个 VACUUM 的计数上
	 */
	vacrel->live_tuples += live_tuples;
	vacrel->recently_dead_tuples += recently_dead_tuples;
	vacrel->missed_dead_tuples += missed_dead_tuples;
	if (missed_dead_tuples > 0)
		vacrel->missed_dead_pages++;

	/* 调用方不需要再对同一页调用 lazy_scan_prune */
	return true;
}

/*
 * 索引清理和堆清理的主入口。
 *
 * 从该表的索引中移除 dead_items 里收集的项，然后把
 * 同样的项在堆中标为 LP_UNUSED。完整细节见
 * lazy_scan_heap 上方的注释。
 *
 * 同时清空 dead_items，为以后的 TIDs 腾出空间。
 *
 * 此时我们可以选择绕过索引清理，但仅当本次
 * VACUUM 操作肯定只会有一次索引扫描/一轮
 * 索引清理时。
 */
static void
lazy_vacuum(LVRelState *vacrel)
{
	bool		bypass;

	/* 没有索引时不应当走到这里 */
	Assert(vacrel->nindexes > 0);
	Assert(vacrel->lpdead_item_pages > 0);

	if (!vacrel->do_index_vacuuming)
	{
		Assert(!vacrel->do_index_cleanup);
		vacrel->dead_items->num_items = 0;
		return;
	}

	/*
	 * 考虑完全绕过索引清理（以及堆清理）。
	 *
	 * 目前我们只在整个 VACUUM 操作的 LP_DEAD 项
	 * 数量接近零时才这样做。这避免针对同一张表、
	 * 固定工作负载连续运行的 VACUUM
	 * 操作在持续时间和开销上出现陡峭
	 * 间断。理想情况下，在接近零时，连续的 VACUUM 操作
	 * 应当表现得好像 LP_DEAD 项
	 *
	 * 恰好为零。
	 * 这对于持续受到
	 * 大多能应用 HOT 优化的 UPDATEs 影响、但偶尔
	 * 出现小偏差、导致少数堆页只保留
	 * 一两个 LP_DEAD 项的表可能有帮助。这相当常见；即使
	 * DBA 特意让 UPDATE 使用 HOT，实际上
	 * 也不可能预测 HOT 是否会在 100% 的情况下被应用。
	 * 通过仔细调节，确保一张表上 99%+ 的 UPDATE 都使用 HOT，要容易得多。
	 */
	bypass = false;
	if (vacrel->consider_bypass_optimization && vacrel->rel_pages > 0)
	{
		BlockNumber threshold;

		Assert(vacrel->num_index_scans == 0);
		Assert(vacrel->lpdead_items == vacrel->dead_items->num_items);
		Assert(vacrel->do_index_vacuuming);
		Assert(vacrel->do_index_cleanup);

		/*
		 * 我们将开始做索引清理的这个转折点，
		 * 表示为该表中已知至少有一个 LP_DEAD 项的
		 * 堆页总数的百分比。这比
		 * LP_DEAD 项的总数重要得多，因为它
		 * 是那些由于绕过索引清理和堆清理而无法设置
		 * 可见性映射位的堆页数量的代理。
		 *
		 * 我们再加一项预防性测试：当前用于
		 * 存放 TIDs 的空间（这些 TID 现在都指向 LP_DEAD 项）不得
		 * 超过 32MB。这限制了我们一遍又一遍绕过索引
		 * 清理、直到最终某次 VACUUM 的
		 * dead_items 空间不再驻留于 CPU 缓存的风险。
		 *
		 * 应用该优化时，我们不采取任何特殊步骤去记住 LP_DEAD 项
		 * （例如在对统计系统的最终更新中清点它们）。
		 * 尽管 analyze.c 的
		 * acquire_sample_rows() 所用的记账会在它自己的统计报告中把同样的 LP_DEAD 项认作死
		 * 行，那也没关系。差异应当
		 * 可以忽略。若这项优化将来扩大到覆盖更多
		 * 情形，这一点也许需要重新考虑。
		 */
		threshold = (double) vacrel->rel_pages * BYPASS_THRESHOLD_PAGES;
		bypass = (vacrel->lpdead_item_pages < threshold &&
				  vacrel->lpdead_items < MAXDEADITEMS(32L * 1024L * 1024L));
	}

	if (bypass)
	{
		/*
		 * TIDs 几乎为零。表现得好像恰好
		 * 为零：绕过索引清理，但做索引善后。
		 *
		 * 我们预期本次 VACUUM 操作会很快
		 * 结束，所以没有必要考虑把它当作针对回卷失败的
		 * 故障安全来加速。（在没有 ambulkdelete()
		 * 调用的情形下，索引善后预期会
		 * 很快结束。）
		 */
		vacrel->do_index_vacuuming = false;
	}
	else if (lazy_vacuum_all_indexes(vacrel))
	{
		/*
		 * 我们成功完成了一轮索引清理。现在做相关的
		 * 堆清理。
		 */
		lazy_vacuum_heap_rel(vacrel);
	}
	else
	{
		/*
		 * 故障安全情形。
		 *
		 * 我们尝试了索引清理，但没有完成完整一轮/完整
		 * 索引扫描。这发生在 relfrozenxid 或 relminmxid
		 * 过于久远时。
		 *
		 * 从此以后，本次 VACUUM 操作不再做进一步的索引
		 * 清理或堆清理。本次 VACUUM 操作不会
		 * 再次回到这里。
		 */
		Assert(VacuumFailsafeActive);
	}

	/*
	 * 忘掉我们刚刚清理掉的（或刚刚决定不
	 * 清理的）LP_DEAD 项
	 */
	vacrel->dead_items->num_items = 0;
}

/*
 *	lazy_vacuum_all_indexes() -- 索引清理的主入口
 *
 * 在常见情形下，当所有索引都成功
 * 清理时返回真。在罕见情形下，若我们判定本次
 * VACUUM 操作有耗时过长、导致
 * 回卷失败的风险，则返回假。
 */
static bool
lazy_vacuum_all_indexes(LVRelState *vacrel)
{
	bool		allindexes = true;
	double		old_live_tuples = vacrel->rel->rd_rel->reltuples;

	Assert(vacrel->nindexes > 0);
	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);

	/* XID 回卷紧急情况的预检 */
	if (lazy_check_wraparound_failsafe(vacrel))
	{
		/* 回卷紧急情况 -- 甚至不要开始索引扫描 */
		return false;
	}

	/* 报告我们现在正在清理索引 */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_INDEX);

	if (!ParallelVacuumIsActive(vacrel))
	{
		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] = lazy_vacuum_one_index(indrel, istat,
														  old_live_tuples,
														  vacrel);

			if (lazy_check_wraparound_failsafe(vacrel))
			{
				/* 回卷紧急情况 -- 结束当前索引扫描 */
				allindexes = false;
				break;
			}
		}
	}
	else
	{
		/* 把一切都交给并行变体 */
		parallel_vacuum_bulkdel_all_indexes(vacrel->pvs, old_live_tuples,
											vacrel->num_index_scans);

		/*
		 * 做一次事后检查，考虑现在是否应用回卷故障安全。注意
		 * 并行 VACUUM 只有这次预检和这次事后检查。
		 */
		if (lazy_check_wraparound_failsafe(vacrel))
			allindexes = false;
	}

	/*
	 * 每次在这里调用时，我们都从所有索引中删除第一次堆扫描的全部 LP_DEAD 项
	 * （选择做故障安全的那些调用除外）。这
	 * 使下一次 lazy_vacuum_heap_rel() 调用是安全的（除非
	 * 故障安全触发，从而阻止下一次调用
	 * 发生）。
	 */
	Assert(vacrel->num_index_scans > 0 ||
		   vacrel->dead_items->num_items == vacrel->lpdead_items);
	Assert(allindexes || VacuumFailsafeActive);

	/*
	 * 增加并报告索引扫描次数。
	 *
	 * 我们有意包括已经开始一轮批量
	 * 删除、但由于故障安全触发而未能完成的情形。
	 */
	vacrel->num_index_scans++;
	pgstat_progress_update_param(PROGRESS_VACUUM_NUM_INDEX_VACUUMS,
								 vacrel->num_index_scans);

	return allindexes;
}

/*
 *	lazy_vacuum_heap_rel() -- 两遍策略中对堆的第二次扫描
 *
 * 本例程把 vacrel->dead_items 数组中的 LP_DEAD 项标为 LP_UNUSED。
 * lazy_scan_prune 从未记下过 LP_DEAD 项的页
 * 完全不会被访问。
 *
 * 我们也许还能截断所访问堆页的行指针数组。
 * 若数组末尾有一组连续的 LP_UNUSED 项，
 * 它可以作为空闲空间回收。这些 LP_UNUSED 项通常
 * 起初是 lazy_scan_prune 记下的 LP_DEAD 项（我们把
 * 每一页的项设为 LP_UNUSED，然后考虑是否可以截断
 * 该页的行指针数组）。
 *
 * 注意：之所以作为第二次扫描来做，是因为在移除索引项之前
 * 我们无法移除元组，而且我们希望
 * 尽可能大批地处理索引项的移除。
 */
static void
lazy_vacuum_heap_rel(LVRelState *vacrel)
{
	int			index = 0;
	BlockNumber vacuumed_pages = 0;
	Buffer		vmbuffer = InvalidBuffer;
	LVSavedErrInfo saved_err_info;

	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);
	Assert(vacrel->num_index_scans > 0);

	/* 报告我们现在正在清理堆 */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_HEAP);

	/* 更新错误回溯信息 */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	while (index < vacrel->dead_items->num_items)
	{
		BlockNumber blkno;
		Buffer		buf;
		Page		page;
		Size		freespace;

		vacuum_delay_point();

		blkno = ItemPointerGetBlockNumber(&vacrel->dead_items->items[index]);
		vacrel->blkno = blkno;

		/*
		 * 钉住可见性映射页，以防需要把该页标为
		 * 全可见。多数情况下这非常便宜，因为我们
		 * 反正已经钉住了正确的页。
		 */
		visibilitymap_pin(vacrel->rel, blkno, &vmbuffer);

		/* 我们需要非清理用的排他锁，以便把 dead_items 标为未使用 */
		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 vacrel->bstrategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		index = lazy_vacuum_heap_page(vacrel, blkno, buf, index, vmbuffer);

		/* 既然已经清理了该页，就记录它的可用空间 */
		page = BufferGetPage(buf);
		freespace = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		vacuumed_pages++;
	}

	vacrel->blkno = InvalidBlockNumber;
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * 我们在第二次堆扫描期间把第一次堆扫描的全部 LP_DEAD 项设为 LP_UNUSED。
	 * 不多也不少。
	 */
	Assert(index > 0);
	Assert(vacrel->num_index_scans > 1 ||
		   (index == vacrel->lpdead_items &&
			vacuumed_pages == vacrel->lpdead_item_pages));

	ereport(DEBUG2,
			(errmsg("table \"%s\": removed %lld dead item identifiers in %u pages",
					vacrel->relname, (long long) index, vacuumed_pages)));

	/* 恢复先前的阶段信息，供错误回溯使用 */
	restore_vacuum_error_info(vacrel, &saved_err_info);
}

/*
 *	lazy_vacuum_heap_page() -- 释放 vacrel->dead_items 数组中
 *						  列出的该页 LP_DEAD 项。
 *
 * 调用方必须在该缓冲区上持有排他缓冲区锁（完整的
 * 清理锁也可以）。vmbuffer 必须有效，并且已经
 * 钉住 blkno 的可见性映射页。
 *
 * index 是 vacrel->dead_items 数组中该页第一个列出的
 * LP_DEAD 项的偏移。返回值是数组中同一页全部
 * LP_DEAD 项之后紧接着的第一个下标。
 */
static int
lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno, Buffer buffer,
					  int index, Buffer vmbuffer)
{
	VacDeadItems *dead_items = vacrel->dead_items;
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxHeapTuplesPerPage];
	int			nunused = 0;
	TransactionId visibility_cutoff_xid;
	bool		all_frozen;
	LVSavedErrInfo saved_err_info;

	Assert(vacrel->nindexes == 0 || vacrel->do_index_vacuuming);

	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* 更新错误回溯信息 */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP, blkno,
							 InvalidOffsetNumber);

	START_CRIT_SECTION();

	for (; index < dead_items->num_items; index++)
	{
		BlockNumber tblk;
		OffsetNumber toff;
		ItemId		itemid;

		tblk = ItemPointerGetBlockNumber(&dead_items->items[index]);
		if (tblk != blkno)
			break;				/* 已超过本块元组的末尾 */
		toff = ItemPointerGetOffsetNumber(&dead_items->items[index]);
		itemid = PageGetItemId(page, toff);

		Assert(ItemIdIsDead(itemid) && !ItemIdHasStorage(itemid));
		ItemIdSetUnused(itemid);
		unused[nunused++] = toff;
	}

	Assert(nunused > 0);

	/* 现在尝试截断行指针数组 */
	PageTruncateLinePointerArray(page);

	/*
	 * 在写 WAL 之前把缓冲区标脏。
	 */
	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(vacrel->rel))
	{
		xl_heap_vacuum xlrec;
		XLogRecPtr	recptr;

		xlrec.nunused = nunused;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapVacuum);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) unused, nunused * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VACUUM);

		PageSetLSN(page, recptr);
	}

	/*
	 * 结束临界区，这样我们就能安全地做可见性测试（它们
	 * 可能需要执行 I/O 并分配内存！）。若我们现在崩溃，该
	 * 页（包括对应的 vm 位）也许不会被标为全
	 * 可见，但那没关系。以后的清理会修好。
	 */
	END_CRIT_SECTION();

	/*
	 * 既然已经从该页移除了 LP_DEAD 项，再次
	 * 检查该页是否已变成全可见。该页已经被标
	 * 脏、排他锁定，并且如果需要，已经发出了
	 * 整页映像。
	 */
	Assert(!PageIsAllVisible(page));
	if (heap_page_is_all_visible(vacrel, buffer, &visibility_cutoff_xid,
								 &all_frozen))
	{
		uint8		flags = VISIBILITYMAP_ALL_VISIBLE;

		if (all_frozen)
		{
			Assert(!TransactionIdIsValid(visibility_cutoff_xid));
			flags |= VISIBILITYMAP_ALL_FROZEN;
		}

		PageSetAllVisible(page);
		visibilitymap_set(vacrel->rel, blkno, buffer, InvalidXLogRecPtr,
						  vmbuffer, visibility_cutoff_xid, flags);
	}

	/* 恢复先前的阶段信息，供错误回溯使用 */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	return index;
}

/*
 * 当 vacrel 表的 relfrozenxid 和/或 relminmxid
 * 危险地过于久远时，触发故障安全以避免回卷失败。
 * 触发故障安全会使本次 VACUUM 绕过任何进一步的索引
 * 清理和堆清理。堆截断也会被绕过。
 *
 * 任何剩余工作（VACUUM 无法直接绕过的工作）通常会在
 * 故障安全触发时被加速。VACUUM 停止应用它开始时
 * 采用的任何基于代价的延迟。
 *
 * 故障安全已触发时返回真。
 */
static bool
lazy_check_wraparound_failsafe(LVRelState *vacrel)
{
	/* 每次 VACUUM 不要警告超过一次 */
	if (VacuumFailsafeActive)
		return true;

	if (unlikely(vacuum_xid_failsafe_check(&vacrel->cutoffs)))
	{
		VacuumFailsafeActive = true;

		/*
		 * 放弃使用缓冲区访问策略，以便使用全部
		 * 共享缓冲区。我们假设分配 BufferAccessStrategy
		 * 内存的调用方会释放它。
		 */
		vacrel->bstrategy = NULL;

		/* 禁用索引清理、索引善后和堆关系截断 */
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
		vacrel->do_rel_truncate = false;

		ereport(WARNING,
				(errmsg("bypassing nonessential maintenance of table \"%s.%s.%s\" as a failsafe after %d index scans",
						vacrel->dbname, vacrel->relnamespace, vacrel->relname,
						vacrel->num_index_scans),
				 errdetail("The table's relfrozenxid or relminmxid is too far in the past."),
				 errhint("Consider increasing configuration parameter \"maintenance_work_mem\" or \"autovacuum_work_mem\".\n"
						 "You might also need to consider other ways for VACUUM to keep up with the allocation of transaction IDs.")));

		/* 从这里起停止应用代价限制 */
		VacuumCostActive = false;
		VacuumCostBalance = 0;

		return true;
	}

	return false;
}

/*
 *	lazy_cleanup_all_indexes() -- 对该关系的所有索引做善后。
 */
static void
lazy_cleanup_all_indexes(LVRelState *vacrel)
{
	double		reltuples = vacrel->new_rel_tuples;
	bool		estimated_count = vacrel->scanned_pages < vacrel->rel_pages;

	Assert(vacrel->do_index_cleanup);
	Assert(vacrel->nindexes > 0);

	/* 报告我们现在正在对索引做善后 */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_INDEX_CLEANUP);

	if (!ParallelVacuumIsActive(vacrel))
	{
		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] =
				lazy_cleanup_one_index(indrel, istat, reltuples,
									   estimated_count, vacrel);
		}
	}
	else
	{
		/* 把一切都交给并行变体 */
		parallel_vacuum_cleanup_all_indexes(vacrel->pvs, reltuples,
											vacrel->num_index_scans,
											estimated_count);
	}
}

/*
 *	lazy_vacuum_one_index() -- 清理索引关系。
 *
 *		删除所有包含 vacrel->dead_items 数组中
 *		收集到的 TID 的索引元组。同时更新运行中的统计信息。
 *		具体细节取决于索引 AM 的 ambulkdelete 例程。
 *
 *		reltuples 是要传给
 *		bulkdelete 回调的堆元组数。它总是被假定为估计值。
 *		更多信息见 indexam.sgml。
 *
 * 返回从输入统计信息导出的批量删除统计信息
 */
static IndexBulkDeleteResult *
lazy_vacuum_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					  double reltuples, LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	LVSavedErrInfo saved_err_info;

	ivinfo.index = indrel;
	ivinfo.heaprel = vacrel->rel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.estimated_count = true;
	ivinfo.message_level = DEBUG2;
	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/*
	 * 更新错误回溯信息。
	 *
	 * 本阶段保存索引名，并在本阶段
	 * 结束后立即恢复。见 vacuum_error_callback。
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_INDEX,
							 InvalidBlockNumber, InvalidOffsetNumber);

	/* 执行批量删除 */
	istat = vac_bulkdel_one_index(&ivinfo, istat, (void *) vacrel->dead_items);

	/* 恢复先前的阶段信息，供错误回溯使用 */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	return istat;
}

/*
 *	lazy_cleanup_one_index() -- 对索引关系做清理后的善后。
 *
 *		调用索引 AM 的 amvacuumcleanup 例程。reltuples 是堆元组
 *		数，若 reltuples 是估计值则 estimated_count 为真。
 *		更多信息见 indexam.sgml。
 *
 * 返回从输入统计信息导出的批量删除统计信息
 */
static IndexBulkDeleteResult *
lazy_cleanup_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					   double reltuples, bool estimated_count,
					   LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	LVSavedErrInfo saved_err_info;

	ivinfo.index = indrel;
	ivinfo.heaprel = vacrel->rel;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = false;
	ivinfo.estimated_count = estimated_count;
	ivinfo.message_level = DEBUG2;

	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/*
	 * 更新错误回溯信息。
	 *
	 * 本阶段保存索引名，并在本阶段
	 * 结束后立即恢复。见 vacuum_error_callback。
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	istat = vac_cleanup_one_index(&ivinfo, istat);

	/* 恢复先前的阶段信息，供错误回溯使用 */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	return istat;
}

/*
 * should_attempt_truncation - 我们是否应尝试截断堆？
 *
 * 除非我们有机会释放相当多的
 * 页，否则想都不要想。否则花费的时间不值得，主要因为
 * AccessExclusive 锁必须在任何热备上重放，在那里它可能
 * 特别扰人。
 *
 * 若回卷故障安全正在生效，也不要尝试。此时整个
 * 系统可能正在拒绝分配新的 XIDs。除非 VACUUM 真正推进
 * 最旧的 relfrozenxid，系统肯定不会恢复正常
 * -- 对目标 rel 来说这还没发生。
 * 若 lazy_truncate_heap 在这种情况下试图取得 AccessExclusiveLock 来
 * 截断该表，一次 XID 耗尽错误可能
 * 使 VACUUM 无法修复底层的 XID 耗尽问题。
 * 故障安全生效时，截断成功的机会
 * 本来就很小。lazy_scan_prune 做一个乐观假设：
 * 它遇到的任何 LP_DEAD 项在我们被调用时
 * 总会已经是 LP_UNUSED。
 *
 * 若我们正在做提前剪枝/清理，也不要尝试，因为
 * 找不到被截断堆页的扫描无法判定
 * 快照是否过旧而无法读取该页。
 */
static bool
should_attempt_truncation(LVRelState *vacrel)
{
	BlockNumber possibly_freeable;

	if (!vacrel->do_rel_truncate || VacuumFailsafeActive ||
		old_snapshot_threshold >= 0)
		return false;

	possibly_freeable = vacrel->rel_pages - vacrel->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrel->rel_pages / REL_TRUNCATE_FRACTION))
		return true;

	return false;
}

/*
 * lazy_truncate_heap - 尝试截掉末尾的任何空页
 */
static void
lazy_truncate_heap(LVRelState *vacrel)
{
	BlockNumber orig_rel_pages = vacrel->rel_pages;
	BlockNumber new_rel_pages;
	bool		lock_waiter_detected;
	int			lock_retry;

	/* 报告我们现在正在截断 */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_TRUNCATE);

	/* 最后一次更新错误回溯信息 */
	update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_TRUNCATE,
							 vacrel->nonempty_pages, InvalidOffsetNumber);

	/*
	 * 循环，直到不能再截断。
	 */
	do
	{
		/*
		 * 为了做截断，我们需要该关系上的完整排他
		 * 锁。若拿不到，就放弃而不是等待 --- 我们
		 * 不想阻塞其他后端，也不想死锁
		 * （考虑到我们已经持有较低级别的
		 * 锁，这很有可能）。
		 */
		lock_waiter_detected = false;
		lock_retry = 0;
		while (true)
		{
			if (ConditionalLockRelation(vacrel->rel, AccessExclusiveLock))
				break;

			/*
			 * 在尝试（重新）取得排他
			 * 锁时检查中断。
			 */
			CHECK_FOR_INTERRUPTS();

			if (++lock_retry > (VACUUM_TRUNCATE_LOCK_TIMEOUT /
								VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL))
			{
				/*
				 * 我们未能在指定的重试次数内建立起锁。
				 * 这意味着我们放弃截断。
				 */
				ereport(vacrel->verbose ? INFO : DEBUG2,
						(errmsg("\"%s\": stopping truncate due to conflicting lock request",
								vacrel->relname)));
				return;
			}

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL,
							 WAIT_EVENT_VACUUM_TRUNCATE);
			ResetLatch(MyLatch);
		}

		/*
		 * 既然已经持有排他锁，看看该关系在我们以非排他
		 * 锁进行清理期间是否增长了。若是，就放弃；
		 * 新增加的页大概含有不可删除的元组。
		 */
		new_rel_pages = RelationGetNumberOfBlocks(vacrel->rel);
		if (new_rel_pages != orig_rel_pages)
		{
			/*
			 * 注意：我们有意不在这里用新的关系大小更新 vacrel->rel_pages。
			 * 若那样做，就等于假设
			 * 新页是空的，这不太可能。保持数字
			 * 不动等于假设新页与现有页有相同的
			 * 元组密度，这不那么不可能。
			 */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * 从末尾反向扫描，以核实末尾的页实际上
		 * 不含元组。这是必要的，不是可选的，因为
		 * 其他后端可能在我们
		 * 清理期间向这些页添加了元组。
		 */
		new_rel_pages = count_nondeletable_pages(vacrel, &lock_waiter_detected);
		vacrel->blkno = new_rel_pages;

		if (new_rel_pages >= orig_rel_pages)
		{
			/* 毕竟什么也做不了 */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * 可以截断。
		 */
		RelationTruncate(vacrel->rel, new_rel_pages);

		/*
		 * 一旦截断完成，就可以释放排他锁。
		 * 其他后端在处理完 smgrtruncate 发出的 smgr 失效之前，
		 * 不能安全访问该关系 ... 但
		 * 那应当在它们取得该关系上的锁之后，
		 * 作为标准失效处理的一部分发生。
		 */
		UnlockRelation(vacrel->rel, AccessExclusiveLock);

		/*
		 * 更新统计信息。这里只调整 rel_pages
		 * 而不动 reltuples 是正确的，因为元组数
		 * 没有因截断而改变。
		 */
		vacrel->removed_pages += orig_rel_pages - new_rel_pages;
		vacrel->rel_pages = new_rel_pages;

		ereport(vacrel->verbose ? INFO : DEBUG2,
				(errmsg("table \"%s\": truncated %u to %u pages",
						vacrel->relname,
						orig_rel_pages, new_rel_pages)));
		orig_rel_pages = new_rel_pages;
	} while (new_rel_pages > vacrel->nonempty_pages && lock_waiter_detected);
}

/*
 * 重新扫描末尾的页，以核实它们（仍然）没有元组。
 *
 * 返回不可删除页的数量（最后一个非空页 + 1）。
 */
static BlockNumber
count_nondeletable_pages(LVRelState *vacrel, bool *lock_waiter_detected)
{
	BlockNumber blkno;
	BlockNumber prefetchedUntil;
	instr_time	starttime;

	/* 若我们检查冲突的锁请求，就初始化 starttime */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * 从我们认为的关系末尾开始检查块，并
	 * 反向移动。（循环控制的写法有点奇怪，是因为 blkno
	 * 是无符号的。）为了让扫描更快，我们一次向前
	 * 预取几个块，以便 OS 级预读能够生效。
	 */
	blkno = vacrel->rel_pages;
	StaticAssertStmt((PREFETCH_SIZE & (PREFETCH_SIZE - 1)) == 0,
					 "prefetch size must be power of 2");
	prefetchedUntil = InvalidBlockNumber;
	while (blkno > vacrel->nonempty_pages)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		hastup;

		/*
		 * 检查是否有另一个进程请求我们关系上的锁。我们在这里
		 * 持有 AccessExclusiveLock，所以它们会在等待。我们
		 * 每个 VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL 只做一次，并且
		 * 每 32 个块才检查一次该间隔是否已过，
		 * 以便把系统调用和实际的共享锁表
		 * 查找次数降到最低。
		 */
		if ((blkno % 32) == 0)
		{
			instr_time	currenttime;
			instr_time	elapsed;

			INSTR_TIME_SET_CURRENT(currenttime);
			elapsed = currenttime;
			INSTR_TIME_SUBTRACT(elapsed, starttime);
			if ((INSTR_TIME_GET_MICROSEC(elapsed) / 1000)
				>= VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL)
			{
				if (LockHasWaitersRelation(vacrel->rel, AccessExclusiveLock))
				{
					ereport(vacrel->verbose ? INFO : DEBUG2,
							(errmsg("table \"%s\": suspending truncate due to conflicting lock request",
									vacrel->relname)));

					*lock_waiter_detected = true;
					return blkno;
				}
				starttime = currenttime;
			}
		}

		/*
		 * 这里不插入清理延迟点，因为我们持有
		 * 表上的排他锁，希望持有的
		 * 时间尽可能短。不过仍然需要检查中断。
		 */
		CHECK_FOR_INTERRUPTS();

		blkno--;

		/* 若这一批还没预取，现在就预取。 */
		if (prefetchedUntil > blkno)
		{
			BlockNumber prefetchStart;
			BlockNumber pblkno;

			prefetchStart = blkno & ~(PREFETCH_SIZE - 1);
			for (pblkno = prefetchStart; pblkno <= blkno; pblkno++)
			{
				PrefetchBuffer(vacrel->rel, MAIN_FORKNUM, pblkno);
				CHECK_FOR_INTERRUPTS();
			}
			prefetchedUntil = prefetchStart;
		}

		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 vacrel->bstrategy);

		/* 本阶段我们只需要对缓冲区的共享访问 */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		hastup = false;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/*
			 * 注意：任何非未使用项都应作为保留
			 * 此页的理由。即使一个 LP_DEAD 项也会使截断不安全，因为
			 * 我们肯定还没有清掉它的索引项。
			 */
			if (ItemIdIsUsed(itemid))
			{
				hastup = true;
				break;			/* 可以停止扫描 */
			}
		}						/* 沿该页扫描 */

		UnlockReleaseBuffer(buf);

		/* 若在这里找到元组，就扫描完毕 */
		if (hastup)
			return blkno + 1;
	}

	/*
	 * 若我们跳出该循环，则所有先前认为是空的
	 * 页仍然是空的；不必再看最后一页已知非空的
	 * 页。
	 */
	return vacrel->nonempty_pages;
}

/*
 * 返回 VACUUM 应当分配空间来存放的死 TIDs 数量，
 * 给定大小为 vacrel->rel_pages 的堆关系，以及当前的
 * maintenance_work_mem 设置（或在适用时当前的 autovacuum_work_mem
 * 设置）。
 *
 * 理由见本文件开头的注释。
 */
static int
dead_items_max_items(LVRelState *vacrel)
{
	int64		max_items;
	int			vac_work_mem = IsAutoVacuumWorkerProcess() &&
		autovacuum_work_mem != -1 ?
		autovacuum_work_mem : maintenance_work_mem;

	if (vacrel->nindexes > 0)
	{
		BlockNumber rel_pages = vacrel->rel_pages;

		max_items = MAXDEADITEMS(vac_work_mem * 1024L);
		max_items = Min(max_items, INT_MAX);
		max_items = Min(max_items, MAXDEADITEMS(MaxAllocSize));

		/* 这里的写法有意确保乘法不会溢出 */
		if ((BlockNumber) (max_items / MaxHeapTuplesPerPage) > rel_pages)
			max_items = rel_pages * MaxHeapTuplesPerPage;

		/* 若 maintenance_work_mem 很小，保持合理 */
		max_items = Max(max_items, MaxHeapTuplesPerPage);
	}
	else
	{
		/* 单遍情形一次只存放单个堆页的 TIDs */
		max_items = MaxHeapTuplesPerPage;
	}

	return (int) max_items;
}

/*
 * 分配 dead_items（用 palloc，或放在动态共享内存中）。
 * 为调用方在 vacrel 中设置 dead_items。
 *
 * 需要时，也在 DSM 中分配 dead_items 的过程中
 * 处理并行初始化。
 */
static void
dead_items_alloc(LVRelState *vacrel, int nworkers)
{
	VacDeadItems *dead_items;
	int			max_items;

	max_items = dead_items_max_items(vacrel);
	Assert(max_items >= MaxHeapTuplesPerPage);

	/*
	 * 初始化并行清理的状态。到目前为止，一个索引只能使用
	 * 一个工作进程，所以只有当表上至少有
	 * 两个索引时我们才启用并行。
	 */
	if (nworkers >= 0 && vacrel->nindexes > 1 && vacrel->do_index_vacuuming)
	{
		/*
		 * 由于并行工作进程无法访问临时表中的数据，我们
		 * 无法对它们执行并行清理。
		 */
		if (RelationUsesLocalBuffers(vacrel->rel))
		{
			/*
			 * 仅当用户显式尝试对临时表
			 * 执行并行清理时才给出警告。
			 */
			if (nworkers > 0)
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
								vacrel->relname)));
		}
		else
			vacrel->pvs = parallel_vacuum_init(vacrel->rel, vacrel->indrels,
											   vacrel->nindexes, nworkers,
											   max_items,
											   vacrel->verbose ? INFO : DEBUG2,
											   vacrel->bstrategy);

		/* 若并行模式已启动，dead_items 空间在 DSM 中分配 */
		if (ParallelVacuumIsActive(vacrel))
		{
			vacrel->dead_items = parallel_vacuum_get_dead_items(vacrel->pvs);
			return;
		}
	}

	/* 串行 VACUUM 情形 */
	dead_items = (VacDeadItems *) palloc(vac_max_items_to_alloc_size(max_items));
	dead_items->max_items = max_items;
	dead_items->num_items = 0;

	vacrel->dead_items = dead_items;
}

/*
 * 对 dead_items_alloc 中分配的资源做清理
 */
static void
dead_items_cleanup(LVRelState *vacrel)
{
	if (!ParallelVacuumIsActive(vacrel))
	{
		/* 这里不必费心 pfree */
		return;
	}

	/* 结束并行模式 */
	parallel_vacuum_end(vacrel->pvs, vacrel->indstats);
	vacrel->pvs = NULL;
}

/*
 * 检查给定页中的每个元组是否对所有当前和未来的
 * 事务可见。同时返回 visibility_cutoff_xid，即可见元组中
 * 最高的 xmin。若此页上每个元组
 * 都已冻结，则把 *all_frozen 设为真。
 *
 * 这是 lazy_scan_prune() 的精简版。若你改动
 * 这里的任何东西，请确保一切保持同步。注意有一个
 * 断言会调用我们来核实大家仍然一致。务必避免
 * 在这里引入新的副作用。
 */
static bool
heap_page_is_all_visible(LVRelState *vacrel, Buffer buf,
						 TransactionId *visibility_cutoff_xid,
						 bool *all_frozen)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blockno = BufferGetBlockNumber(buf);
	OffsetNumber offnum,
				maxoff;
	bool		all_visible = true;

	*visibility_cutoff_xid = InvalidTransactionId;
	*all_frozen = true;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff && all_visible;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		HeapTupleData tuple;

		/*
		 * 设置偏移号，以便在处理此元组时若发生
		 * 任何错误，可以把它一起显示出来。
		 */
		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		/* 未使用或重定向的行指针没有意义 */
		if (!ItemIdIsUsed(itemid) || ItemIdIsRedirected(itemid))
			continue;

		ItemPointerSet(&(tuple.t_self), blockno, offnum);

		/*
		 * 死行指针可能有指向它们的索引指针。所以
		 * 不能把它们当作可见
		 */
		if (ItemIdIsDead(itemid))
		{
			all_visible = false;
			*all_frozen = false;
			break;
		}

		Assert(ItemIdIsNormal(itemid));

		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(vacrel->rel);

		switch (HeapTupleSatisfiesVacuum(&tuple, vacrel->cutoffs.OldestXmin,
										 buf))
		{
			case HEAPTUPLE_LIVE:
				{
					TransactionId xmin;

					/* 查看 lazy_scan_prune 中的注释。 */
					if (!HeapTupleHeaderXminCommitted(tuple.t_data))
					{
						all_visible = false;
						*all_frozen = false;
						break;
					}

					/*
					 * 插入者肯定已提交。但它是否足够旧，
					 * 以至于所有人都把它看作已提交？
					 */
					xmin = HeapTupleHeaderGetXmin(tuple.t_data);
					if (!TransactionIdPrecedes(xmin,
											   vacrel->cutoffs.OldestXmin))
					{
						all_visible = false;
						*all_frozen = false;
						break;
					}

					/* 跟踪该页上最新的 xmin。 */
					if (TransactionIdFollows(xmin, *visibility_cutoff_xid) &&
						TransactionIdIsNormal(xmin))
						*visibility_cutoff_xid = xmin;

					/* 检查此元组是否已经冻结 */
					if (all_visible && *all_frozen &&
						heap_tuple_needs_eventual_freeze(tuple.t_data))
						*all_frozen = false;
				}
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				{
					all_visible = false;
					*all_frozen = false;
					break;
				}
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}							/* 沿该页扫描 */

	/* 处理完给定页后，清除偏移信息。 */
	vacrel->offnum = InvalidOffsetNumber;

	return all_visible;
}

/*
 * 若统计信息准确，就更新 pg_class 中的索引统计信息。
 */
static void
update_relstats_all_indexes(LVRelState *vacrel)
{
	Relation   *indrels = vacrel->indrels;
	int			nindexes = vacrel->nindexes;
	IndexBulkDeleteResult **indstats = vacrel->indstats;

	Assert(vacrel->do_index_cleanup);

	for (int idx = 0; idx < nindexes; idx++)
	{
		Relation	indrel = indrels[idx];
		IndexBulkDeleteResult *istat = indstats[idx];

		if (istat == NULL || istat->estimated_count)
			continue;

		/* 更新索引统计信息 */
		vac_update_relstats(indrel,
							istat->num_pages,
							istat->num_index_tuples,
							0,
							false,
							InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL, false);
	}
}

/*
 * 清理期间发生错误时的错误上下文回调。索引阶段的错误
 * 上下文消息应当与并行
 * 清理中设置的消息一致。若你为那些阶段改动本函数，也要改
 * parallel_vacuum_error_callback()。
 */
static void
vacuum_error_callback(void *arg)
{
	LVRelState *errinfo = arg;

	switch (errinfo->phase)
	{
		case VACUUM_ERRCB_PHASE_SCAN_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while scanning block %u offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while scanning block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while scanning relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while vacuuming block %u offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while vacuuming block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while vacuuming relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_INDEX:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_INDEX_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_TRUNCATE:
			if (BlockNumberIsValid(errinfo->blkno))
				errcontext("while truncating relation \"%s.%s\" to %u blocks",
						   errinfo->relnamespace, errinfo->relname, errinfo->blkno);
			break;

		case VACUUM_ERRCB_PHASE_UNKNOWN:
		default:
			return;				/* 什么也不做；errinfo 可能尚未
								 * 初始化 */
	}
}

/*
 * 更新清理错误回调所需的信息。同时保存
 * 当前信息，以后可以通过 restore_vacuum_error_info 恢复。
 */
static void
update_vacuum_error_info(LVRelState *vacrel, LVSavedErrInfo *saved_vacrel,
						 int phase, BlockNumber blkno, OffsetNumber offnum)
{
	if (saved_vacrel)
	{
		saved_vacrel->offnum = vacrel->offnum;
		saved_vacrel->blkno = vacrel->blkno;
		saved_vacrel->phase = vacrel->phase;
	}

	vacrel->blkno = blkno;
	vacrel->offnum = offnum;
	vacrel->phase = phase;
}

/*
 * 恢复先前调用 update_vacuum_error_info 时保存的清理信息。
 */
static void
restore_vacuum_error_info(LVRelState *vacrel,
						  const LVSavedErrInfo *saved_vacrel)
{
	vacrel->blkno = saved_vacrel->blkno;
	vacrel->offnum = saved_vacrel->offnum;
	vacrel->phase = saved_vacrel->phase;
}
