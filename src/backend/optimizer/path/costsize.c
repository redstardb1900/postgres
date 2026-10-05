/*-------------------------------------------------------------------------
 *
 * costsize.c
 *	  计算（并设置）关系大小与路径代价的例程
 *
 * 路径代价用任意单位度量，这些单位由下列基本
 * 参数确立：
 *
 *	seq_page_cost		顺序读取一页的代价
 *	random_page_cost	非顺序读取一页的代价
 *	cpu_tuple_cost		处理一个元组的典型 CPU 时间代价
 *	cpu_index_tuple_cost  处理一个索引元组的典型 CPU 时间代价
 *	cpu_operator_cost	执行一个操作符或函数的 CPU 时间代价
 *	parallel_tuple_cost 把一个元组从工作进程传给领导进程的 CPU 时间代价
 *	parallel_setup_cost 为并行设置共享内存的代价
 *
 * 我们预期内核通常会做一定程度的预读
 * 优化；再加上寻道代价，这意味着 seq_page_cost
 * 通常远小于 random_page_cost。（不过，若
 * 数据库已完全缓存在内存中，把二者设成相等是合理的。）
 *
 * 我们还用粗略估计 "effective_cache_size" 表示
 * Postgres 与操作系统级磁盘缓存中的磁盘页数。（不能简单地用
 * NBuffers 来充当，因为那会忽略
 * 内核磁盘缓存的影响。）
 *
 * 显然，把这些值取成常量是一种过度简化，
 * 但即便在这个细节层次上，要得到有用的估计也已经
 * 够难了。注意这些参数都可以由用户设置，以防
 * 默认值对某个特定平台严重偏离。
 *
 * 也可以为单个表空间覆盖 seq_page_cost 与 random_page_cost，
 * 以防有的数据在快速磁盘上，有的数据在慢速
 * 磁盘上。按表空间的覆盖从不应用于临时工作文件，例如
 * 外部排序，或溢出 work_mem 的物化节点。
 *
 * 我们为每条路径计算两种代价：
 *		total_cost：取出全部元组的总估计代价
 *		startup_cost：取出第一个元组之前付出的代价
 * 在某些情形下，例如存在 LIMIT，或我们正在实现
 * EXISTS(...) 子查询时，不必取出该路径结果的全部元组。
 * 调用者可以估计取出部分结果的代价，办法是在 startup_cost 与
 * total_cost 之间插值。具体而言：
 *		actual_cost = startup_cost +
 *			(total_cost - startup_cost) * tuples_to_fetch / path->rows;
 * 注意，基关系的行数（进而 LIMIT 节点之下计划节点的 plan_rows）
 * 在设置时不考虑任何 LIMIT，这样
 * 这个等式才能成立。（注意：普通关系的 path->rows 从不为零，
 * 但可证明为空的关系，其路径上该值为零，
 * 因此要当心除以零。）LIMIT 作为顶层
 * 计划节点施加。
 *
 * 主要出于历史原因，本模块的大多数例程只用
 * 传入的结果 Path 来存放结果（rows、startup_cost 与
 * total_cost）。它们需要的全部输入数据都作为单独的
 * 参数传入，尽管其中很多可以从 Path 里取出。
 * cost_XXXjoin() 例程是例外，它们期望传入的 XXXPath 的
 * 其他字段都已填好；类似地，
 * cost_index() 假定传入的 IndexPath 除输出
 * 值之外都是有效的。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/costsize.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/tsmapi.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeMemoize.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/tuplesort.h"


#define LOG2(x)  (log(x) / 0.693147180559945)

/*
 * Append 和 MergeAppend 节点比另一些使用 cpu_tuple_cost 的操作更便宜；
 * 与其另加一个 GUC，不如把
 * 每元组代价估计为 cpu_tuple_cost 乘以这个值。
 */
#define APPEND_CPU_COST_MULTIPLIER 0.5

/*
 * 行数估计的最大值。我们把行数估计封顶到这个值，以帮助
 * 确保基于这些估计的代价仍处在
 * double 能表示的范围内。若代价是无穷或 NaN，add_path()
 * 不会表现正常。
 */
#define MAXIMUM_ROWCOUNT 1e100

double		seq_page_cost = DEFAULT_SEQ_PAGE_COST;
double		random_page_cost = DEFAULT_RANDOM_PAGE_COST;
double		cpu_tuple_cost = DEFAULT_CPU_TUPLE_COST;
double		cpu_index_tuple_cost = DEFAULT_CPU_INDEX_TUPLE_COST;
double		cpu_operator_cost = DEFAULT_CPU_OPERATOR_COST;
double		parallel_tuple_cost = DEFAULT_PARALLEL_TUPLE_COST;
double		parallel_setup_cost = DEFAULT_PARALLEL_SETUP_COST;
double		recursive_worktable_factor = DEFAULT_RECURSIVE_WORKTABLE_FACTOR;

int			effective_cache_size = DEFAULT_EFFECTIVE_CACHE_SIZE;

Cost		disable_cost = 1.0e10;

int			max_parallel_workers_per_gather = 2;

bool		enable_seqscan = true;
bool		enable_indexscan = true;
bool		enable_indexonlyscan = true;
bool		enable_bitmapscan = true;
bool		enable_tidscan = true;
bool		enable_sort = true;
bool		enable_incremental_sort = true;
bool		enable_hashagg = true;
bool		enable_nestloop = true;
bool		enable_material = true;
bool		enable_memoize = true;
bool		enable_mergejoin = true;
bool		enable_hashjoin = true;
bool		enable_gathermerge = true;
bool		enable_partitionwise_join = false;
bool		enable_partitionwise_aggregate = false;
bool		enable_parallel_append = true;
bool		enable_parallel_hash = true;
bool		enable_partition_pruning = true;
bool		enable_presorted_aggregate = true;
bool		enable_async_append = true;

typedef struct
{
	PlannerInfo *root;
	QualCost	total;
} cost_qual_eval_context;

static List *extract_nonindex_conditions(List *qual_clauses, List *indexclauses);
static MergeScanSelCache *cached_scansel(PlannerInfo *root,
										 RestrictInfo *rinfo,
										 PathKey *pathkey);
static void cost_rescan(PlannerInfo *root, Path *path,
						Cost *rescan_startup_cost, Cost *rescan_total_cost);
static bool cost_qual_eval_walker(Node *node, cost_qual_eval_context *context);
static void get_restriction_qual_cost(PlannerInfo *root, RelOptInfo *baserel,
									  ParamPathInfo *param_info,
									  QualCost *qpqual_cost);
static bool has_indexed_join_quals(NestPath *path);
static double approx_tuple_count(PlannerInfo *root, JoinPath *path,
								 List *quals);
static double calc_joinrel_size_estimate(PlannerInfo *root,
										 RelOptInfo *joinrel,
										 RelOptInfo *outer_rel,
										 RelOptInfo *inner_rel,
										 double outer_rows,
										 double inner_rows,
										 SpecialJoinInfo *sjinfo,
										 List *restrictlist);
static Selectivity get_foreign_key_join_selectivity(PlannerInfo *root,
													Relids outer_relids,
													Relids inner_relids,
													SpecialJoinInfo *sjinfo,
													List **restrictlist);
static Cost append_nonpartial_cost(List *subpaths, int numpaths,
								   int parallel_workers);
static void set_rel_width(PlannerInfo *root, RelOptInfo *rel);
static int32 get_expr_width(PlannerInfo *root, const Node *expr);
static double relation_byte_size(double tuples, int width);
static double page_size(double tuples, int width);
static double get_parallel_divisor(Path *path);


/*
 * clamp_row_est
 *		把行数估计强制成一个合理的值。
 */
double
clamp_row_est(double nrows)
{
	/*
	 * 避免无穷和 NaN 行数估计。由这类值导出的代价
	 * 将没有用。还把估计强制为至少一行，
	 * 好让 explain 输出好看些，并避免插值代价时可能的
	 * 除零。也把它变成整数。
	 */
	if (nrows > MAXIMUM_ROWCOUNT || isnan(nrows))
		nrows = MAXIMUM_ROWCOUNT;
	else if (nrows <= 1.0)
		nrows = 1.0;
	else
		nrows = rint(nrows);

	return nrows;
}

/*
 * clamp_cardinality_to_long
 *		把 Cardinality 值转换成一个合理的 long 值。
 */
long
clamp_cardinality_to_long(Cardinality x)
{
	/*
	 * 只是为了偏执，确保对负数或
	 * NaN 值做点合理的事。
	 */
	if (isnan(x))
		return LONG_MAX;
	if (x <= 0)
		return 0;

	/*
	 * 若 "long" 是 64 位，则 LONG_MAX 无法被精确表示为
	 * double。把它转成 double 再转回来，很可能会因
	 * 舍入而溢出，因此避免那样做。我们相信任何严格小于
	 * "(double) LONG_MAX" 的 double 值都能转成
	 * 可表示的 "long" 值。
	 */
	return (x < (double) LONG_MAX) ? (long) x : LONG_MAX;
}


/*
 * cost_seqscan
 *	  确定并返回顺序扫描一个关系的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_seqscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		cpu_run_cost;
	Cost		disk_run_cost;
	double		spc_seq_page_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	if (!enable_seqscan)
		startup_cost += disable_cost;

	/* 取包含该表的表空间的估计页代价 */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);

	/*
	 * 磁盘代价
	 */
	disk_run_cost = spc_seq_page_cost * baserel->pages;

	/* CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	cpu_run_cost = cpu_per_tuple * baserel->tuples;
	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	cpu_run_cost += path->pathtarget->cost.per_tuple * path->rows;

	/* 若使用了并行，就调整计价。 */
	if (path->parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(path);

		/* CPU 代价分摊到所有工作进程。 */
		cpu_run_cost /= parallel_divisor;

		/*
		 * 也许可以把一部分 I/O 代价摊销掉，但大概
		 * 摊不了多少，因为多数操作系统已经在积极
		 * 预取。目前假定磁盘运行代价完全
		 * 无法摊销。
		 */

		/*
		 * 对并行计划，行数需要表示
		 * 每个工作进程处理的元组数。
		 */
		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + cpu_run_cost + disk_run_cost;
}

/*
 * cost_samplescan
 *	  确定并返回用采样扫描一个关系的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_samplescan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	RangeTblEntry *rte;
	TableSampleClause *tsc;
	TsmRoutine *tsm;
	double		spc_seq_page_cost,
				spc_random_page_cost,
				spc_page_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于带 tablesample 子句的基关系 */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);
	tsm = GetTsmRoutine(tsc->tsmhandler);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 取包含该表的表空间的估计页代价 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/* 若使用 NextSampleBlock，假定随机访问，否则假定顺序访问 */
	spc_page_cost = (tsm->NextSampleBlock != NULL) ?
		spc_random_page_cost : spc_seq_page_cost;

	/*
	 * 磁盘代价（记住 baserel->pages 已经被设成
	 * 采样方法将访问的页数）
	 */
	run_cost += spc_page_cost * baserel->pages;

	/*
	 * CPU 代价（记住 baserel->tuples 已经被设成
	 * 采样方法将选出的元组数）。注意我们忽略
	 * TABLESAMPLE 参数表达式的执行代价；它们每次扫描
	 * 只求值一次，而且在多数用法里它们大概
	 * 反正是简单常量。我们也不为采样方法内部可能做的
	 * 计算收取任何费用。
	 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;
	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_gather
 *	  确定并返回 gather 路径的代价。
 *
 * 'rel' 是要操作的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 * 'rows' 可以指向一个行数估计；若非 NULL，它覆盖
 * 'rel' 和 'param_info' 两者。当路径并不恰好
 * 对应任何一个 RelOptInfo 时，这很有用。
 */
void
cost_gather(GatherPath *path, PlannerInfo *root,
			RelOptInfo *rel, ParamPathInfo *param_info,
			double *rows)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;

	/* 用正确的行数估计标记该路径 */
	if (rows)
		path->path.rows = *rows;
	else if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = rel->rows;

	startup_cost = path->subpath->startup_cost;

	run_cost = path->subpath->total_cost - path->subpath->startup_cost;

	/* 并行建立和通信代价。 */
	startup_cost += parallel_setup_cost;
	run_cost += parallel_tuple_cost * path->path.rows;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = (startup_cost + run_cost);
}

/*
 * cost_gather_merge
 *	  确定并返回 gather merge 路径的代价。
 *
 * GatherMerge 用一个堆归并若干已预排序的输入流，该堆在
 * 任一时刻保存每个流的下一个元组。若有 N
 * 个流，启动时构造堆大约需要 N*log2(N) 次元组比较，
 * 然后对每个输出元组，大约需要 log2(N) 次比较来
 * 用同一流的下一个元组替换堆顶项。
 */
void
cost_gather_merge(GatherMergePath *path, PlannerInfo *root,
				  RelOptInfo *rel, ParamPathInfo *param_info,
				  Cost input_startup_cost, Cost input_total_cost,
				  double *rows)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		comparison_cost;
	double		N;
	double		logN;

	/* 用正确的行数估计标记该路径 */
	if (rows)
		path->path.rows = *rows;
	else if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = rel->rows;

	if (!enable_gathermerge)
		startup_cost += disable_cost;

	/*
	 * 把工作进程数加一，以计入主导进程。这也许
	 * 偏慷慨，因为主导进程在典型情况下比其他工作进程做的工作少，
	 * 但目前就这样。
	 */
	Assert(path->num_workers > 0);
	N = (double) path->num_workers + 1;
	logN = LOG2(N);

	/* 假定的每次元组比较代价 */
	comparison_cost = 2.0 * cpu_operator_cost;

	/* 建堆代价 */
	startup_cost += comparison_cost * N * logN;

	/* 每元组的堆维护代价 */
	run_cost += path->path.rows * comparison_cost * logN;

	/* 堆管理的小代价，类似 cost_merge_append */
	run_cost += cpu_operator_cost * path->path.rows;

	/*
	 * 并行建立和通信代价。与 Gather 不同，Gather Merge
	 * 需要我们阻塞到每个
	 * 工作进程都有元组可用，因此与 Gather 相比把 IPC 代价略微抬高。
	 * 没有更好的办法，就额外收取 5%。
	 */
	startup_cost += parallel_setup_cost;
	run_cost += parallel_tuple_cost * path->path.rows * 1.05;

	path->path.startup_cost = startup_cost + input_startup_cost;
	path->path.total_cost = (startup_cost + run_cost + input_total_cost);
}

/*
 * cost_index
 *	  确定并返回用索引扫描一个关系的代价。
 *
 * 'path' 描述正在考虑的索引扫描，除了本例程要设置的字段之外，
 *		它是完整的
 * 'loop_count' 是要计入缓存行为估计的
 *		索引扫描重复次数
 *
 * 除了行数、startup_cost 和 total_cost，cost_index() 还设置
 * 路径的 indextotalcost 和 indexselectivity 字段。若 IndexPath 用于 BitmapIndexScan，
 * 就需要这些值。
 *
 * 注意：path->indexquals 只能含有可用作索引
 * 限制的子句。作为 qpqual 求值的任何额外限定条件可能减少
 * 返回的元组数，但不会减少必须从表中取出的元组数，
 * 因此它们不降低扫描代价。
 */
void
cost_index(IndexPath *path, PlannerInfo *root, double loop_count,
		   bool partial_path)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	bool		indexonly = (path->path.pathtype == T_IndexOnlyScan);
	amcostestimate_function amcostestimate;
	List	   *qpquals;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_run_cost = 0;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation,
				csquared;
	double		spc_seq_page_cost,
				spc_random_page_cost;
	Cost		min_IO_cost,
				max_IO_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	double		tuples_fetched;
	double		pages_fetched;
	double		rand_heap_pages;
	double		index_pages;

	/* 只应当用于基关系 */
	Assert(IsA(baserel, RelOptInfo) &&
		   IsA(index, IndexOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/*
	 * 用正确的行数估计标记该路径，并识别哪些限定条件
	 * 需要作为 qpqual 强制执行。不必检查任何由
	 * 索引谓词蕴含的限定条件，因此可以用 indrestrictinfo 而不是
	 * baserestrictinfo 作为该关系相关限制子句的
	 * 列表。
	 */
	if (path->path.param_info)
	{
		path->path.rows = path->path.param_info->ppi_rows;
		/* qpqual 来自该关系的限制子句和 ppi_clauses */
		qpquals = list_concat(extract_nonindex_conditions(path->indexinfo->indrestrictinfo,
														  path->indexclauses),
							  extract_nonindex_conditions(path->path.param_info->ppi_clauses,
														  path->indexclauses));
	}
	else
	{
		path->path.rows = baserel->rows;
		/* qpqual 只来自该关系的限制子句 */
		qpquals = extract_nonindex_conditions(path->indexinfo->indrestrictinfo,
											  path->indexclauses);
	}

	if (!enable_indexscan)
		startup_cost += disable_cost;
	/* 不必检查 enable_indexonlyscan；indxpath.c 会做那件事 */

	/*
	 * 调用特定于索引访问方法的代码，估计扫描索引的处理代价，
	 * 以及索引的选择率（即
	 * 我们必须检索的主表元组所占比例）和它与
	 * 主表元组顺序的相关性。这里需要一次转换，因为
	 * pathnodes.h 用弱函数类型来避免包含 amapi.h。
	 */
	amcostestimate = (amcostestimate_function) index->amcostestimate;
	amcostestimate(root, path, loop_count,
				   &indexStartupCost, &indexTotalCost,
				   &indexSelectivity, &indexCorrelation,
				   &index_pages);

	/*
	 * 保存 amcostestimate 的结果，以备位图扫描规划使用。
	 * 不费心保存 indexStartupCost 或 indexCorrelation，因为
	 * 位图扫描不关心这两者。
	 */
	path->indextotalcost = indexTotalCost;
	path->indexselectivity = indexSelectivity;

	/* 触及索引本身的全部代价都算在这里 */
	startup_cost += indexStartupCost;
	run_cost += indexTotalCost - indexStartupCost;

	/* 估计取出的主表元组数 */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	/* 取包含该表的表空间的估计页代价 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*----------
	 * Estimate number of main-table pages fetched, and compute I/O cost.
	 *
	 * When the index ordering is uncorrelated with the table ordering,
	 * we use an approximation proposed by Mackert and Lohman (see
	 * index_pages_fetched() for details) to compute the number of pages
	 * fetched, and then charge spc_random_page_cost per page fetched.
	 *
	 * When the index ordering is exactly correlated with the table ordering
	 * (just after a CLUSTER, for example), the number of pages fetched should
	 * be exactly selectivity * table_size.  What's more, all but the first
	 * will be sequential fetches, not the random fetches that occur in the
	 * uncorrelated case.  So if the number of pages is more than 1, we
	 * ought to charge
	 *		spc_random_page_cost + (pages_fetched - 1) * spc_seq_page_cost
	 * For partially-correlated indexes, we ought to charge somewhere between
	 * these two estimates.  We currently interpolate linearly between the
	 * estimates based on the correlation squared (XXX is that appropriate?).
	 *
	 * If it's an index-only scan, then we will not need to fetch any heap
	 * pages for which the visibility map shows all tuples are visible.
	 * Hence, reduce the estimated number of heap fetches accordingly.
	 * We use the measured fraction of the entire heap that is all-visible,
	 * which might not be particularly relevant to the subset of the heap
	 * that this query will fetch; but it's not clear how to do better.
	 *----------
	 */
	if (loop_count > 1)
	{
		/*
		 * 对重复的索引扫描，不相关情形的适当估计是
		 * 按扫描次数放大 Mackert 和 Lohman 公式中取出的元组数，
		 * 以便估计
		 * 全部扫描取出的页数；然后
		 * 按比例分摊一次扫描的代价。这种情况下假定所有
		 * 取出都是随机访问。
		 */
		pages_fetched = index_pages_fetched(tuples_fetched * loop_count,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		rand_heap_pages = pages_fetched;

		max_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;

		/*
		 * 在完全相关的情形里，每次扫描触及的页数是
		 * 选择率乘以 table_size，可以在页这一层使用 Mackert
		 * 和 Lohman 公式来估计跨扫描缓存节省了多少工作。
		 * 不过仍假定所有取出都是
		 * 随机的，这是一个很难纠正的高估，
		 * 否则会把缓存效应算两次。（但在多数
		 * 这种计划真正有意义的情形里，每次扫描反正只会取出
		 * 一页，所以关系不大。）
		 */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

		pages_fetched = index_pages_fetched(pages_fetched * loop_count,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		min_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;
	}
	else
	{
		/*
		 * 普通情形：应用 Mackert 和 Lohman 公式，然后
		 * 在它和由相关性导出的结果之间插值。
		 */
		pages_fetched = index_pages_fetched(tuples_fetched,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		rand_heap_pages = pages_fetched;

		/* max_IO_cost 用于完全不相关的情形（csquared=0） */
		max_IO_cost = pages_fetched * spc_random_page_cost;

		/* min_IO_cost 用于完全相关的情形（csquared=1） */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		if (pages_fetched > 0)
		{
			min_IO_cost = spc_random_page_cost;
			if (pages_fetched > 1)
				min_IO_cost += (pages_fetched - 1) * spc_seq_page_cost;
		}
		else
			min_IO_cost = 0;
	}

	if (partial_path)
	{
		/*
		 * 对仅索引扫描，根据取出的索引页数计算工作进程；
		 * 我们取出的堆页数可能小到
		 * 实际上排除并行，而我们不想那样。
		 */
		if (indexonly)
			rand_heap_pages = -1;

		/*
		 * 估计扫描索引所需的并行工作进程数。使用
		 * 考虑到堆取出不会是
		 * 顺序的页数，因为并行扫描以随机
		 * 顺序访问页。
		 */
		path->path.parallel_workers = compute_parallel_worker(baserel,
															  rand_heap_pages,
															  index_pages,
															  max_parallel_workers_per_gather);

		/*
		 * 若无法为并行扫描分配工作进程，就退出，因为
		 * 这种情况下这条路径会被拒绝。多做
		 * 额外计算没有好处。
		 */
		if (path->path.parallel_workers <= 0)
			return;

		path->path.parallel_aware = true;
	}

	/*
	 * 现在根据估计的索引顺序相关性插值，得到主表访问的总
	 * 磁盘 I/O 代价。
	 */
	csquared = indexCorrelation * indexCorrelation;

	run_cost += max_IO_cost + csquared * (min_IO_cost - max_IO_cost);

	/*
	 * 估计每个元组的 CPU 代价。
	 *
	 * 这里要的是 cpu_tuple_cost 加上任何
	 * 必须作为 qpqual 求值的限定子句的求值代价。
	 */
	cost_qual_eval(&qpqual_cost, qpquals, root);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;

	cpu_run_cost += cpu_per_tuple * tuples_fetched;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->path.pathtarget->cost.startup;
	cpu_run_cost += path->path.pathtarget->cost.per_tuple * path->path.rows;

	/* 若使用了并行，就调整计价。 */
	if (path->path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->path);

		path->path.rows = clamp_row_est(path->path.rows / parallel_divisor);

		/* CPU 代价分摊到所有工作进程。 */
		cpu_run_cost /= parallel_divisor;
	}

	run_cost += cpu_run_cost;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}

/*
 * extract_nonindex_conditions
 *
 * 给定一份要在索引扫描中强制执行的限定条件列表，抽出那些
 * 必须作为 qpqual 应用的（即索引机制不会处理
 * 它们）。这里只检测一条限定子句是否与某条索引子句直接冗余。
 * 若选中该索引路径来用，createplan.c
 * 会更努力地去掉冗余限定条件；具体说
 * 它会看限定条件能否被证明由索引限定条件蕴含。但
 * 在这个阶段把那一点算进去似乎不值得花周期，
 * 因为我们只是在估计限定条件求值代价。除此之外，这必须
 * 与 create_indexscan_plan() 中的逻辑匹配。
 *
 * qual_clauses 和结果都是 RestrictInfos 的列表。
 * indexclauses 是 IndexClauses 的列表。
 */
static List *
extract_nonindex_conditions(List *qual_clauses, List *indexclauses)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, qual_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			continue;			/* 这里可以丢掉伪常量 */
		if (is_redundant_with_indexclauses(rinfo, indexclauses))
			continue;			/* 重复，或派生自同一个 EquivalenceClass */
		/* ……跳过 createplan.c 会尝试的谓词证明…… */
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * index_pages_fetched
 *	  在计入缓存效应之后，估计实际取出的页数。
 *	  缓存效应。
 *
 * 我们使用 Mackert 和 Lohman 提出的近似，"Index Scans
 * Using a Finite LRU Buffer: A Validated I/O Model"，ACM Transactions
 * on Database Systems，第 14 卷，第 3 期，1989 年 9 月，第 401-424 页。
 * Mackert 和 Lohman 近似是：取出的页数
 * 为
 *	PF =
 *		min(2TNs/(2T+Ns), T)			当 T <= b
 *		2TNs/(2T+Ns)					当 T > b 且 Ns <= 2Tb/(2T-b)
 *		b + (Ns - 2Tb/(2T-b))*(T-b)/T	当 T > b 且 Ns > 2Tb/(2T-b)
 * 其中
 *		T = 表中的页数
 *		N = 表中的元组数
 *		s = 选择率 = 要扫描的表所占比例
 *		b = 可用缓冲页数（这里包括内核空间）
 *
 * 假定 effective_cache_size 是整个查询可用的缓冲页总数，
 * 并把该空间按比例分给查询中的全部
 * 表和当前考虑的索引。（这
 * 忽略了查询使用的其他索引所需的空间，但既然我们
 * 不知道哪些索引会被用到，就无法把这一点估计得很好；
 * 而且把所有表都算上很可能是高估，因为
 * 取决于连接计划，并非所有表都会被并发扫描。）
 *
 * 乘积 Ns 是取出的元组数；我们传入该
 * 乘积，而不是在这里计算它。"pages" 是
 * 所考虑对象（索引或表）中的页数。
 * "index_pages" 是要加到总表空间上的量，它由
 * make_one_rel 为我们算好。
 *
 * 调用方预期已确保 tuples_fetched 大于零
 * 并舍入为整数（见 clamp_row_est）。结果同样将
 * 大于零并且是整数。
 */
double
index_pages_fetched(double tuples_fetched, BlockNumber pages,
					double index_pages, PlannerInfo *root)
{
	double		pages_fetched;
	double		total_pages;
	double		T,
				b;

	/* T 是表中的页数，但不允许它为零 */
	T = (pages > 1) ? (double) pages : 1.0;

	/* 计算假定在争夺缓存空间的页数 */
	total_pages = root->total_table_pages + index_pages;
	total_pages = Max(total_pages, 1.0);
	Assert(T <= total_pages);

	/* b 是 effective_cache_size 按比例分得的份额 */
	b = (double) effective_cache_size * T / total_pages;

	/* 强制它为正并且是整数 */
	if (b <= 1.0)
		b = 1.0;
	else
		b = ceil(b);

	/* 这一部分是 Mackert 和 Lohman 公式 */
	if (T <= b)
	{
		pages_fetched =
			(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		if (pages_fetched >= T)
			pages_fetched = T;
		else
			pages_fetched = ceil(pages_fetched);
	}
	else
	{
		double		lim;

		lim = (2.0 * T * b) / (2.0 * T - b);
		if (tuples_fetched <= lim)
		{
			pages_fetched =
				(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		}
		else
		{
			pages_fetched =
				b + (tuples_fetched - lim) * (T - b) / T;
		}
		pages_fetched = ceil(pages_fetched);
	}
	return pages_fetched;
}

/*
 * get_indexpath_pages
 *		确定位图索引路径中使用的索引的总规模。
 *
 * 注意：若同一索引在位图树中使用不止一次，我们会
 * 把它计算多次，这也许不对……但这
 * 并不完全清楚，而且检测重复很难，所以暂时
 * 忽略。
 */
static double
get_indexpath_pages(Path *bitmapqual)
{
	double		result = 0;
	ListCell   *l;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;

		foreach(l, apath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;

		foreach(l, opath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		result = (double) ipath->indexinfo->pages;
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));

	return result;
}

/*
 * cost_bitmap_heap_scan
 *	  确定并返回用位图
 *	  先索引后堆计划扫描一个关系的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 * 'bitmapqual' 是 IndexPaths、BitmapAndPaths 和 BitmapOrPaths 组成的树
 * 'loop_count' 是要计入缓存行为估计的
 *		索引扫描重复次数
 *
 * 注意：bitmapqual 中的组成 IndexPaths 应当已经用相同的 loop_count
 * 计过价。
 */
void
cost_bitmap_heap_scan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
					  ParamPathInfo *param_info,
					  Path *bitmapqual, double loop_count)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		indexTotalCost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	Cost		cost_per_page;
	Cost		cpu_run_cost;
	double		tuples_fetched;
	double		pages_fetched;
	double		spc_seq_page_cost,
				spc_random_page_cost;
	double		T;

	/* 只应当用于基关系 */
	Assert(IsA(baserel, RelOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	if (!enable_bitmapscan)
		startup_cost += disable_cost;

	pages_fetched = compute_bitmap_pages(root, baserel, bitmapqual,
										 loop_count, &indexTotalCost,
										 &tuples_fetched);

	startup_cost += indexTotalCost;
	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;

	/* 取包含该表的表空间的估计页代价。 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*
	 * 页数少时应当按页收取 spc_random_page_cost，
	 * 而若几乎要读表的全部页，更
	 * 适合按页收取 spc_seq_page_cost。效果
	 * 也是非线性的。没有更好的办法，就这样插值来
	 * 确定每页代价。
	 */
	if (pages_fetched >= 2.0)
		cost_per_page = spc_random_page_cost -
			(spc_random_page_cost - spc_seq_page_cost)
			* sqrt(pages_fetched / T);
	else
		cost_per_page = spc_random_page_cost;

	run_cost += pages_fetched * cost_per_page;

	/*
	 * 估计每个元组的 CPU 代价。
	 *
	 * 索引限定条件常常不必在每个元组上复查……但
	 * 并不总是如此，尤其是涉及的元组多到
	 * 位图变得有损时。目前就假定它们总会
	 * 被复查。这意味着我们对全部
	 * 扫描子句收取全部费用。
	 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	cpu_run_cost = cpu_per_tuple * tuples_fetched;

	/* 若使用了并行，就调整计价。 */
	if (path->parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(path);

		/* CPU 代价分摊到所有工作进程。 */
		cpu_run_cost /= parallel_divisor;

		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}


	run_cost += cpu_run_cost;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_bitmap_tree_node
 *		从位图树节点（索引/与/或）提取代价和选择率
 */
void
cost_bitmap_tree_node(Path *path, Cost *cost, Selectivity *selec)
{
	if (IsA(path, IndexPath))
	{
		*cost = ((IndexPath *) path)->indextotalcost;
		*selec = ((IndexPath *) path)->indexselectivity;

		/*
		 * 对每个检索到的元组收取一小笔费用，以反映操作
		 * 位图的代价。这主要是为了确保位图
		 * 扫描看起来不会与检索单个
		 * 元组的索引扫描代价相同。
		 */
		*cost += 0.1 * cpu_operator_cost * path->rows;
	}
	else if (IsA(path, BitmapAndPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapAndPath *) path)->bitmapselectivity;
	}
	else if (IsA(path, BitmapOrPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapOrPath *) path)->bitmapselectivity;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(path));
		*cost = *selec = 0;		/* 消除编译器告警 */
	}
}

/*
 * cost_bitmap_and_node
 *		估计 BitmapAnd 节点的代价
 *
 * 注意这只考虑索引扫描和位图
 * 创建的代价，不是最终的堆访问。从这个意义上说，该对象并不
 * 真正是一条 Path，但它有足够的类路径性质（尤其是代价），
 * 值得把它当作一条来对待。
 * 不过我们不费心设置路径的行数字段。
 */
void
cost_bitmap_and_node(BitmapAndPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * 我们在假定输入相互
	 * 独立的前提下估计 AND 选择率。这大概常常是错的，但我们没有信息
	 * 做得更好。
	 *
	 * BitmapAnd 本身的运行时代价估计为每次所需的 tbm_intersect 为 100 倍
	 * cpu_operator_cost。大概太小，
	 * 肯定太简化了？
	 */
	totalCost = 0.0;
	selec = 1.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec *= subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = selec;
	path->path.rows = 0;		/* 如上所述，未使用 */
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_bitmap_or_node
 *		估计 BitmapOr 节点的代价
 *
 * 见 cost_bitmap_and_node 的注释。
 */
void
cost_bitmap_or_node(BitmapOrPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * 我们在假定输入
	 * 不重叠的前提下估计 OR 选择率，因为 "x IN (list)" 这类
	 * 情形常常如此。当然，最后钳制到 1.0。
	 *
	 * BitmapOr 本身的运行时代价估计为每次所需的 tbm_union 为 100 倍
	 * cpu_operator_cost。大概太小，
	 * 肯定太简化了？我们知道当输入是 BitmapIndexScans 时，tbm_unions
	 * 会被优化掉。
	 */
	totalCost = 0.0;
	selec = 0.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec += subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals) &&
			!IsA(subpath, IndexPath))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = Min(selec, 1.0);
	path->path.rows = 0;		/* 如上所述，未使用 */
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_tidscan
 *	  确定并返回用 TIDs 扫描一个关系的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'tidquals' 是可做 TID 检查的限定条件列表
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_tidscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, List *tidquals, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	bool		isCurrentOf = false;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	QualCost	tid_qual_cost;
	int			ntuples;
	ListCell   *l;
	double		spc_random_page_cost;

	/* 只应当用于基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 数我们预期要检索多少元组 */
	ntuples = 0;
	foreach(l, tidquals)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		Expr	   *qual = rinfo->clause;

		if (IsA(qual, ScalarArrayOpExpr))
		{
			/* 数组的每个元素产生 1 个元组 */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) qual;
			Node	   *arraynode = (Node *) lsecond(saop->args);

			ntuples += estimate_array_length(arraynode);
		}
		else if (IsA(qual, CurrentOfExpr))
		{
			/* CURRENT OF 产生 1 个元组 */
			isCurrentOf = true;
			ntuples++;
		}
		else
		{
			/* 只是 CTID = 某物，计 1 个元组 */
			ntuples++;
		}
	}

	/*
	 * 对 WHERE CURRENT OF 必须强制 TID 扫描，因为只有 nodeTidscan.c
	 * 知道如何正确做。因此仅当不存在 CURRENT OF 时
	 * 才遵守 enable_tidscan。还要注意 cost_qual_eval
	 * 把 CurrentOfExpr 计为具有启动代价 disable_cost，我们
	 * 在这里把它减掉；这是为了防止顺序扫描等其他计划类型
	 * 胜出。
	 */
	if (isCurrentOf)
	{
		Assert(baserel->baserestrictcost.startup >= disable_cost);
		startup_cost -= disable_cost;
	}
	else if (!enable_tidscan)
		startup_cost += disable_cost;

	/*
	 * TID 限定表达式会计算一次，任何其他 baserestrict
	 * 限定条件对每个检索到的元组计算一次。
	 */
	cost_qual_eval(&tid_qual_cost, tidquals, root);

	/* 取包含该表的表空间的估计页代价 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/* 磁盘代价 --- 假定每个元组在不同的页上 */
	run_cost += spc_random_page_cost * ntuples;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	/* XXX 目前假定 TID 限定条件是 qpqual 的子集 */
	startup_cost += qpqual_cost.startup + tid_qual_cost.per_tuple;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple -
		tid_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_tidrangescan
 *	  确定并设置用一段 TIDs 范围扫描一个关系的代价，
 *	  用于 'path'
 *
 * 'baserel' 是要扫描的关系
 * 'tidrangequals' 是可做 TID 检查的范围限定条件列表
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_tidrangescan(Path *path, PlannerInfo *root,
				  RelOptInfo *baserel, List *tidrangequals,
				  ParamPathInfo *param_info)
{
	Selectivity selectivity;
	double		pages;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	QualCost	tid_qual_cost;
	double		ntuples;
	double		nseqpages;
	double		spc_random_page_cost;
	double		spc_seq_page_cost;

	/* 只应当用于基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 数我们预期要扫描多少元组和页 */
	selectivity = clauselist_selectivity(root, tidrangequals, baserel->relid,
										 JOIN_INNER, NULL);
	pages = ceil(selectivity * baserel->pages);

	if (pages <= 0.0)
		pages = 1.0;

	/*
	 * 范围内的第一页需要一次随机寻道，但随后每一
	 * 页只是普通的顺序页读。注意：希望
	 * TID 范围扫描比等价的顺序扫描更贵，
	 * 因为顺序扫描有扫描
	 * 同步和可并行性等性能优势，我们更希望选中其中之一，
	 * 除非 TID 范围扫描真的更好。
	 */
	ntuples = selectivity * baserel->tuples;
	nseqpages = pages - 1.0;

	if (!enable_tidscan)
		startup_cost += disable_cost;

	/*
	 * TID 限定表达式会计算一次，任何其他 baserestrict
	 * 限定条件对每个检索到的元组计算一次。
	 */
	cost_qual_eval(&tid_qual_cost, tidrangequals, root);

	/* 取包含该表的表空间的估计页代价 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/* 磁盘代价；1 次随机页，其余作为顺序页 */
	run_cost += spc_random_page_cost + spc_seq_page_cost * nseqpages;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	/*
	 * XXX 目前假定在这一点上 TID 限定条件是 qpqual 的子集；
	 * 建立计划时会（若可能）去掉它们，因此
	 * 我们从总 qpqual 代价中减去它们的代价。（若 TID 限定条件
	 * 无法去掉，这就是个错误，我们会略微低估
	 * CPU 代价。）
	 */
	startup_cost += qpqual_cost.startup + tid_qual_cost.per_tuple;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple -
		tid_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_subqueryscan
 *	  确定并返回扫描子查询 RTE 的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 * 若认为 pathtarget 是平凡的，'trivial_pathtarget' 为 true。
 */
void
cost_subqueryscan(SubqueryScanPath *path, PlannerInfo *root,
				  RelOptInfo *baserel, ParamPathInfo *param_info,
				  bool trivial_pathtarget)
{
	Cost		startup_cost;
	Cost		run_cost;
	List	   *qpquals;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于作为子查询的基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_SUBQUERY);

	/*
	 * 把行数估计算成子计划的估计乘以
	 * 相关限制子句的选择率。简单情形下这会
	 * 得出与 baserel->rows 相同的结果；但处理并行
	 * 路径时必须这样算才能得到正确答案。
	 */
	if (param_info)
		qpquals = list_concat_copy(param_info->ppi_clauses,
								   baserel->baserestrictinfo);
	else
		qpquals = baserel->baserestrictinfo;

	path->path.rows = clamp_row_est(path->subpath->rows *
									clauselist_selectivity(root,
														   qpquals,
														   0,
														   JOIN_INNER,
														   NULL));

	/*
	 * 路径代价是求值子计划的代价，加上求值
	 * 将挂到 SubqueryScan 节点上的任何限制子句和目标列表的代价，
	 * 再加上 cpu_tuple_cost 以计入选择和
	 * 投影开销。
	 */
	path->path.startup_cost = path->subpath->startup_cost;
	path->path.total_cost = path->subpath->total_cost;

	/*
	 * 不过，若没有相关限制子句，并且
	 * pathtarget 是平凡的，就预期 setrefs.c 会把
	 * SubqueryScan 计划节点整个优化掉，因此应当让它的代价
	 * 和行数等于输入路径的。
	 *
	 * 注意：有些边角情形 createplan.c 会给
	 * SubqueryScan 节点应用不同的目标列表，从而使我们
	 * 当前关于目标是否平凡的估计不成立，并使代价
	 * 估计（尽管不是行数）出错。似乎不值得为
	 * 额外的复杂性去精确计入这一点，尤其是因为
	 * 那种行为也会使其他代价估计不成立。
	 */
	if (qpquals == NIL && trivial_pathtarget)
		return;

	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost = qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost = cpu_per_tuple * path->subpath->rows;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->path.pathtarget->cost.startup;
	run_cost += path->path.pathtarget->cost.per_tuple * path->path.rows;

	path->path.startup_cost += startup_cost;
	path->path.total_cost += startup_cost + run_cost;
}

/*
 * cost_functionscan
 *	  确定并返回扫描函数 RTE 的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_functionscan(Path *path, PlannerInfo *root,
				  RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	RangeTblEntry *rte;
	QualCost	exprcost;

	/* 只应当用于作为函数的基关系 */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * 估计执行函数表达式的代价。
	 *
	 * 目前，nodeFunctionscan.c 总是把函数执行
	 * 完毕才返回任何行，并把结果缓存在
	 * tuplestore 中。因此函数求值代价全是启动代价，每行
	 * 代价很小。
	 *
	 * XXX 原则上若行数很大，应当收取 tuplestore 溢出代价。
	 * 不过，鉴于我们对函数的行数
	 * 估计往往很假，现在做这种
	 * 细化意义不大。
	 */
	cost_qual_eval_node(&exprcost, (Node *) rte->functions, root);

	startup_cost += exprcost.startup + exprcost.per_tuple;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_tablefuncscan
 *	  确定并返回扫描表函数的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_tablefuncscan(Path *path, PlannerInfo *root,
				   RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	RangeTblEntry *rte;
	QualCost	exprcost;

	/* 只应当用于作为函数的基关系 */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_TABLEFUNC);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * 估计执行表函数表达式的代价。
	 *
	 * XXX 原则上若行数很大，应当收取 tuplestore 溢出代价。
	 * 不过，鉴于我们对表函数的行数
	 * 估计往往很假，现在做这种
	 * 细化意义不大。
	 */
	cost_qual_eval_node(&exprcost, (Node *) rte->tablefunc, root);

	startup_cost += exprcost.startup + exprcost.per_tuple;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_valuesscan
 *	  确定并返回扫描 VALUES RTE 的代价。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 若这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_valuesscan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于作为值列表的基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_VALUES);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * 目前，把列表求值代价估计为每个列表一次操作符求值
	 * （大概很不靠谱，但值得更聪明吗？）
	 */
	cpu_per_tuple = cpu_operator_cost;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_ctescan
 *	  确定并返回扫描 CTE RTE 的代价。
 *
 * 注意：自引用和普通 CTEs 都用这个；
 * 可能的代价差异低于我们反正能
 * 准确估计的阈值。注意求值
 * 被引用 CTE 查询的代价作为 initplan 代价加进最终计划，
 * 不应当算在这里。
 */
void
cost_ctescan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于作为 CTEs 的基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_CTE);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 每行收取一次 CPU 元组代价，用于 tuplestore 操作 */
	cpu_per_tuple = cpu_tuple_cost;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_namedtuplestorescan
 *	  确定并返回扫描命名 tuplestore 的代价。
 */
void
cost_namedtuplestorescan(Path *path, PlannerInfo *root,
						 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于作为 Tuplestores 的基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_NAMEDTUPLESTORE);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 每行收取一次 CPU 元组代价，用于 tuplestore 操作 */
	cpu_per_tuple = cpu_tuple_cost;

	/* 加上扫描的 CPU 代价 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_resultscan
 *	  确定并返回扫描 RTE_RESULT 关系的代价。
 */
void
cost_resultscan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应当用于 RTE_RESULT 基关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RESULT);

	/* 用正确的行数估计标记该路径 */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 我们收取限定条件代价加上 cpu_tuple_cost */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_recursive_union
 *	  确定并返回执行递归并集的代价，
 *	  以及估计的输出规模。
 *
 * 我们得到非递归项和递归项的 Paths。
 */
void
cost_recursive_union(Path *runion, Path *nrterm, Path *rterm)
{
	Cost		startup_cost;
	Cost		total_cost;
	double		total_rows;

	/* 对非递归项，我们大概有不错的估计 */
	startup_cost = nrterm->startup_cost;
	total_cost = nrterm->total_cost;
	total_rows = nrterm->rows;

	/*
	 * 我们任意假定大约需要 10 次递归迭代，
	 * 并且已经设法很好地掌握每一次的代价和输出
	 * 规模。这些假定非常不牢靠，但很难
	 * 看出怎样做得更好。
	 */
	total_cost += 10 * rterm->total_cost;
	total_rows += 10 * rterm->rows;

	/*
	 * 还按每行收取 cpu_tuple_cost，以计入
	 * 操作 tuplestore 的代价。（我们不担心可能的
	 * 溢出到磁盘的代价。）
	 */
	total_cost += cpu_tuple_cost * total_rows;

	runion->startup_cost = startup_cost;
	runion->total_cost = total_cost;
	runion->rows = total_rows;
	runion->pathtarget->width = Max(nrterm->pathtarget->width,
									rterm->pathtarget->width);
}

/*
 * cost_tuplesort
 *	  确定并返回用 tuplesort 对一个关系排序的代价，
 *    不包括读取输入数据的代价。
 *
 * 若要排序的数据总量小于 sort_mem，就做
 * 内存中排序，这不需要 I/O，对 t 个元组大约做 t*log2(t) 次
 * 元组比较。
 *
 * 若总量超过 sort_mem，就切换到磁带式归并
 * 算法。总共仍大约有 t*log2(t) 次元组比较，
 * 但每个归并趟还需要把每个元组写一次、读一次。
 * 预期大约有 ceil(logM(r)) 个归并趟，其中 r 是
 * 形成的初始趟数，M 是 tuplesort.c 使用的归并阶。
 * 由于平均初始趟大约应当是 sort_mem，我们有
 *		磁盘流量 = 2 * relsize * ceil(logM(p / sort_mem))
 *		cpu = comparison_cost * t * log2(t)
 *
 * 若排序有界（即只需要前 k 个结果元组）
 * 并且 k 个元组能放进 sort_mem，就用只在堆中保留
 * k 个元组的堆方法；这大约需要 t*log2(k) 次元组比较。
 *
 * 假定磁盘流量有 3/4 是顺序访问、1/4 是随机
 * 访问（XXX 不能把这个猜测细化吗？）
 *
 * 默认每次元组比较收取两次操作符求值，这在
 * 多数情形里应当差不多。调用方可以通过
 * 指定非零的 comparison_cost 来调整；通常那用于比较操作符的输入
 * 必须做的任何额外准备工作。
 *
 * 'tuples' 是关系中的元组数
 * 'width' 是平均元组宽度（字节）
 * 'comparison_cost' 是每次比较的额外代价，若有的话
 * 'sort_mem' 是允许用于排序的工作内存千字节数
 * 'limit_tuples' 是输出元组数的上界；若无上界则为 -1
 */
static void
cost_tuplesort(Cost *startup_cost, Cost *run_cost,
			   double tuples, int width,
			   Cost comparison_cost, int sort_mem,
			   double limit_tuples)
{
	double		input_bytes = relation_byte_size(tuples, width);
	double		output_bytes;
	double		output_tuples;
	long		sort_mem_bytes = sort_mem * 1024L;

	/*
	 * 我们想确保排序代价永远不会被估计为零，即使
	 * 传入的元组数为零。另外，不得做 log(0)……
	 */
	if (tuples < 2.0)
		tuples = 2.0;

	/* 包括默认的每次比较代价 */
	comparison_cost += 2.0 * cpu_operator_cost;

	/* 我们有有用的 LIMIT 吗？ */
	if (limit_tuples > 0 && limit_tuples < tuples)
	{
		output_tuples = limit_tuples;
		output_bytes = relation_byte_size(output_tuples, width);
	}
	else
	{
		output_tuples = tuples;
		output_bytes = input_bytes;
	}

	if (output_bytes > sort_mem_bytes)
	{
		/*
		 * 必须对全部元组做基于磁盘的排序
		 */
		double		npages = ceil(input_bytes / BLCKSZ);
		double		nruns = input_bytes / sort_mem_bytes;
		double		mergeorder = tuplesort_merge_order(sort_mem_bytes);
		double		log_runs;
		double		npageaccesses;

		/*
		 * CPU 代价
		 *
		 * 假定大约 N log2 N 次比较
		 */
		*startup_cost = comparison_cost * tuples * LOG2(tuples);

		/* 磁盘代价 */

		/* 把 logM(r) 算成 log(r) / log(M) */
		if (nruns > mergeorder)
			log_runs = ceil(log(nruns) / log(mergeorder));
		else
			log_runs = 1.0;
		npageaccesses = 2.0 * npages * log_runs;
		/* 假定 3/4 的访问是顺序的，1/4 不是 */
		*startup_cost += npageaccesses *
			(seq_page_cost * 0.75 + random_page_cost * 0.25);
	}
	else if (tuples > 2 * output_tuples || input_bytes > sort_mem_bytes)
	{
		/*
		 * 将使用只在内存中保留 K 个元组的有界堆排序，
		 * 元组比较总数为 N log2 K；但常数
		 * 因子比快速排序略高。调整它，使
		 * 代价曲线在交叉点连续。
		 */
		*startup_cost = comparison_cost * tuples * LOG2(2.0 * output_tuples);
	}
	else
	{
		/* 将对全部输入元组使用普通快速排序 */
		*startup_cost = comparison_cost * tuples * LOG2(tuples);
	}

	/*
	 * 还对每个
	 * 抽出的元组收取一小笔费用（任意设成等于操作符代价）。不收取 cpu_tuple_cost，因为 Sort 节点
	 * 不做限定条件检查或投影，因此开销比
	 * 多数计划节点小。注意这里用 tuples 而不是 output_tuples
	 * 是对的 --- 上层 LIMIT 会按比例分摊运行代价，否则会把
	 * LIMIT 算两次。
	 */
	*run_cost = cpu_operator_cost * tuples;
}

/*
 * cost_incremental_sort
 * 	确定并返回增量排序一个关系的代价，当
 *  输入路径已按路径键的一个前缀预排序时。
 *
 * 'presorted_keys' 是输入路径已按其排序的前导路径键个数。
 * 估计关系被前导
 *
 * 路径键分成多少组，然后用 cost_tuplesort()
 * 计算用 tuplesort 对单个组
 * 排序的代价。
 */
void
cost_incremental_sort(Path *path,
					  PlannerInfo *root, List *pathkeys, int presorted_keys,
					  Cost input_startup_cost, Cost input_total_cost,
					  double input_tuples, int width, Cost comparison_cost, int sort_mem,
					  double limit_tuples)
{
	Cost		startup_cost,
				run_cost,
				input_run_cost = input_total_cost - input_startup_cost;
	double		group_tuples,
				input_groups;
	Cost		group_startup_cost,
				group_run_cost,
				group_input_run_cost;
	List	   *presortedExprs = NIL;
	ListCell   *l;
	bool		unknown_varno = false;

	Assert(presorted_keys > 0 && presorted_keys < list_length(pathkeys));

	/*
	 * 我们想确保排序代价永远不会被估计为零，即使
	 * 传入的元组数为零。另外，不得做 log(0)……
	 */
	if (input_tuples < 2.0)
		input_tuples = 2.0;

	/* 组数的默认估计，封顶为每行一组。 */
	input_groups = Min(input_tuples, DEFAULT_NUM_DISTINCT);

	/*
	 * 把预排序键抽成表达式列表。
	 *
	 * 必须小心含有 "varno 0" 的 Vars，它们可能由
	 * generate_append_tlist 引入，那会搞乱
	 * estimate_num_groups（事实上对这类表达式它会失败）。见
	 * 必须处理同一问题的 recurse_set_operations。
	 *
	 * 与 recurse_set_operations 不同，这里无法访问原来的目标列表，
	 * 而且即使能，对组合多张表的集合运算那有多大用也不
	 * 很清楚。因此只是检测是否
	 * 有任何带 "varno 0" 的表达式，若有就使用默认的
	 * DEFAULT_NUM_DISTINCT。
	 *
	 * 也可以用 1.0（单个组）或 input_tuples（每一行
	 * 单独成组），差不多是增量排序最坏和最好的情形。
	 * 但那些是极端情形，用中间某个值似乎合理。
	 * 而且 generate_append_tlist 用于
	 * 集合运算，它们很可能本来就会产生大多唯一的输出
	 * --- 从这个角度看 DEFAULT_NUM_DISTINCT 是防御性的，
	 * 同时保持较低的启动代价。
	 */
	foreach(l, pathkeys)
	{
		PathKey    *key = (PathKey *) lfirst(l);
		EquivalenceMember *member = (EquivalenceMember *)
			linitial(key->pk_eclass->ec_members);

		/*
		 * 检查表达式是否含有 "varno 0" 的 Var，以免
		 * 在那种情况下调用 estimate_num_groups。
		 */
		if (bms_is_member(0, pull_varnos(root, (Node *) member->em_expr)))
		{
			unknown_varno = true;
			break;
		}

		/* 不含任何 "varno 0" Var 的表达式 */
		presortedExprs = lappend(presortedExprs, member->em_expr);

		if (foreach_current_index(l) + 1 >= presorted_keys)
			break;
	}

	/* 估计预排序键相等的组数。 */
	if (!unknown_varno)
		input_groups = estimate_num_groups(root, presortedExprs, input_tuples,
										   NULL, NULL);

	group_tuples = input_tuples / input_groups;
	group_input_run_cost = input_run_cost / input_groups;

	/*
	 * 估计预排序键相等的单个组的平均排序
	 * 代价。
	 */
	cost_tuplesort(&group_startup_cost, &group_run_cost,
				   group_tuples, width, comparison_cost, sort_mem,
				   limit_tuples);

	/*
	 * 增量排序的启动代价是第一组的启动代价
	 * 加上其输入的代价。
	 */
	startup_cost = group_startup_cost + input_startup_cost +
		group_input_run_cost;

	/*
	 * 开始从第一组产出元组之后，产出全部
	 * 元组的代价等于处理完这一
	 * 组的代价，加上处理其余组的总代价，加上
	 * 输入的剩余代价。
	 */
	run_cost = group_run_cost + (group_run_cost + group_startup_cost) *
		(input_groups - 1) + group_input_run_cost * (input_groups - 1);

	/*
	 * 增量排序本身会增加一些开销。首先，它必须
	 * 检测排序组。这大致等于每个元组多一次复制和
	 * 比较。
	 */
	run_cost += (cpu_tuple_cost + comparison_cost) * input_tuples;

	/*
	 * 另外，对每个输入组收取两倍 cpu_tuple_cost，以
	 * 计入每组之后执行的 tuplesort_reset。
	 */
	run_cost += 2.0 * cpu_tuple_cost * input_groups;

	path->rows = input_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_sort
 *	  确定并返回对一个关系排序的代价，包括
 *	  读取输入数据的代价。
 *
 * 注意：有些调用方目前为路径键传入 NIL，因为它们
 * 不方便提供排序键。既然本例程目前反正
 * 对路径键什么都不做，那就没关系……
 * 但若以后会做，它应当从容应对缺少键数据。
 * （实际上，我们最可能关心的只是排序键的个数，
 * 所有调用方 *都能* 提供。）
 */
void
cost_sort(Path *path, PlannerInfo *root,
		  List *pathkeys, Cost input_cost, double tuples, int width,
		  Cost comparison_cost, int sort_mem,
		  double limit_tuples)

{
	Cost		startup_cost;
	Cost		run_cost;

	cost_tuplesort(&startup_cost, &run_cost,
				   tuples, width,
				   comparison_cost, sort_mem,
				   limit_tuples);

	if (!enable_sort)
		startup_cost += disable_cost;

	startup_cost += input_cost;

	path->rows = tuples;
	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * append_nonpartial_cost
 *	  估计并行 Append 中非部分路径的代价。
 *	  假定非部分路径是 subpaths 列表中前 "numpaths" 条
 *	  路径，并且按代价递减排序。
 */
static Cost
append_nonpartial_cost(List *subpaths, int numpaths, int parallel_workers)
{
	Cost	   *costarr;
	int			arrlen;
	ListCell   *l;
	ListCell   *cell;
	int			path_index;
	int			min_index;
	int			max_index;

	if (numpaths == 0)
		return 0;

	/*
	 * 数组长度是工作进程数或相关路径数，
	 * 取较小者。
	 */
	arrlen = Min(parallel_workers, numpaths);
	costarr = (Cost *) palloc(sizeof(Cost) * arrlen);

	/* 前几条路径将各由不同的工作进程认领。 */
	path_index = 0;
	foreach(cell, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(cell);

		if (path_index == arrlen)
			break;
		costarr[path_index++] = subpath->total_cost;
	}

	/*
	 * 由于子路径按代价递减排序，最后一条将具有
	 * 最小代价。
	 */
	min_index = arrlen - 1;

	/*
	 * 对其余每条子路径，把它的代价加到具有最小代价的
	 * 数组元素上。
	 */
	for_each_cell(l, subpaths, cell)
	{
		Path	   *subpath = (Path *) lfirst(l);

		/* 只考虑非部分路径 */
		if (path_index++ == numpaths)
			break;

		costarr[min_index] += subpath->total_cost;

		/* 更新新的最小代价数组下标 */
		min_index = 0;
		for (int i = 0; i < arrlen; i++)
		{
			if (costarr[i] < costarr[min_index])
				min_index = i;
		}
	}

	/* 返回数组中的最高代价 */
	max_index = 0;
	for (int i = 0; i < arrlen; i++)
	{
		if (costarr[i] > costarr[max_index])
			max_index = i;
	}

	return costarr[max_index];
}

/*
 * cost_append
 *	  确定并返回 Append 节点的代价。
 */
void
cost_append(AppendPath *apath)
{
	ListCell   *l;

	apath->path.startup_cost = 0;
	apath->path.total_cost = 0;
	apath->path.rows = 0;

	if (apath->subpaths == NIL)
		return;

	if (!apath->path.parallel_aware)
	{
		List	   *pathkeys = apath->path.pathkeys;

		if (pathkeys == NIL)
		{
			Path	   *firstsubpath = (Path *) linitial(apath->subpaths);

			/*
			 * 对无序、不感知并行的 Append，把启动
			 * 代价取为第一条子路径的启动代价。
			 */
			apath->path.startup_cost = firstsubpath->startup_cost;

			/* 把行数和代价算成子计划行数和代价之和。 */
			foreach(l, apath->subpaths)
			{
				Path	   *subpath = (Path *) lfirst(l);

				apath->path.rows += subpath->rows;
				apath->path.total_cost += subpath->total_cost;
			}
		}
		else
		{
			/*
			 * 对有序、不感知并行的 Append，把启动
			 * 代价取为子路径启动代价之和。这确保
			 * 当查询的
			 * LIMIT 使得必须运行若干子项才能
			 * 满足它时，我们不会低估启动代价。这也许过头了 --- 另一个说得通的做法
			 * 是把 Append 的启动代价取为子项启动代价的
			 * 最大值。但我们不想冒险认为
			 * 一条 ORDER BY LIMIT 查询能以很小的代价得到满足，
			 * 当第一个子项启动代价很小但后面的
			 * 不是时。（若我们有能力处理非线性的
			 * 部分检索代价插值，就不必
			 * 对此如此保守。）
			 *
			 * 这种情形与上面不同，因为我们必须
			 * 计入可能向本身没有原生顺序的子路径注入
			 * 排序。
			 */
			foreach(l, apath->subpaths)
			{
				Path	   *subpath = (Path *) lfirst(l);
				Path		sort_path;	/* cost_sort 结果的占位 */

				if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
				{
					/*
					 * 需要插入一个 Sort 节点，因此计入
					 * 它的代价。若父项有 LIMIT，可以用它，因为我们
					 * 肯定不会从
					 * 任何子项取出比那更多的元组。
					 */
					cost_sort(&sort_path,
							  NULL, /* 目前不需要 root */
							  pathkeys,
							  subpath->total_cost,
							  subpath->rows,
							  subpath->pathtarget->width,
							  0.0,
							  work_mem,
							  apath->limit_tuples);
					subpath = &sort_path;
				}

				apath->path.rows += subpath->rows;
				apath->path.startup_cost += subpath->startup_cost;
				apath->path.total_cost += subpath->total_cost;
			}
		}
	}
	else						/* 感知并行 */
	{
		int			i = 0;
		double		parallel_divisor = get_parallel_divisor(&apath->path);

		/* 感知并行的 Append 从不产生有序输出。 */
		Assert(apath->path.pathkeys == NIL);

		/* 计算启动代价。 */
		foreach(l, apath->subpaths)
		{
			Path	   *subpath = (Path *) lfirst(l);

			/*
			 * 当启动代价最低的子节点
			 * 完成建立时，Append 将开始返回元组。我们只考虑
			 * 立刻分到工作进程的前几条子计划。
			 */
			if (i == 0)
				apath->path.startup_cost = subpath->startup_cost;
			else if (i < apath->path.parallel_workers)
				apath->path.startup_cost = Min(apath->path.startup_cost,
											   subpath->startup_cost);

			/*
			 * 对子路径应用并行除数。根据原先用于该子路径的并行
			 * 除数与我们采用的除数之比，缩放每条部分子路径的
			 * 行数。
			 * 也把部分路径的代价加到总代价，但
			 * 暂时忽略非部分路径。
			 */
			if (i < apath->first_partial_path)
				apath->path.rows += subpath->rows / parallel_divisor;
			else
			{
				double		subpath_parallel_divisor;

				subpath_parallel_divisor = get_parallel_divisor(subpath);
				apath->path.rows += subpath->rows * (subpath_parallel_divisor /
													 parallel_divisor);
				apath->path.total_cost += subpath->total_cost;
			}

			apath->path.rows = clamp_row_est(apath->path.rows);

			i++;
		}

		/* 加上非部分子路径的代价。 */
		apath->path.total_cost +=
			append_nonpartial_cost(apath->subpaths,
								   apath->first_partial_path,
								   apath->path.parallel_workers);
	}

	/*
	 * 虽然 Append 不做任何选择或投影，它也不是免费的；
	 * 加上一小笔每元组开销。
	 */
	apath->path.total_cost +=
		cpu_tuple_cost * APPEND_CPU_COST_MULTIPLIER * apath->path.rows;
}

/*
 * cost_merge_append
 *	  确定并返回 MergeAppend 节点的代价。
 *
 * MergeAppend 用一个堆归并若干已预排序的输入流，该堆
 * 在任一时刻保存每个流的下一个元组。若
 * 有 N 个流，启动时构造堆大约需要 N*log2(N) 次元组比较，
 * 然后对每个输出元组，大约需要 log2(N)
 * 次比较来替换堆顶项。
 *
 * （一旦某些输入流耗尽，N 的有效值会下降，
 * 但试图计入这一点似乎不值得。）
 *
 * 堆从不溢出到磁盘，因为我们假定 N 不是很大。
 * 因此这比 cost_sort 简单得多。
 *
 * 与 cost_sort 一样，每次元组比较收取两次操作符求值。
 *
 * 'pathkeys' 是排序键列表
 * 'n_streams' 是输入流的个数
 * 'input_startup_cost' 是各输入流启动代价之和
 * 'input_total_cost' 是各输入流总代价之和
 * 'tuples' 是所有流中的元组数
 */
void
cost_merge_append(Path *path, PlannerInfo *root,
				  List *pathkeys, int n_streams,
				  Cost input_startup_cost, Cost input_total_cost,
				  double tuples)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		comparison_cost;
	double		N;
	double		logN;

	/*
	 * 避免 log(0)……
	 */
	N = (n_streams < 2) ? 2.0 : (double) n_streams;
	logN = LOG2(N);

	/* 假定的每次元组比较代价 */
	comparison_cost = 2.0 * cpu_operator_cost;

	/* 建堆代价 */
	startup_cost += comparison_cost * N * logN;

	/* 每元组的堆维护代价 */
	run_cost += tuples * comparison_cost * logN;

	/*
	 * 虽然 MergeAppend 不做任何选择或投影，它也不是
	 * 免费的；加上一小笔每元组开销。
	 */
	run_cost += cpu_tuple_cost * APPEND_CPU_COST_MULTIPLIER * tuples;

	path->startup_cost = startup_cost + input_startup_cost;
	path->total_cost = startup_cost + run_cost + input_total_cost;
}

/*
 * cost_material
 *	  确定并返回物化一个关系的代价，包括
 *	  读取输入数据的代价。
 *
 * 若要物化的数据总量超过 work_mem，就需要
 * 把它写到磁盘，那种情况下代价高得多。
 *
 * 注意这里估计的是对该关系第一次扫描的
 * 代价，因此物化全是开销 --- 任何节省只发生在
 * 重新扫描时，那在 cost_rescan 中估计。
 */
void
cost_material(Path *path,
			  Cost input_startup_cost, Cost input_total_cost,
			  double tuples, int width)
{
	Cost		startup_cost = input_startup_cost;
	Cost		run_cost = input_total_cost - input_startup_cost;
	double		nbytes = relation_byte_size(tuples, width);
	long		work_mem_bytes = work_mem * 1024L;

	path->rows = tuples;

	/*
	 * 无论是否溢出，每个元组收取 2 倍 cpu_operator_cost，以
	 * 反映簿记开销。（这个费率必须高于
	 * cost_rescan 对物化收取的每个元组 cpu_operator_cost；
	 * 若完全相同，则 A 在外侧、物化的 B 在内侧的嵌套循环，与 B 在外侧、
	 * 物化的 A 在内侧的嵌套循环之间会出现代价平局。额外代价确保我们更愿意
	 * 物化较小的关系。）
	 * 注意这通常比
	 * cpu_tuple_cost 小不少；这可以，因为 Material 计划节点
	 * 不做限定条件检查或投影，因此开销比
	 * 多数计划节点小。
	 */
	run_cost += 2 * cpu_operator_cost * tuples;

	/*
	 * 若将溢出到磁盘，按每页 seq_page_cost 的费率收取。
	 * 假定这笔代价均匀分布在计划运行阶段，
	 * 这并不完全准确，但我们的代价模型不允许
	 * 运行阶段内的非均匀代价。
	 */
	if (nbytes > work_mem_bytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);

		run_cost += seq_page_cost * npages;
	}

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_memoize_rescan
 *	  确定重新扫描 Memoize 节点的估计代价。
 *
 * 为了估计这一点，必须了解预期被调用的频率，以及
 * 可能带着多少组不同的参数被
 * 调用。若预期缓存命中率不错，就可以把
 * 代价设成计入该命中率，再加上缓存本身的一小笔代价。
 * 若预期带着过多不同的参数值被调用，缓存就不会很好。
 * 这里最坏的情况是我们
 * 从未看见任何参数值出现两次，那时永远不会命中缓存，
 * 缓存就完全是白费力气。
 */
static void
cost_memoize_rescan(PlannerInfo *root, MemoizePath *mpath,
					Cost *rescan_startup_cost, Cost *rescan_total_cost)
{
	EstimationInfo estinfo;
	ListCell   *lc;
	Cost		input_startup_cost = mpath->subpath->startup_cost;
	Cost		input_total_cost = mpath->subpath->total_cost;
	double		tuples = mpath->subpath->rows;
	double		calls = mpath->calls;
	int			width = mpath->subpath->pathtarget->width;

	double		hash_mem_bytes;
	double		est_entry_bytes;
	double		est_cache_entries;
	double		ndistinct;
	double		evict_ratio;
	double		hit_ratio;
	Cost		startup_cost;
	Cost		total_cost;

	/* 可用的缓存空间 */
	hash_mem_bytes = get_hash_memory_limit();

	/*
	 * 设置每个缓存项在缓存中应占用的字节数。
	 * 为了更好地估计一次能存多少缓存项，
	 * 这里调用执行器，询问
	 * 单个缓存项有哪些内存开销。
	 */
	est_entry_bytes = relation_byte_size(tuples, width) +
		ExecEstimateCacheEntryOverheadBytes(tuples);

	/* 包括缓存键的估计宽度 */
	foreach(lc, mpath->param_exprs)
		est_entry_bytes += get_expr_width(root, (Node *) lfirst(lc));

	/* 一次能容纳的缓存项数上限的估计 */
	est_cache_entries = floor(hash_mem_bytes / est_entry_bytes);

	/* 不同参数值个数的估计 */
	ndistinct = estimate_num_groups(root, mpath->param_exprs, calls, NULL,
									&estinfo);

	/*
	 * 当估计退回到使用默认值时，假定可以使用 Memoize 节点就有点太
	 * 冒险。使用
	 * 默认值可能导致在其实
	 * 不合适时使用 Memoize 节点。若看到已经这样做了，就
	 * 假定每次调用的参数都是唯一的，这几乎
	 * 肯定意味着 MemoizePath 永远通不过 add_path()。
	 */
	if ((estinfo.flags & SELFLAG_USED_DEFAULT) != 0)
		ndistinct = calls;

	/*
	 * 既然已经估计了一次能存储的最大项数，
	 * 并且知道将被调用时的不同值估计个数，
	 * 就借此机会设置路径的 est_entries。
	 * 这最终决定执行器
	 * 将使用的哈希表大小。若把它留为零，执行器会自己
	 * 选择大小。这其实不是做这件事的正确地方，但
	 * 既然一切都已经算好，这样做很方便。
	 */
	mpath->est_entries = Min(Min(ndistinct, est_cache_entries),
							 PG_UINT32_MAX);

	/*
	 * 当不同参数值的个数高于缓存能
	 * 存储的数量时，就必须从
	 * 缓存中逐出一些项。这不是免费的。这里估计会多常
	 * 承担那次逐出的代价。
	 */
	evict_ratio = 1.0 - Min(est_cache_entries, ndistinct) / ndistinct;

	/*
	 * 为了估计单次扫描有多贵，需要
	 * 试着估计缓存命中率会是多少。为此必须
	 * 看这个节点总共估计有多少次扫描，以及
	 * 其中有多少次扫描预期会命中缓存。
	 */
	hit_ratio = ((calls - ndistinct) / calls) *
		(est_cache_entries / Max(ndistinct, est_cache_entries));

	Assert(hit_ratio >= 0 && hit_ratio <= 1.0);

	/*
	 * 按预期缓存命中率设置 total_cost。还
	 * 加上一次 cpu_operator_cost 以计入缓存查找。无论
	 * 是否命中都会发生。
	 */
	total_cost = input_total_cost * (1.0 - hit_ratio) + cpu_operator_cost;

	/* 现在调整总代价以计入缓存逐出 */

	/* 为逐出实际的缓存项收取一次 cpu_tuple_cost */
	total_cost += cpu_tuple_cost * evict_ratio;

	/*
	 * 为逐出该项中的每个元组收取十分之一 cpu_operator_cost。
	 * 每元组逐出其实只是一次 pfree，因此收取整整一次
	 * cpu_operator_cost 似乎有点过分。
	 */
	total_cost += cpu_operator_cost / 10.0 * evict_ratio * tuples;

	/*
	 * 现在调整把东西存进缓存的代价，因为那也不是免费的。
	 * 一切都必须进缓存。我们不按任何比例
	 * 分摊，只对这次扫描应用一次。为
	 * 创建缓存项收取一次 cpu_tuple_cost，也为预期要缓存的每个元组收取一次
	 * cpu_operator_cost。
	 */
	total_cost += cpu_tuple_cost + cpu_operator_cost * tuples;

	/*
	 * 取得第一行也必须按
	 * 预期缓存命中率按比例计算。
	 */
	startup_cost = input_startup_cost * (1.0 - hit_ratio);

	/*
	 * 另外收取一次 cpu_tuple_cost 以计入缓存查找，
	 * 无论是否命中都会做。
	 */
	startup_cost += cpu_tuple_cost;

	*rescan_startup_cost = startup_cost;
	*rescan_total_cost = total_cost;
}

/*
 * cost_agg
 *		确定并返回执行 Agg 计划节点的代价，
 *		包括其输入的代价。
 *
 * 当没有实际的聚合函数时，aggcosts 可以为 NULL（即
 * 只用哈希 Agg 节点来做分组）。
 *
 * 注意：当 aggstrategy == AGG_SORTED 时，调用方必须确保输入代价
 * 是针对已适当排序的输入的。
 */
void
cost_agg(Path *path, PlannerInfo *root,
		 AggStrategy aggstrategy, const AggClauseCosts *aggcosts,
		 int numGroupCols, double numGroups,
		 List *quals,
		 Cost input_startup_cost, Cost input_total_cost,
		 double input_tuples, double input_width)
{
	double		output_tuples;
	Cost		startup_cost;
	Cost		total_cost;
	AggClauseCosts dummy_aggcosts;

	/* 若传入 NULL，就使用全零的每聚合代价 */
	if (aggcosts == NULL)
	{
		Assert(aggstrategy == AGG_HASHED);
		MemSet(&dummy_aggcosts, 0, sizeof(AggClauseCosts));
		aggcosts = &dummy_aggcosts;
	}

	/*
	 * aggcosts 的 transCost.per_tuple 分量应当对每个
	 * 输入元组收取一次，对应于求值聚合
	 * 转移函数及其输入表达式的代价。finalCost.per_tuple 分量
	 * 对每个输出元组收取一次，对应于求值
	 * 最终函数的代价。启动代价当然只收取一次。
	 *
	 * 若在分组，就对每个
	 * 输入元组的每个分组列额外收取一次 cpu_operator_cost，用于分组比较。
	 *
	 * 若不分组，将产生单个输出元组，否则每个
	 * 组一个元组。对每个输出元组收取 cpu_tuple_cost。
	 *
	 * 注意：在这个代价模型里，AGG_SORTED 和 AGG_HASHED 的总
	 * CPU 代价完全相同，但 AGG_SORTED 的启动代价更低。若
	 * 输入路径已经适当排好序，应当优先选 AGG_SORTED
	 * （因为它没有内存溢出的风险）。只要
	 * 算出的总代价确实完全相等，就会这样 --- 但若
	 * 有舍入误差，我们可能会做错。
	 * 因此确保
	 * 下面的计算以相同顺序形成相同的中间值。
	 */
	if (aggstrategy == AGG_PLAIN)
	{
		startup_cost = input_total_cost;
		startup_cost += aggcosts->transCost.startup;
		startup_cost += aggcosts->transCost.per_tuple * input_tuples;
		startup_cost += aggcosts->finalCost.startup;
		startup_cost += aggcosts->finalCost.per_tuple;
		/* 我们不在分组 */
		total_cost = startup_cost + cpu_tuple_cost;
		output_tuples = 1;
	}
	else if (aggstrategy == AGG_SORTED || aggstrategy == AGG_MIXED)
	{
		/* 这里能够随时交付输出 */
		startup_cost = input_startup_cost;
		total_cost = input_total_cost;
		if (aggstrategy == AGG_MIXED && !enable_hashagg)
		{
			startup_cost += disable_cost;
			total_cost += disable_cost;
		}
		/* 计算这样表述是为了匹配 HASHED 情形，见上面的说明 */
		total_cost += aggcosts->transCost.startup;
		total_cost += aggcosts->transCost.per_tuple * input_tuples;
		total_cost += (cpu_operator_cost * numGroupCols) * input_tuples;
		total_cost += aggcosts->finalCost.startup;
		total_cost += aggcosts->finalCost.per_tuple * numGroups;
		total_cost += cpu_tuple_cost * numGroups;
		output_tuples = numGroups;
	}
	else
	{
		/* 必须是 AGG_HASHED */
		startup_cost = input_total_cost;
		if (!enable_hashagg)
			startup_cost += disable_cost;
		startup_cost += aggcosts->transCost.startup;
		startup_cost += aggcosts->transCost.per_tuple * input_tuples;
		/* 计算哈希值的代价 */
		startup_cost += (cpu_operator_cost * numGroupCols) * input_tuples;
		startup_cost += aggcosts->finalCost.startup;

		total_cost = startup_cost;
		total_cost += aggcosts->finalCost.per_tuple * numGroups;
		/* 从哈希表检索的代价 */
		total_cost += cpu_tuple_cost * numGroups;
		output_tuples = numGroups;
	}

	/*
	 * 加上溢出到磁盘的哈希聚合的磁盘代价。
	 *
	 * 进入哈希表的组一直留在内存里直到定稿，因此
	 * 溢出并重新处理元组不会引起额外的
	 * transCost 或 finalCost 调用。而且，算出的哈希值
	 * 与溢出的元组一起存储，因此不会额外调用
	 * 哈希函数。
	 *
	 * 哈希聚合在第一批完成之后开始返回元组。
	 * 把写（溢出的元组）计入 startup_cost 和 total_cost；
	 * 读只计入 total_cost。
	 */
	if (aggstrategy == AGG_HASHED || aggstrategy == AGG_MIXED)
	{
		double		pages;
		double		pages_written = 0.0;
		double		pages_read = 0.0;
		double		spill_cost;
		double		hashentrysize;
		double		nbatches;
		Size		mem_limit;
		uint64		ngroups_limit;
		int			num_partitions;
		int			depth;

		/*
		 * 根据算出的限制估计批次数。若小于
		 * 或等于一，预期全部组都能放进内存；
		 * 否则预期会溢出。
		 */
		hashentrysize = hash_agg_entry_size(list_length(root->aggtransinfos),
											input_width,
											aggcosts->transitionSpace);
		hash_agg_set_limits(hashentrysize, numGroups, 0, &mem_limit,
							&ngroups_limit, &num_partitions);

		nbatches = Max((numGroups * hashentrysize) / mem_limit,
					   numGroups / ngroups_limit);

		nbatches = Max(ceil(nbatches), 1.0);
		num_partitions = Max(num_partitions, 2);

		/*
		 * 分区数在不同的递归
		 * 层次可能变化；但为了这次计算，假定它保持
		 * 不变。
		 */
		depth = ceil(log(nbatches) / log(num_partitions));

		/*
		 * 估计读和写的页数。对每一层
		 * 递归，一个元组必须先写、以后再读。
		 */
		pages = relation_byte_size(input_tuples, input_width) / BLCKSZ;
		pages_written = pages_read = pages * depth;

		/*
		 * 在典型的硬件/操作系统组合上，HashAgg 的 IO 行为比 Sort 略差。
		 * 用一项通用惩罚来计入这一点。
		 */
		pages_read *= 2.0;
		pages_written *= 2.0;

		startup_cost += pages_written * random_page_cost;
		total_cost += pages_written * random_page_cost;
		total_cost += pages_read * seq_page_cost;

		/* 计入溢出一个元组再读回来的 CPU 代价 */
		spill_cost = depth * input_tuples * 2.0 * cpu_tuple_cost;
		startup_cost += spill_cost;
		total_cost += spill_cost;
	}

	/*
	 * 若有限定条件（HAVING 限定条件），计入它们的代价和
	 * 选择率。
	 */
	if (quals)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, quals, root);
		startup_cost += qual_cost.startup;
		total_cost += qual_cost.startup + output_tuples * qual_cost.per_tuple;

		output_tuples = clamp_row_est(output_tuples *
									  clauselist_selectivity(root,
															 quals,
															 0,
															 JOIN_INNER,
															 NULL));
	}

	path->rows = output_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * cost_windowagg
 *		确定并返回执行 WindowAgg 计划节点的代价，
 *		包括其输入的代价。
 *
 * 假定输入已经正确排序。
 */
void
cost_windowagg(Path *path, PlannerInfo *root,
			   List *windowFuncs, int numPartCols, int numOrderCols,
			   Cost input_startup_cost, Cost input_total_cost,
			   double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;
	ListCell   *lc;

	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * 假定窗口函数的代价是它们声明的执行代价，加上
	 * 每个元组求值其输入表达式的代价。由于它们
	 * 事实上可能在每个周期的多行上求值输入，
	 * 这可能严重低估；但没有办法知道
	 * 窗口函数会取出多少行，就很难做得更好。在
	 * 任何情况下，这对所有内建窗口函数都是好的估计，
	 * 所以目前就这样做。
	 */
	foreach(lc, windowFuncs)
	{
		WindowFunc *wfunc = lfirst_node(WindowFunc, lc);
		Cost		wfunccost;
		QualCost	argcosts;

		argcosts.startup = argcosts.per_tuple = 0;
		add_function_cost(root, wfunc->winfnoid, (Node *) wfunc,
						  &argcosts);
		startup_cost += argcosts.startup;
		wfunccost = argcosts.per_tuple;

		/* 也把输入表达式的代价加到每个输入行的代价上 */
		cost_qual_eval_node(&argcosts, (Node *) wfunc->args, root);
		startup_cost += argcosts.startup;
		wfunccost += argcosts.per_tuple;

		/*
		 * 把过滤器的代价加到每个输入行的代价上。XXX 应当根据
		 * 过滤器选择率降低输入表达式代价。
		 */
		cost_qual_eval_node(&argcosts, (Node *) wfunc->aggfilter, root);
		startup_cost += argcosts.startup;
		wfunccost += argcosts.per_tuple;

		total_cost += wfunccost * input_tuples;
	}

	/*
	 * 还对每个元组的每个分组列收取 cpu_operator_cost，用于
	 * 分组比较，再加上每个元组的 cpu_tuple_cost 作为一般
	 * 开销。
	 *
	 * XXX 这忽略了数据溢出
	 * work_mem 时写到磁盘的代价。迟早应当把那计入。
	 */
	total_cost += cpu_operator_cost * (numPartCols + numOrderCols) * input_tuples;
	total_cost += cpu_tuple_cost * input_tuples;

	path->rows = input_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * cost_group
 *		确定并返回执行 Group 计划节点的代价，
 *		包括其输入的代价。
 *
 * 注意：调用方必须确保输入代价是针对已适当排序的
 * 输入的。
 */
void
cost_group(Path *path, PlannerInfo *root,
		   int numGroupCols, double numGroups,
		   List *quals,
		   Cost input_startup_cost, Cost input_total_cost,
		   double input_tuples)
{
	double		output_tuples;
	Cost		startup_cost;
	Cost		total_cost;

	output_tuples = numGroups;
	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * 对每个输入元组的每次比较收取一次 cpu_operator_cost。假定
	 * 多数元组上所有列都会被比较。
	 */
	total_cost += cpu_operator_cost * input_tuples * numGroupCols;

	/*
	 * 若有限定条件（HAVING 限定条件），计入它们的代价和
	 * 选择率。
	 */
	if (quals)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, quals, root);
		startup_cost += qual_cost.startup;
		total_cost += qual_cost.startup + output_tuples * qual_cost.per_tuple;

		output_tuples = clamp_row_est(output_tuples *
									  clauselist_selectivity(root,
															 quals,
															 0,
															 JOIN_INNER,
															 NULL));
	}

	path->rows = output_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * initial_cost_nestloop
 *	  嵌套循环连接路径代价的初步估计。
 *
 * 这必须迅速产生该路径启动代价和
 * 总代价的下界估计。若无法用下界把提议的路径从
 * 考虑中排除，就会调用 final_cost_nestloop
 * 以取得最终估计。
 *
 * 本函数与 final_cost_nestloop 之间的分工
 * 是它们私有的，代表初始估计速度
 * 与得到较紧下界之间的折中。这里选择不检查
 * 连接限定条件，因为那是计算中最贵的部分。
 * 最终结果是 CPU 代价的考虑必须
 * 留到第二阶段；对 SEMI/ANTI 连接，还必须推迟
 * 并入内层路径的运行代价。
 *
 * 'workspace' 将被填入 startup_cost、total_cost，也许还有
 *		供 final_cost_nestloop 使用的其他数据
 * 'jointype' 是要执行的连接类型
 * 'outer_path' 是连接的外侧输入
 * 'inner_path' 是连接的内侧输入
 * 'extra' 含有关于该连接的杂项信息
 */
void
initial_cost_nestloop(PlannerInfo *root, JoinCostWorkspace *workspace,
					  JoinType jointype,
					  Path *outer_path, Path *inner_path,
					  JoinPathExtraData *extra)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		outer_path_rows = outer_path->rows;
	Cost		inner_rescan_start_cost;
	Cost		inner_rescan_total_cost;
	Cost		inner_run_cost;
	Cost		inner_rescan_run_cost;

	/* 估计重新扫描内层关系的代价 */
	cost_rescan(root, inner_path,
				&inner_rescan_start_cost,
				&inner_rescan_total_cost);

	/* 源数据的代价 */

	/*
	 * 注意：显然，在开始返回元组之前必须支付
	 * 外侧和内侧路径的 startup_cost，
	 * 因此连接的启动代价是
	 * 它们之和。还会多次支付内层路径的重新扫描启动代价。
	 */
	startup_cost += outer_path->startup_cost + inner_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	if (outer_path_rows > 1)
		run_cost += (outer_path_rows - 1) * inner_rescan_start_cost;

	inner_run_cost = inner_path->total_cost - inner_path->startup_cost;
	inner_rescan_run_cost = inner_rescan_total_cost - inner_rescan_start_cost;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
		extra->inner_unique)
	{
		/*
		 * 对 SEMI 或 ANTI 连接，或者已知内层关系唯一时，
		 * 执行器会在第一次匹配后停止。
		 *
		 * 要得到像样的估计，需要检查连接限定条件，
		 * 我们选择把这推迟到 final_cost_nestloop。
		 */

		/* 为 final_cost_nestloop 保存私有数据 */
		workspace->inner_run_cost = inner_run_cost;
		workspace->inner_rescan_run_cost = inner_rescan_run_cost;
	}
	else
	{
		/* 普通情形；对每个外侧行将扫描整个输入关系 */
		run_cost += inner_run_cost;
		if (outer_path_rows > 1)
			run_cost += (outer_path_rows - 1) * inner_rescan_run_cost;
	}

	/* CPU 代价留到以后 */

	/* 公开的结果字段 */
	workspace->startup_cost = startup_cost;
	workspace->total_cost = startup_cost + run_cost;
	/* 为 final_cost_nestloop 保存私有数据 */
	workspace->run_cost = run_cost;
}

/*
 * final_cost_nestloop
 *	  嵌套循环连接路径的代价和结果规模的最终估计。
 *
 * 'path' 除了行数和代价字段之外已经填好
 * 'workspace' 是 initial_cost_nestloop 的结果
 * 'extra' 含有关于该连接的杂项信息
 */
void
final_cost_nestloop(PlannerInfo *root, NestPath *path,
					JoinCostWorkspace *workspace,
					JoinPathExtraData *extra)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	Cost		startup_cost = workspace->startup_cost;
	Cost		run_cost = workspace->run_cost;
	Cost		cpu_per_tuple;
	QualCost	restrict_qual_cost;
	double		ntuples;

	/* 保护下面关于行数不为零的一些假定 */
	if (outer_path_rows <= 0)
		outer_path_rows = 1;
	if (inner_path_rows <= 0)
		inner_path_rows = 1;
	/* 用正确的行数估计标记该路径 */
	if (path->jpath.path.param_info)
		path->jpath.path.rows = path->jpath.path.param_info->ppi_rows;
	else
		path->jpath.path.rows = path->jpath.path.parent->rows;

	/* 对部分路径，缩放行数估计。 */
	if (path->jpath.path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->jpath.path);

		path->jpath.path.rows =
			clamp_row_est(path->jpath.path.rows / parallel_divisor);
	}

	/*
	 * 可以把 disable_cost 包括进初步估计，但那
	 * 等于针对连接方法被
	 * 禁用的情形做优化，似乎不该押在那上面。
	 */
	if (!enable_nestloop)
		startup_cost += disable_cost;

	/* 内层关系源数据的代价（外侧关系已经处理过） */

	if (path->jpath.jointype == JOIN_SEMI || path->jpath.jointype == JOIN_ANTI ||
		extra->inner_unique)
	{
		/*
		 * 对 SEMI 或 ANTI 连接，或者已知内层关系唯一时，
		 * 执行器会在第一次匹配后停止。
		 */
		Cost		inner_run_cost = workspace->inner_run_cost;
		Cost		inner_rescan_run_cost = workspace->inner_rescan_run_cost;
		double		outer_matched_rows;
		double		outer_unmatched_rows;
		Selectivity inner_scan_frac;

		/*
		 * 对至少有一次匹配的外侧行，可以预期
		 * 内层扫描在内层行的 1/(match_count+1) 这个比例之后停止，
		 * 若匹配均匀分布。由于它们大概
		 * 并不十分均匀，对该比例施加 2.0 的模糊因子。
		 * （若用更大的模糊因子，就必须把
		 * inner_scan_frac 钳制到至多 1.0；但既然 match_count 至少
		 * 为 1，现在不需要这样的钳制。）
		 */
		outer_matched_rows = rint(outer_path_rows * extra->semifactors.outer_match_frac);
		outer_unmatched_rows = outer_path_rows - outer_matched_rows;
		inner_scan_frac = 2.0 / (extra->semifactors.match_count + 1.0);

		/*
		 * 计算处理的元组数（不是发出的个数！）。首先，
		 * 计入成功匹配的外侧行。
		 */
		ntuples = outer_matched_rows * inner_path_rows * inner_scan_frac;

		/*
		 * 现在需要估计扫描内层
		 * 关系的实际代价，由于提前停止扫描，这可能比 N 乘 inner_run_cost
		 * 小不少。考虑两种情形。若内层路径
		 * 是把全部连接限定条件都用作索引限定条件的索引扫描，那么
		 * 未匹配的外侧行会导致索引扫描不返回行，
		 * 这大概相当便宜。否则，执行器将不得不
		 * 为未匹配的行扫描整个内层关系；没那么便宜。
		 */
		if (has_indexed_join_quals(path))
		{
			/*
			 * 成功匹配的外侧行只需要扫描
			 * 内层关系的 inner_scan_frac。这种情况下，即使 inner_run_cost
			 * 高于 inner_rescan_run_cost，也不
			 * 需要收取全部 inner_run_cost，因为可以假定没有任何
			 * 内层扫描会扫完整个内层关系。因此
			 * 可以假定所有内层扫描执行都可以是
			 * 全部代价的一个比例，即使物化正在降低
			 * 重新扫描代价。写这段时，物化的内层扫描不可能走到这里，
			 * 因此 inner_run_cost 和
			 * inner_rescan_run_cost 反正会相同；但以防万一，
			 * 对第一个匹配的元组用 inner_run_cost，对
			 * 其余的用 inner_rescan_run_cost。
			 */
			run_cost += inner_run_cost * inner_scan_frac;
			if (outer_matched_rows > 1)
				run_cost += (outer_matched_rows - 1) * inner_rescan_run_cost * inner_scan_frac;

			/*
			 * 加上未匹配外侧行的内层扫描执行代价。
			 * 把这估计成与返回非空扫描的第一个元组
			 * 相同的代价。认为这些都是重新扫描，
			 * 因为已经用过一次 inner_run_cost。
			 */
			run_cost += outer_unmatched_rows *
				inner_rescan_run_cost / inner_path_rows;

			/*
			 * 对未匹配的行根本不会求值任何限定条件，因此
			 * 不要把它们加进 ntuples。
			 */
		}
		else
		{
			/*
			 * 这里一个复杂因素是重新扫描可能比
			 * 第一次扫描更便宜。若从未扫到内层关系的
			 * 末尾，有可能（取决于计划类型）永远不会
			 * 支付全部的内层第一次扫描运行代价。不过
			 * 很难估计那会不会发生
			 * （而且若有任何未匹配的外侧行，
			 * 它就不会发生！），因此保守一点，总是把全部第一次扫描代价收取一次。
			 * 认为这笔费用对应第一个未匹配的
			 * 外侧行，除非估计里没有那样的行，那时
			 * 就把它算到第一个匹配的行上。
			 */

			/* 首先，把所有未匹配的连接元组计为已处理 */
			ntuples += outer_unmatched_rows * inner_path_rows;

			/* 现在加上强制的全扫描，并递减相应的计数 */
			run_cost += inner_run_cost;
			if (outer_unmatched_rows >= 1)
				outer_unmatched_rows -= 1;
			else
				outer_matched_rows -= 1;

			/* 为另外有匹配的外侧元组加上内层运行代价 */
			if (outer_matched_rows > 0)
				run_cost += outer_matched_rows * inner_rescan_run_cost * inner_scan_frac;

			/* 为另外未匹配的外侧元组加上内层运行代价 */
			if (outer_unmatched_rows > 0)
				run_cost += outer_unmatched_rows * inner_rescan_run_cost;
		}
	}
	else
	{
		/* 普通情形的源代价已包括在初步估计中 */

		/* 计算处理的元组数（不是发出的个数！） */
		ntuples = outer_path_rows * inner_path_rows;
	}

	/* CPU 代价 */
	cost_qual_eval(&restrict_qual_cost, path->jpath.joinrestrictinfo, root);
	startup_cost += restrict_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + restrict_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->jpath.path.pathtarget->cost.startup;
	run_cost += path->jpath.path.pathtarget->cost.per_tuple * path->jpath.path.rows;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}

/*
 * initial_cost_mergejoin
 *	  归并连接路径代价的初步估计。
 *
 * 这必须迅速产生该路径启动代价和
 * 总代价的下界估计。若无法用下界把提议的路径从
 * 考虑中排除，就会调用 final_cost_mergejoin
 * 以取得最终估计。
 *
 * 本函数与 final_cost_mergejoin 之间的分工
 * 是它们私有的，代表初始估计速度
 * 与得到较紧下界之间的折中。这里选择不检查
 * 连接限定条件，除了取得扫描选择率估计，那
 * 确实必不可少（但幸好，利用缓存能把取得它的代价
 * 降到合理的程度）。
 * 还假定 cost_sort 便宜到可以在这里使用。
 *
 * 'workspace' 将被填入 startup_cost、total_cost，也许还有
 *		供 final_cost_mergejoin 使用的其他数据
 * 'jointype' 是要执行的连接类型
 * 'mergeclauses' 是要用作归并子句的连接子句列表
 * 'outer_path' 是连接的外侧输入
 * 'inner_path' 是连接的内侧输入
 * 'outersortkeys' 是外侧路径的排序键列表
 * 'innersortkeys' 是内侧路径的排序键列表
 * 'extra' 含有关于该连接的杂项信息
 *
 * 注意：若因为相应的源路径已经有序而不需要显式
 * 排序，outersortkeys 和 innersortkeys 应当为 NIL。
 */
void
initial_cost_mergejoin(PlannerInfo *root, JoinCostWorkspace *workspace,
					   JoinType jointype,
					   List *mergeclauses,
					   Path *outer_path, Path *inner_path,
					   List *outersortkeys, List *innersortkeys,
					   JoinPathExtraData *extra)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	Cost		inner_run_cost;
	double		outer_rows,
				inner_rows,
				outer_skip_rows,
				inner_skip_rows;
	Selectivity outerstartsel,
				outerendsel,
				innerstartsel,
				innerendsel;
	Path		sort_path;		/* cost_sort 结果的占位 */

	/* 保护下面关于行数不为零的一些假定 */
	if (outer_path_rows <= 0)
		outer_path_rows = 1;
	if (inner_path_rows <= 0)
		inner_path_rows = 1;

	/*
	 * 归并连接会在任一输入流耗尽时停止
	 * （除非是外连接，那时外侧反正必须
	 * 一直扫完）。估计左右
	 * 输入中实际需要扫描的比例。同样，可以
	 * 估计找到第一对连接之前会跳过的行数，
	 * 这应当计入启动代价。为此只用
	 * 第一条（最重要的）归并子句。由于
	 * mergejoinscansel() 是相当贵的计算，我们把
	 * 结果缓存在归并子句的 RestrictInfo 中。
	 */
	if (mergeclauses && jointype != JOIN_FULL)
	{
		RestrictInfo *firstclause = (RestrictInfo *) linitial(mergeclauses);
		List	   *opathkeys;
		List	   *ipathkeys;
		PathKey    *opathkey;
		PathKey    *ipathkey;
		MergeScanSelCache *cache;

		/* 取得输入路径键以确定排序细节 */
		opathkeys = outersortkeys ? outersortkeys : outer_path->pathkeys;
		ipathkeys = innersortkeys ? innersortkeys : inner_path->pathkeys;
		Assert(opathkeys);
		Assert(ipathkeys);
		opathkey = (PathKey *) linitial(opathkeys);
		ipathkey = (PathKey *) linitial(ipathkeys);
		/* 调试检查 */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation ||
			opathkey->pk_strategy != ipathkey->pk_strategy ||
			opathkey->pk_nulls_first != ipathkey->pk_nulls_first)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* 带缓存地取得选择率 */
		cache = cached_scansel(root, firstclause, opathkey);

		if (bms_is_subset(firstclause->left_relids,
						  outer_path->parent->relids))
		{
			/* 子句左侧是外侧 */
			outerstartsel = cache->leftstartsel;
			outerendsel = cache->leftendsel;
			innerstartsel = cache->rightstartsel;
			innerendsel = cache->rightendsel;
		}
		else
		{
			/* 子句左侧是内侧 */
			outerstartsel = cache->rightstartsel;
			outerendsel = cache->rightendsel;
			innerstartsel = cache->leftstartsel;
			innerendsel = cache->leftendsel;
		}
		if (jointype == JOIN_LEFT ||
			jointype == JOIN_ANTI)
		{
			outerstartsel = 0.0;
			outerendsel = 1.0;
		}
		else if (jointype == JOIN_RIGHT ||
				 jointype == JOIN_RIGHT_ANTI)
		{
			innerstartsel = 0.0;
			innerendsel = 1.0;
		}
	}
	else
	{
		/* 应付无子句或完全归并连接 */
		outerstartsel = innerstartsel = 0.0;
		outerendsel = innerendsel = 1.0;
	}

	/*
	 * 把选择率转换成行数。强制 outer_rows 和
	 * inner_rows 至少为 1，但 skip_rows 估计可以为零。
	 */
	outer_skip_rows = rint(outer_path_rows * outerstartsel);
	inner_skip_rows = rint(inner_path_rows * innerstartsel);
	outer_rows = clamp_row_est(outer_path_rows * outerendsel);
	inner_rows = clamp_row_est(inner_path_rows * innerendsel);

	Assert(outer_skip_rows <= outer_rows);
	Assert(inner_skip_rows <= inner_rows);

	/*
	 * 重新调整扫描选择率以计入上面的舍入。这
	 * 通常影响很小，但当输入只有很少几行时，
	 * 不做这件事会造成很大的百分比误差。
	 */
	outerstartsel = outer_skip_rows / outer_path_rows;
	innerstartsel = inner_skip_rows / inner_path_rows;
	outerendsel = outer_rows / outer_path_rows;
	innerendsel = inner_rows / inner_path_rows;

	Assert(outerstartsel <= outerendsel);
	Assert(innerstartsel <= innerendsel);

	/* 源数据的代价 */

	if (outersortkeys)			/* 需要对外侧排序吗？ */
	{
		cost_sort(&sort_path,
				  root,
				  outersortkeys,
				  outer_path->total_cost,
				  outer_path_rows,
				  outer_path->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);
		startup_cost += sort_path.startup_cost;
		startup_cost += (sort_path.total_cost - sort_path.startup_cost)
			* outerstartsel;
		run_cost += (sort_path.total_cost - sort_path.startup_cost)
			* (outerendsel - outerstartsel);
	}
	else
	{
		startup_cost += outer_path->startup_cost;
		startup_cost += (outer_path->total_cost - outer_path->startup_cost)
			* outerstartsel;
		run_cost += (outer_path->total_cost - outer_path->startup_cost)
			* (outerendsel - outerstartsel);
	}

	if (innersortkeys)			/* 需要对内侧排序吗？ */
	{
		cost_sort(&sort_path,
				  root,
				  innersortkeys,
				  inner_path->total_cost,
				  inner_path_rows,
				  inner_path->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);
		startup_cost += sort_path.startup_cost;
		startup_cost += (sort_path.total_cost - sort_path.startup_cost)
			* innerstartsel;
		inner_run_cost = (sort_path.total_cost - sort_path.startup_cost)
			* (innerendsel - innerstartsel);
	}
	else
	{
		startup_cost += inner_path->startup_cost;
		startup_cost += (inner_path->total_cost - inner_path->startup_cost)
			* innerstartsel;
		inner_run_cost = (inner_path->total_cost - inner_path->startup_cost)
			* (innerendsel - innerstartsel);
	}

	/*
	 * 还不能确定是否会发生重新扫描，或者是否
	 * 应当物化内侧输入。无论重新扫描和物化
	 * 如何考虑，内层输入可能的最小
	 * 代价是 inner_run_cost。把它包括进
	 * workspace->total_cost，但还不要放进 run_cost。
	 */

	/* CPU 代价留到以后 */

	/* 公开的结果字段 */
	workspace->startup_cost = startup_cost;
	workspace->total_cost = startup_cost + run_cost + inner_run_cost;
	/* 为 final_cost_mergejoin 保存私有数据 */
	workspace->run_cost = run_cost;
	workspace->inner_run_cost = inner_run_cost;
	workspace->outer_rows = outer_rows;
	workspace->inner_rows = inner_rows;
	workspace->outer_skip_rows = outer_skip_rows;
	workspace->inner_skip_rows = inner_skip_rows;
}

/*
 * final_cost_mergejoin
 *	  归并连接路径的代价和结果规模的最终估计。
 *
 * 与其他 costsize 函数不同，本例程做两个实际决定：
 * 执行器是否需要做标记/恢复，以及是否应当
 * 物化内层路径。为这些备选方案分别建立
 * 路径在逻辑上更干净，但那需要重复
 * 大部分代价计算，而那些并不都很便宜。既然这个
 * 选择不影响输出路径键或启动代价，只影响总代价，
 * 就不可能想保留不止一条路径。因此似乎
 * 最好在这里做决定，并记到路径的
 * skip_mark_restore 和 materialize_inner 字段里。
 *
 * 通常需要标记/恢复开销，但若知道
 * 执行器对每个外侧元组只需找到一次匹配，并且
 * 归并子句足以识别匹配，就可以跳过。
 *
 * 若需要标记/恢复，并且内层
 * 路径不能支持标记/恢复，或者插入一个
 * Material 节点来处理标记/恢复更便宜，就物化内层路径。
 *
 * 'path' 除了行数和代价字段以及
 *		skip_mark_restore 和 materialize_inner 之外已经填好
 * 'workspace' 是 initial_cost_mergejoin 的结果
 * 'extra' 含有关于该连接的杂项信息
 */
void
final_cost_mergejoin(PlannerInfo *root, MergePath *path,
					 JoinCostWorkspace *workspace,
					 JoinPathExtraData *extra)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	double		inner_path_rows = inner_path->rows;
	List	   *mergeclauses = path->path_mergeclauses;
	List	   *innersortkeys = path->innersortkeys;
	Cost		startup_cost = workspace->startup_cost;
	Cost		run_cost = workspace->run_cost;
	Cost		inner_run_cost = workspace->inner_run_cost;
	double		outer_rows = workspace->outer_rows;
	double		inner_rows = workspace->inner_rows;
	double		outer_skip_rows = workspace->outer_skip_rows;
	double		inner_skip_rows = workspace->inner_skip_rows;
	Cost		cpu_per_tuple,
				bare_inner_cost,
				mat_inner_cost;
	QualCost	merge_qual_cost;
	QualCost	qp_qual_cost;
	double		mergejointuples,
				rescannedtuples;
	double		rescanratio;

	/* 保护下面关于行数不为零的一些假定 */
	if (inner_path_rows <= 0)
		inner_path_rows = 1;

	/* 用正确的行数估计标记该路径 */
	if (path->jpath.path.param_info)
		path->jpath.path.rows = path->jpath.path.param_info->ppi_rows;
	else
		path->jpath.path.rows = path->jpath.path.parent->rows;

	/* 对部分路径，缩放行数估计。 */
	if (path->jpath.path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->jpath.path);

		path->jpath.path.rows =
			clamp_row_est(path->jpath.path.rows / parallel_divisor);
	}

	/*
	 * 可以把 disable_cost 包括进初步估计，但那
	 * 等于针对连接方法被
	 * 禁用的情形做优化，似乎不该押在那上面。
	 */
	if (!enable_mergejoin)
		startup_cost += disable_cost;

	/*
	 * 分别计算归并限定条件和 qpqual（其他限制子句）的
	 * 代价。
	 */
	cost_qual_eval(&merge_qual_cost, mergeclauses, root);
	cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo, root);
	qp_qual_cost.startup -= merge_qual_cost.startup;
	qp_qual_cost.per_tuple -= merge_qual_cost.per_tuple;

	/*
	 * 对 SEMI 或 ANTI 连接，或者已知内层关系唯一时，
	 * 执行器会在第一次匹配后停止扫描匹配。当
	 * 全部连接子句都是归并子句时，这意味着永远不需要
	 * 回退归并，因此可以跳过标记/恢复开销。
	 */
	if ((path->jpath.jointype == JOIN_SEMI ||
		 path->jpath.jointype == JOIN_ANTI ||
		 extra->inner_unique) &&
		(list_length(path->jpath.joinrestrictinfo) ==
		 list_length(path->path_mergeclauses)))
		path->skip_mark_restore = true;
	else
		path->skip_mark_restore = false;

	/*
	 * 取得通过归并限定条件的近似元组数。这里用 approx_tuple_count，
	 * 因为需要按 JOIN_INNER 语义做的估计。
	 */
	mergejointuples = approx_tuple_count(root, &path->jpath, mergeclauses);

	/*
	 * 当外侧关系中有相等的归并键时，归并连接
	 * 必须重新扫描内层关系中任何匹配的元组。这意味着
	 * 重新取出内层元组；必须估计那发生得多频繁。
	 *
	 * 对普通的内连接和外连接，重新取出的次数可以
	 * 近似估计为归并连接输出规模减去内层
	 * 关系规模。假定不同的键值是 1、2、……，并把
	 * 外侧关系中每个键的值的个数记为 m1、
	 * m2、……；内层关系中为 n1、n2、……。于是有
	 *
	 * 连接规模 = m1 * n1 + m2 * n2 + ...
	 *
	 * 重新扫描的元组数 = (m1 - 1) * n1 + (m2 - 1) * n2 + ... = m1 *
	 * n1 + m2 * n2 + ... - (n1 + n2 + ...) = 连接规模 - 内层
	 * 关系规模
	 *
	 * 这个等式对没有内层匹配的外侧元组是对的
	 * （nk = 0），但对没有外侧匹配的内层元组不对（mk = 0）；我们
	 * 实际上从重新扫描的元组数中减去了那些，
	 * 而本不该减。不做昂贵的选择率
	 * 计算，能做得更好吗？
	 *
	 * 若我们处理的是已唯一化的外侧
	 * 输入，或者知道根本不需要标记/恢复，整个问题就无关了。
	 */
	if (IsA(outer_path, UniquePath) || path->skip_mark_restore)
		rescannedtuples = 0;
	else
	{
		rescannedtuples = mergejointuples - inner_path_rows;
		/* 必须钳制，因为可能低估 */
		if (rescannedtuples < 0)
			rescannedtuples = 0;
	}

	/*
	 * 将把各种代价膨胀这么多以计入重新扫描。注意
	 * 这要乘上某个涉及 inner_rows 的东西，或
	 * 与我们将扫描的内层关系那一部分相关的另一个数。
	 */
	rescanratio = 1.0 + (rescannedtuples / inner_rows);

	/*
	 * 决定是否要物化内侧输入，以把它与
	 * 标记/恢复和执行重新取出隔开。我们对普通
	 * 重新取出的代价模型是，重新取出与原来取出代价相同，
	 * 这大概是高估；但另一方面我们忽略了
	 * 标记/恢复的簿记代价。不清楚是否值得发展
	 * 更精细的模型。因此只需把内层运行代价按
	 * rescanratio 膨胀。
	 */
	bare_inner_cost = inner_run_cost * rescanratio;

	/*
	 * 插入 Material 节点时，假定重新取出代价
	 * 只是每个元组 cpu_operator_cost，与底层
	 * 计划的代价无关；对原来的
	 * 取出也额外收取一次 cpu_operator_cost。注意我们假定物化节点
	 * 从不溢出到磁盘，因为它只需记住回到
	 * 上一个标记的元组。（若有大量重复，我们的其他代价
	 * 因素会让这条路径贵到大概反正不会被
	 * 选中。）因此这里不用 cost_rescan。
	 *
	 * 注意：让这个估计与 create_mergejoin_plan 给生成的 Material 节点
	 * 贴的标签保持同步。
	 */
	mat_inner_cost = inner_run_cost +
		cpu_operator_cost * inner_rows * rescanratio;

	/*
	 * 若根本不需要标记/恢复，就不需要物化。
	 */
	if (path->skip_mark_restore)
		path->materialize_inner = false;

	/*
	 * 若看起来更便宜就优先物化，除非用户要求
	 * 抑制物化。
	 */
	else if (enable_material && mat_inner_cost < bare_inner_cost)
		path->materialize_inner = true;

	/*
	 * 即使物化看起来并不更便宜，若内层路径
	 * 要被直接使用（不排序）并且它不
	 * 支持标记/恢复，就 *必须* 物化。
	 *
	 * 既然内侧必须有序，而起初只有 Sort 和 IndexScans 能
	 * 建立顺序，并且它们都支持标记/恢复，你
	 * 也许觉得没问题 --- 但你错了。嵌套循环和
	 * 归并连接可以 *保持* 其输入的顺序，因此它们可以被
	 * 选为归并连接的输入，而它们目前不支持
	 * 标记/恢复。
	 *
	 * 这里不测试 enable_material 的值，因为
	 * 这种情况下物化是正确性所必需的，关掉
	 * 它并不允许我们交付无效计划。
	 */
	else if (innersortkeys == NIL &&
			 !ExecSupportsMarkRestore(inner_path))
		path->materialize_inner = true;

	/*
	 * 另外，若内层路径要被排序，并且
	 * 预期排序会溢出到磁盘，就强制物化。
	 * 这是因为若最终归并趟不必支持标记/恢复，
	 * 就可以随时完成。
	 * 不过不为这一考虑调整代价估计。
	 *
	 * 既然这种情况下物化是性能优化，
	 * 而不是正确性所必需的，若 enable_material 为
	 * 关就跳过它。
	 */
	else if (enable_material && innersortkeys != NIL &&
			 relation_byte_size(inner_path_rows,
								inner_path->pathtarget->width) >
			 (work_mem * 1024L))
		path->materialize_inner = true;
	else
		path->materialize_inner = false;

	/* 为所选情形收取正确的增量代价 */
	if (path->materialize_inner)
		run_cost += mat_inner_cost;
	else
		run_cost += bare_inner_cost;

	/* CPU 代价 */

	/*
	 * 所需的元组比较次数大约是外侧
	 * 行数加上内侧行数再加上重新扫描的元组数（能
	 * 把这细化吗？）。每一次都需要求值归并连接限定条件。
	 */
	startup_cost += merge_qual_cost.startup;
	startup_cost += merge_qual_cost.per_tuple *
		(outer_skip_rows + inner_skip_rows * rescanratio);
	run_cost += merge_qual_cost.per_tuple *
		((outer_rows - outer_skip_rows) +
		 (inner_rows - inner_skip_rows) * rescanratio);

	/*
	 * 对每个通过归并连接本身的元组，收取
	 * cpu_tuple_cost 加上求值要在连接处应用的额外限制
	 * 子句的代价。（这是悲观的，因为
	 * 并非所有限定条件都会在每个元组上求值。）
	 *
	 * 注意：可以调整以计入 SEMI/ANTI 连接跳过一些限定条件
	 * 求值，但大概不值得费这个事。
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * mergejointuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->jpath.path.pathtarget->cost.startup;
	run_cost += path->jpath.path.pathtarget->cost.per_tuple * path->jpath.path.rows;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}

/*
 * 带缓存地运行 mergejoinscansel()
 */
static MergeScanSelCache *
cached_scansel(PlannerInfo *root, RestrictInfo *rinfo, PathKey *pathkey)
{
	MergeScanSelCache *cache;
	ListCell   *lc;
	Selectivity leftstartsel,
				leftendsel,
				rightstartsel,
				rightendsel;
	MemoryContext oldcontext;

	/* 已经有这个结果了吗？ */
	foreach(lc, rinfo->scansel_cache)
	{
		cache = (MergeScanSelCache *) lfirst(lc);
		if (cache->opfamily == pathkey->pk_opfamily &&
			cache->collation == pathkey->pk_eclass->ec_collation &&
			cache->strategy == pathkey->pk_strategy &&
			cache->nulls_first == pathkey->pk_nulls_first)
			return cache;
	}

	/* 没有，做计算 */
	mergejoinscansel(root,
					 (Node *) rinfo->clause,
					 pathkey->pk_opfamily,
					 pathkey->pk_strategy,
					 pathkey->pk_nulls_first,
					 &leftstartsel,
					 &leftendsel,
					 &rightstartsel,
					 &rightendsel);

	/* 把结果缓存在寿命足够长的工作区里 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	cache = (MergeScanSelCache *) palloc(sizeof(MergeScanSelCache));
	cache->opfamily = pathkey->pk_opfamily;
	cache->collation = pathkey->pk_eclass->ec_collation;
	cache->strategy = pathkey->pk_strategy;
	cache->nulls_first = pathkey->pk_nulls_first;
	cache->leftstartsel = leftstartsel;
	cache->leftendsel = leftendsel;
	cache->rightstartsel = rightstartsel;
	cache->rightendsel = rightendsel;

	rinfo->scansel_cache = lappend(rinfo->scansel_cache, cache);

	MemoryContextSwitchTo(oldcontext);

	return cache;
}

/*
 * initial_cost_hashjoin
 *	  哈希连接路径代价的初步估计。
 *
 * 这必须迅速产生该路径启动代价和
 * 总代价的下界估计。若无法用下界把提议的路径从
 * 考虑中排除，就会调用 final_cost_hashjoin
 * 以取得最终估计。
 *
 * 本函数与 final_cost_hashjoin 之间的分工
 * 是它们私有的，代表初始估计速度
 * 与得到较紧下界之间的折中。这里选择不检查
 * 连接限定条件（除了数哈希子句的个数），
 * 因此对 CPU 代价做不了多少。假定
 * ExecChooseHashTableSize 便宜到可以在这里使用。
 *
 * 'workspace' 将被填入 startup_cost、total_cost，也许还有
 *		供 final_cost_hashjoin 使用的其他数据
 * 'jointype' 是要执行的连接类型
 * 'hashclauses' 是要用作哈希子句的连接子句列表
 * 'outer_path' 是连接的外侧输入
 * 'inner_path' 是连接的内侧输入
 * 'extra' 含有关于该连接的杂项信息
 * 'parallel_hash' 表示 inner_path 是部分的，并且将并行建立共享
 *		哈希表
 */
void
initial_cost_hashjoin(PlannerInfo *root, JoinCostWorkspace *workspace,
					  JoinType jointype,
					  List *hashclauses,
					  Path *outer_path, Path *inner_path,
					  JoinPathExtraData *extra,
					  bool parallel_hash)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	double		inner_path_rows_total = inner_path_rows;
	int			num_hashclauses = list_length(hashclauses);
	int			numbuckets;
	int			numbatches;
	int			num_skew_mcvs;
	size_t		space_allowed;	/* 未使用 */

	/* 源数据的代价 */
	startup_cost += outer_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	startup_cost += inner_path->total_cost;

	/*
	 * 计算哈希函数的代价：必须对每个输入元组做一次。对
	 * 每列的哈希函数收取一次 cpu_operator_cost。另外，
	 * 对每个内侧行贴上一次 cpu_tuple_cost，以模拟把
	 * 行插入哈希表的代价。
	 *
	 * XXX 当哈希子句比单个操作符更复杂时，其实
	 * 应当在这里收取左侧或右侧适当的额外求值代价。
	 * 目前这似乎比它的价值更费事。
	 */
	startup_cost += (cpu_operator_cost * num_hashclauses + cpu_tuple_cost)
		* inner_path_rows;
	run_cost += cpu_operator_cost * num_hashclauses * outer_path_rows;

	/*
	 * 若这是并行哈希建立，那么 inner_rows_total 目前的值
	 * 只指每个
	 * 参与者返回的行。为了估计共享哈希表大小，需要总数，
	 * 因此需要撤销那次除法。
	 */
	if (parallel_hash)
		inner_path_rows_total *= get_parallel_divisor(inner_path);

	/*
	 * 取得执行器会对内层关系使用的哈希表大小。
	 *
	 * XXX 目前总是假定会执行倾斜优化。
	 * 只要 SKEW_HASH_MEM_PERCENT 很小，就不值得
	 * 设法确定这一点。
	 *
	 * XXX 将来也许有兴趣试着在代价估计中计入倾斜
	 * 优化，但目前不这样做。
	 */
	ExecChooseHashTableSize(inner_path_rows_total,
							inner_path->pathtarget->width,
							true,	/* useskew（是否做倾斜优化） */
							parallel_hash,	/* try_combined_hash_mem（是否尝试合并哈希内存） */
							outer_path->parallel_workers,
							&space_allowed,
							&numbuckets,
							&numbatches,
							&num_skew_mcvs);

	/*
	 * 若内层关系太大，就需要对连接分 "批"，
	 * 这意味着要把大部分元组额外写到磁盘并读一次。
	 * 按每页收取 seq_page_cost，因为 I/O 应当又好又
	 * 顺序。写内层关系算作启动代价，其余全部
	 * 算作运行代价。
	 */
	if (numbatches > 1)
	{
		double		outerpages = page_size(outer_path_rows,
										   outer_path->pathtarget->width);
		double		innerpages = page_size(inner_path_rows,
										   inner_path->pathtarget->width);

		startup_cost += seq_page_cost * innerpages;
		run_cost += seq_page_cost * (innerpages + 2 * outerpages);
	}

	/* CPU 代价留到以后 */

	/* 公开的结果字段 */
	workspace->startup_cost = startup_cost;
	workspace->total_cost = startup_cost + run_cost;
	/* 为 final_cost_hashjoin 保存私有数据 */
	workspace->run_cost = run_cost;
	workspace->numbuckets = numbuckets;
	workspace->numbatches = numbatches;
	workspace->inner_rows_total = inner_path_rows_total;
}

/*
 * final_cost_hashjoin
 *	  哈希连接路径的代价和结果规模的最终估计。
 *
 * 注意：numbatches 估计也存进 'path' 供以后使用
 *
 * 'path' 除了行数和代价字段以及
 *		num_batches
 * 'workspace' 是 initial_cost_hashjoin 的结果
 * 'extra' 含有关于该连接的杂项信息
 */
void
final_cost_hashjoin(PlannerInfo *root, HashPath *path,
					JoinCostWorkspace *workspace,
					JoinPathExtraData *extra)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	double		inner_path_rows_total = workspace->inner_rows_total;
	List	   *hashclauses = path->path_hashclauses;
	Cost		startup_cost = workspace->startup_cost;
	Cost		run_cost = workspace->run_cost;
	int			numbuckets = workspace->numbuckets;
	int			numbatches = workspace->numbatches;
	Cost		cpu_per_tuple;
	QualCost	hash_qual_cost;
	QualCost	qp_qual_cost;
	double		hashjointuples;
	double		virtualbuckets;
	Selectivity innerbucketsize;
	Selectivity innermcvfreq;
	ListCell   *hcl;

	/* 用正确的行数估计标记该路径 */
	if (path->jpath.path.param_info)
		path->jpath.path.rows = path->jpath.path.param_info->ppi_rows;
	else
		path->jpath.path.rows = path->jpath.path.parent->rows;

	/* 对部分路径，缩放行数估计。 */
	if (path->jpath.path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->jpath.path);

		path->jpath.path.rows =
			clamp_row_est(path->jpath.path.rows / parallel_divisor);
	}

	/*
	 * 可以把 disable_cost 包括进初步估计，但那
	 * 等于针对连接方法被
	 * 禁用的情形做优化，似乎不该押在那上面。
	 */
	if (!enable_hashjoin)
		startup_cost += disable_cost;

	/* 用估计的批次数标记该路径 */
	path->num_batches = numbatches;

	/* 存储元组总数（部分行数估计之和） */
	path->inner_rows_total = inner_path_rows_total;

	/* 并计算整个连接中 "虚拟" 桶的个数 */
	virtualbuckets = (double) numbuckets * (double) numbatches;

	/*
	 * 确定内层关系的桶大小比例和 MCV 频率。
	 * 使用任何
	 * 单个哈希子句估计出的最小桶大小或 MCV 频率；这无疑是保守的。
	 *
	 * 但是：若内层关系已被唯一化，可以假定它对
	 * 哈希是好的。这很重要，既因为这是正确答案，也
	 * 因为我们避免用一个对非唯一化路径是错的值
	 * 污染缓存。
	 */
	if (IsA(inner_path, UniquePath))
	{
		innerbucketsize = 1.0 / virtualbuckets;
		innermcvfreq = 0.0;
	}
	else
	{
		innerbucketsize = 1.0;
		innermcvfreq = 1.0;
		foreach(hcl, hashclauses)
		{
			RestrictInfo *restrictinfo = lfirst_node(RestrictInfo, hcl);
			Selectivity thisbucketsize;
			Selectivity thismcvfreq;

			/*
			 * 首先必须弄清哈希连接子句的哪一侧
			 * 是内侧。
			 *
			 * 由于规划大查询时往往会反复访问相同的子句，
			 * 把桶统计估计缓存在
			 * RestrictInfo 节点中，以避免重复查找统计信息。
			 */
			if (bms_is_subset(restrictinfo->right_relids,
							  inner_path->parent->relids))
			{
				/* 右侧是内侧 */
				thisbucketsize = restrictinfo->right_bucketsize;
				if (thisbucketsize < 0)
				{
					/* 尚未缓存 */
					estimate_hash_bucket_stats(root,
											   get_rightop(restrictinfo->clause),
											   virtualbuckets,
											   &restrictinfo->right_mcvfreq,
											   &restrictinfo->right_bucketsize);
					thisbucketsize = restrictinfo->right_bucketsize;
				}
				thismcvfreq = restrictinfo->right_mcvfreq;
			}
			else
			{
				Assert(bms_is_subset(restrictinfo->left_relids,
									 inner_path->parent->relids));
				/* 左侧是内侧 */
				thisbucketsize = restrictinfo->left_bucketsize;
				if (thisbucketsize < 0)
				{
					/* 尚未缓存 */
					estimate_hash_bucket_stats(root,
											   get_leftop(restrictinfo->clause),
											   virtualbuckets,
											   &restrictinfo->left_mcvfreq,
											   &restrictinfo->left_bucketsize);
					thisbucketsize = restrictinfo->left_bucketsize;
				}
				thismcvfreq = restrictinfo->left_mcvfreq;
			}

			if (innerbucketsize > thisbucketsize)
				innerbucketsize = thisbucketsize;
			if (innermcvfreq > thismcvfreq)
				innermcvfreq = thismcvfreq;
		}
	}

	/*
	 * 若持有内侧 MCV 的桶会超过 hash_mem，除非真的没有其他选择，
	 * 否则不想哈希，因此应用
	 * disable_cost。（执行器通常通过拆分批次来应付过多的内存使用，
	 * 但显然它无法用那种方式分开相等的值，
	 * 因此当这一点为真时，它将无法把批次大小压到 hash_mem
	 * 以下。）
	 */
	if (relation_byte_size(clamp_row_est(inner_path_rows * innermcvfreq),
						   inner_path->pathtarget->width) > get_hash_memory_limit())
		startup_cost += disable_cost;

	/*
	 * 分别计算哈希限定条件和 qpqual（其他限制子句）的
	 * 代价。
	 */
	cost_qual_eval(&hash_qual_cost, hashclauses, root);
	cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo, root);
	qp_qual_cost.startup -= hash_qual_cost.startup;
	qp_qual_cost.per_tuple -= hash_qual_cost.per_tuple;

	/* CPU 代价 */

	if (path->jpath.jointype == JOIN_SEMI ||
		path->jpath.jointype == JOIN_ANTI ||
		extra->inner_unique)
	{
		double		outer_matched_rows;
		Selectivity inner_scan_frac;

		/*
		 * 对 SEMI 或 ANTI 连接，或者已知内层关系唯一时，
		 * 执行器会在第一次匹配后停止。
		 *
		 * 对至少有一次匹配的外侧行，可以预期
		 * 桶扫描在该桶行数的 1/(match_count+1) 这个比例之后停止，
		 * 若匹配均匀分布。由于它们
		 * 大概并不十分均匀，对该比例施加
		 * 2.0 的模糊因子。（若用更大的模糊因子，就必须
		 * 把 inner_scan_frac 钳制到至多 1.0；但既然 match_count
		 * 至少为 1，现在不需要这样的钳制。）
		 */
		outer_matched_rows = rint(outer_path_rows * extra->semifactors.outer_match_frac);
		inner_scan_frac = 2.0 / (extra->semifactors.match_count + 1.0);

		startup_cost += hash_qual_cost.startup;
		run_cost += hash_qual_cost.per_tuple * outer_matched_rows *
			clamp_row_est(inner_path_rows * innerbucketsize * inner_scan_frac) * 0.5;

		/*
		 * 对未匹配的外侧行，情况相当不同。
		 * 首先，没有理由假定这些行
		 * 优先命中人口稠密的桶；改为假定它们
		 * 与内层分布不相关，因此它们看到的
		 * 平均桶大小是 inner_path_rows / virtualbuckets。
		 * 其次，它们很可能几乎没有精确的
		 * 哈希码匹配，因此桶中很少有元组
		 * 真正需要计算哈希限定条件。我们没有任何好
		 * 办法估计会有多少，但目前假定
		 * 每个桶项的有效代价是可匹配元组的
		 * 十分之一。
		 */
		run_cost += hash_qual_cost.per_tuple *
			(outer_path_rows - outer_matched_rows) *
			clamp_row_est(inner_path_rows / virtualbuckets) * 0.05;

		/* 取得将通过基本连接的元组数 */
		if (path->jpath.jointype == JOIN_ANTI)
			hashjointuples = outer_path_rows - outer_matched_rows;
		else
			hashjointuples = outer_matched_rows;
	}
	else
	{
		/*
		 * 所需的元组比较次数是外侧
		 * 元组数乘以哈希桶中的典型元组数，即
		 * 内层关系规模乘以其桶大小比例。每一次
		 * 都需要求值哈希连接限定条件。但实际上，
		 * 在每个元组上收取全部限定条件求值代价是悲观的，
		 * 因为除非哈希值精确
		 * 匹配，否则不求值限定条件。没有更好的办法，就把代价估计减半，以
		 * 计入这一点。
		 */
		startup_cost += hash_qual_cost.startup;
		run_cost += hash_qual_cost.per_tuple * outer_path_rows *
			clamp_row_est(inner_path_rows * innerbucketsize) * 0.5;

		/*
		 * 取得通过哈希限定条件的近似元组数。这里用
		 * approx_tuple_count，因为需要按
		 * JOIN_INNER 语义做的估计。
		 */
		hashjointuples = approx_tuple_count(root, &path->jpath, hashclauses);
	}

	/*
	 * 对每个通过哈希连接本身的元组，收取
	 * cpu_tuple_cost 加上求值要在连接处应用的额外限制
	 * 子句的代价。（这是悲观的，因为
	 * 并非所有限定条件都会在每个元组上求值。）
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * hashjointuples;

	/* 目标列表求值代价按每个输出行支付，而不是按每个被扫描的元组 */
	startup_cost += path->jpath.path.pathtarget->cost.startup;
	run_cost += path->jpath.path.pathtarget->cost.per_tuple * path->jpath.path.rows;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}


/*
 * cost_subplan
 *		算出 SubPlan（或 initplan）的代价。
 *
 * 注意：可以从根列表里挖出子计划的 Plan，但实践中
 * 所有调用方都已经手头有它，因此让它们传进来。
 */
void
cost_subplan(PlannerInfo *root, SubPlan *subplan, Plan *plan)
{
	QualCost	sp_cost;

	/*
	 * 算出求值 testexpr 的任何代价。
	 *
	 * 通常，SubPlan 节点建得很早，在为父查询层次构造任何
	 * RelOptInfos 之前，这意味着父 root
	 * 还没有足够的信息来安全地查阅统计信息。
	 * 因此这里把 root 作为 NULL 传入。cost_qual_eval() 已经
	 * 有能力处理 NULL root。
	 *
	 * 一个例外是为来自索引的 MIN/MAX
	 * 聚合的 initplan 建立的 SubPlan 节点（参见 SS_make_initplan_from_plan）。在这种
	 * 情况下 NULL root 是安全的，因为 testexpr 将为 NULL。
	 * 而且，initplan 按定义不会查阅父计划中的
	 * 任何东西。
	 */
	cost_qual_eval(&sp_cost,
				   make_ands_implicit((Expr *) subplan->testexpr),
				   NULL);

	if (subplan->useHashTable)
	{
		/*
		 * 若对子查询输出使用哈希表，那么
		 * 求值查询的代价是一次性代价。
		 * 还对每个元组收取一次
		 * cpu_operator_cost，用于装入哈希表的工作。
		 */
		sp_cost.startup += plan->total_cost +
			cpu_operator_cost * plan->plan_rows;

		/*
		 * 每元组代价包括求值左侧
		 * 表达式的代价，加上探测哈希表的代价。已经
		 * 把左侧表达式作为 testexpr 的一部分计入，并且
		 * 还会对每个比较
		 * 操作符计过一次 cpu_operator_cost。对探测代价来说这大概太低，但
		 * 很难做出更好的估计，所以目前就这样。
		 */
	}
	else
	{
		/*
		 * 否则，每次
		 * 求值都会重新扫描子计划输出。需要估计实际需要扫描多少
		 * 输出。注意：这段逻辑应当与
		 * plan/subselect.c 中 make_subplan() 使用的
		 * tuple_fraction 估计一致。
		 */
		Cost		plan_run_cost = plan->total_cost - plan->startup_cost;

		if (subplan->subLinkType == EXISTS_SUBLINK)
		{
			/* 只需取出 1 个元组；钳制以避免除零 */
			sp_cost.per_tuple += plan_run_cost / clamp_row_est(plan->plan_rows);
		}
		else if (subplan->subLinkType == ALL_SUBLINK ||
				 subplan->subLinkType == ANY_SUBLINK)
		{
			/* 假定需要 50% 的元组 */
			sp_cost.per_tuple += 0.50 * plan_run_cost;
			/* 对检查的每一行也收取一次 cpu_operator_cost */
			sp_cost.per_tuple += 0.50 * plan->plan_rows * cpu_operator_cost;
		}
		else
		{
			/* 假定需要全部元组 */
			sp_cost.per_tuple += plan_run_cost;
		}

		/*
		 * 也计入子计划的启动代价。若子计划
		 * 不相关或非直接相关，并且其最顶层节点是
		 * 物化其输出的那种，就假定只需支付
		 * 一次启动代价；
		 * 否则假定每次都支付启动代价。
		 */
		if (subplan->parParam == NIL &&
			ExecMaterializesOutput(nodeTag(plan)))
			sp_cost.startup += plan->startup_cost;
		else
			sp_cost.per_tuple += plan->startup_cost;
	}

	subplan->startup_cost = sp_cost.startup;
	subplan->per_call_cost = sp_cost.per_tuple;
}


/*
 * cost_rescan
 *		给定一条已完成的 Path，估计第一次做完之后重新扫描它的
 *		代价。对某些 Path 类型，重新扫描
 *		比原来的扫描便宜（若参数不变），本
 *		函数体现了关于这一点的知识。默认是返回
 *		Path 中存储的相同代价。（注意 Path 中实际存储的代价估计
 *		总是针对第一次扫描的。）
 *
 * 本函数目前不打算模拟诸如因磁盘块缓存而使重新扫描
 * 更便宜这类效应；我们关心的是
 * 执行器显式缓存结果、或不
 * 重做启动计算等计划类型。
 */
static void
cost_rescan(PlannerInfo *root, Path *path,
			Cost *rescan_startup_cost,	/* 输出参数 */
			Cost *rescan_total_cost)
{
	switch (path->pathtype)
	{
		case T_FunctionScan:

			/*
			 * 目前，nodeFunctionscan.c 总是把函数执行
			 * 完毕才返回任何行，并把结果缓存在
			 * tuplestore 中。因此函数求值代价全是启动代价，
			 * 重新扫描时不再支付。不过，全部运行代价
			 * 在重新扫描时都要再付一次。
			 */
			*rescan_startup_cost = 0;
			*rescan_total_cost = path->total_cost - path->startup_cost;
			break;
		case T_HashJoin:

			/*
			 * 若是单批连接，重新扫描时不必
			 * 重建哈希表。
			 */
			if (((HashPath *) path)->num_batches == 1)
			{
				/* 启动代价正好就是建立哈希表的代价 */
				*rescan_startup_cost = 0;
				*rescan_total_cost = path->total_cost - path->startup_cost;
			}
			else
			{
				/* 否则没有特殊处理 */
				*rescan_startup_cost = path->startup_cost;
				*rescan_total_cost = path->total_cost;
			}
			break;
		case T_CteScan:
		case T_WorkTableScan:
			{
				/*
				 * 这些计划类型把最终结果物化在
				 * tuplestore 或 tuplesort 对象里。因此重新扫描代价只是
				 * 每个元组 cpu_tuple_cost，除非结果大到
				 * 会溢出到磁盘。
				 */
				Cost		run_cost = cpu_tuple_cost * path->rows;
				double		nbytes = relation_byte_size(path->rows,
														path->pathtarget->width);
				long		work_mem_bytes = work_mem * 1024L;

				if (nbytes > work_mem_bytes)
				{
					/* 会溢出，因此计入重新读取的代价 */
					double		npages = ceil(nbytes / BLCKSZ);

					run_cost += seq_page_cost * npages;
				}
				*rescan_startup_cost = 0;
				*rescan_total_cost = run_cost;
			}
			break;
		case T_Material:
		case T_Sort:
			{
				/*
				 * 这些计划类型不仅把结果物化，而且不
				 * 做限定条件过滤或投影。因此它们重新扫描比上面那些
				 * 更便宜。每个元组只收取
				 * cpu_operator_cost。（注意：与 cost_sort 里的
				 * run_cost 收取保持同步，改之前也请看
				 * cost_material 里的注释。）
				 */
				Cost		run_cost = cpu_operator_cost * path->rows;
				double		nbytes = relation_byte_size(path->rows,
														path->pathtarget->width);
				long		work_mem_bytes = work_mem * 1024L;

				if (nbytes > work_mem_bytes)
				{
					/* 会溢出，因此计入重新读取的代价 */
					double		npages = ceil(nbytes / BLCKSZ);

					run_cost += seq_page_cost * npages;
				}
				*rescan_startup_cost = 0;
				*rescan_total_cost = run_cost;
			}
			break;
		case T_Memoize:
			/* 难做的部分都由 cost_memoize_rescan 完成 */
			cost_memoize_rescan(root, (MemoizePath *) path,
								rescan_startup_cost, rescan_total_cost);
			break;
		default:
			*rescan_startup_cost = path->startup_cost;
			*rescan_total_cost = path->total_cost;
			break;
	}
}


/*
 * cost_qual_eval
 *		估计求值 WHERE 子句的 CPU 代价。
 *		输入可以是布尔表达式的隐式 ANDed 列表，
 *		也可以是 RestrictInfo 节点列表。（后者更好，
 *		因为它允许缓存结果。）
 *		结果既包括一次性（启动）分量，
 *		也包括每次求值的分量。
 */
void
cost_qual_eval(QualCost *cost, List *quals, PlannerInfo *root)
{
	cost_qual_eval_context context;
	ListCell   *l;

	context.root = root;
	context.total.startup = 0;
	context.total.per_tuple = 0;

	/* 顶层隐式的 ANDing 不收取任何代价…… */

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		cost_qual_eval_walker(qual, &context);
	}

	*cost = context.total;
}

/*
 * cost_qual_eval_node
 *		同上，针对单个 RestrictInfo 或表达式。
 */
void
cost_qual_eval_node(QualCost *cost, Node *qual, PlannerInfo *root)
{
	cost_qual_eval_context context;

	context.root = root;
	context.total.startup = 0;
	context.total.per_tuple = 0;

	cost_qual_eval_walker(qual, &context);

	*cost = context.total;
}

static bool
cost_qual_eval_walker(Node *node, cost_qual_eval_context *context)
{
	if (node == NULL)
		return false;

	/*
	 * RestrictInfo 节点含有留给本例程使用的 eval_cost 字段，
	 * 这样就不必把限定子句的代价求值
	 * 一次以上。若该子句的代价还没算过，
	 * 该字段的启动值会是 -1。
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		if (rinfo->eval_cost.startup < 0)
		{
			cost_qual_eval_context locContext;

			locContext.root = context->root;
			locContext.total.startup = 0;
			locContext.total.per_tuple = 0;

			/*
			 * 对 OR 子句，递归进入标记过的树，以便
			 * 也给其中的 RestrictInfos 设置 eval_cost。
			 */
			if (rinfo->orclause)
				cost_qual_eval_walker((Node *) rinfo->orclause, &locContext);
			else
				cost_qual_eval_walker((Node *) rinfo->clause, &locContext);

			/*
			 * 若 RestrictInfo 被标成伪常量，它只会测试
			 * 一次，因此把它的代价全部当作启动代价。
			 */
			if (rinfo->pseudoconstant)
			{
				/* 启动期间计一次执行 */
				locContext.total.startup += locContext.total.per_tuple;
				locContext.total.per_tuple = 0;
			}
			rinfo->eval_cost = locContext.total;
		}
		context->total.startup += rinfo->eval_cost.startup;
		context->total.per_tuple += rinfo->eval_cost.per_tuple;
		/* 不要递归进入子节点 */
		return false;
	}

	/*
	 * 对给定树中的每个操作符或函数节点，收取
	 * pg_proc.procost 给出的估计执行代价（记得乘上
	 * cpu_operator_cost）。
	 *
	 * Var 和 Const 收取零，布尔操作符（AND、
	 * OR、NOT）也是。这很简陋，但比完全没有模型好得多。
	 *
	 * 要不要试着计入 AND/OR 短路
	 * 求值的可能？大概 *不要*，因为那会让
	 * 结果依赖于子句顺序，而我们根本没有立场
	 * 指望当前的子句顺序就是最终
	 * 会用的那个。上面按每个 RestrictInfo 做的缓存，
	 * 和试图重排子句本来也不搭。
	 *
	 * 这里完全忽略的另一个问题是：若集合返回
	 * 函数不在树的顶层，它上面的函数/操作符
	 * 就得求值多次。实际使用中这类
	 * 情形少到不值得为此增加复杂度；
	 * 而且我们对函数的行数估计往往相当
	 * 假，结果也会相当假。
	 */
	if (IsA(node, FuncExpr))
	{
		add_function_cost(context->root, ((FuncExpr *) node)->funcid, node,
						  &context->total);
	}
	else if (IsA(node, OpExpr) ||
			 IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		/* 靠结构等价把这些都同样对待 */
		set_opfuncid((OpExpr *) node);
		add_function_cost(context->root, ((OpExpr *) node)->opfuncid, node,
						  &context->total);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Node	   *arraynode = (Node *) lsecond(saop->args);
		QualCost	sacosts;
		QualCost	hcosts;
		int			estarraylen = estimate_array_length(arraynode);

		set_sa_opfuncid(saop);
		sacosts.startup = sacosts.per_tuple = 0;
		add_function_cost(context->root, saop->opfuncid, NULL,
						  &sacosts);

		if (OidIsValid(saop->hashfuncid))
		{
			/* 处理哈希版 ScalarArrayOpExpr 的代价 */
			hcosts.startup = hcosts.per_tuple = 0;

			add_function_cost(context->root, saop->hashfuncid, NULL, &hcosts);
			context->total.startup += sacosts.startup + hcosts.startup;

			/* 估计建立哈希表的代价。 */
			context->total.startup += estarraylen * hcosts.per_tuple;

			/*
			 * XXX 建表时要不要为 sacosts.per_tuple 收一点费，
			 * 还是可以假定哈希
			 * 冲突为零？
			 */

			/*
			 * 收取哈希表查找的费用。收取一次哈希和
			 * 一次比较。
			 */
			context->total.per_tuple += hcosts.per_tuple + sacosts.per_tuple;
		}
		else
		{
			/*
			 * 估计操作符大约会应用到一半
			 * 数组元素之后答案才确定。
			 */
			context->total.startup += sacosts.startup;
			context->total.per_tuple += sacosts.per_tuple *
				estimate_array_length(arraynode) * 0.5;
		}
	}
	else if (IsA(node, Aggref) ||
			 IsA(node, WindowFunc))
	{
		/*
		 * Aggref 和 WindowFunc 节点应当（也确实）像 Var 一样对待，
		 * 即在当前模型里执行代价为零，因为执行时它们
		 * 本质上就像 Var。出于同样原因，也不计
		 * 其输入表达式的代价。聚合/窗口函数及其参数的实际执行
		 * 代价必须
		 * 计入 Agg 或 WindowAgg
		 * 计划节点各自的代价模型。
		 */
		return false;			/* 不要递归进入子节点 */
	}
	else if (IsA(node, GroupingFunc))
	{
		/* 把它的代价当作 1 */
		context->total.per_tuple += cpu_operator_cost;
		return false;			/* 不要递归进入子节点 */
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) node;
		Oid			iofunc;
		Oid			typioparam;
		bool		typisvarlena;

		/* 检查结果类型的输入函数 */
		getTypeInputInfo(iocoerce->resulttype,
						 &iofunc, &typioparam);
		add_function_cost(context->root, iofunc, NULL,
						  &context->total);
		/* 检查输入类型的输出函数 */
		getTypeOutputInfo(exprType((Node *) iocoerce->arg),
						  &iofunc, &typisvarlena);
		add_function_cost(context->root, iofunc, NULL,
						  &context->total);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
		QualCost	perelemcost;

		cost_qual_eval_node(&perelemcost, (Node *) acoerce->elemexpr,
							context->root);
		context->total.startup += perelemcost.startup;
		if (perelemcost.per_tuple > 0)
			context->total.per_tuple += perelemcost.per_tuple *
				estimate_array_length((Node *) acoerce->arg);
	}
	else if (IsA(node, RowCompareExpr))
	{
		/* 保守地假定会检查所有列 */
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *lc;

		foreach(lc, rcexpr->opnos)
		{
			Oid			opid = lfirst_oid(lc);

			add_function_cost(context->root, get_opcode(opid), NULL,
							  &context->total);
		}
	}
	else if (IsA(node, MinMaxExpr) ||
			 IsA(node, SQLValueFunction) ||
			 IsA(node, XmlExpr) ||
			 IsA(node, CoerceToDomain) ||
			 IsA(node, NextValueExpr))
	{
		/* 把这些的代价都当作 1 */
		context->total.per_tuple += cpu_operator_cost;
	}
	else if (IsA(node, CurrentOfExpr))
	{
		/* 报高代价，以防选中 TID 扫描以外的任何东西 */
		context->total.startup += disable_cost;
	}
	else if (IsA(node, SubLink))
	{
		/* 本例程不应当用在尚未规划的表达式上 */
		elog(ERROR, "cannot handle unplanned sub-select");
	}
	else if (IsA(node, SubPlan))
	{
		/*
		 * 表达式里的子计划节点通常表示
		 * 每次求值都会执行该子计划，因此照此收费。
		 * （能作为 InitPlans 执行的子查询已经从
		 * 表达式里去掉了。）
		 */
		SubPlan    *subplan = (SubPlan *) node;

		context->total.startup += subplan->startup_cost;
		context->total.per_tuple += subplan->per_call_cost;

		/*
		 * 不想递归进入 testexpr，因为它已经
		 * 计在 SubPlan 节点的代价里。因此到此为止。
		 */
		return false;
	}
	else if (IsA(node, AlternativeSubPlan))
	{
		/*
		 * 任意用第一条备选计划来估价。（确实
		 * 只应包括一条备选，而且目前还没有
		 * 足够信息知道执行器最可能
		 * 用哪一条。）
		 */
		AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;

		return cost_qual_eval_walker((Node *) linitial(asplan->subplans),
									 context);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		/*
		 * 考虑一般表达式求值代价时，PlaceHolderVar 的代价应为零。
		 * 所含表达式的开销作为首次计算该 PHV 的扫描
		 * 或连接的目标列表求值代价的一部分来收取（见 set_rel_width 和
		 * add_placeholders_to_joinrel）。若在这里再收一次，就会对
		 * PHV 逐层冒泡上去的每一层计划
		 * 把代价重复计算。因此直接返回，
		 * 不要递归进入
		 * phexpr。
		 */
		return false;
	}

	/* 递归进入子节点 */
	return expression_tree_walker(node, cost_qual_eval_walker,
								  (void *) context);
}

/*
 * get_restriction_qual_cost
 *	  计算基关系限制限定条件的求值代价，加上已经
 *	  下推到该扫描的任何可移动连接限定条件。
 *	  结果返回到 *qpqual_cost。
 *
 * 这是个方便的子程序，适用于顺序扫描以及其他
 * 全部给定限定条件都要硬算的情形。它对
 * cost_index() 之类没用，那些由索引机制处理
 * 一部分限定条件。假定 baserestrictcost 先前已由
 * set_baserel_size_estimates() 设好。
 */
static void
get_restriction_qual_cost(PlannerInfo *root, RelOptInfo *baserel,
						  ParamPathInfo *param_info,
						  QualCost *qpqual_cost)
{
	if (param_info)
	{
		/* 包括下推子句的代价 */
		cost_qual_eval(qpqual_cost, param_info->ppi_clauses, root);

		qpqual_cost->startup += baserel->baserestrictcost.startup;
		qpqual_cost->per_tuple += baserel->baserestrictcost.per_tuple;
	}
	else
		*qpqual_cost = baserel->baserestrictcost;
}


/*
 * compute_semi_anti_join_factors
 *	  估计 SEMI、ANTI 或 inner_unique 连接预期会扫描
 *	  多少内层输入。
 *
 * 在哈希或嵌套循环 SEMI/ANTI 连接中，执行器一旦
 * 为当前外侧行找到匹配，就会停止扫描内层行。
 * 若已检测到内层关系是唯一的，也会如此。
 * 因此应当按这种效应调整部分代价分量。
 * 本函数计算这些调整所需的一些估计。
 * 无论外侧和内层关系用哪条具体路径，这些估计都相同，
 * 因此只计算一次，然后传给
 * 所有连接代价估计函数。
 *
 * 输入参数：
 *	joinrel：正在考虑的连接关系
 *	outerrel：正在考虑的外侧关系
 *	innerrel：正在考虑的内层关系
 *	jointype：若不是 JOIN_SEMI 或 JOIN_ANTI，就假定是 inner_unique
 *	sjinfo：与该连接相关的 SpecialJoinInfo
 *	restrictlist：连接限定条件
 * 输出参数：
 *	*semifactors 被填好（字段定义见 pathnodes.h）
 */
void
compute_semi_anti_join_factors(PlannerInfo *root,
							   RelOptInfo *joinrel,
							   RelOptInfo *outerrel,
							   RelOptInfo *innerrel,
							   JoinType jointype,
							   SpecialJoinInfo *sjinfo,
							   List *restrictlist,
							   SemiAntiJoinFactors *semifactors)
{
	Selectivity jselec;
	Selectivity nselec;
	Selectivity avgmatch;
	SpecialJoinInfo norm_sjinfo;
	List	   *joinquals;
	ListCell   *l;

	/*
	 * 在 ANTI 连接中必须忽略"下推"的子句，因为
	 * 那些不影响匹配逻辑。在 SEMI 连接中不
	 * 区分连接限定条件和"下推"限定条件，因此直接用整份
	 * restrictinfo 列表。对其他外连接类型，只应考虑
	 * 非下推限定条件，这样它就退化成 IS_OUTER_JOIN 检查。
	 */
	if (IS_OUTER_JOIN(jointype))
	{
		joinquals = NIL;
		foreach(l, restrictlist)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
				joinquals = lappend(joinquals, rinfo);
		}
	}
	else
		joinquals = restrictlist;

	/*
	 * 取得连接子句的 JOIN_SEMI 或 JOIN_ANTI 选择率。
	 */
	jselec = clauselist_selectivity(root,
									joinquals,
									0,
									(jointype == JOIN_ANTI) ? JOIN_ANTI : JOIN_SEMI,
									sjinfo);

	/*
	 * 也取得连接子句的普通内连接选择率。
	 */
	norm_sjinfo.type = T_SpecialJoinInfo;
	norm_sjinfo.min_lefthand = outerrel->relids;
	norm_sjinfo.min_righthand = innerrel->relids;
	norm_sjinfo.syn_lefthand = outerrel->relids;
	norm_sjinfo.syn_righthand = innerrel->relids;
	norm_sjinfo.jointype = JOIN_INNER;
	norm_sjinfo.ojrelid = 0;
	norm_sjinfo.commute_above_l = NULL;
	norm_sjinfo.commute_above_r = NULL;
	norm_sjinfo.commute_below_l = NULL;
	norm_sjinfo.commute_below_r = NULL;
	/* 不费力去让其余字段有效 */
	norm_sjinfo.lhs_strict = false;
	norm_sjinfo.semi_can_btree = false;
	norm_sjinfo.semi_can_hash = false;
	norm_sjinfo.semi_operators = NIL;
	norm_sjinfo.semi_rhs_exprs = NIL;

	nselec = clauselist_selectivity(root,
									joinquals,
									0,
									JOIN_INNER,
									&norm_sjinfo);

	/* 避免泄漏大量 ListCells */
	if (IS_OUTER_JOIN(jointype))
		list_free(joinquals);

	/*
	 * jselec 可以解释成外侧关系中有
	 * 任何匹配的行的比例（SEMI 和 ANTI 都成立）。nselec 则是
	 * 笛卡尔积中匹配的比例。因此，至少有一次匹配的每个外侧行，
	 * 平均匹配数是
	 * nselec * inner_rows / jselec。
	 *
	 * 注意：这里用内层关系的 "rows" 计数是对的，即使
	 * 以后可能考虑行数更少的参数化内层路径。
	 * 这是因为选择率估计里已经包括了
	 * 全部连接子句。
	 */
	if (jselec > 0)				/* 防止除以零 */
	{
		avgmatch = nselec * innerrel->rows / jselec;
		/* 钳制到合理范围 */
		avgmatch = Max(1.0, avgmatch);
	}
	else
		avgmatch = 1.0;

	semifactors->outer_match_frac = jselec;
	semifactors->match_count = avgmatch;
}

/*
 * has_indexed_join_quals
 *	  检查嵌套循环连接的全部连接限定条件是否都用作
 *	  内层索引限定条件。
 *
 * 若 SEMI/ANTI 连接的内层路径是索引扫描（包括位图
 * 索引扫描），并且把全部连接限定条件都用作索引限定条件，就可以假定
 * 未匹配的外侧元组处理起来便宜，否则大概
 * 很贵。
 */
static bool
has_indexed_join_quals(NestPath *path)
{
	JoinPath   *joinpath = &path->jpath;
	Relids		joinrelids = joinpath->path.parent->relids;
	Path	   *innerpath = joinpath->innerjoinpath;
	List	   *indexclauses;
	bool		found_one;
	ListCell   *lc;

	/* 若连接还有限定条件要算，就不快 */
	if (joinpath->joinrestrictinfo != NIL)
		return false;
	/* 内层路径完全没有参数化时也不是 */
	if (innerpath->param_info == NULL)
		return false;

	/* 找出内层扫描的 indexclauses 列表 */
	switch (innerpath->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			indexclauses = ((IndexPath *) innerpath)->indexclauses;
			break;
		case T_BitmapHeapScan:
			{
				/* 只接受简单位图扫描，不接受 AND/OR 情形 */
				Path	   *bmqual = ((BitmapHeapPath *) innerpath)->bitmapqual;

				if (IsA(bmqual, IndexPath))
					indexclauses = ((IndexPath *) bmqual)->indexclauses;
				else
					return false;
				break;
			}
		default:

			/*
			 * 若不是简单索引扫描，零行输出时大概也跑不快，
			 * 即使它是用上全部连接限定条件的
			 * 参数化路径。
			 */
			return false;
	}

	/*
	 * 检查内层路径的参数子句。任何来自外侧
	 * 路径的子句都必须出现在 indexclauses 列表里，要么完全相同，要么是
	 * equivclass.c 生成的等价形式。另外，至少必须找到
	 * 一条这样的子句，否则就是没有子句的连接，那不快。
	 */
	found_one = false;
	foreach(lc, innerpath->param_info->ppi_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										innerpath->parent->relids,
										joinrelids))
		{
			if (!is_redundant_with_indexclauses(rinfo, indexclauses))
				return false;
			found_one = true;
		}
	}
	return found_one;
}


/*
 * approx_tuple_count
 *		粗略估计通过一组限定条件的连接行数。
 *		限定条件可以是布尔表达式的隐式 ANDed 列表，
 *
 * 也可以是 RestrictInfo 节点列表（通常是后者）。
 * 故意按 JOIN_INNER 规则计算选择率，即使
 *
 * 它是某种外连接。这是合适的，因为我们要
 * 弄清有多少元组通过最初的归并或哈希
 * 连接步骤。
 * 之所以粗略，是因为绕过了 clauselist_selectivity，
 *
 * 只是把各独立子句的选择率乘在一起。其实
 * clauselist_selectivity 常常也没法比这更好，但
 * 有些情形（例如范围约束）它更聪明。不过，
 * 没法有效缓存 clauselist_selectivity 的结果，而
 * 单个子句的选择率可以缓存，也确实缓存了。
 * 既然结果只用来估计
 *
 * 会生成并送去 qpqual 检查的潜在输出元组有多少，
 * 将就这个近似
 * 似乎可以。
 */
static double
approx_tuple_count(PlannerInfo *root, JoinPath *path, List *quals)
{
	double		tuples;
	double		outer_tuples = path->outerjoinpath->rows;
	double		inner_tuples = path->innerjoinpath->rows;
	SpecialJoinInfo sjinfo;
	Selectivity selec = 1.0;
	ListCell   *l;

	/*
	 * 编造一个按 JOIN_INNER 语义的 SpecialJoinInfo。
	 */
	sjinfo.type = T_SpecialJoinInfo;
	sjinfo.min_lefthand = path->outerjoinpath->parent->relids;
	sjinfo.min_righthand = path->innerjoinpath->parent->relids;
	sjinfo.syn_lefthand = path->outerjoinpath->parent->relids;
	sjinfo.syn_righthand = path->innerjoinpath->parent->relids;
	sjinfo.jointype = JOIN_INNER;
	sjinfo.ojrelid = 0;
	sjinfo.commute_above_l = NULL;
	sjinfo.commute_above_r = NULL;
	sjinfo.commute_below_l = NULL;
	sjinfo.commute_below_r = NULL;
	/* 不费力去让其余字段有效 */
	sjinfo.lhs_strict = false;
	sjinfo.semi_can_btree = false;
	sjinfo.semi_can_hash = false;
	sjinfo.semi_operators = NIL;
	sjinfo.semi_rhs_exprs = NIL;

	/* 取得近似选择率 */
	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		/* 注意 clause_selectivity 能够缓存其结果 */
		selec *= clause_selectivity(root, qual, 0, JOIN_INNER, &sjinfo);
	}

	/* 把它用到输入关系的规模上 */
	tuples = selec * outer_tuples * inner_tuples;

	return clamp_row_est(tuples);
}


/*
 * set_baserel_size_estimates
 *		为给定基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经建好，
 * 并且 rel->tuples 必须已设置。
 *
 * 设置该关系节点的下列字段：
 *	rows：估计的输出元组数（应用
 *		  限制子句之后）。
 *	width：估计的平均输出元组宽度（字节）。
 *	baserestrictcost：求值 baserestrictinfo 子句的估计代价。
 */
void
set_baserel_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	double		nrows;

	/* 只应当用于基关系 */
	Assert(rel->relid > 0);

	nrows = rel->tuples *
		clauselist_selectivity(root,
							   rel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);

	rel->rows = clamp_row_est(nrows);

	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo, root);

	set_rel_width(root, rel);
}

/*
 * get_parameterized_baserel_size
 *		为基关系的参数化扫描做规模估计。
 *
 * 'param_clauses' 列出要使用的额外连接子句。
 *
 * 必须已经用过 set_baserel_size_estimates。
 */
double
get_parameterized_baserel_size(PlannerInfo *root, RelOptInfo *rel,
							   List *param_clauses)
{
	List	   *allclauses;
	double		nrows;

	/*
	 * 估计参数化扫描返回的行数，已知
	 * 它会应用全部额外连接子句以及该关系自己的
	 * 限制子句。注意强制这些子句在选择率估计期间
	 * 被当作非连接子句。
	 */
	allclauses = list_concat_copy(param_clauses, rel->baserestrictinfo);
	nrows = rel->tuples *
		clauselist_selectivity(root,
							   allclauses,
							   rel->relid,	/* 不要用 0！ */
							   JOIN_INNER,
							   NULL);
	nrows = clamp_row_est(nrows);
	/* 为安全起见，确保结果不超过基估计 */
	if (nrows > rel->rows)
		nrows = rel->rows;
	return nrows;
}

/*
 * set_joinrel_size_estimates
 *		为给定连接关系设置规模估计。
 *
 * 该关系的目标列表必须已经建好，并且必须提供
 * 与给定成员关系匹配的限制子句列表。
 * 多于两个基关系的连接关系有不止一种做法，
 *
 * 这里得到的结果可能取决于提供的是哪一对
 * 成员关系。理论上无论提供哪一对都应得到相同答案；
 * 实践中，由于选择率估计
 * 例程并非对所有情形都同样好，也许得不到。但也
 * 没什么办法。（要对遇到的每一对输入关系
 * 重复计算，再以某种方式
 * 平均结果，说得通吗？大概麻烦远大于价值，而且
 * 无论如何必须让该连接关系所有路径的行数估计
 * 保持相同。）
 * 这里只设置 rows 字段。
 *
 * reltarget 字段已由
 * build_joinrel_tlist 设置，baserestrictcost 不用于连接关系。
 */
void
set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   SpecialJoinInfo *sjinfo,
						   List *restrictlist)
{
	rel->rows = calc_joinrel_size_estimate(root,
										   rel,
										   outer_rel,
										   inner_rel,
										   outer_rel->rows,
										   inner_rel->rows,
										   sjinfo,
										   restrictlist);
}

/*
 * get_parameterized_joinrel_size
 *		为连接关系的参数化扫描做规模估计。
 *
 * 'rel' 是正在考虑的连接关系。
 * 'outer_path'、'inner_path' 是产生被连接关系的路径
 *		（多半也已参数化）。
 * 'sjinfo' 是与该连接相关的任何 SpecialJoinInfo。
 * 'restrict_clauses' 列出必须在
 * 连接节点应用的连接子句（包括下移到该连接的任何可移动子句，
 * 不包括下推到
 * 子路径里的任何可移动子句）。
 *
 * 必须已经用过 set_joinrel_size_estimates。
 */
double
get_parameterized_joinrel_size(PlannerInfo *root, RelOptInfo *rel,
							   Path *outer_path,
							   Path *inner_path,
							   SpecialJoinInfo *sjinfo,
							   List *restrict_clauses)
{
	double		nrows;

	/*
	 * 把参数化连接返回的行数估计成
	 * 输入路径规模乘以最终落在该连接节点的子句的选择率。
	 * 与 set_joinrel_size_estimates 一样，行数估计可能取决于
	 *
	 * 提供的那对输入路径，虽然理想情况下
	 * 参数化相同的任何一对
	 * 都应得到相同估计。
	 */
	nrows = calc_joinrel_size_estimate(root,
									   rel,
									   outer_path->parent,
									   inner_path->parent,
									   outer_path->rows,
									   inner_path->rows,
									   sjinfo,
									   restrict_clauses);
	/* 为安全起见，确保结果不超过基估计 */
	if (nrows > rel->rows)
		nrows = rel->rows;
	return nrows;
}

/*
 * calc_joinrel_size_estimate
 *		set_joinrel_size_estimates 和
 *		get_parameterized_joinrel_size 的主力。
 *
 * outer_rel/inner_rel 是被连接的关系，但应假定
 * 它们的规模是 outer_rows/inner_rows；考虑参数化路径时，这些数字可能小于
 * rel->rows 所说的值。
 */
static double
calc_joinrel_size_estimate(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   double outer_rows,
						   double inner_rows,
						   SpecialJoinInfo *sjinfo,
						   List *restrictlist)
{
	JoinType	jointype = sjinfo->jointype;
	Selectivity fkselec;
	Selectivity jselec;
	Selectivity pselec;
	double		nrows;

	/*
	 * 计算连接子句选择率。注意这里只考虑
	 * 在该连接层次变成限制子句的子句；不会
	 * 重复计算它们，因为估计成员关系规模时
	 * 没有考虑过它们。
	 *
	 * 首先看是否有连接子句能匹配已知的外键
	 * 约束。若有，就把那些子句从 restrictlist 去掉，
	 * 改为按外键语义估计它们的选择率。（这样做
	 * 不管所说子句是局部的还是"下推"的。
	 * 外键匹配的子句大概永远不会在
	 * 外连接处被看成下推的，因为它是严格的，因而会成为
	 * 连接强度归约的依据。）fkselec 取得
	 * 外键匹配子句的净选择率，若没有则为 1.0。
	 */
	fkselec = get_foreign_key_join_selectivity(root,
											   outer_rel->relids,
											   inner_rel->relids,
											   sjinfo,
											   &restrictlist);

	/*
	 * 对外连接，必须区分连接自己的子句（JOIN/ON 条件）
	 * 与任何"下推"子句的选择率。对内连接则把它们
	 * 全部算作连接子句。
	 */
	if (IS_OUTER_JOIN(jointype))
	{
		List	   *joinquals = NIL;
		List	   *pushedquals = NIL;
		ListCell   *l;

		/* 翻找子句，分成两份列表 */
		foreach(l, restrictlist)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
				pushedquals = lappend(pushedquals, rinfo);
			else
				joinquals = lappend(joinquals, rinfo);
		}

		/* 分别取得选择率 */
		jselec = clauselist_selectivity(root,
										joinquals,
										0,
										jointype,
										sjinfo);
		pselec = clauselist_selectivity(root,
										pushedquals,
										0,
										jointype,
										sjinfo);

		/* 避免泄漏大量 ListCells */
		list_free(joinquals);
		list_free(pushedquals);
	}
	else
	{
		jselec = clauselist_selectivity(root,
										restrictlist,
										0,
										jointype,
										sjinfo);
		pselec = 0.0;			/* 未使用，消除编译器告警 */
	}

	/*
	 * 基本上，用选择率去乘笛卡尔积的规模。
	 *
	 * 若在做外连接，就要计入这一点：连接限定条件的
	 * 选择率必须用"输出至少要和不可空输入一样大"
	 * 这一知识来钳制。不过，任何
	 * 下推限定条件是在外连接之后应用的，因此它们的
	 * 选择率完全生效。
	 *
	 * 对 JOIN_SEMI 和 JOIN_ANTI，选择率定义为
	 * 有匹配的左侧行的比例，我们直接套用。
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			/* 未使用 pselec */
			break;
		case JOIN_LEFT:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			if (nrows < outer_rows)
				nrows = outer_rows;
			nrows *= pselec;
			break;
		case JOIN_FULL:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			if (nrows < outer_rows)
				nrows = outer_rows;
			if (nrows < inner_rows)
				nrows = inner_rows;
			nrows *= pselec;
			break;
		case JOIN_SEMI:
			nrows = outer_rows * fkselec * jselec;
			/* 未使用 pselec */
			break;
		case JOIN_ANTI:
			nrows = outer_rows * (1.0 - fkselec * jselec);
			nrows *= pselec;
			break;
		default:
			/* 这里不预期出现其他值 */
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			nrows = 0;			/* 消除编译器告警 */
			break;
	}

	return clamp_row_est(nrows);
}

/*
 * get_foreign_key_join_selectivity
 *		估计与外键相关的子句的连接选择率。
 *
 * 从 *restrictlist 去掉任何能匹配外键约束的子句，
 * 并返回它们选择率的替代估计。没有这种子句时
 * 返回 1.0。
 *
 * 之所以特殊对待这些子句，是因为这样能得到比依赖
 * clauselist_selectivity() 更好的估计，尤其是
 * 多列外键，那个函数关于子句
 * 相互独立的假定会严重失效。即使是单列外键，当
 * pg_statistic 统计信息缺少或过时时，也可能
 * 得到更好的答案。
 */
static Selectivity
get_foreign_key_join_selectivity(PlannerInfo *root,
								 Relids outer_relids,
								 Relids inner_relids,
								 SpecialJoinInfo *sjinfo,
								 List **restrictlist)
{
	Selectivity fkselec = 1.0;
	JoinType	jointype = sjinfo->jointype;
	List	   *worklist = *restrictlist;
	ListCell   *lc;

	/* 考虑已知与查询匹配的每条外键约束 */
	foreach(lc, root->fkey_list)
	{
		ForeignKeyOptInfo *fkinfo = (ForeignKeyOptInfo *) lfirst(lc);
		bool		ref_is_outer;
		List	   *removedlist;
		ListCell   *cell;

		/*
		 * 除非这条外键把该连接一侧的基关系和
		 * 另一侧的基关系连起来，否则它不相关。
		 */
		if (bms_is_member(fkinfo->con_relid, outer_relids) &&
			bms_is_member(fkinfo->ref_relid, inner_relids))
			ref_is_outer = false;
		else if (bms_is_member(fkinfo->ref_relid, outer_relids) &&
				 bms_is_member(fkinfo->con_relid, inner_relids))
			ref_is_outer = true;
		else
			continue;

		/*
		 * 若处理的是半/反连接，并且外键的被引用
		 * 关系在外侧，那么知道这条外键也帮不上
		 * 我们弄清需要知道的事（即外侧
		 * 行中有匹配的比例）。另一方面，若被引用关系
		 * 在内侧，那么所有外侧行都必须在
		 * 被引用表中有匹配（忽略空值）。但任何过滤该表的限制或连接
		 * 子句都会降低匹配比例。
		 * 可以计入限制子句，但很难猜测
		 * 有多少表行能通过位于右侧内部的连接。
		 * 因此，若两种情形之一适用，就放弃并忽略这条外键。
		 */
		if ((jointype == JOIN_SEMI || jointype == JOIN_ANTI) &&
			(ref_is_outer || bms_membership(inner_relids) != BMS_SINGLETON))
			continue;

		/*
		 * 修改 restrictlist，去掉与外键匹配的子句（并
		 * 把它们放进 removedlist）。修改
		 * 原先传入的 List 结构似乎不安全，因此
		 * 第一次经过时做一份浅拷贝。
		 */
		if (worklist == *restrictlist)
			worklist = list_copy(worklist);

		removedlist = NIL;
		foreach(cell, worklist)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);
			bool		remove_it = false;
			int			i;

			/* 若该子句匹配外键的任何列，就丢掉它 */
			for (i = 0; i < fkinfo->nkeys; i++)
			{
				if (rinfo->parent_ec)
				{
					/*
					 * 由等价类导出的子句只能按等价类匹配。把
					 * 同一等价类导出的任何子句都视为
					 * 与外键匹配是可以的：即使 equivclass.c 选择生成
					 * 的是另一对 Var 的等值子句，它本来
					 * 也可以生成外键那些 Var 的等值子句。因此就
					 * 估计而言，可以当作它已经那样做了。
					 *
					 * 注意：检查 parent_ec 有点取巧，因为
					 * 有些由等价类导出的子句没有设置 parent_ec；
					 * 但那些子句比较的表达式
					 * 不只是 Var，因此反正无法匹配外键。
					 */
					if (fkinfo->eclass[i] == rinfo->parent_ec)
					{
						remove_it = true;
						break;
					}
				}
				else
				{
					/*
					 * 否则，看 rinfo 先前是否已作为
					 * "宽松"子句匹配到外键。
					 */
					if (list_member_ptr(fkinfo->rinfos[i], rinfo))
					{
						remove_it = true;
						break;
					}
				}
			}
			if (remove_it)
			{
				worklist = foreach_delete_current(worklist, cell);
				removedlist = lappend(removedlist, rinfo);
			}
		}

		/*
		 * 若没能去掉预期会找到的全部匹配子句，
		 * 就退缩并忽略这条外键；套用它的选择率
		 * 可能导致重复计算。把已经设法
		 * 去掉的子句放回工作列表。
		 *
		 * 匹配子句已知不是外连接延迟的，它们
		 * 通常会出现在最初的连接子句列表里。若
		 * 没找到它们，有两种可能：
		 *
		 * 1. 若外键匹配基于 ec_has_const 的等价类，它根本
		 * 不会生成任何连接子句。检查是否已有"全部"子句时，
		 * 不计这类等价类。（下面会针对这种情形
		 * 调整选择率估计。）
		 *
		 * 2. 子句在本循环先前某次迭代中匹配了另一条外键，
		 * 因而已从工作列表去掉。（常见
		 * 情形是两条外键匹配同一个等价类；最初列表里
		 * 只有一条由等价类导出的子句，于是第一条外键
		 * 会把它用掉。）独立套用两条外键的选择率有
		 * 低估连接规模的风险；尤其会抵消
		 * 等价类被发明出来要做的一件主要事情，即避免
		 * 把冗余等值条件的选择率重复计算。
		 * 以后也许会想到把这些估计合理地合并起来，
		 * 但目前只能放弃，因为这种情形相当少见。
		 */
		if (removedlist == NIL ||
			list_length(removedlist) !=
			(fkinfo->nmatched_ec - fkinfo->nconst_ec + fkinfo->nmatched_ri))
		{
			worklist = list_concat(worklist, removedlist);
			continue;
		}

		/*
		 * 最后到了回报：利用这一点估计选择率：
		 * 每个引用行恰好匹配被引用表中的
		 * 一行。
		 *
		 * XXX 若引用列里有空值，这就不成立，
		 * 原则上应当为此降低估计。
		 * 不过 (1) 若查询别处对引用列有任何严格限制子句，
		 * 在这里再降就会
		 * 把空值比例重复计算，而且 (2) 不太清楚
		 * 多个引用列的空值比例如何合并。因此
		 * 目前不对空值做修正。
		 *
		 * XXX 另一点是，若外键约束的任一侧
		 * 是继承父表，估计时就当作约束
		 * 也覆盖其全部子表。对引用表来说这不是
		 * 不合理的假定，即用户大概施加了
		 * 相同的约束到所有子表（不过也许应当
		 * 检查这一点）。但对被引用表不可能那样做。
		 * 幸好，正因为那样做不成，
		 * 实践中外键引用父表并不常见。
		 * 因此至少目前，这里不考虑继承。
		 */
		if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
		{
			/*
			 * 对 JOIN_SEMI 和 JOIN_ANTI，只有外键的
			 * 被引用表正好是连接内侧时才会到这里。连接
			 * 选择率定义为有匹配的左侧行的比例。
			 * 外键意味着每一左侧行在
			 * *被引用表中* 都有匹配；但该表上的任何限制子句都会
			 * 减少匹配数。因此把连接
			 * 选择率取成该表限制子句的选择率，即 rows / tuples；
			 * 但必须提防
			 * tuples == 0。
			 */
			RelOptInfo *ref_rel = find_base_rel(root, fkinfo->ref_relid);
			double		ref_tuples = Max(ref_rel->tuples, 1.0);

			fkselec *= ref_rel->rows / ref_tuples;
		}
		else
		{
			/*
			 * 否则选择率正好是 1/被引用表规模；但
			 * 要提防 tuples == 0。注意应当用原始表
			 * 元组计数，而不是它过滤或连接后规模的任何估计。
			 */
			RelOptInfo *ref_rel = find_base_rel(root, fkinfo->ref_relid);
			double		ref_tuples = Max(ref_rel->tuples, 1.0);

			fkselec *= 1.0 / ref_tuples;
		}

		/*
		 * 若任何外键列参与了 ec_has_const 等价类，那么
		 * equivclass.c 会为
		 * 连接的每一侧生成 "var = const" 限制，
		 * 从而缩小两个输入
		 * 关系的规模。按字面取 fkselec 就等于
		 * 把引用 Var 上常量限制的选择率重复计算。
		 * 因此找出施加到引用 Var 上的限制子句，
		 * 并除掉它们的选择率来修正这一点。
		 */
		if (fkinfo->nconst_ec > 0)
		{
			for (int i = 0; i < fkinfo->nkeys; i++)
			{
				EquivalenceClass *ec = fkinfo->eclass[i];

				if (ec && ec->ec_has_const)
				{
					EquivalenceMember *em = fkinfo->fk_eclass_member[i];
					RestrictInfo *rinfo = find_derived_clause_for_ec_member(ec,
																			em);

					if (rinfo)
					{
						Selectivity s0;

						s0 = clause_selectivity(root,
												(Node *) rinfo,
												0,
												jointype,
												sjinfo);
						if (s0 > 0)
							fkselec /= s0;
					}
				}
			}
		}
	}

	*restrictlist = worklist;
	CLAMP_PROBABILITY(fkselec);
	return fkselec;
}

/*
 * set_subquery_size_estimates
 *		为一个子查询基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经建好，
 * 并且子查询的路径必须已经完成。
 * 查看子查询的 PlannerInfo 来提取数据。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_subquery_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	PlannerInfo *subroot = rel->subroot;
	RelOptInfo *sub_final_rel;
	ListCell   *lc;

	/* 只应当用于作为子查询的基关系 */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_SUBQUERY);

	/*
	 * 从子查询复制原始输出行数。它的所有路径
	 * 输出行数都应相同，因此只看总代价最低的那条。
	 */
	sub_final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
	rel->tuples = sub_final_rel->cheapest_total_path->rows;

	/*
	 * 通过检查子查询的目标列表，计算每个输出列的宽度估计。
	 * 对任何是普通 Var 的输出，取得
	 * 规划子查询时做出的宽度估计。否则留给
	 * set_rel_width 填入基于数据类型的默认估计。
	 */
	foreach(lc, subroot->parse->targetList)
	{
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		Node	   *texpr = (Node *) te->expr;
		int32		item_width = 0;

		/* 废列对上层查询不可见 */
		if (te->resjunk)
			continue;

		/*
		 * 子查询可能是某个视图的展开，自当前查询被解析以来又给它
		 * 加过列，因此里面有
		 * 非废的目标列表列并不对应我们这一查询层次
		 * 可见的任何列。忽略这些列。
		 */
		if (te->resno < rel->min_attr || te->resno > rel->max_attr)
			continue;

		/*
		 * XXX 这对含有集合
		 * 运算的子查询目前不行，因为它们目标列表里的 Var 是指向
		 * 第一个叶子子查询的假引用，即使仍能拿到它的 PlannerInfo，
		 * 也给不出正确答案。
		 *
		 * 另外，子查询可能是追加关系，其全部分支都因
		 * 约束排除而已知为空，这时
		 * set_append_rel_pathlist 会把 attr_widths 留成零。
		 *
		 * 无论哪种情形，都先把宽度估计留成零，直到
		 * set_rel_width 把它补上。
		 */
		if (IsA(texpr, Var) &&
			subroot->parse->setOperations == NULL)
		{
			Var		   *var = (Var *) texpr;
			RelOptInfo *subrel = find_base_rel(subroot, var->varno);

			item_width = subrel->attr_widths[var->varattno - subrel->min_attr];
		}
		rel->attr_widths[te->resno - rel->min_attr] = item_width;
	}

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_function_size_estimates
 *		为一个函数调用的基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_function_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;
	ListCell   *lc;

	/* 只应当用于作为函数的基关系 */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * 估计这些函数将返回的行数。该节点的行数是
	 * 最大那个函数结果的行数。
	 */
	rel->tuples = 0;
	foreach(lc, rte->functions)
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
		double		ntup = expression_returns_set_rows(root, rtfunc->funcexpr);

		if (ntup > rel->tuples)
			rel->tuples = ntup;
	}

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_function_size_estimates
 *		为一个函数调用的基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 *
 * 设置的字段与 set_tablefunc_size_estimates 相同。
 */
void
set_tablefunc_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* 只应当用于作为函数的基关系 */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_TABLEFUNC);

	rel->tuples = 100;

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_values_size_estimates
 *		为一个值列表基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_values_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* 只应当用于作为值列表的基关系 */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * 估计值列表将返回的行数。根据
	 * 列表长度可以精确知道（当然，列表项里的集合返回
	 * 函数除外，但别的地方也没有为这种细化
	 * 做准备）。
	 */
	rel->tuples = list_length(rte->values_lists);

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_cte_size_estimates
 *		为一个 CTE 引用的基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经建好，
 * 并且需要 CTE 返回行数的估计
 * （普通 CTE），或非递归项的估计（自引用时）。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_cte_size_estimates(PlannerInfo *root, RelOptInfo *rel, double cte_rows)
{
	RangeTblEntry *rte;

	/* 只应当用于作为 CTE 引用的基关系 */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_CTE);

	if (rte->self_reference)
	{
		/*
		 * 自引用时，假定工作表平均规模是
		 * 非递归项规模的某个倍数。最佳倍数
		 * 随查询"扇出"而变，因此让这个值可调。
		 */
		rel->tuples = clamp_row_est(recursive_worktable_factor * cte_rows);
	}
	else
	{
		/* 否则就相信 CTE 的行数估计 */
		rel->tuples = cte_rows;
	}

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_namedtuplestore_size_estimates
 *		为一个 tuplestore 引用的基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_namedtuplestore_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* 只应当用于作为 tuplestore 引用的基关系 */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);

	/*
	 * 使用生成该命名
	 * tuplestore 的代码所提供的估计。有些情况下实际数量可得；
	 * 另一些情况下同一计划会被复用，因此也许会估计并使用
	 * 一个"典型"值。
	 */
	rel->tuples = rte->enrtuples;
	if (rel->tuples < 0)
		rel->tuples = 1000;

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_result_size_estimates
 *		为 RTE_RESULT 基关系设置规模估计。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 *
 * 设置的字段与 set_baserel_size_estimates 相同。
 */
void
set_result_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* 只应当用于 RTE_RESULT 基关系 */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_RESULT);

	/* RTE_RESULT 原生总是生成单行 */
	rel->tuples = 1;

	/* 现在估计输出行数等 */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_foreign_size_estimates
 *		为一张外部表基关系设置规模估计。
 *
 * 这里能做的不多；外部数据包装器
 * 负责产生有用的估计。baserestrictcost 可以估计得像样，
 * 因此把它设上，也用目标列表里纯粹由数据类型驱动的估计
 * 把宽度设上。
 * 对 rows 值没法做任何合理的事，因此只放
 * 一个默认估计，希望包装器能改进它。包装器的
 * GetForeignRelSize 函数马上会被调用。
 *
 * 该关系的目标列表和 restrictinfo 列表必须已经
 * 建好。
 */
void
set_foreign_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* 只应当用于基关系 */
	Assert(rel->relid > 0);

	rel->rows = 1000;			/* 完全瞎编的默认估计 */

	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo, root);

	set_rel_width(root, rel);
}


/*
 * set_rel_width
 *		设置基关系的估计输出宽度。
 *
 * 估计输出宽度是每属性宽度估计之和，
 * 针对实际引用的列，再加上任何 PHVs 或其他表达式，
 * 这些必须在该关系上计算。这是要
 * 在排序、哈希等情况下向上传递的数据量。
 *
 * 本函数也设置 reltarget->cost，所以现在名字有点不准。
 *
 * 注意：它对普通关系效果最好，因为它优先看
 * 真正的 Var。对子查询，set_subquery_size_estimates 已经
 * 把子查询内部做出的每列估计复制上来，
 * 对其他类型的关系反正也做不了多少。
 * 若无法得到更好的数字，
 * 就退回到（相当笨的）基于数据类型的宽度估计。
 *
 * 每属性宽度估计会被缓存，以便在
 * 建立连接关系或扫描/连接之后的 pathtarget 时复用。
 */
static void
set_rel_width(PlannerInfo *root, RelOptInfo *rel)
{
	Oid			reloid = planner_rt_fetch(rel->relid, root)->relid;
	int32		tuple_width = 0;
	bool		have_wholerow_var = false;
	ListCell   *lc;

	/* 假定 Var 的代价为零，但其他表达式不是 */
	rel->reltarget->cost.startup = 0;
	rel->reltarget->cost.per_tuple = 0;

	foreach(lc, rel->reltarget->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		/*
		 * Ordinarily, a Var in a rel's targetlist must belong to that rel;
		 * but there are corner cases involving LATERAL references where that
		 * isn't so.  If the Var has the wrong varno, fall through to the
		 * generic case (it doesn't seem worth the trouble to be any smarter).
		 */
		if (IsA(node, Var) &&
			((Var *) node)->varno == rel->relid)
		{
			Var		   *var = (Var *) node;
			int			ndx;
			int32		item_width;

			Assert(var->varattno >= rel->min_attr);
			Assert(var->varattno <= rel->max_attr);

			ndx = var->varattno - rel->min_attr;

			/*
			 * 若是整行 Var，等已经
			 * 尽量缓存属性宽度之后再在下面处理。
			 */
			if (var->varattno == 0)
			{
				have_wholerow_var = true;
				continue;
			}

			/*
			 * 宽度也许已经缓存过（尤其当它是
			 * 子查询时），因此不要重复劳动。
			 */
			if (rel->attr_widths[ndx] > 0)
			{
				tuple_width += rel->attr_widths[ndx];
				continue;
			}

			/* 试着从统计信息取得列宽 */
			if (reloid != InvalidOid && var->varattno > 0)
			{
				item_width = get_attavgwidth(reloid, var->varattno);
				if (item_width > 0)
				{
					rel->attr_widths[ndx] = item_width;
					tuple_width += item_width;
					continue;
				}
			}

			/*
			 * 不是普通关系，或找不到它的统计信息。只根据
			 * 类型信息来估计。
			 */
			item_width = get_typavgwidth(var->vartype, var->vartypmod);
			Assert(item_width > 0);
			rel->attr_widths[ndx] = item_width;
			tuple_width += item_width;
		}
		else if (IsA(node, PlaceHolderVar))
		{
			/*
			 * 扫描该关系时需要求值 PHV 所含的表达式，
			 * 因此务必把它计入 reltarget->cost。
			 */
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);
			QualCost	cost;

			tuple_width += phinfo->ph_width;
			cost_qual_eval_node(&cost, (Node *) phv->phexpr, root);
			rel->reltarget->cost.startup += cost.startup;
			rel->reltarget->cost.per_tuple += cost.per_tuple;
		}
		else
		{
			/*
			 * 看到的可能是从子查询上拉的表达式，
			 * 或表示整行子 Var 的 ROW()，等等。能做的
			 * 就用表达式的类型信息来做。
			 */
			int32		item_width;
			QualCost	cost;

			item_width = get_typavgwidth(exprType(node), exprTypmod(node));
			Assert(item_width > 0);
			tuple_width += item_width;
			/* 是否必须计入代价并不完全清楚，但还是计入 */
			cost_qual_eval_node(&cost, node, root);
			rel->reltarget->cost.startup += cost.startup;
			rel->reltarget->cost.per_tuple += cost.per_tuple;
		}
	}

	/*
	 * 若有整行引用，把它的宽度估计成
	 * 每列宽度之和加上堆元组头开销。
	 */
	if (have_wholerow_var)
	{
		int32		wholerow_width = MAXALIGN(SizeofHeapTupleHeader);

		if (reloid != InvalidOid)
		{
			/* 真实关系，因此估计真实元组宽度 */
			wholerow_width += get_relation_data_width(reloid,
													  rel->attr_widths - rel->min_attr);
		}
		else
		{
			/* 对假关系，能用什么信息就用什么 */
			AttrNumber	i;

			for (i = 1; i <= rel->max_attr; i++)
				wholerow_width += rel->attr_widths[i - rel->min_attr];
		}

		rel->attr_widths[0 - rel->min_attr] = wholerow_width;

		/*
		 * 把整行 Var 包括进输出元组。是的，运行时
		 * 确实如此。
		 */
		tuple_width += wholerow_width;
	}

	Assert(tuple_width >= 0);
	rel->reltarget->width = tuple_width;
}

/*
 * set_pathtarget_cost_width
 *		设置 PathTarget 目标列表的估计求值代价和输出宽度。
 *
 * 作为记法上的方便，返回传入的同一个 PathTarget 指针。
 *
 * 本函数的多数（虽非全部）使用发生在已经对基关系跑过
 * set_rel_width() 之后；因此通常能取得 Var 的缓存宽度
 * 估计。若不能，就退回到基于数据类型的宽度
 * 估计。早期规划阶段对 PathTargets 的使用并不那么
 * 急需准确宽度，不值得为此去系统目录找更好的数据。
 */
PathTarget *
set_pathtarget_cost_width(PlannerInfo *root, PathTarget *target)
{
	int32		tuple_width = 0;
	ListCell   *lc;

	/* 假定 Var 的代价为零，但其他表达式不是 */
	target->cost.startup = 0;
	target->cost.per_tuple = 0;

	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		tuple_width += get_expr_width(root, node);

		/* 对非 Var，计入求值代价 */
		if (!IsA(node, Var))
		{
			QualCost	cost;

			cost_qual_eval_node(&cost, node, root);
			target->cost.startup += cost.startup;
			target->cost.per_tuple += cost.per_tuple;
		}
	}

	Assert(tuple_width >= 0);
	target->width = tuple_width;

	return target;
}

/*
 * get_expr_width
 *		估计给定表达式的宽度，尽量使用
 *		Var 所属 RelOptInfo 里缓存的宽度，否则在做不到或给定节点不是 Var 时
 *		退回到该类型的平均宽度。
 */
static int32
get_expr_width(PlannerInfo *root, const Node *expr)
{
	int32		width;

	if (IsA(expr, Var))
	{
		const Var  *var = (const Var *) expr;

		/* 这里不应当看到任何上层 Var */
		Assert(var->varlevelsup == 0);

		/* 试着从 RelOptInfo 缓存取得数据 */
		if (!IS_SPECIAL_VARNO(var->varno) &&
			var->varno < root->simple_rel_array_size)
		{
			RelOptInfo *rel = root->simple_rel_array[var->varno];

			if (rel != NULL &&
				var->varattno >= rel->min_attr &&
				var->varattno <= rel->max_attr)
			{
				int			ndx = var->varattno - rel->min_attr;

				if (rel->attr_widths[ndx] > 0)
					return rel->attr_widths[ndx];
			}
		}

		/*
		 * 没有缓存数据，因此只根据类型信息估计。
		 */
		width = get_typavgwidth(var->vartype, var->vartypmod);
		Assert(width > 0);

		return width;
	}

	width = get_typavgwidth(exprType(expr), exprTypmod(expr));
	Assert(width > 0);
	return width;
}

/*
 * relation_byte_size
 *	  估计给定宽度（字节）的给定数量元组
 *	  所占的存储空间（字节）。
 */
static double
relation_byte_size(double tuples, int width)
{
	return tuples * (MAXALIGN(width) + MAXALIGN(SizeofHeapTupleHeader));
}

/*
 * page_size
 *	  返回给定宽度（字节）的给定数量元组
 *	  所覆盖页数的估计。
 */
static double
page_size(double tuples, int width)
{
	return ceil(relation_byte_size(tuples, width) / BLCKSZ);
}

/*
 * 给定为该路径预算的工作进程数，估计每个工作进程
 * 将承担的工作比例。
 */
static double
get_parallel_divisor(Path *path)
{
	double		parallel_divisor = path->parallel_workers;

	/*
	 * 并行查询的早期经验表明，只有
	 * 一个工作进程时，领导进程常常对
	 * 执行计划的并行部分贡献很大，但随着工作进程
	 * 增加，它做得越来越少，因为它忙着从
	 * 工作进程读取元组，并做所需的非并行后处理。到
	 * 4 个工作进程时，领导进程不再有实质性
	 * 贡献。因此目前估计领导进程把 30% 的
	 * 时间花在服务每个工作进程上，其余时间执行
	 * 并行计划。
	 */
	if (parallel_leader_participation)
	{
		double		leader_contribution;

		leader_contribution = 1.0 - (0.3 * path->parallel_workers);
		if (leader_contribution > 0)
			parallel_divisor += leader_contribution;
	}

	return parallel_divisor;
}

/*
 * compute_bitmap_pages
 *
 * 计算位图堆扫描中从堆取出的页数。
 */
double
compute_bitmap_pages(PlannerInfo *root, RelOptInfo *baserel, Path *bitmapqual,
					 int loop_count, Cost *cost, double *tuple)
{
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		T;
	double		pages_fetched;
	double		tuples_fetched;
	double		heap_pages;
	long		maxentries;

	/*
	 * 取得获得该位图的总代价，以及它的总
	 * 选择率。
	 */
	cost_bitmap_tree_node(bitmapqual, &indexTotalCost, &indexSelectivity);

	/*
	 * 估计从主表取出的页数。
	 */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;

	/*
	 * 对单次扫描，需要从堆取出的页数
	 * 与 Mackert 和 Lohman 公式在 T <= b 时的情形相同（即不
	 * 需要重新读取）。
	 */
	pages_fetched = (2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);

	/*
	 * 计算从堆取出的页数。然后根据
	 * 当前 work_mem 估计位图中的估计 maxentries。
	 * （注意这个计算总是基于单次迭代会取出的页数，
	 * 即使 loop_count > 1 也如此。
	 * 这是对的，因为同一时刻位图里只会存放
	 * 那么多项。）
	 */
	heap_pages = Min(pages_fetched, baserel->pages);
	maxentries = tbm_calculate_entries(work_mem * 1024L);

	if (loop_count > 1)
	{
		/*
		 * 对重复的位图扫描，把 Mackert 和 Lohman 公式中取出的元组数
		 * 按扫描次数放大，以便
		 * 估计全部扫描取出的页数。然后
		 * 按一次扫描分摊。
		 */
		pages_fetched = index_pages_fetched(tuples_fetched * loop_count,
											baserel->pages,
											get_indexpath_pages(bitmapqual),
											root);
		pages_fetched /= loop_count;
	}

	if (pages_fetched >= T)
		pages_fetched = T;
	else
		pages_fetched = ceil(pages_fetched);

	if (maxentries < heap_pages)
	{
		double		exact_pages;
		double		lossy_pages;

		/*
		 * 有损页数的粗略近似。由于
		 * tbm_lossify() 的写法，一旦内存不够，有损页数
		 * 会非常急剧地增加；这个公式有
		 * 这个性质，测试里表现也够用，但
		 * 也许还能做得更好。
		 */
		lossy_pages = Max(0, heap_pages - maxentries / 2);
		exact_pages = heap_pages - lossy_pages;

		/*
		 * 若有有损页，就重新计算位图堆节点
		 * 处理的元组数。这里假定某个元组来自精确页的机会
		 * 与某一页是精确页的机会相同。这也许不成立，但
		 * 不清楚还能怎么
		 * 做得更好。
		 */
		if (lossy_pages > 0)
			tuples_fetched =
				clamp_row_est(indexSelectivity *
							  (exact_pages / heap_pages) * baserel->tuples +
							  (lossy_pages / heap_pages) * baserel->tuples);
	}

	if (cost)
		*cost = indexTotalCost;
	if (tuple)
		*tuple = tuples_fetched;

	return pages_fetched;
}
