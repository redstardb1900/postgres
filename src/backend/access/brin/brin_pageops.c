/*
 * brin_pageops.c
 *		BRIN 索引的页处理例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_pageops.c
 */
#include "postgres.h"

#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_revmap.h"
#include "access/brin_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"

/*
 * BRIN_PAGETYPE_REGULAR 页中一个条目的最大尺寸。与其他索引访问
 * 方法不同，我们可以容忍每页只有一个项。
 */
#define BrinMaxItemSize \
	MAXALIGN_DOWN(BLCKSZ - \
				  (MAXALIGN(SizeOfPageHeaderData + \
							sizeof(ItemIdData)) + \
				   MAXALIGN(sizeof(BrinSpecialSpace))))

static Buffer brin_getinsertbuffer(Relation irel, Buffer oldbuf, Size itemsz,
								   bool *extended);
static Size br_page_get_freespace(Page page);
static void brin_initialize_empty_new_buffer(Relation idxrel, Buffer buffer);


/*
 * 把缓冲区 oldbuf 中偏移 oldoff 处的元组 origtup（大小 origsz）
 * 更新为 newtup（大小 newsz），作为从 heapBlk 开始的页范围的
 * 汇总元组。进入时 oldbuf 不得上锁，退出时也不持有锁。
 *
 * 若 samepage 为真，尝试把新元组放在同一页；若没有空间，
 * 则改用其他页。
 *
 * 更新成功则返回真，并把 revmap 更新为指向新元组。无论
 * 何种原因未能完成更新，都返回假。发生这种情况时，调用者
 * 可以重试该更新。
 */
bool
brin_doupdate(Relation idxrel, BlockNumber pagesPerRange,
			  BrinRevmap *revmap, BlockNumber heapBlk,
			  Buffer oldbuf, OffsetNumber oldoff,
			  const BrinTuple *origtup, Size origsz,
			  const BrinTuple *newtup, Size newsz,
			  bool samepage)
{
	Page		oldpage;
	ItemId		oldlp;
	BrinTuple  *oldtup;
	Size		oldsz;
	Buffer		newbuf;
	BlockNumber newblk = InvalidBlockNumber;
	bool		extended;

	Assert(newsz == MAXALIGN(newsz));

	/* 若该项过大，则不必费心。 */
	if (newsz > BrinMaxItemSize)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
						newsz, BrinMaxItemSize, RelationGetRelationName(idxrel))));
		return false;			/* 消除编译器告警 */
	}

	/* 确保 revmap 足够长，能容纳所需条目 */
	brinRevmapExtend(revmap, heapBlk);

	if (!samepage)
	{
		/* 需要一页来放置该项 */
		newbuf = brin_getinsertbuffer(idxrel, oldbuf, newsz, &extended);
		if (!BufferIsValid(newbuf))
		{
			Assert(!extended);
			return false;
		}

		/*
		 * 注意：返回的 newbuf 有可能（尽管不太可能）与 oldbuf
		 * 相同，如果 brin_getinsertbuffer 判定旧缓冲区确实
		 * 有足够空间。
		 */
		if (newbuf == oldbuf)
		{
			Assert(!extended);
			newbuf = InvalidBuffer;
		}
		else
			newblk = BufferGetBlockNumber(newbuf);
	}
	else
	{
		LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
		newbuf = InvalidBuffer;
		extended = false;
	}
	oldpage = BufferGetPage(oldbuf);
	oldlp = PageGetItemId(oldpage, oldoff);

	/*
	 * 检查旧元组没有被并发更新：它可能已经
	 * 整个移到别处，甚至整页都可能已变成
	 * revmap 页。注意，这里检查的前两种情况下，
	 * 刚算出的 "oldlp" 是无效的；但
	 * PageGetItemId() 足够简单，事先做这次计算
	 * 仍然是安全的。
	 */
	if (!BRIN_IS_REGULAR_PAGE(oldpage) ||
		oldoff > PageGetMaxOffsetNumber(oldpage) ||
		!ItemIdIsNormal(oldlp))
	{
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		/*
		 * 若发生这种情况，且新缓冲区是通过扩展关系得到的，
		 * 则必须确保既不把它留在未初始化状态，也不把它
		 * 忘掉。
		 */
		if (BufferIsValid(newbuf))
		{
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
			if (extended)
				FreeSpaceMapVacuumRange(idxrel, newblk, newblk + 1);
		}
		return false;
	}

	oldsz = ItemIdGetLength(oldlp);
	oldtup = (BrinTuple *) PageGetItem(oldpage, oldlp);

	/*
	 * ……或者它可能已被原地更新成不同的内容。
	 */
	if (!brin_tuples_equal(oldtup, oldsz, origtup, origsz))
	{
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		if (BufferIsValid(newbuf))
		{
			/* 同上，若得到了新页，则初始化并记录它 */
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
			if (extended)
				FreeSpaceMapVacuumRange(idxrel, newblk, newblk + 1);
		}
		return false;
	}

	/*
	 * 很好，旧元组完好。可以继续更新。
	 *
	 * 若旧页有足够空间放下新元组，就替换它。
	 *
	 * 注意，即使调用者告知空间不够，现在页上也可能已有足够
	 * 空间：并发更新可能把另一个元组移走，或换成了更小的
	 * 元组。
	 */
	if (((BrinPageFlags(oldpage) & BRIN_EVACUATE_PAGE) == 0) &&
		brin_can_do_samepage_update(oldbuf, origsz, newsz))
	{
		START_CRIT_SECTION();
		if (!PageIndexTupleOverwrite(oldpage, oldoff, (Item) unconstify(BrinTuple *, newtup), newsz))
			elog(ERROR, "failed to replace BRIN tuple");
		MarkBufferDirty(oldbuf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(idxrel))
		{
			xl_brin_samepage_update xlrec;
			XLogRecPtr	recptr;
			uint8		info = XLOG_BRIN_SAMEPAGE_UPDATE;

			xlrec.offnum = oldoff;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfBrinSamepageUpdate);

			XLogRegisterBuffer(0, oldbuf, REGBUF_STANDARD);
			XLogRegisterBufData(0, (char *) unconstify(BrinTuple *, newtup), newsz);

			recptr = XLogInsert(RM_BRIN_ID, info);

			PageSetLSN(oldpage, recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		if (BufferIsValid(newbuf))
		{
			/* 同上，若得到了新页，则初始化并记录它 */
			if (extended)
				brin_initialize_empty_new_buffer(idxrel, newbuf);
			UnlockReleaseBuffer(newbuf);
			if (extended)
				FreeSpaceMapVacuumRange(idxrel, newblk, newblk + 1);
		}

		return true;
	}
	else if (newbuf == InvalidBuffer)
	{
		/*
		 * 空间不够，但调用者说有。告诉他们
		 * 从头再来。
		 */
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		return false;
	}
	else
	{
		/*
		 * oldpage 上没有足够空闲空间。把新元组放到新页上，
		 * 并更新 revmap。
		 */
		Page		newpage = BufferGetPage(newbuf);
		Buffer		revmapbuf;
		ItemPointerData newtid;
		OffsetNumber newoff;
		Size		freespace = 0;

		revmapbuf = brinLockRevmapPageForUpdate(revmap, heapBlk);

		START_CRIT_SECTION();

		/*
		 * 若该页是新得到的，需要先初始化。注意，
		 * 初始化会作为更新的一部分记入 WAL，因此
		 * 这里不必单独做。
		 */
		if (extended)
			brin_page_init(newpage, BRIN_PAGETYPE_REGULAR);

		PageIndexTupleDeleteNoCompact(oldpage, oldoff);
		newoff = PageAddItem(newpage, (Item) unconstify(BrinTuple *, newtup), newsz,
							 InvalidOffsetNumber, false, false);
		if (newoff == InvalidOffsetNumber)
			elog(ERROR, "failed to add BRIN tuple to new page");
		MarkBufferDirty(oldbuf);
		MarkBufferDirty(newbuf);

		/* 下面更新 FSM 时需要 */
		if (extended)
			freespace = br_page_get_freespace(newpage);

		ItemPointerSet(&newtid, newblk, newoff);
		brinSetHeapBlockItemptr(revmapbuf, pagesPerRange, heapBlk, newtid);
		MarkBufferDirty(revmapbuf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(idxrel))
		{
			xl_brin_update xlrec;
			XLogRecPtr	recptr;
			uint8		info;

			info = XLOG_BRIN_UPDATE | (extended ? XLOG_BRIN_INIT_PAGE : 0);

			xlrec.insert.offnum = newoff;
			xlrec.insert.heapBlk = heapBlk;
			xlrec.insert.pagesPerRange = pagesPerRange;
			xlrec.oldOffnum = oldoff;

			XLogBeginInsert();

			/* 新页 */
			XLogRegisterData((char *) &xlrec, SizeOfBrinUpdate);

			XLogRegisterBuffer(0, newbuf, REGBUF_STANDARD | (extended ? REGBUF_WILL_INIT : 0));
			XLogRegisterBufData(0, (char *) unconstify(BrinTuple *, newtup), newsz);

			/* revmap 页 */
			XLogRegisterBuffer(1, revmapbuf, 0);

			/* 旧页 */
			XLogRegisterBuffer(2, oldbuf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_BRIN_ID, info);

			PageSetLSN(oldpage, recptr);
			PageSetLSN(newpage, recptr);
			PageSetLSN(BufferGetPage(revmapbuf), recptr);
		}

		END_CRIT_SECTION();

		LockBuffer(revmapbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);
		UnlockReleaseBuffer(newbuf);

		if (extended)
		{
			RecordPageWithFreeSpace(idxrel, newblk, freespace);
			FreeSpaceMapVacuumRange(idxrel, newblk, newblk + 1);
		}

		return true;
	}
}

/*
 * 返回 brin_doupdate 能否做同页更新。
 */
bool
brin_can_do_samepage_update(Buffer buffer, Size origsz, Size newsz)
{
	return
		((newsz <= origsz) ||
		 PageGetExactFreeSpace(BufferGetPage(buffer)) >= (newsz - origsz));
}

/*
 * 向索引关系插入一个索引元组。revmap 会被更新，
 * 把包含给定页的范围标记为指向刚插入的条目。
 * 同时写入 WAL。
 *
 * 若缓冲区有效，先检查是否有空闲空间插入新条目；若不够，
 * 则取得并钉住一个新缓冲区。进入时不得持有缓冲区锁，
 * 退出时也不持有缓冲区锁。
 *
 * 返回值是元组被插入处的偏移号。
 */
OffsetNumber
brin_doinsert(Relation idxrel, BlockNumber pagesPerRange,
			  BrinRevmap *revmap, Buffer *buffer, BlockNumber heapBlk,
			  BrinTuple *tup, Size itemsz)
{
	Page		page;
	BlockNumber blk;
	OffsetNumber off;
	Size		freespace = 0;
	Buffer		revmapbuf;
	ItemPointerData tid;
	bool		extended;

	Assert(itemsz == MAXALIGN(itemsz));

	/* 若该项过大，则根本不必费心。 */
	if (itemsz > BrinMaxItemSize)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
						itemsz, BrinMaxItemSize, RelationGetRelationName(idxrel))));
		return InvalidOffsetNumber; /* 消除编译器告警 */
	}

	/* 确保 revmap 足够长，能容纳所需条目 */
	brinRevmapExtend(revmap, heapBlk);

	/*
	 * 若调用者提供了缓冲区，则取得其上的锁。若空间不够，
	 * 则解除钉住，以便下面取得一个新缓冲区。
	 */
	if (BufferIsValid(*buffer))
	{
		/*
		 * 另一个后端（或我们自己！）可能已经把 revmap
		 * 扩展到了我们钉住的那一页上，因此无法假定
		 * 它仍是常规页。
		 */
		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		if (br_page_get_freespace(BufferGetPage(*buffer)) < itemsz)
		{
			UnlockReleaseBuffer(*buffer);
			*buffer = InvalidBuffer;
		}
	}

	/*
	 * 若仍然没有可用缓冲区，就让 brin_getinsertbuffer
	 * 为我们取得一个。
	 */
	if (!BufferIsValid(*buffer))
	{
		do
			*buffer = brin_getinsertbuffer(idxrel, InvalidBuffer, itemsz, &extended);
		while (!BufferIsValid(*buffer));
	}
	else
		extended = false;

	/* 现在取得 revmap 缓冲区上的锁 */
	revmapbuf = brinLockRevmapPageForUpdate(revmap, heapBlk);

	page = BufferGetPage(*buffer);
	blk = BufferGetBlockNumber(*buffer);

	/* 执行实际的插入 */
	START_CRIT_SECTION();
	if (extended)
		brin_page_init(page, BRIN_PAGETYPE_REGULAR);
	off = PageAddItem(page, (Item) tup, itemsz, InvalidOffsetNumber,
					  false, false);
	if (off == InvalidOffsetNumber)
		elog(ERROR, "failed to add BRIN tuple to new page");
	MarkBufferDirty(*buffer);

	/* 下面更新 FSM 时需要 */
	if (extended)
		freespace = br_page_get_freespace(page);

	ItemPointerSet(&tid, blk, off);
	brinSetHeapBlockItemptr(revmapbuf, pagesPerRange, heapBlk, tid);
	MarkBufferDirty(revmapbuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(idxrel))
	{
		xl_brin_insert xlrec;
		XLogRecPtr	recptr;
		uint8		info;

		info = XLOG_BRIN_INSERT | (extended ? XLOG_BRIN_INIT_PAGE : 0);
		xlrec.heapBlk = heapBlk;
		xlrec.pagesPerRange = pagesPerRange;
		xlrec.offnum = off;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinInsert);

		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD | (extended ? REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(0, (char *) tup, itemsz);

		XLogRegisterBuffer(1, revmapbuf, 0);

		recptr = XLogInsert(RM_BRIN_ID, info);

		PageSetLSN(page, recptr);
		PageSetLSN(BufferGetPage(revmapbuf), recptr);
	}

	END_CRIT_SECTION();

	/* 元组已稳妥写入缓冲区；可以释放锁 */
	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
	LockBuffer(revmapbuf, BUFFER_LOCK_UNLOCK);

	BRIN_elog((DEBUG2, "inserted tuple (%u,%u) for range starting at %u",
			   blk, off, heapBlk));

	if (extended)
	{
		RecordPageWithFreeSpace(idxrel, blk, freespace);
		FreeSpaceMapVacuumRange(idxrel, blk, blk + 1);
	}

	return off;
}

/*
 * 用给定类型初始化一页。
 *
 * 调用者负责在适当时把它标为脏。
 */
void
brin_page_init(Page page, uint16 type)
{
	PageInit(page, BLCKSZ, sizeof(BrinSpecialSpace));

	BrinPageType(page) = type;
}

/*
 * 初始化新 BRIN 索引的元页。
 */
void
brin_metapage_init(Page page, BlockNumber pagesPerRange, uint16 version)
{
	BrinMetaPageData *metadata;

	brin_page_init(page, BRIN_PAGETYPE_META);

	metadata = (BrinMetaPageData *) PageGetContents(page);

	metadata->brinMagic = BRIN_META_MAGIC;
	metadata->brinVersion = version;
	metadata->pagesPerRange = pagesPerRange;

	/*
	 * 注意，这里稍微取了个巧。0 不是有效的 revmap 块号
	 * （因为它是元页缓冲区），但这样做能在创建索引时
	 * 同时创建第一张 revmap 页。
	 */
	metadata->lastRevmapPage = 0;

	/*
	 * 把 pd_lower 设到元数据末尾刚过的位置。这一点至关重要，
	 * 因为若不这样做，xlog.c 压缩该页时
	 * 元数据就会丢失。
	 */
	((PageHeader) page)->pd_lower =
		((char *) metadata + sizeof(BrinMetaPageData)) - (char *) page;
}

/*
 * 启动页面 evacuated 协议。
 *
 * 调用者必须以排他模式锁住该页。
 *
 * 若该页尚未初始化或为空，则什么也不做并返回假；
 * 它可以不加任何改动就用作 revmap。若其中含有元组，
 * 则把它标记为 evacuated 并返回真。
 */
bool
brin_start_evacuating_page(Relation idxRel, Buffer buf)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	Page		page;

	page = BufferGetPage(buf);

	if (PageIsNew(page))
		return false;

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp;

		lp = PageGetItemId(page, off);
		if (ItemIdIsUsed(lp))
		{
			/*
			 * 阻止其他后端再往该页添加内容：
			 * BRIN_EVACUATE_PAGE 告知 br_page_get_freespace，此页
			 * 不能再用来添加新元组。注意，该标志不记入 WAL，
			 * 除非是偶然。
			 */
			BrinPageFlags(page) |= BRIN_EVACUATE_PAGE;
			MarkBufferDirtyHint(buf, true);

			return true;
		}
	}
	return false;
}

/*
 * 把一页上的全部元组移走。
 *
 * 调用者必须持有该页的锁。锁和钉住都会被释放。
 */
void
brin_evacuate_page(Relation idxRel, BlockNumber pagesPerRange,
				   BrinRevmap *revmap, Buffer buf)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	Page		page;
	BrinTuple  *btup = NULL;
	Size		btupsz = 0;

	page = BufferGetPage(buf);

	Assert(BrinPageFlags(page) & BRIN_EVACUATE_PAGE);

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		BrinTuple  *tup;
		Size		sz;
		ItemId		lp;

		CHECK_FOR_INTERRUPTS();

		lp = PageGetItemId(page, off);
		if (ItemIdIsUsed(lp))
		{
			sz = ItemIdGetLength(lp);
			tup = (BrinTuple *) PageGetItem(page, lp);
			tup = brin_copy_tuple(tup, sz, btup, &btupsz);

			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			if (!brin_doupdate(idxRel, pagesPerRange, revmap, tup->bt_blkno,
							   buf, off, tup, sz, tup, sz, false))
				off--;			/* 重试 */

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			/* 有人可能已经把 revmap 扩展到了这一页上 */
			if (!BRIN_IS_REGULAR_PAGE(page))
				break;
		}
	}

	UnlockReleaseBuffer(buf);
}

/*
 * 给定一个 BRIN 索引页，必要时初始化它，并把当前
 * 空闲空间记入 FSM。
 *
 * 主要用途是：清理期间发现未初始化的页，这可能是
 * 关系扩展之后、该页投入使用之前发生崩溃
 * 造成的。
 *
 * 这里不费心更新上层 FSM 页，而是指望调用者
 * （brin_vacuum_scan）在扫描结束时修正它们。本文件
 * 其他地方通常应把新增的空闲空间立即传播到
 * 上层 FSM 页。
 */
void
brin_page_cleanup(Relation idxrel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * 若某页被留在未初始化状态，现在初始化它；
	 * 同时把它记入 FSM。
	 *
	 * 可能有人正在并发扩展该关系。为避免在他们取得
	 * 缓冲区锁之前重新初始化该页，我们短暂取得扩展锁。
	 * 由于他们从取得该页之前一直持有扩展锁，
	 * 直到该页初始化之后，我们一定能看到
	 * 他们的初始化。
	 */
	if (PageIsNew(page))
	{
		LockRelationForExtension(idxrel, ShareLock);
		UnlockRelationForExtension(idxrel, ShareLock);

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(page))
		{
			brin_initialize_empty_new_buffer(idxrel, buf);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			return;
		}
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}

	/* 非常规索引页无需处理 */
	if (BRIN_IS_META_PAGE(BufferGetPage(buf)) ||
		BRIN_IS_REVMAP_PAGE(BufferGetPage(buf)))
		return;

	/* 测量空闲空间并记录它 */
	RecordPageWithFreeSpace(idxrel, BufferGetBlockNumber(buf),
							br_page_get_freespace(page));
}

/*
 * 返回一个已钉住且排他锁住的缓冲区，可用于插入
 * 大小为 itemsz 的索引项（调用者必须确保不请求
 * 无法满足的大小）。若 oldbuf 是有效缓冲区，
 * 也会被锁住（加锁顺序经过安排，以避免死锁）。
 *
 * 若发现旧页已不再是常规索引页（因为 revmap
 * 扩展），则解锁旧缓冲区并返回
 * InvalidBuffer。
 *
 * 若没有任何现有页有足够空闲空间容纳新项，
 * 就扩展关系。若发生这种情况，*extended 被置为真，
 * 调用者有责任在使用前初始化该页（并把这一事实
 * 记入 WAL）。调用者还应在插入后用该页剩余空闲空间
 * 更新 FSM。
 *
 * 注意，除非 *extended 被置为真，否则并不预期调用者
 * 更新 FSM。这一策略意味着：创建页时，以及发现页
 * 空间不足以插入所需元组时，我们会更新 FSM；但并非
 * 每次向该页添加元组都更新。
 *
 * 注意，在某些极端情况下，本例程可能扩展了关系
 * 却不返回新页。若发生这种情况，本例程有责任把页
 * 初始化记入 WAL，并把该页记入 FSM，因为调用者
 * 肯定做不到。
 */
static Buffer
brin_getinsertbuffer(Relation irel, Buffer oldbuf, Size itemsz,
					 bool *extended)
{
	BlockNumber oldblk;
	BlockNumber newblk;
	Page		page;
	Size		freespace;

	/* 调用者必须已经检查过 */
	Assert(itemsz <= BrinMaxItemSize);

	if (BufferIsValid(oldbuf))
		oldblk = BufferGetBlockNumber(oldbuf);
	else
		oldblk = InvalidBlockNumber;

	/* 选择初始目标页；若已知现有目标则复用 */
	newblk = RelationGetTargetBlock(irel);
	if (newblk == InvalidBlockNumber)
		newblk = GetPageWithFreeSpace(irel, itemsz);

	/*
	 * 循环直到找到空闲空间足够的页。从该循环返回给
	 * 调用者时，两个缓冲区都有效且已上锁；若必须在此
	 * 重新开始，则两页都未上锁，且 newblk 未被钉住
	 * （若它确实有效的话）。
	 */
	for (;;)
	{
		Buffer		buf;
		bool		extensionLockHeld = false;

		CHECK_FOR_INTERRUPTS();

		*extended = false;

		if (newblk == InvalidBlockNumber)
		{
			/*
			 * 按 FSM 的说法，任何现有索引页
			 * 都没有足够空闲空间：扩展关系以取得
			 * 一张崭新的页。
			 *
			 * XXX：这里很可能可以用 RBM_ZERO_AND_LOCK，
			 * 从而不必在缓冲区回收期间
			 * 一直持有扩展锁。
			 */
			if (!RELATION_IS_LOCAL(irel))
			{
				LockRelationForExtension(irel, ExclusiveLock);
				extensionLockHeld = true;
			}
			buf = ReadBuffer(irel, P_NEW);
			newblk = BufferGetBlockNumber(buf);
			*extended = true;

			BRIN_elog((DEBUG2, "brin_getinsertbuffer: extending to page %u",
					   BufferGetBlockNumber(buf)));
		}
		else if (newblk == oldblk)
		{
			/*
			 * 这里有个古怪的极端情况：FSM 已过时，
			 * 给了我们旧页。
			 */
			buf = oldbuf;
		}
		else
		{
			buf = ReadBuffer(irel, newblk);
		}

		/*
		 * 若旧缓冲区比新的更靠前，先锁旧缓冲区；但随后
		 * 必须检查它没有被并发变成 revmap 页。若检测到
		 * 发生了这种情况，就放弃，并告诉调用者
		 * 从头再来。
		 */
		if (BufferIsValid(oldbuf) && oldblk < newblk)
		{
			LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
			if (!BRIN_IS_REGULAR_PAGE(BufferGetPage(oldbuf)))
			{
				LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

				/*
				 * 新页有可能是通过扩展关系得到的。
				 * 若是这样，
				 * 离开前必须把它记入 FSM，
				 * 否则这块空间会永远丢失。
				 * 不过，无法让未初始化的页进入 FSM，
				 * 所以需要先初始化它。
				 */
				if (*extended)
					brin_initialize_empty_new_buffer(irel, buf);

				if (extensionLockHeld)
					UnlockRelationForExtension(irel, ExclusiveLock);

				ReleaseBuffer(buf);

				if (*extended)
				{
					FreeSpaceMapVacuumRange(irel, newblk, newblk + 1);
					/* 应当无关紧要，但不要迷惑调用者 */
					*extended = false;
				}

				return InvalidBuffer;
			}
		}

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		if (extensionLockHeld)
			UnlockRelationForExtension(irel, ExclusiveLock);

		page = BufferGetPage(buf);

		/*
		 * 我们有一个可插入的新缓冲区。检查新页是否有足够
		 * 空闲空间，若有则返回它；否则从头再来。
		 * （br_page_get_freespace 还会检查 FSM 没有交给我们
		 * 一页后来被改作 revmap 的页。）
		 */
		freespace = *extended ?
			BrinMaxItemSize : br_page_get_freespace(page);
		if (freespace >= itemsz)
		{
			RelationSetTargetBlock(irel, newblk);

			/*
			 * 若旧缓冲区尚未上锁，则锁住它。注意，此时可以
			 * 确定它是常规页：它位于刚取得的新页之后，
			 * 而新页不是 revmap 页，且 revmap 页
			 * 总是连续的。
			 */
			if (BufferIsValid(oldbuf) && oldblk > newblk)
			{
				LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
				Assert(BRIN_IS_REGULAR_PAGE(BufferGetPage(oldbuf)));
			}

			return buf;
		}

		/* 这一页不行。 */

		/*
		 * 若一张全新的页都放不下新项所需的空闲空间，
		 * 那么该项肯定过大。大声报错；但首先确保
		 * 初始化该页并把它记为空闲，
		 * 以备下次使用。
		 */
		if (*extended)
		{
			brin_initialize_empty_new_buffer(irel, buf);
			/* 既然这不该发生，就跳过 FreeSpaceMapVacuum */

			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
							itemsz, freespace, RelationGetRelationName(irel))));
			return InvalidBuffer;	/* 消除编译器告警 */
		}

		if (newblk != oldblk)
			UnlockReleaseBuffer(buf);
		if (BufferIsValid(oldbuf) && oldblk <= newblk)
			LockBuffer(oldbuf, BUFFER_LOCK_UNLOCK);

		/*
		 * 用该页新的、想必更小的空闲空间值更新 FSM，然后
		 * 搜索新的目标页。
		 */
		newblk = RecordAndGetPageWithFreeSpace(irel, newblk, freespace, itemsz);
	}
}

/*
 * 把一页初始化为空的常规 BRIN 页，把这记入 WAL，
 * 并把该页记入 FSM。
 *
 * 有几种极端情况：我们扩展关系以取得新页，
 * 后来发现无法立即使用它。这时不应让该页
 * 不记入 FSM，因为没有机制能把空间收回，
 * 索引会膨胀。另外，因为我们不会把本应
 * 初始化该页的动作记入 WAL，该页在备库上
 * （或恢复之后）会保持未初始化。
 *
 * 虽然这里把该页记入了 FSM，若合适的话，
 * 调用者仍负责做 FSM 上层页更新。
 */
static void
brin_initialize_empty_new_buffer(Relation idxrel, Buffer buffer)
{
	Page		page;

	BRIN_elog((DEBUG2,
			   "brin_initialize_empty_new_buffer: initializing blank page %u",
			   BufferGetBlockNumber(buffer)));

	START_CRIT_SECTION();
	page = BufferGetPage(buffer);
	brin_page_init(page, BRIN_PAGETYPE_REGULAR);
	MarkBufferDirty(buffer);
	log_newpage_buffer(buffer, true);
	END_CRIT_SECTION();

	/*
	 * 我们更新该页的 FSM，但这不记入 WAL。这是可以接受的，
	 * 因为 VACUUM 会扫描索引，并用那些在崩溃中被遗忘了
	 * FSM 记录的页来更新 FSM。
	 */
	RecordPageWithFreeSpace(idxrel, BufferGetBlockNumber(buffer),
							br_page_get_freespace(page));
}


/*
 * 返回常规 BRIN 索引页上的空闲空间量。
 *
 * 若该页不是常规页，或已被标记 BRIN_EVACUATE_PAGE
 * 标志，则返回 0。
 */
static Size
br_page_get_freespace(Page page)
{
	if (!BRIN_IS_REGULAR_PAGE(page) ||
		(BrinPageFlags(page) & BRIN_EVACUATE_PAGE) != 0)
		return 0;
	else
		return PageGetFreeSpace(page);
}
