/*-------------------------------------------------------------------------
 *
 * nodeGatherMerge.c
 *		通过多个 worker 扫描计划，并做保序合并。       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGatherMerge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGatherMerge.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * 从 worker 读取元组时，只要可行，最好一次读多条，      
 * 这样做效率更高，可将上下文切换的开销降到最低。   
 * 但一次读取过多会浪费内存，且无助于提升性能。         
 * 我们最多读取 MAX_TUPLE_STORE 条元组（外加第一条）。   
 */
#define MAX_TUPLE_STORE 10

/*
 * 每个 worker 的待处理元组数组。其中保存这些元组：   
 * 我们能够取到它们，但尚无法处理。此外，该结构体还   
 * 保存 "done" 标志，表明已知该 worker 不会再有              
 * 更多元组。（leader 不使用该结构体；我们不会为它      
 * 保存待处理元组，need_to_scan_locally 标志充当它的            
 * "done" 指示器。）   
 */
typedef struct GMReaderTupleBuffer
{
	MinimalTuple *tuple;		/* 数组长度为 MAX_TUPLE_STORE */
	int			nTuples;		/* 当前已存储的元组数量    */
	int			readCounter;	/* 下一个待提取元组下标 */
	bool		done;			/* 已知读取器耗尽时为 true  */
} GMReaderTupleBuffer;

static TupleTableSlot *ExecGatherMerge(PlanState *pstate);
static int32 heap_compare_slots(Datum a, Datum b, void *arg);
static TupleTableSlot *gather_merge_getnext(GatherMergeState *gm_state);
static MinimalTuple gm_readnext_tuple(GatherMergeState *gm_state, int nreader,
									  bool nowait, bool *done);
static void ExecShutdownGatherMergeWorkers(GatherMergeState *node);
static void gather_merge_setup(GatherMergeState *gm_state);
static void gather_merge_init(GatherMergeState *gm_state);
static void gather_merge_clear_tuples(GatherMergeState *gm_state);
static bool gather_merge_readnext(GatherMergeState *gm_state, int reader,
								  bool nowait);
static void load_tuple_array(GatherMergeState *gm_state, int reader);

/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherMergeState *
ExecInitGatherMerge(GatherMerge *node, EState *estate, int eflags)
{
	GatherMergeState *gm_state;
	Plan	   *outerNode;
	TupleDesc	tupDesc;

	/* Gather merge 节点没有 innerPlan 节点。  */
	Assert(innerPlan(node) == NULL);

	/*
	 * 创建状态结构体 
	 */
	gm_state = makeNode(GatherMergeState);
	gm_state->ps.plan = (Plan *) node;
	gm_state->ps.state = estate;
	gm_state->ps.ExecProcNode = ExecGatherMerge;

	gm_state->initialized = false;
	gm_state->gm_initialized = false;
	gm_state->tuples_needed = -1;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &gm_state->ps);

	/*
	 * GatherMerge 不支持检查限定条件（在子节点中进行        
	 * 总是更高效）。       
	 */
	Assert(!node->plan.qual);

	/*
	 * 初始化外层计划    
	 */
	outerNode = outerPlan(node);
	outerPlanState(gm_state) = ExecInitNode(outerNode, estate, eflags);

	/*
	 * leader 可直接访问 ExecProcNode 结果（若  
	 * need_to_scan_locally），或经元组队列从 worker 取得。    
	 * 因此无法想当然认为本节点内求值表达式的 slot       
	 * 类型固定。  
	 */
	gm_state->ps.outeropsset = true;
	gm_state->ps.outeropsfixed = false;

	/*
	 * 把元组描述符存入 gather merge 状态，供初始化         
	 * gather merge slot 时使用。            
	 */
	tupDesc = ExecGetResultType(outerPlanState(gm_state));
	gm_state->tupDesc = tupDesc;

	/*
	 * 初始化结果类型和投影。     
	 */
	ExecInitResultTypeTL(&gm_state->ps);
	ExecConditionalAssignProjectionInfo(&gm_state->ps, tupDesc, OUTER_VAR);

	/*
	 * 没有投影时结果 slot 类型并非显而易见，见       
	 * 上文。     
	 */
	if (gm_state->ps.ps_ProjInfo == NULL)
	{
		gm_state->ps.resultopsset = true;
		gm_state->ps.resultopsfixed = false;
	}

	/*
	 * 初始化排序键信息       
	 */
	if (node->numCols)
	{
		int			i;

		gm_state->gm_nkeys = node->numCols;
		gm_state->gm_sortkeys =
			palloc0(sizeof(SortSupportData) * node->numCols);

		for (i = 0; i < node->numCols; i++)
		{
			SortSupport sortKey = gm_state->gm_sortkeys + i;

			sortKey->ssup_cxt = CurrentMemoryContext;
			sortKey->ssup_collation = node->collations[i];
			sortKey->ssup_nulls_first = node->nullsFirst[i];
			sortKey->ssup_attno = node->sortColIdx[i];

			/*
			 * 我们此处不进行缩写键转换，原因与 MergeAppend  
			 * 未使用它的原因相同。           
			 */
			sortKey->abbreviate = false;

			PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
		}
	}

	/* 现在为 gather merge 分配工作区      */
	gather_merge_setup(gm_state);

	return gm_state;
}

/* ----------------------------------------------------------------
 *		ExecGatherMerge(node)
 *
 *		通过多个 worker 扫描关系并返回          
 *		满足条件的元组。  
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGatherMerge(PlanState *pstate)
{
	GatherMergeState *node = castNode(GatherMergeState, pstate);
	TupleTableSlot *slot;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 与 Gather 一样，我们直到本节点真正执行时才启动  
	 * worker。
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		GatherMerge *gm = castNode(GatherMerge, node->ps.plan);

		/*
		 * 有时可能必须以无并行方式运行；但若并行模式    
		 * 已启用，则可以尝试启动一些 worker。      
		 */
		if (gm->num_workers > 0 && estate->es_use_parallel_mode)
		{
			ParallelContext *pcxt;

			/* 初始化或重新初始化 worker 所需的共享状态。   */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(outerPlanState(node),
												 estate,
												 gm->initParam,
												 gm->num_workers,
												 node->tuples_needed);
			else
				ExecParallelReinitialize(outerPlanState(node),
										 node->pei,
										 gm->initParam);

			/* 尝试启动 worker。 */
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
				/* 没有 worker 就算了。   */
				node->nreaders = 0;
				node->reader = NULL;
			}
		}

		/* 若启用或别无选择，允许 leader 参与     */
		if (parallel_leader_participation || node->nreaders == 0)
			node->need_to_scan_locally = true;
		node->initialized = true;
	}

	/*
	 * 重置每元组内存上下文，释放上一个元组周期中 
	 * 分配的表达式求值存储。             
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/*
	 * 获取下一个元组：或来自我们的某个 worker，或自行   
	 * 执行。 
	 */
	slot = gather_merge_getnext(node);
	if (TupIsNull(slot))
		return NULL;

	/* 若无需投影，我们就完成了。   */
	if (node->ps.ps_ProjInfo == NULL)
		return slot;

	/*
	 * 用 ExecProject() 构造结果元组并返回它。      
	 */
	econtext->ecxt_outertuple = slot;
	return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *		ExecEndGatherMerge
 *
 *		释放通过 C 例程分配的所有存储。  
 * ----------------------------------------------------------------
 */
void
ExecEndGatherMerge(GatherMergeState *node)
{
	ExecEndNode(outerPlanState(node));	/* 先让子节点清理       */
	ExecShutdownGatherMerge(node);
	ExecFreeExprContext(&node->ps);
	if (node->ps.ps_ResultTupleSlot)
		ExecClearTuple(node->ps.ps_ResultTupleSlot);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMerge
 *
 *		销毁并行 worker 的全部设置，包括并行上下文。    
 * ----------------------------------------------------------------
 */
void
ExecShutdownGatherMerge(GatherMergeState *node)
{
	ExecShutdownGatherMergeWorkers(node);

	/* 现在销毁并行上下文。    */
	if (node->pei != NULL)
	{
		ExecParallelCleanup(node->pei);
		node->pei = NULL;
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherMergeWorkers
 *
 *		停止所有并行 worker。  
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherMergeWorkers(GatherMergeState *node)
{
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);

	/* 清刷读取器数组本地副本*/
	if (node->reader)
		pfree(node->reader);
	node->reader = NULL;
}

/* ----------------------------------------------------------------
 *		ExecReScanGatherMerge
 *
 *		准备重新扫描 GatherMerge 的结果。    
 * ----------------------------------------------------------------
 */
void
ExecReScanGatherMerge(GatherMergeState *node)
{
	GatherMerge *gm = (GatherMerge *) node->ps.plan;
	PlanState  *outerPlan = outerPlanState(node);

	/* 确保现有 worker 都被妥善关闭                  */
	ExecShutdownGatherMergeWorkers(node);

	/* 释放未使用的元组，避免在重扫描间泄漏内存   */
	gather_merge_clear_tuples(node);

	/* 标记节点，使共享状态在下次调用时重建      */
	node->initialized = false;
	node->gm_initialized = false;

	/*
	 * 设置子节点的 chgParam，告知它下一次扫描在 leader       
	 * 进程内可能交付不同的行集。（总体行集不应变化，
	 * 但 leader 进程的子集可能变化；因此从本节点到并行
	 * 表扫描节点间的节点不得基于行集不变的假设进行   
	 * 优化。）                        
	 */
	if (gm->rescan_param >= 0)
		outerPlan->chgParam = bms_add_member(outerPlan->chgParam,
											 gm->rescan_param);

	/*
	 * 若子节点的 chgParam 非空，则计划会被第一次        
	 * ExecProcNode 重新扫描。注：如果我们设有             
	 * rescan_param，那么它当前不做任何事，由此可以保证     
	 * 感知并行的子节点在收到 ReInitializeDSM 调用之前         
	 * 不会看到 ReScan 调用。不过这种顺序未必可以依赖。 
	 * 一条好的经验法则是：ReInitializeDSM 只应重置             
	 * 共享状态，ReScan 只应重置本地状态，而同时依赖   
	 * 这两步完成的操作必须等到第一次 ExecProcNode 调用。   
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/*
 * 设置 Gather Merge 所需的数据结构。                
 *
 * 我们基于 gm->num_workers 一次性分配它们，该值是     
 * 我们实际会拥有的 worker 数量的上限。重扫描时， 
 * 我们将这些结构重置为空。这一做法简化了跨重扫描
 * 不泄漏内存的问题。       
 *
 * 在 gm_slots[] 数组中，下标 0 属于 leader，下标 1 到 n    
 * 属于 worker。放入 gm_heap 的值对应 gm_slots[] 的            
 * 下标。不过 gm_tuple_buffers[] 数组的下标则从 0 到        
 * n-1；没有 leader 对应的条目。   
 */
static void
gather_merge_setup(GatherMergeState *gm_state)
{
	GatherMerge *gm = castNode(GatherMerge, gm_state->ps.plan);
	int			nreaders = gm->num_workers;
	int			i;

	/*
	 * 为 worker 数量外加一个 leader 槽来分配 gm_slots。          
	 * 槽 0 始终属于 leader。leader 总是调用 ExecProcNode()         
	 * 来读取元组，然后将其直接存入 gm_slots 条目中。    
	 * 对于其他槽，下面的代码调用 ExecInitExtraTupleSlot() 
	 * 来为 worker 的结果创建槽。注意在单次扫描期间，   
	 * 可用的 worker 数量可能少于 num_workers，此时              
	 * 多余条目将闲置不用。         
	 */
	gm_state->gm_slots = (TupleTableSlot **)
		palloc0((nreaders + 1) * sizeof(TupleTableSlot *));

	/* 为每个 worker 分配元组槽和元组数组         */
	gm_state->gm_tuple_buffers = (GMReaderTupleBuffer *)
		palloc0(nreaders * sizeof(GMReaderTupleBuffer));

	for (i = 0; i < nreaders; i++)
	{
		/* 分配长度为 MAX_TUPLE_STORE 的元组数组      */
		gm_state->gm_tuple_buffers[i].tuple =
			(MinimalTuple *) palloc0(sizeof(MinimalTuple) * MAX_TUPLE_STORE);

		/* 为 worker 初始化元组槽    */
		gm_state->gm_slots[i + 1] =
			ExecInitExtraTupleSlot(gm_state->ps.state, gm_state->tupDesc,
								   &TTSOpsMinimalTuple);
	}

	/* 为合并分配所需资源          */
	gm_state->gm_heap = binaryheap_allocate(nreaders + 1,
											heap_compare_slots,
											gm_state);
}

/*
 * 初始化 Gather Merge。   
 *
 * 重置各数据结构以确保其为空。然后从 leader 与每个  
 * worker 处取至少一条元组（或设其 "done" 指示器），然后 
 * 建堆。
 */
static void
gather_merge_init(GatherMergeState *gm_state)
{
	int			nreaders = gm_state->nreaders;
	bool		nowait = true;
	int			i;

	/* 断言 gather_merge_setup 已分配足够空间  */
	Assert(nreaders <= castNode(GatherMerge, gm_state->ps.plan)->num_workers);

	/* leader 的元组槽重置为空    */
	gm_state->gm_slots[0] = NULL;

	/* 重置每个 worker 的元组槽和元组数组      */
	for (i = 0; i < nreaders; i++)
	{
		/* 元组数组重置为空   */
		gm_state->gm_tuple_buffers[i].nTuples = 0;
		gm_state->gm_tuple_buffers[i].readCounter = 0;
		/* 将 done 重置为未完成 */
		gm_state->gm_tuple_buffers[i].done = false;
		/* 确保输出槽为空       */
		ExecClearTuple(gm_state->gm_slots[i + 1]);
	}

	/* 将二叉堆重置为空   */
	binaryheap_reset(gm_state->gm_heap);

	/*
	 * 先以 nowait 模式尝试从每个 worker（包括 leader）    
	 * 读取一条元组。此后，若并非所有 worker 都能产出  
	 * 元组（或 "done" 指示），则再从其余 worker 读取，    
	 * 改用等待模式。将全部活跃读取器（至少产出一条  
	 * 元组者）加入堆中。  
	 */
reread:
	for (i = 0; i <= nreaders; i++)
	{
		CHECK_FOR_INTERRUPTS();

		/* 若该源已知已完成，则跳过   */
		if ((i == 0) ? gm_state->need_to_scan_locally :
			!gm_state->gm_tuple_buffers[i - 1].done)
		{
			if (TupIsNull(gm_state->gm_slots[i]))
			{
				/* 还没有元组，尝试获取一条   */
				if (gather_merge_readnext(gm_state, i, nowait))
					binaryheap_add_unordered(gm_state->gm_heap,
											 Int32GetDatum(i));
			}
			else
			{
				/*
				 * 我们已从该 worker 拿到至少一条元组，但  
				 * 不妨看看它现在是否还有更多就绪。  
				 */
				load_tuple_array(gm_state, i);
			}
		}
	}

	/* 无需复查 leader，因为 nowait 对它无关紧要      */
	for (i = 1; i <= nreaders; i++)
	{
		if (!gm_state->gm_tuple_buffers[i - 1].done &&
			TupIsNull(gm_state->gm_slots[i]))
		{
			nowait = false;
			goto reread;
		}
	}

	/* 现在执行堆化。 */
	binaryheap_build(gm_state->gm_heap);

	gm_state->gm_initialized = true;
}

/*
 * 对每个 gather merge 输入，清空元组表槽以及      
 * 未用的待处理元组。 
 */
static void
gather_merge_clear_tuples(GatherMergeState *gm_state)
{
	int			i;

	for (i = 0; i < gm_state->nreaders; i++)
	{
		GMReaderTupleBuffer *tuple_buffer = &gm_state->gm_tuple_buffers[i];

		while (tuple_buffer->readCounter < tuple_buffer->nTuples)
			pfree(tuple_buffer->tuple[tuple_buffer->readCounter++]);

		ExecClearTuple(gm_state->gm_slots[i + 1]);
	}
}

/*
 * 为 gather merge 取下一元组。  
 *
 * 从堆中取出已排序的元组。   
 */
static TupleTableSlot *
gather_merge_getnext(GatherMergeState *gm_state)
{
	int			i;

	if (!gm_state->gm_initialized)
	{
		/*
		 * 首次执行时：从每个参与者处拉取第一条元组，并 
		 * 建立堆。    
		 */
		gather_merge_init(gm_state);
	}
	else
	{
		/*
		 * 否则，从我们上次返回的参与者拉取下一个   
		 * 元组，并将该参与者的下标重新插入堆中，因为它 
		 * 现在与其他堆元素的比较结果                       
		 * 可能已经不同。      
		 */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));

		if (gather_merge_readnext(gm_state, i, false))
			binaryheap_replace_first(gm_state->gm_heap, Int32GetDatum(i));
		else
		{
			/* 读取器耗尽，将其从堆移除  */
			(void) binaryheap_remove_first(gm_state->gm_heap);
		}
	}

	if (binaryheap_empty(gm_state->gm_heap))
	{
		/* 所有队列都已耗尽，堆也已耗尽       */
		gather_merge_clear_tuples(gm_state);
		return NULL;
	}
	else
	{
		/* 从拥有首位元组的参与者返回下一个元组           */
		i = DatumGetInt32(binaryheap_first(gm_state->gm_heap));
		return gm_state->gm_slots[i];
	}
}

/*
 * 以 nowait 模式为给定读取器读取元组，并装入其元组  
 * 数组，直到已有 MAX_TUPLE_STORE 条或将要阻塞为止。   
 */
static void
load_tuple_array(GatherMergeState *gm_state, int reader)
{
	GMReaderTupleBuffer *tuple_buffer;
	int			i;

	/* 若是 leader 则什么都不做。      */
	if (reader == 0)
		return;

	tuple_buffer = &gm_state->gm_tuple_buffers[reader - 1];

	/* 若数组中没有内容，则将计数器重置为零。    */
	if (tuple_buffer->nTuples == tuple_buffer->readCounter)
		tuple_buffer->nTuples = tuple_buffer->readCounter = 0;

	/* 尝试填充数组中的其余槽位。    */
	for (i = tuple_buffer->nTuples; i < MAX_TUPLE_STORE; i++)
	{
		MinimalTuple tuple;

		tuple = gm_readnext_tuple(gm_state,
								  reader,
								  true,
								  &tuple_buffer->done);
		if (!tuple)
			break;
		tuple_buffer->tuple[i] = tuple;
		tuple_buffer->nTuples++;
	}
}

/*
 * 将给定读取器的下一条元组存入相应的槽。         
 *
 * 成功则返回 true，否则返回 false（读取器已耗尽，    
 * 或者不想等待元组）。若发现读取器已耗尽，     
 * 则设置 done 标志。 
 */
static bool
gather_merge_readnext(GatherMergeState *gm_state, int reader, bool nowait)
{
	GMReaderTupleBuffer *tuple_buffer;
	MinimalTuple tup;

	/*
	 * 如果要求我们从 leader 生成元组，那么我们只需像     
	 * 平常一样调用 ExecProcNode 生成。  
	 */
	if (reader == 0)
	{
		if (gm_state->need_to_scan_locally)
		{
			PlanState  *outerPlan = outerPlanState(gm_state);
			TupleTableSlot *outerTupleSlot;
			EState	   *estate = gm_state->ps.state;

			/* 执行计划期间安装自身 DSA 区域。   */
			estate->es_query_dsa = gm_state->pei ? gm_state->pei->area : NULL;
			outerTupleSlot = ExecProcNode(outerPlan);
			estate->es_query_dsa = NULL;

			if (!TupIsNull(outerTupleSlot))
			{
				gm_state->gm_slots[0] = outerTupleSlot;
				return true;
			}
			/* need_to_scan_locally 充当 leader 的 "done" 标志  */
			gm_state->need_to_scan_locally = false;
		}
		return false;
	}

	/* 否则，检查相关元组缓冲区的状态。         */
	tuple_buffer = &gm_state->gm_tuple_buffers[reader - 1];

	if (tuple_buffer->nTuples > tuple_buffer->readCounter)
	{
		/* 返回之前读取且仍在缓冲中的任何元组。   */
		tup = tuple_buffer->tuple[tuple_buffer->readCounter++];
	}
	else if (tuple_buffer->done)
	{
		/* 已知读取器已耗尽。      */
		return false;
	}
	else
	{
		/* 读下一元组并缓冲。 */
		tup = gm_readnext_tuple(gm_state,
								reader,
								nowait,
								&tuple_buffer->done);
		if (!tup)
			return false;

		/*
		 * 尝试以 nowait 模式读取更多元组，存入读取器的  
		 * 待处理元组数组中。        
		 */
		load_tuple_array(gm_state, reader);
	}

	Assert(tup);

	/* 为给定元组构建 TupleTableSlot         */
	ExecStoreMinimalTuple(tup,	/* 待存元组   */
						  gm_state->gm_slots[reader],	/* slot in which to
														 * store the tuple */
						  true);	/* 用完后 pfree 元组        */

	return true;
}

/*
 * 尝试从给定 worker 读取元组。    
 */
static MinimalTuple
gm_readnext_tuple(GatherMergeState *gm_state, int nreader, bool nowait,
				  bool *done)
{
	TupleQueueReader *reader;
	MinimalTuple tup;

	/* 检查异步事件，特别是来自 worker 的消息。    */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 尝试读取元组。   
	 *
	 * 注意 TupleQueueReaderNext 对于初始化失败的 worker            
	 * 只会返回 NULL。我们会把该 worker 视作未产出任何   
	 * 元组；等执行到 WaitForParallelWorkersToFinish 时会      
	 * 报错
	 */
	reader = gm_state->reader[nreader - 1];
	tup = TupleQueueReaderNext(reader, nowait, done);

	/*
	 * 由于我们会在多次调用之间缓冲这些元组，因此需要  
	 * copy 
	 */
	return tup ? heap_copy_minimal_tuple(tup) : NULL;
}

/*
 * 堆数组中的每一项都有一个对应的槽。我们用        
 * SlotNumber 存储槽下标。这并未真正提供任何形式  
 * 的类型安全，但能让代码更具自明性。      
 */
typedef int32 SlotNumber;

/*
 * 比较两个给定槽中的元组。      
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	GatherMergeState *node = (GatherMergeState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->gm_slots[slot1];
	TupleTableSlot *s2 = node->gm_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->gm_nkeys; nkey++)
	{
		SortSupport sortKey = node->gm_sortkeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int			compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
		{
			INVERT_COMPARE_RESULT(compare);
			return compare;
		}
	}
	return 0;
}
