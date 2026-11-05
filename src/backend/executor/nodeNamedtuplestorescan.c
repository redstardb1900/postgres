/*-------------------------------------------------------------------------
 *
 * nodeNamedtuplestorescan.c
 *	  处理 NamedTuplestoreScan 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeNamedtuplestorescan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeNamedtuplestorescan.h"
#include "miscadmin.h"
#include "utils/queryenvironment.h"

static TupleTableSlot *NamedTuplestoreScanNext(NamedTuplestoreScanState *node);

/* ----------------------------------------------------------------
 *		NamedTuplestoreScanNext
 *
 *		这是 ExecNamedTuplestoreScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
NamedTuplestoreScanNext(NamedTuplestoreScanState *node)
{
	TupleTableSlot *slot;

	/* 我们有意不支持反向扫描。           */
	Assert(ScanDirectionIsForward(node->ss.ps.state->es_direction));

	/*
	 * 从 tuplestore 取下条元组。无更多元组时返回 NULL。 
	 */
	slot = node->ss.ss_ScanTupleSlot;
	tuplestore_select_read_pointer(node->relation, node->readptr);
	(void) tuplestore_gettupleslot(node->relation, true, false, slot);
	return slot;
}

/*
 * NamedTuplestoreScanRecheck -- 访问方法例程：在 EvalPlanQual 中  
 * 重检元组
 */
static bool
NamedTuplestoreScanRecheck(NamedTuplestoreScanState *node, TupleTableSlot *slot)
{
	/* 无需检查     */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecNamedTuplestoreScan(node)
 *
 *		顺序扫描该 CTE，并返回下一个满足条件的元组。 
 *		我们调用 ExecScan() 例程，把相应的访问方法 
 *		函数传递给它。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecNamedTuplestoreScan(PlanState *pstate)
{
	NamedTuplestoreScanState *node = castNode(NamedTuplestoreScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) NamedTuplestoreScanNext,
					(ExecScanRecheckMtd) NamedTuplestoreScanRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitNamedTuplestoreScan
 * ----------------------------------------------------------------
 */
NamedTuplestoreScanState *
ExecInitNamedTuplestoreScan(NamedTuplestoreScan *node, EState *estate, int eflags)
{
	NamedTuplestoreScanState *scanstate;
	EphemeralNamedRelation enr;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * NamedTuplestoreScan 不应有任何子节点。  
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 为节点新建 NamedTuplestoreScanState    
	 */
	scanstate = makeNode(NamedTuplestoreScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecNamedTuplestoreScan;

	enr = get_ENR(estate->es_queryEnv, node->enrname);
	if (!enr)
		elog(ERROR, "executor could not find named tuplestore \"%s\"",
			 node->enrname);

	Assert(enr->reldata);
	scanstate->relation = (Tuplestorestate *) enr->reldata;
	scanstate->tupdesc = ENRMetadataGetTupDesc(&(enr->md));
	scanstate->readptr =
		tuplestore_alloc_read_pointer(scanstate->relation, EXEC_FLAG_REWIND);

	/*
	 * 新的读指针复制自读指针 0，而后者可能在           
	 * 任意位置，因此需显式回绕。    
	 */
	tuplestore_select_read_pointer(scanstate->relation, scanstate->readptr);
	tuplestore_rescan(scanstate->relation);

	/*
	 * XXX: 是否该加个函数，用完后释放该读指针？       
	 *
	 * 曾尝试过这样做，但在任何已测案例中未改善          
	 * 性能/内存。    
	 */

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 扫描元组类型由该 tuplestore 指定。       
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss, scanstate->tupdesc,
						  &TTSOpsMinimalTuple);

	/*
	 * 初始化结果类型和投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndNamedTuplestoreScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndNamedTuplestoreScan(NamedTuplestoreScanState *node)
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
 *		ExecReScanNamedTuplestoreScan
 *
 *		重新扫描关系。
 * ----------------------------------------------------------------
 */
void
ExecReScanNamedTuplestoreScan(NamedTuplestoreScanState *node)
{
	Tuplestorestate *tuplestorestate = node->relation;

	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	ExecScanReScan(&node->ss);

	/*
	 * 回绕自身指针。 
	 */
	tuplestore_select_read_pointer(tuplestorestate, node->readptr);
	tuplestore_rescan(tuplestorestate);
}
