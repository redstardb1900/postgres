/*-------------------------------------------------------------------------
 *
 * pathnodes.h
 *	规划器内部数据结构的定义,尤其是 Paths。
 *
 * 不支持拷贝 RelOptInfo、IndexOptInfo 或 Path 节点。
 * 不过有些附属结构仍值得拷贝。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pathnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODES_H
#define PATHNODES_H

#include "access/sdir.h"
#include "lib/stringinfo.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "storage/block.h"


/*
 * Relids
 *	关系标识符集合(范围表下标)。
 */
typedef Bitmapset *Relids;

/*
 * 寻找"最廉价路径"时,本枚举指定我们要的是
 * 最低启动代价或最低总代价。
 */
typedef enum CostSelector
{
	STARTUP_COST, TOTAL_COST
} CostSelector;

/*
 * cost_qual_eval() 给出的代价估计同时含一次性
 * (启动)代价与每元组代价。
 */
typedef struct QualCost
{
	Cost		startup;		/* 一次代价 */
	Cost		per_tuple;		/* 每次求值代价 */
} QualCost;

/*
 * 估计聚合函数执行代价需要关于将由给定
 * Agg 节点执行的聚合的这些统计。注意代价
 * 包含聚合实参表达式的执行代价,以及
 * 聚合函数本身。此外,字段必须如此定义,
 * 使得用 memset 把结构初始化为零是正确的。
 */
typedef struct AggClauseCosts
{
	QualCost	transCost;		/* 每输入行总执行代价 */
	QualCost	finalCost;		/* 每聚合行总代价 */
	Size		transitionSpace;	/* 传引用转移数据所需空间 */
} AggClauseCosts;

/*
 * 本枚举标识规划期间可能处理的各类"上层"
 * (扫描/连接之后)关系。
 */
typedef enum UpperRelationKind
{
	UPPERREL_SETOP,				/* UNION/INTERSECT/EXCEPT 的结果(若有) */
	UPPERREL_PARTIAL_GROUP_AGG, /* 部分分组/聚合的结果,若
								 * 有 */
	UPPERREL_GROUP_AGG,			/* 分组/聚合的结果(若有) */
	UPPERREL_WINDOW,			/* 窗口函数的结果(若有) */
	UPPERREL_PARTIAL_DISTINCT,	/* 部分 SELECT DISTINCT 的结果(若有) */
	UPPERREL_DISTINCT,			/* SELECT DISTINCT 的结果(若有) */
	UPPERREL_ORDERED,			/* ORDER BY 的结果(若有) */
	UPPERREL_FINAL				/* 其余顶层动作的结果 */
	/* 注: UPPERREL_FINAL 须为末项;用于确定数组大小 */
} UpperRelationKind;

/*----------
 * PlannerGlobal
 *	规划/优化的全局信息
 *
 * PlannerGlobal 保存整次规划器调用的状态;此状态
 * 在正被规划的命令中所有层级的子 Query 之间
 * 共享.
 *
 * 并非所有字段都会打印。(有时该字段类型无打印支持;
 * 有时打印会导致无限递归。)
 *----------
 */
typedef struct PlannerGlobal
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 提供给 planner() 的 Param 值 */
	ParamListInfo boundParams pg_node_attr(read_write_ignore);

	/* SubPlan 节点的计划 */
	List	   *subplans;

	/* SubPlan 节点的 PlannerInfo */
	List	   *subroots pg_node_attr(read_write_ignore);

	/* 需要 REWIND 的子计划下标 */
	Bitmapset  *rewindPlanIDs;

	/* 执行器用的扁平范围表 */
	List	   *finalrtable;

	/* RTEPermissionInfo 扁平列表 */
	List	   *finalrteperminfos;

	/* PlanRowMark 扁平列表 */
	List	   *finalrowmarks;

	/* 整数 RT 下标的扁平列表 */
	List	   *resultRelations;

	/* AppendRelInfo 扁平列表 */
	List	   *appendRelations;

	/* 计划所依赖关系的 OID */
	List	   *relationOids;

	/* 其他依赖,以 PlanInvalItem 形式 */
	List	   *invalItems;

	/* PARAM_EXEC Param 的类型 OID */
	List	   *paramExecTypes;

	/* 已赋最高 PlaceHolderVar ID */
	Index		lastPHId;

	/* 已赋最高 PlanRowMark ID */
	Index		lastRowMarkId;

	/* 已赋最高计划节点 ID */
	int			lastPlanNodeId;

	/* TransactionXmin 变化时重做计划? */
	bool		transientPlan;

	/* 计划是否专属当前角色? */
	bool		dependsOnRole;

	/* 并行模式潜在可用? */
	bool		parallelModeOK;

	/* 实际需要并行模式? */
	bool		parallelModeNeeded;

	/* 最差 PROPARALLEL 危害级 */
	char		maxParallelHazard;

	/* 分区描述符 */
	PartitionDirectory partition_directory pg_node_attr(read_write_ignore);
} PlannerGlobal;

/* 取与 SubPlan 节点关联的 Plan 的宏 */
#define planner_subplan_get_plan(root, subplan) \
	((Plan *) list_nth((root)->glob->subplans, (subplan)->plan_id - 1))


/*----------
 * PlannerInfo
 *	规划/优化的每查询信息
 *
 * 本结构在全部规划器例程中习惯称为 "root"。
 * 它保存规划器全部工作状态的链接,以及
 * 原始 Query。注意当前规划器会大量修改
 * 传入的 Query 数据结构;将来应停止这样做。
 *
 * 出于 optimizer/optimizer.h 中所述原因,typedef
 * 在此处或该头中定义,以先读到的为准。
 *
 * 并非所有字段都会打印。(有时该字段类型无打印支持;
 * 有时打印会导致无限递归或
 * 倾印输出臃肿得不值得。)
 *----------
 */
#ifndef HAVE_PLANNERINFO_TYPEDEF
typedef struct PlannerInfo PlannerInfo;
#define HAVE_PLANNERINFO_TYPEDEF 1
#endif

struct PlannerInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 正在规划的 Query */
	Query	   *parse;

	/* 当前规划器运行全局信息 */
	PlannerGlobal *glob;

	/* 最外层 Query 为 1 */
	Index		query_level;

	/* 最外层为 NULL */
	PlannerInfo *parent_root pg_node_attr(read_write_ignore);

	/*
	 * plan_params 含本查询层需提供给
	 * 当前正在规划的下层查询的表达式。
	 * outer_params 含外层查询将提供给本层的
	 * PARAM_EXEC Param 的 paramId。
	 */
	/* PlannerParamItem 列表,见下 */
	List	   *plan_params;
	Bitmapset  *outer_params;

	/*
	 * simple_rel_array 保存"基关系"与"其他关系"指针(详见
	 * RelOptInfo 注释)。按下标索引到范围表
	 * (故项 0 总是浪费的)。当 RTE 不对应基关系时可
	 * 为 NULL,例如连接 RTE 或未引用的视图 RTE;
	 * 或尚未创建 RelOptInfo 时。
	 */
	struct RelOptInfo **simple_rel_array pg_node_attr(array_size(simple_rel_array_size));
	/* 数组已分配大小 */
	int			simple_rel_array_size;

	/*
	 * simple_rte_array 与 simple_rel_array 等长,保存
	 * 指向关联范围表项的指针。用它比 rt_fetch()
	 * 稍快,主要因间接层次更少。(不打印,因与
	 * parse->rtable 重复。)
	 */
	RangeTblEntry **simple_rte_array pg_node_attr(read_write_ignore);

	/*
	 * append_rel_array 与上述数组等长,保存按
	 * child_relid 索引的对应 AppendRelInfo 指针,
	 * 若该关系不是 appendrel 子项则为 NULL。若
	 * append_rel_list 为空则不分配本数组。(不打印,
	 * 因与 append_rel_list 重复。)
	 */
	struct AppendRelInfo **append_rel_array pg_node_attr(read_write_ignore);

	/*
	 * all_baserels 是查询中全部基 relid 的 Relids 集合(不含
	 * 连接或"其他"关系)。在 deconstruct_jointree 中计算。
	 */
	Relids		all_baserels;

	/*
	 * outer_join_rels 是查询中全部外连接 relid 的 Relids 集合。
	 * 在 deconstruct_jointree 中计算。
	 */
	Relids		outer_join_rels;

	/*
	 * all_query_rels 是全部基 relid 与外连接 relid 的 Relids
	 * 集合(不含"其他" relid)。这是最终须形成的连接的
	 * Relids 标识。在
	 * deconstruct_jointree.
	 */
	Relids		all_query_rels;

	/*
	 * join_rel_list 是本规划运行中已考虑的全部
	 * 连接关系 RelOptInfo 的列表。小问题只扫列表
	 * 查找;连接关系很多时则建哈希表加速查找。
	 * 当 join_rel_hash 非 NULL 时哈希表存在且有效。
	 * 注意即使使用哈希表查找仍维护该列表;这简化了
	 * 面向
	 * GEQO.
	 */
	List	   *join_rel_list;
	struct HTAB *join_rel_hash pg_node_attr(read_write_ignore);

	/*
	 * 做动态规划式连接搜索时,join_rel_level[k]
	 * 是级别 k 的全部连接关系 RelOptInfo 列表,而
	 * join_cur_level 是当前级别。新的连接关系 RelOptInfo
	 * 会自动加入 join_rel_level[join_cur_level] 列表。
	 * 未用时 join_rel_level 为 NULL。
	 *
	 * 注:上方已打印全部基关系与连接关系 RelOptInfo,
	 * 故不倾印 join_rel_level 或其他 RelOptInfo 列表。
	 */
	/* 连接关系 RelOptInfo 的列表 */
	List	  **join_rel_level pg_node_attr(read_write_ignore);
	/* 正在扩展的列表下标 */
	int			join_cur_level;

	/* 查询的 init SubPlan */
	List	   *init_plans;

	/*
	 * 每 CTE 项的子计划 ID 列表(未建子计划则为 -1,对该
	 * CTE)
	 */
	List	   *cte_plan_ids;

	/* MULTIEXPR 子查询输出的 Params 的列表之列表 */
	List	   *multiexpr_params;

	/* 查询中所用 JoinDomain 列表(高层在前) */
	List	   *join_domains;

	/* 活动 EquivalenceClass 列表 */
	List	   *eq_classes;

	/* EC 规范化后置真 */
	bool		ec_merging_done;

	/* 规范 PathKeys 列表 */
	List	   *canon_pathkeys;

	/*
	 * 可归并外连接子句的 OuterJoinClauseInfo 列表
	 * 左侧为非空 var
	 */
	List	   *left_join_clauses;

	/*
	 * 可归并外连接子句的 OuterJoinClauseInfo 列表
	 * 右侧为非空 var
	 */
	List	   *right_join_clauses;

	/*
	 * 可归并全连接子句的 OuterJoinClauseInfo 列表
	 */
	List	   *full_join_clauses;

	/* SpecialJoinInfo 列表 */
	List	   *join_info_list;

	/* 分配 RestrictInfo 序列号的计数器 */
	int			last_rinfo_serial;

	/*
	 * SELECT 时 all_result_relids 为空,否则至少含
	 * parse->resultRelation。跨继承或分区树的
	 * UPDATE/DELETE/MERGE 会加入结果关系的子 relid。
	 * 多层分区时,中间分区关系也会
	 * 列入。leaf_result_relids 类似,但只含真正结果表,
	 * 不含分区表。
	 */
	/* 全部结果 relid 集 */
	Relids		all_result_relids;
	/* 全部叶子 relid 集 */
	Relids		leaf_result_relids;

	/*
	 * AppendRelInfo 列表
	 *
	 * 注:对描述分区表之分区的 AppendRelInfo,
	 * 我们保证 PartitionDesc 中靠前的分区在
	 * append_rel_list 中也靠前出现。
	 */
	List	   *append_rel_list;

	/* RowIdentityVarInfo 列表 */
	List	   *row_identity_vars;

	/* PlanRowMark 列表 */
	List	   *rowMarks;

	/* PlaceHolderInfo 列表 */
	List	   *placeholder_list;

	/* 按 phid 索引的 PlaceHolderInfo 数组 */
	struct PlaceHolderInfo **placeholder_array pg_node_attr(read_write_ignore, array_size(placeholder_array_size));
	/* 数组已分配大小 */
	int			placeholder_array_size pg_node_attr(read_write_ignore);

	/* ForeignKeyOptInfo 列表 */
	List	   *fkey_list;

	/* query_planner() 所需 pathkeys */
	List	   *query_pathkeys;

	/* groupClause pathkeys(若有) */
	List	   *group_pathkeys;

	/*
	 * group_pathkeys 列表中属于 GROUP BY 子句的
	 * 元素个数。其余属于 ORDER BY / DISTINCT
	 * 聚合。
	 */
	int			num_groupby_pathkeys;

	/* 底层窗口的 pathkeys(若有) */
	List	   *window_pathkeys;
	/* distinctClause pathkeys(若有) */
	List	   *distinct_pathkeys;
	/* sortClause pathkeys(若有) */
	List	   *sort_pathkeys;

	/* 查询中所用的规范化分区方案。 */
	List	   *part_schemes pg_node_attr(read_write_ignore);

	/* 当前试图连接的 RelOptInfo */
	List	   *initial_rels pg_node_attr(read_write_ignore);

	/*
	 * 上层关系 RelOptInfo。用 fetch_upper_rel() 取特定
	 * 上层rel
	 */
	List	   *upper_rels[UPPERREL_FINAL + 1] pg_node_attr(read_write_ignore);

	/* grouping_planner 为上层处理所选的结果 tlist */
	struct PathTarget *upper_targets[UPPERREL_FINAL + 1] pg_node_attr(read_write_ignore);

	/*
	 * 完整处理后的 groupClause 保存在此。它与
	 * parse->groupClause 的差异是我们去掉可证明
	 * 冗余的项,因而此处点名的列才真正需要比较
	 * 以判定分组。注意可能*全部*项都被证明冗余,
	 * 意味着只有一个组,包含查询全部行。故若要
	 * 检查是否指定了 GROUP BY,应测 parse->groupClause
	 * 是否非空,而非测
	 * processed_groupClause 非空。
	 *
	 * 当前若指定了分组集,则不尝试优化
	 * groupClause,故 processed_groupClause 将与
	 * parse->groupClause 相同。
	 */
	List	   *processed_groupClause;

	/*
	 * 完整处理后的 distinctClause 保存在此。它与
	 * parse->distinctClause 的差异是我们去掉可证明
	 * 冗余的项,因而此处点名的列才真正需要比较
	 * 以判定唯一性。注意可能*全部*项都被证明冗余,
	 * 意味着应只有一行输出。故若要检查是否指定了
	 * DISTINCT,应测 parse->distinctClause 是否非空,
	 * 而非测非空的
	 * processed_distinctClause.
	 */
	List	   *processed_distinctClause;

	/*
	 * 完整处理后的目标列表保存在此。它与
	 * parse->targetList 的差异是(对 INSERT)已重排以匹配
	 * 目标表,并填好缺省值。此外还可能有额外
	 * resjunk 目标。preprocess_targetlist() 完成大部分
	 * 工作,但注意 appendrel 展开期间还可再加
	 * resjunk 目标。(故 upper_targets 须等到
	 * 其后。)
	 */
	List	   *processed_tlist;

	/*
	 * 对 UPDATE,本列表含目标表属性号,processed_tlist
	 * 前 N 项将赋给它们。(processed_tlist 中任何额外
	 * 项必须是 resjunk。)不要用 processed_tlist 中的
	 * resno 来识别 UPDATE 目标列。
	 */
	List	   *update_colnos;

	/*
	 * create_plan() 填写、供 setrefs.c 用的字段
	 */
	/* 供 GroupingFunc 修补(不能打印:此处不知数组长度) */
	AttrNumber *grouping_map pg_node_attr(read_write_ignore);
	/* MinMaxAggInfo 列表 */
	List	   *minmax_aggs;

	/* context holding PlannerInfo */
	MemoryContext planner_cxt pg_node_attr(read_write_ignore);

	/* 查询中全部非哑表的页数 */
	Cardinality total_table_pages;

	/* 交给 query_planner  tuple_fraction */
	Selectivity tuple_fraction;
	/* 交给 query_planner  limit_tuples */
	Cardinality limit_tuples;

	/*
	 * 限定的最小 security_level。注:无 securityQuals 时
	 * qual_security_level 为0.
	 */
	Index		qual_security_level;

	/* 有 RTE 属 RTE_JOIN 类则为真 */
	bool		hasJoinRTEs;
	/* 若有 RTE 标为 LATERAL 则为真 */
	bool		hasLateralRTEs;
	/* 若 havingQual 非空则为真 */
	bool		hasHavingQual;
	/* 有 RestrictInfo 的 pseudoconstant=真则为真 */
	bool		hasPseudoConstantQuals;
	/* 已做其中任一种则为真 */
	bool		hasAlternativeSubPlans;
	/* 一旦不再允许添加 PlaceHolderInfo 则为真 */
	bool		placeholdersFrozen;
	/* 若在规划递归 WITH 项则为真 */
	bool		hasRecursion;

	/*
	 * 关于聚合的信息。由 preprocess_aggrefs() 填写。
	 */
	/* AggInfo 结构 */
	List	   *agginfos;
	/* AggTransInfo 结构 */
	List	   *aggtransinfos;
	/* 带 DISTINCT/ORDER BY/WITHIN GROUP 的聚合数 */
	int			numOrderedAggs;
	/* 是否有聚合不支持部分模式? */
	bool		hasNonPartialAggs;
	/* 有部分聚合不可序列化? */
	bool		hasNonSerialAggs;

	/*
	 * 下列字段仅在 hasRecursion 为真时使用:
	 */
	/* 工作表的 PARAM_EXEC ID */
	int			wt_param_id;
	/* 非递归项的路径 */
	struct Path *non_recursive_path;

	/*
	 * 下列字段是 createplan.c 的工作区
	 */
	/* 当前节点之上的外层 */
	Relids		curOuterRels;
	/* 尚未赋值的 NestLoopParam */
	List	   *curOuterParams;

	/*
	 * 下列字段是 setrefs.c 的工作区。各为与
	 * glob->subplans 对应的数组。(或许可让
	 * gen_node_support.pl 判定数组长度,但似乎不值
	 * 那麻烦,故仅标为 read_write_ignore。)
	 */
	bool	   *isAltSubplan pg_node_attr(read_write_ignore);
	bool	   *isUsedSubplan pg_node_attr(read_write_ignore);

	/* join_search_hook 的可选私有数据,如 GEQO */
	void	   *join_search_private pg_node_attr(read_write_ignore);

	/* 本查询是否修改任何分区键列? */
	bool		partColsUpdated;
};


/*
 * 在已知 simple_rte_array[] 必已准备好的地方,
 * 我们直接下标取 RTE。在可能于进入
 * query_planner() 之前或之后执行的代码中用本宏。
 */
#define planner_rt_fetch(rti, root) \
	((root)->simple_rte_array ? (root)->simple_rte_array[rti] : \
	 rt_fetch(rti, (root)->parse->rtable))

/*
 * 若多个关系按相同方式分区,所有这些分区
 * 将指向同一 PartitionScheme。PartitionScheme 对象
 * 列表挂在 PlannerInfo 上。按设计,分区方案只纳入
 * 分区方法的一般属性(LIST 对
 * RANGE、分区列数及各列的类型信息)
 * 而不含具体边界。
 *
 * 我们存储 opclass 声明的输入数据类型,而非分区键
 * 数据类型,因比较分区边界时用的是前者而非后者。
 * 因分区键数据类型与 opclass 所声明的
 * 输入数据类型预期二进制兼容(按 ResolveOpClass),
 * 故两者应有相同的 byval 与长度属性。
 */
typedef struct PartitionSchemeData
{
	char		strategy;		/* 分区策略 */
	int16		partnatts;		/* 分区属性个数 */
	Oid		   *partopfamily;	/* 运算符族 OID */
	Oid		   *partopcintype;	/* opclass 声明的输入数据类型 OID */
	Oid		   *partcollation;	/* 分区排序规则 OID */

	/* 关于分区键数据类型的缓存信息。 */
	int16	   *parttyplen;
	bool	   *parttypbyval;

	/* 关于分区比较函数的缓存信息。 */
	struct FmgrInfo *partsupfunc;
}			PartitionSchemeData;

typedef struct PartitionSchemeData *PartitionScheme;

/*----------
 * RelOptInfo
 *		Per-relation information for planning/optimization
 *
 * For planning purposes, a "base rel" is either a plain relation (a table)
 * or the output of a sub-SELECT or function that appears in the range table.
 * In either case it is uniquely identified by an RT index.  A "joinrel"
 * is the joining of two or more base rels.  A joinrel is identified by
 * the set of RT indexes for its component baserels, along with RT indexes
 * for any outer joins it has computed.  We create RelOptInfo nodes for each
 * baserel and joinrel, and store them in the PlannerInfo's simple_rel_array
 * and join_rel_list respectively.
 *
 * Note that there is only one joinrel for any given set of component
 * baserels, no matter what order we assemble them in; so an unordered
 * set is the right datatype to identify it with.
 *
 * We also have "other rels", which are like base rels in that they refer to
 * single RT indexes; but they are not part of the join tree, and are given
 * a different RelOptKind to identify them.
 * Currently the only kind of otherrels are those made for member relations
 * of an "append relation", that is an inheritance set or UNION ALL subquery.
 * An append relation has a parent RTE that is a base rel, which represents
 * the entire append relation.  The member RTEs are otherrels.  The parent
 * is present in the query join tree but the members are not.  The member
 * RTEs and otherrels are used to plan the scans of the individual tables or
 * subqueries of the append set; then the parent baserel is given Append
 * and/or MergeAppend paths comprising the best paths for the individual
 * member rels.  (See comments for AppendRelInfo for more information.)
 *
 * At one time we also made otherrels to represent join RTEs, for use in
 * handling join alias Vars.  Currently this is not needed because all join
 * alias Vars are expanded to non-aliased form during preprocess_expression.
 *
 * We also have relations representing joins between child relations of
 * different partitioned tables. These relations are not added to
 * join_rel_level lists as they are not joined directly by the dynamic
 * programming algorithm.
 *
 * There is also a RelOptKind for "upper" relations, which are RelOptInfos
 * that describe post-scan/join processing steps, such as aggregation.
 * Many of the fields in these RelOptInfos are meaningless, but their Path
 * fields always hold Paths showing ways to do that processing step.
 *
 * Parts of this data structure are specific to various scan and join
 * mechanisms.  It didn't seem worth creating new node types for them.
 *
 *		relids - Set of relation identifiers (RT indexes).  This is a base
 *				 relation if there is just one, a join relation if more;
 *				 in the join case, RT indexes of any outer joins formed
 *				 at or below this join are included along with baserels
 *		rows - estimated number of tuples in the relation after restriction
 *			   clauses have been applied (ie, output rows of a plan for it)
 *		consider_startup - true if there is any value in keeping plain paths for
 *						   this rel on the basis of having cheap startup cost
 *		consider_param_startup - the same for parameterized paths
 *		reltarget - Default Path output tlist for this rel; normally contains
 *					Var and PlaceHolderVar nodes for the values we need to
 *					output from this relation.
 *					List is in no particular order, but all rels of an
 *					appendrel set must use corresponding orders.
 *					NOTE: in an appendrel child relation, may contain
 *					arbitrary expressions pulled up from a subquery!
 *		pathlist - List of Path nodes, one for each potentially useful
 *				   method of generating the relation
 *		ppilist - ParamPathInfo nodes for parameterized Paths, if any
 *		cheapest_startup_path - the pathlist member with lowest startup cost
 *			(regardless of ordering) among the unparameterized paths;
 *			or NULL if there is no unparameterized path
 *		cheapest_total_path - the pathlist member with lowest total cost
 *			(regardless of ordering) among the unparameterized paths;
 *			or if there is no unparameterized path, the path with lowest
 *			total cost among the paths with minimum parameterization
 *		cheapest_unique_path - for caching cheapest path to produce unique
 *			(no duplicates) output from relation; NULL if not yet requested
 *		cheapest_parameterized_paths - best paths for their parameterizations;
 *			always includes cheapest_total_path, even if that's unparameterized
 *		direct_lateral_relids - rels this rel has direct LATERAL references to
 *		lateral_relids - required outer rels for LATERAL, as a Relids set
 *			(includes both direct and indirect lateral references)
 *
 * If the relation is a base relation it will have these fields set:
 *
 *		relid - RTE index (this is redundant with the relids field, but
 *				is provided for convenience of access)
 *		rtekind - copy of RTE's rtekind field
 *		min_attr, max_attr - range of valid AttrNumbers for rel
 *		attr_needed - array of bitmapsets indicating the highest joinrel
 *				in which each attribute is needed; if bit 0 is set then
 *				the attribute is needed as part of final targetlist
 *		attr_widths - cache space for per-attribute width estimates;
 *					  zero means not computed yet
 *		nulling_relids - relids of outer joins that can null this rel
 *		lateral_vars - lateral cross-references of rel, if any (list of
 *					   Vars and PlaceHolderVars)
 *		lateral_referencers - relids of rels that reference this one laterally
 *				(includes both direct and indirect lateral references)
 *		indexlist - list of IndexOptInfo nodes for relation's indexes
 *					(always NIL if it's not a table or partitioned table)
 *		pages - number of disk pages in relation (zero if not a table)
 *		tuples - number of tuples in relation (not considering restrictions)
 *		allvisfrac - fraction of disk pages that are marked all-visible
 *		eclass_indexes - EquivalenceClasses that mention this rel (filled
 *						 only after EC merging is complete)
 *		subroot - PlannerInfo for subquery (NULL if it's not a subquery)
 *		subplan_params - list of PlannerParamItems to be passed to subquery
 *
 *		Note: for a subquery, tuples and subroot are not set immediately
 *		upon creation of the RelOptInfo object; they are filled in when
 *		set_subquery_pathlist processes the object.
 *
 *		For otherrels that are appendrel members, these fields are filled
 *		in just as for a baserel, except we don't bother with lateral_vars.
 *
 * If the relation is either a foreign table or a join of foreign tables that
 * all belong to the same foreign server and are assigned to the same user to
 * check access permissions as (cf checkAsUser), these fields will be set:
 *
 *		serverid - OID of foreign server, if foreign table (else InvalidOid)
 *		userid - OID of user to check access as (InvalidOid means current user)
 *		useridiscurrent - we've assumed that userid equals current user
 *		fdwroutine - function hooks for FDW, if foreign table (else NULL)
 *		fdw_private - private state for FDW, if foreign table (else NULL)
 *
 * Two fields are used to cache knowledge acquired during the join search
 * about whether this rel is provably unique when being joined to given other
 * relation(s), ie, it can have at most one row matching any given row from
 * that join relation.  Currently we only attempt such proofs, and thus only
 * populate these fields, for base rels; but someday they might be used for
 * join rels too:
 *
 *		unique_for_rels - list of Relid sets, each one being a set of other
 *					rels for which this one has been proven unique
 *		non_unique_for_rels - list of Relid sets, each one being a set of
 *					other rels for which we have tried and failed to prove
 *					this one unique
 *
 * The presence of the following fields depends on the restrictions
 * and joins that the relation participates in:
 *
 *		baserestrictinfo - List of RestrictInfo nodes, containing info about
 *					each non-join qualification clause in which this relation
 *					participates (only used for base rels)
 *		baserestrictcost - Estimated cost of evaluating the baserestrictinfo
 *					clauses at a single tuple (only used for base rels)
 *		baserestrict_min_security - Smallest security_level found among
 *					clauses in baserestrictinfo
 *		joininfo  - List of RestrictInfo nodes, containing info about each
 *					join clause in which this relation participates (but
 *					note this excludes clauses that might be derivable from
 *					EquivalenceClasses)
 *		has_eclass_joins - flag that EquivalenceClass joins are possible
 *
 * Note: Keeping a restrictinfo list in the RelOptInfo is useful only for
 * base rels, because for a join rel the set of clauses that are treated as
 * restrict clauses varies depending on which sub-relations we choose to join.
 * (For example, in a 3-base-rel join, a clause relating rels 1 and 2 must be
 * treated as a restrictclause if we join {1} and {2 3} to make {1 2 3}; but
 * if we join {1 2} and {3} then that clause will be a restrictclause in {1 2}
 * and should not be processed again at the level of {1 2 3}.)	Therefore,
 * the restrictinfo list in the join case appears in individual JoinPaths
 * (field joinrestrictinfo), not in the parent relation.  But it's OK for
 * the RelOptInfo to store the joininfo list, because that is the same
 * for a given rel no matter how we form it.
 *
 * We store baserestrictcost in the RelOptInfo (for base relations) because
 * we know we will need it at least once (to price the sequential scan)
 * and may need it multiple times to price index scans.
 *
 * A join relation is considered to be partitioned if it is formed from a
 * join of two relations that are partitioned, have matching partitioning
 * schemes, and are joined on an equijoin of the partitioning columns.
 * Under those conditions we can consider the join relation to be partitioned
 * by either relation's partitioning keys, though some care is needed if
 * either relation can be forced to null by outer-joining.  For example, an
 * outer join like (A LEFT JOIN B ON A.a = B.b) may produce rows with B.b
 * NULL.  These rows may not fit the partitioning conditions imposed on B.
 * Hence, strictly speaking, the join is not partitioned by B.b and thus
 * partition keys of an outer join should include partition key expressions
 * from the non-nullable side only.  However, if a subsequent join uses
 * strict comparison operators (and all commonly-used equijoin operators are
 * strict), the presence of nulls doesn't cause a problem: such rows couldn't
 * match anything on the other side and thus they don't create a need to do
 * any cross-partition sub-joins.  Hence we can treat such values as still
 * partitioning the join output for the purpose of additional partitionwise
 * joining, so long as a strict join operator is used by the next join.
 *
 * If the relation is partitioned, these fields will be set:
 *
 *		part_scheme - Partitioning scheme of the relation
 *		nparts - Number of partitions
 *		boundinfo - Partition bounds
 *		partbounds_merged - true if partition bounds are merged ones
 *		partition_qual - Partition constraint if not the root
 *		part_rels - RelOptInfos for each partition
 *		all_partrels - Relids set of all partition relids
 *		partexprs, nullable_partexprs - Partition key expressions
 *
 * The partexprs and nullable_partexprs arrays each contain
 * part_scheme->partnatts elements.  Each of the elements is a list of
 * partition key expressions.  For partitioned base relations, there is one
 * expression in each partexprs element, and nullable_partexprs is empty.
 * For partitioned join relations, each base relation within the join
 * contributes one partition key expression per partitioning column;
 * that expression goes in the partexprs[i] list if the base relation
 * is not nullable by this join or any lower outer join, or in the
 * nullable_partexprs[i] list if the base relation is nullable.
 * Furthermore, FULL JOINs add extra nullable_partexprs expressions
 * corresponding to COALESCE expressions of the left and right join columns,
 * to simplify matching join clauses to those lists.
 *
 * Not all fields are printed.  (In some cases, there is no print support for
 * the field type.)
 *----------
 */

/* 表 AM 支持的标志位掩码 */
#define AMFLAG_HAS_TID_RANGE (1 << 0)

typedef enum RelOptKind
{
	RELOPT_BASEREL,
	RELOPT_JOINREL,
	RELOPT_OTHER_MEMBER_REL,
	RELOPT_OTHER_JOINREL,
	RELOPT_UPPER_REL,
	RELOPT_OTHER_UPPER_REL
} RelOptKind;

/*
 * 给定关系是否为简单关系,即基关系或"其他"成员
 * 关系?
 */
#define IS_SIMPLE_REL(rel) \
	((rel)->reloptkind == RELOPT_BASEREL || \
	 (rel)->reloptkind == RELOPT_OTHER_MEMBER_REL)

/* 给定关系是否为连接关系? */
#define IS_JOIN_REL(rel)	\
	((rel)->reloptkind == RELOPT_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL)

/* 给定关系是否为上层关系? */
#define IS_UPPER_REL(rel)	\
	((rel)->reloptkind == RELOPT_UPPER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

/* 给定关系是否为"其他"关系? */
#define IS_OTHER_REL(rel) \
	((rel)->reloptkind == RELOPT_OTHER_MEMBER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

typedef struct RelOptInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	RelOptKind	reloptkind;

	/*
	 * 本 RelOptInfo 包含的全部关系;基关系 + OJ relid 集合
	 * (范围表下标)
	 */
	Relids		relids;

	/*
	 * 规划器生成的大小估计
	 */
	/* 估计的结果元组数 */
	Cardinality rows;

	/*
	 * 每关系规划器控制标志
	 */
	/* 保留廉价启动路径? */
	bool		consider_startup;
	/* 同上,对参数化路径? */
	bool		consider_param_startup;
	/* 考虑并行路径? */
	bool		consider_parallel;

	/*
	 * 扫描本关系 Path 的默认结果目标列表;Var/Expr 列表、
	 * 代价、宽度
	 */
	struct PathTarget *reltarget;

	/*
	 * 物化信息
	 */
	List	   *pathlist;		/* Path 结构 */
	List	   *ppilist;		/* pathlist 中用 ParamPathInfo */
	List	   *partial_pathlist;	/* 部分 Path */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	struct Path *cheapest_unique_path;
	List	   *cheapest_parameterized_paths;

	/*
	 * 基关系与连接关系都需要的参数化信息
	 * (另见 lateral_vars 与 lateral_referencers)
	 */
	/* 直接侧向引用的关系 */
	Relids		direct_lateral_relids;
	/* 关系的最小参数化 */
	Relids		lateral_relids;

	/*
	 * 关于基关系的信息(不为连接关系设置!)
	 */
	Index		relid;
	/* 所含表空间 */
	Oid			reltablespace;
	/* RELATION, SUBQUERY, FUNCTION, etc */
	RTEKind		rtekind;
	/* 关系最小 attrno(常 <0) */
	AttrNumber	min_attr;
	/* 关系最大 attrno */
	AttrNumber	max_attr;
	/* 数组下标 [min_attr .. max_attr] */
	Relids	   *attr_needed pg_node_attr(read_write_ignore);
	/* 数组下标 [min_attr .. max_attr] */
	int32	   *attr_widths pg_node_attr(read_write_ignore);
	/* 可使本基关系变空的外连接 relid */
	Relids		nulling_relids;
	/* 关系引用的 LATERAL Var 与 PHV */
	List	   *lateral_vars;
	/* 侧向引用本基关系的关系 */
	Relids		lateral_referencers;
	/* IndexOptInfo 列表 */
	List	   *indexlist;
	/* StatisticExtInfo 列表 */
	List	   *statlist;
	/* 源自 pg_class 的大小估计 */
	BlockNumber pages;
	Cardinality tuples;
	double		allvisfrac;
	/* PlannerInfo 的 eq_classes 列表中提及本关系的 EC 下标 */
	Bitmapset  *eclass_indexes;
	PlannerInfo *subroot;		/* 子查询 */
	List	   *subplan_params; /* 子查询 */
	/* 想要的并行工作者数 */
	int			rel_parallel_workers;
	/* 表 AM 支持的可选特性位掩码 */
	uint32		amflags;

	/*
	 * 关于外表与外连接的信息
	 */
	/* 标识表或连接的服务器 */
	Oid			serverid;
	/* 标识要检查访问的用户;0 表示按当前用户检查 */
	Oid			userid;
	/* 连接仅对当前用户有效 */
	bool		useridiscurrent;
	/* 用 "struct FdwRoutine" 以免在此包含 fdwapi.h */
	struct FdwRoutine *fdwroutine pg_node_attr(read_write_ignore);
	void	   *fdw_private pg_node_attr(read_write_ignore);

	/*
	 * 缓存空间,记住是否已证明本关系唯一
	 */
	/* 对这些其他 relid 集合已知唯一 */
	List	   *unique_for_rels;
	/* 对这些集合已知不唯一 */
	List	   *non_unique_for_rels;

	/*
	 * 供各种扫描与连接使用:
	 */
	/* RestrictInfo 结构(若为基关系) */
	List	   *baserestrictinfo;
	/* 求值上述内容的代价 */
	QualCost	baserestrictcost;
	/* baserestrictinfo 中最低 security_level */
	Index		baserestrict_min_security;
	/* 涉及本关系的连接子句的 RestrictInfo 结构 */
	List	   *joininfo;
	/* T 表示 joininfo 不完整 */
	bool		has_eclass_joins;

	/*
	 * 供分区智能连接使用:
	 */
	/* 考虑分区智能连接路径?(若为已分区关系) */
	bool		consider_partitionwise_join;

	/*
	 * 继承链接,若这是其他关系(否则为 NULL):
	 */
	/* 直接父关系(转储会太冗长) */
	struct RelOptInfo *parent pg_node_attr(read_write_ignore);
	/* 最顶层父关系(转储会太冗长) */
	struct RelOptInfo *top_parent pg_node_attr(read_write_ignore);
	/* 最顶层父的 Relids(冗余但方便) */
	Relids		top_parent_relids;

	/*
	 * 供已分区关系使用:
	 */
	/* 分区方案 */
	PartitionScheme part_scheme pg_node_attr(read_write_ignore);

	/*
	 * 分区数;-1 为尚未设置;连接关系时 0 表示视为未分区
	 * 见上。
	 */
	int			nparts;
	/* 分区边界 */
	struct PartitionBoundInfoData *boundinfo pg_node_attr(read_write_ignore);
	/* 分区边界由 partition_bounds_merge() 创建则为真 */
	bool		partbounds_merged;
	/* 分区约束(非根时) */
	List	   *partition_qual;

	/*
	 * 分区 RelOptInfo 数组,与边界同序存放
	 * (不打印,太臃肿且重复)
	 */
	struct RelOptInfo **part_rels pg_node_attr(read_write_ignore);

	/*
	 * 成员作为 part_rels[] 数组下标的位图,指示哪些分区
	 * 在分区裁剪后存活。
	 */
	Bitmapset  *live_parts;
	/* 全部分区的 Relids 集合 */
	Relids		all_partrels;

	/*
	 * 这些数组长度为 partkey->partnatts,此处没有该值,
	 * 故不尝试打印
	 */

	/* 非空分区键表达式 */
	List	  **partexprs pg_node_attr(read_write_ignore);
	/* 可空分区键表达式 */
	List	  **nullable_partexprs pg_node_attr(read_write_ignore);
} RelOptInfo;

/*
 * 给定关系是否已分区?
 *
 * 仅测 rel->part_scheme 是否设置不够,因为可能输入关系
 * 基本分区属性匹配但分区边界不匹配。另外,若能证明
 * 关系为哑(空),此后应视其为未分区。
 * 见上。
 */
#define IS_PARTITIONED_REL(rel) \
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && !IS_DUMMY_REL(rel))

/*
 * 方便宏,确保已分区关系已设置全部所需成员。
 * 见上。
 */
#define REL_HAS_ALL_PART_PROPS(rel)	\
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && (rel)->partexprs && (rel)->nullable_partexprs)

/*
 * IndexOptInfo
 *		Per-index information for planning/optimization
 *
 *		indexkeys[], indexcollations[] each have ncolumns entries.
 *		opfamily[], and opcintype[]	each have nkeycolumns entries. They do
 *		not contain any information about included attributes.
 *
 *		sortopfamily[], reverse_sort[], and nulls_first[] have
 *		nkeycolumns entries, if the index is ordered; but if it is unordered,
 *		those pointers are NULL.
 *
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		For an ordered index, reverse_sort[] and nulls_first[] describe the
 *		sort ordering of a forward indexscan; we can also consider a backward
 *		indexscan, which will generate the reverse ordering.
 *
 *		The indexprs and indpred expressions have been run through
 *		prepqual.c and eval_const_expressions() for ease of matching to
 *		WHERE clauses. indpred is in implicit-AND form.
 *
 *		indextlist is a TargetEntry list representing the index columns.
 *		It provides an equivalent base-relation Var for each simple column,
 *		and links to the matching indexprs element for each expression column.
 *
 *		While most of these fields are filled when the IndexOptInfo is created
 *		(by plancat.c), indrestrictinfo and predOK are set later, in
 *		check_index_predicates().
 */
#ifndef HAVE_INDEXOPTINFO_TYPEDEF
typedef struct IndexOptInfo IndexOptInfo;
#define HAVE_INDEXOPTINFO_TYPEDEF 1
#endif

struct IndexPath;				/* 前向声明 */

struct IndexOptInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 索引关系的 OID */
	Oid			indexoid;
	/* 索引的表空间(非表) */
	Oid			reltablespace;
	/* 链回索引所属表;不打印,否则无限递归 */
	RelOptInfo *rel pg_node_attr(read_write_ignore);

	/*
	 * 索引大小统计(来自 pg_class 及其他)
	 */
	/* 索引磁盘页数 */
	BlockNumber pages;
	/* 索引中的索引元组数 */
	Cardinality tuples;
	/* 索引树高度,未知则为 -1 */
	int			tree_height;

	/*
	 * 索引描述符信息
	 */
	/* 索引列数 */
	int			ncolumns;
	/* 索引键列数 */
	int			nkeycolumns;

	/*
	 * 索引列对应表列号(键列与 INCLUDE 列),表达式列
	 * 则为 0
	 */
	int		   *indexkeys pg_node_attr(array_size(ncolumns));
	/* 索引列排序规则 OIDs */
	Oid		   *indexcollations pg_node_attr(array_size(nkeycolumns));
	/* 列的运算符族 OIDs */
	Oid		   *opfamily pg_node_attr(array_size(nkeycolumns));
	/* opclass 声明的输入数据类型 OID */
	Oid		   *opcintype pg_node_attr(array_size(nkeycolumns));
	/* 可排序时为 btree 运算符族 OID。分区索引则为 NULL */
	Oid		   *sortopfamily pg_node_attr(array_size(nkeycolumns));
	/* 排序是否降序?分区索引则为 NULL */
	bool	   *reverse_sort pg_node_attr(array_size(nkeycolumns));
	/* 空值是否排在前面?分区索引则为 NULL */
	bool	   *nulls_first pg_node_attr(array_size(nkeycolumns));
	/* 列的 opclass 专用选项 */
	bytea	  **opclassoptions pg_node_attr(read_write_ignore);
	/* 哪些索引列可在仅索引扫描中返回? */
	bool	   *canreturn pg_node_attr(array_size(ncolumns));
	/* 访问方法 OID(在 pg_am 中) */
	Oid			relam;

	/*
	 * 非简单索引列的表达式;打印冗余因我们打印
	 * indextlist
	 */
	List	   *indexprs pg_node_attr(read_write_ignore);
	/* 部分索引的谓词,否则为 NIL */
	List	   *indpred;

	/* 表示索引列的目标列表 */
	List	   *indextlist;

	/*
	 * 父关系的 baserestrictinfo 列表,去掉索引谓词蕴含的
	 * 条件(除非是目标关系,见 check_index_predicates()
	 * 注释)
	 */
	List	   *indrestrictinfo;

	/* 索引谓词匹配查询则为真 */
	bool		predOK;
	/* 唯一索引则为真 */
	bool		unique;
	/* 唯一性是否立即强制? */
	bool		immediate;
	/* 索引实际不存在则为真 */
	bool		hypothetical;

	/*
	 * 其余字段从索引 AM 的 API 结构
	 * (IndexAmRoutine) 拷贝。分区索引不设置这些字段。
	 */
	bool		amcanorderbyop;
	bool		amoptionalkey;
	bool		amsearcharray;
	bool		amsearchnulls;
	/* AM 是否有 amgettuple 接口? */
	bool		amhasgettuple;
	/* AM 是否有 amgetbitmap 接口? */
	bool		amhasgetbitmap;
	bool		amcanparallel;
	/* AM 是否有 ammarkpos 接口? */
	bool		amcanmarkpos;
	/* AM 代价估计器 */
	/* 与其在此包含 amapi.h,我们这样声明 amcostestimate */
	void		(*amcostestimate) (struct PlannerInfo *, struct IndexPath *, double, Cost *, Cost *, Selectivity *, double *, double *) pg_node_attr(read_write_ignore);
};

/*
 * ForeignKeyOptInfo
 *	规划/优化的每外键信息
 *
 * 每 FK 列数组可用固定大小,因外键约束最多允许
 * INDEX_MAX_KEYS 列。每个数组有 nkeys 个有效项。
 * 见上。
 */
typedef struct ForeignKeyOptInfo
{
	pg_node_attr(custom_read_write, no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/*
	 * 关于外键的基本数据(取自目录):
	 */

	/* 引用表的 RT 下标 */
	Index		con_relid;
	/* 被引用表的 RT 下标 */
	Index		ref_relid;
	/* 外键中的列数 */
	int			nkeys;
	/* 引用表中的列 */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* 被引用表中的列 */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* PK = FK 运算符 OID */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));

	/*
	 * 关于 FK 相等条件是否匹配查询的派生信息:
	 */

	/* 被 EC 匹配的 FK 列数 */
	int			nmatched_ec;
	/* 其中 ec_has_const 的 EC 数 */
	int			nconst_ec;
	/* 被非 EC rinfo 匹配的 FK 列数 */
	int			nmatched_rcols;
	/* 匹配到 FK 的非 EC rinfo 总数 */
	int			nmatched_ri;
	/* 匹配每列条件的 eclass 指针(若有) */
	struct EquivalenceClass *eclass[INDEX_MAX_KEYS];
	/* 引用 Var 的 eclass 成员指针(若有) */
	struct EquivalenceMember *fk_eclass_member[INDEX_MAX_KEYS];
	/* 匹配每列条件的非 EC RestrictInfo 列表 */
	List	   *rinfos[INDEX_MAX_KEYS];
} ForeignKeyOptInfo;

/*
 * StatisticExtInfo
 *	规划/优化用的扩展统计信息
 *
 * 每行 pg_statistic_ext 由本类型的一个或多个节点表示,
 * ANALYZE 尚未计算时甚至可为零个。
 */
typedef struct StatisticExtInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 统计行的 OID */
	Oid			statOid;

	/* 包含子关系 */
	bool		inherit;

	/* 链回统计所属表;不打印,否则无限递归 */
	RelOptInfo *rel pg_node_attr(read_write_ignore);

	/* 本项的统计种类 */
	char		kind;

	/* 所覆盖列的 attnum */
	Bitmapset  *keys;

	/* 表达式 */
	List	   *exprs;
} StatisticExtInfo;

/*
 * JoinDomains
 *
 * A "join domain" defines the scope of applicability of deductions made via
 * the EquivalenceClass mechanism.  Roughly speaking, a join domain is a set
 * of base+OJ relations that are inner-joined together.  More precisely, it is
 * the set of relations at which equalities deduced from an EquivalenceClass
 * can be enforced or should be expected to hold.  The topmost JoinDomain
 * covers the whole query (so its jd_relids should equal all_query_rels).
 * An outer join creates a new JoinDomain that includes all base+OJ relids
 * within its nullable side, but (by convention) not the OJ's own relid.
 * A FULL join creates two new JoinDomains, one for each side.
 *
 * Notice that a rel that is below outer join(s) will thus appear to belong
 * to multiple join domains.  However, any of its Vars that appear in
 * EquivalenceClasses belonging to higher join domains will have nullingrel
 * bits preventing them from being evaluated at the rel's scan level, so that
 * we will not be able to derive enforceable-at-the-rel-scan-level clauses
 * from such ECs.  We define the join domain relid sets this way so that
 * domains can be said to be "higher" or "lower" when one domain relid set
 * includes another.
 *
 * The JoinDomains for a query are computed in deconstruct_jointree.
 * We do not copy JoinDomain structs once made, so they can be compared
 * for equality by simple pointer equality.
 */
typedef struct JoinDomain
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Relids		jd_relids;		/* 域内包含的全部 relid */
} JoinDomain;

/*
 * EquivalenceClasses
 *
 * Whenever we identify a mergejoinable equality clause A = B that is
 * not an outer-join clause, we create an EquivalenceClass containing
 * the expressions A and B to record this knowledge.  If we later find another
 * equivalence B = C, we add C to the existing EquivalenceClass; this may
 * require merging two existing EquivalenceClasses.  At the end of the qual
 * distribution process, we have sets of values that are known all transitively
 * equal to each other, where "equal" is according to the rules of the btree
 * operator family(s) shown in ec_opfamilies, as well as the collation shown
 * by ec_collation.  (We restrict an EC to contain only equalities whose
 * operators belong to the same set of opfamilies.  This could probably be
 * relaxed, but for now it's not worth the trouble, since nearly all equality
 * operators belong to only one btree opclass anyway.  Similarly, we suppose
 * that all or none of the input datatypes are collatable, so that a single
 * collation value is sufficient.)
 *
 * Strictly speaking, deductions from an EquivalenceClass hold only within
 * a "join domain", that is a set of relations that are innerjoined together
 * (see JoinDomain above).  For the most part we don't need to account for
 * this explicitly, because equality clauses from different join domains
 * will contain Vars that are not equal() because they have different
 * nullingrel sets, and thus we will never falsely merge ECs from different
 * join domains.  But Var-free (pseudoconstant) expressions lack that safety
 * feature.  We handle that by marking "const" EC members with the JoinDomain
 * of the clause they came from; two nominally-equal const members will be
 * considered different if they came from different JoinDomains.  This ensures
 * no false EquivalenceClass merges will occur.
 *
 * We also use EquivalenceClasses as the base structure for PathKeys, letting
 * us represent knowledge about different sort orderings being equivalent.
 * Since every PathKey must reference an EquivalenceClass, we will end up
 * with single-member EquivalenceClasses whenever a sort key expression has
 * not been equivalenced to anything else.  It is also possible that such an
 * EquivalenceClass will contain a volatile expression ("ORDER BY random()"),
 * which is a case that can't arise otherwise since clauses containing
 * volatile functions are never considered mergejoinable.  We mark such
 * EquivalenceClasses specially to prevent them from being merged with
 * ordinary EquivalenceClasses.  Also, for volatile expressions we have
 * to be careful to match the EquivalenceClass to the correct targetlist
 * entry: consider SELECT random() AS a, random() AS b ... ORDER BY b,a.
 * So we record the SortGroupRef of the originating sort clause.
 *
 * NB: if ec_merged isn't NULL, this class has been merged into another, and
 * should be ignored in favor of using the pointed-to class.
 *
 * NB: EquivalenceClasses are never copied after creation.  Therefore,
 * copyObject() copies pointers to them as pointers, and equal() compares
 * pointers to EquivalenceClasses via pointer equality.  This is implemented
 * by putting copy_as_scalar and equal_as_scalar attributes on fields that
 * are pointers to EquivalenceClasses.  The same goes for EquivalenceMembers.
 */
typedef struct EquivalenceClass
{
	pg_node_attr(custom_read_write, no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	List	   *ec_opfamilies;	/* btree 运算符族 OID */
	Oid			ec_collation;	/* 排序规则(若数据类型可排序) */
	List	   *ec_members;		/* EquivalenceMember 列表 */
	List	   *ec_sources;		/* 生成用 RestrictInfo 列表 */
	List	   *ec_derives;		/* 派生 RestrictInfo 列表 */
	Relids		ec_relids;		/* ec_members 中出现的全部 relid,不含
								 * 子成员(见下) */
	bool		ec_has_const;	/* ec_members 中有伪常量? */
	bool		ec_has_volatile;	/* (唯一)成员是易变表达式 */
	bool		ec_broken;		/* 未能生成所需子句? */
	Index		ec_sortref;		/* 源排序子句标签,或 0 */
	Index		ec_min_security;	/* ec_sources 中最小 security_level */
	Index		ec_max_security;	/* ec_sources 中最大 security_level */
	struct EquivalenceClass *ec_merged; /* 已并入另一 EC 则设置 */
} EquivalenceClass;

/*
 * 若 EC 含常量，则依赖它的 PathKey 必为
 * 冗余，因该键只有一种可能取值。
 */
#define EC_MUST_BE_REDUNDANT(eclass)  \
	((eclass)->ec_has_const)

/*
 * EquivalenceMember - EquivalenceClass 的一个成员表达式
 *
 * em_is_child 表示本元素由转置追加关系父关系的成员而建,
 * 以表示追加关系子项对应表达式.这些成员用于确定子关
 * 系扫描 pathkey,并在需要时显式排序子项以构建整个
 * 追加关系树的 MergeAppend 路径。em_is_child 成员不影响
 * EC 整体属性;尤其 EC 的 ec_relids 字段不含子关系。
 * em_is_child 成员也绝不应标 em_is_const,或导致设置
 * ec_has_const 或 ec_has_volatile。因此 em_is_child 成员
 * 并非真正完整的 EC 成员,而只是真实成员的映像。对
 * EquivalenceClass 的多数操作应忽略 em_is_child 成员,
 * 不忽略的应测 em_relids 以确保只考虑相关成员。
 * 见上。
 * 见上。
 * 见上。
 *
 * em_datatype 通常与exprType(em_expr) 相同,但处理二进制
 * 兼容 opfamily 时可能不同;尤其 anyarray_ops 离开它无法
 * 工作。查找与本表达式配合具体 btree 运算符时用
 * em_datatype。
 */
typedef struct EquivalenceMember
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Expr	   *em_expr;		/* 所表示的表达式 */
	Relids		em_relids;		/* em_expr 中出现全部 relid */
	bool		em_is_const;	/* 表达式是伪常量? */
	bool		em_is_child;	/* 子关系的派生版本? */
	Oid			em_datatype;	/* opfamily 使用的"名义类型" */
	JoinDomain *em_jdomain;		/* 含源子句的连接域 */
	/* 若 em_is_child 为真,链到顶层父的对应 EM */
	struct EquivalenceMember *em_parent pg_node_attr(read_write_ignore);
} EquivalenceMember;

/*
 * PathKeys
 *
 * The sort ordering of a path is represented by a list of PathKey nodes.
 * An empty list implies no known ordering.  Otherwise the first item
 * represents the primary sort key, the second the first secondary sort key,
 * etc.  The value being sorted is represented by linking to an
 * EquivalenceClass containing that value and including pk_opfamily among its
 * ec_opfamilies.  The EquivalenceClass tells which collation to use, too.
 * This is a convenient method because it makes it trivial to detect
 * equivalent and closely-related orderings. (See optimizer/README for more
 * information.)
 *
 * Note: pk_strategy is either BTLessStrategyNumber (for ASC) or
 * BTGreaterStrategyNumber (for DESC).  We assume that all ordering-capable
 * index types will use btree-compatible strategy numbers.
 */
typedef struct PathKey
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* 被排序的值 */
	EquivalenceClass *pk_eclass pg_node_attr(copy_as_scalar, equal_as_scalar);
	Oid			pk_opfamily;	/* 定义排序的 btree 运算符族 */
	int			pk_strategy;	/* 排序方向(ASC/DESC) */
	bool		pk_nulls_first; /* 空值是否排在普通值之前? */
} PathKey;

/*
 * VolatileFunctionStatus——允许节点缓存其
 * contain_volatile_functions。VOLATILITY_UNKNOWN 表示尚未
 * 确定。
 */
typedef enum VolatileFunctionStatus
{
	VOLATILITY_UNKNOWN = 0,
	VOLATILITY_VOLATILE,
	VOLATILITY_NOVOLATILE
} VolatileFunctionStatus;

/*
 * PathTarget
 *
 * This struct contains what we need to know during planning about the
 * targetlist (output columns) that a Path will compute.  Each RelOptInfo
 * includes a default PathTarget, which its individual Paths may simply
 * reference.  However, in some cases a Path may compute outputs different
 * from other Paths, and in that case we make a custom PathTarget for it.
 * For example, an indexscan might return index expressions that would
 * otherwise need to be explicitly calculated.  (Note also that "upper"
 * relations generally don't have useful default PathTargets.)
 *
 * exprs contains bare expressions; they do not have TargetEntry nodes on top,
 * though those will appear in finished Plans.
 *
 * sortgrouprefs[] is an array of the same length as exprs, containing the
 * corresponding sort/group refnos, or zeroes for expressions not referenced
 * by sort/group clauses.  If sortgrouprefs is NULL (which it generally is in
 * RelOptInfo.reltarget targets; only upper-level Paths contain this info),
 * we have not identified sort/group columns in this tlist.  This allows us to
 * deal with sort/group refnos when needed with less expense than including
 * TargetEntry nodes in the exprs list.
 */
typedef struct PathTarget
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 要求值的表达式列表 */
	List	   *exprs;

	/* 对应的 sort/group refno,或 0 */
	Index	   *sortgrouprefs pg_node_attr(array_size(exprs));

	/* 求值这些表达式的代价 */
	QualCost	cost;

	/* 结果元组的估计平均宽度 */
	int			width;

	/* exprs 是否含易变函数 */
	VolatileFunctionStatus has_volatile_expr;
} PathTarget;

/* 从 PathTarget 取排序/分组 refno 的方便宏 */
#define get_pathtarget_sortgroupref(target, colno) \
	((target)->sortgrouprefs ? (target)->sortgrouprefs[colno] : (Index) 0)


/*
 * ParamPathInfo
 *
 * All parameterized paths for a given relation with given required outer rels
 * link to a single ParamPathInfo, which stores common information such as
 * the estimated rowcount for this parameterization.  We do this partly to
 * avoid recalculations, but mostly to ensure that the estimated rowcount
 * is in fact the same for every such path.
 *
 * Note: ppi_clauses is only used in ParamPathInfos for base relation paths;
 * in join cases it's NIL because the set of relevant clauses varies depending
 * on how the join is formed.  The relevant clauses will appear in each
 * parameterized join path's joinrestrictinfo list, instead.  ParamPathInfos
 * for append relations don't bother with this, either.
 *
 * ppi_serials is the set of rinfo_serial numbers for quals that are enforced
 * by this path.  As with ppi_clauses, it's only maintained for baserels.
 * (We could construct it on-the-fly from ppi_clauses, but it seems better
 * to materialize a copy.)
 */
typedef struct ParamPathInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Relids		ppi_req_outer;	/* 向路径提供参数的关系 */
	Cardinality ppi_rows;		/* 估计的结果元组数 */
	List	   *ppi_clauses;	/* 可从外层关系得到连接子句 */
	Bitmapset  *ppi_serials;	/* 已强制限定的 rinfo_serial 集合 */
} ParamPathInfo;


/*
 * Type "Path" is used as-is for sequential-scan paths, as well as some other
 * simple plan types that we don't need any extra information in the path for.
 * For other path types it is the first component of a larger struct.
 *
 * "pathtype" is the NodeTag of the Plan node we could build from this Path.
 * It is partially redundant with the Path's NodeTag, but allows us to use
 * the same Path type for multiple Plan types when there is no need to
 * distinguish the Plan type during path processing.
 *
 * "parent" identifies the relation this Path scans, and "pathtarget"
 * describes the precise set of output columns the Path would compute.
 * In simple cases all Paths for a given rel share the same targetlist,
 * which we represent by having path->pathtarget equal to parent->reltarget.
 *
 * "param_info", if not NULL, links to a ParamPathInfo that identifies outer
 * relation(s) that provide parameter values to each scan of this path.
 * That means this path can only be joined to those rels by means of nestloop
 * joins with this path on the inside.  Also note that a parameterized path
 * is responsible for testing all "movable" joinclauses involving this rel
 * and the specified outer rel(s).
 *
 * "rows" is the same as parent->rows in simple paths, but in parameterized
 * paths and UniquePaths it can be less than parent->rows, reflecting the
 * fact that we've filtered by extra join conditions or removed duplicates.
 *
 * "pathkeys" is a List of PathKey nodes (see above), describing the sort
 * ordering of the path's output rows.
 *
 * We do not support copying Path trees, mainly because the circular linkages
 * between RelOptInfo and Path nodes can't be handled easily in a simple
 * depth-first traversal.  We also don't have read support at the moment.
 */
typedef struct Path
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 标识扫描/连接方法标签 */
	NodeTag		pathtype;

	/*
	 * 本路径可构建的关系
	 *
	 * 不打印 parent，否则无限递归。但可
	 * 打印 parent 的 relids 以便识别。
	 */
	RelOptInfo *parent pg_node_attr(write_only_relids);

	/*
	 * Vars/Exprs、代价与宽度
	 *
	 * 仅当 pathtarget 非该 rel 默认目标时才打印。
	 */
	PathTarget *pathtarget pg_node_attr(write_only_nondefault_pathtarget);

	/*
	 * 参数化信息，无则 NULL
	 *
	 * 不打印完整 param_info，因其已通过
	 * RelOptInfo 打印；只打印所需外层 relids
	 * 即可，更简洁。
	 */
	ParamPathInfo *param_info pg_node_attr(write_only_req_outer);

	/* 启用并行感知逻辑? */
	bool		parallel_aware;
	/* 可否用作并行计划一部分? */
	bool		parallel_safe;
	/* 期望工作者数;0=非并行 */
	int			parallel_workers;

	/* 路径的估计大小/代价(详见 costsize.c) */
	Cardinality rows;			/* 估计的结果元组数 */
	Cost		startup_cost;	/* 取任何元组前已花的代价 */
	Cost		total_cost;		/* 总代价(假定取完全部元组) */

	/* 路径输出的排序;PathKey 节点列表;见上 */
	List	   *pathkeys;
} Path;

/* 提取路径参数化 relids 的宏;当心双重求值 */
#define PATH_REQ_OUTER(path)  \
	((path)->param_info ? (path)->param_info->ppi_req_outer : (Relids) NULL)

/*----------
 * IndexPath represents an index scan over a single index.
 *
 * This struct is used for both regular indexscans and index-only scans;
 * path.pathtype is T_IndexScan or T_IndexOnlyScan to show which is meant.
 *
 * 'indexinfo' is the index to be scanned.
 *
 * 'indexclauses' is a list of IndexClause nodes, each representing one
 * index-checkable restriction, with implicit AND semantics across the list.
 * An empty list implies a full index scan.
 *
 * 'indexorderbys', if not NIL, is a list of ORDER BY expressions that have
 * been found to be usable as ordering operators for an amcanorderbyop index.
 * The list must match the path's pathkeys, ie, one expression per pathkey
 * in the same order.  These are not RestrictInfos, just bare expressions,
 * since they generally won't yield booleans.  It's guaranteed that each
 * expression has the index key on the left side of the operator.
 *
 * 'indexorderbycols' is an integer list of index column numbers (zero-based)
 * of the same length as 'indexorderbys', showing which index column each
 * ORDER BY expression is meant to be used with.  (There is no restriction
 * on which index column each ORDER BY can be used with.)
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an index
 *		BackwardScanDirection: backward scan of an ordered index
 * Unordered indexes will always have an indexscandir of ForwardScanDirection.
 *
 * 'indextotalcost' and 'indexselectivity' are saved in the IndexPath so that
 * we need not recompute them when considering using the same index in a
 * bitmap index/heap scan (see BitmapHeapPath).  The costs of the IndexPath
 * itself represent the costs of an IndexScan or IndexOnlyScan plan type.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	IndexOptInfo *indexinfo;
	List	   *indexclauses;
	List	   *indexorderbys;
	List	   *indexorderbycols;
	ScanDirection indexscandir;
	Cost		indextotalcost;
	Selectivity indexselectivity;
} IndexPath;

/*
 * Each IndexClause references a RestrictInfo node from the query's WHERE
 * or JOIN conditions, and shows how that restriction can be applied to
 * the particular index.  We support both indexclauses that are directly
 * usable by the index machinery, which are typically of the form
 * "indexcol OP pseudoconstant", and those from which an indexable qual
 * can be derived.  The simplest such transformation is that a clause
 * of the form "pseudoconstant OP indexcol" can be commuted to produce an
 * indexable qual (the index machinery expects the indexcol to be on the
 * left always).  Another example is that we might be able to extract an
 * indexable range condition from a LIKE condition, as in "x LIKE 'foo%bar'"
 * giving rise to "x >= 'foo' AND x < 'fop'".  Derivation of such lossy
 * conditions is done by a planner support function attached to the
 * indexclause's top-level function or operator.
 *
 * indexquals is a list of RestrictInfos for the directly-usable index
 * conditions associated with this IndexClause.  In the simplest case
 * it's a one-element list whose member is iclause->rinfo.  Otherwise,
 * it contains one or more directly-usable indexqual conditions extracted
 * from the given clause.  The 'lossy' flag indicates whether the
 * indexquals are semantically equivalent to the original clause, or
 * represent a weaker condition.
 *
 * Normally, indexcol is the index of the single index column the clause
 * works on, and indexcols is NIL.  But if the clause is a RowCompareExpr,
 * indexcol is the index of the leading column, and indexcols is a list of
 * all the affected columns.  (Note that indexcols matches up with the
 * columns of the actual indexable RowCompareExpr in indexquals, which
 * might be different from the original in rinfo.)
 *
 * An IndexPath's IndexClause list is required to be ordered by index
 * column, i.e. the indexcol values must form a nondecreasing sequence.
 * (The order of multiple clauses for the same index column is unspecified.)
 */
typedef struct IndexClause
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	struct RestrictInfo *rinfo; /* 原限定或连接子句 */
	List	   *indexquals;		/* 由其派生的 indexqual */
	bool		lossy;			/* indexqual 是否为子句的有损版本? */
	AttrNumber	indexcol;		/* 子句所用的索引列(从零起) */
	List	   *indexcols;		/* 多索引列(若为 RowCompare) */
} IndexClause;

/*
 * BitmapHeapPath represents one or more indexscans that generate TID bitmaps
 * instead of directly accessing the heap, followed by AND/OR combinations
 * to produce a single bitmap, followed by a heap scan that uses the bitmap.
 * Note that the output is always considered unordered, since it will come
 * out in physical heap order no matter what the underlying indexes did.
 *
 * The individual indexscans are represented by IndexPath nodes, and any
 * logic on top of them is represented by a tree of BitmapAndPath and
 * BitmapOrPath nodes.  Notice that we can use the same IndexPath node both
 * to represent a regular (or index-only) index scan plan, and as the child
 * of a BitmapHeapPath that represents scanning the same index using a
 * BitmapIndexScan.  The startup_cost and total_cost figures of an IndexPath
 * always represent the costs to use it as a regular (or index-only)
 * IndexScan.  The costs of a BitmapIndexScan can be computed using the
 * IndexPath's indextotalcost and indexselectivity.
 */
typedef struct BitmapHeapPath
{
	Path		path;
	Path	   *bitmapqual;		/* IndexPath/BitmapAnd/OrPath */
} BitmapHeapPath;

/*
 * BitmapAndPath 表示 BitmapAnd 计划节点；只能作
 * BitmapHeapPath 子结构一部分。Path 结构对此
 * 略偏重，但为简单起见仍令其
 * 派生自 Path。
 */
typedef struct BitmapAndPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPath 与 BitmapOrPath */
	Selectivity bitmapselectivity;
} BitmapAndPath;

/*
 * BitmapOrPath 表示 BitmapOr 计划节点；只能作
 * BitmapHeapPath 子结构一部分。Path 结构对此
 * 略偏重，但为简单起见仍令其
 * 派生自 Path。
 */
typedef struct BitmapOrPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPath 与 BitmapAndPath */
	Selectivity bitmapselectivity;
} BitmapOrPath;

/*
 * TidPath 表示按 TID 扫描
 *
 * tidquals 是隐式 OR 的限定表达式列表，形如
 * "CTID = 伪常量"，或 "CTID = ANY(伪常量数组)"，
 * 或该关系上的 CurrentOfExpr。
 */
typedef struct TidPath
{
	Path		path;
	List	   *tidquals;		/* 涉及 CTID = 某值的限定 */
} TidPath;

/*
 * TidRangePath 表示按连续 TIDs 范围扫描
 *
 * tidrangequals 是隐式 AND 的限定表达式列表，形如
 * "CTID relop 伪常量"，relop 为 >、>=、<、<= 之一。
 */
typedef struct TidRangePath
{
	Path		path;
	List	   *tidrangequals;
} TidRangePath;

/*
 * SubqueryScanPath 表示扫描未展平的 FROM 子查询
 *
 * 注意 subpath 来自不同规划域；例如其中
 * RTE 下标含义与 SubqueryScanPath 所知不同。
 * path.parent->subroot 是解释 subpath 所需的
 * 规划上下文。
 */
typedef struct SubqueryScanPath
{
	Path		path;
	Path	   *subpath;		/* 表示子查询执行的路径 */
} SubqueryScanPath;

/*
 * ForeignPath 表示外表、外部连接或外部
 * 上层关系的扫描。
 *
 * fdw_private 存放 FDW 私有扫描数据。核心代码
 * 正常运行时并不触碰它，但通常宜采用
 * 可被 nodeToString() 转储的表示，以便调试时
 * 用 pprint() 等工具查看结构。
 * 之类工具。
 */
typedef struct ForeignPath
{
	Path		path;
	Path	   *fdw_outerpath;
	List	   *fdw_private;
} ForeignPath;

/*
 * CustomPath represents a table scan or a table join done by some out-of-core
 * extension.
 *
 * We provide a set of hooks here - which the provider must take care to set
 * up correctly - to allow extensions to supply their own methods of scanning
 * a relation or joing relations.  For example, a provider might provide GPU
 * acceleration, a cache-based scan, or some other kind of logic we haven't
 * dreamed up yet.
 *
 * CustomPaths can be injected into the planning process for a base or join
 * relation by set_rel_pathlist_hook or set_join_pathlist_hook functions,
 * respectively.
 *
 * Core code must avoid assuming that the CustomPath is only as large as
 * the structure declared here; providers are allowed to make it the first
 * element in a larger structure.  (Since the planner never copies Paths,
 * this doesn't add any complication.)  However, for consistency with the
 * FDW case, we provide a "custom_private" field in CustomPath; providers
 * may prefer to use that rather than define another struct type.
 */

struct CustomPathMethods;

typedef struct CustomPath
{
	Path		path;
	uint32		flags;			/* CUSTOMPATH_* 标志掩码，见
								 * nodes/extensible.h */
	List	   *custom_paths;	/* 子 Path 节点列表(若有) */
	List	   *custom_private;
	const struct CustomPathMethods *methods;
} CustomPath;

/*
 * AppendPath 表示 Append 计划，即相继执行
 * 若干成员计划。
 *
 * 对部分 Append，subpaths 先非部分再跟
 * 部分路径。
 *
 * 注意："subpaths" 可以只含一个元素，甚至为空。
 * 这些情况在 create_append_plan 中优化。
 * 特别地，无子路径的 AppendPath 是 "dummy" 路径，
 * 用来表示关系已被证明为空。
 * （此表示很方便：构建 appendrel 时若发现
 * 全部子项均被排除，无需额外动作即可
 * 将该关系识别为 dummy。）
 */
typedef struct AppendPath
{
	Path		path;
	List	   *subpaths;		/* 成分 Path 列表 */
	/* subpaths 中首个部分路径下标;无则为 list_length(subpaths) */
	int			first_partial_path;
	Cardinality limit_tuples;	/* 输出元组硬上限,或 -1 */
} AppendPath;

#define IS_DUMMY_APPEND(p) \
	(IsA((p), AppendPath) && ((AppendPath *) (p))->subpaths == NIL)

/*
 * 已证空的关系会有一条 dummy 路径
 * （其上或有投影路径）。因历史原因，
 * 以包装 is_dummy_rel() 的宏提供。
 */
#define IS_DUMMY_REL(r) is_dummy_rel(r)
extern bool is_dummy_rel(RelOptInfo *rel);

/*
 * MergeAppendPath 表示 MergeAppend 计划，即将若干
 * 成员计划的已排序结果归并为同样有序的输出。
 */
typedef struct MergeAppendPath
{
	Path		path;
	List	   *subpaths;		/* 成分 Path 列表 */
	Cardinality limit_tuples;	/* 输出元组硬上限,或 -1 */
} MergeAppendPath;

/*
 * GroupResultPath 表示用 Result 计划节点计算
 * 退化 GROUP BY：已知应产出恰好一行，
 * 随后或可由 HAVING 限定过滤。
 *
 * 注意 quals 是裸子句列表，非 RestrictInfos。
 */
typedef struct GroupResultPath
{
	Path		path;
	List	   *quals;
} GroupResultPath;

/*
 * MaterialPath 表示使用 Material 计划节点，即
 * 缓存其子路径输出。用于子路径昂贵且需
 * 反复扫描，或需要 mark/restore 能力而
 * 子路径不具备时。
 */
typedef struct MaterialPath
{
	Path		path;
	Path	   *subpath;
} MaterialPath;

/*
 * MemoizePath 表示 Memoize 计划节点，即缓存
 * 参数化路径的元组，以免底层节点对已缓存的
 * 参数值再次扫描。
 */
typedef struct MemoizePath
{
	Path		path;
	Path	   *subpath;		/* 缓存元组的 outerpath */
	List	   *hash_operators; /* 缓存键的哈希相等运算符 OID */
	List	   *param_exprs;	/* 作为缓存键的表达式 */
	bool		singlerow;		/* 缓存首条后即将该项标为
								 * complete，则为 true。 */
	bool		binary_mode;	/* 为 true 时缓存键按位比较，
								 * 为 false 时用哈希相等操作符。 */
	Cardinality calls;			/* 期望的重扫次数 */
	uint32		est_entries;	/* 规划器预期缓存最大
								 * 条目数，未知则为
								 * 0 */
} MemoizePath;

/*
 * UniquePath 表示消除子路径输出中的
 * 重复行。
 *
 * 可表示差异很大的计划：基于哈希或基于
 * 排序的实现，或在输入路径已可证明互异时
 * 为 no-op。决策足够局部，不值得另设
 * Path 节点类型。（注意：no-op 时可整棵去掉
 * UniquePath 只返回子路径；但在路径树中保留
 * UniquePath 便于向上层例程表明输入
 * 已知互异。）
 */
typedef enum UniquePathMethod
{
	UNIQUE_PATH_NOOP,			/* 输入已知已唯一 */
	UNIQUE_PATH_HASH,			/* 用哈希 */
	UNIQUE_PATH_SORT			/* 用排序 */
} UniquePathMethod;

typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	UniquePathMethod umethod;
	List	   *in_operators;	/* IN 子句的相等运算符 */
	List	   *uniq_exprs;		/* 要做成唯一的表达式 */
} UniquePath;

/*
 * GatherPath 并行运行若干计划副本并收集
 * 结果。除非设置 single_copy，并行领导者
 * 也可执行该计划。
 */
typedef struct GatherPath
{
	Path		path;
	Path	   *subpath;		/* 每工作者路径 */
	bool		single_copy;	/* 路径不要执行超过一次 */
	int			num_workers;	/* 寻求帮助的工作者数 */
} GatherPath;

/*
 * GatherMergePath 并行运行若干计划副本并收集
 * 结果，同时保持其共同排序顺序。
 */
typedef struct GatherMergePath
{
	Path		path;
	Path	   *subpath;		/* 每工作者路径 */
	int			num_workers;	/* 寻求帮助的工作者数 */
} GatherMergePath;


/*
 * 各连接类型路径共享这些字段
 */

typedef struct JoinPath
{
	pg_node_attr(abstract)

	Path		path;

	JoinType	jointype;

	bool		inner_unique;	/* 每个外表元组可证明至多匹配
								 * 一个内表元组 */

	Path	   *outerjoinpath;	/* 连接外层的路径 */
	Path	   *innerjoinpath;	/* 连接内层的路径 */

	List	   *joinrestrictinfo;	/* 用于连接的 RestrictInfos */

	/*
	 * 参见 RelOptInfo 与 ParamPathInfo 的说明，以理解
	 * 为何 JoinPath 需要 joinrestrictinfo，且不能并入
	 * 父 RelOptInfo。
	 */
} JoinPath;

/*
 * 嵌套循环路径无需特殊字段。
 */

typedef struct NestPath
{
	JoinPath	jpath;
} NestPath;

/*
 * A mergejoin path has these fields.
 *
 * Unlike other path types, a MergePath node doesn't represent just a single
 * run-time plan node: it can represent up to four.  Aside from the MergeJoin
 * node itself, there can be a Sort node for the outer input, a Sort node
 * for the inner input, and/or a Material node for the inner input.  We could
 * represent these nodes by separate path nodes, but considering how many
 * different merge paths are investigated during a complex join problem,
 * it seems better to avoid unnecessary palloc overhead.
 *
 * path_mergeclauses lists the clauses (in the form of RestrictInfos)
 * that will be used in the merge.
 *
 * Note that the mergeclauses are a subset of the parent relation's
 * restriction-clause list.  Any join clauses that are not mergejoinable
 * appear only in the parent's restrict list, and must be checked by a
 * qpqual at execution time.
 *
 * outersortkeys (resp. innersortkeys) is NIL if the outer path
 * (resp. inner path) is already ordered appropriately for the
 * mergejoin.  If it is not NIL then it is a PathKeys list describing
 * the ordering that must be created by an explicit Sort node.
 *
 * skip_mark_restore is true if the executor need not do mark/restore calls.
 * Mark/restore overhead is usually required, but can be skipped if we know
 * that the executor need find only one match per outer tuple, and that the
 * mergeclauses are sufficient to identify a match.  In such cases the
 * executor can immediately advance the outer relation after processing a
 * match, and therefore it need never back up the inner relation.
 *
 * materialize_inner is true if a Material node should be placed atop the
 * inner input.  This may appear with or without an inner Sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;	/* 用于归并的连接子句 */
	List	   *outersortkeys;	/* 显式排序的键(若有) */
	List	   *innersortkeys;	/* 显式排序的键(若有) */
	bool		skip_mark_restore;	/* 可跳过 mark/restore? */
	bool		materialize_inner;	/* 给内层加 Materialize? */
} MergePath;

/*
 * 哈希连接路径有这些字段
 *
 * 上文关于 mergeclauses 的说明同样适用于 hashclauses。
 *
 * 哈希连接不关心输入顺序，故无需
 * sortkeys。
 */

typedef struct HashPath
{
	JoinPath	jpath;
	List	   *path_hashclauses;	/* 用于哈希的连接子句 */
	int			num_batches;	/* 期望的批次数 */
	Cardinality inner_rows_total;	/* 期望的内层总行数 */
} HashPath;

/*
 * ProjectionPath 表示投影（即目标列表计算）
 *
 * 名义上本路径节点表示用 Result 计划节点做
 * 投影步。但若输入计划节点支持投影，
 * 可直接改其输出目标列表完成所需计算，
 * 而无需 Result。规划器有处可直接把目标
 * PathTarget 塞进输入路径节点（并相应调整
 * 代价），故不需要 ProjectionPath。但别处
 * 不能改动输入路径节点，则需单独的
 * ProjectionPath，并标为 dummy 表示打算把
 * 工作交给输入计划节点。ProjectionPath 的
 * 估计代价会计入是否实际使用 Result。
 */
typedef struct ProjectionPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	bool		dummypp;		/* 无需单独 Result 则为真 */
} ProjectionPath;

/*
 * ProjectSetPath 表示求值含集合返回函数的
 * 目标列表，需由 ProjectSet 计划节点
 * 实现。
 */
typedef struct ProjectSetPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
} ProjectSetPath;

/*
 * SortPath 表示显式排序步骤
 *
 * 排序键按定义与 path.pathkeys 相同。
 *
 * 注意：Sort 计划节点不能投影，故 path.pathtarget 须与
 * 输入的 pathtarget 相同。
 */
typedef struct SortPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
} SortPath;

/*
 * IncrementalSortPath 表示增量排序步骤
 *
 * 类似普通排序，但假定若干前导键列
 * 已经有序。
 */
typedef struct IncrementalSortPath
{
	SortPath	spath;
	int			nPresortedCols; /* 预排序列数 */
} IncrementalSortPath;

/*
 * GroupPath 表示分组（输入已预排序）
 *
 * groupClause 表示要分组的列；输入路径
 * 至少须按此排序。
 *
 * 也可对分组后行施加限定（等价于 HAVING）
 */
typedef struct GroupPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	List	   *groupClause;	/* SortGroupClause 列表 */
	List	   *qual;			/* 限定(HAVING)(若有) */
} GroupPath;

/*
 * UpperUniquePath 表示相邻重复消除（输入已预排序）
 *
 * 比较的列是路径 pathkeys 的前 numkeys 列。
 * 假定输入已按该方式排好序。
 */
typedef struct UpperUniquePath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	int			numkeys;		/* 要比较的 pathkey 列数 */
} UpperUniquePath;

/*
 * AggPath 表示聚集函数的通用计算
 *
 * 可涉及普通分组（不含分组集），用排序或
 * 哈希分组；AGG_SORTED 时输入必须
 * 已适当预排序。
 */
typedef struct AggPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	AggStrategy aggstrategy;	/* 基本策略,见 nodes.h */
	AggSplit	aggsplit;		/* 聚合拆分模式,见 nodes.h */
	Cardinality numGroups;		/* 输入中估计的组数 */
	uint64		transitionSpace;	/* 供传引用转移数据 */
	List	   *groupClause;	/* SortGroupClause 列表 */
	List	   *qual;			/* 限定(HAVING)(若有) */
} AggPath;

/*
 * 规划器中分组集所用的各种注解。
 */

typedef struct GroupingSetData
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	List	   *set;			/* 分组集,为 sortgroupref 列表 */
	Cardinality numGroups;		/* 估计的结果组数 */
} GroupingSetData;

typedef struct RollupData
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	List	   *groupClause;	/* parse->groupClause 的适用子集 */
	List	   *gsets;			/* 指向 groupClause 的整数下标列表 */
	List	   *gsets_data;		/* GroupingSetData 列表 */
	Cardinality numGroups;		/* 估计的结果组数 */
	bool		hashable;		/* 可哈希 */
	bool		is_hashed;		/* 要以 hashagg 实现 */
} RollupData;

/*
 * GroupingSetsPath 表示 GROUPING SETS 聚集
 */

typedef struct GroupingSetsPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	AggStrategy aggstrategy;	/* 基本策略 */
	List	   *rollups;		/* RollupData 列表 */
	List	   *qual;			/* 限定(HAVING)(若有) */
	uint64		transitionSpace;	/* 供传引用转移数据 */
} GroupingSetsPath;

/*
 * MinMaxAggPath 表示用索引计算 MIN/MAX 聚集
 */
typedef struct MinMaxAggPath
{
	Path		path;
	List	   *mmaggregates;	/* MinMaxAggInfo 列表 */
	List	   *quals;			/* HAVING 限定,若有 */
} MinMaxAggPath;

/*
 * WindowAggPath 表示窗口函数的通用计算
 */
typedef struct WindowAggPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	WindowClause *winclause;	/* 将使用的 WindowClause */
	List	   *qual;			/* 下层 WindowAgg 的 runcondition */
	bool		topwindow;		/* 除最靠近计划树根的
								 * WindowAgg 外均为 false */
} WindowAggPath;

/*
 * SetOpPath 表示集合运算（INTERSECT/EXCEPT）
 */
typedef struct SetOpPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	SetOpCmd	cmd;			/* 做什么,见 nodes.h */
	SetOpStrategy strategy;		/* 怎么做,见 nodes.h */
	List	   *distinctList;	/* 标识目标列的 SortGroupClause */
	AttrNumber	flagColIdx;		/* 标志列在何处(若有) */
	int			firstFlag;		/* 第一个输入关系的标志值 */
	Cardinality numGroups;		/* 输入中估计的组数 */
} SetOpPath;

/*
 * RecursiveUnionPath 表示递归 UNION 节点
 */
typedef struct RecursiveUnionPath
{
	Path		path;
	Path	   *leftpath;		/* 表示输入源的路径 */
	Path	   *rightpath;
	List	   *distinctList;	/* 标识目标列的 SortGroupClause */
	int			wtParam;		/* 表示工作表的 Param ID */
	Cardinality numGroups;		/* 输入中估计的组数 */
} RecursiveUnionPath;

/*
 * LockRowsPath 表示 SELECT FOR UPDATE/SHARE 行锁
 */
typedef struct LockRowsPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	List	   *rowMarks;		/* PlanRowMark 列表 */
	int			epqParam;		/* EvalPlanQual 重求值的 Param ID */
} LockRowsPath;

/*
 * ModifyTablePath 表示执行 INSERT/UPDATE/DELETE/MERGE
 *
 * ModifyTable 计划节点中多数内容按字面表示，
 * 但子节点是 Path 而非 Plan。OnConflictExpr
 * 分析与 FDW 数据收集推迟到 createplan.c。
 */
typedef struct ModifyTablePath
{
	Path		path;
	Path	   *subpath;		/* 产生源数据的 Path */
	CmdType		operation;		/* INSERT/UPDATE/DELETE/MERGE */
	bool		canSetTag;		/* 是否设置命令标签/es_processed? */
	Index		nominalRelation;	/* 供 EXPLAIN 用的父 RT 下标 */
	Index		rootRelation;	/* 根 RT 下标(若分区/继承) */
	bool		partColsUpdated;	/* 层次中有部分键被更新? */
	List	   *resultRelations;	/* RT 下标的整数列表 */
	List	   *updateColnosLists;	/* 每目标表的 update_colnos 列表 */
	List	   *withCheckOptionLists;	/* 每目标表的 WCO 列表 */
	List	   *returningLists; /* 每目标表的 RETURNING tlist */
	List	   *rowMarks;		/* PlanRowMark(仅非加锁) */
	OnConflictExpr *onconflict; /* ON CONFLICT 子句,或 NULL */
	int			epqParam;		/* EvalPlanQual 重求值的 Param ID */
	List	   *mergeActionLists;	/* 每目标表的动作列表,供
									 * MERGE */
} ModifyTablePath;

/*
 * LimitPath 表示施加 LIMIT/OFFSET 限制
 */
typedef struct LimitPath
{
	Path		path;
	Path	   *subpath;		/* 表示输入源的路径 */
	Node	   *limitOffset;	/* OFFSET 参数,无则为 NULL */
	Node	   *limitCount;		/* COUNT 参数,无则为 NULL */
	LimitOption limitOption;	/* FETCH FIRST 带并列或精确个数 */
} LimitPath;


/*
 * Restriction clause info.
 *
 * We create one of these for each AND sub-clause of a restriction condition
 * (WHERE or JOIN/ON clause).  Since the restriction clauses are logically
 * ANDed, we can use any one of them or any subset of them to filter out
 * tuples, without having to evaluate the rest.  The RestrictInfo node itself
 * stores data used by the optimizer while choosing the best query plan.
 *
 * If a restriction clause references a single base relation, it will appear
 * in the baserestrictinfo list of the RelOptInfo for that base rel.
 *
 * If a restriction clause references more than one base+OJ relation, it will
 * appear in the joininfo list of every RelOptInfo that describes a strict
 * subset of the relations mentioned in the clause.  The joininfo lists are
 * used to drive join tree building by selecting plausible join candidates.
 * The clause cannot actually be applied until we have built a join rel
 * containing all the relations it references, however.
 *
 * When we construct a join rel that includes all the relations referenced
 * in a multi-relation restriction clause, we place that clause into the
 * joinrestrictinfo lists of paths for the join rel, if neither left nor
 * right sub-path includes all relations referenced in the clause.  The clause
 * will be applied at that join level, and will not propagate any further up
 * the join tree.  (Note: the "predicate migration" code was once intended to
 * push restriction clauses up and down the plan tree based on evaluation
 * costs, but it's dead code and is unlikely to be resurrected in the
 * foreseeable future.)
 *
 * Note that in the presence of more than two rels, a multi-rel restriction
 * might reach different heights in the join tree depending on the join
 * sequence we use.  So, these clauses cannot be associated directly with
 * the join RelOptInfo, but must be kept track of on a per-join-path basis.
 *
 * RestrictInfos that represent equivalence conditions (i.e., mergejoinable
 * equalities that are not outerjoin-delayed) are handled a bit differently.
 * Initially we attach them to the EquivalenceClasses that are derived from
 * them.  When we construct a scan or join path, we look through all the
 * EquivalenceClasses and generate derived RestrictInfos representing the
 * minimal set of conditions that need to be checked for this particular scan
 * or join to enforce that all members of each EquivalenceClass are in fact
 * equal in all rows emitted by the scan or join.
 *
 * The clause_relids field lists the base plus outer-join RT indexes that
 * actually appear in the clause.  required_relids lists the minimum set of
 * relids needed to evaluate the clause; while this is often equal to
 * clause_relids, it can be more.  We will add relids to required_relids when
 * we need to force an outer join ON clause to be evaluated exactly at the
 * level of the outer join, which is true except when it is a "degenerate"
 * condition that references only Vars from the nullable side of the join.
 *
 * RestrictInfo nodes contain a flag to indicate whether a qual has been
 * pushed down to a lower level than its original syntactic placement in the
 * join tree would suggest.  If an outer join prevents us from pushing a qual
 * down to its "natural" semantic level (the level associated with just the
 * base rels used in the qual) then we mark the qual with a "required_relids"
 * value including more than just the base rels it actually uses.  By
 * pretending that the qual references all the rels required to form the outer
 * join, we prevent it from being evaluated below the outer join's joinrel.
 * When we do form the outer join's joinrel, we still need to distinguish
 * those quals that are actually in that join's JOIN/ON condition from those
 * that appeared elsewhere in the tree and were pushed down to the join rel
 * because they used no other rels.  That's what the is_pushed_down flag is
 * for; it tells us that a qual is not an OUTER JOIN qual for the set of base
 * rels listed in required_relids.  A clause that originally came from WHERE
 * or an INNER JOIN condition will *always* have its is_pushed_down flag set.
 * It's possible for an OUTER JOIN clause to be marked is_pushed_down too,
 * if we decide that it can be pushed down into the nullable side of the join.
 * In that case it acts as a plain filter qual for wherever it gets evaluated.
 * (In short, is_pushed_down is only false for non-degenerate outer join
 * conditions.  Possibly we should rename it to reflect that meaning?  But
 * see also the comments for RINFO_IS_PUSHED_DOWN, below.)
 *
 * There is also an incompatible_relids field, which is a set of outer-join
 * relids above which we cannot evaluate the clause (because they might null
 * Vars it uses that should not be nulled yet).  In principle this could be
 * filled in any RestrictInfo as the set of OJ relids that appear above the
 * clause and null Vars that it uses.  In practice we only bother to populate
 * it for "clone" clauses, as it's currently only needed to prevent multiple
 * clones of the same clause from being accepted for evaluation at the same
 * join level.
 *
 * There is also an outer_relids field, which is NULL except for outer join
 * clauses; for those, it is the set of relids on the outer side of the
 * clause's outer join.  (These are rels that the clause cannot be applied to
 * in parameterized scans, since pushing it into the join's outer side would
 * lead to wrong answers.)
 *
 * To handle security-barrier conditions efficiently, we mark RestrictInfo
 * nodes with a security_level field, in which higher values identify clauses
 * coming from less-trusted sources.  The exact semantics are that a clause
 * cannot be evaluated before another clause with a lower security_level value
 * unless the first clause is leakproof.  As with outer-join clauses, this
 * creates a reason for clauses to sometimes need to be evaluated higher in
 * the join tree than their contents would suggest; and even at a single plan
 * node, this rule constrains the order of application of clauses.
 *
 * In general, the referenced clause might be arbitrarily complex.  The
 * kinds of clauses we can handle as indexscan quals, mergejoin clauses,
 * or hashjoin clauses are limited (e.g., no volatile functions).  The code
 * for each kind of path is responsible for identifying the restrict clauses
 * it can use and ignoring the rest.  Clauses not implemented by an indexscan,
 * mergejoin, or hashjoin will be placed in the plan qual or joinqual field
 * of the finished Plan node, where they will be enforced by general-purpose
 * qual-expression-evaluation code.  (But we are still entitled to count
 * their selectivity when estimating the result tuple count, if we
 * can guess what it is...)
 *
 * When the referenced clause is an OR clause, we generate a modified copy
 * in which additional RestrictInfo nodes are inserted below the top-level
 * OR/AND structure.  This is a convenience for OR indexscan processing:
 * indexquals taken from either the top level or an OR subclause will have
 * associated RestrictInfo nodes.
 *
 * The can_join flag is set true if the clause looks potentially useful as
 * a merge or hash join clause, that is if it is a binary opclause with
 * nonoverlapping sets of relids referenced in the left and right sides.
 * (Whether the operator is actually merge or hash joinable isn't checked,
 * however.)
 *
 * The pseudoconstant flag is set true if the clause contains no Vars of
 * the current query level and no volatile functions.  Such a clause can be
 * pulled out and used as a one-time qual in a gating Result node.  We keep
 * pseudoconstant clauses in the same lists as other RestrictInfos so that
 * the regular clause-pushing machinery can assign them to the correct join
 * level, but they need to be treated specially for cost and selectivity
 * estimates.  Note that a pseudoconstant clause can never be an indexqual
 * or merge or hash join clause, so it's of no interest to large parts of
 * the planner.
 *
 * When we generate multiple versions of a clause so as to have versions
 * that will work after commuting some left joins per outer join identity 3,
 * we mark the one with the fewest nullingrels bits with has_clone = true,
 * and the rest with is_clone = true.  This allows proper filtering of
 * these redundant clauses, so that we apply only one version of them.
 *
 * When join clauses are generated from EquivalenceClasses, there may be
 * several equally valid ways to enforce join equivalence, of which we need
 * apply only one.  We mark clauses of this kind by setting parent_ec to
 * point to the generating EquivalenceClass.  Multiple clauses with the same
 * parent_ec in the same join are redundant.
 *
 * Most fields are ignored for equality, since they may not be set yet, and
 * should be derivable from the clause anyway.
 *
 * parent_ec, left_ec, right_ec are not printed, lest it lead to infinite
 * recursion in plan tree dump.
 */

typedef struct RestrictInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* 所表示的 WHERE 或 JOIN 子句 */
	Expr	   *clause;

	/* 子句在本层被下推则为真 */
	bool		is_pushed_down;

	/* 见上方注释 */
	bool		can_join pg_node_attr(equal_ignore);

	/* 见上方注释 */
	bool		pseudoconstant pg_node_attr(equal_ignore);

	/* 见上方注释 */
	bool		has_clone;
	bool		is_clone;

	/* 已知不含泄漏的 Var 则为真 */
	bool		leakproof pg_node_attr(equal_ignore);

	/* 子句是否含易变函数 */
	VolatileFunctionStatus has_volatile pg_node_attr(equal_ignore);

	/* 见上方注释 */
	Index		security_level;

	/* clause_relids 中的基关系数 */
	int			num_base_rels pg_node_attr(equal_ignore);

	/* 子句中实际引用的 relids(varnos+varnullingrels): */
	Relids		clause_relids pg_node_attr(equal_ignore);

	/* 求值该子句所需的 relid 集合: */
	Relids		required_relids;

	/* 之上无法求值该子句的 Relids(见上方注释) */
	Relids		incompatible_relids;

	/* 若为外连接子句则为外层关系,否则 NULL: */
	Relids		outer_relids;

	/*
	 * 子句左/右侧的 Relids。对任意二元
	 * opclause 设置。
	 */
	Relids		left_relids pg_node_attr(equal_ignore);
	Relids		right_relids pg_node_attr(equal_ignore);

	/*
	 * 带 RestrictInfos 的改写子句。仅当为
	 * OR 时非 NULL
	 */
	Expr	   *orclause pg_node_attr(equal_ignore);

	/*----------
	 * Serial number of this RestrictInfo.  This is unique within the current
	 * PlannerInfo context, with a few critical exceptions:
	 * 1. When we generate multiple clones of the same qual condition to
	 * cope with outer join identity 3, all the clones get the same serial
	 * number.  This reflects that we only want to apply one of them in any
	 * given plan.
	 * 2. If we manufacture a commuted version of a qual to use as an index
	 * condition, it copies the original's rinfo_serial, since it is in
	 * practice the same condition.
	 * 3. RestrictInfos made for a child relation copy their parent's
	 * rinfo_serial.  Likewise, when an EquivalenceClass makes a derived
	 * equality clause for a child relation, it copies the rinfo_serial of
	 * the matching equality clause for the parent.  This allows detection
	 * of redundant pushed-down equality clauses.
	 *----------
	 */
	int			rinfo_serial;

	/*
	 * 生成用 EquivalenceClass。仅当子句可能
	 * 冗余时非 NULL。
	 */
	EquivalenceClass *parent_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);

	/*
	 * cache space for cost and selectivity
	 */

	/* 子句求值代价;-1 为尚未设置 */
	QualCost	eval_cost pg_node_attr(equal_ignore);

	/* "普通"(JOIN_INNER)语义的选择率;-1 尚未设置 */
	Selectivity norm_selec pg_node_attr(equal_ignore);
	/* 外连接语义的选择率;-1 尚未设置 */
	Selectivity outer_selec pg_node_attr(equal_ignore);

	/*
	 * 含子句操作符的 opfamilies；可归并连接时
	 * 有效，否则 NIL
	 */
	List	   *mergeopfamilies pg_node_attr(equal_ignore);

	/*
	 * cache space for mergeclause processing; NULL if not yet set
	 */

	/* 含左侧的 EquivalenceClass */
	EquivalenceClass *left_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);
	/* 含右侧的 EquivalenceClass */
	EquivalenceClass *right_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);
	/* 左侧的 EquivalenceMember */
	EquivalenceMember *left_em pg_node_attr(copy_as_scalar, equal_ignore);
	/* 右侧的 EquivalenceMember */
	EquivalenceMember *right_em pg_node_attr(copy_as_scalar, equal_ignore);

	/*
	 * MergeScanSelCache 结构列表。它们不是 Node，难
	 * 复制；改为用 NIL 替换。复制效果是重置缓存。
	 * 同样无法比较或打印它们。
	 */
	List	   *scansel_cache pg_node_attr(copy_as(NIL), equal_ignore, read_write_ignore);

	/*
	 * 考虑特定连接路径时的临时工作区；T=
	 * 外表变量在左，F=在右
	 */
	bool		outer_is_left pg_node_attr(equal_ignore);

	/*
	 * 子句运算符拷贝;可哈希连接则有效,否则
	 * InvalidOid
	 */
	Oid			hashjoinoperator pg_node_attr(equal_ignore);

	/*
	 * cache space for hashclause processing; -1 if not yet set
	 */
	/* 左侧平均桶大小 */
	Selectivity left_bucketsize pg_node_attr(equal_ignore);
	/* 右侧平均桶大小 */
	Selectivity right_bucketsize pg_node_attr(equal_ignore);
	/* 左侧最常见值的频率 */
	Selectivity left_mcvfreq pg_node_attr(equal_ignore);
	/* 右侧最常见值的频率 */
	Selectivity right_mcvfreq pg_node_attr(equal_ignore);

	/* memoize 节点用的哈希相等运算符,否则 InvalidOid */
	Oid			left_hasheqoperator pg_node_attr(equal_ignore);
	Oid			right_hasheqoperator pg_node_attr(equal_ignore);
} RestrictInfo;

/*
 * 本宏给出判定 RestrictInfo 是否被
 * "下推"到给定外连接的正确方法，即在该处应作
 * 过滤子句而非连接子句。若 is_pushed_down
 * 为真则必如此；但仅看它已不够，因为生成被
 * 上层 LHS 参数化的下层外连接路径时，外连接
 * 子句会被下推到更低外连接。可通过其
 * required_relids 超出该连接作用域
 * 来识别此类子句。
 */
#define RINFO_IS_PUSHED_DOWN(rinfo, joinrelids) \
	((rinfo)->is_pushed_down || \
	 !bms_is_subset((rinfo)->required_relids, joinrelids))

/*
 * 因 mergejoinscansel() 相对昂贵，且在规划
 * 大型连接树时会被多次调用，我们特意
 * 缓存其结果。每个可归并连接的
 * RestrictInfo 携带已考虑过的具体排序及
 * 相应选择率列表。
 */
typedef struct MergeScanSelCache
{
	/* 排序细节(缓存查找键) */
	Oid			opfamily;		/* 定义排序的 btree 运算符族 */
	Oid			collation;		/* 排序用的排序规则 */
	int			strategy;		/* 排序方向(ASC 或 DESC) */
	bool		nulls_first;	/* 空值是否排在普通值之前? */
	/* Results */
	Selectivity leftstartsel;	/* 子句左侧的首连接分数 */
	Selectivity leftendsel;		/* 子句左侧的末连接分数 */
	Selectivity rightstartsel;	/* 子句右侧的首连接分数 */
	Selectivity rightendsel;	/* 子句右侧的末连接分数 */
} MergeScanSelCache;

/*
 * Placeholder node for an expression to be evaluated below the top level
 * of a plan tree.  This is used during planning to represent the contained
 * expression.  At the end of the planning process it is replaced by either
 * the contained expression or a Var referring to a lower-level evaluation of
 * the contained expression.  Generally the evaluation occurs below an outer
 * join, and Var references above the outer join might thereby yield NULL
 * instead of the expression value.
 *
 * phrels and phlevelsup correspond to the varno/varlevelsup fields of a
 * plain Var, except that phrels has to be a relid set since the evaluation
 * level of a PlaceHolderVar might be a join rather than a base relation.
 * Likewise, phnullingrels corresponds to varnullingrels.
 *
 * Although the planner treats this as an expression node type, it is not
 * recognized by the parser or executor, so we declare it here rather than
 * in primnodes.h.
 *
 * We intentionally do not compare phexpr.  Two PlaceHolderVars with the
 * same ID and levelsup should be considered equal even if the contained
 * expressions have managed to mutate to different states.  This will
 * happen during final plan construction when there are nested PHVs, since
 * the inner PHV will get replaced by a Param in some copies of the outer
 * PHV.  Another way in which it can happen is that initplan sublinks
 * could get replaced by differently-numbered Params when sublink folding
 * is done.  (The end result of such a situation would be some
 * unreferenced initplans, which is annoying but not really a problem.)
 * On the same reasoning, there is no need to examine phrels.  But we do
 * need to compare phnullingrels, as that represents effects that are
 * external to the original value of the PHV.
 */

typedef struct PlaceHolderVar
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;

	/* 所表示的表达式 */
	Expr	   *phexpr pg_node_attr(equal_ignore);

	/* 语法上在表达式源内的基+OJ relid */
	Relids		phrels pg_node_attr(equal_ignore);

	/* 可使 PHV 值变空的外连接 RT 下标 */
	Relids		phnullingrels;

	/* PHV 的 ID(规划器运行内唯一) */
	Index		phid;

	/* >0 表示 PHV 属于外层查询 */
	Index		phlevelsup;
} PlaceHolderVar;

/*
 * "Special join" info.
 *
 * One-sided outer joins constrain the order of joining partially but not
 * completely.  We flatten such joins into the planner's top-level list of
 * relations to join, but record information about each outer join in a
 * SpecialJoinInfo struct.  These structs are kept in the PlannerInfo node's
 * join_info_list.
 *
 * Similarly, semijoins and antijoins created by flattening IN (subselect)
 * and EXISTS(subselect) clauses create partial constraints on join order.
 * These are likewise recorded in SpecialJoinInfo structs.
 *
 * We make SpecialJoinInfos for FULL JOINs even though there is no flexibility
 * of planning for them, because this simplifies make_join_rel()'s API.
 *
 * min_lefthand and min_righthand are the sets of base+OJ relids that must be
 * available on each side when performing the special join.
 * It is not valid for either min_lefthand or min_righthand to be empty sets;
 * if they were, this would break the logic that enforces join order.
 *
 * syn_lefthand and syn_righthand are the sets of base+OJ relids that are
 * syntactically below this special join.  (These are needed to help compute
 * min_lefthand and min_righthand for higher joins.)
 *
 * jointype is never JOIN_RIGHT; a RIGHT JOIN is handled by switching
 * the inputs to make it a LEFT JOIN.  It's never JOIN_RIGHT_ANTI either.
 * So the allowed values of jointype in a join_info_list member are only
 * LEFT, FULL, SEMI, or ANTI.
 *
 * ojrelid is the RT index of the join RTE representing this outer join,
 * if there is one.  It is zero when jointype is INNER or SEMI, and can be
 * zero for jointype ANTI (if the join was transformed from a SEMI join).
 * One use for this field is that when constructing the output targetlist of a
 * join relation that implements this OJ, we add ojrelid to the varnullingrels
 * and phnullingrels fields of nullable (RHS) output columns, so that the
 * output Vars and PlaceHolderVars correctly reflect the nulling that has
 * potentially happened to them.
 *
 * commute_above_l is filled with the relids of syntactically-higher outer
 * joins that have been found to commute with this one per outer join identity
 * 3 (see optimizer/README), when this join is in the LHS of the upper join
 * (so, this is the lower join in the first form of the identity).
 *
 * commute_above_r is filled with the relids of syntactically-higher outer
 * joins that have been found to commute with this one per outer join identity
 * 3, when this join is in the RHS of the upper join (so, this is the lower
 * join in the second form of the identity).
 *
 * commute_below_l is filled with the relids of syntactically-lower outer
 * joins that have been found to commute with this one per outer join identity
 * 3 and are in the LHS of this join (so, this is the upper join in the first
 * form of the identity).
 *
 * commute_below_r is filled with the relids of syntactically-lower outer
 * joins that have been found to commute with this one per outer join identity
 * 3 and are in the RHS of this join (so, this is the upper join in the second
 * form of the identity).
 *
 * lhs_strict is true if the special join's condition cannot succeed when the
 * LHS variables are all NULL (this means that an outer join can commute with
 * upper-level outer joins even if it appears in their RHS).  We don't bother
 * to set lhs_strict for FULL JOINs, however.
 *
 * For a semijoin, we also extract the join operators and their RHS arguments
 * and set semi_operators, semi_rhs_exprs, semi_can_btree, and semi_can_hash.
 * This is done in support of possibly unique-ifying the RHS, so we don't
 * bother unless at least one of semi_can_btree and semi_can_hash can be set
 * true.  (You might expect that this information would be computed during
 * join planning; but it's helpful to have it available during planning of
 * parameterized table scans, so we store it in the SpecialJoinInfo structs.)
 *
 * For purposes of join selectivity estimation, we create transient
 * SpecialJoinInfo structures for regular inner joins; so it is possible
 * to have jointype == JOIN_INNER in such a structure, even though this is
 * not allowed within join_info_list.  We also create transient
 * SpecialJoinInfos with jointype == JOIN_INNER for outer joins, since for
 * cost estimation purposes it is sometimes useful to know the join size under
 * plain innerjoin semantics.  Note that lhs_strict and the semi_xxx fields
 * are not set meaningfully within such structs.
 */
#ifndef HAVE_SPECIALJOININFO_TYPEDEF
typedef struct SpecialJoinInfo SpecialJoinInfo;
#define HAVE_SPECIALJOININFO_TYPEDEF 1
#endif

struct SpecialJoinInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;
	Relids		min_lefthand;	/* 连接最小 LHS 中的基+OJ relid */
	Relids		min_righthand;	/* 连接最小 RHS 中的基+OJ relid */
	Relids		syn_lefthand;	/* 语法上在 LHS 内的基+OJ relid */
	Relids		syn_righthand;	/* 语法上在 RHS 内的基+OJ relid */
	JoinType	jointype;		/* 恒为 INNER/LEFT/FULL/SEMI/ANTI */
	Index		ojrelid;		/* 外连接 RT 下标;无则为 0 */
	Relids		commute_above_l;	/* 其上可交换的 OJ(若在 LHS) */
	Relids		commute_above_r;	/* 其上可交换的 OJ(若在 RHS) */
	Relids		commute_below_l;	/* 本连接 LHS 中可交换 OJ */
	Relids		commute_below_r;	/* 本连接 RHS 中可交换 OJ */
	bool		lhs_strict;		/* 连接子句对某 LHS 关系严格 */
	/* 其余字段仅对 JOIN_SEMI 连接类型设置: */
	bool		semi_can_btree; /* semi_operators 全为 btree 则真 */
	bool		semi_can_hash;	/* semi_operators 全为哈希则真 */
	List	   *semi_operators; /* 相等连接运算符的 OID */
	List	   *semi_rhs_exprs; /* 这些运算符的右侧表达式 */
};

/*
 * 临时外连接子句信息。
 *
 * 凡看起来可归并连接的外连接 ON 子句均搁置，
 * 在限定分发末尾特殊处理。
 */
typedef struct OuterJoinClauseInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	RestrictInfo *rinfo;		/* 可归并的外连接子句 */
	SpecialJoinInfo *sjinfo;	/* 该外连接的 SpecialJoinInfo */
} OuterJoinClauseInfo;

/*
 * Append-relation info.
 *
 * When we expand an inheritable table or a UNION-ALL subselect into an
 * "append relation" (essentially, a list of child RTEs), we build an
 * AppendRelInfo for each child RTE.  The list of AppendRelInfos indicates
 * which child RTEs must be included when expanding the parent, and each node
 * carries information needed to translate between columns of the parent and
 * columns of the child.
 *
 * These structs are kept in the PlannerInfo node's append_rel_list, with
 * append_rel_array[] providing a convenient lookup method for the struct
 * associated with a particular child relid (there can be only one, though
 * parent rels may have many entries in append_rel_list).
 *
 * Note: after completion of the planner prep phase, any given RTE is an
 * append parent having entries in append_rel_list if and only if its
 * "inh" flag is set.  We clear "inh" for plain tables that turn out not
 * to have inheritance children, and (in an abuse of the original meaning
 * of the flag) we set "inh" for subquery RTEs that turn out to be
 * flattenable UNION ALL queries.  This lets us avoid useless searches
 * of append_rel_list.
 *
 * Note: the data structure assumes that append-rel members are single
 * baserels.  This is OK for inheritance, but it prevents us from pulling
 * up a UNION ALL member subquery if it contains a join.  While that could
 * be fixed with a more complex data structure, at present there's not much
 * point because no improvement in the plan could result.
 */

typedef struct AppendRelInfo
{
	pg_node_attr(no_query_jumble)

	NodeTag		type;

	/*
	 * 这些字段唯一标识本 append 关系。同一
	 * parent_relid 可有(应有)多个 AppendRelInfos，
	 * 但每个 child_relid 至多一个，因给定 RTE
	 * 不能是多个 append 父的子项。
	 */
	Index		parent_relid;	/* 追加父关系的 RT 下标 */
	Index		child_relid;	/* 追加子关系的 RT 下标 */

	/*
	 * 对继承 appendrel，父与子均为普通关系，
	 * 在此存行类型 OIDs，用于翻译整行 Vars。
	 * 对 UNION-ALL appendrel，父与子均为无命名
	 * 行类型的子查询，此处存 InvalidOid。
	 */
	Oid			parent_reltype; /* 父复合类型的 OID */
	Oid			child_reltype;	/* 子复合类型的 OID */

	/*
	 * 本列表第 N 元是表示对应于父第 N 列的
	 * 子列的 Var 或表达式。用于把引用父
	 * 关系的 Vars 译为对子的引用。对应父已
	 * 删除列时该元素为 NULL（仅继承可能，非
	 * UNION ALL）。继承情况下元素恒为简单 Vars，
	 * 但在 UNION ALL 情况下可为任意
	 * 表达式。
	 *
	 * 注意仅存用户列（attno > 0）。整行 Vars
	 * 特殊处理；系统列（attno < 0）无需特殊翻译，
	 * 因其 attno 对所有表相同。
	 *
	 * 当心：这些 Vars 的 varlevelsup = 0。复制到
	 * 子查询时须调整。
	 */
	List	   *translated_vars;	/* 子项 Vars 中的表达式 */

	/*
	 * 本数组简化反向翻译：由子列号到父列号。
	 * [ccolno - 1] 处是子列 ccolno 对应的
	 * 以 1 为基的父列号；若该子列已删或不在父中
	 * 则为零。
	 */
	int			num_child_cols; /* 数组长度 */
	AttrNumber *parent_colnos pg_node_attr(array_size(num_child_cols));

	/*
	 * 继承时存父表 OID，UNION ALL 时存 InvalidOid。
	 * 仅用于在试图引用已删除父列时生成
	 * 错误消息。
	 */
	Oid			parent_reloid;	/* 父关系的 OID */
} AppendRelInfo;

/*
 * Information about a row-identity "resjunk" column in UPDATE/DELETE/MERGE.
 *
 * In partitioned UPDATE/DELETE/MERGE it's important for child partitions to
 * share row-identity columns whenever possible, so as not to chew up too many
 * targetlist columns.  We use these structs to track which identity columns
 * have been requested.  In the finished plan, each of these will give rise
 * to one resjunk entry in the targetlist of the ModifyTable's subplan node.
 *
 * All the Vars stored in RowIdentityVarInfos must have varno ROWID_VAR, for
 * convenience of detecting duplicate requests.  We'll replace that, in the
 * final plan, with the varno of the generating rel.
 *
 * Outside this list, a Var with varno ROWID_VAR and varattno k is a reference
 * to the k-th element of the row_identity_vars list (k counting from 1).
 * We add such a reference to root->processed_tlist when creating the entry,
 * and it propagates into the plan tree from there.
 */
typedef struct RowIdentityVarInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Var		   *rowidvar;		/* 要求值的 Var(但 varno=ROWID_VAR) */
	int32		rowidwidth;		/* 估计的平均宽度 */
	char	   *rowidname;		/* resjunk 列名 */
	Relids		rowidrels;		/* 使用本列目标关系 RTE 下标 */
} RowIdentityVarInfo;

/*
 * For each distinct placeholder expression generated during planning, we
 * store a PlaceHolderInfo node in the PlannerInfo node's placeholder_list.
 * This stores info that is needed centrally rather than in each copy of the
 * PlaceHolderVar.  The phid fields identify which PlaceHolderInfo goes with
 * each PlaceHolderVar.  Note that phid is unique throughout a planner run,
 * not just within a query level --- this is so that we need not reassign ID's
 * when pulling a subquery into its parent.
 *
 * The idea is to evaluate the expression at (only) the ph_eval_at join level,
 * then allow it to bubble up like a Var until the ph_needed join level.
 * ph_needed has the same definition as attr_needed for a regular Var.
 *
 * The PlaceHolderVar's expression might contain LATERAL references to vars
 * coming from outside its syntactic scope.  If so, those rels are *not*
 * included in ph_eval_at, but they are recorded in ph_lateral.
 *
 * Notice that when ph_eval_at is a join rather than a single baserel, the
 * PlaceHolderInfo may create constraints on join order: the ph_eval_at join
 * has to be formed below any outer joins that should null the PlaceHolderVar.
 *
 * We create a PlaceHolderInfo only after determining that the PlaceHolderVar
 * is actually referenced in the plan tree, so that unreferenced placeholders
 * don't result in unnecessary constraints on join order.
 */

typedef struct PlaceHolderInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* PH 的 ID(规划器运行内唯一) */
	Index		phid;

	/*
	 * PlaceHolderVar 树副本（比较时应冗余，可
	 * 忽略）
	 */
	PlaceHolderVar *ph_var;

	/* 可求值该值的最低层 */
	Relids		ph_eval_at;

	/* 所含侧向引用的 relid(若有) */
	Relids		ph_lateral;

	/* 需要该值的最高层 */
	Relids		ph_needed;

	/* 估计的属性宽度 */
	int32		ph_width;
} PlaceHolderInfo;

/*
 * 描述一个可能用索引优化的 MIN/MAX 聚集
 * 函数。MinMaxAggPath 含其列表；若采纳该路径，
 * 列表存入 root->minmax_aggs 供 setrefs.c 使用。
 */
typedef struct MinMaxAggInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 聚合的 pg_proc Oid */
	Oid			aggfnoid;

	/* 其排序运算符 Oid */
	Oid			aggsortop;

	/* 正在聚合的表达式 */
	Expr	   *target;

	/*
	 * 规划子查询用的改写 "root"；不打印，太大且
	 * 不够有趣
	 */
	PlannerInfo *subroot pg_node_attr(read_write_ignore);

	/* 子查询的访问路径 */
	Path	   *path;

	/* 取首行的估计代价 */
	Cost		pathcost;

	/* 子计划输出的 param */
	Param	   *param;
} MinMaxAggInfo;

/*
 * At runtime, PARAM_EXEC slots are used to pass values around from one plan
 * node to another.  They can be used to pass values down into subqueries (for
 * outer references in subqueries), or up out of subqueries (for the results
 * of a subplan), or from a NestLoop plan node into its inner relation (when
 * the inner scan is parameterized with values from the outer relation).
 * The planner is responsible for assigning nonconflicting PARAM_EXEC IDs to
 * the PARAM_EXEC Params it generates.
 *
 * Outer references are managed via root->plan_params, which is a list of
 * PlannerParamItems.  While planning a subquery, each parent query level's
 * plan_params contains the values required from it by the current subquery.
 * During create_plan(), we use plan_params to track values that must be
 * passed from outer to inner sides of NestLoop plan nodes.
 *
 * The item a PlannerParamItem represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of this level that must be passed
 * down because subqueries have outer references to it, or must be passed
 * from a NestLoop node to its inner scan.  The varlevelsup value in the Var
 * will always be zero.
 *
 * A PlaceHolderVar: this works much like the Var case, except that the
 * entry is a PlaceHolderVar node with a contained expression.  The PHV
 * will have phlevelsup = 0, and the contained expression is adjusted
 * to match in level.
 *
 * An Aggref (with an expression tree representing its argument): the slot
 * represents an aggregate expression that is an outer reference for some
 * subquery.  The Aggref itself has agglevelsup = 0, and its argument tree
 * is adjusted to match in level.
 *
 * Note: we detect duplicate Var and PlaceHolderVar parameters and coalesce
 * them into one slot, but we do not bother to do that for Aggrefs.
 * The scope of duplicate-elimination only extends across the set of
 * parameters passed from one query level into a single subquery, or for
 * nestloop parameters across the set of nestloop parameters used in a single
 * query level.  So there is no possibility of a PARAM_EXEC slot being used
 * for conflicting purposes.
 *
 * In addition, PARAM_EXEC slots are assigned for Params representing outputs
 * from subplans (values that are setParam items for those subplans).  These
 * IDs need not be tracked via PlannerParamItems, since we do not need any
 * duplicate-elimination nor later processing of the represented expressions.
 * Instead, we just record the assignment of the slot number by appending to
 * root->glob->paramExecTypes.
 */
typedef struct PlannerParamItem
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Node	   *item;			/* 该 Var、PHV 或 Aggref */
	int			paramId;		/* 其分配的 PARAM_EXEC 槽号 */
} PlannerParamItem;

/*
 * When making cost estimates for a SEMI/ANTI/inner_unique join, there are
 * some correction factors that are needed in both nestloop and hash joins
 * to account for the fact that the executor can stop scanning inner rows
 * as soon as it finds a match to the current outer row.  These numbers
 * depend only on the selected outer and inner join relations, not on the
 * particular paths used for them, so it's worthwhile to calculate them
 * just once per relation pair not once per considered path.  This struct
 * is filled by compute_semi_anti_join_factors and must be passed along
 * to the join cost estimation functions.
 *
 * outer_match_frac is the fraction of the outer tuples that are
 *		expected to have at least one match.
 * match_count is the average number of matches expected for
 *		outer tuples that have at least one match.
 */
typedef struct SemiAntiJoinFactors
{
	Selectivity outer_match_frac;
	Selectivity match_count;
} SemiAntiJoinFactors;

/*
 * 传给 add_paths_to_joinrel 子例程的额外信息结构
 *
 * restrictlist 含适用于本连接的全部
 *		限制子句 RestrictInfo
 * mergeclause_list 是本连接可用归并连接
 *		子句的 RestrictInfo 列表
 * inner_unique 为真表示每个外表元组可证明
 *		至多一内元组
 * sjinfo 是特殊连接的额外信息，用于选择率估计
 * semifactors 如上（仅 SEMI/ANTI/inner_unique 连接有效）
 * param_source_rels 是结果路径参数化的合法目标
 */
typedef struct JoinPathExtraData
{
	List	   *restrictlist;
	List	   *mergeclause_list;
	bool		inner_unique;
	SpecialJoinInfo *sjinfo;
	SemiAntiJoinFactors semifactors;
	Relids		param_source_rels;
} JoinPathExtraData;

/*
 * 指示可能有哪些分组方式的各种标志。
 *
 * 若可能做基于排序的分组实现，应置
 * GROUPING_CAN_USE_SORT。使用分组集时，只要
 * 任一分组集可能用排序（即便并非全部），
 * 即置为真。
 *
 * 若可能做基于哈希的分组实现，应置
 * GROUPING_CAN_USE_HASH。
 *
 * 若聚集类型支持部分聚集（例如分组集不支持），
 * 应置 GROUPING_CAN_PARTIAL_AGG。
 * 不涉及并行安全或是否已有合适路径。
 */
#define GROUPING_CAN_USE_SORT       0x0001
#define GROUPING_CAN_USE_HASH       0x0002
#define GROUPING_CAN_PARTIAL_AGG	0x0004

/*
 * 正在使用何种分区级聚集？
 *
 * PARTITIONWISE_AGGREGATE_NONE: 未用。
 *
 * PARTITIONWISE_AGGREGATE_FULL：各分区分别聚集，
 * 再追加结果。
 *
 * PARTITIONWISE_AGGREGATE_PARTIAL：各分区分别部分
 * 聚集，追加结果后再做最终聚集。
 */
typedef enum
{
	PARTITIONWISE_AGGREGATE_NONE,
	PARTITIONWISE_AGGREGATE_FULL,
	PARTITIONWISE_AGGREGATE_PARTIAL
} PartitionwiseAggregateType;

/*
 * 传给 create_grouping_paths 子例程的额外信息结构
 *
 * 指示可能有哪些分组方式的标志。
 * partial_costs_set：agg_partial/final_costs 已初始化，
 * 		则为真。
 * agg_partial_costs 给出部分聚集代价。
 * agg_final_costs 给出最终化代价。
 * target 并行安全则 target_parallel_safe 真。
 * havingQual 是聚集后施加的限定列表。
 * targetList 是要投影的列列表。
 * patype 是正在执行的分区级聚集类型。
 */
typedef struct
{
	/* 一经设置即保持常量数据。 */
	int			flags;
	bool		partial_costs_set;
	AggClauseCosts agg_partial_costs;
	AggClauseCosts agg_final_costs;

	/* 跨分区可能不同的数据。 */
	bool		target_parallel_safe;
	Node	   *havingQual;
	List	   *targetList;
	PartitionwiseAggregateType patype;
} GroupPathExtraData;

/*
 * 传给 grouping_planner 子例程的额外信息结构
 *
 * 若确需 Limit 计划节点则 limit_needed 为真。
 * limit_tuples 是输出元组数的估计上界，
 *		无 LIMIT 或无法估计时为 -1。
 * count_est 与 offset_est 是 preprocess_limit()
 * 		算出的 LIMIT/OFFSET 表达式估计值（详见
 * 		preprocess_limit() 注释）。
 */
typedef struct
{
	bool		limit_needed;
	Cardinality limit_tuples;
	int64		count_est;
	int64		offset_est;
} FinalPathExtraData;

/*
 * 为求速度，连接路径代价估计分两阶段：
 * 第一阶段快速推出连接代价下界，再检查
 * 是否足以拒绝该路径。若不能，再回来做
 * 更精细估计。第一阶段把初步代价估计
 * 及可能的中间值填入 JoinCostWorkspace；
 * 第二阶段取这些值为输入以免重复劳动。
 * （第二阶段复用中间结果。）
 *
 * （理想应声明在 cost.h，但 pathnode.h 也需要，
 * 故放此处似更妥。）
 */
typedef struct JoinCostWorkspace
{
	/* 初步代价估计——不得大于最终值! */
	Cost		startup_cost;	/* 取任何元组前已花的代价 */
	Cost		total_cost;		/* 总代价(假定取完全部元组) */

	/* 以下字段应视为 costsize.c 私有 */
	Cost		run_cost;		/* 非启动代价分量 */

	/* cost_nestloop 代码私有 */
	Cost		inner_run_cost; /* 也被 cost_mergejoin 使用 */
	Cost		inner_rescan_run_cost;

	/* cost_mergejoin 代码私有 */
	Cardinality outer_rows;
	Cardinality inner_rows;
	Cardinality outer_skip_rows;
	Cardinality inner_skip_rows;

	/* cost_hashjoin 代码私有 */
	int			numbuckets;
	int			numbatches;
	Cardinality inner_rows_total;
} JoinCostWorkspace;

/*
 * AggInfo 保存需要计算的一个聚集的信息。
 * 查询中多个 Aggref 可通过相同 'aggno'
 * 引用同一 AggInfo，使该聚集只算一次。
 */
typedef struct AggInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/*
	 * 本状态值所对应的 Aggref 表达式列表。
	 *
	 * 至少有一个；也可有多个相同 Aggref
	 * 共享同一 per-agg。
	 */
	List	   *aggrefs;

	/* 本聚合的转移状态号 */
	int			transno;

	/*
	 * 若该聚集因最终函数可读写而不能与其它
	 * 聚集共享状态值，则 shareable 为假。
	 */
	bool		shareable;

	/* 最终函数的 Oid,无则为 InvalidOid */
	Oid			finalfn_oid;
} AggInfo;

/*
 * AggTransInfo 保存查询中一个或多个聚集所用
 * 转移状态的信息。若输入与转移函数相同，
 * 多个聚集可共享同一转移状态。共享同一
 * 转移信息的 Aggref 具有相同
 * 'aggtransno'。
 */
typedef struct AggTransInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* 本转移状态的输入 */
	List	   *args;
	Expr	   *aggfilter;

	/* 状态转移函数的 Oid */
	Oid			transfn_oid;

	/* 序列化函数的 Oid,无则为 InvalidOid */
	Oid			serialfn_oid;

	/* 反序列化函数的 Oid,无则为 InvalidOid */
	Oid			deserialfn_oid;

	/* 合并函数的 Oid,无则为 InvalidOid */
	Oid			combinefn_oid;

	/* 状态值数据类型的 Oid */
	Oid			aggtranstype;

	/* 关于 transtype 额外数据 */
	int32		aggtranstypmod;
	int			transtypeLen;
	bool		transtypeByVal;

	/* 空间消耗估计 */
	int32		aggtransspace;

	/* 来自 pg_aggregate 项的初值 */
	Datum		initValue pg_node_attr(read_write_ignore);
	bool		initValueIsNull;
} AggTransInfo;

#endif							/* PATHNODES_H */
