/*-------------------------------------------------------------------------
 *
 * gistvacuum.c
 *	  postgres GiST 索引访问方法的清理例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistvacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/transam.h"
#include "commands/vacuum.h"
#include "lib/integerset.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

/* gistbulkdelete 所需的工作状态 */
typedef struct
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	GistNSN		startNSN;

	/*
	 * 用于记住所有内部页与空叶子页。它们
	 * 用于删除全部空页。
	 */
	IntegerSet *internal_page_set;
	IntegerSet *empty_leaf_set;
	MemoryContext page_set_context;
} GistVacState;

static void gistvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						   IndexBulkDeleteCallback callback, void *callback_state);
static void gistvacuumpage(GistVacState *vstate, BlockNumber blkno,
						   BlockNumber orig_blkno);
static void gistvacuum_delete_empty_pages(IndexVacuumInfo *info,
										  GistVacState *vstate);
static bool gistdeletepage(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						   Buffer parentBuffer, OffsetNumber downlink,
						   Buffer leafBuffer);

/*
 * VACUUM bulkdelete 阶段：移除索引条目。
 */
IndexBulkDeleteResult *
gistbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	/* 若是首次进入则分配 stats，否则复用已有结构 */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	gistvacuumscan(info, stats, callback, callback_state);

	return stats;
}

/*
 * VACUUM cleanup 阶段：删除空页，并更新索引统计信息。
 */
IndexBulkDeleteResult *
gistvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* ANALYZE ONLY 模式下为空操作 */
	if (info->analyze_only)
		return stats;

	/*
	 * 若已调用过 gistbulkdelete，则无需再做任何事，只需返回
	 * 最近一次 gistbulkdelete 调用的统计信息。若未调用过，我们
	 * 仍需遍历一遍索引，以获取索引统计信息。
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		gistvacuumscan(info, stats, NULL, NULL);
	}

	/*
	 * 我们很容易被并发页分裂迷惑，从而把某些索引元组重复计数，
	 * 因此若底层堆的计数是准确的，则不相信任何超过堆计数的
	 * 总数……若我们确实准确知道堆计数。否则这可能只会把事情
	 * 弄得更糟。
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}

/*
 * gistvacuumscan --- 为 VACUUMing 目的扫描索引
 *
 * 本函数扫描索引，查找按清理回调可删除的叶子元组，并更新
 * 统计信息。btbulkdelete 与 btvacuumcleanup 都会调用它（后者
 * 仅在未发生 btbulkdelete 调用时）。
 *
 * 在遍历全部索引页时，本函数还会记下任何空叶子页以及所有
 * 内部页。扫描完所有页之后，我们删除空页以便复用。任何已删除
 * 页会直接加入空闲空间映射。（它们本应在最初删除时就已加入，
 * 但例如崩溃后 FSM 丢失时仍可能需要再加一次。）
 *
 * 调用者负责初始分配/清零 stats 结构。
  *
  *
  *
 */
static void
gistvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	GistVacState vstate;
	BlockNumber num_pages;
	bool		needLock;
	BlockNumber blkno;
	MemoryContext oldctx;

	/*
	 * 现在重置跟踪整个索引信息的字段。这在单条 VACUUM
	 * 命令需要对索引进行多次扫描时，可避免重复计数。
	 *
	 * 此处避免重置 tuples_removed 与 pages_newly_deleted 字段，
	 * 因为它们跟踪的是 VACUUM 命令本身的信息，因此必须在
	 * 对 gistvacuumscan() 的每次调用之间保持。
	 *
	 * （注意 pages_free 被当作关于整个索引的状态，而不是
	 * 当前 VACUUM 的状态。这是合适的，因为 RecordFreeIndexPage()
	 * 调用是幂等的，并且在某些场景下会对同一已删除页重复
	 * 执行。对我们而言，要点是在 VACUUM 命令结束时跟踪索引中
	 * 可回收页的数量。）
	  *
	 */
	stats->num_pages = 0;
	stats->estimated_count = false;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/*
	 * 创建整数集合，以便在 page_set_context 中记住所有内部页
	 * 与空叶子页。在内部，整数集合会记住该上下文，使这些
	 * 整数集合的后续分配都从同一上下文进行。
	 *
	 * XXX 下面使用的分配大小早于 generation 上下文的块增长
	 * 代码。这些值或许应经基准测试后设为更合适的数值。
	  *
	  *
	 */
	vstate.page_set_context = GenerationContextCreate(CurrentMemoryContext,
													  "GiST VACUUM page set context",
													  16 * 1024,
													  16 * 1024,
													  16 * 1024);
	oldctx = MemoryContextSwitchTo(vstate.page_set_context);
	vstate.internal_page_set = intset_create();
	vstate.empty_leaf_set = intset_create();
	MemoryContextSwitchTo(oldctx);

	/* 设置要向下传给 gistvacuumpage 的信息 */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	if (RelationNeedsWAL(rel))
		vstate.startNSN = GetInsertRecPtr();
	else
		vstate.startNSN = gistGetFakeLSN(rel);

	/*
	 * 外层循环按物理顺序遍历所有索引页（我们希望内核配合
	 * 提供预读以提高速度）。关键的是必须访问所有叶子页，
	 * 包括在我们开始扫描之后新增的，否则可能无法删除某些
	 * 可删除元组。因此，必须反复检查关系长度。这样做时必须
	 * 获取关系扩展锁以避免竞态条件：若别人正在扩展关系，
	 * 存在一个窗口，此时 bufmgr/smgr 已创建全新的全零页，但
	 * gistNewBuffer() 尚未对其取写锁。若我们在此处扫到这样的
	 * 页，就会错误地假定它可以回收。取锁足以同步并防止问题：
	 * 要么 num_pages 不包含新页，要么 gistNewBuffer 已对缓冲区
	 * 持有写锁，并在我们检查之前完成完全初始化。（另见
	 * vacuumlazy.c，它有同样的问题。）此外，不必担心我们查看
	 * 之后立即新增的页；页分裂代码在添加右页之前已对左页持有
	 * 写锁，因此任何将要移入该页的元组必定已被我们处理过。
	 *
	 * 不过，对新关系或临时关系可以跳过加锁，因为不会有其他
	 * 人访问它们。
	  *
	  *
	  *
	  *
	  *
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	blkno = GIST_ROOT_BLKNO;
	for (;;)
	{
		/* 获取当前关系长度 */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		/* 若已扫描完整个关系则退出 */
		if (blkno >= num_pages)
			break;
		/* 遍历各页，然后回环以重新检查长度 */
		for (; blkno < num_pages; blkno++)
			gistvacuumpage(&vstate, blkno, blkno);
	}

	/*
	 * 若我们发现了任何可回收页（并已将它们记入 FSM），则
	 * 强制更新上层 FSM 页，以确保搜索者能找到它们。这些页
	 * 也可能在先前扫描中已找到，因而这可能是浪费时间，但相对
	 * 于扫描索引而言足够便宜，影响不大，而且让空闲页尽早而非
	 * 较晚可用似乎值得。
	 *
	 * 注意：若不存在可回收页，我们根本不费心清理 FSM。
	  *
	  *
	  *
	 */
	if (stats->pages_free > 0)
		IndexFreeSpaceMapVacuum(rel);

	/* 更新统计信息 */
	stats->num_pages = num_pages;

	/*
	 * 若看到了任何空页，尝试把它们从树中解除链接，以便
	 * 复用。
	 */
	gistvacuum_delete_empty_pages(info, &vstate);

	/* 不再需要内部页与空页集合 */
	MemoryContextDelete(vstate.page_set_context);
	vstate.page_set_context = NULL;
	vstate.internal_page_set = NULL;
	vstate.empty_leaf_set = NULL;
}

/*
 * gistvacuumpage --- VACUUM 一页
 *
 * 本函数为 gistbulkdelete() 处理单个页。在某些情况下我们必须
 * 回退并重新检查先前已扫描的页；必要时本例程会递归以处理
 * 这种情况。
 *
 * blkno 是要处理的页。orig_blkno 是外层 gistvacuumscan 循环
 * 已到达的最高块号（与 blkno 相同，除非我们正在递归以重新检查
 * 先前的页）。
 */
static void
gistvacuumpage(GistVacState *vstate, BlockNumber blkno, BlockNumber orig_blkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	Buffer		buffer;
	Page		page;
	BlockNumber recurse_to;

restart:
	recurse_to = InvalidBlockNumber;

	/* 在未持有任何缓冲区锁时调用 vacuum_delay_point */
	vacuum_delay_point();

	buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								info->strategy);

	/*
	 * 我们不会在此停留很久，积极地取排他锁。
	  *
	 */
	LockBuffer(buffer, GIST_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	if (gistPageRecyclable(page))
	{
		/* 可以回收此页 */
		RecordFreeIndexPage(rel, blkno);
		vstate->stats->pages_deleted++;
		vstate->stats->pages_free++;
	}
	else if (GistPageIsDeleted(page))
	{
		/* 已删除，但尚不能回收 */
		vstate->stats->pages_deleted++;
	}
	else if (GistPageIsLeaf(page))
	{
		OffsetNumber todelete[MaxOffsetNumber];
		int			ntodelete = 0;
		int			nremain;
		GISTPageOpaque opaque = GistPageGetOpaque(page);
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

		/*
		 * 检查是否需要递归回到更早的页。我们关心的是自清理
		 * 扫描开始以来发生的页分裂。若分裂把某些元组移到了
		 * 编号更低的页，则我们可能漏掉了它们。若是如此，为
		 * 尾递归做好准备。
		 *
		 * 这类似于搜索时沿下行指针前进所做的检查，但我们不需要
		 * 跳到编号更高的页，因为反正稍后会处理它们。
		  *
		 */
		if ((GistFollowRight(page) ||
			 vstate->startNSN < GistPageGetNSN(page)) &&
			(opaque->rightlink != InvalidBlockNumber) &&
			(opaque->rightlink < orig_blkno))
		{
			recurse_to = opaque->rightlink;
		}

		/*
		 * 扫描所有项，查看按回调函数哪些需要删除。
		  *
		 */
		if (callback)
		{
			OffsetNumber off;

			for (off = FirstOffsetNumber;
				 off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				ItemId		iid = PageGetItemId(page, off);
				IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

				if (callback(&(idxtuple->t_tid), callback_state))
					todelete[ntodelete++] = off;
			}
		}

		/*
		 * 应用任何需要的删除。我们每页只发一条 WAL 记录，
		 * 以尽量减少 WAL 流量。
		 */
		if (ntodelete > 0)
		{
			START_CRIT_SECTION();

			MarkBufferDirty(buffer);

			PageIndexMultiDelete(page, todelete, ntodelete);
			GistMarkTuplesDeleted(page);

			if (RelationNeedsWAL(rel))
			{
				XLogRecPtr	recptr;

				recptr = gistXLogUpdate(buffer,
										todelete, ntodelete,
										NULL, 0, InvalidBuffer);
				PageSetLSN(page, recptr);
			}
			else
				PageSetLSN(page, gistGetFakeLSN(rel));

			END_CRIT_SECTION();

			vstate->stats->tuples_removed += ntodelete;
			/* 必须重新计算 maxoff */
			maxoff = PageGetMaxOffsetNumber(page);
		}

		nremain = maxoff - FirstOffsetNumber + 1;
		if (nremain == 0)
		{
			/*
			 * 该页现在完全为空。记住其块号，以便我们在第二阶段
			 * 尝试删除该页。
			 *
			 * 递归时跳过，因为 IntegerSet 需要按升序添加值。
			 * 下一次 VACUUM 会拾取它。
			  *
			 */
			if (blkno == orig_blkno)
				intset_add_member(vstate->empty_leaf_set, blkno);
		}
		else
			vstate->stats->num_index_tuples += nremain;
	}
	else
	{
		/*
		 * 在内部页上，检查"无效元组"，它们是 PostgreSQL 9.0
		 * 或更早版本上未完成的页分裂留下的。较新的 PostgreSQL
		 * 版本不会再创建它们，但不幸的是 GiST 索引中没有任何版本
		 * 号，因此我们不知道该索引是否仍可能包含无效元组。
		  *
		 */
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;

		for (off = FirstOffsetNumber;
			 off <= maxoff;
			 off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(page, off);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

			if (GistTupleIsInvalid(idxtuple))
				ereport(LOG,
						(errmsg("index \"%s\" contains an inner tuple marked as invalid",
								RelationGetRelationName(rel)),
						 errdetail("This is caused by an incomplete page split at crash recovery before upgrading to PostgreSQL 9.1."),
						 errhint("Please REINDEX it.")));
		}

		/*
		 * 记住此页的块号，以便稍后在 gistvacuum_delete_empty_pages()
		 * 中查找空叶子页的父页时可以再次访问它。
		  *
		 */
		if (blkno == orig_blkno)
			intset_add_member(vstate->internal_page_set, blkno);
	}

	UnlockReleaseBuffer(buffer);

	/*
	 * 这其实是尾递归，但若编译器笨到无法将其优化为循环，我们
	 * 会在每一层递归上消耗令人不适的大量栈空间（由于 deletable[]
	 * 数组）。失败不太可能，因为层数通常不会很大……但以防万一，
	 * 我们手工优化成循环。
	  *
	 */
	if (recurse_to != InvalidBlockNumber)
	{
		blkno = recurse_to;
		goto restart;
	}
}

/*
 * 扫描所有内部页，并尝试删除它们的空子页。
 */
static void
gistvacuum_delete_empty_pages(IndexVacuumInfo *info, GistVacState *vstate)
{
	Relation	rel = info->index;
	BlockNumber empty_pages_remaining;
	uint64		blkno;

	/*
	 * 重新扫描所有内部页，找出那些有空子页的。
	 */
	empty_pages_remaining = intset_num_entries(vstate->empty_leaf_set);
	intset_begin_iterate(vstate->internal_page_set);
	while (empty_pages_remaining > 0 &&
		   intset_iterate_next(vstate->internal_page_set, &blkno))
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber off,
					maxoff;
		OffsetNumber todelete[MaxOffsetNumber];
		BlockNumber leafs_to_delete[MaxOffsetNumber];
		int			ntodelete;
		int			deleted;

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, (BlockNumber) blkno,
									RBM_NORMAL, info->strategy);

		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || GistPageIsDeleted(page) || GistPageIsLeaf(page))
		{
			/*
			 * 此页先前是内部页，但现在变成了别的东西。
			 * 不应发生……
			 */
			Assert(false);
			UnlockReleaseBuffer(buffer);
			continue;
		}

		/*
		 * 扫描所有下行指针，看是否有任何指向空叶子页。
		  *
		 */
		maxoff = PageGetMaxOffsetNumber(page);
		ntodelete = 0;
		for (off = FirstOffsetNumber;
			 off <= maxoff && ntodelete < maxoff - 1;
			 off = OffsetNumberNext(off))
		{
			ItemId		iid = PageGetItemId(page, off);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			BlockNumber leafblk;

			leafblk = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			if (intset_is_member(vstate->empty_leaf_set, leafblk))
			{
				leafs_to_delete[ntodelete] = leafblk;
				todelete[ntodelete++] = off;
			}
		}

		/*
		 * 为避免死锁，子页必须在父页之前加锁，因此我们必须释放
		 * 父页上的锁，锁住子页，然后再重新获取父页的锁。（而且
		 * 反正也不想在持锁时做 I/O。）
		 *
		 * 在我们未持有父页锁的瞬间，下行指针可能被并发插入移走，
		 * 因此在取得两把锁之后，必须重新检查它是否仍指向同一子
		 * 页。另外，另一个后端可能已向该页插入了元组，使它不再
		 * 为空。gistdeletepage() 会重新检查所有这些条件。
		  *
		  *
		  *
		 */
		LockBuffer(buffer, GIST_UNLOCK);

		deleted = 0;
		for (int i = 0; i < ntodelete; i++)
		{
			Buffer		leafbuf;

			/*
			 * 不要从父页移除最后一个下行指针。那会
			 * 迷惑插入代码。
			 */
			if (PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
				break;

			leafbuf = ReadBufferExtended(rel, MAIN_FORKNUM, leafs_to_delete[i],
										 RBM_NORMAL, info->strategy);
			LockBuffer(leafbuf, GIST_EXCLUSIVE);
			gistcheckpage(rel, leafbuf);

			LockBuffer(buffer, GIST_EXCLUSIVE);
			if (gistdeletepage(info, vstate->stats,
							   buffer, todelete[i] - deleted,
							   leafbuf))
				deleted++;
			LockBuffer(buffer, GIST_UNLOCK);

			UnlockReleaseBuffer(leafbuf);
		}

		ReleaseBuffer(buffer);

		/*
		 * 一旦我们已看到这些下行指针，就可以停止扫描，即使
		 * 未能将它们全部移除。
		 */
		empty_pages_remaining -= ntodelete;
	}
}

/*
 * gistdeletepage 接受一个叶子页及其父页，并尝试删除该
 * 叶子。两页都必须已加锁。
 *
 * 即便我们初次看到该页时它为空，并发插入者此后也可能已向
 * 其添加了元组。同样，下行指针也可能已移动。我们重新检查
 * 全部条件，以确保在修改任何内容之前该页仍可删除。
 *
 * 若页被删除则返回 true；若并发更新阻止了删除则返回
 * false。
  *
 */
static bool
gistdeletepage(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   Buffer parentBuffer, OffsetNumber downlink,
			   Buffer leafBuffer)
{
	Page		parentPage = BufferGetPage(parentBuffer);
	Page		leafPage = BufferGetPage(leafBuffer);
	ItemId		iid;
	IndexTuple	idxtuple;
	XLogRecPtr	recptr;
	FullTransactionId txid;

	/*
	 * 检查叶子是否仍为空且可删除。
	 */
	if (!GistPageIsLeaf(leafPage))
	{
		/* 叶子页绝不应当变成非叶子页 */
		Assert(false);
		return false;
	}

	if (GistFollowRight(leafPage))
		return false;			/* 不要搅扰并发页分裂 */

	if (PageGetMaxOffsetNumber(leafPage) != InvalidOffsetNumber)
		return false;			/* 已不再为空 */

	/*
	 * 好，叶子可删除。父页中的下行指针是否仍然有效？它可能
	 * 已被并发插入移走。我们可以尝试再次扫描该页来重新找到它，
	 * 若发生了分裂还可能向右移动。但目前先保持简单，直接放弃。
	 * 下一次 VACUUM 会拾取它。
	  *
	 */
	if (PageIsNew(parentPage) || GistPageIsDeleted(parentPage) ||
		GistPageIsLeaf(parentPage))
	{
		/* 不应发生，内部页从不会被删除 */
		Assert(false);
		return false;
	}

	if (PageGetMaxOffsetNumber(parentPage) < downlink
		|| PageGetMaxOffsetNumber(parentPage) <= FirstOffsetNumber)
		return false;

	iid = PageGetItemId(parentPage, downlink);
	idxtuple = (IndexTuple) PageGetItem(parentPage, iid);
	if (BufferGetBlockNumber(leafBuffer) !=
		ItemPointerGetBlockNumber(&(idxtuple->t_tid)))
		return false;

	/*
	 * 一切就绪，继续删除。
	 *
	 * 该页无法立即回收，因为看到过下行指针的进行中扫描仍可能
	 * 访问它。用当前的 next-XID 计数器标记该页，以便我们知道何时
	 * 可以回收。一旦该 XID 比 GlobalXmin 更旧，我们就知道当前所有
	 * 进行中的扫描必定已经结束。（这比实际需要保守得多，但让我们
	 * 保持安全与简单。）
	  *
	 */
	txid = ReadNextFullTransactionId();

	START_CRIT_SECTION();

	/* 将该页标记为已删除 */
	MarkBufferDirty(leafBuffer);
	GistPageSetDeleted(leafPage, txid);
	stats->pages_newly_deleted++;
	stats->pages_deleted++;

	/* 从父页移除下行指针 */
	MarkBufferDirty(parentBuffer);
	PageIndexTupleDelete(parentPage, downlink);

	if (RelationNeedsWAL(info->index))
		recptr = gistXLogPageDelete(leafBuffer, txid, parentBuffer, downlink);
	else
		recptr = gistGetFakeLSN(info->index);
	PageSetLSN(parentPage, recptr);
	PageSetLSN(leafPage, recptr);

	END_CRIT_SECTION();

	return true;
}
