/* -------------------------------------------------------------------------
 *
 * pgstat_relation.c
 *	  关系统计的实现。
 *
 * 本文件包含关系统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和
 * 各类统计的细节之间划清清楚的
 * 界限。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_relation.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/partition.h"
#include "postmaster/autovacuum.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "catalog/catalog.h"


/* pgstat 状态持久化时写入 2PC 状态文件的记录 */
typedef struct TwoPhasePgStatRecord
{
	PgStat_Counter tuples_inserted; /* 事务内插入元组 */
	PgStat_Counter tuples_updated;	/* 事务内更新元组 */
	PgStat_Counter tuples_deleted;	/* 事务内删除元组 */
	/* 截断/删除前的元组 i/u/d */
	PgStat_Counter inserted_pre_truncdrop;
	PgStat_Counter updated_pre_truncdrop;
	PgStat_Counter deleted_pre_truncdrop;
	Oid			id;				/* 表的 OID */
	bool		shared;			/* 是否为共享目录 */
	bool		truncdropped;	/* 关系是否曾被截断/删除 */
} TwoPhasePgStatRecord;


static PgStat_TableStatus *pgstat_prep_relation_pending(Oid rel_id, bool isshared);
static void add_tabstat_xact_level(PgStat_TableStatus *pgstat_info, int nest_level);
static void ensure_tabstat_xact_level(PgStat_TableStatus *pgstat_info);
static void save_truncdrop_counters(PgStat_TableXactStatus *trans, bool is_drop);
static void restore_truncdrop_counters(PgStat_TableXactStatus *trans);


/*
 * 在关系之间复制统计。用于 REINDEX CONCURRENTLY
 * 等场景。
 */
void
pgstat_copy_relation_stats(Relation dst, Relation src)
{
	PgStat_StatTabEntry *srcstats;
	PgStatShared_Relation *dstshstats;
	PgStat_EntryRef *dst_ref;

	srcstats = pgstat_fetch_stat_tabentry_ext(src->rd_rel->relisshared,
											  RelationGetRelid(src));
	if (!srcstats)
		return;

	dst_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_RELATION,
										  dst->rd_rel->relisshared ? InvalidOid : MyDatabaseId,
										  RelationGetRelid(dst),
										  false);

	dstshstats = (PgStatShared_Relation *) dst_ref->shared_stats;
	dstshstats->stats = *srcstats;

	pgstat_unlock_entry(dst_ref);
}

/*
 * 初始化 relcache 条目来计数访问统计。每当关系
 * 打开时调用。
 *
 * 假定 relcache 条目的 pgstat_info 字段由 relcache.c 在
 * 创建条目时清零；此后即为长期存活的数据。
 *
 * 这不会创建到共享内存中统计条目的引用，也不
 * 会为待写入统计分配内存。那发生在
 * pgstat_assoc_relation()
 */
void
pgstat_init_relation(Relation rel)
{
	char		relkind = rel->rd_rel->relkind;

	/*
	 * 只统计有存储的关系和分区表
	 */
	if (!RELKIND_HAS_STORAGE(relkind) && relkind != RELKIND_PARTITIONED_TABLE)
	{
		rel->pgstat_enabled = false;
		rel->pgstat_info = NULL;
		return;
	}

	if (!pgstat_track_counts)
	{
		if (rel->pgstat_info)
			pgstat_unlink_relation(rel);

		/* 完全不计数 */
		rel->pgstat_enabled = false;
		rel->pgstat_info = NULL;
		return;
	}

	rel->pgstat_enabled = true;
}

/*
 * 为收集该关系的统计做准备。
 *
 * 这确保在统计可被生成之前，我们已持有统计
 * 条目的引用。这很重要，因为其他会话中删除关
 * 系可能导致统计条目被删除，随后刷写统计时该
 * 条目还会被重建。
 *
 * 这与 pgstat_init_relation() 是分开的，因为 relcache
 * 条目被打开却从未上报统计的情况并不少见。
 */
void
pgstat_assoc_relation(Relation rel)
{
	Assert(rel->pgstat_enabled);
	Assert(rel->pgstat_info == NULL);

	/* 否则查找或建 PgStat_TableStatus 条目并更新链接 */
	rel->pgstat_info = pgstat_prep_relation_pending(RelationGetRelid(rel),
													rel->rd_rel->relisshared);

	/* 不允许将统计链接到多个 relcache 条目 */
	Assert(rel->pgstat_info->relation == NULL);

	/* 将此关系标记为所有者 */
	rel->pgstat_info->relation = rel;
}

/*
 * 断开 relcache 条目与待写入统计条目之间的相互
 * 链接。只要链接的任一端被移除都必须调用。
 */
void
pgstat_unlink_relation(Relation rel)
{
	/* 移除统计信息链接（如有） */
	if (rel->pgstat_info == NULL)
		return;

	/* 链接检查 */
	Assert(rel->pgstat_info->relation == rel);
	rel->pgstat_info->relation = NULL;
	rel->pgstat_info = NULL;
}

/*
 * 确保事务回滚时删除统计。
 */
void
pgstat_create_relation(Relation rel)
{
	pgstat_create_transactional(PGSTAT_KIND_RELATION,
								rel->rd_rel->relisshared ? InvalidOid : MyDatabaseId,
								RelationGetRelid(rel));
}

/*
 * 确保事务提交时删除统计。
 */
void
pgstat_drop_relation(Relation rel)
{
	int			nest_level = GetCurrentTransactionNestLevel();
	PgStat_TableStatus *pgstat_info;

	pgstat_drop_transactional(PGSTAT_KIND_RELATION,
							  rel->rd_rel->relisshared ? InvalidOid : MyDatabaseId,
							  RelationGetRelid(rel));

	if (!pgstat_should_count_relation(rel))
		return;

	/*
	 * 以事务方式将计数器置 0。这确保事务内对
	 * pg_stat_xact_all_tables 的访问显示为 0。
	 */
	pgstat_info = rel->pgstat_info;
	if (pgstat_info->trans &&
		pgstat_info->trans->nest_level == nest_level)
	{
		save_truncdrop_counters(pgstat_info->trans, true);
		pgstat_info->trans->tuples_inserted = 0;
		pgstat_info->trans->tuples_updated = 0;
		pgstat_info->trans->tuples_deleted = 0;
	}
}

/*
 * 报告表刚完成 VACUUM 并刷写 IO 统计。
 */
void
pgstat_report_vacuum(Oid tableoid, bool shared,
					 PgStat_Counter livetuples, PgStat_Counter deadtuples)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Relation *shtabentry;
	PgStat_StatTabEntry *tabentry;
	Oid			dboid = (shared ? InvalidOid : MyDatabaseId);
	TimestampTz ts;

	if (!pgstat_track_counts)
		return;

	/* 将数据存入表的哈希表条目。 */
	ts = GetCurrentTimestamp();

	/* 出于与 pgstat_report_autovac() 相同的原因阻塞获取锁 */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_RELATION,
											dboid, tableoid, false);

	shtabentry = (PgStatShared_Relation *) entry_ref->shared_stats;
	tabentry = &shtabentry->stats;

	tabentry->live_tuples = livetuples;
	tabentry->dead_tuples = deadtuples;

	/*
	 * 激进程度较低的 VACUUM 很可能跳过了许多页面，
	 * 但这里仍会将插入计数器清零。它目前仅用于
	 * 跟踪何时需要执行 "插入型" 自动清理，其主要
	 * 目的在于冻结新插入的元组。将其清零可能只
	 * 意味着不会再次尝试清理该表，直到插入足够
	 * 多元组触发下一轮插入型自动清理。防回卷
	 * 自动清理则会负责捕获这些持续滞留的
	 * 零星表
	 */
	tabentry->ins_since_vacuum = 0;

	if (IsAutoVacuumWorkerProcess())
	{
		tabentry->last_autovacuum_time = ts;
		tabentry->autovacuum_count++;
	}
	else
	{
		tabentry->last_vacuum_time = ts;
		tabentry->vacuum_count++;
	}

	pgstat_unlock_entry(entry_ref);

	/*
	 * 现在就刷写 IO 统计。pgstat_report_stat() 会刷写
	 * IO 统计，但要到整个自动清理周期结束之后（这
	 * 可能清理大量关系）或直到 VACUUM 命令处理完所
	 * 有表并提交。
	 */
	pgstat_flush_io(false);
}

/*
 * 报告表刚完成 ANALYZE 并刷写 IO 统计。
 *
 * 调用者必须提供新的存活和死亡元组估算值，以
 * 及是否重置 mod_since_analyze 计数器的标志。
 */
void
pgstat_report_analyze(Relation rel,
					  PgStat_Counter livetuples, PgStat_Counter deadtuples,
					  bool resetcounter)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Relation *shtabentry;
	PgStat_StatTabEntry *tabentry;
	Oid			dboid = (rel->rd_rel->relisshared ? InvalidOid : MyDatabaseId);

	if (!pgstat_track_counts)
		return;

	/*
	 * 与 VACUUM 不同，ANALYZE 可能运行在已经向目标表
	 * 插入和/或删除了行的事务中。ANALYZE 会分别把
	 * 这些行计为存活或死亡。由于我们会在事务结束
	 * 时上报这些行的计数，所以应当从现在所做的更
	 * 新中减去这些计数，否则提交后它们将被重复
	 * 计入。（这种做法还确保即使我们回滚而非提
	 * 交，共享统计条目最终也会得到正确的数
	 * 值）。
	 *
	 * 不过对分区表不必浪费时间。
	 */
	if (pgstat_should_count_relation(rel) &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		PgStat_TableXactStatus *trans;

		for (trans = rel->pgstat_info->trans; trans; trans = trans->upper)
		{
			livetuples -= trans->tuples_inserted - trans->tuples_deleted;
			deadtuples -= trans->tuples_updated + trans->tuples_deleted;
		}
		/* 也统计已被中止子事务插入的内容 */
		deadtuples -= rel->pgstat_info->counts.delta_dead_tuples;
		/* ANALYZE 的计数是估算值，可能已出现下溢 */
		livetuples = Max(livetuples, 0);
		deadtuples = Max(deadtuples, 0);
	}

	/* 出于与 pgstat_report_autovac() 相同的原因阻塞获取锁 */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_RELATION, dboid,
											RelationGetRelid(rel),
											false);
	/* 被访问期间不会被删除 */
	Assert(entry_ref != NULL && entry_ref->shared_stats != NULL);

	shtabentry = (PgStatShared_Relation *) entry_ref->shared_stats;
	tabentry = &shtabentry->stats;

	tabentry->live_tuples = livetuples;
	tabentry->dead_tuples = deadtuples;

	/*
	 * 若有要求，将 mod_since_analyze 重置为零。这会
	 * 忘掉 ANALYZE 进行期间已提交的变更，但我们
	 * 没有好的方法估算这些变更的数量。
	 */
	if (resetcounter)
		tabentry->mod_since_analyze = 0;

	if (IsAutoVacuumWorkerProcess())
	{
		tabentry->last_autoanalyze_time = GetCurrentTimestamp();
		tabentry->autoanalyze_count++;
	}
	else
	{
		tabentry->last_analyze_time = GetCurrentTimestamp();
		tabentry->analyze_count++;
	}

	pgstat_unlock_entry(entry_ref);

	/* 见 pgstat_report_vacuum() */
	pgstat_flush_io(false);
}

/*
 * 统计 n 个元组的插入
 */
void
pgstat_count_heap_insert(Relation rel, PgStat_Counter n)
{
	if (pgstat_should_count_relation(rel))
	{
		PgStat_TableStatus *pgstat_info = rel->pgstat_info;

		ensure_tabstat_xact_level(pgstat_info);
		pgstat_info->trans->tuples_inserted += n;
	}
}

/*
 * 统计元组更新
 */
void
pgstat_count_heap_update(Relation rel, bool hot, bool newpage)
{
	Assert(!(hot && newpage));

	if (pgstat_should_count_relation(rel))
	{
		PgStat_TableStatus *pgstat_info = rel->pgstat_info;

		ensure_tabstat_xact_level(pgstat_info);
		pgstat_info->trans->tuples_updated++;

		/*
		 * tuples_hot_updated 与 tuples_newpage_updated 计数
		 * 非事务性，直接推进即可
		 */
		if (hot)
			pgstat_info->counts.tuples_hot_updated++;
		else if (newpage)
			pgstat_info->counts.tuples_newpage_updated++;
	}
}

/*
 * 统计元组删除
 */
void
pgstat_count_heap_delete(Relation rel)
{
	if (pgstat_should_count_relation(rel))
	{
		PgStat_TableStatus *pgstat_info = rel->pgstat_info;

		ensure_tabstat_xact_level(pgstat_info);
		pgstat_info->trans->tuples_deleted++;
	}
}

/*
 * 因截断而更新元组计数器
 */
void
pgstat_count_truncate(Relation rel)
{
	if (pgstat_should_count_relation(rel))
	{
		PgStat_TableStatus *pgstat_info = rel->pgstat_info;

		ensure_tabstat_xact_level(pgstat_info);
		save_truncdrop_counters(pgstat_info->trans, false);
		pgstat_info->trans->tuples_inserted = 0;
		pgstat_info->trans->tuples_updated = 0;
		pgstat_info->trans->tuples_deleted = 0;
	}
}

/*
 * 更新死亡元组计数
 *
 * 其语义是我们上报的是 "delta" 死亡元组的非事务
 * 性恢复；因此 delta_dead_tuples 是减少而非增加，
 * 并且变更直接进入每表计数器，而不进入事务性
 * 状态。
 */
void
pgstat_update_heap_dead_tuples(Relation rel, int delta)
{
	if (pgstat_should_count_relation(rel))
	{
		PgStat_TableStatus *pgstat_info = rel->pgstat_info;

		pgstat_info->counts.delta_dead_tuples -= delta;
	}
}

/*
 * SQL 可调用 pgstat* 函数的支持函数。返回
 * 某个表的统计信息或 NULL。NULL 并不代表
 * 该表不存在，只是没有统计信息，因此调用者
 * 最好还是报告 ZERO。
 */
PgStat_StatTabEntry *
pgstat_fetch_stat_tabentry(Oid relid)
{
	return pgstat_fetch_stat_tabentry_ext(IsSharedRelation(relid), relid);
}

/*
 * pgstat_fetch_stat_tabentry() 的更高效版本，允许指定
 * 要访问的表是否为共享关系。
 */
PgStat_StatTabEntry *
pgstat_fetch_stat_tabentry_ext(bool shared, Oid reloid)
{
	Oid			dboid = (shared ? InvalidOid : MyDatabaseId);

	return (PgStat_StatTabEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_RELATION, dboid, reloid);
}

/*
 * 查找 rel 已有的 PgStat_TableStatus 条目
 *
 * 在当前数据库中查找 rel_id 已有的 PgStat_TableStatus
 * 条目。找不到则尝试从共享表查找。
 *
 * 若找不到条目则返回 NULL，不新建条目
 */
PgStat_TableStatus *
find_tabstat_entry(Oid rel_id)
{
	PgStat_EntryRef *entry_ref;

	entry_ref = pgstat_fetch_pending_entry(PGSTAT_KIND_RELATION, MyDatabaseId, rel_id);
	if (!entry_ref)
		entry_ref = pgstat_fetch_pending_entry(PGSTAT_KIND_RELATION, InvalidOid, rel_id);

	if (entry_ref)
		return entry_ref->pending;
	return NULL;
}

/*
 * 执行关系统计的事务收尾工作。AtEOXact_PgStat 的
 * 辅助函数。
 *
 * 将事务性的插入/更新计数转移到基础 tabstat 条目中。
 * 不必费心释放任何事务性状态，因为它们全部位于
 * TopTransactionContext 中，会自行消失。
 */
void
AtEOXact_PgStat_Relations(PgStat_SubXactStatus *xact_state, bool isCommit)
{
	PgStat_TableXactStatus *trans;

	for (trans = xact_state->first; trans != NULL; trans = trans->next)
	{
		PgStat_TableStatus *tabstat;

		Assert(trans->nest_level == 1);
		Assert(trans->upper == NULL);
		tabstat = trans->parent;
		Assert(tabstat->trans == trans);
		/* 事务回滚时恢复截断/删除前的统计（如有） */
		if (!isCommit)
			restore_truncdrop_counters(trans);
		/* 无论提交/回滚都统计尝试过的动作 */
		tabstat->counts.tuples_inserted += trans->tuples_inserted;
		tabstat->counts.tuples_updated += trans->tuples_updated;
		tabstat->counts.tuples_deleted += trans->tuples_deleted;
		if (isCommit)
		{
			tabstat->counts.truncdropped = trans->truncdropped;
			if (trans->truncdropped)
			{
				/* 忘掉后端迄今见到的存活/死亡统计 */
				tabstat->counts.delta_live_tuples = 0;
				tabstat->counts.delta_dead_tuples = 0;
			}
			/* 插入加存活元组，删除减一个 */
			tabstat->counts.delta_live_tuples +=
				trans->tuples_inserted - trans->tuples_deleted;
			/* 更新和删除各产生一个死亡元组 */
			tabstat->counts.delta_dead_tuples +=
				trans->tuples_updated + trans->tuples_deleted;
			/* 插入、更新、删除各计为一个变更事件 */
			tabstat->counts.changed_tuples +=
				trans->tuples_inserted + trans->tuples_updated +
				trans->tuples_deleted;
		}
		else
		{
			/* 插入的元组已死，删除的元组不受影响 */
			tabstat->counts.delta_dead_tuples +=
				trans->tuples_inserted + trans->tuples_updated;
			/* 已回滚事务不产生 changed_tuple 事件 */
		}
		tabstat->trans = NULL;
	}
}

/*
 * 执行关系统计的子事务收尾工作。AtEOSubXact_PgStat 的
 * 辅助函数。
 *
 * 将事务性的插入/更新计数转移到上一层子事务
 * 状态中。
 */
void
AtEOSubXact_PgStat_Relations(PgStat_SubXactStatus *xact_state, bool isCommit, int nestDepth)
{
	PgStat_TableXactStatus *trans;
	PgStat_TableXactStatus *next_trans;

	for (trans = xact_state->first; trans != NULL; trans = next_trans)
	{
		PgStat_TableStatus *tabstat;

		next_trans = trans->next;
		Assert(trans->nest_level == nestDepth);
		tabstat = trans->parent;
		Assert(tabstat->trans == trans);

		if (isCommit)
		{
			if (trans->upper && trans->upper->nest_level == nestDepth - 1)
			{
				if (trans->truncdropped)
				{
					/* 将截断/删除状态向上传递一层 */
					save_truncdrop_counters(trans->upper, false);
					/* 以本层统计替换上层 */
					trans->upper->tuples_inserted = trans->tuples_inserted;
					trans->upper->tuples_updated = trans->tuples_updated;
					trans->upper->tuples_deleted = trans->tuples_deleted;
				}
				else
				{
					trans->upper->tuples_inserted += trans->tuples_inserted;
					trans->upper->tuples_updated += trans->tuples_updated;
					trans->upper->tuples_deleted += trans->tuples_deleted;
				}
				tabstat->trans = trans->upper;
				pfree(trans);
			}
			else
			{
				/*
				 * 若无直接父状态，可以直接复用该记录，
				 * 而不必经历 palloc/pfree 的折腾（这可行，
				 * 它们都在 TopTransactionContext 中）。但必须
				 * 将其重新链接进父层级，这可能意味着
				 * 要向 pgStatXactStack 中推入
				 * 一个新条目
				 */
				PgStat_SubXactStatus *upper_xact_state;

				upper_xact_state = pgstat_get_xact_stack_level(nestDepth - 1);
				trans->next = upper_xact_state->first;
				upper_xact_state->first = trans;
				trans->nest_level = nestDepth - 1;
			}
		}
		else
		{
			/*
			 * 回滚时更新顶层 tabstat 计数，然后忘掉
			 * 子事务
			 */

			/* 先恢复被截断/删除抹去的值 */
			restore_truncdrop_counters(trans);
			/* 无论提交/回滚都统计尝试过的动作 */
			tabstat->counts.tuples_inserted += trans->tuples_inserted;
			tabstat->counts.tuples_updated += trans->tuples_updated;
			tabstat->counts.tuples_deleted += trans->tuples_deleted;
			/* 插入的元组已死，删除的元组不受影响 */
			tabstat->counts.delta_dead_tuples +=
				trans->tuples_inserted + trans->tuples_updated;
			tabstat->trans = trans->upper;
			pfree(trans);
		}
	}
}

/*
 * 为所有待写入的依赖事务的关系统计生成 2PC
 * 记录
 */
void
AtPrepare_PgStat_Relations(PgStat_SubXactStatus *xact_state)
{
	PgStat_TableXactStatus *trans;

	for (trans = xact_state->first; trans != NULL; trans = trans->next)
	{
		PgStat_TableStatus *tabstat PG_USED_FOR_ASSERTS_ONLY;
		TwoPhasePgStatRecord record;

		Assert(trans->nest_level == 1);
		Assert(trans->upper == NULL);
		tabstat = trans->parent;
		Assert(tabstat->trans == trans);

		record.tuples_inserted = trans->tuples_inserted;
		record.tuples_updated = trans->tuples_updated;
		record.tuples_deleted = trans->tuples_deleted;
		record.inserted_pre_truncdrop = trans->inserted_pre_truncdrop;
		record.updated_pre_truncdrop = trans->updated_pre_truncdrop;
		record.deleted_pre_truncdrop = trans->deleted_pre_truncdrop;
		record.id = tabstat->id;
		record.shared = tabstat->shared;
		record.truncdropped = trans->truncdropped;

		RegisterTwoPhaseRecord(TWOPHASE_RM_PGSTAT_ID, 0,
							   &record, sizeof(TwoPhasePgStatRecord));
	}
}

/*
 * 这里只需将事务统计状态与非事务状态解除链接。
 * 非事务的动作计数会立即上报给统计系统，而对
 * 存活和死亡元组计数的影响则保存在 2PC 状态文
 * 件中。
 *
 * 注意：PREPARE 期间不调用 AtEOXact_PgStat_Relations。
 */
void
PostPrepare_PgStat_Relations(PgStat_SubXactStatus *xact_state)
{
	PgStat_TableXactStatus *trans;

	for (trans = xact_state->first; trans != NULL; trans = trans->next)
	{
		PgStat_TableStatus *tabstat;

		tabstat = trans->parent;
		tabstat->trans = NULL;
	}
}

/*
 * COMMIT PREPARED 情形的 2PC 处理例程。
 *
 * 将保存计数载入本地 pgstats 状态。
 */
void
pgstat_twophase_postcommit(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	TwoPhasePgStatRecord *rec = (TwoPhasePgStatRecord *) recdata;
	PgStat_TableStatus *pgstat_info;

	/* 查找或创建该关系的 tabstat 条目 */
	pgstat_info = pgstat_prep_relation_pending(rec->id, rec->shared);

	/* 同 AtEOXact_PgStat 计算，提交情形 */
	pgstat_info->counts.tuples_inserted += rec->tuples_inserted;
	pgstat_info->counts.tuples_updated += rec->tuples_updated;
	pgstat_info->counts.tuples_deleted += rec->tuples_deleted;
	pgstat_info->counts.truncdropped = rec->truncdropped;
	if (rec->truncdropped)
	{
		/* 忘掉后端迄今见到的存活/死亡统计 */
		pgstat_info->counts.delta_live_tuples = 0;
		pgstat_info->counts.delta_dead_tuples = 0;
	}
	pgstat_info->counts.delta_live_tuples +=
		rec->tuples_inserted - rec->tuples_deleted;
	pgstat_info->counts.delta_dead_tuples +=
		rec->tuples_updated + rec->tuples_deleted;
	pgstat_info->counts.changed_tuples +=
		rec->tuples_inserted + rec->tuples_updated +
		rec->tuples_deleted;
}

/*
 * ROLLBACK PREPARED 情形的 2PC 处理例程。
 *
 * 把保存的计数载入本地 pgstats 状态，但视为已
 * 回滚。
 */
void
pgstat_twophase_postabort(TransactionId xid, uint16 info,
						  void *recdata, uint32 len)
{
	TwoPhasePgStatRecord *rec = (TwoPhasePgStatRecord *) recdata;
	PgStat_TableStatus *pgstat_info;

	/* 查找或创建该关系的 tabstat 条目 */
	pgstat_info = pgstat_prep_relation_pending(rec->id, rec->shared);

	/* 同 AtEOXact_PgStat 计算，回滚情形 */
	if (rec->truncdropped)
	{
		rec->tuples_inserted = rec->inserted_pre_truncdrop;
		rec->tuples_updated = rec->updated_pre_truncdrop;
		rec->tuples_deleted = rec->deleted_pre_truncdrop;
	}
	pgstat_info->counts.tuples_inserted += rec->tuples_inserted;
	pgstat_info->counts.tuples_updated += rec->tuples_updated;
	pgstat_info->counts.tuples_deleted += rec->tuples_deleted;
	pgstat_info->counts.delta_dead_tuples +=
		rec->tuples_inserted + rec->tuples_updated;
}

/*
 * 刷出该条目的待写入统计
 *
 * 若 nowait 为 true 且无法立即获得锁，则返回 false，
 * 不刷写该条目。否则返回 true。
 *
 * 刷写成功时，部分统计会复制到对应的待写入数
 * 据库统计条目。
 */
bool
pgstat_relation_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	static const PgStat_TableCounts all_zeroes;
	Oid			dboid;
	PgStat_TableStatus *lstats; /* 待写统计条目 */
	PgStatShared_Relation *shtabstats;
	PgStat_StatTabEntry *tabentry;	/* 共享统计的表条目 */
	PgStat_StatDBEntry *dbentry;	/* 待写数据库条目 */

	dboid = entry_ref->shared_entry->key.dboid;
	lstats = (PgStat_TableStatus *) entry_ref->pending;
	shtabstats = (PgStatShared_Relation *) entry_ref->shared_stats;

	/*
	 * 忽略未积累任何实际计数的条目，例如规划器
	 * 打开但未使用的索引。
	 */
	if (memcmp(&lstats->counts, &all_zeroes,
			   sizeof(PgStat_TableCounts)) == 0)
	{
		return true;
	}

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	/* 将值加到共享条目上。 */
	tabentry = &shtabstats->stats;

	tabentry->numscans += lstats->counts.numscans;
	if (lstats->counts.numscans)
	{
		TimestampTz t = GetCurrentTransactionStopTimestamp();

		if (t > tabentry->lastscan)
			tabentry->lastscan = t;
	}
	tabentry->tuples_returned += lstats->counts.tuples_returned;
	tabentry->tuples_fetched += lstats->counts.tuples_fetched;
	tabentry->tuples_inserted += lstats->counts.tuples_inserted;
	tabentry->tuples_updated += lstats->counts.tuples_updated;
	tabentry->tuples_deleted += lstats->counts.tuples_deleted;
	tabentry->tuples_hot_updated += lstats->counts.tuples_hot_updated;
	tabentry->tuples_newpage_updated += lstats->counts.tuples_newpage_updated;

	/*
	 * 若表被截断/删除，先重置存活/死亡计数器。
	 */
	if (lstats->counts.truncdropped)
	{
		tabentry->live_tuples = 0;
		tabentry->dead_tuples = 0;
		tabentry->ins_since_vacuum = 0;
	}

	tabentry->live_tuples += lstats->counts.delta_live_tuples;
	tabentry->dead_tuples += lstats->counts.delta_dead_tuples;
	tabentry->mod_since_analyze += lstats->counts.changed_tuples;
	tabentry->ins_since_vacuum += lstats->counts.tuples_inserted;
	tabentry->blocks_fetched += lstats->counts.blocks_fetched;
	tabentry->blocks_hit += lstats->counts.blocks_hit;

	/* 若 delta_live_tuples 为负则钳制 live_tuples */
	tabentry->live_tuples = Max(tabentry->live_tuples, 0);
	/* dead_tuples 同理 */
	tabentry->dead_tuples = Max(tabentry->dead_tuples, 0);

	pgstat_unlock_entry(entry_ref);

	/* 条目已成功刷写，将同样的值加到数据库统计 */
	dbentry = pgstat_prep_database_pending(dboid);
	dbentry->tuples_returned += lstats->counts.tuples_returned;
	dbentry->tuples_fetched += lstats->counts.tuples_fetched;
	dbentry->tuples_inserted += lstats->counts.tuples_inserted;
	dbentry->tuples_updated += lstats->counts.tuples_updated;
	dbentry->tuples_deleted += lstats->counts.tuples_deleted;
	dbentry->blocks_fetched += lstats->counts.blocks_fetched;
	dbentry->blocks_hit += lstats->counts.blocks_hit;

	return true;
}

void
pgstat_relation_delete_pending_cb(PgStat_EntryRef *entry_ref)
{
	PgStat_TableStatus *pending = (PgStat_TableStatus *) entry_ref->pending;

	if (pending->relation)
		pgstat_unlink_relation(pending->relation);
}

/*
 * 为 rel 查找或创建 PgStat_TableStatus 条目，不存在时
 * 创建并初始化。
 */
static PgStat_TableStatus *
pgstat_prep_relation_pending(Oid rel_id, bool isshared)
{
	PgStat_EntryRef *entry_ref;
	PgStat_TableStatus *pending;

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_RELATION,
										  isshared ? InvalidOid : MyDatabaseId,
										  rel_id, NULL);
	pending = entry_ref->pending;
	pending->id = rel_id;
	pending->shared = isshared;

	return pending;
}

/*
 * 添加新的（子）事务状态记录
 */
static void
add_tabstat_xact_level(PgStat_TableStatus *pgstat_info, int nest_level)
{
	PgStat_SubXactStatus *xact_state;
	PgStat_TableXactStatus *trans;

	/*
	 * 若这是当前嵌套层级第一个要修改的关系，我们
	 * 首先必须压入事务栈条目。
	 */
	xact_state = pgstat_get_xact_stack_level(nest_level);

	/* 现在创建每表栈条目 */
	trans = (PgStat_TableXactStatus *)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(PgStat_TableXactStatus));
	trans->nest_level = nest_level;
	trans->upper = pgstat_info->trans;
	trans->parent = pgstat_info;
	trans->next = xact_state->first;
	xact_state->first = trans;
	pgstat_info->trans = trans;
}

/*
 * 需要时添加新（子）事务记录。
 */
static void
ensure_tabstat_xact_level(PgStat_TableStatus *pgstat_info)
{
	int			nest_level = GetCurrentTransactionNestLevel();

	if (pgstat_info->trans == NULL ||
		pgstat_info->trans->nest_level != nest_level)
		add_tabstat_xact_level(pgstat_info, nest_level);
}

/*
 * 每当表被截断/删除时，我们保存其 i/u/d 计数器，
 * 以便清零；若执行截断/删除的（子）事务之后回
 * 滚，这些计数器可恢复为保存的（截断/删除前）
 * 值。
 *
 * 注意对截断而言，我们在每个特定子事务层级只
 * 做一次。
 */
static void
save_truncdrop_counters(PgStat_TableXactStatus *trans, bool is_drop)
{
	if (!trans->truncdropped || is_drop)
	{
		trans->inserted_pre_truncdrop = trans->tuples_inserted;
		trans->updated_pre_truncdrop = trans->tuples_updated;
		trans->deleted_pre_truncdrop = trans->tuples_deleted;
		trans->truncdropped = true;
	}
}

/*
 * 截断回滚时恢复计数器
 */
static void
restore_truncdrop_counters(PgStat_TableXactStatus *trans)
{
	if (trans->truncdropped)
	{
		trans->tuples_inserted = trans->inserted_pre_truncdrop;
		trans->tuples_updated = trans->updated_pre_truncdrop;
		trans->tuples_deleted = trans->deleted_pre_truncdrop;
	}
}
