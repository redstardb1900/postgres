/*-------------------------------------------------------------------------
 *
 * tidpath.c
 *	  判断哪些 TID 条件可用于扫描
 *	  给定关系，并据此创建 TidPaths 与 TidRangePaths 的例程。
 *
 * 对于 TidPaths，我们寻找形如
 * "CTID = 伪常量" 的 WHERE 条件，这只需通过 heap_fetch()
 * 直接取出元组即可实现。我们也能处理用 OR 连起来的条件，
 * 例如 (CTID = const1) OR (CTID = const2)，以及形如
 * CTID = ANY(pseudoconstant_array) 的 ScalarArrayOpExpr 条件。尤其是
 * 这允许
 *		WHERE ctid IN (tid1, tid2, ...)
 *
 * 和索引扫描一样，我们对 "伪常量" 的定义相当宽松：
 * 只要不涉及 volatile 函数，也不涉及所考虑关系的 Var，
 * 我们都允许。属于该查询中其他关系的 Var 是允许的，
 * 由此产生参数化的 TID 扫描。
 *
 * 我们也支持 "WHERE CURRENT OF cursor" 条件（CurrentOfExpr），
 * 它相当于 "CTID = 运行时才能确定的 TID"。理论上
 * 可以把这翻译成把 CTID 与某个函数的结果做简单比较，
 * 但实践中更好的做法是把这种特殊节点表示
 * 一直保留到执行。
 *
 * 此外，对于形如
 * "CTID relop 伪常量" 的条件也可以创建 TidRangePaths，其中 relop 是 >、>=、<、<= 之一，以及
 * 由这类条件组成的 AND 子句。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/tidpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


/*
 * 这个 Var 是否表示指定基关系的 CTID 列？
 */
static inline bool
IsCTIDVar(Var *var, RelOptInfo *rel)
{
	/* vartype 检查纯粹是出于谨慎 */
	if (var->varattno == SelfItemPointerAttributeNumber &&
		var->vartype == TIDOID &&
		var->varno == rel->relid &&
		var->varnullingrels == NULL &&
		var->varlevelsup == 0)
		return true;
	return false;
}

/*
 * 检查 RestrictInfo 是否形如
 *		CTID OP 伪常量
 * 或
 *		伪常量 OP CTID
 * 其中 OP 是二元运算，CTID Var 属于关系 "rel"，
 * 且子句另一侧没有任何属于该关系的东西。
 */
static bool
IsBinaryTidClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	OpExpr	   *node;
	Node	   *arg1,
			   *arg2,
			   *other;
	Relids		other_relids;

	/* 必须是 OpExpr */
	if (!is_opclause(rinfo->clause))
		return false;
	node = (OpExpr *) rinfo->clause;

	/* OpExpr 必须有两个参数 */
	if (list_length(node->args) != 2)
		return false;
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	/* 在任一参数中寻找 CTID */
	other = NULL;
	other_relids = NULL;
	if (arg1 && IsA(arg1, Var) &&
		IsCTIDVar((Var *) arg1, rel))
	{
		other = arg2;
		other_relids = rinfo->right_relids;
	}
	if (!other && arg2 && IsA(arg2, Var) &&
		IsCTIDVar((Var *) arg2, rel))
	{
		other = arg1;
		other_relids = rinfo->left_relids;
	}
	if (!other)
		return false;

	/* 另一个参数必须是伪常量 */
	if (bms_is_member(rel->relid, other_relids) ||
		contain_volatile_functions(other))
		return false;

	return true;				/* 成功 */
}

/*
 * 检查 RestrictInfo 是否形如
 *		CTID = 伪常量
 * 或
 *		伪常量 = CTID
 * 其中 CTID Var 属于关系 "rel"，且另一侧
 * 没有任何属于该关系的东西。
 */
static bool
IsTidEqualClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	if (!IsBinaryTidClause(rinfo, rel))
		return false;

	if (((OpExpr *) rinfo->clause)->opno == TIDEqualOperator)
		return true;

	return false;
}

/*
 * 检查 RestrictInfo 是否形如
 *		CTID OP 伪常量
 * 或
 *		伪常量 OP CTID
 * 其中 OP 是 <、<=、> 或 >= 这类范围操作符，CTID Var 属于
 * 关系 "rel"，且子句另一侧没有任何属于该关系的东西。
 */
static bool
IsTidRangeClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	Oid			opno;

	if (!IsBinaryTidClause(rinfo, rel))
		return false;
	opno = ((OpExpr *) rinfo->clause)->opno;

	if (opno == TIDLessOperator || opno == TIDLessEqOperator ||
		opno == TIDGreaterOperator || opno == TIDGreaterEqOperator)
		return true;

	return false;
}

/*
 * 检查 RestrictInfo 是否形如
 *		CTID = ANY (pseudoconstant_array)
 * 其中 CTID Var 属于关系 "rel"，且另一侧
 * 没有任何属于该关系的东西。
 */
static bool
IsTidEqualAnyClause(PlannerInfo *root, RestrictInfo *rinfo, RelOptInfo *rel)
{
	ScalarArrayOpExpr *node;
	Node	   *arg1,
			   *arg2;

	/* 必须是 ScalarArrayOpExpr */
	if (!(rinfo->clause && IsA(rinfo->clause, ScalarArrayOpExpr)))
		return false;
	node = (ScalarArrayOpExpr *) rinfo->clause;

	/* 操作符必须是 tideq */
	if (node->opno != TIDEqualOperator)
		return false;
	if (!node->useOr)
		return false;
	Assert(list_length(node->args) == 2);
	arg1 = linitial(node->args);
	arg2 = lsecond(node->args);

	/* CTID 必须是第一个参数 */
	if (arg1 && IsA(arg1, Var) &&
		IsCTIDVar((Var *) arg1, rel))
	{
		/* 另一个参数必须是伪常量 */
		if (bms_is_member(rel->relid, pull_varnos(root, arg2)) ||
			contain_volatile_functions(arg2))
			return false;

		return true;			/* 成功 */
	}

	return false;
}

/*
 * 检查 RestrictInfo 是否为引用 "rel" 的 CurrentOfExpr。
 */
static bool
IsCurrentOfClause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	CurrentOfExpr *node;

	/* 必须是 CurrentOfExpr */
	if (!(rinfo->clause && IsA(rinfo->clause, CurrentOfExpr)))
		return false;
	node = (CurrentOfExpr *) rinfo->clause;

	/* 若它引用这个 rel，就可以 */
	if (node->cvarno == rel->relid)
		return true;

	return false;
}

/*
 * 从给定的 RestrictInfo 中抽出一组 CTID 条件
 *
 * 返回指定 rel 的 CTID 限定条件 RestrictInfos 列表（列表上
 * 隐含 OR 语义），若没有可用
 * 条件则返回 NIL。
 *
 * 本函数只考虑基本情况；AND/OR 组合在
 * 下面处理。因此返回的 List 绝不会超过一个元素。
 * （用 List 也许显得有点怪，但能简化调用方。）
 */
static List *
TidQualFromRestrictInfo(PlannerInfo *root, RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * 可以忽略伪常量子句（它们无法包含 Var，因此反正也
	 * 无法匹配）。
	 */
	if (rinfo->pseudoconstant)
		return NIL;

	/*
	 * 若子句必须等到某个更低安全级别的限制
	 * 子句之后，则拒绝它。
	 */
	if (!restriction_is_securely_promotable(rinfo, rel))
		return NIL;

	/*
	 * 检查所有基本情况。若匹配，则返回该子句。
	 */
	if (IsTidEqualClause(rinfo, rel) ||
		IsTidEqualAnyClause(root, rinfo, rel) ||
		IsCurrentOfClause(rinfo, rel))
		return list_make1(rinfo);

	return NIL;
}

/*
 * 从隐含 AND 的 RestrictInfos 列表中抽出一组 CTID 条件
 *
 * 返回指定 rel 的 CTID 限定条件 RestrictInfos 列表（列表上
 * 隐含 OR 语义），若没有可用的
 * 等值条件则返回 NIL。
 *
 * 本函数只负责处理 AND/OR 递归。
 */
static List *
TidQualFromRestrictInfoList(PlannerInfo *root, List *rlist, RelOptInfo *rel)
{
	List	   *rlst = NIL;
	ListCell   *l;

	foreach(l, rlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (restriction_is_or_clause(rinfo))
		{
			ListCell   *j;

			/*
			 * 必须能从 OR 的每个
			 * 子子句抽出一个 CTID 条件，否则无法使用它。
			 */
			foreach(j, ((BoolExpr *) rinfo->orclause)->args)
			{
				Node	   *orarg = (Node *) lfirst(j);
				List	   *sublist;

				/* OR 的参数应当是 ANDs 或子 RestrictInfos */
				if (is_andclause(orarg))
				{
					List	   *andargs = ((BoolExpr *) orarg)->args;

					/* 若有子 OR 则递归 */
					sublist = TidQualFromRestrictInfoList(root, andargs, rel);
				}
				else
				{
					RestrictInfo *ri = castNode(RestrictInfo, orarg);

					Assert(!restriction_is_or_clause(ri));
					sublist = TidQualFromRestrictInfo(root, ri, rel);
				}

				/*
				 * 若这一支什么都没找到，对这个 OR 子句就
				 * 无法做任何事。
				 */
				if (sublist == NIL)
				{
					rlst = NIL; /* 丢掉已经找到的内容 */
					break;		/* 离开遍历 OR 参数的循环 */
				}

				/*
				 * 可以，继续构造隐含 OR 的结果列表。
				 */
				rlst = list_concat(rlst, sublist);
			}
		}
		else
		{
			/* 不是 OR 子句，因此处理基本情况 */
			rlst = TidQualFromRestrictInfo(root, rinfo, rel);
		}

		/*
		 * 一旦找到任何可用的 CTID 条件就停止。理论上我们
		 * 可以从不同的 AND 子句得到 CTID 等值条件，
		 * 那时可以试着挑选最高效的一个。
		 * 实践中这种用法似乎很罕见，因此不必费心；我们
		 * 一找到第一个候选就退出。
		 */
		if (rlst)
			break;
	}

	return rlst;
}

/*
 * 从隐含 AND 的 RestrictInfos 列表中抽出一组 CTID 范围条件
 *
 * 返回指定 rel 的 CTID 范围限定条件 RestrictInfos 列表
 * （列表上隐含 AND 语义），若没有
 * 可用范围条件，或该 rel 的表访问方法不支持 TID 范围
 * 扫描，则返回 NIL。
 */
static List *
TidRangeQualFromRestrictInfoList(List *rlist, RelOptInfo *rel)
{
	List	   *rlst = NIL;
	ListCell   *l;

	if ((rel->amflags & AMFLAG_HAS_TID_RANGE) == 0)
		return NIL;

	foreach(l, rlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (IsTidRangeClause(rinfo, rel))
			rlst = lappend(rlst, rinfo);
	}

	return rlst;
}

/*
 * 给定涉及本 rel 的连接子句列表，为每一个合适的
 * TidEqual 子句创建一个参数化 TidPath。
 *
 * 原则上可以把引用相同外部关系的子句合并，
 * 但这种情形似乎不会频繁到值得
 * 费心处理。
 */
static void
BuildParameterizedTidPaths(PlannerInfo *root, RelOptInfo *rel, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		List	   *tidquals;
		Relids		required_outer;

		/*
		 * 验证每个子句是否真正可用；必须检查这一点，
		 * 即使正在检查由 EquivalenceClass 生成的子句，
		 * 因为它们也许不满足另一侧没有本 rel 的 Var
		 * 这一限制，或者有人也许建了一个操作符
		 * 类，它接受 "tid" 类型但还含有其他操作符。
		 *
		 * 目前只考虑 TidEqual 连接子句。原则上我们
		 * 也许能在该 rel 的 joininfo 列表里找到合适的 ScalarArrayOpExpr，
		 * 但为此耗费周期似乎不值得。
		 * 而且这里肯定找不到 CurrentOfExpr。因此我们不
		 * 使用 TidQualFromRestrictInfo；但本逻辑必须与该函数
		 * 保持一致。
		 */
		if (rinfo->pseudoconstant ||
			!restriction_is_securely_promotable(rinfo, rel) ||
			!IsTidEqualClause(rinfo, rel))
			continue;

		/*
		 * 检查子句能否移到这个 rel；考虑由 EC 导出的子句时这多半
		 * 是多余的，但对 "松散" 连接子句
		 * 必须检查。
		 */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		/* 可以，为这条路径建立子句列表 */
		tidquals = list_make1(rinfo);

		/* 计算这条路径所需的外部关系 */
		required_outer = bms_union(rinfo->required_relids, rel->lateral_relids);
		required_outer = bms_del_member(required_outer, rel->relid);

		add_path(rel, (Path *) create_tidscan_path(root, rel, tidquals,
												   required_outer));
	}
}

/*
 * 测试 EquivalenceClass 成员是否匹配本 rel 的 CTID Var。
 *
 * 这是供 generate_implied_equalities_for_column 使用的回调。
 */
static bool
ec_member_matches_ctid(PlannerInfo *root, RelOptInfo *rel,
					   EquivalenceClass *ec, EquivalenceMember *em,
					   void *arg)
{
	if (em->em_expr && IsA(em->em_expr, Var) &&
		IsCTIDVar((Var *) em->em_expr, rel))
		return true;
	return false;
}

/*
 * create_tidscan_paths
 *	  为给定 rel 的直接 TID 扫描创建对应路径。
 *
 *	  候选路径加入该 rel 的 pathlist（使用 add_path）。
 */
void
create_tidscan_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tidquals;
	List	   *tidrangequals;

	/*
	 * 若该 rel 的 baserestrict 列表中有合适的限定条件，用它们生成一条
	 * 普通（非参数化）的 TidPath。
	 */
	tidquals = TidQualFromRestrictInfoList(root, rel->baserestrictinfo, rel);

	if (tidquals != NIL)
	{
		/*
		 * 这条路径不用连接子句，但因其 tlist 中的 LATERAL 引用，
		 * 仍可能需要参数化。
		 */
		Relids		required_outer = rel->lateral_relids;

		add_path(rel, (Path *) create_tidscan_path(root, rel, tidquals,
												   required_outer));
	}

	/*
	 * 若 baserestrict 列表中有范围限定条件，生成一条
	 * TidRangePath。
	 */
	tidrangequals = TidRangeQualFromRestrictInfoList(rel->baserestrictinfo,
													 rel);

	if (tidrangequals != NIL)
	{
		/*
		 * 这条路径不用连接子句，但因其 tlist 中的 LATERAL 引用，
		 * 仍可能需要参数化。
		 */
		Relids		required_outer = rel->lateral_relids;

		add_path(rel, (Path *) create_tidrangescan_path(root, rel,
														tidrangequals,
														required_outer));
	}

	/*
	 * 尝试用抽出的等值子句生成参数化 TidPaths，这些子句来自
	 * EquivalenceClasses。（这很重要，因为简单的 "t1.ctid =
	 * t2.ctid" 子句会变成 EC。）
	 */
	if (rel->has_eclass_joins)
	{
		List	   *clauses;

		/* 生成子句，跳过任何连接到 lateral_referencers 的子句 */
		clauses = generate_implied_equalities_for_column(root,
														 rel,
														 ec_member_matches_ctid,
														 NULL,
														 rel->lateral_referencers);

		/* 为每个可用的连接子句生成一条路径 */
		BuildParameterizedTidPaths(root, rel, clauses);
	}

	/*
	 * 也考虑使用 "松散" 连接限定条件的参数化 TidPaths。形如
	 * "t1.ctid = t2.ctid" 的限定条件若是外
	 * 连接限定条件，就会变成这些，例如。
	 */
	BuildParameterizedTidPaths(root, rel, rel->joininfo);
}
