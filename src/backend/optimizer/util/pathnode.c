/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  操作路径列表并创建路径节点的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/pathnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"

typedef enum
{
	COSTS_EQUAL,				/* 路径代价模糊相等 */
	COSTS_BETTER1,				/* 第一条路径比第二条便宜 */
	COSTS_BETTER2,				/* 第二条路径比第一条便宜 */
	COSTS_DIFFERENT				/* 两条路径在代价上谁也不支配谁 */
} PathCostComparison;

/*
 * STD_FUZZ_FACTOR 是 compare_path_costs_fuzzily 的常规模糊因子。
 * XXX 值得让用户调节它吗？它在规划器运行时间
 * 与路径代价比较的精度之间做取舍。
 */
#define STD_FUZZ_FACTOR 1.01

static List *translate_sub_tlist(List *tlist, int relid);
static int	append_total_cost_compare(const ListCell *a, const ListCell *b);
static int	append_startup_cost_compare(const ListCell *a, const ListCell *b);
static List *reparameterize_pathlist_by_child(PlannerInfo *root,
											  List *pathlist,
											  RelOptInfo *child_rel);
static bool contain_references_to(PlannerInfo *root, Node *clause,
								  Relids relids);
static bool ris_contain_references_to(PlannerInfo *root, List *rinfos,
									  Relids relids);


/*****************************************************************************
 *		杂项路径 UTILITIES
 *****************************************************************************/

/*
 * compare_path_costs
 *	  按 path1 比 path2 更便宜、代价相同
 *	  或更贵，对指定准则返回 -1、0 或 +1。
 */
int
compare_path_costs(Path *path1, Path *path2, CostSelector criterion)
{
	if (criterion == STARTUP_COST)
	{
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;

		/*
		 * 若路径的启动代价相同（这毫不罕见），就按
		 * 总代价给它们排序。
		 */
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;
	}
	else
	{
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;

		/*
		 * 若路径的总代价相同，就按启动代价排序。
		 */
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;
	}
	return 0;
}

/*
 * compare_fractional_path_costs
 *	  按 path1 比 path2 更便宜、代价相同
 *	  或更贵，就取出指定比例的
 *	  全部元组而言返回 -1、0 或 +1。
 *
 * 若 fraction 小于等于 0 或大于 1，就把它当成 1，也就是选择
 * total_cost 更便宜的那条路径。
 */
int
compare_fractional_path_costs(Path *path1, Path *path2,
							  double fraction)
{
	Cost		cost1,
				cost2;

	if (fraction <= 0.0 || fraction >= 1.0)
		return compare_path_costs(path1, path2, TOTAL_COST);
	cost1 = path1->startup_cost +
		fraction * (path1->total_cost - path1->startup_cost);
	cost2 = path2->startup_cost +
		fraction * (path2->total_cost - path2->startup_cost);
	if (cost1 < cost2)
		return -1;
	if (cost1 > cost2)
		return +1;
	return 0;
}

/*
 * compare_path_costs_fuzzily
 *	  比较两条路径的代价，看是否可以说
 *	  其中一条支配另一条。
 *
 * 我们使用模糊比较，以便 add_path() 不必同时保留
 * 一对代价差别微不足道的路径。
 *
 * fuzz_factor 参数必须是 1.0 加上 delta，其中 delta 是
 * 较小代价中被视为显著差异的那个分数。
 * 例如 fuzz_factor = 1.01 时，模糊限度
 * 是较小代价的 1%。
 *
 * 若启动代价和总代价都模糊相同，就说两条路径
 * 代价相等。若 path1 的启动代价模糊更优且总代价
 * 模糊不更差，或总代价模糊更优且启动代价模糊
 * 不更差，就说 path1 优于 path2。反过来则 path2
 * 优于 path1。最后，若一条路径在启动代价上模糊
 * 更优、在总代价上模糊更差，我们就只说它们的代价
 * 不同，因为在整个性能范围里谁也不
 * 支配谁。
 *
 * 本函数还强制一条策略：若 parent->consider_startup 和
 * parent->consider_param_startup 里相关的那个为假，路径就
 * 无法仅凭启动代价好而在比较中留下来，因此当这对
 * 总代价的落败者成立时，我们从不返回 COSTS_DIFFERENT。
 * （但若总代价模糊相等，我们仍会比较启动代价，
 * 希望能淘汰其中一条路径。）
 */
static PathCostComparison
compare_path_costs_fuzzily(Path *path1, Path *path2, double fuzz_factor)
{
#define CONSIDER_PATH_STARTUP_COST(p)  \
	((p)->param_info == NULL ? (p)->parent->consider_startup : (p)->parent->consider_param_startup)

	/*
	 * 先查总代价，因为它更可能不同；许多
	 * 路径的启动代价为零。
	 */
	if (path1->total_cost > path2->total_cost * fuzz_factor)
	{
		/* path1 的总代价模糊更差 */
		if (CONSIDER_PATH_STARTUP_COST(path1) &&
			path2->startup_cost > path1->startup_cost * fuzz_factor)
		{
			/* ……但 path2 的启动代价模糊更差，因此 DIFFERENT */
			return COSTS_DIFFERENT;
		}
		/* 否则 path2 支配 */
		return COSTS_BETTER2;
	}
	if (path2->total_cost > path1->total_cost * fuzz_factor)
	{
		/* path2 的总代价模糊更差 */
		if (CONSIDER_PATH_STARTUP_COST(path2) &&
			path1->startup_cost > path2->startup_cost * fuzz_factor)
		{
			/* ……但 path1 的启动代价模糊更差，因此 DIFFERENT */
			return COSTS_DIFFERENT;
		}
		/* 否则 path1 支配 */
		return COSTS_BETTER1;
	}
	/* 总代价模糊相同…… */
	if (path1->startup_cost > path2->startup_cost * fuzz_factor)
	{
		/* ……但 path1 的启动代价模糊更差，因此 path2 胜出 */
		return COSTS_BETTER2;
	}
	if (path2->startup_cost > path1->startup_cost * fuzz_factor)
	{
		/* ……但 path2 的启动代价模糊更差，因此 path1 胜出 */
		return COSTS_BETTER1;
	}
	/* 两项代价都模糊相同 */
	return COSTS_EQUAL;

#undef CONSIDER_PATH_STARTUP_COST
}

/*
 * set_cheapest
 *	  从关系的各条路径里找出最小代价路径，
 *	  并存进该关系的最便宜路径成员。
 *
 * cheapest_total_path 通常是总代价最便宜的未参数化
 * 路径；若没有未参数化路径，就把它指定为最好的
 * （最便宜且参数化最少的）参数化路径。不过，
 * 只有未参数化路径才会被考虑作 cheapest_startup_path，
 * 因此没有未参数化路径时它将为 NULL。
 *
 * cheapest_parameterized_paths 列表收集这个关系在
 * add_path() 锦标赛中存活下来的全部参数化路径。
 * （因为 add_path 忽略参数化路径的路径键，它们将是
 * 对其参数化而言代价最好或行数最好的路径。有些
 * 情况下同一种参数化也可能同时保留并行安全与
 * 非并行安全两条路径，但这应当比较少见，因为最典型地，
 * 同一关系的全部路径要么都并行安全，
 * 要么都不并行安全。）
 *
 * cheapest_parameterized_paths 也总是包含总代价最便宜的
 * 未参数化路径（如果有的话）；使用该列表的人会觉得
 * 把它也包括进去更方便。
 *
 * 通常只在我们为该关系节点构造完路径列表
 * 之后才调用本函数。
 */
void
set_cheapest(RelOptInfo *parent_rel)
{
	Path	   *cheapest_startup_path;
	Path	   *cheapest_total_path;
	Path	   *best_param_path;
	List	   *parameterized_paths;
	ListCell   *p;

	Assert(IsA(parent_rel, RelOptInfo));

	if (parent_rel->pathlist == NIL)
		elog(ERROR, "could not devise a query plan for the given query");

	cheapest_startup_path = cheapest_total_path = best_param_path = NULL;
	parameterized_paths = NIL;

	foreach(p, parent_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(p);
		int			cmp;

		if (path->param_info)
		{
			/* 参数化路径，因此把它加进 parameterized_paths */
			parameterized_paths = lappend(parameterized_paths, path);

			/*
			 * 若已经有未参数化的最便宜总代价路径，就不再关心
			 * 寻找最好的参数化路径，继续往下。
			 */
			if (cheapest_total_path)
				continue;

			/*
			 * 否则跟踪最好的参数化路径，也就是在最小
			 * 参数化的那些路径里总代价
			 * 最低的一条。
			 */
			if (best_param_path == NULL)
				best_param_path = path;
			else
			{
				switch (bms_subset_compare(PATH_REQ_OUTER(path),
										   PATH_REQ_OUTER(best_param_path)))
				{
					case BMS_EQUAL:
						/* 保留更便宜的那条 */
						if (compare_path_costs(path, best_param_path,
											   TOTAL_COST) < 0)
							best_param_path = path;
						break;
					case BMS_SUBSET1:
						/* 新路径的参数化更少 */
						best_param_path = path;
						break;
					case BMS_SUBSET2:
						/* 旧路径的参数化更少，保留它 */
						break;
					case BMS_DIFFERENT:

						/*
						 * 这意味着两条路径都不是该关系可能的最小
						 * 参数化。我们先留着旧
						 * 路径，直到有更好的出现。
						 */
						break;
				}
			}
		}
		else
		{
			/* 未参数化路径，因此考虑放进最便宜的槽位 */
			if (cheapest_total_path == NULL)
			{
				cheapest_startup_path = cheapest_total_path = path;
				continue;
			}

			/*
			 * 若发现两条路径代价完全相同，尽量保留
			 * 排序更好的那条。两条路径的排序次序可能互不相关，
			 * 那时只能猜测留哪条更好，但若一条明确更优，
			 * 就一定
			 * 应当留下那条。
			 */
			cmp = compare_path_costs(cheapest_startup_path, path, STARTUP_COST);
			if (cmp > 0 ||
				(cmp == 0 &&
				 compare_pathkeys(cheapest_startup_path->pathkeys,
								  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_startup_path = path;

			cmp = compare_path_costs(cheapest_total_path, path, TOTAL_COST);
			if (cmp > 0 ||
				(cmp == 0 &&
				 compare_pathkeys(cheapest_total_path->pathkeys,
								  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_total_path = path;
		}
	}

	/* 若有最便宜的未参数化路径，就加进 parameterized_paths */
	if (cheapest_total_path)
		parameterized_paths = lcons(cheapest_total_path, parameterized_paths);

	/*
	 * 若没有未参数化路径，就把最好的参数化路径用作
	 * cheapest_total_path（但不用作 cheapest_startup_path）。
	 */
	if (cheapest_total_path == NULL)
		cheapest_total_path = best_param_path;
	Assert(cheapest_total_path != NULL);

	parent_rel->cheapest_startup_path = cheapest_startup_path;
	parent_rel->cheapest_total_path = cheapest_total_path;
	parent_rel->cheapest_unique_path = NULL;	/* 仅在需要时才计算 */
	parent_rel->cheapest_parameterized_paths = parameterized_paths;
}

/*
 * add_path
 *	  考虑指定父关系的一条候选实现路径，
 *	  若值得考虑就把它加进该关系的 pathlist。
 *	  一条路径值得保留，如果它比任何具有相同或超集
 *	  参数化关系的现有路径有更好的排序（更好的路径键）、
 *	  更低的代价（任一维度），或产生更少的行。
 *	  我们也认为并行安全的路径比其他路径更值得保留。
 *
 *	  我们也从该关系的 pathlist 里去掉被 new_path 支配的
 *	  旧路径：也就是 new_path 更便宜、排序至少一样好、
 *	  产生的行不多于旧路径、不需要旧路径所不需要的外部关系，
 *	  并且并行安全性不更差。
 *
 *	  多数情况下，参数化是超集的路径会产生
 *	  更少的行（因为它有更多连接子句要应用），因此这两项
 *	  优劣指标方向相反；这意味着一种参数化的路径
 *	  很少能支配另一种参数化的路径。但这种情况
 *	  确实会出现，所以我们仍然做全套检查。
 *
 *	  这个函数及其兄弟 add_path_precheck 里嵌了两项策略决定。
 *	  首先，我们把所有参数化路径的路径键都当成 NIL，
 *	  使它们没法靠排序次序赢得比较。这是为了减少
 *	  所保留的参数化路径数量；讨论见
 *	  src/backend/optimizer/README。
 *
 *	  其次，只有未参数化路径的 parent_rel->consider_startup
 *	  为真，或参数化路径的 parent_rel->consider_param_startup
 *	  为真时，我们才认为较低的启动代价值得关心。
 *	  同样，这可以更早丢掉无用路径。
 *
 *	  pathlist 按 total_cost 排序，更便宜的路径
 *	  排在前面。在本例程里，这只是速度上的权宜做法：
 *	  这样做更可能在少数几次比较后就拒绝一条较差的
 *	  路径，而不是比较很多次。
 *	  不过 add_path_precheck 依赖这个顺序，以便在可能时
 *	  提早退出。
 *
 *	  注意：被丢掉的 Path 对象会立刻 pfree，以减少规划器
 *	  的内存消耗。我们不敢释放 Path 的子结构，
 *	  因为它的很多部分可能与其他 Path 或查询树本身共享；
 *	  但只回收被丢掉的 Path 节点，在大型连接树里
 *	  是很有用的节省。pathlist 的 List 节点也可以回收。
 *
 *	  如 optimizer/README 所述，删除先前已接受的 Path 是
 *	  安全的，因为我们知道这个关系的 Path 还不可能被
 *	  任何其他关系引用，例如更高层的连接。不过有些情况下
 *	  一个 Path 可能被它自己关系的另一个 Path 引用；
 *	  即使它被新的 Path 支配，我们也不得删除这样的 Path。
 *	  目前这只发生在 IndexPath 对象上，它们既可能作为
 *	  BitmapHeapPaths 的子路径被引用，本身也是路径。
 *	  因此拒绝 IndexPaths 时我们不 pfree。
 *
 * parent_rel 是该路径对应的关系项。
 * new_path 是 parent_rel 的一条候选路径。
 *
 * 无返回值，但会修改 parent_rel->pathlist。
 */
void
add_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;	/* 除非我们发现一条更优的旧路径 */
	int			insert_at = 0;	/* 新项插到哪里 */
	List	   *new_path_pathkeys;
	ListCell   *p1;

	/*
	 * 这里适合检查查询是否取消：规划器里没有哪一段
	 * 会很久不调用 add_path()。
	 */
	CHECK_FOR_INTERRUPTS();

	/* 按上面的注释，假装参数化路径没有路径键 */
	new_path_pathkeys = new_path->param_info ? NIL : new_path->pathkeys;

	/*
	 * 循环，用提议的新路径对照旧路径。注意可能有
	 * 不止一条旧路径因为被 new_path 支配而
	 * 被扔掉。
	 */
	foreach(p1, parent_rel->pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* 除非新路径证明更优 */
		PathCostComparison costcmp;
		PathKeysComparison keyscmp;
		BMS_Comparison outercmp;

		/*
		 * 用标准模糊限度做模糊代价比较。
		 */
		costcmp = compare_path_costs_fuzzily(new_path, old_path,
											 STD_FUZZ_FACTOR);

		/*
		 * 若两条路径在启动代价和总代价上比较结果不同，
		 * 就两条都留，并且可以跳过路径键和
		 * required_outer 关系的比较。若比较结果相同，再继续
		 * 其他比较。行数最后检查。（按这个顺序做测试，
		 * 是因为代价比较最可能得出不同，路径键比较
		 * 其次。如上所述，行数很少造成差别，所以尽管
		 * 比较它很便宜，也没有多大必要
		 * 更早去
		 * 检查。）
		 */
		if (costcmp != COSTS_DIFFERENT)
		{
			/* 同样检查是否有一方在路径键上支配 */
			List	   *old_path_pathkeys;

			old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
			keyscmp = compare_pathkeys(new_path_pathkeys,
									   old_path_pathkeys);
			if (keyscmp != PATHKEYS_DIFFERENT)
			{
				switch (costcmp)
				{
					case COSTS_EQUAL:
						outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
													  PATH_REQ_OUTER(old_path));
						if (keyscmp == PATHKEYS_BETTER1)
						{
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;	/* 新路径支配旧路径 */
						}
						else if (keyscmp == PATHKEYS_BETTER2)
						{
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false; /* 旧路径支配新路径 */
						}
						else	/* keyscmp == PATHKEYS_EQUAL（路径键相等） */
						{
							if (outercmp == BMS_EQUAL)
							{
								/*
								 * 路径键和外部关系相同，并且代价
								 * 模糊相同，因此只留一条；要决定
								 * 留哪条，先查并行安全性，再查
								 * 行数，然后用非常小的模糊限度
								 * 做一次模糊代价比较。（从前做的是
								 * 精确代价比较，但那会因代价估计里的舍入
								 * 造成讨厌的、随平台而变的
								 * 计划差异。）若仍然打平，就任意地
								 * 只保留旧路径。注意，即使模糊度更小的
								 * 比较判定启动代价与总代价
								 * 比较结果不同，我们仍会
								 * 只保留
								 * 旧路径。
								 */
								if (new_path->parallel_safe >
									old_path->parallel_safe)
									remove_old = true;	/* 新路径支配旧路径 */
								else if (new_path->parallel_safe <
										 old_path->parallel_safe)
									accept_new = false; /* 旧路径支配新路径 */
								else if (new_path->rows < old_path->rows)
									remove_old = true;	/* 新路径支配旧路径 */
								else if (new_path->rows > old_path->rows)
									accept_new = false; /* 旧路径支配新路径 */
								else if (compare_path_costs_fuzzily(new_path,
																	old_path,
																	1.0000000001) == COSTS_BETTER1)
									remove_old = true;	/* 新路径支配旧路径 */
								else
									accept_new = false; /* 旧路径相等，或
														 * 支配新路径 */
							}
							else if (outercmp == BMS_SUBSET1 &&
									 new_path->rows <= old_path->rows &&
									 new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;	/* 新路径支配旧路径 */
							else if (outercmp == BMS_SUBSET2 &&
									 new_path->rows >= old_path->rows &&
									 new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false; /* 旧路径支配新路径 */
							/* 否则参数化不同，两条都留 */
						}
						break;
					case COSTS_BETTER1:
						if (keyscmp != PATHKEYS_BETTER2)
						{
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
														  PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;	/* 新路径支配旧路径 */
						}
						break;
					case COSTS_BETTER2:
						if (keyscmp != PATHKEYS_BETTER1)
						{
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
														  PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false; /* 旧路径支配新路径 */
						}
						break;
					case COSTS_DIFFERENT:

						/*
						 * 到不了这里，保留这个分支以消除编译器
						 * 告警
						 */
						break;
				}
			}
		}

		/*
		 * 若被新路径支配，就从 pathlist 去掉当前元素。
		 */
		if (remove_old)
		{
			parent_rel->pathlist = foreach_delete_current(parent_rel->pathlist,
														  p1);

			/*
			 * 若可能，删除被删单元所指向的数据
			 */
			if (!IsA(old_path, IndexPath))
				pfree(old_path);
		}
		else
		{
			/* 若新路径代价 >= 旧路径，就排在这条旧路径之后 */
			if (new_path->total_cost >= old_path->total_cost)
				insert_at = foreach_current_index(p1) + 1;
		}

		/*
		 * 若发现一条支配 new_path 的旧路径，就可以停止
		 * 扫描 pathlist；我们不会加入 new_path，并且假定
		 * new_path 无法支配 pathlist 里的任何其他元素。
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/* 接受新路径：把它插到 pathlist 的适当位置 */
		parent_rel->pathlist =
			list_insert_nth(parent_rel->pathlist, insert_at, new_path);
	}
	else
	{
		/* 拒绝并回收新路径 */
		if (!IsA(new_path, IndexPath))
			pfree(new_path);
	}
}

/*
 * add_path_precheck
 *	  检查提议的新路径是否有可能被接受。
 *	  我们假定已经准确知道该路径的路径键和参数化，
 *	  并且有其代价的下界。
 *
 * 注意我们不知道该路径的行数，因为在预检查之前
 * 去估计太贵了。这里假定参数化为超集的路径
 * 会产生更少的行；若这一点成立，
 * 则参数化不同的路径无法互相支配，
 * 因此可以简单地忽略另一种参数化的现有路径。
 * （在这条经验法则失效的少见情形里，add_path 会
 * 丢掉较差的那条路径。）
 *
 * 调用到这里时，我们其实还没建出 Path 结构，
 * 所以所需信息必须零碎传入。
 */
bool
add_path_precheck(RelOptInfo *parent_rel,
				  Cost startup_cost, Cost total_cost,
				  List *pathkeys, Relids required_outer)
{
	List	   *new_path_pathkeys;
	bool		consider_startup;
	ListCell   *p1;

	/* 按 add_path 的策略，假装参数化路径没有路径键 */
	new_path_pathkeys = required_outer ? NIL : pathkeys;

	/* 决定新路径的启动代价是否值得关心 */
	consider_startup = required_outer ? parent_rel->consider_param_startup : parent_rel->consider_startup;

	foreach(p1, parent_rel->pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		/*
		 * 我们在找一条参数化相同（并且
		 * 按假定行数也相同）的 old_path，它在路径键以及
		 * 两项代价指标上都支配新路径。若找到一条，就可以
		 * 拒绝新路径。
		 *
		 * 这里的代价比较应当与 compare_path_costs_fuzzily 一致。
		 */
		if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
		{
			/* 仅当 consider_startup 时，新路径才能靠启动代价取胜 */
			if (startup_cost > old_path->startup_cost * STD_FUZZ_FACTOR ||
				!consider_startup)
			{
				/* 新路径在代价上落败，因此检查路径键…… */
				List	   *old_path_pathkeys;

				old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
				keyscmp = compare_pathkeys(new_path_pathkeys,
										   old_path_pathkeys);
				if (keyscmp == PATHKEYS_EQUAL ||
					keyscmp == PATHKEYS_BETTER2)
				{
					/* 新路径在路径键上并不取胜…… */
					if (bms_equal(required_outer, PATH_REQ_OUTER(old_path)))
					{
						/* 找到一条支配新路径的旧路径 */
						return false;
					}
				}
			}
		}
		else
		{
			/*
			 * 因为 pathlist 按 total_cost 排序，一旦遇到总代价
			 * 大于新路径的路径，就可以停止
			 * 查找。
			 */
			break;
		}
	}

	return true;
}

/*
 * add_partial_path
 *	  与 add_path 一样，这里要考虑一条路径是否值得
 *	  留下来，但考虑的因素略有不同。
 *	  部分路径可以在任意多个工作进程中
 *	  并行执行，使每个工作进程产生该路径
 *	  整体结果的一个子集。
 *
 *	  与 add_path 中一样，partial_pathlist 按总代价排序，
 *	  最便宜的路径在前。多处依赖这一点，
 *	  不搜索就取最前一项当作最便宜路径。
 *
 *	  出于几个理由，我们不生成参数化的部分路径。最
 *	  重要的是它们执行不安全：没有任何机制
 *	  保证计划里参数化部分中的并行扫描，每个工作进程
 *	  在同一时刻使用同一个值。
 *	  幸好，这看起来反正也不值得，因为让
 *	  每个工作进程扫描整个外侧关系、以及内侧
 *	  关系的一个子集，通常会是很糟的计划。内侧
 *	  （参数化的一侧）反正也会很小。可能有罕见情形
 *	  会大赢，例如连接顺序约束把一个 1 行关系放到
 *	  最顶层连接的外侧，内侧是一条参数化计划。
 *	  但在有人做出能应付这些情形的执行器基础设施之前，
 *	  我们只好满足于不去处理它们。
 *
 *	  因为这里不考虑参数化路径，也就不必把行数当作
 *	  质量指标：每条路径产生的行数相同。启动代价
 *	  也不必考虑：并行只用于会一直跑完的计划。
 *	  因此这个例程比 add_path 简单得多：它只需要
 *	  考虑路径键
 *	  和总代价。
 *
 *	  与 add_path 一样，被另一条部分路径支配的路径会被 pfree；
 *	  因此此时还不应有别的引用指向这些路径。因此，在我们
 *	  为一个关系建完全部部分路径之前，不得为它创建
 *	  GatherPaths。与 add_path 不同，我们不为 IndexPaths 破例，
 *	  因为部分索引路径不会被部分的
 *	  BitmapHeapPaths 引用。
 */
void
add_partial_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;	/* 除非我们发现一条更优的旧路径 */
	int			insert_at = 0;	/* 新项插到哪里 */
	ListCell   *p1;

	/* 检查查询是否取消。 */
	CHECK_FOR_INTERRUPTS();

	/* 要加入的路径必须并行安全。 */
	Assert(new_path->parallel_safe);

	/* 该关系也应当适合并行。 */
	Assert(parent_rel->consider_parallel);

	/*
	 * 与 add_path 一样，丢掉被新路径支配的任何路径，
	 * 但若某条现有路径支配新路径，就丢掉新路径。
	 */
	foreach(p1, parent_rel->partial_pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* 除非新路径证明更优 */
		PathKeysComparison keyscmp;

		/* 比较路径键。 */
		keyscmp = compare_pathkeys(new_path->pathkeys, old_path->pathkeys);

		/* 除非路径键不相容，否则两条路径只留一条。 */
		if (keyscmp != PATHKEYS_DIFFERENT)
		{
			if (new_path->total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
			{
				/* 新路径更贵；仅当路径键更好时才保留。 */
				if (keyscmp != PATHKEYS_BETTER1)
					accept_new = false;
			}
			else if (old_path->total_cost > new_path->total_cost
					 * STD_FUZZ_FACTOR)
			{
				/* 旧路径更贵；仅当路径键更好时才保留。 */
				if (keyscmp != PATHKEYS_BETTER2)
					remove_old = true;
			}
			else if (keyscmp == PATHKEYS_BETTER1)
			{
				/* 代价大致相同，新路径的路径键更好。 */
				remove_old = true;
			}
			else if (keyscmp == PATHKEYS_BETTER2)
			{
				/* 代价大致相同，旧路径的路径键更好。 */
				accept_new = false;
			}
			else if (old_path->total_cost > new_path->total_cost * 1.0000000001)
			{
				/* 路径键相同，并且旧路径更贵。 */
				remove_old = true;
			}
			else
			{
				/*
				 * 路径键相同，并且新路径并没有实质性地
				 * 更便宜。
				 */
				accept_new = false;
			}
		}

		/*
		 * 若被新路径支配，就从 partial_pathlist 去掉当前元素。
		 */
		if (remove_old)
		{
			parent_rel->partial_pathlist =
				foreach_delete_current(parent_rel->partial_pathlist, p1);
			pfree(old_path);
		}
		else
		{
			/* 若新路径代价 >= 旧路径，就排在这条旧路径之后 */
			if (new_path->total_cost >= old_path->total_cost)
				insert_at = foreach_current_index(p1) + 1;
		}

		/*
		 * 若发现一条支配 new_path 的旧路径，就可以停止
		 * 扫描 partial_pathlist；我们不会加入 new_path，并且
		 * 假定 new_path 无法支配后面的任何路径。
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/* 接受新路径：把它插到适当位置 */
		parent_rel->partial_pathlist =
			list_insert_nth(parent_rel->partial_pathlist, insert_at, new_path);
	}
	else
	{
		/* 拒绝并回收新路径 */
		pfree(new_path);
	}
}

/*
 * add_partial_path_precheck
 *	  检查提议的新部分路径是否有可能被接受。
 *
 * 与 add_path_precheck 不同，可以忽略启动代价和参数化，
 * 因为它们对部分路径无关紧要（见 add_partial_path）。但
 * 我们确实想确保：若已经有一条支配它的完整路径，
 * 就不要再加部分路径，因为那时提议的路径
 * 肯定是输家。
 */
bool
add_partial_path_precheck(RelOptInfo *parent_rel, Cost total_cost,
						  List *pathkeys)
{
	ListCell   *p1;

	/*
	 * 这里的目标有两层。首先，要弄清这条路径
	 * 是否明显劣于某条现有部分路径。若是，就想
	 * 立刻拒绝它。其次，要弄清这条路径
	 * 是否明显优于某条现有部分路径，至少在最终
	 * 代价计算完成之前是这样。若是，就一定要考虑它。
	 *
	 * 与 add_path() 不同，这里总是比较路径键。这是因为我们
	 * 预期 partial_pathlist 很短，在这一阶段得到确定
	 * 答案就可以不必调用 add_path_precheck。
	 */
	foreach(p1, parent_rel->partial_pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		keyscmp = compare_pathkeys(pathkeys, old_path->pathkeys);
		if (keyscmp != PATHKEYS_DIFFERENT)
		{
			if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER1)
				return false;
			if (old_path->total_cost > total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER2)
				return true;
		}
	}

	/*
	 * 这条路径既不明显劣于现有部分路径，也没有
	 * 好到可能替换掉一条。把它与
	 * 非并行计划比较。若在计入 Gather 节点代价之前就已经落败，
	 * 就一定应当拒绝它。
	 *
	 * 注意我们把 total_cost 传给 add_path_precheck 两次。这是
	 * 因为考虑部分路径的启动代价从来没有好处；由此得到的计划若并行运行，
	 * 会一直跑到
	 * 完成。
	 */
	if (!add_path_precheck(parent_rel, total_cost, total_cost, pathkeys,
						   NULL))
		return false;

	return true;
}


/*****************************************************************************
 *		路径节点 CREATION ROUTINES
 *****************************************************************************/

/*
 * create_seqscan_path
 *	  创建对应于顺序扫描的路径，返回
 *	  该路径节点。
 */
Path *
create_seqscan_path(PlannerInfo *root, RelOptInfo *rel,
					Relids required_outer, int parallel_workers)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = (parallel_workers > 0);
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = parallel_workers;
	pathnode->pathkeys = NIL;	/* 顺序扫描的结果无序 */

	cost_seqscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_samplescan_path
 *	  为采样表扫描创建一个路径节点。
 */
Path *
create_samplescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SampleScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 采样扫描的结果无序 */

	cost_samplescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_index_path
 *	  创建索引扫描的路径节点。
 *
 * index 是一个可用索引。
 * indexclauses 是 IndexClause 节点列表，表示
 *			要在扫描中作为限定条件强制的子句。
 * indexorderbys 是裸表达式列表（没有 RestrictInfos），
 *			用作扫描中的索引排序操作符。
 * indexorderbycols 是索引列号的整数列表（从零开始），
 *			这些排序操作符可以配合使用。
 * pathkeys 描述该路径的排序。
 * indexscandir 为 ForwardScanDirection 或 BackwardScanDirection。
 * indexonly 为真表示想要仅索引扫描。
 * required_outer 是参数化路径的外部 relid 集合。
 * loop_count 是要计入缓存行为估计的
 *		索引扫描重复次数。
 * partial_path 为真表示正在构造并行索引扫描路径。
 *
 * 返回新的路径节点。
 */
IndexPath *
create_index_path(PlannerInfo *root,
				  IndexOptInfo *index,
				  List *indexclauses,
				  List *indexorderbys,
				  List *indexorderbycols,
				  List *pathkeys,
				  ScanDirection indexscandir,
				  bool indexonly,
				  Relids required_outer,
				  double loop_count,
				  bool partial_path)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	RelOptInfo *rel = index->rel;

	pathnode->path.pathtype = indexonly ? T_IndexOnlyScan : T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = pathkeys;

	pathnode->indexinfo = index;
	pathnode->indexclauses = indexclauses;
	pathnode->indexorderbys = indexorderbys;
	pathnode->indexorderbycols = indexorderbycols;
	pathnode->indexscandir = indexscandir;

	cost_index(pathnode, root, loop_count, partial_path);

	return pathnode;
}

/*
 * create_bitmap_heap_path
 *	  创建位图扫描的路径节点。
 *
 * bitmapqual 是 IndexPath、BitmapAndPath 和 BitmapOrPath 节点组成的树。
 * required_outer 是参数化路径的外部 relid 集合。
 * loop_count 是要计入缓存行为估计的
 *		索引扫描重复次数。
 *
 * loop_count 应当与创建各个组成
 * IndexPaths 时所用的值一致。
 */
BitmapHeapPath *
create_bitmap_heap_path(PlannerInfo *root,
						RelOptInfo *rel,
						Path *bitmapqual,
						Relids required_outer,
						double loop_count,
						int parallel_degree)
{
	BitmapHeapPath *pathnode = makeNode(BitmapHeapPath);

	pathnode->path.pathtype = T_BitmapHeapScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = (parallel_degree > 0);
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = parallel_degree;
	pathnode->path.pathkeys = NIL;	/* 总是无序 */

	pathnode->bitmapqual = bitmapqual;

	cost_bitmap_heap_scan(&pathnode->path, root, rel,
						  pathnode->path.param_info,
						  bitmapqual, loop_count);

	return pathnode;
}

/*
 * create_bitmap_and_path
 *	  创建一个表示 BitmapAnd 的路径节点。
 */
BitmapAndPath *
create_bitmap_and_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   List *bitmapquals)
{
	BitmapAndPath *pathnode = makeNode(BitmapAndPath);
	Relids		required_outer = NULL;
	ListCell   *lc;

	pathnode->path.pathtype = T_BitmapAnd;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * 把所需的外部关系标成各子路径所依赖关系的并。
	 * （也可以坚持让调用方传入，但在这里计算
	 * 更方便也更可靠。）
	 */
	foreach(lc, bitmapquals)
	{
		Path	   *bitmapqual = (Path *) lfirst(lc);

		required_outer = bms_add_members(required_outer,
										 PATH_REQ_OUTER(bitmapqual));
	}
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);

	/*
	 * 目前，BitmapHeapPath、BitmapAndPath 或 BitmapOrPath 是否
	 * 并行安全，当且仅当设置了 rel->consider_parallel。因此可以
	 * 只根据关系级标志设置这条路径的标志，
	 * 而不必真的遍历子路径列表。
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;

	pathnode->path.pathkeys = NIL;	/* 总是无序 */

	pathnode->bitmapquals = bitmapquals;

	/* 这里同时设置 bitmapselectivity 和常规代价成员： */
	cost_bitmap_and_node(pathnode, root);

	return pathnode;
}

/*
 * create_bitmap_or_path
 *	  创建一个表示 BitmapOr 的路径节点。
 */
BitmapOrPath *
create_bitmap_or_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  List *bitmapquals)
{
	BitmapOrPath *pathnode = makeNode(BitmapOrPath);
	Relids		required_outer = NULL;
	ListCell   *lc;

	pathnode->path.pathtype = T_BitmapOr;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * 把所需的外部关系标成各子路径所依赖关系的并。
	 * （也可以坚持让调用方传入，但在这里计算
	 * 更方便也更可靠。）
	 */
	foreach(lc, bitmapquals)
	{
		Path	   *bitmapqual = (Path *) lfirst(lc);

		required_outer = bms_add_members(required_outer,
										 PATH_REQ_OUTER(bitmapqual));
	}
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);

	/*
	 * 目前，BitmapHeapPath、BitmapAndPath 或 BitmapOrPath 是否
	 * 并行安全，当且仅当设置了 rel->consider_parallel。因此可以
	 * 只根据关系级标志设置这条路径的标志，
	 * 而不必真的遍历子路径列表。
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;

	pathnode->path.pathkeys = NIL;	/* 总是无序 */

	pathnode->bitmapquals = bitmapquals;

	/* 这里同时设置 bitmapselectivity 和常规代价成员： */
	cost_bitmap_or_node(pathnode, root);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  创建对应于按 TID 扫描的路径，返回该路径节点。
 */
TidPath *
create_tidscan_path(PlannerInfo *root, RelOptInfo *rel, List *tidquals,
					Relids required_outer)
{
	TidPath    *pathnode = makeNode(TidPath);

	pathnode->path.pathtype = T_TidScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;	/* 总是无序 */

	pathnode->tidquals = tidquals;

	cost_tidscan(&pathnode->path, root, rel, tidquals,
				 pathnode->path.param_info);

	return pathnode;
}

/*
 * create_tidrangescan_path
 *	  创建对应于按一段 TIDs 扫描的路径，返回
 *	  该路径节点。
 */
TidRangePath *
create_tidrangescan_path(PlannerInfo *root, RelOptInfo *rel,
						 List *tidrangequals, Relids required_outer)
{
	TidRangePath *pathnode = makeNode(TidRangePath);

	pathnode->path.pathtype = T_TidRangeScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;	/* 总是无序 */

	pathnode->tidrangequals = tidrangequals;

	cost_tidrangescan(&pathnode->path, root, rel, tidrangequals,
					  pathnode->path.param_info);

	return pathnode;
}

/*
 * create_append_path
 *	  创建对应于 Append 计划的路径，返回
 *	  该路径节点。
 *
 * 注意必须处理 subpaths = NIL，它表示一条哑访问路径。
 * 也有调用方传入 root = NULL。
 */
AppendPath *
create_append_path(PlannerInfo *root,
				   RelOptInfo *rel,
				   List *subpaths, List *partial_subpaths,
				   List *pathkeys, Relids required_outer,
				   int parallel_workers, bool parallel_aware,
				   double rows)
{
	AppendPath *pathnode = makeNode(AppendPath);
	ListCell   *l;

	Assert(!parallel_aware || parallel_workers > 0);

	pathnode->path.pathtype = T_Append;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * 若这是给基关系用的（不是连接或非叶分区），我们更愿意
	 * 用 get_baserel_parampathinfo 为该路径构造完整的 ParamPathInfo。
	 * 这支持在这条路径之上建 Memoize 路径，
	 * 并且若这是分区表，该信息可能对运行时
	 * 裁剪有用（参见 make_partition_pruneinfo()）。
	 *
	 * 不过，若没有 root，那就行不通，我们退回到
	 * 更简单的 get_appendrel_parampathinfo。对哑路径做
	 * 更贵的那套也没有意义。
	 */
	if (rel->reloptkind == RELOPT_BASEREL && root && subpaths != NIL)
		pathnode->path.param_info = get_baserel_parampathinfo(root,
															  rel,
															  required_outer);
	else
		pathnode->path.param_info = get_appendrel_parampathinfo(rel,
																required_outer);

	pathnode->path.parallel_aware = parallel_aware;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = parallel_workers;
	pathnode->path.pathkeys = pathkeys;

	/*
	 * 对并行 append，非部分路径按总代价降序
	 * 排序。这样完成全部非部分路径的总时间
	 * 最小。部分路径则按启动代价降序
	 * 排序。有些路径需要由单个工作进程
	 * 做启动工作。这时工作进程最好先选
	 * 贵的那些，而领导进程应当选择启动代价
	 * 最便宜的计划。
	 */
	if (pathnode->path.parallel_aware)
	{
		/*
		 * 当 Append 带有路径键时，我们不得拨弄子路径的顺序。
		 * 它们列出的顺序对保持路径键有效
		 * 至关重要。
		 */
		Assert(pathkeys == NIL);

		list_sort(subpaths, append_total_cost_compare);
		list_sort(partial_subpaths, append_startup_cost_compare);
	}
	pathnode->first_partial_path = list_length(subpaths);
	pathnode->subpaths = list_concat(subpaths, partial_subpaths);

	/*
	 * 若已知查询级 LIMIT 且路径只针对唯一的基关系，就应用它。
	 * （在这么低的层次处理这一点有点权宜。）
	 */
	if (root != NULL && bms_equal(rel->relids, root->all_query_rels))
		pathnode->limit_tuples = root->limit_tuples;
	else
		pathnode->limit_tuples = -1.0;

	foreach(l, pathnode->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		/* 所有子路径必须有相同的参数化 */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	Assert(!parallel_aware || pathnode->path.parallel_safe);

	/*
	 * 若恰好只有一个子路径，则 Append 的输出
	 * 必然与该子路径有相同的顺序，因此可以继承
	 * 子路径的路径键（若有），覆盖调用方可能说过的内容。
	 * 此外，若子路径的并行意识与 Append 一致，
	 * 则 Append 是空操作，稍后会被丢掉（在 setrefs.c 里）。
	 * 那时也可以继承子路径的大小和代价，实际上对
	 * Append 计零。否则必须做正常的 costsize
	 * 计算。
	 */
	if (list_length(pathnode->subpaths) == 1)
	{
		Path	   *child = (Path *) linitial(pathnode->subpaths);

		if (child->parallel_aware == parallel_aware)
		{
			pathnode->path.rows = child->rows;
			pathnode->path.startup_cost = child->startup_cost;
			pathnode->path.total_cost = child->total_cost;
		}
		else
			cost_append(pathnode);
		/* 必须最后做，否则 cost_append 会抱怨 */
		pathnode->path.pathkeys = child->pathkeys;
	}
	else
		cost_append(pathnode);

	/* 若调用方提供了行数估计，就覆盖计算值。 */
	if (rows >= 0)
		pathnode->path.rows = rows;

	return pathnode;
}

/*
 * append_total_cost_compare
 *	  按 total_cost 降序给 append 子路径排序的
 *	  list_sort 比较器
 *
 * 总代价相等时，退回去比较启动代价；若那些
 * 也相等，就用路径 relid 上的 bms_compare 打破平局。
 * （这是为了避免 list_sort 给出不可预测的结果。）
 */
static int
append_total_cost_compare(const ListCell *a, const ListCell *b)
{
	Path	   *path1 = (Path *) lfirst(a);
	Path	   *path2 = (Path *) lfirst(b);
	int			cmp;

	cmp = compare_path_costs(path1, path2, TOTAL_COST);
	if (cmp != 0)
		return -cmp;
	return bms_compare(path1->parent->relids, path2->parent->relids);
}

/*
 * append_startup_cost_compare
 *	  按 startup_cost 降序给 append 子路径排序的
 *	  list_sort 比较器
 *
 * 启动代价相等时，退回去比较总代价；若那些
 * 也相等，就用路径 relid 上的 bms_compare 打破平局。
 * （这是为了避免 list_sort 给出不可预测的结果。）
 */
static int
append_startup_cost_compare(const ListCell *a, const ListCell *b)
{
	Path	   *path1 = (Path *) lfirst(a);
	Path	   *path2 = (Path *) lfirst(b);
	int			cmp;

	cmp = compare_path_costs(path1, path2, STARTUP_COST);
	if (cmp != 0)
		return -cmp;
	return bms_compare(path1->parent->relids, path2->parent->relids);
}

/*
 * create_merge_append_path
 *	  创建对应于 MergeAppend 计划的路径，返回
 *	  该路径节点。
 */
MergeAppendPath *
create_merge_append_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 List *subpaths,
						 List *pathkeys,
						 Relids required_outer)
{
	MergeAppendPath *pathnode = makeNode(MergeAppendPath);
	Cost		input_startup_cost;
	Cost		input_total_cost;
	ListCell   *l;

	pathnode->path.pathtype = T_MergeAppend;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_appendrel_parampathinfo(rel,
															required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = pathkeys;
	pathnode->subpaths = subpaths;

	/*
	 * 若已知查询级 LIMIT 且路径只针对唯一的基关系，就应用它。
	 * （在这么低的层次处理这一点有点权宜。）
	 */
	if (bms_equal(rel->relids, root->all_query_rels))
		pathnode->limit_tuples = root->limit_tuples;
	else
		pathnode->limit_tuples = -1.0;

	/*
	 * 把输入路径的大小和代价加起来。
	 */
	pathnode->path.rows = 0;
	input_startup_cost = 0;
	input_total_cost = 0;
	foreach(l, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.rows += subpath->rows;
		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		if (pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			/* 子路径顺序已经够用，不必再排序 */
			input_startup_cost += subpath->startup_cost;
			input_total_cost += subpath->total_cost;
		}
		else
		{
			/* 需要插入一个 Sort 节点，因此把那份代价算进去 */
			Path		sort_path;	/* cost_sort 结果的哑占位 */

			cost_sort(&sort_path,
					  root,
					  pathkeys,
					  subpath->total_cost,
					  subpath->parent->tuples,
					  subpath->pathtarget->width,
					  0.0,
					  work_mem,
					  pathnode->limit_tuples);
			input_startup_cost += sort_path.startup_cost;
			input_total_cost += sort_path.total_cost;
		}

		/* 所有子路径必须有相同的参数化 */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	/*
	 * 现在可以计算 MergeAppend 的总代价。若恰好
	 * 只有一个子路径，且其并行意识与
	 * MergeAppend 一致，则 MergeAppend 是空操作，稍后会被丢掉
	 * （在 setrefs.c 里）；否则做正常的代价计算。
	 */
	if (list_length(subpaths) == 1 &&
		((Path *) linitial(subpaths))->parallel_aware ==
		pathnode->path.parallel_aware)
	{
		pathnode->path.startup_cost = input_startup_cost;
		pathnode->path.total_cost = input_total_cost;
	}
	else
		cost_merge_append(&pathnode->path, root,
						  pathkeys, list_length(subpaths),
						  input_startup_cost, input_total_cost,
						  pathnode->path.rows);

	return pathnode;
}

/*
 * create_group_result_path
 *	  创建一条只表示 Result、别无其他的计划路径。
 *
 * 这只用于退化的分组情形，那时我们知道必须
 * 产生一行结果，也许再由 HAVING 限定条件过滤。
 */
GroupResultPath *
create_group_result_path(PlannerInfo *root, RelOptInfo *rel,
						 PathTarget *target, List *havingqual)
{
	GroupResultPath *pathnode = makeNode(GroupResultPath);

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = NULL;	/* 没有其他关系…… */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;
	pathnode->quals = havingqual;

	/*
	 * 我们没法直接用 cost_resultscan()，因为要计入的限定条件
	 * 并不是该关系的 baserestrict 限定条件。不妨就在
	 * 这里权宜处理。
	 */
	pathnode->path.rows = 1;
	pathnode->path.startup_cost = target->cost.startup;
	pathnode->path.total_cost = target->cost.startup +
		cpu_tuple_cost + target->cost.per_tuple;

	/*
	 * 若有限定条件，加上它的代价，但忽略其选择率，因为无论
	 * 限定条件是什么，行数估计都应当是 1。
	 */
	if (havingqual)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, havingqual, root);
		/* havingqual 在启动时求值一次 */
		pathnode->path.startup_cost += qual_cost.startup + qual_cost.per_tuple;
		pathnode->path.total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	return pathnode;
}

/*
 * create_material_path
 *	  创建对应于 Material 计划的路径，返回
 *	  该路径节点。
 */
MaterialPath *
create_material_path(RelOptInfo *rel, Path *subpath)
{
	MaterialPath *pathnode = makeNode(MaterialPath);

	Assert(subpath->parent == rel);

	pathnode->path.pathtype = T_Material;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	cost_material(&pathnode->path,
				  subpath->startup_cost,
				  subpath->total_cost,
				  subpath->rows,
				  subpath->pathtarget->width);

	return pathnode;
}

/*
 * create_memoize_path
 *	  创建对应于 Memoize 计划的路径，返回该路径节点。
 */
MemoizePath *
create_memoize_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
					List *param_exprs, List *hash_operators,
					bool singlerow, bool binary_mode, double calls)
{
	MemoizePath *pathnode = makeNode(MemoizePath);

	Assert(subpath->parent == rel);

	pathnode->path.pathtype = T_Memoize;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;
	pathnode->hash_operators = hash_operators;
	pathnode->param_exprs = param_exprs;
	pathnode->singlerow = singlerow;
	pathnode->binary_mode = binary_mode;
	pathnode->calls = clamp_row_est(calls);

	/*
	 * 目前把 est_entries 设为 0。cost_memoize_rescan() 会做全部
	 * 困难的工作，判断可能有多少缓存项，
	 * 所以最好把填写这个成员留给那个函数。
	 * 若留在 0，执行器会猜一个合适的值。
	 */
	pathnode->est_entries = 0;

	/*
	 * 为缓存第一项再加一点小费用。重新扫描时更难的计算
	 * 都在 cost_memoize_rescan() 里做。
	 */
	pathnode->path.startup_cost = subpath->startup_cost + cpu_tuple_cost;
	pathnode->path.total_cost = subpath->total_cost + cpu_tuple_cost;
	pathnode->path.rows = subpath->rows;

	return pathnode;
}

/*
 * create_unique_path
 *	  创建一条表示从输入数据中消除重复行的路径。
 *	  何谓不同，按 sjinfo 所代表的半连接的需要来定义。
 *	  若无法确定怎样让数据唯一，就返回
 *	  NULL。
 *
 * 若真会用到，很可能对同一关系反复调用；
 * 输入子路径应当始终相同（该关系的 cheapest_total 路径）。
 * 因此我们缓存结果。
 */
UniquePath *
create_unique_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   SpecialJoinInfo *sjinfo)
{
	UniquePath *pathnode;
	Path		sort_path;		/* cost_sort 结果的哑占位 */
	Path		agg_path;		/* cost_agg 结果的哑占位 */
	MemoryContext oldcontext;
	int			numCols;

	/* 若子路径不是 cheapest_total，就是调用方搞错了…… */
	Assert(subpath == rel->cheapest_total_path);
	Assert(subpath->parent == rel);
	/* ……或者 SpecialJoinInfo 用错了 */
	Assert(sjinfo->jointype == JOIN_SEMI);
	Assert(bms_equal(rel->relids, sjinfo->syn_righthand));

	/* 若结果已经缓存，就返回它 */
	if (rel->cheapest_unique_path)
		return (UniquePath *) rel->cheapest_unique_path;

	/* 若不可能做成唯一，就返回 NULL */
	if (!(sjinfo->semi_can_btree || sjinfo->semi_can_hash))
		return NULL;

	/*
	 * 在 GEQO 连接规划期间调用时，我们处在短命的内存
	 * 上下文里。必须确保为基关系创建的路径及任何附属数据
	 * 结构能活过这一轮 GEQO，否则该
	 * 基关系会被以后的 GEQO 轮次弄坏。另一方面，当
	 * GEQO 期间为连接关系创建这些东西时，我们不想让它们
	 * 堆在主规划上下文里。结论是最好的办法
	 * 是在给定 RelOptInfo 所在的同一个上下文里
	 * 显式分配内存。
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	pathnode = makeNode(UniquePath);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;

	/*
	 * 假定输出未排序，因为我们不一定有路径键
	 * 来表示它。（下面可能会覆盖这一点。）
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;

	/*
	 * 在 GEQO 下，sjinfo 可能是短命的，因此最好复制
	 * 从它里面取出的数据结构。
	 */
	pathnode->in_operators = copyObject(sjinfo->semi_operators);
	pathnode->uniq_exprs = copyObject(sjinfo->semi_rhs_exprs);

	/*
	 * 若输入是一个关系，并且它有唯一索引能证明
	 * semi_rhs_exprs 是唯一的，那就什么都不必做。注意
	 * relation_has_unique_index_for 也会自动考虑该关系的
	 * 限制子句。
	 */
	if (rel->rtekind == RTE_RELATION && sjinfo->semi_can_btree &&
		relation_has_unique_index_for(root, rel, NIL,
									  sjinfo->semi_rhs_exprs,
									  sjinfo->semi_operators))
	{
		pathnode->umethod = UNIQUE_PATH_NOOP;
		pathnode->path.rows = rel->rows;
		pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost = subpath->total_cost;
		pathnode->path.pathkeys = subpath->pathkeys;

		rel->cheapest_unique_path = (Path *) pathnode;

		MemoryContextSwitchTo(oldcontext);

		return pathnode;
	}

	/*
	 * 若输入是一个子查询，其输出已经必须唯一，那么我们
	 * 什么都不必做。唯一性检验必须考虑
	 * 我们到底在抽取哪些列；例如 SELECT DISTINCT
	 * x,y 并不保证单独的 x 是不同的。因此除非 semi_rhs_exprs
	 * 只由引用子查询输出的简单 Var 组成，否则无法做
	 * 这项优化。（也许对子查询输出里的
	 * 表达式也能做点什么，但目前保持简单。）
	 */
	if (rel->rtekind == RTE_SUBQUERY)
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

		if (query_supports_distinctness(rte->subquery))
		{
			List	   *sub_tlist_colnos;

			sub_tlist_colnos = translate_sub_tlist(sjinfo->semi_rhs_exprs,
												   rel->relid);

			if (sub_tlist_colnos &&
				query_is_distinct_for(rte->subquery,
									  sub_tlist_colnos,
									  sjinfo->semi_operators))
			{
				pathnode->umethod = UNIQUE_PATH_NOOP;
				pathnode->path.rows = rel->rows;
				pathnode->path.startup_cost = subpath->startup_cost;
				pathnode->path.total_cost = subpath->total_cost;
				pathnode->path.pathkeys = subpath->pathkeys;

				rel->cheapest_unique_path = (Path *) pathnode;

				MemoryContextSwitchTo(oldcontext);

				return pathnode;
			}
		}
	}

	/* 估计输出行数 */
	pathnode->path.rows = estimate_num_groups(root,
											  sjinfo->semi_rhs_exprs,
											  rel->rows,
											  NULL,
											  NULL);
	numCols = list_length(sjinfo->semi_rhs_exprs);

	if (sjinfo->semi_can_btree)
	{
		/*
		 * 估计排序加唯一实现的代价
		 */
		cost_sort(&sort_path, root, NIL,
				  subpath->total_cost,
				  rel->rows,
				  subpath->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);

		/*
		 * 每个输入元组的每次比较收取一个 cpu_operator_cost。我们
		 * 假定大多数元组都会比较全部列。（XXX
		 * 这大概估高了。）这应当与
		 * create_upper_unique_path 一致。
		 */
		sort_path.total_cost += cpu_operator_cost * rel->rows * numCols;
	}

	if (sjinfo->semi_can_hash)
	{
		/*
		 * 把每个散列表项的开销估计为 64 字节（与规划器在
		 * planner.c 里相同）。
		 */
		int			hashentrysize = subpath->pathtarget->width + 64;

		if (hashentrysize * pathnode->path.rows > get_hash_memory_limit())
		{
			/*
			 * 我们不应当尝试散列。权宜改写 SpecialJoinInfo，
			 * 以便记住这一点，以防再次经过这里。
			 */
			sjinfo->semi_can_hash = false;
		}
		else
			cost_agg(&agg_path, root,
					 AGG_HASHED, NULL,
					 numCols, pathnode->path.rows,
					 NIL,
					 subpath->startup_cost,
					 subpath->total_cost,
					 rel->rows,
					 subpath->pathtarget->width);
	}

	if (sjinfo->semi_can_btree && sjinfo->semi_can_hash)
	{
		if (agg_path.total_cost < sort_path.total_cost)
			pathnode->umethod = UNIQUE_PATH_HASH;
		else
			pathnode->umethod = UNIQUE_PATH_SORT;
	}
	else if (sjinfo->semi_can_btree)
		pathnode->umethod = UNIQUE_PATH_SORT;
	else if (sjinfo->semi_can_hash)
		pathnode->umethod = UNIQUE_PATH_HASH;
	else
	{
		/* 只有在上面放弃散列之后才会到这里 */
		MemoryContextSwitchTo(oldcontext);
		return NULL;
	}

	if (pathnode->umethod == UNIQUE_PATH_HASH)
	{
		pathnode->path.startup_cost = agg_path.startup_cost;
		pathnode->path.total_cost = agg_path.total_cost;
	}
	else
	{
		pathnode->path.startup_cost = sort_path.startup_cost;
		pathnode->path.total_cost = sort_path.total_cost;
	}

	rel->cheapest_unique_path = (Path *) pathnode;

	MemoryContextSwitchTo(oldcontext);

	return pathnode;
}

/*
 * create_gather_merge_path
 *
 *	  创建对应于汇集归并扫描的路径，返回
 *	  该路径节点。
 */
GatherMergePath *
create_gather_merge_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
						 PathTarget *target, List *pathkeys,
						 Relids required_outer, double *rows)
{
	GatherMergePath *pathnode = makeNode(GatherMergePath);
	Cost		input_startup_cost = 0;
	Cost		input_total_cost = 0;

	Assert(subpath->parallel_safe);
	Assert(pathkeys);

	pathnode->path.pathtype = T_GatherMerge;
	pathnode->path.parent = rel;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;

	pathnode->subpath = subpath;
	pathnode->num_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = pathkeys;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.rows += subpath->rows;

	if (pathkeys_contained_in(pathkeys, subpath->pathkeys))
	{
		/* 子路径顺序已经够用，不必再排序 */
		input_startup_cost += subpath->startup_cost;
		input_total_cost += subpath->total_cost;
	}
	else
	{
		/* 需要插入一个 Sort 节点，因此把那份代价算进去 */
		Path		sort_path;	/* cost_sort 结果的哑占位 */

		cost_sort(&sort_path,
				  root,
				  pathkeys,
				  subpath->total_cost,
				  subpath->rows,
				  subpath->pathtarget->width,
				  0.0,
				  work_mem,
				  -1);
		input_startup_cost += sort_path.startup_cost;
		input_total_cost += sort_path.total_cost;
	}

	cost_gather_merge(pathnode, root, rel, pathnode->path.param_info,
					  input_startup_cost, input_total_cost, rows);

	return pathnode;
}

/*
 * translate_sub_tlist：取得 tlist 所表示的子查询列号
 *
 * 给定的目标列表通常只含引用给定 relid 的 Var。
 * 取出它们的 varattno（也就是子查询的列号）并作为
 * 整数 List 返回。
 *
 * 若 tlist 里有任何一项不是简单 Var，我们就无法判断
 * 子查询的唯一性条件（若有）是否与我们的匹配，于是放弃并
 * 返回 NIL。
 */
static List *
translate_sub_tlist(List *tlist, int relid)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, tlist)
	{
		Var		   *var = (Var *) lfirst(l);

		if (!var || !IsA(var, Var) ||
			var->varno != relid)
			return NIL;			/* 放弃 */

		result = lappend_int(result, var->varattno);
	}
	return result;
}

/*
 * create_gather_path
 *	  创建对应于汇集扫描的路径，返回
 *	  该路径节点。
 *
 * rows 可以选填，用来覆盖来自其他来源的行数估计。
 */
GatherPath *
create_gather_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   PathTarget *target, Relids required_outer, double *rows)
{
	GatherPath *pathnode = makeNode(GatherPath);

	Assert(subpath->parallel_safe);

	pathnode->path.pathtype = T_Gather;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;	/* Gather 的结果无序 */

	pathnode->subpath = subpath;
	pathnode->num_workers = subpath->parallel_workers;
	pathnode->single_copy = false;

	if (pathnode->num_workers == 0)
	{
		pathnode->path.pathkeys = subpath->pathkeys;
		pathnode->num_workers = 1;
		pathnode->single_copy = true;
	}

	cost_gather(pathnode, root, rel, pathnode->path.param_info, rows);

	return pathnode;
}

/*
 * create_subqueryscan_path
 *	  创建对应于扫描子查询的路径，
 *	  返回该路径节点。
 *
 * 若调用方认为 rel->reltarget 是平凡的，也就是只按顺序取出
 * 子查询的全部输出列，就必须传入 trivial_pathtarget = true。
 * 虽然我们在这里也能判断，但调用方通常能做得
 * 更高效（或至少把开销分摊到多次调用上）。
 */
SubqueryScanPath *
create_subqueryscan_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
						 bool trivial_pathtarget,
						 List *pathkeys, Relids required_outer)
{
	SubqueryScanPath *pathnode = makeNode(SubqueryScanPath);

	pathnode->path.pathtype = T_SubqueryScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = pathkeys;
	pathnode->subpath = subpath;

	cost_subqueryscan(pathnode, root, rel, pathnode->path.param_info,
					  trivial_pathtarget);

	return pathnode;
}

/*
 * create_functionscan_path
 *	  创建对应于对函数做顺序扫描的路径，
 *	  返回该路径节点。
 */
Path *
create_functionscan_path(PlannerInfo *root, RelOptInfo *rel,
						 List *pathkeys, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_FunctionScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = pathkeys;

	cost_functionscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_tablefuncscan_path
 *	  创建对应于对表函数做顺序扫描的路径，
 *	  返回该路径节点。
 */
Path *
create_tablefuncscan_path(PlannerInfo *root, RelOptInfo *rel,
						  Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_TableFuncScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 结果总是无序 */

	cost_tablefuncscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_valuesscan_path
 *	  创建对应于扫描 VALUES 列表的路径，
 *	  返回该路径节点。
 */
Path *
create_valuesscan_path(PlannerInfo *root, RelOptInfo *rel,
					   Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_ValuesScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 结果总是无序 */

	cost_valuesscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_ctescan_path
 *	  创建对应于扫描非自引用 CTE 的路径，
 *	  返回该路径节点。
 */
Path *
create_ctescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_CteScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* XXX 目前结果总是无序 */

	cost_ctescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_namedtuplestorescan_path
 *	  创建对应于扫描命名元组存储的路径，返回
 *	  该路径节点。
 */
Path *
create_namedtuplestorescan_path(PlannerInfo *root, RelOptInfo *rel,
								Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_NamedTuplestoreScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 结果总是无序 */

	cost_namedtuplestorescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_resultscan_path
 *	  创建对应于扫描 RTE_RESULT 关系的路径，
 *	  返回该路径节点。
 */
Path *
create_resultscan_path(PlannerInfo *root, RelOptInfo *rel,
					   Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_Result;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 结果总是无序 */

	cost_resultscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_worktablescan_path
 *	  创建对应于扫描自引用 CTE 的路径，
 *	  返回该路径节点。
 */
Path *
create_worktablescan_path(PlannerInfo *root, RelOptInfo *rel,
						  Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_WorkTableScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* 结果总是无序 */

	/* 代价与普通 CTE 扫描相同 */
	cost_ctescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_foreignscan_path
 *	  创建对应于扫描外部基表的路径，
 *	  返回该路径节点。
 *
 * 本函数从不由核心 Postgres 调用；而是预期
 * 由外部数据包装器的 GetForeignPaths 函数来调用。
 * 我们让 FDW 提供路径的全部成员，因为核心里没有任何办法
 * 计算它们。不过 pathtarget（rel->reltarget）通常有合理默认值，
 * 因此 target 为 NULL 时就选用它。
 */
ForeignPath *
create_foreignscan_path(PlannerInfo *root, RelOptInfo *rel,
						PathTarget *target,
						double rows, Cost startup_cost, Cost total_cost,
						List *pathkeys,
						Relids required_outer,
						Path *fdw_outerpath,
						List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/* 历史上有些 FDWs 搞不清该何时使用这个 */
	Assert(IS_SIMPLE_REL(rel));

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * create_foreign_join_path
 *	  创建对应于扫描外部连接的路径，
 *	  返回该路径节点。
 *
 * 本函数从不由核心 Postgres 调用；而是预期
 * 由外部数据包装器的 GetForeignJoinPaths 函数来调用。
 * 我们让 FDW 提供路径的全部成员，因为核心里没有任何办法
 * 计算它们。不过 pathtarget（rel->reltarget）通常有合理默认值，
 * 因此 target 为 NULL 时就选用它。
 */
ForeignPath *
create_foreign_join_path(PlannerInfo *root, RelOptInfo *rel,
						 PathTarget *target,
						 double rows, Cost startup_cost, Cost total_cost,
						 List *pathkeys,
						 Relids required_outer,
						 Path *fdw_outerpath,
						 List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/*
	 * 参数化路径本应使用 get_joinrel_parampathinfo 来处理，
	 * 但本函数的 API 不支持，而且现有
	 * 扩展也还没尝试建造这种路径。目前
	 * 若有人尝试，就直接报错；以后应当
	 * 再回来看这个问题。
	 */
	if (!bms_is_empty(required_outer) || !bms_is_empty(rel->lateral_relids))
		elog(ERROR, "parameterized foreign joins are not supported yet");

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = NULL;	/* XXX 见上 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * create_foreign_upper_path
 *	  创建一条对应于由 FDW 直接计算的上层关系的路径，
 *	  返回该路径节点。
 *
 * 本函数从不由核心 Postgres 调用；而是预期由
 * 外部数据包装器的 GetForeignUpperPaths 函数来调用。
 * 我们让 FDW 提供路径的全部成员，因为核心里没有任何办法
 * 计算它们。不过 pathtarget（rel->reltarget）通常有合理默认值，
 * 因此 target 为 NULL 时就选用它。
 */
ForeignPath *
create_foreign_upper_path(PlannerInfo *root, RelOptInfo *rel,
						  PathTarget *target,
						  double rows, Cost startup_cost, Cost total_cost,
						  List *pathkeys,
						  Path *fdw_outerpath,
						  List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/*
	 * 上层关系不应当再有任何 LATERAL 引用，因为连接
	 * 已经完成。
	 */
	Assert(bms_is_empty(rel->lateral_relids));

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * calc_nestloop_required_outer
 *	  计算嵌套循环连接路径的 required_outer 集合
 *
 * 注意：结果不得与任一输入共享存储
 */
Relids
calc_nestloop_required_outer(Relids outerrelids,
							 Relids outer_paramrels,
							 Relids innerrelids,
							 Relids inner_paramrels)
{
	Relids		required_outer;

	/* inner_path 可以需要外侧路径的关系，但反过来不行 */
	Assert(!bms_overlap(outer_paramrels, innerrelids));
	/* 内侧路径未参数化时，这是容易的情形 */
	if (!inner_paramrels)
		return bms_copy(outer_paramrels);
	/* 否则，做成并…… */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* ……并去掉任何已经满足的外部关系 */
	required_outer = bms_del_members(required_outer,
									 outerrelids);
	return required_outer;
}

/*
 * calc_non_nestloop_required_outer
 *	  计算归并或散列连接路径的 required_outer 集合
 *
 * 注意：结果不得与任一输入共享存储
 */
Relids
calc_non_nestloop_required_outer(Path *outer_path, Path *inner_path)
{
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		required_outer;

	/* 两条路径都不能需要对方的关系 */
	Assert(!bms_overlap(outer_paramrels, inner_path->parent->relids));
	Assert(!bms_overlap(inner_paramrels, outer_path->parent->relids));
	/* 做成并…… */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* 不必显式测试空集；bms_union 会处理对 */
	return required_outer;
}

/*
 * create_nestloop_path
 *	  创建对应于两个关系之间嵌套循环连接的
 *	  路径节点。
 *
 * joinrel 是连接关系。
 * jointype 是所需要的连接类型。
 * workspace 是 initial_cost_nestloop 的结果。
 * extra 含有关于该连接的各种信息。
 * outer_path 是外侧路径。
 * inner_path 是内侧路径。
 * restrict_clauses 是要在连接上应用的 RestrictInfo 节点。
 * pathkeys 是新连接路径的路径键。
 * required_outer 是所需外部关系的集合。
 *
 * 返回得到的路径节点。
 */
NestPath *
create_nestloop_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *workspace,
					 JoinPathExtraData *extra,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *pathkeys,
					 Relids required_outer)
{
	NestPath   *pathnode = makeNode(NestPath);
	Relids		inner_req_outer = PATH_REQ_OUTER(inner_path);

	/*
	 * 若内侧路径由外侧参数化，就必须丢掉任何
	 * 预定要移进内侧路径的 restrict_clauses。现在就得
	 * 做，而不能拖到 createplan 时再做，
	 * 因为 restrict_clauses 列表会影响这条路径的大小和代价
	 * 估计。检测这类子句的办法是看序号是否
	 * 匹配内侧路径里已经强制的子句。
	 */
	if (bms_overlap(inner_req_outer, outer_path->parent->relids))
	{
		Bitmapset  *enforced_serials = get_param_path_clause_serials(inner_path);
		List	   *jclauses = NIL;
		ListCell   *lc;

		foreach(lc, restrict_clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (!bms_is_member(rinfo->rinfo_serial, enforced_serials))
				jclauses = lappend(jclauses, rinfo);
		}
		restrict_clauses = jclauses;
	}

	pathnode->jpath.path.pathtype = T_NestLoop;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = joinrel->reltarget;
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  extra->sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware = false;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* 这样估计 parallel_workers 很笨，但目前先这样…… */
	pathnode->jpath.path.parallel_workers = outer_path->parallel_workers;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.inner_unique = extra->inner_unique;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;

	final_cost_nestloop(root, pathnode, workspace, extra);

	return pathnode;
}

/*
 * create_mergejoin_path
 *	  创建对应于两个关系之间归并连接的
 *	  路径节点。
 *
 * joinrel 是连接关系。
 * jointype 是所需要的连接类型。
 * workspace 是 initial_cost_mergejoin 的结果。
 * extra 含有关于该连接的各种信息。
 * outer_path 是外侧路径。
 * inner_path 是内侧路径。
 * restrict_clauses 是要在连接上应用的 RestrictInfo 节点。
 * pathkeys 是新连接路径的路径键。
 * required_outer 是所需外部关系的集合。
 * mergeclauses 是用作归并子句的 RestrictInfo 节点
 *		（它应当是 restrict_clauses 列表的子集）。
 * outersortkeys 是外侧关系的排序变量键。
 * innersortkeys 是内侧关系的排序变量键。
 */
MergePath *
create_mergejoin_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  JoinCostWorkspace *workspace,
					  JoinPathExtraData *extra,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  Relids required_outer,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys)
{
	MergePath  *pathnode = makeNode(MergePath);

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = joinrel->reltarget;
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  extra->sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware = false;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* 这样估计 parallel_workers 很笨，但目前先这样…… */
	pathnode->jpath.path.parallel_workers = outer_path->parallel_workers;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.inner_unique = extra->inner_unique;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;
	/* pathnode->skip_mark_restore 将由 final_cost_mergejoin 设置 */
	/* pathnode->materialize_inner 将由 final_cost_mergejoin 设置 */

	final_cost_mergejoin(root, pathnode, workspace, extra);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  创建对应于两个关系之间散列连接的路径节点。
 *
 * joinrel 是连接关系。
 * jointype 是所需要的连接类型。
 * workspace 是 initial_cost_hashjoin 的结果。
 * extra 含有关于该连接的各种信息。
 * outer_path 是最便宜的外侧路径。
 * inner_path 是最便宜的内侧路径。
 * parallel_hash 用来选择内侧路径的并行散列（共享散列表）。
 * restrict_clauses 是要在连接上应用的 RestrictInfo 节点。
 * required_outer 是所需外部关系的集合。
 * hashclauses 是用作散列子句的 RestrictInfo 节点
 *		（它应当是 restrict_clauses 列表的子集）。
 */
HashPath *
create_hashjoin_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *workspace,
					 JoinPathExtraData *extra,
					 Path *outer_path,
					 Path *inner_path,
					 bool parallel_hash,
					 List *restrict_clauses,
					 Relids required_outer,
					 List *hashclauses)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = joinrel->reltarget;
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  extra->sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware =
		joinrel->consider_parallel && parallel_hash;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* 这样估计 parallel_workers 很笨，但目前先这样…… */
	pathnode->jpath.path.parallel_workers = outer_path->parallel_workers;

	/*
	 * 散列连接从来没有路径键，因为它的输出次序
	 * 由于可能分批而不可预测。XXX 若内侧关系
	 * 足够小，我们可以指示执行器不得分批，
	 * 然后就可以假定输出继承外侧关系的
	 * 次序，这也许能省掉一步排序。不过若我们对
	 * 内侧关系大小的估计严重偏离，下行风险相当大。
	 * 目前我们不冒这个险。（也要注意，若真想认真做这件事，
	 * joinpath.c 就必须为外侧关系考虑比现在
	 * 多得多的路径。）
	 */
	pathnode->jpath.path.pathkeys = NIL;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.inner_unique = extra->inner_unique;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_hashclauses = hashclauses;
	/* final_cost_hashjoin 会填上 pathnode->num_batches */

	final_cost_hashjoin(root, pathnode, workspace, extra);

	return pathnode;
}

/*
 * create_projection_path
 *	  创建一个表示执行投影的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 */
ProjectionPath *
create_projection_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   Path *subpath,
					   PathTarget *target)
{
	ProjectionPath *pathnode = makeNode(ProjectionPath);
	PathTarget *oldtarget;

	/*
	 * 不得把一个 ProjectionPath 直接放在另一个上面；那没有用，
	 * 还会把 create_projection_plan 弄糊涂。与其确保所有
	 * 调用方都处理这一点，不如在这里实现：剥掉所给内容里的任何
	 * ProjectionPath。按这条规则，不会有
	 * 多于一个。
	 */
	if (IsA(subpath, ProjectionPath))
	{
		ProjectionPath *subpp = (ProjectionPath *) subpath;

		Assert(subpp->path.parent == rel);
		subpath = subpp->subpath;
		Assert(!IsA(subpath, ProjectionPath));
	}

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe &&
		is_parallel_safe(root, (Node *) target->exprs);
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* 投影不改变排序次序 */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	/*
	 * 也许不需要单独的 Result 节点。若输入计划节点
	 * 类型能够投影，只要让它投影别的东西即可。或者，
	 * 若它
	 * 不能投影，但想要的目标与输入反正会产生的表达式列表相同，
	 * 仍可以把想要的 tlist 给它（也许改 ressortgroupref 标签，
	 * 但别的不动）。注意：后一种情形里，create_projection_plan
	 * 必须重新检查我们的结论；见那里的注释。
	 */
	oldtarget = subpath->pathtarget;
	if (is_projection_capable_path(subpath) ||
		equal(oldtarget->exprs, target->exprs))
	{
		/* 不需要单独的 Result 节点 */
		pathnode->dummypp = true;

		/*
		 * 把计划代价设为子路径代价，并按替换后的 tlist 调整。
		 */
		pathnode->path.rows = subpath->rows;
		pathnode->path.startup_cost = subpath->startup_cost +
			(target->cost.startup - oldtarget->cost.startup);
		pathnode->path.total_cost = subpath->total_cost +
			(target->cost.startup - oldtarget->cost.startup) +
			(target->cost.per_tuple - oldtarget->cost.per_tuple) * subpath->rows;
	}
	else
	{
		/* 我们确实需要这个 Result 节点 */
		pathnode->dummypp = false;

		/*
		 * Result 节点的代价是每行一个 cpu_tuple_cost，再加上
		 * 对 tlist 求值的代价。没有限定条件要操心。
		 */
		pathnode->path.rows = subpath->rows;
		pathnode->path.startup_cost = subpath->startup_cost +
			target->cost.startup;
		pathnode->path.total_cost = subpath->total_cost +
			target->cost.startup +
			(cpu_tuple_cost + target->cost.per_tuple) * subpath->rows;
	}

	return pathnode;
}

/*
 * apply_projection_to_path
 *	  增加一个投影步骤，或直接把目标应用到给定路径上。
 *
 * 净效果与 create_projection_path() 相同，只不过若不需要
 * 单独的 Result 计划节点，就直接替换给定路径的
 * pathtarget。只有调用方知道给定路径没有在别处被引用、
 * 因而可以就地修改时，
 * 才必须这样用。
 *
 * 若输入路径是 GatherPath 或 GatherMergePath，也尝试把新目标
 * 下推到它的输入；这是对输入路径更进一步的侵入式修改，
 * create_projection_path() 做不到。
 *
 * 注意不得改掉源路径的父链接；因此当它被
 * add_path 进 rel 时会有点不一致。到目前为止
 * 还没惹出麻烦。
 *
 * rel 是与结果关联的父关系。
 * path 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 */
Path *
apply_projection_to_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *path,
						 PathTarget *target)
{
	QualCost	oldcost;

	/*
	 * 若给定路径不能投影，可能需要一个 Result 节点，因此做一个
	 * 单独的 ProjectionPath。
	 */
	if (!is_projection_capable_path(path))
		return (Path *) create_projection_path(root, rel, path, target);

	/*
	 * 我们可以把想要的 tlist 直接塞进现有路径，同时务必
	 * 适当地更新它的代价估计。
	 */
	oldcost = path->pathtarget->cost;
	path->pathtarget = target;

	path->startup_cost += target->cost.startup - oldcost.startup;
	path->total_cost += target->cost.startup - oldcost.startup +
		(target->cost.per_tuple - oldcost.per_tuple) * path->rows;

	/*
	 * 若路径恰好是 Gather 或 GatherMerge 路径，我们希望
	 * 安排子路径返回所需的目标列表，以便
	 * 工作进程能帮忙投影。但若目标表达式里有什么
	 * 不是并行安全的，那就做不到。
	 */
	if ((IsA(path, GatherPath) || IsA(path, GatherMergePath)) &&
		is_parallel_safe(root, (Node *) target->exprs))
	{
		/*
		 * 这里总是用 create_projection_path，即使子路径
		 * 能够投影，以免就地修改子路径。
		 * 目前似乎不大可能还有别的地方
		 * 引用这条子路径，但小心为上。
		 *
		 * 注意我们不改变并行路径的代价估计；也许
		 * 应当改，以反映目标求值的大头
		 * 会发生在工作进程里。
		 */
		if (IsA(path, GatherPath))
		{
			GatherPath *gpath = (GatherPath *) path;

			gpath->subpath = (Path *)
				create_projection_path(root,
									   gpath->subpath->parent,
									   gpath->subpath,
									   target);
		}
		else
		{
			GatherMergePath *gmpath = (GatherMergePath *) path;

			gmpath->subpath = (Path *)
				create_projection_path(root,
									   gmpath->subpath->parent,
									   gmpath->subpath,
									   target);
		}
	}
	else if (path->parallel_safe &&
			 !is_parallel_safe(root, (Node *) target->exprs))
	{
		/*
		 * 我们正把一个并行受限的目标列表插进一条目前
		 * 标成并行安全的路径，因此必须把它标成不再
		 * 安全。
		 */
		path->parallel_safe = false;
	}

	return path;
}

/*
 * create_set_projection_path
 *	  创建一个路径节点，表示执行包含集合返回函数的
 *	  投影。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 */
ProjectSetPath *
create_set_projection_path(PlannerInfo *root,
						   RelOptInfo *rel,
						   Path *subpath,
						   PathTarget *target)
{
	ProjectSetPath *pathnode = makeNode(ProjectSetPath);
	double		tlist_rows;
	ListCell   *lc;

	pathnode->path.pathtype = T_ProjectSet;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe &&
		is_parallel_safe(root, (Node *) target->exprs);
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* 投影不改变排序次序，XXX？ */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	/*
	 * 估计输入的每一行会由 SRFs 产生多少行；若
	 * 本节点里不止一个，就用最大的那个。
	 */
	tlist_rows = 1;
	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);
		double		itemrows;

		itemrows = expression_returns_set_rows(root, node);
		if (tlist_rows < itemrows)
			tlist_rows = itemrows;
	}

	/*
	 * 除了求值 tlist 的代价，每个输入行收取 cpu_tuple_cost，
	 * 每个新增输出行收取一半 cpu_tuple_cost。
	 * 这也许有点古怪，但 9.6 就是这么做的；以后也许会
	 * 重新审视这个估计。
	 */
	pathnode->path.rows = subpath->rows * tlist_rows;
	pathnode->path.startup_cost = subpath->startup_cost +
		target->cost.startup;
	pathnode->path.total_cost = subpath->total_cost +
		target->cost.startup +
		(cpu_tuple_cost + target->cost.per_tuple) * subpath->rows +
		(pathnode->path.rows - subpath->rows) * cpu_tuple_cost / 2;

	return pathnode;
}

/*
 * create_incremental_sort_path
 *	  创建一个表示执行增量排序的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * pathkeys 表示想要的排序次序。
 * presorted_keys 是输入路径已经排好序的
 *		键的个数。
 * limit_tuples 是输出元组数的估计上界，
 *		若没有 LIMIT 或估计不出来则为 -1。
 */
IncrementalSortPath *
create_incremental_sort_path(PlannerInfo *root,
							 RelOptInfo *rel,
							 Path *subpath,
							 List *pathkeys,
							 int presorted_keys,
							 double limit_tuples)
{
	IncrementalSortPath *sort = makeNode(IncrementalSortPath);
	SortPath   *pathnode = &sort->spath;

	pathnode->path.pathtype = T_IncrementalSort;
	pathnode->path.parent = rel;
	/* 排序不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = pathkeys;

	pathnode->subpath = subpath;

	cost_incremental_sort(&pathnode->path,
						  root, pathkeys, presorted_keys,
						  subpath->startup_cost,
						  subpath->total_cost,
						  subpath->rows,
						  subpath->pathtarget->width,
						  0.0,	/* XXX comparison_cost 不该是 0 吧？ */
						  work_mem, limit_tuples);

	sort->nPresortedCols = presorted_keys;

	return sort;
}

/*
 * create_sort_path
 *	  创建一个表示执行显式排序的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * pathkeys 表示想要的排序次序。
 * limit_tuples 是输出元组数的估计上界，
 *		若没有 LIMIT 或估计不出来则为 -1。
 */
SortPath *
create_sort_path(PlannerInfo *root,
				 RelOptInfo *rel,
				 Path *subpath,
				 List *pathkeys,
				 double limit_tuples)
{
	SortPath   *pathnode = makeNode(SortPath);

	pathnode->path.pathtype = T_Sort;
	pathnode->path.parent = rel;
	/* 排序不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = pathkeys;

	pathnode->subpath = subpath;

	cost_sort(&pathnode->path, root, pathkeys,
			  subpath->total_cost,
			  subpath->rows,
			  subpath->pathtarget->width,
			  0.0,				/* XXX comparison_cost 不该是 0 吧？ */
			  work_mem, limit_tuples);

	return pathnode;
}

/*
 * create_group_path
 *	  创建一个路径节点，表示对已预排序的输入做分组。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 * groupClause 是表示分组的 SortGroupClause 列表。
 * qual 是 HAVING 限定条件，若有的话。
 * numGroups 是估计的分组数。
 */
GroupPath *
create_group_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  List *groupClause,
				  List *qual,
				  double numGroups)
{
	GroupPath  *pathnode = makeNode(GroupPath);
	PathTarget *target = rel->reltarget;

	pathnode->path.pathtype = T_Group;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* 分组不改变排序次序 */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_group(&pathnode->path, root,
			   list_length(groupClause),
			   numGroups,
			   qual,
			   subpath->startup_cost, subpath->total_cost,
			   subpath->rows);

	/* 为每个输出行加上 tlist 求值代价 */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_upper_unique_path
 *	  创建一个路径节点，表示在已预排序的输入上
 *	  执行显式的 Unique 步骤。
 *
 * 这会产生 Unique 计划节点，但使用场景与
 * create_unique_path 差别很大，似乎不值得把两者合并。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * numCols 是分组列的数目。
 * numGroups 是估计的分组数。
 *
 * 输入路径必须按分组列排序，可能还加上
 * 额外的列；因此前 numCols 个路径键就是这些分组列。
 */
UpperUniquePath *
create_upper_unique_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *subpath,
						 int numCols,
						 double numGroups)
{
	UpperUniquePath *pathnode = makeNode(UpperUniquePath);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;
	/* Unique 不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* Unique 不改变输入次序 */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;
	pathnode->numkeys = numCols;

	/*
	 * 每个输入元组的每次比较收取一个 cpu_operator_cost。我们假定
	 * 大多数元组都会比较全部列。（XXX 这大概
	 * 估高了。）
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_operator_cost * subpath->rows * numCols;
	pathnode->path.rows = numGroups;

	return pathnode;
}

/*
 * create_agg_path
 *	  创建一个表示执行聚集或分组的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 * aggstrategy 是 Agg 节点的基本实现策略。
 * aggsplit 是 Agg 节点的聚集拆分模式。
 * groupClause 是表示分组的 SortGroupClause 列表。
 * qual 是 HAVING 限定条件，若有的话。
 * aggcosts 含有待计算聚集函数的代价信息。
 * numGroups 是估计的分组数（不分组时为 1）。
 */
AggPath *
create_agg_path(PlannerInfo *root,
				RelOptInfo *rel,
				Path *subpath,
				PathTarget *target,
				AggStrategy aggstrategy,
				AggSplit aggsplit,
				List *groupClause,
				List *qual,
				const AggClauseCosts *aggcosts,
				double numGroups)
{
	AggPath    *pathnode = makeNode(AggPath);

	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;

	if (aggstrategy == AGG_SORTED)
	{
		/*
		 * 试图保留子路径的次序。为支持 ORDER BY 和 DISTINCT 聚集，
		 * adjust_group_pathkeys_for_groupagg() 里可能已经加上了额外路径键。
		 * 加在那里的路径键属于聚集函数内部的列，因此必须剥掉
		 * 这些额外路径键，因为那些列在
		 * 聚集节点之上
		 * 是得不到的。
		 */
		if (list_length(subpath->pathkeys) > root->num_groupby_pathkeys)
			pathnode->path.pathkeys = list_copy_head(subpath->pathkeys,
													 root->num_groupby_pathkeys);
		else
			pathnode->path.pathkeys = subpath->pathkeys;	/* 保持次序 */
	}
	else
		pathnode->path.pathkeys = NIL;	/* 输出无序 */

	pathnode->subpath = subpath;

	pathnode->aggstrategy = aggstrategy;
	pathnode->aggsplit = aggsplit;
	pathnode->numGroups = numGroups;
	pathnode->transitionSpace = aggcosts ? aggcosts->transitionSpace : 0;
	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_agg(&pathnode->path, root,
			 aggstrategy, aggcosts,
			 list_length(groupClause), numGroups,
			 qual,
			 subpath->startup_cost, subpath->total_cost,
			 subpath->rows, subpath->pathtarget->width);

	/* 为每个输出行加上 tlist 求值代价 */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_groupingsets_path
 *	  创建一个表示执行 GROUPING SETS 聚集的路径节点。
 *
 * GroupingSetsPath 表示带一个或多个分组集的有序分组。
 * 输入路径的结果必须排序，以匹配 rollup_groupclauses 里的
 * 最后一项。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 * having_qual 是 HAVING 限定条件，若有的话。
 * rollups 是 RollupData 节点的列表。
 * agg_costs 含有待计算聚集函数的代价信息。
 */
GroupingSetsPath *
create_groupingsets_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *subpath,
						 List *having_qual,
						 AggStrategy aggstrategy,
						 List *rollups,
						 const AggClauseCosts *agg_costs)
{
	GroupingSetsPath *pathnode = makeNode(GroupingSetsPath);
	PathTarget *target = rel->reltarget;
	ListCell   *lc;
	bool		is_first = true;
	bool		is_first_sort = true;

	/* 最顶上生成的 Plan 节点将是一个 Agg */
	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->subpath = subpath;

	/*
	 * 若可能，在这里把 AGG_SORTED 降成 AGG_PLAIN，把 AGG_MIXED
	 * 降成 AGG_HASHED，以简化调用方。
	 */
	if (aggstrategy == AGG_SORTED &&
		list_length(rollups) == 1 &&
		((RollupData *) linitial(rollups))->groupClause == NIL)
		aggstrategy = AGG_PLAIN;

	if (aggstrategy == AGG_MIXED &&
		list_length(rollups) == 1)
		aggstrategy = AGG_HASHED;

	/*
	 * 当且仅当对非空分组表达式列表只有一次
	 * rollup 操作时，输出才会按 group_pathkeys
	 * 排好序。
	 */
	if (aggstrategy == AGG_SORTED && list_length(rollups) == 1)
		pathnode->path.pathkeys = root->group_pathkeys;
	else
		pathnode->path.pathkeys = NIL;

	pathnode->aggstrategy = aggstrategy;
	pathnode->rollups = rollups;
	pathnode->qual = having_qual;
	pathnode->transitionSpace = agg_costs ? agg_costs->transitionSpace : 0;

	Assert(rollups != NIL);
	Assert(aggstrategy != AGG_PLAIN || list_length(rollups) == 1);
	Assert(aggstrategy != AGG_MIXED || list_length(rollups) > 1);

	foreach(lc, rollups)
	{
		RollupData *rollup = lfirst(lc);
		List	   *gsets = rollup->gsets;
		int			numGroupCols = list_length(linitial(gsets));

		/*
		 * 在 AGG_SORTED 或 AGG_PLAIN 模式下，第一次 rollup 取
		 * （已经排好序的）输入，后面的各自排序。
		 *
		 * 在 AGG_HASHED 模式下，每个分组集有一次 rollup。
		 *
		 * 在 AGG_MIXED 模式下，前面的 rollup 做散列，第一个
		 * 非散列的取（已经排好序的）输入，后面的
		 * 各自排序。
		 */
		if (is_first)
		{
			cost_agg(&pathnode->path, root,
					 aggstrategy,
					 agg_costs,
					 numGroupCols,
					 rollup->numGroups,
					 having_qual,
					 subpath->startup_cost,
					 subpath->total_cost,
					 subpath->rows,
					 subpath->pathtarget->width);
			is_first = false;
			if (!rollup->is_hashed)
				is_first_sort = false;
		}
		else
		{
			Path		sort_path;	/* cost_sort 结果的哑占位 */
			Path		agg_path;	/* cost_agg 结果的哑占位 */

			if (rollup->is_hashed || is_first_sort)
			{
				/*
				 * 计入聚集的代价，但不要再次
				 * 收取输入代价
				 */
				cost_agg(&agg_path, root,
						 rollup->is_hashed ? AGG_HASHED : AGG_SORTED,
						 agg_costs,
						 numGroupCols,
						 rollup->numGroups,
						 having_qual,
						 0.0, 0.0,
						 subpath->rows,
						 subpath->pathtarget->width);
				if (!rollup->is_hashed)
					is_first_sort = false;
			}
			else
			{
				/* 计入排序代价，但不要再次收取输入代价 */
				cost_sort(&sort_path, root, NIL,
						  0.0,
						  subpath->rows,
						  subpath->pathtarget->width,
						  0.0,
						  work_mem,
						  -1.0);

				/* 计入聚集的代价 */

				cost_agg(&agg_path, root,
						 AGG_SORTED,
						 agg_costs,
						 numGroupCols,
						 rollup->numGroups,
						 having_qual,
						 sort_path.startup_cost,
						 sort_path.total_cost,
						 sort_path.rows,
						 subpath->pathtarget->width);
			}

			pathnode->path.total_cost += agg_path.total_cost;
			pathnode->path.rows += agg_path.rows;
		}
	}

	/* 为每个输出行加上 tlist 求值代价 */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_minmaxagg_path
 *	  创建一个表示计算 MIN/MAX 聚集的路径节点。
 *
 * rel 是与结果关联的父关系。
 * target 是要计算的 PathTarget。
 * mmaggregates 是 MinMaxAggInfo 结构的列表。
 * quals 是 HAVING 限定条件，若有的话。
 */
MinMaxAggPath *
create_minmaxagg_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  PathTarget *target,
					  List *mmaggregates,
					  List *quals)
{
	MinMaxAggPath *pathnode = makeNode(MinMaxAggPath);
	Cost		initplan_cost;
	ListCell   *lc;

	/* 最顶上生成的 Plan 节点将是一个 Result */
	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	/* MinMaxAggPath 意味着使用 initplan，因此无法并行安全 */
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	/* 结果是一行无序的行 */
	pathnode->path.rows = 1;
	pathnode->path.pathkeys = NIL;

	pathnode->mmaggregates = mmaggregates;
	pathnode->quals = quals;

	/* 计算全部 initplan 的代价…… */
	initplan_cost = 0;
	foreach(lc, mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		initplan_cost += mminfo->pathcost;
	}

	/* 为每个输出行加上 tlist 求值代价，再加 cpu_tuple_cost */
	pathnode->path.startup_cost = initplan_cost + target->cost.startup;
	pathnode->path.total_cost = initplan_cost + target->cost.startup +
		target->cost.per_tuple + cpu_tuple_cost;

	/*
	 * 若有限定条件，加上它的代价，但忽略其选择率，因为无论
	 * 限定条件是什么，行数估计都应当是 1。
	 */
	if (quals)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, quals, root);
		pathnode->path.startup_cost += qual_cost.startup;
		pathnode->path.total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	return pathnode;
}

/*
 * create_windowagg_path
 *	  创建一个表示计算窗口函数的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * target 是要计算的 PathTarget。
 * windowFuncs 是 WindowFunc 结构的列表。
 * winclause 是全部 WindowFuncs 共用的 WindowClause。
 * qual 来自更低层 WindowAggPaths 的 WindowClause.runconditions。
 *		当 topwindow == false 时必须始终为 NIL。
 * topwindow 仅对最顶层 WindowAgg 传真。对所有
 *		中间 WindowAggs 传假。
 *
 * 输入必须按 WindowClause 的 PARTITION 键
 * 再加上 ORDER BY 键排序。
 */
WindowAggPath *
create_windowagg_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  Path *subpath,
					  PathTarget *target,
					  List *windowFuncs,
					  WindowClause *winclause,
					  List *qual,
					  bool topwindow)
{
	WindowAggPath *pathnode = makeNode(WindowAggPath);

	/* qual 只能对 topwindow 设置 */
	Assert(qual == NIL || topwindow);

	pathnode->path.pathtype = T_WindowAgg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* WindowAgg 保持输入的排序次序 */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;
	pathnode->winclause = winclause;
	pathnode->qual = qual;
	pathnode->topwindow = topwindow;

	/*
	 * 为了算代价，假定没有冗余的分区
	 * 或排序列；在这里处理那个
	 * 边角情形不值得。因此只把未经修改的列表长度传给
	 * cost_windowagg。
	 */
	cost_windowagg(&pathnode->path, root,
				   windowFuncs,
				   list_length(winclause->partitionClause),
				   list_length(winclause->orderClause),
				   subpath->startup_cost,
				   subpath->total_cost,
				   subpath->rows);

	/* 为每个输出行加上 tlist 求值代价 */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_setop_path
 *	  创建一个表示计算 INTERSECT 或 EXCEPT 的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * cmd 是具体语义（INTERSECT 或 EXCEPT，有或没有 ALL）。
 * strategy 是实现策略（排序或散列）。
 * distinctList 是表示分组的 SortGroupClause 列表。
 * flagColIdx 是标志列所在的列号，若有的话。
 * firstFlag 是散列时第一个输入关系的标志值；
 *		排序时为 -1。
 * numGroups 是不同分组的估计数目。
 * outputRows 是输出行数的估计。
 */
SetOpPath *
create_setop_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  SetOpCmd cmd,
				  SetOpStrategy strategy,
				  List *distinctList,
				  AttrNumber flagColIdx,
				  int firstFlag,
				  double numGroups,
				  double outputRows)
{
	SetOpPath  *pathnode = makeNode(SetOpPath);

	pathnode->path.pathtype = T_SetOp;
	pathnode->path.parent = rel;
	/* SetOp 不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* SetOp 在排序模式下保持输入的排序次序 */
	pathnode->path.pathkeys =
		(strategy == SETOP_SORTED) ? subpath->pathkeys : NIL;

	pathnode->subpath = subpath;
	pathnode->cmd = cmd;
	pathnode->strategy = strategy;
	pathnode->distinctList = distinctList;
	pathnode->flagColIdx = flagColIdx;
	pathnode->firstFlag = firstFlag;
	pathnode->numGroups = numGroups;

	/*
	 * 每个输入元组的每次比较收取一个 cpu_operator_cost。我们假定
	 * 大多数元组都会比较全部列。
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_operator_cost * subpath->rows * list_length(distinctList);
	pathnode->path.rows = outputRows;

	return pathnode;
}

/*
 * create_recursiveunion_path
 *	  创建一个表示递归 UNION 节点的路径节点。
 *
 * rel 是与结果关联的父关系。
 * leftpath 是非递归项的数据来源。
 * rightpath 是递归项的数据来源。
 * target 是要计算的 PathTarget。
 * distinctList 是表示分组的 SortGroupClause 列表。
 * wtParam 是表示工作表的 Param 的 ID。
 * numGroups 是估计的分组数。
 *
 * 对递归 UNION ALL，distinctList 为空且 numGroups 为零。
 */
RecursiveUnionPath *
create_recursiveunion_path(PlannerInfo *root,
						   RelOptInfo *rel,
						   Path *leftpath,
						   Path *rightpath,
						   PathTarget *target,
						   List *distinctList,
						   int wtParam,
						   double numGroups)
{
	RecursiveUnionPath *pathnode = makeNode(RecursiveUnionPath);

	pathnode->path.pathtype = T_RecursiveUnion;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		leftpath->parallel_safe && rightpath->parallel_safe;
	/* 很笨，但目前先做得和连接一样： */
	pathnode->path.parallel_workers = leftpath->parallel_workers;
	/* RecursiveUnion 的结果总是未排序 */
	pathnode->path.pathkeys = NIL;

	pathnode->leftpath = leftpath;
	pathnode->rightpath = rightpath;
	pathnode->distinctList = distinctList;
	pathnode->wtParam = wtParam;
	pathnode->numGroups = numGroups;

	cost_recursive_union(&pathnode->path, leftpath, rightpath);

	return pathnode;
}

/*
 * create_lockrows_path
 *	  创建一个表示获取行锁的路径节点。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * rowMarks 是 PlanRowMark 的列表。
 * epqParam 是供 EvalPlanQual 重新求值的 Param 的 ID。
 */
LockRowsPath *
create_lockrows_path(PlannerInfo *root, RelOptInfo *rel,
					 Path *subpath, List *rowMarks, int epqParam)
{
	LockRowsPath *pathnode = makeNode(LockRowsPath);

	pathnode->path.pathtype = T_LockRows;
	pathnode->path.parent = rel;
	/* LockRows 不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = subpath->rows;

	/*
	 * 无法假定结果已排序，因为加锁可能导致排序
	 * 键列被换成新值。
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;
	pathnode->rowMarks = rowMarks;
	pathnode->epqParam = epqParam;

	/*
	 * 行锁定和可能的重新取行应当额外收点费，
	 * 但很难说收多少。目前每行用
	 * cpu_tuple_cost。
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_tuple_cost * subpath->rows;

	return pathnode;
}

/*
 * create_modifytable_path
 *	  创建一个路径节点，表示执行 INSERT/UPDATE/DELETE/MERGE
 *	  修改。
 *
 * rel 是与结果关联的父关系。
 * subpath 是产生源数据的 Path。
 * operation 是操作类型。
 * canSetTag 为真表示我们设置命令标签和 es_processed。
 * nominalRelation 是供 EXPLAIN 使用的父 RT 下标。
 * rootRelation 是分区表或继承表根的 RTI，没有则为 0。
 * partColsUpdated 为真表示正在更新任何分区列，
 *		无论来自目标关系还是后代分区表。
 * resultRelations 是目标关系实际 RT 下标的整数列表。
 * updateColnosLists 是 UPDATE 目标列号列表的列表
 *		（每个关系一个子列表）；若不是 UPDATE 则为 NIL。
 * withCheckOptionLists 是 WCO 列表的列表（每个关系一个）。
 * returningLists 是 RETURNING 目标列表的列表（每个关系一个）。
 * rowMarks 是 PlanRowMarks 列表（仅非锁定）。
 * onconflict 是 ON CONFLICT 子句，或为 NULL。
 * epqParam 是 EvalPlanQual 重新求值所用 Param 的 ID。
 * mergeActionLists 是 MERGE 动作列表的列表（每个关系一个）。
 */
ModifyTablePath *
create_modifytable_path(PlannerInfo *root, RelOptInfo *rel,
						Path *subpath,
						CmdType operation, bool canSetTag,
						Index nominalRelation, Index rootRelation,
						bool partColsUpdated,
						List *resultRelations,
						List *updateColnosLists,
						List *withCheckOptionLists, List *returningLists,
						List *rowMarks, OnConflictExpr *onconflict,
						List *mergeActionLists, int epqParam)
{
	ModifyTablePath *pathnode = makeNode(ModifyTablePath);

	Assert(operation == CMD_MERGE ||
		   (operation == CMD_UPDATE ?
			list_length(resultRelations) == list_length(updateColnosLists) :
			updateColnosLists == NIL));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	pathnode->path.pathtype = T_ModifyTable;
	pathnode->path.parent = rel;
	/* pathtarget 没意思，只把它做成最低限度有效 */
	pathnode->path.pathtarget = rel->reltarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;

	/*
	 * 若有 RETURNING，把代价和行数算成子路径的代价和行数。
	 *
	 * 目前，实际的表修改工作，以及 WITH CHECK OPTIONS 或 RETURNING
	 * 表达式（若有），我们都不额外收费。那只会是装点门面，因为
	 * ModifyTable 总是顶层节点，这些代价没有办法
	 * 改变任何更高层的规划选择。但也许哪天
	 * 我们会想让它看起来
	 * 更好看。
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost;
	if (returningLists != NIL)
	{
		pathnode->path.rows = subpath->rows;

		/*
		 * 把宽度设成与子路径输出一致。XXX 这完全是错的：
		 * 应当返回 RETURNING 目标列表宽度的平均值。但
		 * 历史上就是这样，改进它是
		 * 以后的事。（同样，这多半是装点门面。）
		 */
		pathnode->path.pathtarget->width = subpath->pathtarget->width;
	}
	else
	{
		pathnode->path.rows = 0;
		pathnode->path.pathtarget->width = 0;
	}

	pathnode->subpath = subpath;
	pathnode->operation = operation;
	pathnode->canSetTag = canSetTag;
	pathnode->nominalRelation = nominalRelation;
	pathnode->rootRelation = rootRelation;
	pathnode->partColsUpdated = partColsUpdated;
	pathnode->resultRelations = resultRelations;
	pathnode->updateColnosLists = updateColnosLists;
	pathnode->withCheckOptionLists = withCheckOptionLists;
	pathnode->returningLists = returningLists;
	pathnode->rowMarks = rowMarks;
	pathnode->onconflict = onconflict;
	pathnode->epqParam = epqParam;
	pathnode->mergeActionLists = mergeActionLists;

	return pathnode;
}

/*
 * create_limit_path
 *	  创建一个表示执行 LIMIT/OFFSET 的路径节点。
 *
 * 除了提供实际的 OFFSET 和 LIMIT 表达式，
 * 调用方还必须提供它们的值估计，供算代价用。
 * 估计值按 preprocess_limit() 所算的那样，也就是 0 表示
 * 该子句不存在，-1 表示存在但我们
 * 估计不出它的值。
 *
 * rel 是与结果关联的父关系。
 * subpath 是表示数据来源的路径。
 * limitOffset 是实际的 OFFSET 表达式，或为 NULL。
 * limitCount 是实际的 LIMIT 表达式，或为 NULL。
 * offset_est 是 OFFSET 表达式的估计值。
 * count_est 是 LIMIT 表达式的估计值。
 */
LimitPath *
create_limit_path(PlannerInfo *root, RelOptInfo *rel,
				  Path *subpath,
				  Node *limitOffset, Node *limitCount,
				  LimitOption limitOption,
				  int64 offset_est, int64 count_est)
{
	LimitPath  *pathnode = makeNode(LimitPath);

	pathnode->path.pathtype = T_Limit;
	pathnode->path.parent = rel;
	/* Limit 不投影，因此用源路径的 pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* 目前假定我们在任何连接之上，因此没有参数化 */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.rows = subpath->rows;
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost;
	pathnode->path.pathkeys = subpath->pathkeys;
	pathnode->subpath = subpath;
	pathnode->limitOffset = limitOffset;
	pathnode->limitCount = limitCount;
	pathnode->limitOption = limitOption;

	/*
	 * 按偏移和限制调整输出行数和代价。
	 */
	adjust_limit_rows_costs(&pathnode->path.rows,
							&pathnode->path.startup_cost,
							&pathnode->path.total_cost,
							offset_est, count_est);

	return pathnode;
}

/*
 * adjust_limit_rows_costs
 *	  按偏移和限制，调整 LimitPath 节点的大小
 *	  与代价估计。
 *
 * 若处于顶层，这只是观感问题；但若正在
 * 构造子查询，就必须把正确信息报告给外层
 * 规划器。
 *
 * 当偏移或计数估计不出来时，就用子路径所估计
 * 发出行数的 10%。
 *
 * XXX 我们并没有把偏移和限制表达式本身的求值代价
 * 加到路径代价上。理论上应当加上，但多数情况下那些
 * 表达式很简单，不值得费这个事。
 */
void
adjust_limit_rows_costs(double *rows,	/* 输入输出参数 */
						Cost *startup_cost, /* 输入输出参数 */
						Cost *total_cost,	/* 输入输出参数 */
						int64 offset_est,
						int64 count_est)
{
	double		input_rows = *rows;
	Cost		input_startup_cost = *startup_cost;
	Cost		input_total_cost = *total_cost;

	if (offset_est != 0)
	{
		double		offset_rows;

		if (offset_est > 0)
			offset_rows = (double) offset_est;
		else
			offset_rows = clamp_row_est(input_rows * 0.10);
		if (offset_rows > *rows)
			offset_rows = *rows;
		if (input_rows > 0)
			*startup_cost +=
				(input_total_cost - input_startup_cost)
				* offset_rows / input_rows;
		*rows -= offset_rows;
		if (*rows < 1)
			*rows = 1;
	}

	if (count_est != 0)
	{
		double		count_rows;

		if (count_est > 0)
			count_rows = (double) count_est;
		else
			count_rows = clamp_row_est(input_rows * 0.10);
		if (count_rows > *rows)
			count_rows = *rows;
		if (input_rows > 0)
			*total_cost = *startup_cost +
				(input_total_cost - input_startup_cost)
				* count_rows / input_rows;
		*rows = count_rows;
		if (*rows < 1)
			*rows = 1;
	}
}


/*
 * reparameterize_path
 *		试图修改一条 Path，使它的参数化更大。
 *
 * 我们用它试图把 appendrel 的所有子路径带到
 * 同一参数化水平，确保它们强制同一组
 * 连接限定条件（从而可以把该参数化归于
 * 由这些路径建成的 append 路径）。目前这里只
 * 支持少数路径类型，需要时还可以再加。若没法
 * 重新参数化给定路径，就返回 NULL。
 *
 * 注意：我们有意不把创建出的路径传给 add_path()；它
 * 可能会以代价劣于来源路径为由试图删掉它们，
 * 而我们不想那样。这里做出的路径不一定
 * 有普遍用途，但作为 append 路径的成员
 * 可以有用。
 */
Path *
reparameterize_path(PlannerInfo *root, Path *path,
					Relids required_outer,
					double loop_count)
{
	RelOptInfo *rel = path->parent;

	/* 只能增加路径的参数化，不能减少 */
	if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
		return NULL;
	switch (path->pathtype)
	{
		case T_SeqScan:
			return create_seqscan_path(root, rel, required_outer, 0);
		case T_SampleScan:
			return (Path *) create_samplescan_path(root, rel, required_outer);
		case T_IndexScan:
		case T_IndexOnlyScan:
			{
				IndexPath  *ipath = (IndexPath *) path;
				IndexPath  *newpath = makeNode(IndexPath);

				/*
				 * 我们没法直接用 create_index_path，而且也不想
				 * 用，因为它会重新计算 indexqual 条件，
				 * 那是白费力气。改为稍微权宜一下：
				 * 平铺复制路径节点，改掉它的 param_info，并重做
				 * 代价估计。
				 */
				memcpy(newpath, ipath, sizeof(IndexPath));
				newpath->path.param_info =
					get_baserel_parampathinfo(root, rel, required_outer);
				cost_index(newpath, root, loop_count, false);
				return (Path *) newpath;
			}
		case T_BitmapHeapScan:
			{
				BitmapHeapPath *bpath = (BitmapHeapPath *) path;

				return (Path *) create_bitmap_heap_path(root,
														rel,
														bpath->bitmapqual,
														required_outer,
														loop_count, 0);
			}
		case T_SubqueryScan:
			{
				SubqueryScanPath *spath = (SubqueryScanPath *) path;
				Path	   *subpath = spath->subpath;
				bool		trivial_pathtarget;

				/*
				 * 若现有节点的额外代价为零，我们一定已经判定
				 * 它的目标是平凡的。（反过来不成立，因为它
				 * 也许目标平凡但有限定条件要强制；但那种情况下
				 * 新节点也一样，所以这里是否答对
				 * 并不要紧。）
				 */
				trivial_pathtarget =
					(subpath->total_cost == spath->path.total_cost);

				return (Path *) create_subqueryscan_path(root,
														 rel,
														 subpath,
														 trivial_pathtarget,
														 spath->path.pathkeys,
														 required_outer);
			}
		case T_Result:
			/* 仅支持 RTE_RESULT 扫描路径 */
			if (IsA(path, Path))
				return create_resultscan_path(root, rel, required_outer);
			break;
		case T_Append:
			{
				AppendPath *apath = (AppendPath *) path;
				List	   *childpaths = NIL;
				List	   *partialpaths = NIL;
				int			i;
				ListCell   *lc;

				/* 重新参数化各个子路径 */
				i = 0;
				foreach(lc, apath->subpaths)
				{
					Path	   *spath = (Path *) lfirst(lc);

					spath = reparameterize_path(root, spath,
												required_outer,
												loop_count);
					if (spath == NULL)
						return NULL;
					/* 必须重新拆分普通路径和部分路径 */
					if (i < apath->first_partial_path)
						childpaths = lappend(childpaths, spath);
					else
						partialpaths = lappend(partialpaths, spath);
					i++;
				}
				return (Path *)
					create_append_path(root, rel, childpaths, partialpaths,
									   apath->path.pathkeys, required_outer,
									   apath->path.parallel_workers,
									   apath->path.parallel_aware,
									   -1);
			}
		case T_Material:
			{
				MaterialPath *mpath = (MaterialPath *) path;
				Path	   *spath = mpath->subpath;

				spath = reparameterize_path(root, spath,
											required_outer,
											loop_count);
				if (spath == NULL)
					return NULL;
				return (Path *) create_material_path(rel, spath);
			}
		case T_Memoize:
			{
				MemoizePath *mpath = (MemoizePath *) path;
				Path	   *spath = mpath->subpath;

				spath = reparameterize_path(root, spath,
											required_outer,
											loop_count);
				if (spath == NULL)
					return NULL;
				return (Path *) create_memoize_path(root, rel,
													spath,
													mpath->param_exprs,
													mpath->hash_operators,
													mpath->singlerow,
													mpath->binary_mode,
													mpath->calls);
			}
		default:
			break;
	}
	return NULL;
}

/*
 * reparameterize_path_by_child
 * 		给定一条由其父关系参数化的路径（父关系属于给定子关系），
 * 		把该路径翻译成由给定子关系来参数化。
 *
 * 本函数创建与给定路径同类型的新路径，但改由
 * 给定子关系参数化。原路径的大多数成员
 * 可以平铺复制，但任何表达式都要调整，使之
 * 引用正确的 varno，并且任何路径都要递归地
 * 重新参数化。引用特定 relid 的其他成员
 * 也要调整。
 *
 * 代价、行数、宽度和并行路径属性取决于
 * path->parent，翻译过程中它并不改变。因此这些
 * 成员按原样复制。
 *
 * 目前这里只支持少数几种路径类型，需要时还可以再加。
 * 若没法重新参数化给定路径，就返回 NULL。
 */
Path *
reparameterize_path_by_child(PlannerInfo *root, Path *path,
							 RelOptInfo *child_rel)
{

#define FLAT_COPY_PATH(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define ADJUST_CHILD_ATTRS(node) \
	((node) = \
	 (List *) adjust_appendrel_attrs_multilevel(root, (Node *) (node), \
												child_rel, \
												child_rel->top_parent))

#define REPARAMETERIZE_CHILD_PATH(path) \
do { \
	(path) = reparameterize_path_by_child(root, (path), child_rel); \
	if ((path) == NULL) \
		return NULL; \
} while(0)

#define REPARAMETERIZE_CHILD_PATH_LIST(pathlist) \
do { \
	if ((pathlist) != NIL) \
	{ \
		(pathlist) = reparameterize_pathlist_by_child(root, (pathlist), \
													  child_rel); \
		if ((pathlist) == NIL) \
			return NULL; \
	} \
} while(0)

	Path	   *new_path;
	ParamPathInfo *new_ppi;
	ParamPathInfo *old_ppi;
	Relids		required_outer;

	/*
	 * 若路径不是由给定关系的父关系参数化的，它
	 * 就不需要重新参数化。
	 */
	if (!path->param_info ||
		!bms_overlap(PATH_REQ_OUTER(path), child_rel->top_parent_relids))
		return path;

	/*
	 * 若可能，就重新参数化给定路径，并做一份副本。
	 *
	 * 本函数目前只用于按分区连接代码正在分区的嵌套循环
	 * 连接的内侧。因此，
	 * 我们只需支持在那种上下文里合理出现的路径类型。
	 * （尤其是，支持已排序的路径类型是浪费代码
	 * 和周期：即使在这里翻译了它们，随后的代价比较里
	 * 它们也会输掉。）若确实看到不支持的路径类型，
	 * 那只是说我们无法用那种路径类型生成
	 * 按分区连接的计划。
	 */
	switch (nodeTag(path))
	{
		case T_Path:

			/*
			 * 若路径的限制子句含有对另一个关系的 LATERAL 引用，
			 * 就没法重新参数化，因为不得在这里
			 * 改动 RelOptInfo 的内容。（那样做会
			 * 在最终不用按分区连接时把事情弄坏。）
			 */
			if (ris_contain_references_to(root,
										  path->parent->baserestrictinfo,
										  child_rel->top_parent_relids))
				return NULL;

			/*
			 * 若它是带有引用另一个关系的 tablesample 参数的 SampleScan，
			 * 就没法重新参数化，因为不得在这里
			 * 改动 RTE 的内容。（那样做会在最终不用
			 * 按分区连接时把事情弄坏。）
			 */
			if (path->pathtype == T_SampleScan)
			{
				Index		scan_relid = path->parent->relid;
				RangeTblEntry *rte;

				/* 它应当是带有 tablesample 子句的基关系…… */
				Assert(scan_relid > 0);
				rte = planner_rt_fetch(scan_relid, root);
				Assert(rte->rtekind == RTE_RELATION);
				Assert(rte->tablesample != NULL);

				if (contain_references_to(root, (Node *) rte->tablesample,
										  child_rel->top_parent_relids))
					return NULL;
			}

			FLAT_COPY_PATH(new_path, path, Path);
			break;

		case T_IndexPath:
			{
				IndexPath  *ipath;

				/*
				 * 若路径的限制子句含有对另一个
				 * 关系的 LATERAL 引用，就没法重新参数化，
				 * 因为不得在这里改动 IndexOptInfo 的内容。
				 * （那样做会在最终不用按分区
				 * 连接时把事情弄坏。）
				 */
				if (ris_contain_references_to(root,
											  path->parent->baserestrictinfo,
											  child_rel->top_parent_relids))
					return NULL;

				FLAT_COPY_PATH(ipath, path, IndexPath);
				ADJUST_CHILD_ATTRS(ipath->indexclauses);
				new_path = (Path *) ipath;
			}
			break;

		case T_BitmapHeapPath:
			{
				BitmapHeapPath *bhpath;

				/*
				 * 若路径的限制子句含有对另一个
				 * 关系的 LATERAL 引用，就没法重新参数化，
				 * 因为不得在这里改动 RelOptInfo 的内容。
				 * （那样做会在最终不用按分区
				 * 连接时把事情弄坏。）
				 */
				if (ris_contain_references_to(root,
											  path->parent->baserestrictinfo,
											  child_rel->top_parent_relids))
					return NULL;

				FLAT_COPY_PATH(bhpath, path, BitmapHeapPath);
				REPARAMETERIZE_CHILD_PATH(bhpath->bitmapqual);
				new_path = (Path *) bhpath;
			}
			break;

		case T_BitmapAndPath:
			{
				BitmapAndPath *bapath;

				FLAT_COPY_PATH(bapath, path, BitmapAndPath);
				REPARAMETERIZE_CHILD_PATH_LIST(bapath->bitmapquals);
				new_path = (Path *) bapath;
			}
			break;

		case T_BitmapOrPath:
			{
				BitmapOrPath *bopath;

				FLAT_COPY_PATH(bopath, path, BitmapOrPath);
				REPARAMETERIZE_CHILD_PATH_LIST(bopath->bitmapquals);
				new_path = (Path *) bopath;
			}
			break;

		case T_ForeignPath:
			{
				ForeignPath *fpath;
				ReparameterizeForeignPathByChild_function rfpc_func;

				/*
				 * 若路径的限制子句含有对另一个
				 * 关系的 LATERAL 引用，就没法重新参数化，
				 * 因为不得在这里改动 RelOptInfo 的内容。
				 * （那样做会在最终不用按分区
				 * 连接时把事情弄坏。）
				 */
				if (ris_contain_references_to(root,
											  path->parent->baserestrictinfo,
											  child_rel->top_parent_relids))
					return NULL;

				FLAT_COPY_PATH(fpath, path, ForeignPath);
				if (fpath->fdw_outerpath)
					REPARAMETERIZE_CHILD_PATH(fpath->fdw_outerpath);

				/* 若需要，就交给 FDW。 */
				rfpc_func =
					path->parent->fdwroutine->ReparameterizeForeignPathByChild;
				if (rfpc_func)
					fpath->fdw_private = rfpc_func(root, fpath->fdw_private,
												   child_rel);
				new_path = (Path *) fpath;
			}
			break;

		case T_CustomPath:
			{
				CustomPath *cpath;

				/*
				 * 若路径的限制子句含有对另一个
				 * 关系的 LATERAL 引用，就没法重新参数化，
				 * 因为不得在这里改动 RelOptInfo 的内容。
				 * （那样做会在最终不用按分区
				 * 连接时把事情弄坏。）
				 */
				if (ris_contain_references_to(root,
											  path->parent->baserestrictinfo,
											  child_rel->top_parent_relids))
					return NULL;

				FLAT_COPY_PATH(cpath, path, CustomPath);
				REPARAMETERIZE_CHILD_PATH_LIST(cpath->custom_paths);
				if (cpath->methods &&
					cpath->methods->ReparameterizeCustomPathByChild)
					cpath->custom_private =
						cpath->methods->ReparameterizeCustomPathByChild(root,
																		cpath->custom_private,
																		child_rel);
				new_path = (Path *) cpath;
			}
			break;

		case T_NestPath:
			{
				JoinPath   *jpath;
				NestPath   *npath;

				FLAT_COPY_PATH(npath, path, NestPath);

				jpath = (JoinPath *) npath;
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				new_path = (Path *) npath;
			}
			break;

		case T_MergePath:
			{
				JoinPath   *jpath;
				MergePath  *mpath;

				FLAT_COPY_PATH(mpath, path, MergePath);

				jpath = (JoinPath *) mpath;
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				ADJUST_CHILD_ATTRS(mpath->path_mergeclauses);
				new_path = (Path *) mpath;
			}
			break;

		case T_HashPath:
			{
				JoinPath   *jpath;
				HashPath   *hpath;

				FLAT_COPY_PATH(hpath, path, HashPath);

				jpath = (JoinPath *) hpath;
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				ADJUST_CHILD_ATTRS(hpath->path_hashclauses);
				new_path = (Path *) hpath;
			}
			break;

		case T_AppendPath:
			{
				AppendPath *apath;

				FLAT_COPY_PATH(apath, path, AppendPath);
				REPARAMETERIZE_CHILD_PATH_LIST(apath->subpaths);
				new_path = (Path *) apath;
			}
			break;

		case T_MaterialPath:
			{
				MaterialPath *mpath;

				FLAT_COPY_PATH(mpath, path, MaterialPath);
				REPARAMETERIZE_CHILD_PATH(mpath->subpath);
				new_path = (Path *) mpath;
			}
			break;

		case T_MemoizePath:
			{
				MemoizePath *mpath;

				FLAT_COPY_PATH(mpath, path, MemoizePath);
				REPARAMETERIZE_CHILD_PATH(mpath->subpath);
				ADJUST_CHILD_ATTRS(mpath->param_exprs);
				new_path = (Path *) mpath;
			}
			break;

		case T_GatherPath:
			{
				GatherPath *gpath;

				FLAT_COPY_PATH(gpath, path, GatherPath);
				REPARAMETERIZE_CHILD_PATH(gpath->subpath);
				new_path = (Path *) gpath;
			}
			break;

		default:

			/* 我们不知道如何重新参数化这条路径。 */
			return NULL;
	}

	/*
	 * 调整指向最顶层父关系的参数化信息。最顶层
	 * 父关系可能离给定子关系有好几层，因此使用多层
	 * 表达式调整例程。
	 */
	old_ppi = new_path->param_info;
	required_outer =
		adjust_child_relids_multilevel(root, old_ppi->ppi_req_outer,
									   child_rel,
									   child_rel->top_parent);

	/* 若这种参数化已经有 PPI，直接返回它 */
	new_ppi = find_param_path_info(new_path->parent, required_outer);

	/*
	 * 若没有，就建一个新的，并链到 PPIs 列表上。出于与
	 * mark_dummy_rel() 里所解释的相同理由，在给定
	 * RelOptInfo 所在的同一个上下文里分配新的 PPI。
	 */
	if (new_ppi == NULL)
	{
		MemoryContext oldcontext;
		RelOptInfo *rel = path->parent;

		oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

		new_ppi = makeNode(ParamPathInfo);
		new_ppi->ppi_req_outer = bms_copy(required_outer);
		new_ppi->ppi_rows = old_ppi->ppi_rows;
		new_ppi->ppi_clauses = old_ppi->ppi_clauses;
		ADJUST_CHILD_ATTRS(new_ppi->ppi_clauses);
		new_ppi->ppi_serials = bms_copy(old_ppi->ppi_serials);
		rel->ppilist = lappend(rel->ppilist, new_ppi);

		MemoryContextSwitchTo(oldcontext);
	}
	bms_free(required_outer);

	new_path->param_info = new_ppi;

	/*
	 * 若外侧关系的父关系在目标列表里被引用，就调整路径目标。
	 * 当这个关系里只 LATERAL 引用了外侧关系的父关系时，
	 * 就会发生这种情况。
	 */
	if (bms_overlap(path->parent->lateral_relids,
					child_rel->top_parent_relids))
	{
		new_path->pathtarget = copy_pathtarget(new_path->pathtarget);
		ADJUST_CHILD_ATTRS(new_path->pathtarget->exprs);
	}

	return new_path;
}

/*
 * reparameterize_pathlist_by_child
 * 		按给定子关系重新参数化路径列表的辅助函数。
 */
static List *
reparameterize_pathlist_by_child(PlannerInfo *root,
								 List *pathlist,
								 RelOptInfo *child_rel)
{
	ListCell   *lc;
	List	   *result = NIL;

	foreach(lc, pathlist)
	{
		Path	   *path = reparameterize_path_by_child(root, lfirst(lc),
														child_rel);

		if (path == NULL)
		{
			list_free(result);
			return NIL;
		}

		result = lappend(result, path);
	}

	return result;
}

/*
 * contain_references_to
 *		检测给定子句里是否有任何 Var 或 PlaceHolderVars 含有
 *		对给定 relids 的 LATERAL 引用。
 */
static bool
contain_references_to(PlannerInfo *root, Node *clause, Relids relids)
{
	bool		ret = false;
	List	   *vars;
	ListCell   *lc;

	/*
	 * 检查该子句里用到的全部 Var 和 PlaceHolderVars。
	 *
	 * 省掉相关标志后，这也顺便廉价地检查
	 * 子句里没有聚集或窗口函数。扫描级限制或 tablesample 里
	 * 我们不预期会出现那些。
	 */
	vars = pull_var_clause(clause, PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;

			if (bms_is_member(var->varno, relids))
			{
				ret = true;
				break;
			}
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			/*
			 * 应当同时检查 ph_eval_at（以防 PHV 要在
			 * 另一个关系上计算，然后再 LATERAL 引用到
			 * 这里）和 ph_lateral（以防 PHV 要在这里求值，
			 * 但含有对另一个关系的 LATERAL 引用）。前一种
			 * 情形不该出现在 baserestrictinfo 子句里，但
			 * 可以出现在 tablesample 子句里。
			 */
			if (bms_overlap(phinfo->ph_eval_at, relids) ||
				bms_overlap(phinfo->ph_lateral, relids))
			{
				ret = true;
				break;
			}
		}
		else
			Assert(false);
	}

	list_free(vars);

	return ret;
}

/*
 * ris_contain_references_to
 *		对 RestrictInfos 列表应用 contain_references_to()。
 *
 * 这里需要额外代码，因为 pull_var_clause() 无法下降
 * 穿过 RestrictInfos。
 */
static bool
ris_contain_references_to(PlannerInfo *root, List *rinfos, Relids relids)
{
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		/* 伪常量子句里不可能含有任何 Var 或 PHVs */
		if (rinfo->pseudoconstant)
			continue;
		if (contain_references_to(root, (Node *) rinfo->clause, relids))
			return true;
	}
	return false;
}
