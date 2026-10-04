/*-------------------------------------------------------------------------
 *
 * allpaths.c
 *	  Routines to find possible search paths for processing a query
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/allpaths.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/sysattr.h"
#include "access/tsmapi.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "partitioning/partprune.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


/* pushdown_safety_info.unsafeFlags 的位掩码标志 */
#define UNSAFE_HAS_VOLATILE_FUNC		(1 << 0)
#define UNSAFE_HAS_SET_FUNC				(1 << 1)
#define UNSAFE_NOTIN_DISTINCTON_CLAUSE	(1 << 2)
#define UNSAFE_NOTIN_PARTITIONBY_CLAUSE	(1 << 3)
#define UNSAFE_TYPE_MISMATCH			(1 << 4)

/* subquery_is_pushdown_safe 的结果 */
typedef struct pushdown_safety_info
{
	unsigned char *unsafeFlags; /* 该目标列表
								 * 列为限定条件下推不安全的原因位掩码，或若
								 * 没有原因则为 0。 */
	bool		unsafeVolatile; /* 不要下推易变限定条件 */
	bool		unsafeLeaky;	/* 不要下推有泄漏风险的限定条件 */
} pushdown_safety_info;

/* qual_is_pushdown_safe 的返回类型 */
typedef enum pushdown_safe_type
{
	PUSHDOWN_UNSAFE,			/* 把限定条件下推到子查询不安全 */
	PUSHDOWN_SAFE,				/* 把限定条件下推到子查询安全 */
	PUSHDOWN_WINDOWCLAUSE_RUNCOND	/* 不安全，但也许可作为 WindowClause
									 * 运行条件 */
} pushdown_safe_type;

/* 这些参数由 GUC 设置 */
bool		enable_geqo = false;	/* 万一 GUC 没有设置它 */
int			geqo_threshold;
int			min_parallel_table_scan_size;
int			min_parallel_index_scan_size;

/* 供插件在 set_rel_pathlist() 中取得控制权的钩子 */
set_rel_pathlist_hook_type set_rel_pathlist_hook = NULL;

/* 供插件替换 standard_join_search() 的钩子 */
join_search_hook_type join_search_hook = NULL;


static void set_base_rel_consider_startup(PlannerInfo *root);
static void set_base_rel_sizes(PlannerInfo *root);
static void set_base_rel_pathlists(PlannerInfo *root);
static void set_rel_size(PlannerInfo *root, RelOptInfo *rel,
						 Index rti, RangeTblEntry *rte);
static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 Index rti, RangeTblEntry *rte);
static void set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel,
							   RangeTblEntry *rte);
static void create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel);
static void set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte);
static void set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static void set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel,
									 RangeTblEntry *rte);
static void set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
										 RangeTblEntry *rte);
static void set_foreign_size(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
static void set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel,
								 RangeTblEntry *rte);
static void set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
								Index rti, RangeTblEntry *rte);
static void set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
									Index rti, RangeTblEntry *rte);
static void generate_orderedappend_paths(PlannerInfo *root, RelOptInfo *rel,
										 List *live_childrels,
										 List *all_child_pathkeys);
static Path *get_cheapest_parameterized_child_path(PlannerInfo *root,
												   RelOptInfo *rel,
												   Relids required_outer);
static void accumulate_append_subpath(Path *path,
									  List **subpaths,
									  List **special_subpaths);
static Path *get_singleton_append_subpath(Path *path);
static void set_dummy_rel_pathlist(RelOptInfo *rel);
static void set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
								  Index rti, RangeTblEntry *rte);
static void set_function_pathlist(PlannerInfo *root, RelOptInfo *rel,
								  RangeTblEntry *rte);
static void set_values_pathlist(PlannerInfo *root, RelOptInfo *rel,
								RangeTblEntry *rte);
static void set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static void set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
static void set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
										 RangeTblEntry *rte);
static void set_result_pathlist(PlannerInfo *root, RelOptInfo *rel,
								RangeTblEntry *rte);
static void set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static RelOptInfo *make_rel_from_joinlist(PlannerInfo *root, List *joinlist);
static bool subquery_is_pushdown_safe(Query *subquery, Query *topquery,
									  pushdown_safety_info *safetyInfo);
static bool recurse_pushdown_safe(Node *setOp, Query *topquery,
								  pushdown_safety_info *safetyInfo);
static void check_output_expressions(Query *subquery,
									 pushdown_safety_info *safetyInfo);
static void compare_tlist_datatypes(List *tlist, List *colTypes,
									pushdown_safety_info *safetyInfo);
static bool targetIsInAllPartitionLists(TargetEntry *tle, Query *query);
static pushdown_safe_type qual_is_pushdown_safe(Query *subquery, Index rti,
												RestrictInfo *rinfo,
												pushdown_safety_info *safetyInfo);
static void subquery_push_qual(Query *subquery,
							   RangeTblEntry *rte, Index rti, Node *qual);
static void recurse_push_qual(Node *setOp, Query *topquery,
							  RangeTblEntry *rte, Index rti, Node *qual);
static void remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel,
										   Bitmapset *extra_used_attrs);


/*
 * make_one_rel
 *	  找出执行查询的全部可能访问路径，返回一个
 *	  表示查询中全部基关系连接结果的单个关系。
 */
RelOptInfo *
make_one_rel(PlannerInfo *root, List *joinlist)
{
	RelOptInfo *rel;
	Index		rti;
	double		total_pages;

	/* 标记基关系是否在意快速启动计划 */
	set_base_rel_consider_startup(root);

	/*
	 * 为每个基关系计算规模估计和 consider_parallel 标志。
	 */
	set_base_rel_sizes(root);

	/*
	 * 现在应当已经有查询涉及的每张实际表的规模估计，
	 * 也知道哪些表（若有）已因连接移除、
	 * 分区裁剪或约束排除而从查询中删掉。
	 * 因此现在可以计算 total_table_pages。
	 *
	 * 注意这里不会把追加关系重复计算，即使我们不
	 * 特意区分追加关系父项的 RelOptInfos，因为
	 * 父项的页数将为 0。
	 *
	 * XXX 若一张表被自连接，会按出现次数各计一次，
	 * 这也许不对……但这并不完全清楚，
	 * 而且在这里检测自连接很难，所以暂时忽略。
	 */
	total_pages = 0;
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* 对数组做健全性检查 */

		if (IS_DUMMY_REL(brel))
			continue;

		if (IS_SIMPLE_REL(brel))
			total_pages += (double) brel->pages;
	}
	root->total_table_pages = total_pages;

	/*
	 * 为每个基关系生成访问路径。
	 */
	set_base_rel_pathlists(root);

	/*
	 * 为整棵连接树生成访问路径。
	 */
	rel = make_rel_from_joinlist(root, joinlist);

	/*
	 * 结果应当连接查询的全部且仅限基关系加外连接关系。
	 */
	Assert(bms_equal(rel->relids, root->all_query_rels));

	return rel;
}

/*
 * set_base_rel_consider_startup
 *	  为每个基关系项设置 consider_[param_]startup 标志。
 *
 * 目前这里只处理 consider_param_startup；因为
 * consider_startup 的逻辑很简单，而且对每个基
 * 关系都一样，就让 build_simple_rel() 一开始就把该标志初始化
 * 正确。若那段逻辑以后变复杂，大概
 * 最好把它移到这里。
 */
static void
set_base_rel_consider_startup(PlannerInfo *root)
{
	/*
	 * 由于参数化路径只能用在嵌套循环
	 * 连接计划的内侧，通常很少有必要为它们考虑快速启动
	 * 计划。不过，对位于 SEMI
	 * 或 ANTI 连接右侧的关系，快速启动计划可能有用，因为我们反正
	 * 只关心取出一个元组。
	 *
	 * 为尽量少增加规划时间，目前把这一点限制在
	 * 右侧是单个基关系而不是连接的情形；连接关系上
	 * 根本没有把 consider_param_startup 设上的安排。
	 * 也不操心追加关系。costsize.c 对
	 * 嵌套循环半/反连接的计价规则同样不考虑这些情形。
	 */
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			varno;

		if ((sjinfo->jointype == JOIN_SEMI || sjinfo->jointype == JOIN_ANTI) &&
			bms_get_singleton_member(sjinfo->syn_righthand, &varno))
		{
			RelOptInfo *rel = find_base_rel(root, varno);

			rel->consider_param_startup = true;
		}
	}
}

/*
 * set_base_rel_sizes
 *	  为每个基关系项设置规模估计（行数和宽度）。
 *	  同时决定是否为基关系考虑并行路径。
 *
 * 单独再扫一遍基关系来做这件事，以便行数
 * 估计在生成参数化路径时已经可用，也以便
 * 在开始之前每个关系的 consider_parallel 标志都已正确设置，
 * 再去生成路径。
 */
static void
set_base_rel_sizes(PlannerInfo *root)
{
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		RangeTblEntry *rte;

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* 对数组做健全性检查 */

		/* 忽略作为 "其他关系" 的 RTEs */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		rte = root->simple_rte_array[rti];

		/*
		 * 若这个查询总体上允许并行，再看
		 * 这个关系本身是否允许。必须在
		 * set_rel_size() 之前做，因为 (a) 若该关系是继承
		 * 父项，set_append_rel_size() 会使用并也许改变该关系的
		 * consider_parallel 标志，以及 (b) 对某些 RTE 类型，set_rel_size()
		 * 会立刻往下建立路径。
		 */
		if (root->glob->parallelModeOK)
			set_rel_consider_parallel(root, rel, rte);

		set_rel_size(root, rel, rti, rte);
	}
}

/*
 * set_base_rel_pathlists
 *	  找出扫描每个基关系项的全部可用路径。
 *	  会考虑顺序扫描和任何可用索引。
 *	  每条有用路径都挂到其关系的 pathlist 上。
 */
static void
set_base_rel_pathlists(PlannerInfo *root)
{
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* 对数组做健全性检查 */

		/* 忽略作为 "其他关系" 的 RTEs */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		set_rel_pathlist(root, rel, rti, root->simple_rte_array[rti]);
	}
}

/*
 * set_rel_size
 *	  为基关系设置规模估计
 */
static void
set_rel_size(PlannerInfo *root, RelOptInfo *rel,
			 Index rti, RangeTblEntry *rte)
{
	if (rel->reloptkind == RELOPT_BASEREL &&
		relation_excluded_by_constraints(root, rel, rte))
	{
		/*
		 * 已证明因约束排除而不必扫描该关系，
		 * 因此为它建立一条哑路径。这里只对
		 * 普通基关系检查这一点；若是其他关系，约束排除已在
		 * set_append_rel_size() 中检查过。
		 *
		 * 这种情况下，我们立刻把该关系的路径建好，
		 * 而不是留给 set_rel_pathlist 去做。
		 * 这是因为除了给它赋一条哑路径之外，
		 * 没有把关系标成哑的约定。
		 */
		set_dummy_rel_pathlist(rel);
	}
	else if (rte->inh)
	{
		/* 这是 "追加关系"，按相应方式处理 */
		set_append_rel_size(root, rel, rti, rte);
	}
	else
	{
		switch (rel->rtekind)
		{
			case RTE_RELATION:
				if (rte->relkind == RELKIND_FOREIGN_TABLE)
				{
					/* 外部表 */
					set_foreign_size(root, rel, rte);
				}
				else if (rte->relkind == RELKIND_PARTITIONED_TABLE)
				{
					/*
					 * 若被要求用 ONLY 扫描分区表，会走到这里。
					 * 这种情况下不应当扫描任何
					 * 分区，因此把它标成哑关系。
					 */
					set_dummy_rel_pathlist(rel);
				}
				else if (rte->tablesample != NULL)
				{
					/* 采样关系 */
					set_tablesample_rel_size(root, rel, rte);
				}
				else
				{
					/* 普通关系 */
					set_plain_rel_size(root, rel, rte);
				}
				break;
			case RTE_SUBQUERY:

				/*
				 * 子查询不支持在参数化路径
				 * 和未参数化路径之间做选择，因此直接
				 * 立刻建立它们的路径。
				 */
				set_subquery_pathlist(root, rel, rti, rte);
				break;
			case RTE_FUNCTION:
				set_function_size_estimates(root, rel);
				break;
			case RTE_TABLEFUNC:
				set_tablefunc_size_estimates(root, rel);
				break;
			case RTE_VALUES:
				set_values_size_estimates(root, rel);
				break;
			case RTE_CTE:

				/*
				 * CTEs 不支持在参数化路径
				 * 和未参数化路径之间做选择，因此直接立刻建立它们的
				 * 路径。
				 */
				if (rte->self_reference)
					set_worktable_pathlist(root, rel, rte);
				else
					set_cte_pathlist(root, rel, rte);
				break;
			case RTE_NAMEDTUPLESTORE:
				/* 不妨立刻把路径建好 */
				set_namedtuplestore_pathlist(root, rel, rte);
				break;
			case RTE_RESULT:
				/* 不妨立刻把路径建好 */
				set_result_pathlist(root, rel, rte);
				break;
			default:
				elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
				break;
		}
	}

	/*
	 * 我们坚持所有非哑关系都有非零的行数估计。
	 */
	Assert(rel->rows > 0 || IS_DUMMY_REL(rel));
}

/*
 * set_rel_pathlist
 *	  为基关系建立访问路径
 */
static void
set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
				 Index rti, RangeTblEntry *rte)
{
	if (IS_DUMMY_REL(rel))
	{
		/* 已经证明该关系为空，因此没有更多事可做 */
	}
	else if (rte->inh)
	{
		/* 这是 "追加关系"，按相应方式处理 */
		set_append_rel_pathlist(root, rel, rti, rte);
	}
	else
	{
		switch (rel->rtekind)
		{
			case RTE_RELATION:
				if (rte->relkind == RELKIND_FOREIGN_TABLE)
				{
					/* 外部表 */
					set_foreign_pathlist(root, rel, rte);
				}
				else if (rte->tablesample != NULL)
				{
					/* 采样关系 */
					set_tablesample_rel_pathlist(root, rel, rte);
				}
				else
				{
					/* 普通关系 */
					set_plain_rel_pathlist(root, rel, rte);
				}
				break;
			case RTE_SUBQUERY:
				/* 子查询 --- 在 set_rel_size 期间已完全处理 */
				break;
			case RTE_FUNCTION:
				/* RangeFunction（范围函数） */
				set_function_pathlist(root, rel, rte);
				break;
			case RTE_TABLEFUNC:
				/* 表函数 */
				set_tablefunc_pathlist(root, rel, rte);
				break;
			case RTE_VALUES:
				/* Values 列表 */
				set_values_pathlist(root, rel, rte);
				break;
			case RTE_CTE:
				/* CTE 引用 --- 在 set_rel_size 期间已完全处理 */
				break;
			case RTE_NAMEDTUPLESTORE:
				/* tuplestore 引用 --- 在 set_rel_size 期间已完全处理 */
				break;
			case RTE_RESULT:
				/* 简单 Result --- 在 set_rel_size 期间已完全处理 */
				break;
			default:
				elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
				break;
		}
	}

	/*
	 * 允许插件对本基关系的路径集合做加工。
	 * 它可以调用 add_path() 增加新路径（如 CustomPaths），
	 * 若感知并行则调用 add_partial_path()。它也可以
	 * 删除或修改核心代码加入的路径。
	 */
	if (set_rel_pathlist_hook)
		(*set_rel_pathlist_hook) (root, rel, rti, rte);

	/*
	 * 若这是基关系，通常应当考虑收集我们可能
	 * 为它建立的任何部分路径。必须在调用
	 * set_rel_pathlist_hook 之后做，否则它无法把要包括进来的部分路径
	 * 加到这里。
	 *
	 * 不过，若这是继承子项，就跳过。否则，最终
	 * 可能有非常多的收集节点，每个都试图抓
	 * 自己的工作进程池。改为考虑为父追加关系
	 * 收集部分路径。
	 *
	 * 另外，若这是最顶层的扫描/连接关系，就把收集推迟到
	 * 最终的扫描/连接目标列表可用时（见 grouping_planner）。
	 */
	if (rel->reloptkind == RELOPT_BASEREL &&
		!bms_equal(rel->relids, root->all_query_rels))
		generate_useful_gather_paths(root, rel, false);

	/* 现在找出该关系各条路径中最便宜的 */
	set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
	debug_print_rel(root, rel);
#endif
}

/*
 * set_plain_rel_size
 *	  为普通关系设置规模估计（无子查询、无继承）
 */
static void
set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/*
	 * 测试该关系的任何部分索引是否适用。必须先做这件事，
	 * 因为部分唯一索引会影响规模估计。
	 */
	check_index_predicates(root, rel);

	/* 用估计的输出行数、宽度等标记该关系 */
	set_baserel_size_estimates(root, rel);
}

/*
 * 若该关系有可能从工作进程内部被扫描，就把
 * 其 consider_parallel 标志设上。
 */
static void
set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte)
{
	/*
	 * 该标志先前已初始化为 false，因此一旦清楚无法
	 * 安全地把它设上，就可以直接返回。
	 */
	Assert(!rel->consider_parallel);

	/* 若整个查询不允许并行，就不要调用这个。 */
	Assert(root->glob->parallelModeOK);

	/* 只应当对基关系和追加关系的子项调用这个。 */
	Assert(IS_SIMPLE_REL(rel));

	/* 基于 rtekind 的各项检查。 */
	switch (rte->rtekind)
	{
		case RTE_RELATION:

			/*
			 * 目前，并行工作进程无法访问主导进程的临时
			 * 表。若在查询开始时写出它的全部
			 * 本地缓冲区并且此后不再改动
			 * （也许可以允许提示位变化），并且教会
			 * 工作进程去读它们，也许可以放宽这一点。写出大量
			 * 临时缓冲区可能很贵，而且我们现在反正
			 * 也没有其余必要的基础设施。因此
			 * 目前若看到临时表就放弃。
			 */
			if (get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
				return;

			/*
			 * 若采样函数及其参数是安全的，
			 * 表采样可以下推到工作进程。
			 */
			if (rte->tablesample != NULL)
			{
				char		proparallel = func_parallel(rte->tablesample->tsmhandler);

				if (proparallel != PROPARALLEL_SAFE)
					return;
				if (!is_parallel_safe(root, (Node *) rte->tablesample->args))
					return;
			}

			/*
			 * 询问 FDWs 能否支持在工作进程内执行 ForeignScan。
			 * 多数时候答案是否。例如，
			 * 若 FDW 的性质是与远程服务器打开一条 TCP
			 * 连接，每个并行工作进程最终都会有
			 * 一条单独的连接，而这些连接也许不能在工作进程和主导进程之间
			 * 得到适当协调。
			 */
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				Assert(rel->fdwroutine);
				if (!rel->fdwroutine->IsForeignScanParallelSafe)
					return;
				if (!rel->fdwroutine->IsForeignScanParallelSafe(root, rel, rte))
					return;
			}

			/*
			 * 追加关系还有额外考虑，将在
			 * set_append_rel_size 和 set_append_rel_pathlist 中处理。
			 * 目前只根据该关系自己的限定条件和目标列表
			 * 设置 consider_parallel。
			 */
			break;

		case RTE_SUBQUERY:

			/*
			 * 在并行工作进程中扫描 FROM 中的子查询
			 * （有别于 SubPlan 或 InitPlan）本身没有问题。
			 * 若该子查询碰巧没有任何并行安全路径，
			 * 把它标成 consider_parallel 也不会改变什么，
			 * 但对普通表也是如此。必须根据
			 * 该关系自己的限定条件和目标列表设置 consider_parallel，
			 * 这样若子查询路径是并行安全的，但贴上去的限定条件和
			 * 投影不是，就会正确地把
			 * SubqueryScanPath 标成非并行安全。（注意
			 * set_subquery_pathlist() 也许会把其中一些限定条件下推
			 * 到子查询内部，但这并不改变什么。）
			 *
			 * 无法把含有 LIMIT/OFFSET 的子选择下推到工作进程，因为
			 * 没有保证行顺序完全
			 * 确定，应用 LIMIT/OFFSET 会导致
			 * 顶层结果不一致。（在某些结果有序的情形里，
			 * 可以放宽这个限制。但目前
			 * 似乎不值得为此多花力气。）
			 */
			{
				Query	   *subquery = castNode(Query, rte->subquery);

				if (limit_needed(subquery))
					return;
			}
			break;

		case RTE_JOIN:
			/* 不应当发生；这里只考虑基关系。 */
			Assert(false);
			return;

		case RTE_FUNCTION:
			/* 检查受并行限制的函数。 */
			if (!is_parallel_safe(root, (Node *) rte->functions))
				return;
			break;

		case RTE_TABLEFUNC:
			/* 非并行安全 */
			return;

		case RTE_VALUES:
			/* 检查受并行限制的函数。 */
			if (!is_parallel_safe(root, (Node *) rte->values_lists))
				return;
			break;

		case RTE_CTE:

			/*
			 * CTE 元组存储不在并行工作进程之间共享，因此
			 * 强制所有 CTE 扫描都在主导进程中发生。另外，填充
			 * 该 CTE 需要执行一条工作进程中没有的子计划，
			 * 它也许受并行限制，并且必须
			 * 只执行一次。
			 */
			return;

		case RTE_NAMEDTUPLESTORE:

			/*
			 * 元组存储无法共享，至少在没有更多
			 * 基础设施来支持这一点之前是这样。
			 */
			return;

		case RTE_RESULT:
			/* RESULT RTEs 本身没有问题。 */
			break;
	}

	/*
	 * 若 baserestrictinfo 里有任何受并行限制的东西，就
	 * 放弃对该关系的并行访问。可以考虑
	 * 改为把受限制的限定条件推迟到计划树中全部
	 * 并行之上再应用，但不清楚
	 * 那在很多情形里是否划算，而且要让
	 * 外连接子句正确工作也许很棘手。它还可能打破等价
	 * 类。
	 */
	if (!is_parallel_safe(root, (Node *) rel->baserestrictinfo))
		return;

	/*
	 * 同样，若该关系的输出不是并行安全的，就放弃。
	 * （通常它们只是 Vars，但有时不是。）
	 */
	if (!is_parallel_safe(root, (Node *) rel->reltarget->exprs))
		return;

	/* 找到可用的了。 */
	rel->consider_parallel = true;
}

/*
 * set_plain_rel_pathlist
 *	  为普通关系建立访问路径（无子查询、无继承）
 */
static void
set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * 不支持把连接子句推进顺序扫描的限定条件，但它
	 * 仍可能因目标列表中的 LATERAL 引用而需要
	 * 参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 考虑顺序扫描 */
	add_path(rel, create_seqscan_path(root, rel, required_outer, 0));

	/* 若合适，考虑并行顺序扫描 */
	if (rel->consider_parallel && required_outer == NULL)
		create_plain_partial_paths(root, rel);

	/* 考虑索引扫描 */
	create_index_paths(root, rel);

	/* 考虑 TID 扫描 */
	create_tidscan_paths(root, rel);
}

/*
 * create_plain_partial_paths
 *	  为普通关系的并行扫描建立部分访问路径
 */
static void
create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel)
{
	int			parallel_workers;

	parallel_workers = compute_parallel_worker(rel, rel->pages, -1,
											   max_parallel_workers_per_gather);

	/* 若任何限制被设成零，用户就不想要并行扫描。 */
	if (parallel_workers <= 0)
		return;

	/* 基于并行顺序扫描加入一条无序部分路径。 */
	add_partial_path(rel, create_seqscan_path(root, rel, NULL, parallel_workers));
}

/*
 * set_tablesample_rel_size
 *	  为采样关系设置规模估计
 */
static void
set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	TableSampleClause *tsc = rte->tablesample;
	TsmRoutine *tsm;
	BlockNumber pages;
	double		tuples;

	/*
	 * 测试该关系的任何部分索引是否适用。必须先做这件事，
	 * 因为部分唯一索引会影响规模估计。
	 */
	check_index_predicates(root, rel);

	/*
	 * 调用采样方法的估计函数，估计它将读取的页数
	 * 和将返回的元组数。（注意：
	 * 假定该函数返回的值是合理的。）
	 */
	tsm = GetTsmRoutine(tsc->tsmhandler);
	tsm->SampleScanGetSampleSize(root, rel, tsc->args,
								 &pages, &tuples);

	/*
	 * 目前，因为对该关系只考虑 SampleScan 路径，
	 * 直接覆盖整个关系的页数和元组估计是可以的。
	 * 若以后为采样关系考虑多种路径类型，
	 * 就需要更复杂的处理。
	 */
	rel->pages = pages;
	rel->tuples = tuples;

	/* 用估计的输出行数、宽度等标记该关系 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_tablesample_rel_pathlist
 *	  为采样关系建立访问路径
 */
static void
set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;
	Path	   *path;

	/*
	 * 不支持把连接子句推进采样扫描的限定条件，
	 * 但它仍可能因目标列表或 TABLESAMPLE 参数中的 LATERAL 引用
	 * 而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 考虑采样扫描 */
	path = create_samplescan_path(root, rel, required_outer);

	/*
	 * 若采样方法不支持可重复扫描，就必须避免
	 * 会多次扫描该关系的计划。理想情况下，只是
	 * 避免把该关系放在嵌套循环连接的内侧；但把这种
	 * 考虑加进规划器，为了支持二流采样方法的不常见用法，
	 * 似乎太复杂了。改为
	 * 若查询有做不安全连接的风险，就把
	 * SampleScan 包进一个 Materialize 节点。可以通过
	 * 数 all_query_rels 的成员来检查连接（注意这会正确地
	 * 把继承树计为单个关系）。若在子查询内部，
	 * 不容易检查外层查询是否可能发生连接，因此
	 * 就假定有可能。
	 *
	 * 与这里的其他测试相比，GetTsmRoutine 相对较贵，
	 * 因此最后再检查 repeatable_across_scans，即使这有点怪。
	 */
	if ((root->query_level > 1 ||
		 bms_membership(root->all_query_rels) != BMS_SINGLETON) &&
		!(GetTsmRoutine(rte->tablesample->tsmhandler)->repeatable_across_scans))
	{
		path = (Path *) create_material_path(rel, path);
	}

	add_path(rel, path);

	/* 至少目前，没有其他路径要考虑 */
}

/*
 * set_foreign_size
 *		为外部表 RTE 设置规模估计
 */
static void
set_foreign_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* 用估计的输出行数、宽度等标记该关系 */
	set_foreign_size_estimates(root, rel);

	/* 若 FDW 能调整规模估计，就让它调整 */
	rel->fdwroutine->GetForeignRelSize(root, rel, rte->relid);

	/* ……但不让它把行数估计设成零 */
	rel->rows = clamp_row_est(rel->rows);

	/*
	 * 另外，确保 rel->tuples 相对 rel->rows 不是离谱的。
	 * 尤其是，若 pg_class.reltuples 含有 -1 而
	 * FDW 没有做任何事来替换它，这能保证健全。
	 */
	rel->tuples = Max(rel->tuples, rel->rows);
}

/*
 * set_foreign_pathlist
 *		为外部表 RTE 建立访问路径
 */
static void
set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* 调用 FDW 的 GetForeignPaths 函数来生成 path(s) */
	rel->fdwroutine->GetForeignPaths(root, rel, rte->relid);
}

/*
 * set_append_rel_size
 *	  为简单 "追加关系" 设置规模估计
 *
 * 传入的关系和 RTE 代表整个追加关系。该
 * 关系的内容由把各个成员关系的输出追加在一起而算出。
 * 注意在非分区继承
 * 情形里，第一个成员关系其实就是父 RTE 中提到的
 * 同一张表……但它有不同的 RTE 和 RelOptInfo。这是
 * 好事，因为它们的输出规模并不相同。
 */
static void
set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
					Index rti, RangeTblEntry *rte)
{
	int			parentRTindex = rti;
	bool		has_live_children;
	double		parent_rows;
	double		parent_size;
	double	   *parent_attrsizes;
	int			nattrs;
	ListCell   *l;

	/* 防止继承树过深导致栈溢出。 */
	check_stack_depth();

	Assert(IS_SIMPLE_REL(rel));

	/*
	 * 若这是分区基关系，就设置 consider_partitionwise_join
	 * 标志；目前，仅当其目标列表不含整行 Var 时，
	 * 才考虑与该基关系做按分区连接。
	 */
	if (enable_partitionwise_join &&
		rel->reloptkind == RELOPT_BASEREL &&
		rte->relkind == RELKIND_PARTITIONED_TABLE &&
		bms_is_empty(rel->attr_needed[InvalidAttrNumber - rel->min_attr]))
		rel->consider_partitionwise_join = true;

	/*
	 * 初始化，以便计算整个追加关系的规模估计。
	 *
	 * 宽度估计的做法是按不同子关系的行数
	 * 比例加权它们的宽度。这是合理的，因为
	 * 使用宽度估计主要是为了计算若必须排序或哈希该关系时的总
	 * "占地"。为此，把
	 * 总的等效规模（用 "double" 算术）加起来，再除以
	 * 总行数估计。对总关系
	 * 宽度和每个属性分别做这件事。
	 *
	 * 注意：若考虑改变这段逻辑，要当心子关系可能
	 * 行数和/或宽度为零，若它们被约束排除了。
	 */
	has_live_children = false;
	parent_rows = 0;
	parent_size = 0;
	nattrs = rel->max_attr - rel->min_attr + 1;
	parent_attrsizes = (double *) palloc0(nattrs * sizeof(double));

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;
		List	   *childrinfos;
		ListCell   *parentvars;
		ListCell   *childvars;
		ListCell   *lc;

		/* append_rel_list 含有全部追加关系；忽略其他的 */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];

		/*
		 * 子关系的 RelOptInfo 已在
		 * add_other_rels_to_query 期间建立。
		 */
		childrel = find_base_rel(root, childRTindex);
		Assert(childrel->reloptkind == RELOPT_OTHER_MEMBER_REL);

		/* 也许已经证明该子项是哑的。 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/*
		 * 必须把父项的目标列表和限定条件复制到子项，
		 * 并做适当的变量替换。不过，
		 * baserestrictinfo 限定条件在建立
		 * 子项 RelOptInfo 时已经复制/替换过。因此在应用约束排除之前
		 * 不需要任何额外设置。
		 */
		if (relation_excluded_by_constraints(root, childrel, childRTE))
		{
			/*
			 * 不必扫描这个子项，因此可以从
			 * 追加关系中省掉它。
			 */
			set_dummy_rel_pathlist(childrel);
			continue;
		}

		/*
		 * 约束排除失败，因此把父项的连接限定条件和
		 * 目标列表复制到子项，并做适当的变量替换。
		 *
		 * 跳过那些来自能把该关系置空的外连接之上的连接限定条件，
		 * 因为在为子项生成路径时它们没有价值。
		 * 这在处理子关系时省些力气，
		 * 也避开 adjust_appendrel_attrs 中的一个实现限制
		 * （它不能把 nullingrels 应用到非 Var 上）。
		 */
		childrinfos = NIL;
		foreach(lc, rel->joininfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (!bms_overlap(rinfo->clause_relids, rel->nulling_relids))
				childrinfos = lappend(childrinfos,
									  adjust_appendrel_attrs(root,
															 (Node *) rinfo,
															 1, &appinfo));
		}
		childrel->joininfo = childrinfos;

		/*
		 * 接下来是子项的目标列表。
		 *
		 * 注意：所得 childrel->reltarget->exprs 可能含有任意
		 * 表达式，否则不会出现在关系的目标列表里。
		 * 可能查看追加关系子项的代码必须应付
		 * 这种情况。（通常，关系的目标列表只会包括 Vars 和
		 * PlaceHolderVars。）XXX 我们不费心更新 childrel->reltarget 的代价或宽度
		 * 字段；不清楚那样是否有用。
		 */
		childrel->reltarget->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) rel->reltarget->exprs,
								   1, &appinfo);

		/*
		 * 还必须在 EquivalenceClass 数据
		 * 结构中建立子项条目。若父项
		 * 参与某些等价类连接（因为会想考虑
		 * 对各个子项做内层索引扫描连接），或者父项
		 * 有有用的路径键（因为应当试着建立产生那些排序的 MergeAppend
		 * 路径），就需要这样做。
		 */
		if (rel->has_eclass_joins || has_useful_pathkeys(root, rel))
			add_child_rel_equivalences(root, appinfo, rel, childrel);
		childrel->has_eclass_joins = rel->has_eclass_joins;

		/*
		 * 注意：可以通过 translated_vars 映射变换父项的 attr_needed，
		 * 来为子项的变量计算合适的 attr_needed 数据。
		 * 不过目前没有必要，
		 * 因为 attr_needed 只对基关系检查，不检查
		 * 其他关系。因此就把子项的 attr_needed 留空。
		 */

		/*
		 * 若考虑与父关系做按分区连接，对分区的
		 * 子关系也这样做。
		 *
		 * 注意：这里滥用 consider_partitionwise_join 标志，把它设到
		 * 本身并未分区的子关系上。这样做是为了
		 * 告诉 try_partitionwise_join()：该子关系已足够
		 * 有效，可以作为按分区的输入，即使它后来被
		 * 证明是哑的。（在建好 reltarget 和等价类条目之前它不可用，
		 * 而我们刚刚做完了。）
		 */
		if (rel->consider_partitionwise_join)
			childrel->consider_partitionwise_join = true;

		/*
		 * 若这个查询总体上允许并行，再看
		 * 这个子关系本身是否允许。但若已经
		 * 判定整个追加关系不是并行安全的，
		 * 再考虑这个子项的并行就没有意义。为了
		 * 一致，在为该子项调用 set_rel_size() 之前做这件事。
		 */
		if (root->glob->parallelModeOK && rel->consider_parallel)
			set_rel_consider_parallel(root, childrel, childRTE);

		/*
		 * 计算该子项的规模。
		 */
		set_rel_size(root, childrel, childRTindex, childRTE);

		/*
		 * 约束排除有可能在子查询内部发现矛盾，
		 * 即使上面没有证明出来。若
		 * 是这样，就可以跳过这个子项。
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/* 至少有一个活的子项。 */
		has_live_children = true;

		/*
		 * 若任何活的子项不是并行安全的，就把整个追加关系
		 * 当作非并行安全。将来也许能生成这样的计划：
		 * 一些子项交给工作进程，另一些
		 * 不交；但今天还没有，因此除非整个追加关系都安全，
		 * 否则在其中任何地方考虑部分路径都是浪费。
		 * （在此之前访问过的子关系将在
		 * set_append_rel_pathlist() 中被取消标记。）
		 */
		if (!childrel->consider_parallel)
			rel->consider_parallel = false;

		/*
		 * 从每个活的子项累加规模信息。
		 */
		Assert(childrel->rows > 0);

		parent_rows += childrel->rows;
		parent_size += childrel->reltarget->width * childrel->rows;

		/*
		 * 也累加每列的估计。父列表中的
		 * PlaceHolderVars 不必做任何事。若子表达式不是
		 * Var，或者没有记录它的宽度估计，就不得不退回到
		 * 基于数据类型的估计。
		 *
		 * 按构造，子项的目标列表与父项一一对应。
		 */
		forboth(parentvars, rel->reltarget->exprs,
				childvars, childrel->reltarget->exprs)
		{
			Var		   *parentvar = (Var *) lfirst(parentvars);
			Node	   *childvar = (Node *) lfirst(childvars);

			if (IsA(parentvar, Var) && parentvar->varno == parentRTindex)
			{
				int			pndx = parentvar->varattno - rel->min_attr;
				int32		child_width = 0;

				if (IsA(childvar, Var) &&
					((Var *) childvar)->varno == childrel->relid)
				{
					int			cndx = ((Var *) childvar)->varattno - childrel->min_attr;

					child_width = childrel->attr_widths[cndx];
				}
				if (child_width <= 0)
					child_width = get_typavgwidth(exprType(childvar),
												  exprTypmod(childvar));
				Assert(child_width > 0);
				parent_attrsizes[pndx] += child_width * childrel->rows;
			}
		}
	}

	if (has_live_children)
	{
		/*
		 * 保存完成的规模估计。
		 */
		int			i;

		Assert(parent_rows > 0);
		rel->rows = parent_rows;
		rel->reltarget->width = rint(parent_size / parent_rows);
		for (i = 0; i < nattrs; i++)
			rel->attr_widths[i] = rint(parent_attrsizes[i] / parent_rows);

		/*
		 * 把追加关系的 "原始元组" 计数设成等于 "行数"；需要这样，
		 * 因为有些地方假定 rel->tuples 对任何基关系都有效。
		 */
		rel->tuples = parent_rows;

		/*
		 * 注意我们把 rel->pages 留为零；这对避免
		 * 在 total_table_pages 中把追加关系树重复计算很重要。
		 */
	}
	else
	{
		/*
		 * 所有子项都被约束排除了，因此把整个
		 * 追加关系标成哑的。必须在这个阶段做，以便在为其他关系生成路径时
		 * 能看见该关系的哑状态。
		 */
		set_dummy_rel_pathlist(rel);
	}

	pfree(parent_attrsizes);
}

/*
 * set_append_rel_pathlist
 *	  为 "追加关系" 建立访问路径
 */
static void
set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	int			parentRTindex = rti;
	List	   *live_childrels = NIL;
	ListCell   *l;

	/*
	 * 为每个成员关系生成访问路径，并记住
	 * 非哑子项。
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;

		/* append_rel_list 含有全部追加关系；忽略其他的 */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/* 重新定位子 RTE 和 RelOptInfo */
		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];
		childrel = root->simple_rel_array[childRTindex];

		/*
		 * 若 set_append_rel_size() 在访问过这个子关系之后的某处
		 * 判定父追加关系并行不安全，就需要
		 * 把不安全标记向下传到子项，以免
		 * 为它生成无用的部分路径。
		 */
		if (!rel->consider_parallel)
			childrel->consider_parallel = false;

		/*
		 * 计算该子项的访问路径。
		 */
		set_rel_pathlist(root, childrel, childRTindex, childRTE);

		/*
		 * 若子项是哑的，就忽略它。
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/*
		 * 子项是活的，因此把它加入下面要用的 live_childrels 列表。
		 */
		live_childrels = lappend(live_childrels, childrel);
	}

	/* 把路径加入追加关系。 */
	add_paths_to_append_rel(root, rel, live_childrels);
}


/*
 * add_paths_to_append_rel
 *		给定一组非哑子关系，为给定的追加关系生成路径。
 *		本函数收集非哑子项支持的全部参数化和排序。
 *
 * 对每一种这样的参数化或排序，它建立
 * 一条追加路径，从每个具有给定
 * 参数化或排序的非哑子项收集一条路径。同样，它从
 * 非哑子项收集部分路径，
 * 以建立部分追加路径。
 */
void
add_paths_to_append_rel(PlannerInfo *root, RelOptInfo *rel,
						List *live_childrels)
{
	List	   *subpaths = NIL;
	bool		subpaths_valid = true;
	List	   *partial_subpaths = NIL;
	List	   *pa_partial_subpaths = NIL;
	List	   *pa_nonpartial_subpaths = NIL;
	bool		partial_subpaths_valid = true;
	bool		pa_subpaths_valid;
	List	   *all_child_pathkeys = NIL;
	List	   *all_child_outers = NIL;
	ListCell   *l;
	double		partial_rows = -1;

	/* 若合适，考虑并行追加 */
	pa_subpaths_valid = enable_parallel_append && rel->consider_parallel;

	/*
	 * 对每个非哑子项，记住最便宜的路径。
	 * 同时，识别非哑成员关系可用的全部路径键（排序）
	 * 和参数化（required_outer 集合）。
	 */
	foreach(l, live_childrels)
	{
		RelOptInfo *childrel = lfirst(l);
		ListCell   *lcp;
		Path	   *cheapest_partial_path = NULL;

		/*
		 * 若子项有一条未参数化的最便宜总代价路径，就把它加入
		 * 正在为父项构造的未参数化 Append 路径。
		 * 若没有，就没有可行的未参数化路径。
		 *
		 * 有按分区聚合时，子关系的 pathlist 可能
		 * 为空，因此不要假定这里一定有路径。
		 */
		if (childrel->pathlist != NIL &&
			childrel->cheapest_total_path->param_info == NULL)
			accumulate_append_subpath(childrel->cheapest_total_path,
									  &subpaths, NULL);
		else
			subpaths_valid = false;

		/* 同样的想法，但是针对部分计划。 */
		if (childrel->partial_pathlist != NIL)
		{
			cheapest_partial_path = linitial(childrel->partial_pathlist);
			accumulate_append_subpath(cheapest_partial_path,
									  &partial_subpaths, NULL);
		}
		else
			partial_subpaths_valid = false;

		/*
		 * 同样的想法，但是针对混合部分路径
		 * 与非部分路径的并行追加。
		 */
		if (pa_subpaths_valid)
		{
			Path	   *nppath = NULL;

			nppath =
				get_cheapest_parallel_safe_total_inner(childrel->pathlist);

			if (cheapest_partial_path == NULL && nppath == NULL)
			{
				/* 既没有部分路径也没有并行安全路径？那就忘掉它。 */
				pa_subpaths_valid = false;
			}
			else if (nppath == NULL ||
					 (cheapest_partial_path != NULL &&
					  cheapest_partial_path->total_cost < nppath->total_cost))
			{
				/* 部分路径更便宜，或者是唯一选择。 */
				Assert(cheapest_partial_path != NULL);
				accumulate_append_subpath(cheapest_partial_path,
										  &pa_partial_subpaths,
										  &pa_nonpartial_subpaths);
			}
			else
			{
				/*
				 * 要么只有一条非部分路径，要么我们认为
				 * 单个后端执行最好的非部分路径
				 * 会比所有并行后端一起工作执行
				 * 最好的部分路径更快。
				 *
				 * 这里也许应当更积极一些。即使
				 * 最好的非部分路径比最好的
				 * 部分路径更贵，若有好几条这样的路径可以
				 * 分给不同的工作进程，选择非部分路径
				 * 仍可能更好。目前，我们不试图
				 * 把这一点算出来。
				 */
				accumulate_append_subpath(nppath,
										  &pa_nonpartial_subpaths,
										  NULL);
			}
		}

		/*
		 * 收集所有子项可用的全部路径排序和
		 * 参数化的列表。我们用它们作为
		 * 启发式，指示应当为哪些排序和参数化
		 * 建立 Append 和 MergeAppend 路径。
		 */
		foreach(lcp, childrel->pathlist)
		{
			Path	   *childpath = (Path *) lfirst(lcp);
			List	   *childkeys = childpath->pathkeys;
			Relids		childouter = PATH_REQ_OUTER(childpath);

			/* 无序路径不贡献给路径键列表 */
			if (childkeys != NIL)
			{
				ListCell   *lpk;
				bool		found = false;

				/* 已经见过这种排序了吗？ */
				foreach(lpk, all_child_pathkeys)
				{
					List	   *existing_pathkeys = (List *) lfirst(lpk);

					if (compare_pathkeys(existing_pathkeys,
										 childkeys) == PATHKEYS_EQUAL)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* 没有，因此把它加入 all_child_pathkeys */
					all_child_pathkeys = lappend(all_child_pathkeys,
												 childkeys);
				}
			}

			/* 未参数化路径不贡献给参数集合列表 */
			if (childouter)
			{
				ListCell   *lco;
				bool		found = false;

				/* 已经见过这个参数集合了吗？ */
				foreach(lco, all_child_outers)
				{
					Relids		existing_outers = (Relids) lfirst(lco);

					if (bms_equal(existing_outers, childouter))
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* 没有，因此把它加入 all_child_outers */
					all_child_outers = lappend(all_child_outers,
											   childouter);
				}
			}
		}
	}

	/*
	 * 若为所有子项都找到了未参数化路径，就为该关系建立一条无序、
	 * 未参数化的 Append 路径。（注意：即使因约束排除而
	 * 只有零条或一条活的子路径，这也是对的。）
	 */
	if (subpaths_valid)
		add_path(rel, (Path *) create_append_path(root, rel, subpaths, NIL,
												  NIL, NULL, 0, false,
												  -1));

	/*
	 * 考虑把无序、未参数化的部分路径追加起来。若可能，
	 * 让它感知并行。
	 */
	if (partial_subpaths_valid && partial_subpaths != NIL)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/* 找出任何子路径所请求的最大工作进程数。 */
		foreach(lc, partial_subpaths)
		{
			Path	   *path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * 若允许使用并行追加，总是至少请求
		 * log2(子项数) 个工作进程。假定这种情况下多一些
		 * 工作进程可能有用，因为它们会分散到
		 * 各个子项上。精确公式只是猜测，但我们不想
		 * 让有 N 个分区的表与数据相同的未分区表得出
		 * 截然不同的答案，因此
		 * 这里用某种对数缩放似乎有些道理。
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
								   pg_leftmost_one_pos32(list_length(live_childrels)) + 1);
			parallel_workers = Min(parallel_workers,
								   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		/* 生成一条部分追加路径。 */
		appendpath = create_append_path(root, rel, NIL, partial_subpaths,
										NIL, NULL, parallel_workers,
										enable_parallel_append,
										-1);

		/*
		 * 确保此后任何部分路径都使用相同的行数
		 * 估计。
		 */
		partial_rows = appendpath->path.rows;

		/* 加入该路径。 */
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * 考虑用部分路径和非部分路径混合而成的感知并行追加。
	 * （仅当至少有一个子项的某条非部分路径
	 * 明显比任何部分路径更便宜时，这才有意义；
	 * 否则应当使用上一步加入的追加路径。）
	 */
	if (pa_subpaths_valid && pa_nonpartial_subpaths != NIL)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/*
		 * 找出任何部分
		 * 子路径所请求的最大工作进程数。
		 */
		foreach(lc, pa_partial_subpaths)
		{
			Path	   *path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}

		/*
		 * 这里的公式与上面相同。在这种
		 * 情况下更重要，因为非部分路径对
		 * 计划中的并行工作进程数没有任何贡献。
		 */
		parallel_workers = Max(parallel_workers,
							   pg_leftmost_one_pos32(list_length(live_childrels)) + 1);
		parallel_workers = Min(parallel_workers,
							   max_parallel_workers_per_gather);
		Assert(parallel_workers > 0);

		appendpath = create_append_path(root, rel, pa_nonpartial_subpaths,
										pa_partial_subpaths,
										NIL, NULL, parallel_workers, true,
										partial_rows);
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * 也根据收集到的子路径键列表，
	 * 建立未参数化的有序追加路径。
	 */
	if (subpaths_valid)
		generate_orderedappend_paths(root, rel, live_childrels,
									 all_child_pathkeys);

	/*
	 * 为子关系中见到的每种参数化建立 Append 路径。
	 * （这看起来可能很贵，但在多数实际
	 * 有意义的情形里，子关系暴露的参数化大多相同，
	 * 因此这里实际考虑的情形并没有那么多。）
	 *
	 * Append 节点本身无法强制限定条件，因此所有限定条件检查都必须
	 * 在子路径中完成。这意味着要有一条参数化
	 * Append 路径，每条
	 * 子路径的参数化必须完全相同；否则有些子项可能没有检查
	 * 下移的限定条件。为了让它们对齐，可以试着增加
	 * 参数化较少的路径的参数化。
	 */
	foreach(l, all_child_outers)
	{
		Relids		required_outer = (Relids) lfirst(l);
		ListCell   *lcr;

		/* 为具有这种参数化的 Append 选出子路径 */
		subpaths = NIL;
		subpaths_valid = true;
		foreach(lcr, live_childrels)
		{
			RelOptInfo *childrel = (RelOptInfo *) lfirst(lcr);
			Path	   *subpath;

			if (childrel->pathlist == NIL)
			{
				/* 未能为这个子项做出合适的路径 */
				subpaths_valid = false;
				break;
			}

			subpath = get_cheapest_parameterized_child_path(root,
															childrel,
															required_outer);
			if (subpath == NULL)
			{
				/* 未能为这个子项做出合适的路径 */
				subpaths_valid = false;
				break;
			}
			accumulate_append_subpath(subpath, &subpaths, NULL);
		}

		if (subpaths_valid)
			add_path(rel, (Path *)
					 create_append_path(root, rel, subpaths, NIL,
										NIL, required_outer, 0, false,
										-1));
	}

	/*
	 * 当只有一个子关系时，Append 路径可以继承
	 * 该子关系路径可用的任何排序，因此考虑
	 * 有序的部分路径是有用的。上面只考虑了每个子项最便宜的
	 * 部分路径，但也让我们用任何
	 * 带路径键的部分路径来建立路径。
	 */
	if (list_length(live_childrels) == 1)
	{
		RelOptInfo *childrel = (RelOptInfo *) linitial(live_childrels);

		/* 跳过最便宜的部分路径，因为上面已经用过了 */
		for_each_from(l, childrel->partial_pathlist, 1)
		{
			Path	   *path = (Path *) lfirst(l);
			AppendPath *appendpath;

			/* 跳过没有路径键的路径。 */
			if (path->pathkeys == NIL)
				continue;

			appendpath = create_append_path(root, rel, NIL, list_make1(path),
											NIL, NULL,
											path->parallel_workers, true,
											partial_rows);
			add_partial_path(rel, (Path *) appendpath);
		}
	}
}

/*
 * generate_orderedappend_paths
 *		为追加关系生成有序追加路径
 *
 * 通常在这里生成 MergeAppend 路径，但有些特殊
 * 情形可以生成简单的 Append 路径，因为子路径
 * 已经能按所需顺序提供元组。
 *
 * 对 all_child_pathkeys 中出现的每种排序（路径键列表）
 * 都生成一条路径。
 *
 * 同时考虑最便宜启动和最便宜总代价两种情形，也就是对每种
 * 有意义的排序，收集全部最便宜启动子路径和全部
 * 最便宜总代价路径，并为每种情形建立合适的路径。
 *
 * 目前这里不生成任何有序的参数化路径。虽然
 * 在这里多写一点代码就能做到，但很不清楚
 * 花规划周期去研究这类路径是否值得：有序路径用在
 * 嵌套循环内侧没什么用。事实上，
 * add_path 的现有代码很可能会直接拒绝这类路径，
 * 因为 add_path 不给参数化路径的排序加分，
 * 而参数化的 MergeAppend 会比对应的
 * 参数化 Append 路径更贵。若以后更努力支持
 * 参数化归并连接计划，也许值得在这里增加对
 * 参数化路径的支持，用来喂给这类连接。（见
 * optimizer/README 中关于为什么这也许永远不会发生的说明。）
 */
static void
generate_orderedappend_paths(PlannerInfo *root, RelOptInfo *rel,
							 List *live_childrels,
							 List *all_child_pathkeys)
{
	ListCell   *lcp;
	List	   *partition_pathkeys = NIL;
	List	   *partition_pathkeys_desc = NIL;
	bool		partition_pathkeys_partial = true;
	bool		partition_pathkeys_desc_partial = true;

	/*
	 * 有些分区表设置允许我们用 Append 节点
	 * 而不是 MergeAppend。在 RANGE
	 * 分区表这类情形里这是可能的，因为可以保证更早的分区必须
	 * 包含在排序中更靠前的行。为了检测这是否
	 * 相关，为分区排序建立路径键描述，
	 * 正向和反向扫描都要。
	 */
	if (rel->part_scheme != NULL && IS_SIMPLE_REL(rel) &&
		partitions_are_ordered(rel->boundinfo, rel->live_parts))
	{
		partition_pathkeys = build_partition_pathkeys(root, rel,
													  ForwardScanDirection,
													  &partition_pathkeys_partial);

		partition_pathkeys_desc = build_partition_pathkeys(root, rel,
														   BackwardScanDirection,
														   &partition_pathkeys_desc_partial);

		/*
		 * 你也许觉得这里应当 truncate_useless_pathkeys，但
		 * 允许分区键是查询路径键的子集
		 * 常常有用。例如，考虑按
		 * RANGE (a, b) 分区的表，以及 ORDER BY a, b, c 的查询。若有子
		 * 路径能产生 a, b, c 排序（也许通过 (a, b, c) 上的索引），
		 * 那么把追加关系的输出视为
		 * 按 a, b, c 排序是可行的。
		 */
	}

	/* 现在考虑每种有意义的排序 */
	foreach(lcp, all_child_pathkeys)
	{
		List	   *pathkeys = (List *) lfirst(lcp);
		List	   *startup_subpaths = NIL;
		List	   *total_subpaths = NIL;
		List	   *fractional_subpaths = NIL;
		bool		startup_neq_total = false;
		bool		match_partition_order;
		bool		match_partition_order_desc;
		int			end_index;
		int			first_index;
		int			direction;

		/*
		 * 判断这种排序是否匹配我们已有的任何分区路径键，
		 * 升序和降序分区都要看。若
		 * 分区路径键恰好包含在路径键中，仍然
		 * 可行，如上所述，只要分区路径键
		 * 是完整的，而不只是分区键的前缀。（在这种
		 * 情形里，我们依赖子路径把所需路径键的
		 * 较低顺序列排好序。）
		 */
		match_partition_order =
			pathkeys_contained_in(pathkeys, partition_pathkeys) ||
			(!partition_pathkeys_partial &&
			 pathkeys_contained_in(partition_pathkeys, pathkeys));

		match_partition_order_desc = !match_partition_order &&
			(pathkeys_contained_in(pathkeys, partition_pathkeys_desc) ||
			 (!partition_pathkeys_desc_partial &&
			  pathkeys_contained_in(partition_pathkeys_desc, pathkeys)));

		/*
		 * 当所需路径键匹配分区顺序的反向时，
		 * 必须从最后匹配的分区开始反向建立路径列表。
		 * 下面的循环不必为此做任何
		 * 特殊处理，这种情况下只要反向循环
		 * 子关系即可。
		 */
		if (match_partition_order_desc)
		{
			/* 反向循环 */
			first_index = list_length(live_childrels) - 1;
			end_index = -1;
			direction = -1;

			/*
			 * 把这个设为 true，免得下面的循环里还要检查
			 * match_partition_order_desc。
			 */
			match_partition_order = true;
		}
		else
		{
			/* 其他情况都正向循环 */
			first_index = 0;
			end_index = list_length(live_childrels);
			direction = 1;
		}

		/* 为这种排序选出子路径…… */
		for (int i = first_index; i != end_index; i += direction)
		{
			RelOptInfo *childrel = list_nth_node(RelOptInfo, live_childrels, i);
			Path	   *cheapest_startup,
					   *cheapest_total,
					   *cheapest_fractional = NULL;

			/* 若有合适的路径，就定位它们。 */
			cheapest_startup =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   STARTUP_COST,
											   false);
			cheapest_total =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   TOTAL_COST,
											   false);

			/*
			 * 若找不到任何顺序正确的路径，就用
			 * 最便宜总代价路径；以后再排序。
			 */
			if (cheapest_startup == NULL || cheapest_total == NULL)
			{
				cheapest_startup = cheapest_total =
					childrel->cheapest_total_path;
				/* 断言这个子项确实有一条未参数化路径 */
				Assert(cheapest_total->param_info == NULL);
			}

			/*
			 * 建立分数路径时，也为每个子关系确定一条最便宜的
			 * 分数路径。只看启动
			 * 代价和总代价不够，因为最便宜的分数
			 * 路径可能被两条分开的路径支配（一条管启动，
			 * 一条管总代价）。
			 *
			 * 需要时（建立分数路径），也确定最便宜的
			 * 分数路径。
			 */
			if (root->tuple_fraction > 0)
			{
				double		path_fraction = (1.0 / root->tuple_fraction);

				cheapest_fractional =
					get_cheapest_fractional_path_for_pathkeys(childrel->pathlist,
															  pathkeys,
															  NULL,
															  path_fraction);

				/*
				 * 若没找到路径键匹配的路径，就改用
				 * 最便宜总代价路径。
				 *
				 * XXX 也许也可以考虑部分排序的路径（上面再加
				 * 增量排序）。但那样就必须建立全部
				 * 增量路径，做计价等等。
				 */
				if (!cheapest_fractional)
					cheapest_fractional = cheapest_total;
			}

			/*
			 * 注意 "最便宜" 和 "总代价" 两种情形实际上是否有不同路径；
			 * 经常没有必要
			 * 调用两次 create_merge_append_path()。
			 */
			if (cheapest_startup != cheapest_total)
				startup_neq_total = true;

			/*
			 * 收集合适的子路径。所需逻辑对
			 * Append 和 MergeAppend 情形有所不同。
			 */
			if (match_partition_order)
			{
				/*
				 * 将要建立一条普通 Append 路径。不需要
				 * accumulate_append_subpath 会做的大部分事，但确实
				 * 想切掉那些只有
				 * 单条子路径（因而没做什么
				 * 有用的事）的子 Append 或 MergeAppends。
				 */
				cheapest_startup = get_singleton_append_subpath(cheapest_startup);
				cheapest_total = get_singleton_append_subpath(cheapest_total);

				startup_subpaths = lappend(startup_subpaths, cheapest_startup);
				total_subpaths = lappend(total_subpaths, cheapest_total);

				if (cheapest_fractional)
				{
					cheapest_fractional = get_singleton_append_subpath(cheapest_fractional);
					fractional_subpaths = lappend(fractional_subpaths, cheapest_fractional);
				}
			}
			else
			{
				/*
				 * 否则，靠 accumulate_append_subpath 为 MergeAppend
				 * 收集子路径。
				 */
				accumulate_append_subpath(cheapest_startup,
										  &startup_subpaths, NULL);
				accumulate_append_subpath(cheapest_total,
										  &total_subpaths, NULL);

				if (cheapest_fractional)
					accumulate_append_subpath(cheapest_fractional,
											  &fractional_subpaths, NULL);
			}
		}

		/* ……并建立 Append 或 MergeAppend 路径 */
		if (match_partition_order)
		{
			/* 只需要 Append */
			add_path(rel, (Path *) create_append_path(root,
													  rel,
													  startup_subpaths,
													  NIL,
													  pathkeys,
													  NULL,
													  0,
													  false,
													  -1));
			if (startup_neq_total)
				add_path(rel, (Path *) create_append_path(root,
														  rel,
														  total_subpaths,
														  NIL,
														  pathkeys,
														  NULL,
														  0,
														  false,
														  -1));

			if (fractional_subpaths)
				add_path(rel, (Path *) create_append_path(root,
														  rel,
														  fractional_subpaths,
														  NIL,
														  pathkeys,
														  NULL,
														  0,
														  false,
														  -1));
		}
		else
		{
			/* 需要 MergeAppend */
			add_path(rel, (Path *) create_merge_append_path(root,
															rel,
															startup_subpaths,
															pathkeys,
															NULL));
			if (startup_neq_total)
				add_path(rel, (Path *) create_merge_append_path(root,
																rel,
																total_subpaths,
																pathkeys,
																NULL));

			if (fractional_subpaths)
				add_path(rel, (Path *) create_merge_append_path(root,
																rel,
																fractional_subpaths,
																pathkeys,
																NULL));
		}
	}
}

/*
 * get_cheapest_parameterized_child_path
 *		取得该关系上最便宜的路径，其参数化
 *		恰好为所请求的值。
 *
 * 若无法建立这样的路径，就返回 NULL。
 */
static Path *
get_cheapest_parameterized_child_path(PlannerInfo *root, RelOptInfo *rel,
									  Relids required_outer)
{
	Path	   *cheapest;
	ListCell   *lc;

	/*
	 * 查找参数化不超过所需程度的最便宜现有路径。
	 * 若它的参数化恰好是所需的，就
	 * 完成了。
	 */
	cheapest = get_cheapest_path_for_pathkeys(rel->pathlist,
											  NIL,
											  required_outer,
											  TOTAL_COST,
											  false);
	Assert(cheapest != NULL);
	if (bms_equal(PATH_REQ_OUTER(cheapest), required_outer))
		return cheapest;

	/*
	 * 否则，可以 "重新参数化" 一条现有路径以匹配给定的
	 * 参数化，这实际上意味着把额外的
	 * 连接限定条件下推，在该路径的扫描中检查。不过，有些现有
	 * 路径也许已经检查了可用的连接限定条件，另一些没有；
	 * 因此不清楚重新参数化之后哪条现有路径最便宜。
	 * 必须把它们全部过一遍才能知道。
	 */
	cheapest = NULL;
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/* 若它需要的参数化比请求的更多，就不能用 */
		if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			continue;

		/*
		 * 重新参数化只会增加路径的代价，因此若它
		 * 已经比当前最便宜的更贵，就忘掉它。
		 */
		if (cheapest != NULL &&
			compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
			continue;

		/* 若需要就重新参数化，然后重新检查代价 */
		if (!bms_equal(PATH_REQ_OUTER(path), required_outer))
		{
			path = reparameterize_path(root, path, required_outer, 1.0);
			if (path == NULL)
				continue;		/* 这条重新参数化失败 */
			Assert(bms_equal(PATH_REQ_OUTER(path), required_outer));

			if (cheapest != NULL &&
				compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
				continue;
		}

		/* 有了一条新的最佳路径 */
		cheapest = path;
	}

	/* 返回最佳路径；若没找到合适候选则返回 NULL */
	return cheapest;
}

/*
 * accumulate_append_subpath
 *		把一条子路径加入正在为 Append 或 MergeAppend 建立的列表。
 *
 * 子项本身可能是 Append 或 MergeAppend 路径，
 * 这时可以 "去掉中间人"，只把它的子路径加入
 * 我们自己的列表。（不更早做这件事，是因为需要对限定条件
 * 应用两层变换。）
 *
 * 注意，若以这种方式省掉一个子 MergeAppend，实际上就是
 * 省掉了一步排序，这似乎没问题：若父项将是 Append，
 * 其结果反正是无序的，而若父项将是
 * MergeAppend，对子项再单独排序就没有意义。
 *
 * 通常，要么 path 是部分路径而 subpaths 是部分
 * 路径的列表，要么 path 是非部分计划而 subpaths 是那些路径的列表。
 * 不过，若 path 是感知并行的 Append，就把其部分路径
 * 子项加入 subpaths，其余加入 special_subpaths。若后者为
 * NULL，就完全不展平该路径（除非它只含部分
 * 路径）。
 */
static void
accumulate_append_subpath(Path *path, List **subpaths, List **special_subpaths)
{
	if (IsA(path, AppendPath))
	{
		AppendPath *apath = (AppendPath *) path;

		if (!apath->path.parallel_aware || apath->first_partial_path == 0)
		{
			*subpaths = list_concat(*subpaths, apath->subpaths);
			return;
		}
		else if (special_subpaths != NULL)
		{
			List	   *new_special_subpaths;

			/* 把并行 Append 拆成部分和非部分子路径 */
			*subpaths = list_concat(*subpaths,
									list_copy_tail(apath->subpaths,
												   apath->first_partial_path));
			new_special_subpaths = list_copy_head(apath->subpaths,
												  apath->first_partial_path);
			*special_subpaths = list_concat(*special_subpaths,
											new_special_subpaths);
			return;
		}
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;

		*subpaths = list_concat(*subpaths, mpath->subpaths);
		return;
	}

	*subpaths = lappend(*subpaths, path);
}

/*
 * get_singleton_append_subpath
 *		返回 Append/MergeAppend 的单条子路径，或者若它不是
 *		单子路径的 Append/MergeAppend，就直接返回 'path'。
 *
 * 注意：'path' 不得是感知并行的路径。
 */
static Path *
get_singleton_append_subpath(Path *path)
{
	Assert(!path->parallel_aware);

	if (IsA(path, AppendPath))
	{
		AppendPath *apath = (AppendPath *) path;

		if (list_length(apath->subpaths) == 1)
			return (Path *) linitial(apath->subpaths);
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;

		if (list_length(mpath->subpaths) == 1)
			return (Path *) linitial(mpath->subpaths);
	}

	return path;
}

/*
 * set_dummy_rel_pathlist
 *	  为被约束排除的关系建立一条哑路径
 *
 * 与其发明一种特殊的 "哑" 路径类型，不如把它表示成
 * 没有成员的 AppendPath（另见宏 IS_DUMMY_APPEND/IS_DUMMY_REL）。
 *
 * （另见 mark_dummy_rel，它做的基本上是同一件事，但
 * 通常用于在已经为关系建立
 * 路径之后再把它改成哑状态。）
 */
static void
set_dummy_rel_pathlist(RelOptInfo *rel)
{
	/* 设置哑的规模估计 --- attr_widths[] 留为零 */
	rel->rows = 0;
	rel->reltarget->width = 0;

	/* 丢掉任何已有路径；不再需要它们 */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/* 建立哑路径 */
	add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL,
											  NIL, rel->lateral_relids,
											  0, false, -1));

	/*
	 * 立刻设置最便宜路径字段，以防它们
	 * 指向某条已丢掉的路径。从 set_rel_size() 调用时这是多余的，
	 * 但从别处调用时不是，而且做
	 * 两次反正无害。
	 */
	set_cheapest(rel);
}

/* 粗略测试是否需要做任何连接 */
static bool
has_multiple_baserels(PlannerInfo *root)
{
	int			num_base_rels = 0;
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		/* 忽略作为 "其他关系" 的 RTEs */
		if (brel->reloptkind == RELOPT_BASEREL)
			if (++num_base_rels > 1)
				return true;
	}
	return false;
}

/*
 * find_window_run_conditions
 *		判断 'wfunc' 是否真的是 WindowFunc，并调用其 prosupport
 *		函数来判断该函数的单调性质。然后
 *		看 'opexpr' 能否用来短路执行。
 *
 * 例如 row_number() over (order by ...) 总是产生比前一个
 * 大一的值。若有人在子查询里有窗口函数，
 * 并且外层查询有 WHERE 子句过滤行 <= 10，那么
 * 行号一到 11 就可以停止处理窗口聚合。这里
 * 检查 'opexpr' 是否有助于停止做不必要的额外处理，
 * 就在 WindowAgg 节点中。
 *
 * 若调用方还应当把 'opexpr' 用于
 * 其原来的目的，就把 '*keep_original' 设为 true。若调用方可假定
 * 运行条件会处理全部所需过滤，就把它设为 false。
 *
 * 若发现 'opexpr' 有用并已加入
 * WindowClauses 的 runCondition，就返回 true。也相应设置 *keep_original，并把
 * 'attno' 加上 FirstLowInvalidHeapAttributeNumber 的偏移后加入 *run_cond_attrs。
 * 若 'opexpr' 无法使用，就把 *keep_original 设为 true 并
 * 返回 false。
 */
static bool
find_window_run_conditions(Query *subquery, RangeTblEntry *rte, Index rti,
						   AttrNumber attno, WindowFunc *wfunc, OpExpr *opexpr,
						   bool wfunc_left, bool *keep_original,
						   Bitmapset **run_cond_attrs)
{
	Oid			prosupport;
	Expr	   *otherexpr;
	SupportRequestWFuncMonotonic req;
	SupportRequestWFuncMonotonic *res;
	WindowClause *wclause;
	List	   *opinfos;
	OpExpr	   *runopexpr;
	Oid			runoperator;
	ListCell   *lc;

	*keep_original = true;

	while (IsA(wfunc, RelabelType))
		wfunc = (WindowFunc *) ((RelabelType *) wfunc)->arg;

	/* 只能对窗口函数做这件事 */
	if (!IsA(wfunc, WindowFunc))
		return false;

	/* 若 WindowFunc 中有子计划，就不能用 */
	if (contain_subplans((Node *) wfunc))
		return false;

	prosupport = get_func_support(wfunc->winfnoid);

	/* 检查 'wfunc' 是否有支持函数 */
	if (!OidIsValid(prosupport))
		return false;

	/* 从 OpExpr 的另一侧取出 Expr */
	if (wfunc_left)
		otherexpr = lsecond(opexpr->args);
	else
		otherexpr = linitial(opexpr->args);

	/*
	 * 被比较的值在窗口分区的
	 * 求值期间不得改变。
	 */
	if (!is_pseudo_constant_clause((Node *) otherexpr))
		return false;

	/* 找出属于该窗口函数的窗口子句 */
	wclause = (WindowClause *) list_nth(subquery->windowClause,
										wfunc->winref - 1);

	req.type = T_SupportRequestWFuncMonotonic;
	req.window_func = wfunc;
	req.window_clause = wclause;

	/* 调用支持函数 */
	res = (SupportRequestWFuncMonotonic *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	/*
	 * 若该函数既非单调递增也非
	 * 单调递减，就无事可做。
	 */
	if (res == NULL || res->monotonic == MONOTONICFUNC_NONE)
		return false;

	runopexpr = NULL;
	runoperator = InvalidOid;
	opinfos = get_op_btree_interpretation(opexpr->opno);

	foreach(lc, opinfos)
	{
		OpBtreeInterpretation *opinfo = (OpBtreeInterpretation *) lfirst(lc);
		int			strategy = opinfo->strategy;

		/* 处理 < / <= */
		if (strategy == BTLessStrategyNumber ||
			strategy == BTLessEqualStrategyNumber)
		{
			/*
			 * 对单调递增函数，< / <= 支持
			 * <wfunc> op <pseudoconst> 这种形式；对单调递减函数，支持
			 * <pseudoconst> op <wfunc>。
			 */
			if ((wfunc_left && (res->monotonic & MONOTONICFUNC_INCREASING)) ||
				(!wfunc_left && (res->monotonic & MONOTONICFUNC_DECREASING)))
			{
				*keep_original = false;
				runopexpr = opexpr;
				runoperator = opexpr->opno;
			}
			break;
		}
		/* 处理 > / >= */
		else if (strategy == BTGreaterStrategyNumber ||
				 strategy == BTGreaterEqualStrategyNumber)
		{
			/*
			 * 对单调递减函数，> / >= 支持
			 * <wfunc> op <pseudoconst> 这种形式；对单调递增函数，支持
			 * <pseudoconst> op <wfunc>。
			 */
			if ((wfunc_left && (res->monotonic & MONOTONICFUNC_DECREASING)) ||
				(!wfunc_left && (res->monotonic & MONOTONICFUNC_INCREASING)))
			{
				*keep_original = false;
				runopexpr = opexpr;
				runoperator = opexpr->opno;
			}
			break;
		}
		/* 处理 = */
		else if (strategy == BTEqualStrategyNumber)
		{
			int16		newstrategy;

			/*
			 * 当既单调递增又单调递减时，
			 * 窗口函数的返回值每次都相同。
			 * 可以直接把 'opexpr' 用作运行条件，不必
			 * 修改它。
			 */
			if ((res->monotonic & MONOTONICFUNC_BOTH) == MONOTONICFUNC_BOTH)
			{
				*keep_original = false;
				runopexpr = opexpr;
				runoperator = opexpr->opno;
				break;
			}

			/*
			 * 单调递增时，我们做一条 <wfunc> <=
			 * <value> 或 <value> >= <wfunc> 的限定条件，以便滤掉
			 * 高于等值条件中那个值的值。对
			 * 单调递减函数，则要滤掉
			 * 低于等值条件中那个值的值。
			 */
			if (res->monotonic & MONOTONICFUNC_INCREASING)
				newstrategy = wfunc_left ? BTLessEqualStrategyNumber : BTGreaterEqualStrategyNumber;
			else
				newstrategy = wfunc_left ? BTGreaterEqualStrategyNumber : BTLessEqualStrategyNumber;

			/* 必须保留原来的等值限定条件 */
			*keep_original = true;
			runopexpr = opexpr;

			/* 决定 runCondition 限定条件要用的操作符 */
			runoperator = get_opfamily_member(opinfo->opfamily_id,
											  opinfo->oplefttype,
											  opinfo->oprighttype,
											  newstrategy);
			break;
		}
	}

	if (runopexpr != NULL)
	{
		Expr	   *newexpr;

		/*
		 * 建立运行条件所需的限定条件，让
		 * WindowFunc 留在它原来所在的那一侧。
		 */
		if (wfunc_left)
			newexpr = make_opclause(runoperator,
									runopexpr->opresulttype,
									runopexpr->opretset, (Expr *) wfunc,
									otherexpr, runopexpr->opcollid,
									runopexpr->inputcollid);
		else
			newexpr = make_opclause(runoperator,
									runopexpr->opresulttype,
									runopexpr->opretset,
									otherexpr, (Expr *) wfunc,
									runopexpr->opcollid,
									runopexpr->inputcollid);

		wclause->runCondition = lappend(wclause->runCondition, newexpr);

		/* 记录这个 attno 被用在运行条件中 */
		*run_cond_attrs = bms_add_member(*run_cond_attrs,
										 attno - FirstLowInvalidHeapAttributeNumber);
		return true;
	}

	/* 不支持的 OpExpr */
	return false;
}

/*
 * check_and_push_window_quals
 *		检查 'clause' 是否是可以推进 WindowFunc 的
 *		WindowClause、作为 'runCondition' 限定条件的限定条件。这些条件若存在，就允许
 *		在执行期间跳过一些不必要的工作。
 *
 * 'run_cond_attrs' 将被填上我们为其下推了
 * 窗口限定条件的子查询目标的全部目标列表 resno
 * （偏移 FirstLowInvalidHeapAttributeNumber）。
 *
 * 若调用方仍必须保留原来的限定条件就返回 true，若
 * 调用方可安全忽略原来的限定条件就返回 false，因为 WindowAgg 节点
 * 将用 runCondition 停止返回元组。
 */
static bool
check_and_push_window_quals(Query *subquery, RangeTblEntry *rte, Index rti,
							Node *clause, Bitmapset **run_cond_attrs)
{
	OpExpr	   *opexpr = (OpExpr *) clause;
	bool		keep_original = true;
	Var		   *var1;
	Var		   *var2;

	/* 只能使用有 2 个操作数的 OpExprs */
	if (!IsA(opexpr, OpExpr))
		return true;

	if (list_length(opexpr->args) != 2)
		return true;

	/*
	 * 目前把这项优化限制在严格的 OpExprs。原因
	 * 是执行期间一旦运行条件变成 false，
	 * 就停止求值 WindowFuncs。为避免留下过时的窗口
	 * 函数结果值，把它们设为 NULL。这里只有严格的
	 * OpExprs，才能保证在顶层 WindowAgg 中正确滤掉带 NULLs
	 * 的元组。
	 */
	set_opfuncid(opexpr);
	if (!func_strict(opexpr->opfuncid))
		return true;

	/*
	 * 检查引用子查询中窗口函数的普通 Vars。
	 * 若找到任何，就问 find_window_run_conditions()：'opexpr' 能否
	 * 用作运行条件的一部分。
	 */

	/* 检查 OpExpr 的左侧 */
	var1 = linitial(opexpr->args);
	if (IsA(var1, Var) && var1->varattno > 0)
	{
		TargetEntry *tle = list_nth(subquery->targetList, var1->varattno - 1);
		WindowFunc *wfunc = (WindowFunc *) tle->expr;

		if (find_window_run_conditions(subquery, rte, rti, tle->resno, wfunc,
									   opexpr, true, &keep_original,
									   run_cond_attrs))
			return keep_original;
	}

	/* 并检查右侧 */
	var2 = lsecond(opexpr->args);
	if (IsA(var2, Var) && var2->varattno > 0)
	{
		TargetEntry *tle = list_nth(subquery->targetList, var2->varattno - 1);
		WindowFunc *wfunc = (WindowFunc *) tle->expr;

		if (find_window_run_conditions(subquery, rte, rti, tle->resno, wfunc,
									   opexpr, false, &keep_original,
									   run_cond_attrs))
			return keep_original;
	}

	return true;
}

/*
 * set_subquery_pathlist
 *		为子查询 RTE 生成 SubqueryScan 访问路径
 *
 * 目前不支持通过把连接子句下推到子查询中，来为子查询生成参数化路径；
 * 为考虑不同方案而多次重新规划
 * 子查询似乎太贵。
 * （XXX 既然我们用 Paths，这一点值得重新考虑。）
 * 因此这里建立的路径在子查询含有
 * LATERAL 引用时会是参数化的，否则不是。只要这一点成立，就不需要
 * 单独的 set_subquery_size 阶段：直接把路径建出来即可。
 */
static void
set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	Query	   *subquery = rte->subquery;
	bool		trivial_pathtarget;
	Relids		required_outer;
	pushdown_safety_info safetyInfo;
	double		tuple_fraction;
	RelOptInfo *sub_final_rel;
	Bitmapset  *run_cond_attrs = NULL;
	ListCell   *lc;

	/*
	 * 必须复制 Query，以免规划弄乱 RTE 的内容
	 * （真的很需要修正规划器，让它不要在输入上涂写，
	 * 总有一天……但可以先看 remove_unused_subquery_outputs）。
	 */
	subquery = copyObject(subquery);

	/*
	 * 若是 LATERAL 子查询，它可能含有当前
	 * 查询层次的一些 Vars，从而必须当作参数化来处理，即使
	 * 不支持把连接限定条件下推到子查询。
	 */
	required_outer = rel->lateral_relids;

	/*
	 * 把 subquery_is_pushdown_safe 的结果区清零，以便它在
	 * 递归时按需要设置标志。尤其是，需要一块工作区
	 * 来记录各列不宜被引用的原因。
	 * 发现子查询的第 i
	 * 列不宜用于
	 * 下推限定条件时，这些原因存在 unsafeFlags[i] 的各个位里。
	 */
	memset(&safetyInfo, 0, sizeof(safetyInfo));
	safetyInfo.unsafeFlags = (unsigned char *)
		palloc0((list_length(subquery->targetList) + 1) * sizeof(unsigned char));

	/*
	 * 若子查询有 "security_barrier" 标志，就表示该子查询
	 * 来自必须强制行级安全的视图。于是
	 * 不得下推含有泄漏函数的限定条件。（理想情况下这
	 * 会在 subquery_is_pushdown_safe 内部检查，但既然目前
	 * 不把 RTE 传给那个函数，就必须在这里做。）
	 */
	safetyInfo.unsafeLeaky = rte->security_barrier;

	/*
	 * 若有任何已挂到
	 * 子查询关系上的限制子句，就考虑把它们下推，成为子查询自身的 WHERE 或 HAVING
	 * 限定条件。这种变换有用，因为它
	 * 也许能让我们为子查询生成更好的计划，而不是先求值
	 * 子查询的全部输出行再过滤它们。
	 *
	 * 有几种情形无法下推子句。涉及
	 * 子查询的限制由 subquery_is_pushdown_safe() 检查。
	 * 对单条子句的限制由
	 * qual_is_pushdown_safe() 检查。另外，不想下推
	 * 伪常量子句；最好让门控节点位于
	 * 子查询之上。
	 *
	 * 未下推的子句将作为
	 * SubqueryScan 节点的 qpqual 求值。
	 *
	 * XXX 是否有些情形想做策略决定，不下推
	 * 一条可下推的限定条件，因为它会导致更差的计划？
	 */
	if (rel->baserestrictinfo != NIL &&
		subquery_is_pushdown_safe(subquery, subquery, &safetyInfo))
	{
		/* 可以考虑下推单条限定条件 */
		List	   *upperrestrictlist = NIL;
		ListCell   *l;

		foreach(l, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
			Node	   *clause = (Node *) rinfo->clause;

			if (rinfo->pseudoconstant)
			{
				upperrestrictlist = lappend(upperrestrictlist, rinfo);
				continue;
			}

			switch (qual_is_pushdown_safe(subquery, rti, rinfo, &safetyInfo))
			{
				case PUSHDOWN_SAFE:
					/* 把它下推 */
					subquery_push_qual(subquery, rte, rti, clause);
					break;

				case PUSHDOWN_WINDOWCLAUSE_RUNCOND:

					/*
					 * 既然无法把限定条件下推到子查询，
					 * 就检查它是否恰好引用了窗口函数。若
					 * 是，它也许适合用作 WindowAgg 的
					 * runCondition。
					 */
					if (!subquery->hasWindowFuncs ||
						check_and_push_window_quals(subquery, rte, rti, clause,
													&run_cond_attrs))
					{
						/*
						 * 子查询没有窗口函数，或者该子句不是
						 * 合适的窗口运行条件限定条件，或者它是，但
						 * 原来的限定条件也必须留在上层查询中。
						 */
						upperrestrictlist = lappend(upperrestrictlist, rinfo);
					}
					break;

				case PUSHDOWN_UNSAFE:
					upperrestrictlist = lappend(upperrestrictlist, rinfo);
					break;
			}
		}
		rel->baserestrictinfo = upperrestrictlist;
		/* 不费心重新计算 baserestrict_min_security */
	}

	pfree(safetyInfo.unsafeFlags);

	/*
	 * 上层查询也许不会用到子查询的全部输出列；若
	 * 不用，就可以简化。把下推到
	 * WindowAgg 运行条件中的属性传进去，以确保不会误以为那些
	 * 是未使用的。
	 */
	remove_unused_subquery_outputs(subquery, rel, run_cond_attrs);

	/*
	 * 若外层没有要做的连接、聚合或排序，就可以安全地把外层 tuple_fraction 传给子查询。
	 * 否则
	 * 最好告诉子查询按完整检索来规划。（XXX 这
	 * 也许可以做得更智能……）
	 */
	if (parse->hasAggs ||
		parse->groupClause ||
		parse->groupingSets ||
		root->hasHavingQual ||
		parse->distinctClause ||
		parse->sortClause ||
		has_multiple_baserels(root))
		tuple_fraction = 0.0;	/* 默认情形 */
	else
		tuple_fraction = root->tuple_fraction;

	/* plan_params 不应当在当前查询层次使用 */
	Assert(root->plan_params == NIL);

	/* 为子查询生成 subroot 和 Paths */
	rel->subroot = subquery_planner(root->glob, subquery,
									root,
									false, tuple_fraction);

	/* 隔离这个特定子计划所需的参数 */
	rel->subplan_params = root->plan_params;
	root->plan_params = NIL;

	/*
	 * 约束排除有可能证明子查询为空。若
	 * 是这样，最好产生一条不加装饰的哑路径，以便在这个查询层次
	 * 识别出适当的优化。
	 */
	sub_final_rel = fetch_upper_rel(rel->subroot, UPPERREL_FINAL, NULL);

	if (IS_DUMMY_REL(sub_final_rel))
	{
		set_dummy_rel_pathlist(rel);
		return;
	}

	/*
	 * 用估计的输出行数、宽度等标记该关系。注意必须
	 * 在生成外层查询路径之前做这件事，否则 cost_subqueryscan 会
	 * 不高兴。
	 */
	set_subquery_size_estimates(root, rel);

	/*
	 * 也检测 reltarget 是否平凡，以便把该
	 * 信息传给 cost_subqueryscan（而不是多次重新推导）。
	 * 若它按顺序取出子计划的全部输出列，就是平凡的。
	 */
	if (list_length(rel->reltarget->exprs) != list_length(subquery->targetList))
		trivial_pathtarget = false;
	else
	{
		trivial_pathtarget = true;
		foreach(lc, rel->reltarget->exprs)
		{
			Node	   *node = (Node *) lfirst(lc);
			Var		   *var;

			if (!IsA(node, Var))
			{
				trivial_pathtarget = false;
				break;
			}
			var = (Var *) node;
			if (var->varno != rti ||
				var->varattno != foreach_current_index(lc) + 1)
			{
				trivial_pathtarget = false;
				break;
			}
		}
	}

	/*
	 * 对 subquery_planner 产生的每条 Path，在外层查询中
	 * 建立一条 SubqueryScanPath。
	 */
	foreach(lc, sub_final_rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		List	   *pathkeys;

		/* 把子路径的路径键转换成外层表示 */
		pathkeys = convert_subquery_pathkeys(root,
											 rel,
											 subpath->pathkeys,
											 make_tlist_from_pathtarget(subpath->pathtarget));

		/* 用这条子路径生成外层路径 */
		add_path(rel, (Path *)
				 create_subqueryscan_path(root, rel, subpath,
										  trivial_pathtarget,
										  pathkeys, required_outer));
	}

	/* 若外层关系允许并行，对部分路径也这样做。 */
	if (rel->consider_parallel && bms_is_empty(required_outer))
	{
		/* 若 consider_parallel 为 false，就不应当有部分路径。 */
		Assert(sub_final_rel->consider_parallel ||
			   sub_final_rel->partial_pathlist == NIL);

		/* 对部分路径同样处理。 */
		foreach(lc, sub_final_rel->partial_pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			List	   *pathkeys;

			/* 把子路径的路径键转换成外层表示 */
			pathkeys = convert_subquery_pathkeys(root,
												 rel,
												 subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* 用这条子路径生成外层路径 */
			add_partial_path(rel, (Path *)
							 create_subqueryscan_path(root, rel, subpath,
													  trivial_pathtarget,
													  pathkeys,
													  required_outer));
		}
	}
}

/*
 * set_function_pathlist
 *		为函数 RTE 建立（唯一一条）访问路径
 */
static void
set_function_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;
	List	   *pathkeys = NIL;

	/*
	 * 不支持把连接子句推进函数
	 * 扫描的限定条件，但它仍可能因函数表达式中的 LATERAL
	 * 引用而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/*
	 * 除非使用了 ORDINALITY，否则结果视为无序；若使用了，
	 * 则按序号列（最后一列）排序。通过检查等价类中对该 Var 的使用，看我们是否
	 * 在意。
	 */
	if (rte->funcordinality)
	{
		AttrNumber	ordattno = rel->max_attr;
		Var		   *var = NULL;
		ListCell   *lc;

		/*
		 * 关系的目标列表里有它的 Var 吗？若没有，查询就
		 * 没有引用序号列，或者至少没有以任何
		 * 对排序有意义的方式引用。
		 */
		foreach(lc, rel->reltarget->exprs)
		{
			Var		   *node = (Var *) lfirst(lc);

			/* 检查 varno/varlevelsup 只是偏执 */
			if (IsA(node, Var) &&
				node->varattno == ordattno &&
				node->varno == rel->relid &&
				node->varlevelsup == 0)
			{
				var = node;
				break;
			}
		}

		/*
		 * 试着用 int8 排序为这个 Var 建立路径键。告诉
		 * build_expression_pathkey 不要建立任何新的等价类；若
		 * 该 Var 尚未在某个等价类中被提到，就表示没有东西
		 * 在意这个排序。
		 */
		if (var)
			pathkeys = build_expression_pathkey(root,
												(Expr *) var,
												Int8LessOperator,
												rel->relids,
												false);
	}

	/* 生成合适的路径 */
	add_path(rel, create_functionscan_path(root, rel,
										   pathkeys, required_outer));
}

/*
 * set_values_pathlist
 *		为 VALUES RTE 建立（唯一一条）访问路径
 */
static void
set_values_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * 不支持把连接子句推进 values 扫描的限定条件，
	 * 但它仍可能因值表达式中的 LATERAL 引用
	 * 而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_valuesscan_path(root, rel, required_outer));
}

/*
 * set_tablefunc_pathlist
 *		为表函数 RTE 建立（唯一一条）访问路径
 */
static void
set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * 不支持把连接子句推进表函数
	 * 扫描的限定条件，但它仍可能因函数表达式中的 LATERAL
	 * 引用而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_tablefuncscan_path(root, rel,
											required_outer));
}

/*
 * set_cte_pathlist
 *		为非自引用 CTE RTE 建立（唯一一条）访问路径
 *
 * 不需要单独的 set_cte_size 阶段，因为不
 * 支持 CTEs 的连接限定条件参数化路径。
 */
static void
set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Plan	   *cteplan;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;
	int			plan_id;
	Relids		required_outer;

	/*
	 * 找出被引用的 CTE，并定位先前为它做好的计划。
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* 不应当发生 */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * 注意：若仍在
	 * 规划 CTEs（即这是来自另一个 CTE 的旁路引用），cte_plan_ids 可以比 cteList 短。
	 * 因此这里不得使用 forboth。
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* 不应当发生 */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	if (plan_id <= 0)
		elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
	cteplan = (Plan *) list_nth(root->glob->subplans, plan_id - 1);

	/* 用估计的输出行数、宽度等标记该关系 */
	set_cte_size_estimates(root, rel, cteplan->plan_rows);

	/*
	 * 不支持把连接子句推进 CTE 扫描的限定条件，但它
	 * 仍可能因目标列表中的 LATERAL 引用
	 * 而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_ctescan_path(root, rel, required_outer));
}

/*
 * set_namedtuplestore_pathlist
 *		为命名 tuplestore RTE 建立（唯一一条）访问路径
 *
 * 不需要单独的 set_namedtuplestore_size 阶段，因为我们
 * 不支持元组存储的连接限定条件参数化路径。
 */
static void
set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte)
{
	Relids		required_outer;

	/* 用估计的输出行数、宽度等标记该关系 */
	set_namedtuplestore_size_estimates(root, rel);

	/*
	 * 不支持把连接子句推进 tuplestore
	 * 扫描的限定条件，但它仍可能因目标列表中的 LATERAL
	 * 引用而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_namedtuplestorescan_path(root, rel, required_outer));

	/* 选出最便宜的路径（这种情况下很容易……） */
	set_cheapest(rel);
}

/*
 * set_result_pathlist
 *		为 RTE_RESULT RTE 建立（唯一一条）访问路径
 *
 * 不需要单独的 set_result_size 阶段，因为我们
 * 不支持这些 RTEs 的连接限定条件参数化路径。
 */
static void
set_result_pathlist(PlannerInfo *root, RelOptInfo *rel,
					RangeTblEntry *rte)
{
	Relids		required_outer;

	/* 用估计的输出行数、宽度等标记该关系 */
	set_result_size_estimates(root, rel);

	/*
	 * 不支持把连接子句推进 Result 扫描的限定条件，
	 * 但它仍可能因目标列表中的 LATERAL 引用
	 * 而需要参数化。
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_resultscan_path(root, rel, required_outer));

	/* 选出最便宜的路径（这种情况下很容易……） */
	set_cheapest(rel);
}

/*
 * set_worktable_pathlist
 *		为自引用 CTE RTE 建立（唯一一条）访问路径
 *
 * 不需要单独的 set_worktable_size 阶段，因为不
 * 支持 CTEs 的连接限定条件参数化路径。
 */
static void
set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Path	   *ctepath;
	PlannerInfo *cteroot;
	Index		levelsup;
	Relids		required_outer;

	/*
	 * 需要找到非递归项的路径，它位于正在处理
	 * 递归 UNION 的计划层次，
	 * 比 CTE 的来源低 *一层*。
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* 不应当发生 */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* 不应当发生 */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	ctepath = cteroot->non_recursive_path;
	if (!ctepath)				/* 不应当发生 */
		elog(ERROR, "could not find path for CTE \"%s\"", rte->ctename);

	/* 用估计的输出行数、宽度等标记该关系 */
	set_cte_size_estimates(root, rel, ctepath->rows);

	/*
	 * 不支持把连接子句推进工作表
	 * 扫描的限定条件，但它仍可能因目标列表中的 LATERAL
	 * 引用而需要参数化。（鉴于对递归引用的
	 * 限制，我不确定这实际上是否可能，但支持起来很容易。）
	 */
	required_outer = rel->lateral_relids;

	/* 生成合适的路径 */
	add_path(rel, create_worktablescan_path(root, rel, required_outer));
}

/*
 * generate_gather_paths
 *		通过在部分路径之上加 Gather 或
 *		Gather Merge，为关系生成并行访问路径。
 *
 * 在为指定关系建完所有部分路径之前，不得调用本函数。
 * （否则 add_partial_path 可能删掉某条
 * GatherPath 或 GatherMergePath 还引用着的路径。）
 *
 * 若正在为扫描或连接关系生成路径，override_rows 将
 * 为 false，就直接用该关系的规模估计。但当
 * 为部分分组路径调用时，需要覆盖
 * 行数估计。（不清楚这里用的特定值
 * 是否真的最好，但底层关系没有估计，因此
 * 必须做点什么。）
 */
void
generate_gather_paths(PlannerInfo *root, RelOptInfo *rel, bool override_rows)
{
	Path	   *cheapest_partial_path;
	Path	   *simple_gather_path;
	ListCell   *lc;
	double		rows;
	double	   *rowsp = NULL;

	/* 若没有部分路径，这里就无事可做。 */
	if (rel->partial_pathlist == NIL)
		return;

	/* 是否应当覆盖该关系的行数估计？ */
	if (override_rows)
		rowsp = &rows;

	/*
	 * Gather 的输出总是无序的，因此只有一条部分
	 * 路径有意义：最便宜的那条。由于 add_partial_path 的工作方式，它会在
	 * partial_pathlist 的最前面。
	 */
	cheapest_partial_path = linitial(rel->partial_pathlist);
	rows =
		cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
	simple_gather_path = (Path *)
		create_gather_path(root, rel, cheapest_partial_path, rel->reltarget,
						   NULL, rowsp);
	add_path(rel, simple_gather_path);

	/*
	 * 对每种有用的排序，可以考虑保序的 Gather
	 * Merge。
	 */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		GatherMergePath *path;

		if (subpath->pathkeys == NIL)
			continue;

		rows = subpath->rows * subpath->parallel_workers;
		path = create_gather_merge_path(root, rel, subpath, rel->reltarget,
										subpath->pathkeys, NULL, rowsp);
		add_path(rel, &path->path);
	}
}

/*
 * get_useful_pathkeys_for_relation
 *		判断一个关系的哪些排序可能有用。
 *
 * 按排序取得数据可能有用，要么因为所请求的
 * 顺序匹配正在规划的整个查询的最终输出顺序，
 * 要么因为它使高效的归并连接成为可能。这里试图
 * 弄清要考虑哪些路径键。
 *
 * 这允许我们在 gather
 * merge 节点下的索引扫描之上做增量排序，也就是并行化。
 *
 * 若 require_parallel_safe 为 true，还需要这些表达式
 * 是并行安全的（这允许把排序推到 Gather Merge 之下）。
 *
 * XXX 目前这最多只能返回只有一个元素的列表，
 * 因为它只看 query_pathkeys。因此也许可以直接返回路径键，
 * 但将来似乎有可能想考虑其他排序。
 * 例如，也许想考虑对
 * 归并连接有用的路径键。
 */
static List *
get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel,
								 bool require_parallel_safe)
{
	List	   *useful_pathkeys_list = NIL;

	/*
	 * 考虑 query_pathkeys 总是值得的，因为它也许允许
	 * 在已有部分预排序路径时避免一次完全排序，
	 * 或者把完全排序推进查询的
	 * 并行部分。
	 */
	if (root->query_pathkeys)
	{
		ListCell   *lc;
		int			npathkeys = 0;	/* 有用的路径键 */

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);
			EquivalenceClass *pathkey_ec = pathkey->pk_eclass;

			/*
			 * 只能为含有可从当前
			 * 关系的 reltarget 算出、且可安全提前计算的等价类成员的路径键建立排序，
			 * 因此一旦
			 * 发现某个路径键没有这样的成员，就忽略列表的其余部分。
			 *
			 * 返回满足这一要求的路径键列表的任何前缀
			 * 仍然值得，因为也许能做
			 * 增量排序。
			 *
			 * 若有请求，也确保排序表达式是并行安全的。
			 */
			if (!relation_can_be_sorted_early(root, rel, pathkey_ec,
											  require_parallel_safe))
				break;

			npathkeys++;
		}

		/*
		 * 整个 query_pathkeys 列表都匹配，因此直接追加它，以便
		 * 通过比较列表指针来轻松比较路径键。若
		 * 必须截断路径键，就得做一份副本。
		 */
		if (npathkeys == list_length(root->query_pathkeys))
			useful_pathkeys_list = lappend(useful_pathkeys_list,
										   root->query_pathkeys);
		else if (npathkeys > 0)
			useful_pathkeys_list = lappend(useful_pathkeys_list,
										   list_copy_head(root->query_pathkeys,
														  npathkeys));
	}

	return useful_pathkeys_list;
}

/*
 * generate_useful_gather_paths
 *		通过在部分路径之上加 Gather 或
 *		Gather Merge，为关系生成并行访问路径。
 *
 * 与普通的 generate_gather_paths 不同，它既看输入
 * 路径的路径键（旨在保留排序），也考虑对 gather merge 节点之上的节点
 * 可能有用的排序，并试着加上
 * 排序（普通或增量）来提供它。
 */
void
generate_useful_gather_paths(PlannerInfo *root, RelOptInfo *rel, bool override_rows)
{
	ListCell   *lc;
	double		rows;
	double	   *rowsp = NULL;
	List	   *useful_pathkeys_list = NIL;
	Path	   *cheapest_partial_path = NULL;

	/* 若没有部分路径，这里就无事可做。 */
	if (rel->partial_pathlist == NIL)
		return;

	/* 是否应当覆盖该关系的行数估计？ */
	if (override_rows)
		rowsp = &rows;

	/* 生成普通的 gather（merge）路径 */
	generate_gather_paths(root, rel, override_rows);

	/* 为有意义的排序考虑增量排序 */
	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel, true);

	/* 用于显式（完全）排序路径 */
	cheapest_partial_path = linitial(rel->partial_pathlist);

	/*
	 * 为每种有意义的排序考虑已排序路径。既生成
	 * 增量排序也生成完全排序。
	 */
	foreach(lc, useful_pathkeys_list)
	{
		List	   *useful_pathkeys = lfirst(lc);
		ListCell   *lc2;
		bool		is_sorted;
		int			presorted_keys;

		foreach(lc2, rel->partial_pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc2);
			GatherMergePath *path;

			is_sorted = pathkeys_count_contained_in(useful_pathkeys,
													subpath->pathkeys,
													&presorted_keys);

			/*
			 * 不必考虑子路径已经完全排好序的情形，因为 generate_gather_paths 已经为每条
			 * 带有路径键的子路径建立了
			 * gather merge 路径。
			 *
			 * 但既然子路径已经排好序，就知道不必
			 * 考虑在它上面再加排序（完全或增量），
			 * 因此可以在这里继续。
			 */
			if (is_sorted)
				continue;

			/*
			 * 至少试着对最便宜的路径排序，
			 * 也试着对任何已经部分排好序的路径
			 * 做增量排序
			 * （除非它是最便宜的输入路径，
			 * 否则在禁用增量排序时不必处理已有预排序键的路径）。
			 */
			if (subpath != cheapest_partial_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * 对任何未预排序的路径，或在
			 * 禁用增量排序时，考虑普通排序。不必对同一条路径同时考虑
			 * 排序和增量排序。假定
			 * 有预排序
			 * 键时增量排序总是更快。
			 *
			 * 这与 generate_gather_paths 中建立的 gather 路径并不重复，
			 * 因为那个不生成有序
			 * 输出。这里加上显式排序来匹配有用的
			 * 排序。
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
			{
				subpath = (Path *) create_sort_path(root,
													rel,
													subpath,
													useful_pathkeys,
													-1.0);
				rows = subpath->rows * subpath->parallel_workers;
			}
			else
				subpath = (Path *) create_incremental_sort_path(root,
																rel,
																subpath,
																useful_pathkeys,
																presorted_keys,
																-1);
			path = create_gather_merge_path(root, rel,
											subpath,
											rel->reltarget,
											subpath->pathkeys,
											NULL,
											rowsp);

			add_path(rel, &path->path);
		}
	}
}

/*
 * make_rel_from_joinlist
 *	  用 "joinlist" 指导连接路径搜索，建立访问路径。
 *
 * joinlist 数据结构的定义见
 * deconstruct_jointree() 的注释。
 */
static RelOptInfo *
make_rel_from_joinlist(PlannerInfo *root, List *joinlist)
{
	int			levels_needed;
	List	   *initial_rels;
	ListCell   *jl;

	/*
	 * 数子 joinlist 节点的个数。这是为考虑连接
	 * 这些子节点的全部方式而必须采用的
	 * 动态规划算法的深度。
	 */
	levels_needed = list_length(joinlist);

	if (levels_needed <= 0)
		return NULL;			/* 无事可做？ */

	/*
	 * 构造一份与子 joinlist 节点对应的关系列表。
	 * 其中可能既有基关系，也有按
	 * 子 joinlist 构造的关系。
	 */
	initial_rels = NIL;
	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);
		RelOptInfo *thisrel;

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			thisrel = find_base_rel(root, varno);
		}
		else if (IsA(jlnode, List))
		{
			/* 递归处理子问题 */
			thisrel = make_rel_from_joinlist(root, (List *) jlnode);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
			thisrel = NULL;		/* 消除编译器告警 */
		}

		initial_rels = lappend(initial_rels, thisrel);
	}

	if (levels_needed == 1)
	{
		/*
		 * 只有一个 joinlist 节点，因此做完了。
		 */
		return (RelOptInfo *) linitial(initial_rels);
	}
	else
	{
		/*
		 * 考虑连接这些关系的不同顺序，
		 * 使用插件、GEQO 或常规连接搜索代码。
		 *
		 * 把 initial_rels 列表放进 PlannerInfo 的一个字段，因为
		 * has_legal_joinclause() 需要查看它（难看 :-()。
		 */
		root->initial_rels = initial_rels;

		if (join_search_hook)
			return (*join_search_hook) (root, levels_needed, initial_rels);
		else if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		else
			return standard_join_search(root, levels_needed, initial_rels);
	}
}

/*
 * standard_join_search
 *	  通过相继找出把组成关系连成连接关系的方式，
 *	  找出查询可能的连接路径。
 *
 * 'levels_needed' 是所需迭代次数，即查询中
 *		独立连接树项的个数。它 > 1。
 *
 * 'initial_rels' 是每个独立
 *		连接树项的 RelOptInfo 节点列表。这些是要连在一起的组成部分。
 *		注意 levels_needed == list_length(initial_rels)。
 *
 * 返回连接关系的最终层，即把
 * 全部原始关系连在一起的结果关系。
 * 必须为该关系以及
 * 全部所需的子关系提供至少一条实现路径。
 *
 * 为了支持通过改变
 * 连接搜索算法来修改规划器行为的可装载插件，提供一个钩子变量，让插件
 * 替换或补充本函数。任何这样的钩子都必须返回与
 * 标准代码相同的最终连接关系，但它可以挂上
 * 不同的实现路径集合，而且只需实例化这些路径
 * 所需的子连接关系。
 *
 * 致插件作者：standard_join_search() 期间调用的函数会
 * 修改 root->join_rel_list 和 root->join_rel_hash。若想做
 * 不止一次连接顺序搜索，大概需要保存并恢复这些
 * 数据结构的原始状态。例子见 geqo_eval()。
 */
RelOptInfo *
standard_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	int			lev;
	RelOptInfo *rel;

	/*
	 * 本函数无法在同一个规划
	 * 问题内递归调用，因此 join_rel_level[] 此时也无法已在使用。
	 */
	Assert(root->join_rel_level == NULL);

	/*
	 * 采用一种简单的 "动态规划" 算法：先找出把两个连接树项
	 * 连起来的全部方式，再找出把三个项连起来的全部方式
	 * （由两项连接和单项组成），然后是四项
	 * 连接，如此直到考虑过把全部
	 * 项连成一个关系的全部方式。
	 *
	 * root->join_rel_level[j] 是全部 j 项关系的列表。最初把
	 * root->join_rel_level[1] 设成表示全部单项连接树
	 * 关系。
	 */
	root->join_rel_level = (List **) palloc0((levels_needed + 1) * sizeof(List *));

	root->join_rel_level[1] = initial_rels;

	for (lev = 2; lev <= levels_needed; lev++)
	{
		ListCell   *lc;

		/*
		 * 确定在这一
		 * 层要连接的全部可能关系对，并从每一对可用的
		 * 更低层关系为每一对建立路径。
		 */
		join_search_one_level(root, lev);

		/*
		 * 对每个刚处理完的连接关系运行 generate_partitionwise_join_paths() 和
		 * generate_useful_gather_paths()。我们
		 * 无法更早做这件事，因为普通路径和部分路径
		 * 都可能在 join_search_one_level 内多次
		 * 被加到某个特定连接关系上。
		 *
		 * 此后就完成了为该连接关系建立路径，因此运行
		 * set_cheapest()。
		 */
		foreach(lc, root->join_rel_level[lev])
		{
			rel = (RelOptInfo *) lfirst(lc);

			/* 为按分区连接建立路径。 */
			generate_partitionwise_join_paths(root, rel);

			/*
			 * 除了最顶层的扫描/连接关系，考虑收集
			 * 部分路径。对最顶层的扫描/连接关系，一旦知道最终目标列表，也会这样做
			 * （见 grouping_planner）。
			 */
			if (!bms_equal(rel->relids, root->all_query_rels))
				generate_useful_gather_paths(root, rel, false);

			/* 找出并保存该关系的最便宜路径 */
			set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
			debug_print_rel(root, rel);
#endif
		}
	}

	/*
	 * 最终层应当有单个关系。
	 */
	if (root->join_rel_level[levels_needed] == NIL)
		elog(ERROR, "failed to build any %d-way joins", levels_needed);
	Assert(list_length(root->join_rel_level[levels_needed]) == 1);

	rel = (RelOptInfo *) linitial(root->join_rel_level[levels_needed]);

	root->join_rel_level = NULL;

	return rel;
}

/*****************************************************************************
 *			PUSHING QUALS DOWN INTO SUBQUERIES（把限定条件下推到子查询）
 *****************************************************************************/

/*
 * subquery_is_pushdown_safe - 子查询对下推限定条件是否安全？
 *
 * subquery 是正在检查的特定组成查询。topquery
 * 是集合运算树的顶层组成（若没有
 * 集合运算，就是同一个 Query）。
 *
 * 这里检查的条件：
 *
 * 1. 若子查询有 LIMIT 子句，不得下推任何限定条件，
 * 因为那会改变返回的行集合。
 *
 * 2. 若子查询含有 EXCEPT 或 EXCEPT ALL 集合运算，就没法把
 * 限定条件推进去，因为那会改变结果。
 *
 * 3. 若子查询使用 DISTINCT，就没法把易变限定条件推进去。
 * 这是因为上层限定条件在语义上应当只对
 * 每个不同行求值一次，而不是对每个原始行，若限定条件是
 * 易变的，额外求值会改变结果。（这个问题
 * 不适用于 GROUP BY 等其他聚合形式，因为
 * 那些存在时我们推进 HAVING 而不是 WHERE，从而使限定条件
 * 仍在聚合之后应用。）
 *
 * 4. 若子查询含有窗口函数，就没法把易变限定条件推进
 * 去。这里的问题与 DISTINCT 略有不同：易变限定条件
 * 可能对窗口分区的某些行成功、对另一些失败，
 * 从而改变分区内容，进而改变剩余行上窗口函数的
 * 结果。
 *
 * 5. 若子查询的目标列表中有任何集合返回函数，
 * 就没法把易变限定条件推进去。那会把它们推到 SRFs
 * 之下，从而改变它们被求值的次数。另外，
 * 易变限定条件可能对某些 SRF 输出行成功、对另一些失败，
 * 若它在 SRF 展开之前求值，这种行为就没法发生。
 *
 * 6. 若子查询有非空分组集，就没法下推任何
 * 限定条件。这里的担心是，引用 "常量" 分组
 * 列的限定条件可能被常量折叠，这不恰当，因为该值
 * 可能因分组集展开而为 null。若有一种语法树表示能表明这类
 * 分组列其实不是常量，就可以去掉这个限制。（还有其他想法
 * 可以用来放宽这个限制，但那是将来
 * 最可能采用的办法。注意，只要 subquery_planner 没法在这样的
 * 子查询内部把 HAVING 子句移到 WHERE，
 * 收益就不大。）
 * 另外，对子查询的输出列做几项检查，看
 *
 * 在下推的限定条件中引用它们是否安全。若发现输出列 k
 * 不宜引用，就把原因设进
 * safetyInfo->unsafeFlags[k]，但不因此整体拒绝子查询，因为
 * 有些/全部限定条件也许不引用列 k。unsafeFlags[]
 * 数组以后由 qual_is_pushdown_safe() 查阅。这样做
 * 比直接在 qual_is_pushdown_safe() 里检查更好，
 * 因为子查询涉及集合运算时，必须检查
 * 集合运算每一支的输出表达式。
 * 注意：把限定条件推进 DISTINCT 子查询在理论上可疑：
 *
 * 我们实际上假定限定条件没法区分
 * DISTINCT 的等值操作符视为相等的值，然而对此假定有许多
 * 反例。不过，对 DISTINCT 子查询使用这样的限定条件
 * 反正也不安全，因为没有保证 DISTINCT 运算会选哪个
 * "相等" 值作为输出值。
 * 因此不太担心这一点。另一个反对意见是，若
 * 限定条件求值很贵，对每个原始行运行它的代价
 * 可能高于在 DISTINCT 步骤之前消去行所节省的。但在这个阶段
 * 很难估计这一点，而且实践中下推
 * 似乎很少让事情变差，因此也忽略这个问题。
 * 注意：同样，把限定条件推进带窗口函数的子查询也
 *
 * 有点可疑：限定条件可能去掉窗口分区的某些行而
 * 留下另一些，导致剩余行上窗口函数的结果发生变化。
 * 我们坚持这样的限定条件只引用分区
 * 列，但那同样只在限定条件不区分
 * 分区等值操作符视为相等的值时才保护我们。这里的风险
 * 也许比 DISTINCT 更大，因为不会对行去重，
 * 从而这样的限定条件没有理论上的问题。但
 * 仍然这样做，因为潜在的性能收益非常
 * 大，而且对长期可比的
 * 行为没有见过现场投诉，那与
 * DISTINCT 相当。
 */
static bool
subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  pushdown_safety_info *safetyInfo)
{
	SetOperationStmt *topop;

	/* 检查第 1 点 */
	if (subquery->limitOffset != NULL || subquery->limitCount != NULL)
		return false;

	/* 检查第 6 点 */
	if (subquery->groupClause && subquery->groupingSets)
		return false;

	/* 检查第 3、4、5 点 */
	if (subquery->distinctClause ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs)
		safetyInfo->unsafeVolatile = true;

	/*
	 * 若在叶子查询，就检查其目标
	 * 列表中的不安全表达式，并把它们不安全的原因标进 unsafeFlags[]。
	 * （集合运算树中的非叶子节点的目标列表里只有简单 Vars，
	 * 因此不必检查它们。）
	 */
	if (subquery->setOperations == NULL)
		check_output_expressions(subquery, safetyInfo);

	/* 是在顶层，还是在看集合运算的组成？ */
	if (subquery == topquery)
	{
		/* 顶层，因此检查任何组成查询 */
		if (subquery->setOperations != NULL)
			if (!recurse_pushdown_safe(subquery->setOperations, topquery,
									   safetyInfo))
				return false;
	}
	else
	{
		/* 集合运算组成不得再有更多组成（太怪异） */
		if (subquery->setOperations != NULL)
			return false;
		/* 检查集合运算组成的输出类型是否与顶层匹配 */
		topop = castNode(SetOperationStmt, topquery->setOperations);
		Assert(topop);
		compare_tlist_datatypes(subquery->targetList,
								topop->colTypes,
								safetyInfo);
	}
	return true;
}

/*
 * 递归遍历 setOperations 树的辅助例程
 */
static bool
recurse_pushdown_safe(Node *setOp, Query *topquery,
					  pushdown_safety_info *safetyInfo)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		return subquery_is_pushdown_safe(subquery, topquery, safetyInfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* EXCEPT 不行（subquery_is_pushdown_safe 的第 2 点） */
		if (op->op == SETOP_EXCEPT)
			return false;
		/* 否则递归 */
		if (!recurse_pushdown_safe(op->larg, topquery, safetyInfo))
			return false;
		if (!recurse_pushdown_safe(op->rarg, topquery, safetyInfo))
			return false;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
	return true;
}

/*
 * check_output_expressions - 检查子查询输出表达式是否安全
 *
 * 有几种情形，若上层
 * 限定条件引用子查询的某个输出列，下推就不安全。我们检查
 * 子查询的每个输出列，发现该
 * 列不宜被下推限定条件引用时，就在 unsafeFlags[k] 中设标志。这里检查的
 * 条件是：
 *
 * 1. 不得下推任何引用返回集合的子选择输出的限定条件，
 * 否则会把返回集合的函数引进
 * 子查询的 WHERE/HAVING 限定条件。
 *
 * 2. 不得下推任何引用含有
 * 易变函数的子选择输出的限定条件，以免因
 * 多次求值易变函数而引入奇怪结果。
 *
 * 3. 若子查询使用 DISTINCT ON，不得下推任何引用
 * 非 DISTINCT 输出列的限定条件，因为那会改变返回的
 * 行集合。（对 DISTINCT 这个条件是空的，因为那时
 * 没有非 DISTINCT 输出列，所以不必检查。注意
 * 若有 DISTINCT 或 DISTINCT ON，subquery_is_pushdown_safe 已经报告
 * 不能使用易变
 *
 * 限定条件。）
 * 4. 若子查询有任何窗口函数，不得下推引用
 * 未列在该子查询全部
 * 窗口 PARTITION BY 子句中的任何输出列的限定条件。可以下推只使用
 * 分区列的限定条件，因为它们对
 * 任一窗口分区的每一行应当同样成功或失败，完全排除某些
 * 分区不会改变窗口函数对其余
 * 分区的结果。（同样，这也需要非易变限定条件，但
 * subquery_is_pushdown_safe 处理那一点。）因这个原因被标为
 * 不安全的子查询列仍可以下推 WindowClause 运行条件。
 */
static void
check_output_expressions(Query *subquery, pushdown_safety_info *safetyInfo)
{
	ListCell   *lc;

	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;			/* 忽略 resjunk 列 */

		/* 返回集合的函数不安全（第 1 点） */
		if (subquery->hasTargetSRFs &&
			(safetyInfo->unsafeFlags[tle->resno] &
			 UNSAFE_HAS_SET_FUNC) == 0 &&
			expression_returns_set((Node *) tle->expr))
		{
			safetyInfo->unsafeFlags[tle->resno] |= UNSAFE_HAS_SET_FUNC;
			continue;
		}

		/* 易变函数不安全（第 2 点） */
		if ((safetyInfo->unsafeFlags[tle->resno] &
			 UNSAFE_HAS_VOLATILE_FUNC) == 0 &&
			contain_volatile_functions((Node *) tle->expr))
		{
			safetyInfo->unsafeFlags[tle->resno] |= UNSAFE_HAS_VOLATILE_FUNC;
			continue;
		}

		/* 若子查询使用 DISTINCT ON，检查第 3 点 */
		if (subquery->hasDistinctOn &&
			(safetyInfo->unsafeFlags[tle->resno] &
			 UNSAFE_NOTIN_DISTINCTON_CLAUSE) == 0 &&
			!targetIsInSortList(tle, InvalidOid, subquery->distinctClause))
		{
			/* 非 DISTINCT 列，因此标成不安全 */
			safetyInfo->unsafeFlags[tle->resno] |= UNSAFE_NOTIN_DISTINCTON_CLAUSE;
			continue;
		}

		/* 若子查询使用窗口函数，检查第 4 点 */
		if (subquery->hasWindowFuncs &&
			(safetyInfo->unsafeFlags[tle->resno] &
			 UNSAFE_NOTIN_DISTINCTON_CLAUSE) == 0 &&
			!targetIsInAllPartitionLists(tle, subquery))
		{
			/* 并非出现在全部 PARTITION BY 子句中，因此标成不安全 */
			safetyInfo->unsafeFlags[tle->resno] |= UNSAFE_NOTIN_PARTITIONBY_CLAUSE;
			continue;
		}
	}
}

/*
 * 对使用 UNION/UNION ALL/INTERSECT/INTERSECT ALL 的子查询，可以把
 * 限定条件推进每个组成查询，但限定条件只能引用
 * 在集合运算中不遭受类型强制的子查询列。
 * 否则可能有语义陷阱。因此，检查
 * 组成查询，看它们是否有与
 * 顶层集合运算输出不同的输出类型。若列 k 在任何组成中类型不同，就在
 * unsafeFlags[k] 中设置 UNSAFE_TYPE_MISMATCH 位。
 *
 * 这里不必关心 typmod：集合运算输入与输出 typmod 之间唯一允许的差别是
 * 输入是特定 typmod
 * 而输出是 -1，那不需要强制转换。
 *
 * tlist 是子查询的目标列表。
 * colTypes 是顶层集合运算输出列类型的 OID 列表。
 * safetyInfo 是要为其设置 unsafeFlags[] 的 pushdown_safety_info。
 */
static void
compare_tlist_datatypes(List *tlist, List *colTypes,
						pushdown_safety_info *safetyInfo)
{
	ListCell   *l;
	ListCell   *colType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* 忽略 resjunk 列 */
		if (colType == NULL)
			elog(ERROR, "wrong number of tlist entries");
		if (exprType((Node *) tle->expr) != lfirst_oid(colType))
			safetyInfo->unsafeFlags[tle->resno] |= UNSAFE_TYPE_MISMATCH;
		colType = lnext(colTypes, colType);
	}
	if (colType != NULL)
		elog(ERROR, "wrong number of tlist entries");
}

/*
 * targetIsInAllPartitionLists
 *		若 TargetEntry 列在查询中定义的每个窗口的
 *		PARTITION BY 子句中，则为真。
 *
 * 忽略任何窗口函数实际未使用的窗口是安全的，
 * 但在这个阶段不容易得到该信息；而且
 * 多花周期去取得它不大可能有用，因为
 * 未被引用的窗口定义在实践中大概不常见。
 */
static bool
targetIsInAllPartitionLists(TargetEntry *tle, Query *query)
{
	ListCell   *lc;

	foreach(lc, query->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		if (!targetIsInSortList(tle, InvalidOid, wc->partitionClause))
			return false;
	}
	return true;
}

/*
 * qual_is_pushdown_safe - 某条 rinfo 下推是否安全？
 *
 * rinfo 是应用于给定子查询的限制子句（其 RTE
 * 在父查询中的索引为 rti）。
 *
 * 这里检查的条件：
 *
 * 1. rinfo 的子句不得含有任何 SubPlans（主要是因为
 * 不清楚它能否正确工作：限定条件中的 SubLinks 已经
 * 被变换成 SubPlans，但子查询中还没有）。注意
 * 变换成 initplan 的 SubLinks 是安全的，这里会接受，
 * 因为在限定条件中看到的只是引用该 initplan
 * 输出的 Param。
 *
 * 2. 若设置了 unsafeVolatile，rinfo 的子句不得含有任何易变
 * 函数。
 *
 * 3. 若设置了 unsafeLeaky，rinfo 的子句不得含有任何被传入 Var 节点、
 * 因而可能通过副作用泄露
 * 子查询中的值的泄漏函数。
 *
 * 4. rinfo 的子句不得引用子查询的整行输出
 * （因为在子查询内部没有简单办法命名它）。
 *
 * 5. rinfo 的子句不得引用 subquery_is_pushdown_safe() 已发现
 * 不宜引用的任何子查询输出列。
 */
static pushdown_safe_type
qual_is_pushdown_safe(Query *subquery, Index rti, RestrictInfo *rinfo,
					  pushdown_safety_info *safetyInfo)
{
	pushdown_safe_type safe = PUSHDOWN_SAFE;
	Node	   *qual = (Node *) rinfo->clause;
	List	   *vars;
	ListCell   *vl;

	/* 拒绝子选择（第 1 点） */
	if (contain_subplans(qual))
		return PUSHDOWN_UNSAFE;

	/* 若发现易变限定条件不安全，就拒绝它们（第 2 点） */
	if (safetyInfo->unsafeVolatile &&
		contain_volatile_functions((Node *) rinfo))
		return PUSHDOWN_UNSAFE;

	/* 若被告知，就拒绝有泄漏风险的限定条件（第 3 点） */
	if (safetyInfo->unsafeLeaky &&
		contain_leaked_vars(qual))
		return PUSHDOWN_UNSAFE;

	/*
	 * 检查子句中使用的全部 Vars。既然它是限制子句，所有
	 * 这样的 Vars 都必须引用子选择的输出列……除非这是
	 * LATERAL 子查询的一部分，那时可能有横向
	 * 引用。
	 *
	 * 省掉相关标志，这也给了我们一次廉价的健全性检查：
	 * 限定条件中没有聚合或窗口函数。那些
	 * 下推是不安全的，但至少目前在限定条件里
	 * 反正看不到它们。
	 */
	vars = pull_var_clause(qual, PVC_INCLUDE_PLACEHOLDERS);
	foreach(vl, vars)
	{
		Var		   *var = (Var *) lfirst(vl);

		/*
		 * XXX 若在限制子句中发现任何 PlaceHolderVars，就放弃。
		 * 不清楚 PHV 能否安全下推，更
		 * 不清楚实际有意义的情形里是否会出现这种情况。
		 * 因此目前就拒绝
		 * 下推。
		 */
		if (!IsA(var, Var))
		{
			safe = PUSHDOWN_UNSAFE;
			break;
		}

		/*
		 * 若发现任何横向引用，就放弃。把这些
		 * 下推是安全的，但必须把它们转换成外层引用，
		 * 而 subquery_push_qual 缺少做这件事的基础设施。这种情形
		 * 出现得太少，似乎不值得费力。
		 */
		if (var->varno != rti)
		{
			safe = PUSHDOWN_UNSAFE;
			break;
		}

		/* 子查询没有系统列 */
		Assert(var->varattno >= 0);

		/* 检查第 4 点 */
		if (var->varattno == 0)
		{
			safe = PUSHDOWN_UNSAFE;
			break;
		}

		/* 检查第 5 点 */
		if (safetyInfo->unsafeFlags[var->varattno] != 0)
		{
			if (safetyInfo->unsafeFlags[var->varattno] &
				(UNSAFE_HAS_VOLATILE_FUNC | UNSAFE_HAS_SET_FUNC |
				 UNSAFE_NOTIN_DISTINCTON_CLAUSE | UNSAFE_TYPE_MISMATCH))
			{
				safe = PUSHDOWN_UNSAFE;
				break;
			}
			else
			{
				/* UNSAFE_NOTIN_PARTITIONBY_CLAUSE 对运行条件是可以的 */
				safe = PUSHDOWN_WINDOWCLAUSE_RUNCOND;
				/* 不要中断，也许还会发现另一个不安全的 Var */
			}
		}
	}

	list_free(vars);

	return safe;
}

/*
 * subquery_push_qual - 下推一条已判定为安全的限定条件
 */
static void
subquery_push_qual(Query *subquery, RangeTblEntry *rte, Index rti, Node *qual)
{
	if (subquery->setOperations != NULL)
	{
		/* 递归，分别下推到每个组成查询 */
		recurse_push_qual(subquery->setOperations, subquery,
						  rte, rti, qual);
	}
	else
	{
		/*
		 * 需要替换限定条件中的 Vars（它们必须引用
		 * 子查询的输出），换成子查询目标列表表达式的副本。
		 * 注意此时限定条件中任何上层 Vars 应当已经
		 * 被换成 Params，因此它们不需要处理。
		 *
		 * 这一步也确保在推进集合运算树时，
		 * 每个组成查询得到自己的一份限定条件副本。
		 */
		qual = ReplaceVarsFromTargetList(qual, rti, 0, rte,
										 subquery->targetList,
										 REPLACEVARS_REPORT_ERROR, 0,
										 &subquery->hasSubLinks);

		/*
		 * 现在把限定条件挂到正确的地方：通常是 WHERE，但若
		 * 子查询使用分组或聚合，就放进 HAVING（因为该
		 * 限定条件其实引用的是分组结果行）。
		 */
		if (subquery->hasAggs || subquery->groupClause || subquery->groupingSets || subquery->havingQual)
			subquery->havingQual = make_and_qual(subquery->havingQual, qual);
		else
			subquery->jointree->quals =
				make_and_qual(subquery->jointree->quals, qual);

		/*
		 * 不必改变子查询的 hasAggs 或 hasSubLinks 标志，
		 * 因为不能下推任何原先不在那里的聚合，
		 * 而且根本不下推子选择。
		 */
	}
}

/*
 * 递归遍历 setOperations 树的辅助例程
 */
static void
recurse_push_qual(Node *setOp, Query *topquery,
				  RangeTblEntry *rte, Index rti, Node *qual)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *subrte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = subrte->subquery;

		Assert(subquery != NULL);
		subquery_push_qual(subquery, rte, rti, qual);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		recurse_push_qual(op->larg, topquery, rte, rti, qual);
		recurse_push_qual(op->rarg, topquery, rte, rti, qual);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*****************************************************************************
 *			SIMPLIFYING SUBQUERY TARGETLISTS（简化子查询目标列表）
 *****************************************************************************/

/*
 * remove_unused_subquery_outputs
 *		去掉不需要的子查询目标列表项
 *
 * 上层查询很可能不会读取子查询的全部
 * 输出列。可以去掉子查询自身不需要的那些，
 * 例如作为排序/分组列，并且
 * 在其他方面不影响语义（例如易变函数不能去掉）的这类输出。
 * 这不仅因为也许能去掉计算昂贵的
 * 表达式而有用，也因为删掉输出列也许允许
 * 连接移除等优化在子查询内部发生。
 *
 * 可以把 extra_used_attrs 作为非 NULL 传入，以标记任何
 * （偏移 FirstLowInvalidHeapAttributeNumber）不应当去掉的列。本
 * 函数会修改该参数，因此调用方若在调用本函数之后
 * 还需要使用传入的 Bitmapset，就必须自己复制一份。
 *
 * 为避免影响目标列表中的列编号，并不物理地
 * 去掉未使用的目标列表项，而是把它们的表达式换成 NULL
 * 常量。这通过修改 subquery->targetList 实现。
 */
static void
remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel,
							   Bitmapset *extra_used_attrs)
{
	Bitmapset  *attrs_used;
	ListCell   *lc;

	/*
	 * 直接指向 extra_used_attrs。不必 bms_copy，因为
	 * 当前调用方在调用本函数之后都不使用该 Bitmapset。
	 */
	attrs_used = extra_used_attrs;

	/*
	 * 若子查询有 UNION/INTERSECT/EXCEPT，就什么都不做：原则上可以
	 * 更新所有子 SELECTs 的目标列表，但目前似乎不值得
	 * 费这个事。
	 */
	if (subquery->setOperations)
		return;

	/*
	 * 若子查询有普通 DISTINCT（不是 DISTINCT ON），就是在浪费
	 * 时间：它的全部输出列都必须用在 distinctClause 中。
	 */
	if (subquery->distinctClause && !subquery->hasDistinctOn)
		return;

	/*
	 * 收集上层
	 * 查询使用的全部输出列号的位图。
	 *
	 * 加上连接或最终输出所需的全部属性。注意：必须
	 * 看关系的目标列表，而不是 attr_needed 数据，因为继承子关系不算
	 * attr_needed，参见 set_append_rel_size()。
	 * （XXX 也许值得以后改掉这一点。）
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/* 加上未被下推的限制子句所使用的全部属性。 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * 若有对子查询的整行引用，就不能去掉
	 * 任何东西。
	 */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used))
		return;

	/*
	 * 遍历目标列表，清掉不需要的项。就地
	 * 修改目标列表项是可以的，因为 set_subquery_pathlist 已经
	 * 复制了子查询。
	 */
	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *texpr = (Node *) tle->expr;

		/*
		 * 若它有 sortgroupref 编号，就用在某条排序/分组
		 * 子句中，最好不要去掉。另外，不要去掉任何
		 * resjunk 列，因为它们存在的理由与
		 * 任何人读取子查询输出无关。（子 SELECT 中的
		 * resjunk 列很可能总是设置了 ressortgroupref，
		 * 但即使没有，去掉它们似乎也不慎重。）
		 */
		if (tle->ressortgroupref || tle->resjunk)
			continue;

		/*
		 * 若上层查询使用它，就不能去掉。
		 */
		if (bms_is_member(tle->resno - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
			continue;

		/*
		 * 若它含有集合返回函数，就不能去掉，因为
		 * 那会改变子查询返回的行数。
		 */
		if (subquery->hasTargetSRFs &&
			expression_returns_set(texpr))
			continue;

		/*
		 * 若它含有易变函数，就不敢去掉，以免
		 * 用户预期它们的副作用会发生。
		 */
		if (contain_volatile_functions(texpr))
			continue;

		/*
		 * 好，不需要它。把表达式换成 NULL 常量。
		 * 保留表达式暴露的类型，以防有东西
		 * 查看子查询结果的行类型。
		 */
		tle->expr = (Expr *) makeNullConst(exprType(texpr),
										   exprTypmod(texpr),
										   exprCollation(texpr));
	}
}

/*
 * create_partial_bitmap_paths
 *	  为该关系建立部分位图堆路径
 */
void
create_partial_bitmap_paths(PlannerInfo *root, RelOptInfo *rel,
							Path *bitmapqual)
{
	int			parallel_workers;
	double		pages_fetched;

	/* 为位图堆扫描计算堆页数 */
	pages_fetched = compute_bitmap_pages(root, rel, bitmapqual, 1.0,
										 NULL, NULL);

	parallel_workers = compute_parallel_worker(rel, pages_fetched, -1,
											   max_parallel_workers_per_gather);

	if (parallel_workers <= 0)
		return;

	add_partial_path(rel, (Path *) create_bitmap_heap_path(root, rel,
														   bitmapqual, rel->lateral_relids, 1.0, parallel_workers));
}

/*
 * 计算应当用来扫描一个
 * 关系的并行工作进程数。根据要扫描的堆的规模和
 * 要扫描的索引的规模来计算并行工作进程数，然后取
 * 两者中较小的。
 *
 * "heap_pages" 是预期要扫描的表页数，或
 * 若预期不扫描任何页则为 -1。
 *
 * "index_pages" 是预期要扫描的索引页数，或
 * 若预期不扫描任何页则为 -1。
 *
 * "max_workers" 是调用方对工作进程数的限制。这通常
 * 来自一个 GUC。
 */
int
compute_parallel_worker(RelOptInfo *rel, double heap_pages, double index_pages,
						int max_workers)
{
	int			parallel_workers = 0;

	/*
	 * 若用户设置了 parallel_workers 关系选项，就用它；否则
	 * 选择默认的工作进程数。
	 */
	if (rel->rel_parallel_workers != -1)
		parallel_workers = rel->rel_parallel_workers;
	else
	{
		/*
		 * 若正在扫描的页数不足以证明
		 * 并行扫描合理，就返回零……除非它是继承
		 * 子项。那种情况下，这里仍然想生成一条并行路径。
		 * 仅对这个关系也许不值得，但
		 * 与它的全部继承兄弟合在一起时，很可能
		 * 划算。
		 */
		if (rel->reloptkind == RELOPT_BASEREL &&
			((heap_pages >= 0 && heap_pages < min_parallel_table_scan_size) ||
			 (index_pages >= 0 && index_pages < min_parallel_index_scan_size)))
			return 0;

		if (heap_pages >= 0)
		{
			int			heap_parallel_threshold;
			int			heap_parallel_workers = 1;

			/*
			 * 根据关系规模的对数选择
			 * 工作进程数。这大概需要更
			 * 精细得多，但目前这里得有个东西。注意
			 * GUC min_parallel_table_scan_size 的上限
			 * 选得能防止这里溢出。
			 */
			heap_parallel_threshold = Max(min_parallel_table_scan_size, 1);
			while (heap_pages >= (BlockNumber) (heap_parallel_threshold * 3))
			{
				heap_parallel_workers++;
				heap_parallel_threshold *= 3;
				if (heap_parallel_threshold > INT_MAX / 3)
					break;		/* 避免溢出 */
			}

			parallel_workers = heap_parallel_workers;
		}

		if (index_pages >= 0)
		{
			int			index_parallel_workers = 1;
			int			index_parallel_threshold;

			/* 与上面 heap_pages 相同的计算 */
			index_parallel_threshold = Max(min_parallel_index_scan_size, 1);
			while (index_pages >= (BlockNumber) (index_parallel_threshold * 3))
			{
				index_parallel_workers++;
				index_parallel_threshold *= 3;
				if (index_parallel_threshold > INT_MAX / 3)
					break;		/* 避免溢出 */
			}

			if (parallel_workers > 0)
				parallel_workers = Min(parallel_workers, index_parallel_workers);
			else
				parallel_workers = index_parallel_workers;
		}
	}

	/* 任何情况下都不要使用超过调用方提供的最大工作进程数 */
	parallel_workers = Min(parallel_workers, max_workers);

	return parallel_workers;
}

/*
 * generate_partitionwise_join_paths
 * 		为给定的分区
 * 		连接关系建立表示按分区连接的路径。
 *
 * 在为全部
 * 子连接加完路径之前，不得调用本函数。否则 add_path 可能删掉这里生成的某条路径
 * 所引用的路径。
 */
void
generate_partitionwise_join_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *live_children = NIL;
	int			cnt_parts;
	int			num_parts;
	RelOptInfo **part_rels;

	/* 这里只处理连接关系。 */
	if (!IS_JOIN_REL(rel))
		return;

	/* 若该关系未分区，就无事可做。 */
	if (!IS_PARTITIONED_REL(rel))
		return;

	/* 该关系应当已设置 consider_partitionwise_join。 */
	Assert(rel->consider_partitionwise_join);

	/* 防止分区层次过深导致栈溢出。 */
	check_stack_depth();

	num_parts = rel->nparts;
	part_rels = rel->part_rels;

	/* 收集非哑的子连接。 */
	for (cnt_parts = 0; cnt_parts < num_parts; cnt_parts++)
	{
		RelOptInfo *child_rel = part_rels[cnt_parts];

		/* 若它已被完全裁剪，那肯定是哑的。 */
		if (child_rel == NULL)
			continue;

		/* 为这个分区的子连接建立按分区连接路径。 */
		generate_partitionwise_join_paths(root, child_rel);

		/* 若未能为这个子项建立任何路径，就必须放弃。 */
		if (child_rel->pathlist == NIL)
		{
			/*
			 * 把父连接关系标成未分区，以便后面的
			 * 函数正确对待它。
			 */
			rel->nparts = 0;
			return;
		}

		/* 否则，找出它的最便宜路径。 */
		set_cheapest(child_rel);

		/* 哑子项不必扫描，因此忽略它们。 */
		if (IS_DUMMY_REL(child_rel))
			continue;

#ifdef OPTIMIZER_DEBUG
		debug_print_rel(root, child_rel);
#endif

		live_children = lappend(live_children, child_rel);
	}

	/* 若所有子连接都是哑的，父连接也是哑的。 */
	if (!live_children)
	{
		mark_dummy_rel(rel);
		return;
	}

	/* 从子连接路径为该关系建立额外路径。 */
	add_paths_to_append_rel(root, rel, live_children);
	list_free(live_children);
}


/*****************************************************************************
 *			DEBUG SUPPORT（调试支持）
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG

static void
print_relids(PlannerInfo *root, Relids relids)
{
	int			x;
	bool		first = true;

	x = -1;
	while ((x = bms_next_member(relids, x)) >= 0)
	{
		if (!first)
			printf(" ");
		if (x < root->simple_rel_array_size &&
			root->simple_rte_array[x])
			printf("%s", root->simple_rte_array[x]->eref->aliasname);
		else
			printf("%d", x);
		first = false;
	}
}

static void
print_restrictclauses(PlannerInfo *root, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->parse->rtable);
		if (lnext(clauses, l))
			printf(", ");
	}
}

static void
print_path(PlannerInfo *root, Path *path, int indent)
{
	const char *ptype;
	bool		join = false;
	Path	   *subpath = NULL;
	int			i;

	switch (nodeTag(path))
	{
		case T_Path:
			switch (path->pathtype)
			{
				case T_SeqScan:
					ptype = "SeqScan";
					break;
				case T_SampleScan:
					ptype = "SampleScan";
					break;
				case T_FunctionScan:
					ptype = "FunctionScan";
					break;
				case T_TableFuncScan:
					ptype = "TableFuncScan";
					break;
				case T_ValuesScan:
					ptype = "ValuesScan";
					break;
				case T_CteScan:
					ptype = "CteScan";
					break;
				case T_NamedTuplestoreScan:
					ptype = "NamedTuplestoreScan";
					break;
				case T_Result:
					ptype = "Result";
					break;
				case T_WorkTableScan:
					ptype = "WorkTableScan";
					break;
				default:
					ptype = "???Path";
					break;
			}
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			break;
		case T_BitmapHeapPath:
			ptype = "BitmapHeapScan";
			break;
		case T_BitmapAndPath:
			ptype = "BitmapAndPath";
			break;
		case T_BitmapOrPath:
			ptype = "BitmapOrPath";
			break;
		case T_TidPath:
			ptype = "TidScan";
			break;
		case T_TidRangePath:
			ptype = "TidRangePath";
			break;
		case T_SubqueryScanPath:
			ptype = "SubqueryScan";
			break;
		case T_ForeignPath:
			ptype = "ForeignScan";
			break;
		case T_CustomPath:
			ptype = "CustomScan";
			break;
		case T_NestPath:
			ptype = "NestLoop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		case T_AppendPath:
			ptype = "Append";
			break;
		case T_MergeAppendPath:
			ptype = "MergeAppend";
			break;
		case T_GroupResultPath:
			ptype = "GroupResult";
			break;
		case T_MaterialPath:
			ptype = "Material";
			subpath = ((MaterialPath *) path)->subpath;
			break;
		case T_MemoizePath:
			ptype = "Memoize";
			subpath = ((MemoizePath *) path)->subpath;
			break;
		case T_UniquePath:
			ptype = "Unique";
			subpath = ((UniquePath *) path)->subpath;
			break;
		case T_GatherPath:
			ptype = "Gather";
			subpath = ((GatherPath *) path)->subpath;
			break;
		case T_GatherMergePath:
			ptype = "GatherMerge";
			subpath = ((GatherMergePath *) path)->subpath;
			break;
		case T_ProjectionPath:
			ptype = "Projection";
			subpath = ((ProjectionPath *) path)->subpath;
			break;
		case T_ProjectSetPath:
			ptype = "ProjectSet";
			subpath = ((ProjectSetPath *) path)->subpath;
			break;
		case T_SortPath:
			ptype = "Sort";
			subpath = ((SortPath *) path)->subpath;
			break;
		case T_IncrementalSortPath:
			ptype = "IncrementalSort";
			subpath = ((SortPath *) path)->subpath;
			break;
		case T_GroupPath:
			ptype = "Group";
			subpath = ((GroupPath *) path)->subpath;
			break;
		case T_UpperUniquePath:
			ptype = "UpperUnique";
			subpath = ((UpperUniquePath *) path)->subpath;
			break;
		case T_AggPath:
			ptype = "Agg";
			subpath = ((AggPath *) path)->subpath;
			break;
		case T_GroupingSetsPath:
			ptype = "GroupingSets";
			subpath = ((GroupingSetsPath *) path)->subpath;
			break;
		case T_MinMaxAggPath:
			ptype = "MinMaxAgg";
			break;
		case T_WindowAggPath:
			ptype = "WindowAgg";
			subpath = ((WindowAggPath *) path)->subpath;
			break;
		case T_SetOpPath:
			ptype = "SetOp";
			subpath = ((SetOpPath *) path)->subpath;
			break;
		case T_RecursiveUnionPath:
			ptype = "RecursiveUnion";
			break;
		case T_LockRowsPath:
			ptype = "LockRows";
			subpath = ((LockRowsPath *) path)->subpath;
			break;
		case T_ModifyTablePath:
			ptype = "ModifyTable";
			break;
		case T_LimitPath:
			ptype = "Limit";
			subpath = ((LimitPath *) path)->subpath;
			break;
		default:
			ptype = "???Path";
			break;
	}

	for (i = 0; i < indent; i++)
		printf("\t");
	printf("%s", ptype);

	if (path->parent)
	{
		printf("(");
		print_relids(root, path->parent->relids);
		printf(")");
	}
	if (path->param_info)
	{
		printf(" required_outer (");
		print_relids(root, path->param_info->ppi_req_outer);
		printf(")");
	}
	printf(" rows=%.0f cost=%.2f..%.2f\n",
		   path->rows, path->startup_cost, path->total_cost);

	if (path->pathkeys)
	{
		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  pathkeys: ");
		print_pathkeys(path->pathkeys, root->parse->rtable);
	}

	if (join)
	{
		JoinPath   *jp = (JoinPath *) path;

		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  clauses: ");
		print_restrictclauses(root, jp->joinrestrictinfo);
		printf("\n");

		if (IsA(path, MergePath))
		{
			MergePath  *mp = (MergePath *) path;

			for (i = 0; i < indent; i++)
				printf("\t");
			printf("  sortouter=%d sortinner=%d materializeinner=%d\n",
				   ((mp->outersortkeys) ? 1 : 0),
				   ((mp->innersortkeys) ? 1 : 0),
				   ((mp->materialize_inner) ? 1 : 0));
		}

		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}

	if (subpath)
		print_path(root, subpath, indent + 1);
}

void
debug_print_rel(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	printf("RELOPTINFO (");
	print_relids(root, rel->relids);
	printf("): rows=%.0f width=%d\n", rel->rows, rel->reltarget->width);

	if (rel->baserestrictinfo)
	{
		printf("\tbaserestrictinfo: ");
		print_restrictclauses(root, rel->baserestrictinfo);
		printf("\n");
	}

	if (rel->joininfo)
	{
		printf("\tjoininfo: ");
		print_restrictclauses(root, rel->joininfo);
		printf("\n");
	}

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	if (rel->cheapest_parameterized_paths)
	{
		printf("\n\tcheapest parameterized paths:\n");
		foreach(l, rel->cheapest_parameterized_paths)
			print_path(root, lfirst(l), 1);
	}
	if (rel->cheapest_startup_path)
	{
		printf("\n\tcheapest startup path:\n");
		print_path(root, rel->cheapest_startup_path, 1);
	}
	if (rel->cheapest_total_path)
	{
		printf("\n\tcheapest total path:\n");
		print_path(root, rel->cheapest_total_path, 1);
	}
	printf("\n");
	fflush(stdout);
}

#endif							/* OPTIMIZER_DEBUG（优化器调试） */
