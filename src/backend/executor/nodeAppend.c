/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  处理 Append 节点的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- 初始化 append 节点   
 *		ExecAppend		- 从该节点取下一条元组       
 *		ExecEndAppend	- 关闭 append 节点     
 *		ExecReScanAppend - 重扫 append 节点  
 *
 *	 NOTES
 *		每个 append 节点都包含一个或多个子计划         
 *		构成的列表，需按正向或反向迭代处理。
 *		元组通过执行第 'whichplan' 个子计划来获取， 
 *		直到该子计划不再返回元组为止，此时该计划 
 *		会被关闭，并启动下一个。      
 *
 *		Append 节点不使用其左右子树，           
 *		而是维护一个子计划列表，因此          
 *		典型的 append 节点在计划树中形如：        
 *
 *				   ...
 *				   /
 *				Append -------+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 *
 *		Append 节点目前用于 union 查询，并用于支持  
 *		继承查询，因为这类查询需要扫描多个关系。    
 *		例如，在我们标准的 person/student/employee/            
 *		student-emp 例子中，student 和 employee 继承自 
 *		person，student-emp 继承自 student 和 employee。 
 *		查询
 *
 *				select name from person
 *
 *		生成的计划： 
 *
 *				  |
 *				Append -------+-------+--------+--------+
 *				/	\		  |		  |		   |		|
 *			  nil	nil		 Scan	 Scan	  Scan	   Scan
 *							  |		  |		   |		|
 *							person employee student student-emp
 */

#include "postgres.h"

#include "executor/execAsync.h"
#include "executor/execdebug.h"
#include "executor/execPartition.h"
#include "executor/nodeAppend.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"

/* 并行感知 Append 的共享状态。  */
struct ParallelAppendState
{
	LWLock		pa_lock;		/* 选择下一个子计划的互斥锁    */
	int			pa_next_plan;	/* worker 将选的下一个计划   */

	/*
	 * 当不应再有 worker 选择子计划时，pa_finished[i] 应置     
	 * 为 true。对非 partial 计划，一旦 worker 选中该计划   
	 * 就应把它设为 true；而对 partial 计划，它会保持     
	 * 为 false，直到有 worker 将其做完。
	 */
	bool		pa_finished[FLEXIBLE_ARRAY_MEMBER];
};

#define INVALID_SUBPLAN_INDEX		-1
#define EVENT_BUFFER_SIZE			16

static TupleTableSlot *ExecAppend(PlanState *pstate);
static bool choose_next_subplan_locally(AppendState *node);
static bool choose_next_subplan_for_leader(AppendState *node);
static bool choose_next_subplan_for_worker(AppendState *node);
static void mark_invalid_subplans_as_finished(AppendState *node);
static void ExecAppendAsyncBegin(AppendState *node);
static bool ExecAppendAsyncGetNext(AppendState *node, TupleTableSlot **result);
static bool ExecAppendAsyncRequest(AppendState *node, TupleTableSlot **result);
static void ExecAppendAsyncEventWait(AppendState *node);
static void classify_matching_subplans(AppendState *node);

/* ----------------------------------------------------------------
 *		ExecInitAppend
 *
 *		启动 append 节点的所有子扫描。    
 *
 *	   （可能有些浪费，因为未必扫描 append 节点的  
 *		全部结果，但所有结构会分配到执行器    
 *		顶层内存块里，而非 ExecAppend 调用所用的     
 *		内存块中。）                               
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate, int eflags)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	Bitmapset  *validsubplans;
	Bitmapset  *asyncplans;
	int			nplans;
	int			nasyncplans;
	int			firstvalid;
	int			i,
				j;

	/* 检查不支持的标志    */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * 为 append 节点创建新的 AppendState 
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->ps.ExecProcNode = ExecAppend;

	/* 让 choose_next_subplan_* 函数来负责设置第一个子计划   */
	appendstate->as_whichplan = INVALID_SUBPLAN_INDEX;
	appendstate->as_syncdone = false;
	appendstate->as_begun = false;

	/* 如果启用了运行时分区剪枝，则现在就进行设置*/
	if (node->part_prune_info != NULL)
	{
		PartitionPruneState *prunestate;

		/*
		 * 建立剪枝数据结构。这也会初始化子计划集合    
		 * （validsubplans），其中考虑了执行初始剪枝          
		 * （若有）的结果。                    
		 */
		prunestate = ExecInitPartitionPruning(&appendstate->ps,
											  list_length(node->appendplans),
											  node->part_prune_info,
											  &validsubplans);
		appendstate->as_prune_state = prunestate;
		nplans = bms_num_members(validsubplans);

		/*
		 * 当无需运行时剪枝且至少有一个子计划时，可 
		 * 以立即填充 as_valid_subplans，避免稍后再调用     
		 * ExecFindMatchingSubPlans。             
		 */
		if (!prunestate->do_exec_prune && nplans > 0)
		{
			appendstate->as_valid_subplans = bms_add_range(NULL, 0, nplans - 1);
			appendstate->as_valid_subplans_identified = true;
		}
	}
	else
	{
		nplans = list_length(node->appendplans);

		/*
		 * 当运行时分区剪枝未启用时，直接把全部子计划    
		 * 标为有效；它们也必须全部初始化。     
		 */
		Assert(nplans > 0);
		appendstate->as_valid_subplans = validsubplans =
			bms_add_range(NULL, 0, nplans - 1);
		appendstate->as_valid_subplans_identified = true;
		appendstate->as_prune_state = NULL;
	}

	/*
	 * 初始化结果元组类型和槽。  
	 */
	ExecInitResultTupleSlotTL(&appendstate->ps, &TTSOpsVirtual);

	/* 节点返回的槽来自其各个子节点，因此并非固定   */
	appendstate->ps.resultopsset = true;
	appendstate->ps.resultopsfixed = false;

	appendplanstates = (PlanState **) palloc(nplans *
											 sizeof(PlanState *));

	/*
	 * 对每个待执行的有效计划调用 ExecInitNode，并把       
	 * 结果存入 appendplanstates 数组。     
	 *
	 * 顺便找出第一个有效的 partial 计划。   
	 */
	j = 0;
	asyncplans = NULL;
	nasyncplans = 0;
	firstvalid = nplans;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->appendplans, i);

		/*
		 * 记录异步子计划。执行 EvalPlanQual 时会把它们       
		 * 当作同步子计划；但初始化 EvalPlanQual 计划树时除 
		 * 外  
		 */
		if (initNode->async_capable && estate->es_epq_active == NULL)
		{
			asyncplans = bms_add_member(asyncplans, j);
			nasyncplans++;
		}

		/*
		 * 记录作为有效 partial 计划的最小 appendplans 下标。  
		 */
		if (i >= node->first_partial_plan && j < firstvalid)
			firstvalid = j;

		appendplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	appendstate->as_first_partial_plan = firstvalid;
	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/* 初始化异步状态  */
	appendstate->as_asyncplans = asyncplans;
	appendstate->as_nasyncplans = nasyncplans;
	appendstate->as_asyncrequests = NULL;
	appendstate->as_asyncresults = NULL;
	appendstate->as_nasyncresults = 0;
	appendstate->as_nasyncremain = 0;
	appendstate->as_needrequest = NULL;
	appendstate->as_eventset = NULL;
	appendstate->as_valid_asyncplans = NULL;

	if (nasyncplans > 0)
	{
		appendstate->as_asyncrequests = (AsyncRequest **)
			palloc0(nplans * sizeof(AsyncRequest *));

		i = -1;
		while ((i = bms_next_member(asyncplans, i)) >= 0)
		{
			AsyncRequest *areq;

			areq = palloc(sizeof(AsyncRequest));
			areq->requestor = (PlanState *) appendstate;
			areq->requestee = appendplanstates[i];
			areq->request_index = i;
			areq->callback_pending = false;
			areq->request_complete = false;
			areq->result = NULL;

			appendstate->as_asyncrequests[i] = areq;
		}

		appendstate->as_asyncresults = (TupleTableSlot **)
			palloc0(nasyncplans * sizeof(TupleTableSlot *));

		if (appendstate->as_valid_subplans_identified)
			classify_matching_subplans(appendstate);
	}

	/*
	 * 杂项初始化             
	 */

	appendstate->ps.ps_ProjInfo = NULL;

	/* 对于并行查询，此值稍后会被覆盖。   */
	appendstate->choose_next_subplan = choose_next_subplan_locally;

	return appendstate;
}

/* ----------------------------------------------------------------
 *	   ExecAppend
 *
 *		处理对多个子计划的迭代。     
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecAppend(PlanState *pstate)
{
	AppendState *node = castNode(AppendState, pstate);
	TupleTableSlot *result;

	/*
	 * 如果这是 Init 或 ReScan 之后的首次调用，就需要   
	 * 进行初始化。  
	 */
	if (!node->as_begun)
	{
		Assert(node->as_whichplan == INVALID_SUBPLAN_INDEX);
		Assert(!node->as_syncdone);

		/* 如果没有子计划，则无事可做*/
		if (node->as_nplans == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/* 若存在异步子计划，则开始执行它们。    */
		if (node->as_nasyncplans > 0)
			ExecAppendAsyncBegin(node);

		/*
		 * 若尚未选定同步子计划，必须先选定一个再    
		 * 继续。  
		 */
		if (!node->choose_next_subplan(node) && node->as_nasyncremain == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		Assert(node->as_syncdone ||
			   (node->as_whichplan >= 0 &&
				node->as_whichplan < node->as_nplans));

		/* 初始化完成。     */
		node->as_begun = true;
	}

	for (;;)
	{
		PlanState  *subnode;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 尝试从异步子计划（若有）取元组  
		 */
		if (node->as_syncdone || !bms_is_empty(node->as_needrequest))
		{
			if (ExecAppendAsyncGetNext(node, &result))
				return result;
			Assert(!node->as_syncdone);
			Assert(bms_is_empty(node->as_needrequest));
		}

		/*
		 * 弄清当前正在处理的是哪个同步子计划      
		 */
		Assert(node->as_whichplan >= 0 && node->as_whichplan < node->as_nplans);
		subnode = node->appendplans[node->as_whichplan];

		/*
		 * 从子计划取一条元组 
		 */
		result = ExecProcNode(subnode);

		if (!TupIsNull(result))
		{
			/*
			 * 如果子计划给出了元组，就原样返回。我们并
			 * 不使用 ExecInitAppend 中设置的结果槽， 
			 * 因为没有这个必要。            
			 */
			return result;
		}

		/*
		 * 若有异步事件则等待或轮询。我们把这一步放在 
		 * 检查迭代结束与否之前，因为它可能排空剩余  
		 * 异步子计划
		 */
		if (node->as_nasyncremain > 0)
			ExecAppendAsyncEventWait(node);

		/* 选择新同步子计划；无同步/异步子计划即完成  */
		if (!node->choose_next_subplan(node) && node->as_nasyncremain == 0)
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);
	}
}

/* ----------------------------------------------------------------
 *		ExecEndAppend
 *
 *		关闭 append 节点的各个子扫描。  
 *
 *		无有价值的返回值。 
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(AppendState *node)
{
	PlanState **appendplans;
	int			nplans;
	int			i;

	/*
	 * 从节点获取信息        
	 */
	appendplans = node->appendplans;
	nplans = node->as_nplans;

	/*
	 * 关闭每一个子扫描      
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(appendplans[i]);
}

void
ExecReScanAppend(AppendState *node)
{
	int			nasyncplans = node->as_nasyncplans;
	int			i;

	/*
	 * 如果剪枝表达式中用到的任何 PARAM_EXEC 参数发生了     
	 * 变化，则最好取消有效子计划集合，以便针对新     
	 * 参数值重新选定。 
	 */
	if (node->as_prune_state &&
		bms_overlap(node->ps.chgParam,
					node->as_prune_state->execparamids))
	{
		node->as_valid_subplans_identified = false;
		bms_free(node->as_valid_subplans);
		node->as_valid_subplans = NULL;
		bms_free(node->as_valid_asyncplans);
		node->as_valid_asyncplans = NULL;
	}

	for (i = 0; i < node->as_nplans; i++)
	{
		PlanState  *subnode = node->appendplans[i];

		/*
		 * ExecReScan 不了解我的子计划，因此参数变化  
		 * 信号通知由我自行完成。  
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * 如果子节点的 chgParam 非空，计划就会在首次        
		 * ExecProcNode 或 ExecAsyncRequest 时重扫。  
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}

	/* 重置异步状态*/
	if (nasyncplans > 0)
	{
		i = -1;
		while ((i = bms_next_member(node->as_asyncplans, i)) >= 0)
		{
			AsyncRequest *areq = node->as_asyncrequests[i];

			areq->callback_pending = false;
			areq->request_complete = false;
			areq->result = NULL;
		}

		node->as_nasyncresults = 0;
		node->as_nasyncremain = 0;
		bms_free(node->as_needrequest);
		node->as_needrequest = NULL;
	}

	/* 让 choose_next_subplan_* 函数来负责设置第一个子计划   */
	node->as_whichplan = INVALID_SUBPLAN_INDEX;
	node->as_syncdone = false;
	node->as_begun = false;
}

/* ----------------------------------------------------------------
 *						并行 Append 支持   
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecAppendEstimate
 *
 *		计算我们在并行查询 DSM 中所需的空间， 
 *		把需求告知 pcxt->estimator。                    
 * ----------------------------------------------------------------
 */
void
ExecAppendEstimate(AppendState *node,
				   ParallelContext *pcxt)
{
	node->pstate_len =
		add_size(offsetof(ParallelAppendState, pa_finished),
				 sizeof(bool) * node->as_nplans);

	shm_toc_estimate_chunk(&pcxt->estimator, node->pstate_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}


/* ----------------------------------------------------------------
 *		ExecAppendInitializeDSM
 *
 *		为并行 Append 建立共享状态。  
 * ----------------------------------------------------------------
 */
void
ExecAppendInitializeDSM(AppendState *node,
						ParallelContext *pcxt)
{
	ParallelAppendState *pstate;

	pstate = shm_toc_allocate(pcxt->toc, node->pstate_len);
	memset(pstate, 0, node->pstate_len);
	LWLockInitialize(&pstate->pa_lock, LWTRANCHE_PARALLEL_APPEND);
	shm_toc_insert(pcxt->toc, node->ps.plan->plan_node_id, pstate);

	node->as_pstate = pstate;
	node->choose_next_subplan = choose_next_subplan_for_leader;
}

/* ----------------------------------------------------------------
 *		ExecAppendReInitializeDSM
 *
 *		在开始全新扫描之前重置共享状态。 
 * ----------------------------------------------------------------
 */
void
ExecAppendReInitializeDSM(AppendState *node, ParallelContext *pcxt)
{
	ParallelAppendState *pstate = node->as_pstate;

	pstate->pa_next_plan = 0;
	memset(pstate->pa_finished, 0, sizeof(bool) * node->as_nplans);
}

/* ----------------------------------------------------------------
 *		ExecAppendInitializeWorker
 *
 *		把 TOC 中的相关信息复制到 planstate 里，并初始化 
 *		选择并执行最优子计划所需的一切。               
 * ----------------------------------------------------------------
 */
void
ExecAppendInitializeWorker(AppendState *node, ParallelWorkerContext *pwcxt)
{
	node->as_pstate = shm_toc_lookup(pwcxt->toc, node->ps.plan->plan_node_id, false);
	node->choose_next_subplan = choose_next_subplan_for_worker;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_locally
 *
 *		为非并行感知 Append 选下一个同步子计划， 
 *		如果没有更多则返回 false。 
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_locally(AppendState *node)
{
	int			whichplan = node->as_whichplan;
	int			nextplan;

	/* 如果没有子计划，则决不应被调用        */
	Assert(node->as_nplans > 0);

	/* syncdone 时无事可做  */
	if (node->as_syncdone)
		return false;

	/*
	 * 如果是首次调用，则把 whichplan 初始化为 -1，让 bms 成
	 * 员函数选出第一个有效的同步子计划。如果碰巧不     
	 * 存在有效的同步子计划，bms 成员函数会返回负数    
	 * 来处理这种情况，使我们得以退出并返回 false       
	 * 值。      
	 */
	if (whichplan == INVALID_SUBPLAN_INDEX)
	{
		if (node->as_nasyncplans > 0)
		{
			/* as_valid_subplans 应已填充完毕       */
			Assert(node->as_valid_subplans_identified);
		}
		else if (!node->as_valid_subplans_identified)
		{
			node->as_valid_subplans =
				ExecFindMatchingSubPlans(node->as_prune_state, false);
			node->as_valid_subplans_identified = true;
		}

		whichplan = -1;
	}

	/* 确保 whichplan 处于预期范围之内     */
	Assert(whichplan >= -1 && whichplan <= node->as_nplans);

	if (ScanDirectionIsForward(node->ps.state->es_direction))
		nextplan = bms_next_member(node->as_valid_subplans, whichplan);
	else
		nextplan = bms_prev_member(node->as_valid_subplans, whichplan);

	if (nextplan < 0)
	{
		/* 异步模式下设置 as_syncdone*/
		if (node->as_nasyncplans > 0)
			node->as_syncdone = true;
		return false;
	}

	node->as_whichplan = nextplan;

	return true;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_for_leader
 *
 *      尽量挑选一个不需要我们在本地花费太多  
 *      工作的计划，让尽可能多的工作由 worker     
 *      去做。最便宜的子计划排在末尾。  
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_for_leader(AppendState *node)
{
	ParallelAppendState *pstate = node->as_pstate;

	/* 并行感知的计划不支持反向扫描             */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* 如果没有子计划，则决不应被调用        */
	Assert(node->as_nplans > 0);

	LWLockAcquire(&pstate->pa_lock, LW_EXCLUSIVE);

	if (node->as_whichplan != INVALID_SUBPLAN_INDEX)
	{
		/* 把刚完成子计划标为已完成。  */
		node->as_pstate->pa_finished[node->as_whichplan] = true;
	}
	else
	{
		/* 从最后一个开始。 */
		node->as_whichplan = node->as_nplans - 1;

		/*
		 * 如果尚未确定有效子计划，现在就进行确定。    
		 * 如果禁用了运行时剪枝，则有效子计划将始终被设 
		 * 为全部子计划  
		 */
		if (!node->as_valid_subplans_identified)
		{
			node->as_valid_subplans =
				ExecFindMatchingSubPlans(node->as_prune_state, false);
			node->as_valid_subplans_identified = true;

			/*
			 * 把每个无效计划标为已结束，让下面的循环能 
			 * 选出首个有效子计划。 
			 */
			mark_invalid_subplans_as_finished(node);
		}
	}

	/* 循环直到找到待执行子计划。  */
	while (pstate->pa_finished[node->as_whichplan])
	{
		if (node->as_whichplan == 0)
		{
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
			node->as_whichplan = INVALID_SUBPLAN_INDEX;
			LWLockRelease(&pstate->pa_lock);
			return false;
		}

		/*
		 * 这里无需理会 as_valid_subplans，因为所有无效        
		 * 计划均已被标为已结束。  
		 */
		node->as_whichplan--;
	}

	/* 非 partial 则立即标记为已结束。    */
	if (node->as_whichplan < node->as_first_partial_plan)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	LWLockRelease(&pstate->pa_lock);

	return true;
}

/* ----------------------------------------------------------------
 *		choose_next_subplan_for_worker
 *
 *		为并行感知 Append 选择下一个子计划，如果  
 *		无更多则返回 false。
 *
 *		从第一个计划开始沿列表逐个推进；当回到 
 *		末端时，再绕回到第一个                     
 *		partial 计划。这样会先按成本降序安排非     
 *		partial 计划，然后把 worker 尽量均匀地分     
 *		散到其余 partial 计划上。                        
 * ----------------------------------------------------------------
 */
static bool
choose_next_subplan_for_worker(AppendState *node)
{
	ParallelAppendState *pstate = node->as_pstate;

	/* 并行感知的计划不支持反向扫描             */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* 如果没有子计划，则决不应被调用        */
	Assert(node->as_nplans > 0);

	LWLockAcquire(&pstate->pa_lock, LW_EXCLUSIVE);

	/* 把刚完成子计划标为已完成。  */
	if (node->as_whichplan != INVALID_SUBPLAN_INDEX)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	/*
	 * 如果尚未确定有效子计划，现在就进行确定。    
	 * 如果禁用了运行时剪枝，则有效子计划总会被设为     
	 * 全部子计划 
	 */
	else if (!node->as_valid_subplans_identified)
	{
		node->as_valid_subplans =
			ExecFindMatchingSubPlans(node->as_prune_state, false);
		node->as_valid_subplans_identified = true;

		mark_invalid_subplans_as_finished(node);
	}

	/* 如果所有计划都已完成，我们便无事可做   */
	if (pstate->pa_next_plan == INVALID_SUBPLAN_INDEX)
	{
		LWLockRelease(&pstate->pa_lock);
		return false;
	}

	/* 保存作为搜索起点的计划。                 */
	node->as_whichplan = pstate->pa_next_plan;

	/* 循环直至找到有效子计划来执行。  */
	while (pstate->pa_finished[pstate->pa_next_plan])
	{
		int			nextplan;

		nextplan = bms_next_member(node->as_valid_subplans,
								   pstate->pa_next_plan);
		if (nextplan >= 0)
		{
			/* 前进到下一有效计划。  */
			pstate->pa_next_plan = nextplan;
		}
		else if (node->as_whichplan > node->as_first_partial_plan)
		{
			/*
			 * 尝试绕回首个有效的 partial 计划（若有）；    
			 * 若没有，则安排在下方退出。         
			 */
			nextplan = bms_next_member(node->as_valid_subplans,
									   node->as_first_partial_plan - 1);
			pstate->pa_next_plan =
				nextplan < 0 ? node->as_whichplan : nextplan;
		}
		else
		{
			/*
			 * 已是最后一个计划，且要么无 partial 计划，   
			 * 要么均已试过。安排退出。 
			 */
			pstate->pa_next_plan = node->as_whichplan;
		}

		if (pstate->pa_next_plan == node->as_whichplan)
		{
			/* 全部都试过了！   */
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
			LWLockRelease(&pstate->pa_lock);
			return false;
		}
	}

	/* 选定找到的计划，并把 pa_next_plan 再推进一次。  */
	node->as_whichplan = pstate->pa_next_plan;
	pstate->pa_next_plan = bms_next_member(node->as_valid_subplans,
										   pstate->pa_next_plan);

	/*
	 * 如果没有更多有效计划了，则尝试把下一个置为首个 
	 * 有效 partial 计划    
	 */
	if (pstate->pa_next_plan < 0)
	{
		int			nextplan = bms_next_member(node->as_valid_subplans,
											   node->as_first_partial_plan - 1);

		if (nextplan >= 0)
			pstate->pa_next_plan = nextplan;
		else
		{
			/*
			 * 已经没有有效的 partial 计划，且最后一个非      
			 * partial 计划也已选过；所以标记我们的伙伴   
			 * worker 无事可做  
			 */
			pstate->pa_next_plan = INVALID_SUBPLAN_INDEX;
		}
	}

	/* 非 partial 则立即标记为已结束。    */
	if (node->as_whichplan < node->as_first_partial_plan)
		node->as_pstate->pa_finished[node->as_whichplan] = true;

	LWLockRelease(&pstate->pa_lock);

	return true;
}

/*
 * mark_invalid_subplans_as_finished
 *		ParallelAppendState 的 pa_finished 对各无效子计划置为     
 *		true。 
 *
 * 本函数只应被用于启用了运行时剪枝的并行            
 * Append。       
 */
static void
mark_invalid_subplans_as_finished(AppendState *node)
{
	int			i;

	/* 仅在并行 Append 模式下调用才有效          */
	Assert(node->as_pstate);

	/* 在未启用运行时剪枝时不应调用本函数             */
	Assert(node->as_prune_state);

	/* 全部计划有效时无事可做    */
	if (bms_num_members(node->as_valid_subplans) == node->as_nplans)
		return;

	/* 把所有无效计划标为已结束 */
	for (i = 0; i < node->as_nplans; i++)
	{
		if (!bms_is_member(i, node->as_valid_subplans))
			node->as_pstate->pa_finished[i] = true;
	}
}

/* ----------------------------------------------------------------
 *						异步 Append 支持       
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecAppendAsyncBegin
 *
 *		开始执行指定的支持异步的子计划。
 * ----------------------------------------------------------------
 */
static void
ExecAppendAsyncBegin(AppendState *node)
{
	int			i;

	/* 异步感知的 Append 不支持反向扫描。        */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* 如果没有子计划，则决不应被调用        */
	Assert(node->as_nplans > 0);

	/* 如果没有异步子计划，则决不应被调用。      */
	Assert(node->as_nasyncplans > 0);

	/* 如果尚未确定有效子计划，现在就进行确定。 */
	if (!node->as_valid_subplans_identified)
	{
		node->as_valid_subplans =
			ExecFindMatchingSubPlans(node->as_prune_state, false);
		node->as_valid_subplans_identified = true;

		classify_matching_subplans(node);
	}

	/* 初始化状态变量。    */
	node->as_syncdone = bms_is_empty(node->as_valid_subplans);
	node->as_nasyncremain = bms_num_members(node->as_valid_asyncplans);

	/* 若无有效的异步子计划，则无事可做。 */
	if (node->as_nasyncremain == 0)
		return;

	/* 为每个有效的异步子计划发起请求。     */
	i = -1;
	while ((i = bms_next_member(node->as_valid_asyncplans, i)) >= 0)
	{
		AsyncRequest *areq = node->as_asyncrequests[i];

		Assert(areq->request_index == i);
		Assert(!areq->callback_pending);

		/* 做实际工作。  */
		ExecAsyncRequest(areq);
	}
}

/* ----------------------------------------------------------------
 *		ExecAppendAsyncGetNext
 *
 *		从任意一个异步子计划获取下一条元组。   
 * ----------------------------------------------------------------
 */
static bool
ExecAppendAsyncGetNext(AppendState *node, TupleTableSlot **result)
{
	*result = NULL;

	/* 如果没有有效的异步子计划，则决不应被调用。   */
	Assert(node->as_nasyncremain > 0);

	/* 异步请求一条元组。     */
	if (ExecAppendAsyncRequest(node, result))
		return true;

	while (node->as_nasyncremain > 0)
	{
		CHECK_FOR_INTERRUPTS();

		/* 等待或轮询异步事件。 */
		ExecAppendAsyncEventWait(node);

		/* 异步请求一条元组。     */
		if (ExecAppendAsyncRequest(node, result))
			return true;

		/* 如果还有未完成的同步子计划，则跳出循环。     */
		if (!node->as_syncdone)
			break;
	}

	/*
	 * 如果所有同步子计划都已完成，那么对给定节点的
	 * 扫描就全部完成了。否则异步部分虽已完毕，但仍     
	 * 须继续扫描同步子计划。   
	 */
	if (node->as_syncdone)
	{
		Assert(node->as_nasyncremain == 0);
		*result = ExecClearTuple(node->ps.ps_ResultTupleSlot);
		return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecAppendAsyncRequest
 *
 *		异步请求一条元组。    
 * ----------------------------------------------------------------
 */
static bool
ExecAppendAsyncRequest(AppendState *node, TupleTableSlot **result)
{
	Bitmapset  *needrequest;
	int			i;

	/* 如果没有需要新请求的异步子计划，则无事可做。  */
	if (bms_is_empty(node->as_needrequest))
	{
		Assert(node->as_nasyncresults == 0);
		return false;
	}

	/*
	 * 如果还有一些异步生成的结果尚未返回，那么       
	 * 我们无事可做；直接返回其中一个即可。        
	 */
	if (node->as_nasyncresults > 0)
	{
		--node->as_nasyncresults;
		*result = node->as_asyncresults[node->as_nasyncresults];
		return true;
	}

	/* 为每个有需要的异步子计划发起新请求。          */
	needrequest = node->as_needrequest;
	node->as_needrequest = NULL;
	i = -1;
	while ((i = bms_next_member(needrequest, i)) >= 0)
	{
		AsyncRequest *areq = node->as_asyncrequests[i];

		/* 做实际工作。  */
		ExecAsyncRequest(areq);
	}
	bms_free(needrequest);

	/* 如果存在，则返回一个异步生成的结果。     */
	if (node->as_nasyncresults > 0)
	{
		--node->as_nasyncresults;
		*result = node->as_asyncresults[node->as_nasyncresults];
		return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecAppendAsyncEventWait
 *
 *		等待或轮询文件描述符事件并触发回调。     
 * ----------------------------------------------------------------
 */
static void
ExecAppendAsyncEventWait(AppendState *node)
{
	int			nevents = node->as_nasyncplans + 2;
	long		timeout = node->as_syncdone ? -1 : 0;
	WaitEvent	occurred_event[EVENT_BUFFER_SIZE];
	int			noccurred;
	int			i;

	/* 如果没有有效的异步子计划，则决不应被调用。   */
	Assert(node->as_nasyncremain > 0);

	Assert(node->as_eventset == NULL);
	node->as_eventset = CreateWaitEventSet(CurrentMemoryContext, nevents);
	PG_TRY();
	{
		AddWaitEventToSet(node->as_eventset, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  NULL, NULL);

		/* 让每个等待的子计划有机会添加事件。 */
		i = -1;
		while ((i = bms_next_member(node->as_asyncplans, i)) >= 0)
		{
			AsyncRequest *areq = node->as_asyncrequests[i];

			if (areq->callback_pending)
				ExecAsyncConfigureWait(areq);
		}

		/*
		 * 如果没有任何子计划配置事件，则无需进一步     
		 * 处理。  
		 */
		if (GetNumRegisteredWaitEvents(node->as_eventset) == 1)
			noccurred = 0;
		else
		{
			/*
			 * 把进程 latch 加入集合，以便我们能被唤醒并用  
			 * CHECK_FOR_INTERRUPTS() 处理标准中断。        
			 *
			 * 注意：由于历史原因，这一项必须                  
			 * 晚于 ExecAsyncConfigureWait() 调用加入到              
			 * WaitEventSet 中。具体来说，postgres_fdw 会调用      
			 * "GetNumRegisteredWaitEvents(set) == 1"，以检查            
			 * 集合中是否还有其他事件。这个设计很糟糕，  
			 * postgres_fdw 本就不该这么做，但现在无法改变。 
			 * 该模式亦可能被其他扩展效仿。   
			 */
			AddWaitEventToSet(node->as_eventset, WL_LATCH_SET, PGINVALID_SOCKET,
							  MyLatch, NULL);

			/* 一次至多返回 EVENT_BUFFER_SIZE 个事件。    */
			if (nevents > EVENT_BUFFER_SIZE)
				nevents = EVENT_BUFFER_SIZE;

			/*
			 * 若超时值为 -1，则等到至少一个事件发生；若   
			 * 超时值为 0，则轮询事件但完全不等待。     
			 */
			noccurred = WaitEventSetWait(node->as_eventset, timeout,
										 occurred_event, nevents,
										 WAIT_EVENT_APPEND_READY);
		}
	}
	PG_FINALLY();
	{
		FreeWaitEventSet(node->as_eventset);
		node->as_eventset = NULL;
	}
	PG_END_TRY();
	if (noccurred == 0)
		return;

	/* 分发通知。        */
	for (i = 0; i < noccurred; i++)
	{
		WaitEvent  *w = &occurred_event[i];

		/*
		 * 每个等待的子计划都应该已经注册了它的等待事
		 * 件，其 user_data 指回 AsyncRequest。  
		 */
		if ((w->events & WL_SOCKET_READABLE) != 0)
		{
			AsyncRequest *areq = (AsyncRequest *) w->user_data;

			if (areq->callback_pending)
			{
				/*
				 * 将其标记为不再需要回调。必须在分派回调
				 * 之前完成，以防回调重新设置该                 
				 * 标志。
				 */
				areq->callback_pending = false;

				/* 做实际工作。  */
				ExecAsyncNotify(areq);
			}
		}

		/* 处理标准中断         */
		if ((w->events & WL_LATCH_SET) != 0)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecAsyncAppendResponse
 *
 *		接收我们发出的异步请求的响应。           
 * ----------------------------------------------------------------
 */
void
ExecAsyncAppendResponse(AsyncRequest *areq)
{
	AppendState *node = (AppendState *) areq->requestor;
	TupleTableSlot *slot = areq->result;

	/* 结果应为 TupleTableSlot 或 NULL。        */
	Assert(slot == NULL || IsA(slot, TupleTableSlot));

	/* 请求挂起时无事可做。           */
	if (!areq->request_complete)
	{
		/* 该请求之前应是等待回调的挂起状态。 */
		Assert(areq->callback_pending);
		return;
	}

	/* 如果结果为 NULL 或空槽，则没有更多要做的了。     */
	if (TupIsNull(slot))
	{
		/* 已结束的子计划不应在等待回调的挂起状态。  */
		Assert(!areq->callback_pending);
		--node->as_nasyncremain;
		return;
	}

	/* 保存结果以便返回。      */
	Assert(node->as_nasyncresults < node->as_nasyncplans);
	node->as_asyncresults[node->as_nasyncresults++] = slot;

	/*
	 * 把返回了结果的子计划标为可发起新请求。我们        
	 * 并不在这里立即发起下一个，因为它可能会完成。  
	 */
	node->as_needrequest = bms_add_member(node->as_needrequest,
										  areq->request_index);
}

/* ----------------------------------------------------------------
 *		classify_matching_subplans
 *
 *		将节点 as_valid_subplans 分为同步/异步两类，
 *		调整为只含同步子计划，并把异步的保存到
 *		节点的 as_valid_asyncplans 中。         
 * ----------------------------------------------------------------
 */
static void
classify_matching_subplans(AppendState *node)
{
	Bitmapset  *valid_asyncplans;

	Assert(node->as_valid_subplans_identified);
	Assert(node->as_valid_asyncplans == NULL);

	/* 若无有效的子计划，则无事可做。 */
	if (bms_is_empty(node->as_valid_subplans))
	{
		node->as_syncdone = true;
		node->as_nasyncremain = 0;
		return;
	}

	/* 若无有效的异步子计划，则无事可做。 */
	if (!bms_overlap(node->as_valid_subplans, node->as_asyncplans))
	{
		node->as_nasyncremain = 0;
		return;
	}

	/* 取有效异步子计划  */
	valid_asyncplans = bms_intersect(node->as_asyncplans,
									 node->as_valid_subplans);

	/* 把有效子计划调整为只包含同步子计划。   */
	node->as_valid_subplans = bms_del_members(node->as_valid_subplans,
											  valid_asyncplans);

	/* 保存有效异步子计划*/
	node->as_valid_asyncplans = valid_asyncplans;
}
