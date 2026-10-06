/*-------------------------------------------------------------------------
 *
 * spgxlog.c
 *	  SP-GiST 的 WAL 重放逻辑。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/spgist/spgxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "storage/standby.h"
#include "utils/memutils.h"


static MemoryContext opCtx;		/* 操作用的工作内存 */


/*
 * 准备一个仅含重放所需最少信息的假 SpGistState。
 *
 * 目前我们只需要足以支持 spgFormDeadTuple() 的信息，
 * 以及 isBuild 标志。
 */
static void
fillFakeState(SpGistState *state, spgxlogState stateSrc)
{
	memset(state, 0, sizeof(*state));

	state->myXid = stateSrc.myXid;
	state->isBuild = stateSrc.isBuild;
	state->deadTupleStorage = palloc0(SGDTSIZE);
}

/*
 * 添加叶元组，或替换已有的占位元组。用于重放
 * SpGistPageAddNewItem() 操作。若偏移指向已有元组，
 * 则该元组最好是占位元组。
 */
static void
addOrReplaceTuple(Page page, Item tuple, int size, OffsetNumber offset)
{
	if (offset <= PageGetMaxOffsetNumber(page))
	{
		SpGistDeadTuple dt = (SpGistDeadTuple) PageGetItem(page,
														   PageGetItemId(page, offset));

		if (dt->tupstate != SPGIST_PLACEHOLDER)
			elog(ERROR, "SPGiST tuple to be replaced is not a placeholder");

		Assert(SpGistPageGetOpaque(page)->nPlaceholder > 0);
		SpGistPageGetOpaque(page)->nPlaceholder--;

		PageIndexTupleDelete(page, offset);
	}

	Assert(offset <= PageGetMaxOffsetNumber(page) + 1);

	if (PageAddItem(page, tuple, size, offset, false, false) != offset)
		elog(ERROR, "failed to add item of size %u to SPGiST index page",
			 size);
}

static void
spgRedoAddLeaf(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddLeaf *xldata = (spgxlogAddLeaf *) ptr;
	char	   *leafTuple;
	SpGistLeafTupleData leafTupleHdr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogAddLeaf);
	leafTuple = ptr;
	/* 叶元组未对齐，故复制一份以便访问其头部 */
	memcpy(&leafTupleHdr, leafTuple, sizeof(SpGistLeafTupleData));

	/*
	 * 正常操作中我们会同时锁住当前页和父页；
	 * 但在 WAL 重放中，先更新叶页再更新父页
	 * 应当是安全的。
	 */
	if (xldata->newPage)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		SpGistInitBuffer(buffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		/* 插入新元组 */
		if (xldata->offnumLeaf != xldata->offnumHeadLeaf)
		{
			/* 常规情形，元组由 SpGistPageAddNewItem 添加 */
			addOrReplaceTuple(page, (Item) leafTuple, leafTupleHdr.size,
							  xldata->offnumLeaf);

			/* 若需要则更新头元组的链指针 */
			if (xldata->offnumHeadLeaf != InvalidOffsetNumber)
			{
				SpGistLeafTuple head;

				head = (SpGistLeafTuple) PageGetItem(page,
													 PageGetItemId(page, xldata->offnumHeadLeaf));
				Assert(SGLT_GET_NEXTOFFSET(head) == SGLT_GET_NEXTOFFSET(&leafTupleHdr));
				SGLT_SET_NEXTOFFSET(head, xldata->offnumLeaf);
			}
		}
		else
		{
			/* 替换 DEAD 元组 */
			PageIndexTupleDelete(page, xldata->offnumLeaf);
			if (PageAddItem(page,
							(Item) leafTuple, leafTupleHdr.size,
							xldata->offnumLeaf, false, false) != xldata->offnumLeaf)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 leafTupleHdr.size);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* 若有必要则更新父节点下行链接 */
	if (xldata->offnumParent != InvalidOffsetNumber)
	{
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			SpGistInnerTuple tuple;
			BlockNumber blknoLeaf;

			XLogRecGetBlockTag(record, 0, NULL, NULL, &blknoLeaf);

			page = BufferGetPage(buffer);

			tuple = (SpGistInnerTuple) PageGetItem(page,
												   PageGetItemId(page, xldata->offnumParent));

			spgUpdateNodeLink(tuple, xldata->nodeI,
							  blknoLeaf, xldata->offnumLeaf);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

static void
spgRedoMoveLeafs(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogMoveLeafs *xldata = (spgxlogMoveLeafs *) ptr;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	int			nInsert;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;
	BlockNumber blknoDst;

	XLogRecGetBlockTag(record, 1, NULL, NULL, &blknoDst);

	fillFakeState(&state, xldata->stateSrc);

	nInsert = xldata->replaceDead ? 1 : xldata->nMoves + 1;

	ptr += SizeOfSpgxlogMoveLeafs;
	toDelete = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMoves;
	toInsert = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * nInsert;

	/* 此时 ptr 指向叶元组列表 */

	/*
	 * 正常操作中我们会同时锁住全部三页（源页、目标页和
	 * 父页）；但在 WAL 重放中，只要顺序正确，一次更新
	 * 一页应当是安全的。
	 */

	/* 先在目标页插入元组（先做，以使重定向有效） */
	if (xldata->newPage)
	{
		buffer = XLogInitBufferForRedo(record, 1);
		SpGistInitBuffer(buffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 1, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		int			i;

		page = BufferGetPage(buffer);

		for (i = 0; i < nInsert; i++)
		{
			char	   *leafTuple;
			SpGistLeafTupleData leafTupleHdr;

			/*
			 * 元组未对齐，故必须复制才能访问 size
			 * 字段。
			 */
			leafTuple = ptr;
			memcpy(&leafTupleHdr, leafTuple,
				   sizeof(SpGistLeafTupleData));

			addOrReplaceTuple(page, (Item) leafTuple,
							  leafTupleHdr.size, toInsert[i]);
			ptr += leafTupleHdr.size;
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* 从源页删除元组，并插入重定向指针 */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		spgPageIndexMultiDelete(&state, page, toDelete, xldata->nMoves,
								state.isBuild ? SPGIST_PLACEHOLDER : SPGIST_REDIRECT,
								SPGIST_PLACEHOLDER,
								blknoDst,
								toInsert[nInsert - 1]);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* 并更新父节点下行链接 */
	if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
	{
		SpGistInnerTuple tuple;

		page = BufferGetPage(buffer);

		tuple = (SpGistInnerTuple) PageGetItem(page,
											   PageGetItemId(page, xldata->offnumParent));

		spgUpdateNodeLink(tuple, xldata->nodeI,
						  blknoDst, toInsert[nInsert - 1]);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoAddNode(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogAddNode *xldata = (spgxlogAddNode *) ptr;
	char	   *innerTuple;
	SpGistInnerTupleData innerTupleHdr;
	SpGistState state;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogAddNode);
	innerTuple = ptr;
	/* 元组未对齐，故复制一份以便访问其头部 */
	memcpy(&innerTupleHdr, innerTuple, sizeof(SpGistInnerTupleData));

	fillFakeState(&state, xldata->stateSrc);

	if (!XLogRecHasBlockRef(record, 1))
	{
		/* 就地更新 */
		Assert(xldata->parentBlk == -1);
		if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			PageIndexTupleDelete(page, xldata->offnum);
			if (PageAddItem(page, (Item) innerTuple, innerTupleHdr.size,
							xldata->offnum,
							false, false) != xldata->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 innerTupleHdr.size);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
	else
	{
		BlockNumber blkno;
		BlockNumber blknoNew;

		XLogRecGetBlockTag(record, 0, NULL, NULL, &blkno);
		XLogRecGetBlockTag(record, 1, NULL, NULL, &blknoNew);

		/*
		 * 正常操作中我们会同时锁住全部三页（源页、目标页
		 * 和父页）；但在 WAL 重放中，只要顺序正确，
		 * 一次更新一页应当是安全的。
		 * 我们必须先插入新元组，再把旧元组替换为
		 * 重定向元组。
		 */

		/* 先装入新元组，以使重定向有效 */
		if (xldata->newPage)
		{
			/* AddNode 不用于空值页 */
			buffer = XLogInitBufferForRedo(record, 1);
			SpGistInitBuffer(buffer, 0);
			action = BLK_NEEDS_REDO;
		}
		else
			action = XLogReadBufferForRedo(record, 1, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			addOrReplaceTuple(page, (Item) innerTuple,
							  innerTupleHdr.size, xldata->offnumNew);

			/*
			 * 若父节点在同一页上，现在就更新它。
			 */
			if (xldata->parentBlk == 1)
			{
				SpGistInnerTuple parentTuple;

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);
			}
			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/* 删除旧元组，用重定向或占位元组替换它 */
		if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
		{
			SpGistDeadTuple dt;

			page = BufferGetPage(buffer);

			if (state.isBuild)
				dt = spgFormDeadTuple(&state, SPGIST_PLACEHOLDER,
									  InvalidBlockNumber,
									  InvalidOffsetNumber);
			else
				dt = spgFormDeadTuple(&state, SPGIST_REDIRECT,
									  blknoNew,
									  xldata->offnumNew);

			PageIndexTupleDelete(page, xldata->offnum);
			if (PageAddItem(page, (Item) dt, dt->size,
							xldata->offnum,
							false, false) != xldata->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 dt->size);

			if (state.isBuild)
				SpGistPageGetOpaque(page)->nPlaceholder++;
			else
				SpGistPageGetOpaque(page)->nRedirection++;

			/*
			 * 若父节点在同一页上，现在就更新它。
			 */
			if (xldata->parentBlk == 0)
			{
				SpGistInnerTuple parentTuple;

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);
			}
			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/*
		 * 更新父节点下行链接（若尚未作为源页或
		 * 目标页更新的一部分完成）。
		 */
		if (xldata->parentBlk == 2)
		{
			if (XLogReadBufferForRedo(record, 2, &buffer) == BLK_NEEDS_REDO)
			{
				SpGistInnerTuple parentTuple;

				page = BufferGetPage(buffer);

				parentTuple = (SpGistInnerTuple) PageGetItem(page,
															 PageGetItemId(page, xldata->offnumParent));

				spgUpdateNodeLink(parentTuple, xldata->nodeI,
								  blknoNew, xldata->offnumNew);

				PageSetLSN(page, lsn);
				MarkBufferDirty(buffer);
			}
			if (BufferIsValid(buffer))
				UnlockReleaseBuffer(buffer);
		}
	}
}

static void
spgRedoSplitTuple(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogSplitTuple *xldata = (spgxlogSplitTuple *) ptr;
	char	   *prefixTuple;
	SpGistInnerTupleData prefixTupleHdr;
	char	   *postfixTuple;
	SpGistInnerTupleData postfixTupleHdr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	ptr += sizeof(spgxlogSplitTuple);
	prefixTuple = ptr;
	/* 前缀元组未对齐，故复制一份以便访问其头部 */
	memcpy(&prefixTupleHdr, prefixTuple, sizeof(SpGistInnerTupleData));
	ptr += prefixTupleHdr.size;
	postfixTuple = ptr;
	/* 后缀元组也未对齐 */
	memcpy(&postfixTupleHdr, postfixTuple, sizeof(SpGistInnerTupleData));

	/*
	 * 正常操作中我们会同时锁住两页；但在 WAL
	 * 重放中，只要顺序正确，一次更新一页应当
	 * 是安全的。
	 */

	/* 先插入后缀元组，以避免悬空链接 */
	if (!xldata->postfixBlkSame)
	{
		if (xldata->newPage)
		{
			buffer = XLogInitBufferForRedo(record, 1);
			/* SplitTuple 不用于空值页 */
			SpGistInitBuffer(buffer, 0);
			action = BLK_NEEDS_REDO;
		}
		else
			action = XLogReadBufferForRedo(record, 1, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			addOrReplaceTuple(page, (Item) postfixTuple,
							  postfixTupleHdr.size, xldata->offnumPostfix);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}

	/* 现在处理原页 */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		PageIndexTupleDelete(page, xldata->offnumPrefix);
		if (PageAddItem(page, (Item) prefixTuple, prefixTupleHdr.size,
						xldata->offnumPrefix, false, false) != xldata->offnumPrefix)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 prefixTupleHdr.size);

		if (xldata->postfixBlkSame)
			addOrReplaceTuple(page, (Item) postfixTuple,
							  postfixTupleHdr.size,
							  xldata->offnumPostfix);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoPickSplit(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogPickSplit *xldata = (spgxlogPickSplit *) ptr;
	char	   *innerTuple;
	SpGistInnerTupleData innerTupleHdr;
	SpGistState state;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	uint8	   *leafPageSelect;
	Buffer		srcBuffer;
	Buffer		destBuffer;
	Buffer		innerBuffer;
	Page		srcPage;
	Page		destPage;
	Page		page;
	int			i;
	BlockNumber blknoInner;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 2, NULL, NULL, &blknoInner);

	fillFakeState(&state, xldata->stateSrc);

	ptr += SizeOfSpgxlogPickSplit;
	toDelete = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nDelete;
	toInsert = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nInsert;
	leafPageSelect = (uint8 *) ptr;
	ptr += sizeof(uint8) * xldata->nInsert;

	innerTuple = ptr;
	/* 内元组未对齐，故复制一份以便访问其头部 */
	memcpy(&innerTupleHdr, innerTuple, sizeof(SpGistInnerTupleData));
	ptr += innerTupleHdr.size;

	/* 此时 ptr 指向叶元组列表 */

	if (xldata->isRootSplit)
	{
		/* 分裂根时，我们只以新内页的身份触碰它 */
		srcBuffer = InvalidBuffer;
		srcPage = NULL;
	}
	else if (xldata->initSrc)
	{
		/* 仅重新初始化源页 */
		srcBuffer = XLogInitBufferForRedo(record, 0);
		srcPage = (Page) BufferGetPage(srcBuffer);

		SpGistInitBuffer(srcBuffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* 完成前先不更新 LSN 等 */
	}
	else
	{
		/*
		 * 从源页删除指定元组。（若处于 Hot Standby，需要
		 * 一直持有页锁，直到插完叶元组和新内元组为止，
		 * 否则新加的重定向元组会成为悬空
		 * 链接。）
		 */
		srcPage = NULL;
		if (XLogReadBufferForRedo(record, 0, &srcBuffer) == BLK_NEEDS_REDO)
		{
			srcPage = BufferGetPage(srcBuffer);

			/*
			 * 这里比 doPickSplit() 里容易些，因为我们已经
			 * 知道内元组的位置，所以现在就能注入正确的
			 * 重定向元组。
			 */
			if (!state.isBuild)
				spgPageIndexMultiDelete(&state, srcPage,
										toDelete, xldata->nDelete,
										SPGIST_REDIRECT,
										SPGIST_PLACEHOLDER,
										blknoInner,
										xldata->offnumInner);
			else
				spgPageIndexMultiDelete(&state, srcPage,
										toDelete, xldata->nDelete,
										SPGIST_PLACEHOLDER,
										SPGIST_PLACEHOLDER,
										InvalidBlockNumber,
										InvalidOffsetNumber);

			/* 完成前先不更新 LSN 等 */
		}
	}

	/* 若有目标页则尝试访问 */
	if (!XLogRecHasBlockRef(record, 1))
	{
		destBuffer = InvalidBuffer;
		destPage = NULL;
	}
	else if (xldata->initDest)
	{
		/* 仅重新初始化目标页 */
		destBuffer = XLogInitBufferForRedo(record, 1);
		destPage = (Page) BufferGetPage(destBuffer);

		SpGistInitBuffer(destBuffer,
						 SPGIST_LEAF | (xldata->storesNulls ? SPGIST_NULLS : 0));
		/* 完成前先不更新 LSN 等 */
	}
	else
	{
		/*
		 * 在整页映像情形下或许可以立刻释放页锁，
		 * 但为安全起见还是先持有到稍后。
		 */
		if (XLogReadBufferForRedo(record, 1, &destBuffer) == BLK_NEEDS_REDO)
			destPage = (Page) BufferGetPage(destBuffer);
		else
			destPage = NULL;	/* 不做任何页更新 */
	}

	/* 将叶元组恢复到源页和/或目标页 */
	for (i = 0; i < xldata->nInsert; i++)
	{
		char	   *leafTuple;
		SpGistLeafTupleData leafTupleHdr;

		/* 元组未对齐，故必须复制才能访问 size 字段。 */
		leafTuple = ptr;
		memcpy(&leafTupleHdr, leafTuple, sizeof(SpGistLeafTupleData));
		ptr += leafTupleHdr.size;

		page = leafPageSelect[i] ? destPage : srcPage;
		if (page == NULL)
			continue;			/* 无需触碰此页 */

		addOrReplaceTuple(page, (Item) leafTuple, leafTupleHdr.size,
						  toInsert[i]);
	}

	/* 若需要，现在更新源页和目标页的 LSNs */
	if (srcPage != NULL)
	{
		PageSetLSN(srcPage, lsn);
		MarkBufferDirty(srcBuffer);
	}
	if (destPage != NULL)
	{
		PageSetLSN(destPage, lsn);
		MarkBufferDirty(destBuffer);
	}

	/* 恢复新的内元组 */
	if (xldata->initInner)
	{
		innerBuffer = XLogInitBufferForRedo(record, 2);
		SpGistInitBuffer(innerBuffer, (xldata->storesNulls ? SPGIST_NULLS : 0));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 2, &innerBuffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(innerBuffer);

		addOrReplaceTuple(page, (Item) innerTuple, innerTupleHdr.size,
						  xldata->offnumInner);

		/* 若内页同时也是父页，顺便更新链接 */
		if (xldata->innerIsParent)
		{
			SpGistInnerTuple parent;

			parent = (SpGistInnerTuple) PageGetItem(page,
													PageGetItemId(page, xldata->offnumParent));
			spgUpdateNodeLink(parent, xldata->nodeI,
							  blknoInner, xldata->offnumInner);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(innerBuffer);
	}
	if (BufferIsValid(innerBuffer))
		UnlockReleaseBuffer(innerBuffer);

	/*
	 * 现在可以释放叶页锁。在更新父节点下行链接
	 * 之前这样做是可以的。
	 */
	if (BufferIsValid(srcBuffer))
		UnlockReleaseBuffer(srcBuffer);
	if (BufferIsValid(destBuffer))
		UnlockReleaseBuffer(destBuffer);

	/* 更新父节点下行链接，除非上面已经做过 */
	if (XLogRecHasBlockRef(record, 3))
	{
		Buffer		parentBuffer;

		if (XLogReadBufferForRedo(record, 3, &parentBuffer) == BLK_NEEDS_REDO)
		{
			SpGistInnerTuple parent;

			page = BufferGetPage(parentBuffer);

			parent = (SpGistInnerTuple) PageGetItem(page,
													PageGetItemId(page, xldata->offnumParent));
			spgUpdateNodeLink(parent, xldata->nodeI,
							  blknoInner, xldata->offnumInner);

			PageSetLSN(page, lsn);
			MarkBufferDirty(parentBuffer);
		}
		if (BufferIsValid(parentBuffer))
			UnlockReleaseBuffer(parentBuffer);
	}
	else
		Assert(xldata->innerIsParent || xldata->isRootSplit);
}

static void
spgRedoVacuumLeaf(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumLeaf *xldata = (spgxlogVacuumLeaf *) ptr;
	OffsetNumber *toDead;
	OffsetNumber *toPlaceholder;
	OffsetNumber *moveSrc;
	OffsetNumber *moveDest;
	OffsetNumber *chainSrc;
	OffsetNumber *chainDest;
	SpGistState state;
	Buffer		buffer;
	Page		page;
	int			i;

	fillFakeState(&state, xldata->stateSrc);

	ptr += SizeOfSpgxlogVacuumLeaf;
	toDead = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nDead;
	toPlaceholder = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nPlaceholder;
	moveSrc = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMove;
	moveDest = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nMove;
	chainSrc = (OffsetNumber *) ptr;
	ptr += sizeof(OffsetNumber) * xldata->nChain;
	chainDest = (OffsetNumber *) ptr;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		spgPageIndexMultiDelete(&state, page,
								toDead, xldata->nDead,
								SPGIST_DEAD, SPGIST_DEAD,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		spgPageIndexMultiDelete(&state, page,
								toPlaceholder, xldata->nPlaceholder,
								SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		/* 见 vacuumLeafPage() 中的注释 */
		for (i = 0; i < xldata->nMove; i++)
		{
			ItemId		idSrc = PageGetItemId(page, moveSrc[i]);
			ItemId		idDest = PageGetItemId(page, moveDest[i]);
			ItemIdData	tmp;

			tmp = *idSrc;
			*idSrc = *idDest;
			*idDest = tmp;
		}

		spgPageIndexMultiDelete(&state, page,
								moveSrc, xldata->nMove,
								SPGIST_PLACEHOLDER, SPGIST_PLACEHOLDER,
								InvalidBlockNumber,
								InvalidOffsetNumber);

		for (i = 0; i < xldata->nChain; i++)
		{
			SpGistLeafTuple lt;

			lt = (SpGistLeafTuple) PageGetItem(page,
											   PageGetItemId(page, chainSrc[i]));
			Assert(lt->tupstate == SPGIST_LIVE);
			SGLT_SET_NEXTOFFSET(lt, chainDest[i]);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoVacuumRoot(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRoot *xldata = (spgxlogVacuumRoot *) ptr;
	OffsetNumber *toDelete;
	Buffer		buffer;
	Page		page;

	toDelete = xldata->offsets;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		/* 元组编号是有序的 */
		PageIndexMultiDelete(page, toDelete, xldata->nDelete);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
spgRedoVacuumRedirect(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char	   *ptr = XLogRecGetData(record);
	spgxlogVacuumRedirect *xldata = (spgxlogVacuumRedirect *) ptr;
	OffsetNumber *itemToPlaceholder;
	Buffer		buffer;

	itemToPlaceholder = xldata->offsets;

	/*
	 * 若有任何重定向元组将被移除，确保没有仍存活的
	 * Hot Standby 事务可能需要看到它们。
	 */
	if (InHotStandby)
	{
		RelFileLocator locator;

		XLogRecGetBlockTag(record, 0, &locator, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(xldata->snapshotConflictHorizon,
											xldata->isCatalogRel,
											locator);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
		int			i;

		/* 将重定向指针转换为普通占位符 */
		for (i = 0; i < xldata->nToPlaceholder; i++)
		{
			SpGistDeadTuple dt;

			dt = (SpGistDeadTuple) PageGetItem(page,
											   PageGetItemId(page, itemToPlaceholder[i]));
			Assert(dt->tupstate == SPGIST_REDIRECT);
			dt->tupstate = SPGIST_PLACEHOLDER;
			ItemPointerSetInvalid(&dt->pointer);
		}

		Assert(opaque->nRedirection >= xldata->nToPlaceholder);
		opaque->nRedirection -= xldata->nToPlaceholder;
		opaque->nPlaceholder += xldata->nToPlaceholder;

		/* 移除页末的占位元组 */
		if (xldata->firstPlaceholder != InvalidOffsetNumber)
		{
			int			max = PageGetMaxOffsetNumber(page);
			OffsetNumber *toDelete;

			toDelete = palloc(sizeof(OffsetNumber) * max);

			for (i = xldata->firstPlaceholder; i <= max; i++)
				toDelete[i - xldata->firstPlaceholder] = i;

			i = max - xldata->firstPlaceholder + 1;
			Assert(opaque->nPlaceholder >= i);
			opaque->nPlaceholder -= i;

			/* 数组已排序，故可用 PageIndexMultiDelete */
			PageIndexMultiDelete(page, toDelete, i);

			pfree(toDelete);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

void
spg_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCxt;

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_SPGIST_ADD_LEAF:
			spgRedoAddLeaf(record);
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			spgRedoMoveLeafs(record);
			break;
		case XLOG_SPGIST_ADD_NODE:
			spgRedoAddNode(record);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			spgRedoSplitTuple(record);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			spgRedoPickSplit(record);
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			spgRedoVacuumLeaf(record);
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			spgRedoVacuumRoot(record);
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			spgRedoVacuumRedirect(record);
			break;
		default:
			elog(PANIC, "spg_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

void
spg_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "SP-GiST temporary context",
								  ALLOCSET_DEFAULT_SIZES);
}

void
spg_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}

/*
 * 在对 SpGist 页做一致性检查之前先掩码。
 */
void
spg_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	PageHeader	pagehdr = (PageHeader) page;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);

	/*
	 * 掩码未使用空间，但仅当页的 pd_lower 看起来
	 * 已被正确设置时才做。
	 */
	if (pagehdr->pd_lower >= SizeOfPageHeaderData)
		mask_unused_space(page);
}
