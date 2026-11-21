/* ----------
 *	pgstat.h
 *
 *	PostgreSQL 累计统计系统的定义。
 *
 *	Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 *	src/include/pgstat.h
 * ----------
 */
#ifndef PGSTAT_H
#define PGSTAT_H

#include "datatype/timestamp.h"
#include "portability/instr_time.h"
#include "postmaster/pgarch.h"	/* 供 MAX_XFN_CHARS */
#include "utils/backend_progress.h" /* 为向后兼容 */
#include "utils/backend_status.h"	/* 为向后兼容 */
#include "utils/relcache.h"
#include "utils/wait_event.h"	/* 为向后兼容 */


/* ----------
 * 统计文件路径（相对安装的 $PGDATA）。
 * ----------
 */
#define PGSTAT_STAT_PERMANENT_DIRECTORY		"pg_stat"
#define PGSTAT_STAT_PERMANENT_FILENAME		"pg_stat/pgstat.stat"
#define PGSTAT_STAT_PERMANENT_TMPFILE		"pg_stat/pgstat.tmp"

/* 存放临时统计数据的默认目录 */
#define PG_STAT_TMP_DIR		"pg_stat_tmp"

/* 统计条目的类型 */
typedef enum PgStat_Kind
{
	/* 用 0 表 INVALID，以捕零初始化数据 */
	PGSTAT_KIND_INVALID = 0,

	/* 可变编号对象的统计 */
	PGSTAT_KIND_DATABASE,		/* 数据库级统计 */
	PGSTAT_KIND_RELATION,		/* 每表统计 */
	PGSTAT_KIND_FUNCTION,		/* 每函数统计 */
	PGSTAT_KIND_REPLSLOT,		/* 每槽统计 */
	PGSTAT_KIND_SUBSCRIPTION,	/* 每订阅统计 */

	/* 固定编号对象的统计 */
	PGSTAT_KIND_ARCHIVER,
	PGSTAT_KIND_BGWRITER,
	PGSTAT_KIND_CHECKPOINTER,
	PGSTAT_KIND_IO,
	PGSTAT_KIND_SLRU,
	PGSTAT_KIND_WAL,
} PgStat_Kind;

#define PGSTAT_KIND_FIRST_VALID PGSTAT_KIND_DATABASE
#define PGSTAT_KIND_LAST PGSTAT_KIND_WAL
#define PGSTAT_NUM_KINDS (PGSTAT_KIND_LAST + 1)

/* track_functions GUC 的取值——顺序有意义！ */
typedef enum TrackFunctionsLevel
{
	TRACK_FUNC_OFF,
	TRACK_FUNC_PL,
	TRACK_FUNC_ALL
}			TrackFunctionsLevel;

typedef enum PgStat_FetchConsistency
{
	PGSTAT_FETCH_CONSISTENCY_NONE,
	PGSTAT_FETCH_CONSISTENCY_CACHE,
	PGSTAT_FETCH_CONSISTENCY_SNAPSHOT,
} PgStat_FetchConsistency;

/* 跟踪会话终止原因的取值 */
typedef enum SessionEndType
{
	DISCONNECT_NOT_YET,			/* 仍活动 */
	DISCONNECT_NORMAL,
	DISCONNECT_CLIENT_EOF,
	DISCONNECT_FATAL,
	DISCONNECT_KILLED
} SessionEndType;

/* ----------
 * 计数器所用数据类型。
 * ----------
 */
typedef int64 PgStat_Counter;


/* ------------------------------------------------------------
 * 后端本地内存中累计计数时保留的结构
 * ------------------------------------------------------------
 */

/* ----------
 * PgStat_FunctionCounts	后端保留的每函数实际计数
 *
 * 本结构应只含实际事件计数器，因我们用 memcmp
 * 与零比较以检测是否有待刷统计。
 *
 * 注意：时间计数器此处为 instr_time。刷出待刷统计时
 * 再转为 PgStat_Counter 微秒格式。
 * ----------
 */
typedef struct PgStat_FunctionCounts
{
	PgStat_Counter numcalls;
	instr_time	total_time;
	instr_time	self_time;
} PgStat_FunctionCounts;

/*
 * 累计每次函数调用计时统计所需的工作状态。
 */
typedef struct PgStat_FunctionCallUsage
{
	/* 链到函数哈希表项（退出时仍须在！） */
	/* NULL 表示未跟踪当前函数调用 */
	PgStat_FunctionCounts *fs;
	/* 函数开始时已计入该函数的总时间 */
	instr_time	save_f_total_time;
	/* 函数开始时的后端范围总时间 */
	instr_time	save_total;
	/* 函数开始时的系统时钟 */
	instr_time	start;
} PgStat_FunctionCallUsage;

/* ----------
 * PgStat_BackendSubEntry	未刷出的订阅统计。
 * ----------
 */
typedef struct PgStat_BackendSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
} PgStat_BackendSubEntry;

/* ----------
 * PgStat_TableCounts			后端保留的每表实际计数
 *
 * 本结构应只含实际事件计数器，因我们用 memcmp
 * 与零比较以检测是否有统计更新要应用。
 * 它是 PgStat_TableStatus（后端内状态）的组件。
 *
 * 注意：对表，tuples_returned 是 heap_getnext 成功
 * 取回的元组数，而 tuples_fetched 是位图索引扫描下
 * heap_fetch 成功取回的元组数。
 * 对索引，tuples_returned 是索引 AM 返回的索引项数，
 * 而 tuples_fetched 是在本索引的简单索引扫描控制下
 * heap_fetch 成功取回的元组数。
 *
 * tuples_inserted/updated/deleted/hot_updated/newpage_updated 计尝试的
 * 动作，不论事务是否提交。delta_live_tuples、
 * delta_dead_tuples 与 changed_tuples 则依提交或中止设定。
 * 注意 delta_live_tuples 与 delta_dead_tuples 可为负！
 * ----------
 */
typedef struct PgStat_TableCounts
{
	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;
	bool		truncdropped;

	PgStat_Counter delta_live_tuples;
	PgStat_Counter delta_dead_tuples;
	PgStat_Counter changed_tuples;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
} PgStat_TableCounts;

/* ----------
 * PgStat_TableStatus			后端内的每表状态
 *
 * 许多事件计数器是非事务性的，即提交与中止事务
 * 中的事件一并计数。对这些，我们直接在
 * PgStat_TableStatus 中计数。但 delta_live_tuples、
 * delta_dead_tuples 与 changed_tuples 须由事件计数推导，
 * 并知晓事务或子事务是提交还是
 * 中止。故对当前事务中改过的每张表，还保留
 * 每（子）事务状态记录栈。在提交
 * 或中止时，把 tuples_inserted/updated/deleted 上推到
 * 父级子事务层，或写出到父 PgStat_TableStatus，
 * 视情形。
 * ----------
 */
typedef struct PgStat_TableStatus
{
	Oid			id;				/* 表的 OID */
	bool		shared;			/* 是否共享目录？ */
	struct PgStat_TableXactStatus *trans;	/* 最低子事务计数 */
	PgStat_TableCounts counts;	/* 待发送事件计数 */
	Relation	relation;		/* 使用本条目的 rel */
} PgStat_TableStatus;

/* ----------
 * PgStat_TableXactStatus		每表、每子事务状态
 * ----------
 */
typedef struct PgStat_TableXactStatus
{
	PgStat_Counter tuples_inserted; /* 子事务中插入的元组 */
	PgStat_Counter tuples_updated;	/* 子事务中更新的元组 */
	PgStat_Counter tuples_deleted;	/* 子事务中删除的元组 */
	bool		truncdropped;	/* 关系于此截断/删除
								 * 子事务 */
	/* 截断/删除前的元组 i/u/d */
	PgStat_Counter inserted_pre_truncdrop;
	PgStat_Counter updated_pre_truncdrop;
	PgStat_Counter deleted_pre_truncdrop;
	int			nest_level;		/* 子事务嵌套层级 */
	/* 链到同关系的其他结构： */
	struct PgStat_TableXactStatus *upper;	/* 上一层子事务 */
	PgStat_TableStatus *parent; /* 每表状态 */
	/* 同子事务层的结构链在此： */
	struct PgStat_TableXactStatus *next;	/* 同层下一个 */
} PgStat_TableXactStatus;


/* ------------------------------------------------------------
 * 磁盘与共享内存上的数据结构如下
 *
 * PGSTAT_FILE_FORMAT_ID 在下列变更时须改
 * 数据结构变化。
 * ------------------------------------------------------------
 */

#define PGSTAT_FILE_FORMAT_ID	0x01A5BCAC

typedef struct PgStat_ArchiverStats
{
	PgStat_Counter archived_count;	/* 归档成功次数 */
	char		last_archived_wal[MAX_XFN_CHARS + 1];	/* 末次 WAL
														 * 归档 */
	TimestampTz last_archived_timestamp;	/* 最近归档成功时间 */
	PgStat_Counter failed_count;	/* 归档失败尝试次数 */
	char		last_failed_wal[MAX_XFN_CHARS + 1]; /* 涉及的 WAL 文件
													 * 最近失败 */
	TimestampTz last_failed_timestamp;	/* 最近归档失败时间 */
	TimestampTz stat_reset_timestamp;
} PgStat_ArchiverStats;

typedef struct PgStat_BgWriterStats
{
	PgStat_Counter buf_written_clean;
	PgStat_Counter maxwritten_clean;
	PgStat_Counter buf_alloc;
	TimestampTz stat_reset_timestamp;
} PgStat_BgWriterStats;

typedef struct PgStat_CheckpointerStats
{
	PgStat_Counter timed_checkpoints;
	PgStat_Counter requested_checkpoints;
	PgStat_Counter checkpoint_write_time;	/* 时间以毫秒计 */
	PgStat_Counter checkpoint_sync_time;
	PgStat_Counter buf_written_checkpoints;
	PgStat_Counter buf_written_backend;
	PgStat_Counter buf_fsync_backend;
} PgStat_CheckpointerStats;


/*
 * 与统计 IO 操作相关的类型
 */
typedef enum IOObject
{
	IOOBJECT_RELATION,
	IOOBJECT_TEMP_RELATION,
} IOObject;

#define IOOBJECT_NUM_TYPES (IOOBJECT_TEMP_RELATION + 1)

typedef enum IOContext
{
	IOCONTEXT_BULKREAD,
	IOCONTEXT_BULKWRITE,
	IOCONTEXT_NORMAL,
	IOCONTEXT_VACUUM,
} IOContext;

#define IOCONTEXT_NUM_TYPES (IOCONTEXT_VACUUM + 1)

typedef enum IOOp
{
	IOOP_EVICT,
	IOOP_EXTEND,
	IOOP_FSYNC,
	IOOP_HIT,
	IOOP_READ,
	IOOP_REUSE,
	IOOP_WRITE,
	IOOP_WRITEBACK,
} IOOp;

#define IOOP_NUM_TYPES (IOOP_WRITEBACK + 1)

typedef struct PgStat_BktypeIO
{
	PgStat_Counter counts[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	PgStat_Counter times[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
} PgStat_BktypeIO;

typedef struct PgStat_IO
{
	TimestampTz stat_reset_timestamp;
	PgStat_BktypeIO stats[BACKEND_NUM_TYPES];
} PgStat_IO;


typedef struct PgStat_StatDBEntry
{
	PgStat_Counter xact_commit;
	PgStat_Counter xact_rollback;
	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;
	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	TimestampTz last_autovac_time;
	PgStat_Counter conflict_tablespace;
	PgStat_Counter conflict_lock;
	PgStat_Counter conflict_snapshot;
	PgStat_Counter conflict_logicalslot;
	PgStat_Counter conflict_bufferpin;
	PgStat_Counter conflict_startup_deadlock;
	PgStat_Counter temp_files;
	PgStat_Counter temp_bytes;
	PgStat_Counter deadlocks;
	PgStat_Counter checksum_failures;
	TimestampTz last_checksum_failure;
	PgStat_Counter blk_read_time;	/* 时间以微秒计 */
	PgStat_Counter blk_write_time;
	PgStat_Counter sessions;
	PgStat_Counter session_time;
	PgStat_Counter active_time;
	PgStat_Counter idle_in_transaction_time;
	PgStat_Counter sessions_abandoned;
	PgStat_Counter sessions_fatal;
	PgStat_Counter sessions_killed;

	TimestampTz stat_reset_timestamp;
} PgStat_StatDBEntry;

typedef struct PgStat_StatFuncEntry
{
	PgStat_Counter numcalls;

	PgStat_Counter total_time;	/* 时间以微秒计 */
	PgStat_Counter self_time;
} PgStat_StatFuncEntry;

typedef struct PgStat_StatReplSlotEntry
{
	PgStat_Counter spill_txns;
	PgStat_Counter spill_count;
	PgStat_Counter spill_bytes;
	PgStat_Counter stream_txns;
	PgStat_Counter stream_count;
	PgStat_Counter stream_bytes;
	PgStat_Counter total_txns;
	PgStat_Counter total_bytes;
	TimestampTz stat_reset_timestamp;
} PgStat_StatReplSlotEntry;

typedef struct PgStat_SLRUStats
{
	PgStat_Counter blocks_zeroed;
	PgStat_Counter blocks_hit;
	PgStat_Counter blocks_read;
	PgStat_Counter blocks_written;
	PgStat_Counter blocks_exists;
	PgStat_Counter flush;
	PgStat_Counter truncate;
	TimestampTz stat_reset_timestamp;
} PgStat_SLRUStats;

typedef struct PgStat_StatSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
	TimestampTz stat_reset_timestamp;
} PgStat_StatSubEntry;

typedef struct PgStat_StatTabEntry
{
	PgStat_Counter numscans;
	TimestampTz lastscan;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;

	PgStat_Counter live_tuples;
	PgStat_Counter dead_tuples;
	PgStat_Counter mod_since_analyze;
	PgStat_Counter ins_since_vacuum;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	TimestampTz last_vacuum_time;	/* 用户发起 vacuum */
	PgStat_Counter vacuum_count;
	TimestampTz last_autovacuum_time;	/* autovacuum 发起 */
	PgStat_Counter autovacuum_count;
	TimestampTz last_analyze_time;	/* 用户发起 */
	PgStat_Counter analyze_count;
	TimestampTz last_autoanalyze_time;	/* autovacuum 发起 */
	PgStat_Counter autoanalyze_count;
} PgStat_StatTabEntry;

typedef struct PgStat_WalStats
{
	PgStat_Counter wal_records;
	PgStat_Counter wal_fpi;
	uint64		wal_bytes;
	PgStat_Counter wal_buffers_full;
	PgStat_Counter wal_write;
	PgStat_Counter wal_sync;
	PgStat_Counter wal_write_time;
	PgStat_Counter wal_sync_time;
	TimestampTz stat_reset_timestamp;
} PgStat_WalStats;

/*
 * 本结构以 instr_time 存 wal 相关时长，便于
 * 累加且更便宜，无需类型
 * 转换。刷统计时再把 instr_time 转为
 * 微秒。
 */
typedef struct PgStat_PendingWalStats
{
	PgStat_Counter wal_buffers_full;
	PgStat_Counter wal_write;
	PgStat_Counter wal_sync;
	instr_time	wal_write_time;
	instr_time	wal_sync_time;
} PgStat_PendingWalStats;


/*
 * pgstat.c 中的函数
 */

/* 由 postmaster 调用的函数 */
extern Size StatsShmemSize(void);
extern void StatsShmemInit(void);

/* 服务器启动/关闭时调用的函数 */
extern void pgstat_restore_stats(void);
extern void pgstat_discard_stats(void);
extern void pgstat_before_server_shutdown(int code, Datum arg);

/* 后端初始化用函数 */
extern void pgstat_initialize(void);

/* 由后端调用的函数 */
extern long pgstat_report_stat(bool force);
extern void pgstat_force_next_flush(void);

extern void pgstat_reset_counters(void);
extern void pgstat_reset(PgStat_Kind kind, Oid dboid, Oid objoid);
extern void pgstat_reset_of_kind(PgStat_Kind kind);

/* 统计访问器 */
extern void pgstat_clear_snapshot(void);
extern TimestampTz pgstat_get_stat_snapshot_timestamp(bool *have_snapshot);

/* 辅助 */
extern PgStat_Kind pgstat_get_kind_from_str(char *kind_str);
extern bool pgstat_have_entry(PgStat_Kind kind, Oid dboid, Oid objoid);


/*
 * pgstat_archiver.c 中的函数
 */

extern void pgstat_report_archiver(const char *xlog, bool failed);
extern PgStat_ArchiverStats *pgstat_fetch_stat_archiver(void);


/*
 * pgstat_bgwriter.c 中的函数
 */

extern void pgstat_report_bgwriter(void);
extern PgStat_BgWriterStats *pgstat_fetch_stat_bgwriter(void);


/*
 * pgstat_checkpointer.c 中的函数
 */

extern void pgstat_report_checkpointer(void);
extern PgStat_CheckpointerStats *pgstat_fetch_stat_checkpointer(void);


/*
 * pgstat_io.c 中的函数
 */

extern bool pgstat_bktype_io_stats_valid(PgStat_BktypeIO *backend_io,
										 BackendType bktype);
extern void pgstat_count_io_op(IOObject io_object, IOContext io_context, IOOp io_op);
extern void pgstat_count_io_op_n(IOObject io_object, IOContext io_context, IOOp io_op, uint32 cnt);
extern instr_time pgstat_prepare_io_time(void);
extern void pgstat_count_io_op_time(IOObject io_object, IOContext io_context,
									IOOp io_op, instr_time start_time, uint32 cnt);

extern PgStat_IO *pgstat_fetch_stat_io(void);
extern const char *pgstat_get_io_context_name(IOContext io_context);
extern const char *pgstat_get_io_object_name(IOObject io_object);

extern bool pgstat_tracks_io_bktype(BackendType bktype);
extern bool pgstat_tracks_io_object(BackendType bktype,
									IOObject io_object, IOContext io_context);
extern bool pgstat_tracks_io_op(BackendType bktype, IOObject io_object,
								IOContext io_context, IOOp io_op);


/*
 * pgstat_database.c 中的函数
 */

extern void pgstat_drop_database(Oid databaseid);
extern void pgstat_report_autovac(Oid dboid);
extern void pgstat_report_recovery_conflict(int reason);
extern void pgstat_report_deadlock(void);
extern void pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount);
extern void pgstat_report_checksum_failure(void);
extern void pgstat_report_connect(Oid dboid);

#define pgstat_count_buffer_read_time(n)							\
	(pgStatBlockReadTime += (n))
#define pgstat_count_buffer_write_time(n)							\
	(pgStatBlockWriteTime += (n))
#define pgstat_count_conn_active_time(n)							\
	(pgStatActiveTime += (n))
#define pgstat_count_conn_txn_idle_time(n)							\
	(pgStatTransactionIdleTime += (n))

extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dboid);


/*
 * pgstat_function.c 中的函数
 */

extern void pgstat_create_function(Oid proid);
extern void pgstat_drop_function(Oid proid);

struct FunctionCallInfoBaseData;
extern void pgstat_init_function_usage(struct FunctionCallInfoBaseData *fcinfo,
									   PgStat_FunctionCallUsage *fcu);
extern void pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu,
									  bool finalize);

extern PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry(Oid func_id);
extern PgStat_FunctionCounts *find_funcstat_entry(Oid func_id);


/*
 * pgstat_relation.c 中的函数
 */

extern void pgstat_create_relation(Relation rel);
extern void pgstat_drop_relation(Relation rel);
extern void pgstat_copy_relation_stats(Relation dst, Relation src);

extern void pgstat_init_relation(Relation rel);
extern void pgstat_assoc_relation(Relation rel);
extern void pgstat_unlink_relation(Relation rel);

extern void pgstat_report_vacuum(Oid tableoid, bool shared,
								 PgStat_Counter livetuples, PgStat_Counter deadtuples);
extern void pgstat_report_analyze(Relation rel,
								  PgStat_Counter livetuples, PgStat_Counter deadtuples,
								  bool resetcounter);

/*
 * 若已开统计但待刷数据尚未准备，则调用
 * pgstat_assoc_relation() 来准备。见其注释说明为何与
 * pgstat_init_relation() 分开。
 */
#define pgstat_should_count_relation(rel)                           \
	(likely((rel)->pgstat_info != NULL) ? true :                    \
	 ((rel)->pgstat_enabled ? pgstat_assoc_relation(rel), true : false))

/* 非事务事件计数足够简单，可内联 */

#define pgstat_count_heap_scan(rel)									\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_heap_getnext(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned++;			\
	} while (0)
#define pgstat_count_heap_fetch(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_fetched++;			\
	} while (0)
#define pgstat_count_index_scan(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_index_tuples(rel, n)							\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned += (n);		\
	} while (0)
#define pgstat_count_buffer_read(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_fetched++;			\
	} while (0)
#define pgstat_count_buffer_hit(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_hit++;				\
	} while (0)

extern void pgstat_count_heap_insert(Relation rel, PgStat_Counter n);
extern void pgstat_count_heap_update(Relation rel, bool hot, bool newpage);
extern void pgstat_count_heap_delete(Relation rel);
extern void pgstat_count_truncate(Relation rel);
extern void pgstat_update_heap_dead_tuples(Relation rel, int delta);

extern void pgstat_twophase_postcommit(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void pgstat_twophase_postabort(TransactionId xid, uint16 info,
									  void *recdata, uint32 len);

extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry_ext(bool shared,
														   Oid reloid);
extern PgStat_TableStatus *find_tabstat_entry(Oid rel_id);


/*
 * pgstat_replslot.c 中的函数
 */

extern void pgstat_reset_replslot(const char *name);
struct ReplicationSlot;
extern void pgstat_report_replslot(struct ReplicationSlot *slot, const PgStat_StatReplSlotEntry *repSlotStat);
extern void pgstat_create_replslot(struct ReplicationSlot *slot);
extern void pgstat_acquire_replslot(struct ReplicationSlot *slot);
extern void pgstat_drop_replslot(struct ReplicationSlot *slot);
extern PgStat_StatReplSlotEntry *pgstat_fetch_replslot(NameData slotname);


/*
 * pgstat_slru.c 中的函数
 */

extern void pgstat_reset_slru(const char *);
extern void pgstat_count_slru_page_zeroed(int slru_idx);
extern void pgstat_count_slru_page_hit(int slru_idx);
extern void pgstat_count_slru_page_read(int slru_idx);
extern void pgstat_count_slru_page_written(int slru_idx);
extern void pgstat_count_slru_page_exists(int slru_idx);
extern void pgstat_count_slru_flush(int slru_idx);
extern void pgstat_count_slru_truncate(int slru_idx);
extern const char *pgstat_get_slru_name(int slru_idx);
extern int	pgstat_get_slru_index(const char *name);
extern PgStat_SLRUStats *pgstat_fetch_slru(void);


/*
 * pgstat_subscription.c 中的函数
 */

extern void pgstat_report_subscription_error(Oid subid, bool is_apply_error);
extern void pgstat_create_subscription(Oid subid);
extern void pgstat_drop_subscription(Oid subid);
extern PgStat_StatSubEntry *pgstat_fetch_stat_subscription(Oid subid);


/*
 * pgstat_xact.c 中的函数
 */

extern void AtEOXact_PgStat(bool isCommit, bool parallel);
extern void AtEOSubXact_PgStat(bool isCommit, int nestDepth);
extern void AtPrepare_PgStat(void);
extern void PostPrepare_PgStat(void);
struct xl_xact_stats_item;
extern int	pgstat_get_transactional_drops(bool isCommit, struct xl_xact_stats_item **items);
extern void pgstat_execute_transactional_drops(int ndrops, struct xl_xact_stats_item *items, bool is_redo);


/*
 * pgstat_wal.c 中的函数
 */

extern void pgstat_report_wal(bool force);
extern PgStat_WalStats *pgstat_fetch_stat_wal(void);


/*
 * pgstat.c 中的变量
 */

/* GUC 参数 */
extern PGDLLIMPORT bool pgstat_track_counts;
extern PGDLLIMPORT int pgstat_track_functions;
extern PGDLLIMPORT int pgstat_fetch_consistency;


/*
 * pgstat_bgwriter.c 中的变量
 */

/* 由 bgwriter 与 bufmgr 直接更新 */
extern PGDLLIMPORT PgStat_BgWriterStats PendingBgWriterStats;


/*
 * pgstat_checkpointer.c 中的变量
 */

/*
 * 检查点统计由检查点进程与 bufmgr 更新
 *
 */
extern PGDLLIMPORT PgStat_CheckpointerStats PendingCheckpointerStats;


/*
 * pgstat_database.c 中的变量
 */

/* 由 pgstat_count_buffer_*_time 宏更新 */
extern PGDLLIMPORT PgStat_Counter pgStatBlockReadTime;
extern PGDLLIMPORT PgStat_Counter pgStatBlockWriteTime;

/*
 * Updated by pgstat_count_conn_*_time macros, called by
 * pgstat_report_activity().
 */
extern PGDLLIMPORT PgStat_Counter pgStatActiveTime;
extern PGDLLIMPORT PgStat_Counter pgStatTransactionIdleTime;

/* 由流量控制器与 errfinish() 更新 */
extern PGDLLIMPORT SessionEndType pgStatSessionEndCause;


/*
 * pgstat_wal.c 中的变量
 */

/* 由后端与后台进程直接更新 */
extern PGDLLIMPORT PgStat_PendingWalStats PendingWalStats;


#endif							/* PGSTAT_H */
