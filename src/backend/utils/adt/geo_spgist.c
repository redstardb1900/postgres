/*-------------------------------------------------------------------------
 *
 * geo_spgist.c
 *	  基于 box 的四维四叉树 SP-GiST 实现
 *
 * 本模块借助四维空间中的四叉树类比，为 box 提供
 * SP-GiST 实现。SP-GiST 不允许索引相互重叠的对象，
 * 而我们让二维对象在四维空间中互不重叠。与以
 * GiST 实现的传统 R-Tree 相比，该技术有一定优势。
 * 性能测试表明，该技术在重叠对象特别多时尤其
 * 有益，这类对象被称为"意面数据"（spaghetti
 * data）。
 *
 * 与原始四叉树不同，我们把树拆分为四维空间
 * 中的 16 个象限。把它想象成把空间两次分成 4
 * 份：
 *
 *				|	   |
 *				|	   |
 *				| -----+-----
 *				|	   |
 *				|	   |
 * -------------+-------------
 *				|
 *				|
 *				|
 *				|
 *				|
 *
 * 我们用 box 数据类型作为前缀（prefix），但把它们
 * 当作四维空间中的点来对待，因为二维的 box 不足
 * 以表示四维空间中的象限边界。不过，它们足以
 * 指出下一个象限的附加
 * 边界。
 *
 * 我们借助 SP-GiST 提供的遍历值（traversal value）
 * 来计算并存储象限的边界。遍历值带有四维空间中
 * 的全部边界，并能把所需的边界传递给后续
 * 的遍历值。总之，计算下一个遍历值需要
 * 三样东西，
 * 即：
 *
 *	(1) 父节点的遍历值
 *	(2) 当前节点的象限
 *	(3) 当前节点的前缀
 *
 * 若把它们画在简化示意图上（见上图）：(1) 传递
 * 过来的边界是外侧的轴，(2) 的相关部分是另一条
 * 轴的右上部分，而 (3) 则是内侧
 * 的轴。
 *
 * 例如考虑重叠情形。当递归沿树越走越深时，
 * 当前节点内所有象限都做重叠检查，而边界
 * 将为所有象限重新计算。重叠检查所回答的问题
 * 题是：该象限中可有 box 与给定 box 重叠？若
 * 有，则这个象限会被遍历；若无，则该象
 * 限会被跳过，不再继续向下
 * 遍历。
 *
 * 这一方法在除根之外的每一层上，都为 box 每个角
 * 的每个维度提供最小值与最大值的限制；对根
 * 节点，我们把尚不具备的边界设为
 * 无穷大。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/geo_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/spgist.h"
#include "access/spgist_private.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/geo_decls.h"

/*
 * qsort 的比较器
 *
 * 这里无需使用浮点数宏，因为它只会
 * 用在影响索引性能而非正确性的
 * 地方。
 */
static int
compareDoubles(const void *a, const void *b)
{
	float8		x = *(float8 *) a;
	float8		y = *(float8 *) b;

	if (x == y)
		return 0;
	return (x > y) ? 1 : -1;
}

typedef struct
{
	float8		low;
	float8		high;
} Range;

typedef struct
{
	Range		left;
	Range		right;
} RangeBox;

typedef struct
{
	RangeBox	range_box_x;
	RangeBox	range_box_y;
} RectBox;

/*
 * 计算象限
 *
 * 象限是一个 8 位无符号整数，仅使用低 4 位。
 * 本函数接受 BOX 为输入，此时还未被转成
 * RangeBox。全部 4 位都通过比较 box 的某个角来设置，
 * 总共构成 16 个象限。
 */
static uint8
getQuadrant(BOX *centroid, BOX *inBox)
{
	uint8		quadrant = 0;

	if (inBox->low.x > centroid->low.x)
		quadrant |= 0x8;

	if (inBox->high.x > centroid->high.x)
		quadrant |= 0x4;

	if (inBox->low.y > centroid->low.y)
		quadrant |= 0x2;

	if (inBox->high.y > centroid->high.y)
		quadrant |= 0x1;

	return quadrant;
}

/*
 * BOX 转 RangeBox
 *
 * 我们把 BOX 转换为自有结构，以强调其表示
 * 四维空间点的作用；用这种结构访问值
 * 也更方便。
 */
static RangeBox *
getRangeBox(BOX *box)
{
	RangeBox   *range_box = (RangeBox *) palloc(sizeof(RangeBox));

	range_box->left.low = box->low.x;
	range_box->left.high = box->high.x;

	range_box->right.low = box->low.y;
	range_box->right.high = box->high.y;

	return range_box;
}

/*
 * 初始化遍历值
 *
 * 起初没有任何限制；需要把结构初始化为
 * 覆盖整个四维空间。
 */
static RectBox *
initRectBox(void)
{
	RectBox    *rect_box = (RectBox *) palloc(sizeof(RectBox));
	float8		infinity = get_float8_infinity();

	rect_box->range_box_x.left.low = -infinity;
	rect_box->range_box_x.left.high = infinity;

	rect_box->range_box_x.right.low = -infinity;
	rect_box->range_box_x.right.high = infinity;

	rect_box->range_box_y.left.low = -infinity;
	rect_box->range_box_y.left.high = infinity;

	rect_box->range_box_y.right.low = -infinity;
	rect_box->range_box_y.right.high = infinity;

	return rect_box;
}

/*
 * 计算下一个遍历值
 *
 * 所有质心都以 RectBox 为界，但 SP-GiST 只保存
 * box。遍历树时，我们必须用质心与象限算出
 * RectBox。
 */
static RectBox *
nextRectBox(RectBox *rect_box, RangeBox *centroid, uint8 quadrant)
{
	RectBox    *next_rect_box = (RectBox *) palloc(sizeof(RectBox));

	memcpy(next_rect_box, rect_box, sizeof(RectBox));

	if (quadrant & 0x8)
		next_rect_box->range_box_x.left.low = centroid->left.low;
	else
		next_rect_box->range_box_x.left.high = centroid->left.low;

	if (quadrant & 0x4)
		next_rect_box->range_box_x.right.low = centroid->left.high;
	else
		next_rect_box->range_box_x.right.high = centroid->left.high;

	if (quadrant & 0x2)
		next_rect_box->range_box_y.left.low = centroid->right.low;
	else
		next_rect_box->range_box_y.left.high = centroid->right.low;

	if (quadrant & 0x1)
		next_rect_box->range_box_y.right.low = centroid->right.high;
	else
		next_rect_box->range_box_y.right.high = centroid->right.high;

	return next_rect_box;
}

/* range_box 内是否有 range 能与本参数重叠？ */
static bool
overlap2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->right.high, query->low) &&
		FPle(range_box->left.low, query->high);
}

/* rect_box 内是否有矩形能与本参数重叠？ */
static bool
overlap4D(RectBox *rect_box, RangeBox *query)
{
	return overlap2D(&rect_box->range_box_x, &query->left) &&
		overlap2D(&rect_box->range_box_y, &query->right);
}

/* range_box 内是否有 range 能包含本参数？ */
static bool
contain2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->right.high, query->high) &&
		FPle(range_box->left.low, query->low);
}

/* rect_box 内是否有矩形能包含本参数？ */
static bool
contain4D(RectBox *rect_box, RangeBox *query)
{
	return contain2D(&rect_box->range_box_x, &query->left) &&
		contain2D(&rect_box->range_box_y, &query->right);
}

/* range_box 内是否有 range 会被本参数包含？ */
static bool
contained2D(RangeBox *range_box, Range *query)
{
	return FPle(range_box->left.low, query->high) &&
		FPge(range_box->left.high, query->low) &&
		FPle(range_box->right.low, query->high) &&
		FPge(range_box->right.high, query->low);
}

/* rect_box 内是否有矩形会被本参数包含？ */
static bool
contained4D(RectBox *rect_box, RangeBox *query)
{
	return contained2D(&rect_box->range_box_x, &query->left) &&
		contained2D(&rect_box->range_box_y, &query->right);
}

/* range_box 内是否有 range 低于本参数？ */
static bool
lower2D(RangeBox *range_box, Range *query)
{
	return FPlt(range_box->left.low, query->low) &&
		FPlt(range_box->right.low, query->low);
}

/* range_box 内是否有 range 不延伸到查询右侧？ */
static bool
overLower2D(RangeBox *range_box, Range *query)
{
	return FPle(range_box->left.low, query->high) &&
		FPle(range_box->right.low, query->high);
}

/* range_box 内是否有 range 高于本参数？ */
static bool
higher2D(RangeBox *range_box, Range *query)
{
	return FPgt(range_box->left.high, query->high) &&
		FPgt(range_box->right.high, query->high);
}

/* range_box 内是否有 range 不延伸到查询左侧？ */
static bool
overHigher2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->left.high, query->low) &&
		FPge(range_box->right.high, query->low);
}

/* rect_box 内是否有矩形位于本参数左侧？ */
static bool
left4D(RectBox *rect_box, RangeBox *query)
{
	return lower2D(&rect_box->range_box_x, &query->left);
}

/* rect_box 内是否有矩形不延伸到本参数右侧？ */
static bool
overLeft4D(RectBox *rect_box, RangeBox *query)
{
	return overLower2D(&rect_box->range_box_x, &query->left);
}

/* rect_box 内是否有矩形位于本参数右侧？ */
static bool
right4D(RectBox *rect_box, RangeBox *query)
{
	return higher2D(&rect_box->range_box_x, &query->left);
}

/* rect_box 内是否有矩形不延伸到本参数左侧？ */
static bool
overRight4D(RectBox *rect_box, RangeBox *query)
{
	return overHigher2D(&rect_box->range_box_x, &query->left);
}

/* rect_box 内是否有矩形位于本参数下方？ */
static bool
below4D(RectBox *rect_box, RangeBox *query)
{
	return lower2D(&rect_box->range_box_y, &query->right);
}

/* rect_box 内是否有矩形不延伸到本参数上方？ */
static bool
overBelow4D(RectBox *rect_box, RangeBox *query)
{
	return overLower2D(&rect_box->range_box_y, &query->right);
}

/* rect_box 内是否有矩形位于本参数上方？ */
static bool
above4D(RectBox *rect_box, RangeBox *query)
{
	return higher2D(&rect_box->range_box_y, &query->right);
}

/* rect_box 内是否有矩形不延伸到本参数下方？ */
static bool
overAbove4D(RectBox *rect_box, RangeBox *query)
{
	return overHigher2D(&rect_box->range_box_y, &query->right);
}

/* 点与 rect_box 距离的下界 */
static double
pointToRectBoxDistance(Point *point, RectBox *rect_box)
{
	double		dx;
	double		dy;

	if (point->x < rect_box->range_box_x.left.low)
		dx = rect_box->range_box_x.left.low - point->x;
	else if (point->x > rect_box->range_box_x.right.high)
		dx = point->x - rect_box->range_box_x.right.high;
	else
		dx = 0;

	if (point->y < rect_box->range_box_y.left.low)
		dy = rect_box->range_box_y.left.low - point->y;
	else if (point->y > rect_box->range_box_y.right.high)
		dy = point->y - rect_box->range_box_y.right.high;
	else
		dy = 0;

	return HYPOT(dx, dy);
}


/*
 * SP-GiST 配置函数
 */
Datum
spg_box_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;
	cfg->labelType = VOIDOID;	/* 不需要节点标签。 */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * SP-GiST 选择函数
 */
Datum
spg_box_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid = DatumGetBoxP(in->prefixDatum),
			   *box = DatumGetBoxP(in->leafDatum);

	out->resultType = spgMatchNode;
	out->result.matchNode.restDatum = BoxPGetDatum(box);

	/* allTheSame 时核心会设置 nodeN。 */
	if (!in->allTheSame)
		out->result.matchNode.nodeN = getQuadrant(centroid, box);

	PG_RETURN_VOID();
}

/*
 * SP-GiST 拆分函数
 *
 * 它通过选一个中心四维点（取各 box 坐标的中位
 * 数）把 box 列表拆分进各象限。
 */
Datum
spg_box_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid;
	int			median,
				i;
	float8	   *lowXs = palloc(sizeof(float8) * in->nTuples);
	float8	   *highXs = palloc(sizeof(float8) * in->nTuples);
	float8	   *lowYs = palloc(sizeof(float8) * in->nTuples);
	float8	   *highYs = palloc(sizeof(float8) * in->nTuples);

	/* 计算所有四维坐标的中位数 */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX		   *box = DatumGetBoxP(in->datums[i]);

		lowXs[i] = box->low.x;
		highXs[i] = box->high.x;
		lowYs[i] = box->low.y;
		highYs[i] = box->high.y;
	}

	qsort(lowXs, in->nTuples, sizeof(float8), compareDoubles);
	qsort(highXs, in->nTuples, sizeof(float8), compareDoubles);
	qsort(lowYs, in->nTuples, sizeof(float8), compareDoubles);
	qsort(highYs, in->nTuples, sizeof(float8), compareDoubles);

	median = in->nTuples / 2;

	centroid = palloc(sizeof(BOX));

	centroid->low.x = lowXs[median];
	centroid->high.x = highXs[median];
	centroid->low.y = lowYs[median];
	centroid->high.y = highYs[median];

	/* 填充输出 */
	out->hasPrefix = true;
	out->prefixDatum = BoxPGetDatum(centroid);

	out->nNodes = 16;
	out->nodeLabels = NULL;		/* 不需要节点标签。 */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * 按各 range 相对"质心" range 的象限，把 range 分配
	 * 到相应节点
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX		   *box = DatumGetBoxP(in->datums[i]);
		uint8		quadrant = getQuadrant(centroid, box);

		out->leafTupleDatums[i] = BoxPGetDatum(box);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}

/*
 * 检查基于外接框的一致性方法结果是否精确。
 */
static bool
is_bounding_box_test_exact(StrategyNumber strategy)
{
	switch (strategy)
	{
		case RTLeftStrategyNumber:
		case RTOverLeftStrategyNumber:
		case RTOverRightStrategyNumber:
		case RTRightStrategyNumber:
		case RTOverBelowStrategyNumber:
		case RTBelowStrategyNumber:
		case RTAboveStrategyNumber:
		case RTOverAboveStrategyNumber:
			return true;

		default:
			return false;
	}
}

/*
 * 为 ScanKey 取外接框。
 */
static BOX *
spg_box_quad_get_scankey_bbox(ScanKey sk, bool *recheck)
{
	switch (sk->sk_subtype)
	{
		case BOXOID:
			return DatumGetBoxP(sk->sk_argument);

		case POLYGONOID:
			if (recheck && !is_bounding_box_test_exact(sk->sk_strategy))
				*recheck = true;
			return &DatumGetPolygonP(sk->sk_argument)->boundbox;

		default:
			elog(ERROR, "unrecognized scankey subtype: %d", sk->sk_subtype);
			return NULL;
	}
}

/*
 * SP-GiST 内部一致性函数
 */
Datum
spg_box_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;
	MemoryContext old_ctx;
	RectBox    *rect_box;
	uint8		quadrant;
	RangeBox   *centroid,
			  **queries;

	/*
	 * 我们保存遍历值；若是刚开始遍历树，则把它初
	 * 始化为无边界值。
	 */
	if (in->traversalValue)
		rect_box = in->traversalValue;
	else
		rect_box = initRectBox();

	if (in->allTheSame)
	{
		/* 报告所有节点都应被访问 */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;

		if (in->norderbys > 0 && in->nNodes > 0)
		{
			double	   *distances = palloc(sizeof(double) * in->norderbys);
			int			j;

			for (j = 0; j < in->norderbys; j++)
			{
				Point	   *pt = DatumGetPointP(in->orderbys[j].sk_argument);

				distances[j] = pointToRectBoxDistance(pt, rect_box);
			}

			out->distances = (double **) palloc(sizeof(double *) * in->nNodes);
			out->distances[0] = distances;

			for (i = 1; i < in->nNodes; i++)
			{
				out->distances[i] = palloc(sizeof(double) * in->norderbys);
				memcpy(out->distances[i], distances,
					   sizeof(double) * in->norderbys);
			}
		}

		PG_RETURN_VOID();
	}

	/*
	 * 为便于后续操作，我们把前缀与查询都转型为
	 * RangeBox。
	 */
	centroid = getRangeBox(DatumGetBoxP(in->prefixDatum));
	queries = (RangeBox **) palloc(in->nkeys * sizeof(RangeBox *));
	for (i = 0; i < in->nkeys; i++)
	{
		BOX		   *box = spg_box_quad_get_scankey_bbox(&in->scankeys[i], NULL);

		queries[i] = getRangeBox(box);
	}

	/* 为节点分配足够内存 */
	out->nNodes = 0;
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);
	if (in->norderbys > 0)
		out->distances = (double **) palloc(sizeof(double *) * in->nNodes);

	/*
	 * 我们切换内存上下文，因为要为新遍历值分配
	 * （next_rect_box）内存，并把这些内存块传给本函
	 * 数的后续调用。
	 */
	old_ctx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (quadrant = 0; quadrant < in->nNodes; quadrant++)
	{
		RectBox    *next_rect_box = nextRectBox(rect_box, centroid, quadrant);
		bool		flag = true;

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;

			switch (strategy)
			{
				case RTOverlapStrategyNumber:
					flag = overlap4D(next_rect_box, queries[i]);
					break;

				case RTContainsStrategyNumber:
					flag = contain4D(next_rect_box, queries[i]);
					break;

				case RTSameStrategyNumber:
				case RTContainedByStrategyNumber:
					flag = contained4D(next_rect_box, queries[i]);
					break;

				case RTLeftStrategyNumber:
					flag = left4D(next_rect_box, queries[i]);
					break;

				case RTOverLeftStrategyNumber:
					flag = overLeft4D(next_rect_box, queries[i]);
					break;

				case RTRightStrategyNumber:
					flag = right4D(next_rect_box, queries[i]);
					break;

				case RTOverRightStrategyNumber:
					flag = overRight4D(next_rect_box, queries[i]);
					break;

				case RTAboveStrategyNumber:
					flag = above4D(next_rect_box, queries[i]);
					break;

				case RTOverAboveStrategyNumber:
					flag = overAbove4D(next_rect_box, queries[i]);
					break;

				case RTBelowStrategyNumber:
					flag = below4D(next_rect_box, queries[i]);
					break;

				case RTOverBelowStrategyNumber:
					flag = overBelow4D(next_rect_box, queries[i]);
					break;

				default:
					elog(ERROR, "unrecognized strategy: %d", strategy);
			}

			/* 任一检查失败，答案即已确定。 */
			if (!flag)
				break;
		}

		if (flag)
		{
			out->traversalValues[out->nNodes] = next_rect_box;
			out->nodeNumbers[out->nNodes] = quadrant;

			if (in->norderbys > 0)
			{
				double	   *distances = palloc(sizeof(double) * in->norderbys);
				int			j;

				out->distances[out->nNodes] = distances;

				for (j = 0; j < in->norderbys; j++)
				{
					Point	   *pt = DatumGetPointP(in->orderbys[j].sk_argument);

					distances[j] = pointToRectBoxDistance(pt, next_rect_box);
				}
			}

			out->nNodes++;
		}
		else
		{
			/*
			 * 若此节点未选中，就无需保留下一遍历值
			 * 在内存上下文中。
			 */
			pfree(next_rect_box);
		}
	}

	/* 切回 */
	MemoryContextSwitchTo(old_ctx);

	PG_RETURN_VOID();
}

/*
 * SP-GiST 内部一致性函数
 */
Datum
spg_box_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	Datum		leaf = in->leafDatum;
	bool		flag = true;
	int			i;

	/* 测试均精确。 */
	out->recheck = false;

	/*
	 * 除非被要求，否则不返回 leafValue；box 与
	 * polygon 两个算子类共用本函数；后者的叶数据
	 * 类型根本不对。
	 */
	if (in->returnData)
		out->leafValue = leaf;

	/* 执行所需的各项比较。 */
	for (i = 0; i < in->nkeys; i++)
	{
		StrategyNumber strategy = in->scankeys[i].sk_strategy;
		BOX		   *box = spg_box_quad_get_scankey_bbox(&in->scankeys[i],
														&out->recheck);
		Datum		query = BoxPGetDatum(box);

		switch (strategy)
		{
			case RTOverlapStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overlap, leaf,
														query));
				break;

			case RTContainsStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contain, leaf,
														query));
				break;

			case RTContainedByStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contained, leaf,
														query));
				break;

			case RTSameStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_same, leaf,
														query));
				break;

			case RTLeftStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_left, leaf,
														query));
				break;

			case RTOverLeftStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overleft, leaf,
														query));
				break;

			case RTRightStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_right, leaf,
														query));
				break;

			case RTOverRightStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overright, leaf,
														query));
				break;

			case RTAboveStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_above, leaf,
														query));
				break;

			case RTOverAboveStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overabove, leaf,
														query));
				break;

			case RTBelowStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_below, leaf,
														query));
				break;

			case RTOverBelowStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overbelow, leaf,
														query));
				break;

			default:
				elog(ERROR, "unrecognized strategy: %d", strategy);
		}

		/* 任一检查失败，答案即已确定。 */
		if (!flag)
			break;
	}

	if (flag && in->norderbys > 0)
	{
		Oid			distfnoid = in->orderbys[0].sk_func.fn_oid;

		out->distances = spg_key_orderbys_distances(leaf, false,
													in->orderbys, in->norderbys);

		/* 计算到 polygon 的距离时必须复查 */
		out->recheckDistances = distfnoid == F_DIST_POLYP;
	}

	PG_RETURN_BOOL(flag);
}


/*
 * SP-GiST 配置函数：用于有损地以外接框表示
 * 二维类型
 */
Datum
spg_bbox_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;	/* 以外接框表示的类型 */
	cfg->labelType = VOIDOID;	/* 不需要节点标签。 */
	cfg->leafType = BOXOID;
	cfg->canReturnData = false;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * polygon 的 SP-GiST 压缩函数
 */
Datum
spg_poly_quad_compress(PG_FUNCTION_ARGS)
{
	POLYGON    *polygon = PG_GETARG_POLYGON_P(0);
	BOX		   *box;

	box = (BOX *) palloc(sizeof(BOX));
	*box = polygon->boundbox;

	PG_RETURN_BOX_P(box);
}
