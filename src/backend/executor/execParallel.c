/*-------------------------------------------------------------------------
 *
 * execParallel.c
 *	  并行执行的支持例程。          
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * 本文件包含一些例程，用于在 PostgreSQL 执行器内部  
 * 建立、使用与拆除 ParallelContext。该机制负责启动    
 * worker 并确保其状态总体与 leader 一致；详见        
 * src/backend/access/transam/README.parallel。                      
 * 不过，必须保存并恢复相关执行器状态，例如       
 * 与查询关联的 ParamListInfo、buffer/WAL 使用信息，     
 * 以及要传递给 worker 的                                          
 * 实际执行计划。                           
 *
 * IDENTIFICATION
 *	  src/backend/executor/execParallel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execParallel.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeCustom.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIncrementalSort.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeMemoize.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "jit/jit.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/dsa.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

/*
 * 并行执行器通信所用的魔数。在此使用大于任何 32  
 * 位整数的常数，使各并行节点能把小于 2^32 的值用作 
 * 自身状态的存储。                              
 */
#define PARALLEL_KEY_EXECUTOR_FIXED		UINT64CONST(0xE000000000000001)
#define PARALLEL_KEY_PLANNEDSTMT		UINT64CONST(0xE000000000000002)
#define PARALLEL_KEY_PARAMLISTINFO		UINT64CONST(0xE000000000000003)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xE000000000000004)
#define PARALLEL_KEY_TUPLE_QUEUE		UINT64CONST(0xE000000000000005)
#define PARALLEL_KEY_INSTRUMENTATION	UINT64CONST(0xE000000000000006)
#define PARALLEL_KEY_DSA				UINT64CONST(0xE000000000000007)
#define PARALLEL_KEY_QUERY_TEXT		UINT64CONST(0xE000000000000008)
#define PARALLEL_KEY_JIT_INSTRUMENTATION UINT64CONST(0xE000000000000009)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xE00000000000000A)

#define PARALLEL_TUPLE_QUEUE_SIZE		65536

/*
 * 需要传给并行 worker 的定长杂项数据。               
 */
typedef struct FixedParallelExecutorState
{
	int64		tuples_needed;	/* 元组边界 见 ExecSetTupleBound */
	dsa_pointer param_exec;
	int			eflags;
	int			jit_flags;
} FixedParallelExecutorState;

/*
 * 用于累积各 PlanState 统计信息的 DSM 结构。      
 *
 * instrument_options: 含义与 instrument.c 中相同。  
 *
 * instrument_offset: 相对本结构起点的偏移，即第一个   
 * Instrumentation 对象所在处的偏移。其取决于 plan_node_id   
 * 数组的长度。     
 *
 * num_workers: worker 数目。  
 *
 * num_plan_nodes: 计划节点数目。
 *
 * plan_node_id: 正在从并行 worker 收集统计信息的计划节点 id   
 * 数组。数组的长度由 num_plan_nodes 给出。                        
 */
struct SharedExecutorInstrumentation
{
	int			instrument_options;
	int			instrument_offset;
	int			num_workers;
	int			num_plan_nodes;
	int			plan_node_id[FLEXIBLE_ARRAY_MEMBER];
	/* 后面是 num_plan_nodes * num_workers 个 Instrumentation 数组     */
};
#define GetInstrumentationArray(sei) \
	(AssertVariableIsOfTypeMacro(sei, SharedExecutorInstrumentation *), \
	 (Instrumentation *) (((char *) sei) + sei->instrument_offset))

/* ExecParallelEstimate 的上下文对象  */
typedef struct ExecParallelEstimateContext
{
	ParallelContext *pcxt;
	int			nnodes;
} ExecParallelEstimateContext;

/* ExecParallelInitializeDSM 的上下文对象  */
typedef struct ExecParallelInitializeDSMContext
{
	ParallelContext *pcxt;
	SharedExecutorInstrumentation *instrumentation;
	int			nnodes;
} ExecParallelInitializeDSMContext;

/* 在并行 leader 中运行的辅助函数。      */
static char *ExecSerializePlan(Plan *plan, EState *estate);
static bool ExecParallelEstimate(PlanState *planstate,
								 ExecParallelEstimateContext *e);
static bool ExecParallelInitializeDSM(PlanState *planstate,
									  ExecParallelInitializeDSMContext *d);
static shm_mq_handle **ExecParallelSetupTupleQueues(ParallelContext *pcxt,
													bool reinitialize);
static bool ExecParallelReInitializeDSM(PlanState *planstate,
										ParallelContext *pcxt);
static bool ExecParallelRetrieveInstrumentation(PlanState *planstate,
												SharedExecutorInstrumentation *instrumentation);

/* 在并行 worker 中运行的辅助函数。      */
static DestReceiver *ExecParallelGetReceiver(dsm_segment *seg, shm_toc *toc);

/*
 * 为要发送给每个 worker 的计划创建序列化表示。           
 */
static char *
ExecSerializePlan(Plan *plan, EState *estate)
{
	PlannedStmt *pstmt;
	ListCell   *lc;

	/* 无法涂改原始计划，因此复制一份。        */
	plan = copyObject(plan);

	/*
	 * worker 会启动自己的执行器副本，若顶层节点含有任何
	 * resjunk 条目，就会插入垃圾过滤器。我们不希望发生 
	 * 这种情况，因为 resjunk 列虽不该回传给用户，但此处
	 * 元组回传给另一个 backend，后者很可能需要它们。
	 * 所以请相应地修改目标列表。                          
	 * 这算是一种 hack；或许有更好的办法可以做到这一     
	 * 点... 
	 */
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		tle->resjunk = false;
	}

	/*
	 * 创建一个哑 PlannedStmt。对其中的大多数字段而言，    
	 * 不要求其有效，但 worker 至少要有一个最小的  
	 * PlannedStmt 来启动执行器    
	 */
	pstmt = makeNode(PlannedStmt);
	pstmt->commandType = CMD_SELECT;
	pstmt->queryId = pgstat_get_my_query_id();
	pstmt->hasReturning = false;
	pstmt->hasModifyingCTE = false;
	pstmt->canSetTag = true;
	pstmt->transientPlan = false;
	pstmt->dependsOnRole = false;
	pstmt->parallelModeNeeded = false;
	pstmt->planTree = plan;
	pstmt->rtable = estate->es_range_table;
	pstmt->permInfos = estate->es_rteperminfos;
	pstmt->resultRelations = NIL;
	pstmt->appendRelations = NIL;

	/*
	 * 只传输 parallel-safe 的子计划，并在列表中为不安全的  
	 * 留下 NULL "空洞"（这样安全子计划的列表下标   
	 * 保持）。这绝对能确保 worker 不会尝试运行不安全的  
	 * 子计划，也不会对它执行 ExecInitNode。这对保护例如
	 * 非并行感知的 FDW 免于陷入麻烦很重要。          
	 */
	pstmt->subplans = NIL;
	foreach(lc, estate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(lc);

		if (subplan && !subplan->parallel_safe)
			subplan = NULL;
		pstmt->subplans = lappend(pstmt->subplans, subplan);
	}

	pstmt->rewindPlanIDs = NULL;
	pstmt->rowMarks = NIL;
	pstmt->relationOids = NIL;
	pstmt->invalItems = NIL;	/* worker 反正无法重新规划*/
	pstmt->paramExecTypes = estate->es_plannedstmt->paramExecTypes;
	pstmt->utilityStmt = NULL;
	pstmt->stmt_location = -1;
	pstmt->stmt_len = -1;

	/* 返回哑 PlannedStmt 的序列化副本。      */
	return nodeToString(pstmt);
}

/*
 * 并行感知的计划节点（偶尔还有其他）可能需要一些  
 * 由所有并行 worker 共享的状态。在确定 DSM 尺寸之前，    
 * 给它们机会调用 shm_toc_estimate_chunk 或 shm_toc_estimate_keys  
 * &pcxt->estimator 
 *
 * 顺便统计树中 PlanState 节点的数量，以便知道需要     
 * 多少个 Instrumentation 结构。                 
 */
static bool
ExecParallelEstimate(PlanState *planstate, ExecParallelEstimateContext *e)
{
	if (planstate == NULL)
		return false;

	/* 统计本节点  */
	e->nnodes++;

	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanEstimate((SeqScanState *) planstate,
									e->pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanEstimate((IndexScanState *) planstate,
									  e->pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanEstimate((IndexOnlyScanState *) planstate,
										  e->pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanEstimate((ForeignScanState *) planstate,
										e->pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendEstimate((AppendState *) planstate,
								   e->pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanEstimate((CustomScanState *) planstate,
									   e->pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapEstimate((BitmapHeapScanState *) planstate,
									   e->pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinEstimate((HashJoinState *) planstate,
									 e->pcxt);
			break;
		case T_HashState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecHashEstimate((HashState *) planstate, e->pcxt);
			break;
		case T_SortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecSortEstimate((SortState *) planstate, e->pcxt);
			break;
		case T_IncrementalSortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecIncrementalSortEstimate((IncrementalSortState *) planstate, e->pcxt);
			break;
		case T_AggState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecAggEstimate((AggState *) planstate, e->pcxt);
			break;
		case T_MemoizeState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecMemoizeEstimate((MemoizeState *) planstate, e->pcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelEstimate, e);
}

/*
 * 估计对指定参数进行序列化所需的空间量。                   
 */
static Size
EstimateParamExecSpace(EState *estate, Bitmapset *params)
{
	int			paramid;
	Size		sz = sizeof(int);

	paramid = -1;
	while ((paramid = bms_next_member(params, paramid)) >= 0)
	{
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;
		ParamExecData *prm;

		prm = &(estate->es_param_exec_vals[paramid]);
		typeOid = list_nth_oid(estate->es_plannedstmt->paramExecTypes,
							   paramid);

		sz = add_size(sz, sizeof(int)); /* paramid 的空间 */

		/* datum/isnull 的空间 */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* 无类型 OID 时像 copyParamList 一样假定按值     */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		sz = add_size(sz,
					  datumEstimateSpace(prm->value, prm->isnull,
										 typByVal, typLen));
	}
	return sz;
}

/*
 * 序列化指定的 PARAM_EXEC 参数。   
 *
 * 我们先把参数个数写成 4 字节整数，然后再依次写入 
 * 出每个参数的详情。每个参数的详情包括一个 4 字节的 
 * paramid（参数在执行期内部参数数组中的位置），随后是
 * 由 datumSerialize() 序列化的 datum。                            
 */
static dsa_pointer
SerializeParamExecParams(EState *estate, Bitmapset *params, dsa_area *area)
{
	Size		size;
	int			nparams;
	int			paramid;
	ParamExecData *prm;
	dsa_pointer handle;
	char	   *start_address;

	/* 为当前参数值分配足够的空间。              */
	size = EstimateParamExecSpace(estate, params);
	handle = dsa_allocate(area, size);
	start_address = dsa_get_address(area, handle);

	/* 先把参数个数写为一个 4 字节整数。          */
	nparams = bms_num_members(params);
	memcpy(start_address, &nparams, sizeof(int));
	start_address += sizeof(int);

	/* 依次写入每个参数的详情。      */
	paramid = -1;
	while ((paramid = bms_next_member(params, paramid)) >= 0)
	{
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		prm = &(estate->es_param_exec_vals[paramid]);
		typeOid = list_nth_oid(estate->es_plannedstmt->paramExecTypes,
							   paramid);

		/* 写入 paramid */
		memcpy(start_address, &paramid, sizeof(int));
		start_address += sizeof(int);

		/* 写入 datum/isnull*/
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* 无类型 OID 时像 copyParamList 一样假定按值     */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		datumSerialize(prm->value, prm->isnull, typByVal, typLen,
					   &start_address);
	}

	return handle;
}

/*
 * 恢复指定的 PARAM_EXEC 参数。    
 */
static void
RestoreParamExecParams(char *start_address, EState *estate)
{
	int			nparams;
	int			i;
	int			paramid;

	memcpy(&nparams, start_address, sizeof(int));
	start_address += sizeof(int);

	for (i = 0; i < nparams; i++)
	{
		ParamExecData *prm;

		/* 读 paramid  */
		memcpy(&paramid, start_address, sizeof(int));
		start_address += sizeof(int);
		prm = &(estate->es_param_exec_vals[paramid]);

		/* 读取 datum/isnull*/
		prm->value = datumRestore(&start_address, &prm->isnull);
		prm->execPlan = NULL;
	}
}

/*
 * 初始化动态共享内存段，它将用于控制                      
 * 并行执行。    
 */
static bool
ExecParallelInitializeDSM(PlanState *planstate,
						  ExecParallelInitializeDSMContext *d)
{
	if (planstate == NULL)
		return false;

	/* 若启用统计信息，则为本节点初始化槽位。     */
	if (d->instrumentation != NULL)
		d->instrumentation->plan_node_id[d->nnodes] =
			planstate->plan->plan_node_id;

	/* 统计本节点  */
	d->nnodes++;

	/*
	 * 为 DSM 计划节点调用初始化器。  
	 *
	 * 多数计划节点在此不做任何事，但分配了 DSM 的计划 
	 * 节点可能需要在并行 worker 启动前初始化 DSM 中的  
	 * 共享状态。它们可以分配先前用 shm_toc_allocate 估算
	 * 的空间，并添加先前用 shm_toc_insert 估算的键，均  
	 * 以 pcxt->toc 为目标。                                       
	 */
	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanInitializeDSM((SeqScanState *) planstate,
										 d->pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanInitializeDSM((IndexScanState *) planstate,
										   d->pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanInitializeDSM((IndexOnlyScanState *) planstate,
											   d->pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanInitializeDSM((ForeignScanState *) planstate,
											 d->pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendInitializeDSM((AppendState *) planstate,
										d->pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanInitializeDSM((CustomScanState *) planstate,
											d->pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapInitializeDSM((BitmapHeapScanState *) planstate,
											d->pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinInitializeDSM((HashJoinState *) planstate,
										  d->pcxt);
			break;
		case T_HashState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecHashInitializeDSM((HashState *) planstate, d->pcxt);
			break;
		case T_SortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecSortInitializeDSM((SortState *) planstate, d->pcxt);
			break;
		case T_IncrementalSortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecIncrementalSortInitializeDSM((IncrementalSortState *) planstate, d->pcxt);
			break;
		case T_AggState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecAggInitializeDSM((AggState *) planstate, d->pcxt);
			break;
		case T_MemoizeState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecMemoizeInitializeDSM((MemoizeState *) planstate, d->pcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelInitializeDSM, d);
}

/*
 * 它为后端 worker 设置响应队列，使其能把元组返回  
 * 给主后端，并启动 worker。        
 */
static shm_mq_handle **
ExecParallelSetupTupleQueues(ParallelContext *pcxt, bool reinitialize)
{
	shm_mq_handle **responseq;
	char	   *tqueuespace;
	int			i;

	/* 若无 worker 则跳过  */
	if (pcxt->nworkers == 0)
		return NULL;

	/* 为共享内存队列句柄分配内存。       */
	responseq = (shm_mq_handle **)
		palloc(pcxt->nworkers * sizeof(shm_mq_handle *));

	/*
	 * 若非重新初始化，则从 DSM 为队列分配空间；       
	 * 否则找到已分配的空间。           
	 */
	if (!reinitialize)
		tqueuespace =
			shm_toc_allocate(pcxt->toc,
							 mul_size(PARALLEL_TUPLE_QUEUE_SIZE,
									  pcxt->nworkers));
	else
		tqueuespace = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_TUPLE_QUEUE, false);

	/* 创建队列，并成为每个队列的接收方。  */
	for (i = 0; i < pcxt->nworkers; ++i)
	{
		shm_mq	   *mq;

		mq = shm_mq_create(tqueuespace +
						   ((Size) i) * PARALLEL_TUPLE_QUEUE_SIZE,
						   (Size) PARALLEL_TUPLE_QUEUE_SIZE);

		shm_mq_set_receiver(mq, MyProc);
		responseq[i] = shm_mq_attach(mq, pcxt->seg, NULL);
	}

	/* 把队列数组加入 shm_toc，供他人查找。     */
	if (!reinitialize)
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLE_QUEUE, tqueuespace);

	/* 返回句柄数组。    */
	return responseq;
}

/*
 * 为后端 worker 搭建执行所需的基础设施，使其能把 
 * 结果返回给主后端。                      
 */
ParallelExecutorInfo *
ExecInitParallelPlan(PlanState *planstate, EState *estate,
					 Bitmapset *sendParams, int nworkers,
					 int64 tuples_needed)
{
	ParallelExecutorInfo *pei;
	ParallelContext *pcxt;
	ExecParallelEstimateContext e;
	ExecParallelInitializeDSMContext d;
	FixedParallelExecutorState *fpes;
	char	   *pstmt_data;
	char	   *pstmt_space;
	char	   *paramlistinfo_space;
	BufferUsage *bufusage_space;
	WalUsage   *walusage_space;
	SharedExecutorInstrumentation *instrumentation = NULL;
	SharedJitInstrumentation *jit_instrumentation = NULL;
	int			pstmt_len;
	int			paramlistinfo_len;
	int			instrumentation_len = 0;
	int			jit_instrumentation_len = 0;
	int			instrument_offset = 0;
	Size		dsa_minsize = dsa_minimum_size();
	char	   *query_string;
	int			query_len;

	/*
	 * 强制将要传给 worker 的任何 initplan 输出先              
	 * 行求值（若尚未求值）。  
	 *
	 * 为简单起见，这里用 EState 的每输出元组 ExprContext。  
	 * 这有查询内内存泄漏的风险，因为我们可能在该        
	 * ExprContext 重置之前多次经过这里；但 ExecSetParamPlan  
	 * 通常不在此上下文泄漏内存（见其注释），所以似乎 
	 * 不值得为了传给它一个生命周期更短的 ExprContext 而
	 * 将 API 复杂化。这也许有朝一日需要改变。      
	 */
	ExecSetParamPlanMulti(sendParams, GetPerTupleExprContext(estate));

	/* 为返回值分配对象。       */
	pei = palloc0(sizeof(ParallelExecutorInfo));
	pei->finished = false;
	pei->planstate = planstate;

	/* 修正并序列化要发给 worker 的计划。  */
	pstmt_data = ExecSerializePlan(planstate->plan, estate);

	/* 创建并行上下文。   */
	pcxt = CreateParallelContext("postgres", "ParallelQueryMain", nworkers);
	pei->pcxt = pcxt;

	/*
	 * 在通知并行上下文创建动态共享内存段之前，需要先
	 * 估算要存储的各类内容所需的                             
	 * 空间大小。                         
	 */

	/* 为定长状态估算空间。       */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   sizeof(FixedParallelExecutorState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 为查询文本估算空间。 */
	query_len = strlen(estate->es_sourceText);
	shm_toc_estimate_chunk(&pcxt->estimator, query_len + 1);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 为序列化的 PlannedStmt 估算空间。*/
	pstmt_len = strlen(pstmt_data) + 1;
	shm_toc_estimate_chunk(&pcxt->estimator, pstmt_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 为序列化的 ParamListInfo 估算空间。*/
	paramlistinfo_len = EstimateParamListSpace(estate->es_param_list_info);
	shm_toc_estimate_chunk(&pcxt->estimator, paramlistinfo_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * 为 BufferUsage 估算空间   
	 *
	 * 如果 EXPLAIN 未在使用，也没有加载会关心它的扩展， 
	 * 本可跳过这一步。但我们无从得知是否有人在看    
	 * pgBufferUsage，因此无条件执行。           
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * WalUsage 同理。      
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 为元组队列估算空间。   */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(PARALLEL_TUPLE_QUEUE_SIZE, pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * 让具备并行感知的节点有机会把估算值加上，并得知
	 * 有多少个 PlanState 节点。            
	 */
	e.pcxt = pcxt;
	e.nnodes = 0;
	ExecParallelEstimate(planstate, &e);

	/* 如有需要，为统计信息估算空间。    */
	if (estate->es_instrument)
	{
		instrumentation_len =
			offsetof(SharedExecutorInstrumentation, plan_node_id) +
			sizeof(int) * e.nnodes;
		instrumentation_len = MAXALIGN(instrumentation_len);
		instrument_offset = instrumentation_len;
		instrumentation_len +=
			mul_size(sizeof(Instrumentation),
					 mul_size(e.nnodes, nworkers));
		shm_toc_estimate_chunk(&pcxt->estimator, instrumentation_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);

		/* 如有需要，为 JIT 统计信息估算空间。   */
		if (estate->es_jit_flags != PGJIT_NONE)
		{
			jit_instrumentation_len =
				offsetof(SharedJitInstrumentation, jit_instr) +
				sizeof(JitInstrumentation) * nworkers;
			shm_toc_estimate_chunk(&pcxt->estimator, jit_instrumentation_len);
			shm_toc_estimate_keys(&pcxt->estimator, 1);
		}
	}

	/* 为 DSA 区域估算空间。*/
	shm_toc_estimate_chunk(&pcxt->estimator, dsa_minsize);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* 各方都已请求过空间，现在创建 DSM。                */
	InitializeParallelDSM(pcxt);

	/*
	 * 好，现在已有了一个动态共享内存段，其容量应当足
	 * 以存放我们估计要放入的全部数据，以及 ParallelContext
	 * 本身要存放的一般性内容（并非专属于执行器）。     
	 * 我们请求的空间还没有任何部分被分配或初始化，
	 * 所以现在就来做这件事。                                   
	 */

	/* 存放定长状态。   */
	fpes = shm_toc_allocate(pcxt->toc, sizeof(FixedParallelExecutorState));
	fpes->tuples_needed = tuples_needed;
	fpes->param_exec = InvalidDsaPointer;
	fpes->eflags = estate->es_top_eflags;
	fpes->jit_flags = estate->es_jit_flags;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_EXECUTOR_FIXED, fpes);

	/* 存放查询串    */
	query_string = shm_toc_allocate(pcxt->toc, query_len + 1);
	memcpy(query_string, estate->es_sourceText, query_len + 1);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, query_string);

	/* 存放序列化的 PlannedStmt*/
	pstmt_space = shm_toc_allocate(pcxt->toc, pstmt_len);
	memcpy(pstmt_space, pstmt_data, pstmt_len);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_PLANNEDSTMT, pstmt_space);

	/* 存放序列化的 ParamListInfo*/
	paramlistinfo_space = shm_toc_allocate(pcxt->toc, paramlistinfo_len);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_PARAMLISTINFO, paramlistinfo_space);
	SerializeParamList(estate->es_param_list_info, &paramlistinfo_space);

	/* 为每个 worker 的 BufferUsage 分配空间；无需初始化。   */
	bufusage_space = shm_toc_allocate(pcxt->toc,
									  mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, bufusage_space);
	pei->buffer_usage = bufusage_space;

	/* WalUsage 同理。 */
	walusage_space = shm_toc_allocate(pcxt->toc,
									  mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, walusage_space);
	pei->wal_usage = walusage_space;

	/* 建立 worker 将要写入的元组队列。              */
	pei->tqueue = ExecParallelSetupTupleQueues(pcxt, false);

	/* 不过暂时还不需要 TupleQueueReaders。    */
	pei->reader = NULL;

	/*
	 * 若提供了统计信息选项，则为数据分配空间。它在此 
	 * 只做部分初始化；其余在 ExecParallelInitializeDSM     
	 * 期间完成。           
	 */
	if (estate->es_instrument)
	{
		Instrumentation *instrument;
		int			i;

		instrumentation = shm_toc_allocate(pcxt->toc, instrumentation_len);
		instrumentation->instrument_options = estate->es_instrument;
		instrumentation->instrument_offset = instrument_offset;
		instrumentation->num_workers = nworkers;
		instrumentation->num_plan_nodes = e.nnodes;
		instrument = GetInstrumentationArray(instrumentation);
		for (i = 0; i < nworkers * e.nnodes; ++i)
			InstrInit(&instrument[i], estate->es_instrument);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_INSTRUMENTATION,
					   instrumentation);
		pei->instrumentation = instrumentation;

		if (estate->es_jit_flags != PGJIT_NONE)
		{
			jit_instrumentation = shm_toc_allocate(pcxt->toc,
												   jit_instrumentation_len);
			jit_instrumentation->num_workers = nworkers;
			memset(jit_instrumentation->jit_instr, 0,
				   sizeof(JitInstrumentation) * nworkers);
			shm_toc_insert(pcxt->toc, PARALLEL_KEY_JIT_INSTRUMENTATION,
						   jit_instrumentation);
			pei->jit_instrumentation = jit_instrumentation;
		}
	}

	/*
	 * 创建一个供 leader 与所有 worker 使用的 DSA 区域。  
	 * （不过，若 DSM 创建失败而改用私有内存，则跳过  
	 * 这一步。）          
	 */
	if (pcxt->seg != NULL)
	{
		char	   *area_space;

		area_space = shm_toc_allocate(pcxt->toc, dsa_minsize);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_DSA, area_space);
		pei->area = dsa_create_in_place(area_space, dsa_minsize,
										LWTRANCHE_PARALLEL_QUERY_DSA,
										pcxt->seg);

		/*
		 * 如有参数，用 DSA 存储序列化。我们不敢把主并行  
		 * 查询 DSM 用于此，因为值改变后可能要重启      
		 * worker（届时所需存储量                               
		 * 也已改变）。            
		 */
		if (!bms_is_empty(sendParams))
		{
			pei->param_exec = SerializeParamExecParams(estate, sendParams,
													   pei->area);
			fpes->param_exec = pei->param_exec;
		}
	}

	/*
	 * 让并行感知的节点有机会初始化它们的共享数据。 
	 * 这也会初始化 instrumentation->ps_instrument 的各元素，    
	 * 若存在。 
	 */
	d.pcxt = pcxt;
	d.instrumentation = instrumentation;
	d.nnodes = 0;

	/* 初始化计划期间安装我们的 DSA 区域。*/
	estate->es_query_dsa = pei->area;
	ExecParallelInitializeDSM(planstate, &d);
	estate->es_query_dsa = NULL;

	/*
	 * 确认世界没有在我们脚下生变。这或许本可以只是 
	 * 一条 Assert()，但目前让我们保守行事。             
	 */
	if (e.nnodes != d.nnodes)
		elog(ERROR, "inconsistent count of PlanState nodes");

	/* 好，已就绪，可以开工。 */
	return pei;
}

/*
 * 设置元组队列读取器，用于读取并行子计划的结果。
 *
 * 它与 ExecInitParallelPlan() 分开，因为我们可以先启动    
 * worker 进程并让它们先开始干活，然后再做这一步。   
 */
void
ExecParallelCreateReaders(ParallelExecutorInfo *pei)
{
	int			nworkers = pei->pcxt->nworkers_launched;
	int			i;

	Assert(pei->reader == NULL);

	if (nworkers > 0)
	{
		pei->reader = (TupleQueueReader **)
			palloc(nworkers * sizeof(TupleQueueReader *));

		for (i = 0; i < nworkers; i++)
		{
			shm_mq_set_handle(pei->tqueue[i],
							  pei->pcxt->worker[i].bgwhandle);
			pei->reader[i] = CreateTupleQueueReader(pei->tqueue[i]);
		}
	}
}

/*
 * 在启动新一批 worker 之前，重新初始化并行执行器的共 
 * 享内存状态。       
 */
void
ExecParallelReinitialize(PlanState *planstate,
						 ParallelExecutorInfo *pei,
						 Bitmapset *sendParams)
{
	EState	   *estate = planstate->state;
	FixedParallelExecutorState *fpes;

	/* 旧 worker 必须已经关闭         */
	Assert(pei->finished);

	/*
	 * 强制将要传给 worker 的任何 initplan 输出先行求值     
	 * （若尚未求值，见                           
	 * ExecInitParallelPlan) 
	 */
	ExecSetParamPlanMulti(sendParams, GetPerTupleExprContext(estate));

	ReinitializeParallelDSM(pei->pcxt);
	pei->tqueue = ExecParallelSetupTupleQueues(pei->pcxt, true);
	pei->reader = NULL;
	pei->finished = false;

	fpes = shm_toc_lookup(pei->pcxt->toc, PARALLEL_KEY_EXECUTOR_FIXED, false);

	/* 释放上一轮遗留的已序列化参数。       */
	if (DsaPointerIsValid(fpes->param_exec))
	{
		dsa_free(pei->area, fpes->param_exec);
		fpes->param_exec = InvalidDsaPointer;
	}

	/* 如有需要，序列化当前参数值。      */
	if (!bms_is_empty(sendParams))
	{
		pei->param_exec = SerializeParamExecParams(estate, sendParams,
												   pei->area);
		fpes->param_exec = pei->param_exec;
	}

	/* 遍历计划树，让每个子节点重置关联的状态。       */
	estate->es_query_dsa = pei->area;
	ExecParallelReInitializeDSM(planstate, pei->pcxt);
	estate->es_query_dsa = NULL;
}

/*
 * 遍历计划树，重新初始化各节点动态共享内存状态     
 */
static bool
ExecParallelReInitializeDSM(PlanState *planstate,
							ParallelContext *pcxt)
{
	if (planstate == NULL)
		return false;

	/*
	 * 为 DSM 计划节点调用重初始化器。 
	 */
	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanReInitializeDSM((SeqScanState *) planstate,
										   pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanReInitializeDSM((IndexScanState *) planstate,
											 pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanReInitializeDSM((IndexOnlyScanState *) planstate,
												 pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanReInitializeDSM((ForeignScanState *) planstate,
											   pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendReInitializeDSM((AppendState *) planstate, pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanReInitializeDSM((CustomScanState *) planstate,
											  pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapReInitializeDSM((BitmapHeapScanState *) planstate,
											  pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinReInitializeDSM((HashJoinState *) planstate,
											pcxt);
			break;
		case T_HashState:
		case T_SortState:
		case T_IncrementalSortState:
		case T_MemoizeState:
			/* 这些节点拥有 DSM 状态，但无需重新初始化        */
			break;

		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelReInitializeDSM, pcxt);
}

/*
 * 从动态共享内存中复制关于本节点及其后代节点的统计 
 * 信息。             
 */
static bool
ExecParallelRetrieveInstrumentation(PlanState *planstate,
									SharedExecutorInstrumentation *instrumentation)
{
	Instrumentation *instrument;
	int			i;
	int			n;
	int			ibytes;
	int			plan_node_id = planstate->plan->plan_node_id;
	MemoryContext oldcontext;

	/* 找到本节点的统计信息。       */
	for (i = 0; i < instrumentation->num_plan_nodes; ++i)
		if (instrumentation->plan_node_id[i] == plan_node_id)
			break;
	if (i >= instrumentation->num_plan_nodes)
		elog(ERROR, "plan node %d not found", plan_node_id);

	/* 从所有 worker 累积统计数据。      */
	instrument = GetInstrumentationArray(instrumentation);
	instrument += i * instrumentation->num_workers;
	for (n = 0; n < instrumentation->num_workers; ++n)
		InstrAggNode(planstate->instrument, &instrument[n]);

	/*
	 * 同时存储各 worker 明细。 
	 *
	 * worker 统计信息应分配在与常规统计信息相同的上下  
	 * 文中，也就是每查询上下文。                             
	 * 切换到每查询内存上下文。 
	 */
	oldcontext = MemoryContextSwitchTo(planstate->state->es_query_cxt);
	ibytes = mul_size(instrumentation->num_workers, sizeof(Instrumentation));
	planstate->worker_instrument =
		palloc(ibytes + offsetof(WorkerInstrumentation, instrument));
	MemoryContextSwitchTo(oldcontext);

	planstate->worker_instrument->num_workers = instrumentation->num_workers;
	memcpy(&planstate->worker_instrument->instrument, instrument, ibytes);

	/* 执行任何需要完成的节点类型专属工作。     */
	switch (nodeTag(planstate))
	{
		case T_SortState:
			ExecSortRetrieveInstrumentation((SortState *) planstate);
			break;
		case T_IncrementalSortState:
			ExecIncrementalSortRetrieveInstrumentation((IncrementalSortState *) planstate);
			break;
		case T_HashState:
			ExecHashRetrieveInstrumentation((HashState *) planstate);
			break;
		case T_AggState:
			ExecAggRetrieveInstrumentation((AggState *) planstate);
			break;
		case T_MemoizeState:
			ExecMemoizeRetrieveInstrumentation((MemoizeState *) planstate);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelRetrieveInstrumentation,
								 instrumentation);
}

/*
 * 从动态共享内存累加各 worker 的 JIT 统计信息。      
 */
static void
ExecParallelRetrieveJitInstrumentation(PlanState *planstate,
									   SharedJitInstrumentation *shared_jit)
{
	JitInstrumentation *combined;
	int			ibytes;

	int			n;

	/*
	 * 把 worker JIT 统计累加进合并的 JIT 统计信息   
	 * （如有需要则分配）。             
	 */
	if (!planstate->state->es_jit_worker_instr)
		planstate->state->es_jit_worker_instr =
			MemoryContextAllocZero(planstate->state->es_query_cxt, sizeof(JitInstrumentation));
	combined = planstate->state->es_jit_worker_instr;

	/* 累积所有 worker 的统计信息。        */
	for (n = 0; n < shared_jit->num_workers; ++n)
		InstrJitAgg(combined, &shared_jit->jit_instr[n]);

	/*
	 * 存储 worker 明细。     
	 *
	 * 与 ExecParallelRetrieveInstrumentation() 类似，在        
	 * 每查询上下文分配。          
	 */
	ibytes = offsetof(SharedJitInstrumentation, jit_instr)
		+ mul_size(shared_jit->num_workers, sizeof(JitInstrumentation));
	planstate->worker_jit_instrument =
		MemoryContextAlloc(planstate->state->es_query_cxt, ibytes);

	memcpy(planstate->worker_jit_instrument, shared_jit, ibytes);
}

/*
 * 结束并行执行。我们等待并行 worker 完成，并累积它们
 * 的 buffer/WAL 使用量。       
 */
void
ExecParallelFinish(ParallelExecutorInfo *pei)
{
	int			nworkers = pei->pcxt->nworkers_launched;
	int			i;

	/* 若连续两次调用，则成为无操作。  */
	if (pei->finished)
		return;

	/*
	 * 尽快脱离元组队列，让任何仍活跃的 worker 能察觉到
	 * 不再需要更多结果。               
	 */
	if (pei->tqueue != NULL)
	{
		for (i = 0; i < nworkers; i++)
			shm_mq_detach(pei->tqueue[i]);
		pfree(pei->tqueue);
		pei->tqueue = NULL;
	}

	/*
	 * 在等待 worker 完成期间，先清掉元组队列读取器。  
	 * （其他任何本地清理工作也可以在这里完成。）        
	 */
	if (pei->reader != NULL)
	{
		for (i = 0; i < nworkers; i++)
			DestroyTupleQueueReader(pei->reader[i]);
		pfree(pei->reader);
		pei->reader = NULL;
	}

	/* 现在等待 worker 完成。       */
	WaitForParallelWorkersToFinish(pei->pcxt);

	/*
	 * 接下来累积 buffer/WAL 使用量。（这一步必须等 worker   
	 * 完成，否则数据可能不完整）  
	 */
	for (i = 0; i < nworkers; i++)
		InstrAccumParallelQuery(&pei->buffer_usage[i], &pei->wal_usage[i]);

	pei->finished = true;
}

/*
 * 累积统计信息，然后清理在 ExecParallelFinish 之后仍然       
 * 存在的 ParallelExecutorInfo 资源。这两个例程是分开  
 * 的，因为有人可能想在 ExecParallelFinish 后、调用本例  
 * 程之前检查 DSM 的内容。                         
 */
void
ExecParallelCleanup(ParallelExecutorInfo *pei)
{
	/* 累积统计信息（如有）。   */
	if (pei->instrumentation)
		ExecParallelRetrieveInstrumentation(pei->planstate,
											pei->instrumentation);

	/* 累积 JIT 统计信息（如有）。  */
	if (pei->jit_instrumentation)
		ExecParallelRetrieveJitInstrumentation(pei->planstate,
											   pei->jit_instrumentation);

	/* 释放已序列化的参数。  */
	if (DsaPointerIsValid(pei->param_exec))
	{
		dsa_free(pei->area, pei->param_exec);
		pei->param_exec = InvalidDsaPointer;
	}
	if (pei->area != NULL)
	{
		dsa_detach(pei->area);
		pei->area = NULL;
	}
	if (pei->pcxt != NULL)
	{
		DestroyParallelContext(pei->pcxt);
		pei->pcxt = NULL;
	}
	pfree(pei);
}

/*
 * 创建一个 DestReceiver，把我们产生的元组写入为此目的   
 * 指定的 shm_mq 
 */
static DestReceiver *
ExecParallelGetReceiver(dsm_segment *seg, shm_toc *toc)
{
	char	   *mqspace;
	shm_mq	   *mq;

	mqspace = shm_toc_lookup(toc, PARALLEL_KEY_TUPLE_QUEUE, false);
	mqspace += ParallelWorkerNumber * PARALLEL_TUPLE_QUEUE_SIZE;
	mq = (shm_mq *) mqspace;
	shm_mq_set_sender(mq, MyProc);
	return CreateTupleQueueDestReceiver(shm_mq_attach(mq, seg, NULL));
}

/*
 * 为我们要执行的 PlannedStmt 创建一个 QueryDesc 并返回它。
 */
static QueryDesc *
ExecParallelGetQueryDesc(shm_toc *toc, DestReceiver *receiver,
						 int instrument_options)
{
	char	   *pstmtspace;
	char	   *paramspace;
	PlannedStmt *pstmt;
	ParamListInfo paramLI;
	char	   *queryString;

	/* 从共享内存取得查询字符串    */
	queryString = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, false);

	/* 重建 leader 提供的 PlannedStmt。   */
	pstmtspace = shm_toc_lookup(toc, PARALLEL_KEY_PLANNEDSTMT, false);
	pstmt = (PlannedStmt *) stringToNode(pstmtspace);

	/* 重建 ParamListInfo。    */
	paramspace = shm_toc_lookup(toc, PARALLEL_KEY_PARAMLISTINFO, false);
	paramLI = RestoreParamList(&paramspace);

	/* 为该查询创建 QueryDesc。   */
	return CreateQueryDesc(pstmt,
						   queryString,
						   GetActiveSnapshot(), InvalidSnapshot,
						   receiver, paramLI, NULL, instrument_options);
}

/*
 * 把本节点及其后代的统计信息复制进动态共享内存，   
 * 以便并行 leader 能取回它们。                             
 */
static bool
ExecParallelReportInstrumentation(PlanState *planstate,
								  SharedExecutorInstrumentation *instrumentation)
{
	int			i;
	int			plan_node_id = planstate->plan->plan_node_id;
	Instrumentation *instrument;

	InstrEndLoop(planstate->instrument);

	/*
	 * 如果我们在 ps_instrument 中把 plan_node_id 值整理成有  
	 * 序的，这里就能用二分查找。若将来我们下推足够大
	 * 的计划树，这可能就有关系。目前先按缓慢笨拙的   
	 * 方式来做。
	 */
	for (i = 0; i < instrumentation->num_plan_nodes; ++i)
		if (instrumentation->plan_node_id[i] == plan_node_id)
			break;
	if (i >= instrumentation->num_plan_nodes)
		elog(ERROR, "plan node %d not found", plan_node_id);

	/*
	 * 把我们的统计加进每节点、每 worker 的合计中。有可 
	 * 能：若重启过 worker，这会发生不止一次。        
	 */
	instrument = GetInstrumentationArray(instrumentation);
	instrument += i * instrumentation->num_workers;
	Assert(IsParallelWorker());
	Assert(ParallelWorkerNumber < instrumentation->num_workers);
	InstrAggNode(&instrument[ParallelWorkerNumber], planstate->instrument);

	return planstate_tree_walker(planstate, ExecParallelReportInstrumentation,
								 instrumentation);
}

/*
 * 用从共享内存取得的信息初始化 PlanState 及后代。
 * 这一步必须在 PlanState 已由执行器分配并初始化之后 
 * 进行；也就是说，要在 ExecutorStart() 之后。                 
 */
static bool
ExecParallelInitializeWorker(PlanState *planstate, ParallelWorkerContext *pwcxt)
{
	if (planstate == NULL)
		return false;

	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanInitializeWorker((SeqScanState *) planstate, pwcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanInitializeWorker((IndexScanState *) planstate,
											  pwcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanInitializeWorker((IndexOnlyScanState *) planstate,
												  pwcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanInitializeWorker((ForeignScanState *) planstate,
												pwcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendInitializeWorker((AppendState *) planstate, pwcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanInitializeWorker((CustomScanState *) planstate,
											   pwcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapInitializeWorker((BitmapHeapScanState *) planstate,
											   pwcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinInitializeWorker((HashJoinState *) planstate,
											 pwcxt);
			break;
		case T_HashState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecHashInitializeWorker((HashState *) planstate, pwcxt);
			break;
		case T_SortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecSortInitializeWorker((SortState *) planstate, pwcxt);
			break;
		case T_IncrementalSortState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecIncrementalSortInitializeWorker((IncrementalSortState *) planstate,
												pwcxt);
			break;
		case T_AggState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecAggInitializeWorker((AggState *) planstate, pwcxt);
			break;
		case T_MemoizeState:
			/* 即便非并行感知也供 EXPLAIN ANALYZE 使用*/
			ExecMemoizeInitializeWorker((MemoizeState *) planstate, pwcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelInitializeWorker,
								 pwcxt);
}

/*
 * 并行查询 worker 进程的主入口。           
 *
 * 我们是从 ParallelWorkerMain 进入本函数的，因此创建一     
 * 个合理的并行环境所需的准备工作都已完成； 
 * ParallelWorkerMain 会处理事务状态、组合 CID 映射以及      
 * GUC 值之类的事情，因此这些我们都无需在此处理。    
 * here.
 *
 * 我们的任务是处理执行器特有的事项。并行组的 leader  
 * 已存放了一个序列化的 PlannedStmt，而我们的任务就是   
 * 执行该计划，并把产生的元组写入相应的队列。       
 * 为了做到这些，我们所需的各种辅助信息同样存放在     
 * dsm_segment 中，并可通过                                           
 * shm_toc。  
 */
void
ParallelQueryMain(dsm_segment *seg, shm_toc *toc)
{
	FixedParallelExecutorState *fpes;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	DestReceiver *receiver;
	QueryDesc  *queryDesc;
	SharedExecutorInstrumentation *instrumentation;
	SharedJitInstrumentation *jit_instrumentation;
	int			instrument_options = 0;
	void	   *area_space;
	dsa_area   *area;
	ParallelWorkerContext pwcxt;

	/* 取得定长状态。 */
	fpes = shm_toc_lookup(toc, PARALLEL_KEY_EXECUTOR_FIXED, false);

	/* 设置 DestReceiver、SharedExecutorInstrumentation 和 QueryDesc  */
	receiver = ExecParallelGetReceiver(seg, toc);
	instrumentation = shm_toc_lookup(toc, PARALLEL_KEY_INSTRUMENTATION, true);
	if (instrumentation != NULL)
		instrument_options = instrumentation->instrument_options;
	jit_instrumentation = shm_toc_lookup(toc, PARALLEL_KEY_JIT_INSTRUMENTATION,
										 true);
	queryDesc = ExecParallelGetQueryDesc(toc, receiver, instrument_options);

	/* 为各个 worker 设置 debug_query_string        */
	debug_query_string = queryDesc->sourceText;

	/* 为监控目的上报 worker 的查询        */
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* 连接到动态共享内存区域。      */
	area_space = shm_toc_lookup(toc, PARALLEL_KEY_DSA, false);
	area = dsa_attach_in_place(area_space, seg);

	/* 启动执行器       */
	queryDesc->plannedstmt->jitFlags = fpes->jit_flags;
	ExecutorStart(queryDesc, fpes->eflags);

	/* 针对并行 worker 的执行器特殊初始化步骤      */
	queryDesc->planstate->state->es_query_dsa = area;
	if (DsaPointerIsValid(fpes->param_exec))
	{
		char	   *paramexec_space;

		paramexec_space = dsa_get_address(area, fpes->param_exec);
		RestoreParamExecParams(paramexec_space, queryDesc->estate);
	}
	pwcxt.toc = toc;
	pwcxt.seg = seg;
	ExecParallelInitializeWorker(queryDesc->planstate, &pwcxt);

	/* 下传任何元组边界  */
	ExecSetTupleBound(fpes->tuples_needed, queryDesc->planstate);

	/*
	 * 准备跟踪查询执行期间的 buffer/WAL 使用。   
	 *
	 * 在启动执行器之后再做这一步，以匹配 leader 行为。  
	 * 后者同样不计执行器启动期间的缓冲区访问与 WAL      
	 * 活动。                     
	 */
	InstrStartParallelQuery();

	/*
	 * 运行计划。若指定了元组边界，要当心不要索要多于 
	 * 该边界的元组    
	 */
	ExecutorRun(queryDesc,
				ForwardScanDirection,
				fpes->tuples_needed < 0 ? (int64) 0 : fpes->tuples_needed,
				true);

	/* 关闭执行器        */
	ExecutorFinish(queryDesc);

	/* 上报并行执行期间的 buffer/WAL 使用量。*/
	buffer_usage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* 若设置任何统计信息选项，则上报统计数据。        */
	if (instrumentation != NULL)
		ExecParallelReportInstrumentation(queryDesc->planstate,
										  instrumentation);

	/* 如有则上报 JIT 统计数据       */
	if (queryDesc->estate->es_jit && jit_instrumentation != NULL)
	{
		Assert(ParallelWorkerNumber < jit_instrumentation->num_workers);
		jit_instrumentation->jit_instr[ParallelWorkerNumber] =
			queryDesc->estate->es_jit->instr;
	}

	/* 必须在捕获统计信息之后做这一步 */
	ExecutorEnd(queryDesc);

	/* 清理。*/
	dsa_detach(area);
	FreeQueryDesc(queryDesc);
	receiver->rDestroy(receiver);
}
