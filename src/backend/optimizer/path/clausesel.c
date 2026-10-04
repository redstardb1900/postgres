/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute clause selectivities
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/clausesel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "statistics/statistics.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

/*
 * 用于累积可能的范围查询
 * 子句对信息的数据结构，供 clauselist_selectivity 使用。
 */
typedef struct RangeQueryClause
{
	struct RangeQueryClause *next;	/* 链表中的下一项 */
	Node	   *var;			/* 这些子句的公共变量 */
	bool		have_lobound;	/* 是否已找到下界子句？ */
	bool		have_hibound;	/* 是否已找到上界子句？ */
	Selectivity lobound;		/* var > 某值这种子句的选择率 */
	Selectivity hibound;		/* var < 某值这种子句的选择率 */
} RangeQueryClause;

static void addRangeClause(RangeQueryClause **rqlist, Node *clause,
						   bool varonleft, bool isLTsel, Selectivity s2);
static RelOptInfo *find_single_rel_for_clauses(PlannerInfo *root,
											   List *clauses);
static Selectivity clauselist_selectivity_or(PlannerInfo *root,
											 List *clauses,
											 int varRelid,
											 JoinType jointype,
											 SpecialJoinInfo *sjinfo,
											 bool use_extended_stats);

/****************************************************************************
 *		ROUTINES TO COMPUTE SELECTIVITIES（计算选择率的例程）
 ****************************************************************************/

/*
 * clauselist_selectivity -
 *	  计算隐含 ANDed 的布尔
 *	  表达式子句列表的选择率。列表可以为空，此时 1.0
 *	  必须被返回。列表元素可以是 RestrictInfos
 *	  或裸表达式子句---优先前者，因为
 *	  它可以缓存结果。
 *
 * 其余参数的含义见 clause_selectivity()。
 *
 * 基本做法是先把扩展统计用在尽可能多的
 * 子句上，以便捕捉跨列依赖等。
 * 然后对其余子句取它们的选择率
 * 乘积来估计，但这仅当它们具有独立
 * 概率时才正确，而实际上它们常常并不独立，即使
 * 只引用单列。因此能更聪明时我们就更聪明。
 *
 * 我们也识别 "范围查询"，例如 "x > 34 AND x < 42"。若子句
 * 被认作可能的范围查询成分，条件是它们是限制
 * 操作子句，且其操作符把 scalarltsel 或相关函数当作
 * 限制选择率估计器。我们把这种形式且
 * 引用同一变量的子句配对。无法配对的这类子句就简单地
 * 按通常方式乘进选择率乘积。但当我们
 * 找到一对时，就知道这些选择率表示下界与上界在列范围内的相对
 * 位置，因此不必
 * 把选择率算成 hisel * losel，而可以算成 hisel +
 * losel - 1。（直观地看，hisel 是范围中
 * 低于上界的那一部分，而 losel 是高于下界的那一部分；因此
 * hisel 可以直接解释为 0..1 的值，但需要先把 losel 换成
 * 1-losel 再把它解释成一个值。于是可用
 * 范围是从 1-losel 到 hisel。不过这个计算把空值
 * 排除了两次，所以实际上需要 hisel + losel + null_frac - 1。）
 *
 * 若任一选择率恰好是 DEFAULT_INEQ_SEL，就放弃这个等式
 * 而改用 DEFAULT_RANGE_INEQ_SEL。若等式
 * 得出不可能的（负的）结果，也同样处理。
 *
 * 一个附带的好处是可以识别冗余不等式，例如
 * "x < 4 AND x < 5"；只会计入更紧的约束。
 *
 * 当然这一切都非常依赖于不等式
 * 选择率函数的行为；也许将来可以推广这种方法。
 */
Selectivity
clauselist_selectivity(PlannerInfo *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo)
{
	return clauselist_selectivity_ext(root, clauses, varRelid,
									  jointype, sjinfo, true);
}

/*
 * clauselist_selectivity_ext -
 *	  clauselist_selectivity() 的扩展版本。若 "use_extended_stats"
 *	  为 false，则忽略全部扩展统计，只用按列
 *	  统计。
 */
Selectivity
clauselist_selectivity_ext(PlannerInfo *root,
						   List *clauses,
						   int varRelid,
						   JoinType jointype,
						   SpecialJoinInfo *sjinfo,
						   bool use_extended_stats)
{
	Selectivity s1 = 1.0;
	RelOptInfo *rel;
	Bitmapset  *estimatedclauses = NULL;
	RangeQueryClause *rqlist = NULL;
	ListCell   *l;
	int			listidx;

	/*
	 * 若恰好只有一个子句，就直接进入
	 * clause_selectivity_ext()。下面可能做的事都与此无关。
	 */
	if (list_length(clauses) == 1)
		return clause_selectivity_ext(root, (Node *) linitial(clauses),
									  varRelid, jointype, sjinfo,
									  use_extended_stats);

	/*
	 * 判断这些子句是否只引用单一关系。若是，并且
	 * 它有扩展统计，就尝试应用那些统计。
	 */
	rel = find_single_rel_for_clauses(root, clauses);
	if (use_extended_stats && rel && rel->rtekind == RTE_RELATION && rel->statlist != NIL)
	{
		/*
		 * 用扩展统计估计尽可能多的子句。
		 *
		 * 'estimatedclauses' 会被填上在这里估计过的子句的、从 0 起的列表位置
		 * 下标，下面应当忽略它们。
		 */
		s1 = statext_clauselist_selectivity(root, clauses, varRelid,
											jointype, sjinfo, rel,
											&estimatedclauses, false);
	}

	/*
	 * 对其余子句应用普通选择率估计。我们会
	 * 小心地跳过上面已经估计过的子句。
	 *
	 * 凡是不像潜在范围查询子句的，都
	 * 乘进 s1 然后忘掉。凡是像的，都插入
	 * 一个 rqlist 条目。
	 */
	listidx = -1;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		RestrictInfo *rinfo;
		Selectivity s2;

		listidx++;

		/*
		 * 若这个子句已由上面的其他
		 * 统计估计过，就跳过它。
		 */
		if (bms_is_member(listidx, estimatedclauses))
			continue;

		/* 单独计算这个子句的选择率 */
		s2 = clause_selectivity_ext(root, clause, varRelid, jointype, sjinfo,
									use_extended_stats);

		/*
		 * 检查传来的是不是 RestrictInfo。
		 *
		 * 若它是伪常量 RestrictInfo，则 s2 要么是 1.0 要么是
		 * 0.0；直接用这个值，不必再找范围对。
		 */
		if (IsA(clause, RestrictInfo))
		{
			rinfo = (RestrictInfo *) clause;
			if (rinfo->pseudoconstant)
			{
				s1 = s1 * s2;
				continue;
			}
			clause = (Node *) rinfo->clause;
		}
		else
			rinfo = NULL;

		/*
		 * 看它是否像一侧为伪常量的限制子句。
		 * （比这更复杂的东西也许不会按
		 * 我们期望的简单方式表现。）这里的多数测试有 rinfo 时
		 * 可以做得更高效。
		 */
		if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr	   *expr = (OpExpr *) clause;
			bool		varonleft = true;
			bool		ok;

			if (rinfo)
			{
				ok = (rinfo->num_base_rels == 1) &&
					(is_pseudo_constant_clause_relids(lsecond(expr->args),
													  rinfo->right_relids) ||
					 (varonleft = false,
					  is_pseudo_constant_clause_relids(linitial(expr->args),
													   rinfo->left_relids)));
			}
			else
			{
				ok = (NumRelids(root, clause) == 1) &&
					(is_pseudo_constant_clause(lsecond(expr->args)) ||
					 (varonleft = false,
					  is_pseudo_constant_clause(linitial(expr->args))));
			}

			if (ok)
			{
				/*
				 * 若它不是 "<"/"<="/">"/">=" 操作符，就把
				 * 选择率按通用方式并入。但若 oprrest 正确，
				 * 就把该子句加入 rqlist 供稍后处理。
				 */
				switch (get_oprrest(expr->opno))
				{
					case F_SCALARLTSEL:
					case F_SCALARLESEL:
						addRangeClause(&rqlist, clause,
									   varonleft, true, s2);
						break;
					case F_SCALARGTSEL:
					case F_SCALARGESEL:
						addRangeClause(&rqlist, clause,
									   varonleft, false, s2);
						break;
					default:
						/* 只按通用方式并入选择率 */
						s1 = s1 * s2;
						break;
				}
				continue;		/* 落到循环底部 */
			}
		}

		/* 形式不对，因此按通用方式处理。 */
		s1 = s1 * s2;
	}

	/*
	 * 现在扫描范围查询对的列表。
	 */
	while (rqlist != NULL)
	{
		RangeQueryClause *rqnext;

		if (rqlist->have_lobound && rqlist->have_hibound)
		{
			/* 成功配上一对范围子句 */
			Selectivity s2;

			/*
			 * 与默认值精确相等，多半意味着
			 * 选择率函数放弃了估计。这并不严密，但应当
			 * 足够好。
			 */
			if (rqlist->hibound == DEFAULT_INEQ_SEL ||
				rqlist->lobound == DEFAULT_INEQ_SEL)
			{
				s2 = DEFAULT_RANGE_INEQ_SEL;
			}
			else
			{
				s2 = rqlist->hibound + rqlist->lobound - 1.0;

				/* 对 NULLs 被排除两次做调整 */
				s2 += nulltestsel(root, IS_NULL, rqlist->var,
								  varRelid, jointype, sjinfo);

				/*
				 * 零或略为负的 s2 应当转换成一个
				 * 小的正值；我们多半是在处理非常
				 * 紧的范围，并因舍入误差得到了虚假结果。
				 * 不过，若 s2 负得很多，则多半是范围的
				 * 一侧或两侧用了默认选择率估计，
				 * 而上面由于某种原因没能识别出来。
				 */
				if (s2 <= 0.0)
				{
					if (s2 < -0.01)
					{
						/*
						 * 没有可用数据---使用一个较小、
						 * 但不是非常小的默认估计。
						 */
						s2 = DEFAULT_RANGE_INEQ_SEL;
					}
					else
					{
						/*
						 * 这只是舍入误差；使用一个小的正
						 * 值
						 */
						s2 = 1.0e-10;
					}
				}
			}
			/* 并入这一对子句的选择率 */
			s1 *= s2;
		}
		else
		{
			/* 只找到一对中的一个，按通用方式并入 */
			if (rqlist->have_lobound)
				s1 *= rqlist->lobound;
			else
				s1 *= rqlist->hibound;
		}
		/* 释放存储并前进 */
		rqnext = rqlist->next;
		pfree(rqlist);
		rqlist = rqnext;
	}

	return s1;
}

/*
 * clauselist_selectivity_or -
 *	  计算隐含 ORed 的布尔
 *	  表达式子句列表的选择率。列表可以为空，此时 0.0
 *	  必须被返回。列表元素可以是 RestrictInfos
 *	  或裸表达式子句---优先前者，因为
 *	  它可以缓存结果。
 *
 * 其余参数的含义见 clause_selectivity()。
 *
 * 基本做法是先把扩展统计用在尽可能多的
 * 子句上，以便捕捉跨列依赖等。
 * 然后将其余子句当作相互独立来估计。
 */
static Selectivity
clauselist_selectivity_or(PlannerInfo *root,
						  List *clauses,
						  int varRelid,
						  JoinType jointype,
						  SpecialJoinInfo *sjinfo,
						  bool use_extended_stats)
{
	Selectivity s1 = 0.0;
	RelOptInfo *rel;
	Bitmapset  *estimatedclauses = NULL;
	ListCell   *lc;
	int			listidx;

	/*
	 * 判断这些子句是否只引用单一关系。若是，并且
	 * 它有扩展统计，就尝试应用那些统计。
	 */
	rel = find_single_rel_for_clauses(root, clauses);
	if (use_extended_stats && rel && rel->rtekind == RTE_RELATION && rel->statlist != NIL)
	{
		/*
		 * 用扩展统计估计尽可能多的子句。
		 *
		 * 'estimatedclauses' 会被填上在这里估计过的子句的、从 0 起的列表位置
		 * 下标，下面应当忽略它们。
		 */
		s1 = statext_clauselist_selectivity(root, clauses, varRelid,
											jointype, sjinfo, rel,
											&estimatedclauses, true);
	}

	/*
	 * 将其余子句当作相互独立来估计。
	 *
	 * OR 子句的选择率按 s1+s2 - s1*s2 计算，以计入
	 * 被选中元组集合可能的重叠。
	 *
	 * XXX 这样是不是过于保守？
	 */
	listidx = -1;
	foreach(lc, clauses)
	{
		Selectivity s2;

		listidx++;

		/*
		 * 若这个子句已由上面的其他
		 * 统计估计过，就跳过它。
		 */
		if (bms_is_member(listidx, estimatedclauses))
			continue;

		s2 = clause_selectivity_ext(root, (Node *) lfirst(lc), varRelid,
									jointype, sjinfo, use_extended_stats);

		s1 = s1 + s2 - s1 * s2;
	}

	return s1;
}

/*
 * addRangeClause --- 为 clauselist_selectivity 添加一条新的范围子句
 *
 * 这里尝试把范围查询子句配对
 */
static void
addRangeClause(RangeQueryClause **rqlist, Node *clause,
			   bool varonleft, bool isLTsel, Selectivity s2)
{
	RangeQueryClause *rqelem;
	Node	   *var;
	bool		is_lobound;

	if (varonleft)
	{
		var = get_leftop((Expr *) clause);
		is_lobound = !isLTsel;	/* x < 某值 是上界 */
	}
	else
	{
		var = get_rightop((Expr *) clause);
		is_lobound = isLTsel;	/* 某值 < x 是下界 */
	}

	for (rqelem = *rqlist; rqelem; rqelem = rqelem->next)
	{
		/*
		 * 这里使用完整的 equal()，因为 "var" 也许是同一关系的
		 * 一个或多个属性的函数……
		 */
		if (!equal(var, rqelem->var))
			continue;
		/* 找到了安放这个子句的正确分组 */
		if (is_lobound)
		{
			if (!rqelem->have_lobound)
			{
				rqelem->have_lobound = true;
				rqelem->lobound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x < y AND x <= z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->lobound > s2)
					rqelem->lobound = s2;
			}
		}
		else
		{
			if (!rqelem->have_hibound)
			{
				rqelem->have_hibound = true;
				rqelem->hibound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x > y AND x >= z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->hibound > s2)
					rqelem->hibound = s2;
			}
		}
		return;
	}

	/* 没找到匹配的 var，因此新建一个子句对数据结构 */
	rqelem = (RangeQueryClause *) palloc(sizeof(RangeQueryClause));
	rqelem->var = var;
	if (is_lobound)
	{
		rqelem->have_lobound = true;
		rqelem->have_hibound = false;
		rqelem->lobound = s2;
	}
	else
	{
		rqelem->have_lobound = false;
		rqelem->have_hibound = true;
		rqelem->hibound = s2;
	}
	rqelem->next = *rqlist;
	*rqlist = rqelem;
}

/*
 * find_single_rel_for_clauses
 *		检查 'clauses' 中的每个子句，判断是否所有子句
 *		都只引用单一关系。若是则返回该关系，
 *		否则返回 NULL。
 */
static RelOptInfo *
find_single_rel_for_clauses(PlannerInfo *root, List *clauses)
{
	int			lastrelid = 0;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		int			relid;

		/*
		 * 若拿到的是裸子句列表而不是 RestrictInfos，我们
		 * 可以用 pull_varnos() 费力地抽出它们的 relids。
		 * 不过目前扩展统计机制对非 RestrictInfo 子句反正
		 * 什么也不会做，因此没有必要
		 * 多耗周期；若就是这种情况，直接失败即可。
		 *
		 * 例外是裸的 BoolExpr AND 子句。
		 * 我们把它当作特例，因为 restrictinfo 机制
		 * 不会在 AND 子句之上建立 RestrictInfos。
		 */
		if (is_andclause(rinfo))
		{
			RelOptInfo *rel;

			rel = find_single_rel_for_clauses(root,
											  ((BoolExpr *) rinfo)->args);

			if (rel == NULL)
				return NULL;
			if (lastrelid == 0)
				lastrelid = rel->relid;
			else if (rel->relid != lastrelid)
				return NULL;

			continue;
		}

		if (!IsA(rinfo, RestrictInfo))
			return NULL;

		if (bms_is_empty(rinfo->clause_relids))
			continue;			/* 可以忽略不含变量的子句 */
		if (!bms_get_singleton_member(rinfo->clause_relids, &relid))
			return NULL;		/* 这个子句里有多个关系 */
		if (lastrelid == 0)
			lastrelid = relid;	/* 第一个引用某关系的子句 */
		else if (relid != lastrelid)
			return NULL;		/* 关系与上一个不同 */
	}

	if (lastrelid != 0)
		return find_base_rel(root, lastrelid);

	return NULL;				/* 没有子句 */
}

/*
 * treat_as_join_clause -
 *	  决定一个操作符子句该由
 *	  限制估计器还是连接估计器处理。clause_selectivity() 的子例程。
 */
static inline bool
treat_as_join_clause(PlannerInfo *root, Node *clause, RestrictInfo *rinfo,
					 int varRelid, SpecialJoinInfo *sjinfo)
{
	if (varRelid != 0)
	{
		/*
		 * 调用方强制使用限制模式（例如因为我们正在检查
		 * 内层索引扫描的限定条件）。
		 */
		return false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * 它必须是限制子句，因为它是在
		 * 扫描节点上求值的。
		 */
		return false;
	}
	else
	{
		/*
		 * 否则，若使用了多于一个基关系，它就是连接。
		 * 若传入了 rinfo，可以优化这个计算。
		 *
		 * XXX	既然知道该子句是在连接上求值的，它
		 * 要成为单关系子句，唯一的途径是被外
		 * 连接推迟。这里有意只计算基关系，不算那些
		 * 也许出现在 rinfo->clause_relids 中的外连接，以便把这类
		 * 情形交给限制限定条件估计器，而不是连接估计器。
		 * 最终应当注意注入空值的
		 * 可能性，但我们多半想在限制
		 * 估计器里做这件事，而不是开始把这类情形当成连接限定条件。
		 */
		if (rinfo)
			return (rinfo->num_base_rels > 1);
		else
			return (NumRelids(root, clause) > 1);
	}
}


/*
 * clause_selectivity -
 *	  计算一般布尔表达式子句的选择率。
 *
 * 子句可以是 RestrictInfo，也可以是普通表达式。若它是
 * RestrictInfo，我们就尝试缓存选择率以便复用，
 * 因此优先传入 RestrictInfos。
 *
 * varRelid 要么是 0，要么是范围表下标。
 *
 * 当 varRelid 不为 0 时，只有属于该关系的变量会在
 * 计算选择率时被考虑；其他变量当作取值未知的常量。
 * 这适合用来估计这样一条连接子句的选择率：它在扫描
 * 嵌套循环连接的内层关系时被当作限制子句---
 * 此时 varRelid 应当是该内层关系的 ID。
 * 内层关系就是正在被扫描的那一侧。
 *
 * 当 varRelid 为 0 时，所有变量都当作变量。这
 * 适合普通连接子句和限制子句。
 *
 * 若该子句是连接子句，jointype 是连接类型。传入 JOIN_INNER
 * 若该子句不是连接子句。
 *
 * 对非连接子句 sjinfo 为 NULL，否则它提供关于
 * 正在执行的连接的额外上下文信息。有一些
 * 特殊情形：
 *	1. 对于特殊（非 INNER）连接，sjinfo 总是
 *	   root->join_info_list 的成员。
 *	2. 对于 INNER 连接，sjinfo 只是一个临时结构体，并且只有
 *	   其中的 relids 和 jointype 字段可以信任。
 * jointype 有可能不同于 sjinfo->jointype。
 * 这表示我们正在考虑一种变体连接：要么
 * 左右侧对调，要么对某一输入做了唯一化。
 *
 * 注意：传入非零 varRelid 时，通常应当设置
 * jointype == JOIN_INNER 且 sjinfo == NULL，即使该子句其实是
 * 连接子句；因为我们并没有把它当作连接子句。
 */
Selectivity
clause_selectivity(PlannerInfo *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype,
				   SpecialJoinInfo *sjinfo)
{
	return clause_selectivity_ext(root, clause, varRelid,
								  jointype, sjinfo, true);
}

/*
 * clause_selectivity_ext -
 *	  clause_selectivity() 的扩展版本。若 "use_extended_stats" 为
 *	  false，则忽略全部扩展统计，只用按列
 *	  统计。
 */
Selectivity
clause_selectivity_ext(PlannerInfo *root,
					   Node *clause,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo,
					   bool use_extended_stats)
{
	Selectivity s1 = 0.5;		/* 任何未处理子句类型的默认值 */
	RestrictInfo *rinfo = NULL;
	bool		cacheable = false;

	if (clause == NULL)			/* 这还会发生吗？ */
		return s1;

	if (IsA(clause, RestrictInfo))
	{
		rinfo = (RestrictInfo *) clause;

		/*
		 * 若子句被标为伪常量，它会被用作
		 * 门控限定条件，不应影响选择率估计；因此
		 * 返回 1.0。唯一的例外是常量 FALSE 可以
		 * 视为选择率 0.0，因为它肯定意味着计划
		 * 不会产出任何行。这种情况足够简单，不必
		 * 费心缓存结果。
		 */
		if (rinfo->pseudoconstant)
		{
			if (!IsA(rinfo->clause, Const))
				return (Selectivity) 1.0;
		}

		/*
		 * 尽可能缓存该子句的选择率计算结果。
		 * 若 varRelid 为零，或子句
		 * 只含该 relid 的变量，就可以缓存---否则 varRelid 会影响
		 * 结果，因此不得缓存。外连接限定条件也许会以
		 * 其连接的实际 jointype 或 JOIN_INNER 来检查，因此需要
		 * 两个缓存变量来记住这两种情形。注意：我们假定
		 * 对调输入关系或
		 * 考虑唯一化情形时结果不会变，因此所有非 JOIN_INNER 情形
		 * 只需要一个缓存变量。
		 */
		if (varRelid == 0 ||
			rinfo->num_base_rels == 0 ||
			(rinfo->num_base_rels == 1 &&
			 bms_is_member(varRelid, rinfo->clause_relids)))
		{
			/* 可缓存---是否已经有结果？ */
			if (jointype == JOIN_INNER)
			{
				if (rinfo->norm_selec >= 0)
					return rinfo->norm_selec;
			}
			else
			{
				if (rinfo->outer_selec >= 0)
					return rinfo->outer_selec;
			}
			cacheable = true;
		}

		/*
		 * 继续检查所包含的子句。若该子句是
		 * OR 子句，我们要看带有子 RestrictInfos 的那个变体，
		 * 以便能缓存每个子子句的选择率。
		 */
		if (rinfo->orclause)
			clause = (Node *) rinfo->orclause;
		else
			clause = (Node *) rinfo->clause;
	}

	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/*
		 * 这里大概不应当见到上层 Var，但如果见到了，
		 * 就返回默认选择率……
		 */
		if (var->varlevelsup == 0 &&
			(varRelid == 0 || varRelid == (int) var->varno))
		{
			/* 对布尔 Var 使用限制选择率函数 */
			s1 = boolvarsel(root, (Node *) var, varRelid);
		}
	}
	else if (IsA(clause, Const))
	{
		/* 布尔常量相当简单…… */
		Const	   *con = (Const *) clause;

		s1 = con->constisnull ? 0.0 :
			DatumGetBool(con->constvalue) ? 1.0 : 0.0;
	}
	else if (IsA(clause, Param))
	{
		/* 看看能否替换这个 Param */
		Node	   *subst = estimate_expression_value(root, clause);

		if (IsA(subst, Const))
		{
			/* 布尔常量相当简单…… */
			Const	   *con = (Const *) subst;

			s1 = con->constisnull ? 0.0 :
				DatumGetBool(con->constvalue) ? 1.0 : 0.0;
		}
		else
		{
			/* XXX 有没有办法比默认值更好？ */
		}
	}
	else if (is_notclause(clause))
	{
		/* 底层子句选择率的倒数 */
		s1 = 1.0 - clause_selectivity_ext(root,
										  (Node *) get_notclausearg((Expr *) clause),
										  varRelid,
										  jointype,
										  sjinfo,
										  use_extended_stats);
	}
	else if (is_andclause(clause))
	{
		/* 与 clauselist_selectivity() 共用代码 */
		s1 = clauselist_selectivity_ext(root,
										((BoolExpr *) clause)->args,
										varRelid,
										jointype,
										sjinfo,
										use_extended_stats);
	}
	else if (is_orclause(clause))
	{
		/*
		 * 几乎与 clauselist_selectivity 相同，只是
		 * 子句用 OR 连接。
		 */
		s1 = clauselist_selectivity_or(root,
									   ((BoolExpr *) clause)->args,
									   varRelid,
									   jointype,
									   sjinfo,
									   use_extended_stats);
	}
	else if (is_opclause(clause) || IsA(clause, DistinctExpr))
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Oid			opno = opclause->opno;

		if (treat_as_join_clause(root, clause, rinfo, varRelid, sjinfo))
		{
			/* 估计连接子句的选择率。 */
			s1 = join_selectivity(root, opno,
								  opclause->args,
								  opclause->inputcollid,
								  jointype,
								  sjinfo);
		}
		else
		{
			/* 估计限制子句的选择率。 */
			s1 = restriction_selectivity(root, opno,
										 opclause->args,
										 opclause->inputcollid,
										 varRelid);
		}

		/*
		 * DistinctExpr 的表示与 OpExpr 相同，但
		 * 所含操作符是 "=" 而不是 "<>"，因此必须把结果取反。
		 * 这种估计方法对空值的行为并不正确，
		 * 但总比什么都不做要好。
		 */
		if (IsA(clause, DistinctExpr))
			s1 = 1.0 - s1;
	}
	else if (is_funcclause(clause))
	{
		FuncExpr   *funcclause = (FuncExpr *) clause;

		/* 若有支持函数，试着从它得到估计 */
		s1 = function_selectivity(root,
								  funcclause->funcid,
								  funcclause->args,
								  funcclause->inputcollid,
								  treat_as_join_clause(root, clause, rinfo,
													   varRelid, sjinfo),
								  varRelid,
								  jointype,
								  sjinfo);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* 使用特定于节点的选择率计算函数 */
		s1 = scalararraysel(root,
							(ScalarArrayOpExpr *) clause,
							treat_as_join_clause(root, clause, rinfo,
												 varRelid, sjinfo),
							varRelid,
							jointype,
							sjinfo);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		/* 使用特定于节点的选择率计算函数 */
		s1 = rowcomparesel(root,
						   (RowCompareExpr *) clause,
						   varRelid,
						   jointype,
						   sjinfo);
	}
	else if (IsA(clause, NullTest))
	{
		/* 使用特定于节点的选择率计算函数 */
		s1 = nulltestsel(root,
						 ((NullTest *) clause)->nulltesttype,
						 (Node *) ((NullTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, BooleanTest))
	{
		/* 使用特定于节点的选择率计算函数 */
		s1 = booltestsel(root,
						 ((BooleanTest *) clause)->booltesttype,
						 (Node *) ((BooleanTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, CurrentOfExpr))
	{
		/* CURRENT OF 至多选出其表中的一行 */
		CurrentOfExpr *cexpr = (CurrentOfExpr *) clause;
		RelOptInfo *crel = find_base_rel(root, cexpr->cvarno);

		if (crel->tuples > 0)
			s1 = 1.0 / crel->tuples;
	}
	else if (IsA(clause, RelabelType))
	{
		/* 不确定是否需要这种情况，但没有坏处 */
		s1 = clause_selectivity_ext(root,
									(Node *) ((RelabelType *) clause)->arg,
									varRelid,
									jointype,
									sjinfo,
									use_extended_stats);
	}
	else if (IsA(clause, CoerceToDomain))
	{
		/* 不确定是否需要这种情况，但没有坏处 */
		s1 = clause_selectivity_ext(root,
									(Node *) ((CoerceToDomain *) clause)->arg,
									varRelid,
									jointype,
									sjinfo,
									use_extended_stats);
	}
	else
	{
		/*
		 * 对于其他任何东西，看看能否把它当作布尔变量。
		 * 这仅当它是单一
		 * 关系中各 Var 上的不可变表达式时才成立；但这里不必由我们检查，因为
		 * boolvarsel() 会在内部做这件事，若不成立就返回一个合适的默认
		 * 选择率。
		 */
		s1 = boolvarsel(root, clause, varRelid);
	}

	/* 若可能则缓存结果 */
	if (cacheable)
	{
		if (jointype == JOIN_INNER)
			rinfo->norm_selec = s1;
		else
			rinfo->outer_selec = s1;
	}

#ifdef SELECTIVITY_DEBUG
	elog(DEBUG4, "clause_selectivity: s1 %f", s1);
#endif							/* 若 SELECTIVITY_DEBUG */

	return s1;
}
