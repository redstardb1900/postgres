/* -------------------------------------------------------------------------
 *
 * pgstat_checkpointer.c
 *	  检查点统计的实现。
 *
 * 本文件包含检查点统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_checkpointer.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


PgStat_CheckpointerStats PendingCheckpointerStats = {0};


/*
 * 报告检查点进程与 IO 统计
 */
void
pgstat_report_checkpointer(void)
{
	/* 假定其初始化为零值 */
	static const PgStat_CheckpointerStats all_zeroes;
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	Assert(!pgStatLocal.shmem->is_shutdown);
	pgstat_assert_is_up();

	/*
	 * 本函数即使在什么都没发生时也可能被调用。此时
	 * 应避免不必要地修改统计条目。
	 */
	if (memcmp(&PendingCheckpointerStats, &all_zeroes,
			   sizeof(all_zeroes)) == 0)
		return;

	pgstat_begin_changecount_write(&stats_shmem->changecount);

#define CHECKPOINTER_ACC(fld) stats_shmem->stats.fld += PendingCheckpointerStats.fld
	CHECKPOINTER_ACC(timed_checkpoints);
	CHECKPOINTER_ACC(requested_checkpoints);
	CHECKPOINTER_ACC(checkpoint_write_time);
	CHECKPOINTER_ACC(checkpoint_sync_time);
	CHECKPOINTER_ACC(buf_written_checkpoints);
	CHECKPOINTER_ACC(buf_written_backend);
	CHECKPOINTER_ACC(buf_fsync_backend);
#undef CHECKPOINTER_ACC

	pgstat_end_changecount_write(&stats_shmem->changecount);

	/*
	 * 清空统计缓冲区，以便复用。
	 */
	MemSet(&PendingCheckpointerStats, 0, sizeof(PendingCheckpointerStats));

	/*
	 * 报告 IO 统计
	 */
	pgstat_flush_io(false);
}

/*
 * pgstat_fetch_stat_checkpointer() -
 *
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回检查点进程统计结构的指针。
 */
PgStat_CheckpointerStats *
pgstat_fetch_stat_checkpointer(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_CHECKPOINTER);

	return &pgStatLocal.snapshot.checkpointer;
}

void
pgstat_checkpointer_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;

	/* 重置协议见上方 PgStatShared_Checkpointer 的说明 */
	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_checkpointer_snapshot_cb(void)
{
	PgStatShared_Checkpointer *stats_shmem = &pgStatLocal.shmem->checkpointer;
	PgStat_CheckpointerStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_CheckpointerStats reset;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.checkpointer,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	/* 用重置偏移量补偿 */
#define CHECKPOINTER_COMP(fld) pgStatLocal.snapshot.checkpointer.fld -= reset.fld;
	CHECKPOINTER_COMP(timed_checkpoints);
	CHECKPOINTER_COMP(requested_checkpoints);
	CHECKPOINTER_COMP(checkpoint_write_time);
	CHECKPOINTER_COMP(checkpoint_sync_time);
	CHECKPOINTER_COMP(buf_written_checkpoints);
	CHECKPOINTER_COMP(buf_written_backend);
	CHECKPOINTER_COMP(buf_fsync_backend);
#undef CHECKPOINTER_COMP
}
