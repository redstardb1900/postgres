/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indexes are usable for scanning a
 *	  given relation, and create Paths accordingly.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/indxpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_am.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/* XXX 见 PartCollMatchesExprColl */
#define IndexCollMatchesExprColl(idxcollation, exprcollation) \
	((idxcollation) == InvalidOid || (idxcollation) == (exprcollation))

/* 我们在找普通索引扫描、位图扫描，还是两者皆可 */
typedef enum
{
	ST_INDEXSCAN,				/* 必须支持 amgettuple */
	ST_BITMAPSCAN,				/* 必须支持 amgetbitmap */
	ST_ANYSCAN					/* 两者皆可 */
} ScanTypeControl;

/* 收集与索引匹配的限定子句的数据结构 */
typedef struct
{
	bool		nonempty;		/* 若列表并非全空则为真 */
	/* IndexClause 节点的列表，每个索引列一份 */
	List	   *indexclauses[INDEX_MAX_KEYS];
} IndexClauseSet;

/* choose_bitmap_and() 内部使用的每条路径数据 */
typedef struct
{
	Path	   *path;			/* IndexPath、BitmapAndPath 或 BitmapOrPath */
	List	   *quals;			/* 它使用的 WHERE 子句 */
	List	   *preds;			/* 其部分索引的谓词 index(es) */
	Bitmapset  *clauseids;		/* 限定条件加谓词，表示为一个 bitmapset */
	bool		unclassifiable; /* 限定条件加谓词是否多到无法处理？ */
} PathClauseUsage;

/* ec_member_matches_indexcol 的回调参数 */
typedef struct
{
	IndexOptInfo *index;		/* 正在考虑的索引 */
	int			indexcol;		/* 想要匹配的索引列 */
} ec_member_matches_arg;


static void consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
										IndexOptInfo *index,
										IndexClauseSet *rclauseset,
										IndexClauseSet *jclauseset,
										IndexClauseSet *eclauseset,
										List **bitindexpaths);
static void consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
										   IndexOptInfo *index,
										   IndexClauseSet *rclauseset,
										   IndexClauseSet *jclauseset,
										   IndexClauseSet *eclauseset,
										   List **bitindexpaths,
										   List *indexjoinclauses,
										   int considered_clauses,
										   List **considered_relids);
static void get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
								 IndexOptInfo *index,
								 IndexClauseSet *rclauseset,
								 IndexClauseSet *jclauseset,
								 IndexClauseSet *eclauseset,
								 List **bitindexpaths,
								 Relids relids,
								 List **considered_relids);
static bool eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
								List *indexjoinclauses);
static void get_index_paths(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index, IndexClauseSet *clauses,
							List **bitindexpaths);
static List *build_index_paths(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index, IndexClauseSet *clauses,
							   bool useful_predicate,
							   ScanTypeControl scantype,
							   bool *skip_nonnative_saop,
							   bool *skip_lower_saop);
static List *build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
								List *clauses, List *other_clauses);
static List *generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
									  List *clauses, List *other_clauses);
static Path *choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel,
							   List *paths);
static int	path_usage_comparator(const void *a, const void *b);
static Cost bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel,
								 Path *ipath);
static Cost bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel,
								List *paths);
static PathClauseUsage *classify_index_clause_usage(Path *path,
													List **clauselist);
static void find_indexpath_quals(Path *bitmapqual, List **quals, List **preds);
static int	find_list_position(Node *node, List **nodelist);
static bool check_index_only(RelOptInfo *rel, IndexOptInfo *index);
static double get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids);
static double adjust_rowcount_for_semijoins(PlannerInfo *root,
											Index cur_relid,
											Index outer_relid,
											double rowcount);
static double approximate_joinrel_size(PlannerInfo *root, Relids relids);
static void match_restriction_clauses_to_index(PlannerInfo *root,
											   IndexOptInfo *index,
											   IndexClauseSet *clauseset);
static void match_join_clauses_to_index(PlannerInfo *root,
										RelOptInfo *rel, IndexOptInfo *index,
										IndexClauseSet *clauseset,
										List **joinorclauses);
static void match_eclass_clauses_to_index(PlannerInfo *root,
										  IndexOptInfo *index,
										  IndexClauseSet *clauseset);
static void match_clauses_to_index(PlannerInfo *root,
								   List *clauses,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset);
static void match_clause_to_index(PlannerInfo *root,
								  RestrictInfo *rinfo,
								  IndexOptInfo *index,
								  IndexClauseSet *clauseset);
static IndexClause *match_clause_to_indexcol(PlannerInfo *root,
											 RestrictInfo *rinfo,
											 int indexcol,
											 IndexOptInfo *index);
static bool IsBooleanOpfamily(Oid opfamily);
static IndexClause *match_boolean_index_clause(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol, IndexOptInfo *index);
static IndexClause *match_opclause_to_indexcol(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol,
											   IndexOptInfo *index);
static IndexClause *match_funcclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *get_index_clause_from_support(PlannerInfo *root,
												  RestrictInfo *rinfo,
												  Oid funcid,
												  int indexarg,
												  int indexcol,
												  IndexOptInfo *index);
static IndexClause *match_saopclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *match_rowcompare_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *expand_indexqual_rowcompare(PlannerInfo *root,
												RestrictInfo *rinfo,
												int indexcol,
												IndexOptInfo *index,
												Oid expr_op,
												bool var_on_left);
static void match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
									List **orderby_clauses_p,
									List **clause_columns_p);
static Expr *match_clause_to_ordering_op(IndexOptInfo *index,
										 int indexcol, Expr *clause, Oid pk_opfamily);
static bool ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
									   EquivalenceClass *ec, EquivalenceMember *em,
									   void *arg);


/*
 * create_index_paths()
 *	  为给定关系生成所有有意义的索引路径。
 *	  候选路径用 add_path 加入该关系的 pathlist。
 *
 * 要被考虑做索引扫描，索引必须匹配一条或多条
 * 来自查询限定条件的限制子句或连接子句，
 * 或者匹配查询的 ORDER BY 条件，或者有一个谓词
 * 匹配查询的限定条件。
 *
 * 索引扫描有两种基本类型。"普通" 索引扫描在其 indexqual 中
 * 只用限制子句（也许一条都没有），
 * 因此可以在任何上下文中使用。"参数化" 索引扫描在其 indexqual 中使用
 * 连接子句（若有，再加上限制子句）。
 * 把这样的扫描连到提供其 indexqual 中其他
 * 变量的某个关系时，参数化扫描必须作为
 * 嵌套循环连接的内层关系出现；它不能用在外层一侧，
 * 也不能用在归并或哈希连接中。在那种上下文里，其他关系
 * 属性的值在对该索引路径的任何一次扫描期间都是可用且固定的。
 *
 * 为每条普通或
 * 参数化索引扫描生成 IndexPath 并交给 add_path()，只要本例程认为它对
 * 当前查询可能有意义。
 *
 * 'rel' 是我们想为其生成索引路径的关系
 *
 * 注意：必须已经对该关系运行过 check_index_predicates()。
 *
 * 注意：若关系的目标列表中有 LATERAL 引用，
 * rel->lateral_relids 可能非空。目前，我们把
 * lateral_relids 纳入每条路径所报告的参数化，但并不
 * 另作考虑。这些关系 *必须* 能作为
 * 参数来源这一事实，也许应当影响我们对
 * 索引限定条件的选择……但目前似乎不值得为此费心。
 * 尤其是，下面关于 "未参数化" 路径的说法应当理解为
 * "就 indexqual 而言尚未参数化"。
 */
void
create_index_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexpaths;
	List	   *bitindexpaths;
	List	   *bitjoinpaths;
	List	   *joinorclauses;
	IndexClauseSet rclauseset;
	IndexClauseSet jclauseset;
	IndexClauseSet eclauseset;
	ListCell   *lc;

	/* 若没有索引，就整个跳过 */
	if (rel->indexlist == NIL)
		return;

	/* 位图路径先收集起来，最后再处理 */
	bitindexpaths = bitjoinpaths = joinorclauses = NIL;

	/* 逐个检查每个索引 */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		/* 保护 IndexClauseSets 中大小有限的数组 */
		Assert(index->nkeycolumns <= INDEX_MAX_KEYS);

		/*
		 * 忽略与查询不匹配的部分索引。
		 * （generate_bitmap_or_paths() 也许能对它们做点什么，
		 * 但这里不必关心。）
		 */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/*
		 * 找出能匹配该索引的限制子句。
		 */
		MemSet(&rclauseset, 0, sizeof(rclauseset));
		match_restriction_clauses_to_index(root, index, &rclauseset);

		/*
		 * 用限制子句建立索引路径。这些将是
		 * 未参数化的路径。普通路径直接交给 add_path()，
		 * 位图路径加入 bitindexpaths，留到下面处理。
		 */
		get_index_paths(root, rel, index, &rclauseset,
						&bitindexpaths);

		/*
		 * 找出能匹配该索引的连接子句。目前
		 * 把它们与限制子句分开。注意这一步
		 * 只找到尚未并入
		 * EquivalenceClasses 的 "松散" 连接子句。另外，收集连接 OR 子句供以后使用。
		 */
		MemSet(&jclauseset, 0, sizeof(jclauseset));
		match_join_clauses_to_index(root, rel, index,
									&jclauseset, &joinorclauses);

		/*
		 * 寻找 EquivalenceClasses，它们能生成与
		 * 该索引匹配的连接子句。
		 */
		MemSet(&eclauseset, 0, sizeof(eclauseset));
		match_eclass_clauses_to_index(root, index,
									  &eclauseset);

		/*
		 * 若找到任何普通或等价类连接子句，就用它们建立参数化
		 * 索引路径。
		 */
		if (jclauseset.nonempty || eclauseset.nonempty)
			consider_index_join_clauses(root, rel, index,
										&rclauseset,
										&jclauseset,
										&eclauseset,
										&bitjoinpaths);
	}

	/*
	 * 为限制列表中任何合适的 OR 子句生成 BitmapOrPaths。
	 * 把它们加入 bitindexpaths。
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
										  rel->baserestrictinfo, NIL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * 同样，为连接子句列表中任何合适的 OR 子句生成 BitmapOrPaths。
	 * 把它们加入 bitjoinpaths。
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
										  joinorclauses, rel->baserestrictinfo);
	bitjoinpaths = list_concat(bitjoinpaths, indexpaths);

	/*
	 * 若找到任何可用的东西，就为最有希望的
	 * 限制位图索引路径组合生成一条 BitmapHeapPath。注意无论有多少索引，
	 * 这样的路径都只有一条。
	 * 这应当够了，因为这种路径基本上只有一个优劣指标
	 * （总代价）。
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual;
		BitmapHeapPath *bpath;

		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths);
		bpath = create_bitmap_heap_path(root, rel, bitmapqual,
										rel->lateral_relids, 1.0, 0);
		add_path(rel, (Path *) bpath);

		/* 创建一条部分位图堆路径 */
		if (rel->consider_parallel && rel->lateral_relids == NULL)
			create_partial_bitmap_paths(root, rel, bitmapqual);
	}

	/*
	 * 同样，若找到任何可用的东西，就为最有希望的
	 * 连接位图索引路径组合生成 BitmapHeapPaths。策略是
	 * 为可用位图索引路径中见到的每种不同参数化
	 * 生成一条这样的路径。这看起来可能
	 * 很贵，但通常不会有很多不同的
	 * 参数化。（这段逻辑与
	 * consider_index_join_clauses 很像，只是我们处理的是整条路径，不是
	 * 单条子句。）
	 */
	if (bitjoinpaths != NIL)
	{
		List	   *all_path_outers;

		/* 找出 bitjoinpaths 中见到的每种不同参数化 */
		all_path_outers = NIL;
		foreach(lc, bitjoinpaths)
		{
			Path	   *path = (Path *) lfirst(lc);
			Relids		required_outer = PATH_REQ_OUTER(path);

			all_path_outers = list_append_unique(all_path_outers,
												 required_outer);
		}

		/* 现在，对每一组不同的参数化…… */
		foreach(lc, all_path_outers)
		{
			Relids		max_outers = (Relids) lfirst(lc);
			List	   *this_path_set;
			Path	   *bitmapqual;
			Relids		required_outer;
			double		loop_count;
			BitmapHeapPath *bpath;
			ListCell   *lcp;

			/* 找出所需不超过该参数化的全部位图连接路径 */
			this_path_set = NIL;
			foreach(lcp, bitjoinpaths)
			{
				Path	   *path = (Path *) lfirst(lcp);

				if (bms_is_subset(PATH_REQ_OUTER(path), max_outers))
					this_path_set = lappend(this_path_set, path);
			}

			/*
			 * 加入限制位图路径，因为它们可以与任何
			 * 连接路径一起使用。
			 */
			this_path_set = list_concat(this_path_set, bitindexpaths);

			/* 为这种参数化选出最佳 AND 组合 */
			bitmapqual = choose_bitmap_and(root, rel, this_path_set);

			/* 并把那条路径推进去 */
			required_outer = PATH_REQ_OUTER(bitmapqual);
			loop_count = get_loop_count(root, rel->relid, required_outer);
			bpath = create_bitmap_heap_path(root, rel, bitmapqual,
											required_outer, loop_count, 0);
			add_path(rel, (Path *) bpath);
		}
	}
}

/*
 * consider_index_join_clauses
 *	  给定一个索引的几组连接子句，决定要建立哪些参数化
 *	  索引路径。
 *
 * 普通索引路径直接交给 add_path，潜在的
 * 位图索引路径则加入 *bitindexpaths，留待以后处理。
 *
 * 'rel' 是该索引的堆关系
 * 'index' 是我们想为其生成路径的索引
 * 'rclauseset' 是可索引限制子句的集合
 * 'jclauseset' 是可索引的简单连接子句集合
 * 'eclauseset' 是来自 EquivalenceClasses 的可索引子句集合
 * '*bitindexpaths' 是要加入位图路径的列表
 */
static void
consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index,
							IndexClauseSet *rclauseset,
							IndexClauseSet *jclauseset,
							IndexClauseSet *eclauseset,
							List **bitindexpaths)
{
	int			considered_clauses = 0;
	List	   *considered_relids = NIL;
	int			indexcol;

	/*
	 * 这里的策略是找出每一组可能有用的、能提供可索引连接子句的外层
	 * 关系。对每一组，
	 * 选出从这些外层关系得到的全部连接子句，再加上全部
	 * 可索引限制子句，并为这组子句生成普通和/或位图
	 * 索引路径。这基于如下假定：
	 * 把子句用作索引限定条件总是比用作
	 * 过滤器（qpqual）更好；若从索引限定条件中省掉一条可用子句，它就会
	 * 被当作过滤器来用。
	 *
	 * 这看起来很贵，但在多数实际情形里，要考虑的不同外层关系集合
	 * 不会很多。当并非如此时，用一条启发式作为安全阀：
	 * 把考虑的外层关系集合数限制为所考虑子句数的某个倍数。（不过
	 * 总会考虑单独使用每一条连接子句。）
	 * 为了选择相关子句时简单些，我们把每一组
	 *
	 * 外层关系表示为一组最大的 clause_relids --- 也就是说，被索引的
	 * 关系本身也包含在 relid 集合里。
	 * considered_relids
	 * 列出已经试过的全部 relid 集合。
	 */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		/* 考虑每一条适用的简单连接子句 */
		considered_clauses += list_length(jclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   jclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
		/* 考虑每一条适用的等价类连接子句 */
		considered_clauses += list_length(eclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   eclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
	}
}

/*
 * consider_index_join_outer_rels
 *	  根据子句列表中的子句 relid 生成参数化路径。
 *
 * consider_index_join_clauses 的主力；理由见那里的说明。
 *
 * 'rel'、'index'、'rclauseset'、'jclauseset'、'eclauseset' 以及
 *		'bitindexpaths' 同上
 * 'indexjoinclauses' 是连接子句的 IndexClauses 列表
 * 'considered_clauses' 是迄今考虑过的子句总数
 * '*considered_relids' 是已经考虑过的全部 relid 集合的列表
 */
static void
consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index,
							   IndexClauseSet *rclauseset,
							   IndexClauseSet *jclauseset,
							   IndexClauseSet *eclauseset,
							   List **bitindexpaths,
							   List *indexjoinclauses,
							   int considered_clauses,
							   List **considered_relids)
{
	ListCell   *lc;

	/* 检查给定列表中每条连接子句的 relid */
	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		Relids		clause_relids = iclause->rinfo->clause_relids;
		EquivalenceClass *parent_ec = iclause->rinfo->parent_ec;
		int			num_considered_relids;

		/* 若已经试过它的 relid 集合，就不必再做 */
		if (list_member(*considered_relids, clause_relids))
			continue;

		/*
		 * 生成该子句的 relid 集合与每个
		 * 先前试过的集合的并集。这样就能把该子句与
		 * 先前子句的每个有意义子集一起试。不过，为避免
		 * 子句很多时规划时间指数增长，
		 * 把接受的 relid 集合数限制为 10 * considered_clauses。
		 *
		 * 注意：get_join_index_paths 会把项追加到 *considered_relids，
		 * 但本循环不需要访问这些新加入的项，
		 * 因此这里不用 foreach()。若去访问也没有真正的害处，
		 * 因为子集检查会拒绝它们；但
		 * 会浪费一些周期。
		 */
		num_considered_relids = list_length(*considered_relids);
		for (int pos = 0; pos < num_considered_relids; pos++)
		{
			Relids		oldrelids = (Relids) list_nth(*considered_relids, pos);

			/*
			 * 若其中一个是另一个的子集，就不可能有新集合。
			 * 这不是对冗余的完整测试，但简单而且
			 * 便宜。若已经生成过同一 relid 集合，get_join_index_paths 会
			 * 更仔细地检查。
			 */
			if (bms_subset_compare(clause_relids, oldrelids) != BMS_DIFFERENT)
				continue;

			/*
			 * 若该子句派生自某个等价类，子句
			 * 列表里也许还有派生自同一
			 * 等价类的其他子句。不应当认为把该子句与
			 * 那些子句之一组合会产生有用的不同
			 * 参数化；因此若使用 oldrelids 时，派生自同一
			 * 等价类的任何子句本来就会被包括，就跳过。
			 */
			if (parent_ec &&
				eclass_already_used(parent_ec, oldrelids,
									indexjoinclauses))
				continue;

			/*
			 * 若考虑的 relid 集合数超过启发式
			 * 限制，就停止考虑子句组合。不过仍会
			 * 单独考虑当前子句（在本循环下面）。
			 */
			if (list_length(*considered_relids) >= 10 * considered_clauses)
				break;

			/* 好，试这个并集 */
			get_join_index_paths(root, rel, index,
								 rclauseset, jclauseset, eclauseset,
								 bitindexpaths,
								 bms_union(clause_relids, oldrelids),
								 considered_relids);
		}

		/* 也单独试这组 relid */
		get_join_index_paths(root, rel, index,
							 rclauseset, jclauseset, eclauseset,
							 bitindexpaths,
							 clause_relids,
							 considered_relids);
	}
}

/*
 * get_join_index_paths
 *	  用指定外层关系的子句生成索引路径。
 *	  除了生成路径，还把 relids 加入 *considered_relids，
 *	  若它尚未出现。
 *
 * consider_index_join_clauses 的主力；理由见那里的说明。
 *
 * 'rel'、'index'、'rclauseset'、'jclauseset'、'eclauseset'、
 *		'bitindexpaths'、'considered_relids' 同上
 * 'relids' 是当前要考虑的 relid 集合（目标关系加上
 *		一个或多个外层关系）
 */
static void
get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
					 IndexOptInfo *index,
					 IndexClauseSet *rclauseset,
					 IndexClauseSet *jclauseset,
					 IndexClauseSet *eclauseset,
					 List **bitindexpaths,
					 Relids relids,
					 List **considered_relids)
{
	IndexClauseSet clauseset;
	int			indexcol;

	/* 若已经考虑过这组 relid，就不要重复工作 */
	if (list_member(*considered_relids, relids))
		return;

	/* 找出可用于这组 relid 的索引子句 */
	MemSet(&clauseset, 0, sizeof(clauseset));

	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ListCell   *lc;

		/* 先找适用的简单连接子句 */
		foreach(lc, jclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
		}

		/*
		 * 加入适用的等价类连接子句。为每一列生成的子句
		 * 是冗余的（参见 generate_implied_equalities_for_column），
		 * 因此最多需要一条。这是使用全部可用索引子句这一一般
		 * 规则的唯一例外。
		 */
		foreach(lc, eclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
			{
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
				break;
			}
		}

		/* 加入限制子句 */
		clauseset.indexclauses[indexcol] =
			list_concat(clauseset.indexclauses[indexcol],
						rclauseset->indexclauses[indexcol]);

		if (clauseset.indexclauses[indexcol] != NIL)
			clauseset.nonempty = true;
	}

	/* 应当已经找到一些，否则调用方传入了荒唐的 relid */
	Assert(clauseset.nonempty);

	/* 用收集到的这组子句建立索引 path(s) */
	get_index_paths(root, rel, index, &clauseset, bitindexpaths);

	/*
	 * 记住已经为这组 relid 考虑过路径。
	 */
	*considered_relids = lappend(*considered_relids, relids);
}

/*
 * eclass_already_used
 *		若任何可用于 oldrelids 的连接子句是从
 *		指定等价类生成的，则为真。
 */
static bool
eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
					List *indexjoinclauses)
{
	ListCell   *lc;

	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		RestrictInfo *rinfo = iclause->rinfo;

		if (rinfo->parent_ec == parent_ec &&
			bms_is_subset(rinfo->clause_relids, oldrelids))
			return true;
	}
	return false;
}


/*
 * get_index_paths
 *	  给定一个索引和它的一组索引子句，构造 IndexPaths。
 *
 * 普通索引路径直接交给 add_path，潜在的
 * 位图索引路径则加入 *bitindexpaths，留待以后处理。
 *
 * 这是 build_index_paths() 的一个相当简单的前端。它存在的主要原因是
 * 正确处理 ScalarArrayOpExpr 限定条件。若
 * 索引访问方法原生支持它们，就应当把它们放进简单
 * 索引路径。若不支持，建立简单索引
 * 路径时就排除它们，然后再单独尝试把它们放进位图路径。
 * 此外，应当考虑排除较低序的 ScalarArrayOpExpr
 * 限定条件，以便创建有序路径。
 */
static void
get_index_paths(PlannerInfo *root, RelOptInfo *rel,
				IndexOptInfo *index, IndexClauseSet *clauses,
				List **bitindexpaths)
{
	List	   *indexpaths;
	bool		skip_nonnative_saop = false;
	bool		skip_lower_saop = false;
	ListCell   *lc;

	/*
	 * 用这些子句建立简单索引路径。仅当索引访问方法原生支持时才允许 ScalarArrayOpExpr
	 * 子句，并跳过第一列之后的这类
	 * 子句（以便尽可能产生有序
	 * 路径）。
	 */
	indexpaths = build_index_paths(root, rel,
								   index, clauses,
								   index->predOK,
								   ST_ANYSCAN,
								   &skip_nonnative_saop,
								   &skip_lower_saop);

	/*
	 * 若在支持它们的访问方法的索引上跳过了任何较低序的 ScalarArrayOpExprs，
	 * 就重试并包括那些子句。这会
	 * 产生选择率更高但没有顺序的路径。
	 */
	if (skip_lower_saop)
	{
		indexpaths = list_concat(indexpaths,
								 build_index_paths(root, rel,
												   index, clauses,
												   index->predOK,
												   ST_ANYSCAN,
												   &skip_nonnative_saop,
												   NULL));
	}

	/*
	 * 把所有能形成普通 IndexScan 计划的那些交给 add_path。（一条
	 * 普通 IndexPath 既可以表示普通 IndexScan，也可以表示
	 * IndexOnlyScan，但就这里的目的而言这个区别并不
	 * 要紧。不过，有些索引也许只支持位图扫描，
	 * 那些不得在这里交给 add_path。）
	 *
	 * 另外，挑出可用作位图扫描的那些。为此，必须
	 * 丢掉不支持位图扫描的索引，而且我们
	 * 只对有一定选择率的路径感兴趣；应当丢掉
	 * 纯粹为了排序而生成的任何东西。
	 */
	foreach(lc, indexpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);

		if (index->amhasgettuple)
			add_path(rel, (Path *) ipath);

		if (index->amhasgetbitmap &&
			(ipath->path.pathkeys == NIL ||
			 ipath->indexselectivity < 1.0))
			*bitindexpaths = lappend(*bitindexpaths, ipath);
	}

	/*
	 * 若有索引无法原生处理的 ScalarArrayOpExpr 子句，
	 * 就生成依赖执行器管理的
	 * ScalarArrayOpExpr 的位图扫描路径。
	 */
	if (skip_nonnative_saop)
	{
		indexpaths = build_index_paths(root, rel,
									   index, clauses,
									   false,
									   ST_BITMAPSCAN,
									   NULL,
									   NULL);
		*bitindexpaths = list_concat(*bitindexpaths, indexpaths);
	}
}

/*
 * build_index_paths
 *	  给定一个索引和它的一组索引子句，构造零条
 *	  或多条 IndexPaths。它也构造零条或多条部分 IndexPaths。
 *
 * 返回路径列表，因为 (1) 本例程检查某些
 * 应当使我们不生成任何 IndexPath 的情形，以及 (2) 有些
 * 情形想同时考虑正向和反向扫描，以便
 * 得到两种排序。注意这些路径只返回
 * 给调用方，并不立刻交给 add_path()。
 *
 * 在顶层，useful_predicate 应当正好是该索引的 predOK 标志
 * （即，若它有一个已由限制
 * 子句证明的谓词，则为 true）。在处理 OR 子句的一支时，useful_predicate
 * 若证明谓词需要当前 OR 列表，就应当为 true。
 * 注意，若索引有一个
 * 无法证明的谓词，根本不应当调用本例程。
 *
 * scantype 表明我们想创建普通索引扫描、位图
 * 索引扫描，还是两者。当它是 ST_BITMAPSCAN 时，不考虑
 * 决定一条 Path 是否值得生成时的索引排序。
 *
 * 若 skip_nonnative_saop 非 NULL，就忽略 ScalarArrayOpExpr 子句，
 * 除非索引访问方法直接支持它们，并且若找到任何这类子句，就把 *skip_nonnative_saop
 * 设为 true（调用方必须把该变量初始化
 * 为 false）。若它为 NULL，就不忽略 ScalarArrayOpExpr 子句。
 *
 * 若 skip_lower_saop 非 NULL，就忽略非第一索引列的 ScalarArrayOpExpr 子句，
 * 并且若找到任何这类子句，就把 *skip_lower_saop 设为 true
 * （调用方必须把该变量初始化为 false）。若它为
 * NULL，就不忽略非第一列的 ScalarArrayOpExpr 子句，但它们会导致
 * 把扫描的输出视为无序。
 *
 * 'rel' 是该索引的堆关系
 * 'index' 是我们想为其生成路径的索引
 * 'clauses' 是可索引子句的集合（IndexClause 节点）
 * 'useful_predicate' 表明该索引是否有有用的谓词
 * 'scantype' 表明需要普通扫描还是位图扫描支持
 * 'skip_nonnative_saop' 表明若索引访问方法不支持 SAOP 是否接受
 * 'skip_lower_saop' 表明是否接受非第一列的 SAOP
 */
static List *
build_index_paths(PlannerInfo *root, RelOptInfo *rel,
				  IndexOptInfo *index, IndexClauseSet *clauses,
				  bool useful_predicate,
				  ScanTypeControl scantype,
				  bool *skip_nonnative_saop,
				  bool *skip_lower_saop)
{
	List	   *result = NIL;
	IndexPath  *ipath;
	List	   *index_clauses;
	Relids		outer_relids;
	double		loop_count;
	List	   *orderbyclauses;
	List	   *orderbyclausecols;
	List	   *index_pathkeys;
	List	   *useful_pathkeys;
	bool		found_lower_saop_clause;
	bool		pathkeys_possibly_useful;
	bool		index_is_ordered;
	bool		index_only_scan;
	int			indexcol;

	/*
	 * 检查索引是否支持所要的扫描 type(s)
	 */
	switch (scantype)
	{
		case ST_INDEXSCAN:
			if (!index->amhasgettuple)
				return NIL;
			break;
		case ST_BITMAPSCAN:
			if (!index->amhasgetbitmap)
				return NIL;
			break;
		case ST_ANYSCAN:
			/* 任一或两者都可以 */
			break;
	}

	/*
	 * 1. 把每列的 IndexClause 列表合成一个总列表。
	 *
	 * 在所得列表中，子句按索引键排序，从而使
	 * 列号形成非递减序列。（B-树以及也许其他地方
	 * 依赖这个顺序。）
	 * 若索引访问方法允许，
	 *
	 * 该列表可以是空的。
	 * 若接受非第一索引列的 ScalarArrayOpExpr
	 * 索引子句，就把 found_lower_saop_clause 设为 true。这使我们不能
	 * 假定扫描结果是有序的。（实际上，若更早的
	 * 所有列都有等值约束，结果仍然
	 * 有序，但让这段代码知道这一细化似乎太贵且不够模块化。）
	 *
	 * 还建立一个 Relids 集合，表明所选子句需要哪些外层关系。
	 * 任何 lateral_relids 都包括在内，但并不
	 * 另作计算。
	 */
	index_clauses = NIL;
	found_lower_saop_clause = false;
	outer_relids = bms_copy(rel->lateral_relids);
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ListCell   *lc;

		foreach(lc, clauses->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);
			RestrictInfo *rinfo = iclause->rinfo;

			/* 也许需要省掉 ScalarArrayOpExpr 子句 */
			if (IsA(rinfo->clause, ScalarArrayOpExpr))
			{
				if (!index->amsearcharray)
				{
					if (skip_nonnative_saop)
					{
						/* 忽略，因为索引不支持 */
						*skip_nonnative_saop = true;
						continue;
					}
					/* 调用方最好只打算把它用于位图扫描 */
					Assert(scantype == ST_BITMAPSCAN);
				}
				if (indexcol > 0)
				{
					if (skip_lower_saop)
					{
						/* 调用方不想失去索引顺序 */
						*skip_lower_saop = true;
						continue;
					}
					found_lower_saop_clause = true;
				}
			}

			/* 可以包括这条子句 */
			index_clauses = lappend(index_clauses, iclause);
			outer_relids = bms_add_members(outer_relids,
										   rinfo->clause_relids);
		}

		/*
		 * 若没有子句匹配第一索引列，就检查 amoptionalkey
		 * 限制。除非至少有一条索引子句，否则无法对
		 * amoptionalkey = false 的索引生成扫描。
		 * （处理第一列之后的列时，这个测试无法失败。
		 * 第一列之后的列没有任何
		 * 子句总是可以的。）
		 */
		if (index_clauses == NIL && !index->amoptionalkey)
			return NIL;
	}

	/* 不希望索引自己的关系出现在 outer_relids 里 */
	outer_relids = bms_del_member(outer_relids, rel->relid);

	/* 为代价估计计算 loop_count */
	loop_count = get_loop_count(root, rel->relid, outer_relids);

	/*
	 * 2. 计算描述索引排序的路径键（若有），然后看
	 * 其中有多少对这个查询真正有用。若只想建立位图索引扫描，
	 * 或者必须
	 * 假定扫描是无序的，这就不相关。
	 */
	pathkeys_possibly_useful = (scantype != ST_BITMAPSCAN &&
								!found_lower_saop_clause &&
								has_useful_pathkeys(root, rel));
	index_is_ordered = (index->sortopfamily != NULL);
	if (index_is_ordered && pathkeys_possibly_useful)
	{
		index_pathkeys = build_index_pathkeys(root, index,
											  ForwardScanDirection);
		useful_pathkeys = truncate_useless_pathkeys(root, rel,
													index_pathkeys);
		orderbyclauses = NIL;
		orderbyclausecols = NIL;
	}
	else if (index->amcanorderbyop && pathkeys_possibly_useful)
	{
		/* 看能否为 query_pathkeys 生成排序操作符 */
		match_pathkeys_to_index(index, root->query_pathkeys,
								&orderbyclauses,
								&orderbyclausecols);
		if (orderbyclauses)
			useful_pathkeys = root->query_pathkeys;
		else
			useful_pathkeys = NIL;
	}
	else
	{
		useful_pathkeys = NIL;
		orderbyclauses = NIL;
		orderbyclausecols = NIL;
	}

	/*
	 * 3. 检查是否可以做仅索引扫描。若不是在建立
	 * 普通索引扫描，这就不相关，因为位图扫描反正不支持
	 * 取回索引数据。
	 */
	index_only_scan = (scantype != ST_BITMAPSCAN &&
					   check_index_only(rel, index));

	/*
	 * 4. 若当前子句中有相关限制子句，
	 * 或者索引排序对以后的归并或最终输出排序可能有用，
	 * 或者索引有有用的
	 * 谓词，或者可以做仅索引扫描，就生成一条索引扫描路径。
	 */
	if (index_clauses != NIL || useful_pathkeys != NIL || useful_predicate ||
		index_only_scan)
	{
		ipath = create_index_path(root, index,
								  index_clauses,
								  orderbyclauses,
								  orderbyclausecols,
								  useful_pathkeys,
								  ForwardScanDirection,
								  index_only_scan,
								  outer_relids,
								  loop_count,
								  false);
		result = lappend(result, ipath);

		/*
		 * 若合适，考虑并行索引扫描。位图索引扫描不允许
		 * 并行索引扫描。
		 */
		if (index->amcanparallel &&
			rel->consider_parallel && outer_relids == NULL &&
			scantype != ST_BITMAPSCAN)
		{
			ipath = create_index_path(root, index,
									  index_clauses,
									  orderbyclauses,
									  orderbyclausecols,
									  useful_pathkeys,
									  ForwardScanDirection,
									  index_only_scan,
									  outer_relids,
									  loop_count,
									  true);

			/*
			 * 若代价计算之后发现不值得使用
			 * 并行工作进程，就把它释放掉。
			 */
			if (ipath->path.parallel_workers > 0)
				add_partial_path(rel, (Path *) ipath);
			else
				pfree(ipath);
		}
	}

	/*
	 * 5. 若索引有序，反向扫描也许有意义。
	 */
	if (index_is_ordered && pathkeys_possibly_useful)
	{
		index_pathkeys = build_index_pathkeys(root, index,
											  BackwardScanDirection);
		useful_pathkeys = truncate_useless_pathkeys(root, rel,
													index_pathkeys);
		if (useful_pathkeys != NIL)
		{
			ipath = create_index_path(root, index,
									  index_clauses,
									  NIL,
									  NIL,
									  useful_pathkeys,
									  BackwardScanDirection,
									  index_only_scan,
									  outer_relids,
									  loop_count,
									  false);
			result = lappend(result, ipath);

			/* 若合适，考虑并行索引扫描 */
			if (index->amcanparallel &&
				rel->consider_parallel && outer_relids == NULL &&
				scantype != ST_BITMAPSCAN)
			{
				ipath = create_index_path(root, index,
										  index_clauses,
										  NIL,
										  NIL,
										  useful_pathkeys,
										  BackwardScanDirection,
										  index_only_scan,
										  outer_relids,
										  loop_count,
										  true);

				/*
				 * 若代价计算之后发现不值得
				 * 使用并行工作进程，就把它释放掉。
				 */
				if (ipath->path.parallel_workers > 0)
					add_partial_path(rel, (Path *) ipath);
				else
					pfree(ipath);
			}
		}
	}

	return result;
}

/*
 * build_paths_for_OR
 *	  给定 OR 子句一支中的限制子句列表，
 *	  为该关系构造全部匹配的 IndexPaths。
 *
 * 这里必须扫描该关系的全部索引，因为位图 OR 树
 * 可以使用多个索引。
 *
 * 调用方实际上提供两份限制子句列表：一些
 * "当前" 的和一些 "其他" 的。两份都可以自由
 * 用来匹配索引键，但索引必须至少使用一条
 * "当前" 子句才被认为可用。这样做的动机是
 * 像这样的例子
 *		WHERE (x = 42) AND (... OR (y = 52 AND z = 77) OR ....)
 * 在考虑 OR 的 y/z 子子句时，可以把 "x = 42"
 * 当作一条可用的索引条件；但不应当把该
 * 子子句匹配到只在 x 上的索引，因为这样的 Path 在
 * 上一层已经生成过。因此 OR 子子句可以用 x,y,z 上的索引
 * 或 x,y 上的索引，但不能只用 x 上的索引。
 * 处理部分索引时，索引谓词与
 * 一条 "当前" 子句匹配，也会使该索引可用。
 *
 * 'rel' 是我们想为其生成索引路径的关系
 * 'clauses' 是当前子句列表（RestrictInfo 节点）
 * 'other_clauses' 是额外的上层子句列表
 */
static List *
build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
				   List *clauses, List *other_clauses)
{
	List	   *result = NIL;
	List	   *all_clauses = NIL;	/* 需要时才计算 */
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		IndexClauseSet clauseset;
		List	   *indexpaths;
		bool		useful_predicate;

		/* 若索引不支持位图扫描，就忽略 */
		if (!index->amhasgetbitmap)
			continue;

		/*
		 * 忽略与查询不匹配的部分索引。若部分
		 * 索引被标成 predOK，就知道它可以用。否则，必须
		 * 测试增加的子句是否足以蕴含
		 * 谓词。若是，就可以在当前上下文中使用该索引。
		 *
		 * 当且仅当谓词是用
		 * 当前这组子句证明的，才把 useful_predicate 设为 true。这是为了防止把
		 * predOK 索引匹配到 OR 的一支，那在法律上可以但
		 * 毫无效率。（更好的计划会由
		 * 只扫描该 predOK 索引本身生成，不需要 OR。）
		 */
		useful_predicate = false;
		if (index->indpred != NIL)
		{
			if (index->predOK)
			{
				/* 可用，但不要设置 useful_predicate */
			}
			else
			{
				/* 若还没做，就形成 all_clauses */
				if (all_clauses == NIL)
					all_clauses = list_concat_copy(clauses, other_clauses);

				if (!predicate_implied_by(index->indpred, all_clauses, false))
					continue;	/* 完全不能用 */

				if (!predicate_implied_by(index->indpred, other_clauses, false))
					useful_predicate = true;
			}
		}

		/*
		 * 找出能匹配该索引的限制子句。
		 */
		MemSet(&clauseset, 0, sizeof(clauseset));
		match_clauses_to_index(root, clauses, index, &clauseset);

		/*
		 * 若到目前还没有匹配，并且索引谓词没有用，就
		 * 不要它。
		 */
		if (!clauseset.nonempty && !useful_predicate)
			continue;

		/*
		 * 把 "其他" 限制子句加入子句集。
		 */
		match_clauses_to_index(root, other_clauses, index, &clauseset);

		/*
		 * 若可能，就构造路径。
		 */
		indexpaths = build_index_paths(root, rel,
									   index, &clauseset,
									   useful_predicate,
									   ST_BITMAPSCAN,
									   NULL,
									   NULL);
		result = list_concat(result, indexpaths);
	}

	return result;
}

/*
 * generate_bitmap_or_paths
 *		查看子句列表以找出 OR 子句，并为每个
 *		能这样处理的生成一条 BitmapOrPath。返回生成的
 *		BitmapOrPaths 列表。
 *
 * other_clauses 是可以假定为真、
 * 用于生成索引限定条件、但不要在其中搜索
 * OR 的额外子句列表。（动机见 build_paths_for_OR()。）
 */
static List *
generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
						 List *clauses, List *other_clauses)
{
	List	   *result = NIL;
	List	   *all_clauses;
	ListCell   *lc;

	/*
	 * 当前子句和其他子句都可以作为
	 * build_paths_for_OR 的上下文；不必从列表中去掉 OR。
	 */
	all_clauses = list_concat_copy(clauses, other_clauses);

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		List	   *pathlist;
		Path	   *bitmapqual;
		ListCell   *j;

		/* 忽略不是 OR 的 RestrictInfos */
		if (!restriction_is_or_clause(rinfo))
			continue;

		/*
		 * 必须能把 OR 的每一支至少匹配到一个索引，
		 * 否则就不能用它。
		 */
		pathlist = NIL;
		foreach(j, ((BoolExpr *) rinfo->orclause)->args)
		{
			Node	   *orarg = (Node *) lfirst(j);
			List	   *indlist;

			/* OR 参数应当是 ANDs 或子 RestrictInfos */
			if (is_andclause(orarg))
			{
				List	   *andargs = ((BoolExpr *) orarg)->args;

				indlist = build_paths_for_OR(root, rel,
											 andargs,
											 all_clauses);

				/* 若有子 OR，就递归 */
				indlist = list_concat(indlist,
									  generate_bitmap_or_paths(root, rel,
															   andargs,
															   all_clauses));
			}
			else
			{
				RestrictInfo *ri = castNode(RestrictInfo, orarg);
				List	   *orargs;

				Assert(!restriction_is_or_clause(ri));
				orargs = list_make1(ri);

				indlist = build_paths_for_OR(root, rel,
											 orargs,
											 all_clauses);
			}

			/*
			 * 若这一支什么都没匹配上，对这条 OR
			 * 子句就无能为力。
			 */
			if (indlist == NIL)
			{
				pathlist = NIL;
				break;
			}

			/*
			 * 好，选出最有希望的 AND 组合，并把它加入
			 * pathlist。
			 */
			bitmapqual = choose_bitmap_and(root, rel, indlist);
			pathlist = lappend(pathlist, bitmapqual);
		}

		/*
		 * 若每一支都有匹配，就把它们变成一条
		 * BitmapOrPath，并加入结果列表。
		 */
		if (pathlist != NIL)
		{
			bitmapqual = (Path *) create_bitmap_or_path(root, rel, pathlist);
			result = lappend(result, bitmapqual);
		}
	}

	return result;
}


/*
 * choose_bitmap_and
 *		给定一份非空的位图路径列表，把它们 AND 成一条路径。
 *
 * 这不是平凡的决定，因为允许使用
 * 给定路径集合的任何子集。
 * 想要在选择率
 *
 * 和计算位图的代价之间取得好的折中。
 * 结果要么是输入中的单独一条，要么是组合多个输入的 BitmapAndPath。
 */
static Path *
choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	int			npaths = list_length(paths);
	PathClauseUsage **pathinfoarray;
	PathClauseUsage *pathinfo;
	List	   *clauselist;
	List	   *bestpaths = NIL;
	Cost		bestcost = 0;
	int			i,
				j;
	ListCell   *l;

	Assert(npaths > 0);			/* 否则是调用方错误 */
	if (npaths == 1)
		return (Path *) linitial(paths);	/* 简单情形 */

	/*
	 * 理论上应当考虑给定路径的每个非空子集。
	 * 实践中，鉴于估计的粗糙，
	 * 更不必说更高层 AND 和
	 * OR 子句可能的影响，那似乎过头了。而且，若路径
	 * 数量很大，这完全不切实际，因为工作量会按 O(2^N) 增长。
	 *
	 * 作为启发式，先检查使用完全相同的
	 * WHERE 子句加索引谓词条件集合的路径，并拒绝任何这样一组中除
	 * 扫描最便宜者之外的全部。这主要去掉那些
	 * 包含有意义列、但也包含无关列的索引。（在
	 * DBA 过度创建变体
	 * 索引的情形下，这可以大幅减少
	 * 进一步考虑的路径数。）
	 *
	 * 然后把幸存路径按扫描代价从低到高排序，并且
	 * 对每条路径，考虑单独用它作为位图
	 * 扫描的基础。然后考虑由该路径加上
	 * 每条后续（代价更高）路径形成的位图 AND 扫描，若加上后续路径
	 * 能降低估计的总扫描代价，就把它加上。这意味着考虑
	 * 大约 O(N^2) 而不是 O(2^N) 种路径组合，这
	 * 相当可以忍受，尤其是因为预过滤之后 N 通常相当小。
	 * 返回其中最便宜的。
	 *
	 * 只考虑没有任何两个索引使用
	 * 同一 WHERE 子句的 AND 组合。这有点凑合：需要它是因为
	 * costsize.c 和 clausesel.c 对冗余子句不太聪明。
	 * 它们通常会把冗余子句算两次，产生
	 * 过小的选择率，使冗余的 AND 步骤看起来
	 * 降低了总代价。也许有一天那段代码会更聪明，
	 * 就可以去掉这个限制。（但注意这也防御
	 * 完全重复的输入路径，这可能发生，因为
	 * match_join_clauses_to_index 会找到与
	 * extract_restriction_or_clauses 从中抽出 OR 限制子句的
	 * 相同 OR 连接子句。）
	 *
	 * 出于同一原因，拒绝索引
	 * 谓词子句与另一子句重复的 AND 组合。这里发现有必要
	 * 更严格：若部分索引的任何
	 * 谓词子句被迄今使用的 WHERE 子句和谓词
	 * 子句集合所蕴含，就拒绝该部分索引。这覆盖了条件 "x = 42"
	 * 与普通索引一起使用、随后是部分
	 * 索引 "WHERE x >= 40 AND x < 50" 的无子句扫描这类情形。该部分索引被接受
	 * 只是因为存在 "x = 42"，因此允许它会部分
	 * 重复计算选择率。（也可以对
	 * 普通限定子句使用 predicate_implied_by，做更智能但贵得多的
	 * 冗余检查 --- 但在多数情形里简单相等
	 * 似乎就够了。）
	 */

	/*
	 * 提取子句使用信息，并检测使用完全
	 * 相同子句集合的路径；任何这样一组只保留扫描最便宜的。
	 * 幸存路径放进数组以便 qsort。
	 */
	pathinfoarray = (PathClauseUsage **)
		palloc(npaths * sizeof(PathClauseUsage *));
	clauselist = NIL;
	npaths = 0;
	foreach(l, paths)
	{
		Path	   *ipath = (Path *) lfirst(l);

		pathinfo = classify_index_clause_usage(ipath, &clauselist);

		/* 若无法归类，就把它当作与其他所有都不同 */
		if (pathinfo->unclassifiable)
		{
			pathinfoarray[npaths++] = pathinfo;
			continue;
		}

		for (i = 0; i < npaths; i++)
		{
			if (!pathinfoarray[i]->unclassifiable &&
				bms_equal(pathinfo->clauseids, pathinfoarray[i]->clauseids))
				break;
		}
		if (i < npaths)
		{
			/* 重复的 clauseids，保留更便宜的 */
			Cost		ncost;
			Cost		ocost;
			Selectivity nselec;
			Selectivity oselec;

			cost_bitmap_tree_node(pathinfo->path, &ncost, &nselec);
			cost_bitmap_tree_node(pathinfoarray[i]->path, &ocost, &oselec);
			if (ncost < ocost)
				pathinfoarray[i] = pathinfo;
		}
		else
		{
			/* 不是重复的 clauseids，加入数组 */
			pathinfoarray[npaths++] = pathinfo;
		}
	}

	/* 若只剩一条路径，就做完了 */
	if (npaths == 1)
		return pathinfoarray[0]->path;

	/* 按索引访问代价对幸存路径排序 */
	qsort(pathinfoarray, npaths, sizeof(PathClauseUsage *),
		  path_usage_comparator);

	/*
	 * 对每个幸存索引，把它当作 "AND 组的领头"，看
	 * 加上任何后面的索引是否得到总代价比以前
	 * 更便宜的 AND 路径。然后取最便宜的 AND 组。
	 *
	 * 注意：无子句或无法归类的路径会有
	 * 空的 clauseids，因此不会被这里的 clauseids
	 * 过滤拒绝，也不会因此拒绝后面的路径。
	 */
	for (i = 0; i < npaths; i++)
	{
		Cost		costsofar;
		List	   *qualsofar;
		Bitmapset  *clauseidsofar;

		pathinfo = pathinfoarray[i];
		paths = list_make1(pathinfo->path);
		costsofar = bitmap_scan_cost_est(root, rel, pathinfo->path);
		qualsofar = list_concat_copy(pathinfo->quals, pathinfo->preds);
		clauseidsofar = bms_copy(pathinfo->clauseids);

		for (j = i + 1; j < npaths; j++)
		{
			Cost		newcost;

			pathinfo = pathinfoarray[j];
			/* 检查冗余 */
			if (bms_overlap(pathinfo->clauseids, clauseidsofar))
				continue;		/* 认为它冗余 */
			if (pathinfo->preds)
			{
				bool		redundant = false;

				/* 我们分别检查每个谓词子句 */
				foreach(l, pathinfo->preds)
				{
					Node	   *np = (Node *) lfirst(l);

					if (predicate_implied_by(list_make1(np), qualsofar, false))
					{
						redundant = true;
						break;	/* 跳出内层 foreach 循环 */
					}
				}
				if (redundant)
					continue;
			}
			/* 暂时把新路径加入 paths，以便估计代价 */
			paths = lappend(paths, pathinfo->path);
			newcost = bitmap_and_cost_est(root, rel, paths);
			if (newcost < costsofar)
			{
				/* 把新路径留在 paths 里，并更新辅助变量 */
				costsofar = newcost;
				qualsofar = list_concat(qualsofar, pathinfo->quals);
				qualsofar = list_concat(qualsofar, pathinfo->preds);
				clauseidsofar = bms_add_members(clauseidsofar,
												pathinfo->clauseids);
			}
			else
			{
				/* 拒绝新路径，把它从 paths 列表中去掉 */
				paths = list_truncate(paths, list_length(paths) - 1);
			}
		}

		/* 保留最便宜的 AND 组（或单条） */
		if (i == 0 || costsofar < bestcost)
		{
			bestpaths = paths;
			bestcost = costsofar;
		}

		/* 做些轻松的清理（不过并不很努力） */
		list_free(qualsofar);
	}

	if (list_length(bestpaths) == 1)
		return (Path *) linitial(bestpaths);	/* 不需要 AND */
	return (Path *) create_bitmap_and_path(root, rel, bestpaths);
}

/* qsort 比较器，按索引访问代价递增排序 */
static int
path_usage_comparator(const void *a, const void *b)
{
	PathClauseUsage *pa = *(PathClauseUsage *const *) a;
	PathClauseUsage *pb = *(PathClauseUsage *const *) b;
	Cost		acost;
	Cost		bcost;
	Selectivity aselec;
	Selectivity bselec;

	cost_bitmap_tree_node(pa->path, &acost, &aselec);
	cost_bitmap_tree_node(pb->path, &bcost, &bselec);

	/*
	 * 若代价相同，就按选择率排序。
	 */
	if (acost < bcost)
		return -1;
	if (acost > bcost)
		return 1;

	if (aselec < bselec)
		return -1;
	if (aselec > bselec)
		return 1;

	return 0;
}

/*
 * 估计用单条
 * 索引路径（可以是 BitmapAnd 或 BitmapOr 节点）实际执行位图扫描的代价。
 */
static Cost
bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel, Path *ipath)
{
	BitmapHeapPath bpath;

	/* 建立一个哑 BitmapHeapPath */
	bpath.path.type = T_BitmapHeapPath;
	bpath.path.pathtype = T_BitmapHeapScan;
	bpath.path.parent = rel;
	bpath.path.pathtarget = rel->reltarget;
	bpath.path.param_info = ipath->param_info;
	bpath.path.pathkeys = NIL;
	bpath.bitmapqual = ipath;

	/*
	 * 不考虑并行，检查临时路径的代价。
	 * 并行位图堆路径留到后面阶段考虑。
	 */
	bpath.path.parallel_workers = 0;

	/* 现在可以做 cost_bitmap_heap_scan */
	cost_bitmap_heap_scan(&bpath.path, root, rel,
						  bpath.path.param_info,
						  ipath,
						  get_loop_count(root, rel->relid,
										 PATH_REQ_OUTER(ipath)));

	return bpath.path.total_cost;
}

/*
 * 估计用给定
 * 输入实际执行 BitmapAnd 扫描的代价。
 */
static Cost
bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	BitmapAndPath *apath;

	/*
	 * 不妨在这里建一个真正的 BitmapAndPath，因为这点工作
	 * 略嫌复杂，不值得只为省一次 palloc 而重复。
	 */
	apath = create_bitmap_and_path(root, rel, paths);

	return bitmap_scan_cost_est(root, rel, (Path *) apath);
}


/*
 * classify_index_clause_usage
 *		构造一个 PathClauseUsage 结构，描述给定索引扫描路径使用的 WHERE 子句和
 *		索引谓词子句。
 *		若两条子句 equal()，就认为它们相同。
 *
 * 将来也许想把这些信息迁进 Path 数据
 * 结构本身，但目前只在
 * choose_bitmap_and() 内部需要。
 *
 * *clauselist 按需要使用并扩展，以标识跨相继调用见到的全部不同
 * 子句。调用方必须把它初始化为 NIL，
 * 在一组调用的第一次之前。
 */
static PathClauseUsage *
classify_index_clause_usage(Path *path, List **clauselist)
{
	PathClauseUsage *result;
	Bitmapset  *clauseids;
	ListCell   *lc;

	result = (PathClauseUsage *) palloc(sizeof(PathClauseUsage));
	result->path = path;

	/* 递归找出该路径使用的限定条件和谓词 */
	result->quals = NIL;
	result->preds = NIL;
	find_indexpath_quals(path, &result->quals, &result->preds);

	/*
	 * 有些机器生成的查询有多得离谱的限定子句。
	 * 为避免即使在这个初步
	 * 分类步骤里也陷入 O(N^2) 行为，想限制能在
	 * *clauselist 中积累的项数。把限定条件加
	 * 谓词超过 100 条的任何路径当作无法归类，这会使调用代码把它
	 * 视为与所有其他路径都不同。
	 */
	if (list_length(result->quals) + list_length(result->preds) > 100)
	{
		result->clauseids = NULL;
		result->unclassifiable = true;
		return result;
	}

	/* 建立一个表示限定条件和谓词的 bitmapset */
	clauseids = NULL;
	foreach(lc, result->quals)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	foreach(lc, result->preds)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	result->clauseids = clauseids;
	result->unclassifiable = false;

	return result;
}


/*
 * find_indexpath_quals
 *
 * 给定普通或位图索引扫描的 Path 结构，抽出
 * 该 Path 中使用的全部索引子句和索引谓词条件的列表。
 * 这些被追加到 *quals 和 *preds 的初始内容上（因此
 * 调用方应当把它们初始化为 NIL）。
 *
 * 注意我们不是要准确表示该 Path 的 AND/OR
 * 语义，只是找出使用的全部基础条件。
 *
 * 结果列表含有指向该 Path 中所用表达式的指针，
 * 但所有列表单元都是新造的，因此破坏性地
 * 修改这些列表（例如与其他列表拼接）是安全的。
 */
static void
find_indexpath_quals(Path *bitmapqual, List **quals, List **preds)
{
	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		ListCell   *l;

		foreach(l, apath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		ListCell   *l;

		foreach(l, opath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		ListCell   *l;

		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);

			*quals = lappend(*quals, iclause->rinfo->clause);
		}
		*preds = list_concat(*preds, ipath->indexinfo->indpred);
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
}


/*
 * find_list_position
 *		返回给定节点在给定
 *		节点列表中的位置（从 0 计）。若它不与任何现有列表成员 equal()，
 *		就把它加到末尾，并返回那个位置。
 */
static int
find_list_position(Node *node, List **nodelist)
{
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, *nodelist)
	{
		Node	   *oldnode = (Node *) lfirst(lc);

		if (equal(node, oldnode))
			return i;
		i++;
	}

	*nodelist = lappend(*nodelist, node);

	return i;
}


/*
 * check_index_only
 *		判断该索引是否可以做仅索引扫描。
 */
static bool
check_index_only(RelOptInfo *rel, IndexOptInfo *index)
{
	bool		result;
	Bitmapset  *attrs_used = NULL;
	Bitmapset  *index_canreturn_attrs = NULL;
	ListCell   *lc;
	int			i;

	/* 必须启用仅索引扫描 */
	if (!enable_indexonlyscan)
		return false;

	/*
	 * 检查该关系所需的全部属性是否都能从
	 * 索引得到。
	 */

	/*
	 * 首先，找出连接或最终输出所需的全部属性。
	 * 注意：必须看关系的目标列表，而不是 attr_needed 数据，
	 * 因为不为继承子关系计算 attr_needed。
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/*
	 * 加入限制子句使用的全部属性；但只考虑
	 * 不被索引谓词蕴含的那些子句，因为被
	 * 蕴含的那些不必在计划中显式检查。
	 *
	 * 注意：若确定索引不是有损的，只在索引限定条件中使用的属性在
	 * 运行时也不需要。不过
	 * 要准确计入这一点会很复杂，而且在多数情形里并不
	 * 要紧，因为反正会得出这些属性
	 * 可以从索引得到。
	 */
	foreach(lc, index->indrestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * 构造该索引在仅索引扫描中能返回的列的
	 * bitmapset。
	 */
	for (i = 0; i < index->ncolumns; i++)
	{
		int			attno = index->indexkeys[i];

		/*
		 * 目前只是忽略索引表达式。以后
		 * 对它们做点什么也许不错。
		 */
		if (attno == 0)
			continue;

		if (index->canreturn[i])
			index_canreturn_attrs =
				bms_add_member(index_canreturn_attrs,
							   attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* 是否拥有全部必要属性？ */
	result = bms_is_subset(attrs_used, index_canreturn_attrs);

	bms_free(attrs_used);
	bms_free(index_canreturn_attrs);

	return result;
}

/*
 * get_loop_count
 *		为计价一条参数化路径而选择要使用的循环次数估计，
 *		该路径具有给定的外层 relid 集合。
 *
 * 因为在开始生成连接
 * 关系之前就产生参数化路径，不可能精确预测一条参数化
 * 路径会被迭代多少次；不知道将处在
 * 嵌套循环外侧的那个关系有多大。不过，计价时应当设法以某种方式计入
 * 多次迭代。这里体现的启发式
 * 是使用该路径所用连接子句需要的、最小的另一个基关系的行数。
 * （也可以考虑
 * 最大的那个，但那似乎太乐观。）对单个其他关系的情形，这当然是正确
 * 答案，对多路连接情形，它似乎是合理的
 * 零阶近似。
 *
 * 另外，检查每条连接子句的另一侧是否在
 * 当前关系处于其外侧的某个半连接的内侧。
 * 若是，参数化路径能被使用的唯一方式是该
 * 半连接的 RHS 已被唯一化，因此应当使用唯一
 * RHS 行数，而不是该关系的原始行数。
 *
 * 注意：要让这工作，allpaths.c 必须在开始计算路径之前，或至少在
 * 调用 create_index_paths() 之前，建立全部基关系的大小
 * 估计。
 */
static double
get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids)
{
	double		result;
	int			outer_relid;

	/* 对未参数化的路径，立刻返回 1.0 */
	if (outer_relids == NULL)
		return 1.0;

	result = 0.0;
	outer_relid = -1;
	while ((outer_relid = bms_next_member(outer_relids, outer_relid)) >= 0)
	{
		RelOptInfo *outer_rel;
		double		rowcount;

		/* 偏执：忽略假的 relid 索引 */
		if (outer_relid >= root->simple_rel_array_size)
			continue;
		outer_rel = root->simple_rel_array[outer_relid];
		if (outer_rel == NULL)
			continue;
		Assert(outer_rel->relid == outer_relid);	/* 对数组做健全性检查 */

		/* 其他关系可能被证明为空，若是则忽略 */
		if (IS_DUMMY_REL(outer_rel))
			continue;

		/* 否则，该关系的行数估计现在应当有效 */
		Assert(outer_rel->rows > 0);

		/* 检查该关系是否在任何半连接的内侧 */
		rowcount = adjust_rowcount_for_semijoins(root,
												 cur_relid,
												 outer_relid,
												 outer_rel->rows);

		/* 记住外层关系中最小的行数估计 */
		if (result == 0.0 || result > rowcount)
			result = rowcount;
	}
	/* 若没找到有效关系，就返回 1.0（不应当发生） */
	return (result > 0.0) ? result : 1.0;
}

/*
 * 检查 outer_relid 是否在 cur_relid
 * 处于其外侧的任何半连接的内侧。若是，就用半连接 RHS 的估计唯一行数替换行数
 * （假定那更小，其实
 * 也许不是）。估计很粗糙，但这是进行到这一步
 * 所能做到的最好结果。
 */
static double
adjust_rowcount_for_semijoins(PlannerInfo *root,
							  Index cur_relid,
							  Index outer_relid,
							  double rowcount)
{
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_SEMI &&
			bms_is_member(cur_relid, sjinfo->syn_lefthand) &&
			bms_is_member(outer_relid, sjinfo->syn_righthand))
		{
			/* 估计唯一化后的行数 */
			double		nraw;
			double		nunique;

			nraw = approximate_joinrel_size(root, sjinfo->syn_righthand);
			nunique = estimate_num_groups(root,
										  sjinfo->semi_rhs_exprs,
										  nraw,
										  NULL,
										  NULL);
			if (rowcount > nunique)
				rowcount = nunique;
		}
	}
	return rowcount;
}

/*
 * 对连接关系的大小做一个近似估计。
 *
 * 此时没有足够信息得到好的估计，因此
 * 只是把基关系大小乘在一起。幸好，
 * 对 RHS 上只有单个关系的最常见半连接情形，这
 * 反正是正确答案。另外，estimate_num_groups() 对其 input_rows 参数只有弱
 * 依赖（基本上把它当作一个钳位）。
 * 因此即使严重
 * 高估了 RHS 的原始大小，也许仍能得到相当不错的最终结果。
 */
static double
approximate_joinrel_size(PlannerInfo *root, Relids relids)
{
	double		rowcount = 1.0;
	int			relid;

	relid = -1;
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RelOptInfo *rel;

		/* 偏执：忽略假的 relid 索引 */
		if (relid >= root->simple_rel_array_size)
			continue;
		rel = root->simple_rel_array[relid];
		if (rel == NULL)
			continue;
		Assert(rel->relid == relid);	/* 对数组做健全性检查 */

		/* 关系可能被证明为空，若是则忽略 */
		if (IS_DUMMY_REL(rel))
			continue;

		/* 否则，该关系的行数估计现在应当有效 */
		Assert(rel->rows > 0);

		/* 累乘 */
		rowcount *= rel->rows;
	}
	return rowcount;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK QUERY CLAUSES（检查查询子句的例程）  ----
 ****************************************************************************/

/*
 * match_restriction_clauses_to_index
 *	  找出该关系中与索引匹配的限制子句。
 *	  匹配的子句加入 *clauseset。
 */
static void
match_restriction_clauses_to_index(PlannerInfo *root,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset)
{
	/* 可以忽略被索引谓词蕴含的子句 */
	match_clauses_to_index(root, index->indrestrictinfo, index, clauseset);
}

/*
 * match_join_clauses_to_index
 *	  找出该关系中与索引匹配的连接子句。
 *	  匹配的子句加入 *clauseset。
 *	  另外，把任何可能可用的连接 OR 子句加入 *joinorclauses。
 */
static void
match_join_clauses_to_index(PlannerInfo *root,
							RelOptInfo *rel, IndexOptInfo *index,
							IndexClauseSet *clauseset,
							List **joinorclauses)
{
	ListCell   *lc;

	/* 扫描该关系的连接子句 */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* 检查子句能否移到这个关系 */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		/* 可能可用，因此看它是否匹配索引，或者是否为 OR */
		if (restriction_is_or_clause(rinfo))
			*joinorclauses = lappend(*joinorclauses, rinfo);
		else
			match_clause_to_index(root, rinfo, index, clauseset);
	}
}

/*
 * match_eclass_clauses_to_index
 *	  找出该关系中与索引匹配的 EquivalenceClass 连接子句。
 *	  匹配的子句加入 *clauseset。
 */
static void
match_eclass_clauses_to_index(PlannerInfo *root, IndexOptInfo *index,
							  IndexClauseSet *clauseset)
{
	int			indexcol;

	/* 若关系不在任何这样的 EC 中，就无事可做 */
	if (!index->rel->has_eclass_joins)
		return;

	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ec_member_matches_arg arg;
		List	   *clauses;

		/* 生成子句，跳过任何连到 lateral_referencers 的 */
		arg.index = index;
		arg.indexcol = indexcol;
		clauses = generate_implied_equalities_for_column(root,
														 index->rel,
														 ec_member_matches_indexcol,
														 (void *) &arg,
														 index->rel->lateral_referencers);

		/*
		 * 必须检查结果是否真的匹配索引，
		 * 因为对非 B-树索引，EC 的等值操作符也许不
		 * 在索引 opclass 中（参见 ec_member_matches_indexcol）。
		 */
		match_clauses_to_index(root, clauses, index, clauseset);
	}
}

/*
 * match_clauses_to_index
 *	  对列表中的每条子句执行 match_clause_to_index()。
 *	  匹配的子句加入 *clauseset。
 */
static void
match_clauses_to_index(PlannerInfo *root,
					   List *clauses,
					   IndexOptInfo *index,
					   IndexClauseSet *clauseset)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		match_clause_to_index(root, rinfo, index, clauseset);
	}
}

/*
 * match_clause_to_index
 *	  测试一条限定子句能否与索引一起使用。
 *
 * 若该子句可用，就为它向 *clauseset 中适当的
 * 列表加入一项 IndexClause。（第一次
 * 调用之前必须把 *clauseset 初始化为零。）
 *
 * 注意：有些情况下会从
 * 多个地方发现相同的 RestrictInfos。拒绝把一条
 * 子句加两次来防御冗余输出（指针相等作为这种检查应当足够好）。
 *
 * 注意：定义糟糕的索引可能有多个匹配
 * 列。若是这样，总是选择第一个匹配；这避免了
 * 因把同一子句与不同索引列使用多次而对索引选择率
 * 产生夸大认识的情形。
 */
static void
match_clause_to_index(PlannerInfo *root,
					  RestrictInfo *rinfo,
					  IndexOptInfo *index,
					  IndexClauseSet *clauseset)
{
	int			indexcol;

	/*
	 * 绝不把伪常量匹配到索引。（正常情况下匹配无法
	 * 发生，因为伪常量子句无法包含 Var，
	 * 但若有人在常量上建表达式索引呢？对部分索引这样做
	 * 也并非完全不合理。）
	 */
	if (rinfo->pseudoconstant)
		return;

	/*
	 * 若子句因为必须等到某个更低安全级别的限制子句之后才能用作索引限定条件，
	 * 就拒绝它。
	 */
	if (!restriction_is_securely_promotable(rinfo, index->rel))
		return;

	/* 好，检查每个索引键列是否匹配 */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		IndexClause *iclause;
		ListCell   *lc;

		/* 忽略重复 */
		foreach(lc, clauseset->indexclauses[indexcol])
		{
			iclause = (IndexClause *) lfirst(lc);

			if (iclause->rinfo == rinfo)
				return;
		}

		/* 好，试着把该子句匹配到索引列 */
		iclause = match_clause_to_indexcol(root,
										   rinfo,
										   indexcol,
										   index);
		if (iclause)
		{
			/* 成功，因此记下来 */
			clauseset->indexclauses[indexcol] =
				lappend(clauseset->indexclauses[indexcol], iclause);
			clauseset->nonempty = true;
			return;
		}
	}
}

/*
 * match_clause_to_indexcol()
 *	  判断一条限制子句是否匹配索引的某一列，
 *	  若匹配，就建立一个描述细节的 IndexClause 节点。
 *
 *	  要正常匹配索引，操作符子句：
 *
 *	  (1) 必须形如 (indexkey op const) 或 (const op indexkey)；
 *		   并且
 *	  (2) 必须包含一个属于该列的索引操作符族的操作符；
 *		   并且
 *	  (3) 若排序规则相关，必须匹配索引的排序规则。
 *
 *	  我们对 "const" 的定义极为宽松：允许任何
 *	  不涉及易变函数或该索引关系的 Var 的东西。
 *	  尤其是，这里接受属于查询中其他关系的 Vars，
 *	  因为那种形式的子句可以用于
 *	  参数化索引扫描。由更高层代码
 *	  负责适当地管理限制子句和连接子句。
 *
 *	  注意：确实需要检查子句 "const" 一侧是否有该索引关系的 Vars，
 *	  因为像 (a.f1 OP (b.f2 OP a.f3)) 这样的子句
 *	  不能由对 a.f1 的参数化索引扫描处理，而
 *	  像 (a.f1 OP (b.f2 OP c.f3)) 这样的就可以。
 *
 *	  目前，执行器只能处理索引键在左侧的索引限定条件，
 *	  因此只有能交换子句把键放到左侧时，才能使用索引键
 *	  在右侧的子句。
 *	  办法是生成一个 IndexClause，把正确交换后的
 *	  操作子句作为派生的索引限定条件。
 *
 *	  若索引有排序规则，子句必须有相同的排序规则。
 *	  对没有排序规则的索引，假定这无关紧要；像 "hstore ? text" 这类情形
 *	  需要如此，其中 hstore 的操作符
 *	  不关心排序规则，但子句仍会因为 text 参数而被标上
 *	  排序规则。（这一逻辑
 *	  体现在宏 IndexCollMatchesExprColl 中。）
 *
 *	  也可以把 RowCompareExpr 子句匹配到索引（但
 *	  目前只有 B-树索引处理这个）。
 *
 *	  也可以把 ScalarArrayOpExpr 子句匹配到索引，当
 *	  子句形如 "indexkey op ANY (arrayconst)" 时。
 *
 *	  对布尔索引，也可以把子句直接
 *	  匹配到索引键；或者子句是 (NOT indexkey)。
 *
 *	  最后，有些操作符和函数可以被处理，
 *	  以便从本身不可索引的子句推出（通常有损的）索引限定条件。
 *	  若看到 OpExpr 或 FuncExpr 的任何操作数
 *	  匹配索引键，并且该函数附有规划器支持函数，
 *	  就会调用支持函数，看能否建立这样的
 *	  索引限定条件。
 *
 * 'rinfo' 是要测试的子句（作为 RestrictInfo 节点）。
 * 'indexcol' 是 'index' 的列号（从 0 计）。
 * 'index' 是感兴趣的索引。
 *
 * 若该子句可以与这个索引键一起使用，就返回 IndexClause，
 * 否则返回 NULL。
 *
 * 注意：若子句是 OR 或 AND 子句，则返回 NULL；
 * 由更高层例程负责应付那些。
 */
static IndexClause *
match_clause_to_indexcol(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 int indexcol,
						 IndexOptInfo *index)
{
	IndexClause *iclause;
	Expr	   *clause = rinfo->clause;
	Oid			opfamily;

	Assert(indexcol < index->nkeycolumns);

	/*
	 * 历史上这段代码应付过 NULL 子句。那也许
	 * 不再可能，但不妨继续应付。
	 */
	if (clause == NULL)
		return NULL;

	/* 先检查布尔索引的情形。 */
	opfamily = index->opfamily[indexcol];
	if (IsBooleanOpfamily(opfamily))
	{
		iclause = match_boolean_index_clause(root, rinfo, indexcol, index);
		if (iclause)
			return iclause;
	}

	/*
	 * 子句必须是操作子句、函数子句、ScalarArrayOpExpr 或
	 * RowCompareExpr。或者，若索引支持，可以处理 IS
	 * NULL/NOT NULL 子句。
	 */
	if (IsA(clause, OpExpr))
	{
		return match_opclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, FuncExpr))
	{
		return match_funcclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		return match_saopclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		return match_rowcompare_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (index->amsearchnulls && IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (!nt->argisrow &&
			match_index_to_operand((Node *) nt->arg, indexcol, index))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}
	}

	return NULL;
}

/*
 * IsBooleanOpfamily
 *	  检测一个操作符族是否把布尔等值作为一种操作符来支持。
 *
 * 若操作符族 OID 在内建对象的范围内，就可以依赖
 * 对哪些内建操作符族支持这一点的硬连线知识。
 * 对扩展操作符族，别无选择，只能做一次 catcache 查找。
 */
static bool
IsBooleanOpfamily(Oid opfamily)
{
	if (opfamily < FirstNormalObjectId)
		return IsBuiltinBooleanOpfamily(opfamily);
	else
		return op_in_opfamily(BooleanEqualOperator, opfamily);
}

/*
 * match_boolean_index_clause
 *	  识别可以匹配到布尔索引的限制子句。
 *
 * 这里的想法是，对支持
 * BooleanEqualOperator 的布尔列上的索引，可以把对索引键的普通引用
 * 变成 "indexkey = true"，或把 "NOT indexkey" 变成 "indexkey = false" 等，
 * 以便用该索引的 "=" 操作符使表达式可索引。
 * 自 Postgres 8.1 起必须这样做，因为常量简化会做
 * 相反的变换；没有这段代码，就完全无法
 * 使用这样的索引。
 *
 * 仅当 IsBooleanOpfamily() 认出
 * 该索引的操作符族时才应当调用。我们检查子句是否匹配
 * 索引的键，若匹配，就建立一个合适的 IndexClause。
 */
static IndexClause *
match_boolean_index_clause(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	Node	   *clause = (Node *) rinfo->clause;
	Expr	   *op = NULL;

	/* 直接匹配？ */
	if (match_index_to_operand(clause, indexcol, index))
	{
		/* 转换成 indexkey = TRUE */
		op = make_opclause(BooleanEqualOperator, BOOLOID, false,
						   (Expr *) clause,
						   (Expr *) makeBoolConst(true, false),
						   InvalidOid, InvalidOid);
	}
	/* NOT 子句？ */
	else if (is_notclause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		if (match_index_to_operand(arg, indexcol, index))
		{
			/* 转换成 indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * 因为只考虑 WHERE 顶层的子句，也可以把
	 * indexkey IS TRUE 和 indexkey IS FALSE 转换成索引搜索。NULL 的
	 * 不同含义并不重要。
	 */
	else if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;
		Node	   *arg = (Node *) btest->arg;

		if (btest->booltesttype == IS_TRUE &&
			match_index_to_operand(arg, indexcol, index))
		{
			/* 转换成 indexkey = TRUE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(true, false),
							   InvalidOid, InvalidOid);
		}
		else if (btest->booltesttype == IS_FALSE &&
				 match_index_to_operand(arg, indexcol, index))
		{
			/* 转换成 indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * 若成功地从给定限定条件造出了操作符子句，就必须
	 * 把它包进 IndexClause。它不是有损的。
	 */
	if (op)
	{
		IndexClause *iclause = makeNode(IndexClause);

		iclause->rinfo = rinfo;
		iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		iclause->lossy = false;
		iclause->indexcol = indexcol;
		iclause->indexcols = NIL;
		return iclause;
	}

	return NULL;
}

/*
 * match_opclause_to_indexcol()
 *	  处理 match_clause_to_indexcol() 的 OpExpr 情形，
 *	  注释见该函数。
 */
static IndexClause *
match_opclause_to_indexcol(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	IndexClause *iclause;
	OpExpr	   *clause = (OpExpr *) rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/*
	 * 只有二元操作符需要申请。（理论上，规划器支持
	 * 函数可以对一元操作符做点什么，但为检查它而花的周期
	 * 似乎不值得。）
	 */
	if (list_length(clause->args) != 2)
		return NULL;

	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);
	expr_op = clause->opno;
	expr_coll = clause->inputcollid;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * 检查形如 (indexkey operator constant) 或
	 * (constant operator indexkey) 的子句。见 match_clause_to_indexcol 的说明，
	 * 关于何谓常量。
	 *
	 * 注意，对不具有这些形式之一的子句，我们不问支持函数。
	 * 原则上也许还能
	 * 做点什么，但为检查它而花的周期似乎不值得。
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->right_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * 若没找到该索引操作符族的成员，就试该操作符底层函数的支持
		 * 函数。
		 */
		set_opfuncid(clause);	/* 确保已有 opfuncid */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 0, /* indexarg 在左侧 */
											 indexcol,
											 index);
	}

	if (match_index_to_operand(rightop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->left_relids) &&
		!contain_volatile_functions(leftop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll))
		{
			Oid			comm_op = get_commutator(expr_op);

			if (OidIsValid(comm_op) &&
				op_in_opfamily(comm_op, opfamily))
			{
				RestrictInfo *commrinfo;

				/* 建立一个交换后的 OpExpr 和 RestrictInfo */
				commrinfo = commute_restrictinfo(rinfo, comm_op);

				/* 建立一个 IndexClause，把它显示为派生限定条件 */
				iclause = makeNode(IndexClause);
				iclause->rinfo = rinfo;
				iclause->indexquals = list_make1(commrinfo);
				iclause->lossy = false;
				iclause->indexcol = indexcol;
				iclause->indexcols = NIL;
				return iclause;
			}
		}

		/*
		 * 若没找到该索引操作符族的成员，就试该操作符底层函数的支持
		 * 函数。
		 */
		set_opfuncid(clause);	/* 确保已有 opfuncid */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 1, /* indexarg 在右侧 */
											 indexcol,
											 index);
	}

	return NULL;
}

/*
 * match_funcclause_to_indexcol()
 *	  处理 match_clause_to_indexcol() 的 FuncExpr 情形，
 *	  注释见该函数。
 */
static IndexClause *
match_funcclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	FuncExpr   *clause = (FuncExpr *) rinfo->clause;
	int			indexarg;
	ListCell   *lc;

	/*
	 * 对函数子句没有内建智能，但若有
	 * 规划器支持函数，它也许能做点什么。不过，为
	 * 减少白费的规划周期，仅当
	 * 至少一个参数匹配目标索引列时才调用支持函数。
	 *
	 * 注意，并不坚持其他参数是伪常量；
	 * 支持函数必须自己检查。这是为了允许
	 * 只有部分其他参数需要放进索引限定条件的情形。
	 */
	indexarg = 0;
	foreach(lc, clause->args)
	{
		Node	   *op = (Node *) lfirst(lc);

		if (match_index_to_operand(op, indexcol, index))
		{
			return get_index_clause_from_support(root,
												 rinfo,
												 clause->funcid,
												 indexarg,
												 indexcol,
												 index);
		}

		indexarg++;
	}

	return NULL;
}

/*
 * get_index_clause_from_support()
 *		若该函数有规划器支持函数，就尝试用支持函数创建的索引限定条件构造
 *		一个 IndexClause。
 */
static IndexClause *
get_index_clause_from_support(PlannerInfo *root,
							  RestrictInfo *rinfo,
							  Oid funcid,
							  int indexarg,
							  int indexcol,
							  IndexOptInfo *index)
{
	Oid			prosupport = get_func_support(funcid);
	SupportRequestIndexCondition req;
	List	   *sresult;

	if (!OidIsValid(prosupport))
		return NULL;

	req.type = T_SupportRequestIndexCondition;
	req.root = root;
	req.funcid = funcid;
	req.node = (Node *) rinfo->clause;
	req.indexarg = indexarg;
	req.index = index;
	req.indexcol = indexcol;
	req.opfamily = index->opfamily[indexcol];
	req.indexcollation = index->indexcollations[indexcol];

	req.lossy = true;			/* 默认假定 */

	sresult = (List *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	if (sresult != NIL)
	{
		IndexClause *iclause = makeNode(IndexClause);
		List	   *indexquals = NIL;
		ListCell   *lc;

		/*
		 * 支持函数 API 说它应当只交回裸
		 * 子句，因此这里必须把每一条包进 RestrictInfo。
		 */
		foreach(lc, sresult)
		{
			Expr	   *clause = (Expr *) lfirst(lc);

			indexquals = lappend(indexquals,
								 make_simple_restrictinfo(root, clause));
		}

		iclause->rinfo = rinfo;
		iclause->indexquals = indexquals;
		iclause->lossy = req.lossy;
		iclause->indexcol = indexcol;
		iclause->indexcols = NIL;

		return iclause;
	}

	return NULL;
}

/*
 * match_saopclause_to_indexcol()
 *	  处理 match_clause_to_indexcol() 的 ScalarArrayOpExpr 情形，
 *	  注释见该函数。
 */
static IndexClause *
match_saopclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Relids		right_relids;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/* 只接受 ANY 子句，不接受 ALL */
	if (!saop->useOr)
		return NULL;
	leftop = (Node *) linitial(saop->args);
	rightop = (Node *) lsecond(saop->args);
	right_relids = pull_varnos(root, rightop);
	expr_op = saop->opno;
	expr_coll = saop->inputcollid;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * 必须让索引键在左侧，并且数组参数是伪常量。
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, right_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			IndexClause *iclause = makeNode(IndexClause);

			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * 目前不问支持函数关于 ScalarArrayOpExprs 的事，
		 * 虽然原则上可以。
		 */
	}

	return NULL;
}

/*
 * match_rowcompare_to_indexcol()
 *	  处理 match_clause_to_indexcol() 的 RowCompareExpr 情形，
 *	  注释见该函数。
 *
 * 本例程检查行比较的第一列
 * 是否匹配目标索引列。这足以保证可以从该 RowCompareExpr 构造出某个
 * 索引条件 --- 其余
 * 由 expand_indexqual_rowcompare() 处理。
 */
static IndexClause *
match_rowcompare_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	bool		var_on_left;
	Oid			expr_op;
	Oid			expr_coll;

	/* 若不是在处理 B-树索引，就放弃 */
	if (index->relam != BTREE_AM_OID)
		return NULL;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * 可以坚持
	 * RowCompareExpr 中显示的操作符族与索引列的操作符族相同来做匹配，
	 * 但在存在反向排序操作符族时这可能失败：RowCompareExpr 选中正向还是
	 * 反向排序族只是碰巧。因此只看操作符，若它是
	 * 该索引操作符族的成员（若索引键
	 * 在右侧，则在交换之后）就匹配。
	 * 以后再担心是否有额外的
	 * 操作符可以匹配到该索引。
	 */
	leftop = (Node *) linitial(clause->largs);
	rightop = (Node *) linitial(clause->rargs);
	expr_op = linitial_oid(clause->opnos);
	expr_coll = linitial_oid(clause->inputcollids);

	/* 若相关，排序规则必须匹配 */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * 这些语法测试与 match_opclause_to_indexcol() 中的相同
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, pull_varnos(root, rightop)) &&
		!contain_volatile_functions(rightop))
	{
		/* 好，索引键在左侧 */
		var_on_left = true;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !bms_is_member(index_relid, pull_varnos(root, leftop)) &&
			 !contain_volatile_functions(leftop))
	{
		/* 索引键在右侧，因此交换操作符 */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		var_on_left = false;
	}
	else
		return NULL;

	/* 若操作符是正确类型的操作符族成员，就可以 */
	switch (get_op_opfamily_strategy(expr_op, opfamily))
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return expand_indexqual_rowcompare(root,
											   rinfo,
											   indexcol,
											   index,
											   expr_op,
											   var_on_left);
	}

	return NULL;
}

/*
 * expand_indexqual_rowcompare --- 展开一条
 *		是 RowCompareExpr 的索引限定条件
 *
 * 已经知道行比较的第一列匹配
 * 该索引的指定列。
 * 只要它们与索引
 * "同向" 匹配，就可以把行比较的更多列用作索引限定条件，也就是索引键都在子句的同一侧，
 * 并且操作符都是操作符族中同一类型的成员。
 *
 * 若 RowCompareExpr 的全部列都这样匹配，就按原样使用它，
 * 只是也许交换它，把索引键放到左侧。
 *
 * 否则，就建立一个缩短的 RowCompareExpr（若匹配的
 * 列超过一列）或一个简单的 OpExpr（若只有第一列匹配）。
 * 在这些情形里，修改后的子句总是 "<=" 或 ">="，
 * 即使原来是 "<" 或 ">" --- 这是为了匹配
 * 所有可能匹配原来的行所必需的。（这样做时是在建立该行比较的有损版本，
 * 因此把 lossy 设为 true。）
 *
 * 注意：这其实只是 match_rowcompare_to_indexcol 的后半部分，
 * 但为了可读性把它拆出来。
 */
static IndexClause *
expand_indexqual_rowcompare(PlannerInfo *root,
							RestrictInfo *rinfo,
							int indexcol,
							IndexOptInfo *index,
							Oid expr_op,
							bool var_on_left)
{
	IndexClause *iclause = makeNode(IndexClause);
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	int			matching_cols;
	List	   *expr_ops;
	List	   *opfamilies;
	List	   *lefttypes;
	List	   *righttypes;
	List	   *new_ops;
	List	   *var_args;
	List	   *non_var_args;

	iclause->rinfo = rinfo;
	iclause->indexcol = indexcol;

	if (var_on_left)
	{
		var_args = clause->largs;
		non_var_args = clause->rargs;
	}
	else
	{
		var_args = clause->rargs;
		non_var_args = clause->largs;
	}

	get_op_opfamily_properties(expr_op, index->opfamily[indexcol], false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);

	/* 初始化返回的、哪些索引列被使用的列表 */
	iclause->indexcols = list_make1_int(indexcol);

	/* 建立操作符、操作符族和操作符数据类型的列表，以备需要 */
	expr_ops = list_make1_oid(expr_op);
	opfamilies = list_make1_oid(index->opfamily[indexcol]);
	lefttypes = list_make1_oid(op_lefttype);
	righttypes = list_make1_oid(op_righttype);

	/*
	 * 看其余列中有多少以同样方式匹配某个索引列。
	 * 与 match_clause_to_indexcol() 一样，任何
	 * 潜在索引条件的 "另一" 侧，只要不用被索引关系的 Vars，
	 * 就可以。
	 */
	matching_cols = 1;

	while (matching_cols < list_length(var_args))
	{
		Node	   *varop = (Node *) list_nth(var_args, matching_cols);
		Node	   *constop = (Node *) list_nth(non_var_args, matching_cols);
		int			i;

		expr_op = list_nth_oid(clause->opnos, matching_cols);
		if (!var_on_left)
		{
			/* 索引键在右侧，因此交换操作符 */
			expr_op = get_commutator(expr_op);
			if (expr_op == InvalidOid)
				break;			/* 操作符不可用 */
		}
		if (bms_is_member(index->rel->relid, pull_varnos(root, constop)))
			break;				/* 不行，Var 在错误的一侧 */
		if (contain_volatile_functions(constop))
			break;				/* 不行，比较值是易变的 */

		/*
		 * Var 一侧可以匹配该索引的任何键列。
		 */
		for (i = 0; i < index->nkeycolumns; i++)
		{
			if (match_index_to_operand(varop, i, index) &&
				get_op_opfamily_strategy(expr_op,
										 index->opfamily[i]) == op_strategy &&
				IndexCollMatchesExprColl(index->indexcollations[i],
										 list_nth_oid(clause->inputcollids,
													  matching_cols)))
				break;
		}
		if (i >= index->nkeycolumns)
			break;				/* 没找到匹配 */

		/* 把列号加入返回的列表 */
		iclause->indexcols = lappend_int(iclause->indexcols, i);

		/* 把操作符信息加入列表 */
		get_op_opfamily_properties(expr_op, index->opfamily[i], false,
								   &op_strategy,
								   &op_lefttype,
								   &op_righttype);
		expr_ops = lappend_oid(expr_ops, expr_op);
		opfamilies = lappend_oid(opfamilies, index->opfamily[i]);
		lefttypes = lappend_oid(lefttypes, op_lefttype);
		righttypes = lappend_oid(righttypes, op_righttype);

		/* 这一列匹配，继续扫描 */
		matching_cols++;
	}

	/* 若全部列都可用作索引限定条件，结果就不是有损的 */
	iclause->lossy = (matching_cols != list_length(clause->opnos));

	/*
	 * 若变量在左侧并且全部
	 * 可用作索引限定条件，就可以按原样使用 rinfo->clause。
	 */
	if (var_on_left && !iclause->lossy)
		iclause->indexquals = list_make1(rinfo);
	else
	{
		/*
		 * 必须生成一个修改过的行比较（也许只是一个
		 * OpExpr）。痛苦的部分是把 < 改成 <= 或把 > 改成 >=，
		 * 因此先处理那个。
		 */
		if (!iclause->lossy)
		{
			/* 非常容易，就用交换后的操作符 */
			new_ops = expr_ops;
		}
		else if (op_strategy == BTLessEqualStrategyNumber ||
				 op_strategy == BTGreaterEqualStrategyNumber)
		{
			/* 容易，就用同样的（也许已交换的）操作符 */
			new_ops = list_truncate(expr_ops, matching_cols);
		}
		else
		{
			ListCell   *opfamilies_cell;
			ListCell   *lefttypes_cell;
			ListCell   *righttypes_cell;

			if (op_strategy == BTLessStrategyNumber)
				op_strategy = BTLessEqualStrategyNumber;
			else if (op_strategy == BTGreaterStrategyNumber)
				op_strategy = BTGreaterEqualStrategyNumber;
			else
				elog(ERROR, "unexpected strategy number %d", op_strategy);
			new_ops = NIL;
			forthree(opfamilies_cell, opfamilies,
					 lefttypes_cell, lefttypes,
					 righttypes_cell, righttypes)
			{
				Oid			opfam = lfirst_oid(opfamilies_cell);
				Oid			lefttype = lfirst_oid(lefttypes_cell);
				Oid			righttype = lfirst_oid(righttypes_cell);

				expr_op = get_opfamily_member(opfam, lefttype, righttype,
											  op_strategy);
				if (!OidIsValid(expr_op))	/* 不应当发生 */
					elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
						 op_strategy, lefttype, righttype, opfam);
				new_ops = lappend_oid(new_ops, expr_op);
			}
		}

		/* 若匹配的列超过一列，就建立一个子集行比较 */
		if (matching_cols > 1)
		{
			RowCompareExpr *rc = makeNode(RowCompareExpr);

			rc->rctype = (RowCompareType) op_strategy;
			rc->opnos = new_ops;
			rc->opfamilies = list_copy_head(clause->opfamilies,
											matching_cols);
			rc->inputcollids = list_copy_head(clause->inputcollids,
											  matching_cols);
			rc->largs = list_copy_head(var_args, matching_cols);
			rc->rargs = list_copy_head(non_var_args, matching_cols);
			iclause->indexquals = list_make1(make_simple_restrictinfo(root,
																	  (Expr *) rc));
		}
		else
		{
			Expr	   *op;

			/* 这种情形不报告索引列列表 */
			iclause->indexcols = NIL;

			op = make_opclause(linitial_oid(new_ops), BOOLOID, false,
							   copyObject(linitial(var_args)),
							   copyObject(linitial(non_var_args)),
							   InvalidOid,
							   linitial_oid(clause->inputcollids));
			iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		}
	}

	return iclause;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK ORDERING OPERATORS（检查排序操作符的例程）	----
 ****************************************************************************/

/*
 * match_pathkeys_to_index
 *		测试一个索引能否用 "排序操作符" 产生按
 *		给定路径键排序的输出。
 *
 * 若能，就返回合适的 ORDER BY 表达式列表，每条形如
 * "indexedcol operator pseudoconstant"，以及一个整数列表，给出每条子句将与之使用的
 * 索引列号（从零计）。
 * 若不能以这种方式达到该排序，就返回 NIL 列表。
 *
 * 成功时，结果列表按路径键排序，并且事实上与
 * 所请求的路径键一一对应。
 */
static void
match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
						List **orderby_clauses_p,
						List **clause_columns_p)
{
	List	   *orderby_clauses = NIL;
	List	   *clause_columns = NIL;
	ListCell   *lc1;

	*orderby_clauses_p = NIL;	/* 设置默认结果 */
	*clause_columns_p = NIL;

	/* 只有具有 amcanorderbyop 属性的索引在这里有意义 */
	if (!index->amcanorderbyop)
		return;

	foreach(lc1, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc1);
		bool		found = false;
		ListCell   *lc2;

		/*
		 * 注意：任何匹配失败，都立刻返回 NIL。
		 * 只匹配部分路径键没有价值。
		 */

		/* 路径键必须为目标操作符族请求默认排序方向 */
		if (pathkey->pk_strategy != BTLessStrategyNumber ||
			pathkey->pk_nulls_first)
			return;

		/* 若等价类是易变的，就没有希望使用索引扫描 */
		if (pathkey->pk_eclass->ec_has_volatile)
			return;

		/*
		 * 试着把等价类成员 expression(s) 匹配到索引。注意会考虑子
		 * EC 成员，但仅当它们属于目标
		 * 关系时。（与普通成员不同，同一表达式可以是
		 * 不止一个 EC 的子成员。因此，同一索引可以被
		 * 认为匹配不止一个路径键列表，在这里这是可以的。
		 * 另见 get_eclass_for_sort_expr。）
		 */
		foreach(lc2, pathkey->pk_eclass->ec_members)
		{
			EquivalenceMember *member = (EquivalenceMember *) lfirst(lc2);
			int			indexcol;

			/* 若它引用其他关系，就不可能匹配 */
			if (!bms_equal(member->em_relids, index->rel->relids))
				continue;

			/*
			 * 允许索引的任何列匹配每个路径键；它们
			 * 不必像你也许预期的那样从左到右匹配。这对
			 * GiST 是正确的，对 SP-GiST 也不要紧，因为
			 * 它反正不处理多列，而且没有其他
			 * 现有访问方法支持 amcanorderbyop。将来对其他实现也许需要不同的
			 * 逻辑。
			 */
			for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
			{
				Expr	   *expr;

				expr = match_clause_to_ordering_op(index,
												   indexcol,
												   member->em_expr,
												   pathkey->pk_opfamily);
				if (expr)
				{
					orderby_clauses = lappend(orderby_clauses, expr);
					clause_columns = lappend_int(clause_columns, indexcol);
					found = true;
					break;
				}
			}

			if (found)			/* 不想再看其余成员 */
				break;
		}

		if (!found)				/* 若这个路径键没有匹配，就失败 */
			return;
	}

	*orderby_clauses_p = orderby_clauses;	/* 成功！ */
	*clause_columns_p = clause_columns;
}

/*
 * match_clause_to_ordering_op
 *	  判断一个排序操作符表达式是否匹配一个
 *	  索引列。
 *
 *	  这与 match_clause_to_indexcol 相似，但更简单。
 *	  只关心简单的 OpExpr 情形。输入是一个正在被排序的裸
 *	  表达式，它必须形如
 *	  (indexkey op const) 或 (const op indexkey)，其中 op 是该列操作符族的排序
 *	  操作符。
 *
 * 'index' 是感兴趣的索引。
 * 'indexcol' 是 'index' 的列号（从 0 计）。
 * 'clause' 是要测试的排序表达式。
 * 'pk_opfamily' 是描述所需排序的 B-树操作符族。
 *
 * 注意，目前不考虑排序
 * 操作符结果的排序规则。在实际情形里结果类型是数值
 * 从而没有排序规则，而且若它真有排序规则，不太清楚要匹配
 * 什么。索引的排序规则应当匹配
 * 排序操作符的输入排序规则，而不是它的结果。
 *
 * 若成功，索引键在左侧时就按原样返回 'clause'，
 * 否则返回 'clause' 的交换副本。若没有匹配，返回 NULL。
 */
static Expr *
match_clause_to_ordering_op(IndexOptInfo *index,
							int indexcol,
							Expr *clause,
							Oid pk_opfamily)
{
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Oid			sortfamily;
	bool		commuted;

	Assert(indexcol < index->nkeycolumns);

	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * 子句必须是二元操作子句。
	 */
	if (!is_opclause(clause))
		return NULL;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return NULL;
	expr_op = ((OpExpr *) clause)->opno;
	expr_coll = ((OpExpr *) clause)->inputcollid;

	/*
	 * 若排序规则不对，可以立刻把整件事忘掉。
	 */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * 检查形如 (indexkey operator constant) 或
	 * (constant operator indexkey) 的子句。
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!contain_var_clause(rightop) &&
		!contain_volatile_functions(rightop))
	{
		commuted = false;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !contain_var_clause(leftop) &&
			 !contain_volatile_functions(leftop))
	{
		/* 也许匹配，但需要一个交换后的操作符 */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		commuted = true;
	}
	else
		return NULL;

	/*
	 * （交换后的）操作符是该操作符族的排序操作符吗？并且
	 * 若是，它是否产生正确的排序语义？
	 */
	sortfamily = get_op_opfamily_sortfamily(expr_op, opfamily);
	if (sortfamily != pk_opfamily)
		return NULL;

	/* 有匹配。返回该子句或其交换版本。 */
	if (commuted)
	{
		OpExpr	   *newclause = makeNode(OpExpr);

		/* 平铺复制 clause 的全部字段 */
		memcpy(newclause, clause, sizeof(OpExpr));

		/* 交换它 */
		newclause->opno = expr_op;
		newclause->opfuncid = InvalidOid;
		newclause->args = list_make2(rightop, leftop);

		clause = (Expr *) newclause;
	}

	return clause;
}


/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS（做部分索引谓词测试的例程）	----
 ****************************************************************************/

/*
 * check_index_predicates
 *		为每个索引设置由谓词导出的 IndexOptInfo 字段，
 *		这些索引属于指定关系。
 *
 * 若索引是部分的，并且其谓词对本查询得到满足，
 * 即查询的 WHERE 子句蕴含该谓词，就把 predOK 设为 true。
 *
 * indrestrictinfo 设为该关系的 baserestrictinfo 列表去掉
 * 被索引谓词蕴含的任何条件。（显然，对
 * 非部分索引，这与 baserestrictinfo 相同。）在某些情形下，使用该索引时
 * 可以从计划中丢掉这些条件。
 *
 * 曾经可能在给该关系增加更多
 * 限制之后重新运行这个，从而也许能证明更多索引可用。
 * 现在不再这样了（至少核心代码的用法不是），
 * 但这段代码仍支持它，以防扩展想摆弄
 * baserestrictinfo 列表。假定增加更多限制不能使
 * 一个索引不再 predOK。不过每次都必须重新计算 indrestrictinfo，
 * 以确保任何新加的限制在需要时进入它。
 */
void
check_index_predicates(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *clauselist;
	bool		have_partial;
	bool		is_target_rel;
	Relids		otherrels;
	ListCell   *lc;

	/* 索引只存在于基关系或 "other" 成员关系上。 */
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * 把 indrestrictinfo 列表初始化为与
	 * baserestrictinfo 相同，并检查是否有任何部分索引。若
	 * 没有，需要做的就这些。
	 */
	have_partial = false;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

		index->indrestrictinfo = rel->baserestrictinfo;
		if (index->indpred)
			have_partial = true;
	}
	if (!have_partial)
		return;

	/*
	 * 构造一份为证明
	 * index(es) 可用而可以假定为真的子句列表。该关系的限制子句
	 * 总是可用，任何 "可移到" 这个
	 * 关系的连接子句也可用。另外，可以考虑任何可由等价类导出的连接子句（按定义它们必须
	 * "可移到" 这个关系）。
	 */
	clauselist = list_copy(rel->baserestrictinfo);

	/* 扫描该关系的连接子句 */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* 检查子句能否移到这个关系 */
		if (!join_clause_is_movable_to(rinfo, rel))
			continue;

		clauselist = lappend(clauselist, rinfo);
	}

	/*
	 * 加上任何可由等值关系导出的连接子句。为 generate_join_implied_equalities 计算正确的
	 * relid 集合略为棘手，
	 * 因为该关系可能是子关系而不是真正的基关系，此时
	 * 必须从 all_query_rels 中减去其父项的 relid(s)。
	 * 另外，不得考虑只有在
	 * 能把该关系置空的外连接之后才能计算的子句。
	 */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		otherrels = bms_difference(root->all_query_rels,
								   find_childrel_parents(root, rel));
	else
		otherrels = bms_difference(root->all_query_rels, rel->relids);
	otherrels = bms_del_members(otherrels, rel->nulling_relids);

	if (!bms_is_empty(otherrels))
		clauselist =
			list_concat(clauselist,
						generate_join_implied_equalities(root,
														 bms_union(rel->relids,
																   otherrels),
														 otherrels,
														 rel,
														 NULL));

	/*
	 * 通常会从 indrestrictinfo 中去掉被部分索引的
	 * 谓词蕴含的限定条件，表示使用该索引的索引扫描计划不必
	 * 显式检查它们。不过，若
	 * 该关系是 UPDATE/DELETE/MERGE/SELECT FOR UPDATE 的目标关系，
	 * 就无法从计划中去掉这些限定条件，因为它们需要留在
	 * 计划里，以便 EvalPlanQual
	 * 测试能正确复查。也许有一天仍想从主
	 * 计划中去掉这些限定条件，并通过旁路传给 EvalPlanQual；
	 * 但目前，对目标
	 * 关系根本不去掉被蕴含的限定条件。
	 */
	is_target_rel = (bms_is_member(rel->relid, root->all_result_relids) ||
					 get_plan_rowmark(root->rowMarks, rel->relid) != NULL);

	/*
	 * 现在试着证明每个索引谓词为真，并为部分索引计算
	 * indrestrictinfo 列表。注意即使对非 predOK 索引也计算
	 * indrestrictinfo 列表；这也许显得
	 * 浪费，但也许能在 OR 子句中使用这些索引，参见
	 * generate_bitmap_or_paths()。
	 */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ListCell   *lcr;

		if (index->indpred == NIL)
			continue;			/* 这里忽略非部分索引 */

		if (!index->predOK)		/* 若已经证明可用，就不要重复工作 */
			index->predOK = predicate_implied_by(index->indpred, clauselist,
												 false);

		/* 若关系是更新目标，就保持上面设置的 indrestrictinfo */
		if (is_target_rel)
			continue;

		/* 否则把 indrestrictinfo 计算为未被蕴含的限定条件 */
		index->indrestrictinfo = NIL;
		foreach(lcr, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcr);

			/* predicate_implied_by() 假定第一个参数是不可变的 */
			if (contain_mutable_functions((Node *) rinfo->clause) ||
				!predicate_implied_by(list_make1(rinfo->clause),
									  index->indpred, false))
				index->indrestrictinfo = lappend(index->indrestrictinfo, rinfo);
		}
	}
}

/****************************************************************************
 *				----  ROUTINES TO CHECK EXTERNALLY-VISIBLE CONDITIONS（检查外部可见条件的例程）  ----
 ****************************************************************************/

/*
 * ec_member_matches_indexcol
 *	  测试一个 EquivalenceClass 成员是否匹配一个索引列。
 *
 * 这是供 generate_implied_equalities_for_column 使用的回调。
 */
static bool
ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
						   EquivalenceClass *ec, EquivalenceMember *em,
						   void *arg)
{
	IndexOptInfo *index = ((ec_member_matches_arg *) arg)->index;
	int			indexcol = ((ec_member_matches_arg *) arg)->indexcol;
	Oid			curFamily;
	Oid			curCollation;

	Assert(indexcol < index->nkeycolumns);

	curFamily = index->opfamily[indexcol];
	curCollation = index->indexcollations[indexcol];

	/*
	 * 若是 B-树索引，当其操作符族与该 EC 不
	 * 兼容时可以拒绝它，因为从该 EC 生成的子句都不能
	 * 与该索引一起使用。对非 B-树索引，不容易判断
	 * 从该 EC 生成的子句能否与该索引一起使用，因此
	 * 不检查操作符族。这也许意味着对一个
	 * 无用的 EC 返回 "true"，因此必须复查
	 * generate_implied_equalities_for_column 的结果；见
	 * match_eclass_clauses_to_index。
	 */
	if (index->relam == BTREE_AM_OID &&
		!list_member_oid(ec->ec_opfamilies, curFamily))
		return false;

	/* 不过，对所有索引类型都坚持排序规则匹配 */
	if (!IndexCollMatchesExprColl(curCollation, ec->ec_collation))
		return false;

	return match_index_to_operand((Node *) em->em_expr, indexcol, index);
}

/*
 * relation_has_unique_index_for
 *	  判断该关系是否可证明至多有一行满足
 *	  一组等值条件，因为这些条件约束了某个唯一索引的全部
 *	  列。
 *
 * 这些条件可以用两种方式之一或两者来表示：
 * 1. 一份 RestrictInfo 节点列表，调用方已经确定
 * 每个条件都是可归并连接的等值，一侧是
 * 这个关系中的表达式，另一侧是不涉及这个关系的
 * 表达式。
 * 用临时标志 outer_is_left 标明应当看
 * 哪一侧：outer_is_left 为 false 时看左侧，为 true 时看右侧。
 * 2. 一份这个关系中的表达式列表，以及对应的
 * 等值操作符列表。调用方必须已经检查这些操作符
 * 表示等值。（注意：操作符可以是跨类型的；
 * 表达式应当对应它们的 RHS 输入。）
 *
 * 调用方只需提供由连接产生的等值条件；
 * 本例程会自动加入任何可用的 baserestrictinfo 子句。
 * （注意传入的 restrictlist 会被破坏性地修改！）
 */
bool
relation_has_unique_index_for(PlannerInfo *root, RelOptInfo *rel,
							  List *restrictlist,
							  List *exprlist, List *oprlist)
{
	ListCell   *ic;

	Assert(list_length(exprlist) == list_length(oprlist));

	/* 若没有索引就短路…… */
	if (rel->indexlist == NIL)
		return false;

	/*
	 * 检查该关系的限制子句，找可用的 var = const 子句，
	 * 以便加入 restrictlist。
	 */
	foreach(ic, rel->baserestrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(ic);

		/*
		 * 注意：限制子句不会设置 can_join，但
		 * 若它有可归并连接的操作符并且
		 * 不含易变函数，就会设置 mergeopfamilies。
		 */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;			/* 不可归并连接 */

		/*
		 * 该子句肯定不引用给定关系之外的任何东西。
		 * 若任一侧是伪常量，就可以使用它。
		 */
		if (bms_is_empty(restrictinfo->left_relids))
		{
			/* 右侧是内层 */
			restrictinfo->outer_is_left = true;
		}
		else if (bms_is_empty(restrictinfo->right_relids))
		{
			/* 左侧是内层 */
			restrictinfo->outer_is_left = false;
		}
		else
			continue;

		/* 好，加入列表 */
		restrictlist = lappend(restrictlist, restrictinfo);
	}

	/* 把简单情形短路 */
	if (restrictlist == NIL && exprlist == NIL)
		return false;

	/* 检查该关系的每个索引…… */
	foreach(ic, rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(ic);
		int			c;

		/*
		 * 若索引不唯一，或不是立即强制的，或它是
		 * 部分索引，在这里就没用。由于
		 * check_index_predicates() 也会用连接谓词来
		 * 判断部分索引是否可用，我们无法利用
		 * predOK 的部分唯一索引。这里需要的证明是
		 * 在任何连接求值之前就成立的。
		 */
		if (!ind->unique || !ind->immediate || ind->indpred != NIL)
			continue;

		/*
		 * 试着在条件列表中找出每个索引列。这是
		 * O(N^2) 或更差，但预期所有列表都很短。
		 */
		for (c = 0; c < ind->nkeycolumns; c++)
		{
			bool		matched = false;
			ListCell   *lc;
			ListCell   *lc2;

			foreach(lc, restrictlist)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Node	   *rexpr;

				/*
				 * 条件的等值操作符必须是
				 * 索引操作符族的成员，否则它断言的不是该索引所要的那种
				 * 相等行为。先检查这个，
				 * 因为它大概比 match_index_to_operand() 便宜。
				 */
				if (!list_member_oid(rinfo->mergeopfamilies, ind->opfamily[c]))
					continue;

				/*
				 * XXX 将来也许也需要在这里检查排序规则。
				 * 目前假定所有排序规则都归结为同一种
				 * 相等观念。
				 */

				/* 好，看条件操作数是否匹配索引键 */
				if (rinfo->outer_is_left)
					rexpr = get_rightop(rinfo->clause);
				else
					rexpr = get_leftop(rinfo->clause);

				if (match_index_to_operand(rexpr, c, ind))
				{
					matched = true; /* 该列是唯一的 */
					break;
				}
			}

			if (matched)
				continue;

			forboth(lc, exprlist, lc2, oprlist)
			{
				Node	   *expr = (Node *) lfirst(lc);
				Oid			opr = lfirst_oid(lc2);

				/* 看表达式是否匹配索引键 */
				if (!match_index_to_operand(expr, c, ind))
					continue;

				/*
				 * 等值操作符必须是索引
				 * 操作符族的成员，否则它断言的不是该索引所要的那种
				 * 相等行为。假定调用方
				 * 已确定它是等值操作符，因此不必
				 * 比这查得更紧。
				 */
				if (!op_in_opfamily(opr, ind->opfamily[c]))
					continue;

				/*
				 * XXX 将来也许也需要在这里检查排序规则。
				 * 目前假定所有排序规则都归结为同一种
				 * 相等观念。
				 */

				matched = true; /* 该列是唯一的 */
				break;
			}

			if (!matched)
				break;			/* 没有匹配；这个索引帮不上忙 */
		}

		/* 匹配了该索引的全部键列？ */
		if (c == ind->nkeycolumns)
			return true;
	}

	return false;
}

/*
 * indexcol_is_bool_constant_for_query
 *
 * 若查询的
 * WHERE 条件把某个索引列约束为常量，那么它对排序考虑就无关。
 * 通常这意味着有限制子句 WHERE indexcol = constant，
 * 它被变成含有常量的 EquivalenceClass，并被
 * build_index_pathkeys() 认作冗余。但若索引
 * 列是布尔变量（或表达式），就不会看到
 * WHERE indexcol = constant，因为表达式预处理会把
 * 它简化成 "WHERE indexcol" 或 "WHERE NOT indexcol"。因此不会
 * 有匹配的 EquivalenceClass（除非查询还包含
 * "ORDER BY indexcol"）。为了让这些情形与非布尔值
 * 同样工作，提供本函数来检测
 * 指定索引列是否匹配一条布尔限制子句。
 */
bool
indexcol_is_bool_constant_for_query(PlannerInfo *root,
									IndexOptInfo *index,
									int indexcol)
{
	ListCell   *lc;

	/* 若索引不是布尔的，就不可能得到匹配 */
	if (!IsBooleanOpfamily(index->opfamily[indexcol]))
		return false;

	/* 检查该索引关系的每条限制子句 */
	foreach(lc, index->rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * 与 match_clause_to_indexcol 一样，绝不把伪常量匹配到
		 * 索引。（在这里这样做语义上也许可以，但
		 * 得到匹配的机会小到可以忽略，因此不要浪费周期。）
		 */
		if (rinfo->pseudoconstant)
			continue;

		/* 看能否把子句的表达式匹配到索引列 */
		if (match_boolean_index_clause(root, rinfo, indexcol, index))
			return true;
	}

	return false;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK OPERANDS（检查操作数的例程）  ----
 ****************************************************************************/

/*
 * match_index_to_operand()
 *	  索引的键与
 *	  限制或连接子句一侧操作数之间匹配的一般测试。
 *
 * operand：要与索引比较的节点树
 * indexcol：索引的列号（从 0 计）
 * index：感兴趣的索引
 *
 * 注意这里对排序规则不感兴趣；若调用方处理的操作符对此有要求，必须自己检查
 * 排序规则是否匹配。
 *
 * 导出供 selfuncs.c 使用。
 */
bool
match_index_to_operand(Node *operand,
					   int indexcol,
					   IndexOptInfo *index)
{
	int			indkey;

	/*
	 * 忽略操作数上方的任何 RelabelType 节点。这是为了
	 * 能在二进制兼容操作符的情形下应用索引扫描。注意：
	 * 可以假定最多有一个 RelabelType 节点；
	 * 若多于一个，eval_const_expressions() 会把它简化掉。
	 */
	if (operand && IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	indkey = index->indexkeys[indexcol];
	if (indkey != 0)
	{
		/*
		 * 简单索引列；操作数必须是匹配的 Var。
		 */
		if (operand && IsA(operand, Var) &&
			index->rel->relid == ((Var *) operand)->varno &&
			indkey == ((Var *) operand)->varattno &&
			((Var *) operand)->varnullingrels == NULL)
			return true;
	}
	else
	{
		/*
		 * 索引表达式；找出正确的表达式。（可以避免这次搜索，
		 * 代价是把本例程的所有调用方都弄复杂；
		 * 似乎不值得。）
		 */
		ListCell   *indexpr_item;
		int			i;
		Node	   *indexkey;

		indexpr_item = list_head(index->indexprs);
		for (i = 0; i < indexcol; i++)
		{
			if (index->indexkeys[i] == 0)
			{
				if (indexpr_item == NULL)
					elog(ERROR, "wrong number of index expressions");
				indexpr_item = lnext(index->indexprs, indexpr_item);
			}
		}
		if (indexpr_item == NULL)
			elog(ERROR, "wrong number of index expressions");
		indexkey = (Node *) lfirst(indexpr_item);

		/*
		 * 它与操作数匹配吗？同样，剥掉任何重标记。
		 */
		if (indexkey && IsA(indexkey, RelabelType))
			indexkey = (Node *) ((RelabelType *) indexkey)->arg;

		if (equal(indexkey, operand))
			return true;
	}

	return false;
}

/*
 * is_pseudo_constant_for_index()
 *	  测试给定表达式能否用作索引扫描的
 *	  比较值。
 *
 * 索引扫描的比较值不得含有任何易变函数，
 * 也不能含有该索引自己表的任何 Vars。不过其他
 * 表的 Vars 可以；那种情况下会生成一条
 * 可用于参数化索引扫描的索引限定条件。因此，这是比
 * is_pseudo_constant_clause() 更弱的条件。
 *
 * 本函数导出供规划器支持函数使用，
 * 它们能拿到 IndexOptInfo，但没有任何 RestrictInfo
 * 基础设施。它做的测试与上面的函数
 * 如 match_opclause_to_indexcol() 相同，但那些在可能时依赖
 * RestrictInfo 中关于变量成员资格的信息。
 *
 * expr：要检查的节点树
 * index：感兴趣的索引
 */
bool
is_pseudo_constant_for_index(PlannerInfo *root, Node *expr, IndexOptInfo *index)
{
	/* pull_varnos 比易变性检查便宜，因此先做那个 */
	if (bms_is_member(index->rel->relid, pull_varnos(root, expr)))
		return false;			/* 不行，含有该表的 Var */
	if (contain_volatile_functions(expr))
		return false;			/* 不行，比较值是易变的 */
	return true;
}
