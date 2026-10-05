/*-------------------------------------------------------------------------
 *
 * nbtdedup.c
 *	  在 Postgres B-树中对项去重或做自底向上删除。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtdedup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"

static void _bt_bottomupdel_finish_pending(Page page, BTDedupState state,
										   TM_IndexDeleteOp *delstate);
static bool _bt_do_singleval(Relation rel, Page page, BTDedupState state,
							 OffsetNumber minoff, IndexTuple newitem);
static void _bt_singleval_fillfactor(Page page, BTDedupState state,
									 Size newitemsz);
#ifdef USE_ASSERT_CHECKING
static bool _bt_posting_valid(IndexTuple posting);
#endif

/*
 * 执行一次去重趟。
 *
 * 这里的总体做法是尽可能多地去重，
 * 以尽可能多地腾出空间。不过请注意，"单值"
 * 策略用于非 bottomupdedup 调用者，当页上全是
 * 同一值的元组时。应用该策略的去重趟
 * 会在页尾留下若干未触碰的元组，为
 * 预计采用 nbtsplitloc.c 自身单值
 * 策略的页分裂做准备。我们的高层目标是推迟合并这些未触碰的
 * 元组，直到页分裂之后。
 *
 * 当刚刚调用过 _bt_bottomupdel_pass()（并且失败）时，我们的
 * 高层目标是争取更多时间，从而完全防止页分裂。
 * 我们仍然希望页分裂能被完全避免。因此
 * 对 bottomupdedup 调用者甚至不考虑单值策略。
 *
 * 若无法成功腾出至少
 * newitemsz 的空间，该页将不得不分裂（还需要 newitem 的行指针空间，这部分并未
 * 计入调用者的 newitemsz）。
 *
 * 注意：调用者应当已经删除所有已设置
 * LP_DEAD 位的现有项。
 */
void
_bt_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem, Size newitemsz,
			   bool bottomupdedup)
{
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	Page		newpage;
	BTDedupState state;
	Size		pagesaving PG_USED_FOR_ASSERTS_ONLY = 0;
	bool		singlevalstrat = false;
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	/* 传入的 newitemsz 已 MAXALIGNED，但不包含行指针 */
	newitemsz += sizeof(ItemIdData);

	/*
	 * 初始化去重状态。
	 *
	 * maxpostingsize（倒排列表元组大小的上限）本来可以
	 * 设为页的三分之一。不过，把倒排列表的大小限制为
	 * 页的六分之一似乎是个好主意。
	 * 这样，当充满重复的页可以分裂好几次时，
	 * 我们应当仍能得到一个好的分裂点。
	 */
	state = (BTDedupState) palloc(sizeof(BTDedupStateData));
	state->deduplicate = true;
	state->nmaxitems = 0;
	state->maxpostingsize = Min(BTMaxItemSize(page) / 2, INDEX_SIZE_MASK);
	/* 当前待定倒排列表的基元组元数据 */
	state->base = NULL;
	state->baseoff = InvalidOffsetNumber;
	state->basetupsize = 0;
	/* 当前待定倒排列表 TIDs 的元数据 */
	state->htids = palloc(state->maxpostingsize);
	state->nhtids = 0;
	state->nitems = 0;
	/* 将被待定倒排列表替换的全部物理元组大小 */
	state->phystupsize = 0;
	/* nintervals 应初始化为零 */
	state->nintervals = 0;

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * 考虑应用"单值"策略，不过仅当该页
	 * 看起来很可能在不久的将来分裂时
	 */
	if (!bottomupdedup)
		singlevalstrat = _bt_do_singleval(rel, page, state, minoff, newitem);

	/*
	 * 对页上的项去重，并写入 newpage。
	 *
	 * 把原页的 LSN 复制到 newpage 副本中。这将成为
	 * 该页的更新版本。需要这样做是因为 XLogInsert 会
	 * 检查 LSN，并可能把它转储进页映像。
	 */
	newpage = PageGetTempPageCopySpecial(page);
	PageSetLSN(newpage, PageGetLSN(page));

	/* 若有高键则复制 */
	if (!P_RIGHTMOST(opaque))
	{
		ItemId		hitemid = PageGetItemId(page, P_HIKEY);
		Size		hitemsz = ItemIdGetLength(hitemid);
		IndexTuple	hitem = (IndexTuple) PageGetItem(page, hitemid);

		if (PageAddItem(newpage, (Item) hitem, hitemsz, P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add highkey");
	}

	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(!ItemIdIsDead(itemid));

		if (offnum == minoff)
		{
			/*
			 * 该数据项没有先前的/基元组 -- 把该数据项
			 * 用作待定倒排列表的基元组
			 */
			_bt_dedup_start_pending(state, itup, offnum);
		}
		else if (state->deduplicate &&
				 _bt_keep_natts_fast(rel, state->base, itup) > nkeyatts &&
				 _bt_dedup_save_htid(state, itup))
		{
			/*
			 * 元组等于待定倒排列表的基元组。itup 的堆
			 * TID(s) 已保存在 state 中。
			 */
		}
		else
		{
			/*
			 * 元组不等于待定倒排列表元组，或者
			 * _bt_dedup_save_htid() 因其他原因选择不把当前项合并进
			 * 待定倒排列表（例如，再加入更多
			 * TIDs 会使倒排列表超过当前的
			 * maxpostingsize）。
			 *
			 * 若 state 中的待定倒排列表包含多于一项，
			 * 则构造新的倒排元组并加入临时页（newpage）。
			 * 否则把待定区间的基元组原样加入临时页。
			 */
			pagesaving += _bt_dedup_finish_pending(newpage, state);

			if (singlevalstrat)
			{
				/*
				 * 单值策略的额外步骤。
				 *
				 * 在已形成或观察到 5 个受 maxpostingsize 限制的元组时，
				 * 降低第六个、也是最后一个大型倒排列表
				 * 元组的 maxpostingsize。
				 *
				 * 当第六个受 maxpostingsize 限制的项被形成或观察到时，
				 * 就完全停止把元组合并到一起。页尾剩下的少数元组
				 * 将完全不会被合并在一起
				 * （至少要等到将来某次页分裂
				 * 发生之后，当本页新分配的右兄弟页
				 * 进行它的第一次去重趟时）。
				 */
				if (state->nmaxitems == 5)
					_bt_singleval_fillfactor(page, state, newitemsz);
				else if (state->nmaxitems == 6)
				{
					state->deduplicate = false;
					singlevalstrat = false; /* 不会再回到这里 */
				}
			}

			/* itup 开始新的待定倒排列表 */
			_bt_dedup_start_pending(state, itup, offnum);
		}
	}

	/* 处理最后一项 */
	pagesaving += _bt_dedup_finish_pending(newpage, state);

	/*
	 * 若没有找到适合去重的项，newpage 必须与
	 * 原页完全相同，因此直接从函数返回。
	 *
	 * 我们本可以根据节省的空间是否足以避免立即的页分裂，
	 * 来决定是否继续。我们
	 * 不这么做，是因为让 nbtsplitloc.c 始终针对
	 * 已完全去重的页工作有一点价值（除了
	 * newitem 之外）。况且大部分代价已经付出。
	 */
	if (state->nintervals == 0)
	{
		/* 这里无法泄漏内存 */
		pfree(newpage);
		pfree(state->htids);
		pfree(state);
		return;
	}

	/*
	 * 到这里，去重显然一定会进行。
	 *
	 * 清除 BTP_HAS_GARBAGE 页标志。该索引必须是 heapkeyspace
	 * 索引，因此我们反正永远不会关注 BTP_HAS_GARBAGE。
	 * 但还是保持整洁。
	 */
	if (P_HAS_GARBAGE(opaque))
	{
		BTPageOpaque nopaque = BTPageGetOpaque(newpage);

		nopaque->btpo_flags &= ~BTP_HAS_GARBAGE;
	}

	START_CRIT_SECTION();

	PageRestoreTempPage(newpage, page);
	MarkBufferDirty(buf);

	/* XLOG 相关处理 */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_btree_dedup xlrec_dedup;

		xlrec_dedup.nintervals = state->nintervals;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_dedup, SizeOfBtreeDedup);

		/*
		 * 区间数组不在缓冲区中，但假装它在。
		 * 当 XLogInsert 存储整个缓冲区时，该数组就不必
		 * 再存储一遍。
		 */
		XLogRegisterBufData(0, (char *) state->intervals,
							state->nintervals * sizeof(BTDedupInterval));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DEDUP);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* 本地空间记账应与页记账一致 */
	Assert(pagesaving < newitemsz || PageGetExactFreeSpace(page) >= newitemsz);

	/* 这里无法泄漏内存 */
	pfree(state->htids);
	pfree(state);
}

/*
 * 执行自底向上索引删除趟。
 *
 * 查看重复的索引元组（以及某些邻近元组）是否有资格
 * 经由自底向上索引删除被删掉。这里的高层目标是
 * 完全防止由 MVCC 版本搅动引起的"不必要"页分裂，
 * 这些搅动来自 UPDATEs（当这些 UPDATEs 在逻辑上并未修改
 * 'rel' 索引所覆盖的任何列时）。这是定性的，不是定量的 -- 我们
 * 并不特别在意一次性删掉许多索引
 * 元组的机会。
 *
 * 关于 nbtree 自底向上删除的设计细节，见 nbtree/README。
 * 关于我们应如何与 tableam 配合，见 access/tableam.h
 * 中的说明。
 *
 * 成功时返回 true，此时调用者可以假定页分裂会在
 * 相当长一段时间内被避免。当调用者应当
 * 对该页去重时返回 false（若根本有可能的话）。
 *
 * 注意：有时尽管未能删掉足够多的项来
 * 避免分裂，我们仍返回 true。这会使调用者跳过去重并立刻分裂该页。
 * 我们的返回值始终只是建议性信息。
 *
 * 注意：调用者应当已经删除所有已设置
 * LP_DEAD 位的现有项。
 */
bool
_bt_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
					 Size newitemsz)
{
	OffsetNumber offnum,
				minoff,
				maxoff;
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	BTDedupState state;
	TM_IndexDeleteOp delstate;
	bool		neverdedup;
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	/* 传入的 newitemsz 已 MAXALIGNED，但不包含行指针 */
	newitemsz += sizeof(ItemIdData);

	/* 初始化去重状态 */
	state = (BTDedupState) palloc(sizeof(BTDedupStateData));
	state->deduplicate = true;
	state->nmaxitems = 0;
	state->maxpostingsize = BLCKSZ; /* 我们其实并不是在去重 */
	state->base = NULL;
	state->baseoff = InvalidOffsetNumber;
	state->basetupsize = 0;
	state->htids = palloc(state->maxpostingsize);
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;
	state->nintervals = 0;

	/*
	 * 初始化描述自底向上索引删除
	 * 操作的 tableam 状态。
	 *
	 * 接下来我们会请 tableam 搜索那些索引元组可以
	 * 安全删除的 TIDs。tableam 会一直搜索，直到叶子页空间
	 * 目标得到满足，或直到继续进行 tableam
	 * 操作的代价看起来过高。它把精力集中在与我们标为"有希望"的
	 * 重复索引元组相关联的 TIDs 上。
	 *
	 * 这个空间目标有些任意。tableam 必须能够
	 * 使代价与收益保持平衡。我们向 tableam 提供
	 * 关于什么可能奏效的详尽信息，而不直接
	 * 关心在 tableam 调用期间避免工作。我们
	 * 为自底向上删除过程计价的角色严格是建议性的。
	 */
	delstate.irel = rel;
	delstate.iblknum = BufferGetBlockNumber(buf);
	delstate.bottomup = true;
	delstate.bottomupfreespace = Max(BLCKSZ / 16, newitemsz);
	delstate.ndeltids = 0;
	delstate.deltids = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexDelete));
	delstate.status = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexStatus));

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(!ItemIdIsDead(itemid));

		if (offnum == minoff)
		{
			/* itup 开始第一个待定区间 */
			_bt_dedup_start_pending(state, itup, offnum);
		}
		else if (_bt_keep_natts_fast(rel, state->base, itup) > nkeyatts &&
				 _bt_dedup_save_htid(state, itup))
		{
			/* 元组相等；刚把它的 TIDs 加入待定区间 */
		}
		else
		{
			/* 完成区间 -- 把它的 TIDs 移到删除状态 */
			_bt_bottomupdel_finish_pending(page, state, &delstate);

			/* itup 开始新的待定区间 */
			_bt_dedup_start_pending(state, itup, offnum);
		}
	}
	/* 完成最后一个区间 -- 把它的 TIDs 移到删除状态 */
	_bt_bottomupdel_finish_pending(page, state, &delstate);

	/*
	 * 即使只有很少（甚至为零）个
	 * 可供 tableam 使用的有希望元组，我们现在也不放弃，因为作为索引
	 * AM，管理代价并不是我们的事（注意 tableam 可能自有
	 * 启发式来判断该怎么做）。不过，若有希望元组为零，
	 * 我们至少应当避免让调用者在我们返回之后
	 * 再做一次无用的去重趟。
	 */
	neverdedup = false;
	if (state->nintervals == 0)
		neverdedup = true;

	pfree(state->htids);
	pfree(state);

	/* 询问 tableam 哪些 TIDs 可删除，然后物理删除它们 */
	_bt_delitems_delete_check(rel, buf, heapRel, &delstate);

	pfree(delstate.deltids);
	pfree(delstate.status);

	/* 无条件向调用者报告"成功"，以避免去重 */
	if (neverdedup)
		return true;

	/* 若反正短期内不会再回到这里，就不要去重 */
	return PageGetExactFreeSpace(page) >= Max(BLCKSZ / 24, newitemsz);
}

/*
 * 根据调用者的基元组创建一个新的待定倒排列表元组。
 *
 * 去重所处理的每个元组，要么成为某个
 * 倒排列表的基元组，要么使其堆 TID(s) 被接受到待定倒排列表中。
 * 一开始作为倒排列表基元组的元组，只有当
 * 确实存在可以并入该基元组的重复时，才会在 _bt_dedup_finish_pending() 内
 * 真正被重写。
 */
void
_bt_dedup_start_pending(BTDedupState state, IndexTuple base,
						OffsetNumber baseoff)
{
	Assert(state->nhtids == 0);
	Assert(state->nitems == 0);
	Assert(!BTreeTupleIsPivot(base));

	/*
	 * 把新基元组的堆 TID(s) 复制到新候选倒排列表所用的
	 * 工作状态数组中
	 */
	if (!BTreeTupleIsPosting(base))
	{
		memcpy(state->htids, &base->t_tid, sizeof(ItemPointerData));
		state->nhtids = 1;
		state->basetupsize = IndexTupleSize(base);
	}
	else
	{
		int			nposting;

		nposting = BTreeTupleGetNPosting(base);
		memcpy(state->htids, BTreeTupleGetPosting(base),
			   sizeof(ItemPointerData) * nposting);
		state->nhtids = nposting;
		/* basetupsize 不应包含已有倒排列表 */
		state->basetupsize = BTreeTupleGetPostingOffset(base);
	}

	/*
	 * 保存新的基元组本身 -- 若我们真的要从新的待定倒排列表
	 * 创建出新的倒排列表，就会用到它。
	 *
	 * 必须维护所有现有元组的物理大小（包括行
	 * 指针开销），以便计算页上节省的空间。
	 */
	state->nitems = 1;
	state->base = base;
	state->baseoff = baseoff;
	state->phystupsize = MAXALIGN(IndexTupleSize(base)) + sizeof(ItemIdData);
	/* 同时把 baseoff 存入区间的待定状态 */
	state->intervals[state->nintervals].baseoff = state->baseoff;
}

/*
 * 在可能的情况下，把 itup 的堆 TID(s) 保存到待定倒排列表中。
 *
 * 返回布尔值，表明 state 所管理的待定倒排列表现在是否
 * 包含 itup 的堆 TID(s)。
 */
bool
_bt_dedup_save_htid(BTDedupState state, IndexTuple itup)
{
	int			nhtids;
	ItemPointer htids;
	Size		mergedtupsz;

	Assert(!BTreeTupleIsPivot(itup));

	if (!BTreeTupleIsPosting(itup))
	{
		nhtids = 1;
		htids = &itup->t_tid;
	}
	else
	{
		nhtids = BTreeTupleGetNPosting(itup);
		htids = BTreeTupleGetPosting(itup);
	}

	/*
	 * 若追加 itup 的堆 TID(s) 会使我们超过 maxpostingsize 上限，
	 * 就不要追加（让调用者按原样完成待定倒排列表）。
	 *
	 * 此计算需要与 _bt_form_posting() 内部用于
	 * 新倒排列表元组的代码一致。
	 */
	mergedtupsz = MAXALIGN(state->basetupsize +
						   (state->nhtids + nhtids) * sizeof(ItemPointerData));

	if (mergedtupsz > state->maxpostingsize)
	{
		/*
		 * 把此项计为单值策略的超大项，不过
		 * 仅当最终倒排列表元组中有 50 个 TIDs 时才这样。此
		 * 上限（相当任意）是为了避免搞混：当前
		 * 去重趟已经遇到或创建了多少个
		 * 占页 1/6 的元组。
		 *
		 * 注意：我们有意不考虑是哪一次去重趟
		 * 把元组合并起来创建了此项（可能是上一次
		 * 去重趟，也可能是当前这一趟）。见 _bt_do_singleval()
		 * 的注释。
		 */
		if (state->nhtids > 50)
			state->nmaxitems++;

		return false;
	}

	/*
	 * 把堆 TIDs 保存到待定倒排列表元组中 -- itup 可以并入
	 * 待定倒排列表
	 */
	state->nitems++;
	memcpy(state->htids + state->nhtids, htids,
		   sizeof(ItemPointerData) * nhtids);
	state->nhtids += nhtids;
	state->phystupsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);

	return true;
}

/*
 * 完成待定倒排列表元组，并把它加入页中。最终元组
 * 基于已保存的基元组，以及已保存的堆 TIDs 列表。
 *
 * 返回通过去重构造新倒排列表元组所节省的空间。
 * 注意这包含行指针开销。若不可能去重，
 * 则该值为零。
 */
Size
_bt_dedup_finish_pending(Page newpage, BTDedupState state)
{
	OffsetNumber tupoff;
	Size		tuplesz;
	Size		spacesaving;

	Assert(state->nitems > 0);
	Assert(state->nitems <= state->nhtids);
	Assert(state->intervals[state->nintervals].baseoff == state->baseoff);

	tupoff = OffsetNumberNext(PageGetMaxOffsetNumber(newpage));
	if (state->nitems == 1)
	{
		/* 使用原来未改动的基元组 */
		tuplesz = IndexTupleSize(state->base);
		Assert(tuplesz == MAXALIGN(IndexTupleSize(state->base)));
		Assert(tuplesz <= BTMaxItemSize(newpage));
		if (PageAddItem(newpage, (Item) state->base, tuplesz, tupoff,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add tuple to page");

		spacesaving = 0;
	}
	else
	{
		IndexTuple	final;

		/* 构造带倒排列表的元组 */
		final = _bt_form_posting(state->base, state->htids, state->nhtids);
		tuplesz = IndexTupleSize(final);
		Assert(tuplesz <= state->maxpostingsize);

		/* 保存倒排列表的最终项数 */
		state->intervals[state->nintervals].nitems = state->nitems;

		Assert(tuplesz == MAXALIGN(IndexTupleSize(final)));
		Assert(tuplesz <= BTMaxItemSize(newpage));
		if (PageAddItem(newpage, (Item) final, tuplesz, tupoff, false,
						false) == InvalidOffsetNumber)
			elog(ERROR, "deduplication failed to add tuple to page");

		pfree(final);
		spacesaving = state->phystupsize - (tuplesz + sizeof(ItemIdData));
		/* 递增 nintervals，因为我们写入了新的倒排列表元组 */
		state->nintervals++;
		Assert(spacesaving > 0 && spacesaving < BLCKSZ);
	}

	/* 为下一个待定倒排列表重置状态 */
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;

	return spacesaving;
}

/*
 * 在自底向上索引删除期间完成区间。
 *
 * 在一次自底向上趟中，我们预期 TIDs 会先记入去重状态里，
 * 然后再以大小可变的批次移交给 delstate，办法就是
 * 调用这里。当某个去重区间中的 TID 数量已经知道、
 * 并且该区间被完成时就会调用这里（即调用者看到下一个
 * 页上元组不是重复，或者调用者已经没有叶子页上的元组
 * 可处理时）。
 *
 * 自底向上删除在这里判定并记住哪些项是
 * 重复。这对稍后的 tableam 删除
 * 基础设施是重要信息。普通索引元组的重复在这里被标为
 * "有希望"，依照 tableam 的约定。
 *
 * 对 TIDs 来自倒排列表的那些项，我们的标记办法更为
 * 复杂。倒排列表只能由去重趟形成（或者
 * 在索引构建期间），所以被指向的逻辑行近期发生版本搅动
 * 并不特别可能。我们仍可能就倒排列表元组的各项给出弱信号
 * （只把它的某一个 TID/项标成
 * 有希望），不过只有当覆盖该倒排列表元组的最终区间里还有
 * 更多重复索引元组时才有这种可能（与
 * 普通元组情形相同）。当 tableam 对整个删除操作
 * 没有更强的信号可以配合时，弱信号/提示
 * 对它会有用。
 *
 * 我们所用的启发式在实践中效果很好，因为我们只需要给
 * tableam 关于该往哪里看的正确 _general_ 概念。垃圾往往会
 * 自然地集中在相对较少的表块中，在自底向上删除
 * 所针对的负载下尤其如此。tableam 无法依据我们提供的提示，
 * 合理地给全部可用表块排序，但这没关系 --
 * 只有极端情形才要紧。tableam 只需要能够
 * 预测每一次删除操作中，哪少数几个表块会拥有最多
 * 可以安全删除的元组，并且在相关的
 * 各次删除操作之间保持很低的方差。
 */
static void
_bt_bottomupdel_finish_pending(Page page, BTDedupState state,
							   TM_IndexDeleteOp *delstate)
{
	bool		dupinterval = (state->nitems > 1);

	Assert(state->nitems > 0);
	Assert(state->nitems <= state->nhtids);
	Assert(state->intervals[state->nintervals].baseoff == state->baseoff);

	for (int i = 0; i < state->nitems; i++)
	{
		OffsetNumber offnum = state->baseoff + i;
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		TM_IndexDelete *ideltid = &delstate->deltids[delstate->ndeltids];
		TM_IndexStatus *istatus = &delstate->status[delstate->ndeltids];

		if (!BTreeTupleIsPosting(itup))
		{
			/* 简单情形：普通非枢轴元组 */
			ideltid->tid = itup->t_tid;
			ideltid->id = delstate->ndeltids;
			istatus->idxoffnum = offnum;
			istatus->knowndeletable = false;	/* 目前 */
			istatus->promising = dupinterval;	/* 简单规则 */
			istatus->freespace = ItemIdGetLength(itemid) + sizeof(ItemIdData);

			delstate->ndeltids++;
		}
		else
		{
			/*
			 * 复杂情形：倒排列表元组。
			 *
			 * 我们采取保守假定：每个倒排列表元组至多只会
			 * 涉及一个受影响的逻辑行。在 deltids 里至多有一个有希望的项来代表
 * 这个被假定为唯一的逻辑行。注意：除非
 * 该倒排列表元组同时也处于某个重复项
 * 区间之中，否则这一点甚至不会被考虑 -- 这条复杂规则不过是
			 * 简单规则的一个变体，而简单规则用来决定普通索引元组
			 * 是否有希望。
			 */
			int			nitem = BTreeTupleGetNPosting(itup);
			bool		firstpromising = false;
			bool		lastpromising = false;

			Assert(_bt_posting_valid(itup));

			if (dupinterval)
			{
				/*
				 * 复杂规则：倒排列表中的第一个或最后一个 TID
				 * 会被标为有希望（若终究有任何一个的话）
				 */
				BlockNumber minblocklist,
							midblocklist,
							maxblocklist;
				ItemPointer mintid,
							midtid,
							maxtid;

				mintid = BTreeTupleGetHeapTID(itup);
				midtid = BTreeTupleGetPostingN(itup, nitem / 2);
				maxtid = BTreeTupleGetMaxHeapTID(itup);
				minblocklist = ItemPointerGetBlockNumber(mintid);
				midblocklist = ItemPointerGetBlockNumber(midtid);
				maxblocklist = ItemPointerGetBlockNumber(maxtid);

				/* 只有主导表块上的那一项可以是有希望的 */
				firstpromising = (minblocklist == midblocklist);
				lastpromising = (!firstpromising &&
								 midblocklist == maxblocklist);
			}

			for (int p = 0; p < nitem; p++)
			{
				ItemPointer htid = BTreeTupleGetPostingN(itup, p);

				ideltid->tid = *htid;
				ideltid->id = delstate->ndeltids;
				istatus->idxoffnum = offnum;
				istatus->knowndeletable = false;	/* 目前 */
				istatus->promising = false;
				if ((firstpromising && p == 0) ||
					(lastpromising && p == nitem - 1))
					istatus->promising = true;
				istatus->freespace = sizeof(ItemPointerData);	/* 最坏情况 */

				ideltid++;
				istatus++;
				delstate->ndeltids++;
			}
		}
	}

	if (dupinterval)
	{
		state->intervals[state->nintervals].nitems = state->nitems;
		state->nintervals++;
	}

	/* 为下一个区间重置状态 */
	state->nhtids = 0;
	state->nitems = 0;
	state->phystupsize = 0;
}

/*
 * 判断页上的非枢轴元组（数据项）是否全是同一
 * 值的重复 -- 若是，就应当应用去重的"单值"策略。
 * 该策略的总目标是确保
 * nbtsplitloc.c（它使用自己的单值策略）能找到有用的
 * 分裂点，以便随着更多重复被插入，并在存储同一重复值的各页上发生连续的最右
 * 页分裂。当
 * 该页最终分裂时，它应当达到 BTREE_SINGLEVAL_FILLFACTOR% 满，
 * 就像禁用了去重时那样。
 *
 * 我们预期受影响的负载会需要 _several_ 次单值
 * 策略去重趟（针对只存储重复的页），
 * 然后该页才会最终分裂。第一次去重趟应当只
 * 找到普通的非枢轴元组。后来的去重趟会找到
 * 已有的、受 maxpostingsize 限制的倒排列表元组，这些必须被跳
 * 过。倒数第二次趟通常是第一次真正
 * 到达 _bt_singleval_fillfactor() 的趟，因此会故意留下
 * 若干未触碰的非枢轴元组。最后一次去重趟不会释放
 * 任何空间 -- 它会跳过一切而不合并任何东西（它
 * 重走倒数第二次趟的步骤）。
 *
 * 幸运的是，多趟并不太昂贵。每一趟（在
 * 第一趟之后）不会在先前各趟留下的大型倒排列表元组上
 * 花费很多周期。每一趟都会在页尾找到一大片连续的
 * 较小重复元组，并把它们合并到一起。
 */
static bool
_bt_do_singleval(Relation rel, Page page, BTDedupState state,
				 OffsetNumber minoff, IndexTuple newitem)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	ItemId		itemid;
	IndexTuple	itup;

	itemid = PageGetItemId(page, minoff);
	itup = (IndexTuple) PageGetItem(page, itemid);

	if (_bt_keep_natts_fast(rel, newitem, itup) > nkeyatts)
	{
		itemid = PageGetItemId(page, PageGetMaxOffsetNumber(page));
		itup = (IndexTuple) PageGetItem(page, itemid);

		if (_bt_keep_natts_fast(rel, newitem, itup) > nkeyatts)
			return true;
	}

	return false;
}

/*
 * 使用"单值"策略时降低 maxpostingsize，以避免出现第六个、
 * 也是最后一个受 maxpostingsize 限制的元组。第六个、也是最后一个倒排列表
 * 元组最终会比前五个略小一些。（注意：前
 * 五个元组实际上也可能只是根本无法合并到一起的超大重复元组。
 * 发生这种情况时，去重干脆不会修改
 * 该页。）
 *
 * 当页上已经有六个倒排列表时（当前去重趟
 * 接着创建或观察到第六个超大元组之后），调用者应当结束
 * 自己的去重趟。对那些项尝试去重并无用处，它们
 * 本应在预计的页分裂之后出现在新的右兄弟页上。
 * 将来那个右兄弟页的去重趟也许会处理它们。（这就是为什么
 * 对给定叶子页的第一次单值策略
 * 去重趟通常只会找到普通的
 * 非枢轴元组 -- 见 _bt_do_singleval() 注释。）
 */
static void
_bt_singleval_fillfactor(Page page, BTDedupState state, Size newitemsz)
{
	Size		leftfree;
	int			reduction;

	/* 此计算需要与 nbtsplitloc.c 一致 */
	leftfree = PageGetPageSize(page) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));
	/* 减去新高键的大小（含枢轴堆 TID 空间） */
	leftfree -= newitemsz + MAXALIGN(sizeof(ItemPointerData));

	/*
	 * 把 maxpostingsize 减少一段，减少量等于页左半
	 * 的目标空闲空间
	 */
	reduction = leftfree * ((100 - BTREE_SINGLEVAL_FILLFACTOR) / 100.0);
	if (state->maxpostingsize > reduction)
		state->maxpostingsize -= reduction;
	else
		state->maxpostingsize = 0;
}

/*
 * 根据调用者的"base"索引元组和一份
 * 堆 TIDs 列表构造倒排列表元组。当 nhtids == 1 时，构造一个不带
 * 倒排列表的标准非枢轴元组。（倒排列表元组永远不能只有一个堆 TID，部分
 * 原因是这保证去重总会减小整个元组最终经 MAXALIGN() 对齐后的
 * 大小。）
 *
 * 约定是倒排列表从一个经 MAXALIGN() 对齐的偏移开始（而不是
 * 经 SHORTALIGN() 对齐的偏移），这与后缀截断期间
 * 把堆 TID 追加到新枢轴元组/高键时的做法一致。
 * 这有时会浪费一点空间，那些空间在原元组里只是对齐
 * 填充。遵循这一约定简化了
 * 对页去重时所用的空间记账（同一约定
 * 也简化了选择页分裂点时的记账）。
 *
 * 注意：调用者的 "htids" 数组必须唯一，并且已经按升序 TID
 * 排列。来自 "base" 的任何已有堆 TIDs 不会自动出现在
 * 所返回的倒排列表元组中（它们必须包含在 htids 数组里）。
 */
IndexTuple
_bt_form_posting(IndexTuple base, ItemPointer htids, int nhtids)
{
	uint32		keysize,
				newsize;
	IndexTuple	itup;

	if (BTreeTupleIsPosting(base))
		keysize = BTreeTupleGetPostingOffset(base);
	else
		keysize = IndexTupleSize(base);

	Assert(!BTreeTupleIsPivot(base));
	Assert(nhtids > 0 && nhtids <= PG_UINT16_MAX);
	Assert(keysize == MAXALIGN(keysize));

	/* 确定新元组的最终大小 */
	if (nhtids > 1)
		newsize = MAXALIGN(keysize +
						   nhtids * sizeof(ItemPointerData));
	else
		newsize = keysize;

	Assert(newsize <= INDEX_SIZE_MASK);
	Assert(newsize == MAXALIGN(newsize));

	/* 用 palloc0() 分配内存（与 index_form_tuple() 一致） */
	itup = palloc0(newsize);
	memcpy(itup, base, keysize);
	itup->t_info &= ~INDEX_SIZE_MASK;
	itup->t_info |= newsize;
	if (nhtids > 1)
	{
		/* 构造倒排列表元组 */
		BTreeTupleSetPosting(itup, nhtids, keysize);
		memcpy(BTreeTupleGetPosting(itup), htids,
			   sizeof(ItemPointerData) * nhtids);
		Assert(_bt_posting_valid(itup));
	}
	else
	{
		/* 构造标准非枢轴元组 */
		itup->t_info &= ~INDEX_ALT_TID_MASK;
		ItemPointerCopy(htids, &itup->t_tid);
		Assert(ItemPointerIsValid(&itup->t_tid));
	}

	return itup;
}

/*
 * 通过"更新"倒排列表元组来生成替换元组，使它
 * 不再含有需要删除的 TIDs。
 *
 * VACUUM 和索引删除都会用到它。调用者的 vacposting 参数
 * 指向待更新的现有倒排列表元组。
 *
 * 返回时，调用者的 vacposting 参数将指向最终"已更新"的
 * 元组，它在调用者的内存上下文中由 palloc() 分配。
 */
void
_bt_update_posting(BTVacuumPosting vacposting)
{
	IndexTuple	origtuple = vacposting->itup;
	uint32		keysize,
				newsize;
	IndexTuple	itup;
	int			nhtids;
	int			ui,
				d;
	ItemPointer htids;

	nhtids = BTreeTupleGetNPosting(origtuple) - vacposting->ndeletedtids;

	Assert(_bt_posting_valid(origtuple));
	Assert(nhtids > 0 && nhtids < BTreeTupleGetNPosting(origtuple));

	/*
	 * 确定新元组的最终大小。
	 *
	 * 此计算需要与 _bt_form_posting() 内部用于
	 * 新倒排列表元组的代码一致。这里避免调用 _bt_form_posting()，
	 * 以免为 htids 工作区再做一次内存分配。
	 */
	keysize = BTreeTupleGetPostingOffset(origtuple);
	if (nhtids > 1)
		newsize = MAXALIGN(keysize +
						   nhtids * sizeof(ItemPointerData));
	else
		newsize = keysize;

	Assert(newsize <= INDEX_SIZE_MASK);
	Assert(newsize == MAXALIGN(newsize));

	/* 用 palloc0() 分配内存（与 index_form_tuple() 一致） */
	itup = palloc0(newsize);
	memcpy(itup, origtuple, keysize);
	itup->t_info &= ~INDEX_SIZE_MASK;
	itup->t_info |= newsize;

	if (nhtids > 1)
	{
		/* 构造倒排列表元组 */
		BTreeTupleSetPosting(itup, nhtids, keysize);
		htids = BTreeTupleGetPosting(itup);
	}
	else
	{
		/* 构造标准非枢轴元组 */
		itup->t_info &= ~INDEX_ALT_TID_MASK;
		htids = &itup->t_tid;
	}

	ui = 0;
	d = 0;
	for (int i = 0; i < BTreeTupleGetNPosting(origtuple); i++)
	{
		if (d < vacposting->ndeletedtids && vacposting->deletetids[d] == i)
		{
			d++;
			continue;
		}
		htids[ui++] = *BTreeTupleGetPostingN(origtuple, i);
	}
	Assert(ui == nhtids);
	Assert(d == vacposting->ndeletedtids);
	Assert(nhtids == 1 || _bt_posting_valid(itup));
	Assert(nhtids > 1 || ItemPointerIsValid(&itup->t_tid));

	/* vacposting 参数的 itup 现在将指向更新后的版本 */
	vacposting->itup = itup;
}

/*
 * 为倒排列表分裂做准备：把 newitem 中的堆 TID 与原倒排列表中的堆
 * TID 交换（位于偏移 postingoff 处的那个 oposting 堆 TID）。
 * 会修改 newitem，因此调用者应当传入自己的、可以安全修改的
 * 私有副本。
 *
 * 返回新的倒排列表元组，它在调用者上下文中由 palloc() 分配。
 * 它保证与 oposting 大小相同。修改后的 newitem 才是
 * 调用者实际插入的内容。（这发生在同一个临界
 * 区内：该临界区用这里返回的新
 * 倒排列表对旧倒排列表做原地更新。）
 *
 * 虽然 newitem 与 oposting 的键必须在操作符类意义上相等，并且必须
 * 在经过底层类型的输出
 * 函数后产生相同输出，但这并不意味着它们的表示完全一致。
 * 调用者必须避免假定不存在表示差异，
 * 那些差异可能使来自 oposting 的 datum 比 newitem 中对应的
 * datum 更大或更小。例如，TOAST 输入状态的差异可能
 * 打破关于元组大小的错误假定（执行器有权
 * 按自己的标准应用 TOAST 压缩）。似乎也有可能
 * 将来会引入进一步的表示差异，
 * 以便支持 nbtree 的页级前缀压缩之类特性。
 *
 * 关于倒排列表分裂的设计细节，见 nbtree/README。
 */
IndexTuple
_bt_swap_posting(IndexTuple newitem, IndexTuple oposting, int postingoff)
{
	int			nhtids;
	char	   *replacepos;
	char	   *replaceposright;
	Size		nmovebytes;
	IndexTuple	nposting;

	nhtids = BTreeTupleGetNPosting(oposting);
	Assert(_bt_posting_valid(oposting));

	/*
	 * postingoff 参数最初是 _bt_binsrch_posting() 的返回
	 * 值。若损坏使得叶子页
	 * 含有一个不知怎么与 newitem 相同的非枢轴元组，它就会是 0（任何两个
	 * 非枢轴元组都不应当拥有相同的 TID）。这种情况
	 * 在实际环境中时不时会发生。
	 *
	 * 现在做一次基本健全性检查，以便抓住这种情况。
	 */
	if (!(postingoff > 0 && postingoff < nhtids))
		elog(ERROR, "posting list tuple with %d items cannot be split at offset %d",
			 nhtids, postingoff);

	/*
	 * 移动倒排列表中的项指针，为新项的
	 * 堆 TID 腾出空隙。我们把 TIDs 右移一位，丢掉原来的
	 * 最右 TID。（nmovebytes 不得包含 postingoff 左侧的 TIDs，
	 * 也不得包含将被覆盖的现有最右/最大 TID。）
	 */
	nposting = CopyIndexTuple(oposting);
	replacepos = (char *) BTreeTupleGetPostingN(nposting, postingoff);
	replaceposright = (char *) BTreeTupleGetPostingN(nposting, postingoff + 1);
	nmovebytes = (nhtids - postingoff - 1) * sizeof(ItemPointerData);
	memmove(replaceposright, replacepos, nmovebytes);

	/* 用新项的 TID 填上 postingoff 处的空隙（原来的新 TID） */
	Assert(!BTreeTupleIsPivot(newitem) && !BTreeTupleIsPosting(newitem));
	ItemPointerCopy(&newitem->t_tid, (ItemPointer) replacepos);

	/* 现在把 oposting 的最右/最大 TID 复制到新项中（最终的新 TID） */
	ItemPointerCopy(BTreeTupleGetMaxHeapTID(oposting), &newitem->t_tid);

	Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(nposting),
							  BTreeTupleGetHeapTID(newitem)) < 0);
	Assert(_bt_posting_valid(nposting));

	return nposting;
}

/*
 * 校验 "posting" 的倒排列表不变量；它必须是倒排列表
 * 元组。在断言中使用。
 */
#ifdef USE_ASSERT_CHECKING
static bool
_bt_posting_valid(IndexTuple posting)
{
	ItemPointerData last;
	ItemPointer htid;

	if (!BTreeTupleIsPosting(posting) || BTreeTupleGetNPosting(posting) < 2)
		return false;

	/* 记住循环用的第一个堆 TID */
	ItemPointerCopy(BTreeTupleGetHeapTID(posting), &last);
	if (!ItemPointerIsValid(&last))
		return false;

	/* 从第二个 TID 开始迭代 */
	for (int i = 1; i < BTreeTupleGetNPosting(posting); i++)
	{
		htid = BTreeTupleGetPostingN(posting, i);

		if (!ItemPointerIsValid(htid))
			return false;
		if (ItemPointerCompare(htid, &last) <= 0)
			return false;
		ItemPointerCopy(htid, &last);
	}

	return true;
}
#endif
