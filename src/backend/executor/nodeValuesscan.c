/*-------------------------------------------------------------------------
 *
 * nodeValuesscan.c
 *	  扫描 Values 列表的支撑例程       
 *	  （ rangetable 中的 "VALUES (...)"）。 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeValuesscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecValuesScan 扫描 values 列表  
 *		ExecValuesNext 按顺序取下一个元组。            
 *		ExecInitValuesScan 创建并初始化 valuesscan 节点。    
 *		ExecEndValuesScan 释放所有已分配存储。  
 *		ExecReScanValuesScan 重新扫描列表     
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeValuesscan.h"
#include "jit/jit.h"
#include "optimizer/clauses.h"
#include "utils/expandeddatum.h"


static TupleTableSlot *ValuesNext(ValuesScanState *node);


/* ----------------------------------------------------------------
 *						扫描支持
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ValuesNext
 *
 *		这是 ExecValuesScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ValuesNext(ValuesScanState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	int			curr_idx;

	/*
	 * 从 estate 和扫描状态获取信息        
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;
	econtext = node->rowcontext;

	/*
	 * 取下一个元组。无更多元组时返回 NULL
	 */
	if (ScanDirectionIsForward(direction))
	{
		if (node->curr_idx < node->array_len)
			node->curr_idx++;
	}
	else
	{
		if (node->curr_idx >= 0)
			node->curr_idx--;
	}

	/*
	 * 始终清空结果槽位；如果我们已到数据末尾，这样做 
	 * 是合适的；即使未到末尾，也是存储虚拟元组协议  
	 * 的第一步。先清空槽位似乎更稳妥，                  
	 * 然后再重置它可能存有指针的上下文。     
	 */
	ExecClearTuple(slot);

	curr_idx = node->curr_idx;
	if (curr_idx >= 0 && curr_idx < node->array_len)
	{
		List	   *exprlist = node->exprlists[curr_idx];
		List	   *exprstatelist = node->exprstatelists[curr_idx];
		MemoryContext oldContext;
		Datum	   *values;
		bool	   *isnull;
		ListCell   *lc;
		int			resind;

		/*
		 * 清除上一循环的遗留。我们使用 ReScanExprContext     
		 * 而非只是 ResetExprContext，因为我们想让已注册的   
		 * 关闭回调被调用  
		 */
		ReScanExprContext(econtext);

		/*
		 * 在每元组上下文中做 VALUES 行级工作 
		 */
		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/*
		 * 除非已为该行建好表达式求值状态，否则我们  
		 * 会在 econtext 的每元组内存中构建它。这有点 
		 * 不寻常，但我们希望在移到下一行时再次删除求值 
		 * 状态，以免 values 列表很长时内存需求增长。     
		 * 对不适用的行，我们已在计划启动时构建了        
		 * 求值状态。                
		 */
		if (exprstatelist == NIL)
		{
			/*
			 * 传 parent 为 NULL 而非本计划节点，因为我们    
			 * 不希望该临时状态的任何内容链入永久状态。  
			 * 唯一可能想这么做的表达式类型是 SubPlan，       
			 * 而我们已检查过没有此类节点。   
			 *
			 * 注意，传入 parent = NULL 也会禁用表达式的 JIT   
			 * 编译，这反而有利，因为它们在正常情况下只  
			 * 被使用一次。                         
			 */
			exprstatelist = ExecInitExprList(exprlist, NULL);
		}

		/* 解析器应已检查所有子列表长度相同            */
		Assert(list_length(exprstatelist) == slot->tts_tupleDescriptor->natts);

		/*
		 * 计算各表达式并构建虚拟结果元组。我们已经
		 * 执行过 ExecClearTuple(slot)。
		 */
		values = slot->tts_values;
		isnull = slot->tts_isnull;

		resind = 0;
		foreach(lc, exprstatelist)
		{
			ExprState  *estate = (ExprState *) lfirst(lc);
			Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor,
												   resind);

			values[resind] = ExecEvalExpr(estate,
										  econtext,
										  &isnull[resind]);

			/*
			 * 必须强制任何 R/W 展开的 Datum 进入只读状态，
			 * 以防它们在计划节点输出表达式被多次引用  
			 * 或者我们跳过输出投影时，输出列                
			 * 在上层计划节点中被多次引用。                
			 */
			values[resind] = MakeExpandedObjectReadOnly(values[resind],
														isnull[resind],
														attr->attlen);

			resind++;
		}

		MemoryContextSwitchTo(oldContext);

		/*
		 * 然后返回虚拟元组。  
		 */
		ExecStoreVirtualTuple(slot);
	}

	return slot;
}

/*
 * ValuesRecheck -- EvalPlanQual 中重新检查元组的访问方法例程 
 */
static bool
ValuesRecheck(ValuesScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecValuesScan(node)
 *
 *		按顺序扫描 values 列表，并返回下一个满足条件的  
 *		元组
 *		我们调用 ExecScan() 例程，并向它传入适当的 
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecValuesScan(PlanState *pstate)
{
	ValuesScanState *node = castNode(ValuesScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) ValuesNext,
					(ExecScanRecheckMtd) ValuesRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitValuesScan
 * ----------------------------------------------------------------
 */
ValuesScanState *
ExecInitValuesScan(ValuesScan *node, EState *estate, int eflags)
{
	ValuesScanState *scanstate;
	TupleDesc	tupdesc;
	ListCell   *vtl;
	int			i;
	PlanState  *planstate;

	/*
	 * ValuesScan 不应有任何子节点。  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点创建 ScanState    
	 */
	scanstate = makeNode(ValuesScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecValuesScan;

	/*
	 * 杂项初始化             
	 */
	planstate = &scanstate->ss.ps;

	/*
	 * 创建表达式上下文。需要两个，一个供子列表 
	 * 处理使用，另一个供 execScan.c 用于条件和投影。我们 
	 * 稍微取巧：用 ExecAssignExprContext() 来创建两者。 
	 */
	ExecAssignExprContext(estate, planstate);
	scanstate->rowcontext = planstate->ps_ExprContext;
	ExecAssignExprContext(estate, planstate);

	/*
	 * 获取 values 列表信息，以其初始化扫描槽。 
	 */
	tupdesc = ExecTypeFromExprList((List *) linitial(node->values_lists));
	ExecInitScanTupleSlot(estate, &scanstate->ss, tupdesc, &TTSOpsVirtual);

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * 其他节点专属设置 
	 */
	scanstate->curr_idx = -1;
	scanstate->array_len = list_length(node->values_lists);

	/*
	 * 把表达式子列表的列表转换为数组，方便运行期 
	 * 容易地寻址。此外，检测是否有子列表包含        
	 * SubPlan；只针对这些子列表，继续执行表达式   
	 * 初始化。（这避免了 SubPlan 想自行连接到外层           
	 * 计划树的问题。特别是不这样做时，EXPLAIN              
	 * 将看不到这些子计划；而且若按与简单表达式         
	 * 相同的方式处理 SubPlan，还会有悬空指针和/或    
	 * 资源泄漏。）   
	 */
	scanstate->exprlists = (List **)
		palloc(scanstate->array_len * sizeof(List *));
	scanstate->exprstatelists = (List **)
		palloc0(scanstate->array_len * sizeof(List *));
	i = 0;
	foreach(vtl, node->values_lists)
	{
		List	   *exprs = lfirst_node(List, vtl);

		scanstate->exprlists[i] = exprs;

		/*
		 * 在没有任何 SubPlan 的简单情况下，我们可以       
		 * 避免 contain_subplans() 扫描开销。 
		 */
		if (estate->es_subplanstates &&
			contain_subplans((Node *) exprs))
		{
			int			saved_jit_flags;

			/*
			 * 由于这些表达式只用一次，为它们禁用 JIT。    
			 * 这样做是值得的，因为通过 VALUES() 插入大量  
			 * 数据的情况很常见。注意这并不妨碍在子计划 
			 * *内*使用 JIT，那里的 JIT 单独初始化，     
			 * 这只影响上层的子表达式。                         
			 */
			saved_jit_flags = estate->es_jit_flags;
			estate->es_jit_flags = PGJIT_NONE;

			scanstate->exprstatelists[i] = ExecInitExprList(exprs,
															&scanstate->ss.ps);

			estate->es_jit_flags = saved_jit_flags;
		}
		i++;
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndValuesScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndValuesScan(ValuesScanState *node)
{
	/*
	 * 释放两个上下文 
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->rowcontext;
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanValuesScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanValuesScan(ValuesScanState *node)
{
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	node->curr_idx = -1;
}
