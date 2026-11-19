/*-------------------------------------------------------------------------
 *
 * selfuncs.c
 *	  选择率函数与索引代价估算函数
 *	  用于标准算子与索引访问方法。
 *
 *	  选择率例程通过 pg_operator 目录的
 *	  "oprrest" 与 "oprjoin" 属性登记。
 *
 *	  索引代价函数通过索引 AM 的 API 结构体
 *	  定位，结构体取自 pg_am 中登记的 handler 函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/selfuncs.c
 *
 *-------------------------------------------------------------------------
 */

/*----------
 * 算子选择率估算函数用于估计 WHERE 子句的选择率，
 * 这些子句的顶层算子正是该算子本身。我们把
 * 问题分成两种情形：
 *		限制子句估算：子句只涉及一个关系的变量。
 *
 *		连接子句估算：子句涉及多个关系的变量。
 * 连接选择率估算远为困难，且通常不如限制估算准确。
 *
 *
 * 处理 nestloop 连接的内侧扫描时，我们把该连接的
 * 连接子句当作内侧关系的限制子句，并把外侧关系的
 * 变量当作参数（也就是取值未知的常量）。
 * 因此，限制估算器必须能接受一个参数，告诉它哪个
 * 关系要被当作变量。
 *
 * 限制估算器（oprrest 函数）的调用约定为：
 *
 *		Selectivity oprrest (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 int varRelid);
 *
 * root: 关于查询的一般信息（rtable 与 RelOptInfo
 * 列表对估算器尤为重要）。
 * operator: 所论算子的 OID。
 * args: 来自算子子句的参数列表。
 * varRelid: 若非零，为要当作变量关系的关系的
 * relid（rtable 下标）。若已知 args 列表只含
 * 一个关系的变量，则可为零。
 *
 * 这在 SQL 层面（pg_proc 中）表示为：
 *
 *		float8 oprrest (internal, oid, internal, int4);
 *
 * 结果是一个选择率，即该关系中预计会对给定算子
 * 产生 TRUE 结果的行所占的比例（0 到 1）。
 *
 *
 * 连接估算器（oprjoin 函数）的调用约定与之类似，
 * 只是无需 varRelid，取而代之的是提供连接信息：
 *
 *
 *		Selectivity oprjoin (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 JoinType jointype,
 *							 SpecialJoinInfo *sjinfo);
 *
 *		float8 oprjoin (internal, oid, internal, int2, internal);
 *
 * （在 Postgres 8.4 之前，连接估算器只有这些参数中的
 * 前四个。该签名仍被允许，但已不推荐使用。）
 * jointype 与 sjinfo 的关系在 clause_selectivity() 的注释中
 * 有说明 --- 简而言之，通常最好是忽略 jointype，
 * 改为检查 sjinfo。
 *
 * 常规内连接与外连接的连接选择率定义为关系的
 * 笛卡尔积中预计会对给定算子产生 TRUE 结果的部分
 * （0 到 1）。但对半连接与反连接而言，
 * 选择率定义为左侧关系的行中预计能在右侧找到匹配
 * （即至少有一行产生 TRUE 结果）的比例。
 *
 *
 * 对于 oprrest 与 oprjoin 函数，算子的输入排序规则 OID
 * （若有）通过标准 fmgr 机制传递，估算器函数可用
 * PG_GET_COLLATION() 取得它。但请注意，
 * pg_statistic 中的统计信息当前都用相关列的排序规则。
 *
 *----------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/brin.h"
#include "access/brin_page.h"
#include "access/gin.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#define DEFAULT_PAGE_CPU_MULTIPLIER 50.0

/* 插件请求统计信息时获得控制的钩子 */
get_relation_stats_hook_type get_relation_stats_hook = NULL;
get_index_stats_hook_type get_index_stats_hook = NULL;

static double eqsel_internal(PG_FUNCTION_ARGS, bool negate);
static double eqjoinsel_inner(Oid opfuncoid, Oid collation,
							  VariableStatData *vardata1, VariableStatData *vardata2,
							  double nd1, double nd2,
							  bool isdefault1, bool isdefault2,
							  AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							  Form_pg_statistic stats1, Form_pg_statistic stats2,
							  bool have_mcvs1, bool have_mcvs2);
static double eqjoinsel_semi(Oid opfuncoid, Oid collation,
							 VariableStatData *vardata1, VariableStatData *vardata2,
							 double nd1, double nd2,
							 bool isdefault1, bool isdefault2,
							 AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							 Form_pg_statistic stats1, Form_pg_statistic stats2,
							 bool have_mcvs1, bool have_mcvs2,
							 RelOptInfo *inner_rel);
static bool estimate_multivariate_ndistinct(PlannerInfo *root,
											RelOptInfo *rel, List **varinfos, double *ndistinct);
static bool convert_to_scalar(Datum value, Oid valuetypid, Oid collid,
							  double *scaledvalue,
							  Datum lobound, Datum hibound, Oid boundstypid,
							  double *scaledlobound, double *scaledhibound);
static double convert_numeric_to_scalar(Datum value, Oid typid, bool *failure);
static void convert_string_to_scalar(char *value,
									 double *scaledvalue,
									 char *lobound,
									 double *scaledlobound,
									 char *hibound,
									 double *scaledhibound);
static void convert_bytea_to_scalar(Datum value,
									double *scaledvalue,
									Datum lobound,
									double *scaledlobound,
									Datum hibound,
									double *scaledhibound);
static double convert_one_string_to_scalar(char *value,
										   int rangelo, int rangehi);
static double convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
										  int rangelo, int rangehi);
static char *convert_string_datum(Datum value, Oid typid, Oid collid,
								  bool *failure);
static double convert_timevalue_to_scalar(Datum value, Oid typid,
										  bool *failure);
static void examine_simple_variable(PlannerInfo *root, Var *var,
									VariableStatData *vardata);
static bool get_variable_range(PlannerInfo *root, VariableStatData *vardata,
							   Oid sortop, Oid collation,
							   Datum *min, Datum *max);
static void get_stats_slot_range(AttStatsSlot *sslot,
								 Oid opfuncoid, FmgrInfo *opproc,
								 Oid collation, int16 typLen, bool typByVal,
								 Datum *min, Datum *max, bool *p_have_data);
static bool get_actual_variable_range(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid sortop, Oid collation,
									  Datum *min, Datum *max);
static bool get_actual_variable_endpoint(Relation heapRel,
										 Relation indexRel,
										 ScanDirection indexscandir,
										 ScanKey scankeys,
										 int16 typLen,
										 bool typByVal,
										 TupleTableSlot *tableslot,
										 MemoryContext outercontext,
										 Datum *endpointDatum);
static RelOptInfo *find_join_input_rel(PlannerInfo *root, Relids relids);


/*
 *		eqsel			- 任意数据类型 "=" 的选择率。
 *
 * 注意：本例程也用于估计一些并非 "=" 但选择率
 * 行为相近的算子，例如 "~="（几何近似匹配）。
 * 即便是 "="，我们也必须记住左右两侧的数据类型
 * 可能不同。
 */
Datum
eqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, false));
}

/*
 * eqsel() 与 neqsel() 的公共代码
 */
static double
eqsel_internal(PG_FUNCTION_ARGS, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	double		selec;

	/*
	 * 当被问到 <> 时，我们用相应的 = 算子做估算，
	 * 然后通过 "1.0 - eq_selectivity - nullfrac" 转换为 <>。
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
		{
			/* 使用默认选择率（要不要改成抛错？） */
			return 1.0 - DEFAULT_EQ_SEL;
		}
	}

	/*
	 * 若表达式不是 变量 = 某物 或 某物 = 变量 的形式，
	 * 则放弃并返回默认估算。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return negate ? (1.0 - DEFAULT_EQ_SEL) : DEFAULT_EQ_SEL;

	/*
	 * 若某物是常量，我们能做得好得多。（注意：
	 * 该 Const 可能是估算的产物，而非查询中的简单
	 * 常量。）
	 */
	if (IsA(other, Const))
		selec = var_eq_const(&vardata, operator, collation,
							 ((Const *) other)->constvalue,
							 ((Const *) other)->constisnull,
							 varonleft, negate);
	else
		selec = var_eq_non_const(&vardata, operator, collation, other,
								 varonleft, negate);

	ReleaseVariableStats(vardata);

	return selec;
}

/*
 * var_eq_const --- var=const 情形的 eqsel
 *
 * 导出此函数是为了让其他估算函数能够使用它。
 */
double
var_eq_const(VariableStatData *vardata, Oid oproid, Oid collation,
			 Datum constval, bool constisnull,
			 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;
	Oid			opfuncoid;

	/*
	 * 若常量为 NULL，则假定算子是严格的并返回零，即
	 * 算子绝不会返回 TRUE。（取反算子同样是零。）
	 */
	if (constisnull)
		return 0.0;

	/*
	 * 在此取得 nullfrac 供下文使用。注意我们允许
	 * 无视安全检查用它。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * 若我们把变量匹配到了唯一索引或 DISTINCT 子句，
	 * 则假定恰好只有一个匹配，无视其他一切。（这
	 * 略显武断，因为索引或子句的相等算子可能与我们的
	 * 不同，但这比无视该信息更可能正确。）
	 *
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple) &&
			 statistic_proc_security_check(vardata,
										   (opfuncoid = get_opcode(oproid))))
	{
		AttStatsSlot sslot;
		bool		match = false;
		int			i;

		/*
		 * 该常量是否 "=" 于该列的某个最常用值？
		 * （虽然给定算子未必真的是 "="，但我们将假定
		 * 检查它是否返回 TRUE 是合适的测试。若你不喜欢
		 * 这一点，也许你不该为你的算子用
		 * eqsel…）
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			LOCAL_FCINFO(fcinfo, 2);
			FmgrInfo	eqproc;

			fmgr_info(opfuncoid, &eqproc);

			/*
			 * 只设置一次 fcinfo 结构即可节省几步开销。
			 * 直接使用 FunctionCallInvoke 还能避免在
			 * eqproc 返回 NULL 时失败，虽然相等函数
			 * 绝不该这样。
			 */
			InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* 小心按正确的方向应用算子 */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					match = true;
					break;
				}
			}
		}
		else
		{
			/* 无最常用值信息可用 */
			i = 0;				/* 让编译器安静 */
		}

		if (match)
		{
			/*
			 * 该常量与此常用值 "="。我们确切地知道
			 * 选择率（或至少是 ANALYZE 能算出的精确度）。
			 */
			selec = sslot.numbers[i];
		}
		else
		{
			/*
			 * 比较对象既非 NULL 也非任一常用值的常量。
			 * 其选择率至多为：
			 *
			 */
			double		sumcommon = 0.0;
			double		otherdistinct;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			selec = 1.0 - sumcommon - nullfrac;
			CLAMP_PROBABILITY(selec);

			/*
			 * 而实际上很可能要小得多。我们近似地假定
			 * 所有非常用值均分余下的份额，故我们
			 * 除以其他不同值的数目。
			 */
			otherdistinct = get_variable_numdistinct(vardata, &isdefault) -
				sslot.nnumbers;
			if (otherdistinct > 1)
				selec /= otherdistinct;

			/*
			 * 另一项交叉检查：选择率不应被估计得高于
			 * 最不常见的"最常用值"。
			 */
			if (sslot.nnumbers > 0 && selec > sslot.numbers[sslot.nnumbers - 1])
				selec = sslot.numbers[sslot.nnumbers - 1];
		}

		free_attstatsslot(&sslot);
	}
	else
	{
		/*
		 * 没有 ANALYZE 统计信息，于是用估计的不同值
		 * 数目来猜测，并假定它们同样常见。（这个猜测
		 * 不太可能很好，但我们确知一些特殊情形。）
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* 若想要 <> 而非 =，现在调整 */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * var_eq_non_const --- var = 非常量情形的 eqsel
 *
 * 导出此函数是为了让其他估算函数能够使用它。
 */
double
var_eq_non_const(VariableStatData *vardata, Oid oproid, Oid collation,
				 Node *other,
				 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;

	/*
	 * 取 nullfrac 供下文使用。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * 若我们把变量匹配到了唯一索引或 DISTINCT 子句，
	 * 则假定恰好只有一个匹配，无视其他一切。（这
	 * 略显武断，因为索引或子句的相等算子可能与我们的
	 * 不同，但这比无视该信息更可能正确。）
	 *
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple))
	{
		double		ndistinct;
		AttStatsSlot sslot;

		/*
		 * 搜索的是一个我们事先不知道的值，但我们
		 * 将假定它非 NULL。把选择率估计为非空
		 * 分数除以不同值数目，这样得到的是对所有
		 * 可能值的平均结果（不论是否常见）。
		 * （本质上，我们是假定尚未得知的比较值
		 * 等可能地取任何可能值，无视其在表中
		 * 的出现频率。这是个好主意吗？）
		 *
		 */
		selec = 1.0 - nullfrac;
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		if (ndistinct > 1)
			selec /= ndistinct;

		/*
		 * 交叉检查：选择率绝不应被估计得超过最
		 * 常用值的。
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0 && selec > sslot.numbers[0])
				selec = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}
	else
	{
		/*
		 * 没有 ANALYZE 统计信息，于是用估计的不同值
		 * 数目来猜测，并假定它们同样常见。（这个猜测
		 * 不太可能很好，但我们确知一些特殊情形。）
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* 若想要 <> 而非 =，现在调整 */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *		neqsel			- 任意数据类型 "!=" 的选择率。
 *
 * 本例程也用于一些并非 "!=" 但选择率行为
 * 相近的算子。参见上文 eqsel() 的注释。
 *
 */
Datum
neqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, true));
}

/*
 *	scalarineqsel		- 标量 "<"、"<="、">"、">=" 的选择率。
 *
 * 这是 scalarltsel/scalarlesel/scalargtsel/scalargesel 的核心。
 * isgt 与 iseq 标志区分四种情形中哪一种适用。
 *
 * 调用者已在必要时交换了子句，使变量位于左侧。
 * 调用者还必须确保子句的另一侧是非 NULL 的 Const，
 * 并将其分解为值与数据类型。（这一定义简化了
 * 某些想针对计算出的值而非 Const 节点做估算
 * 的调用者。）
 *
 * 本例程适用于 convert_to_scalar() 已知的任何数据
 * 类型（或类型对）。若用于其他数据类型，
 * 它将返回一个近似估算，其基于假定常量值落在
 * 二分查找所识别出的桶的中间。
 */
static double
scalarineqsel(PlannerInfo *root, Oid operator, bool isgt, bool iseq,
			  Oid collation,
			  VariableStatData *vardata, Datum constval, Oid consttype)
{
	Form_pg_statistic stats;
	FmgrInfo	opproc;
	double		mcv_selec,
				hist_selec,
				sumcommon;
	double		selec;

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/*
		 * 没有统计信息可用。通常这意味着我们不得不
		 * 回退到默认估算；但若变量是 CTID，我们可以
		 * 基于常量与表大小的比较做出估算。
		 */
		if (vardata->var && IsA(vardata->var, Var) &&
			((Var *) vardata->var)->varattno == SelfItemPointerAttributeNumber)
		{
			ItemPointer itemptr;
			double		block;
			double		density;

			/*
			 * 若关系为空，我们将把它的全部包含进来。
			 * （这主要是为了避免下文除零。）
			 */
			if (vardata->rel->pages == 0)
				return 1.0;

			itemptr = (ItemPointer) DatumGetPointer(constval);
			block = ItemPointerGetBlockNumberNoCheck(itemptr);

			/*
			 * 确定每页的平均元组数（密度）。
			 *
			 * 由于最后一页平均只有半满，我们可以估计它
			 * 拥有此前页面一半的元组数。因此给它的权重
			 * 是常规页面的一半。
			 */
			density = vardata->rel->tuples / (vardata->rel->pages - 0.5);

			/* 若目标是最后一页，使用一半密度 */
			if (block >= vardata->rel->pages - 1)
				density *= 0.5;

			/*
			 * 用每页平均元组数计算 itemptr 可能位于页内
			 * 多远处，并据此调整块号：加上整块的
			 * 该比例（但无论 itemptr 的偏移多高，都绝不
			 * 超过一整个块）。这里我们忽略了死元组行
			 * 指针的可能性，这相当粗糙，但我们缺少
			 * 做得更好的信息。
			 */
			if (density > 0.0)
			{
				OffsetNumber offset = ItemPointerGetOffsetNumberNoCheck(itemptr);

				block += Min(offset / density, 1.0);
			}

			/*
			 * 将相对块号转换为选择率。同样，最后一页
			 * 只有一半权重。
			 */
			selec = block / (vardata->rel->pages - 0.5);

			/*
			 * 至此的计算给出了 "<=" 情形的选择率。
			 * "<" 会少一个元组，">=" 会多一个元组，后者
			 * 我们将在下文反转其选择率，所以两种情形
			 * 都可以简单地减去一个元组。需要此调整的
			 * 情形可通过 iseq 等于 isgt 来识别。
			 *
			 */
			if (iseq == isgt && vardata->rel->tuples >= 1.0)
				selec -= (1.0 / vardata->rel->tuples);

			/* 最后，反转 ">"、">=" 情形的选择率 */
			if (isgt)
				selec = 1.0 - selec;

			CLAMP_PROBABILITY(selec);
			return selec;
		}

		/* 无统计信息，用默认结果 */
		return DEFAULT_INEQ_SEL;
	}
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

	fmgr_info(get_opcode(operator), &opproc);

	/*
	 * 若有最常用值信息，就把满足 MCV OP CONST 的
	 * MCV 条目份额累加起来。这些份额直接计入
	 * 结果选择率。同时累加 MCV 条目所代表的
	 * 总份额。
	 */
	mcv_selec = mcv_selectivity(vardata, &opproc, collation, constval, true,
								&sumcommon);

	/*
	 * 若有直方图，确定常量落在哪个桶中，并计算
	 * 对选择率的相应贡献。
	 */
	hist_selec = ineq_histogram_selectivity(root, vardata,
											operator, &opproc, isgt, iseq,
											collation,
											constval, consttype);

	/*
	 * 现在合并 MCV 与直方图计算的结果，注意
	 * 直方图只覆盖未列入 MCV 的非空值。
	 *
	 */
	selec = 1.0 - stats->stanullfrac - sumcommon;

	if (hist_selec >= 0.0)
		selec *= hist_selec;
	else
	{
		/*
		 * 若无直方图但有 MCV 未覆盖的值，则武断地
		 * 假定其中一半会匹配。
		 */
		selec *= 0.5;
	}

	selec += mcv_selec;

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	mcv_selectivity			- 检查 MCV 列表以获得选择率估算
 *
 * 确定变量的 MCV 群体中满足谓词（VAR OP CONST）
 * 的比例，若 !varonleft 则为（CONST OP VAR）。同时
 * 计算 MCV 列表所代表的总列群体的比例。此代码
 * 适用于任何返回布尔的谓词算子。
 *
 * 函数结果是 MCV 选择率，总群体比例则通过
 * *sumcommonp 返回。若无 MCV 列表则返回零值。
 *
 */
double
mcv_selectivity(VariableStatData *vardata, FmgrInfo *opproc, Oid collation,
				Datum constval, bool varonleft,
				double *sumcommonp)
{
	double		mcv_selec,
				sumcommon;
	AttStatsSlot sslot;
	int			i;

	mcv_selec = 0.0;
	sumcommon = 0.0;

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
	{
		LOCAL_FCINFO(fcinfo, 2);

		/*
		 * 我们"手工"调用 opproc，以免在 NULL 结果上
		 * 失败。这类情形对普通比较函数不会出现，
		 * 但 generic_restriction_selectivity 也许会与
		 * 可返回 NULL 的算子一起使用。一个小的
		 * 附带好处是无需每次都从零初始化 fcinfo。
		 */
		InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;
		/* 小心按正确的方向应用算子 */
		if (varonleft)
			fcinfo->args[1].value = constval;
		else
			fcinfo->args[0].value = constval;

		for (i = 0; i < sslot.nvalues; i++)
		{
			Datum		fresult;

			if (varonleft)
				fcinfo->args[0].value = sslot.values[i];
			else
				fcinfo->args[1].value = sslot.values[i];
			fcinfo->isnull = false;
			fresult = FunctionCallInvoke(fcinfo);
			if (!fcinfo->isnull && DatumGetBool(fresult))
				mcv_selec += sslot.numbers[i];
			sumcommon += sslot.numbers[i];
		}
		free_attstatsslot(&sslot);
	}

	*sumcommonp = sumcommon;
	return mcv_selec;
}

/*
 *	histogram_selectivity	- 检查直方图以获得选择率估算
 *
 * 确定变量的直方图条目中满足谓词（VAR OP CONST）
 * 的比例，若 !varonleft 则为（CONST OP VAR）。
 *
 * 此代码适用于任何返回布尔的谓词算子，无论它与
 * 直方图排序算子有无关系。我们本质上是把直方图
 * 当作代表性样本使用。然而，小的直方图不太可能
 * 那么有代表性，因此当直方图缺失或非常小时，
 * 调用者应准备好回退到其他估算方法。当直方图
 * 较小时，将此方法与其他方法结合也可能是明智的。
 *
 *
 * 若实际直方图大小不足 min_hist_size，我们根本
 * 就不必做这个计算。另外，若 n_skip 参数 > 0，
 * 我们会忽略开头和结尾各 n_skip 个直方图元素，
 * 理由是它们属于离群值因而代表性不强。这些参数
 * 的典型取值为 10 和 1。
 *
 * 函数结果为选择率；若无直方图或其小于
 * min_hist_size，则为 -1。
 *
 * 输出参数 *hist_size 接收实际直方图大小；
 * 若无直方图则为零。调用者可用这个数字来决定
 * 对函数结果抱有多大的信任。
 *
 * 注意结果会无视最常用值（若有）与空值条目。
 * 调用者应将此结果与列群体中这些部分的
 * 统计信息相结合。把结果范围钳制起来（即不
 * 相信精确的 0 或 1 输出）也可能是明智的。
 */
double
histogram_selectivity(VariableStatData *vardata,
					  FmgrInfo *opproc, Oid collation,
					  Datum constval, bool varonleft,
					  int min_hist_size, int n_skip,
					  int *hist_size)
{
	double		result;
	AttStatsSlot sslot;

	/* 检查参数的合理性 */
	Assert(n_skip >= 0);
	Assert(min_hist_size > 2 * n_skip);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		*hist_size = sslot.nvalues;
		if (sslot.nvalues >= min_hist_size)
		{
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;
			int			i;

			/*
			 * 我们"手工"调用 opproc，以免在 NULL 结果上
			 * 失败。这类情形对普通比较函数
			 * 不会出现，但 generic_restriction_selectivity
			 * 也许会与可返回 NULL 的算子一起使用。一个小的
			 * 附带好处是无需每次都从零初始化 fcinfo
			 * 结构。
			 */
			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* 小心按正确的方向应用算子 */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = n_skip; i < sslot.nvalues - n_skip; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			result = ((double) nmatch) / ((double) (sslot.nvalues - 2 * n_skip));
		}
		else
			result = -1;
		free_attstatsslot(&sslot);
	}
	else
	{
		*hist_size = 0;
		result = -1;
	}

	return result;
}

/*
 *	generic_restriction_selectivity		- 几乎任何算子的选择率
 *
 * 本函数为这样的算子估算选择率：我们对其没有
 * 任何专门知识，但其数据类型上我们收集了标准
 * MCV 和/或直方图统计信息。（额外的假定是
 * 算子严格且不可变，或至少是稳定的。）
 *
 * 若我们有 "VAR OP CONST" 或 "CONST OP VAR"，则
 * 选择率通过把算子应用于列的 MCV 和/或直方图
 * 统计信息的每个元素来估算，并用直方图是列的
 * 非 MCV 群体的合理随机样本这一假设来合并结果。
 * 注意若算子的语义与直方图排序相关，这可能
 * 不是那么好的假设；此类情形下 scalarineqsel()
 * 等其他函数可能是更好的匹配。
 *
 * 否则，回退到调用者提供的默认选择率。
 */
double
generic_restriction_selectivity(PlannerInfo *root, Oid oproid, Oid collation,
								List *args, int varRelid,
								double default_selectivity)
{
	double		selec;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;

	/*
	 * 若表达式不是 变量 OP 某物 或 某物 OP 变量的形式，
	 * 则放弃并返回默认估算。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return default_selectivity;

	/*
	 * 若某物为 NULL 常量，则假定算子严格并返回零，
	 * 即算子绝不会返回 TRUE。
	 */
	if (IsA(other, Const) &&
		((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}

	if (IsA(other, Const))
	{
		/* 变量正与已知的非 NULL 常量相比较 */
		Datum		constval = ((Const *) other)->constvalue;
		FmgrInfo	opproc;
		double		mcvsum;
		double		mcvsel;
		double		nullfrac;
		int			hist_size;

		fmgr_info(get_opcode(oproid), &opproc);

		/*
		 * 计算该列最常用值的选择率。
		 */
		mcvsel = mcv_selectivity(&vardata, &opproc, collation,
								 constval, varonleft,
								 &mcvsum);

		/*
		 * 若直方图足够大，看看它有多大比例
		 * 匹配查询，并假定这能代表非 MCV 群体；
		 * 否则对非 MCV 群体使用默认选择率。
		 *
		 */
		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, varonleft,
									  10, 1, &hist_size);
		if (selec < 0)
		{
			/* 否则回退到默认值 */
			selec = default_selectivity;
		}
		else if (hist_size < 100)
		{
			/*
			 * 对 10 到 100 的直方图大小，我们把直方图
			 * 与默认选择率结合起来，直方图越大就越
			 * 信任直方图。
			 */
			double		hist_weight = hist_size / 100.0;

			selec = selec * hist_weight +
				default_selectivity * (1.0 - hist_weight);
		}

		/* 无论如何，都不要相信极小或极大的估算值 */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/* 别忘了把空值算进去。 */
		if (HeapTupleIsValid(vardata.statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata.statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;

		/*
		 * 现在合并 MCV 与直方图计算的结果，注意
		 * 直方图只覆盖未列入 MCV 的非空值。
		 *
		 */
		selec *= 1.0 - nullfrac - mcvsum;
		selec += mcvsel;
	}
	else
	{
		/* 比较值不是常量，因此我们无能为力 */
		selec = default_selectivity;
	}

	ReleaseVariableStats(vardata);

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	ineq_histogram_selectivity	- 为 scalarineqsel 检查直方图
 *
 * 确定变量的直方图群体中满足不等式条件的
 * 比例，即 VAR <（或 <=、>、>=）CONST。
 * isgt 与 iseq 标志区分四种情形中哪一种适用。
 *
 * 虽然 opproc 可以从算子 OID 查出，但常见的调用者
 * 还需单独调用它，故让调用者把两者都传入。
 *
 * 若无直方图则返回 -1（有效结果总是 >= 0）。
 *
 * 注意结果会无视最常用值（若有）与空值条目。
 * 调用者应将此结果与列群体中这些部分的
 * 统计信息相结合。
 *
 * 导出此函数是为了让其他估算函数能够使用它。
 */
double
ineq_histogram_selectivity(PlannerInfo *root,
						   VariableStatData *vardata,
						   Oid opoid, FmgrInfo *opproc, bool isgt, bool iseq,
						   Oid collation,
						   Datum constval, Oid consttype)
{
	double		hist_selec;
	AttStatsSlot sslot;

	hist_selec = -1.0;

	/*
	 * 将来 ANALYZE 也许会为每个 rel/att 存多个直方图，
	 * 对应于为该列类型定义的多种可能排序顺序。
	 * 现在我们知道只有一个，所以直接取来
	 * 看它是否匹配查询。
	 *
	 * 注意我们不能把 opoid 用作搜索参数；pg_statistic
	 * 里的 staop 是相关 '<' 算子的，而我们手头的
	 * 可能是其他不等式算子，比如 '>='。（即便 opoid
	 * 是 '<' 算子，也可能跨类型。）因此必须用
	 * comparison_ops_are_compatible() 来看算子是否匹配。
	 */
	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.nvalues > 1 &&
			sslot.stacoll == collation &&
			comparison_ops_are_compatible(sslot.staop, opoid))
		{
			/*
			 * 用二分查找定位目标位置，即包含比较值的
			 * 那个直方图桶的右端，它是最左边的那个使
			 * 比较算子成功（若 isgt）或失败（若 !isgt）
			 *
			 *
			 * 此循环中我们不理会算子是否为 iseq，
			 * 这个细节留到下文处理。（反正我们也无从
			 * 得知算子认为这些值是否相等。）
			 *
			 * 若二分查找触及第一个或最后一个直方图
			 * 条目，我们就尝试把该端点替换为
			 * get_actual_variable_range() 找到的真实
			 * 最小或最大值。这能改善当自上次 ANALYZE
			 * 以来最小或最大值因变化而移动时的误估。
			 * 注意这实际上可能把此前不在直方图中的
			 * MCV 并入直方图，但我们不去纠正这一点。
			 */
			double		histfrac;
			int			lobound = 0;	/* 首个可搜索的槽位 */
			int			hibound = sslot.nvalues;	/* 搜索的末槽位+1 */
			bool		have_end = false;

			/*
			 * 若直方图只有两个条目，我们会想要两者都是
			 * 最新的值。（若超过两个，我们至多只需要
			 * 更新其中一个，这在循环内处理。）
			 *
			 */
			if (sslot.nvalues == 2)
				have_end = get_actual_variable_range(root,
													 vardata,
													 sslot.staop,
													 collation,
													 &sslot.values[0],
													 &sslot.values[1]);

			while (lobound < hibound)
			{
				int			probe = (lobound + hibound) / 2;
				bool		ltcmp;

				/*
				 * 若我们发现自己即将与第一个或最后一个
				 * 直方图条目比较，先尝试换成实际的
				 * 当前最小或最大值（除非上面已做）。
				 */
				if (probe == 0 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 &sslot.values[0],
														 NULL);
				else if (probe == sslot.nvalues - 1 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 NULL,
														 &sslot.values[probe]);

				ltcmp = DatumGetBool(FunctionCall2Coll(opproc,
													   collation,
													   sslot.values[probe],
													   constval));
				if (isgt)
					ltcmp = !ltcmp;
				if (ltcmp)
					lobound = probe + 1;
				else
					hibound = probe;
			}

			if (lobound <= 0)
			{
				/*
				 * 常量低于直方图下界。更准确地说，
				 * 我们发现直方图中没有条目满足不等式
				 * 子句（若 !isgt），或全满足（若 isgt）。
				 * 我们估计整张表也是如此，故把 histfrac
				 * 设为 0.0（若 isgt，下文翻转成 1.0）。
				 *
				 */
				histfrac = 0.0;
			}
			else if (lobound >= sslot.nvalues)
			{
				/*
				 * 相反情形：常量高于直方图上边界。
				 */
				histfrac = 1.0;
			}
			else
			{
				/* values[i-1] <= constant <= values[i]。 */
				int			i = lobound;
				double		eq_selec = 0;
				double		val,
							high,
							low;
				double		binfrac;

				/*
				 * 在下文需要的情形中，取得 "x = constval"
				 * 的选择率估算。我们用与 var_eq_const() 对
				 * 非 MCV 常量的做法类似的计算，即估计所有
				 * 不同的非 MCV 值出现得同样频繁。但乘以
				 * "1.0 - sumcommon - nullfrac" 的操作会由调用者
				 * 完成，所以我们不该在这里做。因此我们
				 * 无法试着参照最不常见的 MCV 来钳制该
				 * 估算；那样结果会太小。
				 *
				 * 注意：由于这实际上是假定 constval 不是
				 * MCV，那么若 constval 实际上是 MCV，逻辑上
				 * 是有疑义的。但我们总得对相等性做某种
				 * 校正，况且我们也无从判断 constval 是否为
				 * MCV，因为手头没有合适的相等算子。
				 */
				if (i == 1 || isgt == iseq)
				{
					double		otherdistinct;
					bool		isdefault;
					AttStatsSlot mcvslot;

					/* 取得估计的不同值数目 */
					otherdistinct = get_variable_numdistinct(vardata,
															 &isdefault);

					/* 减去已知 MCV 的数目 */
					if (get_attstatsslot(&mcvslot, vardata->statsTuple,
										 STATISTIC_KIND_MCV, InvalidOid,
										 ATTSTATSSLOT_NUMBERS))
					{
						otherdistinct -= mcvslot.nnumbers;
						free_attstatsslot(&mcvslot);
					}

					/* 若结果不合理，eq_selec 保持为 0 */
					if (otherdistinct > 1)
						eq_selec = 1.0 / otherdistinct;
				}

				/*
				 * 把常量与最近两个桶边界值转换到统一
				 * 比较尺度，并在该桶内做线性插值。
				 *
				 */
				if (convert_to_scalar(constval, consttype, collation,
									  &val,
									  sslot.values[i - 1], sslot.values[i],
									  vardata->vartype,
									  &low, &high))
				{
					if (high <= low)
					{
						/* 应对桶边界看似相同的情况 */
						binfrac = 0.5;
					}
					else if (val <= low)
						binfrac = 0.0;
					else if (val >= high)
						binfrac = 1.0;
					else
					{
						binfrac = (val - low) / (high - low);

						/*
						 * 当心我们可能从除法得到 NaN 或
						 * Infinity。前述检查也拦不住，比如
						 * "low" 为 -Infinity。
						 *
						 */
						if (isnan(binfrac) ||
							binfrac < 0.0 || binfrac > 1.0)
							binfrac = 0.5;
					}
				}
				else
				{
					/*
					 * 理想情况下我们该在此报错，理由是：
					 * 给定算子不应登记 scalarXXsel
					 * 为它的选择率函数，除非我们能处理其
					 * 操作数类型。但当前各种东西都在调用
					 * scalarXXsel，所以先给默认估算，
					 * 直到那能被修复为止。
					 */
					binfrac = 0.5;
				}

				/*
				 * 现在计算直方图各值上的整体选择率。
				 * 我们有 i-1 个整桶，常量之下还有
				 * binfrac 个部分桶。
				 */
				histfrac = (double) (i - 1) + binfrac;
				histfrac /= (double) (sslot.nvalues - 1);

				/*
				 * 此时 histfrac 是直方图所代表群体中满足
				 * "x <= constval" 群体的比例。略显惊人的是，
				 * 这一论断无论用哪个算子探测都成立，
				 * 只要 convert_to_scalar() 给出合理的结果。
				 *
				 * 若探测常量等于某个直方图条目，用 "<"
				 * 或 ">=" 探测则算入该条目左侧的桶，
				 * 用 "<=" 或 ">" 探测则算入右侧的桶；但
				 * binfrac 在前一情形下为 1.0，后一情形下
				 * 为 0.0，两种情形的 histfrac 相同。对介于
				 * 直方图条目之间的探测常量，任何算子都
				 * 找到同一个桶并得到相同的估算。
				 *
				 *
				 * 估算对应 "x <= constval" 而非 "x < constval"
				 * 的原因在于 ANALYZE 构建直方图的方式：
				 * 每个条目实际上是其样本桶的最右值。
				 * 所以那些选择率值若恰好是
				 * 1/(histogram_size-1) 的整数倍，就应理解为
				 * 包含一个直方图条目加其左侧全部的
				 * 估算。
				 *
				 * 然而，这对第一个直方图条目不成立，
				 * 因为它必然是其样本桶中最左边的值。
				 * 这意味着第一个直方图桶比其余的略窄，
				 * 窄的量正好等于 eq_selec。
				 * 换句话说，我们想要 "x <= leftmost" 被估算为
				 * eq_selec 而非零。因此若我们处理的是
				 * 第一个桶（i==1），就重缩放使它成立，
				 * 对该桶其余部分线性调整。
				 */
				if (i == 1)
					histfrac += eq_selec * (1.0 - binfrac);

				/*
				 * 若我们要为 "<=" 或 ">" 估算，"x <= constval"
				 * 是好的；但若为 "<" 或 ">=" 估算，就得
				 * 把估算减去 eq_selec。
				 */
				if (isgt == iseq)
					histfrac -= eq_selec;
			}

			/*
			 * 现在 "<" 与 "<=" 情形的估算已完成。若我们
			 * 为 ">"、">=" 则翻转它。
			 */
			hist_selec = isgt ? (1.0 - histfrac) : histfrac;

			/*
			 * 直方图边界本来就只是近似值，而且很可能
			 * 早已过时。因此，不要相信极小或极大的
			 * 选择率估算 --- 除非我们从表得到实际的
			 * 当前端点值，此时只需做常规合理性
			 * 钳制。我们多少有些武断地把其他情形的
			 * 截止点设为直方图分辨率的百分之一。
			 *
			 */
			if (have_end)
				CLAMP_PROBABILITY(hist_selec);
			else
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}
		else if (sslot.nvalues > 1)
		{
			/*
			 * 若走到这里，说明我们有直方图但它的排序
			 * 不符合我们想要的方式。做暴力搜索，
			 * 看有多少条目满足比较条件，取该
			 * 比例作为估算。（这与 histogram_selectivity 的
			 * 内层循环一样；共享代码？）
			 */
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;

			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			fcinfo->args[1].value = constval;
			for (int i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				fcinfo->args[0].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			hist_selec = ((double) nmatch) / ((double) sslot.nvalues);

			/*
			 * 同上，钳制到直方图分辨率的百分之一。
			 * 这一情形肯定比常规情形更不可信，所以
			 * 我们不应相信精确的 0 或 1 选择率。（这种
			 * 情形下钳制是否该更严格？）
			 */
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}

		free_attstatsslot(&sslot);
	}

	return hist_selec;
}

/*
 * 那些只是简单调用 scalarineqsel() 的选择率估算器
 * 公共包装函数。
 */
static Datum
scalarineqsel_wrapper(PG_FUNCTION_ARGS, bool isgt, bool iseq)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	double		selec;

	/*
	 * 若表达式不是 变量 op 某物 或 某物 op 变量的形式，
	 * 则放弃并返回默认估算。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * 若某物也不是常量，我们就做不了任何有用的事。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	/*
	 * 若常量为 NULL，则假定算子是严格的并返回零，即
	 * 算子绝不会返回TRUE。
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * 强制变量位于左侧，以简化 scalarineqsel 的逻辑。
	 */
	if (!varonleft)
	{
		operator = get_commutator(operator);
		if (!operator)
		{
			/* 使用默认选择率（要不要改成抛错？） */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = !isgt;
	}

	/* 其余工作由 scalarineqsel() 完成。 */
	selec = scalarineqsel(root, operator, isgt, iseq, collation,
						  &vardata, constval, consttype);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		scalarltsel		- 标量 "<" 的选择率。
 */
Datum
scalarltsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, false);
}

/*
 *		scalarlesel		- 标量 "<=" 的选择率。
 */
Datum
scalarlesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, true);
}

/*
 *		scalargtsel		- 标量 ">" 的选择率。
 */
Datum
scalargtsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, false);
}

/*
 *		scalargesel		- 标量 ">=" 的选择率。
 */
Datum
scalargesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, true);
}

/*
 *		boolvarsel		- 布尔变量的选择率。
 *
 * 实际上本函数可用于任何布尔值表达式。若它只
 * 涉及指定关系的 Var，并且有关于该 Var 或表达式的
 * 统计信息（后者在其被索引时可能出现），我们就会
 * 给出真实的估算；否则就只是默认值。
 */
Selectivity
boolvarsel(PlannerInfo *root, Node *arg, int varRelid)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		/*
		 * 布尔变量 V 等价于子句 V = 't'，所以我们
		 * 就当作拥有的是该子句来计算选择率。
		 */
		selec = var_eq_const(&vardata, BooleanEqualOperator, InvalidOid,
							 BoolGetDatum(true), false, true, false);
	}
	else
	{
		/* 否则默认估算为 0.5 */
		selec = 0.5;
	}
	ReleaseVariableStats(vardata);
	return selec;
}

/*
 *		booltestsel		- BooleanTest 节点选择率。
 */
Selectivity
booltestsel(PlannerInfo *root, BoolTestType booltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)
			&& sslot.nnumbers > 0)
		{
			double		freq_true;
			double		freq_false;

			/*
			 * 取得第一个 MCV 频率并推导 true 的频率。
			 */
			if (DatumGetBool(sslot.values[0]))
				freq_true = sslot.numbers[0];
			else
				freq_true = 1.0 - sslot.numbers[0] - freq_null;

			/*
			 * 接着推导 false 的频率。然后酌情用它们推导
			 * 各情形的频率。
			 */
			freq_false = 1.0 - freq_true - freq_null;

			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* 只选 NULL 值 */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* 选非 NULL 值 */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
					/* 只选 TRUE 值 */
					selec = freq_true;
					break;
				case IS_NOT_TRUE:
					/* 选非 TRUE 值 */
					selec = 1.0 - freq_true;
					break;
				case IS_FALSE:
					/* 只选 FALSE 值 */
					selec = freq_false;
					break;
				case IS_NOT_FALSE:
					/* 选非 FALSE 值 */
					selec = 1.0 - freq_false;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* 让编译器安静 */
					break;
			}

			free_attstatsslot(&sslot);
		}
		else
		{
			/*
			 * 没有最常用值信息。但仍拥有 NULL 比例信息，
			 * 于是对 IS [NOT] UNKNOWN 用它。否则调整
			 * NULL 比例并假定 TRUE 与 FALSE 各占一半。
			 */
			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* 只选 NULL 值 */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* 选非 NULL 值 */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
				case IS_FALSE:
					/* 假定我们选中一半的非 NULL 值 */
					selec = (1.0 - freq_null) / 2.0;
					break;
				case IS_NOT_TRUE:
				case IS_NOT_FALSE:
					/* 假定我们选中 NULL 加一半的非 NULL 值 */
					/* = freq_null + (1.0 - freq_null) / 2.0 */
					selec = (freq_null + 1.0) / 2.0;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* 让编译器安静 */
					break;
			}
		}
	}
	else
	{
		/*
		 * 若我们无法取得参数的变量统计信息，也许
		 * clause_selectivity 能对它做点什么。使用
		 * clause_selectivity 时我们忽略 NULL 值的可能性，
		 * 假定值要么 TRUE 要么 FALSE。
		 */
		switch (booltesttype)
		{
			case IS_UNKNOWN:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_UNKNOWN:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			case IS_TRUE:
			case IS_NOT_FALSE:
				selec = (double) clause_selectivity(root, arg,
													varRelid,
													jointype, sjinfo);
				break;
			case IS_FALSE:
			case IS_NOT_TRUE:
				selec = 1.0 - (double) clause_selectivity(root, arg,
														  varRelid,
														  jointype, sjinfo);
				break;
			default:
				elog(ERROR, "unrecognized booltesttype: %d",
					 (int) booltesttype);
				selec = 0.0;	/* 让编译器安静 */
				break;
		}
	}

	ReleaseVariableStats(vardata);

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 *		nulltestsel		- NullTest 节点选择率。
 */
Selectivity
nulltestsel(PlannerInfo *root, NullTestType nulltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		switch (nulltesttype)
		{
			case IS_NULL:

				/*
				 * 直接用 freq_null。
				 */
				selec = freq_null;
				break;
			case IS_NOT_NULL:

				/*
				 * 选出非未知（即非 NULL）值，由 freq_null
				 * 算。
				 */
				selec = 1.0 - freq_null;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* 让编译器安静 */
		}
	}
	else if (vardata.var && IsA(vardata.var, Var) &&
			 ((Var *) vardata.var)->varattno < 0)
	{
		/*
		 * 系统列无统计信息，但它们绝不为 NULL。
		 *
		 */
		selec = (nulltesttype == IS_NULL) ? 0.0 : 1.0;
	}
	else
	{
		/*
		 * 没有 ANALYZE 统计信息，只能猜测
		 */
		switch (nulltesttype)
		{
			case IS_NULL:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_NULL:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* 让编译器安静 */
		}
	}

	ReleaseVariableStats(vardata);

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 * strip_array_coercion - 从数组表达式剥离二进制兼容重标注。
 *
 * 对数组值，解析器通常会生成 ArrayCoerceExpr 转换，
 * 但 RelabelType 看起来也可能出现。另外，规划器目前
 * 并不急于折叠堆叠的 ArrayCoerceExpr 节点，
 * 所以我们要准备好处理多于一层的情况。
 */
static Node *
strip_array_coercion(Node *node)
{
	for (;;)
	{
		if (node && IsA(node, ArrayCoerceExpr))
		{
			ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

			/*
			 * 若逐元素表达式只是 CaseTestExpr 之上的一个
			 * RelabelType，我们就知道那是二进制兼容重标注。
			 */
			if (IsA(acoerce->elemexpr, RelabelType) &&
				IsA(((RelabelType *) acoerce->elemexpr)->arg, CaseTestExpr))
				node = (Node *) acoerce->arg;
			else
				break;
		}
		else if (node && IsA(node, RelabelType))
		{
			/* 并不真预期这种情形，但不妨应对 */
			node = (Node *) ((RelabelType *) node)->arg;
		}
		else
			break;
	}
	return node;
}

/*
 *		scalararraysel		- ScalarArrayOpExpr 节点选择率。
 */
Selectivity
scalararraysel(PlannerInfo *root,
			   ScalarArrayOpExpr *clause,
			   bool is_join_clause,
			   int varRelid,
			   JoinType jointype,
			   SpecialJoinInfo *sjinfo)
{
	Oid			operator = clause->opno;
	bool		useOr = clause->useOr;
	bool		isEquality = false;
	bool		isInequality = false;
	Node	   *leftop;
	Node	   *rightop;
	Oid			nominal_element_type;
	Oid			nominal_element_collation;
	TypeCacheEntry *typentry;
	RegProcedure oprsel;
	FmgrInfo	oprselproc;
	Selectivity s1;
	Selectivity s1disjoint;

	/* 首先，解构该表达式 */
	Assert(list_length(clause->args) == 2);
	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);

	/* 积极地把两侧都化归为常量 */
	leftop = estimate_expression_value(root, leftop);
	rightop = estimate_expression_value(root, rightop);

	/* 取得 rightop 名义（重标注后）元素类型 */
	nominal_element_type = get_base_element_type(exprType(rightop));
	if (!OidIsValid(nominal_element_type))
		return (Selectivity) 0.5;	/* 按理不该发生 */
	/* 也取得用于生成常量的名义排序规则 */
	nominal_element_collation = exprCollation(rightop);

	/* 看穿 rightop 的任何二进制兼容重标注 */
	rightop = strip_array_coercion(rightop);

	/*
	 * 检测该算子是否为数组元素类型的默认相等或
	 * 不等算子。
	 */
	typentry = lookup_type_cache(nominal_element_type, TYPECACHE_EQ_OPR);
	if (OidIsValid(typentry->eq_opr))
	{
		if (operator == typentry->eq_opr)
			isEquality = true;
		else if (get_negator(operator) == typentry->eq_opr)
			isInequality = true;
	}

	/*
	 * 若它是相等或不等算子，我们也许能把它当作
	 * 一种数组包含来估算；比如 "const = ANY(column)" 可
	 * 当作 "ARRAY[const] <@ column"。scalararraysel_containment
	 * 会尝试这点，成功则返回选择率估算，否则返回 -1。
	 */
	if ((isEquality || isInequality) && !is_join_clause)
	{
		s1 = scalararraysel_containment(root, leftop, rightop,
										nominal_element_type,
										isEquality, useOr, varRelid);
		if (s1 >= 0.0)
			return s1;
	}

	/*
	 * 查出底层算子的选择率估算器。若没有就放弃。
	 *
	 */
	if (is_join_clause)
		oprsel = get_oprjoin(operator);
	else
		oprsel = get_oprrest(operator);
	if (!oprsel)
		return (Selectivity) 0.5;
	fmgr_info(oprsel, &oprselproc);

	/*
	 * 在上面的数组包含检查中，只有当算子是元素
	 * 类型的默认 btree 相等算子（或其取反算子）时
	 * 我们才能相信它是相等或不等算子，因为那些
	 * 正是数组包含会用的算子。但在下文中可以
	 * 稍宽松些：任何以 eqsel() 或 neqsel() 为选择
	 * 率估算器的算子也表现得像相等或不等。
	 */
	if (oprsel == F_EQSEL || oprsel == F_EQJOINSEL)
		isEquality = true;
	else if (oprsel == F_NEQSEL || oprsel == F_NEQJOINSEL)
		isInequality = true;

	/*
	 * 考虑三种情形：
	 *
	 * 1. rightop 是 Array 常量：解构数组，对每个数组
	 * 元素应用算子的选择率函数，然后按 clausesel.c
	 * 对 AND/OR 组合所用的同一方式合并结果。
	 *
	 * 2. rightop 是 ARRAY[] 构造：对该构造的每个元素
	 * 应用算子的选择率函数，然后合并。
	 *
	 * 3. 否则，做个猜测 …
	 */
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			i;

		if (arrayisnull)		/* NULL 数组使 qual 无法成功 */
			return (Selectivity) 0.0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * 对通用算子，假定成功的概率对每个数组
		 * 元素是独立的。但对 "= ANY" 或 "<> ALL"，若数组
		 * 元素互不相同（通常正是如此），那么这些概率
		 * 互斥，我们应该直接把它们求和。
		 *
		 * 若我们真的非常谨慎，就会试着确认元素
		 * 都互不相同，但那样代价高昂，而且似乎
		 * 不值得花那些工夫；那等于惩罚写得好
		 * 的查询而优待写得差的查询。不过，我们
		 * 确实留了一点自保手段：检查
		 * 不相交假设是否导出不可能（越界）概率；
		 * 若是，就回退到常规计算。
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		for (i = 0; i < num_elems; i++)
		{
			List	   *args;
			Selectivity s2;

			args = list_make2(leftop,
							  makeConst(nominal_element_type,
										-1,
										nominal_element_collation,
										elmlen,
										elem_values[i],
										elem_nulls[i],
										elmbyval));
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* 若不相交概率估计在范围内则接受之 */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else if (rightop && IsA(rightop, ArrayExpr) &&
			 !((ArrayExpr *) rightop)->multidims)
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;
		int16		elmlen;
		bool		elmbyval;
		ListCell   *l;

		get_typlenbyval(arrayexpr->element_typeid,
						&elmlen, &elmbyval);

		/*
		 * 我们在这里也使用不相交概率的假定，尽管
		 * 若元素并非全为常量，相等元素出现的几率
		 * 要高不少（它们不会全为常量，否则常量折叠
		 * 早把 ArrayExpr 化归为 Const）。在这一路径中，
		 * 对 s1disjoint 估计做合理性检查至关重要。
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		foreach(l, arrayexpr->elements)
		{
			Node	   *elem = (Node *) lfirst(l);
			List	   *args;
			Selectivity s2;

			/*
			 * 理论上，若 elem 非名义元素类型，我们应插入
			 * 一个 RelabelType，但似乎没哪个算子估算
			 * 函数真会在意这一点…
			 */
			args = list_make2(leftop, elem);
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* 若不相交概率估计在范围内则接受之 */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else
	{
		CaseTestExpr *dummyexpr;
		List	   *args;
		Selectivity s2;
		int			i;

		/*
		 * 需要一个哑 rightop 传给算子选择率例程。
		 * 它几乎可以是任何看起来不像常量的东西；
		 * CaseTestExpr 是个方便的选择。
		 */
		dummyexpr = makeNode(CaseTestExpr);
		dummyexpr->typeId = nominal_element_type;
		dummyexpr->typeMod = -1;
		dummyexpr->collation = clause->inputcollid;
		args = list_make2(leftop, dummyexpr);
		if (is_join_clause)
			s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int16GetDatum(jointype),
												  PointerGetDatum(sjinfo)));
		else
			s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int32GetDatum(varRelid)));
		s1 = useOr ? 0.0 : 1.0;

		/*
		 * 武断地假定最终数组值有 10 个元素（另见
		 * estimate_array_length）。这里我们不冒
		 * 不相交概率之险。
		 */
		for (i = 0; i < 10; i++)
		{
			if (useOr)
				s1 = s1 + s2 - s1 * s2;
			else
				s1 = s1 * s2;
		}
	}

	/* 结果应在范围内，但请确认…… */
	CLAMP_PROBABILITY(s1);

	return s1;
}

/*
 * 估计一个表达式产生的数组中的元素数目。
 *
 * 此估计与 scalararraysel 一致很重要。
 */
int
estimate_array_length(Node *arrayexpr)
{
	/* 透过 arrayexpr 的任何二进制兼容 relabeling */
	arrayexpr = strip_array_coercion(arrayexpr);

	if (arrayexpr && IsA(arrayexpr, Const))
	{
		Datum		arraydatum = ((Const *) arrayexpr)->constvalue;
		bool		arrayisnull = ((Const *) arrayexpr)->constisnull;
		ArrayType  *arrayval;

		if (arrayisnull)
			return 0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		return ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
	}
	else if (arrayexpr && IsA(arrayexpr, ArrayExpr) &&
			 !((ArrayExpr *) arrayexpr)->multidims)
	{
		return list_length(((ArrayExpr *) arrayexpr)->elements);
	}
	else
	{
		/* 默认猜测 --- 另见 scalararraysel */
		return 10;
	}
}

/*
 *		rowcomparesel		- RowCompareExpr 节点选择率。
 *
 * 我们只考虑第一（高阶）列来估计 RowCompare 的
 * 选择率，这使它等价于一个普通 OpExpr。虽然
 * 这一估计还可以通过考虑更多列来精化，但若没有
 * 多列统计信息，我们似乎难以做得好很多。
 *
 */
Selectivity
rowcomparesel(PlannerInfo *root,
			  RowCompareExpr *clause,
			  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	Selectivity s1;
	Oid			opno = linitial_oid(clause->opnos);
	Oid			inputcollid = linitial_oid(clause->inputcollids);
	List	   *opargs;
	bool		is_join_clause;

	/* 为单个算子构建等价的参数列表 */
	opargs = list_make2(linitial(clause->largs), linitial(clause->rargs));

	/*
	 * 判断它是否为连接子句。这应与 clausesel.c 的
	 * treat_as_join_clause() 一致，只是我们有意只考虑
	 * 前导列而不考虑子句的其余部分。
	 */
	if (varRelid != 0)
	{
		/*
		 * 调用者强制限制模式（例如正在检查内侧索引扫描
		 * 的限定）。
		 */
		is_join_clause = false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * 它必是限制子句，因为它在扫描节点处求值。
		 *
		 */
		is_join_clause = false;
	}
	else
	{
		/*
		 * 否则，只要用到了多于一个基关系，它就是连接。
		 */
		is_join_clause = (NumRelids(root, (Node *) opargs) > 1);
	}

	if (is_join_clause)
	{
		/* 估计连接子句的选择率。 */
		s1 = join_selectivity(root, opno,
							  opargs,
							  inputcollid,
							  jointype,
							  sjinfo);
	}
	else
	{
		/* 估计限制子句的选择率。 */
		s1 = restriction_selectivity(root, opno,
									 opargs,
									 inputcollid,
									 varRelid);
	}

	return s1;
}

/*
 *		eqjoinsel		- "=" 的连接选择率
 */
Datum
eqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);

#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	double		selec;
	double		selec_inner;
	VariableStatData vardata1;
	VariableStatData vardata2;
	double		nd1;
	double		nd2;
	bool		isdefault1;
	bool		isdefault2;
	Oid			opfuncoid;
	AttStatsSlot sslot1;
	AttStatsSlot sslot2;
	Form_pg_statistic stats1 = NULL;
	Form_pg_statistic stats2 = NULL;
	bool		have_mcvs1 = false;
	bool		have_mcvs2 = false;
	bool		get_mcv_stats;
	bool		join_is_reversed;
	RelOptInfo *inner_rel;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	nd1 = get_variable_numdistinct(&vardata1, &isdefault1);
	nd2 = get_variable_numdistinct(&vardata2, &isdefault2);

	opfuncoid = get_opcode(operator);

	memset(&sslot1, 0, sizeof(sslot1));
	memset(&sslot2, 0, sizeof(sslot2));

	/*
	 * 若缺少另一侧的 MCV，取这一侧的 MCV 也没有用，
	 * 所以先快速检查确认两侧的统计信息都存在。
	 */
	get_mcv_stats = (HeapTupleIsValid(vardata1.statsTuple) &&
					 HeapTupleIsValid(vardata2.statsTuple) &&
					 get_attstatsslot(&sslot1, vardata1.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0) &&
					 get_attstatsslot(&sslot2, vardata2.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0));

	if (HeapTupleIsValid(vardata1.statsTuple))
	{
		/* 注意我们允许无视安全检查使用 nullfrac */
		stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata1, opfuncoid))
			have_mcvs1 = get_attstatsslot(&sslot1, vardata1.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	if (HeapTupleIsValid(vardata2.statsTuple))
	{
		/* 注意我们允许无视安全检查使用 nullfrac */
		stats2 = (Form_pg_statistic) GETSTRUCT(vardata2.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata2, opfuncoid))
			have_mcvs2 = get_attstatsslot(&sslot2, vardata2.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	/* 无论何种情形都需要计算内连接选择率 */
	selec_inner = eqjoinsel_inner(opfuncoid, collation,
								  &vardata1, &vardata2,
								  nd1, nd2,
								  isdefault1, isdefault2,
								  &sslot1, &sslot2,
								  stats1, stats2,
								  have_mcvs1, have_mcvs2);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:
			selec = selec_inner;
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:

			/*
			 * 查出连接的内侧关系。min_righthand 是足够的
			 * 信息，因为 SEMI 与 ANTI 连接都不允许任何
			 * 重结合进入或离开其 RHS，因此 righthand 将
			 * 始终恰为那个关系集合。
			 */
			inner_rel = find_join_input_rel(root, sjinfo->min_righthand);

			if (!join_is_reversed)
				selec = eqjoinsel_semi(opfuncoid, collation,
									   &vardata1, &vardata2,
									   nd1, nd2,
									   isdefault1, isdefault2,
									   &sslot1, &sslot2,
									   stats1, stats2,
									   have_mcvs1, have_mcvs2,
									   inner_rel);
			else
			{
				Oid			commop = get_commutator(operator);
				Oid			commopfuncoid = OidIsValid(commop) ? get_opcode(commop) : InvalidOid;

				selec = eqjoinsel_semi(commopfuncoid, collation,
									   &vardata2, &vardata1,
									   nd2, nd1,
									   isdefault2, isdefault1,
									   &sslot2, &sslot1,
									   stats2, stats1,
									   have_mcvs2, have_mcvs1,
									   inner_rel);
			}

			/*
			 * 我们绝不应把半连接的输出估计得比具有
			 * 相同输入与连接条件的内连接更多的行；
			 * 那显然不可能发生。前者的估计是 N1 * Ssemi，
			 * 后者是 N1 * N2 * Sinner，所以我们可以钳制
			 * Ssemi <= N2 * Sinner。这样做是值得的，因为
			 * 我们在 eqjoinsel_semi 中使用的估算规则要
			 * 动摇得多，特别是在它完全放弃的情形中。
			 *
			 */
			selec = Min(selec, inner_rel->rows * selec_inner);
			break;
		default:
			/* 不应出现其他取值 */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* 让编译器安静 */
			break;
	}

	free_attstatsslot(&sslot1);
	free_attstatsslot(&sslot2);

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * eqjoinsel_inner --- 常规内连接的 eqjoinsel
 *
 * 我们也把它用于 LEFT/FULL 外连接；目前还不
 * 清楚在这里区分它们是否值得。
 */
static double
eqjoinsel_inner(Oid opfuncoid, Oid collation,
				VariableStatData *vardata1, VariableStatData *vardata2,
				double nd1, double nd2,
				bool isdefault1, bool isdefault2,
				AttStatsSlot *sslot1, AttStatsSlot *sslot2,
				Form_pg_statistic stats1, Form_pg_statistic stats2,
				bool have_mcvs1, bool have_mcvs2)
{
	double		selec;

	if (have_mcvs1 && have_mcvs2)
	{
		/*
		 * 我们有两个关系的最常用值列表。遍历这两个
		 * 列表，看哪些 MCV 真的用给定算子彼此连接。
		 * 这让我们能够确定 MCV 列表所代表的那部分
		 * 关系的精确连接选择率。对其余群体我们仍需
		 * 估计，但在偏斜分布下这让我们的准确度大为
		 * 改观。动机参见 Y. Ioannidis 与 S.
		 * Christodoulakis 的论文 "On the propagation
		 * of errors in the size of join results"（论连接
		 * 结果规模中的错误传播），Technical Report 1018,
		 * Computer Science Dept., University of Wisconsin, Madison, 1991.
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		nullfrac2 = stats2->stanullfrac;
		double		matchprodfreq,
					matchfreq1,
					matchfreq2,
					unmatchfreq1,
					unmatchfreq2,
					otherfreq1,
					otherfreq2,
					totalsel1,
					totalsel2;
		int			i,
					nmatches;

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * 只设置一次 fcinfo 结构即可节省几步开销。直接
		 * 使用 FunctionCallInvoke 还能避免在 eqproc
		 * 返回 NULL 时失败，虽然相等函数绝不该这样。
		 *
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(sslot2->nvalues * sizeof(bool));

		/*
		 * 注意我们假定每个 MCV 至多与另一 MCV 列表的
		 * 一个成员匹配。若算子并非真的是相等，可能有
		 * 多个匹配 --- 但我们不去找它们，一是为了速度，
		 * 二是因为数学上说不通…
		 */
		matchprodfreq = 0.0;
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < sslot2->nvalues; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					matchprodfreq += sslot1->numbers[i] * sslot2->numbers[j];
					nmatches++;
					break;
				}
			}
		}
		CLAMP_PROBABILITY(matchprodfreq);
		/* 累加匹配与未匹配 MCV 的频率 */
		matchfreq1 = unmatchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
			else
				unmatchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		CLAMP_PROBABILITY(unmatchfreq1);
		matchfreq2 = unmatchfreq2 = 0.0;
		for (i = 0; i < sslot2->nvalues; i++)
		{
			if (hasmatch2[i])
				matchfreq2 += sslot2->numbers[i];
			else
				unmatchfreq2 += sslot2->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq2);
		CLAMP_PROBABILITY(unmatchfreq2);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * 计算不在 MCV 列表中的非空值的总频率。
		 *
		 */
		otherfreq1 = 1.0 - nullfrac1 - matchfreq1 - unmatchfreq1;
		otherfreq2 = 1.0 - nullfrac2 - matchfreq2 - unmatchfreq2;
		CLAMP_PROBABILITY(otherfreq1);
		CLAMP_PROBABILITY(otherfreq2);

		/*
		 * 我们能从关系 1 的视角估计总选择率：已匹配
		 * MCV 的已知选择率，加上假定与关系 2 的非
		 * MCV 群体的随机成员匹配的未匹配 MCV，加上
		 * 假定与关系 2 的未匹配 MCV 加非 MCV 值的
		 * 随机成员相匹配的非 MCV 值。
		 *
		 */
		totalsel1 = matchprodfreq;
		if (nd2 > sslot2->nvalues)
			totalsel1 += unmatchfreq1 * otherfreq2 / (nd2 - sslot2->nvalues);
		if (nd2 > nmatches)
			totalsel1 += otherfreq1 * (otherfreq2 + unmatchfreq2) /
				(nd2 - nmatches);
		/* 从关系 2 的视角做同样的估计。 */
		totalsel2 = matchprodfreq;
		if (nd1 > sslot1->nvalues)
			totalsel2 += unmatchfreq2 * otherfreq1 / (nd1 - sslot1->nvalues);
		if (nd1 > nmatches)
			totalsel2 += otherfreq2 * (otherfreq1 + unmatchfreq1) /
				(nd1 - nmatches);

		/*
		 * 使用两个估计中较小的一个。这可以按本质上
		 * 与下文无统计信息情形所给的理由来论证：作为
		 * 一阶近似，我们是在从具有较小 nd 的关系的
		 * 视角进行估计的。
		 */
		selec = (totalsel1 < totalsel2) ? totalsel1 : totalsel2;
	}
	else
	{
		/*
		 * 我们没有两侧的 MCV 列表。把连接选择率估计
		 * 为 MIN(1/nd1,1/nd2)*(1-nullfrac1)*(1-nullfrac2)。
		 * 如果我们假定连接算子是严格的且非空值近似
		 * 均匀分布，这就是合理的：rel1 的一个给定非空
		 * 元组将与 rel2 的零行或 N2*(1-nullfrac2)/nd2
		 * 行连接，故总连接行数至多为
		 * N1*(1-nullfrac1)*N2*(1-nullfrac2)/nd2，给出
		 * 不超过 (1-nullfrac1)*(1-nullfrac2)/nd2 的连接
		 * 选择率。按同样逻辑它也不高于
		 * (1-nullfrac1)*(1-nullfrac2)/nd1，所以带
		 * MIN() 的表达式是一个上界。使用 MIN() 意味着
		 * 我们从 nd 较小的关系的视角进行估计（因为较大
		 * 的 nd 决定了 MIN）。假定该关系的大多数元组
		 * 都有连接伙伴，此界相当紧，可照用。
		 *
		 * XXX 若我们只有一侧的 MCV 列表，能更聪明些吗？
		 * 看起来即使我们假定另一侧均匀分布，最终也会
		 * 得到同样的答案。
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;
		double		nullfrac2 = stats2 ? stats2->stanullfrac : 0.0;

		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2);
		if (nd1 > nd2)
			selec /= nd1;
		else
			selec /= nd2;
	}

	return selec;
}

/*
 * eqjoinsel_semi --- 半连接 eqjoinsel
 *
 * （也用于反连接，按约定估计方式相同。）调用者
 * 已确保 vardata1 是 LHS 变量。与
 * eqjoinsel_inner 不同，我们必须应对 opfuncoid 为 InvalidOid。
 */
static double
eqjoinsel_semi(Oid opfuncoid, Oid collation,
			   VariableStatData *vardata1, VariableStatData *vardata2,
			   double nd1, double nd2,
			   bool isdefault1, bool isdefault2,
			   AttStatsSlot *sslot1, AttStatsSlot *sslot2,
			   Form_pg_statistic stats1, Form_pg_statistic stats2,
			   bool have_mcvs1, bool have_mcvs2,
			   RelOptInfo *inner_rel)
{
	double		selec;

	/*
	 * 我们把 nd2 钳制为不超过我们对内侧关系大小的
	 * 估计。这在直觉上颇为合理，因为显然来自内侧
	 * 关系的不可能有多于这个数目的不同值。不对称
	 * 的原因（即我们不同样钳制 nd1）在于：这是施加
	 * 于内侧关系的限制子句影响连接结果大小估计的
	 * 唯一途径，因为 set_joinrel_size_estimates 会把
	 * SEMI/ANTI 选择率只乘以外侧关系的大小。如果我们
	 * 钳制了 nd1，就会把外侧关系限制条件的选择率
	 * 重复计算。
	 *
	 * 我们可以对两种对象施加这一钳制：连接变量所
	 * 来自的基关系（若只有一个），以及当前连接的
	 * 直接内侧输入关系。
	 *
	 * 若我们做了钳制，就可以把 nd2 视为非默认估计；
	 * 它也许不算好，但它也不是凭空而来的。当内侧
	 * 关系为空因此没有统计信息时，这一点最有帮助。
	 */
	if (vardata2->rel)
	{
		if (nd2 >= vardata2->rel->rows)
		{
			nd2 = vardata2->rel->rows;
			isdefault2 = false;
		}
	}
	if (nd2 >= inner_rel->rows)
	{
		nd2 = inner_rel->rows;
		isdefault2 = false;
	}

	if (have_mcvs1 && have_mcvs2 && OidIsValid(opfuncoid))
	{
		/*
		 * 我们有两个关系的最常用值列表。遍历这两个
		 * 列表，看哪些 MCV 真的用给定算子彼此连接。
		 * 这让我们能够确定 MCV 列表所代表的那部分
		 * 关系的精确连接选择率。对其余群体我们仍需
		 * 估计，但在偏斜分布下这让我们的准确度大为
		 * 改观。动机参见 Y. Ioannidis 与 S.
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		matchfreq1,
					uncertainfrac,
					uncertain;
		int			i,
					nmatches,
					clamped_nvalues2;

		/*
		 * 上面的钳制可能导致 nd2 小于 sslot2->nvalues；
		 * 在这种情况下，我们假定恰好是 nd2 个最常用值
		 * 将出现在连接输入中，因此只与 MCV 列表的前
		 * nd2 个成员比较。当然这经常是错的，但这是
		 * 我们所能下的最好赌注。
		 */
		clamped_nvalues2 = Min(sslot2->nvalues, nd2);

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * 只设置一次 fcinfo 结构即可节省几步开销。直接
		 * 使用 FunctionCallInvoke 还能避免在 eqproc
		 * 返回 NULL 时失败，虽然相等函数绝不该这样。
		 *
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(clamped_nvalues2 * sizeof(bool));

		/*
		 * 注意我们假定每个 MCV 至多与另一 MCV 列表的
		 * 一个成员匹配。若算子并非真的是相等，可能有
		 * 多个匹配 --- 但我们不去找它们，一是为了速度，
		 * 二是因为数学上说不通…
		 */
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < clamped_nvalues2; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					nmatches++;
					break;
				}
			}
		}
		/* 累加匹配 MCV 的频率 */
		matchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * 现在我们需要估计关系 1 中拥有至少一个连接
		 * 伙伴的行所占的比例。可以肯定已匹配的 MCV
		 * 都有，这给了我们一个下界，但对其他一切我们
		 * 确实一无所知。我们的粗略做法是：若 nd1 <=
		 * nd2 则假定所有非空 rel1 行都有连接伙伴，
		 * 否则对不确定的行假定 nd2/nd1 的比例有连接
		 * 伙伴。我们可以在做除法之前从不同值计数中
		 * 扣减已匹配的 MCV。
		 *
		 * 尽管上面的做法很粗糙，如果我们没有两侧可靠
		 * 的 ndistinct 值，它就完全无用。因此，若 nd1
		 * 或 nd2 是默认值，则放弃并假定不确定行的一半
		 * 有伙伴。
		 */
		if (!isdefault1 && !isdefault2)
		{
			nd1 -= nmatches;
			nd2 -= nmatches;
			if (nd1 <= nd2 || nd2 < 0)
				uncertainfrac = 1.0;
			else
				uncertainfrac = nd2 / nd1;
		}
		else
			uncertainfrac = 0.5;
		uncertain = 1.0 - matchfreq1 - nullfrac1;
		CLAMP_PROBABILITY(uncertain);
		selec = matchfreq1 + uncertainfrac * uncertain;
	}
	else
	{
		/*
		 * 没有两侧的 MCV 列表时，我们只能用 nd1 与
		 * nd2 启发式。
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;

		if (!isdefault1 && !isdefault2)
		{
			if (nd1 <= nd2 || nd2 < 0)
				selec = 1.0 - nullfrac1;
			else
				selec = (nd2 / nd1) * (1.0 - nullfrac1);
		}
		else
			selec = 0.5 * (1.0 - nullfrac1);
	}

	return selec;
}

/*
 *		neqjoinsel		- "!=" 的连接选择率
 */
Datum
neqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	float8		result;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		/*
		 * 对半连接而言，若 RHS 关系中有多于一个
		 * 不同值，那么每个非空 LHS 行都必须找到一行
		 * 来连接，因为它只能等于其中之一。为了稳定
		 * 性，我们假定总是有多于一个不同 RHS 值，
		 * 尽管理论上我们可以为空 RHS（选择率 = 0）
		 * 和单一不同值 RHS（选择率 = 与该唯一 RHS 值
		 * 相同的 LHS 行所占比例）设置特殊情形。
		 *
		 * 对反连接而言，如果我们使用同样的"RHS
		 * 关系中有多于一个不同键"假定，那么每个非
		 * 空 LHS 行都必须被抑制。
		 *
		 * 所以无论如何，选择率估计都应为 1 - nullfrac。
		 */
		VariableStatData leftvar;
		VariableStatData rightvar;
		bool		reversed;
		HeapTuple	statsTuple;
		double		nullfrac;

		get_join_variables(root, args, sjinfo, &leftvar, &rightvar, &reversed);
		statsTuple = reversed ? rightvar.statsTuple : leftvar.statsTuple;
		if (HeapTupleIsValid(statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;
		ReleaseVariableStats(leftvar);
		ReleaseVariableStats(rightvar);

		result = 1.0 - nullfrac;
	}
	else
	{
		/*
		 * 我们想要 1 - eqjoinsel()，其中的相等算子是
		 * 与之相关联的那个，即它的取反算子。
		 */
		Oid			eqop = get_negator(operator);

		if (eqop)
		{
			result =
				DatumGetFloat8(DirectFunctionCall5Coll(eqjoinsel,
													   collation,
													   PointerGetDatum(root),
													   ObjectIdGetDatum(eqop),
													   PointerGetDatum(args),
													   Int16GetDatum(jointype),
													   PointerGetDatum(sjinfo)));
		}
		else
		{
			/* 使用默认选择率（要不要改成抛错？） */
			result = DEFAULT_EQ_SEL;
		}
		result = 1.0 - result;
	}

	PG_RETURN_FLOAT8(result);
}

/*
 *		scalarltjoinsel - 标量 "<" 的连接选择率
 */
Datum
scalarltjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalarlejoinsel - 标量 "<=" 的连接选择率
 */
Datum
scalarlejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargtjoinsel - 标量 ">" 的连接选择率
 */
Datum
scalargtjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargejoinsel - 标量 ">=" 的连接选择率
 */
Datum
scalargejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}


/*
 * mergejoinscansel			- 归并连接扫描选择率。
 *
 * 归并连接会在耗尽任一输入流时立即停止。
 * 因此，若我们能估计两个输入变量的范围，就
 * 能估计输入中实际会被读取的部分。这在使用
 * 索引扫描时对代价有相当大的影响。
 *
 * 此外，我们还能估计在找到第一对连接对之前
 * 每个输入必须读取多少，这会影响连接启动时间。
 *
 * 子句应当是已知可归并连接的子句。opfamily、
 * strategy 与 nulls_first 指定所用的排序顺序。
 *
 * 输出为：
 *		*leftstart 被设为左侧变量预计在找到第一对
 *		 连接对之前会被扫描的比例（0 到 1）。
 *		*leftend 被设为左侧变量预计在连接终止之前
 *		 会被扫描的比例（0 到 1）。
 *		*rightstart、*rightend 对右侧变量类似。
 */
void
mergejoinscansel(PlannerInfo *root, Node *clause,
				 Oid opfamily, int strategy, bool nulls_first,
				 Selectivity *leftstart, Selectivity *leftend,
				 Selectivity *rightstart, Selectivity *rightend)
{
	Node	   *left,
			   *right;
	VariableStatData leftvar,
				rightvar;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	Oid			opno,
				collation,
				lsortop,
				rsortop,
				lstatop,
				rstatop,
				ltop,
				leop,
				revltop,
				revleop;
	bool		isgt;
	Datum		leftmin,
				leftmax,
				rightmin,
				rightmax;
	double		selec;

	/* 若什么都算不出来，则设置默认结果。 */
	/* XXX 默认 "start" 比例是否应比 0 稍大？ */
	*leftstart = *rightstart = 0.0;
	*leftend = *rightend = 1.0;

	/* 分解 merge 子句 */
	if (!is_opclause(clause))
		return;					/* 不应发生 */
	opno = ((OpExpr *) clause)->opno;
	collation = ((OpExpr *) clause)->inputcollid;
	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);
	if (!right)
		return;					/* 不应发生 */

	/* 查找输入的统计信息 */
	examine_variable(root, left, 0, &leftvar);
	examine_variable(root, right, 0, &rightvar);

	/* 提取算子声明的左/右数据类型 */
	get_op_opfamily_properties(opno, opfamily, false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);
	Assert(op_strategy == BTEqualStrategyNumber);

	/*
	 * 查出我们需要的各个算子。若找不齐，多半意味着
	 * opfamily 有问题，但我们只是静默失败。
	 *
	 * 注意：我们预期 pg_statistic 直方图按 '<' 算子
	 * 排序，而不论我们正在考虑的是哪个排序方向。
	 */
	switch (strategy)
	{
		case BTLessStrategyNumber:
			isgt = false;
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessEqualStrategyNumber);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessEqualStrategyNumber);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTLessStrategyNumber);
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTLessStrategyNumber);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTLessEqualStrategyNumber);
			}
			break;
		case BTGreaterStrategyNumber:
			/* 降序情形 */
			isgt = true;
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterEqualStrategyNumber);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rstatop = lstatop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterEqualStrategyNumber);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTGreaterStrategyNumber);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTGreaterStrategyNumber);
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rstatop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTLessStrategyNumber);
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTGreaterStrategyNumber);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTGreaterEqualStrategyNumber);
			}
			break;
		default:
			goto fail;			/* 不应到这里 */
	}

	if (!OidIsValid(lsortop) ||
		!OidIsValid(rsortop) ||
		!OidIsValid(lstatop) ||
		!OidIsValid(rstatop) ||
		!OidIsValid(ltop) ||
		!OidIsValid(leop) ||
		!OidIsValid(revltop) ||
		!OidIsValid(revleop))
		goto fail;				/* 目录中信息不足 */

	/* 试取两个输入的范围 */
	if (!isgt)
	{
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmin, &leftmax))
			goto fail;			/* 统计无可用范围 */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmin, &rightmax))
			goto fail;			/* 统计无可用范围 */
	}
	else
	{
		/* 需交换最大最小值 */
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmax, &leftmin))
			goto fail;			/* 统计无可用范围 */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmax, &rightmin))
			goto fail;			/* 统计无可用范围 */
	}

	/*
	 * 现在，左侧变量将被扫描的比例是 <= 右侧最大
	 * 值的比例。但只相信非默认估计，否则维持我们
	 * 的 1.0。
	 */
	selec = scalarineqsel(root, leop, isgt, true, collation, &leftvar,
						  rightmax, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftend = selec;

	/* 对右侧变量做同样处理。 */
	selec = scalarineqsel(root, revleop, isgt, true, collation, &rightvar,
						  leftmax, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightend = selec;

	/*
	 * 两个 "end" 比例中真正能小于 1.0 的只有一个；
	 * 相信较小的估计并把另一个重置为恰好 1.0。如果
	 * 得到恰好相等的估计（自连接中很常见），
	 * 则都不信。
	 */
	if (*leftend > *rightend)
		*leftend = 1.0;
	else if (*leftend < *rightend)
		*rightend = 1.0;
	else
		*leftend = *rightend = 1.0;

	/*
	 * 此外，左侧变量在找到第一对连接对之前将被扫
	 * 描的比例是 < 右侧最小值的比例。但只相信
	 * 非默认估计，否则维持我们自己的默认值。
	 *
	 */
	selec = scalarineqsel(root, ltop, isgt, false, collation, &leftvar,
						  rightmin, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftstart = selec;

	/* 对右侧变量做同样处理。 */
	selec = scalarineqsel(root, revltop, isgt, false, collation, &rightvar,
						  leftmin, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightstart = selec;

	/*
	 * 两个 "start" 比例中真正能大于零的只有一个；
	 * 相信较大的估计并把另一个重置为恰好 0.0。如果
	 * 得到恰好相等的估计（自连接中很常见），
	 * 则都不信。
	 */
	if (*leftstart < *rightstart)
		*leftstart = 0.0;
	else if (*leftstart > *rightstart)
		*rightstart = 0.0;
	else
		*leftstart = *rightstart = 0.0;

	/*
	 * 若排序顺序是 nulls-first，我们将不得不跳过
	 * 任何空值。scalarineqsel 不会把这些计入，而
	 * 且无论我们是否相信 scalarineqsel 的结果，都
	 * 可以安全加入这一比例。但务必把总和钳到 1.0！
	 */
	if (nulls_first)
	{
		Form_pg_statistic stats;

		if (HeapTupleIsValid(leftvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(leftvar.statsTuple);
			*leftstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftstart);
			*leftend += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftend);
		}
		if (HeapTupleIsValid(rightvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(rightvar.statsTuple);
			*rightstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightstart);
			*rightend += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightend);
		}
	}

	/* 不相信 start >= end，以防万一 */
	if (*leftstart >= *leftend)
	{
		*leftstart = 0.0;
		*leftend = 1.0;
	}
	if (*rightstart >= *rightend)
	{
		*rightstart = 0.0;
		*rightend = 1.0;
	}

fail:
	ReleaseVariableStats(leftvar);
	ReleaseVariableStats(rightvar);
}


/*
 *	matchingsel	-- 通用匹配算子选择率支持
 *
 * 对满足以下条件的算子使用这些：(a) 其数据类型
 * 属于我们收集标准统计信息的类型，且 (b) 其行为
 * 使得默认估计（两倍 DEFAULT_EQ_SEL）是合理
 * 的。
 */

Datum
matchingsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	double		selec;

	/* 使用通用限制选择率逻辑。 */
	selec = generic_restriction_selectivity(root, operator, collation,
											args, varRelid,
											DEFAULT_MATCHING_SEL);

	PG_RETURN_FLOAT8((float8) selec);
}

Datum
matchingjoinsel(PG_FUNCTION_ARGS)
{
	/* 目前先放弃。 */
	PG_RETURN_FLOAT8(DEFAULT_MATCHING_SEL);
}


/*
 * estimate_num_groups 的辅助例程：向 GroupVarInfos
 * 列表添加条目，但仅当它不与任何既有条目相等。
 *
 */
typedef struct
{
	Node	   *var;			/* 可能是表达式，不只是 Var */
	RelOptInfo *rel;			/* 所属关系 */
	double		ndistinct;		/* 不同值数目 */
	bool		isdefault;		/* 用 DEFAULT_NUM_DISTINCT 时为真 */
} GroupVarInfo;

static List *
add_unique_group_var(PlannerInfo *root, List *varinfos,
					 Node *var, VariableStatData *vardata)
{
	GroupVarInfo *varinfo;
	double		ndistinct;
	bool		isdefault;
	ListCell   *lc;

	ndistinct = get_variable_numdistinct(vardata, &isdefault);

	/*
	 * var 内的 nullingrels 位可能导致同一个 var 被计
	 * 数多次，若它标记了不同的 nullingrels。它们还可能
	 * 妨碍我们把 var 匹配到扩展统计信息中的表达式
	 * （见 estimate_multivariate_ndistinct）。所以先把它们
	 * 剥掉。
	 */
	var = remove_nulling_relids(var, root->outer_join_rels, NULL);

	foreach(lc, varinfos)
	{
		varinfo = (GroupVarInfo *) lfirst(lc);

		/* 去除完全重复项 */
		if (equal(var, varinfo->var))
			return varinfos;

		/*
		 * 去掉已知相等的 var，但仅限属于不同关系者
		 * （见 estimate_num_groups 的注释）
		 */
		if (vardata->rel != varinfo->rel &&
			exprs_known_equal(root, var, varinfo->var))
		{
			if (varinfo->ndistinct <= ndistinct)
			{
				/* 保留较旧项，忘掉新项 */
				return varinfos;
			}
			else
			{
				/* 删除较旧项 */
				varinfos = foreach_delete_current(varinfos, lc);
			}
		}
	}

	varinfo = (GroupVarInfo *) palloc(sizeof(GroupVarInfo));

	varinfo->var = var;
	varinfo->rel = vardata->rel;
	varinfo->ndistinct = ndistinct;
	varinfo->isdefault = isdefault;
	varinfos = lappend(varinfos, varinfo);
	return varinfos;
}

/*
 * estimate_num_groups		- 估计分组查询中的组数
 *
 * 对带有 GROUP BY 子句的查询，估计将会有多少个
 * 组 --- 即 GROUP BY 表达式的不同组合的数目。
 *
 *
 * 本例程也用于估计 DISTINCT 过滤步骤会发出的行数；
 * 这是一个同构问题。（注意：实际上我们只在
 * DISTINCT 之前没有分组或聚合时才把它用于
 * DISTINCT。）
 *
 * 输入:
 *	root - 查询
 *	groupExprs - 被分组依据的表达式列表
 *	input_rows - 估计将到达分组/去重过滤步骤的行数
 *
 *	pgset - NULL，或指向某个分组集的 List**，用于对
 *		groupExprs 过滤
 *
 * 输出:
 *	estinfo - 以非 NULL 传入时，函数会设置 "flags"
 *		字段中的位，以便向调用者提供估算的额外信息。
 *		当前，我们只在估算中使用了任何默认值时设置
 *		SELFLAG_USED_DEFAULT 位。
 *
 * 鉴于系统中缺乏任何交叉相关性统计信息，对于涉及
 * 多个 Var 的 GROUP BY 条件，不可能做出真正可信的
 * 估算。但应避免假定最坏情形（所有可能的叉积
 * 项实际上都作为组出现），因为被分组 Var 往往高度
 * 相关。我们当前的做法如下：
 *
 *	1.  产生布尔的表达式被假定贡献两个组，独立于
 *		其内容，且在后续步骤中被忽略。这主要是因为
 *		"col IS NULL" 这类测试尤其严重地破坏步骤 2 中
 *		所用的启发式。
 *	2.  把给定表达式归约为所用唯一 Var 的列表。例如
 *		GROUP BY a, a + b 被当作与 GROUP BY a, b 一样。
 *		显然同一 Var 不应计入多于一次。把 f(x) 视同
 *		x：f() 无法增加不同值数（除非它易变，
 *		我们认为分组时不太可能如此），但它多半也
 *		不会大幅减少不同值的数目。
 *
 *		特殊情形：若某个 GROUP BY 表达式能匹配到
 *		我们有统计信息的表达式索引，则把整个表达式
 *		当作一个 Var 对待。
 *	3.  若列表包含已知相等的不同关系的 Var（由于等价
 *		类），则从每个已知相等集合中去掉其余的 Var，
 *		保留估计值数目最小的那个（因为其他 Var 的多余
 *		值不可能出现在连接行中）。
 *		注意只考虑不同关系的 Var 的原因：若考虑
 *		同一关系的 Var，就会在下一步重复计算该等式的
 *		限制选择率。
 *	4.  对同一源关系内的 Var，我们把值的数目相乘，
 *		并钳制到该关系行数（多于一个 Var 时除以 10），
 *		再乘以基于该关系限制子句选择率的系数。
 *		当有多个 Var 时，初始乘积多半过高（它是最坏情
 *		形），但把乘积钳制到关系行数的一部分似乎是
 *		避免估算失控的有用启发式。（系数 10 来自
 *		Postgres 7.4 之前的实践。）用于调整限制选择率的
 *		乘法系数假定限制子句与分组无关，这一假定可能
 *		不成立，但很难做得更好。
 *
 *
 *	5.  若有来自多个关系的 Var，则对每个这样的关系重
 *		复步骤 4，并把结果相乘。
 * 注意不含被分组 Var 的关系被完全忽略，连接子句也
 * 一样。这样的关系无法增加组的数目，我们假定
 * 这样的子句也不会减少组的数目（略显武断，
 * 但我们没有更好的信息）。
 */
double
estimate_num_groups(PlannerInfo *root, List *groupExprs, double input_rows,
					List **pgset, EstimationInfo *estinfo)
{
	List	   *varinfos = NIL;
	double		srf_multiplier = 1.0;
	double		numdistinct;
	ListCell   *l;
	int			i;

	/* 若非 NULL 则把 estinfo 输出参数清零 */
	if (estinfo != NULL)
		memset(estinfo, 0, sizeof(EstimationInfo));

	/*
	 * 我们绝不想返回零组的估算值，因为那往往会导致
	 * 除零和其他不愉快的结果。input_rows 估算通常
	 * 已至少为 1，但为防万一还是钳制一下。
	 *
	 */
	input_rows = clamp_row_est(input_rows);

	/*
	 * 若没有分组列，则恰好只有一个组。（这在带
	 * GROUP BY 或 DISTINCT 的正常情形下不会发生，
	 * 但集合操作下有可能。）
	 */
	if (groupExprs == NIL || (pgset && *pgset == NIL))
		return 1.0;

	/*
	 * 统计由布尔分组表达式产生的组。对其他表达式，
	 * 找出所用唯一 Var，若找得到统计信息就把表达式
	 * 当作 Var。对每个，记录不同值数目的统计估计
	 * （其表中的总数，不考虑过滤）。
	 *
	 */
	numdistinct = 1.0;

	i = 0;
	foreach(l, groupExprs)
	{
		Node	   *groupexpr = (Node *) lfirst(l);
		double		this_srf_multiplier;
		VariableStatData vardata;
		List	   *varshere;
		ListCell   *l2;

		/* 表达式在此分组集中？ */
		if (pgset && !list_member_int(*pgset, i++))
			continue;

		/*
		 * 分组列中的集合返回函数有点棘手。下面的代码会
		 * 实际上无视它们的 SRF 性质，得出如同它们是标量
		 * 函数一样的 numdistinct 估算。我们通过按最大 SRF
		 * 行数估算放大最终结果来补偿。（若 SRF 把
		 * 某个输出值产生多份拷贝，会高估，但最好
		 * 还是假定 SRF 的输出互不相同。无论如何，在
		 * 今天没有好得多的 SRF 输出行数估算的情况
		 * 下为此过分担心多半是没意义的。）
		 *
		 */
		this_srf_multiplier = expression_returns_set_rows(root, groupexpr);
		if (srf_multiplier < this_srf_multiplier)
			srf_multiplier = this_srf_multiplier;

		/* 对返回布尔的表达式短路 */
		if (exprType(groupexpr) == BOOLOID)
		{
			numdistinct *= 2.0;
			continue;
		}

		/*
		 * 若 examine_variable 能对 GROUP BY 表达式推断出任何
		 * 东西，就把它当作单个变量，哪怕它实际更复杂。
		 *
		 *
		 * XXX 这带来的后果是：若表达式上存在统计对象，
		 * 我们不会把它拆成单个 Var。这会影响我们在
		 * estimate_multivariate_ndistinct
		 * 里对每个表达式使用更准确的估算并把它们当作
		 * 独立的，比把抽取出的变量的估算组合起来更好
		 * ——当我们不知道那与表达式的关系如何时。
		 *
		 */
		examine_variable(root, groupexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple) || vardata.isunique)
		{
			varinfos = add_unique_group_var(root, varinfos,
											groupexpr, &vardata);
			ReleaseVariableStats(vardata);
			continue;
		}
		ReleaseVariableStats(vardata);

		/*
		 * 否则抽出组件 Var。对 PlaceHolderVar 的处理是
		 * 递归其参数（等于假定 PlaceHolderVar 不改变
		 * 组的数目，这归根结底就是无视可能向结果集
		 * 添加空值的情形）。
		 */
		varshere = pull_var_clause(groupexpr,
								   PVC_RECURSE_AGGREGATES |
								   PVC_RECURSE_WINDOWFUNCS |
								   PVC_RECURSE_PLACEHOLDERS);

		/*
		 * 若找到任何不含变量的 GROUP BY 项，则它要么
		 * 是常量（可忽略），要么含易变函数；在后一种
		 * 情形下放弃，假定每个输入行都会产生
		 * 一个不同的组。
		 */
		if (varshere == NIL)
		{
			if (contain_volatile_functions(groupexpr))
				return input_rows;
			continue;
		}

		/*
		 * 否则变量加入 varinfos 列表
		 */
		foreach(l2, varshere)
		{
			Node	   *var = (Node *) lfirst(l2);

			examine_variable(root, var, 0, &vardata);
			varinfos = add_unique_group_var(root, varinfos, var, &vardata);
			ReleaseVariableStats(vardata);
		}
	}

	/*
	 * 若无任何 Var，GROUP BY 列表必为全常量或全布尔。
	 *
	 */
	if (varinfos == NIL)
	{
		/* 像长路径中那样应用 SRF 乘数 */
		numdistinct *= srf_multiplier;
		/* 取整 */
		numdistinct = ceil(numdistinct);
		/* 防止答案超出范围 */
		if (numdistinct > input_rows)
			numdistinct = input_rows;
		if (numdistinct < 1.0)
			numdistinct = 1.0;
		return numdistinct;
	}

	/*
	 * 把 Var 按关系分组并估计总 numdistinct。
	 *
	 * 外层循环的每次迭代处理 varinfos 中最前面的 Var，
	 * 加上同一关系的其他 Var。把它们从下一轮迭代
	 * 用的新 varinfos 列表中移除。这是把同一关系的 Var
	 * 聚在一起的最简单方式。
	 */
	do
	{
		GroupVarInfo *varinfo1 = (GroupVarInfo *) linitial(varinfos);
		RelOptInfo *rel = varinfo1->rel;
		double		reldistinct = 1;
		double		relmaxndistinct = reldistinct;
		int			relvarcount = 0;
		List	   *newvarinfos = NIL;
		List	   *relvarinfos = NIL;

		/*
		 * 把 varinfos 列表一分为二：一个属于当前关系，
		 * 一个装其余 Var。
		 */
		relvarinfos = lappend(relvarinfos, varinfo1);
		for_each_from(l, varinfos, 1)
		{
			GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

			if (varinfo2->rel == varinfo1->rel)
			{
				/* 当前关系 varinfos */
				relvarinfos = lappend(relvarinfos, varinfo2);
			}
			else
			{
				/* 还没轮到处理 varinfo2 */
				newvarinfos = lappend(newvarinfos, varinfo2);
			}
		}

		/*
		 * 取该关系 Var 的 numdistinct 估算。我们
		 * 以最大 var 数迭代搜索多变量 n-distinct；假定各
		 * var 组相互独立，将其相乘。再找不到多变量匹
		 * 配后剩余的 relvarinfos 也被假定独立，因此
		 * 它们各自的 ndistinct 估算也相乘。
		 *
		 *
		 * 迭代时，数一数我们应用了多少个独立的
		 * numdistinct 值。下文会乘一个修正系数，但仅当
		 * 乘了不止一个时。
		 */
		while (relvarinfos)
		{
			double		mvndistinct;

			if (estimate_multivariate_ndistinct(root, rel, &relvarinfos,
												&mvndistinct))
			{
				reldistinct *= mvndistinct;
				if (relmaxndistinct < mvndistinct)
					relmaxndistinct = mvndistinct;
				relvarcount++;
			}
			else
			{
				foreach(l, relvarinfos)
				{
					GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

					reldistinct *= varinfo2->ndistinct;
					if (relmaxndistinct < varinfo2->ndistinct)
						relmaxndistinct = varinfo2->ndistinct;
					relvarcount++;

					/*
					 * 当 varinfo2 的 isdefault 被置位时，最好把
					 * 在 EstimationInfo 设 SELFLAG_USED_DEFAULT 位。
					 */
					if (estinfo != NULL && varinfo2->isdefault)
						estinfo->flags |= SELFLAG_USED_DEFAULT;
				}

				/* 该关系已处理完毕 */
				relvarinfos = NIL;
			}
		}

		/*
		 * 健全性检查 --- 关系为空时不要除零。
		 */
		Assert(IS_SIMPLE_REL(rel));
		if (rel->tuples > 0)
		{
			/*
			 * 钳到关系大小，多 Var 时为关系大小 / 10。这
			 * 个修正系数是因为 Var 们多半相关但程度不明。
			 * 但我们绝不该钳到低于任一 Var 的最大 ndistinct
			 * 值，因为肯定至少会有那么多组。
			 *
			 */
			double		clamp = rel->tuples;

			if (relvarcount > 1)
			{
				clamp *= 0.1;
				if (clamp < relmaxndistinct)
				{
					clamp = relmaxndistinct;
					/* 以防某些 ndistinct 过大，保持健全： */
					if (clamp > rel->tuples)
						clamp = rel->tuples;
				}
			}
			if (reldistinct > clamp)
				reldistinct = clamp;

			/*
			 * 按限制选择率更新估算，reldistinct 为零时防
			 * 除零。若已知返回全部行也跳过此步。
			 *
			 */
			if (reldistinct > 0 && rel->rows < rel->tuples)
			{
				/*
				 * 给定含 N 行、n 个不同值且均匀分布的表，
				 * 若随机选 p 行，则选中的不同值数
				 * 目的期望为：
				 *
				 * n * (1 - product((N-N/n-i)/(N-i), i=0..p-1))
				 *
				 * = n * (1 - (N-N/n)! / (N-N/n-p)! * (N-p)! / N!)
				 *
				 * 见 S. B. Yao "Approximating block accesses in
				 * database organizations"，《ACM 通信》：
				 * 第20卷4期，1977年4月 pp.260-261.
				 *
				 * 或者，把阶乘的项重新排列，
				 * 该式可写为：
				 *
				 * n * (1 - product((N-p-i)/(N-i), i=0..N/n-1))
				 *
				 * 该公式形式在 p 大于 N/n 的常见情形下
				 * 计算更高效。另外，正如 Dell'Era 指出的，若
				 * 乘积各项都满足 i << N，则可用下式近似：
				 *
				 *
				 * n * (1 - ((N-p)/N)^(N/n))
				 *
				 * 见 "Expected distinct values when selecting from a bag
				 * without replacement"，Dell'Era，
				 * http://www.adellera.it/investigations/distinct_balls/.
				 *
				 * 条件 i << N 等价于 n >> 1，故表中不同值很多
				 * 时是好近似。事实证明该公式甚至在
				 * n 很小时也工作良好。
				 *
				 */
				reldistinct *=
					(1 - pow((rel->tuples - rel->rows) / rel->tuples,
							 rel->tuples / reldistinct));
			}
			reldistinct = clamp_row_est(reldistinct);

			/*
			 * 更新总不同组数的估算。
			 */
			numdistinct *= reldistinct;
		}

		varinfos = newvarinfos;
	} while (varinfos != NIL);

	/* 现在可以计入任何 SRF 的影响了 */
	numdistinct *= srf_multiplier;

	/* 取整 */
	numdistinct = ceil(numdistinct);

	/* 防止答案超出范围 */
	if (numdistinct > input_rows)
		numdistinct = input_rows;
	if (numdistinct < 1.0)
		numdistinct = 1.0;

	return numdistinct;
}

/*
 * 当指定表达式被用作给定数目桶的哈希键时，估计
 * 桶统计信息。
 *
 * 它试图确定两个值：
 *
 * 1. 表达式最常用值的频率（若得不到则向 *mcv_freq
 * 返回零）。
 *
 * 2. "桶大小分数"，即桶中条目平均数除以关系中的
 * 总元组数。
 *
 * XXX 这实在相当粗糙，因为我们实际上是在假定施加
 * 限制子句之后哈希键的分布与底层关系中的相同。
 * 然而，我们的智慧远不足以弄清限制子句会如何改变
 * 分布，所以目前只能如此。
 *
 *
 * 我们得到的是执行器将对给定输入关系使用的桶数。
 * 若数据完美分布，相同数目的元组进入每个可用桶，
 * 那么桶大小分数就是 1/nbuckets。但只有在以下情况
 * 下这种理想状态才会出现：(a) 至少有 nbuckets 个
 * 不同数据值，且 (b) 分布偏斜不严重。否则各桶会
 * 被不均匀占据。若连接中另一关系的键分布与本关系
 * 相似，那么负载最重的桶正是最常被探测的桶。
 *
 * 因此，为代价估算之用，"平均"桶大小确实应取接近于
 * "最坏情形"桶大小的值。我们试着这样估算：若有太
 * 少的不同值就调整分数，再按最常用值频率与平均
 * 频率之比放大。
 *
 *
 * 若没有统计信息可用，就用默认估算 0.1。若内关系
 * 很大，这会相当强烈地阻止使用哈希，这正是我们想要
 * 的。除非知道内关系分散良好（或备选方案看起来
 * 糟糕得多），否则我们不想用哈希。
 *
 * 调用者还应检查 mcv_freq 是否大到最常用值自身就
 * 需要一个不切实际的大桶。哈希连接中执行器可以在
 * 桶过大时分裂桶，但显然这对包含同一值大量重复的
 * 桶无济于事。
 *
 */
void
estimate_hash_bucket_stats(PlannerInfo *root, Node *hashkey, double nbuckets,
						   Selectivity *mcv_freq,
						   Selectivity *bucketsize_frac)
{
	VariableStatData vardata;
	double		estfract,
				ndistinct,
				stanullfrac,
				avgfreq;
	bool		isdefault;
	AttStatsSlot sslot;

	examine_variable(root, hashkey, 0, &vardata);

	/* 若可用，查找最常用值的频率 */
	*mcv_freq = 0.0;

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			/*
			 * 第一个 MCV 统计即最常用值。
			 */
			if (sslot.nnumbers > 0)
				*mcv_freq = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}

	/* 取得不同值的数目 */
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);

	/*
	 * 若 ndistinct 非真实，放弃。通常返回 0.1，但若
	 * 已知 mcv_freq 比它还高，就用它代替。
	 */
	if (isdefault)
	{
		*bucketsize_frac = (Selectivity) Max(0.1, *mcv_freq);
		ReleaseVariableStats(vardata);
		return;
	}

	/* 取得 NULL 所占比例 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		stanullfrac = stats->stanullfrac;
	}
	else
		stanullfrac = 0.0;

	/* 计算原始关系中所有不同数据值的平均频率 */
	avgfreq = (1.0 - stanullfrac) / ndistinct;

	/*
	 * 调整 ndistinct 计入限制子句的影响。注意我们假定
	 * 数据分布受限制子句均匀影响！
	 *
	 *
	 * XXX 或许更好，但昂贵得多：乘以提及目标 Var
	 * 的关系限制子句的选择率。
	 */
	if (vardata.rel && vardata.rel->tuples > 0)
	{
		ndistinct *= vardata.rel->rows / vardata.rel->tuples;
		ndistinct = clamp_row_est(ndistinct);
	}

	/*
	 * 只要桶数小于预期不同值数目，桶大小分数初始
	 * 估算就是 1/nbuckets；否则为 1/ndistinct。
	 *
	 */
	if (ndistinct > nbuckets)
		estfract = 1.0 / nbuckets;
	else
		estfract = 1.0 / ndistinct;

	/*
	 * 向上调整估算的桶大小以计入偏斜分布。
	 */
	if (avgfreq > 0.0 && *mcv_freq > avgfreq)
		estfract *= *mcv_freq / avgfreq;

	/*
	 * 把桶大小钳到合理范围（上面的调整很容易产生
	 * 超范围的结果）。我们把下界设得比零稍高一点，
	 * 因为零不是很合理的结果。
	 */
	if (estfract < 1.0e-6)
		estfract = 1.0e-6;
	else if (estfract > 1.0)
		estfract = 1.0;

	*bucketsize_frac = (Selectivity) estfract;

	ReleaseVariableStats(vardata);
}

/*
 * estimate_hashagg_tablesize
 *	  估计哈希聚合哈希表将需要的字节数，依据是
 *	  agg_costs、路径宽度与组数。
 *
 * 我们以 "double" 返回结果，以预防乘以 dNumGroups
 * 时可能出现的溢出问题。
 *
 * XXX 既然 hashagg 已知道省去哈希表中不需要的列，
 * 这可能高估了大小。另外对混合模式分组集，不在哈希
 * 集合中的分组列也在这里被计入，尽管 hashagg 不会存
 * 储它们。这是个问题吗？
 */
double
estimate_hashagg_tablesize(PlannerInfo *root, Path *path,
						   const AggClauseCosts *agg_costs, double dNumGroups)
{
	Size		hashentrysize;

	hashentrysize = hash_agg_entry_size(list_length(root->aggtransinfos),
										path->pathtarget->width,
										agg_costs->transitionSpace);

	/*
	 * 注意这忽略了哈希表填充因子与增长策略的影响。
	 * 鉴于默认填充因子较高，问题不大。要有意义
	 * 把"两倍扩容"增长策略计入在内是困难的。
	 *
	 */
	return hashentrysize * dNumGroups;
}


/*-------------------------------------------------------------------------
 *
 * Support routines
 *
 *-------------------------------------------------------------------------
 */

/*
 * 为给定 VarInfo 列表（其必须都属于给定的 rel）找到
 * 适用的 ndistinct 统计信息，并把 *ndistinct 更新为
 * 最佳匹配的 MVNDistinctItem 的估算。若找到匹配，
 * *varinfos 更新为移除匹配的 varinfo。
 *
 * 非简单 Var 的 varinfo 会被忽略。
 *
 * 若能找到匹配则返回 true，否则返回 false。
 */
static bool
estimate_multivariate_ndistinct(PlannerInfo *root, RelOptInfo *rel,
								List **varinfos, double *ndistinct)
{
	ListCell   *lc;
	int			nmatches_vars;
	int			nmatches_exprs;
	Oid			statOid = InvalidOid;
	MVNDistinct *stats;
	StatisticExtInfo *matched_info = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* 若表没有扩展统计信息则立即退出 */
	if (!rel->statlist)
		return false;

	/* 查找匹配最多 var 的 ndistinct 统计信息对象 */
	nmatches_vars = 0;			/* 要求至少有两个匹配 */
	nmatches_exprs = 0;
	foreach(lc, rel->statlist)
	{
		ListCell   *lc2;
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		int			nshared_vars = 0;
		int			nshared_exprs = 0;

		/* 跳过其他种类统计信息 */
		if (info->kind != STATS_EXT_NDISTINCT)
			continue;

		/* 跳过 stxdinherit 值不匹配的统计信息 */
		if (info->inherit != rte->inh)
			continue;

		/*
		 * 确定有多少表达式（以及未匹配表达式中的
		 * 变量）匹配。然后我们会用这些数字来挑选
		 * 最匹配子句的统计信息对象。
		 */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);
			AttrNumber	attnum;

			Assert(varinfo->rel == rel);

			/* 简单 Var，直接在统计键中搜索 */
			if (IsA(varinfo->var, Var))
			{
				attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 忽略系统属性：不支持对其的统计信息，
				 * 因此无法匹配（而且值为负还会失败。）
				 *
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				if (bms_is_member(attnum, info->keys))
					nshared_vars++;

				continue;
			}

			/* 表达式 —— 看是否在统计对象中 */
			foreach(lc3, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					nshared_exprs++;
					break;
				}
			}
		}

		if (nshared_vars + nshared_exprs < 2)
			continue;

		/*
		 * 这个统计信息对象匹配的列是否比当前最佳对象
		 * 更多？若是，改用它。
		 *
		 * XXX 这里应当用对象名或类似物打破平局，
		 * 以使结果稳定。
		 */
		if ((nshared_exprs > nmatches_exprs) ||
			(((nshared_exprs == nmatches_exprs)) && (nshared_vars > nmatches_vars)))
		{
			statOid = info->statOid;
			nmatches_vars = nshared_vars;
			nmatches_exprs = nshared_exprs;
			matched_info = info;
		}
	}

	/* 无匹配 */
	if (statOid == InvalidOid)
		return false;

	Assert(nmatches_vars + nmatches_exprs > 1);

	stats = statext_ndistinct_load(statOid, rte->inh);

	/*
	 * 若我们有匹配，则在其中搜索匹配的特定条目
	 * （必然存在），并构造输出值。
	 */
	if (stats)
	{
		int			i;
		List	   *newlist = NIL;
		MVNDistinctItem *item = NULL;
		ListCell   *lc2;
		Bitmapset  *matched = NULL;
		AttrNumber	attnum_offset;

		/*
		 * attnum 需要偏移多少？若没有表达式，
		 * 则无需偏移。否则偏移足够把最小的那个（等于
		 * 表达式的数目）移到 1。
		 */
		if (matched_info->exprs)
			attnum_offset = (list_length(matched_info->exprs) + 1);
		else
			attnum_offset = 0;

		/* 看实际匹配了什么 */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			int			idx;
			bool		found = false;

			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);

			/*
			 * 直接通过与键匹配处理简单 Var 表达式。
			 * 若存在匹配的表达式，我们稍后会尝试匹配
			 * 它。
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 忽略系统属性上的表达式。不能依赖 bms
				 * 检查来处理负值。
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				/* 该变量是否被统计信息对象覆盖？ */
				if (!bms_is_member(attnum, matched_info->keys))
					continue;

				attnum = attnum + attnum_offset;

				/* 确保足够的偏移量 */
				Assert(AttrNumberIsForUserDefinedAttr(attnum));

				matched = bms_add_member(matched, attnum);

				found = true;
			}

			/*
			 * XXX 即使我们已找到与表达式匹配的属性，也许
			 * 也应允许搜索表达式？这能处理 "(a)" 这类
			 * 平凡表达式，但似乎没什么用处。
			 */
			if (found)
				continue;

			/* 表达式 —— 看是否在统计对象中 */
			idx = 0;
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					AttrNumber	attnum = -(idx + 1);

					attnum = attnum + attnum_offset;

					/* 确保足够的偏移量 */
					Assert(AttrNumberIsForUserDefinedAttr(attnum));

					matched = bms_add_member(matched, attnum);

					/* 应当恰好只有一个匹配的表达式 */
					break;
				}

				idx++;
			}
		}

		/* 找到与该组合恰好匹配的特定条目 */
		for (i = 0; i < stats->nitems; i++)
		{
			int			j;
			MVNDistinctItem *tmpitem = &stats->items[i];

			if (tmpitem->nattributes != bms_num_members(matched))
				continue;

			/* 假定是正确条目 */
			item = tmpitem;

			/* 检查条目所有属性/表达式是否符合匹配 */
			for (j = 0; j < tmpitem->nattributes; j++)
			{
				AttrNumber	attnum = tmpitem->attributes[j];

				/*
				 * 由于上面构建匹配位图的方式，我们可以
				 * 用同样的方式偏移所有 attnum。
				 */
				attnum = attnum + attnum_offset;

				if (!bms_is_member(attnum, matched))
				{
					/* 不是这个条目 */
					item = NULL;
					break;
				}
			}

			/*
			 * 若该条目拥有全部已匹配的属性，我们就知道
			 * 它就是正确的 —— 不可能更好。
			 */
			if (item)
				break;
		}

		/*
		 * 确保我们找到了一个条目。必然存在一个，
		 * 因为 ndistinct 统计信息含属性的所有组合。
		 */
		if (!item)
			elog(ERROR, "corrupt MVNDistinct entry");

		/* 构造输出 varinfo 列表，只保留未匹配的 */
		foreach(lc, *varinfos)
		{
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc);
			ListCell   *lc3;
			bool		found = false;

			/*
			 * 先看普通变量，因为这是最常见情形，
			 * 而且检查相当廉价。我们可以直接取得
			 * attnum 并检查（带偏移的）匹配位图。
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * 若它是系统属性，就完成了。我们不支持
				 * 系统属性上的扩展统计信息，因此它显然
				 * 未匹配。只需保留该表达式并继续。
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
				{
					newlist = lappend(newlist, varinfo);
					continue;
				}

				/* 应用与上文相同的偏移 */
				attnum += attnum_offset;

				/* 若未匹配，保留该 varinfo */
				if (!bms_is_member(attnum, matched))
					newlist = lappend(newlist, varinfo);

				/* 循环的其余部分处理复杂表达式。 */
				continue;
			}

			/*
			 * 处理复杂表达式，而不只是简单 Var。
			 *
			 * 首先我们搜索表达式的精确匹配。若找到，
			 * 我们可以直接丢弃整个 GroupVarInfo，连同
			 * 从中提取的所有变量。
			 *
			 * 否则我们检查各个 var，并尝试把它匹配到
			 * 条目中的变量。
			 */
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					found = true;
					break;
				}
			}

			/* 精确匹配即跳过 */
			if (found)
				continue;

			newlist = lappend(newlist, varinfo);
		}

		*varinfos = newlist;
		*ndistinct = item->ndistinct;
		return true;
	}

	return false;
}

/*
 * convert_to_scalar
 *	  把所指类型的非 NULL 值转换为 scalarineqsel()
 *	  所需的比较尺度。
 *	  若成功返回 "true"。
 *
 * XXX 本例程是个 hack：理想上应在 pg_type 中
 * 查找转换例程。
 *
 * 所有数值数据类型都直接转换为等价的 "double"
 * 值。（超出 "double" 范围的 NUMERIC 值会被钳制到
 * +/- HUGE_VAL。）
 *
 * 字符串数据类型由 convert_string_to_scalar() 转换，
 * 其说明见下文。本例程一次处理三个值而非一个
 * 的原因是字符串需要这样做。
 *
 * bytea 数据类型与字符串的差异刚好大到必须
 * 单独对待。
 *
 * 表示绝对时间的几种数据类型都被转换为
 * Timestamp（实际是 int64），然后我们把它提升为
 * double。注意即使对 Timestamp 的"特殊"值这也
 * 会给出正确结果，因为那些值选得能正确比较；
 * 见 timestamp_cmp。
 *
 * 表示相对时间（区间）的几种数据类型都被转换
 * 为以秒表示的度量。
 */
static bool
convert_to_scalar(Datum value, Oid valuetypid, Oid collid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound)
{
	bool		failure = false;

	/*
	 * valuetypid 与 boundstypid 都应当与我们所服务的
	 * 算子的声明输入类型完全匹配。不过，扩展可能
	 * 会尝试把 scalarineqsel 用作此处未处理输入类型
	 * 的算子的估算器；这种情形下我们想返回 false
	 * 而非失败。无论如何，我们绝不能假定 valuetypid
	 * 与 boundstypid 相同。
	 *
	 * XXX 我们在其点间插值的直方图可能属于一个与声明
	 * 类型只是二进制兼容的列。本质上我们是假定
	 * 二进制兼容类型的语义足够相似，使得我们可以
	 * 用一种类型的算子生成的直方图来估计另一种
	 * 类型的选择率。这在某些情形完全错误 —— 特别
	 * 是有符号与无符号解释可能让我们栽跟头。但
	 * 它在多数情形足够有用，所以我们还是这样做。
	 * 应当考虑更严格的
	 * 做法。
	 */
	switch (valuetypid)
	{
			/*
			 * 内建数值类型
			 */
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			*scaledvalue = convert_numeric_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_numeric_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_numeric_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;

			/*
			 * 内建字符串类型
			 */
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		case NAMEOID:
			{
				char	   *valstr = convert_string_datum(value, valuetypid,
														  collid, &failure);
				char	   *lostr = convert_string_datum(lobound, boundstypid,
														 collid, &failure);
				char	   *histr = convert_string_datum(hibound, boundstypid,
														 collid, &failure);

				/*
				 * 若任一值非字符串类型则退出。我们可能
				 * 泄漏另一值的已转换字符串，但这不值得
				 * 为此烦恼。
				 */
				if (failure)
					return false;

				convert_string_to_scalar(valstr, scaledvalue,
										 lostr, scaledlobound,
										 histr, scaledhibound);
				pfree(valstr);
				pfree(lostr);
				pfree(histr);
				return true;
			}

			/*
			 * 内建 bytea 类型
			 */
		case BYTEAOID:
			{
				/* 我们只支持 bytea 对 bytea 的比较 */
				if (boundstypid != BYTEAOID)
					return false;
				convert_bytea_to_scalar(value, scaledvalue,
										lobound, scaledlobound,
										hibound, scaledhibound);
				return true;
			}

			/*
			 * 内建时间类型
			 */
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case DATEOID:
		case INTERVALOID:
		case TIMEOID:
		case TIMETZOID:
			*scaledvalue = convert_timevalue_to_scalar(value, valuetypid,
													   &failure);
			*scaledlobound = convert_timevalue_to_scalar(lobound, boundstypid,
														 &failure);
			*scaledhibound = convert_timevalue_to_scalar(hibound, boundstypid,
														 &failure);
			return !failure;

			/*
			 * 内建网络类型
			 */
		case INETOID:
		case CIDROID:
		case MACADDROID:
		case MACADDR8OID:
			*scaledvalue = convert_network_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_network_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_network_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;
	}
	/* 不知道如何转换 */
	*scaledvalue = *scaledlobound = *scaledhibound = 0;
	return false;
}

/*
 * 替 convert_to_scalar() 处理各数值数据类型。
 *
 * 失败时把 *failure 置 true（如 typid 不支持）；
 * 否则该变量不变。
 */
static double
convert_numeric_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case BOOLOID:
			return (double) DatumGetBool(value);
		case INT2OID:
			return (double) DatumGetInt16(value);
		case INT4OID:
			return (double) DatumGetInt32(value);
		case INT8OID:
			return (double) DatumGetInt64(value);
		case FLOAT4OID:
			return (double) DatumGetFloat4(value);
		case FLOAT8OID:
			return (double) DatumGetFloat8(value);
		case NUMERICOID:
			/* 注意：超范围值将被钳制到 +-HUGE_VAL */
			return (double)
				DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,
												   value));
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			/* 可把 OID 当作整数… */
			return (double) DatumGetObjectId(value);
	}

	*failure = true;
	return 0;
}

/*
 * 为任何字符串类型做 convert_to_scalar() 的工作。
 *
 * 字符串数据类型被转换到 0 到 1 的尺度，
 * 其中我们把字符串的字节看作小数位。
 *
 * 但我们不想要基数 256，因为那容易产生
 * 偏高的选择率估计；很少有数据库会在每个
 * 位置出现全部 256 种可能的字节值。
 * 相反，用边界中见到的最小与最大字节值作为
 * 每个字节的估计范围，再经一些调整来应对
 * 我们很可能无法以这种方式见到全范围的事实。
 *
 * 进一步的改进是，在计算缩放值之前先去掉三个
 * 字符串的任何公共前缀。这使我们在遇到狭窄
 * 数据范围时能够"放大"。一个例子是电话号码
 * 数据库，其中所有值都以相同的区号开头。
 * （实际上，边界会是相邻的直方图桶边界值，
 * 所以这比你想的更容易发生。）
 */
static void
convert_string_to_scalar(char *value,
						 double *scaledvalue,
						 char *lobound,
						 double *scaledlobound,
						 char *hibound,
						 double *scaledhibound)
{
	int			rangelo,
				rangehi;
	char	   *sptr;

	rangelo = rangehi = (unsigned char) hibound[0];
	for (sptr = lobound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	for (sptr = hibound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	/* 若范围含任何大写 ASCII 字符，就扩大到含全部 */
	if (rangelo <= 'Z' && rangehi >= 'A')
	{
		if (rangelo > 'A')
			rangelo = 'A';
		if (rangehi < 'Z')
			rangehi = 'Z';
	}
	/* 小写同理 */
	if (rangelo <= 'z' && rangehi >= 'a')
	{
		if (rangelo > 'a')
			rangelo = 'a';
		if (rangehi < 'z')
			rangehi = 'z';
	}
	/* 数字同理 */
	if (rangelo <= '9' && rangehi >= '0')
	{
		if (rangelo > '0')
			rangelo = '0';
		if (rangehi < '9')
			rangehi = '9';
	}

	/*
	 * 若范围含少于 10 个字符，则假定数据不足，
	 * 让它含常规 ASCII 集。
	 */
	if (rangehi - rangelo < 9)
	{
		rangelo = ' ';
		rangehi = 127;
	}

	/*
	 * 去掉三个字符串的任何公共前缀。
	 */
	while (*lobound)
	{
		if (*lobound != *hibound || *lobound != *value)
			break;
		lobound++, hibound++, value++;
	}

	/*
	 * 现在可以做转换了。
	 */
	*scaledvalue = convert_one_string_to_scalar(value, rangelo, rangehi);
	*scaledlobound = convert_one_string_to_scalar(lobound, rangelo, rangehi);
	*scaledhibound = convert_one_string_to_scalar(hibound, rangelo, rangehi);
}

static double
convert_one_string_to_scalar(char *value, int rangelo, int rangehi)
{
	int			slen = strlen(value);
	double		num,
				denom,
				base;

	if (slen <= 0)
		return 0.0;				/* 空字符串的标量值为 0 */

	/*
	 * 考虑字符串中超过十几个字节似乎没什么意义。
	 * 由于基数至少为 10，这会给我们至少 12 位十进制
	 * 的名义分辨率，这肯定远超这种估算技术本来
	 * 具有的精度（尤其在非 C 区域设置中）。另外，
	 * 即使基数取最大可能的 256，这也保证 denom
	 * 不会增长到超过 256^13 = 2.03e31，这在任何
	 * 已知机器不溢出。
	 */
	if (slen > 12)
		slen = 12;

	/* 把开头的字符转换为分数 */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (slen-- > 0)
	{
		int			ch = (unsigned char) *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * 把字符串类型 Datum 转为 palloc 的、NULL 结尾串。
 *
 * 失败时把 *failure 置 true（如 typid 不支持）；
 * 否则该变量不变。（失败时我们会返回 NULL。）
 *
 * 使用非 C 区域设置时，我们必须先把字符串通过
 * strxfrm() 处理，以产生正确的区域设置相关结果。
 */
static char *
convert_string_datum(Datum value, Oid typid, Oid collid, bool *failure)
{
	char	   *val;

	switch (typid)
	{
		case CHAROID:
			val = (char *) palloc(2);
			val[0] = DatumGetChar(value);
			val[1] = '\0';
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			val = TextDatumGetCString(value);
			break;
		case NAMEOID:
			{
				NameData   *nm = (NameData *) DatumGetPointer(value);

				val = pstrdup(NameStr(*nm));
				break;
			}
		default:
			*failure = true;
			return NULL;
	}

	if (!lc_collate_is_c(collid))
	{
		char	   *xfrmstr;
		size_t		xfrmlen;
		size_t		xfrmlen2 PG_USED_FOR_ASSERTS_ONLY;

		/*
		 * XXX: 我们可以猜一个合适的输出缓冲区大小，
		 * 猜得太小才再调用 strxfrm。
		 *
		 * XXX: strxfrm 在 Win32 上不支持 UTF-8 编码，它可能
		 * 返回伪造数据或设置错误。除非它崩溃，
		 * 否则不算问题：它只会给出估算错误而已。
		 *
		 */
		xfrmlen = strxfrm(NULL, val, 0);
#ifdef WIN32

		/*
		 * 在 Windows 上，strxfrm 出错时返回 INT_MAX。
		 * 与其尝试分配这么多内存（并失败），不如
		 * 就像在 C 区域设置中一样原样返回原串。
		 */
		if (xfrmlen == INT_MAX)
			return val;
#endif
		xfrmstr = (char *) palloc(xfrmlen + 1);
		xfrmlen2 = strxfrm(xfrmstr, val, xfrmlen + 1);

		/*
		 * 某些系统（如 glibc）第二次调用可能返回比
		 * 第一次小的值；因此 Assert 必须是 <= 而非 ==。
		 */
		Assert(xfrmlen2 <= xfrmlen);
		pfree(val);
		val = xfrmstr;
	}

	return val;
}

/*
 * 对 bytea 做 convert_to_scalar() 的工作。
 *
 * 与 convert_string_to_scalar 非常相似，但我们不能
 * 假定以 NULL 结尾，因此要显式传递长度。
 *
 * 此外，关于字符可能"正常"范围的假定已被移除
 * —— 目前一律使用 0..255 的数据范围。(也许
 * 将来我们会把实际字节数据范围信息加进
 * pg_statistic.)
 */
static void
convert_bytea_to_scalar(Datum value,
						double *scaledvalue,
						Datum lobound,
						double *scaledlobound,
						Datum hibound,
						double *scaledhibound)
{
	bytea	   *valuep = DatumGetByteaPP(value);
	bytea	   *loboundp = DatumGetByteaPP(lobound);
	bytea	   *hiboundp = DatumGetByteaPP(hibound);
	int			rangelo,
				rangehi,
				valuelen = VARSIZE_ANY_EXHDR(valuep),
				loboundlen = VARSIZE_ANY_EXHDR(loboundp),
				hiboundlen = VARSIZE_ANY_EXHDR(hiboundp),
				i,
				minlen;
	unsigned char *valstr = (unsigned char *) VARDATA_ANY(valuep);
	unsigned char *lostr = (unsigned char *) VARDATA_ANY(loboundp);
	unsigned char *histr = (unsigned char *) VARDATA_ANY(hiboundp);

	/*
	 * 假定 bytea 数据在所有字节值上均匀分布。
	 */
	rangelo = 0;
	rangehi = 255;

	/*
	 * 去掉三个字符串的任何公共前缀。
	 */
	minlen = Min(Min(valuelen, loboundlen), hiboundlen);
	for (i = 0; i < minlen; i++)
	{
		if (*lostr != *histr || *lostr != *valstr)
			break;
		lostr++, histr++, valstr++;
		loboundlen--, hiboundlen--, valuelen--;
	}

	/*
	 * 现在可以做转换了。
	 */
	*scaledvalue = convert_one_bytea_to_scalar(valstr, valuelen, rangelo, rangehi);
	*scaledlobound = convert_one_bytea_to_scalar(lostr, loboundlen, rangelo, rangehi);
	*scaledhibound = convert_one_bytea_to_scalar(histr, hiboundlen, rangelo, rangehi);
}

static double
convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
							int rangelo, int rangehi)
{
	double		num,
				denom,
				base;

	if (valuelen <= 0)
		return 0.0;				/* 空字符串的标量值为 0 */

	/*
	 * 由于基数是 256，无需考虑超过约 10 个字符（
	 * 这么多已嫌太多）。
	 */
	if (valuelen > 10)
		valuelen = 10;

	/* 把开头的字符转换为分数 */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (valuelen-- > 0)
	{
		int			ch = *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * 对时间值类型做 convert_to_scalar() 的工作。
 *
 * 失败时把 *failure 置 true（如 typid 不支持）；
 * 否则该变量不变。
 */
static double
convert_timevalue_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case TIMESTAMPOID:
			return DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return DatumGetTimestampTz(value);
		case DATEOID:
			return date2timestamp_no_overflow(DatumGetDateADT(value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * 把 Interval 的月部分转为天，用假定月长
				 * 365.25/12.0 天。不很精确，但对我们
				 * 而言已足够好了。
				 */
				return interval->time + interval->day * (double) USECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
		case TIMETZOID:
			{
				TimeTzADT  *timetz = DatumGetTimeTzADTP(value);

				/* 使用 GMT 等价时间 */
				return (double) (timetz->time + (timetz->zone * 1000000.0));
			}
	}

	*failure = true;
	return 0;
}


/*
 * get_restriction_variable
 *		检查限制子句参数是否为 (变量 op 伪常量)
 *		或 (伪常量 op 变量) 形式，其中"变量"可以是 Var 或
 *		单一关系中 vars 的表达式。若是，则提取关于该
 *		变量的信息，并同时指出它在哪一侧以及另一
 *		个参数是什么。
 *
 * 输入:
 *	root: 规划器信息
 *	args: 子句参数列表
 *	varRelid: 见限制选择率函数的规格说明
 *
 * 输出:（仅当返回 true 时才有效）
 *	*vardata: 取得关于变量的信息（见 examine_variable）
 *	*other: 取得另一个子句参数，被积极简化为常量
 *	*varonleft: 变量在左侧时置为 true，在右侧时置为 false
 *
 * 若识别出变量则返回 true，否则返回 false。
 *
 * 注意：若子句两侧都有 Var，我们必须失败，因为
 * 调用者期望另一侧行如伪常量。
 */
bool
get_restriction_variable(PlannerInfo *root, List *args, int varRelid,
						 VariableStatData *vardata, Node **other,
						 bool *varonleft)
{
	Node	   *left,
			   *right;
	VariableStatData rdata;

	/* 若非二元算子子句则失败（大概不该发生） */
	if (list_length(args) != 2)
		return false;

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	/*
	 * 检查两侧。注意 varRelid 非零时，其他关系的
	 * Var 会被当作伪常量。
	 */
	examine_variable(root, left, varRelid, vardata);
	examine_variable(root, right, varRelid, &rdata);

	/*
	 * 若一侧是变量另一侧不是则成功。
	 */
	if (vardata->rel && rdata.rel == NULL)
	{
		*varonleft = true;
		*other = estimate_expression_value(root, rdata.var);
		/* 假定这里无需 ReleaseVariableStats(rdata) */
		return true;
	}

	if (vardata->rel == NULL && rdata.rel)
	{
		*varonleft = false;
		*other = estimate_expression_value(root, vardata->var);
		/* 假定这里无需 ReleaseVariableStats(*vardata) */
		*vardata = rdata;
		return true;
	}

	/* 糟糕，子句结构不对（大概是 var op var） */
	ReleaseVariableStats(*vardata);
	ReleaseVariableStats(rdata);

	return false;
}

/*
 * get_join_variables
 *		对连接子句的每一侧应用 examine_variable()。
 *		另外，尝试识别该连接子句与 SpecialJoinInfo 的
 *		语义相同还是相反。
 *
 * 若连接子句是 "lhs_var OP rhs_var"，我们认为它是
 * "正常的"；若是 "rhs_var OP lhs_var" 则是"反的"。
 * 在无法确定的复杂情形，默认假定它是正常的。
 */
void
get_join_variables(PlannerInfo *root, List *args, SpecialJoinInfo *sjinfo,
				   VariableStatData *vardata1, VariableStatData *vardata2,
				   bool *join_is_reversed)
{
	Node	   *left,
			   *right;

	if (list_length(args) != 2)
		elog(ERROR, "join operator should take two arguments");

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	examine_variable(root, left, 0, vardata1);
	examine_variable(root, right, 0, vardata2);

	if (vardata1->rel &&
		bms_is_subset(vardata1->rel->relids, sjinfo->syn_righthand))
		*join_is_reversed = true;	/* var1 在 RHS */
	else if (vardata2->rel &&
			 bms_is_subset(vardata2->rel->relids, sjinfo->syn_lefthand))
		*join_is_reversed = true;	/* var2 在 LHS */
	else
		*join_is_reversed = false;
}

/* statext_expressions_load 已复制元组，直接 pfree。 */
static void
ReleaseDummy(HeapTuple tuple)
{
	pfree(tuple);
}

/*
 * examine_variable
 *		尝试查找关于某表达式的统计信息。
 *		填写 VariableStatData 结构以描述该表达式。
 *
 * 输入:
 *	root: 规划器信息
 *	node: 要检查的表达式树
 *	varRelid: 见限制选择率函数的规格说明
 *
 * 输出: *vardata 按如下方式填写:
 *	var: 输入表达式（若它是变量或包含变量，则剥去
 *		二进制重标；否则类型保持原样）
 *	rel: 包含变量的关系的 RelOptInfo；若表达式
 *		不含 Var 则为 NULL（注意这可能指向子查询的
 *		RelOptInfo，非当前查询的）。
 *	statsTuple: 该变量的 pg_statistic 条目（若存在）；
 *		否则 NULL。
 *	freefunc: 用于释放 statsTuple 的函数指针。
 *	vartype: 表达式的暴露类型；它应当总是与我们
 *		正在为其估算的算子的声明输入类型一致。
 *	atttype, atttypmod: "var" 表达式的实际类型/typmod。这
 *		通常与变量参数的暴露类型相同，但在二进制
 *		兼容类型情形下可能不同。
 *	isunique: 若我们能把 var 匹配到唯一索引或单列
 *		DISTINCT 子句（意味着其值对本查询唯一），
 *		则为 true。（注意：只应作统计用途，因为
 *		我们既不检查 indimmediate，也不验证完全相同
 *		的相等定义适用。）
 *	acl_ok: 若当前用户有权读取 pg_statistic 条目底层
 *		的列的所有表行，则为 true。statistic_proc_security_check()
 *		会查询此项。
 *
 * 调用者负责在退出前执行 ReleaseVariableStats()。
 */
void
examine_variable(PlannerInfo *root, Node *node, int varRelid,
				 VariableStatData *vardata)
{
	Node	   *basenode;
	Relids		varnos;
	Relids		basevarnos;
	RelOptInfo *onerel;

	/* 确保我们不会在 vardata 中返回悬空指针 */
	MemSet(vardata, 0, sizeof(VariableStatData));

	/* 保存该表达式暴露的类型 */
	vardata->vartype = exprType(node);

	/* 窥探任何二进制兼容的 relabeling */

	if (IsA(node, RelabelType))
		basenode = (Node *) ((RelabelType *) node)->arg;
	else
		basenode = node;

	/* 简单 Var 快速路径 */

	if (IsA(basenode, Var) &&
		(varRelid == 0 || varRelid == ((Var *) basenode)->varno))
	{
		Var		   *var = (Var *) basenode;

		/* 设置统计信息元组以外的结果字段 */
		vardata->var = basenode;	/* 返回 Var，未 relabeling */
		vardata->rel = find_base_rel(root, var->varno);
		vardata->atttype = var->vartype;
		vardata->atttypmod = var->vartypmod;
		vardata->isunique = has_unique_index(vardata->rel, var->varattno);

		/* 尝试找统计信息 */
		examine_simple_variable(root, var, vardata);

		return;
	}

	/*
	 * 好，它是一个更复杂的表达式。确定变量的
	 * 成员归属。注意当 varRelid 非零时，只有该
	 * 关系的变量算真实变量。
	 */
	varnos = pull_varnos(root, basenode);
	basevarnos = bms_difference(varnos, root->outer_join_rels);

	onerel = NULL;

	switch (bms_membership(basevarnos))
	{
		case BMS_EMPTY_SET:
			/* 完全没有 Var……必是伪常量子句 */
			break;
		case BMS_SINGLETON:
			if (varRelid == 0 || bms_is_member(varRelid, varnos))
			{
				onerel = find_base_rel(root,
									   (varRelid ? varRelid : bms_singleton_member(basevarnos)));
				vardata->rel = onerel;
				node = basenode;	/* 剥掉 relabeling */
			}
			/* 否则当作常量处理 */
			break;
		case BMS_MULTIPLE:
			if (varRelid == 0)
			{
				/* 把它当作连接关系的一个变量 */
				vardata->rel = find_join_rel(root, varnos);
				node = basenode;	/* 剥掉 relabeling */
			}
			else if (bms_is_member(varRelid, varnos))
			{
				/* 忽略属于其他关系的变量 */
				vardata->rel = find_base_rel(root, varRelid);
				node = basenode;	/* 剥掉 relabeling */
				/* 注意：此处无需表达式索引搜索 */
			}
			/* 否则当作常量处理 */
			break;
	}

	bms_free(basevarnos);

	vardata->var = node;
	vardata->atttype = exprType(node);
	vardata->atttypmod = exprTypmod(node);

	if (onerel)
	{
		/*
		 * 我们有一个单关系变量的表达式。尝试把它
		 * 匹配到表达式索引列，希望能找到一些统计
		 * 信息。
		 *
		 * 注意我们会考虑包括 INCLUDE 列在内的所有索引列，
		 * 因为这样的列可能有统计信息。但唯一性检查
		 * 需要更加谨慎。
		 *
		 * XXX 可以设想存在多个匹配，分属不同的
		 * 索引 opfamily；若是这样，我们需要挑一个与
		 * 所估算的算子相匹配。稍后 FIXME。
		 */
		ListCell   *ilist;
		ListCell   *slist;

		/*
		 * 表达式内的 nullingrels 位可能妨碍我们把它匹配到
		 * 表达式索引列或扩展统计信息的表达式上。因此先
		 * 把它们剥离掉。
		 */
		if (bms_overlap(varnos, root->outer_join_rels))
			node = remove_nulling_relids(node, root->outer_join_rels, NULL);

		foreach(ilist, onerel->indexlist)
		{
			IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
			ListCell   *indexpr_item;
			int			pos;

			indexpr_item = list_head(index->indexprs);
			if (indexpr_item == NULL)
				continue;		/* 此处无表达式… */

			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == 0)
				{
					Node	   *indexkey;

					if (indexpr_item == NULL)
						elog(ERROR, "too few entries in indexprs list");
					indexkey = (Node *) lfirst(indexpr_item);
					if (indexkey && IsA(indexkey, RelabelType))
						indexkey = (Node *) ((RelabelType *) indexkey)->arg;
					if (equal(node, indexkey))
					{
						/*
						 * 找到匹配……是唯一索引吗？测试
						 * 应同 has_unique_index()。
						 */
						if (index->unique &&
							index->nkeycolumns == 1 &&
							pos == 0 &&
							(index->indpred == NIL || index->predOK))
							vardata->isunique = true;

						/*
						 * 它有统计信息吗？只考虑非部分
						 * 索引的统计信息，因为部分索引多半不
						 * 反映整表的统计信息；上文对唯一
						 * 性的检查是我们从部分索引得到的
						 * 唯一信息。
						 *
						 * 不过，索引统计钩子必须自行决定
						 * 如何处理部分索引。
						 */
						if (get_index_stats_hook &&
							(*get_index_stats_hook) (root, index->indexoid,
													 pos + 1, vardata))
						{
							/*
							 * 钩子接管了获取统计元组任务。
							 * 若它确实提供了元组，最好也
							 * 提供了 freefunc。
							 */
							if (HeapTupleIsValid(vardata->statsTuple) &&
								!vardata->freefunc)
								elog(ERROR, "no function provided to release variable stats with");
						}
						else if (index->indpred == NIL)
						{
							vardata->statsTuple =
								SearchSysCache3(STATRELATTINH,
												ObjectIdGetDatum(index->indexoid),
												Int16GetDatum(pos + 1),
												BoolGetDatum(false));
							vardata->freefunc = ReleaseSysCache;

							if (HeapTupleIsValid(vardata->statsTuple))
							{
								/*
								 * 测试用户是否有权从索引的表
								 * 访问所有行。
								 *
								 * 为简单起见，我们坚持整表
								 * 选择，而不去尝试识别该索引
								 * 依赖于哪些列。
								 *
								 *
								 * 注意对继承子表，权限在
								 * 继承根父表上检查；父表整表
								 * select 权限不完全保证用户
								 * 能读子表所有列。但实践中
								 * 允许访问表达式索引的统计
								 * 信息不太可能造成值得关注的
								 * 安全违规，因此我们仍允许
								 * 它。类似代码参见
								 * examine_simple_variable() 里的
								 * 更多注释。
								 *
								 *
								 */
								vardata->acl_ok =
									all_rows_selectable(root,
														index->rel->relid,
														NULL);
							}
							else
							{
								/* 抑制之后的防泄漏检查 */
								vardata->acl_ok = true;
							}
						}
						if (vardata->statsTuple)
							break;
					}
					indexpr_item = lnext(index->indexprs, indexpr_item);
				}
			}
			if (vardata->statsTuple)
				break;
		}

		/*
		 * 在扩展统计信息中找与该表达式匹配的一个。
		 * 可能有多个，所以只取第一个。将来我们可能
		 * 会考虑统计目标（并挑出最准确的统计信息）
		 * 以及也许其他一些参数。
		 */
		foreach(slist, onerel->statlist)
		{
			StatisticExtInfo *info = (StatisticExtInfo *) lfirst(slist);
			RangeTblEntry *rte = planner_rt_fetch(onerel->relid, root);
			ListCell   *expr_item;
			int			pos;

			/*
			 * 一旦找到该表达式的统计信息就停止（无论
			 * 来自扩展统计还是前一循环的索引）。
			 */
			if (vardata->statsTuple)
				break;

			/* 无每表达式统计则跳过 */
			if (info->kind != STATS_EXT_EXPRESSIONS)
				continue;

			/* 跳过 stxdinherit 值不匹配的统计 */
			if (info->inherit != rte->inh)
				continue;

			pos = 0;
			foreach(expr_item, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(expr_item);

				Assert(expr);

				/* 比较前先剥掉 RelabelType */
				if (expr && IsA(expr, RelabelType))
					expr = (Node *) ((RelabelType *) expr)->arg;

				/* 找到匹配，看能否提取 pg_statistic 行 */
				if (equal(node, expr))
				{
					/*
					 * XXX 不确定是否应把元组缓存在某处。
					 * 现在我们只是每次建新副本。
					 */
					vardata->statsTuple =
						statext_expressions_load(info->statOid, rte->inh, pos);

					vardata->freefunc = ReleaseDummy;

					/*
					 * 测试用户是否有权限访问该表的所有
					 * 行。
					 *
					 * 为简单起见，我们坚持整表可选择，
					 * 而不去尝试识别统计对象依赖于
					 * 哪些列。
					 *
					 * 注意对继承子表，权限是在继承根父表
					 * 上检查；父表整表 select 权限不完全
					 * 保证用户能读到子表的所有列。但实践中
					 * 不太可能因允许访问表达式统计信息而
					 * 造成任何值得关注的安全违规，所以我们
					 * 仍然允许它。类似代码参见
					 * examine_simple_variable() 中更多的
					 * 注释。
					 *
					 */
					vardata->acl_ok = all_rows_selectable(root,
														  onerel->relid,
														  NULL);

					break;
				}

				pos++;
			}
		}
	}

	bms_free(varnos);
}

/*
 * examine_simple_variable
 *		为 examine_variable 处理简单 Var
 *
 * 本函数单独拆成子例程，以便能够递归处理引用
 * 子查询的 Var。
 *
 * 我们已填好 *vardata 除统计元组外的全部字段。
 */
static void
examine_simple_variable(PlannerInfo *root, Var *var,
						VariableStatData *vardata)
{
	RangeTblEntry *rte = root->simple_rte_array[var->varno];

	Assert(IsA(rte, RangeTblEntry));

	if (get_relation_stats_hook &&
		(*get_relation_stats_hook) (root, rte, var->varattno, vardata))
	{
		/*
		 * 钩子接管了获取统计信息元组的工作。如果它确实
		 * 提供了元组，则最好有 freefunc。
		 */
		if (HeapTupleIsValid(vardata->statsTuple) &&
			!vardata->freefunc)
			elog(ERROR, "no function provided to release variable stats with");
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * 普通表或继承 appendrel 的父表，所以在 pg_statistic
		 * 中查找该列。
		 */
		vardata->statsTuple = SearchSysCache3(STATRELATTINH,
											  ObjectIdGetDatum(rte->relid),
											  Int16GetDatum(var->varattno),
											  BoolGetDatum(rte->inh));
		vardata->freefunc = ReleaseSysCache;

		if (HeapTupleIsValid(vardata->statsTuple))
		{
			/*
			 * 测试用户是否有权限读取此列的所有行。
			 *
			 * 这要求用户有相应 SELECT 权限，且没有
			 * 来自安全屏障视图或 RLS 策略的 securityQuals。
			 * 若情况并非如此，那么我们只允许防泄漏函数
			 * 接收传入 vardata 的 pg_statistic 数据，否则这些
			 * 函数可能泄露用户无权查看的数据 --- 参见
			 * statistic_proc_security_check()。
			 *
			 */
			vardata->acl_ok =
				all_rows_selectable(root, var->varno,
									bms_make_singleton(var->varattno - FirstLowInvalidHeapAttributeNumber));
		}
		else
		{
			/* 抑制之后任何可能的防泄漏检查 */
			vardata->acl_ok = true;
		}
	}
	else if (rte->rtekind == RTE_SUBQUERY && !rte->inh)
	{
		/*
		 * 普通子查询（非转换为 appendrel 的那种）。
		 */
		Query	   *subquery = rte->subquery;
		RelOptInfo *rel;
		TargetEntry *ste;

		/*
		 * 若它是整行 var 而非普通列引用，则放弃处理。
		 */
		if (var->varattno == InvalidAttrNumber)
			return;

		/*
		 * 若子查询使用集合操作或 GROUP BY 则放弃处理，
		 * 因为它们会把底层列的统计信息搅得无法辨认。
		 * （集合操作尤其麻烦；若强行继续，将返回
		 * 只与最左侧子选择相关的统计信息...）DISTINCT
		 * 也有问题，但我们稍后才检查它，因为即使有它
		 * 也仍可能学到一些东西。
		 */
		if (subquery->setOperations ||
			subquery->groupClause ||
			subquery->groupingSets)
			return;

		/*
		 * 好，获取子查询的 RelOptInfo。注意我们不改
		 * vardata 中返回的 rel，因为调用者期望它是
		 * 调用者查询层级的 rel。因为我们可能已经在
		 * 递归中，所以也不能用那个 rel 指针，得重新
		 * 查找。
		 */
		rel = find_base_rel(root, var->varno);

		/* 若子查询尚未被规划，我们必须放弃 */
		if (rel->subroot == NULL)
			return;
		Assert(IsA(rel->subroot, PlannerInfo));

		/*
		 * 把注意力切换到被规划器改造过的子查询上。前面
		 * 的测试看规划前版本没问题，但现在我们需要
		 * 一个引用 subroot 现行 RelOptInfo 的 Var。
		 * 例如，若规划期间发生了任何子查询上拉，
		 * targetlist 中的 Var 可能已被替换，我们需要
		 * 看到替换后的表达式。
		 */
		subquery = rel->subroot->parse;
		Assert(IsA(subquery, Query));

		/* 取得上层 Var 所引用的子查询输出表达式 */
		ste = get_tle_by_resno(subquery->targetList, var->varattno);
		if (ste == NULL || ste->resjunk)
			elog(ERROR, "subquery %s does not have attribute %d",
				 rte->eref->aliasname, var->varattno);
		var = (Var *) ste->expr;

		/*
		 * 若子查询使用了 DISTINCT，我们不能利用该
		 * 变量的任何统计信息……但如果它是唯一的
		 * DISTINCT 列，我们有权把它当作唯一的。这样
		 * 测试就适用于 DISTINCT ON 情形。
		 */
		if (subquery->distinctClause)
		{
			if (list_length(subquery->distinctClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->distinctClause))
				vardata->isunique = true;
			/* 到此为止 */
			return;
		}

		/*
		 * 若该子查询源自带有 security_barrier 属性的视图，
		 * 我们就必须不去查看变量的统计信息，尽管注意
		 * 到 DISTINCT 子句的存在似乎没问题。故到此为止。
		 *
		 *
		 * 这是比必要程度更严厉的限制；对选择率
		 * 估算器（它是 C 函数，因此无论如何都无所不能）
		 * 来说查看统计信息当然没问题。但很多选择率
		 * 估算器会乐意*调用算子函数*来试图算出一个
		 * 好的估算 --- 而这是不被允许的。所以目前
		 * 先不要向下挖掘统计信息。
		 */
		if (rte->security_barrier)
			return;

		/* 只能处理子查询查询层级的简单 Var */
		if (var && IsA(var, Var) &&
			var->varlevelsup == 0)
		{
			/*
			 * 好，递归进入子查询。注意 vardata->isunique 的
			 * 原设置（必为假）在此情形下不变。这
			 * 正是我们想要的，因为即使底层列是唯一的，
			 * 子查询也可能以产生重复的方式连接到
			 * 其他表。
			 */
			examine_simple_variable(rel->subroot, var, vardata);
		}
	}
	else
	{
		/*
		 * 否则，该 Var 来自 FUNCTION、VALUES 或 CTE 的
		 * RTE。（我们不会在这里看到 RTE_JOIN，因为
		 * 连接别名 Var 已被展平。）函数输出我们
		 * 无能为力，但也许有一天能聪明处理 VALUES/CTE。
		 */
	}
}

/*
 * all_rows_selectable
 *		测试用户是否有权从给定关系中选择所有行
 *
 *
 * 输入:
 *	root: 规划器信息
 *	varno: 关系的下标（假定为 RTE_RELATION）
 *	varattnos: 需要权限的属性，若需要整表访问则为
 *		NULL。
 *
 * 若用户有所需的 select 权限，并且没有来自安全
 * 屏障视图或 RLS 策略的 securityQuals，则返回真。
 *
 * 注意若该关系是继承子关系，securityQuals 与访问
 * 权限是针对继承根父表（查询中实际提到的关系）
 * 检查的 --- 为何必须如此，参见
 * expand_single_inheritance_child() 中的注释。
 *
 *
 * 若 varattnos 非 NULL，其属性号应偏移
 * FirstLowInvalidHeapAttributeNumber，以便能
 * 检查系统属性。若 varattnos 为 NULL，则只检查表级
 * SELECT 权限，非列级权限。
 *
 * 注意：若该关系是经视图访问的，本函数实际测试
 * 的是视图属主是否有从该关系选择的权限。要
 * 确保当前用户有权限，还需检查当前用户是否有
 * 从视图选择的权限，我们在规划器启动时做这件
 * 事 --- 参见 subquery_planner()。
 *
 * 导出此函数是为了让其他估算函数能够使用。
 */
bool
all_rows_selectable(PlannerInfo *root, Index varno, Bitmapset *varattnos)
{
	RelOptInfo *rel = find_base_rel(root, varno);
	RangeTblEntry *rte = planner_rt_fetch(varno, root);
	Oid			userid;
	int			varattno;

	Assert(rte->rtekind == RTE_RELATION);

	/*
	 * 用于权限检查的用户 ID（当前用户，或当我们
	 * 经视图访问表时的视图属主）。
	 *
	 * 如果我们向上导航到父关系，则继续使用同一个
	 * userid，因为同一继承树的所有关系里它都相同。
	 */
	userid = OidIsValid(rel->userid) ? rel->userid : GetUserId();

	/*
	 * 权限与 securityQuals 必须针对查询中实际提到
	 * 的表检查，所以如果这是继承子表，就向上导航
	 * 到继承根父表。如果用户能在那里读取整表或
	 * 所需列，那么它们也能从子表读取。对于按列
	 * 检查，我们必须找出子关系属性对应于根父
	 * 表的哪些属性。
	 */
	if (root->append_rel_array != NULL)
	{
		AppendRelInfo *appinfo;

		appinfo = root->append_rel_array[varno];

		/*
		 * 分区映射到直接父表，而非根父表，故必须
		 * 准备好走过多级 AppendRelInfo。但如果碰到的
		 * 父表不是 RTE_RELATION 就停下来 --- 那是被
		 * 展平的 UNION ALL 子查询，不是继承父表。
		 */
		while (appinfo &&
			   planner_rt_fetch(appinfo->parent_relid,
								root)->rtekind == RTE_RELATION)
		{
			Bitmapset  *parent_varattnos = NULL;

			/*
			 * 对每个子属性，找到对应的父属性。在
			 * 罕见情况下，属性可能是子表本地的，
			 * 这种情况下我们只能接受无法访问
			 * 此列。
			 */
			varattno = -1;
			while ((varattno = bms_next_member(varattnos, varattno)) >= 0)
			{
				AttrNumber	attno;
				AttrNumber	parent_attno;

				attno = varattno + FirstLowInvalidHeapAttributeNumber;

				if (attno == InvalidAttrNumber)
				{
					/*
					 * 整行引用，必须把子表每列映射
					 * 到父表。
					 */
					for (attno = 1; attno <= appinfo->num_child_cols; attno++)
					{
						parent_attno = appinfo->parent_colnos[attno - 1];
						if (parent_attno == 0)
							return false;	/* 属性为子表本地 */
						parent_varattnos =
							bms_add_member(parent_varattnos,
										   parent_attno - FirstLowInvalidHeapAttributeNumber);
					}
				}
				else
				{
					if (attno < 0)
					{
						/* 系统属性号在所有表中相同 */
						parent_attno = attno;
					}
					else
					{
						if (attno > appinfo->num_child_cols)
							return false;	/* 安全检查 */
						parent_attno = appinfo->parent_colnos[attno - 1];
						if (parent_attno == 0)
							return false;	/* 属性为子表本地 */
					}
					parent_varattnos =
						bms_add_member(parent_varattnos,
									   parent_attno - FirstLowInvalidHeapAttributeNumber);
				}
			}

			/* 若父表本身是子表，则继续向上 */
			varno = appinfo->parent_relid;
			varattnos = parent_varattnos;
			appinfo = root->append_rel_array[varno];
		}

		/* 对该父关系执行访问检查 */
		rte = planner_rt_fetch(varno, root);
		Assert(rte->rtekind == RTE_RELATION);
	}

	/*
	 * 要能访问所有行，必须没有来自安全屏障视图
	 * 或 RLS 策略的 securityQuals。
	 */
	if (rte->securityQuals != NIL)
		return false;

	/*
	 * 测试表级 SELECT 权限。
	 *
	 * 若 varattnos 非 NULL，这就足以给出对所有
	 * 请求属性的访问，即使是子表也一样，因为我们
	 * 已经验证所有必需的子列都有匹配的父列。
	 *
	 * 若 varattnos 为 NULL（请求整表访问），这并不
	 * 必然保证用户能读取子表的所有列，但我们
	 * 无论如何都允许它（参见 examine_variable() 中的
	 * 注释），并不检查列权限。
	 */
	if (pg_class_aclcheck(rte->relid, userid, ACL_SELECT) == ACLCHECK_OK)
		return true;

	if (varattnos == NULL)
		return false;			/* 请求的是整表访问 */

	/*
	 * 没有表级 SELECT 权限，所以检查按列
	 * 权限。
	 */
	varattno = -1;
	while ((varattno = bms_next_member(varattnos, varattno)) >= 0)
	{
		AttrNumber	attno = varattno + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/* 整行引用，所以必须能访问所有列 */
			if (pg_attribute_aclcheck_all(rte->relid, userid, ACL_SELECT,
										  ACLMASK_ALL) != ACLCHECK_OK)
				return false;
		}
		else
		{
			if (pg_attribute_aclcheck(rte->relid, attno, userid,
									  ACL_SELECT) != ACLCHECK_OK)
				return false;
		}
	}

	/* 走到这里时，就已具备全部所需列权限 */
	return true;
}

/*
 * 检查是否允许调用 func_oid 并向其传入 vardata 中的
 * pg_statistic 数据。满足下列条件之一即允许：
 * （1）用户对 pg_statistic 数据底层的表或列有
 * SELECT 权限且没有来自安全屏障视图或 RLS 策略的
 * securityQuals，或者（2）该函数被标记为
 * leakproof.
 */
bool
statistic_proc_security_check(VariableStatData *vardata, Oid func_oid)
{
	if (vardata->acl_ok)
		return true;			/* 有 SELECT 权限且无 securityQuals */

	if (!OidIsValid(func_oid))
		return false;

	if (get_func_leakproof(func_oid))
		return true;

	ereport(DEBUG2,
			(errmsg_internal("not using statistics because function \"%s\" is not leak-proof",
							 get_func_name(func_oid))));
	return false;
}

/*
 * get_variable_numdistinct
 *	  估计一个变量的不同值数目。
 *
 * vardata: examine_variable 的结果
 * *isdefault: 若结果不是基于任何有意义的东西，
 * 则设为真。
 *
 * NB: 注意要产生正整数值的结果，因为调用者可能
 * 把结果与精确的整数计数相比较，或用它做除数。
 */
double
get_variable_numdistinct(VariableStatData *vardata, bool *isdefault)
{
	double		stadistinct;
	double		stanullfrac = 0.0;
	double		ntuples;

	*isdefault = false;

	/*
	 * 确定要使用的 stadistinct 值。有些情形即使没有
	 * pg_statistic 条目也能得到估算，或者能得到比
	 * pg_statistic 中更好的值。若能找到也一并取
	 * 得 stanullfrac（否则只好假定无空值）。
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		/* 使用 pg_statistic 条目 */
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		stadistinct = stats->stadistinct;
		stanullfrac = stats->stanullfrac;
	}
	else if (vardata->vartype == BOOLOID)
	{
		/*
		 * 布尔列特殊处理：大概有两个不同值。
		 *
		 * 还有没有其他类型值得我们接入特殊估算？
		 *
		 */
		stadistinct = 2.0;
	}
	else if (vardata->rel && vardata->rel->rtekind == RTE_VALUES)
	{
		/*
		 * 若 Var 表示 VALUES RTE 的一列，则假定它是唯一
		 * 的。这当然可能大错特错，但对写得好的查询它
		 * 应当趋于成立。我们可以考虑检查 VALUES 的
		 * 内容来获得一些真实统计信息；但那只在本
		 * 条目都是常量时才行，而且无论如何都相当昂贵。
		 */
		stadistinct = -1.0;		/* 唯一（且全非空） */
	}
	else
	{
		/*
		 * 我们不保留系统列的统计信息，但某些情形
		 * 还是能推断出不同性。
		 */
		if (vardata->var && IsA(vardata->var, Var))
		{
			switch (((Var *) vardata->var)->varattno)
			{
				case SelfItemPointerAttributeNumber:
					stadistinct = -1.0; /* 唯一（且全非空） */
					break;
				case TableOidAttributeNumber:
					stadistinct = 1.0;	/* 仅 1 个值 */
					break;
				default:
					stadistinct = 0.0;	/* 表示"未知" */
					break;
			}
		}
		else
			stadistinct = 0.0;	/* 表示"未知" */

		/*
		 * XXX 考虑对表达式使用 estimate_num_groups？
		 */
	}

	/*
	 * 若变量有唯一索引或 DISTINCT 子句，则无论 pg_statistic
	 * 怎么说都假定它唯一；统计信息可能已过期，或者
	 * 找到的可能只是部分唯一索引，但它能证明变量对此
	 * 查询唯一。不过，我们最好仍然相信
	 * 空值比例统计信息。
	 */
	if (vardata->isunique)
		stadistinct = -1.0 * (1.0 - stanullfrac);

	/*
	 * 若已有绝对估算，就用它。
	 */
	if (stadistinct > 0.0)
		return clamp_row_est(stadistinct);

	/*
	 * 否则我们需要取得关系大小；若不可得就放弃。
	 */
	if (vardata->rel == NULL)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}
	ntuples = vardata->rel->tuples;
	if (ntuples <= 0.0)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}

	/*
	 * 若已有相对估算，就用它。
	 */
	if (stadistinct < 0.0)
		return clamp_row_est(-stadistinct * ntuples);

	/*
	 * 没有数据时，若表较小则估计 ndistinct = ntuples，否则
	 * 使用默认值。我们用 DEFAULT_NUM_DISTINCT 作为"小"
	 * 的界限，使行为不跳变。
	 */
	if (ntuples < DEFAULT_NUM_DISTINCT)
		return clamp_row_est(ntuples);

	*isdefault = true;
	return DEFAULT_NUM_DISTINCT;
}

/*
 * get_variable_range
 *		估计指定变量的最小值与最大值。
 *		若成功，将值存入 *min 与 *max 并返回真。
 *		若无数据可用，返回假。
 *
 * sortop 是要使用的 "<" 比较算子。通常应当用 "<" 而
 * 不是 ">"，因为只有前者才可能存在于 pg_statistic 中。
 * 还必须指定排序规则。
 */
static bool
get_variable_range(PlannerInfo *root, VariableStatData *vardata,
				   Oid sortop, Oid collation,
				   Datum *min, Datum *max)
{
	Datum		tmin = 0;
	Datum		tmax = 0;
	bool		have_data = false;
	int16		typLen;
	bool		typByVal;
	Oid			opfuncoid;
	FmgrInfo	opproc;
	AttStatsSlot sslot;

	/*
	 * XXX 很想尝试用索引探测较廉价地取得实际列的
	 * 最小值与最大值。但由于本函数在连接规划期间会被
	 * 调用很多次，这可能对规划速度产生不良影响。
	 * 在启用此做法之前需要更多调查。
	 *
	 */
#ifdef NOT_USED
	if (get_actual_variable_range(root, vardata, sortop, collation, min, max))
		return true;
#endif

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/* 无统计信息，用默认结果 */
		return false;
	}

	/*
	 * 若无法把 sortop 应用于统计数据，就直接失败。
	 * 原则上，若有直方图而无 MCV，我们可以不应用
	 * sortop 就返回直方图端点……但这不值得尝试，
	 * 因为无论调用者想用这些端点做什么，多半也通不过
	 * 安全检查。
	 */
	if (!statistic_proc_security_check(vardata,
									   (opfuncoid = get_opcode(sortop))))
		return false;

	opproc.fn_oid = InvalidOid; /* 标记为尚未查找 */

	get_typlenbyval(vardata->atttype, &typLen, &typByVal);

	/*
	 * 若有带我们所需排序的直方图，就取首尾
	 * 两个值。
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, sortop,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.stacoll == collation && sslot.nvalues > 0)
		{
			tmin = datumCopy(sslot.values[0], typByVal, typLen);
			tmax = datumCopy(sslot.values[sslot.nvalues - 1], typByVal, typLen);
			have_data = true;
		}
		free_attstatsslot(&sslot);
	}

	/*
	 * 否则，若有带其他排序的直方图，就扫描它并按
	 * 我们想要的排序取得最小与最大值。当然这可能
	 * 找不到按我们排序而言真正极端的值，但这总比
	 * 无视可用数据要好。
	 */
	if (!have_data &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		get_stats_slot_range(&sslot, opfuncoid, &opproc,
							 collation, typLen, typByVal,
							 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	/*
	 * 若有最常用值信息，就查找极端的 MCV。即便同时
	 * 有直方图这一步也是必需的，因为直方图不含 MCV。
	 * 然而，如果我们*只有* MCV 而无直方图，断言它
	 * 就是数据的完整表示就应相当谨慎。只有当 MCV
	 * 能代表整张表（误差在舍入范围内）时才继续。
	 *
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 have_data ? ATTSTATSSLOT_VALUES :
						 (ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)))
	{
		bool		use_mcvs = have_data;

		if (!have_data)
		{
			double		sumcommon = 0.0;
			double		nullfrac;
			int			i;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata->statsTuple))->stanullfrac;
			if (sumcommon + nullfrac > 0.99999)
				use_mcvs = true;
		}

		if (use_mcvs)
			get_stats_slot_range(&sslot, opfuncoid, &opproc,
								 collation, typLen, typByVal,
								 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	*min = tmin;
	*max = tmax;
	return have_data;
}

/*
 * get_stats_slot_range: 扫描 sslot 求 min/max
 *
 * get_variable_range 的子例程：根据在统计信息数组中
 * 的发现更新 min/max/have_data。
 */
static void
get_stats_slot_range(AttStatsSlot *sslot, Oid opfuncoid, FmgrInfo *opproc,
					 Oid collation, int16 typLen, bool typByVal,
					 Datum *min, Datum *max, bool *p_have_data)
{
	Datum		tmin = *min;
	Datum		tmax = *max;
	bool		have_data = *p_have_data;
	bool		found_tmin = false;
	bool		found_tmax = false;

	/* 若尚未查找过，就查找比较函数 */
	if (opproc->fn_oid != opfuncoid)
		fmgr_info(opfuncoid, opproc);

	/* 扫描该槽的全部值 */
	for (int i = 0; i < sslot->nvalues; i++)
	{
		if (!have_data)
		{
			tmin = tmax = sslot->values[i];
			found_tmin = found_tmax = true;
			*p_have_data = have_data = true;
			continue;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   sslot->values[i], tmin)))
		{
			tmin = sslot->values[i];
			found_tmin = true;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   tmax, sslot->values[i])))
		{
			tmax = sslot->values[i];
			found_tmax = true;
		}
	}

	/*
	 * 若发现了新的极端值，就拷贝该槽的值。
	 */
	if (found_tmin)
		*min = datumCopy(tmin, typByVal, typLen);
	if (found_tmax)
		*max = datumCopy(tmax, typByVal, typLen);
}


/*
 * get_actual_variable_range
 *		尝试识别指定变量当前*实际*的最小值
 *		和/或最大值，方法是寻找合适的 btree 索引并取
 *		其低端和/或高端值。
 *		若成功，将值存入 *min 与 *max 并返回真。
 *		（任一指针可为 NULL，若不需要该端点。）
 *		若失败，返回假。
 *
 * sortop 是要使用的 "<" 比较算子。
 * collation 为所需排序规则。
 */
static bool
get_actual_variable_range(PlannerInfo *root, VariableStatData *vardata,
						  Oid sortop, Oid collation,
						  Datum *min, Datum *max)
{
	bool		have_data = false;
	RelOptInfo *rel = vardata->rel;
	RangeTblEntry *rte;
	ListCell   *lc;

	/* 若无关系或其无索引，则没有希望 */
	if (rel == NULL || rel->indexlist == NIL)
		return false;
	/* 若有索引则必为普通关系 */
	rte = root->simple_rte_array[rel->relid];
	Assert(rte->rtekind == RTE_RELATION);

	/* 无视分区表。这里的任何索引都不是真索引 */
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/* 遍历各索引，看是否有匹配我们问题的 */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ScanDirection indexscandir;

		/* 无视非 btree 索引 */
		if (index->relam != BTREE_AM_OID)
			continue;

		/*
		 * 无视部分索引 --- 只想要覆盖整个关系的统计。
		 *
		 */
		if (index->indpred != NIL)
			continue;

		/*
		 * 索引列表可能包含由 get_relation_info 钩子插入的
		 * 假想索引 --- 不要试图访问它们。
		 */
		if (index->hypothetical)
			continue;

		/*
		 * 第一个索引列必须匹配所需的变量、sortop 与
		 * 排序规则 --- 但我们可以使用降序索引。
		 */
		if (collation != index->indexcollations[0])
			continue;			/* 先测它，因其最廉价 */
		if (!match_index_to_operand(vardata->var, 0, index))
			continue;
		switch (get_op_opfamily_strategy(sortop, index->sortopfamily[0]))
		{
			case BTLessStrategyNumber:
				if (index->reverse_sort[0])
					indexscandir = BackwardScanDirection;
				else
					indexscandir = ForwardScanDirection;
				break;
			case BTGreaterStrategyNumber:
				if (index->reverse_sort[0])
					indexscandir = ForwardScanDirection;
				else
					indexscandir = BackwardScanDirection;
				break;
			default:
				/* 索引与 sortop 不匹配 */
				continue;
		}

		/*
		 * 找到了可从中提取数据的合适索引。设置一些供
		 * get_actual_variable_endpoint 两次调用都用的数据。
		 */
		{
			MemoryContext tmpcontext;
			MemoryContext oldcontext;
			Relation	heapRel;
			Relation	indexRel;
			TupleTableSlot *slot;
			int16		typLen;
			bool		typByVal;
			ScanKeyData scankeys[1];

			/* 确保完成后所有杂物都被回收 */
			tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "get_actual_variable_range workspace",
											   ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(tmpcontext);

			/*
			 * 打开表与索引以便读取。两者上应已持有
			 * 某种锁。
			 */
			heapRel = table_open(rte->relid, NoLock);
			indexRel = index_open(index->indexoid, NoLock);

			/* 构建索引扫描执行所需的一些东西 */
			slot = table_slot_create(heapRel, NULL);
			get_typlenbyval(vardata->atttype, &typLen, &typByVal);

			/* 设置 IS NOT NULL 扫描键以忽略空值 */
			ScanKeyEntryInitialize(&scankeys[0],
								   SK_ISNULL | SK_SEARCHNOTNULL,
								   1,	/* 扫描索引列 */
								   InvalidStrategy, /* 无策略 */
								   InvalidOid,	/* 无策略子类型 */
								   InvalidOid,	/* 不指定 */
								   InvalidOid,	/* 无 reg proc */
								   (Datum) 0);	/* 常量 */

			/* 若请求了 min… */
			if (min)
			{
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 min);
			}
			else
			{
				/* 若未请求 min，仍要取最大值 */
				have_data = true;
			}

			/* 若请求了最大值且尚未失败…… */
			if (max && have_data)
			{
				/* 沿相反方向扫描；其余都相同 */
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 -indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 max);
			}

			/* 清理一切 */
			ExecDropSingleTupleTableSlot(slot);

			index_close(indexRel, NoLock);
			table_close(heapRel, NoLock);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(tmpcontext);

			/* 至此完成 */
			break;
		}
	}

	return have_data;
}

/*
 * 从指定索引取得一个端点 Datum（最小或最大，取决于
 * indexscandir）。成功返回真，失败返回假。
 * 成功时，端点值存入 *endpointDatum，并拷贝到
 * outercontext。
 *
 * scankeys 为拒绝空值的 1 元素 scankey 数组。
 * typLen/typByVal 描述索引第一列的数据类型。
 * tableslot 是适合存放表元组的槽，以备我们需要
 * 探测堆。
 * （这些值本可以在本地计算，但那意味着当
 * get_actual_variable_range 需要 min 与 max 时须算两遍。）
 *
 * 失败发生于索引为空时，或我们判定找到合适
 * 元组耗时过长时。
 */
static bool
get_actual_variable_endpoint(Relation heapRel,
							 Relation indexRel,
							 ScanDirection indexscandir,
							 ScanKey scankeys,
							 int16 typLen,
							 bool typByVal,
							 TupleTableSlot *tableslot,
							 MemoryContext outercontext,
							 Datum *endpointDatum)
{
	bool		have_data = false;
	SnapshotData SnapshotNonVacuumable;
	IndexScanDesc index_scan;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber last_heap_block = InvalidBlockNumber;
	int			n_visited_heap_pages = 0;
	ItemPointer tid;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	MemoryContext oldcontext;

	/*
	 * 我们为此使用 index-only-scan 机制。对基本静态的
	 * 表这是一种赢法，因它避免了堆访问。对动态数据
	 * 它同样是赢法，但原因不那么明显；详情见下文。
	 *
	 * 原则上，我们应当用当前活动快照扫描索引，
	 * 这是我们对查询执行时所见内容的最佳近似。
	 * 但如果在运行查询前又取了新快照，结果就不精确，
	 * 而且若索引开头或结尾存在大量最近死亡或未提交
	 * 的行，代价可能非常高（因为我们要费力地逐一取出
	 * 并拒绝它们）。
	 * 因此我们改用 SnapshotNonVacuumable。它会接受
	 * 最近死亡与未提交的行，以及正常可见的行。但另
	 * 一方面它会拒绝已知死亡的行，从而在极端值已被
	 * 删除时不会给出伪造答案（除非删除发生在很近的
	 * 过去）；这正是不用 SnapshotAny 的动机。
	 *
	 * 这里的一个关键点是，以
	 * GlobalVisTestFor(heapRel) 为视界的
	 * SnapshotNonVacuumable，会给出索引扫描用来判定
	 * 索引条目可杀死（killable）之条件的反条件（见
	 * heap_hot_search_buffer()）。因此，若快照拒绝某
	 * 元组（更准确地说，是一条 HOT 链的全部元组），
	 * 而我们必须继续越过它，就知道
	 * 索引扫描会把该索引条目标记为已杀死。这意味着
	 * 下一次 get_actual_variable_endpoint() 调用将不必
	 * 重新考虑该索引条目。
	 *
	 * 但使用 SnapshotNonVacuumable 自身也有危险。在
	 * 新建索引中，某些索引条目可能指向损坏的 HOT 链，
	 * 链中并非所有元组版本都与索引条目数据匹配。
	 * 存活的元组版本当然与索引匹配，但
	 * SnapshotNonVacuumable 可能接受不匹配的最近死亡
	 * 元组版本。因此，如果我们从选定的堆元组取数据，
	 * 就可能得到远离索引极端值的伪造答案，甚至可能是
	 * NULL。我们之所以能避开这一危险，是因为我们
	 * 取的是索引条目而非堆。
	 *
	 * 尽管如此小心，仍有一些情形可能在索引末尾附近
	 * 发现许多不可见元组。我们不想在此耗费大量时间，
	 * 所以一旦读取的堆页面过多就放弃。当我们因此
	 * 失败时，调用者最终会使用 pg_statistic 中记录的
	 * 极端值。
	 */
	InitNonVacuumableSnapshot(SnapshotNonVacuumable,
							  GlobalVisTestFor(heapRel));

	index_scan = index_beginscan(heapRel, indexRel,
								 &SnapshotNonVacuumable,
								 1, 0);
	/* 设置为 index-only 扫描 */
	index_scan->xs_want_itup = true;
	index_rescan(index_scan, scankeys, 1, NULL, 0);

	/* 按指定方向取第一个/下一个元组 */
	while ((tid = index_getnext_tid(index_scan, indexscandir)) != NULL)
	{
		BlockNumber block = ItemPointerGetBlockNumber(tid);

		if (!VM_ALL_VISIBLE(heapRel,
							block,
							&vmbuffer))
		{
			/* 糟糕，必须访问堆来检查可见性 */
			if (!index_fetch_heap(index_scan, tableslot))
			{
				/*
				 * 此索引条目无可见元组，需前进到下一
				 * 个条目。在此之前，统计堆页取用次数，
				 * 若过多就放弃。
				 *
				 * 若与前一元组同一堆页，则不计页面取用。
				 * 这偏保守，因为其他最近访问过的页面多半
				 * 也还在缓冲区中；但对这种启发式来说这已
				 * 足够好了。
				 */
#define VISITED_PAGES_LIMIT 100

				if (block != last_heap_block)
				{
					last_heap_block = block;
					n_visited_heap_pages++;
					if (n_visited_heap_pages > VISITED_PAGES_LIMIT)
						break;
				}

				continue;		/* 无可见元组，试下一条目 */
			}

			/* 其实我们并不需要堆元组 */
			ExecClearTuple(tableslot);

			/*
			 * 无需关心 HOT 链中是否有多于一个可见元组；
			 * 只要有一个可见就足够了。
			 */
		}

		/*
		 * 我们预期 btree 会以 IndexTuple 而非 HeapTuple 形式
		 * 返回数据，且非有损。
		 */
		if (!index_scan->xs_itup)
			elog(ERROR, "no data returned for index-only scan");
		if (index_scan->xs_recheck)
			elog(ERROR, "unexpected recheck indication from btree");

		/* 可以分解索引元组了 */
		index_deform_tuple(index_scan->xs_itup,
						   index_scan->xs_itupdesc,
						   values, isnull);

		/* 本不该得到 NULL，但要小心 */
		if (isnull[0])
			elog(ERROR, "found unexpected null value in index \"%s\"",
				 RelationGetRelationName(indexRel));

		/* 把索引列值拷贝到调用者的上下文 */
		oldcontext = MemoryContextSwitchTo(outercontext);
		*endpointDatum = datumCopy(values[0], typByVal, typLen);
		MemoryContextSwitchTo(oldcontext);
		have_data = true;
		break;
	}

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	index_endscan(index_scan);

	return have_data;
}

/*
 * find_join_input_rel
 *		查找连接的输入关系。
 *
 * 我们假定输入关系的 RelOptInfo 必已被构建出来。
 *
 */
static RelOptInfo *
find_join_input_rel(PlannerInfo *root, Relids relids)
{
	RelOptInfo *rel = NULL;

	switch (bms_membership(relids))
	{
		case BMS_EMPTY_SET:
			/* 不应发生 */
			break;
		case BMS_SINGLETON:
			rel = find_base_rel(root, bms_singleton_member(relids));
			break;
		case BMS_MULTIPLE:
			rel = find_join_rel(root, relids);
			break;
	}

	if (rel == NULL)
		elog(ERROR, "could not find RelOptInfo for given relids");

	return rel;
}


/*-------------------------------------------------------------------------
 *
 * 索引代价估算函数
 *
 *-------------------------------------------------------------------------
 */

/*
 * 从 IndexClause 列表提取实际索引条件（RestrictInfos 形式）
 */
List *
get_quals_from_indexclauses(List *indexclauses)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

			result = lappend(result, rinfo);
		}
	}
	return result;
}

/*
 * 计算索引条件表达式列表中比较操作数的总评估
 * 代价。由于我们知道这些在每次扫描中只求值一次，
 * 因此无需区分启动代价与每行代价。
 *
 * 这既可用于 get_quals_from_indexclauses() 的结果，
 * 也可直接用于 indexorderbys 列表。两种情形下我们
 * 都期望索引键表达式位于二元子句的左侧。
 */
Cost
index_other_operands_eval_cost(PlannerInfo *root, List *indexquals)
{
	Cost		qual_arg_cost = 0;
	ListCell   *lc;

	foreach(lc, indexquals)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		Node	   *other_operand;
		QualCost	index_qual_cost;

		/*
		 * 索引条件带有 RestrictInfos，indexorderbys 则没有。
		 * 有则看穿 RestrictInfo。
		 */
		if (IsA(clause, RestrictInfo))
			clause = ((RestrictInfo *) clause)->clause;

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			other_operand = (Node *) lsecond(op->args);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;

			other_operand = (Node *) rc->rargs;
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			other_operand = (Node *) lsecond(saop->args);
		}
		else if (IsA(clause, NullTest))
		{
			other_operand = NULL;
		}
		else
		{
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
			other_operand = NULL;	/* 让编译器安静 */
		}

		cost_qual_eval_node(&index_qual_cost, other_operand, root);
		qual_arg_cost += index_qual_cost.startup + index_qual_cost.per_tuple;
	}
	return qual_arg_cost;
}

void
genericcostestimate(PlannerInfo *root,
					IndexPath *path,
					double loop_count,
					GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *indexOrderBys = path->indexorderbys;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation;
	double		numIndexPages;
	double		numIndexTuples;
	double		spc_random_page_cost;
	double		num_sa_scans;
	double		num_outer_scans;
	double		num_scans;
	double		qual_op_cost;
	double		qual_arg_cost;
	List	   *selectivityQuals;
	ListCell   *l;

	/*
	 * 若索引是部分的，就把索引谓词与显式给出的索引
	 * 条件相与，以得到索引选择率更准确的看法。
	 *
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/*
	 * 检查 ScalarArrayOpExpr 型索引条件，并估计将要
	 * 执行的索引扫描次数。
	 */
	num_sa_scans = 1;
	foreach(l, indexQuals)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (IsA(rinfo->clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
			int			alength = estimate_array_length(lsecond(saop->args));

			if (alength > 1)
				num_sa_scans *= alength;
		}
	}

	/* 估计将被访问的主表元组比例 */
	indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											  index->rel->relid,
											  JOIN_INNER,
											  NULL);

	/*
	 * 若调用者没有给出估算，就估计将被访问的索引
	 * 元组数。我们用这种看起来颇为特别的方式来
	 * 为部分索引得到正确答案。
	 */
	numIndexTuples = costs->numIndexTuples;
	if (numIndexTuples <= 0.0)
	{
		numIndexTuples = indexSelectivity * index->rel->tuples;

		/*
		 * 上面的计算统计的是 ScalarArrayOpExpr 节点引发的
		 * 所有扫描中访问的元组总数。我们要考虑的是
		 * 平均每次索引扫描的数目，因此要作调整。这也是
		 * 取整的好地方。（若调用者提供了元组估算，
		 * 则由其负责处理这些考虑。）
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * 无论如何，元组数都可用索引大小来限定。另外，
	 * 即使 indexSelectivity 极小也总估计至少触碰
	 * 一个元组。
	 */
	if (numIndexTuples > index->tuples)
		numIndexTuples = index->tuples;
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;

	/*
	 * 估计将被取回的索引页面数。
	 *
	 * 我们用简单的方法：取总索引页数的一个按比例
	 * 份额。实际上这只统计叶子页面，而不计入任何
	 * 索引元页面或上层树层之类的开销。
	 *
	 * 实践中对上层索引层的访问往往几乎免费，因为
	 * 负载下它们常驻缓存；此外，所涉代价高度依赖
	 * 索引类型。因此我们在此忽略这类代价，需要时
	 * 由调用者自行加上适当的收费。
	 */
	if (index->pages > 1 && index->tuples > 1)
		numIndexPages = ceil(numIndexTuples * index->pages / index->tuples);
	else
		numIndexPages = 1.0;

	/* 取索引所在表空间的估计页面代价 */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * 现在计算磁盘访问代价。
	 *
	 * 上面的计算都是每次索引扫描的。然而，如果我们
	 * 在 nestloop 内侧扫描中，扫描预期会为外侧关系
	 * 的每一行重复（用不同的搜索键）。同样，
	 * ScalarArrayOpExpr 条件会导致多次索引扫描。这
	 * 就为缓存效应创造了机会，可减少所需的磁盘页
	 * 面取用次数。我们想估计存在缓存时平均每次
	 * 扫描的 I/O 代价。
	 *
	 * 我们用 Mackert-Lohman 公式（详见 costsize.c）来
	 * 估计发生的页面取用总数。虽然这不是它的设计
	 * 目的，但看起来仍是个合理的模型。注意我们数
	 * 的现在是页面而非元组，所以取 N = T = 索引大小，
	 * 如同每页有一个"元组"。
	 */
	num_outer_scans = loop_count;
	num_scans = num_sa_scans * num_outer_scans;

	if (num_scans > 1)
	{
		double		pages_fetched;

		/* 无视缓存效应的总页面取用数 */
		pages_fetched = numIndexPages * num_scans;

		/* 用 Mackert 和 Lohman 公式调整缓存效应 */
		pages_fetched = index_pages_fetched(pages_fetched,
											index->pages,
											(double) index->pages,
											root);

		/*
		 * 现在计算总磁盘访问代价，然后为每次外侧扫描
		 * 报告一个按比例的份额。（对 ScalarArrayOpExpr
		 * 不分摊，因其为扫描内部。）
		 */
		indexTotalCost = (pages_fetched * spc_random_page_cost)
			/ num_outer_scans;
	}
	else
	{
		/*
		 * 对单次索引扫描，每触碰页收 spc_random_page_cost
		 *
		 */
		indexTotalCost = numIndexPages * spc_random_page_cost;
	}

	/*
	 * CPU 代价：索引条件中的任何复杂表达式都需要在
	 * 扫描开始时求值一次，把它们化约为传给索引 AM 的
	 * 运行时键（见 nodeIndexscan.c）。我们把每元组 CPU
	 * 代价建模为 cpu_index_tuple_cost 加上每个
	 * 索引条件算子的 cpu_operator_cost。因为我们
	 * 已有的 numIndexTuples 是每次扫描的数目，所以要
	 * 乘以 num_sa_scans。类似地，为索引 ORDER BY 表达式
	 * 加入代价。
	 *
	 * 注意：这忽略了复查有损算子的可能代价。
	 * 不过，考虑到此处所有其他的不精确性，检测是否
	 * 需要复查本身似乎并不值得……
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals) +
		index_other_operands_eval_cost(root, indexOrderBys);
	qual_op_cost = cpu_operator_cost *
		(list_length(indexQuals) + list_length(indexOrderBys));

	indexStartupCost = qual_arg_cost;
	indexTotalCost += qual_arg_cost;
	indexTotalCost += numIndexTuples * num_sa_scans * (cpu_index_tuple_cost + qual_op_cost);

	/*
	 * 索引相关性的通用假设：没有任何相关性。
	 */
	indexCorrelation = 0.0;

	/*
	 * 全部返回给调用者。
	 */
	costs->indexStartupCost = indexStartupCost;
	costs->indexTotalCost = indexTotalCost;
	costs->indexSelectivity = indexSelectivity;
	costs->indexCorrelation = indexCorrelation;
	costs->numIndexPages = numIndexPages;
	costs->numIndexTuples = numIndexTuples;
	costs->spc_random_page_cost = spc_random_page_cost;
	costs->num_sa_scans = num_sa_scans;
}

/*
 * 若索引是部分的，把其谓词加入给定的条件列表。
 *
 * 把索引谓词与显式给出的索引条件相与，能得到对
 * 索引选择率更准确的认识。然而我们必须小心，
 * 不要插入冗余子句，因为 clauselist_selectivity() 很
 * 容易被骗着算出过低的选择率估算。我们的做法
 * 是只加入无法被证明由给定索引条件所蕴含的谓词
 * 子句。这能成功处理诸如条件 "x = 42" 配合部分索引
 * "WHERE x >= 40 AND x < 50" 使用的情形。还有很多
 * 其他情形我们检测不出冗余，导致过低的选择率
 * 估算，这会使系统偏向尽可能使用部分索引。不过
 * 这并不一定是坏事。
 *
 * 注意 indexQuals 包含 RestrictInfo 节点而 indpred
 * 不包含，所以输出列表将是混合的。这对
 * predicate_implied_by() 与 clauselist_selectivity() 都
 * 没问题，但传给其他东西可能有问题。
 */
List *
add_predicate_to_index_quals(IndexOptInfo *index, List *indexQuals)
{
	List	   *predExtraQuals = NIL;
	ListCell   *lc;

	if (index->indpred == NIL)
		return indexQuals;

	foreach(lc, index->indpred)
	{
		Node	   *predQual = (Node *) lfirst(lc);
		List	   *oneQual = list_make1(predQual);

		if (!predicate_implied_by(oneQual, indexQuals, false))
			predExtraQuals = list_concat(predExtraQuals, oneQual);
	}
	return list_concat(predExtraQuals, indexQuals);
}


void
btcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Oid			relid;
	AttrNumber	colnum;
	VariableStatData vardata = {0};
	double		numIndexTuples;
	Cost		descentCost;
	List	   *indexBoundQuals;
	int			indexcol;
	bool		eqQualHere;
	bool		found_saop;
	bool		found_is_null_op;
	double		num_sa_scans;
	ListCell   *lc;

	/*
	 * 对 btree 扫描而言，只有前导 "=" 限制条件与紧邻
	 * 不等式限制条件才计入索引选择率（即决定扫描
	 * 起点与终点的"边界限制条件"）。额外的限制条件可以
	 * 减少对堆的访问，把它们计入 indexSelectivity 没问题，
	 * 但它们不应计入 numIndexTuples 的估算。所以我们必须
	 * 检查给定的 indexquals，找出哪些算作边界限制条件。
	 * 我们依赖于它们按索引列顺序给出这一知识。
	 *
	 *
	 * 对 RowCompareExpr 只看第一列，同 rowcomparesel()。
	 *
	 *
	 * 若限制条件中存在 ScalarArrayOpExpr，实际会执行
	 * N 次索引扫描而非一次，但 ScalarArrayOpExpr 的算子
	 * 仍可认为与平常一样起作用。
	 */
	indexBoundQuals = NIL;
	indexcol = 0;
	eqQualHere = false;
	found_saop = false;
	found_is_null_op = false;
	num_sa_scans = 1;
	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (indexcol != iclause->indexcol)
		{
			/* 新列限制条件的开始 */
			if (!eqQualHere)
				break;			/* indexcol 无 '=' 条件 */
			eqQualHere = false;
			indexcol++;
			if (indexcol != iclause->indexcol)
				break;			/* indexcol 无限制条件 */
		}

		/* 检查此索引子句相关的每个索引限制条件 */
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;
			Oid			clause_op = InvalidOid;
			int			op_strategy;

			if (IsA(clause, OpExpr))
			{
				OpExpr	   *op = (OpExpr *) clause;

				clause_op = op->opno;
			}
			else if (IsA(clause, RowCompareExpr))
			{
				RowCompareExpr *rc = (RowCompareExpr *) clause;

				clause_op = linitial_oid(rc->opnos);
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
				Node	   *other_operand = (Node *) lsecond(saop->args);
				int			alength = estimate_array_length(other_operand);

				clause_op = saop->opno;
				found_saop = true;
				/* 只统计由 indexBoundQuals 引发的 SA 扫描数 */
				if (alength > 1)
					num_sa_scans *= alength;
			}
			else if (IsA(clause, NullTest))
			{
				NullTest   *nt = (NullTest *) clause;

				if (nt->nulltesttype == IS_NULL)
				{
					found_is_null_op = true;
					/* 就选择率而言 IS NULL 与 = 相同 */
					eqQualHere = true;
				}
			}
			else
				elog(ERROR, "unsupported indexqual type: %d",
					 (int) nodeTag(clause));

			/* 检查是否为相等算子 */
			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   index->opfamily[indexcol]);
				Assert(op_strategy != 0);	/* 非 opfamily 成员？ */
				if (op_strategy == BTEqualStrategyNumber)
					eqQualHere = true;
			}

			indexBoundQuals = lappend(indexBoundQuals, rinfo);
		}
	}

	/*
	 * 若索引唯一且每列都找到了 '=' 子句，我们可以直接
	 * 假定 numIndexTuples = 1，并跳过昂贵的
	 * clauselist_selectivity 计算。不过 ScalarArrayOp 或
	 * NullTest 会让该理论失效，即使它设置了 eqQualHere。
	 */
	if (index->unique &&
		indexcol == index->nkeycolumns - 1 &&
		eqQualHere &&
		!found_saop &&
		!found_is_null_op)
		numIndexTuples = 1.0;
	else
	{
		List	   *selectivityQuals;
		Selectivity btreeSelectivity;

		/*
		 * 若索引是部分索引，则将索引谓词与
		 * 索引边界限制条件相与，以更准确地了解
		 * 边界条件所覆盖的行数。
		 */
		selectivityQuals = add_predicate_to_index_quals(index, indexBoundQuals);

		btreeSelectivity = clauselist_selectivity(root, selectivityQuals,
												  index->rel->relid,
												  JOIN_INNER,
												  NULL);
		numIndexTuples = btreeSelectivity * index->rel->tuples;

		/*
		 * 与 genericcostestimate() 一样，我们必须调整
		 * indexBoundQuals 中的 ScalarArrayOpExpr 限制条件，然后
		 * 取整。
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * 现在做通用索引代价估算。
	 */
	costs.numIndexTuples = numIndexTuples;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * 添加一项 CPU 代价，表示初始 btree 下降的代价。
	 * 触及上层 btree 层不收任何 I/O 代价，因为它们往往
	 * 留在缓存中，但下降 N 个叶元组的 btree 仍需做
	 * 约 log2(N) 次比较。每次比较收一个
	 * cpu_operator_cost。
	 *
	 * 若有 ScalarArrayOpExpr，则每 SA 扫描收一次此费用。
	 * 就整体计划而言，之后的那些不是启动代价，
	 * 所以只把它们加进 "total" 代价。
	 */
	if (index->tuples > 1)		/* 避免计算 log(0) */
	{
		descentCost = ceil(log(index->tuples) / log(2.0)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * 尽管我们不因触及上层 btree 页而收取 I/O 代价，
	 * 但每下降一页收取一些 CPU 代价仍是合理的。
	 * 此外，若完全没有任何此类收费，膨胀的索引将
	 * 表现为与未膨胀的索引具有相同的搜索代价，至少
	 * 在预计只访问单个叶子页的情形下是如此。此代价
	 * 被略随意地定为每触及一页 50 倍 cpu_operator_cost。
	 * 此类页数是 btree 树高加一（即叶子页也收费）。
	 * 如上所述，每次 SA 扫描收一次。
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	/*
	 * 若能从 pg_statistic 得到第一列排序相关性 C 的估算，
	 * 则对单列索引把索引相关性估计为 C，
	 * 对多列则估计为 C * 0.75。（这里的想法是
	 * 多列会稀释第一列排序的重要性，但不会
	 * 完全抵消它。8.0 之前我们把相关性除以
	 * 列数，但这似乎太强了。）
	 */
	if (index->indexkeys[0] != 0)
	{
		/* 简单变量 --- 查看底层表的统计信息 */
		RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);

		Assert(rte->rtekind == RTE_RELATION);
		relid = rte->relid;
		Assert(relid != InvalidOid);
		colnum = index->indexkeys[0];

		if (get_relation_stats_hook &&
			(*get_relation_stats_hook) (root, rte, colnum, &vardata))
		{
			/*
			 * 钩子接管了获取统计元组的控制权。若它确实
			 * 提供了元组，那它最好也给了 freefunc。
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(rte->inh));
			vardata.freefunc = ReleaseSysCache;
		}
	}
	else
	{
		/* 表达式 --- 也许索引本身就有统计信息 */
		relid = index->indexoid;
		colnum = 1;

		if (get_index_stats_hook &&
			(*get_index_stats_hook) (root, relid, colnum, &vardata))
		{
			/*
			 * 钩子接管了获取统计元组的控制权。若它确实
			 * 提供了元组，那它最好也给了 freefunc。
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(false));
			vardata.freefunc = ReleaseSysCache;
		}
	}

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Oid			sortop;
		AttStatsSlot sslot;

		sortop = get_opfamily_member(index->opfamily[0],
									 index->opcintype[0],
									 index->opcintype[0],
									 BTLessStrategyNumber);
		if (OidIsValid(sortop) &&
			get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_CORRELATION, sortop,
							 ATTSTATSSLOT_NUMBERS))
		{
			double		varCorrelation;

			Assert(sslot.nnumbers == 1);
			varCorrelation = sslot.numbers[0];

			if (index->reverse_sort[0])
				varCorrelation = -varCorrelation;

			if (index->nkeycolumns > 1)
				costs.indexCorrelation = varCorrelation * 0.75;
			else
				costs.indexCorrelation = varCorrelation;

			free_attstatsslot(&sslot);
		}
	}

	ReleaseVariableStats(vardata);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
hashcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * 哈希索引没有所谓的下降代价，因为索引 AM 在计算
	 * 出哈希值后可以直接到达目标桶。不过，这里
	 * 还有几种其他哈希特有的代价是我们原则上可以
	 * 加上的：
	 *
	 * 理想情况下，我们应为目标桶中的每一页收取
	 * spc_random_page_cost，而不只是 genericcostestimate
	 * 认为我们会访问的那 numIndexPages 页。但在大多数
	 * 情形下我们并不知道会是哪个桶。考虑平均桶大小
	 * 无意义，因为哈希 AM 确保它总是一页。
	 *
	 * 同样，如果我们知道桶中有多少索引元组，也可以
	 * 考虑为桶中每个索引元组收取一些 CPU 费用。但
	 * 每元组代价只是一次哈希值比较，而非通用的
	 * 依赖数据类型的比较，所以任何这类收费都应
	 * 远小于 cpu_operator_cost，这可能不值得操心。
	 *
	 * 更大的问题是偶然的哈希值碰撞会导致对堆的
	 * 无效探测。目前我们不去建模这个代价，理由是
	 * 它很罕见，但也许它没有罕见到那种程度。
	 * （对此的任何修复都应考虑有损算子的通用问题；
	 * 它并不完全特属于哈希。）
	 */

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
gistcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * 我们对索引下降代价的建模与 btree 类似，但要做
	 * 到这一点，我们首先需要树高的概念。我们有些
	 * 随意地假定扇出为 100，即树高至多为
	 * log100(index->pages)
	 *
	 * 虽然这个计算并不真的昂贵到需要缓存，
	 * 但我们不妨用 index->tree_height 来缓存它。
	 */
	if (index->tree_height < 0) /* 未知 */
	{
		if (index->pages > 1)	/* 避免计算 log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * 添加一项 CPU 代价成分，表示初始下降的代价。我们
	 * 这里只用 log(N) 而非 log2(N)，因为分支因子
	 * 未必是二。至于 btree，每次 SA 扫描收一次。
	 */
	if (index->tuples > 1)		/* 避免计算 log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * 同样添加每页费用，计算方式与 btree 相同。
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
spgcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * 我们对索引下降代价的建模与 btree 类似，但要做
	 * 到这一点，我们首先需要树高的概念。我们有些
	 * 随意地假定扇出为 100，即树高至多为
	 * log100(index->pages)
	 *
	 * 虽然这个计算并不真的昂贵到需要缓存，
	 * 但我们不妨用 index->tree_height 来缓存它。
	 */
	if (index->tree_height < 0) /* 未知 */
	{
		if (index->pages > 1)	/* 避免计算 log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * 添加一项 CPU 代价成分，表示初始下降的代价。我们
	 * 这里只用 log(N) 而非 log2(N)，因为分支因子
	 * 未必是二。至于 btree，每次 SA 扫描收一次。
	 */
	if (index->tuples > 1)		/* 避免计算 log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * 同样添加每页费用，计算方式与 btree 相同。
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}


/*
 * gincostestimate 的支撑例程
 */

typedef struct
{
	bool		attHasFullScan[INDEX_MAX_KEYS];
	bool		attHasNormalScan[INDEX_MAX_KEYS];
	double		partialEntries;
	double		exactEntries;
	double		searchEntries;
	double		arrayScans;
} GinQualCounts;

/*
 * 给定 GIN 查询，估算所需搜索的索引条目数，
 * 并适当地递增 *counts 中的计数。
 * 若查询不可满足，则返回 false。
 */
static bool
gincost_pattern(IndexOptInfo *index, int indexcol,
				Oid clause_op, Datum query,
				GinQualCounts *counts)
{
	FmgrInfo	flinfo;
	Oid			extractProcOid;
	Oid			collation;
	int			strategy_op;
	Oid			lefttype,
				righttype;
	int32		nentries = 0;
	bool	   *partial_matches = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nullFlags = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	int32		i;

	Assert(indexcol < index->nkeycolumns);

	/*
	 * 取得算子在索引 opfamily 内的策略号与声明的输入
	 * 数据类型。（我们并不需要后者，但要用
	 * get_op_opfamily_properties，因为它找不到匹配的
	 * pg_amop 条目时会抛错）
	 */
	get_op_opfamily_properties(clause_op, index->opfamily[indexcol], false,
							   &strategy_op, &lefttype, &righttype);

	/*
	 * GIN 总是使用"默认"支撑函数，即 lefttype
	 * == righttype == 操作符类的 opcintype (见
	 * relcache.c IndexSupportInitialize)。
	 */
	extractProcOid = get_opfamily_proc(index->opfamily[indexcol],
									   index->opcintype[indexcol],
									   index->opcintype[indexcol],
									   GIN_EXTRACTQUERY_PROC);

	if (!OidIsValid(extractProcOid))
	{
		/* 不应发生；抛同 index_getprocinfo 的错误 */
		elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
			 GIN_EXTRACTQUERY_PROC, indexcol + 1,
			 get_rel_name(index->indexoid));
	}

	/*
	 * 选择传给 extractProc 的排序规则（应同 initGinState）。
	 */
	if (OidIsValid(index->indexcollations[indexcol]))
		collation = index->indexcollations[indexcol];
	else
		collation = DEFAULT_COLLATION_OID;

	fmgr_info(extractProcOid, &flinfo);

	set_fn_opclass_options(&flinfo, index->opclassoptions[indexcol]);

	FunctionCall7Coll(&flinfo,
					  collation,
					  query,
					  PointerGetDatum(&nentries),
					  UInt16GetDatum(strategy_op),
					  PointerGetDatum(&partial_matches),
					  PointerGetDatum(&extra_data),
					  PointerGetDatum(&nullFlags),
					  PointerGetDatum(&searchMode));

	if (nentries <= 0 && searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		/* 不可能有匹配 */
		return false;
	}

	for (i = 0; i < nentries; i++)
	{
		/*
		 * 对于部分匹配，我们没有任何可用来估算索引中
		 * 匹配条目数的信息，只好估算为 100。
		 */
		if (partial_matches && partial_matches[i])
			counts->partialEntries += 100;
		else
			counts->exactEntries++;

		counts->searchEntries++;
	}

	if (searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		counts->attHasNormalScan[indexcol] = true;
	}
	else if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
	{
		/* 把 "include empty" 当作精确匹配项 */
		counts->attHasNormalScan[indexcol] = true;
		counts->exactEntries++;
		counts->searchEntries++;
	}
	else
	{
		/* GIN_SEARCH_MODE_ALL */
		counts->attHasFullScan[indexcol] = true;
	}

	return true;
}

/*
 * 给定 GIN 索引子句，估算所需搜索的索引条目数，
 * 并适当地递增 *counts 中的计数。
 * 若查询不可满足，则返回 false。
 */
static bool
gincost_opexpr(PlannerInfo *root,
			   IndexOptInfo *index,
			   int indexcol,
			   OpExpr *clause,
			   GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *operand = (Node *) lsecond(clause->args);

	/* 积极地化简为常量，并看穿 relabeling */
	operand = estimate_expression_value(root, operand);

	if (IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	/*
	 * 对未知操作数无法调用 extractQuery 方法。所以
	 * 除非操作数是 Const，否则做不了什么；只能假定
	 * 运行时该操作数提供一个普通搜索条目。
	 */
	if (!IsA(operand, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		return true;
	}

	/* 若 Const 为 null，不可能有匹配 */
	if (((Const *) operand)->constisnull)
		return false;

	/* 否则，应用 extractQuery 并取得实际的条目计数 */
	return gincost_pattern(index, indexcol, clause_op,
						   ((Const *) operand)->constvalue,
						   counts);
}

/*
 * 给定 GIN 索引子句，估算所需搜索的索引条目数，
 * 并适当地递增 *counts 中的计数。
 * 若查询不可满足，则返回 false。
 *
 * ScalarArrayOpExpr 在运行时会产生 N 次独立的索引扫描，
 * 每一次都涉及 RHS 数组中的一个值，加上所有
 * 非数组限制条件（若有）。为了建模这一点，我们把
 * 各计数在 RHS 元素间取平均，并把平均值加进
 * *counts（对应每次索引扫描的代价）。我们还把
 * counts->arrayScans 乘以 N，让 gincostestimate 相应放大。
 */
static bool
gincost_scalararrayopexpr(PlannerInfo *root,
						  IndexOptInfo *index,
						  int indexcol,
						  ScalarArrayOpExpr *clause,
						  double numIndexEntries,
						  GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *rightop = (Node *) lsecond(clause->args);
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			numElems;
	Datum	   *elemValues;
	bool	   *elemNulls;
	GinQualCounts arraycounts;
	int			numPossible = 0;
	int			i;

	Assert(clause->useOr);

	/* 积极地化简为常量，并看穿 relabeling */
	rightop = estimate_expression_value(root, rightop);

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	/*
	 * 对未知操作数无法调用 extractQuery 方法。所以
	 * 除非操作数是 Const，否则做不了什么；只能假定
	 * 运行时每个数组元素会提供一个普通搜索条目，并
	 * 回退到对数组元素数目的一个可能很差的估算。
	 */
	if (!IsA(rightop, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		counts->arrayScans *= estimate_array_length(rightop);
		return true;
	}

	/* 若 Const 为 null，不可能有匹配 */
	if (((Const *) rightop)->constisnull)
		return false;

	/* 否则，提取数组元素并遍历它们 */
	arrayval = DatumGetArrayTypeP(((Const *) rightop)->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &elemValues, &elemNulls, &numElems);

	memset(&arraycounts, 0, sizeof(arraycounts));

	for (i = 0; i < numElems; i++)
	{
		GinQualCounts elemcounts;

		/* NULL 无法匹配，忽略之，如同执行器那样 */
		if (elemNulls[i])
			continue;

		/* 否则，应用 extractQuery 并取得实际的条目计数 */
		memset(&elemcounts, 0, sizeof(elemcounts));

		if (gincost_pattern(index, indexcol, clause_op, elemValues[i],
							&elemcounts))
		{
			/* 我们忽略数组中不可满足模式的元素 */
			numPossible++;

			if (elemcounts.attHasFullScan[indexcol] &&
				!elemcounts.attHasNormalScan[indexcol])
			{
				/*
				 * 将需要完整索引扫描。我们把它当作
				 * 索引中的每个键都已被列入查询来对待；
				 * 这合理吗？
				 */
				elemcounts.partialEntries = 0;
				elemcounts.exactEntries = numIndexEntries;
				elemcounts.searchEntries = numIndexEntries;
			}
			arraycounts.partialEntries += elemcounts.partialEntries;
			arraycounts.exactEntries += elemcounts.exactEntries;
			arraycounts.searchEntries += elemcounts.searchEntries;
		}
	}

	if (numPossible == 0)
	{
		/* 数组中没有可满足的模式 */
		return false;
	}

	/*
	 * 现在把平均值加进全局计数。这会给我们对
	 * 每次索引扫描所搜索条目平均数的估算，包括
	 * 数组与非数组限制条件两者的贡献。
	 */
	counts->partialEntries += arraycounts.partialEntries / numPossible;
	counts->exactEntries += arraycounts.exactEntries / numPossible;
	counts->searchEntries += arraycounts.searchEntries / numPossible;

	counts->arrayScans *= numPossible;

	return true;
}

/*
 * GIN 的搜索行为与其他索引类型完全不同
 */
void
gincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *selectivityQuals;
	double		numPages = index->pages,
				numTuples = index->tuples;
	double		numEntryPages,
				numDataPages,
				numPendingPages,
				numEntries;
	GinQualCounts counts;
	bool		matchPossible;
	bool		fullIndexScan;
	double		partialScale;
	double		entryPagesFetched,
				dataPagesFetched,
				dataPagesFetchedBySel;
	double		qual_op_cost,
				qual_arg_cost,
				spc_random_page_cost,
				outer_scans;
	Cost		descentCost;
	Relation	indexRel;
	GinStatsData ginStats;
	ListCell   *lc;
	int			i;

	/*
	 * 如有可能，从元页面取得统计信息。否则把
	 * ginStats 置零，我们在下文应对。
	 */
	if (!index->hypothetical)
	{
		/* 锁应当已在 plancat.c 中取得 */
		indexRel = index_open(index->indexoid, NoLock);
		ginGetStats(indexRel, &ginStats);
		index_close(indexRel, NoLock);
	}
	else
	{
		memset(&ginStats, 0, sizeof(ginStats));
	}

	/*
	 * 假如我们得到的统计信息有效（非零），
	 * nPendingPages 是可信赖的，但其他字段是上次
	 * VACUUM 时的数据。我们可以按之后的增长把它们
	 * 放大来加以考虑，但这种方法只能走到这一步；
	 * 在最坏的情形下，统计信息可能对应一个完全
	 * 空的索引，放大它们将产生非常离谱的数字。我们
	 * 有些随意地把放大门槛设在 4 倍增长；若增长超过
	 * 此值，就回退为按索引尺寸估算（假定其准确）。但
	 * nPendingPages 不明显荒谬（未超索引大小）即可信赖。
	 */
	if (ginStats.nPendingPages < numPages)
		numPendingPages = ginStats.nPendingPages;
	else
		numPendingPages = 0;

	if (numPages > 0 && ginStats.nTotalPages <= numPages &&
		ginStats.nTotalPages > numPages / 4 &&
		ginStats.nEntryPages > 0 && ginStats.nEntries > 0)
	{
		/*
		 * 好，统计信息看起来够接近正常而可信赖。
		 * 但我们仍需把它们按 numPages / nTotalPages 的比率
		 * 放大，以顾及 VACUUM 后的增长。
		 */
		double		scale = numPages / ginStats.nTotalPages;

		numEntryPages = ceil(ginStats.nEntryPages * scale);
		numDataPages = ceil(ginStats.nDataPages * scale);
		numEntries = ceil(ginStats.nEntries * scale);
		/* 确保没有向上取整太多 */
		numEntryPages = Min(numEntryPages, numPages - numPendingPages);
		numDataPages = Min(numDataPages,
						   numPages - numPendingPages - numEntryPages);
	}
	else
	{
		/*
		 * 我们可能走到这里，因为这是一个假想索引，
		 * 或是 9.1 之前创建、升级后从未 VACUUM 过的索引
		 * （其统计信息读数为零），或只是因为自
		 * 上次 VACUUM 后增长太多，不宜再放大。
		 *
		 * 基于索引页数虚构一些说得通的内部统计信息
		 * （并钳制到至少 10 页，以防万一）。我们
		 * 估计索引的 90% 是条目页，其余是数据页。
		 * 估计每个条目页 100 个条目；这相当粗糙，
		 * 因为它取决于键的大小，但比试图预测每个
		 * 堆元组的条目数更稳健。
		 */
		numPages = Max(numPages, 10);
		numEntryPages = floor((numPages - numPendingPages) * 0.90);
		numDataPages = numPages - numPendingPages - numEntryPages;
		numEntries = floor(numEntryPages * 100);
	}

	/* 在空索引中 numEntries 可能为零。避免除零 */
	if (numEntries < 1)
		numEntries = 1;

	/*
	 * 若索引是部分索引，则把索引谓词与索引边界限制
	 * 条件相与，以更准确地了解边界条件所覆盖的
	 * 行数。
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/* 估计将被访问的主表元组比例 */
	*indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											   index->rel->relid,
											   JOIN_INNER,
											   NULL);

	/* 取索引所在表空间的估计页面代价 */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * 索引相关性的通用假设：没有任何相关性。
	 */
	*indexCorrelation = 0.0;

	/*
	 * 检查限制条件以估算搜索条目数与部分匹配数
	 */
	memset(&counts, 0, sizeof(counts));
	counts.arrayScans = 1;
	matchPossible = true;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;

			if (IsA(clause, OpExpr))
			{
				matchPossible = gincost_opexpr(root,
											   index,
											   iclause->indexcol,
											   (OpExpr *) clause,
											   &counts);
				if (!matchPossible)
					break;
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				matchPossible = gincost_scalararrayopexpr(root,
														  index,
														  iclause->indexcol,
														  (ScalarArrayOpExpr *) clause,
														  numEntries,
														  &counts);
				if (!matchPossible)
					break;
			}
			else
			{
				/* 对 GIN 索引而言不应有其他东西 */
				elog(ERROR, "unsupported GIN indexqual type: %d",
					 (int) nodeTag(clause));
			}
		}
	}

	/* 若有可证明不可满足的限制条件则退出 */
	if (!matchPossible)
	{
		*indexStartupCost = 0;
		*indexTotalCost = 0;
		*indexSelectivity = 0;
		return;
	}

	/*
	 * 若属性有完整扫描且同时又没有普通扫描，
	 * 那么我们将不得不扫描该属性的所有非空条目。
	 * 目前，GIN 没有每属性统计信息。因此，
	 * 我们必须假定整个 GIN 索引都要被扫描。
	 */
	fullIndexScan = false;
	for (i = 0; i < index->nkeycolumns; i++)
	{
		if (counts.attHasFullScan[i] && !counts.attHasNormalScan[i])
		{
			fullIndexScan = true;
			break;
		}
	}

	if (fullIndexScan || indexQuals == NIL)
	{
		/*
		 * 将需要完整索引扫描。我们把它当作索引中的
		 * 每个键都已被列入查询来对待；这合理吗？
		 */
		counts.partialEntries = 0;
		counts.exactEntries = numEntries;
		counts.searchEntries = numEntries;
	}

	/* 我们会有不止一次 nestloop 扫描迭代吗？ */
	outer_scans = loop_count;

	/*
	 * 计算开始扫描的代价，首先要留意 pending 列表。
	 *
	 */
	entryPagesFetched = numPendingPages;

	/*
	 * 估算读取的条目页数。我们需要做
	 * counts.searchEntries 次搜索。按理应使用幂函数，
	 * 但叶子页上的元组通常要多得多。这里我们计入
	 * 条目树中的所有搜索，包括部分匹配算法中
	 * 的首次搜索
	 */
	entryPagesFetched += ceil(counts.searchEntries * rint(pow(numEntryPages, 0.15)));

	/*
	 * 把部分匹配算法读取的条目页数估算加进来。它是
	 * 对条目树中叶子页的一次扫描。我们在这里没有
	 * 任何有用的统计信息，所以按比例估算。由于
	 * counts.partialEntries 相当粗糙（见上文代码），
	 * 可能超过 numEntries；钳制保证合理。
	 */
	partialScale = counts.partialEntries / numEntries;
	partialScale = Min(partialScale, 1.0);

	entryPagesFetched += ceil(numEntryPages * partialScale);

	/*
	 * 部分匹配算法在实际扫描前读取所有数据页，
	 * 所以这是启动代价。同样这里没有可用统计信息，
	 * 故按比例估算。
	 */
	dataPagesFetched = ceil(numDataPages * partialScale);

	*indexStartupCost = 0;
	*indexTotalCost = 0;

	/*
	 * 加一项 CPU 代价成分表示初始条目 btree 下降代价。
	 * 触及上层 btree 层不收任何 I/O 代价，因为它们往往
	 * 留在缓存中，但下降 N 个叶元组的 btree 仍需做
	 * 约 log2(N) 次比较。每次比较收一个
	 * cpu_operator_cost。
	 *
	 * 若有 ScalarArrayOpExpr，则每 SA 扫描收一次此费用。
	 * 就整体计划而言，之后的那些不是启动代价，
	 * 所以只把它们加进 "total" 代价。
	 */
	if (numEntries > 1)			/* 避免计算 log(0) */
	{
		descentCost = ceil(log(numEntries) / log(2.0)) * cpu_operator_cost;
		*indexStartupCost += descentCost * counts.searchEntries;
		*indexTotalCost += counts.arrayScans * descentCost * counts.searchEntries;
	}

	/*
	 * 取出的每条目页加一项 CPU 代价，不循环摊销。
	 *
	 */
	*indexStartupCost += entryPagesFetched * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	*indexTotalCost += entryPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * 对取出的每个数据页添加一项 CPU 代价。这同样不
	 * 会随循环摊销。由于这些页来自部分匹配算法，
	 * 故记为启动代价。
	 */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * dataPagesFetched;

	/*
	 * 由于稍后会把启动代价加进总代价，先要把
	 * 初始 arrayscan 从中移除。
	 */
	*indexTotalCost += dataPagesFetched * (counts.arrayScans - 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * 若因 nestloop 或数组限制条件而产生多于一次扫描，
	 * 则计算缓存效应。结果按每次 nestloop 扫描分摊，但
	 * 数组限制条件因子不分摊 (genericcostestimate)。
	 */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		entryPagesFetched *= outer_scans * counts.arrayScans;
		entryPagesFetched = index_pages_fetched(entryPagesFetched,
												(BlockNumber) numEntryPages,
												numEntryPages, root);
		entryPagesFetched /= outer_scans;
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/*
	 * 用随机页代价，因逻辑上相近的页在磁盘上
	 * 可能相距很远。
	 */
	*indexStartupCost += (entryPagesFetched + dataPagesFetched) * spc_random_page_cost;

	/*
	 * 现在计算扫描期间取出的数据页数。
	 *
	 * 我们假定每个条目拥有相同数目的 item，且彼此
	 * 之间没有重叠。（XXX：tsvector 与数组操作符类
	 * 收集了单键频率统计信息；若能在这里用上更好。）
	 *
	 */
	dataPagesFetched = ceil(numDataPages * counts.exactEntries / numEntries);

	/*
	 * 如果条目之间有大量重叠，特别是其中一个条目
	 * 非常频繁，上面的计算可能严重低估。
	 * 作为一个简单的交叉检查，基于限制条件的总体
	 * 选择率计算一个下界。至少，我们必须为每个
	 * 匹配条目读取一个 item 指针。
	 *
	 * 每个 item 指针的宽度随压缩程度而变化。
	 * 我们没有这方面的统计信息，但平均每个
	 * item 约 3 字节是相当典型的。
	 */
	dataPagesFetchedBySel = ceil(*indexSelectivity *
								 (numTuples / (BLCKSZ / 3)));
	if (dataPagesFetchedBySel > dataPagesFetched)
		dataPagesFetched = dataPagesFetchedBySel;

	/* 向启动代价加一页的 CPU 代价 */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * counts.searchEntries;

	/*
	 * 在摊销缓存前，再为这些数据页加一次 CPU 代价。
	 *
	 */
	*indexTotalCost += dataPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/* 计入缓存效应，同上文一样 */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/* 把 random_page_cost 用作每页代价 */
	*indexTotalCost += *indexStartupCost +
		dataPagesFetched * spc_random_page_cost;

	/*
	 * 正如 genericcostestimate，加上索引限定条件求值代价，
	 * 收取 CPU 代价但可无视 GIN 不支持的 indexorderbys。
	 *
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);
	qual_op_cost = cpu_operator_cost * list_length(indexQuals);

	*indexStartupCost += qual_arg_cost;
	*indexTotalCost += qual_arg_cost;

	/*
	 * 为每搜索条目加一份 CPU 代价，对应实际访问条目。
	 *
	 */
	*indexTotalCost += (counts.searchEntries * counts.arrayScans) * (qual_op_cost);
	/* 为各 posting list / 树中的每元组加 CPU 代价 */
	*indexTotalCost += (numTuples * *indexSelectivity) * (cpu_index_tuple_cost);
	*indexPages = dataPagesFetched;
}

/*
 * BRIN 的搜索行为与其他索引类型完全不同
 */
void
brincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	double		numPages = index->pages;
	RelOptInfo *baserel = index->rel;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	Cost		spc_seq_page_cost;
	Cost		spc_random_page_cost;
	double		qual_arg_cost;
	double		qualSelectivity;
	BrinStatsData statsData;
	double		indexRanges;
	double		minimalRanges;
	double		estimatedRanges;
	double		selec;
	Relation	indexRel;
	ListCell   *l;
	VariableStatData vardata;

	Assert(rte->rtekind == RTE_RELATION);

	/* 取得包含该索引的表空间的估计页代价 */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*
	 * 若可能，就从索引本身取得一些数据。否则就基于
	 * 关系页数编造一些看似合理的内部统计信息。
	 */
	if (!index->hypothetical)
	{
		/*
		 * 在 plancat.c 中应当已经取得了该索引上的锁。
		 */
		indexRel = index_open(index->indexoid, NoLock);
		brinGetStats(indexRel, &statsData);
		index_close(indexRel, NoLock);

		/* 算出索引中范围的实际数目 */
		indexRanges = Max(ceil((double) baserel->pages /
							   statsData.pagesPerRange), 1.0);
	}
	else
	{
		/*
		 * 假定每范围默认页数，并据此估计范围的数目。
		 *
		 */
		indexRanges = Max(ceil((double) baserel->pages /
							   BRIN_DEFAULT_PAGES_PER_RANGE), 1.0);

		statsData.pagesPerRange = BRIN_DEFAULT_PAGES_PER_RANGE;
		statsData.revmapNumPages = (indexRanges / REVMAP_PAGE_MAXITEMS) + 1;
	}

	/*
	 * 计算索引相关性
	 *
	 * 因为扫描时可以同等地使用全部索引限定条件，
	 * 我们可以使用查询所用各列相关性中绝对值最大者。
	 * 从零开始，即最坏可能的情形。若找不到任何相关性
	 * 统计信息，就保持其为 0。
	 */
	*indexCorrelation = 0;

	foreach(l, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, l);
		AttrNumber	attnum = index->indexkeys[iclause->indexcol];

		/* 尝试在关系中查找该索引列的统计信息 */
		if (attnum != 0)
		{
			/* 简单变量 —— 查看底层表的统计信息 */
			if (get_relation_stats_hook &&
				(*get_relation_stats_hook) (root, rte, attnum, &vardata))
			{
				/*
				 * 钩子接管了统计信息元组的获取。若它
				 * 确实提供了元组，最好也提供了 freefunc。
				 */
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR,
						 "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple =
					SearchSysCache3(STATRELATTINH,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(attnum),
									BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}
		else
		{
			/*
			 * 看来我们在索引中找到了一个表达式列。看看
			 * 它是否有统计信息。
			 */

			/* 从 0 基索引取得 attnum。 */
			attnum = iclause->indexcol + 1;

			if (get_index_stats_hook &&
				(*get_index_stats_hook) (root, index->indexoid, attnum, &vardata))
			{
				/*
				 * 钩子接管了统计信息元组的获取。若它
				 * 确实提供了元组，最好也提供了 freefunc。
				 */
				if (HeapTupleIsValid(vardata.statsTuple) &&
					!vardata.freefunc)
					elog(ERROR, "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple = SearchSysCache3(STATRELATTINH,
													 ObjectIdGetDatum(index->indexoid),
													 Int16GetDatum(attnum),
													 BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}

		if (HeapTupleIsValid(vardata.statsTuple))
		{
			AttStatsSlot sslot;

			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_CORRELATION, InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				double		varCorrelation = 0.0;

				if (sslot.nnumbers > 0)
					varCorrelation = fabs(sslot.numbers[0]);

				if (varCorrelation > *indexCorrelation)
					*indexCorrelation = varCorrelation;

				free_attstatsslot(&sslot);
			}
		}

		ReleaseVariableStats(vardata);
	}

	qualSelectivity = clauselist_selectivity(root, indexQuals,
											 baserel->relid,
											 JOIN_INNER, NULL);

	/*
	 * 现在计算当所有行在表堆中顺序完美时，我们能
	 * 匹配到的最小可能范围数。
	 */
	minimalRanges = ceil(indexRanges * qualSelectivity);

	/*
	 * 现在用统计信息中的 indexCorrelation 估计我们将
	 * 触及的范围数。小心不要除以零（注意我们用的
	 * 是相关性的绝对值）。
	 */
	if (*indexCorrelation < 1.0e-10)
		estimatedRanges = indexRanges;
	else
		estimatedRanges = Min(minimalRanges / *indexCorrelation, indexRanges);

	/* 我们预计访问表的这一部分 */
	selec = estimatedRanges / indexRanges;

	CLAMP_PROBABILITY(selec);

	*indexSelectivity = selec;

	/*
	 * 计算索引限定条件代价并加入索引代价中，正如
	 * genericcostestimate 那样。可以无视 indexorderbys，因为
	 * 不支持。
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);

	/*
	 * 把启动代价算作顺序读取整个 revmap 的代价，
	 * 其中包括执行索引限定条件的代价。
	 */
	*indexStartupCost =
		spc_seq_page_cost * statsData.revmapNumPages * loop_count;
	*indexStartupCost += qual_arg_cost;

	/*
	 * 读取 BRIN 索引时可能在常规页上有些来回，
	 * 因为 revmap 可能以非顺序次序指向它们；把总代价
	 * 计算为按随机次序读取整个索引。
	 */
	*indexTotalCost = *indexStartupCost +
		spc_random_page_cost * (numPages - statsData.revmapNumPages) * loop_count;

	/*
	 * 为我们预计会匹配到的每个范围元组收取少量代价。
	 * 这是为了反映操纵位图的代价。BRIN 扫描会在
	 * 我们找到匹配范围时为该范围每页设置一位，
	 * 所以我们必须把该代价乘以范围内的页数。
	 *
	 */
	*indexTotalCost += 0.1 * cpu_operator_cost * estimatedRanges *
		statsData.pagesPerRange;

	*indexPages = index->pages;
}
