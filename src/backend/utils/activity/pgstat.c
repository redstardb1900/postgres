/* ----------
 * pgstat.c
 *	  累计统计系统的基础设施。
 *
 * 累计统计系统为不同种类的对象累积统计。某
 * 些种类的统计针对固定数量的对象（最常见为
 * 1）收集，例如检查点进程统计。另一些种类的
 * 统计则针对数量可变的对象收集
 * （例如关系）。当前处理的统计种类见 PgStat_KindInfo
 * 
 *
 * 统计在启动期间（由启动进程）从文件系统加
 * 载，除非之前发生过崩溃，那时所有统计都会
 * 被丢弃。统计由检查点进程在即将关闭之前写
 * 出，立即模式关闭时除外。
 *
 * 固定编号统计存储在普通（非动态）共享内存中。
 *
 * 可变编号对象的统计存储在动态共享内存中，
 * 可以通过 dshash 哈希表找到。统计计数器并不
 * 直接属于 dshash 条目（PgStatShared_HashEntry），而是
 * 单独分配（PgStatShared_HashEntry->body）。这
 * 种分开的分配让不同种类的统计能存放在同一
 * 个哈希表，不浪费 PgStatShared_HashEntry 空间。
 *
 * 可变编号统计在运行期间以 PgStat_HashKey 寻址。
 * 不可能为无法这样寻址的对象保存统计。
 * 序列化到磁盘时可以使用更宽的标识符（用
 * 于复制槽统计）。
 *
 * 为避免共享哈希表上的争用，每个后端在共享
 * 哈希表前面都有一个后端本地哈希表（
 * pgStatEntryRefHash），其中包含对共享哈希表条目
 * 的引用（PgStat_EntryRef）。只有当本地哈希表中
 * 找不到先前引用时才需要访问共享哈希表。除
 * 了指向共享哈希表条目（PgStatShared_HashEntry）
 * 外，PgStat_EntryRef 还包含指向共享统计数据的进程
 * 本地地址的指针。
 *
 * 存储在共享内存中的结构体名称都以 PgStatShared
 * 为前缀而非 PgStat。共享内存中的每个统计条目都
 * 由专用的 lwlock 保护。
 *
 * 大多数统计更新首先作为待写入条目在各进程
 * 中本地累积，之后再刷写到共享内存（就在提
 * 交之后，或由空闲超时触发）。这实际上消除
 * 了单个统计条目上的争用。对大多数种类的可
 * 变编号统计的待写入数据存于 PgStat_EntryRef->pending。
 * 所有待写入数据的条目都在 pgStatPending 列表，由
 * pgstat_report_stat()
 *
 * 不同种类统计的行为由该种类在 pgstat_kind_infos
 * 中的条目决定，详情见 PgStat_KindInfo。
 *
 * 统计读取访问的一致性可以用 stats_fetch_consistency
 * GUC 来配置（详见 config.sgml 与 monitoring.sgml
 * 设置）。当用 PGSTAT_FETCH_CONSISTENCY_CACHE 或
 * PGSTAT_FETCH_CONSISTENCY_SNAPSHOT 时统计存于
 * pgStatLocal.snapshot
 *
 * 为了保持可控性，统计处理被拆分到几个
 * 文件。基础设施部分位于：
 * - pgstat.c - 本文件，负责统筹全局
 * - pgstat_shmem.c - 几乎所有涉及共享内存的事务，包括
 *     哈希表条目的维护
 * - pgstat_xact.c - 事务集成，包括统计条目的事务性
 *     创建与删除
 *
 * 每种统计都在专门文件中处理：
 * - pgstat_archiver.c
 * - pgstat_bgwriter.c
 * - pgstat_checkpointer.c
 * - pgstat_database.c
 * - pgstat_function.c
 * - pgstat_io.c
 * - pgstat_relation.c
 * - pgstat_replslot.c
 * - pgstat_slru.c
 * - pgstat_subscription.c
 * - pgstat_wal.c
 *
 * 基础设施文件应尽可能不包含与特定统计种类
 * 相关的代码。
 *
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat.c
 * ----------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "lib/dshash.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/* ----------
 * 定时器定义。
 *
 * 单位毫秒。
 * ----------
 */

/* 非强制统计刷写的最小间隔。 */
#define PGSTAT_MIN_INTERVAL			1000
/* 过多久之后会阻塞待写入统计更新的刷写 */
#define PGSTAT_MAX_INTERVAL			60000
/* 何时再次调用 pgstat_report_stat()，即使空闲 */
#define PGSTAT_IDLE_INTERVAL		10000

/* ----------
 * 统计所用哈希表的初始大小提示。
 * ----------
 */

#define PGSTAT_SNAPSHOT_HASH_SIZE	512


/* 统计快照条目的哈希表 */
typedef struct PgStat_SnapshotEntry
{
	PgStat_HashKey key;
	char		status;			/* 供 simplehash 用 */
	void	   *data;			/* 统计数据本身 */
} PgStat_SnapshotEntry;


/* ----------
 * 后端本地哈希表定义
 * ----------
 */

/* 统计快照条目用 */
#define SH_PREFIX pgstat_snapshot
#define SH_ELEMENT_TYPE PgStat_SnapshotEntry
#define SH_KEY_TYPE PgStat_HashKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	pgstat_hash_hash_key(&key, sizeof(PgStat_HashKey), NULL)
#define SH_EQUAL(tb, a, b) \
	pgstat_cmp_hash_key(&a, &b, sizeof(PgStat_HashKey), NULL) == 0
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"


/* ----------
 * 局部函数前置声明
 * ----------
 */

static void pgstat_write_statsfile(void);
static void pgstat_read_statsfile(void);

static void pgstat_reset_after_failure(void);

static bool pgstat_flush_pending_entries(bool nowait);

static void pgstat_prep_snapshot(void);
static void pgstat_build_snapshot(void);
static void pgstat_build_snapshot_fixed(PgStat_Kind kind);

static inline bool pgstat_is_kind_valid(int ikind);


/* ----------
 * GUC 参数
 * ----------
 */

bool		pgstat_track_counts = false;
int			pgstat_fetch_consistency = PGSTAT_FETCH_CONSISTENCY_CACHE;


/* ----------
 * 与 pgstat_*.c 共享状态
 * ----------
 */

PgStat_LocalState pgStatLocal;


/* ----------
 * Local data
 *
 * 注：此处应只放置与统计基础设施相关的变量，
 * 而非特定种类的变量。
 * ----------
 */

/*
 * 包含 pgStatEntryRefHash 表、pgStatSharedRef
 * 条目以及待写入数据的各内存上下文。这主要是
 * 便于追踪 / 归属内存用量。
 */

static MemoryContext pgStatPendingContext = NULL;

/*
 * 后端本地的未刷写待写入统计 PgStat_EntryRef 列表。
 *
 * 新产生的待写入条目只应添加到列表末尾，
 * 否则 pgstat_flush_pending_entries() 可能无法立即看到它们。
 */
static dlist_head pgStatPending = DLIST_STATIC_INIT(pgStatPending);


/*
 * 使下一次统计刷写无视
 * PGSTAT_MIN_INTERVAL。测试脚本有用。
 */
static bool pgStatForceNextFlush = false;

/*
 * stats_fetch_consistency 变更后下次使用前须清空现有快照。
 * 
 */
static bool force_stats_snapshot_clear = false;


/*
 * 供断言使用：检查 pgstat 未在初始化之前 / 关闭
 * 之后。
 */
#ifdef USE_ASSERT_CHECKING
static bool pgstat_is_initialized = false;
static bool pgstat_is_shutdown = false;
#endif


/*
 * 各种不同的统计种类。
 *
 * 若合理可行，针对某一种统计的特定处理应尽
 * 量经过此抽象，而不是让 pgstat.c 的更多部分感知。
 *
 * 各字段的详情见 PgStat_KindInfo 结构体的注释。
 * 
 *
 * XXX：把它定义在本文件之外会更好。但目前似
 * 乎没有好办法做到，因为代码分散在多个文件。
 * 
 */
static const PgStat_KindInfo pgstat_kind_infos[PGSTAT_NUM_KINDS] = {

	/* 可变编号对象的统计种类 */

	[PGSTAT_KIND_DATABASE] = {
		.name = "database",

		.fixed_amount = false,
		/* 使 pg_stat_database 条目在所有数据库可见 */
		.accessed_across_databases = true,

		.shared_size = sizeof(PgStatShared_Database),
		.shared_data_off = offsetof(PgStatShared_Database, stats),
		.shared_data_len = sizeof(((PgStatShared_Database *) 0)->stats),
		.pending_size = sizeof(PgStat_StatDBEntry),

		.flush_pending_cb = pgstat_database_flush_cb,
		.reset_timestamp_cb = pgstat_database_reset_timestamp_cb,
	},

	[PGSTAT_KIND_RELATION] = {
		.name = "relation",

		.fixed_amount = false,

		.shared_size = sizeof(PgStatShared_Relation),
		.shared_data_off = offsetof(PgStatShared_Relation, stats),
		.shared_data_len = sizeof(((PgStatShared_Relation *) 0)->stats),
		.pending_size = sizeof(PgStat_TableStatus),

		.flush_pending_cb = pgstat_relation_flush_cb,
		.delete_pending_cb = pgstat_relation_delete_pending_cb,
	},

	[PGSTAT_KIND_FUNCTION] = {
		.name = "function",

		.fixed_amount = false,

		.shared_size = sizeof(PgStatShared_Function),
		.shared_data_off = offsetof(PgStatShared_Function, stats),
		.shared_data_len = sizeof(((PgStatShared_Function *) 0)->stats),
		.pending_size = sizeof(PgStat_FunctionCounts),

		.flush_pending_cb = pgstat_function_flush_cb,
	},

	[PGSTAT_KIND_REPLSLOT] = {
		.name = "replslot",

		.fixed_amount = false,

		.accessed_across_databases = true,
		.named_on_disk = true,

		.shared_size = sizeof(PgStatShared_ReplSlot),
		.shared_data_off = offsetof(PgStatShared_ReplSlot, stats),
		.shared_data_len = sizeof(((PgStatShared_ReplSlot *) 0)->stats),

		.reset_timestamp_cb = pgstat_replslot_reset_timestamp_cb,
		.to_serialized_name = pgstat_replslot_to_serialized_name_cb,
		.from_serialized_name = pgstat_replslot_from_serialized_name_cb,
	},

	[PGSTAT_KIND_SUBSCRIPTION] = {
		.name = "subscription",

		.fixed_amount = false,
		/* 使 pg_stat_subscription_stats 条目在所有数据库可见 */
		.accessed_across_databases = true,

		.shared_size = sizeof(PgStatShared_Subscription),
		.shared_data_off = offsetof(PgStatShared_Subscription, stats),
		.shared_data_len = sizeof(((PgStatShared_Subscription *) 0)->stats),
		.pending_size = sizeof(PgStat_BackendSubEntry),

		.flush_pending_cb = pgstat_subscription_flush_cb,
		.reset_timestamp_cb = pgstat_subscription_reset_timestamp_cb,
	},


	/* 固定编号（多为 1）对象的统计 */

	[PGSTAT_KIND_ARCHIVER] = {
		.name = "archiver",

		.fixed_amount = true,

		.reset_all_cb = pgstat_archiver_reset_all_cb,
		.snapshot_cb = pgstat_archiver_snapshot_cb,
	},

	[PGSTAT_KIND_BGWRITER] = {
		.name = "bgwriter",

		.fixed_amount = true,

		.reset_all_cb = pgstat_bgwriter_reset_all_cb,
		.snapshot_cb = pgstat_bgwriter_snapshot_cb,
	},

	[PGSTAT_KIND_CHECKPOINTER] = {
		.name = "checkpointer",

		.fixed_amount = true,

		.reset_all_cb = pgstat_checkpointer_reset_all_cb,
		.snapshot_cb = pgstat_checkpointer_snapshot_cb,
	},

	[PGSTAT_KIND_IO] = {
		.name = "io",

		.fixed_amount = true,

		.reset_all_cb = pgstat_io_reset_all_cb,
		.snapshot_cb = pgstat_io_snapshot_cb,
	},

	[PGSTAT_KIND_SLRU] = {
		.name = "slru",

		.fixed_amount = true,

		.reset_all_cb = pgstat_slru_reset_all_cb,
		.snapshot_cb = pgstat_slru_snapshot_cb,
	},

	[PGSTAT_KIND_WAL] = {
		.name = "wal",

		.fixed_amount = true,

		.reset_all_cb = pgstat_wal_reset_all_cb,
		.snapshot_cb = pgstat_wal_snapshot_cb,
	},
};


/* ------------------------------------------------------------
 * 管理所有后端的统计系统状态的函数。
 * ------------------------------------------------------------
 */

/*
 * 服务器启动时读入磁盘统计。
 *
 * 只应由启动进程或在单用户模式下调用。
 */
void
pgstat_restore_stats(void)
{
	pgstat_read_statsfile();
}

/*
 * 删除统计文件。目前仅在崩溃后需要 WAL 恢复时
 * 才会使用。
 *
 * 只应由启动进程或在单用户模式下调用。
 */
void
pgstat_discard_stats(void)
{
	int			ret;

	/* 注：单用户模式下也必须这样做 */

	ret = unlink(PGSTAT_STAT_PERMANENT_FILENAME);
	if (ret != 0)
	{
		if (errno == ENOENT)
			elog(DEBUG2,
				 "didn't need to unlink permanent stats file \"%s\" - didn't exist",
				 PGSTAT_STAT_PERMANENT_FILENAME);
		else
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink permanent statistics file \"%s\": %m",
							PGSTAT_STAT_PERMANENT_FILENAME)));
	}
	else
	{
		ereport(DEBUG2,
				(errcode_for_file_access(),
				 errmsg_internal("unlinked permanent statistics file \"%s\"",
								 PGSTAT_STAT_PERMANENT_FILENAME)));
	}

	/*
	 * 重置统计内容。这会把固定编号统计的重置时
	 * 间戳设为当前时间（无可变统计）。
	 */
	pgstat_reset_after_failure();
}

/*
 * pgstat_before_server_shutdown() 需要由恰好一个进
 * 程在常规服务器关闭期间调用。
 *
 * 否则统计全部丢失。目前只在 proc_exit(0) 时写出统计。
 * 我们以后也许想改变这一点……但现在 pgstat_discard_stats()
 * 反正会在无序关闭后的启动中被调用。
 */
void
pgstat_before_server_shutdown(int code, Datum arg)
{
	Assert(pgStatLocal.shmem != NULL);
	Assert(!pgStatLocal.shmem->is_shutdown);

	/*
	 * 统计只应在 pgstat_initialize() 之后、
	 * pgstat_shutdown() 之前上报。此处便于捕获多数
	 * 违规。
	 */
	Assert(pgstat_is_initialized && !pgstat_is_shutdown);

	/* 在写出之前先刷写自己的待写入变更 */
	pgstat_report_stat(true);

	/*
	 * 仅在正常关闭时写出文件。甚至在非正常关闭
	 * 时也不发出已关闭的信号，因为关闭序列并没
	 * 有被协调为确保此后端最后关闭。
	 */
	if (code == 0)
	{
		pgStatLocal.shmem->is_shutdown = true;
		pgstat_write_statsfile();
	}
}


/* ------------------------------------------------------------
 * 后端初始化 / 关闭函数
 * ------------------------------------------------------------
 */

/*
 * 在进程退出时关闭单个后端的统计上报。
 *
 * 刷写出所有残留的统计计数。否则，在后端退
 * 出期间触发的操作（例如临时表删除）将不计
 * 入。
 */
static void
pgstat_shutdown_hook(int code, Datum arg)
{
	Assert(!pgstat_is_shutdown);
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	/*
	 * 如果我们已经确认了自己的数据库 ID，就可以
	 * 刷写出目前为止所做的。否则，我们将上报无
	 * 效的数据库 ID，只能作罢。（这意味着失败的
	 * 对 pg_database 的访问可能不被计数。
	 */
	if (OidIsValid(MyDatabaseId))
		pgstat_report_disconnect(MyDatabaseId);

	pgstat_report_stat(true);

	/* 不应再残留任何待写入变更 */
	Assert(dlist_is_empty(&pgStatPending));
	dlist_init(&pgStatPending);

	pgstat_detach_shmem();

#ifdef USE_ASSERT_CHECKING
	pgstat_is_shutdown = true;
#endif
}

/*
 * 初始化 pgstats 状态，并建立进程退出钩子。调用自
 * BaseInit()
 *
 * 注：MyDatabaseId 尚未设置；因此关闭钩子必须小心。
 */
void
pgstat_initialize(void)
{
	Assert(!pgstat_is_initialized);

	pgstat_attach_shmem();

	pgstat_init_wal();

	/* 设置进程退出清理钩子 */
	before_shmem_exit(pgstat_shutdown_hook, 0);

#ifdef USE_ASSERT_CHECKING
	pgstat_is_initialized = true;
#endif
}


/* ------------------------------------------------------------
 * 以下为后端使用的公共函数
 * ------------------------------------------------------------
 */

/*
 * 必须由执行 DML 的进程调用：tcop/postgres.c、逻辑
 * 接收进程、SPI 工作者等，将待写入统计更新刷入
 * 共享内存
 *
 * 除非以'force'调用，待写入统计更新每
 * PGSTAT_MIN_INTERVAL（1000ms）刷写一次。非强制
 * 时统计刷写不会阻塞等待获得锁，除非统计更
 * 新待写入超过 PGSTAT_MAX_INTERVAL
 *
 * 每当 pgstat_report_stat() 结束时还有待写入统计
 * 更新，则会返回建议的空闲超时。目前该值总
 * 是 PGSTAT_IDLE_INTERVAL（10000ms）。调用方可
 * 据此设置超时，过后调用 pgstat_report_stat(true)
 * 但非必须。
 *
 * 注意本函数只在事务外被调用，所以用事务停
 * 止时间近似当前时间是合理的。
 */
long
pgstat_report_stat(bool force)
{
	static TimestampTz pending_since = 0;
	static TimestampTz last_flush = 0;
	bool		partial_flush;
	TimestampTz now;
	bool		nowait;

	pgstat_assert_is_up();
	Assert(!IsTransactionOrTransactionBlock());

	/* "吸收"强制刷写，即使没有可刷写的内容 */
	if (pgStatForceNextFlush)
	{
		force = true;
		pgStatForceNextFlush = false;
	}

	/* 没事可做就不耗费时钟检查 */
	if (dlist_is_empty(&pgStatPending) &&
		!have_iostats &&
		!have_slrustats &&
		!pgstat_have_pending_wal())
	{
		return 0;
	}

	/*
	 * 统计一旦关闭就绝不应再有统计可上报。
	 * 在上述检查之前无法断言这一点，因为
	 * pgstat_shutdown_hook() 中有一次无条件的 pgstat_report_stat()
	 * 调用，运行过 pgstat_before_server_shutdown() 者仍会调用。
	 */
	Assert(!pgStatLocal.shmem->is_shutdown);

	if (force)
	{
		/*
		 * 强制统计上报的情形：要么距离上次上报统计
		 * 已有很长时间，要么处于强制在特定时点进行
		 * 统计上报的进程中（包括关闭时）。前一种情
		 * 况下事务停止时间可能相当早，后一种情况
		 * 则永远不会清除。
		 */
		now = GetCurrentTimestamp();
	}
	else
	{
		now = GetCurrentTransactionStopTimestamp();

		if (pending_since > 0 &&
			TimestampDifferenceExceeds(pending_since, now, PGSTAT_MAX_INTERVAL))
		{
			/* 待写入更新不要保留超过 PGSTAT_MAX_INTERVAL */
			force = true;
		}
		else if (last_flush > 0 &&
				 !TimestampDifferenceExceeds(last_flush, now, PGSTAT_MIN_INTERVAL))
		{
			/* 不要过于频繁刷写 */
			if (pending_since == 0)
				pending_since = now;

			return PGSTAT_IDLE_INTERVAL;
		}
	}

	pgstat_update_dbstats(now);

	/* 非 force 时不等待获取锁 */
	nowait = !force;

	partial_flush = false;

	/* 刷写数据库 / 关系 / 函数 / ... 统计 */
	partial_flush |= pgstat_flush_pending_entries(nowait);

	/* 刷写IO统计 */
	partial_flush |= pgstat_flush_io(nowait);

	/* 刷写WAL统计 */
	partial_flush |= pgstat_flush_wal(nowait);

	/* 刷写SLRU统计 */
	partial_flush |= pgstat_slru_flush(nowait);

	last_flush = now;

	/*
	 * 若部分待写入统计因锁争用而无法刷写，
	 * 请告知调用方何时重试。
	 */
	if (partial_flush)
	{
		/* force 本应防止我们到达此处 */
		Assert(!force);

		/* 记住统计自何时起待写入 */
		if (pending_since == 0)
			pending_since = now;

		return PGSTAT_IDLE_INTERVAL;
	}

	pending_since = 0;

	return 0;
}

/*
 * 强制把本地待写入的统计放在下次
 * pgstat_report_stat() 调用时进行。便于编写测试。
 */
void
pgstat_force_next_flush(void)
{
	pgStatForceNextFlush = true;
}

/*
 * 仅供 pgstat_reset_counters() 使用
 */
static bool
match_db_entries(PgStatShared_HashEntry *entry, Datum match_data)
{
	return entry->key.dboid == DatumGetObjectId(MyDatabaseId);
}

/*
 * 重置本库的计数器。
 *
 * 本函数的权限检查由常规 GRANT 系统管
 * 理。
 */
void
pgstat_reset_counters(void)
{
	TimestampTz ts = GetCurrentTimestamp();

	pgstat_reset_matching_entries(match_db_entries,
								  ObjectIdGetDatum(MyDatabaseId),
								  ts);
}

/*
 * 重置单个可变编号条目。
 *
 * 若该统计种类位于某数据库内，还同时重置
 * stat_reset_timestamp
 *
 * 本函数的权限检查由常规 GRANT 系统管
 * 理。
 */
void
pgstat_reset(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	TimestampTz ts = GetCurrentTimestamp();

	/* 目前不需要，且对当前签名而言也没有意义 */
	Assert(!pgstat_get_kind_info(kind)->fixed_amount);

	/* 重置"单一计数器" */
	pgstat_reset_entry(kind, dboid, objoid, ts);

	if (!kind_info->accessed_across_databases)
		pgstat_reset_database_timestamp(dboid, ts);
}

/*
 * 重置某类所有条目的统计。
 *
 * 本函数的权限检查由常规 GRANT 系统管
 * 理。
 */
void
pgstat_reset_of_kind(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	TimestampTz ts = GetCurrentTimestamp();

	if (kind_info->fixed_amount)
		kind_info->reset_all_cb(ts);
	else
		pgstat_reset_entries_of_kind(kind, ts);
}


/* ------------------------------------------------------------
 * 统计的获取
 * ------------------------------------------------------------
 */

/*
 * 丢弃当前事务中收集的任何数据。后续的任何
 * 请求都会导致重新读取新快照。
 *
 * 本函数也会在事务提交或中止期间被调用，以
 * 丢弃不再需要的快照。更改 stats_fetch_consistency
 * 也会令本例程被调用。
 */
void
pgstat_clear_snapshot(void)
{
	pgstat_assert_is_up();

	memset(&pgStatLocal.snapshot.fixed_valid, 0,
		   sizeof(pgStatLocal.snapshot.fixed_valid));
	pgStatLocal.snapshot.stats = NULL;
	pgStatLocal.snapshot.mode = PGSTAT_FETCH_CONSISTENCY_NONE;

	/* 若分配过内存则将其释放 */
	if (pgStatLocal.snapshot.context)
	{
		MemoryContextDelete(pgStatLocal.snapshot.context);

		/* 重置变量 */
		pgStatLocal.snapshot.context = NULL;
	}

	/*
	 * 历史上 backend_status.c 的设施位于本文件中，并且
	 * 曾用同一函数重置。目前保持原状，并转发重
	 * 置请求。
	 */
	pgstat_clear_backend_activity_snapshot();

	/* 重置此标志，因为可能发生过强制清理。 */
	force_stats_snapshot_clear = false;
}

void *
pgstat_fetch_entry(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	PgStat_HashKey key;
	PgStat_EntryRef *entry_ref;
	void	   *stats_data;
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

	/* 应从后端调用 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);
	Assert(!kind_info->fixed_amount);

	pgstat_prep_snapshot();

	/* 清零填充 */
	memset(&key, 0, sizeof(struct PgStat_HashKey));

	key.kind = kind;
	key.dboid = dboid;
	key.objoid = objoid;

	/* 若需要构建完整快照，就这么做 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		pgstat_build_snapshot();

	/* 若希望缓存，则在缓存中查找 */
	if (pgstat_fetch_consistency > PGSTAT_FETCH_CONSISTENCY_NONE)
	{
		PgStat_SnapshotEntry *entry = NULL;

		entry = pgstat_snapshot_lookup(pgStatLocal.snapshot.stats, key);

		if (entry)
			return entry->data;

		/*
		 * 若我们构建了完整快照且该键不在
		 * pgStatLocal.snapshot.stats 中则无匹配统计。
		 */
		if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
			return NULL;
	}

	pgStatLocal.snapshot.mode = pgstat_fetch_consistency;

	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, false, NULL);

	if (entry_ref == NULL || entry_ref->shared_entry->dropped)
	{
		/* 使用 PGSTAT_FETCH_CONSISTENCY_CACHE 时创建空条目 */
		if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_CACHE)
		{
			PgStat_SnapshotEntry *entry = NULL;
			bool		found;

			entry = pgstat_snapshot_insert(pgStatLocal.snapshot.stats, key, &found);
			Assert(!found);
			entry->data = NULL;
		}
		return NULL;
	}

	/*
	 * 对 PGSTAT_FETCH_CONSISTENCY_NONE 在调用者的上
	 * 文分配，否则反复访问可能很快耗用大量
	 * 内存。
	 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_NONE)
		stats_data = palloc(kind_info->shared_data_len);
	else
		stats_data = MemoryContextAlloc(pgStatLocal.snapshot.context,
										kind_info->shared_data_len);

	pgstat_lock_entry_shared(entry_ref, false);
	memcpy(stats_data,
		   pgstat_get_entry_data(kind, entry_ref->shared_stats),
		   kind_info->shared_data_len);
	pgstat_unlock_entry(entry_ref);

	if (pgstat_fetch_consistency > PGSTAT_FETCH_CONSISTENCY_NONE)
	{
		PgStat_SnapshotEntry *entry = NULL;
		bool		found;

		entry = pgstat_snapshot_insert(pgStatLocal.snapshot.stats, key, &found);
		entry->data = stats_data;
	}

	return stats_data;
}

/*
 * 若已经获取统计快照，则返回获取该快照时的
 * 时间戳，并把 *have_snapshot 设为真。否则设
 * 假。
 */
TimestampTz
pgstat_get_stat_snapshot_timestamp(bool *have_snapshot)
{
	if (force_stats_snapshot_clear)
		pgstat_clear_snapshot();

	if (pgStatLocal.snapshot.mode == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
	{
		*have_snapshot = true;
		return pgStatLocal.snapshot.snapshot_timestamp;
	}

	*have_snapshot = false;

	return 0;
}

bool
pgstat_have_entry(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	/* 固定编号统计始终存在 */
	if (pgstat_get_kind_info(kind)->fixed_amount)
		return true;

	return pgstat_get_entry_ref(kind, dboid, objoid, false, NULL) != NULL;
}

/*
 * 确保固定编号'kind'的快照存在。
 *
 * 通常由某一统计种类的 pgstat_fetch_* 函数在将数据
 * 加工成所需格式之前使用。
 */
void
pgstat_snapshot_fixed(PgStat_Kind kind)
{
	Assert(pgstat_is_kind_valid(kind));
	Assert(pgstat_get_kind_info(kind)->fixed_amount);

	if (force_stats_snapshot_clear)
		pgstat_clear_snapshot();

	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		pgstat_build_snapshot();
	else
		pgstat_build_snapshot_fixed(kind);

	Assert(pgStatLocal.snapshot.fixed_valid[kind]);
}

static void
pgstat_prep_snapshot(void)
{
	if (force_stats_snapshot_clear)
		pgstat_clear_snapshot();

	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_NONE ||
		pgStatLocal.snapshot.stats != NULL)
		return;

	if (!pgStatLocal.snapshot.context)
		pgStatLocal.snapshot.context = AllocSetContextCreate(TopMemoryContext,
															 "PgStat Snapshot",
															 ALLOCSET_SMALL_SIZES);

	pgStatLocal.snapshot.stats =
		pgstat_snapshot_create(pgStatLocal.snapshot.context,
							   PGSTAT_SNAPSHOT_HASH_SIZE,
							   NULL);
}

static void
pgstat_build_snapshot(void)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;

	/* 只应在需要快照时被调用 */
	Assert(pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT);

	/* 快照已构建 */
	if (pgStatLocal.snapshot.mode == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		return;

	pgstat_prep_snapshot();

	Assert(pgStatLocal.snapshot.stats->members == 0);

	pgStatLocal.snapshot.snapshot_timestamp = GetCurrentTimestamp();

	/*
	 * 快照所有可变统计。
	 */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStat_Kind kind = p->key.kind;
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
		bool		found;
		PgStat_SnapshotEntry *entry;
		PgStatShared_Common *stats_data;

		/*
		 * 检查该统计对象是否应包含在快照中。除非某
		 * 统计种类可从所有数据库访问（例如数据库统
		 * 计本身），否则我们只包含当前数据库或未关
		 * 联数据库的对象（例如共享关系的）统计。
		 * 
		 */
		if (p->key.dboid != MyDatabaseId &&
			p->key.dboid != InvalidOid &&
			!kind_info->accessed_across_databases)
			continue;

		if (p->dropped)
			continue;

		Assert(pg_atomic_read_u32(&p->refcount) > 0);

		stats_data = dsa_get_address(pgStatLocal.dsa, p->body);
		Assert(stats_data);

		entry = pgstat_snapshot_insert(pgStatLocal.snapshot.stats, p->key, &found);
		Assert(!found);

		entry->data = MemoryContextAlloc(pgStatLocal.snapshot.context,
										 kind_info->shared_size);

		/*
		 * 直接获取 LWLock，而不是使用
		 * pg_stat_lock_entry_shared()，它需要一个引用。
		 */
		LWLockAcquire(&stats_data->lock, LW_SHARED);
		memcpy(entry->data,
			   pgstat_get_entry_data(kind, stats_data),
			   kind_info->shared_size);
		LWLockRelease(&stats_data->lock);
	}
	dshash_seq_term(&hstat);

	/*
	 * 构建所有固定编号统计的快照。
	 */
	for (int kind = PGSTAT_KIND_FIRST_VALID; kind <= PGSTAT_KIND_LAST; kind++)
	{
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

		if (!kind_info->fixed_amount)
		{
			Assert(kind_info->snapshot_cb == NULL);
			continue;
		}

		pgstat_build_snapshot_fixed(kind);
	}

	pgStatLocal.snapshot.mode = PGSTAT_FETCH_CONSISTENCY_SNAPSHOT;
}

static void
pgstat_build_snapshot_fixed(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

	Assert(kind_info->fixed_amount);
	Assert(kind_info->snapshot_cb != NULL);

	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_NONE)
	{
		/* 每次重建 */
		pgStatLocal.snapshot.fixed_valid[kind] = false;
	}
	else if (pgStatLocal.snapshot.fixed_valid[kind])
	{
		/* 快照模式下不应再次调用我们 */
		Assert(pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_CACHE);
		return;
	}

	Assert(!pgStatLocal.snapshot.fixed_valid[kind]);

	kind_info->snapshot_cb();

	Assert(!pgStatLocal.snapshot.fixed_valid[kind]);
	pgStatLocal.snapshot.fixed_valid[kind] = true;
}


/* ------------------------------------------------------------
 * 后端本地待写入统计基础设施
 * ------------------------------------------------------------
 */

/*
 * 返回相应的 PgStat_EntryRef，若尚未准备则使其
 * 接收待写入统计。
 *
 * 若 created_entry 非 NULL，则当条目为新建时它会被
 * 设为真，否则假。
 */
PgStat_EntryRef *
pgstat_prep_pending_entry(PgStat_Kind kind, Oid dboid, Oid objoid, bool *created_entry)
{
	PgStat_EntryRef *entry_ref;

	/* 需要能够刷写出去 */
	Assert(pgstat_get_kind_info(kind)->flush_pending_cb != NULL);

	if (unlikely(!pgStatPendingContext))
	{
		pgStatPendingContext =
			AllocSetContextCreate(TopMemoryContext,
								  "PgStat Pending",
								  ALLOCSET_SMALL_SIZES);
	}

	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid,
									 true, created_entry);

	if (entry_ref->pending == NULL)
	{
		size_t		entrysize = pgstat_get_kind_info(kind)->pending_size;

		Assert(entrysize != (size_t) -1);

		entry_ref->pending = MemoryContextAllocZero(pgStatPendingContext, entrysize);
		dlist_push_tail(&pgStatPending, &entry_ref->pending_node);
	}

	return entry_ref;
}

/*
 * 返回既有统计条目，或 NULL。
 *
 * 这只应用作 pgstatfuncs.c 的辅助函数——在其之外
 * 应不需要用到它。
 */
PgStat_EntryRef *
pgstat_fetch_pending_entry(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	PgStat_EntryRef *entry_ref;

	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, false, NULL);

	if (entry_ref == NULL || entry_ref->pending == NULL)
		return NULL;

	return entry_ref;
}

void
pgstat_delete_pending_entry(PgStat_EntryRef *entry_ref)
{
	PgStat_Kind kind = entry_ref->shared_entry->key.kind;
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	void	   *pending_data = entry_ref->pending;

	Assert(pending_data != NULL);
	/* 非 fixed_amount 统计应被显式处理 */
	Assert(!pgstat_get_kind_info(kind)->fixed_amount);

	if (kind_info->delete_pending_cb)
		kind_info->delete_pending_cb(entry_ref);

	pfree(pending_data);
	entry_ref->pending = NULL;

	dlist_delete(&entry_ref->pending_node);
}

/*
 * 刷写数据库对象（数据库、关系、函数）待写入
 * 统计。
 */
static bool
pgstat_flush_pending_entries(bool nowait)
{
	bool		have_pending = false;
	dlist_node *cur = NULL;

	/*
	 * 遍历待写入条目列表时需要稍微小心一些。
	 * 处理某个待写入条目可能把更多待写入条目排
	 * 入到我们想要处理的列表末尾，所以简单的迭
	 * 代行不通。更复杂的是，我们还想在每次迭代
	 * 中把已成功刷写的当前条目从列表中删除。
	 *
	 * 所以我们只需在每轮循环中跟踪 next 指针。
	 */
	if (!dlist_is_empty(&pgStatPending))
		cur = dlist_head_node(&pgStatPending);

	while (cur)
	{
		PgStat_EntryRef *entry_ref =
			dlist_container(PgStat_EntryRef, pending_node, cur);
		PgStat_HashKey key = entry_ref->shared_entry->key;
		PgStat_Kind kind = key.kind;
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
		bool		did_flush;
		dlist_node *next;

		Assert(!kind_info->fixed_amount);
		Assert(kind_info->flush_pending_cb != NULL);

		/* 尽可能刷写统计 */
		did_flush = kind_info->flush_pending_cb(entry_ref, nowait);

		Assert(did_flush || nowait);

		/* 在删除待写入条目之前先确定下一个条目 */
		if (dlist_has_next(&pgStatPending, cur))
			next = dlist_next_node(&pgStatPending, cur);
		else
			next = NULL;

		/* 若刷写成功，则移除条目 */
		if (did_flush)
			pgstat_delete_pending_entry(entry_ref);
		else
			have_pending = true;

		cur = next;
	}

	Assert(dlist_is_empty(&pgStatPending) == !have_pending);

	return have_pending;
}


/* ------------------------------------------------------------
 * 辅助 / 基础设施函数
 * ------------------------------------------------------------
 */

PgStat_Kind
pgstat_get_kind_from_str(char *kind_str)
{
	for (int kind = PGSTAT_KIND_FIRST_VALID; kind <= PGSTAT_KIND_LAST; kind++)
	{
		if (pg_strcasecmp(kind_str, pgstat_kind_infos[kind].name) == 0)
			return kind;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid statistics kind: \"%s\"", kind_str)));
	return PGSTAT_KIND_DATABASE;	/* 避免编译器警告 */
}

static inline bool
pgstat_is_kind_valid(int ikind)
{
	return ikind >= PGSTAT_KIND_FIRST_VALID && ikind <= PGSTAT_KIND_LAST;
}

const PgStat_KindInfo *
pgstat_get_kind_info(PgStat_Kind kind)
{
	Assert(pgstat_is_kind_valid(kind));

	return &pgstat_kind_infos[kind];
}

/*
 * 统计只应在 pgstat_initialize() 之后且在
 * pgstat_shutdown() 之前上报。此检查被放在少数
 * 几处中心位置以发现违规。
 */
#ifdef USE_ASSERT_CHECKING
void
pgstat_assert_is_up(void)
{
	Assert(pgstat_is_initialized && !pgstat_is_shutdown);
}
#endif


/* ------------------------------------------------------------
 * 磁盘统计文件的读取与写入
 * ------------------------------------------------------------
 */

/* 供 pgstat_write_statsfile() 用 */
static void
write_chunk(FILE *fpout, void *ptr, size_t len)
{
	int			rc;

	rc = fwrite(ptr, len, 1, fpout);

	/* 最后用一次 ferror 检查错误 */
	(void) rc;
}

#define write_chunk_s(fpout, ptr) write_chunk(fpout, ptr, sizeof(*ptr))

/*
 * 本函数在访问共享统计的最后一个进程中被调
 * 用，因此无需加锁。
 */
static void
pgstat_write_statsfile(void)
{
	FILE	   *fpout;
	int32		format_id;
	const char *tmpfile = PGSTAT_STAT_PERMANENT_TMPFILE;
	const char *statfile = PGSTAT_STAT_PERMANENT_FILENAME;
	dshash_seq_status hstat;
	PgStatShared_HashEntry *ps;

	pgstat_assert_is_up();

	/* 我们正在关闭，直接覆盖它没有问题 */
	pgstat_fetch_consistency = PGSTAT_FETCH_CONSISTENCY_NONE;

	elog(DEBUG2, "writing stats file \"%s\"", statfile);

	/*
	 * 打开统计临时文件以写出当前值。
	 */
	fpout = AllocateFile(tmpfile, PG_BINARY_W);
	if (fpout == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open temporary statistics file \"%s\": %m",
						tmpfile)));
		return;
	}

	/*
	 * 写文件头——目前只是一个格式 ID。
	 */
	format_id = PGSTAT_FILE_FORMAT_ID;
	write_chunk_s(fpout, &format_id);

	/*
	 * XXX：下面这些现在可以泛化为直接遍历
	 * pgstat_kind_infos，而无须了解不同的统计种
	 * 类。
	 */

	/*
	 * 写archiver统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_ARCHIVER);
	write_chunk_s(fpout, &pgStatLocal.snapshot.archiver);

	/*
	 * 写bgwriter统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_BGWRITER);
	write_chunk_s(fpout, &pgStatLocal.snapshot.bgwriter);

	/*
	 * 写检查点统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_CHECKPOINTER);
	write_chunk_s(fpout, &pgStatLocal.snapshot.checkpointer);

	/*
	 * 写IO统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_IO);
	write_chunk_s(fpout, &pgStatLocal.snapshot.io);

	/*
	 * 写SLRU统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_SLRU);
	write_chunk_s(fpout, &pgStatLocal.snapshot.slru);

	/*
	 * 写WAL统计结构体
	 */
	pgstat_build_snapshot_fixed(PGSTAT_KIND_WAL);
	write_chunk_s(fpout, &pgStatLocal.snapshot.wal);

	/*
	 * 遍历各统计条目
	 */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((ps = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_Common *shstats;
		const PgStat_KindInfo *kind_info = NULL;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 写出统计文件时不应看到任何"dropped"条目，
		 * 因为所有后端和辅助进程在终止前都应已清
		 * 理了自己的引用。
		 *
		 * 不过，由于我们已经在关闭之中，为任何潜在
		 * 的清理问题而崩溃服务器并不值得，因此我们
		 * 只需跳过遇到的条目。
		 */
		Assert(!ps->dropped);
		if (ps->dropped)
			continue;

		shstats = (PgStatShared_Common *) dsa_get_address(pgStatLocal.dsa, ps->body);

		kind_info = pgstat_get_kind_info(ps->key.kind);

		/* 若未删除则有效条目引用计数应存在 */
		Assert(pg_atomic_read_u32(&ps->refcount) > 0);

		if (!kind_info->to_serialized_name)
		{
			/* 普通统计条目，由 PgStat_HashKey 标识 */
			fputc('S', fpout);
			write_chunk_s(fpout, &ps->key);
		}
		else
		{
			/* 由磁盘名称标识的统计条目（如槽） */
			NameData	name;

			kind_info->to_serialized_name(&ps->key, shstats, &name);

			fputc('N', fpout);
			write_chunk_s(fpout, &ps->key.kind);
			write_chunk_s(fpout, &name);
		}

		/* 写出除条目头部之外的部分 */
		write_chunk(fpout,
					pgstat_get_entry_data(ps->key.kind, shstats),
					pgstat_get_entry_len(ps->key.kind));
	}
	dshash_seq_term(&hstat);

	/*
	 * 没有更多输出要做了。关闭临时文件并用它替
	 * 换旧的 pgstat.stat。ferror() 检查取代了上面对
	 * 各 fputc 或 fwrite（在 write_chunk() 中）错误检查。
	 */
	fputc('E', fpout);

	if (ferror(fpout))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write temporary statistics file \"%s\": %m",
						tmpfile)));
		FreeFile(fpout);
		unlink(tmpfile);
	}
	else if (FreeFile(fpout) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close temporary statistics file \"%s\": %m",
						tmpfile)));
		unlink(tmpfile);
	}
	else if (rename(tmpfile, statfile) < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename temporary statistics file \"%s\" to \"%s\": %m",
						tmpfile, statfile)));
		unlink(tmpfile);
	}
}

/* 供 pgstat_read_statsfile() 用 */
static bool
read_chunk(FILE *fpin, void *ptr, size_t len)
{
	return fread(ptr, 1, len, fpin) == len;
}

#define read_chunk_s(fpin, ptr) read_chunk(fpin, ptr, sizeof(*ptr))

/*
 * 把既有的统计文件读入共享统计哈希。
 *
 * 本函数在访问共享统计的唯一进程中被调用，
 * 因此无需加锁。
 */
static void
pgstat_read_statsfile(void)
{
	FILE	   *fpin;
	int32		format_id;
	bool		found;
	const char *statfile = PGSTAT_STAT_PERMANENT_FILENAME;
	PgStat_ShmemControl *shmem = pgStatLocal.shmem;

	/* 不应从 postmaster 调用 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	elog(DEBUG2, "reading stats file \"%s\"", statfile);

	/*
	 * 尝试打开统计文件。若它不存在，后端就对任
	 * 何请求都简单返回零，统计则干脆从空白开始
	 * 计数器为空。
	 *
	 * 如果统计收集先前被禁用、或尚未第一次写出
	 * 统计文件，则可能出现 ENOENT。其他任何失败
	 * 条件都值得怀疑。
	 */
	if ((fpin = AllocateFile(statfile, PG_BINARY_R)) == NULL)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open statistics file \"%s\": %m",
							statfile)));
		pgstat_reset_after_failure();
		return;
	}

	/*
	 * 校验它是否为预期格式。
	 */
	if (!read_chunk_s(fpin, &format_id) ||
		format_id != PGSTAT_FILE_FORMAT_ID)
		goto error;

	/*
	 * XXX：下面这些现在可以泛化为直接遍历
	 * pgstat_kind_infos，而无须了解不同的统计种
	 * 类。
	 */

	/*
	 * 读archiver统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->archiver.stats))
		goto error;

	/*
	 * 读bgwriter统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->bgwriter.stats))
		goto error;

	/*
	 * 读检查点统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->checkpointer.stats))
		goto error;

	/*
	 * 读IO统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->io.stats))
		goto error;

	/*
	 * 读SLRU统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->slru.stats))
		goto error;

	/*
	 * 读WAL统计结构体
	 */
	if (!read_chunk_s(fpin, &shmem->wal.stats))
		goto error;

	/*
	 * 我们发现了一个既有的统计文件。读取它并把
	 * 哈希条目都就位。
	 */
	for (;;)
	{
		int			t = fgetc(fpin);

		switch (t)
		{
			case 'S':
			case 'N':
				{
					PgStat_HashKey key;
					PgStatShared_HashEntry *p;
					PgStatShared_Common *header;

					CHECK_FOR_INTERRUPTS();

					if (t == 'S')
					{
						/* 普通统计条目，由 PgStat_HashKey 标识 */
						if (!read_chunk_s(fpin, &key))
							goto error;

						if (!pgstat_is_kind_valid(key.kind))
							goto error;
					}
					else
					{
						/* 由磁盘名称标识的统计条目（如槽） */
						const PgStat_KindInfo *kind_info = NULL;
						PgStat_Kind kind;
						NameData	name;

						if (!read_chunk_s(fpin, &kind))
							goto error;
						if (!read_chunk_s(fpin, &name))
							goto error;
						if (!pgstat_is_kind_valid(kind))
							goto error;

						kind_info = pgstat_get_kind_info(kind);

						if (!kind_info->from_serialized_name)
							goto error;

						if (!kind_info->from_serialized_name(&name, &key))
						{
							/* 跳过我们不关心的条目的数据 */
							if (fseek(fpin, pgstat_get_entry_len(kind), SEEK_CUR) != 0)
								goto error;

							continue;
						}

						Assert(key.kind == kind);
					}

					/*
					 * 这里有意不使用 pgstat_get_entry_ref()——把
					 * 所有统计放进检查点进程的
					 * pgStatEntryRefHash 只会浪费精力和内存。
					 */
					p = dshash_find_or_insert(pgStatLocal.shared_hash, &key, &found);

					/* 不允许重复条目 */
					if (found)
					{
						dshash_release_lock(pgStatLocal.shared_hash, p);
						elog(WARNING, "found duplicate stats entry %d/%u/%u",
							 key.kind, key.dboid, key.objoid);
						goto error;
					}

					header = pgstat_init_entry(key.kind, p);
					dshash_release_lock(pgStatLocal.shared_hash, p);
					if (header == NULL)
					{
						/*
						 * 把此 ERROR 改成 WARNING 看似诱人，
						 * 但意味着环境因 OOM 失败时所有统计
						 * 都会被丢弃。
						 */
						elog(ERROR,	"could not allocate entry %d/%u/%u",
							 key.kind, key.dboid, key.objoid);
					}

					if (!read_chunk(fpin,
									pgstat_get_entry_data(key.kind, header),
									pgstat_get_entry_len(key.kind)))
						goto error;

					break;
				}
			case 'E':
				/* 检查'E'是否确实表示文件结束 */
				if (fgetc(fpin) != EOF)
					goto error;

				goto done;

			default:
				goto error;
		}
	}

done:
	FreeFile(fpin);

	elog(DEBUG2, "removing permanent stats file \"%s\"", statfile);
	unlink(statfile);

	return;

error:
	ereport(LOG,
			(errmsg("corrupted statistics file \"%s\"", statfile)));

	pgstat_reset_after_failure();

	goto done;
}

/*
 * 崩溃后、或从磁盘恢复统计失败后（可能已经
 * 加载部分）重置 / 删除统计辅助函数。
 */
static void
pgstat_reset_after_failure(void)
{
	TimestampTz ts = GetCurrentTimestamp();

	/* 重置固定编号统计 */
	for (int kind = PGSTAT_KIND_FIRST_VALID; kind <= PGSTAT_KIND_LAST; kind++)
	{
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

		if (!kind_info->fixed_amount)
			continue;

		kind_info->reset_all_cb(ts);
	}

	/* 并删除可变编号的统计 */
	pgstat_drop_all_entries();
}

/*
 * stats_fetch_consistency 的 assign_hook。
 */
void
assign_stats_fetch_consistency(int newval, void *extra)
{
	/*
	 * 在事务中更改此值可能导致快照状态不一致，
	 * 因此强制在下次快照构建尝试时清除当前快照。
	 * 
	 */
	if (pgstat_fetch_consistency != newval)
		force_stats_snapshot_clear = true;
}
