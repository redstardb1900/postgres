/*-------------------------------------------------------------------------
 *
 * joinpath.c
 *	  Routines to find all possible paths for processing a set of joins
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/joinpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/typcache.h"

/* 供插件在 add_paths_to_joinrel() 中取得控制权的钩子 */
set_join_pathlist_hook_type set_join_pathlist_hook = NULL;

/*
 * 由父项参数化的路径可以视为由
 * 它的任何一个子项参数化。
 */
#define PATH_PARAM_BY_PARENT(path, rel)	\
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path),	\
									   (rel)->top_parent_relids))
#define PATH_PARAM_BY_REL_SELF(path, rel)  \
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path), (rel)->relids))

#define PATH_PARAM_BY_REL(path, rel)	\
	(PATH_PARAM_BY_REL_SELF(path, rel) || PATH_PARAM_BY_PARENT(path, rel))

static void try_partial_mergejoin_path(PlannerInfo *root,
									   RelOptInfo *joinrel,
									   Path *outer_path,
									   Path *inner_path,
									   List *pathkeys,
									   List *mergeclauses,
									   List *outersortkeys,
									   List *innersortkeys,
									   JoinType jointype,
									   JoinPathExtraData *extra);
static void sort_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static inline bool clause_sides_match_join(RestrictInfo *rinfo,
										   RelOptInfo *outerrel,
										   RelOptInfo *innerrel);
static void match_unsorted_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static void consider_parallel_nestloop(PlannerInfo *root,
									   RelOptInfo *joinrel,
									   RelOptInfo *outerrel,
									   RelOptInfo *innerrel,
									   JoinType jointype,
									   JoinPathExtraData *extra);
static void consider_parallel_mergejoin(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outerrel,
										RelOptInfo *innerrel,
										JoinType jointype,
										JoinPathExtraData *extra,
										Path *inner_cheapest_total);
static void hash_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static List *select_mergejoin_clauses(PlannerInfo *root,
									  RelOptInfo *joinrel,
									  RelOptInfo *outerrel,
									  RelOptInfo *innerrel,
									  List *restrictlist,
									  JoinType jointype,
									  bool *mergejoin_allowed);
static void generate_mergejoin_paths(PlannerInfo *root,
									 RelOptInfo *joinrel,
									 RelOptInfo *innerrel,
									 Path *outerpath,
									 JoinType jointype,
									 JoinPathExtraData *extra,
									 bool useallclauses,
									 Path *inner_cheapest_total,
									 List *merge_pathkeys,
									 bool is_partial);


/*
 * add_paths_to_joinrel
 *	  给定一个连接关系，以及可以构成它的两个成员关系，
 *	  考虑把这两个成员关系分别当作外层
 *	  和内层的全部可能路径。若这些路径在与其他路径比较后仍留下，
 *	  就把它们加入连接关系的 pathlist（并去掉任何被这些路径
 *	  支配的已有路径）。
 *
 * 修改 joinrel 节点的 pathlist 字段，使之包含迄今找到的
 * 最佳路径。
 *
 * jointype 不一定与 sjinfo->jointype 相同；若考虑按与 sjinfo
 * 所示相反的方向连接这些关系，它也许被
 * "翻转"。
 *
 * 另外，本例程和本模块的其他例程接受特殊 JoinTypes
 * JOIN_UNIQUE_OUTER 和 JOIN_UNIQUE_INNER，表示应当
 * 对外层或内层关系做唯一化，然后做普通内
 * 连接。不过这些值不得传播到本模块之外。
 * 路径代价估计代码也许需要认出它正在
 * 处理这种情形 --- 名义 jointype 为 INNER
 * 且 sjinfo->jointype == JOIN_SEMI 就表示如此。
 */
void
add_paths_to_joinrel(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 SpecialJoinInfo *sjinfo,
					 List *restrictlist)
{
	JoinPathExtraData extra;
	bool		mergejoin_allowed = true;
	bool		consider_join_pushdown = false;
	ListCell   *lc;
	Relids		joinrelids;

	/*
	 * PlannerInfo 并不包含为子关系之间的连接创建的 SpecialJoinInfos，
	 * 即使最顶层父项之间的连接有 SpecialJoinInfo 节点。
	 * 因此在计算表示限制的 Relids 集合时，
	 * 要考虑分区的最顶层父项的
	 * relid。
	 */
	if (joinrel->reloptkind == RELOPT_OTHER_JOINREL)
		joinrelids = joinrel->top_parent_relids;
	else
		joinrelids = joinrel->relids;

	extra.restrictlist = restrictlist;
	extra.mergeclause_list = NIL;
	extra.sjinfo = sjinfo;
	extra.param_source_rels = NULL;

	/*
	 * 看内层关系对这个外层关系是否可证明唯一。
	 *
	 * 有些特殊情形：对 JOIN_SEMI 和 JOIN_ANTI，这无关紧要，
	 * 因为执行器反正能做等价的优化；
	 * 不必在证明上花费规划器周期。对 JOIN_UNIQUE_INNER，我们
	 * 考虑的必定是内层不能证明唯一的半连接
	 * （否则 reduce_unique_semijoins 已经把它简化了），因此
	 * 调用 innerrel_is_unique 没有意义。不过，若 LHS 覆盖了
	 * 该半连接的全部 min_lefthand，就应当设置 inner_unique，
	 * 因为 create_unique_path 产生的路径相对
	 * LHS 是唯一的。（若 LHS 只是 min_lefthand 的一部分，
	 * 则 *并非* 如此。）对 JOIN_UNIQUE_OUTER，传入 JOIN_INNER，以免
	 * 让那个值逃出本模块。
	 */
	switch (jointype)
	{
		case JOIN_SEMI:
		case JOIN_ANTI:

			/*
			 * XXX 也许值得证明这一点，以便考虑对嵌套循环
			 * 半连接/反连接使用 Memoize。
			 */
			extra.inner_unique = false; /* 好吧，尚未证明 */
			break;
		case JOIN_UNIQUE_INNER:
			extra.inner_unique = bms_is_subset(sjinfo->min_lefthand,
											   outerrel->relids);
			break;
		case JOIN_UNIQUE_OUTER:
			extra.inner_unique = innerrel_is_unique(root,
													joinrel->relids,
													outerrel->relids,
													innerrel,
													JOIN_INNER,
													restrictlist,
													false);
			break;
		default:
			extra.inner_unique = innerrel_is_unique(root,
													joinrel->relids,
													outerrel->relids,
													innerrel,
													jointype,
													restrictlist,
													false);
			break;
	}

	/*
	 * 找出可能的归并连接子句。若我们不打算做归并连接，可以跳过。
	 * 不过，归并连接也许是实现全外连接的唯一办法，
	 * 因此若是全连接，
	 * 就覆盖 enable_mergejoin。
	 */
	if (enable_mergejoin || jointype == JOIN_FULL)
		extra.mergeclause_list = select_mergejoin_clauses(root,
														  joinrel,
														  outerrel,
														  innerrel,
														  restrictlist,
														  jointype,
														  &mergejoin_allowed);

	/*
	 * 若是 SEMI、ANTI 或 inner_unique 连接，就计算代价估计用的修正因子。
	 * 对所有路径这些因子都相同。
	 */
	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI || extra.inner_unique)
		compute_semi_anti_join_factors(root, joinrel, outerrel, innerrel,
									   jointype, sjinfo, restrictlist,
									   &extra.semifactors);

	/*
	 * 决定为这次连接生成参数化路径是否合理，
	 * 若合理，这些路径应当需要哪些关系。通常
	 * 不必创建参数化的结果路径，除非
	 * 有连接顺序限制，使某个输入关系不能
	 * 直接连到参数来源关系，而只能连到另一个
	 * 输入关系。（但见 allow_star_schema_join()。）这一限制
	 * 减少更高连接层要处理的参数化路径数量，
	 * 又不降低所得
	 * 计划的质量。我们把该限制表达为一个 Relids 集合，它必须与
	 * 任何提议连接路径的参数化重叠。注意：param_source_rels
	 * 应当只含基关系，不含外连接 relid，因此从
	 * all_baserels 而不是 all_query_rels 出发才对。
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo2 = (SpecialJoinInfo *) lfirst(lc);

		/*
		 * 若我们拥有 SJ 的一部分 RHS（也许不是全部），
		 * 且尚未连到它的 LHS，则该 SJ 与这次连接相关。
		 * （这个测试相当简陋，但连接已被证明合法，应当够用。）
		 * 若 SJ 相关，它就对连到其 RHS 之外的
		 * 任何东西施加约束。
		 */
		if (bms_overlap(joinrelids, sjinfo2->min_righthand) &&
			!bms_overlap(joinrelids, sjinfo2->min_lefthand))
			extra.param_source_rels = bms_join(extra.param_source_rels,
											   bms_difference(root->all_baserels,
															  sjinfo2->min_righthand));

		/* 全连接对两侧对称地施加约束 */
		if (sjinfo2->jointype == JOIN_FULL &&
			bms_overlap(joinrelids, sjinfo2->min_lefthand) &&
			!bms_overlap(joinrelids, sjinfo2->min_righthand))
			extra.param_source_rels = bms_join(extra.param_source_rels,
											   bms_difference(root->all_baserels,
															  sjinfo2->min_lefthand));
	}

	/*
	 * 不过，当涉及 LATERAL 子查询时，只要其参数化
	 * 没有在连接关系内部消解，就不会有不被该子查询
	 * 所参数化的连接关系路径。因此不妨再允许依赖
	 * 连接关系将具有的
	 * 任何剩余 LATERAL 依赖。
	 */
	extra.param_source_rels = bms_add_members(extra.param_source_rels,
											  joinrel->lateral_relids);

	/*
	 * 1. 考虑两侧关系都必须显式
	 * 排序的归并连接路径。若不能做归并连接就跳过。
	 */
	if (mergejoin_allowed)
		sort_inner_and_outer(root, joinrel, outerrel, innerrel,
							 jointype, &extra);

	/*
	 * 2. 考虑外层关系不必显式
	 * 排序的路径。这包括嵌套循环，以及外层
	 * 路径已经有序的归并连接。同样，若不能做归并连接就跳过。
	 * （这没问题，因为嵌套循环根本无法处理
	 * 右连接/右反连接/全连接，因此在
	 * 被禁止的情形里它也行不通。）
	 */
	if (mergejoin_allowed)
		match_unsorted_outer(root, joinrel, outerrel, innerrel,
							 jointype, &extra);

#ifdef NOT_USED

	/*
	 * 3. 考虑内层关系不必显式
	 * 排序的路径。这只包括归并连接（嵌套循环已在
	 * match_unsorted_outer 中建成）。
	 *
	 * 已于 2000-02-13 当作冗余停用 -- tgl。归并连接的内层一侧和外层一侧
	 * 并没有真正
	 * 重要的差别，因此当 add_paths_to_joinrel() 以相反顺序
	 * 传入两个关系时，match_unsorted_inner 造不出与
	 * match_unsorted_outer 不等价的路径。
	 */
	if (mergejoin_allowed)
		match_unsorted_inner(root, joinrel, outerrel, innerrel,
							 jointype, &extra);
#endif

	/*
	 * 4. 考虑外层和内层关系在连接前都必须先
	 * 做哈希的路径。同上，对全
	 * 连接忽略 enable_hashjoin，因为也许没有别的办法。
	 */
	if (enable_hashjoin || jointype == JOIN_FULL)
		hash_inner_and_outer(root, joinrel, outerrel, innerrel,
							 jointype, &extra);

	/*
	 * createplan.c 目前不支持处理由扩展下推到连接的伪常量
	 * 子句；检查
	 * restrictlist 是否有这类子句，若没有，就允许它们考虑
	 * 下推连接。
	 */
	if ((joinrel->fdwroutine &&
		 joinrel->fdwroutine->GetForeignJoinPaths) ||
		set_join_pathlist_hook)
		consider_join_pushdown = !has_pseudoconstant_clauses(root,
															 restrictlist);

	/*
	 * 5. 若内层和外层关系是属于
	 * 同一服务器、并且为检查访问
	 * 权限而指派给同一用户的外部表（或连接），就给 FDW 一次下推连接的机会。
	 */
	if (joinrel->fdwroutine &&
		joinrel->fdwroutine->GetForeignJoinPaths &&
		consider_join_pushdown)
		joinrel->fdwroutine->GetForeignJoinPaths(root, joinrel,
												 outerrel, innerrel,
												 jointype, &extra);

	/*
	 * 6. 最后，给扩展一次操纵路径列表的机会。它们
	 * 可以调用 add_path() 增加新路径（例如 CustomPaths），若感知并行则调用
	 * add_partial_path()。它们也可以删除或修改
	 * 核心代码加入的路径。
	 */
	if (set_join_pathlist_hook &&
		consider_join_pushdown)
		set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
							   jointype, &extra);
}

/*
 * 我们放宽 param_source_rels 启发式，以接受这样的嵌套循环路径：
 * 外层关系满足内层路径的一部分但不是全部
 * 参数化。这对星型模式
 * 得到好计划是必要的：大表的参数化路径也许需要
 * 来自多个不会彼此直接连接的小表的
 * 参数。我们可以把小
 * 表放在外侧、叠起嵌套循环来处理；但这打破了 param_source_rels
 * 启发式所依据的规则，即除非有基于连接顺序约束的理由，
 * 否则不应当把参数向下
 * 传过连接。
 *
 * 因此当这种情形适用时，忽略 param_source_rels 限制。
 * allow_star_schema_join() 在应当覆盖该限制时返回 true，也就是做这次连接可以。
 */
static inline bool
allow_star_schema_join(PlannerInfo *root,
					   Relids outerrelids,
					   Relids inner_paramrels)
{
	/*
	 * 若外层关系提供了内层关系的一部分但不是全部
	 * 参数化，就是星型模式情形。
	 */
	return (bms_overlap(inner_paramrels, outerrelids) &&
			bms_nonempty_difference(inner_paramrels, outerrelids));
}

/*
 * 若参数化只被外层关系部分满足，
 * 未满足的部分不能包含任何会把已满足部分的关系
 * 置空的外连接 relid。那意味着我们试图
 * 在该值尚不可计算的连接层使用带非空 varnullingrels 的
 * Var。
 *
 * 实践中这个测试从不会发现问题，因为更早的连接顺序
 * 限制会阻止我们尝试会引起问题的连接。
 * （这不奇怪，因为在我们把
 * 外连接 relid 加入表达式 relid 之前，代码就能工作。）作为最后防线仍值得检查，
 * 但只在启用断言的构建里做。
 */
#ifdef USE_ASSERT_CHECKING
static inline bool
have_unsafe_outer_join_ref(PlannerInfo *root,
						   Relids outerrelids,
						   Relids inner_paramrels)
{
	bool		result = false;
	Relids		unsatisfied = bms_difference(inner_paramrels, outerrelids);
	Relids		satisfied = bms_intersect(inner_paramrels, outerrelids);

	if (bms_overlap(unsatisfied, root->outer_join_rels))
	{
		ListCell   *lc;

		foreach(lc, root->join_info_list)
		{
			SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

			if (!bms_is_member(sjinfo->ojrelid, unsatisfied))
				continue;		/* 不相关 */
			if (bms_overlap(satisfied, sjinfo->min_righthand) ||
				(sjinfo->jointype == JOIN_FULL &&
				 bms_overlap(satisfied, sjinfo->min_lefthand)))
			{
				result = true;	/* 行不通 */
				break;
			}
		}
	}

	/* 在这里拒绝路径时不要浪费内存 */
	bms_free(unsatisfied);
	bms_free(satisfied);

	return result;
}
#endif							/* USE_ASSERT_CHECKING */

/*
 * paraminfo_get_equal_hashops
 *		判断 param_info 中的子句和 innerrel 的 lateral_vars
 *		能否做哈希。
 *		若可以哈希则返回 true，否则返回 false。
 *
 * 另外，成功时收集外层表达式，以及
 * innerrel 每个可哈希参数的适当等值操作符。
 * 它们以平行列表返回在 *param_exprs 和 *operators 中。
 * 还设置 *binary_mode，以表明是否需要
 * 严格的二进制匹配。
 */
static bool
paraminfo_get_equal_hashops(PlannerInfo *root, ParamPathInfo *param_info,
							RelOptInfo *outerrel, RelOptInfo *innerrel,
							List **param_exprs, List **operators,
							bool *binary_mode)

{
	ListCell   *lc;

	*param_exprs = NIL;
	*operators = NIL;
	*binary_mode = false;

	/* 把 param_info 中的连接子句加入哈希键 */
	if (param_info != NULL)
	{
		List	   *clauses = param_info->ppi_clauses;

		foreach(lc, clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
			OpExpr	   *opexpr;
			Node	   *expr;
			Oid			hasheqoperator;

			opexpr = (OpExpr *) rinfo->clause;

			/*
			 * 若 rinfo 不兼容则放弃。需要一个有
			 * 2 个参数的连接 OpExpr。
			 */
			if (!IsA(opexpr, OpExpr) || list_length(opexpr->args) != 2 ||
				!clause_sides_match_join(rinfo, outerrel, innerrel))
			{
				list_free(*operators);
				list_free(*param_exprs);
				return false;
			}

			if (rinfo->outer_is_left)
			{
				expr = (Node *) linitial(opexpr->args);
				hasheqoperator = rinfo->left_hasheqoperator;
			}
			else
			{
				expr = (Node *) lsecond(opexpr->args);
				hasheqoperator = rinfo->right_hasheqoperator;
			}

			/* 若不能对外层类型做哈希，就不能做 memoize */
			if (!OidIsValid(hasheqoperator))
			{
				list_free(*operators);
				list_free(*param_exprs);
				return false;
			}

			*operators = lappend_oid(*operators, hasheqoperator);
			*param_exprs = lappend(*param_exprs, expr);

			/*
			 * 当连接操作符不可哈希时，有可能
			 * 该操作符能区分哈希
			 * 等值操作符无法区分的东西。例如对浮点
			 * 类型，-0.0 和 +0.0 被哈希
			 * 函数和等值函数视为相等，但别的操作符也许
			 * 能把这两个值分开。这意味着必须让
			 * memoize 进入二进制比较模式，从而做逐位
			 * 比较，而不是像使用
			 * 哈希等值操作符时那样做 "逻辑" 比较。
			 */
			if (!OidIsValid(rinfo->hashjoinoperator))
				*binary_mode = true;
		}
	}

	/* 现在也把任何 lateral 变量加入缓存键 */
	foreach(lc, innerrel->lateral_vars)
	{
		Node	   *expr = (Node *) lfirst(lc);
		TypeCacheEntry *typentry;

		/* 若 PHVs 中有任何易变函数，则拒绝 */
		if (contain_volatile_functions(expr))
		{
			list_free(*operators);
			list_free(*param_exprs);
			return false;
		}

		typentry = lookup_type_cache(exprType(expr),
									 TYPECACHE_HASH_PROC | TYPECACHE_EQ_OPR);

		/* 没有有效的哈希过程和等值操作符就不能使用 memoize */
		if (!OidIsValid(typentry->hash_proc) || !OidIsValid(typentry->eq_opr))
		{
			list_free(*operators);
			list_free(*param_exprs);
			return false;
		}

		*operators = lappend_oid(*operators, typentry->eq_opr);
		*param_exprs = lappend(*param_exprs, expr);

		/*
		 * 必须进入二进制模式，因为不太清楚
		 * 这些 lateral Vars 是怎么用的。见上面对非 lateral Var
		 * 设置 *binary_mode 的注释。若有 RestrictInfos 并知道
		 * 所用操作符，这一点可以放宽一些，但对作为
		 * 函数参数的 Vars 这类情形，必须以
		 * 二进制模式工作，因为看不见
		 * 函数对这些 Vars 做了什么。
		 */
		*binary_mode = true;
	}

	/* 可以使用 memoize */
	return true;
}

/*
 * get_memoize_path
 *		若可能，就在 inner_path 之上造一个 Memoize 路径并返回。
 *		否则返回 NULL。
 */
static Path *
get_memoize_path(PlannerInfo *root, RelOptInfo *innerrel,
				 RelOptInfo *outerrel, Path *inner_path,
				 Path *outer_path, JoinType jointype,
				 JoinPathExtraData *extra)
{
	List	   *param_exprs;
	List	   *hash_operators;
	ListCell   *lc;
	bool		binary_mode;

	/* 若已禁用，显然不必费心 */
	if (!enable_memoize)
		return NULL;

	/*
	 * 除非预期会做不止一次
	 * 内层扫描，否则可以安全地不管这些。第一次扫描总是缓存
	 * 未命中。以后基于代价很可能也会失败，所以这
	 * 其实只是省掉一些白费的功夫。
	 */
	if (outer_path->parent->rows < 2)
		return NULL;

	/*
	 * 只有存在某种缓存键时才能有 memoize 节点，
	 * 要么是参数化路径子句，要么是 lateral Vars。没有缓存键听起来
	 * 更像 Materialize 节点也许更有用的情形。
	 */
	if ((inner_path->param_info == NULL ||
		 inner_path->param_info->ppi_clauses == NIL) &&
		innerrel->lateral_vars == NIL)
		return NULL;

	/*
	 * 目前对 SEMI 和 ANTI 连接不这样做，除非它们被
	 * 标成 inner_unique。这是因为嵌套循环 SEMI/ANTI 连接
	 * 不会把内层节点扫描到完成，这意味着 memoize 无法
	 * 把缓存项标成完成。
	 *
	 * XXX 目前不试图把 SEMI/ANTI 连接的 inner_unique
	 * 标成 true。应当这样做吗？见 add_paths_to_joinrel()
	 */
	if (!extra->inner_unique && (jointype == JOIN_SEMI ||
								 jointype == JOIN_ANTI))
		return NULL;

	/*
	 * Memoize 通常在从子计划读完
	 * 元组时把缓存项标成完成。不过对唯一连接，嵌套
	 * 循环在找到第一个匹配的
	 * 内层元组后会跳到下一个外层元组。这意味着也许不会把连接的内层
	 * 读到完成，从而没有机会把缓存项
	 * 标成完成。为绕过这一点，当连接唯一时，
	 * 在取出第一个
	 * 元组后自动把缓存项标成完成。当整个连接条件都被参数化时，这可行。
	 * 否则，当参数化只是连接
	 * 条件的一个子集时，无法确定是哪一部分使连接
	 * 成为唯一的。这意味着无法保证只会读到 1 个元组。
	 * 没有这个保证，就无法在读完
	 * 第一个元组后把缓存项标成完成。这意味着 Memoize
	 * 节点的用处仅限于没有连接
	 * 伙伴的外层行，因为只有这时嵌套循环才会耗尽
	 * 唯一连接的内层扫描。既然范围仅限于此，
	 * 就干脆不为这种情形造 memoize 路径。
	 *
	 * 这里不必考虑 lateral 变量，因为判断连接是否唯一时
	 * 不考虑它们。
	 *
	 * XXX 若把剩余连接限定条件做成
	 * 内层扫描的过滤条件而不是连接过滤条件，就可以启用。也许值得
	 * 考虑这样做？
	 */
	if (extra->inner_unique &&
		(inner_path->param_info == NULL ||
		 bms_num_members(inner_path->param_info->ppi_serials) <
		 list_length(extra->restrictlist)))
		return NULL;

	/*
	 * 若内层关系的目标列表或限制列表中有易变函数，就不能用 memoize 节点。
	 * 缓存命中会减少
	 * 对这些函数的调用次数。
	 */
	if (contain_volatile_functions((Node *) innerrel->reltarget))
		return NULL;

	foreach(lc, innerrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (contain_volatile_functions((Node *) rinfo))
			return NULL;
	}

	/*
	 * 还要检查参数化路径的 restrictinfos 是否有易变函数。
	 * 索引函数必须是不可变的，因此不应当有任何易变
	 * 函数，不过在 LATERAL 连接下，内层扫描也许不是
	 * 索引扫描。
	 */
	if (inner_path->param_info != NULL)
	{
		foreach(lc, inner_path->param_info->ppi_clauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (contain_volatile_functions((Node *) rinfo))
				return NULL;
		}
	}

	/* 检查该路径的每个参数是否都有哈希操作 */
	if (paraminfo_get_equal_hashops(root,
									inner_path->param_info,
									outerrel->top_parent ?
									outerrel->top_parent : outerrel,
									innerrel,
									&param_exprs,
									&hash_operators,
									&binary_mode))
	{
		return (Path *) create_memoize_path(root,
											innerrel,
											inner_path,
											param_exprs,
											hash_operators,
											extra->inner_unique,
											binary_mode,
											outer_path->rows);
	}

	return NULL;
}

/*
 * try_nestloop_path
 *	  考虑一条嵌套循环连接路径；若它看起来有用，就经由 add_path()
 *	  推进连接关系的 pathlist。
 */
static void
try_nestloop_path(PlannerInfo *root,
				  RelOptInfo *joinrel,
				  Path *outer_path,
				  Path *inner_path,
				  List *pathkeys,
				  JoinType jointype,
				  JoinPathExtraData *extra)
{
	Relids		required_outer;
	JoinCostWorkspace workspace;
	RelOptInfo *innerrel = inner_path->parent;
	RelOptInfo *outerrel = outer_path->parent;
	Relids		innerrelids;
	Relids		outerrelids;
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);

	/*
	 * 若正在这次连接上形成外连接，用一条把该外连接当作其参数化一部分的
	 * 输入路径是没有意义的。
	 * （尽管有连接顺序限制，这仍可能发生，因为那些限制针对的是
	 * 输入关系里有什么，而不是它的参数是什么。）
	 */
	if (extra->sjinfo->ojrelid != 0 &&
		(bms_is_member(extra->sjinfo->ojrelid, inner_paramrels) ||
		 bms_is_member(extra->sjinfo->ojrelid, outer_paramrels)))
		return;

	/*
	 * 路径由顶层父项参数化，因此对父项 relid
	 * 做参数化测试。
	 */
	if (innerrel->top_parent_relids)
		innerrelids = innerrel->top_parent_relids;
	else
		innerrelids = innerrel->relids;

	if (outerrel->top_parent_relids)
		outerrelids = outerrel->top_parent_relids;
	else
		outerrelids = outerrel->relids;

	/*
	 * 检查提议的路径是否仍被参数化，若这种
	 * 参数化不合理就拒绝 --- 除非 allow_star_schema_join
	 * 说无论如何都允许。另外，若 have_dangerous_phv
	 * 不喜欢它的样子，也必须拒绝；这只可能发生在嵌套循环
	 * 仍被参数化时。
	 */
	required_outer = calc_nestloop_required_outer(outerrelids, outer_paramrels,
												  innerrelids, inner_paramrels);
	if (required_outer &&
		((!bms_overlap(required_outer, extra->param_source_rels) &&
		  !allow_star_schema_join(root, outerrelids, inner_paramrels)) ||
		 have_dangerous_phv(root, outerrelids, inner_paramrels)))
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
		return;
	}

	/* 若通过了那一步，就不应当再有不安全的外连接引用 */
	Assert(!have_unsafe_outer_join_ref(root, outerrelids, inner_paramrels));

	/*
	 * 先做预检，以便很快淘汰明显较差的路径。我们
	 * 计算该路径代价的一个廉价下界，然后用
	 * add_path_precheck() 看它是否显然会被
	 * 连接关系的某条已有路径支配。若不是，再做完整的推入：
	 * 创建完全有效的路径结构并交给 add_path()。
	 * 后两步足够贵，使这种两阶段
	 * 做法值得。
	 */
	initial_cost_nestloop(root, &workspace, jointype,
						  outer_path, inner_path, extra);

	if (add_path_precheck(joinrel,
						  workspace.startup_cost, workspace.total_cost,
						  pathkeys, required_outer))
	{
		/*
		 * 若内层路径被参数化，它是被外层关系的
		 * 最顶层父项参数化，而不是被外层关系本身。要
		 * 修正这一点。
		 */
		if (PATH_PARAM_BY_PARENT(inner_path, outer_path->parent))
		{
			inner_path = reparameterize_path_by_child(root, inner_path,
													  outer_path->parent);

			/*
			 * 若无法翻译该路径，就无法创建嵌套循环
			 * 路径。
			 */
			if (!inner_path)
			{
				bms_free(required_outer);
				return;
			}
		}

		add_path(joinrel, (Path *)
				 create_nestloop_path(root,
									  joinrel,
									  jointype,
									  &workspace,
									  extra,
									  outer_path,
									  inner_path,
									  extra->restrictlist,
									  pathkeys,
									  required_outer));
	}
	else
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
	}
}

/*
 * try_partial_nestloop_path
 *	  考虑一条部分嵌套循环连接路径；若它看起来有用，就经由 add_partial_path()
 *	  推进连接关系的 partial_pathlist。
 */
static void
try_partial_nestloop_path(PlannerInfo *root,
						  RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  List *pathkeys,
						  JoinType jointype,
						  JoinPathExtraData *extra)
{
	JoinCostWorkspace workspace;

	/*
	 * 若内层路径被参数化，其参数化必须被提议的外层路径
	 * 完全满足。不支持被参数化的部分路径。
	 * 调用方应当已经验证这里不需要 lateral
	 * 关系。
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;
		RelOptInfo *outerrel = outer_path->parent;
		Relids		outerrelids;

		/*
		 * 内层和外层路径若被参数化，是被顶层
		 * 父项参数化，而不是被子关系，因此参数化测试必须用那些
		 * relid。
		 */
		if (outerrel->top_parent_relids)
			outerrelids = outerrel->top_parent_relids;
		else
			outerrelids = outerrel->relids;

		if (!bms_is_subset(inner_paramrels, outerrelids))
			return;
	}

	/*
	 * 创建路径之前，先对它可能的
	 * 代价取一个快速下界。若看起来很糟，立刻放弃。
	 */
	initial_cost_nestloop(root, &workspace, jointype,
						  outer_path, inner_path, extra);
	if (!add_partial_path_precheck(joinrel, workspace.total_cost, pathkeys))
		return;

	/*
	 * 若内层路径被参数化，它是被外层关系的最顶层
	 * 父项参数化，而不是被外层关系本身。要修正这一点。
	 */
	if (PATH_PARAM_BY_PARENT(inner_path, outer_path->parent))
	{
		inner_path = reparameterize_path_by_child(root, inner_path,
												  outer_path->parent);

		/*
		 * 若无法翻译该路径，就无法创建嵌套循环路径。
		 */
		if (!inner_path)
			return;
	}

	/* 也许好到值得一试，那就试试。 */
	add_partial_path(joinrel, (Path *)
					 create_nestloop_path(root,
										  joinrel,
										  jointype,
										  &workspace,
										  extra,
										  outer_path,
										  inner_path,
										  extra->restrictlist,
										  pathkeys,
										  NULL));
}

/*
 * try_mergejoin_path
 *	  考虑一条归并连接路径；若它看起来有用，就经由 add_path()
 *	  推进连接关系的 pathlist。
 */
static void
try_mergejoin_path(PlannerInfo *root,
				   RelOptInfo *joinrel,
				   Path *outer_path,
				   Path *inner_path,
				   List *pathkeys,
				   List *mergeclauses,
				   List *outersortkeys,
				   List *innersortkeys,
				   JoinType jointype,
				   JoinPathExtraData *extra,
				   bool is_partial)
{
	Relids		required_outer;
	JoinCostWorkspace workspace;

	if (is_partial)
	{
		try_partial_mergejoin_path(root,
								   joinrel,
								   outer_path,
								   inner_path,
								   pathkeys,
								   mergeclauses,
								   outersortkeys,
								   innersortkeys,
								   jointype,
								   extra);
		return;
	}

	/*
	 * 若正在这次连接上形成外连接，用一条把该外连接当作其参数化一部分的
	 * 输入路径是没有意义的。
	 * （尽管有连接顺序限制，这仍可能发生，因为那些限制针对的是
	 * 输入关系里有什么，而不是它的参数是什么。）
	 */
	if (extra->sjinfo->ojrelid != 0 &&
		(bms_is_member(extra->sjinfo->ojrelid, PATH_REQ_OUTER(inner_path)) ||
		 bms_is_member(extra->sjinfo->ojrelid, PATH_REQ_OUTER(outer_path))))
		return;

	/*
	 * 检查提议的路径是否仍被参数化，若这种
	 * 参数化不合理就拒绝。
	 */
	required_outer = calc_non_nestloop_required_outer(outer_path,
													  inner_path);
	if (required_outer &&
		!bms_overlap(required_outer, extra->param_source_rels))
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
		return;
	}

	/*
	 * 若给定路径已经足够有序，就可以跳过
	 * 显式排序。
	 */
	if (outersortkeys &&
		pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
		outersortkeys = NIL;
	if (innersortkeys &&
		pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
		innersortkeys = NIL;

	/*
	 * 见 try_nestloop_path() 中的注释。
	 */
	initial_cost_mergejoin(root, &workspace, jointype, mergeclauses,
						   outer_path, inner_path,
						   outersortkeys, innersortkeys,
						   extra);

	if (add_path_precheck(joinrel,
						  workspace.startup_cost, workspace.total_cost,
						  pathkeys, required_outer))
	{
		add_path(joinrel, (Path *)
				 create_mergejoin_path(root,
									   joinrel,
									   jointype,
									   &workspace,
									   extra,
									   outer_path,
									   inner_path,
									   extra->restrictlist,
									   pathkeys,
									   required_outer,
									   mergeclauses,
									   outersortkeys,
									   innersortkeys));
	}
	else
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
	}
}

/*
 * try_partial_mergejoin_path
 *	  考虑一条部分归并连接路径；若它看起来有用，就经由 add_partial_path()
 *	  推进连接关系的 pathlist。
 */
static void
try_partial_mergejoin_path(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   Path *outer_path,
						   Path *inner_path,
						   List *pathkeys,
						   List *mergeclauses,
						   List *outersortkeys,
						   List *innersortkeys,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinCostWorkspace workspace;

	/*
	 * 见 try_partial_hashjoin_path() 中的注释。
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;

		if (!bms_is_empty(inner_paramrels))
			return;
	}

	/*
	 * 若给定路径已经足够有序，就可以跳过
	 * 显式排序。
	 */
	if (outersortkeys &&
		pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
		outersortkeys = NIL;
	if (innersortkeys &&
		pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
		innersortkeys = NIL;

	/*
	 * 见 try_partial_nestloop_path() 中的注释。
	 */
	initial_cost_mergejoin(root, &workspace, jointype, mergeclauses,
						   outer_path, inner_path,
						   outersortkeys, innersortkeys,
						   extra);

	if (!add_partial_path_precheck(joinrel, workspace.total_cost, pathkeys))
		return;

	/* 也许好到值得一试，那就试试。 */
	add_partial_path(joinrel, (Path *)
					 create_mergejoin_path(root,
										   joinrel,
										   jointype,
										   &workspace,
										   extra,
										   outer_path,
										   inner_path,
										   extra->restrictlist,
										   pathkeys,
										   NULL,
										   mergeclauses,
										   outersortkeys,
										   innersortkeys));
}

/*
 * try_hashjoin_path
 *	  考虑一条哈希连接路径；若它看起来有用，就经由 add_path()
 *	  推进连接关系的 pathlist。
 */
static void
try_hashjoin_path(PlannerInfo *root,
				  RelOptInfo *joinrel,
				  Path *outer_path,
				  Path *inner_path,
				  List *hashclauses,
				  JoinType jointype,
				  JoinPathExtraData *extra)
{
	Relids		required_outer;
	JoinCostWorkspace workspace;

	/*
	 * 若正在这次连接上形成外连接，用一条把该外连接当作其参数化一部分的
	 * 输入路径是没有意义的。
	 * （尽管有连接顺序限制，这仍可能发生，因为那些限制针对的是
	 * 输入关系里有什么，而不是它的参数是什么。）
	 */
	if (extra->sjinfo->ojrelid != 0 &&
		(bms_is_member(extra->sjinfo->ojrelid, PATH_REQ_OUTER(inner_path)) ||
		 bms_is_member(extra->sjinfo->ojrelid, PATH_REQ_OUTER(outer_path))))
		return;

	/*
	 * 检查提议的路径是否仍被参数化，若这种
	 * 参数化不合理就拒绝。
	 */
	required_outer = calc_non_nestloop_required_outer(outer_path,
													  inner_path);
	if (required_outer &&
		!bms_overlap(required_outer, extra->param_source_rels))
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
		return;
	}

	/*
	 * 见 try_nestloop_path() 中的注释。另外注意，按 create_hashjoin_path 里的注释，
	 * 哈希连接路径从来没有任何输出路径键。
	 */
	initial_cost_hashjoin(root, &workspace, jointype, hashclauses,
						  outer_path, inner_path, extra, false);

	if (add_path_precheck(joinrel,
						  workspace.startup_cost, workspace.total_cost,
						  NIL, required_outer))
	{
		add_path(joinrel, (Path *)
				 create_hashjoin_path(root,
									  joinrel,
									  jointype,
									  &workspace,
									  extra,
									  outer_path,
									  inner_path,
									  false,	/* parallel_hash（并行哈希） */
									  extra->restrictlist,
									  required_outer,
									  hashclauses));
	}
	else
	{
		/* 在这里拒绝路径时不要浪费内存 */
		bms_free(required_outer);
	}
}

/*
 * try_partial_hashjoin_path
 *	  考虑一条部分哈希连接路径；若它看起来有用，就经由 add_partial_path()
 *	  推进连接关系的 partial_pathlist。
 *	  外层一侧是部分的。若 parallel_hash 为 true，则内层路径
 *	  必须是部分的，并将并行运行以创建一个或多个共享
 *	  哈希表；否则内层路径必须是完整的，并在每个进程里运行它的一份副本，
 *	  以创建各自相同的私有哈希表。
 */
static void
try_partial_hashjoin_path(PlannerInfo *root,
						  RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  List *hashclauses,
						  JoinType jointype,
						  JoinPathExtraData *extra,
						  bool parallel_hash)
{
	JoinCostWorkspace workspace;

	/*
	 * 若内层路径被参数化，其参数化必须被提议的外层路径
	 * 完全满足。不支持被参数化的部分路径。
	 * 调用方应当已经验证这里不需要 lateral
	 * 关系。
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;

		if (!bms_is_empty(inner_paramrels))
			return;
	}

	/*
	 * 创建路径之前，先对它可能的
	 * 代价取一个快速下界。若看起来很糟，立刻放弃。
	 */
	initial_cost_hashjoin(root, &workspace, jointype, hashclauses,
						  outer_path, inner_path, extra, parallel_hash);
	if (!add_partial_path_precheck(joinrel, workspace.total_cost, NIL))
		return;

	/* 也许好到值得一试，那就试试。 */
	add_partial_path(joinrel, (Path *)
					 create_hashjoin_path(root,
										  joinrel,
										  jointype,
										  &workspace,
										  extra,
										  outer_path,
										  inner_path,
										  parallel_hash,
										  extra->restrictlist,
										  NULL,
										  hashclauses));
}

/*
 * clause_sides_match_join
 *	  判断一条连接子句的形式是否适合这次连接。
 *
 * 已经知道该子句是只引用当前连接中那些
 * 关系的二元操作子句。这里要检查它是否具有
 * "outerrel_expr op innerrel_expr" 或 "innerrel_expr op outerrel_expr" 的形式，
 * 而不是在任一侧混有外层和内层变量。若匹配，
 * 就设置临时标志 outer_is_left，以标明哪一侧是哪一侧。
 */
static inline bool
clause_sides_match_join(RestrictInfo *rinfo, RelOptInfo *outerrel,
						RelOptInfo *innerrel)
{
	if (bms_is_subset(rinfo->left_relids, outerrel->relids) &&
		bms_is_subset(rinfo->right_relids, innerrel->relids))
	{
		/* 左侧是外层 */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrel->relids) &&
			 bms_is_subset(rinfo->right_relids, outerrel->relids))
	{
		/* 右侧是外层 */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* 对这些输入关系不行 */
}

/*
 * sort_inner_and_outer
 *	  通过对每个可用的归并排序，显式排序外层和
 *	  内层连接关系，来创建归并连接路径。
 *
 * 'joinrel' 是连接关系
 * 'outerrel' 是外层连接关系
 * 'innerrel' 是内层连接关系
 * 'jointype' 是要做的连接类型
 * 'extra' 含有额外的输入值
 */
static void
sort_inner_and_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	Path	   *outer_path;
	Path	   *inner_path;
	Path	   *cheapest_partial_outer = NULL;
	Path	   *cheapest_safe_inner = NULL;
	List	   *all_pathkeys;
	ListCell   *l;

	/*
	 * 这里只考虑总代价最低的输入路径，因为我们
	 * 假定需要排序。以后才会考虑
	 * 启动代价最低的输入路径，而且仅当它们不需要
	 * 排序时。
	 *
	 * 本函数有意不考虑被参数化的输入
	 * 路径，除非总代价最低的那条是被参数化的。若考虑，
	 * 就会有归并连接路径的组合爆炸，而价值
	 * 可疑。这与别处同样歧视
	 * 带参数化输入的归并连接的决定相互影响；见
	 * src/backend/optimizer/README 中的注释。
	 */
	outer_path = outerrel->cheapest_total_path;
	inner_path = innerrel->cheapest_total_path;

	/*
	 * 若任一条总代价最低的路径被另一个关系参数化，就不能
	 * 使用归并连接。（不必再找别的输入
	 * 路径，因为这些应当已经是可用路径中参数化最少的
	 * 那些。）
	 */
	if (PATH_PARAM_BY_REL(outer_path, innerrel) ||
		PATH_PARAM_BY_REL(inner_path, outerrel))
		return;

	/*
	 * 若要求唯一化，就先做，然后当作普通
	 * 内连接处理。
	 */
	if (jointype == JOIN_UNIQUE_OUTER)
	{
		outer_path = (Path *) create_unique_path(root, outerrel,
												 outer_path, extra->sjinfo);
		Assert(outer_path);
		jointype = JOIN_INNER;
	}
	else if (jointype == JOIN_UNIQUE_INNER)
	{
		inner_path = (Path *) create_unique_path(root, innerrel,
												 inner_path, extra->sjinfo);
		Assert(inner_path);
		jointype = JOIN_INNER;
	}

	/*
	 * 若连接关系是并行安全的，也许可以考虑部分
	 * 归并连接。不过不能处理 JOIN_UNIQUE_OUTER，因为
	 * 外层路径将是部分的，因此不能正确
	 * 保证唯一性。同样，不能处理 JOIN_FULL、JOIN_RIGHT
	 * 和 JOIN_RIGHT_ANTI，因为它们会产生错误的空值扩展行。
	 * 另外，所得路径不得被参数化。
	 */
	if (joinrel->consider_parallel &&
		save_jointype != JOIN_UNIQUE_OUTER &&
		save_jointype != JOIN_FULL &&
		save_jointype != JOIN_RIGHT &&
		save_jointype != JOIN_RIGHT_ANTI &&
		outerrel->partial_pathlist != NIL &&
		bms_is_empty(joinrel->lateral_relids))
	{
		cheapest_partial_outer = (Path *) linitial(outerrel->partial_pathlist);

		if (inner_path->parallel_safe)
			cheapest_safe_inner = inner_path;
		else if (save_jointype != JOIN_UNIQUE_INNER)
			cheapest_safe_inner =
				get_cheapest_parallel_safe_total_inner(innerrel->pathlist);
	}

	/*
	 * 可用归并连接子句的每种可能顺序，都会以本质上相同的代价生成
	 * 一条排序不同的结果路径。在这一层连接上，我们
	 * 没有根据选择哪一种，但
	 * 有些排序对更高层的
	 * 归并连接也许更有用，因此值得考虑多种顺序。
	 *
	 * 实际上，并非每种归并子句顺序都会
	 * 生成不同的路径顺序，因为有些子句也许
	 * 部分冗余（引用相同的 EquivalenceClasses）。因此，
	 * 我们做的是把归并子句列表转换成规范
	 * 路径键列表，然后考虑路径键的不同顺序。
	 *
	 * 为路径键的 *每一种* 排列都生成一条路径似乎
	 * 不是制胜策略；规划时间的代价太高。目前
	 * 我们为每个路径键生成一条路径，把该路径键放在最前，
	 * 其余按随机顺序。这至少应当允许对任何其他可能的归并连接
	 * 伙伴路径做单子句
	 * 归并连接而不必重排序。但若没猜对次级
	 * 键的顺序，也许会把本来可以作为归并子句
	 * 完成的子句当作 qpquals 来求值。（实践中，归并子句很少
	 * 超过两条或三条，因此在这上面花大量
	 * 心思大概不值得。）
	 *
	 * select_outer_pathkeys_for_merge() 返回的路径键顺序背后
	 * 有一些启发式（见该函数），因此既要按原样试它，
	 * 也要做变体。
	 */
	all_pathkeys = select_outer_pathkeys_for_merge(root,
												   extra->mergeclause_list,
												   joinrel);

	foreach(l, all_pathkeys)
	{
		PathKey    *front_pathkey = (PathKey *) lfirst(l);
		List	   *cur_mergeclauses;
		List	   *outerkeys;
		List	   *innerkeys;
		List	   *merge_pathkeys;

		/* 造一份把这个放在最前的路径键列表 */
		if (l != list_head(all_pathkeys))
			outerkeys = lcons(front_pathkey,
							  list_delete_nth_cell(list_copy(all_pathkeys),
												   foreach_current_index(l)));
		else
			outerkeys = all_pathkeys;	/* 第一个不必费事…… */

		/* 按对应顺序对归并子句排序 */
		cur_mergeclauses =
			find_mergeclauses_for_outer_pathkeys(root,
												 outerkeys,
												 extra->mergeclause_list);

		/* 应当已经全部用上了…… */
		Assert(list_length(cur_mergeclauses) == list_length(extra->mergeclause_list));

		/* 为内层一侧建立排序路径键 */
		innerkeys = make_inner_pathkeys_for_merge(root,
												  cur_mergeclauses,
												  outerkeys);

		/* 建立表示输出排序的路径键 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerkeys);

		/*
		 * 现在就可以造路径了。
		 *
		 * 注意：代价最低的路径也许已经排序
		 * 正确。try_mergejoin_path 会检测那种情形并抑制
		 * 显式排序步骤，因此这里不必做。
		 */
		try_mergejoin_path(root,
						   joinrel,
						   outer_path,
						   inner_path,
						   merge_pathkeys,
						   cur_mergeclauses,
						   outerkeys,
						   innerkeys,
						   jointype,
						   extra,
						   false);

		/*
		 * 若有部分的外层路径和并行安全的内层路径，就尝试
		 * 部分归并连接路径。
		 */
		if (cheapest_partial_outer && cheapest_safe_inner)
			try_partial_mergejoin_path(root,
									   joinrel,
									   cheapest_partial_outer,
									   cheapest_safe_inner,
									   merge_pathkeys,
									   cur_mergeclauses,
									   outerkeys,
									   innerkeys,
									   jointype,
									   extra);
	}
}

/*
 * generate_mergejoin_paths
 *	为输入的 outerpath 创建可能的归并连接路径。
 *
 * 若有归并连接子句，就生成归并连接。生成归并连接的内层路径有
 * 两种办法：对最便宜的
 * 内层路径排序，或使用已经按归并所需
 * 适当有序的内层路径。若有多条归并子句，也许没有内层
 * 路径（或只有很贵的一条）能满足全部归并子句，但
 * 若截断归并子句列表（从而丢掉
 * 一些排序键要求），会有更好的路径。因此，除了完整列表，也考虑
 * 归并子句列表的截断。（理想情况下会考虑
 * 归并子句列表的全部子集，但那似乎太贵了。）
 */
static void
generate_mergejoin_paths(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *innerrel,
						 Path *outerpath,
						 JoinType jointype,
						 JoinPathExtraData *extra,
						 bool useallclauses,
						 Path *inner_cheapest_total,
						 List *merge_pathkeys,
						 bool is_partial)
{
	List	   *mergeclauses;
	List	   *innersortkeys;
	List	   *trialsortkeys;
	Path	   *cheapest_startup_inner;
	Path	   *cheapest_total_inner;
	JoinType	save_jointype = jointype;
	int			num_sortkeys;
	int			sortkeycnt;

	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
		jointype = JOIN_INNER;

	/* 寻找有用的归并子句（若有） */
	mergeclauses =
		find_mergeclauses_for_outer_pathkeys(root,
											 outerpath->pathkeys,
											 extra->mergeclause_list);

	/*
	 * 若没有归并连接的机会，这条外层路径就处理完了。
	 *
	 * 特殊边角情形：对 "x FULL JOIN y ON true"，根本不会有连接
	 * 子句。通常会生成一条无子句的嵌套循环路径，
	 * 但归并连接是唯一支持没有任何连接子句的 FULL JOIN
	 * 的连接类型，因此必须改为生成一条无子句的
	 * 归并连接路径。
	 */
	if (mergeclauses == NIL)
	{
		if (jointype == JOIN_FULL)
			 /* 可以尝试归并连接 */ ;
		else
			return;
	}
	if (useallclauses &&
		list_length(mergeclauses) != list_length(extra->mergeclause_list))
		return;

	/* 计算内层路径所需的顺序 */
	innersortkeys = make_inner_pathkeys_for_merge(root,
												  mergeclauses,
												  outerpath->pathkeys);

	/*
	 * 在对最便宜的内层排序的基础上生成归并连接。
	 * 既然需要排序，只有最低总代价要紧。
	 * （但若 inner_cheapest_total 已经正确排序，
	 * try_mergejoin_path 会做对的事。）
	 */
	try_mergejoin_path(root,
					   joinrel,
					   outerpath,
					   inner_cheapest_total,
					   merge_pathkeys,
					   mergeclauses,
					   NIL,
					   innersortkeys,
					   jointype,
					   extra,
					   is_partial);

	/* 若内层路径需要唯一化，就不能再做别的 */
	if (save_jointype == JOIN_UNIQUE_INNER)
		return;

	/*
	 * 寻找满足 innersortkey 列表的已预排序内层路径 ---
	 * 或者，若允许用
	 * 归并子句的一个子集来建归并连接，则其任何截断也可以。这里既考虑启动
	 * 代价低的，也考虑总代价低的。
	 *
	 * 目前这里不考虑被参数化的内层路径。这与
	 * 别处同样歧视
	 * 带参数化输入的归并连接的决定相互影响；见
	 * src/backend/optimizer/README 中的注释。
	 *
	 * 随着缩短排序键列表，只应当考虑严格比
	 * （尤其是不等于）先前迭代中找到的任何路径
	 * 更便宜的路径。否则就是有意使用比给定路径所允许的更少的
	 * 归并键（其余当作普通
	 * 连接限定条件），这不太可能是好主意。
	 * 另外，在这里用 compare_path_costs 淘汰路径，
	 * 比建成归并连接路径再扔掉
	 *
	 * 便宜得多。
	 * 若 inner_cheapest_total 已经足够有序，
	 * 使上面造的路径不需要排序，也不应当用它再造一条重复路径。
	 * 用处理前一种考虑的同一套逻辑来处理，办法是正确初始化跟踪
	 * 迄今最便宜者的变量。注意，若发现它匹配某一组更短的
	 * 路径键，我们并不拒绝
	 * inner_cheapest_total。那种情形对应于用更少的归并键来避免
	 * 对 inner_cheapest_total 排序，而上面我们已经对它排序了，因此
	 * 所考虑的计划是不同的。
	 */
	if (pathkeys_contained_in(innersortkeys,
							  inner_cheapest_total->pathkeys))
	{
		/* inner_cheapest_total 不需要排序 */
		cheapest_startup_inner = inner_cheapest_total;
		cheapest_total_inner = inner_cheapest_total;
	}
	else
	{
		/* 它确实需要排序，至少对完整的键集合是这样 */
		cheapest_startup_inner = NULL;
		cheapest_total_inner = NULL;
	}
	num_sortkeys = list_length(innersortkeys);
	if (num_sortkeys > 1 && !useallclauses)
		trialsortkeys = list_copy(innersortkeys);	/* 需要可修改的副本 */
	else
		trialsortkeys = innersortkeys;	/* 实际上不会截断 */

	for (sortkeycnt = num_sortkeys; sortkeycnt > 0; sortkeycnt--)
	{
		Path	   *innerpath;
		List	   *newclauses = NIL;

		/*
		 * 寻找对前
		 * 'sortkeycnt' 个 innersortkeys 足够有序的内层路径。注意：trialsortkeys 列表会被
		 * 破坏性地修改，所以我们做了一份副本……
		 */
		trialsortkeys = list_truncate(trialsortkeys, sortkeycnt);
		innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
												   trialsortkeys,
												   NULL,
												   TOTAL_COST,
												   is_partial);
		if (innerpath != NULL &&
			(cheapest_total_inner == NULL ||
			 compare_path_costs(innerpath, cheapest_total_inner,
								TOTAL_COST) < 0))
		{
			/* 找到一条便宜（或更便宜）的已排序路径 */
			/* 若还没选过，就选出正确的归并子句 */
			if (sortkeycnt < num_sortkeys)
			{
				newclauses =
					trim_mergeclauses_for_inner_pathkeys(root,
														 mergeclauses,
														 trialsortkeys);
				Assert(newclauses != NIL);
			}
			else
				newclauses = mergeclauses;
			try_mergejoin_path(root,
							   joinrel,
							   outerpath,
							   innerpath,
							   merge_pathkeys,
							   newclauses,
							   NIL,
							   NIL,
							   jointype,
							   extra,
							   is_partial);
			cheapest_total_inner = innerpath;
		}
		/* 同样，基于最低启动代价…… */
		innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
												   trialsortkeys,
												   NULL,
												   STARTUP_COST,
												   is_partial);
		if (innerpath != NULL &&
			(cheapest_startup_inner == NULL ||
			 compare_path_costs(innerpath, cheapest_startup_inner,
								STARTUP_COST) < 0))
		{
			/* 找到一条便宜（或更便宜）的已排序路径 */
			if (innerpath != cheapest_total_inner)
			{
				/*
				 * 若已经造过子句列表，就避免重建；在大的连接树里这能节省
				 * 内存……
				 */
				if (newclauses == NIL)
				{
					if (sortkeycnt < num_sortkeys)
					{
						newclauses =
							trim_mergeclauses_for_inner_pathkeys(root,
																 mergeclauses,
																 trialsortkeys);
						Assert(newclauses != NIL);
					}
					else
						newclauses = mergeclauses;
				}
				try_mergejoin_path(root,
								   joinrel,
								   outerpath,
								   innerpath,
								   merge_pathkeys,
								   newclauses,
								   NIL,
								   NIL,
								   jointype,
								   extra,
								   is_partial);
			}
			cheapest_startup_inner = innerpath;
		}

		/*
		 * 若需要全部子句，就不考虑截断的排序键。
		 */
		if (useallclauses)
			break;
	}
}

/*
 * match_unsorted_outer
 *	  为处理单个连接关系
 *	  'joinrel' 创建可能的连接路径，办法是对它的每条可能外层路径采用迭代替换或
 *	  归并连接（只考虑
 *	  已经有序到足以归并的外层路径）。
 *
 * 总是为每条可用的外层路径生成一条嵌套循环路径。
 * 实际上最多可以生成五条：一条在总代价最低的
 * 内层路径上，一条在同一路径的物化形式上，一条在
 * 启动代价最低的内层路径上（若不同），一条在
 * 总代价最低的内层索引扫描路径上（若有），以及一条在
 * 启动代价最低的内层索引扫描路径上（若不同）。
 *
 * 若有归并连接子句，也考虑归并连接。见
 * generate_mergejoin_paths 中的详细注释。
 *
 * 'joinrel' 是连接关系
 * 'outerrel' 是外层连接关系
 * 'innerrel' 是内层连接关系
 * 'jointype' 是要做的连接类型
 * 'extra' 含有额外的输入值
 */
static void
match_unsorted_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	bool		nestjoinOK;
	bool		useallclauses;
	Path	   *inner_cheapest_total = innerrel->cheapest_total_path;
	Path	   *matpath = NULL;
	ListCell   *lc1;

	/*
	 * 嵌套循环只支持内连接、左连接、半连接和反连接。另外，若在做
	 * 右连接、右反连接或全归并连接，必须把 *全部*
	 * 归并子句用作连接子句，否则不会有有效计划。
	 * （虽然这两个标志目前互为反面，仍把它们分开，
	 * 以便清晰并适应将来可能的变化。）
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
			nestjoinOK = true;
			useallclauses = false;
			break;
		case JOIN_RIGHT:
		case JOIN_RIGHT_ANTI:
		case JOIN_FULL:
			nestjoinOK = false;
			useallclauses = true;
			break;
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNIQUE_INNER:
			jointype = JOIN_INNER;
			nestjoinOK = true;
			useallclauses = false;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) jointype);
			nestjoinOK = false; /* 消除编译器告警 */
			useallclauses = false;
			break;
	}

	/*
	 * 若 inner_cheapest_total 被外层关系参数化，就忽略它；
	 * 下面会把它当作 cheapest_parameterized_paths 的成员来考虑，
	 * 但本例程考虑的其他可能都用不上。
	 */
	if (PATH_PARAM_BY_REL(inner_cheapest_total, outerrel))
		inner_cheapest_total = NULL;

	/*
	 * 若需要把内层路径唯一化，就只考虑
	 * 总代价最低的内层。
	 */
	if (save_jointype == JOIN_UNIQUE_INNER)
	{
		/* 无法用被外层关系参数化的内层路径来做这件事 */
		if (inner_cheapest_total == NULL)
			return;
		inner_cheapest_total = (Path *)
			create_unique_path(root, innerrel, inner_cheapest_total, extra->sjinfo);
		Assert(inner_cheapest_total);
	}
	else if (nestjoinOK)
	{
		/*
		 * 考虑把最便宜的内层路径物化，除非
		 * enable_material 关闭，或该路径反正会物化其
		 * 输出。
		 */
		if (enable_material && inner_cheapest_total != NULL &&
			!ExecMaterializesOutput(inner_cheapest_total->pathtype))
			matpath = (Path *)
				create_material_path(innerrel, inner_cheapest_total);
	}

	foreach(lc1, outerrel->pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *merge_pathkeys;

		/*
		 * 无法使用被内层关系参数化的外层路径。
		 */
		if (PATH_PARAM_BY_REL(outerpath, innerrel))
			continue;

		/*
		 * 若需要把外层路径唯一化，考虑除最便宜的外层之外的任何路径
		 * 都没有意义。（XXX 对唯一化情形，我们不考虑被参数化的
		 * 外层，也不考虑内层。应当考虑吗？）
		 */
		if (save_jointype == JOIN_UNIQUE_OUTER)
		{
			if (outerpath != outerrel->cheapest_total_path)
				continue;
			outerpath = (Path *) create_unique_path(root, outerrel,
													outerpath, extra->sjinfo);
			Assert(outerpath);
		}

		/*
		 * 结果将具有这种排序（即使它实现为
		 * 嵌套循环，即使有些归并子句是由
		 * qpquals 而不是真正的归并子句实现的）：
		 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerpath->pathkeys);

		if (save_jointype == JOIN_UNIQUE_INNER)
		{
			/*
			 * 考虑嵌套循环连接，但只用唯一化后的最便宜
			 * 内层路径
			 */
			try_nestloop_path(root,
							  joinrel,
							  outerpath,
							  inner_cheapest_total,
							  merge_pathkeys,
							  jointype,
							  extra);
		}
		else if (nestjoinOK)
		{
			/*
			 * 考虑用这条外层路径和内层关系的各种
			 * 可用路径做嵌套循环连接。我们考虑内层关系每种可用参数化
			 * （包括未参数化的情形）的
			 * 总代价最低的路径。
			 */
			ListCell   *lc2;

			foreach(lc2, innerrel->cheapest_parameterized_paths)
			{
				Path	   *innerpath = (Path *) lfirst(lc2);
				Path	   *mpath;

				try_nestloop_path(root,
								  joinrel,
								  outerpath,
								  innerpath,
								  merge_pathkeys,
								  jointype,
								  extra);

				/*
				 * 试着生成一条 memoize 路径，看那是否让
				 * 嵌套循环更便宜。
				 */
				mpath = get_memoize_path(root, innerrel, outerrel,
										 innerpath, outerpath, jointype,
										 extra);
				if (mpath != NULL)
					try_nestloop_path(root,
									  joinrel,
									  outerpath,
									  mpath,
									  merge_pathkeys,
									  jointype,
									  extra);
			}

			/* 也考虑最便宜内层路径的物化形式 */
			if (matpath != NULL)
				try_nestloop_path(root,
								  joinrel,
								  outerpath,
								  matpath,
								  merge_pathkeys,
								  jointype,
								  extra);
		}

		/* 若外层路径需要唯一化，就不能再做别的 */
		if (save_jointype == JOIN_UNIQUE_OUTER)
			continue;

		/* 若内层关系被外层参数化，就不能再做别的 */
		if (inner_cheapest_total == NULL)
			continue;

		/* 生成归并连接路径 */
		generate_mergejoin_paths(root, joinrel, innerrel, outerpath,
								 save_jointype, extra, useallclauses,
								 inner_cheapest_total, merge_pathkeys,
								 false);
	}

	/*
	 * 若 outerrel 有任何部分路径且连接关系并行安全，
	 * 就考虑部分嵌套循环和归并连接计划。不过不能
	 * 处理 JOIN_UNIQUE_OUTER，因为外层路径将是部分的，并且
	 * 因此不能正确保证唯一性。也不能
	 * 处理需要 lateral 关系的连接，因为部分路径不得被
	 * 参数化。同样，不能处理 JOIN_FULL、JOIN_RIGHT 和
	 * JOIN_RIGHT_ANTI，因为它们会产生错误的空值扩展行。
	 */
	if (joinrel->consider_parallel &&
		save_jointype != JOIN_UNIQUE_OUTER &&
		save_jointype != JOIN_FULL &&
		save_jointype != JOIN_RIGHT &&
		save_jointype != JOIN_RIGHT_ANTI &&
		outerrel->partial_pathlist != NIL &&
		bms_is_empty(joinrel->lateral_relids))
	{
		if (nestjoinOK)
			consider_parallel_nestloop(root, joinrel, outerrel, innerrel,
									   save_jointype, extra);

		/*
		 * 若 inner_cheapest_total 为 NULL 或非并行安全，就找出
		 * 最便宜的总代价并行安全路径。若在做 JOIN_UNIQUE_INNER，就不能
		 * 使用任何替代的内层路径。
		 */
		if (inner_cheapest_total == NULL ||
			!inner_cheapest_total->parallel_safe)
		{
			if (save_jointype == JOIN_UNIQUE_INNER)
				return;

			inner_cheapest_total = get_cheapest_parallel_safe_total_inner(innerrel->pathlist);
		}

		if (inner_cheapest_total)
			consider_parallel_mergejoin(root, joinrel, outerrel, innerrel,
										save_jointype, extra,
										inner_cheapest_total);
	}
}

/*
 * consider_parallel_mergejoin
 *	  尝试把外层关系的部分路径连到内层关系的完整路径，
 *	  来为连接关系建立部分路径。
 *
 * 'joinrel' 是连接关系
 * 'outerrel' 是外层连接关系
 * 'innerrel' 是内层连接关系
 * 'jointype' 是要做的连接类型
 * 'extra' 含有额外的输入值
 * 'inner_cheapest_total' 是 innerrel 的最低总代价路径
 */
static void
consider_parallel_mergejoin(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra,
							Path *inner_cheapest_total)
{
	ListCell   *lc1;

	/* 为每条部分外层路径生成归并连接路径 */
	foreach(lc1, outerrel->partial_pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *merge_pathkeys;

		/*
		 * 弄清我们创建的任何路径将具有的有用顺序。
		 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerpath->pathkeys);

		generate_mergejoin_paths(root, joinrel, innerrel, outerpath, jointype,
								 extra, false, inner_cheapest_total,
								 merge_pathkeys, true);
	}
}

/*
 * consider_parallel_nestloop
 *	  尝试把外层关系的部分路径连到内层关系的完整路径，
 *	  来为连接关系建立部分路径。
 *
 * 'joinrel' 是连接关系
 * 'outerrel' 是外层连接关系
 * 'innerrel' 是内层连接关系
 * 'jointype' 是要做的连接类型
 * 'extra' 含有额外的输入值
 */
static void
consider_parallel_nestloop(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outerrel,
						   RelOptInfo *innerrel,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	ListCell   *lc1;

	if (jointype == JOIN_UNIQUE_INNER)
		jointype = JOIN_INNER;

	foreach(lc1, outerrel->partial_pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *pathkeys;
		ListCell   *lc2;

		/* 弄清我们创建的任何路径将具有的有用顺序。 */
		pathkeys = build_join_pathkeys(root, joinrel, jointype,
									   outerpath->pathkeys);

		/*
		 * 尝试最便宜的参数化路径；只有那些与这个 outerrel 连接后会产生
		 * 未参数化路径的，才能通过
		 * try_partial_nestloop_path。最便宜的未参数化路径
		 * 也在这个列表里。
		 */
		foreach(lc2, innerrel->cheapest_parameterized_paths)
		{
			Path	   *innerpath = (Path *) lfirst(lc2);
			Path	   *mpath;

			/* 不能连到非并行安全的内层路径 */
			if (!innerpath->parallel_safe)
				continue;

			/*
			 * 若在做 JOIN_UNIQUE_INNER，只能用内层的
			 * cheapest_total_path，并且必须把它唯一化。（也许
			 * 可以放宽这一点，以允许其他安全的、未参数化的
			 * 内层路径，但目前 create_unique_path 并不
			 * 支持那样。）
			 */
			if (save_jointype == JOIN_UNIQUE_INNER)
			{
				if (innerpath != innerrel->cheapest_total_path)
					continue;
				innerpath = (Path *) create_unique_path(root, innerrel,
														innerpath,
														extra->sjinfo);
				Assert(innerpath);
			}

			try_partial_nestloop_path(root, joinrel, outerpath, innerpath,
									  pathkeys, jointype, extra);

			/*
			 * 试着生成一条 memoize 路径，看那是否让嵌套
			 * 循环更便宜。
			 */
			mpath = get_memoize_path(root, innerrel, outerrel,
									 innerpath, outerpath, jointype,
									 extra);
			if (mpath != NULL)
				try_partial_nestloop_path(root, joinrel, outerpath, mpath,
										  pathkeys, jointype, extra);
		}
	}
}

/*
 * hash_inner_and_outer
 *	  通过对每条可用哈希子句的外层和
 *	  内层键显式做哈希，来创建哈希连接路径。
 *
 * 'joinrel' 是连接关系
 * 'outerrel' 是外层连接关系
 * 'innerrel' 是内层连接关系
 * 'jointype' 是要做的连接类型
 * 'extra' 含有额外的输入值
 */
static void
hash_inner_and_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	List	   *hashclauses;
	ListCell   *l;

	/*
	 * 对任何给定的一对外层
	 * 和内层关系，只需要建一份哈希子句列表；全部可哈希子句都会用作键。
	 *
	 * 扫描该连接的 restrictinfo 列表，找出可用于这一对子关系的
	 * 可哈希连接子句。
	 */
	hashclauses = NIL;
	foreach(l, extra->restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * 若在处理外连接，只使用它自己的连接子句来
		 * 做哈希。对内连接不必这么挑剔。
		 */
		if (isouterjoin && RINFO_IS_PUSHED_DOWN(restrictinfo, joinrel->relids))
			continue;

		if (!restrictinfo->can_join ||
			restrictinfo->hashjoinoperator == InvalidOid)
			continue;			/* 不可哈希连接 */

		/*
		 * 检查子句是否具有 "outer op inner" 或 "inner op outer" 的形式。
		 */
		if (!clause_sides_match_join(restrictinfo, outerrel, innerrel))
			continue;			/* 对这些输入关系不行 */

		hashclauses = lappend(hashclauses, restrictinfo);
	}

	/* 若找到任何可用的哈希子句，就造路径 */
	if (hashclauses)
	{
		/*
		 * 外层路径既考虑总代价最低的，也考虑启动代价最低的。
		 * 不过内层路径除了
		 * 总代价最低的那条，不必考虑其他。
		 */
		Path	   *cheapest_startup_outer = outerrel->cheapest_startup_path;
		Path	   *cheapest_total_outer = outerrel->cheapest_total_path;
		Path	   *cheapest_total_inner = innerrel->cheapest_total_path;

		/*
		 * 若任一条总代价最低的路径被另一个关系参数化，就不能
		 * 使用哈希连接。（不必再找别的
		 * 输入路径，因为这些应当已经是可用路径中参数化最少的
		 * 那些。）
		 */
		if (PATH_PARAM_BY_REL(cheapest_total_outer, innerrel) ||
			PATH_PARAM_BY_REL(cheapest_total_inner, outerrel))
			return;

		/* 若需要就唯一化；忽略被参数化的可能 */
		if (jointype == JOIN_UNIQUE_OUTER)
		{
			cheapest_total_outer = (Path *)
				create_unique_path(root, outerrel,
								   cheapest_total_outer, extra->sjinfo);
			Assert(cheapest_total_outer);
			jointype = JOIN_INNER;
			try_hashjoin_path(root,
							  joinrel,
							  cheapest_total_outer,
							  cheapest_total_inner,
							  hashclauses,
							  jointype,
							  extra);
			/* 这里没有低启动代价的可能 */
		}
		else if (jointype == JOIN_UNIQUE_INNER)
		{
			cheapest_total_inner = (Path *)
				create_unique_path(root, innerrel,
								   cheapest_total_inner, extra->sjinfo);
			Assert(cheapest_total_inner);
			jointype = JOIN_INNER;
			try_hashjoin_path(root,
							  joinrel,
							  cheapest_total_outer,
							  cheapest_total_inner,
							  hashclauses,
							  jointype,
							  extra);
			if (cheapest_startup_outer != NULL &&
				cheapest_startup_outer != cheapest_total_outer)
				try_hashjoin_path(root,
								  joinrel,
								  cheapest_startup_outer,
								  cheapest_total_inner,
								  hashclauses,
								  jointype,
								  extra);
		}
		else
		{
			/*
			 * 对其他连接类型，我们考虑启动代价最低的外层
			 * 配上总代价最低的内层，然后考虑
			 * 包括被参数化路径在内的总代价最低路径的配对。
			 * 基于可能较低的启动代价去生成参数化路径没有用，
			 * 因此这样就够了。
			 */
			ListCell   *lc1;
			ListCell   *lc2;

			if (cheapest_startup_outer != NULL)
				try_hashjoin_path(root,
								  joinrel,
								  cheapest_startup_outer,
								  cheapest_total_inner,
								  hashclauses,
								  jointype,
								  extra);

			foreach(lc1, outerrel->cheapest_parameterized_paths)
			{
				Path	   *outerpath = (Path *) lfirst(lc1);

				/*
				 * 无法使用被内层关系
				 * 参数化的外层路径。
				 */
				if (PATH_PARAM_BY_REL(outerpath, innerrel))
					continue;

				foreach(lc2, innerrel->cheapest_parameterized_paths)
				{
					Path	   *innerpath = (Path *) lfirst(lc2);

					/*
					 * 也无法使用被
					 * 外层关系参数化的内层路径。
					 */
					if (PATH_PARAM_BY_REL(innerpath, outerrel))
						continue;

					if (outerpath == cheapest_startup_outer &&
						innerpath == cheapest_total_inner)
						continue;	/* 已经试过了 */

					try_hashjoin_path(root,
									  joinrel,
									  outerpath,
									  innerpath,
									  hashclauses,
									  jointype,
									  extra);
				}
			}
		}

		/*
		 * 若连接关系是并行安全的，也许可以考虑
		 * 部分哈希连接。不过不能处理 JOIN_UNIQUE_OUTER，
		 * 因为外层路径将是部分的，因此不能
		 * 正确保证唯一性。另外，所得路径
		 * 不得被参数化。
		 */
		if (joinrel->consider_parallel &&
			save_jointype != JOIN_UNIQUE_OUTER &&
			outerrel->partial_pathlist != NIL &&
			bms_is_empty(joinrel->lateral_relids))
		{
			Path	   *cheapest_partial_outer;
			Path	   *cheapest_partial_inner = NULL;
			Path	   *cheapest_safe_inner = NULL;

			cheapest_partial_outer =
				(Path *) linitial(outerrel->partial_pathlist);

			/*
			 * 能否也使用部分的内层计划，以便并行建立
			 * 共享哈希表？不能处理
			 * JOIN_UNIQUE_INNER，因为不能保证唯一性。
			 */
			if (innerrel->partial_pathlist != NIL &&
				save_jointype != JOIN_UNIQUE_INNER &&
				enable_parallel_hash)
			{
				cheapest_partial_inner =
					(Path *) linitial(innerrel->partial_pathlist);
				try_partial_hashjoin_path(root, joinrel,
										  cheapest_partial_outer,
										  cheapest_partial_inner,
										  hashclauses, jointype, extra,
										  true /* parallel_hash（并行哈希） */ );
			}

			/*
			 * 通常，既然连接关系是并行安全的，最便宜的
			 * 总代价内层路径也会是并行安全的，若不是，就得
			 * 去找最便宜的、安全的、未参数化的内层
			 * 路径。若在做 JOIN_UNIQUE_INNER，就不能使用任何替代的
			 * 内层路径。若是全连接、右连接或右反连接，就不能使用
			 * 并行（在每个后端里建哈希表），因为
			 * 没有一个进程拥有全部匹配位。
			 */
			if (save_jointype == JOIN_FULL ||
				save_jointype == JOIN_RIGHT ||
				save_jointype == JOIN_RIGHT_ANTI)
				cheapest_safe_inner = NULL;
			else if (cheapest_total_inner->parallel_safe)
				cheapest_safe_inner = cheapest_total_inner;
			else if (save_jointype != JOIN_UNIQUE_INNER)
				cheapest_safe_inner =
					get_cheapest_parallel_safe_total_inner(innerrel->pathlist);

			if (cheapest_safe_inner != NULL)
				try_partial_hashjoin_path(root, joinrel,
										  cheapest_partial_outer,
										  cheapest_safe_inner,
										  hashclauses, jointype, extra,
										  false /* parallel_hash（并行哈希） */ );
		}
	}
}

/*
 * select_mergejoin_clauses
 *	  选择对某次特定连接可用的归并连接子句。
 *	  返回这些子句的 RestrictInfo 节点列表。
 *
 * *mergejoin_allowed 通常设为 true，但若
 * 这是右连接/右反连接/全连接，并且有不可归并连接的连接
 * 子句，就设为 false。执行器的归并连接机制无法处理这些情形，因此
 * 必须避免生成归并连接计划。（注意，这个标志并不
 * 考虑是否真的有可归并连接的子句。这是
 * 对的，因为有些情形需要建立无子句的归并连接。
 * 因此只返回 NIL 不足以区分安全情形和
 * 不安全情形。）
 *
 * 还标记每条选中的 RestrictInfo，以表明当前把哪一侧
 * 当作外层。这些是临时标记，只在
 * 当前这次 add_paths_to_joinrel() 调用期间有效！
 *
 * 我们检查该连接已知的每条 restrictinfo 子句，看
 * 它是否可归并连接，以及是否涉及当前感兴趣的两个子关系的
 * 变量。
 */
static List *
select_mergejoin_clauses(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *outerrel,
						 RelOptInfo *innerrel,
						 List *restrictlist,
						 JoinType jointype,
						 bool *mergejoin_allowed)
{
	List	   *result_list = NIL;
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	bool		have_nonmergeable_joinclause = false;
	ListCell   *l;

	foreach(l, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * 若在处理外连接，归并时只用它自己的连接子句。
		 * 对内连接也可以用下推的子句。（注意：
		 * 这里不设置 have_nonmergeable_joinclause，因为下推的
		 * 子句会变成 otherquals 而不是 joinquals。）
		 */
		if (isouterjoin && RINFO_IS_PUSHED_DOWN(restrictinfo, joinrel->relids))
			continue;

		/* 检查该子句是否为可归并的操作符子句 */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
		{
			/*
			 * 做右连接/右反连接/全归并连接时，执行器可以处理作为常量的额外 joinquals，但不能
			 * 处理其他任何东西。
			 * （支持常量的原因是为了能做 FULL JOIN ON
			 * FALSE。）
			 */
			if (!restrictinfo->clause || !IsA(restrictinfo->clause, Const))
				have_nonmergeable_joinclause = true;
			continue;			/* 不可归并连接 */
		}

		/*
		 * 检查子句是否具有 "outer op inner" 或 "inner op outer" 的形式。
		 */
		if (!clause_sides_match_join(restrictinfo, outerrel, innerrel))
		{
			have_nonmergeable_joinclause = true;
			continue;			/* 对这些输入关系不行 */
		}

		/*
		 * 坚持每一侧都有一个非冗余的 eclass。
		 * 需要这个限制，是因为规划器的多处代码预期
		 * 归并中的每条子句都能关联到某个规范路径键列表中的路径键，
		 * 但冗余 eclass 不能出现在
		 * 规范排序中。（XXX 也许值得放宽这一点，
		 * 但没有足够时间在 8.3 里处理。）
		 */
		update_mergeclause_eclasses(root, restrictinfo);

		if (EC_MUST_BE_REDUNDANT(restrictinfo->left_ec) ||
			EC_MUST_BE_REDUNDANT(restrictinfo->right_ec))
		{
			have_nonmergeable_joinclause = true;
			continue;			/* 不能处理冗余 eclass */
		}

		result_list = lappend(result_list, restrictinfo);
	}

	/*
	 * 报告是否允许归并连接（见函数顶部的注释）。
	 */
	switch (jointype)
	{
		case JOIN_RIGHT:
		case JOIN_RIGHT_ANTI:
		case JOIN_FULL:
			*mergejoin_allowed = !have_nonmergeable_joinclause;
			break;
		default:
			*mergejoin_allowed = true;
			break;
	}

	return result_list;
}
