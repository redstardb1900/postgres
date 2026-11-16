/*-------------------------------------------------------------------------
 *
 * network_selfuncs.c
 *	  inet/cidr 算子选择率估算函数
 *
 * 本模块为子网包含与重叠算子提供选择率估算。
 * 估算基于 NULL 比例、最常用值（MCV）以及
 * inet/cidr 列的直方图。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/* inet 重叠算子的默认选择率 */
#define DEFAULT_OVERLAP_SEL 0.01

/* 各种包含算子的默认选择率 */
#define DEFAULT_INCLUSION_SEL 0.005

/* 指定算子的默认选择率 */
#define DEFAULT_SEL(operator) \
	((operator) == OID_INET_OVERLAP_OP ? \
	 DEFAULT_OVERLAP_SEL : DEFAULT_INCLUSION_SEL)

/* 连接选择率计算中考虑的条目数上限 */
#define MAX_CONSIDERED_ELEMS 1024

static Selectivity networkjoinsel_inner(Oid operator,
										VariableStatData *vardata1, VariableStatData *vardata2);
static Selectivity networkjoinsel_semi(Oid operator,
									   VariableStatData *vardata1, VariableStatData *vardata2);
static Selectivity mcv_population(float4 *mcv_numbers, int mcv_nvalues);
static Selectivity inet_hist_value_sel(Datum *values, int nvalues,
									   Datum constvalue, int opr_codenum);
static Selectivity inet_mcv_join_sel(Datum *mcv1_values,
									 float4 *mcv1_numbers, int mcv1_nvalues, Datum *mcv2_values,
									 float4 *mcv2_numbers, int mcv2_nvalues, Oid operator);
static Selectivity inet_mcv_hist_sel(Datum *mcv_values, float4 *mcv_numbers,
									 int mcv_nvalues, Datum *hist_values, int hist_nvalues,
									 int opr_codenum);
static Selectivity inet_hist_inclusion_join_sel(Datum *hist1_values,
												int hist1_nvalues,
												Datum *hist2_values, int hist2_nvalues,
												int opr_codenum);
static Selectivity inet_semi_join_sel(Datum lhs_value,
									  bool mcv_exists, Datum *mcv_values, int mcv_nvalues,
									  bool hist_exists, Datum *hist_values, int hist_nvalues,
									  double hist_weight,
									  FmgrInfo *proc, int opr_codenum);
static int	inet_opr_codenum(Oid operator);
static int	inet_inclusion_cmp(inet *left, inet *right, int opr_codenum);
static int	inet_masklen_inclusion_cmp(inet *left, inet *right,
									   int opr_codenum);
static int	inet_hist_match_divider(inet *boundary, inet *query,
									int opr_codenum);

/*
 * 子网包含/重叠算子的选择率估算
 */
Datum
networksel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec,
				mcv_selec,
				non_mcv_selec;
	Datum		constvalue;
	Form_pg_statistic stats;
	AttStatsSlot hslot;
	double		sumcommon,
				nullfrac;
	FmgrInfo	proc;

	/*
	 * 若表达式不是（变量 op 某物）或（某物 op 变
	 * 量），则放弃并返回默认估算值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));

	/*
	 * 若另一侧也不是常量，那就没什么可做的了。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	/* 此处算子全部为严格算子。 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constvalue = ((Const *) other)->constvalue;

	/* 否则，我们需要统计信息才能给出非默认估算。 */
	if (!HeapTupleIsValid(vardata.statsTuple))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
	nullfrac = stats->stanullfrac;

	/*
	 * 若有最常用值（MCV）信息，就把满足 MCV OP
	 * CONST 的 MCV 条目占比累加起来。这些占比直接计
	 * 入结果选择率。同时把 MCV 条目所代表的总占比
	 * 累加起来。
	 */
	fmgr_info(get_opcode(operator), &proc);
	mcv_selec = mcv_selectivity(&vardata, &proc, InvalidOid,
								constvalue, varonleft,
								&sumcommon);

	/*
	 * 若有直方图，就用它估算满足子句的非 MCV 部
	 * 分占总体比例。若没有直方图，则对该部分应用
	 * 默认选择率。
	 */
	if (get_attstatsslot(&hslot, vardata.statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		int			opr_codenum = inet_opr_codenum(operator);

		/* 必要时交换算子，以便让直方图位于左侧 */
		if (!varonleft)
			opr_codenum = -opr_codenum;
		non_mcv_selec = inet_hist_value_sel(hslot.values, hslot.nvalues,
											constvalue, opr_codenum);

		free_attstatsslot(&hslot);
	}
	else
		non_mcv_selec = DEFAULT_SEL(operator);

	/* 合并 MCV 与非 MCV 部分的选择率 */
	selec = mcv_selec + (1.0 - nullfrac - sumcommon) * non_mcv_selec;

	/* 结果应在范围内，但要确认…… */
	CLAMP_PROBABILITY(selec);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8(selec);
}

/*
 * 子网包含/重叠算子的连接选择率估算
 *
 * 本函数结构与 selfuncs.c 的 eqjoinsel() 相同。
 *
 * networkjoinsel 及其子例程都存在一个性能问题：
 * 工作量是 MCV 与直方图数组长度的 O(N^2)。
 * 为避免设置较大统计目标时运行时间失控，我们
 * 武断地把考虑的值数限制为 1024
 * （MAX_CONSIDERED_ELEMS）。对 MCV 数组而言这很
 * 简单：最多只考虑前 N 个元素。（由于 MCV 按频
 * 率递减排序，这样做恰好取到前 N 个 MCV。）
 * 对直方图数组则做抽稀（decimate）：即只考虑每
 * k 个元素，k 的选择保证考虑的元素数不超过
 * MAX_CONSIDERED_ELEMS。这仍应能给出非 MCV 部分
 * 的较好随机样本。抽稀在遍历直方图数组的循环
 * 中即时进行。
 */
Datum
networkjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	double		selec;
	VariableStatData vardata1;
	VariableStatData vardata2;
	bool		join_is_reversed;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:

			/*
			 * 左/全连接的选择率与内连接不完全相同，
			 * 但如 eqjoinsel 一样忽略这一差异。
			 */
			selec = networkjoinsel_inner(operator, &vardata1, &vardata2);
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:
			/* 这里关键是要把外侧变量放在左边传入。 */
			if (!join_is_reversed)
				selec = networkjoinsel_semi(operator, &vardata1, &vardata2);
			else
				selec = networkjoinsel_semi(get_commutator(operator),
											&vardata2, &vardata1);
			break;
		default:
			/* 不应出现其他值 */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* 让编译器安静 */
			break;
	}

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * 子网包含/重叠算子的内连接选择率估算
 *
 * 计算 MCV vs MCV、MCV vs 直方图、直方图 vs
 * 直方图的连接选择率，使用子网包含算
 * 不同于相等算子的连接选择率函数 eqjoinsel_inner()，
 * 值的一对一匹配并不足够。网络包含算子很可
 * 能多对多匹配，因此必须检查所有配对。（注：
 * 或许可以利用直方图 btree 排序的知识来减少工
 * 作量，但目前并未尝试。）
 * 与 eqjoinsel_inner() 不同，MCV vs 直方图选择率未被忽略。
 */
static Selectivity
networkjoinsel_inner(Oid operator,
					 VariableStatData *vardata1, VariableStatData *vardata2)
{
	Form_pg_statistic stats;
	double		nullfrac1 = 0.0,
				nullfrac2 = 0.0;
	Selectivity selec = 0.0,
				sumcommon1 = 0.0,
				sumcommon2 = 0.0;
	bool		mcv1_exists = false,
				mcv2_exists = false,
				hist1_exists = false,
				hist2_exists = false;
	int			opr_codenum;
	int			mcv1_length = 0,
				mcv2_length = 0;
	AttStatsSlot mcv1_slot;
	AttStatsSlot mcv2_slot;
	AttStatsSlot hist1_slot;
	AttStatsSlot hist2_slot;

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		nullfrac1 = stats->stanullfrac;

		mcv1_exists = get_attstatsslot(&mcv1_slot, vardata1->statsTuple,
									   STATISTIC_KIND_MCV, InvalidOid,
									   ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
		hist1_exists = get_attstatsslot(&hist1_slot, vardata1->statsTuple,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										ATTSTATSSLOT_VALUES);
		/* 武断地限制考虑的 MCV 数量 */
		mcv1_length = Min(mcv1_slot.nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv1_exists)
			sumcommon1 = mcv_population(mcv1_slot.numbers, mcv1_length);
	}
	else
	{
		memset(&mcv1_slot, 0, sizeof(mcv1_slot));
		memset(&hist1_slot, 0, sizeof(hist1_slot));
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata2->statsTuple);
		nullfrac2 = stats->stanullfrac;

		mcv2_exists = get_attstatsslot(&mcv2_slot, vardata2->statsTuple,
									   STATISTIC_KIND_MCV, InvalidOid,
									   ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
		hist2_exists = get_attstatsslot(&hist2_slot, vardata2->statsTuple,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										ATTSTATSSLOT_VALUES);
		/* 武断地限制考虑的 MCV 数量 */
		mcv2_length = Min(mcv2_slot.nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv2_exists)
			sumcommon2 = mcv_population(mcv2_slot.numbers, mcv2_length);
	}
	else
	{
		memset(&mcv2_slot, 0, sizeof(mcv2_slot));
		memset(&hist2_slot, 0, sizeof(hist2_slot));
	}

	opr_codenum = inet_opr_codenum(operator);

	/*
	 * 计算 MCV vs MCV 匹配的选择率。
	 */
	if (mcv1_exists && mcv2_exists)
		selec += inet_mcv_join_sel(mcv1_slot.values, mcv1_slot.numbers,
								   mcv1_length,
								   mcv2_slot.values, mcv2_slot.numbers,
								   mcv2_length,
								   operator);

	/*
	 * 把 MCV vs 直方图匹配的选择率累加进来，并按
	 * 各直方图所代表的部分占比缩放。注意第二种
	 * 情况需要交换算子。
	 */
	if (mcv1_exists && hist2_exists)
		selec += (1.0 - nullfrac2 - sumcommon2) *
			inet_mcv_hist_sel(mcv1_slot.values, mcv1_slot.numbers, mcv1_length,
							  hist2_slot.values, hist2_slot.nvalues,
							  opr_codenum);
	if (mcv2_exists && hist1_exists)
		selec += (1.0 - nullfrac1 - sumcommon1) *
			inet_mcv_hist_sel(mcv2_slot.values, mcv2_slot.numbers, mcv2_length,
							  hist1_slot.values, hist1_slot.nvalues,
							  -opr_codenum);

	/*
	 * 把直方图 vs 直方图匹配的选择率累加进来，同
	 * 样缩放。
	 */
	if (hist1_exists && hist2_exists)
		selec += (1.0 - nullfrac1 - sumcommon1) *
			(1.0 - nullfrac2 - sumcommon2) *
			inet_hist_inclusion_join_sel(hist1_slot.values, hist1_slot.nvalues,
										 hist2_slot.values, hist2_slot.nvalues,
										 opr_codenum);

	/*
	 * 若无有用的统计信息，就使用默认估算值。不
	 * 过，若已知 NULL 比例仍可应用。
	 */
	if ((!mcv1_exists && !hist1_exists) || (!mcv2_exists && !hist2_exists))
		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2) * DEFAULT_SEL(operator);

	/* 释放统计 */
	free_attstatsslot(&mcv1_slot);
	free_attstatsslot(&mcv2_slot);
	free_attstatsslot(&hist1_slot);
	free_attstatsslot(&hist2_slot);

	return selec;
}

/*
 * 子网包含/重叠算子的半连接选择率估算
 *
 * 为半/反连接计算 MCV vs MCV、MCV vs 直方图、直方图 vs MCV
 * 及直方图 vs 直方图的选择率。
 */
static Selectivity
networkjoinsel_semi(Oid operator,
					VariableStatData *vardata1, VariableStatData *vardata2)
{
	Form_pg_statistic stats;
	Selectivity selec = 0.0,
				sumcommon1 = 0.0,
				sumcommon2 = 0.0;
	double		nullfrac1 = 0.0,
				nullfrac2 = 0.0,
				hist2_weight = 0.0;
	bool		mcv1_exists = false,
				mcv2_exists = false,
				hist1_exists = false,
				hist2_exists = false;
	int			opr_codenum;
	FmgrInfo	proc;
	int			i,
				mcv1_length = 0,
				mcv2_length = 0;
	AttStatsSlot mcv1_slot;
	AttStatsSlot mcv2_slot;
	AttStatsSlot hist1_slot;
	AttStatsSlot hist2_slot;

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		nullfrac1 = stats->stanullfrac;

		mcv1_exists = get_attstatsslot(&mcv1_slot, vardata1->statsTuple,
									   STATISTIC_KIND_MCV, InvalidOid,
									   ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
		hist1_exists = get_attstatsslot(&hist1_slot, vardata1->statsTuple,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										ATTSTATSSLOT_VALUES);
		/* 武断地限制考虑的 MCV 数量 */
		mcv1_length = Min(mcv1_slot.nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv1_exists)
			sumcommon1 = mcv_population(mcv1_slot.numbers, mcv1_length);
	}
	else
	{
		memset(&mcv1_slot, 0, sizeof(mcv1_slot));
		memset(&hist1_slot, 0, sizeof(hist1_slot));
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		stats = (Form_pg_statistic) GETSTRUCT(vardata2->statsTuple);
		nullfrac2 = stats->stanullfrac;

		mcv2_exists = get_attstatsslot(&mcv2_slot, vardata2->statsTuple,
									   STATISTIC_KIND_MCV, InvalidOid,
									   ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
		hist2_exists = get_attstatsslot(&hist2_slot, vardata2->statsTuple,
										STATISTIC_KIND_HISTOGRAM, InvalidOid,
										ATTSTATSSLOT_VALUES);
		/* 武断地限制考虑的 MCV 数量 */
		mcv2_length = Min(mcv2_slot.nvalues, MAX_CONSIDERED_ELEMS);
		if (mcv2_exists)
			sumcommon2 = mcv_population(mcv2_slot.numbers, mcv2_length);
	}
	else
	{
		memset(&mcv2_slot, 0, sizeof(mcv2_slot));
		memset(&hist2_slot, 0, sizeof(hist2_slot));
	}

	opr_codenum = inet_opr_codenum(operator);
	fmgr_info(get_opcode(operator), &proc);

	/* 估算 RHS 直方图代表的输入行数。 */
	if (hist2_exists && vardata2->rel)
		hist2_weight = (1.0 - nullfrac2 - sumcommon2) * vardata2->rel->rows;

	/*
	 * 逐个考虑 LHS MCV 列表的元素，与手头任何 RHS
	 * 统计匹配；按 MCV 已知频率缩放。
	 */
	if (mcv1_exists && (mcv2_exists || hist2_exists))
	{
		for (i = 0; i < mcv1_length; i++)
		{
			selec += mcv1_slot.numbers[i] *
				inet_semi_join_sel(mcv1_slot.values[i],
								   mcv2_exists, mcv2_slot.values, mcv2_length,
								   hist2_exists,
								   hist2_slot.values, hist2_slot.nvalues,
								   hist2_weight,
								   &proc, opr_codenum);
		}
	}

	/*
	 * 我们要逐个考虑 LHS 直方图中的各个元素，但
	 * 首尾两个元素除外——排除的理由是它们属于
	 * 离群值，不够有代表性。缩放时假设每个直方
	 * 图元素都代表 LHS 直方图总体的相等份额（这
	 * 一假设有点勉强，因为同一桶内的成员对连接
	 * 子句的表现未必全都一样，但也很难做得更
	 * 好）。
	 *
	 * 若直方图元素太多，则抽稀以限制运行时间。
	 */
	if (hist1_exists && hist1_slot.nvalues > 2 && (mcv2_exists || hist2_exists))
	{
		double		hist_selec_sum = 0.0;
		int			k,
					n;

		k = (hist1_slot.nvalues - 3) / MAX_CONSIDERED_ELEMS + 1;

		n = 0;
		for (i = 1; i < hist1_slot.nvalues - 1; i += k)
		{
			hist_selec_sum +=
				inet_semi_join_sel(hist1_slot.values[i],
								   mcv2_exists, mcv2_slot.values, mcv2_length,
								   hist2_exists,
								   hist2_slot.values, hist2_slot.nvalues,
								   hist2_weight,
								   &proc, opr_codenum);
			n++;
		}

		selec += (1.0 - nullfrac1 - sumcommon1) * hist_selec_sum / n;
	}

	/*
	 * 若无有用的统计信息，就使用默认估算值。不
	 * 过，若已知 NULL 比例仍可应用。
	 */
	if ((!mcv1_exists && !hist1_exists) || (!mcv2_exists && !hist2_exists))
		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2) * DEFAULT_SEL(operator);

	/* 释放统计 */
	free_attstatsslot(&mcv1_slot);
	free_attstatsslot(&mcv2_slot);
	free_attstatsslot(&hist1_slot);
	free_attstatsslot(&hist2_slot);

	return selec;
}

/*
 * 计算某关系的总体中由 MCV 列表所代表的
 * 占比。
 */
static Selectivity
mcv_population(float4 *mcv_numbers, int mcv_nvalues)
{
	Selectivity sumcommon = 0.0;
	int			i;

	for (i = 0; i < mcv_nvalues; i++)
	{
		sumcommon += mcv_numbers[i];
	}

	return sumcommon;
}

/*
 * inet 直方图 vs 单值选择率估算
 *
 * 估算直方图总体中满足
 * "值 OPR CONST" 的占比。（结果需要缩放，以
 * 反映直方图所代表的总体占比。）
 *
 * 直方图最初是为 inet btree 比较算子建立的。
 * 对子网包含算子有意义的是网络部分的公共位
 * 以及网络部分的长度（masklen）。所幸 btree 比
 * 较把网络部分当作主排序键。即便如此，网络
 * 部分的长度在直方图中其实并不显著。对
 * masklen 分布不均的数据集，这会导致大错。为
 * 减轻该问题，与桶左、右边界的比较会被一起
 * 使用。
 *
 * 直方图桶匹配按两种形式计算。若常量与桶的
 * 两个端点都匹配，该桶视为完全匹配。第二种
 * 形式是部分匹配桶；当常量只与一个端点匹配
 * 或两个端点分落在常量两侧时，我们即认定是
 * 部分匹配。（注意：当常量与某个直方图内部
 * 元素匹配时，它会同时获得两侧桶的部分匹配
 * 的分值；而与直方图端点匹配则只获得一个部
 * 分匹配。这是合理的。）
 *
 * 部分桶匹配中的分隔值设想为地址的决定位与
 * 公共位之间的距离。它将以 2 的幂使用，因为这
 * 是 IP 网络包含的自然尺度。这个部分桶匹配分
 * 隔值的计算是经验公式，
 * 经更多实验后可能调整。
 *
 * 对部分匹配，我们尝试为两个边界都计算分隔
 * 值。若某边界值的地址族与常量不符，或网络
 * 部分长度的比较对该算子不成立，则该边界的
 * 分隔值将不予考虑。若两个分隔值都有效，则
 * 取较大者，以尽量减少 masklen 悬殊的桶中的错
 * 误。当两个边界都能算出分隔值但彼此相差很
 * 远时，这种计算并不公平；但这并不常见，因
 * 为按预期边界会共享其 masklen 大部分的有效
 * 位。若用最小值而不是最大值，错误会更大，
 * 而且我们也不知道有什么合理的方式能把两
 * 者结合起来，这一点上我们没有更好的办
 * 法
 *
 * 对于左右两侧地址族不同的桶中的部分匹配，
 * 只考虑与常量同族的那个边界。若这些桶的边
 * 界 masklen 也相差悬殊，这会给这些桶带来更多
 * 错误。但地址族只有两种，所以这只会在
 * 一个桶上。看来这仍好过完全不
 * 考虑这些桶。
 */
static Selectivity
inet_hist_value_sel(Datum *values, int nvalues, Datum constvalue,
					int opr_codenum)
{
	Selectivity match = 0.0;
	inet	   *query,
			   *left,
			   *right;
	int			i,
				k,
				n;
	int			left_order,
				right_order,
				left_divider,
				right_divider;

	/* 防止下面出现除零 */
	if (nvalues <= 1)
		return 0.0;

	/* 若直方图元素太多，抽稀以限制运行时间 */
	k = (nvalues - 2) / MAX_CONSIDERED_ELEMS + 1;

	query = DatumGetInetPP(constvalue);

	/* "left" 是当前桶的左边界值…… */
	left = DatumGetInetPP(values[0]);
	left_order = inet_inclusion_cmp(left, query, opr_codenum);

	n = 0;
	for (i = k; i < nvalues; i += k)
	{
		/* 续上："right" 是右边界值 */
		right = DatumGetInetPP(values[i]);
		right_order = inet_inclusion_cmp(right, query, opr_codenum);

		if (left_order == 0 && right_order == 0)
		{
			/* 两端点都匹配，整桶匹配。 */
			match += 1.0;
		}
		else if ((left_order <= 0 && right_order >= 0) ||
				 (left_order >= 0 && right_order <= 0))
		{
			/* 部分桶匹配。 */
			left_divider = inet_hist_match_divider(left, query, opr_codenum);
			right_divider = inet_hist_match_divider(right, query, opr_codenum);

			if (left_divider >= 0 || right_divider >= 0)
				match += 1.0 / pow(2.0, Max(left_divider, right_divider));
		}

		/* 轮换变量。 */
		left = right;
		left_order = right_order;

		/* 统计已考虑的桶数。 */
		n++;
	}

	return match / n;
}

/*
 * inet MCV vs MCV 连接选择率估算
 *
 * 我们只把满足子句的各部分占比直接相加。
 * 结果是精确的，无需再缩放。
 */
static Selectivity
inet_mcv_join_sel(Datum *mcv1_values, float4 *mcv1_numbers, int mcv1_nvalues,
				  Datum *mcv2_values, float4 *mcv2_numbers, int mcv2_nvalues,
				  Oid operator)
{
	Selectivity selec = 0.0;
	FmgrInfo	proc;
	int			i,
				j;

	fmgr_info(get_opcode(operator), &proc);

	for (i = 0; i < mcv1_nvalues; i++)
	{
		for (j = 0; j < mcv2_nvalues; j++)
			if (DatumGetBool(FunctionCall2(&proc,
										   mcv1_values[i],
										   mcv2_values[j])))
				selec += mcv1_numbers[i] * mcv2_numbers[j];
	}
	return selec;
}

/*
 * inet MCV vs 直方图连接选择率估算
 *
 * 对左侧的每个 MCV，估算右侧直方图总体中
 * 满足连接子句的部分，把这些结果累加起来，
 * 并按该 MCV 的频率缩放。结果仍需按右侧总体
 * 中由直方图代表的占比来进一步缩放这部
 * 分。
 */
static Selectivity
inet_mcv_hist_sel(Datum *mcv_values, float4 *mcv_numbers, int mcv_nvalues,
				  Datum *hist_values, int hist_nvalues,
				  int opr_codenum)
{
	Selectivity selec = 0.0;
	int			i;

	/*
	 * 我们会把直方图放在左侧来调用 inet_hist_value_selec，
	 * 所以要交换算子。
	 */
	opr_codenum = -opr_codenum;

	for (i = 0; i < mcv_nvalues; i++)
	{
		selec += mcv_numbers[i] *
			inet_hist_value_sel(hist_values, hist_nvalues, mcv_values[i],
								opr_codenum);
	}
	return selec;
}

/*
 * inet 直方图 vs 直方图连接选择率估算
 *
 * 这里，我们取出第二个直方图中列出的全部值
 * （首尾两个元素除外，理由是它们可能不具代
 * 表性），把它们当作该关系非 MCV 总体的均匀
 * 样本。对其中每个值，我们用
 * inet_hist_value_selec 来看它匹配第一个直方图的
 * 多少。
 *
 * 我们也可以借助算子的交换算子（commutator）
 * 反向完成这件事。XXX 两种做法各做一遍再取
 * 平均值是否值得？至少能避免不可交换的估算
 */
static Selectivity
inet_hist_inclusion_join_sel(Datum *hist1_values, int hist1_nvalues,
							 Datum *hist2_values, int hist2_nvalues,
							 int opr_codenum)
{
	double		match = 0.0;
	int			i,
				k,
				n;

	if (hist2_nvalues <= 2)
		return 0.0;				/* 无内部直方图元素 */

	/* 若直方图元素太多，抽稀以限制运行时间 */
	k = (hist2_nvalues - 3) / MAX_CONSIDERED_ELEMS + 1;

	n = 0;
	for (i = 1; i < hist2_nvalues - 1; i += k)
	{
		match += inet_hist_value_sel(hist1_values, hist1_nvalues,
									 hist2_values[i], opr_codenum);
		n++;
	}

	return match / n;
}

/*
 * inet 单值半连接选择率估算
 *
 * 本函数计算 RHS 表中至少存在一行满足
 * "lhs_value op column" 条件的概率。该函数用于
 * 半连接估算中，用于从左侧表中抽取样本来
 * 检查。
 *
 * 调用时应把右侧表的 MCV 与直方图，连同连接
 * 用的 lhs_value 一并作为参数传入。
 * hist_weight 是直方图代表的总行数。
 * 例如，若表有 1000 行，10% 的行在 MCV 列表里，
 * 另有 10% 为 NULL，则 hist_weight 就是 800。
 *
 * 首先，lhs_value 会与最常用值（MCV）匹配。若
 * 与其中任何一项匹配，就返回 1.0，因为肯定存在
 * 匹配
 *
 * 否则，将用直方图估算第二张表中满足条件的
 * 行数。若估算值大于 1.0，则返回 1.0，因为这意
 * 味着 lhs_value 很可能匹配表中不止一行。若估
 * 算值介于 0.0 与 1.0 之间，就会把它作为概率
 * 返回。
 */
static Selectivity
inet_semi_join_sel(Datum lhs_value,
				   bool mcv_exists, Datum *mcv_values, int mcv_nvalues,
				   bool hist_exists, Datum *hist_values, int hist_nvalues,
				   double hist_weight,
				   FmgrInfo *proc, int opr_codenum)
{
	if (mcv_exists)
	{
		int			i;

		for (i = 0; i < mcv_nvalues; i++)
		{
			if (DatumGetBool(FunctionCall2(proc,
										   lhs_value,
										   mcv_values[i])))
				return 1.0;
		}
	}

	if (hist_exists && hist_weight > 0)
	{
		Selectivity hist_selec;

		/* 交换算子，因为 lhs_value 是在右侧传入的 */
		hist_selec = inet_hist_value_sel(hist_values, hist_nvalues,
										 lhs_value, -opr_codenum);

		if (hist_selec > 0)
			return Min(1.0, hist_weight * hist_selec);
	}

	return 0.0;
}

/*
 * 为子网包含/重叠算子指定便于使用的编码号
 *
 * 只有 inet_masklen_inclusion_cmp() 与 inet_hist_match_divider()
 * 依赖这里指定的具体编码；但本文件中许多其他
 * 地方都知道：对编码取负即得交换算子
 * 编码。
 */
static int
inet_opr_codenum(Oid operator)
{
	switch (operator)
	{
		case OID_INET_SUP_OP:
			return -2;
		case OID_INET_SUPEQ_OP:
			return -1;
		case OID_INET_OVERLAP_OP:
			return 0;
		case OID_INET_SUBEQ_OP:
			return 1;
		case OID_INET_SUB_OP:
			return 2;
		default:
			elog(ERROR, "unrecognized operator %u for inet selectivity",
				 operator);
	}
	return 0;					/* 不可达，让编译器安静 */
}

/*
 * 子网包含/重叠算子的比较函数
 *
 * 若比较对指定的包含算子成立，返回值将为 0；
 * 否则返回值将视算子而定，小于或大
 * 于 0。
 *
 * 该比较与 inet 类型的基本比较函数兼容。原始
 * 实现见 network.c 的 network_cmp_internal()。基
 * 本比较算子都用 network_cmp_internal() 实现。
 * 用本函数来实现子网包含算子，这同样
 * 可行。
 *
 * 比较先比网络部分的公共位，再比网络部分
 * 的长度（masklen），这与 network_cmp_internal()
 * 函数一致。本函数只包含第一部分；第二部分
 * 被拆到另一个函数以便复用。第二部分与原始
 * network_cmp_internal() 的差别在于：比较网络部
 * 分的长度时会把包含算子考虑在内。见下方的
 * inet_masklen_inclusion_cmp() 函数。
 */
static int
inet_inclusion_cmp(inet *left, inet *right, int opr_codenum)
{
	if (ip_family(left) == ip_family(right))
	{
		int			order;

		order = bitncmp(ip_addr(left), ip_addr(right),
						Min(ip_bits(left), ip_bits(right)));
		if (order != 0)
			return order;

		return inet_masklen_inclusion_cmp(left, right, opr_codenum);
	}

	return ip_family(left) - ip_family(right);
}

/*
 * 子网包含/重叠算子的 masklen 比较函数
 *
 * 比较输入的网络部分长度。若比较对指定的
 * 包含算子成立，返回值将为 0；否则返回值将
 * 视算子而定，小于或大于
 * 0。
 */
static int
inet_masklen_inclusion_cmp(inet *left, inet *right, int opr_codenum)
{
	int			order;

	order = (int) ip_bits(left) - (int) ip_bits(right);

	/*
	 * 若算子接受这对 masklen 组合，则返回 0。
	 * 注意 opr_codenum 为零（overlaps）接受一切情形。
	 */
	if ((order > 0 && opr_codenum >= 0) ||
		(order == 0 && opr_codenum >= -1 && opr_codenum <= 1) ||
		(order < 0 && opr_codenum <= 0))
		return 0;

	/*
	 * 否则，对 sup/supeq 返回负值（名义上，RHS 需要
	 * 有比现在更大的 masklen，从而排得更靠后），
	 * 对 sub/subeq 则返回正值（反之亦然）。
	 */
	return opr_codenum;
}

/*
 * inet 直方图部分匹配分隔值计算
 *
 * 先用子网包含算子比较地址族与网络部分长
 * 度。若对算子而言可以接受，则用 masklen 与
 * 地址公共位计算分隔值。若无法计算，则返回
 * -1。
 *
 * 这样做的缘由参见 inet_hist_value_sel() 的说明。
 */
static int
inet_hist_match_divider(inet *boundary, inet *query, int opr_codenum)
{
	if (ip_family(boundary) == ip_family(query) &&
		inet_masklen_inclusion_cmp(boundary, query, opr_codenum) == 0)
	{
		int			min_bits,
					decisive_bits;

		min_bits = Min(ip_bits(boundary), ip_bits(query));

		/*
		 * 把 decisive_bits 设为按算子应包含对方的
		 * 那一方的 masklen。
		 */
		if (opr_codenum < 0)
			decisive_bits = ip_bits(boundary);
		else if (opr_codenum > 0)
			decisive_bits = ip_bits(query);
		else
			decisive_bits = min_bits;

		/*
		 * 现在返回非公共决定位的数目。（若边界与
		 * 查询确实匹配，则为零，否则为正。）
		 */
		if (min_bits > 0)
			return decisive_bits - bitncommon(ip_addr(boundary),
											  ip_addr(query),
											  min_bits);
		return decisive_bits;
	}

	return -1;
}
