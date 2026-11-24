/*-------------------------------------------------------------------------
 *
 * multirangetypes_selfuncs.c
 *	  多范围算子选择率估算函数
 *
 * 估算基于下界与上界的直方图，以及空多
 * 范围的占比。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/multirangetypes_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

static double calc_multirangesel(TypeCacheEntry *typcache,
								 VariableStatData *vardata,
								 const MultirangeType *constval, Oid operator);
static double default_multirange_selectivity(Oid operator);
static double calc_hist_selectivity(TypeCacheEntry *typcache,
									VariableStatData *vardata,
									const MultirangeType *constval,
									Oid operator);
static double calc_hist_selectivity_scalar(TypeCacheEntry *typcache,
										   const RangeBound *constbound,
										   const RangeBound *hist,
										   int hist_nvalues, bool equal);
static int	rbound_bsearch(TypeCacheEntry *typcache, const RangeBound *value,
						   const RangeBound *hist, int hist_length, bool equal);
static float8 get_position(TypeCacheEntry *typcache, const RangeBound *value,
						   const RangeBound *hist1, const RangeBound *hist2);
static float8 get_len_position(double value, double hist1, double hist2);
static float8 get_distance(TypeCacheEntry *typcache, const RangeBound *bound1,
						   const RangeBound *bound2);
static int	length_hist_bsearch(Datum *length_hist_values,
								int length_hist_nvalues, double value,
								bool equal);
static double calc_length_hist_frac(Datum *length_hist_values,
									int length_hist_nvalues, double length1,
									double length2, bool equal);
static double calc_hist_selectivity_contained(TypeCacheEntry *typcache,
											  const RangeBound *lower,
											  RangeBound *upper,
											  const RangeBound *hist_lower,
											  int hist_nvalues,
											  Datum *length_hist_values,
											  int length_hist_nvalues);
static double calc_hist_selectivity_contains(TypeCacheEntry *typcache,
											 const RangeBound *lower,
											 const RangeBound *upper,
											 const RangeBound *hist_lower,
											 int hist_nvalues,
											 Datum *length_hist_values,
											 int length_hist_nvalues);

/*
 * 在没有统计信息、或因故无法使用统计信息时，为给
 * 定算子返回默认选择率估算。
 */
static double
default_multirange_selectivity(Oid operator)
{
	switch (operator)
	{
		case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
		case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
			return 0.01;

		case OID_RANGE_CONTAINS_MULTIRANGE_OP:
		case OID_RANGE_MULTIRANGE_CONTAINED_OP:
		case OID_MULTIRANGE_CONTAINS_RANGE_OP:
		case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
		case OID_MULTIRANGE_RANGE_CONTAINED_OP:
		case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
			return 0.005;

		case OID_MULTIRANGE_CONTAINS_ELEM_OP:
		case OID_MULTIRANGE_ELEM_CONTAINED_OP:

			/*
			 * "multirange @> elem" 大致等同于标量不等式
			 * "A >= b AND A <= c"。
			 */
			return DEFAULT_MULTIRANGE_INEQ_SEL;

		case OID_MULTIRANGE_LESS_OP:
		case OID_MULTIRANGE_LESS_EQUAL_OP:
		case OID_MULTIRANGE_GREATER_OP:
		case OID_MULTIRANGE_GREATER_EQUAL_OP:
		case OID_MULTIRANGE_LEFT_RANGE_OP:
		case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
		case OID_RANGE_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_RIGHT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
		case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
		case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			/* 这些与常规标量不等式类似 */
			return DEFAULT_INEQ_SEL;

		default:

			/*
			 * 多范围算子均应已在上方处理，但以防万一
			 * 
			 */
			return 0.01;
	}
}

/*
 * multirangesel -- 多范围算子的限制选择率
 */
Datum
multirangesel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	TypeCacheEntry *typcache = NULL;
	MultirangeType *constmultirange = NULL;
	RangeType  *constrange = NULL;

	/*
	 * 若表达式不是（变量 op 某物）或（某物 op
	 * 变量），则放弃并返回默认估算值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(default_multirange_selectivity(operator));

	/*
	 * 若另一侧也不是常量，那就没什么有用的可做了。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(default_multirange_selectivity(operator));
	}

	/*
	 * 所有多范围算子都是严格算子，NULL 常量可径直
	 * 处理。
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * 若 var 在右侧，则交换算子，以便此后可假定
	 * var 位于左侧。
	 */
	if (!varonleft)
	{
		/* 我们有 other Op var，交换成 var Op other */
		operator = get_commutator(operator);
		if (!operator)
		{
			/* 使用默认选择率（要不要改为抛出错误？） */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(default_multirange_selectivity(operator));
		}
	}

	/*
	 * 好了，这里要处理的是一个 Var 与一个 Const。我们
	 * 需要 Const 与列具有相同的多范围类型，否则无计
	 * 可施。（这类情况多半运行时会失败，但此处宁可
	 * 直取默认估算值。）
	 *
	 * 若算子是 "multirange @> element"，则常量应为多
	 * 范围列的元素类型。把它转成仅含该单点的多范围，
	 * 这样后续就无需对其进行特殊
	 * 处理。
	 */
	if (operator == OID_MULTIRANGE_CONTAINS_ELEM_OP)
	{
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);

		if (((Const *) other)->consttype == typcache->rngtype->rngelemtype->type_id)
		{
			RangeBound	lower,
						upper;

			lower.inclusive = true;
			lower.val = ((Const *) other)->constvalue;
			lower.infinite = false;
			lower.lower = true;
			upper.inclusive = true;
			upper.val = ((Const *) other)->constvalue;
			upper.infinite = false;
			upper.lower = false;
			constrange = range_serialize(typcache->rngtype, &lower, &upper,
										 false, NULL);
			constmultirange = make_multirange(typcache->type_id, typcache->rngtype,
											  1, &constrange);
		}
	}
	else if (operator == OID_RANGE_MULTIRANGE_CONTAINED_OP ||
			 operator == OID_MULTIRANGE_CONTAINS_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP ||
			 operator == OID_MULTIRANGE_LEFT_RANGE_OP ||
			 operator == OID_MULTIRANGE_RIGHT_RANGE_OP)
	{
		/*
		 * 对 "multirange OP range" 中的范围做提升，
		 * 一如对元素的提升。
		 */
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);
		if (((Const *) other)->consttype == typcache->rngtype->type_id)
		{
			constrange = DatumGetRangeTypeP(((Const *) other)->constvalue);
			constmultirange = make_multirange(typcache->type_id, typcache->rngtype,
											  1, &constrange);
		}
	}
	else if (operator == OID_RANGE_OVERLAPS_MULTIRANGE_OP ||
			 operator == OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP ||
			 operator == OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP ||
			 operator == OID_RANGE_LEFT_MULTIRANGE_OP ||
			 operator == OID_RANGE_RIGHT_MULTIRANGE_OP ||
			 operator == OID_RANGE_CONTAINS_MULTIRANGE_OP ||
			 operator == OID_MULTIRANGE_ELEM_CONTAINED_OP ||
			 operator == OID_MULTIRANGE_RANGE_CONTAINED_OP)
	{
		/*
		 * 这里 Var 是 elem/range 而非多范围。目前只能
		 * 放弃并返回默认估算。将来可拆开多范围常量
		 * 做更聪明的
		 * 事。
		 */
	}
	else if (((Const *) other)->consttype == vardata.vartype)
	{
		/* 两侧是相同的多范围类型 */
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);

		constmultirange = DatumGetMultirangeTypeP(((Const *) other)->constvalue);
	}

	/*
	 * 若得到有效常量，就继续按统计估算；否则放弃并
	 * 返回默认常数估算值。注意 calc_multirangesel 无需
	 * 处理
	 * OID_MULTIRANGE_*_CONTAINED_OP.
	 */
	if (constmultirange)
		selec = calc_multirangesel(typcache, &vardata, constmultirange, operator);
	else
		selec = default_multirange_selectivity(operator);

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

static double
calc_multirangesel(TypeCacheEntry *typcache, VariableStatData *vardata,
				   const MultirangeType *constval, Oid operator)
{
	double		hist_selec;
	double		selec;
	float4		empty_frac,
				null_frac;

	/*
	 * 首先从 pg_statistic 查出 NULL 占比与空多范围
	 * 占比。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		null_frac = stats->stanullfrac;

		/* 尝试获取空多范围的占比 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							 InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers != 1)
				elog(ERROR, "invalid empty fraction statistic");	/* 不应发生 */
			empty_frac = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
		else
		{
			/* 无空多范围占比统计。假定无空范围。 */
			empty_frac = 0.0;
		}
	}
	else
	{
		/*
		 * 无统计。仍往下算，假定无空值且无空多
		 * 范围。仍可按常量是否为空多范围给出聊胜于
		 * 无的估算。
		 * 
		 */
		null_frac = 0.0;
		empty_frac = 0.0;
	}

	if (MultirangeIsEmpty(constval))
	{
		/*
		 * 空多范围匹配所有多范围、所有空多范围
		 * 或无匹配，视算子而定
		 */
		switch (operator)
		{
				/* 任一侧为空时这些算子返回 false */
			case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			case OID_MULTIRANGE_LEFT_RANGE_OP:
			case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
			case OID_MULTIRANGE_RIGHT_RANGE_OP:
			case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
				/* 没有东西小于空多范围 */
			case OID_MULTIRANGE_LESS_OP:
				selec = 0.0;
				break;

				/*
				 * 只有空多范围才能被空多范围包含
				 * 
				 */
			case OID_RANGE_MULTIRANGE_CONTAINED_OP:
			case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
				/* 只有空范围会 <= 空多范围 */
			case OID_MULTIRANGE_LESS_EQUAL_OP:
				selec = empty_frac;
				break;

				/* 一切都包含空多范围 */
			case OID_MULTIRANGE_CONTAINS_RANGE_OP:
			case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
				/* 一切都 >= 空多范围 */
			case OID_MULTIRANGE_GREATER_EQUAL_OP:
				selec = 1.0;
				break;

				/* 所有非空多范围都 > 空多范围 */
			case OID_MULTIRANGE_GREATER_OP:
				selec = 1.0 - empty_frac;
				break;

				/* 元素不可能为空 */
			case OID_MULTIRANGE_CONTAINS_ELEM_OP:

				/* 已被 multirangesel() 滤掉 */
			case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
			case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			case OID_RANGE_LEFT_MULTIRANGE_OP:
			case OID_RANGE_RIGHT_MULTIRANGE_OP:
			case OID_RANGE_CONTAINS_MULTIRANGE_OP:
			case OID_MULTIRANGE_ELEM_CONTAINED_OP:
			case OID_MULTIRANGE_RANGE_CONTAINED_OP:

			default:
				elog(ERROR, "unexpected operator %u", operator);
				selec = 0.0;	/* 让编译器安静 */
				break;
		}
	}
	else
	{
		/*
		 * 用边界直方图计算选择率。若因故失败，比如
		 * pg_statistic 无直方图，对非空值占比用默认常数
		 * 估算。这仍比只给默认估算好些，因为它还考虑
		 * 了空多范围与 NULL 元组占比——前提是有相应
		 * 统计。
		 * 
		 */
		hist_selec = calc_hist_selectivity(typcache, vardata, constval,
										   operator);
		if (hist_selec < 0.0)
			hist_selec = default_multirange_selectivity(operator);

		/*
		 * 现在合并空多范围与直方图的结果，须知直
		 * 方图只涵盖非空且非空多范围的值。
		 * 
		 */
		if (operator == OID_RANGE_MULTIRANGE_CONTAINED_OP ||
			operator == OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP)
		{
			/* 空多范围可被任何非空包含 */
			selec = (1.0 - empty_frac) * hist_selec + empty_frac;
		}
		else
		{
			/* 对其他算子，空 Op 非空均不匹配任何内容 */
			selec = (1.0 - empty_frac) * hist_selec;
		}
	}

	/* 多范围算子均严格 */
	selec *= (1.0 - null_frac);

	/* 结果应在范围内，但需确认... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * 用多范围边界的直方图计算多范围算子选择率。
 *
 * 该估算只针对既非空又非 NULL 的那部分值。
 * 
 */
static double
calc_hist_selectivity(TypeCacheEntry *typcache, VariableStatData *vardata,
					  const MultirangeType *constval, Oid operator)
{
	TypeCacheEntry *rng_typcache = typcache->rngtype;
	AttStatsSlot hslot;
	AttStatsSlot lslot;
	int			nhist;
	RangeBound *hist_lower;
	RangeBound *hist_upper;
	int			i;
	RangeBound	const_lower;
	RangeBound	const_upper;
	RangeBound	tmp;
	double		hist_selec;

	/* 多范围支持函数不安全时无法使用直方图 */
	if (!statistic_proc_security_check(vardata,
									   rng_typcache->rng_cmp_proc_finfo.fn_oid))
		return -1;
	if (OidIsValid(rng_typcache->rng_subdiff_finfo.fn_oid) &&
		!statistic_proc_security_check(vardata,
									   rng_typcache->rng_subdiff_finfo.fn_oid))
		return -1;

	/* 试着取范围直方图 */
	if (!(HeapTupleIsValid(vardata->statsTuple) &&
		  get_attstatsslot(&hslot, vardata->statsTuple,
						   STATISTIC_KIND_BOUNDS_HISTOGRAM, InvalidOid,
						   ATTSTATSSLOT_VALUES)))
		return -1.0;

	/* 检查它是直方图而非只是占位条目 */
	if (hslot.nvalues < 2)
	{
		free_attstatsslot(&hslot);
		return -1.0;
	}

	/*
	 * 把范围的直方图转换成下界与上界两个直方图。
	 * 
	 */
	nhist = hslot.nvalues;
	hist_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist);
	hist_upper = (RangeBound *) palloc(sizeof(RangeBound) * nhist);
	for (i = 0; i < nhist; i++)
	{
		bool		empty;

		range_deserialize(rng_typcache, DatumGetRangeTypeP(hslot.values[i]),
						  &hist_lower[i], &hist_upper[i], &empty);
		/* 直方图不应包含任何空范围 */
		if (empty)
			elog(ERROR, "bounds histogram contains an empty range");
	}

	/* @> 与 @< 还需要范围长度的直方图 */
	if (operator == OID_MULTIRANGE_CONTAINS_RANGE_OP ||
		operator == OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP ||
		operator == OID_MULTIRANGE_RANGE_CONTAINED_OP ||
		operator == OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP)
	{
		if (!(HeapTupleIsValid(vardata->statsTuple) &&
			  get_attstatsslot(&lslot, vardata->statsTuple,
							   STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							   InvalidOid,
							   ATTSTATSSLOT_VALUES)))
		{
			free_attstatsslot(&hslot);
			return -1.0;
		}

		/* 检查它是直方图而非只是占位条目 */
		if (lslot.nvalues < 2)
		{
			free_attstatsslot(&lslot);
			free_attstatsslot(&hslot);
			return -1.0;
		}
	}
	else
		memset(&lslot, 0, sizeof(lslot));

	/* 提取常量值的边界。 */
	Assert(constval->rangeCount > 0);
	multirange_get_bounds(rng_typcache, constval, 0,
						  &const_lower, &tmp);
	multirange_get_bounds(rng_typcache, constval, constval->rangeCount - 1,
						  &tmp, &const_upper);

	/*
	 * 把常量下界或上界与相应的边界直方图进行比
	 * 较，以此计算选择率。
	 */
	switch (operator)
	{
		case OID_MULTIRANGE_LESS_OP:

			/*
			 * 常规 b-tree 比较算子（<,<=,>,>=）先比较下界，
			 * 下界相等时才比上界。只比较下界来估算，
			 * 只要下界等于常量下界的行不多，这一估算就
			 * 相当准确。
			 * 
			 * 
			 */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_LESS_EQUAL_OP:
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_GREATER_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_GREATER_EQUAL_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_LEFT_RANGE_OP:
		case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
			/* upper(var) < lower(const) 则 var << const */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_upper, nhist, false);
			break;

		case OID_MULTIRANGE_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
			/* lower(var) > upper(const) 则 var >> const */
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_upper,
												 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			/* 比较下界 */
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			/* 比较上界 */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_upper,
											 hist_upper, nhist, true);
			break;

		case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_MULTIRANGE_CONTAINS_ELEM_OP:

			/*
			 * A && B <=> NOT (A << B OR A >> B).
			 *
			 * 因 A << B 与 A >> B 互斥，概率可相加，从而
			 * 求得 (A << B OR A >> B) 的概率。
			 * 
			 *
			 * "multirange @> elem" 等价于 "multirange &&
			 * {[elem,elem]}"。调用方已建出单点范围，按 &&
			 * 同样处理即可。
			 * 
			 */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache,
											 &const_lower, hist_upper,
											 nhist, false);
			hist_selec +=
				(1.0 - calc_hist_selectivity_scalar(rng_typcache,
													&const_upper, hist_lower,
													nhist, true));
			hist_selec = 1.0 - hist_selec;
			break;

		case OID_MULTIRANGE_CONTAINS_RANGE_OP:
		case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
			hist_selec =
				calc_hist_selectivity_contains(rng_typcache, &const_lower,
											   &const_upper, hist_lower, nhist,
											   lslot.values, lslot.nvalues);
			break;

		case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
		case OID_RANGE_MULTIRANGE_CONTAINED_OP:
			if (const_lower.infinite)
			{
				/*
				 * 下界已无关。只估上界 <= 常量上界占比
				 * 
				 */
				hist_selec =
					calc_hist_selectivity_scalar(rng_typcache, &const_upper,
												 hist_upper, nhist, true);
			}
			else if (const_upper.infinite)
			{
				hist_selec =
					1.0 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
													   hist_lower, nhist, false);
			}
			else
			{
				hist_selec =
					calc_hist_selectivity_contained(rng_typcache, &const_lower,
													&const_upper, hist_lower, nhist,
													lslot.values, lslot.nvalues);
			}
			break;

			/* 已被 multirangesel() 滤掉 */
		case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_LEFT_MULTIRANGE_OP:
		case OID_RANGE_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_CONTAINS_MULTIRANGE_OP:
		case OID_MULTIRANGE_ELEM_CONTAINED_OP:
		case OID_MULTIRANGE_RANGE_CONTAINED_OP:

		default:
			elog(ERROR, "unknown multirange operator %u", operator);
			hist_selec = -1.0;	/* 让编译器安静 */
			break;
	}

	free_attstatsslot(&lslot);
	free_attstatsslot(&hslot);

	return hist_selec;
}


/*
 * 在范围边界直方图中查出小于（若 equal 参数为真则
 * 小于等于）给定常量的值占比。
 */
static double
calc_hist_selectivity_scalar(TypeCacheEntry *typcache, const RangeBound *constbound,
							 const RangeBound *hist, int hist_nvalues, bool equal)
{
	Selectivity selec;
	int			index;

	/*
	 * 找到常量所落入的直方图箱。选择率按其前
	 * 面的整箱数量来估算。
	 */
	index = rbound_bsearch(typcache, constbound, hist, hist_nvalues, equal);
	selec = (Selectivity) (Max(index, 0)) / (Selectivity) (hist_nvalues - 1);

	/* 用箱内线性插值进行调整 */
	if (index >= 0 && index < hist_nvalues - 1)
		selec += get_position(typcache, constbound, &hist[index],
							  &hist[index + 1]) / (Selectivity) (hist_nvalues - 1);

	return selec;
}

/*
 * 对范围边界数组执行二分查找。返回数组中小于（小
 * 于或等于）给定范围边界的最大下标。若数组中所有
 * 范围边界都大于等于（大于）给定范围边界，则
 * 返回 -1。"equal" 置位时用括号内条件。
 *
 * 本函数用于标量算子选择率估算。本函数的另一个目
 * 标是找到一个直方图箱，以便对小于等于给定
 * 边界的部分插值到何处为止。
 */
static int
rbound_bsearch(TypeCacheEntry *typcache, const RangeBound *value, const RangeBound *hist,
			   int hist_length, bool equal)
{
	int			lower = -1,
				upper = hist_length - 1,
				cmp,
				middle;

	while (lower < upper)
	{
		middle = (lower + upper + 1) / 2;
		cmp = range_cmp_bounds(typcache, &hist[middle], value);

		if (cmp < 0 || (equal && cmp == 0))
			lower = middle;
		else
			upper = middle - 1;
	}
	return lower;
}


/*
 * 对长度直方图执行二分查找。返回直方图中小于（小
 * 于等于）给定长度值的范围长度的最大下标。若直方
 * 图中所有长度都大于（大于等于）给定长度，
 * 则返回 -1。
 */
static int
length_hist_bsearch(Datum *length_hist_values, int length_hist_nvalues,
					double value, bool equal)
{
	int			lower = -1,
				upper = length_hist_nvalues - 1,
				middle;

	while (lower < upper)
	{
		double		middleval;

		middle = (lower + upper + 1) / 2;

		middleval = DatumGetFloat8(length_hist_values[middle]);
		if (middleval < value || (equal && middleval <= value))
			lower = middle;
		else
			upper = middle - 1;
	}
	return lower;
}

/*
 * 求值在直方图箱内 [0,1] 的相对位置。
 */
static float8
get_position(TypeCacheEntry *typcache, const RangeBound *value, const RangeBound *hist1,
			 const RangeBound *hist2)
{
	bool		has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);
	float8		position;

	if (!hist1->infinite && !hist2->infinite)
	{
		float8		bin_width;

		/*
		 * 两边界都有限。假定子类型比较函数工作正常，
		 * 则值也应有限，因其介于两界间。否则就随
		 * 意返回 0.5。
		 * 
		 */
		if (value->infinite)
			return 0.5;

		/* 没有 subdiff 函数无法插值 */
		if (!has_subdiff)
			return 0.5;

		/* 用 subdiff 函数计算相对位置。 */
		bin_width = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
													 typcache->rng_collation,
													 hist2->val,
													 hist1->val));
		if (isnan(bin_width) || bin_width <= 0.0)
			return 0.5;			/* NaN 或零宽箱即放弃 */

		position = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
													typcache->rng_collation,
													value->val,
													hist1->val))
			/ bin_width;

		if (isnan(position))
			return 0.5;			/* subdiff 逢 NaN、Inf/Inf 等则弃 */

		/* 相对位置必须位于 [0,1] 范围内 */
		position = Max(position, 0.0);
		position = Min(position, 1.0);
		return position;
	}
	else if (hist1->infinite && !hist2->infinite)
	{
		/*
		 * 下箱边界为 -inf，上边界有限。若值为 -inf，返回
		 * 0.0 表示它等于下边界。否则返回 1.0 表示它
		 * 距下边界无穷远。
		 * 
		 */
		return ((value->infinite && value->lower) ? 0.0 : 1.0);
	}
	else if (!hist1->infinite && hist2->infinite)
	{
		/* 同上，但方向相反 */
		return ((value->infinite && !value->lower) ? 1.0 : 0.0);
	}
	else
	{
		/*
		 * 若两箱边界均无限，应彼此相等，值应无限并等
		 * 于两界。（但勿断言，免得用户创建的类型带
		 * 有损坏的比较函数时崩溃）。
		 * 
		 *
		 * 假定值位于无限边界的中间。
		 */
		return 0.5;
	}
}


/*
 * 获取值在长度直方图箱内 [0,1] 范围中的相对位置。
 */
static double
get_len_position(double value, double hist1, double hist2)
{
	if (!isinf(hist1) && !isinf(hist2))
	{
		/*
		 * 两边界都有限。值也应有限，因其位于两界之间。
		 * 若不是这样，随便返回一个值即可。
		 * 
		 */
		if (isinf(value))
			return 0.5;

		return 1.0 - (hist2 - value) / (hist2 - hist1);
	}
	else if (isinf(hist1) && !isinf(hist2))
	{
		/*
		 * 下边界为 -inf，上边界有限。返回 1.0 表示值距
		 * 下边界无穷远。
		 */
		return 1.0;
	}
	else if (isinf(hist1) && isinf(hist2))
	{
		/* 同上，但方向相反 */
		return 0.0;
	}
	else
	{
		/*
		 * 若两箱边界均无限，应彼此相等，值应无限并等
		 * 于两界。（但别断言，免得调用失误而无谓崩
		 * 溃）
		 * 
		 *
		 * 假定值位于无限边界的中间。
		 */
		return 0.5;
	}
}

/*
 * 测量两个范围边界之间的距离。
 */
static float8
get_distance(TypeCacheEntry *typcache, const RangeBound *bound1, const RangeBound *bound2)
{
	bool		has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);

	if (!bound1->infinite && !bound2->infinite)
	{
		/*
		 * 两边界都非无限：用 subdiff 函数；若无 subdiff 可
		 * 用则返回默认值 1.0。
		 */
		if (has_subdiff)
		{
			float8		res;

			res = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
												   typcache->rng_collation,
												   bound2->val,
												   bound1->val));
			/* 拒绝可能的 NaN 结果与负结果 */
			if (isnan(res) || res < 0.0)
				return 1.0;
			else
				return res;
		}
		else
			return 1.0;
	}
	else if (bound1->infinite && bound2->infinite)
	{
		/* 两边界都是无限的 */
		if (bound1->lower == bound2->lower)
			return 0.0;
		else
			return get_float8_infinity();
	}
	else
	{
		/* 一边界无限，另一边界有限 */
		return get_float8_infinity();
	}
}

/*
 * 计算函数 P(x) 在区间 [length1, length2] 上的平均
 * 值，其中 P(x) 是长度小于 x（若 'equal' 为真则小
 * 于等于 x）。
 */
static double
calc_length_hist_frac(Datum *length_hist_values, int length_hist_nvalues,
					  double length1, double length2, bool equal)
{
	double		frac;
	double		A,
				B,
				PA,
				PB;
	double		pos;
	int			i;
	double		area;

	Assert(length2 >= length1);

	if (length2 < 0.0)
		return 0.0;				/* 不应发生，但检查一下也无妨 */

	/* 表中所有长度都 <= 无限。 */
	if (isinf(length2) && equal)
		return 1.0;

	/*----------
	 * A 与 B 之间函数的平均值可用如下公式计算：
	 * 
	 *
	 *			B
	 *	  1		/
	 * -------	| P(x)dx
	 *	B - A	/
	 *			A
	 *
	 * 积分的几何解释是 P(x) 图下面积。P(x) 由长度直
	 * 方图定义。我们分段计算面积，遍历长度直方图
	 * 的各个箱。每个箱都是一个梯形：
	 * 
	 *
	 *		 P(x2)
	 *		  /|
	 *		 / |
	 * P(x1)/  |
	 *	   |   |
	 *	   |   |
	 *	---+---+--
	 *	   x1  x2
	 *
	 * 其中 x1 与 x2 是当前直方图的边界，P(x1) 与 P(x1)
	 * 是边界处的元组累计占比。
	 *
	 * 每个梯形的面积为 1/2 * (P(x2) + P(x1)) * (x2 - x1)
	 *
	 * 首个箱包含调用方传入的下界，因此要在前一个与
	 * 下一个直方图箱边界之间做线性插值来算 P(x1)。最
	 * 后一个箱同理：我们用线性插值计算 P(x2)。对中间
	 * 的箱，x1 与 x2 落在直方图箱边界上，因此 P(x1) 与
	 * P(x2) 简单地为：
	 * P(x1) =	  (bin index) / (number of bins)
	 * P(x2) = (bin index + 1 / (number of bins)
	 */

	/* 第一个箱，即含下界的那个 */
	i = length_hist_bsearch(length_hist_values, length_hist_nvalues, length1, equal);
	if (i >= length_hist_nvalues - 1)
		return 1.0;

	if (i < 0)
	{
		i = 0;
		pos = 0.0;
	}
	else
	{
		/* 在箱内插值 length1 的位置 */
		pos = get_len_position(length1,
							   DatumGetFloat8(length_hist_values[i]),
							   DatumGetFloat8(length_hist_values[i + 1]));
	}
	PB = (((double) i) + pos) / (double) (length_hist_nvalues - 1);
	B = length1;

	/*
	 * 在 length1 == length2 的退化情形下，直接返回
	 * P(length1)。这不只是优化：若 length1 == length2，
	 * 之后就会除以零。
	 */
	if (length2 == length1)
		return PB;

	/*
	 * 遍历各箱，直到碰到包含上界的最后一个箱（
	 * 若下界与上界同箱，则会立即跳出。）
	 * 
	 */
	area = 0.0;
	for (; i < length_hist_nvalues - 1; i++)
	{
		double		bin_upper = DatumGetFloat8(length_hist_values[i + 1]);

		/* 查是否已到最后一个箱 */
		if (!(bin_upper < length2 || (equal && bin_upper <= length2)))
			break;

		/* 前一箱的上界即本箱的下界 */
		A = B;
		PA = PB;

		B = bin_upper;
		PB = (double) i / (double) (length_hist_nvalues - 1);

		/*
		 * 将此梯形面积累加到总和。该 if 判断是为了避
		 * 免 NaN——极端情况下 PA == PB == 0 且 B - A ==
		 * Inf。零高梯形（PA == PB == 0）的面积为零，与宽
		 * 度 (B - A) 无关。
		 */
		if (PA > 0 || PB > 0)
			area += 0.5 * (PB + PA) * (B - A);
	}

	/* 末箱 */
	A = B;
	PA = PB;

	B = length2;				/* 最后箱终止于查询上界 */
	if (i >= length_hist_nvalues - 1)
		pos = 0.0;
	else
	{
		if (DatumGetFloat8(length_hist_values[i]) == DatumGetFloat8(length_hist_values[i + 1]))
			pos = 0.0;
		else
			pos = get_len_position(length2,
								   DatumGetFloat8(length_hist_values[i]),
								   DatumGetFloat8(length_hist_values[i + 1]));
	}
	PB = (((double) i) + pos) / (double) (length_hist_nvalues - 1);

	if (PA > 0 || PB > 0)
		area += 0.5 * (PB + PA) * (B - A);

	/*
	 * 好，我们已经算出了面积，即积分值。除以宽度即得
	 * 所求的平均值。
	 *
	 * 避免无穷除以无穷产生的 NaN。若 length2 为无穷则
	 * 至少会发生这种情况。不清楚此时正确值应是多少，
	 * 所以 0.5 与任何值一样好。
	 */
	if (isinf(area) && isinf(length2))
		frac = 0.5;
	else
		frac = area / (length2 - length1);

	return frac;
}

/*
 * 计算 "var <@ const" 算子的选择率，即估算落在常量
 * 下界与上界之内的多范围占比。这用到范围下界直方
 * 图与范围长度直方图，并假定长度与下界相互独立。
 * 
 *
 * 调用方已检查过常量下界与上界都是有限的。
 * 
 */
static double
calc_hist_selectivity_contained(TypeCacheEntry *typcache,
								const RangeBound *lower, RangeBound *upper,
								const RangeBound *hist_lower, int hist_nvalues,
								Datum *length_hist_values, int length_hist_nvalues)
{
	int			i,
				upper_index;
	float8		prev_dist;
	double		bin_width;
	double		upper_bin_width;
	double		sum_frac;

	/*
	 * 先在下界直方图中找包含上界的箱。下界大于常量
	 * 上界的范围都不可能匹配，即大于 upper_index
	 * 的箱中没有匹配。
	 */
	upper->inclusive = !upper->inclusive;
	upper->lower = true;
	upper_index = rbound_bsearch(typcache, upper, hist_lower, hist_nvalues,
								 false);

	/*
	 * 若上界值低于直方图下限，则没有匹配。
	 * 
	 */
	if (upper_index < 0)
		return 0.0;

	/*
	 * 若上界值达到或超过直方图上限，就当上界位于最
	 * 后实际箱中那样开始循环；反正 get_position 会把结果
	 * 钳制为 1.0。（这相当于假定直方图上限之上的数
	 * 据群体为空，与我们刚才对下限的假定相同。）
	 * 
	 * 
	 */
	upper_index = Min(upper_index, hist_nvalues - 2);

	/*
	 * 计算 upper_bin_width，即 (upper_index, upper_index
	 *  + 1) 箱中大于查询范围上界的那部分占比，使用
	 * subdiff 函数的线性插值。
	 */
	upper_bin_width = get_position(typcache, upper,
								   &hist_lower[upper_index],
								   &hist_lower[upper_index + 1]);

	/*
	 * 在循环中，dist 与 prev_dist 是"当前"箱的下界和上
	 * 界到常量上界的距离。
	 *
	 * bin_width 表示当前箱的宽度。正常情况下为 1.0，表
	 * 示满宽箱，但极端情况下可更小：循环的起点与终
	 * 点。我们以 bin_width = upper_bin_width 开始，因为
	 * 我们从包含上界的箱开始。
	 */
	prev_dist = 0.0;
	bin_width = upper_bin_width;

	sum_frac = 0.0;
	for (i = upper_index; i >= 0; i--)
	{
		double		dist;
		double		length_hist_frac;
		bool		final_bin = false;

		/*
		 * dist -- 从查询范围上界到下界直方图中当前箱下界
		 * 的距离；若此为含常量下界的末箱，则是到常量范
		 * 围下界的距离。
		 * 
		 */
		if (range_cmp_bounds(typcache, &hist_lower[i], lower) < 0)
		{
			dist = get_distance(typcache, lower, upper);

			/*
			 * 把 bin_width 减去本箱内要忽略的部分。
			 * 
			 */
			bin_width -= get_position(typcache, lower, &hist_lower[i],
									  &hist_lower[i + 1]);
			if (bin_width < 0.0)
				bin_width = 0.0;
			final_bin = true;
		}
		else
			dist = get_distance(typcache, &hist_lower[i], upper);

		/*
		 * 估算本箱内窄得不超过查询范围上界距离的元组占
		 * 比。
		 */
		length_hist_frac = calc_length_hist_frac(length_hist_values,
												 length_hist_nvalues,
												 prev_dist, dist, true);

		/*
		 * 把本箱中长度合适的元组占比累加到总量上。
		 * 
		 */
		sum_frac += length_hist_frac * bin_width / (double) (hist_nvalues - 1);

		if (final_bin)
			break;

		bin_width = 1.0;
		prev_dist = dist;
	}

	return sum_frac;
}

/*
 * 计算 "var @> const" 算子的选择率，即估算包含常量
 * 下界与上界的多范围占比。这用到范围下界直方图
 * 与范围长度直方图，并假定长度与下界相互独立。
 * 
 */
static double
calc_hist_selectivity_contains(TypeCacheEntry *typcache,
							   const RangeBound *lower, const RangeBound *upper,
							   const RangeBound *hist_lower, int hist_nvalues,
							   Datum *length_hist_values, int length_hist_nvalues)
{
	int			i,
				lower_index;
	double		bin_width,
				lower_bin_width;
	double		sum_frac;
	float8		prev_dist;

	/* 找到包含查询范围下界的箱。 */
	lower_index = rbound_bsearch(typcache, lower, hist_lower, hist_nvalues,
								 true);

	/*
	 * 若下界值低于直方图下限，则没有匹配。
	 * 
	 */
	if (lower_index < 0)
		return 0.0;

	/*
	 * 若下界值达到或超过直方图上限，就当下界位于
	 * 最后实际箱中那样开始循环；反正 get_position 会把
	 * 钳制为 1.0。（这相当于假定直方图上限之上的数
	 * 据群体为空，与我们刚才对下限的假定相同。）
	 * 
	 * 
	 */
	lower_index = Min(lower_index, hist_nvalues - 2);

	/*
	 * 计算 lower_bin_width，即 (lower_index, lower_index
	 *  + 1) 箱中大于查询范围下界的那部分占比，使用
	 * subdiff 函数的线性插值。
	 */
	lower_bin_width = get_position(typcache, lower, &hist_lower[lower_index],
								   &hist_lower[lower_index + 1]);

	/*
	 * 遍历所有小于查询下界的下界箱。循环中，dist 与
	 * prev_dist 是"当前"箱上/下界到常量上界的距离。
	 * 我们从查询下界出发往回走，因此第一个箱的上界就
	 * 是查询下界，其到查询上界的距离即查询范围长度
	 * 
	 * 
	 *
	 * bin_width 表示当前箱的宽度。正常情况下为 1.0，除
	 * 了第一箱之外——第一箱只计到常量下界为止。
	 * 
	 */
	prev_dist = get_distance(typcache, lower, upper);
	sum_frac = 0.0;
	bin_width = lower_bin_width;
	for (i = lower_index; i >= 0; i--)
	{
		float8		dist;
		double		length_hist_frac;

		/*
		 * dist -- 从查询范围上界到下界直方图当前值或查询
		 * 范围下界（若已到达）的距离。
		 * 
		 */
		dist = get_distance(typcache, &hist_lower[i], upper);

		/*
		 * 取长度直方图中覆盖比到查询上界距离更长（
		 * 或相等）的区间的平均占比。
		 */
		length_hist_frac =
			1.0 - calc_length_hist_frac(length_hist_values,
										length_hist_nvalues,
										prev_dist, dist, false);

		sum_frac += length_hist_frac * bin_width / (double) (hist_nvalues - 1);

		bin_width = 1.0;
		prev_dist = dist;
	}

	return sum_frac;
}
