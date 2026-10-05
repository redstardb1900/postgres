/*-------------------------------------------------------------------------
 *
 * ginfast.c
 *	  Postgres 倒排索引访问方法的快速插入例程。
 *	  挂起条目保存在页的线性列表中。之后
 *	  （通常在 VACUUM 期间）会调用 ginInsertCleanup()，把
 *	  挂起条目转入常规索引结构。这样做有利，
 *	  因为批量插入比零售插入高效得多。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginfast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_am.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "postmaster/autovacuum.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC 参数 */
int			gin_pending_list_limit = 0;

#define GIN_PAGE_FREESIZE \
	( BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(GinPageOpaqueData)) )

typedef struct KeyArray
{
	Datum	   *keys;			/* 可扩展数组 */
	GinNullCategory *categories;	/* 另一个可扩展数组 */
	int32		nvalues;		/* 当前有效条目数 */
	int32		maxvalues;		/* 数组已分配大小 */
} KeyArray;


/*
 * 由给定元组数组构建挂起列表页，并写出去。
 *
 * 返回页上剩余的空闲空间量。
 */
static int32
writeListPage(Relation index, Buffer buffer,
			  IndexTuple *tuples, int32 ntuples, BlockNumber rightlink)
{
	Page		page = BufferGetPage(buffer);
	int32		i,
				freesize,
				size = 0;
	OffsetNumber l,
				off;
	PGAlignedBlock workspace;
	char	   *ptr;

	START_CRIT_SECTION();

	GinInitBuffer(buffer, GIN_LIST);

	off = FirstOffsetNumber;
	ptr = workspace.data;

	for (i = 0; i < ntuples; i++)
	{
		int			this_size = IndexTupleSize(tuples[i]);

		memcpy(ptr, tuples[i], this_size);
		ptr += this_size;
		size += this_size;

		l = PageAddItem(page, (Item) tuples[i], this_size, off, false, false);

		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(index));

		off++;
	}

	Assert(size <= BLCKSZ);		/* 否则越界写了 workspace */

	GinPageGetOpaque(page)->rightlink = rightlink;

	/*
	 * 尾页可以只包含完整行（row），或放在前几页上的行的最后一部分
	 * （此处的 "row" 指为一个堆元组生成的全部
	 * 索引元组）
	 */
	if (rightlink == InvalidBlockNumber)
	{
		GinPageSetFullRow(page);
		GinPageGetOpaque(page)->maxoff = 1;
	}
	else
	{
		GinPageGetOpaque(page)->maxoff = 0;
	}

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		ginxlogInsertListPage data;
		XLogRecPtr	recptr;

		data.rightlink = rightlink;
		data.ntuples = ntuples;

		XLogBeginInsert();
		XLogRegisterData((char *) &data, sizeof(ginxlogInsertListPage));

		XLogRegisterBuffer(0, buffer, REGBUF_WILL_INIT);
		XLogRegisterBufData(0, workspace.data, size);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT_LISTPAGE);
		PageSetLSN(page, recptr);
	}

	/* 在释放缓冲区前取得空闲空间 */
	freesize = PageGetExactFreeSpace(page);

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	return freesize;
}

static void
makeSublist(Relation index, IndexTuple *tuples, int32 ntuples,
			GinMetaPageData *res)
{
	Buffer		curBuffer = InvalidBuffer;
	Buffer		prevBuffer = InvalidBuffer;
	int			i,
				size = 0,
				tupsize;
	int			startTuple = 0;

	Assert(ntuples > 0);

	/*
	 * 把元组拆分到各页
	 */
	for (i = 0; i < ntuples; i++)
	{
		if (curBuffer == InvalidBuffer)
		{
			curBuffer = GinNewBuffer(index);

			if (prevBuffer != InvalidBuffer)
			{
				res->nPendingPages++;
				writeListPage(index, prevBuffer,
							  tuples + startTuple,
							  i - startTuple,
							  BufferGetBlockNumber(curBuffer));
			}
			else
			{
				res->head = BufferGetBlockNumber(curBuffer);
			}

			prevBuffer = curBuffer;
			startTuple = i;
			size = 0;
		}

		tupsize = MAXALIGN(IndexTupleSize(tuples[i])) + sizeof(ItemIdData);

		if (size + tupsize > GinListPageSize)
		{
			/* 放不下，强制新开一页并重新处理 */
			i--;
			curBuffer = InvalidBuffer;
		}
		else
		{
			size += tupsize;
		}
	}

	/*
	 * 写出最后一页
	 */
	res->tail = BufferGetBlockNumber(curBuffer);
	res->tailFreeSize = writeListPage(index, curBuffer,
									  tuples + startTuple,
									  ntuples - startTuple,
									  InvalidBlockNumber);
	res->nPendingPages++;
	/* 那只是一个堆元组 */
	res->nPendingHeapTuples = 1;
}

/*
 * 把 *collector 中包含的索引元组写入索引的
 * 挂起列表。
 *
 * 函数保证所有这些元组会按序连续插入，
 * 保持顺序
 */
void
ginHeapTupleFastInsert(GinState *ginstate, GinTupleCollector *collector)
{
	Relation	index = ginstate->index;
	Buffer		metabuffer;
	Page		metapage;
	GinMetaPageData *metadata = NULL;
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;
	ginxlogUpdateMeta data;
	bool		separateList = false;
	bool		needCleanup = false;
	int			cleanupSize;
	bool		needWal;

	if (collector->ntuples == 0)
		return;

	needWal = RelationNeedsWAL(index);

	data.locator = index->rd_locator;
	data.ntuples = 0;
	data.newRightlink = data.prevTail = InvalidBlockNumber;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	/*
	 * 向挂起列表的插入在逻辑上可能属于树中任意位置，
	 * 因此它与所有可串行化扫描冲突。所有扫描都对
	 * 元页面缓冲区获取谓词锁来表示这一点。因此我们会
	 * 检查冲突，但要等到我们已锁定页面并且
	 * 准备好修改页面时才检查。
	 */

	if (collector->sumsize + collector->ntuples * sizeof(ItemIdData) > GinListPageSize)
	{
		/*
		 * 总大小超过一页 => 建立子列表
		 */
		separateList = true;
	}
	else
	{
		LockBuffer(metabuffer, GIN_EXCLUSIVE);
		metadata = GinPageGetMeta(metapage);

		if (metadata->head == InvalidBlockNumber ||
			collector->sumsize + collector->ntuples * sizeof(ItemIdData) > metadata->tailFreeSize)
		{
			/*
			 * 挂起列表为空，或总大小大于尾页
			 * 空闲空间 => 建立子列表
			 *
			 * 解锁元页面缓冲区以保持高并发
			 */
			separateList = true;
			LockBuffer(metabuffer, GIN_UNLOCK);
		}
	}

	if (separateList)
	{
		/*
		 * 应单独建立子列表并追加到尾部
		 */
		GinMetaPageData sublist;

		memset(&sublist, 0, sizeof(GinMetaPageData));
		makeSublist(index, collector->tuples, collector->ntuples, &sublist);

		/*
		 * 元页面此前已解锁，见上
		 */
		LockBuffer(metabuffer, GIN_EXCLUSIVE);
		metadata = GinPageGetMeta(metapage);

		CheckForSerializableConflictIn(index, NULL, GIN_METAPAGE_BLKNO);

		if (metadata->head == InvalidBlockNumber)
		{
			/*
			 * 主列表为空，因此直接把子列表作为主列表插入
			 */
			START_CRIT_SECTION();

			metadata->head = sublist.head;
			metadata->tail = sublist.tail;
			metadata->tailFreeSize = sublist.tailFreeSize;

			metadata->nPendingPages = sublist.nPendingPages;
			metadata->nPendingHeapTuples = sublist.nPendingHeapTuples;

			if (needWal)
				XLogBeginInsert();
		}
		else
		{
			/*
			 * 合并列表
			 */
			data.prevTail = metadata->tail;
			data.newRightlink = sublist.head;

			buffer = ReadBuffer(index, metadata->tail);
			LockBuffer(buffer, GIN_EXCLUSIVE);
			page = BufferGetPage(buffer);

			Assert(GinPageGetOpaque(page)->rightlink == InvalidBlockNumber);

			START_CRIT_SECTION();

			GinPageGetOpaque(page)->rightlink = sublist.head;

			MarkBufferDirty(buffer);

			metadata->tail = sublist.tail;
			metadata->tailFreeSize = sublist.tailFreeSize;

			metadata->nPendingPages += sublist.nPendingPages;
			metadata->nPendingHeapTuples += sublist.nPendingHeapTuples;

			if (needWal)
			{
				XLogBeginInsert();
				XLogRegisterBuffer(1, buffer, REGBUF_STANDARD);
			}
		}
	}
	else
	{
		/*
		 * 插入尾页。元页面已加锁
		 */
		OffsetNumber l,
					off;
		int			i,
					tupsize;
		char	   *ptr;
		char	   *collectordata;

		CheckForSerializableConflictIn(index, NULL, GIN_METAPAGE_BLKNO);

		buffer = ReadBuffer(index, metadata->tail);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);

		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

		collectordata = ptr = (char *) palloc(collector->sumsize);

		data.ntuples = collector->ntuples;

		START_CRIT_SECTION();

		if (needWal)
			XLogBeginInsert();

		/*
		 * 增加堆元组计数器
		 */
		Assert(GinPageGetOpaque(page)->maxoff <= metadata->nPendingHeapTuples);
		GinPageGetOpaque(page)->maxoff++;
		metadata->nPendingHeapTuples++;

		for (i = 0; i < collector->ntuples; i++)
		{
			tupsize = IndexTupleSize(collector->tuples[i]);
			l = PageAddItem(page, (Item) collector->tuples[i], tupsize, off, false, false);

			if (l == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in \"%s\"",
					 RelationGetRelationName(index));

			memcpy(ptr, collector->tuples[i], tupsize);
			ptr += tupsize;

			off++;
		}

		Assert((ptr - collectordata) <= collector->sumsize);
		if (needWal)
		{
			XLogRegisterBuffer(1, buffer, REGBUF_STANDARD);
			XLogRegisterBufData(1, collectordata, collector->sumsize);
		}

		metadata->tailFreeSize = PageGetExactFreeSpace(page);

		MarkBufferDirty(buffer);
	}

	/*
	 * 把 pd_lower 设到刚好越过元数据末尾。这很关键，
	 * 因为若不这样做，xlog.c 压缩页面时元数据会丢失。
	 * （必须在此设置，因为 PG v11 之前的版本没有
	 * 正确设置元页面的 pd_lower，所以经 pg_upgraded 升级的索引可能
	 * 含有错误值。）
	 */
	((PageHeader) metapage)->pd_lower =
		((char *) metadata + sizeof(GinMetaPageData)) - (char *) metapage;

	/*
	 * 写元页面缓冲区，生成 xlog 记录
	 */
	MarkBufferDirty(metabuffer);

	if (needWal)
	{
		XLogRecPtr	recptr;

		memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

		XLogRegisterBuffer(0, metabuffer, REGBUF_WILL_INIT | REGBUF_STANDARD);
		XLogRegisterData((char *) &data, sizeof(ginxlogUpdateMeta));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_UPDATE_META_PAGE);
		PageSetLSN(metapage, recptr);

		if (buffer != InvalidBuffer)
		{
			PageSetLSN(page, recptr);
		}
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);

	/*
	 * 挂起列表变得过长时强制清理。并且，
	 * ginInsertCleanup 可能耗时很长，因此我们希望在它能于
	 * 单次收集周期内完成全部工作时再调用。在
	 * 非 vacuum 模式下，它不应需要 maintenance_work_mem，因此在挂起列表
	 * 仍小到能装进
	 * gin_pending_list_limit 时触发。
	 *
	 * 不应在我们的 CRIT_SECTION 内调用 ginInsertCleanup()。
	 */
	cleanupSize = GinGetPendingListCleanupSize(index);
	if (metadata->nPendingPages * GIN_PAGE_FREESIZE > cleanupSize * 1024L)
		needCleanup = true;

	UnlockReleaseBuffer(metabuffer);

	END_CRIT_SECTION();

	/*
	 * 由于可能与并发清理过程争用，我们不会强制
	 * 清理挂起列表。
	 */
	if (needCleanup)
		ginInsertCleanup(ginstate, false, true, false, NULL);
}

/*
 * 为单个可索引项（ht_ctid 指定的堆元组的一个索引列）
 * 创建临时索引元组，并追加到 *collector 中的
 * 数组。随后会用
 * ginHeapTupleFastInsert 写出去。注意，为保证状态一致，给定
 * 堆元组的全部临时元组必须在一次调用
 * ginHeapTupleFastInsert 中写出。
 */
void
ginHeapTupleFastCollect(GinState *ginstate,
						GinTupleCollector *collector,
						OffsetNumber attnum, Datum value, bool isNull,
						ItemPointer ht_ctid)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		i,
				nentries;

	/*
	 * 提取需要插入索引的键值
	 */
	entries = ginExtractEntries(ginstate, attnum, value, isNull,
								&nentries, &categories);

	/*
	 * 防止分配计算中的整数溢出
	 */
	if (nentries < 0 ||
		collector->ntuples + nentries > MaxAllocSize / sizeof(IndexTuple))
		elog(ERROR, "too many entries for GIN index");

	/*
	 * 为存放已收集元组分配/重新分配内存
	 */
	if (collector->tuples == NULL)
	{
		/*
		 * 确定 tuples 数组最初要分配的元素个数。
		 * 取 2 的幂，以免调整大小时浪费内存
		 * （因为 palloc 喜欢 2 的幂）。
		 */
		collector->lentuples = pg_nextpower2_32(Max(16, nentries));
		collector->tuples = palloc_array(IndexTuple, collector->lentuples);
	}
	else if (collector->lentuples < collector->ntuples + nentries)
	{
		/*
		 * 把 lentuples 推进到下一个合适的 2 的幂。这不会
		 * 溢出，不过我们可能到达超过
		 * MaxAllocSize/sizeof(IndexTuple) 的值，导致 repalloc 出错。
		 */
		collector->lentuples = pg_nextpower2_32(collector->ntuples + nentries);
		collector->tuples = repalloc_array(collector->tuples,
										   IndexTuple, collector->lentuples);
	}

	/*
	 * 为每个键值构建索引元组并加入数组。在挂起
	 * 元组中我们只是把堆 TID 放进 t_tid。
	 */
	for (i = 0; i < nentries; i++)
	{
		IndexTuple	itup;

		itup = GinFormTuple(ginstate, attnum, entries[i], categories[i],
							NULL, 0, 0, true);
		itup->t_tid = *ht_ctid;
		collector->tuples[collector->ntuples++] = itup;
		collector->sumsize += IndexTupleSize(itup);
	}
}

/*
 * 删除直到（不含）newHead 页为止的挂起列表页。
 * 若 newHead == InvalidBlockNumber，则函数丢弃整个列表。
 *
 * 在本函数全程中，元页面保持 pin 且排他锁定。
 */
static void
shiftList(Relation index, Buffer metabuffer, BlockNumber newHead,
		  bool fill_fsm, IndexBulkDeleteResult *stats)
{
	Page		metapage;
	GinMetaPageData *metadata;
	BlockNumber blknoToDelete;

	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);
	blknoToDelete = metadata->head;

	do
	{
		Page		page;
		int			i;
		int64		nDeletedHeapTuples = 0;
		ginxlogDeleteListPages data;
		Buffer		buffers[GIN_NDELETE_AT_ONCE];
		BlockNumber freespace[GIN_NDELETE_AT_ONCE];

		data.ndeleted = 0;
		while (data.ndeleted < GIN_NDELETE_AT_ONCE && blknoToDelete != newHead)
		{
			freespace[data.ndeleted] = blknoToDelete;
			buffers[data.ndeleted] = ReadBuffer(index, blknoToDelete);
			LockBuffer(buffers[data.ndeleted], GIN_EXCLUSIVE);
			page = BufferGetPage(buffers[data.ndeleted]);

			data.ndeleted++;

			Assert(!GinPageIsDeleted(page));

			nDeletedHeapTuples += GinPageGetOpaque(page)->maxoff;
			blknoToDelete = GinPageGetOpaque(page)->rightlink;
		}

		if (stats)
			stats->pages_deleted += data.ndeleted;

		/*
		 * 本操作触及异常多的页面，因此在进入
		 * 临界区之前先为 XLogInsert 机制准备好
		 * 空间。
		 */
		if (RelationNeedsWAL(index))
			XLogEnsureRecordSpace(data.ndeleted, 0);

		START_CRIT_SECTION();

		metadata->head = blknoToDelete;

		Assert(metadata->nPendingPages >= data.ndeleted);
		metadata->nPendingPages -= data.ndeleted;
		Assert(metadata->nPendingHeapTuples >= nDeletedHeapTuples);
		metadata->nPendingHeapTuples -= nDeletedHeapTuples;

		if (blknoToDelete == InvalidBlockNumber)
		{
			metadata->tail = InvalidBlockNumber;
			metadata->tailFreeSize = 0;
			metadata->nPendingPages = 0;
			metadata->nPendingHeapTuples = 0;
		}

		/*
		 * 把 pd_lower 设到刚好越过元数据末尾。这很关键，
		 * 因为若不这样做，xlog.c
		 * 压缩页面时元数据会丢失。（必须在此设置，因为 PG v11
		 * 之前的版本没有正确设置元页面的 pd_lower，所以经
		 * pg_upgraded 升级的索引可能含有错误值。）
		 */
		((PageHeader) metapage)->pd_lower =
			((char *) metadata + sizeof(GinMetaPageData)) - (char *) metapage;

		MarkBufferDirty(metabuffer);

		for (i = 0; i < data.ndeleted; i++)
		{
			page = BufferGetPage(buffers[i]);
			GinPageGetOpaque(page)->flags = GIN_DELETED;
			MarkBufferDirty(buffers[i]);
		}

		if (RelationNeedsWAL(index))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, metabuffer,
							   REGBUF_WILL_INIT | REGBUF_STANDARD);
			for (i = 0; i < data.ndeleted; i++)
				XLogRegisterBuffer(i + 1, buffers[i], REGBUF_WILL_INIT);

			memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

			XLogRegisterData((char *) &data,
							 sizeof(ginxlogDeleteListPages));

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_LISTPAGE);
			PageSetLSN(metapage, recptr);

			for (i = 0; i < data.ndeleted; i++)
			{
				page = BufferGetPage(buffers[i]);
				PageSetLSN(page, recptr);
			}
		}

		for (i = 0; i < data.ndeleted; i++)
			UnlockReleaseBuffer(buffers[i]);

		END_CRIT_SECTION();

		for (i = 0; fill_fsm && i < data.ndeleted; i++)
			RecordFreeIndexPage(index, freespace[i]);

	} while (blknoToDelete != newHead);
}

/* 初始化空的 KeyArray */
static void
initKeyArray(KeyArray *keys, int32 maxvalues)
{
	keys->keys = palloc_array(Datum, maxvalues);
	keys->categories = palloc_array(GinNullCategory, maxvalues);
	keys->nvalues = 0;
	keys->maxvalues = maxvalues;
}

/* 向 KeyArray 添加 datum，必要时调整大小 */
static void
addDatum(KeyArray *keys, Datum datum, GinNullCategory category)
{
	if (keys->nvalues >= keys->maxvalues)
	{
		keys->maxvalues *= 2;
		keys->keys = repalloc_array(keys->keys, Datum, keys->maxvalues);
		keys->categories = repalloc_array(keys->categories, GinNullCategory, keys->maxvalues);
	}

	keys->keys[keys->nvalues] = datum;
	keys->categories[keys->nvalues] = category;
	keys->nvalues++;
}

/*
 * 从挂起列表页收集数据，为插入
 * 主索引做准备。
 *
 * 遍历页上所有 >= startoff 的元组，并把值收集到 accum 中
 *
 * 注意 ka 只是工作区——它不在各次调用间
 * 携带任何状态。
 */
static void
processPendingPage(BuildAccumulator *accum, KeyArray *ka,
				   Page page, OffsetNumber startoff)
{
	ItemPointerData heapptr;
	OffsetNumber i,
				maxoff;
	OffsetNumber attrnum;

	/* 把 *ka 重置为空 */
	ka->nvalues = 0;

	maxoff = PageGetMaxOffsetNumber(page);
	Assert(maxoff >= FirstOffsetNumber);
	ItemPointerSetInvalid(&heapptr);
	attrnum = 0;

	for (i = startoff; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		OffsetNumber curattnum;
		Datum		curkey;
		GinNullCategory curcategory;

		/* 检查堆 TID 或 attnum 是否变化 */
		curattnum = gintuple_get_attrnum(accum->ginstate, itup);

		if (!ItemPointerIsValid(&heapptr))
		{
			heapptr = itup->t_tid;
			attrnum = curattnum;
		}
		else if (!(ItemPointerEquals(&heapptr, &itup->t_tid) &&
				   curattnum == attrnum))
		{
			/*
			 * ginInsertBAEntries 每次调用可插入多个 datum，但仅
			 * 针对一个堆元组和一列。因此在边界处调用它，
			 * 并重置 ka。
			 */
			ginInsertBAEntries(accum, &heapptr, attrnum,
							   ka->keys, ka->categories, ka->nvalues);
			ka->nvalues = 0;
			heapptr = itup->t_tid;
			attrnum = curattnum;
		}

		/* 把键加入 KeyArray */
		curkey = gintuple_get_key(accum->ginstate, itup, &curcategory);
		addDatum(ka, curkey, curcategory);
	}

	/* 倒出所有剩余键 */
	ginInsertBAEntries(accum, &heapptr, attrnum,
					   ka->keys, ka->categories, ka->nvalues);
}

/*
 * 把元组从挂起页移入常规 GIN 结构。
 *
 * 乍看完全不崩溃安全。但若我们在把条目
 * 写入主索引之后、从挂起列表移除之前崩溃，
 * 也没问题，因为之后重做写入时，不会
 * 发生坏事。
 *
 * fill_fsm 表示 ginInsertCleanup 应把已删除页
 * 加入 FSM，否则由调用者负责把已删除页放入
 * FSM。
 *
 * 若 stats 非空，我们把已删除的挂起页计入统计。
 */
void
ginInsertCleanup(GinState *ginstate, bool full_clean,
				 bool fill_fsm, bool forceCleanup,
				 IndexBulkDeleteResult *stats)
{
	Relation	index = ginstate->index;
	Buffer		metabuffer,
				buffer;
	Page		metapage,
				page;
	GinMetaPageData *metadata;
	MemoryContext opCtx,
				oldCtx;
	BuildAccumulator accum;
	KeyArray	datums;
	BlockNumber blkno,
				blknoFinish;
	bool		cleanupFinish = false;
	bool		fsm_vac = false;
	Size		workMemory;

	/*
	 * 我们希望防止并发清理过程。为此我们将用
	 * LockPage() 以排他模式锁定元页面。没有其他
	 * 人会对该元页面使用该锁，因此仍可并发
	 * 向挂起列表插入
	 */

	if (forceCleanup)
	{
		/*
		 * 我们由 [auto]vacuum/analyze 或 gin_clean_pending_list() 调用，
		 * 希望等待并发清理完成。
		 */
		LockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
		workMemory =
			(IsAutoVacuumWorkerProcess() && autovacuum_work_mem != -1) ?
			autovacuum_work_mem : maintenance_work_mem;
	}
	else
	{
		/*
		 * 我们由常规插入调用；若看到并发清理，
		 * 则直接退出，指望并发进程会清理挂起
		 * 列表。
		 */
		if (!ConditionalLockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock))
			return;
		workMemory = work_mem;
	}

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, GIN_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);

	if (metadata->head == InvalidBlockNumber)
	{
		/* 无事可做 */
		UnlockReleaseBuffer(metabuffer);
		UnlockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
		return;
	}

	/*
	 * 记住一个尾页，以防其他后端添加新元组
	 * 比我们清理更快而导致无限清理。
	 */
	blknoFinish = metadata->tail;

	/*
	 * 读取并锁定挂起列表头
	 */
	blkno = metadata->head;
	buffer = ReadBuffer(index, blkno);
	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);

	LockBuffer(metabuffer, GIN_UNLOCK);

	/*
	 * 初始化。所有临时空间都在 opCtx 中
	 */
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "GIN insert cleanup temporary context",
								  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(opCtx);

	initKeyArray(&datums, 128);
	ginInitBA(&accum);
	accum.ginstate = ginstate;

	/*
	 * 在本循环顶部，我们对挂起列表的当前页持有 pin 和锁。
	 * 不过，在退出循环前会释放它们。
	 * 注意我们对元页面也有 pin 但没有锁。
	 */
	for (;;)
	{
		Assert(!GinPageIsDeleted(page));

		/*
		 * 我们是否正走到启动清理时记住为尾页的那一页？
		 * 但若调用者要求清理整个
		 * 挂起列表，则忽略旧尾，我们将工作到列表变为
		 * 空为止。
		 */
		if (blkno == blknoFinish && full_clean == false)
			cleanupFinish = true;

		/*
		 * 把页的 datum 读入 accum
		 */
		processPendingPage(&accum, &datums, page, FirstOffsetNumber);

		vacuum_delay_point();

		/*
		 * 是否该把内存刷到磁盘？若已到挂起列表末尾，
		 * 或已有完整行且内存开始
		 * 变满，则刷出。
		 */
		if (GinPageGetOpaque(page)->rightlink == InvalidBlockNumber ||
			(GinPageHasFullRow(page) &&
			 (accum.allocatedMemory >= workMemory * 1024L)))
		{
			ItemPointerData *list;
			uint32		nlist;
			Datum		key;
			GinNullCategory category;
			OffsetNumber maxoff,
						attnum;

			/*
			 * 解锁当前页以提高性能。页的变化
			 * 将在内存刷出完成后通过比较 maxoff
			 * 再检查。
			 */
			maxoff = PageGetMaxOffsetNumber(page);
			LockBuffer(buffer, GIN_UNLOCK);

			/*
			 * 把已收集数据移入常规结构可能
			 * 耗时很长——因此在不锁定挂起
			 * 列表的情况下运行。
			 */
			ginBeginBAScan(&accum);
			while ((list = ginGetBAEntry(&accum,
										 &attnum, &key, &category, &nlist)) != NULL)
			{
				ginEntryInsert(ginstate, attnum, key, category,
							   list, nlist, NULL);
				vacuum_delay_point();
			}

			/*
			 * 锁定整个列表以移除页面
			 */
			LockBuffer(metabuffer, GIN_EXCLUSIVE);
			LockBuffer(buffer, GIN_SHARE);

			Assert(!GinPageIsDeleted(page));

			/*
			 * 在我们让页面保持解锁期间，可能又有东西
			 * 被加到它上面。若是，则立即处理那些条目。条目
			 * 不应很多，因此我们不担心在持有排他锁时
			 * 做这件事。插入算法
			 * 保证插入的 row 不会延续到下一页。
			 * 注意：本循环中故意没有 vacuum_delay_point。
			 */
			if (PageGetMaxOffsetNumber(page) != maxoff)
			{
				ginInitBA(&accum);
				processPendingPage(&accum, &datums, page, maxoff + 1);

				ginBeginBAScan(&accum);
				while ((list = ginGetBAEntry(&accum,
											 &attnum, &key, &category, &nlist)) != NULL)
					ginEntryInsert(ginstate, attnum, key, category,
								   list, nlist, NULL);
			}

			/*
			 * 记住下一页——它将成为新的列表头
			 */
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);	/* shiftList 会做排他
											 * 加锁 */

			/*
			 * 从挂起列表移除已读页面，此时已读页面的全部内容
			 * 已在常规结构中
			 */
			shiftList(index, metabuffer, blkno, fill_fsm, stats);

			/* 至此，一些挂起页已被释放 */
			fsm_vac = true;

			Assert(blkno == metadata->head);
			LockBuffer(metabuffer, GIN_UNLOCK);

			/*
			 * 若我们移除了整个挂起列表，或清理到了尾（我们在
			 * 启动清理过程时记住的），则直接退出
			 */
			if (blkno == InvalidBlockNumber || cleanupFinish)
				break;

			/*
			 * 释放迄今使用的内存并重新初始化状态
			 */
			MemoryContextReset(opCtx);
			initKeyArray(&datums, datums.maxvalues);
			ginInitBA(&accum);
		}
		else
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);
		}

		/*
		 * 读取挂起列表中的下一页
		 */
		vacuum_delay_point();
		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, GIN_SHARE);
		page = BufferGetPage(buffer);
	}

	UnlockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
	ReleaseBuffer(metabuffer);

	/*
	 * 由于挂起列表页的更替率可能很高，普通后端
	 * 清理列表时最好立即把它们回收到
	 * FreeSpaceMap。
	 */
	if (fsm_vac && fill_fsm)
		IndexFreeSpaceMapVacuum(index);

	/* 清理临时空间 */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(opCtx);
}

/*
 * 可从 SQL 调用的函数，用于清理插入挂起列表
 */
Datum
gin_clean_pending_list(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	indexRel = index_open(indexoid, RowExclusiveLock);
	IndexBulkDeleteResult stats;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("GIN pending list cannot be cleaned up during recovery.")));

	/* 必须是 GIN 索引 */
	if (indexRel->rd_rel->relkind != RELKIND_INDEX ||
		indexRel->rd_rel->relam != GIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a GIN index",
						RelationGetRelationName(indexRel))));

	/*
	 * 拒绝读取非本地临时关系的尝试；我们很可能
	 * 得到错误数据，因为我们看不到所属
	 * 会话的本地缓冲区。
	 */
	if (RELATION_IS_OTHER_TEMP(indexRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary indexes of other sessions")));

	/* 用户必须拥有该索引（与 VACUUM 所需权限相当） */
	if (!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   RelationGetRelationName(indexRel));

	memset(&stats, 0, sizeof(stats));

	/*
	 * 不能对 !indisready 索引的内容做任何假定。让
	 * 那些成为空操作而非错误，以便用户可对访问方法的
	 * 全部索引直接运行本函数。由于 indisready&&!indisvalid 的索引
	 * 只是在等待错过的 aminsert 调用，我们有能力处理
	 * 它。出于谨慎，选择不处理。
	 */
	if (indexRel->rd_index->indisvalid)
	{
		GinState	ginstate;

		initGinState(&ginstate, indexRel);
		ginInsertCleanup(&ginstate, true, true, true, &stats);
	}
	else
		ereport(DEBUG1,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(indexRel))));

	index_close(indexRel, RowExclusiveLock);

	PG_RETURN_INT64((int64) stats.pages_deleted);
}
