/* -------------------------------------------------------------------------
 *
 * pgstat_function.c
 *	  函数统计的实现。
 *
 * 本文件包含函数统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_function.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/inval.h"
#include "utils/pgstat_internal.h"
#include "utils/syscache.h"


/* ----------
 * GUC 参数
 * ----------
 */
int			pgstat_track_functions = TRACK_FUNC_OFF;


/*
 * 到目前为止记入当前后端各函数的时间总计。
 * 用它来帮助区分 "self" 与 "other" 时间计费。
 * （假定其初始化为零。）
 */
static instr_time total_func_time;


/*
 * 确保事务回滚时删除统计。
 */
void
pgstat_create_function(Oid proid)
{
	pgstat_create_transactional(PGSTAT_KIND_FUNCTION,
								MyDatabaseId,
								proid);
}

/*
 * 确保事务提交时删除统计。
 *
 * NB: 仅因 pgstat_init_function_usage() 做了额外工作才可靠。
 * 如果其他地方产出函数统计，多半也需要类似
 * 逻辑。
 */
void
pgstat_drop_function(Oid proid)
{
	pgstat_drop_transactional(PGSTAT_KIND_FUNCTION,
							  MyDatabaseId,
							  proid);
}

/*
 * 初始化函数调用用量数据。
 * 在调用函数之前由执行器调用。
 */
void
pgstat_init_function_usage(FunctionCallInfo fcinfo,
						   PgStat_FunctionCallUsage *fcu)
{
	PgStat_EntryRef *entry_ref;
	PgStat_FunctionCounts *pending;
	bool		created_entry;

	if (pgstat_track_functions <= fcinfo->flinfo->fn_stats)
	{
		/* 不需要统计 */
		fcu->fs = NULL;
		return;
	}

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_FUNCTION,
										  MyDatabaseId,
										  fcinfo->flinfo->fn_oid,
										  &created_entry);

	/*
	 * 若无已存在的共享条目，检查函数是否已被
	 * 并发删除。可能直到此处才被发现，因为执行
	 * 仅调用函数的语句时，不会触发缓存失效处理。
	 * 我们在意这种情况，否则可能为已删除的函数
	 * 新建一条统计条目（对关系等不存在此问题，
	 * 因为产出统计需要事先已持有该关系上的
	 * 锁）。
	 *
	 * 依据 track_functions 是否启用而产生行为差异，
	 * 这确实有失美观，但鉴于在函数是否被缓存等
	 * 方面早已存在行为差异，这样做似乎
	 * 尚可接受。
	 *
	 * 就正确性而言只需将 ->dropped 设为 true 即可。然而
	 * 已接受的失效处理通常会导致 PL 代码中的 "low level"
	 * 失败，且错误消息中带有 OID，这使得测试变得更
	 * 难。
	 */
	if (created_entry)
	{
		AcceptInvalidationMessages();
		if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(fcinfo->flinfo->fn_oid)))
		{
			pgstat_drop_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId,
							  fcinfo->flinfo->fn_oid);
			ereport(ERROR, errcode(ERRCODE_UNDEFINED_FUNCTION),
					errmsg("function call to dropped function"));
		}
	}

	pending = entry_ref->pending;

	fcu->fs = pending;

	/* 保存此函数的统计，稍后用于补偿递归 */
	fcu->save_f_total_time = pending->total_time;

	/* 保存当前后端级总时间 */
	fcu->save_total = total_func_time;

	/* 获取函数起点时钟时间 */
	INSTR_TIME_SET_CURRENT(fcu->start);
}

/*
 * 计算函数调用用量并更新统计计数器。
 * 由执行器在函数调用完成后调用。
 *
 * 对于以 value-per-call 模式运行的集合返回函数，
 * 将看到多次 pgstat_init_function_usage/pgstat_end_function_usage
 * 调用，尽管用户认为这只是该函数的一次调用。
 * finalize 标志应在末次调用为 TRUE。
 */
void
pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu, bool finalize)
{
	PgStat_FunctionCounts *fs = fcu->fs;
	instr_time	total;
	instr_time	others;
	instr_time	self;

	/* 不需要统计 */
	if (fs == NULL)
		return;

	/* 本次函数调用耗费的总时间 */
	INSTR_TIME_SET_CURRENT(total);
	INSTR_TIME_SUBTRACT(total, fcu->start);

	/* 自身耗时：总耗时减去已计入其他调用的部分 */
	others = total_func_time;
	INSTR_TIME_SUBTRACT(others, fcu->save_total);
	self = total;
	INSTR_TIME_SUBTRACT(self, others);

	/* 更新后端级总时间 */
	INSTR_TIME_ADD(total_func_time, self);

	/*
	 * 将新的 total_time 计算为总流逝时间加上调用前的
	 * total_time 值。这样做是为了避免
	 * 把自身递归调用所耗的时间重复计入。（我们
	 * 无需为自身时间做类似处理，因为它已经排除
	 * 任何递归调用）
	 */
	INSTR_TIME_ADD(total, fcu->save_f_total_time);

	/* 更新函数统计表中的计数器 */
	if (finalize)
		fs->numcalls++;
	fs->total_time = total;
	INSTR_TIME_ADD(fs->self_time, self);
}

/*
 * 刷出该条目的待写入统计
 *
 * 若 nowait 为 true 且无法立即获得锁，则返回 false，
 * 不刷写该条目。否则返回 true。
 */
bool
pgstat_function_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_FunctionCounts *localent;
	PgStatShared_Function *shfuncent;

	localent = (PgStat_FunctionCounts *) entry_ref->pending;
	shfuncent = (PgStatShared_Function *) entry_ref->shared_stats;

	/* 局部条目总有非零内容 */

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	shfuncent->stats.numcalls += localent->numcalls;
	shfuncent->stats.total_time +=
		INSTR_TIME_GET_MICROSEC(localent->total_time);
	shfuncent->stats.self_time +=
		INSTR_TIME_GET_MICROSEC(localent->self_time);

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * 查找指定函数已有的 PgStat_FunctionCounts 条目
 *
 * 若无条目则返回 NULL，不新建条目
 */
PgStat_FunctionCounts *
find_funcstat_entry(Oid func_id)
{
	PgStat_EntryRef *entry_ref;

	entry_ref = pgstat_fetch_pending_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId, func_id);

	if (entry_ref)
		return entry_ref->pending;
	return NULL;
}

/*
 * SQL 可调用 pgstat* 函数的支持函数。返回
 * 一个函数的统计信息或 NULL。
 */
PgStat_StatFuncEntry *
pgstat_fetch_stat_funcentry(Oid func_id)
{
	return (PgStat_StatFuncEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_FUNCTION, MyDatabaseId, func_id);
}
