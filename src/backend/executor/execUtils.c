/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  杂项执行器工具例程            
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execUtils.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		CreateExecutorState		创建/删除执行器工作状态  
 *		FreeExecutorState
 *		CreateExprContext
 *		CreateStandaloneExprContext
 *		FreeExprContext
 *		ReScanExprContext
 *
 *		ExecAssignExprContext	计划节点 init 例程的公共代码 
 *		etc
 *
 *		ExecOpenScanRelation	扫描节点 init 例程的公共代码 
 *
 *		ExecInitRangeTable		设置执行器的范围表相关数据    
 *
 *		ExecGetRangeTableRelation		为范围表条目取得 Relation     
 *
 *		executor_errposition	报告错误的语法位置。        
 *
 *		RegisterExprContextCallback    注册函数关闭回调           
 *		UnregisterExprContextCallback  注销函数关闭回调             
 *
 *		GetAttributeByName		运行时从元组中提取列            
 *		GetAttributeByNum
 *
 *	 NOTES
 *		本文件传统上一直是存放                        
 *		无处安放的杂项执行器支持代码的地方。      
 */

#include "postgres.h"

#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeModifyTable.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"


static bool tlist_matches_tupdesc(PlanState *ps, List *tlist, int varno, TupleDesc tupdesc);
static void ShutdownExprContext(ExprContext *econtext, bool isCommit);
static RTEPermissionInfo *GetResultRTEPermissionInfo(ResultRelInfo *relinfo, EState *estate);


/* ----------------------------------------------------------------
 *				 执行器状态与内存管理函数          
 * ----------------------------------------------------------------
 */

/* ----------------
 *		CreateExecutorState
 *
 *		创建并初始化 EState 节点，该节点是整次     
 *		执行器调用的工作存储的根。           
 *
 * 主要是创建每查询内存上下文，它将用于容纳所有 
 * 存活至查询结束的工作数据。                            
 * 另请注意，每查询上下文的父上下文将是调用者的 
 * CurrentMemoryContext 
 * ----------------
 */
EState *
CreateExecutorState(void)
{
	EState	   *estate;
	MemoryContext qcontext;
	MemoryContext oldcontext;

	/*
	 * 为本执行器运行创建每查询内存上下文
	 */
	qcontext = AllocSetContextCreate(CurrentMemoryContext,
									 "ExecutorState",
									 ALLOCSET_DEFAULT_SIZES);

	/*
	 * 在每查询上下文中构造 EState 节点。这样一来，我们  
	 * 关闭时无需再为它单独执行 pfree()。      
	 */
	oldcontext = MemoryContextSwitchTo(qcontext);

	estate = makeNode(EState);

	/*
	 * 初始化执行器状态结构的所有字段。     
	 */
	estate->es_direction = ForwardScanDirection;
	estate->es_snapshot = InvalidSnapshot;	/* 调用者须初始化此项 */
	estate->es_crosscheck_snapshot = InvalidSnapshot;	/* 无 crosscheck*/
	estate->es_range_table = NIL;
	estate->es_range_table_size = 0;
	estate->es_relations = NULL;
	estate->es_rowmarks = NULL;
	estate->es_rteperminfos = NIL;
	estate->es_plannedstmt = NULL;

	estate->es_junkFilter = NULL;

	estate->es_output_cid = (CommandId) 0;

	estate->es_result_relations = NULL;
	estate->es_opened_result_relations = NIL;
	estate->es_tuple_routing_result_relations = NIL;
	estate->es_trig_target_relations = NIL;

	estate->es_insert_pending_result_relations = NIL;
	estate->es_insert_pending_modifytables = NIL;

	estate->es_param_list_info = NULL;
	estate->es_param_exec_vals = NULL;

	estate->es_queryEnv = NULL;

	estate->es_query_cxt = qcontext;

	estate->es_tupleTable = NIL;

	estate->es_processed = 0;
	estate->es_total_processed = 0;

	estate->es_top_eflags = 0;
	estate->es_instrument = 0;
	estate->es_finished = false;

	estate->es_exprcontexts = NIL;

	estate->es_subplanstates = NIL;

	estate->es_auxmodifytables = NIL;

	estate->es_per_tuple_exprcontext = NULL;

	estate->es_sourceText = NULL;

	estate->es_use_parallel_mode = false;

	estate->es_jit_flags = 0;
	estate->es_jit = NULL;

	/*
	 * 返回执行器状态结构        
	 */
	MemoryContextSwitchTo(oldcontext);

	return estate;
}

/* ----------------
 *		FreeExecutorState
 *
 *		释放一个 EState 及其所有剩余的工作存储。   
 *
 * 注意：本函数不负责释放非内存资源，例如已打开       
 * 的关系或缓冲区 pin。但它会关闭 EState 中任何仍       
 * 处于活跃状态的 ExprContext，并释放相关的 JIT 表达式。    
 * 对于 EState 只是被用于表达式求值、而非运行             
 * 完整计划的场景，这样的清理已经足够。         
 *
 * 可在任何内存上下文中调用，只要它不是              
 * 待释放的那些。   
 * ----------------
 */
void
FreeExecutorState(EState *estate)
{
	/*
	 * 显式地关闭并释放所有剩余的 ExprContext。我们这样   
	 * 做是为了确保剩余的关闭回调都会被调用（因为       
	 * 它们可能需要释放一些并非仅属于每查询内存        
	 * 上下文）。           
	 */
	while (estate->es_exprcontexts)
	{
		/*
		 * XXX：似乎应当存在比反复调用 list_delete() 更快的 
		 * 实现方式，不是吗？
		 */
		FreeExprContext((ExprContext *) linitial(estate->es_exprcontexts),
						true);
		/* FreeExprContext 为我们移除列表链接  */
	}

	/* 已分配则释放 JIT 上下文  */
	if (estate->es_jit)
	{
		jit_release_context(estate->es_jit);
		estate->es_jit = NULL;
	}

	/* 若已分配则释放分区目录         */
	if (estate->es_partition_directory)
	{
		DestroyPartitionDirectory(estate->es_partition_directory);
		estate->es_partition_directory = NULL;
	}

	/*
	 * 释放每查询内存上下文，从而一并释放所有工作 
	 * 内存，包括 EState 节点本身。   
	 */
	MemoryContextDelete(estate->es_query_cxt);
}

/*
 * CreateExprContext() 与 CreateWorkExprContext() 的内部实现，         
 * 它允许对 AllocSet 参数进行控制。      
 */
static ExprContext *
CreateExprContextInternal(EState *estate, Size minContextSize,
						  Size initBlockSize, Size maxBlockSize)
{
	ExprContext *econtext;
	MemoryContext oldcontext;

	/* 在每查询内存上下文中创建 ExprContext 节点         */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	econtext = makeNode(ExprContext);

	/* 初始化 ExprContext 字段     */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	econtext->ecxt_per_query_memory = estate->es_query_cxt;

	/*
	 * 在此上下文中为表达式求值创建工作内存。       
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(estate->es_query_cxt,
							  "ExprContext",
							  minContextSize,
							  initBlockSize,
							  maxBlockSize);

	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->caseValue_datum = (Datum) 0;
	econtext->caseValue_isNull = true;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = estate;

	econtext->ecxt_callbacks = NULL;

	/*
	 * 把该 ExprContext 链入 EState，以确保在 EState 被释放       
	 * 时会被关闭。由于使用 lcons()，关闭将以创建的   
	 * 逆序进行，这虽非必需，但也不会有坏处。            
	 */
	estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

	MemoryContextSwitchTo(oldcontext);

	return econtext;
}

/* ----------------
 *		CreateExprContext
 *
 *		在 EState 内为表达式求值创建一个上下文。    
 *
 * 一次执行器运行可能需要多个 ExprContext（我们通常    
 * 为每个 Plan 节点各建一个，另建一个供逐输出元组的 
 * 处理（如约束检查）。每个 ExprContext 各有"per-tuple"     
 * 内存上下文
 *
 * 注意：对调用者的内存上下文不做任何假设。 
 * ----------------
 */
ExprContext *
CreateExprContext(EState *estate)
{
	return CreateExprContextInternal(estate, ALLOCSET_DEFAULT_SIZES);
}


/* ----------------
 *		CreateWorkExprContext
 *
 * 与 CreateExprContext 类似，但把 AllocSet 尺寸设定为           
 * 与 work_mem 相称的合理值。如果最大块分配尺寸过      
 * 大，单次分配就可能轻易超过 work_mem。                   
 * ----------------
 */
ExprContext *
CreateWorkExprContext(EState *estate)
{
	Size		minContextSize = ALLOCSET_DEFAULT_MINSIZE;
	Size		initBlockSize = ALLOCSET_DEFAULT_INITSIZE;
	Size		maxBlockSize = ALLOCSET_DEFAULT_MAXSIZE;

	/* 把 maxBlockSize 选为不超过 work_mem 的 1/16            */
	while (16 * maxBlockSize > work_mem * 1024L)
		maxBlockSize >>= 1;

	if (maxBlockSize < ALLOCSET_DEFAULT_INITSIZE)
		maxBlockSize = ALLOCSET_DEFAULT_INITSIZE;

	return CreateExprContextInternal(estate, minContextSize,
									 initBlockSize, maxBlockSize);
}

/* ----------------
 *		CreateStandaloneExprContext
 *
 *		为独立表达式求值创建一个上下文。      
 *
 * 以这种方式创建的 ExprContext 可用于对不含任何         
 * 参数、子计划或 Var 引用的表达式求值（把元组引用 
 * 放入 scantuple 字段或许可行，但似乎并不明智）。   
 *
 * ExprContext 结构体在调用者的当前内存上下文中      
 * 分配，它同时也是其"per query"上下文。  
 *
 * 用完后由调用者负责释放该 ExprContext，或者至少 
 * 确保任何关闭回调都已得到调用，                  
 * （ReScanExprContext() 很合适）。否则非内存资源        
 * 可能泄漏。 
 * ----------------
 */
ExprContext *
CreateStandaloneExprContext(void)
{
	ExprContext *econtext;

	/* 在调用者的内存上下文中创建 ExprContext 节点     */
	econtext = makeNode(ExprContext);

	/* 初始化 ExprContext 字段     */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	econtext->ecxt_per_query_memory = CurrentMemoryContext;

	/*
	 * 在此上下文中为表达式求值创建工作内存。       
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "ExprContext",
							  ALLOCSET_DEFAULT_SIZES);

	econtext->ecxt_param_exec_vals = NULL;
	econtext->ecxt_param_list_info = NULL;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->caseValue_datum = (Datum) 0;
	econtext->caseValue_isNull = true;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = NULL;

	econtext->ecxt_callbacks = NULL;

	return econtext;
}

/* ----------------
 *		FreeExprContext
 *
 *		释放一个表达式上下文，含调用任何剩余的  
 *		关闭回调。    
 *
 * 由于我们会释放用于表达式求值的临时上下文，    
 * 先前已计算的按引用传递的表达式结果都将消失！       
 *
 * 如果 isCommit 为假，我们就是在错误清理过程中被      
 * 调用，此时不应调用回调，而只应释放内存。（也许   
 * 更妥的做法是调用回调并向它们传入 isCommit 标志，     
 * 但那需要的改动比当前看来合理的更大。）  
 *
 * 注意：对调用者的内存上下文不做任何假设。 
 * ----------------
 */
void
FreeExprContext(ExprContext *econtext, bool isCommit)
{
	EState	   *estate;

	/* 调用任何已注册的回调*/
	ShutdownExprContext(econtext, isCommit);
	/* 然后清理所用的内存  */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	/* 如有 EState 属主则解除链接    */
	estate = econtext->ecxt_estate;
	if (estate)
		estate->es_exprcontexts = list_delete_ptr(estate->es_exprcontexts,
												  econtext);
	/* 最后删除 ExprContext 节点 */
	pfree(econtext);
}

/*
 * ReScanExprContext
 *
 *		重置一个表达式上下文，为其计划节点的重新  
 *		扫描做准备。这需要调用任何已注册的关闭回调，  
 *		因为任何部分完成的集合返回函数都必须被取消。    
 *
 * 注意：对调用者的内存上下文不做任何假设。 
 */
void
ReScanExprContext(ExprContext *econtext)
{
	/* 调用任何已注册的回调*/
	ShutdownExprContext(econtext, true);
	/* 然后清理所用的内存  */
	MemoryContextReset(econtext->ecxt_per_tuple_memory);
}

/*
 * 为 EState 构建逐输出元组的 ExprContext。 
 *
 * 通常经 GetPerTupleExprContext() 宏间接调用，       
 * 而非直接 
 */
ExprContext *
MakePerTupleExprContext(EState *estate)
{
	if (estate->es_per_tuple_exprcontext == NULL)
		estate->es_per_tuple_exprcontext = CreateExprContext(estate);

	return estate->es_per_tuple_exprcontext;
}


/* ----------------------------------------------------------------
 *				 杂项节点初始化支持函数        
 *
 * 注意：所有这些函数都应当在 CurrentMemoryContext 等      
 * 于每查询内存上下文时调用  
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignExprContext
 *
 *		此函数会初始化 ps_ExprContext 字段。只有对使用   
 *		ExecQual 或 ExecProject 的节点才需这样做     
 *		因为那些例程需要 econtext。其他无需求值      
 *		表达式的节点就不必这么做。                  
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, PlanState *planstate)
{
	planstate->ps_ExprContext = CreateExprContext(estate);
}

/* ----------------
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(PlanState *planstate)
{
	return planstate->ps_ResultTupleDesc;
}

/*
 * ExecGetResultSlotOps - 关于节点结果槽类型的信息        
 */
const TupleTableSlotOps *
ExecGetResultSlotOps(PlanState *planstate, bool *isfixed)
{
	if (planstate->resultopsset && planstate->resultops)
	{
		if (isfixed)
			*isfixed = planstate->resultopsfixed;
		return planstate->resultops;
	}

	if (isfixed)
	{
		if (planstate->resultopsset)
			*isfixed = planstate->resultopsfixed;
		else if (planstate->ps_ResultTupleSlot)
			*isfixed = TTS_FIXED(planstate->ps_ResultTupleSlot);
		else
			*isfixed = false;
	}

	if (!planstate->ps_ResultTupleSlot)
		return &TTSOpsVirtual;

	return planstate->ps_ResultTupleSlot->tts_ops;
}


/* ----------------
 *		ExecAssignProjectionInfo
 *
 * 从节点的目标列表形成投影信息                 
 *
 * inputDesc 的注意事项与 ExecBuildProjectionInfo 相同：        
 * 对关系扫描节点要提供它，上层节点可传 NULL  
 * ----------------
 */
void
ExecAssignProjectionInfo(PlanState *planstate,
						 TupleDesc inputDesc)
{
	planstate->ps_ProjInfo =
		ExecBuildProjectionInfo(planstate->plan->targetlist,
								planstate->ps_ExprContext,
								planstate->ps_ResultTupleSlot,
								planstate,
								inputDesc);
}


/* ----------------
 *		ExecConditionalAssignProjectionInfo
 *
 * 与 ExecAssignProjectionInfo 相同，但如果无需投影则             
 * 存 NULL 而不构建投影信息
 * ----------------
 */
void
ExecConditionalAssignProjectionInfo(PlanState *planstate, TupleDesc inputDesc,
									int varno)
{
	if (tlist_matches_tupdesc(planstate,
							  planstate->plan->targetlist,
							  varno,
							  inputDesc))
	{
		planstate->ps_ProjInfo = NULL;
		planstate->resultopsset = planstate->scanopsset;
		planstate->resultopsfixed = planstate->scanopsfixed;
		planstate->resultops = planstate->scanops;
	}
	else
	{
		if (!planstate->ps_ResultTupleSlot)
		{
			ExecInitResultSlot(planstate, &TTSOpsVirtual);
			planstate->resultops = &TTSOpsVirtual;
			planstate->resultopsfixed = true;
			planstate->resultopsset = true;
		}
		ExecAssignProjectionInfo(planstate, inputDesc);
	}
}

static bool
tlist_matches_tupdesc(PlanState *ps, List *tlist, int varno, TupleDesc tupdesc)
{
	int			numattrs = tupdesc->natts;
	int			attrno;
	ListCell   *tlist_item = list_head(tlist);

	/* 检查 tlist 各属性     */
	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = TupleDescAttr(tupdesc, attrno - 1);
		Var		   *var;

		if (tlist_item == NULL)
			return false;		/* tlist 过短    */
		var = (Var *) ((TargetEntry *) lfirst(tlist_item))->expr;
		if (!var || !IsA(var, Var))
			return false;		/* tlist 项不是 Var  */
		/* 这些断言失败说明规划器出错  */
		Assert(var->varno == varno);
		Assert(var->varlevelsup == 0);
		if (var->varattno != attrno)
			return false;		/* 顺序有误 */
		if (att_tup->attisdropped)
			return false;		/* 表包含被删除的列       */
		if (att_tup->atthasmissing)
			return false;		/* 表中包含带缺失值的列          */

		/*
		 * 注意：通常 Var 类型应与 tupdesc 完全一致，但在涉 
		 * 及不同 typmod 列的并集情形下，Var 可能来自    
		 * 并集上方，因而带有 typmod -1。这是合理的情形，  
		 * 因为 Var 仍描述该列，只是不如 tupdesc 精确。   
		 * 我们可以修改规划器来阻止它，但那样就会        
		 * 插入投影步骤，仅仅为了把特定 typmod 转换为 
		 * typmod -1，                                                       
		 * 这相当愚蠢。    
		 */
		if (var->vartype != att_tup->atttypid ||
			(var->vartypmod != att_tup->atttypmod &&
			 var->vartypmod != -1))
			return false;		/* 类型不符  */

		tlist_item = lnext(tlist, tlist_item);
	}

	if (tlist_item)
		return false;			/* tlist 过长   */

	return true;
}

/* ----------------
 *		ExecFreeExprContext
 *
 * 计划节点的 ExprContext 应当在执行器关闭期间被       
 * 显式释放，因为可能有关闭回调需要调用。（其他         
 * 由上述例程创建的资源，例如投影信息，并不需要         
 * 显式释放，因为它们只是每查询内存上下文的内存。）
 *
 * 不过，在 ExecEndNode 期间并没有特别必要这样做，    
 * 因为 FreeExecutorState 会释放 EState 内所有剩余的         
 * ExprContext。让 FreeExecutorState 来做这件事，可以让        
 * 这些 ExprContext 按创建的逆序释放，而不是像在这里    
 * 就地删除时按创建顺序，省去 FreeExprContext 列表清理   
 * 中的 O(N^2) 开销。        
 * ----------------
 */
void
ExecFreeExprContext(PlanState *planstate)
{
	/*
	 * 按上文所述，实际上并不删除该 ExprContext。不过   
	 * 但仍把它从计划节点解链。 
	 */
	planstate->ps_ExprContext = NULL;
}


/* ----------------------------------------------------------------
 *				  Scan node support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(ScanState *scanstate, TupleDesc tupDesc)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc);
}

/* ----------------
 *		ExecCreateScanSlotFromOuterPlan
 * ----------------
 */
void
ExecCreateScanSlotFromOuterPlan(EState *estate,
								ScanState *scanstate,
								const TupleTableSlotOps *tts_ops)
{
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlanState(scanstate);
	tupDesc = ExecGetResultType(outerPlan);

	ExecInitScanTupleSlot(estate, scanstate, tupDesc, tts_ops);
}

/* ----------------------------------------------------------------
 *		ExecRelationIsTargetRelation
 *
 *		检测某关系（以范围表索引标识）是否为    
 *		查询的目标关系之一。              
 *
 * 注意：核心代码目前已不再使用它。我们保留它   
 * 是因为 FDW 可能希望用它来判断自己的外部表是     
 * 否为目标关系。
 * ----------------------------------------------------------------
 */
bool
ExecRelationIsTargetRelation(EState *estate, Index scanrelid)
{
	return list_member_int(estate->es_plannedstmt->resultRelations, scanrelid);
}

/* ----------------------------------------------------------------
 *		ExecOpenScanRelation
 *
 *		打开基础层扫描计划节点将要扫描的堆关系。        
 *		它应当在节点的 ExecInit 例程中调用。        
 * ----------------------------------------------------------------
 */
Relation
ExecOpenScanRelation(EState *estate, Index scanrelid, int eflags)
{
	Relation	rel;

	/* 打开该关系。 */
	rel = ExecGetRangeTableRelation(estate, scanrelid);

	/*
	 * 如果正尝试扫描一个不可扫描的关系则报错，但       
	 * 若查询实际不会运行则除外。此处做事略显笨拙，   
	 * 但或许没有更好的去处。                 
	 */
	if ((eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA)) == 0 &&
		!RelationIsScannable(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view \"%s\" has not been populated",
						RelationGetRelationName(rel)),
				 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

	return rel;
}

/*
 * ExecInitRangeTable
 *		设置执行器的范围表相关数据   
 *
 * 除范围表本身之外，还会初始化那些以范围表索  
 * 引为下标的数组。    
 */
void
ExecInitRangeTable(EState *estate, List *rangeTable, List *permInfos)
{
	/* 按原样记住范围表 List       */
	estate->es_range_table = rangeTable;

	/* 以及 RTEPermissionInfo List 也是   */
	estate->es_rteperminfos = permInfos;

	/* 设置关联数组的大小   */
	estate->es_range_table_size = list_length(rangeTable);

	/*
	 * 分配一个数组来存放与每个范围表条目对应的     
	 * 已打开 Relation，并将各条目初始化为 NULL。关系会      
	 * 按需打开并存放。  
	 */
	estate->es_relations = (Relation *)
		palloc0(estate->es_range_table_size * sizeof(Relation));

	/*
	 * es_result_relations 与 es_rowmarks 同样与           
	 * es_range_table 平行，仅在需要时分配。 
	 */
	estate->es_result_relations = NULL;
	estate->es_rowmarks = NULL;
}

/*
 * ExecGetRangeTableRelation
 *		为范围表条目打开其 Relation（如尚未打开）     
 *
 * Relation 将在 ExecEndPlan() 中关闭。    
 */
Relation
ExecGetRangeTableRelation(EState *estate, Index rti)
{
	Relation	rel;

	Assert(rti > 0 && rti <= estate->es_range_table_size);

	rel = estate->es_relations[rti - 1];
	if (rel == NULL)
	{
		/* 第一次到达此处时打开该关系  */
		RangeTblEntry *rte = exec_rt_fetch(rti, estate);

		Assert(rte->rtekind == RTE_RELATION);

		if (!IsParallelWorker())
		{
			/*
			 * 在普通查询中应当已持有适当的锁，但要通过   
			 * 通过 Assert 验证。由于 table_open 里已有一个    
			 * 坚持要求持有某种锁的 Assert，故只需在          
			 * rellockmode 高于最低级别时才做这项检查就        
			 * 足够了。     
			 */
			rel = table_open(rte->relid, NoLock);
			Assert(rte->rellockmode == AccessShareLock ||
				   CheckRelationLockedByMe(rel, rte->rellockmode, false));
		}
		else
		{
			/*
			 * 如果本进程是并行工作者，则需要自行取得   
			 * 对该关系的本地锁。这样即便父进程比我们先 
			 * 退出，行为也仍然正常。 
			 */
			rel = table_open(rte->relid, rte->rellockmode);
		}

		estate->es_relations[rti - 1] = rel;
	}

	return rel;
}

/*
 * ExecInitResultRelation
 *		打开传入 RT 索引所给定的关系，并填充其   
 *		ResultRelInfo node
 *
 * 这里我们还会把 ResultRelInfo 保存到 estate->es_result_relations
 * 数组中，以便稍后能用 RT 索引访问它。  
 */
void
ExecInitResultRelation(EState *estate, ResultRelInfo *resultRelInfo,
					   Index rti)
{
	Relation	resultRelationDesc;

	resultRelationDesc = ExecGetRangeTableRelation(estate, rti);
	InitResultRelInfo(resultRelInfo,
					  resultRelationDesc,
					  rti,
					  NULL,
					  estate->es_instrument);

	if (estate->es_result_relations == NULL)
		estate->es_result_relations = (ResultRelInfo **)
			palloc0(estate->es_range_table_size * sizeof(ResultRelInfo *));
	estate->es_result_relations[rti - 1] = resultRelInfo;

	/*
	 * 保存在列表中可以避免在可能只有少数条目非      
	 * 空时无谓地遍历整个数组。                       
	 */
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, resultRelInfo);
}

/*
 * UpdateChangedParamSet
 *		把变更参数加入计划节点的 chgParam 集合
 */
void
UpdateChangedParamSet(PlanState *node, Bitmapset *newchg)
{
	Bitmapset  *parmset;

	/*
	 * 计划节点只依赖于其 allParam 集合中列出的参数。      
	 * 别把其他内容并入 chgParam 集合。 
	 */
	parmset = bms_intersect(node->plan->allParam, newchg);
	node->chgParam = bms_join(node->chgParam, parmset);
}

/*
 * executor_errposition
 *		如有可能，报告执行时的光标位置。      
 *
 * 本函数预期在 ereport() 调用内使用。其返回值是         
 * 个占位值（总是 0）。  
 *
 * 语法树中存储的位置是源字符串内的字节偏移量。         
 * 我们必须把它们转换成从 1 开始的字符编号，以便   
 * 报告给客户端。（我们这样做是为了避免在正常      
 * 的非出错情形下付出不必要的开销：计算字符编号   
 * 比存词元偏移量贵得多。）  
 */
int
executor_errposition(EState *estate, int location)
{
	int			pos;

	/* 若未给出位置则不操作     */
	if (location < 0)
		return 0;
	/* 如果源文本不可用则无能为力           */
	if (estate == NULL || estate->es_sourceText == NULL)
		return 0;
	/* 把偏移量转换为字符序号  */
	pos = pg_mbstrlen_with_len(estate->es_sourceText, location) + 1;
	/* 然后把它传给 ereport 机制    */
	return errposition(pos);
}

/*
 * 在一个 ExprContext 中注册关闭回调。 
 *
 * 关闭回调会在 ExprContext 被删除或重新扫描时被调    
 * 用（按注册的逆序）。这为在该上下文中被调用    
 * 的函数提供了一个钩子，让它们完成所需的清理      
 * 这对返回集合的函数尤其有用。请注意：          
 * 如果执行因出错而中止，那么该回调将*不会*         
 * 被调用   
 */
void
RegisterExprContextCallback(ExprContext *econtext,
							ExprContextCallbackFunction function,
							Datum arg)
{
	ExprContext_CB *ecxt_callback;

	/* 把信息保存进合适的内存上下文  */
	ecxt_callback = (ExprContext_CB *)
		MemoryContextAlloc(econtext->ecxt_per_query_memory,
						   sizeof(ExprContext_CB));

	ecxt_callback->function = function;
	ecxt_callback->arg = arg;

	/* 链接到列表头部以获得恰当的执行顺序   */
	ecxt_callback->next = econtext->ecxt_callbacks;
	econtext->ecxt_callbacks = ecxt_callback;
}

/*
 * 从一个 ExprContext 中注销关闭回调。   
 *
 * 匹配该函数与参数的列表条目都会被移除。      
 * 如果不再需要调用该回调，就可以使用本函数。   
 */
void
UnregisterExprContextCallback(ExprContext *econtext,
							  ExprContextCallbackFunction function,
							  Datum arg)
{
	ExprContext_CB **prev_callback;
	ExprContext_CB *ecxt_callback;

	prev_callback = &econtext->ecxt_callbacks;

	while ((ecxt_callback = *prev_callback) != NULL)
	{
		if (ecxt_callback->function == function && ecxt_callback->arg == arg)
		{
			*prev_callback = ecxt_callback->next;
			pfree(ecxt_callback);
		}
		else
			prev_callback = &ecxt_callback->next;
	}
}

/*
 * 调用一个 ExprContext 中注册的所有关闭回调。   
 *
 * 回调列表会被清空（如果这只是重新扫描重置而      
 * 非删除 ExprContext 时尤为重要）。 
 *
 * 如果 isCommit 为假，只清空回调列表而不调用它们。   
 * （参见FreeExprContext注释） 
 */
static void
ShutdownExprContext(ExprContext *econtext, bool isCommit)
{
	ExprContext_CB *ecxt_callback;
	MemoryContext oldcontext;

	/* 在无事可做的正常情形下走快速路径。   */
	if (econtext->ecxt_callbacks == NULL)
		return;

	/*
	 * 在 econtext 的 per-tuple 上下文中调用这些回调。这样    
	 * 它们可能泄漏的内存均获清理。     
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * 按注册的逆序调用每个回调函数。             
	 */
	while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
	{
		econtext->ecxt_callbacks = ecxt_callback->next;
		if (isCommit)
			ecxt_callback->function(ecxt_callback->arg);
		pfree(ecxt_callback);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		这些函数会从给定元组 Datum 里返回请求属性 
 *		的值。                    
 *		以元组为参数的 C 函数应当使用这些函数。 
 *		例如 overpaid(EMP) 就可能调用 GetAttributeByNum()。     
 *		注意：这些函数其实相当慢，因为每次调用都   
 *		查 typcache        
 */
Datum
GetAttributeByName(HeapTupleHeader tuple, const char *attname, bool *isNull)
{
	AttrNumber	attrno;
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;
	int			i;

	if (attname == NULL)
		elog(ERROR, "invalid attribute name");

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* 有点取巧，但与旧行为兼容……      */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	attrno = InvalidAttrNumber;
	for (i = 0; i < tupDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, i);

		if (namestrcmp(&(att->attname), attname) == 0)
		{
			attrno = att->attnum;
			break;
		}
	}

	if (attrno == InvalidAttrNumber)
		elog(ERROR, "attribute \"%s\" does not exist", attname);

	/*
	 * heap_getattr 需要的是 HeapTuple 而非裸的                      
	 * HeapTupleHeader。我们设置全部字段以防用户查看系统
	 * 列。  
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

Datum
GetAttributeByNum(HeapTupleHeader tuple,
				  AttrNumber attrno,
				  bool *isNull)
{
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	if (!AttributeNumberIsValid(attrno))
		elog(ERROR, "invalid attribute number %d", attrno);

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* 有点取巧，但与旧行为兼容……      */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/*
	 * heap_getattr 需要的是 HeapTuple 而非裸的                      
	 * HeapTupleHeader。我们设置全部字段以防用户查看系统
	 * 列。  
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

/*
 * tlist 中的条目数（包括任何 resjunk 条目！）
 */
int
ExecTargetListLength(List *targetlist)
{
	/* 这曾经更复杂，但 fjoin 已消亡          */
	return list_length(targetlist);
}

/*
 * tlist 中的条目数，不包括任何 resjunk 条目     
 */
int
ExecCleanTargetListLength(List *targetlist)
{
	int			len = 0;
	ListCell   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = lfirst_node(TargetEntry, tl);

		if (!curTle->resjunk)
			len++;
	}
	return len;
}

/*
 * 返回 relInfo 用于存放触发器 OLD 元组的槽。 
 */
TupleTableSlot *
ExecGetTriggerOldSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_TrigOldSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_TrigOldSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_TrigOldSlot;
}

/*
 * 返回 relInfo 用于存放触发器 NEW 元组的槽。 
 */
TupleTableSlot *
ExecGetTriggerNewSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_TrigNewSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_TrigNewSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_TrigNewSlot;
}

/*
 * 返回 relInfo 用于处理 RETURNING 元组的槽。         
 */
TupleTableSlot *
ExecGetReturningSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_ReturningSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_ReturningSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_ReturningSlot;
}

/*
 * 返回把给定子结果关系的元组转换为查询主目标         
 * （"root"）关系的行类型所需的映射。注意 NULL 结果    
 * 是有效的，表示无需进行转换。                  
 */
TupleConversionMap *
ExecGetChildToRootMap(ResultRelInfo *resultRelInfo)
{
	/* 如果尚未计算，则为此子关系计算映射。      */
	if (!resultRelInfo->ri_ChildToRootMapValid)
	{
		ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;

		if (rootRelInfo)
			resultRelInfo->ri_ChildToRootMap =
				convert_tuples_by_name(RelationGetDescr(resultRelInfo->ri_RelationDesc),
									   RelationGetDescr(rootRelInfo->ri_RelationDesc));
		else					/* 这不是子结果关系      */
			resultRelInfo->ri_ChildToRootMap = NULL;

		resultRelInfo->ri_ChildToRootMapValid = true;
	}

	return resultRelInfo->ri_ChildToRootMap;
}

/*
 * 返回把给定根结果关系的元组转换为给定子关系         
 * 的行类型所需的映射。注意 NULL 结果是有效的，它        
 * 表示无需进行转换。            
 */
TupleConversionMap *
ExecGetRootToChildMap(ResultRelInfo *resultRelInfo, EState *estate)
{
	/* 绝不能对非子结果关系调用本函数。    */
	Assert(resultRelInfo->ri_RootResultRelInfo);

	/* 如果尚未计算，则为此子关系计算映射。      */
	if (!resultRelInfo->ri_RootToChildMapValid)
	{
		ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;
		TupleDesc	indesc = RelationGetDescr(rootRelInfo->ri_RelationDesc);
		TupleDesc	outdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		Relation	childrel = resultRelInfo->ri_RelationDesc;
		AttrMap    *attrMap;
		MemoryContext oldcontext;

		/*
		 * 当该子表不是分区（!relispartition）时，它可能      
		 * 拥有根表中不存在的列，此时我们通过向           
		 * missing_ok 传 true 来要求忽略。   
		 */
		oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
		attrMap = build_attrmap_by_name_if_req(indesc, outdesc,
											   !childrel->rd_rel->relispartition);
		if (attrMap)
			resultRelInfo->ri_RootToChildMap =
				convert_tuples_by_name_attrmap(indesc, outdesc, attrMap);
		MemoryContextSwitchTo(oldcontext);
		resultRelInfo->ri_RootToChildMapValid = true;
	}

	return resultRelInfo->ri_RootToChildMap;
}

/* 返回表示正在被插入的列的位图。       */
Bitmapset *
ExecGetInsertedCols(ResultRelInfo *relinfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relinfo, estate);

	if (perminfo == NULL)
		return NULL;

	/* 按需把列映射为子关系的属性号。           */
	if (relinfo->ri_RootResultRelInfo)
	{
		TupleConversionMap *map = ExecGetRootToChildMap(relinfo, estate);

		if (map)
			return execute_attr_map_cols(map->attrMap, perminfo->insertedCols);
	}

	return perminfo->insertedCols;
}

/* 返回表示正在被更新的列的位图。      */
Bitmapset *
ExecGetUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relinfo, estate);

	if (perminfo == NULL)
		return NULL;

	/* 按需把列映射为子关系的属性号。           */
	if (relinfo->ri_RootResultRelInfo)
	{
		TupleConversionMap *map = ExecGetRootToChildMap(relinfo, estate);

		if (map)
			return execute_attr_map_cols(map->attrMap, perminfo->updatedCols);
	}

	return perminfo->updatedCols;
}

/* 返回表示正在被更新的生成列的位图。          */
Bitmapset *
ExecGetExtraUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	/* 尚未计算则先算出该信息。  */
	if (relinfo->ri_GeneratedExprsU == NULL)
		ExecInitStoredGenerated(relinfo, estate, CMD_UPDATE);
	return relinfo->ri_extraUpdatedCols;
}

/*
 * 返回正在被更新的列，包括生成列            
 *
 * 该位图在逐元组内存上下文中分配。如有需要，              
 * 调用者需把它复制到具有合适生命周期的上下文中。     
 */
Bitmapset *
ExecGetAllUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	Bitmapset  *ret;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	ret = bms_union(ExecGetUpdatedCols(relinfo, estate),
					ExecGetExtraUpdatedCols(relinfo, estate));

	MemoryContextSwitchTo(oldcxt);

	return ret;
}

/*
 * GetResultRTEPermissionInfo
 *		为 ExecGet*Cols() 例程查找 RTEPermissionInfo     
 */
static RTEPermissionInfo *
GetResultRTEPermissionInfo(ResultRelInfo *relinfo, EState *estate)
{
	Index		rti;
	RangeTblEntry *rte;
	RTEPermissionInfo *perminfo = NULL;

	if (relinfo->ri_RootResultRelInfo)
	{
		/*
		 * 对于继承子结果关系（INSERT 的分区路由目标或     
		 * 子 UPDATE 目标），返回根父 RTE 来获取            
		 * RTEPermissionInfo，因为那是唯一分配有                    
		 * 该信息的 RTE。       
		 */
		rti = relinfo->ri_RootResultRelInfo->ri_RangeTableIndex;
	}
	else if (relinfo->ri_RangeTableIndex != 0)
	{
		/*
		 * 非子结果关系应当拥有自己的 RTEPermissionInfo。      
		 */
		rti = relinfo->ri_RangeTableIndex;
	}
	else
	{
		/*
		 * 该关系既不在范围表中，也非分区路由目标。  
		 * 这个 ResultRelInfo 必定只是为了触发器而创           
		 * 建的，并且并没有向该关系执行插入。(参见        
		 * ExecGetTriggerResultRel.)
		 */
		rti = 0;
	}

	if (rti > 0)
	{
		rte = exec_rt_fetch(rti, estate);
		perminfo = getRTEPermissionInfo(estate->es_rteperminfos, rte);
	}

	return perminfo;
}

/*
 * ExecGetResultRelCheckAsUser
 *		返回修改传入结果关系所用的用户          
 *
 * 该用户通过查找该关系的，或者若为子表则其根          
 * 父的 RTEPermissionInfo。     
 */
Oid
ExecGetResultRelCheckAsUser(ResultRelInfo *relInfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relInfo, estate);

	/* XXX - 此情形下返回 GetUserId() 或许可行? */
	if (perminfo == NULL)
		elog(ERROR, "no RTEPermissionInfo found for result relation with OID %u",
			 RelationGetRelid(relInfo->ri_RelationDesc));

	return perminfo->checkAsUser ? perminfo->checkAsUser : GetUserId();
}
