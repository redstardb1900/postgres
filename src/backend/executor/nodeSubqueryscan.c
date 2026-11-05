/*-------------------------------------------------------------------------
 *
 * nodeSubqueryscan.c
 *	  扫描子查询（范围表内的 subselect）的支持例程。    
 *
 * 它与 sublink（nodeSubplan.c）的差异刚好大                     
 * 到需要两套代码。应当考虑尝试统一两者。             
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSubqueryscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSubqueryScan			扫描子查询  
 *		ExecSubqueryNext			按顺序取下一个元组。          
 *		ExecInitSubqueryScan		创建并初始化 subqueryscan 节点。   
 *		ExecEndSubqueryScan			释放所有已分配存储。 
 *		ExecReScanSubqueryScan	重新扫描关系   
 *
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSubqueryscan.h"

static TupleTableSlot *SubqueryNext(SubqueryScanState *node);

/* ----------------------------------------------------------------
 *						扫描支持
 * ----------------------------------------------------------------
 */
/* ----------------------------------------------------------------
 *		SubqueryNext
 *
 *		这是 ExecSubqueryScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SubqueryNext(SubqueryScanState *node)
{
	TupleTableSlot *slot;

	/*
	 * 从子查询取下一条元组。     
	 */
	slot = ExecProcNode(node->subplan);

	/*
	 * 我们直接返回子计划的结果槽，而不必为 ExecCopySlot()
	 * 耗费额外周期。（我们自己的 ScanTupleSlot 仅用于    
	 * EvalPlanQual 重查） 
	 */
	return slot;
}

/*
 * SubqueryRecheck -- EvalPlanQual 中重新检查元组的访问方法例程 
 */
static bool
SubqueryRecheck(SubqueryScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSubqueryScan(node)
 *
 *		顺序扫描子查询并返回下一个满足条件的元组。
 *		我们
 *		调用 ExecScan() 例程，把相应的访问方法       
 *		函数传递给它。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSubqueryScan(PlanState *pstate)
{
	SubqueryScanState *node = castNode(SubqueryScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SubqueryNext,
					(ExecScanRecheckMtd) SubqueryRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSubqueryScan
 * ----------------------------------------------------------------
 */
SubqueryScanState *
ExecInitSubqueryScan(SubqueryScan *node, EState *estate, int eflags)
{
	SubqueryScanState *subquerystate;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/* SubqueryScan 不应有任何"普通"子节点。   */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 创建状态结构    
	 */
	subquerystate = makeNode(SubqueryScanState);
	subquerystate->ss.ps.plan = (Plan *) node;
	subquerystate->ss.ps.state = estate;
	subquerystate->ss.ps.ExecProcNode = ExecSubqueryScan;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &subquerystate->ss.ps);

	/*
	 * 初始化子查询 
	 */
	subquerystate->subplan = ExecInitNode(node->subplan, estate, eflags);

	/*
	 * 初始化扫描槽与类型（ExecAssignScanProjectionInfo 需要）  
	 */
	ExecInitScanTupleSlot(estate, &subquerystate->ss,
						  ExecGetResultType(subquerystate->subplan),
						  ExecGetResultSlotOps(subquerystate->subplan, NULL));

	/*
	 * 用作 scantuple 的槽并非上面的槽（EPQ 之外时），      
	 * 而用下层节点的槽。     
	 */
	subquerystate->ss.ps.scanopsset = true;
	subquerystate->ss.ps.scanops = ExecGetResultSlotOps(subquerystate->subplan,
														&subquerystate->ss.ps.scanopsfixed);
	subquerystate->ss.ps.resultopsset = true;
	subquerystate->ss.ps.resultops = subquerystate->ss.ps.scanops;
	subquerystate->ss.ps.resultopsfixed = subquerystate->ss.ps.scanopsfixed;

	/*
	 * 初始化结果类型和投影。     
	 */
	ExecInitResultTypeTL(&subquerystate->ss.ps);
	ExecAssignScanProjectionInfo(&subquerystate->ss);

	/*
	 * 初始化子表达式       
	 */
	subquerystate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) subquerystate);

	return subquerystate;
}

/* ----------------------------------------------------------------
 *		ExecEndSubqueryScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndSubqueryScan(SubqueryScanState *node)
{
	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空上层元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 关闭子查询    
	 */
	ExecEndNode(node->subplan);
}

/* ----------------------------------------------------------------
 *		ExecReScanSubqueryScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanSubqueryScan(SubqueryScanState *node)
{
	ExecScanReScan(&node->ss);

	/*
	 * ExecReScan 并不了解我的子计划，所以变更    
	 * 参数信号需由我自行发出。这倒正好，因为             
	 * 子计划有独立内存上下文，chgParam 状态存于其中。   
	 */
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(node->subplan, node->ss.ps.chgParam);

	/*
	 * 若子节点的 chgParam 非空，计划会被首次 ExecProcNode 
	 * 重新扫描。    
	 */
	if (node->subplan->chgParam == NULL)
		ExecReScan(node->subplan);
}
