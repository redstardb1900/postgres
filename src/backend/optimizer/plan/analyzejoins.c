/*-------------------------------------------------------------------------
 *
 * analyzejoins.c
 *	  初步查询分析之后简化连接的例程
 *
 * 虽然我们在 prep/prepjointree.c 里做了大量连接简化，
 * 某些步骤在那个阶段无法进行，因为缺少
 * 关于查询的详细信息。这里的例程在
 * initsplan.c 完成工作之后被调用，并能根据提取出的信息做额外的连接移除
 * 与简化。代价是
 * 我们修改查询之后必须更费力地收拾现场，
 * 因为派生的数据结构也必须更新。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/analyzejoins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"

/* 局部函数 */
static bool join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo);
static void remove_rel_from_query(PlannerInfo *root, int relid,
								  SpecialJoinInfo *sjinfo);
static void remove_rel_from_restrictinfo(RestrictInfo *rinfo,
										 int relid, int ojrelid);
static void remove_rel_from_eclass(EquivalenceClass *ec,
								   int relid, int ojrelid);
static List *remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved);
static bool rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel);
static bool rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel,
								List *clause_list);
static Oid	distinct_col_search(int colno, List *colnos, List *opids);
static bool is_innerrel_unique_for(PlannerInfo *root,
								   Relids joinrelids,
								   Relids outerrelids,
								   RelOptInfo *innerrel,
								   JoinType jointype,
								   List *restrictlist);


/*
 * remove_useless_joins
 *		检查哪些关系其实根本不必连接，
 *		并把它们从查询中去掉。
 *
 * 传入当前的 joinlist，返回更新后的列表。其他
 * 必须更新的数据结构可通过 "root" 访问。
 */
List *
remove_useless_joins(PlannerInfo *root, List *joinlist)
{
	ListCell   *lc;

	/*
	 * 只关心被左连接的关系，因此可以
	 * 扫描 join_info_list 来轻松找到它们。
	 */
restart:
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		int			nremoved;

		/* 若不可移除就跳过 */
		if (!join_is_removable(root, sjinfo))
			continue;

		/*
		 * 目前，只有 sjinfo 的
		 * 右侧是单个基关系时 join_is_removable 才能成功。从查询和
		 * joinlist 中去掉该关系。
		 */
		innerrelid = bms_singleton_member(sjinfo->min_righthand);

		remove_rel_from_query(root, innerrelid, sjinfo);

		/* 验证恰好从 joinlist 去掉一处引用 */
		nremoved = 0;
		joinlist = remove_rel_from_joinlist(joinlist, innerrelid, &nremoved);
		if (nremoved != 1)
			elog(ERROR, "failed to find relation %d in joinlist", innerrelid);

		/*
		 * 也可以把这个 SpecialJoinInfo 从列表里删掉，因为它
		 * 不再有用。（既然马上就会重新开始 foreach 循环，
		 * 就不费心用 foreach_delete_current。）
		 */
		root->join_info_list = list_delete_cell(root->join_info_list, lc);

		/*
		 * 重新开始扫描。这是必要的，以确保不论
		 * join_info_list 的顺序如何都能找到全部
		 * 可移除连接（注意去掉 attr_needed 位可能使一个连接
		 * 变得可移除，而先前并不可移除）。
		 */
		goto restart;
	}

	return joinlist;
}

/*
 * clause_sides_match_join
 *	  判断一条连接子句的形式是否适合用在这个连接里。
 *
 * 已经知道该子句是只引用
 * 当前连接中那些关系的二元操作子句。这里要检查它是否具有
 * "outerrel_expr op innerrel_expr" 或 "innerrel_expr op outerrel_expr" 的形式，
 * 而不是在任一侧混有外侧和内侧变量。若匹配，
 * 就设置临时标志 outer_is_left，以标明哪一侧是哪一侧。
 */
static inline bool
clause_sides_match_join(RestrictInfo *rinfo, Relids outerrelids,
						Relids innerrelids)
{
	if (bms_is_subset(rinfo->left_relids, outerrelids) &&
		bms_is_subset(rinfo->right_relids, innerrelids))
	{
		/* 左侧是外侧 */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrelids) &&
			 bms_is_subset(rinfo->right_relids, outerrelids))
	{
		/* 右侧是外侧 */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* 对这些输入关系不合适 */
}

/*
 * join_is_removable
 *	  检查是否根本不必做这个特殊连接，因为它
 *	  只会复制其左输入。
 *
 * 对左连接，若连接条件无法匹配
 * 多于一行内层行，就是如此。（还有其他也许有意思的
 * 情形，但没有基础设施去证明它们。）还必须
 * 检查内层不会生成连接之上
 * 需要的任何变量。
 */
static bool
join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo)
{
	int			innerrelid;
	RelOptInfo *innerrel;
	Relids		inputrelids;
	Relids		joinrelids;
	List	   *clause_list = NIL;
	ListCell   *l;
	int			attroff;

	/*
	 * 必须是对单个基关系的左连接，否则没法
	 * 对它做任何事。
	 */
	if (sjinfo->jointype != JOIN_LEFT)
		return false;

	if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
		return false;

	/*
	 * 永远不要试图消除对查询结果关系的左连接。尽管
	 * 在标准 SQL 里这种情形在语法上不可能，MERGE 会建出
	 * 看起来正好如此的连接树。
	 */
	if (innerrelid == root->parse->resultRelation)
		return false;

	innerrel = find_base_rel(root, innerrelid);

	/*
	 * 在费力检查连接之上是否需要任何内层关系变量之前，
	 * 先快速检查，排除那些
	 * 肯定无法证明内层关系唯一性的情形。
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/* 计算正在考虑的这个连接的 relid 集合 */
	inputrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	Assert(sjinfo->ojrelid != 0);
	joinrelids = bms_copy(inputrelids);
	joinrelids = bms_add_member(joinrelids, sjinfo->ojrelid);

	/*
	 * 若任何内层关系属性在连接之上被使用，就不能去掉该连接。
	 * 这里，连接"之上"包括下推条件，因此若
	 * attr_needed 含有该外连接自己的 relid 就应当拒绝；因此
	 * 要与 inputrelids 比较，而不是 joinrelids。
	 *
	 * 作为一种微小优化，从 max_attr 开始
	 * 向下数似乎比从 min_attr 开始向上数更好，依据是
	 * 系统属性不太可能被用到，
	 * 应当最后测试。
	 */
	for (attroff = innerrel->max_attr - innerrel->min_attr;
		 attroff >= 0;
		 attroff--)
	{
		if (!bms_is_subset(innerrel->attr_needed[attroff], inputrelids))
			return false;
	}

	/*
	 * 同样检查内层关系是否被任何将在连接之上使用的 PlaceHolderVars 即 PHVs
	 * 所需要。PHV 的情形稍微
	 * 复杂一些，因为 PHV 可能被指定了一个包含内层关系的 ph_eval_at 位置，
	 * 但其包含的表达式也许并不
	 * 真正引用内层关系（例如它可能只是常量）。
	 * 若这样的 PHV 将在连接之上求值，它就
	 * 不必阻止连接移除。
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_overlap(phinfo->ph_lateral, innerrel->relids))
			return false;		/* 它 LATERAL 引用了内层关系 */
		if (!bms_overlap(phinfo->ph_eval_at, innerrel->relids))
			continue;			/* 它肯定不引用内层关系 */
		if (bms_is_subset(phinfo->ph_needed, inputrelids))
			continue;			/* PHV 不在连接之上使用 */
		if (!bms_is_member(sjinfo->ojrelid, phinfo->ph_eval_at))
			return false;		/* 它必须在连接之下求值 */

		/*
		 * 必须确保去掉连接之后仍有地方求值这个 PHV，
		 * 也就是 ph_eval_at 不会变成空。
		 */
		if (!bms_overlap(sjinfo->min_lefthand, phinfo->ph_eval_at))
			return false;		/* 没有别的地方可以求值 PHV */
		/* 最后再检查所含表达式，因为这有点贵 */
		if (bms_overlap(pull_varnos(root, (Node *) phinfo->ph_var->phexpr),
						innerrel->relids))
			return false;		/* 所含表达式引用了内层关系 */
	}

	/*
	 * 寻找把内层关系约束到
	 * 外侧关系或伪常量的可归并连接子句。若一个操作符
	 * 可归并连接，则它对某个 B-树操作符类表现得像等值，因此
	 * 正是我们要的。可归并连接性测试也会排除
	 * 含有易变函数的子句，那些不能依赖。
	 */
	foreach(l, innerrel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * 若当前连接经由
		 * 外连接恒等式 3 与其他某些外连接可交换，其连接
		 * 子句在 joininfo 列表里会有多个克隆。只想考虑
		 * 这些子句的 has_clone 形式。处理不止一种形式
		 * 会是浪费，而且其中另一些还会搞乱下面的
		 * RINFO_IS_PUSHED_DOWN 测试。
		 */
		if (restrictinfo->is_clone)
			continue;			/* 忽略它 */

		/*
		 * 若它不是这个外连接的连接子句，就不能用它。
		 * 注意若该子句是下推的，那么它在逻辑上来自
		 * 外连接之上，即使它不引用其他关系（例如
		 * 它可能来自 WHERE）。
		 */
		if (RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;			/* 忽略；这里没用 */

		/* 若它不是可归并连接的子句就忽略 */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* 不可归并连接 */

		/*
		 * 检查子句是否具有 "outer op inner" 或 "inner op outer" 的形式，
		 * 若是就标明哪一侧是内侧。
		 */
		if (!clause_sides_match_join(restrictinfo, sjinfo->min_lefthand,
									 innerrel->relids))
			continue;			/* 对这些输入关系不合适 */

		/* 好，加到列表里 */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/*
	 * 既然有了相关的等值连接子句，试着证明
	 * 内层关系是互异的。
	 */
	if (rel_is_distinct_for(root, innerrel, clause_list))
		return true;

	/*
	 * 将来要是能检查建立
	 * 互异性的其他方法就好了。
	 */
	return false;
}


/*
 * 从规划器的数据结构中去掉目标 relid 以及对目标连接的引用，
 * 已经确定查询里不必
 * 包括它们。
 *
 * 这里并不十分彻底。只费心更新规划器的
 * 数据结构中以后真的会查阅的部分。
 */
static void
remove_rel_from_query(PlannerInfo *root, int relid, SpecialJoinInfo *sjinfo)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	int			ojrelid = sjinfo->ojrelid;
	Relids		joinrelids;
	Relids		join_plus_commute;
	List	   *joininfos;
	Index		rti;
	ListCell   *l;

	/* 计算正在考虑的这个连接的 relid 集合 */
	joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	Assert(ojrelid != 0);
	joinrelids = bms_add_member(joinrelids, ojrelid);

	/*
	 * 从其他基关系的 attr_needed 数组中去掉对该关系的引用。
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *otherrel = root->simple_rel_array[rti];
		int			attroff;

		/* 可能有对应于非基关系 RTEs 的空槽 */
		if (otherrel == NULL)
			continue;

		Assert(otherrel->relid == rti); /* 对数组做健全性检查 */

		/* 处理目标关系本身没有意义 */
		if (otherrel == rel)
			continue;

		for (attroff = otherrel->max_attr - otherrel->min_attr;
			 attroff >= 0;
			 attroff--)
		{
			otherrel->attr_needed[attroff] =
				bms_del_member(otherrel->attr_needed[attroff], relid);
			otherrel->attr_needed[attroff] =
				bms_del_member(otherrel->attr_needed[attroff], ojrelid);
		}
	}

	/*
	 * 更新 all_baserels 及相关 relid 集合。
	 */
	root->all_baserels = bms_del_member(root->all_baserels, relid);
	root->outer_join_rels = bms_del_member(root->outer_join_rels, ojrelid);
	root->all_query_rels = bms_del_member(root->all_query_rels, relid);
	root->all_query_rels = bms_del_member(root->all_query_rels, ojrelid);

	/*
	 * 同样从 SpecialJoinInfo 数据结构中去掉引用。
	 *
	 * 若正在删除的外连接嵌在
	 * 其他外连接里面，这就有关系：上层连接的 relid 集合必须调整。目标
	 * 外连接的右侧在这里会被弄成空，但这没关系，
	 * 因为调用方会把那个 SpecialJoinInfo 整个删掉。
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinf = (SpecialJoinInfo *) lfirst(l);

		sjinf->min_lefthand = bms_del_member(sjinf->min_lefthand, relid);
		sjinf->min_righthand = bms_del_member(sjinf->min_righthand, relid);
		sjinf->syn_lefthand = bms_del_member(sjinf->syn_lefthand, relid);
		sjinf->syn_righthand = bms_del_member(sjinf->syn_righthand, relid);
		sjinf->min_lefthand = bms_del_member(sjinf->min_lefthand, ojrelid);
		sjinf->min_righthand = bms_del_member(sjinf->min_righthand, ojrelid);
		sjinf->syn_lefthand = bms_del_member(sjinf->syn_lefthand, ojrelid);
		sjinf->syn_righthand = bms_del_member(sjinf->syn_righthand, ojrelid);
		/* relid 无法出现在这些字段里，但 ojrelid 可以： */
		sjinf->commute_above_l = bms_del_member(sjinf->commute_above_l, ojrelid);
		sjinf->commute_above_r = bms_del_member(sjinf->commute_above_r, ojrelid);
		sjinf->commute_below_l = bms_del_member(sjinf->commute_below_l, ojrelid);
		sjinf->commute_below_r = bms_del_member(sjinf->commute_below_r, ojrelid);
	}

	/*
	 * 同样从 PlaceHolderVar（PHVs）数据结构中去掉引用，
	 * 把任何不再需要的占位符整个去掉。
	 *
	 * 去掉比看起来更棘手：可以去掉在目标关系上和/或连接限定条件里
	 * 使用的 PHV，但不去掉在
	 * 连接伙伴关系上或连接之上使用的那些。要区分
	 * 在伙伴关系上使用的 PHV 和在连接限定条件里使用的那些并不容易，
	 * 因为两者的 ph_needed 集合都会是
	 * joinrelids 的子集。不过，在伙伴关系上使用的 PHV 无法把
	 * 目标关系放在 ph_eval_at 里，因此在决定是
	 * 去掉还是只更新该 PHV 时检查这一点。join_is_removable 里没有对应测试，
	 * 因为它不必区分这些情形。
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		Assert(!bms_is_member(relid, phinfo->ph_lateral));
		if (bms_is_subset(phinfo->ph_needed, joinrelids) &&
			bms_is_member(relid, phinfo->ph_eval_at) &&
			!bms_is_member(ojrelid, phinfo->ph_eval_at))
		{
			root->placeholder_list = foreach_delete_current(root->placeholder_list,
															l);
			root->placeholder_array[phinfo->phid] = NULL;
		}
		else
		{
			PlaceHolderVar *phv = phinfo->ph_var;

			phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, relid);
			phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, ojrelid);
			Assert(!bms_is_empty(phinfo->ph_eval_at));	/* 先前已检查过 */
			phinfo->ph_needed = bms_del_member(phinfo->ph_needed, relid);
			phinfo->ph_needed = bms_del_member(phinfo->ph_needed, ojrelid);
			/* ph_needed 也许会变成空，也许不会 */
			phv->phrels = bms_del_member(phv->phrels, relid);
			phv->phrels = bms_del_member(phv->phrels, ojrelid);
			Assert(!bms_is_empty(phv->phrels));
			Assert(phv->phnullingrels == NULL); /* 不必调整 */
		}
	}

	/*
	 * 从 joininfo 列表中去掉引用该关系的任何连接限定条件。
	 *
	 * 有些情况下，删掉对目标关系的
	 * 引用之后必须把连接限定条件放回去。伪常量和
	 * 外连接延迟的限定条件会这样，它们可能被标成需要该关系，
	 * 以便强迫在该连接处或之上求值。不过不能
	 * 直接丢掉它们。只应当从查询中去掉在逻辑上属于
	 * 被丢弃的那个外连接的限定条件。
	 *
	 * 也许会遇到某个可删除限定条件的克隆，并带有一些
	 * 加上的外连接 relid（见 deconstruct_distribute_oj_quals）。为
	 * 确保也去掉这些克隆，把与这个外连接可交换的全部外连接的 relid
	 * 加进用来测试
	 * 是否下推的那个集合。
	 */
	join_plus_commute = bms_union(joinrelids,
								  sjinfo->commute_above_r);
	join_plus_commute = bms_add_members(join_plus_commute,
										sjinfo->commute_below_l);

	/*
	 * 开始循环之前必须复制该关系原来的 joininfo 列表，
	 * 否则 remove_join_clause_from_rels 会在
	 * 扫描时毁掉这份列表。
	 */
	joininfos = list_copy(rel->joininfo);
	foreach(l, joininfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		remove_join_clause_from_rels(root, rinfo, rinfo->required_relids);

		if (RINFO_IS_PUSHED_DOWN(rinfo, join_plus_commute))
		{
			/*
			 * RestrictInfo 的 relid 集合里也许有对 relid 或 ojrelid 的
			 * 引用，这是因为 PHVs 的
			 * ph_eval_at 集合包含了它们。上面已经检查过
			 * 任何这样的 PHV 都是安全的（并更新了它的 ph_eval_at），因此
			 * 可以直接丢掉那些引用。
			 */
			remove_rel_from_restrictinfo(rinfo, relid, ojrelid);

			/*
			 * 交叉检查该子句本身并不引用
			 * 目标关系或连接。
			 */
#ifdef USE_ASSERT_CHECKING
			{
				Relids		clause_varnos = pull_varnos(root,
														(Node *) rinfo->clause);

				Assert(!bms_is_member(relid, clause_varnos));
				Assert(!bms_is_member(ojrelid, clause_varnos));
			}
#endif
			/* 现在把它扔回 joininfo 列表 */
			distribute_restrictinfo_to_rels(root, rinfo);
		}
	}

	/*
	 * 同样从 EquivalenceClasses 中去掉引用。
	 */
	foreach(l, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(l);

		if (bms_is_member(relid, ec->ec_relids) ||
			bms_is_member(ojrelid, ec->ec_relids))
			remove_rel_from_eclass(ec, relid, ojrelid);
	}

	/*
	 * root->fkey_list 里也许有对该关系的引用，若有，
	 * match_foreign_keys_to_quals() 会去掉它们。
	 */

	/*
	 * 最后，从基关系数组中去掉该关系，以防再次引用它。
	 * （不能更早做这件事，因为
	 * remove_join_clause_from_rels 还会碰到它。）
	 */
	root->simple_rel_array[relid] = NULL;

	/* 并且清掉这个 RelOptInfo，以防还有另一条访问路径 */
	pfree(rel);
}

/*
 * 从 RestrictInfo 中去掉对 relid 或 ojrelid 的任何引用。
 *
 * 只费心清掉 clause_relids 和 required_relids 里的位，
 * 不去掉所含 Var 和 PHVs 里的 nullingrel 位。（这一点
 * 也许以后得改进。）不过，若 RestrictInfo 含有 OR 子句，
 * 还必须清理子子句。
 */
static void
remove_rel_from_restrictinfo(RestrictInfo *rinfo, int relid, int ojrelid)
{
	/*
	 * clause_relids 大概不与别的东西共享，但
	 * 还是复制一份以求稳妥。
	 */
	rinfo->clause_relids = bms_copy(rinfo->clause_relids);
	rinfo->clause_relids = bms_del_member(rinfo->clause_relids, relid);
	rinfo->clause_relids = bms_del_member(rinfo->clause_relids, ojrelid);
	/* required_relids 同样处理 */
	rinfo->required_relids = bms_copy(rinfo->required_relids);
	rinfo->required_relids = bms_del_member(rinfo->required_relids, relid);
	rinfo->required_relids = bms_del_member(rinfo->required_relids, ojrelid);

	/* 若是 OR，就递归清理子子句 */
	if (restriction_is_or_clause(rinfo))
	{
		ListCell   *lc;

		Assert(is_orclause(rinfo->orclause));
		foreach(lc, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(lc);

			/* OR 的参数应当是 ANDs 或子 RestrictInfos */
			if (is_andclause(orarg))
			{
				List	   *andargs = ((BoolExpr *) orarg)->args;
				ListCell   *lc2;

				foreach(lc2, andargs)
				{
					RestrictInfo *rinfo2 = lfirst_node(RestrictInfo, lc2);

					remove_rel_from_restrictinfo(rinfo2, relid, ojrelid);
				}
			}
			else
			{
				RestrictInfo *rinfo2 = castNode(RestrictInfo, orarg);

				remove_rel_from_restrictinfo(rinfo2, relid, ojrelid);
			}
		}
	}
}

/*
 * 从 EquivalenceClass 中去掉对 relid 或 ojrelid 的任何引用。
 *
 * 与 remove_rel_from_restrictinfo 一样，不操心清掉
 * 所含 Var 和 PHVs 里的 nullingrel 位。（这一点
 * 也许以后得改进。）确实需要修正等价类和等价成员的 relid 集合，以确保
 * 隐含的连接等值会在适当的连接
 * 层次生成。
 */
static void
remove_rel_from_eclass(EquivalenceClass *ec, int relid, int ojrelid)
{
	ListCell   *lc;

	/* 修正等价类的整体 relid */
	ec->ec_relids = bms_del_member(ec->ec_relids, relid);
	ec->ec_relids = bms_del_member(ec->ec_relids, ojrelid);

	/*
	 * 修正成员表达式。任何以
	 * 空 em_relids 结尾的非常量成员必定是被去掉关系的 Var 或 PHV。不再
	 * 需要它，因此可以丢掉。
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (bms_is_member(relid, cur_em->em_relids) ||
			bms_is_member(ojrelid, cur_em->em_relids))
		{
			Assert(!cur_em->em_is_const);
			cur_em->em_relids = bms_del_member(cur_em->em_relids, relid);
			cur_em->em_relids = bms_del_member(cur_em->em_relids, ojrelid);
			if (bms_is_empty(cur_em->em_relids))
				ec->ec_members = foreach_delete_current(ec->ec_members, lc);
		}
	}

	/* 修正源子句，以防以后能复用它们 */
	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		remove_rel_from_restrictinfo(rinfo, relid, ojrelid);
	}

	/*
	 * 与其花代码去修正任何已经推导的子句，干脆
	 * 丢掉它们。（此时，任何这样的子句都会是基限制
	 * 子句，反正也不再需要。）
	 */
	ec->ec_derives = NIL;
}

/*
 * 从 joinlist 结构中去掉目标 relid 的任何出现。
 *
 * 建一份全新的列表结构最容易，因此就
 * 这样处理。这里效率不是大事。
 *
 * *nremoved 按去掉的出现次数递增（应当
 * 恰好是一次，但调用方会检查）。
 */
static List *
remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved)
{
	List	   *result = NIL;
	ListCell   *jl;

	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			if (varno == relid)
				(*nremoved)++;
			else
				result = lappend(result, jlnode);
		}
		else if (IsA(jlnode, List))
		{
			/* 递归处理子问题 */
			List	   *sublist;

			sublist = remove_rel_from_joinlist((List *) jlnode,
											   relid, nremoved);
			/* 避免把空的子列表放进结果 */
			if (sublist)
				result = lappend(result, sublist);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
		}
	}

	return result;
}


/*
 * reduce_unique_semijoins
 *		检查哪些半连接可以简化为普通内连接，
 *		因为对连接子句而言内层关系可证明是唯一的。
 *
 * 理想情况下这会发生在 reduce_outer_joins 期间，但那时
 * 信息不够。
 *
 * 要在适用时做强度归约，只需从 root->join_info_list 中删除
 * 该半连接的 SpecialJoinInfo。（不
 * 费力去修正查询 jointree 里记给它的连接类型，
 * 因为那不会再被查阅。）
 */
void
reduce_unique_semijoins(PlannerInfo *root)
{
	ListCell   *lc;

	/*
	 * 扫描 join_info_list 以寻找半连接。
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		RelOptInfo *innerrel;
		Relids		joinrelids;
		List	   *restrictlist;

		/*
		 * 必须是对单个基关系的半连接，否则没法
		 * 对它做任何事。
		 */
		if (sjinfo->jointype != JOIN_SEMI)
			continue;

		if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
			continue;

		innerrel = find_base_rel(root, innerrelid);

		/*
		 * 在费力跑 generate_join_implied_equalities 之前，先
		 * 快速检查，排除那些肯定无法
		 * 证明内层关系唯一性的情形。
		 */
		if (!rel_supports_distinctness(root, innerrel))
			continue;

		/* 计算正在考虑的这个连接的 relid 集合 */
		joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
		Assert(sjinfo->ojrelid == 0);	/* SEMI 连接没有 RT 下标 */

		/*
		 * 由于只考虑单关系的右侧，它拥有的任何连接子句
		 * 都必须是把它连到该半连接 min_lefthand 的子句。也可以
		 * 考虑由等价类推导的连接子句。
		 */
		restrictlist =
			list_concat(generate_join_implied_equalities(root,
														 joinrelids,
														 sjinfo->min_lefthand,
														 innerrel,
														 NULL),
						innerrel->joininfo);

		/* 测试内层关系对这些子句是否唯一。 */
		if (!innerrel_is_unique(root,
								joinrelids, sjinfo->min_lefthand, innerrel,
								JOIN_SEMI, restrictlist, true))
			continue;

		/* 好，把这个 SpecialJoinInfo 从列表中去掉。 */
		root->join_info_list = foreach_delete_current(root->join_info_list, lc);
	}
}


/*
 * rel_supports_distinctness
 *		该关系有没有可能在某一组列上被证明为互异？
 *
 * 这实际上是 rel_is_distinct_for() 的预检查函数。
 * 若 rel_is_distinct_for() 对该关系有可能返回真，它必须返回真，
 * 但不应花很多周期。想法是
 * 若这次调用不可能
 * 成功，调用方就可以避免做可能很贵的处理来计算
 * rel_is_distinct_for() 的参数列表。
 */
static bool
rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel)
{
	/* 只了解基关系…… */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * 对普通关系，只知道如何通过
		 * 引用唯一索引来证明唯一性。确保至少有一个
		 * 合适的唯一索引。它必须立即强制，并且不是
		 * 部分索引。（让这些条件与
		 * relation_has_unique_index_for 保持同步！）
		 */
		ListCell   *lc;

		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

			if (ind->unique && ind->immediate && ind->indpred == NIL)
				return true;
		}
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Query	   *subquery = root->simple_rte_array[rel->relid]->subquery;

		/* 检查子查询是否有任何支持互异性的性质 */
		if (query_supports_distinctness(subquery))
			return true;
	}
	/* 对任何其他 rte 种类没有证明规则。 */
	return false;
}

/*
 * rel_is_distinct_for
 *		按 clause_list，该关系是否只返回互异行？
 *
 * clause_list 是涉及该关系与
 * 另一个关系的连接限制子句列表。若该关系发出的任意两行都不可能
 * 连接到另一个关系的同一行，就返回真。
 *
 * 调用方必须已经确定每个条件都是
 * 可归并连接的等值，一侧是该关系中的表达式，
 * 另一侧是不涉及该关系的表达式。临时
 * 标志 outer_is_left 用来标明哪一侧引用该关系：
 * 若 outer_is_left 为假则是左侧，为真则是右侧。
 *
 * 注意传入的 clause_list 可能被破坏性修改！对
 * 目前的用途这没问题，因为 clause_list 是调用方专为
 * 传给本函数而建的。
 */
static bool
rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel, List *clause_list)
{
	/*
	 * 若假定所有调用方都先检查了
	 * rel_supports_distinctness，这里可以跳过几项测试，但似乎不值得为此
	 * 承担任何风险。
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * 检查索引，看是否有匹配的唯一索引。
		 * relation_has_unique_index_for 会自动加上该关系任何可用的
		 * 限制子句，因此这里不必做那件事。
		 */
		if (relation_has_unique_index_for(root, rel, clause_list, NIL, NIL))
			return true;
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Index		relid = rel->relid;
		Query	   *subquery = root->simple_rte_array[relid]->subquery;
		List	   *colnos = NIL;
		List	   *opids = NIL;
		ListCell   *l;

		/*
		 * 为 query_is_distinct_for 建立参数列表：查询需要在其上互异的
		 * 输出列号列表，以及
		 * 输出列需要按其互异的
		 * 等值操作符列表。
		 *
		 * （XXX 没有考虑附在
		 * 子查询上的限制子句；值得做吗？）
		 */
		foreach(l, clause_list)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
			Oid			op;
			Var		   *var;

			/*
			 * 取得用于判断唯一性所依据的等值操作符。
			 * （这也许是跨类型操作符，因而并不正好是
			 * 子查询会考虑的同一个操作符；这没关系，
			 * 因为 query_is_distinct_for 能处理这类情形。）
			 * 调用方的可归并连接性测试应当只选出了
			 * OpExprs。
			 */
			op = castNode(OpExpr, rinfo->clause)->opno;

			/* 调用方已经为我们标明了内侧 */
			if (rinfo->outer_is_left)
				var = (Var *) get_rightop(rinfo->clause);
			else
				var = (Var *) get_leftop(rinfo->clause);

			/*
			 * 可以忽略操作数之上的任何 RelabelType 节点。（不会
			 * 多于一个，因为已经应用过
			 * eval_const_expressions()。）
			 */
			if (var && IsA(var, RelabelType))
				var = (Var *) ((RelabelType *) var)->arg;

			/*
			 * 若内侧不是引用子查询输出列的 Var，
			 * 这条子句对我们就没有帮助。
			 */
			if (!var || !IsA(var, Var) ||
				var->varno != relid || var->varlevelsup != 0)
				continue;

			colnos = lappend_int(colnos, var->varattno);
			opids = lappend_oid(opids, op);
		}

		if (query_is_distinct_for(subquery, colnos, opids))
			return true;
	}
	return false;
}


/*
 * query_supports_distinctness - 该查询有没有可能在
 *		某一组输出列上被证明为互异？
 *
 * 这实际上是 query_is_distinct_for() 的预检查函数。
 * 若 query_is_distinct_for() 对该查询有可能返回真，它必须返回真，
 * 但不应花很多周期。想法是
 * 若这次调用不可能
 * 成功，调用方就可以避免做可能很贵的处理来计算
 * query_is_distinct_for() 的参数列表。
 */
bool
query_supports_distinctness(Query *query)
{
	/* 除了带 DISTINCT 的情形，SRFs 会破坏互异性，见下 */
	if (query->hasTargetSRFs && query->distinctClause == NIL)
		return false;

	/* 检查能用来证明互异性的特性 */
	if (query->distinctClause != NIL ||
		query->groupClause != NIL ||
		query->groupingSets != NIL ||
		query->hasAggs ||
		query->havingQual ||
		query->setOperations)
		return true;

	return false;
}

/*
 * query_is_distinct_for - 查询是否从不返回指定
 *		列的重复行？
 *
 * query 是尚未规划的子查询（按目前用法，它总是来自
 * 子查询 RTE，规划器避免在上面乱写）。
 *
 * colnos 是输出列号（resno）的整数列表。我们关心
 * 只由这些列组成的行是否一定
 * 互异。"互异"的定义取决于
 * opids 里列出的对应上层等值操作符是否认为
 * 这些值互异。（注意：opids 项可能是跨类型
 * 操作符，因而并不正好是子查询
 * 自己会用的等值操作符。我们用 equality_ops_are_compatible() 检查
 * 兼容性。它看的是 B-树或哈希操作符族成员资格，因此
 * 对我们这里可能要处理的全部操作符
 * 都应当给出可信的答案。）
 */
bool
query_is_distinct_for(Query *query, List *colnos, List *opids)
{
	ListCell   *l;
	Oid			opid;

	Assert(list_length(colnos) == list_length(opids));

	/*
	 * DISTINCT（包括 DISTINCT ON）保证唯一性，若 DISTINCT 子句里的全部
	 * 列都出现在 colnos 中并且操作符语义
	 * 匹配。即使 DISTINCT 列里或
	 * 目标列表别处有 SRFs，这也成立。
	 */
	if (query->distinctClause)
	{
		foreach(l, query->distinctClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;			/* 若不匹配就提前退出 */
		}
		if (l == NULL)			/* 是否全部都匹配了？ */
			return true;
	}

	/*
	 * 否则，查询目标列表里的集合返回函数可能
	 * 导致返回重复行，尽管目标列表求值之前也许
	 * 发生过分组。（若全部目标列表 SRFs 都在 GROUP BY
	 * 列里，那会是安全的，因为它们会在分组之前展开。
	 * 但目前似乎不值得费力去检查这一点。）
	 */
	if (query->hasTargetSRFs)
		return false;

	/*
	 * 同样，没有 GROUPING SETS 的 GROUP BY 保证唯一性，若全部
	 * 分组列都出现在 colnos 中并且操作符语义匹配。
	 */
	if (query->groupClause && !query->groupingSets)
	{
		foreach(l, query->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;			/* 若不匹配就提前退出 */
		}
		if (l == NULL)			/* 是否全部都匹配了？ */
			return true;
	}
	else if (query->groupingSets)
	{
		/*
		 * 若分组集带有表达式，大概就没有
		 * 唯一性，而且分析会很难。放弃。
		 */
		if (query->groupClause)
			return false;

		/*
		 * 若没有 groupClause（因此没有分组表达式），
		 * 也许有一个或许多空分组集。若只有一个，
		 * 就只返回一行，当然是唯一的。但
		 * 否则，就知道肯定不唯一。
		 */
		if (list_length(query->groupingSets) == 1 &&
			((GroupingSet *) linitial(query->groupingSets))->kind == GROUPING_SET_EMPTY)
			return true;
		else
			return false;
	}
	else
	{
		/*
		 * 若没有 GROUP BY，但有聚合或 HAVING，那么
		 * 结果至多一行，因此对任何操作符都肯定唯一。
		 */
		if (query->hasAggs || query->havingQual)
			return true;
	}

	/*
	 * UNION、INTERSECT、EXCEPT 保证整个输出行唯一，
	 * 带 ALL 时除外。
	 */
	if (query->setOperations)
	{
		SetOperationStmt *topop = castNode(SetOperationStmt, query->setOperations);

		Assert(topop->op != SETOP_NONE);

		if (!topop->all)
		{
			ListCell   *lg;

			/* 若全部非废输出列都在 colnos 里，就可以 */
			lg = list_head(topop->groupClauses);
			foreach(l, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				SortGroupClause *sgc;

				if (tle->resjunk)
					continue;	/* 忽略 resjunk 列 */

				/* 非 resjunk 列应当有分组子句 */
				Assert(lg != NULL);
				sgc = (SortGroupClause *) lfirst(lg);
				lg = lnext(topop->groupClauses, lg);

				opid = distinct_col_search(tle->resno, colnos, opids);
				if (!OidIsValid(opid) ||
					!equality_ops_are_compatible(opid, sgc->eqop))
					break;		/* 若不匹配就提前退出 */
			}
			if (l == NULL)		/* 是否全部都匹配了？ */
				return true;
		}
	}

	/*
	 * XXX 还有没有其他情形可以轻易看出结果
	 * 必定互异？
	 *
	 * 若确实给本函数增加更多判断，务必同步更新
	 * query_supports_distinctness()。
	 */

	return false;
}

/*
 * distinct_col_search - query_is_distinct_for 的子程序
 *
 * 若 colno 在 colnos 里，就返回 opids 的对应元素，
 * 否则返回 InvalidOid。（通常 colnos 不会含有重复，
 * 但若有，就任意选第一个匹配。）
 */
static Oid
distinct_col_search(int colno, List *colnos, List *opids)
{
	ListCell   *lc1,
			   *lc2;

	forboth(lc1, colnos, lc2, opids)
	{
		if (colno == lfirst_int(lc1))
			return lfirst_oid(lc2);
	}
	return InvalidOid;
}


/*
 * innerrel_is_unique
 *	  检查 innerrel 是否可证明对 outerrel 的任何
 *	  元组至多含有一个匹配元组，依据是 'restrictlist' 里的连接子句。
 *
 * 内层关系必须有一个实际的 RelOptInfo，但用 Relids
 * 标识外层关系就够了。这种不对称支持在
 * 连接关系建好之前使用本函数。（调用方预期还会
 * 提供 joinrelids，只是为了省得再算一次。）
 *
 * 证明必须只基于执行时将成为 "joinquals"
 * 而不是 "otherquals" 的子句。对内连接没有
 * 差别；但若连接是外连接，必须忽略下推限定条件，
 * 因为那些会变成 "otherquals"。注意这意味着答案可能
 * 随 IS_OUTER_JOIN(jointype) 而变；由于缓存答案时
 * 不管这一点，调用方必须小心，不要用会被 IS_OUTER_JOIN()
 * 分成不同类的 jointype 来调用本函数。
 *
 * 实际证明由 is_innerrel_unique_for() 承担；本函数
 * 是主要负责缓存答案的前端。
 * 尤其是，force_cache 参数允许覆盖内部
 * 关于是否缓存否定答案的启发式；若这次询问
 * 不属于正常的自底向上连接搜索
 * 序列，它应当为 "true"。
 */
bool
innerrel_is_unique(PlannerInfo *root,
				   Relids joinrelids,
				   Relids outerrelids,
				   RelOptInfo *innerrel,
				   JoinType jointype,
				   List *restrictlist,
				   bool force_cache)
{
	MemoryContext old_context;
	ListCell   *lc;

	/* 没有连接子句时，肯定不能证明唯一性 */
	if (restrictlist == NIL)
		return false;

	/*
	 * 先快速检查，排除那些肯定无法
	 * 证明内层关系唯一性的情形。
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/*
	 * 查询缓存，看是否已经证明 innerrel 对
	 * 这个 outerrel 的任何子集都是唯一的。不必精确匹配，
	 * 因为额外的外层关系不能使内层关系变得更不唯一（或者更
	 * 形式地说，连接到超集外层关系的 restrictlist 必须是
	 * 先前成功使用的条件的超集）。
	 */
	foreach(lc, innerrel->unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		if (bms_is_subset(unique_for_rels, outerrelids))
			return true;		/* 成功！ */
	}

	/*
	 * 反过来，也许已经确定这个 outerrel，或它的某个
	 * 超集，无法证明这个 innerrel 是唯一的。
	 */
	foreach(lc, innerrel->non_unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		if (bms_is_subset(outerrelids, unique_for_rels))
			return false;
	}

	/* 没有缓存信息，因此试着做证明。 */
	if (is_innerrel_unique_for(root, joinrelids, outerrelids, innerrel,
							   jointype, restrictlist))
	{
		/*
		 * 把肯定的结果缓存起来供以后探测，即使正在做 GEQO 也务必把它
		 * 留在 planner_cxt 里。
		 *
		 * 注意：也许会考虑试着分离出
		 * 证明内层关系唯一的那部分最小外层关系子集。但不值得
		 * 费这个事，因为规划器会逐步建立连接关系，
		 * 因此在任何
		 * 超集之前就会看到最小充分的外层关系。
		 */
		old_context = MemoryContextSwitchTo(root->planner_cxt);
		innerrel->unique_for_rels = lappend(innerrel->unique_for_rels,
											bms_copy(outerrelids));
		MemoryContextSwitchTo(old_context);

		return true;			/* 成功！ */
	}
	else
	{
		/*
		 * outerrel 的连接条件都没能证明 innerrel 唯一，因此
		 * 可以安全地拒绝这个 outerrel，或其任何子集，
		 * 供以后的检查使用。
		 *
		 * 不过，在正常规划模式下，缓存这份知识完全
		 * 没有意义；它不会再被查询，因为连接关系是从
		 * 较小建到较大。它在 GEQO 模式下有用，那时
		 * 知识可以带到相继的规划尝试中；而且
		 * 使用连接搜索插件时也很可能有用。因此
		 * 当 join_search_private 非 NULL 时缓存。（是的，这是个权宜之计，
		 * 但似乎合理。）
		 *
		 * 另外，允许调用方覆盖该启发式并强制缓存；
		 * 这对 reduce_unique_semijoins 有用，它在
		 * 正常连接搜索开始之前就调用到这里。
		 */
		if (force_cache || root->join_search_private)
		{
			old_context = MemoryContextSwitchTo(root->planner_cxt);
			innerrel->non_unique_for_rels =
				lappend(innerrel->non_unique_for_rels,
						bms_copy(outerrelids));
			MemoryContextSwitchTo(old_context);
		}

		return false;
	}
}

/*
 * is_innerrel_unique_for
 *	  检查 innerrel 是否可证明对 outerrel 的任何
 *	  元组至多含有一个匹配元组，依据是 'restrictlist' 里的连接子句。
 */
static bool
is_innerrel_unique_for(PlannerInfo *root,
					   Relids joinrelids,
					   Relids outerrelids,
					   RelOptInfo *innerrel,
					   JoinType jointype,
					   List *restrictlist)
{
	List	   *clause_list = NIL;
	ListCell   *lc;

	/*
	 * 寻找把内层关系约束到
	 * 外层关系的可归并连接子句。若一个操作符可归并连接，则它对某个 B-树操作符类表现得像
	 * 等值，因此正是我们要的。
	 * 可归并连接性测试也会排除含有易变
	 * 函数的子句，那些不能依赖。
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * 如上所述，若它是下推子句并且我们处在外
		 * 连接，就不能用它。
		 */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;

		/* 若它不是可归并连接的子句就忽略 */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* 不可归并连接 */

		/*
		 * 检查子句是否具有 "outer op inner" 或 "inner op outer" 的形式，
		 * 若是就标明哪一侧是内侧。
		 */
		if (!clause_sides_match_join(restrictinfo, outerrelids,
									 innerrel->relids))
			continue;			/* 对这些输入关系不合适 */

		/* 好，加到列表里 */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/* 让 rel_is_distinct_for() 做难的部分 */
	return rel_is_distinct_for(root, innerrel, clause_list);
}
