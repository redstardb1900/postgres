/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  包含控制 POSTGRES 实用命令执行的函数。                  
 *	  它曾一度充当 Lisp 与 C 两个系统之间的接口。       
 *	          
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/event_trigger.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/lockcmds.h"
#include "commands/matview.h"
#include "commands/policy.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* 供插件在 ProcessUtility() 中接管的钩子    */
ProcessUtility_hook_type ProcessUtility_hook = NULL;

/* 局部函数声明          */
static int	ClassifyUtilityCommandAsReadOnly(Node *parsetree);
static void ProcessUtilitySlow(ParseState *pstate,
							   PlannedStmt *pstmt,
							   const char *queryString,
							   ProcessUtilityContext context,
							   ParamListInfo params,
							   QueryEnvironment *queryEnv,
							   DestReceiver *dest,
							   QueryCompletion *qc);
static void ExecDropStmt(DropStmt *stmt, bool isTopLevel);

/*
 * CommandIsReadOnly：可执行查询是否只读？  
 *
 * 该测试比 XactReadOnly 模式的测试严格得多；         
 * 查询必须*真正*只读，因为调用者不希望            
 * 为它执行 CommandCounterIncrement。  
 *
 * 注意：目前无需在此支持原始或已分析的查询   
 */
bool
CommandIsReadOnly(PlannedStmt *pstmt)
{
	Assert(IsA(pstmt, PlannedStmt));
	switch (pstmt->commandType)
	{
		case CMD_SELECT:
			if (pstmt->rowMarks != NIL)
				return false;	/* SELECT FOR [KEY] UPDATE/SHARE */
			else if (pstmt->hasModifyingCTE)
				return false;	/* 数据修改型 CTE*/
			else
				return true;
		case CMD_UPDATE:
		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_MERGE:
			return false;
		case CMD_UTILITY:
			/* 目前将所有实用命令都视为读/写处理 */
			return false;
		default:
			elog(WARNING, "unrecognized commandType: %d",
				 (int) pstmt->commandType);
			break;
	}
	return false;
}

/*
 * 判定一条实用命令的只读程度。                   
 *
 * 相关标志位的定义见 src/include/utility/tcop.h。                
 */
static int
ClassifyUtilityCommandAsReadOnly(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_AlterCollationStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDatabaseStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_AlterDomainStmt:
		case T_AlterEnumStmt:
		case T_AlterEventTrigStmt:
		case T_AlterExtensionContentsStmt:
		case T_AlterExtensionStmt:
		case T_AlterFdwStmt:
		case T_AlterForeignServerStmt:
		case T_AlterFunctionStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOpFamilyStmt:
		case T_AlterOperatorStmt:
		case T_AlterOwnerStmt:
		case T_AlterPolicyStmt:
		case T_AlterPublicationStmt:
		case T_AlterRoleSetStmt:
		case T_AlterRoleStmt:
		case T_AlterSeqStmt:
		case T_AlterStatsStmt:
		case T_AlterSubscriptionStmt:
		case T_AlterTSConfigurationStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTableMoveAllStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableStmt:
		case T_AlterTypeStmt:
		case T_AlterUserMappingStmt:
		case T_CommentStmt:
		case T_CompositeTypeStmt:
		case T_CreateAmStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateEventTrigStmt:
		case T_CreateExtensionStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateForeignTableStmt:
		case T_CreateFunctionStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreatePLangStmt:
		case T_CreatePolicyStmt:
		case T_CreatePublicationStmt:
		case T_CreateRangeStmt:
		case T_CreateRoleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStatsStmt:
		case T_CreateStmt:
		case T_CreateSubscriptionStmt:
		case T_CreateTableAsStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CreateUserMappingStmt:
		case T_CreatedbStmt:
		case T_DefineStmt:
		case T_DropOwnedStmt:
		case T_DropRoleStmt:
		case T_DropStmt:
		case T_DropSubscriptionStmt:
		case T_DropTableSpaceStmt:
		case T_DropUserMappingStmt:
		case T_DropdbStmt:
		case T_GrantRoleStmt:
		case T_GrantStmt:
		case T_ImportForeignSchemaStmt:
		case T_IndexStmt:
		case T_ReassignOwnedStmt:
		case T_RefreshMatViewStmt:
		case T_RenameStmt:
		case T_RuleStmt:
		case T_SecLabelStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
			{
				/* DDL 不是只读的，TRUNCATE 也不是。    */
				return COMMAND_IS_NOT_READ_ONLY;
			}

		case T_AlterSystemStmt:
			{
				/*
				 * 出人意料的是，ALTER SYSTEM 满足我们的      
				 * 全部只读定义：它不改变任何会影响 pg_dump
				 * 输出的内容，不写 WAL 也不危及未来 WAL 的    
				 * 重放，也不依赖任何需要由并行 worker 同步 
				 * 的状态。                             
				 *
				 * 所以，尽管它会写文件，它却是只读的！  
				 *      
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CallStmt:
		case T_DoStmt:
			{
				/*
				 * DO 块或被调用过程内命令可能并非只读，    
				 * 但在尝试执行它们时会单独检查。              
				 * 这里只需关心 DO 或 CALL 命令本身。            
				 *                           
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CheckPointStmt:
			{
				/*
				 * 你也许认为恢复期间不应允许此命令， 
				 * 但我们将恢复期间的 CHECKPOINT 命令解释为
				 * 重启点请求。我们允许这样做，因为它能    
				 * 有效缩短各种复制形式下的切换时间。     
				 *                                              
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ClosePortalStmt:
		case T_ConstraintsSetStmt:
		case T_DeallocateStmt:
		case T_DeclareCursorStmt:
		case T_DiscardStmt:
		case T_ExecuteStmt:
		case T_FetchStmt:
		case T_LoadStmt:
		case T_PrepareStmt:
		case T_UnlistenStmt:
		case T_VariableSetStmt:
			{
				/*
				 * 这些只改后端本地状态，故可在只读事务     
				 * 或备库上运行。但是，它们                     
				 * 并行模式下不允许，因为它们要么依赖      
				 * 或修改协作后端间可能未同步的状态。 
				 *                                         
				 */
				return COMMAND_OK_IN_RECOVERY | COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_ClusterStmt:
		case T_ReindexStmt:
		case T_VacuumStmt:
			{
				/*
				 * 这些命令写 WAL，故并非严格只读，   
				 * 并且不支持在并行 worker 中运行。         
				 *           
				 *
				 * 不过，它们不会以会影响 pg_dump 输出的方式  
				 * 改变数据库状态，故在只读事务中运行它们 
				 * 是没问题的。（CLUSTER 可能改变磁盘上行的 
				 * 顺序，这可能影响 pg_dump 输出的顺序，     
				 * 但这在语义上无关紧要。）             
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				/*
				 * 你可能认为 COPY FROM 完全不是只读的，但      
				 * 复制入临时表没有问题，因为那样       
				 * 不会改变 pg_dump 的输出。若目标表最终并非  
				 * 临时表，DoCopy 自己就会调用                 
				 * PreventCommandIfReadOnly 
				 */
				if (stmt->is_from)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ExplainStmt:
		case T_VariableShowStmt:
			{
				/*
				 * 这些命令不修改任何数据，在并行 worker 中   
				 * 运行安全。   
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ListenStmt:
		case T_NotifyStmt:
			{
				/*
				 * NOTIFY 需要分配 XID，故备库上不允许使用。   
				 * 如果没有 NOTIFY，LISTEN 或许本可以放行，      
				 * 因为什么也不做并无妨，至少提升前如此，
				 * 但我们目前禁止它，以免用户产生误解。  
				 *      
				 *
				 * （不过备库上允许 T_UnlistenStmt，因为它是个
				 * no-op）。   
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_LockStmt:
			{
				LockStmt   *stmt = (LockStmt *) parsetree;

				/*
				 * 恢复期间只允许较弱的加锁模式。            
				 * 限制须与下面函数一致：    
				 * LockAcquireExtended() 
				 */
				if (stmt->mode > RowExclusiveLock)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				/*
				 * PREPARE、COMMIT PREPARED 与 ROLLBACK PREPARED          
				 * 都写 WAL，故严格意义上并非只读；但其中   
				 * 第一个和第三个不改变 pg_dump 输出，因此     
				 * 可用于只读事务。    
				 *
				 * 作为例外，我们也认为 COMMIT PREPARED 在      
				 * 只读事务环境中是可行的。         
				 */
				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
					case TRANS_STMT_COMMIT:
					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_SAVEPOINT:
					case TRANS_STMT_RELEASE:
					case TRANS_STMT_ROLLBACK_TO:
						return COMMAND_IS_STRICTLY_READ_ONLY;

					case TRANS_STMT_PREPARE:
					case TRANS_STMT_COMMIT_PREPARED:
					case TRANS_STMT_ROLLBACK_PREPARED:
						return COMMAND_OK_IN_READ_ONLY_TXN;
				}
				elog(ERROR, "unrecognized TransactionStmtKind: %d",
					 (int) stmt->kind);
				return 0;		/* 让较笨的编译器安静*/
			}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			return 0;			/* 让较笨的编译器安静*/
	}
}

/*
 * PreventCommandIfReadOnly：若 XactReadOnly 则报错
 *
 * 此函数部分用于保证错误消息措辞的一致性；             
 * 有些调用者已自行检查过 XactReadOnly。     
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandIfParallelMode：若当前（子）事务处于并行     
 * 模式报错。  
 *
 * 此函数部分用于保证错误消息措辞的一致性；             
 * 有些调用者已自行检查过 IsInParallelMode()。     
 */
void
PreventCommandIfParallelMode(const char *cmdname)
{
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during a parallel operation",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery：若 RecoveryInProgress 则报错
 *
 * 在热备上不安全的操作中，大多数都会被     
 * XactReadOnly 测试拒绝。但少数几条命令               
 * 虽允许出现在"只读"事务中，却不允许出现在         
 * 热备模式中。这些命令应当调用本函数。         
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation：若处于安全限制上下文中，     
 * 则对危险命令报错。           
 *
 * 需要用它来保护会话本地状态，这类状态没有            
 * 更明确的保护机制，比如所有权。          
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/* translator: %s is name of a SQL command, eg PREPARE */
				 errmsg("cannot execute %s within security-restricted operation",
						cmdname)));
}

/*
 * ProcessUtility
 *		通用实用函数调用器     
 *
 *	pstmt：实用语句的 PlannedStmt 包装          
 *	queryString：命令的原始源码文本   
 *	readOnlyTree：若为真，pstmt 的节点树不得被修改 
 *	context：标识语句的来源（顶层客户端命令、       
 *		非顶层客户端命令、更大的实用命令的子命令）     
 *	params：执行期间使用的参数      
 *	queryEnv：供解析到执行使用的环境（例如瞬时             
 *		命名表，如触发器过渡表）。可为 NULL。
 *	dest：结果发往何处  
 *	qc：存储命令完成状态数据之处。可为 NULL，      
 *		否则调用者必须已初始化它。          
 *
 * 调用者必须提供 queryString；（如今）不允许传入 NULL。    
 * 如果确实没有源码文本，可传入一个常量字符串，   
 * 例如"(query not available)"   
 *
 * ProcessUtility_hook 的用户请注意：在处理含多条分号        
 * 分隔语句的查询串时，同一 queryString 可能会传给        
 * ProcessUtility 的多次调用。应使用 pstmt 的               
 * stmt_location 与 stmt_len 来标识包含当前语句的         
 * 子串。还要记住，某些实用语句（例如 CREATE SCHEMA）  
 * 会递归调用 ProcessUtility 来处理子语句，且常会向下传    
 * 入相同的 queryString、stmt_location 与 stmt_len，即当初      
 * 针对整条语句给出的那些值。              
 */
void
ProcessUtility(PlannedStmt *pstmt,
			   const char *queryString,
			   bool readOnlyTree,
			   ProcessUtilityContext context,
			   ParamListInfo params,
			   QueryEnvironment *queryEnv,
			   DestReceiver *dest,
			   QueryCompletion *qc)
{
	Assert(IsA(pstmt, PlannedStmt));
	Assert(pstmt->commandType == CMD_UTILITY);
	Assert(queryString != NULL);	/* 8.4 起必须如此*/
	Assert(qc == NULL || qc->commandTag == CMDTAG_UNKNOWN);

	/*
	 * 我们提供一个函数钩子变量，让可加载插件能在   
	 * ProcessUtility 被调用时获得控制权，通常会调用        
	 * standard_ProcessUtility()。   
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
}

/*
 * standard_ProcessUtility 本身只处理我们不提供             
 * 事件触发器支持的实用命令。有此支持的命令         
 * 则会向下传给 ProcessUtilitySlow，其中包含这类            
 * 触发器所需的基础设施。          
 *
 * 这种划分不只是为了性能：关键在于例如执行     
 * START TRANSACTION 时不触发事件触发器代码，            
 * 因为我们可能需要刷新事件触发器缓存，而这      
 * 要求处于有效事务中。              
 */
void
standard_ProcessUtility(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isAtomicContext = (!(context == PROCESS_UTILITY_TOPLEVEL || context == PROCESS_UTILITY_QUERY_NONATOMIC) || IsTransactionBlock());
	ParseState *pstate;
	int			readonly_flags;

	/* 这里可能递归，故检查是否递归过度   */
	check_stack_depth();

	/*
	 * 若给定的节点树是只读的，则制作一份副本，         
	 * 以确保解析转换不会破坏原始树。这一点可以  
	 * 重构，以便在更多情况下避免不必要的复制，         
	 * 但是否值得费这么大工夫并不清楚。复杂到          
	 * 复制代价高昂的语句恰恰正是我们需要复制           
	 * 的语句，因此似乎只能节省边角成本。            
	 */
	if (readOnlyTree)
		pstmt = copyObject(pstmt);
	parsetree = pstmt->utilityStmt;

	/* 在只读状态下禁止读/写命令。          */
	readonly_flags = ClassifyUtilityCommandAsReadOnly(parsetree);
	if (readonly_flags != COMMAND_IS_STRICTLY_READ_ONLY &&
		(XactReadOnly || IsInParallelMode()))
	{
		CommandTag	commandtag = CreateCommandTag(parsetree);

		if ((readonly_flags & COMMAND_OK_IN_READ_ONLY_TXN) == 0)
			PreventCommandIfReadOnly(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_PARALLEL_MODE) == 0)
			PreventCommandIfParallelMode(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_RECOVERY) == 0)
			PreventCommandDuringRecovery(GetCommandTagName(commandtag));
	}

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	pstate->p_queryEnv = queryEnv;

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** 事务 ********************      
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * SQL99 定义的 START TRANSACTION：与          
						 * BEGIN 等同。共用代码。
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock(stmt->chain))
						{
							/* 在 qc 中报告未成功提交  */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_PREPARE:
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* 在 qc 中报告未成功提交  */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventInTransactionBlock(isTopLevel, "COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, true);
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventInTransactionBlock(isTopLevel, "ROLLBACK PREPARED");
						FinishPreparedTransaction(stmt->gid, false);
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock(stmt->chain);
						break;

					case TRANS_STMT_SAVEPOINT:
						RequireTransactionBlock(isTopLevel, "SAVEPOINT");
						DefineSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionBlock(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionBlock(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->savepoint_name);

						/*
						 * CommitTransactionCommand 负责         
						 * 再次定义保存点。       
						 */
						break;
				}
			}
			break;

			/*
			 * 门户（游标）操作    
			 */
		case T_DeclareCursorStmt:
			PerformCursorOpen(pstate, (DeclareCursorStmt *) parsetree, params,
							  isTopLevel);
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest, qc);
			break;

		case T_DoStmt:
			ExecuteDoStmt(pstate, (DoStmt *) parsetree, isAtomicContext);
			break;

		case T_CreateTableSpaceStmt:
			/* 全局对象无事件触发器       */
			PreventInTransactionBlock(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropTableSpaceStmt:
			/* 全局对象无事件触发器       */
			PreventInTransactionBlock(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
			break;

		case T_AlterTableSpaceOptionsStmt:
			/* 全局对象无事件触发器       */
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				DoCopy(pstate, (CopyStmt *) parsetree,
					   pstmt->stmt_location, pstmt->stmt_len,
					   &processed);
				if (qc)
					SetQueryCompletion(qc, CMDTAG_COPY, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery(pstate, (PrepareStmt *) parsetree,
						 pstmt->stmt_location, pstmt->stmt_len);
			break;

		case T_ExecuteStmt:
			ExecuteQuery(pstate,
						 (ExecuteStmt *) parsetree, NULL,
						 params,
						 dest, qc);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			/* 全局对象无事件触发器       */
			GrantRole(pstate, (GrantRoleStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			/* 全局对象无事件触发器       */
			PreventInTransactionBlock(isTopLevel, "CREATE DATABASE");
			createdb(pstate, (CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseStmt:
			/* 全局对象无事件触发器       */
			AlterDatabase(pstate, (AlterDatabaseStmt *) parsetree, isTopLevel);
			break;

		case T_AlterDatabaseRefreshCollStmt:
			/* 全局对象无事件触发器       */
			AlterDatabaseRefreshColl((AlterDatabaseRefreshCollStmt *) parsetree);
			break;

		case T_AlterDatabaseSetStmt:
			/* 全局对象无事件触发器       */
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			/* 全局对象无事件触发器       */
			PreventInTransactionBlock(isTopLevel, "DROP DATABASE");
			DropDatabase(pstate, (DropdbStmt *) parsetree);
			break;

			/* 查询级异步通知                 */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				CheckRestrictedOperation("LISTEN");

				/*
				 * 后台进程不允许 LISTEN，因为它们没有         
				 * 收集 NOTIFY 消息的机制，否则它们会无限期   
				 * 阻塞 async SLRU 的清理。                     
				 * （自定义后台 worker 的作者可以直接调用  
				 * Async_Listen 来绕过此限制，但之后就得由       
				 * 他们自行提供机制来处理消息队列了。     
				 * ）注意似乎没有理由禁止 UNLISTEN。          
				 */
				if (MyBackendType != B_BACKEND)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/* translator: %s is name of a SQL command, eg LISTEN */
							 errmsg("cannot execute %s within a background process",
									"LISTEN")));

				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* 大概不需要……     */
				/* 若非超级用户，允许的名字将受限        */
				load_file(stmt->filename, !superuser());
			}
			break;

		case T_CallStmt:
			ExecuteCallStmt(castNode(CallStmt, parsetree), params, isAtomicContext, dest);
			break;

		case T_ClusterStmt:
			cluster(pstate, (ClusterStmt *) parsetree, isTopLevel);
			break;

		case T_VacuumStmt:
			ExecVacuum(pstate, (VacuumStmt *) parsetree, isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery(pstate, (ExplainStmt *) parsetree, params, dest);
			break;

		case T_AlterSystemStmt:
			PreventInTransactionBlock(isTopLevel, "ALTER SYSTEM");
			AlterSystemSetConfigFile((AlterSystemStmt *) parsetree);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree, isTopLevel);
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* 应允许 DISCARD PLANS 吗？ */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
			break;

		case T_CreateEventTrigStmt:
			/* 事件触发器无事件触发器   */
			CreateEventTrigger((CreateEventTrigStmt *) parsetree);
			break;

		case T_AlterEventTrigStmt:
			/* 事件触发器无事件触发器   */
			AlterEventTrigger((AlterEventTrigStmt *) parsetree);
			break;

			/*
			 * ******************************** ROLE 语句 ****    
			 */
		case T_CreateRoleStmt:
			/* 全局对象无事件触发器       */
			CreateRole(pstate, (CreateRoleStmt *) parsetree);
			break;

		case T_AlterRoleStmt:
			/* 全局对象无事件触发器       */
			AlterRole(pstate, (AlterRoleStmt *) parsetree);
			break;

		case T_AlterRoleSetStmt:
			/* 全局对象无事件触发器       */
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
			break;

		case T_DropRoleStmt:
			/* 全局对象无事件触发器       */
			DropRole((DropRoleStmt *) parsetree);
			break;

		case T_ReassignOwnedStmt:
			/* 全局对象无事件触发器       */
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
			break;

		case T_LockStmt:

			/*
			 * 由于锁会立即被丢弃，事务块外的 LOCK TABLE     
			 * 被认定为用户错误。                              
			 */
			RequireTransactionBlock(isTopLevel, "LOCK TABLE");
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			WarnNoTransactionBlock(isTopLevel, "SET CONSTRAINTS");
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!has_privs_of_role(GetUserId(), ROLE_PG_CHECKPOINT))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				/* translator: %s is name of a SQL command, eg CHECKPOINT */
						 errmsg("permission denied to execute %s command",
								"CHECKPOINT"),
						 errdetail("Only roles with privileges of the \"%s\" role may execute this command.",
								   "pg_checkpoint")));

			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
			break;

		case T_ReindexStmt:
			ExecReindex(pstate, (ReindexStmt *) parsetree, isTopLevel);
			break;

			/*
			 * 下列语句仅在某些情况下受事件触发器支持， 
			 * 故在其他情况下对它们走"快速路径"。       
			 */

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecuteGrantStmt(stmt);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->removeType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecDropStmt(stmt, isTopLevel);
			}
			break;

		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->renameType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecRenameStmt(stmt);
			}
			break;

		case T_AlterObjectDependsStmt:
			{
				AlterObjectDependsStmt *stmt = (AlterObjectDependsStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectDependsStmt(stmt, NULL);
			}
			break;

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectSchemaStmt(stmt, NULL);
			}
			break;

		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterOwnerStmt(stmt);
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					CommentObject(stmt);
				break;
			}

		case T_SecLabelStmt:
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecSecLabelStmt(stmt);
				break;
			}

		default:
			/* 其余所有语句类型都有事件触发器支持  */
			ProcessUtilitySlow(pstate, pstmt, queryString,
							   context, params, queryEnv,
							   dest, qc);
			break;
	}

	free_parsestate(pstate);

	/*
	 * 使命令的效果可见，例如让                  
	 * PreCommit_on_commit_actions() 能看到它们 (例如 bug      
	 * #15631) 
	 */
	CommandCounterIncrement();
}

/*
 * ProcessUtility 的"Slow"变体只应接收事件                   
 * 触发器设施支持的语句。因此只要上下文允许，
 * 我们总是执行触发器支持调用。                 
 */
static void
ProcessUtilitySlow(ParseState *pstate,
				   PlannedStmt *pstmt,
				   const char *queryString,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   QueryEnvironment *queryEnv,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isCompleteQuery = (context != PROCESS_UTILITY_SUBCOMMAND);
	bool		needCleanup;
	bool		commandCollected = false;
	ObjectAddress address;
	ObjectAddress secondaryObject = InvalidObjectAddress;

	/* 仅当 isCompleteQuery 为真时才执行事件触发器调用     */
	needCleanup = isCompleteQuery && EventTriggerBeginCompleteQuery();

	/* 用 PG_TRY 块确保调用 EventTriggerEndCompleteQuery        */
	PG_TRY();
	{
		if (isCompleteQuery)
			EventTriggerDDLCommandStart(parsetree);

		switch (nodeTag(parsetree))
		{
				/*
				 * 关系与属性操作              
				 */
			case T_CreateSchemaStmt:
				CreateSchemaCommand((CreateSchemaStmt *) parsetree,
									queryString,
									pstmt->stmt_location,
									pstmt->stmt_len);

				/*
				 * EventTriggerCollectSimpleCommand 由      
				 * CreateSchemaCommand
				 */
				commandCollected = true;
				break;

			case T_CreateStmt:
			case T_CreateForeignTableStmt:
				{
					List	   *stmts;
					RangeVar   *table_rv = NULL;

					/* 解析分析……     */
					stmts = transformCreateStmt((CreateStmt *) parsetree,
												queryString);

					/*
					 * ……然后执行。无法使用 foreach()，因为 
					 * 可能中途修改列表，只能逐一取出  
					 * 元素，用笨办法。             
					 */
					while (stmts != NIL)
					{
						Node	   *stmt = (Node *) linitial(stmts);

						stmts = list_delete_first(stmts);

						if (IsA(stmt, CreateStmt))
						{
							CreateStmt *cstmt = (CreateStmt *) stmt;
							Datum		toast_options;
							static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

							/* 记住为 LIKE 转换后的 RangeVar   */
							table_rv = cstmt->relation;

							/* 创建表本身         */
							address = DefineRelation(cstmt,
													 RELKIND_RELATION,
													 InvalidOid, NULL,
													 queryString);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);

							/*
							 * 让 NewRelationCreateToastTable 决定这一  
							 * 个是否也需要次要关系。  
							 */
							CommandCounterIncrement();

							/*
							 * 为 toast 表解析并校验 reloptions    
							 *      
							 */
							toast_options = transformRelOptions((Datum) 0,
																cstmt->options,
																"toast",
																validnsps,
																true,
																false);
							(void) heap_reloptions(RELKIND_TOASTVALUE,
												   toast_options,
												   true);

							NewRelationCreateToastTable(address.objectId,
														toast_options);
						}
						else if (IsA(stmt, CreateForeignTableStmt))
						{
							CreateForeignTableStmt *cstmt = (CreateForeignTableStmt *) stmt;

							/* 记住为 LIKE 转换后的 RangeVar   */
							table_rv = cstmt->base.relation;

							/* 创建表本身         */
							address = DefineRelation(&cstmt->base,
													 RELKIND_FOREIGN_TABLE,
													 InvalidOid, NULL,
													 queryString);
							CreateForeignTable(cstmt,
											   address.objectId);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);
						}
						else if (IsA(stmt, TableLikeClause))
						{
							/*
							 * 对 LIKE 选项做延迟处理，这会     
							 * 产生额外子语句。它们应在其余     
							 * 任何动作之前完成，故前插到      
							 * "stmts"。                                    
							 */
							TableLikeClause *like = (TableLikeClause *) stmt;
							List	   *morestmts;

							Assert(table_rv != NULL);

							morestmts = expandTableLikeClause(table_rv, like);
							stmts = list_concat(morestmts, stmts);
						}
						else
						{
							/*
							 * 其余一律递归。注意递归调用会把 
							 * 新建的对象存入我们的事件触发器  
							 * 上下文中。       
							 */
							PlannedStmt *wrapper;

							wrapper = makeNode(PlannedStmt);
							wrapper->commandType = CMD_UTILITY;
							wrapper->canSetTag = false;
							wrapper->utilityStmt = stmt;
							wrapper->stmt_location = pstmt->stmt_location;
							wrapper->stmt_len = pstmt->stmt_len;

							ProcessUtility(wrapper,
										   queryString,
										   false,
										   PROCESS_UTILITY_SUBCOMMAND,
										   params,
										   NULL,
										   None_Receiver,
										   NULL);
						}

						/* 命令之间需要 CCI    */
						if (stmts != NIL)
							CommandCounterIncrement();
					}

					/*
					 * 这里生成的多条命令是单独存放的，
					 * 因此禁用下层的收集。            
					 */
					commandCollected = true;
				}
				break;

			case T_AlterTableStmt:
				{
					AlterTableStmt *atstmt = (AlterTableStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					ListCell   *cell;

					/*
					 * 禁止在事务块或函数中 ALTER TABLE ..   
					 * DETACH CONCURRENTLY。（也许在过程中可以   
					 * 允许，但别抱太大期望。）                
					 */
					foreach(cell, atstmt->cmds)
					{
						AlterTableCmd *cmd = (AlterTableCmd *) lfirst(cell);

						/* 事务块中不允许 DETACH CONCURRENTLY           */
						if (cmd->subtype == AT_DetachPartition)
						{
							if (((PartitionCmd *) cmd->def)->concurrent)
								PreventInTransactionBlock(isTopLevel,
														  "ALTER TABLE ... DETACH CONCURRENTLY");
						}
					}

					/*
					 * 确定锁模式并获取锁。这还会执行基本的 
					 * 权限检查，从而不会在（比如）无权限  
					 * 的关系上等待锁。                            
					 *             
					 */
					lockmode = AlterTableGetLockLevel(atstmt->cmds);
					relid = AlterTableLookupRelation(atstmt, lockmode);

					if (OidIsValid(relid))
					{
						AlterTableUtilityContext atcontext;

						/* 设置递归回调所需的信息……        */
						atcontext.pstmt = pstmt;
						atcontext.queryString = queryString;
						atcontext.relid = relid;
						atcontext.params = params;
						atcontext.queryEnv = queryEnv;

						/* ……确保有事件触发器上下文……   */
						EventTriggerAlterTableStart(parsetree);
						EventTriggerAlterTableRelid(relid);

						/* ……执行  */
						AlterTable(atstmt, lockmode, &atcontext);

						/* 完  */
						EventTriggerAlterTableEnd();
					}
					else
						ereport(NOTICE,
								(errmsg("relation \"%s\" does not exist, skipping",
										atstmt->relation->relname)));
				}

				/* ALTER TABLE 在内部存放命令       */
				commandCollected = true;
				break;

			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

					/*
					 * 这些函数部分或全部递归，以覆盖继承  
					 * 的对象，因此权限检查在那里完成。      
					 */
					switch (stmt->subtype)
					{
						case 'T':	/* ALTER DOMAIN默认值*/

							/*
							 * 递归修改表的列默认值；若有要求  
							 * 则连同后代一起修改  
							 */
							address =
								AlterDomainDefault(stmt->typeName,
												   stmt->def);
							break;
						case 'N':	/* ALTER DOMAIN 去除非空  */
							address =
								AlterDomainNotNull(stmt->typeName,
												   false);
							break;
						case 'O':	/* ALTER DOMAIN 设置非空 */
							address =
								AlterDomainNotNull(stmt->typeName,
												   true);
							break;
						case 'C':	/* 添加约束   */
							address =
								AlterDomainAddConstraint(stmt->typeName,
														 stmt->def,
														 &secondaryObject);
							break;
						case 'X':	/* 删除约束    */
							address =
								AlterDomainDropConstraint(stmt->typeName,
														  stmt->name,
														  stmt->behavior,
														  stmt->missing_ok);
							break;
						case 'V':	/* 验证约束        */
							address =
								AlterDomainValidateConstraint(stmt->typeName,
															  stmt->name);
							break;
						default:	/* 唉  */
							elog(ERROR, "unrecognized alter domain type: %d",
								 (int) stmt->subtype);
							break;
					}
				}
				break;

				/*
				 * ************* 对象创建 / 销毁 **************        
				 */
			case T_DefineStmt:
				{
					DefineStmt *stmt = (DefineStmt *) parsetree;

					switch (stmt->kind)
					{
						case OBJECT_AGGREGATE:
							address =
								DefineAggregate(pstate, stmt->defnames, stmt->args,
												stmt->oldstyle,
												stmt->definition,
												stmt->replace);
							break;
						case OBJECT_OPERATOR:
							Assert(stmt->args == NIL);
							address = DefineOperator(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TYPE:
							Assert(stmt->args == NIL);
							address = DefineType(pstate,
												 stmt->defnames,
												 stmt->definition);
							break;
						case OBJECT_TSPARSER:
							Assert(stmt->args == NIL);
							address = DefineTSParser(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TSDICTIONARY:
							Assert(stmt->args == NIL);
							address = DefineTSDictionary(stmt->defnames,
														 stmt->definition);
							break;
						case OBJECT_TSTEMPLATE:
							Assert(stmt->args == NIL);
							address = DefineTSTemplate(stmt->defnames,
													   stmt->definition);
							break;
						case OBJECT_TSCONFIGURATION:
							Assert(stmt->args == NIL);
							address = DefineTSConfiguration(stmt->defnames,
															stmt->definition,
															&secondaryObject);
							break;
						case OBJECT_COLLATION:
							Assert(stmt->args == NIL);
							address = DefineCollation(pstate,
													  stmt->defnames,
													  stmt->definition,
													  stmt->if_not_exists);
							break;
						default:
							elog(ERROR, "unrecognized define stmt type: %d",
								 (int) stmt->kind);
							break;
					}
				}
				break;

			case T_IndexStmt:	/* 创建索引 */
				{
					IndexStmt  *stmt = (IndexStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					int			nparts = -1;
					bool		is_alter_table;

					if (stmt->concurrent)
						PreventInTransactionBlock(isTopLevel,
												  "CREATE INDEX CONCURRENTLY");

					/*
					 * 关系 OID 只在这里开头查找一次，         
					 * 以免后续重复名称查找并中途锁定到     
					 * 另一个关系。为避免锁升级风险，务必
					 * 在这里一开始就取最终需要的最强锁，  
					 * 锁模式计算必须与 DefineIndex() 的做法   
					 * 保持一致。                                       
					 *                                        
					 */
					lockmode = stmt->concurrent ? ShareUpdateExclusiveLock
						: ShareLock;
					relid =
						RangeVarGetRelidExtended(stmt->relation, lockmode,
												 0,
												 RangeVarCallbackOwnsRelation,
												 NULL);

					/*
					 * 分区表上的 CREATE INDEX 会递归到分区    
					 * （但普通继承表不会），故须尽早       
					 * 获取锁以避免死锁。            
					 *
					 * 我们也借此机会验证所有分区都是  
					 * 可以建立索引的对象，以免建了一部分
					 * 之后却失败的情况。顺便还统计分区数，
					 * 让 DefineIndex 无需再做一次重复的          
					 * find_all_inheritors 搜索。                       
					 */
					if (stmt->relation->inh &&
						get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
					{
						ListCell   *lc;
						List	   *inheritors = NIL;

						inheritors = find_all_inheritors(relid, lockmode, NULL);
						foreach(lc, inheritors)
						{
							Oid			partrelid = lfirst_oid(lc);
							char		relkind = get_rel_relkind(partrelid);

							if (relkind != RELKIND_RELATION &&
								relkind != RELKIND_MATVIEW &&
								relkind != RELKIND_PARTITIONED_TABLE &&
								relkind != RELKIND_FOREIGN_TABLE)
								elog(ERROR, "unexpected relkind \"%c\" on partition \"%s\"",
									 relkind, stmt->relation->relname);

							if (relkind == RELKIND_FOREIGN_TABLE &&
								(stmt->unique || stmt->primary))
								ereport(ERROR,
										(errcode(ERRCODE_WRONG_OBJECT_TYPE),
										 errmsg("cannot create unique index on partitioned table \"%s\"",
												stmt->relation->relname),
										 errdetail("Table \"%s\" contains partitions that are foreign tables.",
												   stmt->relation->relname)));
						}
						/* 统计直接与间接子表，不含 rel 本身 */
						nparts = list_length(inheritors) - 1;
						list_free(inheritors);
					}

					/*
					 * 若 IndexStmt 已经过转换，那它必然来自   
					 * generateClonedIndexStmt，而在当前用法中    
					 * 这意味着它来自 expandTableLikeClause，       
					 * 而非原始的解析分析。所以必须把它     
					 * 当作 ALTER TABLE ADD INDEX 而非 CREATE。        
					 * （这有点不雅，但目前看还不值得       
					 * 为此单独增加一个 bool 字段。）          
					 */
					is_alter_table = stmt->transformed;

					/* 解析分析……     */
					stmt = transformIndexStmt(relid, stmt, queryString);

					/* ……执行  */
					EventTriggerAlterTableStart(parsetree);
					address =
						DefineIndex(relid,	/* 堆关系的 OID     */
									stmt,
									InvalidOid, /* 无预定义 OID  */
									InvalidOid, /* 无父索引    */
									InvalidOid, /* 无父约束         */
									nparts, /* 分区数量，或 -1  */
									is_alter_table,
									true,	/* check_rights */
									true,	/* check_not_in_use */
									false,	/* skip_build */
									false); /* quiet */

					/*
					 * 把 CREATE INDEX 节点本身立即存入 stash；   
					 * 若 ALTER TABLE 代码中存放了任何命令，    
					 * 就需要它们出现在本命令之后。  
					 */
					EventTriggerCollectSimpleCommand(address, secondaryObject,
													 parsetree);
					commandCollected = true;
					EventTriggerAlterTableEnd();
				}
				break;

			case T_CreateExtensionStmt:
				address = CreateExtension(pstate, (CreateExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionStmt:
				address = ExecAlterExtensionStmt(pstate, (AlterExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionContentsStmt:
				address = ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree,
														 &secondaryObject);
				break;

			case T_CreateFdwStmt:
				address = CreateForeignDataWrapper(pstate, (CreateFdwStmt *) parsetree);
				break;

			case T_AlterFdwStmt:
				address = AlterForeignDataWrapper(pstate, (AlterFdwStmt *) parsetree);
				break;

			case T_CreateForeignServerStmt:
				address = CreateForeignServer((CreateForeignServerStmt *) parsetree);
				break;

			case T_AlterForeignServerStmt:
				address = AlterForeignServer((AlterForeignServerStmt *) parsetree);
				break;

			case T_CreateUserMappingStmt:
				address = CreateUserMapping((CreateUserMappingStmt *) parsetree);
				break;

			case T_AlterUserMappingStmt:
				address = AlterUserMapping((AlterUserMappingStmt *) parsetree);
				break;

			case T_DropUserMappingStmt:
				RemoveUserMapping((DropUserMappingStmt *) parsetree);
				/* DROP 不存放命令         */
				commandCollected = true;
				break;

			case T_ImportForeignSchemaStmt:
				ImportForeignSchema((ImportForeignSchemaStmt *) parsetree);
				/* 命令存放在 ImportForeignSchema 内部      */
				commandCollected = true;
				break;

			case T_CompositeTypeStmt:	/* CREATE TYPE（组合） */
				{
					CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

					address = DefineCompositeType(stmt->typevar,
												  stmt->coldeflist);
				}
				break;

			case T_CreateEnumStmt:	/* 创建枚举类型  */
				address = DefineEnum((CreateEnumStmt *) parsetree);
				break;

			case T_CreateRangeStmt: /* 创建范围类型   */
				address = DefineRange(pstate, (CreateRangeStmt *) parsetree);
				break;

			case T_AlterEnumStmt:	/* ALTER TYPE 枚举 */
				address = AlterEnum((AlterEnumStmt *) parsetree);
				break;

			case T_ViewStmt:	/* 创建视图*/
				EventTriggerAlterTableStart(parsetree);
				address = DefineView((ViewStmt *) parsetree, queryString,
									 pstmt->stmt_location, pstmt->stmt_len);
				EventTriggerCollectSimpleCommand(address, secondaryObject,
												 parsetree);
				/* 在内部存放    */
				commandCollected = true;
				EventTriggerAlterTableEnd();
				break;

			case T_CreateFunctionStmt:	/* 创建函数    */
				address = CreateFunction(pstate, (CreateFunctionStmt *) parsetree);
				break;

			case T_AlterFunctionStmt:	/* 更改函数   */
				address = AlterFunction(pstate, (AlterFunctionStmt *) parsetree);
				break;

			case T_RuleStmt:	/* 创建规则*/
				address = DefineRule((RuleStmt *) parsetree, queryString);
				break;

			case T_CreateSeqStmt:
				address = DefineSequence(pstate, (CreateSeqStmt *) parsetree);
				break;

			case T_AlterSeqStmt:
				address = AlterSequence(pstate, (AlterSeqStmt *) parsetree);
				break;

			case T_CreateTableAsStmt:
				address = ExecCreateTableAs(pstate, (CreateTableAsStmt *) parsetree,
											params, queryEnv, qc);
				break;

			case T_RefreshMatViewStmt:

				/*
				 * REFRESH CONCURRENTLY 会在内部执行一些 DDL 命令。
				 * 此处禁止收集 DDL 命令，以免这些命令         
				 * 出现在反解析命令队列中。刷新命令本身     
				 * 会入队，这就够了。               
				 */
				EventTriggerInhibitCommandCollection();
				PG_TRY(2);
				{
					address = ExecRefreshMatView((RefreshMatViewStmt *) parsetree,
												 queryString, params, qc);
				}
				PG_FINALLY(2);
				{
					EventTriggerUndoInhibitCommandCollection();
				}
				PG_END_TRY(2);
				break;

			case T_CreateTrigStmt:
				address = CreateTrigger((CreateTrigStmt *) parsetree,
										queryString, InvalidOid, InvalidOid,
										InvalidOid, InvalidOid, InvalidOid,
										InvalidOid, NULL, false, false);
				break;

			case T_CreatePLangStmt:
				address = CreateProceduralLanguage((CreatePLangStmt *) parsetree);
				break;

			case T_CreateDomainStmt:
				address = DefineDomain((CreateDomainStmt *) parsetree);
				break;

			case T_CreateConversionStmt:
				address = CreateConversionCommand((CreateConversionStmt *) parsetree);
				break;

			case T_CreateCastStmt:
				address = CreateCast((CreateCastStmt *) parsetree);
				break;

			case T_CreateOpClassStmt:
				DefineOpClass((CreateOpClassStmt *) parsetree);
				/* 命令存放在 DefineOpClass 中   */
				commandCollected = true;
				break;

			case T_CreateOpFamilyStmt:
				address = DefineOpFamily((CreateOpFamilyStmt *) parsetree);

				/*
				 * DefineOpFamily 调用EventTriggerCollectSimpleCommand
				 * 直接。
				 */
				commandCollected = true;
				break;

			case T_CreateTransformStmt:
				address = CreateTransform((CreateTransformStmt *) parsetree);
				break;

			case T_AlterOpFamilyStmt:
				AlterOpFamily((AlterOpFamilyStmt *) parsetree);
				/* 命令存放在 AlterOpFamily 中     */
				commandCollected = true;
				break;

			case T_AlterTSDictionaryStmt:
				address = AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
				break;

			case T_AlterTSConfigurationStmt:
				AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);

				/*
				 * 命令存放在 MakeConfigurationMapping 与        
				 * DropConfigurationMapping，调用者是        
				 * AlterTSConfiguration
				 */
				commandCollected = true;
				break;

			case T_AlterTableMoveAllStmt:
				AlterTableMoveAll((AlterTableMoveAllStmt *) parsetree);
				/* 命令存放在 AlterTableMoveAll 中     */
				commandCollected = true;
				break;

			case T_DropStmt:
				ExecDropStmt((DropStmt *) parsetree, isTopLevel);
				/* DROP 不存放命令         */
				commandCollected = true;
				break;

			case T_RenameStmt:
				address = ExecRenameStmt((RenameStmt *) parsetree);
				break;

			case T_AlterObjectDependsStmt:
				address =
					ExecAlterObjectDependsStmt((AlterObjectDependsStmt *) parsetree,
											   &secondaryObject);
				break;

			case T_AlterObjectSchemaStmt:
				address =
					ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree,
											  &secondaryObject);
				break;

			case T_AlterOwnerStmt:
				address = ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
				break;

			case T_AlterOperatorStmt:
				address = AlterOperator((AlterOperatorStmt *) parsetree);
				break;

			case T_AlterTypeStmt:
				address = AlterType((AlterTypeStmt *) parsetree);
				break;

			case T_CommentStmt:
				address = CommentObject((CommentStmt *) parsetree);
				break;

			case T_GrantStmt:
				ExecuteGrantStmt((GrantStmt *) parsetree);
				/* 命令存放在 ExecGrantStmt_oids 中     */
				commandCollected = true;
				break;

			case T_DropOwnedStmt:
				DropOwnedObjects((DropOwnedStmt *) parsetree);
				/* DROP 不存放命令         */
				commandCollected = true;
				break;

			case T_AlterDefaultPrivilegesStmt:
				ExecAlterDefaultPrivilegesStmt(pstate, (AlterDefaultPrivilegesStmt *) parsetree);
				EventTriggerCollectAlterDefPrivs((AlterDefaultPrivilegesStmt *) parsetree);
				commandCollected = true;
				break;

			case T_CreatePolicyStmt:	/* 创建策略  */
				address = CreatePolicy((CreatePolicyStmt *) parsetree);
				break;

			case T_AlterPolicyStmt: /* 更改策略 */
				address = AlterPolicy((AlterPolicyStmt *) parsetree);
				break;

			case T_SecLabelStmt:
				address = ExecSecLabelStmt((SecLabelStmt *) parsetree);
				break;

			case T_CreateAmStmt:
				address = CreateAccessMethod((CreateAmStmt *) parsetree);
				break;

			case T_CreatePublicationStmt:
				address = CreatePublication(pstate, (CreatePublicationStmt *) parsetree);
				break;

			case T_AlterPublicationStmt:
				AlterPublication(pstate, (AlterPublicationStmt *) parsetree);

				/*
				 * AlterPublication 调用EventTriggerCollectSimpleCommand
				 * 直接  
				 */
				commandCollected = true;
				break;

			case T_CreateSubscriptionStmt:
				address = CreateSubscription(pstate,
											 (CreateSubscriptionStmt *) parsetree,
											 isTopLevel);
				break;

			case T_AlterSubscriptionStmt:
				address = AlterSubscription(pstate,
											(AlterSubscriptionStmt *) parsetree,
											isTopLevel);
				break;

			case T_DropSubscriptionStmt:
				DropSubscription((DropSubscriptionStmt *) parsetree, isTopLevel);
				/* DROP 不存放命令         */
				commandCollected = true;
				break;

			case T_CreateStatsStmt:
				{
					Oid			relid;
					CreateStatsStmt *stmt = (CreateStatsStmt *) parsetree;
					RangeVar   *rel = (RangeVar *) linitial(stmt->relations);

					if (!IsA(rel, RangeVar))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("CREATE STATISTICS only supports relation names in the FROM clause")));

					/*
					 * CREATE STATISTICS 会影响未来的执行计划       
					 * 但不会干扰当前正在执行的计划。         
					 * 因此对关系加 ShareUpdateExclusiveLock 即可，  
					 * 它与 ANALYZE 及其他会设置统计信息的   
					 * DDL 冲突，但不会与普通查询冲突。        
					 *         
					 *
					 * XXX 此处不需要 RangeVarCallbackOwnsRelation， 
					 * 以保持原有行为。         
					 */
					relid = RangeVarGetRelid(rel, ShareUpdateExclusiveLock, false);

					/* 解析分析……     */
					stmt = transformStatsStmt(relid, stmt, queryString);

					address = CreateStatistics(stmt, true);
				}
				break;

			case T_AlterStatsStmt:
				address = AlterStatistics((AlterStatsStmt *) parsetree);
				break;

			case T_AlterCollationStmt:
				address = AlterCollation((AlterCollationStmt *) parsetree);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(parsetree));
				break;
		}

		/*
		 * 记住该对象，让 ddl_command_end 事件触发器能       
		 * 访问它。 
		 */
		if (!commandCollected)
			EventTriggerCollectSimpleCommand(address, secondaryObject,
											 parsetree);

		if (isCompleteQuery)
		{
			EventTriggerSQLDrop(parsetree);
			EventTriggerDDLCommandEnd(parsetree);
		}
	}
	PG_FINALLY();
	{
		if (needCleanup)
			EventTriggerEndCompleteQuery();
	}
	PG_END_TRY();
}

/*
 * ProcessUtilityForAlterTable
 *		由 ALTER TABLE 递归进入    
 *
 * ALTER TABLE 有时会生成 CREATE INDEX 等子命令。         
 * 它调用本函数而非主入口 ProcessUtility 来执行        
 * 这些子命令  
 *
 * stmt：待执行的实用命令     
 * context：不透明的透传结构，含所需信息    
 *
 * 调用者有责任在调用后执行 CommandCounterIncrement， 
 * （如需要）。      
 */
void
ProcessUtilityForAlterTable(Node *stmt, AlterTableUtilityContext *context)
{
	PlannedStmt *wrapper;

	/*
	 * 对事件触发器而言，必须"关闭"当前的复杂命令集，
	 * 随后再开启新集合；这是为确保命令事件的顺序      
	 * 与其执行方式保持一致。                               
	 */
	EventTriggerAlterTableEnd();

	/* 创建合适的包装器  */
	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = stmt;
	wrapper->stmt_location = context->pstmt->stmt_location;
	wrapper->stmt_len = context->pstmt->stmt_len;

	ProcessUtility(wrapper,
				   context->queryString,
				   false,
				   PROCESS_UTILITY_SUBCOMMAND,
				   context->params,
				   context->queryEnv,
				   None_Receiver,
				   NULL);

	EventTriggerAlterTableStart(context->pstmt->utilityStmt);
	EventTriggerAlterTableRelid(context->relid);
}

/*
 * DropStmt 的分派函数      
 */
static void
ExecDropStmt(DropStmt *stmt, bool isTopLevel)
{
	switch (stmt->removeType)
	{
		case OBJECT_INDEX:
			if (stmt->concurrent)
				PreventInTransactionBlock(isTopLevel,
										  "DROP INDEX CONCURRENTLY");
			/* fall through */

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
			RemoveRelations(stmt);
			break;
		default:
			RemoveObjects(stmt);
			break;
	}
}


/*
 * UtilityReturnsTuples
 *		若此实用命令将向目标发送输出，则返回         
 *		"true"。   
 *
 * 通常，ProcessUtility 中每个继续传递 "dest" 的情形        
 * 此处均应有分支。  
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			{
				CallStmt   *stmt = (CallStmt *) parsetree;

				return (stmt->funcexpr->funcresulttype == RECORDOID);
			}
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;	/* 报错不是本函数的职责  */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;	/* 报错不是本函数的职责  */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		取回某实用命令实际的输出元组描述符，          
 *		其 UtilityReturnsTuples() 曾返回 "true"。              
 *
 * 返回的描述符在当前内存上下文中创建（或复制），    
 *         
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			return CallStmtResultDesc((CallStmt *) parsetree);

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* 报错不是本函数的职责  */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* 报错不是本函数的职责  */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		若该 Query 会向目标发送输出，则返回 "true"。     
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/* 返回元组   */
			return true;
		case CMD_MERGE:
			return false;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* 带 RETURNING 的形式返回元组    */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/* 通常不应走到这里    */
			break;
	}
	return false;				/* 默认  */
}
#endif


/*
 * UtilityContainsQuery
 *		返回其所包含的 Query，若无则返回 NULL  
 *
 * 某些实用命令（如 EXPLAIN）包含可规划的 Query。         
 * 本函数封装了哪些命令含 Query 的确切知识。    
 * 我们假定它只在已完成解析的语句上被调用        
 * （否则所含解析树还不是 Query）。    
 *
 * 在某些情形（目前仅为 CREATE TABLE AS/SELECT INTO 与           
 * CREATE MATERIALIZED VIEW），可能含 Query 的实用命令             
 * 可以嵌套。本函数会向下钻取到非实用命令的 Query，   
 * 无则返回 NULL。
 */
Query *
UtilityContainsQuery(Node *parsetree)
{
	Query	   *qry;

	switch (nodeTag(parsetree))
	{
		case T_DeclareCursorStmt:
			qry = castNode(Query, ((DeclareCursorStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_ExplainStmt:
			qry = castNode(Query, ((ExplainStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_CreateTableAsStmt:
			qry = castNode(Query, ((CreateTableAsStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		default:
			return NULL;
	}
}


/*
 * AlterObjectTypeCommandTag
 *		CreateCommandTag 的辅助函数    
 *
 * 这涵盖将 ALTER 与 ObjectType 枚举联用的大多数情形。
 */
static CommandTag
AlterObjectTypeCommandTag(ObjectType objtype)
{
	CommandTag	tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = CMDTAG_ALTER_AGGREGATE;
			break;
		case OBJECT_ATTRIBUTE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_CAST:
			tag = CMDTAG_ALTER_CAST;
			break;
		case OBJECT_COLLATION:
			tag = CMDTAG_ALTER_COLLATION;
			break;
		case OBJECT_COLUMN:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_CONVERSION:
			tag = CMDTAG_ALTER_CONVERSION;
			break;
		case OBJECT_DATABASE:
			tag = CMDTAG_ALTER_DATABASE;
			break;
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
			tag = CMDTAG_ALTER_DOMAIN;
			break;
		case OBJECT_EXTENSION:
			tag = CMDTAG_ALTER_EXTENSION;
			break;
		case OBJECT_FDW:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = CMDTAG_ALTER_SERVER;
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = CMDTAG_ALTER_FOREIGN_TABLE;
			break;
		case OBJECT_FUNCTION:
			tag = CMDTAG_ALTER_FUNCTION;
			break;
		case OBJECT_INDEX:
			tag = CMDTAG_ALTER_INDEX;
			break;
		case OBJECT_LANGUAGE:
			tag = CMDTAG_ALTER_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			tag = CMDTAG_ALTER_LARGE_OBJECT;
			break;
		case OBJECT_OPCLASS:
			tag = CMDTAG_ALTER_OPERATOR_CLASS;
			break;
		case OBJECT_OPERATOR:
			tag = CMDTAG_ALTER_OPERATOR;
			break;
		case OBJECT_OPFAMILY:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;
		case OBJECT_POLICY:
			tag = CMDTAG_ALTER_POLICY;
			break;
		case OBJECT_PROCEDURE:
			tag = CMDTAG_ALTER_PROCEDURE;
			break;
		case OBJECT_ROLE:
			tag = CMDTAG_ALTER_ROLE;
			break;
		case OBJECT_ROUTINE:
			tag = CMDTAG_ALTER_ROUTINE;
			break;
		case OBJECT_RULE:
			tag = CMDTAG_ALTER_RULE;
			break;
		case OBJECT_SCHEMA:
			tag = CMDTAG_ALTER_SCHEMA;
			break;
		case OBJECT_SEQUENCE:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;
		case OBJECT_TABLE:
		case OBJECT_TABCONSTRAINT:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_TABLESPACE:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;
		case OBJECT_TRIGGER:
			tag = CMDTAG_ALTER_TRIGGER;
			break;
		case OBJECT_EVENT_TRIGGER:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;
		case OBJECT_TSCONFIGURATION:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;
		case OBJECT_TSDICTIONARY:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;
		case OBJECT_TSPARSER:
			tag = CMDTAG_ALTER_TEXT_SEARCH_PARSER;
			break;
		case OBJECT_TSTEMPLATE:
			tag = CMDTAG_ALTER_TEXT_SEARCH_TEMPLATE;
			break;
		case OBJECT_TYPE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_VIEW:
			tag = CMDTAG_ALTER_VIEW;
			break;
		case OBJECT_MATVIEW:
			tag = CMDTAG_ALTER_MATERIALIZED_VIEW;
			break;
		case OBJECT_PUBLICATION:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;
		case OBJECT_SUBSCRIPTION:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;
		case OBJECT_STATISTIC_EXT:
			tag = CMDTAG_ALTER_STATISTICS;
			break;
		default:
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		为命令操作获取 CommandTag 的实用函数，   
 *		输入可为未分析的原始解析树、已分析 Query 或 
 *		PlannedStmt。   
 *
 * 它必须处理所有命令类型，但绝大多数都是      
 * 实用命令，故将其留在此处似乎更为合理。      
 */
CommandTag
CreateCommandTag(Node *parsetree)
{
	CommandTag	tag;

	switch (nodeTag(parsetree))
	{
			/* 若给定的是 RawStmt 则递归*/
		case T_RawStmt:
			tag = CreateCommandTag(((RawStmt *) parsetree)->stmt);
			break;

			/* 原始可规划查询 */
		case T_InsertStmt:
			tag = CMDTAG_INSERT;
			break;

		case T_DeleteStmt:
			tag = CMDTAG_DELETE;
			break;

		case T_UpdateStmt:
			tag = CMDTAG_UPDATE;
			break;

		case T_MergeStmt:
			tag = CMDTAG_MERGE;
			break;

		case T_SelectStmt:
			tag = CMDTAG_SELECT;
			break;

		case T_PLAssignStmt:
			tag = CMDTAG_SELECT;
			break;

			/* 实用命令——原始或加工后皆相同     */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = CMDTAG_BEGIN;
						break;

					case TRANS_STMT_START:
						tag = CMDTAG_START_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT:
						tag = CMDTAG_COMMIT;
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = CMDTAG_ROLLBACK;
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = CMDTAG_SAVEPOINT;
						break;

					case TRANS_STMT_RELEASE:
						tag = CMDTAG_RELEASE;
						break;

					case TRANS_STMT_PREPARE:
						tag = CMDTAG_PREPARE_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = CMDTAG_COMMIT_PREPARED;
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = CMDTAG_ROLLBACK_PREPARED;
						break;

					default:
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = CMDTAG_DECLARE_CURSOR;
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = CMDTAG_CLOSE_CURSOR_ALL;
				else
					tag = CMDTAG_CLOSE_CURSOR;
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? CMDTAG_MOVE : CMDTAG_FETCH;
			}
			break;

		case T_CreateDomainStmt:
			tag = CMDTAG_CREATE_DOMAIN;
			break;

		case T_CreateSchemaStmt:
			tag = CMDTAG_CREATE_SCHEMA;
			break;

		case T_CreateStmt:
			tag = CMDTAG_CREATE_TABLE;
			break;

		case T_CreateTableSpaceStmt:
			tag = CMDTAG_CREATE_TABLESPACE;
			break;

		case T_DropTableSpaceStmt:
			tag = CMDTAG_DROP_TABLESPACE;
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;

		case T_CreateExtensionStmt:
			tag = CMDTAG_CREATE_EXTENSION;
			break;

		case T_AlterExtensionStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_AlterExtensionContentsStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_CreateFdwStmt:
			tag = CMDTAG_CREATE_FOREIGN_DATA_WRAPPER;
			break;

		case T_AlterFdwStmt:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;

		case T_CreateForeignServerStmt:
			tag = CMDTAG_CREATE_SERVER;
			break;

		case T_AlterForeignServerStmt:
			tag = CMDTAG_ALTER_SERVER;
			break;

		case T_CreateUserMappingStmt:
			tag = CMDTAG_CREATE_USER_MAPPING;
			break;

		case T_AlterUserMappingStmt:
			tag = CMDTAG_ALTER_USER_MAPPING;
			break;

		case T_DropUserMappingStmt:
			tag = CMDTAG_DROP_USER_MAPPING;
			break;

		case T_CreateForeignTableStmt:
			tag = CMDTAG_CREATE_FOREIGN_TABLE;
			break;

		case T_ImportForeignSchemaStmt:
			tag = CMDTAG_IMPORT_FOREIGN_SCHEMA;
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = CMDTAG_DROP_TABLE;
					break;
				case OBJECT_SEQUENCE:
					tag = CMDTAG_DROP_SEQUENCE;
					break;
				case OBJECT_VIEW:
					tag = CMDTAG_DROP_VIEW;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_DROP_MATERIALIZED_VIEW;
					break;
				case OBJECT_INDEX:
					tag = CMDTAG_DROP_INDEX;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_DROP_TYPE;
					break;
				case OBJECT_DOMAIN:
					tag = CMDTAG_DROP_DOMAIN;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_DROP_COLLATION;
					break;
				case OBJECT_CONVERSION:
					tag = CMDTAG_DROP_CONVERSION;
					break;
				case OBJECT_SCHEMA:
					tag = CMDTAG_DROP_SCHEMA;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_DROP_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_DROP_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_DROP_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_DROP_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = CMDTAG_DROP_FOREIGN_TABLE;
					break;
				case OBJECT_EXTENSION:
					tag = CMDTAG_DROP_EXTENSION;
					break;
				case OBJECT_FUNCTION:
					tag = CMDTAG_DROP_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_DROP_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_DROP_ROUTINE;
					break;
				case OBJECT_AGGREGATE:
					tag = CMDTAG_DROP_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_DROP_OPERATOR;
					break;
				case OBJECT_LANGUAGE:
					tag = CMDTAG_DROP_LANGUAGE;
					break;
				case OBJECT_CAST:
					tag = CMDTAG_DROP_CAST;
					break;
				case OBJECT_TRIGGER:
					tag = CMDTAG_DROP_TRIGGER;
					break;
				case OBJECT_EVENT_TRIGGER:
					tag = CMDTAG_DROP_EVENT_TRIGGER;
					break;
				case OBJECT_RULE:
					tag = CMDTAG_DROP_RULE;
					break;
				case OBJECT_FDW:
					tag = CMDTAG_DROP_FOREIGN_DATA_WRAPPER;
					break;
				case OBJECT_FOREIGN_SERVER:
					tag = CMDTAG_DROP_SERVER;
					break;
				case OBJECT_OPCLASS:
					tag = CMDTAG_DROP_OPERATOR_CLASS;
					break;
				case OBJECT_OPFAMILY:
					tag = CMDTAG_DROP_OPERATOR_FAMILY;
					break;
				case OBJECT_POLICY:
					tag = CMDTAG_DROP_POLICY;
					break;
				case OBJECT_TRANSFORM:
					tag = CMDTAG_DROP_TRANSFORM;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_DROP_ACCESS_METHOD;
					break;
				case OBJECT_PUBLICATION:
					tag = CMDTAG_DROP_PUBLICATION;
					break;
				case OBJECT_STATISTIC_EXT:
					tag = CMDTAG_DROP_STATISTICS;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_TruncateStmt:
			tag = CMDTAG_TRUNCATE_TABLE;
			break;

		case T_CommentStmt:
			tag = CMDTAG_COMMENT;
			break;

		case T_SecLabelStmt:
			tag = CMDTAG_SECURITY_LABEL;
			break;

		case T_CopyStmt:
			tag = CMDTAG_COPY;
			break;

		case T_RenameStmt:

			/*
			 * 列被重命名时，命令标签会依据其关系类型创建
			 *              
			 */
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType == OBJECT_COLUMN ?
											((RenameStmt *) parsetree)->relationType :
											((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectDependsStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectDependsStmt *) parsetree)->objectType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableMoveAllStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableMoveAllStmt *) parsetree)->objtype);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->objtype);
			break;

		case T_AlterDomainStmt:
			tag = CMDTAG_ALTER_DOMAIN;
			break;

		case T_AlterFunctionStmt:
			switch (((AlterFunctionStmt *) parsetree)->objtype)
			{
				case OBJECT_FUNCTION:
					tag = CMDTAG_ALTER_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_ALTER_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_ALTER_ROUTINE;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT : CMDTAG_REVOKE;
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT_ROLE : CMDTAG_REVOKE_ROLE;
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = CMDTAG_ALTER_DEFAULT_PRIVILEGES;
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = CMDTAG_CREATE_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_CREATE_OPERATOR;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_CREATE_TYPE;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_CREATE_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_CREATE_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_CREATE_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_CREATE_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_CREATE_COLLATION;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_CREATE_ACCESS_METHOD;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CompositeTypeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateEnumStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateRangeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_AlterEnumStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_ViewStmt:
			tag = CMDTAG_CREATE_VIEW;
			break;

		case T_CreateFunctionStmt:
			if (((CreateFunctionStmt *) parsetree)->is_procedure)
				tag = CMDTAG_CREATE_PROCEDURE;
			else
				tag = CMDTAG_CREATE_FUNCTION;
			break;

		case T_IndexStmt:
			tag = CMDTAG_CREATE_INDEX;
			break;

		case T_RuleStmt:
			tag = CMDTAG_CREATE_RULE;
			break;

		case T_CreateSeqStmt:
			tag = CMDTAG_CREATE_SEQUENCE;
			break;

		case T_AlterSeqStmt:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;

		case T_DoStmt:
			tag = CMDTAG_DO;
			break;

		case T_CreatedbStmt:
			tag = CMDTAG_CREATE_DATABASE;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			tag = CMDTAG_ALTER_DATABASE;
			break;

		case T_DropdbStmt:
			tag = CMDTAG_DROP_DATABASE;
			break;

		case T_NotifyStmt:
			tag = CMDTAG_NOTIFY;
			break;

		case T_ListenStmt:
			tag = CMDTAG_LISTEN;
			break;

		case T_UnlistenStmt:
			tag = CMDTAG_UNLISTEN;
			break;

		case T_LoadStmt:
			tag = CMDTAG_LOAD;
			break;

		case T_CallStmt:
			tag = CMDTAG_CALL;
			break;

		case T_ClusterStmt:
			tag = CMDTAG_CLUSTER;
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->is_vacuumcmd)
				tag = CMDTAG_VACUUM;
			else
				tag = CMDTAG_ANALYZE;
			break;

		case T_ExplainStmt:
			tag = CMDTAG_EXPLAIN;
			break;

		case T_CreateTableAsStmt:
			switch (((CreateTableAsStmt *) parsetree)->objtype)
			{
				case OBJECT_TABLE:
					if (((CreateTableAsStmt *) parsetree)->is_select_into)
						tag = CMDTAG_SELECT_INTO;
					else
						tag = CMDTAG_CREATE_TABLE_AS;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_CREATE_MATERIALIZED_VIEW;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_RefreshMatViewStmt:
			tag = CMDTAG_REFRESH_MATERIALIZED_VIEW;
			break;

		case T_AlterSystemStmt:
			tag = CMDTAG_ALTER_SYSTEM;
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = CMDTAG_SET;
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = CMDTAG_RESET;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_VariableShowStmt:
			tag = CMDTAG_SHOW;
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = CMDTAG_DISCARD_ALL;
					break;
				case DISCARD_PLANS:
					tag = CMDTAG_DISCARD_PLANS;
					break;
				case DISCARD_TEMP:
					tag = CMDTAG_DISCARD_TEMP;
					break;
				case DISCARD_SEQUENCES:
					tag = CMDTAG_DISCARD_SEQUENCES;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CreateTransformStmt:
			tag = CMDTAG_CREATE_TRANSFORM;
			break;

		case T_CreateTrigStmt:
			tag = CMDTAG_CREATE_TRIGGER;
			break;

		case T_CreateEventTrigStmt:
			tag = CMDTAG_CREATE_EVENT_TRIGGER;
			break;

		case T_AlterEventTrigStmt:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;

		case T_CreatePLangStmt:
			tag = CMDTAG_CREATE_LANGUAGE;
			break;

		case T_CreateRoleStmt:
			tag = CMDTAG_CREATE_ROLE;
			break;

		case T_AlterRoleStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_AlterRoleSetStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_DropRoleStmt:
			tag = CMDTAG_DROP_ROLE;
			break;

		case T_DropOwnedStmt:
			tag = CMDTAG_DROP_OWNED;
			break;

		case T_ReassignOwnedStmt:
			tag = CMDTAG_REASSIGN_OWNED;
			break;

		case T_LockStmt:
			tag = CMDTAG_LOCK_TABLE;
			break;

		case T_ConstraintsSetStmt:
			tag = CMDTAG_SET_CONSTRAINTS;
			break;

		case T_CheckPointStmt:
			tag = CMDTAG_CHECKPOINT;
			break;

		case T_ReindexStmt:
			tag = CMDTAG_REINDEX;
			break;

		case T_CreateConversionStmt:
			tag = CMDTAG_CREATE_CONVERSION;
			break;

		case T_CreateCastStmt:
			tag = CMDTAG_CREATE_CAST;
			break;

		case T_CreateOpClassStmt:
			tag = CMDTAG_CREATE_OPERATOR_CLASS;
			break;

		case T_CreateOpFamilyStmt:
			tag = CMDTAG_CREATE_OPERATOR_FAMILY;
			break;

		case T_AlterOpFamilyStmt:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;

		case T_AlterOperatorStmt:
			tag = CMDTAG_ALTER_OPERATOR;
			break;

		case T_AlterTypeStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_AlterTSDictionaryStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;

		case T_AlterTSConfigurationStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;

		case T_CreatePolicyStmt:
			tag = CMDTAG_CREATE_POLICY;
			break;

		case T_AlterPolicyStmt:
			tag = CMDTAG_ALTER_POLICY;
			break;

		case T_CreateAmStmt:
			tag = CMDTAG_CREATE_ACCESS_METHOD;
			break;

		case T_CreatePublicationStmt:
			tag = CMDTAG_CREATE_PUBLICATION;
			break;

		case T_AlterPublicationStmt:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;

		case T_CreateSubscriptionStmt:
			tag = CMDTAG_CREATE_SUBSCRIPTION;
			break;

		case T_AlterSubscriptionStmt:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;

		case T_DropSubscriptionStmt:
			tag = CMDTAG_DROP_SUBSCRIPTION;
			break;

		case T_AlterCollationStmt:
			tag = CMDTAG_ALTER_COLLATION;
			break;

		case T_PrepareStmt:
			tag = CMDTAG_PREPARE;
			break;

		case T_ExecuteStmt:
			tag = CMDTAG_EXECUTE;
			break;

		case T_CreateStatsStmt:
			tag = CMDTAG_CREATE_STATISTICS;
			break;

		case T_AlterStatsStmt:
			tag = CMDTAG_ALTER_STATISTICS;
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = CMDTAG_DEALLOCATE_ALL;
				else
					tag = CMDTAG_DEALLOCATE;
			}
			break;

			/* 已规划的查询      */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * 我们在此稍费心思，让结果在关于只读
						 * 语句的报错中更有用。               
						 *           
						 */
						if (stmt->rowMarks != NIL)
						{
							/* 并非 100%，但大概足够接近*/
							switch (((PlanRowMark *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_SELECT;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

			/* 已解析并重写但尚未规划的查询   */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * 我们在此稍费心思，让结果在关于只读
						 * 语句的报错中更有用。               
						 *           
						 */
						if (stmt->rowMarks != NIL)
						{
							/* 并非 100%，但大概足够接近*/
							switch (((RowMarkClause *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_UNKNOWN;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		为命令获取最低 log_statement 级别的实用函数， 
 *		输入可为未分析的原始解析树、已分析 Query 或 
 *		PlannedStmt。   
 *
 * 它必须处理所有命令类型，但绝大多数都是      
 * 实用命令，故将其留在此处似乎更为合理。      
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* 若给定的是 RawStmt 则递归*/
		case T_RawStmt:
			lev = GetCommandLogLevel(((RawStmt *) parsetree)->stmt);
			break;

			/* 原始可规划查询 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;	/* 查询建表*/
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PLAssignStmt:
			lev = LOGSTMT_ALL;
			break;

			/* 实用命令——原始或加工后皆相同     */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_ImportForeignSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* 透过 PREPARE 查看其包含的 stmt       */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* 透过 EXECUTE 查看所引用的 stmt         */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps && ps->plansource->raw_parse_tree)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree->stmt);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectDependsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOperatorStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableMoveAllStmt:
		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRangeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CallStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/* 透过 EXPLAIN ANALYZE 查看其包含的 stmt        */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/* 勿 "break"，因 explain.c 会取最后的值      */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/* 普通 EXPLAIN 意义不大。     */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_CreateTableAsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RefreshMatViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSystemStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* 这该算 DDL 吗？*/
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTransformStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateAmStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterCollationStmt:
			lev = LOGSTMT_DDL;
			break;

			/* 已规划的查询      */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* 已解析并重写但尚未规划的查询   */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}
