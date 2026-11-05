/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.c
 *	  处理 WindowAgg 节点的例程。
 *
 * WindowAgg 节点在输入元组集的合适分区求窗口函数值。  
 * 每个 WindowAgg 仅针对单个窗口规格工作，                    
 * 但也能对共享相同窗口规格的多个窗口函数求值。     
 * 输入元组必须按已排序的次序交付：PARTITION BY         
 *  列（若有）为主排序键，ORDER BY 列（若有）          
 * 为次排序键。                                                   
 * （若查询涉及多个窗口规格，规划器生成一叠 WindowAgg  
 * 节点，并在其间插入所需的 Sort 节点。）             
 *
 * 由于窗口函数可能要访问当前分区任意或全部的行， 
 * 我们会把该分区的行积累到一个 tuplestore 中。     
 * 窗口函数通过 WindowObject API 调用，                          
 * 以便它们能按需访问那些行。      
 *
 * 我们也支持把普通聚集函数用作窗口函数。           
 * 对这些函数，会为每个分区模拟常规的 Agg 节点环境。    
 * SQL 标准要求输出表示聚集函数对当前行窗口帧      
 * 内全部行求值的结果。                                     
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeWindowAgg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "executor/nodeWindowAgg.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "windowapi.h"

/*
 * 窗口函数的所有 API 都以该对象调用，该对象会作为     
 * fcinfo->context 传给窗口函数。  
 */
typedef struct WindowObjectData
{
	NodeTag		type;
	WindowAggState *winstate;	/* 父 WindowAggState    */
	List	   *argstates;		/* fn 各参数的 ExprState 树      */
	void	   *localmem;		/* WinGetPartitionLocalMemory 的块  */
	int			markptr;		/* 此 fn 的 tuplestore 标记指针  */
	int			readptr;		/* 此 fn 的 tuplestore 读指针     */
	int64		markpos;		/* markptr 所定位的行           */
	int64		seekpos;		/* readptr 所定位的行           */
} WindowObjectData;

/*
 * 本节点处理的每个窗口函数与窗口聚集各有一个   
 * WindowStatePerFunc 结构体。       
 */
typedef struct WindowStatePerFuncData
{
	/* 此工作状态对应的 WindowFunc expr 与 state 节点链接    */
	WindowFuncExprState *wfuncstate;
	WindowFunc *wfunc;

	int			numArguments;	/* 参数个数        */

	FmgrInfo	flinfo;			/* 窗口函数的 fmgr 查找数据    */

	Oid			winCollation;	/* 为窗口函数推导的排序规则  */

	/*
	 * 我们需要每个函数结果的 len 与 byval 信息，              
	 * 以确定如何复制/删除值。
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	bool		plain_agg;		/* 它只是普通聚集函数吗？      */
	int			aggno;			/* 若是，其 WindowStatePerAggData 下标 */

	WindowObject winobj;		/* 窗口函数 API 中使用的对象*/
}			WindowStatePerFuncData;

/*
 * 普通聚集窗口函数还会另有一个此结构。          
 */
typedef struct WindowStatePerAggData
{
	/* 状态转移函数的 Oid    */
	Oid			transfn_oid;
	Oid			invtransfn_oid; /* 可为 InvalidOid */
	Oid			finalfn_oid;	/* 可为 InvalidOid */

	/*
	 * 状态转移函数的 fmgr 查找数据 --- 仅当对应 oid 
	 * 不是 InvalidOid 时才有效。特别是 fn_strict                   
	 * 标志存于此。  
	 */
	FmgrInfo	transfn;
	FmgrInfo	invtransfn;
	FmgrInfo	finalfn;

	int			numFinalArgs;	/* 传给 finalfn 的参数个数         */

	/*
	 * 来自 pg_aggregate 条目的初值  
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * 当前帧边界的缓存值              
	 */
	Datum		resultValue;
	bool		resultValueIsNull;

	/*
	 * 我们需要聚集的输入、结果与转移类型的 len 与 
	 * byval 信息，以确定如何复制/删除值。                
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	int			wfuncno;		/* 关联 WindowStatePerFuncData 的下标    */

	/* 保存转移值以及可能的其他附属数据的上下文        */
	MemoryContext aggcontext;	/* 可为私有，或 winstate->aggcontext */

	/* 当前转移值          */
	Datum		transValue;		/* 当前转移值          */
	bool		transValueIsNull;

	int64		transValueCount;	/* 目前已聚集的行数            */

	/* eval_windowaggregates() 局部数据  */
	bool		restart;		/* 此周期内需要重启该聚集吗？ */
} WindowStatePerAggData;

static void initialize_windowaggregate(WindowAggState *winstate,
									   WindowStatePerFunc perfuncstate,
									   WindowStatePerAgg peraggstate);
static void advance_windowaggregate(WindowAggState *winstate,
									WindowStatePerFunc perfuncstate,
									WindowStatePerAgg peraggstate);
static bool advance_windowaggregate_base(WindowAggState *winstate,
										 WindowStatePerFunc perfuncstate,
										 WindowStatePerAgg peraggstate);
static void finalize_windowaggregate(WindowAggState *winstate,
									 WindowStatePerFunc perfuncstate,
									 WindowStatePerAgg peraggstate,
									 Datum *result, bool *isnull);

static void eval_windowaggregates(WindowAggState *winstate);
static void eval_windowfunction(WindowAggState *winstate,
								WindowStatePerFunc perfuncstate,
								Datum *result, bool *isnull);

static void begin_partition(WindowAggState *winstate);
static void spool_tuples(WindowAggState *winstate, int64 pos);
static void release_partition(WindowAggState *winstate);

static int	row_is_in_frame(WindowAggState *winstate, int64 pos,
							TupleTableSlot *slot);
static void update_frameheadpos(WindowAggState *winstate);
static void update_frametailpos(WindowAggState *winstate);
static void update_grouptailpos(WindowAggState *winstate);

static WindowStatePerAggData *initialize_peragg(WindowAggState *winstate,
												WindowFunc *wfunc,
												WindowStatePerAgg peraggstate);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

static bool are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
					  TupleTableSlot *slot2);
static bool window_gettupleslot(WindowObject winobj, int64 pos,
								TupleTableSlot *slot);


/*
 * initialize_windowaggregate
 * 对应 nodeAgg.c 的 initialize_aggregates    
 */
static void
initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate)
{
	MemoryContext oldContext;

	/*
	 * 若我们使用私有 aggcontext，可以在这里重置它。        
	 * 但如果上下文共享，我们不知道其他哪些聚集可能    
	 * 仍需要它，所以必须留给调用者在合适时机重置。    
	 */
	if (peraggstate->aggcontext != winstate->aggcontext)
		MemoryContextResetAndDeleteChildren(peraggstate->aggcontext);

	if (peraggstate->initValueIsNull)
		peraggstate->transValue = peraggstate->initValue;
	else
	{
		oldContext = MemoryContextSwitchTo(peraggstate->aggcontext);
		peraggstate->transValue = datumCopy(peraggstate->initValue,
											peraggstate->transtypeByVal,
											peraggstate->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;
	peraggstate->transValueCount = 0;
	peraggstate->resultValue = (Datum) 0;
	peraggstate->resultValueIsNull = true;
}

/*
 * advance_windowaggregate
 * 对应 nodeAgg.c 的 advance_aggregates    
 */
static void
advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;
	ExprState  *filter = wfuncstate->aggfilter;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* 跳过 FILTER 掉的东西 */
	if (filter)
	{
		bool		isnull;
		Datum		res = ExecEvalExpr(filter, econtext, &isnull);

		if (isnull || !DatumGetBool(res))
		{
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/* 从 1 开始，因为第 0 个参数将是转移值             */
	i = 1;
	foreach(arg, wfuncstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->args[i].value = ExecEvalExpr(argstate, econtext,
											 &fcinfo->args[i].isnull);
		i++;
	}

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * 对严格 transfn 来说，输入为 NULL 时什么都不做；    
		 * 只是保留先前的 transValue。注意 transValueCount    
		 * 也不变。  
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}

		/*
		 * 初值为 NULL 的严格转移函数，我们用第一个非      
		 * NULL 输入作为初始状态。（已检查过聚集的       
		 * 输入类型与其 transtype 二进制兼容，                   
		 * 直接复制即可。）     
		 *
		 * 若 datum 是按引用传递，必须把它复制进 aggcontext。 
		 * 无需 pfree 旧 transValue，因为它为 NULL。    
		 */
		if (peraggstate->transValueCount == 0 && peraggstate->transValueIsNull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			peraggstate->transValue = datumCopy(fcinfo->args[1].value,
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->transValueCount = 1;
			MemoryContextSwitchTo(oldContext);
			return;
		}

		if (peraggstate->transValueIsNull)
		{
			/*
			 * 不要用 NULL 输入调用严格函数。注意，即使 
			 * 有上述检查仍可能走到这里：若 transfn 是严格   
			 * *且* 在前一轮返回了 NULL，就会一路传播 NULL     
			 * 到结尾。这只会在没有逆向转移函数时发生，
			 * 因为当有逆向函数时，                                
			 * 我们禁止转回 NULL。                                   
			 */
			MemoryContextSwitchTo(oldContext);
			Assert(!OidIsValid(peraggstate->invtransfn_oid));
			return;
		}
	}

	/*
	 * 可以调用状态转移函数了。调用期间设置 winstate->     
	 * curaggcontext，供 AggCheckCallContext 使用。   
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->args[0].value = peraggstate->transValue;
	fcinfo->args[0].isnull = peraggstate->transValueIsNull;
	winstate->curaggcontext = peraggstate->aggcontext;
	newVal = FunctionCallInvoke(fcinfo);
	winstate->curaggcontext = NULL;

	/*
	 * 移动聚集状态转移函数不得返回 null，参见       
	 * advance_windowaggregate_base() 
	 */
	if (fcinfo->isnull && OidIsValid(peraggstate->invtransfn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("moving-aggregate transition function must not return null")));

	/*
	 * 必须跟踪 transValue 中包含的行数，因为要移除     
	 * 最后一个输入时，advance_windowaggregate_base()                
	 * 不得调用逆向转移函数，而只需把 transValue 重置回  
	 * 初值。     
	 */
	peraggstate->transValueCount++;

	/*
	 * 若类型为按引用传递，必须把新值复制进 aggcontext   
	 * 并释放先前的 transValue。但如果 transfn 返回的指针   
	 * 指向其第一个输入，则无需做任何事。另外，若 transfn
	 * 返回指向 R/W 展开对象且该对象已是 aggcontext     
	 * 的子对象，则假定无需复制即可采纳该值。（参见  
	 * ExecAggCopyTransValue 的注释，这段代码与之重复。）
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			if (DatumIsReadWriteExpandedObject(newVal,
											   false,
											   peraggstate->transtypeLen) &&
				MemoryContextGetParent(DatumGetEOHP(newVal)->eoh_context) == CurrentMemoryContext)
				 /* 无操作  */ ;
			else
				newVal = datumCopy(newVal,
								   peraggstate->transtypeByVal,
								   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
		{
			if (DatumIsReadWriteExpandedObject(peraggstate->transValue,
											   false,
											   peraggstate->transtypeLen))
				DeleteExpandedObject(peraggstate->transValue);
			else
				pfree(DatumGetPointer(peraggstate->transValue));
		}
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;
}

/*
 * advance_windowaggregate_base
 * 从聚集中移除最老的元组。        
 *
 * 这与 advance_windowaggregate 非常相似，区别在于我们调用  
 * 逆向转移函数（调用者必须已检查过其               
 * 可用）  
 *
 * 若成功把当前行从该聚集中移除则返回 true，      
 * 否则返回 false（后一种情况下，调用者负责         
 * 通过重启聚集来进行清理。）        
 */
static bool
advance_windowaggregate_base(WindowAggState *winstate,
							 WindowStatePerFunc perfuncstate,
							 WindowStatePerAgg peraggstate)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;
	ExprState  *filter = wfuncstate->aggfilter;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* 跳过 FILTER 掉的东西 */
	if (filter)
	{
		bool		isnull;
		Datum		res = ExecEvalExpr(filter, econtext, &isnull);

		if (isnull || !DatumGetBool(res))
		{
			MemoryContextSwitchTo(oldContext);
			return true;
		}
	}

	/* 从 1 开始，因为第 0 个参数将是转移值             */
	i = 1;
	foreach(arg, wfuncstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->args[i].value = ExecEvalExpr(argstate, econtext,
											 &fcinfo->args[i].isnull);
		i++;
	}

	if (peraggstate->invtransfn.fn_strict)
	{
		/*
		 * 对严格 (inv)transfn，输入为 NULL 时什么都不做； 
		 * 只是保留先前的 transValue。注意 transValueCount      
		 * 也不变。          
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				MemoryContextSwitchTo(oldContext);
				return true;
			}
		}
	}

	/* 应该仍有一个已加入但尚未移除的值         */
	Assert(peraggstate->transValueCount > 0);

	/*
	 * 在移动聚集模式下，状态绝不能为 NULL，除非可能是在
	 * 任何行被聚集之前（这在当前时点肯定不成立）。   
	 * 此限制使我们能把逆向函数返回的 NULL 结果解读为 
	 * "抱歉，此时无法执行逆向转移"。我们已经        
	 * 在 advance_windowaggregate 中检查过这一点， 
	 * 但为安全起见，再检查一次。                   
	 */
	if (peraggstate->transValueIsNull)
		elog(ERROR, "aggregate transition value is NULL before inverse transition");

	/*
	 * 不得用逆向状态转移函数来移除最后一个输入。  
	 * 那样做将得到非 NULL 的状态，而此后我们本应处于    
	 * 初始状态，而初始状态很可能是 NULL。因此在这种     
	 * 情况下我们直接重新初始化该聚集。   
	 */
	if (peraggstate->transValueCount == 1)
	{
		MemoryContextSwitchTo(oldContext);
		initialize_windowaggregate(winstate,
								   &winstate->perfunc[peraggstate->wfuncno],
								   peraggstate);
		return true;
	}

	/*
	 * 可调用逆向转移函数了。调用时设置
	 * winstate->curaggcontext，以供 AggCheckCallContext         
	 * 使用。           
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->invtransfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->args[0].value = peraggstate->transValue;
	fcinfo->args[0].isnull = peraggstate->transValueIsNull;
	winstate->curaggcontext = peraggstate->aggcontext;
	newVal = FunctionCallInvoke(fcinfo);
	winstate->curaggcontext = NULL;

	/*
	 * 若函数返回 NULL，则报告失败，强制重启。        
	 */
	if (fcinfo->isnull)
	{
		MemoryContextSwitchTo(oldContext);
		return false;
	}

	/* 更新 transValue 中包含的行数         */
	peraggstate->transValueCount--;

	/*
	 * 若类型为按引用传递，必须把新值复制进 aggcontext   
	 * 并释放先前的 transValue。但如果 invtransfn 返回的指针   
	 * 指向其第一个输入，则无需做事。若 invtransfn    
	 * 返回指向 R/W 展开对象且该对象已是 aggcontext            
	 * 的子对象，则假定无需复制即可采纳该值。（参见     
	 * ExecAggCopyTransValue 的注释，这段代码与之重复。）
	 *
	 * 注意：这里的空值检查永不触发，但让本段          
	 * 看起来与 advance_windowaggregate 完全一致似乎最好。
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			if (DatumIsReadWriteExpandedObject(newVal,
											   false,
											   peraggstate->transtypeLen) &&
				MemoryContextGetParent(DatumGetEOHP(newVal)->eoh_context) == CurrentMemoryContext)
				 /* 无操作  */ ;
			else
				newVal = datumCopy(newVal,
								   peraggstate->transtypeByVal,
								   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
		{
			if (DatumIsReadWriteExpandedObject(peraggstate->transValue,
											   false,
											   peraggstate->transtypeLen))
				DeleteExpandedObject(peraggstate->transValue);
			else
				pfree(DatumGetPointer(peraggstate->transValue));
		}
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;

	return true;
}

/*
 * finalize_windowaggregate
 * 对应 nodeAgg.c 的 finalize_aggregate    
 */
static void
finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * 若聚集提供 finalfn 则应用之，否则返回 transValue。  
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
		int			numFinalArgs = peraggstate->numFinalArgs;
		bool		anynull;
		int			i;

		InitFunctionCallInfoData(fcinfodata.fcinfo, &(peraggstate->finalfn),
								 numFinalArgs,
								 perfuncstate->winCollation,
								 (void *) winstate, NULL);
		fcinfo->args[0].value =
			MakeExpandedObjectReadOnly(peraggstate->transValue,
									   peraggstate->transValueIsNull,
									   peraggstate->transtypeLen);
		fcinfo->args[0].isnull = peraggstate->transValueIsNull;
		anynull = peraggstate->transValueIsNull;

		/* 把其余参数位置填为 NULL                 */
		for (i = 1; i < numFinalArgs; i++)
		{
			fcinfo->args[i].value = (Datum) 0;
			fcinfo->args[i].isnull = true;
			anynull = true;
		}

		if (fcinfo->flinfo->fn_strict && anynull)
		{
			/* 不要以 NULL 输入调用严格函数       */
			*result = (Datum) 0;
			*isnull = true;
		}
		else
		{
			Datum		res;

			winstate->curaggcontext = peraggstate->aggcontext;
			res = FunctionCallInvoke(fcinfo);
			winstate->curaggcontext = NULL;
			*isnull = fcinfo->isnull;
			*result = MakeExpandedObjectReadOnly(res,
												 fcinfo->isnull,
												 peraggstate->resulttypeLen);
		}
	}
	else
	{
		*result =
			MakeExpandedObjectReadOnly(peraggstate->transValue,
									   peraggstate->transValueIsNull,
									   peraggstate->transtypeLen);
		*isnull = peraggstate->transValueIsNull;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * eval_windowaggregates
 * 求值被用作窗口函数的普通聚集              
 *
 * 本函数与 nodeAgg.c 有两点不同。首先，若窗口帧的     
 * 起始位置移动，我们会用逆向状态转移函数（如果存在）
 * 从转移值中移除行。其次，我们预期在把更多数据    
 * 聚集到同一转移值上之后，能够反复调用聚集最终函   
 * 数。nodeAgg.c 并不要求                                            
 * 该行为 
 */
static void
eval_windowaggregates(WindowAggState *winstate)
{
	WindowStatePerAgg peraggstate;
	int			wfuncno,
				numaggs,
				numaggs_restart,
				i;
	int64		aggregatedupto_nonrestarted;
	MemoryContext oldContext;
	ExprContext *econtext;
	WindowObject agg_winobj;
	TupleTableSlot *agg_row_slot;
	TupleTableSlot *temp_slot;

	numaggs = winstate->numaggs;
	if (numaggs == 0)
		return;					/* 无需处理  */

	/* 最终输出执行在 ps_ExprContext 中    */
	econtext = winstate->ss.ps.ps_ExprContext;
	agg_winobj = winstate->agg_winobj;
	agg_row_slot = winstate->agg_row_slot;
	temp_slot = winstate->temp_slot_1;

	/*
	 * 如果窗口帧起始子句是 UNBOUNDED_PRECEDING，且没有  
	 * 指定排除子句，那么窗口帧就是一组从分区起点   
	 * 向前连续延伸的行；随着当前行不断向前推进， 
	 * 行只会进入帧，而绝不会退出帧。这使得可以对聚集  
	 * 的求值采用增量策略：每当有一行加入帧时，就对 
	 * 它运行一次状态转移函数，并且在需要当前聚集值的 
	 * 时候运行最终函数。比起对每个当前行重新执行    
	 * 一遍完整聚集计算的朴素做法，这样做要高效得多。 
	 * 不过它确实假定最终函数不会破坏正在运行中的       
	 * 转移值，但 nodeAgg.c 中也同样存在这一假设（当它    
	 * 重新扫描一张已有的哈希表的                                
	 * 时候）。                                           
	 *
	 * 如果帧起点有时确实会向前移动，那么每当相继的行 
	 * 共享同一帧头时，我们仍能像上面那样优化；        
	 * 但如果帧头越过了先前的帧头，我们就会尝试使用聚
	 * 集的逆向状态转移函数来移除那些行。该函数把聚  
	 * 集的当前状态恢复成被移除的行仿佛从未被聚集时  
	 * 的样子。逆向状态转移函数可选择返回 NULL，      
	 * 表示该函数无法在该情况下把该元组从聚集中移除。 
	 * 如果发生这种情况，或者该聚集完全不具有逆向状  
	 * 态转移函数，就必须对新帧边界内的所有元组重     
	 * 新从头执行一遍完整的                                       
	 * 聚集过程。      
	 *
	 * 如果存在任何排除子句，那么我们可能必须在不连续
	 * 的行集上做聚集，所以我们放弃优化，对每一行都重
	 * 新计算。（对某些帧末端选择，帧可能无论如     
	 * 何都是连续的，但那是日后研究的优化课题。）     
	 *
	 * 在很多常见情形中，多个行共享相同的帧，因而也共 
	 * 享相同的聚集值。（特别地，如果 RANGE 窗口中没有    
	 * ORDER BY，那么所有行都是同级元组，因此它们的窗      
	 * 口帧都等于整个分区。）我们对这类情形优化：在 
	 * 到达同级组的第一行时计算一次聚集值，然后对其   
	 * 其后所有行都返回保存的值。                
	 *
	 * 'aggregatedupto' 记录还未被累积进聚集转移值的        
	 * 第一个行。每当开始一个新的同级组时，我们就向前 
	 * 累积到该同级组的末尾。                                  
	 */

	/*
	 * 首先，更新帧头位置。        
	 *
	 * 帧头绝不应向后移动，下面的代码也无法应付这种情  
	 * 况，故为安全起见，若发生则报错。     
	 */
	update_frameheadpos(winstate);
	if (winstate->frameheadpos < winstate->aggregatedbase)
		elog(ERROR, "window frame head moved backward");

	/*
	 * 若帧与上一行相比没有变化，我们就可以重复使用此 
	 * 函数早先在末尾保存的结果值。由于我们不知道当
	 * 前帧的末端，这一点无法完全检查。但如果帧末端模  
	 * 式是 UNBOUNDED FOLLOWING 或 CURRENT ROW，未指定排           
	 * 除子句，当前行又位于上一行的帧之内，那么两个 
	 * 帧的末端就必定重合。注意在第一个被聚集的行上    
	 * aggregatedbase == aggregatedupto，即此测试必定失         
	 * 败，因此无需在此显式检查"不存在前一行"              
	 * 的情况。                                         
	 */
	if (winstate->aggregatedbase == winstate->frameheadpos &&
		(winstate->frameOptions & (FRAMEOPTION_END_UNBOUNDED_FOLLOWING |
								   FRAMEOPTION_END_CURRENT_ROW)) &&
		!(winstate->frameOptions & FRAMEOPTION_EXCLUSION) &&
		winstate->aggregatedbase <= winstate->currentpos &&
		winstate->aggregatedupto > winstate->currentpos)
	{
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];
			wfuncno = peraggstate->wfuncno;
			econtext->ecxt_aggvalues[wfuncno] = peraggstate->resultValue;
			econtext->ecxt_aggnulls[wfuncno] = peraggstate->resultValueIsNull;
		}
		return;
	}

	/*----------
	 * 初始化重启标志。 
	 *
	 * 重启聚集的情形：   
	 *	 - 若我们正在处理分区的第一行，或者      
	 *	 - 若帧头移动了且无法使用逆向状态         
	 *	   转移函数，或者  
	 *	 - 有 EXCLUSION 子句，或者  
	 *	 - 若新帧与旧帧不重叠                 
	 *
	 * 注意最后一种情况下并非严格需要重启，但如果     
	 * 反正要把所有行都从聚集中移除，那么重启            
	 * 肯定更快。  
	 *----------
	 */
	numaggs_restart = 0;
	for (i = 0; i < numaggs; i++)
	{
		peraggstate = &winstate->peragg[i];
		if (winstate->currentpos == 0 ||
			(winstate->aggregatedbase != winstate->frameheadpos &&
			 !OidIsValid(peraggstate->invtransfn_oid)) ||
			(winstate->frameOptions & FRAMEOPTION_EXCLUSION) ||
			winstate->aggregatedupto <= winstate->frameheadpos)
		{
			peraggstate->restart = true;
			numaggs_restart++;
		}
		else
			peraggstate->restart = false;
	}

	/*
	 * 若存在任何可能移动的聚集，就尝试前移       
	 * aggregatedbase 使之对齐帧头：把已从帧顶部掉出的     
	 * 输入行从聚集里移除。这一步可能失败，              
	 * 即 advance_windowaggregate_base() 返回 false，那么           
	 * 就会在下面重启该聚集。  
	 */
	while (numaggs_restart < numaggs &&
		   winstate->aggregatedbase < winstate->frameheadpos)
	{
		/*
		 * 取回待移除元组中的下一个。这绝不应失败，我们 
		 * 之前应该已经来过这里。  
		 */
		if (!window_gettupleslot(agg_winobj, winstate->aggregatedbase,
								 temp_slot))
			elog(ERROR, "could not re-fetch previously fetched frame row");

		/* 设置元组上下文，用于求值聚集参数        */
		winstate->tmpcontext->ecxt_outertuple = temp_slot;

		/*
		 * 对窗口中的每个聚集函数执行逆向转移，除非它  
		 * 已被标记为需要重启。                                 
		 */
		for (i = 0; i < numaggs; i++)
		{
			bool		ok;

			peraggstate = &winstate->peragg[i];
			if (peraggstate->restart)
				continue;

			wfuncno = peraggstate->wfuncno;
			ok = advance_windowaggregate_base(winstate,
											  &winstate->perfunc[wfuncno],
											  peraggstate);
			if (!ok)
			{
				/* 逆向状态转移函数已失败，必须重启     */
				peraggstate->restart = true;
				numaggs_restart++;
			}
		}

		/* 在处理完每个元组后重置其上下文  */
		ResetExprContext(winstate->tmpcontext);

		/* 并推进已聚集行的状态       */
		winstate->aggregatedbase++;
		ExecClearTuple(temp_slot);
	}

	/*
	 * 如果我们成功前移了所有聚集的基行，那么       
	 * aggregatedbase 现在等于 frameheadpos；但若任一聚集       
	 * 失败，就必须强行更新。   
	 */
	winstate->aggregatedbase = winstate->frameheadpos;

	/*
	 * 若我们为聚集创建了标记指针，就让它始终顶在帧头  
	 * 位置，以便 tuplestore 能丢弃不需要的行。
	 */
	if (agg_winobj->markptr >= 0)
		WinSetMarkPosition(agg_winobj, winstate->frameheadpos);

	/*
	 * 现在重启需要重启的聚集。       
	 *
	 * 我们假定使用共享上下文的聚集只要                    
	 * *任何*个聚集重启，它们就都重启，因此        
	 * 可以清理共享的 aggcontext。私有 aggcontext             
	 * 则由 initialize_windowaggregate() 在所属聚集重启时          
	 * 重置。若未重启某聚集，须释放此前为它保存的     
	 * 结果，否则会泄漏内存。     
	 */
	if (numaggs_restart > 0)
		MemoryContextResetAndDeleteChildren(winstate->aggcontext);
	for (i = 0; i < numaggs; i++)
	{
		peraggstate = &winstate->peragg[i];

		/* 共享 ctx 的聚集只要*任一*重启就必须重启        */
		Assert(peraggstate->aggcontext != winstate->aggcontext ||
			   numaggs_restart == 0 ||
			   peraggstate->restart);

		if (peraggstate->restart)
		{
			wfuncno = peraggstate->wfuncno;
			initialize_windowaggregate(winstate,
									   &winstate->perfunc[wfuncno],
									   peraggstate);
		}
		else if (!peraggstate->resultValueIsNull)
		{
			if (!peraggstate->resulttypeByVal)
				pfree(DatumGetPointer(peraggstate->resultValue));
			peraggstate->resultValue = (Datum) 0;
			peraggstate->resultValueIsNull = true;
		}
	}

	/*
	 * 未重启的聚集现在包含 aggregatedbase（即                 
	 * frameheadpos）到 aggregatedupto 之间的行，而重启过的聚
	 * 集则不包含任何行。如果存在重启过的聚集，我们就
	 * 必须从 frameheadpos 重新开始聚集，否则只需从    
	 * aggregatedupto 处继续。必须记住原                    
	 * aggregatedupto 的旧值，以便知道非重启聚集要跳过
	 * 多长的前移。若修改了 aggregatedupto，就必须   
	 * 清除 agg_row_slot（见下方不变式）
	 */
	aggregatedupto_nonrestarted = winstate->aggregatedupto;
	if (numaggs_restart > 0 &&
		winstate->aggregatedupto != winstate->frameheadpos)
	{
		winstate->aggregatedupto = winstate->frameheadpos;
		ExecClearTuple(agg_row_slot);
	}

	/*
	 * 前移直到到达不在帧内的行（或分区末尾）。    
	 *
	 * 注意循环不变式：agg_row_slot 要么为空，要么保存着   
	 * aggregatedupto 位置上的行。处理完一行之后就前移        
	 * 它。
	 */
	for (;;)
	{
		int			ret;

		/* 若尚未取行则取下一行      */
		if (TupIsNull(agg_row_slot))
		{
			if (!window_gettupleslot(agg_winobj, winstate->aggregatedupto,
									 agg_row_slot))
				break;			/* 必定是分区末尾    */
		}

		/*
		 * 若不可能再有行在帧内就退出循环。如果当前   
		 * 行不在帧内但帧内可能还有行，则跳过聚集。     
		 */
		ret = row_is_in_frame(winstate, winstate->aggregatedupto, agg_row_slot);
		if (ret < 0)
			break;
		if (ret == 0)
			goto next_tuple;

		/* 为聚合参数的求值设置元组上下文           */
		winstate->tmpcontext->ecxt_outertuple = agg_row_slot;

		/* 把当前行累积进各聚合     */
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];

			/* 非重启聚合一直跳到 aggregatedupto_nonrestarted   */
			if (!peraggstate->restart &&
				winstate->aggregatedupto < aggregatedupto_nonrestarted)
				continue;

			wfuncno = peraggstate->wfuncno;
			advance_windowaggregate(winstate,
									&winstate->perfunc[wfuncno],
									peraggstate);
		}

next_tuple:
		/* 每个元组之后重置输入元组上下文  */
		ResetExprContext(winstate->tmpcontext);

		/* 并推进已聚合行的状态       */
		winstate->aggregatedupto++;
		ExecClearTuple(agg_row_slot);
	}

	/* 帧尾在任何情况下都不应向后移动           */
	Assert(aggregatedupto_nonrestarted <= winstate->aggregatedupto);

	/*
	 * 终结各聚合并填充 result/isnull 字段。  
	 */
	for (i = 0; i < numaggs; i++)
	{
		Datum	   *result;
		bool	   *isnull;

		peraggstate = &winstate->peragg[i];
		wfuncno = peraggstate->wfuncno;
		result = &econtext->ecxt_aggvalues[wfuncno];
		isnull = &econtext->ecxt_aggnulls[wfuncno];
		finalize_windowaggregate(winstate,
								 &winstate->perfunc[wfuncno],
								 peraggstate,
								 result, isnull);

		/*
		 * 保存结果，以防下一行共用同一帧。       
		 *
		 * XXX 在某些帧模式（如 ROWS/END_CURRENT_ROW）下可预先  
		 * 得知下一行绝不可能共用同一帧。这是否值得     
		 * 检测并跳过这段代码？                 
		 */
		if (!peraggstate->resulttypeByVal && !*isnull)
		{
			oldContext = MemoryContextSwitchTo(peraggstate->aggcontext);
			peraggstate->resultValue =
				datumCopy(*result,
						  peraggstate->resulttypeByVal,
						  peraggstate->resulttypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		else
		{
			peraggstate->resultValue = *result;
		}
		peraggstate->resultValueIsNull = *isnull;
	}
}

/*
 * eval_windowfunction
 *
 * 窗口函数参数不在此处求值，因为窗口函数可能需要 
 * 对分区内任意元组进行随机访问。窗口函数使用    
 * 专用的 WinGetFuncArgInPartition 和                           
 * WinGetFuncArgInFrame 函数为其所需元组求取这些参数      
 * 的值。
 */
static void
eval_windowfunction(WindowAggState *winstate, WindowStatePerFunc perfuncstate,
					Datum *result, bool *isnull)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * 不给窗口函数传递普通参数，但会传递参数的个数，  
	 * 使窗口函数实现能支持可变参数数量。           
	 * 真正的信息则通过 WindowObject 传递，                        
	 * 后者经 fcinfo->context 传入。                                
	 */
	InitFunctionCallInfoData(*fcinfo, &(perfuncstate->flinfo),
							 perfuncstate->numArguments,
							 perfuncstate->winCollation,
							 (void *) perfuncstate->winobj, NULL);
	/* 以防万一，把所有常规参数槽都置为空       */
	for (int argno = 0; argno < perfuncstate->numArguments; argno++)
		fcinfo->args[argno].isnull = true;
	/* 窗口函数同样没有当前的聚合上下文                */
	winstate->curaggcontext = NULL;

	*result = FunctionCallInvoke(fcinfo);
	*isnull = fcinfo->isnull;

	/*
	 * 窗口函数可能返回按引用传递的结果，它只是指向 
	 * WindowObject 某个临时槽的指针。如果它是唯一使用该   
	 * WindowObject 的窗口函数，那没问题；但若使用它的函数
	 * 不止一个，最好把结果复制一份，以免被后续的窗口
	 * 函数破坏。                                     
	 */
	if (!perfuncstate->resulttypeByVal && !fcinfo->isnull &&
		winstate->numfuncs > 1)
		*result = datumCopy(*result,
							perfuncstate->resulttypeByVal,
							perfuncstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * begin_partition
 * 开始缓冲下一个分区的各行。    
 */
static void
begin_partition(WindowAggState *winstate)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	PlanState  *outerPlan = outerPlanState(winstate);
	int			frameOptions = winstate->frameOptions;
	int			numfuncs = winstate->numfuncs;
	int			i;

	winstate->partition_spooled = false;
	winstate->framehead_valid = false;
	winstate->frametail_valid = false;
	winstate->grouptail_valid = false;
	winstate->spooled_rows = 0;
	winstate->currentpos = 0;
	winstate->frameheadpos = 0;
	winstate->frametailpos = 0;
	winstate->currentgroup = 0;
	winstate->frameheadgroup = 0;
	winstate->frametailgroup = 0;
	winstate->groupheadpos = 0;
	winstate->grouptailpos = -1;	/* 见 update_grouptailpos */
	ExecClearTuple(winstate->agg_row_slot);
	if (winstate->framehead_slot)
		ExecClearTuple(winstate->framehead_slot);
	if (winstate->frametail_slot)
		ExecClearTuple(winstate->frametail_slot);

	/*
	 * 若是第一个分区，则需取入第一条输入行，存入      
	 * first_part_slot 中。          
	 */
	if (TupIsNull(winstate->first_part_slot))
	{
		TupleTableSlot *outerslot = ExecProcNode(outerPlan);

		if (!TupIsNull(outerslot))
			ExecCopySlot(winstate->first_part_slot, outerslot);
		else
		{
			/* 外层计划为空，因此无事可做       */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			return;
		}
	}

	/* 为该分区新建 tuplestore            */
	winstate->buffer = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * 为 tuplestore 设置读指针。当前指针不需要 BACKWARD      
	 * 能力，但每个窗口函数各自的读指针则都需要，而且  
	 * 若我们可能需要重启聚合，聚合指针也需要。           
	 */
	winstate->current_ptr = 0;	/* 读指针 0 是预分配的     */

	/* 为当前指针重置默认的 REWIND 能力位     */
	tuplestore_set_eflags(winstate->buffer, 0);

	/* 如有需要，为聚合创建读指针        */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = winstate->agg_winobj;
		int			readptr_flags = 0;

		/*
		 * 若帧头可能移动，或者我们带有 EXCLUSION 子句，   
		 * 那么就可能需要重启聚合……         
		 */
		if (!(frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) ||
			(frameOptions & FRAMEOPTION_EXCLUSION))
		{
			/* ……因此创建一个标记指针来跟踪帧头  */
			agg_winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer, 0);
			/* 而读指针将需要 BACKWARD 能力              */
			readptr_flags |= EXEC_FLAG_BACKWARD;
		}

		agg_winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
															readptr_flags);
		agg_winobj->markpos = -1;
		agg_winobj->seekpos = -1;

		/* 同时重置各聚合的行计数器       */
		winstate->aggregatedbase = 0;
		winstate->aggregatedupto = 0;
	}

	/* 为每个真正的窗口函数创建标记指针和读指针*/
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		if (!perfuncstate->plain_agg)
		{
			WindowObject winobj = perfuncstate->winobj;

			winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer,
															0);
			winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
															EXEC_FLAG_BACKWARD);
			winobj->markpos = -1;
			winobj->seekpos = -1;
		}
	}

	/*
	 * 若我们处于 RANGE 或 GROUPS 模式，确定帧边界就需要  
	 * 物理访问帧端点行，某些退化情形除外。创建读指    
	 * 针指向这些行，以简化访问，并确保 tuplestore 不会   
	 * 过早丢弃这些端点行。（必须在与                     
	 *  update_frameheadpos 和 update_frametailpos 需要                  
	 * 指针的情形完全一致的情况下创建它们。）         
	 */
	winstate->framehead_ptr = winstate->frametail_ptr = -1; /* 如未使用*/

	if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
	{
		if (((frameOptions & FRAMEOPTION_START_CURRENT_ROW) &&
			 node->ordNumCols != 0) ||
			(frameOptions & FRAMEOPTION_START_OFFSET))
			winstate->framehead_ptr =
				tuplestore_alloc_read_pointer(winstate->buffer, 0);
		if (((frameOptions & FRAMEOPTION_END_CURRENT_ROW) &&
			 node->ordNumCols != 0) ||
			(frameOptions & FRAMEOPTION_END_OFFSET))
			winstate->frametail_ptr =
				tuplestore_alloc_read_pointer(winstate->buffer, 0);
	}

	/*
	 * 若带有需要知道当前行同级组边界的排除子句，我们 
	 * 就创建读指针来跟踪同级组的尾部位置（即下一    
	 * 个同级组的首行）。头部无需专门指针，因为     
	 * 我们在推进当前行的过程中                                  
	 * 会顺带维护它。                                       
	 */
	winstate->grouptail_ptr = -1;

	if ((frameOptions & (FRAMEOPTION_EXCLUDE_GROUP |
						 FRAMEOPTION_EXCLUDE_TIES)) &&
		node->ordNumCols != 0)
	{
		winstate->grouptail_ptr =
			tuplestore_alloc_read_pointer(winstate->buffer, 0);
	}

	/*
	 * 把第一个元组存入 tuplestore（它现在总是可用的；    
	 * 我们要么在上面读过，要么在上个分区末尾存了它） 
	 */
	tuplestore_puttupleslot(winstate->buffer, winstate->first_part_slot);
	winstate->spooled_rows++;
}

/*
 * 从外层节点读取元组，直到并包含位置 'pos'，然后      
 * 存入 tuplestore。若 pos 为 -1，则读取整个分区。           
 */
static void
spool_tuples(WindowAggState *winstate, int64 pos)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	PlanState  *outerPlan;
	TupleTableSlot *outerslot;
	MemoryContext oldcontext;

	if (!winstate->buffer)
		return;					/* 只是安全检查  */
	if (winstate->partition_spooled)
		return;					/* 整个分区早已读完     */

	/*
	 * 直通模式下可直接耗尽当前分区的全部元组。           
	 * 后续的窗口函数求值不再需要这些元组，但如果       
	 * 我们不是顶层窗口，就必须保留它们，因为上层     
	 * 的另一个 WindowAgg 节点必须看到它们。              
	 */
	if (winstate->status != WINDOWAGG_RUN)
	{
		Assert(winstate->status == WINDOWAGG_PASSTHROUGH ||
			   winstate->status == WINDOWAGG_PASSTHROUGH_STRICT);

		pos = -1;
	}

	/*
	 * 若 tuplestore 已溢出到磁盘，读写交替进行会因        
	 * 频繁刷新缓冲区而变得相当昂贵。把整个分区一次   
	 * 性全部写入会更便宜。                         
	 *
	 * XXX 这是糟糕的权宜之计——最好在 tuplestore 内部    
	 * 解决性能问题。FIXME       
	 */
	else if (!tuplestore_in_memory(winstate->buffer))
		pos = -1;

	outerPlan = outerPlanState(winstate);

	/* 调用 outerplan 必须在查询上下文  */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	while (winstate->spooled_rows <= pos || pos == -1)
	{
		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
		{
			/* 已到达最后一个分区的末尾  */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			break;
		}

		if (node->partNumCols > 0)
		{
			ExprContext *econtext = winstate->tmpcontext;

			econtext->ecxt_innertuple = winstate->first_part_slot;
			econtext->ecxt_outertuple = outerslot;

			/* 检查该元组是否仍属于当前分区                 */
			if (!ExecQualAndReset(winstate->partEqfunction, econtext))
			{
				/*
				 * 分区结束；为下一轮循环复制该元组。 
				 */
				ExecCopySlot(winstate->first_part_slot, outerslot);
				winstate->partition_spooled = true;
				winstate->more_partitions = true;
				break;
			}
		}

		/*
		 * 记住该元组，除非我们是顶层窗口且处于           
		 * 直通模式。   
		 */
		if (winstate->status != WINDOWAGG_PASSTHROUGH_STRICT)
		{
			/* 仍在分区内，因此把它保存进 tuplestore */
			tuplestore_puttupleslot(winstate->buffer, outerslot);
			winstate->spooled_rows++;
		}
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * release_partition
 * 清除分区内保存的信息，包括             
 * tuplestore 与聚合结果。    
 */
static void
release_partition(WindowAggState *winstate)
{
	int			i;

	for (i = 0; i < winstate->numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		/* 释放此窗口函数的任何分区局部状态          */
		if (perfuncstate->winobj)
			perfuncstate->winobj->localmem = NULL;
	}

	/*
	 * 释放所有分区局部内存（特别是上面循环中可能已经 
	 * 失去指针指向的分区局部状态，以及任何聚合临时数  
	 * 据）。我们不依赖逐个 pfree，因为某些聚合可能      
	 * 分配了我们没有直接指针指向的数据。                   
	 */
	MemoryContextResetAndDeleteChildren(winstate->partcontext);
	MemoryContextResetAndDeleteChildren(winstate->aggcontext);
	for (i = 0; i < winstate->numaggs; i++)
	{
		if (winstate->peragg[i].aggcontext != winstate->aggcontext)
			MemoryContextResetAndDeleteChildren(winstate->peragg[i].aggcontext);
	}

	if (winstate->buffer)
		tuplestore_end(winstate->buffer);
	winstate->buffer = NULL;
	winstate->partition_spooled = false;
}

/*
 * row_is_in_frame
 * 按窗口帧规则判断某行是否属于当前行的窗口帧       
 * 内。                    
 *
 * 调用者必须已确定该行在分区内，且已将其取入一个   
 * 槽中。本函数只是封装这些帧规                              
 * 则。
 *
 * Returns:
 * -1，若该行在帧外且后续行不可能在帧内                
 * 0，若该行在帧外但后续行可能在帧内                  
 * 1，若该行在帧内   
 *
 * 可能改写 winstate->temp_slot_2
 */
static int
row_is_in_frame(WindowAggState *winstate, int64 pos, TupleTableSlot *slot)
{
	int			frameOptions = winstate->frameOptions;

	Assert(pos >= 0);			/* 否则调用出错*/

	/*
	 * 首先检查帧起始条件。我们不妨把这件事总是交给     
	 * update_frameheadpos 处理；不会增加任何明显开销。 
	 */
	update_frameheadpos(winstate);
	if (pos < winstate->frameheadpos)
		return 0;

	/*
	 * 前面没问题，现在检查帧结束条件。这里我们避免在  
	 * 简单情形下调用 update_frametailpos，以免把元组预读     
	 * 得超过所需。   
	 */
	if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* 当前行之后的行都在帧外       */
			if (pos > winstate->currentpos)
				return -1;
		}
		else if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
		{
			/* 后续非同级元组的行在帧外           */
			if (pos > winstate->currentpos &&
				!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
				return -1;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_OFFSET)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			/* 当前行 + offset 之后的行在帧外         */
			if (frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
				offset = -offset;

			if (pos > winstate->currentpos + offset)
				return -1;
		}
		else if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
		{
			/* 困难情形，交给 update_frametailpos      */
			update_frametailpos(winstate);
			if (pos >= winstate->frametailpos)
				return -1;
		}
		else
			Assert(false);
	}

	/* 检查排除子句     */
	if (frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW)
	{
		if (pos == winstate->currentpos)
			return 0;
	}
	else if ((frameOptions & FRAMEOPTION_EXCLUDE_GROUP) ||
			 ((frameOptions & FRAMEOPTION_EXCLUDE_TIES) &&
			  pos != winstate->currentpos))
	{
		WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;

		/* 若无 ORDER BY，所有行互为同级元组      */
		if (node->ordNumCols == 0)
			return 0;
		/* 否则检查组边界                 */
		if (pos >= winstate->groupheadpos)
		{
			update_grouptailpos(winstate);
			if (pos < winstate->grouptailpos)
				return 0;
		}
	}

	/* 若走到这里，则在帧内*/
	return 1;
}

/*
 * update_frameheadpos
 * 使 frameheadpos 对当前行有效        
 *
 * 注意 frameheadpos 的计算不考虑任何窗口排除子句；         
 * 当前行和/或其同级元组被视为帧的一部分，即使它们   
 * 随后必须被排除。                             
 *
 * 可能改写 winstate->temp_slot_2
 */
static void
update_frameheadpos(WindowAggState *winstate)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;
	MemoryContext oldcontext;

	if (winstate->framehead_valid)
		return;					/* 当前行的值已求出      */

	/* 调用时可能处于短暂上下文      */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
	{
		/* UNBOUNDED PRECEDING 模式下帧头总是行 0          */
		winstate->frameheadpos = 0;
		winstate->framehead_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* ROWS 模式下帧头与当前行相同          */
			winstate->frameheadpos = winstate->currentpos;
			winstate->framehead_valid = true;
		}
		else if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
		{
			/* 若无 ORDER BY，所有行互为同级元组      */
			if (node->ordNumCols == 0)
			{
				winstate->frameheadpos = 0;
				winstate->framehead_valid = true;
				MemoryContextSwitchTo(oldcontext);
				return;
			}

			/*
			 * 在 RANGE 或 GROUPS START_CURRENT_ROW 模式下，帧头   
			 * 是当前行的同级元组中最前面的一行。我们在   
			 * framehead_slot 中保留最后已知帧头行，并按需  
			 * 推进。注意若到达分区末尾，我们将留下     
			 * frameheadpos = end+1 且 framehead_slot 为空。   
			 */
			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->framehead_ptr);
			if (winstate->frameheadpos == 0 &&
				TupIsNull(winstate->framehead_slot))
			{
				/* 若尚未取过，把第一行取入 framehead_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->framehead_slot))
			{
				if (are_peers(winstate, winstate->framehead_slot,
							  winstate->ss.ss_ScanTupleSlot))
					break;		/* 此行正是正确的帧头        */
				/* 注意即使取行失败我们也推进 frameheadpos */
				winstate->frameheadpos++;
				spool_tuples(winstate, winstate->frameheadpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					break;		/* 分区末尾     */
			}
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_OFFSET)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* ROWS 模式下边界物理上是当前行前/后第 n 行*/
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			if (frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
				offset = -offset;

			winstate->frameheadpos = winstate->currentpos + offset;
			/* 帧头不能位于第一行之前    */
			if (winstate->frameheadpos < 0)
				winstate->frameheadpos = 0;
			else if (winstate->frameheadpos > winstate->currentpos + 1)
			{
				/* 确保 frameheadpos 不越过分区末尾           */
				spool_tuples(winstate, winstate->frameheadpos - 1);
				if (winstate->frameheadpos > winstate->spooled_rows)
					winstate->frameheadpos = winstate->spooled_rows;
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/*
			 * 在 RANGE START_OFFSET 模式下，帧头是相对当前    
			 * 行满足 in_range 约束的第一行。我们把最后已知 
			 * 的帧头行保留在 framehead_slot，按需     
			 * 推进。注意若到达分区末尾，我们将留下    
			 * frameheadpos = end+1 且 framehead_slot                       
			 * 为空。            
			 */
			int			sortCol = node->ordColIdx[0];
			bool		sub,
						less;

			/* 我们必须有排序列        */
			Assert(node->ordNumCols == 1);

			/* 为 in_range 检查预计算标志   */
			if (frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
				sub = true;		/* 从当前行减去 startOffset        */
			else
				sub = false;	/* 加上 */
			less = false;		/* 通常我们希望帧头 >= sum     */
			/* 若排序为降序，则翻转两个标志   */
			if (!winstate->inRangeAsc)
			{
				sub = !sub;
				less = true;
			}

			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->framehead_ptr);
			if (winstate->frameheadpos == 0 &&
				TupIsNull(winstate->framehead_slot))
			{
				/* 若尚未取过，把第一行取入 framehead_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->framehead_slot))
			{
				Datum		headval,
							currval;
				bool		headisnull,
							currisnull;

				headval = slot_getattr(winstate->framehead_slot, sortCol,
									   &headisnull);
				currval = slot_getattr(winstate->ss.ss_ScanTupleSlot, sortCol,
									   &currisnull);
				if (headisnull || currisnull)
				{
					/* 行的顺序仅取决于 nulls_first          */
					if (winstate->inRangeNullsFirst)
					{
						/* 若头行为空且当前行非空则推进头*/
						if (!headisnull || currisnull)
							break;
					}
					else
					{
						/* 若头行非空且当前行为空则推进头     */
						if (headisnull || !currisnull)
							break;
					}
				}
				else
				{
					if (DatumGetBool(FunctionCall5Coll(&winstate->startInRangeFunc,
													   winstate->inRangeColl,
													   headval,
													   currval,
													   winstate->startOffsetValue,
													   BoolGetDatum(sub),
													   BoolGetDatum(less))))
						break;	/* 此行正是正确的帧头        */
				}
				/* 注意即使取行失败我们也推进 frameheadpos */
				winstate->frameheadpos++;
				spool_tuples(winstate, winstate->frameheadpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					break;		/* 分区末尾     */
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_GROUPS)
		{
			/*
			 * 在 GROUPS START_OFFSET 模式下，帧头是编号满足      
			 * 偏移约束的第一个同级组首行。把最后已知帧  
			 * 头行保留在 framehead_slot，按需           
			 * 推进。注意若到达分区末尾，我们将留下    
			 * frameheadpos = end+1 且 framehead_slot                       
			 * 为空。            
			 */
			int64		offset = DatumGetInt64(winstate->startOffsetValue);
			int64		minheadgroup;

			if (frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
				minheadgroup = winstate->currentgroup - offset;
			else
				minheadgroup = winstate->currentgroup + offset;

			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->framehead_ptr);
			if (winstate->frameheadpos == 0 &&
				TupIsNull(winstate->framehead_slot))
			{
				/* 若尚未取得，把第一行取入 framehead_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->framehead_slot))
			{
				if (winstate->frameheadgroup >= minheadgroup)
					break;		/* 此行即为正确的帧头        */
				ExecCopySlot(winstate->temp_slot_2, winstate->framehead_slot);
				/* 注意即使获取失败也会推进 frameheadpos    */
				winstate->frameheadpos++;
				spool_tuples(winstate, winstate->frameheadpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->framehead_slot))
					break;		/* 分区末尾     */
				if (!are_peers(winstate, winstate->temp_slot_2,
							   winstate->framehead_slot))
					winstate->frameheadgroup++;
			}
			ExecClearTuple(winstate->temp_slot_2);
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else
		Assert(false);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * update_frametailpos
 * 使 frametailpos 对当前行有效        
 *
 * 注意 frametailpos 的计算不考虑任何窗口排除子句；         
 * 当前行和/或其同级元组仍视为帧的一部分，即使之后   
 * 必须将它们排除。                             
 *
 * 可能覆盖 winstate->temp_slot_2
 */
static void
update_frametailpos(WindowAggState *winstate)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;
	MemoryContext oldcontext;

	if (winstate->frametail_valid)
		return;					/* 当前行已算出此值      */

	/* 可能在短寿命上下文中被调用   */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
	{
		/* UNBOUNDED FOLLOWING 模式下，分区所有行均在帧内  */
		spool_tuples(winstate, -1);
		winstate->frametailpos = winstate->spooled_rows;
		winstate->frametail_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* ROWS 模式下，帧恰好包含到当前行为止的行  */
			winstate->frametailpos = winstate->currentpos + 1;
			winstate->frametail_valid = true;
		}
		else if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
		{
			/* 若无 ORDER BY，所有行彼此互为同级元组*/
			if (node->ordNumCols == 0)
			{
				spool_tuples(winstate, -1);
				winstate->frametailpos = winstate->spooled_rows;
				winstate->frametail_valid = true;
				MemoryContextSwitchTo(oldcontext);
				return;
			}

			/*
			 * RANGE 或 GROUPS END_CURRENT_ROW 模式下，帧末端是     
			 * 当前行同级元组中的最后一行，帧尾是其后的  
			 * 一行（如有）。我们把最近已知帧尾行保存在   
			 * frametail_slot 中，并按需推进。注意如果我们    
			 * 到达分区末尾，我们会让 frametailpos = end+1，且  
			 * frametail_slot 置空
			 */
			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->frametail_ptr);
			if (winstate->frametailpos == 0 &&
				TupIsNull(winstate->frametail_slot))
			{
				/* 若尚未取得，把第一行取入 frametail_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->frametail_slot))
			{
				if (winstate->frametailpos > winstate->currentpos &&
					!are_peers(winstate, winstate->frametail_slot,
							   winstate->ss.ss_ScanTupleSlot))
					break;		/* 此行即帧尾            */
				/* 注意即使获取失败也会推进 frametailpos    */
				winstate->frametailpos++;
				spool_tuples(winstate, winstate->frametailpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					break;		/* 分区末尾     */
			}
			winstate->frametail_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_OFFSET)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* ROWS 模式：边界物理上为当前行前/后 n 行   */
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			if (frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
				offset = -offset;

			winstate->frametailpos = winstate->currentpos + offset + 1;
			/* frametailpos 的最小允许值为 0          */
			if (winstate->frametailpos < 0)
				winstate->frametailpos = 0;
			else if (winstate->frametailpos > winstate->currentpos + 1)
			{
				/* 确保 frametailpos 不超过分区末尾           */
				spool_tuples(winstate, winstate->frametailpos - 1);
				if (winstate->frametailpos > winstate->spooled_rows)
					winstate->frametailpos = winstate->spooled_rows;
			}
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/*
			 * 在 RANGE END_OFFSET 模式下，帧末端是满足     
			 * in_range 约束的最后一行（相对当前行），帧尾  
			 * 是其后的行（如有）。我们把最近已知帧尾行 
			 * 保存在 frametail_slot 中，并按需推进。注意如果  
			 * 到达分区末尾，就让 frametailpos = end+1，并     
			 * 使 frametail_slot 置空。                        
			 */
			int			sortCol = node->ordColIdx[0];
			bool		sub,
						less;

			/* 必须有一个排序列        */
			Assert(node->ordNumCols == 1);

			/* 预计算 in_range 检查的标志   */
			if (frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
				sub = true;		/* 从当前行中减去 endOffset     */
			else
				sub = false;	/* 相加 */
			less = true;		/* 通常我们希望帧尾 <= sum     */
			/* 若为降序排序，翻转两个标志      */
			if (!winstate->inRangeAsc)
			{
				sub = !sub;
				less = false;
			}

			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->frametail_ptr);
			if (winstate->frametailpos == 0 &&
				TupIsNull(winstate->frametail_slot))
			{
				/* 若尚未取得，把第一行取入 frametail_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->frametail_slot))
			{
				Datum		tailval,
							currval;
				bool		tailisnull,
							currisnull;

				tailval = slot_getattr(winstate->frametail_slot, sortCol,
									   &tailisnull);
				currval = slot_getattr(winstate->ss.ss_ScanTupleSlot, sortCol,
									   &currisnull);
				if (tailisnull || currisnull)
				{
					/* 行的次序仅取决于 nulls_first          */
					if (winstate->inRangeNullsFirst)
					{
						/* tail 空或 curr 非空时推进帧尾      */
						if (!tailisnull)
							break;
					}
					else
					{
						/* tail 非空或 curr 空时推进帧尾           */
						if (!currisnull)
							break;
					}
				}
				else
				{
					if (!DatumGetBool(FunctionCall5Coll(&winstate->endInRangeFunc,
														winstate->inRangeColl,
														tailval,
														currval,
														winstate->endOffsetValue,
														BoolGetDatum(sub),
														BoolGetDatum(less))))
						break;	/* 此行即为正确的帧尾        */
				}
				/* 注意即使获取失败也会推进 frametailpos    */
				winstate->frametailpos++;
				spool_tuples(winstate, winstate->frametailpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					break;		/* 分区末尾     */
			}
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_GROUPS)
		{
			/*
			 * 在 GROUPS END_OFFSET 模式下，帧末端是             
			 * 编号满足偏移约束的最后一个同级组中          
			 * 的最后一行，帧尾是其后的行（如有）。把     
			 * 最近已知帧尾行保存在 frametail_slot 中，并按需  
			 * 推进。注意如果我们到达分区末尾，就让        
			 * frametailpos = end+1 且 frametail_slot 置空。   
			 */
			int64		offset = DatumGetInt64(winstate->endOffsetValue);
			int64		maxtailgroup;

			if (frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
				maxtailgroup = winstate->currentgroup - offset;
			else
				maxtailgroup = winstate->currentgroup + offset;

			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->frametail_ptr);
			if (winstate->frametailpos == 0 &&
				TupIsNull(winstate->frametail_slot))
			{
				/* 若尚未取得，把第一行取入 frametail_slot       */
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					elog(ERROR, "unexpected end of tuplestore");
			}

			while (!TupIsNull(winstate->frametail_slot))
			{
				if (winstate->frametailgroup > maxtailgroup)
					break;		/* 此行即为正确的帧尾        */
				ExecCopySlot(winstate->temp_slot_2, winstate->frametail_slot);
				/* 注意即使获取失败也会推进 frametailpos    */
				winstate->frametailpos++;
				spool_tuples(winstate, winstate->frametailpos);
				if (!tuplestore_gettupleslot(winstate->buffer, true, true,
											 winstate->frametail_slot))
					break;		/* 分区末尾     */
				if (!are_peers(winstate, winstate->temp_slot_2,
							   winstate->frametail_slot))
					winstate->frametailgroup++;
			}
			ExecClearTuple(winstate->temp_slot_2);
			winstate->frametail_valid = true;
		}
		else
			Assert(false);
	}
	else
		Assert(false);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * update_grouptailpos
 * 使 grouptailpos 对当前行有效        
 *
 * 可能覆盖 winstate->temp_slot_2
 */
static void
update_grouptailpos(WindowAggState *winstate)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	MemoryContext oldcontext;

	if (winstate->grouptail_valid)
		return;					/* 当前行已算出此值      */

	/* 可能在短寿命上下文中被调用   */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	/* 若无 ORDER BY，所有行彼此互为同级元组*/
	if (node->ordNumCols == 0)
	{
		spool_tuples(winstate, -1);
		winstate->grouptailpos = winstate->spooled_rows;
		winstate->grouptail_valid = true;
		MemoryContextSwitchTo(oldcontext);
		return;
	}

	/*
	 * 由于 grouptail_valid 仅在当前行进入新同级组时重置，  
	 * 所以我们到达此处时总知道 grouptailpos 至少需要         
	 * 推进至少一行。因此，与帧尾跟踪的类似情形不同，
	 * 我们不需要为组尾行保留持久存储的                      
	 * 必要。      
	 */
	Assert(winstate->grouptailpos <= winstate->currentpos);
	tuplestore_select_read_pointer(winstate->buffer,
								   winstate->grouptail_ptr);
	for (;;)
	{
		/* 注意即使获取失败也会推进 grouptailpos    */
		winstate->grouptailpos++;
		spool_tuples(winstate, winstate->grouptailpos);
		if (!tuplestore_gettupleslot(winstate->buffer, true, true,
									 winstate->temp_slot_2))
			break;				/* 分区末尾     */
		if (winstate->grouptailpos > winstate->currentpos &&
			!are_peers(winstate, winstate->temp_slot_2,
					   winstate->ss.ss_ScanTupleSlot))
			break;				/* 此行即组尾            */
	}
	ExecClearTuple(winstate->temp_slot_2);
	winstate->grouptail_valid = true;

	MemoryContextSwitchTo(oldcontext);
}


/* -----------------
 * ExecWindowAgg
 *
 *	ExecWindowAgg 从外层子计划接收元组并存入   
 *	tuplestore，然后处理窗口函数。该节点不减少、  
 *	不限定任何行，返回行数与外层子计划结果    
 *	完全相同。                                                 
 * -----------------
 */
static TupleTableSlot *
ExecWindowAgg(PlanState *pstate)
{
	WindowAggState *winstate = castNode(WindowAggState, pstate);
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			i;
	int			numfuncs;

	CHECK_FOR_INTERRUPTS();

	if (winstate->status == WINDOWAGG_DONE)
		return NULL;

	/*
	 * 首次调用（或重扫描）时计算帧偏移值（如有）。
	 * 这些值假定在整个扫描期间不变；若用户给出的     
	 * 是易变表达式，我们将只使用其初始值。                
	 */
	if (winstate->all_first)
	{
		int			frameOptions = winstate->frameOptions;
		Datum		value;
		bool		isnull;
		int16		len;
		bool		byval;

		econtext = winstate->ss.ps.ps_ExprContext;

		if (frameOptions & FRAMEOPTION_START_OFFSET)
		{
			Assert(winstate->startOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->startOffset,
											  econtext,
											  &isnull);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame starting offset must not be null")));
			/* 将值拷入查询寿命上下文      */
			get_typlenbyval(exprType((Node *) winstate->startOffset->expr),
							&len, &byval);
			winstate->startOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & (FRAMEOPTION_ROWS | FRAMEOPTION_GROUPS))
			{
				/* 值已知为 int8         */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
							 errmsg("frame starting offset must not be negative")));
			}
		}
		if (frameOptions & FRAMEOPTION_END_OFFSET)
		{
			Assert(winstate->endOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->endOffset,
											  econtext,
											  &isnull);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame ending offset must not be null")));
			/* 将值拷入查询寿命上下文      */
			get_typlenbyval(exprType((Node *) winstate->endOffset->expr),
							&len, &byval);
			winstate->endOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & (FRAMEOPTION_ROWS | FRAMEOPTION_GROUPS))
			{
				/* 值已知为 int8         */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
							 errmsg("frame ending offset must not be negative")));
			}
		}
		winstate->all_first = false;
	}

	/* 需要循环，因为 runCondition 或 qual 可能过滤掉元组 */
	for (;;)
	{
		if (winstate->buffer == NULL)
		{
			/* 为第一个分区初始化，并置当前行 = 0      */
			begin_partition(winstate);
			/* 如果没有输入行，我们会在下面检测到并退出 */
		}
		else
		{
			/* 在分区内推进当前行          */
			winstate->currentpos++;
			/* 这也可能意味着帧随之移动      */
			winstate->framehead_valid = false;
			winstate->frametail_valid = false;
			/* 无需在此失效 grouptail；见下文              */
		}

		/*
		 * 如果尚未写过，则把直到并包含当前行的所有元组 
		 * 写入 
		 */
		spool_tuples(winstate, winstate->currentpos);

		/* 如果我们已到达本分区末尾，则移动到下一个分区 */
		if (winstate->partition_spooled &&
			winstate->currentpos >= winstate->spooled_rows)
		{
			release_partition(winstate);

			if (winstate->more_partitions)
			{
				begin_partition(winstate);
				Assert(winstate->spooled_rows > 0);

				/* 切换分区时退出透传模式                     */
				winstate->status = WINDOWAGG_RUN;
			}
			else
			{
				/* 无更多分区？那就结束     */
				winstate->status = WINDOWAGG_DONE;
				return NULL;
			}
		}

		/* 最终输出执行位于 ps_ExprContext 中 */
		econtext = winstate->ss.ps.ps_ExprContext;

		/* 清空当前行所用的每输出元组上下文   */
		ResetExprContext(econtext);

		/*
		 * 从 tuplestore 中读取当前行并保存到        
		 * ScanTupleSlot。（不能依赖外层计划的输出槽，  
		 * 因为可能需要读取当前行之后的行。另外，我们还
		 * 必须把该行实际从 tuplestore 中拷出，因为窗口
		 * 函数求值可能促使 tuplestore 把其状态转储到磁       
		 * 盘。
		 *
		 * 在 GROUPS 模式下或跟踪面向组的排除子句时，必须  
		 * 检测进入新同级组，并在发生时更新相关的状态。 
		 * 我们使用 temp_slot_2 来临时保存                        
		 * 前一行，用于此目的。    
		 *
		 * 当前行必然在 tuplestore 中，上面已把它写入。     
		 */
		tuplestore_select_read_pointer(winstate->buffer, winstate->current_ptr);
		if ((winstate->frameOptions & (FRAMEOPTION_GROUPS |
									   FRAMEOPTION_EXCLUDE_GROUP |
									   FRAMEOPTION_EXCLUDE_TIES)) &&
			winstate->currentpos > 0)
		{
			ExecCopySlot(winstate->temp_slot_2, winstate->ss.ss_ScanTupleSlot);
			if (!tuplestore_gettupleslot(winstate->buffer, true, true,
										 winstate->ss.ss_ScanTupleSlot))
				elog(ERROR, "unexpected end of tuplestore");
			if (!are_peers(winstate, winstate->temp_slot_2,
						   winstate->ss.ss_ScanTupleSlot))
			{
				winstate->currentgroup++;
				winstate->groupheadpos = winstate->currentpos;
				winstate->grouptail_valid = false;
			}
			ExecClearTuple(winstate->temp_slot_2);
		}
		else
		{
			if (!tuplestore_gettupleslot(winstate->buffer, true, true,
										 winstate->ss.ss_ScanTupleSlot))
				elog(ERROR, "unexpected end of tuplestore");
		}

		/* 透传模式下不求值窗口函数                                */
		if (winstate->status == WINDOWAGG_RUN)
		{
			/*
			 * 求值真正的窗口函数   
			 */
			numfuncs = winstate->numfuncs;
			for (i = 0; i < numfuncs; i++)
			{
				WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

				if (perfuncstate->plain_agg)
					continue;
				eval_windowfunction(winstate, perfuncstate,
									&(econtext->ecxt_aggvalues[perfuncstate->wfuncstate->wfuncno]),
									&(econtext->ecxt_aggnulls[perfuncstate->wfuncstate->wfuncno]));
			}

			/*
			 * 求值各聚合    
			 */
			if (winstate->numaggs > 0)
				eval_windowaggregates(winstate);
		}

		/*
		 * 如果已为帧或组边界创建了辅助读指针，则强制  
		 * 它们保持最新，因为我们不知道窗口函数会不会    
		 * 做需要它的任何事情。不推进这些指针会导致无法 
		 * 从 tuplestore 中修剪数据，这很糟。（如果我们能
		 * 预先得知窗口函数是否会使用帧边界信息，就      
		 * 能一开始就跳过创建这些指针……但                  
		 * 遗憾的是窗口函数 API 并不要求                       
		 * 这一点。）                                             
		 */
		if (winstate->framehead_ptr >= 0)
			update_frameheadpos(winstate);
		if (winstate->frametail_ptr >= 0)
			update_frametailpos(winstate);
		if (winstate->grouptail_ptr >= 0)
			update_grouptailpos(winstate);

		/*
		 * 从 tuplestore 中截断不再需要的任何行。    
		 */
		tuplestore_trim(winstate->buffer);

		/*
		 * 用窗口函数结果与当前行组成投影元组并返回。    
		 * 设置 ecxt_outertuple 可确保任何 Var 都相对于该行   
		 * 来求值。                               
		 */
		econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;

		slot = ExecProject(winstate->ss.ps.ps_ProjInfo);

		if (winstate->status == WINDOWAGG_RUN)
		{
			econtext->ecxt_scantuple = slot;

			/*
			 * 现在求值 runcondition，看我们是否需要进入    
			 * 透传模式，或者干脆完全停止。  
			 */
			if (!ExecQual(winstate->runcondition, econtext))
			{
				/*
				 * 确定要进入哪种模式。如果没有        
				 * PARTITION BY 子句且我们为顶层 WindowAgg，那么  
				 * 就完成了。该元组及后续元组均不可能 
				 * 匹配 runcondition。但当有 PARTITION BY 子句或    
				 * 不是顶层窗口时，都不能就此停止，因为  
				 * 要么要处理其他分区，要么要确保     
				 * 上层 WindowAgg 节点收到处理 WindowFuncs 所需   
				 * 的全部元组。                                   
				 */
				if (winstate->use_pass_through)
				{
					/*
					 * 切换到透传模式时最好把聚合结果设为 
					 * NULL，因为它们不再更新；置 NULL 可避免
					 * 旧结果滞留。部分可能是 byref 类型  
					 * 不能让它们指向已释放内存。规划器要求
					 * runcondition 中使用的 qual 必须严格，       
					 * 因此顶层 WindowAgg 总会在                    
					 * filter 子句中过滤掉这些                   
					 * NULL。                                     
					 */
					numfuncs = winstate->numfuncs;
					for (i = 0; i < numfuncs; i++)
					{
						econtext->ecxt_aggvalues[i] = (Datum) 0;
						econtext->ecxt_aggnulls[i] = true;
					}

					/*
					 * 有 PARTITION BY 子句时，顶层窗口需要严格  
					 * （STRICT）的透传模式。否则就必须存储    
					 * 不匹配 runcondition 的元组，供     
					 * 上层 WindowAgg 使用。                            
					 */
					if (winstate->top_window)
					{
						winstate->status = WINDOWAGG_PASSTHROUGH_STRICT;
						continue;
					}
					else
					{
						winstate->status = WINDOWAGG_PASSTHROUGH;
					}
				}
				else
				{
					/*
					 * 无需透传。我们可以直接返回 NULL。     
					 * 再无任何行会匹配 runcondition。 
					 */
					winstate->status = WINDOWAGG_DONE;
					return NULL;
				}
			}

			/*
			 * 在顶层 WindowAgg 中过滤掉不需要的任何元组。    
			 */
			if (!ExecQual(winstate->ss.ps.qual, econtext))
			{
				InstrCountFiltered1(winstate, 1);
				continue;
			}

			break;
		}

		/*
		 * 不在 WINDOWAGG_RUN 模式时，只要我们不是顶层         
		 * 窗口就必须仍然返回该元组。  
		 */
		else if (!winstate->top_window)
			break;
	}

	return slot;
}

/* -----------------
 * ExecInitWindowAgg
 *
 *	为由规划器产生的 WindowAgg 节点创建运行时信息，      
 *	并初始化其外层子树              
 * -----------------
 */
WindowAggState *
ExecInitWindowAgg(WindowAgg *node, EState *estate, int eflags)
{
	WindowAggState *winstate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	WindowStatePerFunc perfunc;
	WindowStatePerAgg peragg;
	int			frameOptions = node->frameOptions;
	int			numfuncs,
				wfuncno,
				numaggs,
				aggno;
	TupleDesc	scanDesc;
	ListCell   *l;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	winstate = makeNode(WindowAggState);
	winstate->ss.ps.plan = (Plan *) node;
	winstate->ss.ps.state = estate;
	winstate->ss.ps.ExecProcNode = ExecWindowAgg;

	/* 把帧选项拷贝到状态节点以便于访问 */
	winstate->frameOptions = frameOptions;

	/*
	 * 创建表达式上下文。需要两个：一个用于每输入  
	 * 元组处理，另一个用于每输出元组处理。我们          
	 * 取巧复用 ExecAssignExprContext() 建两者 
	 */
	ExecAssignExprContext(estate, &winstate->ss.ps);
	tmpcontext = winstate->ss.ps.ps_ExprContext;
	winstate->tmpcontext = tmpcontext;
	ExecAssignExprContext(estate, &winstate->ss.ps);

	/* 为分区局部内存等存储创建长寿命上下文              */
	winstate->partcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg Partition",
							  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 为聚合过渡值等创建中寿命上下文。        
	 *
	 * 注意每个移动聚合都使用自己的私有上下文而非    
	 * 此。   
	 */
	winstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg Aggregates",
							  ALLOCSET_DEFAULT_SIZES);

	/* 只有顶层 WindowAgg 才可能有 qual     */
	Assert(node->plan.qual == NIL || node->topWindow);

	/* 初始化该 qual   */
	winstate->ss.ps.qual = ExecInitQual(node->plan.qual,
										(PlanState *) winstate);

	/*
	 * 设置运行条件（如果我们从查询规划器收到了       
	 * 的话）。设置后，这可能让我们得以进入透传         
	 * 模式，从而不必再对当前分区中的 WindowFunc          
	 * 做进一步求值；或者当所有元组都在同一分区           
	 * 中时完全停止返回元组。
	 */
	winstate->runcondition = ExecInitQual(node->runCondition,
										  (PlanState *) winstate);

	/*
	 * 若本节点不是顶层 WindowAgg 节点，或虽是但带有  
	 * PARTITION BY 子句，则在 runCondition 变假后必须              
	 * 改用 WINDOWAGG_PASSTHROUGH* 模式。   
	 */
	winstate->use_pass_through = !node->topWindow || node->partNumCols > 0;

	/* 记住本窗口是顶层窗口还是位于顶层窗口之下    */
	winstate->top_window = node->topWindow;

	/*
	 * 初始化子节点    
	 */
	outerPlan = outerPlan(node);
	outerPlanState(winstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * 初始化源元组类型（它也是我们存入 tuplestore、       
	 * 并在所有工作 slot 中使用的元组类型）。    
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &winstate->ss, &TTSOpsMinimalTuple);
	scanDesc = winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/* 外层元组不是子节点的元组，而总是最小元组        */
	winstate->ss.ps.outeropsset = true;
	winstate->ss.ps.outerops = &TTSOpsMinimalTuple;
	winstate->ss.ps.outeropsfixed = true;

	/*
	 * 元组表初始化        
	 */
	winstate->first_part_slot = ExecInitExtraTupleSlot(estate, scanDesc,
													   &TTSOpsMinimalTuple);
	winstate->agg_row_slot = ExecInitExtraTupleSlot(estate, scanDesc,
													&TTSOpsMinimalTuple);
	winstate->temp_slot_1 = ExecInitExtraTupleSlot(estate, scanDesc,
												   &TTSOpsMinimalTuple);
	winstate->temp_slot_2 = ExecInitExtraTupleSlot(estate, scanDesc,
												   &TTSOpsMinimalTuple);

	/*
	 * 仅在有需要时才创建帧头与帧尾 slot（创建情形必须 
	 * 与 update_frameheadpos 及 update_frametailpos                        
	 * 一致)   
	 */
	winstate->framehead_slot = winstate->frametail_slot = NULL;

	if (frameOptions & (FRAMEOPTION_RANGE | FRAMEOPTION_GROUPS))
	{
		if (((frameOptions & FRAMEOPTION_START_CURRENT_ROW) &&
			 node->ordNumCols != 0) ||
			(frameOptions & FRAMEOPTION_START_OFFSET))
			winstate->framehead_slot = ExecInitExtraTupleSlot(estate, scanDesc,
															  &TTSOpsMinimalTuple);
		if (((frameOptions & FRAMEOPTION_END_CURRENT_ROW) &&
			 node->ordNumCols != 0) ||
			(frameOptions & FRAMEOPTION_END_OFFSET))
			winstate->frametail_slot = ExecInitExtraTupleSlot(estate, scanDesc,
															  &TTSOpsMinimalTuple);
	}

	/*
	 * 初始化结果 slot、类型与投影。   
	 */
	ExecInitResultTupleSlotTL(&winstate->ss.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&winstate->ss.ps, NULL);

	/* 设置用于比较元组的数据*/
	if (node->partNumCols > 0)
		winstate->partEqfunction =
			execTuplesMatchPrepare(scanDesc,
								   node->partNumCols,
								   node->partColIdx,
								   node->partOperators,
								   node->partCollations,
								   &winstate->ss.ps);

	if (node->ordNumCols > 0)
		winstate->ordEqfunction =
			execTuplesMatchPrepare(scanDesc,
								   node->ordNumCols,
								   node->ordColIdx,
								   node->ordOperators,
								   node->ordCollations,
								   &winstate->ss.ps);

	/*
	 * WindowAgg 节点与 Agg 一样使用 aggvalues 和 aggnulls。  
	 */
	numfuncs = winstate->numfuncs;
	numaggs = winstate->numaggs;
	econtext = winstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numfuncs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numfuncs);

	/*
	 * 分配每个 wfunc/agg 的状态信息。    
	 */
	perfunc = (WindowStatePerFunc) palloc0(sizeof(WindowStatePerFuncData) * numfuncs);
	peragg = (WindowStatePerAgg) palloc0(sizeof(WindowStatePerAggData) * numaggs);
	winstate->perfunc = perfunc;
	winstate->peragg = peragg;

	wfuncno = -1;
	aggno = -1;
	foreach(l, winstate->funcs)
	{
		WindowFuncExprState *wfuncstate = (WindowFuncExprState *) lfirst(l);
		WindowFunc *wfunc = wfuncstate->wfunc;
		WindowStatePerFunc perfuncstate;
		AclResult	aclresult;
		int			i;

		if (wfunc->winref != node->winref)	/* 规划器搞错了  */
			elog(ERROR, "WindowFunc with winref %u assigned to WindowAgg with winref %u",
				 wfunc->winref, node->winref);

		/* 查找先前已有的重复窗口函数       */
		for (i = 0; i <= wfuncno; i++)
		{
			if (equal(wfunc, perfunc[i].wfunc) &&
				!contain_volatile_functions((Node *) wfunc))
				break;
		}
		if (i <= wfuncno)
		{
			/* 找到与既有条目匹配项，只需加以标记 */
			wfuncstate->wfuncno = i;
			continue;
		}

		/* 没有，分配新 PerAgg 记录    */
		perfuncstate = &perfunc[++wfuncno];

		/* 在 WindowFunc 状态节点上标记结果数组中分到的下标 */
		wfuncstate->wfuncno = wfuncno;

		/* 检查调用窗口函数的权限        */
		aclresult = object_aclcheck(ProcedureRelationId, wfunc->winfnoid, GetUserId(),
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION,
						   get_func_name(wfunc->winfnoid));
		InvokeFunctionExecuteHook(wfunc->winfnoid);

		/* 填写 perfuncstate 数据    */
		perfuncstate->wfuncstate = wfuncstate;
		perfuncstate->wfunc = wfunc;
		perfuncstate->numArguments = list_length(wfuncstate->args);
		perfuncstate->winCollation = wfunc->inputcollid;

		get_typlenbyval(wfunc->wintype,
						&perfuncstate->resulttypeLen,
						&perfuncstate->resulttypeByVal);

		/*
		 * 若它其实只是普通聚合函数，我们将为其模拟     
		 * Agg 环境。          
		 */
		perfuncstate->plain_agg = wfunc->winagg;
		if (wfunc->winagg)
		{
			WindowStatePerAgg peraggstate;

			perfuncstate->aggno = ++aggno;
			peraggstate = &winstate->peragg[aggno];
			initialize_peragg(winstate, wfunc, peraggstate);
			peraggstate->wfuncno = wfuncno;
		}
		else
		{
			WindowObject winobj = makeNode(WindowObjectData);

			winobj->winstate = winstate;
			winobj->argstates = wfuncstate->args;
			winobj->localmem = NULL;
			perfuncstate->winobj = winobj;

			/* 这是真正的窗口函数，故安排调用。   */
			fmgr_info_cxt(wfunc->winfnoid, &perfuncstate->flinfo,
						  econtext->ecxt_per_query_memory);
			fmgr_info_set_expr((Node *) wfunc, &perfuncstate->flinfo);
		}
	}

	/* 更新 numfuncs、numaggs 以匹配找到的唯一函数数量     */
	winstate->numfuncs = wfuncno + 1;
	winstate->numaggs = aggno + 1;

	/* 若有需要，为聚合设置 WindowObject   */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = makeNode(WindowObjectData);

		agg_winobj->winstate = winstate;
		agg_winobj->argstates = NIL;
		agg_winobj->localmem = NULL;
		/* 确保 markptr = -1 使其失效，可能不会被用到  */
		agg_winobj->markptr = -1;
		agg_winobj->readptr = -1;
		winstate->agg_winobj = agg_winobj;
	}

	/* 把状态置为运行中  */
	winstate->status = WINDOWAGG_RUN;

	/* 初始化帧边界偏移表达式         */
	winstate->startOffset = ExecInitExpr((Expr *) node->startOffset,
										 (PlanState *) winstate);
	winstate->endOffset = ExecInitExpr((Expr *) node->endOffset,
									   (PlanState *) winstate);

	/* 若有需要则查找 in_range 支持函数 */
	if (OidIsValid(node->startInRangeFunc))
		fmgr_info(node->startInRangeFunc, &winstate->startInRangeFunc);
	if (OidIsValid(node->endInRangeFunc))
		fmgr_info(node->endInRangeFunc, &winstate->endInRangeFunc);
	winstate->inRangeColl = node->inRangeColl;
	winstate->inRangeAsc = node->inRangeAsc;
	winstate->inRangeNullsFirst = node->inRangeNullsFirst;

	winstate->all_first = true;
	winstate->partition_spooled = false;
	winstate->more_partitions = false;

	return winstate;
}

/* -----------------
 * ExecEndWindowAgg
 * -----------------
 */
void
ExecEndWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan;
	int			i;

	release_partition(node);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);
	if (node->framehead_slot)
		ExecClearTuple(node->framehead_slot);
	if (node->frametail_slot)
		ExecClearTuple(node->frametail_slot);

	/*
	 * 释放表达式上下文    
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	for (i = 0; i < node->numaggs; i++)
	{
		if (node->peragg[i].aggcontext != node->aggcontext)
			MemoryContextDelete(node->peragg[i].aggcontext);
	}
	MemoryContextDelete(node->partcontext);
	MemoryContextDelete(node->aggcontext);

	pfree(node->perfunc);
	pfree(node->peragg);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

/* -----------------
 * ExecReScanWindowAgg
 * -----------------
 */
void
ExecReScanWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	node->status = WINDOWAGG_RUN;
	node->all_first = true;

	/* 释放 tuplestore 等    */
	release_partition(node);

	/* 释放所有临时元组，尤其是 first_part_slot    */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);
	if (node->framehead_slot)
		ExecClearTuple(node->framehead_slot);
	if (node->frametail_slot)
		ExecClearTuple(node->frametail_slot);

	/* 忘掉当前 wfunc 值      */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numfuncs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numfuncs);

	/*
	 * 若子节点的 chgParam 非空，则首次 ExecProcNode          
	 * 便会重扫计划 
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/*
 * initialize_peragg
 *
 * 与 nodeAgg.c 中做法几乎相同，但目前不支持 DISTINCT。    
 */
static WindowStatePerAggData *
initialize_peragg(WindowAggState *winstate, WindowFunc *wfunc,
				  WindowStatePerAgg peraggstate)
{
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments;
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggtranstype;
	AttrNumber	initvalAttNo;
	AclResult	aclresult;
	bool		use_ma_code;
	Oid			transfn_oid,
				invtransfn_oid,
				finalfn_oid;
	bool		finalextra;
	char		finalmodify;
	Expr	   *transfnexpr,
			   *invtransfnexpr,
			   *finalfnexpr;
	Datum		textInitVal;
	int			i;
	ListCell   *lc;

	numArguments = list_length(wfunc->args);

	i = 0;
	foreach(lc, wfunc->args)
	{
		inputTypes[i++] = exprType((Node *) lfirst(lc));
	}

	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(wfunc->winfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 wfunc->winfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	/*
	 * 判断是否要使用移动聚合实现，并从 pg_aggregate         
	 * 条目中收集所需的字段集。                            
	 *
	 * 有可能某个聚合同时提供安全的移动聚合实现        
	 * 和不安全的普通实现，这时我们别无选择。         
	 * 否则若帧头不能移动，就无需移动聚合代码。   
	 * 即便想用它，若聚合参数（以及 FILTER 子句，若有）   
	 * 中含有易变函数调用，就不要使用。                     
	 * 否则，重新开始与不重新开始聚合之间的差别         
	 * 会被用户察觉。                                
	 *
	 * 当参数或 FILTER 子句中含有子计划时，我们也不       
	 * 冒险使用移动聚合。部分原因是             
	 * contain_volatile_functions() 不会查看子计划内部；         
	 * 但此外还有别的原因会让子计划的输出易变。    
	 * 例如，syncscan 模式可使结果不可重复。         
	 */
	if (!OidIsValid(aggform->aggminvtransfn))
		use_ma_code = false;	/* 必需条件 */
	else if (aggform->aggmfinalmodify == AGGMODIFY_READ_ONLY &&
			 aggform->aggfinalmodify != AGGMODIFY_READ_ONLY)
		use_ma_code = true;		/* 安全所迫的决定     */
	else if (winstate->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
		use_ma_code = false;	/* 帧头不可移动    */
	else if (contain_volatile_functions((Node *) wfunc))
		use_ma_code = false;	/* 避免可能的行为改变      */
	else if (contain_subplans((Node *) wfunc))
		use_ma_code = false;	/* 子计划可能含有易变函数         */
	else
		use_ma_code = true;		/* 好，就使用它*/
	if (use_ma_code)
	{
		peraggstate->transfn_oid = transfn_oid = aggform->aggmtransfn;
		peraggstate->invtransfn_oid = invtransfn_oid = aggform->aggminvtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggmfinalfn;
		finalextra = aggform->aggmfinalextra;
		finalmodify = aggform->aggmfinalmodify;
		aggtranstype = aggform->aggmtranstype;
		initvalAttNo = Anum_pg_aggregate_aggminitval;
	}
	else
	{
		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->invtransfn_oid = invtransfn_oid = InvalidOid;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;
		finalextra = aggform->aggfinalextra;
		finalmodify = aggform->aggfinalmodify;
		aggtranstype = aggform->aggtranstype;
		initvalAttNo = Anum_pg_aggregate_agginitval;
	}

	/*
	 * ExecInitWindowAgg 已检查过调用聚合函数的权限，           
	 * ...但仍需检查成员函数。                     
	 */

	/* 检查聚合所有者是否有权调用成员函数             */
	{
		HeapTuple	procTuple;
		Oid			aggOwner;

		procTuple = SearchSysCache1(PROCOID,
									ObjectIdGetDatum(wfunc->winfnoid));
		if (!HeapTupleIsValid(procTuple))
			elog(ERROR, "cache lookup failed for function %u",
				 wfunc->winfnoid);
		aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
		ReleaseSysCache(procTuple);

		aclresult = object_aclcheck(ProcedureRelationId, transfn_oid, aggOwner,
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION,
						   get_func_name(transfn_oid));
		InvokeFunctionExecuteHook(transfn_oid);

		if (OidIsValid(invtransfn_oid))
		{
			aclresult = object_aclcheck(ProcedureRelationId, invtransfn_oid, aggOwner,
										ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FUNCTION,
							   get_func_name(invtransfn_oid));
			InvokeFunctionExecuteHook(invtransfn_oid);
		}

		if (OidIsValid(finalfn_oid))
		{
			aclresult = object_aclcheck(ProcedureRelationId, finalfn_oid, aggOwner,
										ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FUNCTION,
							   get_func_name(finalfn_oid));
			InvokeFunctionExecuteHook(finalfn_oid);
		}
	}

	/*
	 * 若所选的 finalfn 不是只读的，就不能把该聚合当        
	 * 窗口函数来运行。这是面向用户的错误，因此对错误 
	 * 信息的处理要比本函数其他地方更仔细。      
	 */
	if (finalmodify != AGGMODIFY_READ_ONLY)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("aggregate function %s does not support use as a window function",
						format_procedure(wfunc->winfnoid))));

	/* 确定要传给 finalfn 的参数个数          */
	if (finalextra)
		peraggstate->numFinalArgs = numArguments + 1;
	else
		peraggstate->numFinalArgs = 1;

	/* 若为多态，解析转换状态的实际类型        */
	aggtranstype = resolve_aggregate_transtype(wfunc->winfnoid,
											   aggtranstype,
											   inputTypes,
											   numArguments);

	/* 用实际参数与结果类型构建表达式树            */
	build_aggregate_transfn_expr(inputTypes,
								 numArguments,
								 0, /* 尚不支持有序集窗口函数   */
								 false, /* 尚无 VARIADIC 窗口函数     */
								 aggtranstype,
								 wfunc->inputcollid,
								 transfn_oid,
								 invtransfn_oid,
								 &transfnexpr,
								 &invtransfnexpr);

	/* 为调用各 transfn 与 finalfn 建立基础设施          */
	fmgr_info(transfn_oid, &peraggstate->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &peraggstate->transfn);

	if (OidIsValid(invtransfn_oid))
	{
		fmgr_info(invtransfn_oid, &peraggstate->invtransfn);
		fmgr_info_set_expr((Node *) invtransfnexpr, &peraggstate->invtransfn);
	}

	if (OidIsValid(finalfn_oid))
	{
		build_aggregate_finalfn_expr(inputTypes,
									 peraggstate->numFinalArgs,
									 aggtranstype,
									 wfunc->wintype,
									 wfunc->inputcollid,
									 finalfn_oid,
									 &finalfnexpr);
		fmgr_info(finalfn_oid, &peraggstate->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &peraggstate->finalfn);
	}

	/* 获取相关数据类型的信息 */
	get_typlenbyval(wfunc->wintype,
					&peraggstate->resulttypeLen,
					&peraggstate->resulttypeByVal);
	get_typlenbyval(aggtranstype,
					&peraggstate->transtypeLen,
					&peraggstate->transtypeByVal);

	/*
	 * initval 可能为空，所以不要把它当结构体字段来    
	 * 访问。必须用 SysCacheGetAttr 费劲地取。  
	 */
	textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple, initvalAttNo,
								  &peraggstate->initValueIsNull);

	if (peraggstate->initValueIsNull)
		peraggstate->initValue = (Datum) 0;
	else
		peraggstate->initValue = GetAggInitVal(textInitVal,
											   aggtranstype);

	/*
	 * 若 transfn 是严格的而 initval 为 NULL，须确保输入        
	 * 类型与 transtype 相同（或至少二进制兼容），这样  
	 * 才能把第一个输入值用作初始 transValue。这本应       
	 * 在聚合定义时检查过，但若 transfn 的严格性属性   
	 * 曾被更改，则必须再次检查。                          
	 */
	if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
	{
		if (numArguments < 1 ||
			!IsBinaryCoercible(inputTypes[0], aggtranstype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate %u needs to have compatible input type and transition type",
							wfunc->winfnoid)));
	}

	/*
	 * 坚持要求正向与逆向转换函数具有相同的严格性   
	 * 设置。允许二者不同将需要在                            
	 * advance_windowaggregate 和                      
	 * advance_windowaggregate_base 中处理更多特殊情况，却        
	 * 无明显益处。本应在聚合定义时检查，但若任一     
	 * 函数的严格性属性被改过，则须再次检查。   
	 */
	if (OidIsValid(invtransfn_oid) &&
		peraggstate->transfn.fn_strict != peraggstate->invtransfn.fn_strict)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("strictness of aggregate's forward and inverse transition functions must match")));

	/*
	 * 移动聚合使用自己的 aggcontext。  
	 *
	 * 这是必要的，因为它们可能在不同时刻重启，否则    
	 * 我们可能永远无法重置共享的上下文。我们不能     
	 * 把善后清理交给聚合自己负责，因为每当移除其最  
	 * 后一个非 NULL 输入时，严格聚合都必须重启，聚合  
	 * 本身并不知道这件事正在发生。                           
	 * 此外，重启时仅 pfree() 掉 transValue 也无济于事，    
	 * 因为会漏掉间接引用的数据。理论上我们可以让      
	 * 移动聚合改用与普通聚合历来不同的内存分配         
	 * 规则，但这似乎很糟糕，                                      
	 * 且容易导致内存泄漏。     
	 */
	if (OidIsValid(invtransfn_oid))
		peraggstate->aggcontext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "WindowAgg Per Aggregate",
								  ALLOCSET_DEFAULT_SIZES);
	else
		peraggstate->aggcontext = winstate->aggcontext;

	ReleaseSysCache(aggTuple);

	return peraggstate;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

/*
 * are_peers
 * 按 ORDER BY 子句比较两行是否相等                               
 *
 * 注意：这不考虑窗口帧模式。          
 */
static bool
are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
		  TupleTableSlot *slot2)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	ExprContext *econtext = winstate->tmpcontext;

	/* 若无 ORDER BY，所有行彼此互为同级元组*/
	if (node->ordNumCols == 0)
		return true;

	econtext->ecxt_outertuple = slot1;
	econtext->ecxt_innertuple = slot2;
	return ExecQualAndReset(winstate->ordEqfunction, econtext);
}

/*
 * window_gettupleslot
 *	用 winobj 的读指针把当前分区第 pos 个元组取入 
 *	对应 slot。                 
 *
 * 成功返回 true，无此行则返回 false。 
 */
static bool
window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	MemoryContext oldcontext;

	/* 常被连续重复调用         */
	CHECK_FOR_INTERRUPTS();

	/* 此处不允许给 spool_tuples 传 -1      */
	if (pos < 0)
		return false;

	/* 如有必要，把元组取入缓冲         */
	spool_tuples(winstate, pos);

	if (pos >= winstate->spooled_rows)
		return false;

	if (pos < winobj->markpos)
		elog(ERROR, "cannot fetch row before WindowObject's mark position");

	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);

	/*
	 * 前进或回退，直到与想要的元组相距不超过一个。 
	 */
	if (winobj->seekpos < pos - 1)
	{
		if (!tuplestore_skiptuples(winstate->buffer,
								   pos - 1 - winobj->seekpos,
								   true))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos = pos - 1;
	}
	else if (winobj->seekpos > pos + 1)
	{
		if (!tuplestore_skiptuples(winstate->buffer,
								   winobj->seekpos - (pos + 1),
								   false))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos = pos + 1;
	}
	else if (winobj->seekpos == pos)
	{
		/*
		 * 没有 API 能重新取回当前位置上的元组。我们     
		 * 必须先向前移动一个元组，再向后移动一个。（  
		 * 不按相反顺序做，是因为那可能尝试取回位于我 
		 * 们标记之前的行，这不允许。）XXX 此情形有待   
		 * 优化。 
		 */
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	/*
	 * 现在我们应当已位于想要的元组的紧邻前一行或     
	 * 后一行，于是按需向前或向后取行即可。   
	 *
	 * 注意我们让 tuplestore_gettupleslot 对取到的元组            
	 * 制作物理副本。这确保 slot 的内容在 tuplestore 的       
	 * 各种操作过程中保持有效，有些调用方依赖于此。    
	 */
	if (winobj->seekpos > pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, false, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos--;
	}
	else
	{
		if (!tuplestore_gettupleslot(winstate->buffer, true, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos++;
	}

	Assert(winobj->seekpos == pos);

	MemoryContextSwitchTo(oldcontext);

	return true;
}


/***********************************************************************
 * 暴露给窗口函数的 API   
 ***********************************************************************/


/*
 * WinGetPartitionLocalMemory
 *		获取存活到分区处理结束为止的工作内存        
 *
 * 在给定分区内的首次调用会分配并清零所需数量      
 * 的空间。后续调用只返回同一块内存。                     
 *
 * 这样获得的内存通常用于保存应当随每个新分区       
 * 自动重置的状态。若窗口函数想保存横跨整个查        
 * 询的状态，则可按通常方式使用 fcinfo->fn_extra              
 * 来达成。       
 */
void *
WinGetPartitionLocalMemory(WindowObject winobj, Size sz)
{
	Assert(WindowObjectIsValid(winobj));
	if (winobj->localmem == NULL)
		winobj->localmem =
			MemoryContextAllocZero(winobj->winstate->partcontext, sz);
	return winobj->localmem;
}

/*
 * WinGetCurrentPosition
 *		返回当前行在当前分区内的位置，计数从 0 开始       
 *		算起。 
 */
int64
WinGetCurrentPosition(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	return winobj->winstate->currentpos;
}

/*
 * WinGetPartitionRowCount
 *		返回当前分区中包含的总行数。                     
 *
 * 注意：这是相对昂贵的操作，因为它强迫把整个分  
 * 区一次性 spool 进 tuplestore。一旦执行过后，         
 * 在同一分区内的后续调用就很廉价。                        
 */
int64
WinGetPartitionRowCount(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	spool_tuples(winobj->winstate, -1);
	return winobj->winstate->spooled_rows;
}

/*
 * WinSetMarkPosition
 *		为窗口对象设置 mark 位置，即当前分区内随后          
 *		所有操作中允许取用的最老行号                           
 *		（从 0 起计）。                   
 *
 * 窗口函数并非必须调用它，但鼓励尽量把标记向          
 * 前推进，以压低 tuplestore 的大小并避免行溢出到        
 * 磁盘。                    
 */
void
WinSetMarkPosition(WindowObject winobj, int64 markpos)
{
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	if (markpos < winobj->markpos)
		elog(ERROR, "cannot move WindowObject's mark position backward");
	tuplestore_select_read_pointer(winstate->buffer, winobj->markptr);
	if (markpos > winobj->markpos)
	{
		tuplestore_skiptuples(winstate->buffer,
							  markpos - winobj->markpos,
							  true);
		winobj->markpos = markpos;
	}
	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	if (markpos > winobj->seekpos)
	{
		tuplestore_skiptuples(winstate->buffer,
							  markpos - winobj->seekpos,
							  true);
		winobj->seekpos = markpos;
	}
}

/*
 * WinRowsArePeers
 *		比较两行（以在分区内的绝对位置指定）是否按      
 *		ORDER BY 子句相等。                           
 *
 * 注意：这不考虑窗口帧模式。          
 */
bool
WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2)
{
	WindowAggState *winstate;
	WindowAgg  *node;
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	bool		res;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	node = (WindowAgg *) winstate->ss.ps.plan;

	/* 若无 ORDER BY，各行互为同级元组；无须取回      */
	if (node->ordNumCols == 0)
		return true;

	/*
	 * 注意：此处可用 temp_slot_2，因为我们没有调用  
	 * 任何帧相关函数（往往覆盖 temp_slot_2）。      
	 */
	slot1 = winstate->temp_slot_1;
	slot2 = winstate->temp_slot_2;

	if (!window_gettupleslot(winobj, pos1, slot1))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos1);
	if (!window_gettupleslot(winobj, pos2, slot2))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos2);

	res = are_peers(winstate, slot1, slot2);

	ExecClearTuple(slot1);
	ExecClearTuple(slot2);

	return res;
}

/*
 * WinGetFuncArgInPartition
 *		在分区指定行上求值窗口函数的参数表达式，   
 *		该行按 lseek(2) 的方式指定，即相对于当前行、  
 *		首行或末行。                               
 *
 * argno: 要求值的参数编号（从 0 起计）  
 * relpos: 相对定位点的带符号行数偏移      
 * seektype: WINDOW_SEEK_CURRENT、WINDOW_SEEK_HEAD/WINDOW_SEEK_TAIL   
 * set_mark: 若找到该行且 set_mark 为真，则作为副作用       
 *		把标记移到该行。 
 * isnull: 输出参数，接收结果的 isnull 状态     
 * isout: 输出参数，置位以指示目标行位置是否越出   
 *		分区（若调用方不关心此信息可传 NULL）。             
 *
 * 指定不存在的行并不是错误，只是会让结果为 NULL         
 * （isout 非 NULL 还会置 *isout 为真）。 
 */
Datum
WinGetFuncArgInPartition(WindowObject winobj, int argno,
						 int relpos, int seektype, bool set_mark,
						 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			abs_pos = relpos;
			break;
		case WINDOW_SEEK_TAIL:
			spool_tuples(winstate, -1);
			abs_pos = winstate->spooled_rows - 1 + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0;		/* 让编译器安静  */
			break;
	}

	gottuple = window_gettupleslot(winobj, abs_pos, slot);

	if (!gottuple)
	{
		if (isout)
			*isout = true;
		*isnull = true;
		return (Datum) 0;
	}
	else
	{
		if (isout)
			*isout = false;
		if (set_mark)
			WinSetMarkPosition(winobj, abs_pos);
		econtext->ecxt_outertuple = slot;
		return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
							econtext, isnull);
	}
}

/*
 * WinGetFuncArgInFrame
 *		在窗口帧指定行上求值窗口函数的参数表达式，
 *		该行按 lseek(2) 方式指定，即相对于帧的首行或     
 *		末行。（此处不支持 WINDOW_SEEK_CURRENT，因为        
 *		若当前行不在帧内，很难说清它应表示               
 *		什么。）                                                 
 *
 * argno: 要求值的参数编号（从 0 起计）  
 * relpos: 相对定位点的带符号行数偏移      
 * seektype: WINDOW_SEEK_HEAD/WINDOW_SEEK_TAIL   
 * set_mark: 若该行存在且在帧内，且 set_mark 为真，          
 *		标记会随之移到该行。    
 * isnull: 输出参数，接收结果的 isnull 状态     
 * isout: 输出参数，置位以指示目标行位置是否越出   
 *		帧（若调用方不关心此信息可传 NULL）。            
 *
 * 指定不存在的行或不在帧内的行都不是错误，只是   
 * 会让结果为 NULL（isout 非 NULL 还会置 *isout 为真）。  
 *
 * 注意某些排除子句选项会导致帧内行在分区中不     
 * 连续的情形。但我们测量 relpos 时                        
 * 只统计帧内行。                          
 *
 * set_mark 标志的含义是：调用方在连续调用中将会给出   
 * 常量（或可能单调递增）的 relpos，这样若无排除       
 * 子句就无需取回先前取过的行之前的行。但我们       
 * 并不指望调用方知道如何应对排除子句。因此，        
 * 若存在排除子句，就由我们负责将标记请求           
 * 调整为在上述关于 relpos 的假设下                            
 * 仍然安全的形式。                                              
 *              
 */
Datum
WinGetFuncArgInFrame(WindowObject winobj, int argno,
					 int relpos, int seektype, bool set_mark,
					 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	int64		abs_pos;
	int64		mark_pos;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			elog(ERROR, "WINDOW_SEEK_CURRENT is not supported for WinGetFuncArgInFrame");
			abs_pos = mark_pos = 0; /* 让编译器安静  */
			break;
		case WINDOW_SEEK_HEAD:
			/* 拒绝 relpos < 0 简单，且能简化后面的代码 */
			if (relpos < 0)
				goto out_of_frame;
			update_frameheadpos(winstate);
			abs_pos = winstate->frameheadpos + relpos;
			mark_pos = abs_pos;

			/*
			 * 若排除选项生效则将其计入，但只推进            
			 * abs_pos 而非 mark_pos。这可避免当前行的同级   
			 * 组变化导致试图取回行位置在某个先前标记之前
			 * 的行。                   
			 *
			 * 注意拐角情形下这些计算理论上偏简单，  
			 * （如当前行在帧外），但没关系，因为我们终将
			 * 判定该行在帧外。我们并不试图避免取到帧末  
			 * 端之后的行；                                          
			 * 反正某些情形下这都会发生。              
			 */
			switch (winstate->frameOptions & FRAMEOPTION_EXCLUSION)
			{
				case 0:
					/* 无需调整         */
					break;
				case FRAMEOPTION_EXCLUDE_CURRENT_ROW:
					if (abs_pos >= winstate->currentpos &&
						winstate->currentpos >= winstate->frameheadpos)
						abs_pos++;
					break;
				case FRAMEOPTION_EXCLUDE_GROUP:
					update_grouptailpos(winstate);
					if (abs_pos >= winstate->groupheadpos &&
						winstate->grouptailpos > winstate->frameheadpos)
					{
						int64		overlapstart = Max(winstate->groupheadpos,
													   winstate->frameheadpos);

						abs_pos += winstate->grouptailpos - overlapstart;
					}
					break;
				case FRAMEOPTION_EXCLUDE_TIES:
					update_grouptailpos(winstate);
					if (abs_pos >= winstate->groupheadpos &&
						winstate->grouptailpos > winstate->frameheadpos)
					{
						int64		overlapstart = Max(winstate->groupheadpos,
													   winstate->frameheadpos);

						if (abs_pos == overlapstart)
							abs_pos = winstate->currentpos;
						else
							abs_pos += winstate->grouptailpos - overlapstart - 1;
					}
					break;
				default:
					elog(ERROR, "unrecognized frame option state: 0x%x",
						 winstate->frameOptions);
					break;
			}
			break;
		case WINDOW_SEEK_TAIL:
			/* 拒绝 relpos > 0 简单，且能简化后面的代码 */
			if (relpos > 0)
				goto out_of_frame;
			update_frametailpos(winstate);
			abs_pos = winstate->frametailpos - 1 + relpos;

			/*
			 * 若排除选项生效则计入。无排除时可放心在所  
			 * 访问的行上设标记。但若有排除，就只能标记   
			 * 帧起点，因为我们无法确定排除将来会使我们   
			 * 在帧内往回取多远。此外，这里必须实际对照  
			 * frameheadpos 检查，因为若标记可能已在帧起点 
			 * 处，还试图取帧起点之前的行是不安全的。     
			 *                                                           
			 */
			switch (winstate->frameOptions & FRAMEOPTION_EXCLUSION)
			{
				case 0:
					/* 无需调整         */
					mark_pos = abs_pos;
					break;
				case FRAMEOPTION_EXCLUDE_CURRENT_ROW:
					if (abs_pos <= winstate->currentpos &&
						winstate->currentpos < winstate->frametailpos)
						abs_pos--;
					update_frameheadpos(winstate);
					if (abs_pos < winstate->frameheadpos)
						goto out_of_frame;
					mark_pos = winstate->frameheadpos;
					break;
				case FRAMEOPTION_EXCLUDE_GROUP:
					update_grouptailpos(winstate);
					if (abs_pos < winstate->grouptailpos &&
						winstate->groupheadpos < winstate->frametailpos)
					{
						int64		overlapend = Min(winstate->grouptailpos,
													 winstate->frametailpos);

						abs_pos -= overlapend - winstate->groupheadpos;
					}
					update_frameheadpos(winstate);
					if (abs_pos < winstate->frameheadpos)
						goto out_of_frame;
					mark_pos = winstate->frameheadpos;
					break;
				case FRAMEOPTION_EXCLUDE_TIES:
					update_grouptailpos(winstate);
					if (abs_pos < winstate->grouptailpos &&
						winstate->groupheadpos < winstate->frametailpos)
					{
						int64		overlapend = Min(winstate->grouptailpos,
													 winstate->frametailpos);

						if (abs_pos == overlapend - 1)
							abs_pos = winstate->currentpos;
						else
							abs_pos -= overlapend - 1 - winstate->groupheadpos;
					}
					update_frameheadpos(winstate);
					if (abs_pos < winstate->frameheadpos)
						goto out_of_frame;
					mark_pos = winstate->frameheadpos;
					break;
				default:
					elog(ERROR, "unrecognized frame option state: 0x%x",
						 winstate->frameOptions);
					mark_pos = 0;	/* 让编译器安静  */
					break;
			}
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = mark_pos = 0; /* 让编译器安静  */
			break;
	}

	if (!window_gettupleslot(winobj, abs_pos, slot))
		goto out_of_frame;

	/* 上述代码不能检测所有越出帧的情形，故要检查 */
	if (row_is_in_frame(winstate, abs_pos, slot) <= 0)
		goto out_of_frame;

	if (isout)
		*isout = false;
	if (set_mark)
		WinSetMarkPosition(winobj, mark_pos);
	econtext->ecxt_outertuple = slot;
	return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
						econtext, isnull);

out_of_frame:
	if (isout)
		*isout = true;
	*isnull = true;
	return (Datum) 0;
}

/*
 * WinGetFuncArgCurrent
 *		在当前行上求值窗口函数的参数表达式。              
 *
 * argno: 要求值的参数编号（从 0 起计）  
 * isnull: 输出参数，接收结果的 isnull 状态     
 *
 * 注意：这与 WinGetFuncArgInPartition 或                    
 * WinGetFuncArgInFrame 以当前行为目标并不完全等价，因为  
 * 即便 WindowObject 标记已设到当前行之后也能成功。   
 * 窗口函数的"普通"参数一般应使用本函数，          
 * 例如 lead() 或 lag() 的偏移参数。               
 */
Datum
WinGetFuncArgCurrent(WindowObject winobj, int argno, bool *isnull)
{
	WindowAggState *winstate;
	ExprContext *econtext;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	econtext = winstate->ss.ps.ps_ExprContext;

	econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
	return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
						econtext, isnull);
}
