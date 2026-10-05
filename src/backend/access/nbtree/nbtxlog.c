/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  B-树的 WAL 重放逻辑。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

static MemoryContext opCtx;		/* 操作用的工作内存 */

/*
 * _bt_restore_page -- 把页上的全部索引元组重新放回
 *
 * 该页是刚初始化的，*from（长度为 len）是它原先上半部分
 * （从 pd_upper 到 pd_special）的副本。我们假定这些元组曾按
 * 项号顺序加入该页，因此项号最高的那个最先出现
 * （位于页上最低的位置）。
 */
static void
_bt_restore_page(Page page, char *from, int len)
{
	IndexTupleData itupdata;
	Size		itemsz;
	char	   *end = from + len;
	Item		items[MaxIndexTuplesPerPage];
	uint16		itemsizes[MaxIndexTuplesPerPage];
	int			i;
	int			nitems;

	/*
	 * 为了按原来的顺序取回这些项，我们以相反顺序把它们加入该页。
	 * 要弄清一个元组在何处结束、下一个从何处开始，必须先按正序
	 * 扫描它们。
	 */
	i = 0;
	while (from < end)
	{
		/*
		 * 逐项前进时，'from' 并不总是正确对齐的，
		 * 所以需要使用 memcpy()。此外，这里的 items 数组
		 * 同样使用 Item（它只是 char*），原因相同；
		 * 不希望编译器或任何人以为某个项已经对齐，
		 * 而实际上并没有。
		 */
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleSize(&itupdata);
		itemsz = MAXALIGN(itemsz);

		items[i] = (Item) from;
		itemsizes[i] = itemsz;
		i++;

		from += itemsz;
	}
	nitems = i;

	for (i = nitems - 1; i >= 0; i--)
	{
		if (PageAddItem(page, items[i], itemsizes[i], nitems - i,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "_bt_restore_page: cannot add item to page");
	}
}

static void
_bt_restore_meta(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;
	xl_btree_metadata *xlrec;
	char	   *ptr;
	Size		len;

	metabuf = XLogInitBufferForRedo(record, block_id);
	ptr = XLogRecGetBlockData(record, block_id, &len);

	Assert(len == sizeof(xl_btree_metadata));
	Assert(BufferGetBlockNumber(metabuf) == BTREE_METAPAGE);
	xlrec = (xl_btree_metadata *) ptr;
	metapg = BufferGetPage(metabuf);

	_bt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = xlrec->version;
	md->btm_root = xlrec->root;
	md->btm_level = xlrec->level;
	md->btm_fastroot = xlrec->fastroot;
	md->btm_fastlevel = xlrec->fastlevel;
	/* 未经升级无法记录 BTREE_MIN_VERSION 索引元页 */
	Assert(md->btm_version >= BTREE_NOVAC_VERSION);
	md->btm_last_cleanup_num_delpages = xlrec->last_cleanup_num_delpages;
	md->btm_last_cleanup_num_heap_tuples = -1.0;
	md->btm_allequalimage = xlrec->allequalimage;

	pageop = BTPageGetOpaque(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * 把 pd_lower 设到元数据末尾刚刚过后的位置。这一步必不可少，
	 * 因为若不这样做，xlog.c 压缩该页时
	 * 元数据就会丢失。
	 */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * _bt_clear_incomplete_split -- 清除一页上的 INCOMPLETE_SPLIT 标志
 *
 * 这是所有能够插入下行链接的 WAL 记录类型
 * 的重放函数的公共子程序：insert、split 与 newroot。
 */
static void
_bt_clear_incomplete_split(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buf;

	if (XLogReadBufferForRedo(record, block_id, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buf);
		BTPageOpaque pageop = BTPageGetOpaque(page);

		Assert(P_INCOMPLETE_SPLIT(pageop));
		pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_insert(bool isleaf, bool ismeta, bool posting,
				  XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	/*
	 * 向内部页插入会完成子层上一次未完成的分裂。
	 * 清除子页中的未完成分裂标志。注意：正常操作期间，
	 * 子页与父页同时加锁（这些锁是耦合的），
	 * 因此清除标志与插入下行链接对其他后端
	 * 看起来是原子的。重放期间我们不必这么做，
	 * 因为读者并不关心未完成分裂标志，
	 * 而且不可能有更新正在进行。
	 */
	if (!isleaf)
		_bt_clear_incomplete_split(record, 1);
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *datapos = XLogRecGetBlockData(record, 0, &datalen);

		page = BufferGetPage(buffer);

		if (!posting)
		{
			/* 简单的单条插入 */
			if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
							false, false) == InvalidOffsetNumber)
				elog(PANIC, "failed to add new item");
		}
		else
		{
			ItemId		itemid;
			IndexTuple	oposting,
						newitem,
						nposting;
			uint16		postingoff;

			/*
			 * 叶子页插入期间发生了倒排列表分裂。WAL 记录数据
			 * 以一个偏移号开始，表示现有倒排列表中
			 * 发生分裂的位置。
			 *
			 * 用 _bt_swap_posting() 重复主库上的倒排列表分裂步骤。
			 * 注意：WAL 记录中的 newitem 是 'orignewitem'，
			 * 而不是实际插入到页上的
			 * 最终版本 newitem。
			 */
			postingoff = *((uint16 *) datapos);
			datapos += sizeof(uint16);
			datalen -= sizeof(uint16);

			itemid = PageGetItemId(page, OffsetNumberPrev(xlrec->offnum));
			oposting = (IndexTuple) PageGetItem(page, itemid);

			/* 在 _bt_swap_posting() 中使用可变且已对齐的 newitem 副本 */
			Assert(isleaf && postingoff > 0);
			newitem = CopyIndexTuple((IndexTuple) datapos);
			nposting = _bt_swap_posting(newitem, oposting, postingoff);

			/* 用分裂后的版本替换现有倒排列表 */
			memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

			/* 插入 "最终" 新项（不是 WAL 流中的 orignewitem） */
			Assert(IndexTupleSize(newitem) == datalen);
			if (PageAddItem(page, (Item) newitem, datalen, xlrec->offnum,
							false, false) == InvalidOffsetNumber)
				elog(PANIC, "failed to add posting split new item");
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * 注意：正常操作中，我们会在仍持有
	 * 所插入页的锁时更新元页。但重放期间不必持有那把锁，
	 * 因为不会有其他索引更新并发进行，
	 * 读者跟随元页上一条过时链接
	 * 也完全没问题。
	 */
	if (ismeta)
		_bt_restore_meta(record, 2);
}

static void
btree_xlog_split(bool newitemonleft, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	bool		isleaf = (xlrec->level == 0);
	Buffer		buf;
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque ropaque;
	char	   *datapos;
	Size		datalen;
	BlockNumber origpagenumber;
	BlockNumber rightpagenumber;
	BlockNumber spagenumber;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &origpagenumber);
	XLogRecGetBlockTag(record, 1, NULL, NULL, &rightpagenumber);
	if (!XLogRecGetBlockTagExtended(record, 2, NULL, NULL, &spagenumber, NULL))
		spagenumber = P_NONE;

	/*
	 * 当 origpage/buf 是内部页时，清除下一层
	 * 相应子页上的未完成分裂标志（若发生内部页分裂，
	 * 最初执行期间必定有级联页分裂）。
	 * 这与内部页上对应的 btree_xlog_insert 调用类似。
	 * 这里并不清除当前这次页分裂的未完成分裂标志
	 * （可以把它看成页分裂动作顺带插入 newitem
	 * 的一部分）。
	 *
	 * 与 btree_xlog_insert 中一样，这可以在锁定
	 * 其他页之前完成。重放例程中永远不需要
	 * 耦合跨层锁。
	 */
	if (!isleaf)
		_bt_clear_incomplete_split(record, 3);

	/* 从头重建右侧（新）兄弟页 */
	rbuf = XLogInitBufferForRedo(record, 1);
	datapos = XLogRecGetBlockData(record, 1, &datalen);
	rpage = (Page) BufferGetPage(rbuf);

	_bt_pageinit(rpage, BufferGetPageSize(rbuf));
	ropaque = BTPageGetOpaque(rpage);

	ropaque->btpo_prev = origpagenumber;
	ropaque->btpo_next = spagenumber;
	ropaque->btpo_level = xlrec->level;
	ropaque->btpo_flags = isleaf ? BTP_LEAF : 0;
	ropaque->btpo_cycleid = 0;

	_bt_restore_page(rpage, datapos, datalen);

	PageSetLSN(rpage, lsn);
	MarkBufferDirty(rbuf);

	/* 现在重建原始页（分裂的左半边） */
	if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
	{
		/*
		 * 为保留这些元组原先的物理顺序，
		 * 我们为左页初始化一张临时空页，
		 * 并按项号顺序把所有项加到那张页上。
		 * 这与 _bt_split() 的做法一致。
		 * 保留相同的物理顺序才使 WAL 一致性检查成为可能。
		 * 亦见 _bt_restore_page()，它对右页做同样的事。
		 */
		Page		origpage = (Page) BufferGetPage(buf);
		BTPageOpaque oopaque = BTPageGetOpaque(origpage);
		OffsetNumber off;
		IndexTuple	newitem = NULL,
					left_hikey = NULL,
					nposting = NULL;
		Size		newitemsz = 0,
					left_hikeysz = 0;
		Page		leftpage;
		OffsetNumber leftoff,
					replacepostingoff = InvalidOffsetNumber;

		datapos = XLogRecGetBlockData(record, 0, &datalen);

		if (newitemonleft || xlrec->postingoff != 0)
		{
			newitem = (IndexTuple) datapos;
			newitemsz = MAXALIGN(IndexTupleSize(newitem));
			datapos += newitemsz;
			datalen -= newitemsz;

			if (xlrec->postingoff != 0)
			{
				ItemId		itemid;
				IndexTuple	oposting;

				/* 倒排列表必须位于新项之前的那个偏移号 */
				replacepostingoff = OffsetNumberPrev(xlrec->newitemoff);

				/* 在 _bt_swap_posting() 中使用可变且已对齐的 newitem 副本 */
				newitem = CopyIndexTuple(newitem);
				itemid = PageGetItemId(origpage, replacepostingoff);
				oposting = (IndexTuple) PageGetItem(origpage, itemid);
				nposting = _bt_swap_posting(newitem, oposting,
											xlrec->postingoff);
			}
		}

		/*
		 * 取出左侧高键及其大小。我们假定 16 位对齐
		 * 已足以应用 IndexTupleSize（因为它读取的是
		 * 一个 uint16 字段）。
		 */
		left_hikey = (IndexTuple) datapos;
		left_hikeysz = MAXALIGN(IndexTupleSize(left_hikey));
		datapos += left_hikeysz;
		datalen -= left_hikeysz;

		Assert(datalen == 0);

		leftpage = PageGetTempPageCopySpecial(origpage);

		/* 把 WAL 中的高键元组加到临时页 */
		leftoff = P_HIKEY;
		if (PageAddItem(leftpage, (Item) left_hikey, left_hikeysz, P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add high key to left page after split");
		leftoff = OffsetNumberNext(leftoff);

		for (off = P_FIRSTDATAKEY(oopaque); off < xlrec->firstrightoff; off++)
		{
			ItemId		itemid;
			Size		itemsz;
			IndexTuple	item;

			/* 需要时加入替换用的倒排列表 */
			if (off == replacepostingoff)
			{
				Assert(newitemonleft ||
					   xlrec->firstrightoff == xlrec->newitemoff);
				if (PageAddItem(leftpage, (Item) nposting,
								MAXALIGN(IndexTupleSize(nposting)), leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new posting list item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
				continue;		/* 不要插入 oposting */
			}

			/* 若新项插在左页上，则加入该新项 */
			else if (newitemonleft && off == xlrec->newitemoff)
			{
				if (PageAddItem(leftpage, (Item) newitem, newitemsz, leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
			}

			itemid = PageGetItemId(origpage, off);
			itemsz = ItemIdGetLength(itemid);
			item = (IndexTuple) PageGetItem(origpage, itemid);
			if (PageAddItem(leftpage, (Item) item, itemsz, leftoff,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add old item to left page after split");
			leftoff = OffsetNumberNext(leftoff);
		}

		/* 应对 newitem 落在末尾的可能 */
		if (newitemonleft && off == xlrec->newitemoff)
		{
			if (PageAddItem(leftpage, (Item) newitem, newitemsz, leftoff,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add new item to left page after split");
			leftoff = OffsetNumberNext(leftoff);
		}

		PageRestoreTempPage(leftpage, origpage);

		/* 修正不透明区字段 */
		oopaque->btpo_flags = BTP_INCOMPLETE_SPLIT;
		if (isleaf)
			oopaque->btpo_flags |= BTP_LEAF;
		oopaque->btpo_next = rightpagenumber;
		oopaque->btpo_cycleid = 0;

		PageSetLSN(origpage, lsn);
		MarkBufferDirty(buf);
	}

	/* 修正新右兄弟右侧那一页的左链接 */
	if (spagenumber != P_NONE)
	{
		Buffer		sbuf;

		if (XLogReadBufferForRedo(record, 2, &sbuf) == BLK_NEEDS_REDO)
		{
			Page		spage = (Page) BufferGetPage(sbuf);
			BTPageOpaque spageop = BTPageGetOpaque(spage);

			spageop->btpo_prev = rightpagenumber;

			PageSetLSN(spage, lsn);
			MarkBufferDirty(sbuf);
		}
		if (BufferIsValid(sbuf))
			UnlockReleaseBuffer(sbuf);
	}

	/*
	 * 最后，释放其余缓冲区。sbuf、rbuf 与 buf 必须一起释放，
	 * 以免读者观察到不一致。
	 */
	UnlockReleaseBuffer(rbuf);
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_dedup(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_dedup *xlrec = (xl_btree_dedup *) XLogRecGetData(record);
	Buffer		buf;

	if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);
		Page		page = (Page) BufferGetPage(buf);
		BTPageOpaque opaque = BTPageGetOpaque(page);
		OffsetNumber offnum,
					minoff,
					maxoff;
		BTDedupState state;
		BTDedupInterval *intervals;
		Page		newpage;

		state = (BTDedupState) palloc(sizeof(BTDedupStateData));
		state->deduplicate = true;	/* 未使用 */
		state->nmaxitems = 0;	/* 未使用 */
		/* 保守地使用比主库更大的 maxpostingsize */
		state->maxpostingsize = BTMaxItemSize(page);
		state->base = NULL;
		state->baseoff = InvalidOffsetNumber;
		state->basetupsize = 0;
		state->htids = palloc(state->maxpostingsize);
		state->nhtids = 0;
		state->nitems = 0;
		state->phystupsize = 0;
		state->nintervals = 0;

		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		newpage = PageGetTempPageCopySpecial(page);

		if (!P_RIGHTMOST(opaque))
		{
			ItemId		itemid = PageGetItemId(page, P_HIKEY);
			Size		itemsz = ItemIdGetLength(itemid);
			IndexTuple	item = (IndexTuple) PageGetItem(page, itemid);

			if (PageAddItem(newpage, (Item) item, itemsz, P_HIKEY,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "deduplication failed to add highkey");
		}

		intervals = (BTDedupInterval *) ptr;
		for (offnum = minoff;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

			if (offnum == minoff)
				_bt_dedup_start_pending(state, itup, offnum);
			else if (state->nintervals < xlrec->nintervals &&
					 state->baseoff == intervals[state->nintervals].baseoff &&
					 state->nitems < intervals[state->nintervals].nitems)
			{
				if (!_bt_dedup_save_htid(state, itup))
					elog(ERROR, "deduplication failed to add heap tid to pending posting list");
			}
			else
			{
				_bt_dedup_finish_pending(newpage, state);
				_bt_dedup_start_pending(state, itup, offnum);
			}
		}

		_bt_dedup_finish_pending(newpage, state);
		Assert(state->nintervals == xlrec->nintervals);
		Assert(memcmp(state->intervals, intervals,
					  state->nintervals * sizeof(BTDedupInterval)) == 0);

		if (P_HAS_GARBAGE(opaque))
		{
			BTPageOpaque nopaque = BTPageGetOpaque(newpage);

			nopaque->btpo_flags &= ~BTP_HAS_GARBAGE;
		}

		PageRestoreTempPage(newpage, page);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
btree_xlog_updates(Page page, OffsetNumber *updatedoffsets,
				   xl_btree_update *updates, int nupdated)
{
	BTVacuumPosting vacposting;
	IndexTuple	origtuple;
	ItemId		itemid;
	Size		itemsz;

	for (int i = 0; i < nupdated; i++)
	{
		itemid = PageGetItemId(page, updatedoffsets[i]);
		origtuple = (IndexTuple) PageGetItem(page, itemid);

		vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
							updates->ndeletedtids * sizeof(uint16));
		vacposting->updatedoffset = updatedoffsets[i];
		vacposting->itup = origtuple;
		vacposting->ndeletedtids = updates->ndeletedtids;
		memcpy(vacposting->deletetids,
			   (char *) updates + SizeOfBtreeUpdate,
			   updates->ndeletedtids * sizeof(uint16));

		_bt_update_posting(vacposting);

		/* 用元组的更新版本覆盖原内容 */
		itemsz = MAXALIGN(IndexTupleSize(vacposting->itup));
		if (!PageIndexTupleOverwrite(page, updatedoffsets[i],
									 (Item) vacposting->itup, itemsz))
			elog(PANIC, "failed to update partially dead item");

		pfree(vacposting->itup);
		pfree(vacposting);

		/* 前进到数组中的下一个 xl_btree_update */
		updates = (xl_btree_update *)
			((char *) updates + SizeOfBtreeUpdate +
			 updates->ndeletedtids * sizeof(uint16));
	}
}

static void
btree_xlog_vacuum(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_vacuum *xlrec = (xl_btree_vacuum *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * 这里需要取清理锁，就像 btvacuumpage() 那样。不过，
	 * 恢复期间不必对索引中每一个块都穷尽地取清理锁
	 * （只对含有待杀死项的页取清理锁就够了）。
	 * 细节见 nbtree/README。
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &buffer)
		== BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(buffer);

		if (xlrec->nupdated > 0)
		{
			OffsetNumber *updatedoffsets;
			xl_btree_update *updates;

			updatedoffsets = (OffsetNumber *)
				(ptr + xlrec->ndeleted * sizeof(OffsetNumber));
			updates = (xl_btree_update *) ((char *) updatedoffsets +
										   xlrec->nupdated *
										   sizeof(OffsetNumber));

			btree_xlog_updates(page, updatedoffsets, updates, xlrec->nupdated);
		}

		if (xlrec->ndeleted > 0)
			PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/*
		 * 把该页标为不含任何 LP_DEAD 项——见
		 * _bt_delitems_vacuum() 中的注释。
		 */
		opaque = BTPageGetOpaque(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * 若有任何冲突处理要做，必须在
	 * 更新该页之前完成
	 */
	if (InHotStandby)
	{
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
											xlrec->isCatalogRel,
											rlocator);
	}

	/*
	 * 应用这些变更不需要取清理锁。
	 * 细节见 nbtree/README。
	 */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(buffer);

		if (xlrec->nupdated > 0)
		{
			OffsetNumber *updatedoffsets;
			xl_btree_update *updates;

			updatedoffsets = (OffsetNumber *)
				(ptr + xlrec->ndeleted * sizeof(OffsetNumber));
			updates = (xl_btree_update *) ((char *) updatedoffsets +
										   xlrec->nupdated *
										   sizeof(OffsetNumber));

			btree_xlog_updates(page, updatedoffsets, updates, xlrec->nupdated);
		}

		if (xlrec->ndeleted > 0)
			PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/* 把该页标为不含任何 LP_DEAD 项 */
		opaque = BTPageGetOpaque(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_mark_page_halfdead(uint8 info, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	IndexTupleData trunctuple;

	/*
	 * 正常操作中，我们会在改动之前锁住这条 WAL 记录
	 * 触及的全部页。WAL 重放时，一次只锁一页应当没问题，
	 * 因为不可能有并发的索引更新，
	 * 读者也不必在意自己是否到达目标页
	 * （反正它一定是空的）。
	 */

	/* 待删除子树的父页 */
	if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
	{
		OffsetNumber poffset;
		ItemId		itemid;
		IndexTuple	itup;
		OffsetNumber nextoffset;
		BlockNumber rightsib;

		page = (Page) BufferGetPage(buffer);
		pageop = BTPageGetOpaque(page);

		poffset = xlrec->poffset;

		nextoffset = OffsetNumberNext(poffset);
		itemid = PageGetItemId(page, nextoffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		rightsib = BTreeTupleGetDownLink(itup);

		itemid = PageGetItemId(page, poffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		BTreeTupleSetDownLink(itup, rightsib);
		nextoffset = OffsetNumberNext(poffset);
		PageIndexTupleDelete(page, nextoffset);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	/*
	 * 重放例程中不需要耦合跨层锁，
	 * 因此立刻释放内部页上的锁
	 */
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* 把叶子页重写为半死页 */
	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = BTPageGetOpaque(page);

	pageop->btpo_prev = xlrec->leftblk;
	pageop->btpo_next = xlrec->rightblk;
	pageop->btpo_level = 0;
	pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
	pageop->btpo_cycleid = 0;

	/*
	 * 构造一个指向最顶父页的哑高键项（当最顶父页就是叶子页本身时，
	 * 其值为 InvalidBlockNumber）
	 */
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	BTreeTupleSetTopParent(&trunctuple, xlrec->topparent);

	if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add dummy high key to half-dead page");

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}


static void
btree_xlog_unlink_page(uint8 info, XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) XLogRecGetData(record);
	BlockNumber leftsib;
	BlockNumber rightsib;
	uint32		level;
	bool		isleaf;
	FullTransactionId safexid;
	Buffer		leftbuf;
	Buffer		target;
	Buffer		rightbuf;
	Page		page;
	BTPageOpaque pageop;

	leftsib = xlrec->leftsib;
	rightsib = xlrec->rightsib;
	level = xlrec->level;
	isleaf = (level == 0);
	safexid = xlrec->safexid;

	/* level 0（叶子页）或 level 1 的目标没有 leaftopparent */
	Assert(!BlockNumberIsValid(xlrec->leaftopparent) || level > 1);

	/*
	 * 正常操作中，我们会在改动之前锁住这条 WAL 记录
	 * 触及的全部页。WAL 重放时，我们至少按同样的
	 * 标准从左到右顺序（leftsib、target、rightsib）
	 * 锁住这些页，并且在目标被标为已删除之前
	 * 不释放兄弟页的锁。
	 */

	/* 若有左兄弟，修正其右链接 */
	if (leftsib != P_NONE)
	{
		if (XLogReadBufferForRedo(record, 1, &leftbuf) == BLK_NEEDS_REDO)
		{
			page = (Page) BufferGetPage(leftbuf);
			pageop = BTPageGetOpaque(page);
			pageop->btpo_next = rightsib;

			PageSetLSN(page, lsn);
			MarkBufferDirty(leftbuf);
		}
	}
	else
		leftbuf = InvalidBuffer;

	/* 把目标页重写为空的已删除页 */
	target = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(target);

	_bt_pageinit(page, BufferGetPageSize(target));
	pageop = BTPageGetOpaque(page);

	pageop->btpo_prev = leftsib;
	pageop->btpo_next = rightsib;
	pageop->btpo_level = level;
	BTPageSetDeleted(page, safexid);
	if (isleaf)
		pageop->btpo_flags |= BTP_LEAF;
	pageop->btpo_cycleid = 0;

	PageSetLSN(page, lsn);
	MarkBufferDirty(target);

	/* 修正右兄弟的左链接 */
	if (XLogReadBufferForRedo(record, 2, &rightbuf) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(rightbuf);
		pageop = BTPageGetOpaque(page);
		pageop->btpo_prev = leftsib;

		PageSetLSN(page, lsn);
		MarkBufferDirty(rightbuf);
	}

	/* 释放兄弟页 */
	if (BufferIsValid(leftbuf))
		UnlockReleaseBuffer(leftbuf);
	if (BufferIsValid(rightbuf))
		UnlockReleaseBuffer(rightbuf);

	/* 释放目标页 */
	UnlockReleaseBuffer(target);

	/*
	 * 若删除的是目标叶子页的父页，而不是叶子页本身，
	 * 则更新该叶子页，使其指向待删除子树中
	 * 下一个仍保留的孩子
	 */
	if (XLogRecHasBlockRef(record, 3))
	{
		/*
		 * 该页上没有真正的数据，因此我们只用 WAL 记录中的信息
		 * 从头重建它。
		 *
		 * 注意：当目标页同时也是 leafbuf 页时，不会走到这里。
		 * 删除 leafbuf 时不必加入带最顶父链接的哑高键项，
		 * 因为它是正在删除的子树中
		 * 我们将删除的最后一页。
		 */
		Buffer		leafbuf;
		IndexTupleData trunctuple;

		Assert(!isleaf);

		leafbuf = XLogInitBufferForRedo(record, 3);
		page = (Page) BufferGetPage(leafbuf);

		_bt_pageinit(page, BufferGetPageSize(leafbuf));
		pageop = BTPageGetOpaque(page);

		pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
		pageop->btpo_prev = xlrec->leafleftsib;
		pageop->btpo_next = xlrec->leafrightsib;
		pageop->btpo_level = 0;
		pageop->btpo_cycleid = 0;

		/* 加入一个哑高键项 */
		MemSet(&trunctuple, 0, sizeof(IndexTupleData));
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetTopParent(&trunctuple, xlrec->leaftopparent);

		if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "could not add dummy high key to half-dead page");

		PageSetLSN(page, lsn);
		MarkBufferDirty(leafbuf);
		UnlockReleaseBuffer(leafbuf);
	}

	/* 需要时更新元页 */
	if (info == XLOG_BTREE_UNLINK_PAGE_META)
		_bt_restore_meta(record, 4);
}

static void
btree_xlog_newroot(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *ptr;
	Size		len;

	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = BTPageGetOpaque(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo_level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;
	pageop->btpo_cycleid = 0;

	if (xlrec->level > 0)
	{
		ptr = XLogRecGetBlockData(record, 0, &len);
		_bt_restore_page(page, ptr, len);

		/* 清除左孩子中的未完成分裂标志 */
		_bt_clear_incomplete_split(record, 1);
	}

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	_bt_restore_meta(record, 2);
}

/*
 * 一般而言，VACUUM 必须推迟回收，以此避开某些竞态条件。
 * 已删除页含有 safexid 值，VACUUM 用它判断现在把
 * 早先由 VACUUM 删除的页放入 FSM 是否安全。
 * 见 nbtree/README。
 *
 * 对最初执行期间运行的任何后端而言，
 * FSM 是可安全回收页的缓存；页仅仅出现在 FSM 中
 * 就表明该页必定已经可以安全回收（实际上
 * _bt_getbuf() 会用 BTPageIsRecyclable() 验证它是否安全，
 * 但这只是因为就 FSM 当前的局限而言，完全信任它
 * 并不明智）。
 *
 * 不过这不足以防止热备期间类似的并发回收竞态。
 * 为此，需要在一页实际被回收、并在 _bt_split() 内部
 * 被复用为完全无关的另一页时，记录一条
 * xl_btree_reuse_page 记录。这些记录包含原已删除页上
 * 相同的 safexid 值，存放在记录的 snapshotConflictHorizon
 * 字段中。
 *
 * BTPageIsRecyclable() 中的 GlobalVisCheckRemovableFullXid()
 * 测试用来判断回收一页是否安全。它对应我们自己的测试：
 * GetConflictingVirtualXIDs() 内部的
 * PGPROC->xmin > limitXmin 测试。因此同一个 XID 值
 * 在主库与备库上达到同样的排除效果。
 */
static void
btree_xlog_reuse_page(XLogReaderState *record)
{
	xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) XLogRecGetData(record);

	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshotFullXid(xlrec->snapshotConflictHorizon,
												   xlrec->isCatalogRel,
												   xlrec->locator);
}

void
btree_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, false, false, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, false, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, true, false, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, record);
			break;
		case XLOG_BTREE_INSERT_POST:
			btree_xlog_insert(true, false, true, record);
			break;
		case XLOG_BTREE_DEDUP:
			btree_xlog_dedup(record);
			break;
		case XLOG_BTREE_VACUUM:
			btree_xlog_vacuum(record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(record);
			break;
		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			btree_xlog_mark_page_halfdead(info, record);
			break;
		case XLOG_BTREE_UNLINK_PAGE:
		case XLOG_BTREE_UNLINK_PAGE_META:
			btree_xlog_unlink_page(info, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(record);
			break;
		case XLOG_BTREE_REUSE_PAGE:
			btree_xlog_reuse_page(record);
			break;
		case XLOG_BTREE_META_CLEANUP:
			_bt_restore_meta(record, 0);
			break;
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(opCtx);
}

void
btree_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "Btree recovery temporary context",
								  ALLOCSET_DEFAULT_SIZES);
}

void
btree_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}

/*
 * 在对 B-树页做一致性检查之前先屏蔽它。
 */
void
btree_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	BTPageOpaque maskopaq;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	maskopaq = BTPageGetOpaque(page);

	if (P_ISLEAF(maskopaq))
	{
		/*
		 * 在 B-树叶子页上，有可能不发出任何 WAL 记录
		 * 就修改 LP_FLAGS。因此屏蔽行指针标志。细节见
		 * _bt_killitems() 与 _bt_check_unique()。
		 */
		mask_lp_flags(page);
	}

	/*
	 * BTP_HAS_GARBAGE 只是一个未记日志的提示位。因此屏蔽它。
	 * 细节见 _bt_delete_or_dedup_one_page()、_bt_killitems()
	 * 与 _bt_check_unique()。
	 */
	maskopaq->btpo_flags &= ~BTP_HAS_GARBAGE;

	/*
	 * 重放 B-树页分裂时，我们不设置右兄弟的 BTP_SPLIT_END
	 * 标志，并把同一页的 cycle_id 初始化为 0。
	 * 细节见 btree_xlog_split()。
	 */
	maskopaq->btpo_flags &= ~BTP_SPLIT_END;
	maskopaq->btpo_cycleid = 0;
}
