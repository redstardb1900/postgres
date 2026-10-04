/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/joinrels.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/appendinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "partitioning/partbounds.h"
#include "utils/memutils.h"


static void make_rels_by_clause_joins(PlannerInfo *root,
									  RelOptInfo *old_rel,
									  List *other_rels_list,
									  ListCell *other_rels);
static void make_rels_by_clauseless_joins(PlannerInfo *root,
										  RelOptInfo *old_rel,
										  List *other_rels);
static bool has_join_restriction(PlannerInfo *root, RelOptInfo *rel);
static bool has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel);
static bool restriction_is_constant_false(List *restrictlist,
										  RelOptInfo *joinrel,
										  bool only_pushed_down);
static void populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
										RelOptInfo *rel2, RelOptInfo *joinrel,
										SpecialJoinInfo *sjinfo, List *restrictlist);
static void try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1,
								   RelOptInfo *rel2, RelOptInfo *joinrel,
								   SpecialJoinInfo *parent_sjinfo,
								   List *parent_restrictlist);
static SpecialJoinInfo *build_child_join_sjinfo(PlannerInfo *root,
												SpecialJoinInfo *parent_sjinfo,
												Relids left_relids, Relids right_relids);
static void compute_partition_bounds(PlannerInfo *root, RelOptInfo *rel1,
									 RelOptInfo *rel2, RelOptInfo *joinrel,
									 SpecialJoinInfo *parent_sjinfo,
									 List **parts1, List **parts2);
static void get_matching_part_pairs(PlannerInfo *root, RelOptInfo *joinrel,
									RelOptInfo *rel1, RelOptInfo *rel2,
									List **parts1, List **parts2);


/*
 * join_search_one_level
 *	  考虑如何产生恰好包含 'level'
 *	  个连接树项的连接关系。（这是动态规划方法中的一步，
 *	  该方法体现在 standard_join_search 里。）每个可行的
 *	  较低层关系组合都会创建连接关系节点，并放进列表返回。
 *	  每个这样的连接关系也会创建实现路径。
 *
 * level：这次要构造的关系所在的层
 * root->join_rel_level[j]（1 <= j < level）是包含 j 个项的关系列表
 *
 * 结果放在 root->join_rel_level[level] 中。
 */
void
join_search_one_level(PlannerInfo *root, int level)
{
	List	  **joinrels = root->join_rel_level;
	ListCell   *r;
	int			k;

	Assert(joinrels[level] == NIL);

	/* 设置 join_cur_level，使新连接关系加入正确的列表 */
	root->join_cur_level = level;

	/*
	 * 首先考虑左深和右深计划：恰好含
	 * level-1 个成员关系的关系，与初始关系相连接。
	 * 我们优先使用连接子句来连接，但若发现某个含 level-1
	 * 个成员的关系没有连接子句，就对所有尚未包含在其中的初始关系
	 * 生成笛卡尔积连接。
	 */
	foreach(r, joinrels[level - 1])
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

		if (old_rel->joininfo != NIL || old_rel->has_eclass_joins ||
			has_join_restriction(root, old_rel))
		{
			/*
			 * 存在与这个关系相关的连接子句或连接顺序限制，
			 * 因此只考虑它与那些
			 * 由子句或限制连到它的初始关系之间的连接。
			 *
			 * 在第 2 层这个条件是对称的，因此不必
			 * 看列表里位于它之前的初始关系；当我们
			 * 还在更早的关系上时，已经
			 * 考虑过这类连接。（镜像连接由 make_join_rel 自动处理。）
			 * 在以后的趟（level > 2）中，把上一层的关系
			 * 连到每个它们尚未包含、但与之有连接
			 * 子句或限制的初始关系。
			 */
			List	   *other_rels_list;
			ListCell   *other_rels;

			if (level == 2)		/* 考虑其余的初始关系 */
			{
				other_rels_list = joinrels[level - 1];
				other_rels = lnext(other_rels_list, r);
			}
			else				/* 考虑全部初始关系 */
			{
				other_rels_list = joinrels[1];
				other_rels = list_head(other_rels_list);
			}

			make_rels_by_clause_joins(root,
									  old_rel,
									  other_rels_list,
									  other_rels);
		}
		else
		{
			/*
			 * 哎呀，有一个关系没有连到任何其他
			 * 关系，无论是直接的还是经由连接顺序限制。
			 * 该做笛卡尔积了。
			 *
			 * 我们考虑与每个尚未包含的
			 * 初始关系做笛卡尔积，无论它有没有其他连接子句。在
			 * 第 2 层，若有两个或更多无子句的初始关系，我们
			 * 会冗余地考虑从两个方向连接它们；但
			 * 这类情形不够常见，不值得增加复杂性来
			 * 避免重复劳动。
			 */
			make_rels_by_clauseless_joins(root,
										  old_rel,
										  joinrels[1]);
		}
	}

	/*
	 * 现在考虑 "丛状计划"：含 k 个初始关系的关系
	 * 与含 level-k 个初始关系的关系相连接，2 <= k <= level-2。
	 *
	 * 只对存在合适连接子句（或连接顺序限制）的关系对考虑丛状连接，
	 * 以免
	 * 规划时间不合理地增长。
	 */
	for (k = 2;; k++)
	{
		int			other_level = level - k;

		/*
		 * 由于 make_join_rel(x, y) 同时处理 x,y 和 y,x，我们只需
		 * 走到中点即可。
		 */
		if (k > other_level)
			break;

		foreach(r, joinrels[k])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			List	   *other_rels_list;
			ListCell   *other_rels;
			ListCell   *r2;

			/*
			 * 这里可以忽略没有连接子句的关系，除非它们
			 * 参与了连接顺序限制---那时我们也许不得不
			 * 强制采用丛状连接计划。
			 */
			if (old_rel->joininfo == NIL && !old_rel->has_eclass_joins &&
				!has_join_restriction(root, old_rel))
				continue;

			if (k == other_level)
			{
				/* 只考虑其余关系 */
				other_rels_list = joinrels[k];
				other_rels = lnext(other_rels_list, r);
			}
			else
			{
				other_rels_list = joinrels[other_level];
				other_rels = list_head(other_rels_list);
			}

			for_each_cell(r2, other_rels_list, other_rels)
			{
				RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);

				if (!bms_overlap(old_rel->relids, new_rel->relids))
				{
					/*
					 * 可以用这一
					 * 对关系构造出层数正确的关系。若至少有一条相关的
					 * 连接子句或连接顺序限制，就这样做。
					 */
					if (have_relevant_joinclause(root, old_rel, new_rel) ||
						have_join_order_restriction(root, old_rel, new_rel))
					{
						(void) make_join_rel(root, old_rel, new_rel);
					}
				}
			}
		}
	}

	/*----------
	 * Last-ditch effort: if we failed to find any usable joins so far, force
	 * a set of cartesian-product joins to be generated.  This handles the
	 * special case where all the available rels have join clauses but we
	 * cannot use any of those clauses yet.  This can only happen when we are
	 * considering a join sub-problem (a sub-joinlist) and all the rels in the
	 * sub-problem have only join clauses with rels outside the sub-problem.
	 * An example is
	 *
	 *		SELECT ... FROM a INNER JOIN b ON TRUE, c, d, ...
	 *		WHERE a.w = c.x and b.y = d.z;
	 *
	 * If the "a INNER JOIN b" sub-problem does not get flattened into the
	 * upper level, we must be willing to make a cartesian join of a and b;
	 * but the code above will not have done so, because it thought that both
	 * a and b have joinclauses.  We consider only left-sided and right-sided
	 * cartesian joins in this case (no bushy).
	 *----------
	 */
	if (joinrels[level] == NIL)
	{
		/*
		 * 这个循环和第一个一样，只是我们总是调用
		 * make_rels_by_clauseless_joins()。
		 */
		foreach(r, joinrels[level - 1])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

			make_rels_by_clauseless_joins(root,
										  old_rel,
										  joinrels[1]);
		}

		/*----------
		 * When special joins are involved, there may be no legal way
		 * to make an N-way join for some values of N.  For example consider
		 *
		 * SELECT ... FROM t1 WHERE
		 *	 x IN (SELECT ... FROM t2,t3 WHERE ...) AND
		 *	 y IN (SELECT ... FROM t4,t5 WHERE ...)
		 *
		 * We will flatten this query to a 5-way join problem, but there are
		 * no 4-way joins that join_is_legal() will consider legal.  We have
		 * to accept failure at level 4 and go on to discover a workable
		 * bushy plan at level 5.
		 *
		 * However, if there are no special joins and no lateral references
		 * then join_is_legal() should never fail, and so the following sanity
		 * check is useful.
		 *----------
		 */
		if (joinrels[level] == NIL &&
			root->join_info_list == NIL &&
			!root->hasLateralRTEs)
			elog(ERROR, "failed to build any %d-way joins", level);
	}
}

/*
 * make_rels_by_clause_joins
 *	  在给定关系 'old_rel' 与其他关系之间建立连接，
 *	  那些其他关系参与了 'old_rel' 也参与的连接子句
 *	  （或与它有连接顺序限制）。
 *	  连接关系放在 root->join_rel_level[join_cur_level] 中。
 *
 * 注意：在高于 2 的层上，同一连接关系会以
 * 多种方式生成---例如 (a join b) join c 与
 * (b join c) join a 是同一个 RelOptInfo，尽管第二种会向它加入
 * 一组不同的 Path。这就是使用 join_rel_level 机制的原因，它
 * 自动保证每个新连接关系只加入列表一次。
 *
 * 'old_rel' 是待连接关系的关系项
 * 'other_rels_list'：包含其他
 * 待考虑连接的关系的列表
 * 'other_rels'：第一个要考虑的单元
 *
 * 目前这只用于 other_rels 中的初始关系，但它
 * 对连接到连接关系也同样有效。
 */
static void
make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  List *other_rels_list,
						  ListCell *other_rels)
{
	ListCell   *l;

	for_each_cell(l, other_rels_list, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(old_rel->relids, other_rel->relids) &&
			(have_relevant_joinclause(root, old_rel, other_rel) ||
			 have_join_order_restriction(root, old_rel, other_rel)))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}

/*
 * make_rels_by_clauseless_joins
 *	  给定关系 'old_rel' 以及其他关系的列表
 *	  'other_rels'，在 'old_rel' 与 'other_rels' 中每个
 *	  尚未包含在 'old_rel' 里的成员之间创建连接关系。
 *	  连接关系放在 root->join_rel_level[join_cur_level] 中。
 *
 * 'old_rel' 是待连接关系的关系项
 * 'other_rels'：包含其他待考虑连接的关系的列表
 *
 * 目前这只用于 other_rels 中的初始关系，但它
 * 对连接到连接关系也同样有效。
 */
static void
make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  List *other_rels)
{
	ListCell   *l;

	foreach(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(other_rel->relids, old_rel->relids))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}


/*
 * join_is_legal
 *	   根据查询的连接顺序约束，判断提议的连接是否合法；
 *	   若合法，再确定连接类型。
 *
 * 调用方不仅要提供两个关系，还要提供它们 relids 的并集。
 * （可以在本地计算 joinrelids 来简化 API，但在
 * 经由 make_join_rel 的正常路径上，
 * 这会是重复工作。
 * 注意这个值不包括此处也许要执行的任何外连接的 RT 下标，
 * 因此它不是该连接关系的规范标识。）
 *
 * 成功时，若这应是普通内连接，则 *sjinfo_p 设为 NULL；
 * 否则设为指向关联的 SpecialJoinInfo 节点。另外，
 * 若给定关系需要交换才能
 * 与该 SpecialJoinInfo 节点匹配，则把 *reversed_p 设为 true。
 */
static bool
join_is_legal(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
			  Relids joinrelids,
			  SpecialJoinInfo **sjinfo_p, bool *reversed_p)
{
	SpecialJoinInfo *match_sjinfo;
	bool		reversed;
	bool		unique_ified;
	bool		must_be_leftjoin;
	ListCell   *l;

	/*
	 * 失败返回时也要设置输出参数。这只是为了
	 * 消除过于挑剔的编译器关于未初始化变量的告警。
	 */
	*sjinfo_p = NULL;
	*reversed_p = false;

	/*
	 * 若存在任何特殊连接，提议的连接也许非法；而且
	 * 无论如何都要确定它的连接类型。扫描连接信息
	 * 列表，寻找匹配与冲突。
	 */
	match_sjinfo = NULL;
	reversed = false;
	unique_ified = false;
	must_be_leftjoin = false;

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/*
		 * 除非这个特殊连接的 RHS 与
		 * 提议的连接重叠，否则它不相关。（先做这个检查，以便快速排除
		 * 大多数无关的 SJ。）
		 */
		if (!bms_overlap(sjinfo->min_righthand, joinrelids))
			continue;

		/*
		 * 若提议的连接完全包含在 RHS 之内，也不相关
		 * （也就是说，我们还在构造 RHS）。
		 */
		if (bms_is_subset(joinrelids, sjinfo->min_righthand))
			continue;

		/*
		 * 若 SJ 已在任一输入之内完成，也不相关。
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
			continue;
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
			continue;

		/*
		 * 若它是半连接，且我们已经把 RHS 连到了任一输入中的
		 * 其他关系，那么当时必定已经对 RHS 做了唯一化
		 * （见下文）。因此在
		 * 这条连接路径上，该半连接不再相关。
		 */
		if (sjinfo->jointype == JOIN_SEMI)
		{
			if (bms_is_subset(sjinfo->syn_righthand, rel1->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel1->relids))
				continue;
			if (bms_is_subset(sjinfo->syn_righthand, rel2->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel2->relids))
				continue;
		}

		/*
		 * 若一个输入包含 min_lefthand，另一个包含
		 * min_righthand，就可以在这次连接上执行该 SJ。
		 *
		 * 若匹配到不止一个 SJ，则拒绝；那意味着我们在
		 * 考虑某种并不真正有效的东西。
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			if (match_sjinfo)
				return false;	/* 无效的连接路径 */
			match_sjinfo = sjinfo;
			reversed = false;
		}
		else if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
				 bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			if (match_sjinfo)
				return false;	/* 无效的连接路径 */
			match_sjinfo = sjinfo;
			reversed = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				 create_unique_path(root, rel2, rel2->cheapest_total_path,
									sjinfo) != NULL)
		{
			/*----------
			 * For a semijoin, we can join the RHS to anything else by
			 * unique-ifying the RHS (if the RHS can be unique-ified).
			 * We will only get here if we have the full RHS but less
			 * than min_lefthand on the LHS.
			 *
			 * The reason to consider such a join path is exemplified by
			 *	SELECT ... FROM a,b WHERE (a.x,b.y) IN (SELECT c1,c2 FROM c)
			 * If we insist on doing this as a semijoin we will first have
			 * to form the cartesian product of A*B.  But if we unique-ify
			 * C then the semijoin becomes a plain innerjoin and we can join
			 * in any order, eg C to A and then to B.  When C is much smaller
			 * than A and B this can be a huge win.  So we allow C to be
			 * joined to just A or just B here, and then make_join_rel has
			 * to handle the case properly.
			 *
			 * Note that actually we'll allow unique-ified C to be joined to
			 * some other relation D here, too.  That is legal, if usually not
			 * very sane, and this routine is only concerned with legality not
			 * with whether the join is good strategy.
			 *----------
			 */
			if (match_sjinfo)
				return false;	/* 无效的连接路径 */
			match_sjinfo = sjinfo;
			reversed = false;
			unique_ified = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel1->relids) &&
				 create_unique_path(root, rel1, rel1->cheapest_total_path,
									sjinfo) != NULL)
		{
			/* 反向半连接的情形 */
			if (match_sjinfo)
				return false;	/* 无效的连接路径 */
			match_sjinfo = sjinfo;
			reversed = true;
			unique_ified = true;
		}
		else
		{
			/*
			 * 否则，提议的连接与 RHS 重叠，但不是这个 SJ 的有效
			 * 实现。不过先别急：RHS
			 * 违规也许先前已经发生，在一个或两个输入
			 * 关系里，那时我们必定已经决定
			 * 可以把某个其他 SJ 与这个交换。若我们需要
			 * 做这次连接才能把 RHS 建完，拒绝它
			 * 可能导致根本找不到任何计划。（这会发生，
			 * 是因为本文件别处推迟无子句连接的启发式：
			 * 我们也许要等到已经对无子句连接的一侧或两侧执行了其他、
			 * 可合法交换的 SJ 之后，才考虑在 RHS 内部做
			 * 无子句连接。）
			 * 这归结为：若两个输入都
			 * 与 RHS 重叠，就可以允许该连接---它们要么
			 * 完全位于 RHS 之内，要么表示先前已允许的、连向
			 * RHS 之外那些关系的连接。
			 */
			if (bms_overlap(rel1->relids, sjinfo->min_righthand) &&
				bms_overlap(rel2->relids, sjinfo->min_righthand))
				continue;		/* 假定先前对 RHS 的违规是有效的 */

			/*
			 * 提议的连接仍可能合法，但仅当我们被
			 * 允许把它结合进这个 SJ 的 RHS 时。这意味着
			 * 这个 SJ 必须是 LEFT 连接（不是 SEMI 或 ANTI，当然也
			 * 不是 FULL），并且提议的连接不得与 LHS 重叠。
			 */
			if (sjinfo->jointype != JOIN_LEFT ||
				bms_overlap(joinrelids, sjinfo->min_lefthand))
				return false;	/* 无效的连接路径 */

			/*
			 * 要成为有效连接，提议的连接必须是 LEFT 连接；否则
			 * 它无法结合进这个 SJ 的 RHS。但我们也许还没有
			 * 找到与提议连接匹配的 SpecialJoinInfo，因此还
			 * 无法现在就测试。把该条件记下来以后再用。
			 */
			must_be_leftjoin = true;
		}
	}

	/*
	 * 若违反了任何 SJ 的 RHS，且没有匹配到 LEFT SJ，则失败：
	 * 提议的连接无法结合进某个 SJ 的 RHS。
	 *
	 * 另外，若提议连接的谓词不严格，也失败；我们
	 * 实质上是在检查能否应用外连接恒等式 3，而
	 * 那是必须满足的条件。（这个检查也许与 make_outerjoininfo 里的检查重复，
	 * 但我不太确定，而且测试起来很便宜。）
	 */
	if (must_be_leftjoin &&
		(match_sjinfo == NULL ||
		 match_sjinfo->jointype != JOIN_LEFT ||
		 !match_sjinfo->lhs_strict))
		return false;			/* 无效的连接路径 */

	/*
	 * 还必须检查 LATERAL 引用施加的约束。
	 */
	if (root->hasLateralRTEs)
	{
		bool		lateral_fwd;
		bool		lateral_rev;
		Relids		join_lateral_rels;

		/*
		 * 提议的关系各自都可能含有对另一方的横向引用，
		 * 此时连接不可能。若横向
		 * 引用只在一个方向上，则该连接必须做成
		 * 嵌套循环，横向引用方放在内侧。若该连接
		 * 匹配一个无法用这种嵌套循环实现的 SJ，则
		 * 连接不可能。
		 *
		 * 另外，若横向引用只是间接的，应当拒绝
		 * 该连接；引用链经过的那些 rel(s) 必须
		 * 先被连接进来。
		 *
		 * 另一种可能使我们无法构造有效计划的情形是
		 * have_dangerous_phv() 所描述的实现限制。
		 */
		lateral_fwd = bms_overlap(rel1->relids, rel2->lateral_relids);
		lateral_rev = bms_overlap(rel2->relids, rel1->lateral_relids);
		if (lateral_fwd && lateral_rev)
			return false;		/* 两个方向都有横向引用 */
		if (lateral_fwd)
		{
			/* 必须实现为 rel1 在左侧的嵌套循环 */
			if (match_sjinfo &&
				(reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* 无法实现为嵌套循环 */
			/* 检查是否存在从 rel2 到 rel1 的直接引用 */
			if (!bms_overlap(rel1->relids, rel2->direct_lateral_relids))
				return false;	/* 只有间接引用，因此拒绝 */
			/* 检查不会出现危险的 PHV */
			if (have_dangerous_phv(root, rel1->relids, rel2->lateral_relids))
				return false;	/* 也许无法处理所需的 PHV */
		}
		else if (lateral_rev)
		{
			/* 必须实现为 rel2 在左侧的嵌套循环 */
			if (match_sjinfo &&
				(!reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* 无法实现为嵌套循环 */
			/* 检查是否存在从 rel1 到 rel2 的直接引用 */
			if (!bms_overlap(rel2->relids, rel1->direct_lateral_relids))
				return false;	/* 只有间接引用，因此拒绝 */
			/* 检查不会出现危险的 PHV */
			if (have_dangerous_phv(root, rel2->relids, rel1->lateral_relids))
				return false;	/* 也许无法处理所需的 PHV */
		}

		/*
		 * 若接受这次连接，LATERAL 引用以后也可能
		 * 带来问题：若该连接的最小参数化包含某些关系，
		 * 而这些关系必须处在与这个连接
		 * 关系所成外连接的内侧，那么永远
		 * 不可能用这次连接构造出完整
		 * 查询。应当拒绝这次连接，不仅因为
		 * 能省事，也因为若不拒绝，无子句连接
		 * 启发式也许会认为这次连接合法就意味着某个
		 * 其他连接关系不必形成，从而可能导致
		 * 根本找不到任何计划。要考虑的不仅是直接处于
		 * 与该连接关系所成外连接内侧的关系，也包括间接如此的，所以要搜索找出全部。
		 */
		join_lateral_rels = min_join_parameterization(root, joinrelids,
													  rel1, rel2);
		if (join_lateral_rels)
		{
			Relids		join_plus_rhs = bms_copy(joinrelids);
			bool		more;

			do
			{
				more = false;
				foreach(l, root->join_info_list)
				{
					SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

					/* 忽略全连接---它们的顺序是预定的 */
					if (sjinfo->jointype == JOIN_FULL)
						continue;

					if (bms_overlap(sjinfo->min_lefthand, join_plus_rhs) &&
						!bms_is_subset(sjinfo->min_righthand, join_plus_rhs))
					{
						join_plus_rhs = bms_add_members(join_plus_rhs,
														sjinfo->min_righthand);
						more = true;
					}
				}
			} while (more);
			if (bms_overlap(join_plus_rhs, join_lateral_rels))
				return false;	/* 将无法连到某个 RHS 关系 */
		}
	}

	/* 否则，这是一次有效连接 */
	*sjinfo_p = match_sjinfo;
	*reversed_p = reversed;
	return true;
}


/*
 * make_join_rel
 *	   查找或创建一个表示给定两个关系之连接的 RelOptInfo，
 *	   并把以这两个关系分别作为外层和内层而
 *	   创建的路径信息加进去。
 *	   （该连接关系也许已经含有由其他
 *	   关系对生成的路径，那些关系对加起来是同一组基关系。）
 *
 * 注意：若尝试的连接无效，将返回 NULL。这会发生在
 * 处理外连接时，或处理已被
 * 变成连接的 IN 或 EXISTS 子句时。
 */
RelOptInfo *
make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2)
{
	Relids		joinrelids;
	SpecialJoinInfo *sjinfo;
	bool		reversed;
	List	   *pushed_down_joins = NIL;
	SpecialJoinInfo sjinfo_data;
	RelOptInfo *joinrel;
	List	   *restrictlist;

	/* 绝不应当尝试连接两个重叠的关系集合。 */
	Assert(!bms_overlap(rel1->relids, rel2->relids));

	/* 构造标识该连接关系的 Relids 集合（此时还没有 OJ）。 */
	joinrelids = bms_union(rel1->relids, rel2->relids);

	/* 检查有效性并确定连接类型。 */
	if (!join_is_legal(root, rel1, rel2, joinrelids,
					   &sjinfo, &reversed))
	{
		/* 无效的连接路径 */
		bms_free(joinrelids);
		return NULL;
	}

	/*
	 * 加上外连接的 relid(s) 以形成规范 relids。除 sjinfo 自身外，任何新增的外
	 * 连接都追加到 pushed_down_joins。
	 */
	joinrelids = add_outer_joins_to_relids(root, joinrelids, sjinfo,
										   &pushed_down_joins);

	/* 必要时交换关系，以匹配连接信息。 */
	if (reversed)
	{
		RelOptInfo *trel = rel1;

		rel1 = rel2;
		rel2 = trel;
	}

	/*
	 * 若是普通内连接，则在
	 * join_info_list 里什么也找不到。造一个 SpecialJoinInfo，让选择率
	 * 估计函数知道正在连接什么。
	 */
	if (sjinfo == NULL)
	{
		sjinfo = &sjinfo_data;
		sjinfo->type = T_SpecialJoinInfo;
		sjinfo->min_lefthand = rel1->relids;
		sjinfo->min_righthand = rel2->relids;
		sjinfo->syn_lefthand = rel1->relids;
		sjinfo->syn_righthand = rel2->relids;
		sjinfo->jointype = JOIN_INNER;
		sjinfo->ojrelid = 0;
		sjinfo->commute_above_l = NULL;
		sjinfo->commute_above_r = NULL;
		sjinfo->commute_below_l = NULL;
		sjinfo->commute_below_r = NULL;
		/* 我们懒得把其余字段弄成有效的 */
		sjinfo->lhs_strict = false;
		sjinfo->semi_can_btree = false;
		sjinfo->semi_can_hash = false;
		sjinfo->semi_operators = NIL;
		sjinfo->semi_rhs_exprs = NIL;
	}

	/*
	 * 查找或构造连接 RelOptInfo，并计算与
	 * 这一次特定连接相配的限制列表。
	 */
	joinrel = build_join_rel(root, joinrelids, rel1, rel2,
							 sjinfo, pushed_down_joins,
							 &restrictlist);

	/*
	 * 若已经证明这次连接为空，就不必再为它考虑任何
	 * 更多路径。
	 */
	if (is_dummy_rel(joinrel))
	{
		bms_free(joinrelids);
		return joinrel;
	}

	/* 向连接关系添加路径。 */
	populate_joinrel_with_paths(root, rel1, rel2, joinrel, sjinfo,
								restrictlist);

	bms_free(joinrelids);

	return joinrel;
}

/*
 * add_outer_joins_to_relids
 *	  向 input_relids 加入 relids，表示将在
 *	  这次连接处计算的任何外连接。
 *
 * input_relids 是两个输入关系 relid 集合的并集。
 * 注意我们原地修改并返回它；若需要单独的值，调用方必须先
 * bms_copy() 它。
 *
 * sjinfo 表示正在执行的连接。
 *
 * 若当前连接完成了任何按外连接恒等式 3
 * 下推的外连接的计算，那些 relids 会连同 sjinfo 自己的 relid
 * 一起加入结果。若 pushed_down_joins
 * 不是 NULL，则这些新增外连接的 SpecialJoinInfos 也会
 * 追加到 *pushed_down_joins（因此调用方必须把它初始化为 NIL）。
 */
Relids
add_outer_joins_to_relids(PlannerInfo *root, Relids input_relids,
						  SpecialJoinInfo *sjinfo,
						  List **pushed_down_joins)
{
	/* 若这不是带有已分配 relid 的外连接，则无事可做。 */
	if (sjinfo == NULL || sjinfo->ojrelid == 0)
		return input_relids;

	/*
	 * 若它不是左连接，就没有规则允许以
	 * 非语法顺序执行它，因此只形成语法上的 relid 集合。（这
	 * 只是快速退出测试；反正也会得出同一结论，
	 * 因为它的 commute_below_l 和 commute_above_l 集合必须为空。）
	 */
	if (sjinfo->jointype != JOIN_LEFT)
		return bms_add_member(input_relids, sjinfo->ojrelid);

	/*
	 * 若这次连接已按 OJ 恒等式 3 被推进语法上更低的左连接的 RHS，
	 * 我们无法加入该 OJ 的 relid。（若已经推进，则我们
	 * 无法声称它的输出代表其 RHS 的最终状态。）
	 * 也不会有任何其他 OJ 可以加入，因此
	 * 到此结束。
	 */
	if (!bms_is_subset(sjinfo->commute_below_l, input_relids))
		return input_relids;

	/* 可以加入 OJ 自己的 relid */
	input_relids = bms_add_member(input_relids, sjinfo->ojrelid);

	/*
	 * 反过来，若我们现在正在形成这样一对已交换 OJ 的
	 * 最终结果，就该加上被下推的 join(s) 的 relid(s)。
	 * 若这个连接从不是上推候选，可以跳过。
	 */
	if (sjinfo->commute_above_l)
	{
		Relids		commute_above_rels = bms_copy(sjinfo->commute_above_l);
		ListCell   *lc;

		/*
		 * 当前连接也许能完成不止一个
		 * 被下推连接的空值填充，因此必须检查全部 SpecialJoinInfos。
		 * 因为 join_info_list 是自底向上建立的，
		 * 遍历它一次就够了：某次循环迭代中加入的 ojrelid
		 * 不会影响更早迭代的决定。
		 */
		foreach(lc, root->join_info_list)
		{
			SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

			if (othersj == sjinfo ||
				othersj->ojrelid == 0 || othersj->jointype != JOIN_LEFT)
				continue;		/* 肯定不值得关注 */

			if (!bms_is_member(othersj->ojrelid, commute_above_rels))
				continue;

			/* 若尚未存在、但条件现已满足，则加入它 */
			if (!bms_is_member(othersj->ojrelid, input_relids) &&
				bms_is_subset(othersj->min_lefthand, input_relids) &&
				bms_is_subset(othersj->min_righthand, input_relids) &&
				bms_is_subset(othersj->commute_below_l, input_relids))
			{
				input_relids = bms_add_member(input_relids, othersj->ojrelid);
				/* 若调用方要求，则报告这些被下推的外连接 */
				if (pushed_down_joins != NULL)
					*pushed_down_joins = lappend(*pushed_down_joins, othersj);

				/*
				 * 还必须检查 othersj 可能与之
				 * 交换的任何连接。它们同样必须出现在
				 * join_info_list 中 othersj 本身之后，因此可以在
				 * 这个循环的稍后访问它们。
				 */
				commute_above_rels = bms_add_members(commute_above_rels,
													 othersj->commute_above_l);
			}
		}
	}

	return input_relids;
}

/*
 * populate_joinrel_with_paths
 *	  为给定的一对连接关系，向给定连接关系添加路径。
 *	  SpecialJoinInfo 提供该连接的细节，restrictlist
 *	  包含连接子句以及适用于这一对
 *	  连接关系的其他子句。
 */
static void
populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
							RelOptInfo *rel2, RelOptInfo *joinrel,
							SpecialJoinInfo *sjinfo, List *restrictlist)
{
	/*
	 * 考虑把每个关系分别当作外层和内层的路径。取决于
	 * 连接类型，已证明为空的外层或内层关系也许意味着该连接
	 * 也已证明为空；此时丢掉先前算出的任何
	 * 路径，并把该连接标为哑。（我们这样做是因为
	 * 多元素连接的哑性质也许只对
	 * 某些构造路径才看得出来。）
	 *
	 * 另外，已证明恒为假的连接限制通常意味着
	 * 可以跳过连接一侧或两侧的求值。做法是
	 * 把相应关系标为哑。对外连接，被下推的
	 * 恒假限制仍意味着整个
	 * 连接是哑的，而未下推的则意味着没有内层行
	 * 会连上，因此可以把内层关系当作哑的。
	 *
	 * 只需考虑 join_info_list 中出现的连接类型，外加
	 * JOIN_INNER。
	 */
	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
			if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
				restriction_is_constant_false(restrictlist, joinrel, false))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			break;
		case JOIN_LEFT:
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_LEFT, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_RIGHT, sjinfo,
								 restrictlist);
			break;
		case JOIN_FULL:
			if ((is_dummy_rel(rel1) && is_dummy_rel(rel2)) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_FULL, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_FULL, sjinfo,
								 restrictlist);

			/*
			 * 若有既不可归并也不可哈希的连接限定条件，我们
			 * 也许无法构造出任何有效计划。在这里报错，以便
			 * 给出还算有用的错误信息。（由于对全连接的
			 * 规划没有余地，以后换另一对输入关系
			 * 也不会成功。）
			 */
			if (joinrel->pathlist == NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable or hash-joinable join conditions")));
			break;
		case JOIN_SEMI:

			/*
			 * 我们也许有一次普通半连接，或者关系还不够、
			 * 做不了半连接、但可以把 RHS 唯一化
			 * 再做内连接的情形（见 join_is_legal 的注释）。在
			 * 后一种情形下无法应用 JOIN_SEMI 连接。
			 */
			if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
				bms_is_subset(sjinfo->min_righthand, rel2->relids))
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_SEMI, sjinfo,
									 restrictlist);
			}

			/*
			 * 若我们知道如何把 RHS 唯一化，且一个输入关系
			 * 恰好就是 RHS（不是超集），就可以考虑先把它唯一化，
			 * 再做普通连接。（这里的 create_unique_path
			 * 检查多半与 join_is_legal 做过的重复，
			 * 但即便如此，检查也很便宜，因为有缓存。所以还是
			 * 测一下以求稳妥。）
			 */
			if (bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				create_unique_path(root, rel2, rel2->cheapest_total_path,
								   sjinfo) != NULL)
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_UNIQUE_INNER, sjinfo,
									 restrictlist);
				add_paths_to_joinrel(root, joinrel, rel2, rel1,
									 JOIN_UNIQUE_OUTER, sjinfo,
									 restrictlist);
			}
			break;
		case JOIN_ANTI:
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_ANTI, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_RIGHT_ANTI, sjinfo,
								 restrictlist);
			break;
		default:
			/* 这里不预期出现其他值 */
			elog(ERROR, "unrecognized join type: %d", (int) sjinfo->jointype);
			break;
	}

	/* 若可能，应用按分区连接技术。 */
	try_partitionwise_join(root, rel1, rel2, joinrel, sjinfo, restrictlist);
}


/*
 * have_join_order_restriction
 *		检测这两个关系是否应当连接，以满足
 *		由特殊连接或 LATERAL 连接引起的连接顺序限制。
 *
 * 实践中它总是与 have_relevant_joinclause() 一起使用，因此
 * 可以并入那个函数，但把两件事分开似乎更清楚。
 * 需要这个测试，是因为存在退化情形：
 * 必须做无子句连接才能满足连接顺序限制。
 * 另外，若一个关系横向引用另一个，或两者都需要
 * 用来计算某个 PHV，即使这次连接会是
 * 无子句的，也应当考虑把它们连起来。
 *
 * 注意：仅当退化外连接的一侧
 * 包含多个关系，或 IN/EXISTS 的 RHS 内需要无子句连接时，这才成问题；
 * 否则会经由 join_search_one_level() 的 "最后手段" 情形找到连接路径。
 * 若愿意在 "最后手段" 情形尝试丛状计划，就可以省掉这个测试，
 * 但那似乎
 * 低效得多。
 */
bool
have_join_order_restriction(PlannerInfo *root,
							RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	ListCell   *l;

	/*
	 * 若任一侧直接横向引用另一侧，就尝试这次
	 * 连接，不必考虑外连接方面的因素。
	 */
	if (bms_overlap(rel1->relids, rel2->direct_lateral_relids) ||
		bms_overlap(rel2->relids, rel1->direct_lateral_relids))
		return true;

	/*
	 * 同样，若两个关系都需要用来计算某个 PlaceHolderVar，
	 * 就尝试这次连接，不必考虑外连接方面的因素。（这并不
	 * 很理想，因为 eval_at 集合很大的 PHV 会导致考虑大量
	 * 多半无用的连接，但不这样做
	 * 可能导致我们根本构造不出计划。）
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel1->relids, phinfo->ph_eval_at) &&
			bms_is_subset(rel2->relids, phinfo->ph_eval_at))
			return true;
	}

	/*
	 * 这两个关系可能对应退化外连接的左右两侧，
	 * 也就是没有连接子句提及
	 * 非空侧的那种；此时应当强制进行该连接。
	 *
	 * 这两个关系也可能表示一次无子句连接，必须把它
	 * 做完才能建起某个外连接的 LHS 或 RHS。
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* 忽略全连接---其他机制会处理它们 */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* 能否用这些关系执行该 SJ？ */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			result = true;
			break;
		}

		/*
		 * 是否需要连接这些关系才能完成 RHS？必须
		 * 使用 "重叠" 测试，因为任一关系都可能包含一个更低的 SJ，
		 * 而该 SJ 已被证明可与这个交换。
		 */
		if (bms_overlap(sjinfo->min_righthand, rel1->relids) &&
			bms_overlap(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}

		/* LHS 也同样处理。 */
		if (bms_overlap(sjinfo->min_lefthand, rel1->relids) &&
			bms_overlap(sjinfo->min_lefthand, rel2->relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * 若任一输入关系可以用连接子句合法地
	 * 连到别的东西，我们就不强制进行这次连接。这实质上意味着
	 * 无子句的丛状连接被尽量推迟。原因是
	 * 当连接树高处有连接顺序限制时
	 * （也就是 LHS 或 RHS 内有许多关系），否则我们会
	 * 花大量工夫去考虑其 LHS 或 RHS 内部那些
	 * 非常愚蠢的连接组合。
	 */
	if (result)
	{
		if (has_legal_joinclause(root, rel1) ||
			has_legal_joinclause(root, rel2))
			result = false;
	}

	return result;
}


/*
 * has_join_restriction
 *		检测指定关系是否有连接顺序限制，
 *		原因是它位于外连接或 IN（子 SELECT）之内，
 *		或参与了任何 LATERAL 引用或多关系 PHVs。
 *
 * 本质上，这是在测试 have_join_order_restriction() 能否
 * 对这个关系和某个其他关系成功。有时错误地说 "true" 也没关系。
 * （因此我们不去费心做相对
 * 昂贵的 has_legal_joinclause 测试。）
 */
static bool
has_join_restriction(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	if (rel->lateral_relids != NULL || rel->lateral_referencers != NULL)
		return true;

	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, phinfo->ph_eval_at) &&
			!bms_equal(rel->relids, phinfo->ph_eval_at))
			return true;
	}

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* 忽略全连接---其他机制会保持它们的顺序 */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* 若 SJ 已包含在 rel 中则忽略 */
		if (bms_is_subset(sjinfo->min_lefthand, rel->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel->relids))
			continue;

		/* 若它与 LHS 或 RHS 重叠、但不包含 SJ，则受限 */
		if (bms_overlap(sjinfo->min_lefthand, rel->relids) ||
			bms_overlap(sjinfo->min_righthand, rel->relids))
			return true;
	}

	return false;
}


/*
 * has_legal_joinclause
 *		检测指定关系能否合法地用连接子句
 *		连到任何其他关系。
 *
 * 我们只考虑与当前
 * initial_rels 列表中单个其他关系的连接。这对多数真实
 * 查询足以得到 "true"；偶尔错误的 "false" 只是多花一点
 * 规划时间。这个限制的原因是：考虑连到
 * 其他连接需要证明那个连接关系可以合法
 * 形成，对一个只是用来节省规划时间的
 * 启发式来说，这似乎太麻烦。（注意：必须看 initial_rels，
 * 而不是整个查询，因为在规划子连接列表时，
 * 即使有连接子句连向查询的其他部分，
 * 我们仍可能被迫在 initial_rels 内部做无子句连接。）
 */
static bool
has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, root->initial_rels)
	{
		RelOptInfo *rel2 = (RelOptInfo *) lfirst(lc);

		/* 忽略已经在 "rel" 中的关系 */
		if (bms_overlap(rel->relids, rel2->relids))
			continue;

		if (have_relevant_joinclause(root, rel, rel2))
		{
			Relids		joinrelids;
			SpecialJoinInfo *sjinfo;
			bool		reversed;

			/* join_is_legal 需要并集的 relids */
			joinrelids = bms_union(rel->relids, rel2->relids);

			if (join_is_legal(root, rel, rel2, joinrelids,
							  &sjinfo, &reversed))
			{
				/* 是的，这样可行 */
				bms_free(joinrelids);
				return true;
			}

			bms_free(joinrelids);
		}
	}

	return false;
}


/*
 * 创建参数化嵌套循环有个陷阱：假设内层
 * 关系（称为 A）有一个参数是 PlaceHolderVar，且该 PHV 的
 * 最小 eval_at 集合包含外层关系（B）和某个第三关系（C）。
 * 我们也许以为可以创建一个由 C 参数化的 B/A 嵌套循环连接。
 * 但最终计划里，该 PHV 的表达式必须
 * 在 B/A 连接处作为嵌套循环参数求值；而执行器只
 * 准备好把简单 Var 当作 NestLoopParams。与其给执行器增加
 * 复杂性和开销来处理这种边角情形，看来更好的做法是
 * 禁止该连接。（注意我们仍可以使用 A 的参数化
 * 路径，把预先连接的 B+C 当作外层关系。have_join_order_restriction()
 * 保证即使没有
 * 其他理由，我们也会考虑做这样的连接。）
 *
 * 因此我们检查查询中用到的任何 PHVs 是否会造成这种危险。
 * 我们没有简单办法检查有风险的 PHV 是否真的
 * 会用在内层计划里，而这种情形如此少见，似乎不
 * 值得花很大力气。
 *
 * 这需要在两处检查。若内层关系的最小
 * 参数化会触发该限制，则 join_is_legal() 应当
 * 整个拒绝该连接，因为不会有可用的路径。
 * 但 joinpath.c 必须对每条提议的嵌套循环路径再检查一次，因为
 * 内层路径的参数化也许超过最小值，从而使
 * 某个 PHV 对它变得危险，否则并不会。
 */
bool
have_dangerous_phv(PlannerInfo *root,
				   Relids outer_relids, Relids inner_params)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (!bms_is_subset(phinfo->ph_eval_at, inner_params))
			continue;			/* 忽略，无法成为嵌套循环参数 */
		if (!bms_overlap(phinfo->ph_eval_at, outer_relids))
			continue;			/* 忽略，与这次连接无关 */
		if (bms_is_subset(phinfo->ph_eval_at, outer_relids))
			continue;			/* 安全，它可以在 outerrel 内求值 */
		/* 否则它可能不安全，因此拒绝该连接 */
		return true;
	}

	/* 可以执行该连接 */
	return false;
}


/*
 * is_dummy_rel --- 该关系是否已被证明为空？
 */
bool
is_dummy_rel(RelOptInfo *rel)
{
	Path	   *path;

	/*
	 * 已知为哑的关系只会有一条路径，即没有子节点的
	 * Append。（即使它不知怎么有更多路径，没有子节点的 Append
	 * 代价为零，因此应当排在 pathlist 的最前面。）
	 */
	if (rel->pathlist == NIL)
		return false;
	path = (Path *) linitial(rel->pathlist);

	/*
	 * 起初，哑路径只是一个没有子节点的 Append。但在以后的
	 * 规划阶段，我们也许会在上面贴一个 ProjectSetPath 和/或 ProjectionPath，
	 * 因为 Append 无法做投影。与其假设
	 * 哪些组合会出现，不如顺着找到的东西一直往下走。
	 */
	for (;;)
	{
		if (IsA(path, ProjectionPath))
			path = ((ProjectionPath *) path)->subpath;
		else if (IsA(path, ProjectSetPath))
			path = ((ProjectSetPath *) path)->subpath;
		else
			break;
	}
	if (IS_DUMMY_APPEND(path))
		return true;
	return false;
}

/*
 * 把一个关系标为已证明为空。
 *
 * 在 GEQO 规划期间，同一个
 * 基关系结构可能被调用不止一次，因此值得检查该关系是否已经标为
 * 哑。
 *
 * 另外，在 GEQO 连接规划期间调用时，我们处于一个短命的
 * 内存上下文。必须确保挂在基关系上的哑路径
 * 能活过这次 GEQO 循环，否则该基关系会被破坏，影响以后的
 * GEQO 循环。另一方面，在 GEQO 期间把连接关系标为哑时，
 * 我们不希望哑路径弄乱主规划上下文。结论
 * 是最好的办法是在给定 RelOptInfo 所在的同一
 * 上下文里显式创建哑路径。
 */
void
mark_dummy_rel(RelOptInfo *rel)
{
	MemoryContext oldcontext;

	/* 已经标过了？ */
	if (is_dummy_rel(rel))
		return;

	/* 没有，因此选择正确的上下文来创建哑路径 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/* 设置哑的大小估计 */
	rel->rows = 0;

	/* 清掉先前选中的任何路径 */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/* 建立哑路径 */
	add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL,
											  NIL, rel->lateral_relids,
											  0, false, -1));

	/* 设置或更新 cheapest_total_path 及相关字段 */
	set_cheapest(rel);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * restriction_is_constant_false --- 限制列表是不是就是 FALSE？
 *
 * 在限定条件可证明恒为 FALSE 的情形下，eval_const_expressions
 * 通常会丢掉任何与它 ANDed 的东西。在外
 * 连接情形下，这会让我们去算笛卡尔积，只是为了
 * 判定外层行没有匹配，这相当愚蠢。因此
 * 我们需要检测这种情况。
 *
 * 若 only_pushed_down 为 true，则只考虑从连接关系的角度看被下推的
 * 限定条件。
 */
static bool
restriction_is_constant_false(List *restrictlist,
							  RelOptInfo *joinrel,
							  bool only_pushed_down)
{
	ListCell   *lc;

	/*
	 * 尽管有上面的注释，这里看到的限制列表
	 * 除了 FALSE 常量之外也许还有其他成员，因为其他
	 * 限定条件可能被 "下推" 到外连接这一层。因此我们检查
	 * 列表的每个成员。
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (only_pushed_down && !RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		if (rinfo->clause && IsA(rinfo->clause, Const))
		{
			Const	   *con = (Const *) rinfo->clause;

			/* 就我们的目的而言，常量 NULL 和常量 FALSE 一样好 */
			if (con->constisnull)
				return true;
			if (!DatumGetBool(con->constvalue))
				return true;
		}
	}
	return false;
}

/*
 * 评估给定的两个分区关系之间的连接能否被拆
 * 成匹配分区之间的连接；这种技术叫做
 * "按分区连接"
 *
 * 当 a. 被连接关系具有相同
 * 分区方案 b. 两个关系的分区键之间存在等值连接时，
 * 就可以做按分区连接。
 *
 * 按分区连接的规划如下（细节见 optimizer/README。）
 *
 * 1. 为匹配分区之间的连接创建 RelOptInfos，即
 * 子连接，并向它们添加路径。
 *
 * 2. 跨这组子连接构造 Append 或 MergeAppend 路径。
 * 第二阶段由 generate_partitionwise_join_paths() 实现。
 *
 * 每个子连接的 RelOptInfo、SpecialJoinInfo 和限制列表
 * 通过转换相应的父连接结构得到。
 */
static void
try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
					   RelOptInfo *joinrel, SpecialJoinInfo *parent_sjinfo,
					   List *parent_restrictlist)
{
	bool		rel1_is_simple = IS_SIMPLE_REL(rel1);
	bool		rel2_is_simple = IS_SIMPLE_REL(rel2);
	List	   *parts1 = NIL;
	List	   *parts2 = NIL;
	ListCell   *lcr1 = NULL;
	ListCell   *lcr2 = NULL;
	int			cnt_parts;

	/* 防止分区层次过深导致栈溢出。 */
	check_stack_depth();

	/* 若连接关系未分区，则无事可做。 */
	if (joinrel->part_scheme == NULL || joinrel->nparts == 0)
		return;

	/* 连接关系应当已设置 consider_partitionwise_join。 */
	Assert(joinrel->consider_partitionwise_join);

	/*
	 * 若参与连接的任一
	 * 关系未分区，就无法做按分区连接。
	 */
	if (!IS_PARTITIONED_REL(rel1) || !IS_PARTITIONED_REL(rel2))
		return;

	Assert(REL_HAS_ALL_PART_PROPS(rel1) && REL_HAS_ALL_PART_PROPS(rel2));

	/* 参与连接的关系应当已设置 consider_partitionwise_join。 */
	Assert(rel1->consider_partitionwise_join &&
		   rel2->consider_partitionwise_join);

	/*
	 * 连接关系的分区方案应当与
	 * 参与连接的关系一致。
	 */
	Assert(joinrel->part_scheme == rel1->part_scheme &&
		   joinrel->part_scheme == rel2->part_scheme);

	Assert(!(joinrel->partbounds_merged && (joinrel->nparts <= 0)));

	compute_partition_bounds(root, rel1, rel2, joinrel, parent_sjinfo,
							 &parts1, &parts2);

	if (joinrel->partbounds_merged)
	{
		lcr1 = list_head(parts1);
		lcr2 = list_head(parts2);
	}

	/*
	 * 若尚不存在，则为这个分区连接创建子连接关系。
	 * 向子连接添加路径，对应给定那对父关系的
	 * 一对子关系。
	 */
	for (cnt_parts = 0; cnt_parts < joinrel->nparts; cnt_parts++)
	{
		RelOptInfo *child_rel1;
		RelOptInfo *child_rel2;
		bool		rel1_empty;
		bool		rel2_empty;
		SpecialJoinInfo *child_sjinfo;
		List	   *child_restrictlist;
		RelOptInfo *child_joinrel;
		AppendRelInfo **appinfos;
		int			nappinfos;

		if (joinrel->partbounds_merged)
		{
			child_rel1 = lfirst_node(RelOptInfo, lcr1);
			child_rel2 = lfirst_node(RelOptInfo, lcr2);
			lcr1 = lnext(parts1, lcr1);
			lcr2 = lnext(parts2, lcr2);
		}
		else
		{
			child_rel1 = rel1->part_rels[cnt_parts];
			child_rel2 = rel2->part_rels[cnt_parts];
		}

		rel1_empty = (child_rel1 == NULL || IS_DUMMY_REL(child_rel1));
		rel2_empty = (child_rel2 == NULL || IS_DUMMY_REL(child_rel2));

		/*
		 * 检查能否证明这段连接
		 * 不返回行，原因是一个或两个输入为空（包括
		 * 已被完全剪枝的输入）。若是，就忽略它。
		 * 这些规则等价于 populate_joinrel_with_paths 对
		 * 哑输入关系的规则。
		 */
		switch (parent_sjinfo->jointype)
		{
			case JOIN_INNER:
			case JOIN_SEMI:
				if (rel1_empty || rel2_empty)
					continue;	/* 忽略这段连接 */
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				if (rel1_empty)
					continue;	/* 忽略这段连接 */
				break;
			case JOIN_FULL:
				if (rel1_empty && rel2_empty)
					continue;	/* 忽略这段连接 */
				break;
			default:
				/* 这里不预期出现其他值 */
				elog(ERROR, "unrecognized join type: %d",
					 (int) parent_sjinfo->jointype);
				break;
		}

		/*
		 * 若某个子分区已被完全剪枝，就无法为它生成路径，
		 * 因此必须拒绝按分区连接，除非上面已经
		 * 能把这个分区消掉。
		 */
		if (child_rel1 == NULL || child_rel2 == NULL)
		{
			/*
			 * 把该连接关系标为未分区，以便后面的函数
			 * 正确处理它。
			 */
			joinrel->nparts = 0;
			return;
		}

		/*
		 * 若叶关系的 consider_partitionwise_join 为 false，意味着
		 * 它是哑关系，我们在 set_append_rel_size() 里跳过了为它设置 tlist
		 * 表达式和添加 EC 成员，因此
		 * 这里必须再次失败。
		 */
		if (rel1_is_simple && !child_rel1->consider_partitionwise_join)
		{
			Assert(child_rel1->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel1));
			joinrel->nparts = 0;
			return;
		}
		if (rel2_is_simple && !child_rel2->consider_partitionwise_join)
		{
			Assert(child_rel2->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel2));
			joinrel->nparts = 0;
			return;
		}

		/* 绝不应当尝试连接两个重叠的关系集合。 */
		Assert(!bms_overlap(child_rel1->relids, child_rel2->relids));

		/*
		 * 根据父连接关系的
		 * SpecialJoinInfo 构造 SpecialJoinInfo。
		 */
		child_sjinfo = build_child_join_sjinfo(root, parent_sjinfo,
											   child_rel1->relids,
											   child_rel2->relids);

		/* 找到 AppendRelInfo 结构 */
		appinfos = find_appinfos_by_relids(root,
										   bms_union(child_rel1->relids,
													 child_rel2->relids),
										   &nappinfos);

		/*
		 * 根据适用于父连接的限制，构造适用于子连接的
		 * 限制。
		 */
		child_restrictlist =
			(List *) adjust_appendrel_attrs(root,
											(Node *) parent_restrictlist,
											nappinfos, appinfos);

		/* 查找或构造子连接的 RelOptInfo */
		child_joinrel = joinrel->part_rels[cnt_parts];
		if (!child_joinrel)
		{
			child_joinrel = build_child_join_rel(root, child_rel1, child_rel2,
												 joinrel, child_restrictlist,
												 child_sjinfo);
			joinrel->part_rels[cnt_parts] = child_joinrel;
			joinrel->live_parts = bms_add_member(joinrel->live_parts, cnt_parts);
			joinrel->all_partrels = bms_add_members(joinrel->all_partrels,
													child_joinrel->relids);
		}

		/* 断言我们得到的是正确的那一个 */
		Assert(bms_equal(child_joinrel->relids,
						 adjust_child_relids(joinrel->relids,
											 nappinfos, appinfos)));

		/* 并为子连接创建路径 */
		populate_joinrel_with_paths(root, child_rel1, child_rel2,
									child_joinrel, child_sjinfo,
									child_restrictlist);

		pfree(appinfos);
	}
}

/*
 * 通过转换父表之间连接的 SpecialJoinInfo，构造子连接的 SpecialJoinInfo。
 * left_relids 和 right_relids
 * 分别是连接左右两侧的 relids。
 */
static SpecialJoinInfo *
build_child_join_sjinfo(PlannerInfo *root, SpecialJoinInfo *parent_sjinfo,
						Relids left_relids, Relids right_relids)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	AppendRelInfo **left_appinfos;
	int			left_nappinfos;
	AppendRelInfo **right_appinfos;
	int			right_nappinfos;

	memcpy(sjinfo, parent_sjinfo, sizeof(SpecialJoinInfo));
	left_appinfos = find_appinfos_by_relids(root, left_relids,
											&left_nappinfos);
	right_appinfos = find_appinfos_by_relids(root, right_relids,
											 &right_nappinfos);

	sjinfo->min_lefthand = adjust_child_relids(sjinfo->min_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->min_righthand = adjust_child_relids(sjinfo->min_righthand,
												right_nappinfos,
												right_appinfos);
	sjinfo->syn_lefthand = adjust_child_relids(sjinfo->syn_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->syn_righthand = adjust_child_relids(sjinfo->syn_righthand,
												right_nappinfos,
												right_appinfos);
	/* 外连接 relids 不需要调整 */
	sjinfo->semi_rhs_exprs = (List *) adjust_appendrel_attrs(root,
															 (Node *) sjinfo->semi_rhs_exprs,
															 right_nappinfos,
															 right_appinfos);

	pfree(left_appinfos);
	pfree(right_appinfos);

	return sjinfo;
}

/*
 * compute_partition_bounds
 *		根据输入的分区边界计算连接关系的分区边界
 */
static void
compute_partition_bounds(PlannerInfo *root, RelOptInfo *rel1,
						 RelOptInfo *rel2, RelOptInfo *joinrel,
						 SpecialJoinInfo *parent_sjinfo,
						 List **parts1, List **parts2)
{
	/*
	 * 若还没有连接关系的分区边界，就尝试
	 * 连同待连接的分区对一起算出来。
	 */
	if (joinrel->nparts == -1)
	{
		PartitionScheme part_scheme = joinrel->part_scheme;
		PartitionBoundInfo boundinfo = NULL;
		int			nparts = 0;

		Assert(joinrel->boundinfo == NULL);
		Assert(joinrel->part_rels == NULL);

		/*
		 * 看输入的分区边界是否完全相同，若是
		 * 就不必费力：连接关系将具有与输入
		 * 相同的分区边界，基数位置相同的分区
		 * 将组成对。
		 *
		 * 注意：即使一个或两个输入的边界已经合并，
		 * 两边的边界仍可能完全相同，但
		 * 为此再花周期去检查似乎不值得。
		 */
		if (!rel1->partbounds_merged &&
			!rel2->partbounds_merged &&
			rel1->nparts == rel2->nparts &&
			partition_bounds_equal(part_scheme->partnatts,
								   part_scheme->parttyplen,
								   part_scheme->parttypbyval,
								   rel1->boundinfo, rel2->boundinfo))
		{
			boundinfo = rel1->boundinfo;
			nparts = rel1->nparts;
		}
		else
		{
			/* 尝试合并输入的分区边界。 */
			boundinfo = partition_bounds_merge(part_scheme->partnatts,
											   part_scheme->partsupfunc,
											   part_scheme->partcollation,
											   rel1, rel2,
											   parent_sjinfo->jointype,
											   parts1, parts2);
			if (boundinfo == NULL)
			{
				joinrel->nparts = 0;
				return;
			}
			nparts = list_length(*parts1);
			joinrel->partbounds_merged = true;
		}

		Assert(nparts > 0);
		joinrel->boundinfo = boundinfo;
		joinrel->nparts = nparts;
		joinrel->part_rels =
			(RelOptInfo **) palloc0(sizeof(RelOptInfo *) * nparts);
	}
	else
	{
		Assert(joinrel->nparts > 0);
		Assert(joinrel->boundinfo);
		Assert(joinrel->part_rels);

		/*
		 * 若连接关系的 partbounds_merged 标志为 true，意味着输入
		 * 不保证具有相同的分区边界，因此我们
		 * 无法假定基数位置相同的分区
		 * 组成对；让 get_matching_part_pairs() 来生成这些对。
		 * 否则无需做事，因为可以这样假定。
		 */
		if (joinrel->partbounds_merged)
		{
			get_matching_part_pairs(root, joinrel, rel1, rel2,
									parts1, parts2);
			Assert(list_length(*parts1) == joinrel->nparts);
			Assert(list_length(*parts2) == joinrel->nparts);
		}
	}
}

/*
 * get_matching_part_pairs
 *		从输入生成待连接的分区对
 */
static void
get_matching_part_pairs(PlannerInfo *root, RelOptInfo *joinrel,
						RelOptInfo *rel1, RelOptInfo *rel2,
						List **parts1, List **parts2)
{
	bool		rel1_is_simple = IS_SIMPLE_REL(rel1);
	bool		rel2_is_simple = IS_SIMPLE_REL(rel2);
	int			cnt_parts;

	*parts1 = NIL;
	*parts2 = NIL;

	for (cnt_parts = 0; cnt_parts < joinrel->nparts; cnt_parts++)
	{
		RelOptInfo *child_joinrel = joinrel->part_rels[cnt_parts];
		RelOptInfo *child_rel1;
		RelOptInfo *child_rel2;
		Relids		child_relids1;
		Relids		child_relids2;

		/*
		 * 若这段连接为空，意味着先前在
		 * try_partitionwise_join() 里为它创建子连接路径时忽略了这段，
		 * 因为它不会对连接
		 * 结果有贡献，原因是一个或两个输入为空；向每个
		 * 给定列表加入 NULL，以便该函数再次
		 * 忽略这段。
		 */
		if (!child_joinrel)
		{
			*parts1 = lappend(*parts1, NULL);
			*parts2 = lappend(*parts2, NULL);
			continue;
		}

		/*
		 * 取得参与这个连接段、且
		 * 来自 rel1 一侧的 partition(s) 的 relids 集合。
		 */
		child_relids1 = bms_intersect(child_joinrel->relids,
									  rel1->all_partrels);
		Assert(bms_num_members(child_relids1) == bms_num_members(rel1->relids));

		/*
		 * 用这些 relids 取得 rel1 的一个子关系。注意即使 rel1 是连接关系，
		 * 我们也应当有这个子关系，因为那种情况下
		 * relids 指定的分区会有匹配或重叠的
		 * 边界，因此在规划 rel1 的按分区连接时，这些指定分区应被当作
		 * 要连接的分区，
		 * 这意味着走到
		 * 这里时该子关系应当已经建好。
		 */
		if (rel1_is_simple)
		{
			int			varno = bms_singleton_member(child_relids1);

			child_rel1 = find_base_rel(root, varno);
		}
		else
			child_rel1 = find_join_rel(root, child_relids1);
		Assert(child_rel1);

		/*
		 * 取得参与这个连接段、且
		 * 来自 rel2 一侧的 partition(s) 的 relids 集合。
		 */
		child_relids2 = bms_intersect(child_joinrel->relids,
									  rel2->all_partrels);
		Assert(bms_num_members(child_relids2) == bms_num_members(rel2->relids));

		/*
		 * 用这些 relids 取得 rel2 的一个子关系。见上面的注释。
		 */
		if (rel2_is_simple)
		{
			int			varno = bms_singleton_member(child_relids2);

			child_rel2 = find_base_rel(root, varno);
		}
		else
			child_rel2 = find_join_rel(root, child_relids2);
		Assert(child_rel2);

		/*
		 * rel1 与 rel2 的连接是合法的，因此上面得到的子
		 * 关系的连接也合法；把它们作为产生这段连接的一对
		 * 加入给定列表。
		 */
		*parts1 = lappend(*parts1, child_rel1);
		*parts2 = lappend(*parts2, child_rel2);
	}
}
