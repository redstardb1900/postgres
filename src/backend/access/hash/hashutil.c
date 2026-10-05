/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Postgres hash 实现的实用代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashutil.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "port/pg_bitutils.h"
#include "storage/buf_internals.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#define CALC_NEW_BUCKET(old_bucket, lowmask) \
			old_bucket | (lowmask + 1)

/*
 * _hash_checkqual -- 索引元组是否满足扫描条件？
 */
bool
_hash_checkqual(IndexScanDesc scan, IndexTuple itup)
{
	/*
	 * 目前无法检查任何扫描条件，因为我们没有
	 * 可供 sk_func 使用的原始索引项值。始终
	 * 返回 true；我们预期 hashgettuple 已经设置了 recheck 标志，
	 * 让主索引扫描代码去做这项检查。
	 */
#ifdef NOT_USED
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	ScanKey		key = scan->keyData;
	int			scanKeySize = scan->numberOfKeys;

	while (scanKeySize > 0)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;

		datum = index_getattr(itup,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		/* 假定 sk_func 是严格的 */
		if (isNull)
			return false;
		if (key->sk_flags & SK_ISNULL)
			return false;

		test = FunctionCall2Coll(&key->sk_func, key->sk_collation,
								 datum, key->sk_argument);

		if (!DatumGetBool(test))
			return false;

		key++;
		scanKeySize--;
	}
#endif

	return true;
}

/*
 * _hash_datum2hashkey -- 给定一个 Datum，调用索引的哈希函数
 *
 * 假定该 Datum 属于索引列的类型，因此可以使用
 * 通用索引代码为我们跟踪的 "primary" 哈希函数。
 */
uint32
_hash_datum2hashkey(Relation rel, Datum key)
{
	FmgrInfo   *procinfo;
	Oid			collation;

	/* XXX 假定索引只有一个属性 */
	procinfo = index_getprocinfo(rel, 1, HASHSTANDARD_PROC);
	collation = rel->rd_indcollation[0];

	return DatumGetUInt32(FunctionCall1Coll(procinfo, collation, key));
}

/*
 * _hash_datum2hashkey_type -- 给定指定类型的 Datum，
 *			以与本索引兼容的方式对其做哈希
 *
 * 这比 _hash_datum2hashkey 昂贵得多，因此只在
 * 跨类型情形下使用。
 */
uint32
_hash_datum2hashkey_type(Relation rel, Datum key, Oid keytype)
{
	RegProcedure hash_proc;
	Oid			collation;

	/* XXX 假定索引只有一个属性 */
	hash_proc = get_opfamily_proc(rel->rd_opfamily[0],
								  keytype,
								  keytype,
								  HASHSTANDARD_PROC);
	if (!RegProcedureIsValid(hash_proc))
		elog(ERROR, "missing support function %d(%u,%u) for index \"%s\"",
			 HASHSTANDARD_PROC, keytype, keytype,
			 RelationGetRelationName(rel));
	collation = rel->rd_indcollation[0];

	return DatumGetUInt32(OidFunctionCall1Coll(hash_proc, collation, key));
}

/*
 * _hash_hashkey2bucket -- 确定 hashkey 映射到哪个桶。
 */
Bucket
_hash_hashkey2bucket(uint32 hashkey, uint32 maxbucket,
					 uint32 highmask, uint32 lowmask)
{
	Bucket		bucket;

	bucket = hashkey & highmask;
	if (bucket > maxbucket)
		bucket = bucket & lowmask;

	return bucket;
}

/*
 * _hash_spareindex -- 返回该桶的 spare 下标，即其全局
 *					   分裂点阶段
 */
uint32
_hash_spareindex(uint32 num_bucket)
{
	uint32		splitpoint_group;
	uint32		splitpoint_phases;

	splitpoint_group = pg_ceil_log2_32(num_bucket);

	if (splitpoint_group < HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)
		return splitpoint_group;

	/* 计入单阶段组 */
	splitpoint_phases = HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE;

	/* 计入 splitpoint_group 之前的多阶段组 */
	splitpoint_phases +=
		((splitpoint_group - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) <<
		 HASH_SPLITPOINT_PHASE_BITS);

	/* 计入当前组内的各阶段 */
	splitpoint_phases +=
		(((num_bucket - 1) >>
		  (splitpoint_group - (HASH_SPLITPOINT_PHASE_BITS + 1))) &
		 HASH_SPLITPOINT_PHASE_MASK);	/* 换成从 0 起算的值。 */

	return splitpoint_phases;
}

/*
 *	_hash_get_totalbuckets -- 返回直到给定分裂点阶段为止
 *							已分配的桶总数。
 */
uint32
_hash_get_totalbuckets(uint32 splitpoint_phase)
{
	uint32		splitpoint_group;
	uint32		total_buckets;
	uint32		phases_within_splitpoint_group;

	if (splitpoint_phase < HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)
		return (1 << splitpoint_phase);

	/* 取得分裂点所属的组 */
	splitpoint_group = HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE;
	splitpoint_group +=
		((splitpoint_phase - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) >>
		 HASH_SPLITPOINT_PHASE_BITS);

	/* 计入 splitpoint_group 之前的桶 */
	total_buckets = (1 << (splitpoint_group - 1));

	/* 计入 splitpoint_group 之内的桶 */
	phases_within_splitpoint_group =
		(((splitpoint_phase - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) &
		  HASH_SPLITPOINT_PHASE_MASK) + 1); /* 从 0 起算改为从 1 起算 */
	total_buckets +=
		(((1 << (splitpoint_group - 1)) >> HASH_SPLITPOINT_PHASE_BITS) *
		 phases_within_splitpoint_group);

	return total_buckets;
}

/*
 * _hash_checkpage -- 对所有 hash 页的格式做健全性检查
 *
 * 若 flags 非零，它是可接受页类型的按位 OR
 * （hasho_flag 与 LH_PAGE_TYPE 的取值）。
 */
void
_hash_checkpage(Relation rel, Buffer buf, int flags)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer 会验证每个新读入的页都通过
	 * PageHeaderIsValid，这意味着它要么含有相当合理的
	 * 页头，要么全为零。不过我们必须防范全零的
	 * 情形。
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * 另外检查特殊空间看起来是否正常。
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(HashPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	if (flags)
	{
		HashPageOpaque opaque = HashPageGetOpaque(page);

		if ((opaque->hasho_flag & flags) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" contains corrupted page at block %u",
							RelationGetRelationName(rel),
							BufferGetBlockNumber(buf)),
					 errhint("Please REINDEX it.")));
	}

	/*
	 * 检查元页时，还要验证魔数和版本。
	 */
	if (flags == LH_META_PAGE)
	{
		HashMetaPage metap = HashPageGetMeta(page);

		if (metap->hashm_magic != HASH_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" is not a hash index",
							RelationGetRelationName(rel))));

		if (metap->hashm_version != HASH_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has wrong hash version",
							RelationGetRelationName(rel)),
					 errhint("Please REINDEX it.")));
	}
}

bytea *
hashoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(HashOptions, fillfactor)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_HASH,
									  sizeof(HashOptions),
									  tab, lengthof(tab));
}

/*
 * _hash_get_indextuple_hashkey - 取得 hash 索引元组的哈希键值
 */
uint32
_hash_get_indextuple_hashkey(IndexTuple itup)
{
	char	   *attp;

	/*
	 * 假定哈希键是第一个属性且不能为 null，因此
	 * 可以做得比较粗糙，但非常非常便宜 ...
	 */
	attp = (char *) itup + IndexInfoFindDataOffset(itup->t_info);
	return *((uint32 *) attp);
}

/*
 * _hash_convert_tuple - 把原始索引数据转换为哈希键
 *
 * 输入：用户数据列 column(s) 的 values 与 isnull 数组
 * 输出：索引元组的 values 与 isnull 数组，适合
 *		传给 index_form_tuple()。
 *
 * 成功返回 true，否则返回 false（因为存在空值）。
 * 结果为 false 时，给定数据不必被索引。
 *
 * 注意：调用者知道索引列数组的长度始终为 1。
 * 原则上输入列可以多于一列，不过目前我们
 * 不支持那样做。
 */
bool
_hash_convert_tuple(Relation index,
					Datum *user_values, bool *user_isnull,
					Datum *index_values, bool *index_isnull)
{
	uint32		hashkey;

	/*
	 * 我们不把空值插入 hash 索引。这没问题，因为
	 * 唯一支持的搜索操作符是 '='，且我们假定它是严格的。
	 */
	if (user_isnull[0])
		return false;

	hashkey = _hash_datum2hashkey(index, user_values[0]);
	index_values[0] = UInt32GetDatum(hashkey);
	index_isnull[0] = false;
	return true;
}

/*
 * _hash_binsearch - 返回页中应查找或插入
 *					 指定哈希值的偏移号。
 *
 * 我们使用二分搜索，依据现有项按哈希键
 * 有序这一假定。
 *
 * 返回第一个满足 hashkey >= hash_value 的索引项偏移，
 * 若 hash_value 大于页中全部现有哈希键，则返回该页
 * 最大偏移加一。这是开始搜索或插入新项的
 * 合适位置。
 */
OffsetNumber
_hash_binsearch(Page page, uint32 hash_value)
{
	OffsetNumber upper;
	OffsetNumber lower;

	/* 循环不变量：lower <= 目标位置 <= upper */
	upper = PageGetMaxOffsetNumber(page) + 1;
	lower = FirstOffsetNumber;

	while (upper > lower)
	{
		OffsetNumber off;
		IndexTuple	itup;
		uint32		hashkey;

		off = (upper + lower) / 2;
		Assert(OffsetNumberIsValid(off));

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		hashkey = _hash_get_indextuple_hashkey(itup);
		if (hashkey < hash_value)
			lower = off + 1;
		else
			upper = off;
	}

	return lower;
}

/*
 * _hash_binsearch_last
 *
 * 与上面相同，只是若页中有多个匹配项，
 * 我们返回最后一项的偏移，而不是第一项，
 * 且输出的可能范围是 0..maxoffset，不是 1..maxoffset+1。
 * 这便于在反向扫描中开始新的一页。
 */
OffsetNumber
_hash_binsearch_last(Page page, uint32 hash_value)
{
	OffsetNumber upper;
	OffsetNumber lower;

	/* 循环不变量：lower <= 目标位置 <= upper */
	upper = PageGetMaxOffsetNumber(page);
	lower = FirstOffsetNumber - 1;

	while (upper > lower)
	{
		IndexTuple	itup;
		OffsetNumber off;
		uint32		hashkey;

		off = (upper + lower + 1) / 2;
		Assert(OffsetNumberIsValid(off));

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		hashkey = _hash_get_indextuple_hashkey(itup);
		if (hashkey > hash_value)
			upper = off - 1;
		else
			lower = off;
	}

	return lower;
}

/*
 *	_hash_get_oldblock_from_newbucket() -- 取得正在分裂出
 *			当前（新）桶的那个桶的块号。
 */
BlockNumber
_hash_get_oldblock_from_newbucket(Relation rel, Bucket new_bucket)
{
	Bucket		old_bucket;
	uint32		mask;
	Buffer		metabuf;
	HashMetaPage metap;
	BlockNumber blkno;

	/*
	 * 要从当前桶得到旧桶，需要一个掩码，以便对表的
	 * 下半部分取模。该掩码存在元页中，名为
	 * hashm_lowmask，但这里不能依赖它，因为我们需要的是
	 * 桶分裂开始时生效的那个 lowmask 值。把新桶的
	 * 最高有效位掩掉，得到的就是
	 * 旧桶。
	 */
	mask = (((uint32) 1) << pg_leftmost_one_pos32(new_bucket)) - 1;
	old_bucket = new_bucket & mask;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	blkno = BUCKET_TO_BLKNO(metap, old_bucket);

	_hash_relbuf(rel, metabuf);

	return blkno;
}

/*
 *	_hash_get_newblock_from_oldbucket() -- 取得旧桶分裂后
 *			将生成的那个桶的块号。
 *
 * 用于按当前表半区，从旧桶找出新桶。
 * 主要用来完成未完成的分裂：此时可以确定，
 * 从该旧桶出发、正在进行的分裂
 * 不会多于一个桶。
 */
BlockNumber
_hash_get_newblock_from_oldbucket(Relation rel, Bucket old_bucket)
{
	Bucket		new_bucket;
	Buffer		metabuf;
	HashMetaPage metap;
	BlockNumber blkno;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	new_bucket = _hash_get_newbucket_from_oldbucket(rel, old_bucket,
													metap->hashm_lowmask,
													metap->hashm_maxbucket);
	blkno = BUCKET_TO_BLKNO(metap, new_bucket);

	_hash_relbuf(rel, metabuf);

	return blkno;
}

/*
 *	_hash_get_newbucket_from_oldbucket() -- 取得从当前（旧）桶
 *			分裂后将生成的新桶。
 *
 * 用于从旧桶找出新桶。把旧桶与当前表半区的
 * 最高有效位做 OR，即可得到新桶（传入本函数的
 * lowmask 可用来识别当前表半区的 msb）。从当前桶
 * 分裂出去的桶可能有多个。我们需要第一个
 * 实际存在的这种桶。
 * 调用者必须保证，从旧桶出发发生的分裂不超过
 * 一次。
 */
Bucket
_hash_get_newbucket_from_oldbucket(Relation rel, Bucket old_bucket,
								   uint32 lowmask, uint32 maxbucket)
{
	Bucket		new_bucket;

	new_bucket = CALC_NEW_BUCKET(old_bucket, lowmask);
	if (new_bucket > maxbucket)
	{
		lowmask = lowmask >> 1;
		new_bucket = CALC_NEW_BUCKET(old_bucket, lowmask);
	}

	return new_bucket;
}

/*
 * _hash_kill_items - 把索引扫描调用者告知已被杀死的项设为
 * LP_DEAD 状态。
 *
 * 经 so 局部引用的 scan->opaque 含有当前页
 * 以及其上被杀死元组的信息（一般仅当
 * so->numKilled > 0 时才应调用）。
 *
 * 调用者没有锁住该页，也可能没有在缓冲区中
 * 钉住该页。注意，设置
 * LP_DEAD 状态（它只是提示）有读锁就够了。
 *
 * 调用者必须钉住桶缓冲区，但溢出缓冲区上的钉住
 * 可有可无，如 HashScanPosIsPinned(so->currPos) 所示。
 *
 * 我们先按堆 TID 匹配这些项，再认定它们就是
 * 要删除的那些。
 *
 * VACUUM 开始时，桶里绝不会有正在进行的扫描，
 * 因为 VACUUM 会在主桶页上取得清理锁，而扫描
 * 持有钉住。扫描可以在 VACUUM 离开主桶页之后、
 * 但在它做完整个桶之前开始，不过绝不能超过 VACUUM，
 * 因为 VACUUM 总是先锁住下一页，再释放
 * 上一页上的锁。因此不必担心意外杀死
 * 一个已被无关元组复用的 TID。
 */
void
_hash_kill_items(IndexScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	int			numKilled = so->numKilled;
	int			i;
	bool		killedsomething = false;
	bool		havePin = false;

	Assert(so->numKilled > 0);
	Assert(so->killedItems != NULL);
	Assert(HashScanPosIsValid(so->currPos));

	/*
	 * 始终重置扫描状态，以免在其他页上
	 * 再找同样的项。
	 */
	so->numKilled = 0;

	blkno = so->currPos.currPage;
	if (HashScanPosIsPinned(so->currPos))
	{
		/*
		 * 我们已经钉住了这个缓冲区，所以只需
		 * 再取得它上面的锁。
		 */
		havePin = true;
		buf = so->currPos.buf;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	else
		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);

	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);

	for (i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		HashScanPosItem *currItem = &so->currPos.items[itemIndex];

		offnum = currItem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);

			if (ItemPointerEquals(&ituple->t_tid, &currItem->heapTid))
			{
				/* 找到了该项 */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* 离开内层搜索循环 */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * 因为以后需要时还可以重做，所以标为脏提示。每当
	 * 我们把任何项标为 LP_DEAD，也设置该页的
	 * LH_PAGE_HAS_DEAD_TUPLES 标志，它同样只是提示。
	 */
	if (killedsomething)
	{
		opaque->hasho_flag |= LH_PAGE_HAS_DEAD_TUPLES;
		MarkBufferDirtyHint(buf, true);
	}

	if (so->hashso_bucket_buf == so->currPos.buf ||
		havePin)
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, buf);
}
