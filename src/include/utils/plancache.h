/*-------------------------------------------------------------------------
 *
 * plancache.h
 *	  计划缓存定义。
 *
 * 注释见 plancache.c。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/plancache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCACHE_H
#define PLANCACHE_H

#include "access/tupdesc.h"
#include "lib/ilist.h"
#include "nodes/params.h"
#include "tcop/cmdtag.h"
#include "utils/queryenvironment.h"
#include "utils/resowner.h"


/*  前向声明，避免在此包含 parsenodes.h  */
struct RawStmt;

/*  plan_cache_mode 的可能取值  */
typedef enum
{
	PLAN_CACHE_MODE_AUTO,
	PLAN_CACHE_MODE_FORCE_GENERIC_PLAN,
	PLAN_CACHE_MODE_FORCE_CUSTOM_PLAN
}			PlanCacheMode;

/*  GUC 参数  */
extern PGDLLIMPORT int plan_cache_mode;

#define CACHEDPLANSOURCE_MAGIC		195726186
#define CACHEDPLAN_MAGIC			953717834
#define CACHEDEXPR_MAGIC			838275847

/*
 * CachedPlanSource（或许更宜称 CachedQuery）表示我们
 * 预期会多次使用的 SQL 查询。它存储查询源文本、原始
 * 解析树，以及分析并重写后的查询树及相关附属数据。
 * 若 DDL 影响查询所用对象，可触发缓存失效。那时我们
 * 丢弃分析并重写后的查询树，下次需要时再重建。
 * 见上。
 *
 * 实际执行计划由 CachedPlan 表示，在需要执行查询时自
 * CachedPlanSource 导出。计划可以是通用的（适用于任意
 * 计划参数集）或定制的（针对特定参数集）。plancache.c
 * 含有对每次执行选择何种方式的逻辑。若使用通用缓存
 * 计划，则意在跨多次执行复用，故调用方须始终把
 * CachedPlan 视为只读。
 * 见上。
 *
 * 一旦成功构建并"保存"，CachedPlanSources 通常贯穿
 * 后端生命周期，虽也可显式删除。CachedPlans 按引用
 * 计数，最后一处引用释放后自动消失。CachedPlan 可以
 * 比创建它的 CachedPlanSource 活得更久。
 * 见上。
 *
 * 未保存的 CachedPlanSource 可用于生成计划，但住在
 * 临时存储中，不会因共享失效事件而更新。
 * 见上.
 *
 * 由已保存 CachedPlanSource 做出的 CachedPlan 同样在永久
 * 存储中，故为免泄漏，对其的引用计数引用须保存在永久
 * 数据结构或 ResourceOwners 中。由未保存源做出的
 * CachedPlan 住在调用方内存上下文之子中，对其
 * 引用不应长于该上下文。（那种情况下引用计数有些
 * 形式化，但若可提早丢弃 CachedPlan 仍有用。）
 * 见上。
 *
 * CachedPlanSource 有两个关联内存上下文：一个存放结构体
 * 本身、查询源文本与原始解析树；另一个存放重写后的
 * 查询树及相关数据。便于失效时丢弃查询树。
 * 见上。
 *
 * 有些调用方即使对无保存必要的一次性查询也想用
 * CachedPlan API。故支持不做数据拷贝或失效检查的
 * 一次性变体。此时无独立内存上下文：CachedPlanSource
 * 结构体及全部附属数据住在调用方 CurrentMemoryContext，
 * 除清空整个上下文外无法单独释放内存。一次性计划
 * 一律视为未保存。
 * 见上。
 *
 * 注：commandTag 所引串非附属存储；假定为编译期常量
 * 串。与门户同，当且仅当原始查询串(重写前)为空
 * 串时 commandTag 为 NULL。
 * 见上。
 */
typedef struct CachedPlanSource
{
	int			magic;			/*  应等于 CACHEDPLANSOURCE_MAGIC  */
	struct RawStmt *raw_parse_tree; /*  raw_parser() 输出或 NULL  */
	const char *query_string;	/*  查询源文本  */
	CommandTag	commandTag;		/*  略  */
	Oid		   *param_types;	/*  参数类型 OIDs 数组，或 NULL  */
	int			num_params;		/*  param_types 数组长度  */
	ParserSetupHook parserSetup;	/*  另一种参数规格方法  */
	void	   *parserSetupArg;
	int			cursor_options; /*  规划所用游标选项  */
	bool		fixed_result;	/*  禁止结果 tupdesc 变更？  */
	TupleDesc	resultDesc;		/*  结果类型；NULL=不返回元组  */
	MemoryContext context;		/*  容纳以上各项的上下文  */
	/*  下列字段描述当前分析并重写后的查询树：  */
	List	   *query_list;		/*  Query 节点列表，无效则为 NIL  */
	List	   *relationOids;	/*  查询所依赖关系的 OIDs  */
	List	   *invalItems;		/*  其他依赖，作 PlanInvalItems  */
	struct OverrideSearchPath *search_path; /*  解析与规划所用的
											 * search_path  */
	MemoryContext query_context;	/* context holding the above, or NULL */
	Oid			rewriteRoleId;	/*  为之重写的角色 ID  */
	bool		rewriteRowSecurity; /*  重写所用 row_security  */
	bool		dependsOnRLS;	/*  重写查询是否专属于上述？  */
	/*  若有通用计划，这是指向它的引用计数链接：  */
	struct CachedPlan *gplan;	/*  通用计划，无效则为 NULL  */
	/*  状态标志：  */
	bool		is_oneshot;		/*  一次性计划？  */
	bool		is_complete;	/*  已做完 CompleteCachedPlan？  */
	bool		is_saved;		/*  CachedPlanSource 已"保存"？  */
	bool		is_valid;		/*  query_list 当前是否有效？  */
	int			generation;		/*  每次创建计划时递增  */
	/*  若已保存，则 CachedPlanSource 是全局列表成员  */
	dlist_node	node;			/*  链，若 is_saved  */
	/*  用于决定用定制还是通用计划的状态：  */
	double		generic_cost;	/*  通用计划代价，未知则为 -1  */
	double		total_custom_cost;	/*  迄今定制计划代价合计  */
	int64		num_custom_plans;	/*  计入合计的定制计划数  */
	int64		num_generic_plans;	/*  通用计划数  */
} CachedPlanSource;

/*
 * CachedPlan 表示自 CachedPlanSource 导出的执行计划。
 * 引用计数含来自父 CachedPlanSource 的链接（若有）以及
 * 任何活动计划执行，故恰好在 refcount 归零时可丢弃。
 * 结构体本身与附属数据都住在 context 字段所指的
 * 上下文中，便于释放不再需要的缓存计划。(但若
 * is_oneshot 为真，该上下文并非专属 CachedPlan，
 * 故无法释放。）
 * 见上。
 */
typedef struct CachedPlan
{
	int			magic;			/*  应等于 CACHEDPLAN_MAGIC  */
	List	   *stmt_list;		/*  PlannedStmts 表  */
	bool		is_oneshot;		/*  一次性计划？  */
	bool		is_saved;		/*  CachedPlan 在长寿上下文中？  */
	bool		is_valid;		/*  stmt_list 当前是否有效？  */
	Oid			planRoleId;		/*  创建计划的角色 ID  */
	bool		dependsOnRole;	/*  计划专属于该角色？  */
	TransactionId saved_xmin;	/*  若有效，当 TransactionXmin
								 * 变化则重规划  */
	int			generation;		/*  本计划对应的父代世代号  */
	int			refcount;		/*  本结构存活引用计数  */
	MemoryContext context;		/*  容纳本 CachedPlan 的上下文 */
} CachedPlan;

/*
 * CachedExpression 是缓存独立标量表达式已规划形式的
 * 低开销机制。此类表达式通常不受缓存失效事件影响，
 * 但也可能发生，例如因替换了被内联进表达式的 SQL
 * 函数。plancache 负责存储表达式树并在失效时标为
 * 无效，但调用方须察觉 !is_valid 并丢弃旧表达式而不
 * 再复用。我们不存原始解析树，只存已规划表达式；
 * 这是基于通常在会话期内不必重规划的优化假定。
 * 见上。
 * 见上。
 * 见上。
 */
typedef struct CachedExpression
{
	int			magic;			/*  应等于 CACHEDEXPR_MAGIC  */
	Node	   *expr;			/*  表达式已规划形式  */
	bool		is_valid;		/*  表达式是否仍有效？  */
	/*  其余字段应视为 plancache.c 私有：  */
	List	   *relationOids;	/*  表达式所依赖关系的 OIDs  */
	List	   *invalItems;		/*  其他依赖，作 PlanInvalItems  */
	MemoryContext context;		/*  容纳本 CachedExpression 的上下文 */
	dlist_node	node;			/*  全局 CachedExpressions 列表链接  */
} CachedExpression;


extern void InitPlanCache(void);
extern void ResetPlanCache(void);

extern CachedPlanSource *CreateCachedPlan(struct RawStmt *raw_parse_tree,
										  const char *query_string,
										  CommandTag commandTag);
extern CachedPlanSource *CreateOneShotCachedPlan(struct RawStmt *raw_parse_tree,
												 const char *query_string,
												 CommandTag commandTag);
extern void CompleteCachedPlan(CachedPlanSource *plansource,
							   List *querytree_list,
							   MemoryContext querytree_context,
							   Oid *param_types,
							   int num_params,
							   ParserSetupHook parserSetup,
							   void *parserSetupArg,
							   int cursor_options,
							   bool fixed_result);

extern void SaveCachedPlan(CachedPlanSource *plansource);
extern void DropCachedPlan(CachedPlanSource *plansource);

extern void CachedPlanSetParentContext(CachedPlanSource *plansource,
									   MemoryContext newcontext);

extern CachedPlanSource *CopyCachedPlan(CachedPlanSource *plansource);

extern bool CachedPlanIsValid(CachedPlanSource *plansource);

extern List *CachedPlanGetTargetList(CachedPlanSource *plansource,
									 QueryEnvironment *queryEnv);

extern CachedPlan *GetCachedPlan(CachedPlanSource *plansource,
								 ParamListInfo boundParams,
								 ResourceOwner owner,
								 QueryEnvironment *queryEnv);
extern void ReleaseCachedPlan(CachedPlan *plan, ResourceOwner owner);

extern bool CachedPlanAllowsSimpleValidityCheck(CachedPlanSource *plansource,
												CachedPlan *plan,
												ResourceOwner owner);
extern bool CachedPlanIsSimplyValid(CachedPlanSource *plansource,
									CachedPlan *plan,
									ResourceOwner owner);

extern CachedExpression *GetCachedExpression(Node *expr);
extern void FreeCachedExpression(CachedExpression *cexpr);

#endif							/* PLANCACHE_H */
