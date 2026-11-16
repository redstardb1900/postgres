/*-------------------------------------------------------------------------
 *
 * mcxtfuncs.c
 *	  显示后端内存上下文的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/mcxtfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"

/* ----------
 * 显示 MemoryContext 标识符的最大字节数。
 * ----------
 */
#define MEMORY_CONTEXT_IDENT_DISPLAY_SIZE	1024

/*
 * PutMemoryContextsStatsTupleStore
 *		pg_get_backend_memory_contexts 的一层递归。
 */
static void
PutMemoryContextsStatsTupleStore(Tuplestorestate *tupstore,
								 TupleDesc tupdesc, MemoryContext context,
								 const char *parent, int level)
{
#define PG_GET_BACKEND_MEMORY_CONTEXTS_COLS	9

	Datum		values[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	bool		nulls[PG_GET_BACKEND_MEMORY_CONTEXTS_COLS];
	MemoryContextCounters stat;
	MemoryContext child;
	const char *name;
	const char *ident;

	Assert(MemoryContextIsValid(context));

	name = context->name;
	ident = context->ident;

	/*
	 * 与日志输出保持一致，我们给 dynahash 上下文仅标以
	 * 哈希表名，与 MemoryContextStatsPrint() 一致。
	 */
	if (ident && strcmp(name, "dynahash") == 0)
	{
		name = ident;
		ident = NULL;
	}

	/* 检查上下文本身 */
	memset(&stat, 0, sizeof(stat));
	(*context->methods->stats) (context, NULL, (void *) &level, &stat, true);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	if (name)
		values[0] = CStringGetTextDatum(name);
	else
		nulls[0] = true;

	if (ident)
	{
		int			idlen = strlen(ident);
		char		clipped_ident[MEMORY_CONTEXT_IDENT_DISPLAY_SIZE];

		/*
		 * 诸如 SQL 查询串之类的标识符可能非常长，
		 * 截断超长标识符。
		 */
		if (idlen >= MEMORY_CONTEXT_IDENT_DISPLAY_SIZE)
			idlen = pg_mbcliplen(ident, idlen, MEMORY_CONTEXT_IDENT_DISPLAY_SIZE - 1);

		memcpy(clipped_ident, ident, idlen);
		clipped_ident[idlen] = '\0';
		values[1] = CStringGetTextDatum(clipped_ident);
	}
	else
		nulls[1] = true;

	if (parent)
		values[2] = CStringGetTextDatum(parent);
	else
		nulls[2] = true;

	values[3] = Int32GetDatum(level);
	values[4] = Int64GetDatum(stat.totalspace);
	values[5] = Int64GetDatum(stat.nblocks);
	values[6] = Int64GetDatum(stat.freespace);
	values[7] = Int64GetDatum(stat.freechunks);
	values[8] = Int64GetDatum(stat.totalspace - stat.freespace);
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);

	for (child = context->firstchild; child != NULL; child = child->nextchild)
	{
		PutMemoryContextsStatsTupleStore(tupstore, tupdesc,
										 child, name, level + 1);
	}
}

/*
 * pg_get_backend_memory_contexts
 *		显示后端内存上下文的 SRF。
 */
Datum
pg_get_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	PutMemoryContextsStatsTupleStore(rsinfo->setResult, rsinfo->setDesc,
									 TopMemoryContext, NULL, 0);

	return (Datum) 0;
}

/*
 * pg_log_backend_memory_contexts
 *		通知一个后端或辅助进程记录其内存上下文。
 *
 * 默认情况下，仅允许超级用户发出记录内存
 * 上下文的信号，因为允许任意用户无限制地发出
 * 这种请求会产生大量日志消息，进而可能导致
 * 拒绝服务。其他角色可经 GRANT 授权。
 *
 * 收到此信号后，后端或辅助进程会在信号处理
 * 器中设置标志位，这使得下一次 CHECK_FOR_INTERRUPTS()
 * 或进程专属中断处理程序记录内存上下文。
 */
Datum
pg_log_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	PGPROC	   *proc;
	BackendId	backendId = InvalidBackendId;

	proc = BackendPidGetProc(pid);

	/*
	 * 检查给定 pid 对应的进程是后端还是辅助进程。
	 *
	 * 如果给定进程是后端，则在稍后的
	 * SendProcSignal() 中使用其后端 id 来加速操作。否则
	 * 不要这样做，因为辅助进程（除 startup 进程外）
	 * 没有有效后端 id。
	 */
	if (proc != NULL)
		backendId = proc->backendId;
	else
		proc = AuxiliaryPidGetProc(pid);

	/*
	 * 若 pid 无效，BackendPidGetProc() 与
	 * AuxiliaryPidGetProc() 会返回 NULL；但等我们执行
	 * kill() 时，我们在此处拿到有效 proc 的进程可能
	 * 已经自行终止。无法对任意进程加锁来防止它。
	 * 但此机制通常用于调试仍在运行并消耗大量
	 * 内存的某后端或辅助进程，故它可能先自行
	 * 结束且其内存上下文未被记录，这并不是问题。
	 */
	if (proc == NULL)
	{
		/*
		 * 这只是一条警告，因此循环遍历结果集不会
		 * 因某后端在运行期间自行终止而中止。
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL server process", pid)));
		PG_RETURN_BOOL(false);
	}

	if (SendProcSignal(pid, PROCSIG_LOG_MEMORY_CONTEXT, backendId) < 0)
	{
		/* 同样仅为警告，允许循环 */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}
