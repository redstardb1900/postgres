/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  对查询进行唯一化处理的例程                   
 *
 * Unique 是很简单的节点类型，只从子计划送来的    
 * 有序元组流中滤除重复元组。它本质上是                   
 * Group 的简化版本：去重功能与之完全相同。          
 * 但 Unique 既不做投影也不做条件检查，                  
 * 因此在两者都不需要的情况下效率略高。              
 * （多携带一种计划节点类型所换取的节省              
 * 是否划算）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeUnique.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecUnique - 生成去重后的临时关系         
 *		ExecInitUnique - 初始化节点与子节点 
 *		ExecEndUnique - 关闭节点与子节点  
 *
 * NOTES
 *		假定从子计划返回的元组是          
 *		有序的。 
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeUnique.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecUnique
 * ----------------------------------------------------------------
 */
static TupleTableSlot *			/* 返回：元组或 NULL */
ExecUnique(PlanState *pstate)
{
	UniqueState *node = castNode(UniqueState, pstate);
	ExprContext *econtext = node->ps.ps_ExprContext;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	PlanState  *outerPlan;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点获取信息        
	 */
	outerPlan = outerPlanState(node);
	resultTupleSlot = node->ps.ps_ResultTupleSlot;

	/*
	 * 现在开始循环，只返回非重复元组。我们假定     
	 * 元组按有序顺序到达，因此很容易检测重复。         
	 * 每组首个元组将被返回。     
	 */
	for (;;)
	{
		/*
		 * 从外层子计划取一个元组   
		 */
		slot = ExecProcNode(outerPlan);
		if (TupIsNull(slot))
		{
			/* 子计划结束，完成      */
			ExecClearTuple(resultTupleSlot);
			return NULL;
		}

		/*
		 * 总是返回子计划的第一个元组。     
		 */
		if (TupIsNull(resultTupleSlot))
			break;

		/*
		 * 否则测试新元组与之前返回的元组是否匹配。       
		 * 若匹配，则回头循环，从子计划再取新元组   
		 * 即可  
		 */
		econtext->ecxt_innertuple = slot;
		econtext->ecxt_outertuple = resultTupleSlot;
		if (!ExecQualAndReset(node->eqfunction, econtext))
			break;
	}

	/*
	 * 新元组与之前保存的元组（如有）不同。               
	 * 保存并返回它。必须复制一份，因为源子计划      
	 * 并不保证取下一个源元组之后该元组                
	 * 仍可访问。                
	 */
	return ExecCopySlot(resultTupleSlot, slot);
}

/* ----------------------------------------------------------------
 *		ExecInitUnique
 *
 *		初始化 Unique 节点的状态结构               
 *		与子计划。    
 * ----------------------------------------------------------------
 */
UniqueState *
ExecInitUnique(Unique *node, EState *estate, int eflags)
{
	UniqueState *uniquestate;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	uniquestate = makeNode(UniqueState);
	uniquestate->ps.plan = (Plan *) node;
	uniquestate->ps.state = estate;
	uniquestate->ps.ExecProcNode = ExecUnique;

	/*
	 * 创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &uniquestate->ps);

	/*
	 * 再初始化外层计划  
	 */
	outerPlanState(uniquestate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 初始化结果槽与类型。Unique 节点不做投影，因此   
	 * 只需对该节点恰当地初始化投影信息。    
	 */
	ExecInitResultTupleSlotTL(&uniquestate->ps, &TTSOpsMinimalTuple);
	uniquestate->ps.ps_ProjInfo = NULL;

	/*
	 * 为内层循环预计算 fmgr 查找数据
	 */
	uniquestate->eqfunction =
		execTuplesMatchPrepare(ExecGetResultType(outerPlanState(uniquestate)),
							   node->numCols,
							   node->uniqColIdx,
							   node->uniqOperators,
							   node->uniqCollations,
							   &uniquestate->ps);

	return uniquestate;
}

/* ----------------------------------------------------------------
 *		ExecEndUnique
 *
 *		此例程关闭子计划，并释放其                  
 *		所耗资源 
 * ----------------------------------------------------------------
 */
void
ExecEndUnique(UniqueState *node)
{
	/* 清理元组表      */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	ExecFreeExprContext(&node->ps);

	ExecEndNode(outerPlanState(node));
}


void
ExecReScanUnique(UniqueState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* 需清空结果元组，才能返回首个输入元组   */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * 如果子节点的 chgParam 非空，则该计划会被           
	 * ExecProcNode 重扫
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
