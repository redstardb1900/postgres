/*-------------------------------------------------------------------------
 *
 * nodeBitmapOr.c
 *	  处理 BitmapOr 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapOr.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitBitmapOr	初始化 BitmapOr 节点。  
 *		MultiExecBitmapOr	从该节点取回结果位图。         
 *		ExecEndBitmapOr		关闭 BitmapOr 节点。    
 *		ExecReScanBitmapOr	重扫 BitmapOr 节点    
 *
 *	 NOTES
 *		BitmapOr 节点并不使用其左右子树，        
 *		而是维护一份子计划列表，              
 *		很像 Append 节点。不过其逻辑要比 Append    
 *		简单得多，因为无需应对前向/反向              
 *		执行。 
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeBitmapOr.h"
#include "miscadmin.h"


/* ----------------------------------------------------------------
 *		ExecBitmapOr
 *
 *		为形式合规而设的桩  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecBitmapOr(PlanState *pstate)
{
	elog(ERROR, "BitmapOr node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapOr
 *
 *		启动 BitmapOr 节点的所有子扫描。    
 * ----------------------------------------------------------------
 */
BitmapOrState *
ExecInitBitmapOr(BitmapOr *node, EState *estate, int eflags)
{
	BitmapOrState *bitmaporstate = makeNode(BitmapOrState);
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
	 * 为 BitmapOr 节点新建 BitmapOrState       
	 */
	bitmaporstate->ps.plan = (Plan *) node;
	bitmaporstate->ps.state = estate;
	bitmaporstate->ps.ExecProcNode = ExecBitmapOr;
	bitmaporstate->bitmapplans = bitmapplanstates;
	bitmaporstate->nplans = nplans;

	/*
	 * 对每个待执行的计划调用 ExecInitNode，并把结果     
	 * 保存到数组 "bitmapplanstates" 中。 
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
	 * BitmapOr 计划没有表达式上下文，因为它们从不调用   
	 * ExecQual 或 ExecProject。它们也不需要任何元组槽。  
	 */

	return bitmaporstate;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapOr
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapOr(BitmapOrState *node)
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
	 * 扫描全部子计划，OR 其结果位图。    
	 */
	for (i = 0; i < nplans; i++)
	{
		PlanState  *subnode = bitmapplans[i];
		TIDBitmap  *subresult;

		/*
		 * 我们可以对 BitmapIndexScan 子节点做特殊处理，      
		 * 以免每个子节点都需显式 tbm_union 步骤：只需     
		 * 传入当前结果位图，让其直接 OR。
		 */
		if (IsA(subnode, BitmapIndexScanState))
		{
			if (result == NULL) /* 首子计划  */
			{
				/* XXX 是否该为此用小于 work_mem 的值？*/
				result = tbm_create(work_mem * 1024L,
									((BitmapOr *) node->ps.plan)->isshared ?
									node->ps.state->es_query_dsa : NULL);
			}

			((BitmapIndexScanState *) subnode)->biss_result = result;

			subresult = (TIDBitmap *) MultiExecProcNode(subnode);

			if (subresult != result)
				elog(ERROR, "unrecognized result from subplan");
		}
		else
		{
			/* 标准实现            */
			subresult = (TIDBitmap *) MultiExecProcNode(subnode);

			if (!subresult || !IsA(subresult, TIDBitmap))
				elog(ERROR, "unrecognized result from subplan");

			if (result == NULL)
				result = subresult; /* 首子计划  */
			else
			{
				tbm_union(result, subresult);
				tbm_free(subresult);
			}
		}
	}

	/* 这里我们能返回空结果集吗？   */
	if (result == NULL)
		elog(ERROR, "BitmapOr doesn't support zero inputs");

	/* 必须提供我们自己的统计信息支持*/
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, 0 /* XXX */ );

	return (Node *) result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapOr
 *
 *		关闭 BitmapOr 节点的各子扫描。     
 *
 *		无有价值的返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapOr(BitmapOrState *node)
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
	 * 关闭各子扫描（仅限已初始化的）。       
	 */
	for (i = 0; i < nplans; i++)
	{
		if (bitmapplans[i])
			ExecEndNode(bitmapplans[i]);
	}
}

void
ExecReScanBitmapOr(BitmapOrState *node)
{
	int			i;

	for (i = 0; i < node->nplans; i++)
	{
		PlanState  *subnode = node->bitmapplans[i];

		/*
		 * ExecReScan 并不了解我的子计划，所以变更     
		 * 参数信号须自行发出。     
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * 若子节点的 chgParam 非空，计划会被首次 ExecProcNode 
		 * 重新扫描。    
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
}
