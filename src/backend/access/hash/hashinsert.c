/*-------------------------------------------------------------------------
 *
 * hashinsert.c
 *	  Postgres hash 表中的项插入。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "storage/predicate.h"
#include "utils/rel.h"

static void _hash_vacuum_one_page(Relation rel, Relation hrel,
								  Buffer metabuf, Buffer buf);

/*
 *	_hash_doinsert() -- 处理单个索引元组的插入。
 *
 *		本例程由公共接口例程 hashbuild
 *		和 hashinsert 调用。到这里时，itup 已完全填好。
 *
 * 仅当插入按 hashkey 顺序进行时，才可以把
 * 'sorted' 作为 'true' 传入。
 */
void
_hash_doinsert(Relation rel, IndexTuple itup, Relation heapRel, bool sorted)
{
	Buffer		buf = InvalidBuffer;
	Buffer		bucket_buf;
	Buffer		metabuf;
	HashMetaPage metap;
	HashMetaPage usedmetap = NULL;
	Page		metapage;
	Page		page;
	HashPageOpaque pageopaque;
	Size		itemsz;
	bool		do_expand;
	uint32		hashkey;
	Bucket		bucket;
	OffsetNumber itup_off;

	/*
	 * 取得该项的哈希键（它存放在索引元组自身中）。
	 */
	hashkey = _hash_get_indextuple_hashkey(itup);

	/* 同时计算项的大小 */
	itemsz = IndexTupleSize(itup);
	itemsz = MAXALIGN(itemsz);	/* 保险起见，PageAddItem 也会这样做，但我们
								 * 需要保持一致 */

restart_insert:

	/*
	 * 读取元页。我们尚未给它加锁；HashMaxItemSize() 会
	 * 检查 pd_pagesize_version，但它不会改变，因此可以
	 * 不加锁地查看。
	 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);
	metapage = BufferGetPage(metabuf);

	/*
	 * 检查该项究竟能否放进 hash 页。（若不能，最终
	 * 应当尝试采用 TOAST 方法。）注意此时
	 * itemsz 不包含 ItemId。
	 *
	 * XXX 若我们只存储哈希键，这段代码就没有用。
	 */
	if (itemsz > HashMaxItemSize(metapage))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds hash maximum %zu",
						itemsz, HashMaxItemSize(metapage)),
				 errhint("Values larger than a buffer page cannot be indexed.")));

	/* 锁定目标桶的主桶页。 */
	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_WRITE,
										  &usedmetap);
	Assert(usedmetap != NULL);

	CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(buf));

	/* 记住主桶缓冲区，以便最后释放其上的钉住。 */
	bucket_buf = buf;

	page = BufferGetPage(buf);
	pageopaque = HashPageGetOpaque(page);
	bucket = pageopaque->hasho_bucket;

	/*
	 * 若此桶正在分裂，尝试在插入之前完成这次
	 * 分裂，因为那样可能腾出空间，使插入不必
	 * 再分配额外的溢出页就能进行。只有当我们试图
	 * 插入到正在从中移出元组的桶
	 * （"旧" 桶）时，完成分裂才有意义；若试图
	 * 插入到元组正在移入的桶
	 * （"新" 桶），则没有意义。
	 */
	if (H_BUCKET_BEING_SPLIT(pageopaque) && IsBufferCleanupOK(buf))
	{
		/* 完成分裂之前，先释放桶缓冲区上的锁。 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		_hash_finish_split(rel, metabuf, buf, bucket,
						   usedmetap->hashm_maxbucket,
						   usedmetap->hashm_highmask,
						   usedmetap->hashm_lowmask);

		/* 释放旧缓冲区和元页缓冲区上的钉住。重试插入。 */
		_hash_dropbuf(rel, buf);
		_hash_dropbuf(rel, metabuf);
		goto restart_insert;
	}

	/* 执行插入 */
	while (PageGetFreeSpace(page) < itemsz)
	{
		BlockNumber nextblkno;

		/*
		 * 检查当前页是否有 DEAD 元组。若有，删掉这些
		 * 元组，看看在移到桶链的下一页之前，能否为
		 * 待插入的新项腾出空间。
		 */
		if (H_HAS_DEAD_TUPLES(pageopaque))
		{

			if (IsBufferCleanupOK(buf))
			{
				_hash_vacuum_one_page(rel, heapRel, metabuf, buf);

				if (PageGetFreeSpace(page) >= itemsz)
					break;		/* 好，现在空间够了 */
			}
		}

		/*
		 * 此页没有空间；检查是否有溢出页
		 */
		nextblkno = pageopaque->hasho_nextblkno;

		if (BlockNumberIsValid(nextblkno))
		{
			/*
			 * 溢出页存在；去取它。若它也放不下，下一轮
			 * 上面的循环测试会发现。若这是溢出页，我们总是
			 * 同时释放锁和钉住；若这是主桶页，则只释放锁，
			 * 因为整个扫描期间必须保持主桶上的
			 * 钉住。
			 */
			if (buf != bucket_buf)
				_hash_relbuf(rel, buf);
			else
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
			page = BufferGetPage(buf);
		}
		else
		{
			/*
			 * 已到桶链末尾，仍未找到空间足够的页。
			 * 分配一个新的溢出页。
			 */

			/* 释放写锁，但不修改缓冲区 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* 链接到一个新的溢出页 */
			buf = _hash_addovflpage(rel, metabuf, buf, (buf == bucket_buf));
			page = BufferGetPage(buf);

			/* 鉴于上面的测试，现在应该放得下 */
			Assert(PageGetFreeSpace(page) >= itemsz);
		}
		pageopaque = HashPageGetOpaque(page);
		Assert((pageopaque->hasho_flag & LH_PAGE_TYPE) == LH_OVERFLOW_PAGE);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	/*
	 * 对元页加写锁，以便增加元组计数。增加之后，
	 * 检查是否到了分裂的时候。
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/* 执行更新。在变更记入日志之前不要 ereport(ERROR) */
	START_CRIT_SECTION();

	/* 找到空间足够的页，把项加在这里 */
	itup_off = _hash_pgaddtup(rel, buf, itemsz, itup, sorted);
	MarkBufferDirty(buf);

	/* 元页操作 */
	metap = HashPageGetMeta(metapage);
	metap->hashm_ntuples += 1;

	/* 确保与 _hash_expandtable() 保持同步 */
	do_expand = metap->hashm_ntuples >
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1);

	MarkBufferDirty(metabuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_insert xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = itup_off;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInsert);

		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) itup, IndexTupleSize(itup));

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INSERT);

		PageSetLSN(BufferGetPage(buf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	/* 释放元页上的锁，但保持钉住 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * 释放被修改的页，并确保释放
	 * 主桶页上的钉住。
	 */
	_hash_relbuf(rel, buf);
	if (buf != bucket_buf)
		_hash_dropbuf(rel, bucket_buf);

	/* 若需要分裂则尝试分裂 */
	if (do_expand)
		_hash_expandtable(rel, metabuf);

	/* 最后释放我们在元页上的钉住 */
	_hash_dropbuf(rel, metabuf);
}

/*
 *	_hash_pgaddtup() -- 把一个元组加到索引的某一页上。
 *
 * 本例程按要求把元组加到页上；它并不把该页
 * 写出去。未在目标缓冲区上持有钉住和写锁
 * 就调用本函数是错误的。
 *
 * 返回元组被插入处的偏移号。本函数负责
 * 维持 hash 索引页中元组按 hashkey 值排序
 * 的条件；不过，若调用方确定待加入元组的
 * hashkey >= 页上全部已有元组的 hashkey，
 * 则可以把 'appendtup' 标志作为 true 传入。
 * 这样就能省去为找到正确插入位置
 * 而做的二分搜索。
 */
OffsetNumber
_hash_pgaddtup(Relation rel, Buffer buf, Size itemsize, IndexTuple itup,
			   bool appendtup)
{
	OffsetNumber itup_off;
	Page		page;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	/*
	 * 寻找插入元组的位置（保持页内 hashkey 顺序）。若
	 * 'appendtup' 为真，就把它插在末尾。
	 */
	if (appendtup)
	{
		itup_off = PageGetMaxOffsetNumber(page) + 1;

#ifdef USE_ASSERT_CHECKING
		/* 确保此元组的 hashkey >= 页上最后一个已有元组 */
		if (PageGetMaxOffsetNumber(page) > 0)
		{
			IndexTuple	lasttup;
			ItemId		itemid;

			itemid = PageGetItemId(page, PageGetMaxOffsetNumber(page));
			lasttup = (IndexTuple) PageGetItem(page, itemid);

			Assert(_hash_get_indextuple_hashkey(lasttup) <=
				   _hash_get_indextuple_hashkey(itup));
		}
#endif
	}
	else
	{
		uint32		hashkey = _hash_get_indextuple_hashkey(itup);

		itup_off = _hash_binsearch(page, hashkey);
	}

	if (PageAddItem(page, (Item) itup, itemsize, itup_off, false, false)
		== InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(rel));

	return itup_off;
}

/*
 *	_hash_pgaddmultitup() -- 把一个元组向量加到索引的
 *							 某一页上。
 *
 * 本例程在锁定和元组排序方面的要求与
 * _hash_pgaddtup() 相同。
 *
 * 返回各元组被插入处的偏移号数组。
 */
void
_hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
					OffsetNumber *itup_offsets, uint16 nitups)
{
	OffsetNumber itup_off;
	Page		page;
	uint32		hashkey;
	int			i;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	for (i = 0; i < nitups; i++)
	{
		Size		itemsize;

		itemsize = IndexTupleSize(itups[i]);
		itemsize = MAXALIGN(itemsize);

		/* 寻找插入元组的位置（保持页内 hashkey 顺序） */
		hashkey = _hash_get_indextuple_hashkey(itups[i]);
		itup_off = _hash_binsearch(page, hashkey);

		itup_offsets[i] = itup_off;

		if (PageAddItem(page, (Item) itups[i], itemsize, itup_off, false, false)
			== InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(rel));
	}
}

/*
 * _hash_vacuum_one_page - 只清理一个索引页。
 *
 * 尝试从给定页移除 LP_DEAD 项。调用本函数之前，必须
 * 在将被修改的页上取得清理锁。
 */

static void
_hash_vacuum_one_page(Relation rel, Relation hrel, Buffer metabuf, Buffer buf)
{
	OffsetNumber deletable[MaxOffsetNumber];
	int			ndeletable = 0;
	OffsetNumber offnum,
				maxoff;
	Page		page = BufferGetPage(buf);
	HashPageOpaque pageopaque;
	HashMetaPage metap;

	/* 扫描页中每个元组，看它是否标为 LP_DEAD */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		TransactionId snapshotConflictHorizon;

		snapshotConflictHorizon =
			index_compute_xid_horizon_for_tuples(rel, hrel, buf,
												 deletable, ndeletable);

		/*
		 * 对元页加写锁，以便减少元组计数。
		 */
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

		/* 在变更记入日志之前不要 ereport(ERROR) */
		START_CRIT_SECTION();

		PageIndexMultiDelete(page, deletable, ndeletable);

		/*
		 * 把该页标为不含任何 LP_DEAD 项。这并不
		 * 一定为真（可能有一些刚被标上、但没进入
		 * 我们的目标项列表），但它几乎总是真的，
		 * 似乎不值得再扫描一页去检查。记住
		 * LH_PAGE_HAS_DEAD_TUPLES 反正只是一个
		 * 提示。
		 */
		pageopaque = HashPageGetOpaque(page);
		pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;

		metap = HashPageGetMeta(BufferGetPage(metabuf));
		metap->hashm_ntuples -= ndeletable;

		MarkBufferDirty(buf);
		MarkBufferDirty(metabuf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			xl_hash_vacuum_one_page xlrec;
			XLogRecPtr	recptr;

			xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(hrel);
			xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
			xlrec.ntuples = ndeletable;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			XLogRegisterData((char *) &xlrec, SizeOfHashVacuumOnePage);

			/*
			 * 无论是否存储整个缓冲区，我们都需要目标偏移
			 * 数组，以便在备库上找到
			 * snapshotConflictHorizon。
			 */
			XLogRegisterData((char *) deletable,
							 ndeletable * sizeof(OffsetNumber));

			XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_VACUUM_ONE_PAGE);

			PageSetLSN(BufferGetPage(buf), recptr);
			PageSetLSN(BufferGetPage(metabuf), recptr);
		}

		END_CRIT_SECTION();

		/*
		 * 元组计数已经更新，释放元页上的
		 * 写锁。
		 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
	}
}
