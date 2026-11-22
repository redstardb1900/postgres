/*-------------------------------------------------------------------------
 *
 * nodeAgg.h
 *	  prototypes for nodeAgg.c
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeAgg.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "access/parallel.h"
#include "nodes/execnodes.h"


/*
 * AggStatePerTransData - per aggregate state value information
 *
 * Working state for updating the aggregate's state value, by calling the
 * transition function with an input row. This struct does not store the
 * information needed to produce the final aggregate result from the transition
 * state, that's stored in AggStatePerAggData instead. This separation allows
 * multiple aggregate results to be produced from a single state value.
 */
typedef struct AggStatePerTransData
{
	/*
	 * 这些值在 ExecInitAgg() 期间建立且此后
	 * 不改:
	 */

	/*
	 * Link to an Aggref expr this state value is for.
	 *
	 * There can be multiple Aggref's sharing the same state value, so long as
	 * the inputs and transition functions are identical and the final
	 * functions are not read-write.  This points to the first one of them.
	 */
	Aggref	   *aggref;

	/*
	 * 此状态值是否确实被多个 Aggref 共享?
	 */
	bool		aggshared;

	/*
	 * 对非 aggpresorted 的 ORDER BY 与 DISTINCT Aggref 为真.
	 */
	bool		aggsortrequired;

	/*
	 * Number of aggregated input columns.  This includes ORDER BY expressions
	 * in both the plain-agg and ordered-set cases.  Ordered-set direct args
	 * are not counted, though.
	 */
	int			numInputs;

	/*
	 * Number of aggregated input columns to pass to the transfn.  This
	 * includes the ORDER BY columns for ordered-set aggs, but not for plain
	 * aggs.  (This doesn't count the transition state value!)
	 */
	int			numTransInputs;

	/*  状态转换或合并函数的 Oid  */
	Oid			transfn_oid;

	/*  序列化函数的 Oid,或 InvalidOid  */
	Oid			serialfn_oid;

	/*  反序列化函数的 Oid,或 InvalidOid  */
	Oid			deserialfn_oid;

	/*  状态值类型的 Oid  */
	Oid			aggtranstype;

	/*
	 * fmgr lookup data for transition function or combine function.  Note in
	 * particular that the fn_strict flag is kept here.
	 */
	FmgrInfo	transfn;

	/*  序列化函数的 fmgr 查找数据  */
	FmgrInfo	serialfn;

	/*  反序列化函数的 fmgr 查找数据  */
	FmgrInfo	deserialfn;

	/*  聚合导出的输入排序规则  */
	Oid			aggCollation;

	/*  排序列数  */
	int			numSortCols;

	/*  DISTINCT 比较中要考虑的排序列数  */
	/*  (为零或等于 numSortCols)  */
	int			numDistinctCols;

	/*  解构后的排序信息(长度为 numSortCols 的数组)  */
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *sortCollations;
	bool	   *sortNullsFirst;

	/*
	 * Comparators for input columns --- only set/used when aggregate has
	 * DISTINCT flag. equalfnOne version is used for single-column
	 * comparisons, equalfnMulti for the case of multiple columns.
	 */
	FmgrInfo	equalfnOne;
	ExprState  *equalfnMulti;

	/*
	 * 来自 pg_aggregate 项的初值
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input and transition data
	 * types in order to know how to copy/delete values.
	 *
	 * Note that the info for the input type is used only when handling
	 * DISTINCT aggs with just one argument, so there is only one input type.
	 */
	int16		inputtypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				transtypeByVal;

	/*
	 * Slots for holding the evaluated input arguments.  These are set up
	 * during ExecInitAgg() and then used for each input row requiring either
	 * FILTER or ORDER BY/DISTINCT processing.
	 */
	TupleTableSlot *sortslot;	/* current input tuple */
	TupleTableSlot *uniqslot;	/*  用于多列 DISTINCT  */
	TupleDesc	sortdesc;		/*  输入元组的描述符  */
	Datum		lastdatum;		/*  用于单列 DISTINCT  */
	bool		lastisnull;		/*  用于单列 DISTINCT  */
	bool		haslast;		/*  已有 DISTINCT 上次值  */

	/*
	 * These values are working state that is initialized at the start of an
	 * input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT/ORDER BY) aggregate, we just feed the input
	 * values straight to the transition function.  If it's DISTINCT or
	 * requires ORDER BY, we pass the input values into a Tuplesort object;
	 * then at completion of the input tuple group, we scan the sorted values,
	 * eliminate duplicates if needed, and run the transition function on the
	 * rest.
	 *
	 * We need a separate tuplesort for each grouping set.
	 */

	Tuplesortstate **sortstates;	/*  排序对象,若 DISTINCT/ORDER BY  */

	/*
	 * This field is a pre-initialized FunctionCallInfo struct used for
	 * calling this aggregate's transfn.  We save a few cycles per row by not
	 * re-initializing the unchanging fields; which isn't much, but it seems
	 * worth the extra space consumption.
	 */
	FunctionCallInfo transfn_fcinfo;

	/*  序列化与反序列化函数同理  */
	FunctionCallInfo serialfn_fcinfo;

	FunctionCallInfo deserialfn_fcinfo;
}			AggStatePerTransData;

/*
 * AggStatePerAggData - per-aggregate information
 *
 * This contains the information needed to call the final function, to produce
 * a final aggregate result from the state value. If there are multiple
 * identical Aggrefs in the query, they can all share the same per-agg data.
 *
 * These values are set up during ExecInitAgg() and do not change thereafter.
 */
typedef struct AggStatePerAggData
{
	/*
	 * Link to an Aggref expr this state value is for.
	 *
	 * There can be multiple identical Aggref's sharing the same per-agg. This
	 * points to the first one of them.
	 */
	Aggref	   *aggref;

	/*  本聚合应使用的状态值下标  */
	int			transno;

	/*  终结函数的可选 Oid(可为 InvalidOid)  */
	Oid			finalfn_oid;

	/*
	 * fmgr lookup data for final function --- only valid when finalfn_oid is
	 * not InvalidOid.
	 */
	FmgrInfo	finalfn;

	/*
	 * Number of arguments to pass to the finalfn.  This is always at least 1
	 * (the transition state value) plus any ordered-set direct args. If the
	 * finalfn wants extra args then we pass nulls corresponding to the
	 * aggregated input columns.
	 */
	int			numFinalArgs;

	/*  任何直接参数表达式的 ExprStates  */
	List	   *aggdirectargs;

	/*
	 * We need the len and byval info for the agg's result data type in order
	 * to know how to copy/delete values.
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	/*
	 * "shareable" is false if this agg cannot share state values with other
	 * aggregates because the final function is read-write.
	 */
	bool		shareable;
}			AggStatePerAggData;

/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In AGG_PLAIN and AGG_SORTED modes, we have a single array of these
 * structs (pointed to by aggstate->pergroup); we re-use the array for
 * each input group, if it's AGG_SORTED mode.  In AGG_HASHED mode, the
 * hash table contains an array of these structs for each tuple group.
 *
 * Logically, the sortstate field belongs in this struct, but we do not
 * keep it here for space reasons: we don't support DISTINCT aggregates
 * in AGG_HASHED mode, so there's no reason to use up a pointer field
 * in every entry of the hashtable.
 */
typedef struct AggStatePerGroupData
{
#define FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUE 0
	Datum		transValue;		/*  当前转换值  */
#define FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL 1
	bool		transValueIsNull;

#define FIELDNO_AGGSTATEPERGROUPDATA_NOTRANSVALUE 2
	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.  They are not
	 * the same though: if transfn later returns a NULL, we want to keep that
	 * NULL and not auto-replace it with a later input value. Only the first
	 * non-NULL input will be auto-substituted.
	 */
}			AggStatePerGroupData;

/*
 * AggStatePerPhaseData - per-grouping-set-phase state
 *
 * Grouping sets are divided into "phases", where a single phase can be
 * processed in one pass over the input. If there is more than one phase, then
 * at the end of input from the current phase, state is reset and another pass
 * taken over the data which has been re-sorted in the mean time.
 *
 * Accordingly, each phase specifies a list of grouping sets and group clause
 * information, plus each phase after the first also has a sort order.
 */
typedef struct AggStatePerPhaseData
{
	AggStrategy aggstrategy;	/*  本阶段的策略  */
	int			numsets;		/*  分组集个数(或 0)  */
	int		   *gset_lengths;	/*  分组集长度  */
	Bitmapset **grouped_cols;	/*  rollup 的列分组  */
	ExprState **eqfunctions;	/*  返回相等性的表达式,按下标
								 * 列数下标  */
	Agg		   *aggnode;		/* Agg node for phase data */
	Sort	   *sortnode;		/*  阶段输入排序的 Sort 节点  */

	ExprState  *evaltrans;		/*  转换函数的求值    */

	/*----------
	 * Cached variants of the compiled expression.
	 * first subscript: 0: outerops; 1: TTSOpsMinimalTuple
	 * second subscript: 0: no NULL check; 1: with NULL check
	 *----------
	 */
	ExprState  *evaltrans_cache[2][2];
}			AggStatePerPhaseData;

/*
 * AggStatePerHashData - per-hashtable state
 *
 * When doing grouping sets with hashing, we have one of these for each
 * grouping set. (When doing hashing without grouping sets, we have just one of
 * them.)
 */
typedef struct AggStatePerHashData
{
	TupleHashTable hashtable;	/*  每组一项的哈希表  */
	TupleHashIterator hashiter; /*  用于遍历哈希表  */
	TupleTableSlot *hashslot;	/*  装入哈希表的槽  */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	Oid		   *eqfuncoids;		/*  每分组字段相等函数  */
	int			numCols;		/*  哈希键列数  */
	int			numhashGrpCols; /*  哈希表中的列数  */
	int			largestGrpColIdx;	/*  哈希所需的最大列  */
	AttrNumber *hashGrpColIdxInput; /*  输入槽哈希列下标  */
	AttrNumber *hashGrpColIdxHash;	/*  哈希表元组下标  */
	Agg		   *aggnode;		/*  原始 Agg 节点,供 numGroups 等  */
}			AggStatePerHashData;


extern AggState *ExecInitAgg(Agg *node, EState *estate, int eflags);
extern void ExecEndAgg(AggState *node);
extern void ExecReScanAgg(AggState *node);

extern Size hash_agg_entry_size(int numTrans, Size tupleWidth,
								Size transitionSpace);
extern void hash_agg_set_limits(double hashentrysize, double input_groups,
								int used_bits, Size *mem_limit,
								uint64 *ngroups_limit, int *num_partitions);

/*  并行插桩支持  */
extern void ExecAggEstimate(AggState *node, ParallelContext *pcxt);
extern void ExecAggInitializeDSM(AggState *node, ParallelContext *pcxt);
extern void ExecAggInitializeWorker(AggState *node, ParallelWorkerContext *pwcxt);
extern void ExecAggRetrieveInstrumentation(AggState *node);

#endif							/* NODEAGG_H */
