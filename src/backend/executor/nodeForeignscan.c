/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.c
 *	  支撑外表扫描的例程。             
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeForeignscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *
 *		ExecForeignScan			扫描一个外表。 
 *		ExecInitForeignScan		创建并初始化状态信息。  
 *		ExecReScanForeignScan	重新扫描外部关系。  
 *		ExecEndForeignScan		释放已分配的任何资源。
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeForeignscan.h"
#include "foreign/fdwapi.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static TupleTableSlot *ForeignNext(ForeignScanState *node);
static bool ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot);


/* ----------------------------------------------------------------
 *		ForeignNext
 *
 *		这是 ExecForeignScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ForeignNext(ForeignScanState *node)
{
	TupleTableSlot *slot;
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	MemoryContext oldcontext;

	/* 在短命上下文中调用 Iterate 函数       */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	if (plan->operation != CMD_SELECT)
	{
		/*
		 * 直接修改无法重新求值，因此在 EvalPlanQual 处理    
		 * 期间不应到达此处      
		 */
		Assert(node->ss.ps.state->es_epq_active == NULL);

		slot = node->fdwroutine->IterateDirectModify(node);
	}
	else
		slot = node->fdwroutine->IterateForeignScan(node);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 向 tableoid 中插入有效值，它是唯一有用的系统    
	 * 列。 
	 */
	if (plan->fsSystemCol && !TupIsNull(slot))
		slot->tts_tableOid = RelationGetRelid(node->ss.ss_currentRelation);

	return slot;
}

/*
 * ForeignRecheck -- 在 EvalPlanQual 中重检元组的访问方法例程   
 */
static bool
ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot)
{
	FdwRoutine *fdwroutine = node->fdwroutine;
	ExprContext *econtext;

	/*
	 * 从外表扫描节点提取必要信息             
	 */
	econtext = node->ss.ps.ps_ExprContext;

	/* 该元组是否满足远端 qual 条件？     */
	econtext->ecxt_scantuple = slot;

	ResetExprContext(econtext);

	/*
	 * 如果下推外连接，RecheckForeignScan 可能需要在槽中存储 
	 * 不同的元组，因为检查时可能有不同的一组列变为    
	 * NULL。否则，它不应需要更改槽内容，只需返回 true    
	 * 或 false 指示 qual 是否仍然通过。对于简单情形          
	 * 设置 fdw_recheck_quals 可能比                                   
	 * 提供回调更容易。
	 */
	if (fdwroutine->RecheckForeignScan &&
		!fdwroutine->RecheckForeignScan(node, slot))
		return false;

	return ExecQual(node->fdw_recheck_quals, econtext);
}

/* ----------------------------------------------------------------
 *		ExecForeignScan(node)
 *
 *		从 FDW 取得下一个元组，检查本地 qual，并     
 *		返回它  
 *		我们调用 ExecScan() 例程，并向它传递相应的 
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecForeignScan(PlanState *pstate)
{
	ForeignScanState *node = castNode(ForeignScanState, pstate);
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;

	/*
	 * 当 EvalPlanQual 处于活动状态时忽略直接修改 --- 它们 
	 * 与 EvalPlanQual 重检无关         
	 */
	if (estate->es_epq_active != NULL && plan->operation != CMD_SELECT)
		return NULL;

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) ForeignNext,
					(ExecScanRecheckMtd) ForeignRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitForeignScan
 * ----------------------------------------------------------------
 */
ForeignScanState *
ExecInitForeignScan(ForeignScan *node, EState *estate, int eflags)
{
	ForeignScanState *scanstate;
	Relation	currentRelation = NULL;
	Index		scanrelid = node->scan.scanrelid;
	int			tlistvarno;
	FdwRoutine *fdwroutine;

	/* 检查不受支持的标志 */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * 创建状态结构    
	 */
	scanstate = makeNode(ForeignScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecForeignScan;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 打开扫描关系（如果有）；同时从 FDW 处理函数取得   
	 * 函数指针 
	 */
	if (scanrelid > 0)
	{
		currentRelation = ExecOpenScanRelation(estate, scanrelid, eflags);
		scanstate->ss.ss_currentRelation = currentRelation;
		fdwroutine = GetFdwRoutineForRelation(currentRelation, true);
	}
	else
	{
		/* 无法用 relcache，只能费力地获取 fdwroutine     */
		fdwroutine = GetFdwRoutineByServerId(node->fs_server);
	}

	/*
	 * 确定扫描元组类型。如果 FDW 提供描述扫描元组  
	 * 的目标列表就采用它；否则使用基础关系的行类型。  
	 */
	if (node->fdw_scan_tlist != NIL || currentRelation == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(node->fdw_scan_tlist);
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* 节点的目标列表将包含 varno = INDEX_VAR 的 Var   */
		tlistvarno = INDEX_VAR;
	}
	else
	{
		TupleDesc	scan_tupdesc;

		/* 不要指望 FDW 会返回满足 NOT NULL 约束的元组         */
		scan_tupdesc = CreateTupleDescCopy(RelationGetDescr(currentRelation));
		ExecInitScanTupleSlot(estate, &scanstate->ss, scan_tupdesc,
							  &TTSOpsHeapTuple);
		/* 节点的目标列表将包含 varno = scanrelid 的 Var   */
		tlistvarno = scanrelid;
	}

	/* 不知道 FDW 可能返回什么    */
	scanstate->ss.ps.scanopsfixed = false;
	scanstate->ss.ps.scanopsset = true;

	/*
	 * 初始化结果槽、类型与投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfoWithVarno(&scanstate->ss, tlistvarno);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);
	scanstate->fdw_recheck_quals =
		ExecInitQual(node->fdw_recheck_quals, (PlanState *) scanstate);

	/*
	 * 确定是否异步扫描外部关系；这一点必须与            
	 * ExecInitAppend() 中的代码保持一致。                  
	 */
	scanstate->ss.ps.async_capable = (((Plan *) node)->async_capable &&
									  estate->es_epq_active == NULL);

	/*
	 * 初始化 FDW 相关状态   
	 */
	scanstate->fdwroutine = fdwroutine;
	scanstate->fdw_state = NULL;

	/*
	 * 为方便 FDW，查找修改目标关系                             
	 * 的 ResultRelInfo。ModifyTable 节点应当已把它初始化好，   
	 * 见 ExecInitModifyTable 
	 *
	 * 不过当 EvalPlanQual 处于活动状态时，不要尝试查找  
	 * ResultRelInfo。直接修改无法作为 EvalPlanQual 的一   
	 * 部分重新求值。且该查找无法工作，因为在    
	 * EvalPlanQual 处理期间，EvalPlanQual 只初始化             
	 * ModifyTable 之下子树，不运行 ExecInitModifyTable   
	 */
	if (node->resultRelation > 0 && estate->es_epq_active == NULL)
	{
		if (estate->es_result_relations == NULL ||
			estate->es_result_relations[node->resultRelation - 1] == NULL)
		{
			elog(ERROR, "result relation not initialized");
		}
		scanstate->resultRelInfo = estate->es_result_relations[node->resultRelation - 1];
	}

	/* 初始化各外层计划。*/
	if (outerPlan(node))
		outerPlanState(scanstate) =
			ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 让 FDW 初始化扫描。          
	 */
	if (node->operation != CMD_SELECT)
	{
		/*
		 * 直接修改无法被 EvalPlanQual 重新求值，所以       
		 * 不必费心准备 FDW。      
		 *
		 * 对于带外表目标的继承 UPDATE/DELETE，其 EvalPlanQual 
		 * 子树中可能有 direct-modify 的 ForeignScan 节点，         
		 * 因此要在 EvalPlanQual 处理期间忽略这类 ForeignScan 
		 * 节点。另见 ExecForeignScan/ExecReScanForeignScan。    
		 */
		if (estate->es_epq_active == NULL)
			fdwroutine->BeginDirectModify(scanstate, eflags);
	}
	else
		fdwroutine->BeginForeignScan(scanstate, eflags);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndForeignScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;

	/* 让 FDW 关闭        */
	if (plan->operation != CMD_SELECT)
	{
		if (estate->es_epq_active == NULL)
			node->fdwroutine->EndDirectModify(node);
	}
	else
		node->fdwroutine->EndForeignScan(node);

	/* 关闭各外层计划     */
	if (outerPlanState(node))
		ExecEndNode(outerPlanState(node));

	/* 释放 exprcontext   */
	ExecFreeExprContext(&node->ss.ps);

	/* 清空元组表           */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecReScanForeignScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanForeignScan(ForeignScanState *node)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 当 EvalPlanQual 处于活动状态时忽略直接修改 --- 它们 
	 * 与 EvalPlanQual 重检无关         
	 */
	if (estate->es_epq_active != NULL && plan->operation != CMD_SELECT)
		return;

	node->fdwroutine->ReScanForeignScan(node);

	/*
	 * 如果子节点的 chgParam 非空，那么第一个 ExecProcNode 
	 * 就会重新扫描该计划。outerPlan 也可能为 NULL，那样     
	 * 完全无需再扫描。 
	 */
	if (outerPlan != NULL && outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecForeignScanEstimate
 *
 *		告知并行协调信息的大小（如果有）             
 * ----------------------------------------------------------------
 */
void
ExecForeignScanEstimate(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->EstimateDSMForeignScan)
	{
		node->pscan_len = fdwroutine->EstimateDSMForeignScan(node, pcxt);
		shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeDSM
 *
 *		初始化并行协调信息                     
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_allocate(pcxt->toc, node->pscan_len);
		fdwroutine->InitializeDSMForeignScan(node, pcxt, coordinate);
		shm_toc_insert(pcxt->toc, plan_node_id, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanReInitializeDSM
 *
 *		在开始全新扫描前重置共享状态。    
 * ----------------------------------------------------------------
 */
void
ExecForeignScanReInitializeDSM(ForeignScanState *node, ParallelContext *pcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ReInitializeDSMForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pcxt->toc, plan_node_id, false);
		fdwroutine->ReInitializeDSMForeignScan(node, pcxt, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecForeignScanInitializeWorker
 *
 *		根据并行协调信息进行初始化                          
 * ----------------------------------------------------------------
 */
void
ExecForeignScanInitializeWorker(ForeignScanState *node,
								ParallelWorkerContext *pwcxt)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->InitializeWorkerForeignScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pwcxt->toc, plan_node_id, false);
		fdwroutine->InitializeWorkerForeignScan(node, pwcxt->toc, coordinate);
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownForeignScan
 *
 *		让 FDW 有机会停止异步资源消耗                 
 *		并释放仍持有的任何资源。 
 * ----------------------------------------------------------------
 */
void
ExecShutdownForeignScan(ForeignScanState *node)
{
	FdwRoutine *fdwroutine = node->fdwroutine;

	if (fdwroutine->ShutdownForeignScan)
		fdwroutine->ShutdownForeignScan(node);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanRequest
 *
 *		以异步方式从指定的支持异步的节点请求元组     
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanRequest(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncRequest != NULL);
	fdwroutine->ForeignAsyncRequest(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanConfigureWait
 *
 *		在异步模式下配置等待     
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanConfigureWait(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncConfigureWait != NULL);
	fdwroutine->ForeignAsyncConfigureWait(areq);
}

/* ----------------------------------------------------------------
 *		ExecAsyncForeignScanNotify
 *
 *		发生相关事件时调用的回调               
 * ----------------------------------------------------------------
 */
void
ExecAsyncForeignScanNotify(AsyncRequest *areq)
{
	ForeignScanState *node = (ForeignScanState *) areq->requestee;
	FdwRoutine *fdwroutine = node->fdwroutine;

	Assert(fdwroutine->ForeignAsyncNotify != NULL);
	fdwroutine->ForeignAsyncNotify(areq);
}
