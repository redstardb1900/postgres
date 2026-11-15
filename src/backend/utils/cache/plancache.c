/*-------------------------------------------------------------------------
 *
 * plancache.c
 *	  计划缓存管理。
 *
 * 计划缓存管理器有两个主要职责：决定何时使用
 * 通用计划还是定制（针对具体参数值的）计划，
 * 以及跟踪缓存计划是否需要因其所依赖对象的模式
 * 变更而失效。
 *
 * 选择通用还是定制计划的逻辑在 choose_custom_plan 中，
 * 注释见该函数。
 *
 * 缓存失效由 sinval 事件驱动。任何与该事件匹配的
 * CachedPlanSource 都会被标记为无效，其通用
 * CachedPlan（如果有）也一样。当下一次对缓存计划
 * 的需求出现时，会重新执行语法分析与重写以构建
 * 新的有效查询树，然后照常进行规划。如果当前
 * search_path 与上次不同，或者涉及 RLS 时用户或 RLS
 * 环境改变，我们也会强制重新分析和重新规划。
 *
 * 注意，如果 sinval 是用户 DDL 操作的结果，语法分析
 * 可能抛出错误，例如查询引用的列已不复存在。另一
 * 种可能是查询的输出 tupdesc 发生变化（例如 "SELECT *"
 * 的展开形式可能与之前不同）。
 * 缓存计划的创建者可以指定在重新规划时是否允许
 * 查询改变输出 tupdesc --- 如果允许，就要靠调用者
 * 留意变化并加以处理。
 *
 * 目前，我们精确跟踪计划对关系、用户定义函数和
 * 域的依赖。当收到 relcache 失效事件或 pg_proc 或
 * pg_type syscache 失效事件时，我们只使那些依赖于
 * 被改对象的计划失效。（注意：这一方案假定任何
 * 需要重新规划的表修改都会产生 relcache 失效事件。）
 * 我们也关注某些其他系统目录上的失效事件，例如
 * pg_namespace；但对它们，我们的应对只是使全部计
 * 划失效。我们期望这些目录的更新足够罕见，因此
 * 更细粒度的跟踪不值得费
 * 力气。
 *
 * 除了成熟的查询计划之外，我们还提供了一种检测
 * 简单标量表达式失效的设施。这相当简陋；
 * 旧表达式失效时，须由调用者重建
 * 新的表达式。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/plancache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/transam.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * 当缓存计划的主题语句是事务控制命令，或是那种
 * 需要快照尚未设置（例如 SET 或 LOCK）的语句时，
 * 我们必须跳过涉及数据库访问的"开销"操作。更一般
 * 地讲，不需要语法分析/重写/规划活动的语句从来
 * 不需要重新验证，所以我们可以一并那样对待。
 * 为了 postgres.c 的方便，空语句也照此对待。
 */
#define StmtPlanRequiresRevalidation(plansource)  \
	((plansource)->raw_parse_tree != NULL && \
	 stmt_requires_parse_analysis((plansource)->raw_parse_tree))

/*
 * 这是后端"已保存"CachedPlanSources 列表的表头（即
 * 存放在长寿命存储中并会被检查 sinval 事件的）。
 * 我们用 dlist 而非独立的 List
 * 单元，从而保证能无错保存。
 */
static dlist_head saved_plan_list = DLIST_STATIC_INIT(saved_plan_list);

/*
 * 这是后端 CachedExpressions 列表的表头。
 */
static dlist_head cached_expression_list = DLIST_STATIC_INIT(cached_expression_list);

static void ReleaseGenericPlan(CachedPlanSource *plansource);
static List *RevalidateCachedQuery(CachedPlanSource *plansource,
								   QueryEnvironment *queryEnv);
static bool CheckCachedPlan(CachedPlanSource *plansource);
static CachedPlan *BuildCachedPlan(CachedPlanSource *plansource, List *qlist,
								   ParamListInfo boundParams, QueryEnvironment *queryEnv);
static bool choose_custom_plan(CachedPlanSource *plansource,
							   ParamListInfo boundParams);
static double cached_plan_cost(CachedPlan *plan, bool include_planner);
static Query *QueryListGetPrimaryStmt(List *stmts);
static void AcquireExecutorLocks(List *stmt_list, bool acquire);
static void AcquirePlannerLocks(List *stmt_list, bool acquire);
static void ScanQueryForLocks(Query *parsetree, bool acquire);
static bool ScanQueryWalker(Node *node, bool *acquire);
static TupleDesc PlanCacheComputeResultDesc(List *stmt_list);
static void PlanCacheRelCallback(Datum arg, Oid relid);
static void PlanCacheObjectCallback(Datum arg, int cacheid, uint32 hashvalue);
static void PlanCacheSysCallback(Datum arg, int cacheid, uint32 hashvalue);

/* GUC 参数 */
int			plan_cache_mode = PLAN_CACHE_MODE_AUTO;

/*
 * InitPlanCache：于 InitPostgres 中初始化。
 *
 * 我们要做的只是挂进 inval.c 的回调列表。
 */
void
InitPlanCache(void)
{
	CacheRegisterRelcacheCallback(PlanCacheRelCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(PROCOID, PlanCacheObjectCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(TYPEOID, PlanCacheObjectCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(NAMESPACEOID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(OPEROID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(AMOPOPID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(FOREIGNSERVEROID, PlanCacheSysCallback, (Datum) 0);
	CacheRegisterSyscacheCallback(FOREIGNDATAWRAPPEROID, PlanCacheSysCallback, (Datum) 0);
}

/*
 * CreateCachedPlan：初始创建计划缓存项。
 *
 * 缓存计划的创建分为两步：CreateCachedPlan 与
 * CompleteCachedPlan。CreateCachedPlan 应在把查询
 * 送入 raw_parser 之后、做语法分析与重写之前调用；
 * CompleteCachedPlan 则在其后调用。这样安排的原因
 * 在于可以省去对原始语法树的一轮复制，因为解析器
 * 通常会涂改原始语法树。否则调用者将不得不额外
 * 复制一份语法树，以确保在创建计划缓存项时手里
 * 还有一份干净的副本可交。
 *
 * 交给 CreateCachedPlan 的所有实参都会被复制进一个
 * 内存上下文中，该上下文是调用时 CurrentMemoryContext
 * 的子上下文——它应当是一个相当短命的工作上下
 * 文，出错时即会消失。这确保缓存计划数据结构
 * 会在尚未完全构建好之前出错时也照样消失。
 * 一旦构建完成，如有需要可调用
 * SaveCachedPlan 延寿。
 *
 * raw_parse_tree：raw_parser() 的输出，空查询则为 NULL
 * query_string：原查询文本
 * commandTag：查询命令标签，空查询为 UNKNOWN
 */
CachedPlanSource *
CreateCachedPlan(RawStmt *raw_parse_tree,
				 const char *query_string,
				 CommandTag commandTag)
{
	CachedPlanSource *plansource;
	MemoryContext source_context;
	MemoryContext oldcxt;

	Assert(query_string != NULL);	/* 8.4 起必需 */

	/*
	 * 为 CachedPlanSource 及其永久附属数据建一个专用
	 * 内存上下文。它多半不会很大，但以防万一，允许
	 * 它长大。起初它是调用者上下文的子上下文（我们
	 * 假定它是短暂的），这样出错时便会被清
	 * 理掉。
	 */
	source_context = AllocSetContextCreate(CurrentMemoryContext,
										   "CachedPlanSource",
										   ALLOCSET_START_SMALL_SIZES);

	/*
	 * 在新上下文内创建并填充 CachedPlanSource 结构体。
	 * 此刻大多数字段先留空。
	 */
	oldcxt = MemoryContextSwitchTo(source_context);

	plansource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	plansource->magic = CACHEDPLANSOURCE_MAGIC;
	plansource->raw_parse_tree = copyObject(raw_parse_tree);
	plansource->query_string = pstrdup(query_string);
	MemoryContextSetIdentifier(source_context, plansource->query_string);
	plansource->commandTag = commandTag;
	plansource->param_types = NULL;
	plansource->num_params = 0;
	plansource->parserSetup = NULL;
	plansource->parserSetupArg = NULL;
	plansource->cursor_options = 0;
	plansource->fixed_result = false;
	plansource->resultDesc = NULL;
	plansource->context = source_context;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;
	plansource->query_context = NULL;
	plansource->rewriteRoleId = InvalidOid;
	plansource->rewriteRowSecurity = false;
	plansource->dependsOnRLS = false;
	plansource->gplan = NULL;
	plansource->is_oneshot = false;
	plansource->is_complete = false;
	plansource->is_saved = false;
	plansource->is_valid = false;
	plansource->generation = 0;
	plansource->generic_cost = -1;
	plansource->total_custom_cost = 0;
	plansource->num_generic_plans = 0;
	plansource->num_custom_plans = 0;

	MemoryContextSwitchTo(oldcxt);

	return plansource;
}

/*
 * CreateOneShotCachedPlan：最初创建一次性计划缓存项。
 *
 * 这一 CreateCachedPlan 变体创建的计划缓存项只打算
 * 使用一次。不发生数据复制：所有数据结构都留在
 * 调用者的内存上下文中（通常应在执行完成后清理
 * 该上下文）。CachedPlanSource 结构体本身也在该上
 * 下文创建。
 *
 * 一次性计划无法被保存或复制，因为我们没有努力
 * 保持原始语法树不被涂改。也不支持失效，所以
 * 计划的使用必须在当前事务内完成，而且必须避免
 * 可能使 querytree_list 失效的 DDL。
 *
 * raw_parse_tree：raw_parser() 的输出，空查询则为 NULL
 * query_string：原查询文本
 * commandTag：查询命令标签，空查询则为 NULL
 */
CachedPlanSource *
CreateOneShotCachedPlan(RawStmt *raw_parse_tree,
						const char *query_string,
						CommandTag commandTag)
{
	CachedPlanSource *plansource;

	Assert(query_string != NULL);	/* 8.4 起必需 */

	/*
	 * 在调用者的内存上下文内创建并填充
	 * CachedPlanSource 结构体。大多数字段先留空。
	 */
	plansource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	plansource->magic = CACHEDPLANSOURCE_MAGIC;
	plansource->raw_parse_tree = raw_parse_tree;
	plansource->query_string = query_string;
	plansource->commandTag = commandTag;
	plansource->param_types = NULL;
	plansource->num_params = 0;
	plansource->parserSetup = NULL;
	plansource->parserSetupArg = NULL;
	plansource->cursor_options = 0;
	plansource->fixed_result = false;
	plansource->resultDesc = NULL;
	plansource->context = CurrentMemoryContext;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;
	plansource->query_context = NULL;
	plansource->rewriteRoleId = InvalidOid;
	plansource->rewriteRowSecurity = false;
	plansource->dependsOnRLS = false;
	plansource->gplan = NULL;
	plansource->is_oneshot = true;
	plansource->is_complete = false;
	plansource->is_saved = false;
	plansource->is_valid = false;
	plansource->generation = 0;
	plansource->generic_cost = -1;
	plansource->total_custom_cost = 0;
	plansource->num_generic_plans = 0;
	plansource->num_custom_plans = 0;

	return plansource;
}

/*
 * CompleteCachedPlan：创建计划缓存项的第二步。
 *
 * 传入查询的分析并重写后的形式，以及关于参数等
 * 所需的附属数据。所有传入值都会被复制进
 * CachedPlanSource 的内存，下述情形除外。
 * 随后即可调用 GetCachedPlan 获取计划，也可选
 * 择用 SaveCachedPlan 保存该 CachedPlanSource。
 *
 * 如果 querytree_context 非 NULL，querytree_list 必须
 * 存放在该上下文中（其他参数则不必）。该列表
 * querytree_list 不会被复制，而是把给定上下文保留为
 * CachedPlanSource 初始的 query_context。（它应当是
 * 作为调用者工作内存上下文的子上下文创建的，但
 * 现在它将被重新挂接为 CachedPlanSource 的成员。）
 * querytree_context 通常就是调用者做原始解析与语
 * 法分析的上下文。相比传 NULL，这种做法省去一次
 * 树复制步骤，但会在 query_context 里留下大量额外
 * 垃圾：既有语法分析产生的多余物，也有原始语法
 * 树中未用到的部分。使用这一选项是以空间换时间
 * 的做法，适合不打算长存的情形。
 *
 * plancache.c 无从得知如何复制 parserSetupArg 所引用
 * 的数据，而且这样做往往并不合适。使用该选项时，
 * 被引用的数据在 CachedPlanSource 存续期间保持有
 * 效是调用者的责任。
 *
 * 如果 CachedPlanSource 是"一次性"计划，则完全不发生
 * querytree 复制，querytree_context 被忽略；传入的
 * querytree_list 足够长寿是调用者的责任。
 *
 * plansource：CreateCachedPlan 返回的结构体
 * querytree_list：查询分析并重写后的形式（Query 节点列表）
 * querytree_context：querytree_list 所在上下文，
 *					  或 NULL，即复制到新上下文中
 * param_types：固定参数类型 OIDs 的数组，无则 NULL
 * num_params：固定参数的个数
 * parserSetup：处理查询参数的备用方法
 * parserSetupArg：parserSetup 的入参
 * cursor_options：传规划器的选项位掩码
 * fixed_result：true 表示禁止查询结果 tupdesc 今后变化
 */
void
CompleteCachedPlan(CachedPlanSource *plansource,
				   List *querytree_list,
				   MemoryContext querytree_context,
				   Oid *param_types,
				   int num_params,
				   ParserSetupHook parserSetup,
				   void *parserSetupArg,
				   int cursor_options,
				   bool fixed_result)
{
	MemoryContext source_context = plansource->context;
	MemoryContext oldcxt = CurrentMemoryContext;

	/* 断言调用者的操作顺序是合理的 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(!plansource->is_complete);

	/*
	 * 若调用者提供了 querytree_context，则将其重新挂到
	 * CachedPlanSource 的上下文之下；否则就创建合适的
	 * 上下文并把 querytree_list 复制进去。但对一次性计
	 * 划则不应做任何数据复制；对它们，假定传入的
	 * 列表足够长寿。
	 */
	if (plansource->is_oneshot)
	{
		querytree_context = CurrentMemoryContext;
	}
	else if (querytree_context != NULL)
	{
		MemoryContextSetParent(querytree_context, source_context);
		MemoryContextSwitchTo(querytree_context);
	}
	else
	{
		/* 同样，querytree_context 很可能可以很小 */
		querytree_context = AllocSetContextCreate(source_context,
												  "CachedPlanQuery",
												  ALLOCSET_START_SMALL_SIZES);
		MemoryContextSwitchTo(querytree_context);
		querytree_list = copyObject(querytree_list);
	}

	plansource->query_context = querytree_context;
	plansource->query_list = querytree_list;

	if (!plansource->is_oneshot && StmtPlanRequiresRevalidation(plansource))
	{
		/*
		 * 用规划器的机制来提取依赖。数据保存在
		 * query_context 中。（假定此调用产生的额外垃圾
		 * 不多。）一次性计划可跳过此步；不需重新验证
		 * 的计划反正也没有这类依赖。
		 */
		extract_query_dependencies((Node *) querytree_list,
								   &plansource->relationOids,
								   &plansource->invalItems,
								   &plansource->dependsOnRLS);

		/* 并更新 RLS 信息。 */
		plansource->rewriteRoleId = GetUserId();
		plansource->rewriteRowSecurity = row_security;

		/*
		 * 同时把当前 search_path 存进 query_context
		 * （这同样不应产生多少额外垃圾，因为几乎可以肯
		 * 定本就有效。）一次性计划同样用不着它；
		 * 而事务控制命令则必须跳过，因为可能引发
		 * 目录访问。
		 */
		plansource->search_path = GetOverrideSearchPath(querytree_context);
	}

	/*
	 * 把最终的参数类型（或其他参数说明数据）存入
	 * source_context，同时保存我们的其他参数。还要保
	 * 存结果元组描述符。
	 */
	MemoryContextSwitchTo(source_context);

	if (num_params > 0)
	{
		plansource->param_types = (Oid *) palloc(num_params * sizeof(Oid));
		memcpy(plansource->param_types, param_types, num_params * sizeof(Oid));
	}
	else
		plansource->param_types = NULL;
	plansource->num_params = num_params;
	plansource->parserSetup = parserSetup;
	plansource->parserSetupArg = parserSetupArg;
	plansource->cursor_options = cursor_options;
	plansource->fixed_result = fixed_result;
	plansource->resultDesc = PlanCacheComputeResultDesc(querytree_list);

	MemoryContextSwitchTo(oldcxt);

	plansource->is_complete = true;
	plansource->is_valid = true;
}

/*
 * SaveCachedPlan：永久保存缓存计划
 *
 * 本函数把缓存计划挪到 CacheMemoryContext 之下（使
 * 其存活到后端生命周期结束，除非被显式丢弃），
 * 并把它加进会在 sinval 事件发生时检查失效的缓存
 * 计划列表中。
 *
 * 保证本函数不抛错，唯一的例外是调用者错误情形
 * ——尝试保存一次性计划时。调用者通常依赖
 * 因为本函数恰在把 CachedPlanSource 指针加进它们某
 * 个永久数据结构之前或之后被调用。在此之前，
 * CachedPlanSource 只是会在事务中止时自动消失的
 * 暂时性数据。
 */
void
SaveCachedPlan(CachedPlanSource *plansource)
{
	/* 断言调用者的操作顺序是合理的 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);
	Assert(!plansource->is_saved);

	/* 但这似乎值得真正测试一下 */
	if (plansource->is_oneshot)
		elog(ERROR, "cannot save one-shot cached plan");

	/*
	 * 典型用法中，本函数会在从 CachedPlanSource 生成任何
	 * 计划之前被调用。如果已有通用计划，把它挪进
	 * CacheMemoryContext 会相当冒险，因为不清楚调用者
	 * 是否已对引用的长寿命化采取了合适防范。最
	 * 好做法似乎是把该计划丢弃。
	 */
	ReleaseGenericPlan(plansource);

	/*
	 * 把源内存上下文重新挂到 CacheMemoryContext 之下，
	 * 使其无限期存活。query_context 也会跟着走，因为它
	 * 已是其子上下文。
	 */
	MemoryContextSetParent(plansource->context, CacheMemoryContext);

	/*
	 * 把该项加入缓存计划的全局列表。
	 */
	dlist_push_tail(&saved_plan_list, &plansource->node);

	plansource->is_saved = true;
}

/*
 * DropCachedPlan：销毁缓存计划。
 *
 * 实际上它只销毁 CachedPlanSource：被引用的任何
 * CachedPlan 都会被释放，但要等其引用计数降为零才
 * 真正销毁。这应对了 DropCachedPlan 在计划仍被调用
 * 的情形。
 */
void
DropCachedPlan(CachedPlanSource *plansource)
{
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

	/* 如果已被保存，则从列表中移除 */
	if (plansource->is_saved)
	{
		dlist_delete(&plansource->node);
		plansource->is_saved = false;
	}

	/* 递减通用 CachedPlan 的引用计数，不再需要则丢弃 */
	ReleaseGenericPlan(plansource);

	/* 标记为不再有效 */
	plansource->magic = 0;

	/*
	 * 移除 CachedPlanSource 与全部附属数据（如有
	 * query_context）。但一次性计划则什么都不能释放。
	 */
	if (!plansource->is_oneshot)
		MemoryContextDelete(plansource->context);
}

/*
 * ReleaseGenericPlan：释放 CachedPlanSource 的通用计划。
 */
static void
ReleaseGenericPlan(CachedPlanSource *plansource)
{
	/* 对 ReleaseCachedPlan 失败的可能性保持戒心 */
	if (plansource->gplan)
	{
		CachedPlan *plan = plansource->gplan;

		Assert(plan->magic == CACHEDPLAN_MAGIC);
		plansource->gplan = NULL;
		ReleaseCachedPlan(plan, NULL);
	}
}

/*
 * RevalidateCachedQuery：确保分析并重写后的查询树有效。
 *
 * 本函数做的是按需重新获取锁并重做语法分析。
 * 返回时 query_list 有效，而且我们持有开始规划所需
 * 的锁。
 *
 * 如果需要任何语法分析活动，将使用调用者的内存
 * 上下文执行。
 *
 * 返回值是我们不得不重新分析时的临时分析重写查
 * 询树，否则为 NIL。（返回它只是为了在后续
 * BuildCachedPlan 调用中省去一次树复制。）
 */
static List *
RevalidateCachedQuery(CachedPlanSource *plansource,
					  QueryEnvironment *queryEnv)
{
	bool		snapshot_set;
	RawStmt    *rawtree;
	List	   *tlist;			/* 临时查询树列表 */
	List	   *qlist;			/* 永久查询树列表 */
	TupleDesc	resultDesc;
	MemoryContext querytree_context;
	MemoryContext oldcxt;

	/*
	 * 对一次性计划，不支持重新验证检查；假定查询
	 * 的解析、规划与执行都在同一事务内完成，因而无
	 * 需重新获取锁。另外，如果语句类型不可能需要重
	 * 新验证，我们就不必做任何事（并且在处理例如事
	 * 务控制命令时，绝不能冒险访问目录）。
	 */
	if (plansource->is_oneshot || !StmtPlanRequiresRevalidation(plansource))
	{
		Assert(plansource->is_valid);
		return NIL;
	}

	/*
	 * 如果查询当前有效，我们应该有一个已保存的
	 * search_path——查它与当前环境是否相符，若不符
	 * 就重规划。
	 */
	if (plansource->is_valid)
	{
		Assert(plansource->search_path != NULL);
		if (!OverrideSearchPathMatchesCurrent(plansource->search_path))
		{
			/* 使查询树和通用计划失效 */
			plansource->is_valid = false;
			if (plansource->gplan)
				plansource->gplan->is_valid = false;
		}
	}

	/*
	 * 如果查询重写阶段可能有 RLS 依赖，那么只要角色
	 * 或 row_security 设置之一已变化，就必须重做它。
	 */
	if (plansource->is_valid && plansource->dependsOnRLS &&
		(plansource->rewriteRoleId != GetUserId() ||
		 plansource->rewriteRowSecurity != row_security))
		plansource->is_valid = false;

	/*
	 * 如果查询当前有效，先对被引用对象取锁；然后
	 * 再检查一遍。必须这样做以覆盖如下竞态：在拿到
	 * 锁之前失效消息就先到达。
	 */
	if (plansource->is_valid)
	{
		AcquirePlannerLocks(plansource->query_list, true);

		/*
		 * 此时若发生失效，失效回调函数应已把
		 * 查询标记为无效。
		 */
		if (plansource->is_valid)
		{
			/* 成功重新验证并锁定了查询。 */
			return NIL;
		}

		/* 不巧，竞态情形发生了。释放无用锁。 */
		AcquirePlannerLocks(plansource->query_list, false);
	}

	/*
	 * 丢弃已无用的查询树。（注意：不能更早丢弃，
	 * 否则就无法在竞态情形中
	 * 正确释放锁。）
	 */
	plansource->is_valid = false;
	plansource->query_list = NIL;
	plansource->relationOids = NIL;
	plansource->invalItems = NIL;
	plansource->search_path = NULL;

	/*
	 * 释放 query_context。我们并不真指望 MemoryContextDelete
	 * 会失败，但以防万一，确保 CachedPlanSource 留在一
	 * 个还算正常的状态。（此时通用计划暂不解除链
	 * 接，可接受）
	 */
	if (plansource->query_context)
	{
		MemoryContext qcxt = plansource->query_context;

		plansource->query_context = NULL;
		MemoryContextDelete(qcxt);
	}

	/* 如有则丢弃通用计划引用 */
	ReleaseGenericPlan(plansource);

	/*
	 * 现在重新执行语法分析与重写。这顺带取得了安全
	 * 规划所需的锁。
	 */
	Assert(plansource->is_complete);

	/*
	 * 如果快照已设置（正常情形），解析/规划就用它
	 * 就行。但如果还没设置，就安装一个。注意：不必
	 * 检查语法分析是否需要快照；实用命令没有可失效
	 * 的计划，所以我们不会因这类命令走到这
	 * 里。
	 */
	snapshot_set = false;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * 执行语法分析与规则重写。解析器往往会涂改其输
	 * 入，因此必须复制原始语法树以免损坏
	 * 缓存。
	 */
	rawtree = copyObject(plansource->raw_parse_tree);
	if (rawtree == NULL)
		tlist = NIL;
	else if (plansource->parserSetup != NULL)
		tlist = pg_analyze_and_rewrite_withcb(rawtree,
											  plansource->query_string,
											  plansource->parserSetup,
											  plansource->parserSetupArg,
											  queryEnv);
	else
		tlist = pg_analyze_and_rewrite_fixedparams(rawtree,
												   plansource->query_string,
												   plansource->param_types,
												   plansource->num_params,
												   queryEnv);

	/* 如果取了快照则释放之 */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * 检查或更新结果 tupdesc。XXX 这里是否该用比
	 * equalTupleDescs() 更弱的条件？
	 *
	 * 我们假定参数类型与第一次相比没有变化，因此无
	 * 需更新它。
	 */
	resultDesc = PlanCacheComputeResultDesc(tlist);
	if (resultDesc == NULL && plansource->resultDesc == NULL)
	{
		/* 好，不返回元组 */
	}
	else if (resultDesc == NULL || plansource->resultDesc == NULL ||
			 !equalTupleDescs(resultDesc, plansource->resultDesc))
	{
		/* 能给出更好的报错吗？ */
		if (plansource->fixed_result)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cached plan must not change result type")));
		oldcxt = MemoryContextSwitchTo(plansource->context);
		if (resultDesc)
			resultDesc = CreateTupleDescCopy(resultDesc);
		if (plansource->resultDesc)
			FreeTupleDesc(plansource->resultDesc);
		plansource->resultDesc = resultDesc;
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 分配新的 query_context 并把完成的查询树复制进去。
	 * 在完成复制与依赖提取之前它都是临时的。
	 */
	querytree_context = AllocSetContextCreate(CurrentMemoryContext,
											  "CachedPlanQuery",
											  ALLOCSET_START_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(querytree_context);

	qlist = copyObject(tlist);

	/*
	 * 用规划器机制提取依赖。数据存于
	 * query_context。（假定这次调用不会产生太多
	 * 垃圾）
	 */
	extract_query_dependencies((Node *) qlist,
							   &plansource->relationOids,
							   &plansource->invalItems,
							   &plansource->dependsOnRLS);

	/* 并更新 RLS 信息。 */
	plansource->rewriteRoleId = GetUserId();
	plansource->rewriteRowSecurity = row_security;

	/*
	 * 同时把当前 search_path 保存在 query_context 中。（这
	 * 同样不该产生多少额外垃圾，因为路径几乎肯定
	 * 是有效的）
	 */
	plansource->search_path = GetOverrideSearchPath(querytree_context);

	MemoryContextSwitchTo(oldcxt);

	/* 现在重新挂接完成的 query_context 并保存链接 */
	MemoryContextSetParent(querytree_context, plansource->context);

	plansource->query_context = querytree_context;
	plansource->query_list = qlist;

	/*
	 * 注意：我们并不重置 generic_cost 或 total_custom_cost，
	 * 尽管我们可以选择这么做。如果引发失效的 DDL 或
	 * 统计变化意味着成本估算显著改变，那么重置这些
	 * 变量并重新开始会更好；但通常并非如此，更好地
	 * 做法是保留我们来之不易的相对成本
	 * 知识。
	 */

	plansource->is_valid = true;

	/* 返回查询树的临时副本，供规划时可能之用 */
	return tlist;
}

/*
 * CheckCachedPlan：检查 CachedPlanSource 通用计划有效性。
 *
 * 调用者必须已先调用
 * RevalidateCachedQuery。
 *
 * 返回"真"时，我们已取得运行该计划所需的锁。
 * （要让"真"结果没有竞态，就必须这么做。）
 */
static bool
CheckCachedPlan(CachedPlanSource *plansource)
{
	CachedPlan *plan = plansource->gplan;

	/* 断言调用者已检查查询树 */
	Assert(plansource->is_valid);

	/* 若无通用计划，就直接返回"假" */
	if (!plan)
		return false;

	Assert(plan->magic == CACHEDPLAN_MAGIC);
	/* 通用计划非一次性计划 */
	Assert(!plan->is_oneshot);

	/*
	 * 若计划对当前角色无效，便不能用。
	 */
	if (plan->is_valid && plan->dependsOnRole &&
		plan->planRoleId != GetUserId())
		plan->is_valid = false;

	/*
	 * 如果它看起来有效，就取锁并重新检查；逻辑同
	 * RevalidateCachedQuery，只是对象是计划。
	 */
	if (plan->is_valid)
	{
		/*
		 * 计划引用计数必为正，因 plansource 引用它；
		 * 所以无须担心它会在这里从我们脚下消失。
		 */
		Assert(plan->refcount > 0);

		AcquireExecutorLocks(plan->stmt_list, true);

		/*
		 * 若计划暂态，检查 TransactionXmin 是否推进，
		 * 若有则使其失效。
		 */
		if (plan->is_valid &&
			TransactionIdIsValid(plan->saved_xmin) &&
			!TransactionIdEquals(plan->saved_xmin, TransactionXmin))
			plan->is_valid = false;

		/*
		 * 此时若发生失效，失效回调函数应已把
		 * 计划标记为无效。
		 */
		if (plan->is_valid)
		{
			/* 成功重新验证并锁定了查询。 */
			return true;
		}

		/* 不巧，竞态情形发生了。释放无用锁。 */
		AcquireExecutorLocks(plan->stmt_list, false);
	}

	/*
	 * 计划已失效，因此把它从父对象解除链接并释放。
	 */
	ReleaseGenericPlan(plansource);

	return false;
}

/*
 * BuildCachedPlan：从 CachedPlanSource 构建新 CachedPlan。
 *
 * qlist 应是先前 RevalidateCachedQuery
 * 的返回值，或传 NIL 以重新复制 plansource 的 query_list。
 *
 * 要构建与参数值无关的通用计划，请为
 * boundParams 传入 NULL。要构建定制计划，请经
 * boundParams 传入实际参数值。为最佳效果，
 * 每个参数值都应设 PARAM_FLAG_CONST，
 * 否则当提示而非常量。
 *
 * 规划工作在调用者的内存上下文中完成。完成的计
 * 划位于一个子内存上下文中，通常应当把它重新挂
 * 接（除非是一次性计划，那时我们不复制计划）。
 */
static CachedPlan *
BuildCachedPlan(CachedPlanSource *plansource, List *qlist,
				ParamListInfo boundParams, QueryEnvironment *queryEnv)
{
	CachedPlan *plan;
	List	   *plist;
	bool		snapshot_set;
	bool		is_transient;
	MemoryContext plan_context;
	MemoryContext oldcxt = CurrentMemoryContext;
	ListCell   *lc;

	/*
	 * 通常查询树此时应当已经有效，但若不是，
	 * 重建。
	 *
	 * 注意：GetCachedPlan 应已先调用过
	 * RevalidateCachedQuery，因此应已持有足够的锁。
	 * 不过，如果在构建并拒绝了一个通用计划之后
	 * 又来构建定制计划，那么到这里时 is_valid 可
	 * 能为假，原因是构建通用计划期间发生了失
	 * 效。理论上该失效必然是误报，也许是 sinval
	 * 重置事件或 debug_discard_caches 代码所致。但为
	 * 安全起见，把它当真并重做该调用。
	 */
	if (!plansource->is_valid)
		qlist = RevalidateCachedQuery(plansource, queryEnv);

	/*
	 * 如果我们还没有一份可供规划器涂改的查询树列
	 * 表副本，就做一份。对一次性计划，我们假定直接
	 * 涂改原始 query_list 没问题。
	 */
	if (qlist == NIL)
	{
		if (!plansource->is_oneshot)
			qlist = copyObject(plansource->query_list);
		else
			qlist = plansource->query_list;
	}

	/*
	 * 如果快照已设置（正常情形），解析/规划就用它
	 * 就行。但如果还没设置而又需要，就装一个。
	 */
	snapshot_set = false;
	if (!ActiveSnapshotSet() &&
		plansource->raw_parse_tree &&
		analyze_requires_snapshot(plansource->raw_parse_tree))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * 生成计划。
	 */
	plist = pg_plan_queries(qlist, plansource->query_string,
							plansource->cursor_options, boundParams);

	/* 如果取了快照则释放之 */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * 通常我们会为 CachedPlan 及其附属数据建一个专用
	 * 内存上下文。（它多半不会很大，但以防万一，允
	 * 许它长大。此刻它是临时的。）但对一次性计划，
	 * 我们就直接把它留在调用者的内存上下文中。
	 */
	if (!plansource->is_oneshot)
	{
		plan_context = AllocSetContextCreate(CurrentMemoryContext,
											 "CachedPlan",
											 ALLOCSET_START_SMALL_SIZES);
		MemoryContextCopyAndSetIdentifier(plan_context, plansource->query_string);

		/*
		 * 复制到新上下文。
		 */
		MemoryContextSwitchTo(plan_context);

		plist = copyObject(plist);
	}
	else
		plan_context = CurrentMemoryContext;

	/*
	 * 在新上下文内创建并填充 CachedPlan 结构体。
	 */
	plan = (CachedPlan *) palloc(sizeof(CachedPlan));
	plan->magic = CACHEDPLAN_MAGIC;
	plan->stmt_list = plist;

	/*
	 * 如果 RLS 影响了重写阶段，或规划期间注入了角
	 * 色依赖，CachedPlan 就依赖角色；且只要有一个计
	 * 划是暂态它就暂态。
	 */
	plan->planRoleId = GetUserId();
	plan->dependsOnRole = plansource->dependsOnRLS;
	is_transient = false;
	foreach(lc, plist)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

		if (plannedstmt->commandType == CMD_UTILITY)
			continue;			/* 忽略实用语句 */

		if (plannedstmt->transientPlan)
			is_transient = true;
		if (plannedstmt->dependsOnRole)
			plan->dependsOnRole = true;
	}
	if (is_transient)
	{
		Assert(TransactionIdIsNormal(TransactionXmin));
		plan->saved_xmin = TransactionXmin;
	}
	else
		plan->saved_xmin = InvalidTransactionId;
	plan->refcount = 0;
	plan->context = plan_context;
	plan->is_oneshot = plansource->is_oneshot;
	plan->is_saved = false;
	plan->is_valid = true;

	/* 给新计划分配代号 */
	plan->generation = ++(plansource->generation);

	MemoryContextSwitchTo(oldcxt);

	return plan;
}

/*
 * choose_custom_plan：选择使用定制计划还是通用计划
 *
 * 这定义了 GetCachedPlan 所遵循的策略。
 */
static bool
choose_custom_plan(CachedPlanSource *plansource, ParamListInfo boundParams)
{
	double		avg_custom_cost;

	/* 一次性计划总会被视为定制计划 */
	if (plansource->is_oneshot)
		return true;

	/* 否则，若无参数则定制计划绝无意义 */
	if (boundParams == NULL)
		return false;
	/* 规划为空操作时也一样 */
	if (!StmtPlanRequiresRevalidation(plansource))
		return false;

	/* 让设置来强制决定 */
	if (plan_cache_mode == PLAN_CACHE_MODE_FORCE_GENERIC_PLAN)
		return false;
	if (plan_cache_mode == PLAN_CACHE_MODE_FORCE_CUSTOM_PLAN)
		return true;

	/* 看调用者是否想强制决定 */
	if (plansource->cursor_options & CURSOR_OPT_GENERIC_PLAN)
		return false;
	if (plansource->cursor_options & CURSOR_OPT_CUSTOM_PLAN)
		return true;

	/* 生成定制计划，直到至少做过 5 次（任意值） */
	if (plansource->num_custom_plans < 5)
		return true;

	avg_custom_cost = plansource->total_custom_cost / plansource->num_custom_plans;

	/*
	 * 如果通用计划比平均定制计划便宜，就优先用通用
	 * 计划。（因为定制计划的成本里计入了规划
	 * 开销，这意味着通用计划只需便宜过各定制
	 * 计划的执行成本加重新规划成本即可。）
	 * plans.)
	 *
	 * 注意如果 generic_cost 为 -1（表示我们尚未确定通
	 * 用计划的成本），此时我们总会优先选通用计划。
	 */
	if (plansource->generic_cost < avg_custom_cost)
		return false;

	return true;
}

/*
 * cached_plan_cost：计算计划的估算成本
 *
 * 如果 include_planner 为真，还要计入构建计划的估算
 * 成本。（必须把它计入使用定制计划的成本中，但
 * 通用计划则不计入。）
 */
static double
cached_plan_cost(CachedPlan *plan, bool include_planner)
{
	double		result = 0;
	ListCell   *lc;

	foreach(lc, plan->stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

		if (plannedstmt->commandType == CMD_UTILITY)
			continue;			/* 忽略实用语句 */

		result += plannedstmt->planTree->total_cost;

		if (include_planner)
		{
			/*
			 * 目前规划开销的估计很粗糙，以完成计划范围
			 * 表中关系数为基础。
			 * 连接规划开销实际随关系数增长远差于线性，
			 * 不过只到连接折叠限制生效为止。另外，继承子
			 * 关系固然增加规划开销，却不会让连接情形更
			 * 糟。所以成本曲线对关系数的实际形状
			 * 并不那么明显，需要花大力气才能得出
			 * 不那么粗糙的估计，而眼下还不清楚这是
			 * 否值得去做
			 * 的。
			 *
			 * 另一大难处在于我们没有很好的模型来比较
			 * 规划成本与执行成本。
			 * 1000 * cpu_operator_cost 乘数可能偏低，但会先试
			 * 用一阵子，然后再做更激进的修
			 * 正。
			 *
			 * 若真要写更复杂的估算器，大概该放进
			 * src/backend/optimizer/ 而不是这里。
			 */
			int			nrelations = list_length(plannedstmt->rtable);

			result += 1000.0 * cpu_operator_cost * (nrelations + 1);
		}
	}

	return result;
}

/*
 * GetCachedPlan：从 CachedPlanSource 取缓存计划。
 *
 * 本函数隐藏了决定对给定参数使用通用计划还是
 * 定制计划的逻辑：调用者并不知道会得到
 * 哪一种。
 *
 * 返回时计划有效，而且我们持有开始执行所需的
 * 锁。
 *
 * 返回时计划的引用计数已递增；预期随后调用
 * ReleaseCachedPlan()。如果 "owner" 非 NULL，则引用
 * 计数已报告给该 ResourceOwner（注意只有"已保存"
 * 的 CachedPlanSources 才行）。
 *
 * 注意：如果需要任何重新规划活动，将使用调用者
 * 的内存上下文。
 */
CachedPlan *
GetCachedPlan(CachedPlanSource *plansource, ParamListInfo boundParams,
			  ResourceOwner owner, QueryEnvironment *queryEnv)
{
	CachedPlan *plan = NULL;
	List	   *qlist;
	bool		customplan;

	/* 断言调用者的操作顺序是合理的 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);
	/* 但这似乎值得真正测试一下 */
	if (owner && !plansource->is_saved)
		elog(ERROR, "cannot apply ResourceOwner to non-saved cached plan");

	/* 确保查询树列表有效且我们持有解析期锁 */
	qlist = RevalidateCachedQuery(plansource, queryEnv);

	/* 决定是否使用定制计划 */
	customplan = choose_custom_plan(plansource, boundParams);

	if (!customplan)
	{
		if (CheckCachedPlan(plansource))
		{
			/* 我们想要通用计划，且已有有效的一个 */
			plan = plansource->gplan;
			Assert(plan->magic == CACHEDPLAN_MAGIC);
		}
		else
		{
			/* 构建新的通用计划 */
			plan = BuildCachedPlan(plansource, qlist, NULL, queryEnv);
			/* 切实确认 plansource->gplan 已清空 */
			ReleaseGenericPlan(plansource);
			/* 把新通用计划链接进 plansource */
			plansource->gplan = plan;
			plan->refcount++;
			/* 立即重新挂接到合适的上下文 */
			if (plansource->is_saved)
			{
				/* 已保存计划都位于 CacheMemoryContext */
				MemoryContextSetParent(plan->context, CacheMemoryContext);
				plan->is_saved = true;
			}
			else
			{
				/* 否则它应是 plansource 的兄弟 */
				MemoryContextSetParent(plan->context,
									   MemoryContextGetParent(plansource->context));
			}
			/* 每当生成新通用计划就更新 generic_cost */
			plansource->generic_cost = cached_plan_cost(plan, false);

			/*
			 * 若基于现已知道的 generic_cost 值我们本不会选通
			 * 用通用计划，那就忘掉它并改做定制计划。这
			 * 是一处小瑕疵，但为了避免行为出现波动：
			 * 定制计划一直大胜的情形。某一刻我们会试
			 * 验一次通用计划并发现它是输家，但并不想
			 * 真的去执行那个计划。
			 * plan.
			 */
			customplan = choose_custom_plan(plansource, boundParams);

			/*
			 * 若再次规划，就得重新复制 query_list，因为规划
			 * 器多半已在上面涂改过。传 NIL 可强制
			 * BuildCachedPlan 这么做。
			 */
			qlist = NIL;
		}
	}

	if (customplan)
	{
		/* 构建定制计划 */
		plan = BuildCachedPlan(plansource, qlist, boundParams, queryEnv);
		/* 累计定制计划的总成本 */
		plansource->total_custom_cost += cached_plan_cost(plan, true);

		plansource->num_custom_plans++;
	}
	else
	{
		plansource->num_generic_plans++;
	}

	Assert(plan != NULL);

	/* 标记计划为调用者在用 */
	if (owner)
		ResourceOwnerEnlargePlanCacheRefs(owner);
	plan->refcount++;
	if (owner)
		ResourceOwnerRememberPlanCacheRef(owner, plan);

	/*
	 * 已保存计划应位于 CacheMemoryContext 之下，这样在
	 * 引用计数降为零之前它们不会消失。通用计划的情
	 * 形我们已经处理过了；而对定制计划，要在创建
	 * 好后尽快处理。
	 */
	if (customplan && plansource->is_saved)
	{
		MemoryContextSetParent(plan->context, CacheMemoryContext);
		plan->is_saved = true;
	}

	return plan;
}

/*
 * ReleaseCachedPlan：释放对缓存计划的使用。
 *
 * 这会递减引用计数，当计数因此降为零时，
 * 就释放该计划。若 "owner" 非 NULL，则假定
 * 该引用计数由 ResourceOwner 管理。
 *
 * 注意：owner == NULL 用于释放持久数据结构中的引
 * 用，例如父 CachedPlanSource 或 Portal。暂态引用应
 * 当由资源所有者保护。
 */
void
ReleaseCachedPlan(CachedPlan *plan, ResourceOwner owner)
{
	Assert(plan->magic == CACHEDPLAN_MAGIC);
	if (owner)
	{
		Assert(plan->is_saved);
		ResourceOwnerForgetPlanCacheRef(owner, plan);
	}
	Assert(plan->refcount > 0);
	plan->refcount--;
	if (plan->refcount == 0)
	{
		/* 标记为不再有效 */
		plan->magic = 0;

		/* 一次性计划不拥有自己的上下文，无法释放 */
		if (!plan->is_oneshot)
			MemoryContextDelete(plan->context);
	}
}

/*
 * CachedPlanAllowsSimpleValidityCheck：能否使用
 *
 * CachedPlanIsSimplyValid？本函数与 CachedPlanIsSimplyValid
 * 一起为重新验证"简单"通用计划提供快速路径。简
 * 单的核心条件是计划不得需要取任何锁，这转化为
 * 不触碰任何表；这恰好与 PL/pgSQL 中一个重要用法
 * 吻合。本函数检验是否如此，同时检查一些我们
 * 不打算在快速路径中费心处理的其他边角情
 * 形。（注意这类计划仍可能失效，例如当某个
 * 被内联进计划的函数变化了
 * 时。）
 *
 * 如果计划简单有效且 "owner" 非 NULL，返回前在该
 * resowner 中记录引用计数。确保正被活跃使用
 * 的计划持有引用计数是调用者的责任。
 *
 * 只能对已知有效的通用计划（例如刚由 GetCachedPlan
 * 返回的那些）调用本函数。若它返回真，调用者在
 * 该缓存计划，只要 CachedPlanIsSimplyValid 返回真；
 * 该检查比 GetCachedPlan 的完整重新验证便宜。
 * 尽管如此，必需检查一样不缺。
 */
bool
CachedPlanAllowsSimpleValidityCheck(CachedPlanSource *plansource,
									CachedPlan *plan, ResourceOwner owner)
{
	ListCell   *lc;

	/*
	 * 完好性检查：调用者给我们的确是已验证的通用计
	 * 划。注意我们*并不*像你想的那样断言
	 * plansource->is_valid；因为当 GetCachedPlan 返回时它
	 * 有可能已经是假，例如 ResetPlanCache 中途发
	 * 生过。我们应当只要 plan->is_valid 为真就接受该计
	 * 划后预期 CachedPlanIsSimplyValid 重规划。
	 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plan->magic == CACHEDPLAN_MAGIC);
	Assert(plan->is_valid);
	Assert(plan == plansource->gplan);
	Assert(plansource->search_path != NULL);
	Assert(OverrideSearchPathMatchesCurrent(plansource->search_path));

	/* 这里不支持一次性计划。 */
	if (plansource->is_oneshot)
		return false;
	Assert(!plan->is_oneshot);

	/*
	 * 如果计划依赖 RLS 相关因素，或者它是暂态的，就
	 * 拒绝。无表查询多半不可能发生这些，但为了安
	 * 全起见还是检查一下。
	 */
	if (plansource->dependsOnRLS)
		return false;
	if (plan->dependsOnRole)
		return false;
	if (TransactionIdIsValid(plan->saved_xmin))
		return false;

	/*
	 * 如果 AcquirePlannerLocks 会有事可做就拒绝。这样
	 * 判断过于简单，但没必要更细致地探查；事实上，
	 * 对现在的调用者来说连触发这些检查都不可
	 * 能。
	 */
	foreach(lc, plansource->query_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->commandType == CMD_UTILITY)
			return false;
		if (query->rtable || query->cteList || query->hasSubLinks)
			return false;
	}

	/*
	 * 如果 AcquireExecutorLocks 会有事可做就拒绝。鉴于
	 * 前一检查这多半并不必要，但求稳起见。
	 */
	foreach(lc, plan->stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);
		ListCell   *lc2;

		if (plannedstmt->commandType == CMD_UTILITY)
			return false;

		/*
		 * 我们不得不遍历 rtable，因为它很可能含有一个
		 * RTE_RESULT 关系而非完全为空。
		 */
		foreach(lc2, plannedstmt->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);

			if (rte->rtekind == RTE_RELATION)
				return false;
		}
	}

	/*
	 * 好了，它足够简单。注意这里主要确立的是：在检
	 * 查计划的 is_valid 标志之前无须取任何锁。
	 */

	/* 如请求则递增计数。 */
	if (owner)
	{
		ResourceOwnerEnlargePlanCacheRefs(owner);
		plan->refcount++;
		ResourceOwnerRememberPlanCacheRef(owner, plan);
	}

	return true;
}

/*
 * CachedPlanIsSimplyValid：快速检查计划是否仍有效
 *
 * 除非 CachedPlanAllowsSimpleValidityCheck 先前说了
 * 行，否则别用。
 *
 * 如果计划有效且 "owner" 非 NULL，返回前在该
 * resowner 中记录引用计数。确保正被活跃使用
 * 的计划持有引用计数是调用者的责任。
 *
 * 只要本 CachedPlanSource 的唯一用途就是与传入的
 * 特定 CachedPlan 指针相关，这里的代码就是无条件
 * 安全的。如果 plansource 被用于其他目的，其通用
 * 计划就可能在当前调用者没注意时失效并重新生
 * 成，那时当前调用者已过时的计划指针与新通用
 * 计划的实际地址就有可能碰巧撞上。对现在的用
 * 法来说这种场景不可能发生；但对跨多种用途共享
 * 的 plansource，最好把 plan->generation 也保存下来并
 * 核实它仍然匹配。
 */
bool
CachedPlanIsSimplyValid(CachedPlanSource *plansource, CachedPlan *plan,
						ResourceOwner owner)
{
	/*
	 * 这里要小心：调用者起初并不一定持有该计划的引
	 * 用计数，"plan" 便可能是悬空指针。核实它与
	 * plansource 的 gplan（有效或 NULL）相符之前不要
	 * 解引用它。
	 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

	/*
	 * 该计划有缓存失效触发过吗？这一点可以立即
	 * 检查，因为不存在需要先获取的锁。注意这里我们
	 * *确实*检查 plansource->is_valid，以便在其为假时强
	 * 制重建计划。
	 */
	if (!plansource->is_valid ||
		plan == NULL || plan != plansource->gplan ||
		!plan->is_valid)
		return false;

	Assert(plan->magic == CACHEDPLAN_MAGIC);

	/* search_path 还与我们当初设置时相同吗？ */
	Assert(plansource->search_path != NULL);
	if (!OverrideSearchPathMatchesCurrent(plansource->search_path))
		return false;

	/* 仍然有效。如请求则递增计数。 */
	if (owner)
	{
		ResourceOwnerEnlargePlanCacheRefs(owner);
		plan->refcount++;
		ResourceOwnerRememberPlanCacheRef(owner, plan);
	}

	return true;
}

/*
 * CachedPlanSetParentContext：把 CachedPlanSource 移到
 *
 * 新的内存上下文。只能用于未保存的计划；保存后
 * 计划总在 CacheMemoryContext。
 */
void
CachedPlanSetParentContext(CachedPlanSource *plansource,
						   MemoryContext newcontext)
{
	/* 断言调用者的操作顺序是合理的 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/* 但这几点值得真正测试 */
	if (plansource->is_saved)
		elog(ERROR, "cannot move a saved cached plan to another context");
	if (plansource->is_oneshot)
		elog(ERROR, "cannot move a one-shot cached plan to another context");

	/* 好，随调用者把计划放在其希望处 */
	MemoryContextSetParent(plansource->context, newcontext);

	/*
	 * query_context 无需特殊处理，因为它是
	 * plansource->context 的子上下文。但若有通用计划，
	 * 它是 plansource->context 的兄弟。
	 */
	if (plansource->gplan)
	{
		Assert(plansource->gplan->magic == CACHEDPLAN_MAGIC);
		MemoryContextSetParent(plansource->gplan->context, newcontext);
	}
}

/*
 * CopyCachedPlan：复制一个 CachedPlanSource
 *
 * 这是便利例程，相当于 CreateCachedPlan +
 * CompleteCachedPlan，使用输入 CachedPlanSource 中
 * 存储的数据。因此结果是"未保存"的（无论源的状
 * 态如何），而且我们也不复制任何通用计划。
 * 结果当前有效与否与源相同。
 */
CachedPlanSource *
CopyCachedPlan(CachedPlanSource *plansource)
{
	CachedPlanSource *newsource;
	MemoryContext source_context;
	MemoryContext querytree_context;
	MemoryContext oldcxt;

	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/*
	 * 一次性计划不能复制，因为我们没确保解析/规划
	 * 未涂改原始语法树或查询树。
	 */
	if (plansource->is_oneshot)
		elog(ERROR, "cannot copy a one-shot cached plan");

	source_context = AllocSetContextCreate(CurrentMemoryContext,
										   "CachedPlanSource",
										   ALLOCSET_START_SMALL_SIZES);

	oldcxt = MemoryContextSwitchTo(source_context);

	newsource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
	newsource->magic = CACHEDPLANSOURCE_MAGIC;
	newsource->raw_parse_tree = copyObject(plansource->raw_parse_tree);
	newsource->query_string = pstrdup(plansource->query_string);
	MemoryContextSetIdentifier(source_context, newsource->query_string);
	newsource->commandTag = plansource->commandTag;
	if (plansource->num_params > 0)
	{
		newsource->param_types = (Oid *)
			palloc(plansource->num_params * sizeof(Oid));
		memcpy(newsource->param_types, plansource->param_types,
			   plansource->num_params * sizeof(Oid));
	}
	else
		newsource->param_types = NULL;
	newsource->num_params = plansource->num_params;
	newsource->parserSetup = plansource->parserSetup;
	newsource->parserSetupArg = plansource->parserSetupArg;
	newsource->cursor_options = plansource->cursor_options;
	newsource->fixed_result = plansource->fixed_result;
	if (plansource->resultDesc)
		newsource->resultDesc = CreateTupleDescCopy(plansource->resultDesc);
	else
		newsource->resultDesc = NULL;
	newsource->context = source_context;

	querytree_context = AllocSetContextCreate(source_context,
											  "CachedPlanQuery",
											  ALLOCSET_START_SMALL_SIZES);
	MemoryContextSwitchTo(querytree_context);
	newsource->query_list = copyObject(plansource->query_list);
	newsource->relationOids = copyObject(plansource->relationOids);
	newsource->invalItems = copyObject(plansource->invalItems);
	if (plansource->search_path)
		newsource->search_path = CopyOverrideSearchPath(plansource->search_path);
	newsource->query_context = querytree_context;
	newsource->rewriteRoleId = plansource->rewriteRoleId;
	newsource->rewriteRowSecurity = plansource->rewriteRowSecurity;
	newsource->dependsOnRLS = plansource->dependsOnRLS;

	newsource->gplan = NULL;

	newsource->is_oneshot = false;
	newsource->is_complete = true;
	newsource->is_saved = false;
	newsource->is_valid = plansource->is_valid;
	newsource->generation = plansource->generation;

	/* 不妨把已获得的成本知识一并复制 */
	newsource->generic_cost = plansource->generic_cost;
	newsource->total_custom_cost = plansource->total_custom_cost;
	newsource->num_generic_plans = plansource->num_generic_plans;
	newsource->num_custom_plans = plansource->num_custom_plans;

	MemoryContextSwitchTo(oldcxt);

	return newsource;
}

/*
 * CachedPlanIsValid：检验 CachedPlanSource 内重写后
 * 的查询树当前是否有效（即未被标记为需要重新验
 * 证）。
 *
 * 只有在调用者已对计划用到的所有关系取锁时，这
 * 一结果才可信（即没有竞态）。
 */
bool
CachedPlanIsValid(CachedPlanSource *plansource)
{
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	return plansource->is_valid;
}

/*
 * CachedPlanGetTargetList：返回描述计划输出的
 *
 * tlist（如有）。结果保证是最新的。不过它是缓存
 * 计划内的本地存储，下次计划更新时可能消失。
 */
List *
CachedPlanGetTargetList(CachedPlanSource *plansource,
						QueryEnvironment *queryEnv)
{
	Query	   *pstmt;

	/* 断言调用者的操作顺序是合理的 */
	Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
	Assert(plansource->is_complete);

	/*
	 * 如果语句不返回元组则无须做什么（我们假定
	 * 该特性不会因失效而改变）
	 */
	if (plansource->resultDesc == NULL)
		return NIL;

	/* 确保查询树列表有效且我们持有解析期锁 */
	RevalidateCachedQuery(plansource, queryEnv);

	/* 取出主语句并查明它返回什么 */
	pstmt = QueryListGetPrimaryStmt(plansource->query_list);

	return FetchStatementTargetList((Node *) pstmt);
}

/*
 * GetCachedExpression：为一个表达式构建 CachedExpression。
 *
 * 它对该表达式执行与
 * expression_planner() 相同的变换，即把语法分析所
 * 得表达式转成可交执行器的形式。
 *
 * 结果存放在私有且长寿命的内存上下文中。
 * （注意此前可能已在调用者的上下文里
 * 泄漏了不少内存。）传入的 expr 树不会被修改。
 */
CachedExpression *
GetCachedExpression(Node *expr)
{
	CachedExpression *cexpr;
	List	   *relationOids;
	List	   *invalItems;
	MemoryContext cexpr_context;
	MemoryContext oldcxt;

	/*
	 * 把表达式送过规划器，收集依赖。
	 * 这里构建的一切都会泄漏在调用者的上下文中；
	 * 这是有意的，为的是缩小永久数据结构体积。
	 */
	expr = (Node *) expression_planner_with_deps((Expr *) expr,
												 &relationOids,
												 &invalItems);

	/*
	 * 建一个私有内存上下文，把所需内容复制进去。为
	 * 了避免在复制数据失败时泄漏长寿命上下文，我们
	 * 起初把该上下文建在调用者上下文之下。
	 */
	cexpr_context = AllocSetContextCreate(CurrentMemoryContext,
										  "CachedExpression",
										  ALLOCSET_SMALL_SIZES);

	oldcxt = MemoryContextSwitchTo(cexpr_context);

	cexpr = (CachedExpression *) palloc(sizeof(CachedExpression));
	cexpr->magic = CACHEDEXPR_MAGIC;
	cexpr->expr = copyObject(expr);
	cexpr->is_valid = true;
	cexpr->relationOids = copyObject(relationOids);
	cexpr->invalItems = copyObject(invalItems);
	cexpr->context = cexpr_context;

	MemoryContextSwitchTo(oldcxt);

	/*
	 * 把 expr 的内存上下文重新挂到 CacheMemoryContext
	 * 之下使其永存。
	 */
	MemoryContextSetParent(cexpr_context, CacheMemoryContext);

	/*
	 * 把该项加入缓存表达式的全局列表。
	 */
	dlist_push_tail(&cached_expression_list, &cexpr->node);

	return cexpr;
}

/*
 * FreeCachedExpression
 *		删除缓存表达式。
 */
void
FreeCachedExpression(CachedExpression *cexpr)
{
	/* 健全检查 */
	Assert(cexpr->magic == CACHEDEXPR_MAGIC);
	/* 脱离全局列表 */
	dlist_delete(&cexpr->node);
	/* 释放与 CachedExpression 相关的全部存储 */
	MemoryContextDelete(cexpr->context);
}

/*
 * QueryListGetPrimaryStmt
 *		取列表中的"主"语句，即标记 canSetTag 的那个。
 *
 * 若无这样的语句则返回 NULL。若列表中多条语句都被
 * 标记为 canSetTag，则返回第一条。这两种情形在本
 * 函数目前用法中都不应出现。
 */
static Query *
QueryListGetPrimaryStmt(List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
	{
		Query	   *stmt = lfirst_node(Query, lc);

		if (stmt->canSetTag)
			return stmt;
	}
	return NULL;
}

/*
 * AcquireExecutorLocks：取得执行缓存计划所需的锁；
 * acquire 为假则释放之。
 */
static void
AcquireExecutorLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc1;

	foreach(lc1, stmt_list)
	{
		PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc1);
		ListCell   *lc2;

		if (plannedstmt->commandType == CMD_UTILITY)
		{
			/*
			 * 忽略实用语句，但含已解析未规划查询的除外
			 * （如 EXPLAIN）。注意即使查询尚未经过规则重
			 * 写，用 ScanQueryForLocks 也没问题，因为重写不
			 * 会改变查询的表示形
			 * 式。
			 */
			Query	   *query = UtilityContainsQuery(plannedstmt->utilityStmt);

			if (query)
				ScanQueryForLocks(query, acquire);
			continue;
		}

		foreach(lc2, plannedstmt->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);

			if (!(rte->rtekind == RTE_RELATION ||
				  (rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid))))
				continue;

			/*
			 * 对每个关系 OID 取得合适类型的锁。注意我
			 * 们并不真去打开该关系，所以即便它已被彻底
			 * 删除了也不会失败——只会暂时取得一个
			 * 不冲突的锁。
			 */
			if (acquire)
				LockRelationOid(rte->relid, rte->rellockmode);
			else
				UnlockRelationOid(rte->relid, rte->rellockmode);
		}
	}
}

/*
 * AcquirePlannerLocks：取得规划查询树列表所需的锁；
 * acquire 为假则释放之。
 *
 * 注意我们并不真去打开关系，因此即使某个关系已
 * 被完全删除也不会失败——只会暂时取得一个不冲
 * 突的锁。
 */
static void
AcquirePlannerLocks(List *stmt_list, bool acquire)
{
	ListCell   *lc;

	foreach(lc, stmt_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->commandType == CMD_UTILITY)
		{
			/* 忽略实用语句，除非其中含 Query */
			query = UtilityContainsQuery(query->utilityStmt);
			if (query)
				ScanQueryForLocks(query, acquire);
			continue;
		}

		ScanQueryForLocks(query, acquire);
	}
}

/*
 * ScanQueryForLocks：递归扫描 Query 供 AcquirePlannerLocks。
 */
static void
ScanQueryForLocks(Query *parsetree, bool acquire)
{
	ListCell   *lc;

	/* 不应在实用命令上被调用 */
	Assert(parsetree->commandType != CMD_UTILITY);

	/*
	 * 首先处理当前查询层的 RTEs。
	 */
	foreach(lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* 取得或释放合适类型的锁 */
				if (acquire)
					LockRelationOid(rte->relid, rte->rellockmode);
				else
					UnlockRelationOid(rte->relid, rte->rellockmode);
				break;

			case RTE_SUBQUERY:
				/* 若是视图，必须锁定/解锁之 */
				if (OidIsValid(rte->relid))
				{
					if (acquire)
						LockRelationOid(rte->relid, rte->rellockmode);
					else
						UnlockRelationOid(rte->relid, rte->rellockmode);
				}
				/* 递归进入 FROM 子查询 */
				ScanQueryForLocks(rte->subquery, acquire);
				break;

			default:
				/* 忽略其他类型的 RTEs */
				break;
		}
	}

	/* 递归进入 WITH 子查询 */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

		ScanQueryForLocks(castNode(Query, cte->ctequery), acquire);
	}

	/*
	 * 也要递归进入 sublink 子查询。不过 rtable 和
	 * cteList 已处理过。
	 */
	if (parsetree->hasSubLinks)
	{
		query_tree_walker(parsetree, ScanQueryWalker,
						  (void *) &acquire,
						  QTW_IGNORE_RC_SUBQUERIES);
	}
}

/*
 * ScanQueryForLocks 的 sublink 子查询遍历器
 */
static bool
ScanQueryWalker(Node *node, bool *acquire)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* 做该做的事 */
		ScanQueryForLocks(castNode(Query, sub->subselect), *acquire);
		/* 继续处理 SubLink 的左参数 */
	}

	/*
	 * 绝不递归进入 Query 节点，因为 ScanQueryForLocks 已
	 * 替我们处理子选择的子选择了。
	 */
	return expression_tree_walker(node, ScanQueryWalker,
								  (void *) acquire);
}

/*
 * PlanCacheComputeResultDesc：给定分析重写后的
 * Query 列表，确定它将产生的结果 tupledesc。若执行
 * 不返回元组则返回 NULL。
 *
 * 注意：结果会被创建或复制进当前内存上下文。
 */
static TupleDesc
PlanCacheComputeResultDesc(List *stmt_list)
{
	Query	   *query;

	switch (ChoosePortalStrategy(stmt_list))
	{
		case PORTAL_ONE_SELECT:
		case PORTAL_ONE_MOD_WITH:
			query = linitial_node(Query, stmt_list);
			return ExecCleanTypeFromTL(query->targetList);

		case PORTAL_ONE_RETURNING:
			query = QueryListGetPrimaryStmt(stmt_list);
			Assert(query->returningList);
			return ExecCleanTypeFromTL(query->returningList);

		case PORTAL_UTIL_SELECT:
			query = linitial_node(Query, stmt_list);
			Assert(query->utilityStmt);
			return UtilityTupleDescriptor(query->utilityStmt);

		case PORTAL_MULTI_QUERY:
			/* 不会返回元组 */
			break;
	}
	return NULL;
}

/*
 * PlanCacheRelCallback
 *		relcache 失效回调函数
 *
 * 使提及给定关系的全部计划失效；若 relid 是
 * InvalidOid 则全部计划均失效。
 */
static void
PlanCacheRelCallback(Datum arg, Oid relid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);

		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		/* 若已失效则无事可做 */
		if (!plansource->is_valid)
			continue;

		/* 若解析/规划反正是空操作则绝不变更 */
		if (!StmtPlanRequiresRevalidation(plansource))
			continue;

		/*
		 * 检查重写查询树的依赖列表。
		 */
		if ((relid == InvalidOid) ? plansource->relationOids != NIL :
			list_member_oid(plansource->relationOids, relid))
		{
			/* 使查询树和通用计划失效 */
			plansource->is_valid = false;
			if (plansource->gplan)
				plansource->gplan->is_valid = false;
		}

		/*
		 * 通用计划若有，其依赖可能多于查询树，也得
		 * 检查它。
		 */
		if (plansource->gplan && plansource->gplan->is_valid)
		{
			ListCell   *lc;

			foreach(lc, plansource->gplan->stmt_list)
			{
				PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);

				if (plannedstmt->commandType == CMD_UTILITY)
					continue;	/* 忽略实用语句 */
				if ((relid == InvalidOid) ? plannedstmt->relationOids != NIL :
					list_member_oid(plannedstmt->relationOids, relid))
				{
					/* 仅使通用计划失效 */
					plansource->gplan->is_valid = false;
					break;		/* 扫描完毕 */
				}
			}
		}
	}

	/* 同样检查缓存的表达式 */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		/* 若已失效则无事可做 */
		if (!cexpr->is_valid)
			continue;

		if ((relid == InvalidOid) ? cexpr->relationOids != NIL :
			list_member_oid(cexpr->relationOids, relid))
		{
			cexpr->is_valid = false;
		}
	}
}

/*
 * PlanCacheObjectCallback
 *		PROCOID 与 TYPEOID 缓存的 syscache 失效回调函数
 *
 * 使提及具有指定哈希值对象的全部计划失效；若
 * hashvalue == 0 则使本缓存任何成员的计划失效。
 */
static void
PlanCacheObjectCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	dlist_iter	iter;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);
		ListCell   *lc;

		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		/* 若已失效则无事可做 */
		if (!plansource->is_valid)
			continue;

		/* 若解析/规划反正是空操作则绝不变更 */
		if (!StmtPlanRequiresRevalidation(plansource))
			continue;

		/*
		 * 检查重写查询树的依赖列表。
		 */
		foreach(lc, plansource->invalItems)
		{
			PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

			if (item->cacheId != cacheid)
				continue;
			if (hashvalue == 0 ||
				item->hashValue == hashvalue)
			{
				/* 使查询树和通用计划失效 */
				plansource->is_valid = false;
				if (plansource->gplan)
					plansource->gplan->is_valid = false;
				break;
			}
		}

		/*
		 * 通用计划若有，其依赖可能多于查询树，也得
		 * 检查它。
		 */
		if (plansource->gplan && plansource->gplan->is_valid)
		{
			foreach(lc, plansource->gplan->stmt_list)
			{
				PlannedStmt *plannedstmt = lfirst_node(PlannedStmt, lc);
				ListCell   *lc3;

				if (plannedstmt->commandType == CMD_UTILITY)
					continue;	/* 忽略实用语句 */
				foreach(lc3, plannedstmt->invalItems)
				{
					PlanInvalItem *item = (PlanInvalItem *) lfirst(lc3);

					if (item->cacheId != cacheid)
						continue;
					if (hashvalue == 0 ||
						item->hashValue == hashvalue)
					{
						/* 仅使通用计划失效 */
						plansource->gplan->is_valid = false;
						break;	/* 扫描完毕 */
					}
				}
				if (!plansource->gplan->is_valid)
					break;		/* 扫描完毕 */
			}
		}
	}

	/* 同样检查缓存的表达式 */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);
		ListCell   *lc;

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		/* 若已失效则无事可做 */
		if (!cexpr->is_valid)
			continue;

		foreach(lc, cexpr->invalItems)
		{
			PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

			if (item->cacheId != cacheid)
				continue;
			if (hashvalue == 0 ||
				item->hashValue == hashvalue)
			{
				cexpr->is_valid = false;
				break;
			}
		}
	}
}

/*
 * PlanCacheSysCallback
 *		其他缓存的 syscache 失效回调函数
 *
 * 就让全部失效。
 */
static void
PlanCacheSysCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	ResetPlanCache();
}

/*
 * ResetPlanCache：全缓存计划失效。
 */
void
ResetPlanCache(void)
{
	dlist_iter	iter;

	dlist_foreach(iter, &saved_plan_list)
	{
		CachedPlanSource *plansource = dlist_container(CachedPlanSource,
													   node, iter.cur);

		Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

		/* 若已失效则无事可做 */
		if (!plansource->is_valid)
			continue;

		/*
		 * 我们*绝不*把事务控制语句标为失效，尤其
		 * ROLLBACK，因为它们可能要在无法重新验证
		 * 的已中止事务中执行（参见 bug #5269）。
		 * 总之，对新一轮语法分析/重写/规划必然给
		 * 出相同结果的语句来说，使之失效毫无意义。
		 * results.
		 */
		if (!StmtPlanRequiresRevalidation(plansource))
			continue;

		plansource->is_valid = false;
		if (plansource->gplan)
			plansource->gplan->is_valid = false;
	}

	/* 同样使缓存的表达式失效 */
	dlist_foreach(iter, &cached_expression_list)
	{
		CachedExpression *cexpr = dlist_container(CachedExpression,
												  node, iter.cur);

		Assert(cexpr->magic == CACHEDEXPR_MAGIC);

		cexpr->is_valid = false;
	}
}
