/*-------------------------------------------------------------------------
 *
 * pruneheap.c
 *	  堆页面剪枝与 HOT 链管理代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/pruneheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/snapmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* heap_page_prune 及其子程序的工作数据 */
typedef struct
{
	Relation	rel;

	/* 用于测试元组可见性的状态；按关系初始化 */
	TransactionId oldest_xmin;
	GlobalVisState *vistest;

	/*
	 * 由 TransactionIdLimitedForOldSnapshots() 设置的阈值，若它们已经
	 * 被计算出来（按需进行，且仅当
	 * OldSnapshotThresholdActive()）。第一次即将基于
	 * 受限视界移除某个元组时，把 old_snap_used 设为 true，并
	 * 调用 SetOldSnapshotThresholdTimestamp()。参见
	 * heap_prune_satisfies_vacuum().
	 */
	TimestampTz old_snap_ts;
	TransactionId old_snap_xmin;
	bool		old_snap_used;

	TransactionId new_prune_xid;	/* 该页新的剪枝提示值 */
	TransactionId snapshotConflictHorizon;	/* 被移除的最新 xid */
	int			nredirected;	/* 下面各数组中的条目数 */
	int			ndead;
	int			nunused;
	/* 累积待变更项下标的数组 */
	OffsetNumber redirected[MaxHeapTuplesPerPage * 2];
	OffsetNumber nowdead[MaxHeapTuplesPerPage];
	OffsetNumber nowunused[MaxHeapTuplesPerPage];

	/*
	 * 若项 i 已进入上面某个数组，则 marked[i] 为真。
	 *
	 * 由于 FirstOffsetNumber 为 1，长度须为 MaxHeapTuplesPerPage + 1。
	 * 否则每次访问都要减 1。
	 */
	bool		marked[MaxHeapTuplesPerPage + 1];

	/*
	 * 每个元组的可见性只计算一次，这是出于正确性
	 * 与效率；细节见 heap_page_prune() 中的注释。
	 * 类型用 int8[] 而不是 HTSV_Result[]，这样可以用 -1
	 * 表示尚未计算可见性，例如对 LP_DEAD 项。
	 *
	 * 下标方式与 ->marked 相同。
	 */
	int8		htsv[MaxHeapTuplesPerPage + 1];
} PruneState;

/* 局部函数 */
static HTSV_Result heap_prune_satisfies_vacuum(PruneState *prstate,
											   HeapTuple tup,
											   Buffer buffer);
static int	heap_prune_chain(Buffer buffer,
							 OffsetNumber rootoffnum,
							 PruneState *prstate);
static void heap_prune_record_prunable(PruneState *prstate, TransactionId xid);
static void heap_prune_record_redirect(PruneState *prstate,
									   OffsetNumber offnum, OffsetNumber rdoffnum);
static void heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum);
static void heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum);
static void page_verify_redirects(Page page);


/*
 * 按需对指定页面做剪枝并修复碎片。
 *
 * 这是一个伺机函数。它只在如下条件下做整理：
 * 页面按启发式看起来是剪枝候选，并且我们
 * 能够在不阻塞的情况下取得缓冲区清理锁。
 *
 * 注意：此函数调用很频繁。若剪枝并无用处，它必须迅速返回，
 * 这一点很重要。
 *
 * 调用方必须已钉住该缓冲区，并且不得持有其上的锁。
 */
void
heap_page_prune_opt(Relation relation, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prune_xid;
	GlobalVisState *vistest;
	TransactionId limited_xmin = InvalidTransactionId;
	TimestampTz limited_ts = 0;
	Size		minfree;

	/*
	 * 恢复模式下无法写 WAL，因此没有必要尝试
	 * 清理该页。主库很可能很快就会发出一条清理用的 WAL 记录，
	 * 所以这并没有什么特别损失。
	 */
	if (RecoveryInProgress())
		return;

	/*
	 * XXX: 权宜手段，让 old_snapshot_threshold 测试看起来"还能工作"。它们
	 * 目前是坏的，关于如何处理的讨论
	 * 仍在进行。参见
	 * https://www.postgresql.org/message-id/20200403001235.e6jfdll3gh2ygbuc%40alap3.anarazel.de
	 */
	if (old_snapshot_threshold == 0)
		SnapshotTooOldMagicForTest();

	/*
	 * 首先检查是否还有任何东西值得剪枝，
	 * 若没有 prune_xid，去确定合适的视界就是浪费
	 * （也就是没有更新/删除留下可能已死的元组）。
	 */
	prune_xid = ((PageHeader) page)->pd_prune_xid;
	if (!TransactionIdIsValid(prune_xid))
		return;

	/*
	 * 检查 prune_xid 是否表明可能有死行可以
	 * 被清理。
	 *
	 * 在取得清理锁之前检查旧快照限制是可以的，
	 * 因为最坏情况不过是清理没有那么
	 * 激进（相差的事务 ID 数目，就是从此时到取得锁之间
	 * 所消耗的那些）。这样我们就能够
	 * 在发现该页不可剪枝时省下可观的
	 * 开销。
	 *
	 * 即便设置了 old_snapshot_threshold，我们也先检查该页
	 * 能否在不依赖它的情况下剪枝。一方面是因为
	 * TransactionIdLimitedForOldSnapshots() 并不便宜，另一方面只有在
	 * 必要时才依赖 old_snapshot_threshold，以免引发
	 * 冲突。
	 */
	vistest = GlobalVisTestFor(relation);

	if (!GlobalVisTestIsRemovableXid(vistest, prune_xid))
	{
		if (!OldSnapshotThresholdActive())
			return;

		if (!TransactionIdLimitedForOldSnapshots(GlobalVisTestNonRemovableHorizon(vistest),
												 relation,
												 &limited_xmin, &limited_ts))
			return;

		if (!TransactionIdPrecedes(prune_xid, limited_xmin))
			return;
	}

	/*
	 * 若先前一次 UPDATE 在该页上找不到足够空间来存放
	 * 新的元组版本，或者空闲空间低于该关系的
	 * 填充因子目标（但不低于 10%），我们就做剪枝。
	 *
	 * 在这里检查空闲空间值得商榷，因为我们并未持有
	 * 缓冲区上的任何锁；最坏情况下会得到错误答案。不过
	 * 它不太可能*严重*出错，因为读取 pd_lower
	 * 或 pd_upper 很可能是原子的。避免加锁似乎比
	 * 偶尔得到错误答案更重要，毕竟这终究
	 * 只是启发式估计。
	 */
	minfree = RelationGetTargetPageFreeSpace(relation,
											 HEAP_DEFAULT_FILLFACTOR);
	minfree = Max(minfree, BLCKSZ / 10);

	if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
	{
		/* 好，尝试取得排他缓冲区锁 */
		if (!ConditionalLockBufferForCleanup(buffer))
			return;

		/*
		 * 既然已经持有缓冲区锁，就取得关于
		 * 该页空闲空间的准确信息，并重新检查是否应当
		 * 剪枝这一启发式判断。
		 */
		if (PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree)
		{
			int			ndeleted,
						nnewlpdead;

			ndeleted = heap_page_prune(relation, buffer, InvalidTransactionId,
									   vistest, limited_xmin,
									   limited_ts, &nnewlpdead, NULL);

			/*
			 * 向 pgstats 报告回收的元组数。这等于
			 * ndeleted 减去新被设为 LP_DEAD 的项数。
			 *
			 * 我们这样推导死元组数目，以免完全
			 * 忘掉被设为 LP_DEAD 的项，因为它们
			 * 仍需由 VACUUM 清理。我们的报告只想计入
			 * 刚刚变成 LP_UNUSED 的堆内元组，
			 * 那些项并不计入。
			 *
			 * VACUUM 跟踪 ndeleted 时不必以同样方式补偿，
			 * 因为它会把同样这些 LP_DEAD 项
			 * 另行设为 LP_UNUSED。
			 */
			if (ndeleted > nnewlpdead)
				pgstat_update_heap_dead_tuples(relation,
											   ndeleted - nnewlpdead);
		}

		/* 然后释放缓冲区锁 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * 我们选择此时不更新空闲空间映射（FSM），以免无关的
		 * UPDATEs/INSERTs 复用该页上新产生的空闲空间。这些
		 * 空闲空间应当由对本页的 UPDATEs 来复用。
		 */
	}
}


/*
 * 对指定页面做剪枝并修复碎片。
 *
 * 调用方必须已钉住该页并持有缓冲区清理锁。注意我们
 * 不会替调用方更新该页的 FSM 信息。调用方可能
 * 还需要计入行指针数组长度的缩短，
 * 那是我们截断该数组造成的。
 *
 * vistest 与 oldest_xmin 用来区分元组是 DEAD 还是
 * RECENTLY_DEAD（见 heap_prune_satisfies_vacuum 与
 * HeapTupleSatisfiesVacuum）。若调用方提供了 oldest_xmin，则
 * 在查阅 GlobalVisState 之前先使用它。
 *
 * old_snap_xmin / old_snap_ts 必须已经由
 * TransactionIdLimitedForOldSnapshots 设置，或者分别为 InvalidTransactionId/0，
 * 二者一一对应。
 *
 * 为调用方设置 *nnewlpdead，表示那些项的数目，它们
 * 在剪枝操作期间新被设为 LP_DEAD。
 *
 * off_loc 是调用方要求在错误
 * 回调中使用的偏移位置。
 *
 * 返回本次调用从该页删除的元组数。
 */
int
heap_page_prune(Relation relation, Buffer buffer,
				TransactionId oldest_xmin,
				GlobalVisState *vistest,
				TransactionId old_snap_xmin,
				TimestampTz old_snap_ts,
				int *nnewlpdead,
				OffsetNumber *off_loc)
{
	int			ndeleted = 0;
	Page		page = BufferGetPage(buffer);
	BlockNumber blockno = BufferGetBlockNumber(buffer);
	OffsetNumber offnum,
				maxoff;
	PruneState	prstate;
	HeapTupleData tup;

	/*
	 * 策略是先扫描该页，列出待变更的项，
	 * 再在临界区内应用这些变更。这样可以把尽可能多的
	 * 逻辑留在临界区之外，并且保证
	 * WAL 重放与正常情形行为一致。
	 *
	 * 首先把新的 pd_prune_xid 初始化为零（表示没有
	 * 可剪枝元组）。若发现任何可能很快变得
	 * 可剪枝的元组，就把相关的最低 XID 存入 new_prune_xid。同时
	 * 初始化其余工作状态。
	 */
	prstate.new_prune_xid = InvalidTransactionId;
	prstate.rel = relation;
	prstate.oldest_xmin = oldest_xmin;
	prstate.vistest = vistest;
	prstate.old_snap_xmin = old_snap_xmin;
	prstate.old_snap_ts = old_snap_ts;
	prstate.old_snap_used = false;
	prstate.snapshotConflictHorizon = InvalidTransactionId;
	prstate.nredirected = prstate.ndead = prstate.nunused = 0;
	memset(prstate.marked, 0, sizeof(prstate.marked));

	maxoff = PageGetMaxOffsetNumber(page);
	tup.t_tableOid = RelationGetRelid(prstate.rel);

	/*
	 * 为所有元组确定 HTSV。
	 *
	 * 出于正确性必须这样做，以处理把 HTSV
	 * 运行两次会得到不同结果的情形（例如 RECENTLY_DEAD 可以变成
	 * DEAD，若另一个被检查的项使 GlobalVisTestIsRemovableFullXid()
	 * 更新了视界；INSERT_IN_PROGRESS 可以变成 DEAD，若插入
	 * 事务中止，……）。那又可能导致
	 * heap_prune_chain() 在同一元组被到达两次时行为不正确，
	 * 一次直接经由 heap_prune_chain()，一次沿着 HOT 链。
	 *
	 * 这对性能也有好处。页内元组最常见的存放方式是
	 * 偏移递减（而项本身按递增的
	 * 偏移存放）。处理一页上的全部元组时，就会按递减偏移读取
	 * 页内内存，而且步长不固定。
	 * CPU 预取器很难应付。按相反顺序处理这些项
	 * （于是元组按递增顺序被处理）能显著提高
	 * 预取效率，并减少缓存
	 * 未命中次数。
	 */
	for (offnum = maxoff;
		 offnum >= FirstOffsetNumber;
		 offnum = OffsetNumberPrev(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		HeapTupleHeader htup;

		/* 若槽位不含元组则无事可做 */
		if (!ItemIdIsNormal(itemid))
		{
			prstate.htsv[offnum] = -1;
			continue;
		}

		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		tup.t_data = htup;
		tup.t_len = ItemIdGetLength(itemid);
		ItemPointerSet(&(tup.t_self), blockno, offnum);

		/*
		 * 设置偏移号，以便连同处理该元组时发生的任何
		 * 错误一起显示出来。
		 */
		if (off_loc)
			*off_loc = offnum;

		prstate.htsv[offnum] = heap_prune_satisfies_vacuum(&prstate, &tup,
														   buffer);
	}

	/* 扫描该页 */
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;

		/* 忽略已经作为更早某条链的一部分处理过的项 */
		if (prstate.marked[offnum])
			continue;

		/* 见前面的循环 */
		if (off_loc)
			*off_loc = offnum;

		/* 若槽位为空或已经死亡则无事可做 */
		itemid = PageGetItemId(page, offnum);
		if (!ItemIdIsUsed(itemid) || ItemIdIsDead(itemid))
			continue;

		/* 处理此项，或由此项开始的一串项 */
		ndeleted += heap_prune_chain(buffer, offnum, &prstate);
	}

	/* 处理完给定页面后，清除偏移信息。 */
	if (off_loc)
		*off_loc = InvalidOffsetNumber;

	/* 应用这些变更时的任何错误都是致命的 */
	START_CRIT_SECTION();

	/* 是否找到了可剪枝的项？ */
	if (prstate.nredirected > 0 || prstate.ndead > 0 || prstate.nunused > 0)
	{
		/*
		 * 应用计划中的项变更，然后修复页面碎片，并
		 * 更新该页关于是否还有空闲行指针的提示位。
		 */
		heap_page_prune_execute(buffer,
								prstate.redirected, prstate.nredirected,
								prstate.nowdead, prstate.ndead,
								prstate.nowunused, prstate.nunused);

		/*
		 * 把该页的 pd_prune_xid 字段更新为零，或更新为最低的那个
		 * 即将可剪枝元组的 XID。
		 */
		((PageHeader) page)->pd_prune_xid = prstate.new_prune_xid;

		/*
		 * 同时清除"页面已满"标志，因为没有必要
		 * 重复剪枝/碎片整理过程，直到别的事情发生在
		 * 该页上。
		 */
		PageClearFull(page);

		MarkBufferDirty(buffer);

		/*
		 * 发出一条 WAL 记录 XLOG_HEAP2_PRUNE，说明我们所做的事
		 */
		if (RelationNeedsWAL(relation))
		{
			xl_heap_prune xlrec;
			XLogRecPtr	recptr;

			xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(relation);
			xlrec.snapshotConflictHorizon = prstate.snapshotConflictHorizon;
			xlrec.nredirected = prstate.nredirected;
			xlrec.ndead = prstate.ndead;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfHeapPrune);

			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

			/*
			 * 这些 OffsetNumber 数组实际上并不在缓冲区里，但我们
			 * 假装它们在。当 XLogInsert 存储整个
			 * 缓冲区时，偏移数组就不必再存一遍。
			 */
			if (prstate.nredirected > 0)
				XLogRegisterBufData(0, (char *) prstate.redirected,
									prstate.nredirected *
									sizeof(OffsetNumber) * 2);

			if (prstate.ndead > 0)
				XLogRegisterBufData(0, (char *) prstate.nowdead,
									prstate.ndead * sizeof(OffsetNumber));

			if (prstate.nunused > 0)
				XLogRegisterBufData(0, (char *) prstate.nowunused,
									prstate.nunused * sizeof(OffsetNumber));

			recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_PRUNE);

			PageSetLSN(BufferGetPage(buffer), recptr);
		}
	}
	else
	{
		/*
		 * 若没有剪掉任何东西，但为
		 * pd_prune_xid 字段找到了新值，就更新它并把缓冲区标脏。这被
		 * 当作不记 WAL 的提示。
		 *
		 * 若已设置"页面已满"标志，也把它清除，因为没有
		 * 必要重复剪枝/碎片整理过程，直到别的事情
		 * 发生在该页上。
		 */
		if (((PageHeader) page)->pd_prune_xid != prstate.new_prune_xid ||
			PageIsFull(page))
		{
			((PageHeader) page)->pd_prune_xid = prstate.new_prune_xid;
			PageClearFull(page);
			MarkBufferDirtyHint(buffer, true);
		}
	}

	END_CRIT_SECTION();

	/* 为调用方记录新被设为 LP_DEAD 的项数 */
	*nnewlpdead = prstate.ndead;

	return ndeleted;
}


/*
 * 为堆剪枝执行可见性检查。
 *
 * 这比只使用 GlobalVisTestIsRemovableXid() 更复杂，
 * 原因是 old_snapshot_threshold。我们只想在真正决定移除一行时
 * 才提高那个会让旧快照报错的阈值，而且移除必须是
 * 基于受限视界做出的。
 *
 * 由于其代价，我们也只想在必要时才调用
 * TransactionIdLimitedForOldSnapshots()，也就是说，若 pd_prune_xid 已经足够旧，
 * 在 heap_page_prune_opt() 里可能还没有调用过。但我们
 * 仍希望能移除那些按 prstate->vistest 看来太新、尚不能移除，
 * 却可以基于
 * old_snapshot_threshold 移除的行。因此我们在这里按需调用 TransactionIdLimitedForOldSnapshots()，
 * 在合适的时候。
 */
static HTSV_Result
heap_prune_satisfies_vacuum(PruneState *prstate, HeapTuple tup, Buffer buffer)
{
	HTSV_Result res;
	TransactionId dead_after;

	res = HeapTupleSatisfiesVacuumHorizon(tup, buffer, &dead_after);

	if (res != HEAPTUPLE_RECENTLY_DEAD)
		return res;

	/*
	 * 若我们已经在依赖这个受限 xmin，就没有必要
	 * 再推迟这样做。
	 */
	if (prstate->old_snap_used)
	{
		Assert(TransactionIdIsValid(prstate->old_snap_xmin));

		if (TransactionIdPrecedes(dead_after, prstate->old_snap_xmin))
			res = HEAPTUPLE_DEAD;
		return res;
	}

	/*
	 * 对 VACUUM 而言，必须剪掉 xmax 早于
	 * oldest_xmin 的元组——oldest_xmin 是开始
	 * 清理该关系时确定的可见性截止点。oldest_xmin 用于冻结判定，
	 * 而我们无法冻结死元组的 xmax。
	 */
	if (TransactionIdIsValid(prstate->oldest_xmin) &&
		NormalTransactionIdPrecedes(dead_after, prstate->oldest_xmin))
		return HEAPTUPLE_DEAD;

	/*
	 * 判断与所提供的 GlobalVisState 相比，该元组是否视为已死。
	 * 访问时剪枝并不提供
	 * oldest_xmin。而对清理而言，即便元组的 xmax 并不早于
	 * oldest_xmin，GlobalVisTestIsRemovableXid() 仍可能判定该行已死，只要
	 * GlobalVisState 自开始清理
	 * 该关系以来已被更新。
	 */
	if (GlobalVisTestIsRemovableXid(prstate->vistest, dead_after))
		return HEAPTUPLE_DEAD;

	/*
	 * 若 GlobalVisTestIsRemovableXid() 不足以判定该行已死，
	 * 并且启用了 old_snapshot_threshold，就尝试使用降低后的视界。
	 */
	if (OldSnapshotThresholdActive())
	{
		/* 尚未确定受限视界，此处发起请求 */
		if (!TransactionIdIsValid(prstate->old_snap_xmin))
		{
			TransactionId horizon =
				GlobalVisTestNonRemovableHorizon(prstate->vistest);

			TransactionIdLimitedForOldSnapshots(horizon, prstate->rel,
												&prstate->old_snap_xmin,
												&prstate->old_snap_ts);
		}

		if (TransactionIdIsValid(prstate->old_snap_xmin) &&
			TransactionIdPrecedes(dead_after, prstate->old_snap_xmin))
		{
			/*
			 * 即将基于 snapshot_too_old 移除该行。需要提高
			 * 阈值，以便有问题的访问会报错。
			 */
			Assert(!prstate->old_snap_used);
			SetOldSnapshotThresholdTimestamp(prstate->old_snap_ts,
											 prstate->old_snap_xmin);
			prstate->old_snap_used = true;
			res = HEAPTUPLE_DEAD;
		}
	}

	return res;
}


/*
 * 剪枝指定的行指针，或从该行指针出发的一条 HOT 链。
 *
 * 若该项是被索引引用的元组（也就是不是堆内元组），
 * HOT 链的剪枝方式是移除 HOT 链开头的全部 DEAD 元组。
 * 位于某个 DEAD 元组之前的 RECENTLY_DEAD 元组也一并剪掉。
 * 这样做可以，因为 DEAD 元组之前的 RECENTLY_DEAD 元组其实已经
 * DEAD，只是我们的可见性测试太粗，检测不出来。
 *
 * 一般而言，剪枝绝不能留下仍然占有元组存储的 DEAD 元组。
 * VACUUM 没准备处理那种情形。因此
 * VACUUM 会再次剪枝同一堆页，并且中途不放开它的锁，
 * 当它看到一个新变成 DEAD、而我们最初看成进行中的元组时。
 * 只有插入
 * 事务并发中止时，才会这样重试剪枝。
 *
 * 根行指针被重定向到紧接在
 * 最近一个 DEAD 元组之后的元组。若链中元组全部 DEAD，根行
 * 指针被标为 LP_DEAD。（这也包括单独一个 DEAD
 * 元组的情形，我们把它当作长度为 1 的链。）
 *
 * 这里并不真正修改页面。只是向 prstate 里的数组添加条目，
 * 以表示将要做的变更。待重定向的项被加入
 * redirected[] 数组（每次重定向占两项）；要设为
 * LP_DEAD 状态的项加入 nowdead[]；要设为 LP_UNUSED
 * 状态的项加入 nowunused[]。
 *
 * 返回（将要）从该页删除的元组数。
 */
static int
heap_prune_chain(Buffer buffer, OffsetNumber rootoffnum, PruneState *prstate)
{
	int			ndeleted = 0;
	Page		dp = (Page) BufferGetPage(buffer);
	TransactionId priorXmax = InvalidTransactionId;
	ItemId		rootlp;
	HeapTupleHeader htup;
	OffsetNumber latestdead = InvalidOffsetNumber,
				maxoff = PageGetMaxOffsetNumber(dp),
				offnum;
	OffsetNumber chainitems[MaxHeapTuplesPerPage];
	int			nchain = 0,
				i;

	rootlp = PageGetItemId(dp, rootoffnum);

	/*
	 * 若它是堆内元组，就不是一条 HOT 链的起点。
	 */
	if (ItemIdIsNormal(rootlp))
	{
		Assert(prstate->htsv[rootoffnum] != -1);
		htup = (HeapTupleHeader) PageGetItem(dp, rootlp);

		if (HeapTupleHeaderIsHeapOnly(htup))
		{
			/*
			 * 若该元组是 DEAD，并且不再链接到任何其他元组，就把它标为
			 * 未使用，立即进行。（若它确实还链接着，我们就只能在
			 * 剪枝其所属链时把它移除。）
			 *
			 * 这主要用来处理已中止的 HOT 更新，也就是
			 * XMIN_INVALID 的堆内元组。它们可能没有被任何
			 * 链链接到，因为父元组可能在剪枝发生之前又被更新了一次。
			 * 因此我们必须能够单独回收它们，
			 * 与按链剪枝分开。（注意
			 * HeapTupleHeaderIsHotUpdated 对一个
			 * XMIN_INVALID 元组永远不会返回真，因此即便
			 * 已中止的事务内部有过一连串更新，这段代码仍然有效。）
			 *
			 * 注意，我们第一次遇到已死的堆内元组，
			 * 可能是在这里，也可能是在下面跟随某条链时。哪条路径
			 * 先到达，就会把该元组标为未使用。
			 */
			if (prstate->htsv[rootoffnum] == HEAPTUPLE_DEAD &&
				!HeapTupleHeaderIsHotUpdated(htup))
			{
				heap_prune_record_unused(prstate, rootoffnum);
				HeapTupleHeaderAdvanceConflictHorizon(htup,
													  &prstate->snapshotConflictHorizon);
				ndeleted++;
			}

			/* 没有更多事情要做 */
			return ndeleted;
		}
	}

	/* 从根元组开始 */
	offnum = rootoffnum;

	/* 当尚未到达链的末端时 */
	for (;;)
	{
		ItemId		lp;
		bool		tupdead,
					recent_dead;

		/* 健全性检查（纯粹的偏执） */
		if (offnum < FirstOffsetNumber)
			break;

		/*
		 * 偏移超出该页行指针数组的末尾是可能的，
		 * 当数组已被截断时（原项必定未曾使用）
		 */
		if (offnum > maxoff)
			break;

		/* 若该项已经处理过，就停止——它不得属于同一条链 */
		if (prstate->marked[offnum])
			break;

		lp = PageGetItemId(dp, offnum);

		/* 未使用的项显然不属于这条链 */
		if (!ItemIdIsUsed(lp))
			break;

		/*
		 * 若看到的是已重定向的根行指针，就跳到
		 * 链中第一个普通元组。若在别的地方发现重定向，
		 * 就停止——它不得属于同一条链。
		 */
		if (ItemIdIsRedirected(lp))
		{
			if (nchain > 0)
				break;			/* 不在链的起点 */
			chainitems[nchain++] = offnum;
			offnum = ItemIdGetRedirect(rootlp);
			continue;
		}

		/*
		 * 同样，死行指针不可能属于这条链。（我们
		 * 已经在本函数之外排除了死根元组的情形，
		 * ）
		 */
		if (ItemIdIsDead(lp))
			break;

		Assert(ItemIdIsNormal(lp));
		Assert(prstate->htsv[offnum] != -1);
		htup = (HeapTupleHeader) PageGetItem(dp, lp);

		/*
		 * 若有的话，对照先前的 XMAX 检查该元组的 XMIN
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), priorXmax))
			break;

		/*
		 * 好，这个元组确实是该链的成员。
		 */
		chainitems[nchain++] = offnum;

		/*
		 * 检查该元组的可见性状态。
		 */
		tupdead = recent_dead = false;

		switch ((HTSV_Result) prstate->htsv[offnum])
		{
			case HEAPTUPLE_DEAD:
				tupdead = true;
				break;

			case HEAPTUPLE_RECENTLY_DEAD:
				recent_dead = true;

				/*
				 * 这个元组可能很快变成 DEAD。更新提示字段，以便
				 * 将来重新考虑对该页做剪枝。
				 */
				heap_prune_record_prunable(prstate,
										   HeapTupleHeaderGetUpdateXid(htup));
				break;

			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * 这个元组可能很快变成 DEAD。更新提示字段，以便
				 * 将来重新考虑对该页做剪枝。
				 */
				heap_prune_record_prunable(prstate,
										   HeapTupleHeaderGetUpdateXid(htup));
				break;

			case HEAPTUPLE_LIVE:
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * 若想为中止情形做优化，我们可能会考虑
				 * 一看到 INSERT_IN_PROGRESS 就把该页标为可剪枝。
				 * 但我们不这么做。关于何时把该页标为
				 * 可剪枝，见 heapam.c 中的相关决定。
				 */
				break;

			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}

		/*
		 * 记住最后看到的 DEAD 元组。我们会越过
		 * RECENTLY_DEAD 元组，以防万一它们后面还有 DEAD 元组；
		 * 但不能越过任何其他状态。我们必须确保
		 * 不漏掉任何 DEAD 元组，因为剪枝之后仍然占有
		 * 元组存储的 DEAD 元组会使 VACUUM 混淆。
		 */
		if (tupdead)
		{
			latestdead = offnum;
			HeapTupleHeaderAdvanceConflictHorizon(htup,
												  &prstate->snapshotConflictHorizon);
		}
		else if (!recent_dead)
			break;

		/*
		 * 若该元组不是 HOT 更新产生的，那么我们就到了这条
		 * HOT 更新链的末端。
		 */
		if (!HeapTupleHeaderIsHotUpdated(htup))
			break;

		/* HOT 意味着它不可能已被移到不同分区 */
		Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

		/*
		 * 前进到链的下一个成员。
		 */
		Assert(ItemPointerGetBlockNumber(&htup->t_ctid) ==
			   BufferGetBlockNumber(buffer));
		offnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
		priorXmax = HeapTupleHeaderGetUpdateXid(htup);
	}

	/*
	 * 若在链中找到 DEAD 元组，就调整 HOT 链，使开头全部
	 * DEAD 元组被移除，并且根行
	 * 指针被恰当地重定向。
	 */
	if (OffsetNumberIsValid(latestdead))
	{
		/*
		 * 把每一个能够从链中移除的中间项标为未使用，
		 * 并把它们从链中去掉。
		 *
		 * 当前一项就是最后看到的死元组时，我们就处在
		 * 适合做重定向的候选位置。
		 */
		for (i = 1; (i < nchain) && (chainitems[i - 1] != latestdead); i++)
		{
			heap_prune_record_unused(prstate, chainitems[i]);
			ndeleted++;
		}

		/*
		 * 若根条目原来是普通元组，我们正在删除它，因此
		 * 把它计入结果。但改变一个重定向（即便改成 DEAD
		 * 状态）并不计入。
		 */
		if (ItemIdIsNormal(rootlp))
			ndeleted++;

		/*
		 * 若这个 DEAD 元组位于链的末端，则整条链都已
		 * 死亡，根行指针可以标为死。否则只是
		 * 把根重定向到正确的链成员。
		 */
		if (i >= nchain)
			heap_prune_record_dead(prstate, rootoffnum);
		else
			heap_prune_record_redirect(prstate, rootoffnum, chainitems[i]);
	}
	else if (nchain < 2 && ItemIdIsRedirected(rootlp))
	{
		/*
		 * 我们发现一个重定向项，它没有指向有效的后续
		 * 项。若 heap_page_prune 里的循环使我们
		 * 在访问该重定向项之前先访问了它的已死后继，
		 * 就会发生这种情况。可以把该重定向项设为
		 * DEAD 状态来完成清理。
		 */
		heap_prune_record_dead(prstate, rootoffnum);
	}

	return ndeleted;
}

/* 记录最低的即将可剪枝 XID */
static void
heap_prune_record_prunable(PruneState *prstate, TransactionId xid)
{
	/*
	 * 这应当与 PageSetPrunable 宏完全一致。我们还无法
	 * 直接写入页头，因此先更新工作状态。
	 */
	Assert(TransactionIdIsNormal(xid));
	if (!TransactionIdIsValid(prstate->new_prune_xid) ||
		TransactionIdPrecedes(xid, prstate->new_prune_xid))
		prstate->new_prune_xid = xid;
}

/* 记录待重定向的行指针 */
static void
heap_prune_record_redirect(PruneState *prstate,
						   OffsetNumber offnum, OffsetNumber rdoffnum)
{
	Assert(prstate->nredirected < MaxHeapTuplesPerPage);
	prstate->redirected[prstate->nredirected * 2] = offnum;
	prstate->redirected[prstate->nredirected * 2 + 1] = rdoffnum;
	prstate->nredirected++;
	Assert(!prstate->marked[offnum]);
	prstate->marked[offnum] = true;
	Assert(!prstate->marked[rdoffnum]);
	prstate->marked[rdoffnum] = true;
}

/* 记录待标为死的行指针 */
static void
heap_prune_record_dead(PruneState *prstate, OffsetNumber offnum)
{
	Assert(prstate->ndead < MaxHeapTuplesPerPage);
	prstate->nowdead[prstate->ndead] = offnum;
	prstate->ndead++;
	Assert(!prstate->marked[offnum]);
	prstate->marked[offnum] = true;
}

/* 记录待标为未使用的行指针 */
static void
heap_prune_record_unused(PruneState *prstate, OffsetNumber offnum)
{
	Assert(prstate->nunused < MaxHeapTuplesPerPage);
	prstate->nowunused[prstate->nunused] = offnum;
	prstate->nunused++;
	Assert(!prstate->marked[offnum]);
	prstate->marked[offnum] = true;
}


/*
 * 执行 heap_page_prune 所需的实际页面变更。
 * 预期调用方对该
 * 缓冲区持有完整的清理锁。
 */
void
heap_page_prune_execute(Buffer buffer,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused)
{
	Page		page = (Page) BufferGetPage(buffer);
	OffsetNumber *offnum;
	HeapTupleHeader htup PG_USED_FOR_ASSERTS_ONLY;

	/* 除非真有事情要做，否则不应调用 */
	Assert(nredirected > 0 || ndead > 0 || nunused > 0);

	/* 更新所有已重定向的行指针 */
	offnum = redirected;
	for (int i = 0; i < nredirected; i++)
	{
		OffsetNumber fromoff = *offnum++;
		OffsetNumber tooff = *offnum++;
		ItemId		fromlp = PageGetItemId(page, fromoff);
		ItemId		tolp PG_USED_FOR_ASSERTS_ONLY;

#ifdef USE_ASSERT_CHECKING

		/*
		 * 任何被我们设为 LP_REDIRECT 的现有项（任一 'from' 项）
		 * 都必须是某条 HOT 链的第一项。若该项仍有元组
		 * 存储，它就不可能是堆内元组。否则我们只是在
		 * 维护一条既有 HOT 链上已经存在的 LP_REDIRECT，而该链
		 * 在此之前至少已经被剪枝过一次。
		 */
		if (!ItemIdIsRedirected(fromlp))
		{
			Assert(ItemIdHasStorage(fromlp) && ItemIdIsNormal(fromlp));

			htup = (HeapTupleHeader) PageGetItem(page, fromlp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* 应当不需要把这个重定向再设置一遍 */
			Assert(ItemIdGetRedirect(fromlp) != tooff);
		}

		/*
		 * 即将被设为 LP_REDIRECT 的项（'from'
		 * 项）会指向一个已经存在的项（'to' 项），而该目标
		 * 已经是堆内元组。每条 HOT 链上至多有一个 LP_REDIRECT
		 * 项。
		 *
		 * 需要保留一个 LP_REDIRECT 项（在原来那个
		 * 非堆内根元组被剪掉之后），这样才始终
		 * 能够让 VACUUM 方便地弄清要从索引中删除哪个 TID，
		 * 当整条 HOT 链变成死链时。堆内元组
		 * 永远不能变成 LP_DEAD；LP_REDIRECT 项或普通的堆
		 * 元组则可以。
		 *
		 * 这项检查可能漏掉问题，例如重定向的目标随后可能
		 * 被标为未使用。下面的 page_verify_redirects() 检查
		 * 会抓住这类问题。
		 */
		tolp = PageGetItemId(page, tooff);
		Assert(ItemIdHasStorage(tolp) && ItemIdIsNormal(tolp));
		htup = (HeapTupleHeader) PageGetItem(page, tolp);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
#endif

		ItemIdSetRedirect(fromlp, tooff);
	}

	/* 更新所有现已死亡的行指针 */
	offnum = nowdead;
	for (int i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		/*
		 * 必须留下 LP_DEAD 行指针，当原始项
		 * （对所有人都已死亡）仍可能被索引中的某个 TID 引用时。
		 * 对任何一个单独的项，这永远不应当是必要的。
		 * 对堆内元组项也是如此。（不清楚那会有多大问题，
		 * 但没有理由允许它。）
		 */
		if (ItemIdHasStorage(lp))
		{
			Assert(ItemIdIsNormal(lp));
			htup = (HeapTupleHeader) PageGetItem(page, lp);
			Assert(!HeapTupleHeaderIsHeapOnly(htup));
		}
		else
		{
			/* 整条 HOT 链变为死亡 */
			Assert(ItemIdIsRedirected(lp));
		}
#endif

		ItemIdSetDead(lp);
	}

	/* 更新所有现已未使用的行指针 */
	offnum = nowunused;
	for (int i = 0; i < nunused; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

#ifdef USE_ASSERT_CHECKING

		/*
		 * 剪枝期间只有堆内元组可以变成 LP_UNUSED。它们
		 * 不必作为 LP_DEAD 项留在原地，直到 VACUUM 着手
		 * 做索引清理。
		 */
		Assert(ItemIdHasStorage(lp) && ItemIdIsNormal(lp));
		htup = (HeapTupleHeader) PageGetItem(page, lp);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
#endif

		ItemIdSetUnused(lp);
	}

	/*
	 * 最后修复任何碎片，并更新该页的提示位，说明
	 * 它是否还有空闲指针。
	 */
	PageRepairFragmentation(page);

	/*
	 * 页面已经修改，现在断言各重定向项仍然
	 * 指向有效目标。
	 */
	page_verify_redirects(page);
}


/*
 * 若以带断言的方式构建，则验证每个 LP_REDIRECT 项都指向一个
 * 有效项。
 *
 * HOT 剪枝相关缺陷的一种表现是：重定向项指向
 * 已被移除的元组。要可靠地检查：把某项标为
 * 未使用，不会在 heap_prune_chain() /
 * heap_page_prune_execute() 期间把某个重定向项变成孤立项，并不容易，因此我们在
 * 剪枝之后再检查整页。没有这项检查，这类缺陷通常只会在稍后引发断言，
 * 而且可能远在损坏被引入之后。
 *
 * 同时参见 heap_page_prune_execute() 重定向循环里的注释。
 */
static void
page_verify_redirects(Page page)
{
#ifdef USE_ASSERT_CHECKING
	OffsetNumber offnum;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		OffsetNumber targoff;
		ItemId		targitem;
		HeapTupleHeader htup;

		if (!ItemIdIsRedirected(itemid))
			continue;

		targoff = ItemIdGetRedirect(itemid);
		targitem = PageGetItemId(page, targoff);

		Assert(ItemIdIsUsed(targitem));
		Assert(ItemIdIsNormal(targitem));
		Assert(ItemIdHasStorage(targitem));
		htup = (HeapTupleHeader) PageGetItem(page, targitem);
		Assert(HeapTupleHeaderIsHeapOnly(htup));
	}
#endif
}


/*
 * 对本页的每一项，找出它各自的根行指针。
 * 若项 k 属于以项 j 为根的 HOT 链，则设置
 * root_offsets[k - 1] = j.
 *
 * 传入的 root_offsets 数组必须有 MaxHeapTuplesPerPage 个条目。
 * 未使用的条目填入 InvalidOffsetNumber（零）。
 *
 * 调用本函数时，至少须对缓冲区持有共享锁，以
 * 防止并发剪枝。
 *
 * 注意：此处收集的信息只在调用方
 * 一直钉住该缓冲区期间有效。一旦解除钉住，元组就可能被剪枝，
 * 并被一个完全无关的元组复用。
 */
void
heap_get_root_tuples(Page page, OffsetNumber *root_offsets)
{
	OffsetNumber offnum,
				maxoff;

	MemSet(root_offsets, InvalidOffsetNumber,
		   MaxHeapTuplesPerPage * sizeof(OffsetNumber));

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
	{
		ItemId		lp = PageGetItemId(page, offnum);
		HeapTupleHeader htup;
		OffsetNumber nextoffnum;
		TransactionId priorXmax;

		/* 跳过未使用的项和死项 */
		if (!ItemIdIsUsed(lp) || ItemIdIsDead(lp))
			continue;

		if (ItemIdIsNormal(lp))
		{
			htup = (HeapTupleHeader) PageGetItem(page, lp);

			/*
			 * 检查这个元组是否属于以其他某个
			 * 元组为根的 HOT 链。若是，暂时跳过；等我们找到
			 * 它的根时再处理。
			 */
			if (HeapTupleHeaderIsHeapOnly(htup))
				continue;

			/*
			 * 这要么是普通元组，要么是一条 HOT 链的根。
			 * 把它记入映射。
			 */
			root_offsets[offnum - 1] = offnum;

			/* 若它不是一条 HOT 链的起点，对它的处理就结束了 */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				continue;

			/* 准备扫描这条 HOT 链 */
			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
		else
		{
			/* 这必须是重定向项。我们不设置它的 root_offsets 条目 */
			Assert(ItemIdIsRedirected(lp));
			/* 准备扫描这条 HOT 链 */
			nextoffnum = ItemIdGetRedirect(lp);
			priorXmax = InvalidTransactionId;
		}

		/*
		 * 现在沿着 HOT 链走，并收集链中的其他元组。
		 *
		 * 注意：尽管这是嵌套循环，本函数的复杂度
		 * 仍是 O(N)，因为页中的元组被访问的次数不应
		 * 超过两次：一次在外层循环，一次在追逐 HOT 链的
		 * 过程中。
		 */
		for (;;)
		{
			/* 健全性检查（纯粹的偏执） */
			if (offnum < FirstOffsetNumber)
				break;

			/*
			 * 偏移超出该页行指针数组的末尾是可能的，
			 * 当数组已被截断时
			 */
			if (offnum > maxoff)
				break;

			lp = PageGetItemId(page, nextoffnum);

			/* 检查是否有断裂的链 */
			if (!ItemIdIsNormal(lp))
				break;

			htup = (HeapTupleHeader) PageGetItem(page, lp);

			if (TransactionIdIsValid(priorXmax) &&
				!TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(htup)))
				break;

			/* 记住此项的根行指针 */
			root_offsets[nextoffnum - 1] = offnum;

			/* 若还有的话，前进到链的下一个成员 */
			if (!HeapTupleHeaderIsHotUpdated(htup))
				break;

			/* HOT 意味着它不可能已被移到不同分区 */
			Assert(!HeapTupleHeaderIndicatesMovedPartitions(htup));

			nextoffnum = ItemPointerGetOffsetNumber(&htup->t_ctid);
			priorXmax = HeapTupleHeaderGetUpdateXid(htup);
		}
	}
}
