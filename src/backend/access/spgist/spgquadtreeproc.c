/*-------------------------------------------------------------------------
 *
 * spgquadtreeproc.c
 *	  面向点的 SP-GiST 四叉树实现。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgquadtreeproc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/spgist.h"
#include "access/spgist_private.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/geo_decls.h"

Datum
spg_quad_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = POINTOID;
	cfg->labelType = VOIDOID;	/* 我们不需要节点标签 */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

#define SPTEST(f, x, y) \
	DatumGetBool(DirectFunctionCall2(f, PointPGetDatum(x), PointPGetDatum(y)))

/*
 * 确定点相对中心点落入哪个象限。
 *
 * 象限标识如下：
 *
 *	 4	|  1
 *	----+-----
 *	 3	|  2
 *
 * 落在某条轴上的点视为位于编号最小的
 * 相邻象限中。
 */
static int16
getQuadrant(Point *centroid, Point *tst)
{
	if ((SPTEST(point_above, tst, centroid) ||
		 SPTEST(point_horiz, tst, centroid)) &&
		(SPTEST(point_right, tst, centroid) ||
		 SPTEST(point_vert, tst, centroid)))
		return 1;

	if (SPTEST(point_below, tst, centroid) &&
		(SPTEST(point_right, tst, centroid) ||
		 SPTEST(point_vert, tst, centroid)))
		return 2;

	if ((SPTEST(point_below, tst, centroid) ||
		 SPTEST(point_horiz, tst, centroid)) &&
		SPTEST(point_left, tst, centroid))
		return 3;

	if (SPTEST(point_above, tst, centroid) &&
		SPTEST(point_left, tst, centroid))
		return 4;

	elog(ERROR, "getQuadrant: impossible case");
	return 0;
}

/* 返回给定边界框内指定象限的边界框 */
static BOX *
getQuadrantArea(BOX *bbox, Point *centroid, int quadrant)
{
	BOX		   *result = (BOX *) palloc(sizeof(BOX));

	switch (quadrant)
	{
		case 1:
			result->high = bbox->high;
			result->low = *centroid;
			break;
		case 2:
			result->high.x = bbox->high.x;
			result->high.y = centroid->y;
			result->low.x = centroid->x;
			result->low.y = bbox->low.y;
			break;
		case 3:
			result->high = *centroid;
			result->low = bbox->low;
			break;
		case 4:
			result->high.x = centroid->x;
			result->high.y = bbox->high.y;
			result->low.x = bbox->low.x;
			result->low.y = centroid->y;
			break;
	}

	return result;
}

Datum
spg_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	Point	   *inPoint = DatumGetPointP(in->datum),
			   *centroid;

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN 将由核心设置 */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = PointPGetDatum(inPoint);
		PG_RETURN_VOID();
	}

	Assert(in->hasPrefix);
	centroid = DatumGetPointP(in->prefixDatum);

	Assert(in->nNodes == 4);

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = getQuadrant(centroid, inPoint) - 1;
	out->result.matchNode.levelAdd = 0;
	out->result.matchNode.restDatum = PointPGetDatum(inPoint);

	PG_RETURN_VOID();
}

#ifdef USE_MEDIAN
static int
x_cmp(const void *a, const void *b, void *arg)
{
	Point	   *pa = *(Point **) a;
	Point	   *pb = *(Point **) b;

	if (pa->x == pb->x)
		return 0;
	return (pa->x > pb->x) ? 1 : -1;
}

static int
y_cmp(const void *a, const void *b, void *arg)
{
	Point	   *pa = *(Point **) a;
	Point	   *pb = *(Point **) b;

	if (pa->y == pb->y)
		return 0;
	return (pa->y > pb->y) ? 1 : -1;
}
#endif

Datum
spg_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	Point	   *centroid;

#ifdef USE_MEDIAN
	/* 用 x 与 y 的中位值作为中心点 */
	Point	  **sorted;

	sorted = palloc(sizeof(*sorted) * in->nTuples);
	for (i = 0; i < in->nTuples; i++)
		sorted[i] = DatumGetPointP(in->datums[i]);

	centroid = palloc(sizeof(*centroid));

	qsort(sorted, in->nTuples, sizeof(*sorted), x_cmp);
	centroid->x = sorted[in->nTuples >> 1]->x;
	qsort(sorted, in->nTuples, sizeof(*sorted), y_cmp);
	centroid->y = sorted[in->nTuples >> 1]->y;
#else
	/* 用 x 与 y 的平均值作为中心点 */
	centroid = palloc0(sizeof(*centroid));

	for (i = 0; i < in->nTuples; i++)
	{
		centroid->x += DatumGetPointP(in->datums[i])->x;
		centroid->y += DatumGetPointP(in->datums[i])->y;
	}

	centroid->x /= in->nTuples;
	centroid->y /= in->nTuples;
#endif

	out->hasPrefix = true;
	out->prefixDatum = PointPGetDatum(centroid);

	out->nNodes = 4;
	out->nodeLabels = NULL;		/* 我们不需要节点标签 */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		Point	   *p = DatumGetPointP(in->datums[i]);
		int			quadrant = getQuadrant(centroid, p) - 1;

		out->leafTupleDatums[i] = PointPGetDatum(p);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}


Datum
spg_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	Point	   *centroid;
	BOX			infbbox;
	BOX		   *bbox = NULL;
	int			which;
	int			i;

	Assert(in->hasPrefix);
	centroid = DatumGetPointP(in->prefixDatum);

	/*
	 * 指定了排序扫描键时，必须为它们计算距离。
	 * 为此需要计算全部子节点的边界框。在非零
	 * 层次上计算这些边界框，需要知道上层节点的
	 * 边界框。因此，我们把边界框保存到
	 * traversalValues。
	 */
	if (in->norderbys > 0)
	{
		out->distances = (double **) palloc(sizeof(double *) * in->nNodes);
		out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);

		if (in->level == 0)
		{
			double		inf = get_float8_infinity();

			infbbox.high.x = inf;
			infbbox.high.y = inf;
			infbbox.low.x = -inf;
			infbbox.low.y = -inf;
			bbox = &infbbox;
		}
		else
		{
			bbox = in->traversalValue;
			Assert(bbox);
		}
	}

	if (in->allTheSame)
	{
		/* 报告应访问全部节点 */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
		{
			out->nodeNumbers[i] = i;

			if (in->norderbys > 0)
			{
				MemoryContext oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

				/* 用父象限框作为 traversalValue */
				BOX		   *quadrant = box_copy(bbox);

				MemoryContextSwitchTo(oldCtx);

				out->traversalValues[i] = quadrant;
				out->distances[i] = spg_key_orderbys_distances(BoxPGetDatum(quadrant), false,
															   in->orderbys, in->norderbys);
			}
		}
		PG_RETURN_VOID();
	}

	Assert(in->nNodes == 4);

	/* "which" 是满足全部约束的象限位掩码 */
	which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);

	for (i = 0; i < in->nkeys; i++)
	{
		Point	   *query = DatumGetPointP(in->scankeys[i].sk_argument);
		BOX		   *boxQuery;

		switch (in->scankeys[i].sk_strategy)
		{
			case RTLeftStrategyNumber:
				if (SPTEST(point_right, centroid, query))
					which &= (1 << 3) | (1 << 4);
				break;
			case RTRightStrategyNumber:
				if (SPTEST(point_left, centroid, query))
					which &= (1 << 1) | (1 << 2);
				break;
			case RTSameStrategyNumber:
				which &= (1 << getQuadrant(centroid, query));
				break;
			case RTBelowStrategyNumber:
			case RTOldBelowStrategyNumber:
				if (SPTEST(point_above, centroid, query))
					which &= (1 << 2) | (1 << 3);
				break;
			case RTAboveStrategyNumber:
			case RTOldAboveStrategyNumber:
				if (SPTEST(point_below, centroid, query))
					which &= (1 << 1) | (1 << 4);
				break;
			case RTContainedByStrategyNumber:

				/*
				 * 对该操作符，查询是框而非点。我们
				 * 采取一点取巧，假定 DatumGetPointP
				 * 不会对指向框的指针做任何有害之事。
				 */
				boxQuery = DatumGetBoxP(in->scankeys[i].sk_argument);

				if (DatumGetBool(DirectFunctionCall2(box_contain_pt,
													 PointerGetDatum(boxQuery),
													 PointerGetDatum(centroid))))
				{
					/* 中心点在框内，故全部象限均可 */
				}
				else
				{
					/* 识别包含框全部角点的象限 quadrant */
					Point		p;
					int			r = 0;

					p = boxQuery->low;
					r |= 1 << getQuadrant(centroid, &p);
					p.y = boxQuery->high.y;
					r |= 1 << getQuadrant(centroid, &p);
					p = boxQuery->high;
					r |= 1 << getQuadrant(centroid, &p);
					p.x = boxQuery->low.x;
					r |= 1 << getQuadrant(centroid, &p);

					which &= r;
				}
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		if (which == 0)
			break;				/* 无需再考虑其余条件 */
	}

	out->levelAdds = palloc(sizeof(int) * 4);
	for (i = 0; i < 4; ++i)
		out->levelAdds[i] = 1;

	/* 必须下行到 which 所标识的象限 quadrant */
	out->nodeNumbers = (int *) palloc(sizeof(int) * 4);
	out->nNodes = 0;

	for (i = 1; i <= 4; i++)
	{
		if (which & (1 << i))
		{
			out->nodeNumbers[out->nNodes] = i - 1;

			if (in->norderbys > 0)
			{
				MemoryContext oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);
				BOX		   *quadrant = getQuadrantArea(bbox, centroid, i);

				MemoryContextSwitchTo(oldCtx);

				out->traversalValues[out->nNodes] = quadrant;

				out->distances[out->nNodes] = spg_key_orderbys_distances(BoxPGetDatum(quadrant), false,
																		 in->orderbys, in->norderbys);
			}

			out->nNodes++;
		}
	}

	PG_RETURN_VOID();
}


Datum
spg_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	Point	   *datum = DatumGetPointP(in->leafDatum);
	bool		res;
	int			i;

	/* 全部测试均为精确 */
	out->recheck = false;

	/* leafDatum 即是其值... */
	out->leafValue = in->leafDatum;

	/* 执行所需的比较 comparison */
	res = true;
	for (i = 0; i < in->nkeys; i++)
	{
		Point	   *query = DatumGetPointP(in->scankeys[i].sk_argument);

		switch (in->scankeys[i].sk_strategy)
		{
			case RTLeftStrategyNumber:
				res = SPTEST(point_left, datum, query);
				break;
			case RTRightStrategyNumber:
				res = SPTEST(point_right, datum, query);
				break;
			case RTSameStrategyNumber:
				res = SPTEST(point_eq, datum, query);
				break;
			case RTBelowStrategyNumber:
			case RTOldBelowStrategyNumber:
				res = SPTEST(point_below, datum, query);
				break;
			case RTAboveStrategyNumber:
			case RTOldAboveStrategyNumber:
				res = SPTEST(point_above, datum, query);
				break;
			case RTContainedByStrategyNumber:

				/*
				 * 对该操作符，查询是框而非点。我们
				 * 采取一点取巧，假定 DatumGetPointP
				 * 不会对指向框的指针做任何有害之事。
				 */
				res = SPTEST(box_contain_pt, query, datum);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[i].sk_strategy);
				break;
		}

		if (!res)
			break;
	}

	if (res && in->norderbys > 0)
		/* 通过了 -> 接着计算距离 */
		out->distances = spg_key_orderbys_distances(in->leafDatum, true,
													in->orderbys, in->norderbys);

	PG_RETURN_BOOL(res);
}
