/*
 * brin_revmap.c
 *		BRIN 索引的范围映射（反向映射）
 *
 * 范围映射（revmap）是 BRIN 索引的转换结构：每个
 * 页范围有一个汇总元组，其位置由
 * revmap 跟踪。每当插入表中的新元组违反
 * 先前保存的摘要值时，就向索引插入一个新元组，
 * 并更新 revmap 使其指向该元组。
 *
 * revmap 存放在索引最前面的若干页中，紧接在
 * 元页之后。当 revmap 需要扩展时，该块上
 * 常规 BRIN 页中的全部元组（若有）都会被移开。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_revmap.c
 */
#include "postgres.h"

#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_revmap.h"
#include "access/brin_tuple.h"
#include "access/brin_xlog.h"
#include "access/rmgr.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"


/*
 * revmap 页中，每个项存放一个 ItemPointerData。这些宏用来
 * 根据给定的堆块号，求出该 revmap 项的
 * 逻辑 revmap 页号和项下标。
 */
#define HEAPBLK_TO_REVMAP_BLK(pagesPerRange, heapBlk) \
	((heapBlk / pagesPerRange) / REVMAP_PAGE_MAXITEMS)
#define HEAPBLK_TO_REVMAP_INDEX(pagesPerRange, heapBlk) \
	((heapBlk / pagesPerRange) % REVMAP_PAGE_MAXITEMS)


struct BrinRevmap
{
	Relation	rm_irel;
	BlockNumber rm_pagesPerRange;
	BlockNumber rm_lastRevmapPage;	/* 从元页缓存而来 */
	Buffer		rm_metaBuf;
	Buffer		rm_currBuf;
};

/* typedef 出现在 brin_revmap.h 中 */


static BlockNumber revmap_get_blkno(BrinRevmap *revmap,
									BlockNumber heapBlk);
static Buffer revmap_get_buffer(BrinRevmap *revmap, BlockNumber heapBlk);
static BlockNumber revmap_extend_and_get_blkno(BrinRevmap *revmap,
											   BlockNumber heapBlk);
static void revmap_physical_extend(BrinRevmap *revmap);

/*
 * 为范围映射初始化一个访问对象。调用者用完后必须由
 * brinRevmapTerminate 释放它。
 */
BrinRevmap *
brinRevmapInitialize(Relation idxrel, BlockNumber *pagesPerRange,
					 Snapshot snapshot)
{
	BrinRevmap *revmap;
	Buffer		meta;
	BrinMetaPageData *metadata;
	Page		page;

	meta = ReadBuffer(idxrel, BRIN_METAPAGE_BLKNO);
	LockBuffer(meta, BUFFER_LOCK_SHARE);
	page = BufferGetPage(meta);
	TestForOldSnapshot(snapshot, idxrel, page);
	metadata = (BrinMetaPageData *) PageGetContents(page);

	revmap = palloc(sizeof(BrinRevmap));
	revmap->rm_irel = idxrel;
	revmap->rm_pagesPerRange = metadata->pagesPerRange;
	revmap->rm_lastRevmapPage = metadata->lastRevmapPage;
	revmap->rm_metaBuf = meta;
	revmap->rm_currBuf = InvalidBuffer;

	*pagesPerRange = metadata->pagesPerRange;

	LockBuffer(meta, BUFFER_LOCK_UNLOCK);

	return revmap;
}

/*
 * 释放与 revmap 访问对象关联的资源。
 */
void
brinRevmapTerminate(BrinRevmap *revmap)
{
	ReleaseBuffer(revmap->rm_metaBuf);
	if (revmap->rm_currBuf != InvalidBuffer)
		ReleaseBuffer(revmap->rm_currBuf);
	pfree(revmap);
}

/*
 * 扩展 revmap，使其覆盖给定的堆块号。
 */
void
brinRevmapExtend(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber mapBlk PG_USED_FOR_ASSERTS_ONLY;

	mapBlk = revmap_extend_and_get_blkno(revmap, heapBlk);

	/* 确保取得的缓冲区落在预期范围内 */
	Assert(mapBlk != InvalidBlockNumber &&
		   mapBlk != BRIN_METAPAGE_BLKNO &&
		   mapBlk <= revmap->rm_lastRevmapPage);
}

/*
 * 准备向 revmap 插入一项；该项将要驻留的 revmap 缓冲区
 * 会被上锁并返回。多数调用者应事先调用
 * brinRevmapExtend，因为若 revmap 不够长，本例程
 * 并不扩展它。
 *
 * 返回的缓冲区也会记入 revmap 结构；结束对该结构的使用时
 * 会释放该缓冲区，因此调用者不必显式释放。
 */
Buffer
brinLockRevmapPageForUpdate(BrinRevmap *revmap, BlockNumber heapBlk)
{
	Buffer		rmBuf;

	rmBuf = revmap_get_buffer(revmap, heapBlk);
	LockBuffer(rmBuf, BUFFER_LOCK_EXCLUSIVE);

	return rmBuf;
}

/*
 * 在调用者已适当上锁的给定 revmap 缓冲区中（该缓冲区用于
 * 每范围 pagesPerRange 页的 BRIN 索引），设置对应于
 * 堆块号 heapBlk 的元素为给定的 TID。
 *
 * 操作完成后，调用者必须更新所返回
 * 缓冲区上的 LSN。
 *
 * 常规操作和 WAL 重放期间都会使用。
 */
void
brinSetHeapBlockItemptr(Buffer buf, BlockNumber pagesPerRange,
						BlockNumber heapBlk, ItemPointerData tid)
{
	RevmapContents *contents;
	ItemPointerData *iptr;
	Page		page;

	/* 正确的页应当已经钉住并上锁 */
	page = BufferGetPage(buf);
	contents = (RevmapContents *) PageGetContents(page);
	iptr = (ItemPointerData *) contents->rm_tids;
	iptr += HEAPBLK_TO_REVMAP_INDEX(pagesPerRange, heapBlk);

	if (ItemPointerIsValid(&tid))
		ItemPointerSet(iptr,
					   ItemPointerGetBlockNumber(&tid),
					   ItemPointerGetOffsetNumber(&tid));
	else
		ItemPointerSetInvalid(iptr);
}

/*
 * 取出给定堆块对应的 BrinTuple。
 *
 * 含有该元组的缓冲区被上锁，并经 *buf 返回。返回的
 * 元组指向共享缓冲区，不得释放；若调用者
 * 想在释放缓冲区锁之后继续使用，必须自行建立
 * palloc 出来的副本。作为优化，调用者可在进入时传入已钉住的缓冲区
 * *buf，这样当下一个元组与前一个在同一页时，可避免一次
 * 钉住与解除钉住的往返。
 *
 * 若给定堆范围找不到元组，则返回 NULL。此时
 * *buf 仍可能被更新（调用者必须解除钉住），但它
 * 并未上锁。
 *
 * 元组在缓冲区内的偏移经 *off 返回，其大小
 * 经 *size 返回。
 */
BrinTuple *
brinGetTupleForHeapBlock(BrinRevmap *revmap, BlockNumber heapBlk,
						 Buffer *buf, OffsetNumber *off, Size *size, int mode,
						 Snapshot snapshot)
{
	Relation	idxRel = revmap->rm_irel;
	BlockNumber mapBlk;
	RevmapContents *contents;
	ItemPointerData *iptr;
	BlockNumber blk;
	Page		page;
	ItemId		lp;
	BrinTuple  *tup;
	ItemPointerData previptr;

	/* 把堆块号规范化为该范围内的第一页 */
	heapBlk = (heapBlk / revmap->rm_pagesPerRange) * revmap->rm_pagesPerRange;

	/*
	 * 计算所需的 revmap 页号。若返回 Invalid（即
	 * revmap 页尚未创建），则所请求的页范围
	 * 尚未摘要。
	 */
	mapBlk = revmap_get_blkno(revmap, heapBlk);
	if (mapBlk == InvalidBlockNumber)
	{
		*off = InvalidOffsetNumber;
		return NULL;
	}

	ItemPointerSetInvalid(&previptr);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (revmap->rm_currBuf == InvalidBuffer ||
			BufferGetBlockNumber(revmap->rm_currBuf) != mapBlk)
		{
			if (revmap->rm_currBuf != InvalidBuffer)
				ReleaseBuffer(revmap->rm_currBuf);

			Assert(mapBlk != InvalidBlockNumber);
			revmap->rm_currBuf = ReadBuffer(revmap->rm_irel, mapBlk);
		}

		LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_SHARE);

		contents = (RevmapContents *)
			PageGetContents(BufferGetPage(revmap->rm_currBuf));
		iptr = contents->rm_tids;
		iptr += HEAPBLK_TO_REVMAP_INDEX(revmap->rm_pagesPerRange, heapBlk);

		if (!ItemPointerIsValid(iptr))
		{
			LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_UNLOCK);
			return NULL;
		}

		/*
		 * 检查上一轮得到的 TID（若有），并保存从
		 * revmap 取得的当前 TID；若再次循环，可做健全性检查，
		 * 确认下一次得到的与这次不同。否则，若 revmap
		 * 以某种方式严重损坏，我们可能会永远循环下去。
		 */
		if (ItemPointerIsValid(&previptr) && ItemPointerEquals(&previptr, iptr))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("corrupted BRIN index: inconsistent range map")));
		previptr = *iptr;

		blk = ItemPointerGetBlockNumber(iptr);
		*off = ItemPointerGetOffsetNumber(iptr);

		LockBuffer(revmap->rm_currBuf, BUFFER_LOCK_UNLOCK);

		/* 好，已得到 BrinTuple 应在位置的指针。取出来。 */
		if (!BufferIsValid(*buf) || BufferGetBlockNumber(*buf) != blk)
		{
			if (BufferIsValid(*buf))
				ReleaseBuffer(*buf);
			*buf = ReadBuffer(idxRel, blk);
		}
		LockBuffer(*buf, mode);
		page = BufferGetPage(*buf);
		TestForOldSnapshot(snapshot, idxRel, page);

		/* 若落到 revmap 页上，则从头再来 */
		if (BRIN_IS_REGULAR_PAGE(page))
		{
			/*
			 * 若偏移号大于该页中已有的最大偏移，则有可能
			 * 该范围已被并发取消摘要。对此直接
			 * 返回 NULL 即可。
			 */
			if (*off > PageGetMaxOffsetNumber(page))
			{
				LockBuffer(*buf, BUFFER_LOCK_UNLOCK);
				return NULL;
			}

			lp = PageGetItemId(page, *off);
			if (ItemIdIsUsed(lp))
			{
				tup = (BrinTuple *) PageGetItem(page, lp);

				if (tup->bt_blkno == heapBlk)
				{
					if (size)
						*size = ItemIdGetLength(lp);
					/* 找到了！ */
					return tup;
				}
			}
		}

		/*
		 * 未找到。假定 revmap 已被并发更新。
		 */
		LockBuffer(*buf, BUFFER_LOCK_UNLOCK);
	}
	/* 不会到达，只为让编译器安静 */
	return NULL;
}

/*
 * 删除一个索引元组，把页范围标记为未摘要。
 *
 * 索引必须以 ShareUpdateExclusiveLock 模式锁定。
 *
 * 若调用者应当重试，则返回 false。
 */
bool
brinRevmapDesummarizeRange(Relation idxrel, BlockNumber heapBlk)
{
	BrinRevmap *revmap;
	BlockNumber pagesPerRange;
	RevmapContents *contents;
	ItemPointerData *iptr;
	ItemPointerData invalidIptr;
	BlockNumber revmapBlk;
	Buffer		revmapBuf;
	Buffer		regBuf;
	Page		revmapPg;
	Page		regPg;
	OffsetNumber revmapOffset;
	OffsetNumber regOffset;
	ItemId		lp;

	revmap = brinRevmapInitialize(idxrel, &pagesPerRange, NULL);

	revmapBlk = revmap_get_blkno(revmap, heapBlk);
	if (!BlockNumberIsValid(revmapBlk))
	{
		/* revmap 页不存在：范围未摘要，到此结束 */
		brinRevmapTerminate(revmap);
		return true;
	}

	/* 锁住 revmap 页，并从中取得索引元组指针 */
	revmapBuf = brinLockRevmapPageForUpdate(revmap, heapBlk);
	revmapPg = BufferGetPage(revmapBuf);
	revmapOffset = HEAPBLK_TO_REVMAP_INDEX(revmap->rm_pagesPerRange, heapBlk);

	contents = (RevmapContents *) PageGetContents(revmapPg);
	iptr = contents->rm_tids;
	iptr += revmapOffset;

	if (!ItemPointerIsValid(iptr))
	{
		/* 没有索引元组：范围未摘要，到此结束 */
		LockBuffer(revmapBuf, BUFFER_LOCK_UNLOCK);
		brinRevmapTerminate(revmap);
		return true;
	}

	regBuf = ReadBuffer(idxrel, ItemPointerGetBlockNumber(iptr));
	LockBuffer(regBuf, BUFFER_LOCK_EXCLUSIVE);
	regPg = BufferGetPage(regBuf);

	/*
	 * 我们只是在删除数据，而不是读取，因此这里无需
	 * 调用 TestForOldSnapshot。
	 */

	/* 若这已不再是常规页，让调用者从头再来 */
	if (!BRIN_IS_REGULAR_PAGE(regPg))
	{
		LockBuffer(revmapBuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(regBuf, BUFFER_LOCK_UNLOCK);
		brinRevmapTerminate(revmap);
		return false;
	}

	regOffset = ItemPointerGetOffsetNumber(iptr);
	if (regOffset > PageGetMaxOffsetNumber(regPg))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupted BRIN index: inconsistent range map")));

	lp = PageGetItemId(regPg, regOffset);
	if (!ItemIdIsUsed(lp))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupted BRIN index: inconsistent range map")));

	/*
	 * 占位元组只在未完成的摘要过程中出现，而我们
	 * 持有 ShareUpdateExclusiveLock，因此本函数无法与该过程
	 * 并发运行。现存的任何占位元组都是
	 * 崩溃或已中止的摘要过程留下的；静默删除它们。
	 */

	START_CRIT_SECTION();

	ItemPointerSetInvalid(&invalidIptr);
	brinSetHeapBlockItemptr(revmapBuf, revmap->rm_pagesPerRange, heapBlk,
							invalidIptr);
	PageIndexTupleDeleteNoCompact(regPg, regOffset);
	/* XXX 要在 FSM 中记录空闲空间吗？ */

	MarkBufferDirty(regBuf);
	MarkBufferDirty(revmapBuf);

	if (RelationNeedsWAL(idxrel))
	{
		xl_brin_desummarize xlrec;
		XLogRecPtr	recptr;

		xlrec.pagesPerRange = revmap->rm_pagesPerRange;
		xlrec.heapBlk = heapBlk;
		xlrec.regOffset = regOffset;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinDesummarize);
		XLogRegisterBuffer(0, revmapBuf, 0);
		XLogRegisterBuffer(1, regBuf, REGBUF_STANDARD);
		recptr = XLogInsert(RM_BRIN_ID, XLOG_BRIN_DESUMMARIZE);
		PageSetLSN(revmapPg, recptr);
		PageSetLSN(regPg, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(regBuf);
	LockBuffer(revmapBuf, BUFFER_LOCK_UNLOCK);
	brinRevmapTerminate(revmap);

	return true;
}

/*
 * 给定堆块号，找出对应的物理 revmap 块号
 * 并返回。若 revmap 页尚未分配，则返回
 * InvalidBlockNumber。
 */
static BlockNumber
revmap_get_blkno(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber targetblk;

	/* 取得 revmap 块号，跳过 1 以避开元页块 */
	targetblk = HEAPBLK_TO_REVMAP_BLK(revmap->rm_pagesPerRange, heapBlk) + 1;

	/* 正常情形：revmap 页已经分配 */
	if (targetblk <= revmap->rm_lastRevmapPage)
		return targetblk;

	return InvalidBlockNumber;
}

/*
 * 取得并返回含有给定堆页所对应 revmap 页的缓冲区。
 * revmap 必须事先已经扩展到覆盖该页。
 * 返回的缓冲区也会记入 revmap 结构；结束对该结构的使用时
 * 会释放该缓冲区，因此调用者不必显式释放。
 */
static Buffer
revmap_get_buffer(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber mapBlk;

	/* 把堆块号转换成物理索引位置。 */
	mapBlk = revmap_get_blkno(revmap, heapBlk);

	if (mapBlk == InvalidBlockNumber)
		elog(ERROR, "revmap does not cover heap block %u", heapBlk);

	/* 确保取得的缓冲区落在预期范围内 */
	Assert(mapBlk != BRIN_METAPAGE_BLKNO &&
		   mapBlk <= revmap->rm_lastRevmapPage);

	/*
	 * 取得需要从中读取的缓冲区。若访问结构中已经有
	 * 正确的缓冲区，就用它；否则释放那个，
	 * （若有效）并读入我们需要的那个。
	 */
	if (revmap->rm_currBuf == InvalidBuffer ||
		mapBlk != BufferGetBlockNumber(revmap->rm_currBuf))
	{
		if (revmap->rm_currBuf != InvalidBuffer)
			ReleaseBuffer(revmap->rm_currBuf);

		revmap->rm_currBuf = ReadBuffer(revmap->rm_irel, mapBlk);
	}

	return revmap->rm_currBuf;
}

/*
 * 给定堆块号，找出对应的物理 revmap 块号
 * 并返回。若 revmap 页尚未分配，则扩展
 * revmap，直到它被分配出来。
 */
static BlockNumber
revmap_extend_and_get_blkno(BrinRevmap *revmap, BlockNumber heapBlk)
{
	BlockNumber targetblk;

	/* 取得 revmap 块号，跳过 1 以避开元页块 */
	targetblk = HEAPBLK_TO_REVMAP_BLK(revmap->rm_pagesPerRange, heapBlk) + 1;

	/* 必要时扩展 revmap */
	while (targetblk > revmap->rm_lastRevmapPage)
	{
		CHECK_FOR_INTERRUPTS();
		revmap_physical_extend(revmap);
	}

	return targetblk;
}

/*
 * 尝试把 revmap 扩展一页。由于多种原因，这可能不会发生；
 * 调用者应重试，直到得到预期的结果。
 */
static void
revmap_physical_extend(BrinRevmap *revmap)
{
	Buffer		buf;
	Page		page;
	Page		metapage;
	BrinMetaPageData *metadata;
	BlockNumber mapBlk;
	BlockNumber nblocks;
	Relation	irel = revmap->rm_irel;

	/*
	 * 锁住元页。这会挡住 revmap 的并发扩展，
	 * 但请注意仍需取得关系扩展锁，因为
	 * 另一个后端可以用常规 BRIN 页来扩展该索引。
	 */
	LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(revmap->rm_metaBuf);
	metadata = (BrinMetaPageData *) PageGetContents(metapage);

	/*
	 * 检查缓存的 lastRevmapPage 值是否仍是最新的；若不是，
	 * 更新缓存的副本，并让调用者从头再来。
	 */
	if (metadata->lastRevmapPage != revmap->rm_lastRevmapPage)
	{
		revmap->rm_lastRevmapPage = metadata->lastRevmapPage;
		LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
		return;
	}
	mapBlk = metadata->lastRevmapPage + 1;

	nblocks = RelationGetNumberOfBlocks(irel);
	if (mapBlk < nblocks)
	{
		buf = ReadBuffer(irel, mapBlk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
	}
	else
	{
		buf = ExtendBufferedRel(BMR_REL(irel), MAIN_FORKNUM, NULL,
								EB_LOCK_FIRST);
		if (BufferGetBlockNumber(buf) != mapBlk)
		{
			/*
			 * 极罕见的极端情况：我们读完关系长度之后，
			 * 有人并发扩展了该关系。若发生这种情况，就放弃
			 * 并让调用者从头再来。以后必须把该页从
			 * 正在使用它的人下面撤离。
			 */
			LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buf);
			return;
		}
		page = BufferGetPage(buf);
	}

	/* 检查它是常规块（或空白页） */
	if (!PageIsNew(page) && !BRIN_IS_REGULAR_PAGE(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("unexpected page type 0x%04X in BRIN index \"%s\" block %u",
						BrinPageType(page),
						RelationGetRelationName(irel),
						BufferGetBlockNumber(buf))));

	/* 若该页正在使用，则撤离它并重新开始 */
	if (brin_start_evacuating_page(irel, buf))
	{
		LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);
		brin_evacuate_page(irel, revmap->rm_pagesPerRange, revmap, buf);

		/* 让调用者从头再来 */
		return;
	}

	/*
	 * 好，现在已经锁住元页和目标块。把目标块
	 * 重新初始化为 revmap 页，并更新元页。
	 */
	START_CRIT_SECTION();

	/* rm_tids 数组由 PageInit 初始化为全部无效 */
	brin_page_init(page, BRIN_PAGETYPE_REVMAP);
	MarkBufferDirty(buf);

	metadata->lastRevmapPage = mapBlk;

	/*
	 * 把 pd_lower 设到元数据末尾刚过的位置。这一点至关重要，
	 * 因为若不这样做，xlog.c 压缩该页时元数据就会丢失。
	 * （必须在这里做，因为 11 之前的 PG 版本没有正确设置
	 * 元页的 pd_lower，因此经 pg_upgraded 的索引可能含有
	 * 错误的值。）
	 */
	((PageHeader) metapage)->pd_lower =
		((char *) metadata + sizeof(BrinMetaPageData)) - (char *) metapage;

	MarkBufferDirty(revmap->rm_metaBuf);

	if (RelationNeedsWAL(revmap->rm_irel))
	{
		xl_brin_revmap_extend xlrec;
		XLogRecPtr	recptr;

		xlrec.targetBlk = mapBlk;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinRevmapExtend);
		XLogRegisterBuffer(0, revmap->rm_metaBuf, REGBUF_STANDARD);

		XLogRegisterBuffer(1, buf, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_BRIN_ID, XLOG_BRIN_REVMAP_EXTEND);
		PageSetLSN(metapage, recptr);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(revmap->rm_metaBuf, BUFFER_LOCK_UNLOCK);

	UnlockReleaseBuffer(buf);
}
