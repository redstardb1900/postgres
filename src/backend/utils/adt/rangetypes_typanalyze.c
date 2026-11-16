/*-------------------------------------------------------------------------
 *
 * rangetypes_typanalyze.c
 *	  从范围列收集统计信息的函数
 *
 * 对范围类型列，将收集下界与上界的直方图，以及
 * NULL 与空范围所占的比例。
 *
 * 两个直方图长度相同，且会合并为一个范围数组。
 * 其形状与 std_typanalyze 所收集的直方图相同，
 * 但值有所不同。数组中每个范围都是有效的范围，
 * 即使下界与上界来自不同的元组。
 * 理论上，标准标量选择率函数
 * 也可以用于合并后的直方图。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "commands/vacuum.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"
#include "varatt.h"

static int	float8_qsort_cmp(const void *a1, const void *a2, void *arg);
static int	range_bound_qsort_cmp(const void *a1, const void *a2, void *arg);
static void compute_range_stats(VacAttrStats *stats,
								AnalyzeAttrFetchFunc fetchfunc, int samplerows,
								double totalrows);

/*
 * range_typanalyze -- 范围列的统计收集函数
 */
Datum
range_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	TypeCacheEntry *typcache;
	Form_pg_attribute attr = stats->attr;

	/* 获取范围类型的信息；注意列可能是一个域 */
	typcache = range_get_typcache(fcinfo, getBaseType(stats->attrtypid));

	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_range_stats;
	stats->extra_data = typcache;
	/* 与 std_typanalyze 相同 */
	stats->minrows = 300 * attr->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 * multirange_typanalyze -- 多范围列的统计收集函数
 *
 * 我们对范围做同样的分析，但对象是完全包含
 * 该多范围的最小范围。
 */
Datum
multirange_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	TypeCacheEntry *typcache;
	Form_pg_attribute attr = stats->attr;

	/* 获取多范围类型的信息；注意列可能是一个域 */
	typcache = multirange_get_typcache(fcinfo, getBaseType(stats->attrtypid));

	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_range_stats;
	stats->extra_data = typcache;
	/* 与 std_typanalyze 相同 */
	stats->minrows = 300 * attr->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 * 用于排序 float8 的比较函数，供范围长度使用。
 */
static int
float8_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	const float8 *f1 = (const float8 *) a1;
	const float8 *f2 = (const float8 *) a2;

	if (*f1 < *f2)
		return -1;
	else if (*f1 == *f2)
		return 0;
	else
		return 1;
}

/*
 * 用于排序 RangeBounds 的比较函数。
 */
static int
range_bound_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	RangeBound *b1 = (RangeBound *) a1;
	RangeBound *b2 = (RangeBound *) a2;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, b1, b2);
}

/*
 * compute_range_stats() -- 计算范围列的统计信息
 */
static void
compute_range_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) stats->extra_data;
	TypeCacheEntry *mltrng_typcache = NULL;
	bool		has_subdiff;
	int			null_cnt = 0;
	int			non_null_cnt = 0;
	int			non_empty_cnt = 0;
	int			empty_cnt = 0;
	int			range_no;
	int			slot_idx;
	int			num_bins = stats->attr->attstattarget;
	int			num_hist;
	float8	   *lengths;
	RangeBound *lowers,
			   *uppers;
	double		total_width = 0;

	if (typcache->typtype == TYPTYPE_MULTIRANGE)
	{
		mltrng_typcache = typcache;
		typcache = typcache->rngtype;
	}
	else
		Assert(typcache->typtype == TYPTYPE_RANGE);
	has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	/* 分配内存以保存范围边界以及采样范围的长度。 */
	lowers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);
	uppers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);
	lengths = (float8 *) palloc(sizeof(float8) * samplerows);

	/* 遍历采样范围。 */
	for (range_no = 0; range_no < samplerows; range_no++)
	{
		Datum		value;
		bool		isnull,
					empty;
		MultirangeType *multirange;
		RangeType  *range;
		RangeBound	lower,
					upper;
		float8		length;

		vacuum_delay_point();

		value = fetchfunc(stats, range_no, &isnull);
		if (isnull)
		{
			/* 空值范围，仅需计数 */
			null_cnt++;
			continue;
		}

		/*
		 * XXX：是否应像 std_typanalyze 那样忽略宽值，以
		 * 避免统计表膨胀？
		 */
		total_width += VARSIZE_ANY(DatumGetPointer(value));

		/* 获取范围并反序列化，供后续分析。 */
		if (mltrng_typcache != NULL)
		{
			/* 将多范围视为没有间隙的大范围。 */
			multirange = DatumGetMultirangeTypeP(value);
			if (!MultirangeIsEmpty(multirange))
			{
				RangeBound	tmp;

				multirange_get_bounds(typcache, multirange, 0,
									  &lower, &tmp);
				multirange_get_bounds(typcache, multirange,
									  multirange->rangeCount - 1,
									  &tmp, &upper);
				empty = false;
			}
			else
			{
				empty = true;
			}
		}
		else
		{
			range = DatumGetRangeTypeP(value);
			range_deserialize(typcache, range, &lower, &upper, &empty);
		}

		if (!empty)
		{
			/* 记住边界与长度，供后面构建直方图使用 */
			lowers[non_empty_cnt] = lower;
			uppers[non_empty_cnt] = upper;

			if (lower.infinite || upper.infinite)
			{
				/* 任何一种无穷范围其长度都是无穷 */
				length = get_float8_infinity();
			}
			else if (has_subdiff)
			{
				/*
				 * 对于普通范围，在上界值与下界值之间使用
				 * subdiff 函数。
				 */
				length = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
														  typcache->rng_collation,
														  upper.val, lower.val));
			}
			else
			{
				/* 若无 subdiff 可用，则使用默认值 1.0。 */
				length = 1.0;
			}
			lengths[non_empty_cnt] = length;

			non_empty_cnt++;
		}
		else
			empty_cnt++;

		non_null_cnt++;
	}

	slot_idx = 0;

	/* 只有找到非空值，我们才能算出真正的统计信息 */
	if (non_null_cnt > 0)
	{
		Datum	   *bound_hist_values;
		Datum	   *length_hist_values;
		int			pos,
					posfrac,
					delta,
					deltafrac,
					i;
		MemoryContext old_cxt;
		float4	   *emptyfrac;

		stats->stats_valid = true;
		/* 做简单的空值比例与宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		stats->stawidth = total_width / (double) non_null_cnt;

		/* 估计非空值都是唯一的 */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);

		/* 必须把目标值复制进 anl_context */
		old_cxt = MemoryContextSwitchTo(stats->anl_context);

		/*
		 * 若有至少两个值，就生成边界直方图槽位条目
		 * 值。
		 */
		if (non_empty_cnt >= 2)
		{
			/* 排序边界值 */
			qsort_interruptible(lowers, non_empty_cnt, sizeof(RangeBound),
								range_bound_qsort_cmp, typcache);
			qsort_interruptible(uppers, non_empty_cnt, sizeof(RangeBound),
								range_bound_qsort_cmp, typcache);

			num_hist = non_empty_cnt;
			if (num_hist > num_bins)
				num_hist = num_bins + 1;

			bound_hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * 本循环的目的是用 lowers[] 与 uppers[] 的首
			 * 尾条目构造范围，其间再插入等距的值。因
			 * 第 i 个值是 lowers[(i * (nvals - 1)) /
			 *  (num_hist - 1)] 与 uppers[(i * (nvals - 1)) /
			 * (num_hist - 1)]。但直接计算该下标，统计目
			 * 标超过几千时有整数溢出风险。因此我们改
			 * 为在每一步给 pos 加 (nvals - 1) / (num_hist - 1)，
			 * 并分别跟踪和的整数部分与分数
			 * 两部分。
			 */
			delta = (non_empty_cnt - 1) / (num_hist - 1);
			deltafrac = (non_empty_cnt - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				bound_hist_values[i] = PointerGetDatum(range_serialize(typcache,
																	   &lowers[pos],
																	   &uppers[pos],
																	   false,
																	   NULL));
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* 分数部分超过 1，进位到整数部分 */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}

			stats->stakind[slot_idx] = STATISTIC_KIND_BOUNDS_HISTOGRAM;
			stats->stavalues[slot_idx] = bound_hist_values;
			stats->numvalues[slot_idx] = num_hist;

			/* 即使分析的是多范围列，也存储这些范围 */
			stats->statypid[slot_idx] = typcache->type_id;
			stats->statyplen[slot_idx] = typcache->typlen;
			stats->statypbyval[slot_idx] = typcache->typbyval;
			stats->statypalign[slot_idx] = typcache->typalign;

			slot_idx++;
		}

		/*
		 * 若有至少两个值，就生成长度直方图槽位条目
		 * 值。
		 */
		if (non_empty_cnt >= 2)
		{
			/*
			 * 将范围长度升序排序，供随后填充
			 * 直方图
			 */
			qsort_interruptible(lengths, non_empty_cnt, sizeof(float8),
								float8_qsort_cmp, NULL);

			num_hist = non_empty_cnt;
			if (num_hist > num_bins)
				num_hist = num_bins + 1;

			length_hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * 本循环的目的是复制 lengths[] 的首尾条目以及
			 * 其间等距的值。因此第 i 个值为
			 * lengths[(i * (nvals - 1)) / (num_hist - 1)]。
			 * 但直接计算该下标在统计目标超过几千时
			 * 有整数溢出风险。因此我们改为每步给
			 * pos 增加 (nvals - 1) / (num_hist - 1)，并跟踪
			 * 和的整数部分与分数部分。
			 */
			delta = (non_empty_cnt - 1) / (num_hist - 1);
			deltafrac = (non_empty_cnt - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				length_hist_values[i] = Float8GetDatum(lengths[pos]);
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* 分数部分超过 1，进位到整数部分 */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}
		}
		else
		{
			/*
			 * 即使不建直方图，也要存储一个空数组来
			 * 表示"没有直方图"。不能把 stavalues 留成
			 * NULL，因为向 get_attstatsslot() 请求的 stavalues
			 * 为 NULL 时会报错。空比例仍存入 stanumbers。
			 */
			length_hist_values = palloc(0);
			num_hist = 0;
		}
		stats->staop[slot_idx] = Float8LessOperator;
		stats->stacoll[slot_idx] = InvalidOid;
		stats->stavalues[slot_idx] = length_hist_values;
		stats->numvalues[slot_idx] = num_hist;
		stats->statypid[slot_idx] = FLOAT8OID;
		stats->statyplen[slot_idx] = sizeof(float8);
		stats->statypbyval[slot_idx] = FLOAT8PASSBYVAL;
		stats->statypalign[slot_idx] = 'd';

		/* 存储空范围的比例 */
		emptyfrac = (float4 *) palloc(sizeof(float4));
		*emptyfrac = ((double) empty_cnt) / ((double) non_null_cnt);
		stats->stanumbers[slot_idx] = emptyfrac;
		stats->numnumbers[slot_idx] = 1;

		stats->stakind[slot_idx] = STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM;
		slot_idx++;

		MemoryContextSwitchTo(old_cxt);
	}
	else if (null_cnt > 0)
	{
		/* 只发现了空值；假定该列完全为空值 */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		stats->stawidth = 0;	/* 未知 */
		stats->stadistinct = 0.0;	/* 未知 */
	}

	/*
	 * 我们不必费心清理任何临时 palloc 的内存。哈希表
	 * 也应随之消失，因为它使用的是子内存上下文。
	 */
}
