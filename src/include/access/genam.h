/*-------------------------------------------------------------------------
 *
 * genam.h
 *	  POSTGRES 通用索引访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/genam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENAM_H
#define GENAM_H

#include "access/sdir.h"
#include "access/skey.h"
#include "nodes/tidbitmap.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* 我们不希望本文件依赖 execnodes.h。 */
struct IndexInfo;

/*
 * ambuild 返回的统计信息结构体
 */
typedef struct IndexBuildResult
{
	double		heap_tuples;	/* # 父表中所见元组数 */
	double		index_tuples;	/* # 插入索引的元组数 */
} IndexBuildResult;

/*
 * 传给 ambulkdelete 与 amvacuumcleanup 的输入参数结构体
 *
 * 仅当 estimated_count 为 false，num_heap_tuples 才准确；
 * 否则它只是估计值(当前该估计值取自关系
 * pg_class.reltuples 字段的先前值，因此甚至
 * 可能为 -1)。在 ambulkdelete 期间它始终只是估计值。
 */
typedef struct IndexVacuumInfo
{
	Relation	index;			/* 被清理的索引 */
	Relation	heaprel;		/* 该索引所属的堆关系 */
	bool		analyze_only;	/* ANALYZE(无实际 vacuum) */
	bool		report_progress;	/* 发出 progress.h 状态报告 */
	bool		estimated_count;	/* num_heap_tuples 为估计值 */
	int			message_level;	/* 进度消息的 ereport 级别 */
	double		num_heap_tuples;	/* 堆中剩余的元组数 */
	BufferAccessStrategy strategy;	/* 读取的访问策略 */
} IndexVacuumInfo;

/*
 * ambulkdelete 与 amvacuumcleanup 返回的统计信息结构体
 *
 * 该结构体通常由首个 ambulkdelete 调用分配，并沿后续
 * 调用一路传递至 amvacuumcleanup；但若未发生任何
 * ambulkdelete 调用(因无元组需删除)，amvacuumcleanup
 * 必须能自行分配它。
 * 注意：索引访问方法可返回更大的结构体，
 * 而本结构体只是其首个字段；借此 ambulkdelete 可
 * 向 amvacuumcleanup 传递额外私有数据。
 *
 * 注意：pages_newly_deleted 是索引中被本次 vacuum 操作
 * 删除的页数。pages_deleted 与 pages_free 指的是
 * 索引文件内的空闲空间。
 *
 * 注意：某些索引访问方法可能参照 num_heap_tuples
 * 计算 num_index_tuples，此时应复制 estimated_count 字段，取自
 * IndexVacuumInfo。
 */
typedef struct IndexBulkDeleteResult
{
	BlockNumber num_pages;		/* 索引中剩余的页数 */
	bool		estimated_count;	/* num_index_tuples 为估计值 */
	double		num_index_tuples;	/* 剩余元组数 */
	double		tuples_removed; /* # vacuum 操作期间移除数 */
	BlockNumber pages_newly_deleted;	/* # 本次标记删除页数  */
	BlockNumber pages_deleted;	/* # 标记删除页数(可能是本次) */
	BlockNumber pages_free;		/* # 可供复用的页数 */
} IndexBulkDeleteResult;

/* 判定元组是否可批量删除的回调函数类型定义 */
typedef bool (*IndexBulkDeleteCallback) (ItemPointer itemptr, void *state);

/* 结构体定义见 relscan.h */
typedef struct IndexScanDescData *IndexScanDesc;
typedef struct SysScanDescData *SysScanDesc;

typedef struct ParallelIndexScanDescData *ParallelIndexScanDesc;

/*
 * 枚举，指定 index_insert() 中要执行的唯一性检查
 * 类型。
 *
 * UNIQUE_CHECK_YES 是 Postgres 传统的即时检查，可能会
 * 为判断冲突事务是否提交而阻塞。
 *
 * 对可延迟唯一约束，插入时指定 UNIQUE_CHECK_PARTIAL。
 * 索引访问方法应检测元组是否唯一，但元组看似
 * 不唯一时，不得抛出错误、阻塞或阻止插入。
 * 待约束需强制执行时，我们再重新检查。
 * 访问方法在元组确知唯一时必须返回 true，
 * 可能不唯一则返回 false。返回 true 时
 * 可安全省略之后的复查。
 *
 * 复查延迟约束时，以 UNIQUE_CHECK_EXISTING 发起一次
 * 伪插入调用。此时元组已在索引中，故不应再插入；
 * 而只需检查是否存在冲突的活元组，这有可能
 * 造成阻塞。
 */
typedef enum IndexUniqueCheck
{
	UNIQUE_CHECK_NO,			/* 不做任何唯一性检查 */
	UNIQUE_CHECK_YES,			/* 在插入时强制唯一性 */
	UNIQUE_CHECK_PARTIAL,		/* 检测唯一性但不报错 */
	UNIQUE_CHECK_EXISTING		/* 检查现有元组是否唯一 */
} IndexUniqueCheck;


/* Nullable "ORDER BY col op const" distance */
typedef struct IndexOrderByDistance
{
	double		value;
	bool		isnull;
} IndexOrderByDistance;

/*
 * 通用 index_ 接口例程(位于 indexam.c)
 */

/*
 * IndexScanIsValid
 *		索引扫描有效时才为真。
 */
#define IndexScanIsValid(scan) PointerIsValid(scan)

extern Relation index_open(Oid relationId, LOCKMODE lockmode);
extern Relation try_index_open(Oid relationId, LOCKMODE lockmode);
extern void index_close(Relation relation, LOCKMODE lockmode);

extern bool index_insert(Relation indexRelation,
						 Datum *values, bool *isnull,
						 ItemPointer heap_t_ctid,
						 Relation heapRelation,
						 IndexUniqueCheck checkUnique,
						 bool indexUnchanged,
						 struct IndexInfo *indexInfo);

extern IndexScanDesc index_beginscan(Relation heapRelation,
									 Relation indexRelation,
									 Snapshot snapshot,
									 int nkeys, int norderbys);
extern IndexScanDesc index_beginscan_bitmap(Relation indexRelation,
											Snapshot snapshot,
											int nkeys);
extern void index_rescan(IndexScanDesc scan,
						 ScanKey keys, int nkeys,
						 ScanKey orderbys, int norderbys);
extern void index_endscan(IndexScanDesc scan);
extern void index_markpos(IndexScanDesc scan);
extern void index_restrpos(IndexScanDesc scan);
extern Size index_parallelscan_estimate(Relation indexRelation, Snapshot snapshot);
extern void index_parallelscan_initialize(Relation heapRelation,
										  Relation indexRelation, Snapshot snapshot,
										  ParallelIndexScanDesc target);
extern void index_parallelrescan(IndexScanDesc scan);
extern IndexScanDesc index_beginscan_parallel(Relation heaprel,
											  Relation indexrel, int nkeys, int norderbys,
											  ParallelIndexScanDesc pscan);
extern ItemPointer index_getnext_tid(IndexScanDesc scan,
									 ScanDirection direction);
struct TupleTableSlot;
extern bool index_fetch_heap(IndexScanDesc scan, struct TupleTableSlot *slot);
extern bool index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
							   struct TupleTableSlot *slot);
extern int64 index_getbitmap(IndexScanDesc scan, TIDBitmap *bitmap);

extern IndexBulkDeleteResult *index_bulk_delete(IndexVacuumInfo *info,
												IndexBulkDeleteResult *istat,
												IndexBulkDeleteCallback callback,
												void *callback_state);
extern IndexBulkDeleteResult *index_vacuum_cleanup(IndexVacuumInfo *info,
												   IndexBulkDeleteResult *istat);
extern bool index_can_return(Relation indexRelation, int attno);
extern RegProcedure index_getprocid(Relation irel, AttrNumber attnum,
									uint16 procnum);
extern FmgrInfo *index_getprocinfo(Relation irel, AttrNumber attnum,
								   uint16 procnum);
extern void index_store_float8_orderby_distances(IndexScanDesc scan,
												 Oid *orderByTypes,
												 IndexOrderByDistance *distances,
												 bool recheckOrderBy);
extern bytea *index_opclass_options(Relation indrel, AttrNumber attnum,
									Datum attoptions, bool validate);


/*
 * 索引访问方法支持例程(位于 genam.c)
 */
extern IndexScanDesc RelationGetIndexScan(Relation indexRelation,
										  int nkeys, int norderbys);
extern void IndexScanEnd(IndexScanDesc scan);
extern char *BuildIndexValueDescription(Relation indexRelation,
										Datum *values, bool *isnull);
extern TransactionId index_compute_xid_horizon_for_tuples(Relation irel,
														  Relation hrel,
														  Buffer ibuf,
														  OffsetNumber *itemnos,
														  int nitems);

/*
 * 系统目录的堆或索引访问(位于 genam.c)
 */
extern SysScanDesc systable_beginscan(Relation heapRelation,
									  Oid indexId,
									  bool indexOK,
									  Snapshot snapshot,
									  int nkeys, ScanKey key);
extern HeapTuple systable_getnext(SysScanDesc sysscan);
extern bool systable_recheck_tuple(SysScanDesc sysscan, HeapTuple tup);
extern void systable_endscan(SysScanDesc sysscan);
extern SysScanDesc systable_beginscan_ordered(Relation heapRelation,
											  Relation indexRelation,
											  Snapshot snapshot,
											  int nkeys, ScanKey key);
extern HeapTuple systable_getnext_ordered(SysScanDesc sysscan,
										  ScanDirection direction);
extern void systable_endscan_ordered(SysScanDesc sysscan);
extern void systable_inplace_update_begin(Relation relation,
										  Oid indexId,
										  bool indexOK,
										  Snapshot snapshot,
										  int nkeys, const ScanKeyData *key,
										  HeapTuple *oldtupcopy,
										  void **state);
extern void systable_inplace_update_finish(void *state, HeapTuple tuple);
extern void systable_inplace_update_cancel(void *state);

#endif							/* GENAM_H */
