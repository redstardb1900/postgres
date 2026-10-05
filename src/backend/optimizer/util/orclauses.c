/*-------------------------------------------------------------------------
 *
 * orclauses.c
 *	  从连接 OR 子句中提取限制 OR 子句的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/orclauses.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/restrictinfo.h"


static bool is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel);
static Expr *extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel);
static void consider_new_or_clause(PlannerInfo *root, RelOptInfo *rel,
								   Expr *orclause, RestrictInfo *join_or_rinfo);


/*
 * extract_restriction_or_clauses
 *	  检查连接的 OR-of-AND 子句，看能否抽出有用的限制 OR
 *	  子句。若能，把它们加进查询。
 *
 * 虽然连接子句总体上必须引用多个关系，
 * 一个 OR of ANDs 子句可能含有只引用一个
 * 关系、可用于为该关系建立限制子句的子子句。
 * 例如考虑
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45));
 * 我们可以把它变成
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45))
 *			AND (a.x = 42 OR a.x = 44)
 *			AND (b.y = 43 OR b.z = 45);
 * 这样后面的子句可以在扫描 a 和 b 时应用，
 * 也许作为索引限定条件，无论如何都能减少
 * 到达连接的行数。本质上这是向
 * CNF（合取范式，即 OR 项的 AND）的部分变换。但它并不完整，因为我们不
 * 拆开原来的 OR——那样做通常会把限定
 * 表达式胀大，收益却很小。
 *
 * 新增的限定条件与原来的 OR 部分冗余，因此
 * 会在最终形成 joinrel 时低估其大小。
 * （完全变换成 CNF 也一样；问题
 * 其实不在变换，而在 clauselist_selectivity
 * 无法识别冗余条件。）我们可以补偿这种
 * 冗余：修改原来 OR 子句缓存的选择率，
 * 抵消对基关系估计大小的（有效）缩减，
 * 使估计的 joinrel 大小保持不变。这是重大权宜做法（MAJOR HACK）：它依赖于子句选择率被缓存，
 * 以及同一个 RestrictInfo 节点会出现在
 * 形成 joinrel 时可能用到的每个
 * joininfo 列表中。
 * 而且在大小估计非线性的情形下它不起作用
 * （即外连接和 IN 连接）。但总比什么都不做强。
 *
 * 我们检查每个基关系，看与它关联的连接子句
 * 是否含有可抽出的限制条件。若有，把那些条件
 * 加到该关系的 baserestrictinfo，并更新连接子句的
 * 缓存选择率。注意同一条连接子句会从
 * 参与它的每个基关系的角度重新检查，因此其
 * 缓存选择率可能被更新多次。
 */
void
extract_restriction_or_clauses(PlannerInfo *root)
{
	Index		rti;

	/* 检查每个基关系，寻找可能的连接 OR 子句 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		ListCell   *lc;

		/* 可能有对应非基关系 RTEs 的空槽 */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* 对数组做健全性检查 */

		/* 忽略作为 "other rels" 的 RTEs */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * 找出可能有用的 OR 连接子句。任何被
		 * 参数化路径机制认为可以安全移到这个关系的
		 * 连接子句都可以用，即使我们要对它做的事
		 * 并不正好是一条参数化路径。
		 */
		foreach(lc, rel->joininfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (restriction_is_or_clause(rinfo) &&
				join_clause_is_movable_to(rinfo, rel))
			{
				/* 尝试只为这个关系抽出一条限定条件 */
				Expr	   *orclause = extract_or_clause(rinfo, rel);

				/*
				 * 若成功，决定是否要用这条子句，
				 * 若用，就把它插入该关系的 restrictinfo 列表。
				 */
				if (orclause)
					consider_new_or_clause(root, rel, orclause, rinfo);
			}
		}
	}
}

/*
 * 给定的简单（非 OR）RestrictInfo 移到该关系是否安全？
 */
static bool
is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * 我们要的是提到该关系、且只提到该关系的子句。因此
	 * 伪常量子句尤其可以很快拒绝。然后检查
	 * 该子句的 Var 成员。
	 */
	if (rinfo->pseudoconstant)
		return false;
	if (!bms_equal(rinfo->clause_relids, rel->relids))
		return false;

	/* 我们不想对任何易变函数做额外求值 */
	if (contain_volatile_functions((Node *) rinfo->clause))
		return false;

	return true;
}

/*
 * 尝试从给定的连接 OR 子句中抽出
 * 只提到 rel 的限制子句。
 *
 * 必须能从 OR 的每一支为这个关系至少抽出一条限定条件，
 * 否则就不能用它。
 *
 * 返回一条属于 rel 的 OR 子句（不是 RestrictInfo！），
 * 若抽不出 OR 子句则返回 NULL。
 */
static Expr *
extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel)
{
	List	   *clauselist = NIL;
	ListCell   *lc;

	/*
	 * 扫描输入 OR 子句的每一支。注意我们下降到
	 * or_rinfo->orclause，其中在顶层
	 * OR/AND 结构之下嵌有 RestrictInfo 节点。
	 * 这很有用，因为我们可以用那些节点里的信息让
	 * is_safe_restriction_clause_for() 的检查更便宜。不过我们会从返回的树中剥掉那些节点，
	 * 这意味着若该子句被接受为限制子句，会新建节点。
	 * 这也许显得浪费——难道不能复用
	 * 现有的 RestrictInfos？但那需要假定
	 * 选择率和其他缓存数据对限制子句和连接子句的计算方式完全相同，
	 * 这似乎不可取。
	 */
	Assert(is_orclause(or_rinfo->orclause));
	foreach(lc, ((BoolExpr *) or_rinfo->orclause)->args)
	{
		Node	   *orarg = (Node *) lfirst(lc);
		List	   *subclauses = NIL;
		Node	   *subclause;

		/* OR 的参数应当是 ANDs 或子 RestrictInfos */
		if (is_andclause(orarg))
		{
			List	   *andargs = ((BoolExpr *) orarg)->args;
			ListCell   *lc2;

			foreach(lc2, andargs)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				if (restriction_is_or_clause(rinfo))
				{
					/*
					 * 递归处理嵌套的 OR。注意这里必须递归，
					 * 这不是过分紧张的优化：我们必须
					 * 下降到足够深，找出并剥掉表达式中的全部
					 * RestrictInfos。
					 */
					Expr	   *suborclause;

					suborclause = extract_or_clause(rinfo, rel);
					if (suborclause)
						subclauses = lappend(subclauses, suborclause);
				}
				else if (is_safe_restriction_clause_for(rinfo, rel))
					subclauses = lappend(subclauses, rinfo->clause);
			}
		}
		else
		{
			RestrictInfo *rinfo = castNode(RestrictInfo, orarg);

			Assert(!restriction_is_or_clause(rinfo));
			if (is_safe_restriction_clause_for(rinfo, rel))
				subclauses = lappend(subclauses, rinfo->clause);
		}

		/*
		 * 若从这一支什么也抽不出，对这条 OR 子句就
		 * 什么也做不了。
		 */
		if (subclauses == NIL)
			return NULL;

		/*
		 * 好，把子子句加进结果 OR。若找到不止一条，
		 * 就需要一个 AND 节点。但若只找到一条，且它本身是
		 * OR 节点，则把它的子子句加进结果；这是为了
		 * 保持 AND/OR 平坦（即 OR 不直接位于 OR 之下）。
		 */
		subclause = (Node *) make_ands_explicit(subclauses);
		if (is_orclause(subclause))
			clauselist = list_concat(clauselist,
									 ((BoolExpr *) subclause)->args);
		else
			clauselist = lappend(clauselist, subclause);
	}

	/*
	 * 若每一支都得到了限制子句，用一个 OR
	 * 节点包起来。（理论上若只有一支，OR 节点也许不必要
	 * ——但那样输入的 OR 节点也同样多余。）
	 */
	if (clauselist != NIL)
		return make_orclause(clauselist);
	return NULL;
}

/*
 * 考虑成功抽出的限制 OR 子句是否
 * 真的值得用。若值得，把它加进规划器的数据结构，
 * 并调整原来的连接子句（join_or_rinfo）以作补偿。
 */
static void
consider_new_or_clause(PlannerInfo *root, RelOptInfo *rel,
					   Expr *orclause, RestrictInfo *join_or_rinfo)
{
	RestrictInfo *or_rinfo;
	Selectivity or_selec,
				orig_selec;

	/*
	 * 从新的 OR 子句建立一个 RestrictInfo。可以假定它作为
	 * 基限制子句是有效的。
	 */
	or_rinfo = make_restrictinfo(root,
								 orclause,
								 true,
								 false,
								 false,
								 false,
								 join_or_rinfo->security_level,
								 NULL,
								 NULL,
								 NULL);

	/*
	 * 估计它的选择率。（本来可以更早做，但在
	 * RestrictInfo 表示上做可以让结果被缓存，
	 * 以后省事。）
	 */
	or_selec = clause_selectivity(root, (Node *) or_rinfo,
								  0, JOIN_INNER, NULL);

	/*
	 * 只有当该子句拒绝掉基关系行的有用比例时，才值得加进查询；
	 * 否则它只会
	 * 造成重复计算（因为形成连接时仍须检查
	 * 原来的 OR 子句）。我们有点任意地把
	 * 选择率阈值设为 0.9。
	 */
	if (or_selec > 0.9)
		return;					/* 作罢 */

	/*
	 * 好，把它加进该关系的限制子句列表。
	 */
	rel->baserestrictinfo = lappend(rel->baserestrictinfo, or_rinfo);
	rel->baserestrict_min_security = Min(rel->baserestrict_min_security,
										 or_rinfo->security_level);

	/*
	 * 调整原来连接 OR 子句缓存的选择率，以补偿
	 * 新增的（但是冗余的）下层限定条件的选择率。这
	 * 应当使连接关系得到的行数估计，与不做这些花样时
	 * 大致相同。
	 *
	 * XXX 重大权宜之计警告：这依赖于如下假定，即
	 * 选择率会保持缓存。
	 *
	 * XXX 另一个重大权宜之计：我们只调整 norm_selec，
	 * 即 JOIN_INNER 语义下缓存的选择率，即使该连接子句
	 * 也许曾是外连接子句。部分原因是这里不能轻易找出
	 * 相关的 SpecialJoinInfo，
	 * 部分原因是我们所作的线性假定反正会失败。（若它是
	 * 外连接子句，rel 必须在可空一侧，否则我们到不了
	 * 这里。因此连接大小的计算相对于 rel 的大小
	 * 会相当非线性，所以即使能正确算出
	 * outer_selec 的原始值，也不清楚应当如何
	 * 调整它。）
	 */
	if (or_selec > 0)
	{
		SpecialJoinInfo sjinfo;

		/*
		 * 编造一个 JOIN_INNER 语义的 SpecialJoinInfo。（对照
		 * costsize.c 中的 approx_tuple_count()。）
		 */
		sjinfo.type = T_SpecialJoinInfo;
		sjinfo.min_lefthand = bms_difference(join_or_rinfo->clause_relids,
											 rel->relids);
		sjinfo.min_righthand = rel->relids;
		sjinfo.syn_lefthand = sjinfo.min_lefthand;
		sjinfo.syn_righthand = sjinfo.min_righthand;
		sjinfo.jointype = JOIN_INNER;
		sjinfo.ojrelid = 0;
		sjinfo.commute_above_l = NULL;
		sjinfo.commute_above_r = NULL;
		sjinfo.commute_below_l = NULL;
		sjinfo.commute_below_r = NULL;
		/* 我们不费心去让其余字段有效 */
		sjinfo.lhs_strict = false;
		sjinfo.semi_can_btree = false;
		sjinfo.semi_can_hash = false;
		sjinfo.semi_operators = NIL;
		sjinfo.semi_rhs_exprs = NIL;

		/* 计算内连接大小 */
		orig_selec = clause_selectivity(root, (Node *) join_or_rinfo,
										0, JOIN_INNER, &sjinfo);

		/* 并改写缓存的选择率，使连接大小保持不变 */
		join_or_rinfo->norm_selec = orig_selec / or_selec;
		/* 确保结果留在合理范围内 */
		if (join_or_rinfo->norm_selec > 1)
			join_or_rinfo->norm_selec = 1;
		/* 如上所述，我们不碰 outer_selec */
	}
}
