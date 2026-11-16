/*-------------------------------------------------------------------------
 *
 * rangetypes_gist.c
 *	  范围类型的 GiST 支持
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes_gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/multirangetypes.h"
#include "utils/rangetypes.h"

/*
 * 用于在 GiST 中区分不同范围类别的范围类别属性。
 * 每种唯一的属性组合即为一个类。CLS_EMPTY 无法与其
 * 任何其他内容组合。
 */
#define CLS_NORMAL			0	/* 普通有限范围（未置位） */
#define CLS_LOWER_INF		1	/* 下边界为无穷 */
#define CLS_UPPER_INF		2	/* 上边界为无穷 */
#define CLS_CONTAIN_EMPTY	4	/* 包含底层空范围 */
#define CLS_EMPTY			8	/* 空范围的专用类 */

#define CLS_COUNT			9	/* 类的数量；包含属性的组合。
								 * CLS_EMPTY 不与其他组合，仅有
								 * 2^3 + 1。 */

/*
 * 同类条目可接受的最小拆分比例。若条目属不同类，
 * 则无论比例如何都会沿这些界线分离，而不考虑
 * 比例。
 */
#define LIMIT_RATIO  0.3

/* 固定惩罚值的常量 */
#define INFINITE_BOUND_PENALTY	2.0
#define CONTAIN_EMPTY_PENALTY  1.0
#define DEFAULT_SUBTYPE_DIFF_PENALTY  1.0

/*
 * range_gist_single_sorting_split 的每条目数据
 */
typedef struct
{
	int			index;
	RangeBound	bound;
} SingleBoundSortItem;

/* 放在拆分的左侧还是右侧？ */
typedef enum
{
	SPLIT_LEFT = 0,				/* 便于初始化为 SPLIT_LEFT */
	SPLIT_RIGHT
} SplitLR;

/*
 * range_gist_consider_split 的上下文
 */
typedef struct
{
	TypeCacheEntry *typcache;	/* 范围的 typcache */
	bool		has_subtype_diff;	/* 有 subtype_diff？ */
	int			entries_count;	/* 被拆分条目总数 */

	/* 以下是当前选定拆分的相关信息 */

	bool		first;			/* 若尚未选定拆分则为真 */

	RangeBound *left_upper;		/* 左区间的上边界 */
	RangeBound *right_lower;	/* 右区间的下边界 */

	float4		ratio;			/* 比例 */
	float4		overlap;		/* 左右谓词间的重叠 */
	int			common_left;	/* 分到每侧的公共条目数 */
	int			common_right;
} ConsiderSplitContext;

/*
 * 从非空范围提取的边界，供函数
 * range_gist_double_sorting_split
 */
typedef struct
{
	RangeBound	lower;
	RangeBound	upper;
} NonEmptyRange;

/*
 * 表示这样的条目的信息：它可放入任一组，
 * 且不影响所选轴上的重叠（即"公共条目"）。
 */
typedef struct
{
	/* 条目在初始数组中的下标 */
	int			index;
	/* 范围与两组各自接近度之间的差值 */
	double		delta;
} CommonEntry;

/* 拆分期间把条目放入左组或右组的辅助宏 */
/* 注意会直接访问变量 v、typcache、left_range、right_range */
#define PLACE_LEFT(range, off)					\
	do {										\
		if (v->spl_nleft > 0)					\
			left_range = range_super_union(typcache, left_range, range); \
		else									\
			left_range = (range);				\
		v->spl_left[v->spl_nleft++] = (off);	\
	} while(0)

#define PLACE_RIGHT(range, off)					\
	do {										\
		if (v->spl_nright > 0)					\
			right_range = range_super_union(typcache, right_range, range); \
		else									\
			right_range = (range);				\
		v->spl_right[v->spl_nright++] = (off);	\
	} while(0)

/* 复制 RangeType datum（为范围硬编码 typbyval/typlen...） */
#define rangeCopy(r) \
	((RangeType *) DatumGetPointer(datumCopy(PointerGetDatum(r), \
											 false, -1)))

static RangeType *range_super_union(TypeCacheEntry *typcache, RangeType *r1,
									RangeType *r2);
static bool range_gist_consistent_int_range(TypeCacheEntry *typcache,
											StrategyNumber strategy,
											const RangeType *key,
											const RangeType *query);
static bool range_gist_consistent_int_multirange(TypeCacheEntry *typcache,
												 StrategyNumber strategy,
												 const RangeType *key,
												 const MultirangeType *query);
static bool range_gist_consistent_int_element(TypeCacheEntry *typcache,
											  StrategyNumber strategy,
											  const RangeType *key,
											  Datum query);
static bool range_gist_consistent_leaf_range(TypeCacheEntry *typcache,
											 StrategyNumber strategy,
											 const RangeType *key,
											 const RangeType *query);
static bool range_gist_consistent_leaf_multirange(TypeCacheEntry *typcache,
												  StrategyNumber strategy,
												  const RangeType *key,
												  const MultirangeType *query);
static bool range_gist_consistent_leaf_element(TypeCacheEntry *typcache,
											   StrategyNumber strategy,
											   const RangeType *key,
											   Datum query);
static void range_gist_fallback_split(TypeCacheEntry *typcache,
									  GistEntryVector *entryvec,
									  GIST_SPLITVEC *v);
static void range_gist_class_split(TypeCacheEntry *typcache,
								   GistEntryVector *entryvec,
								   GIST_SPLITVEC *v,
								   SplitLR *classes_groups);
static void range_gist_single_sorting_split(TypeCacheEntry *typcache,
											GistEntryVector *entryvec,
											GIST_SPLITVEC *v,
											bool use_upper_bound);
static void range_gist_double_sorting_split(TypeCacheEntry *typcache,
											GistEntryVector *entryvec,
											GIST_SPLITVEC *v);
static void range_gist_consider_split(ConsiderSplitContext *context,
									  RangeBound *right_lower, int min_left_count,
									  RangeBound *left_upper, int max_left_count);
static int	get_gist_range_class(RangeType *range);
static int	single_bound_cmp(const void *a, const void *b, void *arg);
static int	interval_cmp_lower(const void *a, const void *b, void *arg);
static int	interval_cmp_upper(const void *a, const void *b, void *arg);
static int	common_entry_cmp(const void *i1, const void *i2);
static float8 call_subtype_diff(TypeCacheEntry *typcache,
								Datum val1, Datum val2);


/* GiST 查询一致性检查 */
Datum
range_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		result;
	Oid			subtype = PG_GETARG_OID(3);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	RangeType  *key = DatumGetRangeTypeP(entry->key);
	TypeCacheEntry *typcache;

	/* 本函数服务的所有算子都是精确的 */
	*recheck = false;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(key));

	/*
	 * 使用与键类型（叶子或内部）对应的函数执行
	 * 一致性检查，查询子类型（范围、多范围或元素）
	 * 注：无效子类型即查询类型与键类型一致，即范
	 * 围。
	 */
	if (GIST_LEAF(entry))
	{
		if (!OidIsValid(subtype) || subtype == ANYRANGEOID)
			result = range_gist_consistent_leaf_range(typcache, strategy, key,
													  DatumGetRangeTypeP(query));
		else if (subtype == ANYMULTIRANGEOID)
			result = range_gist_consistent_leaf_multirange(typcache, strategy, key,
														   DatumGetMultirangeTypeP(query));
		else
			result = range_gist_consistent_leaf_element(typcache, strategy,
														key, query);
	}
	else
	{
		if (!OidIsValid(subtype) || subtype == ANYRANGEOID)
			result = range_gist_consistent_int_range(typcache, strategy, key,
													 DatumGetRangeTypeP(query));
		else if (subtype == ANYMULTIRANGEOID)
			result = range_gist_consistent_int_multirange(typcache, strategy, key,
														  DatumGetMultirangeTypeP(query));
		else
			result = range_gist_consistent_int_element(typcache, strategy,
													   key, query);
	}
	PG_RETURN_BOOL(result);
}

/*
 * 多范围 GiST compress 方法：多范围被近似为无间隙的并
 * 集范围。
 */
Datum
multirange_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	if (entry->leafkey)
	{
		MultirangeType *mr = DatumGetMultirangeTypeP(entry->key);
		RangeType  *r;
		TypeCacheEntry *typcache;
		GISTENTRY  *retval = palloc(sizeof(GISTENTRY));

		typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
		r = multirange_get_union_range(typcache->rngtype, mr);

		gistentryinit(*retval, RangeTypePGetDatum(r),
					  entry->rel, entry->page, entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

/* 多范围的 GiST 查询一致性检查 */
Datum
multirange_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		result;
	Oid			subtype = PG_GETARG_OID(3);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	RangeType  *key = DatumGetRangeTypeP(entry->key);
	TypeCacheEntry *typcache;

	/*
	 * 本函数所服务的所有算子均不精确，因为多范围
	 * 近似为无间隙的并集范围。
	 */
	*recheck = true;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(key));

	/*
	 * 使用与键类型（叶子或内部）对应的函数执行
	 * 一致性检查，查询子类型（范围、多范围或元素）
	 * 注：无效子类型即查询类型与键类型一致，即多
	 * 范围。
	 */
	if (GIST_LEAF(entry))
	{
		if (!OidIsValid(subtype) || subtype == ANYMULTIRANGEOID)
			result = range_gist_consistent_leaf_multirange(typcache, strategy, key,
														   DatumGetMultirangeTypeP(query));
		else if (subtype == ANYRANGEOID)
			result = range_gist_consistent_leaf_range(typcache, strategy, key,
													  DatumGetRangeTypeP(query));
		else
			result = range_gist_consistent_leaf_element(typcache, strategy,
														key, query);
	}
	else
	{
		if (!OidIsValid(subtype) || subtype == ANYMULTIRANGEOID)
			result = range_gist_consistent_int_multirange(typcache, strategy, key,
														  DatumGetMultirangeTypeP(query));
		else if (subtype == ANYRANGEOID)
			result = range_gist_consistent_int_range(typcache, strategy, key,
													 DatumGetRangeTypeP(query));
		else
			result = range_gist_consistent_int_element(typcache, strategy,
													   key, query);
	}
	PG_RETURN_BOOL(result);
}

/* 求并集 */
Datum
range_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GISTENTRY  *ent = entryvec->vector;
	RangeType  *result_range;
	TypeCacheEntry *typcache;
	int			i;

	result_range = DatumGetRangeTypeP(ent[0].key);

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(result_range));

	for (i = 1; i < entryvec->n; i++)
	{
		result_range = range_super_union(typcache, result_range,
										 DatumGetRangeTypeP(ent[i].key));
	}

	PG_RETURN_RANGE_P(result_range);
}

/*
 * 范围在 GiST 索引中即以范围存储，故无需
 * compress、decompress 或 fetch 函数。注意这意味着可被
 * 索引的范围值大小存在限制。
 */

/*
 * GiST 页面拆分惩罚函数。
 *
 * 惩罚函数有以下目标（按重要性从高到
 * 低）：
 * - 保持普通范围分离
 * - 避免拓宽原谓词的类
 * - 避免拓宽原谓词（由 subtype_diff 判定）
 * - 倾向于把范围加入更窄的原谓词
 */
Datum
range_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	RangeType  *orig = DatumGetRangeTypeP(origentry->key);
	RangeType  *new = DatumGetRangeTypeP(newentry->key);
	TypeCacheEntry *typcache;
	bool		has_subtype_diff;
	RangeBound	orig_lower,
				new_lower,
				orig_upper,
				new_upper;
	bool		orig_empty,
				new_empty;

	if (RangeTypeGetOid(orig) != RangeTypeGetOid(new))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(orig));

	has_subtype_diff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	range_deserialize(typcache, orig, &orig_lower, &orig_upper, &orig_empty);
	range_deserialize(typcache, new, &new_lower, &new_upper, &new_empty);

	/*
	 * 以不同分支处理不同类别的范围。注意，惩罚值
	 * 只需在同一新范围的类别内彼此
	 * 相称。
	 */
	if (new_empty)
	{
		/* 处理空范围的插入 */
		if (orig_empty)
		{
			/*
			 * 最佳情形是将其插入空的原范围。
			 * 在此处插入意味着原范围不拓宽。而且
			 * 此时原范围最窄。
			 */
			*penalty = 0.0;
		}
		else if (RangeIsOrContainsEmpty(orig))
		{
			/*
			 * 第二种情形是把空范围插入到
			 * 至少含有一个底层空范围的范围中。此时仍不
			 * 拓宽原范围，但原范围已不是
			 * 最窄的了。
			 */
			*penalty = CONTAIN_EMPTY_PENALTY;
		}
		else if (orig_lower.infinite && orig_upper.infinite)
		{
			/*
			 * 原范围需要拓宽。此时 (-inf; +inf) 距普通范围
			 * 最远。
			 */
			*penalty = 2 * CONTAIN_EMPTY_PENALTY;
		}
		else if (orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * (-inf, x) 或 (x, +inf) 的原范围更接近普通
			 * 范围，故与空范围相混更糟。
			 */
			*penalty = 3 * CONTAIN_EMPTY_PENALTY;
		}
		else
		{
			/*
			 * 最不合意的情形是拓宽普通范围。
			 */
			*penalty = 4 * CONTAIN_EMPTY_PENALTY;
		}
	}
	else if (new_lower.infinite && new_upper.infinite)
	{
		/* 处理 (-inf, +inf) 范围的插入 */
		if (orig_lower.infinite && orig_upper.infinite)
		{
			/*
			 * 最佳情形是插入到 (-inf, +inf) 原范围。
			 */
			*penalty = 0.0;
		}
		else if (orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * 当原范围是 (-inf, x) 或 (x, +inf) 时需要
			 * 拓宽原范围（把一个边界延伸至无
			 * 穷）。
			 */
			*penalty = INFINITE_BOUND_PENALTY;
		}
		else
		{
			/*
			 * 插入普通原范围是最不合意的。
			 */
			*penalty = 2 * INFINITE_BOUND_PENALTY;
		}

		if (RangeIsOrContainsEmpty(orig))
		{
			/*
			 * 不含空范围时原范围更窄。否则应附加
			 * 额外惩罚。
			 */
			*penalty += CONTAIN_EMPTY_PENALTY;
		}
	}
	else if (new_lower.infinite)
	{
		/* 处理 (-inf, x) 范围的插入 */
		if (!orig_empty && orig_lower.infinite)
		{
			if (orig_upper.infinite)
			{
				/*
				 * 插入 (-inf, x) 范围不会使 (-inf, +inf)
				 * 范围扩展。比插入到不扩展的 (-inf, y)
				 * 原范围的情形更不合意，因为在那
				 * 种情形下原范围更窄。但我们无法用单个
				 * 浮点值表达它。
				 */
				*penalty = 0.0;
			}
			else
			{
				if (range_cmp_bounds(typcache, &new_upper, &orig_upper) > 0)
				{
					/*
					 * 用 subtype_diff 求原范围的扩展量。若无
					 * subtype_diff 可用则用常量。
					 */
					if (has_subtype_diff)
						*penalty = call_subtype_diff(typcache,
													 new_upper.val,
													 orig_upper.val);
					else
						*penalty = DEFAULT_SUBTYPE_DIFF_PENALTY;
				}
				else
				{
					/* 原范围无扩展 */
					*penalty = 0.0;
				}
			}
		}
		else
		{
			/*
			 * 若原范围的下边界不是 -inf，则其扩展量就是
			 * 无穷大。
			 */
			*penalty = get_float4_infinity();
		}
	}
	else if (new_upper.infinite)
	{
		/* 处理 (x, +inf) 范围的插入 */
		if (!orig_empty && orig_upper.infinite)
		{
			if (orig_lower.infinite)
			{
				/*
				 * 插入 (x, +inf) 范围不会使 (-inf, +inf)
				 * 范围扩展。比插入到不扩展的 (y, +inf)
				 * 原范围的情形更不合意，因为在那
				 * 种情形下原范围更窄。但我们无法用单个
				 * 浮点值表达它。
				 */
				*penalty = 0.0;
			}
			else
			{
				if (range_cmp_bounds(typcache, &new_lower, &orig_lower) < 0)
				{
					/*
					 * 用 subtype_diff 求原范围的扩展量。若无
					 * subtype_diff 可用则用常量。
					 */
					if (has_subtype_diff)
						*penalty = call_subtype_diff(typcache,
													 orig_lower.val,
													 new_lower.val);
					else
						*penalty = DEFAULT_SUBTYPE_DIFF_PENALTY;
				}
				else
				{
					/* 原范围无扩展 */
					*penalty = 0.0;
				}
			}
		}
		else
		{
			/*
			 * 若原范围的上边界不是 +inf，则其扩展量就是
			 * 无穷大。
			 */
			*penalty = get_float4_infinity();
		}
	}
	else
	{
		/* 处理普通（非空、非无限）范围的插入 */
		if (orig_empty || orig_lower.infinite || orig_upper.infinite)
		{
			/*
			 * 避免将普通范围与无限及空范围相混。
			 */
			*penalty = get_float4_infinity();
		}
		else
		{
			/*
			 * 通过调用 subtype_diff 计算原范围的扩展量。
			 * 若 subtype_diff 不可用则用常量。
			 */
			float8		diff = 0.0;

			if (range_cmp_bounds(typcache, &new_lower, &orig_lower) < 0)
			{
				if (has_subtype_diff)
					diff += call_subtype_diff(typcache,
											  orig_lower.val,
											  new_lower.val);
				else
					diff += DEFAULT_SUBTYPE_DIFF_PENALTY;
			}
			if (range_cmp_bounds(typcache, &new_upper, &orig_upper) > 0)
			{
				if (has_subtype_diff)
					diff += call_subtype_diff(typcache,
											  new_upper.val,
											  orig_upper.val);
				else
					diff += DEFAULT_SUBTYPE_DIFF_PENALTY;
			}
			*penalty = diff;
		}
	}

	PG_RETURN_POINTER(penalty);
}

/*
 * 范围的 GiST PickSplit 方法
 *
 * 首要原则是尽量把不同类别的范围分开。若拆分
 * 同类范围，则对该类使用相应的拆分方法。
 */
Datum
range_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	TypeCacheEntry *typcache;
	OffsetNumber i;
	RangeType  *pred_left;
	int			nbytes;
	OffsetNumber maxoff;
	int			count_in_classes[CLS_COUNT];
	int			j;
	int			non_empty_classes_count = 0;
	int			biggest_class = -1;
	int			biggest_class_count = 0;
	int			total_count;

	/* 用首个条目查找范围类型的信息 */
	pred_left = DatumGetRangeTypeP(entryvec->vector[FirstOffsetNumber].key);
	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(pred_left));

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	/*
	 * 获取范围类别的数量分布。
	 */
	memset(count_in_classes, 0, sizeof(count_in_classes));
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[i].key);

		count_in_classes[get_gist_range_class(range)]++;
	}

	/*
	 * 统计非空类并找出最大的类。
	 */
	total_count = maxoff;
	for (j = 0; j < CLS_COUNT; j++)
	{
		if (count_in_classes[j] > 0)
		{
			if (count_in_classes[j] > biggest_class_count)
			{
				biggest_class_count = count_in_classes[j];
				biggest_class = j;
			}
			non_empty_classes_count++;
		}
	}

	Assert(non_empty_classes_count > 0);

	if (non_empty_classes_count == 1)
	{
		/* 只有一个非空类，故在类内拆分 */
		if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_NORMAL)
		{
			/* 普通范围用双重排序拆分 */
			range_gist_double_sorting_split(typcache, entryvec, v);
		}
		else if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_LOWER_INF)
		{
			/* (-inf, x) 范围用上边界排序拆分 */
			range_gist_single_sorting_split(typcache, entryvec, v, true);
		}
		else if ((biggest_class & ~CLS_CONTAIN_EMPTY) == CLS_UPPER_INF)
		{
			/* (x, +inf) 范围用下边界排序拆分 */
			range_gist_single_sorting_split(typcache, entryvec, v, false);
		}
		else
		{
			/* 全为 (-inf, +inf) 或全空范围时平凡拆分 */
			range_gist_fallback_split(typcache, entryvec, v);
		}
	}
	else
	{
		/*
		 * 按类别拆分。
		 *
		 * 每个类应分到拆分的哪一侧？先把它们
		 * 全部初始化为左侧。
		 */
		SplitLR		classes_groups[CLS_COUNT];

		memset(classes_groups, 0, sizeof(classes_groups));

		if (count_in_classes[CLS_NORMAL] > 0)
		{
			/* 若有普通范围则分开 */
			classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
		}
		else
		{
			/*----------
			 * 尝试按两种方式之一拆分：
			 *	1) 含无穷 - 不含无穷
			 *	2) 含空 - 不含空
			 *
			 * 选择在左右之间最均衡分布范围的方式。若
			 * 无法按这些方式拆分，则类别最多只有 3 个，
			 * 此时只需分离最大的类。
			 *----------
			 */
			int			infCount,
						nonInfCount;
			int			emptyCount,
						nonEmptyCount;

			nonInfCount =
				count_in_classes[CLS_NORMAL] +
				count_in_classes[CLS_CONTAIN_EMPTY] +
				count_in_classes[CLS_EMPTY];
			infCount = total_count - nonInfCount;

			nonEmptyCount =
				count_in_classes[CLS_NORMAL] +
				count_in_classes[CLS_LOWER_INF] +
				count_in_classes[CLS_UPPER_INF] +
				count_in_classes[CLS_LOWER_INF | CLS_UPPER_INF];
			emptyCount = total_count - nonEmptyCount;

			if (infCount > 0 && nonInfCount > 0 &&
				(abs(infCount - nonInfCount) <=
				 abs(emptyCount - nonEmptyCount)))
			{
				classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
				classes_groups[CLS_CONTAIN_EMPTY] = SPLIT_RIGHT;
				classes_groups[CLS_EMPTY] = SPLIT_RIGHT;
			}
			else if (emptyCount > 0 && nonEmptyCount > 0)
			{
				classes_groups[CLS_NORMAL] = SPLIT_RIGHT;
				classes_groups[CLS_LOWER_INF] = SPLIT_RIGHT;
				classes_groups[CLS_UPPER_INF] = SPLIT_RIGHT;
				classes_groups[CLS_LOWER_INF | CLS_UPPER_INF] = SPLIT_RIGHT;
			}
			else
			{
				/*
				 * 要么 total_count == emptyCount，要么等于
				 * infCount
				 */
				classes_groups[biggest_class] = SPLIT_RIGHT;
			}
		}

		range_gist_class_split(typcache, entryvec, v, classes_groups);
	}

	PG_RETURN_POINTER(v);
}

/* GiST 的相等比较器 */
Datum
range_gist_same(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	/*
	 * range_eq 会忽略 RANGE_CONTAIN_EMPTY 标志，所以必须
	 * 自行检查。更一般地，若条目已被正确规范化，
	 * 则不相等的标志字节必意味着不相等的范围……
	 * 故干脆一次测试所有标志位。
	 */
	if (range_get_flags(r1) != range_get_flags(r2))
		*result = false;
	else
	{
		TypeCacheEntry *typcache;

		typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

		*result = range_eq_internal(typcache, r1, r2);
	}

	PG_RETURN_POINTER(result);
}

/*
 *----------------------------------------------------------
 * 静态函数
 *----------------------------------------------------------
 */

/*
 * 返回包含 r1 与 r2 的最小范围
 *
 * 它与常规 range_union 有两点关键区别：
 * 1. 对不相邻的 r1 与 r2 不抛错，而是直接把中间
 * 值吸收进结果范围。
 * 2. 我们会追踪是否有空范围被并入结果，
 * 以便对 contained_by 搜索建立索引。注意这意味
 * 着 GiST 索引内形成的*所有*并集都必须经过这里。
 */
static RangeType *
range_super_union(TypeCacheEntry *typcache, RangeType *r1, RangeType *r2)
{
	RangeType  *result;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	char		flags1,
				flags2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);
	flags1 = range_get_flags(r1);
	flags2 = range_get_flags(r2);

	if (empty1)
	{
		/* 若 r2 已是或含空范围，可直接原样返回 r2 */
		if (flags2 & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY))
			return r2;
		/* 否则最好复制一份（就地修改不安全） */
		r2 = rangeCopy(r2);
		range_set_contain_empty(r2);
		return r2;
	}
	if (empty2)
	{
		/* 若 r1 已是或含空范围，可直接原样返回 r1 */
		if (flags1 & (RANGE_EMPTY | RANGE_CONTAIN_EMPTY))
			return r1;
		/* 否则最好复制一份（就地修改不安全） */
		r1 = rangeCopy(r1);
		range_set_contain_empty(r1);
		return r1;
	}

	if (range_cmp_bounds(typcache, &lower1, &lower2) <= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(typcache, &upper1, &upper2) >= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	/* 避免构造新范围的优化手段 */
	if (result_lower == &lower1 && result_upper == &upper1 &&
		((flags1 & RANGE_CONTAIN_EMPTY) || !(flags2 & RANGE_CONTAIN_EMPTY)))
		return r1;
	if (result_lower == &lower2 && result_upper == &upper2 &&
		((flags2 & RANGE_CONTAIN_EMPTY) || !(flags1 & RANGE_CONTAIN_EMPTY)))
		return r2;

	result = make_range(typcache, result_lower, result_upper, false, NULL);

	if ((flags1 & RANGE_CONTAIN_EMPTY) || (flags2 & RANGE_CONTAIN_EMPTY))
		range_set_contain_empty(result);

	return result;
}

static bool
multirange_union_range_equal(TypeCacheEntry *typcache,
							 const RangeType *r,
							 const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2,
				tmp;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return (RangeIsEmpty(r) && MultirangeIsEmpty(mr));

	range_deserialize(typcache, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(typcache, mr, 0, &lower2, &tmp);
	multirange_get_bounds(typcache, mr, mr->rangeCount - 1, &tmp, &upper2);

	return (range_cmp_bounds(typcache, &lower1, &lower2) == 0 &&
			range_cmp_bounds(typcache, &upper1, &upper2) == 0);
}

/*
 * 范围查询下内部页的 GiST 一致性测试
 */
static bool
range_gist_consistent_int_range(TypeCacheEntry *typcache,
								StrategyNumber strategy,
								const RangeType *key,
								const RangeType *query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			if (RangeIsEmpty(key) || RangeIsEmpty(query))
				return false;
			return (!range_overright_internal(typcache, key, query));
		case RANGESTRAT_OVERLEFT:
			if (RangeIsEmpty(key) || RangeIsEmpty(query))
				return false;
			return (!range_after_internal(typcache, key, query));
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_internal(typcache, key, query);
		case RANGESTRAT_OVERRIGHT:
			if (RangeIsEmpty(key) || RangeIsEmpty(query))
				return false;
			return (!range_before_internal(typcache, key, query));
		case RANGESTRAT_AFTER:
			if (RangeIsEmpty(key) || RangeIsEmpty(query))
				return false;
			return (!range_overleft_internal(typcache, key, query));
		case RANGESTRAT_ADJACENT:
			if (RangeIsEmpty(key) || RangeIsEmpty(query))
				return false;
			if (range_adjacent_internal(typcache, key, query))
				return true;
			return range_overlaps_internal(typcache, key, query);
		case RANGESTRAT_CONTAINS:
			return range_contains_internal(typcache, key, query);
		case RANGESTRAT_CONTAINED_BY:

			/*
			 * 空范围被一切包含，因此若键是空范围或
			 * 包含空范围，就必须下降到其中。否则，仅当
			 * 键与查询重叠时才下降。
			 */
			if (RangeIsOrContainsEmpty(key))
				return true;
			return range_overlaps_internal(typcache, key, query);
		case RANGESTRAT_EQ:

			/*
			 * 若查询为空，仅当键是或包含任何空范围时
			 * 才下降。否则仅当键包含查询时下降。
			 */
			if (RangeIsEmpty(query))
				return RangeIsOrContainsEmpty(key);
			return range_contains_internal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 多范围查询下内部页的 GiST 一致性测试
 */
static bool
range_gist_consistent_int_multirange(TypeCacheEntry *typcache,
									 StrategyNumber strategy,
									 const RangeType *key,
									 const MultirangeType *query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			if (RangeIsEmpty(key) || MultirangeIsEmpty(query))
				return false;
			return (!range_overright_multirange_internal(typcache, key, query));
		case RANGESTRAT_OVERLEFT:
			if (RangeIsEmpty(key) || MultirangeIsEmpty(query))
				return false;
			return (!range_after_multirange_internal(typcache, key, query));
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_multirange_internal(typcache, key, query);
		case RANGESTRAT_OVERRIGHT:
			if (RangeIsEmpty(key) || MultirangeIsEmpty(query))
				return false;
			return (!range_before_multirange_internal(typcache, key, query));
		case RANGESTRAT_AFTER:
			if (RangeIsEmpty(key) || MultirangeIsEmpty(query))
				return false;
			return (!range_overleft_multirange_internal(typcache, key, query));
		case RANGESTRAT_ADJACENT:
			if (RangeIsEmpty(key) || MultirangeIsEmpty(query))
				return false;
			if (range_adjacent_multirange_internal(typcache, key, query))
				return true;
			return range_overlaps_multirange_internal(typcache, key, query);
		case RANGESTRAT_CONTAINS:
			return range_contains_multirange_internal(typcache, key, query);
		case RANGESTRAT_CONTAINED_BY:

			/*
			 * 空范围被一切包含，因此若键是空范围或
			 * 包含空范围，就必须下降到其中。否则，仅当
			 * 键与查询重叠时才下降。
			 */
			if (RangeIsOrContainsEmpty(key))
				return true;
			return range_overlaps_multirange_internal(typcache, key, query);
		case RANGESTRAT_EQ:

			/*
			 * 若查询为空，仅当键是或包含任何空范围时
			 * 才下降。否则仅当键包含查询时下降。
			 */
			if (MultirangeIsEmpty(query))
				return RangeIsOrContainsEmpty(key);
			return range_contains_multirange_internal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 元素查询下内部页的 GiST 一致性测试
 */
static bool
range_gist_consistent_int_element(TypeCacheEntry *typcache,
								  StrategyNumber strategy,
								  const RangeType *key,
								  Datum query)
{
	switch (strategy)
	{
		case RANGESTRAT_CONTAINS_ELEM:
			return range_contains_elem_internal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 范围查询下叶子页的 GiST 一致性测试
 */
static bool
range_gist_consistent_leaf_range(TypeCacheEntry *typcache,
								 StrategyNumber strategy,
								 const RangeType *key,
								 const RangeType *query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			return range_before_internal(typcache, key, query);
		case RANGESTRAT_OVERLEFT:
			return range_overleft_internal(typcache, key, query);
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_internal(typcache, key, query);
		case RANGESTRAT_OVERRIGHT:
			return range_overright_internal(typcache, key, query);
		case RANGESTRAT_AFTER:
			return range_after_internal(typcache, key, query);
		case RANGESTRAT_ADJACENT:
			return range_adjacent_internal(typcache, key, query);
		case RANGESTRAT_CONTAINS:
			return range_contains_internal(typcache, key, query);
		case RANGESTRAT_CONTAINED_BY:
			return range_contained_by_internal(typcache, key, query);
		case RANGESTRAT_EQ:
			return range_eq_internal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 多范围查询下叶子页的 GiST 一致性测试
 */
static bool
range_gist_consistent_leaf_multirange(TypeCacheEntry *typcache,
									  StrategyNumber strategy,
									  const RangeType *key,
									  const MultirangeType *query)
{
	switch (strategy)
	{
		case RANGESTRAT_BEFORE:
			return range_before_multirange_internal(typcache, key, query);
		case RANGESTRAT_OVERLEFT:
			return range_overleft_multirange_internal(typcache, key, query);
		case RANGESTRAT_OVERLAPS:
			return range_overlaps_multirange_internal(typcache, key, query);
		case RANGESTRAT_OVERRIGHT:
			return range_overright_multirange_internal(typcache, key, query);
		case RANGESTRAT_AFTER:
			return range_after_multirange_internal(typcache, key, query);
		case RANGESTRAT_ADJACENT:
			return range_adjacent_multirange_internal(typcache, key, query);
		case RANGESTRAT_CONTAINS:
			return range_contains_multirange_internal(typcache, key, query);
		case RANGESTRAT_CONTAINED_BY:
			return multirange_contains_range_internal(typcache, query, key);
		case RANGESTRAT_EQ:
			return multirange_union_range_equal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 元素查询下叶子页的 GiST 一致性测试
 */
static bool
range_gist_consistent_leaf_element(TypeCacheEntry *typcache,
								   StrategyNumber strategy,
								   const RangeType *key,
								   Datum query)
{
	switch (strategy)
	{
		case RANGESTRAT_CONTAINS_ELEM:
			return range_contains_elem_internal(typcache, key, query);
		default:
			elog(ERROR, "unrecognized range strategy: %d", strategy);
			return false;		/* 让编译器安静 */
	}
}

/*
 * 平凡拆分：一半条目放到一个页面，
 * 另一半放到另一个页面。
 */
static void
range_gist_fallback_split(TypeCacheEntry *typcache,
						  GistEntryVector *entryvec,
						  GIST_SPLITVEC *v)
{
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff,
				split_idx;

	maxoff = entryvec->n - 1;
	/* 此前的条目拆到左页，此后的拆到右页： */
	split_idx = (maxoff - FirstOffsetNumber) / 2 + FirstOffsetNumber;

	v->spl_nleft = 0;
	v->spl_nright = 0;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[i].key);

		if (i < split_idx)
			PLACE_LEFT(range, i);
		else
			PLACE_RIGHT(range, i);
	}

	v->spl_ldatum = RangeTypePGetDatum(left_range);
	v->spl_rdatum = RangeTypePGetDatum(right_range);
}

/*
 * 基于范围类别进行拆分。
 *
 * 类别定义见 get_gist_range_class。
 * classes_groups 是长度为 CLS_COUNT 的数组，指明
 * 各类应去拆分的哪一侧。
 */
static void
range_gist_class_split(TypeCacheEntry *typcache,
					   GistEntryVector *entryvec,
					   GIST_SPLITVEC *v,
					   SplitLR *classes_groups)
{
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff;

	maxoff = entryvec->n - 1;

	v->spl_nleft = 0;
	v->spl_nright = 0;
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[i].key);
		int			class;

		/* 取范围的类别 */
		class = get_gist_range_class(range);

		/* 把范围放到相应的页面 */
		if (classes_groups[class] == SPLIT_LEFT)
			PLACE_LEFT(range, i);
		else
		{
			Assert(classes_groups[class] == SPLIT_RIGHT);
			PLACE_RIGHT(range, i);
		}
	}

	v->spl_ldatum = RangeTypePGetDatum(left_range);
	v->spl_rdatum = RangeTypePGetDatum(right_range);
}

/*
 * 基于排序的拆分。排序后前一半条目将被
 * 放到一个页面，后一半条目将被放到另一个
 * 页面。use_upper_bound 参数指示用上边界还是
 * 下边界排序。
 */
static void
range_gist_single_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v,
								bool use_upper_bound)
{
	SingleBoundSortItem *sortItems;
	RangeType  *left_range = NULL;
	RangeType  *right_range = NULL;
	OffsetNumber i,
				maxoff,
				split_idx;

	maxoff = entryvec->n - 1;

	sortItems = (SingleBoundSortItem *)
		palloc(maxoff * sizeof(SingleBoundSortItem));

	/*
	 * 准备辅助数组并对值排序。
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[i].key);
		RangeBound	bound2;
		bool		empty;

		sortItems[i - 1].index = i;
		/* 把相应的边界放入数组 */
		if (use_upper_bound)
			range_deserialize(typcache, range, &bound2,
							  &sortItems[i - 1].bound, &empty);
		else
			range_deserialize(typcache, range, &sortItems[i - 1].bound,
							  &bound2, &empty);
		Assert(!empty);
	}

	qsort_arg(sortItems, maxoff, sizeof(SingleBoundSortItem),
			  single_bound_cmp, typcache);

	split_idx = maxoff / 2;

	v->spl_nleft = 0;
	v->spl_nright = 0;

	for (i = 0; i < maxoff; i++)
	{
		int			idx = sortItems[i].index;
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[idx].key);

		if (i < split_idx)
			PLACE_LEFT(range, idx);
		else
			PLACE_RIGHT(range, idx);
	}

	v->spl_ldatum = RangeTypePGetDatum(left_range);
	v->spl_rdatum = RangeTypePGetDatum(right_range);
}

/*
 * 双重排序拆分算法。
 *
 * 算法考虑把范围划分为两组。第一组（左组）包含
 * 公共左边界。第二组（右组）包含公共右边界。
 * 难点在于找到左组的上边界与右组的下边界，
 * 使得两组的重叠最小且分配比例可以接受。算
 * 法会为右组的每个下边界找出最小的左组上边界，
 * 并为左组的每个上边界找出最大的右组下边界。
 * 对每一对找到的边界，
 * range_gist_consider_split 会考虑用它替换当前
 * 选定的拆分。
 *
 * 此后，所有条目被划分成三组：
 * 1) 应放入左组的条目
 * 2) 应放入右组的条目
 * 3) "公共条目"，即可放入任一组而不影响
 *	  重叠量。
 *
 * 公共范围按下述差值来分配：公共范围下边界到
 * 右组下边界的距离，与公共范围上边界到左组上
 * 边界的距离之差。
 *
 * 详见：
 * "A new double sorting-based node splitting algorithm for R-tree",
 * A. Korotkov
 * http://syrcose.ispras.ru/2011/files/SYRCoSE2011_Proceedings.pdf#page=36
 */
static void
range_gist_double_sorting_split(TypeCacheEntry *typcache,
								GistEntryVector *entryvec,
								GIST_SPLITVEC *v)
{
	ConsiderSplitContext context;
	OffsetNumber i,
				maxoff;
	RangeType  *left_range = NULL,
			   *right_range = NULL;
	int			common_entries_count;
	NonEmptyRange *by_lower,
			   *by_upper;
	CommonEntry *common_entries;
	int			nentries,
				i1,
				i2;
	RangeBound *right_lower,
			   *left_upper;

	memset(&context, 0, sizeof(ConsiderSplitContext));
	context.typcache = typcache;
	context.has_subtype_diff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	maxoff = entryvec->n - 1;
	nentries = context.entries_count = maxoff - FirstOffsetNumber + 1;
	context.first = true;

	/* 为排序后的范围边界分配数组 */
	by_lower = (NonEmptyRange *) palloc(nentries * sizeof(NonEmptyRange));
	by_upper = (NonEmptyRange *) palloc(nentries * sizeof(NonEmptyRange));

	/* 填充边界数组 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range = DatumGetRangeTypeP(entryvec->vector[i].key);
		bool		empty;

		range_deserialize(typcache, range,
						  &by_lower[i - FirstOffsetNumber].lower,
						  &by_lower[i - FirstOffsetNumber].upper,
						  &empty);
		Assert(!empty);
	}

	/*
	 * 构造两个范围边界数组：其一按下边界排序，
	 * 其二按上边界。
	 */
	memcpy(by_upper, by_lower, nentries * sizeof(NonEmptyRange));
	qsort_arg(by_lower, nentries, sizeof(NonEmptyRange),
			  interval_cmp_lower, typcache);
	qsort_arg(by_upper, nentries, sizeof(NonEmptyRange),
			  interval_cmp_upper, typcache);

	/*----------
	 * 目标是构造出一个左范围与一个右范围，使得
	 * 各条目范围必被左或右区间（或两者）包含。
	 *
	 * 例如，对于范围 (0,1)、(1,3)、(2,3)、(2,4)：
	 *
	 * 0 1 2 3 4
	 * +-+
	 *	 +---+
	 *	   +-+
	 *	   +---+
	 *
	 * 左范围与右范围形如 (0,a) 与 (b,4)。
	 * 我们先考虑 b 为某条目下边界的拆分。
	 * 我们遍历所有条目，对每个 b 计算最小
	 * 可能的 a。接着再考虑 a 为某条目上边
	 * 界的拆分，并对每个 a 计算最大可能的
	 * b。
	 *
	 * 在上面的例子中，第一轮循环会考虑拆分：
	 * b=0: (0,1)-(0,4)
	 * b=1: (0,1)-(1,4)
	 * b=2: (0,3)-(2,4)
	 *
	 * 第二轮为：
	 * a=1: (0,1)-(1,4)
	 * a=3: (0,3)-(2,4)
	 * a=4: (0,4)-(2,4)
	 *----------
	 */

	/*
	 * 遍历右组的下边界，找出最小的可能
	 * 左组上边界。
	 */
	i1 = 0;
	i2 = 0;
	right_lower = &by_lower[i1].lower;
	left_upper = &by_upper[i2].lower;
	while (true)
	{
		/*
		 * 找出右组的下一个下边界。
		 */
		while (i1 < nentries &&
			   range_cmp_bounds(typcache, right_lower,
								&by_lower[i1].lower) == 0)
		{
			if (range_cmp_bounds(typcache, &by_lower[i1].upper,
								 left_upper) > 0)
				left_upper = &by_lower[i1].upper;
			i1++;
		}
		if (i1 >= nentries)
			break;
		right_lower = &by_lower[i1].lower;

		/*
		 * 统计无论如何都应放入左组的范围
		 * 数量
		 */
		while (i2 < nentries &&
			   range_cmp_bounds(typcache, &by_upper[i2].upper,
								left_upper) <= 0)
			i2++;

		/*
		 * 考虑找到的拆分，看它是否优于当前方案。
		 */
		range_gist_consider_split(&context, right_lower, i1, left_upper, i2);
	}

	/*
	 * 遍历左组的上边界，找出最大的可能
	 * 右组下边界。
	 */
	i1 = nentries - 1;
	i2 = nentries - 1;
	right_lower = &by_lower[i1].upper;
	left_upper = &by_upper[i2].upper;
	while (true)
	{
		/*
		 * 找出左组的下一个上边界。
		 */
		while (i2 >= 0 &&
			   range_cmp_bounds(typcache, left_upper,
								&by_upper[i2].upper) == 0)
		{
			if (range_cmp_bounds(typcache, &by_upper[i2].lower,
								 right_lower) < 0)
				right_lower = &by_upper[i2].lower;
			i2--;
		}
		if (i2 < 0)
			break;
		left_upper = &by_upper[i2].upper;

		/*
		 * 统计无论如何都应放入右组的区间
		 * 数量
		 */
		while (i1 >= 0 &&
			   range_cmp_bounds(typcache, &by_lower[i1].lower,
								right_lower) >= 0)
			i1--;

		/*
		 * 考虑找到的拆分，看它是否优于当前方案。
		 */
		range_gist_consider_split(&context, right_lower, i1 + 1,
								  left_upper, i2 + 1);
	}

	/*
	 * 若找不到任何可接受的拆分，则用平凡拆分。
	 */
	if (context.first)
	{
		range_gist_fallback_split(typcache, entryvec, v);
		return;
	}

	/*
	 * 好，现在已经选定了两组的边界。接下来
	 * 要分配条目本身。先分配那些可以无歧义放
	 * 置的条目，并把"公共条目"收集到数组中。
	 */

	/* 为结果分配向量 */
	v->spl_left = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/*
	 * 为"公共条目"分配数组——即可以放入任一
	 * 组而不影响所选轴上重叠的条目。
	 */
	common_entries_count = 0;
	common_entries = (CommonEntry *) palloc(nentries * sizeof(CommonEntry));

	/*
	 * 分配那些可以无歧义放置的条目，并收集
	 * 公共条目。
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		RangeType  *range;
		RangeBound	lower,
					upper;
		bool		empty;

		/*
		 * 获取所选轴上的上边界与下边界。
		 */
		range = DatumGetRangeTypeP(entryvec->vector[i].key);

		range_deserialize(typcache, range, &lower, &upper, &empty);

		if (range_cmp_bounds(typcache, &upper, context.left_upper) <= 0)
		{
			/* 可放入左组 */
			if (range_cmp_bounds(typcache, &lower, context.right_lower) >= 0)
			{
				/* 也可放入右组，故为"公共条目" */
				common_entries[common_entries_count].index = i;
				if (context.has_subtype_diff)
				{
					/*
					 * delta = (lower - context.right_lower) -
					 * (context.left_upper - upper)
					 */
					common_entries[common_entries_count].delta =
						call_subtype_diff(typcache,
										  lower.val,
										  context.right_lower->val) -
						call_subtype_diff(typcache,
										  context.left_upper->val,
										  upper.val);
				}
				else
				{
					/* 无 subtype_diff 时把所有 delta 取为零 */
					common_entries[common_entries_count].delta = 0;
				}
				common_entries_count++;
			}
			else
			{
				/* 放不进右组，于是并入左组 */
				PLACE_LEFT(range, i);
			}
		}
		else
		{
			/*
			 * 每个条目都应能放入左组或右组之一。由于该
			 * 条目未能放入左组，那么它最好能放入右
			 * 组。
			 */
			Assert(range_cmp_bounds(typcache, &lower,
									context.right_lower) >= 0);
			PLACE_RIGHT(range, i);
		}
	}

	/*
	 * 分配"公共条目"（如有）。
	 */
	if (common_entries_count > 0)
	{
		/*
		 * 按算得的 delta 对"公共条目"排序，以便
		 * 优先分配最含糊的条目。
		 */
		qsort(common_entries, common_entries_count, sizeof(CommonEntry),
			  common_entry_cmp);

		/*
		 * 按排序结果在两组之间分配"公共条目"。
		 */
		for (i = 0; i < common_entries_count; i++)
		{
			RangeType  *range;
			int			idx = common_entries[i].index;

			range = DatumGetRangeTypeP(entryvec->vector[idx].key);

			/*
			 * 检查是否必须把该条目放入某一组才能达到
			 * LIMIT_RATIO
			 */
			if (i < context.common_left)
				PLACE_LEFT(range, idx);
			else
				PLACE_RIGHT(range, idx);
		}
	}

	v->spl_ldatum = PointerGetDatum(left_range);
	v->spl_rdatum = PointerGetDatum(right_range);
}

/*
 * 考虑在 range_gist_double_sorting_split 期间
 * 用更优者替换当前选定的拆分
 */
static void
range_gist_consider_split(ConsiderSplitContext *context,
						  RangeBound *right_lower, int min_left_count,
						  RangeBound *left_upper, int max_left_count)
{
	int			left_count,
				right_count;
	float4		ratio,
				overlap;

	/*
	 * 在公共条目分布最均匀的假定下计算条目的
	 * 分配比例。
	 */
	if (min_left_count >= (context->entries_count + 1) / 2)
		left_count = min_left_count;
	else if (max_left_count <= context->entries_count / 2)
		left_count = max_left_count;
	else
		left_count = context->entries_count / 2;
	right_count = context->entries_count - left_count;

	/*
	 * 拆分比例：较小组大小与条目总数之商。
	 * 该值必然 ≤ 0.5；若小于 LIMIT_RATIO，则绝不
	 * 会接受新拆分。
	 */
	ratio = ((float4) Min(left_count, right_count)) /
		((float4) context->entries_count);

	if (ratio > LIMIT_RATIO)
	{
		bool		selectthis = false;

		/*
		 * 比例可以接受，于是将当前拆分与先前选定的
		 * 拆分进行比较。寻找最小重叠（可为负值），
		 * 其次是最小比例。若有 subtype_diff，则用
		 * 它度量重叠。无 subtype_diff 时用"公共条
		 * 目"数量作为重叠度量。
		 */
		if (context->has_subtype_diff)
			overlap = call_subtype_diff(context->typcache,
										left_upper->val,
										right_lower->val);
		else
			overlap = max_left_count - min_left_count;

		/* 若此前没有选定者，则选定此拆分 */
		if (context->first)
			selectthis = true;
		else
		{
			/*
			 * 若新拆分重叠更小，或重叠相同比例更优，
			 * 则选用新拆分。
			 */
			if (overlap < context->overlap ||
				(overlap == context->overlap && ratio > context->ratio))
				selectthis = true;
		}

		if (selectthis)
		{
			/* 保存选定拆分的相关信息 */
			context->first = false;
			context->ratio = ratio;
			context->overlap = overlap;
			context->right_lower = right_lower;
			context->left_upper = left_upper;
			context->common_left = max_left_count - left_count;
			context->common_right = left_count - min_left_count;
		}
	}
}

/*
 * 求范围的类别号。
 *
 * 类别号是范围诸属性的有效组合。注意：最大
 * 可能值是 8，因为 CLS_EMPTY 无法与任何其他
 * 内容组合。
 */
static int
get_gist_range_class(RangeType *range)
{
	int			classNumber;
	char		flags;

	flags = range_get_flags(range);
	if (flags & RANGE_EMPTY)
	{
		classNumber = CLS_EMPTY;
	}
	else
	{
		classNumber = 0;
		if (flags & RANGE_LB_INF)
			classNumber |= CLS_LOWER_INF;
		if (flags & RANGE_UB_INF)
			classNumber |= CLS_UPPER_INF;
		if (flags & RANGE_CONTAIN_EMPTY)
			classNumber |= CLS_CONTAIN_EMPTY;
	}
	return classNumber;
}

/*
 * range_gist_single_sorting_split 的比较函数。
 */
static int
single_bound_cmp(const void *a, const void *b, void *arg)
{
	SingleBoundSortItem *i1 = (SingleBoundSortItem *) a;
	SingleBoundSortItem *i2 = (SingleBoundSortItem *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->bound, &i2->bound);
}

/*
 * 按下边界比较 NonEmptyRange
 */
static int
interval_cmp_lower(const void *a, const void *b, void *arg)
{
	NonEmptyRange *i1 = (NonEmptyRange *) a;
	NonEmptyRange *i2 = (NonEmptyRange *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->lower, &i2->lower);
}

/*
 * 按上边界比较 NonEmptyRange
 */
static int
interval_cmp_upper(const void *a, const void *b, void *arg)
{
	NonEmptyRange *i1 = (NonEmptyRange *) a;
	NonEmptyRange *i2 = (NonEmptyRange *) b;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, &i1->upper, &i2->upper);
}

/*
 * 按 delta 比较 CommonEntry
 */
static int
common_entry_cmp(const void *i1, const void *i2)
{
	double		delta1 = ((CommonEntry *) i1)->delta;
	double		delta2 = ((CommonEntry *) i2)->delta;

	if (delta1 < delta2)
		return -1;
	else if (delta1 > delta2)
		return 1;
	else
		return 0;
}

/*
 * 调用类型专属 subtype_diff 函数的便捷函数。
 * 调用者必须已检查过该范围类型存在此函数。
 */
static float8
call_subtype_diff(TypeCacheEntry *typcache, Datum val1, Datum val2)
{
	float8		value;

	value = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
											 typcache->rng_collation,
											 val1, val2));
	/* 返回零以应对有缺陷的 subtype_diff 函数 */
	if (value >= 0.0)
		return value;
	return 0.0;
}
