/*-------------------------------------------------------------------------
 *
 * spgkdtreeproc.c
 *	  面向点的 SP-GiST k-d 树实现。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgkdtreeproc.c
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
spg_kd_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = FLOAT8OID;
	cfg->labelType = VOIDOID;	/* 我们不需要节点标签 */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

static int
getSide(double coord, bool isX, Point *tst)
{
	double		tstcoord = (isX) ? tst->x : tst->y;

	if (coord == tstcoord)
		return 0;
	else if (coord > tstcoord)
		return 1;
	else
		return -1;
}

Datum
spg_kd_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	Point	   *inPoint = DatumGetPointP(in->datum);
	double		coord;

	if (in->allTheSame)
		elog(ERROR, "allTheSame should not occur for k-d trees");

	Assert(in->hasPrefix);
	coord = DatumGetFloat8(in->prefixDatum);

	Assert(in->nNodes == 2);

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN =
		(getSide(coord, in->level % 2, inPoint) > 0) ? 0 : 1;
	out->result.matchNode.levelAdd = 1;
	out->result.matchNode.restDatum = PointPGetDatum(inPoint);

	PG_RETURN_VOID();
}

typedef struct SortedPoint
{
	Point	   *p;
	int			i;
} SortedPoint;

static int
x_cmp(const void *a, const void *b)
{
	SortedPoint *pa = (SortedPoint *) a;
	SortedPoint *pb = (SortedPoint *) b;

	if (pa->p->x == pb->p->x)
		return 0;
	return (pa->p->x > pb->p->x) ? 1 : -1;
}

static int
y_cmp(const void *a, const void *b)
{
	SortedPoint *pa = (SortedPoint *) a;
	SortedPoint *pb = (SortedPoint *) b;

	if (pa->p->y == pb->p->y)
		return 0;
	return (pa->p->y > pb->p->y) ? 1 : -1;
}


Datum
spg_kd_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int			i;
	int			middle;
	SortedPoint *sorted;
	double		coord;

	sorted = palloc(sizeof(*sorted) * in->nTuples);
	for (i = 0; i < in->nTuples; i++)
	{
		sorted[i].p = DatumGetPointP(in->datums[i]);
		sorted[i].i = i;
	}

	qsort(sorted, in->nTuples, sizeof(*sorted),
		  (in->level % 2) ? x_cmp : y_cmp);
	middle = in->nTuples >> 1;
	coord = (in->level % 2) ? sorted[middle].p->x : sorted[middle].p->y;

	out->hasPrefix = true;
	out->prefixDatum = Float8GetDatum(coord);

	out->nNodes = 2;
	out->nodeLabels = NULL;		/* 我们不需要节点标签 */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * 注意：坐标恰好等于 coord 的点可能被归入
	 * 任一节点，取决于它们在排序列表中碰巧落在
	 * 哪里。只要 inner_consistent 函数对此类情形
	 * 会下行到两侧，这就没问题。这比试图划出精
	 * 确边界的做法更好，因为即使有许多相同点值
	 * 的实例，也能保持树的平衡。因此我们绝不应
	 * 该触发 allTheSame 逻辑。
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		Point	   *p = sorted[i].p;
		int			n = sorted[i].i;

		out->mapTuplesToNodes[n] = (i < middle) ? 0 : 1;
		out->leafTupleDatums[n] = PointPGetDatum(p);
	}

	PG_RETURN_VOID();
}

Datum
spg_kd_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	double		coord;
	int			which;
	int			i;
	BOX			bboxes[2];

	Assert(in->hasPrefix);
	coord = DatumGetFloat8(in->prefixDatum);

	if (in->allTheSame)
		elog(ERROR, "allTheSame should not occur for k-d trees");

	Assert(in->nNodes == 2);

	/* "which" 是满足全部约束的子节点位掩码 */
	which = (1 << 1) | (1 << 2);

	for (i = 0; i < in->nkeys; i++)
	{
		Point	   *query = DatumGetPointP(in->scankeys[i].sk_argument);
		BOX		   *boxQuery;

		switch (in->scankeys[i].sk_strategy)
		{
			case RTLeftStrategyNumber:
				if ((in->level % 2) != 0 && FPlt(query->x, coord))
					which &= (1 << 1);
				break;
			case RTRightStrategyNumber:
				if ((in->level % 2) != 0 && FPgt(query->x, coord))
					which &= (1 << 2);
				break;
			case RTSameStrategyNumber:
				if ((in->level % 2) != 0)
				{
					if (FPlt(query->x, coord))
						which &= (1 << 1);
					else if (FPgt(query->x, coord))
						which &= (1 << 2);
				}
				else
				{
					if (FPlt(query->y, coord))
						which &= (1 << 1);
					else if (FPgt(query->y, coord))
						which &= (1 << 2);
				}
				break;
			case RTBelowStrategyNumber:
			case RTOldBelowStrategyNumber:
				if ((in->level % 2) == 0 && FPlt(query->y, coord))
					which &= (1 << 1);
				break;
			case RTAboveStrategyNumber:
			case RTOldAboveStrategyNumber:
				if ((in->level % 2) == 0 && FPgt(query->y, coord))
					which &= (1 << 2);
				break;
			case RTContainedByStrategyNumber:

				/*
				 * 对该操作符，查询是框而非点。我们
				 * 采取一点取巧，假定 DatumGetPointP
				 * 不会对指向框的指针做任何有害之事。
				 */
				boxQuery = DatumGetBoxP(in->scankeys[i].sk_argument);

				if ((in->level % 2) != 0)
				{
					if (FPlt(boxQuery->high.x, coord))
						which &= (1 << 1);
					else if (FPgt(boxQuery->low.x, coord))
						which &= (1 << 2);
				}
				else
				{
					if (FPlt(boxQuery->high.y, coord))
						which &= (1 << 1);
					else if (FPgt(boxQuery->low.y, coord))
						which &= (1 << 2);
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

	/* 必须下行到 which 所标识的子节点 */
	out->nNodes = 0;

	/* 无匹配子节点时的快速路径 */
	if (!which)
		PG_RETURN_VOID();

	out->nodeNumbers = (int *) palloc(sizeof(int) * 2);

	/*
	 * 指定了排序扫描键时，必须为它们计算距离。
	 * 为此需要计算两个子节点的边界框。在非零
	 * 层次上计算这些边界框，需要知道上层节点的
	 * 边界框。因此，我们把边界框保存到
	 * traversalValues。
	 */
	if (in->norderbys > 0)
	{
		BOX			infArea;
		BOX		   *area;

		out->distances = (double **) palloc(sizeof(double *) * in->nNodes);
		out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);

		if (in->level == 0)
		{
			float8		inf = get_float8_infinity();

			infArea.high.x = inf;
			infArea.high.y = inf;
			infArea.low.x = -inf;
			infArea.low.y = -inf;
			area = &infArea;
		}
		else
		{
			area = (BOX *) in->traversalValue;
			Assert(area);
		}

		bboxes[0].low = area->low;
		bboxes[1].high = area->high;

		if (in->level % 2)
		{
			/* 按 x 分裂框 */
			bboxes[0].high.x = bboxes[1].low.x = coord;
			bboxes[0].high.y = area->high.y;
			bboxes[1].low.y = area->low.y;
		}
		else
		{
			/* 按 y 分裂框 */
			bboxes[0].high.y = bboxes[1].low.y = coord;
			bboxes[0].high.x = area->high.x;
			bboxes[1].low.x = area->low.x;
		}
	}

	for (i = 1; i <= 2; i++)
	{
		if (which & (1 << i))
		{
			out->nodeNumbers[out->nNodes] = i - 1;

			if (in->norderbys > 0)
			{
				MemoryContext oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);
				BOX		   *box = box_copy(&bboxes[i - 1]);

				MemoryContextSwitchTo(oldCtx);

				out->traversalValues[out->nNodes] = box;

				out->distances[out->nNodes] = spg_key_orderbys_distances(BoxPGetDatum(box), false,
																		 in->orderbys, in->norderbys);
			}

			out->nNodes++;
		}
	}

	/* 同时设置层次增量 */
	out->levelAdds = (int *) palloc(sizeof(int) * 2);
	out->levelAdds[0] = 1;
	out->levelAdds[1] = 1;

	PG_RETURN_VOID();
}

/*
 * spg_kd_leaf_consistent() 与 spg_quad_leaf_consistent() 相同，
 * 因为我们支持相同的操作符和相同的叶子数据类型。
 * 因此直接借用该函数。
 */
