/* -------------------------------------------------------------------------
 *
 * pgstat_database.c
 *	  数据库统计的实现。
 *
 * 本文件包含数据库统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_database.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"
#include "storage/procsignal.h"


static bool pgstat_should_report_connstat(void);


PgStat_Counter pgStatBlockReadTime = 0;
PgStat_Counter pgStatBlockWriteTime = 0;
PgStat_Counter pgStatActiveTime = 0;
PgStat_Counter pgStatTransactionIdleTime = 0;
SessionEndType pgStatSessionEndCause = DISCONNECT_NORMAL;


static int	pgStatXactCommit = 0;
static int	pgStatXactRollback = 0;
static PgStat_Counter pgLastSessionReportTime = 0;


/*
 * 删除正被删除的数据库的条目。
 */
void
pgstat_drop_database(Oid databaseid)
{
	pgstat_drop_transactional(PGSTAT_KIND_DATABASE, databaseid, InvalidOid);
}

/*
 * 由 autovacuum.c 调用，报告自动清理进程的启动。
 * 调用发生在 InitPostgres 完成之前，不能依赖 MyDatabaseId；
 * 而是必须传入数据库 OID。
 */
void
pgstat_report_autovac(Oid dboid)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Database *dbentry;

	/* 单用户模式不会到这里 */
	Assert(IsUnderPostmaster);

	/*
	 * 清理结束是即时上报。开始也以同样方式上报，
	 * 以保持一致。VACUUM 不常运行且耗时长，即使
	 * 在此稍有阻塞也无妨。
	 */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE,
											dboid, InvalidOid, false);

	dbentry = (PgStatShared_Database *) entry_ref->shared_stats;
	dbentry->stats.last_autovac_time = GetCurrentTimestamp();

	pgstat_unlock_entry(entry_ref);
}

/*
 * 报告热备恢复冲突。
 */
void
pgstat_report_recovery_conflict(int reason)
{
	PgStat_StatDBEntry *dbentry;

	Assert(IsUnderPostmaster);
	if (!pgstat_track_counts)
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:

			/*
			 * 由于数据库信息一经复制即被删除，统计这些
			 * 冲突毫无意义。
			 */
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			dbentry->conflict_tablespace++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			dbentry->conflict_lock++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			dbentry->conflict_snapshot++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			dbentry->conflict_bufferpin++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			dbentry->conflict_logicalslot++;
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			dbentry->conflict_startup_deadlock++;
			break;
	}
}

/*
 * 报告检测到的死锁。
 */
void
pgstat_report_deadlock(void)
{
	PgStat_StatDBEntry *dbent;

	if (!pgstat_track_counts)
		return;

	dbent = pgstat_prep_database_pending(MyDatabaseId);
	dbent->deadlocks++;
}

/*
 * 报告一或多个校验和失败。
 */
void
pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Database *sharedent;

	if (!pgstat_track_counts)
		return;

	/*
	 * 直接更新共享统计——校验和失败绝不应该多到让
	 * 这成为问题。
	 */
	entry_ref =
		pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE, dboid, InvalidOid, false);

	sharedent = (PgStatShared_Database *) entry_ref->shared_stats;
	sharedent->stats.checksum_failures += failurecount;
	sharedent->stats.last_checksum_failure = GetCurrentTimestamp();

	pgstat_unlock_entry(entry_ref);
}

/*
 * 报告当前数据库中的一次校验和失败。
 */
void
pgstat_report_checksum_failure(void)
{
	pgstat_report_checksum_failures_in_db(MyDatabaseId, 1);
}

/*
 * 报告临时文件的创建。
 */
void
pgstat_report_tempfile(size_t filesize)
{
	PgStat_StatDBEntry *dbent;

	if (!pgstat_track_counts)
		return;

	dbent = pgstat_prep_database_pending(MyDatabaseId);
	dbent->temp_bytes += filesize;
	dbent->temp_files++;
}

/*
 * 向统计系统通知新连接。
 */
void
pgstat_report_connect(Oid dboid)
{
	PgStat_StatDBEntry *dbentry;

	if (!pgstat_should_report_connstat())
		return;

	pgLastSessionReportTime = MyStartTimestamp;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);
	dbentry->sessions++;
}

/*
 * 向统计系统通知断开连接。
 */
void
pgstat_report_disconnect(Oid dboid)
{
	PgStat_StatDBEntry *dbentry;

	if (!pgstat_should_report_connstat())
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	switch (pgStatSessionEndCause)
	{
		case DISCONNECT_NOT_YET:
		case DISCONNECT_NORMAL:
			/* 我们不收集这些 */
			break;
		case DISCONNECT_CLIENT_EOF:
			dbentry->sessions_abandoned++;
			break;
		case DISCONNECT_FATAL:
			dbentry->sessions_fatal++;
			break;
		case DISCONNECT_KILLED:
			dbentry->sessions_killed++;
			break;
	}
}

/*
 * SQL 可调用 pgstat* 函数的支持函数。返回
 * 某个数据库的统计信息或 NULL。NULL 并不代表
 * 数据库不存在，只是尚无统计信息，因此调用者最
 * 好还是报告 ZERO。
 */
PgStat_StatDBEntry *
pgstat_fetch_stat_dbentry(Oid dboid)
{
	return (PgStat_StatDBEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_DATABASE, dboid, InvalidOid);
}

void
AtEOXact_PgStat_Database(bool isCommit, bool parallel)
{
	/* 不统计并行工作进程的事务统计 */
	if (!parallel)
	{
		/*
		 * 统计事务提交或回滚。（使用计数器而非单纯
		 * 布尔值，以防上报消息未立即发出。）
		 */
		if (isCommit)
			pgStatXactCommit++;
		else
			pgStatXactRollback++;
	}
}

/*
 * pgstat_report_stat() 的子例程：处理事务提交/回滚
 * IO计时
 */
void
pgstat_update_dbstats(TimestampTz ts)
{
	PgStat_StatDBEntry *dbentry;

	/*
	 * 若尚未连接到数据库，不要把时间记到 "shared
	 * state"（InvalidOid 用于跟踪共享关系等的统计）。
	 */
	if (!OidIsValid(MyDatabaseId))
		return;

	dbentry = pgstat_prep_database_pending(MyDatabaseId);

	/*
	 * 将事务提交/回滚与 IO 计时累加到当前数据库的
	 * 统计条目。
	 */
	dbentry->xact_commit += pgStatXactCommit;
	dbentry->xact_rollback += pgStatXactRollback;
	dbentry->blk_read_time += pgStatBlockReadTime;
	dbentry->blk_write_time += pgStatBlockWriteTime;

	if (pgstat_should_report_connstat())
	{
		long		secs;
		int			usecs;

		/*
		 * pgLastSessionReportTime 由 pgstat_report_connect() 设为
		 * MyStartTimestamp。
		 */
		TimestampDifference(pgLastSessionReportTime, ts, &secs, &usecs);
		pgLastSessionReportTime = ts;
		dbentry->session_time += (PgStat_Counter) secs * 1000000 + usecs;
		dbentry->active_time += pgStatActiveTime;
		dbentry->idle_in_transaction_time += pgStatTransactionIdleTime;
	}

	pgStatXactCommit = 0;
	pgStatXactRollback = 0;
	pgStatBlockReadTime = 0;
	pgStatBlockWriteTime = 0;
	pgStatActiveTime = 0;
	pgStatTransactionIdleTime = 0;
}

/*
 * 会话统计仅对普通后台进程上报。并行工作进程
 * 并行运行，因此虽然消耗 CPU 时间，却不计入会话
 * 时间。Walsender 进程也可以考虑纳入，但它们的
 * 会话特征与普通后台不同（例如它们总是
 * 处于 "active" 状态），因此会扭曲会话统计。
 */
static bool
pgstat_should_report_connstat(void)
{
	return MyBackendType == B_BACKEND;
}

/*
 * 查找或创建 dboid 本地 PgStat_StatDBEntry 条目。
 */
PgStat_StatDBEntry *
pgstat_prep_database_pending(Oid dboid)
{
	PgStat_EntryRef *entry_ref;

	/*
	 * 连接到数据库之前，不应上报数据库对象的
	 * 统计。
	 */
	Assert(!OidIsValid(dboid) || OidIsValid(MyDatabaseId));

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_DATABASE, dboid, InvalidOid,
										  NULL);

	return entry_ref->pending;
}

/*
 * 重置数据库的重置时间戳，但不重置数据库统计的
 * 内容。
 */
void
pgstat_reset_database_timestamp(Oid dboid, TimestampTz ts)
{
	PgStat_EntryRef *dbref;
	PgStatShared_Database *dbentry;

	dbref = pgstat_get_entry_ref_locked(PGSTAT_KIND_DATABASE, MyDatabaseId, InvalidOid,
										false);

	dbentry = (PgStatShared_Database *) dbref->shared_stats;
	dbentry->stats.stat_reset_timestamp = ts;

	pgstat_unlock_entry(dbref);
}

/*
 * 刷出该条目的待写入统计
 *
 * 若 nowait 为 true 且无法立即获得锁，则返回 false，
 * 不刷写该条目。否则返回 true。
 */
bool
pgstat_database_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_Database *sharedent;
	PgStat_StatDBEntry *pendingent;

	pendingent = (PgStat_StatDBEntry *) entry_ref->pending;
	sharedent = (PgStatShared_Database *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define PGSTAT_ACCUM_DBCOUNT(item)		\
	(sharedent)->stats.item += (pendingent)->item

	PGSTAT_ACCUM_DBCOUNT(xact_commit);
	PGSTAT_ACCUM_DBCOUNT(xact_rollback);
	PGSTAT_ACCUM_DBCOUNT(blocks_fetched);
	PGSTAT_ACCUM_DBCOUNT(blocks_hit);

	PGSTAT_ACCUM_DBCOUNT(tuples_returned);
	PGSTAT_ACCUM_DBCOUNT(tuples_fetched);
	PGSTAT_ACCUM_DBCOUNT(tuples_inserted);
	PGSTAT_ACCUM_DBCOUNT(tuples_updated);
	PGSTAT_ACCUM_DBCOUNT(tuples_deleted);

	/* last_autovac_time 是即时上报的 */
	Assert(pendingent->last_autovac_time == 0);

	PGSTAT_ACCUM_DBCOUNT(conflict_tablespace);
	PGSTAT_ACCUM_DBCOUNT(conflict_lock);
	PGSTAT_ACCUM_DBCOUNT(conflict_snapshot);
	PGSTAT_ACCUM_DBCOUNT(conflict_logicalslot);
	PGSTAT_ACCUM_DBCOUNT(conflict_bufferpin);
	PGSTAT_ACCUM_DBCOUNT(conflict_startup_deadlock);

	PGSTAT_ACCUM_DBCOUNT(temp_bytes);
	PGSTAT_ACCUM_DBCOUNT(temp_files);
	PGSTAT_ACCUM_DBCOUNT(deadlocks);

	/* 校验和失败是即时上报的 */
	Assert(pendingent->checksum_failures == 0);
	Assert(pendingent->last_checksum_failure == 0);

	PGSTAT_ACCUM_DBCOUNT(blk_read_time);
	PGSTAT_ACCUM_DBCOUNT(blk_write_time);

	PGSTAT_ACCUM_DBCOUNT(sessions);
	PGSTAT_ACCUM_DBCOUNT(session_time);
	PGSTAT_ACCUM_DBCOUNT(active_time);
	PGSTAT_ACCUM_DBCOUNT(idle_in_transaction_time);
	PGSTAT_ACCUM_DBCOUNT(sessions_abandoned);
	PGSTAT_ACCUM_DBCOUNT(sessions_fatal);
	PGSTAT_ACCUM_DBCOUNT(sessions_killed);
#undef PGSTAT_ACCUM_DBCOUNT

	pgstat_unlock_entry(entry_ref);

	memset(pendingent, 0, sizeof(*pendingent));

	return true;
}

void
pgstat_database_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Database *) header)->stats.stat_reset_timestamp = ts;
}
