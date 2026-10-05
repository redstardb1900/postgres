/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/restrictinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"


static RestrictInfo *make_restrictinfo_internal(PlannerInfo *root,
												Expr *clause,
												Expr *orclause,
												bool is_pushed_down,
												bool has_clone,
												bool is_clone,
												bool pseudoconstant,
												Index security_level,
												Relids required_relids,
												Relids incompatible_relids,
												Relids outer_relids);
static Expr *make_sub_restrictinfos(PlannerInfo *root,
									Expr *clause,
									bool is_pushed_down,
									bool has_clone,
									bool is_clone,
									bool pseudoconstant,
									Index security_level,
									Relids required_relids,
									Relids incompatible_relids,
									Relids outer_relids);


/*
 * make_restrictinfo
 *
 * 建立一个包含给定子表达式的 RestrictInfo 节点。
 *
 * RestrictInfo 的 is_pushed_down、has_clone、is_clone 和伪常量标志
 * 必须由调用者提供，security_level、incompatible_relids 和 outer_relids 的正确值
 * 也一样。
 * required_relids 可以为 NULL，此时它默认取实际子句
 * 内容（即 clause_relids）。
 *
 * 我们初始化只依赖于给定子表达式的字段，其余
 * 依赖于上下文（或也许永远不需要）的字段留到以后
 * 再填。
 */
RestrictInfo *
make_restrictinfo(PlannerInfo *root,
				  Expr *clause,
				  bool is_pushed_down,
				  bool has_clone,
				  bool is_clone,
				  bool pseudoconstant,
				  Index security_level,
				  Relids required_relids,
				  Relids incompatible_relids,
				  Relids outer_relids)
{
	/*
	 * 若它是 OR 子句，建立一份修改过的副本，把 RestrictInfos 插在
	 * 顶层 AND/OR 结构的每个子子句之上。
	 */
	if (is_orclause(clause))
		return (RestrictInfo *) make_sub_restrictinfos(root,
													   clause,
													   is_pushed_down,
													   has_clone,
													   is_clone,
													   pseudoconstant,
													   security_level,
													   required_relids,
													   incompatible_relids,
													   outer_relids);

	/* 不应当是 AND 子句，否则 AND/OR 展平搞乱了 */
	Assert(!is_andclause(clause));

	return make_restrictinfo_internal(root,
									  clause,
									  NULL,
									  is_pushed_down,
									  has_clone,
									  is_clone,
									  pseudoconstant,
									  security_level,
									  required_relids,
									  incompatible_relids,
									  outer_relids);
}

/*
 * make_restrictinfo_internal
 *
 * 主入口和递归情形的公共代码。
 */
static RestrictInfo *
make_restrictinfo_internal(PlannerInfo *root,
						   Expr *clause,
						   Expr *orclause,
						   bool is_pushed_down,
						   bool has_clone,
						   bool is_clone,
						   bool pseudoconstant,
						   Index security_level,
						   Relids required_relids,
						   Relids incompatible_relids,
						   Relids outer_relids)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		baserels;

	restrictinfo->clause = clause;
	restrictinfo->orclause = orclause;
	restrictinfo->is_pushed_down = is_pushed_down;
	restrictinfo->pseudoconstant = pseudoconstant;
	restrictinfo->has_clone = has_clone;
	restrictinfo->is_clone = is_clone;
	restrictinfo->can_join = false; /* 下面可能会设置 */
	restrictinfo->security_level = security_level;
	restrictinfo->incompatible_relids = incompatible_relids;
	restrictinfo->outer_relids = outer_relids;

	/*
	 * 若它可能被更低层的安全限定条件推迟，弄清
	 * 它是否防泄漏。对层次为零的限定条件可以跳过这项测试，
	 * 因为它们反正不会因安全理由被推迟。
	 */
	if (security_level > 0)
		restrictinfo->leakproof = !contain_leaked_vars((Node *) clause);
	else
		restrictinfo->leakproof = false;	/* 实际上是 "不知道" */

	/*
	 * 把易变性标成未知。contain_volatile_functions 函数
	 * 会在第一次带着这个 RestrictInfo 被调用时，
	 * 判断是否有任何易变函数。
	 */
	restrictinfo->has_volatile = VOLATILITY_UNKNOWN;

	/*
	 * 若它是二元操作子句，设置左/右 relid 信息。无论如何
	 * 都设置整个子句的 relid 信息。
	 */
	if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
	{
		restrictinfo->left_relids = pull_varnos(root, get_leftop(clause));
		restrictinfo->right_relids = pull_varnos(root, get_rightop(clause));

		restrictinfo->clause_relids = bms_union(restrictinfo->left_relids,
												restrictinfo->right_relids);

		/*
		 * 它看起来像普通连接子句吗，也就是一个二元操作符
		 * 关联来自不同关系的表达式？若是，我们
		 * 也许能在连接算法里用它。注意这是
		 * 不论上下文都做的纯语法测试。
		 */
		if (!bms_is_empty(restrictinfo->left_relids) &&
			!bms_is_empty(restrictinfo->right_relids) &&
			!bms_overlap(restrictinfo->left_relids,
						 restrictinfo->right_relids))
		{
			restrictinfo->can_join = true;
			/* 伪常量当然不应当为真 */
			Assert(!restrictinfo->pseudoconstant);
		}
	}
	else
	{
		/* 不是二元操作子句，因此把左/右 relid 集合标成空 */
		restrictinfo->left_relids = NULL;
		restrictinfo->right_relids = NULL;
		/* 并用较费事的办法取得全部 relid 集合 */
		restrictinfo->clause_relids = pull_varnos(root, (Node *) clause);
	}

	/* required_relids 默认取 clause_relids */
	if (required_relids != NULL)
		restrictinfo->required_relids = required_relids;
	else
		restrictinfo->required_relids = restrictinfo->clause_relids;

	/*
	 * 统计 clause_relids 中出现的基关系个数。为此，
	 * 只需删掉 root->outer_join_rels 中提到的关系并清点
	 * 剩下的。因为我们在 deconstruct_jointree 期间被调用，而那正是
	 * 填充 outer_join_rels 的同一次树遍历，这看起来有点
	 * 不安全；但应当没问题，因为 deconstruct_jointree 中的递归
	 * 应当已经访问过本子句可能提到的
	 * 任何外连接。
	 */
	baserels = bms_difference(restrictinfo->clause_relids,
							  root->outer_join_rels);
	restrictinfo->num_base_rels = bms_num_members(baserels);
	bms_free(baserels);

	/*
	 * 给这个 RestrictInfo 标一个新的序列号。
	 */
	restrictinfo->rinfo_serial = ++(root->last_rinfo_serial);

	/*
	 * 把所有可缓存字段填上尚未设置标记。这些
	 * 在需要之前都不会计算。特别要注意，这里不把二元操作子句
	 * 标成可归并连接或可哈希连接；
	 * 那只在它出现在正确上下文（连接子句列表的顶层）时
	 * 才会发生。
	 */
	restrictinfo->parent_ec = NULL;

	restrictinfo->eval_cost.startup = -1;
	restrictinfo->norm_selec = -1;
	restrictinfo->outer_selec = -1;

	restrictinfo->mergeopfamilies = NIL;

	restrictinfo->left_ec = NULL;
	restrictinfo->right_ec = NULL;
	restrictinfo->left_em = NULL;
	restrictinfo->right_em = NULL;
	restrictinfo->scansel_cache = NIL;

	restrictinfo->outer_is_left = false;

	restrictinfo->hashjoinoperator = InvalidOid;

	restrictinfo->left_bucketsize = -1;
	restrictinfo->right_bucketsize = -1;
	restrictinfo->left_mcvfreq = -1;
	restrictinfo->right_mcvfreq = -1;

	restrictinfo->left_hasheqoperator = InvalidOid;
	restrictinfo->right_hasheqoperator = InvalidOid;

	return restrictinfo;
}

/*
 * 递归地把子 RestrictInfo 节点插入布尔表达式。
 *
 * 我们把 RestrictInfos 放在简单（非 AND/OR）子句和
 * 子 OR 子句之上，但不放在子 AND 子句之上，因为没有必要。
 * 这也许显得古怪，但它与我们在 RestrictInfo 列表顶层使用
 * 隐式 AND 列表密切相关。只有 OR 和
 * 简单子句才是有效的 RestrictInfos。
 *
 * 同样的 is_pushed_down、has_clone、is_clone 和伪常量标志
 * 值可以应用到结果中的全部 RestrictInfo 节点。security_level、
 * incompatible_relids 和 outer_relids 也一样。
 *
 * 给定的 required_relids 挂到我们的顶层输出上，
 * 但任何 OR 子句成分可以默认只取
 * 所含的关系。
 */
static Expr *
make_sub_restrictinfos(PlannerInfo *root,
					   Expr *clause,
					   bool is_pushed_down,
					   bool has_clone,
					   bool is_clone,
					   bool pseudoconstant,
					   Index security_level,
					   Relids required_relids,
					   Relids incompatible_relids,
					   Relids outer_relids)
{
	if (is_orclause(clause))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			orlist = lappend(orlist,
							 make_sub_restrictinfos(root,
													lfirst(temp),
													is_pushed_down,
													has_clone,
													is_clone,
													pseudoconstant,
													security_level,
													NULL,
													incompatible_relids,
													outer_relids));
		return (Expr *) make_restrictinfo_internal(root,
												   clause,
												   make_orclause(orlist),
												   is_pushed_down,
												   has_clone,
												   is_clone,
												   pseudoconstant,
												   security_level,
												   required_relids,
												   incompatible_relids,
												   outer_relids);
	}
	else if (is_andclause(clause))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			andlist = lappend(andlist,
							  make_sub_restrictinfos(root,
													 lfirst(temp),
													 is_pushed_down,
													 has_clone,
													 is_clone,
													 pseudoconstant,
													 security_level,
													 required_relids,
													 incompatible_relids,
													 outer_relids));
		return make_andclause(andlist);
	}
	else
		return (Expr *) make_restrictinfo_internal(root,
												   clause,
												   NULL,
												   is_pushed_down,
												   has_clone,
												   is_clone,
												   pseudoconstant,
												   security_level,
												   required_relids,
												   incompatible_relids,
												   outer_relids);
}

/*
 * commute_restrictinfo
 *
 * 给定包含二元操作子句的 RestrictInfo，产生一个 RestrictInfo，
 * 表示该子句的交换。调用者必须传入
 * 交换操作符的 OID（它想必已经查过，否则
 * 不会知道这是有效的）。
 *
 * 注意结果与给定的 RestrictInfo 共享子结构。
 * 对推导出的索引限定条件这一预期用途没问题，但若
 * 源可能被修改，也许有危险。还要注意我们
 * 不作检查就假定交换操作符与原操作符同属
 * 相同的 btree 和 hash 操作符类。
 */
RestrictInfo *
commute_restrictinfo(RestrictInfo *rinfo, Oid comm_op)
{
	RestrictInfo *result;
	OpExpr	   *newclause;
	OpExpr	   *clause = castNode(OpExpr, rinfo->clause);

	Assert(list_length(clause->args) == 2);

	/* 平铺复制 clause 的全部字段 ... */
	newclause = makeNode(OpExpr);
	memcpy(newclause, clause, sizeof(OpExpr));

	/* ... 并调整为了交换它而需要改的那些 */
	newclause->opno = comm_op;
	newclause->opfuncid = InvalidOid;
	newclause->args = list_make2(lsecond(clause->args),
								 linitial(clause->args));

	/* 同样，平铺复制 rinfo 的全部字段 ... */
	result = makeNode(RestrictInfo);
	memcpy(result, rinfo, sizeof(RestrictInfo));

	/*
	 * ... 并调整我们需要改的那些。特别要注意我们可以
	 * 保留任何缓存的选择率或代价估计，因为对新子句它们应当
	 * 相同。同样可以保留源的
	 * parent_ec。保持同一个 rinfo_serial 也很重要。
	 */
	result->clause = (Expr *) newclause;
	result->left_relids = rinfo->right_relids;
	result->right_relids = rinfo->left_relids;
	Assert(result->orclause == NULL);
	result->left_ec = rinfo->right_ec;
	result->right_ec = rinfo->left_ec;
	result->left_em = rinfo->right_em;
	result->right_em = rinfo->left_em;
	result->scansel_cache = NIL;	/* 不值得更新这个 */
	if (rinfo->hashjoinoperator == clause->opno)
		result->hashjoinoperator = comm_op;
	else
		result->hashjoinoperator = InvalidOid;
	result->left_bucketsize = rinfo->right_bucketsize;
	result->right_bucketsize = rinfo->left_bucketsize;
	result->left_mcvfreq = rinfo->right_mcvfreq;
	result->right_mcvfreq = rinfo->left_mcvfreq;
	result->left_hasheqoperator = InvalidOid;
	result->right_hasheqoperator = InvalidOid;

	return result;
}

/*
 * restriction_is_or_clause
 *
 * 当 restrictinfo 节点含有 or 子句时返回真。
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	if (restrictinfo->orclause != NULL)
		return true;
	else
		return false;
}

/*
 * restriction_is_securely_promotable
 *
 * 若可以提前求值这条子句则返回真，也就是在
 * 附在指定关系上的其他限制子句之前。
 */
bool
restriction_is_securely_promotable(RestrictInfo *restrictinfo,
								   RelOptInfo *rel)
{
	/*
	 * 若该关系没有必须排在这条之前的 baserestrictinfo 子句，
	 * 或者这一条是防泄漏的，就可以。
	 */
	if (restrictinfo->security_level <= rel->baserestrict_min_security ||
		restrictinfo->leakproof)
		return true;
	else
		return false;
}

/*
 * 检测 RestrictInfo 的子句是否为常量 TRUE（注意它
 * 肯定是布尔类型）。这种 WHERE 子句不可能活过限定条件
 * 规范化，但 equivclass.c 可能出于其中讨论的原因生成这种 RestrictInfos。
 * 建立完成的计划时应当再次丢掉它们，
 * 这由下面几个函数处理。
 */
static inline bool
rinfo_is_constant_true(RestrictInfo *rinfo)
{
	return IsA(rinfo->clause, Const) &&
		!((Const *) rinfo->clause)->constisnull &&
		DatumGetBool(((Const *) rinfo->clause)->constvalue);
}

/*
 * get_actual_clauses
 *
 * 返回包含 restrictinfo_list 中裸子句的列表。
 *
 * 只用于没有任何 RestrictInfos 可能
 * 是伪常量子句的情形（例如在 indexqual 列表上就可以）。
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		Assert(!rinfo->pseudoconstant);
		Assert(!rinfo_is_constant_true(rinfo));

		result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_clauses
 *
 * 从 restrictinfo_list 抽出裸子句，按 pseudoconstant 返回
 * 普通的那些或伪常量的那些。
 * 无论如何都丢掉常量 TRUE 子句。
 */
List *
extract_actual_clauses(List *restrictinfo_list,
					   bool pseudoconstant)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant == pseudoconstant &&
			!rinfo_is_constant_true(rinfo))
			result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_join_clauses
 *
 * 从 restrictinfo_list 抽出裸子句，把语义上
 * 匹配该连接层的那些与被下推的那些分开。
 * 伪常量和常量 TRUE 子句排除在结果之外。
 *
 * 这只用于外连接，因为对普通连接我们不关心
 * 是否被下推。
 */
void
extract_actual_join_clauses(List *restrictinfo_list,
							Relids joinrelids,
							List **joinquals,
							List **otherquals)
{
	ListCell   *l;

	*joinquals = NIL;
	*otherquals = NIL;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (RINFO_IS_PUSHED_DOWN(rinfo, joinrelids))
		{
			if (!rinfo->pseudoconstant &&
				!rinfo_is_constant_true(rinfo))
				*otherquals = lappend(*otherquals, rinfo->clause);
		}
		else
		{
			/* 连接限定条件不应当被标成伪常量 */
			Assert(!rinfo->pseudoconstant);
			if (!rinfo_is_constant_true(rinfo))
				*joinquals = lappend(*joinquals, rinfo->clause);
		}
	}
}

/*
 * has_pseudoconstant_clauses
 *
 * 若 restrictinfo_list 含有伪常量子句则返回真。
 *
 * 这在我们决定是否允许扩展考虑
 * 在 add_paths_to_joinrel() 中下推连接时使用。
 */
bool
has_pseudoconstant_clauses(PlannerInfo *root,
						   List *restrictinfo_list)
{
	ListCell   *l;

	/* 若已知没有伪常量，就不必看 */
	if (!root->hasPseudoConstantQuals)
		return false;

	/* 看 RestrictInfo 列表中是否有伪常量 */
	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant &&
			!rinfo_is_constant_true(rinfo))
			return true;
	}
	return false;
}

/*
 * join_clause_is_movable_to
 *		测试一条连接子句是否适合作为
 *		对指定基关系扫描的参数化候选。
 *
 * 可移动的连接子句是可以安全地在低于
 * 其正常语义层次（即其 required_relids）的关系上求值的子句，只要提供
 * 它从其他关系需要的变量值。
 *
 * 我们坚持该子句确实引用目标关系；这
 * 防止退化连接子句被不当地移动，并确保
 * 一条子句只有唯一的地方可以下移。
 *
 * 我们没法把外连接子句移进其外连接的不可空一侧，
 * 因为那会改变结果（行会被抑制，
 * 而不是被空值扩展）。
 *
 * 另外，子句之下不得有会把来自目标关系的
 * Var 置空的外连接。否则该子句给出的
 * 结果可能不同于它在正常语义层次上给出的结果。
 *
 * 另外，连接子句不得使用任何对目标关系有 LATERAL
 * 引用的关系，因为我们没法把这种关系放在
 * 与目标关系做嵌套循环时的外侧。
 *
 * 另外，我们拒绝外连接子句的 is_clone 版本。这会
 * 阻止我们生成变体参数化路径，
 * 它们仅仅在哪些外连接把参数化关系置空上有所不同。
 * 从最小参数化的 has_clone 版本生成一条路径
 * 就足够了。
 */
bool
join_clause_is_movable_to(RestrictInfo *rinfo, RelOptInfo *baserel)
{
	/* 子句必须在物理上引用目标关系 */
	if (!bms_is_member(baserel->relid, rinfo->clause_relids))
		return false;

	/* 无法把外连接子句移进该连接的外侧 */
	if (bms_is_member(baserel->relid, rinfo->outer_relids))
		return false;

	/*
	 * 目标关系的 Var 不得被任何外连接置空。我们可以
	 * 不必逐个翻找各个 Var 就检查这一点：看
	 * clause_relids（包含所有这些 Var 的 varnullingrels）是否包含
	 * 任何能把目标关系置空的外连接。你也许会反对说
	 * 这可能因来自某个其他关系的 Var 的外连接 relid
	 * 而拒绝该子句。不过那仍然意味着该子句
	 * 来自该外连接之上，不应当下推；因此
	 * 不应当有假阳性。
	 */
	if (bms_overlap(rinfo->clause_relids, baserel->nulling_relids))
		return false;

	/* 子句不得使用任何对本关系有 LATERAL 引用的关系 */
	if (bms_overlap(baserel->lateral_referencers, rinfo->clause_relids))
		return false;

	/* 也忽略克隆 */
	if (rinfo->is_clone)
		return false;

	return true;
}

/*
 * join_clause_is_movable_into
 *		测试一条连接子句是否可移动，并且能在
 *		当前连接上下文中求值。
 *
 * currentrelids：拟定求值位置的 relid
 * current_and_outer：currentrelids 与 required_outer 的并集
 *		（参数化的外侧关系）
 *
 * 若把当前 relid 与外侧 relid 分开传入、并在内部做 bms_union，
 * API 会更清楚一点；但由于多数
 * 调用者要把本函数用到多条子句上，我们让
 * 调用者自己完成并集。
 *
 * 显然，该子句只能引用当前
 * 关系加上外侧关系所能提供的 Var。我们还检查它确实
 * 至少引用一个当前 Var，确保该子句会被下推到
 * 参数化连接树中的唯一位置。并且检查我们
 * 没有把该子句推进其外连接的外侧。
 *
 * 我们以前需要检查没有把该子句推进更低层
 * 外连接的内侧。不过现在 clause_relids 包含
 * 对可能置空的外连接的引用，其他检查处理了那个
 * 问题。若该子句引用任何来自更低层外连接内侧的
 * Var，其 clause_relids 会提到那个外连接，使
 * 可求值性检查失败；而若它不引用这种 Var，则
 * 引用目标关系的检查会失败。
 *
 * 这里没有等价于 join_clause_is_movable_to 对
 * lateral_referencers 所做的检查。我们假定调用者不会询问，除非
 * 它已验证拟定的外侧关系对当前关系没有 LATERAL 引用。
 * （若考虑外侧关系在外、当前关系在内的连接路径，则
 * 应在开始这种考虑时就检查过；见 join_is_legal
 * 以及 joinpath.c 中的路径参数化检查。）另一方面，
 * 在 join_clause_is_movable_to 里我们问的是该子句能否
 * 针对某组有效的外侧关系而移动，因此享受不到
 * 依赖先前 LATERAL 引用有效性检查的好处。
 * 同样，这里不检查 is_clone：拒绝不合适的
 *
 * 克隆子句变体必须由上游处理。
 * 注意：若这里返回真，意味着该子句可以移到
 *
 * 这个连接关系，但这并不意味着这是
 * 它能移到的最低连接。调用者可能需要额外调用，以验证
 * 对拟定连接的任一输入，这次成功都不会再现。
 * 注意：get_joinrel_parampathinfo 依赖于如下事实：若
 *
 * current_and_outer 为 NULL，本函数将总是
 * 返回假
 * （因为前两项检查必有一项失败）。
 */
bool
join_clause_is_movable_into(RestrictInfo *rinfo,
							Relids currentrelids,
							Relids current_and_outer)
{
	/* 给定可用上下文，子句必须可求值 */
	if (!bms_is_subset(rinfo->clause_relids, current_and_outer))
		return false;

	/* 子句必须在物理上至少引用一个目标关系 */
	if (!bms_overlap(currentrelids, rinfo->clause_relids))
		return false;

	/* 无法把外连接子句移进该连接的外侧 */
	if (bms_overlap(currentrelids, rinfo->outer_relids))
		return false;

	return true;
}
