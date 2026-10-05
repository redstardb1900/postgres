/*-------------------------------------------------------------------------
 *
 * ginentrypage.c
 *	  处理 GIN 条目树页的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginentrypage.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"

static void entrySplitPage(GinBtree btree, Buffer origbuf,
						   GinBtreeStack *stack,
						   GinBtreeEntryInsertData *insertData,
						   BlockNumber updateblkno,
						   Page *newlpage, Page *newrpage);

/*
 * 为条目树构造元组。
 *
 * 若元组太大而无法存储，则 errorTooBig 为 true 时函数报出合适的
 * 错误，errorTooBig 为 false 时返回 NULL。
 *
 * 此处构建的索引元组格式见 src/backend/access/gin/README。
 * 我们假定正在构造一条叶子层键条目，其中含有
 * nipd 个项的倒排列表。
 * 若调用者实际要构造倒排树条目、非叶子条目或
 * 挂起列表条目，则应传入 dataSize = 0，再按需要覆盖
 * t_tid 字段。无论哪种情况，'data' 都可以为 NULL，以跳过
 * 填入倒排列表；若 data = NULL 且 nipd > 0，则由调用者负责
 * 随后填入。
 */
IndexTuple
GinFormTuple(GinState *ginstate,
			 OffsetNumber attnum, Datum key, GinNullCategory category,
			 Pointer data, Size dataSize, int nipd,
			 bool errorTooBig)
{
	Datum		datums[2];
	bool		isnull[2];
	IndexTuple	itup;
	uint32		newsize;

	/* 构建基本元组：可选列号，加上键 datum */
	if (ginstate->oneCol)
	{
		datums[0] = key;
		isnull[0] = (category != GIN_CAT_NORM_KEY);
	}
	else
	{
		datums[0] = UInt16GetDatum(attnum);
		isnull[0] = false;
		datums[1] = key;
		isnull[1] = (category != GIN_CAT_NORM_KEY);
	}

	itup = index_form_tuple(ginstate->tupdesc[attnum - 1], datums, isnull);

	/*
	 * 确定并保存倒排列表的偏移，确保需要时
	 * 留有类别字节的空间。
	 *
	 * 注意：因为 index_form_tuple 会对元组大小做 MAXALIGNs，很可能
	 * 存在一些浪费的填充空间。值得重新计算数据长度来
	 * 避免这一点吗？那样也让我们能 Assert 真实数据
	 * 不会与 GinNullCategory 字节重叠，而这段代码目前
	 * 对此只是想当然。
	 */
	newsize = IndexTupleSize(itup);

	if (IndexTupleHasNulls(itup))
	{
		uint32		minsize;

		Assert(category != GIN_CAT_NORM_KEY);
		minsize = GinCategoryOffset(itup, ginstate) + sizeof(GinNullCategory);
		newsize = Max(newsize, minsize);
	}

	newsize = SHORTALIGN(newsize);

	GinSetPostingOffset(itup, newsize);
	GinSetNPosting(itup, nipd);

	/*
	 * 若有倒排列表，则为它加上所需空间。然后检查元组
	 * 不会大到无法存储。
	 */
	newsize += dataSize;

	newsize = MAXALIGN(newsize);

	if (newsize > GinMaxItemSize)
	{
		if (errorTooBig)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
							(Size) newsize, (Size) GinMaxItemSize,
							RelationGetRelationName(ginstate->index))));
		pfree(itup);
		return NULL;
	}

	/*
	 * 需要时调整元组大小
	 */
	if (newsize != IndexTupleSize(itup))
	{
		itup = repalloc(itup, newsize);

		/*
		 * PostgreSQL 9.3 及更早版本没有清零这块新空间，因此
		 * 从磁盘读元组时可能会遇到未初始化的填充。
		 */
		memset((char *) itup + IndexTupleSize(itup),
			   0, newsize - IndexTupleSize(itup));
		/* 在元组头中设置新大小 */
		itup->t_info &= ~INDEX_SIZE_MASK;
		itup->t_info |= newsize;
	}

	/*
	 * 若提供了倒排列表，则把它复制进去
	 */
	if (data)
	{
		char	   *ptr = GinGetPosting(itup);

		memcpy(ptr, data, dataSize);
	}

	/*
	 * 需要时插入类别字节
	 */
	if (category != GIN_CAT_NORM_KEY)
	{
		Assert(IndexTupleHasNulls(itup));
		GinSetNullCategory(itup, ginstate, category);
	}
	return itup;
}

/*
 * 从叶子条目元组中读取项指针。
 *
 * 返回经 palloc 分配的 ItemPointers 数组。项的个数
 * 经 *nitems 返回。
 */
ItemPointer
ginReadTuple(GinState *ginstate, OffsetNumber attnum, IndexTuple itup,
			 int *nitems)
{
	Pointer		ptr = GinGetPosting(itup);
	int			nipd = GinGetNPosting(itup);
	ItemPointer ipd;
	int			ndecoded;

	if (GinItupIsCompressed(itup))
	{
		if (nipd > 0)
		{
			ipd = ginPostingListDecode((GinPostingList *) ptr, &ndecoded);
			if (nipd != ndecoded)
				elog(ERROR, "number of items mismatch in GIN entry tuple, %d in tuple header, %d decoded",
					 nipd, ndecoded);
		}
		else
		{
			ipd = palloc(0);
		}
	}
	else
	{
		ipd = (ItemPointer) palloc(sizeof(ItemPointerData) * nipd);
		memcpy(ipd, ptr, sizeof(ItemPointerData) * nipd);
	}
	*nitems = nipd;
	return ipd;
}

/*
 * 从给定元组复制键数据，构造一条非叶子条目元组；
 * 给定元组可以是叶子或非叶子条目元组。
 *
 * 源元组中的倒排列表不会被复制。指定的
 * 子块号插入 t_tid。
 */
static IndexTuple
GinFormInteriorTuple(IndexTuple itup, Page page, BlockNumber childblk)
{
	IndexTuple	nitup;

	if (GinPageIsLeaf(page) && !GinIsPostingTree(itup))
	{
		/* 元组含有倒排列表，只复制它之前的内容 */
		uint32		origsize = GinGetPostingOffset(itup);

		origsize = MAXALIGN(origsize);
		nitup = (IndexTuple) palloc(origsize);
		memcpy(nitup, itup, origsize);
		/* ... 务必修正头中的大小字段 ... */
		nitup->t_info &= ~INDEX_SIZE_MASK;
		nitup->t_info |= origsize;
	}
	else
	{
		/* 原样复制该元组 */
		nitup = (IndexTuple) palloc(IndexTupleSize(itup));
		memcpy(nitup, itup, IndexTupleSize(itup));
	}

	/* 现在插入正确的下行指针 */
	GinSetDownlink(nitup, childblk);

	return nitup;
}

/*
 * 条目树是"静态"的，即从不删除其中的元组，
 * 因此不用右边界，而改用最右键。
 */
static IndexTuple
getRightMostTuple(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	return (IndexTuple) PageGetItem(page, PageGetItemId(page, maxoff));
}

static bool
entryIsMoveRight(GinBtree btree, Page page)
{
	IndexTuple	itup;
	OffsetNumber attnum;
	Datum		key;
	GinNullCategory category;

	if (GinPageRightMost(page))
		return false;

	itup = getRightMostTuple(page);
	attnum = gintuple_get_attrnum(btree->ginstate, itup);
	key = gintuple_get_key(btree->ginstate, itup, &category);

	if (ginCompareAttEntries(btree->ginstate,
							 btree->entryAttnum, btree->entryKey, btree->entryCategory,
							 attnum, key, category) > 0)
		return true;

	return false;
}

/*
 * 在非叶子页中找到正确的元组。假定
 * 页已正确选定，且要搜索的值 SHOULD 在该页上
 */
static BlockNumber
entryLocateEntry(GinBtree btree, GinBtreeStack *stack)
{
	OffsetNumber low,
				high,
				maxoff;
	IndexTuple	itup = NULL;
	int			result;
	Page		page = BufferGetPage(stack->buffer);

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		stack->predictNumber *= PageGetMaxOffsetNumber(page);
		return btree->getLeftMostChild(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = PageGetMaxOffsetNumber(page);
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		if (mid == maxoff && GinPageRightMost(page))
		{
			/* 右无穷 */
			result = -1;
		}
		else
		{
			OffsetNumber attnum;
			Datum		key;
			GinNullCategory category;

			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
			attnum = gintuple_get_attrnum(btree->ginstate, itup);
			key = gintuple_get_key(btree->ginstate, itup, &category);
			result = ginCompareAttEntries(btree->ginstate,
										  btree->entryAttnum,
										  btree->entryKey,
										  btree->entryCategory,
										  attnum, key, category);
		}

		if (result == 0)
		{
			stack->off = mid;
			Assert(GinGetDownlink(itup) != GIN_ROOT_BLKNO);
			return GinGetDownlink(itup);
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	Assert(high >= FirstOffsetNumber && high <= maxoff);

	stack->off = high;
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, high));
	Assert(GinGetDownlink(itup) != GIN_ROOT_BLKNO);
	return GinGetDownlink(itup);
}

/*
 * 在叶子页上搜索该值的正确位置。
 * 页应当已被正确选定。
 * 若在页上找到该值则返回 true。
 */
static bool
entryLocateLeafEntry(GinBtree btree, GinBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer);
	OffsetNumber low,
				high;

	Assert(GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		return true;
	}

	low = FirstOffsetNumber;
	high = PageGetMaxOffsetNumber(page);

	if (high < low)
	{
		stack->off = FirstOffsetNumber;
		return false;
	}

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);
		IndexTuple	itup;
		OffsetNumber attnum;
		Datum		key;
		GinNullCategory category;
		int			result;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
		attnum = gintuple_get_attrnum(btree->ginstate, itup);
		key = gintuple_get_key(btree->ginstate, itup, &category);
		result = ginCompareAttEntries(btree->ginstate,
									  btree->entryAttnum,
									  btree->entryKey,
									  btree->entryCategory,
									  attnum, key, category);
		if (result == 0)
		{
			stack->off = mid;
			return true;
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	stack->off = high;
	return false;
}

static OffsetNumber
entryFindChildPtr(GinBtree btree, Page page, BlockNumber blkno, OffsetNumber storedOff)
{
	OffsetNumber i,
				maxoff = PageGetMaxOffsetNumber(page);
	IndexTuple	itup;

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));

	/* 若页未改变，则返回 storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, storedOff));
		if (GinGetDownlink(itup) == blkno)
			return storedOff;

		/*
		 * 我们希望所需指针在更右侧。若未发生
		 * 删除，这一点成立
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
			if (GinGetDownlink(itup) == blkno)
				return i;
		}
		maxoff = storedOff - 1;
	}

	/* 最后机会 */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		if (GinGetDownlink(itup) == blkno)
			return i;
	}

	return InvalidOffsetNumber;
}

static BlockNumber
entryGetLeftMostPage(GinBtree btree, Page page)
{
	IndexTuple	itup;

	Assert(!GinPageIsLeaf(page));
	Assert(!GinPageIsData(page));
	Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
	return GinGetDownlink(itup);
}

static bool
entryIsEnoughSpace(GinBtree btree, Buffer buf, OffsetNumber off,
				   GinBtreeEntryInsertData *insertData)
{
	Size		releasedsz = 0;
	Size		addedsz;
	Page		page = BufferGetPage(buf);

	Assert(insertData->entry);
	Assert(!GinPageIsData(page));

	if (insertData->isDelete)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		releasedsz = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	}

	addedsz = MAXALIGN(IndexTupleSize(insertData->entry)) + sizeof(ItemIdData);

	if (PageGetFreeSpace(page) + releasedsz >= addedsz)
		return true;

	return false;
}

/*
 * 若叶子页上元组已存在且我们
 * 应当更新它，则删除该元组；若子页发生了分裂，
 * 则把旧的子块号更新为指向新右页
 */
static void
entryPreparePage(GinBtree btree, Page page, OffsetNumber off,
				 GinBtreeEntryInsertData *insertData, BlockNumber updateblkno)
{
	Assert(insertData->entry);
	Assert(!GinPageIsData(page));

	if (insertData->isDelete)
	{
		Assert(GinPageIsLeaf(page));
		PageIndexTupleDelete(page, off);
	}

	if (!GinPageIsLeaf(page) && updateblkno != InvalidBlockNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		GinSetDownlink(itup, updateblkno);
	}
}

/*
 * 准备向条目页插入数据。
 *
 * 若放得下，则在进入插入临界区之前做完所需的准备，
 * 然后返回 GPTP_INSERT。可以设置 *ptp_workspace，
 * 以便把信息传给 execPlaceToPage 函数。
 *
 * 若放不下，则执行页分裂，并把两张临时页
 * 映像返回到 *newlpage 和 *newrpage，结果为 GPTP_SPLIT。
 *
 * 两种情况下都不应在此处修改给定的页缓冲区。
 *
 * 注意：向内部节点插入时，除了插入给定
 * 项之外，stack->off 处现有项的下行指针也会被更新为
 * 指向 updateblkno。
 */
static GinPlaceToPageRC
entryBeginPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					  void *insertPayload, BlockNumber updateblkno,
					  void **ptp_workspace,
					  Page *newlpage, Page *newrpage)
{
	GinBtreeEntryInsertData *insertData = insertPayload;
	OffsetNumber off = stack->off;

	/* 若放不下，则处理分裂情形 */
	if (!entryIsEnoughSpace(btree, buf, off, insertData))
	{
		entrySplitPage(btree, buf, stack, insertData, updateblkno,
					   newlpage, newrpage);
		return GPTP_SPLIT;
	}

	/* 否则，已准备好继续插入 */
	return GPTP_INSERT;
}

/*
 * 在 beginPlaceToPage 判定放得下之后执行数据插入。
 *
 * 本函数在临界区内调用，且 XLOG 记录的创建（若
 * 需要）已经开始。目标缓冲区登记在槽 0 中。
 */
static void
entryExecPlaceToPage(GinBtree btree, Buffer buf, GinBtreeStack *stack,
					 void *insertPayload, BlockNumber updateblkno,
					 void *ptp_workspace)
{
	GinBtreeEntryInsertData *insertData = insertPayload;
	Page		page = BufferGetPage(buf);
	OffsetNumber off = stack->off;
	OffsetNumber placed;

	entryPreparePage(btree, page, off, insertData, updateblkno);

	placed = PageAddItem(page,
						 (Item) insertData->entry,
						 IndexTupleSize(insertData->entry),
						 off, false, false);
	if (placed != off)
		elog(ERROR, "failed to add item to index page in \"%s\"",
			 RelationGetRelationName(btree->index));

	if (RelationNeedsWAL(btree->index) && !btree->isBuild)
	{
		/*
		 * 它必须是 static 的，因为它要一直存活到 XLogInsert，
		 * 而这里无法 palloc。虽不雅，但 XLogInsert 的基础设施
		 * 反正也不可重入。
		 */
		static ginxlogInsertEntry data;

		data.isDelete = insertData->isDelete;
		data.offset = off;

		XLogRegisterBufData(0, (char *) &data,
							offsetof(ginxlogInsertEntry, tuple));
		XLogRegisterBufData(0, (char *) insertData->entry,
							IndexTupleSize(insertData->entry));
	}
}

/*
 * 分裂条目页并插入新数据。
 *
 * 把新的临时页返回到 *newlpage 和 *newrpage。
 * 原始缓冲区保持不动。
 */
static void
entrySplitPage(GinBtree btree, Buffer origbuf,
			   GinBtreeStack *stack,
			   GinBtreeEntryInsertData *insertData,
			   BlockNumber updateblkno,
			   Page *newlpage, Page *newrpage)
{
	OffsetNumber off = stack->off;
	OffsetNumber i,
				maxoff,
				separator = InvalidOffsetNumber;
	Size		totalsize = 0;
	Size		lsize = 0,
				size;
	char	   *ptr;
	IndexTuple	itup;
	Page		page;
	Page		lpage = PageGetTempPageCopy(BufferGetPage(origbuf));
	Page		rpage = PageGetTempPageCopy(BufferGetPage(origbuf));
	Size		pageSize = PageGetPageSize(lpage);
	PGAlignedBlock tupstore[2]; /* 可能需要两页那么多的元组 */

	entryPreparePage(btree, lpage, off, insertData, updateblkno);

	/*
	 * 首先，把全部已有元组和正在插入的新元组
	 * 依次追加到临时工作区中。
	 */
	maxoff = PageGetMaxOffsetNumber(lpage);
	ptr = tupstore[0].data;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		if (i == off)
		{
			size = MAXALIGN(IndexTupleSize(insertData->entry));
			memcpy(ptr, insertData->entry, size);
			ptr += size;
			totalsize += size + sizeof(ItemIdData);
		}

		itup = (IndexTuple) PageGetItem(lpage, PageGetItemId(lpage, i));
		size = MAXALIGN(IndexTupleSize(itup));
		memcpy(ptr, itup, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	if (off == maxoff + 1)
	{
		size = MAXALIGN(IndexTupleSize(insertData->entry));
		memcpy(ptr, insertData->entry, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	/*
	 * 初始化左页和右页，并把全部元组
	 * 复制回去。
	 */
	GinInitPage(rpage, GinPageGetOpaque(lpage)->flags, pageSize);
	GinInitPage(lpage, GinPageGetOpaque(rpage)->flags, pageSize);

	ptr = tupstore[0].data;
	maxoff++;
	lsize = 0;

	page = lpage;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) ptr;

		/*
		 * 决定在何处分裂。我们尽量使两页的数据总量
		 * 相等，而不是元组个数相等。
		 */
		if (lsize > totalsize / 2)
		{
			if (separator == InvalidOffsetNumber)
				separator = i - 1;
			page = rpage;
		}
		else
		{
			lsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
		}

		if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(btree->index));
		ptr += MAXALIGN(IndexTupleSize(itup));
	}

	/* 把临时页返回给调用者 */
	*newlpage = lpage;
	*newrpage = rpage;
}

/*
 * 构造插入有效载荷，用于插入给定缓冲区的下行指针。
 */
static void *
entryPrepareDownlink(GinBtree btree, Buffer lbuf)
{
	GinBtreeEntryInsertData *insertData;
	Page		lpage = BufferGetPage(lbuf);
	BlockNumber lblkno = BufferGetBlockNumber(lbuf);
	IndexTuple	itup;

	itup = getRightMostTuple(lpage);

	insertData = palloc(sizeof(GinBtreeEntryInsertData));
	insertData->entry = GinFormInteriorTuple(itup, lpage, lblkno);
	insertData->isDelete = false;

	return insertData;
}

/*
 * 用子页的最右值填充新根。
 * 也会从 ginxlog 调用，不应使用 btree
 */
void
ginEntryFillRoot(GinBtree btree, Page root,
				 BlockNumber lblkno, Page lpage,
				 BlockNumber rblkno, Page rpage)
{
	IndexTuple	itup;

	itup = GinFormInteriorTuple(getRightMostTuple(lpage), lpage, lblkno);
	if (PageAddItem(root, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);

	itup = GinFormInteriorTuple(getRightMostTuple(rpage), rpage, rblkno);
	if (PageAddItem(root, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);
}

/*
 * 为访问条目页设置 GinBtree
 *
 * 注意：WAL 恢复期间，ginstate 中除了一个伪造的 Relation
 * 指针之外可能没有有效数据；键 datum 也是假的。
 */
void
ginPrepareEntryScan(GinBtree btree, OffsetNumber attnum,
					Datum key, GinNullCategory category,
					GinState *ginstate)
{
	memset(btree, 0, sizeof(GinBtreeData));

	btree->index = ginstate->index;
	btree->rootBlkno = GIN_ROOT_BLKNO;
	btree->ginstate = ginstate;

	btree->findChildPage = entryLocateEntry;
	btree->getLeftMostChild = entryGetLeftMostPage;
	btree->isMoveRight = entryIsMoveRight;
	btree->findItem = entryLocateLeafEntry;
	btree->findChildPtr = entryFindChildPtr;
	btree->beginPlaceToPage = entryBeginPlaceToPage;
	btree->execPlaceToPage = entryExecPlaceToPage;
	btree->fillRoot = ginEntryFillRoot;
	btree->prepareDownlink = entryPrepareDownlink;

	btree->isData = false;
	btree->fullScan = false;
	btree->isBuild = false;

	btree->entryAttnum = attnum;
	btree->entryKey = key;
	btree->entryCategory = category;
}
