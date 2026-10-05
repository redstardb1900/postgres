/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  堆访问方法代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam.c
 *
 *
 * 接口例程（INTERFACE ROUTINES）
 *		heap_beginscan	- 开始关系扫描
 *		heap_rescan		- 重新开始一次关系扫描
 *		heap_endscan	- 结束关系扫描
 *		heap_getnext	- 取出扫描中的下一个元组
 *		heap_fetch		- 按给定 tid 取出元组
 *		heap_insert		- 向关系中插入元组
 *		heap_multi_insert - 向关系中插入多个元组
 *		heap_delete		- 从关系中删除元组
 *		heap_update		- 用另一个元组替换关系中的元组
 *
 * 说明（NOTES）
 *	  本文件包含实现 POSTGRES 堆访问方法的
 *	  heap_ 例程，所有 POSTGRES
 *	  关系都使用该方法。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/heaptoast.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/valid.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_replication_origin.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


static HeapTuple heap_prepare_insert(Relation relation, HeapTuple tup,
									 TransactionId xid, CommandId cid, int options);
static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
								  Buffer newbuf, HeapTuple oldtup,
								  HeapTuple newtup, HeapTuple old_key_tuple,
								  bool all_visible_cleared, bool new_all_visible_cleared);
#ifdef USE_ASSERT_CHECKING
static void check_lock_if_inplace_updateable_rel(Relation relation,
												 ItemPointer otid,
												 HeapTuple newtup);
static void check_inplace_rel_lock(HeapTuple oldtup);
#endif
static Bitmapset *HeapDetermineColumnsInfo(Relation relation,
										   Bitmapset *interesting_cols,
										   Bitmapset *external_cols,
										   HeapTuple oldtup, HeapTuple newtup,
										   bool *has_external);
static bool heap_acquire_tuplock(Relation relation, ItemPointer tid,
								 LockTupleMode mode, LockWaitPolicy wait_policy,
								 bool *have_tuple_lock);
static void compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
									  uint16 old_infomask2, TransactionId add_to_xmax,
									  LockTupleMode mode, bool is_update,
									  TransactionId *result_xmax, uint16 *result_infomask,
									  uint16 *result_infomask2);
static TM_Result heap_lock_updated_tuple(Relation rel, HeapTuple tuple,
										 ItemPointer ctid, TransactionId xid,
										 LockTupleMode mode);
static int	heap_log_freeze_plan(HeapTupleFreeze *tuples, int ntuples,
								 xl_heap_freeze_plan *plans_out,
								 OffsetNumber *offsets_out);
static void GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
								   uint16 *new_infomask2);
static TransactionId MultiXactIdGetUpdateXid(TransactionId xmax,
											 uint16 t_infomask);
static bool DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
									LockTupleMode lockmode, bool *current_is_member);
static void MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
							Relation rel, ItemPointer ctid, XLTW_Oper oper,
							int *remaining);
static bool ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
									   uint16 infomask, Relation rel, int *remaining);
static void index_delete_sort(TM_IndexDeleteOp *delstate);
static int	bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate);
static XLogRecPtr log_heap_new_cid(Relation relation, HeapTuple tup);
static HeapTuple ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
										bool *copy);


/*
 * 每种元组锁模式都对应一种重量级锁，以及一个或两个
 * 对应的 MultiXactStatus（一个仅用于锁定元组，另一个用于
 * 更新它们）。本表（以及下面的宏）帮助我们确定
 * 任意特定元组锁强度所应使用的重量级锁模式
 * 与 MultiXactStatus 值。
 *
 * 它们与 InplaceUpdateTupleLock 交互，后者是 ExclusiveLock 的别名。
 *
 * 不要直接查看 lockstatus/updstatus！请改用
 * get_mxact_status_for_lock。
 */
static const struct
{
	LOCKMODE	hwlock;
	int			lockstatus;
	int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
	{							/* LockTupleKeyShare：键共享 */
		AccessShareLock,
		MultiXactStatusForKeyShare,
		-1						/* KeyShare 不允许更新元组 */
	},
	{							/* LockTupleShare：共享 */
		RowShareLock,
		MultiXactStatusForShare,
		-1						/* Share 不允许更新元组 */
	},
	{							/* LockTupleNoKeyExclusive：非键排他 */
		ExclusiveLock,
		MultiXactStatusForNoKeyUpdate,
		MultiXactStatusNoKeyUpdate
	},
	{							/* LockTupleExclusive：排他 */
		AccessExclusiveLock,
		MultiXactStatusForUpdate,
		MultiXactStatusUpdate
	}
};

/* 取得给定 MultiXactStatus 对应的 LOCKMODE */
#define LOCKMODE_from_mxstatus(status) \
			(tupleLockExtraInfo[TUPLOCK_from_mxstatus((status))].hwlock)

/*
 * 按 LockTupleMode 强度值获取元组上的重量级锁。
 * 这比让每个调用方自己把它翻译成 lock.h 中的
 * LOCKMODE 更易读。
 */
#define LockTupleTuplock(rel, tup, mode) \
	LockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define UnlockTupleTuplock(rel, tup, mode) \
	UnlockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define ConditionalLockTupleTuplock(rel, tup, mode) \
	ConditionalLockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)

#ifdef USE_PREFETCH
/*
 * heap_index_delete_tuples 与 index_delete_prefetch_buffer 用此
 * 结构来协调预取活动
 */
typedef struct
{
	BlockNumber cur_hblkno;
	int			next_item;
	int			ndeltids;
	TM_IndexDelete *deltids;
} IndexDeletePrefetchState;
#endif

/* heap_index_delete_tuples 自底向上索引删除的代价常量 */
#define BOTTOMUP_MAX_NBLOCKS			6
#define BOTTOMUP_TOLERANCE_NBLOCKS		3

/*
 * heap_index_delete_tuples 在确定必须访问哪些堆块时使用它，
 * 以便帮助其自底向上的索引删除调用方
 */
typedef struct IndexDeleteCounts
{
	int16		npromisingtids; /* 组内"有希望"的 TID 个数 */
	int16		ntids;			/* 组内 TID 个数 */
	int16		ifirsttid;		/* 组内第一个 deltid 的偏移 */
} IndexDeleteCounts;

/*
 * 本表为每一种 MultiXactStatus
 * 映射元组锁强度值。
 */
static const int MultiXactStatusLock[MaxMultiXactStatus + 1] =
{
	LockTupleKeyShare,			/* ForKeyShare：用于键共享 */
	LockTupleShare,				/* ForShare：用于共享 */
	LockTupleNoKeyExclusive,	/* ForNoKeyUpdate：用于非键更新 */
	LockTupleExclusive,			/* ForUpdate：用于更新 */
	LockTupleNoKeyExclusive,	/* NoKeyUpdate：非键更新 */
	LockTupleExclusive			/* Update：更新 */
};

/* 取得给定 MultiXactStatus 对应的 LockTupleMode */
#define TUPLOCK_from_mxstatus(status) \
			(MultiXactStatusLock[(status)])

/*
 * 若可能需要访问 TOAST，则检查我们持有有效快照。
 */
static inline void
AssertHasSnapshotForToast(Relation rel)
{
#ifdef USE_ASSERT_CHECKING

	/* 引导模式尤其会打破这条规则 */
	if (!IsNormalProcessingMode())
		return;

	/* 若该关系没有 TOAST 表，则没有问题 */
	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		return;

	/*
	 * 提交 16bf24e 通过去掉 pg_replication_origin 的 TOAST 表，修复了
	 * 在没有活动快照时访问它的问题。在更旧的分支上，
	 * 这些缺陷仍留在原地。它唯一的 varlena 列是 roname（复制源
	 * 名称），因此只有名称
	 * 需要行外存储时才会出问题，这看起来不太可能。无论如何，
	 * 在后向分支上为修复它增加额外代码改动似乎不值得。
	 */
	if (RelationGetRelid(rel) == ReplicationOriginRelationId)
		return;

	Assert(HaveRegisteredOrActiveSnapshot());

#endif							/* USE_ASSERT_CHECKING：断言检查 */
}

/* ----------------------------------------------------------------
 *						 堆支持例程
 * ----------------------------------------------------------------
 */

/* ----------------
 *		initscan - heap_beginscan 与 heap_rescan 共用的扫描代码
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key, bool keep_startblock)
{
	ParallelBlockTableScanDesc bpscan = NULL;
	bool		allow_strat;
	bool		allow_sync;

	/*
	 * 确定我们必须扫描的块数。
	 *
	 * 在扫描开始时做一次就够了，因为扫描进行期间新增的任何元组
	 * 对我的快照反正都不可见。
	 * （使用非 MVCC 快照时并非如此。不过，我们反正也无法
	 * 保证返回扫描开始之后新增的元组，因为它们
	 * 可能落在我们已经扫过的页上。要为非 MVCC 快照保证
	 * 结果一致，调用方必须持有某种更高层的
	 * 锁，以确保感兴趣的元组不会改变。）
	 */
	if (scan->rs_base.rs_parallel != NULL)
	{
		bpscan = (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
		scan->rs_nblocks = bpscan->phs_nblocks;
	}
	else
		scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

	/*
	 * 若表相对于 NBuffers 较大，则使用批量读访问
	 * 策略，并启用同步扫描（见 syncscan.c）。尽管
	 * 这些功能的阈值可以不同，我们仍把它们设成
	 * 相同，这样需要调节的行为只有两种而不是四种。
	 * （不过，有些调用方需要能够独立于表的大小，
	 * 单独禁用其中一项或两项；另外还有一个 GUC
	 * 变量可以禁用同步扫描。）
	 *
	 * 注意 table_block_parallelscan_initialize 里有非常相似的测试；
	 * 若改这里，也请考虑改那里。
	 */
	if (!RelationUsesLocalBuffers(scan->rs_base.rs_rd) &&
		scan->rs_nblocks > NBuffers / 4)
	{
		allow_strat = (scan->rs_base.rs_flags & SO_ALLOW_STRAT) != 0;
		allow_sync = (scan->rs_base.rs_flags & SO_ALLOW_SYNC) != 0;
	}
	else
		allow_strat = allow_sync = false;

	if (allow_strat)
	{
		/* 重新扫描期间，保留先前的策略对象。 */
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (scan->rs_base.rs_parallel != NULL)
	{
		/* 对并行扫描，采信 ParallelTableScanDesc 所说的一切。 */
		if (scan->rs_base.rs_parallel->phs_syncscan)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (keep_startblock)
	{
		/*
		 * 重新扫描时，我们希望保留先前的 startblock 设置，
		 * 这样回绕游标就不会产生令人意外的结果。
		 * 不过要重置当前生效的同步扫描设置。
		 */
		if (allow_sync && synchronize_seqscans)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
	}
	else if (allow_sync && synchronize_seqscans)
	{
		scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		scan->rs_startblock = ss_get_location(scan->rs_base.rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;
		scan->rs_startblock = 0;
	}

	scan->rs_numblocks = InvalidBlockNumber;
	scan->rs_inited = false;
	scan->rs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->rs_ctup.t_self);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;

	/* 尚未 rs_inited 时，一次一页的字段总是无效 */

	/*
	 * 在合适时复制扫描键
	 */
	if (key != NULL && scan->rs_base.rs_nkeys > 0)
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

	/*
	 * 目前我们只为顺序堆扫描设了统计计数器（不过
	 * 例如位图扫描中底层的位图索引扫描会被计数，
	 * 采样扫描则会更新元组抓取的统计）。
	 */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(scan->rs_base.rs_rd);
}

/*
 * heap_setscanlimits - 限制堆扫描的范围
 *
 * startBlk 是开始扫描的页
 * numBlks 是要扫描的页数（InvalidBlockNumber 表示"全部"）
 */
void
heap_setscanlimits(TableScanDesc sscan, BlockNumber startBlk, BlockNumber numBlks)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	Assert(!scan->rs_inited);	/* 否则再改就太晚了 */
	/* 否则 rs_startblock 是有意义的 */
	Assert(!(scan->rs_base.rs_flags & SO_ALLOW_SYNC));

	/* 检查 startBlk 有效（但允许块数为零的情形……） */
	Assert(startBlk == 0 || startBlk < scan->rs_nblocks);

	scan->rs_startblock = startBlk;
	scan->rs_numblocks = numBlks;
}

/*
 * heapgetpage - heapgettup() 的子例程
 *
 * 本例程读取并钉住关系中指定的页。
 * 在一次一页模式下它还要做额外工作，即判定
 * 该页上哪些元组可见。
 */
void
heapgetpage(TableScanDesc sscan, BlockNumber block)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	Buffer		buffer;
	Snapshot	snapshot;
	Page		page;
	int			lines;
	int			ntup;
	OffsetNumber lineoff;
	bool		all_visible;

	Assert(block < scan->rs_nblocks);

	/* 若有先前的扫描缓冲区，则释放它 */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
	 * 至少每页检查一次中断。更高层代码里的检查
	 * 无法停下一次遇到许多
	 * 页连续死元组的顺序扫描。
	 */
	CHECK_FOR_INTERRUPTS();

	/* 用选定的策略读取页面 */
	scan->rs_cbuf = ReadBufferExtended(scan->rs_base.rs_rd, MAIN_FORKNUM, block,
									   RBM_NORMAL, scan->rs_strategy);
	scan->rs_cblock = block;

	if (!(scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE))
		return;

	buffer = scan->rs_cbuf;
	snapshot = scan->rs_base.rs_snapshot;

	/*
	 * 若可能，对整页做剪枝并修复碎片。
	 */
	heap_page_prune_opt(scan->rs_base.rs_rd, buffer);

	/*
	 * 检查元组可见性时，必须持有缓冲区内容上的共享锁。
	 * 不过此后，只要我们仍钉住该缓冲区，已判定为
	 * 可见的元组就保证仍然有效。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);
	TestForOldSnapshot(snapshot, scan->rs_base.rs_rd, page);
	lines = PageGetMaxOffsetNumber(page);
	ntup = 0;

	/*
	 * 若全可见标志表明该页上所有元组
	 * 对所有人都可见，就可以跳过逐元组的可见性测试。
	 *
	 * 注意：在热备中，一个已经对所有
	 * 事务可见的元组，对只读
	 * 事务在备库中仍可能不可见。我们部分地处理这个问题：跟踪
	 * 可见元组的最小 xmin，作为在主库上把页面标为
	 * 全可见时的截止 XID，并随可见性映射 SET 操作一起写入 WAL。
	 * 在热备中，我们等待（或中止）
	 * 所有有可能看不到该页上一个或多个元组的
	 * 事务。仅索引扫描因此才能在热备中正常工作。仅索引
	 * 扫描与堆扫描的一个关键差别是：
	 * 仅索引扫描完全依赖可见性映射，而堆
	 * 扫描查看的是页面级 PD_ALL_VISIBLE 标志。我们不确定
	 * 页面级标志能否以同样方式被信任，因为它可能
	 * 未经显式 WAL 记录就以某种方式传播过来，例如经由
	 * 整页写入。在能够毫无疑问地证明这一点之前，
	 * 还是用较费事的办法逐个检查元组的可见性。
	 */
	all_visible = PageIsAllVisible(page) && !snapshot->takenDuringRecovery;

	for (lineoff = FirstOffsetNumber; lineoff <= lines; lineoff++)
	{
		ItemId		lpp = PageGetItemId(page, lineoff);
		HeapTupleData loctup;
		bool		valid;

		if (!ItemIdIsNormal(lpp))
			continue;

		loctup.t_tableOid = RelationGetRelid(scan->rs_base.rs_rd);
		loctup.t_data = (HeapTupleHeader) PageGetItem(page, lpp);
		loctup.t_len = ItemIdGetLength(lpp);
		ItemPointerSet(&(loctup.t_self), block, lineoff);

		if (all_visible)
			valid = true;
		else
			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);

		HeapCheckForSerializableConflictOut(valid, scan->rs_base.rs_rd,
											&loctup, buffer, snapshot);

		if (valid)
			scan->rs_vistuples[ntup++] = lineoff;
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	Assert(ntup <= MaxHeapTuplesPerPage);
	scan->rs_ntuples = ntup;
}

/*
 * heapgettup_initial_block - 返回要扫描的第一个 BlockNumber
 *
 * 没有块可扫描时返回 InvalidBlockNumber。空表会出现这种情况，
 * 并行扫描中并行工作进程在我们有机会取得第一页之前
 * 就把所有页都取走时也会出现。
 */
static BlockNumber
heapgettup_initial_block(HeapScanDesc scan, ScanDirection dir)
{
	Assert(!scan->rs_inited);

	/* 没有页可扫描时，返回 InvalidBlockNumber */
	if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
		return InvalidBlockNumber;

	if (ScanDirectionIsForward(dir))
	{
		/* 串行扫描 */
		if (scan->rs_base.rs_parallel == NULL)
			return scan->rs_startblock;
		else
		{
			/* 并行扫描 */
			table_block_parallelscan_startblock_init(scan->rs_base.rs_rd,
													 scan->rs_parallelworkerdata,
													 (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);

			/* 若没有更多块，可能返回 InvalidBlockNumber */
			return table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
													 scan->rs_parallelworkerdata,
													 (ParallelBlockTableScanDesc) scan->rs_base.rs_parallel);
		}
	}
	else
	{
		/* 不支持反向并行扫描 */
		Assert(scan->rs_base.rs_parallel == NULL);

		/*
		 * 反向扫描时不要向同步扫描逻辑报告位置；不太
		 * 可能同时还有别人在做同样的事，
		 * 更可能的是我们只会搅乱正向
		 * 扫描者。
		 */
		scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		/*
		 * 从扫描的最后一页开始。务必计入
		 * 已被 heap_setscanlimits() 调整过的 rs_numblocks。
		 */
		if (scan->rs_numblocks != InvalidBlockNumber)
			return (scan->rs_startblock + scan->rs_numblocks - 1) % scan->rs_nblocks;

		if (scan->rs_startblock > 0)
			return scan->rs_startblock - 1;

		return scan->rs_nblocks - 1;
	}
}


/*
 * heapgettup_start_page - heapgettup() 的辅助函数
 *
 * 根据 scan->rs_cbuf 返回要扫描的下一页，并把 *linesleft
 * 设为该页上的元组数。同时把 *lineoff 设为第一个要扫描的
 * 偏移：正向扫描取第一个偏移，反向扫描
 * 取该页上的最后一个偏移。
 */
static Page
heapgettup_start_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
					  OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* 若需要，由调用方负责确保缓冲区已上锁 */
	page = BufferGetPage(scan->rs_cbuf);

	TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, page);

	*linesleft = PageGetMaxOffsetNumber(page) - FirstOffsetNumber + 1;

	if (ScanDirectionIsForward(dir))
		*lineoff = FirstOffsetNumber;
	else
		*lineoff = (OffsetNumber) (*linesleft);

	/* lineoff 现在指向物理上的前一个或后一个 tid */
	return page;
}


/*
 * heapgettup_continue_page - heapgettup() 的辅助函数
 *
 * 根据 scan->rs_cbuf 返回要扫描的下一页，并把 *linesleft
 * 设为该页上剩余要扫描的元组数。同时把 *lineoff 设为
 * 按 'dir' 中的 ScanDirection 确定的下一个要扫描偏移。
 */
static inline Page
heapgettup_continue_page(HeapScanDesc scan, ScanDirection dir, int *linesleft,
						 OffsetNumber *lineoff)
{
	Page		page;

	Assert(scan->rs_inited);
	Assert(BufferIsValid(scan->rs_cbuf));

	/* 若需要，由调用方负责确保缓冲区已上锁 */
	page = BufferGetPage(scan->rs_cbuf);

	TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, page);

	if (ScanDirectionIsForward(dir))
	{
		*lineoff = OffsetNumberNext(scan->rs_coffset);
		*linesleft = PageGetMaxOffsetNumber(page) - (*lineoff) + 1;
	}
	else
	{
		/*
		 * 使用非 MVCC 快照时，上次返回的元组可能自
		 * 上次扫描以来已被清理，因此我们必须
		 * 重新确立 lineoff <= PageGetMaxOffsetNumber(page) 这一不变量
		 */
		*lineoff = Min(PageGetMaxOffsetNumber(page), OffsetNumberPrev(scan->rs_coffset));
		*linesleft = *lineoff;
	}

	/* lineoff 现在指向物理上的前一个或后一个 tid */
	return page;
}

/*
 * heapgettup_advance_block - heapgettup() 与 heapgettup_pagemode() 的辅助函数
 *
 * 根据当前块号、扫描方向以及扫描描述符中
 * 包含的各种信息，计算下一个要扫描的 BlockNumber
 * 并返回它。若没有更多块可扫描，则返回
 * InvalidBlockNumber，以此告知调用方。
 *
 * 不应当用它来确定初始块号——只用于
 * 后续的块。
 *
 * 当 heap_setscanlimits() 施加了限制时，它也会调整
 * rs_numblocks。
 */
static inline BlockNumber
heapgettup_advance_block(HeapScanDesc scan, BlockNumber block, ScanDirection dir)
{
	if (ScanDirectionIsForward(dir))
	{
		if (scan->rs_base.rs_parallel == NULL)
		{
			block++;

			/* 回绕到堆的起点 */
			if (block >= scan->rs_nblocks)
				block = 0;

			/*
			 * 为同步目的报告新的扫描位置。不过
			 * 向后移动时不这样做。那样只会
			 * 搅乱其他正向移动的扫描者。
			 *
			 * 注意：我们在检查扫描是否结束之前就这样做，以便
			 * 位置提示的最终状态回到关系的起点。
			 * 这并非严格必要，但否则当你多次运行
			 * 同一查询时，起始位置每次调用都会
			 * 略微向后移，令人困惑。
			 * 不过一般来说我们并不保证任何特定顺序。
			 */
			if (scan->rs_base.rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_base.rs_rd, block);

			/* 若回到起点，则扫描结束 */
			if (block == scan->rs_startblock)
				return InvalidBlockNumber;

			/* 检查是否已达到 heap_setscanlimits() 施加的限制 */
			if (scan->rs_numblocks != InvalidBlockNumber)
			{
				if (--scan->rs_numblocks == 0)
					return InvalidBlockNumber;
			}

			return block;
		}
		else
		{
			return table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
													 scan->rs_parallelworkerdata, (ParallelBlockTableScanDesc)
													 scan->rs_base.rs_parallel);
		}
	}
	else
	{
		/* 若最后一块就是起始位置，则扫描结束 */
		if (block == scan->rs_startblock)
			return InvalidBlockNumber;

		/* 检查是否已达到 heap_setscanlimits() 施加的限制 */
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			if (--scan->rs_numblocks == 0)
				return InvalidBlockNumber;
		}

		/* 上一页是第 0 页时，回绕到堆的末尾 */
		if (block == 0)
			block = scan->rs_nblocks;

		block--;

		return block;
	}
}

/* ----------------
 *		heapgettup - 取出下一个堆元组
 *
 *		若尚未完成则先初始化扫描；然后按 "dir" 前进到下一个
 *		元组；把下一个元组放在 scan->rs_ctup 中返回，
 *		若没有更多元组则把 scan->rs_ctup.t_data 设为 NULL。
 *
 * 注意：nkeys/key 之所以单独传入，尽管它们
 * 保存在扫描描述符里，是因为调用方可能不希望我们检查
 * 扫描键。
 *
 * 注意：无论哪个方向，一旦扫出扫描范围，我们就
 * 重置 rs_inited。这意味着同方向的下一次请求
 * 会重新开始扫描，这有点古怪，但
 * 反方向的请求会按正确方向开始一次新的扫描。
 * 后者是游标所需的行为，
 * 而前者在 Postgres 中一般是未定义行为，
 * 所以我们不太在意。
 * ----------------
 */
static void
heapgettup(HeapScanDesc scan,
		   ScanDirection dir,
		   int nkeys,
		   ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	BlockNumber block;
	Page		page;
	OffsetNumber lineoff;
	int			linesleft;

	if (unlikely(!scan->rs_inited))
	{
		block = heapgettup_initial_block(scan, dir);
		/* 得到 InvalidBlockNumber 时，确保 rs_cbuf 无效 */
		Assert(block != InvalidBlockNumber || !BufferIsValid(scan->rs_cbuf));
		scan->rs_inited = true;
	}
	else
	{
		/* 从先前返回的页或元组继续 */
		block = scan->rs_cblock;

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_continue_page(scan, dir, &linesleft, &lineoff);
		goto continue_page;
	}

	/*
	 * 推进扫描，直到找到符合条件的元组，或没有更多内容
	 * 可扫描
	 */
	while (block != InvalidBlockNumber)
	{
		heapgetpage((TableScanDesc) scan, block);
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		page = heapgettup_start_page(scan, dir, &linesleft, &lineoff);
continue_page:

		/*
		 * 只有在本页还有行时才继续扫描。
		 *
		 * 注意这能防止我们访问超出
		 * PageGetMaxOffsetNumber() 的行指针；无论是恢复
		 * 表扫描时的正向扫描，还是开始扫描新页时都是如此。
		 */
		for (; linesleft > 0; linesleft--, lineoff += dir)
		{
			bool		visible;
			ItemId		lpp = PageGetItemId(page, lineoff);

			if (!ItemIdIsNormal(lpp))
				continue;

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), block, lineoff);

			visible = HeapTupleSatisfiesVisibility(tuple,
												   scan->rs_base.rs_snapshot,
												   scan->rs_cbuf);

			HeapCheckForSerializableConflictOut(visible, scan->rs_base.rs_rd,
												tuple, scan->rs_cbuf,
												scan->rs_base.rs_snapshot);

			/* 跳过对本快照不可见的元组 */
			if (!visible)
				continue;

			/* 跳过任何不匹配扫描键的元组 */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			scan->rs_coffset = lineoff;
			return;
		}

		/*
		 * 若执行到这里，说明本页上的项已经用尽，
		 * 该移到下一页了。
		 */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

		/* 取得下一个要扫描的 BlockNumber */
		block = heapgettup_advance_block(scan, block, dir);
	}

	/* 扫描结束 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}

/* ----------------
 *		heapgettup_pagemode - 以一次一页模式取出下一个堆元组
 *
 *		API 与 heapgettup 相同，但用于一次一页模式
 *
 * 内部逻辑也与 heapgettup 大体相同，但有一些
 * 差别：我们不获取缓冲区内容锁（那只需要在
 * heapgetpage 内部发生），并且只遍历列出的元组
 * 即 rs_vistuples[] 中的那些，而不是该页上的全部元组。注意
 * lineindex 从 0 起计，而 heapgettup 中对应的循环变量 lineoff
 * 从 1 起计。
 * ----------------
 */
static void
heapgettup_pagemode(HeapScanDesc scan,
					ScanDirection dir,
					int nkeys,
					ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	BlockNumber block;
	Page		page;
	int			lineindex;
	int			linesleft;

	if (unlikely(!scan->rs_inited))
	{
		block = heapgettup_initial_block(scan, dir);
		/* 得到 InvalidBlockNumber 时，确保 rs_cbuf 无效 */
		Assert(block != InvalidBlockNumber || !BufferIsValid(scan->rs_cbuf));
		scan->rs_inited = true;
	}
	else
	{
		/* 从先前返回的页或元组继续 */
		block = scan->rs_cblock;	/* 当前页 */
		page = BufferGetPage(scan->rs_cbuf);
		TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, page);

		lineindex = scan->rs_cindex + dir;
		if (ScanDirectionIsForward(dir))
			linesleft = scan->rs_ntuples - lineindex;
		else
			linesleft = scan->rs_cindex;
		/* lineindex 现在指向下一个或上一个可见 tid */

		goto continue_page;
	}

	/*
	 * 推进扫描，直到找到符合条件的元组，或没有更多内容
	 * 可扫描
	 */
	while (block != InvalidBlockNumber)
	{
		heapgetpage((TableScanDesc) scan, block);
		page = BufferGetPage(scan->rs_cbuf);
		TestForOldSnapshot(scan->rs_base.rs_snapshot, scan->rs_base.rs_rd, page);
		linesleft = scan->rs_ntuples;
		lineindex = ScanDirectionIsForward(dir) ? 0 : linesleft - 1;

		/* lineindex 现在指向下一个或上一个可见 tid */
continue_page:

		for (; linesleft > 0; linesleft--, lineindex += dir)
		{
			ItemId		lpp;
			OffsetNumber lineoff;

			lineoff = scan->rs_vistuples[lineindex];
			lpp = PageGetItemId(page, lineoff);
			Assert(ItemIdIsNormal(lpp));

			tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), block, lineoff);

			/* 跳过任何不匹配扫描键的元组 */
			if (key != NULL &&
				!HeapKeyTest(tuple, RelationGetDescr(scan->rs_base.rs_rd),
							 nkeys, key))
				continue;

			scan->rs_cindex = lineindex;
			return;
		}

		/* 取得下一个要扫描的 BlockNumber */
		block = heapgettup_advance_block(scan, block, dir);
	}

	/* 扫描结束 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	tuple->t_data = NULL;
	scan->rs_inited = false;
}


/* ----------------------------------------------------------------
 *					 堆访问方法接口
 * ----------------------------------------------------------------
 */


TableScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key,
			   ParallelTableScanDesc parallel_scan,
			   uint32 flags)
{
	HeapScanDesc scan;

	/*
	 * 扫描关系期间增加关系引用计数
	 *
	 * 这只是为了切实保证：扫描持有指向它的指针时，
	 * relcache 项不会消失。调用方反正应当保持该关系打开，
	 * 所以在所有正常情形下这都是多余的……
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * 分配并初始化扫描描述符
	 */
	scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = parallel_scan;
	scan->rs_strategy = NULL;	/* 在 initscan 中设置 */

	/*
	 * 若快照不是 MVCC 安全的，则禁用一次一页模式。
	 */
	if (!(snapshot && IsMVCCSnapshot(snapshot)))
		scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;

	/*
	 * 对可串行化事务中的顺序扫描和采样扫描，获取整个关系上的
	 * 谓词锁。这不仅是为了
	 * 锁住所有匹配的元组，也是为了与新插入到
	 * 表中的行冲突。索引扫描时，我们对覆盖扫描条件所指定范围的索引页
	 * 取页锁，但堆扫描没有
	 * 更细粒度的东西可锁。位图扫描则是另一回事，
	 * 那时我们已经扫描了索引并锁住了覆盖该谓词的索引
	 * 页。但即便如此，我们仍必须锁住
	 * 任何匹配的堆元组。对采样扫描可以把锁定
	 * 优化到至少页级粒度，但那就需要增加逐元组
	 * 锁定。
	 */
	if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		/*
		 * 确保缺少快照时能可靠地被注意到，即便
		 * 隔离模式意味着不会执行谓词锁定（因此
		 * 这里也不会使用该快照）。
		 */
		Assert(snapshot);
		PredicateLockRelation(relation, snapshot);
	}

	/* 我们只需要设置一次 */
	scan->rs_ctup.t_tableOid = RelationGetRelid(relation);

	/*
	 * 分配内存，以便在做并行扫描时跟踪并行工作进程的
	 * 页面分配。
	 */
	if (parallel_scan != NULL)
		scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
	else
		scan->rs_parallelworkerdata = NULL;

	/*
	 * 放在这里而不是 initscan() 里，是因为 heap_rescan 也会调用
	 * initscan()，而我们不想再次分配内存
	 */
	if (nkeys > 0)
		scan->rs_base.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_base.rs_key = NULL;

	initscan(scan, key, false);

	return (TableScanDesc) scan;
}

void
heap_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
			bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	if (set_params)
	{
		if (allow_strat)
			scan->rs_base.rs_flags |= SO_ALLOW_STRAT;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_STRAT;

		if (allow_sync)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		if (allow_pagemode && scan->rs_base.rs_snapshot &&
			IsMVCCSnapshot(scan->rs_base.rs_snapshot))
			scan->rs_base.rs_flags |= SO_ALLOW_PAGEMODE;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;
	}

	/*
	 * 解除扫描缓冲区的钉住
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
	 * 重新初始化扫描描述符
	 */
	initscan(scan, key, true);
}

void
heap_endscan(TableScanDesc sscan)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* 注意：不需要做锁操作 */

	/*
	 * 解除扫描缓冲区的钉住
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
	 * 减少关系引用计数并释放扫描描述符的存储
	 */
	RelationDecrementReferenceCount(scan->rs_base.rs_rd);

	if (scan->rs_base.rs_key)
		pfree(scan->rs_base.rs_key);

	if (scan->rs_strategy != NULL)
		FreeAccessStrategy(scan->rs_strategy);

	if (scan->rs_parallelworkerdata != NULL)
		pfree(scan->rs_parallelworkerdata);

	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	pfree(scan);
}

HeapTuple
heap_getnext(TableScanDesc sscan, ScanDirection direction)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/*
	 * 它仍被广泛地直接使用，不经过表访问方法，因此
	 * 加上安全检查。以后也许应当
	 * 把它降级为断言。检查访问方法例程
	 * 而不是访问方法 oid 的原因是，这样可以编写回归测试，
	 * 创建另一个复用堆处理器的访问方法。
	 */
	if (unlikely(sscan->rs_rd->rd_tableam != GetHeapamTableAmRoutine()))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("only heap AM is supported")));

	/*
	 * 我们不预期对系统目录表或普通表直接调用 heap_getnext
	 * 时带着有效的 CheckXidAlive。详见 xact.c 中
	 * 这些变量的声明处注释。正常情况下我们在 tableam
	 * 层 API 已有此类检查，但这里被许多地方调用，所以需要在
	 * 此处保证这一点。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected heap_getnext call during logical decoding");

	/* 注意：不需要做锁操作 */

	if (scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction,
							scan->rs_base.rs_nkeys, scan->rs_base.rs_key);
	else
		heapgettup(scan, direction,
				   scan->rs_base.rs_nkeys, scan->rs_base.rs_key);

	if (scan->rs_ctup.t_data == NULL)
		return NULL;

	/*
	 * 若执行到这里，说明有了新的当前扫描元组，于是指向
	 * 合适的返回缓冲区并返回该元组。
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	return &scan->rs_ctup;
}

bool
heap_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;

	/* 注意：不需要做锁操作 */

	if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
		heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
	else
		heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

	if (scan->rs_ctup.t_data == NULL)
	{
		ExecClearTuple(slot);
		return false;
	}

	/*
	 * 若执行到这里，说明有了新的当前扫描元组，于是指向
	 * 合适的返回缓冲区并返回该元组。
	 */

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot,
							 scan->rs_cbuf);
	return true;
}

void
heap_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
				  ItemPointer maxtid)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	BlockNumber startBlk;
	BlockNumber numBlks;
	ItemPointerData highestItem;
	ItemPointerData lowestItem;

	/*
	 * 对没有任何页的关系，可以直接让 TID 范围
	 * 保持未设置。不会有元组可扫描，因此也不会有元组落在
	 * 给定 TID 范围之外。
	 */
	if (scan->rs_nblocks == 0)
		return;

	/*
	 * 设置一些 ItemPointer，指向堆中第一个和最后一个可能的
	 * 元组。
	 */
	ItemPointerSet(&highestItem, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowestItem, 0, FirstOffsetNumber);

	/*
	 * 若给定的最大 TID 低于关系中可能的最高 TID，
	 * 就把范围限制到该值，否则一直扫描到关系的
	 * 末尾。
	 */
	if (ItemPointerCompare(maxtid, &highestItem) < 0)
		ItemPointerCopy(maxtid, &highestItem);

	/*
	 * 若给定的最小 TID 高于关系中可能的最低 TID，
	 * 就把范围限制为只扫描高于该值的 TID。
	 */
	if (ItemPointerCompare(mintid, &lowestItem) > 0)
		ItemPointerCopy(mintid, &lowestItem);

	/*
	 * 检查空范围，并防止下面的 numBlks 计算
	 * 出现本会为负的结果。
	 */
	if (ItemPointerCompare(&highestItem, &lowestItem) < 0)
	{
		/* 设置一个空的待扫描块范围 */
		heap_setscanlimits(sscan, 0, 0);
		return;
	}

	/*
	 * 计算第一个块以及我们必须扫描的块数。这里
	 * 可以更激进一些，做更多校验，试图
	 * 进一步收窄要扫描的块范围：检查
	 * lowestItem 的偏移是否高于 MaxOffsetNumber。若是，可以把
	 * startBlk 加一。同样，若 highestItem 的偏移为 0，
	 * 可以少扫描一块。不过这种优化目前
	 * 似乎不值得费心。
	 */
	startBlk = ItemPointerGetBlockNumberNoCheck(&lowestItem);

	numBlks = ItemPointerGetBlockNumberNoCheck(&highestItem) -
		ItemPointerGetBlockNumberNoCheck(&lowestItem) + 1;

	/* 设置起始块和要扫描的块数 */
	heap_setscanlimits(sscan, startBlk, numBlks);

	/* 最后，在 sscan 中设置 TID 范围 */
	ItemPointerCopy(&lowestItem, &sscan->rs_mintid);
	ItemPointerCopy(&highestItem, &sscan->rs_maxtid);
}

bool
heap_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	ItemPointer mintid = &sscan->rs_mintid;
	ItemPointer maxtid = &sscan->rs_maxtid;

	/* 注意：不需要做锁操作 */
	for (;;)
	{
		if (sscan->rs_flags & SO_ALLOW_PAGEMODE)
			heapgettup_pagemode(scan, direction, sscan->rs_nkeys, sscan->rs_key);
		else
			heapgettup(scan, direction, sscan->rs_nkeys, sscan->rs_key);

		if (scan->rs_ctup.t_data == NULL)
		{
			ExecClearTuple(slot);
			return false;
		}

		/*
		 * heap_set_tidrange 会用 heap_setscanlimits 把我们扫描的
		 * 页范围限制为只包含可能含有目标 TID 范围的那些页。
		 * 这里必须滤掉这些
		 * 页上落在该范围之外的任何元组。
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, mintid) < 0)
		{
			ExecClearTuple(slot);

			/*
			 * 反向扫描时，TID 按降序排列。
			 * 这个方向上后续的元组还会更小，因此可以
			 * 直接返回 false，表示不会再有元组。
			 */
			if (ScanDirectionIsBackward(direction))
				return false;

			continue;
		}

		/*
		 * 对最后一页同样如此，必须滤掉大于
		 * maxtid 的 TID。
		 */
		if (ItemPointerCompare(&scan->rs_ctup.t_self, maxtid) > 0)
		{
			ExecClearTuple(slot);

			/*
			 * 正向扫描时，TID 按升序排列。
			 * 这个方向上后续的元组还会更大，因此可以
			 * 直接返回 false，表示不会再有元组。
			 */
			if (ScanDirectionIsForward(direction))
				return false;
			continue;
		}

		break;
	}

	/*
	 * 若执行到这里，说明有了新的当前扫描元组，于是指向
	 * 合适的返回缓冲区并返回该元组。
	 */
	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	ExecStoreBufferHeapTuple(&scan->rs_ctup, slot, scan->rs_cbuf);
	return true;
}

/*
 *	heap_fetch		- 按给定 tid 取出元组
 *
 * 进入时，tuple->t_self 是要取出的 TID。我们钉住持有
 * 该元组的缓冲区，填好 *tuple 的其余字段，并对照
 * 指定的快照检查该元组。
 *
 * 若成功（找到元组且通过快照时间限定），则把 *userbuf
 * 设为持有该元组的缓冲区并返回 true。调用方
 * 用完该元组后必须解除缓冲区的钉住。
 *
 * 若未找到元组（即项号引用了已删除的槽），
 * 则把 tuple->t_data 设为 NULL，把 *userbuf 设为 InvalidBuffer，
 * 并返回 false。
 *
 * 若找到元组但未通过时间限定检查，则行为
 * 取决于 keep_buf 参数。若 keep_buf 为 false，结果
 * 与未找到元组的情形相同。若 keep_buf 为 true，
 * 则像成功情形那样返回 tuple->t_data 和 *userbuf，
 * 调用方同样必须解除缓冲区的钉住；但返回 false。
 *
 * heap_fetch 不跟随 HOT 链：只会取出
 * 所请求的确切 TID。
 *
 * 对无效块号我们 ereport()，对无效项号却
 * 返回 false，这有些不一致。不过有两个原因。
 * 其一是调用方可以比较容易地检查块号是否
 * 有效，但如果不自己读页就无法检查项号。
 * 其二是跟随 t_ctid 链接时，我们可以
 * 相当有把握地认为页号有效（因为 VACUUM 不应当
 * 在没有先杀掉引用元组的情况下截掉目标页），
 * 但项号很可能并不正确。
 */
bool
heap_fetch(Relation relation,
		   Snapshot snapshot,
		   HeapTuple tuple,
		   Buffer *userbuf,
		   bool keep_buf)
{
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	bool		valid;

	/*
	 * 取出并钉住关系中相应的页。
	 */
	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	/*
	 * 需要缓冲区上的共享锁，以便检查元组的提交状态。
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	TestForOldSnapshot(snapshot, relation, page);

	/*
	 * 最好检查 offnum 是否越界，以防自取得该
	 * TID 以来发生过 VACUUM。
	 */
	offnum = ItemPointerGetOffsetNumber(tid);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * 取得与所请求 tid 对应的项行指针
	 */
	lp = PageGetItemId(page, offnum);

	/*
	 * 必须检查元组是否已被删除。
	 */
	if (!ItemIdIsNormal(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * 填好 *tuple 的字段
	 */
	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * 检查元组可见性，然后释放锁
	 */
	valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

	if (valid)
		PredicateLockTID(relation, &(tuple->t_self), snapshot,
						 HeapTupleHeaderGetXmin(tuple->t_data));

	HeapCheckForSerializableConflictOut(valid, relation, tuple, buffer, snapshot);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * 全部检查都通过了，因此把元组作为有效元组返回。现在由调用方
		 * 负责释放缓冲区。
		 */
		*userbuf = buffer;

		return true;
	}

	/* 元组未通过时间限定，但调用方也许仍想看到它。 */
	if (keep_buf)
		*userbuf = buffer;
	else
	{
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
		tuple->t_data = NULL;
	}

	return false;
}

/*
 *	heap_hot_search_buffer	- 在 HOT 链中搜索满足快照的元组
 *
 * 进入时，*tid 是某个元组的 TID（要么是普通元组，要么是
 * HOT 链的根），buffer 是持有该元组的缓冲区。我们搜索
 * 满足给定快照的第一个链成员。若找到一个，
 * 就把 *tid 更新为该元组的偏移号，并
 * 返回 true。若没有匹配，则返回 false 且不修改 *tid。
 *
 * heapTuple 是调用方提供的缓冲区。找到匹配时，除了更新 *tid，
 * 我们也把元组放在这里返回。若没有找到匹配，返回时
 * 该缓冲区的内容未定义。
 *
 * 若 all_dead 不是 NULL，我们检查不可见元组是否
 * 对全局都已死亡；若 HOT 链的全部成员
 * 都可清理，则把 *all_dead 设为 true，否则设为 false。
 *
 * 与 heap_fetch 不同，调用方必须已经钉住缓冲区并至少持有共享
 * 锁；退出时它仍然钉住并上锁。
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	GlobalVisState *vistest = NULL;

	/* 若这不是第一次调用，上次调用返回了一个（活的！）元组 */
	if (all_dead)
		*all_dead = first_call;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX：应当断言已压入或注册了一个快照 */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/* 扫描 HOT 链上可能的多个成员 */
	for (;;)
	{
		ItemId		lp;

		/* 检查虚假的 TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;

		lp = PageGetItemId(page, offnum);

		/* 检查未使用、已死或已重定向的项 */
		if (!ItemIdIsNormal(lp))
		{
			/* 只应在链的起点看到重定向 */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* 跟随重定向 */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* 否则必定是链的末端 */
			break;
		}

		/*
		 * 更新 heapTuple，使它指向我们正在
		 * 查看的 HOT 链元素。正确设置 t_self 很重要，
		 * 因为 SSI 检查以及历史
		 * MVCC 快照的 *Satisfies 例程需要正确的 tid 来判定可见性。
		 */
		heapTuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSet(&heapTuple->t_self, blkno, offnum);

		/*
		 * 不应当在链的起点看到 HEAP_ONLY 元组。
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * xmin 应当与前一个 xmax 值匹配，否则链已
		 * 断裂。
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * 当 first_call 为 true（因此 skip 最初为 false）时，我们会
		 * 返回找到的第一个元组。但在后续轮次，heapTuple
		 * 起初会指向上次返回的元组。
		 * 再次返回它是不正确的（而且会无限循环），所以
		 * 跳过它，返回我们找到的下一个匹配。
		 */
		if (!skip)
		{
			/* 若按该快照它可见，就必须返回它 */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			HeapCheckForSerializableConflictOut(valid, relation, heapTuple,
												buffer, snapshot);

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTID(relation, &heapTuple->t_self, snapshot,
								 HeapTupleHeaderGetXmin(heapTuple->t_data));
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * 若我们看不到它，也许别人也看不到。按调用方
		 * 的请求，检查链上所有成员是否对所有
		 * 事务都已死亡。
		 *
		 * 注意：若改变这里对"dead"的判定标准，请一并修改
		 * 规划器的 get_actual_variable_range() 函数以保持一致。
		 */
		if (all_dead && *all_dead)
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
				*all_dead = false;
		}

		/*
		 * 检查 HOT 链是否在此元组之后继续；若是，则取出
		 * 下一个 offnum 并回到循环。
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   blkno);
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
		}
		else
			break;				/* 链的末端 */
	}

	return false;
}

/*
 *	heap_get_latest_tid - 取得指定元组的最新 tid
 *
 * 实际上，它取得按扫描快照可见的最新版本。
 * 用 SnapshotDirty 创建扫描，才能取得真正最新的、
 * 可能尚未提交的版本。
 *
 * *tid 既是输入也是输出参数：它会被更新为
 * 该行的最新版本。注意若该行没有任何版本通过快照测试，
 * 它就不会被改变。
 */
void
heap_get_latest_tid(TableScanDesc sscan,
					ItemPointer tid)
{
	Relation	relation = sscan->rs_rd;
	Snapshot	snapshot = sscan->rs_snapshot;
	ItemPointerData ctid;
	TransactionId priorXmax;

	/*
	 * table_tuple_get_latest_tid() 已验证传入的 tid 有效。
	 * 不过假定 t_ctid 链接是有效的——表中不应当有无效
	 * 的链接。
	 */
	Assert(ItemPointerIsValid(tid));

	/*
	 * 循环追逐 t_ctid 链接。循环顶部，ctid 是我们
	 * 需要检查的元组，若 ctid 结果是虚假的，*tid 就是我们将返回的
	 * TID。
	 *
	 * 注意我们会一直循环到 t_ctid 链的末端。
	 * 取决于传入的快照，该行最多可能有一个可见
	 * 版本，但我们不为此做专门优化。
	 */
	ctid = *tid;
	priorXmax = InvalidTransactionId;	/* 无法检查第一个 XMIN */
	for (;;)
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber offnum;
		ItemId		lp;
		HeapTupleData tp;
		bool		valid;

		/*
		 * 读取、钉住并锁定该页。
		 */
		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		TestForOldSnapshot(snapshot, relation, page);

		/*
		 * 检查虚假的项号。这不当作错误
		 * 条件，因为它可能在跟随 t_ctid 链接时发生。我们
		 * 只假定先前的 tid 没问题，并原样返回它。
		 */
		offnum = ItemPointerGetOffsetNumber(&ctid);
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* 可以访问该元组 */
		tp.t_self = ctid;
		tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tp.t_len = ItemIdGetLength(lp);
		tp.t_tableOid = RelationGetRelid(relation);

		/*
		 * 跟随 t_ctid 链接之后，我们可能到达一个无关的
		 * 元组。检查 XMIN 是否匹配。
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(tp.t_data)))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * 检查元组可见性；若可见，把它设为新的结果
		 * 候选。
		 */
		valid = HeapTupleSatisfiesVisibility(&tp, snapshot, buffer);
		HeapCheckForSerializableConflictOut(valid, relation, &tp, buffer, snapshot);
		if (valid)
			*tid = ctid;

		/*
		 * 若有有效的 t_ctid 链接就跟随它，否则结束。
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data) ||
			HeapTupleHeaderIndicatesMovedPartitions(tp.t_data) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		UnlockReleaseBuffer(buffer);
	}							/* 循环结束 */
}


/*
 * UpdateXmaxHintBits - xmax 事务结束后更新元组提示位
 *
 * 在我们等待 XMAX 事务终止之后调用。
 * 若事务已中止，我们保证 XMAX_INVALID 提示位会在
 * 退出时设置。若事务已提交，则设置 XMAX_COMMITTED
 * 提示位（若可能）——但要注意此时也许还做不到，
 * 如果事务是异步提交的。
 *
 * 注意若该事务只是加锁者，我们设置 HEAP_XMAX_INVALID，
 * 即使它提交也是如此。
 *
 * 因此调用方应当只查看 XMAX_INVALID。
 *
 * 注意：xmax 为 multixact 的元组不允许这样做。
 */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
	Assert(TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple), xid));
	Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

	if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) &&
			TransactionIdDidCommit(xid))
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
								 xid);
		else
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
								 InvalidTransactionId);
	}
}


/*
 * GetBulkInsertState - 为批量插入准备状态对象
 */
BulkInsertState
GetBulkInsertState(void)
{
	BulkInsertState bistate;

	bistate = (BulkInsertState) palloc(sizeof(BulkInsertStateData));
	bistate->strategy = GetAccessStrategy(BAS_BULKWRITE);
	bistate->current_buf = InvalidBuffer;
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
	bistate->already_extended_by = 0;
	return bistate;
}

/*
 * FreeBulkInsertState - 批量插入结束后清理
 */
void
FreeBulkInsertState(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	FreeAccessStrategy(bistate->strategy);
	pfree(bistate);
}

/*
 * ReleaseBulkInsertStatePin - 释放 bistate 当前持有的缓冲区
 */
void
ReleaseBulkInsertStatePin(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	bistate->current_buf = InvalidBuffer;

	/*
	 * 尽管名字不这么说，我们也重置批量关系扩展状态。
	 * 否则可能因为在某个分区的
	 * ->next_free 里找空闲空间而报错，即使 ->next_free 是在
	 * 扩展另一个分区时设置的。即便不报错，去看另一个
	 * 分区偏移处的已有块，对
	 * 效率显然也不好。
	 */
	bistate->next_free = InvalidBlockNumber;
	bistate->last_free = InvalidBlockNumber;
}


/*
 *	heap_insert		- 向堆中插入元组
 *
 * 新元组盖上当前事务 ID 和指定的
 * 命令 ID。
 *
 * 关于大多数输入标志的说明见 table_tuple_insert，只是
 * 本例程直接接受元组而不是槽。
 *
 * 所有 TABLE_INSERT_ 选项都有对应的 HEAP_INSERT_
 * 选项，另外还有 HEAP_INSERT_SPECULATIVE，用来
 * 实现 table_tuple_insert_speculative()。
 *
 * 返回时 *tup 的头部字段会更新为与已存储元组一致；
 * 特别是 tup->t_self 收到元组实际存储位置的 TID。
 * 但注意元组数据内部字段的任何 TOAST 处理都不会
 * 反映到 *tup 中。
 */
void
heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple	heaptup;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	bool		all_visible_cleared = false;

	/* 粗略、简单的检查：元组是否匹配关系的行类型。 */
	Assert(HeapTupleHeaderGetNatts(tup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	AssertHasSnapshotForToast(relation);

	/*
	 * 填好元组头部字段，必要时对元组做 TOAST。
	 *
	 * 注意：从此往下，heaptup 是我们实际打算存入
	 * 关系的数据；tup 是调用方原始的、未做 TOAST 的数据。
	 */
	heaptup = heap_prepare_insert(relation, tup, xid, cid, options);

	/*
	 * 寻找用来插入该元组的缓冲区。若该页全可见，
	 * 这里也会钉住所需的可见性映射页。
	 */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, options, bistate,
									   &vmbuffer, NULL,
									   0);

	/*
	 * 即将做实际插入——但先检查冲突，以免
	 * 可能不得不回滚刚刚做完的工作。
	 *
	 * 只要不可能有另一个进程在本次检查与插入
	 * 对扫描可见之间扫描该页，不做复查也是安全的（也就是
	 * 从此时起一直持有排他的缓冲区内容锁，
	 * 直到元组插入变为可见）。
	 *
	 * 对堆插入，我们只需要检查表级 SSI 锁。我们的
	 * 新元组不可能与已有元组锁冲突，而堆
	 * 页锁只是元组锁的汇总形式；它们不像索引页锁那样
	 * 锁定"间隙"。因此调用时不必指定
	 * 缓冲区，检查也就更快。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup,
						 (options & HEAP_INSERT_SPECULATIVE) != 0);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation,
							ItemPointerGetBlockNumber(&(heaptup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/*
	 * XXX 是否应当在此页上设置 PageSetPrunable？
	 *
	 * 插入事务最终可能中止，从而使该元组
	 * 变为 DEAD，因而可以剪枝。虽然我们不想为中止做优化，
	 * 但若本页没有其他元组被更新或删除，这个
	 * 已中止的元组要等到下一次触发清理才会被剪枝。
	 *
	 * 若确实在这里加上 PageSetPrunable，也请加到 heap_xlog_insert 中。
	 */

	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;
		int			bufflags = 0;

		/*
		 * 若这是系统目录，需要传送组合 CID 才能正确
		 * 解码，因此也把它记入日志。
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, heaptup);

		/*
		 * 若这是该页上唯一且第一个元组，可以重新初始化该
		 * 页，而不必恢复整页。设置标志，并向 XLogInsert
		 * 隐藏缓冲区引用。
		 */
		if (ItemPointerGetOffsetNumber(&(heaptup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}

		xlrec.offnum = ItemPointerGetOffsetNumber(&heaptup->t_self);
		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_INSERT_ALL_VISIBLE_CLEARED;
		if (options & HEAP_INSERT_SPECULATIVE)
			xlrec.flags |= XLH_INSERT_IS_SPECULATIVE;
		Assert(ItemPointerGetBlockNumber(&heaptup->t_self) == BufferGetBlockNumber(buffer));

		/*
		 * 为了逻辑解码，即使正在做整页
		 * 写入，我们也需要该元组，因此即使拍了整页
		 * 映像也要把它包含进去。（XXX 也可以改成在 FPW 里存一个指针）。
		 */
		if (RelationIsLogicallyLogged(relation) &&
			!(options & HEAP_INSERT_NO_LOGICAL))
		{
			xlrec.flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;
			bufflags |= REGBUF_KEEP_DATA;

			if (IsToastRelation(relation))
				xlrec.flags |= XLH_INSERT_ON_TOAST_RELATION;
		}

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapInsert);

		xlhdr.t_infomask2 = heaptup->t_data->t_infomask2;
		xlhdr.t_infomask = heaptup->t_data->t_infomask;
		xlhdr.t_hoff = heaptup->t_data->t_hoff;

		/*
		 * 注意我们把 xlhdr 标成属于该缓冲区；若 XLogInsert 决定把
		 * 整页写入 xlog，就不需要在 xlog 里存储
		 * xl_heap_header。
		 */
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(0, (char *) &xlhdr, SizeOfHeapHeader);
		/* PG73FORMAT：写入位图 [+ 填充] [+ oid] + 数据 */
		XLogRegisterBufData(0,
							(char *) heaptup->t_data + SizeofHeapTupleHeader,
							heaptup->t_len - SizeofHeapTupleHeader);

		/* 按行过滤复制源要高效得多 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, info);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 若元组可缓存，则标记它，以便在我们中止时从缓存中失效。
	 * 注意在释放缓冲区之后做这件事是可以的，因为
	 * heaptup 数据结构全在本地内存里，不在共享
	 * 缓冲区中。
	 */
	CacheInvalidateHeapTuple(relation, heaptup, NULL);

	/* 注意：推测性插入也会被计数，即使后来中止 */
	pgstat_count_heap_insert(relation, 1);

	/*
	 * 若 heaptup 是私有副本，则释放它。别忘了把 t_self
	 * 也复制回调用方的映像。
	 */
	if (heaptup != tup)
	{
		tup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}
}

/*
 * heap_insert() 的子例程。为插入准备元组。它设置
 * 元组头部字段，并在必要时做 TOAST。若做过 TOAST，返回该元组的 TOAST
 * 版本，否则返回原元组。注意
 * 无论哪种情况，原元组的头部字段也会被设置。
 */
static HeapTuple
heap_prepare_insert(Relation relation, HeapTuple tup, TransactionId xid,
					CommandId cid, int options)
{
	/*
	 * 为允许并行插入，需要确保它们在工作进程中执行是安全的。
	 * 除了插入会生成新 CommandId 的情形
	 * （例如向带外键列的表插入），我们一般已有允许并行
	 * 插入的基础设施。
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples in a parallel worker")));

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	if (options & HEAP_INSERT_FROZEN)
		HeapTupleHeaderSetXminFrozen(tup->t_data);

	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0); /* 为了整洁 */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * 若新元组太大而无法存储，或含有来自其他关系的、已经做过 TOAST 的
	 * 行外属性，则调用 toaster。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* TOAST 表项绝不应当被递归做 TOAST */
		Assert(!HeapTupleHasExternal(tup));
		return tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		return heap_toast_insert_or_update(relation, tup, NULL, options);
	else
		return tup;
}

/*
 * heap_multi_insert() 的辅助函数，计算插入剩余 heaptuple 需要的
 * 整页数量。用来确定
 * 关系需要扩展多少。
 */
static int
heap_multi_insert_pages(HeapTuple *heaptuples, int done, int ntuples, Size saveFreeSpace)
{
	size_t		page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
	int			npages = 1;

	for (int i = done; i < ntuples; i++)
	{
		size_t		tup_sz = sizeof(ItemIdData) + MAXALIGN(heaptuples[i]->t_len);

		if (page_avail < tup_sz)
		{
			npages++;
			page_avail = BLCKSZ - SizeOfPageHeaderData - saveFreeSpace;
		}
		page_avail -= tup_sz;
	}

	return npages;
}

/*
 *	heap_multi_insert	- 向堆中插入多个元组
 *
 * 与 heap_insert() 类似，但一次操作插入多个元组。
 * 这比循环调用 heap_insert() 更快，因为多个
 * 元组可以插在同一页上时，我们可以只写一条 WAL
 * 记录覆盖全部元组，并且该页只需要加锁/解锁一次。
 *
 * 注意：这会把内存泄漏到当前内存上下文。若这是问题，
 * 可以在调用前创建一个临时上下文。
 */
void
heap_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple  *heaptuples;
	int			i;
	int			ndone;
	PGAlignedBlock scratch;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	bool		needwal;
	Size		saveFreeSpace;
	bool		need_tuple_data = RelationIsLogicallyLogged(relation);
	bool		need_cids = RelationIsAccessibleInLogicalDecoding(relation);
	bool		starting_with_empty_page = false;
	int			npages = 0;
	int			npages_used = 0;

	/* 目前 heap_multi_insert() 不需要（因此不支持） */
	Assert(!(options & HEAP_INSERT_NO_LOGICAL));

	AssertHasSnapshotForToast(relation);

	needwal = RelationNeedsWAL(relation);
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/* 对所有槽做 TOAST 并设置头部数据 */
	heaptuples = palloc(ntuples * sizeof(HeapTuple));
	for (i = 0; i < ntuples; i++)
	{
		HeapTuple	tuple;

		tuple = ExecFetchSlotHeapTuple(slots[i], true, NULL);
		slots[i]->tts_tableOid = RelationGetRelid(relation);
		tuple->t_tableOid = slots[i]->tts_tableOid;
		heaptuples[i] = heap_prepare_insert(relation, tuple, xid, cid,
											options);
	}

	/*
	 * 即将做实际插入——但先检查冲突，
	 * 以尽量降低不得不回滚刚刚做完的工作的
	 * 可能。
	 *
	 * 这里的检查并不能确定性地防止序列化异常；
	 * 该检查必须至少在取得将受影响的每个缓冲区上的
	 * 排他缓冲区内容锁之后进行，
	 * 也可以在所有插入都反映到缓冲区并且
	 * 那些锁已释放之后进行；否则存在竞态条件。由于
	 * 下面的循环里多个缓冲区会被加锁又解锁，而且
	 * 在循环之前找出并锁住所有这些缓冲区并不可行，
	 * 我们必须在结束时做一次最终检查。
	 *
	 * 这里的检查可以省略而不损失正确性；它
	 * 纯粹是一种优化。
	 *
	 * 对堆插入，我们只需要检查表级 SSI 锁。我们的
	 * 新元组不可能与已有元组锁冲突，而堆
	 * 页锁只是元组锁的汇总形式；它们不像索引页锁那样
	 * 锁定"间隙"。因此调用时不必指定
	 * 缓冲区，检查也就更快。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	ndone = 0;
	while (ndone < ntuples)
	{
		Buffer		buffer;
		bool		all_visible_cleared = false;
		bool		all_frozen_set = false;
		int			nthispage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 计算最坏情况下容纳待插入元组所需的页数。
		 * 若需要，RelationGetBufferForTuple() 会用它
		 * 来确定把关系扩展多少。
		 * 若我们刚从空白填满了前一页，
		 * 只需更新上一次计算，但若是从部分填充的页开始，
		 * 就要从头重算，潜在所需页数
		 * 会因元组需要放进页面、页头
		 * 等因素而变化。
		 */
		if (ndone == 0 || !starting_with_empty_page)
		{
			npages = heap_multi_insert_pages(heaptuples, ndone, ntuples,
											 saveFreeSpace);
			npages_used = 0;
		}
		else
			npages_used++;

		/*
		 * 寻找至少能放下下一个元组的缓冲区。若该页
		 * 全可见，这里也会钉住所需的可见性映射页。
		 *
		 * 若 COPY FREEZE 把元组插入空页，也要钉住可见性映射页。
		 * 见下面的 all_frozen_set。
		 */
		buffer = RelationGetBufferForTuple(relation, heaptuples[ndone]->t_len,
										   InvalidBuffer, options, bistate,
										   &vmbuffer, NULL,
										   npages - npages_used);
		page = BufferGetPage(buffer);

		starting_with_empty_page = PageGetMaxOffsetNumber(page) == 0;

		if (starting_with_empty_page && (options & HEAP_INSERT_FROZEN))
			all_frozen_set = true;

		/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
		START_CRIT_SECTION();

		/*
		 * RelationGetBufferForTuple 已确保第一个元组放得下。
		 * 把它放到页上，然后尽量放下其他元组。
		 */
		RelationPutHeapTuple(relation, buffer, heaptuples[ndone], false);

		/*
		 * 为了逻辑解码，我们需要组合 CID 才能正确解码
		 * 系统目录。
		 */
		if (needwal && need_cids)
			log_heap_new_cid(relation, heaptuples[ndone]);

		for (nthispage = 1; ndone + nthispage < ntuples; nthispage++)
		{
			HeapTuple	heaptup = heaptuples[ndone + nthispage];

			if (PageGetHeapFreeSpace(page) < MAXALIGN(heaptup->t_len) + saveFreeSpace)
				break;

			RelationPutHeapTuple(relation, buffer, heaptup, false);

			/*
			 * 为了逻辑解码，我们需要组合 CID 才能正确解码
			 * 系统目录。
			 */
			if (needwal && need_cids)
				log_heap_new_cid(relation, heaptup);
		}

		/*
		 * 若该页全可见，需要清除该标志，除非我们只打算
		 * 再往里添加已冻结的行。
		 *
		 * 若只是向先前为空的页添加已经冻结的行，
		 * 则把它标为全可见。
		 */
		if (PageIsAllVisible(page) && !(options & HEAP_INSERT_FROZEN))
		{
			all_visible_cleared = true;
			PageClearAllVisible(page);
			visibilitymap_clear(relation,
								BufferGetBlockNumber(buffer),
								vmbuffer, VISIBILITYMAP_VALID_BITS);
		}
		else if (all_frozen_set)
			PageSetAllVisible(page);

		/*
		 * XXX 是否应当在此页上设置 PageSetPrunable？见 heap_insert()
		 */

		MarkBufferDirty(buffer);

		/* XLOG 相关 */
		if (needwal)
		{
			XLogRecPtr	recptr;
			xl_heap_multi_insert *xlrec;
			uint8		info = XLOG_HEAP2_MULTI_INSERT;
			char	   *tupledata;
			int			totaldatalen;
			char	   *scratchptr = scratch.data;
			bool		init;
			int			bufflags = 0;

			/*
			 * 若该页先前为空，可以重新初始化该页，
			 * 而不必恢复整页。
			 */
			init = starting_with_empty_page;

			/* 从暂存区分配 xl_heap_multi_insert 结构 */
			xlrec = (xl_heap_multi_insert *) scratchptr;
			scratchptr += SizeOfHeapMultiInsert;

			/*
			 * 分配偏移数组。除非我们正在重新初始化该页，
			 * 那种情况下元组按顺序存放，起点是
			 * FirstOffsetNumber，因而不需要显式
			 * 存储这些偏移。
			 */
			if (!init)
				scratchptr += nthispage * sizeof(OffsetNumber);

			/* 暂存空间的其余部分用于元组数据 */
			tupledata = scratchptr;

			/* 检查互斥的标志没有被同时设置 */
			Assert(!(all_visible_cleared && all_frozen_set));

			xlrec->flags = 0;
			if (all_visible_cleared)
				xlrec->flags = XLH_INSERT_ALL_VISIBLE_CLEARED;
			if (all_frozen_set)
				xlrec->flags = XLH_INSERT_ALL_FROZEN_SET;

			xlrec->ntuples = nthispage;

			/*
			 * 为每个元组写出一个 xl_multi_insert_tuple 以及元组数据
			 * 本身。
			 */
			for (i = 0; i < nthispage; i++)
			{
				HeapTuple	heaptup = heaptuples[ndone + i];
				xl_multi_insert_tuple *tuphdr;
				int			datalen;

				if (!init)
					xlrec->offsets[i] = ItemPointerGetOffsetNumber(&heaptup->t_self);
				/* xl_multi_insert_tuple 需要两字节对齐。 */
				tuphdr = (xl_multi_insert_tuple *) SHORTALIGN(scratchptr);
				scratchptr = ((char *) tuphdr) + SizeOfMultiInsertTuple;

				tuphdr->t_infomask2 = heaptup->t_data->t_infomask2;
				tuphdr->t_infomask = heaptup->t_data->t_infomask;
				tuphdr->t_hoff = heaptup->t_data->t_hoff;

				/* 写入位图 [+ 填充] [+ oid] + 数据 */
				datalen = heaptup->t_len - SizeofHeapTupleHeader;
				memcpy(scratchptr,
					   (char *) heaptup->t_data + SizeofHeapTupleHeader,
					   datalen);
				tuphdr->datalen = datalen;
				scratchptr += datalen;
			}
			totaldatalen = scratchptr - tupledata;
			Assert((scratchptr - scratch.data) < BLCKSZ);

			if (need_tuple_data)
				xlrec->flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;

			/*
			 * 表明这是本次 heap_multi_insert() 调用发出的最后一条
			 * xl_heap_multi_insert 记录。逻辑解码需要它，
			 * 以便知道何时清理临时数据。
			 */
			if (ndone + nthispage == ntuples)
				xlrec->flags |= XLH_INSERT_LAST_IN_MULTI;

			if (init)
			{
				info |= XLOG_HEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}

			/*
			 * 若正在做逻辑解码，即使对该页拍了整页映像，
			 * 也要包含新元组数据。
			 */
			if (need_tuple_data)
				bufflags |= REGBUF_KEEP_DATA;

			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, tupledata - scratch.data);
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);

			XLogRegisterBufData(0, tupledata, totaldatalen);

			/* 按行过滤复制源要高效得多 */
			XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

			recptr = XLogInsert(RM_HEAP2_ID, info);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * 若我们已经冻结了该页上的一切，就更新可见性映射。
		 * 我们已经钉住了 vmbuffer。
		 */
		if (all_frozen_set)
		{
			Assert(PageIsAllVisible(page));
			Assert(visibilitymap_pin_ok(BufferGetBlockNumber(buffer), vmbuffer));

			/*
			 * 这里使用 InvalidTransactionId 没问题——它只在指定了
			 * HEAP_INSERT_FROZEN 时使用，而该选项有意
			 * 违反可见性规则。
			 */
			visibilitymap_set(relation, BufferGetBlockNumber(buffer), buffer,
							  InvalidXLogRecPtr, vmbuffer,
							  InvalidTransactionId,
							  VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN);
		}

		UnlockReleaseBuffer(buffer);
		ndone += nthispage;

		/*
		 * 注意：要等所有元组都插入之后再释放 vmbuffer——我们很
		 * 可能接着插入后续堆页，而那些页很
		 * 可能使用同一张可见性映射页。
		 */
	}

	/* 所有元组都插入完了，于是释放最后一个 vmbuffer。 */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 实际插入已经完成。再次检查冲突，以
	 * 确保所有这些插入上的读写冲突都被检测到。若没有
	 * 这最后一次检查，堆的顺序扫描可能在"之前"检查之后
	 * 才锁住
	 * 表，错过一次检测冲突的机会，然后又在新元组出现之前扫过该表，
	 * 错过另一次检测冲突的机会。
	 *
	 * 对堆插入，我们只需要检查表级 SSI 锁。我们的
	 * 新元组不可能与已有元组锁冲突，而堆
	 * 页锁只是元组锁的汇总形式；它们不像索引页锁那样
	 * 锁定"间隙"。因此调用时不必指定
	 * 缓冲区。
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/*
	 * 若元组可缓存，则标记它们，以便在我们中止时从缓存中失效。
	 * 注意在释放缓冲区之后做这件事是可以的，
	 * 因为 heaptuples 数据结构全在本地内存里，不在
	 * 共享缓冲区中。
	 */
	if (IsCatalogRelation(relation))
	{
		for (i = 0; i < ntuples; i++)
			CacheInvalidateHeapTuple(relation, heaptuples[i], NULL);
	}

	/* 把 t_self 字段复制回调用方的槽 */
	for (i = 0; i < ntuples; i++)
		slots[i]->tts_tid = heaptuples[i]->t_self;

	pgstat_count_heap_insert(relation, ntuples);
}

/*
 *	simple_heap_insert - 插入一个元组
 *
 * 目前，本例程与 heap_insert 的差别只是提供
 * 一个默认命令 ID，并且不允许使用加速选项。
 *
 * 在修改系统目录的大多数地方，应当使用它，
 * 而不是直接使用 heap_insert。
 */
void
simple_heap_insert(Relation relation, HeapTuple tup)
{
	heap_insert(relation, tup, GetCurrentCommandId(true), 0, NULL);
}

/*
 * 给定 infomask/infomask2，计算必须保存在
 * xl_heap_delete、xl_heap_update、xl_heap_lock、
 * xl_heap_lock_updated 这些 WAL 记录的 "infobits" 字段中的位。
 *
 * 见 fix_infomask_from_infobits。
 */
static uint8
compute_infobits(uint16 infomask, uint16 infomask2)
{
	return
		((infomask & HEAP_XMAX_IS_MULTI) != 0 ? XLHL_XMAX_IS_MULTI : 0) |
		((infomask & HEAP_XMAX_LOCK_ONLY) != 0 ? XLHL_XMAX_LOCK_ONLY : 0) |
		((infomask & HEAP_XMAX_EXCL_LOCK) != 0 ? XLHL_XMAX_EXCL_LOCK : 0) |
	/* 注意这里忽略 HEAP_XMAX_SHR_LOCK */
		((infomask & HEAP_XMAX_KEYSHR_LOCK) != 0 ? XLHL_XMAX_KEYSHR_LOCK : 0) |
		((infomask2 & HEAP_KEYS_UPDATED) != 0 ?
		 XLHL_KEYS_UPDATED : 0);
}

/*
 * 给定同一元组 t_infomask 的两个版本，比较它们并
 * 返回与元组 Xmax 相关的状态是否已经改变。这用在
 * 缓冲区锁被释放后又重新获取之后：我们要确保
 * 元组状态仍然与我们先前
 * 检查它时相同。
 *
 * 注意 Xmax 字段本身必须单独比较。
 */
static inline bool
xmax_infomask_changed(uint16 new_infomask, uint16 old_infomask)
{
	const uint16 interesting =
		HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY | HEAP_LOCK_MASK;

	if ((new_infomask & interesting) != (old_infomask & interesting))
		return true;

	return false;
}

/*
 *	heap_delete - 删除一个元组
 *
 * 参数说明见 table_tuple_delete()，只是
 * 本例程直接接受元组而不是槽。
 *
 * 失败时，本例程用该元组的 t_ctid、
 * t_xmax（必要时解析可能的 MultiXact）和 t_cmax 填好 *tmfd（后者
 * 仅用于 TM_SelfModified，因为我们无法从另一个事务生成的组合 CID
 * 取得 cmax）。
 */
TM_Result
heap_delete(Relation relation, ItemPointer tid,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, bool changingPart)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		all_visible_cleared = false;
	HeapTuple	old_key_tuple = NULL;	/* 该元组的副本标识 */
	bool		old_key_copied = false;

	Assert(ItemPointerIsValid(tid));

	AssertHasSnapshotForToast(relation);

	/*
	 * 并行操作期间禁止这样做，以免它分配组合 CID。
	 * 其他工作进程做可见性检查时可能需要该组合 CID，而我们
	 * 没有把它广播给它们的机制。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * 锁定缓冲区之前，若看起来有必要，先钉住可见性映射页。
	 * 由于我们还没拿到锁，别人可能正在
	 * 改它，所以拿到锁之后
	 * 还需要再检查一次。
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:

	/*
	 * 若我们没有钉住可见性映射页，而该页在我们忙着锁缓冲区时变成了全
	 * 可见，就得解锁再重新加锁，以免跨一次 I/O 持有缓冲区锁。这有点
	 * 不幸，但希望
	 * 不会经常发生。
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to delete invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* 解锁缓冲区之前必须复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(tp.t_data);
		infomask = tp.t_data->t_infomask;

		/*
		 * 睡到并发事务结束——除非只有一个
		 * 加锁者且就是我们自己的事务。注意我们不关心
		 * 加锁者持有哪种锁模式，因为我们需要最强的那种。
		 *
		 * 睡眠之前，需要获取元组锁以确立我们对该
		 * 元组的优先级（见 heap_lock_tuple）。
		 * 当我们成为该元组的下一位时，
		 * LockTuple 会唤醒我们。
		 *
		 * 若下面被迫"重新开始"，我们保留元组锁；
		 * 这样在重新检查元组状态时，我们仍排在队列前面。
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										LockTupleExclusive, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * 必要时获取锁（但当我们正在
				 * 请求一把锁并且已经持有时则跳过；避免死锁）。
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
										 LockWaitBlock, &have_tuple_lock);

				/* 等待 multixact */
				MultiXactIdWait((MultiXactId) xwait, MultiXactStatusUpdate, infomask,
								relation, &(tp.t_self), XLTW_Delete,
								NULL);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 若 xwait 刚刚锁过该元组，那么在我们到达这里之前，别的事务
				 * 可能已经更新了它。检查
				 * xmax 是否改变，若改变则重新开始。
				 *
				 * 若我们没有钉住 VM 页，并且
				 * 该页已变为全可见，也必须重新开始。
				 */
				if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
					xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
										 xwait))
					goto l1;
			}

			/*
			 * 你也许以为这里 multixact 必然已经结束，但并非
			 * 如此：它可能还有存活成员，即我们自己的事务或
			 * 本后端的其他子事务。无论哪种情况，我们删除
			 * 该元组都是合法的（后一种情况
			 * 本质上是把先前的共享锁升级为
			 * 排他锁）。我们不必费心去改磁盘上的提示位，
			 * 因为马上就要整个覆盖 xmax。
			 */
		}
		else if (!TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * 等待普通事务结束；但首先获取元组
			 * 锁。
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &(tp.t_self), XLTW_Delete);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait 已经结束，但若 xwait 刚刚锁过该元组，那么在我们
			 * 到达这里之前，别的事务可能已经更新了它。
			 * 检查 xmax 是否改变，若改变则重新开始。
			 *
			 * 若我们没有钉住 VM 页，并且
			 * 该页已变为全可见，也必须重新开始。
			 */
			if ((vmbuffer == InvalidBuffer && PageIsAllVisible(page)) ||
				xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
									 xwait))
				goto l1;

			/* 否则检查它是已提交还是已中止 */
			UpdateXmaxHintBits(tp.t_data, buffer, xwait);
		}

		/*
		 * 若先前的 xmax 已中止，或者它已提交但
		 * 只锁了元组而没有更新它，则我们可以覆盖。
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tp.t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* 对 HeapTupleSatisfiesUpdate() 的结果和上面的逻辑做健全性检查 */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* 对事务快照模式的 RI 更新做额外检查 */
		if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = tp.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tp.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		return result;
	}

	/*
	 * 即将做实际删除——先检查冲突，以免
	 * 可能不得不回滚刚刚做完的工作。
	 *
	 * 只要不可能有另一个进程在本次检查与删除
	 * 对扫描可见之间扫描该页，不做复查也是安全的（也就是
	 * 从此时起一直持有排他的缓冲区内容锁，
	 * 直到元组删除变为可见）。
	 */
	CheckForSerializableConflictIn(relation, tid, BufferGetBlockNumber(buffer));

	/* 必要时用组合 CID 替换 cid */
	HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

	/*
	 * 进入临界区之前先计算副本标识元组，这样
	 * 内存分配失败时就不会 PANIC。
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &tp, true, &old_key_copied);

	/*
	 * 若这是当前事务中第一次可能涉及 multixact 的操作，
	 * 设置本后端的 OldestMemberMXactId。我们可以
	 * 确定该事务绝不会成为任何比它更老的
	 * MultiXactId 的成员。（即使下面最终只用
	 * 我们自己的 TransactionId，也必须这样做，因为别的后端可能
	 * 随即把我们的 XID 并入一个 MultiXact。）
	 */
	MultiXactIdSetOldestMember();

	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(tp.t_data),
							  tp.t_data->t_infomask, tp.t_data->t_infomask2,
							  xid, LockTupleExclusive, true,
							  &new_xmax, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * 若本事务提交，该元组迟早会变成 DEAD。
	 * 设置标志，表明一旦我们的 xid 落到 OldestXmin 视界之下，
	 * 此页就是剪枝候选。若事务最终中止，
	 * 随后的页面剪枝将是空操作，该提示也会被
	 * 清除。
	 */
	PageSetPrunable(page, xid);

	if (PageIsAllVisible(page))
	{
		all_visible_cleared = true;
		PageClearAllVisible(page);
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}

	/* 存储删除该元组的事务信息 */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tp.t_data->t_infomask |= new_infomask;
	tp.t_data->t_infomask2 |= new_infomask2;
	HeapTupleHeaderClearHotUpdated(tp.t_data);
	HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
	HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
	/* 确保 t_ctid 中没有向前的链链接 */
	tp.t_data->t_ctid = tp.t_self;

	/* 表明这实际上是移动到另一个分区 */
	if (changingPart)
		HeapTupleHeaderSetMovedPartitions(tp.t_data);

	MarkBufferDirty(buffer);

	/*
	 * XLOG 相关
	 *
	 * 注意：heap_abort_speculative() 使用相同的 xlog 记录和重放
	 * 例程。
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;

		/*
		 * 为了逻辑解码，我们需要组合 CID 才能正确解码
		 * 系统目录
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, &tp);

		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_DELETE_ALL_VISIBLE_CLEARED;
		if (changingPart)
			xlrec.flags |= XLH_DELETE_IS_PARTITION_MOVE;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = new_xmax;

		if (old_key_tuple != NULL)
		{
			if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_KEY;
		}

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/*
		 * 若有，则记录被删除元组的副本标识
		 */
		if (old_key_tuple != NULL)
		{
			xlhdr.t_infomask2 = old_key_tuple->t_data->t_infomask2;
			xlhdr.t_infomask = old_key_tuple->t_data->t_infomask;
			xlhdr.t_hoff = old_key_tuple->t_data->t_hoff;

			XLogRegisterData((char *) &xlhdr, SizeOfHeapHeader);
			XLogRegisterData((char *) old_key_tuple->t_data
							 + SizeofHeapTupleHeader,
							 old_key_tuple->t_len
							 - SizeofHeapTupleHeader);
		}

		/* 按行过滤复制源要高效得多 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * 若元组有经过 TOAST 的行外属性，也需要删除
	 * 那些项。必须在释放缓冲区之前做这件事，
	 * 因为我们需要查看元组内容，但可以先
	 * 释放缓冲区上的内容锁。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* TOAST 表项绝不应当被递归做 TOAST */
		Assert(!HeapTupleHasExternal(&tp));
	}
	else if (HeapTupleHasExternal(&tp))
		heap_toast_delete(relation, &tp, false);

	/*
	 * 标记该元组，以便在下一个命令
	 * 边界从系统缓存中失效。必须在释放缓冲区之前做这件事，因为我们
	 * 需要查看元组的内容。
	 */
	CacheInvalidateHeapTuple(relation, &tp, NULL);

	/* 现在可以释放缓冲区了 */
	ReleaseBuffer(buffer);

	/*
	 * 若持有过，则释放 lmgr 元组锁。
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);

	pgstat_count_heap_delete(relation);

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	return TM_Ok;
}

/*
 *	simple_heap_delete - 删除一个元组
 *
 * 当不预期目标元组会有并发更新时
 * （例如因为我们持有与该元组关联的关系上的锁），
 * 可以用本例程删除元组。任何失败都通过
 * ereport() 报告。
 */
void
simple_heap_delete(Relation relation, ItemPointer tid)
{
	TM_Result	result;
	TM_FailureData tmfd;

	result = heap_delete(relation, tid,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* 等待提交 */ ,
						 &tmfd, false /* changingPart：正在改分区 */ );
	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中被更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
			break;
	}
}

/*
 *	heap_update - 替换一个元组
 *
 * 参数说明见 table_tuple_update()，只是
 * 本例程直接接受元组而不是槽。
 *
 * 失败时，本例程用该元组的 t_ctid、
 * t_xmax（必要时解析可能的 MultiXact）和 t_cmax 填好 *tmfd（后者
 * 仅用于 TM_SelfModified，因为我们无法从另一个事务生成的组合 CID
 * 取得 cmax）。
 */
TM_Result
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			CommandId cid, Snapshot crosscheck, bool wait,
			TM_FailureData *tmfd, LockTupleMode *lockmode,
			TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TransactionId xid = GetCurrentTransactionId();
	Bitmapset  *hot_attrs;
	Bitmapset  *sum_attrs;
	Bitmapset  *key_attrs;
	Bitmapset  *id_attrs;
	Bitmapset  *interesting_attrs;
	Bitmapset  *modified_attrs;
	ItemId		lp;
	HeapTupleData oldtup;
	HeapTuple	heaptup;
	HeapTuple	old_key_tuple = NULL;
	bool		old_key_copied = false;
	Page		page;
	BlockNumber block;
	MultiXactStatus mxact_status;
	Buffer		buffer,
				newbuf,
				vmbuffer = InvalidBuffer,
				vmbuffer_new = InvalidBuffer;
	bool		need_toast;
	Size		newtupsize,
				pagefree;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		use_hot_update = false;
	bool		summarized_update = false;
	bool		key_intact;
	bool		all_visible_cleared = false;
	bool		all_visible_cleared_new = false;
	bool		checked_lockers;
	bool		locker_remains;
	bool		id_has_external = false;
	TransactionId xmax_new_tuple,
				xmax_old_tuple;
	uint16		infomask_old_tuple,
				infomask2_old_tuple,
				infomask_new_tuple,
				infomask2_new_tuple;

	Assert(ItemPointerIsValid(otid));

	/* 粗略、简单的检查：元组是否匹配关系的行类型。 */
	Assert(HeapTupleHeaderGetNatts(newtup->t_data) <=
		   RelationGetNumberOfAttributes(relation));

	AssertHasSnapshotForToast(relation);

	/*
	 * 并行操作期间禁止这样做，以免它分配组合 CID。
	 * 其他工作进程做可见性检查时可能需要该组合 CID，而我们
	 * 没有把它广播给它们的机制。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

#ifdef USE_ASSERT_CHECKING
	check_lock_if_inplace_updateable_rel(relation, otid, newtup);
#endif

	/*
	 * 取出供各种操作检查的属性列表。
	 *
	 * 从 HOT 的角度看，若更新失败或
	 * 不得不把新元组放到另一页，这笔功夫就白费了。但我们必须在
	 * 取得缓冲区锁之前算出这份列表——最坏情况下，若我们正在
	 * 更新某个相关系统目录，稍后再取这份列表就可能
	 * 死锁。无论如何，relcache
	 * 会缓存这些数据，所以通常相当便宜。
	 *
	 * 我们还需要副本标识使用的列，以及被
	 * 视为表中行"键"的那些列。
	 *
	 * 注意我们拿到的是每张位图的副本，因此不必担心
	 * 中途发生 relcache 刷新。
	 */
	hot_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_HOT_BLOCKING);
	sum_attrs = RelationGetIndexAttrBitmap(relation,
										   INDEX_ATTR_BITMAP_SUMMARIZED);
	key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
	id_attrs = RelationGetIndexAttrBitmap(relation,
										  INDEX_ATTR_BITMAP_IDENTITY_KEY);
	interesting_attrs = NULL;
	interesting_attrs = bms_add_members(interesting_attrs, hot_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, sum_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, key_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, id_attrs);

	block = ItemPointerGetBlockNumber(otid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * 锁定缓冲区之前，若看起来有必要，先钉住可见性映射页。
	 * 由于我们还没拿到锁，别人可能正在
	 * 改它，所以拿到锁之后
	 * 还需要再检查一次。
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));

	/*
	 * 通常，缓冲区钉住和/或快照会阻止对 otid 剪枝，从而保证
	 * 我们在这里看到 LP_NORMAL。当 otid 来自 syscache 时，我们可能既没有
	 * 钉，也没有快照。因此可能看到其他 LP_ 状态，每一种
	 * 都表示发生了并发剪枝。
	 *
	 * 以 TM_Updated 失败最为准确。不过，与其他
	 * TM_Updated 情形不同，在 LP_UNUSED 和
	 * LP_DEAD 情形下我们不知道后继 ctid。TM_Updated 与 TM_Deleted
	 * 的区别对 SQL 语句 UPDATE 和 MERGE 确实重要，但那些 SQL 语句
	 * 持有快照，能保证 LP_NORMAL。因此在 TM_Updated 与
	 * TM_Deleted 之间的选择只影响错误消息的措辞。
	 * 我们选定 TM_Deleted，原因有二。第一，它避免把
	 * tmfd->ctid 何时有效的规格说明搞复杂。第二，它在
	 * 错误日志里留下我们走过这条分支的证据。
	 *
	 * 既然在 otid 处可能看到 LP_UNUSED，也就可能看到
	 * 替换了 LP_UNUSED 的元组处于 LP_NORMAL。若它是
	 * 无关行的元组，我们会因"duplicate key value violates unique"而失败。
	 * XXX 若 otid 是 newtup 这一行更新的、仍活着的版本，我们会丢弃
	 * 源自该系统目录行、晚于调用方从 syscache 得到的那个版本的
	 * 变更。见 syscache-update-pruned.spec。
	 */
	if (!ItemIdIsNormal(lp))
	{
		Assert(RelationSupportsSysCache(RelationGetRelid(relation)));

		UnlockReleaseBuffer(buffer);
		Assert(!have_tuple_lock);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		tmfd->ctid = *otid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		/* modified_attrs 尚未初始化 */
		bms_free(interesting_attrs);
		return TM_Deleted;
	}

	/*
	 * 在 oldtup 中填入足够的数据，使 HeapDetermineColumnsInfo 能正常
	 * 工作。
	 */
	oldtup.t_tableOid = RelationGetRelid(relation);
	oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = *otid;

	/* 新元组已经就绪，除了这一点： */
	newtup->t_tableOid = RelationGetRelid(relation);

	/*
	 * 确定本次更新修改了哪些列。此外，识别
	 * 旧元组中未被修改的副本标识键属性是否有
	 * 外部存储的。这是必需的，因为对
	 * 这类属性，扁平化后的值不会作为新元组的一部分记入 WAL，
	 * 所以我们必须把它包含进 old_key_tuple。见
	 * ExtractReplicaIdentity。
	 */
	modified_attrs = HeapDetermineColumnsInfo(relation, interesting_attrs,
											  id_attrs, &oldtup,
											  newtup, &id_has_external);

	/*
	 * 若我们没有更新任何"键"列，就可以获取较弱的锁类型。
	 * 这样在与外键检查同时运行时，可以有更高的
	 * 并发度。
	 *
	 * 注意若某列在执行更新时被解除 TOAST，但
	 * 最终值相同，这个测试会失败，我们将使用
	 * 更强的锁。这是可以接受的；值得优化的重要情形是
	 * 不改动键列的更新，而不是那些
	 * 碰巧得到相同键值的更新。
	 */
	if (!bms_overlap(modified_attrs, key_attrs))
	{
		*lockmode = LockTupleNoKeyExclusive;
		mxact_status = MultiXactStatusNoKeyUpdate;
		key_intact = true;

		/*
		 * 若这是当前事务中第一次可能涉及 multixact 的
		 * 操作，就设置本后端的
		 * OldestMemberMXactId。
		 * 我们可以确定该事务绝不会成为任何比它更老的
		 * MultiXactId 的成员。（即使下面最终只用
		 * 我们自己的 TransactionId，也必须这样做，因为
		 * 别的后端可能随即把我们的 XID 并入一个 MultiXact。）
		 */
		MultiXactIdSetOldestMember();
	}
	else
	{
		*lockmode = LockTupleExclusive;
		mxact_status = MultiXactStatusUpdate;
		key_intact = false;
	}

	/*
	 * 注意：过了这一点，用 oldtup 而不是 otid 来指旧元组。
	 * otid 很可能会指向 newtup->t_self，而我们即将用
	 * 新元组的位置覆盖它，若再使用 otid 就很容易
	 * 混淆。
	 */

l2:
	checked_lockers = false;
	locker_remains = false;
	result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);

	/* 关于"不等待"情形见下文 */
	Assert(result != TM_BeingModified || wait);

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to update invisible tuple")));
	}
	else if (result == TM_BeingModified && wait)
	{
		TransactionId xwait;
		uint16		infomask;
		bool		can_continue = false;

		/*
		 * XXX 注意这里没有考虑"不等待"情形。目前
		 * 这不是问题，因为没有调用方使用该情形，但若
		 * 引入这样的调用方就应当修复。以前这不是
		 * 问题，因为这段代码总会等待，但现在
		 * 有些元组锁与我们使用的某种锁模式并不冲突，
		 * 因此这个情形也许值得
		 * 专门处理。
		 *
		 * 这可能使直接调用 heap_update 的第三方代码
		 * 失败。
		 */

		/* 解锁缓冲区之前必须复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		/*
		 * 现在必须对已有的加锁者做点什么。若它是
		 * multi，就在它上面睡眠；我们可能在它完全
		 * 消失之前就被唤醒（有些情况下甚至根本不睡）；需要把它保留
		 * 为加锁者，除非它已经完全消失。
		 *
		 * 若它不是 multi，在真正睡下去之前需要检查睡眠条件。
		 * 若更新与这些锁不冲突，
		 * 我们就继续而不睡眠（但要确保
		 * 把它保留下来）。
		 *
		 * 睡眠之前，需要获取元组锁以确立我们对该
		 * 元组的优先级（见 heap_lock_tuple）。LockTuple 会在
		 * 我们成为该元组的下一位时唤醒我们。注意在确定要睡眠之前不得
		 * 获取元组锁；
		 * 否则就会与其他持有该元组锁、
		 * 又在我们身上睡眠的事务发生竞态。
		 *
		 * 若下面被迫"重新开始"，我们保留元组锁；
		 * 这样在重新检查
		 * 元组状态时，我们仍排在队列前面。
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			TransactionId update_xact;
			int			remain;
			bool		current_is_member = false;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										*lockmode, &current_is_member))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/*
				 * 必要时获取锁（但当我们正在
				 * 请求一把锁并且已经持有时则跳过；避免死锁）。
				 */
				if (!current_is_member)
					heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
										 LockWaitBlock, &have_tuple_lock);

				/* 等待 multixact */
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
				checked_lockers = true;
				locker_remains = remain != 0;
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 若 xwait 刚刚锁过该元组，那么在我们到达这里之前，别的事务
				 * 可能已经更新了它。检查
				 * xmax 是否改变，若改变则重新开始。
				 */
				if (xmax_infomask_changed(oldtup.t_data->t_infomask,
										  infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(oldtup.t_data),
										 xwait))
					goto l2;
			}

			/*
			 * 注意此时 multixact 未必已经结束。它可能还有
			 * 存活成员：我们自己的事务，或本后端的其他子事务，
			 * 以及任何其他并发事务：若我们只拿到
			 * LockTupleNoKeyExclusive，它们用 LockTupleKeyShare 锁过该元组。
			 * 若是这种情况，我们必须小心地用这些存活成员
			 * 来标记更新后的元组，写在
			 * Xmax 里。
			 *
			 * 注意 MultiXact 里也可能有过另一次更新。
			 * 那种情况下，需要检查它是已提交
			 * 还是已中止。若已中止，我们可以安全地再次更新；
			 * 否则就是更新冲突，必须在下面返回
			 * TableTuple{Deleted, Updated}。
			 *
			 * 在 LockTupleExclusive 情形下，仍需要保留
			 * 存活成员：其中包括我们在这一把锁之前持有的元组锁，
			 * 若本子事务中止，保留它们
			 * 很重要。
			 */
			if (!HEAP_XMAX_IS_LOCKED_ONLY(oldtup.t_data->t_infomask))
				update_xact = HeapTupleGetUpdateXid(oldtup.t_data);
			else
				update_xact = InvalidTransactionId;

			/*
			 * MultiXact 里没有 UPDATE；或者它已中止。这里不需要
			 * 调用 TransactionIdIsInProgress()，因为上面已经调用了
			 * MultiXactIdWait()。
			 */
			if (!TransactionIdIsValid(update_xact) ||
				TransactionIdDidAbort(update_xact))
				can_continue = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * 唯一的加锁者是我们自己；这里可以避免获取元组
			 * 锁，但必须保留我们的锁定信息。
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) && key_intact)
		{
			/*
			 * 若它只是键共享加锁者，而我们没有改键
			 * 列，就不需要等它结束；但需要
			 * 把它保留为加锁者。
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else
		{
			/*
			 * 等待普通事务结束；但首先获取元组
			 * 锁。
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
			checked_lockers = true;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait 已经结束，但若 xwait 刚刚锁过该元组，那么别的
			 * 事务可能在我们到达这里之前更新了它。
			 * 检查 xmax 是否改变，若改变则重新开始。
			 */
			if (xmax_infomask_changed(oldtup.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(xwait,
									 HeapTupleHeaderGetRawXmax(oldtup.t_data)))
				goto l2;

			/* 否则检查它是已提交还是已中止 */
			UpdateXmaxHintBits(oldtup.t_data, buffer, xwait);
			if (oldtup.t_data->t_infomask & HEAP_XMAX_INVALID)
				can_continue = true;
		}

		if (can_continue)
			result = TM_Ok;
		else if (!ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

	/* 对 HeapTupleSatisfiesUpdate() 的结果和上面的逻辑做健全性检查 */
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified ||
			   result == TM_Updated ||
			   result == TM_Deleted ||
			   result == TM_BeingModified);
		Assert(!(oldtup.t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&oldtup.t_self, &oldtup.t_data->t_ctid));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* 对事务快照模式的 RI 更新做额外检查 */
		if (!HeapTupleSatisfiesVisibility(&oldtup, crosscheck, buffer))
			result = TM_Updated;
	}

	if (result != TM_Ok)
	{
		tmfd->ctid = oldtup.t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(oldtup.t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(oldtup.t_data);
		else
			tmfd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		*update_indexes = TU_None;

		bms_free(hot_attrs);
		bms_free(sum_attrs);
		bms_free(key_attrs);
		bms_free(id_attrs);
		bms_free(modified_attrs);
		bms_free(interesting_attrs);
		return result;
	}

	/*
	 * 若我们没有钉住可见性映射页，而该页在我们忙着锁缓冲区时变成了全可见，
	 * 或在随后某个我们把它解锁的窗口期间也变成了全可见，
	 * 就得解锁再重新加锁，以免跨一次 I/O 持有缓冲区锁。这有点
	 * 不幸，尤其是现在还得重新检查
	 * 该元组是否已在我们手下被锁定或更新，
	 * 但希望这种事
	 * 不会经常发生。
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l2;
	}

	/* 填入事务状态数据 */

	/*
	 * 若正在更新的元组被锁着，需要在旧元组的 Xmax 中保留锁定
	 * 信息。为此准备一个新的 Xmax 值。
	 */
	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
							  oldtup.t_data->t_infomask,
							  oldtup.t_data->t_infomask2,
							  xid, *lockmode, true,
							  &xmax_old_tuple, &infomask_old_tuple,
							  &infomask2_old_tuple);

	/*
	 * 同时为元组的新副本准备一个 Xmax 值。若先前
	 * 没有 xmax，或者有过但所有加锁者都已离开，
	 * 则使用 InvalidTransactionId；否则从旧
	 * 元组取得 xmax。（罕见情况下它也可能是 InvalidTransactionId，却
	 * 没有设置 HEAP_XMAX_INVALID 位；这没问题。）
	 */
	if ((oldtup.t_data->t_infomask & HEAP_XMAX_INVALID) ||
		HEAP_LOCKED_UPGRADED(oldtup.t_data->t_infomask) ||
		(checked_lockers && !locker_remains))
		xmax_new_tuple = InvalidTransactionId;
	else
		xmax_new_tuple = HeapTupleHeaderGetRawXmax(oldtup.t_data);

	if (!TransactionIdIsValid(xmax_new_tuple))
	{
		infomask_new_tuple = HEAP_XMAX_INVALID;
		infomask2_new_tuple = 0;
	}
	else
	{
		/*
		 * 若为新元组找到了有效的 Xmax，那么新元组上使用的 infomask 位
		 * 取决于旧元组上原来有什么。
		 * 注意既然我们在做更新，唯一的可能是
		 * 加锁者持有的是 FOR KEY SHARE 锁。
		 */
		if (oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI)
		{
			GetMultiXactIdHintBits(xmax_new_tuple, &infomask_new_tuple,
								   &infomask2_new_tuple);
		}
		else
		{
			infomask_new_tuple = HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_LOCK_ONLY;
			infomask2_new_tuple = 0;
		}
	}

	/*
	 * 用上面算出的 Xmin、Xmax 初值以及
	 * infomask 位初值来准备新元组。
	 */
	newtup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	newtup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	newtup->t_data->t_infomask |= HEAP_UPDATED | infomask_new_tuple;
	newtup->t_data->t_infomask2 |= infomask2_new_tuple;
	HeapTupleHeaderSetXmax(newtup->t_data, xmax_new_tuple);

	/*
	 * 必要时用组合 CID 替换 cid。注意我们已经把
	 * 普通 cid 放进了新元组。
	 */
	HeapTupleHeaderAdjustCmax(oldtup.t_data, &cid, &iscombo);

	/*
	 * 若需要启动 toaster，或者新元组放不进
	 * 与旧元组相同的页，那么在我们去做
	 * TOAST 和/或扩展表文件时，必须释放旧元组缓冲区上的
	 * 内容锁
	 * （但不要释放钉！）。必须把旧元组标成
	 * 已锁定，否则其他进程可能自己尝试更新它。
	 *
	 * 若已经存在任何行外的
	 * 已 TOAST 值，或者新元组超过阈值，就需要调用 toaster。
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* TOAST 表项绝不应当被递归做 TOAST */
		Assert(!HeapTupleHasExternal(&oldtup));
		Assert(!HeapTupleHasExternal(newtup));
		need_toast = false;
	}
	else
		need_toast = (HeapTupleHasExternal(&oldtup) ||
					  HeapTupleHasExternal(newtup) ||
					  newtup->t_len > TOAST_TUPLE_THRESHOLD);

	pagefree = PageGetHeapFreeSpace(page);

	newtupsize = MAXALIGN(newtup->t_len);

	if (need_toast || newtupsize > pagefree)
	{
		TransactionId xmax_lock_old_tuple;
		uint16		infomask_lock_old_tuple,
					infomask2_lock_old_tuple;
		bool		cleared_all_frozen = false;

		/*
		 * 为防止并发会话更新该元组，我们必须
		 * 在释放页级锁的同时，临时把它标成已锁定。
		 *
		 * 为满足"任何可能出现在被写出磁盘的缓冲区里的 xid"
		 * 这条规则，不幸的是我们必须把这次
		 * 临时修改记入 WAL。为此可以复用 xl_heap_lock。
		 * 若我们在完成真正的更新之前崩溃或出错，
		 * xmax 将属于一个已中止的事务，从而允许
		 * 其他会话继续。
		 */

		/*
		 * 计算适合锁定该元组的 xmax / infomask。这必须
		 * 与即将用于更新的组合分开做，
		 * 否则可能创建出来的 multixact 会是
		 * 错的。
		 */
		compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
								  oldtup.t_data->t_infomask,
								  oldtup.t_data->t_infomask2,
								  xid, *lockmode, false,
								  &xmax_lock_old_tuple, &infomask_lock_old_tuple,
								  &infomask2_lock_old_tuple);

		Assert(HEAP_XMAX_IS_LOCKED_ONLY(infomask_lock_old_tuple));

		START_CRIT_SECTION();

		/* 清除过时的可见性标志…… */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleClearHotUpdated(&oldtup);
		/* ……并存储更新该元组的事务信息 */
		Assert(TransactionIdIsValid(xmax_lock_old_tuple));
		HeapTupleHeaderSetXmax(oldtup.t_data, xmax_lock_old_tuple);
		oldtup.t_data->t_infomask |= infomask_lock_old_tuple;
		oldtup.t_data->t_infomask2 |= infomask2_lock_old_tuple;
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

		/* 临时让它看起来未被更新，但已锁定 */
		oldtup.t_data->t_ctid = oldtup.t_self;

		/*
		 * 若需要，清除可见性映射上的全冻结位。我们本可以
		 * 立刻重置 ALL_VISIBLE，但既然 WAL 记录
		 * 开销不会变，那样做似乎并不一定
		 * 值得。
		 */
		if (PageIsAllVisible(page) &&
			visibilitymap_clear(relation, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		MarkBufferDirty(buffer);

		if (RelationNeedsWAL(relation))
		{
			xl_heap_lock xlrec;
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&oldtup.t_self);
			xlrec.xmax = xmax_lock_old_tuple;
			xlrec.infobits_set = compute_infobits(oldtup.t_data->t_infomask,
												  oldtup.t_data->t_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
			XLogRegisterData((char *) &xlrec, SizeOfHeapLock);
			recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 若需要，让 toaster 做它的事。
		 *
		 * 注意：从此往下，heaptup 是我们实际打算
		 * 存入关系的数据；newtup 是调用方原始的、未做 TOAST 的
		 * 数据。
		 */
		if (need_toast)
		{
			/* 注意更新时我们总是使用 WAL 和 FSM */
			heaptup = heap_toast_insert_or_update(relation, newtup, &oldtup, 0);
			newtupsize = MAXALIGN(heaptup->t_len);
		}
		else
			heaptup = newtup;

		/*
		 * 那么，这个元组需不需要新页？这有点
		 * 棘手，因为在我们没看的时候，别人可能已经往该页加了元组。
		 * 重新获取缓冲区锁之后，必须复查可用空间。
		 * 但若原先的空闲空间仍然不够，就不必费心去查；
		 * 现在的空闲不太可能比以前更多。
		 *
		 * 更进一步，若需要取得新页，就必须获取
		 * 旧页和新页上的缓冲区锁。为避免与
		 * 另一个以后相反顺序获取同样两把锁的后端死锁，
		 * 我们必须在获取锁的顺序上保持一致。
		 * 我们使用的规则是"先锁关系中页号较小的那一页"。
		 * 为实现这一点，必须在不持有旧页锁的情况下
		 * 调用 RelationGetBufferForTuple，并且必须依靠它
		 * 按正确顺序取得两页上的
		 * 锁。
		 *
		 * 另一个考虑是：若必须清除任一页上的全可见标志，
		 * 就需要钉住可见性映射页。若我们
		 * 调用 RelationGetBufferForTuple，就依靠它来获取这些
		 * 钉；若不调用，就得在这里自己处理。因此我们需要
		 * 一个循环。
		 */
		for (;;)
		{
			if (newtupsize > pagefree)
			{
				/* 放不下，必须使用 RelationGetBufferForTuple。 */
				newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
												   buffer, 0, NULL,
												   &vmbuffer_new, &vmbuffer,
												   0);
				/* 全部完成。 */
				break;
			}
			/* 若需要且尚未持有，则获取 VM 页钉住。 */
			if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
				visibilitymap_pin(relation, block, &vmbuffer);
			/* 重新获取旧元组所在页上的锁。 */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* 用最新的空闲空间再检查一次 */
			pagefree = PageGetHeapFreeSpace(page);
			if (newtupsize > pagefree ||
				(vmbuffer == InvalidBuffer && PageIsAllVisible(page)))
			{
				/*
				 * 糟糕，现在放不下了，或者刚刚有人设置了
				 * 全可见标志。现在必须解锁并回到循环，以免
				 * 死锁。幸好这条路径应当很少走到。
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			}
			else
			{
				/* 全部完成。 */
				newbuf = buffer;
				break;
			}
		}
	}
	else
	{
		/* 不需要做 TOAST，并且它能放进同一页 */
		newbuf = buffer;
		heaptup = newtup;
	}

	/*
	 * 即将做实际更新——先检查冲突，以免
	 * 可能不得不回滚刚刚做完的工作。
	 *
	 * 只要不可能有另一个进程在本次检查与更新
	 * 对扫描可见之间扫描这些页，不做复查也是安全的（也就是
	 * 从此时起一直持有排他的缓冲区内容锁，
	 * 直到元组更新变为可见）。
	 *
	 * 对新元组，唯一需要的检查是关系级的，但
	 * 由于两个元组在同一关系中，而对 oldtup 的检查
	 * 已经包含关系级检查，再为新元组做一次
	 * 单独检查没有好处。
	 */
	CheckForSerializableConflictIn(relation, &oldtup.t_self,
								   BufferGetBlockNumber(buffer));

	/*
	 * 此时 newbuf 和 buffer 都已钉住并上锁，而且 newbuf
	 * 有足够空间放下新元组。若它们是同一缓冲区，只
	 * 持有一个钉。
	 */

	if (newbuf == buffer)
	{
		/*
		 * 由于新元组要进入同一页，我们也许可以
		 * 做 HOT 更新。检查是否有任何索引列被
		 * 改过。
		 */
		if (!bms_overlap(modified_attrs, hot_attrs))
		{
			use_hot_update = true;

			/*
			 * 若用于阻碍 HOT 的索引中的列都没有被
			 * 更新，就可以使用 HOT，但仍需要检查
			 * 是否需要更新汇总索引；若那些
			 * 列被更新了就要更新这些索引，否则可能无法发现
			 * 例如 BRIN minmax 索引中的值界变化。
			 */
			if (bms_overlap(modified_attrs, sum_attrs))
				summarized_update = true;
		}
	}
	else
	{
		/* 设置提示：旧页可以做剪枝/碎片整理 */
		PageSetFull(page);
	}

	/*
	 * 进入临界区之前先计算副本标识元组，这样
	 * 内存分配失败时就不会 PANIC。
	 * 若没有需要记录的内容，ExtractReplicaIdentity() 会返回 NULL。
	 * 只有副本标识键列被修改或含有外部数据时，
	 * 才把 old key required 传为 true。
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &oldtup,
										   bms_overlap(modified_attrs, id_attrs) ||
										   id_has_external,
										   &old_key_copied);

	/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	/*
	 * 若本事务提交，旧元组迟早会变成 DEAD。
	 * 设置标志，表明一旦我们的 xid 落到 OldestXmin 视界之下，
	 * 此页就是剪枝候选。若事务最终中止，
	 * 随后的页面剪枝将是空操作，该提示也会被
	 * 清除。
	 *
	 * XXX 是否也应当在 newbuf 上设置提示？若事务中止，
	 * newbuf 里会有一个可剪枝的元组；但目前我们选择
	 * 不为中止做优化。注意若这个决定改变，heap_xlog_update 必须保持
	 * 同步。
	 */
	PageSetPrunable(page, xid);

	if (use_hot_update)
	{
		/* 把旧元组标为 HOT 更新过 */
		HeapTupleSetHotUpdated(&oldtup);
		/* 并把新元组标为堆内元组 */
		HeapTupleSetHeapOnly(heaptup);
		/* 若与 heaptup 不同，也标记调用方的副本 */
		HeapTupleSetHeapOnly(newtup);
	}
	else
	{
		/* 确保元组被正确标为非 HOT */
		HeapTupleClearHotUpdated(&oldtup);
		HeapTupleClearHeapOnly(heaptup);
		HeapTupleClearHeapOnly(newtup);
	}

	RelationPutHeapTuple(relation, newbuf, heaptup, false); /* 插入新元组 */


	/* 清除过时的可见性标志，可能是我们自己在上面设置的…… */
	oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	/* ……并存储更新该元组的事务信息 */
	Assert(TransactionIdIsValid(xmax_old_tuple));
	HeapTupleHeaderSetXmax(oldtup.t_data, xmax_old_tuple);
	oldtup.t_data->t_infomask |= infomask_old_tuple;
	oldtup.t_data->t_infomask2 |= infomask2_old_tuple;
	HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

	/* 在旧元组的 t_ctid 中记录新元组的地址 */
	oldtup.t_data->t_ctid = heaptup->t_self;

	/* 清除 PD_ALL_VISIBLE 标志，重置可见性映射的所有位 */
	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer, VISIBILITYMAP_VALID_BITS);
	}
	if (newbuf != buffer && PageIsAllVisible(BufferGetPage(newbuf)))
	{
		all_visible_cleared_new = true;
		PageClearAllVisible(BufferGetPage(newbuf));
		visibilitymap_clear(relation, BufferGetBlockNumber(newbuf),
							vmbuffer_new, VISIBILITYMAP_VALID_BITS);
	}

	if (newbuf != buffer)
		MarkBufferDirty(newbuf);
	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		/*
		 * 为了逻辑解码，我们需要组合 CID 才能正确解码
		 * 系统目录。
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
		{
			log_heap_new_cid(relation, &oldtup);
			log_heap_new_cid(relation, heaptup);
		}

		recptr = log_heap_update(relation, buffer,
								 newbuf, &oldtup, heaptup,
								 old_key_tuple,
								 all_visible_cleared,
								 all_visible_cleared_new);
		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
	}

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * 标记旧元组，以便在下一个命令边界从系统缓存中失效，
	 * 并标记新元组，以便在我们中止时失效。我们必须
	 * 在释放缓冲区之前做这件事，因为 oldtup 在
	 * 缓冲区里。（heaptup 全在本地内存中，但必须在一次 inval.c 调用里处理
	 * 两个元组版本，才能避免多余的
	 * sinval 消息。）
	 */
	CacheInvalidateHeapTuple(relation, &oldtup, heaptup);

	/* 现在可以释放缓冲区了 */
	if (newbuf != buffer)
		ReleaseBuffer(newbuf);
	ReleaseBuffer(buffer);
	if (BufferIsValid(vmbuffer_new))
		ReleaseBuffer(vmbuffer_new);
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * 若持有过，则释放 lmgr 元组锁。
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);

	pgstat_count_heap_update(relation, use_hot_update, newbuf != buffer);

	/*
	 * 若 heaptup 是私有副本，则释放它。别忘了把 t_self
	 * 也复制回调用方的映像。
	 */
	if (heaptup != newtup)
	{
		newtup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	/*
	 * 若这是 HOT 更新，这次更新可能仍需要更新汇总
	 * 索引，否则我们会漏掉那些汇总并得到不正确的
	 * 结果（例如，这一块的 minmax 界可能因这次
	 * 更新而改变）。
	 */
	if (use_hot_update)
	{
		if (summarized_update)
			*update_indexes = TU_Summarizing;
		else
			*update_indexes = TU_None;
	}
	else
		*update_indexes = TU_All;

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	bms_free(hot_attrs);
	bms_free(sum_attrs);
	bms_free(key_attrs);
	bms_free(id_attrs);
	bms_free(modified_attrs);
	bms_free(interesting_attrs);

	return TM_Ok;
}

#ifdef USE_ASSERT_CHECKING
/*
 * 按 README.tuplock 中"写入原地更新表时的锁定"一节的规则，
 * 确认 heap_update() 期间持有足够的锁。
 */
static void
check_lock_if_inplace_updateable_rel(Relation relation,
									 ItemPointer otid,
									 HeapTuple newtup)
{
	/* 对任何系统目录，LOCKTAG_TUPLE 都可以 */
	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
		case DatabaseRelationId:
			{
				LOCKTAG		tuptag;

				SET_LOCKTAG_TUPLE(tuptag,
								  relation->rd_lockInfo.lockRelId.dbId,
								  relation->rd_lockInfo.lockRelId.relId,
								  ItemPointerGetBlockNumber(otid),
								  ItemPointerGetOffsetNumber(otid));
				if (LockHeldByMe(&tuptag, InplaceUpdateTupleLock))
					return;
			}
			break;
		default:
			Assert(!IsInplaceUpdateRelation(relation));
			return;
	}

	switch (RelationGetRelid(relation))
	{
		case RelationRelationId:
			{
				/* LOCKTAG_TUPLE 或 LOCKTAG_RELATION 都可以 */
				Form_pg_class classForm = (Form_pg_class) GETSTRUCT(newtup);
				Oid			relid = classForm->oid;
				Oid			dbid;
				LOCKTAG		tag;

				if (IsSharedRelation(relid))
					dbid = InvalidOid;
				else
					dbid = MyDatabaseId;

				if (classForm->relkind == RELKIND_INDEX)
				{
					Relation	irel = index_open(relid, AccessShareLock);

					SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
					index_close(irel, AccessShareLock);
				}
				else
					SET_LOCKTAG_RELATION(tag, dbid, relid);

				if (!LockHeldByMe(&tag, ShareUpdateExclusiveLock) &&
					!LockOrStrongerHeldByMe(&tag, ShareRowExclusiveLock))
					elog(WARNING,
						 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
						 NameStr(classForm->relname),
						 relid,
						 classForm->relkind,
						 ItemPointerGetBlockNumber(otid),
						 ItemPointerGetOffsetNumber(otid));
			}
			break;
		case DatabaseRelationId:
			{
				/* 需要 LOCKTAG_TUPLE */
				Form_pg_database dbForm = (Form_pg_database) GETSTRUCT(newtup);

				elog(WARNING,
					 "missing lock on database \"%s\" (OID %u) @ TID (%u,%u)",
					 NameStr(dbForm->datname),
					 dbForm->oid,
					 ItemPointerGetBlockNumber(otid),
					 ItemPointerGetOffsetNumber(otid));
			}
			break;
	}
}

/*
 * 按 README.tuplock 中"写入原地更新表时的锁定"一节的规则，
 * 确认持有足够的关系锁。
 */
static void
check_inplace_rel_lock(HeapTuple oldtup)
{
	Form_pg_class classForm = (Form_pg_class) GETSTRUCT(oldtup);
	Oid			relid = classForm->oid;
	Oid			dbid;
	LOCKTAG		tag;

	if (IsSharedRelation(relid))
		dbid = InvalidOid;
	else
		dbid = MyDatabaseId;

	if (classForm->relkind == RELKIND_INDEX)
	{
		Relation	irel = index_open(relid, AccessShareLock);

		SET_LOCKTAG_RELATION(tag, dbid, irel->rd_index->indrelid);
		index_close(irel, AccessShareLock);
	}
	else
		SET_LOCKTAG_RELATION(tag, dbid, relid);

	if (!LockOrStrongerHeldByMe(&tag, ShareUpdateExclusiveLock))
		elog(WARNING,
			 "missing lock for relation \"%s\" (OID %u, relkind %c) @ TID (%u,%u)",
			 NameStr(classForm->relname),
			 relid,
			 classForm->relkind,
			 ItemPointerGetBlockNumber(&oldtup->t_self),
			 ItemPointerGetOffsetNumber(&oldtup->t_self));
}
#endif

/*
 * 检查指定属性的值是否相同。HeapDetermineColumnsInfo 的
 * 子例程。
 */
static bool
heap_attr_equals(TupleDesc tupdesc, int attrnum, Datum value1, Datum value2,
				 bool isnull1, bool isnull2)
{
	Form_pg_attribute att;

	/*
	 * 若一个值是 NULL 而另一个不是，则它们肯定不
	 * 相等
	 */
	if (isnull1 != isnull2)
		return false;

	/*
	 * 若两者都是 NULL，可以认为相等。
	 */
	if (isnull1)
		return true;

	/*
	 * 我们对两个 datum 做简单的二进制比较。
	 * 这可能过严，因为同一逻辑值
	 * 可以有多种二进制表示。
	 * 但只要没有假阳性，就应当没问题。使用特定类型的相等
	 * 操作符很麻烦，因为不同的操作符
	 * 类里可能有多种相等概念；此外，持有排他缓冲区锁时，
	 * 我们无法安全地调用用户定义函数。
	 */
	if (attrnum <= 0)
	{
		/* 唯一允许的系统列是 OID，因此这样做 */
		return (DatumGetObjectId(value1) == DatumGetObjectId(value2));
	}
	else
	{
		Assert(attrnum <= tupdesc->natts);
		att = TupleDescAttr(tupdesc, attrnum - 1);
		return datumIsEqual(value1, value2, att->attbyval, att->attlen);
	}
}

/*
 * 检查哪些列正在被更新。
 *
 * 给定一个更新后的元组，在那些列为感兴趣的列中，确定（并返回到输出位图集合）
 * 哪些列发生了变化。
 *
 * has_external 表示旧元组中未被修改的属性（来自那些
 * 列为感兴趣的属性）是否属于 external_cols，并且
 * 存储在外部。
 */
static Bitmapset *
HeapDetermineColumnsInfo(Relation relation,
						 Bitmapset *interesting_cols,
						 Bitmapset *external_cols,
						 HeapTuple oldtup, HeapTuple newtup,
						 bool *has_external)
{
	int			attidx;
	Bitmapset  *modified = NULL;
	TupleDesc	tupdesc = RelationGetDescr(relation);

	attidx = -1;
	while ((attidx = bms_next_member(interesting_cols, attidx)) >= 0)
	{
		/* attidx 从零起计，attrnum 是普通的属性号 */
		AttrNumber	attrnum = attidx + FirstLowInvalidHeapAttributeNumber;
		Datum		value1,
					value2;
		bool		isnull1,
					isnull2;

		/*
		 * 若它是整元组引用，就说"不相等"。这种情形不太
		 * 值得支持，因为它只有在空操作更新之后才可能成功，
		 * 而这几乎不值得优化。
		 */
		if (attrnum == 0)
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * 同样，对 tableOID 以外的任何系统属性自动说"不相等"；
		 * 我们无法预期它们在 HOT 链中保持一致，
		 * 甚至无法预期新元组里已经正确设置了它们。
		 */
		if (attrnum < 0)
		{
			if (attrnum != TableOidAttributeNumber)
			{
				modified = bms_add_member(modified, attidx);
				continue;
			}
		}

		/*
		 * 取出对应的值。XXX 若有许多索引列，这相当低效。
		 * 是否应当改为对每个元组调用一次
		 * heap_deform_tuple？但那对系统列并不
		 * 适用……
		 */
		value1 = heap_getattr(oldtup, attrnum, tupdesc, &isnull1);
		value2 = heap_getattr(newtup, attrnum, tupdesc, &isnull2);

		if (!heap_attr_equals(tupdesc, attrnum, value1,
							  value2, isnull1, isnull2))
		{
			modified = bms_add_member(modified, attidx);
			continue;
		}

		/*
		 * 不必检查无法外部存储的属性。注意
		 * 系统属性不能外部存储。
		 */
		if (attrnum < 0 || isnull1 ||
			TupleDescAttr(tupdesc, attrnum - 1)->attlen != -1)
			continue;

		/*
		 * 检查旧元组的该属性是否外部存储，并且是
		 * external_cols 的成员。
		 */
		if (VARATT_IS_EXTERNAL((struct varlena *) DatumGetPointer(value1)) &&
			bms_is_member(attidx, external_cols))
			*has_external = true;
	}

	return modified;
}

/*
 *	simple_heap_update - 替换一个元组
 *
 * 当不预期目标元组会有并发更新时
 * （例如因为我们持有与该元组关联的关系上的锁），
 * 可以用本例程更新元组。任何失败都通过
 * ereport() 报告。
 */
void
simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup,
				   TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TM_FailureData tmfd;
	LockTupleMode lockmode;

	result = heap_update(relation, otid, tup,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* 等待提交 */ ,
						 &tmfd, &lockmode, update_indexes);
	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中被更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized heap_update status: %u", result);
			break;
	}
}


/*
 * 返回与给定元组锁模式对应的 MultiXactStatus。
 */
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update)
{
	int			retval;

	if (is_update)
		retval = tupleLockExtraInfo[mode].updstatus;
	else
		retval = tupleLockExtraInfo[mode].lockstatus;

	if (retval == -1)
		elog(ERROR, "invalid lock tuple mode %d/%s", mode,
			 is_update ? "true" : "false");

	return (MultiXactStatus) retval;
}

/*
 *	heap_lock_tuple - 以共享或排他模式锁定一个元组
 *
 * 注意这会获取一个缓冲区钉，调用方必须释放它。
 *
 * 输入参数：
 *	relation：包含该元组的关系（调用方必须持有合适的锁）
 *	tid：要锁定的元组的 TID
 *	cid：当前命令 ID（用于可见性测试，若锁定成功则写入
 *		元组的 cmax）
 *	mode：表示希望共享还是排他元组锁
 *	wait_policy：元组锁不可用时怎么做
 *	follow_updates：若为 true，则跟随更新链，把后代
 *		元组也锁上。
 *
 * 输出参数：
 *	*tuple：所有字段都已填好
 *	*buffer：设为持有该元组的缓冲区（退出时已钉住但未上锁）
 *	*tmfd：失败时填入（见下）
 *
 * 函数结果与 table_tuple_lock() 的结果相同。
 *
 * 除 TM_Invisible 以外的失败情形中，本例程用
 * 该元组的 t_ctid、t_xmax（必要时解析可能的 MultiXact）
 * 和 t_cmax 填好 *tmfd（后者仅用于 TM_SelfModified，
 * 因为我们无法从另一个事务生成的组合 CID
 * 取得 cmax）。
 * 更多信息见结构 TM_FailureData 的注释。
 *
 * 这一机制的详尽说明见 README.tuplock。
 */
TM_Result
heap_lock_tuple(Relation relation, HeapTuple tuple,
				CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
				bool follow_updates,
				Buffer *buffer, TM_FailureData *tmfd)
{
	TM_Result	result;
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Page		page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;
	TransactionId xid,
				xmax;
	uint16		old_infomask,
				new_infomask,
				new_infomask2;
	bool		first_time = true;
	bool		skip_tuple_lock = false;
	bool		have_tuple_lock = false;
	bool		cleared_all_frozen = false;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	block = ItemPointerGetBlockNumber(tid);

	/*
	 * 锁定缓冲区之前，若看起来有必要，先钉住可见性映射页。
	 * 由于我们还没拿到锁，别人可能正在
	 * 改它，所以拿到锁之后
	 * 还需要再检查一次。
	 */
	if (PageIsAllVisible(BufferGetPage(*buffer)))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buffer);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple, cid, *buffer);

	if (result == TM_Invisible)
	{
		/*
		 * 这是可能的，但只在为 ON CONFLICT
		 * UPDATE 锁定元组时。我们在这里返回这个值，而不是抛出错误，
		 * 以便那种情形有机会抛出更具体的
		 * 错误。
		 */
		result = TM_Invisible;
		goto out_locked;
	}
	else if (result == TM_BeingModified ||
			 result == TM_Updated ||
			 result == TM_Deleted)
	{
		TransactionId xwait;
		uint16		infomask;
		uint16		infomask2;
		bool		require_sleep;
		ItemPointerData t_ctid;

		/* 解锁缓冲区之前必须复制状态数据 */
		xwait = HeapTupleHeaderGetRawXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;
		infomask2 = tuple->t_data->t_infomask2;
		ItemPointerCopy(&tuple->t_data->t_ctid, &t_ctid);

		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 若当前顶层事务的任何子事务已经持有
		 * 与我们请求的一样强或更强的锁，我们
		 * 实际上已经持有了想要的锁。我们*必须*成功，
		 * 而不能去获取元组锁，否则会与任何想获取更强锁的人
		 * 死锁。
		 *
		 * 注意我们只在第一次对 HTSU 结果循环时这样做；
		 * 后续轮次再测没有意义，因为
		 * 显然我们自己的事务不可能在第一次检查之后
		 * 又取得一把新锁。
		 */
		if (first_time)
		{
			first_time = false;

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				int			i;
				int			nmembers;
				MultiXactMember *members;

				/*
				 * 这里不需要允许旧的 multixact；若是那种
				 * 情况，HeapTupleSatisfiesUpdate 会返回
				 * MayBeUpdated，我们就不会到这里。
				 */
				nmembers =
					GetMultiXactIdMembers(xwait, &members, false,
										  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

				for (i = 0; i < nmembers; i++)
				{
					/* 只考虑我们自己事务的成员 */
					if (!TransactionIdIsCurrentTransactionId(members[i].xid))
						continue;

					if (TUPLOCK_from_mxstatus(members[i].status) >= mode)
					{
						pfree(members);
						result = TM_Ok;
						goto out_unlocked;
					}
					else
					{
						/*
						 * 禁止获取重量级元组锁。
						 * 否则，在把较弱的锁提升时，我们可能与另一个
						 * 已取得重量级元组锁、正在等待我们的
						 * 事务结束的加锁者
						 * 发生死锁。
						 *
						 * 注意这种情况下，若有需要，我们仍须等待
						 * 该 multixact，以免取得
						 * 冲突的锁。
						 */
						skip_tuple_lock = true;
					}
				}

				if (members)
					pfree(members);
			}
			else if (TransactionIdIsCurrentTransactionId(xwait))
			{
				switch (mode)
				{
					case LockTupleKeyShare:
						Assert(HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_EXCL_LOCKED(infomask));
						result = TM_Ok;
						goto out_unlocked;
					case LockTupleShare:
						if (HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleNoKeyExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask))
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
					case LockTupleExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask) &&
							infomask2 & HEAP_KEYS_UPDATED)
						{
							result = TM_Ok;
							goto out_unlocked;
						}
						break;
				}
			}
		}

		/*
		 * 起初假定我们必须等待加锁
		 * 事务结束。下面检查各种可以把这一点
		 * 关掉的情形。
		 */
		require_sleep = true;
		if (mode == LockTupleKeyShare)
		{
			/*
			 * 若我们请求的是 KeyShare，并且没有更新，就
			 * 不需要等待。即使有更新，只要键没有被修改，
			 * 我们仍可以继续。
			 * 下面接着处理更新链。
			 *
			 * 不过，若有更新，需要沿着更新链走，
			 * 把该行的未来版本也标成已锁定。这样，
			 * 若有人删除那个未来版本，我们就能防止
			 * 键消失。对未来版本的这种锁定
			 * 可能短暂阻塞，若有并发事务正在
			 * 删除键；也可能返回一个值，表示
			 * 删除键的事务已经提交。因此我们
			 * 在重新锁定缓冲区之前做这件事；否则这里容易
			 * 死锁。
			 *
			 * 注意我们锁定的 TID 是在解锁缓冲区之前取得的。
			 * 要让它在我们没看的时候发生变化，下面重新锁定
			 * 缓冲区之后我们要测试的其他性质也会变化，那样我们就会在上面
			 * 重新开始这个循环。
			 */
			if (!(infomask2 & HEAP_KEYS_UPDATED))
			{
				bool		updated;

				updated = !HEAP_XMAX_IS_LOCKED_ONLY(infomask);

				/*
				 * 若有更新，就跟随更新链；若做不到就退出。
				 * 那是无法完成的。
				 */
				if (follow_updates && updated)
				{
					TM_Result	res;

					res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
												  GetCurrentTransactionId(),
												  mode);
					if (res != TM_Ok)
					{
						result = res;
						/* 恢复代码预期持有缓冲区锁 */
						LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
						goto failed;
					}
				}

				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 确认它仍然是合适的锁，否则重新开始。
				 * 另外，若释放锁之前它还没被更新，但
				 * 现在被更新了，我们也重新开始；原因是现在
				 * 需要跟随更新链去锁定新的
				 * 版本。
				 */
				if (!HeapTupleHeaderIsOnlyLocked(tuple->t_data) &&
					((tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED) ||
					 !updated))
					goto l3;

				/* 看起来没问题，因此可以跳过睡眠 */
				require_sleep = false;

				/*
				 * 注意这里允许 Xmax 改变；其他更新者/加锁者
				 * 可能在我们取得缓冲区锁之前改过它。
				 * 不过这不是问题，因为刚刚做的复查
				 * 保证它们仍然不与我们想要的
				 * 锁冲突。
				 */
			}
		}
		else if (mode == LockTupleShare)
		{
			/*
			 * 若我们请求的是 Share，同样可以避免睡眠，如果
			 * 没有更新也没有排他锁。
			 */
			if (HEAP_XMAX_IS_LOCKED_ONLY(infomask) &&
				!HEAP_XMAX_IS_EXCL_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * 确认它仍然是合适的锁，否则重新开始。
				 * 关于允许 xmax 改变，见上文。
				 */
				if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
					HEAP_XMAX_IS_EXCL_LOCKED(tuple->t_data->t_infomask))
					goto l3;
				require_sleep = false;
			}
		}
		else if (mode == LockTupleNoKeyExclusive)
		{
			/*
			 * 若我们请求的是 NoKeyExclusive，也许也能
			 * 避免睡眠；只要确保还没有取得冲突的
			 * 锁。
			 */
			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				if (!DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
											 mode, NULL))
				{
					/*
					 * 没有冲突，但若 xmax 在此期间于我们手下
					 * 改变了，则重新开始。
					 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
						!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
											 xwait))
						goto l3;

					/* 否则，我们没问题 */
					require_sleep = false;
				}
			}
			else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/* 若 xmax 在此期间改变了，则重新开始 */
				if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
										 xwait))
					goto l3;
				/* 否则，我们没问题 */
				require_sleep = false;
			}
		}

		/*
		 * 作为与上面那些检查相互独立的一项检查，
		 * 若当前事务是该元组的唯一加锁者，
		 * 我们也可以避免睡眠。注意
		 * 已经持有的锁有多强无关紧要；这并不是
		 * 关于把锁记入 Xmax（无论这项优化如何，下面都会做）。
		 * 另外，注意我们已经持有比所请求更强的锁的情形，
		 * 上面已经通过什么都不做处理过了。
		 *
		 * 注意这里只处理非 multixact 情形；MultiXactIdWait
		 * 自己就能很好地处理这种情况。
		 */
		if (require_sleep && !(infomask & HEAP_XMAX_IS_MULTI) &&
			TransactionIdIsCurrentTransactionId(xwait))
		{
			/* ……但若 xmax 在此期间改变了，则重新开始 */
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask));
			require_sleep = false;
		}

		/*
		 * 若有必要，该睡在另一个事务或 multixact 上了。
		 *
		 * 若另一个事务是已经提交的更新或删除，
		 * 那么睡眠不可能有任何好处：若我们被
		 * 要求睡眠，就退出去报错。
		 *
		 * 到这里，我们要么已经取得了缓冲区排他锁，
		 * 要么必须等待加锁事务或 multixact；因此下面
		 * 我们保证在睡眠之后再抓取缓冲区锁。
		 */
		if (require_sleep && (result == TM_Updated || result == TM_Deleted))
		{
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			goto failed;
		}
		else if (require_sleep)
		{
			/*
			 * 获取元组锁以确立我们对该元组的优先级，或者
			 * 在尝试中失败。当我们成为该元组的下一位时，LockTuple 会唤醒
			 * 我们。即使是共享锁定也必须这样做，
			 * 但若我们已经对该元组持有较弱的锁则不必。
			 *
			 * 若下面被迫"重新开始"，我们保留元组锁；
			 * 这样在重新检查
			 * 元组状态时，我们仍排在队列前面。
			 */
			if (!skip_tuple_lock &&
				!heap_acquire_tuplock(relation, tid, mode, wait_policy,
									  &have_tuple_lock))
			{
				/*
				 * 只有当 wait_policy 为 Skip 且锁
				 * 无法取得时才会发生这种情况。
				 */
				result = TM_WouldBlock;
				/* 恢复代码预期持有缓冲区锁 */
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
				goto failed;
			}

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactStatus status = get_mxact_status_for_lock(mode, false);

				/* 我们只锁定元组，从不更新它们 */
				if (status >= MultiXactStatusNoKeyUpdate)
					elog(ERROR, "invalid lock mode in heap_lock_tuple");

				/* 等待 multixact 结束，或者在尝试中失败 */
				switch (wait_policy)
				{
					case LockWaitBlock:
						MultiXactIdWait((MultiXactId) xwait, status, infomask,
										relation, &tuple->t_self, XLTW_Lock, NULL);
						break;
					case LockWaitSkip:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL))
						{
							result = TM_WouldBlock;
							/* 恢复代码预期持有缓冲区锁 */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
														status, infomask, relation,
														NULL))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));

						break;
				}

				/*
				 * 当然，这里 multixact 未必已经结束：若我们
				 * 请求的是轻量锁模式，其他持有轻量
				 * 锁的事务可能仍活着，我们自己的事务或本后端其他子事务
				 * 拥有的锁也是如此。我们需要
				 * 保留存活的 MultiXact 成员。注意后一种情况
				 * 并非绝对必要，但这样做
				 * 更简单。
				 */
			}
			else
			{
				/* 等待普通事务结束，或者在尝试中失败 */
				switch (wait_policy)
				{
					case LockWaitBlock:
						XactLockTableWait(xwait, relation, &tuple->t_self,
										  XLTW_Lock);
						break;
					case LockWaitSkip:
						if (!ConditionalXactLockTableWait(xwait))
						{
							result = TM_WouldBlock;
							/* 恢复代码预期持有缓冲区锁 */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalXactLockTableWait(xwait))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
											RelationGetRelationName(relation))));
						break;
				}
			}

			/* 若有更新，则跟随更新链 */
			if (follow_updates && !HEAP_XMAX_IS_LOCKED_ONLY(infomask))
			{
				TM_Result	res;

				res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
											  GetCurrentTransactionId(),
											  mode);
				if (res != TM_Ok)
				{
					result = res;
					/* 恢复代码预期持有缓冲区锁 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					goto failed;
				}
			}

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait 已经结束，但若 xwait 刚刚锁过该元组，那么别的
			 * 事务可能在我们到达这里之前更新了它。
			 * 检查 xmax 是否改变，若改变则重新开始。
			 */
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;

			if (!(infomask & HEAP_XMAX_IS_MULTI))
			{
				/*
				 * 否则检查它是已提交还是已中止。注意我们无法
				 * 在这里出现，如果该元组只是被某个与我们不
				 * 冲突的人锁过；那种情况上面已经处理了。因此
				 * 那个事务现在必然已经结束。但
				 * 在 multixact 情形下不要检查这一点，因为有些
				 * 加锁事务可能仍在运行。
				 */
				UpdateXmaxHintBits(tuple->t_data, *buffer, xwait);
			}
		}

		/* 到这里，我们确定再次持有缓冲区排他锁 */

		/*
		 * 若先前的 xmax 已中止，或者它已提交但只
		 * 锁了元组而没有更新它，或者无论什么原因我们根本不必等待，
		 * 我们都可以加锁。
		 */
		if (!require_sleep ||
			(tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tuple->t_data))
			result = TM_Ok;
		else if (!ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
			result = TM_Updated;
		else
			result = TM_Deleted;
	}

failed:
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified || result == TM_Updated ||
			   result == TM_Deleted || result == TM_WouldBlock);

		/*
		 * 在 LockWaitSkip 语义下锁定元组、并且上面以
		 * TM_WouldBlock 失败时，并发事务有可能在此期间
		 * 释放锁并设置 HEAP_XMAX_INVALID。因此
		 * 这个断言与 heap_delete 和 heap_update 中
		 * 对应的断言略有不同。
		 */
		Assert((result == TM_WouldBlock) ||
			   !(tuple->t_data->t_infomask & HEAP_XMAX_INVALID));
		Assert(result != TM_Updated ||
			   !ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid));
		tmfd->ctid = tuple->t_data->t_ctid;
		tmfd->xmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
		if (result == TM_SelfModified)
			tmfd->cmax = HeapTupleHeaderGetCmax(tuple->t_data);
		else
			tmfd->cmax = InvalidCommandId;
		goto out_locked;
	}

	/*
	 * 若我们没有钉住可见性映射页，而该页在我们忙着锁缓冲区时变成了全可见，
	 * 或在随后某个我们把它解锁的窗口期间，
	 * 也变成了全可见，
	 * 就得解锁再重新加锁，以免跨 I/O 持有缓冲区锁。这有点
	 * 不幸，尤其是现在还得重新检查
	 * 该元组是否已在我们手下被锁定或更新，但希望这种事
	 * 不会经常发生。
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l3;
	}

	xmax = HeapTupleHeaderGetRawXmax(tuple->t_data);
	old_infomask = tuple->t_data->t_infomask;

	/*
	 * 若这是当前事务中第一次可能涉及 multixact 的操作，
	 * 就设置本后端的 OldestMemberMXactId。我们可以
	 * 确定该事务绝不会成为任何比它更老的
	 * MultiXactId 的成员。（即使下面最终只用
	 * 我们自己的 TransactionId，也必须这样做，因为别的后端可能
	 * 随即把我们的 XID 并入一个 MultiXact。）
	 */
	MultiXactIdSetOldestMember();

	/*
	 * 计算要存入元组的新 xmax 和 infomask。注意我们
	 * 先还不修改元组，因为若 multixact.c 记一条错误日志，
	 * 那样会让它处于错误状态。
	 */
	compute_new_xmax_infomask(xmax, old_infomask, tuple->t_data->t_infomask2,
							  GetCurrentTransactionId(), mode, false,
							  &xid, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * 存储锁定该元组的事务信息。
	 *
	 * 注意：在此上下文中 Cmax 没有意义，所以不要设置它；这避免
	 * 可能生成一个无用的组合 CID。而且，若我们锁定的是
	 * 先前已更新的元组，保留 Cmax 很重要。
	 *
	 * 也重置 HOT UPDATE 位，但仅当没有更新时；否则
	 * 会弄断 HOT 链。
	 */
	tuple->t_data->t_infomask &= ~HEAP_XMAX_BITS;
	tuple->t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tuple->t_data->t_infomask |= new_infomask;
	tuple->t_data->t_infomask2 |= new_infomask2;
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		HeapTupleHeaderClearHotUpdated(tuple->t_data);
	HeapTupleHeaderSetXmax(tuple->t_data, xid);

	/*
	 * 确保 t_ctid 中没有向前的链链接。注意在
	 * 元组已被更新的情形下，不得覆盖 t_ctid，
	 * 因为它是由更新者设置的。而且，若元组已被
	 * 更新，还需要跟随更新链，把该元组的新版本
	 * 也锁上。
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		tuple->t_data->t_ctid = *tid;

	/* 若需要，只清除可见性映射上的全冻结位 */
	if (PageIsAllVisible(page) &&
		visibilitymap_clear(relation, block, vmbuffer,
							VISIBILITYMAP_ALL_FROZEN))
		cleared_all_frozen = true;


	MarkBufferDirty(*buffer);

	/*
	 * XLOG 相关。你也许以为不需要 XLOG 记录，因为
	 * 没有值得在崩溃后恢复的状态变化。但你
	 * 会错：我们刚刚写入的 TransactionId 或
	 * MultiXactId 可能以前从未在磁盘上出现过，而我们需要
	 * 确保有覆盖这些 ID 号的 XLOG 条目。
	 * 否则崩溃后同样的 ID 可能被重用，若此页在崩溃前
	 * 写到了磁盘，那将是灾难性的。本质上
	 * 即使在这种情况下我们也必须强制 WAL 的先日志后数据规则。
	 * （另外，在 PITR 日志传送或 2PC 环境中，反正所有东西都要有 XLOG
	 * 条目。）
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_lock xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD);

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.xmax = xid;
		xlrec.infobits_set = compute_infobits(new_infomask,
											  tuple->t_data->t_infomask2);
		xlrec.flags = cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;
		XLogRegisterData((char *) &xlrec, SizeOfHeapLock);

		/* 目前我们不解码行锁，因此不必记录复制源 */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	result = TM_Ok;

out_locked:
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

out_unlocked:
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * 不要在这里更新可见性映射。锁定元组不会改变
	 * 可见性信息。
	 */

	/*
	 * 既然已经成功把元组标成已锁定，就可以
	 * 释放 lmgr 元组锁，若我们持有过它。
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, tid, mode);

	return result;
}

/*
 * 在给定元组上获取重量级锁，为获取
 * 它常规的、基于 Xmax 的元组锁做准备。
 *
 * have_tuple_lock 既是输入也是输出参数：输入时，它表示
 * 锁是否先前已经取得（若是，本函数
 * 什么都不做）。若本函数返回成功，have_tuple_lock
 * 会被翻转为 true。
 *
 * 若无法取得锁则返回 false；这只在
 * wait_policy 为 Skip 时才会发生。
 */
static bool
heap_acquire_tuplock(Relation relation, ItemPointer tid, LockTupleMode mode,
					 LockWaitPolicy wait_policy, bool *have_tuple_lock)
{
	if (*have_tuple_lock)
		return true;

	switch (wait_policy)
	{
		case LockWaitBlock:
			LockTupleTuplock(relation, tid, mode);
			break;

		case LockWaitSkip:
			if (!ConditionalLockTupleTuplock(relation, tid, mode))
				return false;
			break;

		case LockWaitError:
			if (!ConditionalLockTupleTuplock(relation, tid, mode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(relation))));
			break;
	}
	*have_tuple_lock = true;

	return true;
}

/*
 * 给定一组原始的 Xmax 和 infomask，以及一个事务（由
 * add_to_xmax 标识）正在以某种模式获取新锁，计算要在元组上使用的新 Xmax 和
 * 对应的 infomask。
 *
 * 注意这可能有副作用，例如创建一个新的 MultiXactId。
 *
 * 大多数调用方在本函数之前会调用 HeapTupleSatisfiesUpdate；
 * 若 xmax 是 MultiXactId
 * 但已经不再运行，那会设置 HEAP_XMAX_INVALID 位。存在竞态条件，即
 * MultiXactId 可能在那之后已经结束，但这种不常见的情形会在
 * 这里或 MultiXactIdExpand 内部处理。
 *
 * 当旧 xmax 是普通
 * TransactionId 时，可能有类似的竞态条件。我们再次测试 TransactionIdIsInProgress，只是为了收窄
 * 窗口，但仍可能最终创建一个不必要的
 * MultiXactId。幸好这是无害的。
 */
static void
compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
						  uint16 old_infomask2, TransactionId add_to_xmax,
						  LockTupleMode mode, bool is_update,
						  TransactionId *result_xmax, uint16 *result_infomask,
						  uint16 *result_infomask2)
{
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;

	Assert(TransactionIdIsCurrentTransactionId(add_to_xmax));

l5:
	new_infomask = 0;
	new_infomask2 = 0;
	if (old_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * 没有先前的加锁者；我们只插入自己的 TransactionId。
		 *
		 * 注意这个情形必须最先检查，这一点至关重要，
		 * 因为下面有几段会回到这里
		 * 来实现某些优化；那些情形里 old_infomask 可能含有
		 * 其他脏位，但我们其实并不在意。
		 */
		if (is_update)
		{
			new_xmax = add_to_xmax;
			if (mode == LockTupleExclusive)
				new_infomask2 |= HEAP_KEYS_UPDATED;
		}
		else
		{
			new_infomask |= HEAP_XMAX_LOCK_ONLY;
			switch (mode)
			{
				case LockTupleKeyShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_KEYSHR_LOCK;
					break;
				case LockTupleShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_SHR_LOCK;
					break;
				case LockTupleNoKeyExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					break;
				case LockTupleExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					new_infomask2 |= HEAP_KEYS_UPDATED;
					break;
				default:
					new_xmax = InvalidTransactionId;	/* 让编译器安静 */
					elog(ERROR, "invalid lock mode");
			}
		}
	}
	else if (old_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactStatus new_status;

		/*
		 * 目前不允许为 multi 设置 XMAX_COMMITTED，因此
		 * 交叉检查。
		 */
		Assert(!(old_infomask & HEAP_XMAX_COMMITTED));

		/*
		 * 一个 multi 同时设置了 LOCK_ONLY，但两个锁位都没设
		 * （即 pg_upgrade 之后的共享锁定元组）不可能还在
		 * 运行。对经 pg_upgrade 升级的数据库，这项检查至关重要；
		 * MultiXactIdIsRunning 和 MultiXactIdExpand 都假定
		 * 绝不会传入这样的 multi。
		 */
		if (HEAP_LOCKED_UPGRADED(old_infomask))
		{
			old_infomask &= ~HEAP_XMAX_IS_MULTI;
			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/*
		 * 若 XMAX 已经是 MultiXactId，就需要扩展它以
		 * 包含 add_to_xmax；但若所有成员都是加锁者并且都
		 * 已经离开，就可以去掉 IS_MULTI 位，只把
		 * add_to_xmax 设为唯一的加锁者/更新者。若所有加锁者都已离开，
		 * 而我们有一个已中止的更新者，也可以不要
		 * multi。
		 *
		 * 做 GetMultiXactIdMembers 的代价反正会由
		 * MultiXactIdExpand 支付，若我们不做这次检查的话，因此这次检查并没有
		 * 增加额外工作。
		 */
		if (!MultiXactIdIsRunning(xmax, HEAP_XMAX_IS_LOCKED_ONLY(old_infomask)))
		{
			if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) ||
				!TransactionIdDidCommit(MultiXactIdGetUpdateXid(xmax,
																old_infomask)))
			{
				/*
				 * 重置这些位并重新开始；否则就继续向下，
				 * 在下面创建一个新的 multi。
				 */
				old_infomask &= ~HEAP_XMAX_IS_MULTI;
				old_infomask |= HEAP_XMAX_INVALID;
				goto l5;
			}
		}

		new_status = get_mxact_status_for_lock(mode, is_update);

		new_xmax = MultiXactIdExpand((MultiXactId) xmax, add_to_xmax,
									 new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (old_infomask & HEAP_XMAX_COMMITTED)
	{
		/*
		 * 这是已提交的更新，因此需要把他保留为该元组的
		 * 更新者。
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * 既然它不在运行，旧的
		 * 更新者显然不可能与当前这个相同，因此不必像上面那段那样检查
		 * 那种情形。
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (TransactionIdIsInProgress(xmax))
	{
		/*
		 * 若 XMAX 是有效且正在进行的 TransactionId，就需要
		 * 创建一个新的 MultiXactId，同时包含旧的加锁者或
		 * 更新者以及我们自己的 TransactionId。
		 */
		MultiXactStatus new_status;
		MultiXactStatus old_status;
		LockTupleMode old_mode;

		if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
		{
			if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForKeyShare;
			else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForShare;
			else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
			{
				if (old_infomask2 & HEAP_KEYS_UPDATED)
					old_status = MultiXactStatusForUpdate;
				else
					old_status = MultiXactStatusForNoKeyUpdate;
			}
			else
			{
				/*
				 * 只有当页被
				 * pg_upgrade 升级过时，LOCK_ONLY 才可能单独出现。但那种情况下，
				 * TransactionIdIsInProgress() 应当已经返回 false。我们
				 * 假定这种情况下它不再被锁定。
				 */
				elog(WARNING, "LOCK_ONLY found for Xid in progress %u", xmax);
				old_infomask |= HEAP_XMAX_INVALID;
				old_infomask &= ~HEAP_XMAX_LOCK_ONLY;
				goto l5;
			}
		}
		else
		{
			/* 这是一次更新，但是哪一种？ */
			if (old_infomask2 & HEAP_KEYS_UPDATED)
				old_status = MultiXactStatusUpdate;
			else
				old_status = MultiXactStatusNoKeyUpdate;
		}

		old_mode = TUPLOCK_from_mxstatus(old_status);

		/*
		 * 若要获取的锁与已有锁属于同一个 TransactionId，
		 * 可以做一项优化：只把两者中
		 * 更强的那把锁当作唯一存在的锁，然后重新开始。
		 */
		if (xmax == add_to_xmax)
		{
			/*
			 * 注意原元组不可能已被
			 * 更新：否则我们不会到这里，因为元组会是
			 * 不可见的，我们也不会尝试更新它。有一个细微之处，
			 * 这段代码在沿更新链去锁定
			 * 元组的未来版本时也会运行。但那样我们也不会到这里，
			 * 因为 add_to_xmax 会与原来的
			 * 更新者不同。
			 */
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));

			/* 取得两者中更强的那把 */
			if (mode < old_mode)
				mode = old_mode;
			/* 不得触碰 is_update */

			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/* 否则，退回去创建一个新的 multixact */
		new_status = get_mxact_status_for_lock(mode, is_update);
		new_xmax = MultiXactIdCreate(xmax, old_status,
									 add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (!HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) &&
			 TransactionIdDidCommit(xmax))
	{
		/*
		 * 这是已提交的更新，因此必须把他保留为该元组的
		 * 更新者。
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * 既然它不在运行，旧的
		 * 更新者显然不可能与当前这个相同，因此不必像上面那段那样检查
		 * 那种情形。
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else
	{
		/*
		 * 能到这里，当且仅当加锁/更新事务在
		 * 从元组取出 infomask 时还在运行，但在
		 * TransactionIdIsInProgress 跑起来之前已经结束。当作一开始
		 * 根本没有加锁者来处理。
		 */
		old_infomask |= HEAP_XMAX_INVALID;
		goto l5;
	}

	*result_infomask = new_infomask;
	*result_infomask2 = new_infomask2;
	*result_xmax = new_xmax;
}

/*
 * heap_lock_updated_tuple_rec 的子例程。
 *
 * 给定由给定 xid 标识的事务所持有的一个假想 multixact 状态，
 * 若当前事务想获取给定模式的锁，它需要等待、失败，还是可以
 * 继续？"needwait"
 * 在必须等待时设为 true；若可以继续，则返回
 * TM_Ok。若当前事务已经持有该锁，返回
 * TM_SelfModified。若与另一个事务冲突，则返回
 * 另一个 HeapTupleSatisfiesUpdate 返回码。
 *
 * 所谓假想的持有状态，是因为它可能对应
 * 单个 Xid 持有的锁，即不是真正的 MultiXactId；为了 API 简单，我们这样
 * 表达。
 */
static TM_Result
test_lockmode_for_conflict(MultiXactStatus status, TransactionId xid,
						   LockTupleMode mode, HeapTuple tup,
						   bool *needwait)
{
	MultiXactStatus wantedstatus;

	*needwait = false;
	wantedstatus = get_mxact_status_for_lock(mode, false);

	/*
	 * 注意：我们必须先检查 TransactionIdIsInProgress，再检查
	 * TransactionIdDidAbort/Commit；解释见 heapam_visibility.c
	 * 顶部的注释。
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		/*
		 * 该元组已经被我们自己的事务锁过。这非常
		 * 少见，但当多个事务试图
		 * 锁定同一元组的古老版本时可能发生。
		 */
		return TM_SelfModified;
	}
	else if (TransactionIdIsInProgress(xid))
	{
		/*
		 * 若加锁事务正在运行，怎么做取决于
		 * 锁模式是否冲突：若冲突，就必须等它
		 * 结束；否则可以直接继续，锁定这个元组
		 * 版本而不等待。
		 */
		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			*needwait = true;
		}

		/*
		 * 若上面设置了 needwait，则这个值无关紧要；
		 * 否则，这个值告诉调用方可以继续。
		 */
		return TM_Ok;
	}
	else if (TransactionIdDidAbort(xid))
		return TM_Ok;
	else if (TransactionIdDidCommit(xid))
	{
		/*
		 * 另一个事务已提交。若它只是加锁者，那么
		 * 锁现在已经完全消失，我们可以返回成功；但若它
		 * 是一次更新，则怎么做取决于两种锁
		 * 模式是否冲突。若冲突，就必须向
		 * 调用方报告错误。但若不冲突，就可以继续向下，允许当前
		 * 事务锁定该元组。
		 *
		 * 注意：我们在这里担心 ISUPDATE 的原因是，事务一旦
		 * 结束，它的所有锁都消失且没有意义，
		 * 因此可以忽略；而它的更新会保留。在上面的
		 * TransactionIdIsInProgress 情形中，我们不必检查，
		 * 因为我们知道锁仍然"活着"，因此冲突必须
		 * 总是被检查。
		 */
		if (!ISUPDATE_from_mxstatus(status))
			return TM_Ok;

		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			/* 真倒霉 */
			if (!ItemPointerEquals(&tup->t_self, &tup->t_data->t_ctid))
				return TM_Updated;
			else
				return TM_Deleted;
		}

		return TM_Ok;
	}

	/* 不在进行、未中止、未提交——必定已经崩溃 */
	return TM_Ok;
}


/*
 * heap_lock_updated_tuple 的递归部分
 *
 * 取出 rel 中 tid 指向的元组，并把它标成被给定
 * xid 以给定模式锁定；若该元组已被更新，则递归去锁定新
 * 版本。
 */
static TM_Result
heap_lock_updated_tuple_rec(Relation rel, ItemPointer tid, TransactionId xid,
							LockTupleMode mode)
{
	TM_Result	result;
	ItemPointerData tupid;
	HeapTupleData mytup;
	Buffer		buf;
	uint16		new_infomask,
				new_infomask2,
				old_infomask,
				old_infomask2;
	TransactionId xmax,
				new_xmax;
	TransactionId priorXmax = InvalidTransactionId;
	bool		cleared_all_frozen = false;
	bool		pinned_desired_page;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber block;

	ItemPointerCopy(tid, &tupid);

	for (;;)
	{
		new_infomask = 0;
		new_xmax = InvalidTransactionId;
		block = ItemPointerGetBlockNumber(&tupid);
		ItemPointerCopy(&tupid, &(mytup.t_self));

		if (!heap_fetch(rel, SnapshotAny, &mytup, &buf, false))
		{
			/*
			 * 若找不到该元组的更新版本，那是
			 * 因为它在创建者事务中止之后
			 * 被清理或剪枝掉了。
			 * 因此表现得好像已经到达链的末端，
			 * 没有更多元组要锁：向调用方返回成功。
			 */
			result = TM_Ok;
			goto out_unlocked;
		}

l4:
		CHECK_FOR_INTERRUPTS();

		/*
		 * 锁定缓冲区之前，若看起来有必要，先钉住可见性映射页。
		 * 由于我们还没拿到锁，
		 * 别人可能正在改它，所以拿到锁之后需要
		 * 再检查一次。
		 */
		if (PageIsAllVisible(BufferGetPage(buf)))
		{
			visibilitymap_pin(rel, block, &vmbuffer);
			pinned_desired_page = true;
		}
		else
			pinned_desired_page = false;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * 若我们没有钉住可见性映射页，而该页在我们
		 * 忙着锁缓冲区时变成了全可见，就得
		 * 解锁再重新加锁，以免跨 I/O 持有缓冲区锁。
		 * 这有点不幸，但希望不会经常发生。
		 *
		 * 注意：本函数的某些路径到达这里时，
		 * 会钉着一张可见性映射页，它也许匹配
		 * 这一页，也许不匹配。若这一页不是全可见，我们就不会使用那张可见性映射
		 * 页，但会一直把这个钉留到函数结束。
		 */
		if (!pinned_desired_page && PageIsAllVisible(BufferGetPage(buf)))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			visibilitymap_pin(rel, block, &vmbuffer);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * 若有先前的 XMAX，则对照它检查元组的 XMIN。若已到达
		 * 链的末端，就结束，因此返回成功。
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(mytup.t_data),
								 priorXmax))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/*
		 * 也检查 Xmin：若该元组是由已中止的
		 * （子）事务创建的，那么我们已经锁住了链上最后一个活的，
		 * 因此结束，返回成功。
		 */
		if (TransactionIdDidAbort(HeapTupleHeaderGetXmin(mytup.t_data)))
		{
			result = TM_Ok;
			goto out_locked;
		}

		old_infomask = mytup.t_data->t_infomask;
		old_infomask2 = mytup.t_data->t_infomask2;
		xmax = HeapTupleHeaderGetRawXmax(mytup.t_data);

		/*
		 * 若这个元组版本已被某些并发
		 * 事务更新或锁定，怎么做取决于我们的锁模式
		 * 是否与那些事务持有的锁冲突，也取决于
		 * 它们的状态。
		 */
		if (!(old_infomask & HEAP_XMAX_INVALID))
		{
			TransactionId rawxmax;
			bool		needwait;

			rawxmax = HeapTupleHeaderGetRawXmax(mytup.t_data);
			if (old_infomask & HEAP_XMAX_IS_MULTI)
			{
				int			nmembers;
				int			i;
				MultiXactMember *members;

				/*
				 * 不需要为 pg_upgrade 过的元组做测试：这只
				 * 应用于更新链中第一个之后的元组。所说的
				 * 链中第一个元组完全可能是在 9.2 中锁定并经
				 * pg_upgrade 升级的，但那一个已经由调用方锁过，
				 * 不是我们；后续的任何一个都不可能，因为我们的
				 * 调用方必然取得了比
				 * pg_upgrade 本身更晚的快照。
				 */
				Assert(!HEAP_LOCKED_UPGRADED(mytup.t_data->t_infomask));

				nmembers = GetMultiXactIdMembers(rawxmax, &members, false,
												 HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));
				for (i = 0; i < nmembers; i++)
				{
					result = test_lockmode_for_conflict(members[i].status,
														members[i].xid,
														mode,
														&mytup,
														&needwait);

					/*
					 * 若该元组在本过程的前一轮中
					 * 已经被我们自己锁过
					 * （例如 heap_lock_tuple 因为 xmax
					 * 变化而被迫重新开始锁定循环），那么这个元组
					 * 版本上我们已经持有锁，不必再做任何事；而且这也
					 * 不是错误条件。我们只需跳过
					 * 这个元组，继续锁定更新链中的
					 * 下一个版本。
					 */
					if (result == TM_SelfModified)
					{
						pfree(members);
						goto next;
					}

					if (needwait)
					{
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						XactLockTableWait(members[i].xid, rel,
										  &mytup.t_self,
										  XLTW_LockUpdated);
						pfree(members);
						goto l4;
					}
					if (result != TM_Ok)
					{
						pfree(members);
						goto out_locked;
					}
				}
				if (members)
					pfree(members);
			}
			else
			{
				MultiXactStatus status;

				/*
				 * 对非 multi 的 Xmax，首先需要用 infomask 位计算
				 * 对应的 MultiXactStatus。
				 */
				if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
				{
					if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
						status = MultiXactStatusForKeyShare;
					else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
						status = MultiXactStatusForShare;
					else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
					{
						if (old_infomask2 & HEAP_KEYS_UPDATED)
							status = MultiXactStatusForUpdate;
						else
							status = MultiXactStatusForNoKeyUpdate;
					}
					else
					{
						/*
						 * 单独出现的 LOCK_ONLY（旧集簇里被标为
						 * 共享锁定、经 pg_upgrade 升级的元组）不应当
						 * 出现在更新链的中部。
						 */
						elog(ERROR, "invalid lock status in tuple");
					}
				}
				else
				{
					/* 这是一次更新，但是哪一种？ */
					if (old_infomask2 & HEAP_KEYS_UPDATED)
						status = MultiXactStatusUpdate;
					else
						status = MultiXactStatusNoKeyUpdate;
				}

				result = test_lockmode_for_conflict(status, rawxmax, mode,
													&mytup, &needwait);

				/*
				 * 若该元组在前一轮中已经被我们自己锁过
				 * （例如 heap_lock_tuple 因为 xmax 变化而被迫
				 * 重新开始锁定循环），那么这个元组版本上
				 * 我们已经持有锁，不必
				 * 再做任何事；而且这也不是错误条件。
				 * 我们只需跳过这个元组，继续
				 * 锁定更新链中的下一个版本。
				 */
				if (result == TM_SelfModified)
					goto next;

				if (needwait)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					XactLockTableWait(rawxmax, rel, &mytup.t_self,
									  XLTW_LockUpdated);
					goto l4;
				}
				if (result != TM_Ok)
				{
					goto out_locked;
				}
			}
		}

		/* 计算该元组的新 Xmax 和 infomask 值…… */
		compute_new_xmax_infomask(xmax, old_infomask, mytup.t_data->t_infomask2,
								  xid, mode, false,
								  &new_xmax, &new_infomask, &new_infomask2);

		if (PageIsAllVisible(BufferGetPage(buf)) &&
			visibilitymap_clear(rel, block, vmbuffer,
								VISIBILITYMAP_ALL_FROZEN))
			cleared_all_frozen = true;

		START_CRIT_SECTION();

		/* ……并设置它们 */
		HeapTupleHeaderSetXmax(mytup.t_data, new_xmax);
		mytup.t_data->t_infomask &= ~HEAP_XMAX_BITS;
		mytup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		mytup.t_data->t_infomask |= new_infomask;
		mytup.t_data->t_infomask2 |= new_infomask2;

		MarkBufferDirty(buf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			xl_heap_lock_updated xlrec;
			XLogRecPtr	recptr;
			Page		page = BufferGetPage(buf);

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&mytup.t_self);
			xlrec.xmax = new_xmax;
			xlrec.infobits_set = compute_infobits(new_infomask, new_infomask2);
			xlrec.flags =
				cleared_all_frozen ? XLH_LOCK_ALL_FROZEN_CLEARED : 0;

			XLogRegisterData((char *) &xlrec, SizeOfHeapLockUpdated);

			recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_LOCK_UPDATED);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

next:
		/* 若找到更新链的末端，就结束。 */
		if (mytup.t_data->t_infomask & HEAP_XMAX_INVALID ||
			HeapTupleHeaderIndicatesMovedPartitions(mytup.t_data) ||
			ItemPointerEquals(&mytup.t_self, &mytup.t_data->t_ctid) ||
			HeapTupleHeaderIsOnlyLocked(mytup.t_data))
		{
			result = TM_Ok;
			goto out_locked;
		}

		/* 尾递归 */
		priorXmax = HeapTupleHeaderGetUpdateXid(mytup.t_data);
		ItemPointerCopy(&(mytup.t_data->t_ctid), &tupid);
		UnlockReleaseBuffer(buf);
	}

	result = TM_Ok;

out_locked:
	UnlockReleaseBuffer(buf);

out_unlocked:
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	return result;
}

/*
 * heap_lock_updated_tuple
 *		锁定已更新的元组时跟随更新链，在更新后的版本上获取锁（行
 *		标记）。
 *
 * 假定初始元组已经被锁定。
 *
 * 本函数不检查可见性，只是无条件地把
 * 元组标成已锁定。若更新链中任何元组正在被并发删除
 * （或在修改键的情况下被更新），就睡到做这件事的
 * 事务结束。
 *
 * 注意，当我们不得不等待其他事务释放它们时，我们不会在走过的元组上
 * 获取重量级元组锁，这与
 * heap_lock_tuple 的做法不同。原因是有不止一个
 * 事务在走这条链大概足够少见，饥饿的风险
 * 不大：到这里的前提之一是正在使用的
 * 快照早于创建该元组的那次更新（因为我们
 * 是从该元组的更早版本开始的），但同时这样的
 * 事务不能使用可重复读或可串行化隔离
 * 级别，因为那会导致可串行化失败。
 */
static TM_Result
heap_lock_updated_tuple(Relation rel, HeapTuple tuple, ItemPointer ctid,
						TransactionId xid, LockTupleMode mode)
{
	/*
	 * 若元组没有被更新，或者已经移到另一个分区
	 * （实际上是删除），就在这里停下。
	 */
	if (!HeapTupleHeaderIndicatesMovedPartitions(tuple->t_data) &&
		!ItemPointerEquals(&tuple->t_self, ctid))
	{
		/*
		 * 若这是当前事务中第一次可能涉及 multixact 的
		 * 操作，就设置本后端的
		 * OldestMemberMXactId。
		 * 我们可以确定该事务绝不会成为任何比它更老的
		 * MultiXactId 的成员。（即使下面最终只用
		 * 我们自己的 TransactionId，也必须这样做，因为
		 * 别的后端可能随即把我们的 XID 并入一个 MultiXact。）
		 */
		MultiXactIdSetOldestMember();

		return heap_lock_updated_tuple_rec(rel, ctid, xid, mode);
	}

	/* 没有什么要锁的 */
	return TM_Ok;
}

/*
 *	heap_finish_speculative - 把推测性插入标为成功
 *
 * 要成功结束一次推测性插入，必须清除元组上的推测性
 * 令牌。为此，原地修改将包含
 * 推测性令牌值的 t_ctid 字段，使它指向元组自身，
 * 这是新插入的普通元组的特征。
 *
 * 注意：若不结束或中止一次
 * 推测性插入就提交，是不行的。我们可以把已提交
 * 事务的推测性元组隐式当成已完成，但那样就必须准备好
 * 处理已提交元组上的推测性令牌。那并不
 * 困难——没人会去看 xmax 无效的元组的 ctid 字段——
 * 但在完成时清除令牌也不贵。
 * 一条显式的确认 WAL 记录也让逻辑解码更简单。
 */
void
heap_finish_speculative(Relation relation, ItemPointer tid)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(tid);
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	Assert(HeapTupleHeaderIsSpeculative(htup));

	MarkBufferDirty(buffer);

	/*
	 * 用真正的 t_ctid 替换推测性插入令牌，像普通元组那样指向
	 * 它自己。
	 */
	htup->t_ctid = *tid;

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_confirm xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(tid);

		XLogBeginInsert();

		/* 我们希望这里的过滤与普通插入相同 */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		XLogRegisterData((char *) &xlrec, SizeOfHeapConfirm);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CONFIRM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 *	heap_abort_speculative - 杀掉一个推测性插入的元组
 *
 * 把同一命令中推测性插入的元组标成死亡，
 * 方法是把它的 xmin 设为无效。这使它对所有事务
 * （包括我们自己）立即表现为死亡。特别是，它使
 * HeapTupleSatisfiesDirty() 把该元组视为死亡，这样另一个后端
 * 插入重复键值时就不必白白等待我们整个
 * 事务结束（它只需等待我们的推测性插入
 * 结束）。
 *
 * 杀掉该元组可以防止"无原则死锁"，也就是由于
 * 用户看不见的相互依赖而产生的死锁。按
 * 定义，用户无法通过在客户端代码里重排锁获取顺序来防止无原则死锁，
 * 因为实现层的
 * 锁获取不在用户的直接控制之下。若推测性
 * 插入者不采取这个预防措施，那么在高并发下它们
 * 可能彼此死锁，那是不可接受的。
 *
 * 这与 heap_delete 有些重复，但我们更愿意有一个
 * 要求被精简过的专用例程。注意它也
 * 用来删除推测性插入期间创建的 TOAST 元组。
 *
 * 本例程不影响逻辑解码，因为逻辑解码只看
 * 确认记录。
 */
void
heap_abort_speculative(Relation relation, ItemPointer tid)
{
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;
	TransactionId prune_xid;

	Assert(ItemPointerIsValid(tid));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * 该页不可能全可见，我们刚刚插入了它，而且仍在
	 * 运行。
	 */
	Assert(!PageIsAllVisible(page));

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

	/*
	 * 健全性检查：该元组确实是推测性插入的元组，
	 * 并且是我们插入的。
	 */
	if (tp.t_data->t_choice.t_heap.t_xmin != xid)
		elog(ERROR, "attempted to kill a tuple inserted by another transaction");
	if (!(IsToastRelation(relation) || HeapTupleHeaderIsSpeculative(tp.t_data)))
		elog(ERROR, "attempted to kill a non-speculative tuple");
	Assert(!HeapTupleHeaderIsHeapOnly(tp.t_data));

	/*
	 * 这里不必检查可串行化冲突。也绝不会
	 * 需要组合 CID。不必提取副本标识，也不必
	 * 对 infomask 位做任何特殊处理。
	 */

	START_CRIT_SECTION();

	/*
	 * 该元组会立即变成 DEAD。把 xmin 设为 TransactionXmin，
	 * 标明此页是剪枝候选。虽然不能
	 * 立即剪枝，但这是我们能便宜地确定的、
	 * 对回卷安全、也不会比该表的
	 * relfrozenxid 更老的最老 xid。为防备一种不太可能的情况：新关系的
	 * relfrozenxid 比我们的 TransactionXmin 更新，若是这样就用 relfrozenxid
	 * （清理随后不能把 relfrozenxid 移到
	 * TransactionXmin 之后，因此这里没有竞态）。
	 */
	Assert(TransactionIdIsValid(TransactionXmin));
	if (TransactionIdPrecedes(TransactionXmin, relation->rd_rel->relfrozenxid))
		prune_xid = relation->rd_rel->relfrozenxid;
	else
		prune_xid = TransactionXmin;
	PageSetPrunable(page, prune_xid);

	/* 存储删除该元组的事务信息 */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;

	/*
	 * 把元组头部的 xmin 设为 InvalidTransactionId。这使该
	 * 元组对所有人立即不可见。（特别是对任何
	 * 正在等待推测性令牌、稍后会被唤醒的事务。）
	 */
	HeapTupleHeaderSetXmin(tp.t_data, InvalidTransactionId);

	/* 也清除推测性插入令牌 */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

	/*
	 * XLOG 相关
	 *
	 * 这里生成的 WAL 记录与 heap_delete() 匹配。使用相同的恢复
	 * 例程。
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;

		xlrec.flags = XLH_DELETE_IS_SUPER;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = xid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/* 不记录副本标识和复制源 */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (HeapTupleHasExternal(&tp))
	{
		Assert(!IsToastRelation(relation));
		heap_toast_delete(relation, &tp, true);
	}

	/*
	 * 绝不需要把元组标成失效，因为系统目录不支持
	 * 推测性插入
	 */

	/* 现在可以释放缓冲区了 */
	ReleaseBuffer(buffer);

	/* 把删除计数，因为插入也计数了 */
	pgstat_count_heap_delete(relation);
}

/*
 * heap_inplace_lock - 保护原地更新，使其不受并发 heap_update() 影响
 *
 * 评估元组状态是否与非键更新兼容。
 * 当前事务的行标记没问题，任何事务的 KEY SHARE
 * 也没问题。若兼容，返回 true，并且缓冲区已排他锁定，
 * 调用方必须通过调用
 * heap_inplace_update_and_unlock()、调用 heap_inplace_unlock() 或抛出
 * 错误来释放它。否则，调用 release_callback(arg)，等待阻塞的
 * 事务结束，并返回 false。
 *
 * 由于这是为系统目录准备的，而 SERIALIZABLE 不覆盖
 * DDL，这并不保证任何特定的谓词锁定。
 *
 * 有人可以改成对正在删除的元组也返回 true。
 * 所有原地更新者都会取一把与 DROP 冲突的锁。若显式的
 * "DELETE FROM pg_class" 正在进行，我们会像等待一次
 * 更新那样等待它。
 *
 * 原地更新字段的读者预期这些字段的变化是
 * 持久的。例如 vac_truncate_clog() 经由系统目录快照从
 * pg_database 元组读取 datfrozenxid。将来的快照不得
 * 对同一数据库 OID 返回更小的 datfrozenxid（按
 * FullTransactionIdPrecedes() 的意义更小）。我们能做到这一点，是因为持有其缓冲区锁时，
 * 对该元组的更新不能开始。像
 * BEGIN;GRANT;CREATE INDEX;COMMIT 这样，我们正在原地更新一个只对本
 * 事务可见的元组。于是 ROLLBACK 是可以丢失
 * 原地更新的一种情形。（ROLLBACK 时把 relhasindex 恢复为 false 没问题，因为
 * 任何并发的 CREATE INDEX 都会先被阻塞，然后原地更新那个
 * 已提交的元组。）
 *
 * 原则上，我们可以靠覆盖更新链中的每个元组来避免等待。
 * 读者的预期只允许在以下情况下更新元组：
 * 它已中止、它是链尾，或者我们已经更新了
 * 其 t_ctid 引用的元组。因此需要按从尾到头的
 * 顺序覆盖这些元组。这意味着要么 (a) 在一个临界区里改所有元组，
 * 要么 (b) 接受部分完成的可能。
 * relfrozenxid 更新的部分完成会有奇怪的
 * 后果：该表下一次 VACUUM 可能看到该表的 relfrozenxid
 * 在 vacuum_get_cutoffs() 与结束之间向前移动。
 */
bool
heap_inplace_lock(Relation relation,
				  HeapTuple oldtup_ptr, Buffer buffer,
				  void (*release_callback) (void *), void *arg)
{
	HeapTupleData oldtup = *oldtup_ptr; /* 尽量缩小与 heap_update() 的差异 */
	TM_Result	result;
	bool		ret;

#ifdef USE_ASSERT_CHECKING
	if (RelationGetRelid(relation) == RelationRelationId)
		check_inplace_rel_lock(oldtup_ptr);
#endif

	Assert(BufferIsValid(buffer));

	LockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*----------
	 * 像 heap_update() 那样解释 HeapTupleSatisfiesUpdate()，只是：
	 *
	 * - 无条件等待
	 * - 上面已经锁了元组，因为原地更新无条件需要那样
	 * - 等待之后不再复查头部：推迟到下一轮更简单
	 * - 即使更新者中止也不尝试继续：同样如此
	 * - 没有交叉检查
	 */
	result = HeapTupleSatisfiesUpdate(&oldtup, GetCurrentCommandId(false),
									  buffer);

	if (result == TM_Invisible)
	{
		/* 没有已知途径会发生这种情况 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_internal("attempted to overwrite invisible tuple")));
	}
	else if (result == TM_SelfModified)
	{
		/*
		 * 若表达式傻到去
		 * 调用例如 SELECT ... FROM pg_class FOR SHARE，CREATE INDEX 可能到达这里。其他 SQL
		 * 语句的 C 代码在对同一行做了 heap_update() 之后，若
		 * 中间没有 CommandCounterIncrement()，也可能到这里。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
	}
	else if (result == TM_BeingModified)
	{
		TransactionId xwait;
		uint16		infomask;

		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			LockTupleMode lockmode = LockTupleNoKeyExclusive;
			MultiXactStatus mxact_status = MultiXactStatusNoKeyUpdate;
			int			remain;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										lockmode, NULL))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				release_callback(arg);
				ret = false;
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
			}
			else
				ret = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
			ret = true;
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			ret = true;
		else
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
			ret = false;
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
		}
	}
	else
	{
		ret = (result == TM_Ok);
		if (!ret)
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			release_callback(arg);
		}
	}

	/*
	 * GetCatalogSnapshot() 依靠失效消息来知道何时
	 * 取一个新快照。xwait 的 COMMIT 负责发送
	 * 失效消息。我们没有获取足以在消息尚未发出时
	 * 阻塞的重量级锁，因此必须取一个新快照，以确保稍后的
	 * 尝试有公平的机会。即使 xwait 已中止我们也不需要这样做，
	 * 但不必为那种情况做优化。
	 */
	if (!ret)
	{
		UnlockTuple(relation, &oldtup.t_self, InplaceUpdateTupleLock);
		InvalidateCatalogSnapshot();
	}
	return ret;
}

/*
 * heap_inplace_update_and_unlock - systable_inplace_update_finish 的核心
 *
 * 元组不能改变大小，因此它的头部字段和空值
 * 位图（若有）也不会改变。
 *
 * 由于我们持有 LOCKTAG_TUPLE，没有更新者持有该元组的本地副本。
 */
void
heap_inplace_update_and_unlock(Relation relation,
							   HeapTuple oldtup, HeapTuple tuple,
							   Buffer buffer)
{
	HeapTupleHeader htup = oldtup->t_data;
	uint32		oldlen;
	uint32		newlen;

	Assert(ItemPointerEquals(&oldtup->t_self, &tuple->t_self));
	oldlen = oldtup->t_len - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "wrong tuple length");

	/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	memcpy((char *) htup + htup->t_hoff,
		   (char *) tuple->t_data + tuple->t_data->t_hoff,
		   newlen);

	/*----------
	 * XXX 若在这里崩溃，datfrozenxid() 可能跑到 relfrozenxid 前面：
	 *
	 * ["D" 是一次 VACUUM (ONLY_DATABASE_STATS)]
	 * ["R" 是一次 VACUUM tbl]
	 * D: vac_update_datfrozenid() -> systable_beginscan(pg_class)
	 * D: systable_getnext() 返回 tbl 的 pg_class 元组
	 * R: memcpy() 写入 tbl 的 pg_class 元组
	 * D: 提高 pg_database.datfrozenxid，XLogInsert()，结束
	 * [崩溃]
	 * [恢复时恢复了 datfrozenxid，却没有 relfrozenxid]
	 */

	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_inplace xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapInplace);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) htup + htup->t_hoff, newlen);

		/* 目前不解码原地更新，不要记录复制源 */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);

		PageSetLSN(BufferGetPage(buffer), recptr);
	}

	END_CRIT_SECTION();

	heap_inplace_unlock(relation, oldtup, buffer);

	/*
	 * 必要时发出共享缓存失效。注意因为我们只
	 * 传入元组的新版本，这不得用于任何
	 * 可能改变 catcache 查找键的操作。但我们反正也没有
	 * 费心做索引更新，所以这一点更是如此。
	 *
	 * XXX ROLLBACK 会丢弃这次失效。见测试 inplace-inval.spec。
	 */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple, NULL);
}

/*
 * heap_inplace_unlock - heap_inplace_lock 的逆操作
 */
void
heap_inplace_unlock(Relation relation,
					HeapTuple oldtup, Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	UnlockTuple(relation, &oldtup->t_self, InplaceUpdateTupleLock);
}

/*
 * heap_inplace_update - 已弃用
 *
 * 它存在只是为了让模块在后向分支上仍能工作。受影响的
 * 模块应当迁移到 systable_inplace_update_begin()。
 */
void
heap_inplace_update(Relation relation, HeapTuple tuple)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	uint32		newlen;

	/*
	 * 目前不允许并行更新。与普通更新不同，
	 * 这绝不应当创建组合 CID，因此也许可以放宽
	 * 这个限制，但需要更多思考和测试。反正也不
	 * 清楚那样做是否有用。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&(tuple->t_self)));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(&(tuple->t_self));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	oldlen = ItemIdGetLength(lp) - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "wrong tuple length");

	/* 从这里到变更记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	memcpy((char *) htup + htup->t_hoff,
		   (char *) tuple->t_data + tuple->t_data->t_hoff,
		   newlen);

	MarkBufferDirty(buffer);

	/* XLOG 相关 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_inplace xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapInplace);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) htup + htup->t_hoff, newlen);

		/* 目前不解码原地更新，不要记录复制源 */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	/*
	 * 必要时发出共享缓存失效。注意因为我们只
	 * 传入元组的新版本，这不得用于任何
	 * 可能改变 catcache 查找键的操作。但我们反正也没有
	 * 费心做索引更新，所以这一点更是如此。
	 */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple, NULL);
}

#define		FRM_NOOP				0x0001
#define		FRM_INVALIDATE_XMAX		0x0002
#define		FRM_RETURN_IS_XID		0x0004
#define		FRM_RETURN_IS_MULTI		0x0008
#define		FRM_MARK_COMMITTED		0x0010

/*
 * FreezeMultiXactId
 *		冻结时，当元组被 MultiXactId 标记，决定做什么。
 *
 * "flags" 是输出值；用来告诉调用方返回时做什么。
 * "pagefrz" 是输入/输出值，用来管理页级冻结。
 *
 * 我们可以在 "flags" 里设置的可能值：
 * FRM_NOOP
 *		什么都不做——保留现有的 Xmax
 * FRM_INVALIDATE_XMAX
 *		把 Xmax 标为 InvalidTransactionId 并设置 XMAX_INVALID 标志。
 * FRM_RETURN_IS_XID
 *		返回的 Xid 是要设为 xmax 的单个更新 Xid。
 * FRM_MARK_COMMITTED
 *		可以把 Xmax 标为 HEAP_XMAX_COMMITTED
 * FRM_RETURN_IS_MULTI
 *		返回值是要设为新 Xmax 的新 MultiXactId。
 *		（调用方必须用 GetMultiXactIdHintBits 取得正确的 infomask 位）
 *
 * 调用方把页面冻结的控制权委托给我们。
 * 实践中，除非指示了 FRM_NOOP，
 * 我们总是强制冻结调用方的页。
 * 我们帮助调用方确保 XID < FreezeLimit 且 MXID < MultiXactCutoff
 * 绝不会被留下。我们可以自由选择何时以及如何处理每个
 * Multi，同时绝不违反冻结的截止后置条件。
 *
 * 按主动的时间表（相对于冻结
 * XID）移除 Multi 是有用的，以便把 MultiXact 成员的 SLRU 缓冲区未命中降到最少。对我们来说，短期内
 * 也可能更便宜，因为急切处理同样能让我们避免 SLRU 缓冲区
 * 未命中。
 *
 * 注意：当设置 FRM_RETURN_IS_MULTI 时会创建一个_新的_ MultiXactId，不过只在
 * FreezeLimit 和/或 MultiXactCutoff 截止值让我们别无选择时。
 * 这通常可以推迟，而推迟通常就足以完全避免它。
 * 原则上应当避免在 VACUUM 期间分配新的 multi；
 * 只有 VACUUM 能推进 relminmxid，因此在这里分配新 Multi 有
 * 它自己的特殊风险。
 *
 * 注意：当我们没有强制页级冻结时，调用方必须用 heap_tuple_should_freeze
 * 维护 "no freeze" 的 NewRelfrozenXid/NewRelminMxid 跟踪器。
 *
 * 注意：当我们已经强制了页级冻结时，调用方应避免不必要地调用 heap_tuple_should_freeze，
 * 因为那可能招致我们特意想靠冻结来避免的同样的
 * SLRU 缓冲区未命中。
 */
static TransactionId
FreezeMultiXactId(MultiXactId multi, uint16 t_infomask,
				  const struct VacuumCutoffs *cutoffs, uint16 *flags,
				  HeapPageFreeze *pagefrz)
{
	TransactionId newxmax;
	MultiXactMember *members;
	int			nmembers;
	bool		need_replace;
	int			nnewmembers;
	MultiXactMember *newmembers;
	bool		has_lockers;
	TransactionId update_xid;
	bool		update_committed;
	TransactionId FreezePageRelfrozenXid;

	*flags = 0;

	/* 只应当在 Multi 上被调用 */
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	if (!MultiXactIdIsValid(multi) ||
		HEAP_LOCKED_UPGRADED(t_infomask))
	{
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}
	else if (MultiXactIdPrecedes(multi, cutoffs->relminmxid))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found multixact %u from before relminmxid %u",
								 multi, cutoffs->relminmxid)));
	else if (MultiXactIdPrecedes(multi, cutoffs->OldestMxact))
	{
		TransactionId update_xact;

		/*
		 * 这个旧 multi 不可能还有成员仍在运行，但
		 * 还是核实一下。若它只是加锁者，可以不加
		 * 进一步考虑就移除；但若它包含一次更新，
		 * 我们也许需要保留它。
		 */
		if (MultiXactIdIsRunning(multi,
								 HEAP_XMAX_IS_LOCKED_ONLY(t_infomask)))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u from before multi freeze cutoff %u found to be still running",
									 multi, cutoffs->OldestMxact)));

		if (HEAP_XMAX_IS_LOCKED_ONLY(t_infomask))
		{
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* 用其更新者的单个 XID 替换 multi？ */
		update_xact = MultiXactIdGetUpdateXid(multi, t_infomask);
		if (TransactionIdPrecedes(update_xact, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains update XID %u from before relfrozenxid %u",
									 multi, update_xact,
									 cutoffs->relfrozenxid)));
		else if (TransactionIdPrecedes(update_xact, cutoffs->OldestXmin))
		{
			/*
			 * 更新者 XID 必定已经中止（否则元组会被
			 * 剪掉，因为更新者 XID < OldestXmin）。
			 * 直接移除 xmax。
			 */
			if (TransactionIdDidCommit(update_xact))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
										 multi, update_xact,
										 cutoffs->OldestXmin)));
			*flags |= FRM_INVALIDATE_XMAX;
			pagefrz->freeze_required = true;
			return InvalidTransactionId;
		}

		/* 必须把更新者 XID 保留为新的 xmax */
		*flags |= FRM_RETURN_IS_XID;
		pagefrz->freeze_required = true;
		return update_xact;
	}

	/*
	 * 这个 Multi 的某些成员可能低于 FreezeLimit 这个 xid 截止值，因此我们
	 * 需要走遍整个成员数组，弄清要做什么，如果
	 * 有什么要做的话。
	 */
	nmembers =
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(t_infomask));
	if (nmembers <= 0)
	{
		/* 没有值得保留的东西 */
		*flags |= FRM_INVALIDATE_XMAX;
		pagefrz->freeze_required = true;
		return InvalidTransactionId;
	}

	/*
	 * FRM_NOOP 是唯一可能需要把 FreezePageRelfrozenXid 或
	 * FreezePageRelminMxid 往回调的情形。它也是唯一
	 * 调用方可能把 NoFreezePageRelfrozenXid
	 * 或 NoFreezePageRelminMxid 这些"不冻结"跟踪器往回调以处理某个 multi 的情形。
	 * FRM_NOOP 处理应当使 VACUUM 管理的 NewRelfrozenXid/NewRelminMxid
	 * 跟踪器按 xmax 往回调到这样的程度：
	 * 足以安全地让 xmax 保持不动，而不论
	 * 页面冻结是否在别处被触发。
	 *
	 * 我们的策略是除 FRM_NOOP 以外的每种情形都强制冻结，
	 * 这样就不必在任何地方维护这两套跟踪器。
	 * 其他每种情形都会可靠地为 xmax 执行冻结计划，
	 * 要么用 >= OldestXmin/OldestMxact 的 XID/MXID 替换 xmax，要么
	 * 把 xmax 设为 InvalidTransactionId，从而使 xmax 完全冻结。
	 * （VACUUM 的 NewRelfrozenXid/NewRelminMxid 跟踪器用
	 * OldestXmin/OldestMxact 初始化，因此这里不必跟踪更晚的值。）
	 */
	need_replace = false;
	FreezePageRelfrozenXid = pagefrz->FreezePageRelfrozenXid;
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
		{
			/* 不能违反 FreezeLimit 后置条件 */
			need_replace = true;
			break;
		}
		if (TransactionIdPrecedes(xid, FreezePageRelfrozenXid))
			FreezePageRelfrozenXid = xid;
	}

	/* 也不能违反 MultiXactCutoff 后置条件 */
	if (!need_replace)
		need_replace = MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff);

	if (!need_replace)
	{
		/*
		 * vacuumlazy.c 可能会把 NewRelminMxid、NewRelfrozenXid 或
		 * 两者一起往回调，以便在冻结其页面之后
		 * 仍安全地保留这个特定 multi
		 */
		*flags |= FRM_NOOP;
		pagefrz->FreezePageRelfrozenXid = FreezePageRelfrozenXid;
		if (MultiXactIdPrecedes(multi, pagefrz->FreezePageRelminMxid))
			pagefrz->FreezePageRelminMxid = multi;
		pfree(members);
		return multi;
	}

	/*
	 * 对这个 multi 再做一遍更彻底的扫描，弄清哪些
	 * 成员 XID 真正需要保留。检查各个成员的精确状态
	 * 甚至可能表明我们什么都不必保留。
	 * 尽管这个 Multi 必须 >= OldestMxact，这仍然很有可能，
	 * 因为第二遍只在真正必要时才保留成员 XID；
	 * 即使 >= OldestXmin 的成员 XID，第二遍也常常不会保留。
	 */
	nnewmembers = 0;
	newmembers = palloc(sizeof(MultiXactMember) * nmembers);
	has_lockers = false;
	update_xid = InvalidTransactionId;
	update_committed = false;

	/*
	 * 决定每个成员 xid 是保留，还是改为忽略
	 */
	for (int i = 0; i < nmembers; i++)
	{
		TransactionId xid = members[i].xid;
		MultiXactStatus mstatus = members[i].status;

		Assert(!TransactionIdPrecedes(xid, cutoffs->relfrozenxid));

		if (!ISUPDATE_from_mxstatus(mstatus))
		{
			/*
			 * 加锁者 XID（不是更新者 XID）。我们只保留仍在
			 * 运行的加锁者。
			 */
			if (TransactionIdIsCurrentTransactionId(xid) ||
				TransactionIdIsInProgress(xid))
			{
				if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg_internal("multixact %u contains running locker XID %u from before removable cutoff %u",
											 multi, xid,
											 cutoffs->OldestXmin)));
				newmembers[nnewmembers++] = members[i];
				has_lockers = true;
			}

			continue;
		}

		/*
		 * 更新者 XID（不是加锁者 XID）。应当保留它吗？
		 *
		 * 既然清理剪枝时没有把元组完全移除，这个
		 * 更新 Xid 不可能比 OldestXmin 截止值更老，除非
		 * 更新者 XID 已中止。若更新者事务已知
		 * 已中止或已崩溃，就可以忽略它，否则不行。
		 *
		 * 无论如何，Multi 绝不应当包含两个更新者，无论
		 * 它们各自的提交状态如何。顺便先检查这一点。
		 */
		if (TransactionIdIsValid(update_xid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u has two or more updating members",
									 multi),
					 errdetail_internal("First updater XID=%u second updater XID=%u.",
										update_xid, xid)));

		/*
		 * 与所有元组可见性例程一样，必须先测试
		 * TransactionIdIsInProgress，再测试 TransactionIdDidCommit，因为
		 * heapam_visibility.c 里详细解释过的竞态条件。
		 */
		if (TransactionIdIsCurrentTransactionId(xid) ||
			TransactionIdIsInProgress(xid))
			update_xid = xid;
		else if (TransactionIdDidCommit(xid))
		{
			/*
			 * 事务已提交，因此可以告诉调用方设置
			 * HEAP_XMAX_COMMITTED。（我们能这样做，只是因为知道该
			 * 事务不在运行。）
			 */
			update_committed = true;
			update_xid = xid;
		}
		else
		{
			/*
			 * 不在进行、未提交——必定已中止或已崩溃；
			 * 我们可以忽略它。
			 */
			continue;
		}

		/*
		 * 我们判定必须保留更新者——把它加入待定的新
		 * 成员列表
		 */
		if (TransactionIdPrecedes(xid, cutoffs->OldestXmin))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("multixact %u contains committed update XID %u from before removable cutoff %u",
									 multi, xid, cutoffs->OldestXmin)));
		newmembers[nnewmembers++] = members[i];
	}

	pfree(members);

	/*
	 * 根据第二遍收集的信息，
	 * 决定对调用方的 multi 做什么
	 */
	if (nnewmembers == 0)
	{
		/* 没有值得保留的东西 */
		*flags |= FRM_INVALIDATE_XMAX;
		newxmax = InvalidTransactionId;
	}
	else if (TransactionIdIsValid(update_xid) && !has_lockers)
	{
		/*
		 * 若只剩一个成员且它是更新，就单独把它传回去，
		 * 而不创建新的 Multi。（XXX 只剩一个
		 * 加锁者时也可以这样做，但那会把 API 搞得太
		 * 复杂；而且单个更新者的情形更
		 * 有意思，因为它们活得更久。）
		 */
		Assert(nnewmembers == 1);
		*flags |= FRM_RETURN_IS_XID;
		if (update_committed)
			*flags |= FRM_MARK_COMMITTED;
		newxmax = update_xid;
	}
	else
	{
		/*
		 * 用前一个 multixact 中存活的成员创建一个新的，
		 * 作为元组里的新 Xmax
		 */
		newxmax = MultiXactIdCreateFromMembers(nnewmembers, newmembers);
		*flags |= FRM_RETURN_IS_MULTI;
	}

	pfree(newmembers);

	pagefrz->freeze_required = true;
	return newxmax;
}

/*
 * heap_prepare_freeze_tuple
 *
 * 检查元组的各个 XID 字段（xmin、xmax、xvac）
 * 是否比 OldestXmin 和/或 OldestMxact 冻结截止值更老。若是，
 * 就设置足够的状态（在 *frz 输出参数里），使调用方能够
 * 把该元组作为冻结其页面的一部分来处理，并返回 true。若现在对元组
 * 什么都不能改，则返回 false。
 *
 * 若调用方执行返回的冻结计划后该元组将完全冻结，
 * 也把 *totally_frozen 设为 true（或者该元组先前已被更早的
 * VACUUM 完全冻结）。这表示没有剩余的
 * XID 或 MultiXactId 需要将来的 VACUUM 处理。
 *
 * VACUUM 调用方必须为每个我们返回 true 的
 * 元组组装 HeapTupleFreeze 冻结计划项，并调用 heap_freeze_execute_prepared
 * 来执行冻结。调用方必须在对每个堆页第一次调用这里之前，
 * 为整页初始化 pagefrz 字段。
 *
 * 是否冻结整页由 VACUUM 调用方决定。
 * 我们常常会为调用方随即丢弃的页面准备冻结计划。
 * 不过 VACUUM 并不总能做选择；当
 * pagefrz.freeze_required 被设置时它必须冻结，以确保任何 XID < FreezeLimit（以及
 * MXID < MultiXactCutoff）绝不会被留下。我们帮忙确保
 * VACUUM 始终遵守这条规则。
 *
 * 有时我们会在严格说来还远不必要的时候就强制冻结 xmax 的 MultiXactId 值，
 * 只是为了确保 FreezeLimit 后置条件。
 * 当处理 MultiXactId 很便宜时，主动处理是值得的，
 * 而借"强制冻结"机制顺带完成这件事很方便。
 * 反过来，有时我们会推迟冻结 MultiXactId，
 * 因为眼下很贵（不过只在仍有可能
 * 这样做而不违反 FreezeLimit/MultiXactCutoff 后置条件时）。
 *
 * 假定调用方已经用
 * HeapTupleSatisfiesVacuum() 检查过该元组，并判定它不是 HEAPTUPLE_DEAD
 * （否则我们应当移除元组，而不是冻结它）。
 *
 * 注意：本函数有副作用：它可能分配一个新的 MultiXactId。
 * 稍后在 heap_execute_freeze_tuple 里处理我们的 *frz 输出时，它会被
 * 设为元组的新 xmax。若元组在共享缓冲区里，
 * 调用方最好已经持有它的排他锁。
 */
bool
heap_prepare_freeze_tuple(HeapTupleHeader tuple,
						  const struct VacuumCutoffs *cutoffs,
						  HeapPageFreeze *pagefrz,
						  HeapTupleFreeze *frz, bool *totally_frozen)
{
	bool		xmin_already_frozen = false,
				xmax_already_frozen = false;
	bool		freeze_xmin = false,
				replace_xvac = false,
				replace_xmax = false,
				freeze_xmax = false;
	TransactionId xid;

	frz->xmax = HeapTupleHeaderGetRawXmax(tuple);
	frz->t_infomask2 = tuple->t_infomask2;
	frz->t_infomask = tuple->t_infomask;
	frz->frzflags = 0;
	frz->checkflags = 0;

	/*
	 * 处理 xmin，同时跟踪它是否已经冻结，或者
	 * 是否当且仅当调用方执行我们的冻结计划时才会冻结（也可能
	 * 两者都不是）。
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (!TransactionIdIsNormal(xid))
		xmin_already_frozen = true;
	else
	{
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmin %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* 将在下面的冻结计划里设置 freeze_xmin 标志 */
		freeze_xmin = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/* 若以及当冻结计划被执行时，验证 xmin 已提交 */
		if (freeze_xmin)
			frz->checkflags |= HEAP_FREEZE_CHECK_XMIN_COMMITTED;
	}

	/*
	 * 旧式 VACUUM FULL 已经没有了，但只要数据库里还支持
	 * 有 MOVED_OFF/MOVED_IN 元组，我们就必须处理 xvac
	 */
	xid = HeapTupleHeaderGetXvac(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		Assert(TransactionIdPrecedes(xid, cutoffs->OldestXmin));

		/*
		 * 对 Xvac，我们总是主动冻结。这让 totally_frozen
		 * 跟踪可以忽略 xvac。
		 */
		replace_xvac = pagefrz->freeze_required = true;

		/* 将在下面的冻结计划里设置 replace_xvac 标志 */
	}

	/* 现在处理 xmax */
	xid = frz->xmax;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* 原始 xmax 是 MultiXactId */
		TransactionId newxmax;
		uint16		flags;

		/*
		 * 我们要么完全移除 xmax（走 "freeze_xmax" 路径），
		 * 要么通过替换来处理 xmax（走 "replace_xmax" 路径），要么
		 * 对 xmax 不做任何事。唯一的约束是绝不能违反
		 * FreezeLimit/MultiXactCutoff 后置条件。
		 */
		newxmax = FreezeMultiXactId(xid, tuple->t_infomask, cutoffs,
									&flags, pagefrz);

		if (flags & FRM_NOOP)
		{
			/*
			 * xmax 是 MultiXactId，目前它的任何方面都不变。
			 * 这是 FreezeMultiXactId 不会为我们设置 'freeze_required' 的唯一情形，
			 * 也是（给定一个 multi 时）既不设置 freeze_xmax 也不设置 replace_xmax 的唯一情形。
			 *
			 * 这是空操作，但调用 FreezeMultiXactId 可能已经
			 * 为我们把 NewRelfrozenXid 和/或 NewRelminMxid 跟踪器
			 * 往回调了（具体说是"冻结页面"那些变体）。
			 * 那样
			 * 调用方稍后冻结该页就是安全的，同时
			 * 让这个特定的 xmax 保持不动。
			 *
			 * 不过 FreezeMultiXactId _并不_负责"不冻结"
			 * 的 NewRelfrozenXid/NewRelminMxid 跟踪器——那是我们的
			 * 工作。若还没设置 'freeze_required'，下面会对同一元组
			 * 调用 heap_tuple_should_freeze。
			 * （这重复了 FreezeMultiXactId 的工作，但让"不
			 * 冻结"跟踪器的维护只发生在一个地方。）
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->MultiXactCutoff));
			Assert(MultiXactIdIsValid(newxmax) && xid == newxmax);
		}
		else if (flags & FRM_RETURN_IS_XID)
		{
			/*
			 * xmax 将变成一个更新者 Xid（原 MultiXact 的更新者
			 * 成员 Xid 会作为 Xmax 里的简单 Xid 继续传递）。
			 */
			Assert(!TransactionIdPrecedes(newxmax, cutoffs->OldestXmin));

			/*
			 * 注意：其中一些变换之所以有效，只是因为我们
			 * 知道返回的 Xid 是元组更新者（即不仅仅是
			 * 加锁者）。另外注意我们不显式
			 * 操心 HEAP_KEYS_UPDATED，是因为它住在
			 * t_infomask2 里而不是 t_infomask。
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->xmax = newxmax;
			if (flags & FRM_MARK_COMMITTED)
				frz->t_infomask |= HEAP_XMAX_COMMITTED;
			replace_xmax = true;
		}
		else if (flags & FRM_RETURN_IS_MULTI)
		{
			uint16		newbits;
			uint16		newbits2;

			/*
			 * xmax 是必须替换成新的旧 MultiXactId，
			 * 以便继续传递两个或更多原来的成员 XID。
			 */
			Assert(!MultiXactIdPrecedes(newxmax, cutoffs->OldestMxact));

			/*
			 * 这里不能直接对新 multi 使用 GetMultiXactIdHintBits；
			 * 那个例程把掩码初始化为全零，那样会
			 * 丢掉我们需要的其他位。这样做可以确保所有
			 * 无关的位保持不动。
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			GetMultiXactIdHintBits(newxmax, &newbits, &newbits2);
			frz->t_infomask |= newbits;
			frz->t_infomask2 |= newbits2;
			frz->xmax = newxmax;
			replace_xmax = true;
		}
		else
		{
			/*
			 * 元组的冻结计划在最严格的意义上"冻结 xmax"：
			 * 它会让 xmax 里什么都不留（既没有 Xid 也没有 MultiXactId）。
			 */
			Assert(flags & FRM_INVALIDATE_XMAX);
			Assert(!TransactionIdIsValid(newxmax));

			/* 将在下面的冻结计划里设置 freeze_xmax 标志 */
			freeze_xmax = true;
		}

		/* MultiXactId 处理会强制冻结（FRM_NOOP 情形除外） */
		Assert(pagefrz->freeze_required || (!freeze_xmax && !replace_xmax));
	}
	else if (TransactionIdIsNormal(xid))
	{
		/* 原始 xmax 是普通 XID */
		if (TransactionIdPrecedes(xid, cutoffs->relfrozenxid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("found xmax %u from before relfrozenxid %u",
									 xid, cutoffs->relfrozenxid)));

		/* 将在下面的冻结计划里设置 freeze_xmax 标志 */
		freeze_xmax = TransactionIdPrecedes(xid, cutoffs->OldestXmin);

		/*
		 * 若以及当冻结计划被执行时，验证 xmax 已中止，
		 * 前提是它来自一次更新。（只含锁的 xmax 可以移除，
		 * 与此无关，因为锁在事务结束时释放。）
		 */
		if (freeze_xmax && !HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			frz->checkflags |= HEAP_FREEZE_CHECK_XMAX_ABORTED;
	}
	else if (!TransactionIdIsValid(xid))
	{
		/* 原始 xmax 是 InvalidTransactionId 这个 XID */
		Assert((tuple->t_infomask & HEAP_XMAX_IS_MULTI) == 0);
		xmax_already_frozen = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("found raw xmax %u (infomask 0x%04x) not invalid and not multi",
								 xid, tuple->t_infomask)));

	if (freeze_xmin)
	{
		Assert(!xmin_already_frozen);

		frz->t_infomask |= HEAP_XMIN_FROZEN;
	}
	if (replace_xvac)
	{
		/*
		 * 若 MOVED_OFF 元组不是死的，xvac 事务必定已经
		 * 失败；而非死的 MOVED_IN 元组必定意味着 xvac
		 * 事务成功了。
		 */
		Assert(pagefrz->freeze_required);
		if (tuple->t_infomask & HEAP_MOVED_OFF)
			frz->frzflags |= XLH_INVALID_XVAC;
		else
			frz->frzflags |= XLH_FREEZE_XVAC;
	}
	if (replace_xmax)
	{
		Assert(!xmax_already_frozen && !freeze_xmax);
		Assert(pagefrz->freeze_required);

		/* 先前已在冻结计划里设置了 replace_xmax 标志 */
	}
	if (freeze_xmax)
	{
		Assert(!xmax_already_frozen && !replace_xmax);

		frz->xmax = InvalidTransactionId;

		/*
		 * 元组可能被标成 XMAX_INVALID，或 XMAX_COMMITTED 加
		 * LOCKED。规范成 INVALID，以免有人困惑。
		 * 同时去掉 HEAP_KEYS_UPDATED 位。
		 */
		frz->t_infomask &= ~HEAP_XMAX_BITS;
		frz->t_infomask |= HEAP_XMAX_INVALID;
		frz->t_infomask2 &= ~HEAP_HOT_UPDATED;
		frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	}

	/*
	 * 判断该元组是否已经完全冻结，或者将会
	 * 完全冻结（前提是调用方执行该页的冻结计划）
	 */
	*totally_frozen = ((freeze_xmin || xmin_already_frozen) &&
					   (freeze_xmax || xmax_already_frozen));

	if (!pagefrz->freeze_required && !(xmin_already_frozen &&
									   xmax_already_frozen))
	{
		/*
		 * 到目前为止，该页上没有先前的元组使冻结成为强制。
		 * 这个元组是否迫使调用方冻结整页？
		 */
		pagefrz->freeze_required =
			heap_tuple_should_freeze(tuple, cutoffs,
									 &pagefrz->NoFreezePageRelfrozenXid,
									 &pagefrz->NoFreezePageRelminMxid);
	}

	/* 告诉调用方该元组是否在 *frz 里设置了可用的冻结计划 */
	return freeze_xmin || replace_xvac || replace_xmax || freeze_xmax;
}

/*
 * heap_execute_freeze_tuple
 *		按调用方的冻结计划执行已准备好的元组冻结。
 *
 * 调用方负责确保没有其他后端能访问
 * 该元组底层的存储，要么对该元组所在缓冲区持有排他锁
 * （惰性 VACUUM 就是这样做的），要么让它处于
 * 私有存储中（CLUSTER 及其同类就是这样做的）。
 */
static inline void
heap_execute_freeze_tuple(HeapTupleHeader tuple, HeapTupleFreeze *frz)
{
	HeapTupleHeaderSetXmax(tuple, frz->xmax);

	if (frz->frzflags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

	if (frz->frzflags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);

	tuple->t_infomask = frz->t_infomask;
	tuple->t_infomask2 = frz->t_infomask2;
}

/*
 * heap_freeze_execute_prepared
 *
 * 代表调用方对一页上的一个或多个堆元组执行冻结。
 * 调用方传入来自 heap_prepare_freeze_tuple 的元组计划数组。
 * 调用方必须为我们设置每个计划里的 'offset'。注意我们会就地
 * 破坏性地排序调用方的元组数组，所以调用方最好已经用完它。
 *
 * 把变更记入 WAL，这样 VACUUM 稍后推进该关系的 relfrozenxid
 * 时就不会有不安全的 pg_xact 查找风险，即使随后发生硬
 * 崩溃（或从备库查询时）。我们用设置元组头部的
 * infomask 位来表示冻结，但这不应当被当成提示。
 * 见 src/backend/storage/buffer/README 中关于缓冲区访问规则的一节。
 */
void
heap_freeze_execute_prepared(Relation rel, Buffer buffer,
							 TransactionId snapshotConflictHorizon,
							 HeapTupleFreeze *tuples, int ntuples)
{
	Page		page = BufferGetPage(buffer);

	Assert(ntuples > 0);

	/*
	 * 进入临界区之前，对 xmin/xmax 的 XID 状态做健全性检查。
	 *
	 * heap_prepare_freeze_tuple 不直接做这些检查，因为
	 * pg_xact 查找相对昂贵。决定不对同一页冻结的连续 VACUUM
	 * 不应当重复做这些检查。
	 */
	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);

		/* 这里故意不依赖元组提示位 */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMIN_COMMITTED)
		{
			TransactionId xmin = HeapTupleHeaderGetRawXmin(htup);

			Assert(!HeapTupleHeaderXminFrozen(htup));
			if (unlikely(!TransactionIdDidCommit(xmin)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("uncommitted xmin %u needs to be frozen",
										 xmin)));
		}

		/*
		 * 有崩溃期间仍在进行的事务所留下的 XID 时，
		 * TransactionIdDidAbort 不会可靠工作，
		 * 因此我们只能检查 xmax 没有提交
		 */
		if (frz->checkflags & HEAP_FREEZE_CHECK_XMAX_ABORTED)
		{
			TransactionId xmax = HeapTupleHeaderGetRawXmax(htup);

			Assert(TransactionIdIsNormal(xmax));
			if (unlikely(TransactionIdDidCommit(xmax)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("cannot freeze committed xmax %u",
										 xmax)));
		}
	}

	START_CRIT_SECTION();

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;
		ItemId		itemid = PageGetItemId(page, frz->offset);
		HeapTupleHeader htup;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		heap_execute_freeze_tuple(htup, frz);
	}

	MarkBufferDirty(buffer);

	/* 现在若有必要，把冻结记入 WAL */
	if (RelationNeedsWAL(rel))
	{
		xl_heap_freeze_plan plans[MaxHeapTuplesPerPage];
		OffsetNumber offsets[MaxHeapTuplesPerPage];
		int			nplans;
		xl_heap_freeze_page xlrec;
		XLogRecPtr	recptr;

		/* 准备去重后的表示，供 WAL 记录使用 */
		nplans = heap_log_freeze_plan(tuples, ntuples, plans, offsets);

		xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
		xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(rel);
		xlrec.nplans = nplans;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapFreezePage);

		/*
		 * 冻结计划数组和偏移数组实际上不在
		 * 缓冲区里，但假装它们在。当 XLogInsert 存储
		 * 整个缓冲区时，这些数组就不必再存。
		 */
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) plans,
							nplans * sizeof(xl_heap_freeze_plan));
		XLogRegisterBufData(0, (char *) offsets,
							ntuples * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_FREEZE_PAGE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * 用于为 XLOG_HEAP2_FREEZE_PAGE 冻结计划去重的比较器
 */
static int
heap_log_freeze_cmp(const void *arg1, const void *arg2)
{
	HeapTupleFreeze *frz1 = (HeapTupleFreeze *) arg1;
	HeapTupleFreeze *frz2 = (HeapTupleFreeze *) arg2;

	if (frz1->xmax < frz2->xmax)
		return -1;
	else if (frz1->xmax > frz2->xmax)
		return 1;

	if (frz1->t_infomask2 < frz2->t_infomask2)
		return -1;
	else if (frz1->t_infomask2 > frz2->t_infomask2)
		return 1;

	if (frz1->t_infomask < frz2->t_infomask)
		return -1;
	else if (frz1->t_infomask > frz2->t_infomask)
		return 1;

	if (frz1->frzflags < frz2->frzflags)
		return -1;
	else if (frz1->frzflags > frz2->frzflags)
		return 1;

	/*
	 * heap_log_freeze_eq 会认为这些按元组的计划相等。
	 * （因此这些元组将共享一份规范冻结计划。）
	 *
	 * 我们用页偏移号做决胜，使每份冻结计划的页
	 * 偏移号数组各自有序。（并非必要，但求整齐。）
	 */
	if (frz1->offset < frz2->offset)
		return -1;
	else if (frz1->offset > frz2->offset)
		return 1;

	Assert(false);
	return 0;
}

/*
 * 比较描述冻结元组所需动作的字段与调用方
 * 当前打开的计划。若全部匹配，则该 frz 元组计划等价于
 * 调用方的计划。
 */
static inline bool
heap_log_freeze_eq(xl_heap_freeze_plan *plan, HeapTupleFreeze *frz)
{
	if (plan->xmax == frz->xmax &&
		plan->t_infomask2 == frz->t_infomask2 &&
		plan->t_infomask == frz->t_infomask &&
		plan->frzflags == frz->frzflags)
		return true;

	/* 调用方必须为 frz 再次调用 heap_log_freeze_new_plan */
	return false;
}

/*
 * 用元组级动作初始化并开始一份新计划。重做期间至少有一个元组
 * 会有调用方计划所描述的冻结步骤。
 */
static inline void
heap_log_freeze_new_plan(xl_heap_freeze_plan *plan, HeapTupleFreeze *frz)
{
	plan->xmax = frz->xmax;
	plan->t_infomask2 = frz->t_infomask2;
	plan->t_infomask = frz->t_infomask;
	plan->frzflags = frz->frzflags;
	plan->ntuples = 1;			/* 目前 */
}

/*
 * 为基于元组的冻结计划去重，使每一组不同的
 * 处理步骤在 XLOG_HEAP2_FREEZE_PAGE 记录里只存一次。
 * 在冻结的原始执行期间调用（针对记日志的关系）。
 *
 * 返回值是为调用方设置在 *plans_out 里的计划数。同时把
 * 偏移号数组写进给调用方的 *offsets_out 输出参数
 * （实际上每份冻结计划有一个数组，但我们的调用方
 * 眼下不必关心这一点）。
 */
static int
heap_log_freeze_plan(HeapTupleFreeze *tuples, int ntuples,
					 xl_heap_freeze_plan *plans_out,
					 OffsetNumber *offsets_out)
{
	int			nplans = 0;

	/* 按去重所需的顺序排序基于元组的冻结计划 */
	qsort(tuples, ntuples, sizeof(HeapTupleFreeze), heap_log_freeze_cmp);

	for (int i = 0; i < ntuples; i++)
	{
		HeapTupleFreeze *frz = tuples + i;

		if (i == 0)
		{
			/* 从第一个元组开始的新规范冻结计划 */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}
		else if (heap_log_freeze_eq(plans_out, frz))
		{
			/* 元组匹配当前打开的规范计划——把它包括进去 */
			Assert(offsets_out[i - 1] < frz->offset);
			plans_out->ntuples++;
		}
		else
		{
			/* 元组不匹配当前计划——现在这份计划结束了 */
			plans_out++;

			/* 从该元组开始的新规范冻结计划 */
			heap_log_freeze_new_plan(plans_out, frz);
			nplans++;
		}

		/*
		 * 顺便把页偏移号存进专用缓冲区。
		 *
		 * 重做例程依靠记录的偏移号数组按冻结计划分组
		 * 偏移号。每个分组内部的排序
		 * 是偏移号升序，只是为了整齐。
		 */
		offsets_out[i] = frz->offset;
	}

	Assert(nplans > 0 && nplans <= ntuples);

	return nplans;
}

/*
 * heap_freeze_tuple
 *		就地冻结元组，不记 WAL。
 *
 * 对 CLUSTER 这类自己做 WAL 记录的调用方有用。
 */
bool
heap_freeze_tuple(HeapTupleHeader tuple,
				  TransactionId relfrozenxid, TransactionId relminmxid,
				  TransactionId FreezeLimit, TransactionId MultiXactCutoff)
{
	HeapTupleFreeze frz;
	bool		do_freeze;
	bool		totally_frozen;
	struct VacuumCutoffs cutoffs;
	HeapPageFreeze pagefrz;

	cutoffs.relfrozenxid = relfrozenxid;
	cutoffs.relminmxid = relminmxid;
	cutoffs.OldestXmin = FreezeLimit;
	cutoffs.OldestMxact = MultiXactCutoff;
	cutoffs.FreezeLimit = FreezeLimit;
	cutoffs.MultiXactCutoff = MultiXactCutoff;

	pagefrz.freeze_required = true;
	pagefrz.FreezePageRelfrozenXid = FreezeLimit;
	pagefrz.FreezePageRelminMxid = MultiXactCutoff;
	pagefrz.NoFreezePageRelfrozenXid = FreezeLimit;
	pagefrz.NoFreezePageRelminMxid = MultiXactCutoff;

	do_freeze = heap_prepare_freeze_tuple(tuple, &cutoffs,
										  &pagefrz, &frz, &totally_frozen);

	/*
	 * 注意因为这不是记 WAL 的操作，我们不必
	 * 在冻结记录里填偏移。
	 */

	if (do_freeze)
		heap_execute_freeze_tuple(tuple, &frz);
	return do_freeze;
}

/*
 * 对给定的 MultiXactId，返回应当设置在
 * 元组 infomask 里的提示位。
 *
 * 通常应当对刚刚创建的 multixact 调用它，因此
 * 它在我们的本地缓存里，GetMembers 调用很快。
 */
static void
GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
					   uint16 *new_infomask2)
{
	int			nmembers;
	MultiXactMember *members;
	int			i;
	uint16		bits = HEAP_XMAX_IS_MULTI;
	uint16		bits2 = 0;
	bool		has_update = false;
	LockTupleMode strongest = LockTupleKeyShare;

	/*
	 * 我们只对刚刚创建的 multi 使用它，因此它们不可能是
	 * pg_upgrade 之前的值。
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	for (i = 0; i < nmembers; i++)
	{
		LockTupleMode mode;

		/*
		 * 记住该 multixact 任何成员所持有的
		 * 最强锁模式。
		 */
		mode = TUPLOCK_from_mxstatus(members[i].status);
		if (mode > strongest)
			strongest = mode;

		/* 看看还需要哪些其他位 */
		switch (members[i].status)
		{
			case MultiXactStatusForKeyShare:
			case MultiXactStatusForShare:
			case MultiXactStatusForNoKeyUpdate:
				break;

			case MultiXactStatusForUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				break;

			case MultiXactStatusNoKeyUpdate:
				has_update = true;
				break;

			case MultiXactStatusUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				has_update = true;
				break;
		}
	}

	if (strongest == LockTupleExclusive ||
		strongest == LockTupleNoKeyExclusive)
		bits |= HEAP_XMAX_EXCL_LOCK;
	else if (strongest == LockTupleShare)
		bits |= HEAP_XMAX_SHR_LOCK;
	else if (strongest == LockTupleKeyShare)
		bits |= HEAP_XMAX_KEYSHR_LOCK;

	if (!has_update)
		bits |= HEAP_XMAX_LOCK_ONLY;

	if (nmembers > 0)
		pfree(members);

	*new_infomask = bits;
	*new_infomask2 = bits2;
}

/*
 * MultiXactIdGetUpdateXid
 *
 * 给定一个 multixact Xmax 及对应的 infomask，且没有设置
 * HEAP_XMAX_LOCK_ONLY 位，取得并返回更新
 * 事务的 Xid。
 *
 *
 * 若有必要，调用方应去检查更新事务的状态。
 */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
	TransactionId update_xact = InvalidTransactionId;
	MultiXactMember *members;
	int			nmembers;

	Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	/*
	 * 既然知道没有设置 LOCK_ONLY 位，这就不可能是
	 * pg_upgrade 之前的 multi。
	 */
	nmembers = GetMultiXactIdMembers(xmax, &members, false, false);

	if (nmembers > 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			/* 忽略加锁者 */
			if (!ISUPDATE_from_mxstatus(members[i].status))
				continue;

			/* 最多只能有一个更新者 */
			Assert(update_xact == InvalidTransactionId);
			update_xact = members[i].xid;
#ifndef USE_ASSERT_CHECKING

			/*
			 * 在启用 assert 的构建里，走完整个数组以确保
			 * 没有其他更新者。
			 */
			break;
#endif
		}

		pfree(members);
	}

	return update_xact;
}

/*
 * HeapTupleGetUpdateXid
 *		同上，但使用 HeapTupleHeader
 *
 * 另见 HeapTupleHeaderGetUpdateXid，它可以在不事先
 * 检查提示位的情况下使用。
 */
TransactionId
HeapTupleGetUpdateXid(HeapTupleHeader tuple)
{
	return MultiXactIdGetUpdateXid(HeapTupleHeaderGetRawXmax(tuple),
								   tuple->t_infomask);
}

/*
 * 给定的 multixact 是否与当前事务以给定强度
 * 获取元组锁相冲突？
 *
 * 传入的 infomask 与元组头中给定的 multixact 配对。
 *
 * 若 current_is_member 不是 NULL，且当前
 * 事务是给定 multixact 的成员，则把它设为 'true'。
 */
static bool
DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
						LockTupleMode lockmode, bool *current_is_member)
{
	int			nmembers;
	MultiXactMember *members;
	bool		result = false;
	LOCKMODE	wanted = tupleLockExtraInfo[lockmode].hwlock;

	if (HEAP_LOCKED_UPGRADED(infomask))
		return false;

	nmembers = GetMultiXactIdMembers(multi, &members, false,
									 HEAP_XMAX_IS_LOCKED_ONLY(infomask));
	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid;
			LOCKMODE	memlockmode;

			if (result && (current_is_member == NULL || *current_is_member))
				break;

			memlockmode = LOCKMODE_from_mxstatus(members[i].status);

			/* 忽略来自当前事务的成员（但跟踪它们是否存在） */
			memxid = members[i].xid;
			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				if (current_is_member != NULL)
					*current_is_member = true;
				continue;
			}
			else if (result)
				continue;

			/* 忽略与我们想要的锁不冲突的成员 */
			if (!DoLockModesConflict(memlockmode, wanted))
				continue;

			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				/* 忽略已中止的更新者 */
				if (TransactionIdDidAbort(memxid))
					continue;
			}
			else
			{
				/* 忽略不再进行中的仅加锁者 */
				if (!TransactionIdIsInProgress(memxid))
					continue;
			}

			/*
			 * 剩下的要么是与我们想要的锁冲突的仍存活加锁者，
			 * 以及未中止的更新者。它们与我们想要的东西
			 * 冲突。准备返回 true，但继续往下
			 * 在 multixact 成员中查找当前事务，
			 * 若需要的话。
			 */
			result = true;
		}
		pfree(members);
	}

	return result;
}

/*
 * Do_MultiXactIdWait
 *		下面两个函数的实际实现。
 *
 * 'multi'、'status' 和 'infomask' 指出要在什么上面睡眠（status
 * 用来确保我们只在冲突成员上睡眠，infomask 则
 * 用于在它是仅含锁的 multi 时优化 multixact 访问）；'nowait'
 * 指出是否使用条件锁获取，以便调用方在
 * 锁不可用时失败。'rel'、'ctid' 和 'oper' 用来设置
 * 错误消息的上下文信息。'remaining' 若不是 NULL，接收
 * 仍在运行的成员数，包括我们自己事务的任何（未中止）
 * 子事务。
 *
 * 我们通过对每个成员用 XactLockTableWait 睡眠来做到这一点。不过，
 * 属于当前后端的任何成员都*不*等待；
 * 那不仅无用，还会导致 XactLockTableWait 内部
 * Assert 失败。等到这里返回时，可以确定所有
 * *其他后端的*、曾是该 MultiXactId 成员且
 * 与所请求状态冲突的事务都已死（而且不可能有新的
 * 被加进来，因为不允许向已有的
 * MultiXactId 添加成员）。
 *
 * 但等到我们睡完，别人可能已经改了所在元组的 Xmax，
 * 所以调用方需要以某种方式对我们迭代。
 *
 * 注意若我们返回 false，剩余成员数
 * 不可信。
 */
static bool
Do_MultiXactIdWait(MultiXactId multi, MultiXactStatus status,
				   uint16 infomask, bool nowait,
				   Relation rel, ItemPointer ctid, XLTW_Oper oper,
				   int *remaining)
{
	bool		result = true;
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	/* 对 pg_upgrade 之前的元组，完全不必睡眠 */
	nmembers = HEAP_LOCKED_UPGRADED(infomask) ? -1 :
		GetMultiXactIdMembers(multi, &members, false,
							  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid = members[i].xid;
			MultiXactStatus memstatus = members[i].status;

			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				remain++;
				continue;
			}

			if (!DoLockModesConflict(LOCKMODE_from_mxstatus(memstatus),
									 LOCKMODE_from_mxstatus(status)))
			{
				if (remaining && TransactionIdIsInProgress(memxid))
					remain++;
				continue;
			}

			/*
			 * 该成员与我们的 multi 冲突，因此我们必须睡眠（或者
			 * 若要求避免等待，则返回失败）。
			 *
			 * 注意我们自己不设置错误上下文回调，
			 * 而是把信息往下传给 XactLockTableWait。这
			 * 看起来有点浪费，因为上下文要对 multixact 的每个成员
			 * 建立再拆除，但实际上
			 * 应当几乎察觉不到，而且避免了重复代码。
			 */
			if (nowait)
			{
				result = ConditionalXactLockTableWait(memxid);
				if (!result)
					break;
			}
			else
				XactLockTableWait(memxid, rel, ctid, oper);
		}

		pfree(members);
	}

	if (remaining)
		*remaining = remain;

	return result;
}

/*
 * MultiXactIdWait
 *		在一个 MultiXactId 上睡眠。
 *
 * 等到我们睡完，别人可能已经改了所在元组的 Xmax，
 * 所以调用方需要以某种方式对我们迭代。
 *
 * 我们（在 *remaining 里，若不是 NULL）返回仍在运行的成员数，
 * 包括我们自己事务的任何（未中止）子事务。
 */
static void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
				Relation rel, ItemPointer ctid, XLTW_Oper oper,
				int *remaining)
{
	(void) Do_MultiXactIdWait(multi, status, infomask, false,
							  rel, ctid, oper, remaining);
}

/*
 * ConditionalMultiXactIdWait
 *		同上，但只有能不阻塞地取得锁时才加锁。
 *
 * 等到我们睡完，别人可能已经改了所在元组的 Xmax，
 * 所以调用方需要以某种方式对我们迭代。
 *
 * 若该 multixact 现在已经全部消失，返回 true。若某些
 * 事务可能仍在运行，返回 false。
 *
 * 我们（在 *remaining 里，若不是 NULL）返回仍在运行的成员数，
 * 包括我们自己事务的任何（未中止）子事务。
 */
static bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, Relation rel, int *remaining)
{
	return Do_MultiXactIdWait(multi, status, infomask, true,
							  rel, NULL, XLTW_None, remaining);
}

/*
 * heap_tuple_needs_eventual_freeze
 *
 * 检查元组的任何 XID 字段（xmin、xmax、xvac）
 * 最终是否需要冻结（若元组没有先被剪枝移除）。
 */
bool
heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple)
{
	TransactionId xid;

	/*
	 * 若 xmin 是普通事务 ID，该元组肯定没有
	 * 冻结。
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
		return true;

	/*
	 * 若 xmax 是有效事务或 multixact，该元组也没有冻结。
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId multi;

		multi = HeapTupleHeaderGetRawXmax(tuple);
		if (MultiXactIdIsValid(multi))
			return true;
	}
	else
	{
		xid = HeapTupleHeaderGetRawXmax(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	return false;
}

/*
 * heap_tuple_should_freeze
 *
 * 返回值指出姊妹函数 heap_prepare_freeze_tuple 会
 * （或应当）强制冻结包含调用方元组的堆页。
 * 元组头中小于 FreezeLimit/MultiXactCutoff 的 XID/MXID 会触发冻结。
 * 这包括（xmin、xmax、xvac）字段，以及 MultiXact 成员 XID。
 *
 * *NoFreezePageRelfrozenXid 和 *NoFreezePageRelminMxid 输入/输出
 * 参数帮助 VACUUM 跟踪关系中剩余的现存最旧 XID/MXID。
 * 我们的工作假设是调用方不会决定冻结该元组。
 * 只有在调用方完全确定不冻结该元组/页之后，
 * 才由它把自己的顶层跟踪器往回调。
 */
bool
heap_tuple_should_freeze(HeapTupleHeader tuple,
						 const struct VacuumCutoffs *cutoffs,
						 TransactionId *NoFreezePageRelfrozenXid,
						 MultiXactId *NoFreezePageRelminMxid)
{
	TransactionId xid;
	MultiXactId multi;
	bool		freeze = false;

	/* 先处理 xmin */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}

	/* 现在处理 xmax */
	xid = InvalidTransactionId;
	multi = InvalidMultiXactId;
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
		multi = HeapTupleHeaderGetRawXmax(tuple);
	else
		xid = HeapTupleHeaderGetRawXmax(tuple);

	if (TransactionIdIsNormal(xid))
	{
		Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
		/* xmax 是非永久 XID */
		if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
			*NoFreezePageRelfrozenXid = xid;
		if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
			freeze = true;
	}
	else if (!MultiXactIdIsValid(multi))
	{
		/* xmax 是永久 XID，或无效的 MultiXactId/XID */
	}
	else if (HEAP_LOCKED_UPGRADED(tuple->t_infomask))
	{
		/* xmax 是经 pg_upgrade 升级来的 MultiXact，它无法带有更新者 XID */
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		/* heap_prepare_freeze_tuple 总是冻结经 pg_upgrade 升级来的 xmax */
		freeze = true;
	}
	else
	{
		/* xmax 是可能带有更新者 XID 的 MultiXactId */
		MultiXactMember *members;
		int			nmembers;

		Assert(MultiXactIdPrecedesOrEquals(cutoffs->relminmxid, multi));
		if (MultiXactIdPrecedes(multi, *NoFreezePageRelminMxid))
			*NoFreezePageRelminMxid = multi;
		if (MultiXactIdPrecedes(multi, cutoffs->MultiXactCutoff))
			freeze = true;

		/* 需要检查该 mxact 的任何成员是否过旧 */
		nmembers = GetMultiXactIdMembers(multi, &members, false,
										 HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		for (int i = 0; i < nmembers; i++)
		{
			xid = members[i].xid;
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			if (TransactionIdPrecedes(xid, cutoffs->FreezeLimit))
				freeze = true;
		}
		if (nmembers > 0)
			pfree(members);
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
		{
			Assert(TransactionIdPrecedesOrEquals(cutoffs->relfrozenxid, xid));
			if (TransactionIdPrecedes(xid, *NoFreezePageRelfrozenXid))
				*NoFreezePageRelfrozenXid = xid;
			/* heap_prepare_freeze_tuple 强制冻结 xvac */
			freeze = true;
		}
	}

	return freeze;
}

/*
 * 用 'tuple' 中包含的任何已提交 XID 把值往前调，
 * 为调用方维护 snapshotConflictHorizon。'tuple' 是调用方正在
 * 物理移除的过时堆元组，例如通过 HOT 剪枝
 * 或索引删除。
 *
 * 调用方必须把该值初始化为 InvalidTransactionId，这
 * 通常被解释为"肯定不需要恢复冲突"。
 * 最终值必须反映调用方将通过正在进行的剪枝/删除操作
 * 物理移除的所有堆元组（或移除指向它们的 TID 引用）。
 * 重做例程重放调用方的操作时，会把最终值（取自
 * 调用方的 WAL 记录）传给 ResolveRecoveryConflictWithSnapshot()。
 */
void
HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
									  TransactionId *snapshotConflictHorizon)
{
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetUpdateXid(tuple);
	TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

	if (tuple->t_infomask & HEAP_MOVED)
	{
		if (TransactionIdPrecedes(*snapshotConflictHorizon, xvac))
			*snapshotConflictHorizon = xvac;
	}

	/*
	 * 忽略由已中止事务插入的元组，或该元组被
	 * 插入它的事务更新/删除的情形。
	 *
	 * 寻找已提交提示位；若没有设置 xmin 位，则检查 CLOG。
	 */
	if (HeapTupleHeaderXminCommitted(tuple) ||
		(!HeapTupleHeaderXminInvalid(tuple) && TransactionIdDidCommit(xmin)))
	{
		if (xmax != xmin &&
			TransactionIdFollows(xmax, *snapshotConflictHorizon))
			*snapshotConflictHorizon = xmax;
	}
}

#ifdef USE_PREFETCH
/*
 * heap_index_delete_tuples 的辅助函数。为
 * prefetch_count 个缓冲区发出预取请求。prefetch_state 跟踪所有我们
 * 可以预取的缓冲区，以及哪些已经预取过；每次调用本
 * 函数都从上次调用停下的地方继续。
 *
 * 注意：我们预期 deltids 数组按把 TID
 * 按堆块分组的顺序排序，每个块的全部 TID 一起出现在恰好
 * 一组里。
 */
static void
index_delete_prefetch_buffer(Relation rel,
							 IndexDeletePrefetchState *prefetch_state,
							 int prefetch_count)
{
	BlockNumber cur_hblkno = prefetch_state->cur_hblkno;
	int			count = 0;
	int			i;
	int			ndeltids = prefetch_state->ndeltids;
	TM_IndexDelete *deltids = prefetch_state->deltids;

	for (i = prefetch_state->next_item;
		 i < ndeltids && count < prefetch_count;
		 i++)
	{
		ItemPointer htid = &deltids[i].tid;

		if (cur_hblkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != cur_hblkno)
		{
			cur_hblkno = ItemPointerGetBlockNumber(htid);
			PrefetchBuffer(rel, MAIN_FORKNUM, cur_hblkno);
			count++;
		}
	}

	/*
	 * 保存预取位置，以便下次可以从该
	 * 位置继续。
	 */
	prefetch_state->next_item = i;
	prefetch_state->cur_hblkno = cur_hblkno;
}
#endif

/*
 * heap_index_delete_tuples 的辅助函数。检查索引 AM 调用方索引页中
 * 涉及无效 TID 的索引损坏。
 *
 * 这里是做这些检查的理想位置。索引 AM 必须对包含我们在此检查的 TID 的索引页
 * 持有缓冲区锁，因此我们完全
 * 不必担心并发的 VACUUM。当 htid 直接指向 LP_UNUSED 项或
 * 堆内元组时，我们可以确定索引已损坏，
 * 标准索引扫描期间不会出现这种情况。
 */
static inline void
index_delete_check_htid(TM_IndexDeleteOp *delstate,
						Page page, OffsetNumber maxoff,
						ItemPointer htid, TM_IndexStatus *istatus)
{
	OffsetNumber indexpagehoffnum = ItemPointerGetOffsetNumber(htid);
	ItemId		iid;

	Assert(OffsetNumberIsValid(istatus->idxoffnum));

	if (unlikely(indexpagehoffnum > maxoff))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points past end of heap page line pointer array at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	iid = PageGetItemId(page, indexpagehoffnum);
	if (unlikely(!ItemIdIsUsed(iid)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("heap tid from index tuple (%u,%u) points to unused heap page item at offset %u of block %u in index \"%s\"",
								 ItemPointerGetBlockNumber(htid),
								 indexpagehoffnum,
								 istatus->idxoffnum, delstate->iblknum,
								 RelationGetRelationName(delstate->irel))));

	if (ItemIdHasStorage(iid))
	{
		HeapTupleHeader htup;

		Assert(ItemIdIsNormal(iid));
		htup = (HeapTupleHeader) PageGetItem(page, iid);

		if (unlikely(HeapTupleHeaderIsHeapOnly(htup)))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("heap tid from index tuple (%u,%u) points to heap-only tuple at offset %u of block %u in index \"%s\"",
									 ItemPointerGetBlockNumber(htid),
									 indexpagehoffnum,
									 istatus->idxoffnum, delstate->iblknum,
									 RelationGetRelationName(delstate->irel))));
	}
}

/*
 * tableam 的 index_delete_tuples 接口的 heapam 实现。
 *
 * 索引 AM 在删除索引元组期间调用这个辅助函数。
 * 关于这里实现的接口以及一般工作原理，见 tableam 头注释
 * 的说明。注意这里的每次调用要么是
 * 简单索引删除调用，要么是自底向上索引删除调用。
 *
 * 这可能产生相当多的 I/O，因为我们可能从一个索引块
 * 删除数百个元组。为了在一定程度上分摊该
 * 代价，这里使用预取，并合并对
 * 同一堆块的重复访问。
 */
TransactionId
heap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* 初始假设是先前的剪枝已经处理了冲突 */
	TransactionId snapshotConflictHorizon = InvalidTransactionId;
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	OffsetNumber maxoff = InvalidOffsetNumber;
	TransactionId priorXmax;
#ifdef USE_PREFETCH
	IndexDeletePrefetchState prefetch_state;
	int			prefetch_distance;
#endif
	SnapshotData SnapshotNonVacuumable;
	int			finalndeltids = 0,
				nblocksaccessed = 0;

	/* 只在自底向上索引删除情形中使用的状态 */
	int			nblocksfavorable = 0;
	int			curtargetfreespace = delstate->bottomupfreespace,
				lastfreespace = 0,
				actualfreespace = 0;
	bool		bottomup_final_block = false;

	InitNonVacuumableSnapshot(SnapshotNonVacuumable, GlobalVisTestFor(rel));

	/* 按 TID 排序调用方的 deltids 数组以便进一步处理 */
	index_delete_sort(delstate);

	/*
	 * 自底向上情形：按最有希望找到大量有希望 TID 的位置
	 * 重新排序 deltids 数组，并决定
	 * 排序后数组开头有多少块应当视为
	 * 有利。这也会缩小 deltids 数组，以便
	 * 事先去掉完全不利的块。
	 */
	if (delstate->bottomup)
		nblocksfavorable = bottomup_sort_and_shrink(delstate);

#ifdef USE_PREFETCH
	/* 初始化预取状态。 */
	prefetch_state.cur_hblkno = InvalidBlockNumber;
	prefetch_state.next_item = 0;
	prefetch_state.ndeltids = delstate->ndeltids;
	prefetch_state.deltids = delstate->deltids;

	/*
	 * 决定我们将试图维持的预取距离。
	 *
	 * 由于调用方在 rel 的某处持有缓冲区锁，在调用做
	 * syscache 查找的代码之前，我们最好确认
	 * 那不是系统目录关系，以免有死锁风险。
	 */
	if (IsCatalogRelation(rel))
		prefetch_distance = maintenance_io_concurrency;
	else
		prefetch_distance =
			get_tablespace_maintenance_io_concurrency(rel->rd_rel->reltablespace);

	/* 为自底向上删除的调用方限制初始预取距离上限 */
	if (delstate->bottomup)
	{
		Assert(nblocksfavorable >= 1);
		Assert(nblocksfavorable <= BOTTOMUP_MAX_NBLOCKS);
		prefetch_distance = Min(prefetch_distance, nblocksfavorable);
	}

	/* 开始预取。 */
	index_delete_prefetch_buffer(rel, &prefetch_state, prefetch_distance);
#endif

	/* 遍历 deltids，决定删除哪些，检查它们的视界 */
	Assert(delstate->ndeltids > 0);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		OffsetNumber offnum;

		/*
		 * 读取缓冲区，并且每次遇到新块时
		 * 执行所需的额外步骤。
		 * 若与上一个 htid 是同一块，则避免再次获取。
		 */
		if (blkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != blkno)
		{
			/*
			 * 先考虑为自底向上索引删除的调用方
			 * 提前放弃。（之后才预取下下一块，
			 * 那时已经清楚我们至少会访问
			 * 队列中的下一块。）
			 *
			 * 有时第一块为自底向上调用方释放了如此多的空间，
			 * 删除过程可以不再访问任何更多块就结束。
			 * 不过通常每次自底向上删除操作
			 * 需要访问 2 或 3 个块。
			 */
			if (delstate->bottomup)
			{
				/*
				 * 我们常常允许调用方再删除另外几个项，
				 * 这些项是在调用方的空间目标已经满足之后
				 * 才到达的。
				 * 访问该页的代价那时已经付过，因此把它做完
				 * 是合理的。发生这种情况时，我们在这里把一切收尾
				 * （结束整个自底向上删除操作，
				 * 不必白白付出访问更多
				 * 块的代价）。
				 */
				if (bottomup_final_block)
					break;

				/*
				 * 若处理刚完成的那一页之后，没能让调用方释放任何
				 * 额外空间，就放弃。
				 * 这条规则是我们把自底向上删除的代价
				 * 保持在可控范围内的主要方式。
				 */
				if (nblocksaccessed >= 1 && actualfreespace == lastfreespace)
					break;
				lastfreespace = actualfreespace;	/* 供下次使用 */

				/*
				 * 删除操作（自底向上）肯定会
				 * 访问队列中的下一块。
				 * 现在就为此做准备。
				 *
				 * 衰减目标空闲空间，以免在边缘情形上
				 * 拖太久。（空间目标只有在它让我们认出不必
				 * 访问超过 1 或 2 个块就能满足调用方时才真正
				 * 有帮助，这要归功于
				 * 适宜的工作负载特征。）
				 *
				 * 遇到连续块时我们会更有耐心一些：
				 * 这些块被视为有利块。只有当队列中的下一块
				 * 不是有利/连续块时才应用衰减过程。这不是对
				 * 一般规则的例外；我们仍然坚持每个被访问的块至少找到
				 * 一个可删除项。关于有利块以及一般的堆块局部性，
				 * 理论的全部细节见
				 * bottomup_nblocksfavorable()。
				 *
				 * 注意：队列中的第一块始终被视为
				 * 有利块，因此最早可能应用衰减的时点
				 * 就在我们访问队列中第二块之前。
				 * Assert() 为我们验证这一点。
				 */
				Assert(nblocksaccessed > 0 || nblocksfavorable > 0);
				if (nblocksfavorable > 0)
					nblocksfavorable--;
				else
					curtargetfreespace /= 2;
			}

			/* 释放旧缓冲区 */
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);

			blkno = ItemPointerGetBlockNumber(htid);
			buf = ReadBuffer(rel, blkno);
			nblocksaccessed++;
			Assert(!delstate->bottomup ||
				   nblocksaccessed <= BOTTOMUP_MAX_NBLOCKS);

#ifdef USE_PREFETCH

			/*
			 * 为维持预取距离，我们每读一页
			 * 就再预取一页。
			 */
			index_delete_prefetch_buffer(rel, &prefetch_state, 1);
#endif

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			page = BufferGetPage(buf);
			maxoff = PageGetMaxOffsetNumber(page);
		}

		/*
		 * 顺便检测这种索引损坏：索引页上的
		 * TID 指向堆中绝无可能
		 * 正确的位置。我们只对来自调用方索引页的实际 TID 做这件事
		 * （不是顺着 HOT 链走到的项）。
		 */
		index_delete_check_htid(delstate, page, maxoff, htid, istatus);

		if (istatus->knowndeletable)
			Assert(!delstate->bottomup && !istatus->promising);
		else
		{
			ItemPointerData tmp = *htid;
			HeapTupleData heapTuple;

			/* 这条 HOT 链上有没有任何元组不可被清理？ */
			if (heap_hot_search_buffer(&tmp, rel, buf, &SnapshotNonVacuumable,
									   &heapTuple, NULL, true))
				continue;		/* 无法删除该项 */

			/* 调用方将删除，因为整条 HOT 链都可被清理 */
			istatus->knowndeletable = true;

			/* 为自底向上删除情形维护索引空闲空间信息 */
			if (delstate->bottomup)
			{
				Assert(istatus->freespace > 0);
				actualfreespace += istatus->freespace;
				if (actualfreespace >= curtargetfreespace)
					bottomup_final_block = true;
			}
		}

		/*
		 * 用堆元组头推进当前值，为整个删除操作维护
		 * snapshotConflictHorizon。这
		 * 大致基于剪枝 HOT 链的逻辑。
		 */
		offnum = ItemPointerGetOffsetNumber(htid);
		priorXmax = InvalidTransactionId;	/* 无法检查第一个 XMIN */
		for (;;)
		{
			ItemId		lp;
			HeapTupleHeader htup;

			/* 健全性检查（纯粹出于谨慎） */
			if (offnum < FirstOffsetNumber)
				break;

			/*
			 * 偏移超出页的行指针数组末尾是可能的，
			 * 当该数组被截断时
			 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, offnum);
			if (ItemIdIsRedirected(lp))
			{
				offnum = ItemIdGetRedirect(lp);
				continue;
			}

			/*
			 * 我们会经常遇到 LP_DEAD 行指针（尤其是调用方事先
			 * 标成 knowndeletable 的项）。把我们带到
			 * LP_DEAD 项的 htid 不会检查任何堆
			 * 元组头。这没问题，因为最初把行指针变成 LP_DEAD 的
			 * 先前剪枝操作
			 * 在生成自己的 snapshotConflictHorizon 值时，
			 * 必定已经把原元组头考虑在内。
			 *
			 * 像这样依赖 XLOG_HEAP2_PRUNE 记录，与
			 * 索引清理在所有情形中使用的策略相同。索引 VACUUM
			 * 的 WAL 记录甚至没有自己的 snapshotConflictHorizon 字段，
			 * 原因就在这里。
			 */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

			/*
			 * 若有先前的 XMAX，对照它检查元组的 XMIN
			 */
			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
				break;

			HeapTupleHeaderAdvanceConflictHorizon(htup,
												  &snapshotConflictHorizon);

			/*
			 * 若元组不是 HOT 更新的，那么我们就到了这条
			 * HOT 链的末尾。不必访问同一更新
			 * 链上更后面的元组（它们有自己的索引项）——直接转到
			 * 索引 AM 调用方的下一个 htid。
			 */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* 前进到下一个 HOT 链成员 */
			Assert(ItemPointerGetBlockNumber(&htup->t_ctid) == blkno);
			offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}

		/* 允许为调用方进一步/最终缩小 deltids */
		finalndeltids = i + 1;
	}

	UnlockReleaseBuffer(buf);

	/*
	 * 缩小 deltids 数组，排除末尾不可删除的项。这
	 * 不只是小优化。对自底向上调用方，最终 deltids 数组大小可能
	 * 为零。索引 AM 被明确允许在可删除项总数为零的所有情形中
	 * 依赖 ndeltids 为零。
	 */
	Assert(finalndeltids > 0 || delstate->bottomup);
	delstate->ndeltids = finalndeltids;

	return snapshotConflictHorizon;
}

/*
 * 供 index_delete_sort() 使用的专用可内联比较函数
 */
static inline int
index_delete_sort_cmp(TM_IndexDelete *deltid1, TM_IndexDelete *deltid2)
{
	ItemPointer tid1 = &deltid1->tid;
	ItemPointer tid2 = &deltid2->tid;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(tid1);
		BlockNumber blk2 = ItemPointerGetBlockNumber(tid2);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(tid1);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(tid2);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	Assert(false);

	return 0;
}

/*
 * 按 TID 排序 delstate 中的 deltids 数组。这为
 * heap_index_delete_tuples() 的进一步处理做准备。
 *
 * 在某些工作负载下，这个操作会明显消耗 CPU 周期，
 * 因此我们费心做特化/微优化。
 * 这里用希尔排序，因为它容易特化，编译后
 * 指令相对少，并且对预排序的输入/子集自适应
 * （这里很典型）。
 */
static void
index_delete_sort(TM_IndexDeleteOp *delstate)
{
	TM_IndexDelete *deltids = delstate->deltids;
	int			ndeltids = delstate->ndeltids;
	int			low = 0;

	/*
	 * 希尔排序的间隔序列（取自 Sedgewick-Incerpi 论文）。
	 *
	 * 这个实现对大约 4500 以内的数组大小很快。这覆盖
	 * 所有受支持的 BLCKSZ 值。
	 */
	const int	gaps[9] = {1968, 861, 336, 112, 48, 21, 7, 3, 1};

	/* 改这里的任何东西之前请仔细想——让交换保持廉价 */
	StaticAssertDecl(sizeof(TM_IndexDelete) <= 8,
					 "element size exceeds 8 bytes");

	for (int g = 0; g < lengthof(gaps); g++)
	{
		for (int hi = gaps[g], i = low + hi; i < ndeltids; i++)
		{
			TM_IndexDelete d = deltids[i];
			int			j = i;

			while (j >= hi && index_delete_sort_cmp(&deltids[j - hi], &d) >= 0)
			{
				deltids[j] = deltids[j - hi];
				j -= hi;
			}
			deltids[j] = d;
		}
	}
}

/*
 * 返回一次过程中有多少块应当视为有利/连续，用于
 * 自底向上索引删除。这是从队列中第一块开始
 * 并包含该块的堆块数。
 *
 * 自底向上索引删除期间总是至少有一个有利块。
 * 最坏情况下（即堆块完全随机），
 * 队列中的第一块（唯一的有利块）可以看成
 * 只由单个块组成的退化连续块数组。
 * heap_index_delete_tuples() 会预期这一点。
 *
 * 调用方传入 blockgroups，即 deltids 最终顺序的描述，
 * 该顺序用于 heap_index_delete_tuples() 的自底向上索引删除
 * 处理。注意 deltids 此刻不必真的已经排序（调用方
 * 把 deltids 传给我们，只是为了让我们能解释 blockgroups）。
 *
 * 你可能会猜连续块的存在起不了太大作用，
 * 因为一般来说，决定我们访问哪些块的主要因素是
 * 有希望的 TID 数量，那是来自索引 AM 的固定提示。
 * 不过我们针对的并不是一般情形——实际目标是
 * 让行为适应各种各样自然出现的状况。
 * 我们应用的大多数启发式，其效果只有在
 * 总体上、随时间、跨许多_相关的_自底向上索引删除
 * 过程中才能察觉到。
 *
 * 把某些块视为有利，让 heapam 能够认出并适应
 * 这样的工作负载：自底向上索引删除期间访问的堆块可以
 * 连续访问，意思是每个新访问的块是
 * 自底向上删除刚处理完的那块的邻居（或者
 * 足够接近）。更早而不是更晚地访问更多有利
 * 块可能会更便宜（例如在这一次过程中，而不是跨一系列
 * 相关的自底向上过程）。无论如何，大概只是时间问题
 * （或进一步相关的版本更替问题），所有作为单
 * 一大批有利块一起出现的块就会被
 * _某一次_自底向上过程访问。大批有利块往往要么
 * 几乎持续出现，要么一次也不出现（这完全取决于每个索引的
 * 工作负载特征）。
 *
 * 注意 blockgroups 的排序顺序应用了二的幂分桶
 * 方案，为连续块组被
 * 一起分批创造机会，至少对那些天然适合
 * 由堆块局部性驱动的工作负载是这样。这不只是以明显方式增强
 * 自底向上堆块处理的空间局部性。它也
 * 使访问具有时间局部性，因为按堆块号排序
 * 自然倾向于使自底向上处理顺序确定。
 *
 * 考虑下面的例子，以感受时间局部性
 * 为何重要：有一个带若干索引的堆关系，每个索引
 * 都是低到中等基数。它经受持续的非 HOT 更新。
 * 更新是偏斜的（也许在主键的某一部分）。这些
 * 索引都不会被 UPDATE 语句在逻辑上修改（若会被修改，
 * 那么一开始就不会触发自底向上索引删除）。
 * 自然，每一轮新的索引元组（对每个得到
 * heap_update() 调用的堆元组）在每一个索引里都会有相同的堆 TID。
 * 由于这些索引基数低，且从不被逻辑修改，
 * 自底向上删除过程期间的 heapam 处理将以
 * 大致顺序访问堆块。访问的时间局部性来自
 * 各个索引上的自底向上删除过程在任一给定时刻
 * 行为非常相似。这把访问堆块所需的缓冲区未命中数
 * 保持在最低。
 */
static int
bottomup_nblocksfavorable(IndexDeleteCounts *blockgroups, int nblockgroups,
						  TM_IndexDelete *deltids)
{
	int64		lastblock = -1;
	int			nblocksfavorable = 0;

	Assert(nblockgroups >= 1);
	Assert(nblockgroups <= BOTTOMUP_MAX_NBLOCKS);

	/*
	 * 我们容忍将要访问的堆块只是略微偏离
	 * 物理顺序。当一对几乎连续的
	 * 块碰巧落入不同的桶时，会出现小起伏（也许只是因为
	 * npromisingtids 的微小差异，分桶方案没能
	 * 完全忽略）。我们用一个小容差
	 * 实际上忽略这些起伏。所用的精确容差有点任意，
	 * 但实践中足够好用。
	 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = deltids + group->ifirsttid;
		BlockNumber block = ItemPointerGetBlockNumber(&firstdtid->tid);

		if (lastblock != -1 &&
			((int64) block < lastblock - BOTTOMUP_TOLERANCE_NBLOCKS ||
			 (int64) block > lastblock + BOTTOMUP_TOLERANCE_NBLOCKS))
			break;

		nblocksfavorable++;
		lastblock = block;
	}

	/* 始终表明至少有 1 个有利块 */
	Assert(nblocksfavorable >= 1);

	return nblocksfavorable;
}

/*
 * bottomup_sort_and_shrink() 的 qsort 比较函数
 */
static int
bottomup_sort_and_shrink_cmp(const void *arg1, const void *arg2)
{
	const IndexDeleteCounts *group1 = (const IndexDeleteCounts *) arg1;
	const IndexDeleteCounts *group2 = (const IndexDeleteCounts *) arg2;

	/*
	 * 最重要的字段是 npromisingtids（我们把它的顺序反过来，
	 * 以便按降序排序）。
	 *
	 * 调用方应当已经把 npromisingtids 字段规范化成
	 * 二的幂值（桶）。
	 */
	if (group1->npromisingtids > group2->npromisingtids)
		return -1;
	if (group1->npromisingtids < group2->npromisingtids)
		return 1;

	/*
	 * 决胜：ntids 降序。
	 *
	 * 我们无法预期 ntids 字段是二的幂值。我们应当
	 * 表现得好像它们已经被为我们向上取整。
	 */
	if (group1->ntids != group2->ntids)
	{
		uint32		ntids1 = pg_nextpower2_32((uint32) group1->ntids);
		uint32		ntids2 = pg_nextpower2_32((uint32) group2->ntids);

		if (ntids1 > ntids2)
			return -1;
		if (ntids1 < ntids2)
			return 1;
	}

	/*
	 * 决胜：deltids 中该块的偏移升序（该块在
	 * deltids 数组中第一个 TID 的偏移）。
	 *
	 * 这等价于按堆块号升序排序
	 * （在数组中其他方面相等的子集之间）。这种方法让我们
	 * 避免访问行外的 TID。（我们依赖的假设是：
	 * 形成每个堆块组第一个 TID 的这些偏移时，
	 * deltids 数组已按堆 TID 升序排序。）
	 */
	if (group1->ifirsttid > group2->ifirsttid)
		return 1;
	if (group1->ifirsttid < group2->ifirsttid)
		return -1;

	pg_unreachable();

	return 0;
}

/*
 * 供自底向上删除调用方使用的 heap_index_delete_tuples() 辅助函数。
 *
 * 按自底向上删除有用处理所需的顺序排序 deltids 数组。
 * 调用我们时，数组应当已经按 TID 顺序排序。
 * 排序过程把 deltids 中的堆 TID 分成堆块
 * 分组。更早/更有希望的组/块通常是那些
 * 已知拥有最多"有希望"TID
 * 的那些。
 *
 * 在 state 里设置 deltids 数组的新大小（ndeltids）。返回时 deltids 将只含
 * 来自最有希望的 BOTTOMUP_MAX_NBLOCKS 个堆块的
 * TID。这常常意味着 deltids 会缩小到原来大小的一小部分
 * （我们事先为调用方排除许多堆块，不再考虑）。
 *
 * 返回"有利"块的数量。定义和全部细节见
 * bottomup_nblocksfavorable()。
 */
static int
bottomup_sort_and_shrink(TM_IndexDeleteOp *delstate)
{
	IndexDeleteCounts *blockgroups;
	TM_IndexDelete *reordereddeltids;
	BlockNumber curblock = InvalidBlockNumber;
	int			nblockgroups = 0;
	int			ncopied = 0;
	int			nblocksfavorable = 0;

	Assert(delstate->bottomup);
	Assert(delstate->ndeltids > 0);

	/* 计算每个堆块的 TID 计数 */
	blockgroups = palloc(sizeof(IndexDeleteCounts) * delstate->ndeltids);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		bool		promising = istatus->promising;

		if (curblock != ItemPointerGetBlockNumber(htid))
		{
			/* 新的块组 */
			nblockgroups++;

			Assert(curblock < ItemPointerGetBlockNumber(htid) ||
				   !BlockNumberIsValid(curblock));

			curblock = ItemPointerGetBlockNumber(htid);
			blockgroups[nblockgroups - 1].ifirsttid = i;
			blockgroups[nblockgroups - 1].ntids = 1;
			blockgroups[nblockgroups - 1].npromisingtids = 0;
		}
		else
		{
			blockgroups[nblockgroups - 1].ntids++;
		}

		if (promising)
			blockgroups[nblockgroups - 1].npromisingtids++;
	}

	/*
	 * 我们差不多准备好排序块组，以决定访问堆块的最优顺序。
	 * 但在那之前，把每个块组的有希望元组数
	 * 向上取整到下一个二的幂，
	 * 除非它非常低（小于 4），那时我们向上取整到 4。
	 * 在两个值都很低的块组之间选择时，
	 * npromisingtids 噪声太大，
	 * 难以信赖。
	 *
	 * 这个方案把堆块/块组分成桶。每个桶
	 * 包含彼此有希望 TID 数量_大致_相同的
	 * 块。目标是忽略有希望项总数中相对小的差异，
	 * 以便整个过程可以转而
	 * 给 heapam 因素（比如堆块局部性）一点权重。
	 * 这其实不是权衡——我们没什么可失去的。把 npromisingtids
	 * 值里的小差异解释成噪声以外的任何东西
	 * 都是不明智的。
	 *
	 * 对 npromisingtids 相同的块组子集排序时，我们用 nhtids 决胜，
	 * 但这与 npromisingtids 有同样的问题，
	 * 因此 nhtids 也服从同样的二的幂分桶方案。
	 * 我们不在这里也用同样方式修正 nhtids，唯一原因是
	 * 排序之后我们需要准确的 nhtids 值。我们改为
	 * 动态处理 nhtids 分桶（在排序比较器里）。
	 *
	 * 关于堆局部性/有利块何时以及如何显著影响堆块
	 * 何时以及如何被访问，完整说明见 bottomup_nblocksfavorable()。
	 */
	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;

		/* npromisingtids 很低时，不如退回到 nhtids */
		if (group->npromisingtids <= 4)
			group->npromisingtids = 4;
		else
			group->npromisingtids =
				pg_nextpower2_32((uint32) group->npromisingtids);
	}

	/* 排序分组并重排调用方的 deltids 数组 */
	qsort(blockgroups, nblockgroups, sizeof(IndexDeleteCounts),
		  bottomup_sort_and_shrink_cmp);
	reordereddeltids = palloc(delstate->ndeltids * sizeof(TM_IndexDelete));

	nblockgroups = Min(BOTTOMUP_MAX_NBLOCKS, nblockgroups);
	/* 决定最终 deltids 开头的有利块数量 */
	nblocksfavorable = bottomup_nblocksfavorable(blockgroups, nblockgroups,
												 delstate->deltids);

	for (int b = 0; b < nblockgroups; b++)
	{
		IndexDeleteCounts *group = blockgroups + b;
		TM_IndexDelete *firstdtid = delstate->deltids + group->ifirsttid;

		memcpy(reordereddeltids + ncopied, firstdtid,
			   sizeof(TM_IndexDelete) * group->ntids);
		ncopied += group->ntids;
	}

	/* 把最终分组并排序的 TID 复制回调用方数组的开头 */
	memcpy(delstate->deltids, reordereddeltids,
		   sizeof(TM_IndexDelete) * ncopied);
	delstate->ndeltids = ncopied;

	pfree(reordereddeltids);
	pfree(blockgroups);

	return nblocksfavorable;
}

/*
 * 为堆全可见操作执行 XLogInsert。'block' 是正在被标为
 * 全可见的块，vm_buffer 是包含对应
 * 可见性映射块的缓冲区。两者应当都已经被修改
 * 并标脏。
 *
 * snapshotConflictHorizon 来自正在被标为全可见的页上最大的 xmin。
 * 重做例程用它生成恢复冲突。
 *
 * 若启用了校验和或 wal_log_hints，我们也可能生成 heap_buffer 的整页
 * 映像。否则我们优化掉 FPI（对堆缓冲区指定
 * REGBUF_NO_IMAGE），此时调用方*不*
 * 应当更新堆页的 LSN。
 */
XLogRecPtr
log_heap_visible(Relation rel, Buffer heap_buffer, Buffer vm_buffer,
				 TransactionId snapshotConflictHorizon, uint8 vmflags)
{
	xl_heap_visible xlrec;
	XLogRecPtr	recptr;
	uint8		flags;

	Assert(BufferIsValid(heap_buffer));
	Assert(BufferIsValid(vm_buffer));

	xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
	xlrec.flags = vmflags;
	if (RelationIsAccessibleInLogicalDecoding(rel))
		xlrec.flags |= VISIBILITYMAP_XLOG_CATALOG_REL;
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapVisible);

	XLogRegisterBuffer(0, vm_buffer, 0);

	flags = REGBUF_STANDARD;
	if (!XLogHintBitIsNeeded())
		flags |= REGBUF_NO_IMAGE;
	XLogRegisterBuffer(1, heap_buffer, flags);

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VISIBLE);

	return recptr;
}

/*
 * 为堆更新操作执行 XLogInsert。调用方必须已经
 * 修改缓冲区并标脏。
 */
static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf,
				Buffer newbuf, HeapTuple oldtup, HeapTuple newtup,
				HeapTuple old_key_tuple,
				bool all_visible_cleared, bool new_all_visible_cleared)
{
	xl_heap_update xlrec;
	xl_heap_header xlhdr;
	xl_heap_header xlhdr_idx;
	uint8		info;
	uint16		prefix_suffix[2];
	uint16		prefixlen = 0,
				suffixlen = 0;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(newbuf);
	bool		need_tuple_data = RelationIsLogicallyLogged(reln);
	bool		init;
	int			bufflags;

	/* 调用方不应当对不记 WAL 的关系调用我 */
	Assert(RelationNeedsWAL(reln));

	XLogBeginInsert();

	if (HeapTupleIsHeapOnly(newtup))
		info = XLOG_HEAP_HOT_UPDATE;
	else
		info = XLOG_HEAP_UPDATE;

	/*
	 * 若旧元组和新元组在同一页，我们只需记录新元组中
	 * 发生变化的部分。这能减少需要写的
	 * WAL 量。目前我们只统计元组开头和结尾
	 * 未变化的字节。这检查起来很快，并且
	 * 完美覆盖只更新一个字段的常见情形。
	 *
	 * 即使旧元组和新元组在不同页，我们也可以这样做，
	 * 但前提是我们不为旧页做整页映像，而这
	 * 很难事先知道。另外，若旧元组因某种原因损坏，
	 * 就会让损坏传播到新页，
	 * 因此最好避免。在大多数更新倾向于在同一页创建新元组版本
	 * 这个一般假设下，跨页这样做
	 * 反正也得不到多少好处。
	 *
	 * 若我们正在为新页做整页映像，就跳过，因为那种情况下
	 * WAL 记录不包含新元组。另外，
	 * 若 wal_level='logical' 则禁用，因为逻辑解码需要能够
	 * 仅从 WAL 记录完整读出新元组。
	 */
	if (oldbuf == newbuf && !need_tuple_data &&
		!XLogCheckBufferNeedsBackup(newbuf))
	{
		char	   *oldp = (char *) oldtup->t_data + oldtup->t_data->t_hoff;
		char	   *newp = (char *) newtup->t_data + newtup->t_data->t_hoff;
		int			oldlen = oldtup->t_len - oldtup->t_data->t_hoff;
		int			newlen = newtup->t_len - newtup->t_data->t_hoff;

		/* 检查旧元组与新元组之间的公共前缀 */
		for (prefixlen = 0; prefixlen < Min(oldlen, newlen); prefixlen++)
		{
			if (newp[prefixlen] != oldp[prefixlen])
				break;
		}

		/*
		 * 存储前缀长度要占 2 字节，因此我们需要至少省下
		 * 3 字节，否则没有意义。
		 */
		if (prefixlen < 3)
			prefixlen = 0;

		/* 后缀同理 */
		for (suffixlen = 0; suffixlen < Min(oldlen, newlen) - prefixlen; suffixlen++)
		{
			if (newp[newlen - suffixlen - 1] != oldp[oldlen - suffixlen - 1])
				break;
		}
		if (suffixlen < 3)
			suffixlen = 0;
	}

	/* 准备主 WAL 数据链 */
	xlrec.flags = 0;
	if (all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED;
	if (new_all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED;
	if (prefixlen > 0)
		xlrec.flags |= XLH_UPDATE_PREFIX_FROM_OLD;
	if (suffixlen > 0)
		xlrec.flags |= XLH_UPDATE_SUFFIX_FROM_OLD;
	if (need_tuple_data)
	{
		xlrec.flags |= XLH_UPDATE_CONTAINS_NEW_TUPLE;
		if (old_key_tuple)
		{
			if (reln->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_KEY;
		}
	}

	/* 若新元组是页上唯一且第一个元组…… */
	if (ItemPointerGetOffsetNumber(&(newtup->t_self)) == FirstOffsetNumber &&
		PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
	{
		info |= XLOG_HEAP_INIT_PAGE;
		init = true;
	}
	else
		init = false;

	/* 为旧页准备 WAL 数据 */
	xlrec.old_offnum = ItemPointerGetOffsetNumber(&oldtup->t_self);
	xlrec.old_xmax = HeapTupleHeaderGetRawXmax(oldtup->t_data);
	xlrec.old_infobits_set = compute_infobits(oldtup->t_data->t_infomask,
											  oldtup->t_data->t_infomask2);

	/* 为新页准备 WAL 数据 */
	xlrec.new_offnum = ItemPointerGetOffsetNumber(&newtup->t_self);
	xlrec.new_xmax = HeapTupleHeaderGetRawXmax(newtup->t_data);

	bufflags = REGBUF_STANDARD;
	if (init)
		bufflags |= REGBUF_WILL_INIT;
	if (need_tuple_data)
		bufflags |= REGBUF_KEEP_DATA;

	XLogRegisterBuffer(0, newbuf, bufflags);
	if (oldbuf != newbuf)
		XLogRegisterBuffer(1, oldbuf, REGBUF_STANDARD);

	XLogRegisterData((char *) &xlrec, SizeOfHeapUpdate);

	/*
	 * 为新元组准备 WAL 数据。
	 */
	if (prefixlen > 0 || suffixlen > 0)
	{
		if (prefixlen > 0 && suffixlen > 0)
		{
			prefix_suffix[0] = prefixlen;
			prefix_suffix[1] = suffixlen;
			XLogRegisterBufData(0, (char *) &prefix_suffix, sizeof(uint16) * 2);
		}
		else if (prefixlen > 0)
		{
			XLogRegisterBufData(0, (char *) &prefixlen, sizeof(uint16));
		}
		else
		{
			XLogRegisterBufData(0, (char *) &suffixlen, sizeof(uint16));
		}
	}

	xlhdr.t_infomask2 = newtup->t_data->t_infomask2;
	xlhdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.t_hoff = newtup->t_data->t_hoff;
	Assert(SizeofHeapTupleHeader + prefixlen + suffixlen <= newtup->t_len);

	/*
	 * PG73FORMAT：写入位图 [+ 填充] [+ oid] + 数据
	 *
	 * 'data' 不包括公共前缀或后缀。
	 */
	XLogRegisterBufData(0, (char *) &xlhdr, SizeOfHeapHeader);
	if (prefixlen == 0)
	{
		XLogRegisterBufData(0,
							((char *) newtup->t_data) + SizeofHeapTupleHeader,
							newtup->t_len - SizeofHeapTupleHeader - suffixlen);
	}
	else
	{
		/*
		 * 必须把空值位图和公共前缀之后的数据写成
		 * 两个单独的 rdata 项。
		 */
		/* 位图 [+ 填充] [+ oid] */
		if (newtup->t_data->t_hoff - SizeofHeapTupleHeader > 0)
		{
			XLogRegisterBufData(0,
								((char *) newtup->t_data) + SizeofHeapTupleHeader,
								newtup->t_data->t_hoff - SizeofHeapTupleHeader);
		}

		/* 公共前缀之后的数据 */
		XLogRegisterBufData(0,
							((char *) newtup->t_data) + newtup->t_data->t_hoff + prefixlen,
							newtup->t_len - newtup->t_data->t_hoff - prefixlen - suffixlen);
	}

	/* 我们需要记录一个元组标识 */
	if (need_tuple_data && old_key_tuple)
	{
		/* 其实并不需要这个，但解码起来更顺手 */
		xlhdr_idx.t_infomask2 = old_key_tuple->t_data->t_infomask2;
		xlhdr_idx.t_infomask = old_key_tuple->t_data->t_infomask;
		xlhdr_idx.t_hoff = old_key_tuple->t_data->t_hoff;

		XLogRegisterData((char *) &xlhdr_idx, SizeOfHeapHeader);

		/* PG73FORMAT：写入位图 [+ 填充] [+ oid] + 数据 */
		XLogRegisterData((char *) old_key_tuple->t_data + SizeofHeapTupleHeader,
						 old_key_tuple->t_len - SizeofHeapTupleHeader);
	}

	/* 在行级按来源过滤要高效得多 */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	recptr = XLogInsert(RM_HEAP_ID, info);

	return recptr;
}

/*
 * 执行 XLOG_HEAP2_NEW_CID 记录的 XLogInsert
 *
 * 这只在 wal_level >= WAL_LEVEL_LOGICAL 时使用，且只用于系统目录
 * 元组。
 */
static XLogRecPtr
log_heap_new_cid(Relation relation, HeapTuple tup)
{
	xl_heap_new_cid xlrec;

	XLogRecPtr	recptr;
	HeapTupleHeader hdr = tup->t_data;

	Assert(ItemPointerIsValid(&tup->t_self));
	Assert(tup->t_tableOid != InvalidOid);

	xlrec.top_xid = GetTopTransactionId();
	xlrec.target_locator = relation->rd_locator;
	xlrec.target_tid = tup->t_self;

	/*
	 * 若元组在同一事务中被插入又被删除，我们肯定有
	 * 组合 CID，设置 cmin 和 cmax。
	 */
	if (hdr->t_infomask & HEAP_COMBOCID)
	{
		Assert(!(hdr->t_infomask & HEAP_XMAX_INVALID));
		Assert(!HeapTupleHeaderXminInvalid(hdr));
		xlrec.cmin = HeapTupleHeaderGetCmin(hdr);
		xlrec.cmax = HeapTupleHeaderGetCmax(hdr);
		xlrec.combocid = HeapTupleHeaderGetRawCommandId(hdr);
	}
	/* 没有组合 CID，因此该事务只能设置 cmin 或 cmax */
	else
	{
		/*
		 * 元组已插入。
		 *
		 * 我们需要检查 LOCK ONLY，因为在 FOR KEY SHARE 更新时
		 * multixact 可能被转移到新元组，
		 * 此时会有 xmax，尽管元组只是刚刚
		 * 被插入。
		 */
		if (hdr->t_infomask & HEAP_XMAX_INVALID ||
			HEAP_XMAX_IS_LOCKED_ONLY(hdr->t_infomask))
		{
			xlrec.cmin = HeapTupleHeaderGetRawCommandId(hdr);
			xlrec.cmax = InvalidCommandId;
		}
		/* 来自另一个事务的元组被更新或删除。 */
		else
		{
			xlrec.cmin = InvalidCommandId;
			xlrec.cmax = HeapTupleHeaderGetRawCommandId(hdr);
		}
		xlrec.combocid = InvalidCommandId;
	}

	/*
	 * 注意这里不必登记缓冲区，因为这个
	 * 操作不修改该页。调用我们的插入/更新/删除
	 * 当然改过，但那是单独记入 WAL 的。
	 */
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapNewCid);

	/* 无论来源如何都会被查看 */

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_NEW_CID);

	return recptr;
}

/*
 * 构造一个表示所配置 REPLICA IDENTITY 的堆元组，用来表示
 * UPDATE 或 DELETE 中的旧元组。
 *
 * 若不必记录标识，或没有定义合适的
 * 键，则返回 NULL。
 *
 * 若任何副本标识列的值变了，或者
 * 其中任何列含有外部数据，则传入 key_required 为 true。删除必须始终传入 true。
 *
 * 若返回的元组是修改过的副本，而不是
 * 传入的同一个元组，则把 *copy 设为 true。
 */
static HeapTuple
ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_required,
					   bool *copy)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char		replident = relation->rd_rel->relreplident;
	Bitmapset  *idattrs;
	HeapTuple	key_tuple;
	bool		nulls[MaxHeapAttributeNumber];
	Datum		values[MaxHeapAttributeNumber];

	*copy = false;

	if (!RelationIsLogicallyLogged(relation))
		return NULL;

	if (replident == REPLICA_IDENTITY_NOTHING)
		return NULL;

	if (replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * 记录整个旧元组时，它完全可能包含
		 * 已 TOAST 的列。若是这样，强制把它们内联。
		 */
		if (HeapTupleHasExternal(tp))
		{
			*copy = true;
			tp = toast_flatten_tuple(tp, desc);
		}
		return tp;
	}

	/* 若键不是必需的，且我们只记录键，就结束了 */
	if (!key_required)
		return NULL;

	/* 找出副本标识列 */
	idattrs = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/*
	 * 若没有定义副本标识列，就当作 !key_required。
	 * （从 heap_update 不应当到达这种情形，因为它应当
	 * 准确地计算 key_required。但 heap_delete 只传入
	 * 常量 true 作为 key_required，因此删除时会碰到这种情形。）
	 */
	if (bms_is_empty(idattrs))
		return NULL;

	/*
	 * 构造一个只含副本标识列的新元组，
	 * 其他地方为空。同时断言副本
	 * 标识列不是空。
	 */
	heap_deform_tuple(tp, desc, values, nulls);

	for (int i = 0; i < desc->natts; i++)
	{
		if (bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			Assert(!nulls[i]);
		else
			nulls[i] = true;
	}

	key_tuple = heap_form_tuple(desc, values, nulls);
	*copy = true;

	bms_free(idattrs);

	/*
	 * 若元组到这里只含索引列，却仍然有
	 * 已 TOAST 的列，强制把它们内联。这有些不太可能，
	 * 因为索引列的大小有限制，所以我们不在上面遍历
	 * 索引列的循环里重复 toast_flatten_tuple() 的功能，
	 * 即使那样会更高效。
	 */
	if (HeapTupleHasExternal(key_tuple))
	{
		HeapTuple	oldtup = key_tuple;

		key_tuple = toast_flatten_tuple(oldtup, desc);
		heap_freetuple(oldtup);
	}

	return key_tuple;
}

/*
 * 处理 XLOG_HEAP2_PRUNE 记录类型。
 *
 * 获取完全清理锁。
 */
static void
heap_xlog_prune(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_prune *xlrec = (xl_heap_prune *) XLogRecGetData(record);
	Buffer		buffer;
	RelFileLocator rlocator;
	BlockNumber blkno;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	/*
	 * 我们即将移除元组。在热备模式下，确保没有正在运行的
	 * 查询仍能看见将被移除的元组。
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											xlrec->isCatalogRel,
											rlocator);

	/*
	 * 若有整页映像，恢复它（使用清理锁），然后
	 * 就完成了。
	 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true,
										   &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buffer);
		OffsetNumber *end;
		OffsetNumber *redirected;
		OffsetNumber *nowdead;
		OffsetNumber *nowunused;
		int			nredirected;
		int			ndead;
		int			nunused;
		Size		datalen;

		redirected = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);

		nredirected = xlrec->nredirected;
		ndead = xlrec->ndead;
		end = (OffsetNumber *) ((char *) redirected + datalen);
		nowdead = redirected + (nredirected * 2);
		nowunused = nowdead + ndead;
		nunused = (end - nowunused);
		Assert(nunused >= 0);

		/* 按记录更新所有行指针，并修复碎片 */
		heap_page_prune_execute(buffer,
								redirected, nredirected,
								nowdead, ndead,
								nowunused, nunused);

		/*
		 * 注意：我们不操心更新该页的可剪枝性提示。
		 * 最坏情况下这会导致很快再发生一轮剪枝。
		 */

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
	{
		Size		freespace = PageGetHeapFreeSpace(BufferGetPage(buffer));

		UnlockReleaseBuffer(buffer);

		/*
		 * 对一页做完剪枝记录之后，更新 FSM
		 * 是有用的，因为这可能使该页成为稍后插入的目标，
		 * 即使清理决定不访问它（若它被标成
		 * 全可见，就可能发生）。
		 *
		 * 无论是否应用了整页映像都这样做，因为
		 * FSM 数据本来就不在页里。
		 */
		XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
	}
}

/*
 * 处理 XLOG_HEAP2_VACUUM 记录类型。
 *
 * 只获取普通排他锁。
 */
static void
heap_xlog_vacuum(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_vacuum *xlrec = (xl_heap_vacuum *) XLogRecGetData(record);
	Buffer		buffer;
	BlockNumber blkno;
	XLogRedoAction action;

	/*
	 * 若有整页映像，恢复它（不使用清理锁），
	 * 然后就完成了。
	 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, false,
										   &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buffer);
		OffsetNumber *nowunused;
		Size		datalen;
		OffsetNumber *offnum;

		nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);

		/* 除非有事可做，否则不应当有这条记录 */
		Assert(xlrec->nunused > 0);

		/* 更新所有现在未使用的行指针 */
		offnum = nowunused;
		for (int i = 0; i < xlrec->nunused; i++)
		{
			OffsetNumber off = *offnum++;
			ItemId		lp = PageGetItemId(page, off);

			Assert(ItemIdIsDead(lp) && !ItemIdHasStorage(lp));
			ItemIdSetUnused(lp);
		}

		/* 现在尝试截断行指针数组 */
		PageTruncateLinePointerArray(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
	{
		Size		freespace = PageGetHeapFreeSpace(BufferGetPage(buffer));
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

		UnlockReleaseBuffer(buffer);

		/*
		 * 从一页清理掉 LP_DEAD 项之后，更新
		 * FSM 是有用的，因为这可能使该页成为稍后
		 * 插入的目标，即使清理决定不访问它（若它被标成
		 * 全可见，就可能发生）。
		 *
		 * 无论是否应用了整页映像都这样做，因为
		 * FSM 数据本来就不在页里。
		 */
		XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
	}
}

/*
 * 重放 XLOG_HEAP2_VISIBLE 记录。
 *
 * 这里关键的完整性要求是，我们绝不能落到
 * 可见性映射位已设置、而页级
 * PD_ALL_VISIBLE 位被清除的局面。若发生那种情况，随后的
 * 页修改就会无法清除可见性映射位。
 */
static void
heap_xlog_visible(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_visible *xlrec = (xl_heap_visible *) XLogRecGetData(record);
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	RelFileLocator rlocator;
	BlockNumber blkno;
	XLogRedoAction action;

	Assert((xlrec->flags & VISIBILITYMAP_XLOG_VALID_BITS) == xlrec->flags);

	XLogRecGetBlockTag(record, 1, &rlocator, NULL, &blkno);

	/*
	 * 若有任何热备事务正在运行，其 xmin
	 * 视界旧到该页对它们并非全可见，它们
	 * 可能错误地认定仅索引扫描可以跳过堆取数。
	 *
	 * 注意：这里抛出某种"软"冲突可能更好，它
	 * 迫使任何正在进行的仅索引扫描执行堆取数，
	 * 而不是直接终止该事务。
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											xlrec->flags & VISIBILITYMAP_XLOG_CATALOG_REL,
											rlocator);

	/*
	 * 读取堆页，若它仍然存在。若堆文件在恢复中稍后被删除或
	 * 截断，我们不必更新该页，但最好
	 * 仍然更新可见性映射。
	 */
	action = XLogReadBufferForRedo(record, 1, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		/*
		 * 设置可见性映射位时，我们不推进堆页的 LSN
		 * （除非启用了校验和或 wal_hint_bits，那时
		 * 我们必须推进）。这使我们面临撕裂页风险，但由于
		 * 我们不以任何方式检查现有页内容，我们
		 * 并不在意。
		 */
		page = BufferGetPage(buffer);

		PageSetAllVisible(page);

		if (XLogHintBitIsNeeded())
			PageSetLSN(page, lsn);

		MarkBufferDirty(buffer);
	}
	else if (action == BLK_RESTORED)
	{
		/*
		 * 若堆块已备份，我们已经恢复了它，没有
		 * 更多事情要做。（这只在启用校验和或
		 * wal_log_hints 时发生。）
		 */
	}

	if (BufferIsValid(buffer))
	{
		Size		space = PageGetFreeSpace(BufferGetPage(buffer));

		UnlockReleaseBuffer(buffer);

		/*
		 * 由于 FSM 不记 WAL，且只启发式地更新，它
		 * 在备库上很容易变陈旧。
		 * 若备库后来被提升
		 * 并运行 VACUUM，它会跳过更新那些变成全可见
		 * （或全冻结，取决于
		 * 清理模式）的页的各个空闲空间数值，当 FreeSpaceMapVacuum
		 * 把过于乐观的空闲空间值传播到 FSM 上层时，这很麻烦；
		 * 后来的插入者试图使用这样的页，结果发现它们
		 * 不可用。当这样的页很多时，这会造成长时间停顿。
		 *
		 * 对正在变成全可见或全冻结的页，更新 FSM 对它的看法，
		 * 以此预先防止那些问题。
		 *
		 * 无论是否应用了整页映像都这样做，因为
		 * FSM 数据本来就不在页里。
		 */
		if (xlrec->flags & VISIBILITYMAP_VALID_BITS)
			XLogRecordPageWithFreeSpace(rlocator, blkno, space);
	}

	/*
	 * 即使因 LSN 互锁而跳过了堆页更新，
	 * 更新可见性映射仍然安全。任何清除
	 * 可见性映射位的 WAL 记录都在检查页 LSN 之前这样做，因此任何
	 * 需要清除的位仍会被清除。
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_ZERO_ON_ERROR, false,
									  &vmbuffer) == BLK_NEEDS_REDO)
	{
		Page		vmpage = BufferGetPage(vmbuffer);
		Relation	reln;
		uint8		vmbits;

		/* 若该页被读成全零，则初始化它 */
		if (PageIsNew(vmpage))
			PageInit(vmpage, BLCKSZ, 0);

		/* 去掉 VISIBILITYMAP_XLOG_* */
		vmbits = xlrec->flags & VISIBILITYMAP_VALID_BITS;

		/*
		 * XLogReadBufferForRedoExtended 锁住了缓冲区。但
		 * visibilitymap_set 会自己处理加锁。
		 */
		LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);

		reln = CreateFakeRelcacheEntry(rlocator);
		visibilitymap_pin(reln, blkno, &vmbuffer);

		visibilitymap_set(reln, blkno, InvalidBuffer, lsn, vmbuffer,
						  xlrec->snapshotConflictHorizon, vmbits);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}
	else if (BufferIsValid(vmbuffer))
		UnlockReleaseBuffer(vmbuffer);
}

/*
 * 重放 XLOG_HEAP2_FREEZE_PAGE 记录
 */
static void
heap_xlog_freeze_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_freeze_page *xlrec = (xl_heap_freeze_page *) XLogRecGetData(record);
	Buffer		buffer;

	/*
	 * 在热备模式下，确保没有正在运行的查询仍把
	 * 已冻结的 xid 视为正在运行。
	 */
	if (InHotStandby)
	{
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											xlrec->isCatalogRel,
											rlocator);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		xl_heap_freeze_plan *plans;
		OffsetNumber *offsets;
		int			curoff = 0;

		plans = (xl_heap_freeze_plan *) XLogRecGetBlockData(record, 0, NULL);
		offsets = (OffsetNumber *) ((char *) plans +
									(xlrec->nplans *
									 sizeof(xl_heap_freeze_plan)));
		for (int p = 0; p < xlrec->nplans; p++)
		{
			HeapTupleFreeze frz;

			/*
			 * 把冻结计划表示从 WAL 记录转换成
			 * heap_execute_freeze_tuple 使用的按元组格式
			 */
			frz.xmax = plans[p].xmax;
			frz.t_infomask2 = plans[p].t_infomask2;
			frz.t_infomask = plans[p].t_infomask;
			frz.frzflags = plans[p].frzflags;
			frz.offset = InvalidOffsetNumber;	/* 未使用，但求整齐 */

			for (int i = 0; i < plans[p].ntuples; i++)
			{
				OffsetNumber offset = offsets[curoff++];
				ItemId		lp;
				HeapTupleHeader tuple;

				lp = PageGetItemId(page, offset);
				tuple = (HeapTupleHeader) PageGetItem(page, lp);
				heap_execute_freeze_tuple(tuple, &frz);
			}
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * 给定 XLog 记录中的 "infobits" 字段，为该记录触及的元组
 * 在给定的 infomask 和 infomask2 里设置正确的位。
 *
 * （这是 compute_infobits 的逆操作）。
 */
static void
fix_infomask_from_infobits(uint8 infobits, uint16 *infomask, uint16 *infomask2)
{
	*infomask &= ~(HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY |
				   HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_EXCL_LOCK);
	*infomask2 &= ~HEAP_KEYS_UPDATED;

	if (infobits & XLHL_XMAX_IS_MULTI)
		*infomask |= HEAP_XMAX_IS_MULTI;
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		*infomask |= HEAP_XMAX_LOCK_ONLY;
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		*infomask |= HEAP_XMAX_EXCL_LOCK;
	/* 注意这里不考虑 HEAP_XMAX_SHR_LOCK */
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		*infomask |= HEAP_XMAX_KEYSHR_LOCK;

	if (infobits & XLHL_KEYS_UPDATED)
		*infomask2 |= HEAP_KEYS_UPDATED;
}

static void
heap_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_delete *xlrec = (xl_heap_delete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	BlockNumber blkno;
	RelFileLocator target_locator;
	ItemPointerData target_tid;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_locator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) >= xlrec->offnum)
			lp = PageGetItemId(page, xlrec->offnum);

		if (PageGetMaxOffsetNumber(page) < xlrec->offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->infobits_set,
								   &htup->t_infomask, &htup->t_infomask2);
		if (!(xlrec->flags & XLH_DELETE_IS_SUPER))
			HeapTupleHeaderSetXmax(htup, xlrec->xmax);
		else
			HeapTupleHeaderSetXmin(htup, InvalidTransactionId);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);

		/* 把该页标为剪枝候选 */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		/* 确保 t_ctid 设置正确 */
		if (xlrec->flags & XLH_DELETE_IS_PARTITION_MOVE)
			HeapTupleHeaderSetMovedPartitions(htup);
		else
			htup->t_ctid = target_tid;
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_insert *xlrec = (xl_heap_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	RelFileLocator target_locator;
	BlockNumber blkno;
	ItemPointerData target_tid;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/* heap_insert() 代码路径中不冻结 */
	Assert(!(xlrec->flags & XLH_INSERT_ALL_FROZEN_SET));

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_locator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * 若我们插入的是该页上第一个且唯一的元组，从头
	 * 重新初始化该页。
	 */
	if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *data;

		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) + 1 < xlrec->offnum)
			elog(PANIC, "invalid max offset number");

		data = XLogRecGetBlockData(record, 0, &datalen);

		newlen = datalen - SizeOfHeapHeader;
		Assert(datalen > SizeOfHeapHeader && newlen <= MaxHeapTupleSize);
		memcpy((char *) &xlhdr, data, SizeOfHeapHeader);
		data += SizeOfHeapHeader;

		htup = &tbuf.hdr;
		MemSet((char *) htup, 0, SizeofHeapTupleHeader);
		/* PG73FORMAT：取得位图 [+ 填充] [+ oid] + 数据 */
		memcpy((char *) htup + SizeofHeapTupleHeader,
			   data,
			   newlen);
		newlen += SizeofHeapTupleHeader;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;
		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		htup->t_ctid = target_tid;

		if (PageAddItem(page, (Item) htup, newlen, xlrec->offnum,
						true, true) == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		freespace = PageGetHeapFreeSpace(page); /* 下面更新 FSM 时需要 */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * 若该页空闲空间不足，也更新 FSM。
	 * 任意地，我们对"不足"的定义是低于 20%。不知道表的填充因子，
	 * 我们就无法比这做得好很多。
	 *
	 * XXX：若该页是从整页映像恢复的，不要这样做。那种情况下
	 * 我们懒得更新 FSM，反正它也不必
	 * 完全准确。
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(target_locator, blkno, freespace);
}

/*
 * 处理 MULTI_INSERT 记录类型。
 */
static void
heap_xlog_multi_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_multi_insert *xlrec;
	RelFileLocator rlocator;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	uint32		newlen;
	Size		freespace = 0;
	int			i;
	bool		isinit = (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0;
	XLogRedoAction action;

	/*
	 * 插入不覆盖 MVCC 数据，因此不必
	 * 做冲突处理。
	 */
	xlrec = (xl_heap_multi_insert *) XLogRecGetData(record);

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	/* 检查互斥标志没有同时设置 */
	Assert(!((xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED) &&
			 (xlrec->flags & XLH_INSERT_ALL_FROZEN_SET)));

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (isinit)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	   *tupdata;
		char	   *endptr;
		Size		len;

		/* 元组作为块数据存储 */
		tupdata = XLogRecGetBlockData(record, 0, &len);
		endptr = tupdata + len;

		page = (Page) BufferGetPage(buffer);

		for (i = 0; i < xlrec->ntuples; i++)
		{
			OffsetNumber offnum;
			xl_multi_insert_tuple *xlhdr;

			/*
			 * 若我们正在重新初始化该页，元组从 FirstOffsetNumber 起
			 * 按顺序存储。否则 WAL 记录里有一个偏移
			 * 数组，元组跟在它后面。
			 */
			if (isinit)
				offnum = FirstOffsetNumber + i;
			else
				offnum = xlrec->offsets[i];
			if (PageGetMaxOffsetNumber(page) + 1 < offnum)
				elog(PANIC, "invalid max offset number");

			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(tupdata);
			tupdata = ((char *) xlhdr) + SizeOfMultiInsertTuple;

			newlen = xlhdr->datalen;
			Assert(newlen <= MaxHeapTupleSize);
			htup = &tbuf.hdr;
			MemSet((char *) htup, 0, SizeofHeapTupleHeader);
			/* PG73FORMAT：取得位图 [+ 填充] [+ oid] + 数据 */
			memcpy((char *) htup + SizeofHeapTupleHeader,
				   (char *) tupdata,
				   newlen);
			tupdata += newlen;

			newlen += SizeofHeapTupleHeader;
			htup->t_infomask2 = xlhdr->t_infomask2;
			htup->t_infomask = xlhdr->t_infomask;
			htup->t_hoff = xlhdr->t_hoff;
			HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
			HeapTupleHeaderSetCmin(htup, FirstCommandId);
			ItemPointerSetBlockNumber(&htup->t_ctid, blkno);
			ItemPointerSetOffsetNumber(&htup->t_ctid, offnum);

			offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
			if (offnum == InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");
		}
		if (tupdata != endptr)
			elog(PANIC, "total tuple length mismatch");

		freespace = PageGetHeapFreeSpace(page); /* 下面更新 FSM 时需要 */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		/* XLH_INSERT_ALL_FROZEN_SET 意味着所有元组都可见 */
		if (xlrec->flags & XLH_INSERT_ALL_FROZEN_SET)
			PageSetAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * 若该页空闲空间不足，也更新 FSM。
	 * 任意地，我们对"不足"的定义是低于 20%。不知道表的填充因子，
	 * 我们就无法比这做得好很多。
	 *
	 * XXX：若该页是从整页映像恢复的，不要这样做。那种情况下
	 * 我们懒得更新 FSM，反正它也不必
	 * 完全准确。
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
}

/*
 * 处理 UPDATE 和 HOT_UPDATE
 */
static void
heap_xlog_update(XLogReaderState *record, bool hot_update)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	RelFileLocator rlocator;
	BlockNumber oldblk;
	BlockNumber newblk;
	ItemPointerData newtid;
	Buffer		obuffer,
				nbuffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleData oldtup;
	HeapTupleHeader htup;
	uint16		prefixlen = 0,
				suffixlen = 0;
	char	   *newp;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	XLogRedoAction oldaction;
	XLogRedoAction newaction;

	/* 初始化，以免编译器报警 */
	oldtup.t_data = NULL;
	oldtup.t_len = 0;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &newblk);
	if (XLogRecGetBlockTagExtended(record, 1, NULL, NULL, &oldblk, NULL))
	{
		/* HOT 更新从不跨页进行 */
		Assert(!hot_update);
	}
	else
		oldblk = newblk;

	ItemPointerSet(&newtid, newblk, xlrec->new_offnum);

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, oldblk, &vmbuffer);
		visibilitymap_clear(reln, oldblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * 正常操作中，按页号顺序锁住这两页很重要，
	 * 以免与反方向进行的其他更新
	 * 操作可能死锁。不过 WAL 重放期间不会
	 * 有其他更新在进行，因此不必担心那个。但
	 * 我们*确实*需要担心不要向热备查询暴露不一致状态——
	 * 因此在把新元组加到新页之前，
	 * 原页无法解锁。
	 */

	/* 处理旧元组版本 */
	oldaction = XLogReadBufferForRedo(record, (oldblk == newblk) ? 0 : 1,
									  &obuffer);
	if (oldaction == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(obuffer);
		offnum = xlrec->old_offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldtup.t_data = htup;
		oldtup.t_len = ItemIdGetLength(lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		if (hot_update)
			HeapTupleHeaderSetHotUpdated(htup);
		else
			HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->old_infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->old_xmax);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		/* 在 t_ctid 里设置前向链链接 */
		htup->t_ctid = newtid;

		/* 把该页标为剪枝候选 */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(obuffer);
	}

	/*
	 * 若新元组要去的页与旧页不同，读取该页。
	 */
	if (oldblk == newblk)
	{
		nbuffer = obuffer;
		newaction = oldaction;
	}
	else if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		nbuffer = XLogInitBufferForRedo(record, 0);
		page = (Page) BufferGetPage(nbuffer);
		PageInit(page, BufferGetPageSize(nbuffer), 0);
		newaction = BLK_NEEDS_REDO;
	}
	else
		newaction = XLogReadBufferForRedo(record, 0, &nbuffer);

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rlocator);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, newblk, &vmbuffer);
		visibilitymap_clear(reln, newblk, vmbuffer, VISIBILITYMAP_VALID_BITS);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/* 处理新元组 */
	if (newaction == BLK_NEEDS_REDO)
	{
		char	   *recdata;
		char	   *recdata_end;
		Size		datalen;
		Size		tuplen;

		recdata = XLogRecGetBlockData(record, 0, &datalen);
		recdata_end = recdata + datalen;

		page = BufferGetPage(nbuffer);

		offnum = xlrec->new_offnum;
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "invalid max offset number");

		if (xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&prefixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}
		if (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&suffixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}

		memcpy((char *) &xlhdr, recdata, SizeOfHeapHeader);
		recdata += SizeOfHeapHeader;

		tuplen = recdata_end - recdata;
		Assert(tuplen <= MaxHeapTupleSize);

		htup = &tbuf.hdr;
		MemSet((char *) htup, 0, SizeofHeapTupleHeader);

		/*
		 * 用旧元组的前缀和/或后缀，以及 WAL 记录里
		 * 存放的数据，重建新元组。
		 */
		newp = (char *) htup + SizeofHeapTupleHeader;
		if (prefixlen > 0)
		{
			int			len;

			/* 从 WAL 记录复制位图 [+ 填充] [+ oid] */
			len = xlhdr.t_hoff - SizeofHeapTupleHeader;
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;

			/* 从旧元组复制前缀 */
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_data->t_hoff, prefixlen);
			newp += prefixlen;

			/* 从 WAL 记录复制新元组数据 */
			len = tuplen - (xlhdr.t_hoff - SizeofHeapTupleHeader);
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}
		else
		{
			/*
			 * 从记录一次性复制位图 [+ 填充] [+ oid] + 数据，
			 * 全部一次完成
			 */
			memcpy(newp, recdata, tuplen);
			recdata += tuplen;
			newp += tuplen;
		}
		Assert(recdata == recdata_end);

		/* 从旧元组复制后缀 */
		if (suffixlen > 0)
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_len - suffixlen, suffixlen);

		newlen = SizeofHeapTupleHeader + tuplen + prefixlen + suffixlen;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;

		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		HeapTupleHeaderSetXmax(htup, xlrec->new_xmax);
		/* 确保 t_ctid 里没有前向链链接 */
		htup->t_ctid = newtid;

		offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		freespace = PageGetHeapFreeSpace(page); /* 下面更新 FSM 时需要 */

		PageSetLSN(page, lsn);
		MarkBufferDirty(nbuffer);
	}

	if (BufferIsValid(nbuffer) && nbuffer != obuffer)
		UnlockReleaseBuffer(nbuffer);
	if (BufferIsValid(obuffer))
		UnlockReleaseBuffer(obuffer);

	/*
	 * 若新页空闲空间不足，也更新 FSM。
	 * 任意地，我们对"不足"的定义是低于 20%。不知道表的填充因子，
	 * 我们就无法比这做得好很多。
	 *
	 * 不过，不要在 HOT 更新时更新 FSM，因为崩溃
	 * 恢复之后，旧元组或新元组肯定有一个是死的且
	 * 可剪枝。剪枝之后，该页的空闲空间大致会和
	 * 更新前一样多，假定新元组与旧元组大小
	 * 差不多。
	 *
	 * XXX：若该页是从整页映像恢复的，不要这样做。那种情况下
	 * 我们懒得更新 FSM，反正它也不必
	 * 完全准确。
	 */
	if (newaction == BLK_NEEDS_REDO && !hot_update && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rlocator, newblk, freespace);
}

static void
heap_xlog_confirm(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_confirm *xlrec = (xl_heap_confirm *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		/*
		 * 确认元组确实已插入
		 */
		ItemPointerSet(&htup->t_ctid, BufferGetBlockNumber(buffer), offnum);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_lock(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock *xlrec = (xl_heap_lock *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
	{
		RelFileLocator rlocator;
		Buffer		vmbuffer = InvalidBuffer;
		BlockNumber block;
		Relation	reln;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, &block);
		reln = CreateFakeRelcacheEntry(rlocator);

		visibilitymap_pin(reln, block, &vmbuffer);
		visibilitymap_clear(reln, block, vmbuffer, VISIBILITYMAP_ALL_FROZEN);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);

		/*
		 * 清除相关的更新标志，但仅当修改后的 infomask 表明
		 * 没有更新时。
		 */
		if (HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
		{
			HeapTupleHeaderClearHotUpdated(htup);
			/* 确保 t_ctid 里没有前向链链接 */
			ItemPointerSet(&htup->t_ctid,
						   BufferGetBlockNumber(buffer),
						   offnum);
		}
		HeapTupleHeaderSetXmax(htup, xlrec->xmax);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_lock_updated(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock_updated *xlrec;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	xlrec = (xl_heap_lock_updated *) XLogRecGetData(record);

	/*
	 * 即使堆页已经是最新的，可见性映射
	 * 也可能需要修正。
	 */
	if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
	{
		RelFileLocator rlocator;
		Buffer		vmbuffer = InvalidBuffer;
		BlockNumber block;
		Relation	reln;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, &block);
		reln = CreateFakeRelcacheEntry(rlocator);

		visibilitymap_pin(reln, block, &vmbuffer);
		visibilitymap_clear(reln, block, vmbuffer, VISIBILITYMAP_ALL_FROZEN);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->xmax);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_inplace(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_inplace *xlrec = (xl_heap_inplace *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	Size		newlen;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *newtup = XLogRecGetBlockData(record, 0, &newlen);

		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldlen = ItemIdGetLength(lp) - htup->t_hoff;
		if (oldlen != newlen)
			elog(PANIC, "wrong tuple length");

		memcpy((char *) htup + htup->t_hoff, newtup, newlen);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

void
heap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/*
	 * 这些操作不覆盖 MVCC 数据，因此不必做冲突处理。
	 * heap2 资源管理器里的那些会做。
	 */

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
			heap_xlog_insert(record);
			break;
		case XLOG_HEAP_DELETE:
			heap_xlog_delete(record);
			break;
		case XLOG_HEAP_UPDATE:
			heap_xlog_update(record, false);
			break;
		case XLOG_HEAP_TRUNCATE:

			/*
			 * TRUNCATE 是空操作，因为这些动作已经作为
			 * SMGR WAL 记录记入日志。TRUNCATE WAL 记录只为逻辑
			 * 解码而存在。
			 */
			break;
		case XLOG_HEAP_HOT_UPDATE:
			heap_xlog_update(record, true);
			break;
		case XLOG_HEAP_CONFIRM:
			heap_xlog_confirm(record);
			break;
		case XLOG_HEAP_LOCK:
			heap_xlog_lock(record);
			break;
		case XLOG_HEAP_INPLACE:
			heap_xlog_inplace(record);
			break;
		default:
			elog(PANIC, "heap_redo: unknown op code %u", info);
	}
}

void
heap2_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_PRUNE:
			heap_xlog_prune(record);
			break;
		case XLOG_HEAP2_VACUUM:
			heap_xlog_vacuum(record);
			break;
		case XLOG_HEAP2_FREEZE_PAGE:
			heap_xlog_freeze_page(record);
			break;
		case XLOG_HEAP2_VISIBLE:
			heap_xlog_visible(record);
			break;
		case XLOG_HEAP2_MULTI_INSERT:
			heap_xlog_multi_insert(record);
			break;
		case XLOG_HEAP2_LOCK_UPDATED:
			heap_xlog_lock_updated(record);
			break;
		case XLOG_HEAP2_NEW_CID:

			/*
			 * 真正重放时无事可做，只在逻辑
			 * 解码期间使用。
			 */
			break;
		case XLOG_HEAP2_REWRITE:
			heap_xlog_logical_rewrite(record);
			break;
		default:
			elog(PANIC, "heap2_redo: unknown op code %u", info);
	}
}

/*
 * 在对堆页做一致性检查之前屏蔽它。
 */
void
heap_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	OffsetNumber off;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	for (off = 1; off <= PageGetMaxOffsetNumber(page); off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		char	   *page_item;

		page_item = (char *) (page + ItemIdGetOffset(iid));

		if (ItemIdIsNormal(iid))
		{
			HeapTupleHeader page_htup = (HeapTupleHeader) page_item;

			/*
			 * 若元组的 xmin 尚未冻结，我们应当忽略
			 * 提示位的差异，因为它们可以在不
			 * 发出 WAL 的情况下被设置。
			 */
			if (!HeapTupleHeaderXminFrozen(page_htup))
				page_htup->t_infomask &= ~HEAP_XACT_MASK;
			else
			{
				/* 我们仍然需要屏蔽 xmax 提示位。 */
				page_htup->t_infomask &= ~HEAP_XMAX_INVALID;
				page_htup->t_infomask &= ~HEAP_XMAX_COMMITTED;
			}

			/*
			 * 重放期间，我们把命令 Id 设为 FirstCommandId。因此屏蔽
			 * 它。详情见 heap_xlog_insert()。
			 */
			page_htup->t_choice.t_heap.t_field3.t_cid = MASK_MARKER;

			/*
			 * 对推测性元组，heap_insert() 并不在调用方传入的堆元组本身里设置 ctid，
			 * 让 ctid 字段
			 * 含有推测性令牌值——每个后端单调
			 * 递增的标识符。此外，它在任何情况下都不把 ctid
			 * 记入 WAL。
			 *
			 * 重做期间，heap_xlog_insert() 把 t_ctid 设为当前块
			 * 号和自身偏移号。它不关心主库上的任何
			 * 推测性插入。因此我们把 t_ctid 设为
			 * 当前块号和自身偏移号，以忽略任何
			 * 不一致。
			 */
			if (HeapTupleHeaderIsSpeculative(page_htup))
				ItemPointerSet(&page_htup->t_ctid, blkno, off);

			/*
			 * 注意：不要忽略因元组已移动而导致的 ctid 变化
			 * （即 HeapTupleHeaderIndicatesMovedPartitions），因为那是
			 * 需要在主库与备库之间保持同步的重要信息，
			 * 因此会记入 WAL。
			 */
		}

		/*
		 * 当项的长度不是 MAXALIGNed 时，忽略元组之后的
		 * 任何填充字节。
		 */
		if (ItemIdHasStorage(iid))
		{
			int			len = ItemIdGetLength(iid);
			int			padlen = MAXALIGN(len) - len;

			if (padlen > 0)
				memset(page_item + len, MASK_MARKER, padlen);
		}
	}
}

/*
 * HeapCheckForSerializableConflictOut
 *		我们正在读取一个元组。若它不可见，可能与插入者存在
 *		向外的读写冲突。否则，若它对我们可见
 *		但已被删除，可能与删除者存在向外的读写冲突。
 *
 * 我们将确定可能与之冲突的写入事务的顶层 xid，
 * 并请 CheckForSerializableConflictOut() 检查
 * 是否与我们自己的事务重叠。
 *
 * 在 heapam.c 里几乎任何读过元组的地方都应当调用本函数。
 * 调用方必须至少持有缓冲区上的共享锁，
 * 因为本函数可能在元组上设置提示位。目前
 * 没有已知理由从索引访问方法调用本函数。
 */
void
HeapCheckForSerializableConflictOut(bool visible, Relation relation,
									HeapTuple tuple, Buffer buffer,
									Snapshot snapshot)
{
	TransactionId xid;
	HTSV_Result htsvResult;

	if (!CheckForSerializableConflictOutNeeded(relation, snapshot))
		return;

	/*
	 * 检查元组是否已被并发事务写入，
	 * 要么创建了对我们不可见的元组，要么在它对我们可见时
	 * 删除了它。"visible" 布尔值指出该元组
	 * 是否对我们可见，而 HeapTupleSatisfiesVacuum 检查此外
	 * 还发生了什么。
	 *
	 * 若并发插入的元组碰巧也被
	 * 另一个事务并发更新，将使用该元组的 xmin
	 * ——而不是更新者的 xid。
	 */
	htsvResult = HeapTupleSatisfiesVacuum(tuple, TransactionXmin, buffer);
	switch (htsvResult)
	{
		case HEAPTUPLE_LIVE:
			if (visible)
				return;
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_RECENTLY_DEAD:
		case HEAPTUPLE_DELETE_IN_PROGRESS:
			if (visible)
				xid = HeapTupleHeaderGetUpdateXid(tuple->t_data);
			else
				xid = HeapTupleHeaderGetXmin(tuple->t_data);

			if (TransactionIdPrecedes(xid, TransactionXmin))
			{
				/* 这类似于 HEAPTUPLE_DEAD 情形 */
				Assert(!visible);
				return;
			}
			break;
		case HEAPTUPLE_INSERT_IN_PROGRESS:
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_DEAD:
			Assert(!visible);
			return;
		default:

			/*
			 * 到达这个 default 子句的唯一方式，是枚举类型新增了值，
			 * 却没有把它加进这个 switch
			 * 语句。那是缺陷，因此 elog。
			 */
			elog(ERROR, "unrecognized return value from HeapTupleSatisfiesVacuum: %u", htsvResult);

			/*
			 * 尽管覆盖了所有枚举值，并在这个 default 上调用 elog，
			 * 有些编译器仍认为这是一条代码路径，
			 * 允许下面未经初始化就使用 xid。消除
			 * 该警告。
			 */
			xid = InvalidTransactionId;
	}

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/*
	 * 找出顶层 xid。若 xid 早到不可能成为冲突，或者
	 * 它是我们自己的 xid，就返回。
	 */
	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;
	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;

	CheckForSerializableConflictOut(relation, xid, snapshot);
}
