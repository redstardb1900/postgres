/*-------------------------------------------------------------------------
 *
 * tableam.h
 *	POSTGRES 表访问方法定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam.h
 *
 * NOTES
 *	更高层文档见 tableam.sgml。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_H
#define TABLEAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "utils/snapshot.h"


#define DEFAULT_TABLE_ACCESS_METHOD	"heap"

/* GUCs */
extern PGDLLIMPORT char *default_table_access_method;
extern PGDLLIMPORT bool synchronize_seqscans;


struct BulkInsertStateData;
struct IndexInfo;
struct SampleScanState;
struct TBMIterateResult;
struct VacuumParams;
struct ValidateIndexState;

/*
 * scan_begin 回调 flags 参数的位掩码值。
 */
typedef enum ScanOptions
{
	/* 可指定其中一个 SO_TYPE_* */
	SO_TYPE_SEQSCAN = 1 << 0,
	SO_TYPE_BITMAPSCAN = 1 << 1,
	SO_TYPE_SAMPLESCAN = 1 << 2,
	SO_TYPE_TIDSCAN = 1 << 3,
	SO_TYPE_TIDRANGESCAN = 1 << 4,
	SO_TYPE_ANALYZE = 1 << 5,

	/* 可指定若干 SO_ALLOW_* */
	/* 允许或禁止使用访问策略 */
	SO_ALLOW_STRAT = 1 << 6,
	/* 向 syncscan 逻辑报告位置？ */
	SO_ALLOW_SYNC = 1 << 7,
	/* 逐页校验可见性？ */
	SO_ALLOW_PAGEMODE = 1 << 8,

	/* 扫描结束时注销快照？ */
	SO_TEMP_SNAPSHOT = 1 << 9
} ScanOptions;

/*
 * table_{update,delete,lock_tuple} 及表 AM 内可见性
 * 例程的返回码。
 */
typedef enum TM_Result
{
	/*
	 * 表示动作成功（即已执行更新/删除，锁
	 * 已获取）
	 */
	TM_Ok,

	/* 受影响元组对相关快照不可见 */
	TM_Invisible,

	/* 受影响元组已被调用后端修改 */
	TM_SelfModified,

	/*
	 * 受影响元组被另一事务更新。也包括
	 * 元组被移到另一分区的情形。
	 */
	TM_Updated,

	/* 受影响元组被另一事务删除 */
	TM_Deleted,

	/*
	 * 受影响元组正被另一会话修改。仅当 
	 * table_(update/delete/lock_tuple) 被指示
	 * 不等待时返回。
	 */
	TM_BeingModified,

	/* 无法获取锁，跳过动作。仅 lock_tuple 使用 */
	TM_WouldBlock
} TM_Result;

/*
 * table_update(..., update_indexes*..) 的返回码。
 * 用来决定要更新哪些索引。
 */
typedef enum TU_UpdateIndexes
{
	/* 没有索引列被更新（含元组的 TID 寻址） */
	TU_None,

	/* 非汇总索引列被更新，或 TID 已变 */
	TU_All,

	/* 仅汇总列被更新，TID 未变 */
	TU_Summarizing
} TU_UpdateIndexes;

/*
 * 当 table_tuple_update、table_tuple_delete 或 table_tuple_lock 
 * 因目标元组已经过时而失败时，会填入本结构以
 * 向调用方说明发生了什么。这些函数成功时，
 * 除 `traversed` 外不应依赖本结构内容；该字段
 * 在成功与失败时都可能被设置。
 *
 * ctid 是目标的 ctid 链接：若目标已删除则等于其 TID；
 * 若目标被更新则为替换元组的位置。
 * 被更新。
 *
 * xmax 是使之过时的事务的 XID。若调用方要访问
 * 替换元组，必须先核对二者相符，才可相信
 * 替换真是匹配。若目标为 !LP_NORMAL 则为 
 * InvalidTransactionId（预期仅来自 syscache 的 TID）。
 *
 * cmax 是使之过时的命令的 CID，但仅当失败码为 
 * TM_SelfModified（即当前事务中有东西使该
 * 元组过时）时有值；否则 cmax 为零。
 * 因 HeapTupleHeaderGetCmax 对其他事务中过时元组
 * 不起作用
 *
 * traversed 表示是否沿更新链查找以试图锁住
 * 目标元组。（成功与失败时都可能设置。）
 */
typedef struct TM_FailureData
{
	ItemPointerData ctid;
	TransactionId xmax;
	CommandId	cmax;
	bool		traversed;
} TM_FailureData;

/*
 * 调用 table_index_delete_tuples() 的状态。
 *
 * 表示表元组状态，由表 TID 引用，由索引 AM 从
 * 索引元组取得。状态含删除操作的高层参数，外加
 * 两个可变的 palloc() 数组，记录各个表元组的
 * 状态。二者在概念上是一个数组。分成两个数组
 * 可使 TM_IndexDelete 结构体保持小巧，从而
 * 对第一个数组（deltids 数组）排序更快。
 *
 * 有些索引 AM 调用方做简单索引元组删除（指定 
 * bottomup = false），且只含已知死亡的 deltids。这些
 * 已知死亡项都直接标 knowndeletable = true（通常是
 * 来自标 LP_DEAD 的索引元组的 TIDs），但并非严格必须。
 *
 * 指定 bottomup = true 的调用方是"自底向上索引删除"
 * 调用方。tableam 对这些调用方的考量更微妙，
 * 因为它们请 tableam 做高度推测性的工作，且可能
 * 只期望 tableam 检查全部条目中的一小部分。
 * 不允许对任何条目指定 knowndeletable = true，
 * 因为一切都高度推测。自底向上调用方会向 
 * tableam 提供上下文与提示——索引 AM 与 tableam 如何在
 * 自底向上索引删除中协调，详见下方注释。
 *
 * 简单索引删除调用方也可能请 tableam 做推测性
 * 工作。这有点像自底向上删除，但并不太像。
 * tableam 仅在对简单删除调用方顺带几乎无代价时
 * 才做推测性工作（同时始终完成为使
 * knowndeletable/LP_DEAD 索引元组能在索引 AM 内
 * 被删除所需的工作）。这才是简单索引删除调用方可在
 * 一开始就把 knowndeletable = false 的真正原因（
 * 意为"若顺带很便宜，就检查我能否删除对应索引
 * 元组"）。索引 AM 应只为 TIDs 指向 tableam 反正
 * 都得访问的表块的索引元组纳入"额外"条目（
 * 面向块的 tableam 时）。tableam 并非严格必须检查这些
 * "额外" TIDs，但基于块的 AM 实践中应总能
 * 做到。
 * practice.
 *
 * deltids/status 数组的最终内容，对请 tableam 做
 * 推测性工作的调用方有意义（即事先有 _any_ 项
 * knowndeletable 为 false）。这些索引 AM 调用方
 * 自然需要查阅最终状态，以确定哪些索引元组
 * 实际可删。
 *
 * 索引 AM 可通过设置 idxoffnum（和/或依赖每项可用 
 * tid 唯一标识）跟踪哪个索引元组对应哪个 deltid，
 * 在需要解读数组最终内容时这很重要——数组在 
 * tableam 处理后可能比初始更小，和/或条目顺序改变（
 * tableam 可能因自身原因排序 deltids 数组）。
 * 自底向上调用方可能发现 tableam 返回时 final 
 * ndeltids 为 0，此时无法删除任何索引元组。
 * 简单删除调用方可依赖其已知可删的条目在
 * 最终数组中仍以可删形式出现。
 */
typedef struct TM_IndexDelete
{
	ItemPointerData tid;		/* 索引元组的表 TID */
	int16		id;				/* TM_IndexStatus 中的偏移 */
} TM_IndexDelete;

typedef struct TM_IndexStatus
{
	OffsetNumber idxoffnum;		/* 索引 AM 页偏移号 */
	bool		knowndeletable; /* 当前已知可删除？ */

	/* 以下为自底向上索引删除专用字段 */
	bool		promising;		/* 有希望（重复）元组？ */
	int16		freespace;		/* 删除则释放的索引空间 */
} TM_IndexStatus;

/*
 * 索引 AM 与 tableam 的协调是自底向上索引删除
 * 设计的核心。索引 AM 通过把某些条目标为
 * "有希望"向 tableam 提示该往哪看。索引 AM 对强烈
 * 怀疑是 UPDATEs 留下的旧版本、且该 UPDATE 并未
 * 逻辑上修改被索引值的重复索引元组这样做。索引 AM 
 * 可能觉得只把近期受此类 UPDATE 影响的条目标为
 * 有希望更有帮助。
 *
 * 自底向上索引删除起初撒大网，通常纳入目标
 * 索引页上的全部 TIDs。检查事务状态信息的代价
 * 由 tableam 操心。tableam 处于控制地位，但需要
 * 索引 AM 的仔细引导。索引 AM 要求达到 
 * bottomupfreespace 目标，而 tableam 通过累加已知
 * 可删条目的每项 freespace 值来衡量进度。（
 * 所有 !bottomup 调用方只需把这些空间相关字段
 * 置零。）
 */
typedef struct TM_IndexDeleteOp
{
	Relation	irel;			/* 目标索引关系 */
	BlockNumber iblknum;		/* 索引块号（用于错误报告） */
	bool		bottomup;		/* 自底向上非简单删除？ */
	int			bottomupfreespace;	/* bottomup 空间目标 */

	/* 以下为可变的每 TID 信息（索引 AM 初始化条目） */
	int			ndeltids;		/* 当前 deltids/status 元素个数 */
	TM_IndexDelete *deltids;
	TM_IndexStatus *status;
} TM_IndexDeleteOp;

/* table_tuple_insert 的 "options" 标志位 */
/* TABLE_INSERT_SKIP_WAL 曾为 0x0001；RelationNeedsWAL() 管辖 */
#define TABLE_INSERT_SKIP_FSM		0x0002
#define TABLE_INSERT_FROZEN			0x0004
#define TABLE_INSERT_NO_LOGICAL		0x0008

/* table_tuple_lock 的标志位 */
/* 若锁模式不冲突则跟随正在更新中的元组  */
#define TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS	(1 << 0)
/* 跟随更新链并锁住元组的最新版本 */
#define TUPLE_LOCK_FLAG_FIND_LAST_VERSION		(1 << 1)


/* table_index_build_scan 回调函数的 typedef */
typedef void (*IndexBuildCallback) (Relation index,
									ItemPointer tid,
									Datum *values,
									bool *isnull,
									bool tupleIsAlive,
									void *state);

/*
 * 表 AM 的 API 结构体。注意须按服务器生命周期
 * 分配，通常做成静态常量结构体，再由 
 * FormData_pg_am.amhandler 返回。
 *
 * 多数情况下不宜直接调回调，应改用 
 * table_* 包装函数。
 *
 * GetTableAmRoutine() 断言必填回调已填入，新增
 * 回调时请记得更新。
 */
typedef struct TableAmRoutine
{
	/* 必须设为 T_TableAmRoutine */
	NodeTag		type;


	/* ------------------------------------------------------------------------
	 * 槽相关回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 返回适合存放本 AM 元组的槽实现。
	 */
	const TupleTableSlotOps *(*slot_callbacks) (Relation rel);


	/* ------------------------------------------------------------------------
	 * 表扫描回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 开始扫描 `rel`。回调必须返回 TableScanDesc，
	 * 通常会嵌在更大的、AM 专用的结构体中。
	 *
	 * 若 nkeys != 0，结果需按这些扫描键过滤。
	 *
	 * pscan 若非 NULL，必已由 parallelscan_initialize() 
	 * 初始化，且必须针对同一关系。仅来自 
	 * table_beginscan_parallel() 时才会设置。
	 *
	 * `flags` 是位掩码，表示扫描类型（ScanOptions 的 
	 * SO_TYPE_*，目前只能指定一个）、控制扫描行为的
	 * 选项（ScanOptions 的 SO_ALLOW_*，可指定若干，
	 * AM 可忽略不支持的）以及快照是否须在 
	 * scan_end 时释放（ScanOptions 的 SO_TEMP_SNAPSHOT）。
	 */
	TableScanDesc (*scan_begin) (Relation rel,
								 Snapshot snapshot,
								 int nkeys, struct ScanKeyData *key,
								 ParallelTableScanDesc pscan,
								 uint32 flags);

	/*
	 * 释放资源并释放扫描。若 TableScanDesc.temp_snap，
	 * 则须注销 TableScanDesc.rs_snapshot。
	 */
	void		(*scan_end) (TableScanDesc scan);

	/*
	 * 重新开始关系扫描。若 set_params 为 true，
	 * 则 allow_{strat,sync,pagemode}（见 scan_begin）须纳入考虑。
	 */
	void		(*scan_rescan) (TableScanDesc scan, struct ScanKeyData *key,
								bool set_params, bool allow_strat,
								bool allow_sync, bool allow_pagemode);

	/*
	 * 从 `scan` 取下一元组，存入槽。
	 */
	bool		(*scan_getnextslot) (TableScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot);

	/*-----------
	 * 用于按 ItemPointers 范围扫描的可选函数。
	 * 实现必须二者都提供，或都不提供。
	 * 皆无
	 *
	 * scan_set_tidrange 的实现必须自行处理
	 * 任意值的 ItemPointer。即必须处理以下每一项：
	 *
	 * 1) mintid 或 maxtid 超出表尾；且
	 * 2) mintid 高于 maxtid；且
	 * 3) mintid 或 maxtid 的项偏移超出
	 * AM 所允许。
	 *
	 * 实现可假定 scan_set_tidrange 总是在
	 * scan_getnextslot_tidrange 前调用，或 scan_rescan 之后、
	 * 再调该函数之前。
	 */
	void		(*scan_set_tidrange) (TableScanDesc scan,
									  ItemPointer mintid,
									  ItemPointer maxtid);

	/*
	 * 返回 `scan` 中 TIDs 范围内的下一元组，范围由
	 * scan_set_tidrange.
	 */
	bool		(*scan_getnextslot_tidrange) (TableScanDesc scan,
											  ScanDirection direction,
											  TupleTableSlot *slot);

	/* ------------------------------------------------------------------------
	 * 并行表扫描相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 估算对本关系做并行扫描所需共享内存
	 * 大小。快照无须计入。
	 */
	Size		(*parallelscan_estimate) (Relation rel);

	/*
	 * 为对本关系的并行扫描初始化 ParallelTableScanDesc。
	 * `pscan` 大小依 parallelscan_estimate() 按同一
	 * 关系。
	 */
	Size		(*parallelscan_initialize) (Relation rel,
											ParallelTableScanDesc pscan);

	/*
	 * 为新扫描重新初始化 `pscan`。`rel` 须是
	 * parallelscan_initialize 初始化时的同一关系。
	 */
	void		(*parallelscan_reinitialize) (Relation rel,
											  ParallelTableScanDesc pscan);


	/* ------------------------------------------------------------------------
	 * 索引扫描回调
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 准备从关系取元组，供索引扫描取
	 * 元组时使用。回调必须返回
	 * IndexFetchTableData，AM 通常嵌在更大的
	 * 带附加信息的结构体中。
	 *
	 * 之后即可经 index_fetch_tuple 取索引扫描的元组。
	 */
	struct IndexFetchTableData *(*index_fetch_begin) (Relation rel);

	/*
	 * 重置索引取数。通常会释放 IndexFetchTableData
	 * 中持有的跨索引取数资源。
	 */
	void		(*index_fetch_reset) (struct IndexFetchTableData *data);

	/*
	 * 释放资源并释放索引取数。
	 */
	void		(*index_fetch_end) (struct IndexFetchTableData *data);

	/*
	 * 把 `tid` 处元组取入 `slot`，先按
	 * `snapshot` 做可见性测试。若找到元组且通过可见性
	 * 测试则返 true，否则 false。
	 *
	 * 注意：索引列未变时不一定更新索引的 AM，
	 * 须返回对快照可见的当前/正确版本，即使
	 * tid 指向该元组的
	 * 较旧版本。
	 *
	 * 对某 tid 首次调用 index_fetch_tuple 时 *call_again 为 false。
	 * 若可能还有另一元组匹配该 tid，index_fetch_tuple
	 * 须把 *call_again 置 true，向调用方表明应对
	 * 同一 tid 再次调用 index_fetch_tuple。
	 *
	 * *all_dead 若 all_dead 非 NULL，则当且仅当
	 * index_fetch_tuple 能保证无后端需要看见
	 * 该元组时须置 true。索引 AM 可免在后续搜索
	 * 返回该 tid。
	 */
	bool		(*index_fetch_tuple) (struct IndexFetchTableData *scan,
									  ItemPointer tid,
									  Snapshot snapshot,
									  TupleTableSlot *slot,
									  bool *call_again, bool *all_dead);


	/* ------------------------------------------------------------------------
	 * 对单个元组做非修改操作的回调。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 把 `tid` 处元组取入 `slot`，先按
	 * `snapshot` 做可见性测试。若找到元组且通过可见性
	 * 测试则返 true，否则 false。
	 */
	bool		(*tuple_fetch_row_version) (Relation rel,
											ItemPointer tid,
											Snapshot snapshot,
											TupleTableSlot *slot);

	/*
	 * tid 对本关系的扫描是否有效。
	 */
	bool		(*tuple_tid_valid) (TableScanDesc scan,
									ItemPointer tid);

	/*
	 * 返回 `tid` 处元组的最新版本，把 `tid` 更新为
	 * 指向最新版本。
	 */
	void		(*tuple_get_latest_tid) (TableScanDesc scan,
										 ItemPointer tid);

	/*
	 * `slot` 中的元组是否满足 `snapshot`？槽须为
	 * 适合该 AM 的类型。
	 */
	bool		(*tuple_satisfies_snapshot) (Relation rel,
											 TupleTableSlot *slot,
											 Snapshot snapshot);

	/* 见 table_index_delete_tuples */
	TransactionId (*index_delete_tuples) (Relation rel,
										  TM_IndexDeleteOp *delstate);


	/* ------------------------------------------------------------------------
	 * 物理元组操作。
	 * ------------------------------------------------------------------------
	 */

	/* 参数见 table_tuple_insert()。 */
	void		(*tuple_insert) (Relation rel, TupleTableSlot *slot,
								 CommandId cid, int options,
								 struct BulkInsertStateData *bistate);

	/* 参数见 table_tuple_insert_speculative()。 */
	void		(*tuple_insert_speculative) (Relation rel,
											 TupleTableSlot *slot,
											 CommandId cid,
											 int options,
											 struct BulkInsertStateData *bistate,
											 uint32 specToken);

	/* 参数见 table_tuple_complete_speculative()。 */
	void		(*tuple_complete_speculative) (Relation rel,
											   TupleTableSlot *slot,
											   uint32 specToken,
											   bool succeeded);

	/* 参数见 table_multi_insert()。 */
	void		(*multi_insert) (Relation rel, TupleTableSlot **slots, int nslots,
								 CommandId cid, int options, struct BulkInsertStateData *bistate);

	/* 参数见 table_tuple_delete()。 */
	TM_Result	(*tuple_delete) (Relation rel,
								 ItemPointer tid,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 bool changingPart);

	/* 参数见 table_tuple_update()。 */
	TM_Result	(*tuple_update) (Relation rel,
								 ItemPointer otid,
								 TupleTableSlot *slot,
								 CommandId cid,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 LockTupleMode *lockmode,
								 TU_UpdateIndexes *update_indexes);

	/* 参数见 table_tuple_lock()。 */
	TM_Result	(*tuple_lock) (Relation rel,
							   ItemPointer tid,
							   Snapshot snapshot,
							   TupleTableSlot *slot,
							   CommandId cid,
							   LockTupleMode mode,
							   LockWaitPolicy wait_policy,
							   uint8 flags,
							   TM_FailureData *tmfd);

	/*
	 * 完成经 tuple_insert 与 multi_insert 并指定
	 * BulkInsertState 的插入所需操作。源码树内
	 * 访问方法已停用本回调。
	 *
	 * tuple_insert 与 multi_insert 的调用方通常会把
	 * 适用标志全部传入，各 AM 须自行判断哪些
	 * 对自己有意义；finish_bulk_insert 只处理
	 * 那些标志，忽略其余。
	 *
	 * 可选回调。
	 */
	void		(*finish_bulk_insert) (Relation rel, int options);


	/* ------------------------------------------------------------------------
	 * DDL 相关功能。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 本回调须为 `rel` 创建新的关系存储，并按
	 * `persistence` 采用相应耐久行为。
	 *
	 * 注意：仅可依赖 RelationBuildLocalRelation()
	 * 填好的 relcache 子集；关系的目录条目要么
	 * 尚不存在（新关系），要么仍引用旧的
	 * relfilelocator。
	 *
	 * 输出 *freezeXid、*minmulti 必须设为适合
	 * pg_class.{relfrozenxid, relminmxid} 的值。不需要
	 * 填这些字段的 AM，可分别设为 InvalidTransactionId
	 * 与 InvalidMultiXactId。
	 *
	 * 另见 table_relation_set_new_filelocator()
	 */
	void		(*relation_set_new_filelocator) (Relation rel,
												 const RelFileLocator *newrlocator,
												 char persistence,
												 TransactionId *freezeXid,
												 MultiXactId *minmulti);

	/*
	 * 本回调须从 `rel` 当前
	 * relfilelocator 中清除全部内容。无须为事务
	 * 行为做安排。常可通过把底层存储
	 * 截断到最小尺寸。
	 *
	 * 另见 table_relation_nontransactional_truncate()
	 */
	void		(*relation_nontransactional_truncate) (Relation rel);

	/*
	 * 见 table_relation_copy_data()
	 *
	 * 通常可直接复制底层存储来实现，除非
	 * 其内部含有对表空间的引用。
	 */
	void		(*relation_copy_data) (Relation rel,
									   const RelFileLocator *newrlocator);

	/* 见 table_relation_copy_for_cluster() */
	void		(*relation_copy_for_cluster) (Relation OldTable,
											  Relation NewTable,
											  Relation OldIndex,
											  bool use_sort,
											  TransactionId OldestXmin,
											  TransactionId *xid_cutoff,
											  MultiXactId *multi_cutoff,
											  double *num_tuples,
											  double *tups_vacuumed,
											  double *tups_recently_dead);

	/*
	 * 响应关系上的 VACUUM 命令。VACUUM 可由用户
	 * 或 autovacuum 触发。AM 所执行的具体动作
	 * 高度取决于具体 AM。
	 *
	 * 进入时事务已建立，关系已加上
	 * ShareUpdateExclusive 锁。
	 *
	 * 注意 VACUUM FULL（及 CLUSTER）与 ANALYZE 都不走
	 * 本例程，即使 ANALYZE 同属同一 VACUUM 命令。
	 * command.
	 *
	 * 将来或许需要单独回调，以便
	 * 对接 autovacuum 的调度。
	 */
	void		(*relation_vacuum) (Relation rel,
									struct VacuumParams *params,
									BufferAccessStrategy bstrategy);

	/*
	 * 准备分析 `scan` 的块 `blockno`。扫描已由
	 * table_beginscan_analyze() 启动。见
	 * table_scan_analyze_next_block()
	 *
	 * 回调可获取锁之类资源，持有到
	 * table_scan_analyze_next_tuple() 返回 false。例如
	 * 持锁直到块上全部元组都已由该函数分析。
	 * scan_analyze_next_tuple.
	 *
	 * 若块不适合抽样，回调可返回 false，例如
	 * 因其是永不会含元组的元页。
	 *
	 * XXX: 这显然主要适合基于块的 AM。非块
	 * AM 的良好接口尚不清楚，因此目前
	 * 尚无。
	 */
	bool		(*scan_analyze_next_block) (TableScanDesc scan,
											BlockNumber blockno,
											BufferAccessStrategy bstrategy);

	/*
	 * 见 table_scan_analyze_next_tuple()
	 *
	 * 并非每个 AM 都有有意义的死行概念，此时
	 * 可不递增 *deadrows——但注意这可能
	 * 影响 autovacuum 调度（见 relation_vacuum
	 * 回调)。
	 */
	bool		(*scan_analyze_next_tuple) (TableScanDesc scan,
											TransactionId OldestXmin,
											double *liverows,
											double *deadrows,
											TupleTableSlot *slot);

	/* 参数见 table_index_build_range_scan。 */
	double		(*index_build_range_scan) (Relation table_rel,
										   Relation index_rel,
										   struct IndexInfo *index_info,
										   bool allow_sync,
										   bool anyvisible,
										   bool progress,
										   BlockNumber start_blockno,
										   BlockNumber numblocks,
										   IndexBuildCallback callback,
										   void *callback_state,
										   TableScanDesc scan);

	/* 参数见 table_index_validate_scan。 */
	void		(*index_validate_scan) (Relation table_rel,
										Relation index_rel,
										struct IndexInfo *index_info,
										Snapshot snapshot,
										struct ValidateIndexState *state);


	/* ------------------------------------------------------------------------
	 * 杂项函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 见 table_relation_size()
	 *
	 * 注意：目前有些调用方用 MAIN_FORKNUM 大小来
	 * 确定可能感兴趣的块范围（brin、analyze）。
	 * 日后大概需要为这些修订接口
	 * 时。
	 */
	uint64		(*relation_size) (Relation rel, ForkNumber forkNumber);


	/*
	 * 若关系需要 TOAST 表则本回调应返回 true，
	 * 否则 false。它可能想先查看关系的元组
	 * 描述符再做决定；但若它用其他方法
	 * 存储大值（或不支持），则可直接
	 * 返回 false
	 */
	bool		(*relation_needs_toast_table) (Relation rel);

	/*
	 * 本回调应返回为本 AM 实现 TOAST 表的表
	 * AM 的 OID。若 relation_needs_toast_table
	 * 回调总是返回 false，则不需要本回调。
	 */
	Oid			(*relation_toast_am) (Relation rel);

	/*
	 * 当对本 AM 实现的 toast 表中存放的值做
	 * detoast 时调用。见 table_relation_fetch_toast_slice()
	 * 了解细节。
	 */
	void		(*relation_fetch_toast_slice) (Relation toastrel, Oid valueid,
											   int32 attrsize,
											   int32 sliceoffset,
											   int32 slicelength,
											   struct varlena *result);


	/* ------------------------------------------------------------------------
	 * 规划器相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 见 table_relation_estimate_size()
	 *
	 * 虽面向块，但不内部用块的 AM 转换成
	 * 可用表示通常并不太难。
	 *
	 * 这与 relation_size 回调的差异在于返回供
	 * 规划用的大小估计（关系大小与元组数），
	 * 而非返回当前正确的估计。
	 */
	void		(*relation_estimate_size) (Relation rel, int32 *attr_widths,
										   BlockNumber *pages, double *tuples,
										   double *allvisfrac);


	/* ------------------------------------------------------------------------
	 * 执行器相关函数。
	 * ------------------------------------------------------------------------
	 */

	/*
	 * 准备从 `tbmres->blockno` 取/检查/返回元组，作为
	 * 位图表扫描之一。`scan` 由 table_beginscan_bm() 启动。
	 * 页上无元组则返回 false，有则 true。
	 * otherwise.
	 *
	 * 通常会读取并钉住目标块，并做必要工作以便
	 * scan_bitmap_next_tuple() 能返回元组（例如此时
	 * 做元组可见性检查就很合理）。对某些 AM，
	 * 在此完成所有引用 `tbmres` 内容的工作更合理；
	 * 对另一些则宜把更多工作推迟到
	 * scan_bitmap_next_tuple.
	 *
	 * 若 `tbmres->blockno` 为 -1，这是有损扫描，页上
	 * 全部可见元组都必须返回；否则须返回
	 * `tbmres->offsets` 处的元组。
	 *
	 * XXX: 目前仅当 AM 用 md.c 作存储管理器，且
	 * 按把 ItemPointer->ip_blkid 直接映射到底层
	 * 存储的方式使用。nodeBitmapHeapscan.c
	 * 正用该接口直接预取。日后大概
	 * 需要纠正这一点。
	 *
	 * XXX: 目前仅当 AM 使用
	 * 可见性映射时才可实现，因 nodeBitmapHeapscan.c
	 * 会无条件访问它来做预取。日后大概须纠正。
	 * point.
	 *
	 * 可选回调，但 scan_bitmap_next_block 与
	 * scan_bitmap_next_tuple 须皆有，或皆无。
	 */
	bool		(*scan_bitmap_next_block) (TableScanDesc scan,
										   struct TBMIterateResult *tbmres);

	/*
	 * 把位图表扫描的下一元组取入 `slot`，若找到
	 * 可见元组则返回 true，否则 false。
	 *
	 * 对某些 AM，在 scan_bitmap_next_block 中完成
	 * 所有引用 `tbmres` 内容的工作更合理；对另一些
	 * 则宜把更多工作推迟到本回调。
	 *
	 * 可选回调，但 scan_bitmap_next_block 与
	 * scan_bitmap_next_tuple 须皆有，或皆无。
	 */
	bool		(*scan_bitmap_next_tuple) (TableScanDesc scan,
										   struct TBMIterateResult *tbmres,
										   TupleTableSlot *slot);

	/*
	 * 准备从抽样扫描的下一块取元组。若抽样扫描
	 * 已结束则返回 false，否则 true。`scan` 由
	 * table_beginscan_sampling() 启动。
	 *
	 * 通常先调用 TsmRoutine 的 NextSampleBlock()
	 * 回调（若非 NULL）来确定目标块，否则对
	 * 全部块做顺序扫描。确定的块随后通常
	 * 读取并钉住。
	 *
	 * 因 TsmRoutine 接口面向块，须向 NextSampleBlock()
	 * 传入块号。若这对某 AM 不合适，则它须在
	 * 内部表示与基于块的表示之间
	 * 做映射。
	 *
	 * 注意：切勿持有易导致死锁的资源（如
	 * lwlocks）直到 scan_sample_next_tuple() 耗尽该
	 * 块上的元组——元组很可能返回给上层查询节点，
	 * 下一次调用可能隔很久。持有缓冲区钉住等
	 * 显然可以。
	 *
	 * 目前必须实现本接口，因为没有（与位图扫描
	 * 不同）其他办法来实现抽样扫描。若无法实现，
	 * 则 AM 可报错。
	 */
	bool		(*scan_sample_next_block) (TableScanDesc scan,
										   struct SampleScanState *scanstate);

	/*
	 * 本回调仅在 scan_sample_next_block 返回
	 * true 之后调用，应使用 TsmRoutine 的
	 * NextSampleTuple() 确定所选块的下一元组。
	 *
	 * 回调须做可见性检查，且只返回
	 * 可见元组。这显然可能须多次调用
	 * 该回调。
	 *
	 * TsmRoutine 接口假定给定页有最大偏移，
	 * 故若某 AM 不适用，则须以某种方式模拟
	 * 该假设。
	 */
	bool		(*scan_sample_next_tuple) (TableScanDesc scan,
										   struct SampleScanState *scanstate,
										   TupleTableSlot *slot);

} TableAmRoutine;


/* ----------------------------------------------------------------------------
 * 槽函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 返回适合存放该关系相应类型元组的槽回调，
 * 适用于表、视图、外表和分区
 * 表。
 */
extern const TupleTableSlotOps *table_slot_callbacks(Relation relation);

/*
 * 用 table_slot_callbacks() 返回的回调创建槽，并
 * 登记到 *reglist。
 */
extern TupleTableSlot *table_slot_create(Relation relation, List **reglist);


/* ----------------------------------------------------------------------------
 * 表扫描函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 开始扫描 `rel`。返回的元组须通过
 * `snapshot` 可见性测试；nkeys != 0 则按扫描键过滤。
 */
static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot,
				int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * 类似 table_beginscan()，但用于扫描目录。会自动使用
 * 适合扫描目录关系的快照。
 */
extern TableScanDesc table_beginscan_catalog(Relation relation, int nkeys,
											 struct ScanKeyData *key);

/*
 * 类似 table_beginscan()，但 table_beginscan_strat() 提供扩展 API，
 * 让调用方控制是否可用非默认缓冲区访问策略，
 * 以及是否可选 syncscan（可能导致
 * 扫描不从块 0 起）。二者在普通
 * plain table_beginscan.
 */
static inline TableScanDesc
table_beginscan_strat(Relation rel, Snapshot snapshot,
					  int nkeys, struct ScanKeyData *key,
					  bool allow_strat, bool allow_sync)
{
	uint32		flags = SO_TYPE_SEQSCAN | SO_ALLOW_PAGEMODE;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_bm 是建立位图堆扫描
 * TableScanDesc 的另一入口。虽该扫描技术与
 * 标准顺序扫描颇为不同，但共性仍足以
 * 值得使用同一数据结构。
 */
static inline TableScanDesc
table_beginscan_bm(Relation rel, Snapshot snapshot,
				   int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_BITMAPSCAN | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_sampling 是建立 TABLESAMPLE 扫描
 * TableScanDesc 的另一入口。与位图扫描一样，虽行为
 * 相当不同，仍值得使用同一数据结构。
 * 除 table_beginscan_strat 已有选项外，本调用
 * 还可控制是否用逐页校验可见性。
 */
static inline TableScanDesc
table_beginscan_sampling(Relation rel, Snapshot snapshot,
						 int nkeys, struct ScanKeyData *key,
						 bool allow_strat, bool allow_sync,
						 bool allow_pagemode)
{
	uint32		flags = SO_TYPE_SAMPLESCAN;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_sync)
		flags |= SO_ALLOW_SYNC;
	if (allow_pagemode)
		flags |= SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, NULL, flags);
}

/*
 * table_beginscan_tid 是建立 Tid 扫描
 * TableScanDesc 的另一入口。与位图扫描一样，虽行为
 * 相当不同，仍值得使用同一数据结构。
 */
static inline TableScanDesc
table_beginscan_tid(Relation rel, Snapshot snapshot)
{
	uint32		flags = SO_TYPE_TIDSCAN;

	return rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);
}

/*
 * table_beginscan_analyze 是建立 ANALYZE 扫描
 * TableScanDesc 的另一入口。与位图扫描一样，虽行为
 * 相当不同，仍值得使用同一数据结构。
 */
static inline TableScanDesc
table_beginscan_analyze(Relation rel)
{
	uint32		flags = SO_TYPE_ANALYZE;

	return rel->rd_tableam->scan_begin(rel, NULL, 0, NULL, NULL, flags);
}

/*
 * 结束关系扫描
 */
static inline void
table_endscan(TableScanDesc scan)
{
	scan->rs_rd->rd_tableam->scan_end(scan);
}

/*
 * 重启关系扫描。
 */
static inline void
table_rescan(TableScanDesc scan,
			 struct ScanKeyData *key)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, false, false, false, false);
}

/*
 * 在更改参数后重启关系扫描。
 *
 * 本调用允许在开始新扫描前更改缓冲区策略、
 * syncscan 与 pagemode 选项。注意实际是否使用
 * syncscan 可能改变（即启用或禁止报告），但
 * 先前选定的 startblock 会保留。
 */
static inline void
table_rescan_set_params(TableScanDesc scan, struct ScanKeyData *key,
						bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, true,
										 allow_strat, allow_sync,
										 allow_pagemode);
}

/*
 * 更新扫描所用快照。
 */
extern void table_scan_update_snapshot(TableScanDesc scan, Snapshot snapshot);

/*
 * 从 `scan` 取下一元组，存入槽。
 */
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);

	/* 不预期实际扫描使用 NoMovementScanDirection */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	/*
	 * 不预期直接调用 table_scan_getnextslot 且带有效
	 * CheckXidAlive 用于目录表或普通表。详见
	 * xact.c 中声明这些变量的注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_getnextslot call during logical decoding");

	return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}

/* ----------------------------------------------------------------------------
 * TID 范围扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * table_beginscan_tidrange 是建立 TableScanDesc 用于
 * TID 范围扫描。
 */
static inline TableScanDesc
table_beginscan_tidrange(Relation rel, Snapshot snapshot,
						 ItemPointer mintid,
						 ItemPointer maxtid)
{
	TableScanDesc sscan;
	uint32		flags = SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE;

	sscan = rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, NULL, flags);

	/* 设置扫描 TIDs 范围 */
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);

	return sscan;
}

/*
 * table_rescan_tidrange 重置扫描位置并设置最小与最大
 * TID 范围，用于由其创建的 TableScanDesc。
 * table_beginscan_tidrange.
 */
static inline void
table_rescan_tidrange(TableScanDesc sscan, ItemPointer mintid,
					  ItemPointer maxtid)
{
	/* 确保已用 table_beginscan_tidrange()。 */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	sscan->rs_rd->rd_tableam->scan_rescan(sscan, NULL, false, false, false, false);
	sscan->rs_rd->rd_tableam->scan_set_tidrange(sscan, mintid, maxtid);
}

/*
 * 从 `sscan` 取 TID 范围扫描的下一元组，该扫描由
 * table_beginscan_tidrange() 创建。元组存入 `slot`，有则 true，
 * 范围内无更多元组则返回 false。
 */
static inline bool
table_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
								TupleTableSlot *slot)
{
	/* 确保已用 table_beginscan_tidrange()。 */
	Assert((sscan->rs_flags & SO_TYPE_TIDRANGESCAN) != 0);

	/* 不预期实际扫描使用 NoMovementScanDirection */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	return sscan->rs_rd->rd_tableam->scan_getnextslot_tidrange(sscan,
															   direction,
															   slot);
}


/* ----------------------------------------------------------------------------
 * 并行表扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 估算对本关系做并行扫描所需共享内存的
 * 大小。
 */
extern Size table_parallelscan_estimate(Relation rel, Snapshot snapshot);

/*
 * 为本关系并行扫描初始化 ParallelTableScanDesc。
 * `pscan` 须按同一关系的 parallelscan_estimate() 定
 * 大小。只在 leader 进程调用一次；随后
 * 各 worker 经 table_beginscan_parallel 挂接。
 */
extern void table_parallelscan_initialize(Relation rel,
										  ParallelTableScanDesc pscan,
										  Snapshot snapshot);

/*
 * 开始并行扫描。`pscan` 须已由
 * table_parallelscan_initialize() 针对同一关系初始化。初始化
 * 不必发生在本后端。
 *
 * 调用方必须持有该关系上合适的锁。
 */
extern TableScanDesc table_beginscan_parallel(Relation relation,
											  ParallelTableScanDesc pscan);

/*
 * 重启并行扫描。在 leader 进程中调用。调用方
 * 负责确保所有 worker 已事先结束
 * 扫描。
 */
static inline void
table_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	rel->rd_tableam->parallelscan_reinitialize(rel, pscan);
}


/* ----------------------------------------------------------------------------
 * 索引扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 准备从关系取元组，供索引扫描
 * 取元组时用。
 *
 * 之后即可经 table_index_fetch_tuple() 取索引扫描的元组。
 */
static inline IndexFetchTableData *
table_index_fetch_begin(Relation rel)
{
	return rel->rd_tableam->index_fetch_begin(rel);
}

/*
 * 重置索引取数。通常释放 IndexFetchTableData
 * 中持有的资源。
 */
static inline void
table_index_fetch_reset(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_reset(scan);
}

/*
 * 释放资源并释放索引取数。
 */
static inline void
table_index_fetch_end(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_end(scan);
}

/*
 * 作为索引扫描的一部分，把 `tid` 处元组取入 `slot`，先按
 * `snapshot` 做可见性测试。若找到元组且通过
 * 可见性测试则返 true，否则 false。注意返回 true 时
 * *tid 可能被修改（见后文关于经单个索引项
 * 可达多个行版本的说明）。
 *
 * 对某 tid 首次调用 table_index_fetch_tuple() 时 *call_again 须为
 * false。若可能还有另一元组匹配该 tid，*call_again
 * 会置 true，表明应对同一 tid
 * 再调该函数。
 *
 * *all_dead 若 all_dead 非 NULL，则当且仅当保证
 * 无后端需要看见该元组时，table_index_fetch_tuple() 置 true。
 * 索引 AM 可免再返回该 tid 于后续
 * searches.
 *
 * 本函数与 table_tuple_fetch_row_version() 的差异在于：
 * 若 AM 支持经单个索引项到达多个行版本
 * （如堆的 HOT），本函数返回当前可见版本。
 * 而 table_tuple_fetch_row_version() 只
 * 评估恰好位于 `tid` 的元组。索引项到表元组
 * 查找之外，通常需要 table_tuple_fetch_row_version()。
 */
static inline bool
table_index_fetch_tuple(struct IndexFetchTableData *scan,
						ItemPointer tid,
						Snapshot snapshot,
						TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	/*
	 * 不预期直接调用 table_index_fetch_tuple 且带有效
	 * CheckXidAlive 用于目录表或普通表。详见
	 * xact.c 中声明这些变量的注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_index_fetch_tuple call during logical decoding");

	return scan->rel->rd_tableam->index_fetch_tuple(scan, tid, snapshot,
													slot, call_again,
													all_dead);
}

/*
 * 这是 table_index_fetch_tuple() 的便捷包装，返回
 * 是否存在与某索引项对应的表元组。
 * 多半只用于校验是否有冲突于
 * 唯一索引
 */
extern bool table_index_fetch_tuple_check(Relation rel,
										  ItemPointer tid,
										  Snapshot snapshot,
										  bool *all_dead);


/* ------------------------------------------------------------------------
 * 对单个元组做非修改操作的函数
 * ------------------------------------------------------------------------
 */


/*
 * 把 `tid` 处元组取入 `slot`，先按
 * `snapshot` 做可见性测试。若找到且通过则返回
 * true，否则 false。
 *
 * 二者差异见 table_index_fetch_tuple 的注释。
 * 在索引项到表元组查找之外使用本函数是
 * 正确的。
 */
static inline bool
table_tuple_fetch_row_version(Relation rel,
							  ItemPointer tid,
							  Snapshot snapshot,
							  TupleTableSlot *slot)
{
	/*
	 * 不预期直接调用 table_tuple_fetch_row_version 且带
	 * 有效 CheckXidAlive 用于目录表或普通表。详见
	 * xact.c 中声明这些变量的注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_tuple_fetch_row_version call during logical decoding");

	return rel->rd_tableam->tuple_fetch_row_version(rel, tid, snapshot, slot);
}

/*
 * 验证 `tid` 是否为潜在有效的元组标识。并不表示
 * 所指行必须存在或可见，而是以该 tid 去取行
 * （例如 table_tuple_get_latest_tid() 或
 * table_tuple_fetch_row_version()）不应报错。
 * tid.
 *
 * `scan` 须已由 table_beginscan() 启动。
 */
static inline bool
table_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return scan->rs_rd->rd_tableam->tuple_tid_valid(scan, tid);
}

/*
 * 返回 `tid` 处元组的最新版本，把 `tid` 更新为
 * 指向最新版本。
 */
extern void table_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);

/*
 * 当且仅当槽中元组满足快照时返回 true。
 *
 * 假定槽中元组有效，且类型适合该
 * AM.
 *
 * 有些 AM 可能作为副作用修改元组底层数据。若如此
 * 则应把相关缓冲区标脏。
 */
static inline bool
table_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							   Snapshot snapshot)
{
	return rel->rd_tableam->tuple_satisfies_snapshot(rel, slot, snapshot);
}

/*
 * 根据表 TID 判定哪些索引元组可安全删除。
 *
 * 判定索引 AM 调用方 TM_IndexDeleteOp 状态中哪些条目
 * 指向可清理的表元组。tableam 认定可清理的条目
 * 自然可供索引 AM 删除，因而直接标为
 * 可删。详见 TM_IndexDelete 与
 * TM_IndexDeleteOp 上方注释。
 *
 * 返回 snapshotConflictHorizon 事务 ID，供调用方写入
 * 其索引删除 WAL 记录。热备下随后 REDO 该记录时
 * 可能用到——备库上该索引删除操作或许需要
 * 一次恢复冲突。
 */
static inline TransactionId
table_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	return rel->rd_tableam->index_delete_tuples(rel, delstate);
}


/* ----------------------------------------------------------------------------
 * 物理元组操作函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 从表 AM 例程把槽中元组插入。
 *
 * options 位掩码让调用方指定可能改变 AM 行为的选项。
 * AM 会忽略其不支持的选项。
 *
 * 若指定 TABLE_INSERT_SKIP_FSM，AM 可不复用
 * 关系中的空闲空间。在已知关系为新、没有
 * 多少有用空闲空间时可省些周期。
 * TABLE_INSERT_SKIP_FSM 通常直接传给
 * RelationGetBufferForTuple。详见该方法。
 *
 * TABLE_INSERT_FROZEN 只应在插入到
 * 当前子事务中创建的关系存储，且没有
 * 先前快照或已打开的既有门户时指定。
 * 这会使行被冻结，属 MVCC 违规，
 * 须由用户显式选择选项。
 *
 * TABLE_INSERT_NO_LOGICAL 强制禁止为该元组发出逻辑解码
 * 信息。仅应在表重写期间使用，此时
 * 对新关系 RelationIsLogicallyLogged(relation) 尚不
 * 准确。
 *
 * 注意：若元组需要任何行外数据，这些选项多数也会
 * 应用到堆的 TOAST 表插入。
 *
 * BulkInsertState 对象（若有；bistate 可为 NULL 表示默认
 * 行为）也只是转交给 RelationGetBufferForTuple。若提供
 * `bistate`，则须调用 table_finish_bulk_insert()。
 *
 * 返回时槽的 tts_tid 与 tts_tableOid 会更新以反映
 * 插入。但槽内字段的任何 toast 处理并不
 * 反映到槽的内容中。
 */
static inline void
table_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				   int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->tuple_insert(rel, slot, cid, options,
								  bistate);
}

/*
 * 执行一次"推测性插入"。之后可撤销，
 * 而不中止整个事务。其他会话可等待该
 * 推测性插入被确认（变成普通元组），或
 * 被中止，如同从未存在。推测性插入的元组充当
 * 短时"值锁"，用来实现 INSERT .. ON CONFLICT。
 *
 * 已做推测性插入的事务必须要么中止，
 * 要么用下式结束推测性插入：
 * table_tuple_complete_speculative(...)。
 */
static inline void
table_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
							   CommandId cid, int options,
							   struct BulkInsertStateData *bistate,
							   uint32 specToken)
{
	rel->rd_tableam->tuple_insert_speculative(rel, slot, cid, options,
											  bistate, specToken);
}

/*
 * 完成同一事务中开始的"推测性插入"。若
 * succeeded 为 true 则元组完全插入，为 false 则移除。
 */
static inline void
table_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								 uint32 specToken, bool succeeded)
{
	rel->rd_tableam->tuple_complete_speculative(rel, slot, specToken,
												succeeded);
}

/*
 * 向表插入多个元组。
 *
 * 类似 table_tuple_insert()，但一次操作插入多个元组。
 * 往往比循环调用 table_tuple_insert() 更快，
 * 因为 AM 可减少 WAL 记录与页锁开销。
 *
 * 除了以 `nslots` 个元组及 `slots` 中 TupleTableSlots
 *  数组为输入外，table_multi_insert() 参数同于
 * table_tuple_insert().
 *
 * 注意：会向当前内存上下文泄漏内存。若这成问题，
 * 可在调用前先建一个临时上下文。
 */
static inline void
table_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				   CommandId cid, int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->multi_insert(rel, slots, nslots,
								  cid, options, bistate);
}

/*
 * 删除元组。
 *
 * 注意：除非已准备好处理并发更新，否则
 * 不要直接调用。应改用 simple_table_tuple_delete。
 *
 * 输入参数：
 *	relation - 要修改的表（调用方必须持有合适锁）
 *	tid - 要删除的元组的 TID
 *	cid - 删除命令 ID（用于可见性测试，并在
 *	成功写入 cmax）
 *	crosscheck - 若非 InvalidSnapshot，还对照此检查元组
 *	wait - true 表示应等待冲突更新提交/中止
 * 输出参数：
 *	tmfd - 失败时填入（见下）
 *	changingPart - 当且仅当元组因分区键更新移到另一分区
 *	表时为 true。否则为 false。
 *
 * 正常成功返回值为 TM_Ok，表示确实已
 * 删除。失败码为 TM_SelfModified、TM_Updated 和
 * TM_BeingModified（仅 wait == false 时可能）。
 *
 * 失败时本例程用该元组的 t_ctid、
 * t_xmax 以及可能的 t_cmax 填好 *tmfd。详见结构
 * TM_FailureData 的注释。
 */
static inline TM_Result
table_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
				   Snapshot snapshot, Snapshot crosscheck, bool wait,
				   TM_FailureData *tmfd, bool changingPart)
{
	return rel->rd_tableam->tuple_delete(rel, tid, cid,
										 snapshot, crosscheck,
										 wait, tmfd, changingPart);
}

/*
 * 更新元组。
 *
 * 注意：除非已准备好处理并发更新，否则
 * 不要直接调用。应改用 simple_table_tuple_update。
 *
 * 输入参数：
 *	relation - 要修改的表（调用方必须持有合适锁）
 *	otid - 要替换的旧元组的 TID
 *	slot - 新构造的待存元组数据
 *	cid - 更新命令 ID（用于可见性测试，并在
 *	成功写入 cmax/cmin）
 *	crosscheck - 若非 InvalidSnapshot，还对照此检查旧元组
 *	wait - true 表示应等待冲突更新提交/中止
 * 输出参数：
 *	tmfd - 失败时填入（见下）
 *	lockmode - 填入在元组上获取的锁模式
 *  update_indexes - 成功时若本元组需要新索引条目则
 *	设为 true
 *
 * 正常成功返回值为 TM_Ok，表示确实已
 * 更新。失败码为 TM_SelfModified、TM_Updated 和
 * TM_BeingModified（仅 wait == false 时可能）。
 *
 * 成功时槽的 tts_tid 与 tts_tableOid 会更新以匹配新
 * 存入的元组；特别地，slot->tts_tid 设为新元组
 * 插入处的 TID，且当且仅当做了 HOT
 * 更新时设置 HEAP_ONLY_TUPLE。但新元组数据中的
 * TOAST 变化不反映到 *newtup。
 *
 * 失败时本例程用该元组的 t_ctid、
 * t_xmax 以及可能的 t_cmax 填好 *tmfd。详见结构 TM_FailureData
 *  的注释。
 */
static inline TM_Result
table_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				   CommandId cid, Snapshot snapshot, Snapshot crosscheck,
				   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
				   TU_UpdateIndexes *update_indexes)
{
	return rel->rd_tableam->tuple_update(rel, otid, slot,
										 cid, snapshot, crosscheck,
										 wait, tmfd,
										 lockmode, update_indexes);
}

/*
 * 以指定模式锁定元组。
 *
 * 输入参数：
 *	relation：含该元组的关系（调用方必须持有合适锁）
 *	tid：要锁定的元组的 TID（若跟随更新链则会更新）
 *	snapshot：用于判定可见性的快照
 *	cid：当前命令 ID（用于可见性测试，成功则
 *	写入元组的 cmax）
 *	mode：所需锁模式
 *	wait_policy：元组锁不可用时怎么做
 *	flags:
 *	若 TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS，跟随更新链，
 *	锁模式不冲突时也锁住后代元组。
 *	若 TUPLE_LOCK_FLAG_FIND_LAST_VERSION，跟随更新链并锁住
 *	最新版本。
 *
 * 输出参数：
 *	*slot：含目标元组
 *	*tmfd：失败时填入（见下）
 *
 * 函数结果可为：
 *	TM_Ok：已成功获取锁
 *	TM_Invisible：失败，因元组对我们从未可见
 *	TM_SelfModified：失败，因元组被自身更新
 *	TM_Updated：失败，因元组被其他事务更新
 *	TM_Deleted：失败，因元组被其他事务删除
 *	TM_WouldBlock：无法获取锁且 wait_policy 为 skip
 *
 * 除 TM_Invisible 与 TM_Deleted 外的失败情形中，本例程
 * 用该元组的 t_ctid、t_xmax 以及可能的 t_cmax 填好 *tmfd。
 * 另外，成功与失败时若跟随了更新链，都会设置
 * tmfd->traversed。详见结构 TM_FailureData 的注释，以了解
 * 更多信息。
 */
static inline TM_Result
table_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	return rel->rd_tableam->tuple_lock(rel, tid, snapshot, slot,
									   cid, mode, wait_policy,
									   flags, tmfd);
}

/*
 * 完成经 tuple_insert 与 multi_insert 并指定
 * BulkInsertState 的插入所需操作。
 */
static inline void
table_finish_bulk_insert(Relation rel, int options)
{
	/* 可选回调。 */
	if (rel->rd_tableam && rel->rd_tableam->finish_bulk_insert)
		rel->rd_tableam->finish_bulk_insert(rel, options);
}


/* ------------------------------------------------------------------------
 * DDL related functionality.
 * ------------------------------------------------------------------------
 */

/*
 * Create storage for `rel` in `newrlocator`, with persistence set to
 * `persistence`.
 *
 * This is used both during relation creation and various DDL operations to
 * create new rel storage that can be filled from scratch.  When creating
 * new storage for an existing relfilelocator, this should be called before the
 * relcache entry has been updated.
 *
 * *freezeXid, *minmulti are set to the xid / multixact horizon for the table
 * that pg_class.{relfrozenxid, relminmxid} have to be set to.
 */
static inline void
table_relation_set_new_filelocator(Relation rel,
								   const RelFileLocator *newrlocator,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	rel->rd_tableam->relation_set_new_filelocator(rel, newrlocator,
												  persistence, freezeXid,
												  minmulti);
}

/*
 * Remove all table contents from `rel`, in a non-transactional manner.
 * Non-transactional meaning that there's no need to support rollbacks. This
 * commonly only is used to perform truncations for relation storage created in
 * the current transaction.
 */
static inline void
table_relation_nontransactional_truncate(Relation rel)
{
	rel->rd_tableam->relation_nontransactional_truncate(rel);
}

/*
 * Copy data from `rel` into the new relfilelocator `newrlocator`. The new
 * relfilelocator may not have storage associated before this function is
 * called. This is only supposed to be used for low level operations like
 * changing a relation's tablespace.
 */
static inline void
table_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	rel->rd_tableam->relation_copy_data(rel, newrlocator);
}

/*
 * Copy data from `OldTable` into `NewTable`, as part of a CLUSTER or VACUUM
 * FULL.
 *
 * Additional Input parameters:
 * - use_sort - if true, the table contents are sorted appropriate for
 *   `OldIndex`; if false and OldIndex is not InvalidOid, the data is copied
 *   in that index's order; if false and OldIndex is InvalidOid, no sorting is
 *   performed
 * - OldIndex - see use_sort
 * - OldestXmin - computed by vacuum_get_cutoffs(), even when
 *   not needed for the relation's AM
 * - *xid_cutoff - ditto
 * - *multi_cutoff - ditto
 *
 * Output parameters:
 * - *xid_cutoff - rel's new relfrozenxid value, may be invalid
 * - *multi_cutoff - rel's new relminmxid value, may be invalid
 * - *tups_vacuumed - stats, for logging, if appropriate for AM
 * - *tups_recently_dead - stats, for logging, if appropriate for AM
 */
static inline void
table_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								Relation OldIndex,
								bool use_sort,
								TransactionId OldestXmin,
								TransactionId *xid_cutoff,
								MultiXactId *multi_cutoff,
								double *num_tuples,
								double *tups_vacuumed,
								double *tups_recently_dead)
{
	OldTable->rd_tableam->relation_copy_for_cluster(OldTable, NewTable, OldIndex,
													use_sort, OldestXmin,
													xid_cutoff, multi_cutoff,
													num_tuples, tups_vacuumed,
													tups_recently_dead);
}

/*
 * Perform VACUUM on the relation. The VACUUM can be triggered by a user or by
 * autovacuum. The specific actions performed by the AM will depend heavily on
 * the individual AM.
 *
 * On entry a transaction needs to already been established, and the
 * table is locked with a ShareUpdateExclusive lock.
 *
 * Note that neither VACUUM FULL (and CLUSTER), nor ANALYZE go through this
 * routine, even if (for ANALYZE) it is part of the same VACUUM command.
 */
static inline void
table_relation_vacuum(Relation rel, struct VacuumParams *params,
					  BufferAccessStrategy bstrategy)
{
	rel->rd_tableam->relation_vacuum(rel, params, bstrategy);
}

/*
 * Prepare to analyze block `blockno` of `scan`. The scan needs to have been
 * started with table_beginscan_analyze().  Note that this routine might
 * acquire resources like locks that are held until
 * table_scan_analyze_next_tuple() returns false.
 *
 * Returns false if block is unsuitable for sampling, true otherwise.
 */
static inline bool
table_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
							  BufferAccessStrategy bstrategy)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_block(scan, blockno,
															bstrategy);
}

/*
 * Iterate over tuples in the block selected with
 * table_scan_analyze_next_block() (which needs to have returned true, and
 * this routine may not have returned false for the same block before). If a
 * tuple that's suitable for sampling is found, true is returned and a tuple
 * is stored in `slot`.
 *
 * *liverows and *deadrows are incremented according to the encountered
 * tuples.
 */
static inline bool
table_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							  double *liverows, double *deadrows,
							  TupleTableSlot *slot)
{
	return scan->rs_rd->rd_tableam->scan_analyze_next_tuple(scan, OldestXmin,
															liverows, deadrows,
															slot);
}

/*
 * table_index_build_scan - scan the table to find tuples to be indexed
 *
 * This is called back from an access-method-specific index build procedure
 * after the AM has done whatever setup it needs.  The parent table relation
 * is scanned to find tuples that should be entered into the index.  Each
 * such tuple is passed to the AM's callback routine, which does the right
 * things to add it to the new index.  After we return, the AM's index
 * build procedure does whatever cleanup it needs.
 *
 * The total count of live tuples is returned.  This is for updating pg_class
 * statistics.  (It's annoying not to be able to do that here, but we want to
 * merge that update with others; see index_update_stats.)  Note that the
 * index AM itself must keep track of the number of index tuples; we don't do
 * so here because the AM might reject some of the tuples for its own reasons,
 * such as being unable to store NULLs.
 *
 * If 'progress', the PROGRESS_SCAN_BLOCKS_TOTAL counter is updated when
 * starting the scan, and PROGRESS_SCAN_BLOCKS_DONE is updated as we go along.
 *
 * A side effect is to set indexInfo->ii_BrokenHotChain to true if we detect
 * any potentially broken HOT chains.  Currently, we set this if there are any
 * RECENTLY_DEAD or DELETE_IN_PROGRESS entries in a HOT chain, without trying
 * very hard to detect whether they're really incompatible with the chain tip.
 * This only really makes sense for heap AM, it might need to be generalized
 * for other AMs later.
 */
static inline double
table_index_build_scan(Relation table_rel,
					   Relation index_rel,
					   struct IndexInfo *index_info,
					   bool allow_sync,
					   bool progress,
					   IndexBuildCallback callback,
					   void *callback_state,
					   TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 false,
														 progress,
														 0,
														 InvalidBlockNumber,
														 callback,
														 callback_state,
														 scan);
}

/*
 * As table_index_build_scan(), except that instead of scanning the complete
 * table, only the given number of blocks are scanned.  Scan to end-of-rel can
 * be signaled by passing InvalidBlockNumber as numblocks.  Note that
 * restricting the range to scan cannot be done when requesting syncscan.
 *
 * When "anyvisible" mode is requested, all tuples visible to any transaction
 * are indexed and counted as live, including those inserted or deleted by
 * transactions that are still in progress.
 */
static inline double
table_index_build_range_scan(Relation table_rel,
							 Relation index_rel,
							 struct IndexInfo *index_info,
							 bool allow_sync,
							 bool anyvisible,
							 bool progress,
							 BlockNumber start_blockno,
							 BlockNumber numblocks,
							 IndexBuildCallback callback,
							 void *callback_state,
							 TableScanDesc scan)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 allow_sync,
														 anyvisible,
														 progress,
														 start_blockno,
														 numblocks,
														 callback,
														 callback_state,
														 scan);
}

/*
 * table_index_validate_scan - second table scan for concurrent index build
 *
 * See validate_index() for an explanation.
 */
static inline void
table_index_validate_scan(Relation table_rel,
						  Relation index_rel,
						  struct IndexInfo *index_info,
						  Snapshot snapshot,
						  struct ValidateIndexState *state)
{
	table_rel->rd_tableam->index_validate_scan(table_rel,
											   index_rel,
											   index_info,
											   snapshot,
											   state);
}


/* ----------------------------------------------------------------------------
 * Miscellaneous functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Return the current size of `rel` in bytes. If `forkNumber` is
 * InvalidForkNumber, return the relation's overall size, otherwise the size
 * for the indicated fork.
 *
 * Note that the overall size might not be the equivalent of the sum of sizes
 * for the individual forks for some AMs, e.g. because the AMs storage does
 * not neatly map onto the builtin types of forks.
 */
static inline uint64
table_relation_size(Relation rel, ForkNumber forkNumber)
{
	return rel->rd_tableam->relation_size(rel, forkNumber);
}

/*
 * table_relation_needs_toast_table - does this relation need a toast table?
 */
static inline bool
table_relation_needs_toast_table(Relation rel)
{
	return rel->rd_tableam->relation_needs_toast_table(rel);
}

/*
 * Return the OID of the AM that should be used to implement the TOAST table
 * for this relation.
 */
static inline Oid
table_relation_toast_am(Relation rel)
{
	return rel->rd_tableam->relation_toast_am(rel);
}

/*
 * Fetch all or part of a TOAST value from a TOAST table.
 *
 * If this AM is never used to implement a TOAST table, then this callback
 * is not needed. But, if toasted values are ever stored in a table of this
 * type, then you will need this callback.
 *
 * toastrel is the relation in which the toasted value is stored.
 *
 * valueid identifies which toast value is to be fetched. For the heap,
 * this corresponds to the values stored in the chunk_id column.
 *
 * attrsize is the total size of the toast value to be fetched.
 *
 * sliceoffset is the offset within the toast value of the first byte that
 * should be fetched.
 *
 * slicelength is the number of bytes from the toast value that should be
 * fetched.
 *
 * result is caller-allocated space into which the fetched bytes should be
 * stored.
 */
static inline void
table_relation_fetch_toast_slice(Relation toastrel, Oid valueid,
								 int32 attrsize, int32 sliceoffset,
								 int32 slicelength, struct varlena *result)
{
	toastrel->rd_tableam->relation_fetch_toast_slice(toastrel, valueid,
													 attrsize,
													 sliceoffset, slicelength,
													 result);
}


/* ----------------------------------------------------------------------------
 * Planner related functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Estimate the current size of the relation, as an AM specific workhorse for
 * estimate_rel_size(). Look there for an explanation of the parameters.
 */
static inline void
table_relation_estimate_size(Relation rel, int32 *attr_widths,
							 BlockNumber *pages, double *tuples,
							 double *allvisfrac)
{
	rel->rd_tableam->relation_estimate_size(rel, attr_widths, pages, tuples,
											allvisfrac);
}


/* ----------------------------------------------------------------------------
 * Executor related functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Prepare to fetch / check / return tuples from `tbmres->blockno` as part of
 * a bitmap table scan. `scan` needs to have been started via
 * table_beginscan_bm(). Returns false if there are no tuples to be found on
 * the page, true otherwise.
 *
 * Note, this is an optionally implemented function, therefore should only be
 * used after verifying the presence (at plan time or such).
 */
static inline bool
table_scan_bitmap_next_block(TableScanDesc scan,
							 struct TBMIterateResult *tbmres)
{
	/*
	 * We don't expect direct calls to table_scan_bitmap_next_block with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_bitmap_next_block call during logical decoding");

	return scan->rs_rd->rd_tableam->scan_bitmap_next_block(scan,
														   tbmres);
}

/*
 * Fetch the next tuple of a bitmap table scan into `slot` and return true if
 * a visible tuple was found, false otherwise.
 * table_scan_bitmap_next_block() needs to previously have selected a
 * block (i.e. returned true), and no previous
 * table_scan_bitmap_next_tuple() for the same block may have
 * returned false.
 */
static inline bool
table_scan_bitmap_next_tuple(TableScanDesc scan,
							 struct TBMIterateResult *tbmres,
							 TupleTableSlot *slot)
{
	/*
	 * We don't expect direct calls to table_scan_bitmap_next_tuple with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_bitmap_next_tuple call during logical decoding");

	return scan->rs_rd->rd_tableam->scan_bitmap_next_tuple(scan,
														   tbmres,
														   slot);
}

/*
 * Prepare to fetch tuples from the next block in a sample scan. Returns false
 * if the sample scan is finished, true otherwise. `scan` needs to have been
 * started via table_beginscan_sampling().
 *
 * This will call the TsmRoutine's NextSampleBlock() callback if necessary
 * (i.e. NextSampleBlock is not NULL), or perform a sequential scan over the
 * underlying relation.
 */
static inline bool
table_scan_sample_next_block(TableScanDesc scan,
							 struct SampleScanState *scanstate)
{
	/*
	 * We don't expect direct calls to table_scan_sample_next_block with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_block call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_block(scan, scanstate);
}

/*
 * Fetch the next sample tuple into `slot` and return true if a visible tuple
 * was found, false otherwise. table_scan_sample_next_block() needs to
 * previously have selected a block (i.e. returned true), and no previous
 * table_scan_sample_next_tuple() for the same block may have returned false.
 *
 * This will call the TsmRoutine's NextSampleTuple() callback.
 */
static inline bool
table_scan_sample_next_tuple(TableScanDesc scan,
							 struct SampleScanState *scanstate,
							 TupleTableSlot *slot)
{
	/*
	 * We don't expect direct calls to table_scan_sample_next_tuple with valid
	 * CheckXidAlive for catalog or regular tables.  See detailed comments in
	 * xact.c where these variables are declared.
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_scan_sample_next_tuple call during logical decoding");
	return scan->rs_rd->rd_tableam->scan_sample_next_tuple(scan, scanstate,
														   slot);
}


/* ----------------------------------------------------------------------------
 * Functions to make modifications a bit simpler.
 * ----------------------------------------------------------------------------
 */

extern void simple_table_tuple_insert(Relation rel, TupleTableSlot *slot);
extern void simple_table_tuple_delete(Relation rel, ItemPointer tid,
									  Snapshot snapshot);
extern void simple_table_tuple_update(Relation rel, ItemPointer otid,
									  TupleTableSlot *slot, Snapshot snapshot,
									  TU_UpdateIndexes *update_indexes);


/* ----------------------------------------------------------------------------
 * Helper functions to implement parallel scans for block oriented AMs.
 * ----------------------------------------------------------------------------
 */

extern Size table_block_parallelscan_estimate(Relation rel);
extern Size table_block_parallelscan_initialize(Relation rel,
												ParallelTableScanDesc pscan);
extern void table_block_parallelscan_reinitialize(Relation rel,
												  ParallelTableScanDesc pscan);
extern BlockNumber table_block_parallelscan_nextpage(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);
extern void table_block_parallelscan_startblock_init(Relation rel,
													 ParallelBlockTableScanWorker pbscanwork,
													 ParallelBlockTableScanDesc pbscan);


/* ----------------------------------------------------------------------------
 * Helper functions to implement relation sizing for block oriented AMs.
 * ----------------------------------------------------------------------------
 */

extern uint64 table_block_relation_size(Relation rel, ForkNumber forkNumber);
extern void table_block_relation_estimate_size(Relation rel,
											   int32 *attr_widths,
											   BlockNumber *pages,
											   double *tuples,
											   double *allvisfrac,
											   Size overhead_bytes_per_tuple,
											   Size usable_bytes_per_page);

/* ----------------------------------------------------------------------------
 * Functions in tableamapi.c
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *GetTableAmRoutine(Oid amhandler);
extern const TableAmRoutine *GetHeapamTableAmRoutine(void);

#endif							/* TABLEAM_H */
