/*-------------------------------------------------------------------------
 *
 * spgscan.c
 *	  扫描 SP-GiST 索引的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/spgist_private.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef void (*storeRes_func) (SpGistScanOpaque so, ItemPointer heapPtr,
							   Datum leafValue, bool isNull,
							   SpGistLeafTuple leafTuple, bool recheck,
							   bool recheckDistances, double *distances);

/*
 * SpGistSearchItem 队列的配对堆比较函数。
 * KNN 搜索目前仅支持 NULLS LAST。因此在此保留该
 * 逻辑。
 */
static int
pairingheap_SpGistSearchItem_cmp(const pairingheap_node *a,
								 const pairingheap_node *b, void *arg)
{
	const SpGistSearchItem *sa = (const SpGistSearchItem *) a;
	const SpGistSearchItem *sb = (const SpGistSearchItem *) b;
	SpGistScanOpaque so = (SpGistScanOpaque) arg;
	int			i;

	if (sa->isNull)
	{
		if (!sb->isNull)
			return -1;
	}
	else if (sb->isNull)
	{
		return 1;
	}
	else
	{
		/* 按距离比较排序 */
		for (i = 0; i < so->numberOfNonNullOrderBys; i++)
		{
			if (isnan(sa->distances[i]) && isnan(sb->distances[i]))
				continue;		/* NaN == NaN */
			if (isnan(sa->distances[i]))
				return -1;		/* NaN > 数值 */
			if (isnan(sb->distances[i]))
				return 1;		/* 数值 < NaN */
			if (sa->distances[i] != sb->distances[i])
				return (sa->distances[i] < sb->distances[i]) ? 1 : -1;
		}
	}

	/* 叶子项排在内部页之前，以确保深度优先搜索 */
	if (sa->isLeaf && !sb->isLeaf)
		return 1;
	if (!sa->isLeaf && sb->isLeaf)
		return -1;

	return 0;
}

static void
spgFreeSearchItem(SpGistScanOpaque so, SpGistSearchItem *item)
{
	/* 若 isLeaf 则 value 类型为 attType，否则为 attLeafType */
	/* （不，这并没有写反；是的，确实容易混淆） */
	if (!(item->isLeaf ? so->state.attType.attbyval :
		  so->state.attLeafType.attbyval) &&
		DatumGetPointer(item->value) != NULL)
		pfree(DatumGetPointer(item->value));

	if (item->leafTuple)
		pfree(item->leafTuple);

	if (item->traversalValue)
		pfree(item->traversalValue);

	pfree(item);
}

/*
 * 将 SpGistSearchItem 加入队列
 *
 * 在队列上下文中调用
 */
static void
spgAddSearchItemToQueue(SpGistScanOpaque so, SpGistSearchItem *item)
{
	pairingheap_add(so->scanQueue, &item->phNode);
}

static SpGistSearchItem *
spgAllocSearchItem(SpGistScanOpaque so, bool isnull, double *distances)
{
	/* 仅为非 NULL 项分配距离数组 */
	SpGistSearchItem *item =
		palloc(SizeOfSpGistSearchItem(isnull ? 0 : so->numberOfNonNullOrderBys));

	item->isNull = isnull;

	if (!isnull && so->numberOfNonNullOrderBys > 0)
		memcpy(item->distances, distances,
			   sizeof(item->distances[0]) * so->numberOfNonNullOrderBys);

	return item;
}

static void
spgAddStartItem(SpGistScanOpaque so, bool isnull)
{
	SpGistSearchItem *startEntry =
		spgAllocSearchItem(so, isnull, so->zeroDistances);

	ItemPointerSet(&startEntry->heapPtr,
				   isnull ? SPGIST_NULL_BLKNO : SPGIST_ROOT_BLKNO,
				   FirstOffsetNumber);
	startEntry->isLeaf = false;
	startEntry->level = 0;
	startEntry->value = (Datum) 0;
	startEntry->leafTuple = NULL;
	startEntry->traversalValue = NULL;
	startEntry->recheck = false;
	startEntry->recheckDistances = false;

	spgAddSearchItemToQueue(so, startEntry);
}

/*
 * 初始化队列以搜索根页，并重置
 * 任何先前活动的扫描
 */
static void
resetSpGistScanOpaque(SpGistScanOpaque so)
{
	MemoryContext oldCtx;

	MemoryContextReset(so->traversalCxt);

	oldCtx = MemoryContextSwitchTo(so->traversalCxt);

	/* 仅为按距离排序的扫描初始化队列 */
	so->scanQueue = pairingheap_allocate(pairingheap_SpGistSearchItem_cmp, so);

	if (so->searchNulls)
		/* 添加工作项以扫描空值索引项 */
		spgAddStartItem(so, true);

	if (so->searchNonNulls)
		/* 添加工作项以扫描非空索引项 */
		spgAddStartItem(so, false);

	MemoryContextSwitchTo(oldCtx);

	if (so->numberOfOrderBys > 0)
	{
		/* 必须 pfree distances 以免内存泄漏 */
		int			i;

		for (i = 0; i < so->nPtrs; i++)
			if (so->distances[i])
				pfree(so->distances[i]);
	}

	if (so->want_itup)
	{
		/* 必须 pfree 重建的元组以免内存泄漏 */
		int			i;

		for (i = 0; i < so->nPtrs; i++)
			pfree(so->reconTups[i]);
	}
	so->iPtr = so->nPtrs = 0;
}

/*
 * 根据调用方给出的扫描键，在 SpGistScanOpaque 中准备扫描键
 *
 * 设置 *so 的 searchNulls、searchNonNulls、numberOfKeys、keyData 字段。
 *
 * 此处的目的是让操作符类的一致性函数不必再处理与空值
 * 相关的事项。我们假定所有可建 SPGiST 索引的操作符都是
 * 严格的，因此 RHS 为空会使扫描条件无法满足。我们还抽出
 * 任何 IS NULL/IS NOT NULL 条件；其效果反映到
 * searchNulls/searchNonNulls 中。
 */
static void
spgPrepareScanKeys(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	bool		qual_ok;
	bool		haveIsNull;
	bool		haveNotNull;
	int			nkeys;
	int			i;

	so->numberOfOrderBys = scan->numberOfOrderBys;
	so->orderByData = scan->orderByData;

	if (so->numberOfOrderBys <= 0)
		so->numberOfNonNullOrderBys = 0;
	else
	{
		int			j = 0;

		/*
		 * 移除所有 NULL 键，但记住它们在原
		 * 数组中的偏移。
		 */
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &so->orderByData[i];

			if (skey->sk_flags & SK_ISNULL)
				so->nonNullOrderByOffsets[i] = -1;
			else
			{
				if (i != j)
					so->orderByData[j] = *skey;

				so->nonNullOrderByOffsets[i] = j++;
			}
		}

		so->numberOfNonNullOrderBys = j;
	}

	if (scan->numberOfKeys <= 0)
	{
		/* 若无限定条件，则需要整索引扫描 */
		so->searchNulls = true;
		so->searchNonNulls = true;
		so->numberOfKeys = 0;
		return;
	}

	/* 检查给定的限定条件 */
	qual_ok = true;
	haveIsNull = haveNotNull = false;
	nkeys = 0;
	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scan->keyData[i];

		if (skey->sk_flags & SK_SEARCHNULL)
			haveIsNull = true;
		else if (skey->sk_flags & SK_SEARCHNOTNULL)
			haveNotNull = true;
		else if (skey->sk_flags & SK_ISNULL)
		{
			/* 带空参数的普通限定条件 - 无法满足 */
			qual_ok = false;
			break;
		}
		else
		{
			/* 普通限定条件，传播到 so->keyData */
			so->keyData[nkeys++] = *skey;
			/* 这实际上产生了非空要求 */
			haveNotNull = true;
		}
	}

	/* IS NULL 与其他条件组合时无法满足 */
	if (haveIsNull && haveNotNull)
		qual_ok = false;

	/* 输出结果 */
	if (qual_ok)
	{
		so->searchNulls = haveIsNull;
		so->searchNonNulls = haveNotNull;
		so->numberOfKeys = nkeys;
	}
	else
	{
		so->searchNulls = false;
		so->searchNonNulls = false;
		so->numberOfKeys = 0;
	}
}

IndexScanDesc
spgbeginscan(Relation rel, int keysz, int orderbysz)
{
	IndexScanDesc scan;
	SpGistScanOpaque so;
	int			i;

	scan = RelationGetIndexScan(rel, keysz, orderbysz);

	so = (SpGistScanOpaque) palloc0(sizeof(SpGistScanOpaqueData));
	if (keysz > 0)
		so->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * keysz);
	else
		so->keyData = NULL;
	initSpGistState(&so->state, scan->indexRelation);

	so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
										"SP-GiST search temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->traversalCxt = AllocSetContextCreate(CurrentMemoryContext,
											 "SP-GiST traversal-value context",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * 以防这是仅索引扫描，设置 reconTupDesc 与 xs_hitupdesc，
	 * 并确保键列显示为 attType 类型。
	 * （做这项工作可能白费，有点烦人，但对多数操作符类
	 * 我们可以复用索引的 reldesc 而不必新建。）
	 */
	so->reconTupDesc = scan->xs_hitupdesc =
		getSpGistTupleDesc(rel, &so->state.attType);

	/* 为 order-by 扫描分配所需的各种数组 */
	if (scan->numberOfOrderBys > 0)
	{
		/* 将在 spgrescan 中填充，但在此分配空间 */
		so->orderByTypes = (Oid *)
			palloc(sizeof(Oid) * scan->numberOfOrderBys);
		so->nonNullOrderByOffsets = (int *)
			palloc(sizeof(int) * scan->numberOfOrderBys);

		/* 这些数组内容恒定，因此现在就可以填充 */
		so->zeroDistances = (double *)
			palloc(sizeof(double) * scan->numberOfOrderBys);
		so->infDistances = (double *)
			palloc(sizeof(double) * scan->numberOfOrderBys);

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			so->zeroDistances[i] = 0.0;
			so->infDistances[i] = get_float8_infinity();
		}

		scan->xs_orderbyvals = (Datum *)
			palloc0(sizeof(Datum) * scan->numberOfOrderBys);
		scan->xs_orderbynulls = (bool *)
			palloc(sizeof(bool) * scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true,
			   sizeof(bool) * scan->numberOfOrderBys);
	}

	fmgr_info_copy(&so->innerConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_INNER_CONSISTENT_PROC),
				   CurrentMemoryContext);

	fmgr_info_copy(&so->leafConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_LEAF_CONSISTENT_PROC),
				   CurrentMemoryContext);

	so->indexCollation = rel->rd_indcollation[0];

	scan->opaque = so;

	return scan;
}

void
spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	/* 将扫描键复制到本地存储 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));

	/* 若需要则初始化 order-by 数据 */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
		int			i;

		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &scan->orderByData[i];

			/*
			 * 查找原始排序操作符返回的数据
			 * 类型。SP-GiST 的距离函数始终使用 float8，
			 * 但排序操作符可以是任何其他类型。
			 *
			 * XXX: 仅当排序操作符的结果类型为
			 * float4 或 float8 时，才允许距离函数有损。否则
			 * 我们不知道如何把距离返回给执行器。但
			 * 此处无法检查，因为直到距离函数首次返回
			 * *recheck = true 时，我们才知道它是否
			 * 有损。
			 */
			so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);
		}
	}

	/* 预处理扫描键，在 *so 中建立表示 */
	spgPrepareScanKeys(scan);

	/* 建立起始队列项 */
	resetSpGistScanOpaque(so);

	/* 为统计计数一次索引扫描 */
	pgstat_count_index_scan(scan->indexRelation);
}

void
spgendscan(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	MemoryContextDelete(so->tempCxt);
	MemoryContextDelete(so->traversalCxt);

	if (so->keyData)
		pfree(so->keyData);

	if (so->state.leafTupDesc &&
		so->state.leafTupDesc != RelationGetDescr(so->state.index))
		FreeTupleDesc(so->state.leafTupDesc);

	if (so->state.deadTupleStorage)
		pfree(so->state.deadTupleStorage);

	if (scan->numberOfOrderBys > 0)
	{
		pfree(so->orderByTypes);
		pfree(so->nonNullOrderByOffsets);
		pfree(so->zeroDistances);
		pfree(so->infDistances);
		pfree(scan->xs_orderbyvals);
		pfree(scan->xs_orderbynulls);
	}

	pfree(so);
}

/*
 * 叶子 SpGistSearchItem 构造函数，在队列上下文中调用
 */
static SpGistSearchItem *
spgNewHeapItem(SpGistScanOpaque so, int level, SpGistLeafTuple leafTuple,
			   Datum leafValue, bool recheck, bool recheckDistances,
			   bool isnull, double *distances)
{
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->level = level;
	item->heapPtr = leafTuple->heapPtr;

	/*
	 * 若需要重建值，则从临时上下文复制到队列上下文。
	 * 注意：若我们未要求，leaf_consistent 方法可能未提供值，
	 * 且略有问题的方法可能提供错误类型的值。正确的
	 * leafValue 类型是 attType 而非 leafType。
	 */
	if (so->want_itup)
	{
		item->value = isnull ? (Datum) 0 :
			datumCopy(leafValue, so->state.attType.attbyval,
					  so->state.attType.attlen);

		/*
		 * 若稍后需要重建 INCLUDE 属性，则保存整个
		 * 叶子元组，以便从中取出 INCLUDE 属性。
		 */
		if (so->state.leafTupDesc->natts > 1)
		{
			item->leafTuple = palloc(leafTuple->size);
			memcpy(item->leafTuple, leafTuple, leafTuple->size);
		}
		else
			item->leafTuple = NULL;
	}
	else
	{
		item->value = (Datum) 0;
		item->leafTuple = NULL;
	}
	item->traversalValue = NULL;
	item->isLeaf = true;
	item->recheck = recheck;
	item->recheckDistances = recheckDistances;

	return item;
}

/*
 * 测试叶子元组是否满足所有扫描键
 *
 * 在以下情况将 *reportedSome 设为 true：
 *		扫描无序且该项满足扫描键
 */
static bool
spgLeafTest(SpGistScanOpaque so, SpGistSearchItem *item,
			SpGistLeafTuple leafTuple, bool isnull,
			bool *reportedSome, storeRes_func storeRes)
{
	Datum		leafValue;
	double	   *distances;
	bool		result;
	bool		recheck;
	bool		recheckDistances;

	if (isnull)
	{
		/* 除非需要空值，否则不应到达空值页 */
		Assert(so->searchNulls);
		leafValue = (Datum) 0;
		distances = NULL;
		recheck = false;
		recheckDistances = false;
		result = true;
	}
	else
	{
		spgLeafConsistentIn in;
		spgLeafConsistentOut out;

		/* 调用 leaf_consistent 时使用临时上下文 */
		MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);

		in.scankeys = so->keyData;
		in.nkeys = so->numberOfKeys;
		in.orderbys = so->orderByData;
		in.norderbys = so->numberOfNonNullOrderBys;
		Assert(!item->isLeaf);	/* 否则 reconstructedValue 类型会不对 */
		in.reconstructedValue = item->value;
		in.traversalValue = item->traversalValue;
		in.level = item->level;
		in.returnData = so->want_itup;
		in.leafDatum = SGLTDATUM(leafTuple, &so->state);

		out.leafValue = (Datum) 0;
		out.recheck = false;
		out.distances = NULL;
		out.recheckDistances = false;

		result = DatumGetBool(FunctionCall2Coll(&so->leafConsistentFn,
												so->indexCollation,
												PointerGetDatum(&in),
												PointerGetDatum(&out)));
		recheck = out.recheck;
		recheckDistances = out.recheckDistances;
		leafValue = out.leafValue;
		distances = out.distances;

		MemoryContextSwitchTo(oldCxt);
	}

	if (result)
	{
		/* 项通过了扫描键 */
		if (so->numberOfNonNullOrderBys > 0)
		{
			/* 扫描有序 -> 将该项加入队列 */
			MemoryContext oldCxt = MemoryContextSwitchTo(so->traversalCxt);
			SpGistSearchItem *heapItem = spgNewHeapItem(so, item->level,
														leafTuple,
														leafValue,
														recheck,
														recheckDistances,
														isnull,
														distances);

			spgAddSearchItemToQueue(so, heapItem);

			MemoryContextSwitchTo(oldCxt);
		}
		else
		{
			/* 非有序扫描，因此立即报告该项 */
			Assert(!recheckDistances);
			storeRes(so, &leafTuple->heapPtr, leafValue, isnull,
					 leafTuple, recheck, false, NULL);
			*reportedSome = true;
		}
	}

	return result;
}

/* inner_consistent 方法的打包初始化器 */
static void
spgInitInnerConsistentIn(spgInnerConsistentIn *in,
						 SpGistScanOpaque so,
						 SpGistSearchItem *item,
						 SpGistInnerTuple innerTuple)
{
	in->scankeys = so->keyData;
	in->orderbys = so->orderByData;
	in->nkeys = so->numberOfKeys;
	in->norderbys = so->numberOfNonNullOrderBys;
	Assert(!item->isLeaf);		/* 否则 reconstructedValue 类型会不对 */
	in->reconstructedValue = item->value;
	in->traversalMemoryContext = so->traversalCxt;
	in->traversalValue = item->traversalValue;
	in->level = item->level;
	in->returnData = so->want_itup;
	in->allTheSame = innerTuple->allTheSame;
	in->hasPrefix = (innerTuple->prefixSize > 0);
	in->prefixDatum = SGITDATUM(innerTuple, &so->state);
	in->nNodes = innerTuple->nNodes;
	in->nodeLabels = spgExtractNodeLabels(&so->state, innerTuple);
}

static SpGistSearchItem *
spgMakeInnerItem(SpGistScanOpaque so,
				 SpGistSearchItem *parentItem,
				 SpGistNodeTuple tuple,
				 spgInnerConsistentOut *out, int i, bool isnull,
				 double *distances)
{
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->heapPtr = tuple->t_tid;
	item->level = out->levelAdds ? parentItem->level + out->levelAdds[i]
		: parentItem->level;

	/* 必须把值从临时上下文中复制出来 */
	/* （请记住重建值的类型为 leafType） */
	item->value = out->reconstructedValues
		? datumCopy(out->reconstructedValues[i],
					so->state.attLeafType.attbyval,
					so->state.attLeafType.attlen)
		: (Datum) 0;

	item->leafTuple = NULL;

	/*
	 * out.traversalValues 的元素应分配在
	 * in.traversalMemoryContext 中，它实际上是索引扫描的
	 * 长寿命上下文。
	 */
	item->traversalValue =
		out->traversalValues ? out->traversalValues[i] : NULL;

	item->isLeaf = false;
	item->recheck = false;
	item->recheckDistances = false;

	return item;
}

static void
spgInnerTest(SpGistScanOpaque so, SpGistSearchItem *item,
			 SpGistInnerTuple innerTuple, bool isnull)
{
	MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);
	spgInnerConsistentOut out;
	int			nNodes = innerTuple->nNodes;
	int			i;

	memset(&out, 0, sizeof(out));

	if (!isnull)
	{
		spgInnerConsistentIn in;

		spgInitInnerConsistentIn(&in, so, item, innerTuple);

		/* 使用用户定义的内部一致性方法 */
		FunctionCall2Coll(&so->innerConsistentFn,
						  so->indexCollation,
						  PointerGetDatum(&in),
						  PointerGetDatum(&out));
	}
	else
	{
		/* 强制访问所有子节点 */
		out.nNodes = nNodes;
		out.nodeNumbers = (int *) palloc(sizeof(int) * nNodes);
		for (i = 0; i < nNodes; i++)
			out.nodeNumbers[i] = i;
	}

	/* 若 allTheSame，则应全部匹配或全不匹配 */
	if (innerTuple->allTheSame && out.nNodes != 0 && out.nNodes != nNodes)
		elog(ERROR, "inconsistent inner_consistent results for allTheSame inner tuple");

	if (out.nNodes)
	{
		/* 收集节点指针 */
		SpGistNodeTuple node;
		SpGistNodeTuple *nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * nNodes);

		SGITITERATE(innerTuple, i, node)
		{
			nodes[i] = node;
		}

		MemoryContextSwitchTo(so->traversalCxt);

		for (i = 0; i < out.nNodes; i++)
		{
			int			nodeN = out.nodeNumbers[i];
			SpGistSearchItem *innerItem;
			double	   *distances;

			Assert(nodeN >= 0 && nodeN < nNodes);

			node = nodes[nodeN];

			if (!ItemPointerIsValid(&node->t_tid))
				continue;

			/*
			 * 若 innerConsistentFn() 未能返回距离，或该项为 NULL
			 * （其距离实际未使用），则使用无穷大距离。
			 */
			distances = out.distances ? out.distances[i] : so->infDistances;

			innerItem = spgMakeInnerItem(so, item, node, &out, i, isnull,
										 distances);

			spgAddSearchItemToQueue(so, innerItem);
		}
	}

	MemoryContextSwitchTo(oldCxt);
}

/* 返回（有序）扫描中的下一项；若索引已耗尽则返回 null */
static SpGistSearchItem *
spgGetNextQueueItem(SpGistScanOpaque so)
{
	if (pairingheap_is_empty(so->scanQueue))
		return NULL;			/* 两个堆都空时完成 */

	/* 返回项；由调用方负责 pfree */
	return (SpGistSearchItem *) pairingheap_remove_first(so->scanQueue);
}

enum SpGistSpecialOffsetNumbers
{
	SpGistBreakOffsetNumber = InvalidOffsetNumber,
	SpGistRedirectOffsetNumber = MaxOffsetNumber + 1,
	SpGistErrorOffsetNumber = MaxOffsetNumber + 2
};

static OffsetNumber
spgTestLeafTuple(SpGistScanOpaque so,
				 SpGistSearchItem *item,
				 Page page, OffsetNumber offset,
				 bool isnull, bool isroot,
				 bool *reportedSome,
				 storeRes_func storeRes)
{
	SpGistLeafTuple leafTuple = (SpGistLeafTuple)
		PageGetItem(page, PageGetItemId(page, offset));

	if (leafTuple->tupstate != SPGIST_LIVE)
	{
		if (!isroot)			/* 根上的所有元组都应为活的 */
		{
			if (leafTuple->tupstate == SPGIST_REDIRECT)
			{
				/* 重定向元组应为链中的首个 */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* 把注意力转到重定向目标 */
				item->heapPtr = ((SpGistDeadTuple) leafTuple)->pointer;
				Assert(ItemPointerGetBlockNumber(&item->heapPtr) != SPGIST_METAPAGE_BLKNO);
				return SpGistRedirectOffsetNumber;
			}

			if (leafTuple->tupstate == SPGIST_DEAD)
			{
				/* 死元组应为链中的首个 */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* 本页上没有活项 */
				Assert(SGLT_GET_NEXTOFFSET(leafTuple) == InvalidOffsetNumber);
				return SpGistBreakOffsetNumber;
			}
		}

		/* 我们不应到达占位符 */
		elog(ERROR, "unexpected SPGiST tuple state: %d", leafTuple->tupstate);
		return SpGistErrorOffsetNumber;
	}

	Assert(ItemPointerIsValid(&leafTuple->heapPtr));

	spgLeafTest(so, item, leafTuple, isnull, reportedSome, storeRes);

	return SGLT_GET_NEXTOFFSET(leafTuple);
}

/*
 * 遍历树，并将所有通过扫描限定条件的元组报告给
 * storeRes 子例程。
 *
 * 若 scanWholeIndex 为真，就照此做完。否则，一旦已
 * 报告至少一个元组，就在下一页边界处停止。
 */
static void
spgWalk(Relation index, SpGistScanOpaque so, bool scanWholeIndex,
		storeRes_func storeRes, Snapshot snapshot)
{
	Buffer		buffer = InvalidBuffer;
	bool		reportedSome = false;

	while (scanWholeIndex || !reportedSome)
	{
		SpGistSearchItem *item = spgGetNextQueueItem(so);

		if (item == NULL)
			break;				/* 队列中没有更多项 -> 完成 */

redirect:
		/* 检查中断，以防陷入无限循环 */
		CHECK_FOR_INTERRUPTS();

		if (item->isLeaf)
		{
			/* 仅在有序搜索时才把堆项存入队列 */
			Assert(so->numberOfNonNullOrderBys > 0);
			storeRes(so, &item->heapPtr, item->value, item->isNull,
					 item->leafTuple, item->recheck,
					 item->recheckDistances, item->distances);
			reportedSome = true;
		}
		else
		{
			BlockNumber blkno = ItemPointerGetBlockNumber(&item->heapPtr);
			OffsetNumber offset = ItemPointerGetOffsetNumber(&item->heapPtr);
			Page		page;
			bool		isnull;

			if (buffer == InvalidBuffer)
			{
				buffer = ReadBuffer(index, blkno);
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
			}
			else if (blkno != BufferGetBlockNumber(buffer))
			{
				UnlockReleaseBuffer(buffer);
				buffer = ReadBuffer(index, blkno);
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
			}

			/* 否则新指针指向同一页，无需操作 */

			page = BufferGetPage(buffer);
			TestForOldSnapshot(snapshot, index, page);

			isnull = SpGistPageStoresNulls(page) ? true : false;

			if (SpGistPageIsLeaf(page))
			{
				/* 页是叶子页——即其上所有元组都是堆项 */
				OffsetNumber max = PageGetMaxOffsetNumber(page);

				if (SpGistBlockIsRoot(blkno))
				{
					/* 当根是叶子时，检查其所有元组 */
					for (offset = FirstOffsetNumber; offset <= max; offset++)
						(void) spgTestLeafTuple(so, item, page, offset,
												isnull, true,
												&reportedSome, storeRes);
				}
				else
				{
					/* 普通情况：只检查我们到达的链 */
					while (offset != InvalidOffsetNumber)
					{
						Assert(offset >= FirstOffsetNumber && offset <= max);
						offset = spgTestLeafTuple(so, item, page, offset,
												  isnull, false,
												  &reportedSome, storeRes);
						if (offset == SpGistRedirectOffsetNumber)
							goto redirect;
					}
				}
			}
			else				/* 页是内部页 */
			{
				SpGistInnerTuple innerTuple = (SpGistInnerTuple)
					PageGetItem(page, PageGetItemId(page, offset));

				if (innerTuple->tupstate != SPGIST_LIVE)
				{
					if (innerTuple->tupstate == SPGIST_REDIRECT)
					{
						/* 把注意力转到重定向目标 */
						item->heapPtr = ((SpGistDeadTuple) innerTuple)->pointer;
						Assert(ItemPointerGetBlockNumber(&item->heapPtr) !=
							   SPGIST_METAPAGE_BLKNO);
						goto redirect;
					}
					elog(ERROR, "unexpected SPGiST tuple state: %d",
						 innerTuple->tupstate);
				}

				spgInnerTest(so, item, innerTuple, isnull);
			}
		}

		/* 处理完本扫描项 */
		spgFreeSearchItem(so, item);
		/* 在继续下一项之前清空临时上下文 */
		MemoryContextReset(so->tempCxt);
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);
}


/* getbitmap 情形下的 storeRes 子例程 */
static void
storeBitmap(SpGistScanOpaque so, ItemPointer heapPtr,
			Datum leafValue, bool isnull,
			SpGistLeafTuple leafTuple, bool recheck,
			bool recheckDistances, double *distances)
{
	Assert(!recheckDistances && !distances);
	tbm_add_tuples(so->tbm, heapPtr, 1, recheck);
	so->ntids++;
}

int64
spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	/* 将 want_itup 复制到 *so，这样就不必单独传递 */
	so->want_itup = false;

	so->tbm = tbm;
	so->ntids = 0;

	spgWalk(scan->indexRelation, so, true, storeBitmap, scan->xs_snapshot);

	return so->ntids;
}

/* gettuple 情形下的 storeRes 子例程 */
static void
storeGettuple(SpGistScanOpaque so, ItemPointer heapPtr,
			  Datum leafValue, bool isnull,
			  SpGistLeafTuple leafTuple, bool recheck,
			  bool recheckDistances, double *nonNullDistances)
{
	Assert(so->nPtrs < MaxIndexTuplesPerPage);
	so->heapPtrs[so->nPtrs] = *heapPtr;
	so->recheck[so->nPtrs] = recheck;
	so->recheckDistances[so->nPtrs] = recheckDistances;

	if (so->numberOfOrderBys > 0)
	{
		if (isnull || so->numberOfNonNullOrderBys <= 0)
			so->distances[so->nPtrs] = NULL;
		else
		{
			IndexOrderByDistance *distances =
				palloc(sizeof(distances[0]) * so->numberOfOrderBys);
			int			i;

			for (i = 0; i < so->numberOfOrderBys; i++)
			{
				int			offset = so->nonNullOrderByOffsets[i];

				if (offset >= 0)
				{
					/* 复制非 NULL 距离值 */
					distances[i].value = nonNullDistances[offset];
					distances[i].isnull = false;
				}
				else
				{
					/* 设置距离的 NULL 标志。 */
					distances[i].value = 0.0;
					distances[i].isnull = true;
				}
			}

			so->distances[so->nPtrs] = distances;
		}
	}

	if (so->want_itup)
	{
		/*
		 * 重建索引数据。反正要把 datum 从临时上下文中
		 * 复制出来，不妨在此创建元组。
		 */
		Datum		leafDatums[INDEX_MAX_KEYS];
		bool		leafIsnulls[INDEX_MAX_KEYS];

		/* 仅当旧元组有 INCLUDE 属性时才需要变形 */
		if (so->state.leafTupDesc->natts > 1)
			spgDeformLeafTuple(leafTuple, so->state.leafTupDesc,
							   leafDatums, leafIsnulls, isnull);

		leafDatums[spgKeyColumn] = leafValue;
		leafIsnulls[spgKeyColumn] = isnull;

		so->reconTups[so->nPtrs] = heap_form_tuple(so->reconTupDesc,
												   leafDatums,
												   leafIsnulls);
	}
	so->nPtrs++;
}

bool
spggettuple(IndexScanDesc scan, ScanDirection dir)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "SP-GiST only supports forward scan direction");

	/* 将 want_itup 复制到 *so，这样就不必单独传递 */
	so->want_itup = scan->xs_want_itup;

	for (;;)
	{
		if (so->iPtr < so->nPtrs)
		{
			/* 继续返回已报告的元组 */
			scan->xs_heaptid = so->heapPtrs[so->iPtr];
			scan->xs_recheck = so->recheck[so->iPtr];
			scan->xs_hitup = so->reconTups[so->iPtr];

			if (so->numberOfOrderBys > 0)
				index_store_float8_orderby_distances(scan, so->orderByTypes,
													 so->distances[so->iPtr],
													 so->recheckDistances[so->iPtr]);
			so->iPtr++;
			return true;
		}

		if (so->numberOfOrderBys > 0)
		{
			/* 必须 pfree distances 以免内存泄漏 */
			int			i;

			for (i = 0; i < so->nPtrs; i++)
				if (so->distances[i])
					pfree(so->distances[i]);
		}

		if (so->want_itup)
		{
			/* 必须 pfree 重建的元组以免内存泄漏 */
			int			i;

			for (i = 0; i < so->nPtrs; i++)
				pfree(so->reconTups[i]);
		}
		so->iPtr = so->nPtrs = 0;

		spgWalk(scan->indexRelation, so, false, storeGettuple,
				scan->xs_snapshot);

		if (so->nPtrs == 0)
			break;				/* 扫描必定已完成 */
	}

	return false;
}

bool
spgcanreturn(Relation index, int attno)
{
	SpGistCache *cache;

	/* INCLUDE 属性在仅索引扫描中总是可以取回 */
	if (attno > 1)
		return true;

	/* 若操作符类配置函数如此声明，则可以做到 */
	cache = spgGetCache(index);

	return cache->config.canReturnData;
}
