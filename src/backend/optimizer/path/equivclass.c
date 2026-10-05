/*-------------------------------------------------------------------------
 *
 * equivclass.c
 *	  管理 EquivalenceClasses 的例程
 *
 * 关于 EquivalenceClasses 的讨论，见 src/backend/optimizer/README。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/equivclass.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


static EquivalenceMember *add_eq_member(EquivalenceClass *ec,
										Expr *expr, Relids relids,
										JoinDomain *jdomain,
										EquivalenceMember *parent,
										Oid datatype);
static void generate_base_implied_equalities_const(PlannerInfo *root,
												   EquivalenceClass *ec);
static void generate_base_implied_equalities_no_const(PlannerInfo *root,
													  EquivalenceClass *ec);
static void generate_base_implied_equalities_broken(PlannerInfo *root,
													EquivalenceClass *ec);
static List *generate_join_implied_equalities_normal(PlannerInfo *root,
													 EquivalenceClass *ec,
													 Relids join_relids,
													 Relids outer_relids,
													 Relids inner_relids);
static List *generate_join_implied_equalities_broken(PlannerInfo *root,
													 EquivalenceClass *ec,
													 Relids nominal_join_relids,
													 Relids outer_relids,
													 Relids nominal_inner_relids,
													 RelOptInfo *inner_rel);
static Oid	select_equality_operator(EquivalenceClass *ec,
									 Oid lefttype, Oid righttype);
static RestrictInfo *create_join_clause(PlannerInfo *root,
										EquivalenceClass *ec, Oid opno,
										EquivalenceMember *leftem,
										EquivalenceMember *rightem,
										EquivalenceClass *parent_ec);
static bool reconsider_outer_join_clause(PlannerInfo *root,
										 OuterJoinClauseInfo *ojcinfo,
										 bool outer_on_left);
static bool reconsider_full_join_clause(PlannerInfo *root,
										OuterJoinClauseInfo *ojcinfo);
static JoinDomain *find_join_domain(PlannerInfo *root, Relids relids);
static Bitmapset *get_eclass_indexes_for_relids(PlannerInfo *root,
												Relids relids);
static Bitmapset *get_common_eclass_indexes(PlannerInfo *root, Relids relids1,
											Relids relids2);


/*
 * process_equivalence
 *	  给定子句有可归并连接的操作符，且不是外连接
 *	  限定条件，因此只要两侧都能计算，就可以认为它们
 *	  处处相等；而且该相等可以
 *	  传递地扩展。若适用，把这一知识记入 EquivalenceClass
 *	  数据结构。成功返回 true，否则返回 false
 *	  （此时调用方应把该子句当作普通子句，而不是
 *	  等值关系）。
 *
 * 有些情形下，虽然无法把子句转换成 EquivalenceClass
 * 知识，仍可以把它改成比原来更有用的形式。
 * 此时 *p_restrictinfo 会被换成新的 RestrictInfo，这就是
 * 调用方后续处理应当使用的对象。
 *
 * jdomain 是找到该子句时所在的连接域。
 * 这限制了从 EquivalenceClass 推出的结论能用在哪里，
 * 如 optimizer/README 所述。
 *
 * 若提议的等值子句含有泄漏函数
 * 且 security_level 大于零，我们就拒绝。EC 的求值规则需要我们
 * 在特定的连接层次施加特定测试，我们无法容忍
 * 以 security_level 为由推迟任何测试。拒绝那些
 * 可能需要安全推迟的候选子句，就能保证 EC
 * 子句可以在应当施加的时候立刻施加。
 *
 * 成功返回时，我们还初始化了子句的 left_ec/right_ec
 * 字段，指向表示它的 EquivalenceClass。这省去了以后的
 * 查找开销。
 *
 * 注意：构造合并后的 EquivalenceClasses 是标准的 UNION-FIND
 * 问题，对此存在比简单列表更好的数据结构。
 * 若这段代码成为瓶颈，可以再加速 ---
 * 但目前，简单就是美。
 *
 * 注意：本函数只在规划器启动期间调用，不在 GEQO
 * 探索期间调用，因此不必担心是否处在正确的
 * 内存上下文中。
 */
bool
process_equivalence(PlannerInfo *root,
					RestrictInfo **p_restrictinfo,
					JoinDomain *jdomain)
{
	RestrictInfo *restrictinfo = *p_restrictinfo;
	Expr	   *clause = restrictinfo->clause;
	Oid			opno,
				collation,
				item1_type,
				item2_type;
	Expr	   *item1;
	Expr	   *item2;
	Relids		item1_relids,
				item2_relids;
	List	   *opfamilies;
	EquivalenceClass *ec1,
			   *ec2;
	EquivalenceMember *em1,
			   *em2;
	ListCell   *lc1;
	int			ec2_idx;

	/* 不应当已被标记为生成过 eclass */
	Assert(restrictinfo->left_ec == NULL);
	Assert(restrictinfo->right_ec == NULL);

	/* 若可能因安全考虑而推迟，则拒绝 */
	if (restrictinfo->security_level > 0 && !restrictinfo->leakproof)
		return false;

	/* 从给定子句中提取信息 */
	Assert(is_opclause(clause));
	opno = ((OpExpr *) clause)->opno;
	collation = ((OpExpr *) clause)->inputcollid;
	item1 = (Expr *) get_leftop(clause);
	item2 = (Expr *) get_rightop(clause);
	item1_relids = restrictinfo->left_relids;
	item2_relids = restrictinfo->right_relids;

	/*
	 * 确保两个输入表达式都暴露出所要的排序规则（它们的类型
	 * 应当已经没问题）；见 canonicalize_ec_expression 的注释。
	 */
	item1 = canonicalize_ec_expression(item1,
									   exprType((Node *) item1),
									   collation);
	item2 = canonicalize_ec_expression(item2,
									   exprType((Node *) item2),
									   collation);

	/*
	 * 形如 X=X 的子句无法翻译成 EquivalenceClasses。
	 * 否则要么得到单项 EC，从而丢失该子句曾经存在的知识，
	 * 要么造出带重复项的 EC，
	 * 引起别的问题。
	 */
	if (equal(item1, item2))
	{
		/*
		 * 若操作符是严格的，该子句可以只当作
		 * "X IS NOT NULL"。（既然考虑的是顶层
		 * 限定条件，就可以忽略 FALSE 与 NULL 结果的差别。）
		 * 值得做这个转换，因为通常会得到比 X=X
		 * 好得多的选择率估计。
		 *
		 * 若操作符不严格，就无法确定它对
		 * NULLs 会做什么，因此不要试图优化它。
		 */
		set_opfuncid((OpExpr *) clause);
		if (func_strict(((OpExpr *) clause)->opfuncid))
		{
			NullTest   *ntest = makeNode(NullTest);

			ntest->arg = item1;
			ntest->nulltesttype = IS_NOT_NULL;
			ntest->argisrow = false;	/* 即使参数是复合类型也正确 */
			ntest->location = -1;

			*p_restrictinfo =
				make_restrictinfo(root,
								  (Expr *) ntest,
								  restrictinfo->is_pushed_down,
								  restrictinfo->has_clone,
								  restrictinfo->is_clone,
								  restrictinfo->pseudoconstant,
								  restrictinfo->security_level,
								  NULL,
								  restrictinfo->incompatible_relids,
								  restrictinfo->outer_relids);
		}
		return false;
	}

	/*
	 * 我们用操作符声明的输入类型，而不是输入的 exprType()
	 * 结果，作为操作符族查找的名义数据类型。这假定
	 * B-树操作符登记时 amoplefttype 与
	 * amoprighttype 总等于其声明的输入类型。反正构造
	 * EquivalenceMember 节点也需要这些信息，现在取出来
	 * 就可以用类型比较短路某些 equal() 测试。
	 */
	op_input_types(opno, &item1_type, &item2_type);

	opfamilies = restrictinfo->mergeopfamilies;

	/*
	 * 遍历已有的 EquivalenceClasses，寻找与
	 * item1 和 item2 的匹配。可能的结果是：
	 *
	 * 1. 两者在同一个 EC 中。等值关系已知，
	 * 因此无事可做。
	 *
	 * 2. 两者在不同的 EC 中。把两个 EC 合并。
	 *
	 * 3. 只找到一个。把另一个加入它的 EC。
	 *
	 * 4. 一个都没找到。新建一个两项 EC。
	 *
	 * 注意：因为所有 EC 都经此过程，或经
	 * get_eclass_for_sort_expr() 中类似的搜索建成，不可能在
	 * 不止一个已有的非易变 EC 中匹配到同一项。因此在
	 * 第一次匹配处停下是可以的。
	 */
	ec1 = ec2 = NULL;
	em1 = em2 = NULL;
	ec2_idx = -1;
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/* 永不匹配易变 EC */
		if (cur_ec->ec_has_volatile)
			continue;

		/*
		 * 排序规则必须匹配；先查这个，因为它比
		 * 操作符族比较更便宜。
		 */
		if (collation != cur_ec->ec_collation)
			continue;

		/*
		 * 一次 "匹配" 需要匹配的 B-树操作符族集合。对此使用
		 * equal() 的影响，在
		 * get_mergejoin_opfamilies() 的注释里讨论过。
		 */
		if (!equal(opfamilies, cur_ec->ec_opfamilies))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			Assert(!cur_em->em_is_child);	/* 还没有子项 */

			/*
			 * 只在同一 JoinDomain 内匹配常量（见
			 * optimizer/README）。
			 */
			if (cur_em->em_is_const && cur_em->em_jdomain != jdomain)
				continue;

			if (!ec1 &&
				item1_type == cur_em->em_datatype &&
				equal(item1, cur_em->em_expr))
			{
				ec1 = cur_ec;
				em1 = cur_em;
				if (ec2)
					break;
			}

			if (!ec2 &&
				item2_type == cur_em->em_datatype &&
				equal(item2, cur_em->em_expr))
			{
				ec2 = cur_ec;
				ec2_idx = foreach_current_index(lc1);
				em2 = cur_em;
				if (ec1)
					break;
			}
		}

		if (ec1 && ec2)
			break;
	}

	/* 扫描结束，找到了什么？ */

	if (ec1 && ec2)
	{
		/* 若是情形 1，除了加入来源外无事可做 */
		if (ec1 == ec2)
		{
			ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
			ec1->ec_min_security = Min(ec1->ec_min_security,
									   restrictinfo->security_level);
			ec1->ec_max_security = Max(ec1->ec_max_security,
									   restrictinfo->security_level);
			/* 把该 RI 标记为与此 eclass 相关联 */
			restrictinfo->left_ec = ec1;
			restrictinfo->right_ec = ec1;
			/* 把该 RI 标记为可用于这一对 EM */
			restrictinfo->left_em = em1;
			restrictinfo->right_em = em2;
			return true;
		}

		/*
		 * 情形 2：需要合并 ec1 和 ec2。EC 到达规范状态之后
		 * 不应当再发生；否则合并会使路径键
		 * 不再规范，而且删掉 eq_classes 列表项会弄坏
		 * 关系的 eclass 索引。
		 */
		if (root->ec_merging_done)
			elog(ERROR, "too late to merge equivalence classes");

		/*
		 * 我们把 ec2 的项加入 ec1，再让 ec2 的 ec_merged 链接指向
		 * ec1，并把 ec2 从 eq_classes 列表中去掉。无法简单地
		 * 删除 ec2，因为那会在已有的
		 * PathKeys 里留下悬空指针。我们把它留下来并带上链接，以便能找到
		 * 合并后的 EC。
		 */
		ec1->ec_members = list_concat(ec1->ec_members, ec2->ec_members);
		ec1->ec_sources = list_concat(ec1->ec_sources, ec2->ec_sources);
		ec1->ec_derives = list_concat(ec1->ec_derives, ec2->ec_derives);
		ec1->ec_relids = bms_join(ec1->ec_relids, ec2->ec_relids);
		ec1->ec_has_const |= ec2->ec_has_const;
		/* 不可能还需要设置 has_volatile */
		ec1->ec_min_security = Min(ec1->ec_min_security,
								   ec2->ec_min_security);
		ec1->ec_max_security = Max(ec1->ec_max_security,
								   ec2->ec_max_security);
		ec2->ec_merged = ec1;
		root->eq_classes = list_delete_nth_cell(root->eq_classes, ec2_idx);
		/* 只是为了避免悬空指针造成调试混乱： */
		ec2->ec_members = NIL;
		ec2->ec_sources = NIL;
		ec2->ec_derives = NIL;
		ec2->ec_relids = NULL;
		ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
		ec1->ec_min_security = Min(ec1->ec_min_security,
								   restrictinfo->security_level);
		ec1->ec_max_security = Max(ec1->ec_max_security,
								   restrictinfo->security_level);
		/* 把该 RI 标记为与此 eclass 相关联 */
		restrictinfo->left_ec = ec1;
		restrictinfo->right_ec = ec1;
		/* 把该 RI 标记为可用于这一对 EM */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec1)
	{
		/* 情形 3：把 item2 加入 ec1 */
		em2 = add_eq_member(ec1, item2, item2_relids,
							jdomain, NULL, item2_type);
		ec1->ec_sources = lappend(ec1->ec_sources, restrictinfo);
		ec1->ec_min_security = Min(ec1->ec_min_security,
								   restrictinfo->security_level);
		ec1->ec_max_security = Max(ec1->ec_max_security,
								   restrictinfo->security_level);
		/* 把该 RI 标记为与此 eclass 相关联 */
		restrictinfo->left_ec = ec1;
		restrictinfo->right_ec = ec1;
		/* 把该 RI 标记为可用于这一对 EM */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else if (ec2)
	{
		/* 情形 3：把 item1 加入 ec2 */
		em1 = add_eq_member(ec2, item1, item1_relids,
							jdomain, NULL, item1_type);
		ec2->ec_sources = lappend(ec2->ec_sources, restrictinfo);
		ec2->ec_min_security = Min(ec2->ec_min_security,
								   restrictinfo->security_level);
		ec2->ec_max_security = Max(ec2->ec_max_security,
								   restrictinfo->security_level);
		/* 把该 RI 标记为与此 eclass 相关联 */
		restrictinfo->left_ec = ec2;
		restrictinfo->right_ec = ec2;
		/* 把该 RI 标记为可用于这一对 EM */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}
	else
	{
		/* 情形 4：新建一个两项 EC */
		EquivalenceClass *ec = makeNode(EquivalenceClass);

		ec->ec_opfamilies = opfamilies;
		ec->ec_collation = collation;
		ec->ec_members = NIL;
		ec->ec_sources = list_make1(restrictinfo);
		ec->ec_derives = NIL;
		ec->ec_relids = NULL;
		ec->ec_has_const = false;
		ec->ec_has_volatile = false;
		ec->ec_broken = false;
		ec->ec_sortref = 0;
		ec->ec_min_security = restrictinfo->security_level;
		ec->ec_max_security = restrictinfo->security_level;
		ec->ec_merged = NULL;
		em1 = add_eq_member(ec, item1, item1_relids,
							jdomain, NULL, item1_type);
		em2 = add_eq_member(ec, item2, item2_relids,
							jdomain, NULL, item2_type);

		root->eq_classes = lappend(root->eq_classes, ec);

		/* 把该 RI 标记为与此 eclass 相关联 */
		restrictinfo->left_ec = ec;
		restrictinfo->right_ec = ec;
		/* 把该 RI 标记为可用于这一对 EM */
		restrictinfo->left_em = em1;
		restrictinfo->right_em = em2;
	}

	return true;
}

/*
 * canonicalize_ec_expression
 *
 * 本函数确保表达式暴露出预期的类型和
 * 排序规则，从而能与它应当 equal() 到的其他等价类表达式
 * equal() 相等。
 *
 * 数据类型的规则是：暴露类型应当与它作为
 * 该 EC 操作符族中某个操作符的输入时相同；通常就是
 * 操作符声明的输入类型，但多态
 * 操作符不必重标记（对照 parse_coerce.c 的行为）。
 * 来自限定条件的表达式通常类型已经正确，
 * 但来自索引键的表达式也许不对（因为它们在
 * pg_index 中没有任何显式重标记），排序表达式也有
 * 同样的问题（解析器同样随意地
 * 给它们加不加重标记）。这些情形与真正的
 * 操作符二进制兼容，因此加上 RelabelType 就够了。
 *
 * 另外，表达式暴露的排序规则必须与 EC 的排序规则一致。
 * 这很重要，因为像 "foo < bar COLLATE baz" 这样的比较里，
 * 我们从解析器收到时，只有其中一个表达式的暴露排序规则是对的。
 * 强制两者都带上它，才能保证这种结构的所有
 * 不同写法行为相同。同样，我们可以
 * 贴上 RelabelType 来强制正确的暴露排序规则。（也可以
 * 完全不给 EC 成员标排序规则，但那样有风险，
 * 因为系统有些部分预期 exprCollation() 会给出
 * 排序键的正确答案。）
 */
Expr *
canonicalize_ec_expression(Expr *expr, Oid req_type, Oid req_collation)
{
	Oid			expr_type = exprType((Node *) expr);

	/*
	 * 对多态输入类型的 opclass，保持原来的暴露类型。
	 * RECORD opclass 在这里与多态类型的一样处理。
	 */
	if (IsPolymorphicType(req_type) || req_type == RECORDOID)
		req_type = expr_type;

	/*
	 * 若表达式已经暴露出正确的类型/排序规则，就无事可做。
	 */
	if (expr_type != req_type ||
		exprCollation((Node *) expr) != req_collation)
	{
		/*
		 * 若必须改变表达式的类型，把 typmod 设为 -1，
		 * 因为新类型对 typmod 的解释也许不同。
		 * 若只改排序规则，就保留暴露的 typmod。
		 */
		int32		req_typmod;

		if (expr_type != req_type)
			req_typmod = -1;
		else
			req_typmod = exprTypmod((Node *) expr);

		/*
		 * 用 applyRelabelType，以便保持常量平坦。这很
		 * 重要，因为 eval_const_expressions 已经施加过了。
		 */
		expr = (Expr *) applyRelabelType((Node *) expr,
										 req_type, req_typmod, req_collation,
										 COERCE_IMPLICIT_CAST, -1, false);
	}

	return expr;
}

/*
 * add_eq_member - 构造新的 EquivalenceMember 并加入某个 EC
 */
static EquivalenceMember *
add_eq_member(EquivalenceClass *ec, Expr *expr, Relids relids,
			  JoinDomain *jdomain, EquivalenceMember *parent, Oid datatype)
{
	EquivalenceMember *em = makeNode(EquivalenceMember);

	em->em_expr = expr;
	em->em_relids = relids;
	em->em_is_const = false;
	em->em_is_child = (parent != NULL);
	em->em_datatype = datatype;
	em->em_jdomain = jdomain;
	em->em_parent = parent;

	if (bms_is_empty(relids))
	{
		/*
		 * 没有 Vars，就假定它是伪常量。对
		 * process_equivalence() 生成的项这是对的，因为 WHERE 子句不能
		 * 包含聚集或 SRFs，而且在
		 * process_equivalence() 被调用之前已经检查过非易变性。但
		 * get_eclass_for_sort_expr() 得更费力。我们把测试放在
		 * 那里而不是这里，以便在等值情形下省掉这些周期。
		 */
		Assert(!parent);
		em->em_is_const = true;
		ec->ec_has_const = true;
		/* 它影响不了 ec_relids */
	}
	else if (!parent)			/* 子成员不加入 ec_relids */
	{
		ec->ec_relids = bms_add_members(ec->ec_relids, relids);
	}
	ec->ec_members = lappend(ec->ec_members, em);

	return em;
}


/*
 * get_eclass_for_sort_expr
 *	  给定表达式以及操作符族/排序规则信息，找出它所属的已有
 *	  等价类；若没有，可选地为它新建一个
 *	  单成员 EquivalenceClass。
 *
 * sortref 是来源 SortGroupClause 的 SortGroupRef（若有），
 * 否则为零。（表达式易变时它绝不应当为零！）
 *
 * 若 rel 不是 NULL，它标识我们正在为其考虑
 * 路径的某个特定关系，并表示可以考虑该关系的子 EC 成员。
 * 否则忽略子成员。（注意：子 EC
 * 成员不保证唯一，非 NULL 意味着可能
 * 有不止一个 EC 匹配该表达式；若是这样，你得到哪一个取决于顺序。
 * 这很烦人，但只发生在边角情形，
 * 所以目前我们接受只报告第一个匹配。另见
 * generate_implied_equalities_for_column 和 match_pathkeys_to_index。）
 *
 * 若 create_it 为 true，没有匹配时就新建 EquivalenceClass。
 * 若 create_it 为 false，没有匹配就返回 NULL。
 *
 * EquivalenceClass 合并前后都可以安全使用本函数；
 * 它从不引起合并，因此不会使已有 EC
 * 或 PathKeys 失效。不过，路径生成开始之后才加入的 EC
 * 用处有限，所以通常最好事先创建。
 *
 * 注意：操作符族的选择必须与
 * process_equivalence() 的做法一致，也就是从可归并连接的
 * 等值操作符生成。否则我们也许检测不到有效的等值关系，
 * 从而生成较差（但并非不正确）的计划。
 */
EquivalenceClass *
get_eclass_for_sort_expr(PlannerInfo *root,
						 Expr *expr,
						 List *opfamilies,
						 Oid opcintype,
						 Oid collation,
						 Index sortref,
						 Relids rel,
						 bool create_it)
{
	JoinDomain *jdomain;
	Relids		expr_relids;
	EquivalenceClass *newec;
	EquivalenceMember *newem;
	ListCell   *lc1;
	MemoryContext oldcontext;

	/*
	 * 确保表达式暴露出正确的类型和排序规则。
	 */
	expr = canonicalize_ec_expression(expr, opcintype, collation);

	/*
	 * SortGroupClause 节点是顶层表达式（GROUP BY、ORDER
	 * BY 等），可以认为它们属于最顶层 JoinDomain。
	 */
	jdomain = linitial_node(JoinDomain, root->join_domains);

	/*
	 * 扫描已有的 EquivalenceClasses 寻找匹配
	 */
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		ListCell   *lc2;

		/*
		 * 永不匹配易变 EC，除非我们正在看同一条易变
		 * SortGroupClause 的另一处引用。
		 */
		if (cur_ec->ec_has_volatile &&
			(sortref == 0 || sortref != cur_ec->ec_sortref))
			continue;

		if (collation != cur_ec->ec_collation)
			continue;
		if (!equal(opfamilies, cur_ec->ec_opfamilies))
			continue;

		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			/*
			 * 除非子成员符合请求，否则忽略它们。
			 */
			if (cur_em->em_is_child &&
				!bms_equal(cur_em->em_relids, rel))
				continue;

			/*
			 * 只在同一 JoinDomain 内匹配常量（见
			 * optimizer/README）。
			 */
			if (cur_em->em_is_const && cur_em->em_jdomain != jdomain)
				continue;

			if (opcintype == cur_em->em_datatype &&
				equal(expr, cur_em->em_expr))
				return cur_ec;	/* 匹配！ */
		}
	}

	/* 没有匹配；调用方是否想要 NULL 结果？ */
	if (!create_it)
		return NULL;

	/*
	 * 好，新建一个单成员 EC
	 *
	 * 这里必须确保在正确的上下文中构造该 EC。
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	newec = makeNode(EquivalenceClass);
	newec->ec_opfamilies = list_copy(opfamilies);
	newec->ec_collation = collation;
	newec->ec_members = NIL;
	newec->ec_sources = NIL;
	newec->ec_derives = NIL;
	newec->ec_relids = NULL;
	newec->ec_has_const = false;
	newec->ec_has_volatile = contain_volatile_functions((Node *) expr);
	newec->ec_broken = false;
	newec->ec_sortref = sortref;
	newec->ec_min_security = UINT_MAX;
	newec->ec_max_security = 0;
	newec->ec_merged = NULL;

	if (newec->ec_has_volatile && sortref == 0) /* 不应当发生 */
		elog(ERROR, "volatile EquivalenceClass has no sortref");

	/*
	 * 取得表达式中出现的精确 relid 集合。
	 */
	expr_relids = pull_varnos(root, (Node *) expr);

	newem = add_eq_member(newec, copyObject(expr), expr_relids,
						  jdomain, NULL, opcintype);

	/*
	 * add_eq_member 不检查易变函数、集合返回
	 * 函数、聚集或窗口函数，但排序表达式里可能出现这些；
	 * 因此必须检查它对常量的标记是否
	 * 正确。
	 */
	if (newec->ec_has_const)
	{
		if (newec->ec_has_volatile ||
			expression_returns_set((Node *) expr) ||
			contain_agg_clause((Node *) expr) ||
			contain_window_function((Node *) expr))
		{
			newec->ec_has_const = false;
			newem->em_is_const = false;
		}
	}

	root->eq_classes = lappend(root->eq_classes, newec);

	/*
	 * 若 EC 合并已经完成，就得补做：把新
	 * EC 加入它所提到的 relation(s) 的 eclass_indexes。
	 */
	if (root->ec_merging_done)
	{
		int			ec_index = list_length(root->eq_classes) - 1;
		int			i = -1;

		while ((i = bms_next_member(newec->ec_relids, i)) > 0)
		{
			RelOptInfo *rel = root->simple_rel_array[i];

			if (rel == NULL)	/* 必须是外连接 */
			{
				Assert(bms_is_member(i, root->outer_join_rels));
				continue;
			}

			Assert(rel->reloptkind == RELOPT_BASEREL);

			rel->eclass_indexes = bms_add_member(rel->eclass_indexes,
												 ec_index);
		}
	}

	MemoryContextSwitchTo(oldcontext);

	return newec;
}

/*
 * find_ec_member_matching_expr
 *		找出与给定 expr 匹配的 EquivalenceClass 成员（若有）；
 *		没有匹配则返回 NULL。
 *
 * "匹配" 定义为 "剥掉 RelabelTypes 之后相等"。
 * 这用于识别排序表达式，有些涉及二进制兼容
 * 排序操作符的情形需要允许
 * 二进制兼容的重标记。
 *
 * 除非子 EC 成员属于给定的 relids，否则忽略它们。
 */
EquivalenceMember *
find_ec_member_matching_expr(EquivalenceClass *ec,
							 Expr *expr,
							 Relids relids)
{
	ListCell   *lc;

	/* 两端的二进制兼容重标记都忽略 */
	while (expr && IsA(expr, RelabelType))
		expr = ((RelabelType *) expr)->arg;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *emexpr;

		/*
		 * 不应当试图按含有常量的等价类排序，
		 * 因此不必再考虑这些情形。
		 */
		if (em->em_is_const)
			continue;

		/*
		 * 除非子成员属于所请求的关系，否则忽略它们。
		 */
		if (em->em_is_child &&
			!bms_is_subset(em->em_relids, relids))
			continue;

		/*
		 * 若表达式相同（剥掉重标记之后）则匹配。
		 */
		emexpr = em->em_expr;
		while (emexpr && IsA(emexpr, RelabelType))
			emexpr = ((RelabelType *) emexpr)->arg;

		if (equal(emexpr, expr))
			return em;
	}

	return NULL;
}

/*
 * find_computable_ec_member
 *		找出一个能从 "exprs" 中出现的表达式计算出来的 EquivalenceClass 成员；
 *		没有匹配则返回 NULL。
 *
 * "exprs" 可以是裸表达式树的列表，也可以是
 * TargetEntry 节点的列表。通常它含有 Vars，也许还有 Aggrefs
 * 和 WindowFuncs；不过在考虑 appendrel 成员时，列表
 * 可能含有任意表达式。若某个 EC 成员所需的全部 Vars、PlaceHolderVars、Aggrefs 和 WindowFuncs
 * 都出现在 "exprs" 中，就认为它
 * 可计算。
 *
 * 这个定义有点微妙：例如 EC 成员是
 * Var_A + 1，而 "exprs" 里是 Var_A + 2，它仍然可计算。
 * 这是因为在最终计划树里，EC 成员的表达式会
 * 作为当前由 "exprs" 表示的同一计划节点目标列表的一部分来
 * 计算。因此若已有目标列表成员能用到 Var_A，
 * 在 EC 表达式里用它也应当可以。
 *
 * 与 find_ec_member_matching_expr 不同，这里不为
 * 二进制兼容重标记做特殊处理。这是有意的：若必须
 * 以这种方式计算表达式，setrefs.c 会坚持 Vars 与源目标列表
 * 精确匹配。
 *
 * 除非子 EC 成员属于给定的 relids，否则忽略它们。
 * 另外，若 require_parallel_safe，则忽略非并行安全的表达式。
 *
 * 注意：有些调用方出于写法原因传入 root == NULL。当
 * require_parallel_safe 为 false 时这是可以的。
 */
EquivalenceMember *
find_computable_ec_member(PlannerInfo *root,
						  EquivalenceClass *ec,
						  List *exprs,
						  Relids relids,
						  bool require_parallel_safe)
{
	List	   *exprvars;
	ListCell   *lc;

	/*
	 * 抽出 "exprs" 中的 Vars 和准 Vars。在典型的
	 * 非 appendrel 情形下，这只是同一列表的另一种表示。
	 * 不过，它确实消除了普通表达式列表与
	 * TargetEntrys 列表这两种情形的差别。
	 */
	exprvars = pull_var_clause((Node *) exprs,
							   PVC_INCLUDE_AGGREGATES |
							   PVC_INCLUDE_WINDOWFUNCS |
							   PVC_INCLUDE_PLACEHOLDERS);

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		List	   *emvars;
		ListCell   *lc2;

		/*
		 * 不应当试图按含有常量的等价类排序，
		 * 因此不必再考虑这些情形。
		 */
		if (em->em_is_const)
			continue;

		/*
		 * 除非子成员属于所请求的关系，否则忽略它们。
		 */
		if (em->em_is_child &&
			!bms_is_subset(em->em_relids, relids))
			continue;

		/*
		 * 若全部 Vars 和准 Vars 都出现在 "exprs" 中，则匹配。
		 */
		emvars = pull_var_clause((Node *) em->em_expr,
								 PVC_INCLUDE_AGGREGATES |
								 PVC_INCLUDE_WINDOWFUNCS |
								 PVC_INCLUDE_PLACEHOLDERS);
		foreach(lc2, emvars)
		{
			if (!list_member(exprvars, lfirst(lc2)))
				break;
		}
		list_free(emvars);
		if (lc2)
			continue;			/* 碰到了不可用的 Var */

		/*
		 * 若要求，则拒绝非并行安全的表达式。我们
		 * 最后才做这个检查，因为它相当贵。
		 */
		if (require_parallel_safe &&
			!is_parallel_safe(root, (Node *) em->em_expr))
			continue;

		return em;				/* 找到了可用表达式 */
	}

	return NULL;
}

/*
 * relation_can_be_sorted_early
 *		这个关系能否在最终输出步骤之前按该 EC 排序？
 *
 * 要成功，必须找到一个 EC 成员，使得 prepare_sort_from_pathkeys 知道
 * 在以该关系的 reltarget 为输入时如何按它排序。另外还有几条
 * 约束，因为所要的排序会
 * "提早" 做，在计划的扫描/连接部分之内。另外，若 require_parallel_safe，
 * 则忽略非并行安全的表达式。
 *
 * 将来也许想返回找到的 EquivalenceMember，
 * 但目前调用方只想知道有没有。
 */
bool
relation_can_be_sorted_early(PlannerInfo *root, RelOptInfo *rel,
							 EquivalenceClass *ec, bool require_parallel_safe)
{
	PathTarget *target = rel->reltarget;
	EquivalenceMember *em;
	ListCell   *lc;

	/*
	 * 立即拒绝易变 EC；这种排序必须一直推迟。
	 */
	if (ec->ec_has_volatile)
		return false;

	/*
	 * 试着找一个直接匹配某个 reltarget 成员的 EM。
	 */
	foreach(lc, target->exprs)
	{
		Expr	   *targetexpr = (Expr *) lfirst(lc);

		em = find_ec_member_matching_expr(ec, targetexpr, rel->relids);
		if (!em)
			continue;

		/*
		 * 拒绝涉及集合返回函数的表达式，那些
		 * 也不能提早计算。（注意：本测试和下面
		 * 那个实际上是在检查 targetexpr 的性质，因此
		 * 不必问别的 EC 成员会不会更好。）
		 */
		if (expression_returns_set((Node *) em->em_expr))
			continue;

		/*
		 * 若要求，则拒绝非并行安全的表达式。我们
		 * 最后才做这个检查，因为它相当贵。
		 */
		if (require_parallel_safe &&
			!is_parallel_safe(root, (Node *) em->em_expr))
			continue;

		return true;
	}

	/*
	 * 试着找一个能从 reltarget 计算出来的表达式。
	 */
	em = find_computable_ec_member(root, ec, target->exprs, rel->relids,
								   require_parallel_safe);
	if (!em)
		return false;

	/*
	 * 拒绝涉及集合返回函数的表达式，那些也无法
	 * 提早计算。（这种情形不必再找别的 EC
	 * 成员；因为 SRFs 无法出现在 WHERE 里，它们无法
	 * 属于多成员 EC。）
	 */
	if (expression_returns_set((Node *) em->em_expr))
		return false;

	return true;
}

/*
 * generate_base_implied_equalities
 *	  生成我们能从等价类推出的任何限制
 *	  子句。
 *
 * 当 EC 含有伪常量时，策略是生成
 * "member = const1" 子句，其中 const1 是第一个常量成员，对
 * 其余每个成员（包括其他常量）都生成。若做得到，
 * 就不需要任何 "var = var" 比较，因为已经在创建点
 * 成功约束了全部变量。若因缺少跨类型操作符而未能
 * 生成其中任何子句，就退回到下面描述的 "ec_broken" 策略。（XXX 若有
 * 多个不同类型的常量，从另一个常量
 * 成员出发也许能组成全部所需子句；
 * 但这是够古怪的边角情形，不值得
 * 花很多周期。）
 * 对不含伪常量的 EC，我们为属于同一基
 *
 * 关系的每一对成员生成派生子句 "member1 = member2"
 * （实际上，同一基关系若超过两个，
 * 只需要足够把它们彼此连起来的子句）。这提供了
 * 递归的基础情形：基关系扫描发出的每一行
 * 都会把该 EC 中全部可计算成员约束为相等。每形成一条
 * 连接路径，就会当场追加派生子句
 * 来维持这个不变量（见 generate_join_implied_equalities）。
 * 若 EC 使用的操作符族没有提供完整的跨类型
 *
 * 等值操作符集合，就可能无法生成一条
 * 为维持不变量而必须生成的子句。（例如：给定
 * "WHERE a.x = b.y AND b.y = a.z"，若无法
 * 生成 "a.x = a.z" 作为 A 的限制子句，这套方案就垮了。）此时把
 * 该 EC 标成 "ec_broken"，并退回到在适当时候吐回其原始来源
 * RestrictInfos。我们不试图收回已从损坏 EC 生成的派生
 * 子句，因此所得计划可能
 * 因冗余子句造成的选择率估计偏差而变差。但
 * 正确的解决办法是修好操作符族……
 * 本函数推出的等值子句交给
 *
 * process_implied_equality（在 plan/initsplan.c 中），以便插入
 * restrictinfo 数据结构。注意必须在初次
 * 扫描限定条件之后、
 * Path 构造开始之前调用。
 *
 * 这里不试图避免生成重复的 RestrictInfos：
 * 不在 ec_sources 或 ec_derives 里找匹配。这么做
 * 似乎不值得费那个事。
 */
void
generate_base_implied_equalities(PlannerInfo *root)
{
	int			ec_index;
	ListCell   *lc;

	/*
	 * 到这里，查询中的等值知识已经吸收完，
	 * 因此不应当再发生 EC 合并，留在
	 * eq_classes 列表中的 EC 可以视为规范的。（但注意仍
	 * 可能经由
	 * get_eclass_for_sort_expr() 加入新的单成员 EC。）
	 */
	root->ec_merging_done = true;

	ec_index = 0;
	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		bool		can_generate_joinclause = false;
		int			i;

		Assert(ec->ec_merged == NULL);	/* 否则不应当在列表中 */
		Assert(!ec->ec_broken); /* 反正现在还没有…… */

		/*
		 * 生成作为限制子句的隐含等值。
		 * 单成员 EC 在这里或在
		 * 连接层都不会产生任何推导。
		 */
		if (list_length(ec->ec_members) > 1)
		{
			if (ec->ec_has_const)
				generate_base_implied_equalities_const(root, ec);
			else
				generate_base_implied_equalities_no_const(root, ec);

			/* 若未能生成所需的派生子句，则回退处理 */
			if (ec->ec_broken)
				generate_base_implied_equalities_broken(root, ec);

			/* 检测该 EC 是否可能生成连接子句 */
			can_generate_joinclause =
				(bms_membership(ec->ec_relids) == BMS_MULTIPLE);
		}

		/*
		 * 给每个 eclass 所引用的基关系（此刻应当都已存在）
		 * 标上提到它们的全部 eclass 在 eq_classes 中的索引。
		 * 这样以后查找就不必再搜索。同时
		 * 还可以标出有待处理 eclass 连接的基关系；
		 * 这是 has_relevant_eclass_joinclause() 的廉价版本。
		 */
		i = -1;
		while ((i = bms_next_member(ec->ec_relids, i)) > 0)
		{
			RelOptInfo *rel = root->simple_rel_array[i];

			if (rel == NULL)	/* 必须是外连接 */
			{
				Assert(bms_is_member(i, root->outer_join_rels));
				continue;
			}

			Assert(rel->reloptkind == RELOPT_BASEREL);

			rel->eclass_indexes = bms_add_member(rel->eclass_indexes,
												 ec_index);

			if (can_generate_joinclause)
				rel->has_eclass_joins = true;
		}

		ec_index++;
	}
}

/*
 * generate_base_implied_equalities，当 EC 含有 pseudoconstant(s) 时
 */
static void
generate_base_implied_equalities_const(PlannerInfo *root,
									   EquivalenceClass *ec)
{
	EquivalenceMember *const_em = NULL;
	ListCell   *lc;

	/*
	 * 在只曾有一条 "var = const" 子句的平凡情形下，把
	 * 原始子句推回主规划器机制。换种做法
	 * 没有好处，还能省掉
	 * 重建并重新分析一条将与旧子句完全
	 * 等值的等值子句的功夫。
	 */
	if (list_length(ec->ec_members) == 2 &&
		list_length(ec->ec_sources) == 1)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) linitial(ec->ec_sources);

		distribute_restrictinfo_to_rels(root, restrictinfo);
		return;
	}

	/*
	 * 找出要使用的常量成员。我们更喜欢真正的常量，而不是
	 * 伪常量（例如 Params），因为约束排除
	 * 机制也许能根据生成的
	 * "var = const" 等值排除关系，但 "var = param" 做不到。
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (cur_em->em_is_const)
		{
			const_em = cur_em;
			if (IsA(cur_em->em_expr, Const))
				break;
		}
	}
	Assert(const_em != NULL);

	/* 对其余每个成员生成一条派生等值 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		Oid			eq_op;
		RestrictInfo *rinfo;

		Assert(!cur_em->em_is_child);	/* 还没有子项 */
		if (cur_em == const_em)
			continue;
		eq_op = select_equality_operator(ec,
										 cur_em->em_datatype,
										 const_em->em_datatype);
		if (!OidIsValid(eq_op))
		{
			/* 失败了…… */
			ec->ec_broken = true;
			break;
		}

		/*
		 * 我们用常量的 em_jdomain 作为 qualscope，这样若
		 * 生成的子句不含变量（即两个 EM 都是常量），就会
		 * 在连接域这一层强制它。
		 */
		rinfo = process_implied_equality(root, eq_op, ec->ec_collation,
										 cur_em->em_expr, const_em->em_expr,
										 const_em->em_jdomain->jd_relids,
										 ec->ec_min_security,
										 cur_em->em_is_const);

		/*
		 * 若子句没有退化成常量，就填上可归并连接子句的正确
		 * 标记，并把它存进 ec_derives。（我们
		 * 不会直接复用这些子句，但选择率估计
		 * 以后也许会查阅该列表。注意 ec_derives 的这种用法
		 * 与它用于连接子句的用法不重叠，因为我们从不从
		 * ec_has_const 的 eclass 生成连接子句。）
		 */
		if (rinfo && rinfo->mergeopfamilies)
		{
			/* 它不是冗余的，所以不要设置 parent_ec */
			rinfo->left_ec = rinfo->right_ec = ec;
			rinfo->left_em = cur_em;
			rinfo->right_em = const_em;
			ec->ec_derives = lappend(ec->ec_derives, rinfo);
		}
	}
}

/*
 * generate_base_implied_equalities，当 EC 不含伪常量时
 */
static void
generate_base_implied_equalities_no_const(PlannerInfo *root,
										  EquivalenceClass *ec)
{
	EquivalenceMember **prev_ems;
	ListCell   *lc;

	/*
	 * 我们把 EC 成员扫描一遍，并跟踪每个
	 * 基关系上次见到的成员。当再次看到同一基关系的成员时，
	 * 就生成 "prev_em = cur_em"。这样派生子句数量最少，
	 * 但换一种顺序也许能成功，而这次可能失败。
	 * XXX FIXME：用与构造合并 EC 类似的 UNION-FIND 算法。
	 * （每个关系用一个列表的列表。）
	 */
	prev_ems = (EquivalenceMember **)
		palloc0(root->simple_rel_array_size * sizeof(EquivalenceMember *));

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		int			relid;

		Assert(!cur_em->em_is_child);	/* 还没有子项 */
		if (!bms_get_singleton_member(cur_em->em_relids, &relid))
			continue;
		Assert(relid < root->simple_rel_array_size);

		if (prev_ems[relid] != NULL)
		{
			EquivalenceMember *prev_em = prev_ems[relid];
			Oid			eq_op;
			RestrictInfo *rinfo;

			eq_op = select_equality_operator(ec,
											 prev_em->em_datatype,
											 cur_em->em_datatype);
			if (!OidIsValid(eq_op))
			{
				/* 失败了…… */
				ec->ec_broken = true;
				break;
			}

			/*
			 * 这些表达式不是常量，因此传入的 qualscope 永远
			 * 不会用来放置生成的子句。只需要
			 * 确保它覆盖两个表达式，em_relids 应当做得到。
			 */
			rinfo = process_implied_equality(root, eq_op, ec->ec_collation,
											 prev_em->em_expr, cur_em->em_expr,
											 cur_em->em_relids,
											 ec->ec_min_security,
											 false);

			/*
			 * 若子句没有退化成常量，就填上
			 * 可归并连接子句的正确标记。不过不把它放进
			 * ec_derives；目前不需要重新找到这些
			 * 子句，也不想用非连接
			 * 子句把那个列表弄乱。
			 */
			if (rinfo && rinfo->mergeopfamilies)
			{
				/* 它不是冗余的，所以不要设置 parent_ec */
				rinfo->left_ec = rinfo->right_ec = ec;
				rinfo->left_em = prev_em;
				rinfo->right_em = cur_em;
			}
		}
		prev_ems[relid] = cur_em;
	}

	pfree(prev_ems);

	/*
	 * 还必须确保成员子句用到的全部 Vars
	 * 在我们可能引用它们的任何连接节点上都可用。
	 * 目前强制全部 Vars 在该 eclass 的所有连接节点上都可用。
	 * 也许可以先分析我们更想连接哪些成员来改进，
	 * 但这不会比
	 * 8.3 之前的代码更差。
	 */
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);
		List	   *vars = pull_var_clause((Node *) cur_em->em_expr,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, ec->ec_relids);
		list_free(vars);
	}
}

/*
 * generate_base_implied_equalities 失败后的清理
 *
 * 这里必须做的是：把该 EC 中零关系或单关系的来源 RestrictInfos
 * 推回主 restrictinfo 数据结构。多关系
 * 子句以后由 generate_join_implied_equalities() 吐回。
 * （这样做是为了与 ec_broken
 * 直到上升一两层连接之后才被置位的情形保持连续。）不过，对
 * 含常量的 EC，可以采用更简单的策略，立刻
 * 把全部来源 RestrictInfos 退回去；这行得通，因为
 * 我们知道这样的 EC 以后不会再损坏。（这条规则说明了为什么
 * 即使 ec_has_const 的 EC 已损坏，在 generate_join_implied_equalities 里也可以
 * 忽略它们。）
 */
static void
generate_base_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec)
{
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		if (ec->ec_has_const ||
			bms_membership(restrictinfo->required_relids) != BMS_MULTIPLE)
			distribute_restrictinfo_to_rels(root, restrictinfo);
	}
}


/*
 * generate_join_implied_equalities
 *	  生成我们能从等价类推出的任何连接子句。
 *
 * 在连接节点上，必须强制足够的限制子句，以确保
 * 在该节点可计算的全部等价类成员彼此相等。
 * 要强制的子句集合随哪些子集
 * 关系作为输入而变化，因此必须为每一对连接
 * 关系重新计算。所以每次调用都新建一份 RestrictInfo 的 List 并
 * 返回。
 *
 * 除了用于连接节点，这也可以用来为基关系的参数化扫描
 * 生成基于 eclass 的连接子句。
 * 把内层关系指定为 RelOptInfo、
 * 外层关系指定为 Relids 这种不对称，是因为这种用法发生在我们
 * 建成任何连接 RelOptInfos 之前。
 *
 * 参数化扫描有个烦人的特例：内层关系可以
 * 是 appendrel 的子项（一个 "other rel"）。此时必须用子 EC 成员
 * 生成适当的子句。对子关系，add_child_rel_equivalences
 * 必须已经做过。
 *
 * 结果足以用于归并、哈希以及普通嵌套循环连接
 * 方法。这里不操心挑选对参数化索引扫描最合适的
 * 子句。indxpath.c 会自己挑选
 * 要用的子句，若我们这里挑的与那些冗余，
 * 多余的会在 createplan 时用我们提供的 parent_ec
 * 标记消掉（见 is_redundant_derived_clause()）。
 *
 * 因为考虑不同连接路径时，同一批连接子句很可能多次需要，
 * 我们避免生成多份副本：
 * 每当选定某一对 EquivalenceMembers 来连接，
 * 就检查这一对是否匹配任何原始子句（在 ec_sources 中）
 * 或先前建成的子句（在 ec_derives 中）。这省内存，并允许
 * 复用缓存在 RestrictInfos 里的信息。我们也避免生成
 * 交换重复，也就是算法选出 "a.x = b.y" 但
 * 已经有 "b.y = a.x" 时，就返回已有子句。
 *
 * 若正在考虑外连接，sjinfo 是相关的外连接信息，
 * 否则可以为 NULL。
 *
 * join_relids 应当总是等于 bms_union(outer_relids, inner_rel->relids)
 * 再加上 add_outer_joins_to_relids() 会加上的那些。我们可以在内部计算它来简化
 * 本函数的 API，但大多数调用方反正
 * 手头就有这个值。
 */
List *
generate_join_implied_equalities(PlannerInfo *root,
								 Relids join_relids,
								 Relids outer_relids,
								 RelOptInfo *inner_rel,
								 SpecialJoinInfo *sjinfo)
{
	List	   *result = NIL;
	Relids		inner_relids = inner_rel->relids;
	Relids		nominal_inner_relids;
	Relids		nominal_join_relids;
	Bitmapset  *matching_ecs;
	int			i;

	/* 若内层关系是子关系，则需要额外的准备 */
	if (IS_OTHER_REL(inner_rel))
	{
		Assert(!bms_is_empty(inner_rel->top_parent_relids));

		/* 取最顶层父关系的 relid 集合 */
		nominal_inner_relids = inner_rel->top_parent_relids;
		/* EC 会标上父关系的 relid，而不是子关系的 */
		nominal_join_relids = bms_union(outer_relids, nominal_inner_relids);
		nominal_join_relids = add_outer_joins_to_relids(root,
														nominal_join_relids,
														sjinfo,
														NULL);
	}
	else
	{
		nominal_inner_relids = inner_relids;
		nominal_join_relids = join_relids;
	}

	/*
	 * 检查所有可能相关的 eclass。
	 *
	 * 若正在考虑外连接，必须包括提到任一输入关系
	 * 再加上该外连接 relid 的 "连接" 子句；这些
	 * 表示必须在这次
	 * 连接上施加的连接后过滤子句。我们没有能廉价识别这类
	 * eclass 的基础设施，因此退回到考虑所有提到
	 * nominal_join_relids 中任何内容的 eclass。
	 *
	 * 在内连接上可以更聪明：只考虑提到
	 * 两个输入关系的 eclass。
	 */
	if (sjinfo && sjinfo->ojrelid != 0)
		matching_ecs = get_eclass_indexes_for_relids(root, nominal_join_relids);
	else
		matching_ecs = get_common_eclass_indexes(root, nominal_inner_relids,
												 outer_relids);

	i = -1;
	while ((i = bms_next_member(matching_ecs, i)) >= 0)
	{
		EquivalenceClass *ec = (EquivalenceClass *) list_nth(root->eq_classes, i);
		List	   *sublist = NIL;

		/* 含常量的 EC 不需要进一步强制 */
		if (ec->ec_has_const)
			continue;

		/* 单成员 EC 不会产生任何推导 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/* 健全性检查：该 eclass 与这次连接有重叠 */
		Assert(bms_overlap(ec->ec_relids, nominal_join_relids));

		if (!ec->ec_broken)
			sublist = generate_join_implied_equalities_normal(root,
															  ec,
															  join_relids,
															  outer_relids,
															  inner_relids);

		/* 若未能生成所需的派生子句，则回退处理 */
		if (ec->ec_broken)
			sublist = generate_join_implied_equalities_broken(root,
															  ec,
															  nominal_join_relids,
															  outer_relids,
															  nominal_inner_relids,
															  inner_rel);

		result = list_concat(result, sublist);
	}

	return result;
}

/*
 * generate_join_implied_equalities_for_ecs
 *	  同上，但只考虑列出的 EC。
 *
 * 对目前唯一的调用方，可以假定 sjinfo == NULL，也就是我们
 * 对外连接过滤子句不感兴趣。将来也许需要
 * 改掉这一点。
 */
List *
generate_join_implied_equalities_for_ecs(PlannerInfo *root,
										 List *eclasses,
										 Relids join_relids,
										 Relids outer_relids,
										 RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	Relids		inner_relids = inner_rel->relids;
	Relids		nominal_inner_relids;
	Relids		nominal_join_relids;
	ListCell   *lc;

	/* 若内层关系是子关系，则需要额外的准备 */
	if (IS_OTHER_REL(inner_rel))
	{
		Assert(!bms_is_empty(inner_rel->top_parent_relids));

		/* 取最顶层父关系的 relid 集合 */
		nominal_inner_relids = inner_rel->top_parent_relids;
		/* EC 会标上父关系的 relid，而不是子关系的 */
		nominal_join_relids = bms_union(outer_relids, nominal_inner_relids);
	}
	else
	{
		nominal_inner_relids = inner_relids;
		nominal_join_relids = join_relids;
	}

	foreach(lc, eclasses)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		List	   *sublist = NIL;

		/* 含常量的 EC 不需要进一步强制 */
		if (ec->ec_has_const)
			continue;

		/* 单成员 EC 不会产生任何推导 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/* 与这次连接不重叠的也可以很快忽略 */
		if (!bms_overlap(ec->ec_relids, nominal_join_relids))
			continue;

		if (!ec->ec_broken)
			sublist = generate_join_implied_equalities_normal(root,
															  ec,
															  join_relids,
															  outer_relids,
															  inner_relids);

		/* 若未能生成所需的派生子句，则回退处理 */
		if (ec->ec_broken)
			sublist = generate_join_implied_equalities_broken(root,
															  ec,
															  nominal_join_relids,
															  outer_relids,
															  nominal_inner_relids,
															  inner_rel);

		result = list_concat(result, sublist);
	}

	return result;
}

/*
 * generate_join_implied_equalities，针对仍然有效的 EC
 */
static List *
generate_join_implied_equalities_normal(PlannerInfo *root,
										EquivalenceClass *ec,
										Relids join_relids,
										Relids outer_relids,
										Relids inner_relids)
{
	List	   *result = NIL;
	List	   *new_members = NIL;
	List	   *outer_members = NIL;
	List	   *inner_members = NIL;
	ListCell   *lc1;

	/*
	 * 首先扫描该 EC，找出在外层关系、
	 * 内层关系、或在本关系但不在任一
	 * 输入关系上可计算的成员值。外层关系的成员应当已经强制相等，
	 * 内层关系的成员同样如此。还需要造子句来
	 * 强制任何新可计算的成员彼此全部相等，
	 * 并且至少与一个输入成员相等，另外至少强制一个
	 * 外层成员与至少一个内层成员相等。
	 */
	foreach(lc1, ec->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc1);

		/*
		 * 不必显式检查子 EC 成员。对
		 * join_relids 的这个测试会让它们被忽略，除非正在
		 * 考虑子内层关系，而这正是我们想要的。
		 */
		if (!bms_is_subset(cur_em->em_relids, join_relids))
			continue;			/* 还不可计算，或子关系不对 */

		if (bms_is_subset(cur_em->em_relids, outer_relids))
			outer_members = lappend(outer_members, cur_em);
		else if (bms_is_subset(cur_em->em_relids, inner_relids))
			inner_members = lappend(inner_members, cur_em);
		else
			new_members = lappend(new_members, cur_em);
	}

	/*
	 * 首先，若需要就选出连接子句。可以把任意一个外层
	 * 成员与任意一个内层成员等起来，但必须找到存在操作符族成员操作符的
	 * 数据类型组合。若有
	 * 选择，我们更喜欢简单的 Var 成员（也许带 RelabelType），因为
	 * 它们 (a) 运行时计算最便宜，(b) 最可能
	 * 有有用的统计信息。另外，更喜欢同时也
	 * 可哈希连接的操作符。
	 */
	if (outer_members && inner_members)
	{
		EquivalenceMember *best_outer_em = NULL;
		EquivalenceMember *best_inner_em = NULL;
		Oid			best_eq_op = InvalidOid;
		int			best_score = -1;
		RestrictInfo *rinfo;

		foreach(lc1, outer_members)
		{
			EquivalenceMember *outer_em = (EquivalenceMember *) lfirst(lc1);
			ListCell   *lc2;

			foreach(lc2, inner_members)
			{
				EquivalenceMember *inner_em = (EquivalenceMember *) lfirst(lc2);
				Oid			eq_op;
				int			score;

				eq_op = select_equality_operator(ec,
												 outer_em->em_datatype,
												 inner_em->em_datatype);
				if (!OidIsValid(eq_op))
					continue;
				score = 0;
				if (IsA(outer_em->em_expr, Var) ||
					(IsA(outer_em->em_expr, RelabelType) &&
					 IsA(((RelabelType *) outer_em->em_expr)->arg, Var)))
					score++;
				if (IsA(inner_em->em_expr, Var) ||
					(IsA(inner_em->em_expr, RelabelType) &&
					 IsA(((RelabelType *) inner_em->em_expr)->arg, Var)))
					score++;
				if (op_hashjoinable(eq_op,
									exprType((Node *) outer_em->em_expr)))
					score++;
				if (score > best_score)
				{
					best_outer_em = outer_em;
					best_inner_em = inner_em;
					best_eq_op = eq_op;
					best_score = score;
					if (best_score == 3)
						break;	/* 不必再找下去 */
				}
			}
			if (best_score == 3)
				break;			/* 不必再找下去 */
		}
		if (best_score < 0)
		{
			/* 失败了…… */
			ec->ec_broken = true;
			return NIL;
		}

		/*
		 * 创建子句，设置 parent_ec 以标记它与其他
		 * 连接子句冗余
		 */
		rinfo = create_join_clause(root, ec, best_eq_op,
								   best_outer_em, best_inner_em,
								   ec);

		result = lappend(result, rinfo);
	}

	/*
	 * 现在处理为涉及
	 * 连接两侧 Vars 的表达式建立限制。必须把它们全部彼此等起来，
	 * 并且至少与一个旧成员等起来（若有）。
	 *
	 * XXX 与 generate_base_implied_equalities_no_const 一样，这里可以聪明
	 * 得多，以避免跨类型情形下不必要的失败。
	 * 目前先用那里用的从左到右方法。
	 */
	if (new_members)
	{
		List	   *old_members = list_concat(outer_members, inner_members);
		EquivalenceMember *prev_em = NULL;
		RestrictInfo *rinfo;

		/* 目前先任意取第一个 old_member 来用 */
		if (old_members)
			new_members = lappend(new_members, linitial(old_members));

		foreach(lc1, new_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc1);

			if (prev_em != NULL)
			{
				Oid			eq_op;

				eq_op = select_equality_operator(ec,
												 prev_em->em_datatype,
												 cur_em->em_datatype);
				if (!OidIsValid(eq_op))
				{
					/* 失败了…… */
					ec->ec_broken = true;
					return NIL;
				}
				/* 不要设置 parent_ec，这个限定条件不是冗余的！ */
				rinfo = create_join_clause(root, ec, eq_op,
										   prev_em, cur_em,
										   NULL);

				result = lappend(result, rinfo);
			}
			prev_em = cur_em;
		}
	}

	return result;
}

/*
 * generate_join_implied_equalities 失败后的清理
 *
 * 返回在这次连接上可强制的任何原始 RestrictInfos。
 *
 * 若内层关系是子关系，必须把
 * 原始 RestrictInfos 从父 Vars 翻译成子 Vars。
 */
static List *
generate_join_implied_equalities_broken(PlannerInfo *root,
										EquivalenceClass *ec,
										Relids nominal_join_relids,
										Relids outer_relids,
										Relids nominal_inner_relids,
										RelOptInfo *inner_rel)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, ec->ec_sources)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);
		Relids		clause_relids = restrictinfo->required_relids;

		if (bms_is_subset(clause_relids, nominal_join_relids) &&
			!bms_is_subset(clause_relids, outer_relids) &&
			!bms_is_subset(clause_relids, nominal_inner_relids))
			result = lappend(result, restrictinfo);
	}

	/*
	 * 若必须翻译，就粗暴地对全部 RestrictInfos 一次施加 adjust_appendrel_attrs。
	 * 这会返回
	 * 不在 ec_derives 里列出的 RestrictInfos，但不应当有
	 * 重复，而且这是足够窄的边角情形，
	 * 不必为此太费心。
	 *
	 * 因为 inner_rel 也许是 ec_sources 子句所提到的基关系的
	 * 间接后代，必须准备好施加
	 * 多层 Var 翻译。
	 */
	if (IS_OTHER_REL(inner_rel) && result != NIL)
		result = (List *) adjust_appendrel_attrs_multilevel(root,
															(Node *) result,
															inner_rel,
															inner_rel->top_parent);

	return result;
}


/*
 * select_equality_operator
 *	  为比较两个 EC 成员选择合适的等值操作符
 *
 * 若找不到该数据类型组合的操作符，则返回 InvalidOid
 */
static Oid
select_equality_operator(EquivalenceClass *ec, Oid lefttype, Oid righttype)
{
	ListCell   *lc;

	foreach(lc, ec->ec_opfamilies)
	{
		Oid			opfamily = lfirst_oid(lc);
		Oid			opno;

		opno = get_opfamily_member(opfamily, lefttype, righttype,
								   BTEqualStrategyNumber);
		if (!OidIsValid(opno))
			continue;
		/* 若查询中没有屏障限定条件，就不必担心泄漏操作符 */
		if (ec->ec_max_security == 0)
			return opno;
		/* 否则，坚持所选操作符必须是防泄漏的 */
		if (get_func_leakproof(get_opcode(opno)))
			return opno;
	}
	return InvalidOid;
}


/*
 * create_join_clause
 *	  查找或制造一个 RestrictInfo，用给定操作符比较两个给定的 EC 成员
 *	  （或者也许用它的交换子，因为
 *	  结果中操作数的顺序没有保证）。
 *
 * parent_ec 要么等于 ec（若该子句是可能冗余的
 * 连接子句），要么为 NULL（若不是）。必须把这一点当作
 * 匹配要求的一部分 --- 比较同一对
 * EM 的子句，在一条连接路径上可能是连接子句，在另一条上可能是限制子句。
 */
static RestrictInfo *
create_join_clause(PlannerInfo *root,
				   EquivalenceClass *ec, Oid opno,
				   EquivalenceMember *leftem,
				   EquivalenceMember *rightem,
				   EquivalenceClass *parent_ec)
{
	RestrictInfo *rinfo;
	RestrictInfo *parent_rinfo = NULL;
	ListCell   *lc;
	MemoryContext oldcontext;

	/*
	 * 搜索是否已经为这一对
	 * EquivalenceMembers 建过 RestrictInfo。可以用原始来源子句，
	 * 也可以用先前派生的子句，交换子句也可以接受。
	 *
	 * 以前会验证 opno 是否匹配，但那似乎多余：即使
	 * 不完全相同，效果也应当一样，否则我们用的操作符
	 * 族就是坏的。
	 */
	foreach(lc, ec->ec_sources)
	{
		rinfo = (RestrictInfo *) lfirst(lc);
		if (rinfo->left_em == leftem &&
			rinfo->right_em == rightem &&
			rinfo->parent_ec == parent_ec)
			return rinfo;
		if (rinfo->left_em == rightem &&
			rinfo->right_em == leftem &&
			rinfo->parent_ec == parent_ec)
			return rinfo;
	}

	foreach(lc, ec->ec_derives)
	{
		rinfo = (RestrictInfo *) lfirst(lc);
		if (rinfo->left_em == leftem &&
			rinfo->right_em == rightem &&
			rinfo->parent_ec == parent_ec)
			return rinfo;
		if (rinfo->left_em == rightem &&
			rinfo->right_em == leftem &&
			rinfo->parent_ec == parent_ec)
			return rinfo;
	}

	/*
	 * 没有，那就造一个，放在规划器上下文里以便复用。（普通
	 * 规划里不重要，但在 GEQO 里绝对重要。）
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	/*
	 * 若任一 EM 是子项，就递归创建对应的
	 * 父到父子句，以便复制它的 rinfo_serial。
	 */
	if (leftem->em_is_child || rightem->em_is_child)
	{
		EquivalenceMember *leftp = leftem->em_parent ? leftem->em_parent : leftem;
		EquivalenceMember *rightp = rightem->em_parent ? rightem->em_parent : rightem;

		parent_rinfo = create_join_clause(root, ec, opno,
										  leftp, rightp,
										  parent_ec);
	}

	rinfo = build_implied_join_equality(root,
										opno,
										ec->ec_collation,
										leftem->em_expr,
										rightem->em_expr,
										bms_union(leftem->em_relids,
												  rightem->em_relids),
										ec->ec_min_security);

	/*
	 * 若任一 EM 是子项，就强制该子句的 clause_relids 包含
	 * 子关系的 relid(s)。正常情形下已经包含，但
	 * 若考虑的是带伪常量
	 * 翻译变量的 appendrel 子关系（即输出项为常量的 UNION ALL 子查询），则不然。
	 * 必须这样做，join_clause_is_movable_into() 才会
	 * 认为该子句应当在正确的地方求值。
	 */
	if (leftem->em_is_child)
		rinfo->clause_relids = bms_add_members(rinfo->clause_relids,
											   leftem->em_relids);
	if (rightem->em_is_child)
		rinfo->clause_relids = bms_add_members(rinfo->clause_relids,
											   rightem->em_relids);

	/* 若是子子句，就复制父项的 rinfo_serial */
	if (parent_rinfo)
		rinfo->rinfo_serial = parent_rinfo->rinfo_serial;

	/* 把子句标记为冗余，或不冗余 */
	rinfo->parent_ec = parent_ec;

	/*
	 * 我们知道 left_ec/right_ec 的正确值，即这个特定 EC，
	 * 因此可以直接设置，不必再强制查找一次。
	 */
	rinfo->left_ec = ec;
	rinfo->right_ec = ec;

	/* 把它标记为可用于这些 EM */
	rinfo->left_em = leftem;
	rinfo->right_em = rightem;
	/* 并保存起来以便可能复用 */
	ec->ec_derives = lappend(ec->ec_derives, rinfo);

	MemoryContextSwitchTo(oldcontext);

	return rinfo;
}


/*
 * reconsider_outer_join_clauses
 *	  重新检查被 distribute_qual_to_rels() 搁置的外连接子句，
 *	  看能否从中推出
 *	  EquivalenceClasses。然后，若它们没有变成
 *	  冗余，就把它们推进常规的连接子句列表。
 *
 * 当可归并连接的子句 A = B 是外连接子句时，
 * 无法盲目把它们与其他子句 A = C 组合来推出 B = C，
 * 因为事实上 "相等" A = B 在外连接之上不一定成立
 * （其中一个变量也许是 NULL）。尽管如此，
 * 仍有些情形可以用传递性追加限定子句。
 *
 * 这里要找的一种情形是外连接子句 OUTERVAR = INNERVAR，
 * 并且还有等值子句 OUTERVAR = CONSTANT。
 * 把子句 INNERVAR = CONSTANT 推进
 * 内层（可空）关系的求值是安全且有用的，因为不满足该条件的内层行
 * 反正不会对外连接结果有贡献。
 * （它们能连上的任何外层行都会被推下去的
 * 等值子句消掉。）
 *
 * 注意上面的规则对全外连接不成立；考虑生成的等值子句
 * 会涉及外连接之外的关系也
 * 没什么意思，因为这样的子句反正无法
 * 推进内侧扫描。因此限制为
 * outervar = 伪常量并没有真正放弃什么。
 *
 * 对全连接情形，只有当它是 FULL JOIN
 * USING，且某个合并列有等值 MERGEDVAR = CONSTANT 时，才能做点有用的事。
 * 到这里时，合并列看起来会像
 *		COALESCE(LEFTVAR, RIGHTVAR)
 * 并且会有一条全连接子句 LEFTVAR = RIGHTVAR，可以拿来匹配
 * COALESCE 表达式。此时可以把 LEFTVAR = CONSTANT
 * 和 RIGHTVAR = CONSTANT 推进输入关系，因为不满足这些条件的任何行
 * 都无法对连接结果有贡献。
 *
 * 再者，试图处理把合并变量与非伪常量比较的
 * 子句也得不到什么好处。因此可以利用
 * EquivalenceClasses 去搜索曾被
 * 等值到常量的匹配变量。有意思的外连接子句是由
 * distribute_qual_to_rels 为我们积累的。
 *
 * 找到这些情形之一时，我们通过
 * 生成新的等值子句 INNERVAR = CONSTANT（或 LEFTVAR 等）
 * 并推进 EquivalenceClass 结构来实现所要的改动。这是因为我们
 * 也许已经知道 INNERVAR 与某些其他 var(s) 等值，并且
 * 希望常量也传播到它们。注意，把 INNERVAR 的任何已有 EC 与 OUTERVAR 的 EC 合并
 * 是不安全的 ---
 * 那会导致常量限制从
 * INNERVAR 传播到 OUTERVAR，那就大错了。
 *
 * INNERVAR 也可能是另一条
 * 外连接子句的 OUTERVAR，此时过程可以重复。因此我们反复
 * 遍历子句列表，直到再也推不出新结论。
 * 每当做出一次推导，就把产生它的子句从
 * 列表中去掉，因为不想把同一推导做两遍。
 *
 * 若对搁置的外连接子句找不到任何匹配，就必须
 * 把它退回常规连接子句处理，办法是传给
 * distribute_restrictinfo_to_rels()。若确实生成了派生子句，
 * 则外连接子句是冗余的。仍必须把某条
 * 子句放进常规处理，否则该连接会被
 * 看成无子句连接，并在连接顺序搜索中被避开。
 * 我们的办法是生成一条常量 TRUE 子句，标上与去掉的外连接子句相同的
 * required_relids 等，从而
 * 使它成为正确关系之间的连接子句。
 */
void
reconsider_outer_join_clauses(PlannerInfo *root)
{
	bool		found;
	ListCell   *cell;

	/* 外层循环重复，直到再也推不出新结论 */
	do
	{
		found = false;

		/* 处理 LEFT JOIN 子句 */
		foreach(cell, root->left_join_clauses)
		{
			OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

			if (reconsider_outer_join_clause(root, ojcinfo, true))
			{
				RestrictInfo *rinfo = ojcinfo->rinfo;

				found = true;
				/* 把它从列表中去掉 */
				root->left_join_clauses =
					foreach_delete_current(root->left_join_clauses, cell);
				/* 退回一条哑替换子句（见上面的说明） */
				rinfo = make_restrictinfo(root,
										  (Expr *) makeBoolConst(true, false),
										  rinfo->is_pushed_down,
										  rinfo->has_clone,
										  rinfo->is_clone,
										  false,	/* 伪常量 */
										  0,	/* security_level（安全级别） */
										  rinfo->required_relids,
										  rinfo->incompatible_relids,
										  rinfo->outer_relids);
				distribute_restrictinfo_to_rels(root, rinfo);
			}
		}

		/* 处理 RIGHT JOIN 子句 */
		foreach(cell, root->right_join_clauses)
		{
			OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

			if (reconsider_outer_join_clause(root, ojcinfo, false))
			{
				RestrictInfo *rinfo = ojcinfo->rinfo;

				found = true;
				/* 把它从列表中去掉 */
				root->right_join_clauses =
					foreach_delete_current(root->right_join_clauses, cell);
				/* 退回一条哑替换子句（见上面的说明） */
				rinfo = make_restrictinfo(root,
										  (Expr *) makeBoolConst(true, false),
										  rinfo->is_pushed_down,
										  rinfo->has_clone,
										  rinfo->is_clone,
										  false,	/* 伪常量 */
										  0,	/* security_level（安全级别） */
										  rinfo->required_relids,
										  rinfo->incompatible_relids,
										  rinfo->outer_relids);
				distribute_restrictinfo_to_rels(root, rinfo);
			}
		}

		/* 处理 FULL JOIN 子句 */
		foreach(cell, root->full_join_clauses)
		{
			OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

			if (reconsider_full_join_clause(root, ojcinfo))
			{
				RestrictInfo *rinfo = ojcinfo->rinfo;

				found = true;
				/* 把它从列表中去掉 */
				root->full_join_clauses =
					foreach_delete_current(root->full_join_clauses, cell);
				/* 退回一条哑替换子句（见上面的说明） */
				rinfo = make_restrictinfo(root,
										  (Expr *) makeBoolConst(true, false),
										  rinfo->is_pushed_down,
										  rinfo->has_clone,
										  rinfo->is_clone,
										  false,	/* 伪常量 */
										  0,	/* security_level（安全级别） */
										  rinfo->required_relids,
										  rinfo->incompatible_relids,
										  rinfo->outer_relids);
				distribute_restrictinfo_to_rels(root, rinfo);
			}
		}
	} while (found);

	/* 现在，其余子句都必须退回去 */
	foreach(cell, root->left_join_clauses)
	{
		OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, ojcinfo->rinfo);
	}
	foreach(cell, root->right_join_clauses)
	{
		OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, ojcinfo->rinfo);
	}
	foreach(cell, root->full_join_clauses)
	{
		OuterJoinClauseInfo *ojcinfo = (OuterJoinClauseInfo *) lfirst(cell);

		distribute_restrictinfo_to_rels(root, ojcinfo->rinfo);
	}
}

/*
 * reconsider_outer_join_clauses，针对单条 LEFT/RIGHT JOIN 子句
 *
 * 若能把常量经该子句传播出去，则返回 true。
 */
static bool
reconsider_outer_join_clause(PlannerInfo *root, OuterJoinClauseInfo *ojcinfo,
							 bool outer_on_left)
{
	RestrictInfo *rinfo = ojcinfo->rinfo;
	SpecialJoinInfo *sjinfo = ojcinfo->sjinfo;
	Expr	   *outervar,
			   *innervar;
	Oid			opno,
				collation,
				left_type,
				right_type,
				inner_datatype;
	Relids		inner_relids;
	ListCell   *lc1;

	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;
	collation = ((OpExpr *) rinfo->clause)->inputcollid;

	/* 从该子句提取所需信息 */
	op_input_types(opno, &left_type, &right_type);
	if (outer_on_left)
	{
		outervar = (Expr *) get_leftop(rinfo->clause);
		innervar = (Expr *) get_rightop(rinfo->clause);
		inner_datatype = right_type;
		inner_relids = rinfo->right_relids;
	}
	else
	{
		outervar = (Expr *) get_rightop(rinfo->clause);
		innervar = (Expr *) get_leftop(rinfo->clause);
		inner_datatype = left_type;
		inner_relids = rinfo->left_relids;
	}

	/* 在 EquivalenceClasses 中扫描与 outervar 的匹配 */
	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		bool		match;
		ListCell   *lc2;

		/* 除非 EC 含有伪常量，否则忽略 */
		if (!cur_ec->ec_has_const)
			continue;
		/* 永不匹配易变 EC */
		if (cur_ec->ec_has_volatile)
			continue;
		/* 它在语义上也必须与外连接子句匹配 */
		if (collation != cur_ec->ec_collation)
			continue;
		if (!equal(rinfo->mergeopfamilies, cur_ec->ec_opfamilies))
			continue;
		/* 它是否包含与 outervar 的匹配？ */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);

			Assert(!cur_em->em_is_child);	/* 还没有子项 */
			if (equal(outervar, cur_em->em_expr))
			{
				match = true;
				break;
			}
		}
		if (!match)
			continue;			/* 没有匹配，因此忽略该 EC */

		/*
		 * 是的！试着为 EC 中的每个
		 * CONSTANT 生成子句 INNERVAR = CONSTANT。注意必须至少有一个
		 * 常量成功，才能决定丢掉外连接子句。
		 */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *newrinfo;
			JoinDomain *jdomain;

			if (!cur_em->em_is_const)
				continue;		/* 忽略非常量成员 */
			eq_op = select_equality_operator(cur_ec,
											 inner_datatype,
											 cur_em->em_datatype);
			if (!OidIsValid(eq_op))
				continue;		/* 无法生成等值 */
			newrinfo = build_implied_join_equality(root,
												   eq_op,
												   cur_ec->ec_collation,
												   innervar,
												   cur_em->em_expr,
												   bms_copy(inner_relids),
												   cur_ec->ec_min_security);
			/* 该等值在外连接的子 JoinDomain 内成立 */
			jdomain = find_join_domain(root, sjinfo->syn_righthand);
			if (process_equivalence(root, &newrinfo, jdomain))
				match = true;
		}

		/*
		 * 若能把 INNERVAR 与任何常量等起来，就报告成功。
		 * 否则跳出搜索循环，因为我们知道 OUTERVAR
		 * 最多出现在一个 EC 中。
		 */
		if (match)
			return true;
		else
			break;
	}

	return false;				/* 未能做出任何推导 */
}

/*
 * reconsider_outer_join_clauses，针对单条 FULL JOIN 子句
 *
 * 若能把常量经该子句传播出去，则返回 true。
 */
static bool
reconsider_full_join_clause(PlannerInfo *root, OuterJoinClauseInfo *ojcinfo)
{
	RestrictInfo *rinfo = ojcinfo->rinfo;
	SpecialJoinInfo *sjinfo = ojcinfo->sjinfo;
	Relids		fjrelids = bms_make_singleton(sjinfo->ojrelid);
	Expr	   *leftvar;
	Expr	   *rightvar;
	Oid			opno,
				collation,
				left_type,
				right_type;
	Relids		left_relids,
				right_relids;
	ListCell   *lc1;

	/* 从该子句提取所需信息 */
	Assert(is_opclause(rinfo->clause));
	opno = ((OpExpr *) rinfo->clause)->opno;
	collation = ((OpExpr *) rinfo->clause)->inputcollid;
	op_input_types(opno, &left_type, &right_type);
	leftvar = (Expr *) get_leftop(rinfo->clause);
	rightvar = (Expr *) get_rightop(rinfo->clause);
	left_relids = rinfo->left_relids;
	right_relids = rinfo->right_relids;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc1);
		EquivalenceMember *coal_em = NULL;
		bool		match;
		bool		matchleft;
		bool		matchright;
		ListCell   *lc2;
		int			coal_idx = -1;

		/* 除非 EC 含有伪常量，否则忽略 */
		if (!cur_ec->ec_has_const)
			continue;
		/* 永不匹配易变 EC */
		if (cur_ec->ec_has_volatile)
			continue;
		/* 它在语义上也必须与外连接子句匹配 */
		if (collation != cur_ec->ec_collation)
			continue;
		if (!equal(rinfo->mergeopfamilies, cur_ec->ec_opfamilies))
			continue;

		/*
		 * 它是否包含 COALESCE(leftvar, rightvar) 结构？
		 *
		 * 可以假定 COALESCE() 的输入与
		 * 连接子句的顺序相同，因为在我们关心的情形里
		 * 两者都是自动生成的。
		 *
		 * XXX 目前在跨类型情形下这也许匹配失败，因为
		 * COALESCE 会包含类型转换运算，而连接子句
		 * 也许没有（若两个列类型
		 * 有可用的跨类型归并连接操作符）。
		 * 从 COALESCE 参数上剥掉隐式强制转换可以吗？
		 */
		match = false;
		foreach(lc2, cur_ec->ec_members)
		{
			coal_em = (EquivalenceMember *) lfirst(lc2);
			Assert(!coal_em->em_is_child);	/* 还没有子项 */
			if (IsA(coal_em->em_expr, CoalesceExpr))
			{
				CoalesceExpr *cexpr = (CoalesceExpr *) coal_em->em_expr;
				Node	   *cfirst;
				Node	   *csecond;

				if (list_length(cexpr->args) != 2)
					continue;
				cfirst = (Node *) linitial(cexpr->args);
				csecond = (Node *) lsecond(cexpr->args);

				/*
				 * COALESCE 参数会被标记为可能被
				 * 全连接置空，而我们想生成施加于
				 * 连接输入的子句。因此在把 cfirst/csecond 的
				 * nullingrels 与 leftvar/rightvar 比较之前，必须从中剥掉该连接。
				 * （也许 FULL JOIN USING 输出列若有不那么古怪的
				 * 表示，
				 * 就不需要这样做？）
				 */
				cfirst = remove_nulling_relids(cfirst, fjrelids, NULL);
				csecond = remove_nulling_relids(csecond, fjrelids, NULL);

				if (equal(leftvar, cfirst) && equal(rightvar, csecond))
				{
					coal_idx = foreach_current_index(lc2);
					match = true;
					break;
				}
			}
		}
		if (!match)
			continue;			/* 没有匹配，因此忽略该 EC */

		/*
		 * 是的！试着为 EC 中的每个 CONSTANT 生成子句 LEFTVAR = CONSTANT 和
		 * RIGHTVAR = CONSTANT。注意必须
		 * 每个变量至少有一个常量成功，才能
		 * 决定丢掉外连接子句。
		 */
		matchleft = matchright = false;
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *newrinfo;
			JoinDomain *jdomain;

			if (!cur_em->em_is_const)
				continue;		/* 忽略非常量成员 */
			eq_op = select_equality_operator(cur_ec,
											 left_type,
											 cur_em->em_datatype);
			if (OidIsValid(eq_op))
			{
				newrinfo = build_implied_join_equality(root,
													   eq_op,
													   cur_ec->ec_collation,
													   leftvar,
													   cur_em->em_expr,
													   bms_copy(left_relids),
													   cur_ec->ec_min_security);
				/* 该等值在左侧子 JoinDomain 内成立 */
				jdomain = find_join_domain(root, sjinfo->syn_lefthand);
				if (process_equivalence(root, &newrinfo, jdomain))
					matchleft = true;
			}
			eq_op = select_equality_operator(cur_ec,
											 right_type,
											 cur_em->em_datatype);
			if (OidIsValid(eq_op))
			{
				newrinfo = build_implied_join_equality(root,
													   eq_op,
													   cur_ec->ec_collation,
													   rightvar,
													   cur_em->em_expr,
													   bms_copy(right_relids),
													   cur_ec->ec_min_security);
				/* 该等值在右侧子 JoinDomain 内成立 */
				jdomain = find_join_domain(root, sjinfo->syn_righthand);
				if (process_equivalence(root, &newrinfo, jdomain))
					matchright = true;
			}
		}

		/*
		 * 若能把两个变量都与常量等起来，就做完了，并且
		 * 可以把全连接子句当作冗余丢掉。此外，可以
		 * 从 EC 中去掉 COALESCE 项，因为加上的
		 * 限制保证它总会有预期的值。（我们
		 * 懒得去更新 ec_relids 或 ec_sources。）
		 */
		if (matchleft && matchright)
		{
			cur_ec->ec_members = list_delete_nth_cell(cur_ec->ec_members, coal_idx);
			return true;
		}

		/*
		 * 否则跳出搜索循环，因为我们知道 COALESCE
		 * 最多出现在一个 EC 中（XXX 若允许
		 * 上面那样剥掉强制转换，这也许不再成立？）
		 */
		break;
	}

	return false;				/* 未能做出任何推导 */
}

/*
 * find_join_domain
 *	  找出包含在给定 relid 集合之内的最高 JoinDomain。
 *
 * （可以不做这次搜索，代价是把别处的 APIs 弄复杂，
 * 似乎不值得。）
 */
static JoinDomain *
find_join_domain(PlannerInfo *root, Relids relids)
{
	ListCell   *lc;

	foreach(lc, root->join_domains)
	{
		JoinDomain *jdomain = (JoinDomain *) lfirst(lc);

		if (bms_is_subset(jdomain->jd_relids, relids))
			return jdomain;
	}
	elog(ERROR, "failed to find appropriate JoinDomain");
	return NULL;				/* 消除编译器告警 */
}


/*
 * exprs_known_equal
 *	  检测两个表达式是否因等值
 *	  关系而已知相等。
 *
 * 实际上这只表明这些表达式按
 * 某个操作符族的相等观念是相等的 --- 但我们只用它做
 * 选择率估计，所以模糊的相等观念就可以。
 *
 * 注意：懒得检查 "equal(item1, item2)"；若可能传入相同的项，调用方必须
 * 自己检查那种情形。
 */
bool
exprs_known_equal(PlannerInfo *root, Node *item1, Node *item2)
{
	ListCell   *lc1;

	foreach(lc1, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc1);
		bool		item1member = false;
		bool		item2member = false;
		ListCell   *lc2;

		/* 永不匹配易变 EC */
		if (ec->ec_has_volatile)
			continue;

		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);

			if (em->em_is_child)
				continue;		/* 这里忽略子项 */
			if (equal(item1, em->em_expr))
				item1member = true;
			else if (equal(item2, em->em_expr))
				item2member = true;
			/* 一旦证明相等就退出 */
			if (item1member && item2member)
				return true;
		}
	}
	return false;
}


/*
 * match_eclasses_to_foreign_key_col
 *	  看外键列的匹配是否被某个 eclass 证明。
 *
 * 若该外键第 colno 列的被引用 Vars 与引用 Vars
 * 因某个 eclass 而已知相等，就返回那个 eclass；否则返回 NULL。
 * （原则上若涉及多种
 * 排序规则，也许有不止一个匹配的 eclass，但排序规则对相等无关，
 * 这里就忽略这个细节。）这很像 exprs_known_equal，
 * 只是我们坚持比较操作符与该 eclass 匹配，从而
 * 结果是确定的而不是近似的。
 *
 * 成功时，还把 fkinfo->eclass[colno] 设为匹配的 eclass，
 * 并把 fkinfo->fk_eclass_member[colno] 设为引用 Var 的
 * eclass 成员。
 */
EquivalenceClass *
match_eclasses_to_foreign_key_col(PlannerInfo *root,
								  ForeignKeyOptInfo *fkinfo,
								  int colno)
{
	Index		var1varno = fkinfo->con_relid;
	AttrNumber	var1attno = fkinfo->conkey[colno];
	Index		var2varno = fkinfo->ref_relid;
	AttrNumber	var2attno = fkinfo->confkey[colno];
	Oid			eqop = fkinfo->conpfeqop[colno];
	RelOptInfo *rel1 = root->simple_rel_array[var1varno];
	RelOptInfo *rel2 = root->simple_rel_array[var2varno];
	List	   *opfamilies = NIL;	/* 仅在需要时计算 */
	Bitmapset  *matching_ecs;
	int			i;

	/* 只考虑同时提到两个关系的 eclass */
	Assert(root->ec_merging_done);
	Assert(IS_SIMPLE_REL(rel1));
	Assert(IS_SIMPLE_REL(rel2));
	matching_ecs = bms_intersect(rel1->eclass_indexes,
								 rel2->eclass_indexes);

	i = -1;
	while ((i = bms_next_member(matching_ecs, i)) >= 0)
	{
		EquivalenceClass *ec = (EquivalenceClass *) list_nth(root->eq_classes,
															 i);
		EquivalenceMember *item1_em = NULL;
		EquivalenceMember *item2_em = NULL;
		ListCell   *lc2;

		/* 永不匹配易变 EC */
		if (ec->ec_has_volatile)
			continue;
		/* 注意：这里匹配 "损坏" 的 eclass 似乎没问题 */

		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Var		   *var;

			if (em->em_is_child)
				continue;		/* 这里忽略子项 */

			/* EM 必须是 Var，也许带 RelabelType */
			var = (Var *) em->em_expr;
			while (var && IsA(var, RelabelType))
				var = (Var *) ((RelabelType *) var)->arg;
			if (!(var && IsA(var, Var)))
				continue;

			/* 匹配吗？ */
			if (var->varno == var1varno && var->varattno == var1attno)
				item1_em = em;
			else if (var->varno == var2varno && var->varattno == var2attno)
				item2_em = em;

			/* 是否已在该 EC 中同时找到主键列和外键列？ */
			if (item1_em && item2_em)
			{
				/*
				 * 若 eqop 匹配 EC 的操作符族，则成功。我们可以在
				 * 扫描成员之前测试这一点，但先测试
				 * 成员是否匹配也许更便宜。
				 */
				if (opfamilies == NIL)	/* 若还没算过就计算 */
					opfamilies = get_mergejoin_opfamilies(eqop);
				if (equal(opfamilies, ec->ec_opfamilies))
				{
					fkinfo->eclass[colno] = ec;
					fkinfo->fk_eclass_member[colno] = item2_em;
					return ec;
				}
				/* 否则，该 EC 处理完，转到下一个 */
				break;
			}
		}
	}
	return NULL;
}

/*
 * find_derived_clause_for_ec_member
 *	  搜索先前派生的、提到给定 EM 的子句。
 *
 * 该 eclass 应当是 ec_has_const 的 EC，且该 EM 是非常量
 * 成员。这应当保证只有一条提到
 * 该 EM（并把它与常量等起来）的派生子句。
 * 若找不到这样的子句，则返回 NULL。
 */
RestrictInfo *
find_derived_clause_for_ec_member(EquivalenceClass *ec,
								  EquivalenceMember *em)
{
	ListCell   *lc;

	Assert(ec->ec_has_const);
	Assert(!em->em_is_const);
	foreach(lc, ec->ec_derives)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * generate_base_implied_equalities_const 会把非常量
		 * 成员放在派生子句的左侧。
		 */
		if (rinfo->left_em == em)
			return rinfo;
	}
	return NULL;
}


/*
 * add_child_rel_equivalences
 *	  搜索引用 child_rel 的根父项的 EC 成员，并
 *	  加入引用该 child_rel 的变换后成员。
 *
 * 除非我们至少有一些
 * 理由相信它生成的 EC 成员会有用，否则根本不会调用本函数。
 *
 * parent_rel 和 child_rel 可以从 appinfo 推出，但既然
 * 调用方已经算过，不妨直接传进来。
 *
 * 传入的 AppendRelInfo 在 parent_rel 不是
 * 顶层基关系时不用，因为它显示的是从该 parent_rel 出发的映射，而
 * 我们需要翻译引用顶层父项的 EC 表达式。
 * 用它比用 adjust_appendrel_attrs_multilevel() 更快，
 * 因此能用时就用它。
 */
void
add_child_rel_equivalences(PlannerInfo *root,
						   AppendRelInfo *appinfo,
						   RelOptInfo *parent_rel,
						   RelOptInfo *child_rel)
{
	Relids		top_parent_relids = child_rel->top_parent_relids;
	Relids		child_relids = child_rel->relids;
	int			i;

	/*
	 * EC 合并应当已经完成，因此可以用父关系的
	 * eclass_indexes，避免搜索全部 root->eq_classes。
	 */
	Assert(root->ec_merging_done);
	Assert(IS_SIMPLE_REL(parent_rel));

	i = -1;
	while ((i = bms_next_member(parent_rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) list_nth(root->eq_classes, i);
		int			num_members;

		/*
		 * 若该 EC 含有易变表达式，生成子
		 * EM 会非常危险，因此跳过。我们依赖
		 * 易变 EC 只有一个 EM。
		 */
		if (cur_ec->ec_has_volatile)
			continue;

		/* 健全性检查：eclass_indexes 只含 parent_rel 的 EC */
		Assert(bms_is_subset(top_parent_relids, cur_ec->ec_relids));

		/*
		 * 这里不用 foreach()，因为扫描
		 * 新加入的子成员没有意义，可以在最后一个
		 * 原有 EC 成员之后停下。
		 */
		num_members = list_length(cur_ec->ec_members);
		for (int pos = 0; pos < num_members; pos++)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) list_nth(cur_ec->ec_members, pos);

			if (cur_em->em_is_const)
				continue;		/* 这里忽略常量 */

			/*
			 * 这里只考虑原始 EC 成员，不考虑
			 * 已经变换过的子成员。否则，若某个原始
			 * 成员表达式引用了不止一个 appendrel，就会得到
			 * 无用派生表达式的 O(N^2) 爆炸，对应
			 * 子项的各种组合。（但 add_child_join_rel_equivalences
			 * 可以为按分区连接追加有针对性的组合。）
			 */
			if (cur_em->em_is_child)
				continue;		/* 这里忽略子项 */

			/*
			 * 只考虑引用并且能在
			 * 子关系最顶层父关系上计算的成员。尤其要排除
			 * varnullingrels 非空的父关系 Vars。翻译
			 * 那些也许会失败，若变换后的表达式不是
			 * 简单 Var；而且无论如何也产生不出对
			 * 为子关系创建计划有任何用处的成员。
			 */
			if (bms_is_subset(cur_em->em_relids, top_parent_relids) &&
				!bms_is_empty(cur_em->em_relids))
			{
				/* 好，生成变换后的子版本 */
				Expr	   *child_expr;
				Relids		new_relids;

				if (parent_rel->reloptkind == RELOPT_BASEREL)
				{
					/* 简单的单层变换 */
					child_expr = (Expr *)
						adjust_appendrel_attrs(root,
											   (Node *) cur_em->em_expr,
											   1, &appinfo);
				}
				else
				{
					/* 必须做多层变换 */
					child_expr = (Expr *)
						adjust_appendrel_attrs_multilevel(root,
														  (Node *) cur_em->em_expr,
														  child_rel,
														  child_rel->top_parent);
				}

				/*
				 * 变换 em_relids 使之匹配。注意这里 *不* 做
				 * pull_varnos(child_expr)，例如
				 * 变换也许替换进了一个常量，但我们
				 * 不想把子成员标记为常量。
				 */
				new_relids = bms_difference(cur_em->em_relids,
											top_parent_relids);
				new_relids = bms_add_members(new_relids, child_relids);

				(void) add_eq_member(cur_ec, child_expr, new_relids,
									 cur_em->em_jdomain,
									 cur_em, cur_em->em_datatype);

				/* 为子关系记下该 EC 索引 */
				child_rel->eclass_indexes = bms_add_member(child_rel->eclass_indexes, i);
			}
		}
	}
}

/*
 * add_child_join_rel_equivalences
 *	  与 add_child_rel_equivalences() 类似，但针对连接关系
 *
 * 这里找出与最顶层父连接关系相关的 EC，并加入引用
 * 这个子连接关系的变换后成员表达式。
 *
 * 除非我们至少有一些
 * 理由相信它生成的 EC 成员会有用，否则根本不会调用本函数。
 */
void
add_child_join_rel_equivalences(PlannerInfo *root,
								int nappinfos, AppendRelInfo **appinfos,
								RelOptInfo *parent_joinrel,
								RelOptInfo *child_joinrel)
{
	Relids		top_parent_relids = child_joinrel->top_parent_relids;
	Relids		child_relids = child_joinrel->relids;
	Bitmapset  *matching_ecs;
	MemoryContext oldcontext;
	int			i;

	Assert(IS_JOIN_REL(child_joinrel) && IS_JOIN_REL(parent_joinrel));

	/* 只需考虑提到父连接关系的 EC */
	matching_ecs = get_eclass_indexes_for_relids(root, top_parent_relids);

	/*
	 * 若在 GEQO 连接规划期间被调用，仍必须
	 * 在主规划器上下文中创建任何新的 EC 成员，以免
	 * GEQO 上下文重置之后 EC 数据结构损坏。这有
	 * 问题，因为重复的 GEQO 周期会泄漏内存。目前
	 * 膨胀总比崩溃好。若这真成了问题，
	 * 就得想办法避免生成重复的 EC 成员。
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	i = -1;
	while ((i = bms_next_member(matching_ecs, i)) >= 0)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) list_nth(root->eq_classes, i);
		int			num_members;

		/*
		 * 若该 EC 含有易变表达式，生成子
		 * EM 会非常危险，因此跳过。我们依赖
		 * 易变 EC 只有一个 EM。
		 */
		if (cur_ec->ec_has_volatile)
			continue;

		/* 对 get_eclass_indexes_for_relids 的结果做健全性检查 */
		Assert(bms_overlap(top_parent_relids, cur_ec->ec_relids));

		/*
		 * 这里不用 foreach()，因为扫描
		 * 新加入的子成员没有意义，可以在最后一个
		 * 原有 EC 成员之后停下。
		 */
		num_members = list_length(cur_ec->ec_members);
		for (int pos = 0; pos < num_members; pos++)
		{
			EquivalenceMember *cur_em = (EquivalenceMember *) list_nth(cur_ec->ec_members, pos);

			if (cur_em->em_is_const)
				continue;		/* 这里忽略常量 */

			/*
			 * 这里只考虑原始 EC 成员，不考虑
			 * 已经变换过的子成员。
			 */
			if (cur_em->em_is_child)
				continue;		/* 这里忽略子项 */

			/*
			 * 可以忽略只引用单个基关系的表达式，
			 * 因为 add_child_rel_equivalences 应当已经处理过它们。
			 */
			if (bms_membership(cur_em->em_relids) != BMS_MULTIPLE)
				continue;

			/* 该成员是否引用子关系的最顶层父关系？ */
			if (bms_overlap(cur_em->em_relids, top_parent_relids))
			{
				/* 是的，生成变换后的子版本 */
				Expr	   *child_expr;
				Relids		new_relids;

				if (parent_joinrel->reloptkind == RELOPT_JOINREL)
				{
					/* 简单的单层变换 */
					child_expr = (Expr *)
						adjust_appendrel_attrs(root,
											   (Node *) cur_em->em_expr,
											   nappinfos, appinfos);
				}
				else
				{
					/* 必须做多层变换 */
					Assert(parent_joinrel->reloptkind == RELOPT_OTHER_JOINREL);
					child_expr = (Expr *)
						adjust_appendrel_attrs_multilevel(root,
														  (Node *) cur_em->em_expr,
														  child_joinrel,
														  child_joinrel->top_parent);
				}

				/*
				 * 变换 em_relids 使之匹配。注意这里 *不* 做
				 * pull_varnos(child_expr)，例如
				 * 变换也许替换进了一个常量，但我们
				 * 不想把子成员标记为常量。
				 */
				new_relids = bms_difference(cur_em->em_relids,
											top_parent_relids);
				new_relids = bms_add_members(new_relids, child_relids);

				(void) add_eq_member(cur_ec, child_expr, new_relids,
									 cur_em->em_jdomain,
									 cur_em, cur_em->em_datatype);
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);
}


/*
 * generate_implied_equalities_for_column
 *	  创建可用于特定列的、由 EC 派生的连接子句。
 *
 * indxpath.c 用它从 EC 中抽出可能可索引的连接子句，
 * 外部数据包装器也可以为类似目的使用。
 * 我们假定只有单表 Vars 中的表达式值得关心，
 * 但调用方提供回调函数，以精确指出
 * 它想知道哪些这样的表达式。
 *
 * 我们假定任何给定的表/索引列只会出现在一个 EC 中。
 * （除了最病态的情形，这应当成立；若不成立，
 * 我们反正在第一个匹配处停下。）因此返回的
 * 是一份冗余的子句列表，把该表/索引列与它已知相等的每一个
 * 其他关系的值等起来。其中任何一条
 * 都可以用来创建参数化路径，而且
 * 用不止一条没有价值。（但为每一条分别创建
 * 一条参数化路径是值得的，因为那会导致不同的
 * 连接顺序。）
 *
 * 调用方可以传入一组我们不想与之连接的 Relids，
 * 以免费力去创建无用子句。
 */
List *
generate_implied_equalities_for_column(PlannerInfo *root,
									   RelOptInfo *rel,
									   ec_matches_callback_type callback,
									   void *callback_arg,
									   Relids prohibited_rels)
{
	List	   *result = NIL;
	bool		is_child_rel = (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
	Relids		parent_relids;
	int			i;

	/* 可以依赖 eclass_indexes */
	Assert(root->ec_merging_done);

	/* 索引只存在于基关系或 "other" 成员关系上。 */
	Assert(IS_SIMPLE_REL(rel));

	/* 若是子关系，就需要知道它的 parent(s) 是什么 */
	if (is_child_rel)
		parent_relids = find_childrel_parents(root, rel);
	else
		parent_relids = NULL;	/* 未使用，但用来消除编译器告警 */

	i = -1;
	while ((i = bms_next_member(rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *cur_ec = (EquivalenceClass *) list_nth(root->eq_classes, i);
		EquivalenceMember *cur_em;
		ListCell   *lc2;

		/* 健全性检查：eclass_indexes 只含该 rel 的 EC */
		Assert(is_child_rel || bms_is_subset(rel->relids, cur_ec->ec_relids));

		/*
		 * 若是常量或单成员，就不会生成连接子句（后一种
		 * 测试也覆盖易变情形）
		 */
		if (cur_ec->ec_has_const || list_length(cur_ec->ec_members) <= 1)
			continue;

		/*
		 * 扫描成员，寻找与目标列的匹配。注意
		 * 会考虑子 EC 成员，但仅当它们属于
		 * 目标关系时。（与普通成员不同，同一表达式
		 * 可以是不止一个 EC 的子成员。因此
		 * 子关系的目标列匹配到哪个 EC 可能取决于顺序。
		 * 这很烦人，但只发生在
		 * 边角情形，所以目前我们接受只报告第一个
		 * 匹配。另见 get_eclass_for_sort_expr。）
		 */
		cur_em = NULL;
		foreach(lc2, cur_ec->ec_members)
		{
			cur_em = (EquivalenceMember *) lfirst(lc2);
			if (bms_equal(cur_em->em_relids, rel->relids) &&
				callback(root, rel, cur_ec, cur_em, callback_arg))
				break;
			cur_em = NULL;
		}

		if (!cur_em)
			continue;

		/*
		 * 找到匹配了。扫描其他 EC 成员并尝试生成
		 * 连接子句。
		 */
		foreach(lc2, cur_ec->ec_members)
		{
			EquivalenceMember *other_em = (EquivalenceMember *) lfirst(lc2);
			Oid			eq_op;
			RestrictInfo *rinfo;

			if (other_em->em_is_child)
				continue;		/* 这里忽略子项 */

			/* 确保它会连接到另一个关系 */
			if (other_em == cur_em ||
				bms_overlap(other_em->em_relids, rel->relids))
				continue;

			/* 若调用方不想与该关系连接，就放弃 */
			if (bms_overlap(other_em->em_relids, prohibited_rels))
				continue;

			/*
			 * 另外，若这是子关系，避免生成与其
			 * 父 rel(s) 的无用连接。
			 */
			if (is_child_rel &&
				bms_overlap(parent_relids, other_em->em_relids))
				continue;

			eq_op = select_equality_operator(cur_ec,
											 cur_em->em_datatype,
											 other_em->em_datatype);
			if (!OidIsValid(eq_op))
				continue;

			/* 设置 parent_ec，以标记它与其他连接子句冗余 */
			rinfo = create_join_clause(root, cur_ec, eq_op,
									   cur_em, other_em,
									   cur_ec);

			result = lappend(result, rinfo);
		}

		/*
		 * 若不知怎么没能创建任何连接子句，不妨
		 * 继续扫描 EC 寻找另一个匹配。但若已经创建了任何一条，
		 * 就做完了，因为不想返回非冗余子句。
		 */
		if (result)
			break;
	}

	return result;
}

/*
 * have_relevant_eclass_joinclause
 *		检测是否有 EquivalenceClass 能产生
 *		涉及给定两个关系的连接子句。
 *
 * 这本质上是
 * generate_join_implied_equalities() 大幅精简后的版本。注意偶尔错误地说 "yes"
 * 是可以的。因此我们不管缺少
 * 跨类型操作符是否会阻止子句真正生成这类细节。
 * 假阴性也不总是致命的：它们会劝阻、但不会
 * 完全阻止对特定连接途径的考察。
 */
bool
have_relevant_eclass_joinclause(PlannerInfo *root,
								RelOptInfo *rel1, RelOptInfo *rel2)
{
	Bitmapset  *matching_ecs;
	int			i;

	/*
	 * 只检查同时提到 rel1 和 rel2 的 eclass。
	 *
	 * 注意我们不考虑某个 eclass 生成
	 * 只提到其中一个关系再加上一个可由它们形成的外连接的 "连接" 子句这种可能。
	 * 虽然形成该外连接时必须
	 * 正确强制这些子句，它们似乎并不是
	 * 把这次连接优先于其他连接的充分理由。连接
	 * 排序规则会在必要时强制做这次连接。
	 */
	matching_ecs = get_common_eclass_indexes(root, rel1->relids,
											 rel2->relids);

	i = -1;
	while ((i = bms_next_member(matching_ecs, i)) >= 0)
	{
		EquivalenceClass *ec = (EquivalenceClass *) list_nth(root->eq_classes,
															 i);

		/*
		 * 健全性检查：get_common_eclass_indexes 只给出同时
		 * 包含两个关系的 EC。
		 */
		Assert(bms_overlap(rel1->relids, ec->ec_relids));
		Assert(bms_overlap(rel2->relids, ec->ec_relids));

		/*
		 * 若是单成员，就不会生成连接子句（本测试也覆盖
		 * 易变情形）
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * 不必检查 EC 的各个成员，因为
		 * 我们只关心每个关系是否与至少一个成员的 relid 重叠，
		 * 而 get_common_eclass_indexes() 加上上面的单
		 * 成员检查就足以证明这一点。（与
		 * have_relevant_joinclause() 一样，不必要求该 EC 能
		 * 形成恰好关联给定两个关系的连接子句，只要求
		 * 它能形成提到两者的连接子句，而若两者都与 ec_relids 重叠，
		 * 这肯定成立。）
		 *
		 * 注意我们不测试 ec_broken；若测试，就需要单独的代码
		 * 路径去查看 ec_sources。反正检查成员资格
		 * 作为可能过于乐观的启发式是可以的。
		 *
		 * 也不测试 ec_has_const，尽管常量 eclass 不会
		 * 生成真正的连接子句。这是因为若有 "WHERE a.x =
		 * b.y and a.x = 42"，就值得考虑 a 与 b 之间的连接，
		 * 因为连接结果很可能很小，即使它最后
		 * 会变成无限定条件的嵌套循环。
		 */

		return true;
	}

	return false;
}


/*
 * has_relevant_eclass_joinclause
 *		检测是否有 EquivalenceClass 能产生
 *		涉及给定关系以及任何其他东西的连接子句。
 *
 * 这与 have_relevant_eclass_joinclause 相同，只是另一个关系
 * 被隐含定义为 "查询中的其他一切"。
 */
bool
has_relevant_eclass_joinclause(PlannerInfo *root, RelOptInfo *rel1)
{
	Bitmapset  *matched_ecs;
	int			i;

	/* 只检查提到 rel1 的 eclass */
	matched_ecs = get_eclass_indexes_for_relids(root, rel1->relids);

	i = -1;
	while ((i = bms_next_member(matched_ecs, i)) >= 0)
	{
		EquivalenceClass *ec = (EquivalenceClass *) list_nth(root->eq_classes,
															 i);

		/*
		 * 若是单成员，就不会生成连接子句（本测试也覆盖
		 * 易变情形）
		 */
		if (list_length(ec->ec_members) <= 1)
			continue;

		/*
		 * 按 have_relevant_eclass_joinclause 里的注释，只要
		 * 找到一个同时提到本关系和其他某个关系的 EC 就够了。
		 */
		if (!bms_is_subset(ec->ec_relids, rel1->relids))
			return true;
	}

	return false;
}


/*
 * eclass_useful_for_merging
 *	  检测该 EC 能否产生任何
 *	  针对指定关系的可归并连接子句。
 *
 * 这只是启发式测试，不必精确；错误地说 "yes"
 * 比说 "no" 更好。因此我们不管缺少
 * 跨类型操作符是否会阻止子句
 * 真正生成这类细节。
 */
bool
eclass_useful_for_merging(PlannerInfo *root,
						  EquivalenceClass *eclass,
						  RelOptInfo *rel)
{
	Relids		relids;
	ListCell   *lc;

	Assert(!eclass->ec_merged);

	/*
	 * 若是常量或单成员，就不会生成连接子句（后一种测试
	 * 也覆盖易变情形）
	 */
	if (eclass->ec_has_const || list_length(eclass->ec_members) <= 1)
		return false;

	/*
	 * 注意我们不测试 ec_broken；若测试，就需要单独的代码路径
	 * 去查看 ec_sources。反正检查成员
	 * 作为可能过于乐观的启发式是可以的。
	 */

	/* 若指定的关系是子关系，就必须考虑最顶层父关系 */
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
	else
		relids = rel->relids;

	/* 若关系已包含 eclass 的全部成员，就没有搜索的意义 */
	if (bms_is_subset(eclass->ec_relids, relids))
		return false;

	/* 要连接，就需要一个不在给定关系中的成员 */
	foreach(lc, eclass->ec_members)
	{
		EquivalenceMember *cur_em = (EquivalenceMember *) lfirst(lc);

		if (cur_em->em_is_child)
			continue;			/* 这里忽略子项 */

		if (!bms_overlap(cur_em->em_relids, relids))
			return true;
	}

	return false;
}


/*
 * is_redundant_derived_clause
 *		测试 rinfo 是否与 clauselist 中任何子句派生自同一 EC；
 *		若是，就可以认为它表示的条件与该列表成员
 *		冗余。
 */
bool
is_redundant_derived_clause(RestrictInfo *rinfo, List *clauselist)
{
	EquivalenceClass *parent_ec = rinfo->parent_ec;
	ListCell   *lc;

	/* 若它不是某个 EC 中可能冗余的子句，则失败 */
	if (parent_ec == NULL)
		return false;

	foreach(lc, clauselist)
	{
		RestrictInfo *otherrinfo = (RestrictInfo *) lfirst(lc);

		if (otherrinfo->parent_ec == parent_ec)
			return true;
	}

	return false;
}

/*
 * is_redundant_with_indexclauses
 *		测试 rinfo 是否与 IndexClause
 *		列表中的任何子句冗余。这里为方便起见，既测试简单的同一性，也测试
 *		它是否与列表中任何成员派生自同一 EC。
 */
bool
is_redundant_with_indexclauses(RestrictInfo *rinfo, List *indexclauses)
{
	EquivalenceClass *parent_ec = rinfo->parent_ec;
	ListCell   *lc;

	foreach(lc, indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		RestrictInfo *otherrinfo = iclause->rinfo;

		/* 若 indexclause 是有损的，它不会精确强制该条件 */
		if (iclause->lossy)
			continue;

		/* 若是同一子句则匹配（指针相等应当就够了） */
		if (rinfo == otherrinfo)
			return true;
		/* 若派生自同一 EC 则匹配 */
		if (parent_ec && otherrinfo->parent_ec == parent_ec)
			return true;

		/*
		 * 不必看 iclause->indexquals 里的派生子句；若父子句不匹配，
		 * 它们也不可能匹配。
		 */
	}

	return false;
}

/*
 * get_eclass_indexes_for_relids
 *		构造并返回一个 Bitmapset，其中是 root 的
 *		eq_classes 列表里所有提到这些 relid 中任何一个的 eclass 的索引
 */
static Bitmapset *
get_eclass_indexes_for_relids(PlannerInfo *root, Relids relids)
{
	Bitmapset  *ec_indexes = NULL;
	int			i = -1;

	/* 可以依赖 eclass_indexes */
	Assert(root->ec_merging_done);

	while ((i = bms_next_member(relids, i)) > 0)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel == NULL)		/* 必须是外连接 */
		{
			Assert(bms_is_member(i, root->outer_join_rels));
			continue;
		}

		ec_indexes = bms_add_members(ec_indexes, rel->eclass_indexes);
	}
	return ec_indexes;
}

/*
 * get_common_eclass_indexes
 *		构造并返回一个 Bitmapset，其中是 root 的
 *		eq_classes 列表里所有同时提到
 *		relids1 和 relids2 中关系的 eclass 的索引。
 */
static Bitmapset *
get_common_eclass_indexes(PlannerInfo *root, Relids relids1, Relids relids2)
{
	Bitmapset  *rel1ecs;
	Bitmapset  *rel2ecs;
	int			relid;

	rel1ecs = get_eclass_indexes_for_relids(root, relids1);

	/*
	 * 当 relids2 是单元素集合时，可以直接用该关系的 eclass_indexes
	 * 应付过去。
	 */
	if (bms_get_singleton_member(relids2, &relid))
		rel2ecs = root->simple_rel_array[relid]->eclass_indexes;
	else
		rel2ecs = get_eclass_indexes_for_relids(root, relids2);

	/* 计算并返回公共 EC 索引，复用左侧输入。 */
	return bms_int_members(rel1ecs, rel2ecs);
}
