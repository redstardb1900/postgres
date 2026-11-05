/*-------------------------------------------------------------------------
 *
 * nodeSort.c
 *	  处理关系排序的例程。          
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"


/* ----------------------------------------------------------------
 *		ExecSort
 *
 *		使用 tuplesort 对节点外层子树的元组进行排序，  
 *		结果保存到临时文件或内存中。初始调用之后， 
 *		每次调用都从文件返回一个元组。              
 *
 *		执行这种排序有两种截然不同的方式：         
 *
 *		1) 当结果为单列时，我们执行 Datum 排序。       
 *
 *		2) 当结果包含多列时，我们执行元组排序。            
 *
 *		我们本可以总是用元组排序来实现，不过仅对       
 *		Datum 进行排序会明显快于元组排序，尤其当   
 *		Datum 为按值传递类型时。                      
 *
 *		条件：  
 *		  --无。
 *
 *		初始状态：
 *		  -- 外层子节点已准备好返回第一个元组。   
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSort(PlanState *pstate)
{
	SortState  *node = castNode(SortState, pstate);
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate;
	TupleTableSlot *slot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 获取节点状态信息
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) node->tuplesortstate;

	/*
	 * 如果是首次执行，从外层计划读取所有元组并传给     
	 * tuplesort.c。后续调用只需从 tuplesort 获取元组。  
	 */

	if (!node->sort_Done)
	{
		Sort	   *plannode = (Sort *) node->ss.ps.plan;
		PlanState  *outerNode;
		TupleDesc	tupDesc;
		int			tuplesortopts = TUPLESORT_NONE;

		SO1_printf("ExecSort: %s\n",
				   "sorting subplan");

		/*
		 * 希望在创建排序数据时按前向方向扫描             
		 * 子计划。
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * 初始化tuplesort模块。 
		 */
		SO1_printf("ExecSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);
		tupDesc = ExecGetResultType(outerNode);

		if (node->randomAccess)
			tuplesortopts |= TUPLESORT_RANDOMACCESS;
		if (node->bounded)
			tuplesortopts |= TUPLESORT_ALLOWBOUNDED;

		if (node->datumSort)
			tuplesortstate = tuplesort_begin_datum(TupleDescAttr(tupDesc, 0)->atttypid,
												   plannode->sortOperators[0],
												   plannode->collations[0],
												   plannode->nullsFirst[0],
												   work_mem,
												   NULL,
												   tuplesortopts);
		else
			tuplesortstate = tuplesort_begin_heap(tupDesc,
												  plannode->numCols,
												  plannode->sortColIdx,
												  plannode->sortOperators,
												  plannode->collations,
												  plannode->nullsFirst,
												  work_mem,
												  NULL,
												  tuplesortopts);
		if (node->bounded)
			tuplesort_set_bound(tuplesortstate, node->bound);
		node->tuplesortstate = (void *) tuplesortstate;

		/*
		 * 扫描子计划，并按所做排序的类型以适当方法将
		 * 所有元组提供给 tuplesort。                       
		 */
		if (node->datumSort)
		{
			for (;;)
			{
				slot = ExecProcNode(outerNode);

				if (TupIsNull(slot))
					break;
				slot_getsomeattrs(slot, 1);
				tuplesort_putdatum(tuplesortstate,
								   slot->tts_values[0],
								   slot->tts_isnull[0]);
			}
		}
		else
		{
			for (;;)
			{
				slot = ExecProcNode(outerNode);

				if (TupIsNull(slot))
					break;
				tuplesort_puttupleslot(tuplesortstate, slot);
			}
		}

		/*
		 * 完成排序。   
		 */
		tuplesort_performsort(tuplesortstate);

		/*
		 * 恢复为用户指定的方向     
		 */
		estate->es_direction = dir;

		/*
		 * 最后将 sorted 标志设为 true 
		 */
		node->sort_Done = true;
		node->bounded_Done = node->bounded;
		node->bound_Done = node->bound;
		if (node->shared_info && node->am_worker)
		{
			TuplesortInstrumentation *si;

			Assert(IsParallelWorker());
			Assert(ParallelWorkerNumber <= node->shared_info->num_workers);
			si = &node->shared_info->sinstrument[ParallelWorkerNumber];
			tuplesort_get_stats(tuplesortstate, si);
		}
		SO1_printf("ExecSort: %s\n", "sorting done");
	}

	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from tuplesort");

	slot = node->ss.ps.ps_ResultTupleSlot;

	/*
	 * 从适当的 tuplesort 函数取下一个已排序条目。对于      
	 * Datum 排序，我们必须自己管理槽位，并在               
	 * tuplesort_getdatum 返回 false 时保持槽位为空，以表明     
	 * 已没有更多 Datum。对于元组排序，tuplesort_gettupleslot
	 * 会管理槽位，元组耗尽时清空。  
	 */
	if (node->datumSort)
	{
		ExecClearTuple(slot);
		if (tuplesort_getdatum(tuplesortstate, ScanDirectionIsForward(dir),
							   false, &(slot->tts_values[0]),
							   &(slot->tts_isnull[0]), NULL))
			ExecStoreVirtualTuple(slot);
	}
	else
		(void) tuplesort_gettupleslot(tuplesortstate,
									  ScanDirectionIsForward(dir),
									  false, slot, NULL);

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 *		为规划器生成的排序节点创建运行时状态  
 *		并初始化其外层子树。                            
 * ----------------------------------------------------------------
 */
SortState *
ExecInitSort(Sort *node, EState *estate, int eflags)
{
	SortState  *sortstate;
	TupleDesc	outerTupDesc;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/*
	 * 创建状态结构    
	 */
	sortstate = makeNode(SortState);
	sortstate->ss.ps.plan = (Plan *) node;
	sortstate->ss.ps.state = estate;
	sortstate->ss.ps.ExecProcNode = ExecSort;

	/*
	 * 需要随机访问排序输出才能做反向扫描或              
	 * mark/restore。我们还希望物化排序输出，以备         
	 * 多次被要求倒回并重放它。                  
	 */
	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	sortstate->bounded = false;
	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;

	/*
	 * 杂项初始化             
	 *
	 * 排序节点不初始化 ExprContext，因为从不调用 ExecQual    
	 * 或 ExecProject。      
	 */

	/*
	 * 初始化子节点    
	 *
	 * 我们让子节点无需支持 REWIND、BACKWARD 或                  
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * 初始化扫描槽和类型   
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &sortstate->ss, &TTSOpsVirtual);

	/*
	 * 初始化返回槽位与类型。无需初始化投影信息，       
	 * 因为该节点不做投影。           
	 */
	ExecInitResultTupleSlotTL(&sortstate->ss.ps, &TTSOpsMinimalTuple);
	sortstate->ss.ps.ps_ProjInfo = NULL;

	outerTupDesc = ExecGetResultType(outerPlanState(sortstate));

	/*
	 * 当只对单列排序时，我们执行 Datum 排序，         
	 * 否则执行元组排序。       
	 */
	if (outerTupDesc->natts == 1)
		sortstate->datumSort = true;
	else
		sortstate->datumSort = false;

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	return sortstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndSort(SortState *node)
{
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* 必须丢弃排序结果元组的指针*/
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * 释放 tuplesort 资源    
	 */
	if (node->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
	node->tuplesortstate = NULL;

	/*
	 * 关闭子计划      
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		调用 tuplesort 保存已排序文件中的当前位置。     
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(SortState *node)
{
	/*
	 * 如果尚未排序，则直接返回 
	 */
	if (!node->sort_Done)
		return;

	tuplesort_markpos((Tuplesortstate *) node->tuplesortstate);
}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		调用 tuplesort 恢复上次保存的排序文件位置。  
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(SortState *node)
{
	/*
	 * 如果尚未排序，则直接返回  
	 */
	if (!node->sort_Done)
		return;

	/*
	 * 将扫描恢复到先前标记的位置           
	 */
	tuplesort_restorepos((Tuplesortstate *) node->tuplesortstate);
}

void
ExecReScanSort(SortState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/*
	 * 若尚未排序则直接返回。外层计划的 chgParam 不为     
	 * NULL，则 ExecProcNode 会重新扫描它，否则完全没必要
	 * 重新扫描它。
	 */
	if (!node->sort_Done)
		return;

	/* 必须丢弃排序结果元组的指针*/
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * 若子节点将重新扫描，我们便忘记先前的排序结果； 
	 * 必须重新读取子计划并重新排序。若 bounded-sort     
	 * 参数改变或未选择 randomAccess，也必须重新排序。 
	 *
	 * 否则可直接倒回并重新扫描已排序输出。    
	 */
	if (outerPlan->chgParam != NULL ||
		node->bounded != node->bounded_Done ||
		node->bound != node->bound_Done ||
		!node->randomAccess)
	{
		node->sort_Done = false;
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
		node->tuplesortstate = NULL;

		/*
		 * 如果子节点的 chgParam 非空，则第一个 ExecProcNode    
		 * 重新扫描计划 
		 */
		if (outerPlan->chgParam == NULL)
			ExecReScan(outerPlan);
	}
	else
		tuplesort_rescan((Tuplesortstate *) node->tuplesortstate);
}

/* ----------------------------------------------------------------
 *						并行查询支持    
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSortEstimate
 *
 *		估算传播排序统计信息所需的空间。     
 * ----------------------------------------------------------------
 */
void
ExecSortEstimate(SortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 不插桩或无 worker 时不需要此操作       */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(TuplesortInstrumentation));
	size = add_size(size, offsetof(SharedSortInfo, sinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeDSM
 *
 *		为排序统计初始化 DSM 空间。   
 * ----------------------------------------------------------------
 */
void
ExecSortInitializeDSM(SortState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 不插桩或无 worker 时不需要此操作       */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedSortInfo, sinstrument)
		+ pcxt->nworkers * sizeof(TuplesortInstrumentation);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* 确保所有未填充的槽位都包含零    */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecSortInitializeWorker
 *
 *		把 worker 附着到排序统计 DSM 空间。 
 * ----------------------------------------------------------------
 */
void
ExecSortInitializeWorker(SortState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
	node->am_worker = true;
}

/* ----------------------------------------------------------------
 *		ExecSortRetrieveInstrumentation
 *
 *		把排序统计从 DSM 传送到私有内存。     
 * ----------------------------------------------------------------
 */
void
ExecSortRetrieveInstrumentation(SortState *node)
{
	Size		size;
	SharedSortInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedSortInfo, sinstrument)
		+ node->shared_info->num_workers * sizeof(TuplesortInstrumentation);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
