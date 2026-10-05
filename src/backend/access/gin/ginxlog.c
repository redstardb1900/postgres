/*-------------------------------------------------------------------------
 *
 * ginxlog.c
 *	  倒排索引的 WAL 重放逻辑。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 src/backend/access/gin/ginxlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xlogutils.h"
#include "utils/memutils.h"

static MemoryContext opCtx;		/* 操作用的工作内存 */

static void
ginRedoClearIncompleteSplit(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, block_id, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);
		GinPageGetOpaque(page)->flags &= ~GIN_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoCreatePTree(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogCreatePostingTree *data = (ginxlogCreatePostingTree *) XLogRecGetData(record);
	char	   *ptr;
	Buffer		buffer;
	Page		page;

	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_DATA | GIN_LEAF | GIN_COMPRESSED);

	ptr = XLogRecGetData(record) + sizeof(ginxlogCreatePostingTree);

	/* 放入页数据 */
	memcpy(GinDataLeafPageGetPostingList(page), ptr, data->size);

	GinDataPageSetDataSize(page, data->size);

	PageSetLSN(page, lsn);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoInsertEntry(Buffer buffer, bool isLeaf, BlockNumber rightblkno, void *rdata)
{
	Page		page = BufferGetPage(buffer);
	ginxlogInsertEntry *data = (ginxlogInsertEntry *) rdata;
	OffsetNumber offset = data->offset;
	IndexTuple	itup;

	if (rightblkno != InvalidBlockNumber)
	{
		/* 分裂后更新指向右页的链接 */
		Assert(!GinPageIsLeaf(page));
		Assert(offset >= FirstOffsetNumber && offset <= PageGetMaxOffsetNumber(page));
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offset));
		GinSetDownlink(itup, rightblkno);
	}

	if (data->isDelete)
	{
		Assert(GinPageIsLeaf(page));
		Assert(offset >= FirstOffsetNumber && offset <= PageGetMaxOffsetNumber(page));
		PageIndexTupleDelete(page, offset);
	}

	itup = &data->tuple;

	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), offset, false, false) == InvalidOffsetNumber)
	{
		RelFileLocator locator;
		ForkNumber	forknum;
		BlockNumber blknum;

		BufferGetTag(buffer, &locator, &forknum, &blknum);
		elog(ERROR, "failed to add item to index page in %u/%u/%u",
			 locator.spcOid, locator.dbOid, locator.relNumber);
	}
}

/*
 * 重放倒排列表的重压缩。并非总是能就地完成全部修改，
 * 因为它需要的空间可能超过页上已有的空间。
 * 因此，一旦需要修改，就把页上尚未处理的尾部
 * 复制到单独分配的内存块中，以便继续读取各段的
 * 原始版本。这样一来，就不必操心就地移动
 * 页数据。
 */
static void
ginRedoRecompress(Page page, ginxlogRecompressDataLeaf *data)
{
	int			actionno;
	int			segno;
	GinPostingList *oldseg;
	Pointer		segmentend;
	char	   *walbuf;
	int			totalsize;
	Pointer		tailCopy = NULL;
	Pointer		writePtr;
	Pointer		segptr;

	/*
	 * 若页是 9.4 之前的格式，先转换成新格式。
	 */
	if (!GinPageIsCompressed(page))
	{
		ItemPointer uncompressed = (ItemPointer) GinDataPageGetData(page);
		int			nuncompressed = GinPageGetOpaque(page)->maxoff;
		int			npacked;

		/*
		 * 空叶子页会作为清理的一部分被删除，但最左页和
		 * 最右页从不删除。因此，经 pg_upgrade 从 9.4 之前
		 * 升级来的实例可能含有空叶子页，我们需要正确
		 * 处理它们。
		 */
		if (nuncompressed > 0)
		{
			GinPostingList *plist;

			plist = ginCompressPostingList(uncompressed, nuncompressed,
										   BLCKSZ, &npacked);
			totalsize = SizeOfGinPostingList(plist);

			Assert(npacked == nuncompressed);

			memcpy(GinDataLeafPageGetPostingList(page), plist, totalsize);
		}
		else
		{
			totalsize = 0;
		}

		GinDataPageSetDataSize(page, totalsize);
		GinPageSetCompressed(page);
		GinPageGetOpaque(page)->maxoff = InvalidOffsetNumber;
	}

	oldseg = GinDataLeafPageGetPostingList(page);
	writePtr = (Pointer) oldseg;
	segmentend = (Pointer) oldseg + GinDataLeafPageGetPostingListSize(page);
	segno = 0;

	walbuf = ((char *) data) + sizeof(ginxlogRecompressDataLeaf);
	for (actionno = 0; actionno < data->nactions; actionno++)
	{
		uint8		a_segno = *((uint8 *) (walbuf++));
		uint8		a_action = *((uint8 *) (walbuf++));
		GinPostingList *newseg = NULL;
		int			newsegsize = 0;
		ItemPointerData *items = NULL;
		uint16		nitems = 0;
		ItemPointerData *olditems;
		int			nolditems;
		ItemPointerData *newitems;
		int			nnewitems;
		int			segsize;

		/* 从 WAL 记录中取出我们需要的全部信息 */
		if (a_action == GIN_SEGMENT_INSERT ||
			a_action == GIN_SEGMENT_REPLACE)
		{
			newseg = (GinPostingList *) walbuf;
			newsegsize = SizeOfGinPostingList(newseg);
			walbuf += SHORTALIGN(newsegsize);
		}

		if (a_action == GIN_SEGMENT_ADDITEMS)
		{
			memcpy(&nitems, walbuf, sizeof(uint16));
			walbuf += sizeof(uint16);
			items = (ItemPointerData *) walbuf;
			walbuf += nitems * sizeof(ItemPointerData);
		}

		/* 跳到本动作所涉及的段 */
		Assert(segno <= a_segno);
		while (segno < a_segno)
		{
			/*
			 * 一旦开始修改并已复制页尾，就必须
			 * 复制未修改的段。
			 */
			segsize = SizeOfGinPostingList(oldseg);
			if (tailCopy)
			{
				Assert(writePtr + segsize < PageGetSpecialPointer(page));
				memcpy(writePtr, (Pointer) oldseg, segsize);
			}
			writePtr += segsize;
			oldseg = GinNextPostingListSegment(oldseg);
			segno++;
		}

		/*
		 * ADDITEMS 动作的处理类似 REPLACE，但用来替换旧段的
		 * 新段要由磁盘上的旧段与 WAL 记录中的新项
		 * 重新构造。
		 */
		if (a_action == GIN_SEGMENT_ADDITEMS)
		{
			int			npacked;

			olditems = ginPostingListDecode(oldseg, &nolditems);

			newitems = ginMergeItemPointers(items, nitems,
											olditems, nolditems,
											&nnewitems);
			Assert(nnewitems == nolditems + nitems);

			newseg = ginCompressPostingList(newitems, nnewitems,
											BLCKSZ, &npacked);
			Assert(npacked == nnewitems);

			newsegsize = SizeOfGinPostingList(newseg);
			a_action = GIN_SEGMENT_REPLACE;
		}

		segptr = (Pointer) oldseg;
		if (segptr != segmentend)
			segsize = SizeOfGinPostingList(oldseg);
		else
		{
			/*
			 * 定位在最后一个已有段之后。这里只应出现
			 * INSERTs。
			 */
			Assert(a_action == GIN_SEGMENT_INSERT);
			segsize = 0;
		}

		/*
		 * 即将开始修改该页。因此，若尚未复制页尾，
		 * 先把页尾复制出来。
		 */
		if (!tailCopy && segptr != segmentend)
		{
			int			tailSize = segmentend - segptr;

			tailCopy = (Pointer) palloc(tailSize);
			memcpy(tailCopy, segptr, tailSize);
			segptr = tailCopy;
			oldseg = (GinPostingList *) segptr;
			segmentend = segptr + tailSize;
		}

		switch (a_action)
		{
			case GIN_SEGMENT_DELETE:
				segptr += segsize;
				segno++;
				break;

			case GIN_SEGMENT_INSERT:
				/* 把新段就地复制 */
				Assert(writePtr + newsegsize <= PageGetSpecialPointer(page));
				memcpy(writePtr, newseg, newsegsize);
				writePtr += newsegsize;
				break;

			case GIN_SEGMENT_REPLACE:
				/* 把该段的新版本就地复制 */
				Assert(writePtr + newsegsize <= PageGetSpecialPointer(page));
				memcpy(writePtr, newseg, newsegsize);
				writePtr += newsegsize;
				segptr += segsize;
				segno++;
				break;

			default:
				elog(ERROR, "unexpected GIN leaf action: %u", a_action);
		}
		oldseg = (GinPostingList *) segptr;
	}

	/* 若还有未修改的段，则复制其余部分。 */
	segptr = (Pointer) oldseg;
	if (segptr != segmentend && tailCopy)
	{
		int			restSize = segmentend - segptr;

		Assert(writePtr + restSize <= PageGetSpecialPointer(page));
		memcpy(writePtr, segptr, restSize);
		writePtr += restSize;
	}

	totalsize = writePtr - (Pointer) GinDataLeafPageGetPostingList(page);
	GinDataPageSetDataSize(page, totalsize);
}

static void
ginRedoInsertData(Buffer buffer, bool isLeaf, BlockNumber rightblkno, void *rdata)
{
	Page		page = BufferGetPage(buffer);

	if (isLeaf)
	{
		ginxlogRecompressDataLeaf *data = (ginxlogRecompressDataLeaf *) rdata;

		Assert(GinPageIsLeaf(page));

		ginRedoRecompress(page, data);
	}
	else
	{
		ginxlogInsertDataInternal *data = (ginxlogInsertDataInternal *) rdata;
		PostingItem *oldpitem;

		Assert(!GinPageIsLeaf(page));

		/* 分裂后更新指向右页的链接 */
		oldpitem = GinDataPageGetPostingItem(page, data->offset);
		PostingItemSetBlockNumber(oldpitem, rightblkno);

		GinDataPageAddPostingItem(page, &data->newitem, data->offset);
	}
}

static void
ginRedoInsert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogInsert *data = (ginxlogInsert *) XLogRecGetData(record);
	Buffer		buffer;
#ifdef NOT_USED
	BlockNumber leftChildBlkno = InvalidBlockNumber;
#endif
	BlockNumber rightChildBlkno = InvalidBlockNumber;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;

	/*
	 * 若本次完成一次分裂，先清除子页上的未完成分裂
	 * 标志。
	 */
	if (!isLeaf)
	{
		char	   *payload = XLogRecGetData(record) + sizeof(ginxlogInsert);

#ifdef NOT_USED
		leftChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
#endif
		payload += sizeof(BlockIdData);
		rightChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
		payload += sizeof(BlockIdData);

		ginRedoClearIncompleteSplit(record, 1);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		Size		len;
		char	   *payload = XLogRecGetBlockData(record, 0, &len);

		/* 如何插入有效载荷取决于树类型 */
		if (data->flags & GIN_INSERT_ISDATA)
		{
			Assert(GinPageIsData(page));
			ginRedoInsertData(buffer, isLeaf, rightChildBlkno, payload);
		}
		else
		{
			Assert(!GinPageIsData(page));
			ginRedoInsertEntry(buffer, isLeaf, rightChildBlkno, payload);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoSplit(XLogReaderState *record)
{
	ginxlogSplit *data = (ginxlogSplit *) XLogRecGetData(record);
	Buffer		lbuffer,
				rbuffer,
				rootbuf;
	bool		isLeaf = (data->flags & GIN_INSERT_ISLEAF) != 0;
	bool		isRoot = (data->flags & GIN_SPLIT_ROOT) != 0;

	/*
	 * 若本次完成一次分裂，先清除子页上的未完成分裂
	 * 标志
	 */
	if (!isLeaf)
		ginRedoClearIncompleteSplit(record, 3);

	if (XLogReadBufferForRedo(record, 0, &lbuffer) != BLK_RESTORED)
		elog(ERROR, "GIN split record did not contain a full-page image of left page");

	if (XLogReadBufferForRedo(record, 1, &rbuffer) != BLK_RESTORED)
		elog(ERROR, "GIN split record did not contain a full-page image of right page");

	if (isRoot)
	{
		if (XLogReadBufferForRedo(record, 2, &rootbuf) != BLK_RESTORED)
			elog(ERROR, "GIN split record did not contain a full-page image of root page");
		UnlockReleaseBuffer(rootbuf);
	}

	UnlockReleaseBuffer(rbuffer);
	UnlockReleaseBuffer(lbuffer);
}

/*
 * VACUUM_PAGE 记录只含该页的整页映像，类似于
 * 一条 XLOG_FPI 记录。
 */
static void
ginRedoVacuumPage(XLogReaderState *record)
{
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) != BLK_RESTORED)
	{
		elog(ERROR, "replay of gin entry tree page vacuum did not restore the page");
	}
	UnlockReleaseBuffer(buffer);
}

static void
ginRedoVacuumDataLeafPage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		Size		len;
		ginxlogVacuumDataLeafPage *xlrec;

		xlrec = (ginxlogVacuumDataLeafPage *) XLogRecGetBlockData(record, 0, &len);

		Assert(GinPageIsLeaf(page));
		Assert(GinPageIsData(page));

		ginRedoRecompress(page, &xlrec->data);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeletePage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogDeletePage *data = (ginxlogDeletePage *) XLogRecGetData(record);
	Buffer		dbuffer;
	Buffer		pbuffer;
	Buffer		lbuffer;
	Page		page;

	/*
	 * 先锁定左页，以免与 ginStepRight()
	 * 发生死锁。
	 */
	if (XLogReadBufferForRedo(record, 2, &lbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(lbuffer);
		Assert(GinPageIsData(page));
		GinPageGetOpaque(page)->rightlink = data->rightLink;
		PageSetLSN(page, lsn);
		MarkBufferDirty(lbuffer);
	}

	if (XLogReadBufferForRedo(record, 0, &dbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(dbuffer);
		Assert(GinPageIsData(page));
		GinPageSetDeleted(page);
		GinPageSetDeleteXid(page, data->deleteXid);
		PageSetLSN(page, lsn);
		MarkBufferDirty(dbuffer);
	}

	if (XLogReadBufferForRedo(record, 1, &pbuffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(pbuffer);
		Assert(GinPageIsData(page));
		Assert(!GinPageIsLeaf(page));
		GinPageDeletePostingItem(page, data->parentOffset);
		PageSetLSN(page, lsn);
		MarkBufferDirty(pbuffer);
	}

	if (BufferIsValid(lbuffer))
		UnlockReleaseBuffer(lbuffer);
	if (BufferIsValid(pbuffer))
		UnlockReleaseBuffer(pbuffer);
	if (BufferIsValid(dbuffer))
		UnlockReleaseBuffer(dbuffer);
}

static void
ginRedoUpdateMetapage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogUpdateMeta *data = (ginxlogUpdateMeta *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	Buffer		buffer;

	/*
	 * 恢复元页。这本质上与整页映像相同，
	 * 因此无条件恢复元页，而不查看
	 * LSN，以避免撕裂页风险。
	 */
	metabuffer = XLogInitBufferForRedo(record, 0);
	Assert(BufferGetBlockNumber(metabuffer) == GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	GinInitMetabuffer(metabuffer);
	memcpy(GinPageGetMeta(metapage), &data->metadata, sizeof(GinMetaPageData));
	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuffer);

	if (data->ntuples > 0)
	{
		/*
		 * 插入到尾页
		 */
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			Page		page = BufferGetPage(buffer);
			OffsetNumber off;
			int			i;
			Size		tupsize;
			char	   *payload;
			IndexTuple	tuples;
			Size		totaltupsize;

			payload = XLogRecGetBlockData(record, 1, &totaltupsize);
			tuples = (IndexTuple) payload;

			if (PageIsEmpty(page))
				off = FirstOffsetNumber;
			else
				off = OffsetNumberNext(PageGetMaxOffsetNumber(page));

			for (i = 0; i < data->ntuples; i++)
			{
				tupsize = IndexTupleSize(tuples);

				if (PageAddItem(page, (Item) tuples, tupsize, off,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to index page");

				tuples = (IndexTuple) (((char *) tuples) + tupsize);

				off++;
			}
			Assert(payload + totaltupsize == (char *) tuples);

			/*
			 * 增加堆元组计数
			 */
			GinPageGetOpaque(page)->maxoff++;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
	else if (data->prevTail != InvalidBlockNumber)
	{
		/*
		 * 新的尾页
		 */
		if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
		{
			Page		page = BufferGetPage(buffer);

			GinPageGetOpaque(page)->rightlink = data->newRightlink;

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}

	UnlockReleaseBuffer(metabuffer);
}

static void
ginRedoInsertListPage(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogInsertListPage *data = (ginxlogInsertListPage *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber l,
				off = FirstOffsetNumber;
	int			i,
				tupsize;
	char	   *payload;
	IndexTuple	tuples;
	Size		totaltupsize;

	/* 我们总是重新初始化该页。 */
	buffer = XLogInitBufferForRedo(record, 0);
	page = BufferGetPage(buffer);

	GinInitBuffer(buffer, GIN_LIST);
	GinPageGetOpaque(page)->rightlink = data->rightlink;
	if (data->rightlink == InvalidBlockNumber)
	{
		/* 子列表的尾部 */
		GinPageSetFullRow(page);
		GinPageGetOpaque(page)->maxoff = 1;
	}
	else
	{
		GinPageGetOpaque(page)->maxoff = 0;
	}

	payload = XLogRecGetBlockData(record, 0, &totaltupsize);

	tuples = (IndexTuple) payload;
	for (i = 0; i < data->ntuples; i++)
	{
		tupsize = IndexTupleSize(tuples);

		l = PageAddItem(page, (Item) tuples, tupsize, off, false, false);

		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page");

		tuples = (IndexTuple) (((char *) tuples) + tupsize);
		off++;
	}
	Assert((char *) tuples == payload + totaltupsize);

	PageSetLSN(page, lsn);
	MarkBufferDirty(buffer);

	UnlockReleaseBuffer(buffer);
}

static void
ginRedoDeleteListPages(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	ginxlogDeleteListPages *data = (ginxlogDeleteListPages *) XLogRecGetData(record);
	Buffer		metabuffer;
	Page		metapage;
	int			i;

	metabuffer = XLogInitBufferForRedo(record, 0);
	Assert(BufferGetBlockNumber(metabuffer) == GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	GinInitMetabuffer(metabuffer);

	memcpy(GinPageGetMeta(metapage), &data->metadata, sizeof(GinMetaPageData));
	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuffer);

	/*
	 * 正常操作中，shiftList() 同时对所有待删除
	 * 页取排他锁。不过重放期间，一次锁一页
	 * 应当没问题。这依赖于如下事实：我们从列表
	 * 头部删除页，且读者在释放当前页之前
	 * 会对下一页加共享锁。因此，正位于或即将访问
	 * 我们要删除的任一页的读者，我们都无法越过。新来的
	 * 读者会阻塞在我们的元页锁之后，然后看到
	 * 已完全更新的页列表。
	 *
	 * 不对被删除页做整页映像。它们会被
	 * 重新初始化为空的已删除页。其右链接不必
	 * 保留，因为没有新读者能看到这些页，原因如
	 * 上所述。
	 */
	for (i = 0; i < data->ndeleted; i++)
	{
		Buffer		buffer;
		Page		page;

		buffer = XLogInitBufferForRedo(record, i + 1);
		page = BufferGetPage(buffer);
		GinInitBuffer(buffer, GIN_DELETED);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);

		UnlockReleaseBuffer(buffer);
	}
	UnlockReleaseBuffer(metabuffer);
}

void
gin_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	MemoryContext oldCtx;

	/*
	 * GIN 索引不需要任何冲突处理。注意：若将来
	 * 实现与 B-树类似的优化，并在 VACUUM 之外移除
	 * 已杀死的元组，就需要在这里处理。
	 */

	oldCtx = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIN_CREATE_PTREE:
			ginRedoCreatePTree(record);
			break;
		case XLOG_GIN_INSERT:
			ginRedoInsert(record);
			break;
		case XLOG_GIN_SPLIT:
			ginRedoSplit(record);
			break;
		case XLOG_GIN_VACUUM_PAGE:
			ginRedoVacuumPage(record);
			break;
		case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
			ginRedoVacuumDataLeafPage(record);
			break;
		case XLOG_GIN_DELETE_PAGE:
			ginRedoDeletePage(record);
			break;
		case XLOG_GIN_UPDATE_META_PAGE:
			ginRedoUpdateMetapage(record);
			break;
		case XLOG_GIN_INSERT_LISTPAGE:
			ginRedoInsertListPage(record);
			break;
		case XLOG_GIN_DELETE_LISTPAGE:
			ginRedoDeleteListPages(record);
			break;
		default:
			elog(PANIC, "gin_redo: unknown op code %u", info);
	}
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(opCtx);
}

void
gin_xlog_startup(void)
{
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "GIN recovery temporary context",
								  ALLOCSET_DEFAULT_SIZES);
}

void
gin_xlog_cleanup(void)
{
	MemoryContextDelete(opCtx);
	opCtx = NULL;
}

/*
 * 在对 GIN 页做一致性检查之前先屏蔽它。
 */
void
gin_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	PageHeader	pagehdr = (PageHeader) page;
	GinPageOpaque opaque;

	mask_page_lsn_and_checksum(page);
	opaque = GinPageGetOpaque(page);

	mask_page_hint_bits(page);

	/*
	 * 对 GIN_DELETED 页，该页被初始化为空。因此屏蔽
	 * 整页内容。对其他页，若 pd_lower
	 * 看起来已被正确设置，则屏蔽空洞。
	 */
	if (opaque->flags & GIN_DELETED)
		mask_page_content(page);
	else if (pagehdr->pd_lower > SizeOfPageHeaderData)
		mask_unused_space(page);
}
