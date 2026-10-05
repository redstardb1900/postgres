/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/relnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "rewrite/rewriteManip.h"
#include "parser/parse_relation.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"


typedef struct JoinHashEntry
{
	Relids		join_relids;	/* 散列键必须排在最前（MUST BE FIRST） */
	RelOptInfo *join_rel;
} JoinHashEntry;

static void build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *input_rel,
								SpecialJoinInfo *sjinfo,
								List *pushed_down_joins,
								bool can_null);
static List *build_joinrel_restrictlist(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel,
										SpecialJoinInfo *sjinfo);
static void build_joinrel_joinlist(RelOptInfo *joinrel,
								   RelOptInfo *outer_rel,
								   RelOptInfo *inner_rel);
static List *subbuild_joinrel_restrictlist(PlannerInfo *root,
										   RelOptInfo *joinrel,
										   RelOptInfo *input_rel,
										   Relids both_input_relids,
										   List *new_restrictlist);
static List *subbuild_joinrel_joinlist(RelOptInfo *joinrel,
									   List *joininfo_list,
									   List *new_joininfo);
static void set_foreign_rel_properties(RelOptInfo *joinrel,
									   RelOptInfo *outer_rel, RelOptInfo *inner_rel);
static void add_join_rel(PlannerInfo *root, RelOptInfo *joinrel);
static void build_joinrel_partition_info(PlannerInfo *root,
										 RelOptInfo *joinrel,
										 RelOptInfo *outer_rel, RelOptInfo *inner_rel,
										 SpecialJoinInfo *sjinfo,
										 List *restrictlist);
static bool have_partkey_equi_join(PlannerInfo *root, RelOptInfo *joinrel,
								   RelOptInfo *rel1, RelOptInfo *rel2,
								   JoinType jointype, List *restrictlist);
static int	match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel,
										 bool strict_op);
static void set_joinrel_partition_key_exprs(RelOptInfo *joinrel,
											RelOptInfo *outer_rel, RelOptInfo *inner_rel,
											JoinType jointype);
static void build_child_join_reltarget(PlannerInfo *root,
									   RelOptInfo *parentrel,
									   RelOptInfo *childrel,
									   int nappinfos,
									   AppendRelInfo **appinfos);


/*
 * setup_simple_rel_arrays
 *	  准备用来快速访问基关系
 *	  和 AppendRelInfos 的数组。
 */
void
setup_simple_rel_arrays(PlannerInfo *root)
{
	int			size;
	Index		rti;
	ListCell   *lc;

	/* 数组用 RT 下标（1..N）访问 */
	size = list_length(root->parse->rtable) + 1;
	root->simple_rel_array_size = size;

	/*
	 * simple_rel_array 初始化为全 NULLs，因为还没有 RelOptInfos。
	 * 稍后由对 build_simple_rel() 的调用填入。
	 */
	root->simple_rel_array = (RelOptInfo **)
		palloc0(size * sizeof(RelOptInfo *));

	/* simple_rte_array 是 rtable 列表的数组等价物 */
	root->simple_rte_array = (RangeTblEntry **)
		palloc0(size * sizeof(RangeTblEntry *));
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		root->simple_rte_array[rti++] = rte;
	}

	/* 若没有 AppendRelInfos，就不需要 append_rel_array */
	if (root->append_rel_list == NIL)
	{
		root->append_rel_array = NULL;
		return;
	}

	root->append_rel_array = (AppendRelInfo **)
		palloc0(size * sizeof(AppendRelInfo *));

	/*
	 * append_rel_array 填入已经存在的 AppendRelInfos，
	 * 目前它们只可能来自 UNION ALL 的展平。继承展开时
	 * 我们以后可能再加，但正确更新该数组
	 * 是展开代码的责任。
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
		int			child_relid = appinfo->child_relid;

		/* 健全性检查 */
		Assert(child_relid < size);

		if (root->append_rel_array[child_relid])
			elog(ERROR, "child relation already exists");

		root->append_rel_array[child_relid] = appinfo;
	}
}

/*
 * expand_planner_arrays
 *		把 PlannerInfo 的按 RTE 数组扩大 add_size 个成员，
 *		并把新加入的项初始化为 NULLs。
 *
 * 注意：这会使 append_rel_array 即使原先没有分配也被分配。
 * 对当前用途没问题，因为我们只在添加子关系时调用它，
 * 而子关系总是带有 AppendRelInfos。
 */
void
expand_planner_arrays(PlannerInfo *root, int add_size)
{
	int			new_size;

	Assert(add_size > 0);

	new_size = root->simple_rel_array_size + add_size;

	root->simple_rel_array =
		repalloc0_array(root->simple_rel_array, RelOptInfo *, root->simple_rel_array_size, new_size);

	root->simple_rte_array =
		repalloc0_array(root->simple_rte_array, RangeTblEntry *, root->simple_rel_array_size, new_size);

	if (root->append_rel_array)
		root->append_rel_array =
			repalloc0_array(root->append_rel_array, AppendRelInfo *, root->simple_rel_array_size, new_size);
	else
		root->append_rel_array =
			palloc0_array(AppendRelInfo *, new_size);

	root->simple_rel_array_size = new_size;
}

/*
 * build_simple_rel
 *	  为基关系或 other 关系构造一个新的 RelOptInfo。
 */
RelOptInfo *
build_simple_rel(PlannerInfo *root, int relid, RelOptInfo *parent)
{
	RelOptInfo *rel;
	RangeTblEntry *rte;

	/* 该 Rel 不应已经存在 */
	Assert(relid > 0 && relid < root->simple_rel_array_size);
	if (root->simple_rel_array[relid] != NULL)
		elog(ERROR, "rel %d already exists", relid);

	/* 取出该关系的 RTE */
	rte = root->simple_rte_array[relid];
	Assert(rte != NULL);

	rel = makeNode(RelOptInfo);
	rel->reloptkind = parent ? RELOPT_OTHER_MEMBER_REL : RELOPT_BASEREL;
	rel->relids = bms_make_singleton(relid);
	rel->rows = 0;
	/* 仅当并非要取出全部元组时，较低的启动代价才有意义 */
	rel->consider_startup = (root->tuple_fraction > 0);
	rel->consider_param_startup = false;	/* 以后可能会改 */
	rel->consider_parallel = false; /* 以后可能会改 */
	rel->reltarget = create_empty_pathtarget();
	rel->pathlist = NIL;
	rel->ppilist = NIL;
	rel->partial_pathlist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_unique_path = NULL;
	rel->cheapest_parameterized_paths = NIL;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr、max_attr、attr_needed、attr_widths 在下面设置 */
	rel->lateral_vars = NIL;
	rel->indexlist = NIL;
	rel->statlist = NIL;
	rel->pages = 0;
	rel->tuples = 0;
	rel->allvisfrac = 0;
	rel->eclass_indexes = NULL;
	rel->subroot = NULL;
	rel->subplan_params = NIL;
	rel->rel_parallel_workers = -1; /* 在 get_relation_info 里建立 */
	rel->amflags = 0;
	rel->serverid = InvalidOid;
	if (rte->rtekind == RTE_RELATION)
	{
		Assert(parent == NULL ||
			   parent->rtekind == RTE_RELATION ||
			   parent->rtekind == RTE_SUBQUERY);

		/*
		 * 对任何 RELATION rte，我们需要一个 userid 来检查
		 * 权限访问。基关系直接使用自己的
		 * RTEPermissionInfo 的 checkAsUser。
		 *
		 * 对 otherrel，通常没有 RTEPermissionInfo，因此用
		 * 父关系的，父关系通常有一份。例外是
		 * 父关系是子查询，那时 otherrel 会有自己的。
		 */
		if (rel->reloptkind == RELOPT_BASEREL ||
			(rel->reloptkind == RELOPT_OTHER_MEMBER_REL &&
			 parent->rtekind == RTE_SUBQUERY))
		{
			RTEPermissionInfo *perminfo;

			perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
			rel->userid = perminfo->checkAsUser;
		}
		else
			rel->userid = parent->userid;
	}
	else
		rel->userid = InvalidOid;
	rel->useridiscurrent = false;
	rel->fdwroutine = NULL;
	rel->fdw_private = NULL;
	rel->unique_for_rels = NIL;
	rel->non_unique_for_rels = NIL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->baserestrict_min_security = UINT_MAX;
	rel->joininfo = NIL;
	rel->has_eclass_joins = false;
	rel->consider_partitionwise_join = false;	/* 以后可能会改 */
	rel->part_scheme = NULL;
	rel->nparts = -1;
	rel->boundinfo = NULL;
	rel->partbounds_merged = false;
	rel->partition_qual = NIL;
	rel->part_rels = NULL;
	rel->live_parts = NULL;
	rel->all_partrels = NULL;
	rel->partexprs = NULL;
	rel->nullable_partexprs = NULL;

	/*
	 * 把各种信息沿着继承层次往下传。
	 */
	if (parent)
	{
		/* 我们保留指向直接父关系和最顶层父关系的反向链接。 */
		rel->parent = parent;
		rel->top_parent = parent->top_parent ? parent->top_parent : parent;
		rel->top_parent_relids = rel->top_parent->relids;

		/*
		 * 子关系位于与其父关系相同的外连接之下。（我们
		 * 假定父关系的这项信息已经算好。）
		 */
		rel->nulling_relids = parent->nulling_relids;

		/*
		 * 也把 LATERAL 引用信息从 appendrel 父
		 * 关系传播到其子关系。我们有意让每个子关系
		 * 具有相同的最小参数化，尽管很有可能
		 * 有些并不引用全部 LATERAL 关系。这是因为
		 * 父关系的任何 append 路径反正都得让每个
		 * 子关系具有相同的参数化，多强制几次
		 * reparameterize_path() 调用也没有价值。同样，
		 * 对父关系的 LATERAL 引用会阻止每个子关系
		 * 使用本可移动的连接关系。
		 *
		 * 子关系自己也可能有子关系，那时
		 * 最顶层父关系的 LATERAL 信息会一直传到最下面。
		 */
		rel->direct_lateral_relids = parent->direct_lateral_relids;
		rel->lateral_relids = parent->lateral_relids;
		rel->lateral_referencers = parent->lateral_referencers;
	}
	else
	{
		rel->parent = NULL;
		rel->top_parent = NULL;
		rel->top_parent_relids = NULL;
		rel->nulling_relids = NULL;
		rel->direct_lateral_relids = NULL;
		rel->lateral_relids = NULL;
		rel->lateral_referencers = NULL;
	}

	/* 检查 rtable 项的类型 */
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 表：从系统目录取出统计信息 */
			get_relation_info(root, rte->relid, rte->inh, rel);
			break;
		case RTE_SUBQUERY:
		case RTE_FUNCTION:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:

			/*
			 * 子查询、函数、tablefunc、值列表、CTE 或 ENR：设置
			 * 属性范围和数组。
			 *
			 * 注意：范围包含 0，以支持整行 Var。
			 */
			rel->min_attr = 0;
			rel->max_attr = list_length(rte->eref->colnames);
			rel->attr_needed = (Relids *)
				palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
			rel->attr_widths = (int32 *)
				palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));
			break;
		case RTE_RESULT:
			/* RTE_RESULT 没有列，也不可能有整行 Var */
			rel->min_attr = 0;
			rel->max_attr = -1;
			rel->attr_needed = NULL;
			rel->attr_widths = NULL;
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) rte->rtekind);
			break;
	}

	/*
	 * 把父关系的限定条件复制到子关系，并做适当的变量
	 * 替换。若出现常量假或 NULL 子句，可以立刻把
	 * 子关系标成哑。必须马上做，这样在
	 * expand_partitioned_rtentry 里递归时裁剪才正确。
	 */
	if (parent)
	{
		AppendRelInfo *appinfo = root->append_rel_array[relid];

		Assert(appinfo != NULL);
		if (!apply_child_basequals(root, parent, rel, rte, appinfo))
		{
			/*
			 * 某条限制子句在替换后化成常量 FALSE 或 NULL，
			 * 因此不必扫描这个子关系。
			 */
			mark_dummy_rel(rel);
		}
	}

	/* 把完成的结构存进查询的 simple_rel_array */
	root->simple_rel_array[relid] = rel;

	return rel;
}

/*
 * find_base_rel
 *	  查找必须已经存在的基关系或 otherrel 项。
 */
RelOptInfo *
find_base_rel(PlannerInfo *root, int relid)
{
	RelOptInfo *rel;

	Assert(relid > 0);

	if (relid < root->simple_rel_array_size)
	{
		rel = root->simple_rel_array[relid];
		if (rel)
			return rel;
	}

	elog(ERROR, "no relation entry for relid %d", relid);

	return NULL;				/* 消除编译器告警 */
}

/*
 * find_base_rel_ignore_join
 *	  查找必须已经存在的基关系或 otherrel 项。
 *
 * 与 find_base_rel 不同，若 relid 引用的是外连接，这里
 * 返回 NULL 而不是报错。这对必须处理同时包含基关系
 * 和外连接的 relid 集合的调用方
 * 比较方便。
 */
RelOptInfo *
find_base_rel_ignore_join(PlannerInfo *root, int relid)
{
	Assert(relid > 0);

	if (relid < root->simple_rel_array_size)
	{
		RelOptInfo *rel;
		RangeTblEntry *rte;

		rel = root->simple_rel_array[relid];
		if (rel)
			return rel;

		/*
		 * 这里可以直接返回 NULL，但为了调试，最好
		 * 实际验证这个 relid 是外连接，而不是
		 * 某种怪异的东西。
		 */
		rte = root->simple_rte_array[relid];
		if (rte && rte->rtekind == RTE_JOIN && rte->jointype != JOIN_INNER)
			return NULL;
	}

	elog(ERROR, "no relation entry for relid %d", relid);

	return NULL;				/* 消除编译器告警 */
}

/*
 * build_join_rel_hash
 *	  为连接关系构造辅助散列表。
 */
static void
build_join_rel_hash(PlannerInfo *root)
{
	HTAB	   *hashtab;
	HASHCTL		hash_ctl;
	ListCell   *l;

	/* 创建散列表 */
	hash_ctl.keysize = sizeof(Relids);
	hash_ctl.entrysize = sizeof(JoinHashEntry);
	hash_ctl.hash = bitmap_hash;
	hash_ctl.match = bitmap_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	hashtab = hash_create("JoinRelHashTable",
						  256L,
						  &hash_ctl,
						  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* 插入全部已经存在的连接关系 */
	foreach(l, root->join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(l);
		JoinHashEntry *hentry;
		bool		found;

		hentry = (JoinHashEntry *) hash_search(hashtab,
											   &(rel->relids),
											   HASH_ENTER,
											   &found);
		Assert(!found);
		hentry->join_rel = rel;
	}

	root->join_rel_hash = hashtab;
}

/*
 * find_join_rel
 *	  返回与 relids（一组 RT 下标）对应的关系项，
 *	  若不存在则返回 NULL。这是给连接关系用的。
 */
RelOptInfo *
find_join_rel(PlannerInfo *root, Relids relids)
{
	/*
	 * 当列表变得过长时，改用散列查找。阈值
	 * 是随意定的，而且只有这里知道。
	 */
	if (!root->join_rel_hash && list_length(root->join_rel_list) > 32)
		build_join_rel_hash(root);

	/*
	 * 按情况使用散列表查找或线性搜索。
	 *
	 * 注意：看似多余的 hashkey 变量是为了避免取
	 * relids 的地址；除非编译器极其聪明，否则那样做
	 * 会把 relids 挤出寄存器，从而可能拖慢
	 * 列表搜索的情形。
	 */
	if (root->join_rel_hash)
	{
		Relids		hashkey = relids;
		JoinHashEntry *hentry;

		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &hashkey,
											   HASH_FIND,
											   NULL);
		if (hentry)
			return hentry->join_rel;
	}
	else
	{
		ListCell   *l;

		foreach(l, root->join_rel_list)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(l);

			if (bms_equal(rel->relids, relids))
				return rel;
		}
	}

	return NULL;
}

/*
 * set_foreign_rel_properties
 *		若外侧和内侧关系是属于同一服务器、并且被指派给
 *		同一用户做访问权限检查的外部表（或连接），
 *		就设置外部连接的相关成员。
 *
 * 除了 userid 精确匹配，也允许一侧
 * userid 为零（表示当前用户）、另一侧显式
 * userid 恰好等于当前用户的情形；但那时，把该
 * 连接下推只对当前用户有效。useridiscurrent 成员
 * 记录我们是否不得不为这个连接或任何
 * 子连接做了这种假定。
 *
 * 否则这些成员保持无效，因此不会对这个连接关系
 * 调用 GetForeignJoinPaths。
 */
static void
set_foreign_rel_properties(RelOptInfo *joinrel, RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel)
{
	if (OidIsValid(outer_rel->serverid) &&
		inner_rel->serverid == outer_rel->serverid)
	{
		if (inner_rel->userid == outer_rel->userid)
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = outer_rel->useridiscurrent || inner_rel->useridiscurrent;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(inner_rel->userid) &&
				 outer_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(outer_rel->userid) &&
				 inner_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = inner_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
	}
}

/*
 * add_join_rel
 *		把给定的连接关系加进给定 PlannerInfo 的连接关系
 *		列表。若有辅助散列表，也加进去。
 */
static void
add_join_rel(PlannerInfo *root, RelOptInfo *joinrel)
{
	/* GEQO 需要把新的连接关系追加到列表末尾！ */
	root->join_rel_list = lappend(root->join_rel_list, joinrel);

	/* 若有辅助散列表，就存进去。 */
	if (root->join_rel_hash)
	{
		JoinHashEntry *hentry;
		bool		found;

		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &(joinrel->relids),
											   HASH_ENTER,
											   &found);
		Assert(!found);
		hentry->join_rel = joinrel;
	}
}

/*
 * build_join_rel
 *	  返回与两个给定关系之并对应的关系项，
 *	  若尚不存在就新建一项。
 *
 * joinrelids 是唯一标识该连接的 Relids 集合。
 * outer_rel 和 inner_rel 是待连接关系的
 *		关系节点。
 * sjinfo：连接的上下文信息。
 * pushed_down_joins：现在已经完成的任何下推外连接。
 * restrictlist_ptr：结果变量。若非 NULL，*restrictlist_ptr
 *		收到适用于这一对
 *		可连接关系的 RestrictInfo 节点列表。
 *
 * restrictlist_ptr 让这个例程的 API 有点别扭，但它省下了
 * 对 restrictlist 的重复计算……
 */
RelOptInfo *
build_join_rel(PlannerInfo *root,
			   Relids joinrelids,
			   RelOptInfo *outer_rel,
			   RelOptInfo *inner_rel,
			   SpecialJoinInfo *sjinfo,
			   List *pushed_down_joins,
			   List **restrictlist_ptr)
{
	RelOptInfo *joinrel;
	List	   *restrictlist;

	/* 本函数只应用于父关系之间的连接。 */
	Assert(!IS_OTHER_REL(outer_rel) && !IS_OTHER_REL(inner_rel));

	/*
	 * 看看这一组基关系是否已经有连接关系。
	 */
	joinrel = find_join_rel(root, joinrelids);

	if (joinrel)
	{
		/*
		 * 是的，因此只需算出这一对
		 * 组成关系的 restrictlist。
		 */
		if (restrictlist_ptr)
			*restrictlist_ptr = build_joinrel_restrictlist(root,
														   joinrel,
														   outer_rel,
														   inner_rel,
														   sjinfo);
		return joinrel;
	}

	/*
	 * 没有，那就建一个。
	 */
	joinrel = makeNode(RelOptInfo);
	joinrel->reloptkind = RELOPT_JOINREL;
	joinrel->relids = bms_copy(joinrelids);
	joinrel->rows = 0;
	/* 仅当并非要取出全部元组时，较低的启动代价才有意义 */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	/* 从子关系初始化 direct_lateral_relids；下面再把它补完 */
	joinrel->direct_lateral_relids =
		bms_union(outer_rel->direct_lateral_relids,
				  inner_rel->direct_lateral_relids);
	joinrel->lateral_relids = min_join_parameterization(root, joinrel->relids,
														outer_rel, inner_rel);
	joinrel->relid = 0;			/* 表示不是基关系 */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->nulling_relids = NULL;
	joinrel->lateral_vars = NIL;
	joinrel->lateral_referencers = NULL;
	joinrel->indexlist = NIL;
	joinrel->statlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->eclass_indexes = NULL;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->rel_parallel_workers = -1;
	joinrel->amflags = 0;
	joinrel->serverid = InvalidOid;
	joinrel->userid = InvalidOid;
	joinrel->useridiscurrent = false;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->unique_for_rels = NIL;
	joinrel->non_unique_for_rels = NIL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->baserestrict_min_security = UINT_MAX;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* 以后可能会改 */
	joinrel->parent = NULL;
	joinrel->top_parent = NULL;
	joinrel->top_parent_relids = NULL;
	joinrel->part_scheme = NULL;
	joinrel->nparts = -1;
	joinrel->boundinfo = NULL;
	joinrel->partbounds_merged = false;
	joinrel->partition_qual = NIL;
	joinrel->part_rels = NULL;
	joinrel->live_parts = NULL;
	joinrel->all_partrels = NULL;
	joinrel->partexprs = NULL;
	joinrel->nullable_partexprs = NULL;

	/* 计算与外部关系有关的信息。 */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/*
	 * 用恰好需要从这次连接输出的 Var 和 PHVs
	 * 填连接关系的 tlist（也就是更高层连接子句或最终
	 * 输出所需要的）。
	 *
	 * 注意：连接关系的 tlist 顺序取决于我们第一次尝试用哪一对
	 * 外侧和内侧关系来建它。但内容应当
	 * 无论怎样都相同。
	 */
	build_joinrel_tlist(root, joinrel, outer_rel, sjinfo, pushed_down_joins,
						(sjinfo->jointype == JOIN_FULL));
	build_joinrel_tlist(root, joinrel, inner_rel, sjinfo, pushed_down_joins,
						(sjinfo->jointype != JOIN_INNER));
	add_placeholders_to_joinrel(root, joinrel, outer_rel, inner_rel, sjinfo);

	/*
	 * add_placeholders_to_joinrel 也负责把这里计算出的任何
	 * PlaceHolderVars 的 ph_lateral 集合加进 direct_lateral_relids，因此
	 * 现在可以把那一项算完。这很像在 min_join_parameterization 里
	 * 计算传递闭包的 lateral_relids，
	 * 只不过这里必须考虑新增的 PHVs。
	 */
	joinrel->direct_lateral_relids =
		bms_del_members(joinrel->direct_lateral_relids, joinrel->relids);

	/*
	 * 为新连接关系构造限制子句和连接子句列表。（调用方
	 * 也许需要、也许不需要 restrictlist，但我无论如何都要它，
	 * 供 set_joinrel_size_estimates() 使用。）
	 */
	restrictlist = build_joinrel_restrictlist(root, joinrel,
											  outer_rel, inner_rel,
											  sjinfo);
	if (restrictlist_ptr)
		*restrictlist_ptr = restrictlist;
	build_joinrel_joinlist(joinrel, outer_rel, inner_rel);

	/*
	 * 这里也适合检查该连接关系是否有任何
	 * 待处理的 EquivalenceClass 连接。
	 */
	joinrel->has_eclass_joins = has_relevant_eclass_joinclause(root, joinrel);

	/* 存储分区信息。 */
	build_joinrel_partition_info(root, joinrel, outer_rel, inner_rel, sjinfo,
								 restrictlist);

	/*
	 * 设置该连接关系大小的估计。
	 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/*
	 * 若这个连接关系有可能在并行工作进程内被
	 * 扫描，就设置 consider_parallel 标志。若 inner_rel 或
	 * outer_rel 任一该标志为假，则连接关系也必须
	 * 为假。即便两者都为真，目标列表或限定条件里
	 * 仍可能有并行受限的表达式。
	 *
	 * 注意，若这个关系里不止两个关系，它们可以
	 * 以任意方式分到 inner_rel 和 outer_rel 里。我们
	 * 假定这无关紧要，因为无论取哪一种，在建到这个
	 * 连接关系的过程中都会碰到同样的基关系和
	 * 连接子句；因此无论怎么到达这里，这里的决定
	 * 都应当相同。
	 */
	if (inner_rel->consider_parallel && outer_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) restrictlist) &&
		is_parallel_safe(root, (Node *) joinrel->reltarget->exprs))
		joinrel->consider_parallel = true;

	/* 把连接关系加进 PlannerInfo。 */
	add_join_rel(root, joinrel);

	/*
	 * 另外，若动态规划的连接搜索正在进行，把新连接关系
	 * 加进相应的子列表。注意：你也许以为对成员数的 Assert
	 * 应当是相等，但某些第 1 层关系可能
	 * 已经是连接关系，所以只能 Assert <=。
	 */
	if (root->join_rel_level)
	{
		Assert(root->join_cur_level > 0);
		Assert(root->join_cur_level <= bms_num_members(joinrel->relids));
		root->join_rel_level[root->join_cur_level] =
			lappend(root->join_rel_level[root->join_cur_level], joinrel);
	}

	return joinrel;
}

/*
 * build_child_join_rel
 *	  构造表示给定两个子关系之间连接的 RelOptInfo。
 *
 * outer_rel 和 inner_rel 是正在
 *		连接的子关系的 RelOptInfos。
 * parent_joinrel 是表示父关系之间
 *		连接的 RelOptInfo。新 RelOptInfo 的某些成员
 *		由翻译该 RelOptInfo 的对应成员产生。
 * restrictlist：适用于这一对可连接关系的
 *		RestrictInfo 节点列表。
 * sjinfo：子连接的连接类型细节。
 */
RelOptInfo *
build_child_join_rel(PlannerInfo *root, RelOptInfo *outer_rel,
					 RelOptInfo *inner_rel, RelOptInfo *parent_joinrel,
					 List *restrictlist, SpecialJoinInfo *sjinfo)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);
	AppendRelInfo **appinfos;
	int			nappinfos;

	/* 只有 other 关系之间的连接才会到这里。 */
	Assert(IS_OTHER_REL(outer_rel) && IS_OTHER_REL(inner_rel));

	/* 父连接关系应当已经设置了 consider_partitionwise_join。 */
	Assert(parent_joinrel->consider_partitionwise_join);

	/*
	 * 找出子基关系的 AppendRelInfo 结构。计算子连接的
	 * relid 集合时需要它们，以后把 Var 映射到
	 * 子关系时也需要。
	 */
	appinfos = find_appinfos_by_relids(root,
									   bms_union(outer_rel->relids,
												 inner_rel->relids),
									   &nappinfos);

	joinrel->reloptkind = RELOPT_OTHER_JOINREL;
	joinrel->relids = adjust_child_relids(parent_joinrel->relids,
										  nappinfos, appinfos);
	joinrel->rows = 0;
	/* 仅当并非要取出全部元组时，较低的启动代价才有意义 */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	joinrel->direct_lateral_relids = NULL;
	joinrel->lateral_relids = NULL;
	joinrel->relid = 0;			/* 表示不是基关系 */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->nulling_relids = NULL;
	joinrel->lateral_vars = NIL;
	joinrel->lateral_referencers = NULL;
	joinrel->indexlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->eclass_indexes = NULL;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->amflags = 0;
	joinrel->serverid = InvalidOid;
	joinrel->userid = InvalidOid;
	joinrel->useridiscurrent = false;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* 以后可能会改 */
	joinrel->parent = parent_joinrel;
	joinrel->top_parent = parent_joinrel->top_parent ? parent_joinrel->top_parent : parent_joinrel;
	joinrel->top_parent_relids = joinrel->top_parent->relids;
	joinrel->part_scheme = NULL;
	joinrel->nparts = -1;
	joinrel->boundinfo = NULL;
	joinrel->partbounds_merged = false;
	joinrel->partition_qual = NIL;
	joinrel->part_rels = NULL;
	joinrel->live_parts = NULL;
	joinrel->all_partrels = NULL;
	joinrel->partexprs = NULL;
	joinrel->nullable_partexprs = NULL;

	/* 计算与外部关系有关的信息。 */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/* 建立 reltarget 结构 */
	build_child_join_reltarget(root, parent_joinrel, joinrel,
							   nappinfos, appinfos);

	/* 构造 joininfo 列表。 */
	joinrel->joininfo = (List *) adjust_appendrel_attrs(root,
														(Node *) parent_joinrel->joininfo,
														nappinfos,
														appinfos);

	/*
	 * 子连接里引用的 LATERAL relid 与父关系里
	 * 引用的相同。
	 */
	joinrel->direct_lateral_relids = (Relids) bms_copy(parent_joinrel->direct_lateral_relids);
	joinrel->lateral_relids = (Relids) bms_copy(parent_joinrel->lateral_relids);

	/*
	 * 若父连接关系有待处理的等价类，
	 * 子关系也有。
	 */
	joinrel->has_eclass_joins = parent_joinrel->has_eclass_joins;

	/* 分区之间的连接本身是否也分区？ */
	build_joinrel_partition_info(root, joinrel, outer_rel, inner_rel, sjinfo,
								 restrictlist);

	/* 若父关系并行安全，则子连接关系也并行安全。 */
	joinrel->consider_parallel = parent_joinrel->consider_parallel;

	/* 设置子连接关系大小的估计。 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/* 连接只构造一次。 */
	Assert(!find_join_rel(root, joinrel->relids));

	/* 把该关系加进 PlannerInfo。 */
	add_join_rel(root, joinrel);

	/*
	 * 我们可能需要与子连接对应的 EquivalenceClass 成员，
	 * 以便表示它的排序路径键。和基关系的子关系一样，
	 * 除非有相关的等价类连接（意味着可能做归并连接）
	 * 或要按其排序的路径键，否则不该需要这个。
	 */
	if (joinrel->has_eclass_joins || has_useful_pathkeys(root, parent_joinrel))
		add_child_join_rel_equivalences(root,
										nappinfos, appinfos,
										parent_joinrel, joinrel);

	pfree(appinfos);

	return joinrel;
}

/*
 * min_join_parameterization
 *
 * 确定连接关系可能的最小参数化，也就是它所包含的
 * 对其他关系的 LATERAL 引用集合。我们把这个值存在
 * 该连接的 RelOptInfo 里。这个函数从 build_join_rel()
 * 拆出来，是因为 join_is_legal() 需要该值来检查候选连接。
 */
Relids
min_join_parameterization(PlannerInfo *root,
						  Relids joinrelids,
						  RelOptInfo *outer_rel,
						  RelOptInfo *inner_rel)
{
	Relids		result;

	/*
	 * 基本上只需要各输入的 lateral_relids 之并，
	 * 再去掉已经在该连接里的部分。
	 *
	 * 这样计算结果并不一眼就能看出是对的，
	 * 因为看起来我们忽略了本应在该连接上计算、
	 * 却不在任一输入里的 PlaceHolderVars 可能有的 LATERAL 引用。
	 * 不过，因为 create_lateral_join_info() 已经把所有这类
	 * PHV 引用记到该连接的每个成员基关系上，它们
	 * 已经算进各输入的 lateral_relids。同样，这里
	 * 也不必操心传递闭包，因为那已经在原先基关系的
	 * lateral_relids 里算过了。
	 */
	result = bms_union(outer_rel->lateral_relids, inner_rel->lateral_relids);
	result = bms_del_members(result, joinrelids);
	return result;
}

/*
 * build_joinrel_tlist
 *	  根据一个输入关系构造连接关系的目标列表。
 *	  （调用两次，以处理两个输入关系。）
 *
 * 连接的目标列表包括其成员关系中、在该连接之上
 * 仍会用到的全部 Var。本子程序把指定输入关系
 * tlist 里的所有这类 Var 加进连接关系的 tlist。
 * 输入关系发出的任何 PlaceHolderVars 也同样处理。
 *
 * 我们也计算该连接输出的预期宽度，利用
 * set_rel_width() 在基关系层缓存的数据。
 *
 * 若该连接是可能把来自这个输入关系的 Var 置空的外连接，
 * 就把 can_null 传为真。若如此，我们（通常）会把该连接的 relid
 * 加进从该输入冒上来的 Var 和 PHVs 的置空位图。
 *
 * 构造外连接的目标列表时，若该外连接曾按
 * 外连接恒等式 3 与另一个交换过
 * （见 optimizer/README），必须采取措施，确保输出的
 * Var 具有两个连接按语法顺序执行时
 * 会有的相同置空位图；否则匹配不上
 * 查询树更高处出现的 Var。匹配语法顺序这条规则的例外是：
 * 按恒等式 3 把一个外连接下推进另一个的 RHS 时，
 * 在如今位于上层的外连接也完成之前，没法把其 Var 标成已置空。
 * 因此要做三件事：
 *
 * 首先，仅当该外连接已经完整执行、且该 Var 或 PHV 确实
 * 来自该外连接在语法上可空的一侧（或若干侧）时，
 * 才把该外连接的 relid 加进置空位图。
 * 这覆盖了我们可能做过如下变换的情形：
 *		(A leftjoin B on (Pab)) leftjoin C on (Pbc)
 * to
 *		A leftjoin (B leftjoin C on (Pbc)) on (Pab)
 * 这里下推后的 B/C 连接还没法把 C 的列标成已置空，
 * 而如今位于上层的 A/B 连接不得自行把 C 的列标成已置空。
 *
 * 其次，对 pushed_down_joins 里列出的每个 SpecialJoinInfo
 * 做同样的操作（在这个例子里，当我们处在如今位于上层的
 * A/B 连接时，那就是 B/C 连接）。这让如今位于上层的连接
 * 能完成对现在已有完全有效值的 C 的 Var 的标记。
 *
 * 第三，sjinfo->commute_above_r 里已经属于
 * 该连接关系的任何 relid，都加进可空 Var 和 PHVs 的置空位图。
 * 这覆盖了我们把下面这种写法反过来实现的情形：
 *		A leftjoin (B leftjoin C on (Pbc)) on (Pab)
 * as
 *		(A leftjoin B on (Pab)) leftjoin C on (Pbc)
 * B/C 连接发出的 C 的列，必须显示成同时被
 * B/C 和 A/B 两个连接置空，即使它们并没有在物理上穿过
 * A/B 连接。
 */
static void
build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *input_rel,
					SpecialJoinInfo *sjinfo,
					List *pushed_down_joins,
					bool can_null)
{
	Relids		relids = joinrel->relids;
	ListCell   *vars;
	ListCell   *lc;

	foreach(vars, input_rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(vars);

		/*
		 * 对 PlaceHolderVar，必须去查 PlaceHolderInfo。
		 */
		if (IsA(var, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) var;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			/* 在这个连接关系之上是否仍然需要？ */
			if (bms_nonempty_difference(phinfo->ph_needed, relids))
			{
				/*
				 * 是的，把它加进输出。若这次连接可能把
				 * 这个输入置空，就必须更新该 PHV 的 phnullingrels，
				 * 也就是要做一份副本。
				 */
				if (can_null)
				{
					phv = copyObject(phv);
					/* 见上面的注释以理解这段逻辑 */
					if (sjinfo->ojrelid != 0 &&
						bms_is_member(sjinfo->ojrelid, relids) &&
						(bms_is_subset(phv->phrels, sjinfo->syn_righthand) ||
						 (sjinfo->jointype == JOIN_FULL &&
						  bms_is_subset(phv->phrels, sjinfo->syn_lefthand))))
						phv->phnullingrels = bms_add_member(phv->phnullingrels,
															sjinfo->ojrelid);
					foreach(lc, pushed_down_joins)
					{
						SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

						Assert(bms_is_member(othersj->ojrelid, relids));
						if (bms_is_subset(phv->phrels, othersj->syn_righthand))
							phv->phnullingrels = bms_add_member(phv->phnullingrels,
																othersj->ojrelid);
					}
					phv->phnullingrels =
						bms_join(phv->phnullingrels,
								 bms_intersect(sjinfo->commute_above_r,
											   relids));
				}

				joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
													phv);
				/* 把预先算好的结果冒上来，代价为零 */
				joinrel->reltarget->width += phinfo->ph_width;
			}
			continue;
		}

		/*
		 * 否则，基关系或连接关系目标列表里的东西应当是
		 * Var。（更一般的情形只会出现在 appendrel 的子
		 * 关系里，这里永远看不到。）
		 */
		if (!IsA(var, Var))
			elog(ERROR, "unexpected node type in rel targetlist: %d",
				 (int) nodeTag(var));

		if (var->varno == ROWID_VAR)
		{
			/* UPDATE/DELETE/MERGE 的行标识 Var 总是需要的 */
			RowIdentityVarInfo *ridinfo = (RowIdentityVarInfo *)
				list_nth(root->row_identity_vars, var->varattno - 1);

			/* 用 RowIdentityVarInfo 更新 reltarget 的宽度估计 */
			joinrel->reltarget->width += ridinfo->rowidwidth;
		}
		else
		{
			RelOptInfo *baserel;
			int			ndx;

			/* 取得该 Var 原来的基关系 */
			baserel = find_base_rel(root, var->varno);

			/* 在这个连接关系之上是否仍然需要？ */
			ndx = var->varattno - baserel->min_attr;
			if (!bms_nonempty_difference(baserel->attr_needed[ndx], relids))
				continue;		/* 不需要，跳过 */

			/* 用基关系的 attr_widths 更新 reltarget 的宽度估计 */
			joinrel->reltarget->width += baserel->attr_widths[ndx];
		}

		/*
		 * 把该 Var 加进输出。若这次连接可能把这个
		 * 输入置空，就必须更新该 Var 的 varnullingrels，也就是
		 * 要做一份副本。但注意，我们从不给
		 * 行标识 Var 加置空关系位（参见 setrefs.c 里的注释）。
		 */
		if (can_null && var->varno != ROWID_VAR)
		{
			var = copyObject(var);
			/* 见上面的注释以理解这段逻辑 */
			if (sjinfo->ojrelid != 0 &&
				bms_is_member(sjinfo->ojrelid, relids) &&
				(bms_is_member(var->varno, sjinfo->syn_righthand) ||
				 (sjinfo->jointype == JOIN_FULL &&
				  bms_is_member(var->varno, sjinfo->syn_lefthand))))
				var->varnullingrels = bms_add_member(var->varnullingrels,
													 sjinfo->ojrelid);
			foreach(lc, pushed_down_joins)
			{
				SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

				Assert(bms_is_member(othersj->ojrelid, relids));
				if (bms_is_member(var->varno, othersj->syn_righthand))
					var->varnullingrels = bms_add_member(var->varnullingrels,
														 othersj->ojrelid);
			}
			var->varnullingrels =
				bms_join(var->varnullingrels,
						 bms_intersect(sjinfo->commute_above_r,
									   relids));
		}

		joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
											var);

		/* Var 的代价为零，因此不必调整 reltarget->cost */
	}
}

/*
 * build_joinrel_restrictlist
 * build_joinrel_joinlist
 *	  这两个例程根据所连接关系的 joininfo 列表，
 *	  为连接关系构造限制子句和连接子句列表。
 *
 *	  这两个例程分开，是因为每一对考虑的输入子关系
 *	  都必须重新构造限制列表，而连接列表对任何一个
 *	  连接 RelOptInfo 只需计算一次。
 *	  连接列表完全由组成该连接关系的关系集合决定，
 *	  因此任何一对候选子关系都应当得到相同结果
 *	  （次序除外）。但限制列表是子关系里
 *	  尚未处理的那些，所以取决于考虑的是
 *	  哪些子关系。
 *
 *	  若来自输入关系的连接子句引用的基关系加外连接
 *	  仍不在该连接关系里，则它对这个连接关系仍是连接子句；
 *	  我们把它放进该连接关系的 joininfo 列表。否则，
 *	  该子句现在是已连接关系的限制子句，我们把它
 *	  返回给 build_joinrel_restrictlist() 的调用方，存进
 *	  由这一对子关系做出的连接路径。（它不必
 *	  在连接树更上方再被考虑。）
 *
 *	  许多情形下会在两个输入关系的 joinlist 里
 *	  发现相同的 RestrictInfos，因此要小心消除重复。
 *	  指针相等就足以检测重复，因为各种
 *	  joinlist 项最终都指向由
 *	  distribute_restrictinfo_to_rels() 推进去的 RestrictInfos。
 *
 * joinrel 是一个连接关系节点。
 * outer_rel 和 inner_rel 是可以连接起来
 *		形成 joinrel 的一对关系。
 * sjinfo：连接的上下文信息。
 *
 * build_joinrel_restrictlist() 返回相关 restrictinfo 的列表，
 * 而 build_joinrel_joinlist() 把结果存在连接关系的
 * joininfo 列表里。每一条给定子句必须被其中之一接受！
 *
 * 注意：从前我们为每个输入 RestrictInfo 做深层（deep）副本，
 * 再传到连接关系。我相信这不再必要，因为
 * RestrictInfo 节点不再依赖于上下文。改为只把
 * 原始节点放进为连接关系做的列表。
 */
static List *
build_joinrel_restrictlist(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   SpecialJoinInfo *sjinfo)
{
	List	   *result;
	Relids		both_input_relids;

	both_input_relids = bms_union(outer_rel->relids, inner_rel->relids);

	/*
	 * 收集语法上属于这一层的全部子句，
	 * 并消除任何重复（这很重要，因为我们会看到许多
	 * 相同的子句从两个输入关系两边到来）。
	 */
	result = subbuild_joinrel_restrictlist(root, joinrel, outer_rel,
										   both_input_relids, NIL);
	result = subbuild_joinrel_restrictlist(root, joinrel, inner_rel,
										   both_input_relids, result);

	/*
	 * 再加上从 EquivalenceClasses 导出的任何子句。这些无法
	 * 与 joininfo 列表里的子句重复，所以不必费心
	 * 检查。
	 */
	result = list_concat(result,
						 generate_join_implied_equalities(root,
														  joinrel->relids,
														  outer_rel->relids,
														  inner_rel,
														  sjinfo));

	return result;
}

static void
build_joinrel_joinlist(RelOptInfo *joinrel,
					   RelOptInfo *outer_rel,
					   RelOptInfo *inner_rel)
{
	List	   *result;

	/*
	 * 收集语法上属于这一层之上的全部子句，
	 * 并消除任何重复（这很重要，因为我们会看到许多
	 * 相同的子句从两个输入关系两边到来）。
	 */
	result = subbuild_joinrel_joinlist(joinrel, outer_rel->joininfo, NIL);
	result = subbuild_joinrel_joinlist(joinrel, inner_rel->joininfo, result);

	joinrel->joininfo = result;
}

static List *
subbuild_joinrel_restrictlist(PlannerInfo *root,
							  RelOptInfo *joinrel,
							  RelOptInfo *input_rel,
							  Relids both_input_relids,
							  List *new_restrictlist)
{
	ListCell   *l;

	foreach(l, input_rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * 这条子句应当成为该连接关系的限制子句，
			 * 因为它不引用外部关系。不过，若它是克隆
			 * 子句，求值可能已经太晚，因此必须
			 * 检查。（若确实太晚，就忽略该子句，姑且相信
			 * 另一个克隆已经被选中或将会被选中。）克隆
			 * 子句应当总是外连接子句，所以我们与
			 * both_input_relids 比较。
			 */
			if (rinfo->has_clone || rinfo->is_clone)
			{
				Assert(!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids));
				if (!bms_is_subset(rinfo->required_relids, both_input_relids))
					continue;
				if (bms_overlap(rinfo->incompatible_relids, both_input_relids))
					continue;
			}
			else
			{
				/*
				 * 对非克隆子句，我们只 Assert 它没问题。它们可能
				 * 是连接子句或过滤子句；若是连接子句，
				 * 则不应当引用当前连接的输出。
				 * （检查 incompatible_relids 没什么意义，
				 * 因为它会是 NULL。）
				 */
				Assert(RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids) ||
					   bms_is_subset(rinfo->required_relids,
									 both_input_relids));
			}

			/*
			 * 好，把它加进列表，并小心消除
			 * 重复。（不同 joinlist 里的 RestrictInfo 节点
			 * 是多重链接而不是复制的，因此指针
			 * 相等就足以检测。）
			 */
			new_restrictlist = list_append_unique_ptr(new_restrictlist, rinfo);
		}
		else
		{
			/*
			 * 这条子句在这一层仍是连接子句，因此本例程
			 * 忽略它。
			 */
		}
	}

	return new_restrictlist;
}

static List *
subbuild_joinrel_joinlist(RelOptInfo *joinrel,
						  List *joininfo_list,
						  List *new_joininfo)
{
	ListCell   *l;

	/* 预期只对父关系之间的连接调用。 */
	Assert(joinrel->reloptkind == RELOPT_JOINREL);

	foreach(l, joininfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * 这条子句成为该连接关系的限制子句，因为
			 * 它不引用外部关系。因此本例程可以
			 * 忽略它。
			 */
		}
		else
		{
			/*
			 * 这条子句在这一层仍是连接子句，因此把它加进
			 * 新的 joininfo 列表，并小心消除重复。
			 * （不同 joinlist 里的 RestrictInfo 节点
			 * 是多重链接而不是复制的，因此指针相等
			 * 就足以检测。）
			 */
			new_joininfo = list_append_unique_ptr(new_joininfo, rinfo);
		}
	}

	return new_joininfo;
}


/*
 * fetch_upper_rel
 *		构造描述扫描或连接之后某段查询处理的 RelOptInfo，
 *		若已经有人建过，就返回已有的那个。
 *
 * 上层关系由 UpperRelationKind 和一个 Relids 集合标识。
 * 这里不规定 Relids 集合的含义，而且很可能会
 * 随关系种类而不同。
 *
 * 上层 RelOptInfo 里的大多数成员用不到，这里也不设置
 * （不过 makeNode 应当保证它们是零）。我们基本上只关心
 * add_path() 和 set_cheapest() 感兴趣的成员。
 */
RelOptInfo *
fetch_upper_rel(PlannerInfo *root, UpperRelationKind kind, Relids relids)
{
	RelOptInfo *upperrel;
	ListCell   *lc;

	/*
	 * 目前，我们的索引数据结构只是每种关系
	 * 一个 List。若某一种多到这种做法不再
	 * 好用，可以再改进。本函数以外的代码不应当
	 * 对如何找到某个特定上层关系做任何假定。
	 */

	/* 若已经为该查询建过这个上层关系，就返回它 */
	foreach(lc, root->upper_rels[kind])
	{
		upperrel = (RelOptInfo *) lfirst(lc);

		if (bms_equal(upperrel->relids, relids))
			return upperrel;
	}

	upperrel = makeNode(RelOptInfo);
	upperrel->reloptkind = RELOPT_UPPER_REL;
	upperrel->relids = bms_copy(relids);

	/* 仅当并非要取出全部元组时，较低的启动代价才有意义 */
	upperrel->consider_startup = (root->tuple_fraction > 0);
	upperrel->consider_param_startup = false;
	upperrel->consider_parallel = false;	/* 以后可能会改 */
	upperrel->reltarget = create_empty_pathtarget();
	upperrel->pathlist = NIL;
	upperrel->cheapest_startup_path = NULL;
	upperrel->cheapest_total_path = NULL;
	upperrel->cheapest_unique_path = NULL;
	upperrel->cheapest_parameterized_paths = NIL;

	root->upper_rels[kind] = lappend(root->upper_rels[kind], upperrel);

	return upperrel;
}


/*
 * find_childrel_parents
 *		计算一个 appendrel 子关系的父 relid 集合。
 *
 * 因为 appendrel 可以嵌套，一个子关系可能有多层
 * appendrel 祖先。本函数算出全部
 * 父关系 ID 的 Relids 集合。
 */
Relids
find_childrel_parents(PlannerInfo *root, RelOptInfo *rel)
{
	Relids		result = NULL;

	Assert(rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
	Assert(rel->relid > 0 && rel->relid < root->simple_rel_array_size);

	do
	{
		AppendRelInfo *appinfo = root->append_rel_array[rel->relid];
		Index		prelid = appinfo->parent_relid;

		result = bms_add_member(result, prelid);

		/* 向上走到父关系；若它也是子关系就继续循环 */
		rel = find_base_rel(root, prelid);
	} while (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);

	Assert(rel->reloptkind == RELOPT_BASEREL);

	return result;
}


/*
 * get_baserel_parampathinfo
 *		取得基关系的参数化路径的 ParamPathInfo，
 *		若还没有就构造一个。
 *
 * 这里集中估计参数化路径的行数。
 * 必须缓存它们，以确保对给定关系的同一种参数化，
 * 所有路径用同一个行数。这里也便于
 * 确定该参数化路径将负责计算哪些
 * 可移动的连接子句。
 */
ParamPathInfo *
get_baserel_parampathinfo(PlannerInfo *root, RelOptInfo *baserel,
						  Relids required_outer)
{
	ParamPathInfo *ppi;
	Relids		joinrelids;
	List	   *pclauses;
	List	   *eqclauses;
	Bitmapset  *pserials;
	double		rows;
	ListCell   *lc;

	/* 若关系有 LATERAL 引用，它的每条路径都应当把它们算进去 */
	Assert(bms_is_subset(baserel->lateral_relids, required_outer));

	/* 未参数化的路径没有 ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(baserel->relids, required_outer));

	/* 若这种参数化已经有 PPI，直接返回它 */
	if ((ppi = find_param_path_info(baserel, required_outer)))
		return ppi;

	/*
	 * 找出在给定这种参数化下可移到该基关系的
	 * 全部连接子句。
	 */
	joinrelids = bms_union(baserel->relids, required_outer);
	pclauses = NIL;
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										baserel->relids,
										joinrelids))
			pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * 也加入 EquivalenceClasses 生成的连接子句。（这些
	 * 必然满足 join_clause_is_movable_into；但在启用断言的
	 * 构建里，还是验证一下。）
	 */
	eqclauses = generate_join_implied_equalities(root,
												 joinrelids,
												 required_outer,
												 baserel,
												 NULL);
#ifdef USE_ASSERT_CHECKING
	foreach(lc, eqclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(join_clause_is_movable_into(rinfo,
										   baserel->relids,
										   joinrelids));
	}
#endif
	pclauses = list_concat(pclauses, eqclauses);

	/* 计算被强制子句的序号集合 */
	pserials = NULL;
	foreach(lc, pclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pserials = bms_add_member(pserials, rinfo->rinfo_serial);
	}

	/* 估计该参数化扫描返回的行数 */
	rows = get_parameterized_baserel_size(root, baserel, pclauses);

	/* 现在可以建立 ParamPathInfo 了 */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = pclauses;
	ppi->ppi_serials = pserials;
	baserel->ppilist = lappend(baserel->ppilist, ppi);

	return ppi;
}

/*
 * get_joinrel_parampathinfo
 *		取得连接关系的参数化路径的 ParamPathInfo，
 *		若还没有就构造一个。
 *
 * 这里集中估计参数化路径的行数。
 * 必须缓存它们，以确保对给定关系的同一种参数化，
 * 所有路径用同一个行数。这里也便于
 * 确定该参数化路径将负责计算哪些
 * 可移动的连接子句。
 *
 * outer_path 和 inner_path 是一对可用于
 * 构造该连接的输入路径，restrict_clauses 是使用这些输入时
 * 必须在连接节点上应用的常规连接
 * 子句列表（包括从 EquivalenceClasses 导出的子句）。
 *
 * 与基关系的情形不同，要在一次连接上强制执行的
 * 可移动连接子句集合，随所选的一对输入路径而变，
 * 因此即使已经有匹配的 ParamPathInfo，也必须算出它并传回。
 * 做法是把下移到这次连接的任何子句加进
 * *restrict_clauses，它是一个输入输出参数。
 * （添加时不会改动传入的 List 结构。）
 *
 * 注意：考虑嵌套循环连接时，调用方必须已经从
 * restrict_clauses 里去掉那些本身预定要
 * 推进右侧路径的可移动子句。这里不这么做，因为
 * 对其他连接类型没有必要。
 */
ParamPathInfo *
get_joinrel_parampathinfo(PlannerInfo *root, RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  SpecialJoinInfo *sjinfo,
						  Relids required_outer,
						  List **restrict_clauses)
{
	ParamPathInfo *ppi;
	Relids		join_and_req;
	Relids		outer_and_req;
	Relids		inner_and_req;
	List	   *pclauses;
	List	   *eclauses;
	List	   *dropped_ecs;
	double		rows;
	ListCell   *lc;

	/* 若关系有 LATERAL 引用，它的每条路径都应当把它们算进去 */
	Assert(bms_is_subset(joinrel->lateral_relids, required_outer));

	/* 未参数化的路径没有 ParamPathInfo，也没有额外的连接子句 */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(joinrel->relids, required_outer));

	/*
	 * 找出在给定这种参数化下可移到这个连接关系的
	 * 全部连接子句。它们可移进这次连接，
	 * 但不能移进任一输入路径。把未参数化的
	 * 输入路径当成不接受参数化子句（因为它不会，
	 * 见上面的提前返回），即使连接子句的移动规则
	 * 也许允许把同样的子句移进该关系的一条
	 * 参数化路径。
	 */
	join_and_req = bms_union(joinrel->relids, required_outer);
	if (outer_path->param_info)
		outer_and_req = bms_union(outer_path->parent->relids,
								  PATH_REQ_OUTER(outer_path));
	else
		outer_and_req = NULL;	/* 外侧路径不接受参数 */
	if (inner_path->param_info)
		inner_and_req = bms_union(inner_path->parent->relids,
								  PATH_REQ_OUTER(inner_path));
	else
		inner_and_req = NULL;	/* 内侧路径不接受参数 */

	pclauses = NIL;
	foreach(lc, joinrel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										joinrel->relids,
										join_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 outer_path->parent->relids,
										 outer_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 inner_path->parent->relids,
										 inner_and_req))
			pclauses = lappend(pclauses, rinfo);
	}

	/* 也考虑 EquivalenceClasses 生成的连接子句 */
	eclauses = generate_join_implied_equalities(root,
												join_and_req,
												required_outer,
												joinrel,
												NULL);
	/* 我们只要那些不能移到更低层的 */
	dropped_ecs = NIL;
	foreach(lc, eclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(join_clause_is_movable_into(rinfo,
										   joinrel->relids,
										   join_and_req));
		if (join_clause_is_movable_into(rinfo,
										outer_path->parent->relids,
										outer_and_req))
			continue;			/* 若可移进左侧就丢掉 */
		if (join_clause_is_movable_into(rinfo,
										inner_path->parent->relids,
										inner_and_req))
		{
			/* 若可移进右侧就丢掉，但记住这个 EC 供下面使用 */
			Assert(rinfo->left_ec == rinfo->right_ec);
			dropped_ecs = lappend(dropped_ecs, rinfo->left_ec);
			continue;
		}
		pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * 等价类（EquivalenceClasses）比我们希望的更难对付，因为
	 * 给定的 EC 会随上下文生成不同的子句。设有 EC {X.X, Y.Y, Z.Z}，
	 * 其中 X 和 Y 是当前连接的左侧和右侧，Z 在 required_outer 里，并且
	 * 再设 inner_path 同时由 X 和 Z 参数化。上面的代码
	 * 会从该 EC 产生 Z.Z = X.X 或 Z.Z = Y.Y，
	 * 后一种情形会因为它可移进右侧而被丢掉。
	 * 然而 EC 机制在 inner_path 内部作为强制子句，
	 * 可能产生了 Y.Y = X.X 或 Y.Y = Z.Z；
	 * 它不会两个都产生，而我们从这里没法轻易看出
	 * 它选了哪一个。若这次连接不加任何子句，最终对该
	 * EC 的强制会不足：Z.Z 或 X.X 会没法
	 * 被约束成等于该 EC 的其他成员。（等我们把
	 * Z 连到这条 X/Y 路径时，肯定会丢掉在那次连接上
	 * 生成的那条 EC 子句，所以这个遗漏
	 * 以后补不上。）
	 *
	 * 为处理这一点，对每个我们从中丢掉了这样一条子句的 EC，
	 * 尝试生成一条把 required_outer 关系连到该连接左侧的子句
	 * （按上面例子的说法就是 Z.Z = X.X）。若成功，并且该
	 * 子句没法移到左侧，就把它加进当前连接的
	 * 限制子句。（若某个 EC 无法生成这样的子句，则它
	 * 没有什么必须在这里强制的；而若该子句可以
	 * 移进左侧，则它本应已在那条路径内部被强制。）
	 *
	 * 注意，对子句被认为可移进左侧的那些 EC，不需要类似处理，
	 * 因为左侧没法引用右侧，所以不存在可比的歧义，
	 * 说不清它内部实际
	 * 在强制什么。
	 */
	if (dropped_ecs)
	{
		Relids		real_outer_and_req;

		real_outer_and_req = bms_union(outer_path->parent->relids,
									   required_outer);
		eclauses =
			generate_join_implied_equalities_for_ecs(root,
													 dropped_ecs,
													 real_outer_and_req,
													 required_outer,
													 outer_path->parent);
		foreach(lc, eclauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			Assert(join_clause_is_movable_into(rinfo,
											   outer_path->parent->relids,
											   real_outer_and_req));
			if (!join_clause_is_movable_into(rinfo,
											 outer_path->parent->relids,
											 outer_and_req))
				pclauses = lappend(pclauses, rinfo);
		}
	}

	/*
	 * 现在，把找出的下移子句接到调用方的
	 * restrict_clauses 列表上。按这个顺序使用 list_concat，
	 * 就不会破坏 restrict_clauses 原来的列表结构。
	 */
	*restrict_clauses = list_concat(pclauses, *restrict_clauses);

	/* 若这种参数化已经有 PPI，直接返回它 */
	if ((ppi = find_param_path_info(joinrel, required_outer)))
		return ppi;

	/* 估计该参数化连接返回的行数 */
	rows = get_parameterized_joinrel_size(root, joinrel,
										  outer_path,
										  inner_path,
										  sjinfo,
										  *restrict_clauses);

	/*
	 * 现在可以建立 ParamPathInfo 了。不过没必要保存
	 * 依赖于这一对输入的子句列表。
	 *
	 * 注意：在 GEQO 模式下，我们会在临时内存上下文里被调用，但
	 * 连接关系结构也在那里，所以没问题。
	 */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = NIL;
	ppi->ppi_serials = NULL;
	joinrel->ppilist = lappend(joinrel->ppilist, ppi);

	return ppi;
}

/*
 * get_appendrel_parampathinfo
 *		取得 append 关系的参数化路径的 ParamPathInfo。
 *
 * 对 append 关系，行数估计就是其子关系
 * 估计之和。不过仍需要一个 ParamPathInfo
 * 来标明该路径需要参数。因此这里只建立
 * 一个合适的结构，ppi_rows 为零（也没有 ppi_clauses，
 * 因为 Append 节点不负责检查限定条件）。
 */
ParamPathInfo *
get_appendrel_parampathinfo(RelOptInfo *appendrel, Relids required_outer)
{
	ParamPathInfo *ppi;

	/* 若关系有 LATERAL 引用，它的每条路径都应当把它们算进去 */
	Assert(bms_is_subset(appendrel->lateral_relids, required_outer));

	/* 未参数化的路径没有 ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(appendrel->relids, required_outer));

	/* 若这种参数化已经有 PPI，直接返回它 */
	if ((ppi = find_param_path_info(appendrel, required_outer)))
		return ppi;

	/* 否则建立 ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = 0;
	ppi->ppi_clauses = NIL;
	ppi->ppi_serials = NULL;
	appendrel->ppilist = lappend(appendrel->ppilist, ppi);

	return ppi;
}

/*
 * 若给定关系里已经有 required_outer 所给参数化的 ParamPathInfo，
 * 就返回它。否则返回 NULL。
 */
ParamPathInfo *
find_param_path_info(RelOptInfo *rel, Relids required_outer)
{
	ListCell   *lc;

	foreach(lc, rel->ppilist)
	{
		ParamPathInfo *ppi = (ParamPathInfo *) lfirst(lc);

		if (bms_equal(ppi->ppi_req_outer, required_outer))
			return ppi;
	}

	return NULL;
}

/*
 * get_param_path_clause_serials
 *		给定一条参数化 Path，返回在该 Path 内部强制执行的
 *		下推子句集合（以 rinfo_serial 序号标识）。
 */
Bitmapset *
get_param_path_clause_serials(Path *path)
{
	if (path->param_info == NULL)
		return NULL;			/* 未参数化 */
	if (IsA(path, NestPath) ||
		IsA(path, MergePath) ||
		IsA(path, HashPath))
	{
		/*
		 * 对连接路径，把任一输入路径内部强制的子句
		 * 与本路径里作为 joinrestrictinfo 强制的子句合并。注意
		 * joinrestrictinfo 可能含一些非下推子句，但就
		 * 当前目的而言，把它们也放进结果是可以的。（若要
		 * 更小心，可以检查 clause_relids 是否与该
		 * 路径的参数化重叠，但目前不值得花这些周期。）
		 */
		JoinPath   *jpath = (JoinPath *) path;
		Bitmapset  *pserials;
		ListCell   *lc;

		pserials = NULL;
		pserials = bms_add_members(pserials,
								   get_param_path_clause_serials(jpath->outerjoinpath));
		pserials = bms_add_members(pserials,
								   get_param_path_clause_serials(jpath->innerjoinpath));
		foreach(lc, jpath->joinrestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pserials = bms_add_member(pserials, rinfo->rinfo_serial);
		}
		return pserials;
	}
	else if (IsA(path, AppendPath))
	{
		/*
		 * 对 appendrel，取每条输入路径里强制子句
		 * 集合的交集。
		 */
		AppendPath *apath = (AppendPath *) path;
		Bitmapset  *pserials;
		ListCell   *lc;

		pserials = NULL;
		foreach(lc, apath->subpaths)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			Bitmapset  *subserials;

			subserials = get_param_path_clause_serials(subpath);
			if (lc == list_head(apath->subpaths))
				pserials = bms_copy(subserials);
			else
				pserials = bms_int_members(pserials, subserials);
		}
		return pserials;
	}
	else if (IsA(path, MergeAppendPath))
	{
		/* 与 AppendPath 的情形相同 */
		MergeAppendPath *apath = (MergeAppendPath *) path;
		Bitmapset  *pserials;
		ListCell   *lc;

		pserials = NULL;
		foreach(lc, apath->subpaths)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			Bitmapset  *subserials;

			subserials = get_param_path_clause_serials(subpath);
			if (lc == list_head(apath->subpaths))
				pserials = bms_copy(subserials);
			else
				pserials = bms_int_members(pserials, subserials);
		}
		return pserials;
	}
	else
	{
		/*
		 * 否则它是基关系路径，可以用
		 * 先前算好的序号集合。
		 */
		return path->param_info->ppi_serials;
	}
}

/*
 * build_joinrel_partition_info
 *		检查正在连接的两个关系能否使用按分区连接，
 *		若可以，就初始化结果分区连接关系的
 *		分区信息。
 */
static void
build_joinrel_partition_info(PlannerInfo *root,
							 RelOptInfo *joinrel, RelOptInfo *outer_rel,
							 RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo,
							 List *restrictlist)
{
	PartitionScheme part_scheme;

	/* 若按分区连接技术被禁用，就无事可做。 */
	if (!enable_partitionwise_join)
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	/*
	 * 只有当 (a) 输入关系已分区且
	 * consider_partitionwise_join 为真，(b) 分区方案匹配，并且
	 * (c) 我们能认出分区键之间的等值连接时，才能把这次连接
	 * 当作进一步按分区连接的输入。注意，若
	 * have_partkey_equi_join 对同一个连接关系会因我们先尝试
	 * 哪种连接顺序而返回不同答案，这段逻辑就会坏掉。
	 * 不过那不该发生，因为查询规划器推导隐含等式并重排
	 * 连接的方式决定了这一点。详情见
	 * optimizer/README。
	 */
	if (outer_rel->part_scheme == NULL || inner_rel->part_scheme == NULL ||
		!outer_rel->consider_partitionwise_join ||
		!inner_rel->consider_partitionwise_join ||
		outer_rel->part_scheme != inner_rel->part_scheme ||
		!have_partkey_equi_join(root, joinrel, outer_rel, inner_rel,
								sjinfo->jointype, restrictlist))
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	part_scheme = outer_rel->part_scheme;

	/*
	 * 本函数对每个连接关系只调用一次，因此
	 * 分区相关成员应当还没填过。
	 */
	Assert(!joinrel->part_scheme && !joinrel->partexprs &&
		   !joinrel->nullable_partexprs && !joinrel->part_rels &&
		   !joinrel->boundinfo);

	/*
	 * 若该连接关系是分区的，它使用与参与连接的
	 * 关系相同的分区方案。
	 *
	 * 注意：连接关系的分区边界、分区数和
	 * 子连接关系在 try_partitionwise_join() 里计算。
	 */
	joinrel->part_scheme = part_scheme;
	set_joinrel_partition_key_exprs(joinrel, outer_rel, inner_rel,
									sjinfo->jointype);

	/*
	 * 设置 consider_partitionwise_join 标志。
	 */
	Assert(outer_rel->consider_partitionwise_join);
	Assert(inner_rel->consider_partitionwise_join);
	joinrel->consider_partitionwise_join = true;
}

/*
 * have_partkey_equi_join
 *
 * 若对全部分区键，都存在涉及正在连接的
 * 关系之匹配分区键对的等值连接条件，
 * 则返回真。
 */
static bool
have_partkey_equi_join(PlannerInfo *root, RelOptInfo *joinrel,
					   RelOptInfo *rel1, RelOptInfo *rel2,
					   JoinType jointype, List *restrictlist)
{
	PartitionScheme part_scheme = rel1->part_scheme;
	ListCell   *lc;
	int			cnt_pks;
	bool		pk_has_clause[PARTITION_MAX_KEYS];
	bool		strict_op;

	/*
	 * 仅当被连接的关系具有相同的
	 * 分区方案时，才可以调用本函数。
	 */
	Assert(rel1->part_scheme == rel2->part_scheme);
	Assert(part_scheme);

	memset(pk_has_clause, 0, sizeof(pk_has_clause));
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *opexpr;
		Expr	   *expr1;
		Expr	   *expr2;
		int			ipk1;
		int			ipk2;

		/* 若正在处理外连接，只用它自己的连接子句。 */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		/* 跳过不能用于连接的子句。 */
		if (!rinfo->can_join)
			continue;

		/* 跳过不是等值条件的子句。 */
		if (!rinfo->mergeopfamilies && !OidIsValid(rinfo->hashjoinoperator))
			continue;

		/* 应当可以假定它是 OpExpr。 */
		opexpr = castNode(OpExpr, rinfo->clause);

		/* 把操作数匹配到关系上。 */
		if (bms_is_subset(rinfo->left_relids, rel1->relids) &&
			bms_is_subset(rinfo->right_relids, rel2->relids))
		{
			expr1 = linitial(opexpr->args);
			expr2 = lsecond(opexpr->args);
		}
		else if (bms_is_subset(rinfo->left_relids, rel2->relids) &&
				 bms_is_subset(rinfo->right_relids, rel1->relids))
		{
			expr1 = lsecond(opexpr->args);
			expr2 = linitial(opexpr->args);
		}
		else
			continue;

		/*
		 * 现在需要知道该连接操作符是否严格；见
		 * pathnodes.h 里的注释。
		 */
		strict_op = op_strict(opexpr->opno);

		/*
		 * 出现在关系分区键里的 Var 不会有任何
		 * varnullingrels，但若我们位于可能把相应关系置空的
		 * 外连接之上，expr1 和 expr2 里的就会有。只要连接操作符
		 * 是严格的，照样匹配也没关系。
		 */
		if (strict_op)
		{
			if (bms_overlap(rel1->relids, root->outer_join_rels))
				expr1 = (Expr *) remove_nulling_relids((Node *) expr1,
													   root->outer_join_rels,
													   NULL);
			if (bms_overlap(rel2->relids, root->outer_join_rels))
				expr2 = (Expr *) remove_nulling_relids((Node *) expr2,
													   root->outer_join_rels,
													   NULL);
		}

		/*
		 * 只有引用分区键的子句才对
		 * 按分区连接有用。
		 */
		ipk1 = match_expr_to_partition_keys(expr1, rel1, strict_op);
		if (ipk1 < 0)
			continue;
		ipk2 = match_expr_to_partition_keys(expr2, rel2, strict_op);
		if (ipk2 < 0)
			continue;

		/*
		 * 若子句引用的键序号位置不同，它就
		 * 不能用于按分区连接。
		 */
		if (ipk1 != ipk2)
			continue;

		/* 若分区键的排序规则与子句的不同，就拒绝。 */
		if (rel1->part_scheme->partcollation[ipk1] != opexpr->inputcollid)
			return false;

		/*
		 * 仅当子句使用的操作符族与分区键所指定的
		 * 相同时，该子句才允许按分区连接。
		 */
		if (rel1->part_scheme->strategy == PARTITION_STRATEGY_HASH)
		{
			if (!OidIsValid(rinfo->hashjoinoperator) ||
				!op_in_opfamily(rinfo->hashjoinoperator,
								part_scheme->partopfamily[ipk1]))
				continue;
		}
		else if (!list_member_oid(rinfo->mergeopfamilies,
								  part_scheme->partopfamily[ipk1]))
			continue;

		/* 把该分区键标成已有等值连接子句。 */
		pk_has_clause[ipk1] = true;
	}

	/* 检查是否每个分区键都有等值连接条件。 */
	for (cnt_pks = 0; cnt_pks < part_scheme->partnatts; cnt_pks++)
	{
		if (!pk_has_clause[cnt_pks])
			return false;
	}

	return true;
}

/*
 * match_expr_to_partition_keys
 *
 * 尝试把一个表达式匹配到 rel 的某个可空或非可空
 * 分区键。返回匹配键的序号位置，
 * 若该表达式无法匹配到任何键则返回 -1。
 *
 * 若表达式将用严格操作符与分区键比较，
 * strict_op 必须为真。这样就可以同时考虑
 * 可空以及非可空的分区键。
 */
static int
match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel, bool strict_op)
{
	int			cnt;

	/* 本函数只应对已分区的关系调用。 */
	Assert(rel->part_scheme);
	Assert(rel->partexprs);
	Assert(rel->nullable_partexprs);

	/* 去掉任何 relabel 装饰。 */
	while (IsA(expr, RelabelType))
		expr = (Expr *) (castNode(RelabelType, expr))->arg;

	for (cnt = 0; cnt < rel->part_scheme->partnatts; cnt++)
	{
		ListCell   *lc;

		/* 我们总是可以匹配到非可空的分区键。 */
		foreach(lc, rel->partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}

		if (!strict_op)
			continue;

		/*
		 * 若它是严格连接操作符，则一侧的 NULL 分区键
		 * 不会连到另一侧的任何分区键，尤其是
		 * 这样的行不能连到另一侧不同
		 * 分区里的行。因此也可以去搜可空的
		 * 分区键。
		 */
		foreach(lc, rel->nullable_partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}
	}

	return -1;
}

/*
 * set_joinrel_partition_key_exprs
 *		为分区的连接关系初始化分区键表达式。
 */
static void
set_joinrel_partition_key_exprs(RelOptInfo *joinrel,
								RelOptInfo *outer_rel, RelOptInfo *inner_rel,
								JoinType jointype)
{
	PartitionScheme part_scheme = joinrel->part_scheme;
	int			partnatts = part_scheme->partnatts;

	joinrel->partexprs = (List **) palloc0(sizeof(List *) * partnatts);
	joinrel->nullable_partexprs =
		(List **) palloc0(sizeof(List *) * partnatts);

	/*
	 * 连接关系的分区表达式与输入
	 * 关系的相同，但必须在连接关系的输出里
	 * 把它们正确分成可空或不可空。（另外，若是
	 * FULL JOIN，我们还会再加一些分区表达式。）
	 */
	for (int cnt = 0; cnt < partnatts; cnt++)
	{
		/* 把它们标成 const，以强制我们正确地复制 */
		const List *outer_expr = outer_rel->partexprs[cnt];
		const List *outer_null_expr = outer_rel->nullable_partexprs[cnt];
		const List *inner_expr = inner_rel->partexprs[cnt];
		const List *inner_null_expr = inner_rel->nullable_partexprs[cnt];
		List	   *partexpr = NIL;
		List	   *nullable_partexpr = NIL;
		ListCell   *lc;

		switch (jointype)
		{
				/*
				 * 由 INNER 连接得到的连接关系，可以看作
				 * 按内侧或外侧关系键之一来分区。例如
				 * A INNER JOIN B ON A.a = B.b
				 * 可以看作按 A.a 或 B.b 分区。因此我们
				 * 把两个键都加进连接关系的 partexpr 列表。不过，
				 * 任何已经可空的东西仍必须当作
				 * 可空来处理。
				 */
			case JOIN_INNER:
				partexpr = list_concat_copy(outer_expr, inner_expr);
				nullable_partexpr = list_concat_copy(outer_null_expr,
													 inner_null_expr);
				break;

				/*
				 * 由 SEMI 或 ANTI 连接得到的连接关系，可以
				 * 看作按外侧关系键来分区。内侧关系的键
				 * 不再有意思；它们在连接输出里不可见，
				 * 没有什么能够
				 * 连到它们。
				 */
			case JOIN_SEMI:
			case JOIN_ANTI:
				partexpr = list_copy(outer_expr);
				nullable_partexpr = list_copy(outer_null_expr);
				break;

				/*
				 * 由 LEFT OUTER JOIN 得到的连接关系，同样
				 * 可以看作按（非可空的）外侧关系键来分区。
				 * 内侧（可空）关系键，只要后续连接使用
				 * 严格连接操作符，仍可以作为进一步连接的
				 * 分区键。
				 */
			case JOIN_LEFT:
				partexpr = list_copy(outer_expr);
				nullable_partexpr = list_concat_copy(inner_expr,
													 outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);
				break;

				/*
				 * 对 FULL OUTER JOINs，两侧关系都可空，因此
				 * 结果连接关系可以看作按内侧或外侧关系键
				 * 之一来分区，但仅限于涉及严格连接操作符的
				 * 那些连接。
				 */
			case JOIN_FULL:
				nullable_partexpr = list_concat_copy(outer_expr,
													 inner_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);

				/*
				 * 也加入与每个可能的
				 * 全连接输出变量对应的 CoalesceExprs
				 * （也就是左侧合并到右侧），以便匹配
				 * 使用这些变量的等值连接表达式。真正用得着的
				 * 只是被 JOIN USING 合并的那些列，并且只针对
				 * 与语法分析会为这类变量建立的
				 * 数据结构相对应的那几对输入项。
				 * 但很难分辨哪些才是那些，所以就把
				 * 所有配对都做出来。nullable_partexprs 列表里
				 * 多出来的项不会造成大问题。（这些项
				 * 有可能匹配到用户写下的 COALESCEs，但按它们
				 * 分区仍然有效，因为它们将要么是分区列，
				 * 要么是 NULL；这与任何外连接做按分区
				 * 嵌套时的论据相同。）我们假定构造合并表达式
				 * 不需要类型强制，因为不同类型的列不会
				 * 被归成同一个 PartitionScheme。注意，我们
				 * 有意省掉通常会出现在这些
				 * CoalesceExprs 内部 Var 上的
				 * varnullingrels 装饰，因为 have_partkey_equi_join
				 * 会从它将拿来比较的表达式上剥掉
				 * varnullingrels，再与 partexprs
				 * 比较。
				 */
				foreach(lc, list_concat_copy(outer_expr, outer_null_expr))
				{
					Node	   *larg = (Node *) lfirst(lc);
					ListCell   *lc2;

					foreach(lc2, list_concat_copy(inner_expr, inner_null_expr))
					{
						Node	   *rarg = (Node *) lfirst(lc2);
						CoalesceExpr *c = makeNode(CoalesceExpr);

						c->coalescetype = exprType(larg);
						c->coalescecollid = exprCollation(larg);
						c->args = list_make2(larg, rarg);
						c->location = -1;
						nullable_partexpr = lappend(nullable_partexpr, c);
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized join type: %d", (int) jointype);
		}

		joinrel->partexprs[cnt] = partexpr;
		joinrel->nullable_partexprs[cnt] = nullable_partexpr;
	}
}

/*
 * build_child_join_reltarget
 *	  根据父连接关系建立子连接关系的 reltarget。
 */
static void
build_child_join_reltarget(PlannerInfo *root,
						   RelOptInfo *parentrel,
						   RelOptInfo *childrel,
						   int nappinfos,
						   AppendRelInfo **appinfos)
{
	/* 构造目标列表 */
	childrel->reltarget->exprs = (List *)
		adjust_appendrel_attrs(root,
							   (Node *) parentrel->reltarget->exprs,
							   nappinfos, appinfos);

	/* 设置代价和宽度成员 */
	childrel->reltarget->cost.startup = parentrel->reltarget->cost.startup;
	childrel->reltarget->cost.per_tuple = parentrel->reltarget->cost.per_tuple;
	childrel->reltarget->width = parentrel->reltarget->width;
}
