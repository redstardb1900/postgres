/*-------------------------------------------------------------------------
 *
 * array_selfuncs.c
 *	  数组算子选择率估算函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"


/* "@>" 与 "<@" 算子的默认选择率常数 */
#define DEFAULT_CONTAIN_SEL 0.005

/* "&&" 算子的默认选择率常数 */
#define DEFAULT_OVERLAP_SEL 0.01

/* 指定算子的默认选择率 */
#define DEFAULT_SEL(operator) \
	((operator) == OID_ARRAY_OVERLAP_OP ? \
		DEFAULT_OVERLAP_SEL : DEFAULT_CONTAIN_SEL)

static Selectivity calc_arraycontsel(VariableStatData *vardata, Datum constval,
									 Oid elemtype, Oid operator);
static Selectivity mcelem_array_selec(ArrayType *array,
									  TypeCacheEntry *typentry,
									  Datum *mcelem, int nmcelem,
									  float4 *numbers, int nnumbers,
									  float4 *hist, int nhist,
									  Oid operator);
static Selectivity mcelem_array_contain_overlap_selec(Datum *mcelem, int nmcelem,
													  float4 *numbers, int nnumbers,
													  Datum *array_data, int nitems,
													  Oid operator, TypeCacheEntry *typentry);
static Selectivity mcelem_array_contained_selec(Datum *mcelem, int nmcelem,
												float4 *numbers, int nnumbers,
												Datum *array_data, int nitems,
												float4 *hist, int nhist,
												Oid operator, TypeCacheEntry *typentry);
static float *calc_hist(const float4 *hist, int nhist, int n);
static float *calc_distr(const float *p, int n, int m, float rest);
static int	floor_log2(uint32 n);
static bool find_next_mcelem(Datum *mcelem, int nmcelem, Datum value,
							 int *index, TypeCacheEntry *typentry);
static int	element_compare(const void *key1, const void *key2, void *arg);
static int	float_compare_desc(const void *key1, const void *key2);


/*
 * scalararraysel_containment
 *		经数组包含来估算 ScalarArrayOpExpr 的选择率。
 *
 * 若有 const =/<> ANY/ALL (array_var)，我们可以把选择
 * 率估算为一次数组包含算子操作，即 array_var op
 * ARRAY[const]。
 *
 * scalararraysel() 已经验证过 ScalarArrayOpExpr 的算子
 * 就是数组元素类型的默认相等或不相等算子，并且已
 * 将两个输入都激进地简化为常量。
 *
 * 返回选择率（0..1），若无法估算选择率则返回 -1。
 */
Selectivity
scalararraysel_containment(PlannerInfo *root,
						   Node *leftop, Node *rightop,
						   Oid elemtype, bool isEquality, bool useOr,
						   int varRelid)
{
	Selectivity selec;
	VariableStatData vardata;
	Datum		constval;
	TypeCacheEntry *typentry;
	FmgrInfo   *cmpfunc;

	/*
	 * rightop 若非变量则弃。
	 */
	examine_variable(root, rightop, varRelid, &vardata);
	if (!vardata.rel)
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}

	/*
	 * leftop 若非常量则弃。
	 */
	if (!IsA(leftop, Const))
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}
	if (((Const *) leftop)->constisnull)
	{
		/* 左侧为 NULL 则必不成立 */
		ReleaseVariableStats(vardata);
		return (Selectivity) 0.0;
	}
	constval = ((Const *) leftop)->constvalue;

	/* 获取元素类型的默认比较函数 */
	typentry = lookup_type_cache(elemtype, TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
	{
		ReleaseVariableStats(vardata);
		return -1.0;
	}
	cmpfunc = &typentry->cmp_proc_finfo;

	/*
	 * 若算子为 <>，则交换 ANY/ALL，稍后反转结果。
	 */
	if (!isEquality)
		useOr = !useOr;

	/* 获取 var 的数组元素统计（若有） */
	if (HeapTupleIsValid(vardata.statsTuple) &&
		statistic_proc_security_check(&vardata, cmpfunc->fn_oid))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;
		AttStatsSlot hslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);

		/* MCELEM 将是与元素同类型的数组 */
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			/* 对 ALL 情形，还需获取不同元素计数的直方图 */
			if (useOr ||
				!get_attstatsslot(&hslot, vardata.statsTuple,
								  STATISTIC_KIND_DECHIST, InvalidOid,
								  ATTSTATSSLOT_NUMBERS))
				memset(&hslot, 0, sizeof(hslot));

			/*
			 * = ANY 按 var @> ARRAY[const] 估算。
			 *
			 * = ALL 按 var <@ ARRAY[const] 估算。
			 */
			if (useOr)
				selec = mcelem_array_contain_overlap_selec(sslot.values,
														   sslot.nvalues,
														   sslot.numbers,
														   sslot.nnumbers,
														   &constval, 1,
														   OID_ARRAY_CONTAINS_OP,
														   typentry);
			else
				selec = mcelem_array_contained_selec(sslot.values,
													 sslot.nvalues,
													 sslot.numbers,
													 sslot.nnumbers,
													 &constval, 1,
													 hslot.numbers,
													 hslot.nnumbers,
													 OID_ARRAY_CONTAINED_OP,
													 typentry);

			free_attstatsslot(&hslot);
			free_attstatsslot(&sslot);
		}
		else
		{
			/* 无最常用元素信息，只能放弃 */
			if (useOr)
				selec = mcelem_array_contain_overlap_selec(NULL, 0,
														   NULL, 0,
														   &constval, 1,
														   OID_ARRAY_CONTAINS_OP,
														   typentry);
			else
				selec = mcelem_array_contained_selec(NULL, 0,
													 NULL, 0,
													 &constval, 1,
													 NULL, 0,
													 OID_ARRAY_CONTAINED_OP,
													 typentry);
		}

		/*
		 * MCE 统计只数非空行，所以要按空行调整。
		 */
		selec *= (1.0 - stats->stanullfrac);
	}
	else
	{
		/* 全无统计，只好放弃 */
		if (useOr)
			selec = mcelem_array_contain_overlap_selec(NULL, 0,
													   NULL, 0,
													   &constval, 1,
													   OID_ARRAY_CONTAINS_OP,
													   typentry);
		else
			selec = mcelem_array_contained_selec(NULL, 0,
												 NULL, 0,
												 &constval, 1,
												 NULL, 0,
												 OID_ARRAY_CONTAINED_OP,
												 typentry);
		/* 这里假定无空值，不需 stanullfrac 校正 */
	}

	ReleaseVariableStats(vardata);

	/*
	 * 若算子为 <>，则反转结果。
	 */
	if (!isEquality)
		selec = 1.0 - selec;

	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * arraycontsel -- 数组 @>、&&、<@ 算子的限制选择率
 */
Datum
arraycontsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	Oid			element_typeid;

	/*
	 * 若表达式不是（变量 op 某物）或（某物 op
	 * 变量），则放弃并返回默认估算值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));

	/*
	 * 若另一侧也不是常量，那就没什么有用的可做了。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	/*
	 * "&&"、"@>" 与 "<@" 算子都是严格算子，因此可以直接
	 * 处理 NULL 常量。
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * 若 var 在右侧，则交换算子，以便此后可假定 var 位
	 * 于左侧。
	 */
	if (!varonleft)
	{
		if (operator == OID_ARRAY_CONTAINS_OP)
			operator = OID_ARRAY_CONTAINED_OP;
		else if (operator == OID_ARRAY_CONTAINED_OP)
			operator = OID_ARRAY_CONTAINS_OP;
	}

	/*
	 * 好了，这里要处理的是一个 Var 与一个 Const。我们
	 * 需要 Const 是与列具有相同元素类型的数组，否则我们
	 * 无能为力。（这类情况多半运行时会失败，但此处
	 * 更愿意直接返回默认估算值。）
	 */
	element_typeid = get_base_element_type(((Const *) other)->consttype);
	if (element_typeid != InvalidOid &&
		element_typeid == get_base_element_type(vardata.vartype))
	{
		selec = calc_arraycontsel(&vardata, ((Const *) other)->constvalue,
								  element_typeid, operator);
	}
	else
	{
		selec = DEFAULT_SEL(operator);
	}

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * arraycontjoinsel -- 数组 @>、&&、<@ 算子的连接选择率
 */
Datum
arraycontjoinsel(PG_FUNCTION_ARGS)
{
	/* 目前这只是一个桩实现 */
	Oid			operator = PG_GETARG_OID(1);

	PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
}

/*
 * 基于统计信息为 "arraycolumn @> const"、"arraycolumn && const"
 * 或 "arraycolumn <@ const" 计算选择率
 *
 * 该函数主要负责提取要用的 pg_statistic 数据；随后把
 * 问题交给 mcelem_array_selec() 处理。
 */
static Selectivity
calc_arraycontsel(VariableStatData *vardata, Datum constval,
				  Oid elemtype, Oid operator)
{
	Selectivity selec;
	TypeCacheEntry *typentry;
	FmgrInfo   *cmpfunc;
	ArrayType  *array;

	/* 获取元素类型的默认比较函数 */
	typentry = lookup_type_cache(elemtype, TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
		return DEFAULT_SEL(operator);
	cmpfunc = &typentry->cmp_proc_finfo;

	/*
	 * 调用方已确认常量为同元素类型的数组，现在就来
	 * 获取它
	 */
	array = DatumGetArrayTypeP(constval);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, cmpfunc->fn_oid))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;
		AttStatsSlot hslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/* MCELEM 将是与列同类型的数组 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			/*
			 * 对 "array <@ const" 还需不同元素计数直方图。
			 *
			 */
			if (operator != OID_ARRAY_CONTAINED_OP ||
				!get_attstatsslot(&hslot, vardata->statsTuple,
								  STATISTIC_KIND_DECHIST, InvalidOid,
								  ATTSTATSSLOT_NUMBERS))
				memset(&hslot, 0, sizeof(hslot));

			/* 为数组 Var 使用最常用元素槽位。 */
			selec = mcelem_array_selec(array, typentry,
									   sslot.values, sslot.nvalues,
									   sslot.numbers, sslot.nnumbers,
									   hslot.numbers, hslot.nnumbers,
									   operator);

			free_attstatsslot(&hslot);
			free_attstatsslot(&sslot);
		}
		else
		{
			/* 无最常用元素信息，只能放弃 */
			selec = mcelem_array_selec(array, typentry,
									   NULL, 0, NULL, 0, NULL, 0,
									   operator);
		}

		/*
		 * MCE 统计只数非空行，所以要按空行调整。
		 */
		selec *= (1.0 - stats->stanullfrac);
	}
	else
	{
		/* 全无统计，只好放弃 */
		selec = mcelem_array_selec(array, typentry,
								   NULL, 0, NULL, 0, NULL, 0,
								   operator);
		/* 这里假定无空值，不需 stanullfrac 校正 */
	}

	/* 若常量被 TOAST，释放我们的副本 */
	if (PointerGetDatum(array) != constval)
		pfree(array);

	return selec;
}

/*
 * 基于最常用元素统计的数组选择率估算
 *
 * 该函数只是解构并排序数组常量的内容，然后把问题
 * 按算子转交给 mcelem_array_contain_overlap_selec 或
 * mcelem_array_contained_selec 处理。
 */
static Selectivity
mcelem_array_selec(ArrayType *array, TypeCacheEntry *typentry,
				   Datum *mcelem, int nmcelem,
				   float4 *numbers, int nnumbers,
				   float4 *hist, int nhist,
				   Oid operator)
{
	Selectivity selec;
	int			num_elems;
	Datum	   *elem_values;
	bool	   *elem_nulls;
	bool		null_present;
	int			nonnull_nitems;
	int			i;

	/*
	 * 为常量数组数据做排序准备。排序能让我们找出唯
	 * 一元素并与 MCELEM 数组高效合并。
	 */
	deconstruct_array(array,
					  typentry->type_id,
					  typentry->typlen,
					  typentry->typbyval,
					  typentry->typalign,
					  &elem_values, &elem_nulls, &num_elems);

	/* 剔除所有空元素 */
	nonnull_nitems = 0;
	null_present = false;
	for (i = 0; i < num_elems; i++)
	{
		if (elem_nulls[i])
			null_present = true;
		else
			elem_values[nonnull_nitems++] = elem_values[i];
	}

	/*
	 * 查询 "column @> '{anything, null}'" 匹配不到任何
	 * 内容。其余两个算子则可忽略常量中的空值。
	 */
	if (null_present && operator == OID_ARRAY_CONTAINS_OP)
	{
		pfree(elem_values);
		pfree(elem_nulls);
		return (Selectivity) 0.0;
	}

	/* 使用默认比较函数对提取出的元素排序。 */
	qsort_arg(elem_values, nonnull_nitems, sizeof(Datum),
			  element_compare, typentry);

	/* 按算子区分各种情形 */
	if (operator == OID_ARRAY_CONTAINS_OP || operator == OID_ARRAY_OVERLAP_OP)
		selec = mcelem_array_contain_overlap_selec(mcelem, nmcelem,
												   numbers, nnumbers,
												   elem_values, nonnull_nitems,
												   operator, typentry);
	else if (operator == OID_ARRAY_CONTAINED_OP)
		selec = mcelem_array_contained_selec(mcelem, nmcelem,
											 numbers, nnumbers,
											 elem_values, nonnull_nitems,
											 hist, nhist,
											 operator, typentry);
	else
	{
		elog(ERROR, "arraycontsel called for unrecognized operator %u",
			 operator);
		selec = 0.0;			/* 让编译器安静 */
	}

	pfree(elem_values);
	pfree(elem_nulls);
	return selec;
}

/*
 * 基于最常用元素统计来估算 "column @> const" 与
 * "column && const" 的选择率。该估算假定元素的出现
 * 相互独立。
 *
 * mcelem（长度 nmcelem）与 numbers（长度 nnumbers）来
 * 自数组列的 MCELEM 统计槽位；若无统计则为 NULL/0。
 * array_data（长度 nitems）是常量的元素。
 *
 * mcelem 与 array_data 两个数组都假定已按元素类型的
 * cmpfunc 预排序。其中不含空元素。
 *
 * TODO：这一估算大概可以通过使用不同元素计数的直
 * 方图来改进。例如，除 "column @> '{}'" 这一特例之外，
 * 我们可以把算得的选择率乘以
 * 该列非空数组的占比。
 */
static Selectivity
mcelem_array_contain_overlap_selec(Datum *mcelem, int nmcelem,
								   float4 *numbers, int nnumbers,
								   Datum *array_data, int nitems,
								   Oid operator, TypeCacheEntry *typentry)
{
	Selectivity selec,
				elem_selec;
	int			mcelem_index,
				i;
	bool		use_bsearch;
	float4		minfreq;

	/*
	 * Numbers 应比 Values 多出三个，因为最后三个单元
	 * 格应存非空元素最小/最大频率及空元素频率。
	 * 数目不对就忽略 Numbers。
	 *
	 */
	if (nnumbers != nmcelem + 3)
	{
		numbers = NULL;
		nnumbers = 0;
	}

	if (numbers)
	{
		/* 取观测到的最小频率 */
		minfreq = numbers[nmcelem];
	}
	else
	{
		/* 没有统计信息时作一些默认假设 */
		minfreq = 2 * (float4) DEFAULT_CONTAIN_SEL;
	}

	/* 决定使用二分查找是否更快。 */
	if (nitems * floor_log2((uint32) nmcelem) < nmcelem + nitems)
		use_bsearch = true;
	else
		use_bsearch = false;

	if (operator == OID_ARRAY_CONTAINS_OP)
	{
		/*
		 * "column @> const" 查询的初始选择率为 1.0，之后会随
		 * 常量数组的每个元素而减小。
		 */
		selec = 1.0;
	}
	else
	{
		/*
		 * "column && const" 查询的初始选择率为 0.0，之后会随
		 * 常量数组的每个元素而增大。
		 */
		selec = 0.0;
	}

	/* 并行扫描 mcelem 与数组。 */
	mcelem_index = 0;
	for (i = 0; i < nitems; i++)
	{
		bool		match = false;

		/* 忽略数组数据中的重复项。 */
		if (i > 0 &&
			element_compare(&array_data[i - 1], &array_data[i], typentry) == 0)
			continue;

		/* 找出 >= 此数组项的最小 MCELEM。 */
		if (use_bsearch)
		{
			match = find_next_mcelem(mcelem, nmcelem, array_data[i],
									 &mcelem_index, typentry);
		}
		else
		{
			while (mcelem_index < nmcelem)
			{
				int			cmp = element_compare(&mcelem[mcelem_index],
												  &array_data[i],
												  typentry);

				if (cmp < 0)
					mcelem_index++;
				else
				{
					if (cmp == 0)
						match = true;	/* 找到 mcelem */
					break;
				}
			}
		}

		if (match && numbers)
		{
			/* MCELEM 匹配该数组项；用其频率。 */
			elem_selec = numbers[mcelem_index];
			mcelem_index++;
		}
		else
		{
			/*
			 * 该元素不在 MCELEM 中则弃权，选择率至
			 * 多为 minfreq / 2。
			 */
			elem_selec = Min(DEFAULT_CONTAIN_SEL, minfreq / 2);
		}

		/*
		 * 以当前元素的选择率、并按元素出现相互独立的假
		 * 定来更新总选择率。
		 */
		if (operator == OID_ARRAY_CONTAINS_OP)
			selec *= elem_selec;
		else
			selec = selec + elem_selec - selec * elem_selec;

		/* 钳制中间结果以防舍入误差致其失控 */
		CLAMP_PROBABILITY(selec);
	}

	return selec;
}

/*
 * 基于最常用元素统计估算 "column <@ const" 的选择
 * 率。
 *
 * mcelem（长度 nmcelem）与 numbers（长度 nnumbers）来
 * 自数组列的 MCELEM 统计槽位；若无统计则为 NULL/0。
 * array_data（长度 nitems）是常量的元素。hist（长度
 * nhist）则来自数组列的 DECHIST 统计槽位；若那些统
 * 计不可用则为 NULL/0。
 *
 * mcelem 与 array_data 两个数组都假定已按元素类型的
 * cmpfunc 预排序。其中不含空元素。
 *
 * 元素独立出现意味着匹配行中不同元素计数会服从
 * 一种特定分布。真实数据通常推翻这一假设。例如，在
 * 由 11 元素整数数组构成的集合中，若元素取值范围为
 * [0..10]，各元素的出现通常并不独立。果真如此，一
 * 个足够大的集合就会包含 0 到 11 全部元素计数。
 * 我们用
 * 不同元素计数直方图校正。
 *
 * 在 "column @> const" 与 "column && const" 情形下，常
 * 量 "const" 的元素数通常很少（否则选择率会分别接
 * 近 0 或 1），故不同元素计数分布相关的依赖效应在
 * 那里可忽略。"column <@ const" 元素数
 * 通常很多（否则选择率会接近 0）。因此这里应当用
 * 数组的不同元素计数分布来做
 * 校正。
 *
 * 用不同元素计数直方图得到的分布规律，不同于元
 * 素独立时的分布规律。该
 * 分布规律可描述如下：
 *
 * P(o1, o2, ..., on) = f1^o1 * (1 - f1)^(1 - o1) * f2^o2 *
 *	  (1 - f2)^(1 - o2) * ... * fn^on * (1 - fn)^(1 - on) * hist[m] / ind[m]
 *
 * where:
 * o1, o2, ..., on - 第 1, 2, ..., n 个元素的出现
 *		（行中出现为 1，未出现为 0）
 * f1, f2, ..., fn - 第 1, 2, ..., n 个元素的频率
 *		（按收集的统计为 [0..1] 内的标量值）
 * m = o1 + o2 + ... + on = 行中不同元素总数
 * hist[m] - 出现 m 个元素的直方图数据。
 * ind[m] - 假定事件概率同元素频率时，n 事
 *	  件中出现 m 次的概率。
 *
 * ind[m] = sum(f1^o1 * (1 - f1)^(1 - o1) * f2^o2 * (1 - f2)^(1 - o2) *
 * ... * fn^on * (1 - fn)^(1 - on), o1, o2, ..., on) | o1 + o2 + .. on = m
 */
static Selectivity
mcelem_array_contained_selec(Datum *mcelem, int nmcelem,
							 float4 *numbers, int nnumbers,
							 Datum *array_data, int nitems,
							 float4 *hist, int nhist,
							 Oid operator, TypeCacheEntry *typentry)
{
	int			mcelem_index,
				i,
				unique_nitems = 0;
	float		selec,
				minfreq,
				nullelem_freq;
	float	   *dist,
			   *mcelem_dist,
			   *hist_part;
	float		avg_count,
				mult,
				rest;
	float	   *elem_selec;

	/*
	 * MCELEM 槽位中 Numbers 应比 Values 多三个，因为最后
	 * 三个单元格应存放非空元素中的最小与最大频率，再
	 * 存放空元素频率。若不对则只好放弃，因为没有元素
	 * 频率就寸步难行。
	 */
	if (numbers == NULL || nnumbers != nmcelem + 3)
		return DEFAULT_CONTAIN_SEL;

	/* 没有计数直方图同样寸步难行 */
	if (hist == NULL || nhist < 3)
		return DEFAULT_CONTAIN_SEL;

	/*
	 * 提取 compute_array_stats() 存储的部分汇总统计：最
	 * 小频率、空元素频率以及平均不同元素计数。
	 *
	 */
	minfreq = numbers[nmcelem];
	nullelem_freq = numbers[nmcelem + 2];
	avg_count = hist[nhist - 1];

	/*
	 * "rest" 为未出现在 MCELEM 中的元素频率之和。
	 * 平均不同元素计数是*全部*元素的频率之和。先从
	 * 它开始；随后再减去
	 * MCELEM 的频率。
	 */
	rest = avg_count;

	/*
	 * mult 为一乘数，表示常量中没有的每个 mcelem 不出
	 * 现的概率估计。
	 */
	mult = 1.0f;

	/*
	 * elem_selec 是常量中各元素估计频率组成的数组。
	 *
	 */
	elem_selec = (float *) palloc(sizeof(float) * nitems);

	/* 并行扫描 mcelem 与数组。 */
	mcelem_index = 0;
	for (i = 0; i < nitems; i++)
	{
		bool		match = false;

		/* 忽略数组数据中的重复项。 */
		if (i > 0 &&
			element_compare(&array_data[i - 1], &array_data[i], typentry) == 0)
			continue;

		/*
		 * 遍历 MCELEM，直到发现 >= 常量此元素的条目。对跳
		 * 过的 mcelem 条目更新 "rest" 与 "mult"。
		 *
		 */
		while (mcelem_index < nmcelem)
		{
			int			cmp = element_compare(&mcelem[mcelem_index],
											  &array_data[i],
											  typentry);

			if (cmp < 0)
			{
				mult *= (1.0f - numbers[mcelem_index]);
				rest -= numbers[mcelem_index];
				mcelem_index++;
			}
			else
			{
				if (cmp == 0)
					match = true;	/* 找到 mcelem */
				break;
			}
		}

		if (match)
		{
			/* MCELEM 与此项匹配。 */
			elem_selec[unique_nitems] = numbers[mcelem_index];
			/* "rest" 依是否匹配为各 mcelem 递减 */
			rest -= numbers[mcelem_index];
			mcelem_index++;
		}
		else
		{
			/*
			 * 该元素不在 MCELEM 中则弃权，选择率至
			 * 多为 minfreq / 2。
			 */
			elem_selec[unique_nitems] = Min(DEFAULT_CONTAIN_SEL,
											minfreq / 2);
		}

		unique_nitems++;
	}

	/*
	 * 若未耗尽 MCELEM 数组就处理完全部常量元素，则
	 * 继续走完它，以完成 "rest" 与 "mult" 的计算。
	 */
	while (mcelem_index < nmcelem)
	{
		mult *= (1.0f - numbers[mcelem_index]);
		rest -= numbers[mcelem_index];
		mcelem_index++;
	}

	/*
	 * 大量稀有元素会显著拉低选择率。用泊松分布
	 * 估算列值中此类元素零出现的概率。
	 * "rest" 的定义见上文。
	 *
	 */
	mult *= exp(-rest);

	/*----------
	 * 使用不同元素计数直方图需要
	 *		O(unique_nitems * (nmcelem + unique_nitems))
	 * 次运算。成本超阈值后，牺牲精度换时间是
	 * 理的。我们把运算次数限制为 EFFORT * nmcelem；由于
	 * nmcelem 受列的统计目标限制，所做的工作量可由用户
	 * 控制。
	 *
	 * 若运算次数会过大，可以在不损失全部精度的前
	 * 提下缩小 unique_nitems，只考虑常量数组的最常用元
	 * 素。要与起初仅用这些元素的结果一致，就
	 * 必须从 "mult" 中除去被丢弃元素的频率。但这
	 * 只是近似，所以我们不费这个事。因此对
	 * elem_selec[] 做 qsort 取最大的一些元素即可。（
	 * 它们将不再与 array_data[] 的元素对应，但我们并不
	 * 在乎。）
	 *
	 *----------
	 */
#define EFFORT 100

	if ((nmcelem + unique_nitems) > 0 &&
		unique_nitems > EFFORT * nmcelem / (nmcelem + unique_nitems))
	{
		/*
		 * 用二次公式解最大允许 N。A=1，B=nmcelem，C=
		 *  - EFFORT * nmcelem。
		 */
		double		b = (double) nmcelem;
		int			n;

		n = (int) ((sqrt(b * b + 4 * EFFORT * b) - b) / 2);

		/* 排序，然后只取前 n 个元素 */
		qsort(elem_selec, unique_nitems, sizeof(float),
			  float_compare_desc);
		unique_nitems = n;
	}

	/*
	 * 为 mcelem 与常量元素分别计算各不同元素计数的概
	 * 率。此处假定元素的出现相互独立。
	 *
	 */
	dist = calc_distr(elem_selec, unique_nitems, unique_nitems, 0.0f);
	mcelem_dist = calc_distr(numbers, nmcelem, unique_nitems, rest);

	/* 忽略 hist[nhist-1]，它是平均值而非直方图成员 */
	hist_part = calc_hist(hist, nhist - 1, unique_nitems);

	selec = 0.0f;
	for (i = 0; i <= unique_nitems; i++)
	{
		/*
		 * mult * dist[i] / mcelem_dist[i] 给出的是在元素出现
		 * 相互独立的假设下、且不同元素计数为 i 时条件匹
		 * 配的概率。
		 */
		if (mcelem_dist[i] > 0)
			selec += hist_part[i] * mult * dist[i] / mcelem_dist[i];
	}

	pfree(dist);
	pfree(mcelem_dist);
	pfree(hist_part);
	pfree(elem_selec);

	/* 把 NULL 元素的出现也计入。 */
	selec *= (1.0f - nullelem_freq);

	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * 用不同元素计数直方图算出前
 * n 个不同元素计数的概率。
 *
 * 返回 palloc 出的 n+1 项数组，array[k] 为元素计数
 * 为 k 的概率，k 属于 [0..n]。
 *
 * 我们假定边界为 a 和 b 的直方图箱给 (a,b) 内每个值
 * 赋予 1 / ((b - a + 1) * (nhist - 1)) 的概率，并给 a 和
 * b 本身额外一半。
 */
static float *
calc_hist(const float4 *hist, int nhist, int n)
{
	float	   *hist_part;
	int			k,
				i = 0;
	float		prev_interval = 0,
				next_interval;
	float		frac;

	hist_part = (float *) palloc((n + 1) * sizeof(float));

	/*
	 * frac 是直方图各值之间每个区间的概率贡献。我们共
	 * 有 nhist - 1 个区间，因此每个区间的贡献为
	 * 1 / (nhist - 1)。
	 */
	frac = 1.0f / ((float) (nhist - 1));

	for (k = 0; k <= n; k++)
	{
		int			count = 0;

		/*
		 * 数一数等于 k 的直方图边界。（虽然直方图理论应
		 * 只含精确整数，条目是浮点，大值或有舍
		 * 入误差。任何带小数的值都视为等于下一个更大
		 * 的 k。）
		 */
		while (i < nhist && hist[i] <= k)
		{
			count++;
			i++;
		}

		if (count > 0)
		{
			/* k 为至少一个箱的精确边界。 */
			float		val;

			/* 求当前直方图值与下一个之间的长度 */
			if (i < nhist)
				next_interval = hist[i] - hist[i - 1];
			else
				next_interval = 0;

			/*
			 * count - 1 个箱恰含 k。它们共贡献
			 *  (count - 1) * frac 的概率。两侧的部分箱
			 * 也要计入在内。
			 */
			val = (float) (count - 1);
			if (next_interval > 0)
				val += 0.5f / next_interval;
			if (prev_interval > 0)
				val += 0.5f / prev_interval;
			hist_part[k] = frac * val;

			prev_interval = next_interval;
		}
		else
		{
			/* k 不是任何直方图的精确边界。 */
			if (prev_interval > 0)
				hist_part[k] = frac / prev_interval;
			else
				hist_part[k] = 0.0f;
		}
	}

	return hist_part;
}

/*
 * 设有 n 个独立事件，概率为 p[]。本函数计算事件恰
 * 好发生 k 次的概率，k 属于 [0..m]。返回一个大小为
 * m+1 的 palloc 数组。
 *
 * "rest" 是所有未计入 p 的低概率事件的概率之和。
 *
 *
 * 设想一个大小为 (n + 1) x (m + 1) 的矩阵 M。元素
 * M[i,j] 表示前 i 个事件中恰好发生 j 个的概率。显
 * 然 M[0,0] = 1。对任意固定的 j，i 每增 1 概率上升
 * 当且仅当事件发生。由全概率公式：
 *	M[i,j] = M[i - 1, j] * (1 - p[i]) + M[i - 1, j - 1] * p[i]
 *		for i > 0, j > 0.
 *	M[i,0] = M[i - 1, 0] * (1 - p[i]) for i > 0.
 */
static float *
calc_distr(const float *p, int n, int m, float rest)
{
	float	   *row,
			   *prev_row,
			   *tmp;
	int			i,
				j;

	/*
	 * 因只返回最后一行，计算只需当前行与上一
	 * 行，所以分配两行即可。
	 */
	row = (float *) palloc((m + 1) * sizeof(float));
	prev_row = (float *) palloc((m + 1) * sizeof(float));

	/* M[0,0] = 1 */
	row[0] = 1.0f;
	for (i = 1; i <= n; i++)
	{
		float		t = p[i - 1];

		/* 对调 */
		tmp = row;
		row = prev_row;
		prev_row = tmp;

		/* 计算下一行 */
		for (j = 0; j <= i && j <= m; j++)
		{
			float		val = 0.0f;

			if (j < i)
				val += prev_row[j] * (1.0f - t);
			if (j > 0)
				val += prev_row[j - 1] * t;
			row[j] = val;
		}
	}

	/*
	 * 大量不在 "p" 中的稀有元素会显著降低选择率。用
	 * 泊松分布来建模它们的集体出
	 * 现。
	 */
	if (rest > DEFAULT_CONTAIN_SEL)
	{
		float		t;

		/* 对调 */
		tmp = row;
		row = prev_row;
		prev_row = tmp;

		for (i = 0; i <= m; i++)
			row[i] = 0.0f;

		/* 0 次出现的泊松分布值 */
		t = exp(-rest);

		/*
		 * 计算先前算得的分布与泊松
		 * 分布的卷积。
		 */
		for (i = 0; i <= m; i++)
		{
			for (j = 0; j <= m - i; j++)
				row[j + i] += prev_row[j] * t;

			/* 获取 (i + 1) 次出现的泊松分布值 */
			t *= rest / (float) (i + 1);
		}
	}

	pfree(prev_row);
	return row;
}

/* 用于计算以 2 为底对数下取整的快速函数。 */
static int
floor_log2(uint32 n)
{
	int			logval = 0;

	if (n == 0)
		return -1;
	if (n >= (1 << 16))
	{
		n >>= 16;
		logval += 16;
	}
	if (n >= (1 << 8))
	{
		n >>= 8;
		logval += 8;
	}
	if (n >= (1 << 4))
	{
		n >>= 4;
		logval += 4;
	}
	if (n >= (1 << 2))
	{
		n >>= 2;
		logval += 2;
	}
	if (n >= (1 << 1))
	{
		logval += 1;
	}
	return logval;
}

/*
 * find_next_mcelem 对最常用元素数组执行二分查找，
 * 从 *index 开始寻找第一个 >= value 的成员。它会把
 * 匹配位置存入 *index，若是精确匹配则返回 true。
 * 注：假定 mcelem 元素互异，故不会有多个精确匹配。
 *
 */
static bool
find_next_mcelem(Datum *mcelem, int nmcelem, Datum value, int *index,
				 TypeCacheEntry *typentry)
{
	int			l = *index,
				r = nmcelem - 1,
				i,
				res;

	while (l <= r)
	{
		i = (l + r) / 2;
		res = element_compare(&mcelem[i], &value, typentry);
		if (res == 0)
		{
			*index = i;
			return true;
		}
		else if (res < 0)
			l = i + 1;
		else
			r = i - 1;
	}
	*index = l;
	return false;
}

/*
 * 元素比较函数。
 *
 * 我们使用元素类型的默认 btree 操作符类；若类型区
 * 分排序则用其默认排序。
 *
 * XXX：考虑使用 SortSupport 基础设施
 */
static int
element_compare(const void *key1, const void *key2, void *arg)
{
	Datum		d1 = *((const Datum *) key1);
	Datum		d2 = *((const Datum *) key2);
	TypeCacheEntry *typentry = (TypeCacheEntry *) arg;
	FmgrInfo   *cmpfunc = &typentry->cmp_proc_finfo;
	Datum		c;

	c = FunctionCall2Coll(cmpfunc, typentry->typcollation, d1, d2);
	return DatumGetInt32(c);
}

/*
 * 将浮点数按降序排序的比较函数。
 */
static int
float_compare_desc(const void *key1, const void *key2)
{
	float		d1 = *((const float *) key1);
	float		d2 = *((const float *) key2);

	if (d1 > d2)
		return -1;
	else if (d1 < d2)
		return 1;
	else
		return 0;
}
