/* -------------------------------------------------------------------------
 *
 * pgstat_wal.c
 *	  WAL 统计的实现。
 *
 * 本文件包含 WAL 统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_wal.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "executor/instrument.h"


PgStat_PendingWalStats PendingWalStats = {0};

/*
 * 保存自上次调用 pgstat_report_wal() 时 pgWalUsage 的
 * WAL 使用计数。用来计算 pgstat_report_wal() 调用
 * 之间发生的 WAL 使用量，从当前值中减去
 * 先前计数器而得。
 */
static WalUsage prevWalUsage;


/*
 * 计算 WAL 使用计数器的增量并更新共享 WAL 与 IO
 * 统计信息。
 *
 * 必须由生成 WAL 而不调用 pgstat_report_stat() 的
 * 进程调用，例如 walwriter。
 *
 * "force" 设为真可确保统计被刷写；注意这需要获取
 * pgstat shmem LWLock，并等待其获取。设为假时，
 * 若无法获得锁，统计可能不会被刷
 * 写。
 */
void
pgstat_report_wal(bool force)
{
	bool		nowait;

	/* 与 pgstat.c 中一样，!force 时不等待获取锁 */
	nowait = !force;

	/* 刷写WAL统计 */
	pgstat_flush_wal(nowait);

	/* 刷写IO统计 */
	pgstat_flush_io(nowait);
}

/*
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回 WAL 统计结构的指针。
 */
PgStat_WalStats *
pgstat_fetch_stat_wal(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_WAL);

	return &pgStatLocal.snapshot.wal;
}

/*
 * 通过从当前计数器中减去先前计数器，计算 WAL 使用
 * 计数器的增量。
 *
 * 若 nowait 为真，且无法获得锁，本函数返回真。
 * 否则返回假。
 */
bool
pgstat_flush_wal(bool nowait)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;
	WalUsage	wal_usage_diff = {0};

	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);
	Assert(pgStatLocal.shmem != NULL &&
		   !pgStatLocal.shmem->is_shutdown);

	/*
	 * 即使什么都没发生也可能调用本函数。那种情况下
	 * 就不必白白获取锁。
	 */
	if (!pgstat_have_pending_wal())
		return false;

	/*
	 * 本地 WalStats 的 WAL 使用部分不会在别处更新。
	 * 从当前计数器中减去先前计数器，即可算出 WAL 使用
	 * 计数增加了多少。
	 */
	WalUsageAccumDiff(&wal_usage_diff, &pgWalUsage, &prevWalUsage);

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

#define WALSTAT_ACC(fld, var_to_add) \
	(stats_shmem->stats.fld += var_to_add.fld)
#define WALSTAT_ACC_INSTR_TIME(fld) \
	(stats_shmem->stats.fld += INSTR_TIME_GET_MICROSEC(PendingWalStats.fld))
	WALSTAT_ACC(wal_records, wal_usage_diff);
	WALSTAT_ACC(wal_fpi, wal_usage_diff);
	WALSTAT_ACC(wal_bytes, wal_usage_diff);
	WALSTAT_ACC(wal_buffers_full, PendingWalStats);
	WALSTAT_ACC(wal_write, PendingWalStats);
	WALSTAT_ACC(wal_sync, PendingWalStats);
	WALSTAT_ACC_INSTR_TIME(wal_write_time);
	WALSTAT_ACC_INSTR_TIME(wal_sync_time);
#undef WALSTAT_ACC_INSTR_TIME
#undef WALSTAT_ACC

	LWLockRelease(&stats_shmem->lock);

	/*
	 * 保存当前计数器，供后续计算 WAL 使用量之用。
	 */
	prevWalUsage = pgWalUsage;

	/*
	 * 清空统计缓冲区，以便复用。
	 */
	MemSet(&PendingWalStats, 0, sizeof(PendingWalStats));

	return false;
}

void
pgstat_init_wal(void)
{
	/*
	 * 用 pgWalUsage 初始化 prevWalUsage，使 pgstat_flush_wal()
	 * 能够通过从 pgWalUsage 中减去 prevWalUsage，计算出
	 * pgWalUsage 的增量。
	 */
	prevWalUsage = pgWalUsage;
}

/*
 * 要判断上次以来是否发生过 WAL 活动，不仅需要检查
 * 已生成的 WAL 记录数，还需检查 WAL 写入与同步的
 * 次数。因为即使不生成 WAL 记录的事务也会在
 * 刷写时执行写入或同步 WAL 数据，涉及数据
 * 页。
 */
bool
pgstat_have_pending_wal(void)
{
	return pgWalUsage.wal_records != prevWalUsage.wal_records ||
		PendingWalStats.wal_write != 0 ||
		PendingWalStats.wal_sync != 0;
}

void
pgstat_wal_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	memset(&stats_shmem->stats, 0, sizeof(stats_shmem->stats));
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_wal_snapshot_cb(void)
{
	PgStatShared_Wal *stats_shmem = &pgStatLocal.shmem->wal;

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&pgStatLocal.snapshot.wal, &stats_shmem->stats,
		   sizeof(pgStatLocal.snapshot.wal));
	LWLockRelease(&stats_shmem->lock);
}
