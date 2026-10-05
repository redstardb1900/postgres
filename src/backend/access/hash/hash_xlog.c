/*-------------------------------------------------------------------------
 *
 * hash_xlog.c
 *	  hash 索引的 WAL 重放逻辑。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hash_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"

/*
 * 重放 hash 索引元页
 */
static void
hash_xlog_init_meta_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Page		page;
	Buffer		metabuf;
	ForkNumber	forknum;

	xl_hash_init_meta_page *xlrec = (xl_hash_init_meta_page *) XLogRecGetData(record);

	/* 创建索引的元页 */
	metabuf = XLogInitBufferForRedo(record, 0);
	Assert(BufferIsValid(metabuf));
	_hash_init_metabuffer(metabuf, xlrec->num_tuples, xlrec->procid,
						  xlrec->ffactor, true);
	page = (Page) BufferGetPage(metabuf);
	PageSetLSN(page, lsn);
	MarkBufferDirty(metabuf);

	/*
	 * 强制使初始化分叉的磁盘状态始终与共享缓冲区中的
	 * 状态保持同步。参见 XLogReadBufferForRedoExtended。创建索引
	 * 操作不会记录元页的整页映像，因此初始化分叉
	 * 需要特殊处理。
	 */
	XLogRecGetBlockTag(record, 0, NULL, &forknum, NULL);
	if (forknum == INIT_FORKNUM)
		FlushOneBuffer(metabuf);

	/* 全部完成 */
	UnlockReleaseBuffer(metabuf);
}

/*
 * 重放 hash 索引位图页
 */
static void
hash_xlog_init_bitmap_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		bitmapbuf;
	Buffer		metabuf;
	Page		page;
	HashMetaPage metap;
	uint32		num_buckets;
	ForkNumber	forknum;

	xl_hash_init_bitmap_page *xlrec = (xl_hash_init_bitmap_page *) XLogRecGetData(record);

	/*
	 * 初始化位图页
	 */
	bitmapbuf = XLogInitBufferForRedo(record, 0);
	_hash_initbitmapbuffer(bitmapbuf, xlrec->bmsize, true);
	PageSetLSN(BufferGetPage(bitmapbuf), lsn);
	MarkBufferDirty(bitmapbuf);

	/*
	 * 强制使初始化分叉的磁盘状态始终与共享缓冲区中的
	 * 状态保持同步。参见 XLogReadBufferForRedoExtended。创建索引
	 * 操作不会记录元页的整页映像，因此初始化分叉
	 * 需要特殊处理。
	 */
	XLogRecGetBlockTag(record, 0, NULL, &forknum, NULL);
	if (forknum == INIT_FORKNUM)
		FlushOneBuffer(bitmapbuf);
	UnlockReleaseBuffer(bitmapbuf);

	/* 把新位图页加入元页的位图列表 */
	if (XLogReadBufferForRedo(record, 1, &metabuf) == BLK_NEEDS_REDO)
	{
		/*
		 * 注意：正常操作中，我们会在仍持有位图页锁的同时
		 * 更新元页。但重放期间不必持有该锁，因为还没有人
		 * 能看见它；创建该页的事务
		 * 尚未提交。
		 */
		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);

		num_buckets = metap->hashm_maxbucket + 1;
		metap->hashm_mapp[metap->hashm_nmaps] = num_buckets + 1;
		metap->hashm_nmaps++;

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);

		XLogRecGetBlockTag(record, 1, NULL, &forknum, NULL);
		if (forknum == INIT_FORKNUM)
			FlushOneBuffer(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * 重放不带分裂的 hash 索引插入
 */
static void
hash_xlog_insert(XLogReaderState *record)
{
	HashMetaPage metap;
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_insert *xlrec = (xl_hash_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *datapos = XLogRecGetBlockData(record, 0, &datalen);

		page = BufferGetPage(buffer);

		if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "hash_xlog_insert: failed to add item");

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
	{
		/*
		 * 注意：正常操作中，我们会在仍持有所插入页的锁时
		 * 更新元页。但重放期间不必持有那把锁，
		 * 因为不会有其他索引更新
		 * 并发进行。
		 */
		page = BufferGetPage(buffer);
		metap = HashPageGetMeta(page);
		metap->hashm_ntuples += 1;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * 重放为 hash 索引增加溢出页
 */
static void
hash_xlog_add_ovfl_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_add_ovfl_page *xlrec = (xl_hash_add_ovfl_page *) XLogRecGetData(record);
	Buffer		leftbuf;
	Buffer		ovflbuf;
	Buffer		metabuf;
	BlockNumber leftblk;
	BlockNumber rightblk;
	BlockNumber newmapblk = InvalidBlockNumber;
	Page		ovflpage;
	HashPageOpaque ovflopaque;
	uint32	   *num_bucket;
	char	   *data;
	Size		datalen PG_USED_FOR_ASSERTS_ONLY;
	bool		new_bmpage = false;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &rightblk);
	XLogRecGetBlockTag(record, 1, NULL, NULL, &leftblk);

	ovflbuf = XLogInitBufferForRedo(record, 0);
	Assert(BufferIsValid(ovflbuf));

	data = XLogRecGetBlockData(record, 0, &datalen);
	num_bucket = (uint32 *) data;
	Assert(datalen == sizeof(uint32));
	_hash_initbuf(ovflbuf, InvalidBlockNumber, *num_bucket, LH_OVERFLOW_PAGE,
				  true);
	/* 更新反向链接 */
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = HashPageGetOpaque(ovflpage);
	ovflopaque->hasho_prevblkno = leftblk;

	PageSetLSN(ovflpage, lsn);
	MarkBufferDirty(ovflbuf);

	if (XLogReadBufferForRedo(record, 1, &leftbuf) == BLK_NEEDS_REDO)
	{
		Page		leftpage;
		HashPageOpaque leftopaque;

		leftpage = BufferGetPage(leftbuf);
		leftopaque = HashPageGetOpaque(leftpage);
		leftopaque->hasho_nextblkno = rightblk;

		PageSetLSN(leftpage, lsn);
		MarkBufferDirty(leftbuf);
	}

	if (BufferIsValid(leftbuf))
		UnlockReleaseBuffer(leftbuf);
	UnlockReleaseBuffer(ovflbuf);

	/*
	 * 注意：正常操作中，我们会在仍持有溢出页上的锁时
	 * 更新位图页和元页。但重放期间不必持有那些锁，
	 * 因为不会有其他索引更新
	 * 并发进行。
	 */
	if (XLogRecHasBlockRef(record, 2))
	{
		Buffer		mapbuffer;

		if (XLogReadBufferForRedo(record, 2, &mapbuffer) == BLK_NEEDS_REDO)
		{
			Page		mappage = (Page) BufferGetPage(mapbuffer);
			uint32	   *freep = NULL;
			uint32	   *bitmap_page_bit;

			freep = HashPageGetBitmap(mappage);

			data = XLogRecGetBlockData(record, 2, &datalen);
			bitmap_page_bit = (uint32 *) data;

			SETBIT(freep, *bitmap_page_bit);

			PageSetLSN(mappage, lsn);
			MarkBufferDirty(mapbuffer);
		}
		if (BufferIsValid(mapbuffer))
			UnlockReleaseBuffer(mapbuffer);
	}

	if (XLogRecHasBlockRef(record, 3))
	{
		Buffer		newmapbuf;

		newmapbuf = XLogInitBufferForRedo(record, 3);

		_hash_initbitmapbuffer(newmapbuf, xlrec->bmsize, true);

		new_bmpage = true;
		newmapblk = BufferGetBlockNumber(newmapbuf);

		MarkBufferDirty(newmapbuf);
		PageSetLSN(BufferGetPage(newmapbuf), lsn);

		UnlockReleaseBuffer(newmapbuf);
	}

	if (XLogReadBufferForRedo(record, 4, &metabuf) == BLK_NEEDS_REDO)
	{
		HashMetaPage metap;
		Page		page;
		uint32	   *firstfree_ovflpage;

		data = XLogRecGetBlockData(record, 4, &datalen);
		firstfree_ovflpage = (uint32 *) data;

		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);
		metap->hashm_firstfree = *firstfree_ovflpage;

		if (!xlrec->bmpage_found)
		{
			metap->hashm_spares[metap->hashm_ovflpoint]++;

			if (new_bmpage)
			{
				Assert(BlockNumberIsValid(newmapblk));

				metap->hashm_mapp[metap->hashm_nmaps] = newmapblk;
				metap->hashm_nmaps++;
				metap->hashm_spares[metap->hashm_ovflpoint]++;
			}
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * 重放分裂操作的页分配
 */
static void
hash_xlog_split_allocate_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_split_allocate_page *xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(record);
	Buffer		oldbuf;
	Buffer		newbuf;
	Buffer		metabuf;
	Size		datalen PG_USED_FOR_ASSERTS_ONLY;
	char	   *data;
	XLogRedoAction action;

	/*
	 * 为与正常操作保持一致，这里对旧桶和新桶都取清理锁，
	 * 尽管不会有任何并发
	 * 插入。
	 */

	/* 重放旧桶的记录 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &oldbuf);

	/*
	 * 注意：即使该页是从整页映像恢复的，
	 * 我们仍要更新它，因为特殊空间不包含在映像中。
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		oldpage;
		HashPageOpaque oldopaque;

		oldpage = BufferGetPage(oldbuf);
		oldopaque = HashPageGetOpaque(oldpage);

		oldopaque->hasho_flag = xlrec->old_bucket_flag;
		oldopaque->hasho_prevblkno = xlrec->new_bucket;

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuf);
	}

	/* 重放新桶的记录 */
	XLogReadBufferForRedoExtended(record, 1, RBM_ZERO_AND_CLEANUP_LOCK, true,
								  &newbuf);
	_hash_initbuf(newbuf, xlrec->new_bucket, xlrec->new_bucket,
				  xlrec->new_bucket_flag, true);
	MarkBufferDirty(newbuf);
	PageSetLSN(BufferGetPage(newbuf), lsn);

	/*
	 * 我们也可以更早释放旧桶上的锁，但放在这里做
	 * 是为了与正常操作保持一致。
	 */
	if (BufferIsValid(oldbuf))
		UnlockReleaseBuffer(oldbuf);
	if (BufferIsValid(newbuf))
		UnlockReleaseBuffer(newbuf);

	/*
	 * 注意：正常操作中，我们会在仍持有旧桶页和新桶页锁时
	 * 更新元页。但重放期间不必持有那些锁，
	 * 因为不会有其他桶分裂
	 * 并发进行。
	 */

	/* 重放元页变更的记录 */
	if (XLogReadBufferForRedo(record, 2, &metabuf) == BLK_NEEDS_REDO)
	{
		Page		page;
		HashMetaPage metap;

		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);
		metap->hashm_maxbucket = xlrec->new_bucket;

		data = XLogRecGetBlockData(record, 2, &datalen);

		if (xlrec->flags & XLH_SPLIT_META_UPDATE_MASKS)
		{
			uint32		lowmask;
			uint32	   *highmask;

			/* 取出低位掩码和高位掩码。 */
			memcpy(&lowmask, data, sizeof(uint32));
			highmask = (uint32 *) ((char *) data + sizeof(uint32));

			/* 更新元页 */
			metap->hashm_lowmask = lowmask;
			metap->hashm_highmask = *highmask;

			data += sizeof(uint32) * 2;
		}

		if (xlrec->flags & XLH_SPLIT_META_UPDATE_SPLITPOINT)
		{
			uint32		ovflpoint;
			uint32	   *ovflpages;

			/* 取出溢出页的信息。 */
			memcpy(&ovflpoint, data, sizeof(uint32));
			ovflpages = (uint32 *) ((char *) data + sizeof(uint32));

			/* 更新元页 */
			metap->hashm_spares[ovflpoint] = *ovflpages;
			metap->hashm_ovflpoint = ovflpoint;
		}

		MarkBufferDirty(metabuf);
		PageSetLSN(BufferGetPage(metabuf), lsn);
	}

	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * 重放分裂操作
 */
static void
hash_xlog_split_page(XLogReaderState *record)
{
	Buffer		buf;

	if (XLogReadBufferForRedo(record, 0, &buf) != BLK_RESTORED)
		elog(ERROR, "Hash split record did not contain a full-page image");

	UnlockReleaseBuffer(buf);
}

/*
 * 重放分裂操作的完成
 */
static void
hash_xlog_split_complete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_split_complete *xlrec = (xl_hash_split_complete *) XLogRecGetData(record);
	Buffer		oldbuf;
	Buffer		newbuf;
	XLogRedoAction action;

	/* 重放旧桶的记录 */
	action = XLogReadBufferForRedo(record, 0, &oldbuf);

	/*
	 * 注意：即使该页是从整页映像恢复的，
	 * 我们仍要更新它，因为桶标志不包含在映像中。
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		oldpage;
		HashPageOpaque oldopaque;

		oldpage = BufferGetPage(oldbuf);
		oldopaque = HashPageGetOpaque(oldpage);

		oldopaque->hasho_flag = xlrec->old_bucket_flag;

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuf);
	}
	if (BufferIsValid(oldbuf))
		UnlockReleaseBuffer(oldbuf);

	/* 重放新桶的记录 */
	action = XLogReadBufferForRedo(record, 1, &newbuf);

	/*
	 * 注意：即使该页是从整页映像恢复的，
	 * 我们仍要更新它，因为桶标志不包含在映像中。
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		newpage;
		HashPageOpaque nopaque;

		newpage = BufferGetPage(newbuf);
		nopaque = HashPageGetOpaque(newpage);

		nopaque->hasho_flag = xlrec->new_bucket_flag;

		PageSetLSN(newpage, lsn);
		MarkBufferDirty(newbuf);
	}
	if (BufferIsValid(newbuf))
		UnlockReleaseBuffer(newbuf);
}

/*
 * 重放 hash 索引挤压操作中的页内容移动
 */
static void
hash_xlog_move_page_contents(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_move_page_contents *xldata = (xl_hash_move_page_contents *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		writebuf = InvalidBuffer;
	Buffer		deletebuf = InvalidBuffer;
	XLogRedoAction action;

	/*
	 * 在开始实际的重放操作之前，确保已在主桶页上持有清理锁。
	 * 这是为了确保本操作重放期间既不能有扫描开始，
	 * 也不能已有扫描正在进行。若允许在本操作期间扫描，
	 * 它们就可能漏掉某些记录，或多次显示
	 * 同一条记录。
	 */
	if (xldata->is_prim_bucket_same_wrt)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &writebuf);
	else
	{
		/*
		 * 我们不关心返回值，因为读取 bucketbuf 的目的
		 * 是确保在主桶页上取得清理锁。
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &writebuf);
	}

	/* 重放向溢出缓冲区添加条目的记录 */
	if (action == BLK_NEEDS_REDO)
	{
		Page		writepage;
		char	   *begin;
		char	   *data;
		Size		datalen;
		uint16		ninserted = 0;

		data = begin = XLogRecGetBlockData(record, 1, &datalen);

		writepage = (Page) BufferGetPage(writebuf);

		if (xldata->ntups > 0)
		{
			OffsetNumber *towrite = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntups;

			while (data - begin < datalen)
			{
				IndexTuple	itup = (IndexTuple) data;
				Size		itemsz;
				OffsetNumber l;

				itemsz = IndexTupleSize(itup);
				itemsz = MAXALIGN(itemsz);

				data += itemsz;

				l = PageAddItem(writepage, (Item) itup, itemsz, towrite[ninserted], false, false);
				if (l == InvalidOffsetNumber)
					elog(ERROR, "hash_xlog_move_page_contents: failed to add item to hash index page, size %d bytes",
						 (int) itemsz);

				ninserted++;
			}
		}

		/*
		 * 插入的元组数必须与重做项中请求的数量相同。
		 */
		Assert(ninserted == xldata->ntups);

		PageSetLSN(writepage, lsn);
		MarkBufferDirty(writebuf);
	}

	/* 重放从溢出缓冲区删除条目的记录 */
	if (XLogReadBufferForRedo(record, 2, &deletebuf) == BLK_NEEDS_REDO)
	{
		Page		page;
		char	   *ptr;
		Size		len;

		ptr = XLogRecGetBlockData(record, 2, &len);

		page = (Page) BufferGetPage(deletebuf);

		if (len > 0)
		{
			OffsetNumber *unused;
			OffsetNumber *unend;

			unused = (OffsetNumber *) ptr;
			unend = (OffsetNumber *) ((char *) ptr + len);

			if ((unend - unused) > 0)
				PageIndexMultiDelete(page, unused, unend - unused);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(deletebuf);
	}

	/*
	 * 重放已经完成，现在可以释放这些缓冲区。我们把锁的释放放在
	 * 重放操作结束时，以确保直到操作结束都持有主桶页
	 * 上的锁。若写缓冲区与主桶页不是同一页，可以在针对
	 * 写缓冲区的操作一完成就释放其上的锁，以此优化，
	 * 但那似乎不值得让代码变得
	 * 更复杂。
	 */
	if (BufferIsValid(deletebuf))
		UnlockReleaseBuffer(deletebuf);

	if (BufferIsValid(writebuf))
		UnlockReleaseBuffer(writebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);
}

/*
 * 重放 hash 索引的挤压页操作
 */
static void
hash_xlog_squeeze_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_squeeze_page *xldata = (xl_hash_squeeze_page *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		writebuf;
	Buffer		ovflbuf;
	Buffer		prevbuf = InvalidBuffer;
	Buffer		mapbuf;
	XLogRedoAction action;

	/*
	 * 在开始实际的重放操作之前，确保已在主桶页上持有清理锁。
	 * 这是为了确保本操作重放期间既不能有扫描开始，
	 * 也不能已有扫描正在进行。若允许在本操作期间扫描，
	 * 它们就可能漏掉某些记录，或多次显示
	 * 同一条记录。
	 */
	if (xldata->is_prim_bucket_same_wrt)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &writebuf);
	else
	{
		/*
		 * 我们不关心返回值，因为读取 bucketbuf 的目的
		 * 是确保在主桶页上取得清理锁。
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &writebuf);
	}

	/* 重放向溢出缓冲区添加条目的记录 */
	if (action == BLK_NEEDS_REDO)
	{
		Page		writepage;
		char	   *begin;
		char	   *data;
		Size		datalen;
		uint16		ninserted = 0;

		data = begin = XLogRecGetBlockData(record, 1, &datalen);

		writepage = (Page) BufferGetPage(writebuf);

		if (xldata->ntups > 0)
		{
			OffsetNumber *towrite = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntups;

			while (data - begin < datalen)
			{
				IndexTuple	itup = (IndexTuple) data;
				Size		itemsz;
				OffsetNumber l;

				itemsz = IndexTupleSize(itup);
				itemsz = MAXALIGN(itemsz);

				data += itemsz;

				l = PageAddItem(writepage, (Item) itup, itemsz, towrite[ninserted], false, false);
				if (l == InvalidOffsetNumber)
					elog(ERROR, "hash_xlog_squeeze_page: failed to add item to hash index page, size %d bytes",
						 (int) itemsz);

				ninserted++;
			}
		}

		/*
		 * 插入的元组数必须与重做项中请求的数量相同。
		 */
		Assert(ninserted == xldata->ntups);

		/*
		 * 若正在添加元组的页就是已释放溢出页的前一页，
		 * 则更新它的 nextblkno。
		 */
		if (xldata->is_prev_bucket_same_wrt)
		{
			HashPageOpaque writeopaque = HashPageGetOpaque(writepage);

			writeopaque->hasho_nextblkno = xldata->nextblkno;
		}

		PageSetLSN(writepage, lsn);
		MarkBufferDirty(writebuf);
	}

	/* 重放初始化溢出缓冲区的记录 */
	if (XLogReadBufferForRedo(record, 2, &ovflbuf) == BLK_NEEDS_REDO)
	{
		Page		ovflpage;
		HashPageOpaque ovflopaque;

		ovflpage = BufferGetPage(ovflbuf);

		_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));

		ovflopaque = HashPageGetOpaque(ovflpage);

		ovflopaque->hasho_prevblkno = InvalidBlockNumber;
		ovflopaque->hasho_nextblkno = InvalidBlockNumber;
		ovflopaque->hasho_bucket = InvalidBucket;
		ovflopaque->hasho_flag = LH_UNUSED_PAGE;
		ovflopaque->hasho_page_id = HASHO_PAGE_ID;

		PageSetLSN(ovflpage, lsn);
		MarkBufferDirty(ovflbuf);
	}
	if (BufferIsValid(ovflbuf))
		UnlockReleaseBuffer(ovflbuf);

	/* 重放已释放溢出页的前一页的记录 */
	if (!xldata->is_prev_bucket_same_wrt &&
		XLogReadBufferForRedo(record, 3, &prevbuf) == BLK_NEEDS_REDO)
	{
		Page		prevpage = BufferGetPage(prevbuf);
		HashPageOpaque prevopaque = HashPageGetOpaque(prevpage);

		prevopaque->hasho_nextblkno = xldata->nextblkno;

		PageSetLSN(prevpage, lsn);
		MarkBufferDirty(prevbuf);
	}
	if (BufferIsValid(prevbuf))
		UnlockReleaseBuffer(prevbuf);

	/* 重放已释放溢出页的后一页的记录 */
	if (XLogRecHasBlockRef(record, 4))
	{
		Buffer		nextbuf;

		if (XLogReadBufferForRedo(record, 4, &nextbuf) == BLK_NEEDS_REDO)
		{
			Page		nextpage = BufferGetPage(nextbuf);
			HashPageOpaque nextopaque = HashPageGetOpaque(nextpage);

			nextopaque->hasho_prevblkno = xldata->prevblkno;

			PageSetLSN(nextpage, lsn);
			MarkBufferDirty(nextbuf);
		}
		if (BufferIsValid(nextbuf))
			UnlockReleaseBuffer(nextbuf);
	}

	if (BufferIsValid(writebuf))
		UnlockReleaseBuffer(writebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);

	/*
	 * 注意：正常操作中，我们会在仍持有主桶页和溢出页锁时
	 * 更新位图页和元页。但重放期间不必持有那些锁，
	 * 因为不会有其他索引更新
	 * 并发进行。
	 */
	/* 重放位图页的记录 */
	if (XLogReadBufferForRedo(record, 5, &mapbuf) == BLK_NEEDS_REDO)
	{
		Page		mappage = (Page) BufferGetPage(mapbuf);
		uint32	   *freep = NULL;
		char	   *data;
		uint32	   *bitmap_page_bit;
		Size		datalen;

		freep = HashPageGetBitmap(mappage);

		data = XLogRecGetBlockData(record, 5, &datalen);
		bitmap_page_bit = (uint32 *) data;

		CLRBIT(freep, *bitmap_page_bit);

		PageSetLSN(mappage, lsn);
		MarkBufferDirty(mapbuf);
	}
	if (BufferIsValid(mapbuf))
		UnlockReleaseBuffer(mapbuf);

	/* 重放元页的记录 */
	if (XLogRecHasBlockRef(record, 6))
	{
		Buffer		metabuf;

		if (XLogReadBufferForRedo(record, 6, &metabuf) == BLK_NEEDS_REDO)
		{
			HashMetaPage metap;
			Page		page;
			char	   *data;
			uint32	   *firstfree_ovflpage;
			Size		datalen;

			data = XLogRecGetBlockData(record, 6, &datalen);
			firstfree_ovflpage = (uint32 *) data;

			page = BufferGetPage(metabuf);
			metap = HashPageGetMeta(page);
			metap->hashm_firstfree = *firstfree_ovflpage;

			PageSetLSN(page, lsn);
			MarkBufferDirty(metabuf);
		}
		if (BufferIsValid(metabuf))
			UnlockReleaseBuffer(metabuf);
	}
}

/*
 * 重放 hash 索引的删除操作
 */
static void
hash_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_delete *xldata = (xl_hash_delete *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		deletebuf;
	Page		page;
	XLogRedoAction action;

	/*
	 * 在开始实际的重放操作之前，确保已在主桶页上持有清理锁。
	 * 这是为了确保本操作重放期间既不能有扫描开始，
	 * 也不能已有扫描正在进行。若允许在本操作期间扫描，
	 * 它们就可能漏掉某些记录，或多次显示
	 * 同一条记录。
	 */
	if (xldata->is_primary_bucket_page)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &deletebuf);
	else
	{
		/*
		 * 我们不关心返回值，因为读取 bucketbuf 的目的
		 * 是确保在主桶页上取得清理锁。
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &deletebuf);
	}

	/* 重放删除桶页中条目的记录 */
	if (action == BLK_NEEDS_REDO)
	{
		char	   *ptr;
		Size		len;

		ptr = XLogRecGetBlockData(record, 1, &len);

		page = (Page) BufferGetPage(deletebuf);

		if (len > 0)
		{
			OffsetNumber *unused;
			OffsetNumber *unend;

			unused = (OffsetNumber *) ptr;
			unend = (OffsetNumber *) ((char *) ptr + len);

			if ((unend - unused) > 0)
				PageIndexMultiDelete(page, unused, unend - unused);
		}

		/*
		 * 仅当 clear_dead_marking 标志设为 true 时，
		 * 才把该页标为不含任何 LP_DEAD 项。细节见
		 * hashbucketcleanup() 中的注释。
		 */
		if (xldata->clear_dead_marking)
		{
			HashPageOpaque pageopaque;

			pageopaque = HashPageGetOpaque(page);
			pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(deletebuf);
	}
	if (BufferIsValid(deletebuf))
		UnlockReleaseBuffer(deletebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);
}

/*
 * 重放主桶页上的分裂清理标志操作。
 */
static void
hash_xlog_split_cleanup(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		HashPageOpaque bucket_opaque;

		page = (Page) BufferGetPage(buffer);

		bucket_opaque = HashPageGetOpaque(page);
		bucket_opaque->hasho_flag &= ~LH_BUCKET_NEEDS_SPLIT_CLEANUP;
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * 重放元页更新
 */
static void
hash_xlog_update_meta_page(XLogReaderState *record)
{
	HashMetaPage metap;
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_update_meta_page *xldata = (xl_hash_update_meta_page *) XLogRecGetData(record);
	Buffer		metabuf;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &metabuf) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);

		metap->hashm_ntuples = xldata->ntuples;

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * 重放 hash 索引中的删除操作，以移除
 * 索引元组插入期间被标为 DEAD 的元组。
 */
static void
hash_xlog_vacuum_one_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_vacuum_one_page *xldata;
	Buffer		buffer;
	Buffer		metabuf;
	Page		page;
	XLogRedoAction action;
	HashPageOpaque pageopaque;
	OffsetNumber *toDelete;

	xldata = (xl_hash_vacuum_one_page *) XLogRecGetData(record);
	toDelete = xldata->offsets;

	/*
	 * 若有任何冲突处理要做，必须在我们
	 * 更新该页之前完成。
	 *
	 * 在 hash 索引元组插入期间被标为 LP_DEAD 并正在移除的
	 * hash 索引条目，可能与备库查询冲突。你也许会认为
	 * 清理产生的条目同样会冲突，但那已经处理过了。
	 * XLOG_HEAP2_PRUNE 条目给出由堆清理所清理的最高 xid，
	 * 因此我们可以在它到达时只解决一次冲突。此后便知道，
	 * 来自该索引上单独的 hash 索引清理条目的冲突
	 * 已经不存在。
	 */
	if (InHotStandby)
	{
		RelFileLocator rlocator;

		XLogRecGetBlockTag(record, 0, &rlocator, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(xldata->snapshotConflictHorizon,
											xldata->isCatalogRel,
											rlocator);
	}

	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		PageIndexMultiDelete(page, toDelete, xldata->ntuples);

		/*
		 * 把该页标为不含任何 LP_DEAD 项。细节见
		 * _hash_vacuum_one_page() 中的注释。
		 */
		pageopaque = HashPageGetOpaque(page);
		pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	if (XLogReadBufferForRedo(record, 1, &metabuf) == BLK_NEEDS_REDO)
	{
		Page		metapage;
		HashMetaPage metap;

		metapage = BufferGetPage(metabuf);
		metap = HashPageGetMeta(metapage);

		metap->hashm_ntuples -= xldata->ntuples;

		PageSetLSN(metapage, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

void
hash_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_HASH_INIT_META_PAGE:
			hash_xlog_init_meta_page(record);
			break;
		case XLOG_HASH_INIT_BITMAP_PAGE:
			hash_xlog_init_bitmap_page(record);
			break;
		case XLOG_HASH_INSERT:
			hash_xlog_insert(record);
			break;
		case XLOG_HASH_ADD_OVFL_PAGE:
			hash_xlog_add_ovfl_page(record);
			break;
		case XLOG_HASH_SPLIT_ALLOCATE_PAGE:
			hash_xlog_split_allocate_page(record);
			break;
		case XLOG_HASH_SPLIT_PAGE:
			hash_xlog_split_page(record);
			break;
		case XLOG_HASH_SPLIT_COMPLETE:
			hash_xlog_split_complete(record);
			break;
		case XLOG_HASH_MOVE_PAGE_CONTENTS:
			hash_xlog_move_page_contents(record);
			break;
		case XLOG_HASH_SQUEEZE_PAGE:
			hash_xlog_squeeze_page(record);
			break;
		case XLOG_HASH_DELETE:
			hash_xlog_delete(record);
			break;
		case XLOG_HASH_SPLIT_CLEANUP:
			hash_xlog_split_cleanup(record);
			break;
		case XLOG_HASH_UPDATE_META_PAGE:
			hash_xlog_update_meta_page(record);
			break;
		case XLOG_HASH_VACUUM_ONE_PAGE:
			hash_xlog_vacuum_one_page(record);
			break;
		default:
			elog(PANIC, "hash_redo: unknown op code %u", info);
	}
}

/*
 * 在对 hash 页做一致性检查之前先屏蔽它。
 */
void
hash_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	HashPageOpaque opaque;
	int			pagetype;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	opaque = HashPageGetOpaque(page);

	pagetype = opaque->hasho_flag & LH_PAGE_TYPE;
	if (pagetype == LH_UNUSED_PAGE)
	{
		/*
		 * 屏蔽 UNUSED 页上的全部内容。
		 */
		mask_page_content(page);
	}
	else if (pagetype == LH_BUCKET_PAGE ||
			 pagetype == LH_OVERFLOW_PAGE)
	{
		/*
		 * 在 hash 桶页和溢出页上，有可能不发出任何 WAL 记录
		 * 就修改 LP_FLAGS。因此屏蔽行指针标志。细节见
		 * hashgettuple() 与 _hash_kill_items()。
		 */
		mask_lp_flags(page);
	}

	/*
	 * 提示位 LH_PAGE_HAS_DEAD_TUPLES 可能仍未被记入日志。
	 * 因此屏蔽它。细节见 _hash_kill_items()。
	 */
	opaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
}
