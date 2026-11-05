/*-------------------------------------------------------------------------
 *
 * execAmi.c
 *	  杂项执行器访问方法例程            
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/executor/execAmi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "executor/execdebug.h"
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
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static bool IndexSupportsBackwardScan(Oid indexid);


/*
 * ExecReScan
 *		重置计划节点，使其输出可被重新扫描。 
 *
 * 注意，如果计划节点具有值已发生变化的参数，   
 * 其输出可能与上次不同。            
 */
void
ExecReScan(PlanState *node)
{
	/* 若在收集时间统计则更新它们 */
	if (node->instrument)
		InstrEndLoop(node->instrument);

	/*
	 * 若有已变化的参数，则传播该信息。   
	 *
	 * 注意：ExecReScanSetParamPlan() 可以向 node->chgParam 中
	 * 添加一些位，对应于该 InitPlan 将更新的输出参数。
	 * 由于我们对列表只做一趟遍历，这意味着一个 InitPlan 
	 * 只有当兄弟 InitPlan 在列表中更靠前时，它才能依赖 
	 * 该兄弟的输出参数。目前这样是可行的，因为一个  
	 * InitPlan 能依赖另一个 InitPlan 的方式有限，但最终   
	 * 我们可能需要更加努力（或者让规划器扩大        
	 * extParam/allParam 集合，把被依赖 InitPlans 的参数也         
	 * 纳入）  
	 */
	if (node->chgParam != NULL)
	{
		ListCell   *l;

		foreach(l, node->initPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			PlanState  *splan = sstate->planstate;

			if (splan->plan->extParam != NULL)	/* don't care about child
												 * local Params */
				UpdateChangedParamSet(splan, node->chgParam);
			if (splan->chgParam != NULL)
				ExecReScanSetParamPlan(sstate, node);
		}
		foreach(l, node->subPlan)
		{
			SubPlanState *sstate = (SubPlanState *) lfirst(l);
			PlanState  *splan = sstate->planstate;

			if (splan->plan->extParam != NULL)
				UpdateChangedParamSet(splan, node->chgParam);
		}
		/* 好，现在为子树设置 chgParam。 */
		if (outerPlanState(node) != NULL)
			UpdateChangedParamSet(outerPlanState(node), node->chgParam);
		if (innerPlanState(node) != NULL)
			UpdateChangedParamSet(innerPlanState(node), node->chgParam);
	}

	/* 调用表达式回调     */
	if (node->ps_ExprContext)
		ReScanExprContext(node->ps_ExprContext);

	/* 再执行节点类型专属处理    */
	switch (nodeTag(node))
	{
		case T_ResultState:
			ExecReScanResult((ResultState *) node);
			break;

		case T_ProjectSetState:
			ExecReScanProjectSet((ProjectSetState *) node);
			break;

		case T_ModifyTableState:
			ExecReScanModifyTable((ModifyTableState *) node);
			break;

		case T_AppendState:
			ExecReScanAppend((AppendState *) node);
			break;

		case T_MergeAppendState:
			ExecReScanMergeAppend((MergeAppendState *) node);
			break;

		case T_RecursiveUnionState:
			ExecReScanRecursiveUnion((RecursiveUnionState *) node);
			break;

		case T_BitmapAndState:
			ExecReScanBitmapAnd((BitmapAndState *) node);
			break;

		case T_BitmapOrState:
			ExecReScanBitmapOr((BitmapOrState *) node);
			break;

		case T_SeqScanState:
			ExecReScanSeqScan((SeqScanState *) node);
			break;

		case T_SampleScanState:
			ExecReScanSampleScan((SampleScanState *) node);
			break;

		case T_GatherState:
			ExecReScanGather((GatherState *) node);
			break;

		case T_GatherMergeState:
			ExecReScanGatherMerge((GatherMergeState *) node);
			break;

		case T_IndexScanState:
			ExecReScanIndexScan((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecReScanIndexOnlyScan((IndexOnlyScanState *) node);
			break;

		case T_BitmapIndexScanState:
			ExecReScanBitmapIndexScan((BitmapIndexScanState *) node);
			break;

		case T_BitmapHeapScanState:
			ExecReScanBitmapHeapScan((BitmapHeapScanState *) node);
			break;

		case T_TidScanState:
			ExecReScanTidScan((TidScanState *) node);
			break;

		case T_TidRangeScanState:
			ExecReScanTidRangeScan((TidRangeScanState *) node);
			break;

		case T_SubqueryScanState:
			ExecReScanSubqueryScan((SubqueryScanState *) node);
			break;

		case T_FunctionScanState:
			ExecReScanFunctionScan((FunctionScanState *) node);
			break;

		case T_TableFuncScanState:
			ExecReScanTableFuncScan((TableFuncScanState *) node);
			break;

		case T_ValuesScanState:
			ExecReScanValuesScan((ValuesScanState *) node);
			break;

		case T_CteScanState:
			ExecReScanCteScan((CteScanState *) node);
			break;

		case T_NamedTuplestoreScanState:
			ExecReScanNamedTuplestoreScan((NamedTuplestoreScanState *) node);
			break;

		case T_WorkTableScanState:
			ExecReScanWorkTableScan((WorkTableScanState *) node);
			break;

		case T_ForeignScanState:
			ExecReScanForeignScan((ForeignScanState *) node);
			break;

		case T_CustomScanState:
			ExecReScanCustomScan((CustomScanState *) node);
			break;

		case T_NestLoopState:
			ExecReScanNestLoop((NestLoopState *) node);
			break;

		case T_MergeJoinState:
			ExecReScanMergeJoin((MergeJoinState *) node);
			break;

		case T_HashJoinState:
			ExecReScanHashJoin((HashJoinState *) node);
			break;

		case T_MaterialState:
			ExecReScanMaterial((MaterialState *) node);
			break;

		case T_MemoizeState:
			ExecReScanMemoize((MemoizeState *) node);
			break;

		case T_SortState:
			ExecReScanSort((SortState *) node);
			break;

		case T_IncrementalSortState:
			ExecReScanIncrementalSort((IncrementalSortState *) node);
			break;

		case T_GroupState:
			ExecReScanGroup((GroupState *) node);
			break;

		case T_AggState:
			ExecReScanAgg((AggState *) node);
			break;

		case T_WindowAggState:
			ExecReScanWindowAgg((WindowAggState *) node);
			break;

		case T_UniqueState:
			ExecReScanUnique((UniqueState *) node);
			break;

		case T_HashState:
			ExecReScanHash((HashState *) node);
			break;

		case T_SetOpState:
			ExecReScanSetOp((SetOpState *) node);
			break;

		case T_LockRowsState:
			ExecReScanLockRows((LockRowsState *) node);
			break;

		case T_LimitState:
			ExecReScanLimit((LimitState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}

	if (node->chgParam != NULL)
	{
		bms_free(node->chgParam);
		node->chgParam = NULL;
	}
}

/*
 * ExecMarkPos
 *
 * 标记当前扫描位置。     
 *
 * 注意：mark/restore 能力目前只需要用于                    
 * 那些作为 MergeJoin 直接内层子节点的计划类型。          
 * 由于 MergeJoin 需要有序输入，无法产生有序输出的节点  
 * 类型永远不需要支持 mark/restore。在某些情况下，      
 * 节点可以传递来自子节点的有序数据；如果我们      
 * 不为这类节点类型实现 mark/restore，规划器会代之以    
 * 在其上方插入 Material 节点。     
 */
void
ExecMarkPos(PlanState *node)
{
	switch (nodeTag(node))
	{
		case T_IndexScanState:
			ExecIndexMarkPos((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecIndexOnlyMarkPos((IndexOnlyScanState *) node);
			break;

		case T_CustomScanState:
			ExecCustomMarkPos((CustomScanState *) node);
			break;

		case T_MaterialState:
			ExecMaterialMarkPos((MaterialState *) node);
			break;

		case T_SortState:
			ExecSortMarkPos((SortState *) node);
			break;

		case T_ResultState:
			ExecResultMarkPos((ResultState *) node);
			break;

		default:
			/* 调用者未要求恢复时不要硬性报错...       */
			elog(DEBUG2, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecRestrPos
 *
 * 恢复先前用 ExecMarkPos() 保存的扫描位置           
 *
 * 注意：这里的语义是，restore 之后的第一次             
 * ExecProcNode 将产生与 mark 操作之后的第一次相同             
 * 的元组。计划节点的结果槽会发生什么未作规定。    
 * （大多数情况下 restore 不改变结果槽，但节点可选择 
 * 清空它，或者用它装入恢复到的那个元组。）         
 * 因此调用者应在做过 restore 后丢弃任何先前         
 * 返回的 TupleTableSlot。                   
 */
void
ExecRestrPos(PlanState *node)
{
	switch (nodeTag(node))
	{
		case T_IndexScanState:
			ExecIndexRestrPos((IndexScanState *) node);
			break;

		case T_IndexOnlyScanState:
			ExecIndexOnlyRestrPos((IndexOnlyScanState *) node);
			break;

		case T_CustomScanState:
			ExecCustomRestrPos((CustomScanState *) node);
			break;

		case T_MaterialState:
			ExecMaterialRestrPos((MaterialState *) node);
			break;

		case T_SortState:
			ExecSortRestrPos((SortState *) node);
			break;

		case T_ResultState:
			ExecResultRestrPos((ResultState *) node);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * ExecSupportsMarkRestore - Path 是否支持 mark/restore？
 *
 * 此函数在规划期间使用，必须接受 Path 而非 Plan。  
 * 我们把它放在这里以紧邻上述例程，它们也必须      
 * 知道哪些计划类型支持 mark/restore
 */
bool
ExecSupportsMarkRestore(Path *pathnode)
{
	/*
	 * 为与上述例程保持一致，我们不检查 nodeTag，           
	 * 而是检查 pathtype，即该 Path 将要产生的 Plan 节点    
	 * 类型  
	 */
	switch (pathnode->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:

			/*
			 * 索引类型并不都支持 mark/restore 
			 */
			return castNode(IndexPath, pathnode)->indexinfo->amcanmarkpos;

		case T_Material:
		case T_Sort:
			return true;

		case T_CustomScan:
			if (castNode(CustomPath, pathnode)->flags & CUSTOMPATH_SUPPORT_MARK_RESTORE)
				return true;
			return false;

		case T_Result:

			/*
			 * 当且仅当 Result 有支持 mark/restore 的子计划。     
			 *
			 * 这里必须小心，因为能够产生 Result 计划节点的
			 * Path 类型不止一种。               
			 */
			if (IsA(pathnode, ProjectionPath))
				return ExecSupportsMarkRestore(((ProjectionPath *) pathnode)->subpath);
			else if (IsA(pathnode, MinMaxAggPath))
				return false;	/* 无子的 Result */
			else if (IsA(pathnode, GroupResultPath))
				return false;	/* 无子的 Result */
			else
			{
				/* 简单 RTE_RESULT 基础关系  */
				Assert(IsA(pathnode, Path));
				return false;	/* 无子的 Result */
			}

		case T_Append:
			{
				AppendPath *appendPath = castNode(AppendPath, pathnode);

				/*
				 * 如果恰好只有一个子节点，那么最终计划中 
				 * 将不会有 Append，mark/restore 能力取决于     
				 * 子节点。        
				 */
				if (list_length(appendPath->subpaths) == 1)
					return ExecSupportsMarkRestore((Path *) linitial(appendPath->subpaths));
				/* 否则 Append 无法处理它     */
				return false;
			}

		case T_MergeAppend:
			{
				MergeAppendPath *mapath = castNode(MergeAppendPath, pathnode);

				/*
				 * 与上述 Append 相同，单子路径的 MergeAppend  
				 * 不会进入最终计划，故直接返回子节点的
				 * mark/restore。      
				 */
				if (list_length(mapath->subpaths) == 1)
					return ExecSupportsMarkRestore((Path *) linitial(mapath->subpaths));
				/* 否则 MergeAppend 无法处理它     */
				return false;
			}

		default:
			break;
	}

	return false;
}

/*
 * ExecSupportsBackwardScan - 计划类型是否支持反向扫描？     
 *
 * 理想情况下，所有计划类型都支持反向扫描，但这  
 * 似乎不太可能很快实现。在某些情况下，计划节点会把 
 * 反向扫描下推给子节点，因此只有当其子节点支持   
 * 反向扫描时才支持。因此必须向例程传入完整计划树。  
 */
bool
ExecSupportsBackwardScan(Plan *node)
{
	if (node == NULL)
		return false;

	/*
	 * 并行感知节点在每个 worker 中返回元组的一个子集，  
	 * 通常我们无法指望拥有足够的簿记状态来知道       
	 * 自己在这个 worker（而非其他 worker）中返回了哪些。  
	 */
	if (node->parallel_aware)
		return false;

	switch (nodeTag(node))
	{
		case T_Result:
			if (outerPlan(node) != NULL)
				return ExecSupportsBackwardScan(outerPlan(node));
			else
				return false;

		case T_Append:
			{
				ListCell   *l;

				/* 有 async 时元组可能交错，因此无法倒回。  */
				if (((Append *) node)->nasyncplans > 0)
					return false;

				foreach(l, ((Append *) node)->appendplans)
				{
					if (!ExecSupportsBackwardScan((Plan *) lfirst(l)))
						return false;
				}
				/* 无需检查 tlist，因为 Append 不会对它求值   */
				return true;
			}

		case T_SampleScan:
			/* 通过禁止此功能让 tablesample 方法更简单      */
			return false;

		case T_Gather:
			return false;

		case T_IndexScan:
			return IndexSupportsBackwardScan(((IndexScan *) node)->indexid);

		case T_IndexOnlyScan:
			return IndexSupportsBackwardScan(((IndexOnlyScan *) node)->indexid);

		case T_SubqueryScan:
			return ExecSupportsBackwardScan(((SubqueryScan *) node)->subplan);

		case T_CustomScan:
			if (((CustomScan *) node)->flags & CUSTOMPATH_SUPPORT_BACKWARD_SCAN)
				return true;
			return false;

		case T_SeqScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_Material:
		case T_Sort:
			/* 它们不对 tlist 求值  */
			return true;

		case T_IncrementalSort:

			/*
			 * 与完整排序不同，增量排序在内存中只保留一组
			 * 元组，因此无法反向扫描。         
			 */
			return false;

		case T_LockRows:
		case T_Limit:
			return ExecSupportsBackwardScan(outerPlan(node));

		default:
			return false;
	}
}

/*
 * IndexScan 或 IndexOnlyScan 节点仅当索引的 AM 支持          
 * 反向扫描。 
 */
static bool
IndexSupportsBackwardScan(Oid indexid)
{
	bool		result;
	HeapTuple	ht_idxrel;
	Form_pg_class idxrelrec;
	IndexAmRoutine *amroutine;

	/* 获取索引关系的 pg_class 元组          */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indexid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", indexid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* 获取索引 AM 的 API 结构  */
	amroutine = GetIndexAmRoutineByAmId(idxrelrec->relam, false);

	result = amroutine->amcanbackward;

	pfree(amroutine);
	ReleaseSysCache(ht_idxrel);

	return result;
}

/*
 * ExecMaterializesOutput - 计划类型是否物化其输出？    
 *
 * 如果计划节点类型属于会自动物化其输出（通常          
 * 保存在 tuplestore 中）的类型则返回 true。对这类        
 * 计划而言，无参数变化的重新扫描没有启动成本，   
 * 每元组成本极低。
 */
bool
ExecMaterializesOutput(NodeTag plantype)
{
	switch (plantype)
	{
		case T_Material:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_Sort:
			return true;

		default:
			break;
	}

	return false;
}
