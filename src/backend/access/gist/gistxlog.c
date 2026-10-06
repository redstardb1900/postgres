/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  GiST 的 WAL 重放逻辑。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/gist/gistxlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/gist_private.h"
#include "access/gistxlog.h"
#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static MemoryContext opCtx;		/* 操作用的工作内存 */

/*
 * 重放子页上清除 F_FOLLOW_RIGHT 标志的操作。
 *
 * 即便 WAL 记录包含整页映像，我们也必须更新 follow-right
 * 标志，因为该变更未包含在整页映像中。为确保带有错误标志值的
 * 中间状态对并发 Hot Standby 查询不可见，本函数既处理恢复整页
 * 映像，也处理更新该标志。（注意：在当前 WAL 动作中，我们从不需要
 * 对子页做任何其他事。）
  *
  *
 */
static void
gistRedoClearFollowRight(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;
	XLogRedoAction action;

	/*
	 * 注意：即便该页是从整页映像恢复的，我们仍会更新它，
	 * 因为更新后的 NSN 未包含在映像中。
	 */
	action = XLogReadBufferForRedo(record, block_id, &buffer);
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		page = BufferGetPage(buffer);

		GistPageSetNSN(page, lsn);
		GistClearFollowRight(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * 重放任何页更新（页分裂除外）
 */
static void
gistRedoPageUpdateRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *begin;
		char	   *data;
		Size		datalen;
		int			ninserted PG_USED_FOR_ASSERTS_ONLY = 0;

		data = begin = XLogRecGetBlockData(record, 0, &datalen);

		page = (Page) BufferGetPage(buffer);

		if (xldata->ntodelete == 1 && xldata->ntoinsert == 1)
		{
			/*
			 * 当用一个元组替换另一个元组时，必须使用
			 * PageIndexTupleOverwrite，以与 gistplacetopage 保持一致。
			 */
			OffsetNumber offnum = *((OffsetNumber *) data);
			IndexTuple	itup;
			Size		itupsize;

			data += sizeof(OffsetNumber);
			itup = (IndexTuple) data;
			itupsize = IndexTupleSize(itup);
			if (!PageIndexTupleOverwrite(page, offnum, (Item) itup, itupsize))
				elog(ERROR, "failed to add item to GiST index page, size %d bytes",
					 (int) itupsize);
			data += itupsize;
			/* 消费 1 个元组后应无剩余 */
			Assert(data - begin == datalen);
			/* 更新插入计数，供下方断言检查 */
			ninserted++;
		}
		else if (xldata->ntodelete > 0)
		{
			/* 否则，若有旧元组则删除 */
			OffsetNumber *todelete = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntodelete;

			PageIndexMultiDelete(page, todelete, xldata->ntodelete);
			if (GistPageIsLeaf(page))
				GistMarkTuplesDeleted(page);
		}

		/* 若有新元组则添加 */
		if (data - begin < datalen)
		{
			OffsetNumber off = (PageIsEmpty(page)) ? FirstOffsetNumber :
				OffsetNumberNext(PageGetMaxOffsetNumber(page));

			while (data - begin < datalen)
			{
				IndexTuple	itup = (IndexTuple) data;
				Size		sz = IndexTupleSize(itup);
				OffsetNumber l;

				data += sz;

				l = PageAddItem(page, (Item) itup, sz, off, false, false);
				if (l == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to GiST index page, size %d bytes",
						 (int) sz);
				off++;
				ninserted++;
			}
		}

		/* 检查 XLOG 记录是否包含预期数量的元组 */
		Assert(ninserted == xldata->ntoinsert);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	/*
	 * 修复左子页上的 follow-right 数据
	 *
	 * 这必须在仍持有目标页锁时完成。注意即便目标页已不存在，
	 * 我们仍尝试在子页上重放该变更。
	  *
	 */
	if (XLogRecHasBlockRef(record, 1))
		gistRedoClearFollowRight(record, 1);

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}


/*
 * 重放 GiST 索引页上的删除，以移除在索引元组插入期间
 * 标记为 DEAD 的元组
 */
static void
gistRedoDeleteRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogDelete *xldata = (gistxlogDelete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber *toDelete = xldata->offsets;

	/*
	 * 若有任何冲突处理要做，必须在我们更新页之前发生。
	 *
	 * GiST 删除记录可能与备库查询冲突。你也许会认为清理
	 * 记录也会冲突，但我们已经处理过了。XLOG_HEAP2_PRUNE
	 * 记录提供了堆清理所清除的最高 xid，因此当它到达时我们
	 * 可以只解决一次冲突。此后我们就知道该索引上的各条
	 * gist 清理记录不再存在冲突。
	  *
	  *
	 */
	if (InHotStandby)
	{
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xldata->snapshotConflictHorizon,
											xldata->isCatalogRel,
											rlocator);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		PageIndexMultiDelete(page, toDelete, xldata->ntodelete);

		GistClearPageHasGarbage(page);
		GistMarkTuplesDeleted(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * 返回索引指针数组。
 */
static IndexTuple *
decodePageSplitRecord(char *begin, int len, int *n)
{
	char	   *ptr;
	int			i = 0;
	IndexTuple *tuples;

	/* 提取元组个数 */
	memcpy(n, begin, sizeof(int));
	ptr = begin + sizeof(int);

	tuples = palloc(*n * sizeof(IndexTuple));

	for (i = 0; i < *n; i++)
	{
		Assert(ptr - begin < len);
		tuples[i] = (IndexTuple) ptr;
		ptr += IndexTupleSize((IndexTuple) ptr);
	}
	Assert(ptr - begin == len);

	return tuples;
}

static void
gistRedoPageSplitRecord(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageSplit *xldata = (gistxlogPageSplit *) XLogRecGetData(record);
	Buffer		firstbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	int			i;
	bool		isrootsplit = false;

	/*
	 * 在整个动作期间，我们必须对列表中的第一页持锁，
	 * 包括在更新左子页（若有）时。列表中其余页一旦写完
	 * 即可解锁，因为并发查询若不先访问列表中的第一页，就
	 * 没有路径到达那些页。
	  *
	 */

	/* 遍历所有页 */
	for (i = 0; i < xldata->npage; i++)
	{
		int			flags;
		char	   *data;
		Size		datalen;
		int			num;
		BlockNumber blkno;
		IndexTuple *tuples;

		XLogRecGetBlockTag(record, i + 1, NULL, NULL, &blkno);
		if (blkno == GIST_ROOT_BLKNO)
		{
			Assert(i == 0);
			isrootsplit = true;
		}

		buffer = XLogInitBufferForRedo(record, i + 1);
		page = (Page) BufferGetPage(buffer);
		data = XLogRecGetBlockData(record, i + 1, &datalen);

		tuples = decodePageSplitRecord(data, datalen, &num);

		/* 好，清空缓冲区 */
		if (xldata->origleaf && blkno != GIST_ROOT_BLKNO)
			flags = F_LEAF;
		else
			flags = 0;
		GISTInitBuffer(buffer, flags);

		/* 然后填充它 */
		gistfillbuffer(page, tuples, num, FirstOffsetNumber);

		if (blkno == GIST_ROOT_BLKNO)
		{
			GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
			GistPageSetNSN(page, xldata->orignsn);
			GistClearFollowRight(page);
		}
		else
		{
			if (i < xldata->npage - 1)
			{
				BlockNumber nextblkno;

				XLogRecGetBlockTag(record, i + 2, NULL, NULL, &nextblkno);
				GistPageGetOpaque(page)->rightlink = nextblkno;
			}
			else
				GistPageGetOpaque(page)->rightlink = xldata->origrlink;
			GistPageSetNSN(page, xldata->orignsn);
			if (i < xldata->npage - 1 && !isrootsplit &&
				xldata->markfollowright)
				GistMarkFollowRight(page);
			else
				GistClearFollowRight(page);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);

		if (i == 0)
			firstbuffer = buffer;
		else
			UnlockReleaseBuffer(buffer);
	}

	/* 若有，修复左子页上的 follow-right 数据 */
	if (XLogRecHasBlockRef(record, 0))
		gistRedoClearFollowRight(record, 0);

	/* 最后，释放第一页上的锁 */
	UnlockReleaseBuffer(firstbuffer);
}

/* 重放页删除 */
static void
gistRedoPageDelete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageDelete *xldata = (gistxlogPageDelete *) XLogRecGetData(record);
	Buffer		parentBuffer;
	Buffer		leafBuffer;

	if (XLogReadBufferForRedo(record, 0, &leafBuffer) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(leafBuffer);

		GistPageSetDeleted(page, xldata->deleteXid);

		PageSetLSN(page, lsn);
		MarkBufferDirty(leafBuffer);
	}

	if (XLogReadBufferForRedo(record, 1, &parentBuffer) == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(parentBuffer);

		PageIndexTupleDelete(page, xldata->downlinkOffset);

		PageSetLSN(page, lsn);
		MarkBufferDirty(parentBuffer);
	}

	if (BufferIsValid(parentBuffer))
		UnlockReleaseBuffer(parentBuffer);
	if (BufferIsValid(leafBuffer))
		UnlockReleaseBuffer(leafBuffer);
}

static void
gistRedoPageReuse(XLogReaderState *record)
{
	gistxlogPageReuse *xlrec = (gistxlogPageReuse *) XLogRecGetData(record);

	/*
	 * PAGE_REUSE 记录的存在是为了在我们通过 FSM 复用索引中的页
	 * 时提供一个冲突点。不过这就是它们的全部作用。
	 *
	 * snapshotConflictHorizon 曾是该页的 deleteXid。gistPageRecyclable()
	 * 中的 GlobalVisCheckRemovableFullXid(deleteXid) 测试在概念上镜像
	 * GetConflictingVirtualXIDs() 中的 PGPROC->xmin > limitXmin 测试。
	 * 因此，同一个 XID 值在主库与备库上达到相同的排斥效果。
	  *
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshotFullXid(xlrec->snapshotConflictHorizon,
												   xlrec->isCatalogRel,
												   xlrec->locator);
}

void
gist_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCxt;

	/*
	 * GiST 索引不需要任何冲突处理。注意：若我们将来实现与
	 * b-tree 类似的优化，并在 VACUUM 之外移除已杀死的元组，
	 * 则需要在此处处理。
	 */

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			gistRedoPageUpdateRecord(record);
			break;
		case XLOG_GIST_DELETE:
			gistRedoDeleteRecord(record);
			break;
		case XLOG_GIST_PAGE_REUSE:
			gistRedoPageReuse(record);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(record);
			break;
		case XLOG_GIST_PAGE_DELETE:
			gistRedoPageDelete(record);
			break;
		case XLOG_GIST_ASSIGN_LSN:
			/* 空操作。见 gistGetFakeLSN()。 */
			break;
		default:
			elog(PANIC, "gist_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

void
gist_xlog_startup(void)
{
	opCtx = createTempGistContext();
}

void
gist_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
}

/*
 * 在对 Gist 页运行一致性检查之前对其进行掩码。
 */
void
gist_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	/*
	 * NSN 不过是一种特殊用途的 LSN。因此，出于与
	 * mask_page_lsn_and_checksum 相同的原因对其进行掩码。
	 */
	GistPageSetNSN(page, (uint64) MASK_MARKER);

	/*
	 * 我们在写入 WAL 记录之后更新左子页上的 F_FOLLOW_RIGHT
	 * 标志。因此掩码该标志。细节见 gistplacetopage()。
	 */
	GistMarkFollowRight(page);

	if (GistPageIsLeaf(page))
	{
		/*
		 * 在 gist 叶子页上，可以在不发出任何 WAL 记录的情况下
		 * 修改 LP_FLAGS。因此掩码行指针标志。细节见
		 * gistkillitems()。
		 */
		mask_lp_flags(page);
	}

	/*
	 * 在 gist 重放期间，我们从不把页标记为含垃圾。因此掩码它以
	 * 忽略任何差异。
	 */
	GistClearPageHasGarbage(page);
}

/*
 * 写入页分裂的 WAL 记录。
 */
XLogRecPtr
gistXLogSplit(bool page_is_leaf,
			  SplitedPageLayout *dist,
			  BlockNumber origrlink, GistNSN orignsn,
			  Buffer leftchildbuf, bool markfollowright)
{
	gistxlogPageSplit xlrec;
	SplitedPageLayout *ptr;
	int			npage = 0;
	XLogRecPtr	recptr;
	int			i;

	for (ptr = dist; ptr; ptr = ptr->next)
		npage++;

	xlrec.origrlink = origrlink;
	xlrec.orignsn = orignsn;
	xlrec.origleaf = page_is_leaf;
	xlrec.npage = (uint16) npage;
	xlrec.markfollowright = markfollowright;

	XLogBeginInsert();

	/*
	 * 包含子缓冲区的整页映像。（仅当自子页分裂以来发生过
	 * 检查点时才有必要）
	 */
	if (BufferIsValid(leftchildbuf))
		XLogRegisterBuffer(0, leftchildbuf, REGBUF_STANDARD);

	/*
	 * 注意：我们注册了大量数据。调用者必须已调用
	 * XLogEnsureRecordSpace() 为此做好准备。我们无法在此处做，
	 * 因为我们已经处于临界区中。若你更改此处注册的缓冲区或
	 * 数据数量，请确保相应地修改 XLogEnsureRecordSpace() 调用！
	  *
	 */
	XLogRegisterData((char *) &xlrec, sizeof(gistxlogPageSplit));

	i = 1;
	for (ptr = dist; ptr; ptr = ptr->next)
	{
		XLogRegisterBuffer(i, ptr->buffer, REGBUF_WILL_INIT);
		XLogRegisterBufData(i, (char *) &(ptr->block.num), sizeof(int));
		XLogRegisterBufData(i, (char *) ptr->list, ptr->lenlist);
		i++;
	}

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT);

	return recptr;
}

/*
 * 写入描述页删除的 XLOG 记录。这也包括从父页移除
 * 下行指针。
 */
XLogRecPtr
gistXLogPageDelete(Buffer buffer, FullTransactionId xid,
				   Buffer parentBuffer, OffsetNumber downlinkOffset)
{
	gistxlogPageDelete xlrec;
	XLogRecPtr	recptr;

	xlrec.deleteXid = xid;
	xlrec.downlinkOffset = downlinkOffset;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfGistxlogPageDelete);

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBuffer(1, parentBuffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_DELETE);

	return recptr;
}

/*
 * 写入一条空 XLOG 记录以分配一个不同的 LSN。
 */
XLogRecPtr
gistXLogAssignLSN(void)
{
	int			dummy = 0;

	/*
	 * 除 XLOG_SWITCH 外的记录必须有内容。我们使用整数 0
	 * 以满足该限制。
	 */
	XLogBeginInsert();
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	XLogRegisterData((char *) &dummy, sizeof(dummy));
	return XLogInsert(RM_GIST_ID, XLOG_GIST_ASSIGN_LSN);
}

/*
 * 写入关于复用已删除页的 XLOG 记录。
 */
void
gistXLogPageReuse(Relation rel, Relation heaprel,
				  BlockNumber blkno, FullTransactionId deleteXid)
{
	gistxlogPageReuse xlrec_reuse;

	/*
	 * 注意我们没有向记录注册缓冲区，因为此操作并不修改
	 * 页。该记录的存在只是为 Hot Standby 提供一个冲突点。
	  *
	 */

	/* XLOG 相关 */
	xlrec_reuse.isCatalogRel = RelationIsAccessibleInLogicalDecoding(heaprel);
	xlrec_reuse.locator = rel->rd_locator;
	xlrec_reuse.block = blkno;
	xlrec_reuse.snapshotConflictHorizon = deleteXid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec_reuse, SizeOfGistxlogPageReuse);

	XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_REUSE);
}

/*
 * 写入描述页更新的 XLOG 记录。更新可以包括在单个索引页上
 * 任意数量的元组删除和/或插入。
 *
 * 若此更新为分裂页插入下行指针，还要记录子页上的
 * F_FOLLOW_RIGHT 标志被清除以及 NSN 被设置。
 *
 * 注意 todelete 数组与元组都被标记为属于目标缓冲区；若
 * XLogInsert 决定改为记录整个缓冲区内容，则不必把它们存入
 * XLOG。
 */
XLogRecPtr
gistXLogUpdate(Buffer buffer,
			   OffsetNumber *todelete, int ntodelete,
			   IndexTuple *itup, int ituplen,
			   Buffer leftchildbuf)
{
	gistxlogPageUpdate xlrec;
	int			i;
	XLogRecPtr	recptr;

	xlrec.ntodelete = ntodelete;
	xlrec.ntoinsert = ituplen;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(gistxlogPageUpdate));

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) todelete, sizeof(OffsetNumber) * ntodelete);

	/* 新元组 */
	for (i = 0; i < ituplen; i++)
		XLogRegisterBufData(0, (char *) (itup[i]), IndexTupleSize(itup[i]));

	/*
	 * 包含子缓冲区的整页映像。（仅当自子页分裂以来发生过
	 * 检查点时才有必要）
	 */
	if (BufferIsValid(leftchildbuf))
		XLogRegisterBuffer(1, leftchildbuf, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE);

	return recptr;
}

/*
 * 写入描述删除在新元组插入期间标记为 DEAD 的叶子索引元组的
 * XLOG 记录。或许有人认为 gistXLogUpdate() 已覆盖此情形。但
 * 删除索引元组可能与备库查询冲突，需要特殊处理。
  *
 */
XLogRecPtr
gistXLogDelete(Buffer buffer, OffsetNumber *todelete, int ntodelete,
			   TransactionId snapshotConflictHorizon, Relation heaprel)
{
	gistxlogDelete xlrec;
	XLogRecPtr	recptr;

	xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(heaprel);
	xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
	xlrec.ntodelete = ntodelete;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfGistxlogDelete);

	/*
	 * 无论是否存储整个缓冲区，我们都需要目标偏移量数组，
	 * 以便在备库上找到 snapshotConflictHorizon。
	  *
	 */
	XLogRegisterData((char *) todelete, ntodelete * sizeof(OffsetNumber));

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_DELETE);

	return recptr;
}
