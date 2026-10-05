/*
 * brin.c
 *		PostgreSQL 的 BRIN 索引实现
 *
 * 详见 src/backend/access/brin/README。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin.c
 *
 * TODO
 *		* 支持 ScalarArrayOpExpr (amsearcharray -> SK_SEARCHARRAY)
 */
#include "postgres.h"

#include "access/brin.h"
#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_xlog.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"
#include "utils/rel.h"


/*
 * 构建 BRIN 索引的初始阶段使用 BrinBuildState。
 * 运行中的状态保存在 BrinMemTuple 里。
 */
typedef struct BrinBuildState
{
	Relation	bs_irel;
	int			bs_numtuples;
	Buffer		bs_currentInsertBuf;
	BlockNumber bs_pagesPerRange;
	BlockNumber bs_currRangeStart;
	BrinRevmap *bs_rmAccess;
	BrinDesc   *bs_bdesc;
	BrinMemTuple *bs_dtuple;
} BrinBuildState;

/*
 * 索引扫描期间用作 "opaque" 的结构
 */
typedef struct BrinOpaque
{
	BlockNumber bo_pagesPerRange;
	BrinRevmap *bo_rmAccess;
	BrinDesc   *bo_bdesc;
} BrinOpaque;

#define BRIN_ALL_BLOCKRANGES	InvalidBlockNumber

static BrinBuildState *initialize_brin_buildstate(Relation idxRel,
												  BrinRevmap *revmap, BlockNumber pagesPerRange);
static void terminate_brin_buildstate(BrinBuildState *state);
static void brinsummarize(Relation index, Relation heapRel, BlockNumber pageRange,
						  bool include_partial, double *numSummarized, double *numExisting);
static void form_and_insert_tuple(BrinBuildState *state);
static void union_tuples(BrinDesc *bdesc, BrinMemTuple *a,
						 BrinTuple *b);
static void brin_vacuum_scan(Relation idxrel, BufferAccessStrategy strategy);
static bool add_values_to_range(Relation idxRel, BrinDesc *bdesc,
								BrinMemTuple *dtup, Datum *values, bool *nulls);
static bool check_null_keys(BrinValues *bval, ScanKey *nullkeys, int nnullkeys);

/*
 * BRIN 处理函数：返回带有访问方法参数的 IndexAmRoutine
 * 以及各个回调。
 */
Datum
brinhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = BRIN_LAST_OPTIONAL_PROCNUM;
	amroutine->amoptsprocnum = BRIN_PROCNUM_OPTIONS;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = true;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = brinbuild;
	amroutine->ambuildempty = brinbuildempty;
	amroutine->aminsert = brininsert;
	amroutine->ambulkdelete = brinbulkdelete;
	amroutine->amvacuumcleanup = brinvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = brincostestimate;
	amroutine->amoptions = brinoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = brinvalidate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = brinbeginscan;
	amroutine->amrescan = brinrescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = bringetbitmap;
	amroutine->amendscan = brinendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * 正在向堆插入一个元组。为使 BRIN 索引保持最新，
 * 需要取得相应的索引元组，并把其中保存的值
 * 与新元组的值比较。若这些值与
 * 汇总元组不一致，就需要更新索引元组。
 *
 * 若启用了自动摘要，检查是否需要对前一个
 * 页范围做摘要。
 *
 * 若该范围当前尚未摘要（即 revmap 对其返回 NULL），
 * 则对此元组无事可做。
 */
bool
brininsert(Relation idxRel, Datum *values, bool *nulls,
		   ItemPointer heaptid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   IndexInfo *indexInfo)
{
	BlockNumber pagesPerRange;
	BlockNumber origHeapBlk;
	BlockNumber heapBlk;
	BrinDesc   *bdesc = (BrinDesc *) indexInfo->ii_AmCache;
	BrinRevmap *revmap;
	Buffer		buf = InvalidBuffer;
	MemoryContext tupcxt = NULL;
	MemoryContext oldcxt = CurrentMemoryContext;
	bool		autosummarize = BrinGetAutoSummarize(idxRel);

	revmap = brinRevmapInitialize(idxRel, &pagesPerRange, NULL);

	/*
	 * origHeapBlk 是发生插入的块号。heapBlk
	 * 是对应页范围的第一个块。
	 */
	origHeapBlk = ItemPointerGetBlockNumber(heaptid);
	heapBlk = (origHeapBlk / pagesPerRange) * pagesPerRange;

	for (;;)
	{
		bool		need_insert = false;
		OffsetNumber off;
		BrinTuple  *brtup;
		BrinMemTuple *dtup;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 若启用了自动摘要，且刚刚把第一个
		 * 元组插入某个新的、非首个页范围的第一个块，则请求对
		 * 前一个范围做一次摘要。
		 */
		if (autosummarize &&
			heapBlk > 0 &&
			heapBlk == origHeapBlk &&
			ItemPointerGetOffsetNumber(heaptid) == FirstOffsetNumber)
		{
			BlockNumber lastPageRange = heapBlk - 1;
			BrinTuple  *lastPageTuple;

			lastPageTuple =
				brinGetTupleForHeapBlock(revmap, lastPageRange, &buf, &off,
										 NULL, BUFFER_LOCK_SHARE, NULL);
			if (!lastPageTuple)
			{
				bool		recorded;

				recorded = AutoVacuumRequestWork(AVW_BRINSummarizeRange,
												 RelationGetRelid(idxRel),
												 lastPageRange);
				if (!recorded)
					ereport(LOG,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("request for BRIN range summarization for index \"%s\" page %u was not recorded",
									RelationGetRelationName(idxRel),
									lastPageRange)));
			}
			else
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}

		brtup = brinGetTupleForHeapBlock(revmap, heapBlk, &buf, &off,
										 NULL, BUFFER_LOCK_SHARE, NULL);

		/* 若范围未摘要，则无事可做 */
		if (!brtup)
			break;

		/* 本语句中第一次执行？ */
		if (bdesc == NULL)
		{
			MemoryContextSwitchTo(indexInfo->ii_Context);
			bdesc = brin_build_desc(idxRel);
			indexInfo->ii_AmCache = (void *) bdesc;
			MemoryContextSwitchTo(oldcxt);
		}
		/* 本次 brininsert 调用中第一次？ */
		if (tupcxt == NULL)
		{
			tupcxt = AllocSetContextCreate(CurrentMemoryContext,
										   "brininsert cxt",
										   ALLOCSET_DEFAULT_SIZES);
			MemoryContextSwitchTo(tupcxt);
		}

		dtup = brin_deform_tuple(bdesc, brtup, NULL);

		need_insert = add_values_to_range(idxRel, bdesc, dtup, values, nulls);

		if (!need_insert)
		{
			/*
			 * 该元组与新值一致，因此无事
			 * 可做。
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		else
		{
			Page		page = BufferGetPage(buf);
			ItemId		lp = PageGetItemId(page, off);
			Size		origsz;
			BrinTuple  *origtup;
			Size		newsz;
			BrinTuple  *newtup;
			bool		samepage;

			/*
			 * 复制旧元组，以便在
			 * 重新取得锁之后进行比较。
			 */
			origsz = ItemIdGetLength(lp);
			origtup = brin_copy_tuple(brtup, origsz, NULL, NULL);

			/*
			 * 释放锁之前，检查能否尝试同页
			 * 更新。不过另一进程可能并发地向
			 * 同一页插入元组，因此后续必须准备好应对
			 * 最终发现这并不可行的情况。
			 */
			newtup = brin_form_tuple(bdesc, heapBlk, dtup, &newsz);
			samepage = brin_can_do_samepage_update(buf, origsz, newsz);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/*
			 * 尝试更新该元组。若因任何原因
			 * 没有成功，就需要从头再来；revmap 现在可能
			 * 指向此块的另一个元组，因此需要
			 * 重新计算，以确保新的堆元组与另一插入
			 * 进程的元组都被合并后的元组覆盖。也可能
			 * 根本不需要更新。
			 */
			if (!brin_doupdate(idxRel, pagesPerRange, revmap, heapBlk,
							   buf, off, origtup, origsz, newtup, newsz,
							   samepage))
			{
				/* 未成功；从头再来 */
				MemoryContextResetAndDeleteChildren(tupcxt);
				continue;
			}
		}

		/* 成功！ */
		break;
	}

	brinRevmapTerminate(revmap);
	if (BufferIsValid(buf))
		ReleaseBuffer(buf);
	MemoryContextSwitchTo(oldcxt);
	if (tupcxt != NULL)
		MemoryContextDelete(tupcxt);

	return false;
}

/*
 * 初始化 BRIN 索引扫描的状态。
 *
 * 在此读取元页，以确定构建本索引时使用的
 * 每范围页数。注意，持有索引锁期间该值无法更改，
 * 因此不必在 brinrescan 期间重新计算。
 */
IndexScanDesc
brinbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BrinOpaque *opaque;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	opaque = palloc_object(BrinOpaque);
	opaque->bo_rmAccess = brinRevmapInitialize(r, &opaque->bo_pagesPerRange,
											   scan->xs_snapshot);
	opaque->bo_bdesc = brin_build_desc(r);
	scan->opaque = opaque;

	return scan;
}

/*
 * 执行索引扫描。
 *
 * 做法是从 revmap 读取索引 TIDs，再取得它们指向的索引
 * 元组；把索引元组中的摘要值
 * 与扫描键比较。把与扫描键匹配的索引元组所对应
 * 范围内的全部页返回到 TID 位图中。
 *
 * 若从 revmap 读到的 TID 为 InvalidTID，则知道该范围
 * 尚未摘要。这些范围内的页必须返回，与扫描
 * 键无关。
 */
int64
bringetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation	idxRel = scan->indexRelation;
	Buffer		buf = InvalidBuffer;
	BrinDesc   *bdesc;
	Oid			heapOid;
	Relation	heapRel;
	BrinOpaque *opaque;
	BlockNumber nblocks;
	int64		totalpages = 0;
	FmgrInfo   *consistentFn;
	MemoryContext oldcxt;
	MemoryContext perRangeCxt;
	BrinMemTuple *dtup;
	BrinTuple  *btup = NULL;
	Size		btupsz = 0;
	ScanKey   **keys,
			  **nullkeys;
	int		   *nkeys,
			   *nnullkeys;
	char	   *ptr;
	Size		len;
	char	   *tmp PG_USED_FOR_ASSERTS_ONLY;

	opaque = (BrinOpaque *) scan->opaque;
	bdesc = opaque->bo_bdesc;
	pgstat_count_index_scan(idxRel);

	/*
	 * 需要知道表的大小，才能知道要在
	 * revmap 上迭代多久。
	 */
	heapOid = IndexGetRelation(RelationGetRelid(idxRel), false);
	heapRel = table_open(heapOid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(heapRel);
	table_close(heapRel, AccessShareLock);

	/*
	 * 为被索引列的 consistent 支持过程留出空间。此处
	 * 不查找它们；等到第一次看到引用其中每一个的扫描
	 * 键时再惰性查找。这依赖于把 fn_oid 清零为 InvalidOid。
	 */
	consistentFn = palloc0_array(FmgrInfo, bdesc->bd_tupdesc->natts);

	/*
	 * 为将传给 consistent 支持过程的、按属性分组的扫描键
	 * 列表留出空间。不知道哪些属性带有扫描
	 * 键，因此为全部属性分配空间。这可能多用一些内存，
	 * 但多半比先判定哪些属性被用到更省。
	 *
	 * 把空值键和常规键分开存放，以便能方便地只把
	 * 常规键传给 consistent 函数。
	 *
	 * 为降低分配开销，先分配一大块，再
	 * 自己把它切成较小的数组。所有这些片段的生命期完全
	 * 相同，因此这样做没问题。
	 *
	 * XXX 最宽的索引可以有 32 个属性，因此浪费的
	 * 内存可以忽略。可以设计更紧凑的做法（只为
	 * 用到的属性留空间），但那会使匹配更
	 * 复杂，并不划算。
	 */
	len =
		MAXALIGN(sizeof(ScanKey *) * bdesc->bd_tupdesc->natts) +	/* 常规键 */
		MAXALIGN(sizeof(ScanKey) * scan->numberOfKeys) * bdesc->bd_tupdesc->natts +
		MAXALIGN(sizeof(int) * bdesc->bd_tupdesc->natts) +
		MAXALIGN(sizeof(ScanKey *) * bdesc->bd_tupdesc->natts) +	/* NULL 键 */
		MAXALIGN(sizeof(ScanKey) * scan->numberOfKeys) * bdesc->bd_tupdesc->natts +
		MAXALIGN(sizeof(int) * bdesc->bd_tupdesc->natts);

	ptr = palloc(len);
	tmp = ptr;

	keys = (ScanKey **) ptr;
	ptr += MAXALIGN(sizeof(ScanKey *) * bdesc->bd_tupdesc->natts);

	nullkeys = (ScanKey **) ptr;
	ptr += MAXALIGN(sizeof(ScanKey *) * bdesc->bd_tupdesc->natts);

	nkeys = (int *) ptr;
	ptr += MAXALIGN(sizeof(int) * bdesc->bd_tupdesc->natts);

	nnullkeys = (int *) ptr;
	ptr += MAXALIGN(sizeof(int) * bdesc->bd_tupdesc->natts);

	for (int i = 0; i < bdesc->bd_tupdesc->natts; i++)
	{
		keys[i] = (ScanKey *) ptr;
		ptr += MAXALIGN(sizeof(ScanKey) * scan->numberOfKeys);

		nullkeys[i] = (ScanKey *) ptr;
		ptr += MAXALIGN(sizeof(ScanKey) * scan->numberOfKeys);
	}

	Assert(tmp + len == ptr);

	/* 把键的数量清零 */
	memset(nkeys, 0, sizeof(int) * bdesc->bd_tupdesc->natts);
	memset(nnullkeys, 0, sizeof(int) * bdesc->bd_tupdesc->natts);

	/* 预处理扫描键，拆成按属性分组的数组。 */
	for (int keyno = 0; keyno < scan->numberOfKeys; keyno++)
	{
		ScanKey		key = &scan->keyData[keyno];
		AttrNumber	keyattno = key->sk_attno;

		/*
		 * 扫描键的排序规则必须与索引
		 * 列所用的排序规则一致（但仅当搜索不是 IS NULL/ IS NOT NULL 时）。
		 * 否则就不该使用这个索引……
		 */
		Assert((key->sk_flags & SK_ISNULL) ||
			   (key->sk_collation ==
				TupleDescAttr(bdesc->bd_tupdesc,
							  keyattno - 1)->attcollation));

		/*
		 * 第一次看到这个索引属性，按需初始化。
		 *
		 * 这有些过度——不知道该属性有多少扫描键，
		 * 因此简单地按可能的最大数量分配
		 * （就好像全部键都属于该属性）。这可能浪费一点
		 * 内存，但一般只预期少量扫描键，
		 * 因此这点浪费可以忽略，而反复调用 repalloc
		 * 也并非没有代价。
		 */
		if (consistentFn[keyattno - 1].fn_oid == InvalidOid)
		{
			FmgrInfo   *tmp;

			/* 第一次看到该属性，因此还没有键或空值键。 */
			Assert(nkeys[keyattno - 1] == 0);
			Assert(nnullkeys[keyattno - 1] == 0);

			tmp = index_getprocinfo(idxRel, keyattno,
									BRIN_PROCNUM_CONSISTENT);
			fmgr_info_copy(&consistentFn[keyattno - 1], tmp,
						   CurrentMemoryContext);
		}

		/* 把键加入对应的按属性数组。 */
		if (key->sk_flags & SK_ISNULL)
		{
			nullkeys[keyattno - 1][nnullkeys[keyattno - 1]] = key;
			nnullkeys[keyattno - 1]++;
		}
		else
		{
			keys[keyattno - 1][nkeys[keyattno - 1]] = key;
			nkeys[keyattno - 1]++;
		}
	}

	/* 从按范围的内存上下文中分配初始内存元组 */
	dtup = brin_new_memtuple(bdesc);

	/*
	 * 建立并使用按范围的内存上下文，每次在下面
	 * 循环时重置。这样就不需要在循环内释放元组。
	 */
	perRangeCxt = AllocSetContextCreate(CurrentMemoryContext,
										"bringetbitmap cxt",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(perRangeCxt);

	/*
	 * 现在扫描 revmap。从堆页 0 开始查询，
	 * 每次增加每范围的页数；这样就能得到整张
	 * 表的完整视图。heapBlk 使用 uint64，因为 BlockNumber
	 * 在接近 2^32 页的表上可能回绕。
	 */
	for (uint64 heapBlk = 0; heapBlk < nblocks; heapBlk += opaque->bo_pagesPerRange)
	{
		bool		addrange;
		bool		gottuple = false;
		BrinTuple  *tup;
		OffsetNumber off;
		Size		size;

		CHECK_FOR_INTERRUPTS();

		MemoryContextResetAndDeleteChildren(perRangeCxt);

		tup = brinGetTupleForHeapBlock(opaque->bo_rmAccess, (BlockNumber) heapBlk, &buf,
									   &off, &size, BUFFER_LOCK_SHARE,
									   scan->xs_snapshot);
		if (tup)
		{
			gottuple = true;
			btup = brin_copy_tuple(tup, size, btup, &btupsz);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}

		/*
		 * 对于没有索引元组的页范围，必须返回整个
		 * 范围；否则，把它与扫描键比较。
		 */
		if (!gottuple)
		{
			addrange = true;
		}
		else
		{
			dtup = brin_deform_tuple(bdesc, btup, dtup);
			if (dtup->bt_placeholder)
			{
				/*
				 * 占位元组一律返回，无论其中
				 * 存放的值是什么。
				 */
				addrange = true;
			}
			else
			{
				int			attno;

				/*
				 * 把扫描键与该范围所存的摘要值比较。
				 * 若与扫描键匹配，必须把该页范围加入
				 * 位图。一开始假定需要加入该范围；
				 * 尤其可以覆盖没有任何
				 * 键的情形。
				 */
				addrange = true;
				for (attno = 1; attno <= bdesc->bd_tupdesc->natts; attno++)
				{
					BrinValues *bval;
					Datum		add;
					Oid			collation;

					/*
					 * 跳过没有任何扫描键的属性（包括常规键和
					 * IS [NOT] NULL）
					 */
					if (nkeys[attno - 1] == 0 && nnullkeys[attno - 1] == 0)
						continue;

					bval = &dtup->bt_columns[attno - 1];

					/*
					 * 若 BRIN 元组表明该范围为空，
					 * 可以跳过：没有可匹配的内容。不必
					 * 再检查后面的列。
					 */
					if (dtup->bt_empty_range)
					{
						addrange = false;
						break;
					}

					/*
					 * 先检查是否有 IS [NOT] NULL 扫描键，
					 * 以及是否违反了它们。若是，可以
					 * 提前终止，不必调用支持函数。
					 *
					 * 因为可能还有更多键，只能在
					 * 本循环内判定不匹配。
					 */
					if (bdesc->bd_info[attno - 1]->oi_regular_nulls &&
						!check_null_keys(bval, nullkeys[attno - 1],
										 nnullkeys[attno - 1]))
					{
						/*
						 * 若任一 IS [NOT] NULL 键失败，整个页
						 * 范围都无法通过。因此终止循环。
						 */
						addrange = false;
						break;
					}

					/*
					 * 因此，要么没有 IS [NOT] NULL 键，要么全部
					 * 已通过。若没有常规扫描键，则已完成——
					 * 该页范围匹配。若有常规键，但
					 * 该页范围被标为 "all nulls"，则不可能
					 * 通过（这里假定操作符是
					 * 严格的）。
					 */

					/* 没有常规扫描键，整个页范围通过。 */
					if (!nkeys[attno - 1])
						continue;

					Assert((nkeys[attno - 1] > 0) &&
						   (nkeys[attno - 1] <= scan->numberOfKeys));

					/* 若全为空值，则不可能一致。 */
					if (bval->bv_allnulls)
					{
						addrange = false;
						break;
					}

					/*
					 * 排序规则取自第一个键（同一属性的
					 * 全部键必须相同）。
					 */
					collation = keys[attno - 1][0]->sk_collation;

					/*
					 * 检查扫描键是否与该页
					 * 范围的值一致；若一致，就把该范围内的页加入
					 * 输出位图。
					 *
					 * 操作符类未必支持一次处理
					 * 多个扫描键。可以根据
					 * 参数个数来判断：带有额外参数
					 * （扫描键个数）的函数支持这样做，否则
					 * 只能逐个传入扫描键。
					 */
					if (consistentFn[attno - 1].fn_nargs >= 4)
					{
						/* 一次检查全部键 */
						add = FunctionCall4Coll(&consistentFn[attno - 1],
												collation,
												PointerGetDatum(bdesc),
												PointerGetDatum(bval),
												PointerGetDatum(keys[attno - 1]),
												Int32GetDatum(nkeys[attno - 1]));
						addrange = DatumGetBool(add);
					}
					else
					{
						/*
						 * 逐个检查键
						 *
						 * 有多个扫描键时，只要其中一个
						 * 不满足条件，就足以
						 * 把整个范围丢弃，因此一旦得到假的返回值
						 * 就跳出循环。
						 */
						int			keyno;

						for (keyno = 0; keyno < nkeys[attno - 1]; keyno++)
						{
							add = FunctionCall3Coll(&consistentFn[attno - 1],
													keys[attno - 1][keyno]->sk_collation,
													PointerGetDatum(bdesc),
													PointerGetDatum(bval),
													PointerGetDatum(keys[attno - 1][keyno]));
							addrange = DatumGetBool(add);
							if (!addrange)
								break;
						}
					}

					/*
					 * 若已有扫描键排除了该范围，就不必
					 * 再检查其余的键。
					 */
					if (!addrange)
						break;
				}
			}
		}

		/* 若需要，把该范围内的页加入输出位图 */
		if (addrange)
		{
			uint64		pageno;

			for (pageno = heapBlk;
				 pageno <= Min(nblocks, heapBlk + opaque->bo_pagesPerRange) - 1;
				 pageno++)
			{
				MemoryContextSwitchTo(oldcxt);
				tbm_add_page(tbm, pageno);
				totalpages++;
				MemoryContextSwitchTo(perRangeCxt);
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perRangeCxt);

	if (buf != InvalidBuffer)
		ReleaseBuffer(buf);

	/*
	 * XXX 扫描返回的*页*数只是近似值，
	 * 但并不确切知道涉及的堆元组
	 * 有多少。
	 */
	return totalpages * 10;
}

/*
 * 重新初始化 BRIN 索引扫描的状态
 */
void
brinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	/*
	 * 其他索引访问方法会在此处，或在扫描
	 * 早期预处理扫描键；这样它们就可以去掉冗余
	 * 键来做优化，或在无法满足时提前返回；参见
	 * _bt_preprocess_keys 的例子。将来这里也可以
	 * 加上类似处理。
	 */

	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
}

/*
 * 结束 BRIN 索引扫描
 */
void
brinendscan(IndexScanDesc scan)
{
	BrinOpaque *opaque = (BrinOpaque *) scan->opaque;

	brinRevmapTerminate(opaque->bo_rmAccess);
	brin_free_desc(opaque->bo_bdesc);
	pfree(opaque);
}

/*
 * table_index_build_scan 的按堆元组回调。
 *
 * 注意，这里不必操心表末尾的页范围；它在
 * 最后一次调用我们之后仍留在构建状态结构中，但尚未
 * 插入索引。若合适，调用者必须保证把它插入。
 */
static void
brinbuildCallback(Relation index,
				  ItemPointer tid,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *brstate)
{
	BrinBuildState *state = (BrinBuildState *) brstate;
	BlockNumber thisblock;

	thisblock = ItemPointerGetBlockNumber(tid);

	/*
	 * 若当前块属于将来的范围，先把已有内容做摘要，
	 * 再重新开始。注意扫描可能跳过许多
	 * 页（若其中没有活元组）；那些页也要确保插入索引
	 * 元组。
	 */
	while (thisblock > state->bs_currRangeStart + state->bs_pagesPerRange - 1)
	{

		BRIN_elog((DEBUG2,
				   "brinbuildCallback: completed a range: %u--%u",
				   state->bs_currRangeStart,
				   state->bs_currRangeStart + state->bs_pagesPerRange));

		/* 创建索引元组并插入 */
		form_and_insert_tuple(state);

		/* 使状态对应下一个范围 */
		state->bs_currRangeStart += state->bs_pagesPerRange;

		/* 为它重新初始化状态 */
		brin_memtuple_initialize(state->bs_dtuple, state->bs_bdesc);
	}

	/* 把当前元组累积到运行状态中 */
	(void) add_values_to_range(index, state->bs_bdesc, state->bs_dtuple,
							   values, isnull);
}

/*
 * brinbuild() -- 构建一个新的 BRIN 索引。
 */
IndexBuildResult *
brinbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	double		idxtuples;
	BrinRevmap *revmap;
	BrinBuildState *state;
	Buffer		meta;
	BlockNumber pagesPerRange;

	/*
	 * 预期对任一索引关系只调用一次。
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/*
	 * 不需要临界区，因为出错时整个关系的
	 * 创建都会回滚。
	 */

	meta = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(meta) == BRIN_METAPAGE_BLKNO);

	brin_metapage_init(BufferGetPage(meta), BrinGetPagesPerRange(index),
					   BRIN_CURRENT_VERSION);
	MarkBufferDirty(meta);

	if (RelationNeedsWAL(index))
	{
		xl_brin_createidx xlrec;
		XLogRecPtr	recptr;
		Page		page;

		xlrec.version = BRIN_CURRENT_VERSION;
		xlrec.pagesPerRange = BrinGetPagesPerRange(index);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinCreateIdx);
		XLogRegisterBuffer(0, meta, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_BRIN_ID, XLOG_BRIN_CREATE_INDEX);

		page = BufferGetPage(meta);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(meta);

	/*
	 * 初始化状态，包括变形元组的状态。
	 */
	revmap = brinRevmapInitialize(index, &pagesPerRange, NULL);
	state = initialize_brin_buildstate(index, revmap, pagesPerRange);

	/*
	 * 现在扫描该关系。这里不允许同步扫描，因为需要
	 * 堆块按物理顺序排列。
	 */
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   brinbuildCallback, (void *) state, NULL);

	/* 处理最后一批 */
	form_and_insert_tuple(state);

	/* 释放资源 */
	idxtuples = state->bs_numtuples;
	brinRevmapTerminate(state->bs_rmAccess);
	terminate_brin_buildstate(state);

	/*
	 * 返回统计信息
	 */
	result = palloc_object(IndexBuildResult);

	result->heap_tuples = reltuples;
	result->index_tuples = idxtuples;

	return result;
}

void
brinbuildempty(Relation index)
{
	Buffer		metabuf;

	/* 空的 BRIN 索引只有一个元页。 */
	metabuf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);

	/* 初始化元页缓冲区并记入 WAL。 */
	START_CRIT_SECTION();
	brin_metapage_init(BufferGetPage(metabuf), BrinGetPagesPerRange(index),
					   BRIN_CURRENT_VERSION);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuf);
}

/*
 * brinbulkdelete
 *		由于 BRIN 索引没有按堆元组一一对应的索引元组，
 *		这里能做的不多。
 *
 * XXX 可以把项元组标为 "dirty"（当最小或最大的堆
 * 元组被删除时），表示需要对受影响的范围重新做摘要。
 * 为此需要在 BRIN 元组中增加一个额外标志。
 */
IndexBulkDeleteResult *
brinbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	/* 若是第一次则分配统计结构，否则复用已有结构 */
	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	return stats;
}

/*
 * 本例程负责"清理" BRIN 索引：只对当前
 * 尚未摘要的范围做摘要。
 */
IndexBulkDeleteResult *
brinvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	heapRel;

	/* ANALYZE ONLY 模式下为空操作 */
	if (info->analyze_only)
		return stats;

	if (!stats)
		stats = palloc0_object(IndexBulkDeleteResult);
	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	/* 统计信息的其余部分由清零来初始化 */

	heapRel = table_open(IndexGetRelation(RelationGetRelid(info->index), false),
						 AccessShareLock);

	brin_vacuum_scan(info->index, info->strategy);

	brinsummarize(info->index, heapRel, BRIN_ALL_BLOCKRANGES, false,
				  &stats->num_index_tuples, &stats->num_index_tuples);

	table_close(heapRel, AccessShareLock);

	return stats;
}

/*
 * BRIN 索引的关系选项处理器
 */
bytea *
brinoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"pages_per_range", RELOPT_TYPE_INT, offsetof(BrinOptions, pagesPerRange)},
		{"autosummarize", RELOPT_TYPE_BOOL, offsetof(BrinOptions, autosummarize)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_BRIN,
									  sizeof(BrinOptions),
									  tab, lengthof(tab));
}

/*
 * 可由 SQL 调用的函数：扫描索引，并对当前尚未
 * 摘要的全部范围做摘要。
 */
Datum
brin_summarize_new_values(PG_FUNCTION_ARGS)
{
	Datum		relation = PG_GETARG_DATUM(0);

	return DirectFunctionCall2(brin_summarize_range,
							   relation,
							   Int64GetDatum((int64) BRIN_ALL_BLOCKRANGES));
}

/*
 * 可由 SQL 调用的函数：若指定页范围尚未
 * 摘要，则对其做摘要。若第二个参数为 BRIN_ALL_BLOCKRANGES，则对全部
 * 未摘要范围做摘要。
 */
Datum
brin_summarize_range(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	int64		heapBlk64 = PG_GETARG_INT64(1);
	BlockNumber heapBlk;
	Oid			heapoid;
	Relation	indexRel;
	Relation	heapRel;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	double		numSummarized = 0;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("BRIN control functions cannot be executed during recovery.")));

	if (heapBlk64 > BRIN_ALL_BLOCKRANGES || heapBlk64 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("block number out of range: %lld",
						(long long) heapBlk64)));
	heapBlk = (BlockNumber) heapBlk64;

	/*
	 * 必须先锁表再锁索引，以避免死锁。不过，若传入的
	 * indexoid 不是索引，则 IndexGetRelation() 会失败。
	 * 与其发出一条帮助不大的错误信息，不如推迟
	 * 报错，预期下面的 "是否为索引" 检查会失败。
	 */
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid))
	{
		heapRel = table_open(heapoid, ShareUpdateExclusiveLock);

		/*
		 * 自动清理会调用我们。为它着想，切换到表所有者的
		 * userid，以便任何索引函数都以该用户身份运行。同时
		 * 锁定安全受限操作，并使 GUC
		 * 变量的改动仅限于本命令。从 SQL 调用时这并无害处，
		 * 尽管也无必要，因为若用户并不拥有该索引，
		 * 我们很快就会失败。
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heapRel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}
	else
	{
		heapRel = NULL;
		/* 设置这些只是为了抑制 "未初始化变量" 告警 */
		save_userid = InvalidOid;
		save_sec_context = -1;
		save_nestlevel = -1;
	}

	indexRel = index_open(indexoid, ShareUpdateExclusiveLock);

	/* 必须是 BRIN 索引 */
	if (indexRel->rd_rel->relkind != RELKIND_INDEX ||
		indexRel->rd_rel->relam != BRIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a BRIN index",
						RelationGetRelationName(indexRel))));

	/* 用户必须拥有该索引（与 VACUUM 所需权限相当） */
	if (heapRel != NULL && !object_ownercheck(RelationRelationId, indexoid, save_userid))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   RelationGetRelationName(indexRel));

	/*
	 * 上面调用 IndexGetRelation 时没有加任何锁，因此
	 * 与索引删除/重建的竞争有很小的可能使我们
	 * 得到错误的表。重新检查。
	 */
	if (heapRel == NULL || heapoid != IndexGetRelation(indexoid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(indexRel))));

	/* 参见 gin_clean_pending_list() */
	if (indexRel->rd_index->indisvalid)
		brinsummarize(indexRel, heapRel, heapBlk, true, &numSummarized, NULL);
	else
		ereport(DEBUG1,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(indexRel))));

	/* 回滚索引函数执行的任何 GUC 改动 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 与安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	relation_close(indexRel, ShareUpdateExclusiveLock);
	relation_close(heapRel, ShareUpdateExclusiveLock);

	PG_RETURN_INT32((int32) numSummarized);
}

/*
 * 可由 SQL 调用的接口，用于把一个范围标记为未摘要
 */
Datum
brin_desummarize_range(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	int64		heapBlk64 = PG_GETARG_INT64(1);
	BlockNumber heapBlk;
	Oid			heapoid;
	Relation	heapRel;
	Relation	indexRel;
	bool		done;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("BRIN control functions cannot be executed during recovery.")));

	if (heapBlk64 > MaxBlockNumber || heapBlk64 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("block number out of range: %lld",
						(long long) heapBlk64)));
	heapBlk = (BlockNumber) heapBlk64;

	/*
	 * 必须先锁表再锁索引，以避免死锁。不过，若传入的
	 * indexoid 不是索引，则 IndexGetRelation() 会失败。
	 * 与其发出一条帮助不大的错误信息，不如推迟
	 * 报错，预期下面的 "是否为索引" 检查会失败。
	 *
	 * 与 brin_summarize_range() 不同，自动清理从不调用本函数。因此
	 * 不切换 userid。
	 */
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid))
		heapRel = table_open(heapoid, ShareUpdateExclusiveLock);
	else
		heapRel = NULL;

	indexRel = index_open(indexoid, ShareUpdateExclusiveLock);

	/* 必须是 BRIN 索引 */
	if (indexRel->rd_rel->relkind != RELKIND_INDEX ||
		indexRel->rd_rel->relam != BRIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a BRIN index",
						RelationGetRelationName(indexRel))));

	/* 用户必须拥有该索引（与 VACUUM 所需权限相当） */
	if (!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   RelationGetRelationName(indexRel));

	/*
	 * 上面调用 IndexGetRelation 时没有加任何锁，因此
	 * 与索引删除/重建的竞争有很小的可能使我们
	 * 得到错误的表。重新检查。
	 */
	if (heapRel == NULL || heapoid != IndexGetRelation(indexoid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(indexRel))));

	/* 参见 gin_clean_pending_list() */
	if (indexRel->rd_index->indisvalid)
	{
		/* 具体工作由 revmap 完成 */
		do
		{
			done = brinRevmapDesummarizeRange(indexRel, heapBlk);
		}
		while (!done);
	}
	else
		ereport(DEBUG1,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(indexRel))));

	relation_close(indexRel, ShareUpdateExclusiveLock);
	relation_close(heapRel, ShareUpdateExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * 构建用于创建或扫描 BRIN 索引的 BrinDesc
 */
BrinDesc *
brin_build_desc(Relation rel)
{
	BrinOpcInfo **opcinfo;
	BrinDesc   *bdesc;
	TupleDesc	tupdesc;
	int			totalstored = 0;
	int			keyno;
	long		totalsize;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"brin desc cxt",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	tupdesc = RelationGetDescr(rel);

	/*
	 * 为每个被索引列取得 BrinOpcInfo。同时累计
	 * 所存储的列数，因为该数量由操作符类定义。
	 */
	opcinfo = palloc_array(BrinOpcInfo *, tupdesc->natts);
	for (keyno = 0; keyno < tupdesc->natts; keyno++)
	{
		FmgrInfo   *opcInfoFn;
		Form_pg_attribute attr = TupleDescAttr(tupdesc, keyno);

		opcInfoFn = index_getprocinfo(rel, keyno + 1, BRIN_PROCNUM_OPCINFO);

		opcinfo[keyno] = (BrinOpcInfo *)
			DatumGetPointer(FunctionCall1(opcInfoFn, attr->atttypid));
		totalstored += opcinfo[keyno]->oi_nstored;
	}

	/* 分配结果结构并填入 */
	totalsize = offsetof(BrinDesc, bd_info) +
		sizeof(BrinOpcInfo *) * tupdesc->natts;

	bdesc = palloc(totalsize);
	bdesc->bd_context = cxt;
	bdesc->bd_index = rel;
	bdesc->bd_tupdesc = tupdesc;
	bdesc->bd_disktdesc = NULL; /* 惰性生成 */
	bdesc->bd_totalstored = totalstored;

	for (keyno = 0; keyno < tupdesc->natts; keyno++)
		bdesc->bd_info[keyno] = opcinfo[keyno];
	pfree(opcinfo);

	MemoryContextSwitchTo(oldcxt);

	return bdesc;
}

void
brin_free_desc(BrinDesc *bdesc)
{
	/* 确认 tupdesc 仍然有效 */
	Assert(bdesc->bd_tupdesc->tdrefcount >= 1);
	/* 不必逐个 pfree */
	MemoryContextDelete(bdesc->bd_context);
}

/*
 * 把索引的统计数据取入 *stats
 */
void
brinGetStats(Relation index, BrinStatsData *stats)
{
	Buffer		metabuffer;
	Page		metapage;
	BrinMetaPageData *metadata;

	metabuffer = ReadBuffer(index, BRIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = (BrinMetaPageData *) PageGetContents(metapage);

	stats->pagesPerRange = metadata->pagesPerRange;
	stats->revmapNumPages = metadata->lastRevmapPage - 1;

	UnlockReleaseBuffer(metabuffer);
}

/*
 * 初始化一个适合在给定索引上创建元组的 BrinBuildState。
 */
static BrinBuildState *
initialize_brin_buildstate(Relation idxRel, BrinRevmap *revmap,
						   BlockNumber pagesPerRange)
{
	BrinBuildState *state;

	state = palloc_object(BrinBuildState);

	state->bs_irel = idxRel;
	state->bs_numtuples = 0;
	state->bs_currentInsertBuf = InvalidBuffer;
	state->bs_pagesPerRange = pagesPerRange;
	state->bs_currRangeStart = 0;
	state->bs_rmAccess = revmap;
	state->bs_bdesc = brin_build_desc(idxRel);
	state->bs_dtuple = brin_new_memtuple(state->bs_bdesc);

	return state;
}

/*
 * 释放与 BrinBuildState 关联的资源。
 */
static void
terminate_brin_buildstate(BrinBuildState *state)
{
	/*
	 * 释放最后使用的索引缓冲区。不妨同时确保
	 * 该页剩余的空闲空间也记入 FSM。
	 */
	if (!BufferIsInvalid(state->bs_currentInsertBuf))
	{
		Page		page;
		Size		freespace;
		BlockNumber blk;

		page = BufferGetPage(state->bs_currentInsertBuf);
		freespace = PageGetFreeSpace(page);
		blk = BufferGetBlockNumber(state->bs_currentInsertBuf);
		ReleaseBuffer(state->bs_currentInsertBuf);
		RecordPageWithFreeSpace(state->bs_irel, blk, freespace);
		FreeSpaceMapVacuumRange(state->bs_irel, blk, blk + 1);
	}

	brin_free_desc(state->bs_bdesc);
	pfree(state->bs_dtuple);
	pfree(state);
}

/*
 * 在给定的 BRIN 索引上，对与给定堆块号对应的
 * 堆页范围做摘要。
 *
 * 本例程可以与向堆的插入并行运行。为避免
 * 汇总元组漏掉这些值，先向索引插入一个占位
 * 索引元组，再执行堆扫描；与扫描并发的事务
 * 会更新占位元组。扫描之后，把
 * 占位元组与本例程算出的元组合并。索引值的
 * 更新在循环中进行，这样若有人在我们读过之后更新了
 * 占位元组，就能发现这种情况并重试。
 * 这保证并发插入的元组不会被漏掉。
 *
 * 还有一种极端情况：本例程被要求对表末尾的未满
 * 范围做摘要。heapNumBlocks 是（可能已过时的）
 * 表大小；若发现所请求的范围超出该大小，
 * 则在插入占位元组之后重新计算表大小，以免
 * 漏掉最近追加的页。
 */
static void
summarize_range(IndexInfo *indexInfo, BrinBuildState *state, Relation heapRel,
				BlockNumber heapBlk, BlockNumber heapNumBlks)
{
	Buffer		phbuf;
	BrinTuple  *phtup;
	Size		phsz;
	OffsetNumber offset;
	BlockNumber scanNumBlks;

	/*
	 * 插入占位元组
	 */
	phbuf = InvalidBuffer;
	phtup = brin_form_placeholder_tuple(state->bs_bdesc, heapBlk, &phsz);
	offset = brin_doinsert(state->bs_irel, state->bs_pagesPerRange,
						   state->bs_rmAccess, &phbuf,
						   heapBlk, phtup, phsz);

	/*
	 * 计算范围终点。我们对表持有 ShareUpdateExclusive 锁，因此它
	 * 无法并发收缩（但可以增长）。
	 */
	Assert(heapBlk % state->bs_pagesPerRange == 0);
	if (heapBlk + state->bs_pagesPerRange > heapNumBlks)
	{
		/*
		 * 若被要求扫描我们认为是表上最后一个范围
		 * （即可能未满的范围），需要在插入占位
		 * 元组之后，重新计算我们所认为的最新页。
		 * 之后扩展该表的人会更新
		 * 占位元组，因此我们自己不扫描这些
		 * 页也没关系。注意：表可能已被扩展到
		 * 当前范围之外，因此要把结果钳制住。
		 *
		 * 好在这种情况应当很少发生。
		 */
		scanNumBlks = Min(RelationGetNumberOfBlocks(heapRel) - heapBlk,
						  state->bs_pagesPerRange);
	}
	else
	{
		/* 简单情形：已知该范围是完整的 */
		scanNumBlks = state->bs_pagesPerRange;
	}

	/*
	 * 对指定页范围内的堆块执行部分堆扫描，
	 * 并摘要其中的堆元组。这次扫描刚好停在
	 * brinbuildCallback 创建新索引项之前。
	 *
	 * 注意，这里必须使用 table_index_build_range_scan 的
	 * "any visible" 模式：否则会错过仍在进行中的事务
	 * 所插入的元组，以及其他极端
	 * 情况。
	 */
	state->bs_currRangeStart = heapBlk;
	table_index_build_range_scan(heapRel, state->bs_irel, indexInfo, false, true, false,
								 heapBlk, scanNumBlks,
								 brinbuildCallback, (void *) state, NULL);

	/*
	 * 现在用占位元组更新扫描得到的值。
	 * 这在一个循环中进行，只有成功
	 * 更新占位元组循环才会终止；若没有成功，说明
	 * 有人在我们读取之后修改了占位元组。
	 */
	for (;;)
	{
		BrinTuple  *newtup;
		Size		newsize;
		bool		didupdate;
		bool		samepage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 更新汇总元组并尝试更新。
		 */
		newtup = brin_form_tuple(state->bs_bdesc,
								 heapBlk, state->bs_dtuple, &newsize);
		samepage = brin_can_do_samepage_update(phbuf, phsz, newsize);
		didupdate =
			brin_doupdate(state->bs_irel, state->bs_pagesPerRange,
						  state->bs_rmAccess, heapBlk, phbuf, offset,
						  phtup, phsz, newtup, newsize, samepage);
		brin_free_tuple(phtup);
		brin_free_tuple(newtup);

		/* 若更新成功，则已完成。 */
		if (didupdate)
			break;

		/*
		 * 若更新没有成功，可能是有人并发更新了
		 * 占位元组。取出新版本，把它与
		 * 扫描所得的值合并，然后从头再来。（更新失败还有
		 * 其他原因，但对它们做同样处理很
		 * 简单。）
		 */
		phtup = brinGetTupleForHeapBlock(state->bs_rmAccess, heapBlk, &phbuf,
										 &offset, &phsz, BUFFER_LOCK_SHARE,
										 NULL);
		/* 占位元组必须存在 */
		if (phtup == NULL)
			elog(ERROR, "missing placeholder tuple");
		phtup = brin_copy_tuple(phtup, phsz, NULL, NULL);
		LockBuffer(phbuf, BUFFER_LOCK_UNLOCK);

		/* 把它合并进堆扫描得到的元组 */
		union_tuples(state->bs_bdesc, state->bs_dtuple, phtup);
	}

	ReleaseBuffer(phbuf);
}

/*
 * 对尚未摘要的页范围做摘要。若 pageRange 为
 * BRIN_ALL_BLOCKRANGES，则扫描整张表；否则只扫描
 * 包含给定堆页号的页范围。
 * 若 include_partial 为真，则对表末尾的未满范围
 * 做摘要，否则不做。
 *
 * 每插入一个新索引元组，*numSummarized（若非 NULL）就
 * 递增；每遇到一个已有元组，*numExisting（若非 NULL）就
 * 递增。
 */
static void
brinsummarize(Relation index, Relation heapRel, BlockNumber pageRange,
			  bool include_partial, double *numSummarized, double *numExisting)
{
	BrinRevmap *revmap;
	BrinBuildState *state = NULL;
	IndexInfo  *indexInfo = NULL;
	BlockNumber heapNumBlocks;
	BlockNumber pagesPerRange;
	Buffer		buf;
	BlockNumber startBlk;

	revmap = brinRevmapInitialize(index, &pagesPerRange, NULL);

	/* 确定要处理的页范围 */
	heapNumBlocks = RelationGetNumberOfBlocks(heapRel);
	if (pageRange == BRIN_ALL_BLOCKRANGES)
		startBlk = 0;
	else
	{
		startBlk = (pageRange / pagesPerRange) * pagesPerRange;
		heapNumBlocks = Min(heapNumBlocks, startBlk + pagesPerRange);
	}
	if (startBlk > heapNumBlocks)
	{
		/* 若起点超出表尾，则无事可做 */
		brinRevmapTerminate(revmap);
		return;
	}

	/*
	 * 扫描 revmap，找出未摘要的项。
	 */
	buf = InvalidBuffer;
	for (; startBlk < heapNumBlocks; startBlk += pagesPerRange)
	{
		BrinTuple  *tup;
		OffsetNumber off;

		/*
		 * 除非被要求连未满范围也做摘要，否则若认为
		 * 下一个范围是未满的，现在就返回。调用者在批量装载
		 * 数据通常已完成时传入真
		 * （brin_summarize_new_values），在它通常是任意调度的
		 * 维护命令（清理）的结果时传入假。
		 */
		if (!include_partial &&
			(startBlk + pagesPerRange > heapNumBlocks))
			break;

		CHECK_FOR_INTERRUPTS();

		tup = brinGetTupleForHeapBlock(revmap, startBlk, &buf, &off, NULL,
									   BUFFER_LOCK_SHARE, NULL);
		if (tup == NULL)
		{
			/* 该堆范围没有 revmap 项。对它做摘要。 */
			if (state == NULL)
			{
				/* 第一次执行 */
				Assert(!indexInfo);
				state = initialize_brin_buildstate(index, revmap,
												   pagesPerRange);
				indexInfo = BuildIndexInfo(index);
			}
			summarize_range(indexInfo, state, heapRel, startBlk, heapNumBlocks);

			/* 并为下一个范围重新初始化状态 */
			brin_memtuple_initialize(state->bs_dtuple, state->bs_bdesc);

			if (numSummarized)
				*numSummarized += 1.0;
		}
		else
		{
			if (numExisting)
				*numExisting += 1.0;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
	}

	if (BufferIsValid(buf))
		ReleaseBuffer(buf);

	/* 释放资源 */
	brinRevmapTerminate(revmap);
	if (state)
	{
		terminate_brin_buildstate(state);
		pfree(indexInfo);
	}
}

/*
 * 给定构建状态中的变形元组，把它转换成磁盘
 * 格式并插入索引，使 revmap 指向它。
 */
static void
form_and_insert_tuple(BrinBuildState *state)
{
	BrinTuple  *tup;
	Size		size;

	tup = brin_form_tuple(state->bs_bdesc, state->bs_currRangeStart,
						  state->bs_dtuple, &size);
	brin_doinsert(state->bs_irel, state->bs_pagesPerRange, state->bs_rmAccess,
				  &state->bs_currentInsertBuf, state->bs_currRangeStart,
				  tup, size);
	state->bs_numtuples++;

	pfree(tup);
}

/*
 * 给定两个变形元组，调整第一个，使它与两者中的
 * 摘要值都一致。
 */
static void
union_tuples(BrinDesc *bdesc, BrinMemTuple *a, BrinTuple *b)
{
	int			keyno;
	BrinMemTuple *db;
	MemoryContext cxt;
	MemoryContext oldcxt;

	/* 使用自己的内存上下文，以避免逐个 pfree */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"brin union",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	db = brin_deform_tuple(bdesc, b, NULL);
	MemoryContextSwitchTo(oldcxt);

	/*
	 * 检查这些范围是否为空。
	 *
	 * 若其中至少一个为空，就完全不必调用逐键的 union
	 * 函数。若 "b" 为空，就直接用 "a" 作为结果（即使
	 * "a" 为空也无妨）。若 "a" 为空而 "b" 不为空，
	 * 则用 "b" 作为结果（但必须先把数据复制到 "a"）。
	 *
	 * 只有两个范围都不为空时，才真正做逐键合并。
	 */

	/* 若 "b" 为空则忽略它，直接使用 "a"（即使它为空等也如此）。 */
	if (db->bt_empty_range)
	{
		/* 跳过逐键合并 */
		MemoryContextDelete(cxt);
		return;
	}

	/*
	 * 现在知道 "b" 不为空。若 "a" 为空，则结果是 "b"。
	 * 但必须先把数据从 "b" 复制到 "a"，因为结果就是
	 * 这样传出去的。
	 *
	 * 全局标志和逐键标志等也都必须复制。
	 */
	if (a->bt_empty_range)
	{
		for (keyno = 0; keyno < bdesc->bd_tupdesc->natts; keyno++)
		{
			int			i;
			BrinValues *col_a = &a->bt_columns[keyno];
			BrinValues *col_b = &db->bt_columns[keyno];
			BrinOpcInfo *opcinfo = bdesc->bd_info[keyno];

			col_a->bv_allnulls = col_b->bv_allnulls;
			col_a->bv_hasnulls = col_b->bv_hasnulls;

			/* 若 "b" 没有数据，则已完成。 */
			if (col_b->bv_allnulls)
				continue;

			for (i = 0; i < opcinfo->oi_nstored; i++)
				col_a->bv_values[i] =
					datumCopy(col_b->bv_values[i],
							  opcinfo->oi_typcache[i]->typbyval,
							  opcinfo->oi_typcache[i]->typlen);
		}

		/* "a" 一开始为空，但 "b" 不为空，因此记住这一点 */
		a->bt_empty_range = false;

		/* 跳过逐键合并 */
		MemoryContextDelete(cxt);
		return;
	}

	/* 现在知道两个范围都不为空。 */
	for (keyno = 0; keyno < bdesc->bd_tupdesc->natts; keyno++)
	{
		FmgrInfo   *unionFn;
		BrinValues *col_a = &a->bt_columns[keyno];
		BrinValues *col_b = &db->bt_columns[keyno];
		BrinOpcInfo *opcinfo = bdesc->bd_info[keyno];

		if (opcinfo->oi_regular_nulls)
		{
			/* "b" 的摘要是否表示任何 NULL 值？ */
			bool		b_has_nulls = (col_b->bv_hasnulls || col_b->bv_allnulls);

			/* 调整 "hasnulls"。 */
			if (!col_a->bv_allnulls && b_has_nulls)
				col_a->bv_hasnulls = true;

			/* 若 B 中没有值，就没有剩下要做的事。 */
			if (col_b->bv_allnulls)
				continue;

			/*
			 * 调整 "allnulls"。若 A 没有值，就把 B 的
			 * 值复制到 A，然后结束。此时无法运行
			 * 操作符，因为 A 中的值可能含有
			 * 垃圾。注意，已经确定 B 含有值。
			 *
			 * 同时调整 "hasnulls"，以免忘记该摘要
			 * 表示 NULL 值。这与前面的更新并不重复，
			 * 因为那只在 allnulls=false 时发生。
			 */
			if (col_a->bv_allnulls)
			{
				int			i;

				col_a->bv_allnulls = false;
				col_a->bv_hasnulls = true;

				for (i = 0; i < opcinfo->oi_nstored; i++)
					col_a->bv_values[i] =
						datumCopy(col_b->bv_values[i],
								  opcinfo->oi_typcache[i]->typbyval,
								  opcinfo->oi_typcache[i]->typlen);

				continue;
			}
		}

		unionFn = index_getprocinfo(bdesc->bd_index, keyno + 1,
									BRIN_PROCNUM_UNION);
		FunctionCall3Coll(unionFn,
						  bdesc->bd_index->rd_indcollation[keyno],
						  PointerGetDatum(bdesc),
						  PointerGetDatum(col_a),
						  PointerGetDatum(col_b));
	}

	MemoryContextDelete(cxt);
}

/*
 * brin_vacuum_scan
 *		在 VACUUM 期间对索引做一次完整扫描。
 *
 * 本例程扫描整个索引，寻找未登记的索引页，
 * 即索引扩展之后因崩溃等原因可能丢失的
 * 那些页。
 */
static void
brin_vacuum_scan(Relation idxrel, BufferAccessStrategy strategy)
{
	BlockNumber nblocks;
	BlockNumber blkno;

	/*
	 * 按物理顺序扫描索引，并清理每一页上
	 * 可能存在的混乱。
	 */
	nblocks = RelationGetNumberOfBlocks(idxrel);
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(idxrel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, strategy);

		brin_page_cleanup(idxrel, buf);

		ReleaseBuffer(buf);
	}

	/*
	 * 同时更新索引 FSM 中的全部上层页。这不仅确保
	 * 传播 brin_page_cleanup 对叶页 FSM 所做的更新，
	 * 也修复任何原先已有的损坏或过时。
	 */
	FreeSpaceMapVacuum(idxrel);
}

static bool
add_values_to_range(Relation idxRel, BrinDesc *bdesc, BrinMemTuple *dtup,
					Datum *values, bool *nulls)
{
	int			keyno;

	/* 若范围一开始为空，则肯定会修改它。 */
	bool		modified = dtup->bt_empty_range;

	/*
	 * 把新元组的键值与索引中存放的值比较；若新元组
	 * 放不进原来的范围，变形元组就会被更新（注意，这意味着
	 * 无法提前跳出循环）。记下是否发生了这种情况，
	 * 以便稍后知道要插入修改后的
	 * 元组。
	 */
	for (keyno = 0; keyno < bdesc->bd_tupdesc->natts; keyno++)
	{
		Datum		result;
		BrinValues *bval;
		FmgrInfo   *addValue;
		bool		has_nulls;

		bval = &dtup->bt_columns[keyno];

		/*
		 * 该范围是否含有实际的 NULL 值？两个标志都可能被
		 * 置位，但在加入第一行之前忽略该状态。
		 *
		 * 必须记住这一点，因为接下来会修改标志，而且
		 * 需要知道该范围一开始是否为空。
		 */
		has_nulls = ((!dtup->bt_empty_range) &&
					 (bval->bv_hasnulls || bval->bv_allnulls));

		/*
		 * 若要加入的值是 NULL，就在本地处理。否则
		 * 调用 BRIN_PROCNUM_ADDVALUE 过程。
		 */
		if (bdesc->bd_info[keyno]->oi_regular_nulls && nulls[keyno])
		{
			/*
			 * 若新值是空值，且这是第一个，就记下见过它；
			 * 否则无事可做。
			 */
			if (!bval->bv_hasnulls)
			{
				bval->bv_hasnulls = true;
				modified = true;
			}

			continue;
		}

		addValue = index_getprocinfo(idxRel, keyno + 1,
									 BRIN_PROCNUM_ADDVALUE);
		result = FunctionCall4Coll(addValue,
								   idxRel->rd_indcollation[keyno],
								   PointerGetDatum(bdesc),
								   PointerGetDatum(bval),
								   values[keyno],
								   nulls[keyno]);
		/* 若该调用返回真，就需要插入更新后的元组 */
		modified |= DatumGetBool(result);

		/*
		 * 若该范围曾含有实际的 NULL 值（即一开始不为空），
		 * 务必不要忘记这些 NULL 值。要么
		 * allnulls 标志仍为真，要么（若操作符类把它清掉了）
		 * 需要把 hasnulls 设为真。
		 *
		 * XXX 这只在操作符类修改了元组时才会发生，因此
		 * modified 标志应当已置位。
		 */
		if (has_nulls && !(bval->bv_hasnulls || bval->bv_allnulls))
		{
			Assert(modified);
			bval->bv_hasnulls = true;
		}
	}

	/*
	 * 更新完所有键的摘要后，把它标为非空。
	 *
	 * 若确实在改变该标志的值（即元组一开始是
	 * 空的），就应当已经修改了元组。因此不应当看到
	 * 未被修改的空范围。
	 */
	Assert(!dtup->bt_empty_range || modified);
	dtup->bt_empty_range = false;

	return modified;
}

static bool
check_null_keys(BrinValues *bval, ScanKey *nullkeys, int nnullkeys)
{
	int			keyno;

	/*
	 * 先检查是否有 IS [NOT] NULL 扫描键，以及是否
	 * 违反了它们。
	 */
	for (keyno = 0; keyno < nnullkeys; keyno++)
	{
		ScanKey		key = nullkeys[keyno];

		Assert(key->sk_attno == bval->bv_attno);

		/* 只处理 IS NULL/IS NOT NULL 测试 */
		if (!(key->sk_flags & SK_ISNULL))
			continue;

		if (key->sk_flags & SK_SEARCHNULL)
		{
			/* IS NULL 扫描键，但范围中没有 NULLs */
			if (!bval->bv_allnulls && !bval->bv_hasnulls)
				return false;
		}
		else if (key->sk_flags & SK_SEARCHNOTNULL)
		{
			/*
			 * 对于 IS NOT NULL，只能跳过已知仅含
			 * 空值的范围。
			 */
			if (bval->bv_allnulls)
				return false;
		}
		else
		{
			/*
			 * 既没有使用 IS NULL，也没有使用 IS NOT NULL；假定所有可索引
			 * 操作符都是严格的，因此扫描键中的值为 NULL 时
			 * 返回假。
			 */
			return false;
		}
	}

	return true;
}
