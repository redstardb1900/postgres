/*-------------------------------------------------------------------------
 *
 * execProcnode.c
 *	 包含分派函数，会针对给定节点类型调用相应的     
 *	 "初始化"、"取元组"和"清理"例程。                 
 *	 若节点有子节点，它大概会对自己的子节点调用     
 *	 ExecInitNode、ExecProcNode 或 ExecEndNode，并做适当的      
 *	 处理。  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execProcnode.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 NOTES
 *		本文件过去是三个，现已合并为一个，从而
 *		在新增节点时更容易让各分派例程              
 *		保持同步。                  
 *
 *	 示例 
 *		假设我们想要鞋类部门经理的年龄，以及该部门  
 *		雇员人数。于是我们有了这样的查询：               
 *
 *				select DEPT.no_emps, EMP.age
 *				from DEPT, EMP
 *				where EMP.name = DEPT.mgr and
 *					  DEPT.name = "shoe"
 *
 *		假设规划器给出下面这个计划：      
 *
 *						Nest Loop (DEPT.mgr = EMP.name)
 *						/		\
 *					   /		 \
 *				   Seq Scan		Seq Scan
 *					DEPT		  EMP
 *				(name = "shoe")
 *
 *		先调用 ExecutorStart()。    
 *		它调用 InitPlan()，后者对计划根节点  
 *		调用 ExecInitNode()（嵌套循环）。 
 *
 *	  * ExecInitNode() 注意到自己在处理嵌套循环，于是
 *		如下方代码所示，它会调用 ExecInitNestLoop()。  
 *		最终这会对左右子计划调用 ExecInitNode()，如此层层 
 *		递归下去，直至整个计划初始化完毕。结果是  
 *		ExecInitNode() 构建出一棵与底层计划树结构相同        
 *		的计划状态树。       
 *
 *	  * 随后当 ExecutorRun() 被调用时，它会调用 ExecutePlan()    
 *		后者会在计划状态树的顶层节点上反复调用        
 *		ExecProcNode()。每次如此，它最终都会调用     
 *		ExecNestLoop()，其又对子计划调用 ExecProcNode()   
 *		这些子计划均为顺序扫描，所以 ExecSeqScan() 会   
 *		被调用。ExecSeqScan() 返回的槽中可能包含   
 *		元组，其中含有 ExecNestLoop() 用来构成         
 *		其返回元组的属性。
 *
 *	  * 最终 ExecSeqScan() 停止返回元组，嵌套循环连接
 *		结束。最后，ExecutorEnd() 调用 ExecEndNode()，后者   
 *		接着调用 ExecEndNestLoop()，后者再对子计划调用
 *		ExecEndNode()，最终到 ExecEndSeqScan()。 
 *
 *		这应该说明了执行器如何工作：让    
 *		ExecInitNode()、ExecProcNode() 和 ExecEndNode() 将    
 *		工作分派给合适的节点支撑例程，后者还可能 
 *		反过来在自己的子计划上调用这些例程。   
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapAnd.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeBitmapIndexscan.h"
#include "executor/nodeBitmapOr.h"
#include "executor/nodeCtescan.h"
#include "executor/nodeCustom.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeFunctionscan.h"
#include "executor/nodeGather.h"
#include "executor/nodeGatherMerge.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIncrementalSort.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeLimit.h"
#include "executor/nodeLockRows.h"
#include "executor/nodeMaterial.h"
#include "executor/nodeMemoize.h"
#include "executor/nodeMergeAppend.h"
#include "executor/nodeMergejoin.h"
#include "executor/nodeModifyTable.h"
#include "executor/nodeNamedtuplestorescan.h"
#include "executor/nodeNestloop.h"
#include "executor/nodeProjectSet.h"
#include "executor/nodeRecursiveunion.h"
#include "executor/nodeResult.h"
#include "executor/nodeSamplescan.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSetOp.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/nodeSubqueryscan.h"
#include "executor/nodeTableFuncscan.h"
#include "executor/nodeTidrangescan.h"
#include "executor/nodeTidscan.h"
#include "executor/nodeUnique.h"
#include "executor/nodeValuesscan.h"
#include "executor/nodeWindowAgg.h"
#include "executor/nodeWorktablescan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"

static TupleTableSlot *ExecProcNodeFirst(PlanState *node);
static TupleTableSlot *ExecProcNodeInstr(PlanState *node);
static bool ExecShutdownNode_walker(PlanState *node, void *context);


/* ------------------------------------------------------------------------
 *		ExecInitNode
 *
 *		以递归方式初始化计划树中根为 'node' 的        
 *		节点。 
 *
 *		输入 
 *		  'node' 是查询规划器产出的计划中的当前节点          
 *		  'estate' 是计划树共享的执行状态              
 *		  'eflags' 是 executor.h 中所述各标志位的按位 OR    
 *
 *		返回与给定 Plan 节点相对应的 PlanState 节点。   
 * ------------------------------------------------------------------------
 */
PlanState *
ExecInitNode(Plan *node, EState *estate, int eflags)
{
	PlanState  *result;
	List	   *subps;
	ListCell   *l;

	/*
	 * 到达树中叶节点末尾时什么也不做。    
	 */
	if (node == NULL)
		return NULL;

	/*
	 * 确保有足够的栈可用。除了在 ExecProcNode()（通过 
	 * ExecProcNodeFirst()）之外，还需要在这里检查，以确保
	 * 初始化节点树时栈不会被耗尽。           
	 */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * 控制节点 
			 */
		case T_Result:
			result = (PlanState *) ExecInitResult((Result *) node,
												  estate, eflags);
			break;

		case T_ProjectSet:
			result = (PlanState *) ExecInitProjectSet((ProjectSet *) node,
													  estate, eflags);
			break;

		case T_ModifyTable:
			result = (PlanState *) ExecInitModifyTable((ModifyTable *) node,
													   estate, eflags);
			break;

		case T_Append:
			result = (PlanState *) ExecInitAppend((Append *) node,
												  estate, eflags);
			break;

		case T_MergeAppend:
			result = (PlanState *) ExecInitMergeAppend((MergeAppend *) node,
													   estate, eflags);
			break;

		case T_RecursiveUnion:
			result = (PlanState *) ExecInitRecursiveUnion((RecursiveUnion *) node,
														  estate, eflags);
			break;

		case T_BitmapAnd:
			result = (PlanState *) ExecInitBitmapAnd((BitmapAnd *) node,
													 estate, eflags);
			break;

		case T_BitmapOr:
			result = (PlanState *) ExecInitBitmapOr((BitmapOr *) node,
													estate, eflags);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScan:
			result = (PlanState *) ExecInitSeqScan((SeqScan *) node,
												   estate, eflags);
			break;

		case T_SampleScan:
			result = (PlanState *) ExecInitSampleScan((SampleScan *) node,
													  estate, eflags);
			break;

		case T_IndexScan:
			result = (PlanState *) ExecInitIndexScan((IndexScan *) node,
													 estate, eflags);
			break;

		case T_IndexOnlyScan:
			result = (PlanState *) ExecInitIndexOnlyScan((IndexOnlyScan *) node,
														 estate, eflags);
			break;

		case T_BitmapIndexScan:
			result = (PlanState *) ExecInitBitmapIndexScan((BitmapIndexScan *) node,
														   estate, eflags);
			break;

		case T_BitmapHeapScan:
			result = (PlanState *) ExecInitBitmapHeapScan((BitmapHeapScan *) node,
														  estate, eflags);
			break;

		case T_TidScan:
			result = (PlanState *) ExecInitTidScan((TidScan *) node,
												   estate, eflags);
			break;

		case T_TidRangeScan:
			result = (PlanState *) ExecInitTidRangeScan((TidRangeScan *) node,
														estate, eflags);
			break;

		case T_SubqueryScan:
			result = (PlanState *) ExecInitSubqueryScan((SubqueryScan *) node,
														estate, eflags);
			break;

		case T_FunctionScan:
			result = (PlanState *) ExecInitFunctionScan((FunctionScan *) node,
														estate, eflags);
			break;

		case T_TableFuncScan:
			result = (PlanState *) ExecInitTableFuncScan((TableFuncScan *) node,
														 estate, eflags);
			break;

		case T_ValuesScan:
			result = (PlanState *) ExecInitValuesScan((ValuesScan *) node,
													  estate, eflags);
			break;

		case T_CteScan:
			result = (PlanState *) ExecInitCteScan((CteScan *) node,
												   estate, eflags);
			break;

		case T_NamedTuplestoreScan:
			result = (PlanState *) ExecInitNamedTuplestoreScan((NamedTuplestoreScan *) node,
															   estate, eflags);
			break;

		case T_WorkTableScan:
			result = (PlanState *) ExecInitWorkTableScan((WorkTableScan *) node,
														 estate, eflags);
			break;

		case T_ForeignScan:
			result = (PlanState *) ExecInitForeignScan((ForeignScan *) node,
													   estate, eflags);
			break;

		case T_CustomScan:
			result = (PlanState *) ExecInitCustomScan((CustomScan *) node,
													  estate, eflags);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoop:
			result = (PlanState *) ExecInitNestLoop((NestLoop *) node,
													estate, eflags);
			break;

		case T_MergeJoin:
			result = (PlanState *) ExecInitMergeJoin((MergeJoin *) node,
													 estate, eflags);
			break;

		case T_HashJoin:
			result = (PlanState *) ExecInitHashJoin((HashJoin *) node,
													estate, eflags);
			break;

			/*
			 * 物化节点         
			 */
		case T_Material:
			result = (PlanState *) ExecInitMaterial((Material *) node,
													estate, eflags);
			break;

		case T_Sort:
			result = (PlanState *) ExecInitSort((Sort *) node,
												estate, eflags);
			break;

		case T_IncrementalSort:
			result = (PlanState *) ExecInitIncrementalSort((IncrementalSort *) node,
														   estate, eflags);
			break;

		case T_Memoize:
			result = (PlanState *) ExecInitMemoize((Memoize *) node, estate,
												   eflags);
			break;

		case T_Group:
			result = (PlanState *) ExecInitGroup((Group *) node,
												 estate, eflags);
			break;

		case T_Agg:
			result = (PlanState *) ExecInitAgg((Agg *) node,
											   estate, eflags);
			break;

		case T_WindowAgg:
			result = (PlanState *) ExecInitWindowAgg((WindowAgg *) node,
													 estate, eflags);
			break;

		case T_Unique:
			result = (PlanState *) ExecInitUnique((Unique *) node,
												  estate, eflags);
			break;

		case T_Gather:
			result = (PlanState *) ExecInitGather((Gather *) node,
												  estate, eflags);
			break;

		case T_GatherMerge:
			result = (PlanState *) ExecInitGatherMerge((GatherMerge *) node,
													   estate, eflags);
			break;

		case T_Hash:
			result = (PlanState *) ExecInitHash((Hash *) node,
												estate, eflags);
			break;

		case T_SetOp:
			result = (PlanState *) ExecInitSetOp((SetOp *) node,
												 estate, eflags);
			break;

		case T_LockRows:
			result = (PlanState *) ExecInitLockRows((LockRows *) node,
													estate, eflags);
			break;

		case T_Limit:
			result = (PlanState *) ExecInitLimit((Limit *) node,
												 estate, eflags);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;		/* 让编译器安静  */
			break;
	}

	ExecSetExecProcNode(result, result->ExecProcNode);

	/*
	 * 初始化本节点里存在的任何 initPlan。规划器把它们     
	 * 放入单独列表。  
	 */
	subps = NIL;
	foreach(l, node->initPlan)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(l);
		SubPlanState *sstate;

		Assert(IsA(subplan, SubPlan));
		sstate = ExecInitSubPlan(subplan, result);
		subps = lappend(subps, sstate);
	}
	result->initPlan = subps;

	/* 若被请求，为本节点设置 instrumentation */
	if (estate->es_instrument)
		result->instrument = InstrAlloc(1, estate->es_instrument,
										result->async_capable);

	return result;
}


/*
 * 如果节点想在 ExecInitNode() 结束之后更改自己的           
 * ExecProcNode 函数，就应该使用此函数来做。这样任何    
 * 包装函数都能被重新安装，而节点无需知道其工作    
 * 原理
 */
void
ExecSetExecProcNode(PlanState *node, ExecProcNodeMtd function)
{
	/*
	 * 在 ExecProcNode 回调外再包一层包装，用以检查栈深度  
	 * 这发生在首次执行期间，并可能添加 instrumentation     
	 * 包装。若执行已开始后才改回调，那意味着我们会 
	 * 多余地执行一次 ExecProcNodeFirst，但这似乎无妨。        
	 */
	node->ExecProcNodeReal = function;
	node->ExecProcNode = ExecProcNodeFirst;
}


/*
 * ExecProcNode 包装器，会在调用相关节点方法之前执行    
 * 一些一次性检查（可能经由 instrumentation 包装器）  
 */
static TupleTableSlot *
ExecProcNodeFirst(PlanState *node)
{
	/*
	 * 在该节点首次执行期间执行栈深度检查。我们         
	 * 只在第一轮这么做，因为事实证明这在某些常见体系  
	 * 结构（例如 x86）上并不便宜。这有赖于如下假设：
	 * 即给定计划节点的各次 ExecProcNode 调用总是发生在   
	 * 大致相同的栈深度。  
	 */
	check_stack_depth();

	/*
	 * 若需要 instrumentation，就把包装换成只做统计的那一
	 * 种。否则我们可以省掉所有包装，让 ExecProcNode() 从   
	 * 此以后直接调用相关函数。                                
	 */
	if (node->instrument)
		node->ExecProcNode = ExecProcNodeInstr;
	else
		node->ExecProcNode = node->ExecProcNodeReal;

	return node->ExecProcNode(node);
}


/*
 * 负责执行 instrumentation 调用的 ExecProcNode 包装器。     
 * 把它保留为单独函数，可避免在不需要统计信息的  
 * 常规情形下的开销。  
 */
static TupleTableSlot *
ExecProcNodeInstr(PlanState *node)
{
	TupleTableSlot *result;

	InstrStartNode(node->instrument);

	result = node->ExecProcNodeReal(node);

	InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}


/* ----------------------------------------------------------------
 *		MultiExecProcNode
 *
 *		执行一个不返回单个元组的节点          
 *		（它可能返回哈希表、位图等）。调用方应 
 *		检查所得类型是否符合预期。     
 *
 * 它的职责本质上与 ExecProcNode 相同，但它不做    
 * InstrStartNode/InstrStopNode（主要是因为它无法        
 * 判断该把多少返回元组计入统计）。每个节点级 
 * 函数都必须自行提供统计支持。            
 * ----------------------------------------------------------------
 */
Node *
MultiExecProcNode(PlanState *node)
{
	Node	   *result;

	check_stack_depth();

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* 发生了变化   */
		ExecReScan(node);		/* 交给 ReScan 处理   */

	switch (nodeTag(node))
	{
			/*
			 * 只有实际支持 multiexec 的节点类型才会被列出   
			 */

		case T_HashState:
			result = MultiExecHash((HashState *) node);
			break;

		case T_BitmapIndexScanState:
			result = MultiExecBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapAndState:
			result = MultiExecBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			result = MultiExecBitmapOr((BitmapOrState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			result = NULL;
			break;
	}

	return result;
}


/* ----------------------------------------------------------------
 *		ExecEndNode
 *
 *		以递归方式清理计划树中根为 'node' 的    
 *		节点。 
 *
 *		此操作之后，查询计划将无法再被进一步     
 *		处理。应当只在查询计划已被完整           
 *		执行之后才可调用。            
 * ----------------------------------------------------------------
 */
void
ExecEndNode(PlanState *node)
{
	/*
	 * 到达树中叶节点末尾时什么也不做。    
	 */
	if (node == NULL)
		return;

	/*
	 * 确保有足够的栈可用。除了在 ExecProcNode()（经由 
	 * ExecProcNodeFirst()）所做的检查外，这里也要检查，因为
	 * 并非所有节点都保证会到达 ExecProcNode()。  
	 */
	check_stack_depth();

	if (node->chgParam != NULL)
	{
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}

	switch (nodeTag(node))
	{
			/*
			 * 控制节点 
			 */
		case T_ResultState:
			ExecEndResult((ResultState *) node);
			break;

		case T_ProjectSetState:
			ExecEndProjectSet((ProjectSetState *) node);
			break;

		case T_ModifyTableState:
			ExecEndModifyTable((ModifyTableState *) node);
			break;

		case T_AppendState:
			ExecEndAppend((AppendState *) node);
			break;

		case T_MergeAppendState:
			ExecEndMergeAppend((MergeAppendState *) node);
			break;

		case T_RecursiveUnionState:
			ExecEndRecursiveUnion((RecursiveUnionState *) node);
			break;

		case T_BitmapAndState:
			ExecEndBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			ExecEndBitmapOr((BitmapOrState *) node);
			break;

			/*
			 * scan nodes
			 */
		case T_SeqScanState:
			ExecEndSeqScan((SeqScanState *) node);
			break;

		case T_SampleScanState:
			ExecEndSampleScan((SampleScanState *) node);
			break;

		case T_GatherState:
			ExecEndGather((GatherState *) node);
			break;

		case T_GatherMergeState:
			ExecEndGatherMerge((GatherMergeState *) node);
			break;

		case T_IndexScanState:
			ExecEndIndexScan((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecEndIndexOnlyScan((IndexOnlyScanState *) node);
			break;

		case T_BitmapIndexScanState:
			ExecEndBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecEndBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			ExecEndTidScan((TidScanState *) node);
			break;

		case T_TidRangeScanState:
			ExecEndTidRangeScan((TidRangeScanState *) node);
			break;

		case T_SubqueryScanState:
			ExecEndSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			ExecEndFunctionScan((FunctionScanState *) node);
			break;

		case T_TableFuncScanState:
			ExecEndTableFuncScan((TableFuncScanState *) node);
			break;

		case T_ValuesScanState:
			ExecEndValuesScan((ValuesScanState *) node);
			break;

		case T_CteScanState:
			ExecEndCteScan((CteScanState *) node);
			break;

		case T_NamedTuplestoreScanState:
			ExecEndNamedTuplestoreScan((NamedTuplestoreScanState *) node);
			break;

		case T_WorkTableScanState:
			ExecEndWorkTableScan((WorkTableScanState *) node);
			break;

		case T_ForeignScanState:
			ExecEndForeignScan((ForeignScanState *) node);
			break;

		case T_CustomScanState:
			ExecEndCustomScan((CustomScanState *) node);
			break;

			/*
			 * join nodes
			 */
		case T_NestLoopState:
			ExecEndNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			ExecEndMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			ExecEndHashJoin((HashJoinState *) node);
			break;

			/*
			 * 物化节点         
			 */
		case T_MaterialState:
			ExecEndMaterial((MaterialState *) node);
			break;

		case T_SortState:
			ExecEndSort((SortState *) node);
			break;

		case T_IncrementalSortState:
			ExecEndIncrementalSort((IncrementalSortState *) node);
			break;

		case T_MemoizeState:
			ExecEndMemoize((MemoizeState *) node);
			break;

		case T_GroupState:
			ExecEndGroup((GroupState *) node);
			break;

		case T_AggState:
			ExecEndAgg((AggState *) node);
			break;

		case T_WindowAggState:
			ExecEndWindowAgg((WindowAggState *) node);
			break;

		case T_UniqueState:
			ExecEndUnique((UniqueState *) node);
			break;

		case T_HashState:
			ExecEndHash((HashState *) node);
			break;

		case T_SetOpState:
			ExecEndSetOp((SetOpState *) node);
			break;

		case T_LockRowsState:
			ExecEndLockRows((LockRowsState *) node);
			break;

		case T_LimitState:
			ExecEndLimit((LimitState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecShutdownNode
 *
 * 给执行节点一个机会，使其停止异步资源消耗，        
 * 并释放仍持有的任何资源。 
 */
void
ExecShutdownNode(PlanState *node)
{
	(void) ExecShutdownNode_walker(node, NULL);
}

static bool
ExecShutdownNode_walker(PlanState *node, void *context)
{
	if (node == NULL)
		return false;

	check_stack_depth();

	/*
	 * 在关闭节点期间把它当作正在运行，但前提是它已经
	 * 至少运行过一次。我们不指望节点关闭期间消耗    
	 * 多少 CPU，但在 Gather 或 Gather Merge 的情形下，我们    
	 * 可能在此阶段关闭 worker。这样它们的缓冲用量  
	 * 将在此时传播进 pgBufferUsage，我们要确保它被关联到 
	 * Gather 节点上。若节点从未执行，我们就跳过这步，    
	 * 以免错误地让它显得好像已经执行过，而实际        
	 * 并非如此
	 */
	if (node->instrument && node->instrument->running)
		InstrStartNode(node->instrument);

	planstate_tree_walker(node, ExecShutdownNode_walker, context);

	switch (nodeTag(node))
	{
		case T_GatherState:
			ExecShutdownGather((GatherState *) node);
			break;
		case T_ForeignScanState:
			ExecShutdownForeignScan((ForeignScanState *) node);
			break;
		case T_CustomScanState:
			ExecShutdownCustomScan((CustomScanState *) node);
			break;
		case T_GatherMergeState:
			ExecShutdownGatherMerge((GatherMergeState *) node);
			break;
		case T_HashState:
			ExecShutdownHash((HashState *) node);
			break;
		case T_HashJoinState:
			ExecShutdownHashJoin((HashJoinState *) node);
			break;
		default:
			break;
	}

	/* 若上面启动过该节点，停止它并报告 0 元组 */
	if (node->instrument && node->instrument->running)
		InstrStopNode(node->instrument, 0);

	return false;
}

/*
 * ExecSetTupleBound
 *
 * 为一个 planstate 节点设置元组上限。这样各子计划  
 * 节点能够基于如下认识进行优化：其父节点将索取的 
 * 元组数量存在上限。节点的元组上限只能在扫描     
 * 之间修改（即节点初始化之后，或恰在 ExecReScan 调    
 * 用之前进行）。      
 *
 * 任何负的 tuples_needed 值都表示"无限制"，当某个节点  
 * 完全未被调用时，这就是应当采用的默认假设。         
 *
 * 注意：若在一个计划树上反复调用本函数，每次必须
 * 用新的上限更新完全相同的节点集合；要小心这里    
 * 只检测不会变化的条件。          
 */
void
ExecSetTupleBound(int64 tuples_needed, PlanState *child_node)
{
	/*
	 * 由于本函数会递归，原则上应当在此检查栈深度。    
	 * 但这多半没有意义，因为更早的节点初始化树      
	 * 遍历肯定已经消耗了更多的栈空间。                    
	 */

	if (IsA(child_node, SortState))
	{
		/*
		 * 如果这是 Sort 节点，通知它可以有界排序。    
		 *
		 * 注意：对 nodeSort.c 而言，正确响应这些参数的     
		 * 变化是它的责任。如果将来重新设计此处，        
		 * 那么最好将这一信号机制与参数变更机制集成   
		 * 起来。 
		 */
		SortState  *sortState = (SortState *) child_node;

		if (tuples_needed < 0)
		{
			/* 确保重扫描时按需重置该标志         */
			sortState->bounded = false;
		}
		else
		{
			sortState->bounded = true;
			sortState->bound = tuples_needed;
		}
	}
	else if (IsA(child_node, IncrementalSortState))
	{
		/*
		 * 如果这是 IncrementalSort 节点，通知它可以有界排序  
		 *      
		 *
		 * 注意：nodeIncrementalSort.c 负责正确响应这些         
		 * 参数的变化。如果将来重新设计此处，最好把这一 
		 * 信号机制与参数变更                             
		 * 机制集成起来。      
		 */
		IncrementalSortState *sortState = (IncrementalSortState *) child_node;

		if (tuples_needed < 0)
		{
			/* 确保重扫描时按需重置该标志         */
			sortState->bounded = false;
		}
		else
		{
			sortState->bounded = true;
			sortState->bound = tuples_needed;
		}
	}
	else if (IsA(child_node, AppendState))
	{
		/*
		 * 如果这是 Append，可把上限施加到作为其子节点的
		 * 那些节点上，因为 Append 必然不需要从任何单个   
		 * 输入读取多于该数量的元组。  
		 */
		AppendState *aState = (AppendState *) child_node;
		int			i;

		for (i = 0; i < aState->as_nplans; i++)
			ExecSetTupleBound(tuples_needed, aState->appendplans[i]);
	}
	else if (IsA(child_node, MergeAppendState))
	{
		/*
		 * 如果这是 MergeAppend，可把上限施加到作为其子节 
		 * 点的那些节点上，因为 MergeAppend 必然不需要从任  
		 * 何单个输入读取多于该数量的元组。      
		 */
		MergeAppendState *maState = (MergeAppendState *) child_node;
		int			i;

		for (i = 0; i < maState->ms_nplans; i++)
			ExecSetTupleBound(tuples_needed, maState->mergeplans[i]);
	}
	else if (IsA(child_node, ResultState))
	{
		/*
		 * 类似地，对于投影 Result，可以把上限施加到其子 
		 * 节点。  
		 *
		 * 如果 Result 支持 qual 检查，遇到 qual 时只能回避  
		 * 注意：拥有 resconstantqual 并非致命问题：如果该    
		 * 条件成立则无影响；若不成立，反正也不会        
		 * 从 Result 子节点索取任何行。             
		 */
		if (outerPlanState(child_node))
			ExecSetTupleBound(tuples_needed, outerPlanState(child_node));
	}
	else if (IsA(child_node, SubqueryScanState))
	{
		/*
		 * 我们也可以穿过 SubqueryScan 继续向下，但仅当它 
		 * 没有 qual（否则可能丢行）。 
		 */
		SubqueryScanState *subqueryState = (SubqueryScanState *) child_node;

		if (subqueryState->ss.ps.qual == NULL)
			ExecSetTupleBound(tuples_needed, subqueryState->subplan);
	}
	else if (IsA(child_node, GatherState))
	{
		/*
		 * Gather 节点可以把上限传播给它的 worker。与      
		 * MergeAppend 一样，单个 worker 所需返回的元组     
		 * 都不会多于 Gather 自身所需。 
		 *
		 * 注意：与 Sort 一样，Gather 节点负责正确响应     
		 * 此参数的变化。                 
		 */
		GatherState *gstate = (GatherState *) child_node;

		gstate->tuples_needed = tuples_needed;

		/* 也把上限下传给我们自己那份子计划副本     */
		ExecSetTupleBound(tuples_needed, outerPlanState(child_node));
	}
	else if (IsA(child_node, GatherMergeState))
	{
		/* 注释与 Gather 相同     */
		GatherMergeState *gstate = (GatherMergeState *) child_node;

		gstate->tuples_needed = tuples_needed;

		ExecSetTupleBound(tuples_needed, outerPlanState(child_node));
	}

	/*
	 * 原则上，我们可以穿过任何确定不丢弃、不合并 
	 * 输入行的计划节点类型；但一旦遇到会这样做的节     
	 * 点，我们就无法再继续传播上限。目前尚不清楚       
	 * 是否还有其他情形值得在此检查。             
	 */
}
