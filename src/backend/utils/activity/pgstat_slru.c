/* -------------------------------------------------------------------------
 *
 * pgstat_slru.c
 *	  SLRU 统计的实现。
 *
 * 本文件包含 SLRU 统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_slru.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


static inline PgStat_SLRUStats *get_slru_entry(int slru_idx);
static void pgstat_reset_slru_counter_internal(int index, TimestampTz ts);


/*
 * SLRU 统计计数等待被刷写。假定本变量初始化为零
 * 值。各条目与 slru_names[] 一一对应。SLRU 计数的
 * 变更都在临界区内报告，因此我们使用静态内存，
 * 以避免内存分配。
 */
static PgStat_SLRUStats pending_SLRUStats[SLRU_NUM_ELEMENTS];
bool		have_slrustats = false;


/*
 * 重置单个 SLRU 的计数器。
 *
 * 本函数的权限检查由常规的 GRANT 体系
 * 管理。
 */
void
pgstat_reset_slru(const char *name)
{
	TimestampTz ts = GetCurrentTimestamp();

	Assert(name != NULL);

	pgstat_reset_slru_counter_internal(pgstat_get_slru_index(name), ts);
}

/*
 * SLRU 统计计数累加函数 --- 由 slru.c 调用
 */

void
pgstat_count_slru_page_zeroed(int slru_idx)
{
	get_slru_entry(slru_idx)->blocks_zeroed += 1;
}

void
pgstat_count_slru_page_hit(int slru_idx)
{
	get_slru_entry(slru_idx)->blocks_hit += 1;
}

void
pgstat_count_slru_page_exists(int slru_idx)
{
	get_slru_entry(slru_idx)->blocks_exists += 1;
}

void
pgstat_count_slru_page_read(int slru_idx)
{
	get_slru_entry(slru_idx)->blocks_read += 1;
}

void
pgstat_count_slru_page_written(int slru_idx)
{
	get_slru_entry(slru_idx)->blocks_written += 1;
}

void
pgstat_count_slru_flush(int slru_idx)
{
	get_slru_entry(slru_idx)->flush += 1;
}

void
pgstat_count_slru_truncate(int slru_idx)
{
	get_slru_entry(slru_idx)->truncate += 1;
}

/*
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回 slru 统计结构的指针。
 */
PgStat_SLRUStats *
pgstat_fetch_slru(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_SLRU);

	return pgStatLocal.snapshot.slru;
}

/*
 * 返回某索引对应的 SLRU 名称。该索引可以超过
 * SLRU_NUM_ELEMENTS，此时返回 NULL。这使代码可以
 * 无需预先知道条目数量。
 */
const char *
pgstat_get_slru_name(int slru_idx)
{
	if (slru_idx < 0 || slru_idx >= SLRU_NUM_ELEMENTS)
		return NULL;

	return slru_names[slru_idx];
}

/*
 * 确定具有给定名称的 SLRU 的条目索引。若没有精
 * 确匹配，则返回最后一个"other"条目的索引，其供外
 * 部项目使用。
 */
int
pgstat_get_slru_index(const char *name)
{
	int			i;

	for (i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		if (strcmp(slru_names[i], name) == 0)
			return i;
	}

	/* 返回最后一个条目（即"other"项）的索引 */
	return (SLRU_NUM_ELEMENTS - 1);
}

/*
 * 刷出本地待写入的 SLRU 统计条目
 *
 * 若 nowait 为真，获取锁失败时本函数返回假。否则
 * 本函数总是返回真。
 *
 * 若 nowait 为真，且无法获得锁，本函数返回真。
 * 否则返回假。
 */
bool
pgstat_slru_flush(bool nowait)
{
	PgStatShared_SLRU *stats_shmem = &pgStatLocal.shmem->slru;
	int			i;

	if (!have_slrustats)
		return false;

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

	for (i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		PgStat_SLRUStats *sharedent = &stats_shmem->stats[i];
		PgStat_SLRUStats *pendingent = &pending_SLRUStats[i];

#define SLRU_ACC(fld) sharedent->fld += pendingent->fld
		SLRU_ACC(blocks_zeroed);
		SLRU_ACC(blocks_hit);
		SLRU_ACC(blocks_read);
		SLRU_ACC(blocks_written);
		SLRU_ACC(blocks_exists);
		SLRU_ACC(flush);
		SLRU_ACC(truncate);
#undef SLRU_ACC
	}

	/* 随后清空待写入条目 */
	MemSet(pending_SLRUStats, 0, sizeof(pending_SLRUStats));

	LWLockRelease(&stats_shmem->lock);

	have_slrustats = false;

	return false;
}

void
pgstat_slru_reset_all_cb(TimestampTz ts)
{
	for (int i = 0; i < SLRU_NUM_ELEMENTS; i++)
		pgstat_reset_slru_counter_internal(i, ts);
}

void
pgstat_slru_snapshot_cb(void)
{
	PgStatShared_SLRU *stats_shmem = &pgStatLocal.shmem->slru;

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);

	memcpy(pgStatLocal.snapshot.slru, &stats_shmem->stats,
		   sizeof(stats_shmem->stats));

	LWLockRelease(&stats_shmem->lock);
}

/*
 * 返回给定 SLRU 的计数条目指针（基于 SlruCtl 中
 * 存储的 lwlock tranche 名称）。
 */
static inline PgStat_SLRUStats *
get_slru_entry(int slru_idx)
{
	pgstat_assert_is_up();

	/*
	 * postmaster 绝不应注册任何 SLRU 统计计数；否则
	 * 这些计数会经由 fork() 被复制到子进程中。
	 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	Assert((slru_idx >= 0) && (slru_idx < SLRU_NUM_ELEMENTS));

	have_slrustats = true;

	return &pending_SLRUStats[slru_idx];
}

static void
pgstat_reset_slru_counter_internal(int index, TimestampTz ts)
{
	PgStatShared_SLRU *stats_shmem = &pgStatLocal.shmem->slru;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);

	memset(&stats_shmem->stats[index], 0, sizeof(PgStat_SLRUStats));
	stats_shmem->stats[index].stat_reset_timestamp = ts;

	LWLockRelease(&stats_shmem->lock);
}
