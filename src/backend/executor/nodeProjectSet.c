/*-------------------------------------------------------------------------
 *
 * nodeProjectSet.c
 *	  支持对包含集合返回函数的目标列表求值               
 *
 * DESCRIPTION
 *
 *		规划器插入 ProjectSet 节点，用于对目标列表中的集合 
 *		函数求值。集合返回函数保证位于目标列表顶层，  
 *		即它们不能位于更复杂的表达式之内。若本来会     
 *		出现这种情况，规划器会添加额外的                 
 *		ProjectSet 节点。                                   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeProjectSet.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeProjectSet.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"


static TupleTableSlot *ExecProjectSRF(ProjectSetState *node, bool continuing);


/* ----------------------------------------------------------------
 *		ExecProjectSet(node)
 *
 *		对目标列表（其中包含集合返回函数）              
 *		求值后返回元组
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecProjectSet(PlanState *pstate)
{
	ProjectSetState *node = castNode(ProjectSetState, pstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *resultSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	econtext = node->ps.ps_ExprContext;

	/*
	 * 重置每元组上下文，以释放为之前可能返回的元组     
	 * 分配的表达式求值存储。注意 SRF 参数上下文            
	 * 的生命周期不同，将在下方重置。       
	 */
	ResetExprContext(econtext);

	/*
	 * 检查是否仍在从上一个扫描元组投影输出元组          
	 * （因为投影表达式中有返回集合的函数）。若是，
	 * 则尝试再投影一个。                      
	 */
	if (node->pending_srf_tuples)
	{
		resultSlot = ExecProjectSRF(node, true);

		if (resultSlot != NULL)
			return resultSlot;
	}

	/*
	 * 获取另一条输入元组并从中投影 SRF。
	 */
	for (;;)
	{
		/*
		 * 重置参数上下文，释放上一元组周期中分配的    
		 * 表达式求值存储。注意要等投影完扫描元组后  
		 * 才能这样做，ValuePerCall 函数每条返回的元组  
		 * 都允许引用其参数。但若我们发现某扫描元组     
		 * 不产生任何行而绕回时，应当重置，以免在大量    
		 * 连续的扫描元组都不产出行时                          
		 * 泄漏内存。                                                 
		 */
		MemoryContextReset(node->argcontext);

		/*
		 * 从外层计划取元组，直到没有更多为止。      
		 */
		outerPlan = outerPlanState(node);
		outerTupleSlot = ExecProcNode(outerPlan);

		if (TupIsNull(outerTupleSlot))
			return NULL;

		/*
		 * 准备计算投影表达式，它们期望以 varno OUTER      
		 * 的身份访问输入元组。         
		 */
		econtext->ecxt_outertuple = outerTupleSlot;

		/* 对这些表达式求值 */
		resultSlot = ExecProjectSRF(node, false);

		/*
		 * 返回该元组，除非投影因空集而未产出任何行，   
		 * 此时必须绕回查看是否还有更多外层               
		 * 元组。             
		 */
		if (resultSlot)
			return resultSlot;

		/*
		 * 当真的绕回时，最好再次重置 econtext，以防          
		 * SRF 在那里泄漏了内存。       
		 */
		ResetExprContext(econtext);
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecProjectSRF
 *
 *		投影包含一个或多个集合返回函数的目标列表。     
 *
 *		continuing 指示是继续为同一输入元组投影行，       
 *		还是正在投影新的输入元组。                           
 *
 *		若未产出输出元组则返回 NULL。         
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecProjectSRF(ProjectSetState *node, bool continuing)
{
	TupleTableSlot *resultSlot = node->ps.ps_ResultTupleSlot;
	ExprContext *econtext = node->ps.ps_ExprContext;
	MemoryContext oldcontext;
	bool		hassrf PG_USED_FOR_ASSERTS_ONLY;
	bool		hasresult;
	int			argno;

	ExecClearTuple(resultSlot);

	/* 在每元组上下文中调用 SRF 以及普通表达式      */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * 假定不会再产出元组，除非从某个集合返回函数      
	 * 遇到 ExprMultipleResult。              
	 */
	node->pending_srf_tuples = false;

	hassrf = hasresult = false;
	for (argno = 0; argno < node->nelems; argno++)
	{
		Node	   *elem = node->elems[argno];
		ExprDoneCond *isdone = &node->elemdone[argno];
		Datum	   *result = &resultSlot->tts_values[argno];
		bool	   *isnull = &resultSlot->tts_isnull[argno];

		if (continuing && *isdone == ExprEndResult)
		{
			/*
			 * 如果正在继续从源元组投影输出行，那么一旦   
			 * SRF 耗尽就返回 NULL。                  
			 */
			*result = (Datum) 0;
			*isnull = true;
			hassrf = true;
		}
		else if (IsA(elem, SetExprState))
		{
			/*
			 * 对 SRF 求值 - 可能继续之前已开始的输出。     
			 */
			*result = ExecMakeFunctionResultSet((SetExprState *) elem,
												econtext, node->argcontext,
												isnull, isdone);

			if (*isdone != ExprEndResult)
				hasresult = true;
			if (*isdone == ExprMultipleResult)
				node->pending_srf_tuples = true;
			hassrf = true;
		}
		else
		{
			/* 非 SRF 的目标列表表达式，正常求值。*/
			*result = ExecEvalExpr((ExprState *) elem, econtext, isnull);
			*isdone = ExprSingleResult;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* 没有 SRF 时不应使用 ProjectSet            */
	Assert(hassrf);

	/*
	 * 如果所有 SRF 都返回 ExprEndResult，则视为              
	 * 未产出行。
	 */
	if (hasresult)
	{
		ExecStoreVirtualTuple(resultSlot);
		return resultSlot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitProjectSet
 *
 *		为规划器产生的 ProjectSet 节点创建运行时        
 *		状态信息，并初始化外层关系（子节       
 *		点）。     
 * ----------------------------------------------------------------
 */
ProjectSetState *
ExecInitProjectSet(ProjectSet *node, EState *estate, int eflags)
{
	ProjectSetState *state;
	ListCell   *lc;
	int			off;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)));

	/*
	 * 创建状态结构    
	 */
	state = makeNode(ProjectSetState);
	state->ps.plan = (Plan *) node;
	state->ps.state = estate;
	state->ps.ExecProcNode = ExecProjectSet;

	state->pending_srf_tuples = false;

	/*
	 * 其他初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &state->ps);

	/*
	 * 初始化子节点    
	 */
	outerPlanState(state) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 不用内层计划     
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * 元组表与结果类型初始化         
	 */
	ExecInitResultTupleSlotTL(&state->ps, &TTSOpsVirtual);

	/* 为每 tlist 条目创建 expr 状态与 SRF 完成状态工作区  */
	state->nelems = list_length(node->plan.targetlist);
	state->elems = (Node **)
		palloc(sizeof(Node *) * state->nelems);
	state->elemdone = (ExprDoneCond *)
		palloc(sizeof(ExprDoneCond) * state->nelems);

	/*
	 * 构建用于对目标列表求值的表达式。无法 
	 * 使用 ExecBuildProjectionInfo，因为它不处理             
	 * SRF。改为逐个编译各表达式，能用    
	 * 地方就用 ExecInitFunctionResultSet。  
	 */
	off = 0;
	foreach(lc, node->plan.targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Expr	   *expr = te->expr;

		if ((IsA(expr, FuncExpr) && ((FuncExpr *) expr)->funcretset) ||
			(IsA(expr, OpExpr) && ((OpExpr *) expr)->opretset))
		{
			state->elems[off] = (Node *)
				ExecInitFunctionResultSet(expr, state->ps.ps_ExprContext,
										  &state->ps);
		}
		else
		{
			Assert(!expression_returns_set((Node *) expr));
			state->elems[off] = (Node *) ExecInitExpr(expr, &state->ps);
		}

		off++;
	}

	/* 不支持 ProjectSet 节点上的任何 qual  */
	Assert(node->plan.qual == NIL);

	/*
	 * 创建一个 ExecMakeFunctionResultSet 可用来                 
	 * 在其中求值函数参数的内存上下文。不能用每           
	 * 元组上下文，因为它重置得太频繁；但我们      
	 * 也不想把求值结果泄漏进查询期上下文。我们为       
	 * 所有 tSRF 的参数共用一个上下文，因其寿命大致        
	 * 相当。 
	 */
	state->argcontext = AllocSetContextCreate(CurrentMemoryContext,
											  "tSRF function arguments",
											  ALLOCSET_DEFAULT_SIZES);

	return state;
}

/* ----------------------------------------------------------------
 *		ExecEndProjectSet
 *
 *		释放通过 C 例程分配的存储         
 * ----------------------------------------------------------------
 */
void
ExecEndProjectSet(ProjectSetState *node)
{
	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * 关闭子计划   
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanProjectSet(ProjectSetState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* 忘掉所有未完成求值的 SRF     */
	node->pending_srf_tuples = false;

	/*
	 * 如果子节点的 chgParam 非空，则计划会被第一次     
	 * ExecProcNode 重扫
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
