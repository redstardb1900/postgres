/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  Postgres hash 表的搜索代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsearch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/rel.h"

static bool _hash_readpage(IndexScanDesc scan, Buffer *bufP,
						   ScanDirection dir);
static int	_hash_load_qualified_items(IndexScanDesc scan, Page page,
									   OffsetNumber offnum, ScanDirection dir);
static inline void _hash_saveitem(HashScanOpaque so, int itemIndex,
								  OffsetNumber offnum, IndexTuple itup);
static void _hash_readnext(IndexScanDesc scan, Buffer *bufp,
						   Page *pagep, HashPageOpaque *opaquep);

/*
 *	_hash_next() -- 取得扫描中的下一项。
 *
 *		进入时，so->currPos 描述当前页，该页可能
 *		已钉住但未上锁，而 so->currPos.itemIndex 指出
 *		先前返回的是哪一项。
 *
 *		成功退出时，scan->xs_ctup.t_self 被设为下一个
 *		堆元组的 TID。so->currPos 按需更新。
 *
 *		失败退出时（没有更多元组），返回 false，桶页上
 *		仍保持钉住，但溢出
 *		页上不持有钉住或锁。
 */
bool
_hash_next(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashScanPosItem *currItem;
	BlockNumber blkno;
	Buffer		buf;
	bool		end_of_scan = false;

	/*
	 * 前进到当前页的下一个元组；若已经走完，则按扫描方向
	 * 尝试从下一页或上一页读取数据。在移到
	 * 下一页或上一页之前，务必先处理全部
	 * 被杀死的项。
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			blkno = so->currPos.nextPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(buf));
				if (!_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}
	else
	{
		if (--so->currPos.itemIndex < so->currPos.firstItem)
		{
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			blkno = so->currPos.prevPage;
			if (BlockNumberIsValid(blkno))
			{
				buf = _hash_getbuf(rel, blkno, HASH_READ,
								   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
				TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(buf));

				/*
				 * 整个扫描期间我们一直保持桶页上的钉住，
				 * 因此释放这里额外取得的那个
				 * 钉住。
				 */
				if (buf == so->hashso_bucket_buf ||
					buf == so->hashso_split_bucket_buf)
					_hash_dropbuf(rel, buf);

				if (!_hash_readpage(scan, &buf, dir))
					end_of_scan = true;
			}
			else
				end_of_scan = true;
		}
	}

	if (end_of_scan)
	{
		_hash_dropscanbuf(rel, so);
		HashScanPosInvalidate(so->currPos);
		return false;
	}

	/* 可以，itemIndex 指出该返回哪一项 */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;

	return true;
}

/*
 * 前进到桶中的下一页（若有）。若正在扫描分裂操作期间
 * 正在被填充的桶，则本函数在该正在被填充的桶的最后一页之后，
 * 前进到正在被分裂的桶。
 */
static void
_hash_readnext(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		block_found = false;

	blkno = (*opaquep)->hasho_nextblkno;

	/*
	 * 直到扫描结束都保持主桶页上的钉住。保留钉住的
	 * 原因见 _hash_first 中的注释。
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, *bufp);

	*bufp = InvalidBuffer;
	/* 未持有任何缓冲区锁时检查中断 */
	CHECK_FOR_INTERRUPTS();
	if (BlockNumberIsValid(blkno))
	{
		*bufp = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		block_found = true;
	}
	else if (so->hashso_buc_populated && !so->hashso_buc_split)
	{
		/*
		 * 桶的末尾；若扫描开始时有正在进行的分裂，
		 * 就去扫描正在被分裂的桶。
		 */
		*bufp = so->hashso_split_bucket_buf;

		/*
		 * 正在被分裂的桶的缓冲区必须有效，因为我们在扫描开始前
		 * 就钉住了它，并且一直保持到扫描结束。
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		PredicateLockPage(rel, BufferGetBlockNumber(*bufp), scan->xs_snapshot);

		/*
		 * 把 hashso_buc_split 设为 true，表示我们正在扫描
		 * 正在被分裂的桶。
		 */
		so->hashso_buc_split = true;

		block_found = true;
	}

	if (block_found)
	{
		*pagep = BufferGetPage(*bufp);
		TestForOldSnapshot(scan->xs_snapshot, rel, *pagep);
		*opaquep = HashPageGetOpaque(*pagep);
	}
}

/*
 * 前进到桶中的上一页（若有）。若当前扫描是在分裂操作期间
 * 开始的，则本函数在正在被分裂的桶的第一页之后，前进到
 * 正在被填充的桶。
 */
static void
_hash_readprev(IndexScanDesc scan,
			   Buffer *bufp, Page *pagep, HashPageOpaque *opaquep)
{
	BlockNumber blkno;
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		haveprevblk;

	blkno = (*opaquep)->hasho_prevblkno;

	/*
	 * 直到扫描结束都保持主桶页上的钉住。保留钉住的
	 * 原因见 _hash_first 中的注释。
	 */
	if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
	{
		LockBuffer(*bufp, BUFFER_LOCK_UNLOCK);
		haveprevblk = false;
	}
	else
	{
		_hash_relbuf(rel, *bufp);
		haveprevblk = true;
	}

	*bufp = InvalidBuffer;
	/* 未持有任何缓冲区锁时检查中断 */
	CHECK_FOR_INTERRUPTS();

	if (haveprevblk)
	{
		Assert(BlockNumberIsValid(blkno));
		*bufp = _hash_getbuf(rel, blkno, HASH_READ,
							 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
		*pagep = BufferGetPage(*bufp);
		TestForOldSnapshot(scan->xs_snapshot, rel, *pagep);
		*opaquep = HashPageGetOpaque(*pagep);

		/*
		 * 整个扫描期间我们一直保持桶页上的钉住，
		 * 因此释放这里额外取得的那个钉住。
		 */
		if (*bufp == so->hashso_bucket_buf || *bufp == so->hashso_split_bucket_buf)
			_hash_dropbuf(rel, *bufp);
	}
	else if (so->hashso_buc_populated && so->hashso_buc_split)
	{
		/*
		 * 桶的末尾；若扫描开始时有正在进行的分裂，
		 * 就去扫描正在被填充的桶。
		 */
		*bufp = so->hashso_bucket_buf;

		/*
		 * 正在被填充的桶的缓冲区必须有效，因为我们在扫描开始前
		 * 就钉住了它，并且一直保持到扫描结束。
		 */
		Assert(BufferIsValid(*bufp));

		LockBuffer(*bufp, BUFFER_LOCK_SHARE);
		*pagep = BufferGetPage(*bufp);
		*opaquep = HashPageGetOpaque(*pagep);

		/* 移到桶链的末尾 */
		while (BlockNumberIsValid((*opaquep)->hasho_nextblkno))
			_hash_readnext(scan, bufp, pagep, opaquep);

		/*
		 * 把 hashso_buc_split 设为 false，表示我们正在扫描
		 * 正在被填充的桶。
		 */
		so->hashso_buc_split = false;
	}
}

/*
 *	_hash_first() -- 找出扫描中的第一项。
 *
 *		我们在索引中找出满足扫描描述符所关联
 *		限定条件的第一项（若是反向扫描，
 *		则为最后一项）。
 *
 *		成功退出时，若含有当前索引元组的页是
 *		溢出页，则钉住和锁都释放；若是桶
 *		页，则保持钉住但不持锁，并且该页上
 *		匹配元组（tuple）的数据已装入 so->currPos，
 *		scan->xs_ctup.t_self 被设为当前元组的堆 TID。
 *
 *		失败退出时（没有更多元组），返回 false，桶页上
 *		保持钉住，但溢出页上不持有钉住或锁。
 */
bool
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	HashScanPosItem *currItem;

	pgstat_count_index_scan(rel);

	/*
	 * 我们不支持没有任何索引限定条件的 hash 扫描，因为那样
	 * 就得读整个索引，而不是只读一个桶。那会
	 * 带来一大堆问题，因为我们没有切实可行的办法
	 * 把所有桶都锁住，以防止分裂或压缩。
	 */
	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes do not support whole-index scans")));

	/* 索引限定条件可能不止一个，但我们只对第一个做哈希 */
	cur = &scan->keyData[0];

	/* 我们只支持单列 hash 索引 */
	Assert(cur->sk_attno == 1);
	/* 而且也只有一种操作符策略 */
	Assert(cur->sk_strategy == HTEqualStrategyNumber);

	/*
	 * 若索引限定条件中的常量是 NULL，就认为它无法匹配
	 * 索引中的任何项。
	 */
	if (cur->sk_flags & SK_ISNULL)
		return false;

	/*
	 * 可以计算哈希键了。我们希望在取得任何
	 * 锁之前做这件事，以防用户定义的哈希函数恰好很慢。
	 *
	 * 若扫描键的操作符不是跨类型比较，就可以使用
	 * 缓存的哈希函数；否则就得去系统目录里查。
	 *
	 * 我们支持这样的约定：sk_subtype == InvalidOid 表示
	 * 操作符类的输入类型；这是为简化 ScanKeyInit() 而做的变通。
	 */
	if (cur->sk_subtype == rel->rd_opcintype[0] ||
		cur->sk_subtype == InvalidOid)
		hashkey = _hash_datum2hashkey(rel, cur->sk_argument);
	else
		hashkey = _hash_datum2hashkey_type(rel, cur->sk_argument,
										   cur->sk_subtype);

	so->hashso_sk_hash = hashkey;

	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_READ, NULL);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	TestForOldSnapshot(scan->xs_snapshot, rel, page);
	opaque = HashPageGetOpaque(page);
	bucket = opaque->hasho_bucket;

	so->hashso_bucket_buf = buf;

	/*
	 * 若桶分裂正在进行，则在扫描正在被填充的桶时，
	 * 需要跳过从正在被分裂的桶复制来的元组。
	 * 还须在正在被分裂的桶上保持钉住，以确保
	 * VACUUM 所做的分裂清理在本次扫描结束前
	 * 不会移除其中的元组。还须在正在被填充的桶上
	 * 保持钉住，以确保 VACUUM 在本次扫描完成前
	 * 不会挤压该桶；否则正向与反向扫描期间
	 * 无法维持元组顺序。这里必须小心加锁顺序：首先，
	 * 取得正在被分裂的桶上的锁；然后释放该锁，但
	 * 不释放钉住；然后取得正在被填充的桶上的锁，并
	 * 再次确认桶分裂是否仍在进行。先取得正在被分裂的桶
	 * 上的锁，可以确保 VACUUM 会一直等到
	 * 本次扫描
	 * 结束。
	 */
	if (H_BUCKET_BEING_POPULATED(opaque))
	{
		BlockNumber old_blkno;
		Buffer		old_buf;

		old_blkno = _hash_get_oldblock_from_newbucket(rel, bucket);

		/*
		 * 释放新桶上的锁，并在取得
		 * 旧桶上的锁之后重新取得它。
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		old_buf = _hash_getbuf(rel, old_blkno, HASH_READ, LH_BUCKET_PAGE);
		TestForOldSnapshot(scan->xs_snapshot, rel, BufferGetPage(old_buf));

		/*
		 * 记住分裂桶的缓冲区，以便稍后
		 * 扫描时使用。
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
		Assert(opaque->hasho_bucket == bucket);

		if (H_BUCKET_BEING_POPULATED(opaque))
			so->hashso_buc_populated = true;
		else
		{
			_hash_dropbuf(rel, so->hashso_split_bucket_buf);
			so->hashso_split_bucket_buf = InvalidBuffer;
		}
	}

	/* 若请求反向扫描，则移到链的末尾 */
	if (ScanDirectionIsBackward(dir))
	{
		/*
		 * 在分裂期间开始的反向扫描，需要从正在被分裂的桶的
		 * 末尾开始。
		 */
		while (BlockNumberIsValid(opaque->hasho_nextblkno) ||
			   (so->hashso_buc_populated && !so->hashso_buc_split))
			_hash_readnext(scan, &buf, &page, &opaque);
	}

	/* 记住我们钉住了哪个缓冲区（若有） */
	Assert(BufferIsInvalid(so->currPos.buf));
	so->currPos.buf = buf;

	/* 现在从一页中找出所有满足限定条件的元组 */
	if (!_hash_readpage(scan, &buf, dir))
		return false;

	/* 可以，itemIndex 指出该返回哪一项 */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;

	/* 若执行到这里，说明 _hash_readpage 找到了有效元组 */
	return true;
}

/*
 *	_hash_readpage() -- 把当前索引页的数据装入 so->currPos
 *
 *	我们扫描当前索引页上的全部项，若满足限定条件
 *	就把它们存入 so->currPos。若当前页没有找到
 *	匹配项，就按方向移到桶链中的下一页或
 *	上一页。
 *
 *	若找到任何匹配项则返回 true，否则返回 false。
 */
static bool
_hash_readpage(IndexScanDesc scan, Buffer *bufP, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum;
	uint16		itemIndex;

	buf = *bufP;
	Assert(BufferIsValid(buf));
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);

	so->currPos.buf = buf;
	so->currPos.currPage = BufferGetBlockNumber(buf);

	if (ScanDirectionIsForward(dir))
	{
		BlockNumber prev_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* 新页，用二分搜索定位起始位置 */
			offnum = _hash_binsearch(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != 0)
				break;

			/*
			 * 当前页找不到任何匹配元组，移到
			 * 下一页。离开当前页之前，先处理
			 * 被杀死的项。
			 */
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			/*
			 * 若这是主桶页，hasho_prevblkno 并不是真正的
			 * 块号。
			 */
			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				prev_blkno = InvalidBlockNumber;
			else
				prev_blkno = opaque->hasho_prevblkno;

			_hash_readnext(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * 记住下一块和上一块的块号，供可滚动
				 * 游标得知起始位置，并返回 false，
				 * 表示没有找到更多匹配元组。另外，
				 * 不要重置 currPage 或 lsn，因为我们预期
				 * 本函数返回之后，会对旧页调用
				 * _hash_kill_items。
				 */
				so->currPos.prevPage = prev_blkno;
				so->currPos.nextPage = InvalidBlockNumber;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		BlockNumber next_blkno = InvalidBlockNumber;

		for (;;)
		{
			/* 新页，用二分搜索定位起始位置 */
			offnum = _hash_binsearch_last(page, so->hashso_sk_hash);

			itemIndex = _hash_load_qualified_items(scan, page, offnum, dir);

			if (itemIndex != MaxIndexTuplesPerPage)
				break;

			/*
			 * 当前页找不到任何匹配元组，移到
			 * 上一页。离开当前页之前，先处理
			 * 被杀死的项。
			 */
			if (so->numKilled > 0)
				_hash_kill_items(scan);

			if (so->currPos.buf == so->hashso_bucket_buf ||
				so->currPos.buf == so->hashso_split_bucket_buf)
				next_blkno = opaque->hasho_nextblkno;

			_hash_readprev(scan, &buf, &page, &opaque);
			if (BufferIsValid(buf))
			{
				so->currPos.buf = buf;
				so->currPos.currPage = BufferGetBlockNumber(buf);
			}
			else
			{
				/*
				 * 记住下一块和上一块的块号，供可滚动
				 * 游标得知起始位置，并返回 false，
				 * 表示没有找到更多匹配元组。另外，
				 * 不要重置 currPage 或 lsn，因为我们预期
				 * 本函数返回之后，会对旧页调用
				 * _hash_kill_items。
				 */
				so->currPos.prevPage = InvalidBlockNumber;
				so->currPos.nextPage = next_blkno;
				so->currPos.buf = buf;
				return false;
			}
		}

		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxIndexTuplesPerPage - 1;
		so->currPos.itemIndex = MaxIndexTuplesPerPage - 1;
	}

	if (so->currPos.buf == so->hashso_bucket_buf ||
		so->currPos.buf == so->hashso_split_bucket_buf)
	{
		so->currPos.prevPage = InvalidBlockNumber;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		so->currPos.prevPage = opaque->hasho_prevblkno;
		so->currPos.nextPage = opaque->hasho_nextblkno;
		_hash_relbuf(rel, so->currPos.buf);
		so->currPos.buf = InvalidBuffer;
	}

	Assert(so->currPos.firstItem <= so->currPos.lastItem);
	return true;
}

/*
 * 把当前索引页上所有合格的项装入
 * so->currPos。_hash_readpage 的辅助函数。
 */
static int
_hash_load_qualified_items(IndexScanDesc scan, Page page,
						   OffsetNumber offnum, ScanDirection dir)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	IndexTuple	itup;
	int			itemIndex;
	OffsetNumber maxoff;

	maxoff = PageGetMaxOffsetNumber(page);

	if (ScanDirectionIsForward(dir))
	{
		/* 按升序装入 items[] */
		itemIndex = 0;

		while (offnum <= maxoff)
		{
			Assert(offnum >= FirstOffsetNumber);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * 对在分裂进行期间开始的扫描，跳过被分裂操作
			 * 移走的元组。同时跳过
			 * 已标为死的元组。
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberNext(offnum);	/* 向前移动 */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				/* 元组满足限定条件，因此记住它 */
				_hash_saveitem(so, itemIndex, offnum, itup);
				itemIndex++;
			}
			else
			{
				/*
				 * 本页不再有匹配元组。因此退出 while
				 * 循环。
				 */
				break;
			}

			offnum = OffsetNumberNext(offnum);
		}

		Assert(itemIndex <= MaxIndexTuplesPerPage);
		return itemIndex;
	}
	else
	{
		/* 按降序装入 items[] */
		itemIndex = MaxIndexTuplesPerPage;

		while (offnum >= FirstOffsetNumber)
		{
			Assert(offnum <= maxoff);
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * 对在分裂进行期间开始的扫描，跳过被分裂操作
			 * 移走的元组。同时跳过
			 * 已标为死的元组。
			 */
			if ((so->hashso_buc_populated && !so->hashso_buc_split &&
				 (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberPrev(offnum);	/* 向后移动 */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup) &&
				_hash_checkqual(scan, itup))
			{
				itemIndex--;
				/* 元组满足限定条件，因此记住它 */
				_hash_saveitem(so, itemIndex, offnum, itup);
			}
			else
			{
				/*
				 * 本页不再有匹配元组。因此退出 while
				 * 循环。
				 */
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		return itemIndex;
	}
}

/* 把一个索引项存入 so->currPos.items[itemIndex] */
static inline void
_hash_saveitem(HashScanOpaque so, int itemIndex,
			   OffsetNumber offnum, IndexTuple itup)
{
	HashScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
}
