/*-------------------------------------------------------------------------
 *
 * execMain.c
 *	  顶层执行器接口例程          
 *
 * 接口例程      
 *	ExecutorStart()
 *	ExecutorRun()
 *	ExecutorFinish()
 *	ExecutorEnd()
 *
 *	这四个过程是执行器的外部接口。                    
 *	每种情况下都需要以查询描述符作为参数。     
 *
 *	必须在任何查询计划的执行开始时调用 ExecutorStart
 *	并且总要在计划执行结束时调用 ExecutorEnd        
 *	（除非因错误而中止）。                       
 *
 *	ExecutorRun 接受 direction 与 count 实参，指定计划          
 *	应正向还是反向执行，以及执行多少条元组。            
 *	某些情况下可能需多次调用 ExecutorRun，才能处理完   
 *	计划所需的全部元组。也可以在执行完整计划之前停下
 *	（但仅当它是 SELECT 时）。         
 *
 *	必须在最后一次 ExecutorRun 调用之后、                 
 *	ExecutorEnd 之前调用。仅 EXPLAIN 场景可省略，        
 *	此时也应省略 ExecutorRun。  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execMain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_publication.h"
#include "commands/matview.h"
#include "commands/trigger.h"
#include "executor/execdebug.h"
#include "executor/nodeSubplan.h"
#include "foreign/fdwapi.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"


/* 插件在 ExecutorStart/Run/Finish/End 中获取控制的钩子  */
ExecutorStart_hook_type ExecutorStart_hook = NULL;
ExecutorRun_hook_type ExecutorRun_hook = NULL;
ExecutorFinish_hook_type ExecutorFinish_hook = NULL;
ExecutorEnd_hook_type ExecutorEnd_hook = NULL;

/* 插件在 ExecCheckPermissions() 中获取控制的钩子*/
ExecutorCheckPerms_hook_type ExecutorCheckPerms_hook = NULL;

/* 仅本模块内部使用的局部例程声明         */
static void InitPlan(QueryDesc *queryDesc, int eflags);
static void CheckValidRowMarkRel(Relation rel, RowMarkType markType);
static void ExecPostprocessPlan(EState *estate);
static void ExecEndPlan(PlanState *planstate, EState *estate);
static void ExecutePlan(QueryDesc *queryDesc,
						CmdType operation,
						bool sendTuples,
						uint64 numberTuples,
						ScanDirection direction,
						DestReceiver *dest);
static bool ExecCheckPermissionsModified(Oid relOid, Oid userid,
										 Bitmapset *modifiedCols,
										 AclMode requiredPerms);
static void ExecCheckXactReadOnly(PlannedStmt *plannedstmt);
static char *ExecBuildSlotValueDescription(Oid reloid,
										   TupleTableSlot *slot,
										   TupleDesc tupdesc,
										   Bitmapset *modifiedCols,
										   int maxfieldlen);
static void EvalPlanQualStart(EPQState *epqstate, Plan *planTree);

/* 局部声明结束 */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		本例程必须在任何查询计划开始执行时                 
 *		被调用 
 *
 * 接受先前由 CreateQueryDesc 创建的 QueryDesc（之所以           
 * 分开创建，只因有些地方将 QueryDesc 用于工具命令）。    
 * 会填写 QueryDesc 的 tupDesc 字段，用以描述将返回的      
 * 元组，并建立内部字段（estate 与 planstate）。         
 *
 * eflags 包含 executor.h 中描述的标志位。    
 *
 * 注意：调用时所处 CurrentMemoryContext 将成为本次          
 * Executor 调用所用每查询上下文的父上下文。  
 *
 * 我们提供函数钩子变量，让可加载插件能在     
 * ExecutorStart 被调用时取得控制。插件通常会      
 * 调用 standard_ExecutorStart()。     
 *
 * ----------------------------------------------------------------
 */
void
ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/*
	 * 某些情况下（例如 EXECUTE 语句或扩展查询协议           
	 * execute 消息）不会上报 query_id，所以现在就上报。     
	 *
	 * 注意：多次上报 query_id 并无害，如果顶层              
	 * query_id 已经上报过，则后续调用将被               
	 * 忽略。
	 */
	pgstat_report_query_id(queryDesc->plannedstmt->queryId, false);

	if (ExecutorStart_hook)
		(*ExecutorStart_hook) (queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
standard_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全性检查：queryDesc 不得已被启动       */
	Assert(queryDesc != NULL);
	Assert(queryDesc->estate == NULL);

	/*
	 * 若事务为只读，则需检查是否计划对非临时表       
	 * 执行写入。EXPLAIN 被视为只读。                         
	 *
	 * 并行模式下不允许写入。支持 UPDATE 和 DELETE         
	 * 需要 (a) 将组合 CID 哈希存入共享内存，而不是        
	 * 只在并行开始时同步一次；且需要 (b) 一种替代         
	 * heap_update() 依靠 xmax 实现互斥的做法。                   
	 * INSERT 可能没有这类麻烦，但我们为简化检查而     
	 * 禁止 
	 *
	 * 我们在 CommandCounterIncrement 及其他地方设有              
	 * 更低层的防线来防止在并行模式下执行不安全操作，  
	 * 但此处报错更友好。      
	 */
	if ((XactReadOnly || IsInParallelMode()) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecCheckXactReadOnly(queryDesc->plannedstmt);

	/*
	 * 构建 EState，切入启动用每查询内存上下文。     
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * 如有则从 queryDesc 填入外部参数；并为内部参数   
	 * 分配工作区                  
	 */
	estate->es_param_list_info = queryDesc->params;

	if (queryDesc->plannedstmt->paramExecTypes != NIL)
	{
		int			nParamExec;

		nParamExec = list_length(queryDesc->plannedstmt->paramExecTypes);
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(nParamExec * sizeof(ParamExecData));
	}

	/* 现在要求所有调用者都提供 sourceText  */
	Assert(queryDesc->sourceText != NULL);
	estate->es_sourceText = queryDesc->sourceText;

	/*
	 * 如有则从 queryDesc 填入查询环境。          
	 */
	estate->es_queryEnv = queryDesc->queryEnv;

	/*
	 * 若查询非只读，则设置用于标记输出元组的命令 ID   
	 */
	switch (queryDesc->operation)
	{
		case CMD_SELECT:

			/*
			 * SELECT FOR [KEY] UPDATE/SHARE 与修改型 CTE 需要标记  
			 * 元组
			 */
			if (queryDesc->plannedstmt->rowMarks != NIL ||
				queryDesc->plannedstmt->hasModifyingCTE)
				estate->es_output_cid = GetCurrentCommandId(true);

			/*
			 * 不含修改型 CTE 的 SELECT 不可能排队触发器，    
			 * 因此强制跳过触发器。这只是微小的效率         
			 * 手段，因为 AfterTriggerBeginQuery/AfterTriggerEndQuery   
			 * 并不算昂贵，但我们不妨这么做。  
			 */
			if (!queryDesc->plannedstmt->hasModifyingCTE)
				eflags |= EXEC_FLAG_SKIP_TRIGGERS;
			break;

		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_UPDATE:
		case CMD_MERGE:
			estate->es_output_cid = GetCurrentCommandId(true);
			break;

		default:
			elog(ERROR, "unrecognized operation code: %d",
				 (int) queryDesc->operation);
			break;
	}

	/*
	 * 将其他重要信息复制进 EState           
	 */
	estate->es_snapshot = RegisterSnapshot(queryDesc->snapshot);
	estate->es_crosscheck_snapshot = RegisterSnapshot(queryDesc->crosscheck_snapshot);
	estate->es_top_eflags = eflags;
	estate->es_instrument = queryDesc->instrument_options;
	estate->es_jit_flags = queryDesc->plannedstmt->jitFlags;

	/*
	 * 除非被告知不要，或为仅 EXPLAIN 模式（此时不会  
	 * 调用 ExecutorFinish），否则建立 AFTER 触发器上下文。 
	 */
	if (!(eflags & (EXEC_FLAG_SKIP_TRIGGERS | EXEC_FLAG_EXPLAIN_ONLY)))
		AfterTriggerBeginQuery();

	/*
	 * 初始化计划状态树      
	 */
	InitPlan(queryDesc, eflags);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorRun
 *
 *		这是执行器模块的主例程。它接受来自        
 *		traffic cop 的查询描述符，并执行查询          
 *		计划。  
 *
 *		必须已调用过 ExecutorStart。         
 *
 *		若 direction 为 NoMovementScanDirection，则除了       
 *		启动/关闭目标外不做其他操作。否则，     
 *		我们按指定方向最多取出 'count' 条元组。      
 *
 *		注意：count = 0 表示无 portal 限制，即一直运行   
 *		到完成为止。另注意 count 限制只作用于取出的 
 *		元组，而不是例如由 ModifyTable 计划节点插入/         
 *		更新或删除的元组。
 *
 *		execute_once 会被忽略，只为避免稳定分支中           
 *		的 API 中断。  
 *
 *		没有返回值，但输出元组（若有）会发送到       
 *		QueryDesc 中指定的目标接收器；顶层处理的            
 *		元组数可在 estate->es_processed 中查到。   
 *		而所有 ExecutorRun 调用处理的元组总数可在           
 *		estate->es_total_processed 中查到。                          
 *
 *		我们提供函数钩子变量，让可加载插件能在     
 *		ExecutorRun 被调用时取得控制。插件通常会      
 *		调用 standard_ExecutorRun()。     
 *
 * ----------------------------------------------------------------
 */
void
ExecutorRun(QueryDesc *queryDesc,
			ScanDirection direction, uint64 count,
			bool execute_once)
{
	if (ExecutorRun_hook)
		(*ExecutorRun_hook) (queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

void
standard_ExecutorRun(QueryDesc *queryDesc,
					 ScanDirection direction, uint64 count, bool execute_once)
{
	EState	   *estate;
	CmdType		operation;
	DestReceiver *dest;
	bool		sendTuples;
	MemoryContext oldcontext;

	/* 健全检查  */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * 切换到每查询内存上下文   
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 允许对执行器整体运行时进行插桩     */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/*
	 * 从查询描述符与查询特性中提取信息。                 
	 */
	operation = queryDesc->operation;
	dest = queryDesc->dest;

	/*
	 * 若将发出元组，则启动元组接收器        
	 */
	estate->es_processed = 0;

	sendTuples = (operation == CMD_SELECT ||
				  queryDesc->plannedstmt->hasReturning);

	if (sendTuples)
		dest->rStartup(dest, operation, queryDesc->tupDesc);

	/*
	 * run plan
	 */
	if (!ScanDirectionIsNoMovement(direction))
		ExecutePlan(queryDesc,
					operation,
					sendTuples,
					count,
					direction,
					dest);

	/*
	 * 更新 es_total_processed，以跟踪多次 ExecutorRun()      
	 * 调用处理的元组数                      
	 */
	estate->es_total_processed += estate->es_processed;

	/*
	 * 若已启动，则关闭元组接收器  
	 */
	if (sendTuples)
		dest->rShutdown(dest);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, estate->es_processed);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorFinish
 *
 *		须在最后一次 ExecutorRun 调用后调用本例程。  
 *		它执行诸如引发 AFTER 触发器等清理工作。  
 *		它之所以与 ExecutorEnd 分开，是因为 EXPLAIN    
 *		ANALYZE 需将其计入总运行时。     
 *
 *		我们提供函数钩子变量，让可加载插件能在     
 *		ExecutorFinish 被调用时取得控制。插件通常会      
 *		调用 standard_ExecutorFinish()。     
 *
 * ----------------------------------------------------------------
 */
void
ExecutorFinish(QueryDesc *queryDesc)
{
	if (ExecutorFinish_hook)
		(*ExecutorFinish_hook) (queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

void
standard_ExecutorFinish(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全检查  */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/* 每个 Executor 实例应运行一次且仅一次           */
	Assert(!estate->es_finished);

	/* 切换到每查询内存上下文    */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* 允许对执行器整体运行时进行插桩     */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/* 将 ModifyTable 节点执行完毕  */
	ExecPostprocessPlan(estate);

	/* 如未禁止则执行已排队 AFTER 触发器    */
	if (!(estate->es_top_eflags & EXEC_FLAG_SKIP_TRIGGERS))
		AfterTriggerEndQuery(estate);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, 0);

	MemoryContextSwitchTo(oldcontext);

	estate->es_finished = true;
}

/* ----------------------------------------------------------------
 *		ExecutorEnd
 *
 *		本例程必须在任何查询计划执行结束时       
 *		调用    
 *
 *		我们提供函数钩子变量，让可加载插件能在     
 *		ExecutorEnd 被调用时取得控制。插件通常会      
 *		调用 standard_ExecutorEnd()。     
 *
 * ----------------------------------------------------------------
 */
void
ExecutorEnd(QueryDesc *queryDesc)
{
	if (ExecutorEnd_hook)
		(*ExecutorEnd_hook) (queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
standard_ExecutorEnd(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全检查  */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * 检查是否已调用 ExecutorFinish，仅 EXPLAIN 模式除外。     
	 * 此断言是必需的，因为 ExecutorFinish 为 9.1 新增，      
	 * 调用者可能遗漏。
	 */
	Assert(estate->es_finished ||
		   (estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * 切换到每查询内存上下文以运行 ExecEndPlan 
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndPlan(queryDesc->planstate, estate);

	/* 丢弃我们的快照      */
	UnregisterSnapshot(estate->es_snapshot);
	UnregisterSnapshot(estate->es_crosscheck_snapshot);

	/*
	 * 必须在销毁该上下文之前切出它     
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 释放 EState 与每查询内存上下文。这应当会释放   
	 * 执行器分配的一切              
	 */
	FreeExecutorState(estate);

	/* 重置 queryDesc 中已不再指向任何内容的字段*/
	queryDesc->tupDesc = NULL;
	queryDesc->estate = NULL;
	queryDesc->planstate = NULL;
	queryDesc->totaltime = NULL;
}

/* ----------------------------------------------------------------
 *		ExecutorRewind
 *
 *		本例程可在已打开的 queryDesc 上调用，将其    
 *		倒回。    
 * ----------------------------------------------------------------
 */
void
ExecutorRewind(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* 健全检查  */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/* 对更新型查询重新扫描多半并不合理      */
	Assert(queryDesc->operation == CMD_SELECT);

	/*
	 * 切换到每查询内存上下文   
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * rescan plan
	 */
	ExecReScan(queryDesc->planstate);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * ExecCheckPermissions
 *		检查查询中提及的关系的访问权限             
 *
 * 若权限足够则返回真。否则，当 ereport_on_violation            
 * 为真时抛出相应错误，否则仅返回假。                      
 *
 * 注意：这并不处理行级安全策略（即 RLS）。如果           
 * 本权限检查通过后会有行返回给用户，则还需          
 * 考虑 RLS（以及 check_enable_rls()）。                          
 *
 * 见 rewrite/rowsecurity.c.
 *
 * 注意：我们已不再使用 rangeTable，但为可能仍想            
 * 查看 RTE 的钩子而保留。     
 */
bool
ExecCheckPermissions(List *rangeTable, List *rteperminfos,
					 bool ereport_on_violation)
{
	ListCell   *l;
	bool		result = true;

#ifdef USE_ASSERT_CHECKING
	Bitmapset  *indexset = NULL;

	/* 检查 rteperminfos 与 rangeTable 是否一致       */
	foreach(l, rangeTable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

		if (rte->perminfoindex != 0)
		{
			/* 健全检查  */

			/*
			 * 只有关系 RTE 和曾是关系 RTE（视图）的         
			 * 子查询 RTE 才设置 perminfoindex。  
			 */
			Assert(rte->rtekind == RTE_RELATION ||
				   (rte->rtekind == RTE_SUBQUERY &&
					rte->relkind == RELKIND_VIEW));

			(void) getRTEPermissionInfo(rteperminfos, rte);
			/* 不允许多对一映射        */
			Assert(!bms_is_member(rte->perminfoindex, indexset));
			indexset = bms_add_member(indexset, rte->perminfoindex);
		}
	}

	/* 所有 rteperminfos 均被引用*/
	Assert(bms_num_members(indexset) == list_length(rteperminfos));
#endif

	foreach(l, rteperminfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, l);

		Assert(OidIsValid(perminfo->relid));
		result = ExecCheckOneRelPerms(perminfo);
		if (!result)
		{
			if (ereport_on_violation)
				aclcheck_error(ACLCHECK_NO_PRIV,
							   get_relkind_objtype(get_rel_relkind(perminfo->relid)),
							   get_rel_name(perminfo->relid));
			return false;
		}
	}

	if (ExecutorCheckPerms_hook)
		result = (*ExecutorCheckPerms_hook) (rangeTable, rteperminfos,
											 ereport_on_violation);
	return result;
}

/*
 * ExecCheckOneRelPerms
 *		检查单个关系的访问权限。           
 */
bool
ExecCheckOneRelPerms(RTEPermissionInfo *perminfo)
{
	AclMode		requiredPerms;
	AclMode		relPerms;
	AclMode		remainingPerms;
	Oid			userid;
	Oid			relOid = perminfo->relid;

	requiredPerms = perminfo->requiredPerms;
	Assert(requiredPerms != 0);

	/*
	 * 用以检查的 userid：当前用户，除非有 setuid 迹象。  
	 *
	 * 注意：GetUserId() 目前足够快，因此为每个关系      
	 * 单独调用一次并无妨害。如果这一前提不再成立，    
	 * 我们可以改为在 ExecCheckPermissions 中调用一次，      
	 * 并把 userid 向下传递。目前暂无此必要。   
	 */
	userid = OidIsValid(perminfo->checkAsUser) ?
		perminfo->checkAsUser : GetUserId();

	/*
	 * 我们必须拥有 *全部* requiredPerms 位，但其中一些       
	 * 位可以由列级权限而非关系级权限来满足。          
	 * 首先，移除已由关系权限满足的那些位。            
	 */
	relPerms = pg_class_aclmask(relOid, userid, requiredPerms, ACLMASK_ALL);
	remainingPerms = requiredPerms & ~relPerms;
	if (remainingPerms != 0)
	{
		int			col = -1;

		/*
		 * 如果缺少仅以关系权限形式存在的权限，则可以    
		 * 直接失败。           
		 */
		if (remainingPerms & ~(ACL_SELECT | ACL_INSERT | ACL_UPDATE))
			return false;

		/*
		 * 检查我们在列级别是否具有所需权限。           
		 *
		 * 注意：失败仅报表级错误；若我们拥有部分而非  
		 * 全部列权限，则报告列级错误会更贴切一些，     
		 * 但尚未实现。
		 */
		if (remainingPerms & ACL_SELECT)
		{
			/*
			 * 当查询未显式引用任何列时（例如 SELECT        
			 * COUNT(*) FROM table），只要拥有该关系任一       
			 * 列的 SELECT 权限即允许，此为 SQL 规范。  
			 */
			if (bms_is_empty(perminfo->selectedCols))
			{
				if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
											  ACLMASK_ANY) != ACLCHECK_OK)
					return false;
			}

			while ((col = bms_next_member(perminfo->selectedCols, col)) >= 0)
			{
				/* 位号以 FirstLowInvalidHeapAttributeNumber 为偏移  */
				AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

				if (attno == InvalidAttrNumber)
				{
					/* 整行引用，必须对所有列都有权限   */
					if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
												  ACLMASK_ALL) != ACLCHECK_OK)
						return false;
				}
				else
				{
					if (pg_attribute_aclcheck(relOid, attno, userid,
											  ACL_SELECT) != ACLCHECK_OK)
						return false;
				}
			}
		}

		/*
		 * 对 mod 列的处理基本相同，适用于 remainingPerms       
		 * 所指定的 INSERT 与 UPDATE 权限。 
		 */
		if (remainingPerms & ACL_INSERT &&
			!ExecCheckPermissionsModified(relOid,
										  userid,
										  perminfo->insertedCols,
										  ACL_INSERT))
			return false;

		if (remainingPerms & ACL_UPDATE &&
			!ExecCheckPermissionsModified(relOid,
										  userid,
										  perminfo->updatedCols,
										  ACL_UPDATE))
			return false;
	}
	return true;
}

/*
 * ExecCheckPermissionsModified
 *		检查单个关系的 INSERT 或 UPDATE 访问权限（这两者      
 *		处理一致）。       
 */
static bool
ExecCheckPermissionsModified(Oid relOid, Oid userid, Bitmapset *modifiedCols,
							 AclMode requiredPerms)
{
	int			col = -1;

	/*
	 * 当查询未显式更新任何列时，如果我们对该关系      
	 * 任一列具有权限，则允许该查询。这是为了处理   
	 * SELECT FOR UPDATE 以及 UPDATE 中的一些极端情况。  
	 */
	if (bms_is_empty(modifiedCols))
	{
		if (pg_attribute_aclcheck_all(relOid, userid, requiredPerms,
									  ACLMASK_ANY) != ACLCHECK_OK)
			return false;
	}

	while ((col = bms_next_member(modifiedCols, col)) >= 0)
	{
		/* 位号以 FirstLowInvalidHeapAttributeNumber 为偏移  */
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/* 整行引用在此不可能出现     */
			elog(ERROR, "whole-row update is not implemented");
		}
		else
		{
			if (pg_attribute_aclcheck(relOid, attno, userid,
									  requiredPerms) != ACLCHECK_OK)
				return false;
		}
	}
	return true;
}

/*
 * 检查查询不会隐含对非临时表的任何写入；除非   
 * 我们处于并行模式——此时连临时表也一律禁止     
 * 写入。      
 *
 * 注意：在热备（Hot Standby）中，这将需要像并行 
 * 模式那样拒绝临时表写入；但 HS 备机根本无法创建        
 * 临时表，因此无需做该检查。                      
 */
static void
ExecCheckXactReadOnly(PlannedStmt *plannedstmt)
{
	ListCell   *l;

	/*
	 * 若在并行模式下对表（临时或非临时）请求写权   
	 * 限则失败，否则对任何非临时表都失败。    
	 */
	foreach(l, plannedstmt->permInfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, l);

		if ((perminfo->requiredPerms & (~ACL_SELECT)) == 0)
			continue;

		if (isTempNamespace(get_rel_namespace(perminfo->relid)))
			continue;

		PreventCommandIfReadOnly(CreateCommandName((Node *) plannedstmt));
	}

	if (plannedstmt->commandType != CMD_SELECT || plannedstmt->hasModifyingCTE)
		PreventCommandIfParallelMode(CreateCommandName((Node *) plannedstmt));
}


/* ----------------------------------------------------------------
 *		InitPlan
 *
 *		初始化查询计划：打开文件、分配存储，  
 *		并启动规则管理器     
 * ----------------------------------------------------------------
 */
static void
InitPlan(QueryDesc *queryDesc, int eflags)
{
	CmdType		operation = queryDesc->operation;
	PlannedStmt *plannedstmt = queryDesc->plannedstmt;
	Plan	   *plan = plannedstmt->planTree;
	List	   *rangeTable = plannedstmt->rtable;
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate;
	TupleDesc	tupType;
	ListCell   *l;
	int			i;

	/*
	 * 执行权限检查   
	 */
	ExecCheckPermissions(rangeTable, plannedstmt->permInfos, true);

	/*
	 * 初始化节点的执行状态       
	 */
	ExecInitRangeTable(estate, rangeTable, plannedstmt->permInfos);

	estate->es_plannedstmt = plannedstmt;

	/*
	 * 接着，若有 PlanRowMark，则据此建 ExecRowMark 数组。  
	 */
	if (plannedstmt->rowMarks)
	{
		estate->es_rowmarks = (ExecRowMark **)
			palloc0(estate->es_range_table_size * sizeof(ExecRowMark *));
		foreach(l, plannedstmt->rowMarks)
		{
			PlanRowMark *rc = (PlanRowMark *) lfirst(l);
			Oid			relid;
			Relation	relation;
			ExecRowMark *erm;

			/* 忽略 "parent" rowmark；它们在运行时无关紧要 */
			if (rc->isParent)
				continue;

			/* 获取关系的 OID（子查询则得 InvalidOid）      */
			relid = exec_rt_fetch(rc->rti, estate)->relid;

			/* 若此标记类型需要访问关系，则打开关系    */
			switch (rc->markType)
			{
				case ROW_MARK_EXCLUSIVE:
				case ROW_MARK_NOKEYEXCLUSIVE:
				case ROW_MARK_SHARE:
				case ROW_MARK_KEYSHARE:
				case ROW_MARK_REFERENCE:
					relation = ExecGetRangeTableRelation(estate, rc->rti);
					break;
				case ROW_MARK_COPY:
					/* 无需物理表访问                */
					relation = NULL;
					break;
				default:
					elog(ERROR, "unrecognized markType: %d", rc->markType);
					relation = NULL;	/* 防编译器告警  */
					break;
			}

			/* 检查关系是否为标记的合法目标        */
			if (relation)
				CheckValidRowMarkRel(relation, rc->markType);

			erm = (ExecRowMark *) palloc(sizeof(ExecRowMark));
			erm->relation = relation;
			erm->relid = relid;
			erm->rti = rc->rti;
			erm->prti = rc->prti;
			erm->rowmarkId = rc->rowmarkId;
			erm->markType = rc->markType;
			erm->strength = rc->strength;
			erm->waitPolicy = rc->waitPolicy;
			erm->ermActive = false;
			ItemPointerSetInvalid(&(erm->curCtid));
			erm->ermExtra = NULL;

			Assert(erm->rti > 0 && erm->rti <= estate->es_range_table_size &&
				   estate->es_rowmarks[erm->rti - 1] == NULL);

			estate->es_rowmarks[erm->rti - 1] = erm;
		}
	}

	/*
	 * 将执行器的元组表初始化为空。     
	 */
	estate->es_tupleTable = NIL;

	/* 表明此 EState 不用于 EPQ              */
	estate->es_epq_active = NULL;

	/*
	 * 为每个 SubPlan 初始化私有状态信息。必须先于           
	 * 对主查询树的 ExecInitNode 执行此操作，因为 
	 * ExecInitSubPlan 期望能找到这些表项。           
	 */
	Assert(estate->es_subplanstates == NIL);
	i = 1;						/* 子计划索引自 1 起算  */
	foreach(l, plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;
		int			sp_eflags;

		/*
		 * 子计划绝不需要执行 BACKWARD 扫描或 MARK/RESTORE。    
		 * 如果它是无参数子计划（非 initplan），我们建议它 
		 * 准备好高效处理 REWIND；否则无此必要。              
		 */
		sp_eflags = eflags
			& ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
		if (bms_is_member(i, plannedstmt->rewindPlanIDs))
			sp_eflags |= EXEC_FLAG_REWIND;

		subplanstate = ExecInitNode(subplan, estate, sp_eflags);

		estate->es_subplanstates = lappend(estate->es_subplanstates,
										   subplanstate);

		i++;
	}

	/*
	 * 为查询树中所有节点初始化私有状态信息。这一步     
	 * 会打开文件、分配存储空间，并让我们准备就绪，     
	 * 以处理元组。
	 */
	planstate = ExecInitNode(plan, estate, eflags);

	/*
	 * 获取描述返回元组类型的元组描述符。              
	 */
	tupType = ExecGetResultType(planstate);

	/*
	 * 如需要则初始化垃圾过滤器。若顶层 tlist 中存在      
	 * 垃圾属性，则 SELECT 需要过滤器。    
	 */
	if (operation == CMD_SELECT)
	{
		bool		junk_filter_needed = false;
		ListCell   *tlist;

		foreach(tlist, plan->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist);

			if (tle->resjunk)
			{
				junk_filter_needed = true;
				break;
			}
		}

		if (junk_filter_needed)
		{
			JunkFilter *j;
			TupleTableSlot *slot;

			slot = ExecInitExtraTupleSlot(estate, NULL, &TTSOpsVirtual);
			j = ExecInitJunkFilter(planstate->plan->targetlist,
								   slot);
			estate->es_junkFilter = j;

			/* 希望返回清理后的元组类型  */
			tupType = j->jf_cleanTupType;
		}
	}

	queryDesc->tupDesc = tupType;
	queryDesc->planstate = planstate;
}

/*
 * 检查拟议的结果关系是否为该操作的合法目标             
 *
 * 一般来说解析器和/或规划器本应已发现任何此类        
 * 错误，但需再确认。  
 *
 * 对 INSERT ON CONFLICT 而言，无论冲突是否真的发生，     
 * 结果关系都必须支持 onConflictAction。                    
 *
 * 对 MERGE 而言，mergeActions 是可执行动作的列表。            
 * 结果关系必须支持其中每个动作，无论它们是否           
 * 全部会被执行。        
 *
 * 注意：修改此函数时，多半还需要查看                 
 * CheckValidRowMarkRel.
 */
void
CheckValidResultRelNew(ResultRelInfo *resultRelInfo, CmdType operation,
					   OnConflictAction onConflictAction, List *mergeActions)
{
	Relation	resultRel = resultRelInfo->ri_RelationDesc;
	TriggerDesc *trigDesc = resultRel->trigdesc;
	FdwRoutine *fdwroutine;

	/* InitResultRelInfo() 应给出成形完整的 ResultRelInfo。 */
	Assert(resultRelInfo->ri_needLockTagTuple ==
		   IsInplaceUpdateRelation(resultRel));

	switch (resultRel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:

			/*
			 * 对于 MERGE，检查目标关系是否支持每个动作。   
			 * 对于其他操作，只需检查该操作本身。   
			 */
			if (operation == CMD_MERGE)
			{
				ListCell   *lc;

				foreach(lc, mergeActions)
				{
					MergeAction *action = (MergeAction *) lfirst(lc);

					CheckCmdReplicaIdentity(resultRel, action->commandType);
				}
			}
			else
				CheckCmdReplicaIdentity(resultRel, operation);

			/*
			 * 对于 INSERT ON CONFLICT DO UPDATE，还要检查           
			 * 目标关系须支持 UPDATE。 
			 */
			if (onConflictAction == ONCONFLICT_UPDATE)
				CheckCmdReplicaIdentity(resultRel, CMD_UPDATE);
			break;
		case RELKIND_SEQUENCE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change sequence \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_TOASTVALUE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change TOAST relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_VIEW:

			/*
			 * 仅当存在合适的 INSTEAD OF 触发器方可。此处的 
			 * 消息应与 rewriteHandler.c 的 rewriteTargetView 及   
			 * RewriteQuery 一致，只是未包含 errdetail，因为手头
			 * 没有该信息（反正也不该到这里，不值得费力 
			 * 获取）。                                           
			 */
			switch (operation)
			{
				case CMD_INSERT:
					if (!trigDesc || !trigDesc->trig_insert_instead_row)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot insert into view \"%s\"",
										RelationGetRelationName(resultRel)),
								 errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.")));
					break;
				case CMD_UPDATE:
					if (!trigDesc || !trigDesc->trig_update_instead_row)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot update view \"%s\"",
										RelationGetRelationName(resultRel)),
								 errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.")));
					break;
				case CMD_DELETE:
					if (!trigDesc || !trigDesc->trig_delete_instead_row)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot delete from view \"%s\"",
										RelationGetRelationName(resultRel)),
								 errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.")));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d", (int) operation);
					break;
			}
			break;
		case RELKIND_MATVIEW:
			if (!MatViewIncrementalMaintenanceIsEnabled())
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot change materialized view \"%s\"",
								RelationGetRelationName(resultRel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* 仅当 FDW 支持时方可。    */
			fdwroutine = resultRelInfo->ri_FdwRoutine;
			switch (operation)
			{
				case CMD_INSERT:
					if (fdwroutine->ExecForeignInsert == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot insert into foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_INSERT)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow inserts",
										RelationGetRelationName(resultRel))));
					break;
				case CMD_UPDATE:
					if (fdwroutine->ExecForeignUpdate == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot update foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_UPDATE)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow updates",
										RelationGetRelationName(resultRel))));
					break;
				case CMD_DELETE:
					if (fdwroutine->ExecForeignDelete == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot delete from foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_DELETE)) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("foreign table \"%s\" does not allow deletes",
										RelationGetRelationName(resultRel))));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d", (int) operation);
					break;
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
	}
}

/*
 * 与 ABI 兼容的包装函数，模拟上述函数的旧版本。   
 * 新代码中请勿调用此版本。 
 */
void
CheckValidResultRel(ResultRelInfo *resultRelInfo, CmdType operation)
{
	CheckValidResultRelNew(resultRelInfo, operation, ONCONFLICT_NONE, NIL);
}

/*
 * 检查作为 rowmark 候选的目标关系是否合法。      
 *
 * 多数情况下解析器和/或规划器本应早已发现，但         
 * 并未覆盖全部情况。
 */
static void
CheckValidRowMarkRel(Relation rel, RowMarkType markType)
{
	FdwRoutine *fdwroutine;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			/* OK */
			break;
		case RELKIND_SEQUENCE:
			/* 必须禁止：我们不对序列执行 vacuum。    */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in sequence \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_TOASTVALUE:
			/* 本可允许，但似乎没有充分的理由。       */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in TOAST relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_VIEW:
			/* 不应到达此处；规划器应已展开该视图。     */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in view \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_MATVIEW:
			/* 允许引用物化视图，但禁止实际的锁定子句。*/
			if (markType != ROW_MARK_REFERENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot lock rows in materialized view \"%s\"",
								RelationGetRelationName(rel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* 仅当 FDW 支持时方可。    */
			fdwroutine = GetFdwRoutineForRelation(rel, false);
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(rel))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
	}
}

/*
 * 为结果关系初始化 ResultRelInfo 数据。     
 *
 * 注意：Postgres 9.1 之前，该函数含如今在 CheckValidResultRel 
 * 中的 relkind 检查，适当时候也会执行 ExecOpenIndices。  
 * 务必确保调用者满足这些需求。      
 */
void
InitResultRelInfo(ResultRelInfo *resultRelInfo,
				  Relation resultRelationDesc,
				  Index resultRelationIndex,
				  ResultRelInfo *partition_root_rri,
				  int instrument_options)
{
	MemSet(resultRelInfo, 0, sizeof(ResultRelInfo));
	resultRelInfo->type = T_ResultRelInfo;
	resultRelInfo->ri_RangeTableIndex = resultRelationIndex;
	resultRelInfo->ri_RelationDesc = resultRelationDesc;
	resultRelInfo->ri_NumIndices = 0;
	resultRelInfo->ri_IndexRelationDescs = NULL;
	resultRelInfo->ri_IndexRelationInfo = NULL;
	resultRelInfo->ri_needLockTagTuple =
		IsInplaceUpdateRelation(resultRelationDesc);
	/* 复制一份，以免依赖 relcache 信息保持不变……    */
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(resultRelationDesc->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
	{
		int			n = resultRelInfo->ri_TrigDesc->numtriggers;

		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(n * sizeof(FmgrInfo));
		resultRelInfo->ri_TrigWhenExprs = (ExprState **)
			palloc0(n * sizeof(ExprState *));
		if (instrument_options)
			resultRelInfo->ri_TrigInstrument = InstrAlloc(n, instrument_options, false);
	}
	else
	{
		resultRelInfo->ri_TrigFunctions = NULL;
		resultRelInfo->ri_TrigWhenExprs = NULL;
		resultRelInfo->ri_TrigInstrument = NULL;
	}
	if (resultRelationDesc->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		resultRelInfo->ri_FdwRoutine = GetFdwRoutineForRelation(resultRelationDesc, true);
	else
		resultRelInfo->ri_FdwRoutine = NULL;

	/* 以下字段稍后在需要时设置。      */
	resultRelInfo->ri_RowIdAttNo = 0;
	resultRelInfo->ri_extraUpdatedCols = NULL;
	resultRelInfo->ri_projectNew = NULL;
	resultRelInfo->ri_newTupleSlot = NULL;
	resultRelInfo->ri_oldTupleSlot = NULL;
	resultRelInfo->ri_projectNewInfoValid = false;
	resultRelInfo->ri_FdwState = NULL;
	resultRelInfo->ri_usesFdwDirectModify = false;
	resultRelInfo->ri_ConstraintExprs = NULL;
	resultRelInfo->ri_GeneratedExprsI = NULL;
	resultRelInfo->ri_GeneratedExprsU = NULL;
	resultRelInfo->ri_projectReturning = NULL;
	resultRelInfo->ri_onConflictArbiterIndexes = NIL;
	resultRelInfo->ri_onConflict = NULL;
	resultRelInfo->ri_ReturningSlot = NULL;
	resultRelInfo->ri_TrigOldSlot = NULL;
	resultRelInfo->ri_TrigNewSlot = NULL;
	resultRelInfo->ri_matchedMergeAction = NIL;
	resultRelInfo->ri_notMatchedMergeAction = NIL;

	/*
	 * 仅 ExecInitPartitionInfo() 和 ExecInitPartitionDispatchInfo()      
	 * 会传入非 NULL 的 partition_root_rri。对于初始查询中      
	 * 的子关系（而非由元组路由动态加入的），该字段 
	 * ExecInitModifyTable() 中填充。            
	 */
	resultRelInfo->ri_RootResultRelInfo = partition_root_rri;
	/* ExecGetRootToChildMap 设置 */
	resultRelInfo->ri_RootToChildMap = NULL;
	resultRelInfo->ri_RootToChildMapValid = false;
	/* ExecInitRoutingInfo 设置 */
	resultRelInfo->ri_PartitionTupleSlot = NULL;
	resultRelInfo->ri_ChildToRootMap = NULL;
	resultRelInfo->ri_ChildToRootMapValid = false;
	resultRelInfo->ri_CopyMultiInsertBuffer = NULL;
}

/*
 * ExecGetTriggerResultRel
 *		获取触发器目标关系的 ResultRelInfo。   
 *
 * 通常触发器都触发在查询的某个结果关系上，此时可     
 * 直接返回已建好并存入 es_opened_result_relations 或             
 * es_tuple_routing_result_relations 列表中的现成                  
 * 项。
 *
 * 但有时需要在其他关系上触发触发器；                    
 * 这多发生在 RI 更新触发器把额外触发器排入其他关系  
 * 的队列时，它们在外层查询上下文中处理。为提高效  
 * 率起见，我们也希望为这些触发器准备 ResultRelInfo，    
 * 从而避免反复重新打开关系。（这也为 EXPLAIN ANALYZE   
 * 提供了报告这类触发器运行时间的渠道。）             
 * 所以我们按需创建额外的 ResultRelInfo，并保存到            
 * es_trig_target_relations    
 */
ResultRelInfo *
ExecGetTriggerResultRel(EState *estate, Oid relid,
						ResultRelInfo *rootRelInfo)
{
	ResultRelInfo *rInfo;
	ListCell   *l;
	Relation	rel;
	MemoryContext oldcontext;

	/*
	 * 创建新的 ResultRelInfo 之前，检查是否为该关系建立  
	 * 并缓存过它的 ResultRelInfo。必须确保所给的    
	 * 'rootRelInfo' 与缓存 ResultRelInfo 中保存的那个一致，  
	 * 因为分区触发器处理可能导致 ri_RootResultRelInfo 的    
	 * 取值出现不一致的需求。   
	 */

	/* 遍历查询结果关系列表。         */
	foreach(l, estate->es_opened_result_relations)
	{
		rInfo = lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}

	/*
	 * 遍历在元组路由期间创建的那些结果关系，         
	 * 若有的话。 
	 */
	foreach(l, estate->es_tuple_routing_result_relations)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}

	/* 没有；但可能已为它建过额外的 ResultRelInfo。   */
	foreach(l, estate->es_trig_target_relations)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid &&
			rInfo->ri_RootResultRelInfo == rootRelInfo)
			return rInfo;
	}
	/* 没有，需新建一个。*/

	/*
	 * 打开目标关系的 relcache 表项。我们假定自从   
	 * 触发事件入队时起，后端持有适当的锁，故此处无需  
	 * 再取新锁。另外也无需重新检查 relkind，不必再调用  
	 * CheckValidResultRel。                                  
	 */
	rel = table_open(relid, NoLock);

	/*
	 * 在合适的上下文中新建表项。 
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	rInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(rInfo,
					  rel,
					  0,		/* 占位范围表索引  */
					  rootRelInfo,
					  estate->es_instrument);
	estate->es_trig_target_relations =
		lappend(estate->es_trig_target_relations, rInfo);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 仅用于触发器的 ResultRelInfo 不需要索引信息，         
	 * 因此无需调用 ExecOpenIndices。                 
	 */

	return rInfo;
}

/*
 * 返回给定叶子分区结果关系的祖先关系，直至并包含  
 * 查询的根目标关系。                          
 *
 * 它们与 ExecGetTriggerResultRel 打开的那些十分相似，只    
 * 是需将它们存入单独的列表。      
 *
 * 由 ExecCloseResultRelations 关闭。       
 */
List *
ExecGetAncestorResultRels(EState *estate, ResultRelInfo *resultRelInfo)
{
	ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;
	Relation	partRel = resultRelInfo->ri_RelationDesc;
	Oid			rootRelOid;

	if (!partRel->rd_rel->relispartition)
		elog(ERROR, "cannot find ancestors of a non-partition result relation");
	Assert(rootRelInfo != NULL);
	rootRelOid = RelationGetRelid(rootRelInfo->ri_RelationDesc);
	if (resultRelInfo->ri_ancestorResultRels == NIL)
	{
		ListCell   *lc;
		List	   *oids = get_partition_ancestors(RelationGetRelid(partRel));
		List	   *ancResultRels = NIL;

		foreach(lc, oids)
		{
			Oid			ancOid = lfirst_oid(lc);
			Relation	ancRel;
			ResultRelInfo *rInfo;

			/*
			 * 忽略根祖先，改用 ri_RootResultRelInfo（见下）  
			 * 来代替。另外，一旦找到查询中所提到的
			 * 表时就停止上                                        
			 * 溯。
			 */
			if (ancOid == rootRelOid)
				break;

			/*
			 * 至根目标关系的所有祖先均应已由规划器或  
			 * AcquireExecutorLocks() 锁定。                
			 */
			ancRel = table_open(ancOid, NoLock);
			rInfo = makeNode(ResultRelInfo);

			/* 占位范围表索引  */
			InitResultRelInfo(rInfo, ancRel, 0, NULL,
							  estate->es_instrument);
			ancResultRels = lappend(ancResultRels, rInfo);
		}
		ancResultRels = lappend(ancResultRels, rootRelInfo);
		resultRelInfo->ri_ancestorResultRels = ancResultRels;
	}

	/* 必定已找到某个祖先。   */
	Assert(resultRelInfo->ri_ancestorResultRels != NIL);

	return resultRelInfo->ri_ancestorResultRels;
}

/* ----------------------------------------------------------------
 *		ExecPostprocessPlan
 *
 *		在关闭前给计划节点最后一次执行机会。   
 * ----------------------------------------------------------------
 */
static void
ExecPostprocessPlan(EState *estate)
{
	ListCell   *lc;

	/*
	 * 确保节点向前运行。 
	 */
	estate->es_direction = ForwardScanDirection;

	/*
	 * 将次要 ModifyTable 节点运行完毕，以防主查询未取   
	 * 其全部行。（这样做是为了确保此类节点具有       
	 * 可预期的结果。）             
	 */
	foreach(lc, estate->es_auxmodifytables)
	{
		PlanState  *ps = (PlanState *) lfirst(lc);

		for (;;)
		{
			TupleTableSlot *slot;

			/* 每次重置每个输出元组的 exprcontext。 */
			ResetPerTupleExprContext(estate);

			slot = ExecProcNode(ps);

			if (TupIsNull(slot))
				break;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecEndPlan
 *
 *		清理查询计划——关闭文件并释放存储空间。 
 *
 * 注意：已无需担心释放存储本身；FreeExecutorState  
 * 应保证释放所有需要释放的内存。我们所             
 * 要做的是关闭关系并丢弃缓冲区 pin。因此，        
 * 例如，元组表必须被清除或丢弃，以确保            
 * pin 得以释放。                                                 
 * ----------------------------------------------------------------
 */
static void
ExecEndPlan(PlanState *planstate, EState *estate)
{
	ListCell   *l;

	/*
	 * 关闭节点类型专有的查询处理。       
	 */
	ExecEndNode(planstate);

	/*
	 * 子计划亦然 
	 */
	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/*
	 * 销毁执行器的元组表。实际只关心缓冲区钉住与 
	 * tupdesc 引用计数；无需 pfree 那些 TupleTableSlot，因为   
	 * 包含它们的内存上下文无论如何都即将消失              
	 * 了。      
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/*
	 * 关闭所有为范围表表项或结果关系而打开的           
	 * 关系。        
	 */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);
}

/*
 * 关闭所有为 ResultRelInfo 而打开的关系。          
 */
void
ExecCloseResultRelations(EState *estate)
{
	ListCell   *l;

	/*
	 * 关闭结果关系（若有）的索引。（关系本身则由  
	 * ExecCloseRangeTableRelations() 关闭） 
	 *
	 * 另外还要关闭各 resultrel 的 ri_ancestorResultRels 中  
	 * 存在的占位 RT。 
	 */
	foreach(l, estate->es_opened_result_relations)
	{
		ResultRelInfo *resultRelInfo = lfirst(l);
		ListCell   *lc;

		ExecCloseIndices(resultRelInfo);
		foreach(lc, resultRelInfo->ri_ancestorResultRels)
		{
			ResultRelInfo *rInfo = lfirst(lc);

			/*
			 * RTI > 0 的祖先（应只有根祖先）则由              
			 * ExecCloseRangeTableRelations 关闭。 
			 */
			if (rInfo->ri_RangeTableIndex > 0)
				continue;

			table_close(rInfo->ri_RelationDesc, NoLock);
		}
	}

	/* 关闭所有曾由 ExecGetTriggerResultRel() 打开的关系。         */
	foreach(l, estate->es_trig_target_relations)
	{
		ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l);

		/*
		 * 断言这是"哑" ResultRelInfo，见上文。否则我们可能
		 * 对 ExecGetRangeTableRelation 打开的关系发出重复      
		 * 的关闭。              
		 */
		Assert(resultRelInfo->ri_RangeTableIndex == 0);

		/*
		 * 由于 ExecGetTriggerResultRel 不会对这些关系调用    
		 * ExecOpenIndices，故也无需 ExecCloseIndices。  
		 */
		Assert(resultRelInfo->ri_NumIndices == 0);

		table_close(resultRelInfo->ri_RelationDesc, NoLock);
	}
}

/*
 * 关闭 ExecGetRangeTableRelation() 打开的关系。     
 *
 * 不会释放这些关系上可能持有的任何锁。  
 */
void
ExecCloseRangeTableRelations(EState *estate)
{
	int			i;

	for (i = 0; i < estate->es_range_table_size; i++)
	{
		if (estate->es_relations[i])
			table_close(estate->es_relations[i], NoLock);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		处理查询计划，直到取回 'numberTuples' 个元组为止，    
 *		沿指定方向推进。          
 *
 *		numberTuples 为 0 时运行到完成  
 * ----------------------------------------------------------------
 */
static void
ExecutePlan(QueryDesc *queryDesc,
			CmdType operation,
			bool sendTuples,
			uint64 numberTuples,
			ScanDirection direction,
			DestReceiver *dest)
{
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate = queryDesc->planstate;
	bool		use_parallel_mode;
	TupleTableSlot *slot;
	uint64		current_tuple_count;

	/*
	 * 初始化局部变量     
	 */
	current_tuple_count = 0;

	/*
	 * 设置方向。   
	 */
	estate->es_direction = direction;

	/*
	 * 如合适则设置并行模式。   
	 *
	 * 并行模式只支持完整执行计划。如果我们已经部分 
	 * 执行过该计划，或调用者要求提前退出，则必须强制 
	 * 计划以非并行方式运行。                 
	 */
	if (queryDesc->already_executed || numberTuples != 0)
		use_parallel_mode = false;
	else
		use_parallel_mode = queryDesc->plannedstmt->parallelModeNeeded;
	queryDesc->already_executed = true;

	estate->es_use_parallel_mode = use_parallel_mode;
	if (use_parallel_mode)
		EnterParallelMode();

	/*
	 * 循环，直到从计划中处理了应有数量的元组为止。   
	 */
	for (;;)
	{
		/* 重置每输出元组的 exprcontext。*/
		ResetPerTupleExprContext(estate);

		/*
		 * 执行计划并获得一个元组  
		 */
		slot = ExecProcNode(planstate);

		/*
		 * 若元组为空，则假定已没有更多要处理的内    
		 * 容，就直接结束循环…… 
		 */
		if (TupIsNull(slot))
			break;

		/*
		 * 若有垃圾过滤器，则投影出移除了垃圾的新元    
		 * 组。  
		 *
		 * 将新的"干净"元组存入 junkfilter 的 resultSlot。  
		 * （以前把它存回"脏"元组之上，那是错误的，因为  
		 * 该元组槽的描述符是错的。）           
		 */
		if (estate->es_junkFilter != NULL)
			slot = ExecFilterJunk(estate->es_junkFilter, slot);

		/*
		 * 若应当把元组发送到某处，就发送。（实际 
		 * 上此时很可能总是这种情况。）                
		 */
		if (sendTuples)
		{
			/*
			 * 如果元组无法发送出去，我们假定目标已关闭、
			 * 不再会有元组可发。若是如此，则就此结束     
			 * 循环。    
			 */
			if (!dest->receiveSlot(slot, dest))
				break;
		}

		/*
		 * 若为 SELECT 则统计已处理的元组数。至于其他操    
		 * 作类型，ModifyTable 计划节点必须统计相应的  
		 * 事件  
		 */
		if (operation == CMD_SELECT)
			(estate->es_processed)++;

		/*
		 * 检查元组计数……若已处理完应处理的数量则退  
		 * 出，否则循环，处理更多元组。numberTuples 为零    
		 * 即无限制。
		 */
		current_tuple_count++;
		if (numberTuples && numberTuples == current_tuple_count)
			break;
	}

	/*
	 * 如果我们知道不需要回退，就可以在此处释放资      
	 * 源。
	 */
	if (!(estate->es_top_eflags & EXEC_FLAG_BACKWARD))
		ExecShutdownNode(planstate);

	if (use_parallel_mode)
		ExitParallelMode();
}


/*
 * ExecRelCheck --- 检查元组是否满足结果关系的约束         
 *
 * 成功返回 NULL，否则为失败的检查约束名。
 */
static const char *
ExecRelCheck(ResultRelInfo *resultRelInfo,
			 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	int			ncheck = rel->rd_att->constr->num_check;
	ConstrCheck *check = rel->rd_att->constr->check;
	ExprContext *econtext;
	MemoryContext oldContext;
	int			i;

	/*
	 * CheckConstraintFetch 此前只发警告就放行，但现在我们
	 * 应当报错，而非可能无法强制执行一条重要的约 
	 * 束。     
	 */
	if (ncheck != rel->rd_rel->relchecks)
		elog(ERROR, "%d pg_constraint record(s) missing for relation \"%s\"",
			 rel->rd_rel->relchecks - ncheck, RelationGetRelationName(rel));

	/*
	 * 如果第一次经过该结果关系，则为其约束表达式 
	 * 构建表达式节点树。将它们保存在查询生存期的内存  
	 * 上下文中，使其能在整个查询期间存续。 
	 */
	if (resultRelInfo->ri_ConstraintExprs == NULL)
	{
		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		resultRelInfo->ri_ConstraintExprs =
			(ExprState **) palloc(ncheck * sizeof(ExprState *));
		for (i = 0; i < ncheck; i++)
		{
			Expr	   *checkconstr;

			checkconstr = stringToNode(check[i].ccbin);
			resultRelInfo->ri_ConstraintExprs[i] =
				ExecPrepareExpr(checkconstr, estate);
		}
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束表达式
	 * （若尚不存在则创建它）。                
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 让 econtext 的扫描元组成为受测元组。            */
	econtext->ecxt_scantuple = slot;

	/* 然后求值这些约束。  */
	for (i = 0; i < ncheck; i++)
	{
		ExprState  *checkconstr = resultRelInfo->ri_ConstraintExprs[i];

		/*
		 * 注意：SQL 规定约束表达式得出的 NULL 结果不算      
		 * 失败。因此要改用 ExecCheck 而非                       
		 * ExecQual.
		 */
		if (!ExecCheck(checkconstr, econtext))
			return check[i].ccname;
	}

	/* NULL 结果即无错误。 */
	return NULL;
}

/*
 * ExecPartitionCheck --- 检查元组是否满足分区约束。         
 *
 * 满足分区约束则返回真。如果约束不满足且被要求   
 * 报错，则报出错误且不返回；否则返回                      
 * 假。       
 */
bool
ExecPartitionCheck(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
				   EState *estate, bool emitError)
{
	ExprContext *econtext;
	bool		success;

	/*
	 * 如果是第一次经过，则为分区检查表达式构建表达  
	 * 式状态树。（分区检查表达式为空的情况下，即 
	 * 有默认分区而没有其他分区时，我们会被骗得每次都
	 * 执行这段代码。但该情况下开销非常小，所以不必   
	 * 为此担心。）                                          
	 */
	if (resultRelInfo->ri_PartitionCheckExpr == NULL)
	{
		/*
		 * 确保 qual 树和预备表达式位于查询生存期的   
		 * 上下文中。        
		 */
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		List	   *qual = RelationGetPartitionQual(resultRelInfo->ri_RelationDesc);

		resultRelInfo->ri_PartitionCheckExpr = ExecPrepareCheck(qual, estate);
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束表达式
	 * （若尚不存在则创建它）。                
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 让 econtext 的扫描元组成为受测元组。            */
	econtext->ecxt_scantuple = slot;

	/*
	 * 与编目约束的情形一样，这里我们把 NULL 结果视为 
	 * 成功而非失败。       
	 */
	success = ExecCheck(resultRelInfo->ri_PartitionCheckExpr, econtext);

	/* 若被要求报错，失败时并不真正返回。      */
	if (!success && emitError)
		ExecPartitionCheckEmitError(resultRelInfo, slot, estate);

	return success;
}

/*
 * ExecPartitionCheckEmitError - 在分区约束检查失败后构造并发出
 * 错误消息。            
 */
void
ExecPartitionCheckEmitError(ResultRelInfo *resultRelInfo,
							TupleTableSlot *slot,
							EState *estate)
{
	Oid			root_relid;
	TupleDesc	tupdesc;
	char	   *val_desc;
	Bitmapset  *modifiedCols;

	/*
	 * 如果元组已经过路由，则它已被转换为分区的行类  
	 * 型，可能与根表的不同。我们必须把它转换回根表的 
	 * 行类型，使错误消息中的 val_desc 与输入元组            
	 * 一致。               
	 */
	if (resultRelInfo->ri_RootResultRelInfo)
	{
		ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
		TupleDesc	old_tupdesc;
		AttrMap    *map;

		root_relid = RelationGetRelid(rootrel->ri_RelationDesc);
		tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);

		old_tupdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		/* 反向映射  */
		map = build_attrmap_by_name_if_req(old_tupdesc, tupdesc, false);

		/*
		 * 分区专属槽的 tupdesc 不能改变，所以要新分配     
		 * 一个  
		 */
		if (map != NULL)
			slot = execute_attr_map_slot(map, slot,
										 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
		modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
								 ExecGetUpdatedCols(rootrel, estate));
	}
	else
	{
		root_relid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
		tupdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
								 ExecGetUpdatedCols(resultRelInfo, estate));
	}

	val_desc = ExecBuildSlotValueDescription(root_relid,
											 slot,
											 tupdesc,
											 modifiedCols,
											 64);
	ereport(ERROR,
			(errcode(ERRCODE_CHECK_VIOLATION),
			 errmsg("new row for relation \"%s\" violates partition constraint",
					RelationGetRelationName(resultRelInfo->ri_RelationDesc)),
			 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
			 errtable(resultRelInfo->ri_RelationDesc)));
}

/*
 * ExecConstraints - 检查 'slot' 中元组的约束        
 *
 * 这里检查传统的 NOT NULL 与检查约束。          
 *
 * 分区约束*不会*被检查。          
 *
 * 注意：'slot' 包含要检查约束的元组，该元组可能在   
 * 元组路由之后经过转换，而非原始输入元组。          
 * 'resultRelInfo' 是元组路由之后的最终结果关系。     
 */
void
ExecConstraints(ResultRelInfo *resultRelInfo,
				TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TupleConstr *constr = tupdesc->constr;
	Bitmapset  *modifiedCols;

	Assert(constr);				/* 否则不应被这样调用。    */

	if (constr->has_not_null)
	{
		int			natts = tupdesc->natts;
		int			attrChk;

		for (attrChk = 1; attrChk <= natts; attrChk++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, attrChk - 1);

			if (att->attnotnull && slot_attisnull(slot, attrChk))
			{
				char	   *val_desc;
				Relation	orig_rel = rel;
				TupleDesc	orig_tupdesc = RelationGetDescr(rel);

				/*
				 * 若元组经过路由，则它已被转换为分区的  
				 * 行类型，可能与根表不同，须转回根表  
				 * 的行类型，使错误消息中显示的 val_desc  
				 * 与输入的元组                                      
				 * 一致。   
				 */
				if (resultRelInfo->ri_RootResultRelInfo)
				{
					ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
					AttrMap    *map;

					tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
					/* 反向映射  */
					map = build_attrmap_by_name_if_req(orig_tupdesc,
													   tupdesc,
													   false);

					/*
					 * 分区专属槽的 tupdesc 不能改变，因此      
					 * 要分配新的。 
					 */
					if (map != NULL)
						slot = execute_attr_map_slot(map, slot,
													 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
					modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
											 ExecGetUpdatedCols(rootrel, estate));
					rel = rootrel->ri_RelationDesc;
				}
				else
					modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
											 ExecGetUpdatedCols(resultRelInfo, estate));
				val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
														 slot,
														 tupdesc,
														 modifiedCols,
														 64);

				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("null value in column \"%s\" of relation \"%s\" violates not-null constraint",
								NameStr(att->attname),
								RelationGetRelationName(orig_rel)),
						 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
						 errtablecol(orig_rel, attrChk)));
			}
		}
	}

	if (rel->rd_rel->relchecks > 0)
	{
		const char *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
		{
			char	   *val_desc;
			Relation	orig_rel = rel;

			/* 参见上文注释。  */
			if (resultRelInfo->ri_RootResultRelInfo)
			{
				ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
				TupleDesc	old_tupdesc = RelationGetDescr(rel);
				AttrMap    *map;

				tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
				/* 反向映射  */
				map = build_attrmap_by_name_if_req(old_tupdesc,
												   tupdesc,
												   false);

				/*
				 * 分区专属槽的 tupdesc 不能改变，因此      
				 * 要分配新的。 
				 */
				if (map != NULL)
					slot = execute_attr_map_slot(map, slot,
												 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));
				modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
										 ExecGetUpdatedCols(rootrel, estate));
				rel = rootrel->ri_RelationDesc;
			}
			else
				modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
										 ExecGetUpdatedCols(resultRelInfo, estate));
			val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
													 slot,
													 tupdesc,
													 modifiedCols,
													 64);
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("new row for relation \"%s\" violates check constraint \"%s\"",
							RelationGetRelationName(orig_rel), failed),
					 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
					 errtableconstraint(orig_rel, failed)));
		}
	}
}

/*
 * ExecWithCheckOptions -- 检查元组是否满足指定类型的          
 * WITH CHECK OPTION。  
 *
 * 注意，需要多次调用本函数，以确保所有种类的             
 * WITH CHECK OPTION 都得到处理（包括来自设置了 WITH           
 * CHECK OPTION 的视图也包括行级安全策略）。见 ExecInsert()  
 * 和 ExecUpdate() 
 */
void
ExecWithCheckOptions(WCOKind kind, ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ExprContext *econtext;
	ListCell   *l1,
			   *l2;

	/*
	 * 我们将使用 EState 的每元组上下文来求值约束         
	 * 表达式（如果尚不存在则创建之）。    
	 */
	econtext = GetPerTupleExprContext(estate);

	/* 让 econtext 的扫描元组成为被测元组               */
	econtext->ecxt_scantuple = slot;

	/* 检查每一个约束         */
	forboth(l1, resultRelInfo->ri_WithCheckOptions,
			l2, resultRelInfo->ri_WithCheckOptionExprs)
	{
		WithCheckOption *wco = (WithCheckOption *) lfirst(l1);
		ExprState  *wcoExpr = (ExprState *) lfirst(l2);

		/*
		 * 跳过并非我们此刻所需种类的                        
		 * WCO  
		 */
		if (wco->kind != kind)
			continue;

		/*
		 * WITH CHECK OPTION 检查旨在确保新元组                     
		 * 可见（对视图而言），或者能通过           
		 * 'with-check' 策略（对行安全而言）。如果 qual     
		 * 求值为 NULL 或 FALSE，那么新元组将不会                
		 * 进入视图，或未通过该表的 'with-check' 策略。    
		 */
		if (!ExecQual(wcoExpr, econtext))
		{
			char	   *val_desc;
			Bitmapset  *modifiedCols;

			switch (wco->kind)
			{
					/*
					 * 对于来自视图的 WITH CHECK OPTION，我们     
					 * 或能给出该行的细节，视关系权限而定 
					 * （即用户本就能直接看它）。RLS 违规    
					 * 我们不包含数据，因为不知道用户          
					 * 是否应能查看该元组，因为这取决于      
					 * USING                                                  
					 * 策略。    
					 */
				case WCO_VIEW_CHECK:
					/* 参见 ExecConstraints() 中的注释 */
					if (resultRelInfo->ri_RootResultRelInfo)
					{
						ResultRelInfo *rootrel = resultRelInfo->ri_RootResultRelInfo;
						TupleDesc	old_tupdesc = RelationGetDescr(rel);
						AttrMap    *map;

						tupdesc = RelationGetDescr(rootrel->ri_RelationDesc);
						/* 反向映射  */
						map = build_attrmap_by_name_if_req(old_tupdesc,
														   tupdesc,
														   false);

						/*
						 * 分区专有槽的 tupdesc 不能改动，         
						 * 故需另行分配。 
						 */
						if (map != NULL)
							slot = execute_attr_map_slot(map, slot,
														 MakeTupleTableSlot(tupdesc, &TTSOpsVirtual));

						modifiedCols = bms_union(ExecGetInsertedCols(rootrel, estate),
												 ExecGetUpdatedCols(rootrel, estate));
						rel = rootrel->ri_RelationDesc;
					}
					else
						modifiedCols = bms_union(ExecGetInsertedCols(resultRelInfo, estate),
												 ExecGetUpdatedCols(resultRelInfo, estate));
					val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
															 slot,
															 tupdesc,
															 modifiedCols,
															 64);

					ereport(ERROR,
							(errcode(ERRCODE_WITH_CHECK_OPTION_VIOLATION),
							 errmsg("new row violates check option for view \"%s\"",
									wco->relname),
							 val_desc ? errdetail("Failing row contains %s.",
												  val_desc) : 0));
					break;
				case WCO_RLS_INSERT_CHECK:
				case WCO_RLS_UPDATE_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy \"%s\" for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy for table \"%s\"",
										wco->relname)));
					break;
				case WCO_RLS_MERGE_UPDATE_CHECK:
				case WCO_RLS_MERGE_DELETE_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("target row violates row-level security policy \"%s\" (USING expression) for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("target row violates row-level security policy (USING expression) for table \"%s\"",
										wco->relname)));
					break;
				case WCO_RLS_CONFLICT_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy \"%s\" (USING expression) for table \"%s\"",
										wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("new row violates row-level security policy (USING expression) for table \"%s\"",
										wco->relname)));
					break;
				default:
					elog(ERROR, "unrecognized WCO kind: %u", wco->kind);
					break;
			}
		}
	}
}

/*
 * ExecBuildSlotValueDescription -- 构造一个表示元组的字符串   
 *
 * 本函数有意写得与 BuildIndexValueDescription 非常相似，  
 * 但不同的是，我们截断过长的字段值（最多 maxfieldlen      
 * 字节）。这似乎是必要的，因为堆字段值可能非常长，
 * 而索引条目通常没有这么宽。              
 *
 * 此外，不同于索引条目，我们还须忽略被删除的列。     
 * 过去用槽的元组描述符解码数据，但它无法标识           
 * 被删除的列，所以现在需要把关系的                       
 * 描述符传进来。                           
 *
 * 注意，与 BuildIndexValueDescription 一样，如果用户没有  
 * 权限查看任何一列，则返回 NULL。不同的是，若用户       
 * 有权查看其中一部分列，则会返回该子集，并带有        
 * 一个键，用以标识                                                  
 * 哪些列。     
 */
static char *
ExecBuildSlotValueDescription(Oid reloid,
							  TupleTableSlot *slot,
							  TupleDesc tupdesc,
							  Bitmapset *modifiedCols,
							  int maxfieldlen)
{
	StringInfoData buf;
	StringInfoData collist;
	bool		write_comma = false;
	bool		write_comma_collist = false;
	int			i;
	AclResult	aclresult;
	bool		table_perm = false;
	bool		any_perm = false;

	/*
	 * 检查 RLS 是否已启用且应对该关系生效；若是，       
	 * 则不返回任何内容。否则走正常的权限                 
	 * 检查 
	 */
	if (check_enable_rls(reloid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	/*
	 * 检查用户是否有权查看该行。表级 SELECT 允许访问    
	 * 所有列。若用户没有表级 SELECT，我们就逐列检查， 
	 * 并纳入用户有 SELECT 权限的列。此外，我们总是纳入  
	 * 用户提供了数据的那些                                       
	 * 列。   
	 */
	aclresult = pg_class_aclcheck(reloid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/* 为列清单准备好缓冲区        */
		initStringInfo(&collist);
		appendStringInfoChar(&collist, '(');
	}
	else
		table_perm = any_perm = true;

	/* 确保元组已被完整解构             */
	slot_getallattrs(slot);

	for (i = 0; i < tupdesc->natts; i++)
	{
		bool		column_perm = false;
		char	   *val;
		int			vallen;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* 忽略被删除的列  */
		if (att->attisdropped)
			continue;

		if (!table_perm)
		{
			/*
			 * 没有表级 SELECT，需确保用户要么有该列的     
			 * SELECT 权限，要么已提供该列的数据。否则，     
			 * 如果不满足，则从错误信息中省略           
			 * 该列  
			 */
			aclresult = pg_attribute_aclcheck(reloid, att->attnum,
											  GetUserId(), ACL_SELECT);
			if (bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols) || aclresult == ACLCHECK_OK)
			{
				column_perm = any_perm = true;

				if (write_comma_collist)
					appendStringInfoString(&collist, ", ");
				else
					write_comma_collist = true;

				appendStringInfoString(&collist, NameStr(att->attname));
			}
		}

		if (table_perm || column_perm)
		{
			if (slot->tts_isnull[i])
				val = "null";
			else
			{
				Oid			foutoid;
				bool		typisvarlena;

				getTypeOutputInfo(att->atttypid,
								  &foutoid, &typisvarlena);
				val = OidOutputFunctionCall(foutoid, slot->tts_values[i]);
			}

			if (write_comma)
				appendStringInfoString(&buf, ", ");
			else
				write_comma = true;

			/* 按需截断       */
			vallen = strlen(val);
			if (vallen <= maxfieldlen)
				appendBinaryStringInfo(&buf, val, vallen);
			else
			{
				vallen = pg_mbcliplen(val, vallen, maxfieldlen);
				appendBinaryStringInfo(&buf, val, vallen);
				appendStringInfoString(&buf, "...");
			}
		}
	}

	/* 如果最终返回的列数为零，则返回 NULL。            */
	if (!any_perm)
		return NULL;

	appendStringInfoChar(&buf, ')');

	if (!table_perm)
	{
		appendStringInfoString(&collist, ") = ");
		appendBinaryStringInfo(&collist, buf.data, buf.len);

		return collist.data;
	}

	return buf.data;
}


/*
 * ExecUpdateLockMode -- 为给定的 ResultRelInfo 找出合适的 UPDATE
 * 元组锁模式    
 */
LockTupleMode
ExecUpdateLockMode(EState *estate, ResultRelInfo *relinfo)
{
	Bitmapset  *keyCols;
	Bitmapset  *updatedCols;

	/*
	 * 计算要用的锁模式。若键中的列未被修改，              
	 * 那么就能用较弱的锁，从而获得更好的              
	 * 并发性。
	 */
	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);
	keyCols = RelationGetIndexAttrBitmap(relinfo->ri_RelationDesc,
										 INDEX_ATTR_BITMAP_KEY);

	if (bms_overlap(keyCols, updatedCols))
		return LockTupleExclusive;

	return LockTupleNoKeyExclusive;
}

/*
 * ExecFindRowMark -- 找给定范围表索引的 ExecRowMark 结构体     
 *
 * 若无该结构体，则依据 missing_ok 返回 NULL 或抛出错误       
 */
ExecRowMark *
ExecFindRowMark(EState *estate, Index rti, bool missing_ok)
{
	if (rti > 0 && rti <= estate->es_range_table_size &&
		estate->es_rowmarks != NULL)
	{
		ExecRowMark *erm = estate->es_rowmarks[rti - 1];

		if (erm)
			return erm;
	}
	if (!missing_ok)
		elog(ERROR, "failed to find ExecRowMark for rangetable index %u", rti);
	return NULL;
}

/*
 * ExecBuildAuxRowMark -- 创建 ExecAuxRowMark 结构体
 *
 * 输入为底层 ExecRowMark 结构体与输入计划节点的         
 * targetlist（不是 planstate 节点！）。后者用于找出       
 * 各 resjunk 列的列号。               
 */
ExecAuxRowMark *
ExecBuildAuxRowMark(ExecRowMark *erm, List *targetlist)
{
	ExecAuxRowMark *aerm = (ExecAuxRowMark *) palloc0(sizeof(ExecAuxRowMark));
	char		resname[32];

	aerm->rowmark = erm;

	/* 查找与该 rowmark 关联的 resjunk 列               */
	if (erm->markType != ROW_MARK_COPY)
	{
		/* 除 COPY 外的所有方法都需要 ctid */
		snprintf(resname, sizeof(resname), "ctid%u", erm->rowmarkId);
		aerm->ctidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->ctidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}
	else
	{
		/* COPY 时需 wholerow  */
		snprintf(resname, sizeof(resname), "wholerow%u", erm->rowmarkId);
		aerm->wholeAttNo = ExecFindJunkAttributeInTlist(targetlist,
														resname);
		if (!AttributeNumberIsValid(aerm->wholeAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	/* 子关系时需要 tableoid */
	if (erm->rti != erm->prti)
	{
		snprintf(resname, sizeof(resname), "tableoid%u", erm->rowmarkId);
		aerm->toidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->toidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	return aerm;
}


/*
 * EvalPlanQual 逻辑 --- 重新检查被修改的元组，看我们   
 * 是否要按 READ COMMITTED 规则处理更新版本   
 *
 * 其工作原理的说明参见 backend/executor/README。      
 */


/*
 * 检查元组的更新后版本，以确定是否要按 READ COMMITTED     
 * 规则处理。      
 *
 *	epqstate - EvalPlanQual 复查的状态     
 *	relation - 包含元组的表    
 *	rti - 包含元组的表的范围表索引      
 *	inputslot - 要处理的元组 - 可以是本关系的      
 *		EvalPlanQualSlot() 槽，以提高效率。               
 *
 * 本函数测试 inputslot 中的元组是否仍匹配相关的      
 * qual。为使该结果有用，通常输入元组必须是              
 * 最后一行版本（否则结果用处不大），并且            
 * 已被锁定（否则结果可能过时）。这通常              
 * 通过使用 table_tuple_lock() 并带       
 * TUPLE_LOCK_FLAG_FIND_LAST_VERSION。   
 *
 * 返回一个包含新的候选更新/删除元组的槽，如果   
 * 我们判定不应处理该行，则为 NULL。   
 */
TupleTableSlot *
EvalPlanQual(EPQState *epqstate, Relation relation,
			 Index rti, TupleTableSlot *inputslot)
{
	TupleTableSlot *slot;
	TupleTableSlot *testslot;

	Assert(rti > 0);

	/*
	 * 需运行复查子查询。初始化或重新初始化 EPQ 状态。  
	 */
	EvalPlanQualBegin(epqstate);

	/*
	 * 调用者常用 EvalPlanQualSlot 存元组，以免一次不必要     
	 * 的拷贝。        
	 */
	testslot = EvalPlanQualSlot(epqstate, relation, rti);
	if (testslot != inputslot)
		ExecCopySlot(testslot, inputslot);

	/*
	 * 标记本关系有一个可用的 EPQ 元组。（如果有         
	 * 不止一个结果关系，其余的关系仍保持标记为没有  
	 * 可用元组）  
	 */
	epqstate->relsubs_done[rti - 1] = false;
	epqstate->relsubs_blocked[rti - 1] = false;

	/*
	 * 运行 EPQ 查询。我们假定它至多返回一个元组。 
	 */
	slot = EvalPlanQualNext(epqstate);

	/*
	 * 若得到了元组，强制槽将元组实体化，使其不         
	 * 依赖于 EPQ 查询中的任何本地状态（尤其是，         
	 * 槽中很可能含有对 copyTuple 中可能存在的任何按引用   
	 * 传递的 datum 引用）。与下一步一样，这是为了防      
	 * 止 EPQ 查询被过早地重用。               
	 */
	if (!TupIsNull(slot))
		ExecMaterializeSlot(slot);

	/*
	 * 清空测试元组，并标记此处没有可用元组。          
	 * 在 EPQ 状态被重用于测试另一个目标关系的元组时， 
	 * 需要这样做。        
	 */
	ExecClearTuple(testslot);
	epqstate->relsubs_blocked[rti - 1] = true;

	return slot;
}

/*
 * EvalPlanQualInit -- 在创建可能需调用 EPQ 处理的计划   
 * 状态节点时进行初始化。        
 *
 * 如果调用者打算使用 EvalPlanQual()，则 resultRelations 应  
 * 该为 EvalPlanQual() 的潜在目标关系的 RT 索引列表，     
 * 且我们会安排其他被列出的关系在 EvalPlanQual() 调用 
 * 期间不返回元组。否则 resultRelations                 
 * 应为 NIL。 
 *
 * 注意：subplan/auxrowmarks 可为 NULL/NIL，若稍后将以     
 * EvalPlanQualSetPlan设置
 */
void
EvalPlanQualInit(EPQState *epqstate, EState *parentestate,
				 Plan *subplan, List *auxrowmarks,
				 int epqParam, List *resultRelations)
{
	Index		rtsize = parentestate->es_range_table_size;

	/* 初始化 EPQState 生存期内不变的数据        */
	epqstate->parentestate = parentestate;
	epqstate->epqParam = epqParam;
	epqstate->resultRelations = resultRelations;

	/*
	 * 为每个潜在的 rti 分配引用槽的空间 - 现在就做，    
	 * 而非像其他动态资源那样在 EvalPlanQualBegin() 中     
	 * 分配，这样 EvalPlanQualSlot() 就能用于保存以后*可*     
	 * 需要 EPQ 的元组，免去 EvalPlanQualBegin()         
	 * 的开销。        
	 */
	epqstate->tuple_table = NIL;
	epqstate->relsubs_slot = (TupleTableSlot **)
		palloc0(rtsize * sizeof(TupleTableSlot *));

	/* ... 并记住 EvalPlanQualBegin 将需要的数据     */
	epqstate->plan = subplan;
	epqstate->arowMarks = auxrowmarks;

	/* ... 并标记 EPQ 状态未激活   */
	epqstate->origslot = NULL;
	epqstate->recheckestate = NULL;
	epqstate->recheckplanstate = NULL;
	epqstate->relsubs_rowmark = NULL;
	epqstate->relsubs_done = NULL;
	epqstate->relsubs_blocked = NULL;
}

/*
 * EvalPlanQualSetPlan -- 设置/更改 EPQState 子计划。  
 *
 * 过去需要它，是为了让 ModifyTable 能处理多个子计划。     
 * 如今本可以把它重构消除掉。     
 */
void
EvalPlanQualSetPlan(EPQState *epqstate, Plan *subplan, List *auxrowmarks)
{
	/* 若有活跃的 EPQ 查询，将其关闭 */
	EvalPlanQualEnd(epqstate);
	/* 并设置/更改计划指针    */
	epqstate->plan = subplan;
	/* rowmark 也依赖于计划           */
	epqstate->arowMarks = auxrowmarks;
}

/*
 * 返回一个 EPQ 测试元组用的槽，必要时创建。   
 *
 * 注意这只要求已调用过 EvalPlanQualInit()，           
 * EvalPlanQualBegin() 并非必需。  
 */
TupleTableSlot *
EvalPlanQualSlot(EPQState *epqstate,
				 Relation relation, Index rti)
{
	TupleTableSlot **slot;

	Assert(relation);
	Assert(rti > 0 && rti <= epqstate->parentestate->es_range_table_size);
	slot = &epqstate->relsubs_slot[rti - 1];

	if (*slot == NULL)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(epqstate->parentestate->es_query_cxt);
		*slot = table_slot_create(relation, &epqstate->tuple_table);
		MemoryContextSwitchTo(oldcontext);
	}

	return *slot;
}

/*
 * 为 rti 所标识的未锁定关系取出当前行值，该关系需要  
 * 由 EvalPlanQual 操作来扫描。origslot 必须已被设置为         
 * 包含我们需要复查的当前结果行（顶层行）。如果找到  
 * 了替换元组则返回 true，否则返回 false。                  
 */
bool
EvalPlanQualFetchRowMark(EPQState *epqstate, Index rti, TupleTableSlot *slot)
{
	ExecAuxRowMark *earm = epqstate->relsubs_rowmark[rti - 1];
	ExecRowMark *erm;
	Datum		datum;
	bool		isNull;

	Assert(earm != NULL);
	Assert(epqstate->origslot != NULL);

	erm = earm->rowmark;

	if (RowMarkRequiresRowShareLock(erm->markType))
		elog(ERROR, "EvalPlanQual doesn't support locking rowmarks");

	/* 若是子关系，必须检查它是否产生了该行*/
	if (erm->rti != erm->prti)
	{
		Oid			tableoid;

		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->toidAttNo,
									 &isNull);
		/* 未锁定的关系可能处于外连接的内侧      */
		if (isNull)
			return false;

		tableoid = DatumGetObjectId(datum);

		Assert(OidIsValid(erm->relid));
		if (tableoid != erm->relid)
		{
			/* 此子关系当前不活动      */
			return false;
		}
	}

	if (erm->markType == ROW_MARK_REFERENCE)
	{
		Assert(erm->relation != NULL);

		/* 取元组的 ctid      */
		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->ctidAttNo,
									 &isNull);
		/* 未锁定的关系可能处于外连接的内侧      */
		if (isNull)
			return false;

		/* 外部表上的获取请求必须传给它们的 FDW         */
		if (erm->relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			FdwRoutine *fdwroutine;
			bool		updated = false;

			fdwroutine = GetFdwRoutineForRelation(erm->relation, false);
			/* 此点此前应已检查，但稳妥起见仍再检查   */
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(erm->relation))));

			fdwroutine->RefetchForeignRow(epqstate->recheckestate,
										  erm,
										  datum,
										  slot,
										  &updated);
			if (TupIsNull(slot))
				elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");

			/*
			 * 理想情况下此处应坚持 updated == false，但这      
			 * 假定 FDW 能精确跟踪它，它们未必能做到。      
			 * 所以就忽略该标志。  
			 */
			return true;
		}
		else
		{
			/* 普通表，取出元组        */
			if (!table_tuple_fetch_row_version(erm->relation,
											   (ItemPointer) DatumGetPointer(datum),
											   SnapshotAny, slot))
				elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");
			return true;
		}
	}
	else
	{
		Assert(erm->markType == ROW_MARK_COPY);

		/* 取出该关系的整行 Var             */
		datum = ExecGetJunkAttribute(epqstate->origslot,
									 earm->wholeAttNo,
									 &isNull);
		/* 未锁定的关系可能处于外连接的内侧      */
		if (isNull)
			return false;

		ExecStoreHeapTupleDatum(datum, slot);
		return true;
	}
}

/*
 * 从 EvalPlanQual 测试取出下一行（如有）   
 *
 * （实践中绝不应有多于一行……）            
 */
TupleTableSlot *
EvalPlanQualNext(EPQState *epqstate)
{
	MemoryContext oldcontext;
	TupleTableSlot *slot;

	oldcontext = MemoryContextSwitchTo(epqstate->recheckestate->es_query_cxt);
	slot = ExecProcNode(epqstate->recheckplanstate);
	MemoryContextSwitchTo(oldcontext);

	return slot;
}

/*
 * 初始化或重置 EvalPlanQual 状态树     
 */
void
EvalPlanQualBegin(EPQState *epqstate)
{
	EState	   *parentestate = epqstate->parentestate;
	EState	   *recheckestate = epqstate->recheckestate;

	if (recheckestate == NULL)
	{
		/* 首次经过，故创建一个子 EState     */
		EvalPlanQualStart(epqstate, epqstate->plan);
	}
	else
	{
		/*
		 * 已有合适的子 EPQ 树，所以只需重置它。       
		 */
		Index		rtsize = parentestate->es_range_table_size;
		PlanState  *rcplanstate = epqstate->recheckplanstate;

		/*
		 * 将 relsubs_done[] 重置为等于 relsubs_blocked[]，使        
		 *  EPQ 运行绝不会尝试从被阻塞的目标关系取          
		 * 元组。 
		 */
		memcpy(epqstate->relsubs_done, epqstate->relsubs_blocked,
			   rtsize * sizeof(bool));

		/* 重新复制父参数的当前值          */
		if (parentestate->es_plannedstmt->paramExecTypes != NIL)
		{
			int			i;

			/*
			 * 强制求值可能被子计划需要的 InitPlan 输出，   
			 * 以防它们在 EvalPlanQualStart 后被重置   
			 * （其理由见其中的注释）。     
			 */
			ExecSetParamPlanMulti(rcplanstate->plan->extParam,
								  GetPerTupleExprContext(parentestate));

			i = list_length(parentestate->es_plannedstmt->paramExecTypes);

			while (--i >= 0)
			{
				/* 有值即复制，不含 execPlan 链接 */
				recheckestate->es_param_exec_vals[i].value =
					parentestate->es_param_exec_vals[i].value;
				recheckestate->es_param_exec_vals[i].isnull =
					parentestate->es_param_exec_vals[i].isnull;
			}
		}

		/*
		 * 将子计划树标记为所有扫描节点都需重扫描。  
		 * 第一个 ExecProcNode 会负责实际执行重扫描。       
		 */
		rcplanstate->chgParam = bms_add_member(rcplanstate->chgParam,
											   epqstate->epqParam);
	}
}

/*
 * 开始执行 EvalPlanQual 计划树。       
 *
 * 这是 ExecutorStart() 的精简版：我们从顶层 estate          
 * 复制一些状态，而非全新初始化。          
 */
static void
EvalPlanQualStart(EPQState *epqstate, Plan *planTree)
{
	EState	   *parentestate = epqstate->parentestate;
	Index		rtsize = parentestate->es_range_table_size;
	EState	   *rcestate;
	MemoryContext oldcontext;
	ListCell   *l;

	epqstate->recheckestate = rcestate = CreateExecutorState();

	oldcontext = MemoryContextSwitchTo(rcestate->es_query_cxt);

	/* 标明这是一个用于执行 EPQ 的 EState   */
	rcestate->es_epq_active = epqstate;

	/*
	 * 子 EPQ EState 共享父节点的不变状态副本，例如         
	 * 快照、范围表和外部参数信息。它们需要自己的        
	 * 本地状态副本，包括元组表、es_param_exec_vals、       
	 * result-rel 信息等 
	 */
	rcestate->es_direction = ForwardScanDirection;
	rcestate->es_snapshot = parentestate->es_snapshot;
	rcestate->es_crosscheck_snapshot = parentestate->es_crosscheck_snapshot;
	rcestate->es_range_table = parentestate->es_range_table;
	rcestate->es_range_table_size = parentestate->es_range_table_size;
	rcestate->es_relations = parentestate->es_relations;
	rcestate->es_rowmarks = parentestate->es_rowmarks;
	rcestate->es_rteperminfos = parentestate->es_rteperminfos;
	rcestate->es_plannedstmt = parentestate->es_plannedstmt;
	rcestate->es_junkFilter = parentestate->es_junkFilter;
	rcestate->es_output_cid = parentestate->es_output_cid;
	rcestate->es_queryEnv = parentestate->es_queryEnv;

	/*
	 * 子计划所需的 ResultRelInfos 会在子计划自身初始化时    
	 * 从头初始化。                  
	 */
	rcestate->es_result_relations = NULL;
	/* es_trig_target_relations 绝不能被复制 */
	rcestate->es_top_eflags = parentestate->es_top_eflags;
	rcestate->es_instrument = parentestate->es_instrument;
	/* es_auxmodifytables 绝不能复制    */

	/*
	 * 外部参数列表直接从父级共享。内部参数工作区    
	 * 则必须是本地状态，但我们从父级复制初始值，      
	 * 以便访问父计划树中其他部分已设置的                
	 * 参数值。                                           
	 */
	rcestate->es_param_list_info = parentestate->es_param_list_info;
	if (parentestate->es_plannedstmt->paramExecTypes != NIL)
	{
		int			i;

		/*
		 * 强制求值子计划可能需要的 InitPlan 输出。         
		 * （若做得更复杂，或许能把这一步推迟到子计划  
		 * 真正需要它们的时候，但似乎不值得这么麻烦；  
		 * 因为这本就是边缘情形，通常 InitPlan             
		 * 会在到达 EvalPlanQual 之前就已求值。）                
		 *
		 * 这不会触及子计划树内某处的 InitPlan 输出参数，   
		 * 只触及挂在 ModifyTable 节点或其上层且在        
		 * 子计划内被引用的那些。但这没关系，因为规划器 
		 * 只会把这样的 InitPlan 挂到更低层的 SubqueryScan 
		 * 节点上，而 EPQ 执行不会下钻进                      
		 * SubqueryScan。                      
		 *
		 * EState 的每元组输出 econtext 对此生存期足够短，    
		 * 因为应当在任何可能再次执行 EvalPlanQual 的机会   
		 * 到来之前被重置。 
		 */
		ExecSetParamPlanMulti(planTree->extParam,
							  GetPerTupleExprContext(parentestate));

		/* 现在创建内部参数工作区...      */
		i = list_length(parentestate->es_plannedstmt->paramExecTypes);
		rcestate->es_param_exec_vals = (ParamExecData *)
			palloc0(i * sizeof(ParamExecData));
		/* ... 并向下复制所有值，无论是否真正需要    */
		while (--i >= 0)
		{
			/* 有值即复制，不含 execPlan 链接 */
			rcestate->es_param_exec_vals[i].value =
				parentestate->es_param_exec_vals[i].value;
			rcestate->es_param_exec_vals[i].isnull =
				parentestate->es_param_exec_vals[i].isnull;
		}
	}

	/*
	 * 为每个 SubPlan 初始化私有状态信息。这必须在           
	 * 对主查询树执行 ExecInitNode 前完成，因为    
	 * ExecInitSubPlan 期望能找到这些条目。                       
	 * 某些 SubPlans 可能并未用在我们打算运行的那部分    
	 * 计划树中，但由于难以区分是哪些，故一律初始    
	 * 化 
	 */
	Assert(rcestate->es_subplanstates == NIL);
	foreach(l, parentestate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;

		subplanstate = ExecInitNode(subplan, rcestate, 0);
		rcestate->es_subplanstates = lappend(rcestate->es_subplanstates,
											 subplanstate);
	}

	/*
	 * 构建 RTI 索引的 rowmark 数组，好让   
	 *  EvalPlanQualFetchRowMark() 能高效访问待取的               
	 * rowmark 
	 */
	epqstate->relsubs_rowmark = (ExecAuxRowMark **)
		palloc0(rtsize * sizeof(ExecAuxRowMark *));
	foreach(l, epqstate->arowMarks)
	{
		ExecAuxRowMark *earm = (ExecAuxRowMark *) lfirst(l);

		epqstate->relsubs_rowmark[earm->rowmark->rti - 1] = earm;
	}

	/*
	 * 初始化各关系的 EPQ 元组状态。结果关系（如有）   
	 * 标记为受阻；其余标记为未取。   
	 */
	epqstate->relsubs_done = palloc_array(bool, rtsize);
	epqstate->relsubs_blocked = palloc0_array(bool, rtsize);

	foreach(l, epqstate->resultRelations)
	{
		int			rtindex = lfirst_int(l);

		Assert(rtindex > 0 && rtindex <= rtsize);
		epqstate->relsubs_blocked[rtindex - 1] = true;
	}

	memcpy(epqstate->relsubs_done, epqstate->relsubs_blocked,
		   rtsize * sizeof(bool));

	/*
	 * 为我们需要运行的那部分计划树中的所有节点          
	 * 初始化私有状态信息。这会打开文件、分配存储，   
	 * 并使我们准备好开始处理元组。     
	 */
	epqstate->recheckplanstate = ExecInitNode(planTree, rcestate, 0);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * EvalPlanQualEnd -- 在父计划状态节点终止时关闭，         
 * 或在我们结束当前 EPQ 子项时。    
 *
 * 这是 ExecutorEnd() 的精简版本；基本上我们想做             
 * 大部分常规清理，但*不*关闭结果关系（我们只是    
 * 从外层查询借用它们而已）。不过，我们必须关闭    
 * 任何已打开的结果与触发器目标关系，因为那些并      
 * 未被共享。（后者多半不应该存在，但以防           
 * 万一  
 */
void
EvalPlanQualEnd(EPQState *epqstate)
{
	EState	   *estate = epqstate->recheckestate;
	Index		rtsize;
	MemoryContext oldcontext;
	ListCell   *l;

	rtsize = epqstate->parentestate->es_range_table_size;

	/*
	 * 即使 EPQ 未启动也可能有元组表，因为允许               
	 * 用 EvalPlanQualSlot() 而无须调用 EvalPlanQualBegin()    
	 */
	if (epqstate->tuple_table != NIL)
	{
		memset(epqstate->relsubs_slot, 0,
			   rtsize * sizeof(TupleTableSlot *));
		ExecResetTupleTable(epqstate->tuple_table, true);
		epqstate->tuple_table = NIL;
	}

	/* EPQ 未启动，没有更多要做的事  */
	if (estate == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndNode(epqstate->recheckplanstate);

	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/* 丢弃 estate 专有的元组表，可能有节点用过它       */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/* 关闭挂接在本 EState 上的任何结果与触发器目标关系  */
	ExecCloseResultRelations(estate);

	MemoryContextSwitchTo(oldcontext);

	FreeExecutorState(estate);

	/* 置 EPQState 空闲*/
	epqstate->origslot = NULL;
	epqstate->recheckestate = NULL;
	epqstate->recheckplanstate = NULL;
	epqstate->relsubs_rowmark = NULL;
	epqstate->relsubs_done = NULL;
	epqstate->relsubs_blocked = NULL;
}
