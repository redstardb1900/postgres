/*-------------------------------------------------------------------------
 *
 * nodeGroup.c
 *	  处理 group 节点的例程（用于带 GROUP BY 子句的查询）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * DESCRIPTION
 *	  Group 节点用于处理带 GROUP BY 子句的查询。                
 *	  其外层计划必须按分组列指定的顺序交付元组，
 *	  且元组已排序（即同一组的元组是连续的）。因此，
 *	  我们只需比较相邻元组，即可确定                      
 *	  组的边界。         
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGroup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeGroup.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/*
 *	 ExecGroup -
 *
 *		为每组匹配的输入元组返回一个元组。      
 */
static TupleTableSlot *
ExecGroup(PlanState *pstate)
{
	GroupState *node = castNode(GroupState, pstate);
	ExprContext *econtext;
	TupleTableSlot *firsttupleslot;
	TupleTableSlot *outerslot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点取状态信息
	 */
	if (node->grp_done)
		return NULL;
	econtext = node->ss.ps.ps_ExprContext;

	/*
	 * ScanTupleSlot 保存每组第一个元组（已复制）。    
	 */
	firsttupleslot = node->ss.ss_ScanTupleSlot;

	/*
	 * 这里无需调用 ResetExprContext，因为 ExecQualAndReset() 会   
	 * 每输入元组重置一次每元组内存上下文。  
	 */

	/*
	 * 如果是首次执行，则获取第一个输入元组，并确定    
	 * 是否返回它。  
	 */
	if (TupIsNull(firsttupleslot))
	{
		outerslot = ExecProcNode(outerPlanState(node));
		if (TupIsNull(outerslot))
		{
			/* 输入为空，故不返回    */
			node->grp_done = true;
			return NULL;
		}
		/* 复制元组至 firsttupleslot */
		ExecCopySlot(firsttupleslot, outerslot);

		/*
		 * 把它设置为条件测试和投影的输入。这些表达式  
		 * 将按 varno OUTER 访问输入元组。   
		 */
		econtext->ecxt_outertuple = firsttupleslot;

		/*
		 * 检查 qual（HAVING 子句）；如果该组不匹配，则忽略 
		 * 并落入扫描循环。   
		 */
		if (ExecQual(node->ss.ps.qual, econtext))
		{
			/*
			 * 使用第一个输入元组构建并返回投影元组。      
			 */
			return ExecProject(node->ss.ps.ps_ProjInfo);
		}
		else
			InstrCountFiltered1(node, 1);
	}

	/*
	 * 本循环对每组输入元组迭代一次。在循环开头处，
	 * 我们已经处理完该组的第一个元组，现在需要扫描    
	 * 该组的所有其余成员。                
	 */
	for (;;)
	{
		/*
		 * 扫描属于该组的所有其余元组                 
		 */
		for (;;)
		{
			outerslot = ExecProcNode(outerPlanState(node));
			if (TupIsNull(outerslot))
			{
				/* 没有更多组，就此结束*/
				node->grp_done = true;
				return NULL;
			}

			/*
			 * 与第一个元组比较，看该元组是否属于          
			 * 同一组。若是，忽略并继续扫描 
			 */
			econtext->ecxt_innertuple = firsttupleslot;
			econtext->ecxt_outertuple = outerslot;
			if (!ExecQualAndReset(node->eqfunction, econtext))
				break;
		}

		/*
		 * 我们已拿到下一个输入组的首个元组。看看是否    
		 * 返回它 
		 */
		/* 复制元组，设为条件测试和投影的输入      */
		ExecCopySlot(firsttupleslot, outerslot);
		econtext->ecxt_outertuple = firsttupleslot;

		/*
		 * 检查 qual（HAVING 子句）；如果该组不匹配，则忽略 
		 * 它并回头扫描该组的其余部分。     
		 */
		if (ExecQual(node->ss.ps.qual, econtext))
		{
			/*
			 * 使用第一个输入元组构建并返回投影元组。      
			 */
			return ExecProject(node->ss.ps.ps_ProjInfo);
		}
		else
			InstrCountFiltered1(node, 1);
	}
}

/* -----------------
 * ExecInitGroup
 *
 *	为规划器生成的 group 节点创建运行期信息，         
 *	并初始化其外层子树              
 * -----------------
 */
GroupState *
ExecInitGroup(Group *node, EState *estate, int eflags)
{
	GroupState *grpstate;
	const TupleTableSlotOps *tts_ops;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	grpstate = makeNode(GroupState);
	grpstate->ss.ps.plan = (Plan *) node;
	grpstate->ss.ps.state = estate;
	grpstate->ss.ps.ExecProcNode = ExecGroup;
	grpstate->grp_done = false;

	/*
	 * 创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &grpstate->ss.ps);

	/*
	 * 初始化子节点    
	 */
	outerPlanState(grpstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 初始化扫描槽与类型。
	 */
	tts_ops = ExecGetResultSlotOps(outerPlanState(&grpstate->ss), NULL);
	ExecCreateScanSlotFromOuterPlan(estate, &grpstate->ss, tts_ops);

	/*
	 * 初始化结果槽、类型与投影。     
	 */
	ExecInitResultTupleSlotTL(&grpstate->ss.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&grpstate->ss.ps, NULL);

	/*
	 * 初始化子表达式       
	 */
	grpstate->ss.ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) grpstate);

	/*
	 * 为内层循环预计算 fmgr 查找数据
	 */
	grpstate->eqfunction =
		execTuplesMatchPrepare(ExecGetResultType(outerPlanState(grpstate)),
							   node->numCols,
							   node->grpColIdx,
							   node->grpOperators,
							   node->grpCollations,
							   &grpstate->ss.ps);

	return grpstate;
}

/* ------------------------
 *		ExecEndGroup(node)
 *
 * -----------------------
 */
void
ExecEndGroup(GroupState *node)
{
	PlanState  *outerPlan;

	ExecFreeExprContext(&node->ss.ps);

	/* 清理元组表      */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanGroup(GroupState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	node->grp_done = false;
	/* 必须清空首元组  */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 如果子节点的 chgParam 非空，计划将重新扫描于     
	 * 首次 ExecProcNode
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
