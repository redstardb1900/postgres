/*-------------------------------------------------------------------------
 *
 * nodeResult.c
 *	  为需要特殊代码的常量节点提供支持
 *
 * 描述     
 *
 *		Result 节点用于不扫描任何关系的查询。            
 *		这类查询的例子有：  
 *
 *				select 1 * 2
 *
 *				insert into emp values ('mike', 15000)
 *
 *		（记住在 INSERT 或 UPDATE 中，我们需要一棵        
 *		生成新行的树。）
 *
 *		Result 节点还用于优化带常量                       
 *		限定条件的查询（即不依赖被扫描数据的条件），
 *		如：  
 *
 *				select * from emp where 2 > 1
 *
 *		此时生成的计划是：        
 *
 *						Result	(with 2 > 1 qual)
 *						/
 *				   SeqScan (emp.*)
 *
 *		运行时，Result 节点只对常量条件求值一次，   
 *		EXPLAIN 会将其显示为 One-Time Filter。如果     
 *		为假，我们可以不运行受控计划而直接返回   
 *		空结果集。如果为真，则正常运行受控         
 *		计划并传回结果。                
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeResult.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeResult.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecResult(node)
 *
 *		返回外层计划中满足                             
 *		条件子句的元组。带右子树的 Result      
 *		节点从不进入规划，因此完全忽略右子树 
 *		（暂时如此）-cim 10/7/89   
 *
 *		只包含常量子句的条件会先被检查，      
 *		然后才做其他处理。如果不满足常量条件，     
 *		它总是返回 'nil'。                             
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecResult(PlanState *pstate)
{
	ResultState *node = castNode(ResultState, pstate);
	TupleTableSlot *outerTupleSlot;
	PlanState  *outerPlan;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	econtext = node->ps.ps_ExprContext;

	/*
	 * 如果尚未检查，则检查 (2 > 1) 这类常量条件      
	 */
	if (node->rs_checkqual)
	{
		bool		qualResult = ExecQual(node->resconstantqual, econtext);

		node->rs_checkqual = false;
		if (!qualResult)
		{
			node->rs_done = true;
			return NULL;
		}
	}

	/*
	 * 重置每元组内存上下文，以释放上一元组周期    
	 * 中分配的表达式求值存储。          
	 */
	ResetExprContext(econtext);

	/*
	 * 如果 rs_done 为真，则意味着我们曾被要求         
	 * 返回常量元组，且上次调用 ExecResult() 时已经     
	 * 做过，或者我们未通过常量条件检查。总之现在      
	 * 完成了。
	 */
	if (!node->rs_done)
	{
		outerPlan = outerPlanState(node);

		if (outerPlan != NULL)
		{
			/*
			 * 从外层计划取元组，直到没有更多为止。      
			 */
			outerTupleSlot = ExecProcNode(outerPlan);

			if (TupIsNull(outerTupleSlot))
				return NULL;

			/*
			 * 准备计算投影表达式，这些表达式将期望以      
			 * varno OUTER 访问输入元组         
			 */
			econtext->ecxt_outertuple = outerTupleSlot;
		}
		else
		{
			/*
			 * 如果我们没有外层计划，那么我们只是在         
			 * 从常量目标列表生成结果。只做一次。   
			 */
			node->rs_done = true;
		}

		/* 用 ExecProject() 形成结果元组并返回            */
		return ExecProject(node->ps.ps_ProjInfo);
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecResultMarkPos
 * ----------------------------------------------------------------
 */
void
ExecResultMarkPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecMarkPos(outerPlan);
	else
		elog(DEBUG2, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecResultRestrPos
 * ----------------------------------------------------------------
 */
void
ExecResultRestrPos(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	if (outerPlan != NULL)
		ExecRestrPos(outerPlan);
	else
		elog(ERROR, "Result nodes do not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecInitResult
 *
 *		为规划器所生成的 Result 节点创建              
 *		运行时状态信息，并初始化外层关系       
 *		(子节点)。
 * ----------------------------------------------------------------
 */
ResultState *
ExecInitResult(Result *node, EState *estate, int eflags)
{
	ResultState *resstate;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)) ||
		   outerPlan(node) != NULL);

	/*
	 * 创建状态结构    
	 */
	resstate = makeNode(ResultState);
	resstate->ps.plan = (Plan *) node;
	resstate->ps.state = estate;
	resstate->ps.ExecProcNode = ExecResult;

	resstate->rs_done = false;
	resstate->rs_checkqual = (node->resconstantqual != NULL);

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &resstate->ps);

	/*
	 * 初始化子节点    
	 */
	outerPlanState(resstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 不用内层计划     
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * 初始化结果槽、类型和投影。     
	 */
	ExecInitResultTupleSlotTL(&resstate->ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&resstate->ps, NULL);

	/*
	 * 初始化子表达式       
	 */
	resstate->ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) resstate);
	resstate->resconstantqual =
		ExecInitQual((List *) node->resconstantqual, (PlanState *) resstate);

	return resstate;
}

/* ----------------------------------------------------------------
 *		ExecEndResult
 *
 *		释放通过 C 例程分配的存储。      
 * ----------------------------------------------------------------
 */
void
ExecEndResult(ResultState *node)
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
ExecReScanResult(ResultState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	node->rs_done = false;
	node->rs_checkqual = (node->resconstantqual != NULL);

	/*
	 * 如果子节点的 chgParam 非空，则该计划会被           
	 * ExecProcNode 重扫
	 */
	if (outerPlan && outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
