/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  The postgres vacuum cleaner.
 *
 * This file includes (a) control and dispatch code for VACUUM and ANALYZE
 * commands, (b) code to compute various vacuum thresholds, and (c) index
 * vacuum code.
 *
 * VACUUM for heap AM is implemented in vacuumlazy.c, parallel vacuum in
 * vacuumparallel.c, ANALYZE in analyze.c, and VACUUM FULL is a variant of
 * CLUSTER, handled in cluster.c.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/index.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * GUC 参数
 */
int			vacuum_freeze_min_age;
int			vacuum_freeze_table_age;
int			vacuum_multixact_freeze_min_age;
int			vacuum_multixact_freeze_table_age;
int			vacuum_failsafe_age;
int			vacuum_multixact_failsafe_age;

/*
 * 基于成本的清理延迟变量。默认值在自动清理和手动清理之间不同。应在清理
 * 代码中使用适当的 GUC 值进行设置。此处初始化为执行 VACUUM 或 ANALYZE
 * 的客户端后端的默认值。
 * executing VACUUM or ANALYZE.
 */
double		vacuum_cost_delay = 0;
int			vacuum_cost_limit = 200;

/*
 * VacuumFailsafeActive 定义为全局变量，以便确定清理表时是否重新启用
 * 基于成本的清理延迟。如果已启用故障安全模式，则在清理完成之前，
 * 不会为该表重新启用基于成本的延迟，无论其他设置如何。
 * 
 * 只有 VACUUM 代码应检查此变量，且只有表访问方法应将其设为 true。
 *在与表 AM 无关的 VACUUM 代码中，此变量用于确定是否允许基于成本的
 * 延迟。表 AM 可自由设置以实现此行为，但默认为 false，并在每次清理
 * 关系之间重置为 false。
 * inspected to determine whether or not to allow cost-based delays. Table AMs
 * are free to set it if they desire this behavior, but it is false by default
 * and reset to false in between vacuuming each relation.
 */
bool		VacuumFailsafeActive = false;

/*
 * 基于成本的并行清理变量。参见 compute_parallel_delay 顶部的注释以了
 * 解其工作原理。
 */
pg_atomic_uint32 *VacuumSharedCostBalance = NULL;
pg_atomic_uint32 *VacuumActiveNWorkers = NULL;
int			VacuumCostBalanceLocal = 0;

/* 非导出函数原型 */
static List *expand_vacuum_rel(VacuumRelation *vrel,
							   MemoryContext vac_context, int options);
static List *get_all_vacuum_rels(MemoryContext vac_context, int options);
static void vac_truncate_clog(TransactionId frozenXID,
							  MultiXactId minMulti,
							  TransactionId lastSaneFrozenXid,
							  MultiXactId lastSaneMinMulti);
static bool vacuum_rel(Oid relid, RangeVar *relation, VacuumParams *params,
					   BufferAccessStrategy bstrategy);
static double compute_parallel_delay(void);
static VacOptValue get_vacoptval_from_boolean(DefElem *def);
static bool vac_tid_reaped(ItemPointer itemptr, void *state);
static int	vac_cmp_itemptr(const void *left, const void *right);

/*
 * GUC 校验函数，确保指定的 GUC 值在允许的范围内。
 * range.
 */
bool
check_vacuum_buffer_usage_limit(int *newval, void **extra,
								GucSource source)
{
	/* 值的上下硬限制均包含在内 */
	if (*newval == 0 || (*newval >= MIN_BAS_VAC_RING_SIZE_KB &&
						 *newval <= MAX_BAS_VAC_RING_SIZE_KB))
		return true;

	/* 值不在任何允许的范围内 */
	GUC_check_errdetail("\"vacuum_buffer_usage_limit\" must be 0 or between %d kB and %d kB",
						MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB);

	return false;
}

/*
 * 手动 VACUUM 和 ANALYZE 命令的主要入口点
 *
 * 这主要是为 vacuum() 中将发生的实际操作做准备的包装器。
 * happen in vacuum().
 */
void
ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel)
{
	VacuumParams params;
	BufferAccessStrategy bstrategy = NULL;
	bool		verbose = false;
	bool		skip_locked = false;
	bool		analyze = false;
	bool		freeze = false;
	bool		full = false;
	bool		disable_page_skipping = false;
	bool		process_main = true;
	bool		process_toast = true;
	int			ring_size;
	bool		skip_database_stats = false;
	bool		only_database_stats = false;
	MemoryContext vac_context;
	ListCell   *lc;

	/* index_cleanup 和 truncate 值暂未指定 */
	params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
	params.truncate = VACOPTVALUE_UNSPECIFIED;

	/* 默认启用并行清理 */
	params.nworkers = 0;

	/*
	 * 将此设为无效值，以便在创建访问策略时清楚是否指定了
	 * BUFFER_USAGE_LIMIT。
	 */
	ring_size = -1;

	/* 解析选项列表 */
	foreach(lc, vacstmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		/* 解析 VACUUM 和 ANALYZE 的公共选项 */
		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "skip_locked") == 0)
			skip_locked = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffer_usage_limit") == 0)
		{
			const char *hintmsg;
			int			result;
			char	   *vac_buffer_size;

			vac_buffer_size = defGetString(opt);

			/*
			 * 检查指定值是否有效，如果大小不为 0，则应在硬上下限范围内。
			 * within the hard upper and lower limits if it is not 0.
			 */
			if (!parse_int(vac_buffer_size, &result, GUC_UNIT_KB, &hintmsg) ||
				(result != 0 &&
				 (result < MIN_BAS_VAC_RING_SIZE_KB || result > MAX_BAS_VAC_RING_SIZE_KB)))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("BUFFER_USAGE_LIMIT option must be 0 or between %d kB and %d kB",
								MIN_BAS_VAC_RING_SIZE_KB, MAX_BAS_VAC_RING_SIZE_KB),
						 hintmsg ? errhint("%s", _(hintmsg)) : 0));
			}

			ring_size = result;
		}
		else if (!vacstmt->is_vacuumcmd)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized ANALYZE option \"%s\"", opt->defname),
					 parser_errposition(pstate, opt->location)));

		/* 解析 VACUUM 可用的选项 */
		else if (strcmp(opt->defname, "analyze") == 0)
			analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "freeze") == 0)
			freeze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "full") == 0)
			full = defGetBoolean(opt);
		else if (strcmp(opt->defname, "disable_page_skipping") == 0)
			disable_page_skipping = defGetBoolean(opt);
		else if (strcmp(opt->defname, "index_cleanup") == 0)
		{
			/* 将无字符串解释为默认值，即 'auto' */
			if (!opt->arg)
				params.index_cleanup = VACOPTVALUE_AUTO;
			else
			{
				char	   *sval = defGetString(opt);

				/* 尝试匹配 'auto' 字符串，或回退到布尔值 */
				if (pg_strcasecmp(sval, "auto") == 0)
					params.index_cleanup = VACOPTVALUE_AUTO;
				else
					params.index_cleanup = get_vacoptval_from_boolean(opt);
			}
		}
		else if (strcmp(opt->defname, "process_main") == 0)
			process_main = defGetBoolean(opt);
		else if (strcmp(opt->defname, "process_toast") == 0)
			process_toast = defGetBoolean(opt);
		else if (strcmp(opt->defname, "truncate") == 0)
			params.truncate = get_vacoptval_from_boolean(opt);
		else if (strcmp(opt->defname, "parallel") == 0)
		{
			if (opt->arg == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parallel option requires a value between 0 and %d",
								MAX_PARALLEL_WORKER_LIMIT),
						 parser_errposition(pstate, opt->location)));
			}
			else
			{
				int			nworkers;

				nworkers = defGetInt32(opt);
				if (nworkers < 0 || nworkers > MAX_PARALLEL_WORKER_LIMIT)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("parallel workers for vacuum must be between 0 and %d",
									MAX_PARALLEL_WORKER_LIMIT),
							 parser_errposition(pstate, opt->location)));

				/*
				 * 如果用户指定并行度为 0，则禁用并行清理。
				 * degree as zero.
				 */
				if (nworkers == 0)
					params.nworkers = -1;
				else
					params.nworkers = nworkers;
			}
		}
		else if (strcmp(opt->defname, "skip_database_stats") == 0)
			skip_database_stats = defGetBoolean(opt);
		else if (strcmp(opt->defname, "only_database_stats") == 0)
			only_database_stats = defGetBoolean(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized VACUUM option \"%s\"", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* 设置 vacuum 选项 */
	params.options =
		(vacstmt->is_vacuumcmd ? VACOPT_VACUUM : VACOPT_ANALYZE) |
		(verbose ? VACOPT_VERBOSE : 0) |
		(skip_locked ? VACOPT_SKIP_LOCKED : 0) |
		(analyze ? VACOPT_ANALYZE : 0) |
		(freeze ? VACOPT_FREEZE : 0) |
		(full ? VACOPT_FULL : 0) |
		(disable_page_skipping ? VACOPT_DISABLE_PAGE_SKIPPING : 0) |
		(process_main ? VACOPT_PROCESS_MAIN : 0) |
		(process_toast ? VACOPT_PROCESS_TOAST : 0) |
		(skip_database_stats ? VACOPT_SKIP_DATABASE_STATS : 0) |
		(only_database_stats ? VACOPT_ONLY_DATABASE_STATS : 0);

	/* 选项的健全性检查 */
	Assert(params.options & (VACOPT_VACUUM | VACOPT_ANALYZE));
	Assert((params.options & VACOPT_VACUUM) ||
		   !(params.options & (VACOPT_FULL | VACOPT_FREEZE)));

	if ((params.options & VACOPT_FULL) && params.nworkers > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM FULL cannot be performed in parallel")));

	/*
	 * BUFFER_USAGE_LIMIT 对 VACUUM (FULL) 无任何作用，因此对该情况直接
	 * 引发 ERROR。VACUUM (FULL, ANALYZE) 确实会使用它，所以我们允许
	 * 这种情况。
	 */
	if (ring_size != -1 && (params.options & VACOPT_FULL) &&
		!(params.options & VACOPT_ANALYZE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BUFFER_USAGE_LIMIT cannot be specified for VACUUM FULL")));

	/*
	 * 如果存在任何列列表，确保指定了 VACOPT_ANALYZE。
	 */
	if (!(params.options & VACOPT_ANALYZE))
	{
		foreach(lc, vacstmt->rels)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);

			if (vrel->va_cols != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ANALYZE option must be specified when a column list is provided")));
		}
	}


	/*
	 * 对 DISABLE_PAGE_SKIPPING 选项进行健全性检查。
	 */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_DISABLE_PAGE_SKIPPING) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VACUUM option DISABLE_PAGE_SKIPPING cannot be used with FULL")));

	/* PROCESS_TOAST 的健全性检查 */
	if ((params.options & VACOPT_FULL) != 0 &&
		(params.options & VACOPT_PROCESS_TOAST) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PROCESS_TOAST required with VACUUM FULL")));

	/* ONLY_DATABASE_STATS 的健全性检查 */
	if (params.options & VACOPT_ONLY_DATABASE_STATS)
	{
		Assert(params.options & VACOPT_VACUUM);
		if (vacstmt->rels != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with a list of tables")));
		/* 不要求用户显式关闭 PROCESS_TOAST/MAIN */
		if (params.options & ~(VACOPT_VACUUM |
							   VACOPT_VERBOSE |
							   VACOPT_PROCESS_MAIN |
							   VACOPT_PROCESS_TOAST |
							   VACOPT_ONLY_DATABASE_STATS))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ONLY_DATABASE_STATS cannot be specified with other VACUUM options")));
	}

	/*
	 * 如果指定了 FREEZE 选项，则所有冻结年龄为零；否则传递 -1 表示使用
	 * 默认值。
	 */
	if (params.options & VACOPT_FREEZE)
	{
		params.freeze_min_age = 0;
		params.freeze_table_age = 0;
		params.multixact_freeze_min_age = 0;
		params.multixact_freeze_table_age = 0;
	}
	else
	{
		params.freeze_min_age = -1;
		params.freeze_table_age = -1;
		params.multixact_freeze_min_age = -1;
		params.multixact_freeze_table_age = -1;
	}

	/* 用户调用的清理绝不"用于回卷" */
	params.is_wraparound = false;

	/* 用户调用的清理使用 VACOPT_VERBOSE 而非 log_min_duration */
	params.log_min_duration = -1;

	/*
	 * 为跨事务存储创建特殊内存上下文。
	 *
	 * 由于它是 PortalContext 的子上下文，即使发生错误最终也会被清除；
	 * 无需特殊的异常中止清理逻辑。
	 */
	vac_context = AllocSetContextCreate(PortalContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * 在跨事务内存上下文中创建缓冲区策略对象。对于 VACUUM (FULL) 或
	 * VACUUM (ONLY_DATABASE_STATS)，无需创建，因为它们不会使用它。
	 * VACUUM (FULL, ANALYZE) 是可能的，因此当看到 ANALYZE 时，最好确保
	 * 创建策略。
	 * when we see ANALYZE.
	 */
	if ((params.options & (VACOPT_ONLY_DATABASE_STATS |
						   VACOPT_FULL)) == 0 ||
		(params.options & VACOPT_ANALYZE) != 0)
	{

		MemoryContext old_context = MemoryContextSwitchTo(vac_context);

		Assert(ring_size >= -1);

		/*
		 * 如果 VACUUM 或 ANALYZE 命令指定了 BUFFER_USAGE_LIMIT，它将覆盖
		 * VacuumBufferUsageLimit 的值。任一值可为 0，此时
		 * GetAccessStrategyWithSize() 将返回 NULL，有效地允许完全使用共享
		 * 缓冲区。
		 */
		if (ring_size == -1)
			ring_size = VacuumBufferUsageLimit;

		bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, ring_size);

		MemoryContextSwitchTo(old_context);
	}

	/* 现在执行公共例程 */
	vacuum(vacstmt->rels, &params, bstrategy, vac_context, isTopLevel);

	/* 最后，清理 vacuum 内存上下文 */
	MemoryContextDelete(vac_context);
}

/*
 * 自动清理和 VACUUM / ANALYZE 命令的内部入口点。
 *
 * relations 如果不为 NIL，是要处理的一列 VacuumRelation；否则，
 * 处理数据库中所有相关的表。对于每个 VacuumRelation，如果提供了有效
 * 的 OID，则处理具有该 OID 的表；否则，VacuumRelation 的 RangeVar
 * 指示要处理什么。
 *
 * params 包含一组可用于自定义行为的参数。
 * 
 *bstrategy 可以在调用方不想限制 VACUUM / ANALYZE 可使用的
 * shared_buffers 数量时传入 NULL，否则调用方必须构建一个
 * BufferAccessStrategy，其中包含 VACUUM / ANALYZE 应尝试限制自身
 * 使用的 shared_buffers 数量。
 * 
 * isTopLevel 应从 ProcessUtility 向下传递。
 *
 * 调用方有责任确保所有参数分配在事务提交后不会消失的内存上下文中。
 *
 * It is the caller's responsibility that all parameters are allocated in a
 * memory context that will not disappear at transaction commit.
 */
void
vacuum(List *relations, VacuumParams *params, BufferAccessStrategy bstrategy,
	   MemoryContext vac_context, bool isTopLevel)
{
	static bool in_vacuum = false;

	const char *stmttype;
	volatile bool in_outer_xact,
				use_own_xacts;

	Assert(params != NULL);

	stmttype = (params->options & VACOPT_VACUUM) ? "VACUUM" : "ANALYZE";

	/*
	 * 我们不能在用户事务块内运行 VACUUM；如果我们在事务内部，那么我们的
	 * 提交和启动事务命令调用将不会产生预期效果！还有很多其他微妙的依赖
	 * 关系也与此相关。
	 * 
	 *ANALYZE（不带 VACUUM）可以按任意方式运行。
	 * ANALYZE (without VACUUM) can run either way.
	 */
	if (params->options & VACOPT_VACUUM)
	{
		PreventInTransactionBlock(isTopLevel, stmttype);
		in_outer_xact = false;
	}
	else
		in_outer_xact = IsInTransactionBlock(isTopLevel);

	/*
	 * 检查并禁止递归调用。这可能发生在 VACUUM FULL 或 ANALYZE 调用恶意
	 * 的索引表达式，而该表达式自身又调用 ANALYZE 时。
	 * ANALYZE.
	 */
	if (in_vacuum)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s cannot be executed from VACUUM or ANALYZE",
						stmttype)));

	/*
	 * 构建要处理的关系列表，将任何新数据放入 vac_context 以安全保存。
	 * vac_context for safekeeping.
	 */
	if (params->options & VACOPT_ONLY_DATABASE_STATS)
	{
		/* 在这种情况下我们不处理任何表 */
		Assert(relations == NIL);
	}
	else if (relations != NIL)
	{
		List	   *newrels = NIL;
		ListCell   *lc;

		foreach(lc, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
			List	   *sublist;
			MemoryContext old_context;

			sublist = expand_vacuum_rel(vrel, vac_context, params->options);
			old_context = MemoryContextSwitchTo(vac_context);
			newrels = list_concat(newrels, sublist);
			MemoryContextSwitchTo(old_context);
		}
		relations = newrels;
	}
	else
		relations = get_all_vacuum_rels(vac_context, params->options);

	/*
	 * 决定是否需要启动/提交我们自己的事务。
	 *
	 * 对于 VACUUM（带或不带 ANALYZE）：始终如此，以便尽快释放锁。
	 * （对于单表 VACUUM，我们可能可以使用外部事务，但处理 TOAST 表
	 * 会有问题。）
	 * 
	 *对于 ANALYZE（不带 VACUUM）：如果在事务块内，我们不能启动/提交
	 * 自己的事务。此外，如果只处理一个关系则无需这样做。当不在事务块内
	 * 且处理多个关系时，以及在自动清理工作进程中，使用自己的事务以便
	 * 更早释放锁。
	 * transaction block, and also in an autovacuum worker, use own
	 * transactions so we can release locks sooner.
	 */
	if (params->options & VACOPT_VACUUM)
		use_own_xacts = true;
	else
	{
		Assert(params->options & VACOPT_ANALYZE);
		if (IsAutoVacuumWorkerProcess())
			use_own_xacts = true;
		else if (in_outer_xact)
			use_own_xacts = false;
		else if (list_length(relations) > 1)
			use_own_xacts = true;
		else
			use_own_xacts = false;
	}

	/*
	 * vacuum_rel 期望在没有活动事务的情况下进入；它将启动并提交自己的
	 * 事务。但我们是由 SQL 命令调用的，因此我们已经在事务内部执行。
	 * 我们在此提交在 PostgresMain() 中启动的事务，并在退出前启动另一个
	 * 事务，以匹配在 PostgresMain() 中等待我们的提交。
	 * another one before exiting to match the commit waiting for us back in
	 * PostgresMain().
	 */
	if (use_own_xacts)
	{
		Assert(!in_outer_xact);

		/* ActiveSnapshot 不由自动清理设置 */
		if (ActiveSnapshotSet())
			PopActiveSnapshot();

		/* 匹配 PostgresMain() 中的 StartTransaction */
		CommitTransactionCommand();
	}

	/* 打开或关闭清理成本核算，并设置/清除 in_vacuum */
	PG_TRY();
	{
		ListCell   *cur;

		in_vacuum = true;
		VacuumFailsafeActive = false;
		VacuumUpdateCosts();
		VacuumCostBalance = 0;
		VacuumPageHit = 0;
		VacuumPageMiss = 0;
		VacuumPageDirty = 0;
		VacuumCostBalanceLocal = 0;
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;

		/*
		 * 循环处理每个选定的关系。
		 */
		foreach(cur, relations)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, cur);

			if (params->options & VACOPT_VACUUM)
			{
				VacuumParams params_copy;

				/*
				 * vacuum_rel() 会在参数上涂写，因此给它一份副本以避免影响其他关系。
				 * to avoid affecting other relations.
				 */
				memcpy(&params_copy, params, sizeof(VacuumParams));

				if (!vacuum_rel(vrel->oid, vrel->relation, &params_copy, bstrategy))
					continue;
			}

			if (params->options & VACOPT_ANALYZE)
			{
				/*
				 * 如果使用单独的事务，为 ANALYZE 启动一个。否则，我们可以使用外部
				 * 事务。
				 */
				if (use_own_xacts)
				{
					StartTransactionCommand();
					/* 索引中的函数可能需要设置快照 */
					PushActiveSnapshot(GetTransactionSnapshot());
				}

				analyze_rel(vrel->oid, vrel->relation, params,
							vrel->va_cols, in_outer_xact, bstrategy);

				if (use_own_xacts)
				{
					PopActiveSnapshot();
					/* 如果 !use_own_xacts，standard_ProcessUtility() 执行 CCI */
					CommandCounterIncrement();
					CommitTransactionCommand();
				}
				else
				{
					/*
					 * 如果我们不使用单独的事务，最好用 CCI 分隔 ANALYZE 操作。这避免了
					 * 用户说 "ANALYZE t, t" 时出现问题。
					 * says "ANALYZE t, t".
					 */
					CommandCounterIncrement();
				}
			}

			/*
			 * 确保在清理下一个关系之前重置 VacuumFailsafeActive。
			 * next relation.
			 */
			VacuumFailsafeActive = false;
		}
	}
	PG_FINALLY();
	{
		in_vacuum = false;
		VacuumCostActive = false;
		VacuumFailsafeActive = false;
		VacuumCostBalance = 0;
	}
	PG_END_TRY();

	/*
	 * 完成处理。
	 */
	if (use_own_xacts)
	{
		/* 此处，我们不在事务中 */

		/*
		 * 这与 PostgresMain() 中等待我们的 CommitTransaction 相匹配。
		 * PostgresMain().
		 */
		StartTransactionCommand();
	}

	if ((params->options & VACOPT_VACUUM) &&
		!(params->options & VACOPT_SKIP_DATABASE_STATS))
	{
		/*
		 * 更新 pg_database.datfrozenxid，并在可能时截断 pg_xact。
		 */
		vac_update_datfrozenxid();
	}

}

/*
 * 检查给定关系是否可以安全地清理或分析。如果用户不是关系所有者，
 * 则发出 WARNING 日志消息并返回 false，让调用方决定如何处理此关系。
 * 此例程用于决定关系是否可以被处理以执行 VACUUM 或 ANALYZE。
 * routine is used to decide if a relation can be processed for VACUUM or
 * ANALYZE.
 */
bool
vacuum_is_relation_owner(Oid relid, Form_pg_class reltuple, bits32 options)
{
	char	   *relname;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*
	 * 检查权限。
	 *
	 * 如果用户是超级用户、表所有者或数据库所有者（但在后一种情况下，
	 * 仅当不是共享关系时），允许用户清理或分析表。
	 * object_ownercheck 包含超级用户的情况。
	 * 
	 *注意，我们选择将权限失败视为 WARNING 并继续尝试清理或分析数据库
	 * 的其余部分 --- 这合适吗？
	 * trying to vacuum or analyze the rest of the DB --- is this appropriate?
	 */
	if (object_ownercheck(RelationRelationId, relid, GetUserId()) ||
		(object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) && !reltuple->relisshared))
		return true;

	relname = NameStr(reltuple->relname);

	if ((options & VACOPT_VACUUM) != 0)
	{
		ereport(WARNING,
				(errmsg("permission denied to vacuum \"%s\", skipping it",
						relname)));

		/*
		 * 对于 VACUUM ANALYZE，两条日志都可能出现，但仅为 VACUUM 生成信息，
		 * 因为那将是第一个被处理的。
		 * processed.
		 */
		return false;
	}

	if ((options & VACOPT_ANALYZE) != 0)
		ereport(WARNING,
				(errmsg("permission denied to analyze \"%s\", skipping it",
						relname)));

	return false;
}


/*
 * vacuum_open_relation
 *
 * 此例程用于尝试打开并锁定即将被清理或分析的关系。如果无法打开或
 * 锁定关系，则在可能的情况下发出日志。
 * or locked, a log is emitted if possible.
 */
Relation
vacuum_open_relation(Oid relid, RangeVar *relation, bits32 options,
					 bool verbose, LOCKMODE lmode)
{
	Relation	rel;
	bool		rel_lock = true;
	int			elevel;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*
	 * 打开关系并获取适当的锁。
	 *
	 * 这里存在竞态条件：关系可能自上次我们看到它以来已经消失。如果是
	 * 这样，我们不需要清理或分析它。
	 *
	 * 如果我们被要求不等待关系锁，则在调用 try_relation_open() 之前，
	 * 先以非阻塞模式获取它。
	 */
	if (!(options & VACOPT_SKIP_LOCKED))
		rel = try_relation_open(relid, lmode);
	else if (ConditionalLockRelationOid(relid, lmode))
		rel = try_relation_open(relid, NoLock);
	else
	{
		rel = NULL;
		rel_lock = false;
	}

	/* 如果关系已打开，则离开 */
	if (rel)
		return rel;

	/*
	 * 无法打开关系，因此尽可能生成一条记录情况的日志。
	 * 
	 *如果未定义 RangeVar，则我们没有足够的信息来提供有意义的日志语句。
	 * 很可能调用方有意不提供此信息，以便无论如何都跳过此日志记录。
	 * provide a meaningful log statement.  Chances are that the caller has
	 * intentionally not provided this information so that this logging is
	 * skipped, anyway.
	 */
	if (relation == NULL)
		return NULL;

	/*
	 * 确定日志级别。
	 *
	 * 对于手动 VACUUM 或 ANALYZE，我们发出 WARNING 以匹配权限检查中的
	 * 日志语句；否则，仅在调用方请求时才记录。
	 * so requested.
	 */
	if (!IsAutoVacuumWorkerProcess())
		elevel = WARNING;
	else if (verbose)
		elevel = LOG;
	else
		return NULL;

	if ((options & VACOPT_VACUUM) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping vacuum of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping vacuum of \"%s\" --- relation no longer exists",
							relation->relname)));

		/*
		 * 对于 VACUUM ANALYZE，两条日志都可能出现，但仅为 VACUUM 生成信息，
		 * 因为那将是第一个被处理的。
		 * processed.
		 */
		return NULL;
	}

	if ((options & VACOPT_ANALYZE) != 0)
	{
		if (!rel_lock)
			ereport(elevel,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping analyze of \"%s\" --- lock not available",
							relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("skipping analyze of \"%s\" --- relation no longer exists",
							relation->relname)));
	}

	return NULL;
}


/*
 * 给定一个 VacuumRelation，如果未指定表 OID 则填充它，并可选择为表
 * 的分区添加 VacuumRelation。
 *
 * 如果 VacuumRelation 未提供 OID 且是分区表，则将为每个分区在输出中
 * 添加一个额外的条目。目前，只有自动清理在调用 vacuum() 时提供 OID，
 * 且它不希望我们展开分区表。
 * 
 *我们注意不修改输入数据结构，而是构建新的 VacuumRelation 以返回。
 * （但请注意，它们将引用输入的未修改部分，例如列列表。）新的数据
 * 结构在 vac_context 中创建。
 * unmodified parts of the input, eg column lists.)  New data structures
 * are made in vac_context.
 */
static List *
expand_vacuum_rel(VacuumRelation *vrel, MemoryContext vac_context,
				  int options)
{
	List	   *vacrels = NIL;
	MemoryContext oldcontext;

	/* 如果调用方提供了 OID，则此处无需执行任何操作。 */
	if (OidIsValid(vrel->oid))
	{
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, vrel);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/* 处理特定的关系，可能还有其分区 */
		Oid			relid;
		HeapTuple	tuple;
		Form_pg_class classForm;
		bool		include_parts;
		int			rvr_opts;

		/*
		 * 由于自动清理工作进程在调用 vacuum() 时提供 OID，因此不应有自动
		 * 清理工作进程到达此代码。
		 */
		Assert(!IsAutoVacuumWorkerProcess());

		/*
		 * 我们暂时获取 AccessShareLock 以保护下面的 syscache 查找，以及
		 * find_all_inheritors 的期望，即调用方在起始关系上持有某个锁。
		 * holds some lock on the starting relation.
		 */
		rvr_opts = (options & VACOPT_SKIP_LOCKED) ? RVR_SKIP_LOCKED : 0;
		relid = RangeVarGetRelidExtended(vrel->relation,
										 AccessShareLock,
										 rvr_opts,
										 NULL, NULL);

		/*
		 * 如果锁不可用，发出与 vacuum_rel() 和 analyze_rel() 相同的日志语句。
		 * vacuum_rel() and analyze_rel() would.
		 */
		if (!OidIsValid(relid))
		{
			if (options & VACOPT_VACUUM)
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping vacuum of \"%s\" --- lock not available",
								vrel->relation->relname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("skipping analyze of \"%s\" --- lock not available",
								vrel->relation->relname)));
			return vacrels;
		}

		/*
		 * 要检查关系是否为分区表及其所有权，获取其 syscache 条目。
		 * ownership, fetch its syscache entry.
		 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * 如果用户是适当的所有者，为此关系创建可返回的 VacuumRelation。
		 * owner.
		 */
		if (vacuum_is_relation_owner(relid, classForm, options))
		{
			oldcontext = MemoryContextSwitchTo(vac_context);
			vacrels = lappend(vacrels, makeVacuumRelation(vrel->relation,
														  relid,
														  vrel->va_cols));
			MemoryContextSwitchTo(oldcontext);
		}


		include_parts = (classForm->relkind == RELKIND_PARTITIONED_TABLE);
		ReleaseSysCache(tuple);

		/*
		 * 如果是，为其分区创建关系列表条目。注意 find_all_inheritors() 返回
		 * 的列表包含传入的 OID，因此我们必须跳过它。目前对各个分区加锁没有
		 * 意义，这样做只会增加不必要的死锁风险。出于最后这个原因，我们尚未
		 * 检查分区的所有权，它们被添加到要处理的列表中。所有权将在稍后检查。
		 * unnecessary deadlock risk.  For this last reason we do not check
		 * yet the ownership of the partitions, which get added to the list to
		 * process.  Ownership will be checked later on anyway.
		 */
		if (include_parts)
		{
			List	   *part_oids = find_all_inheritors(relid, NoLock, NULL);
			ListCell   *part_lc;

			foreach(part_lc, part_oids)
			{
				Oid			part_oid = lfirst_oid(part_lc);

				if (part_oid == relid)
					continue;	/* 忽略原始表 */

				/*
				 * 我们省略了 RangeVar，因为稍后抱怨打开这些关系中某个失败是不合适的。
				 * complain about failure to open one of these relations
				 * later.
				 */
				oldcontext = MemoryContextSwitchTo(vac_context);
				vacrels = lappend(vacrels, makeVacuumRelation(NULL,
															  part_oid,
															  vrel->va_cols));
				MemoryContextSwitchTo(oldcontext);
			}
		}

		/*
		 * 再次释放锁。这意味着当我们实际尝试处理该表时，它可能已经消失或
		 * 重命名。在前一种情况下，我们将静默地忽略它；在后一种情况下，我们
		 * 仍将处理它，但必须注意 RangeVar 不再必然标识它。这可能不是理想的，
		 * 但几乎没有实际替代方案，因为我们通常会在现在和那时之间提交此事务
		 * 并开始一个新事务。此外，在多个关系上持有锁会产生显著的死锁风险。
		 * practical alternative, since we're typically going to commit this
		 * transaction and begin a new one between now and then.  Moreover,
		 * holding locks on multiple relations would create significant risk
		 * of deadlock.
		 */
		UnlockRelationOid(relid, AccessShareLock);
	}

	return vacrels;
}

/*
 * 为当前数据库中所有可清理的关系构建 VacuumRelation 列表。该列表在
 * vac_context 中构建。
 */
static List *
get_all_vacuum_rels(MemoryContext vac_context, int options)
{
	List	   *vacrels = NIL;
	Relation	pgclass;
	TableScanDesc scan;
	HeapTuple	tuple;

	pgclass = table_open(RelationRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pgclass, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		MemoryContext oldcontext;
		Oid			relid = classForm->oid;

		/*
		 * 我们在此包含分区表；取决于要执行的操作，调用方将决定是处理还是
		 * 忽略它们。
		 * them.
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* 检查关系的权限 */
		if (!vacuum_is_relation_owner(relid, classForm, options))
			continue;

		/*
		 * 构建指定要处理的表 OID 的 VacuumRelation。我们省略了 RangeVar，
		 * 因为稍后抱怨打开这些关系中某个失败是不合适的。
		 * about failure to open one of these relations later.
		 */
		oldcontext = MemoryContextSwitchTo(vac_context);
		vacrels = lappend(vacrels, makeVacuumRelation(NULL,
													  relid,
													  NIL));
		MemoryContextSwitchTo(oldcontext);
	}

	table_endscan(scan);
	table_close(pgclass, AccessShareLock);

	return vacrels;
}

/*
 * vacuum_get_cutoffs() -- 计算 OldestXmin 和冻结截断点
 *
 * 输入为目标关系及 VACUUM 参数。
 *
 * 输出参数为 VACUUM 调用方应使用的截断值。
 *
 * 返回值指示 vacuumlazy.c 调用方是否应将其 VACUUM 操作设为激进的。
 * 激进的 VACUUM 必须将 relfrozenxid 至少推进到 FreezeLimit，
 * 并将 relminmxid 至少推进到 MultiXactCutoff。
 * minimum).
 */
bool
vacuum_get_cutoffs(Relation rel, const VacuumParams *params,
				   struct VacuumCutoffs *cutoffs)
{
	int			freeze_min_age,
				multixact_freeze_min_age,
				freeze_table_age,
				multixact_freeze_table_age,
				effective_multixact_freeze_max_age;
	TransactionId nextXID,
				safeOldestXmin,
				aggressiveXIDCutoff;
	MultiXactId nextMXID,
				safeOldestMxact,
				aggressiveMXIDCutoff;

	/* 使用冻结年龄参数的可变副本 */
	freeze_min_age = params->freeze_min_age;
	multixact_freeze_min_age = params->multixact_freeze_min_age;
	freeze_table_age = params->freeze_table_age;
	multixact_freeze_table_age = params->multixact_freeze_table_age;

	/* 设置截断值中的 pg_class 字段 */
	cutoffs->relfrozenxid = rel->rd_rel->relfrozenxid;
	cutoffs->relminmxid = rel->rd_rel->relminmxid;

	/*
	 * 获取 OldestXmin。
	 *
	 * 我们始终可以忽略运行惰性清理的进程。这是因为我们仅使用这些值来
	 * 决定必须保留在表中的元组。由于惰性清理通常不会将其 XID 写在任何
	 * 地方（通常不会分配 XID），因此可以安全地忽略它。理论上，在完整
	 * 清理中忽略惰性清理可能会有问题，但请记住，任何时候只有一个清理
	 * 进程可以在特定表上工作，且每个清理始终是独立的事务。
	 * that only one vacuum process can be working on a particular table at
	 * any time, and that each vacuum is always an independent transaction.
	 */
	cutoffs->OldestXmin = GetOldestNonRemovableTransactionId(rel);

	if (OldSnapshotThresholdActive())
	{
		TransactionId limit_xmin;
		TimestampTz limit_ts;

		if (TransactionIdLimitedForOldSnapshots(cutoffs->OldestXmin, rel,
												&limit_xmin, &limit_ts))
		{
			/*
			 * TODO: 我们应仅在基于增加的阈值进行修剪时才设置阈值。这里不如
			 * 机会性修剪（通常以更高的频率发生）那么关键，但仍将是显著的改进。
			 * for opportunistic pruning (which often happens at a much higher
			 * frequency), but would still be a significant improvement.
			 */
			SetOldSnapshotThresholdTimestamp(limit_ts, limit_xmin);
			cutoffs->OldestXmin = limit_xmin;
		}
	}

	Assert(TransactionIdIsNormal(cutoffs->OldestXmin));

	/* 获取 OldestMxact */
	cutoffs->OldestMxact = GetOldestMultiXactId();
	Assert(MultiXactIdIsValid(cutoffs->OldestMxact));

	/* 获取 next XID/next MXID 值，用于应用基于年龄的设置 */
	nextXID = ReadNextTransactionId();
	nextMXID = ReadNextMultiXactId();

	/*
	 * 同时计算冻结紧急的多事务年龄。通常是
	 * autovacuum_multixact_freeze_max_age，但如果多事务成员空间不足，
	 * 则可能更小。
	 */
	effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold();

	/*
	 * 准备设置冻结输出参数之前，先检查 OldestXmin 或 OldestMxact 是否
	 * 被延迟到不安全的程度
	 */
	safeOldestXmin = nextXID - autovacuum_freeze_max_age;
	if (!TransactionIdIsNormal(safeOldestXmin))
		safeOldestXmin = FirstNormalTransactionId;
	safeOldestMxact = nextMXID - effective_multixact_freeze_max_age;
	if (safeOldestMxact < FirstMultiXactId)
		safeOldestMxact = FirstMultiXactId;
	if (TransactionIdPrecedes(cutoffs->OldestXmin, safeOldestXmin))
		ereport(WARNING,
				(errmsg("cutoff for removing and freezing tuples is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, safeOldestMxact))
		ereport(WARNING,
				(errmsg("cutoff for freezing multixacts is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.\n"
						 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));

	/*
	 * 确定要使用的最小冻结年龄：由调用方指定，或使用
	 * vacuum_freeze_min_age，但无论如何不应超过
	 * autovacuum_freeze_max_age 的一半，以免防 XID 回卷的自动清理触发
	 * 过于频繁。
	 */
	if (freeze_min_age < 0)
		freeze_min_age = vacuum_freeze_min_age;
	freeze_min_age = Min(freeze_min_age, autovacuum_freeze_max_age / 2);
	Assert(freeze_min_age >= 0);

	/* 计算 FreezeLimit，注意生成正常的 XID */
	cutoffs->FreezeLimit = nextXID - freeze_min_age;
	if (!TransactionIdIsNormal(cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = FirstNormalTransactionId;
	/* FreezeLimit 必须始终 <= OldestXmin */
	if (TransactionIdPrecedes(cutoffs->OldestXmin, cutoffs->FreezeLimit))
		cutoffs->FreezeLimit = cutoffs->OldestXmin;

	/*
	 * 确定要使用的最小多事务冻结年龄：由调用方指定，或使用
	 * vacuum_multixact_freeze_min_age，但无论如何不应超过
	 * effective_multixact_freeze_max_age 的一半，以免防 MultiXact 回卷
	 * 的自动清理触发过于频繁。
	 */
	if (multixact_freeze_min_age < 0)
		multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
	multixact_freeze_min_age = Min(multixact_freeze_min_age,
								   effective_multixact_freeze_max_age / 2);
	Assert(multixact_freeze_min_age >= 0);

	/* 计算 MultiXactCutoff，注意生成有效值 */
	cutoffs->MultiXactCutoff = nextMXID - multixact_freeze_min_age;
	if (cutoffs->MultiXactCutoff < FirstMultiXactId)
		cutoffs->MultiXactCutoff = FirstMultiXactId;
	/* MultiXactCutoff 必须始终 <= OldestMxact */
	if (MultiXactIdPrecedes(cutoffs->OldestMxact, cutoffs->MultiXactCutoff))
		cutoffs->MultiXactCutoff = cutoffs->OldestMxact;

	/*
	 * 最后，确定调用方是否需要执行激进的 VACUUM。
	 *
	 * 确定要使用的表冻结年龄：由调用方指定，或使用
	 * vacuum_freeze_table_age GUC 的值，但无论如何不应超过
	 * autovacuum_freeze_max_age * 0.95，这样如果你有例如每晚 VACUUM
	 * 调度，每晚 VACUUM 就有机会在防回卷自动清理启动之前冻结 XID。
	 * anti-wraparound autovacuum is launched.
	 */
	if (freeze_table_age < 0)
		freeze_table_age = vacuum_freeze_table_age;
	freeze_table_age = Min(freeze_table_age, autovacuum_freeze_max_age * 0.95);
	Assert(freeze_table_age >= 0);
	aggressiveXIDCutoff = nextXID - freeze_table_age;
	if (!TransactionIdIsNormal(aggressiveXIDCutoff))
		aggressiveXIDCutoff = FirstNormalTransactionId;
	if (TransactionIdPrecedesOrEquals(rel->rd_rel->relfrozenxid,
									  aggressiveXIDCutoff))
		return true;

	/*
	 * 与上述类似，确定用于多事务的表冻结年龄：由调用方指定，或使用
	 * vacuum_multixact_freeze_table_age GUC 的值，但无论如何不应超过
	 * effective_multixact_freeze_max_age * 0.95，这样如果你有例如每晚
	 * VACUUM 调度，每晚 VACUUM 就有机会在防回卷自动清理启动之前冻结
	 * 多事务。
	 * multixacts before anti-wraparound autovacuum is launched.
	 */
	if (multixact_freeze_table_age < 0)
		multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	multixact_freeze_table_age =
		Min(multixact_freeze_table_age,
			effective_multixact_freeze_max_age * 0.95);
	Assert(multixact_freeze_table_age >= 0);
	aggressiveMXIDCutoff = nextMXID - multixact_freeze_table_age;
	if (aggressiveMXIDCutoff < FirstMultiXactId)
		aggressiveMXIDCutoff = FirstMultiXactId;
	if (MultiXactIdPrecedesOrEquals(rel->rd_rel->relminmxid,
									aggressiveMXIDCutoff))
		return true;

	/* 非激进 VACUUM */
	return false;
}

/*
 * vacuum_xid_failsafe_check() -- 由 VACUUM 的回卷故障安全机制使用，
 * 用于确定表的 relfrozenxid 和 relminmxid 是否已危险地滞后。
 * 
 *当返回 true 时，VACUUM 调用方触发故障安全模式。
 * When we return true, VACUUM caller triggers the failsafe.
 */
bool
vacuum_xid_failsafe_check(const struct VacuumCutoffs *cutoffs)
{
	TransactionId relfrozenxid = cutoffs->relfrozenxid;
	MultiXactId relminmxid = cutoffs->relminmxid;
	TransactionId xid_skip_limit;
	MultiXactId multi_skip_limit;
	int			skip_index_vacuum;

	Assert(TransactionIdIsNormal(relfrozenxid));
	Assert(MultiXactIdIsValid(relminmxid));

	/*
	 * 确定要使用的索引跳过年龄。在任何情况下不应小于
	 * autovacuum_freeze_max_age * 1.05。
	 */
	skip_index_vacuum = Max(vacuum_failsafe_age, autovacuum_freeze_max_age * 1.05);

	xid_skip_limit = ReadNextTransactionId() - skip_index_vacuum;
	if (!TransactionIdIsNormal(xid_skip_limit))
		xid_skip_limit = FirstNormalTransactionId;

	if (TransactionIdPrecedes(relfrozenxid, xid_skip_limit))
	{
		/* 表的 relfrozenxid 太旧 */
		return true;
	}

	/*
	 * 与上述类似，确定用于多事务的索引跳过年龄。在任何情况下不应小于
	 * autovacuum_multixact_freeze_max_age * 1.05。
	 * 1.05.
	 */
	skip_index_vacuum = Max(vacuum_multixact_failsafe_age,
							autovacuum_multixact_freeze_max_age * 1.05);

	multi_skip_limit = ReadNextMultiXactId() - skip_index_vacuum;
	if (multi_skip_limit < FirstMultiXactId)
		multi_skip_limit = FirstMultiXactId;

	if (MultiXactIdPrecedes(relminmxid, multi_skip_limit))
	{
		/* 表的 relminmxid 太旧 */
		return true;
	}

	return false;
}

/*
 * vac_estimate_reltuples() -- 估计 pg_class.reltuples 的新值
 *
 *如果我们扫描了整个关系，则只需使用看到的存活元组数；但如果没有，
 *我们不应盲目地从该数字外推，因为 VACUUM 可能扫描了表的某个相当
 *不随机的子集。当我们只有部分信息时，我们取 pg_class.reltuples /
 *pg_class.relpages 的旧值作为未扫描页面中元组密度的度量。
 *
 *注意：scanned_tuples 应仅计数*存活*元组，因 pg_class.reltuples 是
 *这样定义的。
 *		Note: scanned_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 */
double
vac_estimate_reltuples(Relation relation,
					   BlockNumber total_pages,
					   BlockNumber scanned_pages,
					   double scanned_tuples)
{
	BlockNumber old_rel_pages = relation->rd_rel->relpages;
	double		old_rel_tuples = relation->rd_rel->reltuples;
	double		old_density;
	double		unscanned_pages;
	double		total_tuples;

	/* 如果确实扫描了整个表，直接使用该计数值 */
	if (scanned_pages >= total_pages)
		return scanned_tuples;

	/*
	 * 当连续的 VACUUM 命令反复扫描相同几个页面，而表中没有任何实际变化时，
	 * 我们对元组密度的信念可能逐渐失真。这可能是由 vacuumlazy.c 的实现
	 * 细节引起的，例如它倾向于始终扫描最后一个堆页面。在此处理此情况。
	 * 
	 * 如果根据现有 pg_class 条目关系大小*完全相同*，且仅扫描了其中少数
	 *页面（少于 2%），则保留 reltuples 的现有值。当只扫描了不超过单个
	 * 页面的子集时，也保留现有值。
	 * 
	 * （注意：此处可能返回 -1。）
	 * value when only a subset of rel's pages <= a single page were scanned.
	 *
	 * (Note: we might be returning -1 here.)
	 */
	if (old_rel_pages == total_pages &&
		scanned_pages < (double) total_pages * 0.02)
		return old_rel_tuples;
	if (scanned_pages <= 1)
		return old_rel_tuples;

	/*
	 * 如果旧密度未知，除将 scanned_tuples 按比例放大至 total_pages 外
	 * 我们无能为力。
	 */
	if (old_rel_tuples < 0 || old_rel_pages == 0)
		return floor((scanned_tuples / scanned_pages) * total_pages + 0.5);

	/*
	 * 好了，我们已经处理了边界情况。正常计算是将旧测量值转换为密度
	 * （每页元组数），然后使用该值估计未扫描页面中的元组数，最后加上
	 * 扫描页面中的元组数。
	 * and finally add on the number of tuples in the scanned pages.
	 */
	old_density = old_rel_tuples / old_rel_pages;
	unscanned_pages = (double) total_pages - (double) scanned_pages;
	total_tuples = old_density * unscanned_pages + scanned_tuples;
	return floor(total_tuples + 0.5);
}


/*
 *vac_update_relstats() -- 更新单个关系的统计信息
 *
 *更新保存在 pg_class 行中的全关系统计信息。如果执行 ANALYZE，
 *还会有额外的统计信息被更新，但我们总是更新这些统计信息。此例程
 *同时适用于 pg_class 中的索引和堆关系条目。
 *
 *我们在此违反事务语义，用新值覆盖关系的现有 pg_class 元组。
 *只要我们确信新值无论本事务是否提交都是正确的，这是合理安全的。
 *这样做的原因是，如果以常规方式更新这些元组，清理 pg_class 本身
 *将无法很好地工作 --- 当我们完成一个清理周期时，pg_class 中的大多
 *数元组都已变得陈旧。当然，这只适用于固定大小的非空列，而这些
 *正是固定大小的非空列。
 *
 *这样做的另一个原因是，当我们处于惰性 VACUUM 且已设置
 *PROC_IN_VACUUM 时，我们不能执行任何常规更新。有人清理 pg_class
 *时可能会认为他们可以删除标记为 xmin = 我们 xid 的元组。
 *
 *除了根本非事务性的统计信息如 relpages 和 relallvisible 之外，
 *我们还尝试维护某些惰性更新的 DDL 标志，如 relhasindex，如果不正确
 *则清除它们。在 VACUUM 中这样做是安全的，因为 VACUUM 不能与
 *CREATE INDEX/RULE/TRIGGER 并行运行，且不能是事务块的一部分。
 *但是，在处于外部事务内的 ANALYZE 中这样做是*不*安全的，例如当前
 *事务可能已删除最后一个索引；然后我们会认为应清除 relhasindex，
 *但如果事务稍后回滚，这将是错误的。因此，如果我们处于外部事务内，
 *则不更新 DDL 标志。这是可以的，因为推迟标志维护总是允许的。
 *
 *注意：num_tuples 应仅计数*存活*元组，因 pg_class.reltuples 是这样
 *定义的。
 *
 *此例程由 VACUUM 和 ANALYZE 共享。
 *		So we refrain from updating the DDL flags if we're inside an outer
 *		transaction.  This is OK since postponing the flag maintenance is
 *		always allowable.
 *
 *		Note: num_tuples should count only *live* tuples, since
 *		pg_class.reltuples is defined that way.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 */
void
vac_update_relstats(Relation relation,
					BlockNumber num_pages, double num_tuples,
					BlockNumber num_all_visible_pages,
					bool hasindex, TransactionId frozenxid,
					MultiXactId minmulti,
					bool *frozenxid_updated, bool *minmulti_updated,
					bool in_outer_xact)
{
	Oid			relid = RelationGetRelid(relation);
	Relation	rd;
	ScanKeyData key[1];
	HeapTuple	ctup;
	void	   *inplace_state;
	Form_pg_class pgcform;
	bool		dirty,
				futurexid,
				futuremxid;
	TransactionId oldfrozenxid;
	MultiXactId oldminmulti;

	rd = table_open(RelationRelationId, RowExclusiveLock);

	/* 获取元组副本以供涂写 */
	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	systable_inplace_update_begin(rd, ClassOidIndexId, true,
								  NULL, 1, key, &ctup, &inplace_state);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);
	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* 应用统计更新（如果有）到复制的元组 */

	dirty = false;
	if (pgcform->relpages != (int32) num_pages)
	{
		pgcform->relpages = (int32) num_pages;
		dirty = true;
	}
	if (pgcform->reltuples != (float4) num_tuples)
	{
		pgcform->reltuples = (float4) num_tuples;
		dirty = true;
	}
	if (pgcform->relallvisible != (int32) num_all_visible_pages)
	{
		pgcform->relallvisible = (int32) num_all_visible_pages;
		dirty = true;
	}

	/* 应用 DDL 更新，但不处于外部事务内（见上文） */

	if (!in_outer_xact)
	{
		/*
		 * 如果未发现任何索引，重置 relhasindex。
		 */
		if (pgcform->relhasindex && !hasindex)
		{
			pgcform->relhasindex = false;
			dirty = true;
		}

		/* 我们也在需要时清除 relhasrules 和 relhastriggers */
		if (pgcform->relhasrules && relation->rd_rules == NULL)
		{
			pgcform->relhasrules = false;
			dirty = true;
		}
		if (pgcform->relhastriggers && relation->trigdesc == NULL)
		{
			pgcform->relhastriggers = false;
			dirty = true;
		}
	}

	/*
	 * 更新 relfrozenxid，除非调用方传入 InvalidTransactionId 表示没有
	 * 新数据。
	 *
	 * 通常，我们不允许 relfrozenxid 倒退。但是，如果存储的 relfrozenxid
	 * 处于"将来"，最好假定它已损坏，并用表中剩余的最旧 XID 覆盖。这应
	 * 与 vac_update_datfrozenxid() 关于什么被视为"将来"的处理一致。
	 * This should match vac_update_datfrozenxid() concerning what we consider
	 * to be "in the future".
	 */
	oldfrozenxid = pgcform->relfrozenxid;
	futurexid = false;
	if (frozenxid_updated)
		*frozenxid_updated = false;
	if (TransactionIdIsNormal(frozenxid) && oldfrozenxid != frozenxid)
	{
		bool		update = false;

		if (TransactionIdPrecedes(oldfrozenxid, frozenxid))
			update = true;
		else if (TransactionIdPrecedes(ReadNextTransactionId(), oldfrozenxid))
			futurexid = update = true;

		if (update)
		{
			pgcform->relfrozenxid = frozenxid;
			dirty = true;
			if (frozenxid_updated)
				*frozenxid_updated = true;
		}
	}

	/* 对 relminmxid 同理 */
	oldminmulti = pgcform->relminmxid;
	futuremxid = false;
	if (minmulti_updated)
		*minmulti_updated = false;
	if (MultiXactIdIsValid(minmulti) && oldminmulti != minmulti)
	{
		bool		update = false;

		if (MultiXactIdPrecedes(oldminmulti, minmulti))
			update = true;
		else if (MultiXactIdPrecedes(ReadNextMultiXactId(), oldminmulti))
			futuremxid = update = true;

		if (update)
		{
			pgcform->relminmxid = minmulti;
			dirty = true;
			if (minmulti_updated)
				*minmulti_updated = true;
		}
	}

	/* 如果任何内容发生变化，写出元组。 */
	if (dirty)
		systable_inplace_update_finish(inplace_state, ctup);
	else
		systable_inplace_update_cancel(inplace_state);

	table_close(rd, RowExclusiveLock);

	if (futurexid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relfrozenxid value %u with new value %u for table \"%s\"",
								 oldfrozenxid, frozenxid,
								 RelationGetRelationName(relation))));
	if (futuremxid)
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("overwrote invalid relminmxid value %u with new value %u for table \"%s\"",
								 oldminmulti, minmulti,
								 RelationGetRelationName(relation))));
}


/*
 *vac_update_datfrozenxid() -- 更新本数据库的 pg_database.datfrozenxid
 *
 *将 pg_database 中本数据库的 datfrozenxid 条目更新为
 *pg_class.relfrozenxid 值的最小值。
 *
 *类似地，将 datminmxid 更新为 pg_class.relminmxid 值的最小值。
 *
 *如果能够推进任一 pg_database 值，也尝试截断 pg_xact 和
 *pg_multixact。
 *
 *我们在此违反事务语义，用新值覆盖数据库的现有 pg_database 元组。
 *这是合理安全的，因为新值无论本事务是否提交都是正确的。与
 *vac_update_relstats 一样，这避免了在 VACUUM 后留下死元组。
 *		safe since the new values are correct whether or not this transaction
 *		commits.  As with vac_update_relstats, this avoids leaving dead tuples
 *		behind after a VACUUM.
 */
void
vac_update_datfrozenxid(void)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	Relation	relation;
	SysScanDesc scan;
	HeapTuple	classTup;
	TransactionId newFrozenXid;
	MultiXactId newMinMulti;
	TransactionId lastSaneFrozenXid;
	MultiXactId lastSaneMinMulti;
	bool		bogus = false;
	bool		dirty = false;
	ScanKeyData key[1];
	void	   *inplace_state;

	/*
	 * 将此任务限制为每个数据库一个后端。这避免了可能使 datfrozenxid 或
	 * datminmxid 倒退的竞态条件。也避免了用早于之前
	 * vac_truncate_clog() 调用传入的 datfrozenxid 来调用
	 * ext5truncate_clog()。
	 */
	LockDatabaseFrozenIds(ExclusiveLock);

	/*
	 * 用 GetOldestNonRemovableTransactionId() 初始化 "min" 计算，
	 * 这是对尚未提交的新表 pg_class 条目最小 relfrozenxid 的合理近似；
	 * 参见 AddNewRelationTuple()。因此，以此开始计算不会产生错误的最小值。
	 * pg_class entries for new tables; see AddNewRelationTuple().  So we
	 * cannot produce a wrong minimum by starting with this.
	 */
	newFrozenXid = GetOldestNonRemovableTransactionId(NULL);

	/*
	 * 类似地，用 AddNewRelationTuple() 中用于新表 pg_class 的值初始化
	 * MultiXact "min"。
	 */
	newMinMulti = GetOldestMultiXactId();

	/*
	 * 识别我们在扫描期间能有效看到的最新 relfrozenxid 和 relminmxid 值。
	 * 这些是保守值，但尝试更精确并不值得。
	 * not really worth trying to be more exact.
	 */
	lastSaneFrozenXid = ReadNextTransactionId();
	lastSaneMinMulti = ReadNextMultiXactId();

	/*
	 * 我们必须顺序扫描 pg_class 以找到最小 Xid，因为没有索引可以帮助我们。
	 * 
	 *参见 vac_truncate_clog() 了解要防止的竞态条件。
	 * See vac_truncate_clog() for the race condition to prevent.
	 */
	relation = table_open(RelationRelationId, AccessShareLock);

	scan = systable_beginscan(relation, InvalidOid, false,
							  NULL, 0, NULL);

	while ((classTup = systable_getnext(scan)) != NULL)
	{
		volatile FormData_pg_class *classForm = (Form_pg_class) GETSTRUCT(classTup);
		TransactionId relfrozenxid = classForm->relfrozenxid;
		TransactionId relminmxid = classForm->relminmxid;

		/*
		 * 仅考虑能够持有未冻结 XID 的关系（其他类型无论如何应始终具有
		 * relfrozenxid 中的 InvalidTransactionId）。
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_TOASTVALUE)
		{
			Assert(!TransactionIdIsValid(relfrozenxid));
			Assert(!MultiXactIdIsValid(relminmxid));
			continue;
		}

		/*
		 * 某些表 AM 可能不需要每关系的 xid / multixid 水平线。因此，允许
		 * relfrozenxid 和 relminmxid 分别不设置（即设置为各自的 Invalid*Id）
		 * 似乎是合理的。因此，仅当设置时才分别验证和计算水平线。
		 * 
		 * 如果工作正常，任何关系都不应有"将来"的 relfrozenxid 或 relminmxid。
		 *但是，已知由于 pg_upgrade 中的 bug 会出现这种情况。如果我们看到
		 * 任何"将来"的条目，我们退出并不做任何操作。这确保我们不会在这些
		 * 关系被扫描和清理之前截断 clog 和 multixact SLRU。
		 * cases have been known to arise due to bugs in pg_upgrade.  If we
		 * see any entries that are "in the future", chicken out and don't do
		 * anything.  This ensures we won't truncate clog & multixact SLRUs
		 * before those relations have been scanned and cleaned up.
		 */

		if (TransactionIdIsValid(relfrozenxid))
		{
			Assert(TransactionIdIsNormal(relfrozenxid));

			/* 检查将来值 */
			if (TransactionIdPrecedes(lastSaneFrozenXid, relfrozenxid))
			{
				bogus = true;
				break;
			}

			/* 确定新水平线 */
			if (TransactionIdPrecedes(relfrozenxid, newFrozenXid))
				newFrozenXid = relfrozenxid;
		}

		if (MultiXactIdIsValid(relminmxid))
		{
			/* 检查将来值 */
			if (MultiXactIdPrecedes(lastSaneMinMulti, relminmxid))
			{
				bogus = true;
				break;
			}

			/* 确定新水平线 */
			if (MultiXactIdPrecedes(relminmxid, newMinMulti))
				newMinMulti = relminmxid;
		}
	}

	/* pg_class 扫描完成 */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	/* 如果发现伪造数据则退出 */
	if (bogus)
		return;

	Assert(TransactionIdIsNormal(newFrozenXid));
	Assert(MultiXactIdIsValid(newMinMulti));

	/* 现在获取需要更新的 pg_database 元组。 */
	relation = table_open(DatabaseRelationId, RowExclusiveLock);

	/*
	 * 获取元组副本以供涂写。我们可以先检查 syscache 元组。如果得出
	 * !dirty 的结论，我们将避免等待并发的 heap_update() 并避免对缓冲区
	 * 加排他锁。现在暂不优化此点。
	 * For now, don't optimize that.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(MyDatabaseId));

	systable_inplace_update_begin(relation, DatabaseOidIndexId, true,
								  NULL, 1, key, &tuple, &inplace_state);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);

	dbform = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * 与 vac_update_relstats() 中一样，我们通常不希望 datfrozenxid 倒退；
	 * 但如果它处于"将来"，则它一定是损坏的，最好覆盖它。
	 * corrupt and it seems best to overwrite it.
	 */
	if (dbform->datfrozenxid != newFrozenXid &&
		(TransactionIdPrecedes(dbform->datfrozenxid, newFrozenXid) ||
		 TransactionIdPrecedes(lastSaneFrozenXid, dbform->datfrozenxid)))
	{
		dbform->datfrozenxid = newFrozenXid;
		dirty = true;
	}
	else
		newFrozenXid = dbform->datfrozenxid;

	/* 对 datminmxid 同理 */
	if (dbform->datminmxid != newMinMulti &&
		(MultiXactIdPrecedes(dbform->datminmxid, newMinMulti) ||
		 MultiXactIdPrecedes(lastSaneMinMulti, dbform->datminmxid)))
	{
		dbform->datminmxid = newMinMulti;
		dirty = true;
	}
	else
		newMinMulti = dbform->datminmxid;

	if (dirty)
		systable_inplace_update_finish(inplace_state, tuple);
	else
		systable_inplace_update_cancel(inplace_state);

	heap_freetuple(tuple);
	table_close(relation, RowExclusiveLock);

	/*
	 * 如果能够推进 datfrozenxid 或 datminmxid，查看是否可以截断 pg_xact
	 * 和/或 pg_multixact。如果共享 XID 回卷限制信息已陈旧，也执行此操作，
	 * 因为此操作也会更新它。
	 */
	if (dirty || ForceTransactionIdLimitUpdate())
		vac_truncate_clog(newFrozenXid, newMinMulti,
						  lastSaneFrozenXid, lastSaneMinMulti);
}


/*
 *vac_truncate_clog() -- 尝试截断提交日志
 *
 *扫描 pg_database 以确定系统范围内最旧的 datfrozenxid，并用它截断
 *事务提交日志（pg_xact）。同时更新由 varsup.c 维护的 XID 回卷限制
 *信息。对 datminmxid 同理。
 *
 *传入的 frozenXID 和 minMulti 是调用方自己 pg_database 条目的更新值。
 *它们用于初始化 "min" 计算。调用方还传入 "last sane" XID 和 MXID，
 *因为调用方已有这些值。
 *
 *此例程仅在已成功修改本数据库的 datfrozenxid / datminmxid 值或发现
 *共享 XID 回卷限制信息已陈旧时调用。
 *		This routine is only invoked when we've managed to change our
 *		DB's datfrozenxid/datminmxid values, or we found that the shared
 *		XID-wrap-limit info is stale.
 */
static void
vac_truncate_clog(TransactionId frozenXID,
				  MultiXactId minMulti,
				  TransactionId lastSaneFrozenXid,
				  MultiXactId lastSaneMinMulti)
{
	TransactionId nextXID = ReadNextTransactionId();
	Relation	relation;
	TableScanDesc scan;
	HeapTuple	tuple;
	Oid			oldestxid_datoid;
	Oid			minmulti_datoid;
	bool		bogus = false;
	bool		frozenAlreadyWrapped = false;

	/* 将任务限制为每个数据库集簇一个后端；参见 SimpleLruTruncate()。 */
	LWLockAcquire(WrapLimitsVacuumLock, LW_EXCLUSIVE);

	/* 初始化最旧的 datoid 以与我的 frozenXID / minMulti 值同步 */
	oldestxid_datoid = MyDatabaseId;
	minmulti_datoid = MyDatabaseId;

	/*
	 * 扫描 pg_database 以计算最小 datfrozenxid / datminmxid
	 *
	 * 由于 vac_update_datfrozenxid 会就地更新 datfrozenxid / datminmxid，
	 * 值可能在我们查看时改变。获取每个值一次以确保比较逻辑的健全行为。
	 * （在此处及许多其他地方，我们假设获取或更新共享存储中的 XID 是
	 * 原子的。）
	 * 
	 *注意：无需担心 CREATE DATABASE 插入新条目的竞态条件。任何此类
	 * 条目都将拥有某个现有数据库 datfrozenxid 的副本，而该源数据库由于
	 * 不能复制包含活动后端的数据库的互锁机制，不可能是我们自己的。因此
	 * 新条目不会降低最小值。此外，如果两个 VACUUM 并发修改不同数据库的
	 * datfrozenxid，最坏的结果是 pg_xact 未被尽可能积极截断。
	 * Hence the new entry will not reduce the minimum.  Also, if two VACUUMs
	 * concurrently modify the datfrozenxid's of different databases, the
	 * worst possible outcome is that pg_xact is not truncated as aggressively
	 * as it could be.
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);

	scan = table_beginscan_catalog(relation, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		volatile FormData_pg_database *dbform = (Form_pg_database) GETSTRUCT(tuple);
		TransactionId datfrozenxid = dbform->datfrozenxid;
		TransactionId datminmxid = dbform->datminmxid;

		Assert(TransactionIdIsNormal(datfrozenxid));
		Assert(MultiXactIdIsValid(datminmxid));

		/*
		 * 如果数据库正在被删除过程中，或已被中断，则无法再连接到它。因此
		 * 无需在此考虑它。这很好，因为它也无法被自动清理处理。
		 * anymore. Therefore we don't need to take it into account here.
		 * Which is good, because it can't be processed by autovacuum either.
		 */
		if (database_is_invalid_form((Form_pg_database) dbform))
		{
			elog(DEBUG2,
				 "skipping invalid database \"%s\" while computing relfrozenxid",
				 NameStr(dbform->datname));
			continue;
		}

		/*
		 * 如果工作正常，任何数据库都不应有"将来"的 datfrozenxid 或
		 * datminmxid。但是，已知由于 pg_upgrade 中的 bug 会出现这种情况。
		 * 如果我们看到任何"将来"的条目，我们退出并不做任何操作。这确保我们
		 * 不会在这些数据库被扫描和清理之前截断 clog。（但如果合适，我们仍会
		 * 发出 "already wrapped" 警告。）
		 * databases have been scanned and cleaned up.  (We will issue the
		 * "already wrapped" warning if appropriate, though.)
		 */
		if (TransactionIdPrecedes(lastSaneFrozenXid, datfrozenxid) ||
			MultiXactIdPrecedes(lastSaneMinMulti, datminmxid))
			bogus = true;

		if (TransactionIdPrecedes(nextXID, datfrozenxid))
			frozenAlreadyWrapped = true;
		else if (TransactionIdPrecedes(datfrozenxid, frozenXID))
		{
			frozenXID = datfrozenxid;
			oldestxid_datoid = dbform->oid;
		}

		if (MultiXactIdPrecedes(datminmxid, minMulti))
		{
			minMulti = datminmxid;
			minmulti_datoid = dbform->oid;
		}
	}

	table_endscan(scan);

	table_close(relation, AccessShareLock);

	/*
	 * 如果我们似乎已遭遇回卷，则不截断 CLOG；计算出的最小 XID 可能是伪造
	 * 的。由于 GetNewTransactionId 中的防御措施，这种情况现在应不可能，
	 * 但我们仍保留此测试。
	 * test anyway.
	 */
	if (frozenAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 2 billion transactions"),
				 errdetail("You might have already suffered transaction-wraparound data loss.")));
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/* 如果数据以任何其他方式为伪造，则退出 */
	if (bogus)
	{
		LWLockRelease(WrapLimitsVacuumLock);
		return;
	}

	/*
	 * 在截断之前推进提交时间戳的最旧值，这样如果用户在此之后立即请求
	 * 我们正在截断的事务的时间戳，他们将得到 NULL，而不是 slru.c 发出的
	 * 难看的 "file not found" 错误。这对 xact / multixact 不重要，因为
	 * 它们不会受到来自用户的任意查找。
	 * because they are not subject to arbitrary lookups from users.
	 */
	AdvanceOldestCommitTsXid(frozenXID);

	/*
	 * 将 CLOG、multixact 和 CommitTs 截断到最旧的计算值。
	 */
	TruncateCLOG(frozenXID, oldestxid_datoid);
	TruncateCommitTs(frozenXID);
	TruncateMultiXact(minMulti, minmulti_datoid);

	/*
	 * 更新 GetNewTransactionId 的包装限制和新 MultiXactId 的创建限制。
	 * 注意：这些函数也会在需要时向 postmaster 发送信号以进行另一个
	 * 自动清理周期。XXX 我们是否应避免可能发出两次信号？
	 * signaling twice?
	 */
	SetTransactionIdLimit(frozenXID, oldestxid_datoid);
	SetMultiXactIdLimit(minMulti, minmulti_datoid, false);

	LWLockRelease(WrapLimitsVacuumLock);
}


/*
 *vacuum_rel() -- 清理一个堆关系
 *
 *relid 标识要清理的关系。如果提供了 relation，则使用其中的名称报告
 *打开/锁定关系的任何失败；一旦成功打开关系，不要使用它，因为它可能
 *已过时。
 *
 *如果对此表执行请求的 ANALYZE 操作是安全的，则返回 true。
 *
 *一次处理一个堆会带来额外的开销，因为我们需要在清理之前再次确认
 *堆存在。我们这样做的原因是为了使清理可以分布在许多小事务中。否则，
 *两阶段锁定将要求我们在清理器的一次遍历期间锁定整个数据库。
 *
 *在进入和退出时，我们不处于事务内部。
 *		many small transactions.  Otherwise, two-phase locking would require
 *		us to lock the entire database during one pass of the vacuum cleaner.
 *
 *		At entry and exit, we are not inside a transaction.
 */
static bool
vacuum_rel(Oid relid, RangeVar *relation, VacuumParams *params,
		   BufferAccessStrategy bstrategy)
{
	LOCKMODE	lmode;
	Relation	rel;
	LockRelId	lockrelid;
	Oid			toast_relid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	VacuumParams toast_vacuum_params;

	Assert(params != NULL);

	/*
	 * 此函数会在参数上涂写，因此尽早制作副本以避免影响 TOAST 表
	 * （如果我们最终确实递归到它）。
	 */
	memcpy(&toast_vacuum_params, params, sizeof(VacuumParams));

	/* 为清理此关系开始一个事务 */
	StartTransactionCommand();

	if (!(params->options & VACOPT_FULL))
	{
		/*
		 * 在惰性清理中，我们可以设置 PROC_IN_VACUUM 标志，让其他并发的
		 * VACUUM 知道在确定其 OldestXmin 时可以忽略此操作。（在完整 VACUUM
		 * 期间不设置此标志的原因正是：对于功能性索引，我们可能需要运行
		 * 用户定义的函数，我们希望确保如果它们使用上面设置的快照，它所需
		 * 的任何元组都不会从其他表中被移除。依赖于其他表内容的索引函数可能
		 * 存在问题，但我们不会在此违反事务语义来破坏它。）
		 * 
		 * 我们也设置 VACUUM_FOR_WRAPAROUND 标志，该标志由自动清理向下传递；
		 * 用于避免取消在紧急情况下调用的清理。
		 *
		 * 注意：这些标志一直保持到 CommitTransaction 或 AbortTransaction。
		 * 我们希望在重置 MyProc->xid/xmin 之前不清除它们，否则
		 * GetOldestNonRemovableTransactionId() 可能看起来倒退，这可能是
		 *Not Good。（我们还在获取自己的快照*之前*设置 PROC_IN_VACUUM，
		 * 以便我们的 xmin 不会在设置标志之前变得可见。）
		 * AbortTransaction.  We don't want to clear them until we reset
		 * MyProc->xid/xmin, otherwise GetOldestNonRemovableTransactionId()
		 * might appear to go backwards, which is probably Not Good.  (We also
		 * set PROC_IN_VACUUM *before* taking our own snapshot, so that our
		 * xmin doesn't become visible ahead of setting the flag.)
		 */
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_VACUUM;
		if (params->is_wraparound)
			MyProc->statusFlags |= PROC_VACUUM_FOR_WRAPAROUND;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/*
	 * 需要获取快照以防止 pg_subtrans 被截断、本地内存中截断的 xid 回卷，
	 * 并确保 xmin 水平线已更新。
	 * horizons.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * 检查用户请求的中止。注意我们希望这在事务内部进行，以免 xact.c 发出
	 * 无用的 WARNING。
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 确定我们想要的锁类型 --- 完整清理需要硬排他锁，但并发清理仅需
	 * ShareUpdateExclusiveLock。无论哪种方式，我们可以确保没有其他后端在
	 * 清理同一张表。
	 */
	lmode = (params->options & VACOPT_FULL) ?
		AccessExclusiveLock : ShareUpdateExclusiveLock;

	/* 打开关系并获取适当的锁 */
	rel = vacuum_open_relation(relid, relation, params->options,
							   params->log_min_duration >= 0, lmode);

	/* 如果无法打开或锁定关系，则离开 */
	if (!rel)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * 检查是否需要根据所有权跳过该关系。此检查在手动操作构建要清理的
	 * 关系列表时也会发生，并且需要在此额外进行，因为 VACUUM 可能跨越
	 * 多个事务，在期间关系所有权可能已更改。确保仅在这种情况下为 VACUUM
	 * 生成日志。
	 * changed in-between.  Make sure to only generate logs for VACUUM in this
	 * case.
	 */
	if (!vacuum_is_relation_owner(RelationGetRelid(rel),
								  rel->rd_rel,
								  params->options & VACOPT_VACUUM))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * 检查它是否为可清理的 relkind。
	 */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum non-tables or special system tables",
						RelationGetRelationName(rel))));
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * 静默忽略属于其他后端的临时表 --- 尝试清理这些表将导致重大不愉快，
	 * 因为它们的内容可能不会在磁盘上保持最新。（我们不在此处发出警告；
	 * 那样只会在数据库范围的 VACUUM 期间带来噪声。）
	 * warning here; it would just lead to chatter during a database-wide
	 * VACUUM.)
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * 静默忽略分区表，因为没有工作要做。有用的工作在其子分区上，它们已
	 * 被单独排队。
	 * us separately.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		/* 可以继续对此表执行 ANALYZE */
		return true;
	}

	/*
	 * 同时获取会话级锁。这将保护我们对关系的跨多个事务的访问，以便我们
	 * 可以清理关系的 TOAST 表（如果有），安全地知道没有人在删除父关系。
	 * 
	 * 注意：这不能阻塞，即使其他人在等待访问，因为锁管理器知道两个锁
	 *请求来自同一进程。
	 * NOTE: this cannot block, even if someone else is waiting for access,
	 * because the lock manager knows that both lock requests are from the
	 * same process.
	 */
	lockrelid = rel->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&lockrelid, lmode);

	/*
	 * 如果 VACUUM 命令中未指定 index_cleanup 选项，或在自动清理工作进程
	 * 中运行时，根据 index_cleanup 关系选项设置 index_cleanup 选项。
	 */
	if (params->index_cleanup == VACOPTVALUE_UNSPECIFIED)
	{
		StdRdOptIndexCleanup vacuum_index_cleanup;

		if (rel->rd_options == NULL)
			vacuum_index_cleanup = STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO;
		else
			vacuum_index_cleanup =
				((StdRdOptions *) rel->rd_options)->vacuum_index_cleanup;

		if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO)
			params->index_cleanup = VACOPTVALUE_AUTO;
		else if (vacuum_index_cleanup == STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON)
			params->index_cleanup = VACOPTVALUE_ENABLED;
		else
		{
			Assert(vacuum_index_cleanup ==
				   STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF);
			params->index_cleanup = VACOPTVALUE_DISABLED;
		}
	}

	/*
	 * 如果 VACUUM 命令中未指定 truncate 选项，或在自动清理工作进程中运行
	 * 时，根据 truncate 关系选项设置 truncate 选项。
	 */
	if (params->truncate == VACOPTVALUE_UNSPECIFIED)
	{
		if (rel->rd_options == NULL ||
			((StdRdOptions *) rel->rd_options)->vacuum_truncate)
			params->truncate = VACOPTVALUE_ENABLED;
		else
			params->truncate = VACOPTVALUE_DISABLED;
	}

	/*
	 * 如果调用方要求处理 TOAST 表，则记住关系的 TOAST 关系以备后用。
	 * 但在 VACUUM FULL 中，TOAST 表由 cluster_rel 自动重建，因此我们
	 * 不应递归到它，除非 PROCESS_MAIN 被禁用。
	 * unless PROCESS_MAIN is disabled.
	 */
	if ((params->options & VACOPT_PROCESS_TOAST) != 0 &&
		((params->options & VACOPT_FULL) == 0 ||
		 (params->options & VACOPT_PROCESS_MAIN) == 0))
		toast_relid = rel->rd_rel->reltoastrelid;
	else
		toast_relid = InvalidOid;

	/*
	 * 切换到表所有者的 userid，以便任何索引函数都以该用户身份运行。同时
	 * 锁定安全受限操作，并安排使 GUC 变量更改仅发生于此命令的本地。
	 * （这对于惰性 VACUUM 是不必要的，但无害。）
	 * unnecessary, but harmless, for lazy VACUUM.)
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/*
	 * 如果 PROCESS_MAIN 已设置（默认值），则是清理主关系的时候了。否则，
	 * 我们可以跳过此部分。如果需要处理 TOAST 表（例如 PROCESS_TOAST 已设
	 * 置），则在递归到 TOAST 表时强制设置 PROCESS_MAIN。
	 * to be set when we recurse to the TOAST table.
	 */
	if (params->options & VACOPT_PROCESS_MAIN)
	{
		/*
		 * 执行实际工作 --- FULL 或 "lazy" 清理
		 */
		if (params->options & VACOPT_FULL)
		{
			ClusterParams cluster_params = {0};

			/* 在清理之前关闭关系，但保持锁直到提交 */
			relation_close(rel, NoLock);
			rel = NULL;

			if ((params->options & VACOPT_VERBOSE) != 0)
				cluster_params.options |= CLUOPT_VERBOSE;

			/* VACUUM FULL 现在是 CLUSTER 的变体；参见 cluster.c */
			cluster_rel(relid, InvalidOid, &cluster_params);
		}
		else
			table_relation_vacuum(rel, params, bstrategy);
	}

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 此关系处理完毕，但保持锁直到提交 */
	if (rel)
		relation_close(rel, NoLock);

	/*
	 * 完成事务并释放所有使用的临时内存。
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * 如果关系有辅助 TOAST 关系，同样在持有主表的会话锁期间清理它。
	 * 但注意，不会对 TOAST 表执行 "analyze"。这很好，因为 TOAST 处理
	 * 始终使用硬编码的索引访问，统计信息对 TOAST 关系完全不重要。
	 * the toaster always uses hardcoded index access and statistics are
	 * totally unimportant for toast relations.
	 */
	if (toast_relid != InvalidOid)
	{
		/* 强制 VACOPT_PROCESS_MAIN 使 vacuum_rel() 处理它 */
		toast_vacuum_params.options |= VACOPT_PROCESS_MAIN;

		vacuum_rel(toast_relid, NULL, &toast_vacuum_params, bstrategy);
	}

	/*
	 * 现在释放主表上的会话级锁。
	 */
	UnlockRelationIdForSession(&lockrelid, lmode);

	/* 报告我们确实完成了。 */
	return true;
}


/*
 * 打开给定关系的所有可清理索引，对每个索引获取指定类型的锁。将索引
 * 的 Relation 指针数组返回到 *Irel，索引数量返回到 *nindexes。
 * 
 *如果索引被标记为可插入（indisready），我们将其视为可清理的。如果
 * 不是，可能是 CREATE INDEX CONCURRENTLY 命令在执行早期失败，我们
 * 拥有的内容过于损坏无法处理。即使索引不是 indisvalid，我们也会清理；
 * 这很重要，因为在唯一索引中，唯一性检查仍会被执行，最好不要命中
 * 悬垂的索引指针。
 * unique index, uniqueness checks will be performed anyway and had better not
 * hit dangling index pointers.
 */
void
vac_open_indexes(Relation relation, LOCKMODE lockmode,
				 int *nindexes, Relation **Irel)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i;

	Assert(lockmode != NoLock);

	indexoidlist = RelationGetIndexList(relation);

	/* 为所有索引分配足够的内存 */
	i = list_length(indexoidlist);

	if (i > 0)
		*Irel = (Relation *) palloc(i * sizeof(Relation));
	else
		*Irel = NULL;

	/* 仅收集就绪的索引 */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indrel;

		indrel = index_open(indexoid, lockmode);
		if (indrel->rd_index->indisready)
			(*Irel)[i++] = indrel;
		else
			index_close(indrel, lockmode);
	}

	*nindexes = i;

	list_free(indexoidlist);
}

/*
 * 释放 vac_open_indexes 获取的资源。可选择释放锁（传 NoLock 表示
 * 保持锁）。
 */
void
vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode)
{
	if (Irel == NULL)
		return;

	while (nindexes--)
	{
		Relation	ind = Irel[nindexes];

		index_close(ind, lockmode);
	}
	pfree(Irel);
}

/*
 * vacuum_delay_point --- 检查中断和基于成本的延迟。
 *
 * 应在 VACUUM 处理的每个主循环中调用，通常是每处理一页调用一次。
 * typically once per page processed.
 */
void
vacuum_delay_point(void)
{
	double		msec = 0;

	/* 始终检查中断 */
	CHECK_FOR_INTERRUPTS();

	if (InterruptPending ||
		(!VacuumCostActive && !ConfigReloadPending))
		return;

	/*
	 * 自动清理工作进程应在请求时重新加载配置文件。这允许在表被清理或
	 * 分析时使 [autovacuum_]vacuum_cost_limit 和
	 * [autovacuum_]vacuum_cost_delay 的更改生效。
	 * vacuumed or analyzed.
	 */
	if (ConfigReloadPending && IsAutoVacuumWorkerProcess())
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
		VacuumUpdateCosts();
	}

	/*
	 * 如果在重新加载配置文件后禁用了基于成本的延迟，则返回。
	 * return.
	 */
	if (!VacuumCostActive)
		return;

	/*
	 * 对于并行清理，延迟基于共享成本余额计算。参见
	 * compute_parallel_delay。
	 */
	if (VacuumSharedCostBalance != NULL)
		msec = compute_parallel_delay();
	else if (VacuumCostBalance >= vacuum_cost_limit)
		msec = vacuum_cost_delay * VacuumCostBalance / vacuum_cost_limit;

	/* 如果适当则小睡 */
	if (msec > 0)
	{
		if (msec > vacuum_cost_delay * 4)
			msec = vacuum_cost_delay * 4;

		pgstat_report_wait_start(WAIT_EVENT_VACUUM_DELAY);
		pg_usleep(msec * 1000);
		pgstat_report_wait_end();

		/*
		 * 我们不想在配置了 vacuum_cost_delay 的非常漫长的清理期间忽略
		 * postmaster 死亡。我们不能在此处使用通常的 WaitLatch() 方法，
		 * 因为我们需要基于微秒的睡眠时长。
		 * durations above.
		 */
		if (IsUnderPostmaster && !PostmasterIsAlive())
			exit(1);

		VacuumCostBalance = 0;

		/*
		 * 平衡并更新自动清理工作进程的 limit 值。我们必须定期执行此操作，
		 * 因为工作进程的数量可能已更改。
		 * 
		 *TODO: 理想情况下，我们应该在中间重新读取工作进程数量，但暂时
		 * 跳过。
		 * besides "check after napping".
		 */
		AutoVacuumUpdateCostLimit();

		/* 睡眠期间可能收到中断 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * 计算并行工作进程的清理延迟。
 *
 * 基于成本的并行清理延迟的基本思想是让每个工作进程按其完成工作的
 * 比例进行睡眠。这通过使用共享成本余额来实现，工作进程在其中累积
 * 其成本。当共享余额超过 vacuum_cost_limit 时，工作进程会进行睡眠。
 * 睡眠后，共享余额对应于超出 vacuum_cost_limit 的成本量进行递减。
 * 
 * 应通过 VacuumSharedCostBalance 和 VacuumActiveNWorkers 从并行
 * 清理工作进程调用此函数。
 * (VacuumCostBalanceLocal) and then reduce the VacuumSharedCostBalance by
 * that amount.  This avoids putting to sleep those workers which have done less
 * I/O than other workers and therefore ensure that workers
 * which are doing more I/O got throttled more.
 *
 * We allow a worker to sleep only if it has performed I/O above a certain
 * threshold, which is calculated based on the number of active workers
 * (VacuumActiveNWorkers), and the overall cost balance is more than
 * VacuumCostLimit set by the system.  Testing reveals that we achieve
 * the required throttling if we force a worker that has done more than 50%
 * of its share of work to sleep.
 */
static double
compute_parallel_delay(void)
{
	double		msec = 0;
	uint32		shared_balance;
	int			nworkers;

	/* 并行清理必须处于活动状态 */
	Assert(VacuumSharedCostBalance);

	nworkers = pg_atomic_read_u32(VacuumActiveNWorkers);

	/* 至少计数自身 */
	Assert(nworkers >= 1);

	/* 原子更新共享成本余额值 */
	shared_balance = pg_atomic_add_fetch_u32(VacuumSharedCostBalance, VacuumCostBalance);

	/* 计算当前工作进程的本地总余额 */
	VacuumCostBalanceLocal += VacuumCostBalance;

	if ((shared_balance >= vacuum_cost_limit) &&
		(VacuumCostBalanceLocal > 0.5 * ((double) vacuum_cost_limit / nworkers)))
	{
		/* 根据本地成本余额计算睡眠时间 */
		msec = vacuum_cost_delay * VacuumCostBalanceLocal / vacuum_cost_limit;
		pg_atomic_sub_fetch_u32(VacuumSharedCostBalance, VacuumCostBalanceLocal);
		VacuumCostBalanceLocal = 0;
	}

	/*
	 * 重置本地余额，因为已将其累积到共享值中。
	 */
	VacuumCostBalance = 0;

	return msec;
}

/*
 * defGetBoolean() 的包装函数。
 *
 * 此函数返回 VACOPTVALUE_ENABLED 和 VACOPTVALUE_DISABLED
 * 而非 true 和 false。
 */
static VacOptValue
get_vacoptval_from_boolean(DefElem *def)
{
	return defGetBoolean(def) ? VACOPTVALUE_ENABLED : VACOPTVALUE_DISABLED;
}

/*
 *vac_bulkdel_one_index() -- 索引关系的批量删除。
 *
 * 返回从输入统计信息派生的批量删除统计信息
 */
IndexBulkDeleteResult *
vac_bulkdel_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat,
					  VacDeadItems *dead_items)
{
	/* 执行批量删除 */
	istat = index_bulk_delete(ivinfo, istat, vac_tid_reaped,
							  (void *) dead_items);

	ereport(ivinfo->message_level,
			(errmsg("scanned index \"%s\" to remove %d row versions",
					RelationGetRelationName(ivinfo->index),
					dead_items->num_items)));

	return istat;
}

/*
 *vac_cleanup_one_index() -- 对索引关系执行清理后的清除。
 *
 * 返回从输入统计信息派生的批量删除统计信息
 */
IndexBulkDeleteResult *
vac_cleanup_one_index(IndexVacuumInfo *ivinfo, IndexBulkDeleteResult *istat)
{
	istat = index_vacuum_cleanup(ivinfo, istat);

	if (istat)
		ereport(ivinfo->message_level,
				(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
						RelationGetRelationName(ivinfo->index),
						istat->num_index_tuples,
						istat->num_pages),
				 errdetail("%.0f index row versions were removed.\n"
						   "%u index pages were newly deleted.\n"
						   "%u index pages are currently deleted, of which %u are currently reusable.",
						   istat->tuples_removed,
						   istat->pages_newly_deleted,
						   istat->pages_deleted, istat->pages_free)));

	return istat;
}

/*
 * 返回给定 max_items 值的 VACUUM dead_items 数组所需的总空间。
 * max_items value.
 */
Size
vac_max_items_to_alloc_size(int max_items)
{
	Assert(max_items <= MAXDEADITEMS(MaxAllocSize));

	return offsetof(VacDeadItems, items) + sizeof(ItemPointerData) * max_items;
}

/*
 *vac_tid_reaped() -- 特定 tid 是否可删除？
 *
 *具有作为 IndexBulkDeleteCallback 的正确签名。
 *
 *假定 dead_items 数组已排序（按 TID 升序）。
 */
static bool
vac_tid_reaped(ItemPointer itemptr, void *state)
{
	VacDeadItems *dead_items = (VacDeadItems *) state;
	int64		litem,
				ritem,
				item;
	ItemPointer res;

	litem = itemptr_encode(&dead_items->items[0]);
	ritem = itemptr_encode(&dead_items->items[dead_items->num_items - 1]);
	item = itemptr_encode(itemptr);

	/*
	 * 在 bsearch() 之前执行简单的范围检查有助于避免 bsearch() 的额外开销，
	 * 特别是当堆上的死元组集中在某个范围内时。由于此函数对每个活动元组
	 * 都会被调用，此类优化是值得的。此外，如果找不到，我们应返回 false，
	 * 而非段错误。
	 */
	if (item < litem || item > ritem)
		return false;

	res = (ItemPointer) bsearch(itemptr,
								dead_items->items,
								dead_items->num_items,
								sizeof(ItemPointerData),
								vac_cmp_itemptr);

	return (res != NULL);
}

/*
 * 用于 qsort() 和 bsearch() 的比较例程。
 */
static int
vac_cmp_itemptr(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;
	OffsetNumber loff,
				roff;

	lblk = ItemPointerGetBlockNumber((ItemPointer) left);
	rblk = ItemPointerGetBlockNumber((ItemPointer) right);

	if (lblk < rblk)
		return -1;
	if (lblk > rblk)
		return 1;

	loff = ItemPointerGetOffsetNumber((ItemPointer) left);
	roff = ItemPointerGetOffsetNumber((ItemPointer) right);

	if (loff < roff)
		return -1;
	if (loff > roff)
		return 1;

	return 0;
}
