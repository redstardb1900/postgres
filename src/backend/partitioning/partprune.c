/*-------------------------------------------------------------------------
 *
 * partprune.c
 *		查询规划与执行期间的分区剪枝支持
 *
 * 本模块利用表的分区描述符、查询子句以及运行期参数里
 * 所含的信息来实现分区剪枝。
 *
 * 规划期间，能与表的分区键匹配的子句会被转成一组"剪枝步骤"，
 * 然后执行这些步骤，以找出满足该步骤中约束的一组分区
 * （以 RelOptInfo->part_rels 数组里的下标表示）。
 * 不在该集合中的分区
 * 称为已被剪枝。
 *
 * 基本剪枝步骤可能涉及只在执行期间才知道取值的表达式，
 * 例如 Params。这时剪枝无法
 * 完全在规划期间完成。这种情况下，这些步骤会随
 * 计划一起保留，以便执行器用来做进一步剪枝。
 *
 * 剪枝步骤有两种。基本（base）剪枝步骤表示
 * 对分区键列的检验，通常是与表达式做比较。
 * 组合（combine）剪枝步骤表示布尔连接符（AND/OR），并用
 * 适当的组合方法，把此前若干步骤的输出
 * 组合起来。
 *
 * 步骤生成的更多细节见 gen_partprune_steps_internal()。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partprune.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/hash.h"
#include "access/nbtree.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/lsyscache.h"


/*
 * 与分区键匹配的子句的信息。
 */
typedef struct PartClauseInfo
{
	int			keyno;			/* 分区键号（0 到 partnatts - 1） */
	Oid			opno;			/* 用来比较分区键与 expr 的操作符 */
	bool		op_is_ne;		/* 子句原来的操作符是 <> 吗？ */
	Expr	   *expr;			/* 与分区键相比较的 expr */
	Oid			cmpfn;			/* 用来把 expr 与
								 * 分区键相比较的函数的 Oid */
	int			op_strategy;	/* 标识该操作符的 btree 策略 */
} PartClauseInfo;

/*
 * PartClauseMatchStatus
 *		描述 match_clause_to_partition_key() 的结果
 */
typedef enum PartClauseMatchStatus
{
	PARTCLAUSE_NOMATCH,
	PARTCLAUSE_MATCH_CLAUSE,
	PARTCLAUSE_MATCH_NULLNESS,
	PARTCLAUSE_MATCH_STEPS,
	PARTCLAUSE_MATCH_CONTRADICT,
	PARTCLAUSE_UNSUPPORTED
} PartClauseMatchStatus;

/*
 * PartClauseTarget
 *		标识哪些限定子句可以用来生成剪枝步骤
 */
typedef enum PartClauseTarget
{
	PARTTARGET_PLANNER,			/* 希望在规划期间做剪枝 */
	PARTTARGET_INITIAL,			/* 希望在执行器启动期间做剪枝 */
	PARTTARGET_EXEC				/* 希望在每个计划节点扫描期间做剪枝 */
} PartClauseTarget;

/*
 * GeneratePruningStepsContext
 *		为一组子句生成剪枝步骤时
 *		当前状态的信息。
 *
 * gen_partprune_steps() 初始化并返回该结构的一个实例。
 *
 * 注意：若我们发现任何可能对剪枝有用、且具有这些性质的子句，
 * 就会设置 has_mutable_op、has_mutable_arg 和 has_exec_param，
 * 不论是否真的在步骤列表里用了该子句。这个
 * 定义让我们在某些情况下可以跳过 PARTTARGET_EXEC 趟。
 */
typedef struct GeneratePruningStepsContext
{
	/* gen_partprune_steps 输入参数的副本： */
	RelOptInfo *rel;			/* 已分区关系 */
	PartClauseTarget target;	/* 正在为其生成步骤的使用场景 */
	/* 结果数据： */
	List	   *steps;			/* PartitionPruneSteps 的列表 */
	bool		has_mutable_op; /* 子句含有任何稳定操作符 */
	bool		has_mutable_arg;	/* 子句含有任何可变的比较
									 * 值，但执行期参数除外 */
	bool		has_exec_param; /* 子句含有任何 PARAM_EXEC 参数 */
	bool		contradictory;	/* 子句已被证明自相矛盾 */
	/* 工作状态： */
	int			next_step_id;
} GeneratePruningStepsContext;

/* 执行一个 PartitionPruneStep 的结果 */
typedef struct PruneStepResult
{
	/*
	 * 边界的偏移（在表的 boundinfo 里），其分区被
	 * 该剪枝步骤选中。
	 */
	Bitmapset  *bound_offsets;

	bool		scan_default;	/* 扫描默认分区吗？ */
	bool		scan_null;		/* 扫描 NULL 值的分区吗？ */
} PruneStepResult;


static List *add_part_relids(List *allpartrelids, Bitmapset *partrelids);
static List *make_partitionedrel_pruneinfo(PlannerInfo *root,
										   RelOptInfo *parentrel,
										   List *prunequal,
										   Bitmapset *partrelids,
										   int *relid_subplan_map,
										   Bitmapset **matchedsubplans);
static void gen_partprune_steps(RelOptInfo *rel, List *clauses,
								PartClauseTarget target,
								GeneratePruningStepsContext *context);
static List *gen_partprune_steps_internal(GeneratePruningStepsContext *context,
										  List *clauses);
static PartitionPruneStep *gen_prune_step_op(GeneratePruningStepsContext *context,
											 StrategyNumber opstrategy, bool op_is_ne,
											 List *exprs, List *cmpfns, Bitmapset *nullkeys);
static PartitionPruneStep *gen_prune_step_combine(GeneratePruningStepsContext *context,
												  List *source_stepids,
												  PartitionPruneCombineOp combineOp);
static List *gen_prune_steps_from_opexps(GeneratePruningStepsContext *context,
										 List **keyclauses, Bitmapset *nullkeys);
static PartClauseMatchStatus match_clause_to_partition_key(GeneratePruningStepsContext *context,
														   Expr *clause, Expr *partkey, int partkeyidx,
														   bool *clause_is_not_null,
														   PartClauseInfo **pc, List **clause_steps);
static List *get_steps_using_prefix(GeneratePruningStepsContext *context,
									StrategyNumber step_opstrategy,
									bool step_op_is_ne,
									Expr *step_lastexpr,
									Oid step_lastcmpfn,
									Bitmapset *step_nullkeys,
									List *prefix);
static List *get_steps_using_prefix_recurse(GeneratePruningStepsContext *context,
											StrategyNumber step_opstrategy,
											bool step_op_is_ne,
											Expr *step_lastexpr,
											Oid step_lastcmpfn,
											Bitmapset *step_nullkeys,
											List *prefix,
											ListCell *start,
											List *step_exprs,
											List *step_cmpfns);
static PruneStepResult *get_matching_hash_bounds(PartitionPruneContext *context,
												 StrategyNumber opstrategy, Datum *values, int nvalues,
												 FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static PruneStepResult *get_matching_list_bounds(PartitionPruneContext *context,
												 StrategyNumber opstrategy, Datum value, int nvalues,
												 FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static PruneStepResult *get_matching_range_bounds(PartitionPruneContext *context,
												  StrategyNumber opstrategy, Datum *values, int nvalues,
												  FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static Bitmapset *pull_exec_paramids(Expr *expr);
static bool pull_exec_paramids_walker(Node *node, Bitmapset **context);
static Bitmapset *get_partkey_exec_paramids(List *steps);
static PruneStepResult *perform_pruning_base_step(PartitionPruneContext *context,
												  PartitionPruneStepOp *opstep);
static PruneStepResult *perform_pruning_combine_step(PartitionPruneContext *context,
													 PartitionPruneStepCombine *cstep,
													 PruneStepResult **step_results);
static PartClauseMatchStatus match_boolean_partition_clause(Oid partopfamily,
															Expr *clause,
															Expr *partkey,
															Expr **outconst,
															bool *noteq);
static void partkey_datum_from_expr(PartitionPruneContext *context,
									Expr *expr, int stateidx,
									Datum *value, bool *isnull);


/*
 * make_partition_pruneinfo
 *		构造 PartitionPruneInfo，供执行器用来允许
 *		进行额外的分区剪枝。当分区剪枝
 *		没有用处时返回 NULL。
 *
 * parentrel 是某个追加关系的 RelOptInfo，subpaths 是其孩子关系的
 * 扫描路径列表。
 * prunequal 是可能用于剪枝的限定条件列表（即适用于该
 * 追加关系的限制子句）。
 */
PartitionPruneInfo *
make_partition_pruneinfo(PlannerInfo *root, RelOptInfo *parentrel,
						 List *subpaths,
						 List *prunequal)
{
	PartitionPruneInfo *pruneinfo;
	Bitmapset  *allmatchedsubplans = NULL;
	List	   *allpartrelids;
	List	   *prunerelinfos;
	int		   *relid_subplan_map;
	ListCell   *lc;
	int			i;

	/*
	 * 扫描子路径，看哪些是分区孩子
	 * 关系的扫描，并识别它们的父已分区关系。（注意：必须
	 * 把父已分区关系限制为 parentrel 或其孩子，
	 * 否则就没法把 prunequal 翻译成匹配的形式。）
	 *
	 * 另外构造一个临时数组，把分区孩子关系的
	 * relid 映射到该分区扫描计划在 subpaths 里的下标。
	 * （这里用 subplan 而不是 subpath 有点用词不当，但
	 * 我们就沿用它。）为方便起见，这里用从 1 起计的下标，这样
	 * 零可以表示尚未填充的数组项。
	 */
	allpartrelids = NIL;
	relid_subplan_map = palloc0(sizeof(int) * root->simple_rel_array_size);

	i = 1;
	foreach(lc, subpaths)
	{
		Path	   *path = (Path *) lfirst(lc);
		RelOptInfo *pathrel = path->parent;

		/* 这里不考虑已分区连接 */
		if (pathrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		{
			RelOptInfo *prel = pathrel;
			Bitmapset  *partrelids = NULL;

			/*
			 * 向上走到 pathrel 最顶层的已分区父关系，
			 * 沿途收集父关系的 relid；但若到达
			 * parentrel 就停止。（通常，pathrel 最顶层的已分区父关系
			 * 要么是 parentrel，要么是 parentrel 的一个 UNION ALL 追加关系孩子。
			 * 但在处理多层分区树的分区连接时，
			 * 我们可能看到一条追加路径，其
			 * parentrel 是中间的已分区表。）
			 */
			do
			{
				AppendRelInfo *appinfo;

				Assert(prel->relid < root->simple_rel_array_size);
				appinfo = root->append_rel_array[prel->relid];
				prel = find_base_rel(root, appinfo->parent_relid);
				if (!IS_PARTITIONED_REL(prel))
					break;		/* 到达了非分区的父关系 */
				/* 接受这一层作为值得关注的父关系 */
				partrelids = bms_add_member(partrelids, prel->relid);
				if (prel == parentrel)
					break;		/* 不要遍历到 parentrel 之上 */
			} while (prel->reloptkind == RELOPT_OTHER_MEMBER_REL);

			if (partrelids)
			{
				/*
				 * 找到了一些相关的父分区，它们可能与已经找到的
				 * 分区树重叠，也可能不重叠。把新
				 * 信息加入 allpartrelids 列表。
				 */
				allpartrelids = add_part_relids(allpartrelids, partrelids);
				/* 同时把该子计划记入 relid_subplan_map[] */
				/* 请不要重复 */
				Assert(relid_subplan_map[pathrel->relid] == 0);
				relid_subplan_map[pathrel->relid] = i;
			}
		}
		i++;
	}

	/*
	 * 现在为每个最顶层的已分区关系构造一个 PartitionedRelPruneInfo
	 * （略去那些结果没有有用剪枝限定条件的）。
	 */
	prunerelinfos = NIL;
	foreach(lc, allpartrelids)
	{
		Bitmapset  *partrelids = (Bitmapset *) lfirst(lc);
		List	   *pinfolist;
		Bitmapset  *matchedsubplans = NULL;

		pinfolist = make_partitionedrel_pruneinfo(root, parentrel,
												  prunequal,
												  partrelids,
												  relid_subplan_map,
												  &matchedsubplans);

		/* 在可以剪枝时，记录匹配到的子计划 */
		if (pinfolist != NIL)
		{
			prunerelinfos = lappend(prunerelinfos, pinfolist);
			allmatchedsubplans = bms_join(matchedsubplans,
										  allmatchedsubplans);
		}
	}

	pfree(relid_subplan_map);

	/*
	 * 若没有哪个分区层次有任何有用的运行期剪枝
	 * 限定条件，就可以不必费心做运行期剪枝。
	 */
	if (prunerelinfos == NIL)
		return NULL;

	/* 否则构造结果数据结构 */
	pruneinfo = makeNode(PartitionPruneInfo);
	pruneinfo->prune_infos = prunerelinfos;

	/*
	 * 有些子计划可能不属于任何已识别的已分区关系。
	 * 这可能发生在 UNION ALL 查询包含非分区
	 * 表时，或某些层次无法做运行期剪枝时。构造
	 * 所有这些子计划的下标 bitmapset，以便执行器
	 * 能识别哪些子计划永远不应当被剪枝。
	 */
	if (bms_num_members(allmatchedsubplans) < list_length(subpaths))
	{
		Bitmapset  *other_subplans;

		/* 构造 allmatchedsubplans 的补集 */
		other_subplans = bms_add_range(NULL, 0, list_length(subpaths) - 1);
		other_subplans = bms_del_members(other_subplans, allmatchedsubplans);

		pruneinfo->other_subplans = other_subplans;
	}
	else
		pruneinfo->other_subplans = NULL;

	return pruneinfo;
}

/*
 * add_part_relids
 *		把新信息加入已分区关系 relid 的 Bitmapset 列表。
 *
 * 在 allpartrelids 里，每个最顶层的父
 * 已分区关系有一个 Bitmapset。每个 Bitmapset 含最顶层
 * 父关系的 RT 下标，以及其相关的非叶孩子分区。由于（按
 * 范围表列表的构造）父分区的 RT 下标必须低于
 * 其子分区，我们可以把最顶层父关系
 * 辨认为 Bitmapset 里最低的那个置位。
 *
 * partrelids 含一个父已分区关系的 RT 下标，以及
 * 可能的一些非叶孩子，它们刚被识别为传给
 * make_partition_pruneinfo() 的某个子路径关系的父关系。这些被加入
 * allpartrelids 的某个合适成员。
 *
 * 注意，该列表只含这样的已分区表的 RT 下标：它们
 * 是 make_partition_pruneinfo() 正在处理的 subpaths 里
 * 出现的某个扫描层关系的父关系。另外，最顶层父关系
 * 不允许高于与该追加路径相关联的 parentrel。
 * 这样，我们就避免在对当前问题没法贡献有用剪枝信息的
 * 已分区关系上耗费周期。
 * （parentrel 可能是孩子已分区表，而且
 * 扫描层关系也可能是孩子已分区表，
 * 而不是叶分区。因此我们必须参照
 * 正在处理的特定追加路径来构造这个关系集合，而不是
 * 去看 RelOptInfos 里表示的
 * 完整分区结构。）
 */
static List *
add_part_relids(List *allpartrelids, Bitmapset *partrelids)
{
	Index		targetpart;
	ListCell   *lc;

	/* 用这种方式很容易取得最低的置位： */
	targetpart = bms_next_member(partrelids, -1);
	Assert(targetpart > 0);

	/* 寻找匹配的最顶层父关系 */
	foreach(lc, allpartrelids)
	{
		Bitmapset  *currpartrelids = (Bitmapset *) lfirst(lc);
		Index		currtarget = bms_next_member(currpartrelids, -1);

		if (targetpart == currtarget)
		{
			/* 找到了匹配，就把任何新的 RT 下标加入该层次 */
			currpartrelids = bms_add_members(currpartrelids, partrelids);
			lfirst(lc) = currpartrelids;
			return allpartrelids;
		}
	}
	/* 没有匹配，就把新的分区层次加入列表 */
	return lappend(allpartrelids, partrelids);
}

/*
 * make_partitionedrel_pruneinfo
 *		构造 PartitionedRelPruneInfos 的 List，分区层次里每个值得关注的
 *		已分区关系各一个。执行器可以用它们来允许
 *		进行额外的分区剪枝。
 *
 * parentrel：与正在考虑的追加路径相关联的关系
 * prunequal：可能用于剪枝的限定条件，按 parentrel 表示
 * partrelids：标识相关已分区表的 RT 下标集合，
 *   这些表位于单个分区层次之内
 * relid_subplan_map[]：把孩子关系的 relid 映射到子计划下标
 * matchedsubplans：成功时接收与该
 *   分区层次相匹配的子计划下标集合。
 *
 * 若无法找到任何有用的运行期剪枝步骤，就返回 NIL。
 * 不过，成功时 partrelids 里标识的每个关系都会在
 * 结果列表里有一个元素，即使其中一些没有用处。
 */
static List *
make_partitionedrel_pruneinfo(PlannerInfo *root, RelOptInfo *parentrel,
							  List *prunequal,
							  Bitmapset *partrelids,
							  int *relid_subplan_map,
							  Bitmapset **matchedsubplans)
{
	RelOptInfo *targetpart = NULL;
	List	   *pinfolist = NIL;
	bool		doruntimeprune = false;
	int		   *relid_subpart_map;
	Bitmapset  *subplansfound = NULL;
	ListCell   *lc;
	int			rti;
	int			i;

	/*
	 * 逐个检查已分区关系，构造一个临时数组，把
	 * 规划器 relid 映射到该已分区关系的下标，并为每个已分区关系
	 * 构造一个 PartitionedRelPruneInfo。
	 *
	 * 在这一阶段我们发现是否需要运行期剪枝；若不需要，
	 * 就可以避免做进一步的工作。
	 */
	relid_subpart_map = palloc0(sizeof(int) * root->simple_rel_array_size);

	i = 1;
	rti = -1;
	while ((rti = bms_next_member(partrelids, rti)) > 0)
	{
		RelOptInfo *subpart = find_base_rel(root, rti);
		PartitionedRelPruneInfo *pinfo;
		List	   *partprunequal;
		List	   *initial_pruning_steps;
		List	   *exec_pruning_steps;
		Bitmapset  *execparamids;
		GeneratePruningStepsContext context;

		/*
		 * 填充映射数组。
		 *
		 * relid_subpart_map 把非叶分区的 relid 映射到
		 * 返回的 PartitionedRelPruneInfo 列表里该分区
		 * 信息的下标。这里用从 1 起计的下标，这样零可以表示
		 * 尚未填充的数组项。
		 */
		Assert(rti < root->simple_rel_array_size);
		relid_subpart_map[rti] = i++;

		/*
		 * 必要时为该分区翻译剪枝限定条件。
		 *
		 * 列表的第一项是目标已分区关系。
		 */
		if (!targetpart)
		{
			targetpart = subpart;

			/*
			 * prunequal 是作为 parentrel 的限定条件交给我们的。
			 * 这个关系常常与 targetpart 相同，因此可以跳过
			 * adjust_appendrel_attrs 这一步。但它也可能不同，那时
			 * 就必须翻译。这里更新 prunequal 参数，
			 * 因为在稍后针对孩子分区的循环迭代里，
			 * 我们要从父变量翻译到孩子变量。
			 */
			if (!bms_equal(parentrel->relids, subpart->relids))
			{
				int			nappinfos;
				AppendRelInfo **appinfos = find_appinfos_by_relids(root,
																   subpart->relids,
																   &nappinfos);

				prunequal = (List *) adjust_appendrel_attrs(root, (Node *)
															prunequal,
															nappinfos,
															appinfos);

				pfree(appinfos);
			}

			partprunequal = prunequal;
		}
		else
		{
			/*
			 * 对子分区表，列的顺序可能与父表
			 * 不同，因此必须翻译 prunequal，使它
			 * 与该关系相容。
			 */
			partprunequal = (List *)
				adjust_appendrel_attrs_multilevel(root,
												  (Node *) prunequal,
												  subpart,
												  targetpart);
		}

		/*
		 * 把剪枝限定条件转换成剪枝步骤。我们可能需要做这
		 * 两次：一次取得执行器启动剪枝步骤，一次取得
		 * 执行器按扫描剪枝步骤。第一趟创建启动
		 * 剪枝步骤，并检测是否有任何可能有用、且
		 * 需要按扫描剪枝的限定条件。
		 */
		gen_partprune_steps(subpart, partprunequal, PARTTARGET_INITIAL,
							&context);

		if (context.contradictory)
		{
			/*
			 * 这不应当发生，因为规划器应当更早检测到
			 * 这一点。不过，我们在这里确实使用来自参数化
			 * 路径的额外限定条件。它们只把参数与分区键
			 * 比较，因此不应当发现任何先前在规划期间
			 * 未发现的新的限定条件矛盾，因为规划期间参数
			 * 值是未知的。无论如何，这里最好做点
			 * 合理的事，所以干脆禁用运行期剪枝。
			 */
			return NIL;
		}

		/*
		 * 若可用的剪枝子句里没有可变操作符或表达式，
		 * 就没有必要做启动剪枝，因为
		 * 规划期剪枝应当已经剪掉了所有可剪的部分。
		 */
		if (context.has_mutable_op || context.has_mutable_arg)
			initial_pruning_steps = context.steps;
		else
			initial_pruning_steps = NIL;

		/*
		 * 若可能有用的剪枝子句里没有执行期参数，
		 * 就没有必要考虑按扫描剪枝。
		 */
		if (context.has_exec_param)
		{
			/* ……好，我们最好考虑一下 */
			gen_partprune_steps(subpart, partprunequal, PARTTARGET_EXEC,
								&context);

			if (context.contradictory)
			{
				/* 同上，若出现异常情况，就跳过运行期剪枝 */
				return NIL;
			}

			exec_pruning_steps = context.steps;

			/*
			 * 检测实际用到了哪些执行期参数；某些参数出现在
			 * 可用子句里并不意味着我们真的用了它们。
			 * 若一个都没有，就跳过按扫描剪枝。
			 */
			execparamids = get_partkey_exec_paramids(exec_pruning_steps);

			if (bms_is_empty(execparamids))
				exec_pruning_steps = NIL;
		}
		else
		{
			/* 任何地方都没有执行期参数，因此忘掉扫描期剪枝 */
			exec_pruning_steps = NIL;
			execparamids = NULL;
		}

		if (initial_pruning_steps || exec_pruning_steps)
			doruntimeprune = true;

		/* 开始为该关系构造 PartitionedRelPruneInfo */
		pinfo = makeNode(PartitionedRelPruneInfo);
		pinfo->rtindex = rti;
		pinfo->initial_pruning_steps = initial_pruning_steps;
		pinfo->exec_pruning_steps = exec_pruning_steps;
		pinfo->execparamids = execparamids;
		/* 其余字段在下一轮循环里填充 */

		pinfolist = lappend(pinfolist, pinfo);
	}

	if (!doruntimeprune)
	{
		/* 不需要运行期剪枝。 */
		pfree(relid_subpart_map);
		return NIL;
	}

	/*
	 * 将需要运行期剪枝，因此初始化其他信息。
	 * 其中包括两张映射：一张用来把叶分区的分区下标
	 * 转换成它们在子计划列表里的子计划下标，
	 * 另一张用来把子分区的分区下标
	 * 转换成它们的 PartitionedRelPruneInfo 在
	 * PartitionedRelPruneInfo 列表里的下标。
	 */
	foreach(lc, pinfolist)
	{
		PartitionedRelPruneInfo *pinfo = lfirst(lc);
		RelOptInfo *subpart = find_base_rel(root, pinfo->rtindex);
		Bitmapset  *present_parts;
		int			nparts = subpart->nparts;
		int		   *subplan_map;
		int		   *subpart_map;
		Oid		   *relid_map;

		/*
		 * 为这一分区层构造子计划和子分区映射。
		 * 这里转换成从 0 起计的下标，空项用 -1。
		 * 同时构造当前存在（也就是
		 * 尚未被剪掉）的全部分区的 Bitmapset。
		 */
		subplan_map = (int *) palloc(nparts * sizeof(int));
		memset(subplan_map, -1, nparts * sizeof(int));
		subpart_map = (int *) palloc(nparts * sizeof(int));
		memset(subpart_map, -1, nparts * sizeof(int));
		relid_map = (Oid *) palloc0(nparts * sizeof(Oid));
		present_parts = NULL;

		i = -1;
		while ((i = bms_next_member(subpart->live_parts, i)) >= 0)
		{
			RelOptInfo *partrel = subpart->part_rels[i];
			int			subplanidx;
			int			subpartidx;

			Assert(partrel != NULL);

			subplan_map[i] = subplanidx = relid_subplan_map[partrel->relid] - 1;
			subpart_map[i] = subpartidx = relid_subpart_map[partrel->relid] - 1;
			relid_map[i] = planner_rt_fetch(partrel->relid, root)->relid;
			if (subplanidx >= 0)
			{
				present_parts = bms_add_member(present_parts, i);

				/* 记录发现了该子计划  */
				subplansfound = bms_add_member(subplansfound, subplanidx);
			}
			else if (subpartidx >= 0)
				present_parts = bms_add_member(present_parts, i);
		}

		/*
		 * 确保没有为那些我们既没有子路径、
		 * 也没有子 PartitionedRelPruneInfo 的已分区表
		 * 生成多余的 PartitionedRelPruneInfo。
		 */
		Assert(!bms_is_empty(present_parts));

		/* 记录这些映射和其他信息。 */
		pinfo->present_parts = present_parts;
		pinfo->nparts = nparts;
		pinfo->subplan_map = subplan_map;
		pinfo->subpart_map = subpart_map;
		pinfo->relid_map = relid_map;
	}

	pfree(relid_subpart_map);

	*matchedsubplans = subplansfound;

	return pinfolist;
}

/*
 * gen_partprune_steps
 *		处理 clauses（通常是关系的 baserestrictinfo 子句列表），
 *		并创建剪枝步骤列表。
 *
 * target 指明是为规划生成剪枝步骤（只用
 * 不可变子句），还是为执行器启动生成（除含 PARAM_EXEC 参数的
 * 子句外，任何允许的子句都可以用），还是为执行器
 * 按扫描剪枝生成（任何允许的子句都可以用）。
 *
 * context 是输出参数，接收步骤列表以及
 * 一些附属标志；见 GeneratePruningStepsContext 类型定义。
 */
static void
gen_partprune_steps(RelOptInfo *rel, List *clauses, PartClauseTarget target,
					GeneratePruningStepsContext *context)
{
	/* 把所有输出值初始化为零、假或 NULL */
	memset(context, 0, sizeof(GeneratePruningStepsContext));
	context->rel = rel;
	context->target = target;

	/*
	 * 若该已分区表本身又是一个分区，并且与其父表共享任何
	 * 分区键，则层次结构可能允许父表的取值范围比它的某些
	 * 分区更窄（尤其是默认分区）。
	 * 这通常没有用处，但可以用来
	 * 剪掉默认分区。
	 */
	if (partition_bound_has_default(rel->boundinfo) && rel->partition_qual)
	{
		/* 做一份副本，以免修改传入的 List */
		clauses = list_concat_copy(clauses, rel->partition_qual);
	}

	/* 接下来进入递归处理。 */
	(void) gen_partprune_steps_internal(context, clauses);
}

/*
 * prune_append_rel_partitions
 *		处理关系的 baserestrictinfo，并利用能在
 *		查询规划期间求值的限定条件，确定为满足这些限定条件
 *		必须扫描的最小分区集合。以 Bitmapset 形式返回
 *		匹配的分区，其中含这些分区在该关系 part_rels 数组里的
 *		下标。
 *
 * 调用者必须保证 rel 是已分区表。
 */
Bitmapset *
prune_append_rel_partitions(RelOptInfo *rel)
{
	List	   *clauses = rel->baserestrictinfo;
	List	   *pruning_steps;
	GeneratePruningStepsContext gcontext;
	PartitionPruneContext context;

	Assert(rel->part_scheme != NULL);

	/* 若没有分区，就返回空集 */
	if (rel->nparts == 0)
		return NULL;

	/*
	 * 若禁用了剪枝，或没有可用于剪枝的子句，就返回
	 * 全部分区。
	 */
	if (!enable_partition_pruning || clauses == NIL)
		return bms_add_range(NULL, 0, rel->nparts - 1);

	/*
	 * 处理子句，提取在规划期可用的剪枝步骤。
	 * 若发现子句互相矛盾，就可以返回空
	 * 集。
	 */
	gen_partprune_steps(rel, clauses, PARTTARGET_PLANNER,
						&gcontext);
	if (gcontext.contradictory)
		return NULL;
	pruning_steps = gcontext.steps;

	/* 若没有可用的，就返回全部分区 */
	if (pruning_steps == NIL)
		return bms_add_range(NULL, 0, rel->nparts - 1);

	/* 设置 PartitionPruneContext */
	context.strategy = rel->part_scheme->strategy;
	context.partnatts = rel->part_scheme->partnatts;
	context.nparts = rel->nparts;
	context.boundinfo = rel->boundinfo;
	context.partcollation = rel->part_scheme->partcollation;
	context.partsupfunc = rel->part_scheme->partsupfunc;
	context.stepcmpfuncs = (FmgrInfo *) palloc0(sizeof(FmgrInfo) *
												context.partnatts *
												list_length(pruning_steps));
	context.ppccontext = CurrentMemoryContext;

	/* 从规划器调用时这些无效 */
	context.planstate = NULL;
	context.exprcontext = NULL;
	context.exprstates = NULL;

	/* 实际的剪枝在这里发生。 */
	return get_matching_partitions(&context, pruning_steps);
}

/*
 * get_matching_partitions
 *		确定在分区剪枝后仍然保留的分区。
 *
 * 注意：当 pruning_steps 的生成目标不是 PARTTARGET_PLANNER 时，
 * context->exprcontext 必须有效。
 *
 * 返回幸存分区在 RelOptInfo->part_rels 里的
 * 下标 Bitmapset。
 */
Bitmapset *
get_matching_partitions(PartitionPruneContext *context, List *pruning_steps)
{
	Bitmapset  *result;
	int			num_steps = list_length(pruning_steps),
				i;
	PruneStepResult **results,
			   *final_result;
	ListCell   *lc;
	bool		scan_default;

	/* 若没有剪枝步骤，则所有分区都匹配。 */
	if (num_steps == 0)
	{
		Assert(context->nparts > 0);
		return bms_add_range(NULL, 0, context->nparts - 1);
	}

	/*
	 * 为各个剪枝步骤分配存放结果的空间。每个
	 * 槽在执行给定剪枝步骤后将保存一个 PruneStepResult。
	 * 后面的步骤可能使用一个或多个更早步骤的结果。
	 * 应用全部剪枝步骤的结果，就是最后一个剪枝步骤
	 * 那个槽里的值。
	 */
	results = (PruneStepResult **)
		palloc0(num_steps * sizeof(PruneStepResult *));
	foreach(lc, pruning_steps)
	{
		PartitionPruneStep *step = lfirst(lc);

		switch (nodeTag(step))
		{
			case T_PartitionPruneStepOp:
				results[step->step_id] =
					perform_pruning_base_step(context,
											  (PartitionPruneStepOp *) step);
				break;

			case T_PartitionPruneStepCombine:
				results[step->step_id] =
					perform_pruning_combine_step(context,
												 (PartitionPruneStepCombine *) step,
												 results);
				break;

			default:
				elog(ERROR, "invalid pruning step type: %d",
					 (int) nodeTag(step));
		}
	}

	/*
	 * 至此我们知道所有 datum 的偏移，其对应
	 * 分区需要出现在结果里，包括特殊的接受 NULL 的
	 * 分区和默认分区。现在收集实际的分区下标。
	 */
	final_result = results[num_steps - 1];
	Assert(final_result != NULL);
	i = -1;
	result = NULL;
	scan_default = final_result->scan_default;
	while ((i = bms_next_member(final_result->bound_offsets, i)) >= 0)
	{
		int			partindex;

		Assert(i < context->boundinfo->nindexes);
		partindex = context->boundinfo->indexes[i];

		if (partindex < 0)
		{
			/*
			 * 在区间分区情形下，若分区下标为 -1，
			 * 就表示该偏移处的边界是某个区间的上界，而该区间
			 * 不被任何分区覆盖（可能存在的
			 * 默认分区除外）。在哈希分区中，这同样表示
			 * 没有为对应的余数
			 * 定义分区。
			 *
			 * 无论哪种情形，该值仍然属于被查询的值
			 * 区间，因此若存在默认分区，就标记为扫描它。
			 */
			scan_default |= partition_bound_has_default(context->boundinfo);
			continue;
		}

		result = bms_add_member(result, partindex);
	}

	/* 若需要且存在，就把 NULL 分区和/或默认分区加进去。 */
	if (final_result->scan_null)
	{
		Assert(context->strategy == PARTITION_STRATEGY_LIST);
		Assert(partition_bound_accepts_nulls(context->boundinfo));
		result = bms_add_member(result, context->boundinfo->null_index);
	}
	if (scan_default)
	{
		Assert(context->strategy == PARTITION_STRATEGY_LIST ||
			   context->strategy == PARTITION_STRATEGY_RANGE);
		Assert(partition_bound_has_default(context->boundinfo));
		result = bms_add_member(result, context->boundinfo->default_index);
	}

	return result;
}

/*
 * gen_partprune_steps_internal
 *		处理 clauses 以生成分区剪枝步骤的 List。若没有生成
 *		任何步骤，就返回 NIL。
 *
 * 这些分区剪枝步骤有两种形式：操作符步骤和组合
 * 步骤。
 *
 * 操作符步骤（PartitionPruneStepOp）含我们判定可以用于
 * 分区剪枝的子句细节。其中含正在与分区键比较的
 * 表达式以及
 * 比较函数的细节。
 *
 * 组合步骤（PartitionPruneStepCombine）指示分区剪枝
 * 代码如何从多个输入
 * 操作符步骤和其他组合步骤产生单一分区集合。PARTPRUNE_COMBINE_INTERSECT 类型的
 * 组合步骤会合并其输入步骤，产生的结果只
 * 含所有输入操作符
 * 步骤里都出现的分区。PARTPRUNE_COMBINE_UNION 组合步骤产生的结果
 * 含每个输入操作符步骤的全部分区。
 *
 * 对 BoolExpr 子句，每个参数都递归处理。处理 OR BoolExpr
 * 所生成的步骤用
 * PARTPRUNE_COMBINE_UNION 组合。AND 的 BoolExprs 用
 * PARTPRUNE_COMBINE_INTERSECT 组合。
 *
 * 否则，我们假定收到的子句列表彼此 ANDed。
 * 我们基于这些子句生成能生成的全部剪枝步骤，然后
 * 在末尾，若步骤多于 1 个，就用
 * PARTPRUNE_COMBINE_INTERSECT 组合步骤把每个步骤组合起来。单个步骤则原样返回。
 *
 * 若我们发现彼此矛盾的子句，或与
 * 分区约束矛盾的子句，或含有
 * false 的伪常量子句，就把 context->contradictory 设为真并返回 NIL
 * （也就是没有剪枝步骤）。那种情况下，调用者应认为
 * 所有分区都已被剪枝。
 */
static List *
gen_partprune_steps_internal(GeneratePruningStepsContext *context,
							 List *clauses)
{
	PartitionScheme part_scheme = context->rel->part_scheme;
	List	   *keyclauses[PARTITION_MAX_KEYS];
	Bitmapset  *nullkeys = NULL,
			   *notnullkeys = NULL;
	bool		generate_opsteps = false;
	List	   *result = NIL;
	ListCell   *lc;

	/*
	 * 若该已分区关系有默认分区，并且它自身也是
	 * 分区（partition_qual 不是 NIL 即表明这一点），我们先
	 * 检查子句是否与分区约束矛盾。若矛盾，
	 * 就不必生成任何步骤，因为已经可以证明没有
	 * 分区需要被扫描。
	 *
	 * 这只是不得已的最后手段，因为默认
	 * 分区无法根据与父分区约束相矛盾的子句所生成的步骤来剪枝；当不存在
	 * 默认分区时，更便宜的常规剪枝
	 * 就足够了。
	 */
	if (partition_bound_has_default(context->rel->boundinfo) &&
		predicate_refuted_by(context->rel->partition_qual, clauses, false))
	{
		context->contradictory = true;
		return NIL;
	}

	memset(keyclauses, 0, sizeof(keyclauses));
	foreach(lc, clauses)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		int			i;

		/* 若有 RestrictInfo，就看穿它 */
		if (IsA(clause, RestrictInfo))
			clause = ((RestrictInfo *) clause)->clause;

		/* 常量假或空是矛盾的 */
		if (IsA(clause, Const) &&
			(((Const *) clause)->constisnull ||
			 !DatumGetBool(((Const *) clause)->constvalue)))
		{
			context->contradictory = true;
			return NIL;
		}

		/* 先把 BoolExpr 处理掉。 */
		if (IsA(clause, BoolExpr))
		{
			/*
			 * 为参数生成步骤。
			 *
			 * 参数本身生成的步骤会在递归期间
			 * 加入 context->steps，并会被
			 * 独立求值，这里收集它们的步骤 ID，存入
			 * 我们将创建的组合步骤。
			 */
			if (is_orclause(clause))
			{
				List	   *arg_stepids = NIL;
				bool		all_args_contradictory = true;
				ListCell   *lc1;

				/*
				 * 递归调用可以共用外层的上下文区域，
				 * 但 contradictory 此时最好还不是真。
				 */
				Assert(!context->contradictory);

				/*
				 * 为每个参数取得剪枝步骤。若所有参数都得到矛盾，
				 * 就意味着整个 OR 表达式为假。
				 */
				foreach(lc1, ((BoolExpr *) clause)->args)
				{
					Expr	   *arg = lfirst(lc1);
					bool		arg_contradictory;
					List	   *argsteps;

					argsteps = gen_partprune_steps_internal(context,
															list_make1(arg));
					arg_contradictory = context->contradictory;
					/* 在做完之前保持 context->contradictory 为清除状态 */
					context->contradictory = false;

					if (arg_contradictory)
					{
						/* 直接忽略自相矛盾的参数。 */
						continue;
					}
					else
						all_args_contradictory = false;

					if (argsteps != NIL)
					{
						/*
						 * gen_partprune_steps_internal() 在生成多个步骤时总会加入单个
						 * 组合步骤，因此
						 * 这里只需关注列表里的最后一个。若它只
						 * 生成了一个，则列表里的最后一个
						 * 仍然是我们要的那个。
						 */
						PartitionPruneStep *last = llast(argsteps);

						arg_stepids = lappend_int(arg_stepids, last->step_id);
					}
					else
					{
						PartitionPruneStep *orstep;

						/*
						 * 该参数不含匹配该
						 * 分区键的子句。我们无法用这种参数做剪枝。
						 * 为了向剪枝代码表明这一点，必须
						 * 构造一个哑的 PartitionPruneStepCombine，其
						 * source_stepids 被设为空 List。
						 */
						orstep = gen_prune_step_combine(context, NIL,
														PARTPRUNE_COMBINE_UNION);
						arg_stepids = lappend_int(arg_stepids, orstep->step_id);
					}
				}

				/* 若所有 OR 分支都矛盾，就可以停止 */
				if (all_args_contradictory)
				{
					context->contradictory = true;
					return NIL;
				}

				if (arg_stepids != NIL)
				{
					PartitionPruneStep *step;

					step = gen_prune_step_combine(context, arg_stepids,
												  PARTPRUNE_COMBINE_UNION);
					result = lappend(result, step);
				}
				continue;
			}
			else if (is_andclause(clause))
			{
				List	   *args = ((BoolExpr *) clause)->args;
				List	   *argsteps;

				/*
				 * args 本身可能含任意类型的子句，因此直接
				 * 递归，稍后用一个组合步骤把各成分分区集合
				 * 组合起来。
				 */
				argsteps = gen_partprune_steps_internal(context, args);

				/* 若任何 AND 分支矛盾，就可以立即停止 */
				if (context->contradictory)
					return NIL;

				/*
				 * gen_partprune_steps_internal() 在生成多个步骤时总会加入单个组合
				 * 步骤，因此这里只需
				 * 关注列表里的最后一个。若它只
				 * 生成了一个，则列表里的最后一个仍然是
				 * 我们要的那个。
				 */
				if (argsteps != NIL)
					result = lappend(result, llast(argsteps));

				continue;
			}

			/*
			 * Fall-through for a NOT clause, which if it's a Boolean clause,
			 * will be handled in match_clause_to_partition_key(). We
			 * currently don't perform any pruning for more complex NOT
			 * clauses.
			 */
		}

		/*
		 * 看看能否把该子句匹配到任何一个分区键。
		 */
		for (i = 0; i < part_scheme->partnatts; i++)
		{
			Expr	   *partkey = linitial(context->rel->partexprs[i]);
			bool		clause_is_not_null = false;
			PartClauseInfo *pc = NULL;
			List	   *clause_steps = NIL;

			switch (match_clause_to_partition_key(context,
												  clause, partkey, i,
												  &clause_is_not_null,
												  &pc, &clause_steps))
			{
				case PARTCLAUSE_MATCH_CLAUSE:
					Assert(pc != NULL);

					/*
					 * 由于只允许严格操作符，检查是否有任何
					 * 相矛盾的 IS NULL。
					 */
					if (bms_is_member(i, nullkeys))
					{
						context->contradictory = true;
						return NIL;
					}
					generate_opsteps = true;
					keyclauses[i] = lappend(keyclauses[i], pc);
					break;

				case PARTCLAUSE_MATCH_NULLNESS:
					if (!clause_is_not_null)
					{
						/*
						 * 检查冲突的 IS NOT NULL，以及
						 * 相矛盾的严格子句
						 */
						if (bms_is_member(i, notnullkeys) ||
							keyclauses[i] != NIL)
						{
							context->contradictory = true;
							return NIL;
						}
						nullkeys = bms_add_member(nullkeys, i);
					}
					else
					{
						/* 检查冲突的 IS NULL */
						if (bms_is_member(i, nullkeys))
						{
							context->contradictory = true;
							return NIL;
						}
						notnullkeys = bms_add_member(notnullkeys, i);
					}
					break;

				case PARTCLAUSE_MATCH_STEPS:
					Assert(clause_steps != NIL);
					result = list_concat(result, clause_steps);
					break;

				case PARTCLAUSE_MATCH_CONTRADICT:
					/* 若发现了矛盾，就没有更多事情要做。 */
					context->contradictory = true;
					return NIL;

				case PARTCLAUSE_NOMATCH:

					/*
					 * 子句不匹配该键，但它可能匹配
					 * 下一个。
					 */
					continue;

				case PARTCLAUSE_UNSUPPORTED:
					/* 本子句无法用于剪枝。 */
					break;
			}

			/* 完成；去检查下一个子句。 */
			break;
		}
	}

	/*-----------
	 * Now generate some (more) pruning steps.  We have three strategies:
	 *
	 * 1) Generate pruning steps based on IS NULL clauses:
	 *   a) For list partitioning, null partition keys can only be found in
	 *      the designated null-accepting partition, so if there are IS NULL
	 *      clauses containing partition keys we should generate a pruning
	 *      step that gets rid of all partitions but that one.  We can
	 *      disregard any OpExpr we may have found.
	 *   b) For range partitioning, only the default partition can contain
	 *      NULL values, so the same rationale applies.
	 *   c) For hash partitioning, we only apply this strategy if we have
	 *      IS NULL clauses for all the keys.  Strategy 2 below will take
	 *      care of the case where some keys have OpExprs and others have
	 *      IS NULL clauses.
	 *
	 * 2) If not, generate steps based on OpExprs we have (if any).
	 *
	 * 3) If this doesn't work either, we may be able to generate steps to
	 *    prune just the null-accepting partition (if one exists), if we have
	 *    IS NOT NULL clauses for all partition keys.
	 */
	if (!bms_is_empty(nullkeys) &&
		(part_scheme->strategy == PARTITION_STRATEGY_LIST ||
		 part_scheme->strategy == PARTITION_STRATEGY_RANGE ||
		 (part_scheme->strategy == PARTITION_STRATEGY_HASH &&
		  bms_num_members(nullkeys) == part_scheme->partnatts)))
	{
		PartitionPruneStep *step;

		/* 策略 1 */
		step = gen_prune_step_op(context, InvalidStrategy,
								 false, NIL, NIL, nullkeys);
		result = lappend(result, step);
	}
	else if (generate_opsteps)
	{
		List	   *opsteps;

		/* 策略 2 */
		opsteps = gen_prune_steps_from_opexps(context, keyclauses, nullkeys);
		result = list_concat(result, opsteps);
	}
	else if (bms_num_members(notnullkeys) == part_scheme->partnatts)
	{
		PartitionPruneStep *step;

		/* 策略 3 */
		step = gen_prune_step_op(context, InvalidStrategy,
								 false, NIL, NIL, NULL);
		result = lappend(result, step);
	}

	/*
	 * 最后，若有多个步骤，由于 clauses 彼此
	 * ANDed，就加入一个 INTERSECT 步骤，把由它们得到的分区集合
	 * 组合起来，并追加到结果列表。
	 */
	if (list_length(result) > 1)
	{
		List	   *step_ids = NIL;
		PartitionPruneStep *final;

		foreach(lc, result)
		{
			PartitionPruneStep *step = lfirst(lc);

			step_ids = lappend_int(step_ids, step->step_id);
		}

		final = gen_prune_step_combine(context, step_ids,
									   PARTPRUNE_COMBINE_INTERSECT);
		result = lappend(result, final);
	}

	return result;
}

/*
 * gen_prune_step_op
 *		为特定操作符生成一个剪枝步骤。
 *
 * 该步骤被赋予唯一的步骤标识符，并加入 context 的 steps
 * 列表。
 */
static PartitionPruneStep *
gen_prune_step_op(GeneratePruningStepsContext *context,
				  StrategyNumber opstrategy, bool op_is_ne,
				  List *exprs, List *cmpfns,
				  Bitmapset *nullkeys)
{
	PartitionPruneStepOp *opstep = makeNode(PartitionPruneStepOp);

	opstep->step.step_id = context->next_step_id++;

	/*
	 * 对含有 <> 操作符的子句，把 opstrategy 设为
	 * InvalidStrategy，以告知 get_matching_list_bounds 去做正确的
	 * 事。
	 */
	opstep->opstrategy = op_is_ne ? InvalidStrategy : opstrategy;
	Assert(list_length(exprs) == list_length(cmpfns));
	opstep->exprs = exprs;
	opstep->cmpfns = cmpfns;
	opstep->nullkeys = nullkeys;

	context->steps = lappend(context->steps, opstep);

	return (PartitionPruneStep *) opstep;
}

/*
 * gen_prune_step_combine
 *		为若干其他步骤的组合生成一个剪枝步骤。
 *
 * 该步骤被赋予唯一的步骤标识符，并加入 context 的
 * steps 列表。
 */
static PartitionPruneStep *
gen_prune_step_combine(GeneratePruningStepsContext *context,
					   List *source_stepids,
					   PartitionPruneCombineOp combineOp)
{
	PartitionPruneStepCombine *cstep = makeNode(PartitionPruneStepCombine);

	cstep->step.step_id = context->next_step_id++;
	cstep->combineOp = combineOp;
	cstep->source_stepids = source_stepids;

	context->steps = lappend(context->steps, cstep);

	return (PartitionPruneStep *) cstep;
}

/*
 * gen_prune_steps_from_opexps
 *		生成并返回基于已匹配到分区
 *		键的 OpExpr 和 BooleanTest 子句的 PartitionPruneStepOp
 *		列表。
 *
 * keyclauses 是 List 指针数组，按下标索引，下标即分区键的
 * 下标。数组里的每个 List 元素可以含匹配到
 * 对应分区键列的子句。没有任何
 * 匹配子句的分区键列将有一个空 List。
 *
 * 有些分区策略在我们只有
 * 分区键列前缀的子句时仍允许剪枝，例如 RANGE
 * 分区。其他策略，例如哈希分区，需要
 * 全部分区键列的子句。
 *
 * 这里返回多个剪枝步骤时，由调用者加入
 * 相应的组合步骤来组合返回的步骤。这里不
 * 做这件事，因为调用者可能希望在
 * 把它们全部组合之前再纳入额外的剪枝步骤。
 */
static List *
gen_prune_steps_from_opexps(GeneratePruningStepsContext *context,
							List **keyclauses, Bitmapset *nullkeys)
{
	PartitionScheme part_scheme = context->rel->part_scheme;
	List	   *opsteps = NIL;
	List	   *btree_clauses[BTMaxStrategyNumber + 1],
			   *hash_clauses[HTMaxStrategyNumber + 1];
	int			i;
	ListCell   *lc;

	memset(btree_clauses, 0, sizeof(btree_clauses));
	memset(hash_clauses, 0, sizeof(hash_clauses));
	for (i = 0; i < part_scheme->partnatts; i++)
	{
		List	   *clauselist = keyclauses[i];
		bool		consider_next_key = true;

		/*
		 * 对区间分区，若当前键没有子句，
		 * 也就没法考虑任何更后面的键，因此可以在这里停止。
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_RANGE &&
			clauselist == NIL)
			break;

		/*
		 * 对哈希分区，若某列没有必要的
		 * 相等子句，就应当有 IS NULL 子句，否则
		 * 剪枝是不可能的。
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_HASH &&
			clauselist == NIL && !bms_is_member(i, nullkeys))
			return NIL;

		foreach(lc, clauselist)
		{
			PartClauseInfo *pc = (PartClauseInfo *) lfirst(lc);
			Oid			lefttype,
						righttype;

			/* 查找该操作符的 btree/哈希策略号。 */
			if (pc->op_strategy == InvalidStrategy)
				get_op_opfamily_properties(pc->opno,
										   part_scheme->partopfamily[i],
										   false,
										   &pc->op_strategy,
										   &lefttype,
										   &righttype);

			switch (part_scheme->strategy)
			{
				case PARTITION_STRATEGY_LIST:
				case PARTITION_STRATEGY_RANGE:
					btree_clauses[pc->op_strategy] =
						lappend(btree_clauses[pc->op_strategy], pc);

					/*
					 * 若当前键的子句含有非包含
					 * 操作符，我们就没法考虑后续的
					 * 分区键。
					 */
					if (pc->op_strategy == BTLessStrategyNumber ||
						pc->op_strategy == BTGreaterStrategyNumber)
						consider_next_key = false;
					break;

				case PARTITION_STRATEGY_HASH:
					if (pc->op_strategy != HTEqualStrategyNumber)
						elog(ERROR, "invalid clause for hash partitioning");
					hash_clauses[pc->op_strategy] =
						lappend(hash_clauses[pc->op_strategy], pc);
					break;

				default:
					elog(ERROR, "invalid partition strategy: %c",
						 part_scheme->strategy);
					break;
			}
		}

		/*
		 * 若已判定后续分区键的子句
		 * 对剪枝没有用，就不要再往下搜索。
		 */
		if (!consider_next_key)
			break;
	}

	/*
	 * 现在，我们已经按操作符策略把子句分开。
	 * 对每种策略，检查能否通过
	 * 收集一组表达式来生成剪枝步骤，这些表达式的值将构成一个向量，
	 * 分区边界搜索
	 * 函数可以把它当作查找键。
	 */
	switch (part_scheme->strategy)
	{
		case PARTITION_STRATEGY_LIST:
		case PARTITION_STRATEGY_RANGE:
			{
				List	   *eq_clauses = btree_clauses[BTEqualStrategyNumber];
				List	   *le_clauses = btree_clauses[BTLessEqualStrategyNumber];
				List	   *ge_clauses = btree_clauses[BTGreaterEqualStrategyNumber];
				int			strat;

				/*
				 * 对给定策略下考虑的每个子句，
				 * 我们把更早键的子句里、操作符策略为包含的
				 * 那些表达式收集到一个名为
				 * prefix 的列表里。把该子句自己的表达式追加到
				 * prefix 之后，我们将用如此生成的向量生成一个步骤，
				 * 并把当前策略赋给它。实际上，
				 * prefix 可能对同一个键含有多个子句，
				 * 那种情况下，我们必须为不同键的表达式的各种
				 * 组合生成步骤，这由
				 * get_steps_using_prefix 为我们处理。
				 */
				for (strat = 1; strat <= BTMaxStrategyNumber; strat++)
				{
					foreach(lc, btree_clauses[strat])
					{
						PartClauseInfo *pc = lfirst(lc);
						ListCell   *eq_start;
						ListCell   *le_start;
						ListCell   *ge_start;
						ListCell   *lc1;
						List	   *prefix = NIL;
						List	   *pc_steps;
						bool		prefix_valid = true;
						bool		pk_has_clauses;
						int			keyno;

						/*
						 * 若这是第一个分区键的子句，
						 * 就没有在前的表达式；生成一个
						 * 不带前缀的剪枝步骤。
						 *
						 * 注意，我们为 step_nullkeys 传入 NULL，因为
						 * 我们不搜索某些键为 NULL 的列表/区间分区
						 * 边界。
						 */
						if (pc->keyno == 0)
						{
							Assert(pc->op_strategy == strat);
							pc_steps = get_steps_using_prefix(context, strat,
															  pc->op_is_ne,
															  pc->expr,
															  pc->cmpfn,
															  NULL,
															  NIL);
							opsteps = list_concat(opsteps, pc_steps);
							continue;
						}

						eq_start = list_head(eq_clauses);
						le_start = list_head(le_clauses);
						ge_start = list_head(ge_clauses);

						/*
						 * 我们按分区键号的升序
						 * 把子句排进 prefix。
						 */
						for (keyno = 0; keyno < pc->keyno; keyno++)
						{
							pk_has_clauses = false;

							/*
							 * 来自 = 子句的表达式总可以放进
							 * 前缀，只要它们来自更早的键。
							 */
							for_each_cell(lc1, eq_clauses, eq_start)
							{
								PartClauseInfo *eqpc = lfirst(lc1);

								if (eqpc->keyno == keyno)
								{
									prefix = lappend(prefix, eqpc);
									pk_has_clauses = true;
								}
								else
								{
									Assert(eqpc->keyno > keyno);
									break;
								}
							}
							eq_start = lc1;

							/*
							 * 若正在为 </<= 策略生成步骤，就可以
							 * 把其他 <= 子句加入前缀，
							 * 只要它们来自更早的键。
							 */
							if (strat == BTLessStrategyNumber ||
								strat == BTLessEqualStrategyNumber)
							{
								for_each_cell(lc1, le_clauses, le_start)
								{
									PartClauseInfo *lepc = lfirst(lc1);

									if (lepc->keyno == keyno)
									{
										prefix = lappend(prefix, lepc);
										pk_has_clauses = true;
									}
									else
									{
										Assert(lepc->keyno > keyno);
										break;
									}
								}
								le_start = lc1;
							}

							/*
							 * 若正在为 >/>= 策略生成步骤，就可以
							 * 把其他 >= 子句加入前缀，
							 * 只要它们来自更早的键。
							 */
							if (strat == BTGreaterStrategyNumber ||
								strat == BTGreaterEqualStrategyNumber)
							{
								for_each_cell(lc1, ge_clauses, ge_start)
								{
									PartClauseInfo *gepc = lfirst(lc1);

									if (gepc->keyno == keyno)
									{
										prefix = lappend(prefix, gepc);
										pk_has_clauses = true;
									}
									else
									{
										Assert(gepc->keyno > keyno);
										break;
									}
								}
								ge_start = lc1;
							}

							/*
							 * 若该键没有子句，prefix 就不再
							 * 有效。
							 */
							if (!pk_has_clauses)
							{
								prefix_valid = false;
								break;
							}
						}

						/*
						 * 若 prefix_valid，就生成 PartitionPruneStepOps。
						 * 否则，对该策略来说，我们已经找不到
						 * 分区键的有效
						 * 子集的子句了；放弃为该策略
						 * 继续生成分区剪枝步骤。
						 *
						 * 如上所述，若 prefix 对同一个键含有多个
						 * 表达式，下面就会
						 * 生成多个步骤，每个步骤对应不同键的
						 * 表达式的一种组合。
						 *
						 * 注意，我们为 step_nullkeys 传入 NULL，因为
						 * 我们不搜索某些键为 NULL 的列表/区间分区
						 * 边界。
						 */
						if (prefix_valid)
						{
							Assert(pc->op_strategy == strat);
							pc_steps = get_steps_using_prefix(context, strat,
															  pc->op_is_ne,
															  pc->expr,
															  pc->cmpfn,
															  NULL,
															  prefix);
							opsteps = list_concat(opsteps, pc_steps);
						}
						else
							break;
					}
				}
				break;
			}

		case PARTITION_STRATEGY_HASH:
			{
				List	   *eq_clauses = hash_clauses[HTEqualStrategyNumber];

				/* 对哈希分区，我们只有 = 策略。 */
				if (eq_clauses != NIL)
				{
					PartClauseInfo *pc;
					List	   *pc_steps;
					List	   *prefix = NIL;
					int			last_keyno;
					ListCell   *lc1;

					/*
					 * 定位最大列的子句。它可能
					 * 不属于最后一个分区键，但它是上面我们为之找到
					 * 子句的最后一个分区键
					 * 所属的子句。
					 */
					pc = llast(eq_clauses);

					/*
					 * 可能有多个子句匹配到该
					 * 分区键；找出第一个这样的子句。与此同时，
					 * 把该子句之前的所有子句加入 prefix。
					 */
					last_keyno = pc->keyno;
					foreach(lc, eq_clauses)
					{
						pc = lfirst(lc);
						if (pc->keyno == last_keyno)
							break;
						prefix = lappend(prefix, pc);
					}

					/*
					 * 对最后一列的每个子句，在把
					 * 该子句自己的表达式追加到 prefix 之后，我们将
					 * 用如此生成的向量生成一个步骤，并
					 * 把 = 赋为其策略。实际上，prefix 可能
					 * 对同一个键含有多个子句，那种
					 * 情况下，我们必须为不同键的表达式的各种组合
					 * 生成步骤，这由
					 * get_steps_using_prefix 为我们处理。
					 */
					for_each_cell(lc1, eq_clauses, lc)
					{
						pc = lfirst(lc1);

						/*
						 * 注意，我们为 step_nullkeys 传入 nullkeys，
						 * 因为需要告诉哈希分区边界搜索
						 * 函数，我们发现哪些键有 IS NULL
						 * 子句。
						 */
						Assert(pc->op_strategy == HTEqualStrategyNumber);
						pc_steps =
							get_steps_using_prefix(context,
												   HTEqualStrategyNumber,
												   false,
												   pc->expr,
												   pc->cmpfn,
												   nullkeys,
												   prefix);
						opsteps = list_concat(opsteps, pc_steps);
					}
				}
				break;
			}

		default:
			elog(ERROR, "invalid partition strategy: %c",
				 part_scheme->strategy);
			break;
	}

	return opsteps;
}

/*
 * 若分区键有排序规则，则子句必须有相同的
 * 输入排序规则。若分区键不可排序，我们就假定
 * 排序规则无关紧要，因为执行分区时并未考虑排序规则，
 * 子句仍可能被赋予排序规则，
 * 这是因为另一个输入属于可排序类型。
 *
 * 另见 IndexCollMatchesExprColl。
 */
#define PartCollMatchesExprColl(partcoll, exprcoll) \
	((partcoll) == InvalidOid || (partcoll) == (exprcoll))

/*
 * match_clause_to_partition_key
 *		试图把给定的 clause 与指定的分区键相匹配。
 *
 * 返回值是：
 * * PARTCLAUSE_NOMATCH：子句不匹配该分区键（但
 *   调用者应继续尝试，因为它可能匹配后续的键）。
 *   输出参数：均不设置。
 *
 * * PARTCLAUSE_MATCH_CLAUSE：存在匹配。
 *   输出参数：把 *pc 设为为该
 *   匹配子句构造的 PartClauseInfo。
 *
 * * PARTCLAUSE_MATCH_NULLNESS：存在匹配，且匹配的子句是
 *   a IS NULL 或 a IS NOT NULL 子句。
 *   输出参数：前一种情形把 *clause_is_not_null 设为假，
 *   否则设为真。
 *
 * * PARTCLAUSE_MATCH_STEPS：存在匹配。
 *   输出参数：把 *clause_steps 设为为该子句
 *   递归生成的步骤列表。
 *
 * * PARTCLAUSE_MATCH_CONTRADICT：子句自相矛盾，即
 *   它可以证明返回 FALSE 或 NULL。
 *   输出参数：均不设置。
 *
 * * PARTCLAUSE_UNSUPPORTED：子句不匹配该分区键，
 *   并且由于其形式或性质（例如含有易变函数）
 *   也不可能匹配任何其他键。
 *   输出参数：均不设置。
 */
static PartClauseMatchStatus
match_clause_to_partition_key(GeneratePruningStepsContext *context,
							  Expr *clause, Expr *partkey, int partkeyidx,
							  bool *clause_is_not_null, PartClauseInfo **pc,
							  List **clause_steps)
{
	PartClauseMatchStatus boolmatchstatus;
	PartitionScheme part_scheme = context->rel->part_scheme;
	Oid			partopfamily = part_scheme->partopfamily[partkeyidx],
				partcoll = part_scheme->partcollation[partkeyidx];
	Expr	   *expr;
	bool		noteq;

	/*
	 * 识别匹配布尔分区键的特殊形状子句。
	 */
	boolmatchstatus = match_boolean_partition_clause(partopfamily, clause,
													 partkey, &expr, &noteq);

	if (boolmatchstatus == PARTCLAUSE_MATCH_CLAUSE)
	{
		PartClauseInfo *partclause;

		/*
		 * 对 partkey IS NOT true 和 IS NOT false 形式的布尔测试，
		 * 我们把这些子句取反。实际上，partkey IS NOT true
		 * 变成 partkey IS false OR partkey IS NULL。我们通过
		 * 构造一个 OR BoolExpr、形成恰好那样的子句，并把它
		 * 交给 gen_partprune_steps_internal() 来生成剪枝
		 * 步骤。
		 */
		if (noteq)
		{
			List	   *new_clauses;
			List	   *or_clause;
			BooleanTest *new_booltest = (BooleanTest *) copyObject(clause);
			NullTest   *nulltest;

			/* 我们预期 noteq 只对 BooleanTests 被设为真 */
			Assert(IsA(clause, BooleanTest));

			/* 反转该布尔测试 */
			if (new_booltest->booltesttype == IS_NOT_TRUE)
				new_booltest->booltesttype = IS_FALSE;
			else if (new_booltest->booltesttype == IS_NOT_FALSE)
				new_booltest->booltesttype = IS_TRUE;
			else
			{
				/*
				 * 我们只预期 match_boolean_partition_clause 匹配
				 * IS_NOT_TRUE 和 IS_NOT_FALSE。不支持
				 * IS_NOT_UNKNOWN。
				 */
				Assert(false);
			}

			nulltest = makeNode(NullTest);
			nulltest->arg = copyObject(partkey);
			nulltest->nulltesttype = IS_NULL;
			nulltest->argisrow = false;
			nulltest->location = -1;

			new_clauses = list_make2(new_booltest, nulltest);
			or_clause = list_make1(makeBoolExpr(OR_EXPR, new_clauses, -1));

			/* 最后，生成步骤 */
			*clause_steps = gen_partprune_steps_internal(context, or_clause);

			if (context->contradictory)
				return PARTCLAUSE_MATCH_CONTRADICT; /* 不应当发生 */
			else if (*clause_steps == NIL)
				return PARTCLAUSE_UNSUPPORTED;	/* 步骤生成失败 */
			return PARTCLAUSE_MATCH_STEPS;
		}

		partclause = (PartClauseInfo *) palloc(sizeof(PartClauseInfo));
		partclause->keyno = partkeyidx;
		/* 用布尔相等操作符做剪枝。 */
		partclause->opno = BooleanEqualOperator;
		partclause->op_is_ne = false;
		partclause->expr = expr;
		/* 我们知道 expr 是布尔类型。 */
		partclause->cmpfn = part_scheme->partsupfunc[partkeyidx].fn_oid;
		partclause->op_strategy = InvalidStrategy;

		*pc = partclause;

		return PARTCLAUSE_MATCH_CLAUSE;
	}
	else if (IsA(clause, OpExpr) &&
			 list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Expr	   *leftop,
				   *rightop;
		Oid			opno,
					op_lefttype,
					op_righttype,
					negator = InvalidOid;
		Oid			cmpfn;
		int			op_strategy;
		bool		is_opne_listp = false;
		PartClauseInfo *partclause;

		leftop = (Expr *) get_leftop(clause);
		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;
		rightop = (Expr *) get_rightop(clause);
		if (IsA(rightop, RelabelType))
			rightop = ((RelabelType *) rightop)->arg;
		opno = opclause->opno;

		/* 检查该子句是否匹配这个分区键 */
		if (equal(leftop, partkey))
			expr = rightop;
		else if (equal(rightop, partkey))
		{
			/*
			 * 只有能交换操作符、把分区键放到左边时，它才有用。若没法
			 * 交换，该子句就可以被判定为
			 * UNSUPPORTED。即使它的 leftop 匹配某个更后面的分区键，我们
			 * 现在也知道右边有 Var，因此没有用。
			 */
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return PARTCLAUSE_UNSUPPORTED;
			expr = leftop;
		}
		else
			/* 子句不匹配这个分区键，但也许匹配下一个。 */
			return PARTCLAUSE_NOMATCH;

		/*
		 * 分区键匹配还需要排序规则匹配。可能有
		 * 多个表达式相同但排序规则不同的分区键，
		 * 因此失败是 NOMATCH。
		 */
		if (!PartCollMatchesExprColl(partcoll, opclause->inputcollid))
			return PARTCLAUSE_NOMATCH;

		/*
		 * 查看该操作符是否与分区操作符族相关。
		 *
		 * 通常我们只关心被列为分区操作符族
		 * 一部分的操作符。但有一个例外：
		 * 不等操作符不会被列入任何操作符族，
		 * 但它们的否定符（相等）会。若找到这样的否定符就可以用，但只用于列表
		 * 分区。
		 *
		 * 注意：失败时我们报告 NOMATCH，以防更后面的分区键有
		 * 相同的表达式但不同的操作符族。这不太可能，但并不比
		 * 排序规则不同的重复表达式更不可能多少。
		 */
		if (op_in_opfamily(opno, partopfamily))
		{
			get_op_opfamily_properties(opno, partopfamily, false,
									   &op_strategy, &op_lefttype,
									   &op_righttype);
		}
		else
		{
			if (part_scheme->strategy != PARTITION_STRATEGY_LIST)
				return PARTCLAUSE_NOMATCH;

			/* 查看否定符是否为相等 */
			negator = get_negator(opno);
			if (OidIsValid(negator) && op_in_opfamily(negator, partopfamily))
			{
				get_op_opfamily_properties(negator, partopfamily, false,
										   &op_strategy, &op_lefttype,
										   &op_righttype);
				if (op_strategy == BTEqualStrategyNumber)
					is_opne_listp = true;	/* 正是 */
			}

			/* 不是，它也不是 <>。 */
			if (!is_opne_listp)
				return PARTCLAUSE_NOMATCH;
		}

		/*
		 * 只允许严格操作符。这将保证空值被
		 * 滤掉。（这个测试很可能没有用，因为 btree 和哈希
		 * 比较操作符通常都是严格的。）
		 */
		if (!op_strict(opno))
			return PARTCLAUSE_UNSUPPORTED;

		/*
		 * 好，我们有与分区键的匹配以及合适的操作符。
		 * 检查另一个参数，看它能否用于剪枝。
		 *
		 * 在这些情形的大多数里，我们可以返回 UNSUPPORTED，因为无论
		 * 匹配到哪个分区键，同样的
		 * 失败都会发生。（特别是，既然我们已经成功把操作子句的
		 * 一侧匹配到某个分区键，再把另一
		 * 侧匹配到另一个分区键就不会产生可用结果，因为那
		 * 意味着两侧都有 Var。）
		 *
		 * 另外，若我们因与目标相关的原因拒绝某个参数，就设置
		 * *context 的相应字段来报告这一点。我们把这些
		 * 测试推迟到匹配分区键和操作符之后，以便
		 * 降低为最终并不贡献剪枝步骤的子句
		 * 设置上下文字段的几率。
		 *
		 * 首先检查非 Const 参数。（我们假定任何不可变
		 * 子表达式都已经被折叠成 Const。）
		 */
		if (!IsA(expr, Const))
		{
			Bitmapset  *paramids;

			/*
			 * 在规划器里做剪枝时，我们只支持用与
			 * 常量的比较来剪枝。我们无法依据
			 * 并非不可变的任何东西做剪枝。（注意，对该目标值
			 * 不会设置 has_mutable_arg 和 has_exec_param。）
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 我们永远无法用含有 Var 的表达式做剪枝。
			 */
			if (contain_var_clause((Node *) expr))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 而且必须拒绝任何含有易变函数的东西。
			 * 稳定函数则可以。
			 */
			if (contain_volatile_functions((Node *) expr))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 查看是否有任何执行期参数。若有，就只能在
			 * 按扫描剪枝期间使用该表达式。
			 */
			paramids = pull_exec_paramids(expr);
			if (!bms_is_empty(paramids))
			{
				context->has_exec_param = true;
				if (context->target != PARTTARGET_EXEC)
					return PARTCLAUSE_UNSUPPORTED;
			}
			else
			{
				/* 它可能可用，但是可变的 */
				context->has_mutable_arg = true;
			}
		}

		/*
		 * 检查比较操作符本身是否不可变。（我们
		 * 假定 btree 或哈希操作符类里的任何东西至少是
		 * 稳定的，但仍需要检查不可变性。）
		 */
		if (op_volatile(opno) != PROVOLATILE_IMMUTABLE)
		{
			context->has_mutable_op = true;

			/*
			 * 在规划器里做剪枝时，无法用可变
			 * 操作符做剪枝。
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * 现在按类型找出要用的过程。若子句的
		 * 另一个参数与分区操作符类声明的输入类型
		 * 相同，就可以用缓存在
		 * PartitionKey 里的过程。否则，在同一
		 * 操作符族里搜索跨类型过程；若不存在，就报告不匹配。
		 */
		if (op_righttype == part_scheme->partopcintype[partkeyidx])
			cmpfn = part_scheme->partsupfunc[partkeyidx].fn_oid;
		else
		{
			switch (part_scheme->strategy)
			{
					/*
					 * 对区间和列表分区，我们需要排序
					 * 过程，其 lefttype 是分区键的类型，
					 * righttype 是子句操作符的右类型。
					 */
				case PARTITION_STRATEGY_LIST:
				case PARTITION_STRATEGY_RANGE:
					cmpfn =
						get_opfamily_proc(part_scheme->partopfamily[partkeyidx],
										  part_scheme->partopcintype[partkeyidx],
										  op_righttype, BTORDER_PROC);
					break;

					/*
					 * 对哈希分区，我们需要该子句类型的
					 * 哈希过程。
					 */
				case PARTITION_STRATEGY_HASH:
					cmpfn =
						get_opfamily_proc(part_scheme->partopfamily[partkeyidx],
										  op_righttype, op_righttype,
										  HASHEXTENDED_PROC);
					break;

				default:
					elog(ERROR, "invalid partition strategy: %c",
						 part_scheme->strategy);
					cmpfn = InvalidOid; /* 消除编译器告警 */
					break;
			}

			if (!OidIsValid(cmpfn))
				return PARTCLAUSE_NOMATCH;
		}

		/*
		 * 构造该子句，若适用就传入否定符。
		 */
		partclause = (PartClauseInfo *) palloc(sizeof(PartClauseInfo));
		partclause->keyno = partkeyidx;
		if (is_opne_listp)
		{
			Assert(OidIsValid(negator));
			partclause->opno = negator;
			partclause->op_is_ne = true;
			partclause->op_strategy = InvalidStrategy;
		}
		else
		{
			partclause->opno = opno;
			partclause->op_is_ne = false;
			partclause->op_strategy = op_strategy;
		}
		partclause->expr = expr;
		partclause->cmpfn = cmpfn;

		*pc = partclause;

		return PARTCLAUSE_MATCH_CLAUSE;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Oid			saop_op = saop->opno;
		Oid			saop_coll = saop->inputcollid;
		Expr	   *leftop = (Expr *) linitial(saop->args),
				   *rightop = (Expr *) lsecond(saop->args);
		List	   *elem_exprs,
				   *elem_clauses;
		ListCell   *lc1;

		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		/* 检查左部是否匹配这个分区键 */
		if (!equal(leftop, partkey) ||
			!PartCollMatchesExprColl(partcoll, saop->inputcollid))
			return PARTCLAUSE_NOMATCH;

		/*
		 * 查看该操作符是否与分区操作符族相关。
		 *
		 * 对 NOT IN (..)，我们得到 <>，若正在使用列表
		 * 分区，并且能确认它的否定符
		 * 是属于分区操作符族的 btree 相等操作符，就处理它。
		 * 如上，对不匹配的操作符报告 NOMATCH。
		 */
		if (!op_in_opfamily(saop_op, partopfamily))
		{
			Oid			negator;

			if (part_scheme->strategy != PARTITION_STRATEGY_LIST)
				return PARTCLAUSE_NOMATCH;

			negator = get_negator(saop_op);
			if (OidIsValid(negator) && op_in_opfamily(negator, partopfamily))
			{
				int			strategy;
				Oid			lefttype,
							righttype;

				get_op_opfamily_properties(negator, partopfamily,
										   false, &strategy,
										   &lefttype, &righttype);
				if (strategy != BTEqualStrategyNumber)
					return PARTCLAUSE_NOMATCH;
			}
			else
				return PARTCLAUSE_NOMATCH;	/* 没有有用的否定符 */
		}

		/*
		 * 只允许严格操作符。这将保证空值被
		 * 滤掉。（这个测试很可能没有用，因为 btree 和哈希
		 * 比较操作符通常都是严格的。）
		 */
		if (!op_strict(saop_op))
			return PARTCLAUSE_UNSUPPORTED;

		/*
		 * 好，我们有与分区键的匹配以及合适的操作符。
		 * 检查数组参数，看它能否用于剪枝。这
		 * 与普通 OpExpr 的逻辑相同。
		 */
		if (!IsA(rightop, Const))
		{
			Bitmapset  *paramids;

			/*
			 * 在规划器里做剪枝时，我们只支持用与
			 * 常量的比较来剪枝。我们无法依据
			 * 并非不可变的任何东西做剪枝。（注意，对该目标值
			 * 不会设置 has_mutable_arg 和 has_exec_param。）
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 我们永远无法用含有 Var 的表达式做剪枝。
			 */
			if (contain_var_clause((Node *) rightop))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 而且必须拒绝任何含有易变函数的东西。
			 * 稳定函数则可以。
			 */
			if (contain_volatile_functions((Node *) rightop))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 查看是否有任何执行期参数。若有，就只能在
			 * 按扫描剪枝期间使用该表达式。
			 */
			paramids = pull_exec_paramids(rightop);
			if (!bms_is_empty(paramids))
			{
				context->has_exec_param = true;
				if (context->target != PARTTARGET_EXEC)
					return PARTCLAUSE_UNSUPPORTED;
			}
			else
			{
				/* 它可能可用，但是可变的 */
				context->has_mutable_arg = true;
			}
		}

		/*
		 * 检查比较操作符本身是否不可变。（我们
		 * 假定 btree 或哈希操作符类里的任何东西至少是
		 * 稳定的，但仍需要检查不可变性。）
		 */
		if (op_volatile(saop_op) != PROVOLATILE_IMMUTABLE)
		{
			context->has_mutable_op = true;

			/*
			 * 在规划器里做剪枝时，无法用可变
			 * 操作符做剪枝。
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * 检查数组参数的内容。
		 */
		elem_exprs = NIL;
		if (IsA(rightop, Const))
		{
			/*
			 * 对常量数组，把元素转换成 Const
			 * 节点列表，每个数组元素一个（空值除外）。
			 */
			Const	   *arr = (Const *) rightop;
			ArrayType  *arrval;
			int16		elemlen;
			bool		elembyval;
			char		elemalign;
			Datum	   *elem_values;
			bool	   *elem_nulls;
			int			num_elems,
						i;

			/* 若数组本身为空，saop 就返回空 */
			if (arr->constisnull)
				return PARTCLAUSE_MATCH_CONTRADICT;

			arrval = DatumGetArrayTypeP(arr->constvalue);
			get_typlenbyvalalign(ARR_ELEMTYPE(arrval),
								 &elemlen, &elembyval, &elemalign);
			deconstruct_array(arrval,
							  ARR_ELEMTYPE(arrval),
							  elemlen, elembyval, elemalign,
							  &elem_values, &elem_nulls,
							  &num_elems);
			for (i = 0; i < num_elems; i++)
			{
				Const	   *elem_expr;

				/*
				 * 空数组元素必然导致空的比较结果，
				 * 因为已知 saop_op 是严格的。在 useOr 情形下可以忽略它，
				 * 否则就意味着自相矛盾。
				 */
				if (elem_nulls[i])
				{
					if (saop->useOr)
						continue;
					return PARTCLAUSE_MATCH_CONTRADICT;
				}

				elem_expr = makeConst(ARR_ELEMTYPE(arrval), -1,
									  arr->constcollid, elemlen,
									  elem_values[i], false, elembyval);
				elem_exprs = lappend(elem_exprs, elem_expr);
			}
		}
		else if (IsA(rightop, ArrayExpr))
		{
			ArrayExpr  *arrexpr = castNode(ArrayExpr, rightop);

			/*
			 * 对嵌套的 ArrayExpr，我们不知道如何把实际的
			 * 标量值取成一个扁平列表，因此放弃对该
			 * ScalarArrayOpExpr 做任何处理。
			 */
			if (arrexpr->multidims)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * 否则，可以直接使用元素值列表。
			 */
			elem_exprs = arrexpr->elements;
		}
		else
		{
			/* 对任何其他子句类型都放弃。 */
			return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * 现在为每个数组元素生成一个子句列表，形式为
		 * leftop saop_op elem_expr
		 */
		elem_clauses = NIL;
		foreach(lc1, elem_exprs)
		{
			Expr	   *elem_clause;

			elem_clause = make_opclause(saop_op, BOOLOID, false,
										leftop, lfirst(lc1),
										InvalidOid, saop_coll);
			elem_clauses = lappend(elem_clauses, elem_clause);
		}

		/*
		 * 若有 ANY 子句且有多个元素，现在把子句
		 * 列表变成一个 OR 表达式。
		 */
		if (saop->useOr && list_length(elem_clauses) > 1)
			elem_clauses = list_make1(makeBoolExpr(OR_EXPR, elem_clauses, -1));

		/* 最后，生成步骤 */
		*clause_steps = gen_partprune_steps_internal(context, elem_clauses);
		if (context->contradictory)
			return PARTCLAUSE_MATCH_CONTRADICT;
		else if (*clause_steps == NIL)
			return PARTCLAUSE_UNSUPPORTED;	/* 步骤生成失败 */
		return PARTCLAUSE_MATCH_STEPS;
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *nulltest = (NullTest *) clause;
		Expr	   *arg = nulltest->arg;

		if (IsA(arg, RelabelType))
			arg = ((RelabelType *) arg)->arg;

		/* arg 是否匹配该分区键列？ */
		if (!equal(arg, partkey))
			return PARTCLAUSE_NOMATCH;

		*clause_is_not_null = (nulltest->nulltesttype == IS_NOT_NULL);

		return PARTCLAUSE_MATCH_NULLNESS;
	}

	/*
	 * 若执行到这里，返回值取决于上面
	 * match_boolean_partition_clause 调用的结果。若该调用返回
	 * PARTCLAUSE_UNSUPPORTED，则我们要么不是在处理布尔限定条件，
	 * 要么该布尔限定条件不适合剪枝。既然该限定条件没有
	 * 匹配这里支持的任何其他限定条件类型，那么试着把它
	 * 与任何其他分区键匹配就是浪费时间，因此直接
	 * 返回 PARTCLAUSE_UNSUPPORTED。若该限定条件只是无法匹配
	 * 这个分区键，则它可能匹配另一个，因此返回
	 * PARTCLAUSE_NOMATCH。match_boolean_partition_clause 还能返回的唯一其他值是
	 * PARTCLAUSE_MATCH_CLAUSE，
	 * 而该值上面已经处理过，因此我们可以直接
	 * 返回 boolmatchstatus。
	 */
	return boolmatchstatus;
}

/*
 * get_steps_using_prefix
 *		基于给定输入生成 PartitionPruneStepOps 列表。
 *
 * step_lastexpr 和 step_lastcmpfn 是我们有子句的最后一个分区键
 * 所属的 Expr 和比较函数。prefix
 * 是该 step_lastexpr 和 step_lastcmpfn 之前的分区键号的 PartClauseInfos 列表。
 * prefix 可能含属于同一个分区键的多个
 * PartClauseInfos。我们将为给定 PartClauseInfos 的每种组合
 * 生成一个 PartitionPruneStepOp，
 * 每个分区键最多使用一个 PartClauseInfo。
 *
 * 对 LIST 和 RANGE 已分区表，调用者必须保证
 * step_nullkeys 为 NULL，并且 prefix 对
 * step_lastexpr 和 step_lastcmpfn 所属键之前的每个分区键
 * 都至少含一个子句。
 *
 * 对哈希已分区表，调用者必须保证 prefix 对
 * 除最后一个键之外的每个分区键都至少含一个子句
 * （最后一个键的表达式和比较函数在 step_lastexpr
 * 和 step_lastcmpfn 里）。step_nullkeys 里置位可以代替
 * prefix 列表里任意给定键的子句。若某个给定键在 step_nullkeys
 * 里置了位，则 prefix 列表里不得有该键的
 * PartClauseInfo。
 *
 * 对上述每种情形，调用者必须保证 prefix 里的 PartClauseInfos
 * 按 keyno 升序排列。
 */
static List *
get_steps_using_prefix(GeneratePruningStepsContext *context,
					   StrategyNumber step_opstrategy,
					   bool step_op_is_ne,
					   Expr *step_lastexpr,
					   Oid step_lastcmpfn,
					   Bitmapset *step_nullkeys,
					   List *prefix)
{
	/* 对 RANGE 和 LIST 已分区表，step_nullkeys 必须为空 */
	Assert(step_nullkeys == NULL ||
		   context->rel->part_scheme->strategy == PARTITION_STRATEGY_HASH);

	/*
	 * 当 prefix 为空列表时，不需要递归处理。
	 * 这发生在只有 1 个分区键列时。
	 */
	if (prefix == NIL)
	{
		PartitionPruneStep *step;

		step = gen_prune_step_op(context,
								 step_opstrategy,
								 step_op_is_ne,
								 list_make1(step_lastexpr),
								 list_make1_oid(step_lastcmpfn),
								 step_nullkeys);
		return list_make1(step);
	}

	/* 递归，为子句的每种组合生成步骤。 */
	return get_steps_using_prefix_recurse(context,
										  step_opstrategy,
										  step_op_is_ne,
										  step_lastexpr,
										  step_lastcmpfn,
										  step_nullkeys,
										  prefix,
										  list_head(prefix),
										  NIL, NIL);
}

/*
 * get_steps_using_prefix_recurse
 *		用从 start 单元开始的 prefix
 *		PartClauseInfos 列表生成并返回 PartitionPruneStepOps 列表。
 *
 * 当 prefix 对单个分区键含有多个 PartClauseInfos 时，
 * 我们为重复的 PartClauseInfos 的每种组合创建一个
 * PartitionPruneStepOp。返回的列表对输入 PartClauseInfos 的
 * 每种唯一组合都含一个 PartitionPruneStepOp，
 * 每个分区键最多含一个 PartClauseInfo。
 *
 * prefix 是按 keyno 排序的 PartClauseInfos 输入列表。
 * start 标出搜索 prefix 列表应当开始的单元。
 * step_exprs 和 step_cmpfns 分别含我们迄今从
 * 前面分区键的子句生成的表达式和比较函数。
 */
static List *
get_steps_using_prefix_recurse(GeneratePruningStepsContext *context,
							   StrategyNumber step_opstrategy,
							   bool step_op_is_ne,
							   Expr *step_lastexpr,
							   Oid step_lastcmpfn,
							   Bitmapset *step_nullkeys,
							   List *prefix,
							   ListCell *start,
							   List *step_exprs,
							   List *step_cmpfns)
{
	List	   *result = NIL;
	ListCell   *lc;
	int			cur_keyno;
	int			final_keyno;

	/* 实际上，递归会受到 PARTITION_MAX_KEYS 的限制。 */
	check_stack_depth();

	Assert(start != NULL);
	cur_keyno = ((PartClauseInfo *) lfirst(start))->keyno;
	final_keyno = ((PartClauseInfo *) llast(prefix))->keyno;

	/* 检查是否需要递归。 */
	if (cur_keyno < final_keyno)
	{
		PartClauseInfo *pc;
		ListCell   *next_start;

		/*
		 * 找出属于下一个分区键的第一个 PartClauseInfo，
		 * 下一次递归调用必须从那一点开始迭代
		 * prefix 列表。
		 */
		for_each_cell(lc, prefix, start)
		{
			pc = lfirst(lc);

			if (pc->keyno > cur_keyno)
				break;
		}

		/* 记录下一次递归调用从哪里开始迭代 */
		next_start = lc;

		/*
		 * 对每个 keyno 设为 cur_keyno 的 PartClauseInfo，把它的 expr
		 * 和 cmpfn 分别加入 step_exprs 和 step_cmpfns，并用
		 * next_start 作为 prefix 列表的起点来递归。
		 */
		for_each_cell(lc, prefix, start)
		{
			List	   *moresteps;
			List	   *step_exprs1,
					   *step_cmpfns1;

			pc = lfirst(lc);
			if (pc->keyno == cur_keyno)
			{
				/* 保持原来的 step_exprs 不被修改。 */
				step_exprs1 = list_copy(step_exprs);
				step_exprs1 = lappend(step_exprs1, pc->expr);

				/* 保持原来的 step_cmpfns 不被修改。 */
				step_cmpfns1 = list_copy(step_cmpfns);
				step_cmpfns1 = lappend_oid(step_cmpfns1, pc->cmpfn);
			}
			else
			{
				/* 检查 prefix 列表是否按正确顺序排列 */
				Assert(pc->keyno > cur_keyno);
				break;
			}

			moresteps = get_steps_using_prefix_recurse(context,
													   step_opstrategy,
													   step_op_is_ne,
													   step_lastexpr,
													   step_lastcmpfn,
													   step_nullkeys,
													   prefix,
													   next_start,
													   step_exprs1,
													   step_cmpfns1);
			result = list_concat(result, moresteps);

			list_free(step_exprs1);
			list_free(step_cmpfns1);
		}
	}
	else
	{
		/*
		 * 结束当前递归周期并开始生成步骤，对每个
		 * cur_keyno 的子句各一个，也就是从这里起直到
		 * 列表末尾的全部子句。注意，对哈希分区，
		 * 允许 step_nullkeys 非空，那种情况下 step_exprs
		 * 只会含那些没有在 step_nullkeys 里
		 * 指定的分区键的表达式。
		 */
		Assert(list_length(step_exprs) == cur_keyno ||
			   !bms_is_empty(step_nullkeys));

		/*
		 * 还要注意，对哈希分区，每个分区键应当
		 * 要么有相等子句，要么有 IS NULL 子句，因此若某个
		 * 分区键没有表达式，它就会在
		 * step_nullkeys 里被指定。
		 */
		Assert(context->rel->part_scheme->strategy
			   != PARTITION_STRATEGY_HASH ||
			   list_length(step_exprs) + 2 + bms_num_members(step_nullkeys) ==
			   context->rel->part_scheme->partnatts);
		for_each_cell(lc, prefix, start)
		{
			PartClauseInfo *pc = lfirst(lc);
			PartitionPruneStep *step;
			List	   *step_exprs1,
					   *step_cmpfns1;

			Assert(pc->keyno == cur_keyno);

			/* 保持原来的 step_exprs 不被修改。 */
			step_exprs1 = list_copy(step_exprs);
			step_exprs1 = lappend(step_exprs1, pc->expr);
			step_exprs1 = lappend(step_exprs1, step_lastexpr);

			/* 保持原来的 step_cmpfns 不被修改。 */
			step_cmpfns1 = list_copy(step_cmpfns);
			step_cmpfns1 = lappend_oid(step_cmpfns1, pc->cmpfn);
			step_cmpfns1 = lappend_oid(step_cmpfns1, step_lastcmpfn);

			step = gen_prune_step_op(context,
									 step_opstrategy, step_op_is_ne,
									 step_exprs1, step_cmpfns1,
									 step_nullkeys);
			result = lappend(result, step);
		}
	}

	return result;
}

/*
 * get_matching_hash_bounds
 *		确定与指定值匹配的哈希边界的偏移，
 *		考虑到所有非空值都来自含有
 *		相容哈希相等操作符的子句，而任何为空的键都来自
 *		IS NULL 子句。
 *
 * 通常本函数返回单个匹配的边界偏移，
 * 不过若某个给定模数尚未建立分区，我们可能
 * 返回零个匹配。若找到的子句数没有覆盖整个
 * 分区键，就需要返回全部偏移。
 *
 * opstrategy 若非零，则必须是 HTEqualStrategyNumber。
 *
 * values 含按下标索引的 Datum，下标即用于剪枝的分区键。
 *
 * nvalues 是 values 数组里的 Datum 个数。
 *
 * partsupfunc 含能为 values 里所含值的类型产生正确
 * 哈希的分区哈希函数。
 *
 * nullkeys 是为空的分区键集合。
 */
static PruneStepResult *
get_matching_hash_bounds(PartitionPruneContext *context,
						 StrategyNumber opstrategy, Datum *values, int nvalues,
						 FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	int		   *partindices = boundinfo->indexes;
	int			partnatts = context->partnatts;
	bool		isnull[PARTITION_MAX_KEYS];
	int			i;
	uint64		rowHash;
	int			greatest_modulus;
	Oid		   *partcollation = context->partcollation;

	Assert(context->strategy == PARTITION_STRATEGY_HASH);

	/*
	 * 对哈希分区，我们只能基于与分区键的相等
	 * 子句或 IS NULL 子句来剪枝。而且只有在
	 * 取得了全部键的值时才能剪枝。
	 */
	if (nvalues + bms_num_members(nullkeys) == partnatts)
	{
		/*
		 * 若有任何值，它们必须来自含有与哈希分区
		 * 相容的相等操作符的子句。
		 */
		Assert(opstrategy == HTEqualStrategyNumber || nvalues == 0);

		for (i = 0; i < partnatts; i++)
			isnull[i] = bms_is_member(i, nullkeys);

		rowHash = compute_partition_hash_value(partnatts, partsupfunc, partcollation,
											   values, isnull);

		greatest_modulus = boundinfo->nindexes;
		if (partindices[rowHash % greatest_modulus] >= 0)
			result->bound_offsets =
				bms_make_singleton(rowHash % greatest_modulus);
	}
	else
	{
		/* 报告 boundinfo->indexes 数组里所有有效偏移。 */
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->nindexes - 1);
	}

	/*
	 * 既没有特殊的哈希空分区，也没有默认的哈希
	 * 分区。
	 */
	result->scan_null = result->scan_default = false;

	return result;
}

/*
 * get_matching_list_bounds
 *		按给定操作符策略的语义，确定与指定值
 *		匹配的列表边界的偏移。
 *
 * 若默认分区需要被扫描（前提是确实存在一个），返回的结构里会设置
 * scan_default。若特殊的接受 NULL 的分区需要被扫描，会设置
 * scan_null。
 *
 * opstrategy 若非零，则必须是 btree 策略号。
 *
 * value 含用于剪枝的值。
 *
 * nvalues 若非零，则应当恰好为 1，因为这是列表分区。
 *
 * partsupfunc 含要用来执行 partition_list_bsearch 的
 * 列表分区比较函数。
 *
 * nullkeys 是为空的分区键集合。
 */
static PruneStepResult *
get_matching_list_bounds(PartitionPruneContext *context,
						 StrategyNumber opstrategy, Datum value, int nvalues,
						 FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	int			off,
				minoff,
				maxoff;
	bool		is_equal;
	bool		inclusive = false;
	Oid		   *partcollation = context->partcollation;

	Assert(context->strategy == PARTITION_STRATEGY_LIST);
	Assert(context->partnatts == 1);

	result->scan_null = result->scan_default = false;

	if (!bms_is_empty(nullkeys))
	{
		/*
		 * 空值可能只存在于一个分区里：其
		 * 接受的值集合包含空的那个分区，或者若前者不存在，则是
		 * 默认分区。
		 */
		if (partition_bound_accepts_nulls(boundinfo))
			result->scan_null = true;
		else
			result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	/*
	 * 若没有可与键比较的 datum，但有分区，
	 * 若存在默认分区就只返回它。
	 */
	if (boundinfo->ndatums == 0)
	{
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	minoff = 0;
	maxoff = boundinfo->ndatums - 1;

	/*
	 * 若没有可与 boundinfo 里的 datum 比较的值，就
	 * 意味着调用者要求所有非空 datum 的分区。把
	 * *所有*分区的下标加进去，包括默认分区（若有）。
	 */
	if (nvalues == 0)
	{
		Assert(boundinfo->ndatums > 0);
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->ndatums - 1);
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	/* 对来自 <> 操作符子句的值做特殊处理。 */
	if (opstrategy == InvalidStrategy)
	{
		/*
		 * 先匹配全部分界。下面会去掉任何匹配的 datum。
		 */
		Assert(boundinfo->ndatums > 0);
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->ndatums - 1);

		off = partition_list_bsearch(partsupfunc, partcollation, boundinfo,
									 value, &is_equal);
		if (off >= 0 && is_equal)
		{

			/* 我们有一个匹配。从结果里去掉它。 */
			Assert(boundinfo->indexes[off] >= 0);
			result->bound_offsets = bms_del_member(result->bound_offsets,
												   off);
		}

		/* 若有默认分区，总是把它包括进去。 */
		result->scan_default = partition_bound_has_default(boundinfo);

		return result;
	}

	/*
	 * 对区间查询，总是包括默认列表分区，因为
	 * 列表分区以不连续的方式划分键空间，给定区间里并非所有
	 * 值都分配了分区。对某些数据类型（例如整数类型）这在技术上
	 * 可能并不成立，然而
	 * 我们目前缺少任何能为我们提供证明的基础设施，
	 * 因而没法在这里做得更聪明。
	 */
	if (opstrategy != BTEqualStrategyNumber)
		result->scan_default = partition_bound_has_default(boundinfo);

	switch (opstrategy)
	{
		case BTEqualStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0 && is_equal)
			{
				Assert(boundinfo->indexes[off] >= 0);
				result->bound_offsets = bms_make_singleton(off);
			}
			else
				result->scan_default = partition_bound_has_default(boundinfo);
			return result;

		case BTGreaterEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTGreaterStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0)
			{
				/* 我们不希望匹配的 datum 出现在结果里。 */
				if (!is_equal || !inclusive)
					off++;
			}
			else
			{
				/*
				 * 这种情形意味着所有分区边界都更大，这
				 * 进而意味着所有分区都满足该键。
				 */
				off = 0;
			}

			/*
			 * off 大于我们有分区的 datum 个数。
			 * 唯一可能含有匹配的分区是
			 * 默认分区，但若存在一个，我们上面反正已经设置了
			 * context->scan_default。
			 */
			if (off > boundinfo->ndatums - 1)
				return result;

			minoff = off;
			break;

		case BTLessEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTLessStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0 && is_equal && !inclusive)
				off--;

			/*
			 * off 小于所有非默认分区的 datum。
			 * 唯一可能含有匹配的分区是
			 * 默认分区，但若存在一个，我们上面反正已经设置了
			 * context->scan_default。
			 */
			if (off < 0)
				return result;

			maxoff = off;
			break;

		default:
			elog(ERROR, "invalid strategy number %d", opstrategy);
			break;
	}

	Assert(minoff >= 0 && maxoff >= 0);
	result->bound_offsets = bms_add_range(NULL, minoff, maxoff);
	return result;
}


/*
 * get_matching_range_bounds
 *		按给定操作符策略的语义，确定与指定值
 *		匹配的区间边界的偏移。
 *
 * 结果里偏移所对应的每个 datum 都要当作
 * 将含有所需值的分区的上界。
 *
 * 若存在默认分区，并且我们完全确定需要扫描它，
 * 返回的结构里会设置 scan_default。我们并不
 * 仅仅因为值匹配了默认分区以外的分区未覆盖的键空间
 * 部分就设置它（我们通常假定这些空间属于
 * 默认分区）：组合多个剪枝步骤后得到的最终边界集合
 * 可能把它排除，因此我们在
 * 别处推断是否纳入它。
 *
 * opstrategy 若非零，则必须是 btree 策略号。
 *
 * values 含按下标索引的 Datum，下标即用于剪枝的分区键。
 *
 * nvalues 是 values 数组里的 Datum 个数。必须 <= context->partnatts。
 *
 * partsupfunc 含区间分区比较函数，用来
 * 执行 partition_range_datum_bsearch 或
 * partition_rbound_datum_cmp。
 *
 * nullkeys 是为空的分区键集合。
 */
static PruneStepResult *
get_matching_range_bounds(PartitionPruneContext *context,
						  StrategyNumber opstrategy, Datum *values, int nvalues,
						  FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	Oid		   *partcollation = context->partcollation;
	int			partnatts = context->partnatts;
	int		   *partindices = boundinfo->indexes;
	int			off,
				minoff,
				maxoff;
	bool		is_equal;
	bool		inclusive = false;

	Assert(context->strategy == PARTITION_STRATEGY_RANGE);
	Assert(nvalues <= partnatts);

	result->scan_null = result->scan_default = false;

	/*
	 * 若没有可与键比较的 datum，或者我们得到了 IS NULL
	 * 子句，若存在默认分区就只返回它。
	 */
	if (boundinfo->ndatums == 0 || !bms_is_empty(nullkeys))
	{
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	minoff = 0;
	maxoff = boundinfo->ndatums;

	/*
	 * 若没有可与 boundinfo 里的 datum 比较的值，就
	 * 意味着调用者要求所有非空 datum 的分区。把
	 * *所有*分区的下标加进去，包括默认分区（若
	 * 存在一个）。
	 */
	if (nvalues == 0)
	{
		/* 忽略任何分区都未覆盖的键空间 */
		if (partindices[minoff] < 0)
			minoff++;
		if (partindices[maxoff] < 0)
			maxoff--;

		result->scan_default = partition_bound_has_default(boundinfo);
		Assert(partindices[minoff] >= 0 &&
			   partindices[maxoff] >= 0);
		result->bound_offsets = bms_add_range(NULL, minoff, maxoff);

		return result;
	}

	/*
	 * 若查询没有约束全部键列，就需要扫描
	 * 默认分区（若有）。
	 */
	if (nvalues < partnatts)
		result->scan_default = partition_bound_has_default(boundinfo);

	switch (opstrategy)
	{
		case BTEqualStrategyNumber:
			/* 寻找等于查找值的最小边界。 */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);

			if (off >= 0 && is_equal)
			{
				if (nvalues == partnatts)
				{
					/* 匹配的分区只能有零个或一个。 */
					result->bound_offsets = bms_make_singleton(off + 1);
					return result;
				}
				else
				{
					int			saved_off = off;

					/*
					 * 由于查找值只含键的前缀，
					 * 我们必须找出也可能匹配该
					 * 前缀的其他边界。partition_range_datum_bsearch() 返回
					 * 其中之一的偏移，通过检查相邻
					 * 边界来找出其余的。
					 */

					/*
					 * 先找出小于查找
					 * 值的最大边界。
					 */
					while (off >= 1)
					{
						int32		cmpval;

						cmpval =
							partition_rbound_datum_cmp(partsupfunc,
													   partcollation,
													   boundinfo->datums[off - 1],
													   boundinfo->kind[off - 1],
													   values, nvalues);
						if (cmpval != 0)
							break;
						off--;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					/*
					 * 若我们知道 off 是查找值
					 * 可能存在的那个分区的上界，就可以把 off 当作
					 * 要纳入结果的最小边界的偏移。
					 * 它不可能是的一种情形是：该边界，
					 * 或者精确地说其前缀中匹配的那一部分，
					 * 不是闭的。
					 */
					if (boundinfo->kind[off][nvalues] ==
						PARTITION_RANGE_DATUM_MINVALUE)
						off++;

					minoff = off;

					/*
					 * 现在找出大于查找
					 * 值的最小边界。
					 */
					off = saved_off;
					while (off < boundinfo->ndatums - 1)
					{
						int32		cmpval;

						cmpval = partition_rbound_datum_cmp(partsupfunc,
															partcollation,
															boundinfo->datums[off + 1],
															boundinfo->kind[off + 1],
															values, nvalues);
						if (cmpval != 0)
							break;
						off++;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					/*
					 * 那么 off + 1 就是要纳入结果的
					 * 最大边界的偏移。
					 */
					maxoff = off + 1;
				}

				Assert(minoff >= 0 && maxoff >= 0);
				result->bound_offsets = bms_add_range(NULL, minoff, maxoff);
			}
			else
			{
				/*
				 * 查找值落在 boundinfo 里某些边界之间的区间中。
				 * off 会是 <= 查找值的最大边界的
				 * 偏移，因此改为把 off + 1 加入结果，
				 * 作为可能含有查找值的唯一
				 * 分区的上界偏移。若 off
				 * 为 -1，表示所有边界都更大，那么我们只是
				 * 最终加入第一个边界的偏移，也就是 0。
				 */
				result->bound_offsets = bms_make_singleton(off + 1);
			}

			return result;

		case BTGreaterEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTGreaterStrategyNumber:

			/*
			 * 寻找 > 或 >= 查找值的最小边界，并
			 * 把它的偏移设为 minoff。
			 */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);
			if (off < 0)
			{
				/*
				 * 所有边界都大于查找值，因此把
				 * 它们全部纳入结果。
				 */
				minoff = 0;
			}
			else
			{
				if (is_equal && nvalues < partnatts)
				{
					/*
					 * 由于查找值只含键的前缀，
					 * 我们必须找出也可能匹配该
					 * 前缀的其他边界。partition_range_datum_bsearch() 返回
					 * 其中之一的偏移，通过检查相邻
					 * 边界来找出其余的。
					 *
					 * 根据查找值是否为闭区间，
					 * 我们必须要么把所有这些
					 * 边界的下标纳入结果（也就是把 minoff 设为
					 * 最小的那个边界的下标），要么找出大于查找值的
					 * 最小边界并把 minoff 设为它。
					 */
					while (off >= 1 && off < boundinfo->ndatums - 1)
					{
						int32		cmpval;
						int			nextoff;

						nextoff = inclusive ? off - 1 : off + 1;
						cmpval =
							partition_rbound_datum_cmp(partsupfunc,
													   partcollation,
													   boundinfo->datums[nextoff],
													   boundinfo->kind[nextoff],
													   values, nvalues);
						if (cmpval != 0)
							break;

						off = nextoff;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					minoff = inclusive ? off : off + 1;
				}
				else
				{

					/*
					 * 查找值落在 boundinfo 里某些边界之间的区间中。
					 * off 会是 <= 查找值的最大
					 * 边界的偏移，因此改为把 off + 1 加入
					 * 结果，作为可能含有查找值的最小
					 * 分区的上界偏移。
					 */
					minoff = off + 1;
				}
			}
			break;

		case BTLessEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTLessStrategyNumber:

			/*
			 * 寻找 < 或 <= 查找值的最大边界，并
			 * 把它的偏移设为 maxoff。
			 */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);
			if (off >= 0)
			{
				/*
				 * 见上面的注释。
				 */
				if (is_equal && nvalues < partnatts)
				{
					while (off >= 1 && off < boundinfo->ndatums - 1)
					{
						int32		cmpval;
						int			nextoff;

						nextoff = inclusive ? off + 1 : off - 1;
						cmpval = partition_rbound_datum_cmp(partsupfunc,
															partcollation,
															boundinfo->datums[nextoff],
															boundinfo->kind[nextoff],
															values, nvalues);
						if (cmpval != 0)
							break;

						off = nextoff;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					maxoff = inclusive ? off + 1 : off;
				}

				/*
				 * 查找值落在 boundinfo 里某些边界之间的区间中。
				 * off 会是 <= 查找值的最大边界的
				 * 偏移，因此改为把 off + 1 加入结果，
				 * 作为可能含有查找值的最大
				 * 分区的上界偏移。若查找
				 * 值恰好匹配了该边界，但它不是
				 * 闭的，就不必加入相邻分区。
				 */
				else if (!is_equal || inclusive)
					maxoff = off + 1;
				else
					maxoff = off;
			}
			else
			{
				/*
				 * off 为 -1，表示所有边界都更大，因此就把
				 * 第一个边界的偏移设为 maxoff。
				 */
				maxoff = off + 1;
			}
			break;

		default:
			elog(ERROR, "invalid strategy number %d", opstrategy);
			break;
	}

	Assert(minoff >= 0 && minoff <= boundinfo->ndatums);
	Assert(maxoff >= 0 && maxoff <= boundinfo->ndatums);

	/*
	 * 若要返回的最小分区以 MINVALUE（负无穷）作为
	 * 其下界，就把它递增，指向下一个有限边界
	 * （应当是它的上界），以免无意中
	 * 去扫描默认分区。
	 */
	if (minoff < boundinfo->ndatums && partindices[minoff] < 0)
	{
		int			lastkey = nvalues - 1;

		if (boundinfo->kind[minoff][lastkey] ==
			PARTITION_RANGE_DATUM_MINVALUE)
		{
			minoff++;
			Assert(boundinfo->indexes[minoff] >= 0);
		}
	}

	/*
	 * 若前一个最大分区的上界是 MAXVALUE（正无穷）
	 * （只有多列区间
	 * 分区才能这样），我们可以改用它作为要返回的最大
	 * 分区。同样是为了避免无意中去扫描
	 * 默认分区。
	 */
	if (maxoff >= 1 && partindices[maxoff] < 0)
	{
		int			lastkey = nvalues - 1;

		if (boundinfo->kind[maxoff - 1][lastkey] ==
			PARTITION_RANGE_DATUM_MAXVALUE)
		{
			maxoff--;
			Assert(boundinfo->indexes[maxoff] >= 0);
		}
	}

	Assert(minoff >= 0 && maxoff >= 0);
	if (minoff <= maxoff)
		result->bound_offsets = bms_add_range(NULL, minoff, maxoff);

	return result;
}

/*
 * pull_exec_paramids
 *		返回一个 Bitmapset，含 expr 里所有
 *		paramkind = PARAM_EXEC 的参数的 paramid。
 */
static Bitmapset *
pull_exec_paramids(Expr *expr)
{
	Bitmapset  *result = NULL;

	(void) pull_exec_paramids_walker((Node *) expr, &result);

	return result;
}

static bool
pull_exec_paramids_walker(Node *node, Bitmapset **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXEC)
			*context = bms_add_member(*context, param->paramid);
		return false;
	}
	return expression_tree_walker(node, pull_exec_paramids_walker,
								  (void *) context);
}

/*
 * get_partkey_exec_paramids
 *		遍历给定的剪枝步骤，找出用到了哪些执行期
 *		参数。
 *
 * 返回参数 ID 的 Bitmapset。
 */
static Bitmapset *
get_partkey_exec_paramids(List *steps)
{
	Bitmapset  *execparamids = NULL;
	ListCell   *lc;

	foreach(lc, steps)
	{
		PartitionPruneStepOp *step = (PartitionPruneStepOp *) lfirst(lc);
		ListCell   *lc2;

		if (!IsA(step, PartitionPruneStepOp))
			continue;

		foreach(lc2, step->exprs)
		{
			Expr	   *expr = lfirst(lc2);

			/* 对普通 Const 可以走快路径 */
			if (!IsA(expr, Const))
				execparamids = bms_join(execparamids,
										pull_exec_paramids(expr));
		}
	}

	return execparamids;
}

/*
 * perform_pruning_base_step
 *		确定满足 opstep 里指定条件的 datum
 *		下标。
 *
 * 结果还含是否需要扫描特殊的接受 NULL 分区和/或默认
 * 分区。
 */
static PruneStepResult *
perform_pruning_base_step(PartitionPruneContext *context,
						  PartitionPruneStepOp *opstep)
{
	ListCell   *lc1,
			   *lc2;
	int			keyno,
				nvalues;
	Datum		values[PARTITION_MAX_KEYS];
	FmgrInfo   *partsupfunc;
	int			stateidx;

	/*
	 * 表达式和比较函数的个数最好相同。
	 */
	Assert(list_length(opstep->exprs) == list_length(opstep->cmpfns));

	nvalues = 0;
	lc1 = list_head(opstep->exprs);
	lc2 = list_head(opstep->cmpfns);

	/*
	 * 生成下面将调用的某个 get_matching_*_bounds 函数
	 * 要用的分区查找键。
	 */
	for (keyno = 0; keyno < context->partnatts; keyno++)
	{
		/*
		 * 对哈希分区，某些键的值可能
		 * 没有在操作符子句里提供，而是规划器发现
		 * 它们出现在 IS NULL 子句里。
		 */
		if (bms_is_member(keyno, opstep->nullkeys))
			continue;

		/*
		 * 对区间分区，我们必须只用全部分区键的值，
		 * 或者其前缀的值来剪枝。
		 */
		if (keyno > nvalues && context->strategy == PARTITION_STRATEGY_RANGE)
			break;

		if (lc1 != NULL)
		{
			Expr	   *expr;
			Datum		datum;
			bool		isnull;
			Oid			cmpfn;

			expr = lfirst(lc1);
			stateidx = PruneCxtStateIdx(context->partnatts,
										opstep->step.step_id, keyno);
			partkey_datum_from_expr(context, expr, stateidx,
									&datum, &isnull);

			/*
			 * 由于剪枝步骤里只允许严格操作符，任何
			 * 空的比较值都必然使比较失败，
			 * 从而没有任何分区能够匹配。
			 */
			if (isnull)
			{
				PruneStepResult *result;

				result = (PruneStepResult *) palloc(sizeof(PruneStepResult));
				result->bound_offsets = NULL;
				result->scan_default = false;
				result->scan_null = false;

				return result;
			}

			/* 设置 stepcmpfuncs 项，除非已经设置过 */
			cmpfn = lfirst_oid(lc2);
			Assert(OidIsValid(cmpfn));
			if (cmpfn != context->stepcmpfuncs[stateidx].fn_oid)
			{
				/*
				 * 若所需的支持函数就是缓存在
				 * 该关系分区键里的那个，就复制缓存的 FmgrInfo。
				 * 否则（也就是跨类型比较时），
				 * 需要做一次实际查找。
				 */
				if (cmpfn == context->partsupfunc[keyno].fn_oid)
					fmgr_info_copy(&context->stepcmpfuncs[stateidx],
								   &context->partsupfunc[keyno],
								   context->ppccontext);
				else
					fmgr_info_cxt(cmpfn, &context->stepcmpfuncs[stateidx],
								  context->ppccontext);
			}

			values[keyno] = datum;
			nvalues++;

			lc1 = lnext(opstep->exprs, lc1);
			lc2 = lnext(opstep->cmpfns, lc2);
		}
	}

	/*
	 * 让 partsupfunc 指向该步骤第 0 个键的项；若还有
	 * 其他支持函数，就紧接着连续排列。
	 */
	stateidx = PruneCxtStateIdx(context->partnatts, opstep->step.step_id, 0);
	partsupfunc = &context->stepcmpfuncs[stateidx];

	switch (context->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			return get_matching_hash_bounds(context,
											opstep->opstrategy,
											values, nvalues,
											partsupfunc,
											opstep->nullkeys);

		case PARTITION_STRATEGY_LIST:
			return get_matching_list_bounds(context,
											opstep->opstrategy,
											values[0], nvalues,
											&partsupfunc[0],
											opstep->nullkeys);

		case PARTITION_STRATEGY_RANGE:
			return get_matching_range_bounds(context,
											 opstep->opstrategy,
											 values, nvalues,
											 partsupfunc,
											 opstep->nullkeys);

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) context->strategy);
			break;
	}

	return NULL;
}

/*
 * perform_pruning_combine_step
 *		用指定的组合方法，确定把 cstep->source_stepids
 *		所标识的步骤给出的那些 datum 组合起来之后
 *		得到的下标。
 *
 * 由于 cstep 可能引用更早步骤的结果，这里也接收
 * step_results。
 */
static PruneStepResult *
perform_pruning_combine_step(PartitionPruneContext *context,
							 PartitionPruneStepCombine *cstep,
							 PruneStepResult **step_results)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	bool		firststep;
	ListCell   *lc1;

	/*
	 * 没有源步骤的组合步骤表示不要做
	 * 任何分区剪枝。那种情况下返回全部 datum 下标。
	 */
	if (cstep->source_stepids == NIL)
	{
		PartitionBoundInfo boundinfo = context->boundinfo;

		result->bound_offsets =
			bms_add_range(NULL, 0, boundinfo->nindexes - 1);
		result->scan_default = partition_bound_has_default(boundinfo);
		result->scan_null = partition_bound_accepts_nulls(boundinfo);
		return result;
	}

	switch (cstep->combineOp)
	{
		case PARTPRUNE_COMBINE_UNION:
			foreach(lc1, cstep->source_stepids)
			{
				int			step_id = lfirst_int(lc1);
				PruneStepResult *step_result;

				/*
				 * step_results[step_id] 必须含有有效结果，这一点由
				 * cstep 的 step_id 大于
				 * step_id、以及各个步骤的结果
				 * 按其 step_ids 的顺序求值这两点来确认。
				 */
				if (step_id >= cstep->step.step_id)
					elog(ERROR, "invalid pruning combine step argument");
				step_result = step_results[step_id];
				Assert(step_result != NULL);

				/* 记录来自该步骤的任何额外 datum 下标 */
				result->bound_offsets = bms_add_members(result->bound_offsets,
														step_result->bound_offsets);

				/* 更新是否扫描空分区和默认分区。 */
				if (!result->scan_null)
					result->scan_null = step_result->scan_null;
				if (!result->scan_default)
					result->scan_default = step_result->scan_default;
			}
			break;

		case PARTPRUNE_COMBINE_INTERSECT:
			firststep = true;
			foreach(lc1, cstep->source_stepids)
			{
				int			step_id = lfirst_int(lc1);
				PruneStepResult *step_result;

				if (step_id >= cstep->step.step_id)
					elog(ERROR, "invalid pruning combine step argument");
				step_result = step_results[step_id];
				Assert(step_result != NULL);

				if (firststep)
				{
					/* 第一次时复制该步骤的结果。 */
					result->bound_offsets =
						bms_copy(step_result->bound_offsets);
					result->scan_null = step_result->scan_null;
					result->scan_default = step_result->scan_default;
					firststep = false;
				}
				else
				{
					/* 记录两个步骤共有的 datum 下标 */
					result->bound_offsets =
						bms_int_members(result->bound_offsets,
										step_result->bound_offsets);

					/* 更新是否扫描空分区和默认分区。 */
					if (result->scan_null)
						result->scan_null = step_result->scan_null;
					if (result->scan_default)
						result->scan_default = step_result->scan_default;
				}
			}
			break;
	}

	return result;
}

/*
 * match_boolean_partition_clause
 *
 * 若我们能把该子句作为特殊形状的
 * 布尔子句匹配到分区键，就把 *outconst 设为含真值或假值的 Const，
 * 按子句是否为否定形式（即 is not
 * true 或 is not false）设置 *noteq，并返回 PARTCLAUSE_MATCH_CLAUSE。若子句不是布尔子句，或该
 * 布尔子句不适合分区剪枝，则返回
 * PARTCLAUSE_UNSUPPORTED。若它是布尔限定条件但就是不匹配该
 * 分区键，则返回 PARTCLAUSE_NOMATCH。后两种情形把
 * *outconst 设为 NULL。
 */
static PartClauseMatchStatus
match_boolean_partition_clause(Oid partopfamily, Expr *clause, Expr *partkey,
							   Expr **outconst, bool *noteq)
{
	Expr	   *leftop;

	*outconst = NULL;
	*noteq = false;

	/*
	 * 分区目前只能使用内建访问方法，因此检查
	 * 内建布尔操作符族就足够了。
	 */
	if (!IsBuiltinBooleanOpfamily(partopfamily))
		return PARTCLAUSE_UNSUPPORTED;

	if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		/* 只有 IS [NOT] TRUE/FALSE 对我们有用 */
		if (btest->booltesttype == IS_UNKNOWN ||
			btest->booltesttype == IS_NOT_UNKNOWN)
			return PARTCLAUSE_UNSUPPORTED;

		leftop = btest->arg;
		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		if (equal(leftop, partkey))
		{
			switch (btest->booltesttype)
			{
				case IS_NOT_TRUE:
					*noteq = true;
					/* fall through */
				case IS_TRUE:
					*outconst = (Expr *) makeBoolConst(true, false);
					break;
				case IS_NOT_FALSE:
					*noteq = true;
					/* fall through */
				case IS_FALSE:
					*outconst = (Expr *) makeBoolConst(false, false);
					break;
				default:
					return PARTCLAUSE_UNSUPPORTED;
			}
		}
		if (*outconst)
			return PARTCLAUSE_MATCH_CLAUSE;
	}
	else
	{
		bool		is_not_clause = is_notclause(clause);

		leftop = is_not_clause ? get_notclausearg(clause) : clause;

		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		/* 与分区键比较，并编造一个子句…… */
		if (equal(leftop, partkey))
			*outconst = (Expr *) makeBoolConst(!is_not_clause, false);
		else if (equal(negate_clause((Node *) leftop), partkey))
			*outconst = (Expr *) makeBoolConst(is_not_clause, false);

		if (*outconst)
			return PARTCLAUSE_MATCH_CLAUSE;
	}

	return PARTCLAUSE_NOMATCH;
}

/*
 * partkey_datum_from_expr
 *		为可能的分区剪枝对表达式求值。
 *
 * 对 expr 求值；把 *value 和 *isnull 设为得到的 Datum 和空标志。
 *
 * 若 expr 不是 Const，其 ExprState 在 context
 * 表达式状态数组的 stateidx 处。
 *
 * 注意，求值结果可能位于 context->exprcontext 的
 * 按元组内存上下文里，我们也可能在那里泄漏了其他内存。
 * 这次剪枝操作结束后，必须通过重置该 ExprContext 来回收
 * 这些内存（见 execPartition.c）。
 */
static void
partkey_datum_from_expr(PartitionPruneContext *context,
						Expr *expr, int stateidx,
						Datum *value, bool *isnull)
{
	if (IsA(expr, Const))
	{
		/* 我们总是能确定常量的值 */
		Const	   *con = (Const *) expr;

		*value = con->constvalue;
		*isnull = con->constisnull;
	}
	else
	{
		ExprState  *exprstate;
		ExprContext *ectx;

		/*
		 * 除非调用者传入了有效的 ExprContext，否则步骤里不应当出现
		 * 非 Const。
		 *
		 * 当 context->planstate 有效时，context->exprcontext 与
		 * context->planstate->ps_ExprContext 相同。
		 */
		Assert(context->planstate != NULL || context->exprcontext != NULL);
		Assert(context->planstate == NULL ||
			   (context->exprcontext == context->planstate->ps_ExprContext));

		exprstate = context->exprstates[stateidx];
		ectx = context->exprcontext;
		*value = ExecEvalExprSwitchContext(exprstate, ectx, isnull);
	}
}
