/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  管理 GiST 索引关系扫描的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/relscan.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"


/*
 * GISTSearchItem 队列的配对堆比较函数
 */
static int
pairingheap_GISTSearchItem_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const GISTSearchItem *sa = (const GISTSearchItem *) a;
	const GISTSearchItem *sb = (const GISTSearchItem *) b;
	IndexScanDesc scan = (IndexScanDesc) arg;
	int			i;

	/* 按距离比较排序 */
	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		if (sa->distances[i].isnull)
		{
			if (!sb->distances[i].isnull)
				return -1;
		}
		else if (sb->distances[i].isnull)
		{
			return 1;
		}
		else
		{
			int			cmp = -float8_cmp_internal(sa->distances[i].value,
												   sb->distances[i].value);

			if (cmp != 0)
				return cmp;
		}
	}

	/* 堆项排在内页之前，以确保深度优先搜索 */
	if (GISTSearchItemIsHeap(*sa) && !GISTSearchItemIsHeap(*sb))
		return 1;
	if (!GISTSearchItemIsHeap(*sa) && GISTSearchItemIsHeap(*sb))
		return -1;

	return 0;
}


/*
 * 扫描 GiST 索引的索引访问方法 API 函数
 */

IndexScanDesc
gistbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	GISTSTATE  *giststate;
	GISTScanOpaque so;
	MemoryContext oldCxt;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	/* 首先，建立带有扫描生命周期内存上下文的 GISTSTATE */
	giststate = initGISTstate(scan->indexRelation);

	/*
	 * 下面创建的一切都在 scanCxt 中，或是 scanCxt 的子上下文，
	 * 因此在 gistendscan 中会全部自动消失。
	 */
	oldCxt = MemoryContextSwitchTo(giststate->scanCxt);

	/* 初始化不透明数据 */
	so = (GISTScanOpaque) palloc0(sizeof(GISTScanOpaqueData));
	so->giststate = giststate;
	giststate->tempCxt = createTempGistContext();
	so->queue = NULL;
	so->queueCxt = giststate->scanCxt;	/* 见 gistrescan */

	/* 大小依赖于 numberOfOrderBys 的工作区： */
	so->distances = palloc(sizeof(so->distances[0]) * scan->numberOfOrderBys);
	so->qual_ok = true;			/* 以防键数为零 */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
		scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
	}

	so->killedItems = NULL;		/* 直到需要时 */
	so->numKilled = 0;
	so->curBlkno = InvalidBlockNumber;
	so->curPageLSN = InvalidXLogRecPtr;

	scan->opaque = so;

	/*
	 * 仅索引扫描所需的全部字段在 gistrescan 中初始化，
	 * 因为我们此时还不知道是否在做仅索引扫描。
	 */

	MemoryContextSwitchTo(oldCxt);

	return scan;
}

void
gistrescan(IndexScanDesc scan, ScanKey key, int nkeys,
		   ScanKey orderbys, int norderbys)
{
	/* nkeys 和 norderbys 参数被忽略 */
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	bool		first_time;
	int			i;
	MemoryContext oldCxt;

	/* 重新扫描已有的索引扫描 --- 重置状态 */

	/*
	 * 第一次经过时，我们在 scanCxt 中创建搜索队列。
	 * 随后各次则在单独的 queueCxt 中创建队列，该上下文在
	 * 第二次调用时创建，并在更晚的调用中重置。这样，在
	 * 常见的扫描只被重新扫描一次的情况下，我们只需把
	 * 队列放在 scanCxt 里，不必承担再建第二个内存
	 * 上下文的开销。若确实重新扫描超过一次，第一个队列
	 * 就闲置到扫描结束；这点小浪费在常见情形下似乎
	 * 值得换取的节省。
	 */
	if (so->queue == NULL)
	{
		/* 第一次经过 */
		Assert(so->queueCxt == so->giststate->scanCxt);
		first_time = true;
	}
	else if (so->queueCxt == so->giststate->scanCxt)
	{
		/* 第二次经过 */
		so->queueCxt = AllocSetContextCreate(so->giststate->scanCxt,
											 "GiST queue context",
											 ALLOCSET_DEFAULT_SIZES);
		first_time = false;
	}
	else
	{
		/* 第三次或更晚经过 */
		MemoryContextReset(so->queueCxt);
		first_time = false;
	}

	/*
	 * 若在做仅索引扫描，则在第一次调用时也初始化一个
	 * 元组描述符以表示返回的索引元组，并创建在
	 * 扫描期间保存它们的内存上下文。
	 */
	if (scan->xs_want_itup && !scan->xs_hitupdesc)
	{
		int			natts;
		int			nkeyatts;
		int			attno;

		/*
		 * 索引的存储类型可能不同于被索引的原始
		 * 数据类型，因此无法直接拿索引的元组
		 * 描述符。改为用原始数据
		 * 类型构造一个描述符。
		 */
		natts = RelationGetNumberOfAttributes(scan->indexRelation);
		nkeyatts = IndexRelationGetNumberOfKeyAttributes(scan->indexRelation);
		so->giststate->fetchTupdesc = CreateTemplateTupleDesc(natts);
		for (attno = 1; attno <= nkeyatts; attno++)
		{
			TupleDescInitEntry(so->giststate->fetchTupdesc, attno, NULL,
							   scan->indexRelation->rd_opcintype[attno - 1],
							   -1, 0);
		}

		for (; attno <= natts; attno++)
		{
			/* 从 giststate->tupdesc 取 opcintype */
			TupleDescInitEntry(so->giststate->fetchTupdesc, attno, NULL,
							   TupleDescAttr(so->giststate->leafTupdesc,
											 attno - 1)->atttypid,
							   -1, 0);
		}
		scan->xs_hitupdesc = so->giststate->fetchTupdesc;

		/* 同时创建用于保存返回元组的内存上下文 */
		so->pageDataCxt = AllocSetContextCreate(so->giststate->scanCxt,
												"GiST page data context",
												ALLOCSET_DEFAULT_SIZES);
	}

	/* 为搜索队列创建新的空配对堆 */
	oldCxt = MemoryContextSwitchTo(so->queueCxt);
	so->queue = pairingheap_allocate(pairingheap_GISTSearchItem_cmp, scan);
	MemoryContextSwitchTo(oldCxt);

	so->firstCall = true;

	/* 若给出了新的扫描键则更新 */
	if (key && scan->numberOfKeys > 0)
	{
		void	  **fn_extras = NULL;

		/*
		 * 若不是第一次经过，则保留 fn_extra
		 * 指针，以便 consistentFn 若用它们缓存
		 * 数据，这些数据不会在重新扫描时泄漏。
		 */
		if (!first_time)
		{
			fn_extras = (void **) palloc(scan->numberOfKeys * sizeof(void *));
			for (i = 0; i < scan->numberOfKeys; i++)
				fn_extras[i] = scan->keyData[i].sk_func.fn_extra;
		}

		memmove(scan->keyData, key,
				scan->numberOfKeys * sizeof(ScanKeyData));

		/*
		 * 修改扫描键，使所有比较都调用 Consistent
		 * 方法。原始操作符以其策略编号的形式传给
		 * Consistent 函数，该编号可从 sk_strategy
		 * 字段取得，子类型则来自 sk_subtype
		 * 字段。
		 *
		 * 接下来，若任一键为 NULL，且该键未标以
		 * SK_SEARCHNULL/SK_SEARCHNOTNULL，则什么也找不到
		 * （即，我们假定所有可索引操作符都是严格的）。
		 */
		so->qual_ok = true;

		for (i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		skey = scan->keyData + i;

			/*
			 * 将一致性支持函数复制到 ScanKey 结构，以取代
			 * 实现过滤操作符的函数。
			 */
			fmgr_info_copy(&(skey->sk_func),
						   &(so->giststate->consistentFn[skey->sk_attno - 1]),
						   so->giststate->scanCxt);

			/* 若非第一次则恢复先前的 fn_extra 指针 */
			if (!first_time)
				skey->sk_func.fn_extra = fn_extras[i];

			if (skey->sk_flags & SK_ISNULL)
			{
				if (!(skey->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)))
					so->qual_ok = false;
			}
		}

		if (!first_time)
			pfree(fn_extras);
	}

	/* 若给出了新的 order-by 键则更新 */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
		void	  **fn_extras = NULL;

		/* 同上，若非第一次经过则保留 fn_extra */
		if (!first_time)
		{
			fn_extras = (void **) palloc(scan->numberOfOrderBys * sizeof(void *));
			for (i = 0; i < scan->numberOfOrderBys; i++)
				fn_extras[i] = scan->orderByData[i].sk_func.fn_extra;
		}

		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

		so->orderByTypes = (Oid *) palloc(scan->numberOfOrderBys * sizeof(Oid));

		/*
		 * 修改 order-by 键，使所有比较都调用 Distance
		 * 方法。原始操作符以其策略编号的形式传给
		 * Distance 函数，该编号可从 sk_strategy
		 * 字段取得，子类型则来自 sk_subtype
		 * 字段。
		 */
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = scan->orderByData + i;
			FmgrInfo   *finfo = &(so->giststate->distanceFn[skey->sk_attno - 1]);

			/* 检查我们确实有距离函数 ... */
			if (!OidIsValid(finfo->fn_oid))
				elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
					 GIST_DISTANCE_PROC, skey->sk_attno,
					 RelationGetRelationName(scan->indexRelation));

			/*
			 * 查找原始排序操作符返回的数据类型。
			 * GiST 的距离函数始终使用 float8，
			 * 但排序操作符可以是任何其他类型。
			 *
			 * XXX: 仅当排序操作符的结果类型为
			 * float4 或 float8 时，才允许距离函数有损。
			 * 否则我们不知道如何把距离返回给执行器。但
			 * 此处无法检查，因为直到距离函数首次返回
			 * *recheck = true 时，我们才知道它是否
			 * 有损。
			 */
			so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);

			/*
			 * 将距离支持函数复制到 ScanKey 结构，以取代
			 * 实现排序操作符的函数。
			 */
			fmgr_info_copy(&(skey->sk_func), finfo, so->giststate->scanCxt);

			/* 若非第一次则恢复先前的 fn_extra 指针 */
			if (!first_time)
				skey->sk_func.fn_extra = fn_extras[i];
		}

		if (!first_time)
			pfree(fn_extras);
	}

	/* 先前的 xs_hitup 已在上面的上下文重置中被 pfree */
	scan->xs_hitup = NULL;
}

void
gistendscan(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	/*
	 * freeGISTstate 足以清理 gistbeginscan 所创建的一切，
	 * 以及若 queueCxt 有单独上下文时的该上下文。
	 */
	freeGISTstate(so->giststate);
}
