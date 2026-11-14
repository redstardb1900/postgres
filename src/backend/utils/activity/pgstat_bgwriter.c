/* -------------------------------------------------------------------------
 *
 * pgstat_bgwriter.c
 *	  后台写进程统计的实现。
 *
 * 本文件包含后台写进程统计的实现。与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_bgwriter.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


PgStat_BgWriterStats PendingBgWriterStats = {0};


/*
 * 报告 bgwriter 与 IO 统计
 */
void
pgstat_report_bgwriter(void)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;
	static const PgStat_BgWriterStats all_zeroes;

	Assert(!pgStatLocal.shmem->is_shutdown);
	pgstat_assert_is_up();

	/*
	 * 本函数即使在什么都没发生时也可能被调用。此时
	 * 应避免不必要地修改统计条目。
	 */
	if (memcmp(&PendingBgWriterStats, &all_zeroes, sizeof(all_zeroes)) == 0)
		return;

	pgstat_begin_changecount_write(&stats_shmem->changecount);

#define BGWRITER_ACC(fld) stats_shmem->stats.fld += PendingBgWriterStats.fld
	BGWRITER_ACC(buf_written_clean);
	BGWRITER_ACC(maxwritten_clean);
	BGWRITER_ACC(buf_alloc);
#undef BGWRITER_ACC

	pgstat_end_changecount_write(&stats_shmem->changecount);

	/*
	 * 清空统计缓冲区，以便复用。
	 */
	MemSet(&PendingBgWriterStats, 0, sizeof(PendingBgWriterStats));

	/*
	 * 报告 IO 统计
	 */
	pgstat_flush_io(false);
}

/*
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回后台写进程统计结构指针。
 */
PgStat_BgWriterStats *
pgstat_fetch_stat_bgwriter(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_BGWRITER);

	return &pgStatLocal.snapshot.bgwriter;
}

void
pgstat_bgwriter_reset_all_cb(TimestampTz ts)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;

	/* 重置协议见上方 PgStatShared_BgWriter 的说明 */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_bgwriter_snapshot_cb(void)
{
	PgStatShared_BgWriter *stats_shmem = &pgStatLocal.shmem->bgwriter;
	PgStat_BgWriterStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_BgWriterStats reset;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.bgwriter,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* 用重置偏移量补偿 */
#define BGWRITER_COMP(fld) pgStatLocal.snapshot.bgwriter.fld -= reset.fld;
	BGWRITER_COMP(buf_written_clean);
	BGWRITER_COMP(maxwritten_clean);
	BGWRITER_COMP(buf_alloc);
#undef BGWRITER_COMP
}
