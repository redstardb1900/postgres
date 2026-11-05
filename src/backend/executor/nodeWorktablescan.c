/*-------------------------------------------------------------------------
 *
 * nodeWorktablescan.c
 *	  处理 WorkTableScan 节点的例程   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeWorktablescan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeWorktablescan.h"

static TupleTableSlot *WorkTableScanNext(WorkTableScanState *node);

/* ----------------------------------------------------------------
 *		WorkTableScanNext
 *
 *		这是 ExecWorkTableScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
WorkTableScanNext(WorkTableScanState *node)
{
	TupleTableSlot *slot;
	Tuplestorestate *tuplestorestate;

	/*
	 * 从 estate 与扫描状态中获取信息     
	 *
	 * 注意：我们有意不支持反向扫描。虽然在这里只需     
	 * 多加几行代码，但会迫使 nodeRecursiveunion.c 在创建       
	 * tuplestore 时启用反向扫描，有性能代价。实际上  
	 * 反向扫描对工作表计划节点从来没用，因为它在   
	 * 可滚动游标的计划树中位置不够高，不会暴露给    
	 * 反向扫描需求。                                       
	 * 所以不值得花力气去支持它。                        
	 *
	 * 注意：我们还假定此节点是工作表唯一读取者。    
	 * 因此不需要为 tuplestore 提供私有读指针，             
	 * 也不需要让 tuplestore_gettupleslot 进行复制。            
	 */
	Assert(ScanDirectionIsForward(node->ss.ps.state->es_direction));

	tuplestorestate = node->rustate->working_table;

	/*
	 * 从 tuplestore 取下一个元组，没有更多则返回 NULL。 
	 */
	slot = node->ss.ss_ScanTupleSlot;
	(void) tuplestore_gettupleslot(tuplestorestate, true, false, slot);
	return slot;
}

/*
 * WorkTableScanRecheck -- 在 EvalPlanQual 中重新检查元组访问方法例程
 */
static bool
WorkTableScanRecheck(WorkTableScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecWorkTableScan(node)
 *
 *		顺序扫描工作表并返回下一个满足条件的元组。        
 *		我们调用 ExecScan() 例程，并传给它合适的    
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecWorkTableScan(PlanState *pstate)
{
	WorkTableScanState *node = castNode(WorkTableScanState, pstate);

	/*
	 * 首次调用时，通过为其保留的 Param 槽找到祖先      
	 * RecursiveUnion 的状态。（在节点初始化期间不能这        
	 * 样做，因为某些边角情况会在 RecursiveUnion 之前  
	 * 先初始化。）   
	 */
	if (node->rustate == NULL)
	{
		WorkTableScan *plan = (WorkTableScan *) node->ss.ps.plan;
		EState	   *estate = node->ss.ps.state;
		ParamExecData *param;

		param = &(estate->es_param_exec_vals[plan->wtParam]);
		Assert(param->execPlan == NULL);
		Assert(!param->isnull);
		node->rustate = castNode(RecursiveUnionState, DatumGetPointer(param->value));
		Assert(node->rustate);

		/*
		 * 扫描元组类型（即我们预期在工作表中找到的      
		 * 行类型）与祖先 RecursiveUnion 节点的结果行 
		 * 类型相同。注意这依赖于如下假定：              
		 * RecursiveUnion 不允许投影。       
		 */
		ExecAssignScanType(&node->ss,
						   ExecGetResultType(&node->rustate->ps));

		/*
		 * 现在可以初始化投影信息了。这必须在调用         
		 * ExecScan() 之前完成。    
		 */
		ExecAssignScanProjectionInfo(&node->ss);
	}

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) WorkTableScanNext,
					(ExecScanRecheckMtd) WorkTableScanRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitWorkTableScan
 * ----------------------------------------------------------------
 */
WorkTableScanState *
ExecInitWorkTableScan(WorkTableScan *node, EState *estate, int eflags)
{
	WorkTableScanState *scanstate;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * WorkTableScan 不应拥有任何子节点  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点创建 WorkTableScanState    
	 */
	scanstate = makeNode(WorkTableScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecWorkTableScan;
	scanstate->rustate = NULL;	/* 稍后设置此项   */

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 初始化元组表        
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);

	/* 表明返回类型尚未确定           */
	scanstate->ss.ps.resultopsset = true;
	scanstate->ss.ps.resultopsfixed = false;

	ExecInitScanTupleSlot(estate, &scanstate->ss, NULL, &TTSOpsMinimalTuple);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * 暂不初始化投影信息，详见 ExecWorkTableScan() 的      
	 * 说明  
	 */

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndWorkTableScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndWorkTableScan(WorkTableScanState *node)
{
	/*
	 * 释放上下文 
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanWorkTableScan
 *
 *		重新扫描关系   
 * ----------------------------------------------------------------
 */
void
ExecReScanWorkTableScan(WorkTableScanState *node)
{
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	/* 尚未调用 ExecWorkTableScan 时无需（或无法）重扫  */
	if (node->rustate)
		tuplestore_rescan(node->rustate->working_table);
}
