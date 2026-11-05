/* ------------------------------------------------------------------------
 *
 * nodeCustom.c
 *		处理自定义扫描节点执行的例程。   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static TupleTableSlot *ExecCustomScan(PlanState *pstate);


CustomScanState *
ExecInitCustomScan(CustomScan *cscan, EState *estate, int eflags)
{
	CustomScanState *css;
	const TupleTableSlotOps *slotOps;
	Relation	scan_rel = NULL;
	Index		scanrelid = cscan->scan.scanrelid;
	int			tlistvarno;

	/*
	 * 分配 CustomScanState 对象。我们让自定义扫描提供者    
	 * 自行执行 palloc，以便它能构造一个把 CustomScanState  
	 * 作为第一个字段嵌入其中的更大对象。此时它必须把
	 * 节点标签与 methods 字段设置正确。其他标准字段应    
	 * 置为零。
	 */
	css = castNode(CustomScanState,
				   cscan->methods->CreateCustomScanState(cscan));

	/* 确保 flags 被正确填充     */
	css->flags = cscan->flags;

	/* 填充 ScanState 字段     */
	css->ss.ps.plan = &cscan->scan.plan;
	css->ss.ps.state = estate;
	css->ss.ps.ExecProcNode = ExecCustomScan;

	/* 为节点创建表达式上下文  */
	ExecAssignExprContext(estate, &css->ss.ps);

	/*
	 * 打开扫描关系（如有）
	 */
	if (scanrelid > 0)
	{
		scan_rel = ExecOpenScanRelation(estate, scanrelid, eflags);
		css->ss.ss_currentRelation = scan_rel;
	}

	/*
	 * 若 CustomScanState 中指定了自定义槽则用它，否则用虚 
	 * 拟槽。 
	 */
	slotOps = css->slotOps;
	if (!slotOps)
		slotOps = &TTSOpsVirtual;

	/*
	 * 确定扫描元组类型。若自定义扫描提供者提供了描述 
	 * 扫描元组的目标列表，则用之；否则使用基础  
	 * 行类型。       
	 */
	if (cscan->custom_scan_tlist != NIL || scan_rel == NULL)
	{
		TupleDesc	scan_tupdesc;

		scan_tupdesc = ExecTypeFromTL(cscan->custom_scan_tlist);
		ExecInitScanTupleSlot(estate, &css->ss, scan_tupdesc, slotOps);
		/* 节点的目标列表将包含 varno = INDEX_VAR 的 Var。*/
		tlistvarno = INDEX_VAR;
	}
	else
	{
		ExecInitScanTupleSlot(estate, &css->ss, RelationGetDescr(scan_rel),
							  slotOps);
		/* 节点的目标列表将包含 varno = scanrelid 的 Var。*/
		tlistvarno = scanrelid;
	}

	/*
	 * 初始化结果槽、类型和投影。     
	 */
	ExecInitResultTupleSlotTL(&css->ss.ps, &TTSOpsVirtual);
	ExecAssignScanProjectionInfoWithVarno(&css->ss, tlistvarno);

	/* 初始化子表达式        */
	css->ss.ps.qual =
		ExecInitQual(cscan->scan.plan.qual, (PlanState *) css);

	/*
	 * 自定义扫描提供者的回调按照其自身逻辑，对自定义
	 * 扫描状态节点做最后的初始化。           
	 */
	css->methods->BeginCustomScan(css, estate, eflags);

	return css;
}

static TupleTableSlot *
ExecCustomScan(PlanState *pstate)
{
	CustomScanState *node = castNode(CustomScanState, pstate);

	CHECK_FOR_INTERRUPTS();

	Assert(node->methods->ExecCustomScan != NULL);
	return node->methods->ExecCustomScan(node);
}

void
ExecEndCustomScan(CustomScanState *node)
{
	Assert(node->methods->EndCustomScan != NULL);
	node->methods->EndCustomScan(node);

	/* 释放 exprcontext   */
	ExecFreeExprContext(&node->ss.ps);

	/* 清空元组表           */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
}

void
ExecReScanCustomScan(CustomScanState *node)
{
	Assert(node->methods->ReScanCustomScan != NULL);
	node->methods->ReScanCustomScan(node);
}

void
ExecCustomMarkPos(CustomScanState *node)
{
	if (!node->methods->MarkPosCustomScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("custom scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->MarkPosCustomScan(node);
}

void
ExecCustomRestrPos(CustomScanState *node)
{
	if (!node->methods->RestrPosCustomScan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("custom scan \"%s\" does not support MarkPos",
						node->methods->CustomName)));
	node->methods->RestrPosCustomScan(node);
}

void
ExecCustomScanEstimate(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->EstimateDSMCustomScan)
	{
		node->pscan_len = methods->EstimateDSMCustomScan(node, pcxt);
		shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
}

void
ExecCustomScanInitializeDSM(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->InitializeDSMCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_allocate(pcxt->toc, node->pscan_len);
		methods->InitializeDSMCustomScan(node, pcxt, coordinate);
		shm_toc_insert(pcxt->toc, plan_node_id, coordinate);
	}
}

void
ExecCustomScanReInitializeDSM(CustomScanState *node, ParallelContext *pcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->ReInitializeDSMCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pcxt->toc, plan_node_id, false);
		methods->ReInitializeDSMCustomScan(node, pcxt, coordinate);
	}
}

void
ExecCustomScanInitializeWorker(CustomScanState *node,
							   ParallelWorkerContext *pwcxt)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->InitializeWorkerCustomScan)
	{
		int			plan_node_id = node->ss.ps.plan->plan_node_id;
		void	   *coordinate;

		coordinate = shm_toc_lookup(pwcxt->toc, plan_node_id, false);
		methods->InitializeWorkerCustomScan(node, pwcxt->toc, coordinate);
	}
}

void
ExecShutdownCustomScan(CustomScanState *node)
{
	const CustomExecMethods *methods = node->methods;

	if (methods->ShutdownCustomScan)
		methods->ShutdownCustomScan(node);
}
