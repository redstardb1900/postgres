/*-------------------------------------------------------------------------
 *
 * hashovfl.c
 *	  Postgres hash 访问方法的溢出页管理代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashovfl.c
 *
 * 说明（NOTES）
 *	  溢出页看起来和普通关系页一样。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"


static uint32 _hash_firstfreebit(uint32 map);


/*
 * 把溢出页位号（它在空闲页位图中的下标）
 * 转换成索引内的块号。
 */
static BlockNumber
bitno_to_blkno(HashMetaPage metap, uint32 ovflbitnum)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;

	/* 把从零起的位号转换成从 1 起的页号 */
	ovflbitnum += 1;

	/* 确定该页的分裂点号（必须 >= 1） */
	for (i = 1;
		 i < splitnum && ovflbitnum > metap->hashm_spares[i];
		 i++)
		 /* 循环 */ ;

	/*
	 * 加上该分裂点之前已经存在的桶页数，
	 * 转换成绝对页号。
	 */
	return (BlockNumber) (_hash_get_totalbuckets(i) + ovflbitnum);
}

/*
 * _hash_ovflblkno_to_bitno
 *
 * 把溢出页块号转换成空闲页位图所用的位号。
 */
uint32
_hash_ovflblkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;
	uint32		bitnum;

	/* 确定包含该页的分裂点号 */
	for (i = 1; i <= splitnum; i++)
	{
		if (ovflblkno <= (BlockNumber) _hash_get_totalbuckets(i))
			break;				/* 糟糕 */
		bitnum = ovflblkno - _hash_get_totalbuckets(i);

		/*
		 * bitnum 必须大于前一个分裂点已经加入的溢出页数量。
		 * 本 splitnum (i) 上若有溢出页，则应当从
		 * (_hash_get_totalbuckets(i) +
		 * metap->hashm_spares[i - 1] + 1) 开始。
		 */
		if (bitnum > metap->hashm_spares[i - 1] &&
			bitnum <= metap->hashm_spares[i])
			return bitnum - 1;	/* 减 1，把从 1 起转换成从 0 起 */
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid overflow block number %u", ovflblkno)));
	return 0;					/* 让编译器保持安静 */
}

/*
 *	_hash_addovflpage
 *
 *	向 'buf' 所指向的最后一页所属的桶添加一张溢出页。
 *
 *	进入时，调用者必须钉住 'buf'，但不持锁。
 *	若未要求保留，退出前会放下该钉住
 *	（我们假定调用者此后不再关心 'buf'）。
 *	钉住只为主桶保留。返回的溢出页会被钉住并加上写锁；
 *	它保证是空的。
 *
 *	调用者必须钉住元页缓冲区，但不持锁。
 *	该缓冲区以同样的状态返回。
 *
 * 注意：本函数可能被多个进程并发执行，
 * 不应假定返回的溢出页就是原先传入的 'buf'
 * 的直接后继。其间可能已有更多溢出页
 * 被加入桶链。
 */
Buffer
_hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf, bool retain_pin)
{
	Buffer		ovflbuf;
	Page		page;
	Page		ovflpage;
	HashPageOpaque pageopaque;
	HashPageOpaque ovflopaque;
	HashMetaPage metap;
	Buffer		mapbuf = InvalidBuffer;
	Buffer		newmapbuf = InvalidBuffer;
	BlockNumber blkno;
	uint32		orig_firstfree;
	uint32		splitnum;
	uint32	   *freep = NULL;
	uint32		max_ovflpg;
	uint32		bit;
	uint32		bitmap_page_bit;
	uint32		first_page;
	uint32		last_bit;
	uint32		last_page;
	uint32		i,
				j;
	bool		page_found = false;

	/*
	 * 对尾页加写锁。这里需要保持这样的加锁顺序：
	 * 先取得桶尾页上的锁，再取得元页上的锁，
	 * 以便找到并锁住位图页；若找到了，就释放元页
	 * 上的锁，最后再取得新溢出缓冲区上的锁。
	 * 需要这一加锁顺序，以避免与正在做插入的
	 * 后端发生死锁。
	 *
	 * 注意：若把取得溢出页做成两条 WAL 记录
	 * （一条用来分配溢出页，另一条把它加入溢出桶链），
	 * 本来可以少锁许多缓冲区。但若分配之后系统崩溃，
	 * 就会泄漏一张溢出页。
	 * 不用说，从性能角度看，只用一条记录
	 * 也更好。
	 */
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	/* 也许多余…… */
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

	/* 循环寻找当前尾页，以防别人也插入了 */
	for (;;)
	{
		BlockNumber nextblkno;

		page = BufferGetPage(buf);
		pageopaque = HashPageGetOpaque(page);
		nextblkno = pageopaque->hasho_nextblkno;

		if (!BlockNumberIsValid(nextblkno))
			break;

		/* 我们假定不需要写出未修改的页 */
		if (retain_pin)
		{
			/* 只为主桶页保留钉住 */
			Assert((pageopaque->hasho_flag & LH_PAGE_TYPE) == LH_BUCKET_PAGE);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		else
			_hash_relbuf(rel, buf);

		retain_pin = false;

		buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
	}

	/* 取得元页上的独占锁 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	_hash_checkpage(rel, metabuf, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	/* 从 hashm_firstfree 开始搜索 */
	orig_firstfree = metap->hashm_firstfree;
	first_page = orig_firstfree >> BMPG_SHIFT(metap);
	bit = orig_firstfree & BMPG_MASK(metap);
	i = first_page;
	j = bit / BITS_PER_MAP;
	bit &= ~(BITS_PER_MAP - 1);

	/* 外层循环对每个位图页迭代一次 */
	for (;;)
	{
		BlockNumber mapblkno;
		Page		mappage;
		uint32		last_inpage;

		/* 希望搜索在最后一个已有溢出页处结束 */
		splitnum = metap->hashm_ovflpoint;
		max_ovflpg = metap->hashm_spares[splitnum] - 1;
		last_page = max_ovflpg >> BMPG_SHIFT(metap);
		last_bit = max_ovflpg & BMPG_MASK(metap);

		if (i > last_page)
			break;

		Assert(i < metap->hashm_nmaps);
		mapblkno = metap->hashm_mapp[i];

		if (i == last_page)
			last_inpage = last_bit;
		else
			last_inpage = BMPGSZ_BIT(metap) - 1;

		/* 读取位图页时释放元页上的独占锁 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

		mapbuf = _hash_getbuf(rel, mapblkno, HASH_WRITE, LH_BITMAP_PAGE);
		mappage = BufferGetPage(mapbuf);
		freep = HashPageGetBitmap(mappage);

		for (; bit <= last_inpage; j++, bit += BITS_PER_MAP)
		{
			if (freep[j] != ALL_SET)
			{
				page_found = true;

				/* 重新取得元页上的独占锁 */
				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

				/* 把位转换成页内位号 */
				bit += _hash_firstfreebit(freep[j]);
				bitmap_page_bit = bit;

				/* 把位转换成绝对位号 */
				bit += (i << BMPG_SHIFT(metap));
				/* 计算回收溢出页的地址 */
				blkno = bitno_to_blkno(metap, bit);

				/* 取出并初始化回收的页 */
				ovflbuf = _hash_getinitbuf(rel, blkno);

				goto found;
			}
		}

		/* 这里没有空闲空间，试着前进到下一张位图页 */
		_hash_relbuf(rel, mapbuf);
		mapbuf = InvalidBuffer;
		i++;
		j = 0;					/* 从下一张位图页的开头扫描 */
		bit = 0;

		/* 重新取得元页上的独占锁 */
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	}

	/*
	 * 没有空闲页——必须扩展该关系才能加入一张溢出页。
	 * 首先检查是否还要增加一张新的位图页。
	 */
	if (last_bit == (uint32) (BMPGSZ_BIT(metap) - 1))
	{
		/*
		 * 创建新位图页时，把所有页都标为 "使用中"。
		 * 实际上新位图范围内会立刻存在两页：
		 * 位图页本身，以及紧随其后的那一页，
		 * 也就是返回给调用者的那一页。这两页都被正确地
		 * 标为 "使用中"。后续页尚不存在，但预先
		 * 把它们也标为 "使用中" 更方便。
		 */
		bit = metap->hashm_spares[splitnum];

		/* 元页已经持有写锁 */
		if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of overflow pages in hash index \"%s\"",
							RelationGetRelationName(rel))));

		newmapbuf = _hash_getnewbuf(rel, bitno_to_blkno(metap, bit), MAIN_FORKNUM);
	}
	else
	{
		/*
		 * 这里无事可做；因为该页将位于最后一张已用页之后，
		 * 我们知道它的位图位已被预先初始化为 "使用中"。
		 */
	}

	/* 计算新溢出页的地址 */
	bit = BufferIsValid(newmapbuf) ?
		metap->hashm_spares[splitnum] + 1 : metap->hashm_spares[splitnum];
	blkno = bitno_to_blkno(metap, bit);

	/*
	 * 用 _hash_getnewbuf 取出该页，以确保 smgr 对关系长度的
	 * 看法与我们保持同步。XXX 在持有元页写锁时做这件事
	 * 很烦人；更好的做法是改用一把不会阻塞
	 * 后续搜索的锁。
	 *
	 * 这里可以同时持有两把缓冲区锁（一把在桶尾页上，
	 * 另一把在新溢出页上），因为此时无法有其他人
	 * 争用对 ovflbuf 的访问。
	 */
	ovflbuf = _hash_getnewbuf(rel, blkno, MAIN_FORKNUM);

found:

	/*
	 * 执行更新。在变更记入日志之前不要调用 ereport(ERROR)。
	 * 我们希望把位图页和溢出页的变更一起记入日志，以免
	 * 在加入新页时丢失页。
	 */
	START_CRIT_SECTION();

	if (page_found)
	{
		Assert(BufferIsValid(mapbuf));

		/* 在位图中把该页标为 "使用中" */
		SETBIT(freep, bitmap_page_bit);
		MarkBufferDirty(mapbuf);
	}
	else
	{
		/* 更新计数，表示已加入新的溢出页 */
		metap->hashm_spares[splitnum]++;

		if (BufferIsValid(newmapbuf))
		{
			_hash_initbitmapbuffer(newmapbuf, metap->hashm_bmsize, false);
			MarkBufferDirty(newmapbuf);

			/* 把新位图页加入元页的位图列表 */
			metap->hashm_mapp[metap->hashm_nmaps] = BufferGetBlockNumber(newmapbuf);
			metap->hashm_nmaps++;
			metap->hashm_spares[splitnum]++;
		}

		MarkBufferDirty(metabuf);

		/*
		 * 对于新的溢出页，不必在位图页中显式置位，
		 * 因为默认就会被设为 "使用中"。
		 */
	}

	/*
	 * 调整 hashm_firstfree，以避免重复搜索。但若有人在我们
	 * 搜索位图页期间移动了它，就不要冒险去改它。
	 */
	if (metap->hashm_firstfree == orig_firstfree)
	{
		metap->hashm_firstfree = bit + 1;
		MarkBufferDirty(metabuf);
	}

	/* 初始化新的溢出页 */
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = HashPageGetOpaque(ovflpage);
	ovflopaque->hasho_prevblkno = BufferGetBlockNumber(buf);
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = pageopaque->hasho_bucket;
	ovflopaque->hasho_flag = LH_OVERFLOW_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(ovflbuf);

	/* 把溢出页逻辑链接到前一页 */
	pageopaque->hasho_nextblkno = BufferGetBlockNumber(ovflbuf);

	MarkBufferDirty(buf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_hash_add_ovfl_page xlrec;

		xlrec.bmpage_found = page_found;
		xlrec.bmsize = metap->hashm_bmsize;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashAddOvflPage);

		XLogRegisterBuffer(0, ovflbuf, REGBUF_WILL_INIT);
		XLogRegisterBufData(0, (char *) &pageopaque->hasho_bucket, sizeof(Bucket));

		XLogRegisterBuffer(1, buf, REGBUF_STANDARD);

		if (BufferIsValid(mapbuf))
		{
			XLogRegisterBuffer(2, mapbuf, REGBUF_STANDARD);
			XLogRegisterBufData(2, (char *) &bitmap_page_bit, sizeof(uint32));
		}

		if (BufferIsValid(newmapbuf))
			XLogRegisterBuffer(3, newmapbuf, REGBUF_WILL_INIT);

		XLogRegisterBuffer(4, metabuf, REGBUF_STANDARD);
		XLogRegisterBufData(4, (char *) &metap->hashm_firstfree, sizeof(uint32));

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_ADD_OVFL_PAGE);

		PageSetLSN(BufferGetPage(ovflbuf), recptr);
		PageSetLSN(BufferGetPage(buf), recptr);

		if (BufferIsValid(mapbuf))
			PageSetLSN(BufferGetPage(mapbuf), recptr);

		if (BufferIsValid(newmapbuf))
			PageSetLSN(BufferGetPage(newmapbuf), recptr);

		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	if (retain_pin)
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, buf);

	if (BufferIsValid(mapbuf))
		_hash_relbuf(rel, mapbuf);

	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	if (BufferIsValid(newmapbuf))
		_hash_relbuf(rel, newmapbuf);

	return ovflbuf;
}

/*
 *	_hash_firstfreebit()
 *
 *	返回字 'map' 中第一个未被置位的位编号。
 */
static uint32
_hash_firstfreebit(uint32 map)
{
	uint32		i,
				mask;

	mask = 0x1;
	for (i = 0; i < BITS_PER_MAP; i++)
	{
		if (!(mask & map))
			return i;
		mask <<= 1;
	}

	elog(ERROR, "firstfreebit found no free bit");

	return 0;					/* 让编译器保持安静 */
}

/*
 *	_hash_freeovflpage() -
 *
 *	把这张溢出页从它的桶链中移除，并把该页标为
 *	空闲。进入时 ovflbuf 已加写锁；退出前会释放它。
 *
 *	在本函数里把元组 (itups) 加入 wbuf。本来也可以在
 *	调用者中做，但放在这里的好处是便于写入
 *	XLOG_HASH_SQUEEZE_PAGE 操作的 WAL。元组的加入与
 *	溢出页的移除必须作为一次原子操作完成，否则
 *	在备库上重放时，用户可能会看到重复条目。
 *
 *	本函数在 VACUUM 中被调用，因此我们提供一个访问策略
 *	参数，用来控制对桶页的读取。
 *
 *	返回桶中紧跟在给定页之后的那一页的块号；
 *	若没有后继页，则返回 InvalidBlockNumber。
 *
 *	注意：调用者不能持有元页上的锁，也不能持有紧挨
 *	ovflbuf 之后那一页上的锁。若链中位于 ovflbuf 之前的页
 *	与 wbuf 相同，我们不去取得它的锁，因为调用者已经
 *	持有同一把锁。
 */
BlockNumber
_hash_freeovflpage(Relation rel, Buffer bucketbuf, Buffer ovflbuf,
				   Buffer wbuf, IndexTuple *itups, OffsetNumber *itup_offsets,
				   Size *tups_size, uint16 nitups,
				   BufferAccessStrategy bstrategy)
{
	HashMetaPage metap;
	Buffer		metabuf;
	Buffer		mapbuf;
	BlockNumber ovflblkno;
	BlockNumber prevblkno;
	BlockNumber blkno;
	BlockNumber nextblkno;
	BlockNumber writeblkno;
	HashPageOpaque ovflopaque;
	Page		ovflpage;
	Page		mappage;
	uint32	   *freep;
	uint32		ovflbitno;
	int32		bitmappage,
				bitmapbit;
	Bucket		bucket PG_USED_FOR_ASSERTS_ONLY;
	Buffer		prevbuf = InvalidBuffer;
	Buffer		nextbuf = InvalidBuffer;
	bool		update_metap = false;

	/* 从即将释放的页上取得信息 */
	_hash_checkpage(rel, ovflbuf, LH_OVERFLOW_PAGE);
	ovflblkno = BufferGetBlockNumber(ovflbuf);
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = HashPageGetOpaque(ovflpage);
	nextblkno = ovflopaque->hasho_nextblkno;
	prevblkno = ovflopaque->hasho_prevblkno;
	writeblkno = BufferGetBlockNumber(wbuf);
	bucket = ovflopaque->hasho_bucket;

	/*
	 * 修复桶链。这是一条双向链表，因此必须
	 * 修复被删除溢出页前后的桶链成员。
	 * 并发问题靠锁链避免，具体做法见
	 * hashbucketcleanup 顶部的说明。
	 */
	if (BlockNumberIsValid(prevblkno))
	{
		if (prevblkno == writeblkno)
			prevbuf = wbuf;
		else
			prevbuf = _hash_getbuf_with_strategy(rel,
												 prevblkno,
												 HASH_WRITE,
												 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE,
												 bstrategy);
	}
	if (BlockNumberIsValid(nextblkno))
		nextbuf = _hash_getbuf_with_strategy(rel,
											 nextblkno,
											 HASH_WRITE,
											 LH_OVERFLOW_PAGE,
											 bstrategy);

	/* 注意：有意不把 bstrategy 用于元页和位图页 */

	/* 读取元页，以便确定使用哪张位图页 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	/* 确定要设置的是哪一位 */
	ovflbitno = _hash_ovflblkno_to_bitno(metap, ovflblkno);

	bitmappage = ovflbitno >> BMPG_SHIFT(metap);
	bitmapbit = ovflbitno & BMPG_MASK(metap);

	if (bitmappage >= metap->hashm_nmaps)
		elog(ERROR, "invalid overflow bit number %u", ovflbitno);
	blkno = metap->hashm_mapp[bitmappage];

	/* 访问位图页时释放元页锁 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/* 读取位图页以清除位图位 */
	mapbuf = _hash_getbuf(rel, blkno, HASH_WRITE, LH_BITMAP_PAGE);
	mappage = BufferGetPage(mapbuf);
	freep = HashPageGetBitmap(mappage);
	Assert(ISSET(freep, bitmapbit));

	/* 取得元页上的写锁以更新 firstfree */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/* 本操作要把多个元组记入 WAL，为此先做准备 */
	if (RelationNeedsWAL(rel))
		XLogEnsureRecordSpace(HASH_XLOG_FREE_OVFL_BUFS, 4 + nitups);

	START_CRIT_SECTION();

	/*
	 * 我们必须把元组插入 "写" 页，同时小心保持
	 * 哈希键顺序。（如果向同一张 "写" 页插入许多元组，
	 * 值得先对它们做 qsort）。
	 */
	if (nitups > 0)
	{
		_hash_pgaddmultitup(rel, wbuf, itups, itup_offsets, nitups);
		MarkBufferDirty(wbuf);
	}

	/*
	 * 重新初始化已释放的溢出页。仅仅把该页清零是不行的，
	 * 因为 WAL 重放例程预期这些页已经初始化。参见
	 * XLogReadBufferExtended 顶部对 RBM_NORMAL 模式的说明。我们
	 * 小心地让这里的特殊空间保持有效，以免 pageinspect
	 * 之类的工具被弄糊涂。
	 */
	_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));

	ovflopaque = HashPageGetOpaque(ovflpage);

	ovflopaque->hasho_prevblkno = InvalidBlockNumber;
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = InvalidBucket;
	ovflopaque->hasho_flag = LH_UNUSED_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(ovflbuf);

	if (BufferIsValid(prevbuf))
	{
		Page		prevpage = BufferGetPage(prevbuf);
		HashPageOpaque prevopaque = HashPageGetOpaque(prevpage);

		Assert(prevopaque->hasho_bucket == bucket);
		prevopaque->hasho_nextblkno = nextblkno;
		MarkBufferDirty(prevbuf);
	}
	if (BufferIsValid(nextbuf))
	{
		Page		nextpage = BufferGetPage(nextbuf);
		HashPageOpaque nextopaque = HashPageGetOpaque(nextpage);

		Assert(nextopaque->hasho_bucket == bucket);
		nextopaque->hasho_prevblkno = prevblkno;
		MarkBufferDirty(nextbuf);
	}

	/* 清除位图位，表示该溢出页空闲 */
	CLRBIT(freep, bitmapbit);
	MarkBufferDirty(mapbuf);

	/* 若这现在是第一个空闲页，则更新 hashm_firstfree */
	if (ovflbitno < metap->hashm_firstfree)
	{
		metap->hashm_firstfree = ovflbitno;
		update_metap = true;
		MarkBufferDirty(metabuf);
	}

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_squeeze_page xlrec;
		XLogRecPtr	recptr;
		int			i;

		xlrec.prevblkno = prevblkno;
		xlrec.nextblkno = nextblkno;
		xlrec.ntups = nitups;
		xlrec.is_prim_bucket_same_wrt = (wbuf == bucketbuf);
		xlrec.is_prev_bucket_same_wrt = (wbuf == prevbuf);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashSqueezePage);

		/*
		 * 桶缓冲区需要登记，以确保重放期间
		 * 我们能对它取得清理锁。
		 */
		if (!xlrec.is_prim_bucket_same_wrt)
			XLogRegisterBuffer(0, bucketbuf, REGBUF_STANDARD | REGBUF_NO_IMAGE);

		XLogRegisterBuffer(1, wbuf, REGBUF_STANDARD);
		if (xlrec.ntups > 0)
		{
			XLogRegisterBufData(1, (char *) itup_offsets,
								nitups * sizeof(OffsetNumber));
			for (i = 0; i < nitups; i++)
				XLogRegisterBufData(1, (char *) itups[i], tups_size[i]);
		}

		XLogRegisterBuffer(2, ovflbuf, REGBUF_STANDARD);

		/*
		 * 若 prevpage 与 writepage（正在把元组从溢出页
		 * 移入的那个块）是同一页，就不必单独登记
		 * prevpage。重放期间可以直接更新 writepage
		 * 中的 nextblock。
		 */
		if (BufferIsValid(prevbuf) && !xlrec.is_prev_bucket_same_wrt)
			XLogRegisterBuffer(3, prevbuf, REGBUF_STANDARD);

		if (BufferIsValid(nextbuf))
			XLogRegisterBuffer(4, nextbuf, REGBUF_STANDARD);

		XLogRegisterBuffer(5, mapbuf, REGBUF_STANDARD);
		XLogRegisterBufData(5, (char *) &bitmapbit, sizeof(uint32));

		if (update_metap)
		{
			XLogRegisterBuffer(6, metabuf, REGBUF_STANDARD);
			XLogRegisterBufData(6, (char *) &metap->hashm_firstfree, sizeof(uint32));
		}

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SQUEEZE_PAGE);

		PageSetLSN(BufferGetPage(wbuf), recptr);
		PageSetLSN(BufferGetPage(ovflbuf), recptr);

		if (BufferIsValid(prevbuf) && !xlrec.is_prev_bucket_same_wrt)
			PageSetLSN(BufferGetPage(prevbuf), recptr);
		if (BufferIsValid(nextbuf))
			PageSetLSN(BufferGetPage(nextbuf), recptr);

		PageSetLSN(BufferGetPage(mapbuf), recptr);

		if (update_metap)
			PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	/* 若前一桶与写桶不同，则释放前一桶 */
	if (BufferIsValid(prevbuf) && prevblkno != writeblkno)
		_hash_relbuf(rel, prevbuf);

	if (BufferIsValid(ovflbuf))
		_hash_relbuf(rel, ovflbuf);

	if (BufferIsValid(nextbuf))
		_hash_relbuf(rel, nextbuf);

	_hash_relbuf(rel, mapbuf);
	_hash_relbuf(rel, metabuf);

	return nextblkno;
}


/*
 *	_hash_initbitmapbuffer()
 *
 *	 初始化一张新的位图页。新位图页中的所有位都设为
 *	 "1"，表示 "使用中"。
 */
void
_hash_initbitmapbuffer(Buffer buf, uint16 bmsize, bool initpage)
{
	Page		pg;
	HashPageOpaque op;
	uint32	   *freep;

	pg = BufferGetPage(buf);

	/* 初始化该页 */
	if (initpage)
		_hash_pageinit(pg, BufferGetPageSize(buf));

	/* 初始化该页的特殊空间 */
	op = HashPageGetOpaque(pg);
	op->hasho_prevblkno = InvalidBlockNumber;
	op->hasho_nextblkno = InvalidBlockNumber;
	op->hasho_bucket = InvalidBucket;
	op->hasho_flag = LH_BITMAP_PAGE;
	op->hasho_page_id = HASHO_PAGE_ID;

	/* 把所有位都设为 1 */
	freep = HashPageGetBitmap(pg);
	memset(freep, 0xFF, bmsize);

	/*
	 * 把 pd_lower 设到位图页数据末尾刚过去的位置。我们甚至可以
	 * 把 pd_lower 设成等于 pd_upper，但这样更精确，并让
	 * 该页在 xlog.c 看来是可压缩的。
	 */
	((PageHeader) pg)->pd_lower = ((char *) freep + bmsize) - (char *) pg;
}


/*
 *	_hash_squeezebucket(rel, bucket)
 *
 *	尝试把元组挤压到桶链中更早出现的那些页上，
 *	以便把溢出页腾出来。开始 "挤压" 时，
 *	我们从中取出元组的那一页（"读" 页）
 *	是桶链里的最后一页；开始把元组挤上去的那一页
 *	（"写" 页）则是桶链里的第一页。
 *	读页向后走，写页向前走；
 *	当读页和写页走到同一页时，
 *	这个过程就终止。
 *
 *	本过程完成时，保证桶中每一页都非空，
 *	除非该桶完全为空（这种情况下所有溢出页都会被释放）。
 *	原先的实现还要求进入时也满足这一点，
 *	但让调用者留下空的溢出页、
 *	再由本函数来清理，要容易得多。
 *
 *	调用者必须在目标桶的主页上取得清理锁，
 *	以排除任何正在进行的扫描。这里的重排很容易
 *	让扫描把同一元组返回多次，或者让某些元组
 *	根本不被返回。为防止并发扫描越过挤压扫描，
 *	我们使用与 hashbucketcleanup 类似的锁链。
 *	请参见 hashbucketcleanup 顶部的注释。
 *
 *	我们需要保持主桶上的钉住，以确保不会有
 *	并发分裂开始。
 *
 *	由于本函数在 VACUUM 中被调用，我们提供一个访问策略
 *	参数，用来控制对桶页的读取。
 */
void
_hash_squeezebucket(Relation rel,
					Bucket bucket,
					BlockNumber bucket_blkno,
					Buffer bucket_buf,
					BufferAccessStrategy bstrategy)
{
	BlockNumber wblkno;
	BlockNumber rblkno;
	Buffer		wbuf;
	Buffer		rbuf;
	Page		wpage;
	Page		rpage;
	HashPageOpaque wopaque;
	HashPageOpaque ropaque;

	/*
	 * 开始向主桶页挤压。
	 */
	wblkno = bucket_blkno;
	wbuf = bucket_buf;
	wpage = BufferGetPage(wbuf);
	wopaque = HashPageGetOpaque(wpage);

	/*
	 * 若没有任何溢出页，就没有什么可挤压的。调用者
	 * 负责释放主桶页上的钉住。
	 */
	if (!BlockNumberIsValid(wopaque->hasho_nextblkno))
	{
		LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
		return;
	}

	/*
	 * 从基本桶页出发向前走，找到桶链中的最后一页。
	 * 注意：我们假定 hash 桶链通常小于 VACUUM 正在使用的
	 * 缓冲区环，否则在这里使用访问策略
	 * 会适得其反。
	 */
	rbuf = InvalidBuffer;
	ropaque = wopaque;
	do
	{
		rblkno = ropaque->hasho_nextblkno;
		if (rbuf != InvalidBuffer)
			_hash_relbuf(rel, rbuf);
		rbuf = _hash_getbuf_with_strategy(rel,
										  rblkno,
										  HASH_WRITE,
										  LH_OVERFLOW_PAGE,
										  bstrategy);
		rpage = BufferGetPage(rbuf);
		ropaque = HashPageGetOpaque(rpage);
		Assert(ropaque->hasho_bucket == bucket);
	} while (BlockNumberIsValid(ropaque->hasho_nextblkno));

	/*
	 * 挤压这些元组。
	 */
	for (;;)
	{
		OffsetNumber roffnum;
		OffsetNumber maxroffnum;
		OffsetNumber deletable[MaxOffsetNumber];
		IndexTuple	itups[MaxIndexTuplesPerPage];
		Size		tups_size[MaxIndexTuplesPerPage];
		OffsetNumber itup_offsets[MaxIndexTuplesPerPage];
		uint16		ndeletable = 0;
		uint16		nitups = 0;
		Size		all_tups_size = 0;
		int			i;
		bool		retain_pin = false;

readpage:
		/* 扫描 "读" 页中的每个元组 */
		maxroffnum = PageGetMaxOffsetNumber(rpage);
		for (roffnum = FirstOffsetNumber;
			 roffnum <= maxroffnum;
			 roffnum = OffsetNumberNext(roffnum))
		{
			IndexTuple	itup;
			Size		itemsz;

			/* 跳过死元组 */
			if (ItemIdIsDead(PageGetItemId(rpage, roffnum)))
				continue;

			itup = (IndexTuple) PageGetItem(rpage,
											PageGetItemId(rpage, roffnum));
			itemsz = IndexTupleSize(itup);
			itemsz = MAXALIGN(itemsz);

			/*
			 * 沿桶链向上走，寻找一张放得下本项
			 * 以及所有其他已累积项的页。若到达
			 * 读页则退出。
			 */
			while (PageGetFreeSpaceForMultipleTuples(wpage, nitups + 1) < (all_tups_size + itemsz))
			{
				Buffer		next_wbuf = InvalidBuffer;
				bool		tups_moved = false;

				Assert(!PageIsEmpty(wpage));

				if (wblkno == bucket_blkno)
					retain_pin = true;

				wblkno = wopaque->hasho_nextblkno;
				Assert(BlockNumberIsValid(wblkno));

				/* 若已到达读页，则不必移到下一页 */
				if (wblkno != rblkno)
					next_wbuf = _hash_getbuf_with_strategy(rel,
														   wblkno,
														   HASH_WRITE,
														   LH_OVERFLOW_PAGE,
														   bstrategy);

				if (nitups > 0)
				{
					Assert(nitups == ndeletable);

					/*
					 * 本操作要把多个元组记入 WAL，
					 * 为此先做准备。
					 */
					if (RelationNeedsWAL(rel))
						XLogEnsureRecordSpace(0, 3 + nitups);

					START_CRIT_SECTION();

					/*
					 * 我们必须把元组插入 "写" 页，同时
					 * 小心保持哈希键顺序。（若向同一张
					 * "写" 页插入许多元组，值得
					 * 先对它们做 qsort）。
					 */
					_hash_pgaddmultitup(rel, wbuf, itups, itup_offsets, nitups);
					MarkBufferDirty(wbuf);

					/* 删除已经从读页移走的元组 */
					PageIndexMultiDelete(rpage, deletable, ndeletable);
					MarkBufferDirty(rbuf);

					/* XLOG 相关 */
					if (RelationNeedsWAL(rel))
					{
						XLogRecPtr	recptr;
						xl_hash_move_page_contents xlrec;

						xlrec.ntups = nitups;
						xlrec.is_prim_bucket_same_wrt = (wbuf == bucket_buf);

						XLogBeginInsert();
						XLogRegisterData((char *) &xlrec, SizeOfHashMovePageContents);

						/*
						 * 桶缓冲区需要登记，以确保
						 * 重放期间我们能对它取得清理锁。
						 */
						if (!xlrec.is_prim_bucket_same_wrt)
							XLogRegisterBuffer(0, bucket_buf, REGBUF_STANDARD | REGBUF_NO_IMAGE);

						XLogRegisterBuffer(1, wbuf, REGBUF_STANDARD);
						XLogRegisterBufData(1, (char *) itup_offsets,
											nitups * sizeof(OffsetNumber));
						for (i = 0; i < nitups; i++)
							XLogRegisterBufData(1, (char *) itups[i], tups_size[i]);

						XLogRegisterBuffer(2, rbuf, REGBUF_STANDARD);
						XLogRegisterBufData(2, (char *) deletable,
											ndeletable * sizeof(OffsetNumber));

						recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_MOVE_PAGE_CONTENTS);

						PageSetLSN(BufferGetPage(wbuf), recptr);
						PageSetLSN(BufferGetPage(rbuf), recptr);
					}

					END_CRIT_SECTION();

					tups_moved = true;
				}

				/*
				 * 在取得下一页的锁之后，
				 * 再释放前一页上的锁
				 */
				if (retain_pin)
					LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
				else
					_hash_relbuf(rel, wbuf);

				/* 若已到达读页，则没有更多事情要做 */
				if (rblkno == wblkno)
				{
					_hash_relbuf(rel, rbuf);
					return;
				}

				wbuf = next_wbuf;
				wpage = BufferGetPage(wbuf);
				wopaque = HashPageGetOpaque(wpage);
				Assert(wopaque->hasho_bucket == bucket);
				retain_pin = false;

				/* 保持整洁 */
				for (i = 0; i < nitups; i++)
					pfree(itups[i]);
				nitups = 0;
				all_tups_size = 0;
				ndeletable = 0;

				/*
				 * 移走这些元组之后，rpage 已经被压紧，
				 * 因此需要重新扫描它。
				 */
				if (tups_moved)
					goto readpage;
			}

			/* 记住要从 "读" 页删除的元组 */
			deletable[ndeletable++] = roffnum;

			/*
			 * 需要一份索引元组副本，因为它们可能作为
			 * 溢出页的一部分被释放，但在 _hash_freeovflpage
			 * 里写 WAL 时还要用到它们。
			 */
			itups[nitups] = CopyIndexTuple(itup);
			tups_size[nitups++] = itemsz;
			all_tups_size += itemsz;
		}

		/*
		 * 若到达这里，"读" 页上已经没有活元组——
		 * 要么我们到达时它就是空的，要么已经全部移走。因此可以
		 * 直接释放该页，不必逐个删除元组。
		 * 然后前进到前一张 "读" 页。
		 *
		 * 这里有个微妙之处：若读页与写页在桶链里相邻，
		 * 我们持有的 wbuf 写锁会和 _hash_freeovflpage
		 * 更新被移除页兄弟链接的尝试相冲突。
		 * 这种情况下不需要再次给它加锁。
		 */
		rblkno = ropaque->hasho_prevblkno;
		Assert(BlockNumberIsValid(rblkno));

		/* 释放该溢出页（同时释放 rbuf） */
		_hash_freeovflpage(rel, bucket_buf, rbuf, wbuf, itups, itup_offsets,
						   tups_size, nitups, bstrategy);

		/* 保持整洁 */
		for (i = 0; i < nitups; i++)
			pfree(itups[i]);

		/* 正在释放的是否是紧挨 wbuf 的页？ */
		if (rblkno == wblkno)
		{
			/* 直到桶扫描结束都保持主桶页上的钉住 */
			if (wblkno == bucket_blkno)
				LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
			else
				_hash_relbuf(rel, wbuf);
			return;
		}

		rbuf = _hash_getbuf_with_strategy(rel,
										  rblkno,
										  HASH_WRITE,
										  LH_OVERFLOW_PAGE,
										  bstrategy);
		rpage = BufferGetPage(rbuf);
		ropaque = HashPageGetOpaque(rpage);
		Assert(ropaque->hasho_bucket == bucket);
	}

	/* 不可达 (NOTREACHED) */
}
