/* -------------------------------------------------------------------------
 *
 * pgstat_subscription.c
 *	  订阅统计的实现。
 *
 * 本文件包含订阅统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_subscription.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


/*
 * 报告订阅错误。
 */
void
pgstat_report_subscription_error(Oid subid, bool is_apply_error)
{
	PgStat_EntryRef *entry_ref;
	PgStat_BackendSubEntry *pending;

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_SUBSCRIPTION,
										  InvalidOid, subid, NULL);
	pending = entry_ref->pending;

	if (is_apply_error)
		pending->apply_error_count++;
	else
		pending->sync_error_count++;
}

/*
 * 报告订阅的创建。
 */
void
pgstat_create_subscription(Oid subid)
{
	/* 确保事务回滚时统计被丢弃 */
	pgstat_create_transactional(PGSTAT_KIND_SUBSCRIPTION,
								InvalidOid, subid);

	/* 创建并初始化订阅统计条目 */
	pgstat_get_entry_ref(PGSTAT_KIND_SUBSCRIPTION, InvalidOid, subid,
						 true, NULL);
	pgstat_reset_entry(PGSTAT_KIND_SUBSCRIPTION, InvalidOid, subid, 0);
}

/*
 * 报告订阅的删除。
 *
 * 确保事务提交时统计被丢弃。
 */
void
pgstat_drop_subscription(Oid subid)
{
	pgstat_drop_transactional(PGSTAT_KIND_SUBSCRIPTION,
							  InvalidOid, subid);
}

/*
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回单个订阅的已收集统计或 NULL。
 */
PgStat_StatSubEntry *
pgstat_fetch_stat_subscription(Oid subid)
{
	return (PgStat_StatSubEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_SUBSCRIPTION, InvalidOid, subid);
}

/*
 * 刷出该条目的待写入统计
 *
 * 若 nowait 为真且无法立即获得锁，则不刷写
 * 该条目而返回假。否则返回真。
 */
bool
pgstat_subscription_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_BackendSubEntry *localent;
	PgStatShared_Subscription *shsubent;

	localent = (PgStat_BackendSubEntry *) entry_ref->pending;
	shsubent = (PgStatShared_Subscription *) entry_ref->shared_stats;

	/* localent 的内容总是非零 */

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define SUB_ACC(fld) shsubent->stats.fld += localent->fld
	SUB_ACC(apply_error_count);
	SUB_ACC(sync_error_count);
#undef SUB_ACC

	pgstat_unlock_entry(entry_ref);
	return true;
}

void
pgstat_subscription_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Subscription *) header)->stats.stat_reset_timestamp = ts;
}
