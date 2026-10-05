/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  操作目标列表的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/tlist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"


/*
 * 测试表达式节点是否表示一次 SRF 调用。当心多次求值！
 *
 * 请注意这只打算用在 split_pathtarget_at_srfs() 里；
 * 若在别处使用，对嵌在表达式里的 SRFs 你的代码几乎一定是错的。
 * 请改用 expression_returns_set()。
 */
#define IS_SRF_CALL(node) \
	((IsA(node, FuncExpr) && ((FuncExpr *) (node))->funcretset) || \
	 (IsA(node, OpExpr) && ((OpExpr *) (node))->opretset))

/*
 * split_pathtarget_at_srfs() 用的数据结构。为了在 sortgroupref 项
 * 即使文本上 equal() 时仍保持其身份，我们跟踪的
 * 不只是裸表达式，而是表达式加上它们的 sortgroupref 下标。
 */
typedef struct
{
	Node	   *expr;			/* 某个 PathTarget 的子表达式 */
	Index		sortgroupref;	/* 它的 sortgroupref，没有则为 0 */
} split_pathtarget_item;

typedef struct
{
	/* 这是裸表达式的 List： */
	List	   *input_target_exprs; /* 可从输入取得的表达式 */
	/* 这些是 split_pathtarget_items 的 List 的 List： */
	List	   *level_srfs;		/* 每一层要求值的 SRF 表达式 */
	List	   *level_input_vars;	/* 每一层需要的输入 var */
	List	   *level_input_srfs;	/* 每一层需要的输入 SRFs */
	/* 这些是 split_pathtarget_items 的 List： */
	List	   *current_input_vars; /* 当前子表达式需要的 var */
	List	   *current_input_srfs; /* 当前子表达式需要的 SRFs */
	/* 当前 split_pathtarget_walker 遍历的辅助数据： */
	int			current_depth;	/* 当前子表达式中的最大 SRF 深度 */
	Index		current_sgref;	/* 当前子表达式的 sortgroupref，没有则为 0 */
} split_pathtarget_context;

static bool split_pathtarget_walker(Node *node,
									split_pathtarget_context *context);
static void add_sp_item_to_pathtarget(PathTarget *target,
									  split_pathtarget_item *item);
static void add_sp_items_to_pathtarget(PathTarget *target, List *items);


/*****************************************************************************
 *		目标列表的建立与查找工具
 *****************************************************************************/

/*
 * tlist_member
 *	  找出给定目标列表中表达式与给定表达式
 *	  equal() 的（第一个）成员。若没有这样的成员则结果为 NULL。
 */
TargetEntry *
tlist_member(Expr *node, List *targetlist)
{
	ListCell   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);

		if (equal(node, tlentry->expr))
			return tlentry;
	}
	return NULL;
}

/*
 * tlist_member_match_var
 *	  与上面相同，只是我们只按
 *	  varno/varattno/varlevelsup/vartype 匹配给出的 Var，而不是完整的 equal()。
 *
 * 某些情况下我们不能确定 typmod 精确匹配，就需要这样做。
 * 不过为安全起见，我们坚持 vartype 必须匹配。
 */
static TargetEntry *
tlist_member_match_var(Var *var, List *targetlist)
{
	ListCell   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
		Var		   *tlvar = (Var *) tlentry->expr;

		if (!tlvar || !IsA(tlvar, Var))
			continue;
		if (var->varno == tlvar->varno &&
			var->varattno == tlvar->varattno &&
			var->varlevelsup == tlvar->varlevelsup &&
			var->vartype == tlvar->vartype)
			return tlentry;
	}
	return NULL;
}

/*
 * add_to_flat_tlist
 *		往展平的目标列表里再加一些项（若它们还不在里面）
 *
 * tlist 是展平后的目标列表
 * exprs 是表达式列表（通常是 Var，但不必一定是）
 *
 * 返回扩展后的目标列表。
 */
List *
add_to_flat_tlist(List *tlist, List *exprs)
{
	int			next_resno = list_length(tlist) + 1;
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (!tlist_member(expr, tlist))
		{
			TargetEntry *tle;

			tle = makeTargetEntry(copyObject(expr), /* 需要复制吗？ */
								  next_resno++,
								  NULL,
								  false);
			tlist = lappend(tlist, tle);
		}
	}
	return tlist;
}


/*
 * get_tlist_exprs
 *		只取目标列表的表达式子树
 *
 * 除非 includeJunk 为真，否则忽略废列
 */
List *
get_tlist_exprs(List *tlist, bool includeJunk)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk && !includeJunk)
			continue;

		result = lappend(result, tle->expr);
	}
	return result;
}


/*
 * count_nonjunk_tlist_entries
 *		顾名思义……
 */
int
count_nonjunk_tlist_entries(List *tlist)
{
	int			len = 0;
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (!tle->resjunk)
			len++;
	}
	return len;
}


/*
 * tlist_same_exprs
 *		检查两个目标列表是否含有相同的表达式
 *
 * 注意：本函数用来决定能否安全地把新目标列表
 * 塞进不具备投影能力的计划节点。显然，除非
 * 该节点的目标列表显示它已经返回我们想要的列值，否则不能这么做。
 * 不过我们可以忽略 TargetEntry 属性 resname、ressortgroupref、
 * resorigtbl、resorigcol 和 resjunk，因为那些只是不影响
 * 该节点算出的行值的标注。（而且，若我们不忽略它们，
 * 就经常做不出想要的优化，因为规划器往往不费心
 * 让中间计划节点里的 resname 等保持有效。）注意成功时，
 * 调用者仍须把想要的目标列表塞进计划节点，
 * 否则它不会有想要的标注成员。
 */
bool
tlist_same_exprs(List *tlist1, List *tlist2)
{
	ListCell   *lc1,
			   *lc2;

	if (list_length(tlist1) != list_length(tlist2))
		return false;			/* 长度不同，因此无法匹配 */

	forboth(lc1, tlist1, lc2, tlist2)
	{
		TargetEntry *tle1 = (TargetEntry *) lfirst(lc1);
		TargetEntry *tle2 = (TargetEntry *) lfirst(lc2);

		if (!equal(tle1->expr, tle2->expr))
			return false;
	}

	return true;
}


/*
 * 目标列表的输出数据类型是否与 colTypes 中列出的相同？
 *
 * 若 junkOK 为真则忽略废列；否则只要出现
 * 废列就一律得到假结果。
 *
 * 注意：目前没有调用者关心 typmod 的比较。
 */
bool
tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK)
{
	ListCell   *l;
	ListCell   *curColType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColType == NULL)
				return false;	/* 目标列表比 colTypes 长 */
			if (exprType((Node *) tle->expr) != lfirst_oid(curColType))
				return false;
			curColType = lnext(colTypes, curColType);
		}
	}
	if (curColType != NULL)
		return false;			/* 目标列表比 colTypes 短 */
	return true;
}

/*
 * 目标列表暴露的排序规则是否与 colCollations 中列出的相同？
 *
 * 逻辑与上面相同，只是针对排序规则。
 */
bool
tlist_same_collations(List *tlist, List *colCollations, bool junkOK)
{
	ListCell   *l;
	ListCell   *curColColl = list_head(colCollations);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColColl == NULL)
				return false;	/* 目标列表比 colCollations 长 */
			if (exprCollation((Node *) tle->expr) != lfirst_oid(curColColl))
				return false;
			curColColl = lnext(colCollations, curColColl);
		}
	}
	if (curColColl != NULL)
		return false;			/* 目标列表比 colCollations 短 */
	return true;
}

/*
 * apply_tlist_labeling
 *		把 src_tlist 的 TargetEntry 标注属性应用到 dest_tlist
 *
 * 这用来把列名等重新挂到计划最终输出的
 * 目标列表上。
 */
void
apply_tlist_labeling(List *dest_tlist, List *src_tlist)
{
	ListCell   *ld,
			   *ls;

	Assert(list_length(dest_tlist) == list_length(src_tlist));
	forboth(ld, dest_tlist, ls, src_tlist)
	{
		TargetEntry *dest_tle = (TargetEntry *) lfirst(ld);
		TargetEntry *src_tle = (TargetEntry *) lfirst(ls);

		Assert(dest_tle->resno == src_tle->resno);
		dest_tle->resname = src_tle->resname;
		dest_tle->ressortgroupref = src_tle->ressortgroupref;
		dest_tle->resorigtbl = src_tle->resorigtbl;
		dest_tle->resorigcol = src_tle->resorigcol;
		dest_tle->resjunk = src_tle->resjunk;
	}
}


/*
 * get_sortgroupref_tle
 *		找出与给定 SortGroupRef 下标匹配的目标列表项，
 *		并返回它。
 */
TargetEntry *
get_sortgroupref_tle(Index sortref, List *targetList)
{
	ListCell   *l;

	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->ressortgroupref == sortref)
			return tle;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
	return NULL;				/* 消除编译器告警 */
}

/*
 * get_sortgroupclause_tle
 *		按 ressortgroupref 找出与给定 SortGroupClause
 *		匹配的目标列表项，并返回它。
 */
TargetEntry *
get_sortgroupclause_tle(SortGroupClause *sgClause,
						List *targetList)
{
	return get_sortgroupref_tle(sgClause->tleSortGroupRef, targetList);
}

/*
 * get_sortgroupclause_expr
 *		按 ressortgroupref 找出与给定 SortGroupClause
 *		匹配的目标列表项，并返回它的表达式。
 */
Node *
get_sortgroupclause_expr(SortGroupClause *sgClause, List *targetList)
{
	TargetEntry *tle = get_sortgroupclause_tle(sgClause, targetList);

	return (Node *) tle->expr;
}

/*
 * get_sortgrouplist_exprs
 *		给定 SortGroupClauses 的列表，建立
 *		被引用的目标列表表达式的列表。
 */
List *
get_sortgrouplist_exprs(List *sgClauses, List *targetList)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, sgClauses)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		Node	   *sortexpr;

		sortexpr = get_sortgroupclause_expr(sortcl, targetList);
		result = lappend(result, sortexpr);
	}
	return result;
}


/*****************************************************************************
 *		从 SortGroupClauses 列表中抽出数据的函数
 *
 * 它们其实不属于 tlist.c，但和紧上面的
 * 函数多少有关，又不值得单独占一个文件。
 *****************************************************************************/

/*
 * get_sortgroupref_clause
 *		找出与给定 SortGroupRef 下标匹配的 SortGroupClause，
 *		并返回它。
 */
SortGroupClause *
get_sortgroupref_clause(Index sortref, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		SortGroupClause *cl = (SortGroupClause *) lfirst(l);

		if (cl->tleSortGroupRef == sortref)
			return cl;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in list");
	return NULL;				/* 消除编译器告警 */
}

/*
 * get_sortgroupref_clause_noerr
 *		与上面相同，但找不到时返回 NULL 而不是报错。
 */
SortGroupClause *
get_sortgroupref_clause_noerr(Index sortref, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		SortGroupClause *cl = (SortGroupClause *) lfirst(l);

		if (cl->tleSortGroupRef == sortref)
			return cl;
	}

	return NULL;
}

/*
 * extract_grouping_ops：为 SortGroupClause 列表
 *		做一个等值操作符 OIDs 的数组
 */
Oid *
extract_grouping_ops(List *groupClause)
{
	int			numCols = list_length(groupClause);
	int			colno = 0;
	Oid		   *groupOperators;
	ListCell   *glitem;

	groupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		groupOperators[colno] = groupcl->eqop;
		Assert(OidIsValid(groupOperators[colno]));
		colno++;
	}

	return groupOperators;
}

/*
 * extract_grouping_collations：为 SortGroupClause 列表
 *		做一个分组列排序规则的数组
 */
Oid *
extract_grouping_collations(List *groupClause, List *tlist)
{
	int			numCols = list_length(groupClause);
	int			colno = 0;
	Oid		   *grpCollations;
	ListCell   *glitem;

	grpCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);
		TargetEntry *tle = get_sortgroupclause_tle(groupcl, tlist);

		grpCollations[colno++] = exprCollation((Node *) tle->expr);
	}

	return grpCollations;
}

/*
 * extract_grouping_cols：为 SortGroupClause 列表
 *		做一个分组列 resno 的数组
 */
AttrNumber *
extract_grouping_cols(List *groupClause, List *tlist)
{
	AttrNumber *grpColIdx;
	int			numCols = list_length(groupClause);
	int			colno = 0;
	ListCell   *glitem;

	grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);
		TargetEntry *tle = get_sortgroupclause_tle(groupcl, tlist);

		grpColIdx[colno++] = tle->resno;
	}

	return grpColIdx;
}

/*
 * grouping_is_sortable：能否用排序来实现分组列表？
 *
 * 这很容易，因为若存在 sortop，解析器就会把它放进来。
 */
bool
grouping_is_sortable(List *groupClause)
{
	ListCell   *glitem;

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		if (!OidIsValid(groupcl->sortop))
			return false;
	}
	return true;
}

/*
 * grouping_is_hashable：能否用散列来实现分组列表？
 *
 * 我们依赖解析器已正确设置可散列标志。
 */
bool
grouping_is_hashable(List *groupClause)
{
	ListCell   *glitem;

	foreach(glitem, groupClause)
	{
		SortGroupClause *groupcl = (SortGroupClause *) lfirst(glitem);

		if (!groupcl->hashable)
			return false;
	}
	return true;
}


/*****************************************************************************
 *		PathTarget 操作函数
 *
 * PathTarget 是完整目标列表的某种精简版；它
 * 省去了除（可选的）sortgroupref 数据以外的全部 TargetEntry 装饰，
 * 并加上求值代价和输出数据宽度信息。
 *****************************************************************************/

/*
 * make_pathtarget_from_tlist
 *	  构造与给定目标列表等价的 PathTarget。
 *
 * 这把代价和宽度成员留为零。多数调用者会想
 * 改用 create_pathtarget()，以便把它们设上。
 */
PathTarget *
make_pathtarget_from_tlist(List *tlist)
{
	PathTarget *target = makeNode(PathTarget);
	int			i;
	ListCell   *lc;

	target->sortgrouprefs = (Index *) palloc(list_length(tlist) * sizeof(Index));

	i = 0;
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		target->exprs = lappend(target->exprs, tle->expr);
		target->sortgrouprefs[i] = tle->ressortgroupref;
		i++;
	}

	/*
	 * 把易变性标成未知。contain_volatile_functions 函数
	 * 会在第一次带着这个 PathTarget 被调用时，
	 * 判断是否有任何易变函数。
	 */
	target->has_volatile_expr = VOLATILITY_UNKNOWN;

	return target;
}

/*
 * make_tlist_from_pathtarget
 *	  从 PathTarget 构造目标列表。
 */
List *
make_tlist_from_pathtarget(PathTarget *target)
{
	List	   *tlist = NIL;
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  i + 1,
							  NULL,
							  false);
		if (target->sortgrouprefs)
			tle->ressortgroupref = target->sortgrouprefs[i];
		tlist = lappend(tlist, tle);
		i++;
	}

	return tlist;
}

/*
 * copy_pathtarget
 *	  复制一个 PathTarget。
 *
 * 新 PathTarget 有自己的 exprs List，但与旧的
 * 共享底层目标表达式树。
 */
PathTarget *
copy_pathtarget(PathTarget *src)
{
	PathTarget *dst = makeNode(PathTarget);

	/* 复制标量成员 */
	memcpy(dst, src, sizeof(PathTarget));
	/* 浅复制表达式列表 */
	dst->exprs = list_copy(src->exprs);
	/* 若有 sortgrouprefs 则复制一份（若没有，memcpy 已经处理过） */
	if (src->sortgrouprefs)
	{
		Size		nbytes = list_length(src->exprs) * sizeof(Index);

		dst->sortgrouprefs = (Index *) palloc(nbytes);
		memcpy(dst->sortgrouprefs, src->sortgrouprefs, nbytes);
	}
	return dst;
}

/*
 * create_empty_pathtarget
 *	  建立一个空的（零列、零代价）PathTarget。
 */
PathTarget *
create_empty_pathtarget(void)
{
	/* 这很容易，但我们不想让调用者把这一点写死…… */
	return makeNode(PathTarget);
}

/*
 * add_column_to_pathtarget
 *		往 PathTarget 追加一个目标列。
 *
 * 与 make_pathtarget_from_tlist 一样，代价和宽度
 * 成员留给调用者更新。
 */
void
add_column_to_pathtarget(PathTarget *target, Expr *expr, Index sortgroupref)
{
	/* 更新 exprs 列表很容易…… */
	target->exprs = lappend(target->exprs, expr);
	/* ……sortgroupref 数据就不那么容易了 */
	if (target->sortgrouprefs)
	{
		int			nexprs = list_length(target->exprs);

		/* 这看起来也许低效，其实通常很便宜 */
		target->sortgrouprefs = (Index *)
			repalloc(target->sortgrouprefs, nexprs * sizeof(Index));
		target->sortgrouprefs[nexprs - 1] = sortgroupref;
	}
	else if (sortgroupref)
	{
		/* 给先前没有标注的目标加上 sortgroupref 标注 */
		int			nexprs = list_length(target->exprs);

		target->sortgrouprefs = (Index *) palloc0(nexprs * sizeof(Index));
		target->sortgrouprefs[nexprs - 1] = sortgroupref;
	}

	/*
	 * 把 has_volatile_expr 重置为 UNKNOWN。我们把它留给
	 * contain_volatile_functions 再次正确设置。严格说我们
	 * 可以在这里省点事，只检查新的 Expr，但把设置
	 * 该标志的逻辑留在一处，而不是
	 * 在这里复制一份，似乎更好。
	 */
	if (target->has_volatile_expr == VOLATILITY_NOVOLATILE)
		target->has_volatile_expr = VOLATILITY_UNKNOWN;
}

/*
 * add_new_column_to_pathtarget
 *		往 PathTarget 追加一个目标列，但仅当它不
 *		equal() 于任何已有目标表达式时。
 *
 * 调用者无法指定 sortgroupref，因为不清楚如何
 * 把它和已有的列合并。
 *
 * 与 make_pathtarget_from_tlist 一样，代价和宽度
 * 成员留给调用者更新。
 */
void
add_new_column_to_pathtarget(PathTarget *target, Expr *expr)
{
	if (!list_member(target->exprs, expr))
		add_column_to_pathtarget(target, expr, 0);
}

/*
 * add_new_columns_to_pathtarget
 *		对列表的每个元素应用 add_new_column_to_pathtarget()。
 */
void
add_new_columns_to_pathtarget(PathTarget *target, List *exprs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		add_new_column_to_pathtarget(target, expr);
	}
}

/*
 * apply_pathtarget_labeling_to_tlist
 *		把 PathTarget 中的任何 sortgrouprefs 应用到匹配的目标列表项
 *
 * 这里我们不假定目标列表项与
 * PathTarget 一一对应。本函数的预期用途是处理
 * createplan.c 决定改用别的目标列表、而我们又得
 * 认出存在哪些匹配的情形。
 */
void
apply_pathtarget_labeling_to_tlist(List *tlist, PathTarget *target)
{
	int			i;
	ListCell   *lc;

	/* 若 PathTarget 没有 sortgrouprefs 数据，则无事可做 */
	if (target->sortgrouprefs == NULL)
		return;

	i = 0;
	foreach(lc, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		TargetEntry *tle;

		if (target->sortgrouprefs[i])
		{
			/*
			 * 对 Var，使用 tlist_member_match_var 放宽后的匹配规则；
			 * 这样我们能处理某些集合返回
			 * 函数已被内联的情形，于是比起原来的 Var
			 * 被创建时，我们现在更清楚
			 * 它返回什么。否则用普通的 equal() 找出匹配的
			 * TLE。（按当前用法，其实只需要 Var 这种情形；
			 * 但对非 Var 也有合理行为，似乎最好。）
			 */
			if (expr && IsA(expr, Var))
				tle = tlist_member_match_var((Var *) expr, tlist);
			else
				tle = tlist_member(expr, tlist);

			/*
			 * 若 sortgrouprefs 标注没有去处，或者我们不得不
			 * 给同一列标两次，就报错。（它已经有
			 * 想要的标注这种情形大概不会发生，但我们不妨
			 * 也允许。）
			 */
			if (!tle)
				elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
			if (tle->ressortgroupref != 0 &&
				tle->ressortgroupref != target->sortgrouprefs[i])
				elog(ERROR, "targetlist item has multiple sortgroupref labels");

			tle->ressortgroupref = target->sortgrouprefs[i];
		}
		i++;
	}
}

/*
 * split_pathtarget_at_srfs
 *		把给定的 PathTarget 拆成多层，以便安全地放置 SRFs
 *
 * 执行器只能处理出现在 ProjectSet 计划节点
 * 目标列表顶层的集合返回函数。若有任何 SRFs
 * 不在顶层，就需要把求值拆到多个
 * 计划层，使每一层都满足这一约束。本函数
 * 为每一层建立适当的 PathTarget。
 *
 * 例如考虑目标列表表达式
 *		x + srf1(srf2(y + z))
 * 这个表达式应当原样出现在顶层 PathTarget 中，但其下
 * 我们必须有一个含有下列内容的 PathTarget
 *		x, srf1(srf2(y + z))
 * 再往下，另一个含有下列内容的 PathTarget
 *		x, srf2(y + z)
 * 再往下，另一个含有下列内容的 PathTarget
 *		x, y, z
 * 当 setrefs.c 处理这些目标列表时，与
 * 下一层更低目标列表的输出表达式匹配的子表达式会被换成 Var，
 * 于是执行器得到的目标列表看起来像
 *		Var1 + Var2
 *		Var1, srf1(Var2)
 *		Var1, srf2(Var2 + Var3)
 *		x, y, z
 * 从而满足所要的性质。
 *
 * 另一个例子是
 *		srf1(x), srf2(srf3(y))
 * 它必须原样出现在顶层 PathTarget 中，但其下我们需要
 *		srf1(x), srf3(y)
 * 也就是说，每个 SRF 必须在与其参数内
 * SRFs 嵌套深度对应的那一层计算。
 *
 * 有些情况下，某个 SRF 已经在先前的计划层求过值，
 * 我们不应当再次展开它（也就是说，目标里看到的
 * 已经意在引用更低层的子表达式）。因此，不要展开
 * input_target 中出现的任何目标列表表达式，只要它不是 NULL。
 *
 * 同样重要的是，我们要保留给定目标中出现的任何 sortgroupref 标注，
 * 尤其是与 input_target 项匹配的表达式上的标注。
 *
 * 本函数的输出是两个平行列表，一个是
 * PathTargets 的列表，另一个是整数形式的布尔标志列表，表示
 * 对应的 PathTarget 是否含有任何可求值的 SRFs。
 * 列表按它们需要被求值的顺序给出，
 * 最低的 PathTarget 在前。因此最后一项始终是
 * 原来给定的 PathTarget，它之前的任何项表示必须
 * 插在它下面的求值层。第一项不得
 * 含有任何 SRFs（重复 input_target 项的那些除外），因为
 * 它通常会挂到一个没法求值 SRFs 的计划节点上。
 *
 * 注意：用列表表示这些标志看似过重，因为
 * 哪些层含有 SRFs 只有少数几种可能模式。
 * 但这种表示把调用者与那种知识解耦。
 */
void
split_pathtarget_at_srfs(PlannerInfo *root,
						 PathTarget *target, PathTarget *input_target,
						 List **targets, List **targets_contain_srfs)
{
	split_pathtarget_context context;
	int			max_depth;
	bool		need_extra_projection;
	List	   *prev_level_tlist;
	int			lci;
	ListCell   *lc,
			   *lc1,
			   *lc2,
			   *lc3;

	/*
	 * 规划器的 planner.c 把两个物理上相同的
	 * 目标传给我们并不少见，这时不必再费事就可以断定全部
	 * 表达式都能从输入取得。（下面的逻辑也会
	 * 得到同一结论，只是麻烦得多。）
	 */
	if (target == input_target)
	{
		*targets = list_make1(target);
		*targets_contain_srfs = list_make1_int(false);
		return;
	}

	/* 把任何 input_target 表达式往下传给 split_pathtarget_walker() */
	context.input_target_exprs = input_target ? input_target->exprs : NIL;

	/*
	 * 用空的第零层列表做初始化，其后不再有层。
	 * （注意：其实可以不显式表示第零层，因为
	 * 它永远不会收到任何 SRFs，但那样等到建立结果 PathTargets 时
	 * 就得对该层做特殊处理。第零层描述的是
	 * 将交给输入计划节点的、不含 SRF 的 PathTarget。）
	 */
	context.level_srfs = list_make1(NIL);
	context.level_input_vars = list_make1(NIL);
	context.level_input_srfs = list_make1(NIL);

	/* 初始化要在全部目标表达式上累积的数据 */
	context.current_input_vars = NIL;
	context.current_input_srfs = NIL;
	max_depth = 0;
	need_extra_projection = false;

	/* 扫描 PathTarget 中的每个表达式，寻找 SRFs */
	lci = 0;
	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		/* 把这个表达式的 sortgroupref 告诉 split_pathtarget_walker */
		context.current_sgref = get_pathtarget_sortgroupref(target, lci);
		lci++;

		/*
		 * 找出这个表达式中的全部 SRFs 和 Var（以及类似 Var 的节点），
		 * 并登记进上下文结构中适当的列表。
		 */
		context.current_depth = 0;
		split_pathtarget_walker(node, &context);

		/* 不含 SRFs 的表达式没有进一步的意义 */
		if (context.current_depth == 0)
			continue;

		/*
		 * 跟踪整个 PathTarget 上的最大 SRF 嵌套深度。另外，若
		 * 这个表达式确立了新的最大深度，我们就不再关心
		 * 先前的表达式是否含有嵌套的 SRFs；所需的投影
		 * 可以放在最终的 ProjectSet 节点里处理。
		 */
		if (max_depth < context.current_depth)
		{
			max_depth = context.current_depth;
			need_extra_projection = false;
		}

		/*
		 * 若任何最大深度的 SRF 不在其表达式的顶层，
		 * 就需要额外的 Result 节点来计算顶层标量
		 * 表达式。
		 */
		if (max_depth == context.current_depth && !IS_SRF_CALL(node))
			need_extra_projection = true;
	}

	/*
	 * 若没有发现需要求值的 SRFs（也许它们都已在
	 * input_target 里，也许都被常量简化去掉了），
	 * 就不需要 ProjectSet；直接退出。
	 */
	if (max_depth == 0)
	{
		*targets = list_make1(target);
		*targets_contain_srfs = list_make1_int(false);
		return;
	}

	/*
	 * 若不需要额外的投影步骤，顶层需要的 Var 和 SRF 输出
	 * 可以加进最后的 level_input 列表。若
	 * 需要，就往列表里加一个不含 SRF 的层。
	 */
	if (need_extra_projection)
	{
		context.level_srfs = lappend(context.level_srfs, NIL);
		context.level_input_vars = lappend(context.level_input_vars,
										   context.current_input_vars);
		context.level_input_srfs = lappend(context.level_input_srfs,
										   context.current_input_srfs);
	}
	else
	{
		lc = list_nth_cell(context.level_input_vars, max_depth);
		lfirst(lc) = list_concat(lfirst(lc), context.current_input_vars);
		lc = list_nth_cell(context.level_input_srfs, max_depth);
		lfirst(lc) = list_concat(lfirst(lc), context.current_input_srfs);
	}

	/*
	 * 现在构造输出的 PathTargets。最后一个可以直接用原来的目标，
	 * 但我们需要构造一个新的、不含 SRF 的目标，
	 * 表示前一个计划节点必须发出什么，以及
	 * 每个中间 ProjectSet 节点的目标。
	 */
	*targets = *targets_contain_srfs = NIL;
	prev_level_tlist = NIL;

	forthree(lc1, context.level_srfs,
			 lc2, context.level_input_vars,
			 lc3, context.level_input_srfs)
	{
		List	   *level_srfs = (List *) lfirst(lc1);
		PathTarget *ntarget;

		if (lnext(context.level_srfs, lc1) == NULL)
		{
			ntarget = target;
		}
		else
		{
			ntarget = create_empty_pathtarget();

			/*
			 * 这个目标应当实际求值当前
			 * 层的任何 SRFs，并且需要把以后各层需要的任何 Var，
			 * 以及更早算出、以后各层还需要的 SRFs，
			 * 向前传播。
			 */
			add_sp_items_to_pathtarget(ntarget, level_srfs);
			for_each_cell(lc, context.level_input_vars,
						  lnext(context.level_input_vars, lc2))
			{
				List	   *input_vars = (List *) lfirst(lc);

				add_sp_items_to_pathtarget(ntarget, input_vars);
			}
			for_each_cell(lc, context.level_input_srfs,
						  lnext(context.level_input_srfs, lc3))
			{
				List	   *input_srfs = (List *) lfirst(lc);
				ListCell   *lcx;

				foreach(lcx, input_srfs)
				{
					split_pathtarget_item *item = lfirst(lcx);

					if (list_member(prev_level_tlist, item->expr))
						add_sp_item_to_pathtarget(ntarget, item);
				}
			}
			set_pathtarget_cost_width(root, ntarget);
		}

		/*
		 * 把当前目标和它是否计算 SRFs 的标志加进输出列表。
		 */
		*targets = lappend(*targets, ntarget);
		*targets_contain_srfs = lappend_int(*targets_contain_srfs,
											(level_srfs != NIL));

		/* 记住这一层的输出，供下一趟使用 */
		prev_level_tlist = ntarget->exprs;
	}
}

/*
 * 为 split_pathtarget_at_srfs 递归检查表达式。
 *
 * 注意这里我们不努力防止输出列表里出现重复项。
 * 重复项以后会被去掉。
 */
static bool
split_pathtarget_walker(Node *node, split_pathtarget_context *context)
{
	if (node == NULL)
		return false;

	/*
	 * 与 input_target 中已经算过的表达式匹配的子表达式
	 * 可以当作 Var 对待（setrefs.c 处理完之后它确实会变成 Var），
	 * 即使它实际上是 SRF。把它记成
	 * 当前表达式所需要的，并忽略任何
	 * 子结构。（尤其要注意，这保留了
	 * 作为 input_target 中 sortgrouprefs 出现的任何表达式的身份。）
	 */
	if (list_member(context->input_target_exprs, node))
	{
		split_pathtarget_item *item = palloc(sizeof(split_pathtarget_item));

		item->expr = node;
		item->sortgroupref = context->current_sgref;
		context->current_input_vars = lappend(context->current_input_vars,
											  item);
		return false;
	}

	/*
	 * Var 以及类似 Var 的构造也预期从输入取得。
	 * 我们假定这些构造无法含有任何 SRFs（若有一个
	 * 含有，执行器就会因为 SRF 放错位置而失败）。
	 */
	if (IsA(node, Var) ||
		IsA(node, PlaceHolderVar) ||
		IsA(node, Aggref) ||
		IsA(node, GroupingFunc) ||
		IsA(node, WindowFunc))
	{
		split_pathtarget_item *item = palloc(sizeof(split_pathtarget_item));

		item->expr = node;
		item->sortgroupref = context->current_sgref;
		context->current_input_vars = lappend(context->current_input_vars,
											  item);
		return false;
	}

	/*
	 * 若它是 SRF，就递归检查它的输入，确定它的层，并
	 * 在输出列表中做适当的登记。
	 */
	if (IS_SRF_CALL(node))
	{
		split_pathtarget_item *item = palloc(sizeof(split_pathtarget_item));
		List	   *save_input_vars = context->current_input_vars;
		List	   *save_input_srfs = context->current_input_srfs;
		int			save_current_depth = context->current_depth;
		int			srf_depth;
		ListCell   *lc;

		item->expr = node;
		item->sortgroupref = context->current_sgref;

		context->current_input_vars = NIL;
		context->current_input_srfs = NIL;
		context->current_depth = 0;
		context->current_sgref = 0; /* 子表达式不是 sortgroup 项 */

		(void) expression_tree_walker(node, split_pathtarget_walker,
									  (void *) context);

		/* 深度比它下面任何 SRF 再深一层 */
		srf_depth = context->current_depth + 1;

		/* 若创下新的记录深度，就再初始化一层输出列表 */
		if (srf_depth >= list_length(context->level_srfs))
		{
			context->level_srfs = lappend(context->level_srfs, NIL);
			context->level_input_vars = lappend(context->level_input_vars, NIL);
			context->level_input_srfs = lappend(context->level_input_srfs, NIL);
		}

		/* 把这个 SRF 记成需要在适当的层求值 */
		lc = list_nth_cell(context->level_srfs, srf_depth);
		lfirst(lc) = lappend(lfirst(lc), item);

		/* 把它的输入记成同一层所需要的 */
		lc = list_nth_cell(context->level_input_vars, srf_depth);
		lfirst(lc) = list_concat(lfirst(lc), context->current_input_vars);
		lc = list_nth_cell(context->level_input_srfs, srf_depth);
		lfirst(lc) = list_concat(lfirst(lc), context->current_input_srfs);

		/*
		 * 恢复调用者那一层的状态，并按这个 SRF 的存在更新它。
		 * 注意我们把 SRF 本身报告为外围表达式
		 * 求值所需要的。
		 */
		context->current_input_vars = save_input_vars;
		context->current_input_srfs = lappend(save_input_srfs, item);
		context->current_depth = Max(save_current_depth, srf_depth);

		/* 这里就做完了 */
		return false;
	}

	/*
	 * 否则该节点是标量（非集合）表达式，因此递归去
	 * 检查它的输入。
	 */
	context->current_sgref = 0; /* 子表达式不是 sortgroup 项 */
	return expression_tree_walker(node, split_pathtarget_walker,
								  (void *) context);
}

/*
 * 把一个 split_pathtarget_item 加进 PathTarget，除非已经有匹配项。
 * 这类似于 add_new_column_to_pathtarget，但允许
 * 处理 sortgrouprefs。sortgroupref 为零的项可以
 * 与有 sortgroupref 的项合并，并取得后者的
 * sortgroupref。
 *
 * 注意我们不担心可能往
 * PathTarget 里加入重复的 sortgrouprefs。那会很糟，但除非
 * 传给 split_pathtarget_at_srfs 的目标已经有重复，否则应当不可能。
 * 只要它没有，对任何一个非零 sortgroupref，我们最多只能有一个
 * split_pathtarget_item。
 */
static void
add_sp_item_to_pathtarget(PathTarget *target, split_pathtarget_item *item)
{
	int			lci;
	ListCell   *lc;

	/*
	 * 寻找已有的、equal() 且还没有
	 * 冲突 sortgroupref 的项。
	 */
	lci = 0;
	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(target, lci);

		if ((item->sortgroupref == sgref ||
			 item->sortgroupref == 0 ||
			 sgref == 0) &&
			equal(item->expr, node))
		{
			/* 找到匹配。若该项有 sortgroupref，就赋上。 */
			if (item->sortgroupref)
			{
				if (target->sortgrouprefs == NULL)
				{
					target->sortgrouprefs = (Index *)
						palloc0(list_length(target->exprs) * sizeof(Index));
				}
				target->sortgrouprefs[lci] = item->sortgroupref;
			}
			return;
		}
		lci++;
	}

	/*
	 * 没有匹配，因此把项加进 PathTarget。为安全起见复制表达式。
	 */
	add_column_to_pathtarget(target, (Expr *) copyObject(item->expr),
							 item->sortgroupref);
}

/*
 * 对列表的每个元素应用 add_sp_item_to_pathtarget。
 */
static void
add_sp_items_to_pathtarget(PathTarget *target, List *items)
{
	ListCell   *lc;

	foreach(lc, items)
	{
		split_pathtarget_item *item = lfirst(lc);

		add_sp_item_to_pathtarget(target, item);
	}
}
