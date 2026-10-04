/*-------------------------------------------------------------------------
 *
 * pathkeys.c
 *	  Utilities for matching and building path keys
 *
 * See src/backend/optimizer/README for a great deal of information about
 * the nature and use of path keys.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/pathkeys.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "partitioning/partbounds.h"
#include "utils/lsyscache.h"


static bool pathkey_is_redundant(PathKey *new_pathkey, List *pathkeys);
static bool matches_boolean_partition_clause(RestrictInfo *rinfo,
											 RelOptInfo *partrel,
											 int partkeycol);
static Var *find_var_for_subquery_tle(RelOptInfo *rel, TargetEntry *tle);
static bool right_merge_direction(PlannerInfo *root, PathKey *pathkey);


/****************************************************************************
 *		PATHKEY CONSTRUCTION AND REDUNDANCY TESTING（路径键的构造与冗余检测）
 ****************************************************************************/

/*
 * make_canonical_pathkey
 *	  给定 PathKey 的参数，在查询的 "规范" 路径键列表中寻找已有的匹配
 *	  路径键。若还没有，就新建
 *	  一项。
 *
 * 注意：在完成
 * EquivalenceClasses 的合并之前，不得使用本函数。
 */
PathKey *
make_canonical_pathkey(PlannerInfo *root,
					   EquivalenceClass *eclass, Oid opfamily,
					   int strategy, bool nulls_first)
{
	PathKey    *pk;
	ListCell   *lc;
	MemoryContext oldcontext;

	/* 若 EC 集合仍可能变化，就无法构造规范路径键 */
	if (!root->ec_merging_done)
		elog(ERROR, "too soon to build canonical pathkeys");

	/* 传入的 eclass 也许不是规范的，因此要追到最顶层 */
	while (eclass->ec_merged)
		eclass = eclass->ec_merged;

	foreach(lc, root->canon_pathkeys)
	{
		pk = (PathKey *) lfirst(lc);
		if (eclass == pk->pk_eclass &&
			opfamily == pk->pk_opfamily &&
			strategy == pk->pk_strategy &&
			nulls_first == pk->pk_nulls_first)
			return pk;
	}

	/*
	 * 确保规范路径键分配在主规划上下文中。
	 * 普通规划不成问题，但 GEQO 会。
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	pk = makeNode(PathKey);
	pk->pk_eclass = eclass;
	pk->pk_opfamily = opfamily;
	pk->pk_strategy = strategy;
	pk->pk_nulls_first = nulls_first;

	root->canon_pathkeys = lappend(root->canon_pathkeys, pk);

	MemoryContextSwitchTo(oldcontext);

	return pk;
}

/*
 * append_pathkeys
 *		把 'source' 中所有非冗余 PathKeys 追加到 'target'，并
 *		返回更新后的 'target' 列表。
 */
List *
append_pathkeys(List *target, List *source)
{
	ListCell   *lc;

	Assert(target != NIL);

	foreach(lc, source)
	{
		PathKey    *pk = lfirst_node(PathKey, lc);

		if (!pathkey_is_redundant(pk, target))
			target = lappend(target, pk);
	}
	return target;
}

/*
 * pathkey_is_redundant
 *	   路径键是否与列表中已有路径键冗余？
 *
 * 我们检测两种情形：
 *
 * 1. 若新路径键的等价类包含常量，且不在
 * 外连接之下，则可以不把它当作排序键。例如：
 *			SELECT ... WHERE x = 42 ORDER BY x, y;
 * 不妨只按 y 排序。注意，由于操作符族匹配，
 * 这在语义上是正确的：我们知道该等值约束会
 * 在可能与该等价类配套的任何
 * 排序操作符的意义下，把变量绑定到单一值。这条规则不仅让
 * 我们简化（甚至跳过）显式排序，还允许在存在无关索引列时，
 * 把索引的排序顺序匹配到查询。
 *
 * 2. 若新路径键的等价类与路径键列表中任何
 * 已有成员的等价类相同，则它是冗余的。例如：
 *			SELECT ... ORDER BY x, x;
 *			SELECT ... ORDER BY x, x DESC;
 *			SELECT ... WHERE x = y ORDER BY x, y;
 * 在所有这些情形中，第二个排序键无法区分那些被第一个
 * 视为相等的值，因此用它没有意义。
 * 尤其要注意，不必比较操作符族（该 EC 的所有操作符族
 * 对相等的看法相同），也不必比较排序方向。
 *
 * 给定路径键和列表成员都必须是规范的，这样
 * 才能正确工作，但这没问题，因为我们不再构造任何
 * 非规范路径键。（注意：路径键 *列表* 为
 * 规范，还包括不得有冗余项这一额外条件，
 * 而这正是这里要检查的。）
 *
 * 因为 equivclass.c 的机制对每个查询的任何 EC 只形成一份副本，
 * 指针比较就足以判断规范 EC 是否相同。
 */
static bool
pathkey_is_redundant(PathKey *new_pathkey, List *pathkeys)
{
	EquivalenceClass *new_ec = new_pathkey->pk_eclass;
	ListCell   *lc;

	/* 检查 EC 是否包含常量---若包含则无条件冗余 */
	if (EC_MUST_BE_REDUNDANT(new_ec))
		return true;

	/* 若同一 EC 已在列表中使用，则冗余 */
	foreach(lc, pathkeys)
	{
		PathKey    *old_pathkey = (PathKey *) lfirst(lc);

		if (new_ec == old_pathkey->pk_eclass)
			return true;
	}

	return false;
}

/*
 * make_pathkey_from_sortinfo
 *	  给定表达式和排序信息，创建一个 PathKey。
 *	  结果总是 "规范" PathKey，但也许是冗余的。
 *
 * 若 PathKey 由 SortGroupClause 生成，sortref 应当是
 * 该 SortGroupClause 的 SortGroupRef；否则为零。
 *
 * 若 rel 不是 NULL，它标识我们正在为其考虑
 * 路径的某个特定关系，并表示可以考虑该关系的子 EC 成员。
 * 否则忽略子成员。（见
 * get_eclass_for_sort_expr 的注释。）
 *
 * 若应当创建表示该排序键所缺的任何 EquivalenceClass，
 * 则 create_it 为 true。若为 false，而排序键尚未出现在任何
 * EquivalenceClass 中，则返回 NULL。
 */
static PathKey *
make_pathkey_from_sortinfo(PlannerInfo *root,
						   Expr *expr,
						   Oid opfamily,
						   Oid opcintype,
						   Oid collation,
						   bool reverse_sort,
						   bool nulls_first,
						   Index sortref,
						   Relids rel,
						   bool create_it)
{
	int16		strategy;
	Oid			equality_op;
	List	   *opfamilies;
	EquivalenceClass *eclass;

	strategy = reverse_sort ? BTGreaterStrategyNumber : BTLessStrategyNumber;

	/*
	 * EquivalenceClasses 需要包含基于可归并等值操作符族
	 * 成员资格的操作符族列表，而这些操作符可能属于
	 * 不止一个操作符族。因此必须查出该操作符族的等值
	 * 操作符并取得其成员资格。
	 */
	equality_op = get_opfamily_member(opfamily,
									  opcintype,
									  opcintype,
									  BTEqualStrategyNumber);
	if (!OidIsValid(equality_op))	/* 不应当发生 */
		elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
			 BTEqualStrategyNumber, opcintype, opcintype, opfamily);
	opfamilies = get_mergejoin_opfamilies(equality_op);
	if (!opfamilies)			/* 肯定应当能找到一些 */
		elog(ERROR, "could not find opfamilies for equality operator %u",
			 equality_op);

	/* 现在查找或（可选地）创建一个匹配的 EquivalenceClass */
	eclass = get_eclass_for_sort_expr(root, expr,
									  opfamilies, opcintype, collation,
									  sortref, rel, create_it);

	/* 若没有 EC 且 !create_it，则失败 */
	if (!eclass)
		return NULL;

	/* 最后就可以查找或创建一个 PathKey 节点 */
	return make_canonical_pathkey(root, eclass, opfamily,
								  strategy, nulls_first);
}

/*
 * make_pathkey_from_sortop
 *	  与 make_pathkey_from_sortinfo 类似，但从排序操作符出发。
 *
 * 这最终应当去掉，但需要先重构
 * SortGroupClause。
 */
static PathKey *
make_pathkey_from_sortop(PlannerInfo *root,
						 Expr *expr,
						 Oid ordering_op,
						 bool nulls_first,
						 Index sortref,
						 bool create_it)
{
	Oid			opfamily,
				opcintype,
				collation;
	int16		strategy;

	/* 在 pg_amop 中查找该操作符---失败不应当发生 */
	if (!get_ordering_op_properties(ordering_op,
									&opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 ordering_op);

	/* 因为 SortGroupClause 不携带排序规则，去查表达式 */
	collation = exprCollation((Node *) expr);

	return make_pathkey_from_sortinfo(root,
									  expr,
									  opfamily,
									  opcintype,
									  collation,
									  (strategy == BTGreaterStrategyNumber),
									  nulls_first,
									  sortref,
									  NULL,
									  create_it);
}


/****************************************************************************
 *		PATHKEY COMPARISONS（路径键比较）
 ****************************************************************************/

/*
 * compare_pathkeys
 *	  比较两个路径键，看它们是否等价；若不等价，看
 *	  其中一个是否 "更好"。
 *
 *	  我们假定路径键是规范的，因此可以用简单的指针比较
 *	  判断相等。
 */
PathKeysComparison
compare_pathkeys(List *keys1, List *keys2)
{
	ListCell   *key1,
			   *key2;

	/*
	 * 若传入的是两个相同列表，就快速退出。这大多
	 * 抓住两者都是 NIL 的情形，而这足够常见，
	 * 值得做这个测试。
	 */
	if (keys1 == keys2)
		return PATHKEYS_EQUAL;

	forboth(key1, keys1, key2, keys2)
	{
		PathKey    *pathkey1 = (PathKey *) lfirst(key1);
		PathKey    *pathkey2 = (PathKey *) lfirst(key2);

		if (pathkey1 != pathkey2)
			return PATHKEYS_DIFFERENT;	/* 不必再找下去 */
	}

	/*
	 * 若只到达其中一个列表的末尾，另一个更长，
	 * 因此不是子集。
	 */
	if (key1 != NULL)
		return PATHKEYS_BETTER1;	/* key1 更长 */
	if (key2 != NULL)
		return PATHKEYS_BETTER2;	/* key2 更长 */
	return PATHKEYS_EQUAL;
}

/*
 * pathkeys_contained_in
 *	  compare_pathkeys 的常见特例：我们只想知道
 *	  keys2 的排序是否至少和 keys1 一样好。
 */
bool
pathkeys_contained_in(List *keys1, List *keys2)
{
	switch (compare_pathkeys(keys1, keys2))
	{
		case PATHKEYS_EQUAL:
		case PATHKEYS_BETTER2:
			return true;
		default:
			break;
	}
	return false;
}

/*
 * pathkeys_count_contained_in
 *    与 pathkeys_contained_in 相同，但还会设置 keys1 与 keys2
 *    最长公共前缀的长度。
 */
bool
pathkeys_count_contained_in(List *keys1, List *keys2, int *n_common)
{
	int			n = 0;
	ListCell   *key1,
			   *key2;

	/*
	 * 看看能否避免同时遍历两个列表。这项优化
	 * 在最坏情况测试中能为规划时间省下几个百分点。
	 */
	if (keys1 == keys2)
	{
		*n_common = list_length(keys1);
		return true;
	}
	else if (keys1 == NIL)
	{
		*n_common = 0;
		return true;
	}
	else if (keys2 == NIL)
	{
		*n_common = 0;
		return false;
	}

	/*
	 * 若两个列表都非空，就同时遍历，看有多少
	 * 项是共享的。
	 */
	forboth(key1, keys1, key2, keys2)
	{
		PathKey    *pathkey1 = (PathKey *) lfirst(key1);
		PathKey    *pathkey2 = (PathKey *) lfirst(key2);

		if (pathkey1 != pathkey2)
		{
			*n_common = n;
			return false;
		}
		n++;
	}

	/* 若结束时遇到空值，说明已经处理完整个列表。 */
	*n_common = n;
	return (key1 == NULL);
}

/*
 * get_cheapest_path_for_pathkeys
 *	  按指定准则，找出满足给定路径键和参数化的
 *	  最低代价路径。
 *	  若没有这样的路径则返回 NULL。
 *
 * 'paths' 是生成同一关系的可能路径列表
 * 'pathkeys' 表示所需的排序（规范形式！）
 * 'required_outer' 表示参数化路径允许的外部关系
 * 'cost_criterion' 是 STARTUP_COST 或 TOTAL_COST
 * 'require_parallel_safe' 使我们只考虑并行安全的路径
 */
Path *
get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   Relids required_outer,
							   CostSelector cost_criterion,
							   bool require_parallel_safe)
{
	Path	   *matched_path = NULL;
	ListCell   *l;

	foreach(l, paths)
	{
		Path	   *path = (Path *) lfirst(l);

		/*
		 * 因为代价比较比路径键比较便宜得多，先做
		 * 前者。（XXX 现在还是这样吗？）
		 */
		if (matched_path != NULL &&
			compare_path_costs(matched_path, path, cost_criterion) <= 0)
			continue;

		if (require_parallel_safe && !path->parallel_safe)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys) &&
			bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			matched_path = path;
	}
	return matched_path;
}

/*
 * get_cheapest_fractional_path_for_pathkeys
 *	  找出满足给定路径键和参数化、且取出全部
 *	  元组中指定比例时代价最低的路径。
 *	  若没有这样的路径则返回 NULL。
 *
 * 分数参数的含义见
 * compare_fractional_path_costs()。
 *
 * 'paths' 是生成同一关系的可能路径列表
 * 'pathkeys' 表示所需的排序（规范形式！）
 * 'required_outer' 表示参数化路径允许的外部关系
 * 'fraction' 是预期取出的总元组比例
 */
Path *
get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  Relids required_outer,
										  double fraction)
{
	Path	   *matched_path = NULL;
	ListCell   *l;

	foreach(l, paths)
	{
		Path	   *path = (Path *) lfirst(l);

		/*
		 * 因为代价比较比路径键比较便宜得多，先做
		 * 前者。（XXX 现在还是这样吗？）
		 */
		if (matched_path != NULL &&
			compare_fractional_path_costs(matched_path, path, fraction) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys) &&
			bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			matched_path = path;
	}
	return matched_path;
}


/*
 * get_cheapest_parallel_safe_total_inner
 *	  找出总代价最低的、未参数化的并行安全路径。
 */
Path *
get_cheapest_parallel_safe_total_inner(List *paths)
{
	ListCell   *l;

	foreach(l, paths)
	{
		Path	   *innerpath = (Path *) lfirst(l);

		if (innerpath->parallel_safe &&
			bms_is_empty(PATH_REQ_OUTER(innerpath)))
			return innerpath;
	}

	return NULL;
}

/****************************************************************************
 *		NEW PATHKEY FORMATION（新路径键的形成）
 ****************************************************************************/

/*
 * build_index_pathkeys
 *	  构造描述给定索引的索引扫描所诱导排序的路径键列表。
 *	  （注意无序索引不
 *	  诱导任何排序，因此返回 NIL。）
 *
 * 若 'scandir' 为 BackwardScanDirection，则构造表示
 * 反向扫描该索引的路径键。
 *
 * 对覆盖索引只迭代键列，因为非键列
 * 不影响索引排序。结果是规范的，也就是
 * 去掉了冗余路径键；因此项数也许少于
 * 索引中的键列数。
 *
 * 提前停止的另一个原因是，我们也许能判断
 * 某索引列的排序顺序对本查询没有意义。不过
 * 该测试只基于是否存在 EquivalenceClass，而不是
 * 基于在路径键列表中的位置，因此并不完整。调用方应当调用
 * truncate_useless_pathkeys()，以便可能去掉更多路径键。
 */
List *
build_index_pathkeys(PlannerInfo *root,
					 IndexOptInfo *index,
					 ScanDirection scandir)
{
	List	   *retval = NIL;
	ListCell   *lc;
	int			i;

	if (index->sortopfamily == NULL)
		return NIL;				/* 不可排序的索引 */

	i = 0;
	foreach(lc, index->indextlist)
	{
		TargetEntry *indextle = (TargetEntry *) lfirst(lc);
		Expr	   *indexkey;
		bool		reverse_sort;
		bool		nulls_first;
		PathKey    *cpathkey;

		/*
		 * INCLUDE 列在索引中无序存放，因此它们不
		 * 支持有序索引扫描。
		 */
		if (i >= index->nkeycolumns)
			break;

		/* 我们假定不必复制 tlist 项 */
		indexkey = indextle->expr;

		if (ScanDirectionIsBackward(scandir))
		{
			reverse_sort = !index->reverse_sort[i];
			nulls_first = !index->nulls_first[i];
		}
		else
		{
			reverse_sort = index->reverse_sort[i];
			nulls_first = index->nulls_first[i];
		}

		/*
		 * 好，试着为这个排序键构造规范路径键。
		 */
		cpathkey = make_pathkey_from_sortinfo(root,
											  indexkey,
											  index->sortopfamily[i],
											  index->opcintype[i],
											  index->indexcollations[i],
											  reverse_sort,
											  nulls_first,
											  0,
											  index->rel->relids,
											  false);

		if (cpathkey)
		{
			/*
			 * 在某个 EquivalenceClass 中找到了该排序键，因此它与
			 * 本查询相关。除非冗余，否则加入列表。
			 */
			if (!pathkey_is_redundant(cpathkey, retval))
				retval = lappend(retval, cpathkey);
		}
		else
		{
			/*
			 * 布尔索引键即使不
			 * 出现在 EquivalenceClass 中，也可能冗余，因为我们对
			 * 布尔等值条件有特殊处理---见
			 * indexcol_is_bool_constant_for_query() 的注释。若适用，就可以
			 * 继续检查更低序的索引列。否则，该
			 * 排序键对本查询不是有意义的排序，因此应当
			 * 停止考虑索引列；任何更低序的排序
			 * 键也不会有用。
			 */
			if (!indexcol_is_bool_constant_for_query(root, index, i))
				break;
		}

		i++;
	}

	return retval;
}

/*
 * partkey_is_bool_constant_for_query
 *
 * 若分区键列被查询的 WHERE 条件约束为常量，则它对排序
 * 考虑没有意义。通常这意味着有限制子句
 * WHERE partkeycol = constant，它会变成一个
 * 包含常量的 EquivalenceClass，
 * 并被
 * build_partition_pathkeys() 认作冗余。但若分区键列是
 * 布尔变量（或表达式），就不会看到这样的
 * WHERE 子句，因为表达式预处理会把它简化成
 * "WHERE partkeycol" 或 "WHERE NOT partkeycol"。因此不会
 * 有匹配的 EquivalenceClass（除非查询还包含
 * "ORDER BY partkeycol"）。为了让这类情形与
 * 非布尔值时同样工作，本函数用来检测指定的
 * 分区键列是否匹配一条布尔限制子句。
 */
static bool
partkey_is_bool_constant_for_query(RelOptInfo *partrel, int partkeycol)
{
	PartitionScheme partscheme = partrel->part_scheme;
	ListCell   *lc;

	/*
	 * 若分区键不是布尔类型，就不可能匹配上。
	 *
	 * 分区目前只能用内建访问方法，因此检查
	 * 内建布尔操作符族就够了。
	 */
	if (!IsBuiltinBooleanOpfamily(partscheme->partopfamily[partkeycol]))
		return false;

	/* 检查分区关系的每条限制子句 */
	foreach(lc, partrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* 忽略伪常量限定条件，它们不会匹配 */
		if (rinfo->pseudoconstant)
			continue;

		/* 看能否把子句的表达式匹配到分区键列 */
		if (matches_boolean_partition_clause(rinfo, partrel, partkeycol))
			return true;
	}

	return false;
}

/*
 * matches_boolean_partition_clause
 *		判断 rinfo 描述的布尔子句是否匹配
 *		partrel 的第 partkeycol 个分区键列。
 *
 * "匹配" 可以是精确匹配（等价于 partkey = true），
 * 或精确匹配之上的 NOT（等价于 partkey = false）。
 */
static bool
matches_boolean_partition_clause(RestrictInfo *rinfo,
								 RelOptInfo *partrel, int partkeycol)
{
	Node	   *clause = (Node *) rinfo->clause;
	Node	   *partexpr = (Node *) linitial(partrel->partexprs[partkeycol]);

	/* 直接匹配？ */
	if (equal(partexpr, clause))
		return true;
	/* NOT 子句？ */
	else if (is_notclause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		if (equal(partexpr, arg))
			return true;
	}

	return false;
}

/*
 * build_partition_pathkeys
 *	  构造描述 partrel 的分区所诱导排序的路径键列表，
 *	  按 scandir 做正向或反向扫描。
 *	  调用方必须已经检查分区确实有序，
 *
 * 这由 partitions_are_ordered() 检测。
 * 正向和反向扫描都要处理。
 *
 * 若路径键只为分区键的一个前缀而建，则把 *partialkeys 设为 true；
 * 若路径键包含分区键的全部列，
 * 则设为 false。
 */
List *
build_partition_pathkeys(PlannerInfo *root, RelOptInfo *partrel,
						 ScanDirection scandir, bool *partialkeys)
{
	List	   *retval = NIL;
	PartitionScheme partscheme = partrel->part_scheme;
	int			i;

	Assert(partscheme != NULL);
	Assert(partitions_are_ordered(partrel->boundinfo, partrel->live_parts));
	/* 目前只能应付基关系 */
	Assert(IS_SIMPLE_REL(partrel));

	for (i = 0; i < partscheme->partnatts; i++)
	{
		PathKey    *cpathkey;
		Expr	   *keyCol = (Expr *) linitial(partrel->partexprs[i]);

		/*
		 * 试着为这个分区键构造规范路径键。
		 *
		 * 我们假定 PartitionDesc 把任何 NULL 分区列在最后，因此
		 * 把扫描当作 NULLS LAST 索引：仅反向扫描时
		 * nulls_first 为真。
		 */
		cpathkey = make_pathkey_from_sortinfo(root,
											  keyCol,
											  partscheme->partopfamily[i],
											  partscheme->partopcintype[i],
											  partscheme->partcollation[i],
											  ScanDirectionIsBackward(scandir),
											  ScanDirectionIsBackward(scandir),
											  0,
											  partrel->relids,
											  false);


		if (cpathkey)
		{
			/*
			 * 在某个 EquivalenceClass 中找到了该排序键，因此它与
			 * 本查询相关。除非冗余，否则加入列表。
			 */
			if (!pathkey_is_redundant(cpathkey, retval))
				retval = lappend(retval, cpathkey);
		}
		else
		{
			/*
			 * 布尔分区键即使不
			 * 出现在 EquivalenceClass 中，也可能冗余，因为我们对
			 * 布尔等值条件有特殊处理---见
			 * partkey_is_bool_constant_for_query() 的注释。若适用，就可以
			 * 继续检查更低序的分区键。否则，该
			 * 排序键对本查询不是有意义的排序，因此应当
			 * 停止考虑分区列；任何更低序的排序
			 * 键也不会有用。
			 */
			if (!partkey_is_bool_constant_for_query(partrel, i))
			{
				*partialkeys = true;
				return retval;
			}
		}
	}

	*partialkeys = false;
	return retval;
}

/*
 * build_expression_pathkey
 *	  构造描述按给定排序操作符对单个表达式排序的路径键列表。
 *	  expr 和 rel 的含义同 make_pathkey_from_sortinfo。
 *
 * 我们假定该操作符的默认排序方向，
 * 从而推出其余参数。
 *
 * 与 make_pathkey_from_sortinfo 类似，若 create_it
 * 为 false 且该表达式尚未在某个 EquivalenceClass 中，则结果为 NIL。
 */
List *
build_expression_pathkey(PlannerInfo *root,
						 Expr *expr,
						 Oid opno,
						 Relids rel,
						 bool create_it)
{
	List	   *pathkeys;
	Oid			opfamily,
				opcintype;
	int16		strategy;
	PathKey    *cpathkey;

	/* 在 pg_amop 中查找该操作符---失败不应当发生 */
	if (!get_ordering_op_properties(opno,
									&opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 opno);

	cpathkey = make_pathkey_from_sortinfo(root,
										  expr,
										  opfamily,
										  opcintype,
										  exprCollation((Node *) expr),
										  (strategy == BTGreaterStrategyNumber),
										  (strategy == BTGreaterStrategyNumber),
										  0,
										  rel,
										  create_it);

	if (cpathkey)
		pathkeys = list_make1(cpathkey);
	else
		pathkeys = NIL;

	return pathkeys;
}

/*
 * convert_subquery_pathkeys
 *	  构造描述子查询结果排序的路径键列表，
 *	  用外层查询的术语来表达。这本质上是一项
 *	  转换工作。
 *
 * 'rel'：外层查询中该子查询关系的 RelOptInfo。
 * 'subquery_pathkeys'：子查询的输出路径键，用它自己的术语。
 * 'subquery_tlist'：子查询的输出目标列表，用它自己的术语。
 *
 * 这里有意不做 truncate_useless_pathkeys()，因为有些
 * 情形下看到子查询的原始排序是有帮助的。
 * 例如，若它返回 ORDER BY x DESC，这也许促使我们
 * 用 DESC 而不是 ASC 构造归并连接；但
 * right_merge_direction 启发式会把这一信息丢掉。
 */
List *
convert_subquery_pathkeys(PlannerInfo *root, RelOptInfo *rel,
						  List *subquery_pathkeys,
						  List *subquery_tlist)
{
	List	   *retval = NIL;
	int			retvallen = 0;
	int			outer_query_keys = list_length(root->query_pathkeys);
	ListCell   *i;

	foreach(i, subquery_pathkeys)
	{
		PathKey    *sub_pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *sub_eclass = sub_pathkey->pk_eclass;
		PathKey    *best_pathkey = NULL;

		if (sub_eclass->ec_has_volatile)
		{
			/*
			 * 若 sub_pathkey 的 EquivalenceClass 是易变的，则它必定
			 * 来自 ORDER BY 子句，我们必须把它匹配到
			 * 同一个目标列表项。
			 */
			TargetEntry *tle;
			Var		   *outer_var;

			if (sub_eclass->ec_sortref == 0)	/* 不可能发生 */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(sub_eclass->ec_sortref, subquery_tlist);
			Assert(tle);
			/* TLE 对外层查询真的可用吗？ */
			outer_var = find_var_for_subquery_tle(rel, tle);
			if (outer_var)
			{
				/* 我们可以表示这个 sub_pathkey */
				EquivalenceMember *sub_member;
				EquivalenceClass *outer_ec;

				Assert(list_length(sub_eclass->ec_members) == 1);
				sub_member = (EquivalenceMember *) linitial(sub_eclass->ec_members);

				/*
				 * 注意：对易变的 sub_eclass 把 sortref 设为 0
				 * 看起来也许奇怪。不过，该
				 * 表达式在外层查询中 *不是* 易变的：它只是
				 * 一个引用子查询所输出内容的 Var。（也就是说，
				 * 外层查询不会重新执行这个易变
				 * 表达式本身。）因此这样是可以的。
				 */
				outer_ec =
					get_eclass_for_sort_expr(root,
											 (Expr *) outer_var,
											 sub_eclass->ec_opfamilies,
											 sub_member->em_datatype,
											 sub_eclass->ec_collation,
											 0,
											 rel->relids,
											 false);

				/*
				 * 若找不到匹配的 EC，子路径键对
				 * 外层查询没有意义
				 */
				if (outer_ec)
					best_pathkey =
						make_canonical_pathkey(root,
											   outer_ec,
											   sub_pathkey->pk_opfamily,
											   sub_pathkey->pk_strategy,
											   sub_pathkey->pk_nulls_first);
			}
		}
		else
		{
			/*
			 * 否则，sub_pathkey 的 EquivalenceClass 可能包含
			 * 多个元素（表示已知多个项
			 * 实际上相等）。每个元素也许匹配外层查询可见的
			 * 输出列中的零个、一个或
			 * 多个。
			 * 这意味着在外层查询的语境中，该
			 * sub_pathkey 可能有多种表示。理想情况下我们会
			 * 全部生成，并全部放进外层查询的一个 EC，
			 * 从而把相等知识传播到外层查询。目前无法这样做，因为调用本函数时外层
			 * 查询的 EquivalenceClasses 已经冻结。
			 * 我们改为优先选择 "得分" 最高的那个
			 * （EC 同伴数，若匹配外层
			 * query_pathkeys 再加一）。这最可能在
			 * 外层查询中有用。
			 */
			int			best_score = -1;
			ListCell   *j;

			foreach(j, sub_eclass->ec_members)
			{
				EquivalenceMember *sub_member = (EquivalenceMember *) lfirst(j);
				Expr	   *sub_expr = sub_member->em_expr;
				Oid			sub_expr_type = sub_member->em_datatype;
				Oid			sub_expr_coll = sub_eclass->ec_collation;
				ListCell   *k;

				if (sub_member->em_is_child)
					continue;	/* 这里忽略子项 */

				foreach(k, subquery_tlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(k);
					Var		   *outer_var;
					Expr	   *tle_expr;
					EquivalenceClass *outer_ec;
					PathKey    *outer_pk;
					int			score;

					/* TLE 对外层查询真的可用吗？ */
					outer_var = find_var_for_subquery_tle(rel, tle);
					if (!outer_var)
						continue;

					/*
					 * 若目标列表项在排序键规范化之后
					 * 匹配，就认为它匹配。这是
					 * 需要的，因为 sub_expr 经过了同样的
					 * 处理。
					 */
					tle_expr = canonicalize_ec_expression(tle->expr,
														  sub_expr_type,
														  sub_expr_coll);
					if (!equal(tle_expr, sub_expr))
						continue;

					/* 看 TLE 是否有匹配的 EC */
					outer_ec = get_eclass_for_sort_expr(root,
														(Expr *) outer_var,
														sub_eclass->ec_opfamilies,
														sub_expr_type,
														sub_expr_coll,
														0,
														rel->relids,
														false);

					/*
					 * 若找不到匹配的 EC，这个子路径键对
					 * 外层查询没有意义
					 */
					if (!outer_ec)
						continue;

					outer_pk = make_canonical_pathkey(root,
													  outer_ec,
													  sub_pathkey->pk_opfamily,
													  sub_pathkey->pk_strategy,
													  sub_pathkey->pk_nulls_first);
					/* 得分 = 等价同伴的个数 */
					score = list_length(outer_ec->ec_members) - 1;
					/* 若它匹配对应的 query_pathkeys 项，则 +1 */
					if (retvallen < outer_query_keys &&
						list_nth(root->query_pathkeys, retvallen) == outer_pk)
						score++;
					if (score > best_score)
					{
						best_pathkey = outer_pk;
						best_score = score;
					}
				}
			}
		}

		/*
		 * 若找不到这个 sub_pathkey 的表示，就
		 * 结束（它右边的那些也不能用）。
		 */
		if (!best_pathkey)
			break;

		/*
		 * 去掉冗余的排序信息；若外层查询把
		 * 子查询的键视为等价，就会发生……
		 */
		if (!pathkey_is_redundant(best_pathkey, retval))
		{
			retval = lappend(retval, best_pathkey);
			retvallen++;
		}
	}

	return retval;
}

/*
 * find_var_for_subquery_tle
 *
 * 若给定的子查询 tlist 项将由子查询的
 * 扫描节点输出，则返回它的 Var，否则返回 NULL。
 *
 * 需要这样做，以确保我们不返回描述那些
 * 在子查询扫描之上不可用的值的路径键。
 */
static Var *
find_var_for_subquery_tle(RelOptInfo *rel, TargetEntry *tle)
{
	ListCell   *lc;

	/* 若 TLE 是 resjunk，它对外层查询肯定不可见 */
	if (tle->resjunk)
		return NULL;

	/* 搜索该关系的目标列表，看它会返回什么 */
	foreach(lc, rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);

		/* 忽略占位符 */
		if (!IsA(var, Var))
			continue;
		Assert(var->varno == rel->relid);

		/* 若找到引用这个 TLE 的 Var，就可以 */
		if (var->varattno == tle->resno)
			return copyObject(var); /* 为安全起见做一份副本 */
	}
	return NULL;
}

/*
 * build_join_pathkeys
 *	  为归并连接或
 *	  嵌套循环连接所构造的连接关系建立路径键。通常与外层路径的键相同。
 *
 *	  EXCEPTION：在 FULL、RIGHT 或 RIGHT_ANTI 连接中，无法把
 *	  结果视为具有外层路径的路径键，因为空的左侧行
 *	  可能被插入到随机位置。必须把它视为未排序。
 *
 *	  我们截掉对更高层连接没有意义的任何路径键。
 *
 * 'joinrel' 是正在为其形成路径的连接关系
 * 'jointype' 是连接类型（内连接、左连接、全连接等）
 * 'outer_pathkeys' 是当前外层路径的路径键列表
 *
 * 返回新路径键的列表。
 */
List *
build_join_pathkeys(PlannerInfo *root,
					RelOptInfo *joinrel,
					JoinType jointype,
					List *outer_pathkeys)
{
	if (jointype == JOIN_FULL ||
		jointype == JOIN_RIGHT ||
		jointype == JOIN_RIGHT_ANTI)
		return NIL;

	/*
	 * 这段代码以前相当复杂，但现在所有路径键
	 * 子列表从一开始就是规范化的，这里什么都
	 * 不必做！
	 *
	 * 不过，需要截断路径键列表，因为它也许
	 * 含有对形成这个连接关系有用、但对
	 * 更高层没有意义的路径键。
	 */
	return truncate_useless_pathkeys(root, joinrel, outer_pathkeys);
}

/****************************************************************************
 *		PATHKEYS AND SORT CLAUSES（路径键与排序子句）
 ****************************************************************************/

/*
 * make_pathkeys_for_sortclauses
 *		生成表示由一组 SortGroupClauses
 *		指定的排序顺序的路径键列表
 *
 * 得到的 PathKeys 总是规范形式。（实际上，
 * 已经没有任何代码会创建非规范 PathKeys。）
 *
 * 'sortclauses' 是 SortGroupClause 节点的列表
 * 'tlist' 是用来查找被引用 tlist 项的目标列表
 */
List *
make_pathkeys_for_sortclauses(PlannerInfo *root,
							  List *sortclauses,
							  List *tlist)
{
	List	   *result;
	bool		sortable;

	result = make_pathkeys_for_sortclauses_extended(root,
													&sortclauses,
													tlist,
													false,
													&sortable);
	/* 若并非所有子句都可排序，那是调用方的错误 */
	Assert(sortable);
	return result;
}

/*
 * make_pathkeys_for_sortclauses_extended
 *		生成表示由一组 SortGroupClauses
 *		指定的排序顺序的路径键列表
 *
 * make_pathkeys_for_sortclauses 的注释这里也适用。此外：
 *
 * 若 remove_redundant 为 true，则发现会导致
 * 冗余路径键的任何排序子句都会从 sortclauses 列表中去掉
 * （因此这个版本里该列表必须按引用传递）。
 *
 * 若所有排序子句事实上都可排序，则 *sortable 设为 true。
 * 若有不可排序的，则忽略它们，只把 *sortable 设为 false。
 * （那种情况下，输出的路径键列表并不真正有用。不过，
 * 我们仍会处理整个 sortclauses 列表，因为经由等价类逻辑证明冗余的子句
 * 仍然可以去掉，这仍然有效。
 * 即使那时必须做哈希，也最好不要对
 * 冗余列做哈希。）
 */
List *
make_pathkeys_for_sortclauses_extended(PlannerInfo *root,
									   List **sortclauses,
									   List *tlist,
									   bool remove_redundant,
									   bool *sortable)
{
	List	   *pathkeys = NIL;
	ListCell   *l;

	*sortable = true;
	foreach(l, *sortclauses)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		Expr	   *sortkey;
		PathKey    *pathkey;

		sortkey = (Expr *) get_sortgroupclause_expr(sortcl, tlist);
		if (!OidIsValid(sortcl->sortop))
		{
			*sortable = false;
			continue;
		}
		pathkey = make_pathkey_from_sortop(root,
										   sortkey,
										   sortcl->sortop,
										   sortcl->nulls_first,
										   sortcl->tleSortGroupRef,
										   true);

		/* 规范形式会去掉冗余的排序键 */
		if (!pathkey_is_redundant(pathkey, pathkeys))
			pathkeys = lappend(pathkeys, pathkey);
		else if (remove_redundant)
			*sortclauses = foreach_delete_current(*sortclauses, l);
	}
	return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES（路径键与归并子句）
 ****************************************************************************/

/*
 * initialize_mergeclause_eclasses
 *		设置归并子句 restrictinfo 中的 EquivalenceClass 链接。
 *
 * RestrictInfo 含有一些字段，我们可以在其中缓存指向
 * 该归并子句左右输入的 EquivalenceClasses 的指针。
 * （若归并子句是真正的等价子句，它们会是
 * 同一个 EquivalenceClass，否则不是。）若归并子句被用来
 * 生成 EquivalenceClass，或派生自 EquivalenceClass，
 * 则很容易设置 left_ec 和 right_ec 成员---否则，
 * 应当调用本函数来设置它们。必要时会生成新的
 * EquivalenceClauses，以表示归并子句的左
 * 右两侧。
 *
 * 注意这是在 EC 合并完成之前调用的，因此链接不一定
 * 指向规范 EC。在真正用于
 * 任何事情之前，必须调用 update_mergeclause_eclasses，以确保
 * 它们已更新为指向规范 EC。
 */
void
initialize_mergeclause_eclasses(PlannerInfo *root, RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			lefttype,
				righttype;

	/* 应当是归并子句…… */
	Assert(restrictinfo->mergeopfamilies != NIL);
	/* ……且链接尚未设置 */
	Assert(restrictinfo->left_ec == NULL);
	Assert(restrictinfo->right_ec == NULL);

	/* 需要该操作符声明的输入类型 */
	op_input_types(((OpExpr *) clause)->opno, &lefttype, &righttype);

	/* 为每一侧查找或创建一个匹配的 EquivalenceClass */
	restrictinfo->left_ec =
		get_eclass_for_sort_expr(root,
								 (Expr *) get_leftop(clause),
								 restrictinfo->mergeopfamilies,
								 lefttype,
								 ((OpExpr *) clause)->inputcollid,
								 0,
								 NULL,
								 true);
	restrictinfo->right_ec =
		get_eclass_for_sort_expr(root,
								 (Expr *) get_rightop(clause),
								 restrictinfo->mergeopfamilies,
								 righttype,
								 ((OpExpr *) clause)->inputcollid,
								 0,
								 NULL,
								 true);
}

/*
 * update_mergeclause_eclasses
 *		使归并子句 restrictinfo 中缓存的 EquivalenceClass 链接有效。
 *		这些指针应当已由 process_equivalence 或
 *
 * initialize_mergeclause_eclasses 设置，但它们也许被设成了
 * 非规范 EC，而那些 EC 后来被合并了。
 * 若是这样，就一直追到规范的
 * 已合并父项。
 */
void
update_mergeclause_eclasses(PlannerInfo *root, RestrictInfo *restrictinfo)
{
	/* 应当是归并子句…… */
	Assert(restrictinfo->mergeopfamilies != NIL);
	/* ……且指针已经设置 */
	Assert(restrictinfo->left_ec != NULL);
	Assert(restrictinfo->right_ec != NULL);

	/* 按需要追到最顶层 */
	while (restrictinfo->left_ec->ec_merged)
		restrictinfo->left_ec = restrictinfo->left_ec->ec_merged;
	while (restrictinfo->right_ec->ec_merged)
		restrictinfo->right_ec = restrictinfo->right_ec->ec_merged;
}

/*
 * find_mergeclauses_for_outer_pathkeys
 *	  本例程尝试找出一组可以与
 *	  连接的外层关系的指定排序一起使用的归并子句。
 *	  若成功，返回归并子句的列表。
 *
 * 'pathkeys' 是显示外层关系路径排序的路径键列表。
 * 'restrictinfos' 是正在形成的连接关系的可归并限制子句列表，
 *			没有特定顺序。
 *
 * restrictinfos 必须已标记（经由 outer_is_left），以表明每条子句的哪一侧
 * 与当前外层路径相关联。（见
 * select_mergejoin_clauses()）
 *
 * 若无法做归并，结果为 NIL；否则是可用归并子句的
 * 最大列表（表示为其 restrictinfo 节点的列表）。
 * 该列表按路径键排序，这是执行所需要的。
 */
List *
find_mergeclauses_for_outer_pathkeys(PlannerInfo *root,
									 List *pathkeys,
									 List *restrictinfos)
{
	List	   *mergeclauses = NIL;
	ListCell   *i;

	/* 确保子句中已缓存 eclasses */
	foreach(i, restrictinfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

		update_mergeclause_eclasses(root, rinfo);
	}

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
		List	   *matched_restrictinfos = NIL;
		ListCell   *j;

		/*----------
		 * A mergejoin clause matches a pathkey if it has the same EC.
		 * If there are multiple matching clauses, take them all.  In plain
		 * inner-join scenarios we expect only one match, because
		 * equivalence-class processing will have removed any redundant
		 * mergeclauses.  However, in outer-join scenarios there might be
		 * multiple matches.  An example is
		 *
		 *	select * from a full join b
		 *		on a.v1 = b.v1 and a.v2 = b.v2 and a.v1 = b.v2;
		 *
		 * Given the pathkeys ({a.v1}, {a.v2}) it is okay to return all three
		 * clauses (in the order a.v1=b.v1, a.v1=b.v2, a.v2=b.v2) and indeed
		 * we *must* do so or we will be unable to form a valid plan.
		 *
		 * We expect that the given pathkeys list is canonical, which means
		 * no two members have the same EC, so it's not possible for this
		 * code to enter the same mergeclause into the result list twice.
		 *
		 * It's possible that multiple matching clauses might have different
		 * ECs on the other side, in which case the order we put them into our
		 * result makes a difference in the pathkeys required for the inner
		 * input rel.  However this routine hasn't got any info about which
		 * order would be best, so we don't worry about that.
		 *
		 * It's also possible that the selected mergejoin clauses produce
		 * a noncanonical ordering of pathkeys for the inner side, ie, we
		 * might select clauses that reference b.v1, b.v2, b.v1 in that
		 * order.  This is not harmful in itself, though it suggests that
		 * the clauses are partially redundant.  Since the alternative is
		 * to omit mergejoin clauses and thereby possibly fail to generate a
		 * plan altogether, we live with it.  make_inner_pathkeys_for_merge()
		 * has to delete duplicates when it constructs the inner pathkeys
		 * list, and we also have to deal with such cases specially in
		 * create_mergejoin_plan().
		 *----------
		 */
		foreach(j, restrictinfos)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);
			EquivalenceClass *clause_ec;

			clause_ec = rinfo->outer_is_left ?
				rinfo->left_ec : rinfo->right_ec;
			if (clause_ec == pathkey_ec)
				matched_restrictinfos = lappend(matched_restrictinfos, rinfo);
		}

		/*
		 * 若没找到归并子句，就结束---路径键中任何更多的
		 * 排序键位置都没用。（但只要找到至少一条归并子句，
		 * 仍可以做归并连接。）
		 */
		if (matched_restrictinfos == NIL)
			break;

		/*
		 * 若确实为这个排序键位置找到了可用的 mergeclause(s)，
		 * 就把它们加入结果列表。
		 */
		mergeclauses = list_concat(mergeclauses, matched_restrictinfos);
	}

	return mergeclauses;
}

/*
 * select_outer_pathkeys_for_merge
 *	  构造表示一种可能排序顺序的路径键列表，
 *	  可与给定的归并子句一起使用。
 *
 * 'mergeclauses' 是归并连接子句的 RestrictInfos 列表，
 *			这些子句将用于归并连接。
 * 'joinrel' 是我们试图构造的连接关系。
 *
 * restrictinfos 必须已标记（经由 outer_is_left），以表明
 * 每条子句的哪一侧与当前外层路径相关联。（见
 * select_mergejoin_clauses()）
 *
 * 返回可应用于外层关系的路径键列表。
 *
 * 既然这里假定需要排序，匹配外层关系任何已有排序
 * 就没有特别用处。（joinpath.c 另有一条
 * 完全独立的代码路径，用来考虑免排序的归并连接。）相反，
 * 有意义的是尝试匹配所请求的 query_pathkeys，
 * 或其一个前缀，以便避免第二次输出排序，或改为做
 * 增量排序。当前缀覆盖整个连接条件时，
 * 只取 query_pathkeys 的一个前缀就可以。
 * 若做不到，就试着把 "更常见" 的键（未匹配的
 * EquivalenceClass 同伴最多的那些）列在前面，希望所得
 * 排序对尽可能多的更高层归并连接有用。
 */
List *
select_outer_pathkeys_for_merge(PlannerInfo *root,
								List *mergeclauses,
								RelOptInfo *joinrel)
{
	List	   *pathkeys = NIL;
	int			nClauses = list_length(mergeclauses);
	EquivalenceClass **ecs;
	int		   *scores;
	int			necs;
	ListCell   *lc;
	int			j;

	/* 也许没有归并子句 */
	if (nClauses == 0)
		return NIL;

	/*
	 * 为归并子句所用的 EC 做数组（去掉
	 * 重复项）并记录它们的 "常见度" 得分。
	 */
	ecs = (EquivalenceClass **) palloc(nClauses * sizeof(EquivalenceClass *));
	scores = (int *) palloc(nClauses * sizeof(int));
	necs = 0;

	foreach(lc, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		int			score;
		ListCell   *lc2;

		/* 取得外层 eclass */
		update_mergeclause_eclasses(root, rinfo);

		if (rinfo->outer_is_left)
			oeclass = rinfo->left_ec;
		else
			oeclass = rinfo->right_ec;

		/* 拒绝重复项 */
		for (j = 0; j < necs; j++)
		{
			if (ecs[j] == oeclass)
				break;
		}
		if (j < necs)
			continue;

		/* 计算得分 */
		score = 0;
		foreach(lc2, oeclass->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);

			/* 潜在的未来连接伙伴？ */
			if (!em->em_is_const && !em->em_is_child &&
				!bms_overlap(em->em_relids, joinrel->relids))
				score++;
		}

		ecs[necs] = oeclass;
		scores[necs] = score;
		necs++;
	}

	/*
	 * 弄清是否拥有 query_pathkeys 中提到的全部 EC；若是，就可以
	 * 生成对最终输出也有用的排序。若只
	 * 拥有 query_pathkeys 的一个前缀，且该前缀就是整个
	 * 连接条件，则用该前缀作为路径键是有用的，因为
	 * 这增加了上层规划器能够使用
	 * 增量排序的机会。
	 */
	if (root->query_pathkeys)
	{
		int			matches = 0;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *query_pathkey = (PathKey *) lfirst(lc);
			EquivalenceClass *query_ec = query_pathkey->pk_eclass;

			for (j = 0; j < necs; j++)
			{
				if (ecs[j] == query_ec)
					break;		/* 找到了匹配 */
			}
			if (j >= necs)
				break;			/* 没找到匹配 */

			matches++;
		}
		/* 若走到列表末尾，就全部都有了 */
		if (lc == NULL)
		{
			/* 把 query_pathkeys 复制为我们输出的起点 */
			pathkeys = list_copy(root->query_pathkeys);
			/* 把它们的 EC 标为已经发出 */
			foreach(lc, root->query_pathkeys)
			{
				PathKey    *query_pathkey = (PathKey *) lfirst(lc);
				EquivalenceClass *query_ec = query_pathkey->pk_eclass;

				for (j = 0; j < necs; j++)
				{
					if (ecs[j] == query_ec)
					{
						scores[j] = -1;
						break;
					}
				}
			}
		}

		/*
		 * 若没有匹配全部 query_pathkeys，但匹配了
		 * 全部连接子句，我们仍会使用这些，因为部分
		 * 排序的输入对上层规划器总比没有好，它也许
		 * 会做增量排序而不是全排序。
		 */
		else if (matches == nClauses)
		{
			pathkeys = list_copy_head(root->query_pathkeys, matches);

			/* 已经有了全部连接路径键，因此没有更多要做的 */
			pfree(ecs);
			pfree(scores);

			return pathkeys;
		}
	}

	/*
	 * 按常见度顺序把剩余 EC 加入列表，使用默认排序
	 * 方向。（这里可以用 qsort()，但列表长度通常
	 * 太小，不值得。）
	 */
	for (;;)
	{
		int			best_j;
		int			best_score;
		EquivalenceClass *ec;
		PathKey    *pathkey;

		best_j = 0;
		best_score = scores[0];
		for (j = 1; j < necs; j++)
		{
			if (scores[j] > best_score)
			{
				best_j = j;
				best_score = scores[j];
			}
		}
		if (best_score < 0)
			break;				/* 全部完成 */
		ec = ecs[best_j];
		scores[best_j] = -1;
		pathkey = make_canonical_pathkey(root,
										 ec,
										 linitial_oid(ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		/* 不可能冗余，因为没有重复的 EC */
		Assert(!pathkey_is_redundant(pathkey, pathkeys));
		pathkeys = lappend(pathkeys, pathkey);
	}

	pfree(ecs);
	pfree(scores);

	return pathkeys;
}

/*
 * make_inner_pathkeys_for_merge
 *	  构造表示必须施加于内层路径的显式排序的路径键列表，
 *	  该排序使内层路径能够与
 *	  给定的归并子句一起使用。
 *
 * 'mergeclauses' 是归并连接子句的 RestrictInfos 列表，
 *			将按顺序用于归并连接。
 * 'outer_pathkeys' 是外层一侧已经知道的规范路径键，
 *			即该连接的外侧。
 *
 * restrictinfos 必须已标记（经由 outer_is_left），以表明
 * 每条子句的哪一侧与当前外层路径相关联。（见
 * select_mergejoin_clauses()）
 *
 * 返回可应用于内层关系的路径键列表。
 *
 * 注意，判断某个输入路径是否真的需要排序
 * 不是本例程的事。假定需要排序；
 * 把键造出来就行，嗯？
 */
List *
make_inner_pathkeys_for_merge(PlannerInfo *root,
							  List *mergeclauses,
							  List *outer_pathkeys)
{
	List	   *pathkeys = NIL;
	EquivalenceClass *lastoeclass;
	PathKey    *opathkey;
	ListCell   *lc;
	ListCell   *lop;

	lastoeclass = NULL;
	opathkey = NULL;
	lop = list_head(outer_pathkeys);

	foreach(lc, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *pathkey;

		update_mergeclause_eclasses(root, rinfo);

		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}

		/* 外层 eclass 应当匹配当前或下一个路径键 */
		/* 为了调试，我们仔细检查这一点 */
		if (oeclass != lastoeclass)
		{
			if (!lop)
				elog(ERROR, "too few pathkeys for mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			lop = lnext(outer_pathkeys, lop);
			lastoeclass = opathkey->pk_eclass;
			if (oeclass != lastoeclass)
				elog(ERROR, "outer pathkeys do not match mergeclause");
		}

		/*
		 * 两侧常常是同一个 EC，此时外层
		 * 路径键对内层一侧也是规范的，可以跳过
		 * 一次无用的查找。
		 */
		if (ieclass == oeclass)
			pathkey = opathkey;
		else
			pathkey = make_canonical_pathkey(root,
											 ieclass,
											 opathkey->pk_opfamily,
											 opathkey->pk_strategy,
											 opathkey->pk_nulls_first);

		/*
		 * 不要生成冗余路径键（多条
		 * 归并子句引用同一 EC 时会发生）。因为我们这样做，输出的
		 * 路径键列表不一定按归并子句的顺序，这让
		 * create_mergejoin_plan() 更麻烦。但若不这样做，
		 * 就会有非规范的排序键列表，那很糟；原因之一是，
		 * 它肯定匹配不上输入关系任何可用的
		 * 排序顺序。
		 */
		if (!pathkey_is_redundant(pathkey, pathkeys))
			pathkeys = lappend(pathkeys, pathkey);
	}

	return pathkeys;
}

/*
 * trim_mergeclauses_for_inner_pathkeys
 *	  本例程把归并子句列表修剪到只包含那些能与
 *	  连接的内层关系的指定排序一起工作的子句。
 *
 * 'mergeclauses' 是正在形成的连接关系的归并连接子句的 RestrictInfos 列表，
 *			其顺序已知适用于
 *			当前考虑的、该连接外层关系的排序。
 * 'pathkeys' 是显示内层关系路径排序的路径键列表；
 *			它应当等于这些归并子句的
 *			make_inner_pathkeys_for_merge 结果，或是其截断。
 *
 * 返回的将是给定归并子句列表的一个前缀。
 *
 * 需要这个逻辑，是因为 make_inner_pathkeys_for_merge 的结果
 * 不一定与归并子句同序。这意味着若我们
 * 考虑的内层关系路径键列表是该结果的截断，
 * 即使归并子句匹配一个仍保留的内层
 * 路径键，也许仍要丢掉它们。当它们位于一条匹配
 * 已去掉的内层路径键的归并子句的右边时，就会这样。
 *
 * 归并子句必须已标记（经由 outer_is_left），以表明每条子句的哪一侧
 * 与当前外层路径相关联。（见
 * select_mergejoin_clauses()）
 */
List *
trim_mergeclauses_for_inner_pathkeys(PlannerInfo *root,
									 List *mergeclauses,
									 List *pathkeys)
{
	List	   *new_mergeclauses = NIL;
	PathKey    *pathkey;
	EquivalenceClass *pathkey_ec;
	bool		matched_pathkey;
	ListCell   *lip;
	ListCell   *i;

	/* 没有路径键 => 没有归并子句（尽管我们不预期这种情形） */
	if (pathkeys == NIL)
		return NIL;
	/* 初始化为考虑第一个路径键 */
	lip = list_head(pathkeys);
	pathkey = (PathKey *) lfirst(lip);
	pathkey_ec = pathkey->pk_eclass;
	lip = lnext(pathkeys, lip);
	matched_pathkey = false;

	/* 扫描归并子句，看能用多少条 */
	foreach(i, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);
		EquivalenceClass *clause_ec;

		/* 假定这里不必再做 update_mergeclause_eclasses */

		/* 把子句的内层关系 EC 与当前路径键比较 */
		clause_ec = rinfo->outer_is_left ?
			rinfo->right_ec : rinfo->left_ec;

		/* 若没有匹配，尝试前进到下一个路径键 */
		if (clause_ec != pathkey_ec)
		{
			/* 若没有子句匹配这个内层路径键，必须停止 */
			if (!matched_pathkey)
				break;

			/* 若还有，前进到下一个内层路径键 */
			if (lip == NULL)
				break;
			pathkey = (PathKey *) lfirst(lip);
			pathkey_ec = pathkey->pk_eclass;
			lip = lnext(pathkeys, lip);
			matched_pathkey = false;
		}

		/* 若归并子句匹配当前内层路径键，就可以使用它 */
		if (clause_ec == pathkey_ec)
		{
			new_mergeclauses = lappend(new_mergeclauses, rinfo);
			matched_pathkey = true;
		}
		else
		{
			/* 否则，没有希望再加入更多归并子句 */
			break;
		}
	}

	return new_mergeclauses;
}


/****************************************************************************
 *		PATHKEY USEFULNESS CHECKS（路径键有用性检查）
 *
 * 我们只想记住一条路径的路径键中那些有某种
 * 潜在用途的，无论是用于后续归并连接，还是用于满足查询
 * 所请求的输出排序。这确保 add_path() 不会认为
 * 一条路径具有有用的不同排序，除非它真的有用。
 * 这些例程检查给定路径键是否有用。
 ****************************************************************************/

/*
 * pathkeys_useful_for_merging
 *		统计对该关系之上的归并连接可能有用的
 *		路径键个数。
 *
 * 若路径键对应该关系任何连接子句任一侧的归并
 * 排序，就认为它可能有用。这也许
 * 过于乐观，因为需要不同其他关系的连接子句
 * 也许永远无法同时使用，但试图精确
 * 很可能得不偿失。
 *
 * 为避免把考虑的归并连接路径数加倍，我们希望
 * 对任何给定目标列，只把两个扫描方向（ASC 或 DESC）中的一个
 * 视为对归并有用。除非
 * 其中一个方向恰好匹配 ORDER BY 键，否则这个选择是任意的；若匹配，
 * 就应当优先那个方向，希望避免最后的排序步骤。
 * right_merge_direction() 实现了这个启发式。
 */
static int
pathkeys_useful_for_merging(PlannerInfo *root, RelOptInfo *rel, List *pathkeys)
{
	int			useful = 0;
	ListCell   *i;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		bool		matched = false;
		ListCell   *j;

		/* 若方向 "不对"，则对归并没有用 */
		if (!right_merge_direction(root, pathkey))
			break;

		/*
		 * 先查看路径键的 EquivalenceClass，看是否
		 * 还有尚未连到该关系的成员。若有，
		 * 肯定可以用它们生成归并连接子句。
		 */
		if (rel->has_eclass_joins &&
			eclass_useful_for_merging(root, pathkey->pk_eclass, rel))
			matched = true;
		else
		{
			/*
			 * 否则搜索该关系的 joininfo 列表，其中包含
			 * 不能由 EquivalenceClass 导出、但也许
			 * 仍然可归并的连接子句。
			 */
			foreach(j, rel->joininfo)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(j);

				if (restrictinfo->mergeopfamilies == NIL)
					continue;
				update_mergeclause_eclasses(root, restrictinfo);

				if (pathkey->pk_eclass == restrictinfo->left_ec ||
					pathkey->pk_eclass == restrictinfo->right_ec)
				{
					matched = true;
					break;
				}
			}
		}

		/*
		 * 若没找到归并子句，就结束---路径键中任何更多的
		 * 排序键位置都没用。（但只要找到至少一条归并子句，
		 * 仍可以做归并连接。）
		 */
		if (matched)
			useful++;
		else
			break;
	}

	return useful;
}

/*
 * right_merge_direction
 *		检查该路径键是否体现了对其目标列做归并时
 *		所偏好的排序方向。
 */
static bool
right_merge_direction(PlannerInfo *root, PathKey *pathkey)
{
	ListCell   *l;

	foreach(l, root->query_pathkeys)
	{
		PathKey    *query_pathkey = (PathKey *) lfirst(l);

		if (pathkey->pk_eclass == query_pathkey->pk_eclass &&
			pathkey->pk_opfamily == query_pathkey->pk_opfamily)
		{
			/*
			 * 找到了匹配的查询排序列。当且仅当方向匹配时，
			 * 才偏好这个路径键的方向。注意我们忽略 pk_nulls_first，
			 * 这意味着也许仍然需要排序……但我们仍
			 * 希望只偏好两种可能方向中的一种，并且
			 * 不妨就用这一种。
			 */
			return (pathkey->pk_strategy == query_pathkey->pk_strategy);
		}
	}

	/* 若没有匹配的 ORDER BY 请求，则偏好 ASC 方向 */
	return (pathkey->pk_strategy == BTLessStrategyNumber);
}

/*
 * pathkeys_useful_for_ordering
 *		统计对满足查询所请求的
 *		输出排序有用的路径键个数。
 *
 * 因为有可能做增量排序，键的一个前缀列表
 * 可能有助于改善所请求
 * 排序的性能。因此若找不到有价值的键就返回 0，否则返回该列表与所请求排序
 * 共享的前导键个数。
 */
static int
pathkeys_useful_for_ordering(PlannerInfo *root, List *pathkeys)
{
	int			n_common_pathkeys;

	if (root->query_pathkeys == NIL)
		return 0;				/* 没有请求特殊排序 */

	if (pathkeys == NIL)
		return 0;				/* 无序路径 */

	(void) pathkeys_count_contained_in(root->query_pathkeys, pathkeys,
									   &n_common_pathkeys);

	return n_common_pathkeys;
}

/*
 * truncate_useless_pathkeys
 *		把给定路径键列表缩短到只保留有用的路径键。
 */
List *
truncate_useless_pathkeys(PlannerInfo *root,
						  RelOptInfo *rel,
						  List *pathkeys)
{
	int			nuseful;
	int			nuseful2;

	nuseful = pathkeys_useful_for_merging(root, rel, pathkeys);
	nuseful2 = pathkeys_useful_for_ordering(root, pathkeys);
	if (nuseful2 > nuseful)
		nuseful = nuseful2;

	/*
	 * 注意：破坏性地修改输入列表不安全，但若实际上不会
	 * 改动它，就可以避免复制该列表
	 */
	if (nuseful == 0)
		return NIL;
	else if (nuseful == list_length(pathkeys))
		return pathkeys;
	else
		return list_copy_head(pathkeys, nuseful);
}

/*
 * has_useful_pathkeys
 *		检测指定关系是否可能有任何按
 *		truncate_useless_pathkeys() 来说有用的路径键。
 *
 * 这是一项便宜的测试，让我们在非常
 * 简单的查询中可以完全跳过构造路径键。可以错误地在其实没有可用路径键时返回 "true"，
 * 但往另一个方向出错
 * 就糟了---因此要与上面的例程保持同步！
 *
 * 可以把测试做得更复杂，例如检查是否有
 * 连接子句真的可归并，但那很可能不会
 * 经常划算到能抵消额外周期。不过，既无连接也无
 * 排序的查询相当常见，因此做到这一步似乎值得。
 */
bool
has_useful_pathkeys(PlannerInfo *root, RelOptInfo *rel)
{
	if (rel->joininfo != NIL || rel->has_eclass_joins)
		return true;			/* 也许能用路径键来做归并 */
	if (root->query_pathkeys != NIL)
		return true;			/* 也许能用它们来做排序 */
	return false;				/* 肯定没用 */
}
