/*-------------------------------------------------------------------------
 *
 * selfuncs.h
 *	  标准运算符的选择率函数，以及选择率与
 *	  代价估计的配套基础设施。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SELFUNCS_H
#define SELFUNCS_H

#include "access/htup.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"


/*
 * 注意：默认选择率估计并非完全随意选取。我们希望
 * 它们足够小，以便在典型表密度约 100 元组/页时能
 * 用上索引扫描。例如 0.01 还不够小，因为那样会看
 * 起来几乎每页都会被命中。另因有时把 eqsel 估为
 * 1/num_distinct，DEFAULT_NUM_DISTINCT 宜等于
 * 1/DEFAULT_EQ_SEL。
 * 见上。
 */

/* 诸如 "A = b" 的相等比较的默认选择率估计 */
#define DEFAULT_EQ_SEL	0.005

/* 诸如 "A < b" 的不等比较的默认选择率估计 */
#define DEFAULT_INEQ_SEL  0.3333333333333333

/* 范围不等 "A > b AND A < c" 的默认选择率估计 */
#define DEFAULT_RANGE_INEQ_SEL	0.005

/* 多重范围不等 "A > b AND A < c" 的默认选择率估计 */
#define DEFAULT_MULTIRANGE_INEQ_SEL	0.005

/* 诸如 LIKE 的模式匹配运算符的默认选择率估计 */
#define DEFAULT_MATCH_SEL	0.005

/* 其他匹配运算符的默认选择率估计 */
#define DEFAULT_MATCHING_SEL	0.010

/* 表中不同值个数的默认估计 */
#define DEFAULT_NUM_DISTINCT  200

/* 布尔与空值测试节点的默认选择率估计 */
#define DEFAULT_UNK_SEL			0.005
#define DEFAULT_NOT_UNK_SEL		(1.0 - DEFAULT_UNK_SEL)


/*
 * 把可能因舍入或估计误差而不在有效范围的概率估计
 * 钳到合法区间。参数须为 float 变量。
 */
#define CLAMP_PROBABILITY(p) \
	do { \
		if (p < 0.0) \
			p = 0.0; \
		else if (p > 1.0) \
			p = 1.0; \
	} while (0)

/*
 * 若干选择率估计函数可回传给调用方的一组标志，以
 * 提供估计过程中所作某些假设的更多细节。
 * 见上。
 */
#define SELFLAG_USED_DEFAULT		(1 << 0)	/* 估计回退到下列
												 * DEFAULTs 之一
												 * 见上 */

typedef struct EstimationInfo
{
	uint32		flags;			/* 标志，如上定义，标记估计的
								 * 特殊性质。 */
} EstimationInfo;

/* examine_variable 及其友元的返回数据 */
typedef struct VariableStatData
{
	Node	   *var;			/* Var 或表达式树 */
	RelOptInfo *rel;			/* Relation，无法识别则为 NULL */
	HeapTuple	statsTuple;		/* pg_statistic 元组，否则 NULL */
	/* 注意：若 statsTuple!=NULL，调用方用完须释放 */
	void		(*freefunc) (HeapTuple tuple);	/* how to free statsTuple */
	Oid			vartype;		/* 表达式的对外类型 */
	Oid			atttype;		/* 实际类型（剥 relabel 后） */
	int32		atttypmod;		/* 实际 typmod（剥 relabel 后） */
	bool		isunique;		/* 匹配唯一索引或 DISTINCT 子句 */
	bool		acl_ok;			/* 用户是否对表或列的全部行有
								 * SELECT 特权 */
} VariableStatData;

#define ReleaseVariableStats(vardata)  \
	do { \
		if (HeapTupleIsValid((vardata).statsTuple)) \
			(vardata).freefunc((vardata).statsTuple); \
	} while(0)


/*
 * genericcostestimate 是多数索引类型可用的通用估
 * 计器。有时以其为基，再在类型专用调用函数中并入
 * 额外的索引类型知识。为免代码重复，它除初步输出
 * 代价估计外还返回若干中间值。GenericCosts 结构
 * 含全部这些值。
 * 见上。
 * 见上。
 *
 * 调用方应将 GenericCosts 全部字段初始化为零。另
 * 若有比默认更好的叶索引元组访问数估计，可将
 * numIndexTuples 设为正值。
 */
typedef struct
{
	/* 代价估计器必须返回给规划器的这些值 */
	Cost		indexStartupCost;	/* 索引相关启动代价 */
	Cost		indexTotalCost; /* 索引相关总扫描代价 */
	Selectivity indexSelectivity;	/* 索引的选择率 */
	double		indexCorrelation;	/* 索引的次序相关性 */

	/* 途中得到的中间值 */
	double		numIndexPages;	/* 访问的叶页数 */
	double		numIndexTuples; /* 访问的叶元组数 */
	double		spc_random_page_cost;	/* 相关的 random_page_cost 值 */
	double		num_sa_scans;	/* ScalarArrayOpExprs 索引扫描数 */
} GenericCosts;

/* 请求统计时插件取得控制的钩子 */
typedef bool (*get_relation_stats_hook_type) (PlannerInfo *root,
											  RangeTblEntry *rte,
											  AttrNumber attnum,
											  VariableStatData *vardata);
extern PGDLLIMPORT get_relation_stats_hook_type get_relation_stats_hook;
typedef bool (*get_index_stats_hook_type) (PlannerInfo *root,
										   Oid indexOid,
										   AttrNumber indexattnum,
										   VariableStatData *vardata);
extern PGDLLIMPORT get_index_stats_hook_type get_index_stats_hook;

/* selfuncs.c 中的函数 */

extern void examine_variable(PlannerInfo *root, Node *node, int varRelid,
							 VariableStatData *vardata);
extern bool all_rows_selectable(PlannerInfo *root, Index varno, Bitmapset *varattnos);
extern bool statistic_proc_security_check(VariableStatData *vardata, Oid func_oid);
extern bool get_restriction_variable(PlannerInfo *root, List *args,
									 int varRelid,
									 VariableStatData *vardata, Node **other,
									 bool *varonleft);
extern void get_join_variables(PlannerInfo *root, List *args,
							   SpecialJoinInfo *sjinfo,
							   VariableStatData *vardata1,
							   VariableStatData *vardata2,
							   bool *join_is_reversed);
extern double get_variable_numdistinct(VariableStatData *vardata,
									   bool *isdefault);
extern double mcv_selectivity(VariableStatData *vardata,
							  FmgrInfo *opproc, Oid collation,
							  Datum constval, bool varonleft,
							  double *sumcommonp);
extern double histogram_selectivity(VariableStatData *vardata,
									FmgrInfo *opproc, Oid collation,
									Datum constval, bool varonleft,
									int min_hist_size, int n_skip,
									int *hist_size);
extern double generic_restriction_selectivity(PlannerInfo *root,
											  Oid oproid, Oid collation,
											  List *args, int varRelid,
											  double default_selectivity);
extern double ineq_histogram_selectivity(PlannerInfo *root,
										 VariableStatData *vardata,
										 Oid opoid, FmgrInfo *opproc,
										 bool isgt, bool iseq,
										 Oid collation,
										 Datum constval, Oid consttype);
extern double var_eq_const(VariableStatData *vardata,
						   Oid oproid, Oid collation,
						   Datum constval, bool constisnull,
						   bool varonleft, bool negate);
extern double var_eq_non_const(VariableStatData *vardata,
							   Oid oproid, Oid collation,
							   Node *other,
							   bool varonleft, bool negate);

extern Selectivity boolvarsel(PlannerInfo *root, Node *arg, int varRelid);
extern Selectivity booltestsel(PlannerInfo *root, BoolTestType booltesttype,
							   Node *arg, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo);
extern Selectivity nulltestsel(PlannerInfo *root, NullTestType nulltesttype,
							   Node *arg, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo);
extern Selectivity scalararraysel(PlannerInfo *root,
								  ScalarArrayOpExpr *clause,
								  bool is_join_clause,
								  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo);
extern int	estimate_array_length(Node *arrayexpr);
extern Selectivity rowcomparesel(PlannerInfo *root,
								 RowCompareExpr *clause,
								 int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo);

extern void mergejoinscansel(PlannerInfo *root, Node *clause,
							 Oid opfamily, int strategy, bool nulls_first,
							 Selectivity *leftstart, Selectivity *leftend,
							 Selectivity *rightstart, Selectivity *rightend);

extern double estimate_num_groups(PlannerInfo *root, List *groupExprs,
								  double input_rows, List **pgset,
								  EstimationInfo *estinfo);

extern void estimate_hash_bucket_stats(PlannerInfo *root,
									   Node *hashkey, double nbuckets,
									   Selectivity *mcv_freq,
									   Selectivity *bucketsize_frac);
extern double estimate_hashagg_tablesize(PlannerInfo *root, Path *path,
										 const AggClauseCosts *agg_costs,
										 double dNumGroups);

extern List *get_quals_from_indexclauses(List *indexclauses);
extern Cost index_other_operands_eval_cost(PlannerInfo *root,
										   List *indexquals);
extern List *add_predicate_to_index_quals(IndexOptInfo *index,
										  List *indexQuals);
extern void genericcostestimate(PlannerInfo *root, IndexPath *path,
								double loop_count,
								GenericCosts *costs);

/* array_selfuncs.c 中的函数 */

extern Selectivity scalararraysel_containment(PlannerInfo *root,
											  Node *leftop, Node *rightop,
											  Oid elemtype, bool isEquality, bool useOr,
											  int varRelid);

#endif							/* SELFUNCS_H */
