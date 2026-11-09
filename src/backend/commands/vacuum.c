/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  postgres 清理工具。    
 *
 * 本文件包括 (a) VACUUM 和 ANALYZE 命令的控制与分发        
 * 代码、(b) 计算各种清理阈值的代码，及 (c) 索引清理 
 * 代码。   
 *
 * 堆 AM 的 VACUUM 在 vacuumlazy.c 中实现，并行清理在       
 * vacuumparallel.c，ANALYZE 在 analyze.c，VACUUM FULL 则是 CLUSTER  
 * 的变体，见 cluster.c。  
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
 * VacuumFailsafeActive 被定义为全局变量，以便我们能确定 
 * 清理表时是否重新启用基于成本的清理延迟。               
 * 如果已启用故障安全模式，则在该表的清理完成之前， 
 * 无论其他设置如何，该表都不会重新启用基于成本的 
 * 延迟。
 *
 * 只有 VACUUM 代码应检查此变量，且只有表访问方法应       
 * 将其设为 true。在与表 AM 无关的 VACUUM 代码中，此变量  
 * 用于确定是否允许基于成本的延迟。表 AM 可自由设置     
 * 它来如愿实现此行为，但默认值为 false。                     
 * 并在各关系清理之间重置为 false。         
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
 * 手动 VACUUM 和 ANALYZE 命令的主要入口点。     
 *
 * 这主要是一个准备性包装器，实际操作将由             
 * vacuum() 完成。 
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
			 * 检查指定值是否有效，若非 0 则大小须处于 
			 * 硬性上下限范围内。                           
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
 * relations 若非 NIL，就是要处理的 VacuumRelation 列表；否则 
 * 将处理数据库中所有相关的表。对于每个 VacuumRelation， 
 * 如果提供了有效的 OID，则处理具有该 OID 的表；        
 * 否则由 VacuumRelation 的 RangeVar 指示要处理什么。     
 *
 * params 包含一组可用于自定义行为的                       
 * 参数。
 *
 * bstrategy 可以在调用方不想限制 VACUUM / ANALYZE          
 * 可使用的 shared_buffers 数量时传入 NULL，否则调用方  
 * 必须构建一个 BufferAccessStrategy，其中包含 VACUUM             
 * / ANALYZE 应尝试限制自身使用的 shared_buffers 数量。     
 *       
 *
 * isTopLevel 应从 ProcessUtility 向下传递。     
 *
 * 调用方需确保所有参数分配在事务提交后不会消失的   
 * 内存上下文中。                                        
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
	 * 无法在用户事务块内运行 VACUUM；若在事务内，我们的 
	 * 提交与启动事务命令的调用就不会有预期效果！    
	 * 此外，对此还存在许多其他微妙的                      
	 * 依赖关系。           
	 *
	 * ANALYZE（不带 VACUUM）则两者皆可。
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
	 * 决定我们是否需要启动/提交自己的事务。     
	 *
	 * 对于 VACUUM（带或不带 ANALYZE）：总是这么做，以便
	 * 尽早释放锁。（单表 VACUUM 或可用外层事务，但      
	 * 处理 TOAST 表就会有问题。                                    
	 * ）          
	 *
	 * 对于 ANALYZE（无 VACUUM）：若处于事务块内，无法  
	 * 启动/提交自己的事务。若只处理一个关系，则无需  
	 * 这样做。处理多个关系且不在事务块内时，以及        
	 * 自动清理 worker 中，均用自身事务，以便更    
	 * 早地释放锁。                          
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
	 * vacuum_rel 预期进入时无活动的事务；它将自行         
	 * 启动并提交自身的事务。但因 SQL 命令调用，已在 
	 * 事务内执行。我们在此提交 PostgresMain() 中启动的 
	 * 事务，并在退出前启动另一个事务，以配合       
	 * PostgresMain() 中等待我们的那次提交                        
	 * 。            
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
				 * vacuum_rel() 会涂改参数，故传入一份副本以免 
				 * 影响其他关系。              
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
 * 检查给定关系能否安全清理或分析。如果用户不是该
 * 关系的所有者，发出 WARNING 日志消息并返回 false，    
 * 让调用方决定如何处置。本例程判定某关系能否作 
 * VACUUM 或 ANALYZE 处理                                             
 * 。     
 */
bool
vacuum_is_relation_owner(Oid relid, Form_pg_class reltuple, bits32 options)
{
	char	   *relname;

	Assert((options & (VACOPT_VACUUM | VACOPT_ANALYZE)) != 0);

	/*
	 * 检查权限。   
	 *
	 * 若用户为超级用户、表所有者或数据库所有者，允许 
	 * 清理或分析表（但最后一种情况仅限非共享关系    
	 * 时）。object_ownercheck 已涵盖超级用户情况                
	 * 。  
	 *
	 * 注意，我们选择将权限失败按 WARNING 处理并继续  
	 * 清理或分析数据库的其余部分 —— 这样做合适吗？   
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
 * 本例程用于尝试打开并锁定将要清理或分析的关系   
 * 。若无法打开或锁定该关系，则在可能时发出日志    
 * 。                                     
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
	 * 关系无法打开，故若可能则生成日志说明情况  
	 * 。                        
	 *
	 * 如果未定义 RangeVar，则没有足够的信息可提供        
	 * 有意义的日志语句。多半是调用方有意不提供信息  
	 * ，从而使这条日志被跳过，反正                         
	 * 也一样。    
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
 * 给定一个 VacuumRelation，若未指定则填入表 OID，        
 * 并可选择为该表的各个分区添加 VacuumRelation。   
 *
 * 如果一个 VacuumRelation 未提供 OID 且是分区表，则将    
 * 为每个分区在输出中添加额外条目。目前只有自动   
 * 清理在调用 vacuum() 时提供 OID，且不希望我们展开  
 * 分区表。                                     
 *
 * 我们注意不修改输入的数据结构，而是构建新的       
 * VacuumRelation 返回。（但注意它们将引用输入未修     
 * 改的部分，如列列表。）新数据结构在 vac_context 中  
 * 创建。               
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
		 * 我们暂时取得 AccessShareLock 保护下面的 syscache 查   
		 * 找，也满足 find_all_inheritors 对调用方持有起始关    
		 * 系上某种锁的期望。              
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
		 * 要检查关系是否为分区表及其所有权，需获取
		 * syscache 条目。                  
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
		 * 如果是，就为其分区建立关系列表条目。注意       
		 * find_all_inheritors() 返回的列表含传入 OID，故我      
		 * 们必须跳过它。目前无需对单个分区取得锁，      
		 * 这样做只会徒增不必要的死锁风险。          
		 * 出于后一原因，我们暂不检查分区所有权，它们 
		 * 会被加入待处理列表；反正所有权稍后                
		 * 还会被检查。                                  
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
				 * 我们省去 RangeVar，因为稍后报错说打开   
				 * 这些关系之一失败并不合适                 
				 * 。   
				 */
				oldcontext = MemoryContextSwitchTo(vac_context);
				vacrels = lappend(vacrels, makeVacuumRelation(NULL,
															  part_oid,
															  vrel->va_cols));
				MemoryContextSwitchTo(oldcontext);
			}
		}

		/*
		 * 再次释放锁。这意味着，等真正尝试处理该表时， 
		 * 它可能已消失或已被改名。前者我们将静默忽略； 
		 * 后者仍会处理它，但必须当心 RangeVar 已未必   
		 * 仍能标识它。这或许不理想，但几乎没什么可行 
		 * 的替代方案，因为我们通常会在此时与彼时之间    
		 * 提交该事务并再开一个新事务。此外，              
		 * 在多个关系上持有锁还会带来相当大的             
		 * 死锁风险                                                     
		 * 。         
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
		 * 我们在此也包含分区表；调用方会根据要执行的是 
		 * 哪种操作，决定处理还是忽略它们。                
		 * 。  
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* 检查关系的权限 */
		if (!vacuum_is_relation_owner(relid, classForm, options))
			continue;

		/*
		 * 构建 VacuumRelation，指定要处理的表 OID。我们省去  
		 * RangeVar，因为稍后报错说打开某个关系失败并    
		 * 不合适。                                       
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
	 * 获取OldestXmin。
	 *
	 * 我们总可忽略运行惰性清理的进程。这是因为我们     
	 * 仅用这些值决定哪些元组必须留在表中。惰性清理 
	 * 不会在任何地方写其 XID（通常未分配 XID），故忽略 
	 * 它是安全的。理论上 VACUUM FULL 中忽略惰性清   
	 * 理可能有问题，但记住：任一时刻只能有一个清理   
	 * 进程在处理某一特定表，且每次清理总是一个独立  
	 * 事务。                                                           
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
			 * TODO: 仅当依据提高后的界限剪枝时才应设阈值   
			 * 。这里不如机会主义剪枝（频率更高）时重要
			 * ，但仍然会是一项显著的                              
			 * 改进。                                                
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
	 * 最后，判断调用方是否需要进行激进的 VACUUM。         
	 *
	 * 确定要使用的表冻结年龄：由调用方指定，或使用   
	 * vacuum_freeze_table_age GUC 的值，但任何情况下都不         
	 * 超过 autovacuum_freeze_max_age * 0.95，这样如果你设有       
	 * 每夜 VACUUM 计划，每晚的 VACUUM 就有机会在防回卷       
	 * 自动清理启动前冻结 XID。     
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
	 * 与上述类似，确定用于多事务的表冻结年龄：由
	 * 调用方指定，或 vacuum_multixact_freeze_table_age    
	 * GUC 的值，但任何情况下都不超过 effective_              
	 * multixact_freeze_max_age * 0.95，这样如果你设有每夜      
	 * VACUUM 计划，每晚的 VACUUM 就有机会在防回卷自动清 
	 * 理启动前冻结多事务。                           
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
 * vacuum_xid_failsafe_check() -- 由 VACUUM 的回卷故障安全    
 * 机制用来判定其表的 relfrozenxid 和 relminmxid 现在是否     
 * 已危险地过于久远。 
 *
 * 返回 true 时，VACUUM 调用方触发故障安全。  
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
 *		若扫描了整个关系，则直接使用看到的存活元组数；
 *		但如果没有，不应盲目地按该数字外推，因为 VACUUM   
 *		可能扫描了表中相当不随机的子集。                 
 *		当只有部分信息时，我们取 pg_class.reltuples/            
 *		pg_class.relpages 的旧值作为未扫描页面中元组密度       
 *		的度量。                                
 *
 *		注意：scanned_tuples 应只统计*存活*元组，      
 *		pg_class.reltuples 即如此定义。  
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
	 * 当连续多个 VACUUM 命令反复扫描同样几页，         
	 * 而表中内容没有实际变化时，对元组密度的认知就     
	 * 可能逐渐失真。成因可能是 vacuumlazy.c 的实现细节， 
	 * 例如它倾向于总是扫描最后一个堆页面。              
	 * 在这里处理这种情况。                                    
	 *
	 * 如果按照现有 pg_class 条目，关系大小*完全相同*，  
	 * 并且只有少数页面（少于 2%）被扫描，则保留   
	 * reltuples 的现有值。当仅扫描了该关系页面中不超过   
	 * 单个页面的子集时，也保留现有值。                      
	 *
	 * （注意：可能返回 -1。）     
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
	 * 好了，边界情况已覆盖。通常计算是先把旧测量值 
	 * 转为密度（每页元组数），再据此估计未扫描页 
	 * 中的元组数，最后再加上                                      
	 * 已扫描页中的元组数。                               
	 */
	old_density = old_rel_tuples / old_rel_pages;
	unscanned_pages = (double) total_pages - (double) scanned_pages;
	total_tuples = old_density * unscanned_pages + scanned_tuples;
	return floor(total_tuples + 0.5);
}


/*
 *	vac_update_relstats() -- 更新单个关系的统计信息 
 *
 *		更新其 pg_class 行中保存的整个关系统计信息。     
 *		如果执行 ANALYZE，还会有额外统计信息被更新， 
 *		但我们总是更新这些统计信息。此例程同时适用于  
 *		pg_class 中的索引和堆关系条目。           
 *
 *		这里我们违反了事务语义：用新值覆盖关系     
 *		现有的 pg_class 元组。只要我们确信无论本事务   
 *		是否提交新值都是正确的，这样做就是相当安全      
 *		的。之所以要这样做，是因为如果以常规方式更新 
 *		这些元组，那么清理 pg_class 本身就无法很好地工   
 *		作了 --- 等到一轮清理周期完成时，pg_class 中      
 *		的大多数元组都已经过时了。当然，这只适用于   
 *		固定大小的非空列，而这些列恰好就是这样的列。     
 *
 *		这样做的另一个原因是：当我们处于惰性 VACUUM     
 *		且设置了 PROC_IN_VACUUM 时，不能执行任何常规更新。   
 *		正在清理 pg_class 的人可能认为能删除 xmin =         
 *		我们 xid 的元组。    
 *
 *		除 relpages、relallvisible 这类根本非事                 
 *		务性的统计信息外，我们还尽力维护某些惰性更      
 *		新的 DDL 标志（如 relhasindex），不再正确时便          
 *		将其清除。这在 VACUUM 中是安全的，因为 VACUUM    
 *		不能与 CREATE INDEX/RULE/TRIGGER 并行运行，也不          
 *		能属于事务块。但在处于外层事务内的 ANALYZE 中 
 *		却*不*安全：例如当前事务可能已删除最后一个      
 *		索引；此时我们会认为应清除 relhasindex，但事务   
 *		稍后若回滚，那便是错的。所以在外层事务内时      
 *		就不更新这些 DDL 标志。这没有问题，因为推迟    
 *		标志维护总是允许                                         
 *		的。           
 *
 *		注意：num_tuples 应只统计*存活*元组，      
 *		pg_class.reltuples 即如此定义。  
 *
 *		此例程由 VACUUM 和 ANALYZE 共用。    
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
	 * 更新 relfrozenxid，除非调用方传 InvalidTransactionId 
	 * 表示其无新数据。      
	 *
	 * 通常我们不让 relfrozenxid 倒退。然而，若存储的      
	 * relfrozenxid "处于未来"，则最好假定其损坏，并用    
	 * 表中剩余的最旧 XID 覆盖。关于什么算"处于未来"，这 
	 * 应与 vac_update_datfrozenxid() 所用标准保持                    
	 * 一致。             
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
 *	vac_update_datfrozenxid() -- 更新本库 pg_database.datfrozenxid     
 *
 *		把 pg_database 的 datfrozenxid 条目更新为本数据库中   
 *		pg_class.relfrozenxid 值的最小值。    
 *
 *		类似地，把 datminmxid 更新为各 pg_class.        
 *		relminmxid 的最小值。 
 *
 *		若能推进任一 pg_database 值，则还应尝试截断     
 *		pg_xact 与 pg_multixact。       
 *
 *		我们在此违反事务语义：用新值覆盖数据库现有    
 *		pg_database 元组。这相当安全，因为无论本事务是否 
 *		提交，新值都正确。与 vac_update_relstats 一样，这      
 *		避免了 VACUUM 之后留下死元组                                
 *		。                   
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
	 * "min" 计算的初始化采用       
	 * GetOldestNonRemovableTransactionId()，对新建表的     
	 * 未提交 pg_class 条目而言，是最小 relfrozenxid 的一
	 * 个合理近似；参见 AddNewRelationTuple()。因此从它出  
	 * 发不会得出错误的最小值。                 
	 */
	newFrozenXid = GetOldestNonRemovableTransactionId(NULL);

	/*
	 * 类似地，用 AddNewRelationTuple() 中用于新表 pg_class 的值初始化
	 * MultiXact "min"。
	 */
	newMinMulti = GetOldestMultiXactId();

	/*
	 * 找出扫描期间我们能有效见到的最新 relfrozenxid 与   
	 * relminmxid 值。这些都是保守值，但追求更精确并不值 
	 * 得。                                   
	 */
	lastSaneFrozenXid = ReadNextTransactionId();
	lastSaneMinMulti = ReadNextMultiXactId();

	/*
	 * 我们必须顺序扫描 pg_class 来找出最小的 Xid，因为    
	 * 没有索引可帮上忙。 
	 *
	 * 要防范的竞态条件参见 vac_truncate_clog()。     
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
		 * 某些表 AM 可能不需要每关系的 xid / multixid 水平线  
		 * 因此，允许 relfrozenxid 和 relminmxid 独立地不被       
		 * 设置（即各自 Invalid*Id），似乎合理。     
		 * 于是仅当已设置时，才分别验证并计算水平线。  
		 *     
		 *
		 * 如果一切正常，任何关系都不应有"未来"的 
		 * relfrozenxid 或 relminmxid。不过，已知 pg_upgrade 中      
		 * 的 bug 曾导致出现这种情况。若看到任何"未来"   
		 * 的条目，就临阵退缩。这能确保我们不会在这些   
		 * 关系被扫描并清理之前截断 clog 与 multixact         
		 * SLRU。                                                 
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
	 * 获取待涂改元组的副本。我们可以先检查 syscache 元   
	 * 组。若其结论为 !dirty，便可避免等待并发的   
	 * heap_update()，也无需以独占方式锁定缓冲区。眼下不   
	 * 要做这种优化。        
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
	 * 与 vac_update_relstats() 一样，我们通常不愿让    
	 * datfrozenxid 倒退；但如果它"处于未来"，那它必定已   
	 * 损坏，覆盖它似乎最好。         
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
 *	vac_truncate_clog() -- 尝试截断提交日志          
 *
 *		扫描 pg_database 以确定全系统最旧的 datfrozenxid，    
 *		并用它截断事务提交日志（pg_xact）。也更新  
 *		varsup.c 维护的 XID 回卷限制信息。               
 *		datminmxid 亦然。    
 *
 *		传入的 frozenXID 和 minMulti 是本库自身 pg_database 条   
 *		目的更新后取值。它们用于初始化"最小值"计算。调 
 *		用方还传入"最后健全"的 XID 和 MXID，因其手头已  
 *		有这些值。       
 *
 *		本例程只在我们成功改变了本库的 datfrozenxid/  
 *		datminmxid 值时才会调用，或在我们发现共享的     
 *		XID 回卷限制已过时。 
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
	 * 扫描 pg_database 以计算最小的 datfrozenxid/datminmxid  
	 *
	 * 由于 vac_update_datfrozenxid 是就地更新 datfrozenxid/           
	 * datminmxid，故我们查看时值可能变化。每个值只      
	 * 获取一次，以确保比较逻辑行为正常。（此处和其 
	 * 他许多地方一样，我们假定共享存储中读取或更新     
	 * XID 为原子的。
	 *
	 * 注意：我们无需担心 CREATE DATABASE 插入新条目带来的 
	 * 竞态条件。这种条目都会带有某现有库的               
	 * datfrozenxid 副本，且该源库不可能是我们的库，因为  
	 * 有禁止复制含活动后端的库的互锁。因此新条目    
	 * 不会降低最小值。此外，若两个 VACUUM 并发修改        
	 * 不同库的 datfrozenxid，最坏结果也只是 pg_xact         
	 * 未被尽可能地积极截断而已                                   
	 * 。            
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
		 * 若数据库正在被删除或删除中被中断则不可能 
		 * 再有连接到它，因此此处无需考虑它。这一点 
		 * 是有利的，因为它也无法被                          
		 * 自动清理处理。                                             
		 */
		if (database_is_invalid_form((Form_pg_database) dbform))
		{
			elog(DEBUG2,
				 "skipping invalid database \"%s\" while computing relfrozenxid",
				 NameStr(dbform->datname));
			continue;
		}

		/*
		 * 若一切正常，数据库都不应有"处于未来"的 
		 * datfrozenxid 或 datminmxid。然而，pg_upgrade 的缺陷曾    
		 * 导致过这种情况。若见任何"处于未来"的条目，  
		 * 我们就退缩而什么都不做。这确保那些数据库      
		 * 被扫描并清理前不会截断 clog。（不过，仍会
		 * 在合适时发出"已回卷"警告。                         
		 * ）                                               
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
	 * 在截断前先推进提交时间戳的最旧值，这样若用户  
	 * 在此点后立即请求我们正截断掉的事务的时间戳，    
	 * 他将得到 NULL，而不是 slru.c 的难看的"文件未            
	 * 找到"错误。对 xact/multixact 无关紧要，因其不受   
	 * 用户任意查找的影响。                               
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
 *	vacuum_rel() -- 清理一个堆关系   
 *
 *		relid 标识要清理的关系。若提供了 relation，则用   
 *		其中的名称来报告打开/锁定关系的失败；一旦成功 
 *		打开关系后就不要再使用它，因为它可能已过时。  
 *		         
 *
 *		若可对此表继续执行请求的 ANALYZE 操作，       
 *		则返回 true。       
 *
 *		一次只清理一个堆有额外开销，因为我们要在清 
 *		理之前再次确认堆存在。这是为了让清理分散   
 *		到多个小事务。否则，两阶段锁定会要求我       
 *		们在清理器的一次完整遍历期间锁定整个数据库。  
 *		                                                                     
 *
 *		进入和退出时我们都不在事务之中。   
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
		 * 惰性清理中，我们可以设置 PROC_IN_VACUUM 标志， 
		 * 让其他并发的 VACUUM 知道它们在确定 OldestXmin 时   
		 * 可以忽略本次清理。（在完整 VACUUM 期间不设置     
		 * 它的原因恰恰是：可能要为函数索引运行用  
		 * 户定义的函数，并要确保：若它们使用了上面的   
		 * 快照，它所需的元组就不会从其他表移除。依赖  
		 * 其他表内容的索引函数可以说有问题，但我们不  
		 * 会在此通过违反事务语义的方式破坏它。）         
		 *                                          
		 *
		 * 我们也设置 VACUUM_FOR_WRAPAROUND 标志，该标志由自动  
		 * 清理向下传递；用于避免取消紧急调用的            
		 * 清理。       
		 *
		 * 注意：这些标志保持到 CommitTransaction 或   
		 * AbortTransaction。在重置 MyProc->xid/xmin 之前，      
		 * 我们不想清除它们，否则 GetOldestNonRemovable         
		 * TransactionId() 可能倒退，这可能不是好事。（我们   
		 * 还在获取自身快照*之前*设置 PROC_IN_VACUUM，        
		 * 使 xmin 不会先于标志设置变为可见。）    
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
	 * 检查是否需基于所有权跳过关系。为手动操作构建   
	 * 待清理关系列表时也会进行此检查；但 VACUUM 可能  
	 * 跨多个事务发生，其间所有权可能已变，故此处  
	 * 还需额外做此检查。这种情况下请确保只为 VACUUM 生   
	 * 成日志                                                              
	 * 。  
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
	 * 静默忽略属于其他后端的临时表 —— 尝试清      
	 * 理它们会招致大麻烦，因为其内容在磁盘上可能   
	 * 不是最新的。（这里不发出警告；那只会造成    
	 * 全库 VACUUM 期间的喋喋不休。                            
	 * ）     
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
	 * 同时获取会话级锁。它在多个事务中保护我们对  
	 * 关系的访问，以便放心清理关系的 TOAST 表（      
	 * 如果有的话），这样就能确信没有人在删除             
	 * 父关系。                 
	 *
	 * 注意：这不会阻塞，即使有人在等待访问，因为锁  
	 * 管理器知道这两个锁请求来自同一进程。             
	 *              
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
	 * 切换为表所有者的 userid，使各索引函数均以该用户    
	 * 运行。同时锁定安全受限操作，并安排 GUC 变更  
	 * 仅对本命令局部生效。（惰性清理无需此操作，但  
	 * 无害。）                                
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/*
	 * 如果设置了 PROCESS_MAIN（默认），则是清理主关系  
	 * 的时候。否则可跳过这一部分。如果需要处理         
	 * TOAST 表（例如设置了 PROCESS_TOAST），那么在递归到    
	 * TOAST 表时会强制设置 PROCESS_MAIN。  
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
	 * 如果该关系有次级 TOAST 表，那么趁我们仍持有主表
	 * 会话锁时，把 TOAST 表也一并清理。不过要注意， 
	 * "analyze" 不会在 toast 表上执行。这很好，因为 toaster   
	 * 总是用硬编码索引访问，而统计信息对 toast 关系 
	 * 完全不重要。                      
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
 * 打开给定关系全部可清理的索引，并对每个索引取  
 * 得指定类型的锁。把索引的 Relation 指针数组返回到      
 * *Irel 中，索引数目返回到 *nindexes 中。               
 *
 * 若一个索引被标记为可插入（indisready），我们就认     
 * 为它是可清理的。如果不是，很可能是 CREATE INDEX         
 * CONCURRENTLY 命令在执行早期失败，而我们手中的东      
 * 西已损坏到无法处理的地步。即使索引非 indisvalid，     
 * 我们也会清理；这很重要，因唯一索引必查唯一性，勿   
 * 撞悬空索引指针。    
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
 * vacuum_delay_point --- 检查中断和基于代价的延迟。   
 *
 * VACUUM 处理的每个主循环均应调用，通常每处理 
 * 一个页调用一次。          
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
		 * 为自动清理 worker 平衡并更新限制值。必须定期这 
		 * 样做，因参与分摊限制的 worker 数目可能已改变 
		 * 。                                  
		 *
		 * TODO: 除"睡醒后检查"外，何时这样做的判定标准    
		 * 也许还有更优者。      
		 */
		AutoVacuumUpdateCostLimit();

		/* 睡眠期间可能收到中断 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * 为并行工作进程计算清理延迟。     
 *
 * 并行清理的基于成本延迟，其基本思想是让每个工作进 
 * 程按其完成工作的份额比例来睡眠。实现方式是让            
 * 所有并行清理工作进程（含领导进程）都共享看到成  
 * 本相关参数（主要是 VacuumCostBalance）。我们允许每       
 * 个工作进程在产生成本时随时更新它，然后据此决定是  
 * 否需要睡眠。我们按工作进程已产生的成本                
 * （VacuumCostBalanceLocal）来计算其睡眠时间   
 * 把 VacuumSharedCostBalance 减去相应的数额。这样可避       
 * 免让完成 I/O 较少的工作进程陷入睡眠，从而确保 I/O        
 * 较多的工作进程                                   
 * 受到更强的节流。                    
 *
 * 仅当工作进程执行的 I/O 超过某个阈值时，我们才允许
 * 其睡眠。该阈值基于活跃工作进程的数量              
 * （VacuumActiveNWorkers）算出，且总成本余额超过      
 * 系统所设 VacuumCostLimit。测试表明，若我们强制      
 * 已做完超过其 50% 工作份额的工作进程去睡眠，就能达 
 * 到所需的节流效果。   
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
