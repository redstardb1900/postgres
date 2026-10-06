/*-------------------------------------------------------------------------
 *
 * gistproc.c
 *	  二维对象（盒子、多边形、圆、点）上 GiSTs 的
 *	  支持过程。
 *
 * 这给出 R-tree 行为，采用 Guttman 的多项式时间分裂算法。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	src/backend/access/gist/gistproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/geo_decls.h"
#include "utils/sortsupport.h"


static bool gist_box_leaf_consistent(BOX *key, BOX *query,
									 StrategyNumber strategy);
static bool rtree_internal_consistent(BOX *key, BOX *query,
									  StrategyNumber strategy);

static uint64 point_zorder_internal(float4 x, float4 y);
static uint64 part_bits32_by2(uint32 x);
static uint32 ieee_float32_to_uint32(float f);
static int	gist_bbox_zorder_cmp(Datum a, Datum b, SortSupport ssup);
static Datum gist_bbox_zorder_abbrev_convert(Datum original, SortSupport ssup);
static bool gist_bbox_zorder_abbrev_abort(int memtupcount, SortSupport ssup);


/* 分裂所接受的最小比例 */
#define LIMIT_RATIO 0.3


/**************************************************
 * 盒子操作
 **************************************************/

/*
 * 计算两个盒子 a 与 b 的并集。结果存入 *n。
 */
static void
rt_box_union(BOX *n, const BOX *a, const BOX *b)
{
	n->high.x = float8_max(a->high.x, b->high.x);
	n->high.y = float8_max(a->high.y, b->high.y);
	n->low.x = float8_min(a->low.x, b->low.x);
	n->low.y = float8_min(a->low.y, b->low.y);
}

/*
 * 用于惩罚计算的 BOX 大小。
 * 结果可为 +Infinity，但不能为 NaN。
 */
static float8
size_box(const BOX *box)
{
	/*
	 * 检查零宽度情形。注意我们把零宽乘以无穷的盒子的大小
	 * 定义为零。必须以某种方式特殊处理，因为天真地把无穷
	 * 乘以零会产生 NaN。
	 *
	 * 小于情形本不应发生，但若发生则视为"零"。
	 */
	if (float8_le(box->high.x, box->low.x) ||
		float8_le(box->high.y, box->low.y))
		return 0.0;

	/*
	 * 我们把 NaN 视为大于 +Infinity，因此任何涉及 NaN 与
	 * 非 NaN 的距离都是无穷。注意前一检查已排除 low 字段
	 * 为 NaNs 的可能。
	 */
	if (isnan(box->high.x) || isnan(box->high.y))
		return get_float8_infinity();
	return float8_mul(float8_mi(box->high.x, box->low.x),
					  float8_mi(box->high.y, box->low.y));
}

/*
 * 返回两盒子并集相对原 BOX 面积增大的量。
 * 结果可为 +Infinity，但不能为 NaN。
 */
static float8
box_penalty(const BOX *original, const BOX *new)
{
	BOX			unionbox;

	rt_box_union(&unionbox, original, new);
	return float8_mi(size_box(&unionbox), size_box(original));
}

/*
 * 盒子的 GiST 一致性方法
 *
 * 若对 entry 下方所有数据项 x，谓词 x op query 必为假，
 * 则应返回 false，其中 op 是 pg_amop 表中与 strategy
 * 对应的操作符。
 */
Datum
gist_box_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *query = PG_GETARG_BOX_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);

	/* 本函数处理的所有情形都是精确的 */
	*recheck = false;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * 若 entry 非叶，用 rtree_internal_consistent，否则用
	 * gist_box_leaf_consistent
	 */
	if (GIST_LEAF(entry))
		PG_RETURN_BOOL(gist_box_leaf_consistent(DatumGetBoxP(entry->key),
												query,
												strategy));
	else
		PG_RETURN_BOOL(rtree_internal_consistent(DatumGetBoxP(entry->key),
												 query,
												 strategy));
}

/*
 * 扩大 BOX b 以包含 addon。
 */
static void
adjustBox(BOX *b, const BOX *addon)
{
	if (float8_lt(b->high.x, addon->high.x))
		b->high.x = addon->high.x;
	if (float8_gt(b->low.x, addon->low.x))
		b->low.x = addon->low.x;
	if (float8_lt(b->high.y, addon->high.y))
		b->high.y = addon->high.y;
	if (float8_gt(b->low.y, addon->low.y))
		b->low.y = addon->low.y;
}

/*
 * 盒子的 GiST 联合方法
 *
 * 返回包围 entryvec 中全部项的最小边界框
 */
Datum
gist_box_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			numranges,
				i;
	BOX		   *cur,
			   *pageunion;

	numranges = entryvec->n;
	pageunion = (BOX *) palloc(sizeof(BOX));
	cur = DatumGetBoxP(entryvec->vector[0].key);
	memcpy(pageunion, cur, sizeof(BOX));

	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetBoxP(entryvec->vector[i].key);
		adjustBox(pageunion, cur);
	}
	*sizep = sizeof(BOX);

	PG_RETURN_POINTER(pageunion);
}

/*
 * 我们在 GiST 索引中按盒子存储盒子，因此不需要
 * 压缩、解压或取回函数。
 */

/*
 * 盒子的 GiST 惩罚方法（点也使用）
 *
 * 与 R-tree 论文一样，我们用面积变化作为惩罚度量
 */
Datum
gist_box_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	BOX		   *origbox = DatumGetBoxP(origentry->key);
	BOX		   *newbox = DatumGetBoxP(newentry->key);

	*result = (float) box_penalty(origbox, newbox);
	PG_RETURN_POINTER(result);
}

/*
 * 平凡分裂：一半项放到一页，
 * 另一半放到另一页
 */
static void
fallbackSplit(GistEntryVector *entryvec, GIST_SPLITVEC *v)
{
	OffsetNumber i,
				maxoff;
	BOX		   *unionL = NULL,
			   *unionR = NULL;
	int			nbytes;

	maxoff = entryvec->n - 1;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		BOX		   *cur = DatumGetBoxP(entryvec->vector[i].key);

		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = i;
			if (unionL == NULL)
			{
				unionL = (BOX *) palloc(sizeof(BOX));
				*unionL = *cur;
			}
			else
				adjustBox(unionL, cur);

			v->spl_nleft++;
		}
		else
		{
			v->spl_right[v->spl_nright] = i;
			if (unionR == NULL)
			{
				unionR = (BOX *) palloc(sizeof(BOX));
				*unionR = *cur;
			}
			else
				adjustBox(unionR, cur);

			v->spl_nright++;
		}
	}

	v->spl_ldatum = BoxPGetDatum(unionL);
	v->spl_rdatum = BoxPGetDatum(unionR);
}

/*
 * 表示可放入任一组而不影响所选轴上重叠的项
 * 的信息（"公共项"）。
 */
typedef struct
{
	/* 项在初始数组中的下标 */
	int			index;
	/* 把项插入不同组时惩罚之差 */
	float8		delta;
} CommonEntry;

/*
 * g_box_consider_split 的上下文。包含当前已选
 * 分裂以及若干一般信息。
 */
typedef struct
{
	int			entriesCount;	/* 正在分裂的项总数 */
	BOX			boundingBox;	/* 全部项上的最小边界框 */

	/* 以下为当前已选分裂的信息 */

	bool		first;			/* 为真表示尚未选定任何分裂 */

	float8		leftUpper;		/* 左区间的上界 */
	float8		rightLower;		/* 右区间的下界 */

	float4		ratio;
	float4		overlap;
	int			dim;			/* 此次分裂的轴 */
	float8		range;			/* 总体 MBR 投影到所
								 * 选轴上的宽度 */
} ConsiderSplitContext;

/*
 * Interval 表示盒子到轴上的投影。
 */
typedef struct
{
	float8		lower,
				upper;
} SplitInterval;

/*
 * 按区间下界比较的区间比较函数；
 */
static int
interval_cmp_lower(const void *i1, const void *i2)
{
	float8		lower1 = ((const SplitInterval *) i1)->lower,
				lower2 = ((const SplitInterval *) i2)->lower;

	return float8_cmp_internal(lower1, lower2);
}

/*
 * 按区间上界比较的区间比较函数；
 */
static int
interval_cmp_upper(const void *i1, const void *i2)
{
	float8		upper1 = ((const SplitInterval *) i1)->upper,
				upper2 = ((const SplitInterval *) i2)->upper;

	return float8_cmp_internal(upper1, upper2);
}

/*
 * 把负值（或 NaN）替换为零。
 */
static inline float
non_negative(float val)
{
	if (val >= 0.0f)
		return val;
	else
		return 0.0f;
}

/*
 * 考虑用更好的分裂替换当前已选分裂。
 */
static inline void
g_box_consider_split(ConsiderSplitContext *context, int dimNum,
					 float8 rightLower, int minLeftCount,
					 float8 leftUpper, int maxLeftCount)
{
	int			leftCount,
				rightCount;
	float4		ratio,
				overlap;
	float8		range;

	/*
	 * 在假定公共项尽可能均匀分布的前提下，
	 * 计算项分布比例。
	 */
	if (minLeftCount >= (context->entriesCount + 1) / 2)
	{
		leftCount = minLeftCount;
	}
	else
	{
		if (maxLeftCount <= context->entriesCount / 2)
			leftCount = maxLeftCount;
		else
			leftCount = context->entriesCount / 2;
	}
	rightCount = context->entriesCount - leftCount;

	/*
	 * 分裂比例——较小一组的大小与项总数
	 * 之商。
	 */
	ratio = float4_div(Min(leftCount, rightCount), context->entriesCount);

	if (ratio > LIMIT_RATIO)
	{
		bool		selectthis = false;

		/*
		 * 比例可接受，因此拿当前分裂与先前选定的
		 * 比较。同一维的分裂中，我们寻找最小重叠
		 * （允许负值）以及相同重叠下的最小比例。
		 * 若找到更小（非负）重叠或相同重叠下更小
		 * 的范围，则切换维度。
		 */
		if (dimNum == 0)
			range = float8_mi(context->boundingBox.high.x,
							  context->boundingBox.low.x);
		else
			range = float8_mi(context->boundingBox.high.y,
							  context->boundingBox.low.y);

		overlap = float8_div(float8_mi(leftUpper, rightLower), range);

		/* 若尚无先前选定，则选此分裂 */
		if (context->first)
			selectthis = true;
		else if (context->dim == dimNum)
		{
			/*
			 * 在同一维内，若新分裂有更小重叠，或
			 * 重叠相同但比例更好，则选用它。
			 */
			if (overlap < context->overlap ||
				(overlap == context->overlap && ratio > context->ratio))
				selectthis = true;
		}
		else
		{
			/*
			 * 跨维度时，若新分裂有更小的*非负*重叠，
			 * 或相同*非负*重叠但范围更大，则选用它。
			 * 此条件与文中所述不同。在叶 MBRs 自身
			 * 不重叠的数据集上，非重叠分裂（即具有
			 * 零*非负*重叠的分裂）经常可行。此时分
			 * 裂倾向于沿同一维进行，因为最远的非重
			 * 叠分裂（即负重叠最低者）往往出现在与
			 * 前次分裂相同的维度上。于是 MBRs 会沿另
			 * 一维变得非常细长，从而导致搜索性能变
			 * 差。把范围作为第二分裂准则可使 MBRs 更
			 * 接近方形。把*非负*重叠而不是重叠作为
			 * 第一分裂准则，能让范围准则有机会起作
			 * 用，因为非重叠分裂在这一准则下是彼此
			 * 等价的，范围便可成为有效的决胜因素。
			 * 这有助于避免 MBR 沿单一维度过度拉长。
			 */
			if (non_negative(overlap) < non_negative(context->overlap) ||
				(range > context->range &&
				 non_negative(overlap) <= non_negative(context->overlap)))
				selectthis = true;
		}

		if (selectthis)
		{
			/* 保存所选分裂的信息 */
			context->first = false;
			context->ratio = ratio;
			context->range = range;
			context->overlap = overlap;
			context->rightLower = rightLower;
			context->leftUpper = leftUpper;
			context->dim = dimNum;
		}
	}
}

/*
 * 按 delta 比较公共项。
 */
static int
common_entry_cmp(const void *i1, const void *i2)
{
	float8		delta1 = ((const CommonEntry *) i1)->delta,
				delta2 = ((const CommonEntry *) i2)->delta;

	return float8_cmp_internal(delta1, delta2);
}

/*
 * --------------------------------------------------------------------------
 * 双重排序分裂算法。盒子与点都使用此算法。
 *
 * 算法沿各轴考虑分裂，以找到盒子的分裂方式。
 * 每个项先投影为 X 轴上的区间，再考虑把区间
 * 分成两组的不同方式，力求使两组重叠最小。
 * 然后对 Y 轴重复同样过程，并选出总体最佳
 * 分裂。分裂质量由该轴上的重叠及其他若干准则
 * 决定（见
 * g_box_consider_split）。
 *
 * 之后，全部项被分成三组：
 *
 * 1) 应放入左组的项
 * 2) 应放入右组的项
 * 3) "公共项"：可放入任一组而不影响
 *	所选轴上的重叠。
 *
 * 公共项通过最小化惩罚来分配。
 *
 * 细节见：
 * "A new double sorting-based node splitting algorithm for R-tree", A. Korotkov
 * http://syrcose.ispras.ru/2011/files/SYRCoSE2011_Proceedings.pdf#page=36
 * --------------------------------------------------------------------------
 */
Datum
gist_box_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i,
				maxoff;
	ConsiderSplitContext context;
	BOX		   *box,
			   *leftBox,
			   *rightBox;
	int			dim,
				commonEntriesCount;
	SplitInterval *intervalsLower,
			   *intervalsUpper;
	CommonEntry *commonEntries;
	int			nentries;

	memset(&context, 0, sizeof(ConsiderSplitContext));

	maxoff = entryvec->n - 1;
	nentries = context.entriesCount = maxoff - FirstOffsetNumber + 1;

	/* 为各轴上的区间分配数组 */
	intervalsLower = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));
	intervalsUpper = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));

	/*
	 * 计算全部项上的总体最小边界框。
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		box = DatumGetBoxP(entryvec->vector[i].key);
		if (i == FirstOffsetNumber)
			context.boundingBox = *box;
		else
			adjustBox(&context.boundingBox, box);
	}

	/*
	 * 沿各轴迭代以搜索最优分裂。
	 */
	context.first = true;		/* 尚未选定任何分裂 */
	for (dim = 0; dim < 2; dim++)
	{
		float8		leftUpper,
					rightLower;
		int			i1,
					i2;

		/* 把每个项投影为所选轴上的区间。 */
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			box = DatumGetBoxP(entryvec->vector[i].key);
			if (dim == 0)
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->low.x;
				intervalsLower[i - FirstOffsetNumber].upper = box->high.x;
			}
			else
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->low.y;
				intervalsLower[i - FirstOffsetNumber].upper = box->high.y;
			}
		}

		/*
		 * 制作两个区间数组：一个按下界排序，另一个
		 * 按上界排序。
		 */
		memcpy(intervalsUpper, intervalsLower,
			   sizeof(SplitInterval) * nentries);
		qsort(intervalsLower, nentries, sizeof(SplitInterval),
			  interval_cmp_lower);
		qsort(intervalsUpper, nentries, sizeof(SplitInterval),
			  interval_cmp_upper);

		/*----
		 * 目标是形成左、右区间，使每个项
		 * 区间都包含于左区间或右区间（或两者）。
		 *
		 * 例如，对区间 (0,1)、(1,3)、(2,3)、(2,4)：
		 *
		 * 0 1 2 3 4
		 * +-+
		 *	 +---+
		 *	   +-+
		 *	   +---+
		 *
		 * 左、右区间形如 (0,a) 与 (b,4)。
		 * 我们先考虑 b 为某项下界的分裂。
		 * 遍历全部项，对每个 b 计算尽可能小的
		 * a。再考虑 a 为某项上界的分裂，对每个
		 * a 计算尽可能大的
		 * b。
		 *
		 * 上例中，第一轮循环会考虑分裂：
		 * b=0: (0,1)-(0,4)
		 * b=1: (0,1)-(1,4)
		 * b=2: (0,3)-(2,4)
		 *
		 * 第二轮循环：
		 * a=1: (0,1)-(1,4)
		 * a=3: (0,3)-(2,4)
		 * a=4: (0,4)-(2,4)
		 */

		/*
		 * 遍历右组下界，寻找左组尽可能小的
		 * 上界。
		 */
		i1 = 0;
		i2 = 0;
		rightLower = intervalsLower[i1].lower;
		leftUpper = intervalsUpper[i2].lower;
		while (true)
		{
			/*
			 * 寻找右组的下一个下界。
			 */
			while (i1 < nentries &&
				   float8_eq(rightLower, intervalsLower[i1].lower))
			{
				if (float8_lt(leftUpper, intervalsLower[i1].upper))
					leftUpper = intervalsLower[i1].upper;
				i1++;
			}
			if (i1 >= nentries)
				break;
			rightLower = intervalsLower[i1].lower;

			/*
			 * 统计无论如何都应放入左组的
			 * 区间个数。
			 */
			while (i2 < nentries &&
				   float8_le(intervalsUpper[i2].upper, leftUpper))
				i2++;

			/*
			 * 考虑找到的分裂。
			 */
			g_box_consider_split(&context, dim, rightLower, i1, leftUpper, i2);
		}

		/*
		 * 遍历左组上界，寻找右组尽可能大的
		 * 下界。
		 */
		i1 = nentries - 1;
		i2 = nentries - 1;
		rightLower = intervalsLower[i1].upper;
		leftUpper = intervalsUpper[i2].upper;
		while (true)
		{
			/*
			 * 寻找左组的下一个上界。
			 */
			while (i2 >= 0 && float8_eq(leftUpper, intervalsUpper[i2].upper))
			{
				if (float8_gt(rightLower, intervalsUpper[i2].lower))
					rightLower = intervalsUpper[i2].lower;
				i2--;
			}
			if (i2 < 0)
				break;
			leftUpper = intervalsUpper[i2].upper;

			/*
			 * 统计无论如何都应放入右组的
			 * 区间个数。
			 */
			while (i1 >= 0 && float8_ge(intervalsLower[i1].lower, rightLower))
				i1--;

			/*
			 * 考虑找到的分裂。
			 */
			g_box_consider_split(&context, dim,
								 rightLower, i1 + 1, leftUpper, i2 + 1);
		}
	}

	/*
	 * 若未能找到任何可接受的分裂，则使用平凡分裂。
	 */
	if (context.first)
	{
		fallbackSplit(entryvec, v);
		PG_RETURN_POINTER(v);
	}

	/*
	 * 好了，现已沿某一轴选定了分裂。
	 *
	 * 考虑分裂时我们已确定两组都会有足够的项以达到
	 * 期望比例，但并未记下哪些项进哪一组。所以现在
	 * 来确定这一点。
	 */

	/* 为结果分配向量 */
	v->spl_left = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/* 为左、右组分配边界框 */
	leftBox = palloc0(sizeof(BOX));
	rightBox = palloc0(sizeof(BOX));

	/*
	 * 为"公共项"分配数组——即可放入任一组而不影响
	 * 所选轴上重叠的项。
	 */
	commonEntriesCount = 0;
	commonEntries = (CommonEntry *) palloc(nentries * sizeof(CommonEntry));

	/* 把项放入左组或右组的辅助宏 */
#define PLACE_LEFT(box, off)					\
	do {										\
		if (v->spl_nleft > 0)					\
			adjustBox(leftBox, box);			\
		else									\
			*leftBox = *(box);					\
		v->spl_left[v->spl_nleft++] = off;		\
	} while(0)

#define PLACE_RIGHT(box, off)					\
	do {										\
		if (v->spl_nright > 0)					\
			adjustBox(rightBox, box);			\
		else									\
			*rightBox = *(box);					\
		v->spl_right[v->spl_nright++] = off;	\
	} while(0)

	/*
	 * 分配可明确归组的项，并收集
	 * 公共项。
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		float8		lower,
					upper;

		/*
		 * 沿所选轴取得上、下界。
		 */
		box = DatumGetBoxP(entryvec->vector[i].key);
		if (context.dim == 0)
		{
			lower = box->low.x;
			upper = box->high.x;
		}
		else
		{
			lower = box->low.y;
			upper = box->high.y;
		}

		if (float8_le(upper, context.leftUpper))
		{
			/* 适合左组 */
			if (float8_ge(lower, context.rightLower))
			{
				/* 也适合右组，故为"公共项" */
				commonEntries[commonEntriesCount++].index = i;
			}
			else
			{
				/* 不适合右组，因此加入左组 */
				PLACE_LEFT(box, i);
			}
		}
		else
		{
			/*
			 * 每个项都应适合左组或右组之一。既然此项
			 * 不适合左组，就应当适合
			 * 右组。
			 */
			Assert(float8_ge(lower, context.rightLower));

			/* 不适合左组，因此加入右组 */
			PLACE_RIGHT(box, i);
		}
	}

	/*
	 * 若有"公共项"则分配之。
	 */
	if (commonEntriesCount > 0)
	{
		/*
		 * 计算为达到 LIMIT_RATIO，两组各自必须放入的
		 * 最少项数。
		 */
		int			m = ceil(LIMIT_RATIO * nentries);

		/*
		 * 计算把"公共项"加入不同组时惩罚
		 * 之差。
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetBoxP(entryvec->vector[commonEntries[i].index].key);
			commonEntries[i].delta = fabs(float8_mi(box_penalty(leftBox, box),
													box_penalty(rightBox, box)));
		}

		/*
		 * 按算出的 delta 对"公共项"排序，以便先分配
		 * 最含糊的项。
		 */
		qsort(commonEntries, commonEntriesCount, sizeof(CommonEntry), common_entry_cmp);

		/*
		 * 在两组之间分配"公共项"。
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetBoxP(entryvec->vector[commonEntries[i].index].key);

			/*
			 * 检查是否必须把此项放入某一组才能达到
			 * LIMIT_RATIO。
			 */
			if (v->spl_nleft + (commonEntriesCount - i) <= m)
				PLACE_LEFT(box, commonEntries[i].index);
			else if (v->spl_nright + (commonEntriesCount - i) <= m)
				PLACE_RIGHT(box, commonEntries[i].index);
			else
			{
				/* 否则按最小惩罚选择组 */
				if (box_penalty(leftBox, box) < box_penalty(rightBox, box))
					PLACE_LEFT(box, commonEntries[i].index);
				else
					PLACE_RIGHT(box, commonEntries[i].index);
			}
		}
	}

	v->spl_ldatum = PointerGetDatum(leftBox);
	v->spl_rdatum = PointerGetDatum(rightBox);
	PG_RETURN_POINTER(v);
}

/*
 * 相等方法
 *
 * 盒子、点、圆和多边形都使用此方法；它们都以
 * 盒子作为 GiST 索引项存储。
 *
 * 仅当盒子完全相同时返回 true。这里不能用模糊
 * 比较，否则会破坏索引一致性；因此这并不等价于
 * box_same()。
 */
Datum
gist_box_same(PG_FUNCTION_ARGS)
{
	BOX		   *b1 = PG_GETARG_BOX_P(0);
	BOX		   *b2 = PG_GETARG_BOX_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = (float8_eq(b1->low.x, b2->low.x) &&
				   float8_eq(b1->low.y, b2->low.y) &&
				   float8_eq(b1->high.x, b2->high.x) &&
				   float8_eq(b1->high.y, b2->high.y));
	else
		*result = (b1 == NULL && b2 == NULL);
	PG_RETURN_POINTER(result);
}

/*
 * 盒子的叶级一致性：直接应用查询操作符
 */
static bool
gist_box_leaf_consistent(BOX *key, BOX *query, StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_left,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverLeftStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overleft,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overright,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTRightStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_right,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_same,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contained,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverBelowStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overbelow,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTBelowStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_below,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTAboveStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_above,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverAboveStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overabove,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* 让编译器保持安静 */
			break;
	}
	return retval;
}

/*****************************************
 * 公共 rtree 函数（用于盒子、多边形和圆）
 *****************************************/

/*
 * 这些类型的内部页一致性
 *
 * 可用同一函数，因为各类型都以边界框作为
 * 内部页表示。
 */
static bool
rtree_internal_consistent(BOX *key, BOX *query, StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overright,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverLeftStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_right,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverlapStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverRightStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_left,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTRightStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overleft,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_contain,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTContainedByStrategyNumber:
			retval = DatumGetBool(DirectFunctionCall2(box_overlap,
													  PointerGetDatum(key),
													  PointerGetDatum(query)));
			break;
		case RTOverBelowStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_above,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTBelowStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overabove,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTAboveStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_overbelow,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		case RTOverAboveStrategyNumber:
			retval = !DatumGetBool(DirectFunctionCall2(box_below,
													   PointerGetDatum(key),
													   PointerGetDatum(query)));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* 让编译器保持安静 */
			break;
	}
	return retval;
}

/**************************************************
 * 多边形操作
 **************************************************/

/*
 * 多边形的 GiST 压缩：用其边界框表示多边形
 */
Datum
gist_poly_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		POLYGON    *in = DatumGetPolygonP(entry->key);
		BOX		   *r;

		r = (BOX *) palloc(sizeof(BOX));
		memcpy(r, &(in->boundbox), sizeof(BOX));

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * 多边形的 GiST 一致性方法
 */
Datum
gist_poly_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	POLYGON    *query = PG_GETARG_POLYGON_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		result;

	/* 本函数处理的所有情形都是不精确的 */
	*recheck = true;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * 既然操作符反正需要重检，即使在叶节点也可直接用
	 * rtree_internal_consistent。（这之所以可行，部分
	 * 是因为索引项是边界框而非多边形。）
	 */
	result = rtree_internal_consistent(DatumGetBoxP(entry->key),
									   &(query->boundbox), strategy);

	/* 若传入的多边形已 toast，避免内存泄漏 */
	PG_FREE_IF_COPY(query, 1);

	PG_RETURN_BOOL(result);
}

/**************************************************
 * 圆操作
 **************************************************/

/*
 * 圆的 GiST 压缩：用其边界框表示圆
 */
Datum
gist_circle_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		CIRCLE	   *in = DatumGetCircleP(entry->key);
		BOX		   *r;

		r = (BOX *) palloc(sizeof(BOX));
		r->high.x = float8_pl(in->center.x, in->radius);
		r->low.x = float8_mi(in->center.x, in->radius);
		r->high.y = float8_pl(in->center.y, in->radius);
		r->low.y = float8_mi(in->center.y, in->radius);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * 圆的 GiST 一致性方法
 */
Datum
gist_circle_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	CIRCLE	   *query = PG_GETARG_CIRCLE_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	BOX			bbox;
	bool		result;

	/* 本函数处理的所有情形都是不精确的 */
	*recheck = true;

	if (DatumGetBoxP(entry->key) == NULL || query == NULL)
		PG_RETURN_BOOL(false);

	/*
	 * 既然操作符反正需要重检，即使在叶节点也可直接用
	 * rtree_internal_consistent。（这之所以可行，部分
	 * 是因为索引项是边界框而非圆。）
	 */
	bbox.high.x = float8_pl(query->center.x, query->radius);
	bbox.low.x = float8_mi(query->center.x, query->radius);
	bbox.high.y = float8_pl(query->center.y, query->radius);
	bbox.low.y = float8_mi(query->center.y, query->radius);

	result = rtree_internal_consistent(DatumGetBoxP(entry->key),
									   &bbox, strategy);

	PG_RETURN_BOOL(result);
}

/**************************************************
 * 点操作
 **************************************************/

Datum
gist_point_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	if (entry->leafkey)			/* 实际上是点 */
	{
		BOX		   *box = palloc(sizeof(BOX));
		Point	   *point = DatumGetPointP(entry->key);
		GISTENTRY  *retval = palloc(sizeof(GISTENTRY));

		box->high = box->low = *point;

		gistentryinit(*retval, BoxPGetDatum(box),
					  entry->rel, entry->page, entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

/*
 * 点的 GiST 取回方法
 *
 * 从边界框坐标取得点坐标并形成新的
 * gistentry。
 */
Datum
gist_point_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	BOX		   *in = DatumGetBoxP(entry->key);
	Point	   *r;
	GISTENTRY  *retval;

	retval = palloc(sizeof(GISTENTRY));

	r = (Point *) palloc(sizeof(Point));
	r->x = in->high.x;
	r->y = in->high.y;
	gistentryinit(*retval, PointerGetDatum(r),
				  entry->rel, entry->page,
				  entry->offset, false);

	PG_RETURN_POINTER(retval);
}


#define point_point_distance(p1,p2) \
	DatumGetFloat8(DirectFunctionCall2(point_distance, \
									   PointPGetDatum(p1), PointPGetDatum(p2)))

static float8
computeDistance(bool isLeaf, BOX *box, Point *point)
{
	float8		result = 0.0;

	if (isLeaf)
	{
		/* 简单的点到点距离 */
		result = point_point_distance(point, &box->low);
	}
	else if (point->x <= box->high.x && point->x >= box->low.x &&
			 point->y <= box->high.y && point->y >= box->low.y)
	{
		/* 点在盒子内部 */
		result = 0.0;
	}
	else if (point->x <= box->high.x && point->x >= box->low.x)
	{
		/* 点在盒子上方或下方 */
		Assert(box->low.y <= box->high.y);
		if (point->y > box->high.y)
			result = float8_mi(point->y, box->high.y);
		else if (point->y < box->low.y)
			result = float8_mi(box->low.y, point->y);
		else
			elog(ERROR, "inconsistent point values");
	}
	else if (point->y <= box->high.y && point->y >= box->low.y)
	{
		/* 点在盒子左侧或右侧 */
		Assert(box->low.x <= box->high.x);
		if (point->x > box->high.x)
			result = float8_mi(point->x, box->high.x);
		else if (point->x < box->low.x)
			result = float8_mi(box->low.x, point->x);
		else
			elog(ERROR, "inconsistent point values");
	}
	else
	{
		/* 最近点将是一个顶点 */
		Point		p;
		float8		subresult;

		result = point_point_distance(point, &box->low);

		subresult = point_point_distance(point, &box->high);
		if (result > subresult)
			result = subresult;

		p.x = box->low.x;
		p.y = box->high.y;
		subresult = point_point_distance(point, &p);
		if (result > subresult)
			result = subresult;

		p.x = box->high.x;
		p.y = box->low.y;
		subresult = point_point_distance(point, &p);
		if (result > subresult)
			result = subresult;
	}

	return result;
}

static bool
gist_point_consistent_internal(StrategyNumber strategy,
							   bool isLeaf, BOX *key, Point *query)
{
	bool		result = false;

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			result = FPlt(key->low.x, query->x);
			break;
		case RTRightStrategyNumber:
			result = FPgt(key->high.x, query->x);
			break;
		case RTAboveStrategyNumber:
			result = FPgt(key->high.y, query->y);
			break;
		case RTBelowStrategyNumber:
			result = FPlt(key->low.y, query->y);
			break;
		case RTSameStrategyNumber:
			if (isLeaf)
			{
				/* key.high 必等于 key.low，故可忽略它 */
				result = (FPeq(key->low.x, query->x) &&
						  FPeq(key->low.y, query->y));
			}
			else
			{
				result = (FPle(query->x, key->high.x) &&
						  FPge(query->x, key->low.x) &&
						  FPle(query->y, key->high.y) &&
						  FPge(query->y, key->low.y));
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			result = false;		/* 让编译器保持安静 */
			break;
	}

	return result;
}

#define GeoStrategyNumberOffset		20
#define PointStrategyNumberGroup	0
#define BoxStrategyNumberGroup		1
#define PolygonStrategyNumberGroup	2
#define CircleStrategyNumberGroup	3

Datum
gist_point_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		result;
	StrategyNumber strategyGroup;

	/*
	 * 必须重新映射这些策略号，才能让这套笨拙的
	 * 分类逻辑工作。
	 */
	if (strategy == RTOldBelowStrategyNumber)
		strategy = RTBelowStrategyNumber;
	else if (strategy == RTOldAboveStrategyNumber)
		strategy = RTAboveStrategyNumber;

	strategyGroup = strategy / GeoStrategyNumberOffset;
	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			result = gist_point_consistent_internal(strategy % GeoStrategyNumberOffset,
													GIST_LEAF(entry),
													DatumGetBoxP(entry->key),
													PG_GETARG_POINT_P(1));
			*recheck = false;
			break;
		case BoxStrategyNumberGroup:
			{
				/*
				 * 本组唯一操作符是 point <@ box (on_pb)，因此
				 * 不必再检查 strategy。
				 *
				 * 由于历史原因，on_pb 使用精确而非模糊
				 * 比较。在内部页本可用 box_overlap，但那会
				 * 导致可能无谓访问子页，因为 box_overlap
				 * 使用模糊比较。于是我们改写非模糊重叠
				 * 测试。同一代码也适用于叶页测试，因为
				 * 叶键满足 high == low。
				 * 这样可避免因模糊比较而多余地下钻。
				 */
				BOX		   *query,
						   *key;

				query = PG_GETARG_BOX_P(1);
				key = DatumGetBoxP(entry->key);

				result = (key->high.x >= query->low.x &&
						  key->low.x <= query->high.x &&
						  key->high.y >= query->low.y &&
						  key->low.y <= query->high.y);
				*recheck = false;
			}
			break;
		case PolygonStrategyNumberGroup:
			{
				POLYGON    *query = PG_GETARG_POLYGON_P(1);

				result = DatumGetBool(DirectFunctionCall5(gist_poly_consistent,
														  PointerGetDatum(entry),
														  PolygonPGetDatum(query),
														  Int16GetDatum(RTOverlapStrategyNumber),
														  0, PointerGetDatum(recheck)));

				if (GIST_LEAF(entry) && result)
				{
					/*
					 * 我们在叶页上，且快速检查显示多边形的
					 * 边界框与点重叠
					 */
					BOX		   *box = DatumGetBoxP(entry->key);

					Assert(box->high.x == box->low.x
						   && box->high.y == box->low.y);
					result = DatumGetBool(DirectFunctionCall2(poly_contain_pt,
															  PolygonPGetDatum(query),
															  PointPGetDatum(&box->high)));
					*recheck = false;
				}
			}
			break;
		case CircleStrategyNumberGroup:
			{
				CIRCLE	   *query = PG_GETARG_CIRCLE_P(1);

				result = DatumGetBool(DirectFunctionCall5(gist_circle_consistent,
														  PointerGetDatum(entry),
														  CirclePGetDatum(query),
														  Int16GetDatum(RTOverlapStrategyNumber),
														  0, PointerGetDatum(recheck)));

				if (GIST_LEAF(entry) && result)
				{
					/*
					 * 我们在叶页上，且快速检查显示多边形的
					 * 边界框与点重叠
					 */
					BOX		   *box = DatumGetBoxP(entry->key);

					Assert(box->high.x == box->low.x
						   && box->high.y == box->low.y);
					result = DatumGetBool(DirectFunctionCall2(circle_contain_pt,
															  CirclePGetDatum(query),
															  PointPGetDatum(&box->high)));
					*recheck = false;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			result = false;		/* 让编译器保持安静 */
			break;
	}

	PG_RETURN_BOOL(result);
}

Datum
gist_point_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	float8		distance;
	StrategyNumber strategyGroup = strategy / GeoStrategyNumberOffset;

	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			distance = computeDistance(GIST_LEAF(entry),
									   DatumGetBoxP(entry->key),
									   PG_GETARG_POINT_P(1));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			distance = 0.0;		/* 让编译器保持安静 */
			break;
	}

	PG_RETURN_FLOAT8(distance);
}

static float8
gist_bbox_distance(GISTENTRY *entry, Datum query, StrategyNumber strategy)
{
	float8		distance;
	StrategyNumber strategyGroup = strategy / GeoStrategyNumberOffset;

	switch (strategyGroup)
	{
		case PointStrategyNumberGroup:
			distance = computeDistance(false,
									   DatumGetBoxP(entry->key),
									   DatumGetPointP(query));
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			distance = 0.0;		/* 让编译器保持安静 */
	}

	return distance;
}

Datum
gist_box_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	/* bool	   *recheck = (bool *) PG_GETARG_POINTER(4); */
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);

	PG_RETURN_FLOAT8(distance);
}

/*
 * 以边界框存储的几何类型的不精确 GiST 距离
 * 方法。
 *
 * 计算点到索引项的有损距离。结果不精确，因为
 * 索引项是边界框，而非被索引几何类型的确切
 * 形状。我们使用点到索引项 MBR 的距离。这是
 * 点到被索引几何类型距离的下界估计。
 * 因此可用于排序与剪枝，但不是最终精确距离。
 */
Datum
gist_circle_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);
	*recheck = true;

	PG_RETURN_FLOAT8(distance);
}

Datum
gist_poly_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	float8		distance;

	distance = gist_bbox_distance(entry, query, strategy);
	*recheck = true;

	PG_RETURN_FLOAT8(distance);
}

/*
 * 用于快速索引构建的 Z 序例程
 */

/*
 * 计算点的 Z 值
 *
 * Z 序（亦称 Morton 码）把二维点映射为单个
 * 整数，并保持局部性。二维空间中相近的点会
 * 被映射为彼此不太远的整数。做法是交错 X、
 * Y 分量的各位。
 *
 * Morton 码通常只对整数定义，但点的 X、Y
 * 值是浮点数。我们期望浮点数为 IEEE 格式。
 */
static uint64
point_zorder_internal(float4 x, float4 y)
{
	uint32		ix = ieee_float32_to_uint32(x);
	uint32		iy = ieee_float32_to_uint32(y);

	/* 交错各位 */
	return part_bits32_by2(ix) | (part_bits32_by2(iy) << 1);
}

/* 用零交错 32 位 */
static uint64
part_bits32_by2(uint32 x)
{
	uint64		n = x;

	n = (n | (n << 16)) & UINT64CONST(0x0000FFFF0000FFFF);
	n = (n | (n << 8)) & UINT64CONST(0x00FF00FF00FF00FF);
	n = (n | (n << 4)) & UINT64CONST(0x0F0F0F0F0F0F0F0F);
	n = (n | (n << 2)) & UINT64CONST(0x3333333333333333);
	n = (n | (n << 1)) & UINT64CONST(0x5555555555555555);

	return n;
}

/*
 * 把 32 位 IEEE 浮点转换为保持序关系的 uint32
 */
static uint32
ieee_float32_to_uint32(float f)
{
	/*----
	 *
	 * IEEE 754 浮点格式
	 * ------------------------------
	 *
	 * IEEE 754 浮点数具有如下格式：
	 *
	 *   指数（8 位）
	 *   |
	 * s eeeeeeee mmmmmmmmmmmmmmmmmmmmmmm
	 * |          |
	 * 符号       尾数（23 位）
	 *
	 * 无穷的指数各位全置位且尾数全为
	 * 零。负无穷相同但符号位被置位。
	 *
	 * NaN 表示为指数各位全置位，且尾数最低
	 * 有效位也置位。尾数其余位可用于区分不同
	 * 种类的 NaNs。
	 *
	 * IEEE 格式有一个优良性质：把位表示解释
	 * 为整数时序关系得以保持，符号除外。对
	 * +-Infinity 值同样成立。
	 *
	 * 映射到 uint32
	 * -----------------
	 *
	 * 为使负数到正数过渡平滑，我们把浮点映射
	 * 到无符号整数如下：
	 *
	 * x < 0 映射到范围 0-7FFFFFFF
	 * x = 0 映射到值 8000000（正零与负零皆然）
	 * x > 0 映射到范围 8000001-FFFFFFFF
	 *
	 * 我们不区分不同种类的 NaN，因此全部映射
	 * 到同一任意值 FFFFFFFF。由于 NaN 的 IEEE
	 * 位表示，没有任何非 NaN 值会映射到
	 * FFFFFFFF。实际上 uint32 空间两端都有一段
	 * 未使用的值。
	 */
	if (isnan(f))
		return 0xFFFFFFFF;
	else
	{
		union
		{
			float		f;
			uint32		i;
		}			u;

		u.f = f;

		/* 检查符号位 */
		if ((u.i & 0x80000000) != 0)
		{
			/*
			 * 把负值映射到范围 0-7FFFFFFF。同一条指令
			 * 也把符号位翻转为 0。
			 */
			Assert(f <= 0);		/* 可为 -0 */
			u.i ^= 0xFFFFFFFF;
		}
		else
		{
			/* 把正值（或 0）映射到范围 80000000-FFFFFFFF */
			u.i |= 0x80000000;
		}

		return u.i;
	}
}

/*
 * 比较点的 Z 序
 */
static int
gist_bbox_zorder_cmp(Datum a, Datum b, SortSupport ssup)
{
	Point	   *p1 = &(DatumGetBoxP(a)->low);
	Point	   *p2 = &(DatumGetBoxP(b)->low);
	uint64		z1;
	uint64		z2;

	/*
	 * 先快速检查相等。一般是否值得尚不清楚，但在
	 * 用作缩写键的决胜比较时肯定
	 * 值得，
	 */
	if (p1->x == p2->x && p1->y == p2->y)
		return 0;

	z1 = point_zorder_internal(p1->x, p1->y);
	z2 = point_zorder_internal(p2->x, p2->y);
	if (z1 > z2)
		return 1;
	else if (z1 < z2)
		return -1;
	else
		return 0;
}

/*
 * Z 序比较的缩写版本
 *
 * 缩写格式是由两个 32 位浮点算出的 Z 序值。若
 * SIZEOF_DATUM == 8，64 位 Z 序值可完整放入缩写
 * Datum，否则使用其最高有效位。
 */
static Datum
gist_bbox_zorder_abbrev_convert(Datum original, SortSupport ssup)
{
	Point	   *p = &(DatumGetBoxP(original)->low);
	uint64		z;

	z = point_zorder_internal(p->x, p->y);

#if SIZEOF_DATUM == 8
	return (Datum) z;
#else
	return (Datum) (z >> 32);
#endif
}

/*
 * 我们从不考虑中止缩写。
 *
 * 在 64 位系统上缩写无损，因此总是值得。（在
 * 32 位系统上或许不值得，但我们懒得写判定
 * 逻辑。）
 */
static bool
gist_bbox_zorder_abbrev_abort(int memtupcount, SortSupport ssup)
{
	return false;
}

/*
 * 通过排序实现快速 GiST 索引构建的排序支持例程。
 */
Datum
gist_point_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	if (ssup->abbreviate)
	{
		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = gist_bbox_zorder_abbrev_convert;
		ssup->abbrev_abort = gist_bbox_zorder_abbrev_abort;
		ssup->abbrev_full_comparator = gist_bbox_zorder_cmp;
	}
	else
	{
		ssup->comparator = gist_bbox_zorder_cmp;
	}
	PG_RETURN_VOID();
}
