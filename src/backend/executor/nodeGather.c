/*-------------------------------------------------------------------------
 *
 * nodeGather.c
 *	  通过多个 worker 扫描计划的支撑例程。        
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Gather 执行器会启动并行 worker 来运行一个计划的        
 * 多个副本。若 worker 不可用或尚未启动，它也会自行    
 * 运行该计划。随后它会把自身产生的结果与各个 worker    
 * 的结果合并为单一输出流。                                     
 * 它通常用于这样的计划：运行同一计划的多个副本不会 
 * 产生重复输出，例如支持并行的                         
 * SeqScan 
 *
 * 另一种配置是让 Gather 节点只使用一个 worker，         
 * 并设置单副本标志。此时，Gather 节点将只在一个        
 * worker 中运行计划，而自身不执行该计划。这种       
 * 情况下，它只是简单返回该 worker 返回的所有元组。     
 * 如果无法获得 worker，那么它会自行运行计划并       
 * 返回结果。因此，与单副本 Gather 节点配合使用的    
 * 计划不必支持并行。     
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGather.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGather.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"


static TupleTableSlot *ExecGather(PlanState *pstate);
static TupleTableSlot *gather_getnext(GatherState *gatherstate);
static MinimalTuple gather_readnext(GatherState *gatherstate);
static void ExecShutdownGatherWorkers(GatherState *node);


/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherState *
ExecInitGather(Gather *node, EState *estate, int eflags)
{
	GatherState *gatherstate;
	Plan	   *outerNode;
	TupleDesc	tupDesc;

	/* Gather 节点没有 innerPlan 节点。  */
	Assert(innerPlan(node) == NULL);

	/*
	 * 创建状态结构    
	 */
	gatherstate = makeNode(GatherState);
	gatherstate->ps.plan = (Plan *) node;
	gatherstate->ps.state = estate;
	gatherstate->ps.ExecProcNode = ExecGather;

	gatherstate->initialized = false;
	gatherstate->need_to_scan_locally =
		!node->single_copy && parallel_leader_participation;
	gatherstate->tuples_needed = -1;

	/*
	 * 杂项初始化             
	 *
	 * 创建表达式上下文          
	 */
	ExecAssignExprContext(estate, &gatherstate->ps);

	/*
	 * 初始化外层计划    
	 */
	outerNode = outerPlan(node);
	outerPlanState(gatherstate) = ExecInitNode(outerNode, estate, eflags);
	tupDesc = ExecGetResultType(outerPlanState(gatherstate));

	/*
	 * Leader 可能直接取 ExecProcNode 结果（若  
	 * need_to_scan_locally），或通过元组队列从 worker 取得。 
	 * 因此本节点内求值的表达式无法简单地假定槽         
	 * 类型固定。  
	 */
	gatherstate->ps.outeropsset = true;
	gatherstate->ps.outeropsfixed = false;

	/*
	 * 初始化结果类型与投影。     
	 */
	ExecInitResultTypeTL(&gatherstate->ps);
	ExecConditionalAssignProjectionInfo(&gatherstate->ps, tupDesc, OUTER_VAR);

	/*
	 * 若无投影，结果槽类型无法简单地确定，参见    
	 * 上面注释  
	 */
	if (gatherstate->ps.ps_ProjInfo == NULL)
	{
		gatherstate->ps.resultopsset = true;
		gatherstate->ps.resultopsfixed = false;
	}

	/*
	 * 初始化 funnel 槽为与外层计划相同的元组描述符
	 */
	gatherstate->funnel_slot = ExecInitExtraTupleSlot(estate, tupDesc,
													  &TTSOpsMinimalTuple);

	/*
	 * Gather 不支持检查 qual（在子节点中做这件事            
	 * 总是更高效）。    
	 */
	Assert(!node->plan.qual);

	return gatherstate;
}

/* ----------------------------------------------------------------
 *		ExecGather(node)
 *
 *		用多个 worker 扫描关系，返回下一个    
 *		满足条件的元组。  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGather(PlanState *pstate)
{
	GatherState *node = castNode(GatherState, pstate);
	TupleTableSlot *slot;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 在首次执行时初始化并行上下文与 worker。我们        
	 * 把它放在首次执行而非节点初始化时做，因为它      
	 * 要分配很大的动态段，故最好只在真正需要时       
	 * 才执行这一步。       
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		Gather	   *gather = (Gather *) node->ps.plan;

		/*
		 * 有时我们可能必须在没有并行的情况下运行；       
		 * 但若并行模式已启用，即可尝试启动 worker
		 */
		if (gather->num_workers > 0 && estate->es_use_parallel_mode)
		{
			ParallelContext *pcxt;

			/* 初始化或重新初始化 worker 所需的共享状态      */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(outerPlanState(node),
												 estate,
												 gather->initParam,
												 gather->num_workers,
												 node->tuples_needed);
			else
				ExecParallelReinitialize(outerPlanState(node),
										 node->pei,
										 gather->initParam);

			/*
			 * 注册后台 worker。得到的数量可能少于请求 
			 * 量，甚至一个都没有。  
			 */
			pcxt = node->pei->pcxt;
			LaunchParallelWorkers(pcxt);
			/* 保存已启动 worker 数，供 EXPLAIN 使用       */
			node->nworkers_launched = pcxt->nworkers_launched;

			/* 设置元组队列读取器以读取结果。   */
			if (pcxt->nworkers_launched > 0)
			{
				ExecParallelCreateReaders(node->pei);
				/* 构建工作数组，列出活跃读取器      */
				node->nreaders = pcxt->nworkers_launched;
				node->reader = (TupleQueueReader **)
					palloc(node->nreaders * sizeof(TupleQueueReader *));
				memcpy(node->reader, node->pei->reader,
					   node->nreaders * sizeof(TupleQueueReader *));
			}
			else
			{
				/* 无 worker？那就作罢。 */
				node->nreaders = 0;
				node->reader = NULL;
			}
			node->nextreader = 0;
		}

		/* 无 worker 或已启用且非单副本时，本地运行计划 */
		node->need_to_scan_locally = (node->nreaders == 0)
			|| (!gather->single_copy && parallel_leader_participation);
		node->initialized = true;
	}

	/*
	 * 重置元组级内存上下文，释放上个元组周期中    
	 * 分配的表达式求值存储。             
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/*
	 * 获取下一个元组：要么来自我们某个 worker，要么      
	 * 自产。 
	 */
	slot = gather_getnext(node);
	if (TupIsNull(slot))
		return NULL;

	/* 若无需投影，就完成了。         */
	if (node->ps.ps_ProjInfo == NULL)
		return slot;

	/*
	 * 使用 ExecProject() 形成结果元组并返回。      
	 */
	econtext->ecxt_outertuple = slot;
	return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *		ExecEndGather
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndGather(GatherState *node)
{
	ExecEndNode(outerPlanState(node));	/* 先让子节点清理       */
	ExecShutdownGather(node);
	ExecFreeExprContext(&node->ps);
	if (node->ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ps.ps_ResultTupleSlot);
}

/*
 * 读取下一个元组。我们可能通过 gather_readnext 从某个     
 * 元组队列中取；若没有元组队列含有元组且           
 * 未设置 single_copy 标志，则可能在本地生成一个。   
 */
static TupleTableSlot *
gather_getnext(GatherState *gatherstate)
{
	PlanState  *outerPlan = outerPlanState(gatherstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *fslot = gatherstate->funnel_slot;
	MinimalTuple tup;

	while (gatherstate->nreaders > 0 || gatherstate->need_to_scan_locally)
	{
		CHECK_FOR_INTERRUPTS();

		if (gatherstate->nreaders > 0)
		{
			tup = gather_readnext(gatherstate);

			if (HeapTupleIsValid(tup))
			{
				ExecStoreMinimalTuple(tup,	/* 待存元组   */
									  fslot,	/* 存储元组的槽      */
									  false);	/* 不 pfree 该元组*/
				return fslot;
			}
		}

		if (gatherstate->need_to_scan_locally)
		{
			EState	   *estate = gatherstate->ps.state;

			/* 执行计划期间安装我们的 DSA 区域   */
			estate->es_query_dsa =
				gatherstate->pei ? gatherstate->pei->area : NULL;
			outerTupleSlot = ExecProcNode(outerPlan);
			estate->es_query_dsa = NULL;

			if (!TupIsNull(outerTupleSlot))
				return outerTupleSlot;

			gatherstate->need_to_scan_locally = false;
		}
	}

	return ExecClearTuple(fslot);
}

/*
 * 尝试从我们的某个并行 worker 读取元组。    
 */
static MinimalTuple
gather_readnext(GatherState *gatherstate)
{
	int			nvisited = 0;

	for (;;)
	{
		TupleQueueReader *reader;
		MinimalTuple tup;
		bool		readerdone;

		/* 检查异步事件，尤其是来自 worker 的消息。    */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 尝试读取元组，但若无可用元组则不阻塞。     
		 *
		 * 注意：TupleQueueReaderNext 对初始化失败的 worker       
		 * 只会返回 NULL。我们将该 worker 视为没有         
		 * 产出任何元组；等到 WaitForParallelWorkersToFinish 时   
		 * 它会报错。   
		 */
		Assert(gatherstate->nextreader < gatherstate->nreaders);
		reader = gatherstate->reader[gatherstate->nextreader];
		tup = TupleQueueReaderNext(reader, true, &readerdone);

		/*
		 * 若此读取器已完成，就把它从活跃读取器工作数组
		 * 中移除。若读取器全部完成，就收工。 
		 */
		if (readerdone)
		{
			Assert(!tup);
			--gatherstate->nreaders;
			if (gatherstate->nreaders == 0)
			{
				ExecShutdownGatherWorkers(gatherstate);
				return NULL;
			}
			memmove(&gatherstate->reader[gatherstate->nextreader],
					&gatherstate->reader[gatherstate->nextreader + 1],
					sizeof(TupleQueueReader *)
					* (gatherstate->nreaders - gatherstate->nextreader));
			if (gatherstate->nextreader >= gatherstate->nreaders)
				gatherstate->nextreader = 0;
			continue;
		}

		/* 若有元组，则返回。   */
		if (tup)
			return tup;

		/*
		 * 以轮转方式推进 nextreader 指针。注意只有当       
		 * 无法从当前 worker 取得元组时，才会走到这里。 
		 * 过去我们在每个元组之后都推进 nextreader 指针， 
		 * 但事实证明，持续读同一队列会高效得多，      
		 * 直到必须阻塞为止。                                   
		 */
		gatherstate->nextreader++;
		if (gatherstate->nextreader >= gatherstate->nreaders)
			gatherstate->nextreader = 0;

		/* 是否已遍历每个活跃 TupleQueueReader？     */
		nvisited++;
		if (nvisited >= gatherstate->nreaders)
		{
			/*
			 * 若（仍）本地运行计划，返回 NULL，让调用  
			 * 者能从计划的本地副本再生成一个元组。 
			 */
			if (gatherstate->need_to_scan_locally)
				return NULL;

			/* 除等待事态进展外无事可做。     */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 WAIT_EVENT_EXECUTE_GATHER);
			ResetLatch(MyLatch);
			nvisited = 0;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherWorkers
 *
 *		停止所有并行 worker。  
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherWorkers(GatherState *node)
{
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);

	/* 刷出读取器数组本地副本*/
	if (node->reader)
		pfree(node->reader);
	node->reader = NULL;
}

/* ----------------------------------------------------------------
 *		ExecShutdownGather
 *
 *		销毁为并行 worker 建立的设置，包括并行上下文。 
 * ----------------------------------------------------------------
 */
void
ExecShutdownGather(GatherState *node)
{
	ExecShutdownGatherWorkers(node);

	/* 现在销毁并行上下文。    */
	if (node->pei != NULL)
	{
		ExecParallelCleanup(node->pei);
		node->pei = NULL;
	}
}

/* ----------------------------------------------------------------
 *						连接支持
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanGather
 *
 *		准备重新扫描 Gather 的结果。    
 * ----------------------------------------------------------------
 */
void
ExecReScanGather(GatherState *node)
{
	Gather	   *gather = (Gather *) node->ps.plan;
	PlanState  *outerPlan = outerPlanState(node);

	/* 确保所有现存 worker 都已被优雅地关闭      */
	ExecShutdownGatherWorkers(node);

	/* 标记节点，使共享状态在下次调用时被重建   */
	node->initialized = false;

	/*
	 * 设置子节点的 chgParam，告知它下一次扫描在领导        
	 * 者进程中可能交付不同的行集合。（总行集不应改   
	 * 变，但领导者进程的子集可能会变；因此位于此处  
	 * 与并行表扫描节点之间的节点不得基于行集不变      
	 * 的假设进行优化。）         
	 */
	if (gather->rescan_param >= 0)
		outerPlan->chgParam = bms_add_member(outerPlan->chgParam,
											 gather->rescan_param);

	/*
	 * 如果子节点的 chgParam 非空，计划将由首次           
	 * ExecProcNode 重新扫描。注意：若我们有                
	 * rescan_param，则此调用不做任何事，这当前保证           
	 * 支持并行的子节点在收到 ReInitializeDSM 调用               
	 * 之前不会看到 ReScan 调用。不过这种顺序未必          
	 * 可靠。一条好的经验法则是：ReInitializeDSM 只             
	 * 重置共享状态，ReScan 只重置本地状态，而依赖      
	 * 这两步都完成的操作必须等到首次 ExecProcNode 调用。   
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
