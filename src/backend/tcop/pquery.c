/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES 查询命令处理代码  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/pquery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "commands/prepare.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * ActivePortal 是当前执行的门户（嵌套最深的一个，        
 * 如存在多个）。 
 */
Portal		ActivePortal = NULL;


static void ProcessQuery(PlannedStmt *plan,
						 const char *sourceText,
						 ParamListInfo params,
						 QueryEnvironment *queryEnv,
						 DestReceiver *dest,
						 QueryCompletion *qc);
static void FillPortalStore(Portal portal, bool isTopLevel);
static uint64 RunFromStore(Portal portal, ScanDirection direction, uint64 count,
						   DestReceiver *dest);
static uint64 PortalRunSelect(Portal portal, bool forward, long count,
							  DestReceiver *dest);
static void PortalRunUtility(Portal portal, PlannedStmt *pstmt,
							 bool isTopLevel, bool setHoldSnapshot,
							 DestReceiver *dest, QueryCompletion *qc);
static void PortalRunMulti(Portal portal,
						   bool isTopLevel, bool setHoldSnapshot,
						   DestReceiver *dest, DestReceiver *altdest,
						   QueryCompletion *qc);
static uint64 DoPortalRunFetch(Portal portal,
							   FetchDirection fdirection,
							   long count,
							   DestReceiver *dest);
static void DoPortalRewind(Portal portal);


/*
 * CreateQueryDesc
 */
QueryDesc *
CreateQueryDesc(PlannedStmt *plannedstmt,
				const char *sourceText,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				QueryEnvironment *queryEnv,
				int instrument_options)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = plannedstmt->commandType;	/* 操作    */
	qd->plannedstmt = plannedstmt;	/* plan */
	qd->sourceText = sourceText;	/* 查询串  */
	qd->snapshot = RegisterSnapshot(snapshot);	/* 快照   */
	/* RI 检查快照   */
	qd->crosscheck_snapshot = RegisterSnapshot(crosscheck_snapshot);
	qd->dest = dest;			/* 输出目标*/
	qd->params = params;		/* 传入查询的参数值           */
	qd->queryEnv = queryEnv;
	qd->instrument_options = instrument_options;	/* 需要监测统计吗？*/

	/* 在 ExecutorStart 设置前先置为 NULL    */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;
	qd->totaltime = NULL;

	/* 尚未执行     */
	qd->already_executed = false;

	return qd;
}

/*
 * FreeQueryDesc
 */
void
FreeQueryDesc(QueryDesc *qdesc)
{
	/* 不能是活动查询 */
	Assert(qdesc->estate == NULL);

	/* 注销我们的快照*/
	UnregisterSnapshot(qdesc->snapshot);
	UnregisterSnapshot(qdesc->crosscheck_snapshot);

	/* 只需释放 QueryDesc 本身           */
	pfree(qdesc);
}


/*
 * ProcessQuery
 *		执行 PORTAL_MULTI_QUERY、PORTAL_ONE_RETURNING 或         
 *		PORTAL_ONE_MOD_WITH 门户内的可规划查询    
 *
 *	plan: 该查询的计划树      
 *	sourceText: 查询的源文本          
 *	params: 所需的参数      
 *	dest: 结果发送目的地
 *	qc: 存储命令完成状态数据的位置。        
 *
 * 若调用者不需要状态串，qc 可为 NULL。    
 *
 * 必须在出错时会被重置或删除的内存上下文中调用 
 * 否则执行器占用的内存将被泄漏。               
 */
static void
ProcessQuery(PlannedStmt *plan,
			 const char *sourceText,
			 ParamListInfo params,
			 QueryEnvironment *queryEnv,
			 DestReceiver *dest,
			 QueryCompletion *qc)
{
	QueryDesc  *queryDesc;

	/*
	 * 创建 QueryDesc 对象    
	 */
	queryDesc = CreateQueryDesc(plan, sourceText,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, 0);

	/*
	 * 调用 ExecutorStart 为执行做好准备          
	 */
	ExecutorStart(queryDesc, 0);

	/*
	 * 完整地执行该计划。
	 */
	ExecutorRun(queryDesc, ForwardScanDirection, 0, true);

	/*
	 * 若调用者需要，则构建命令完成状态数据。 
	 */
	if (qc)
	{
		switch (queryDesc->operation)
		{
			case CMD_SELECT:
				SetQueryCompletion(qc, CMDTAG_SELECT, queryDesc->estate->es_processed);
				break;
			case CMD_INSERT:
				SetQueryCompletion(qc, CMDTAG_INSERT, queryDesc->estate->es_processed);
				break;
			case CMD_UPDATE:
				SetQueryCompletion(qc, CMDTAG_UPDATE, queryDesc->estate->es_processed);
				break;
			case CMD_DELETE:
				SetQueryCompletion(qc, CMDTAG_DELETE, queryDesc->estate->es_processed);
				break;
			case CMD_MERGE:
				SetQueryCompletion(qc, CMDTAG_MERGE, queryDesc->estate->es_processed);
				break;
			default:
				SetQueryCompletion(qc, CMDTAG_UNKNOWN, queryDesc->estate->es_processed);
				break;
		}
	}

	/*
	 * 现在，关闭所有扫描并释放已分配的资源。     
	 */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);
}

/*
 * ChoosePortalStrategy
 *		根据给定的语句列表选择门户执行策略。             
 *
 * 列表元素可以是 Query 或 PlannedStmt。  
 * 这比门户所需的更通用，但 plancache.c 也会用到。     
 *
 * 见 portal.h 中的注释。 
 */
PortalStrategy
ChoosePortalStrategy(List *stmts)
{
	int			nSetTag;
	ListCell   *lc;

	/*
	 * PORTAL_ONE_SELECT 与 PORTAL_UTIL_SELECT 只需考虑          
	 * 单语句情形，因为不存在能为 SELECT 或实用命令      
	 * 添加辅助查询的重写规则。而 PORTAL_ONE_MOD_WITH            
	 * 同样只允许一个顶层语句。         
	 */
	if (list_length(stmts) == 1)
	{
		Node	   *stmt = (Node *) linitial(stmts);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (query->commandType == CMD_SELECT)
				{
					if (query->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
				if (query->commandType == CMD_UTILITY)
				{
					if (UtilityReturnsTuples(query->utilityStmt))
						return PORTAL_UTIL_SELECT;
					/* 不可能是 ONE_RETURNING，放弃   */
					return PORTAL_MULTI_QUERY;
				}
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (pstmt->commandType == CMD_SELECT)
				{
					if (pstmt->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
				if (pstmt->commandType == CMD_UTILITY)
				{
					if (UtilityReturnsTuples(pstmt->utilityStmt))
						return PORTAL_UTIL_SELECT;
					/* 不可能是 ONE_RETURNING，放弃   */
					return PORTAL_MULTI_QUERY;
				}
			}
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
	}

	/*
	 * PORTAL_ONE_RETURNING 必须允许重写添加的辅助查询。      
	 * 若恰好有一个 canSetTag 查询且带有 RETURNING 列表，则选 
	 * PORTAL_ONE_RETURNING。 
	 */
	nSetTag = 0;
	foreach(lc, stmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* 无需再往下查找   */
				if (query->commandType == CMD_UTILITY ||
					query->returningList == NIL)
					return PORTAL_MULTI_QUERY;	/* 无需再往下查找   */
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* 无需再往下查找   */
				if (pstmt->commandType == CMD_UTILITY ||
					!pstmt->hasReturning)
					return PORTAL_MULTI_QUERY;	/* 无需再往下查找   */
			}
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
	}
	if (nSetTag == 1)
		return PORTAL_ONE_RETURNING;

	/* 否则就是一般情形…… */
	return PORTAL_MULTI_QUERY;
}

/*
 * FetchPortalTargetList
 *		给定返回元组的门户，提取查询目标列表。        
 *		若门户没有可确定的目标列表，则返回 NIL。       
 *
 * 注意：不要修改结果。 
 */
List *
FetchPortalTargetList(Portal portal)
{
	/* 若已确定其不返回元组，再查也毫无意义        */
	if (portal->strategy == PORTAL_MULTI_QUERY)
		return NIL;
	/* 获取主语句并查明其返回什么                */
	return FetchStatementTargetList((Node *) PortalGetPrimaryStmt(portal));
}

/*
 * FetchStatementTargetList
 *		给定一条返回元组的语句，提取其查询目标列表。  
 *		若该语句没有可确定的目标列表，则返回 NIL。       
 *
 * 可用于 Query 或 PlannedStmt。              
 * 这比门户所需的更通用，但 plancache.c 也会用到。     
 *
 * 注意：不要修改结果。 
 *
 * XXX 注意与 UtilityReturnsTuples 保持同步。            
 */
List *
FetchStatementTargetList(Node *stmt)
{
	if (stmt == NULL)
		return NIL;
	if (IsA(stmt, Query))
	{
		Query	   *query = (Query *) stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/* 把注意力转向实用语句          */
			stmt = query->utilityStmt;
		}
		else
		{
			if (query->commandType == CMD_SELECT)
				return query->targetList;
			if (query->returningList)
				return query->returningList;
			return NIL;
		}
	}
	if (IsA(stmt, PlannedStmt))
	{
		PlannedStmt *pstmt = (PlannedStmt *) stmt;

		if (pstmt->commandType == CMD_UTILITY)
		{
			/* 把注意力转向实用语句          */
			stmt = pstmt->utilityStmt;
		}
		else
		{
			if (pstmt->commandType == CMD_SELECT)
				return pstmt->planTree->targetlist;
			if (pstmt->hasReturning)
				return pstmt->planTree->targetlist;
			return NIL;
		}
	}
	if (IsA(stmt, FetchStmt))
	{
		FetchStmt  *fstmt = (FetchStmt *) stmt;
		Portal		subportal;

		Assert(!fstmt->ismove);
		subportal = GetPortalByName(fstmt->portalname);
		Assert(PortalIsValid(subportal));
		return FetchPortalTargetList(subportal);
	}
	if (IsA(stmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = (ExecuteStmt *) stmt;
		PreparedStatement *entry;

		entry = FetchPreparedStatement(estmt->name, true);
		return FetchPreparedStatementTargetList(entry);
	}
	return NIL;
}

/*
 * PortalStart
 *		为执行准备门户。       
 *
 * 调用者必须已创建该门户，并完成 PortalDefineQuery()，  
 * 并在必要时调整门户选项。  
 *
 * 如果查询需要参数，则必须通过 params 传入              
 * （由调用者负责给它们适当的生存期）。       
 *
 * 调用者还可提供一组初始 eflags，供传给                  
 * ExecutorStart（但注意这些标志可能被内部修改，目前   
 * 仅对 PORTAL_ONE_SELECT 门户有效）。多数调用者          
 * 直接传零即可。   
 *
 * 调用者可选择传入要使用的快照；传 InvalidSnapshot          
 * 以获得设置新快照的常规行为。该参数                  
 * 目前对非 PORTAL_ONE_SELECT 门户忽略（它只打算             
 * 用于游标）。      
 *
 * 返回时，门户已就绪，可接受 PortalRun() 调用，结果   
 * tupdesc（若有）已知 
 */
void
PortalStart(Portal portal, ParamListInfo params,
			int eflags, Snapshot snapshot)
{
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;
	QueryDesc  *queryDesc;
	int			myeflags;

	Assert(PortalIsValid(portal));
	Assert(portal->status == PORTAL_DEFINED);

	/*
	 * 设置全局门户上下文指针。  
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		oldContext = MemoryContextSwitchTo(PortalContext);

		/* 若有参数列表，必须记住它    */
		portal->portalParams = params;

		/*
		 * 确定门户执行策略               
		 */
		portal->strategy = ChoosePortalStrategy(portal->stmts);

		/*
		 * 按策略将它启动起来。       
		 */
		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:

				/* 必须在启动执行器前设置快照。  */
				if (snapshot)
					PushActiveSnapshot(snapshot);
				else
					PushActiveSnapshot(GetTransactionSnapshot());

				/*
				 * 本可把该快照记入 portal->portalSnapshot，       
				 * 但目前似乎没必要，因为该代码路径       
				 * 无法用于非原子执行，因此不可能发生        
				 * 可能销毁该快照的提交/中止。既然             
				 * 我们不那样做，也就无需强制使用    
				 * 非默认嵌套级别的快照。          
				 */

				/*
				 * 在门户上下文中创建 QueryDesc；暂时先把     
				 * 目的地设为 DestNone。 
				 */
				queryDesc = CreateQueryDesc(linitial_node(PlannedStmt, portal->stmts),
											portal->sourceText,
											GetActiveSnapshot(),
											InvalidSnapshot,
											None_Receiver,
											params,
											portal->queryEnv,
											0);

				/*
				 * 如果是可滚动游标，则执行器需要支持   
				 * REWIND 与反向扫描，外加调用者所要求的     
				 * 其他一切。    
				 */
				if (portal->cursorOptions & CURSOR_OPT_SCROLL)
					myeflags = eflags | EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD;
				else
					myeflags = eflags;

				/*
				 * 调用 ExecutorStart 为执行做好准备          
				 */
				ExecutorStart(queryDesc, myeflags);

				/*
				 * 这会告知 PortalCleanup 关闭执行器        
				 */
				portal->queryDesc = queryDesc;

				/*
				 * 记住元组描述符（由 ExecutorStart 计算）  
				 */
				portal->tupDesc = queryDesc->tupDesc;

				/*
				 * 将游标位置数据重置为"查询起点"  
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* 允许抓取  */
				portal->portalPos = 0;

				PopActiveSnapshot();
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:

				/*
				 * 直到被告知运行门户时才会启动执行器。  
				 * 但我们确实需要设置结果 tupdesc。     
				 */
				{
					PlannedStmt *pstmt;

					pstmt = PortalGetPrimaryStmt(portal);
					portal->tupDesc =
						ExecCleanTypeFromTL(pstmt->planTree->targetlist);
				}

				/*
				 * 将游标位置数据重置为"查询起点"  
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* 允许抓取  */
				portal->portalPos = 0;
				break;

			case PORTAL_UTIL_SELECT:

				/*
				 * 我们不在此设置快照，PortalRunUtility 会在    
				 * 需要时设置它。     
				 */
				{
					PlannedStmt *pstmt = PortalGetPrimaryStmt(portal);

					Assert(pstmt->commandType == CMD_UTILITY);
					portal->tupDesc = UtilityTupleDescriptor(pstmt->utilityStmt);
				}

				/*
				 * 将游标位置数据重置为"查询起点"  
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* 允许抓取  */
				portal->portalPos = 0;
				break;

			case PORTAL_MULTI_QUERY:
				/* 现在无事可做  */
				portal->tupDesc = NULL;
				break;
		}
	}
	PG_CATCH();
	{
		/* 执行门户未捕获错误：将其标为失效    */
		MarkPortalFailed(portal);

		/* 恢复全局变量并传播错误       */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	portal->status = PORTAL_READY;
}

/*
 * PortalSetResultFormat
 *		为门户的输出选择格式代码。       
 *
 * 对于将经由 DestRemote 或 DestRemoteExecute 目的地读取的 
 * 门户，必须在 PortalStart 之后运行。目前其他目的地类型 
 * 并不需要它。          
 *
 * formats[] 是客户端的格式请求，遵循 Bind 消息的约定。  
 */
void
PortalSetResultFormat(Portal portal, int nFormats, int16 *formats)
{
	int			natts;
	int			i;

	/* 门户不返回元组则无需操作     */
	if (portal->tupDesc == NULL)
		return;
	natts = portal->tupDesc->natts;
	portal->formats = (int16 *)
		MemoryContextAlloc(portal->portalContext,
						   natts * sizeof(int16));
	if (nFormats > 1)
	{
		/* 为每列指定了格式         */
		if (nFormats != natts)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("bind message has %d result formats but query has %d columns",
							nFormats, natts)));
		memcpy(portal->formats, formats, natts * sizeof(int16));
	}
	else if (nFormats > 0)
	{
		/* 只指定了一个格式，用于所有列   */
		int16		format1 = formats[0];

		for (i = 0; i < natts; i++)
			portal->formats[i] = format1;
	}
	else
	{
		/* 对所有列使用默认格式     */
		for (i = 0; i < natts; i++)
			portal->formats[i] = 0;
	}
}

/*
 * PortalRun
 *		运行门户的查询。        
 *
 * count <= 0 被解释为无操作：目的地会经历启动          
 * 和关闭，但不会发生其他事情。count == FETCH_ALL         
 * 被解释为"所有行"。注意在多查询情形下 count          
 * 会被忽略，我们总将门户运行完毕。         
 *
 * isTopLevel: 若查询正在后端的"顶层"执行，则为 true  
 * （即直接来自客户端命令消息）。    
 *
 * run_once: 被忽略，只为避免稳定分支的 API 中断而存在。 
 *
 * dest: 主（canSetTag）查询的输出发送目的地 
 *
 * altdest: 非主查询的输出发送目的地       
 *
 * qc: 存储命令完成状态数据的位置。    
 *		不需要状态数据时可为 NULL。         
 *
 * 若门户的执行已完成则返回 true，若因 count 参数    
 * 耗尽而挂起则返回 false。                  
 */
bool
PortalRun(Portal portal, long count, bool isTopLevel, bool run_once,
		  DestReceiver *dest, DestReceiver *altdest,
		  QueryCompletion *qc)
{
	bool		result;
	uint64		nprocessed;
	ResourceOwner saveTopTransactionResourceOwner;
	MemoryContext saveTopTransactionContext;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext saveMemoryContext;

	Assert(PortalIsValid(portal));

	TRACE_POSTGRESQL_QUERY_EXECUTE_START();

	/* 初始化空的完成数据      */
	if (qc)
		InitializeQueryCompletion(qc);

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
	{
		elog(DEBUG3, "PortalRun");
		/* PORTAL_MULTI_QUERY 按查询记录自身统计  */
		ResetUsage();
	}

	/*
	 * 检查是否被误用，并将门户标为活跃。   
	 */
	MarkPortalActive(portal);

	/*
	 * 设置全局门户上下文指针。  
	 *
	 * 这里我们必须耍点特殊把戏，以支持诸如 VACUUM 和   
	 * CLUSTER 这类命令，它们在内部启动并提交事务。     
	 * 当我们被调用来执行这类命令时，CurrentResourceOwner      
	 * 将指向 TopTransactionResourceOwner --- 它会在             
	 * 内部提交与重启的过程中被销毁并替换。         
	 * 所以我们需要准备好把它恢复为指向                    
	 * 退出时的 TopTransactionResourceOwner。（这很丑陋吧？       
	 * 在内部启动全新事务的想法并不好。）     
	 * CurrentMemoryContext 也有类似问题，但我们在此保存的   
	 * 其余指针将为 NULL 或指向存续更久的对象。  
	 */
	saveTopTransactionResourceOwner = TopTransactionResourceOwner;
	saveTopTransactionContext = TopTransactionContext;
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	saveMemoryContext = CurrentMemoryContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * 若尚未运行该命令，就运行它，并将结果
				 * 存入门户的 tuplestore。但对 PORTAL_ONE_SELECT    
				 * 情形则不这么做。       
				 */
				if (portal->strategy != PORTAL_ONE_SELECT && !portal->holdStore)
					FillPortalStore(portal, isTopLevel);

				/*
				 * 现在获取所需部分的结果。 
				 */
				nprocessed = PortalRunSelect(portal, true, count, dest);

				/*
				 * 若门户结果含命令标签，且调用者传入了    
				 * 存储它的指针，则复制该标签并更新     
				 * 行数。
				 */
				if (qc && portal->qc.commandTag != CMDTAG_UNKNOWN)
				{
					CopyQueryCompletion(qc, &portal->qc);
					qc->nprocessed = nprocessed;
				}

				/* 标记门户非活跃  */
				portal->status = PORTAL_READY;

				/*
				 * 由于是向前抓取，atEnd 现在为真时报 DONE。   
				 */
				result = portal->atEnd;
				break;

			case PORTAL_MULTI_QUERY:
				PortalRunMulti(portal, isTopLevel, false,
							   dest, altdest, qc);

				/* 防止门户的命令被再次执行             */
				MarkPortalDone(portal);

				/* 在 RunMulti 结束时总是完成 */
				result = true;
				break;

			default:
				elog(ERROR, "unrecognized portal strategy: %d",
					 (int) portal->strategy);
				result = false; /* 使编译器安静  */
				break;
		}
	}
	PG_CATCH();
	{
		/* 执行门户未捕获错误：将其标为失效    */
		MarkPortalFailed(portal);

		/* 恢复全局变量并传播错误       */
		if (saveMemoryContext == saveTopTransactionContext)
			MemoryContextSwitchTo(TopTransactionContext);
		else
			MemoryContextSwitchTo(saveMemoryContext);
		ActivePortal = saveActivePortal;
		if (saveResourceOwner == saveTopTransactionResourceOwner)
			CurrentResourceOwner = TopTransactionResourceOwner;
		else
			CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (saveMemoryContext == saveTopTransactionContext)
		MemoryContextSwitchTo(TopTransactionContext);
	else
		MemoryContextSwitchTo(saveMemoryContext);
	ActivePortal = saveActivePortal;
	if (saveResourceOwner == saveTopTransactionResourceOwner)
		CurrentResourceOwner = TopTransactionResourceOwner;
	else
		CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
		ShowUsage("EXECUTOR STATISTICS");

	TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();

	return result;
}

/*
 * PortalRunSelect
 *		以 PORTAL_ONE_SELECT 模式执行门户查询，以及从  
 *		已完成的 holdStore 抓取时，即 PORTAL_ONE_RETURNING、   
 *		PORTAL_ONE_MOD_WITH 与 PORTAL_UTIL_SELECT。     
 *
 * 本函数处理简单的向前/向后 N 行情形。更复杂的       
 * 非顺序门户访问见 PortalRunFetch。           
 *
 * count <= 0 被解释为无操作：目的地会经历启动          
 * 和关闭，但不会发生其他事情。count == FETCH_ALL         
 * 被解释为"所有行"（cf FetchStmt.howMany） 
 *
 * 调用者必须已校验过该 Portal 并完成适当设置       
 * （见 PortalRun）。
 *
 * 返回已处理的行数（适合用于结果标签）。        
 */
static uint64
PortalRunSelect(Portal portal,
				bool forward,
				long count,
				DestReceiver *dest)
{
	QueryDesc  *queryDesc;
	ScanDirection direction;
	uint64		nprocessed;

	/*
	 * 注意：从已保持的游标或已完成的实用查询抓取时，
	 * queryDesc 将为 NULL；那条路径无法使用。
	 */
	queryDesc = portal->queryDesc;

	/* 既无就绪查询也无保持数据，即调用者出错了。  */
	Assert(queryDesc || portal->holdStore);

	/*
	 * 强制把 queryDesc 的目的地设置为正确的值。这支持 
	 * 例如 MOVE，它会传入 dest = DestNone。只要每次抓取        
	 * 都这样做就是允许的。（执行器绝不能               
	 * 假设 dest 永不改变。）  
	 */
	if (queryDesc)
		queryDesc->dest = dest;

	/*
	 * 确定移动方向，并检查我们是否已经处于该方向上的
	 * 可用元组末尾。如果是，则将方向设置为 NoMovement， 
	 * 以免抓取任何元组。（这一检查的存在，是因为    
	 * 并非所有计划节点类型在已返回过一次 NULL 之后仍 
	 * 能被健壮地再次调用。）然后调用执行器（绝不能 
	 * 跳过它，因为目的地需要看到一次启动和关闭，        
	 * 即使没有元组可用）。最后，还要根据取回的         
	 * 元组数量来更新门户的位置状态。                        
	 *           
	 */
	if (forward)
	{
		if (portal->atEnd || count <= 0)
		{
			direction = NoMovementScanDirection;
			count = 0;			/* 不要把负数 count 传给执行器 */
		}
		else
			direction = ForwardScanDirection;

		/* 执行器中 count 为零会处理所有行    */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, (uint64) count, dest);
		else
		{
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, (uint64) count,
						false);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
		{
			if (nprocessed > 0)
				portal->atStart = false;	/* 现在可以向后了 */
			if (count == 0 || nprocessed < (uint64) count)
				portal->atEnd = true;	/* 已取回全部行   */
			portal->portalPos += nprocessed;
		}
	}
	else
	{
		if (portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cursor can only scan forward"),
					 errhint("Declare it with SCROLL option to enable backward scan.")));

		if (portal->atStart || count <= 0)
		{
			direction = NoMovementScanDirection;
			count = 0;			/* 不要向执行器传递负的 count  */
		}
		else
			direction = BackwardScanDirection;

		/* 执行器中 count 为零表示处理全部行 */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, (uint64) count, dest);
		else
		{
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, (uint64) count,
						false);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
		{
			if (nprocessed > 0 && portal->atEnd)
			{
				portal->atEnd = false;	/* 现在可以向前了*/
				portal->portalPos++;	/* 为端点情形作调整 */
			}
			if (count == 0 || nprocessed < (uint64) count)
			{
				portal->atStart = true; /* 已取回全部行   */
				portal->portalPos = 0;
			}
			else
			{
				portal->portalPos -= nprocessed;
			}
		}
	}

	return nprocessed;
}

/*
 * FillPortalStore
 *		运行查询并把结果元组载入门户的 tuplestore。        
 *
 * 仅用于 PORTAL_ONE_RETURNING、PORTAL_ONE_MOD_WITH 和       
 * PORTAL_UTIL_SELECT 情形。  
 */
static void
FillPortalStore(Portal portal, bool isTopLevel)
{
	DestReceiver *treceiver;
	QueryCompletion qc;

	InitializeQueryCompletion(&qc);
	PortalCreateHoldStore(portal);
	treceiver = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(treceiver,
									portal->holdStore,
									portal->holdContext,
									false,
									NULL,
									NULL);

	switch (portal->strategy)
	{
		case PORTAL_ONE_RETURNING:
		case PORTAL_ONE_MOD_WITH:

			/*
			 * 把门户运行到完成，方式同默认          
			 * PORTAL_MULTI_QUERY 情形，但把主查询的输出发往     
			 * tuplestore。辅助查询的输出被丢弃。将门户的     
			 * holdSnapshot 设置为所用快照（或其副本）。      
			 */
			PortalRunMulti(portal, isTopLevel, true,
						   treceiver, None_Receiver, &qc);
			break;

		case PORTAL_UTIL_SELECT:
			PortalRunUtility(portal, linitial_node(PlannedStmt, portal->stmts),
							 isTopLevel, true, treceiver, &qc);
			break;

		default:
			elog(ERROR, "unsupported portal strategy: %d",
				 (int) portal->strategy);
			break;
	}

	/* 用实际命令结果覆盖门户的完成数据            */
	if (qc.commandTag != CMDTAG_UNKNOWN)
		CopyQueryCompletion(&portal->qc, &qc);

	treceiver->rDestroy(treceiver);
}

/*
 * RunFromStore
 *		从门户的 tuplestore 中取元组。    
 *
 * 调用约定与 ExecutorRun 类似，只是我们             
 * 不要求存在 queryDesc 或 estate。因此我们把                 
 * 处理元组数作为结果返回，而非 estate->es_processed。    
 *
 * 与 ExecutorRun 的不同之处是：目的地接收器函数运行在    
 * 调用者的内存上下文中（因为我们没有 estate）。注意  
 * 提防内存泄漏。
 */
static uint64
RunFromStore(Portal portal, ScanDirection direction, uint64 count,
			 DestReceiver *dest)
{
	uint64		current_tuple_count = 0;
	TupleTableSlot *slot;

	slot = MakeSingleTupleTableSlot(portal->tupDesc, &TTSOpsMinimalTuple);

	dest->rStartup(dest, CMD_SELECT, portal->tupDesc);

	if (ScanDirectionIsNoMovement(direction))
	{
		/* 除启动/停止目的地外什么都不做  */
	}
	else
	{
		bool		forward = ScanDirectionIsForward(direction);

		for (;;)
		{
			MemoryContext oldcontext;
			bool		ok;

			oldcontext = MemoryContextSwitchTo(portal->holdContext);

			ok = tuplestore_gettupleslot(portal->holdStore, forward, false,
										 slot);

			MemoryContextSwitchTo(oldcontext);

			if (!ok)
				break;

			/*
			 * 如果无法发送元组，我们假定目的地               
			 * 已经关闭、不可能再发送元组。若真如此，     
			 * 结束循环 
			 */
			if (!dest->receiveSlot(slot, dest))
				break;

			ExecClearTuple(slot);

			/*
			 * 检查元组计数：若已处理了正确数量            
			 * 则退出，否则再循环处理更多元组。count 为零  
			 * 即无限制   
			 */
			current_tuple_count++;
			if (count && count == current_tuple_count)
				break;
		}
	}

	dest->rShutdown(dest);

	ExecDropSingleTupleTableSlot(slot);

	return current_tuple_count;
}

/*
 * PortalRunUtility
 *		在门户内执行一条实用语句。     
 */
static void
PortalRunUtility(Portal portal, PlannedStmt *pstmt,
				 bool isTopLevel, bool setHoldSnapshot,
				 DestReceiver *dest, QueryCompletion *qc)
{
	/*
	 * 实用语句需要时设置快照。   
	 */
	if (PlannedStmtRequiresSnapshot(pstmt))
	{
		Snapshot	snapshot = GetTransactionSnapshot();

		/* 如被要求，注册所用快照并保存到门户中           */
		if (setHoldSnapshot)
		{
			snapshot = RegisterSnapshot(snapshot);
			portal->holdSnapshot = snapshot;
		}

		/*
		 * 无论如何，都要激活该快照并记入门户。          
		 * 因为门户现在引用该快照，必须告诉            
		 * snapmgr.c：该快照属于门户的事务层级，            
		 * 否则 portalSnapshot 可能变成悬空指针。              
		 */
		PushActiveSnapshotWithLevel(snapshot, portal->createLevel);
		/* PushActiveSnapshotWithLevel 可能已复制过快照       */
		portal->portalSnapshot = GetActiveSnapshot();
	}
	else
		portal->portalSnapshot = NULL;

	ProcessUtility(pstmt,
				   portal->sourceText,
				   (portal->cplan != NULL), /* plancache 内保护计划树 */
				   isTopLevel ? PROCESS_UTILITY_TOPLEVEL : PROCESS_UTILITY_QUERY,
				   portal->portalParams,
				   portal->queryEnv,
				   dest,
				   qc);

	/* 某些实用语句可能改变当前上下文    */
	MemoryContextSwitchTo(portal->portalContext);

	/*
	 * 某些实用命令（如 VACUUM）会把 ActiveSnapshot               
	 * 栈从我们脚下弹空，故若其现已为空也不报错。        
	 * 否则，我们的快照应当位于栈顶；将其弹出。       
	 * 注意它可能异于上文快照；见 EnsurePortalSnapshotExists  
	 */
	if (portal->portalSnapshot != NULL && ActiveSnapshotSet())
	{
		Assert(portal->portalSnapshot == GetActiveSnapshot());
		PopActiveSnapshot();
	}
	portal->portalSnapshot = NULL;
}

/*
 * PortalRunMulti
 *		一般情形下执行门户的查询（多查询             
 *		或非 SELECT 类查询） 
 */
static void
PortalRunMulti(Portal portal,
			   bool isTopLevel, bool setHoldSnapshot,
			   DestReceiver *dest, DestReceiver *altdest,
			   QueryCompletion *qc)
{
	bool		active_snapshot_set = false;
	ListCell   *stmtlist_item;

	/*
	 * 若目的地为 DestRemoteExecute，则改成 DestNone。原因  
	 * 是客户端不会预期收到任何元组，而且确实              
	 * 无从得知它们是什么，因为该门户执行策略下没有    
	 * 让 Describe 发送 RowDescription 消息的机制。这只影响      
	 * 由重写规则加入非 SELECT 查询的 SELECT 命令：它们    
	 * 会被执行，但结果会被丢弃，除非使用                 
	 * "simple Query" 协议。                                       
	 *          
	 */
	if (dest->mydest == DestRemoteExecute)
		dest = None_Receiver;
	if (altdest->mydest == DestRemoteExecute)
		altdest = None_Receiver;

	/*
	 * 循环处理由单个 parsetree 经分析与重写产生的            
	 * 各个查询。         
	 */
	foreach(stmtlist_item, portal->stmts)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, stmtlist_item);

		/*
		 * 若上一命令中收到取消信号则退出   
		 */
		CHECK_FOR_INTERRUPTS();

		if (pstmt->utilityStmt == NULL)
		{
			/*
			 * 处理可规划查询。  
			 */
			TRACE_POSTGRESQL_QUERY_EXECUTE_START();

			if (log_executor_stats)
				ResetUsage();

			/*
			 * 可规划查询必须总有快照。第一次                 
			 * 进入时取一个新快照；对同一门户中的        
			 * 后续查询，只更新快照中命令计数器副本。  
			 *         
			 */
			if (!active_snapshot_set)
			{
				Snapshot	snapshot = GetTransactionSnapshot();

				/* 如果被要求，注册快照并保存到门户中  */
				if (setHoldSnapshot)
				{
					snapshot = RegisterSnapshot(snapshot);
					portal->holdSnapshot = snapshot;
				}

				/*
				 * 不能让 holdSnapshot 同时也是活动快照，    
				 * 因为 UpdateActiveSnapshotCommandId 会抱怨。所以  
				 * 强制额外复制快照。普通 PushActiveSnapshot   
				 * 反正也会复制事务快照，因此这一步          
				 * 只在 setHoldSnapshot 为真时才加复制步骤。（  
				 * 活动快照的命令 ID 与 holdSnapshot 的值         
				 * 不一致也没关系。） 
				 */
				PushCopiedSnapshot(snapshot);

				/*
				 * 与 PORTAL_ONE_SELECT 类门户一样，这里    
				 * 似乎不必维护 portal->portalSnapshot。      
				 */

				active_snapshot_set = true;
			}
			else
				UpdateActiveSnapshotCommandId();

			if (pstmt->canSetTag)
			{
				/* 语句可设置命令标签  */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 portal->queryEnv,
							 dest, qc);
			}
			else
			{
				/* 重写加入的语句无法设标签 */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 portal->queryEnv,
							 altdest, NULL);
			}

			if (log_executor_stats)
				ShowUsage("EXECUTOR STATISTICS");

			TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();
		}
		else
		{
			/*
			 * 处理实用函数（创建、销毁等）        
			 *
			 * 对实用命令我们绝不能在此设置快照（如果      
			 * 需要，PortalRunUtility 会设置）。若实用命令      
			 * 独自位于门户中，则一切正常。唯一可能出现  
			 * 实用命令位于更长列表中的情形是规则         
			 * 允许包含 NotifyStmt。NotifyStmt 并不在乎          
			 * 自己是否有快照，所以我们只需保持当前        
			 * 快照不动即可。         
			 */
			if (pstmt->canSetTag)
			{
				Assert(!active_snapshot_set);
				/* 语句可设置命令标签  */
				PortalRunUtility(portal, pstmt, isTopLevel, false,
								 dest, qc);
			}
			else
			{
				Assert(IsA(pstmt->utilityStmt, NotifyStmt));
				/* 重写加入的语句无法设标签 */
				PortalRunUtility(portal, pstmt, isTopLevel, false,
								 altdest, NULL);
			}
		}

		/*
		 * 清空附属上下文以回收临时内存。         
		 */
		Assert(portal->portalContext == CurrentMemoryContext);

		MemoryContextDeleteChildren(portal->portalContext);

		/*
		 * 避免在 portal->stmts 已被重置时崩溃。这只可能  
		 * 发生在 CALL 或 DO 实用语句执行了内部           
		 * COMMIT/ROLLBACK 的情况（参见 PortalReleaseCachedPlan）。  
		 * CALL 或 DO 必然是门户中唯一的语句，所以我们       
		 * 再没有什么要做；但别解引用已悬空的列表指针。
		 *         
		 */
		if (portal->stmts == NIL)
			break;

		/*
		 * 查询间递增命令计数器，最后一条之后不递增。  
		 *     
		 */
		if (lnext(portal->stmts, stmtlist_item) != NULL)
			CommandCounterIncrement();
	}

	/* 压入过快照则将其弹出。  */
	if (active_snapshot_set)
		PopActiveSnapshot();

	/*
	 * 若请求了命令标签而上面未填入运行时确定的     
	 * 标签，则从门户复制解析期标签。（                      
	 * 边缘情形（如空预处理语句）那里也可能没有标签。  
	 * 没关系。）         
	 */
	if (qc &&
		qc->commandTag == CMDTAG_UNKNOWN &&
		portal->qc.commandTag != CMDTAG_UNKNOWN)
		CopyQueryCompletion(qc, &portal->qc);
}

/*
 * PortalRunFetch
 *		PortalRun 的变体形式，支持 SQL FETCH 方向。       
 *
 * 注意：目前假定没有调用者会传 isTopLevel = true。          
 *
 * count <= 0 被解释为无操作：目的地会经历启动          
 * 和关闭，但不发生其他事情。此外 count == FETCH_ALL     
 * 被解释为"所有行"。(cf FetchStmt.howMany)  
 *
 * 返回已处理的行数（适合用于结果标签）           
 */
uint64
PortalRunFetch(Portal portal,
			   FetchDirection fdirection,
			   long count,
			   DestReceiver *dest)
{
	uint64		result;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;

	Assert(PortalIsValid(portal));

	/*
	 * 检查是否被误用，并将门户标为活跃。   
	 */
	MarkPortalActive(portal);

	/*
	 * 设置全局门户上下文指针。  
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		oldContext = MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * 若尚未运行该命令，则运行它并把结果   
				 * 存入门户的 tuplestore。      
				 */
				if (!portal->holdStore)
					FillPortalStore(portal, false /* isTopLevel */ );

				/*
				 * 现在获取所需部分的结果。 
				 */
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			default:
				elog(ERROR, "unsupported portal strategy");
				result = 0;		/* 使编译器安静  */
				break;
		}
	}
	PG_CATCH();
	{
		/* 执行门户未捕获错误：将其标为失效    */
		MarkPortalFailed(portal);

		/* 恢复全局变量并传播错误       */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	/* 标记门户非活跃  */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	return result;
}

/*
 * DoPortalRunFetch
 *		PortalRunFetch 的主体 --- 门户上下文已经建立       
 *
 * 这里 count < 0 通常表示反转方向。此外 count == FETCH_ALL      
 * 被解释为"所有行"。(cf FetchStmt.howMany)     
 *
 * 返回已处理的行数（适合用于结果标签）           
 */
static uint64
DoPortalRunFetch(Portal portal,
				 FetchDirection fdirection,
				 long count,
				 DestReceiver *dest)
{
	bool		forward;

	Assert(portal->strategy == PORTAL_ONE_SELECT ||
		   portal->strategy == PORTAL_ONE_RETURNING ||
		   portal->strategy == PORTAL_ONE_MOD_WITH ||
		   portal->strategy == PORTAL_UTIL_SELECT);

	/*
	 * 注意：禁止向后取行（含重取当前行），               
	 * 但我们对 NO SCROLL 游标的解释非常宽松：可以使用     
	 * 任何 FetchDirection 选项，只要最终结果是向前             
	 * 移动至少一行。目前只需在 DoPortalRewind() 与            
	 * PortalRunSelect() 的 forward == false 路径中检查           
	 * NO SCROLL 就够了；但将来或许更愿意                    
	 * 显式考虑此限制。    
	 */
	switch (fdirection)
	{
		case FETCH_FORWARD:
			if (count < 0)
			{
				fdirection = FETCH_BACKWARD;
				count = -count;
			}
			/* 跳出 switch 以与 FETCH_BACKWARD 共享代码     */
			break;
		case FETCH_BACKWARD:
			if (count < 0)
			{
				fdirection = FETCH_FORWARD;
				count = -count;
			}
			/* 跳出 switch 以与 FETCH_FORWARD 共享代码     */
			break;
		case FETCH_ABSOLUTE:
			if (count > 0)
			{
				/*
				 * 定义：回绕起点，前进 count-1 行，返回下一
				 * 行（若有）。
				 *
				 * 实践中，若目标位置距起点不足一半，      
				 * 则从当前位置扫描更好。            
				 *
				 * 此外，若当前 portalPos 超出 "long" 的范围，   
				 * 就用笨办法以避免 PortalRunSelect 的 count        
				 * 参数可能溢出的风险。还必须把 LONG_MAX  
				 * 本身排除在外，以免看起来像 FETCH_ALL。  
				 *
				 * 无论如何，都安排向前去取目标行。     
				 *          
				 */
				if ((uint64) (count - 1) <= portal->portalPos / 2 ||
					portal->portalPos >= (uint64) LONG_MAX)
				{
					DoPortalRewind(portal);
					if (count > 1)
						PortalRunSelect(portal, true, count - 1,
										None_Receiver);
				}
				else
				{
					long		pos = (long) portal->portalPos;

					if (portal->atEnd)
						pos++;	/* 若越过末尾需多取一次  */
					if (count <= pos)
						PortalRunSelect(portal, false, pos - count + 1,
										None_Receiver);
					else if (count > pos + 1)
						PortalRunSelect(portal, true, count - pos - 1,
										None_Receiver);
				}
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * 定义：前进到末尾，回退 abs(count)-1 行， 
				 * 返回前一行（若有）。若预先知道末尾     
				 * 在哪里，本可优化，但通常我们无从得知。 
				 * （是否值得考虑 count 超过查询规模一半       
				 * 的情形？得知规模即可回绕……）     
				 */
				PortalRunSelect(portal, true, FETCH_ALL, None_Receiver);
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0 */
				/* 回绕到起点，返回零行    */
				DoPortalRewind(portal);
				return PortalRunSelect(portal, true, 0L, dest);
			}
			break;
		case FETCH_RELATIVE:
			if (count > 0)
			{
				/*
				 * 定义：前进 count-1 行，返回下一行（若有）  
				 */
				if (count > 1)
					PortalRunSelect(portal, true, count - 1, None_Receiver);
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * 定义：回退 abs(count)-1 行，返回前一行，若有
				 * 。  
				 */
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0 */
				/* 与 FETCH FORWARD 0 相同，故跳出 switch  */
				fdirection = FETCH_FORWARD;
			}
			break;
		default:
			elog(ERROR, "bogus direction");
			break;
	}

	/*
	 * 至此处时 fdirection 为 FETCH_FORWARD 或 FETCH_BACKWARD，count  
	 * >= 0 
	 */
	forward = (fdirection == FETCH_FORWARD);

	/*
	 * count 为零表示重取当前行（若有的话，按 SQL） 
	 */
	if (count == 0)
	{
		bool		on_row;

		/* 正停在某行上吗？ */
		on_row = (!portal->atStart && !portal->atEnd);

		if (dest->mydest == DestNone)
		{
			/* MOVE 0 返回 0/1，取决于 FETCH 0 是否会返回行  */
			return on_row ? 1 : 0;
		}
		else
		{
			/*
			 * 若正停在某个行上，则回退一行以便重取。     
			 * 如果未停在行上，我们仍须启动并关闭          
			 * 执行器，使目的地被正确初始化和关闭；所以 
			 * 继续前进。对 PortalRunSelect 来说，count == 0      
			 * 表示我们将取不到任何行。     
			 */
			if (on_row)
			{
				PortalRunSelect(portal, false, 1L, None_Receiver);
				/* 设置为向前取一行        */
				count = 1;
				forward = true;
			}
		}
	}

	/*
	 * 将 MOVE BACKWARD ALL 优化为回绕。 
	 */
	if (!forward && count == FETCH_ALL && dest->mydest == DestNone)
	{
		uint64		result = portal->portalPos;

		if (result > 0 && !portal->atEnd)
			result--;
		DoPortalRewind(portal);
		return result;
	}

	return PortalRunSelect(portal, forward, count, dest);
}

/*
 * DoPortalRewind - 将门户回绕到起点         
 */
static void
DoPortalRewind(Portal portal)
{
	QueryDesc  *queryDesc;

	/*
	 * 如果我们既未前进过，也未尝试前进游标，           
	 * 则无需任何操作（此时也不想让 NO SCROLL 报错）。  
	 */
	if (portal->atStart && !portal->atEnd)
		return;

	/* 否则游标必须允许滚动         */
	if (portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cursor can only scan forward"),
				 errhint("Declare it with SCROLL option to enable backward scan.")));

	/* 回绕 holdStore（若有）     */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_rescan(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
	}

	/* 执行器活动时回绕   */
	queryDesc = portal->queryDesc;
	if (queryDesc)
	{
		PushActiveSnapshot(queryDesc->snapshot);
		ExecutorRewind(queryDesc);
		PopActiveSnapshot();
	}

	portal->atStart = true;
	portal->atEnd = false;
	portal->portalPos = 0;
}

/*
 * PlannedStmtRequiresSnapshot - 顾名思义           
 */
bool
PlannedStmtRequiresSnapshot(PlannedStmt *pstmt)
{
	Node	   *utilityStmt = pstmt->utilityStmt;

	/* 如果不是实用语句，那它肯定需要快照             */
	if (utilityStmt == NULL)
		return true;

	/*
	 * 大多数实用语句都需要快照，对新语句的默认        
	 * 假设也应当是同样需要。因此，这里枚举                 
	 * 无需快照者 
	 *
	 * 事务控制、LOCK 与 SET 绝*不*能设置快照，因为       
	 * 它们必须能在事务快照模式事务的开头执行，          
	 * 而不冻结快照。据此我们也允许 SHOW 不设置           
	 * 快照。所列的其他语句只是效率上的技巧。         
	 * 当心列出任何能修改数据库的对象 --- 比如说，若      
	 * 它必须用含用户定义函数调用的表达式更新  
	 * 索引，那它最好有快照。                          
	 */
	if (IsA(utilityStmt, TransactionStmt) ||
		IsA(utilityStmt, LockStmt) ||
		IsA(utilityStmt, VariableSetStmt) ||
		IsA(utilityStmt, VariableShowStmt) ||
		IsA(utilityStmt, ConstraintsSetStmt) ||
	/* 从此往下都是效率技巧  */
		IsA(utilityStmt, FetchStmt) ||
		IsA(utilityStmt, ListenStmt) ||
		IsA(utilityStmt, NotifyStmt) ||
		IsA(utilityStmt, UnlistenStmt) ||
		IsA(utilityStmt, CheckPointStmt))
		return false;

	return true;
}

/*
 * EnsurePortalSnapshotExists - 按需重建门户级快照              
 *
 * 一般而言，只要我们在某个门户内执行，              
 * 就会有活动快照，除非该门户的查询属于          
 * 被豁免的实用语句（见 PlannedStmtRequiresSnapshot）。     
 * 然而，过程与 DO 块可以提交或中止事务，               
 * 从而销毁所有快照。可以调用本函数来在            
 * 不存在任何快照时重建门户级快照。        
 */
void
EnsurePortalSnapshotExists(void)
{
	Portal		portal;

	/*
	 * 若已设置快照则无事可做。（我们宁可相信         
	 * 最外层的活动快照属于某个门户；而如果             
	 * 没有门户，那就归别人负责管理。）              
	 */
	if (ActiveSnapshotSet())
		return;

	/* 否则我们最好有一个活动门户      */
	portal = ActivePortal;
	if (unlikely(portal == NULL))
		elog(ERROR, "cannot execute SQL without an outer snapshot or portal");
	Assert(portal->portalSnapshot == NULL);

	/*
	 * 创建新快照，将其激活，并记入门户。              
	 * 因为门户现在引用该快照，必须告知 snapmgr.c            
	 * 该快照属于门户的事务层级，以免                       
	 * portalSnapshot 变成悬空指针。            
	 */
	PushActiveSnapshotWithLevel(GetTransactionSnapshot(), portal->createLevel);
	/* PushActiveSnapshotWithLevel 可能已复制过快照       */
	portal->portalSnapshot = GetActiveSnapshot();
}
