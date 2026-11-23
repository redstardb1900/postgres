/*-------------------------------------------------------------------------
 *
 * orderedsetaggs.c
 *		有序集聚合函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/orderedsetaggs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"


/*
 * 有序集聚合的通用支持
 *
 * 有序集聚合的状态分为每组结构(作为内部类型过渡
 * 状态 datum 返回给 nodeAgg.c)与每查询结构;后者含
 * 可只建一次的数据与子对象,因各组间不会变化。每查
 * 询结构及附属数据位于执行器的每查询内存上下文,
 * 在 ExecutorEnd() 时隐式释放。
 *
 *
 * 这些结构在转移函数首次调用时建立。因允许
 * nodeAgg.c 合并输入与转移函数相同的有序集聚合
 * (但不合并假想集聚合),此信息不得依赖特定聚合
 * (即特定最终函数),也不得依赖该聚合的直接参数。
 *
 */

typedef struct OSAPerQueryState
{
	/* 本聚合的代表性 Aggref: */
	Aggref	   *aggref;
	/* 含本结构及其他每查询数据的内存上下文: */
	MemoryContext qcontext;
	/* 表达式求值上下文 */
	ExprContext *econtext;
	/* 是否预期一组内多次调用最终函数? */
	bool		rescan_needed;

	/* 以下字段仅在累积元组时使用: */

	/* 插入 sortstate 的元组的元组描述符: */
	TupleDesc	tupdesc;
	/* 用于插入/取出元组的元组槽: */
	TupleTableSlot *tupslot;
	/* 各排序列的排序信息 */
	int			numSortCols;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *eqOperators;
	Oid		   *sortCollations;
	bool	   *sortNullsFirsts;
	/* 相等操作符调用信息,仅在需要时创建: */
	ExprState  *compareTuple;

	/* 以下字段仅在累积 datum 时使用: */

	/* 被排序 datum 的数据类型信息: */
	Oid			sortColType;
	int16		typLen;
	bool		typByVal;
	char		typAlign;
	/* 排序次序信息: */
	Oid			sortOperator;
	Oid			eqOperator;
	Oid			sortCollation;
	bool		sortNullsFirst;
	/* 相等操作符调用信息,仅在需要时创建: */
	FmgrInfo	equalfn;
} OSAPerQueryState;

typedef struct OSAPerGroupState
{
	/* 链到本聚合的每查询状态: */
	OSAPerQueryState *qstate;
	/* 含每组数据的内存上下文: */
	MemoryContext gcontext;
	/* 正在累积数据的排序对象: */
	Tuplesortstate *sortstate;
	/* 已插入 sortstate 的普通行数: */
	int64		number_of_rows;
	/* 是否已做过 tuplesort_performsort? */
	bool		sort_done;
} OSAPerGroupState;

static void ordered_set_shutdown(Datum arg);


/*
 * 为有序集聚合建立工作状态
 */
static OSAPerGroupState *
ordered_set_startup(FunctionCallInfo fcinfo, bool use_tuples)
{
	OSAPerGroupState *osastate;
	OSAPerQueryState *qstate;
	MemoryContext gcontext;
	MemoryContext oldcontext;
	int			tuplesortopt;

	/*
	 * 确认按聚合调用(而非窗口函数),并取得 Agg 节点
	 * 的组寿命上下文(可能随组变化,故不应缓存在每
	 * 查询状态中)。
	 */
	if (AggCheckCallContext(fcinfo, &gcontext) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "ordered-set aggregate called in non-aggregate context");

	/*
	 * 在 fn_extra 中保留到每查询状态的链接;若尚无则
	 * 创建它,并做所需的每查询设置。
	 */
	qstate = (OSAPerQueryState *) fcinfo->flinfo->fn_extra;
	if (qstate == NULL)
	{
		Aggref	   *aggref;
		MemoryContext qcontext;
		List	   *sortlist;
		int			numSortCols;

		/* 取 Aggref 以便检查聚合的参数 */
		aggref = AggGetAggref(fcinfo);
		if (!aggref)
			elog(ERROR, "ordered-set aggregate called in non-aggregate context");
		if (!AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			elog(ERROR, "ordered-set aggregate support function called for non-ordered-set aggregate");

		/*
		 * 在 fn_mcxt 中准备每查询结构;假定它是执行器的
		 * 每查询上下文;无论如何都是存放经 fn_extra 找到
		 * 之物的合适位置。
		 */
		qcontext = fcinfo->flinfo->fn_mcxt;
		oldcontext = MemoryContextSwitchTo(qcontext);

		qstate = (OSAPerQueryState *) palloc0(sizeof(OSAPerQueryState));
		qstate->aggref = aggref;
		qstate->qcontext = qcontext;

		/* 过渡状态共享时须支持重扫 */
		qstate->rescan_needed = AggStateIsShared(fcinfo);

		/* 提取排序信息 */
		sortlist = aggref->aggorder;
		numSortCols = list_length(sortlist);

		if (use_tuples)
		{
			bool		ishypothetical = (aggref->aggkind == AGGKIND_HYPOTHETICAL);
			ListCell   *lc;
			int			i;

			if (ishypothetical)
				numSortCols++;	/* 为标志列腾出空间 */
			qstate->numSortCols = numSortCols;
			qstate->sortColIdx = (AttrNumber *) palloc(numSortCols * sizeof(AttrNumber));
			qstate->sortOperators = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->eqOperators = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->sortCollations = (Oid *) palloc(numSortCols * sizeof(Oid));
			qstate->sortNullsFirsts = (bool *) palloc(numSortCols * sizeof(bool));

			i = 0;
			foreach(lc, sortlist)
			{
				SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
				TargetEntry *tle = get_sortgroupclause_tle(sortcl,
														   aggref->args);

				/* 解析器应已保证这一点 */
				Assert(OidIsValid(sortcl->sortop));

				qstate->sortColIdx[i] = tle->resno;
				qstate->sortOperators[i] = sortcl->sortop;
				qstate->eqOperators[i] = sortcl->eqop;
				qstate->sortCollations[i] = exprCollation((Node *) tle->expr);
				qstate->sortNullsFirsts[i] = sortcl->nulls_first;
				i++;
			}

			if (ishypothetical)
			{
				/* 追加整型标志列作为最后排序列 */
				qstate->sortColIdx[i] = list_length(aggref->args) + 1;
				qstate->sortOperators[i] = Int4LessOperator;
				qstate->eqOperators[i] = Int4EqualOperator;
				qstate->sortCollations[i] = InvalidOid;
				qstate->sortNullsFirsts[i] = false;
				i++;
			}

			Assert(i == numSortCols);

			/*
			 * 取得对应聚合输入(含排序表达式)的
			 * tupledesc。
			 */
			qstate->tupdesc = ExecTypeFromTL(aggref->args);

			/* 若需要标志列,改 tupledesc 以包含它 */
			if (ishypothetical)
			{
				TupleDesc	newdesc;
				int			natts = qstate->tupdesc->natts;

				newdesc = CreateTemplateTupleDesc(natts + 1);
				for (i = 1; i <= natts; i++)
					TupleDescCopyEntry(newdesc, i, qstate->tupdesc, i);

				TupleDescInitEntry(newdesc,
								   (AttrNumber) ++natts,
								   "flag",
								   INT4OID,
								   -1,
								   0);

				FreeTupleDesc(qstate->tupdesc);
				qstate->tupdesc = newdesc;
			}

			/* 创建用于存取行的槽 */
			qstate->tupslot = MakeSingleTupleTableSlot(qstate->tupdesc,
													   &TTSOpsMinimalTuple);
		}
		else
		{
			/* 排序单值 */
			SortGroupClause *sortcl;
			TargetEntry *tle;

			if (numSortCols != 1 || aggref->aggkind == AGGKIND_HYPOTHETICAL)
				elog(ERROR, "ordered-set aggregate support function does not support multiple aggregated columns");

			sortcl = (SortGroupClause *) linitial(sortlist);
			tle = get_sortgroupclause_tle(sortcl, aggref->args);

			/* 解析器应已保证这一点 */
			Assert(OidIsValid(sortcl->sortop));

			/* 保存排序信息 */
			qstate->sortColType = exprType((Node *) tle->expr);
			qstate->sortOperator = sortcl->sortop;
			qstate->eqOperator = sortcl->eqop;
			qstate->sortCollation = exprCollation((Node *) tle->expr);
			qstate->sortNullsFirst = sortcl->nulls_first;

			/* 保存类型信息 */
			get_typlenbyvalalign(qstate->sortColType,
								 &qstate->typLen,
								 &qstate->typByVal,
								 &qstate->typAlign);
		}

		fcinfo->flinfo->fn_extra = (void *) qstate;

		MemoryContextSwitchTo(oldcontext);
	}

	/* 在组寿命上下文中构建所需对象 */
	oldcontext = MemoryContextSwitchTo(gcontext);

	osastate = (OSAPerGroupState *) palloc(sizeof(OSAPerGroupState));
	osastate->qstate = qstate;
	osastate->gcontext = gcontext;

	tuplesortopt = TUPLESORT_NONE;

	if (qstate->rescan_needed)
		tuplesortopt |= TUPLESORT_RANDOMACCESS;

	/*
	 * 初始化 tuplesort 对象
	 */
	if (use_tuples)
		osastate->sortstate = tuplesort_begin_heap(qstate->tupdesc,
												   qstate->numSortCols,
												   qstate->sortColIdx,
												   qstate->sortOperators,
												   qstate->sortCollations,
												   qstate->sortNullsFirsts,
												   work_mem,
												   NULL,
												   tuplesortopt);
	else
		osastate->sortstate = tuplesort_begin_datum(qstate->sortColType,
													qstate->sortOperator,
													qstate->sortCollation,
													qstate->sortNullsFirst,
													work_mem,
													NULL,
													tuplesortopt);

	osastate->number_of_rows = 0;
	osastate->sort_done = false;

	/* 注册关闭回调,在组结束时清理 */
	AggRegisterCallback(fcinfo,
						ordered_set_shutdown,
						PointerGetDatum(osastate));

	MemoryContextSwitchTo(oldcontext);

	return osastate;
}

/*
 * 有序集聚合求值完成时清理。
 *
 * 不必费心释放每组内存上下文中的对象,nodeAgg.c
 * 反正会复位;也不应释放每查询上下文中的任何东
 * 西,ExecutorEnd 会在(若为本最后一组时)清掉它。
 * 但必须留意释放任何潜在的非内存资源。
 *
 *
 * 若不预期多次 finalfn 调用,或可指望 finalfn 自
 * 行清理;但两种情形用同一方式更简单也更易测。
 *
 */
static void
ordered_set_shutdown(Datum arg)
{
	OSAPerGroupState *osastate = (OSAPerGroupState *) DatumGetPointer(arg);

	/* Tuplesort 或有临时文件。 */
	if (osastate->sortstate)
		tuplesort_end(osastate->sortstate);
	osastate->sortstate = NULL;
	/* tupleslot 大概未持有 pin,但为安全起见仍处理。 */
	if (osastate->qstate->tupslot)
		ExecClearTuple(osastate->qstate->tupslot);
}


/*
 * 有序集聚合的通用转移函数
 * 用于单输入列且要抑制空值的情形
 */
Datum
ordered_set_transition(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;

	/* 若为首次调用,创建过渡状态工作区 */
	if (PG_ARGISNULL(0))
		osastate = ordered_set_startup(fcinfo, false);
	else
		osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 仅当非空时把 datum 载入 tuplesort 对象 */
	if (!PG_ARGISNULL(1))
	{
		tuplesort_putdatum(osastate->sortstate, PG_GETARG_DATUM(1), false);
		osastate->number_of_rows++;
	}

	PG_RETURN_POINTER(osastate);
}

/*
 * 有序集聚合的通用转移函数
 * 用于(可能)有多列聚合输入的情形
 */
Datum
ordered_set_transition_multi(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;
	TupleTableSlot *slot;
	int			nargs;
	int			i;

	/* 若为首次调用,创建过渡状态工作区 */
	if (PG_ARGISNULL(0))
		osastate = ordered_set_startup(fcinfo, true);
	else
		osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 由过渡值以外的其余输入组成元组 */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	nargs = PG_NARGS() - 1;
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	if (osastate->qstate->aggref->aggkind == AGGKIND_HYPOTHETICAL)
	{
		/* 加零标志值,标此行为普通输入行 */
		slot->tts_values[i] = Int32GetDatum(0);
		slot->tts_isnull[i] = false;
		i++;
	}
	Assert(i == slot->tts_tupleDescriptor->natts);
	ExecStoreVirtualTuple(slot);

	/* 把该行载入 tuplesort 对象 */
	tuplesort_puttupleslot(osastate->sortstate, slot);
	osastate->number_of_rows++;

	PG_RETURN_POINTER(osastate);
}


/*
 * percentile_disc(float8) WITHIN GROUP (anyelement) - 离散百分位数
 */
Datum
percentile_disc_final(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;
	double		percentile;
	Datum		val;
	bool		isnull;
	int64		rownum;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 取得并检查百分位参数 */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	percentile = PG_GETARG_FLOAT8(1);

	if (percentile < 0 || percentile > 1 || isnan(percentile))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("percentile value %g is not between 0 and 1",
						percentile)));

	/* 若无普通行,结果为 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 若只见过 NULL 输入,number_of_rows 可能为 0 */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* 完成排序,或若已做过则重扫 */
	if (!osastate->sort_done)
	{
		tuplesort_performsort(osastate->sortstate);
		osastate->sort_done = true;
	}
	else
		tuplesort_rescan(osastate->sortstate);

	/*----------
	 * 求满足 (K/N) >= percentile 的最小 K。
	 * N>0,故 K >= N*percentile,故 K = ceil(N*percentile)。
	 * 因此跳过 K-1 行(若 K>0)并返回接着取出的行。
	 *----------
	 */
	rownum = (int64) ceil(percentile * osastate->number_of_rows);
	Assert(rownum <= osastate->number_of_rows);

	if (rownum > 1)
	{
		if (!tuplesort_skiptuples(osastate->sortstate, rownum - 1, true))
			elog(ERROR, "missing row in percentile_disc");
	}

	if (!tuplesort_getdatum(osastate->sortstate, true, true, &val, &isnull,
							NULL))
		elog(ERROR, "missing row in percentile_disc");

	/* 本不应存过空值,但仍做正确处理 */
	if (isnull)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(val);
}


/*
 * percentile_cont 需在相邻值之间插值。用辅助函数
 * 完成,以便其余代码可在各类型间共享。
 *
 */
typedef Datum (*LerpFunc) (Datum lo, Datum hi, double pct);

static Datum
float8_lerp(Datum lo, Datum hi, double pct)
{
	double		loval = DatumGetFloat8(lo);
	double		hival = DatumGetFloat8(hi);

	return Float8GetDatum(loval + (pct * (hival - loval)));
}

static Datum
interval_lerp(Datum lo, Datum hi, double pct)
{
	Datum		diff_result = DirectFunctionCall2(interval_mi, hi, lo);
	Datum		mul_result = DirectFunctionCall2(interval_mul,
												 diff_result,
												 Float8GetDatumFast(pct));

	return DirectFunctionCall2(interval_pl, mul_result, lo);
}

/*
 * 连续百分位数
 */
static Datum
percentile_cont_final_common(FunctionCallInfo fcinfo,
							 Oid expect_type,
							 LerpFunc lerpfunc)
{
	OSAPerGroupState *osastate;
	double		percentile;
	int64		first_row = 0;
	int64		second_row = 0;
	Datum		val;
	Datum		first_val;
	Datum		second_val;
	double		proportion;
	bool		isnull;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 取得并检查百分位参数 */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	percentile = PG_GETARG_FLOAT8(1);

	if (percentile < 0 || percentile > 1 || isnan(percentile))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("percentile value %g is not between 0 and 1",
						percentile)));

	/* 若无普通行,结果为 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 若只见过 NULL 输入,number_of_rows 可能为 0 */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	Assert(expect_type == osastate->qstate->sortColType);

	/* 完成排序,或若已做过则重扫 */
	if (!osastate->sort_done)
	{
		tuplesort_performsort(osastate->sortstate);
		osastate->sort_done = true;
	}
	else
		tuplesort_rescan(osastate->sortstate);

	first_row = floor(percentile * (osastate->number_of_rows - 1));
	second_row = ceil(percentile * (osastate->number_of_rows - 1));

	Assert(first_row < osastate->number_of_rows);

	if (!tuplesort_skiptuples(osastate->sortstate, first_row, true))
		elog(ERROR, "missing row in percentile_cont");

	if (!tuplesort_getdatum(osastate->sortstate, true, true, &first_val,
							&isnull, NULL))
		elog(ERROR, "missing row in percentile_cont");
	if (isnull)
		PG_RETURN_NULL();

	if (first_row == second_row)
	{
		val = first_val;
	}
	else
	{
		if (!tuplesort_getdatum(osastate->sortstate, true, true, &second_val,
								&isnull, NULL))
			elog(ERROR, "missing row in percentile_cont");

		if (isnull)
			PG_RETURN_NULL();

		proportion = (percentile * (osastate->number_of_rows - 1)) - first_row;
		val = lerpfunc(first_val, second_val, proportion);
	}

	PG_RETURN_DATUM(val);
}

/*
 * percentile_cont(float8) WITHIN GROUP (float8) - 连续百分位数
 */
Datum
percentile_cont_float8_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_final_common(fcinfo, FLOAT8OID, float8_lerp);
}

/*
 * percentile_cont(float8) WITHIN GROUP (interval) - 连续百分位数
 */
Datum
percentile_cont_interval_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_final_common(fcinfo, INTERVALOID, interval_lerp);
}


/*
 * 处理百分位点数组的支持代码
 *
 * 注意:各 pct_info 项中,second_row 应等于或恰好比
 * first_row 多 1。
 */
struct pct_info
{
	int64		first_row;		/* 要采样的首行 */
	int64		second_row;		/* 可能的第二采样行 */
	double		proportion;		/* 插值比例 */
	int			idx;			/* 本项在原数组中的下标 */
};

/*
 * 按 first_row 再 second_row 排序 pct_infos 的比较器
 */
static int
pct_info_cmp(const void *pa, const void *pb)
{
	const struct pct_info *a = (const struct pct_info *) pa;
	const struct pct_info *b = (const struct pct_info *) pb;

	if (a->first_row != b->first_row)
		return (a->first_row < b->first_row) ? -1 : 1;
	if (a->second_row != b->second_row)
		return (a->second_row < b->second_row) ? -1 : 1;
	return 0;
}

/*
 * 构造数组,标明百分位点要采样哪些行。
 */
static struct pct_info *
setup_pct_info(int num_percentiles,
			   Datum *percentiles_datum,
			   bool *percentiles_null,
			   int64 rowcount,
			   bool continuous)
{
	struct pct_info *pct_info;
	int			i;

	pct_info = (struct pct_info *) palloc(num_percentiles * sizeof(struct pct_info));

	for (i = 0; i < num_percentiles; i++)
	{
		pct_info[i].idx = i;

		if (percentiles_null[i])
		{
			/* 数组中任一 NULL 的哑项 */
			pct_info[i].first_row = 0;
			pct_info[i].second_row = 0;
			pct_info[i].proportion = 0;
		}
		else
		{
			double		p = DatumGetFloat8(percentiles_datum[i]);

			if (p < 0 || p > 1 || isnan(p))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("percentile value %g is not between 0 and 1",
								p)));

			if (continuous)
			{
				pct_info[i].first_row = 1 + floor(p * (rowcount - 1));
				pct_info[i].second_row = 1 + ceil(p * (rowcount - 1));
				pct_info[i].proportion = (p * (rowcount - 1)) - floor(p * (rowcount - 1));
			}
			else
			{
				/*----------
				 * 求满足 (K/N) >= percentile 的最小 K。
				 * N>0,故 K >= N*percentile,故
				 * K = ceil(N*percentile);且不小于 1。
				 *----------
				 */
				int64		row = (int64) ceil(p * rowcount);

				row = Max(1, row);
				pct_info[i].first_row = row;
				pct_info[i].second_row = row;
				pct_info[i].proportion = 0;
			}
		}
	}

	/*
	 * 参数数组未必有序,但须按行序访问,故按
	 * first_row/second_row 排序。
	 */
	qsort(pct_info, num_percentiles, sizeof(struct pct_info), pct_info_cmp);

	return pct_info;
}

/*
 * percentile_disc(float8[]) WITHIN GROUP (anyelement) - 离散百分位点
 */
Datum
percentile_disc_multi_final(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;
	ArrayType  *param;
	Datum	   *percentiles_datum;
	bool	   *percentiles_null;
	int			num_percentiles;
	struct pct_info *pct_info;
	Datum	   *result_datum;
	bool	   *result_isnull;
	int64		rownum = 0;
	Datum		val = (Datum) 0;
	bool		isnull = true;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 若无普通行,结果为 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 若只见过 NULL 输入,number_of_rows 可能为 0 */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* 解构百分位数组输入 */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	param = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array_builtin(param, FLOAT8OID,
							  &percentiles_datum,
							  &percentiles_null,
							  &num_percentiles);

	if (num_percentiles == 0)
		PG_RETURN_POINTER(construct_empty_array(osastate->qstate->sortColType));

	pct_info = setup_pct_info(num_percentiles,
							  percentiles_datum,
							  percentiles_null,
							  osastate->number_of_rows,
							  false);

	result_datum = (Datum *) palloc(num_percentiles * sizeof(Datum));
	result_isnull = (bool *) palloc(num_percentiles * sizeof(bool));

	/*
	 * 先处理参数数组中的空值—它们按 row=0 排到前
	 * 面,故把对应结果下标置为空
	 */
	for (i = 0; i < num_percentiles; i++)
	{
		int			idx = pct_info[i].idx;

		if (pct_info[i].first_row > 0)
			break;

		result_datum[idx] = (Datum) 0;
		result_isnull[idx] = true;
	}

	/*
	 * 处理完空值后若还有剩余,则研磨输入并提取
	 * 所需值
	 */
	if (i < num_percentiles)
	{
		/* 完成排序,或若已做过则重扫 */
		if (!osastate->sort_done)
		{
			tuplesort_performsort(osastate->sortstate);
			osastate->sort_done = true;
		}
		else
			tuplesort_rescan(osastate->sortstate);

		for (; i < num_percentiles; i++)
		{
			int64		target_row = pct_info[i].first_row;
			int			idx = pct_info[i].idx;

			/* 推进到目标行(若尚未到达) */
			if (target_row > rownum)
			{
				if (!tuplesort_skiptuples(osastate->sortstate, target_row - rownum - 1, true))
					elog(ERROR, "missing row in percentile_disc");

				if (!tuplesort_getdatum(osastate->sortstate, true, true, &val,
										&isnull, NULL))
					elog(ERROR, "missing row in percentile_disc");

				rownum = target_row;
			}

			result_datum[idx] = val;
			result_isnull[idx] = isnull;
		}
	}

	/* 使输出数组与输入同形 */
	PG_RETURN_POINTER(construct_md_array(result_datum, result_isnull,
										 ARR_NDIM(param),
										 ARR_DIMS(param),
										 ARR_LBOUND(param),
										 osastate->qstate->sortColType,
										 osastate->qstate->typLen,
										 osastate->qstate->typByVal,
										 osastate->qstate->typAlign));
}

/*
 * percentile_cont(float8[]) WITHIN GROUP () - 连续百分位点
 */
static Datum
percentile_cont_multi_final_common(FunctionCallInfo fcinfo,
								   Oid expect_type,
								   int16 typLen, bool typByVal, char typAlign,
								   LerpFunc lerpfunc)
{
	OSAPerGroupState *osastate;
	ArrayType  *param;
	Datum	   *percentiles_datum;
	bool	   *percentiles_null;
	int			num_percentiles;
	struct pct_info *pct_info;
	Datum	   *result_datum;
	bool	   *result_isnull;
	int64		rownum = 0;
	Datum		first_val = (Datum) 0;
	Datum		second_val = (Datum) 0;
	bool		isnull;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 若无普通行,结果为 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 若只见过 NULL 输入,number_of_rows 可能为 0 */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	Assert(expect_type == osastate->qstate->sortColType);

	/* 解构百分位数组输入 */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	param = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array_builtin(param, FLOAT8OID,
							  &percentiles_datum,
							  &percentiles_null,
							  &num_percentiles);

	if (num_percentiles == 0)
		PG_RETURN_POINTER(construct_empty_array(osastate->qstate->sortColType));

	pct_info = setup_pct_info(num_percentiles,
							  percentiles_datum,
							  percentiles_null,
							  osastate->number_of_rows,
							  true);

	result_datum = (Datum *) palloc(num_percentiles * sizeof(Datum));
	result_isnull = (bool *) palloc(num_percentiles * sizeof(bool));

	/*
	 * 先处理参数数组中的空值—它们按 row=0 排到前
	 * 面,故把对应结果下标置为空
	 */
	for (i = 0; i < num_percentiles; i++)
	{
		int			idx = pct_info[i].idx;

		if (pct_info[i].first_row > 0)
			break;

		result_datum[idx] = (Datum) 0;
		result_isnull[idx] = true;
	}

	/*
	 * 处理完空值后若还有剩余,则研磨输入并提取
	 * 所需值
	 */
	if (i < num_percentiles)
	{
		/* 完成排序,或若已做过则重扫 */
		if (!osastate->sort_done)
		{
			tuplesort_performsort(osastate->sortstate);
			osastate->sort_done = true;
		}
		else
			tuplesort_rescan(osastate->sortstate);

		for (; i < num_percentiles; i++)
		{
			int64		first_row = pct_info[i].first_row;
			int64		second_row = pct_info[i].second_row;
			int			idx = pct_info[i].idx;

			/*
			 * 推进到 first_row(若尚未到达)。注意 rownum
			 * 可能已超过 first_row,此时 first_val 已正确。
			 * (与上一百分位点相同的两输入行之间插值时
			 * 会发生。)
			 */
			if (first_row > rownum)
			{
				if (!tuplesort_skiptuples(osastate->sortstate, first_row - rownum - 1, true))
					elog(ERROR, "missing row in percentile_cont");

				if (!tuplesort_getdatum(osastate->sortstate, true, true,
										&first_val, &isnull, NULL) || isnull)
					elog(ERROR, "missing row in percentile_cont");

				rownum = first_row;
				/* 始终推进 second_val 为最新输入值 */
				second_val = first_val;
			}
			else if (first_row == rownum)
			{
				/*
				 * 已在目标行,故先前必已把其值读入
				 * second_val(或许也读入 first_val,但赋值无妨)。
				 * 
				 */
				first_val = second_val;
			}

			/* 若需要则取 second_row */
			if (second_row > rownum)
			{
				if (!tuplesort_getdatum(osastate->sortstate, true, true,
										&second_val, &isnull, NULL) || isnull)
					elog(ERROR, "missing row in percentile_cont");
				rownum++;
			}
			/* 此时应正好位于 second_row */
			Assert(second_row == rownum);

			/* 计算适当结果 */
			if (second_row > first_row)
				result_datum[idx] = lerpfunc(first_val, second_val,
											 pct_info[i].proportion);
			else
				result_datum[idx] = first_val;

			result_isnull[idx] = false;
		}
	}

	/* 使输出数组与输入同形 */
	PG_RETURN_POINTER(construct_md_array(result_datum, result_isnull,
										 ARR_NDIM(param),
										 ARR_DIMS(param), ARR_LBOUND(param),
										 expect_type,
										 typLen,
										 typByVal,
										 typAlign));
}

/*
 * percentile_cont(float8[]) WITHIN GROUP (float8) - 连续百分位点
 */
Datum
percentile_cont_float8_multi_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_multi_final_common(fcinfo,
											  FLOAT8OID,
	/* float8 类型硬接线信息 */
											  sizeof(float8),
											  FLOAT8PASSBYVAL,
											  TYPALIGN_DOUBLE,
											  float8_lerp);
}

/*
 * percentile_cont(float8[]) WITHIN GROUP (interval) - 连续百分位点
 */
Datum
percentile_cont_interval_multi_final(PG_FUNCTION_ARGS)
{
	return percentile_cont_multi_final_common(fcinfo,
											  INTERVALOID,
	/* interval 类型硬接线信息 */
											  16, false, TYPALIGN_DOUBLE,
											  interval_lerp);
}


/*
 * mode() WITHIN GROUP (anyelement) - 众数
 */
Datum
mode_final(PG_FUNCTION_ARGS)
{
	OSAPerGroupState *osastate;
	Datum		val;
	bool		isnull;
	Datum		mode_val = (Datum) 0;
	int64		mode_freq = 0;
	Datum		last_val = (Datum) 0;
	int64		last_val_freq = 0;
	bool		last_val_is_mode = false;
	FmgrInfo   *equalfn;
	Datum		abbrev_val = (Datum) 0;
	Datum		last_abbrev_val = (Datum) 0;
	bool		shouldfree;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 若无普通行,结果为 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);

	/* 若只见过 NULL 输入,number_of_rows 可能为 0 */
	if (osastate->number_of_rows == 0)
		PG_RETURN_NULL();

	/* 若尚未查找,则查找该数据类型的相等函数 */
	equalfn = &(osastate->qstate->equalfn);
	if (!OidIsValid(equalfn->fn_oid))
		fmgr_info_cxt(get_opcode(osastate->qstate->eqOperator), equalfn,
					  osastate->qstate->qcontext);

	shouldfree = !(osastate->qstate->typByVal);

	/* 完成排序,或若已做过则重扫 */
	if (!osastate->sort_done)
	{
		tuplesort_performsort(osastate->sortstate);
		osastate->sort_done = true;
	}
	else
		tuplesort_rescan(osastate->sortstate);

	/* 扫描元组并计频率 */
	while (tuplesort_getdatum(osastate->sortstate, true, true, &val, &isnull,
							  &abbrev_val))
	{
		/* 不预期有空值,但若有则忽略 */
		if (isnull)
			continue;

		if (last_val_freq == 0)
		{
			/* 首个非空值—暂为众数 */
			mode_val = last_val = val;
			mode_freq = last_val_freq = 1;
			last_val_is_mode = true;
			last_abbrev_val = abbrev_val;
		}
		else if (abbrev_val == last_abbrev_val &&
				 DatumGetBool(FunctionCall2Coll(equalfn, PG_GET_COLLATION(), val, last_val)))
		{
			/* 值等于前值,计入 */
			if (last_val_is_mode)
				mode_freq++;	/* 不必维护 last_val_freq */
			else if (++last_val_freq > mode_freq)
			{
				/* last_val 成为新众数 */
				if (shouldfree)
					pfree(DatumGetPointer(mode_val));
				mode_val = last_val;
				mode_freq = last_val_freq;
				last_val_is_mode = true;
			}
			if (shouldfree)
				pfree(DatumGetPointer(val));
		}
		else
		{
			/* val 应替换 last_val */
			if (shouldfree && !last_val_is_mode)
				pfree(DatumGetPointer(last_val));
			last_val = val;
			/* 复用缩写键以避免相等函数调用 */
			last_abbrev_val = abbrev_val;
			last_val_freq = 1;
			last_val_is_mode = false;
		}

		CHECK_FOR_INTERRUPTS();
	}

	if (shouldfree && !last_val_is_mode)
		pfree(DatumGetPointer(last_val));

	if (mode_freq)
		PG_RETURN_DATUM(mode_val);
	else
		PG_RETURN_NULL();
}


/*
 * 假想集函数参数合理性检查的共用代码。无需友
 * 好报错,仅当有人弄乱聚合定义时才会发生。但
 * 检查仍为安全所需。
 */
static void
hypothetical_check_argtypes(FunctionCallInfo fcinfo, int nargs,
							TupleDesc tupdesc)
{
	int			i;

	/* 检查确有 int4 标志列 */
	if (!tupdesc ||
		(nargs + 1) != tupdesc->natts ||
		TupleDescAttr(tupdesc, nargs)->atttypid != INT4OID)
		elog(ERROR, "type mismatch in hypothetical-set function");

	/* 检查直接参数与聚合参数类型匹配 */
	for (i = 0; i < nargs; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (get_fn_expr_argtype(fcinfo->flinfo, i + 1) != attr->atttypid)
			elog(ERROR, "type mismatch in hypothetical-set function");
	}
}

/*
 * 计算假想行的秩
 *
 * flag 为 -1 时假想行排在同侪之前,+1 时
 * 排到后面。
 * 普通行总数经 *number_of_rows 返回。
 */
static int64
hypothetical_rank_common(FunctionCallInfo fcinfo, int flag,
						 int64 *number_of_rows)
{
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	OSAPerGroupState *osastate;
	TupleTableSlot *slot;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 若无普通行,秩恒为 1 */
	if (PG_ARGISNULL(0))
	{
		*number_of_rows = 0;
		return 1;
	}

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);
	*number_of_rows = osastate->number_of_rows;

	/* 把 nargs 调整为直接(或聚合)参数个数 */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->qstate->tupdesc);

	/* 因需要假想行,不能共享过渡状态 */
	Assert(!osastate->sort_done);

	/* 把假想行插入排序 */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(flag);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* 完成排序 */
	tuplesort_performsort(osastate->sortstate);
	osastate->sort_done = true;

	/* 迭代直至找到假想行 */
	while (tuplesort_gettupleslot(osastate->sortstate, true, true, slot, NULL))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);

	return rank;
}


/*
 * rank() - 假想行的秩
 */
Datum
hypothetical_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	PG_RETURN_INT64(rank);
}

/*
 * percent_rank() - 假想行的百分位秩
 */
Datum
hypothetical_percent_rank_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, -1, &rowcount);

	if (rowcount == 0)
		PG_RETURN_FLOAT8(0);

	result_val = (double) (rank - 1) / (double) (rowcount);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * cume_dist() - 假想行的累积分布
 */
Datum
hypothetical_cume_dist_final(PG_FUNCTION_ARGS)
{
	int64		rank;
	int64		rowcount;
	double		result_val;

	rank = hypothetical_rank_common(fcinfo, 1, &rowcount);

	result_val = (double) (rank) / (double) (rowcount + 1);

	PG_RETURN_FLOAT8(result_val);
}

/*
 * dense_rank() - 假想行无间断秩
 */
Datum
hypothetical_dense_rank_final(PG_FUNCTION_ARGS)
{
	ExprContext *econtext;
	ExprState  *compareTuple;
	int			nargs = PG_NARGS() - 1;
	int64		rank = 1;
	int64		duplicate_count = 0;
	OSAPerGroupState *osastate;
	int			numDistinctCols;
	Datum		abbrevVal = (Datum) 0;
	Datum		abbrevOld = (Datum) 0;
	TupleTableSlot *slot;
	TupleTableSlot *extraslot;
	TupleTableSlot *slot2;
	int			i;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	/* 若无普通行,秩恒为 1 */
	if (PG_ARGISNULL(0))
		PG_RETURN_INT64(rank);

	osastate = (OSAPerGroupState *) PG_GETARG_POINTER(0);
	econtext = osastate->qstate->econtext;
	if (!econtext)
	{
		MemoryContext oldcontext;

		/* 确保在正确的父上下文下创建 econtext。 */
		oldcontext = MemoryContextSwitchTo(osastate->qstate->qcontext);
		osastate->qstate->econtext = CreateStandaloneExprContext();
		econtext = osastate->qstate->econtext;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 把 nargs 调整为直接(或聚合)参数个数 */
	if (nargs % 2 != 0)
		elog(ERROR, "wrong number of arguments in hypothetical-set function");
	nargs /= 2;

	hypothetical_check_argtypes(fcinfo, nargs, osastate->qstate->tupdesc);

	/*
	 * 比较元组时可省略标志列,因只会比较
	 * flag == 0 的行。
	 */
	numDistinctCols = osastate->qstate->numSortCols - 1;

	/* 若尚未建立则构建元组比较器 */
	compareTuple = osastate->qstate->compareTuple;
	if (compareTuple == NULL)
	{
		AttrNumber *sortColIdx = osastate->qstate->sortColIdx;
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(osastate->qstate->qcontext);
		compareTuple = execTuplesMatchPrepare(osastate->qstate->tupdesc,
											  numDistinctCols,
											  sortColIdx,
											  osastate->qstate->eqOperators,
											  osastate->qstate->sortCollations,
											  NULL);
		MemoryContextSwitchTo(oldContext);
		osastate->qstate->compareTuple = compareTuple;
	}

	/* 因需要假想行,不能共享过渡状态 */
	Assert(!osastate->sort_done);

	/* 把假想行插入排序 */
	slot = osastate->qstate->tupslot;
	ExecClearTuple(slot);
	for (i = 0; i < nargs; i++)
	{
		slot->tts_values[i] = PG_GETARG_DATUM(i + 1);
		slot->tts_isnull[i] = PG_ARGISNULL(i + 1);
	}
	slot->tts_values[i] = Int32GetDatum(-1);
	slot->tts_isnull[i] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(osastate->sortstate, slot);

	/* 完成排序 */
	tuplesort_performsort(osastate->sortstate);
	osastate->sort_done = true;

	/*
	 * 交替取入 tupslot 与 extraslot,以便比较时有前
	 * 一行可用。做法是每行后交换槽指针变量。
	 * 
	 */
	extraslot = MakeSingleTupleTableSlot(osastate->qstate->tupdesc,
										 &TTSOpsMinimalTuple);
	slot2 = extraslot;

	/* 迭代直至找到假想行 */
	while (tuplesort_gettupleslot(osastate->sortstate, true, true, slot,
								  &abbrevVal))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, nargs + 1, &isnull);
		TupleTableSlot *tmpslot;

		if (!isnull && DatumGetInt32(d) != 0)
			break;

		/* 计非互异行 */
		econtext->ecxt_outertuple = slot;
		econtext->ecxt_innertuple = slot2;

		if (!TupIsNull(slot2) &&
			abbrevVal == abbrevOld &&
			ExecQualAndReset(compareTuple, econtext))
			duplicate_count++;

		tmpslot = slot2;
		slot2 = slot;
		slot = tmpslot;
		/* 复用缩写键以避免 ExecQual() 调用 */
		abbrevOld = abbrevVal;

		rank++;

		CHECK_FOR_INTERRUPTS();
	}

	ExecClearTuple(slot);
	ExecClearTuple(slot2);

	ExecDropSingleTupleTableSlot(extraslot);

	rank = rank - duplicate_count;

	PG_RETURN_INT64(rank);
}
