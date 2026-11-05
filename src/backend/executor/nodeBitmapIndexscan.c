/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.c
 *	  支持关系位图索引扫描的例程               
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeBitmapIndexscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecBitmapIndexScan	用索引扫描关系。     
 *		ExecInitBitmapIndexScan	创建并初始化状态信息。   
 *		ExecReScanBitmapIndexScan	准备重新扫描计划。 
 *		ExecEndBitmapIndexScan	释放所有存储。 
 */
#include "postgres.h"

#include "access/genam.h"
#include "executor/execdebug.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "utils/memutils.h"


/* ----------------------------------------------------------------
 *		ExecBitmapIndexScan
 *
 *		例行公事式的桩函数  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecBitmapIndexScan(PlanState *pstate)
{
	elog(ERROR, "BitmapIndexScan node does not support ExecProcNode call convention");
	return NULL;
}

/* ----------------------------------------------------------------
 *		MultiExecBitmapIndexScan(node)
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapIndexScan(BitmapIndexScanState *node)
{
	TIDBitmap  *tbm;
	IndexScanDesc scandesc;
	double		nTuples = 0;
	bool		doscan;

	/* 必须提供我们自己的统计支持      */
	if (node->ss.ps.instrument)
		InstrStartNode(node->ss.ps.instrument);

	/*
	 * 从索引扫描节点提取必要信息           
	 */
	scandesc = node->biss_ScanDesc;

	/*
	 * 如果有运行时键且尚未设置，现在就设置它们。        
	 * 数组键也按运行时键处理；注意如果 ExecReScan         
	 * 返回时 biss_RuntimeKeysReady 仍为 false，则存在              
	 * 空数组键，无需操作。    
	 */
	if (!node->biss_RuntimeKeysReady &&
		(node->biss_NumRuntimeKeys != 0 || node->biss_NumArrayKeys != 0))
	{
		ExecReScan((PlanState *) node);
		doscan = node->biss_RuntimeKeysReady;
	}
	else
		doscan = true;

	/*
	 * 准备结果位图。通常我们直接新建一个传回去；      
	 * 不过父节点允许把预先建好的位图存入                   
	 * node->biss_result，此时只需将我们的元组 TID            
	 * 按位或并入现有位图（省去显式 UNION 步骤）  
	 */
	if (node->biss_result)
	{
		tbm = node->biss_result;
		node->biss_result = NULL;	/* 为下次重置     */
	}
	else
	{
		/* XXX 是否该为此用小于 work_mem 的值？*/
		tbm = tbm_create(work_mem * 1024L,
						 ((BitmapIndexScan *) node->ss.ps.plan)->isshared ?
						 node->ss.ps.state->es_query_dsa : NULL);
	}

	/*
	 * 从索引取出 TID 并插入位图       
	 */
	while (doscan)
	{
		nTuples += (double) index_getbitmap(scandesc, tbm);

		CHECK_FOR_INTERRUPTS();

		doscan = ExecIndexAdvanceArrayKeys(node->biss_ArrayKeys,
										   node->biss_NumArrayKeys);
		if (doscan)				/* 重置索引     */
			index_rescan(node->biss_ScanDesc,
						 node->biss_ScanKeys, node->biss_NumScanKeys,
						 NULL, 0);
	}

	/* 必须提供我们自己的统计支持      */
	if (node->ss.ps.instrument)
		InstrStopNode(node->ss.ps.instrument, nTuples);

	return (Node *) tbm;
}

/* ----------------------------------------------------------------
 *		ExecReScanBitmapIndexScan(node)
 *
 *		重新计算值依赖于运行时信息的扫描键的值，   
 *		然后重新扫描索引关系。                               
 * ----------------------------------------------------------------
 */
void
ExecReScanBitmapIndexScan(BitmapIndexScanState *node)
{
	ExprContext *econtext = node->biss_RuntimeContext;

	/*
	 * 重置运行时键上下文，以免每扫描一条外层          
	 * 元组就泄漏一次内存。注意这里假定每次调用       
	 * 都重算运行时键。  
	 */
	if (econtext)
		ResetExprContext(econtext);

	/*
	 * 如果需要做运行时键计算（即任何索引键值都      
	 * 不是简单 Const），则计算新的键值。          
	 *
	 * 数组键也按运行时键处理；注意如果我们返回       
	 * 时 biss_RuntimeKeysReady 仍为 false，则存在空               
	 * 数组键，无需索引扫描 
	 */
	if (node->biss_NumRuntimeKeys != 0)
		ExecIndexEvalRuntimeKeys(econtext,
								 node->biss_RuntimeKeys,
								 node->biss_NumRuntimeKeys);
	if (node->biss_NumArrayKeys != 0)
		node->biss_RuntimeKeysReady =
			ExecIndexEvalArrayKeys(econtext,
								   node->biss_ArrayKeys,
								   node->biss_NumArrayKeys);
	else
		node->biss_RuntimeKeysReady = true;

	/* 重置索引     */
	if (node->biss_RuntimeKeysReady)
		index_rescan(node->biss_ScanDesc,
					 node->biss_ScanKeys, node->biss_NumScanKeys,
					 NULL, 0);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapIndexScan(BitmapIndexScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;

	/*
	 * 从节点提取信息            
	 */
	indexRelationDesc = node->biss_RelationDesc;
	indexScanDesc = node->biss_ScanDesc;

	/*
	 * 释放 exprcontext（死代码，见 ExecFreeExprContext）    
	 */
#ifdef NOT_USED
	if (node->biss_RuntimeContext)
		FreeExprContext(node->biss_RuntimeContext, true);
#endif

	/*
	 * 关闭索引关系（没打开过则为空操作）  
	 */
	if (indexScanDesc)
		index_endscan(indexScanDesc);
	if (indexRelationDesc)
		index_close(indexRelationDesc, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapIndexScan
 *
 *		初始化索引扫描的状态信息。        
 * ----------------------------------------------------------------
 */
BitmapIndexScanState *
ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate, int eflags)
{
	BitmapIndexScanState *indexstate;
	LOCKMODE	lockmode;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	indexstate = makeNode(BitmapIndexScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;
	indexstate->ss.ps.ExecProcNode = ExecBitmapIndexScan;

	/* 通常直到运行时才创建结果位图            */
	indexstate->biss_result = NULL;

	/*
	 * 我们不在此处打开或锁定基础关系。我们假定     
	 * 某个祖先 BitmapHeapScan 节点在整个计划树执行            
	 * 期间对堆关系持有 AccessShareLock 或更强的锁。 
	 */

	indexstate->ss.ss_currentRelation = NULL;
	indexstate->ss.ss_currentScanDesc = NULL;

	/*
	 * 其他初始化             
	 *
	 * 本节点不需要标准 exprcontext，不过我们稍后         
	 * 可能决定创建运行时键 exprcontext      
	 */

	/*
	 * 初始化子表达式       
	 *
	 * 无需初始化目标列表或 qual，因为两者都不使用。     
	 *
	 * 注意：我们并非初始化整个 indexqual 表达式，        
	 * 只初始化对应运行时键的部分。          
	 */

	/*
	 * 如果只是执行 EXPLAIN（即不打算运行计划），          
	 * 就到此为止。这样索引顾问插件就能对引用不存在     
	 * 索引的计划执行 EXPLAIN。  
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return indexstate;

	/* 打开索引关系。    */
	lockmode = exec_rt_fetch(node->scan.scanrelid, estate)->rellockmode;
	indexstate->biss_RelationDesc = index_open(node->indexid, lockmode);

	/*
	 * 初始化索引专有的扫描状态
	 */
	indexstate->biss_RuntimeKeysReady = false;
	indexstate->biss_RuntimeKeys = NULL;
	indexstate->biss_NumRuntimeKeys = 0;

	/*
	 * 从索引条件构建索引扫描键                  
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->biss_RelationDesc,
						   node->indexqual,
						   false,
						   &indexstate->biss_ScanKeys,
						   &indexstate->biss_NumScanKeys,
						   &indexstate->biss_RuntimeKeys,
						   &indexstate->biss_NumRuntimeKeys,
						   &indexstate->biss_ArrayKeys,
						   &indexstate->biss_NumArrayKeys);

	/*
	 * 如果有运行时键或数组键，就需要一个             
	 * ExprContext 来求值。本可以直接创建"标准"的               
	 * 计划节点 exprcontext，但为了让代码看起来与        
	 * nodeIndexscan.c 一致，故最好沿用独立 ExprContext        
	 */
	if (indexstate->biss_NumRuntimeKeys != 0 ||
		indexstate->biss_NumArrayKeys != 0)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->biss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->biss_RuntimeContext = NULL;
	}

	/*
	 * 初始化扫描描述符。
	 */
	indexstate->biss_ScanDesc =
		index_beginscan_bitmap(indexstate->biss_RelationDesc,
							   estate->es_snapshot,
							   indexstate->biss_NumScanKeys);

	/*
	 * 若没有要计算的运行时键，就直接把扫描键传给索引  
	 * AM。    
	 */
	if (indexstate->biss_NumRuntimeKeys == 0 &&
		indexstate->biss_NumArrayKeys == 0)
		index_rescan(indexstate->biss_ScanDesc,
					 indexstate->biss_ScanKeys, indexstate->biss_NumScanKeys,
					 NULL, 0);

	/*
	 * 完成。
	 */
	return indexstate;
}
