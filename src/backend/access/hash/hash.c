/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Margo Seltzer 的 Hashing 包在 Postgres 中的实现。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hash.c
 *
 * 说明（NOTES）
 *	  本文件只包含公共接口例程。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/rel.h"

/* hashbuild 及其回调的工作状态 */
typedef struct
{
	HSpool	   *spool;			/* 未使用假脱机时为 NULL */
	double		indtuples;		/* 接受进索引的元组数 */
	Relation	heapRel;		/* 堆关系描述符 */
} HashBuildState;

static void hashbuildCallback(Relation index,
							  ItemPointer tid,
							  Datum *values,
							  bool *isnull,
							  bool tupleIsAlive,
							  void *state);


/*
 * Hash 处理函数：返回带有访问方法参数
 * 和回调的 IndexAmRoutine。
 */
Datum
hashhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = HTMaxStrategyNumber;
	amroutine->amsupport = HASHNProcs;
	amroutine->amoptsprocnum = HASHOPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype = INT4OID;

	amroutine->ambuild = hashbuild;
	amroutine->ambuildempty = hashbuildempty;
	amroutine->aminsert = hashinsert;
	amroutine->ambulkdelete = hashbulkdelete;
	amroutine->amvacuumcleanup = hashvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = hashcostestimate;
	amroutine->amoptions = hashoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = hashvalidate;
	amroutine->amadjustmembers = hashadjustmembers;
	amroutine->ambeginscan = hashbeginscan;
	amroutine->amrescan = hashrescan;
	amroutine->amgettuple = hashgettuple;
	amroutine->amgetbitmap = hashgetbitmap;
	amroutine->amendscan = hashendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 *	hashbuild() -- 构建一个新的 hash 索引。
 */
IndexBuildResult *
hashbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BlockNumber relpages;
	double		reltuples;
	double		allvisfrac;
	uint32		num_buckets;
	long		sort_threshold;
	HashBuildState buildstate;

	/*
	 * 我们预期对任何一个索引关系都恰好被调用一次。若不是
	 * 这样，那可就出大问题了。
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* 估计表中当前的行数 */
	estimate_rel_size(heap, NULL, &relpages, &reltuples, &allvisfrac);

	/* 初始化 hash 索引的元数据页和初始桶 */
	num_buckets = _hash_init(index, reltuples, MAIN_FORKNUM);

	/*
	 * 若只按扫描顺序把元组插入索引，那么
	 * （假定它们的哈希码相当随机）对索引就没有访问
	 * 局部性；而且若索引大于可用 RAM，
	 * 就会严重颠簸。为避免这种情况，可以按
	 * （预期）桶号给元组排序。不过当索引能放进 RAM 时，
	 * 这种排序只是无用的开销。若初始索引大小超过
	 * maintenance_work_mem 与该索引可用
	 * 缓冲区个数二者中的较小者，我们就选择排序。（按
	 * 缓冲区个数来限制，应能减轻 PG 缓冲区与内核
	 * 缓冲区之间的颠簸；即便不会产生物理 I/O，这似乎也有用。按
	 * maintenance_work_mem 来限制，便于测试排序
	 * 代码路径，对 DBAs 来说也可作为额外的控制旋钮。）
	 *
	 * 注意：若一个桶将来不再刚好是
	 * 一页，这项测试就得调整。另外，"初始索引大小"的统计不包括
	 * 元页，也不包括第一张位图页。
	 */
	sort_threshold = (maintenance_work_mem * 1024L) / BLCKSZ;
	if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		sort_threshold = Min(sort_threshold, NBuffers);
	else
		sort_threshold = Min(sort_threshold, NLocBuffer);

	if (num_buckets >= (uint32) sort_threshold)
		buildstate.spool = _h_spoolinit(heap, index, num_buckets);
	else
		buildstate.spool = NULL;

	/* 准备构建索引 */
	buildstate.indtuples = 0;
	buildstate.heapRel = heap;

	/* 做堆扫描 */
	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   hashbuildCallback,
									   (void *) &buildstate, NULL);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL,
								 buildstate.indtuples);

	if (buildstate.spool)
	{
		/* 给元组排序并插入索引 */
		_h_indexbuild(buildstate.spool, buildstate.heapRel);
		_h_spooldestroy(buildstate.spool);
	}

	/*
	 * 返回统计信息
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 *	hashbuildempty() -- 在初始化分叉中构建一个空的 hash 索引
 */
void
hashbuildempty(Relation index)
{
	_hash_init(index, 0, INIT_FORKNUM);
}

/*
 * table_index_build_scan 的逐元组回调
 */
static void
hashbuildCallback(Relation index,
				  ItemPointer tid,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	HashBuildState *buildstate = (HashBuildState *) state;
	Datum		index_values[1];
	bool		index_isnull[1];
	IndexTuple	itup;

	/* 把数据转换为哈希键；失败则什么都不插入 */
	if (!_hash_convert_tuple(index,
							 values, isnull,
							 index_values, index_isnull))
		return;

	/* 要么把元组假脱机以便排序，要么直接放入索引 */
	if (buildstate->spool)
		_h_spool(buildstate->spool, tid, index_values, index_isnull);
	else
	{
		/* 形成一个索引元组，并让它指向堆元组 */
		itup = index_form_tuple(RelationGetDescr(index),
								index_values, index_isnull);
		itup->t_tid = *tid;
		_hash_doinsert(index, itup, buildstate->heapRel, false);
		pfree(itup);
	}

	buildstate->indtuples += 1;
}

/*
 *	hashinsert() -- 把一个索引元组插入 hash 表。
 *
 *	对堆元组的键做哈希，形成一个带哈希码的索引元组。
 *	为新元组找到合适的位置，并放进去。
 */
bool
hashinsert(Relation rel, Datum *values, bool *isnull,
		   ItemPointer ht_ctid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   IndexInfo *indexInfo)
{
	Datum		index_values[1];
	bool		index_isnull[1];
	IndexTuple	itup;

	/* 把数据转换为哈希键；失败则什么都不插入 */
	if (!_hash_convert_tuple(rel,
							 values, isnull,
							 index_values, index_isnull))
		return false;

	/* 形成一个索引元组，并让它指向堆元组 */
	itup = index_form_tuple(RelationGetDescr(rel), index_values, index_isnull);
	itup->t_tid = *ht_ctid;

	_hash_doinsert(rel, itup, heapRel, false);

	pfree(itup);

	return false;
}


/*
 *	hashgettuple() -- 取得扫描中的下一个元组。
 */
bool
hashgettuple(IndexScanDesc scan, ScanDirection dir)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		res;

	/* hash 索引总是有损的，因为我们只存储哈希码 */
	scan->xs_recheck = true;

	/*
	 * 若这次扫描已经初始化，只需按
	 * 相应方向推进它。若还没有，就调用一个例程来
	 * 取得扫描中的第一项。
	 */
	if (!HashScanPosIsValid(so->currPos))
		res = _hash_first(scan, dir);
	else
	{
		/*
		 * 检查是否应该杀死先前取出的元组。
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * 是的，那就记住它，稍后再处理。（我们会在离开索引页之后
			 * 或扫描结束时，一次性处理所有这些元组。）
			 * 若调用者反转了索引扫描方向，完全
			 * 可能同一项会被记入多次。
			 * 但我们不检测这种情况；多余的
			 * 条目直接丢掉。
			 */
			if (so->killedItems == NULL)
				so->killedItems = (int *)
					palloc(MaxIndexTuplesPerPage * sizeof(int));

			if (so->numKilled < MaxIndexTuplesPerPage)
				so->killedItems[so->numKilled++] = so->currPos.itemIndex;
		}

		/*
		 * 现在继续扫描。
		 */
		res = _hash_next(scan, dir);
	}

	return res;
}


/*
 *	hashgetbitmap() -- 一次取得全部元组
 */
int64
hashgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		res;
	int64		ntids = 0;
	HashScanPosItem *currItem;

	res = _hash_first(scan, ForwardScanDirection);

	while (res)
	{
		currItem = &so->currPos.items[so->currPos.itemIndex];

		/*
		 * 每当 scan->ignore_killed_tuples 为 true 时，_hash_first 和 _hash_next
		 * 会负责消掉死索引项。因此这里
		 * 除了把结果加入 TIDBitmap 之外无事可做。
		 */
		tbm_add_tuples(tbm, &(currItem->heapTid), 1, true);
		ntids++;

		res = _hash_next(scan, ForwardScanDirection);
	}

	return ntids;
}


/*
 *	hashbeginscan() -- 在 hash 索引上开始一次扫描
 */
IndexScanDesc
hashbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	HashScanOpaque so;

	/* 不允许 order by 操作符 */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData));
	HashScanPosInvalidate(so->currPos);
	so->hashso_bucket_buf = InvalidBuffer;
	so->hashso_split_bucket_buf = InvalidBuffer;

	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;

	so->killedItems = NULL;
	so->numKilled = 0;

	scan->opaque = so;

	return scan;
}

/*
 *	hashrescan() -- 重新扫描一个索引关系
 */
void
hashrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	if (HashScanPosIsValid(so->currPos))
	{
		/* 离开当前页之前，先处理被杀死的项 */
		if (so->numKilled > 0)
			_hash_kill_items(scan);
	}

	_hash_dropscanbuf(rel, so);

	/* 把位置设为无效（这会导致调用 _hash_first） */
	HashScanPosInvalidate(so->currPos);

	/* 若给出了新的扫描键，则更新扫描键 */
	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;
}

/*
 *	hashendscan() -- 结束一次扫描
 */
void
hashendscan(IndexScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	if (HashScanPosIsValid(so->currPos))
	{
		/* 离开当前页之前，先处理被杀死的项 */
		if (so->numKilled > 0)
			_hash_kill_items(scan);
	}

	_hash_dropscanbuf(rel, so);

	if (so->killedItems != NULL)
		pfree(so->killedItems);
	pfree(so);
	scan->opaque = NULL;
}

/*
 * 批量删除指向一组堆元组的全部索引项。
 * 目标元组集合由回调例程指定，它告知
 * 任一给定堆元组（由 ItemPointer 标识）是否正在被删除。
 *
 * 本函数也删除被分裂移到其他
 * 桶的那些元组。
 *
 * 结果：一个由 palloc 分配的结构，含有供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
hashbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	double		tuples_removed;
	double		num_index_tuples;
	double		orig_ntuples;
	Bucket		orig_maxbucket;
	Bucket		cur_maxbucket;
	Bucket		cur_bucket;
	Buffer		metabuf = InvalidBuffer;
	HashMetaPage metap;
	HashMetaPage cachedmetap;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * 我们需要元页的一份副本，以便用它的 hashm_spares[]
	 * 值计算桶页地址，不过一份缓存副本就
	 * 够了。（若不够，我们会在后面发现并按需要刷新
	 * 缓存。）
	 */
	cachedmetap = _hash_getcachedmetap(rel, &metabuf, false);
	Assert(cachedmetap != NULL);

	orig_maxbucket = cachedmetap->hashm_maxbucket;
	orig_ntuples = cachedmetap->hashm_ntuples;

	/* 扫描我们知道存在的那些桶 */
	cur_bucket = 0;
	cur_maxbucket = orig_maxbucket;

loop_top:
	while (cur_bucket <= cur_maxbucket)
	{
		BlockNumber bucket_blkno;
		BlockNumber blkno;
		Buffer		bucket_buf;
		Buffer		buf;
		HashPageOpaque bucket_opaque;
		Page		page;
		bool		split_cleanup = false;

		/* 取得桶起始页的地址 */
		bucket_blkno = BUCKET_TO_BLKNO(cachedmetap, cur_bucket);

		blkno = bucket_blkno;

		/*
		 * 删除死元组之前，需要在主桶页上取得清理锁，以便
		 * 等过并发扫描。
		 */
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
		LockBufferForCleanup(buf);
		_hash_checkpage(rel, buf, LH_BUCKET_PAGE);

		page = BufferGetPage(buf);
		bucket_opaque = HashPageGetOpaque(page);

		/*
		 * 若桶中含有被分裂移来的元组，就需要
		 * 删除这些元组。若该桶上的分裂
		 * 操作尚未完成，就不能删除，因为扫描还需要它们。
		 */
		if (!H_BUCKET_BEING_SPLIT(bucket_opaque) &&
			H_NEEDS_SPLIT_CLEANUP(bucket_opaque))
		{
			split_cleanup = true;

			/*
			 * 自我们上次持有元页上的锁以来，这个桶可能已经分裂。
			 * 若是这样，hashm_maxbucket、hashm_highmask 和
			 * hashm_lowmask 可能已经旧到让我们无法移除
			 * 最近一次分裂留下的元组。为防止这一点，
			 * 既然目标桶的主桶页已经上锁
			 * （因此不能再继续分裂），就检查是否需要
			 * 更新我们缓存的元页数据。
			 */
			Assert(bucket_opaque->hasho_prevblkno != InvalidBlockNumber);
			if (bucket_opaque->hasho_prevblkno > cachedmetap->hashm_maxbucket)
			{
				cachedmetap = _hash_getcachedmetap(rel, &metabuf, true);
				Assert(cachedmetap != NULL);
			}
		}

		bucket_buf = buf;

		hashbucketcleanup(rel, cur_bucket, bucket_buf, blkno, info->strategy,
						  cachedmetap->hashm_maxbucket,
						  cachedmetap->hashm_highmask,
						  cachedmetap->hashm_lowmask, &tuples_removed,
						  &num_index_tuples, split_cleanup,
						  callback, callback_state);

		_hash_dropbuf(rel, bucket_buf);

		/* 前进到下一个桶 */
		cur_bucket++;
	}

	if (BufferIsInvalid(metabuf))
		metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);

	/* 对元页加写锁，并检查自我们开始以来是否发生了分裂 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	if (cur_maxbucket != metap->hashm_maxbucket)
	{
		/* 已经发生分裂，因此处理额外的桶（bucket） */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		cachedmetap = _hash_getcachedmetap(rel, &metabuf, true);
		Assert(cachedmetap != NULL);
		cur_maxbucket = cachedmetap->hashm_maxbucket;
		goto loop_top;
	}

	/* 好，我们真的做完了。更新元页中的元组计数。 */
	START_CRIT_SECTION();

	if (orig_maxbucket == metap->hashm_maxbucket &&
		orig_ntuples == metap->hashm_ntuples)
	{
		/*
		 * 自扫描开始以来没有人分裂或插入过任何东西，因此
		 * 把我们的计数当作准数。
		 */
		metap->hashm_ntuples = num_index_tuples;
	}
	else
	{
		/*
		 * 否则我们的计数不可信，因为我们可能
		 * 对分裂桶中的元组做了重复扫描。改为按差值推算。
		 * （注意：我们仍返回 estimated_count = false，因为用这个
		 * 计数总比完全不更新 reltuples 要好。）
		 */
		if (metap->hashm_ntuples > tuples_removed)
			metap->hashm_ntuples -= tuples_removed;
		else
			metap->hashm_ntuples = 0;
		num_index_tuples = metap->hashm_ntuples;
	}

	MarkBufferDirty(metabuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_update_meta_page xlrec;
		XLogRecPtr	recptr;

		xlrec.ntuples = metap->hashm_ntuples;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHashUpdateMetaPage);

		XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_UPDATE_META_PAGE);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	_hash_relbuf(rel, metabuf);

	/* 返回统计信息 */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->estimated_count = false;
	stats->num_index_tuples = num_index_tuples;
	stats->tuples_removed += tuples_removed;
	/* hashvacuumcleanup 会填上 num_pages */

	return stats;
}

/*
 * VACUUM 之后的清理。
 *
 * 结果：一个由 palloc 分配的结构，含有供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
hashvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;
	BlockNumber num_pages;

	/* 若未调用 hashbulkdelete，返回 NULL 表示没有变化 */
	/* 注意：这也覆盖 analyze_only 的情形 */
	if (stats == NULL)
		return NULL;

	/* 更新统计信息 */
	num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_pages = num_pages;

	return stats;
}

/*
 * 从桶中删除索引项的辅助函数。
 *
 * 本函数预期调用者已在主桶页上取得清理锁，
 * 并且返回时会再次在主桶页上持有写锁。
 * 不过该锁未必一直持有，
 * 因为访问溢出页时会把它释放。
 *
 * 刚进入本函数时不可能有并发扫描正在进行，
 * 这是因为我们在主桶页上持有清理锁；
 * 但一旦释放该锁，就可能出现。若那些扫描
 * 跑到我们的清理扫描前面，它们可能在我们杀死元组之前看见它，
 * 并且直到 VACUUM 完成、该 TID 已被无关元组复用之后才醒来。
 * 为避免这种灾难，我们阻止扫描超过
 * 我们的清理扫描：在释放桶链中上一页的锁之前，
 * 先锁住下一页。（这种锁链并不
 * 理想，以后也许该找个更好的办法。）
 *
 * 必须在主桶上保持钉住，以确保不会有并发
 * 分裂开始。
 */
void
hashbucketcleanup(Relation rel, Bucket cur_bucket, Buffer bucket_buf,
				  BlockNumber bucket_blkno, BufferAccessStrategy bstrategy,
				  uint32 maxbucket, uint32 highmask, uint32 lowmask,
				  double *tuples_removed, double *num_index_tuples,
				  bool split_cleanup,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	BlockNumber blkno;
	Buffer		buf;
	Bucket		new_bucket PG_USED_FOR_ASSERTS_ONLY = InvalidBucket;
	bool		bucket_dirty = false;

	blkno = bucket_blkno;
	buf = bucket_buf;

	if (split_cleanup)
		new_bucket = _hash_get_newbucket_from_oldbucket(rel, cur_bucket,
														lowmask, maxbucket);

	/* 扫描桶中的每一页 */
	for (;;)
	{
		HashPageOpaque opaque;
		OffsetNumber offno;
		OffsetNumber maxoffno;
		Buffer		next_buf;
		Page		page;
		OffsetNumber deletable[MaxOffsetNumber];
		int			ndeletable = 0;
		bool		retain_pin = false;
		bool		clear_dead_marking = false;

		vacuum_delay_point();

		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);

		/* 扫描页中的每个元组 */
		maxoffno = PageGetMaxOffsetNumber(page);
		for (offno = FirstOffsetNumber;
			 offno <= maxoffno;
			 offno = OffsetNumberNext(offno))
		{
			ItemPointer htup;
			IndexTuple	itup;
			Bucket		bucket;
			bool		kill_tuple = false;

			itup = (IndexTuple) PageGetItem(page,
											PageGetItemId(page, offno));
			htup = &(itup->t_tid);

			/*
			 * 要移除死元组，我们严格依据回调函数的
			 * 结果。详细原因见 btvacuumpage。
			 */
			if (callback && callback(htup, callback_state))
			{
				kill_tuple = true;
				if (tuples_removed)
					*tuples_removed += 1;
			}
			else if (split_cleanup)
			{
				/* 删除被分裂移走的元组。 */
				bucket = _hash_hashkey2bucket(_hash_get_indextuple_hashkey(itup),
											  maxbucket,
											  highmask,
											  lowmask);
				/* 把该项标为待删除 */
				if (bucket != cur_bucket)
				{
					/*
					 * 我们预期元组要么属于当前桶，要么属于
					 * new_bucket。这是有保证的，因为我们不允许
					 * 从含有垃圾的桶再做进一步分裂。见
					 * _hash_expandtable 中的注释。
					 */
					Assert(bucket == new_bucket);
					kill_tuple = true;
				}
			}

			if (kill_tuple)
			{
				/* 把该项标为待删除 */
				deletable[ndeletable++] = offno;
			}
			else
			{
				/* 我们保留它，因此计入 */
				if (num_index_tuples)
					*num_index_tuples += 1;
			}
		}

		/* 直到桶扫描结束都保持主桶页上的钉住 */
		if (blkno == bucket_blkno)
			retain_pin = true;
		else
			retain_pin = false;

		blkno = opaque->hasho_nextblkno;

		/*
		 * 实施删除，前进到下一页，并在需要时写页。
		 */
		if (ndeletable > 0)
		{
			/* 在改动记入日志之前不得 ereport(ERROR) */
			START_CRIT_SECTION();

			PageIndexMultiDelete(page, deletable, ndeletable);
			bucket_dirty = true;

			/*
			 * 若 VACUUM 从索引页移除了 DEAD 元组，
			 * 就把该页标为干净。做法是清除
			 * LH_PAGE_HAS_DEAD_TUPLES 标志。
			 */
			if (tuples_removed && *tuples_removed > 0 &&
				H_HAS_DEAD_TUPLES(opaque))
			{
				opaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
				clear_dead_marking = true;
			}

			MarkBufferDirty(buf);

			/* XLOG 相关 */
			if (RelationNeedsWAL(rel))
			{
				xl_hash_delete xlrec;
				XLogRecPtr	recptr;

				xlrec.clear_dead_marking = clear_dead_marking;
				xlrec.is_primary_bucket_page = (buf == bucket_buf);

				XLogBeginInsert();
				XLogRegisterData((char *) &xlrec, SizeOfHashDelete);

				/*
				 * 桶缓冲区需要登记，以确保重放期间
				 * 我们能对它取得清理锁。
				 */
				if (!xlrec.is_primary_bucket_page)
					XLogRegisterBuffer(0, bucket_buf, REGBUF_STANDARD | REGBUF_NO_IMAGE);

				XLogRegisterBuffer(1, buf, REGBUF_STANDARD);
				XLogRegisterBufData(1, (char *) deletable,
									ndeletable * sizeof(OffsetNumber));

				recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_DELETE);
				PageSetLSN(BufferGetPage(buf), recptr);
			}

			END_CRIT_SECTION();
		}

		/* 若没有更多页要扫描，就退出。 */
		if (!BlockNumberIsValid(blkno))
			break;

		next_buf = _hash_getbuf_with_strategy(rel, blkno, HASH_WRITE,
											  LH_OVERFLOW_PAGE,
											  bstrategy);

		/*
		 * 取得下一页上的锁之后，释放上一页上的
		 * 锁
		 */
		if (retain_pin)
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, buf);

		buf = next_buf;
	}

	/*
	 * 锁住桶页，以清除垃圾标志并挤压该桶。
	 * 若当前缓冲区就是桶缓冲区，那么我们已经
	 * 持有桶页上的锁。
	 */
	if (buf != bucket_buf)
	{
		_hash_relbuf(rel, buf);
		LockBuffer(bucket_buf, BUFFER_LOCK_EXCLUSIVE);
	}

	/*
	 * 删除被分裂移走的元组之后，清除桶上的垃圾标志。
	 * 我们有意在挤压桶之前清除该标志，
	 * 这样重启之后，VACUUM 不会再次试图删除被分裂
	 * 移走的元组。
	 */
	if (split_cleanup)
	{
		HashPageOpaque bucket_opaque;
		Page		page;

		page = BufferGetPage(bucket_buf);
		bucket_opaque = HashPageGetOpaque(page);

		/* 在改动记入日志之前不得 ereport(ERROR) */
		START_CRIT_SECTION();

		bucket_opaque->hasho_flag &= ~LH_BUCKET_NEEDS_SPLIT_CLEANUP;
		MarkBufferDirty(bucket_buf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, bucket_buf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_CLEANUP);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}

	/*
	 * 若删除了任何内容，就尝试压缩空闲空间。挤压
	 * 该桶时必须持有清理锁，否则可能影响
	 * 在此之前已经开始的扫描中元组的顺序。
	 */
	if (bucket_dirty && IsBufferCleanupOK(bucket_buf))
		_hash_squeezebucket(rel, cur_bucket, bucket_blkno, bucket_buf,
							bstrategy);
	else
		LockBuffer(bucket_buf, BUFFER_LOCK_UNLOCK);
}
