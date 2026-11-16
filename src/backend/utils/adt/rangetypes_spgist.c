/*-------------------------------------------------------------------------
 *
 * rangetypes_spgist.c
 *	  针对映射为二维点的范围实现 SP-GiST 四叉树。
 *
 * 四叉树是一种类似于二叉树、但适配于二维
 * 数据的结构。每个内部节点都包含一个点（质心），
 * 它把二维空间划分为 4 个象限。每个象限关联一个子
 * 节点。
 *
 * 把范围映射为二维点：下界作为其中一个维度，
 * 上界作为另一个维度。按照惯例，我们把下界画成
 * 横轴，把上界画成纵轴。
 *
 * 这种映射有一个特别之处：空范围的处理。空范围
 * 没有下界和上界，因此无法以直观的方式映射到
 * 二维空间。为应对这一点，根节点可以拥有第 5 个
 * 象限，专门保留给空范围。此外，树中还可以存在
 * 没有质心的内部节点。它们只包含两个子节点：
 * 一个用于空范围，另一个用于非空范围。这样的节点
 * 可以作为根节点出现，也可以出现在根节点第 5 个
 * 子之下（此时它只含空范围）。
 *
 * SP-GiST 的 picksplit 函数使用两轴上的中位数作为质心。
 * 该实现只使用范围元素数据类型的比较函数，因此
 * 适用于任何范围类型。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/rangetypes_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/spgist.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/rangetypes.h"

static int16 getQuadrant(TypeCacheEntry *typcache, const RangeType *centroid,
						 const RangeType *tst);
static int	bound_cmp(const void *a, const void *b, void *arg);

static int	adjacent_inner_consistent(TypeCacheEntry *typcache,
									  const RangeBound *arg, const RangeBound *centroid,
									  const RangeBound *prev);
static int	adjacent_cmp_bounds(TypeCacheEntry *typcache, const RangeBound *arg,
								const RangeBound *centroid);

/*
 * SP-GiST 'config' 接口函数。
 */
Datum
spg_range_quad_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = ANYRANGEOID;
	cfg->labelType = VOIDOID;	/* 无需节点标签 */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

/*----------
 * 判断二维映射后的范围相对于质心会落入哪个
 * 象限。
 *
 * 象限编号方式如下：
 *
 *	 4	|  1
 *	----+----
 *	 3	|  2
 *
 * 其中范围的下界为横轴，上界为
 * 纵轴。
 *
 * 位于某个轴上的范围被视为落在垂直轴上取值更
 * 大的那一象限。也就是说，横轴上的值被归入
 * 象限 1 或 4，纵轴上的值则被归入
 * 象限 1 或 2。等于质心的范围被视为位于
 * 象限 1。
 *
 * 空范围被视为位于特殊的象限 5。
 *----------
 */
static int16
getQuadrant(TypeCacheEntry *typcache, const RangeType *centroid, const RangeType *tst)
{
	RangeBound	centroidLower,
				centroidUpper;
	bool		centroidEmpty;
	RangeBound	lower,
				upper;
	bool		empty;

	range_deserialize(typcache, centroid, &centroidLower, &centroidUpper,
					  &centroidEmpty);
	range_deserialize(typcache, tst, &lower, &upper, &empty);

	if (empty)
		return 5;

	if (range_cmp_bounds(typcache, &lower, &centroidLower) >= 0)
	{
		if (range_cmp_bounds(typcache, &upper, &centroidUpper) >= 0)
			return 1;
		else
			return 2;
	}
	else
	{
		if (range_cmp_bounds(typcache, &upper, &centroidUpper) >= 0)
			return 4;
		else
			return 3;
	}
}

/*
 * SP-GiST choose 函数：为新加入的范围选择路径。
 */
Datum
spg_range_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	RangeType  *inRange = DatumGetRangeTypeP(in->datum),
			   *centroid;
	int16		quadrant;
	TypeCacheEntry *typcache;

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN 由核心设置 */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = RangeTypePGetDatum(inRange);
		PG_RETURN_VOID();
	}

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(inRange));

	/*
	 * 没有质心的节点完全依据范围是否为空来划分。
	 * 空范围都进入子节点 0，非空范围都进入节点
	 * 1。
	 */
	if (!in->hasPrefix)
	{
		out->resultType = spgMatchNode;
		if (RangeIsEmpty(inRange))
			out->result.matchNode.nodeN = 0;
		else
			out->result.matchNode.nodeN = 1;
		out->result.matchNode.levelAdd = 1;
		out->result.matchNode.restDatum = RangeTypePGetDatum(inRange);
		PG_RETURN_VOID();
	}

	centroid = DatumGetRangeTypeP(in->prefixDatum);
	quadrant = getQuadrant(typcache, centroid, inRange);

	Assert(quadrant <= in->nNodes);

	/* 选择与象限号匹配的节点 */
	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = quadrant - 1;
	out->result.matchNode.levelAdd = 1;
	out->result.matchNode.restDatum = RangeTypePGetDatum(inRange);

	PG_RETURN_VOID();
}

/*
 * 排序用边界比较。
 */
static int
bound_cmp(const void *a, const void *b, void *arg)
{
	RangeBound *ba = (RangeBound *) a;
	RangeBound *bb = (RangeBound *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, ba, bb);
}

/*
 * Picksplit SP-GiST 函数：拆分范围到各节点，选"质心"
 * 范围，并按象限分配各范围。
 */
Datum
spg_range_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	int			j;
	int			nonEmptyCount;
	RangeType  *centroid;
	bool		empty;
	TypeCacheEntry *typcache;

	/* 用下界和上界的中位数值作为质心范围 */
	RangeBound *lowerBounds,
			   *upperBounds;

	typcache = range_get_typcache(fcinfo,
								  RangeTypeGetOid(DatumGetRangeTypeP(in->datums[0])));

	/* 为边界分配内存 */
	lowerBounds = palloc(sizeof(RangeBound) * in->nTuples);
	upperBounds = palloc(sizeof(RangeBound) * in->nTuples);
	j = 0;

	/* 反序列化范围边界，统计非空范围数 */
	for (i = 0; i < in->nTuples; i++)
	{
		range_deserialize(typcache, DatumGetRangeTypeP(in->datums[i]),
						  &lowerBounds[j], &upperBounds[j], &empty);
		if (!empty)
			j++;
	}
	nonEmptyCount = j;

	/*
	 * 所有范围均为空。最好的办法是构造一个没有质心
	 * 的内部节点，并把所有范围放入节点 0。若之后
	 * 加入非空范围，将被路由到节点 1。
	 */
	if (nonEmptyCount == 0)
	{
		out->nNodes = 2;
		out->hasPrefix = false;
		/* 前缀为空 */
		out->prefixDatum = PointerGetDatum(NULL);
		out->nodeLabels = NULL;

		out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
		out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

		/* 全部范围放入节点 0 */
		for (i = 0; i < in->nTuples; i++)
		{
			RangeType  *range = DatumGetRangeTypeP(in->datums[i]);

			out->leafTupleDatums[i] = RangeTypePGetDatum(range);
			out->mapTuplesToNodes[i] = 0;
		}
		PG_RETURN_VOID();
	}

	/* 排序范围边界以求得中位数 */
	qsort_arg(lowerBounds, nonEmptyCount, sizeof(RangeBound),
			  bound_cmp, typcache);
	qsort_arg(upperBounds, nonEmptyCount, sizeof(RangeBound),
			  bound_cmp, typcache);

	/* 由下界与上界的中位数构造"质心"范围 */
	centroid = range_serialize(typcache, &lowerBounds[nonEmptyCount / 2],
							   &upperBounds[nonEmptyCount / 2], false, NULL);
	out->hasPrefix = true;
	out->prefixDatum = RangeTypePGetDatum(centroid);

	/* 仅当是根节点时才为空范围创建节点 */
	out->nNodes = (in->level == 0) ? 5 : 4;
	out->nodeLabels = NULL;		/* 无需节点标签 */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * 根据相对于"质心"范围的象限，把各范围分配到
	 * 对应节点。
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		RangeType  *range = DatumGetRangeTypeP(in->datums[i]);
		int16		quadrant = getQuadrant(typcache, centroid, range);

		out->leafTupleDatums[i] = RangeTypePGetDatum(range);
		out->mapTuplesToNodes[i] = quadrant - 1;
	}

	PG_RETURN_VOID();
}

/*
 * 内部节点的 SP-GiST 一致性函数：检查哪些节点与
 * 给定的查询集合一致。
 */
Datum
spg_range_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			which;
	int			i;
	MemoryContext oldCtx;

	/*
	 * 对于相邻搜索，还需要上一个质心（若有）来提
	 * 高一致性检查的精度。此时设置 needPrevious 标志，
	 * 并将质心传入 traversalValue。
	 */
	bool		needPrevious = false;

	if (in->allTheSame)
	{
		/* 报告所有节点都应被访问 */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;
		PG_RETURN_VOID();
	}

	if (!in->hasPrefix)
	{
		/*
		 * 该内部节点没有质心。这种节点有两个子节点，
		 * 第一个用于空范围，第二个用于非空范围。
		 */
		Assert(in->nNodes == 2);

		/*
		 * which 变量的第 N 位表示第 (N - 1) 个节点应被
		 * 访问。初始时所有位都被置位。应跳过节点的位
		 * 将被清除。
		 */
		which = (1 << 1) | (1 << 2);
		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;
			bool		empty;

			/*
			 * 算子第二参数不是范围的唯一策略是
			 * RANGESTRAT_CONTAINS_ELEM。
			 */
			if (strategy != RANGESTRAT_CONTAINS_ELEM)
				empty = RangeIsEmpty(DatumGetRangeTypeP(in->scankeys[i].sk_argument));
			else
				empty = false;

			switch (strategy)
			{
				case RANGESTRAT_BEFORE:
				case RANGESTRAT_OVERLEFT:
				case RANGESTRAT_OVERLAPS:
				case RANGESTRAT_OVERRIGHT:
				case RANGESTRAT_AFTER:
				case RANGESTRAT_ADJACENT:
					/* 只要任一参数为空，这些策略就返回假 */
					if (empty)
						which = 0;
					else
						which &= (1 << 2);
					break;

				case RANGESTRAT_CONTAINS:

					/*
					 * 所有范围都包含空范围。仅非空范围
					 * 才能包含非空范围。
					 */
					if (!empty)
						which &= (1 << 2);
					break;

				case RANGESTRAT_CONTAINED_BY:

					/*
					 * 只有空范围会被空范围包含。
					 * 空范围和非空范围都能被某个非空
					 * 范围包含。
					 */
					if (empty)
						which &= (1 << 1);
					break;

				case RANGESTRAT_CONTAINS_ELEM:
					which &= (1 << 2);
					break;

				case RANGESTRAT_EQ:
					if (empty)
						which &= (1 << 1);
					else
						which &= (1 << 2);
					break;

				default:
					elog(ERROR, "unrecognized range strategy: %d", strategy);
					break;
			}
			if (which == 0)
				break;			/* 无需考虑其余条件 */
		}
	}
	else
	{
		RangeBound	centroidLower,
					centroidUpper;
		bool		centroidEmpty;
		TypeCacheEntry *typcache;
		RangeType  *centroid;

		/* 此节点有质心。取出它。 */
		centroid = DatumGetRangeTypeP(in->prefixDatum);
		typcache = range_get_typcache(fcinfo,
									  RangeTypeGetOid(centroid));
		range_deserialize(typcache, centroid, &centroidLower, &centroidUpper,
						  &centroidEmpty);

		Assert(in->nNodes == 4 || in->nNodes == 5);

		/*
		 * which 变量的第 N 位表示第 (N - 1) 个节点（第 N
		 * 象限）应被访问。初始时全部置位。可跳过节
		 * 点的位将被清除。
		 */
		which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy;
			RangeBound	lower,
						upper;
			bool		empty;
			RangeType  *range = NULL;

			RangeType  *prevCentroid = NULL;
			RangeBound	prevLower,
						prevUpper;
			bool		prevEmpty;

			/* 按扫描策略对范围边界的限制 */
			RangeBound *minLower = NULL,
					   *maxLower = NULL,
					   *minUpper = NULL,
					   *maxUpper = NULL;

			/* 范围边界的限制是否为包含式？ */
			bool		inclusive = true;
			bool		strictEmpty = true;
			int			cmp,
						which1,
						which2;

			strategy = in->scankeys[i].sk_strategy;

			/*
			 * RANGESTRAT_CONTAINS_ELEM 如同 RANGESTRAT_CONTAINS，
			 * 但其参数是单个元素。把该单个元素扩展为
			 * 只含它的范围，再按 RANGESTRAT_CONTAINS
			 * 处理。
			 */
			if (strategy == RANGESTRAT_CONTAINS_ELEM)
			{
				lower.inclusive = true;
				lower.infinite = false;
				lower.lower = true;
				lower.val = in->scankeys[i].sk_argument;

				upper.inclusive = true;
				upper.infinite = false;
				upper.lower = false;
				upper.val = in->scankeys[i].sk_argument;

				empty = false;

				strategy = RANGESTRAT_CONTAINS;
			}
			else
			{
				range = DatumGetRangeTypeP(in->scankeys[i].sk_argument);
				range_deserialize(typcache, range, &lower, &upper, &empty);
			}

			/*
			 * 多数策略的处理是：由搜索键构造外接框，
			 * 由 minLower、maxLower、minUpper、
			 * maxUpper 定义。有些策略直接修改 'which'，以
			 * 确切指定需访问的象限。
			 *
			 * 多数策略下，空搜索键匹配不到任何东西，而
			 * 空范围永远不匹配非空键。若某策略
			 * 对空范围的行为与此不同，就把 strictEmpty 设为
			 * 假。
			 */
			switch (strategy)
			{
				case RANGESTRAT_BEFORE:

					/*
					 * 若 A 的上界小于 B 的下界，则 A 位于
					 * 范围 B 之前。
					 */
					maxUpper = &lower;
					inclusive = false;
					break;

				case RANGESTRAT_OVERLEFT:

					/*
					 * 若 A 的上界不大于 B 的上界，A 不延伸
					 * 到范围 B 的右侧。
					 */
					maxUpper = &upper;
					break;

				case RANGESTRAT_OVERLAPS:

					/*
					 * 非空范围重叠：各范围的下界小于等于
					 * 对方范围的上界。
					 */
					maxLower = &upper;
					minUpper = &lower;
					break;

				case RANGESTRAT_OVERRIGHT:

					/*
					 * 若 A 的下界不小于 B 的下界，则 A 不延伸
					 * 到范围 B 的左侧。
					 */
					minLower = &lower;
					break;

				case RANGESTRAT_AFTER:

					/*
					 * 若 A 的下界大于 B 的上界，则 A 位于
					 * 范围 B 之后。
					 */
					minLower = &upper;
					inclusive = false;
					break;

				case RANGESTRAT_ADJACENT:
					if (empty)
						break;	/* 跳至strictEmpty检查。 */

					/*
					 * 先前选定的象限可能排除了下界或上界
					 * 相邻的可能。若存在上一个质心范围，
					 * 则反序列化它以进行此项检查。
					 */
					if (in->traversalValue)
					{
						prevCentroid = in->traversalValue;
						range_deserialize(typcache, prevCentroid,
										  &prevLower, &prevUpper, &prevEmpty);
					}

					/*
					 * 若范围上界要与参数的下界相邻，
					 * 将沿紧邻（且略低于）Y=lower的线找到它。
					 * 因此，若参数的下界小于质心的
					 * 上界，则该线落在象限 2 和 3 中；若
					 * 大于，该线落在象限 1 与 4 内。（参见
					 * adjacent_cmp_bounds，了解边界情况
					 * 的说明）。
					 */
					cmp = adjacent_inner_consistent(typcache, &lower,
													&centroidUpper,
													prevCentroid ? &prevUpper : NULL);
					if (cmp > 0)
						which1 = (1 << 1) | (1 << 4);
					else if (cmp < 0)
						which1 = (1 << 2) | (1 << 3);
					else
						which1 = 0;

					/*
					 * 同时搜索与参数上界相邻的范围，将沿
					 * 紧邻（并恰位于其右）X=upper 的线，
					 * 该线落在象限 3 和 4，或 1
					 * 和 2 中。
					 */
					cmp = adjacent_inner_consistent(typcache, &upper,
													&centroidLower,
													prevCentroid ? &prevLower : NULL);
					if (cmp > 0)
						which2 = (1 << 1) | (1 << 2);
					else if (cmp < 0)
						which2 = (1 << 3) | (1 << 4);
					else
						which2 = 0;

					/* 必须追查与任一边界相邻的范围。 */
					which &= which1 | which2;

					needPrevious = true;
					break;

				case RANGESTRAT_CONTAINS:

					/*
					 * 非空范围 A 包含非空范围 B 当 A 的下界
					 * 小于等于范围 B 的下界，且范围 A
					 * 的上界大于等于
					 * 范围 A 的上界。
					 *
					 * 所有非空范围都包含空范围。
					 */
					strictEmpty = false;
					if (!empty)
					{
						which &= (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
						maxLower = &lower;
						minUpper = &upper;
					}
					break;

				case RANGESTRAT_CONTAINED_BY:
					/* 与 contains 相反。 */
					strictEmpty = false;
					if (empty)
					{
						/* 空范围只会被空范围包含 */
						which &= (1 << 5);
					}
					else
					{
						minLower = &lower;
						maxUpper = &upper;
					}
					break;

				case RANGESTRAT_EQ:

					/*
					 * 相等范围只可能位于参数会被放入的
					 * 同一个象限中。
					 */
					strictEmpty = false;
					which &= (1 << getQuadrant(typcache, centroid, range));
					break;

				default:
					elog(ERROR, "unrecognized range strategy: %d", strategy);
					break;
			}

			if (strictEmpty)
			{
				if (empty)
				{
					/* 扫描键为空，没有分支满足 */
					which = 0;
					break;
				}
				else
				{
					/* 不应访问含空范围的树分支 */
					which &= (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
				}
			}

			/*
			 * 利用外接框，判断需要下降进入哪些象限。
			 *
			 */
			if (minLower)
			{
				/*
				 * 若质心的下界小于等于最小下界，第 3 和第
				 * 4 象限中任何范围的下界都更小，因而
				 * 不能匹
				 * 配。
				 */
				if (range_cmp_bounds(typcache, &centroidLower, minLower) <= 0)
					which &= (1 << 1) | (1 << 2) | (1 << 5);
			}
			if (maxLower)
			{
				/*
				 * 若质心的下界大于最大下界，第 1 和第
				 * 2 象限中的任何范围其下界也都会大于等
				 * 于，因而不能匹配。
				 * 若质心的下界等于
				 * 最大下界，则当所要查找的值严格大于该
				 * 最大值时，仍可排除第 1 和第 2 个象限。
				 *
				 */

				cmp = range_cmp_bounds(typcache, &centroidLower, maxLower);
				if (cmp > 0 || (!inclusive && cmp == 0))
					which &= (1 << 3) | (1 << 4) | (1 << 5);
			}
			if (minUpper)
			{
				/*
				 * 若质心的上界小于等于最小上界，第 2 和第
				 * 3 象限中任何范围的上界都会更小，因而
				 * 不能匹
				 * 配。
				 */
				if (range_cmp_bounds(typcache, &centroidUpper, minUpper) <= 0)
					which &= (1 << 1) | (1 << 4) | (1 << 5);
			}
			if (maxUpper)
			{
				/*
				 * 若质心的上界大于最大上界，第 1 和第 4
				 * 象限中的任何范围其上界也都会大于等
				 * 于，因而不能匹配。
				 * 若质心的上界等于
				 * 最大上界，则当所要查找的值严格大于该
				 * 最大值时，仍可排除第 1 和第 4 个象限。
				 *
				 */

				cmp = range_cmp_bounds(typcache, &centroidUpper, maxUpper);
				if (cmp > 0 || (!inclusive && cmp == 0))
					which &= (1 << 2) | (1 << 3) | (1 << 5);
			}

			if (which == 0)
				break;			/* 无需考虑其余条件 */
		}
	}

	/* 我们必须下降到 'which' 所标识的象限 */
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	if (needPrevious)
		out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);
	out->nNodes = 0;

	/*
	 * traversalValues 元素应在traversalMemoryContext
	 * 中分配。
	 */
	oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (i = 1; i <= in->nNodes; i++)
	{
		if (which & (1 << i))
		{
			/* 按需保存上一个前缀 */
			if (needPrevious)
			{
				Datum		previousCentroid;

				/*
				 * 我们知道，此处 in->prefixDatum 为 varlena，
				 * 因为它是范围
				 */
				previousCentroid = datumCopy(in->prefixDatum, false, -1);
				out->traversalValues[out->nNodes] = (void *) previousCentroid;
			}
			out->nodeNumbers[out->nNodes] = i - 1;
			out->nNodes++;
		}
	}

	MemoryContextSwitchTo(oldCtx);

	PG_RETURN_VOID();
}

/*
 * adjacent_cmp_bounds
 *
 * 给定参数与质心边界，本函数判断是否有任何与
 * 参数相邻的边界是小于质心，还是大于等于质心。
 * 为简洁，我们把参数 < 质心的情形称为"左"，
 * 参数 >= 质心时则称"右"。这与象限的排列
 * 方式一致，只要把"左"想象成"下"，"右"想象成
 * "上"即可。
 *
 * "左"情形返回 -1，"右"情形返回 1。
 */
static int
adjacent_cmp_bounds(TypeCacheEntry *typcache, const RangeBound *arg,
					const RangeBound *centroid)
{
	int			cmp;

	Assert(arg->lower != centroid->lower);

	cmp = range_cmp_bounds(typcache, arg, centroid);

	if (centroid->lower)
	{
		/*------
		 * 参数是一个上界，此时搜索的是与该边界相邻
		 * 的下界。匹配的相邻下界必须*大于*该参数
		 * 但只大一点。
		 *
		 * 下表以固定的参数边界和不同质心展示期望
		 * 的结果。CMP 列显示变量 'cmp' 的取值，ADJ 列
		 * 则显示参数与质心是否相邻（依据
		 * bounds_adjacent() 判断）。(N) 表示该情形
		 * 无需检查，因为 CMP 已经隐含了它。
		 * 设参数范围为 [..., 500)，要找的相邻范围
		 * 是 [500, ...)：
		 *
		 *	ARGUMENT   CENTROID		CMP   ADJ
		 *	[..., 500) [498, ...)	 >	  (N)	[500, ...) is to the right
		 *	[..., 500) [499, ...)	 =	  (N)	[500, ...) is to the right
		 *	[..., 500) [500, ...)	 <	   Y	[500, ...) is to the right
		 *	[..., 500) [501, ...)	 <	   N	[500, ...) is to the left
		 *
		 * 因此，当参数小于质心且不相邻时，必须
		 * 向左搜索；否则向右搜索。
		 *------
		 */
		if (cmp < 0 && !bounds_adjacent(typcache, *arg, *centroid))
			return -1;
		else
			return 1;
	}
	else
	{
		/*------
		 * 参数是一个下界，此时搜索的是与该边界相邻
		 * 的上界。匹配的相邻上界必须*小于*该参数
		 * 但只小一点。
		 *
		 *	ARGUMENT   CENTROID		CMP   ADJ
		 *	[500, ...) [..., 499)	 >	  (N)	[..., 500) is to the right
		 *	[500, ...) [..., 500)	 >	  (Y)	[..., 500) is to the right
		 *	[500, ...) [..., 501)	 =	  (N)	[..., 500) is to the left
		 *	[500, ...) [..., 502)	 <	  (N)	[..., 500) is to the left
		 *
		 * 当参数小于等于质心时必须向左搜索；否则向右
		 * 搜索。无需检查参数与质心
		 * 是否相邻，因为这一点无关
		 * 紧要。
		 *------
		 */
		if (cmp <= 0)
			return -1;
		else
			return 1;
	}
}

/*----------
 * adjacent_inner_consistent
 *
 * 与 adjacent_cmp_bounds 类似，但还要把上一层质
 * 心纳入考虑。在上层节点，我们可能曾向左（或向
 * 右）遍历以搜索与另一边界相邻的范围，尽管
 * 对这一边界而言，该方向上的匹配可能早已
 * 被排除。通过把参数与上一个质心比较，再把上
 * 一个质心与当前质心比较，我们就能确定上一层
 * 本应朝哪个方向移动，以及我们实际朝哪个方向
 * 移动。
 *
 * 若左侧可能存在匹配，返回 -1；若在右侧，
 * 返回 1。若在此质心之下不可能有匹配（因为我
 * 们已在上层将其排除），返回 0。
 *
 * XXX: 仅比较上一层与当前层并非万无一失；我们
 * 可能仍会不必要地搜索某些分支。例如，设想我们
 * 正在查找值 15，并按如下顺序遍历各质心（暂时
 * 只考虑其中一个边界）：
 *
 * Level 1: 20
 * Level 2: 50
 * Level 3: 25
 *
 * 此时，上一个质心是 50，当前质心是 25，而目标
 * 值在左侧。但由于我们在第一层已经从质心 20 移
 * 动到了 50，当前分支中不可能存在任何 < 20 的
 * 值。但仅凭观察上一个质心与当前质心，我们是
 * 不知道这一点的，于是我们向左遍历，白费功夫。
 * 我们之所以在这条分支上，是因为我们正在搜索
 * 与*另一*边界的匹配。如果我们显式记录正在搜
 * 索的边界，而非从上一个与当前质心来推断，便
 * 可避免多余的工作。
 *----------
 */
static int
adjacent_inner_consistent(TypeCacheEntry *typcache, const RangeBound *arg,
						  const RangeBound *centroid, const RangeBound *prev)
{
	if (prev)
	{
		int			prevcmp;
		int			cmp;

		/*
		 * 上一层我们本应朝哪个方向遍历，向左还是向
		 * 右？
		 */
		prevcmp = adjacent_cmp_bounds(typcache, arg, prev);

		/* 而我们实际走了哪个方向？ */
		cmp = range_cmp_bounds(typcache, centroid, prev);

		/* 若两者不一致，那就没什么可看 */
		if ((prevcmp < 0 && cmp >= 0) || (prevcmp > 0 && cmp < 0))
			return 0;
	}

	return adjacent_cmp_bounds(typcache, arg, centroid);
}

/*
 * 叶子节点的 SP-GiST 一致性函数：按查询用相应
 * 的函数检查叶子值。
 */
Datum
spg_range_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	RangeType  *leafRange = DatumGetRangeTypeP(in->leafDatum);
	TypeCacheEntry *typcache;
	bool		res;
	int			i;

	/* 测试全部精确 */
	out->recheck = false;

	/* leafDatum 原样... */
	out->leafValue = in->leafDatum;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(leafRange));

	/* 执行所需的比较 */
	res = true;
	for (i = 0; i < in->nkeys; i++)
	{
		Datum		keyDatum = in->scankeys[i].sk_argument;

		/* 调用与扫描策略对应的函数 */
		switch (in->scankeys[i].sk_strategy)
		{
			case RANGESTRAT_BEFORE:
				res = range_before_internal(typcache, leafRange,
											DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_OVERLEFT:
				res = range_overleft_internal(typcache, leafRange,
											  DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_OVERLAPS:
				res = range_overlaps_internal(typcache, leafRange,
											  DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_OVERRIGHT:
				res = range_overright_internal(typcache, leafRange,
											   DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_AFTER:
				res = range_after_internal(typcache, leafRange,
										   DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_ADJACENT:
				res = range_adjacent_internal(typcache, leafRange,
											  DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_CONTAINS:
				res = range_contains_internal(typcache, leafRange,
											  DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_CONTAINED_BY:
				res = range_contained_by_internal(typcache, leafRange,
												  DatumGetRangeTypeP(keyDatum));
				break;
			case RANGESTRAT_CONTAINS_ELEM:
				res = range_contains_elem_internal(typcache, leafRange,
												   keyDatum);
				break;
			case RANGESTRAT_EQ:
				res = range_eq_internal(typcache, leafRange,
										DatumGetRangeTypeP(keyDatum));
				break;
			default:
				elog(ERROR, "unrecognized range strategy: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		/*
		 * 若叶子 datum 不匹配某查询键，无需检查
		 * 后续的键。
		 */
		if (!res)
			break;
	}

	PG_RETURN_BOOL(res);
}
