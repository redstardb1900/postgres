/*-------------------------------------------------------------------------
 *
 * vacuum.h
 *	postgres 清理器与统计信息分析器的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/vacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUM_H
#define VACUUM_H

#include "access/htup.h"
#include "access/genam.h"
#include "access/parallel.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "parser/parse_node.h"
#include "storage/buf.h"
#include "storage/lock.h"
#include "utils/relcache.h"

/*
 * amparallelvacuumoptions 标志,控制 bulkdelete 与
 * vacuumcleanup 参与并行 vacuum。
 */

/*
 * 默认禁用 bulkdelete 与 vacuumcleanup。供不想或
 * 不能参与并行 vacuum 的 IndexAM 使用。例如，若索引
 * AM 无法把首次 ambulkdelete 调用分配的索引统计
 * 传递给后续调用直至 amvacuumcleanup，则该索引 AM
 * 不能参与并行
 * vacuum.
 */
#define VACUUM_OPTION_NO_PARALLEL			0

/*
 * 可并行执行 bulkdelete。供需要扫描索引以删除
 * 元组的索引 AM 使用。
 */
#define VACUUM_OPTION_PARALLEL_BULKDEL		(1 << 0)

/*
 * 若尚未执行 bulkdelete，则可并行执行 vacuumcleanup。
 * 供能在未执行 bulkdelete 时扫描索引的
 * IndexAM 使用。
 */
#define VACUUM_OPTION_PARALLEL_COND_CLEANUP	(1 << 1)

/*
 * 即使 bulkdelete 已处理过该索引，也可并行执行
 * vacuumcleanup。供在索引清理阶段扫描索引、
 * 无论 bulkdelete 阶段是否已扫描过该索引的
 * IndexAM 使用。
 */
#define VACUUM_OPTION_PARALLEL_CLEANUP		(1 << 2)

/* 用于检查 vacuum 标志值 */
#define VACUUM_OPTION_MAX_VALID_VALUE		((1 << 3) - 1)

/* 并行 vacuum 状态的抽象类型 */
typedef struct ParallelVacuumState ParallelVacuumState;

/*----------
 * ANALYZE 为每个待分析属性（列）构建本结构之一。
 * 结构及附属数据位于 anl_context，
 * 故存活至 ANALYZE 操作结束。
 *
 * 类型相关的 typanalyze 函数收到指向本结构的指针，
 * 须返回 true 以继续分析，或 false 以跳过本列。
 * 返回 true 时须设置 compute_stats 与 minrows 字段，
 * 并可选用 extra_data 向 compute_stats 传附加信息。
 * minrows 是它请求的最少采样行数
 * （注意该请求未必兑现，例如表中行数
 * 少于此数时）。
 *
 * 采样行收集完毕后将调用 compute_stats
 * 例程。除本结构外还传入：
 *		fetchfunc: 从采样行访问列值的函数
 *				   (采样行)
 *		samplerows: 采样元组数
 *		totalrows: 关系估计总行数
 * fetchfunc 的 rownum 可从 0 跑到 samplerows-1。
 * 它返回 Datum 与 isNull 标志。
 *
 * 若能算出有用统计，compute_stats 应置 stats_valid
 * 为真。若如此，结构余部保存将写入该列
 * pg_statistic 行的信息。务必在 anl_context 中
 * 分配所有指针指向的数据；调用 compute_stats 时
 * 它不是 CurrentMemoryContext。
 *
 * 注意：凡统计用途的比较均应使用底层列的
 * 排序规则（attcollation），除非不可排序容器类型
 * 内含可排序类型；此时用该类型默认排序规则。
 * 务必把合适的排序规则记入
 * stacoll。
 *----------
 */
typedef struct VacAttrStats *VacAttrStatsP;

typedef Datum (*AnalyzeAttrFetchFunc) (VacAttrStatsP stats, int rownum,
									   bool *isNull);

typedef void (*AnalyzeAttrComputeStatsFunc) (VacAttrStatsP stats,
											 AnalyzeAttrFetchFunc fetchfunc,
											 int samplerows,
											 double totalrows);

typedef struct VacAttrStats
{
	/*
	 * 这些字段由主 ANALYZE 代码在调用类型相关
	 * typanalyze 函数前设置。
	 *
	 * 注意：勿假定被分析数据与 attr 所示类型相同，
	 * 即勿信任 attr->atttypid、attlen 等。因为有些
	 * 索引操作类存储的类型异于底层列/表达式。
	 * 应改用 attrtypid、attrtypmod 与 attrtype 了解
	 * 喂给 typanalyze 函数的数据类型。
	 * 同理用 attrcollid 而非 attr->attcollation.
	 */
	Form_pg_attribute attr;		/* 列的 pg_attribute 行副本 */
	Oid			attrtypid;		/* 被分析数据的类型 */
	int32		attrtypmod;		/* 被分析数据的 typmod */
	Form_pg_type attrtype;		/* attrtypid 的 pg_type 行副本 */
	Oid			attrcollid;		/* 被分析数据的排序规则 */
	MemoryContext anl_context;	/* 长寿数据的保存处 */

	/*
	 * 这些字段须由 typanalyze 例程填写，除非它
	 * 返回 false.
	 */
	AnalyzeAttrComputeStatsFunc compute_stats;	/* 函数指针 */
	int			minrows;		/* 统计所需最少行数 */
	void	   *extra_data;		/* 额外类型相关数据 */

	/*
	 * 这些字段由 compute_stats 例程填写。（结构
	 * 创建时初始化为零。）
	 */
	bool		stats_valid;
	float4		stanullfrac;	/* 为 NULL 的项所占比例 */
	int32		stawidth;		/* 列值平均宽度 */
	float4		stadistinct;	/* 不同值个数 */
	int16		stakind[STATISTIC_NUM_SLOTS];
	Oid			staop[STATISTIC_NUM_SLOTS];
	Oid			stacoll[STATISTIC_NUM_SLOTS];
	int			numnumbers[STATISTIC_NUM_SLOTS];
	float4	   *stanumbers[STATISTIC_NUM_SLOTS];
	int			numvalues[STATISTIC_NUM_SLOTS];
	Datum	   *stavalues[STATISTIC_NUM_SLOTS];

	/*
	 * 这些字段描述 stavalues[n] 元素类型。将初始化
	 * 为与 attrtypid 匹配，但定制 typanalyze 函数可能
	 * 想存非被分析列元素的数组，
	 * 此时应覆盖这些字段。
	 */
	Oid			statypid[STATISTIC_NUM_SLOTS];
	int16		statyplen[STATISTIC_NUM_SLOTS];
	bool		statypbyval[STATISTIC_NUM_SLOTS];
	char		statypalign[STATISTIC_NUM_SLOTS];

	/*
	 * 这些字段属主 ANALYZE 代码私有，类型相关函数
	 * 不应查看。
	 */
	int			tupattnum;		/* 元组内属性号 */
	HeapTuple  *rows;			/* 标准取函数的访问信息 */
	TupleDesc	tupDesc;
	Datum	   *exprvals;		/* 索引取函数的访问信息 */
	bool	   *exprnulls;
	int			rowstride;
} VacAttrStats;

/* VacuumParams->options 的标志位 */
#define VACOPT_VACUUM 0x01		/* VACUUM */
#define VACOPT_ANALYZE 0x02		/* ANALYZE */
#define VACOPT_VERBOSE 0x04		/* 输出 INFO 检测消息 */
#define VACOPT_FREEZE 0x08		/* FREEZE 选项 */
#define VACOPT_FULL 0x10		/* FULL（非并发）vacuum */
#define VACOPT_SKIP_LOCKED 0x20 /* 无法获锁则跳过 */
#define VACOPT_PROCESS_MAIN 0x40	/* 处理主关系 */
#define VACOPT_PROCESS_TOAST 0x80	/* 若有则处理 TOAST 表 */
#define VACOPT_DISABLE_PAGE_SKIPPING 0x100	/* 不跳过任何页 */
#define VACOPT_SKIP_DATABASE_STATS 0x200	/* 跳过 datfrozenxid 更新 */
#define VACOPT_ONLY_DATABASE_STATS 0x400	/* 仅 vac_update_datfrozenxid() */

/*
 * index_cleanup 与 truncate 参数所用值。
 *
 * VACUUM 命令无显式值时，VACOPTVALUE_UNSPECIFIED
 * 用作初始占位。此时最终可用值来自对应
 * reloption（通常用 reloption
 * 默认值)。
 */
typedef enum VacOptValue
{
	VACOPTVALUE_UNSPECIFIED = 0,
	VACOPTVALUE_AUTO,
	VACOPTVALUE_DISABLED,
	VACOPTVALUE_ENABLED,
} VacOptValue;

/*
 * Parameters customizing behavior of VACUUM and ANALYZE.
 *
 * Note that at least one of VACOPT_VACUUM and VACOPT_ANALYZE must be set
 * in options.
 *
 * When adding a new VacuumParam member, consider adding it to vacuumdb as
 * well.
 */
typedef struct VacuumParams
{
	bits32		options;		/* VACOPT_* 位掩码 */
	int			freeze_min_age; /* 最小冻结年龄，-1 用默认 */
	int			freeze_table_age;	/* 扫描整表的年龄阈值 */
	int			multixact_freeze_min_age;	/* mxact 最小冻结年龄,-1 用
											 * 默认 */
	int			multixact_freeze_table_age; /* 扫描整表的 multixact
											 * 年龄 */
	bool		is_wraparound;	/* 强制环绕式 vacuum */
	int			log_min_duration;	/* 自动清理记日志阈值(ms),
									 * -1 表示用
									 * 默认 */
	VacOptValue index_cleanup;	/* 做索引 vacuum 与清理 */
	VacOptValue truncate;		/* 末尾截断空页 */

	/*
	 * 并行 vacuum 工作进程数。默认 0 表示按索引数
	 * 选择。-1 表示禁用并行
	 * vacuum。
	 */
	int			nworkers;
} VacuumParams;

/*
 * VacuumCutoffs 是描述 VACUUM 所用截断点的不可变状态。
 * 在每次 VACUUM 操作开始时确立。
 */
struct VacuumCutoffs
{
	/*
	 * VACUUM 开始时已有的 pg_class 字段
	 */
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	/*
	 * OldestXmin 是 Xid 下界：任何已提交事务删除的元组
	 * 低于此值应视为 DEAD，而非仅 RECENTLY_DEAD。
	 *
	 * OldestMxact 是 Mxid 下界:低于此的 MultiXact
	 * 必不被运行中事务视为可见.
	 *
	 * OldestXmin 与 OldestMxact 也是 VACUUM 结束时能传给
	 * vac_update_relstats() 作为 frozenxid 与 minmulti
	 * 参数的最新值。当 VACUUM 将不留下未冻结
	 * XID/MXID 时，也应传入这些
	 * 值。
	 */
	TransactionId OldestXmin;
	MultiXactId OldestMxact;

	/*
	 * FreezeLimit 是 Xid 下界：VACUUM 扫描并持清理锁页中，
	 * 低于此值的 Xid 必已冻结或移除。
	 *
	 * MultiXactCutoff 是下界:VACUUM 扫描并持清理锁页中,
	 * 低于此的 MultiXactIds 必已从 Xmax 移除。
	 */
	TransactionId FreezeLimit;
	MultiXactId MultiXactCutoff;
};

/*
 * VacDeadItems 保存索引清理删除其索引元组的 TIDs。
 */
typedef struct VacDeadItems
{
	int			max_items;		/* 数组中已分配槽数 */
	int			num_items;		/* 当前项数 */

	/* 要从索引删除的 TIDs 已排序数组 */
	ItemPointerData items[FLEXIBLE_ARRAY_MEMBER];
} VacDeadItems;

#define MAXDEADITEMS(avail_mem) \
	(((avail_mem) - offsetof(VacDeadItems, items)) / sizeof(ItemPointerData))

/* GUC 参数 */
extern PGDLLIMPORT int default_statistics_target;	/* PostGIS 用 PGDLLIMPORT */
extern PGDLLIMPORT int vacuum_freeze_min_age;
extern PGDLLIMPORT int vacuum_freeze_table_age;
extern PGDLLIMPORT int vacuum_multixact_freeze_min_age;
extern PGDLLIMPORT int vacuum_multixact_freeze_table_age;
extern PGDLLIMPORT int vacuum_failsafe_age;
extern PGDLLIMPORT int vacuum_multixact_failsafe_age;

/* 基于代价的并行 vacuum 变量 */
extern PGDLLIMPORT pg_atomic_uint32 *VacuumSharedCostBalance;
extern PGDLLIMPORT pg_atomic_uint32 *VacuumActiveNWorkers;
extern PGDLLIMPORT int VacuumCostBalanceLocal;

extern PGDLLIMPORT bool VacuumFailsafeActive;
extern PGDLLIMPORT double vacuum_cost_delay;
extern PGDLLIMPORT int vacuum_cost_limit;

/* commands/vacuum.c */
extern void ExecVacuum(ParseState *pstate, VacuumStmt *vacstmt, bool isTopLevel);
extern void vacuum(List *relations, VacuumParams *params,
				   BufferAccessStrategy bstrategy, MemoryContext vac_context,
				   bool isTopLevel);
extern void vac_open_indexes(Relation relation, LOCKMODE lockmode,
							 int *nindexes, Relation **Irel);
extern void vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode);
extern double vac_estimate_reltuples(Relation relation,
									 BlockNumber total_pages,
									 BlockNumber scanned_pages,
									 double scanned_tuples);
extern void vac_update_relstats(Relation relation,
								BlockNumber num_pages,
								double num_tuples,
								BlockNumber num_all_visible_pages,
								bool hasindex,
								TransactionId frozenxid,
								MultiXactId minmulti,
								bool *frozenxid_updated,
								bool *minmulti_updated,
								bool in_outer_xact);
extern bool vacuum_get_cutoffs(Relation rel, const VacuumParams *params,
							   struct VacuumCutoffs *cutoffs);
extern bool vacuum_xid_failsafe_check(const struct VacuumCutoffs *cutoffs);
extern void vac_update_datfrozenxid(void);
extern void vacuum_delay_point(void);
extern bool vacuum_is_relation_owner(Oid relid, Form_pg_class reltuple,
									 bits32 options);
extern Relation vacuum_open_relation(Oid relid, RangeVar *relation,
									 bits32 options, bool verbose,
									 LOCKMODE lmode);
extern IndexBulkDeleteResult *vac_bulkdel_one_index(IndexVacuumInfo *ivinfo,
													IndexBulkDeleteResult *istat,
													VacDeadItems *dead_items);
extern IndexBulkDeleteResult *vac_cleanup_one_index(IndexVacuumInfo *ivinfo,
													IndexBulkDeleteResult *istat);
extern Size vac_max_items_to_alloc_size(int max_items);

/* postmaster/autovacuum.c */
extern void AutoVacuumUpdateCostLimit(void);
extern void VacuumUpdateCosts(void);

/* commands/vacuumparallel.c */
extern ParallelVacuumState *parallel_vacuum_init(Relation rel, Relation *indrels,
												 int nindexes, int nrequested_workers,
												 int max_items, int elevel,
												 BufferAccessStrategy bstrategy);
extern void parallel_vacuum_end(ParallelVacuumState *pvs, IndexBulkDeleteResult **istats);
extern VacDeadItems *parallel_vacuum_get_dead_items(ParallelVacuumState *pvs);
extern void parallel_vacuum_bulkdel_all_indexes(ParallelVacuumState *pvs,
												long num_table_tuples,
												int num_index_scans);
extern void parallel_vacuum_cleanup_all_indexes(ParallelVacuumState *pvs,
												long num_table_tuples,
												int num_index_scans,
												bool estimated_count);
extern void parallel_vacuum_main(dsm_segment *seg, shm_toc *toc);

/* commands/analyze.c */
extern void analyze_rel(Oid relid, RangeVar *relation,
						VacuumParams *params, List *va_cols, bool in_outer_xact,
						BufferAccessStrategy bstrategy);
extern bool std_typanalyze(VacAttrStats *stats);

/* 见 utils/misc/sampling.c —— 与 utils/sampling.h 声明重复 */
extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif							/* VACUUM_H */
