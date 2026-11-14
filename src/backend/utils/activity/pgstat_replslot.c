/* -------------------------------------------------------------------------
 *
 * pgstat_replslot.c
 *	  复制槽统计的实现。
 *
 * 本文件包含复制槽统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * 复制槽统计的工作方式与其他可变编号统计有些差异。
 * 槽没有 OID，故能在物理备机创建。运行期间以
 * 槽索引作为对象 ID。然而，槽索引在重启时可能
 * 改变。针对这一点，办法是在（反）序列化时使用
 * 槽名。重启之后，槽可能在停机期间被删除，这
 * 一问题的对策是：不恢复启动时按名称找不到的槽
 * 的统计信息。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_replslot.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/slot.h"
#include "utils/builtins.h"		/* namestrcpy() */
#include "utils/pgstat_internal.h"


static int	get_replslot_index(const char *name);


/*
 * 重置单个复制槽的计数器。
 *
 * 本函数的权限检查由常规的 GRANT 体系
 * 管理。
 */
void
pgstat_reset_replslot(const char *name)
{
	ReplicationSlot *slot;

	Assert(name != NULL);

	/* 检查具有给定名称的槽是否存在 */
	slot = SearchNamedReplicationSlot(name, true);

	if (!slot)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("replication slot \"%s\" does not exist",
						name)));

	/*
	 * 物理槽无需处理，因为统计信息仅收集自逻辑
	 * 槽。
	 */
	if (SlotIsPhysical(slot))
		return;

	/* 重置该条目 */
	pgstat_reset(PGSTAT_KIND_REPLSLOT, InvalidOid,
				 ReplicationSlotIndex(slot));
}

/*
 * 报告复制槽统计。
 *
 * 可认定该槽的统计必然存在，且必然属于本
 * 槽。只有已经调用过 pgstat_create_replslot() 或
 * pgstat_acquire_replslot() 才会到达此处。
 */
void
pgstat_report_replslot(ReplicationSlot *slot, const PgStat_StatReplSlotEntry *repSlotStat)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ReplSlot *shstatent;
	PgStat_StatReplSlotEntry *statent;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;
	statent = &shstatent->stats;

	/* 更新复制槽统计 */
#define REPLSLOT_ACC(fld) statent->fld += repSlotStat->fld
	REPLSLOT_ACC(spill_txns);
	REPLSLOT_ACC(spill_count);
	REPLSLOT_ACC(spill_bytes);
	REPLSLOT_ACC(stream_txns);
	REPLSLOT_ACC(stream_count);
	REPLSLOT_ACC(stream_bytes);
	REPLSLOT_ACC(total_txns);
	REPLSLOT_ACC(total_bytes);
#undef REPLSLOT_ACC

	pgstat_unlock_entry(entry_ref);
}

/*
 * 报告复制槽的创建。
 *
 * 注意：调用本函数时已持有 ReplicationSlotAllocationLock，
 * 回调 slot.c 时须谨慎。
 */
void
pgstat_create_replslot(ReplicationSlot *slot)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ReplSlot *shstatent;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_REPLSLOT, InvalidOid,
											ReplicationSlotIndex(slot), false);
	shstatent = (PgStatShared_ReplSlot *) entry_ref->shared_stats;

	/*
	 * 注意：需接受可能存在来自旧槽的统计，例如在
	 * 删除槽之后曾崩溃的情况下。
	 */
	memset(&shstatent->stats, 0, sizeof(shstatent->stats));

	pgstat_unlock_entry(entry_ref);
}

/*
 * 报告复制槽已被获取。
 *
 * 保证后续 pgstat_report_replslot() 调用时存在
 * 统计条目。
 *
 * 如果之前曾崩溃，则不存在统计数据。但如果未崩溃，
 * 这些统计就属于本槽：
 * - 这些统计不可能属于已删除的槽，否则删除函数
 *   早被调用过
 * - 若槽在停机期间被移除，
 *   pgstat_replslot_from_serialized_name_cb() 返回假就会
 *   导致统计被删除。
 */
void
pgstat_acquire_replslot(ReplicationSlot *slot)
{
	pgstat_get_entry_ref(PGSTAT_KIND_REPLSLOT, InvalidOid,
						 ReplicationSlotIndex(slot), true, NULL);
}

/*
 * 报告复制槽的删除。
 */
void
pgstat_drop_replslot(ReplicationSlot *slot)
{
	if (!pgstat_drop_entry(PGSTAT_KIND_REPLSLOT, InvalidOid,
						   ReplicationSlotIndex(slot)))
		pgstat_request_entry_refs_gc();
}

/*
 * 供 SQL 可调用的 pgstat* 函数使用的支持函数。
 * 返回复制槽统计结构的指针。
 */
PgStat_StatReplSlotEntry *
pgstat_fetch_replslot(NameData slotname)
{
	int			idx = get_replslot_index(NameStr(slotname));

	if (idx == -1)
		return NULL;

	return (PgStat_StatReplSlotEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_REPLSLOT, InvalidOid, idx);
}

void
pgstat_replslot_to_serialized_name_cb(const PgStat_HashKey *key, const PgStatShared_Common *header, NameData *name)
{
	/*
	 * 它只在停机过程的后期被调用。此时现有槽的
	 * 集合不允许改变，因此可以假定该偏移处存在
	 * 槽。
	 */
	if (!ReplicationSlotName(key->objoid, name))
		elog(ERROR, "could not find name for replication slot index %u",
			 key->objoid);
}

bool
pgstat_replslot_from_serialized_name_cb(const NameData *name, PgStat_HashKey *key)
{
	int			idx = get_replslot_index(NameStr(*name));

	/* 该槽可能已被删除 */
	if (idx == -1)
		return false;

	key->kind = PGSTAT_KIND_REPLSLOT;
	key->dboid = InvalidOid;
	key->objoid = idx;

	return true;
}

void
pgstat_replslot_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_ReplSlot *) header)->stats.stat_reset_timestamp = ts;
}

static int
get_replslot_index(const char *name)
{
	ReplicationSlot *slot;

	Assert(name != NULL);

	slot = SearchNamedReplicationSlot(name, true);

	if (!slot)
		return -1;

	return ReplicationSlotIndex(slot);
}
