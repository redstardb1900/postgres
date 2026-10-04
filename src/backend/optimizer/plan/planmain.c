/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * What's in a name, anyway?  The top-level entry point of the planner/
 * optimizer is over in planner.c, not here as you might think from the
 * file name.  But this is the main code for planning a basic join operation,
 * shorn of features like subselects, inheritance, aggregates, grouping,
 * and so on.  (Those are the things planner.c deals with.)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"


/*
 * query_planner
 *	  为基本查询生成一条路径（也就是一份简化计划），
 *	  它可以含有连接，但没有更花哨的特性。
 *
 * 由于 query_planner 不处理顶层加工（分组、
 * 排序等），它无法自己选出最佳路径。它改为
 * 返回连接最顶层的 RelOptInfo，由调用方
 * （grouping_planner）在该关系幸存的路径中选择。
 *
 * root 描述要规划的查询
 * qp_callback 是在可以安全计算 query_pathkeys 时调用的函数
 * qp_extra 是传给 qp_callback 的可选额外数据
 *
 * 注意：PlannerInfo 节点也含有 query_pathkeys 字段，它
 * 告诉 query_planner 最终输出
 * 计划所要的排序。调用时这个值 *还没有*，要等
 * qp_callback 在合并完查询的等价类之后才算出。
 * （在那完成之前，我们无法构造规范路径键。）
 */
RelOptInfo *
query_planner(PlannerInfo *root,
			  query_pathkeys_callback qp_callback, void *qp_extra)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;

	/*
	 * 把规划器的列表初始化为空。
	 *
	 * 注意：append_rel_list 已由 subquery_planner 设好，因此这里不要
	 * 动它。
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->placeholder_list = NIL;
	root->placeholder_array = NULL;
	root->placeholder_array_size = 0;
	root->fkey_list = NIL;
	root->initial_rels = NIL;

	/*
	 * 设置用于访问基关系和 AppendRelInfos 的数组。
	 */
	setup_simple_rel_arrays(root);

	/*
	 * 在平凡情形下，jointree 只是单个 RTE_RESULT 关系，
	 * 就绕过本函数其余部分，只做一个 RelOptInfo 及其
	 * 一条访问路径。这值得优化，因为它适用于
	 * "SELECT expression" 和 "INSERT ... VALUES()" 这类常见情形。
	 */
	Assert(parse->jointree->fromlist != NIL);
	if (list_length(parse->jointree->fromlist) == 1)
	{
		Node	   *jtnode = (Node *) linitial(parse->jointree->fromlist);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			Assert(rte != NULL);
			if (rte->rtekind == RTE_RESULT)
			{
				/* 直接为它做 RelOptInfo */
				final_rel = build_simple_rel(root, varno, NULL);

				/*
				 * 若查询总体上允许并行，就检查
				 * 限定条件是否受并行限制。（此时不必检查
				 * final_rel->reltarget，因为它还是空的。
				 * 查询目标列表里受并行限制的东西
				 * 以后再处理。）这通常很无聊，因为
				 * 只有 Result 的计划从来就不值得
				 * 并行化。不过，若 debug_parallel_query 开着，
				 * 我们就希望尽可能在并行工作进程里执行 Result，
				 * 因此必须做这件事。
				 */
				if (root->glob->parallelModeOK &&
					debug_parallel_query != DEBUG_PARALLEL_OFF)
					final_rel->consider_parallel =
						is_parallel_safe(root, parse->jointree->quals);

				/*
				 * 它唯一的路径是一条平凡的 Result 路径。这里取了点巧，
				 * 用了 GroupResultPath，因为这样
				 * 可以把限定条件直接塞进去，而不必预处理。
				 * （不过，把脑袋歪到合适的角度，没有 FROM 的
				 * SELECT 也算一种退化的分组，所以这不算
				 * 太取巧。）
				 */
				add_path(final_rel, (Path *)
						 create_group_result_path(root, final_rel,
												  final_rel->reltarget,
												  (List *) parse->jointree->quals));

				/* 选出最便宜的路径（这种情形相当容易……） */
				set_cheapest(final_rel);

				/*
				 * 不必跑 generate_base_implied_equalities，但
				 * 必须假装等价类合并已经完成。
				 */
				root->ec_merging_done = true;

				/*
				 * 仍然必须调用 qp_callback，以防是
				 * "SELECT 2+2 ORDER BY 1" 这类。
				 */
				(*qp_callback) (root, qp_extra);

				return final_rel;
			}
		}
	}

	/*
	 * 为查询中用到的全部基关系构造 RelOptInfo 节点。
	 * 追加关系的成员关系（"其他关系"）以后再加。
	 *
	 * 注意：靠搜索 jointree 来找基关系，而不是
	 * 扫描范围表，是因为范围表里可能有
	 * 并不积极参与查询的关系的 RTEs，例如视图。我们不想
	 * 为它们做 RelOptInfos。
	 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * 检查目标列表和连接树，为所有被引用的 Var 向基关系
	 * 目标列表加项，并为所有被引用的 PlaceHolderVars 生成 PlaceHolderInfo
	 * 项。限制子句和连接子句
	 * 加到所提及关系所属的适当列表。我们
	 * 也为可证明等价的表达式建立 EquivalenceClasses。
	 * SpecialJoinInfo 列表也建起来，用来保存连接顺序
	 * 限制的信息。最后，形成一份目标连接列表，供 make_one_rel()
	 * 据此工作。
	 */
	build_base_rel_tlists(root, root->processed_tlist);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * 既然已经建好等价类，重新考虑任何推迟的外连接限定条件。
	 * （这可能导致类的进一步添加或
	 * 合并。）
	 */
	reconsider_outer_join_clauses(root);

	/*
	 * 若形成了任何等价类，就酌情生成额外的限制
	 * 子句。（隐含的连接子句以后
	 * 随时形成。）
	 */
	generate_base_implied_equalities(root);

	/*
	 * 等价集的合并已经完成，因此现在可以
	 * 生成规范形式的路径键；于是计算 query_pathkeys 和
	 * PlannerInfo 里的其他路径键字段。
	 */
	(*qp_callback) (root, qp_extra);

	/*
	 * 检查子查询上拉期间生成的任何"占位"表达式。
	 * 确保它们需要的 Var 在相关
	 * 连接层次被标成需要。这必须在连接移除之前做，因为它可能
	 * 使 Var 或占位符在连接之上变为需要，而先前
	 * 并没有这样标记。
	 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * 去掉任何无用的外连接。理想情况下这会在
	 * jointree 预处理期间做，但必要的信息要到
	 * 建好基关系数据结构并分类限定子句之后才有。
	 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * 另外，把内层关系唯一的半连接归约为普通内连接。
	 * 同样，由于缺少所需信息，现在之前不能做这件事。
	 */
	reduce_unique_semijoins(root);

	/*
	 * 现在按需要把"占位符"分配到基关系。这必须在
	 * 连接移除之后做，因为移除可能改变某个
	 * 占位符能否在基关系上求值。
	 */
	add_placeholders_to_base_rels(root);

	/*
	 * 既然已经定下 PlaceHolderVar 的求值层次，现在构造
	 * LATERAL 引用集合。
	 */
	create_lateral_join_info(root);

	/*
	 * 把外键匹配到等价类和连接限定条件。这必须在
	 * 定稿等价类之后做，而且等到
	 * 连接移除之后再做也有用，这样就可以跳过涉及已移除关系的
	 * 外键。
	 */
	match_foreign_keys_to_quals(root);

	/*
	 * 寻找可以从中抽出单关系
	 * 限制 OR 子句的连接 OR 子句。
	 */
	extract_restriction_or_clauses(root);

	/*
	 * 现在展开追加关系，为其子项添加 "otherrels"。我们
	 * 把这推迟到最后，以便每个基关系能有尽可能多的
	 * 信息，包括全部限制子句。那样
	 * 才能剪掉不满足限制子句的分区。
	 * 另外注意，lateral_relids 等一些信息在这里从基关系
	 * 传播到其他关系，因此必须已经算好。
	 */
	add_other_rels_to_query(root);

	/*
	 * 把任何 UPDATE/DELETE/MERGE 行标识变量分发到目标
	 * 关系。这要到追加关系展开
	 * 完成之后才能做。
	 */
	distribute_row_identity_vars(root);

	/*
	 * 可以开始核心规划了。
	 */
	final_rel = make_one_rel(root, joinlist);

	/* 检查至少得到一条可用路径 */
	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");

	return final_rel;
}
