/*-------------------------------------------------------------------------
 *
 * hashpage.c
 *	  Postgres hash 访问方法的 hash 表页管理代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashpage.c
 *
 * 说明（NOTES）
 *	  Postgres 的 hash 页看起来和普通关系页一样。
 *	  高地址处的特殊空间含有该页的信息，
 *	  包括它是溢出页还是真正的桶、桶号，
 *	  以及同一桶中前一页和后一页的
 *	  块号。
 *
 *	  hash 关系中的第一页，即第零页，是特殊的——它存储
 *	  描述 hash 表的信息；它被称为
 *	  "元页"。第一页及更高的页存放实际数据。
 *
 *	  另外还有位图页，这里并不操作它们；
 *	  见 hashovfl.c。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"

static bool _hash_alloc_buckets(Relation rel, BlockNumber firstblock,
								uint32 nblocks);
static void _hash_splitbucket(Relation rel, Buffer metabuf,
							  Bucket obucket, Bucket nbucket,
							  Buffer obuf,
							  Buffer nbuf,
							  HTAB *htab,
							  uint32 maxbucket,
							  uint32 highmask, uint32 lowmask);
static void log_split_page(Relation rel, Buffer buf);


/*
 *	_hash_getbuf() -- 按块号取得供读或写的缓冲区。
 *
 *		'access' 必须是 HASH_READ、HASH_WRITE 或 HASH_NOLOCK。
 *		'flags' 是所允许页类型的按位 OR。
 *
 *		只能用来读取预期已经有效的页。
 *		会按给定的 flags 调用 _hash_checkpage()。
 *
 *		本例程返回时，已在所请求的缓冲区上加上适当的锁，
 *		并且其引用计数已经增加
 *		（即该缓冲区 "已上锁并钉住"）。
 *
 *		不允许使用 P_NEW，因为本例程只能用来
 *		访问已知位于文件系统 EOF 之前的页。
 *		扩展索引应当使用 _hash_getnewbuf。
 */
Buffer
_hash_getbuf(Relation rel, BlockNumber blkno, int access, int flags)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBuffer(rel, blkno);

	if (access != HASH_NOLOCK)
		LockBuffer(buf, access);

	/* 引用计数和锁类型都正确 */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 * _hash_getbuf_with_condlock_cleanup() -- 尝试取得供清理用的缓冲区。
 *
 *		我们读入该页并尝试取得清理锁。若取得了，
 *		就返回该缓冲区；否则返回 InvalidBuffer。
 */
Buffer
_hash_getbuf_with_condlock_cleanup(Relation rel, BlockNumber blkno, int flags)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBuffer(rel, blkno);

	if (!ConditionalLockBufferForCleanup(buf))
	{
		ReleaseBuffer(buf);
		return InvalidBuffer;
	}

	/* 引用计数和锁类型都正确 */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 *	_hash_getinitbuf() -- 按块号取得并初始化缓冲区。
 *
 *		只能用来读取已知位于索引的文件系统 EOF 之前、
 *		但需要从头填充的页。
 *		会自动调用 _hash_pageinit()。除此之外，其效果
 *		类似于 access = HASH_WRITE 的 _hash_getbuf()。
 *
 *		本例程返回时，已在所请求的缓冲区上加上写锁，
 *		并且其引用计数已经增加
 *		（即该缓冲区 "已上锁并钉住"）。
 *
 *		不允许使用 P_NEW，因为本例程只能用来
 *		访问已知位于文件系统 EOF 之前的页。
 *		扩展索引应当使用 _hash_getnewbuf。
 */
Buffer
_hash_getinitbuf(Relation rel, BlockNumber blkno)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_ZERO_AND_LOCK,
							 NULL);

	/* 引用计数和锁类型都正确 */

	/* 初始化该页 */
	_hash_pageinit(BufferGetPage(buf), BufferGetPageSize(buf));

	return buf;
}

/*
 *	_hash_initbuf() -- 按桶号取得并初始化缓冲区。
 */
void
_hash_initbuf(Buffer buf, uint32 max_bucket, uint32 num_bucket, uint32 flag,
			  bool initpage)
{
	HashPageOpaque pageopaque;
	Page		page;

	page = BufferGetPage(buf);

	/* 初始化该页 */
	if (initpage)
		_hash_pageinit(page, BufferGetPageSize(buf));

	pageopaque = HashPageGetOpaque(page);

	/*
	 * 用当前的 hashm_maxbucket 设置 hasho_prevblkno。该值将用来
	 * 校验缓存的 HashMetaPageData。参见
	 * _hash_getbucketbuf_from_hashkey()。
	 */
	pageopaque->hasho_prevblkno = max_bucket;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = num_bucket;
	pageopaque->hasho_flag = flag;
	pageopaque->hasho_page_id = HASHO_PAGE_ID;
}

/*
 *	_hash_getnewbuf() -- 在索引末尾取得一个新页。
 *
 *		API 与 _hash_getinitbuf 相同，只是我们在向索引
 *		增加一页，因此预期该页位于逻辑 EOF 之后。
 *		（不过必须支持并非如此的情形，因为先前的尝试
 *		可能在扩展了文件系统 EOF 之后、更新元页以反映
 *		所增加的页之前崩溃。）
 *
 *		调用者有责任保证同一时刻只有一个进程
 *		能扩展该索引。实践中，本函数只在持有
 *		元页写锁时调用，因为增加一页总是
 *		伴随着元页数据的更新。
 */
Buffer
_hash_getnewbuf(Relation rel, BlockNumber blkno, ForkNumber forkNum)
{
	BlockNumber nblocks = RelationGetNumberOfBlocksInFork(rel, forkNum);
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");
	if (blkno > nblocks)
		elog(ERROR, "access to noncontiguous page in hash index \"%s\"",
			 RelationGetRelationName(rel));

	/* smgr 坚持要我们显式扩展该关系 */
	if (blkno == nblocks)
	{
		buf = ExtendBufferedRel(BMR_REL(rel), forkNum, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
		if (BufferGetBlockNumber(buf) != blkno)
			elog(ERROR, "unexpected hash relation size: %u, should be %u",
				 BufferGetBlockNumber(buf), blkno);
	}
	else
	{
		buf = ReadBufferExtended(rel, forkNum, blkno, RBM_ZERO_AND_LOCK,
								 NULL);
	}

	/* 引用计数和锁类型都正确 */

	/* 初始化该页 */
	_hash_pageinit(BufferGetPage(buf), BufferGetPageSize(buf));

	return buf;
}

/*
 *	_hash_getbuf_with_strategy() -- 用非默认策略取得缓冲区。
 *
 *		与 _hash_getbuf() 相同，但还允许指定缓冲区访问
 *		策略。我们把它用于 VACUUM 操作。
 */
Buffer
_hash_getbuf_with_strategy(Relation rel, BlockNumber blkno,
						   int access, int flags,
						   BufferAccessStrategy bstrategy)
{
	Buffer		buf;

	if (blkno == P_NEW)
		elog(ERROR, "hash AM does not use P_NEW");

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);

	if (access != HASH_NOLOCK)
		LockBuffer(buf, access);

	/* 引用计数和锁类型都正确 */

	_hash_checkpage(rel, buf, flags);

	return buf;
}

/*
 *	_hash_relbuf() -- 释放一个已上锁的缓冲区。
 *
 * 锁和钉住（引用计数）都会放下。
 */
void
_hash_relbuf(Relation rel, Buffer buf)
{
	UnlockReleaseBuffer(buf);
}

/*
 *	_hash_dropbuf() -- 释放一个未上锁的缓冲区。
 *
 * 用于解除我们并未持锁的缓冲区上的钉住。
 */
void
_hash_dropbuf(Relation rel, Buffer buf)
{
	ReleaseBuffer(buf);
}

/*
 *	_hash_dropscanbuf() -- 释放扫描中使用的缓冲区。
 *
 * 本例程解除扫描期间使用、且我们并未持锁的
 * 那些缓冲区上的钉住。
 */
void
_hash_dropscanbuf(Relation rel, HashScanOpaque so)
{
	/* 释放我们在主桶页上持有的钉住 */
	if (BufferIsValid(so->hashso_bucket_buf) &&
		so->hashso_bucket_buf != so->currPos.buf)
		_hash_dropbuf(rel, so->hashso_bucket_buf);
	so->hashso_bucket_buf = InvalidBuffer;

	/* 释放我们在正在分裂的桶的主桶页上持有的钉住 */
	if (BufferIsValid(so->hashso_split_bucket_buf) &&
		so->hashso_split_bucket_buf != so->currPos.buf)
		_hash_dropbuf(rel, so->hashso_split_bucket_buf);
	so->hashso_split_bucket_buf = InvalidBuffer;

	/* 释放我们仍持有的任何钉住 */
	if (BufferIsValid(so->currPos.buf))
		_hash_dropbuf(rel, so->currPos.buf);
	so->currPos.buf = InvalidBuffer;

	/* 重置分裂扫描 */
	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;
}


/*
 *	_hash_init() -- 初始化 hash 索引的元数据页、
 *				初始的各个桶，以及初始位图页。
 *
 * 初始桶数取决于 num_tuples，即对最初要装入
 * 索引的元组数量的估计。返回所选定的
 * 桶数。
 *
 * 这里对加锁相当随意，因为我们知道没有别人
 * 会访问这个索引。尤其是，关于不得同时持有
 * 多把缓冲区锁的规则被忽略了。
 */
uint32
_hash_init(Relation rel, double num_tuples, ForkNumber forkNum)
{
	Buffer		metabuf;
	Buffer		buf;
	Buffer		bitmapbuf;
	Page		pg;
	HashMetaPage metap;
	RegProcedure procid;
	int32		data_width;
	int32		item_width;
	int32		ffactor;
	uint32		num_buckets;
	uint32		i;
	bool		use_wal;

	/* 安全检查 */
	if (RelationGetNumberOfBlocksInFork(rel, forkNum) != 0)
		elog(ERROR, "cannot initialize non-empty hash index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * 若关系是持久的，或者这是 init 分支，就把页的
	 * 创建记入 WAL。不记录 WAL 的关系的 init 分支始终
	 * 需要记入 WAL。
	 */
	use_wal = RelationNeedsWAL(rel) || forkNum == INIT_FORKNUM;

	/*
	 * 确定该索引的目标填充因子（每桶元组数）。
	 * 想法是让填充因子对应的页满度大约等于
	 * 用户可设置的 fillfactor 参数所说的程度。由于索引
	 * 数据类型（即 uint32 哈希键）是定宽的，我们可以精确计算。
	 */
	data_width = sizeof(uint32);
	item_width = MAXALIGN(sizeof(IndexTupleData)) + MAXALIGN(data_width) +
		sizeof(ItemIdData);		/* 计入行指针 */
	ffactor = HashGetTargetPageUsage(rel) / item_width;
	/* 限制在合理范围内 */
	if (ffactor < 10)
		ffactor = 10;

	procid = index_getprocid(rel, 1, HASHSTANDARD_PROC);

	/*
	 * 我们依次初始化元页、前 N 个桶页和第一张
	 * 位图页，用 _hash_getnewbuf 促使发生 smgrextend()
	 * 调用。这保证 smgr 层对索引的物理长度
	 * 有正确认识。
	 *
	 * 不需要临界区，因为出错时整个关系的创建
	 * 都会回滚。
	 */
	metabuf = _hash_getnewbuf(rel, HASH_METAPAGE, forkNum);
	_hash_init_metabuffer(metabuf, num_tuples, procid, ffactor, false);
	MarkBufferDirty(metabuf);

	pg = BufferGetPage(metabuf);
	metap = HashPageGetMeta(pg);

	/* XLOG 相关 */
	if (use_wal)
	{
		xl_hash_init_meta_page xlrec;
		XLogRecPtr	recptr;

		xlrec.num_tuples = num_tuples;
		xlrec.procid = metap->hashm_procid;
		xlrec.ffactor = metap->hashm_ffactor;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInitMetaPage);
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_META_PAGE);

		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	num_buckets = metap->hashm_maxbucket + 1;

	/*
	 * 初始化各个桶期间释放元页上的缓冲区锁。
	 * 否则我们会处于中断推迟状态，CHECK_FOR_INTERRUPTS
	 * 将起不了作用。无论如何，长时间持有缓冲区锁
	 * 都不是好主意，因为那会堵住后台写入进程。
	 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * 初始化前 N 个桶并记入 WAL
	 */
	for (i = 0; i < num_buckets; i++)
	{
		BlockNumber blkno;

		/* 允许中断，以防 N 很大 */
		CHECK_FOR_INTERRUPTS();

		blkno = BUCKET_TO_BLKNO(metap, i);
		buf = _hash_getnewbuf(rel, blkno, forkNum);
		_hash_initbuf(buf, metap->hashm_maxbucket, i, LH_BUCKET_PAGE, false);
		MarkBufferDirty(buf);

		if (use_wal)
			log_newpage(&rel->rd_locator,
						forkNum,
						blkno,
						BufferGetPage(buf),
						true);
		_hash_relbuf(rel, buf);
	}

	/* 现在重新取得元页上的缓冲区锁 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * 初始化位图页
	 */
	bitmapbuf = _hash_getnewbuf(rel, num_buckets + 1, forkNum);
	_hash_initbitmapbuffer(bitmapbuf, metap->hashm_bmsize, false);
	MarkBufferDirty(bitmapbuf);

	/* 把新位图页加入元页的位图列表 */
	/* 元页已经持有写锁 */
	if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of overflow pages in hash index \"%s\"",
						RelationGetRelationName(rel))));

	metap->hashm_mapp[metap->hashm_nmaps] = num_buckets + 1;

	metap->hashm_nmaps++;
	MarkBufferDirty(metabuf);

	/* XLOG 相关 */
	if (use_wal)
	{
		xl_hash_init_bitmap_page xlrec;
		XLogRecPtr	recptr;

		xlrec.bmsize = metap->hashm_bmsize;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashInitBitmapPage);
		XLogRegisterBuffer(0, bitmapbuf, REGBUF_WILL_INIT);

		/*
		 * 这是安全的，只因为在此阶段没有别人
		 * 能修改该索引；它只对正在创建它的
		 * 那个事务可见。
		 */
		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INIT_BITMAP_PAGE);

		PageSetLSN(BufferGetPage(bitmapbuf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	/* 全部完成 */
	_hash_relbuf(rel, bitmapbuf);
	_hash_relbuf(rel, metabuf);

	return num_buckets;
}

/*
 *	_hash_init_metabuffer() -- 初始化 hash 索引的元数据页。
 */
void
_hash_init_metabuffer(Buffer buf, double num_tuples, RegProcedure procid,
					  uint16 ffactor, bool initpage)
{
	HashMetaPage metap;
	HashPageOpaque pageopaque;
	Page		page;
	double		dnumbuckets;
	uint32		num_buckets;
	uint32		spare_index;
	uint32		lshift;

	/*
	 * 选择初始桶页数量，使它在给定的估计元组数下
	 * 匹配填充因子。我们把结果向上取整到
	 * 使用其 hashm_spares 元素之前必须分配的
	 * 桶总数。不过始终至少强制 2 个桶页。
	 * 上限由 _hash_expandtable() 中说明的
	 * 考量决定。
	 */
	dnumbuckets = num_tuples / ffactor;
	if (dnumbuckets <= 2.0)
		num_buckets = 2;
	else if (dnumbuckets >= (double) 0x40000000)
		num_buckets = 0x40000000;
	else
		num_buckets = _hash_get_totalbuckets(_hash_spareindex(dnumbuckets));

	spare_index = _hash_spareindex(num_buckets);
	Assert(spare_index < HASH_MAX_SPLITPOINTS);

	page = BufferGetPage(buf);
	if (initpage)
		_hash_pageinit(page, BufferGetPageSize(buf));

	pageopaque = HashPageGetOpaque(page);
	pageopaque->hasho_prevblkno = InvalidBlockNumber;
	pageopaque->hasho_nextblkno = InvalidBlockNumber;
	pageopaque->hasho_bucket = InvalidBucket;
	pageopaque->hasho_flag = LH_META_PAGE;
	pageopaque->hasho_page_id = HASHO_PAGE_ID;

	metap = HashPageGetMeta(page);

	metap->hashm_magic = HASH_MAGIC;
	metap->hashm_version = HASH_VERSION;
	metap->hashm_ntuples = 0;
	metap->hashm_nmaps = 0;
	metap->hashm_ffactor = ffactor;
	metap->hashm_bsize = HashGetMaxBitmapSize(page);

	/* 找出能放进页大小的最大位图数组尺寸 */
	lshift = pg_leftmost_one_pos32(metap->hashm_bsize);
	Assert(lshift > 0);
	metap->hashm_bmsize = 1 << lshift;
	metap->hashm_bmshift = lshift + BYTE_TO_BIT;
	Assert((1 << BMPG_SHIFT(metap)) == (BMPG_MASK(metap) + 1));

	/*
	 * 用其主 hash 支持函数的 OID 标记该索引。这对
	 * 正常操作几乎没用（事实上 hashm_procid 在任何地方
	 * 都没有使用），但取证时也许用得上，所以保留它。
	 */
	metap->hashm_procid = procid;

	/*
	 * 我们用 N 个桶（0 .. N-1）初始化索引，它们占用物理
	 * 块 1 到 N。第一张空闲空间位图页在块 N+1。
	 */
	metap->hashm_maxbucket = num_buckets - 1;

	/*
	 * 把 highmask 设为紧接着的下一个 ((2 ^ x) - 1)，它应当
	 * 足以覆盖 num_buckets。
	 */
	metap->hashm_highmask = pg_nextpower2_32(num_buckets + 1) - 1;
	metap->hashm_lowmask = (metap->hashm_highmask >> 1);

	MemSet(metap->hashm_spares, 0, sizeof(metap->hashm_spares));
	MemSet(metap->hashm_mapp, 0, sizeof(metap->hashm_mapp));

	/* 为初始分裂点之后的一个备用页建立映射 */
	metap->hashm_spares[spare_index] = 1;
	metap->hashm_ovflpoint = spare_index;
	metap->hashm_firstfree = 0;

	/*
	 * 把 pd_lower 设到元数据末尾刚过去的位置。这是
	 * 必需的，因为若不这样做，xlog.c 压缩该页时
	 * 元数据就会丢失。
	 */
	((PageHeader) page)->pd_lower =
		((char *) metap + sizeof(HashMetaPageData)) - (char *) page;
}

/*
 *	_hash_pageinit() -- 初始化一个新的 hash 索引页。
 */
void
_hash_pageinit(Page page, Size size)
{
	PageInit(page, size, sizeof(HashPageOpaqueData));
}

/*
 * 尝试通过创建一个新桶来扩展 hash 表。
 *
 * 若我们拿不到旧桶或新桶上的清理锁，就静默地
 * 什么也不做。
 *
 * 完成尚未结束的分裂，并删除旧桶中
 * 上次分裂残留的元组（若还有的话）。
 *
 * 调用者必须钉住元页缓冲区，但不持锁。
 * 返回时该缓冲区处于同样的状态。
 */
void
_hash_expandtable(Relation rel, Buffer metabuf)
{
	HashMetaPage metap;
	Bucket		old_bucket;
	Bucket		new_bucket;
	uint32		spare_ndx;
	BlockNumber start_oblkno;
	BlockNumber start_nblkno;
	Buffer		buf_nblkno;
	Buffer		buf_oblkno;
	Page		opage;
	Page		npage;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	uint32		maxbucket;
	uint32		highmask;
	uint32		lowmask;
	bool		metap_update_masks = false;
	bool		metap_update_splitpoint = false;

restart_expand:

	/*
	 * 对元页加写锁。过去开始一次分裂必须取得
	 * 一把重量级锁，但现在不再需要。
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	_hash_checkpage(rel, metabuf, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	/*
	 * 检查是否仍需要分裂；等待锁期间也许已经有别人
	 * 做过一次了。
	 *
	 * 务必与 _hash_doinsert() 保持同步
	 */
	if (metap->hashm_ntuples <=
		(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1))
		goto fail;

	/*
	 * 若 maxbucket 已达到其可能的
	 * 最大值，就不能再分裂。
	 *
	 * 理想情况下我们允许桶号直到 UINT_MAX-1（不能再高，因为
	 * maxbucket+1 的计算不得溢出）。目前我们限制为
	 * 该值的一半，以防止 pg_ceil_log2_32() 失败以及
	 * hashm_spares[] 空间不足。反正这也无关紧要，因为拥有 2^32
	 * 个桶的索引肯定会使 BlockNumber 溢出，因而
	 * _hash_alloc_buckets() 会失败；但若我们支持小于一个
	 * 磁盘块的桶，这就会成为一条独立约束。
	 *
	 * 若改动这里，另见 _hash_init() 中初始桶数的
	 * 上限。
	 */
	if (metap->hashm_maxbucket >= (uint32) 0x7FFFFFFE)
		goto fail;

	/*
	 * 确定要分裂哪个桶，并尝试在旧桶上取得清理锁。
	 * 若拿不到锁，就放弃。
	 *
	 * 清理锁不仅防止其他后端，也防止
	 * 我们自己的后端。
	 *
	 * 清理锁主要是为了保护分裂不受并发插入影响。
	 * 详见 src/backend/access/hash/README 的 "锁定义"。
	 * 由于这一加锁限制，若存在任何未完成的扫描，
	 * 分裂就会放弃。这并不好，但无害。
	 */
	new_bucket = metap->hashm_maxbucket + 1;

	old_bucket = (new_bucket & metap->hashm_lowmask);

	start_oblkno = BUCKET_TO_BLKNO(metap, old_bucket);

	buf_oblkno = _hash_getbuf_with_condlock_cleanup(rel, start_oblkno, LH_BUCKET_PAGE);
	if (!buf_oblkno)
		goto fail;

	opage = BufferGetPage(buf_oblkno);
	oopaque = HashPageGetOpaque(opage);

	/*
	 * 我们希望做完来自一个桶的分裂，因为不做完
	 * 没有明显好处，而且考虑到新分裂也会失败的情形，
	 * 要把涉及多个桶的分裂做完会使代码变复杂。这里
	 * 完成分裂时不必考虑新桶，因为当旧桶仍有
	 * 未完成的分裂时，不可能开始对新桶
	 * 再次分裂。
	 */
	if (H_BUCKET_BEING_SPLIT(oopaque))
	{
		/*
		 * 现在复制桶映射信息；理由见下面
		 * 在调用 _hash_splitbucket 之前复制该信息
		 * 的那段注释。
		 */
		maxbucket = metap->hashm_maxbucket;
		highmask = metap->hashm_highmask;
		lowmask = metap->hashm_lowmask;

		/*
		 * 完成分裂之前，释放元页和 old_bucket 上的
		 * 锁。
		 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf_oblkno, BUFFER_LOCK_UNLOCK);

		_hash_finish_split(rel, metabuf, buf_oblkno, old_bucket, maxbucket,
						   highmask, lowmask);

		/* 释放旧缓冲区上的钉住，并重试扩展。 */
		_hash_dropbuf(rel, buf_oblkno);

		goto restart_expand;
	}

	/*
	 * 清理上次分裂残留的元组。该操作
	 * 需要清理锁，而旧桶上我们已经有一把，
	 * 所以现在就做。在上次分裂的垃圾清理完之前，
	 * 我们也不想允许该桶再分裂。这样做有
	 * 两个好处：第一，有助于避免垃圾造成的膨胀；
	 * 第二，清理桶期间我们总能确定这些垃圾
	 * 元组属于最近一次被分裂的桶。相反，若允许
	 * 在元页已更新以表示新分裂之后、真正分裂之前
	 * 就清理该桶，清理操作将无法判断元组是否
	 * 已被移到新建的桶，最终就会删掉
	 * 这样的元组。
	 */
	if (H_NEEDS_SPLIT_CLEANUP(oopaque))
	{
		/*
		 * 现在复制桶映射信息；理由见下面代码里
		 * 调用 _hash_splitbucket 之前复制该信息
		 * 的那段注释。
		 */
		maxbucket = metap->hashm_maxbucket;
		highmask = metap->hashm_highmask;
		lowmask = metap->hashm_lowmask;

		/* 释放元页锁。 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

		hashbucketcleanup(rel, old_bucket, buf_oblkno, start_oblkno, NULL,
						  maxbucket, highmask, lowmask, NULL, NULL, true,
						  NULL, NULL);

		_hash_dropbuf(rel, buf_oblkno);

		goto restart_expand;
	}

	/*
	 * 新桶上不应当有任何活动扫描。
	 *
	 * 注意：即使这里可能仍需更新 BUCKET_TO_BLKNO 映射，
	 * 现在计算新桶的块号也是安全的。这是因为
	 * hashm_spares[hashm_ovflpoint] 的当前值正确地指出了
	 * 我们将把新分裂点那一批桶放在哪里。
	 */
	start_nblkno = BUCKET_TO_BLKNO(metap, new_bucket);

	/*
	 * 若分裂点在增大，就需要分配新的一批
	 * 桶页。
	 */
	spare_ndx = _hash_spareindex(new_bucket + 1);
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		uint32		buckets_to_add;

		Assert(spare_ndx == metap->hashm_ovflpoint + 1);

		/*
		 * 我们把桶的分配当作单独的、记入 WAL 的动作。
		 * 即使这次操作之后失败，也不会泄漏桶页；
		 * 下次分裂会用掉这块空间。无论如何，即使
		 * 没有失败，一次分裂操作也不会用完全部空间。
		 */
		buckets_to_add = _hash_get_totalbuckets(spare_ndx) - new_bucket;
		if (!_hash_alloc_buckets(rel, start_nblkno, buckets_to_add))
		{
			/* 因 BlockNumber 溢出而无法分裂 */
			_hash_relbuf(rel, buf_oblkno);
			goto fail;
		}
	}

	/*
	 * 物理分配新桶的主页。我们希望在改元页的
	 * 映射信息之前做这件事，以防无法取得
	 * 磁盘空间。
	 *
	 * XXX 先调用 _hash_getnewbuf 把缓冲区清零，
	 * 然后再检查是否持有清理锁，这样做没什么道理。
	 * 不过此时还不会有扫描访问该缓冲区，任何并发
	 * 访问只会来自后台写入进程或检查点进程这类
	 * 并不关心其内容的进程，所以其实无所谓。
	 */
	buf_nblkno = _hash_getnewbuf(rel, start_nblkno, MAIN_FORKNUM);
	if (!IsBufferCleanupOK(buf_nblkno))
	{
		_hash_relbuf(rel, buf_oblkno);
		_hash_relbuf(rel, buf_nblkno);
		goto fail;
	}

	/*
	 * 既然我们在直接改写共享缓冲区中的页，就建立一个
	 * 临界区。接下来这段代码若失败，会留下大麻烦：
	 * 元页实际上已损坏，却可能被写回
	 * 磁盘。
	 */
	START_CRIT_SECTION();

	/*
	 * 可以继续分裂了。更新元页中的桶映射信息。
	 */
	metap->hashm_maxbucket = new_bucket;

	if (new_bucket > metap->hashm_highmask)
	{
		/* 开始新一轮加倍 */
		metap->hashm_lowmask = metap->hashm_highmask;
		metap->hashm_highmask = new_bucket | metap->hashm_lowmask;
		metap_update_masks = true;
	}

	/*
	 * 若分裂点在增大，就需要调整 hashm_spares[]
	 * 数组和 hashm_ovflpoint，使以后的溢出页创建在
	 * 这新一批桶页之后。
	 */
	if (spare_ndx > metap->hashm_ovflpoint)
	{
		metap->hashm_spares[spare_ndx] = metap->hashm_spares[metap->hashm_ovflpoint];
		metap->hashm_ovflpoint = spare_ndx;
		metap_update_splitpoint = true;
	}

	MarkBufferDirty(metabuf);

	/*
	 * 现在复制桶映射信息；这样 _hash_splitbucket 的内层循环
	 * 就不必再次访问元页。注意，一旦我们放下分裂锁，
	 * 其他分裂就可能开始，因此这些值可能在
	 * _hash_splitbucket 结束前过时。这没关系，因为它只需要
	 * 判断该把哈希键映射到这两个桶中的哪一个。
	 */
	maxbucket = metap->hashm_maxbucket;
	highmask = metap->hashm_highmask;
	lowmask = metap->hashm_lowmask;

	opage = BufferGetPage(buf_oblkno);
	oopaque = HashPageGetOpaque(opage);

	/*
	 * 标记旧桶，表示分裂正在进行。（操作结束时，
	 * 我们会清除正在分裂标志。）另外，对于主桶页，
	 * hasho_prevblkno 存放截至上次分裂时已经存在的
	 * 桶数，所以必须在这里更新该值。
	 */
	oopaque->hasho_flag |= LH_BUCKET_BEING_SPLIT;
	oopaque->hasho_prevblkno = maxbucket;

	MarkBufferDirty(buf_oblkno);

	npage = BufferGetPage(buf_nblkno);

	/*
	 * 初始化新桶的主页，并标记它以表示
	 * 分裂正在进行。
	 */
	nopaque = HashPageGetOpaque(npage);
	nopaque->hasho_prevblkno = maxbucket;
	nopaque->hasho_nextblkno = InvalidBlockNumber;
	nopaque->hasho_bucket = new_bucket;
	nopaque->hasho_flag = LH_BUCKET_PAGE | LH_BUCKET_BEING_POPULATED;
	nopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(buf_nblkno);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_split_allocate_page xlrec;
		XLogRecPtr	recptr;

		xlrec.new_bucket = maxbucket;
		xlrec.old_bucket_flag = oopaque->hasho_flag;
		xlrec.new_bucket_flag = nopaque->hasho_flag;
		xlrec.flags = 0;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf_oblkno, REGBUF_STANDARD);
		XLogRegisterBuffer(1, buf_nblkno, REGBUF_WILL_INIT);
		XLogRegisterBuffer(2, metabuf, REGBUF_STANDARD);

		if (metap_update_masks)
		{
			xlrec.flags |= XLH_SPLIT_META_UPDATE_MASKS;
			XLogRegisterBufData(2, (char *) &metap->hashm_lowmask, sizeof(uint32));
			XLogRegisterBufData(2, (char *) &metap->hashm_highmask, sizeof(uint32));
		}

		if (metap_update_splitpoint)
		{
			xlrec.flags |= XLH_SPLIT_META_UPDATE_SPLITPOINT;
			XLogRegisterBufData(2, (char *) &metap->hashm_ovflpoint,
								sizeof(uint32));
			XLogRegisterBufData(2,
								(char *) &metap->hashm_spares[metap->hashm_ovflpoint],
								sizeof(uint32));
		}

		XLogRegisterData((char *) &xlrec, SizeOfHashSplitAllocPage);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_ALLOCATE_PAGE);

		PageSetLSN(BufferGetPage(buf_oblkno), recptr);
		PageSetLSN(BufferGetPage(buf_nblkno), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	/* 放下锁，但保持钉住 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/* 把记录搬到新桶 */
	_hash_splitbucket(rel, metabuf,
					  old_bucket, new_bucket,
					  buf_oblkno, buf_nblkno, NULL,
					  maxbucket, highmask, lowmask);

	/* 全部完成，现在释放主桶上的钉住。 */
	_hash_dropbuf(rel, buf_oblkno);
	_hash_dropbuf(rel, buf_nblkno);

	return;

	/* 决定不分裂，或未能取得旧桶锁时走到这里 */
fail:

	/* 我们没有写元页，所以只放下锁 */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
}


/*
 * _hash_alloc_buckets -- 分配一个新分裂点所需的桶页
 *
 * 这里不必初始化新的桶页；等 _hash_expandtable() 用到
 * 每一页时再做。但必须把逻辑 EOF 延伸到分裂点的末尾；
 * 这使 smgr 对 EOF 的看法与我们保持同步，
 * 从而不会收到 smgr 的抱怨。
 *
 * 做法是在分裂点范围的末尾写一页零。我们预期文件系统
 * 会保证中间那些页读出来也是零。在许多文件系统上，
 * 这个 "空洞" 不会被立即分配，这意味着索引文件最终
 * 可能比现在就强制全部分配更加碎片化；但由于我们
 * 本来就不会顺序扫描 hash 索引，所以这大概
 * 不要紧。
 *
 * XXX 这段代码在持有元页锁时执行，这挺烦人。
 * 我们需要与并发的 _hash_addovflpage() 增加新溢出页
 * 互锁，但为此使用 LockRelationForExtension 可能更好。
 * 另一方面，增加一个分裂点是非常少见的操作，
 * 所以也许不值得操心。
 *
 * 成功则返回 true，若因 BlockNumber 溢出导致分配失败
 * 则返回 false。
 */
static bool
_hash_alloc_buckets(Relation rel, BlockNumber firstblock, uint32 nblocks)
{
	BlockNumber lastblock;
	PGIOAlignedBlock zerobuf;
	Page		page;
	HashPageOpaque ovflopaque;

	lastblock = firstblock + nblocks - 1;

	/*
	 * 检查块号计算是否溢出；若是，就无法再
	 * 扩展该索引。
	 */
	if (lastblock < firstblock || lastblock == InvalidBlockNumber)
		return false;

	page = (Page) zerobuf.data;

	/*
	 * 初始化该页。只把页清零是不行的；类似用法见
	 * _hash_freeovflpage。我们特意让特殊空间有效，
	 * 以方便 pageinspect 这类工具。
	 */
	_hash_pageinit(page, BLCKSZ);

	ovflopaque = HashPageGetOpaque(page);

	ovflopaque->hasho_prevblkno = InvalidBlockNumber;
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = InvalidBucket;
	ovflopaque->hasho_flag = LH_UNUSED_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	if (RelationNeedsWAL(rel))
		log_newpage(&rel->rd_locator,
					MAIN_FORKNUM,
					lastblock,
					zerobuf.data,
					true);

	PageSetChecksumInplace(page, lastblock);
	smgrextend(RelationGetSmgr(rel), MAIN_FORKNUM, lastblock, zerobuf.data,
			   false);

	return true;
}


/*
 * _hash_splitbucket -- 把 'obucket' 分裂成 'obucket' 和 'nbucket'
 *
 * 本例程用于在旧桶和新桶之间划分元组，
 * 也用于完成未做完的分裂操作。要完成先前被
 * 中断的分裂操作，调用者需要填好 htab。若设置了
 * htab，就跳过已存在于 htab 中的那些元组的移动，
 * 否则 htab 为 NULL 表示把属于新桶的
 * 全部元组都移动过去。
 *
 * 我们正在分裂的桶由一张基桶页以及零张或
 * 多张溢出（桶链）页组成。必须把属于新桶的
 * 元组搬过去。
 *
 * 调用者必须在两个桶上都持有清理锁，以确保
 * 没有别人试图访问它们（见 README）。
 *
 * 调用者必须钉住元页缓冲区，但不持锁。
 * 返回时该缓冲区处于同样的状态。（只有在需要
 * 增加或删除溢出页时才会碰元页。）
 *
 * 分裂必须一直保持新旧两个桶的主桶页上的钉住，
 * 直到操作结束。这是为了防止分裂进行期间
 * VACUUM 开始工作。
 *
 * 此外，调用者必须已经创建新桶的基页，
 * 它通过缓冲区 nbuf 传入，已钉住并加上写锁。锁会在
 * 这里释放，钉住必须由调用者释放。（API 这样设计
 * 是因为我们必须在释放元页写锁之前做 _hash_getnewbuf()。
 * 因此我们不传递新桶的起始块号，
 * 而是传递一个实际的缓冲区。）
 */
static void
_hash_splitbucket(Relation rel,
				  Buffer metabuf,
				  Bucket obucket,
				  Bucket nbucket,
				  Buffer obuf,
				  Buffer nbuf,
				  HTAB *htab,
				  uint32 maxbucket,
				  uint32 highmask,
				  uint32 lowmask)
{
	Buffer		bucket_obuf;
	Buffer		bucket_nbuf;
	Page		opage;
	Page		npage;
	HashPageOpaque oopaque;
	HashPageOpaque nopaque;
	OffsetNumber itup_offsets[MaxIndexTuplesPerPage];
	IndexTuple	itups[MaxIndexTuplesPerPage];
	Size		all_tups_size = 0;
	int			i;
	uint16		nitups = 0;

	bucket_obuf = obuf;
	opage = BufferGetPage(obuf);
	oopaque = HashPageGetOpaque(opage);

	bucket_nbuf = nbuf;
	npage = BufferGetPage(nbuf);
	nopaque = HashPageGetOpaque(npage);

	/* 把谓词锁从旧桶复制到新桶。 */
	PredicateLockPageSplit(rel,
						   BufferGetBlockNumber(bucket_obuf),
						   BufferGetBlockNumber(bucket_nbuf));

	/*
	 * 把旧桶中的元组划分到旧桶和新桶，沿旧桶的
	 * 溢出桶链向前，并按需为新桶增加溢出页。
	 * 外层循环对旧桶中的每一页迭代
	 * 一次。
	 */
	for (;;)
	{
		BlockNumber oblkno;
		OffsetNumber ooffnum;
		OffsetNumber omaxoffnum;

		/* 扫描旧页中的每个元组 */
		omaxoffnum = PageGetMaxOffsetNumber(opage);
		for (ooffnum = FirstOffsetNumber;
			 ooffnum <= omaxoffnum;
			 ooffnum = OffsetNumberNext(ooffnum))
		{
			IndexTuple	itup;
			Size		itemsz;
			Bucket		bucket;
			bool		found = false;

			/* 跳过死元组 */
			if (ItemIdIsDead(PageGetItemId(opage, ooffnum)))
				continue;

			/*
			 * 插入元组之前，先探测含有属于新桶的
			 * 那些元组的 TIDs 的哈希表；若找到匹配，
			 * 就跳过该元组，否则取出该项的哈希键
			 * （它方便地存放在该项中），并确定它现在
			 * 属于哪个桶。
			 */
			itup = (IndexTuple) PageGetItem(opage,
											PageGetItemId(opage, ooffnum));

			if (htab)
				(void) hash_search(htab, &itup->t_tid, HASH_FIND, &found);

			if (found)
				continue;

			bucket = _hash_hashkey2bucket(_hash_get_indextuple_hashkey(itup),
										  maxbucket, highmask, lowmask);

			if (bucket == nbucket)
			{
				IndexTuple	new_itup;

				/*
				 * 复制一份索引元组，因为我们得在它上面直接改写。
				 */
				new_itup = CopyIndexTuple(itup);

				/*
				 * 把索引元组标为因分裂而移动的；若某桶的分裂
				 * 正在进行，扫描会跳过这类元组。
				 */
				new_itup->t_info |= INDEX_MOVED_BY_SPLIT_MASK;

				/*
				 * 把元组插入新桶。若它放不进新桶的当前页，
				 * 就必须分配一张新的溢出页，改为把元组
				 * 放在那一页上。
				 */
				itemsz = IndexTupleSize(new_itup);
				itemsz = MAXALIGN(itemsz);

				if (PageGetFreeSpaceForMultipleTuples(npage, nitups + 1) < (all_tups_size + itemsz))
				{
					/*
					 * 在临界区中更改共享缓冲区状态，
					 * 否则任何错误都可能使它无法恢复。
					 */
					START_CRIT_SECTION();

					_hash_pgaddmultitup(rel, nbuf, itups, itup_offsets, nitups);
					MarkBufferDirty(nbuf);
					/* 释放锁之前把分裂操作记入日志 */
					log_split_page(rel, nbuf);

					END_CRIT_SECTION();

					/* 放下锁，但保持钉住 */
					LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);

					/* 收拾干净 */
					for (i = 0; i < nitups; i++)
						pfree(itups[i]);
					nitups = 0;
					all_tups_size = 0;

					/* 链接到一张新的溢出页 */
					nbuf = _hash_addovflpage(rel, metabuf, nbuf, (nbuf == bucket_nbuf));
					npage = BufferGetPage(nbuf);
					nopaque = HashPageGetOpaque(npage);
				}

				itups[nitups++] = new_itup;
				all_tups_size += itemsz;
			}
			else
			{
				/*
				 * 元组留在这一页，所以无事可做。
				 */
				Assert(bucket == obucket);
			}
		}

		oblkno = oopaque->hasho_nextblkno;

		/* 保留旧主桶上的钉住 */
		if (obuf == bucket_obuf)
			LockBuffer(obuf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, obuf);

		/* 旧桶中不再有溢出页时退出循环 */
		if (!BlockNumberIsValid(oblkno))
		{
			/*
			 * 在临界区中更改共享缓冲区状态，否则
			 * 任何错误都可能使它无法恢复。
			 */
			START_CRIT_SECTION();

			_hash_pgaddmultitup(rel, nbuf, itups, itup_offsets, nitups);
			MarkBufferDirty(nbuf);
			/* 释放锁之前把分裂操作记入日志 */
			log_split_page(rel, nbuf);

			END_CRIT_SECTION();

			if (nbuf == bucket_nbuf)
				LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);
			else
				_hash_relbuf(rel, nbuf);

			/* 收拾干净 */
			for (i = 0; i < nitups; i++)
				pfree(itups[i]);
			break;
		}

		/* 否则，前进到下一张旧页 */
		obuf = _hash_getbuf(rel, oblkno, HASH_READ, LH_OVERFLOW_PAGE);
		opage = BufferGetPage(obuf);
		oopaque = HashPageGetOpaque(opage);
	}

	/*
	 * 我们已经到了旧桶链的末尾，元组划分
	 * 完成。标记新旧桶，表示分裂已经
	 * 结束。
	 *
	 * 为避免因桶的加锁顺序导致死锁，先锁旧桶，
	 * 再锁新桶。
	 */
	LockBuffer(bucket_obuf, BUFFER_LOCK_EXCLUSIVE);
	opage = BufferGetPage(bucket_obuf);
	oopaque = HashPageGetOpaque(opage);

	LockBuffer(bucket_nbuf, BUFFER_LOCK_EXCLUSIVE);
	npage = BufferGetPage(bucket_nbuf);
	nopaque = HashPageGetOpaque(npage);

	START_CRIT_SECTION();

	oopaque->hasho_flag &= ~LH_BUCKET_BEING_SPLIT;
	nopaque->hasho_flag &= ~LH_BUCKET_BEING_POPULATED;

	/*
	 * 分裂结束后，标记旧桶，表示它含有可删除的元组。
	 * 我们会在删除这类元组之后清除分裂清理标志，
	 * 删除发生在分裂结束时、下次从旧桶分裂时，
	 * 或者 VACUUM 时。
	 */
	oopaque->hasho_flag |= LH_BUCKET_NEEDS_SPLIT_CLEANUP;

	/*
	 * 现在把缓冲区写出去；这里不释放锁，因为
	 * 调用者负责释放锁。
	 */
	MarkBufferDirty(bucket_obuf);
	MarkBufferDirty(bucket_nbuf);

	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_hash_split_complete xlrec;

		xlrec.old_bucket_flag = oopaque->hasho_flag;
		xlrec.new_bucket_flag = nopaque->hasho_flag;

		XLogBeginInsert();

		XLogRegisterData((char *) &xlrec, SizeOfHashSplitComplete);

		XLogRegisterBuffer(0, bucket_obuf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, bucket_nbuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_COMPLETE);

		PageSetLSN(BufferGetPage(bucket_obuf), recptr);
		PageSetLSN(BufferGetPage(bucket_nbuf), recptr);
	}

	END_CRIT_SECTION();

	/*
	 * 若可能，就清理旧桶。若别人在它上面持有钉住，
	 * 我们也许做不到，但若没有就可以进行。这并非
	 * 绝对必要，但能减少膨胀；若现在不做，
	 * VACUUM 最终会做，但也许要等到新的溢出页
	 * 已经被分配。注意没有必要清理
	 * 新桶。
	 */
	if (IsBufferCleanupOK(bucket_obuf))
	{
		LockBuffer(bucket_nbuf, BUFFER_LOCK_UNLOCK);
		hashbucketcleanup(rel, obucket, bucket_obuf,
						  BufferGetBlockNumber(bucket_obuf), NULL,
						  maxbucket, highmask, lowmask, NULL, NULL, true,
						  NULL, NULL);
	}
	else
	{
		LockBuffer(bucket_nbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(bucket_obuf, BUFFER_LOCK_UNLOCK);
	}
}

/*
 *	_hash_finish_split() -- 完成先前被中断的分裂操作
 *
 * 为完成分裂操作，我们建立新桶中 TIDs 的哈希表，
 * 供分裂操作用来跳过那些在先前分裂被中断之前
 * 就已经移走的元组。
 *
 * 调用者必须钉住元页以及旧桶的主页缓冲区，
 * 但不持锁。返回时这些缓冲区处于同样的状态。
 * （只有在需要增加或删除溢出页时才会碰
 * 元页。）
 */
void
_hash_finish_split(Relation rel, Buffer metabuf, Buffer obuf, Bucket obucket,
				   uint32 maxbucket, uint32 highmask, uint32 lowmask)
{
	HASHCTL		hash_ctl;
	HTAB	   *tidhtab;
	Buffer		bucket_nbuf = InvalidBuffer;
	Buffer		nbuf;
	Page		npage;
	BlockNumber nblkno;
	BlockNumber bucket_nblkno;
	HashPageOpaque npageopaque;
	Bucket		nbucket;
	bool		found;

	/* 初始化用于跟踪 TIDs 的哈希表 */
	hash_ctl.keysize = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(ItemPointerData);
	hash_ctl.hcxt = CurrentMemoryContext;

	tidhtab =
		hash_create("bucket ctids",
					256,		/* 任意的初始大小 */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	bucket_nblkno = nblkno = _hash_get_newblock_from_oldbucket(rel, obucket);

	/*
	 * 扫描新桶并建立 TIDs 的哈希表
	 */
	for (;;)
	{
		OffsetNumber noffnum;
		OffsetNumber nmaxoffnum;

		nbuf = _hash_getbuf(rel, nblkno, HASH_READ,
							LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

		/* 记住主桶缓冲区，以便在其上取得清理锁。 */
		if (nblkno == bucket_nblkno)
			bucket_nbuf = nbuf;

		npage = BufferGetPage(nbuf);
		npageopaque = HashPageGetOpaque(npage);

		/* 扫描新页中的每个元组 */
		nmaxoffnum = PageGetMaxOffsetNumber(npage);
		for (noffnum = FirstOffsetNumber;
			 noffnum <= nmaxoffnum;
			 noffnum = OffsetNumberNext(noffnum))
		{
			IndexTuple	itup;

			/* 取出该项的 TID 并插入哈希表。 */
			itup = (IndexTuple) PageGetItem(npage,
											PageGetItemId(npage, noffnum));

			(void) hash_search(tidhtab, &itup->t_tid, HASH_ENTER, &found);

			Assert(!found);
		}

		nblkno = npageopaque->hasho_nextblkno;

		/*
		 * 不修改缓冲区就释放写锁，并确保保留
		 * 主桶上的钉住。
		 */
		if (nbuf == bucket_nbuf)
			LockBuffer(nbuf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, nbuf);

		/* 新桶中不再有溢出页时退出循环 */
		if (!BlockNumberIsValid(nblkno))
			break;
	}

	/*
	 * 有条件地取得新旧桶上的清理锁以执行
	 * 分裂操作。若拿不到清理锁，就静默放弃，
	 * 下次向旧桶插入时会重试以完成
	 * 这次分裂。
	 */
	if (!ConditionalLockBufferForCleanup(obuf))
	{
		hash_destroy(tidhtab);
		return;
	}
	if (!ConditionalLockBufferForCleanup(bucket_nbuf))
	{
		LockBuffer(obuf, BUFFER_LOCK_UNLOCK);
		hash_destroy(tidhtab);
		return;
	}

	npage = BufferGetPage(bucket_nbuf);
	npageopaque = HashPageGetOpaque(npage);
	nbucket = npageopaque->hasho_bucket;

	_hash_splitbucket(rel, metabuf, obucket,
					  nbucket, obuf, bucket_nbuf, tidhtab,
					  maxbucket, highmask, lowmask);

	_hash_dropbuf(rel, bucket_nbuf);
	hash_destroy(tidhtab);
}

/*
 *	log_split_page() -- 把分裂操作记入日志
 *
 *	当新桶中的新页变满时，我们记录这次分裂操作，
 *	因此我们记录整页。
 *
 *	调用者必须锁住 'buf'，并且也负责解开
 *	它。
 */
static void
log_split_page(Relation rel, Buffer buf)
{
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_PAGE);

		PageSetLSN(BufferGetPage(buf), recptr);
	}
}

/*
 *	_hash_getcachedmetap() -- 返回缓存的元页数据。
 *
 *	若 metabuf 不是 InvalidBuffer，调用者必须钉住元页，
 *	但不持锁。若未设置，在必须刷新缓存时我们会在返回前
 *	设置它，并在返回时保持钉住但不持锁；调用者
 *	负责释放该钉住。
 *
 *	若缓存尚未初始化，或 force_refresh 为 true，我们就刷新缓存。
 */
HashMetaPage
_hash_getcachedmetap(Relation rel, Buffer *metabuf, bool force_refresh)
{
	Page		page;

	Assert(metabuf);
	if (force_refresh || rel->rd_amcache == NULL)
	{
		char	   *cache = NULL;

		/*
		 * 重要的是不要把 rd_amcache 设成无效值。
		 * MemoryContextAlloc 或 _hash_getbuf 都可能失败，所以
		 * 在二者都成功之前，不要把指向新分配存储的指针
		 * 装进实际的 relcache 项。
		 */
		if (rel->rd_amcache == NULL)
			cache = MemoryContextAlloc(rel->rd_indexcxt,
									   sizeof(HashMetaPageData));

		/* 读取元页。 */
		if (BufferIsValid(*metabuf))
			LockBuffer(*metabuf, BUFFER_LOCK_SHARE);
		else
			*metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ,
									LH_META_PAGE);
		page = BufferGetPage(*metabuf);

		/* 填充缓存。 */
		if (rel->rd_amcache == NULL)
			rel->rd_amcache = cache;
		memcpy(rel->rd_amcache, HashPageGetMeta(page),
			   sizeof(HashMetaPageData));

		/* 释放元页锁，但保持钉住。 */
		LockBuffer(*metabuf, BUFFER_LOCK_UNLOCK);
	}

	return (HashMetaPage) rel->rd_amcache;
}

/*
 *	_hash_getbucketbuf_from_hashkey() -- 取得给定 hashkey 对应
 *										 桶的缓冲区。
 *
 *	桶页一旦分配就不会移动或被删除。这使我们有机会
 *	用先前保存的元页内容到达目标桶缓冲区，而不必
 *	每次都从元页读取。这样每次要到达目标桶缓冲区时
 *	都省一次缓冲区访问，对减少 bufmgr 流量和争用
 *	很有帮助。
 *
 *	访问类型参数（HASH_READ 或 HASH_WRITE）指出桶缓冲区
 *	必须为读还是为写而加锁。
 *
 *	输出参数 cachedmetap 被设为用于 hashkey 到桶缓冲区
 *	映射的元页内容。有些调用者需要此信息以便在桶分裂时
 *	到达旧桶，见 _hash_doinsert()。
 */
Buffer
_hash_getbucketbuf_from_hashkey(Relation rel, uint32 hashkey, int access,
								HashMetaPage *cachedmetap)
{
	HashMetaPage metap;
	Buffer		buf;
	Buffer		metabuf = InvalidBuffer;
	Page		page;
	Bucket		bucket;
	BlockNumber blkno;
	HashPageOpaque opaque;

	/* 我们要读目标桶缓冲区，因此必须加锁。 */
	Assert(access == HASH_READ || access == HASH_WRITE);

	metap = _hash_getcachedmetap(rel, &metabuf, false);
	Assert(metap != NULL);

	/*
	 * 循环，直到我们在正确的目标桶上取得锁。
	 */
	for (;;)
	{
		/*
		 * 计算目标桶号，并转换成块号。
		 */
		bucket = _hash_hashkey2bucket(hashkey,
									  metap->hashm_maxbucket,
									  metap->hashm_highmask,
									  metap->hashm_lowmask);

		blkno = BUCKET_TO_BLKNO(metap, bucket);

		/* 取出该桶的主桶页 */
		buf = _hash_getbuf(rel, blkno, access, LH_BUCKET_PAGE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
		Assert(opaque->hasho_bucket == bucket);
		Assert(opaque->hasho_prevblkno != InvalidBlockNumber);

		/*
		 * 若该桶尚未被分裂，就完成了。
		 */
		if (opaque->hasho_prevblkno <= metap->hashm_maxbucket)
			break;

		/* 放下该缓冲区上的锁，更新缓存的元页，然后重试。 */
		_hash_relbuf(rel, buf);
		metap = _hash_getcachedmetap(rel, &metabuf, true);
		Assert(metap != NULL);
	}

	if (BufferIsValid(metabuf))
		_hash_dropbuf(rel, metabuf);

	if (cachedmetap)
		*cachedmetap = metap;

	return buf;
}
