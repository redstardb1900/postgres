/*-------------------------------------------------------------------------
 *
 * spi.c
 *				服务器编程接口       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/spi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/printtup.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi_priv.h"
#include "miscadmin.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * 这些全局变量是 SPI 函数 API 的一部分（这一 API      
 * 选择很糟糕，但为时已晚）。为降低 SPI 调用者相互  
 * 干扰的风险，会在进入/退出 SPI 嵌套层级时保存并  
 * 恢复这些变量。                     
 */
uint64		SPI_processed = 0;
SPITupleTable *SPI_tuptable = NULL;
int			SPI_result = 0;

static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int	_SPI_stack_depth = 0;	/* _SPI_stack 的已分配大小*/
static int	_SPI_connected = -1;	/* 当前栈索引     */

typedef struct SPICallbackArg
{
	const char *query;
	RawParseMode mode;
} SPICallbackArg;

static Portal SPI_cursor_open_internal(const char *name, SPIPlanPtr plan,
									   ParamListInfo paramLI, bool read_only);

static void _SPI_prepare_plan(const char *src, SPIPlanPtr plan);

static void _SPI_prepare_oneshot_plan(const char *src, SPIPlanPtr plan);

static int	_SPI_execute_plan(SPIPlanPtr plan, const SPIExecuteOptions *options,
							  Snapshot snapshot, Snapshot crosscheck_snapshot,
							  bool fire_triggers);

static ParamListInfo _SPI_convert_params(int nargs, Oid *argtypes,
										 Datum *Values, const char *Nulls);

static int	_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, uint64 tcount);

static void _SPI_error_callback(void *arg);

static void _SPI_cursor_operation(Portal portal,
								  FetchDirection direction, long count,
								  DestReceiver *dest);

static SPIPlanPtr _SPI_make_plan_non_temp(SPIPlanPtr plan);
static SPIPlanPtr _SPI_save_plan(SPIPlanPtr plan);

static int	_SPI_begin_call(bool use_exec);
static int	_SPI_end_call(bool use_exec);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);


/* =================== 接口函数 ===================        */

int
SPI_connect(void)
{
	return SPI_connect_ext(0);
}

int
SPI_connect_ext(int options)
{
	int			newdepth;

	/* 必要时扩大栈         */
	if (_SPI_stack == NULL)
	{
		if (_SPI_connected != -1 || _SPI_stack_depth != 0)
			elog(ERROR, "SPI stack corrupted");
		newdepth = 16;
		_SPI_stack = (_SPI_connection *)
			MemoryContextAlloc(TopMemoryContext,
							   newdepth * sizeof(_SPI_connection));
		_SPI_stack_depth = newdepth;
	}
	else
	{
		if (_SPI_stack_depth <= 0 || _SPI_stack_depth <= _SPI_connected)
			elog(ERROR, "SPI stack corrupted");
		if (_SPI_stack_depth == _SPI_connected + 1)
		{
			newdepth = _SPI_stack_depth * 2;
			_SPI_stack = (_SPI_connection *)
				repalloc(_SPI_stack,
						 newdepth * sizeof(_SPI_connection));
			_SPI_stack_depth = newdepth;
		}
	}

	/* 进入新的栈层级 */
	_SPI_connected++;
	Assert(_SPI_connected >= 0 && _SPI_connected < _SPI_stack_depth);

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;
	_SPI_current->execSubid = InvalidSubTransactionId;
	slist_init(&_SPI_current->tuptables);
	_SPI_current->procCxt = NULL;	/* 以防创建它们失败      */
	_SPI_current->execCxt = NULL;
	_SPI_current->connectSubid = GetCurrentSubTransactionId();
	_SPI_current->queryEnv = NULL;
	_SPI_current->atomic = (options & SPI_OPT_NONATOMIC ? false : true);
	_SPI_current->internal_xact = false;
	_SPI_current->outer_processed = SPI_processed;
	_SPI_current->outer_tuptable = SPI_tuptable;
	_SPI_current->outer_result = SPI_result;

	/*
	 * 为该过程创建内存上下文        
	 *
	 * 原子上下文（正常情况）下使用 TopTransactionContext，
	 * 否则使用 PortalContext，使其存活跨越事务       
	 * 边界。  
	 *
	 * XXX 若一律用 PortalContext 作为父上下文会更好，但    
	 * 可能并不处于 portal 内（考虑延迟触发器执行）。也许
	 * CurTransactionContext 是个选项？目前这一点无关紧要，    
	 * 因为我们会在 AtEOSubXact_SPI() 中显式清理；               
	 * 另见 AtEOXact_SPI()。    
	 */
	_SPI_current->procCxt = AllocSetContextCreate(_SPI_current->atomic ? TopTransactionContext : PortalContext,
												  "SPI Proc",
												  ALLOCSET_DEFAULT_SIZES);
	_SPI_current->execCxt = AllocSetContextCreate(_SPI_current->atomic ? TopTransactionContext : _SPI_current->procCxt,
												  "SPI Exec",
												  ALLOCSET_DEFAULT_SIZES);
	/* ... 并切换到过程上下文       */
	_SPI_current->savedcxt = MemoryContextSwitchTo(_SPI_current->procCxt);

	/*
	 * 重置这些 API 全局变量，使当前调用者不会意外地    
	 * 依赖外层调用者的状态。  
	 */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	SPI_result = 0;

	return SPI_OK_CONNECT;
}

int
SPI_finish(void)
{
	int			res;

	res = _SPI_begin_call(false);	/* 仅检查是否已连接   */
	if (res < 0)
		return res;

	/* 把内存上下文恢复为过程调用前的状态    */
	MemoryContextSwitchTo(_SPI_current->savedcxt);

	/* 释放在过程调用中使用的内存（包括 tuptable）*/
	MemoryContextDelete(_SPI_current->execCxt);
	_SPI_current->execCxt = NULL;
	MemoryContextDelete(_SPI_current->procCxt);
	_SPI_current->procCxt = NULL;

	/*
	 * 恢复外层的 API 全局变量，尤其是 SPI_tuptable，它可能 
	 * 正指向刚删除的 tuptable。  
	 */
	SPI_processed = _SPI_current->outer_processed;
	SPI_tuptable = _SPI_current->outer_tuptable;
	SPI_result = _SPI_current->outer_result;

	/* 退出栈层级  */
	_SPI_connected--;
	if (_SPI_connected < 0)
		_SPI_current = NULL;
	else
		_SPI_current = &(_SPI_stack[_SPI_connected]);

	return SPI_OK_FINISH;
}

/*
 * SPI_start_transaction 是空操作，保留是为了向后兼容。
 * SPI 调用者总是处于事务之中。      
 */
void
SPI_start_transaction(void)
{
}

static void
_SPI_commit(bool chain)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	SavedTransactionCharacteristics savetc;

	/*
	 * 若当前上下文不允许终止事务，则发出抱怨。   
	 * （注意：此处与 _SPI_rollback 应当是仅有的两个抛出   
	 * ERRCODE_INVALID_TRANSACTION_TERMINATION 的地方，                   
	 * 这样调用者测试时便能确知发生了什么。） 
	 */
	if (_SPI_current->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("invalid transaction termination")));

	/*
	 * 这一限制是基于 SPI 之上的 PL 实现所要求的。        
	 * 它们用子事务建立异常块，出错时块内状态应当       
	 * 被一起回滚。                                            
	 * 在此类块中终止顶层事务违背该理念。未来的          
	 * PL 实现对此可能有不同想法，若那样这一限制       
	 * 就得细化，或者把检查移出 SPI、下放到各 PL。     
	 * 不过要注意，下面的代码依赖于不在子事务内           
	 * 这一前提。                      
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("cannot commit while a subtransaction is active")));

	if (chain)
		SaveTransactionCharacteristics(&savetc);

	/* 捕获 COMMIT 期间发生的任何错误   */
	PG_TRY();
	{
		/* 保护当前 SPI 栈条目不被删除           */
		_SPI_current->internal_xact = true;

		/*
		 * 占住任何 PL 可能正在使用的钉住 portal。我们必须  
		 * 要在改变事务状态前做这事，因为那将运行  
		 * 可能抛出错误的用户代码。        
		 */
		HoldPinnedPortals();

		/* 释放与 portal 关联的快照          */
		ForgetPortalSnapshots();

		/* 执行操作*/
		CommitTransactionCommand();

		/* 立即启动一个新事务         */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics(&savetc);

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* 错误信息存入调用者上下文*/
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * 中止失败的事务。若这一步也失败，那就只能
		 * 把错误向外传播……我们对此无能为力。      
		 */
		AbortCurrentTransaction();

		/* ……再启动新事务*/
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics(&savetc);

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;

		/* 既然事务已清理完毕，重新抛出该错误           */
		ReThrowError(edata);
	}
	PG_END_TRY();
}

void
SPI_commit(void)
{
	_SPI_commit(false);
}

void
SPI_commit_and_chain(void)
{
	_SPI_commit(true);
}

static void
_SPI_rollback(bool chain)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	SavedTransactionCharacteristics savetc;

	/* 见 _SPI_commit() 中注释   */
	if (_SPI_current->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("invalid transaction termination")));

	/* 见 _SPI_commit() 中注释   */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
				 errmsg("cannot roll back while a subtransaction is active")));

	if (chain)
		SaveTransactionCharacteristics(&savetc);

	/* 捕获 ROLLBACK 期间发生的任何错误   */
	PG_TRY();
	{
		/* 保护当前 SPI 栈条目不被删除           */
		_SPI_current->internal_xact = true;

		/*
		 * 占住任何 PL 可能正在使用的钉住 portal。我们必须  
		 * 要在改变事务状态前做这事，因为那将运行  
		 * 可能抛错的用户代码；反正也无法在已中止   
		 * 事务里运行。                                
		 */
		HoldPinnedPortals();

		/* 释放与 portal 关联的快照          */
		ForgetPortalSnapshots();

		/* 执行操作*/
		AbortCurrentTransaction();

		/* 立即启动一个新事务         */
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics(&savetc);

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* 错误信息存入调用者上下文*/
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * 重试中止失败的事务。若这步也失败，就只能  
		 * 把错误向外传播……对此我们无能为                   
		 * 力
		 */
		AbortCurrentTransaction();

		/* ……再启动新事务*/
		StartTransactionCommand();
		if (chain)
			RestoreTransactionCharacteristics(&savetc);

		MemoryContextSwitchTo(oldcontext);

		_SPI_current->internal_xact = false;

		/* 既然事务已清理完毕，重新抛出该错误           */
		ReThrowError(edata);
	}
	PG_END_TRY();
}

void
SPI_rollback(void)
{
	_SPI_rollback(false);
}

void
SPI_rollback_and_chain(void)
{
	_SPI_rollback(true);
}

/*
 * 在事务提交或中止时清理 SPI 状态。   
 */
void
AtEOXact_SPI(bool isCommit)
{
	bool		found = false;

	/*
	 * 弹出各栈条目，遇到标记了 internal_xact 的条目就停止
	 * （它属于 SPI_commit/SPI_rollback 的调用者）。  
	 */
	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->internal_xact)
			break;

		found = true;

		/*
		 * 无需显式释放过程的内存上下文，因为父上下文   
		 * 消失时它们会自动随之消失；参见 SPI_connect_ext 中
		 * 的说明。             
		 */

		/*
		 * 恢复外层全局变量并弹出栈条目。与 SPI_finish()  
		 * 不同，我们不会冒切换到可能已消失的内存上下    
		 * 文的风险。 
		 */
		SPI_processed = connection->outer_processed;
		SPI_tuptable = connection->outer_tuptable;
		SPI_result = connection->outer_result;

		_SPI_connected--;
		if (_SPI_connected < 0)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
	}

	/* 只有 ABORT 时才应发现待弹出条目。      */
	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("transaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));
}

/*
 * 在子事务提交或中止时清理 SPI 状态。   
 *
 * 提交时，不应有当前子事务遗留的未关闭条目；若   
 * 发现任何此类条目则发出警告。                     
 */
void
AtEOSubXact_SPI(bool isCommit, SubTransactionId mySubid)
{
	bool		found = false;

	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->connectSubid != mySubid)
			break;				/* 其下也不可能再有其他条目 */

		if (connection->internal_xact)
			break;

		found = true;

		/*
		 * 显式释放过程内存（参见 SPI_connect 中的说明）
		 */
		if (connection->execCxt)
		{
			MemoryContextDelete(connection->execCxt);
			connection->execCxt = NULL;
		}
		if (connection->procCxt)
		{
			MemoryContextDelete(connection->procCxt);
			connection->procCxt = NULL;
		}

		/*
		 * 恢复外层全局变量并弹出栈条目。与 SPI_finish()  
		 * 不同，我们不会冒切换到可能已消失的内存上下    
		 * 文的风险。 
		 */
		SPI_processed = connection->outer_processed;
		SPI_tuptable = connection->outer_tuptable;
		SPI_result = connection->outer_result;

		_SPI_connected--;
		if (_SPI_connected < 0)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
	}

	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("subtransaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	/*
	 * 如果我们正在中止一个子事务，且其外围存在未关  
	 * 闭的 SPI 上下文，则做清理以防内存泄漏。    
	 */
	if (_SPI_current && !isCommit)
	{
		slist_mutable_iter siter;

		/*
		 * 如果当前执行器操作是在当前子事务内启动的，则 
		 * 丢弃执行器状态（即强制 _SPI_end_call(true)）。       
		 */
		if (_SPI_current->execSubid >= mySubid)
		{
			_SPI_current->execSubid = InvalidSubTransactionId;
			MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
		}

		/* 丢弃当前子事务内创建的任何元组表           */
		slist_foreach_modify(siter, &_SPI_current->tuptables)
		{
			SPITupleTable *tuptable;

			tuptable = slist_container(SPITupleTable, next, siter.cur);
			if (tuptable->subid >= mySubid)
			{
				/*
				 * 若在此用 SPI_freetuptable()，其对 tuptables 列表 
				 * 的内部搜索将使本操作变成 O(N^2)。      
				 * 所以，这里只是手动释放该 tuptable。      
				 * 应与 SPI_freetuptable() 相符。
				 */
				slist_delete_current(&siter);
				if (tuptable == _SPI_current->tuptable)
					_SPI_current->tuptable = NULL;
				if (tuptable == SPI_tuptable)
					SPI_tuptable = NULL;
				MemoryContextDelete(tuptable->tuptabcxt);
			}
		}
	}
}

/*
 * 我们是否正在过程（即非原子 SPI 上下文）内执行？   
 */
bool
SPI_inside_nonatomic_context(void)
{
	if (_SPI_current == NULL)
		return false;			/* 完全不在 SPI 上下文中 */
	/* 这些测试必须与 _SPI_commit 的原子性判定一致：  */
	if (_SPI_current->atomic)
		return false;			/* 原子性的（即函数而非过程） */
	if (IsSubTransaction())
		return false;			/* 若在子事务内，则是原子的  */
	return true;
}


/* 解析、规划并执行查询字符串 */
int
SPI_execute(const char *src, bool read_only, long tcount)
{
	_SPI_plan	plan;
	SPIExecuteOptions options;
	int			res;

	if (src == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;

	_SPI_prepare_oneshot_plan(src, &plan);

	memset(&options, 0, sizeof(options));
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(&plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* SPI_execute 的过时版本     */
int
SPI_exec(const char *src, long tcount)
{
	return SPI_execute(src, false, tcount);
}

/* 解析、规划并执行查询字符串，带可扩展的选项  */
int
SPI_execute_extended(const char *src,
					 const SPIExecuteOptions *options)
{
	int			res;
	_SPI_plan	plan;

	if (src == NULL || options == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;
	if (options->params)
	{
		plan.parserSetup = options->params->parserSetup;
		plan.parserSetupArg = options->params->parserSetupArg;
	}

	_SPI_prepare_oneshot_plan(src, &plan);

	res = _SPI_execute_plan(&plan, options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* 执行先前准备好的计划     */
int
SPI_execute_plan(SPIPlanPtr plan, Datum *Values, const char *Nulls,
				 bool read_only, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = _SPI_convert_params(plan->nargs, plan->argtypes,
										 Values, Nulls);
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* SPI_execute_plan 的过时版本     */
int
SPI_execp(SPIPlanPtr plan, Datum *Values, const char *Nulls, long tcount)
{
	return SPI_execute_plan(plan, Values, Nulls, false, tcount);
}

/* 执行先前准备好的计划     */
int
SPI_execute_plan_extended(SPIPlanPtr plan,
						  const SPIExecuteOptions *options)
{
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || options == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan(plan, options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/* 执行先前准备好的计划     */
int
SPI_execute_plan_with_paramlist(SPIPlanPtr plan, ParamListInfo params,
								bool read_only, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = params;
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

/*
 * SPI_execute_snapshot -- 与 SPI_execute_plan 相同，但允许调用者   
 * 精确指定使用哪些快照，快照将在此处注册。       
 * 此外，调用者可以指定把 AFTER 触发器排队为外层查询      
 * 的一部分，而不是在命令末尾                                     
 * 立即触发。    
 *
 * 此函数目前未在 spi.sgml 中记载，因为它只打算供 RI     
 * 触发器使用。     
 *
 * 传入 snapshot == InvalidSnapshot 将选择常规行为，即        
 * 为每个查询获取一个新快照。
 */
int
SPI_execute_snapshot(SPIPlanPtr plan,
					 Datum *Values, const char *Nulls,
					 Snapshot snapshot, Snapshot crosscheck_snapshot,
					 bool read_only, bool fire_triggers, long tcount)
{
	SPIExecuteOptions options;
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&options, 0, sizeof(options));
	options.params = _SPI_convert_params(plan->nargs, plan->argtypes,
										 Values, Nulls);
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(plan, &options,
							snapshot, crosscheck_snapshot,
							fire_triggers);

	_SPI_end_call(true);
	return res;
}

/*
 * SPI_execute_with_args -- 使用给定参数规划并执行查询         
 *
 * 这在功能上等同于先 SPI_prepare 再               
 * SPI_execute_plan.
 */
int
SPI_execute_with_args(const char *src,
					  int nargs, Oid *argtypes,
					  Datum *Values, const char *Nulls,
					  bool read_only, long tcount)
{
	int			res;
	_SPI_plan	plan;
	ParamListInfo paramLI;
	SPIExecuteOptions options;

	if (src == NULL || nargs < 0 || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (nargs > 0 && (argtypes == NULL || Values == NULL))
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = CURSOR_OPT_PARALLEL_OK;
	plan.nargs = nargs;
	plan.argtypes = argtypes;
	plan.parserSetup = NULL;
	plan.parserSetupArg = NULL;

	paramLI = _SPI_convert_params(nargs, argtypes,
								  Values, Nulls);

	_SPI_prepare_oneshot_plan(src, &plan);

	memset(&options, 0, sizeof(options));
	options.params = paramLI;
	options.read_only = read_only;
	options.tcount = tcount;

	res = _SPI_execute_plan(&plan, &options,
							InvalidSnapshot, InvalidSnapshot,
							true);

	_SPI_end_call(true);
	return res;
}

SPIPlanPtr
SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
	return SPI_prepare_cursor(src, nargs, argtypes, 0);
}

SPIPlanPtr
SPI_prepare_cursor(const char *src, int nargs, Oid *argtypes,
				   int cursorOptions)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL))
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = cursorOptions;
	plan.nargs = nargs;
	plan.argtypes = argtypes;
	plan.parserSetup = NULL;
	plan.parserSetupArg = NULL;

	_SPI_prepare_plan(src, &plan);

	/* 复制计划到过程上下文 */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

SPIPlanPtr
SPI_prepare_extended(const char *src,
					 const SPIPrepareOptions *options)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL || options == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = options->parseMode;
	plan.cursor_options = options->cursorOptions;
	plan.nargs = 0;
	plan.argtypes = NULL;
	plan.parserSetup = options->parserSetup;
	plan.parserSetupArg = options->parserSetupArg;

	_SPI_prepare_plan(src, &plan);

	/* 复制计划到过程上下文 */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

SPIPlanPtr
SPI_prepare_params(const char *src,
				   ParserSetupHook parserSetup,
				   void *parserSetupArg,
				   int cursorOptions)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = cursorOptions;
	plan.nargs = 0;
	plan.argtypes = NULL;
	plan.parserSetup = parserSetup;
	plan.parserSetupArg = parserSetupArg;

	_SPI_prepare_plan(src, &plan);

	/* 复制计划到过程上下文 */
	result = _SPI_make_plan_non_temp(&plan);

	_SPI_end_call(true);

	return result;
}

int
SPI_keepplan(SPIPlanPtr plan)
{
	ListCell   *lc;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC ||
		plan->saved || plan->oneshot)
		return SPI_ERROR_ARGUMENT;

	/*
	 * 把它标记为已保存，重新挂到 CacheMemoryContext 之下， 
	 * 并把各 CachedPlanSource 标记为已保存。该序列不会 
	 * 中途失败，因此没有长期内存泄漏的风险。       
	 */
	plan->saved = true;
	MemoryContextSetParent(plan->plancxt, CacheMemoryContext);

	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		SaveCachedPlan(plansource);
	}

	return 0;
}

SPIPlanPtr
SPI_saveplan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(false);	/* 不改变上下文   */
	if (SPI_result < 0)
		return NULL;

	newplan = _SPI_save_plan(plan);

	SPI_result = _SPI_end_call(false);

	return newplan;
}

int
SPI_freeplan(SPIPlanPtr plan)
{
	ListCell   *lc;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
		return SPI_ERROR_ARGUMENT;

	/* 释放这些 plancache 条目 */
	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		DropCachedPlan(plansource);
	}

	/* 接着清掉 _SPI_plan 及其 plancxt 中的附属数据        */
	MemoryContextDelete(plan->plancxt);

	return 0;
}

HeapTuple
SPI_copytuple(HeapTuple tuple)
{
	MemoryContext oldcxt;
	HeapTuple	ctuple;

	if (tuple == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	ctuple = heap_copytuple(tuple);

	MemoryContextSwitchTo(oldcxt);

	return ctuple;
}

HeapTupleHeader
SPI_returntuple(HeapTuple tuple, TupleDesc tupdesc)
{
	MemoryContext oldcxt;
	HeapTupleHeader dtup;

	if (tuple == NULL || tupdesc == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	/* 对 RECORD 结果，确保已分配 typmod               */
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	dtup = DatumGetHeapTupleHeader(heap_copy_tuple_as_datum(tuple, tupdesc));

	MemoryContextSwitchTo(oldcxt);

	return dtup;
}

HeapTuple
SPI_modifytuple(Relation rel, HeapTuple tuple, int natts, int *attnum,
				Datum *Values, const char *Nulls)
{
	MemoryContext oldcxt;
	HeapTuple	mtuple;
	int			numberOfAttributes;
	Datum	   *v;
	bool	   *n;
	int			i;

	if (rel == NULL || tuple == NULL || natts < 0 || attnum == NULL || Values == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_current == NULL)
	{
		SPI_result = SPI_ERROR_UNCONNECTED;
		return NULL;
	}

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	SPI_result = 0;

	numberOfAttributes = rel->rd_att->natts;
	v = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	n = (bool *) palloc(numberOfAttributes * sizeof(bool));

	/* 取出旧值和空值      */
	heap_deform_tuple(tuple, rel->rd_att, v, n);

	/* 替换值和空值       */
	for (i = 0; i < natts; i++)
	{
		if (attnum[i] <= 0 || attnum[i] > numberOfAttributes)
			break;
		v[attnum[i] - 1] = Values[i];
		n[attnum[i] - 1] = (Nulls && Nulls[i] == 'n');
	}

	if (i == natts)				/* *attnum 中无错误 */
	{
		mtuple = heap_form_tuple(rel->rd_att, v, n);

		/*
		 * 复制旧元组的标识信息：t_ctid、t_self，以及 OID     
		 * （如有）
		 */
		mtuple->t_data->t_ctid = tuple->t_data->t_ctid;
		mtuple->t_self = tuple->t_self;
		mtuple->t_tableOid = tuple->t_tableOid;
	}
	else
	{
		mtuple = NULL;
		SPI_result = SPI_ERROR_NOATTRIBUTE;
	}

	pfree(v);
	pfree(n);

	MemoryContextSwitchTo(oldcxt);

	return mtuple;
}

int
SPI_fnumber(TupleDesc tupdesc, const char *fname)
{
	int			res;
	const FormData_pg_attribute *sysatt;

	for (res = 0; res < tupdesc->natts; res++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, res);

		if (namestrcmp(&attr->attname, fname) == 0 &&
			!attr->attisdropped)
			return res + 1;
	}

	sysatt = SystemAttributeByName(fname);
	if (sysatt != NULL)
		return sysatt->attnum;

	/* SPI_ERROR_NOATTRIBUTE 与所有系统列号都不同           */
	return SPI_ERROR_NOATTRIBUTE;
}

char *
SPI_fname(TupleDesc tupdesc, int fnumber)
{
	const FormData_pg_attribute *att;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		att = TupleDescAttr(tupdesc, fnumber - 1);
	else
		att = SystemAttributeDefinition(fnumber);

	return pstrdup(NameStr(att->attname));
}

char *
SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
	Datum		val;
	bool		isnull;
	Oid			typoid,
				foutoid;
	bool		typisvarlena;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	val = heap_getattr(tuple, fnumber, tupdesc, &isnull);
	if (isnull)
		return NULL;

	if (fnumber > 0)
		typoid = TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber))->atttypid;

	getTypeOutputInfo(typoid, &foutoid, &typisvarlena);

	return OidOutputFunctionCall(foutoid, val);
}

Datum
SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		*isnull = true;
		return (Datum) NULL;
	}

	return heap_getattr(tuple, fnumber, tupdesc, isnull);
}

char *
SPI_gettype(TupleDesc tupdesc, int fnumber)
{
	Oid			typoid;
	HeapTuple	typeTuple;
	char	   *result;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		typoid = TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber))->atttypid;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));

	if (!HeapTupleIsValid(typeTuple))
	{
		SPI_result = SPI_ERROR_TYPUNKNOWN;
		return NULL;
	}

	result = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(typeTuple))->typname));
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * 获取某列的数据类型 OID。 
 *
 * typmod 和 typcollation 没有类似的函数。少数用到它们的   
 * 使用者应当直接检查 TupleDesc。      
 */
Oid
SPI_gettypeid(TupleDesc tupdesc, int fnumber)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return InvalidOid;
	}

	if (fnumber > 0)
		return TupleDescAttr(tupdesc, fnumber - 1)->atttypid;
	else
		return (SystemAttributeDefinition(fnumber))->atttypid;
}

char *
SPI_getrelname(Relation rel)
{
	return pstrdup(RelationGetRelationName(rel));
}

char *
SPI_getnspname(Relation rel)
{
	return get_namespace_name(RelationGetNamespace(rel));
}

void *
SPI_palloc(Size size)
{
	if (_SPI_current == NULL)
		elog(ERROR, "SPI_palloc called while not connected to SPI");

	return MemoryContextAlloc(_SPI_current->savedcxt, size);
}

void *
SPI_repalloc(void *pointer, Size size)
{
	/* 不必再关心该块原先在哪个上下文中……*/
	return repalloc(pointer, size);
}

void
SPI_pfree(void *pointer)
{
	/* 不必再关心该块原先在哪个上下文中……*/
	pfree(pointer);
}

Datum
SPI_datumTransfer(Datum value, bool typByVal, int typLen)
{
	MemoryContext oldcxt;
	Datum		result;

	if (_SPI_current == NULL)
		elog(ERROR, "SPI_datumTransfer called while not connected to SPI");

	oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);

	result = datumTransfer(value, typByVal, typLen);

	MemoryContextSwitchTo(oldcxt);

	return result;
}

void
SPI_freetuple(HeapTuple tuple)
{
	/* 不必关心该元组原先在哪个上下文中……*/
	heap_freetuple(tuple);
}

void
SPI_freetuptable(SPITupleTable *tuptable)
{
	bool		found = false;

	/* NULL 指针则忽略调用  */
	if (tuptable == NULL)
		return;

	/*
	 * 只在最上层的 SPI 上下文中搜索匹配的元组表。 
	 */
	if (_SPI_current != NULL)
	{
		slist_mutable_iter siter;

		/* 在活跃列表找到 tuptable 并移除     */
		slist_foreach_modify(siter, &_SPI_current->tuptables)
		{
			SPITupleTable *tt;

			tt = slist_container(SPITupleTable, next, siter.cur);
			if (tt == tuptable)
			{
				slist_delete_current(&siter);
				found = true;
				break;
			}
		}
	}

	/*
	 * 若未在最上层的 SPI 上下文中找到它，则拒绝删除。
	 * 这主要是防止重复删除，但也可能避免其他错误。  
	 * 因为漏删 tuptable 的最坏后果只是短暂内存泄漏，    
	 * 因此这里只发出 WARNING。                                  
	 */
	if (!found)
	{
		elog(WARNING, "attempt to delete invalid SPITupleTable %p", tuptable);
		return;
	}

	/* 为安全起见，重置可能指向 tuptable 的全局变量   */
	if (tuptable == _SPI_current->tuptable)
		_SPI_current->tuptable = NULL;
	if (tuptable == SPI_tuptable)
		SPI_tuptable = NULL;

	/* 释放 tuptable 所属的全部内存    */
	MemoryContextDelete(tuptable->tuptabcxt);
}


/*
 * SPI_cursor_open()
 *
 *	打开 prepared SPI 计划为 portal
 */
Portal
SPI_cursor_open(const char *name, SPIPlanPtr plan,
				Datum *Values, const char *Nulls,
				bool read_only)
{
	Portal		portal;
	ParamListInfo paramLI;

	/* 在调用者上下文中构建临时 ParamListInfo*/
	paramLI = _SPI_convert_params(plan->nargs, plan->argtypes,
								  Values, Nulls);

	portal = SPI_cursor_open_internal(name, plan, paramLI, read_only);

	/* 临时 ParamListInfo 使用完毕     */
	if (paramLI)
		pfree(paramLI);

	return portal;
}


/*
 * SPI_cursor_open_with_args()
 *
 * 解析并规划查询，再打开为 portal.   
 */
Portal
SPI_cursor_open_with_args(const char *name,
						  const char *src,
						  int nargs, Oid *argtypes,
						  Datum *Values, const char *Nulls,
						  bool read_only, int cursorOptions)
{
	Portal		result;
	_SPI_plan	plan;
	ParamListInfo paramLI;

	if (src == NULL || nargs < 0)
		elog(ERROR, "SPI_cursor_open_with_args called with invalid arguments");

	if (nargs > 0 && (argtypes == NULL || Values == NULL))
		elog(ERROR, "SPI_cursor_open_with_args called with missing parameters");

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		elog(ERROR, "SPI_cursor_open_with_args called while not connected");

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = cursorOptions;
	plan.nargs = nargs;
	plan.argtypes = argtypes;
	plan.parserSetup = NULL;
	plan.parserSetupArg = NULL;

	/* 在执行器上下文中构建临时 ParamListInfo*/
	paramLI = _SPI_convert_params(nargs, argtypes,
								  Values, Nulls);

	_SPI_prepare_plan(src, &plan);

	/* 计划无需复制；SPI_cursor_open_internal 会代为复制 */

	result = SPI_cursor_open_internal(name, &plan, paramLI, read_only);

	/* 然后清理 */
	_SPI_end_call(true);

	return result;
}


/*
 * SPI_cursor_open_with_paramlist()
 *
 *	与 SPI_cursor_open 类似，但参数(若有)以 ParamListInfo   
 *	传入，它支持动态确定参数集                               
 */
Portal
SPI_cursor_open_with_paramlist(const char *name, SPIPlanPtr plan,
							   ParamListInfo params, bool read_only)
{
	return SPI_cursor_open_internal(name, plan, params, read_only);
}

/* 解析一条查询并打开为游标  */
Portal
SPI_cursor_parse_open(const char *name,
					  const char *src,
					  const SPIParseOpenOptions *options)
{
	Portal		result;
	_SPI_plan	plan;

	if (src == NULL || options == NULL)
		elog(ERROR, "SPI_cursor_parse_open called with invalid arguments");

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		elog(ERROR, "SPI_cursor_parse_open called while not connected");

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.parse_mode = RAW_PARSE_DEFAULT;
	plan.cursor_options = options->cursorOptions;
	if (options->params)
	{
		plan.parserSetup = options->params->parserSetup;
		plan.parserSetupArg = options->params->parserSetupArg;
	}

	_SPI_prepare_plan(src, &plan);

	/* 计划无需复制；SPI_cursor_open_internal 会代为复制 */

	result = SPI_cursor_open_internal(name, &plan,
									  options->params, options->read_only);

	/* 然后清理 */
	_SPI_end_call(true);

	return result;
}


/*
 * SPI_cursor_open_internal()
 *
 *	SPI_cursor_open 各变体的公共代码
 */
static Portal
SPI_cursor_open_internal(const char *name, SPIPlanPtr plan,
						 ParamListInfo paramLI, bool read_only)
{
	CachedPlanSource *plansource;
	CachedPlan *cplan;
	List	   *stmt_list;
	char	   *query_string;
	Snapshot	snapshot;
	MemoryContext oldcontext;
	Portal		portal;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;

	/*
	 * 检查该计划是否是 Portal 代码会特判为                   
	 * 返回元组集。     
	 */
	if (!SPI_is_cursor_plan(plan))
	{
		/* 尽量给出恰当的错误消息*/
		const char *cmdtag;

		if (list_length(plan->plancache_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
					 errmsg("cannot open multi-query plan as cursor")));
		plansource = (CachedPlanSource *) linitial(plan->plancache_list);
		/* 未通过 SPI_is_cursor_plan() 的必是 SELECT INTO         */
		if (plansource->commandTag == CMDTAG_SELECT)
			cmdtag = "SELECT INTO";
		else
			cmdtag = GetCommandTagName(plansource->commandTag);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is name of a SQL command, eg INSERT */
				 errmsg("cannot open %s query as cursor", cmdtag)));
	}

	Assert(list_length(plan->plancache_list) == 1);
	plansource = (CachedPlanSource *) linitial(plan->plancache_list);

	/* 压入 SPI 栈     */
	if (_SPI_begin_call(true) < 0)
		elog(ERROR, "SPI_cursor_open called while not connected");

	/* 重置 SPI 结果（注意我们刻意不动 lastoid）     */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* 创建该 portal  */
	if (name == NULL || name[0] == '\0')
	{
		/* 使用随机的非冲突名称   */
		portal = CreateNewPortal();
	}
	else
	{
		/* 此路径下，若同名 portal 已存在则报错        */
		portal = CreatePortal(name, false, false);
	}

	/* 把计划的查询字符串复制进 portal  */
	query_string = MemoryContextStrdup(portal->portalContext,
									   plansource->query_string);

	/*
	 * 为 ereport() 设置错误回溯支持，以防 GetCachedPlan     
	 * 抛出错误。 
	 */
	spicallbackarg.query = plansource->query_string;
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * 注意：对于已保存的计划，在 GetCachedPlan 与        
	 * PortalDefineQuery 间绝不能失败，否则会泄漏 plancache     
	 * 引用计数。    
	 */

	/* 必要时重新规划并为 portal 增加引用计数    */
	cplan = GetCachedPlan(plansource, paramLI, NULL, _SPI_current->queryEnv);
	stmt_list = cplan->stmt_list;

	if (!plan->saved)
	{
		/*
		 * 我们不希望 portal 依赖未保存的 CachedPlanSource，     
		 * 故须把计划复制进 portal 上下文。此处发生错误 
		 * 会泄漏我们对该计划的引用计数，但不要紧，   
		 * 反正该计划未保存、本质上是临时性的。        
		 */
		oldcontext = MemoryContextSwitchTo(portal->portalContext);
		stmt_list = copyObject(stmt_list);
		MemoryContextSwitchTo(oldcontext);
		ReleaseCachedPlan(cplan, NULL);
		cplan = NULL;			/* portal 不应依赖 cplan        */
	}

	/*
	 * 设置该 portal. 
	 */
	PortalDefineQuery(portal,
					  NULL,		/* 无语句名称   */
					  query_string,
					  plansource->commandTag,
					  stmt_list,
					  cplan);

	/*
	 * 为 portal 设置选项。默认 SCROLL 类型的选择方式与      
	 * PerformCursorOpen 相同。  
	 */
	portal->cursorOptions = plan->cursor_options;
	if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL)))
	{
		if (list_length(stmt_list) == 1 &&
			linitial_node(PlannedStmt, stmt_list)->commandType != CMD_UTILITY &&
			linitial_node(PlannedStmt, stmt_list)->rowMarks == NIL &&
			ExecSupportsBackwardScan(linitial_node(PlannedStmt, stmt_list)->planTree))
			portal->cursorOptions |= CURSOR_OPT_SCROLL;
		else
			portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
	}

	/*
	 * 禁止在 SELECT FOR UPDATE 上使用 SCROLL。这与                  
	 * transformDeclareCursorStmt 检查并不重复，因为游标选项  
	 * 可能未经该处传来。 
	 */
	if (portal->cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (list_length(stmt_list) == 1 &&
			linitial_node(PlannedStmt, stmt_list)->commandType != CMD_UTILITY &&
			linitial_node(PlannedStmt, stmt_list)->rowMarks != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DECLARE SCROLL CURSOR ... FOR UPDATE/SHARE is not supported"),
					 errdetail("Scrollable cursors must be READ ONLY.")));
	}

	/* 让 portal 在执行时能够使用当前查询环境。              */
	portal->queryEnv = _SPI_current->queryEnv;

	/*
	 * 若被要求只读，就必须检查查询是否只读。这无法    
	 * 提前进行，因为需查看规划后的最终查询。（特别是 
	 * 我们不想在 GetCachedPlan 与 PortalDefineQuery 之间            
	 * 进行这一检查，因为在这两步之间抛出错误会泄漏  
	 * 我们的 plancache 引用计数。）          
	 */
	if (read_only)
	{
		ListCell   *lc;

		foreach(lc, stmt_list)
		{
			PlannedStmt *pstmt = lfirst_node(PlannedStmt, lc);

			if (!CommandIsReadOnly(pstmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
						 errmsg("%s is not allowed in a non-volatile function",
								CreateCommandName((Node *) pstmt))));
		}
	}

	/* 设置要使用的快照。 */
	if (read_only)
		snapshot = GetActiveSnapshot();
	else
	{
		CommandCounterIncrement();
		snapshot = GetTransactionSnapshot();
	}

	/*
	 * 若计划带参数，就把它们复制进 portal。注意，这一步
	 * 必须在重新验证计划之后进行，因为动态参数下，    
	 * 参数集合在重新解析期间可能已经发生变化。     
	 */
	if (paramLI)
	{
		oldcontext = MemoryContextSwitchTo(portal->portalContext);
		paramLI = copyParamList(paramLI);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * 启动 portal 执行。
	 */
	PortalStart(portal, paramLI, 0, snapshot);

	Assert(portal->strategy != PORTAL_MULTI_QUERY);

	/* 弹出错误上下文栈    */
	error_context_stack = spierrcontext.previous;

	/* 弹出 SPI 栈    */
	_SPI_end_call(true);

	/* 返回已创建的 portal */
	return portal;
}


/*
 * SPI_cursor_find()
 *
 *	查找现有已打开游标的 portal     
 */
Portal
SPI_cursor_find(const char *name)
{
	return GetPortalByName(name);
}


/*
 * SPI_cursor_fetch()
 *
 *	从游标中取行    
 */
void
SPI_cursor_fetch(Portal portal, bool forward, long count)
{
	_SPI_cursor_operation(portal,
						  forward ? FETCH_FORWARD : FETCH_BACKWARD, count,
						  CreateDestReceiver(DestSPI));
	/* 我们已知 DestSPI 接收器不需要 destroy 调用        */
}


/*
 * SPI_cursor_move()
 *
 *	游标内移动 
 */
void
SPI_cursor_move(Portal portal, bool forward, long count)
{
	_SPI_cursor_operation(portal,
						  forward ? FETCH_FORWARD : FETCH_BACKWARD, count,
						  None_Receiver);
}


/*
 * SPI_scroll_cursor_fetch()
 *
 *	从可滚动游标中取行      
 */
void
SPI_scroll_cursor_fetch(Portal portal, FetchDirection direction, long count)
{
	_SPI_cursor_operation(portal,
						  direction, count,
						  CreateDestReceiver(DestSPI));
	/* 我们已知 DestSPI 接收器不需要 destroy 调用        */
}


/*
 * SPI_scroll_cursor_move()
 *
 *	可滚动游标内移动   
 */
void
SPI_scroll_cursor_move(Portal portal, FetchDirection direction, long count)
{
	_SPI_cursor_operation(portal, direction, count, None_Receiver);
}


/*
 * SPI_cursor_close()
 *
 *	关闭游标  
 */
void
SPI_cursor_close(Portal portal)
{
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	PortalDrop(portal, false);
}

/*
 * 返回表示 argIndex 处参数类型 id 的 Oid。第一个            
 * 参数位于索引 0 处。
 */
Oid
SPI_getargtypeid(SPIPlanPtr plan, int argIndex)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC ||
		argIndex < 0 || argIndex >= plan->nargs)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return InvalidOid;
	}
	return plan->argtypes[argIndex];
}

/*
 * 返回已准备计划的参数个数。               
 */
int
SPI_getargcount(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return -1;
	}
	return plan->nargs;
}

/*
 * 若计划恰好只含一条命令，且该命令     
 * 会向调用者返回元组（如 SELECT 或                
 * INSERT ... RETURNING，但不含 SELECT ... INTO）。     
 * 本质上，该结果说明命令能否配合 SPI_cursor_open 使用
 *
 * 参数    
 *	  plan: 之前用 SPI_prepare 准备的计划       
 */
bool
SPI_is_cursor_plan(SPIPlanPtr plan)
{
	CachedPlanSource *plansource;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return false;
	}

	if (list_length(plan->plancache_list) != 1)
	{
		SPI_result = 0;
		return false;			/* 重写前命令数不恰好为 1  */
	}
	plansource = (CachedPlanSource *) linitial(plan->plancache_list);

	/*
	 * 过去在此强制重新验证缓存计划，但似乎不必要：   
	 * 失效可能意味着计划返回元组的 rowtype 发生变化， 
	 * 而不在于它是否返回元组。                                
	 */
	SPI_result = 0;

	/* 它是否返回元组? */
	if (plansource->resultDesc)
		return true;

	return false;
}

/*
 * SPI_plan_is_valid --- 测试 SPI 计划当前是否有效       
 * （即未被标记为需要重新验证）。          
 *
 * 使用前请参阅 CachedPlanIsValid 的说明。 
 */
bool
SPI_plan_is_valid(SPIPlanPtr plan)
{
	ListCell   *lc;

	Assert(plan->magic == _SPI_PLAN_MAGIC);

	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		if (!CachedPlanIsValid(plansource))
			return false;
	}
	return true;
}

/*
 * SPI_result_code_string --- 将 SPI 返回码转换为字符串    
 *
 * 这在错误消息中常常很有用。大多数调用者可能    
 * 只传负数（错误情形）码，但为通用起见我们也识别 
 * 成功码。          
 */
const char *
SPI_result_code_string(int code)
{
	static char buf[64];

	switch (code)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
			return "SPI_ERROR_UNCONNECTED";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_TRANSACTION:
			return "SPI_ERROR_TRANSACTION";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		case SPI_ERROR_REL_DUPLICATE:
			return "SPI_ERROR_REL_DUPLICATE";
		case SPI_ERROR_REL_NOT_FOUND:
			return "SPI_ERROR_REL_NOT_FOUND";
		case SPI_OK_CONNECT:
			return "SPI_OK_CONNECT";
		case SPI_OK_FINISH:
			return "SPI_OK_FINISH";
		case SPI_OK_FETCH:
			return "SPI_OK_FETCH";
		case SPI_OK_UTILITY:
			return "SPI_OK_UTILITY";
		case SPI_OK_SELECT:
			return "SPI_OK_SELECT";
		case SPI_OK_SELINTO:
			return "SPI_OK_SELINTO";
		case SPI_OK_INSERT:
			return "SPI_OK_INSERT";
		case SPI_OK_DELETE:
			return "SPI_OK_DELETE";
		case SPI_OK_UPDATE:
			return "SPI_OK_UPDATE";
		case SPI_OK_CURSOR:
			return "SPI_OK_CURSOR";
		case SPI_OK_INSERT_RETURNING:
			return "SPI_OK_INSERT_RETURNING";
		case SPI_OK_DELETE_RETURNING:
			return "SPI_OK_DELETE_RETURNING";
		case SPI_OK_UPDATE_RETURNING:
			return "SPI_OK_UPDATE_RETURNING";
		case SPI_OK_REWRITTEN:
			return "SPI_OK_REWRITTEN";
		case SPI_OK_REL_REGISTER:
			return "SPI_OK_REL_REGISTER";
		case SPI_OK_REL_UNREGISTER:
			return "SPI_OK_REL_UNREGISTER";
		case SPI_OK_TD_REGISTER:
			return "SPI_OK_TD_REGISTER";
		case SPI_OK_MERGE:
			return "SPI_OK_MERGE";
	}
	/* 未识别的码……返回有用的内容……  */
	sprintf(buf, "Unrecognized SPI code %d", code);
	return buf;
}

/*
 * SPI_plan_get_plan_sources --- 获取 SPI 计划底层的         
 * CachedPlanSources.
 *
 * CAUTION: 对于 CachedPlanSource 是否为最新，没有任何检查。   
 *
 * 导出此函数是为了供 PL/pgSQL 使用（这好过让 PL/pgSQL      
 * 自行直接窥探 SPIPlan）。它未在                          
 * spi.sgml 中文档化，因为我们并不希望太多处使用它。  
 */
List *
SPI_plan_get_plan_sources(SPIPlanPtr plan)
{
	Assert(plan->magic == _SPI_PLAN_MAGIC);
	return plan->plancache_list;
}

/*
 * SPI_plan_get_cached_plan --- 获取 SPI 计划的通用          
 * CachedPlan，若计划并非只含一个 CachedPlanSource，   
 * 返回 NULL 
 *
 * 计划引用计数会递增（若为已保存计划，还会登记到  
 * CurrentResourceOwner）。调用者负责执行 ReleaseCachedPlan.        
 *
 * 导出此函数是为了供 PL/pgSQL 使用（这好过让 PL/pgSQL      
 * 自行直接窥探 SPIPlan）。它未在                          
 * spi.sgml 中文档化，因为我们并不希望太多处使用它。  
 */
CachedPlan *
SPI_plan_get_cached_plan(SPIPlanPtr plan)
{
	CachedPlanSource *plansource;
	CachedPlan *cplan;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;

	Assert(plan->magic == _SPI_PLAN_MAGIC);

	/* 此处不支持一次性计划    */
	if (plan->oneshot)
		return NULL;

	/* 必须恰有一个 CachedPlanSource    */
	if (list_length(plan->plancache_list) != 1)
		return NULL;
	plansource = (CachedPlanSource *) linitial(plan->plancache_list);

	/* 为 ereport() 设置错误回溯支持      */
	spicallbackarg.query = plansource->query_string;
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/* 获取该查询的通用计划     */
	cplan = GetCachedPlan(plansource, NULL,
						  plan->saved ? CurrentResourceOwner : NULL,
						  _SPI_current->queryEnv);
	Assert(cplan == plansource->gplan);

	/* 弹出错误上下文栈    */
	error_context_stack = spierrcontext.previous;

	return cplan;
}


/* =================== private functions =================== */

/*
 * spi_dest_startup
 *		初始化以接收执行器元组到当前 SPI 过程的     
 *		SPITupleTable 中       
 */
void
spi_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;
	MemoryContext tuptabcxt;

	if (_SPI_current == NULL)
		elog(ERROR, "spi_dest_startup called while not connected to SPI");

	if (_SPI_current->tuptable != NULL)
		elog(ERROR, "improper call to spi_dest_startup");

	/* 把元组表上下文创建为 procCxt 的子上下文  */

	oldcxt = _SPI_procmem();	/* 切换到过程内存上下文     */

	tuptabcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "SPI TupTable",
									  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(tuptabcxt);

	_SPI_current->tuptable = tuptable = (SPITupleTable *)
		palloc0(sizeof(SPITupleTable));
	tuptable->tuptabcxt = tuptabcxt;
	tuptable->subid = GetCurrentSubTransactionId();

	/*
	 * 此时 tuptable 已足够有效，能被 AtEOSubXact_SPI 释放，     
	 * 把它挂到 SPI 上下文的 tuptables 列表上。这样能确保 
	 * 即使接下来几行代码万一失败也不会泄漏它。   
	 */
	slist_push_head(&_SPI_current->tuptables, &tuptable->next);

	/* 建立初始分配         */
	tuptable->alloced = 128;
	tuptable->vals = (HeapTuple *) palloc(tuptable->alloced * sizeof(HeapTuple));
	tuptable->numvals = 0;
	tuptable->tupdesc = CreateTupleDescCopy(typeinfo);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * spi_printtup
 *		把执行器取回的元组存入当前 SPI 过程  
 *		SPITupleTable 中       
 */
bool
spi_printtup(TupleTableSlot *slot, DestReceiver *self)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;

	if (_SPI_current == NULL)
		elog(ERROR, "spi_printtup called while not connected to SPI");

	tuptable = _SPI_current->tuptable;
	if (tuptable == NULL)
		elog(ERROR, "improper call to spi_printtup");

	oldcxt = MemoryContextSwitchTo(tuptable->tuptabcxt);

	if (tuptable->numvals >= tuptable->alloced)
	{
		/* 将指针数组的大小翻倍       */
		uint64		newalloced = tuptable->alloced * 2;

		tuptable->vals = (HeapTuple *) repalloc_huge(tuptable->vals,
													 newalloced * sizeof(HeapTuple));
		tuptable->alloced = newalloced;
	}

	tuptable->vals[tuptable->numvals] = ExecCopySlotHeapTuple(slot);
	(tuptable->numvals)++;

	MemoryContextSwitchTo(oldcxt);

	return true;
}

/*
 * 静态函数    
 */

/*
 * 解析并分析查询字符串. 
 *
 * 入口处 plan->argtypes、plan->nargs（或备选的 plan->parserSetup     
 * 与 plan->parserSetupArg）必须有效，plan->parse_mode 与       
 * plan->cursor_options.
 *
 * 结果存入 *plan（具体说是 plan->plancache_list）。       
 * 注意结果数据全部位于 CurrentMemoryContext 或其子上下文； 
 * 实际即数据在 SPI 执行器上下文，我们创建的           
 * 是一个"temporary" SPIPlan。解析期间产生的垃圾也会留在
 * CurrentMemoryContext 中。                  
 */
static void
_SPI_prepare_plan(const char *src, SPIPlanPtr plan)
{
	List	   *raw_parsetree_list;
	List	   *plancache_list;
	ListCell   *list_item;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;

	/*
	 * 为 ereport() 设置错误回溯支持     
	 */
	spicallbackarg.query = src;
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * 把请求字符串解析为原始解析树的列表。  
	 */
	raw_parsetree_list = raw_parser(src, plan->parse_mode);

	/*
	 * 对每个原始解析树做解析分析与规则重写，结果存入 
	 * 未保存的 plancache 条目。       
	 */
	plancache_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, list_item);
		List	   *stmt_list;
		CachedPlanSource *plansource;

		/*
		 * 在做解析分析之前先创建 CachedPlanSource，因为它   
		 * 需要看到未修改的原始解析树。 
		 */
		plansource = CreateCachedPlan(parsetree,
									  src,
									  CreateCommandTag(parsetree->stmt));

		/*
		 * 若提供 parserSetup 钩子，由它驱动参数数据类型  
		 * 否则我们使用固定的参数列表。
		 */
		if (plan->parserSetup != NULL)
		{
			Assert(plan->nargs == 0);
			stmt_list = pg_analyze_and_rewrite_withcb(parsetree,
													  src,
													  plan->parserSetup,
													  plan->parserSetupArg,
													  _SPI_current->queryEnv);
		}
		else
		{
			stmt_list = pg_analyze_and_rewrite_fixedparams(parsetree,
														   src,
														   plan->argtypes,
														   plan->nargs,
														   _SPI_current->queryEnv);
		}

		/* 完成 CachedPlanSource 的填写      */
		CompleteCachedPlan(plansource,
						   stmt_list,
						   NULL,
						   plan->argtypes,
						   plan->nargs,
						   plan->parserSetup,
						   plan->parserSetupArg,
						   plan->cursor_options,
						   false);	/* 非固定结果  */

		plancache_list = lappend(plancache_list, plansource);
	}

	plan->plancache_list = plancache_list;
	plan->oneshot = false;

	/*
	 * 弹出错误上下文栈   
	 */
	error_context_stack = spierrcontext.previous;
}

/*
 * 解析但不分析查询字符串。    
 *
 * 这是 _SPI_prepare_plan 的精简版本，只做最初的             
 * 原始解析。它创建"one shot" CachedPlanSources，      
 * 在执行可行之前仍需做解析分析。                  
 *
 * 使用"one shot"形式 CachedPlanSource 的好处是                  
 * 消除了数据复制与失效处理开销。推迟解析分析还能 
 * 避免某些原始解析树是 DDL 命令、影响后续解析树  
 * 有效性时产生的问题。这两个特性对                 
 * SPI_execute() 及类似场景都是好事。                   
 *
 * 结果存入 *plan（具体说是 plan->plancache_list）。       
 * 注意结果数据全部位于 CurrentMemoryContext 或其子上下文； 
 * 实际即数据在 SPI 执行器上下文，我们创建的           
 * 是一个"temporary" SPIPlan。解析期间产生的垃圾也会留在
 * CurrentMemoryContext 中。                  
 */
static void
_SPI_prepare_oneshot_plan(const char *src, SPIPlanPtr plan)
{
	List	   *raw_parsetree_list;
	List	   *plancache_list;
	ListCell   *list_item;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;

	/*
	 * 为 ereport() 设置错误回溯支持     
	 */
	spicallbackarg.query = src;
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * 把请求字符串解析为原始解析树的列表。  
	 */
	raw_parsetree_list = raw_parser(src, plan->parse_mode);

	/*
	 * 构建 plancache 条目，但先不做解析分析。        
	 */
	plancache_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, list_item);
		CachedPlanSource *plansource;

		plansource = CreateOneShotCachedPlan(parsetree,
											 src,
											 CreateCommandTag(parsetree->stmt));

		plancache_list = lappend(plancache_list, plansource);
	}

	plan->plancache_list = plancache_list;
	plan->oneshot = true;

	/*
	 * 弹出错误上下文栈   
	 */
	error_context_stack = spierrcontext.previous;
}

/*
 * _SPI_execute_plan: 以给定选项执行给定计划            
 *
 * options 包含可从 SPI 外部访问的选项：    
 * params: 传给查询的参数值         
 * read_only: true 表示只读执行（无 CommandCounterIncrement） 
 * allow_nonatomic: true 表示允许非原子 CALL/DO 执行
 * must_return_tuples: 查询未返回元组时抛错            
 * tcount: 执行元组数上限，0 表示不限    
 * dest: 接收输出的 DestReceiver，NULL 表示常规 SPI 输出  
 * owner: 用于持有计划引用计数的 ResourceOwner；       
 *		NULL 则用 CurrentResourceOwner（未保存计划则忽略）    
 *
 * 其他仅限内部访问的选项：           
 * snapshot: 使用的查询快照，InvalidSnapshot 表示          
 *		对每条查询取新快照的常规行为。    
 * crosscheck_snapshot: 供 RI 用，其他均传 InvalidSnapshot  
 * fire_triggers: true 表示查询结束时触发 AFTER 触发器，       
 *		常规情形；false 表示 AFTER 触发器推迟到外层末尾  
 */
static int
_SPI_execute_plan(SPIPlanPtr plan, const SPIExecuteOptions *options,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool fire_triggers)
{
	int			my_res = 0;
	uint64		my_processed = 0;
	SPITupleTable *my_tuptable = NULL;
	int			res = 0;
	bool		allow_nonatomic;
	bool		pushed_active_snap = false;
	ResourceOwner plan_owner = options->owner;
	SPICallbackArg spicallbackarg;
	ErrorContextCallback spierrcontext;
	CachedPlan *cplan = NULL;
	ListCell   *lc1;

	/*
	 * 仅当 options->allow_nonatomic 已设置，且连接时给定了  
	 * *SPI_OPT_NONATOMIC 标志，且我们不处于子事务，则允许  
	 * 非原子行为，才允许。后两项测试与 _SPI_commit()  
	 * 是否允许提交相一致；更多说明见该处。            
	 */
	allow_nonatomic = options->allow_nonatomic &&
		!_SPI_current->atomic && !IsSubTransaction();

	/*
	 * 为 ereport() 设置错误回溯支持     
	 */
	spicallbackarg.query = NULL;	/* 稍后再填充       */
	spicallbackarg.mode = plan->parse_mode;
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = &spicallbackarg;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * 我们支持四种不同的快照管理行为：       
	 *
	 * snapshot != InvalidSnapshot, read_only = true：严格按给定的  
	 * 快照。
	 *
	 * snapshot != InvalidSnapshot, read_only = false：采用                
	 * 的快照，但在每个 querytree 前推进命令 ID。   
	 *
	 * snapshot == InvalidSnapshot, read_only = true：对不需要快照   
	 * 的查询不做处理。对需要的查询，确保存在 Portal  
	 * 快照；或者当进入时的 ActiveSnapshot 存在且不同时使用 
	 * 后者。                    
	 *
	 * snapshot == InvalidSnapshot, read_only = false：对不需要快照   
	 * 的查询不做处理。对需要的查询，确保存在 Portal  
	 * 快照；并在原子执行（!allow_nonatomic）时为每个       
	 * 用户命令取一个完整的新快照，并在命令内每个    
	 * querytree 前推进其命令 ID。在 allow_nonatomic 模式下      
	 * 我们原样使用 Portal 快照。     
	 *
	 * 在前两种情形下，只需为整个计划列表把该快照压栈
	 * 一次即可。         
	 *
	 * 注意 snapshot != InvalidSnapshot 意味着处于原子执行   
	 * 语境  
	 */
	if (snapshot != InvalidSnapshot)
	{
		/* 这里刻意测试的是 options 字段而非派生出的值     */
		Assert(!options->allow_nonatomic);
		if (options->read_only)
		{
			PushActiveSnapshot(snapshot);
			pushed_active_snap = true;
		}
		else
		{
			/* 确保拥有一份快照私有副本以便修改           */
			PushCopiedSnapshot(snapshot);
			pushed_active_snap = true;
		}
	}

	/*
	 * 若保存计划须持有资源所有者，否则不必持           
	 * 有。
	 */
	if (!plan->saved)
		plan_owner = NULL;
	else if (plan_owner == NULL)
		plan_owner = CurrentResourceOwner;

	/*
	 * 我们将 must_return_tuples 理解为"必须至少有一个查询、
	 * 且全部查询都必须返回元组"。这比                
	 * SPI_is_cursor_plan 的检查略宽松，但似乎没有理由强制  
	 * 只能有一个查询。
	 */
	if (options->must_return_tuples && plan->plancache_list == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("empty query does not return tuples")));

	foreach(lc1, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc1);
		List	   *stmt_list;
		ListCell   *lc2;

		spicallbackarg.query = plansource->query_string;

		/*
		 * 如果这是一次性计划，我们仍需执行解析分析。
		 */
		if (plan->oneshot)
		{
			RawStmt    *parsetree = plansource->raw_parse_tree;
			const char *src = plansource->query_string;
			List	   *querytree_list;

			/*
			 * 参数数据类型由 parserSetup 钩子驱动，              
			 * 若未提供，则用固定参数列表。
			 */
			if (parsetree == NULL)
				querytree_list = NIL;
			else if (plan->parserSetup != NULL)
			{
				Assert(plan->nargs == 0);
				querytree_list = pg_analyze_and_rewrite_withcb(parsetree,
															   src,
															   plan->parserSetup,
															   plan->parserSetupArg,
															   _SPI_current->queryEnv);
			}
			else
			{
				querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree,
																	src,
																	plan->argtypes,
																	plan->nargs,
																	_SPI_current->queryEnv);
			}

			/* 完成 CachedPlanSource 的填充      */
			CompleteCachedPlan(plansource,
							   querytree_list,
							   NULL,
							   plan->argtypes,
							   plan->nargs,
							   plan->parserSetup,
							   plan->parserSetupArg,
							   plan->cursor_options,
							   false);	/* 非固定结果  */
		}

		/*
		 * 若被要求，则当查询未返回元组时抱怨。  
		 * （重新计划改变不了它，可在此之前检查。     
		 * 不过要到解析分析之后才能检查，所以对一次性
		 * 计划而言，此处已是可能检查的最早时机。） 
		 */
		if (options->must_return_tuples && !plansource->resultDesc)
		{
			/* 尽量给出有用的错误消息*/
			const char *cmdtag;

			/* 不带 resultDesc 的 SELECT 必是 SELECT INTO */
			if (plansource->commandTag == CMDTAG_SELECT)
				cmdtag = "SELECT INTO";
			else
				cmdtag = GetCommandTagName(plansource->commandTag);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/* translator: %s is name of a SQL command, eg INSERT */
					 errmsg("%s query does not return tuples", cmdtag)));
		}

		/*
		 * 需要时重新计划并递增计划引用计数。若计划已
		 * 保存，则引用计数须由 plan_owner 支撑。 
		 */
		cplan = GetCachedPlan(plansource, options->params,
							  plan_owner, _SPI_current->queryEnv);

		stmt_list = cplan->stmt_list;

		/*
		 * 若未指定要使用的快照，且语句列表需要快照，  
		 * 则做好设置。                    
		 */
		if (snapshot == InvalidSnapshot &&
			(list_length(stmt_list) > 1 ||
			 (list_length(stmt_list) == 1 &&
			  PlannedStmtRequiresSnapshot(linitial_node(PlannedStmt,
														stmt_list)))))
		{
			/*
			 * 首先，确保存在 Portal 级快照。如果前一个操作 
			 * 是过程或 DO 块内的 COMMIT 或 ROLLBACK，就用它     
			 * 回填快照栈。（我们无法更早恢复 Portal 快照， 
			 * 否则会破坏诸如 COMMIT 后紧接着执行 SET 或       
			 * LOCK 的情形。）对每条语句列表只需检查一次，
			 * 因为 COMMIT/ROLLBACK/CALL/DO 不会出现在            
			 * 多条语句列表中。      
			 */
			EnsurePortalSnapshotExists();

			/*
			 * 在默认非只读情形下，获取新的每语句列表      
			 * 快照，替换先前压入的那份。非原子执行时  
			 * 则跳过该步骤（那种情况下我们完全依赖  
			 * Portal 快照）。                           
			 */
			if (!options->read_only && !allow_nonatomic)
			{
				if (pushed_active_snap)
					PopActiveSnapshot();
				PushActiveSnapshot(GetTransactionSnapshot());
				pushed_active_snap = true;
			}
		}

		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);
			bool		canSetTag = stmt->canSetTag;
			DestReceiver *dest;

			/*
			 * 重置输出状态。（注意若使用非 SPI 的接收器，
			 * _SPI_current->processed 将保持为 0，这也正是我们  
			 * 将报告给调用者的值。统计元组是接收器的     
			 * 职责。）  
			 */
			_SPI_current->processed = 0;
			_SPI_current->tuptable = NULL;

			/* 检查不支持的情形。  */
			if (stmt->utilityStmt)
			{
				if (IsA(stmt->utilityStmt, CopyStmt))
				{
					CopyStmt   *cstmt = (CopyStmt *) stmt->utilityStmt;

					if (cstmt->filename == NULL)
					{
						my_res = SPI_ERROR_COPY;
						goto fail;
					}
				}
				else if (IsA(stmt->utilityStmt, TransactionStmt))
				{
					my_res = SPI_ERROR_TRANSACTION;
					goto fail;
				}
			}

			if (options->read_only && !CommandIsReadOnly(stmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
						 errmsg("%s is not allowed in a non-volatile function",
								CreateCommandName((Node *) stmt))));

			/*
			 * 若非只读模式，在每条命令前推进命令计数器  
			 * 并更新快照。（但若快照不受我们控制则跳过  
			 * 该步骤。）          
			 */
			if (!options->read_only && pushed_active_snap)
			{
				CommandCounterIncrement();
				UpdateActiveSnapshotCommandId();
			}

			/*
			 * 选择合适的元组接收器。非 canSetTag 子查询的  
			 * 输出总是被丢弃。                 
			 */
			if (!canSetTag)
				dest = CreateDestReceiver(DestNone);
			else if (options->dest)
				dest = options->dest;
			else
				dest = CreateDestReceiver(DestSPI);

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;
				Snapshot	snap;

				if (ActiveSnapshotSet())
					snap = GetActiveSnapshot();
				else
					snap = InvalidSnapshot;

				qdesc = CreateQueryDesc(stmt,
										plansource->query_string,
										snap, crosscheck_snapshot,
										dest,
										options->params,
										_SPI_current->queryEnv,
										0);
				res = _SPI_pquery(qdesc, fire_triggers,
								  canSetTag ? options->tcount : 0);
				FreeQueryDesc(qdesc);
			}
			else
			{
				ProcessUtilityContext context;
				QueryCompletion qc;

				/*
				 * 若不允许非原子操作，则告知         
				 * ProcessUtility 这是原子执行上下文。      
				 */
				if (allow_nonatomic)
					context = PROCESS_UTILITY_QUERY_NONATOMIC;
				else
					context = PROCESS_UTILITY_QUERY;

				InitializeQueryCompletion(&qc);
				ProcessUtility(stmt,
							   plansource->query_string,
							   true,	/* 保护 plancache 的节点树 */
							   context,
							   options->params,
							   _SPI_current->queryEnv,
							   dest,
							   &qc);

				/* 若 stmt 返回元组则更新 "processed" */
				if (_SPI_current->tuptable)
					_SPI_current->processed = _SPI_current->tuptable->numvals;

				res = SPI_OK_UTILITY;

				/*
				 * 有些实用工具语句会返回行数，尽管元组未  
				 * 返回给调用者。                 
				 */
				if (IsA(stmt->utilityStmt, CreateTableAsStmt))
				{
					CreateTableAsStmt *ctastmt = (CreateTableAsStmt *) stmt->utilityStmt;

					if (qc.commandTag == CMDTAG_SELECT)
						_SPI_current->processed = qc.nprocessed;
					else
					{
						/*
						 * 必定是没做事的 IF NOT EXISTS，或是   
						 * CREATE ... WITH NO DATA 
						 */
						Assert(ctastmt->if_not_exists ||
							   ctastmt->into->skipData);
						_SPI_current->processed = 0;
					}

					/*
					 * 由于历史原因，若 CREATE TABLE AS 被写成    
					 * SELECT INTO，则返回特殊的返回码。 
					 */
					if (ctastmt->is_select_into)
						res = SPI_OK_SELINTO;
				}
				else if (IsA(stmt->utilityStmt, CopyStmt))
				{
					Assert(qc.commandTag == CMDTAG_COPY);
					_SPI_current->processed = qc.nprocessed;
				}
			}

			/*
			 * 最后 canSetTag 查询设置返回给调用者的状态值。 
			 * 务必释放所有未返回的 tuptable，以防事务内 
			 * 内存泄漏。                     
			 */
			if (canSetTag)
			{
				my_processed = _SPI_current->processed;
				SPI_freetuptable(my_tuptable);
				my_tuptable = _SPI_current->tuptable;
				my_res = res;
			}
			else
			{
				SPI_freetuptable(_SPI_current->tuptable);
				_SPI_current->tuptable = NULL;
			}

			/*
			 * 我们不会向接收器发出销毁调用。SPI 与 None  
			 * 接收器反正会忽略它；若为调用者所提供， 
			 * 那销毁它就不是我们的职责。             
			 */

			if (res < 0)
			{
				my_res = res;
				goto fail;
			}
		}

		/* 该计划已用完，释放引用计数  */
		ReleaseCachedPlan(cplan, plan_owner);
		cplan = NULL;

		/*
		 * 若非只读模式，则在最后一条命令之后推进命令  
		 * 计数器。这能确保其效果可见，以防它是会影响    
		 * 下一个 CachedPlanSource 的 DDL。           
		 */
		if (!options->read_only)
			CommandCounterIncrement();
	}

fail:

	/* 若压入过快照，就把它从栈中弹出   */
	if (pushed_active_snap)
		PopActiveSnapshot();

	/* 不再需要缓存计划的引用计数（若有）*/
	if (cplan)
		ReleaseCachedPlan(cplan, plan_owner);

	/*
	 * 弹出错误上下文栈   
	 */
	error_context_stack = spierrcontext.previous;

	/* 为调用者保存结果*/
	SPI_processed = my_processed;
	SPI_tuptable = my_tuptable;

	/* tuptable 现在归调用者管，不归 SPI 管    */
	_SPI_current->tuptable = NULL;

	/*
	 * 若没有任何查询带 canSetTag，则返回 SPI_OK_REWRITTEN。     
	 * 在 8.4 之前我们曾返回最后一个查询的结果码，却不给
	 * 辅助结果，令人费解。
	 */
	if (my_res == 0)
		my_res = SPI_OK_REWRITTEN;

	return my_res;
}

/*
 * 把查询参数数组转换成规划器和执行器想要的形式       
 */
static ParamListInfo
_SPI_convert_params(int nargs, Oid *argtypes,
					Datum *Values, const char *Nulls)
{
	ParamListInfo paramLI;

	if (nargs > 0)
	{
		paramLI = makeParamList(nargs);

		for (int i = 0; i < nargs; i++)
		{
			ParamExternData *prm = &paramLI->params[i];

			prm->value = Values[i];
			prm->isnull = (Nulls && Nulls[i] == 'n');
			prm->pflags = PARAM_FLAG_CONST;
			prm->ptype = argtypes[i];
		}
	}
	else
		paramLI = NULL;
	return paramLI;
}

static int
_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, uint64 tcount)
{
	int			operation = queryDesc->operation;
	int			eflags;
	int			res;

	switch (operation)
	{
		case CMD_SELECT:
			if (queryDesc->dest->mydest == DestNone)
			{
				/* 若要丢弃结果，则不要返回 SPI_OK_SELECT    */
				res = SPI_OK_UTILITY;
			}
			else
				res = SPI_OK_SELECT;
			break;
		case CMD_INSERT:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_INSERT_RETURNING;
			else
				res = SPI_OK_INSERT;
			break;
		case CMD_DELETE:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_DELETE_RETURNING;
			else
				res = SPI_OK_DELETE;
			break;
		case CMD_UPDATE:
			if (queryDesc->plannedstmt->hasReturning)
				res = SPI_OK_UPDATE_RETURNING;
			else
				res = SPI_OK_UPDATE;
			break;
		case CMD_MERGE:
			res = SPI_OK_MERGE;
			break;
		default:
			return SPI_ERROR_OPUNKNOWN;
	}

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ResetUsage();
#endif

	/* 选择执行选项       */
	if (fire_triggers)
		eflags = 0;				/* 默认的运行至完成标志  */
	else
		eflags = EXEC_FLAG_SKIP_TRIGGERS;

	ExecutorStart(queryDesc, eflags);

	ExecutorRun(queryDesc, ForwardScanDirection, tcount, true);

	_SPI_current->processed = queryDesc->estate->es_processed;

	if ((res == SPI_OK_SELECT || queryDesc->plannedstmt->hasReturning) &&
		queryDesc->dest->mydest == DestSPI)
	{
		if (_SPI_checktuples())
			elog(ERROR, "consistency check on SPI tuple count failed");
	}

	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);
	/* FreeQueryDesc 由调用者完成    */

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ShowUsage("SPI EXECUTOR STATS");
#endif

	return res;
}

/*
 * _SPI_error_callback
 *
 * 当经 SPI 调用的查询失败时添加上下文信息  
 */
static void
_SPI_error_callback(void *arg)
{
	SPICallbackArg *carg = (SPICallbackArg *) arg;
	const char *query = carg->query;
	int			syntaxerrposition;

	if (query == NULL)			/* 以防 arg 尚未设置    */
		return;

	/*
	 * 若存在语法错误位置，则转换为内部语法错误；       
	 * 否则把该查询当作上下文栈的一项。     
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(query);
	}
	else
	{
		/* 用解析模式来决定如何描述该查询          */
		switch (carg->mode)
		{
			case RAW_PARSE_PLPGSQL_EXPR:
				errcontext("SQL expression \"%s\"", query);
				break;
			case RAW_PARSE_PLPGSQL_ASSIGN1:
			case RAW_PARSE_PLPGSQL_ASSIGN2:
			case RAW_PARSE_PLPGSQL_ASSIGN3:
				errcontext("PL/pgSQL assignment \"%s\"", query);
				break;
			default:
				errcontext("SQL statement \"%s\"", query);
				break;
		}
	}
}

/*
 * _SPI_cursor_operation()
 *
 *	对游标执行 FETCH 或 MOVE
 */
static void
_SPI_cursor_operation(Portal portal, FetchDirection direction, long count,
					  DestReceiver *dest)
{
	uint64		nfetched;

	/* 检查 portal 是否有效     */
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	/* 压入 SPI 栈     */
	if (_SPI_begin_call(true) < 0)
		elog(ERROR, "SPI cursor operation called while not connected");

	/* 重置 SPI 结果（注意我们刻意不碰 lastoid）         */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* 运行游标   */
	nfetched = PortalRunFetch(portal,
							  direction,
							  count,
							  dest);

	/*
	 * 切勿把这次保存与前面的函数调用合并。若           
	 * portal 内含有使用 SPI 的函数调用，则 portal 运行期间   
	 * _SPI_stack 很可能会移动。当控制返回时，              
	 * _SPI_current 将指向正确的栈条目……但该指针           
	 * 可能已与之前不同。因此必须确保在函数调用           
	 * 完成后重新获取该指针。             
	 */
	_SPI_current->processed = nfetched;

	if (dest->mydest == DestSPI && _SPI_checktuples())
		elog(ERROR, "consistency check on SPI tuple count failed");

	/* 把结果放置到位，供调用者访问     */
	SPI_processed = _SPI_current->processed;
	SPI_tuptable = _SPI_current->tuptable;

	/* tuptable 现在归调用者管，不归 SPI 管    */
	_SPI_current->tuptable = NULL;

	/* 弹出 SPI 栈    */
	_SPI_end_call(true);
}


static MemoryContext
_SPI_execmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->execCxt);
}

static MemoryContext
_SPI_procmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->procCxt);
}

/*
 * _SPI_begin_call：在已连接的过程内开始一次 SPI 操作  
 *
 * use_exec 为 true 表示我们打算在本次 SPI 操作中         
 * 利用过程的 execCxt。我们会切换进该上下文，并安排   
 * 在 _SPI_end_call 或出错时清理它。                     
 */
static int
_SPI_begin_call(bool use_exec)
{
	if (_SPI_current == NULL)
		return SPI_ERROR_UNCONNECTED;

	if (use_exec)
	{
		/* 记住 Executor 操作的开始时间        */
		_SPI_current->execSubid = GetCurrentSubTransactionId();
		/* 切换到 Executor 内存上下文    */
		_SPI_execmem();
	}

	return 0;
}

/*
 * _SPI_end_call：在已连接的过程内结束一次 SPI 操作
 *
 * use_exec 必须与之前 _SPI_begin_call 中传入的相同 
 *
 * 注意：该函数目前没有失败返回情形，调用者不检查   
 */
static int
_SPI_end_call(bool use_exec)
{
	if (use_exec)
	{
		/* 切换到过程内存上下文         */
		_SPI_procmem();
		/* 标记 Executor 上下文不再使用  */
		_SPI_current->execSubid = InvalidSubTransactionId;
		/* 并释放 Executor 内存*/
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
	}

	return 0;
}

static bool
_SPI_checktuples(void)
{
	uint64		processed = _SPI_current->processed;
	SPITupleTable *tuptable = _SPI_current->tuptable;
	bool		failed = false;

	if (tuptable == NULL)		/* 未调用 spi_dest_startup      */
		failed = true;
	else if (processed != tuptable->numvals)
		failed = true;

	return failed;
}

/*
 * 把"临时" SPIPlan 转换成"未保存"计划。    
 *
 * 传入的 _SPI_plan 结构体位于栈上，其全部附属数据都    
 * 处于当前 SPI 执行器上下文（或其下）。把计划复制到 
 * SPI 过程上下文，使其能在 _SPI_end_call() 之后存活。为 
 * 减少数据复制，此处以破坏性方式修改输入计划：把  
 * plancache 条目从它那里取走并重新挂到新 SPIPlan 之下。 
 */
static SPIPlanPtr
_SPI_make_plan_non_temp(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;
	MemoryContext parentcxt = _SPI_current->procCxt;
	MemoryContext plancxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	/* 断言输入是临时 SPIPlan           */
	Assert(plan->magic == _SPI_PLAN_MAGIC);
	Assert(plan->plancxt == NULL);
	/* 一次性计划无法被保存*/
	Assert(!plan->oneshot);

	/*
	 * 为该计划创建内存上下文，置于过程上下文之下。     
	 * 我们预计该计划不会很大。      
	 */
	plancxt = AllocSetContextCreate(parentcxt,
									"SPI Plan",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* 把 _SPI_plan 结构体及附属数据复制进新上下文        */
	newplan = (SPIPlanPtr) palloc0(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->plancxt = plancxt;
	newplan->parse_mode = plan->parse_mode;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;
	newplan->parserSetup = plan->parserSetup;
	newplan->parserSetupArg = plan->parserSetupArg;

	/*
	 * 把所有 CachedPlanSource 重新挂到过程上下文之下。   
	 * 理论上这可能因 palloc 而中途失败，但我们对此         
	 * 并不太在意，因为出错时过程上下文与执行器上下文  
	 * 都会消失。        
	 */
	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		CachedPlanSetParentContext(plansource, parentcxt);

		/* 构建新列表，其单元位于 plancxt  */
		newplan->plancache_list = lappend(newplan->plancache_list, plansource);
	}

	MemoryContextSwitchTo(oldcxt);

	/* 保险起见，把 CachedPlanSource 与临时计划解除链接  */
	plan->plancache_list = NIL;

	return newplan;
}

/*
 * 为给定计划制作"已保存"副本
 */
static SPIPlanPtr
_SPI_save_plan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;
	MemoryContext plancxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	/* 一次性计划无法被保存*/
	Assert(!plan->oneshot);

	/*
	 * 为该计划创建一个内存上下文。我们预计计划不会   
	 * 很大，故用小于默认值的分配参数。在完成全部  
	 * 复制之前它是一个临时上下文。           
	 */
	plancxt = AllocSetContextCreate(CurrentMemoryContext,
									"SPI Plan",
									ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* 把 SPI 计划复制进专属上下文 */
	newplan = (SPIPlanPtr) palloc0(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->plancxt = plancxt;
	newplan->parse_mode = plan->parse_mode;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;
	newplan->parserSetup = plan->parserSetup;
	newplan->parserSetupArg = plan->parserSetupArg;

	/* 复制所有 plancache 条目  */
	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);
		CachedPlanSource *newsource;

		newsource = CopyCachedPlan(plansource);
		newplan->plancache_list = lappend(newplan->plancache_list, newsource);
	}

	MemoryContextSwitchTo(oldcxt);

	/*
	 * 将其标记为已保存，重新挂到 CacheMemoryContext 之下， 
	 * 把各成员 CachedPlanSource 都标记为已保存。这一    
	 * 序列无法中途失败，因此无长期内存泄漏风险。 
	 */
	newplan->saved = true;
	MemoryContextSetParent(newplan->plancxt, CacheMemoryContext);

	foreach(lc, newplan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		SaveCachedPlan(plansource);
	}

	return newplan;
}

/*
 * 按名称内部查找临时命名关系。          
 */
static EphemeralNamedRelation
_SPI_find_ENR_by_name(const char *name)
{
	/* 内部静态函数；任何错误都是 SPI 自身的 bug*/
	Assert(name != NULL);

	/* 若没添加过 tuplestore，则快速退出*/
	if (_SPI_current->queryEnv == NULL)
		return NULL;

	return get_ENR(_SPI_current->queryEnv, name);
}

/*
 * 注册一个临时命名关系，供本 SPI 连接上的后续调用中    
 * 的规划器和执行器使用。          
 */
int
SPI_register_relation(EphemeralNamedRelation enr)
{
	EphemeralNamedRelation match;
	int			res;

	if (enr == NULL || enr->md.name == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(false);	/* 保持当前内存上下文 */
	if (res < 0)
		return res;

	match = _SPI_find_ENR_by_name(enr->md.name);
	if (match)
		res = SPI_ERROR_REL_DUPLICATE;
	else
	{
		if (_SPI_current->queryEnv == NULL)
			_SPI_current->queryEnv = create_queryEnv();

		register_ENR(_SPI_current->queryEnv, enr);
		res = SPI_OK_REL_REGISTER;
	}

	_SPI_end_call(false);

	return res;
}

/*
 * 按名称注销一个临时命名关系。这大概会是一个很少   
 * 被用到的函数，因为 SPI_finish 会自动清除它。       
 */
int
SPI_unregister_relation(const char *name)
{
	EphemeralNamedRelation match;
	int			res;

	if (name == NULL)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(false);	/* 保持当前内存上下文 */
	if (res < 0)
		return res;

	match = _SPI_find_ENR_by_name(name);
	if (match)
	{
		unregister_ENR(_SPI_current->queryEnv, match->md.name);
		res = SPI_OK_REL_UNREGISTER;
	}
	else
		res = SPI_ERROR_REL_NOT_FOUND;

	_SPI_end_call(false);

	return res;
}

/*
 * 使用本 SPI 连接注册来自 'tdata' 的过渡关系。             
 * 各 PL 实现的触发器处理器应在连接之后调用它，   
 * 以使过渡表对本连接中运行的任何                            
 * 查询可见。    
 */
int
SPI_register_trigger_data(TriggerData *tdata)
{
	if (tdata == NULL)
		return SPI_ERROR_ARGUMENT;

	if (tdata->tg_newtable)
	{
		EphemeralNamedRelation enr =
			palloc(sizeof(EphemeralNamedRelationData));
		int			rc;

		enr->md.name = tdata->tg_trigger->tgnewtable;
		enr->md.reliddesc = tdata->tg_relation->rd_id;
		enr->md.tupdesc = NULL;
		enr->md.enrtype = ENR_NAMED_TUPLESTORE;
		enr->md.enrtuples = tuplestore_tuple_count(tdata->tg_newtable);
		enr->reldata = tdata->tg_newtable;
		rc = SPI_register_relation(enr);
		if (rc != SPI_OK_REL_REGISTER)
			return rc;
	}

	if (tdata->tg_oldtable)
	{
		EphemeralNamedRelation enr =
			palloc(sizeof(EphemeralNamedRelationData));
		int			rc;

		enr->md.name = tdata->tg_trigger->tgoldtable;
		enr->md.reliddesc = tdata->tg_relation->rd_id;
		enr->md.tupdesc = NULL;
		enr->md.enrtype = ENR_NAMED_TUPLESTORE;
		enr->md.enrtuples = tuplestore_tuple_count(tdata->tg_oldtable);
		enr->reldata = tdata->tg_oldtable;
		rc = SPI_register_relation(enr);
		if (rc != SPI_OK_REL_REGISTER)
			return rc;
	}

	return SPI_OK_TD_REGISTER;
}
