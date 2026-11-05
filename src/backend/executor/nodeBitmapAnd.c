/*-------------------------------------------------------------------------
 *
 * nodeBitmapAnd.c
 *	  处理 BitmapAnd 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapAnd.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitBitmapAnd	- 初始化 BitmapAnd 节点   
 *		MultiExecBitmapAnd	- 从节点取回结果位图             
 *		ExecEndBitmapAnd	- 关闭 BitmapAnd 节点     
 *		ExecReScanBitmapAnd - 重扫 BitmapAnd 节点  
 *
 *	 NOTES
 *		BitmapAnd 节点不使用其左右子树，而是像  
 *		Append 节点那样维护一份子计划列表。 
 *		不过其逻辑比 Append 简单得多，因为我们  
 *		无须应付正向/反向                                   
 *		执行。 
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeBitmapAnd.h"


/* ----------------------------------------------------------------
 *		ExecBitmapAnd
 *
 *		为形式合规而设的桩  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecBitmapAnd(PlanState *pstate)
{
	elog(ERROR, "BitmapAnd node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapAnd
 *
 *		启动 BitmapAnd 节点的全部子扫描。    
 * ----------------------------------------------------------------
 */
BitmapAndState *
ExecInitBitmapAnd(BitmapAnd *node, EState *estate, int eflags)
{
	BitmapAndState *bitmapandstate = makeNode(BitmapAndState);
	PlanState **bitmapplanstates;
	int			nplans;
	int			i;
	ListCell   *l;
	Plan	   *initNode;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 建立空的子计划状态数组    
	 */
	nplans = list_length(node->bitmapplans);

	bitmapplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * 为本 BitmapAnd 节点新建 BitmapAndState    
	 */
	bitmapandstate->ps.plan = (Plan *) node;
	bitmapandstate->ps.state = estate;
	bitmapandstate->ps.ExecProcNode = ExecBitmapAnd;
	bitmapandstate->bitmapplans = bitmapplanstates;
	bitmapandstate->nplans = nplans;

	/*
	 * 对每个待执行的计划调用 ExecInitNode，并把结果     
	 * 保存到 "bitmapplanstates" 数组中。 
	 */
	i = 0;
	foreach(l, node->bitmapplans)
	{
		initNode = (Plan *) lfirst(l);
		bitmapplanstates[i] = ExecInitNode(initNode, estate, eflags);
		i++;
	}

	/*
	 * 杂项初始化             
	 *
	 * BitmapAnd 计划没有表达式上下文，因为它们从不调用   
	 * ExecQual 或 ExecProject。它们也不需要任何元组槽。  
	 */

	return bitmapandstate;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapAnd
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;
	TIDBitmap  *result = NULL;

	/* 必须提供我们自己的统计信息支持*/
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	/*
	 * 从节点获取信息        
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;

	/*
	 * 扫描所有子计划并 AND 其结果位图      
	 */
	for (i = 0; i < nplans; i++)
	{
		PlanState  *subnode = bitmapplans[i];
		TIDBitmap  *subresult;

		subresult = (TIDBitmap *) MultiExecProcNode(subnode);

		if (!subresult || !IsA(subresult, TIDBitmap))
			elog(ERROR, "unrecognized result from subplan");

		if (result == NULL)
			result = subresult; /* 首子计划  */
		else
		{
			tbm_intersect(result, subresult);
			tbm_free(subresult);
		}

		/*
		 * 如果任一阶段得到完全为空的位图，我们就可以   
		 * 提前退出，不再评估其余子计划，因为把它们       
		 * AND 起来已不可能再改变结果。（注：indxpath.c      
		 * 按选择率排序这一事实，应能让该情况更常发生 
		 * ）。 
		 */
		if (tbm_is_empty(result))
			break;
	}

	if (result == NULL)
		elog(ERROR, "BitmapAnd doesn't support zero inputs");

	/* 必须提供我们自己的统计信息支持*/
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, 0 /* XXX */ );

	return (Node *) result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapAnd
 *
 *		关闭 BitmapAnd 节点的各子扫描。     
 *
 *		无有价值的返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapAnd(BitmapAndState *node)
{
	PlanState **bitmapplans;
	int			nplans;
	int			i;

	/*
	 * 从节点获取信息        
	 */
	bitmapplans = node->bitmapplans;
	nplans = node->nplans;

	/*
	 * 关闭每一个子扫描（已初始化的那些）    
	 */
	for (i = 0; i < nplans; i++)
	{
		if (bitmapplans[i])
			ExecEndNode(bitmapplans[i]);
	}
}

void
ExecReScanBitmapAnd(BitmapAndState *node)
{
	int			i;

	for (i = 0; i < node->nplans; i++)
	{
		PlanState  *subnode = node->bitmapplans[i];

		/*
		 * ExecReScan 不了解我的子计划，所以我得亲自  
		 * 完成变更参数的通知。     
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * 若子节点的 chgParam 非空，则将由首次 ExecProcNode    
		 * 重扫该计划。 
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
}
