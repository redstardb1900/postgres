/*-------------------------------------------------------------------------
 *
 * nodeSamplescan.c
 *	  关系采样扫描的支撑例程（表采样）。             
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSamplescan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/nodeSamplescan.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/rel.h"

static TupleTableSlot *SampleNext(SampleScanState *node);
static void tablesample_init(SampleScanState *scanstate);
static TupleTableSlot *tablesample_getnext(SampleScanState *scanstate);

/* ----------------------------------------------------------------
 *						扫描支持
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SampleNext
 *
 *		这是 ExecSampleScan 的主力函数 
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SampleNext(SampleScanState *node)
{
	/*
	 * 若是扫描内首次调用，则初始化     
	 */
	if (!node->begun)
		tablesample_init(node);

	/*
	 * 取下一个元组，存入我们的结果槽      
	 */
	return tablesample_getnext(node);
}

/*
 * SampleRecheck -- EvalPlanQual 中重新检查元组的访问方法例程 
 */
static bool
SampleRecheck(SampleScanState *node, TupleTableSlot *slot)
{
	/*
	 * 与 SeqScan 一样不向 heap_beginscan 传入任何可检查的键， 
	 * 故 SampleScan 无需再检查。
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSampleScan(node)
 *
 *		使用采样方法扫描关系，并返回下一个     
 *		满足条件的元组。  
 *		我们调用 ExecScan() 例程，并向它传入适当的 
 *		访问方法函数。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSampleScan(PlanState *pstate)
{
	SampleScanState *node = castNode(SampleScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SampleNext,
					(ExecScanRecheckMtd) SampleRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSampleScan
 * ----------------------------------------------------------------
 */
SampleScanState *
ExecInitSampleScan(SampleScan *node, EState *estate, int eflags)
{
	SampleScanState *scanstate;
	TableSampleClause *tsc = node->tablesample;
	TsmRoutine *tsm;

	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * 创建状态结构    
	 */
	scanstate = makeNode(SampleScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecSampleScan;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * 打开扫描关系    
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scan.scanrelid,
							 eflags);

	/* 我们稍后才会设置 HeapScanDesc       */
	scanstate->ss.ss_currentScanDesc = NULL;

	/* 并创建具有相应行类型的槽     */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * 初始化子表达式       
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	scanstate->args = ExecInitExprList(tsc->args, (PlanState *) scanstate);
	scanstate->repeatable =
		ExecInitExpr(tsc->repeatable, (PlanState *) scanstate);

	/*
	 * 如果没有 REPEATABLE 子句，则选择一个随机种子。我们  
	 * 只此一次，因为种子不应随重新扫描而改变。    
	 */
	if (tsc->repeatable == NULL)
		scanstate->seed = pg_prng_uint32(&pg_global_prng_state);

	/*
	 * 最后初始化 TABLESAMPLE 方法处理器。     
	 */
	tsm = GetTsmRoutine(tsc->tsmhandler);
	scanstate->tsmroutine = tsm;
	scanstate->tsm_state = NULL;

	if (tsm->InitSampleScan)
		tsm->InitSampleScan(scanstate, eflags);

	/* 稍后再执行 BeginSampleScan；参数暂无法求值      */
	scanstate->begun = false;

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSampleScan
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndSampleScan(SampleScanState *node)
{
	/*
	 * 告知采样函数我们已完成扫描。       
	 */
	if (node->tsmroutine->EndSampleScan)
		node->tsmroutine->EndSampleScan(node);

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 清空元组表          
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * 关闭堆扫描
	 */
	if (node->ss.ss_currentScanDesc)
		table_endscan(node->ss.ss_currentScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecReScanSampleScan
 *
 *		重新扫描关系。
 *
 * ----------------------------------------------------------------
 */
void
ExecReScanSampleScan(SampleScanState *node)
{
	/* 记住若之前做过，就需要再次执行 BeginSampleScan      */
	node->begun = false;
	node->done = false;
	node->haveblock = false;
	node->donetuples = 0;

	ExecScanReScan(&node->ss);
}


/*
 * 初始化 TABLESAMPLE 方法：求值参数并调用 BeginSampleScan。     
 */
static void
tablesample_init(SampleScanState *scanstate)
{
	TsmRoutine *tsm = scanstate->tsmroutine;
	ExprContext *econtext = scanstate->ss.ps.ps_ExprContext;
	Datum	   *params;
	Datum		datum;
	bool		isnull;
	uint32		seed;
	bool		allow_sync;
	int			i;
	ListCell   *arg;

	scanstate->donetuples = 0;
	params = (Datum *) palloc(list_length(scanstate->args) * sizeof(Datum));

	i = 0;
	foreach(arg, scanstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		params[i] = ExecEvalExprSwitchContext(argstate,
											  econtext,
											  &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
					 errmsg("TABLESAMPLE parameter cannot be null")));
		i++;
	}

	if (scanstate->repeatable)
	{
		datum = ExecEvalExprSwitchContext(scanstate->repeatable,
										  econtext,
										  &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLESAMPLE_REPEAT),
					 errmsg("TABLESAMPLE REPEATABLE parameter cannot be null")));

		/*
		 * REPEATABLE 参数已被解析器强制转换为 float8。         
		 * 在 SQL 层使用 float8 是因为它对两类用户都会  
		 * 产生不意外的结果：无论是习惯于 REPEATABLE 子句   
		 * 只接受整数的数据库的用户，还是预期 REPEATABLE 
		 * 像 setseed()（-1 到 1 范围内的浮点数）那样工作    
		 * 的用户。        
		 *
		 * 我们使用 hashfloat8() 把所提供的值转换成合适的   
		 * 种子。就回归测试而言，这有一个便利性质，   
		 * 即 REPEATABLE(0) 给出与机器无关的结果。            
		 */
		seed = DatumGetUInt32(DirectFunctionCall1(hashfloat8, datum));
	}
	else
	{
		/* 使用 ExecInitSampleScan 选定的种子   */
		seed = scanstate->seed;
	}

	/* 为 BeginSampleScan 可调整的参数设置默认值         */
	scanstate->use_bulkread = true;
	scanstate->use_pagemode = true;

	/* 让 tablesample 方法自行处理  */
	tsm->BeginSampleScan(scanstate,
						 params,
						 list_length(scanstate->args),
						 seed);

	/* 如果没有 NextSampleBlock 函数，则使用 syncscan  */
	allow_sync = (tsm->NextSampleBlock == NULL);

	/* 现在可以创建或重置 HeapScanDesc    */
	if (scanstate->ss.ss_currentScanDesc == NULL)
	{
		scanstate->ss.ss_currentScanDesc =
			table_beginscan_sampling(scanstate->ss.ss_currentRelation,
									 scanstate->ss.ps.state->es_snapshot,
									 0, NULL,
									 scanstate->use_bulkread,
									 allow_sync,
									 scanstate->use_pagemode);
	}
	else
	{
		table_rescan_set_params(scanstate->ss.ss_currentScanDesc, NULL,
								scanstate->use_bulkread,
								allow_sync,
								scanstate->use_pagemode);
	}

	pfree(params);

	/* 初始化完成。     */
	scanstate->begun = true;
}

/*
 * 从 TABLESAMPLE 方法取下个元组  
 */
static TupleTableSlot *
tablesample_getnext(SampleScanState *scanstate)
{
	TableScanDesc scan = scanstate->ss.ss_currentScanDesc;
	TupleTableSlot *slot = scanstate->ss.ss_ScanTupleSlot;

	ExecClearTuple(slot);

	if (scanstate->done)
		return NULL;

	for (;;)
	{
		if (!scanstate->haveblock)
		{
			if (!table_scan_sample_next_block(scan, scanstate))
			{
				scanstate->haveblock = false;
				scanstate->done = true;

				/* 关系已耗尽    */
				return NULL;
			}

			scanstate->haveblock = true;
		}

		if (!table_scan_sample_next_tuple(scan, scanstate, slot))
		{
			/*
			 * 如果走到这里，说明本页上的条目已全部取完，
			 * 应该移向下一页了。       
			 */
			scanstate->haveblock = false;
			continue;
		}

		/* 找到可见元组并返回。  */
		break;
	}

	scanstate->donetuples++;

	return slot;
}
