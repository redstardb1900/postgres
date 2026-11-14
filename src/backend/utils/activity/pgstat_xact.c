/* -------------------------------------------------------------------------
 *
 * pgstat_xact.c
 *	  累计统计系统的事务集成。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_xact.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


typedef struct PgStat_PendingDroppedStatsItem
{
	xl_xact_stats_item item;
	bool		is_create;
	dlist_node	node;
} PgStat_PendingDroppedStatsItem;


static void AtEOXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state, bool isCommit);
static void AtEOSubXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state,
											bool isCommit, int nestDepth);

static PgStat_SubXactStatus *pgStatXactStack = NULL;


/*
 * 由 access/transam/xact.c 在顶层事务提交/回滚时调用。
 */
void
AtEOXact_PgStat(bool isCommit, bool parallel)
{
	PgStat_SubXactStatus *xact_state;

	AtEOXact_PgStat_Database(isCommit, parallel);

	/* 处理事务性统计信息 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		AtEOXact_PgStat_Relations(xact_state, isCommit);
		AtEOXact_PgStat_DroppedStats(xact_state, isCommit);
	}
	pgStatXactStack = NULL;

	/* 确保丢弃任何统计快照 */
	pgstat_clear_snapshot();
}

/*
 * 提交时，删除事务中删除对象的统计。回滚时，删
 * 除事务中创建对象的统计。
 */
static void
AtEOXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state, bool isCommit)
{
	dlist_mutable_iter iter;
	int			not_freed_count = 0;

	if (dclist_count(&xact_state->pending_drops) == 0)
		return;

	dclist_foreach_modify(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);
		xl_xact_stats_item *it = &pending->item;

		if (isCommit && !pending->is_create)
		{
			/*
			 * 删除对象的事务已提交。统计也一并删
			 * 除
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, it->objoid))
				not_freed_count++;
		}
		else if (!isCommit && pending->is_create)
		{
			/*
			 * 创建对象的事务已回滚。删除与该对象
			 * 相关联的统计。
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, it->objoid))
				not_freed_count++;
		}

		dclist_delete_from(&xact_state->pending_drops, &pending->node);
		pfree(pending);
	}

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * 由 access/transam/xact.c 在子事务提交/回滚时调用
 */
void
AtEOSubXact_PgStat(bool isCommit, int nestDepth)
{
	PgStat_SubXactStatus *xact_state;

	/* 将子事务的事务性统计合并到父事务 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL &&
		xact_state->nest_level >= nestDepth)
	{
		/* 立即解除 xact_state 与栈的链接，简化复用情形 */
		pgStatXactStack = xact_state->prev;

		AtEOSubXact_PgStat_Relations(xact_state, isCommit, nestDepth);
		AtEOSubXact_PgStat_DroppedStats(xact_state, isCommit, nestDepth);

		pfree(xact_state);
	}
}

/*
 * 类似 AtEOXact_PgStat_DroppedStats()，但用于子事务。
 */
static void
AtEOSubXact_PgStat_DroppedStats(PgStat_SubXactStatus *xact_state,
								bool isCommit, int nestDepth)
{
	PgStat_SubXactStatus *parent_xact_state;
	dlist_mutable_iter iter;
	int			not_freed_count = 0;

	if (dclist_count(&xact_state->pending_drops) == 0)
		return;

	parent_xact_state = pgstat_get_xact_stack_level(nestDepth - 1);

	dclist_foreach_modify(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);
		xl_xact_stats_item *it = &pending->item;

		dclist_delete_from(&xact_state->pending_drops, &pending->node);

		if (!isCommit && pending->is_create)
		{
			/*
			 * 创建新统计对象的子事务已回滚。删除该统计
			 * 对象。
			 */
			if (!pgstat_drop_entry(it->kind, it->dboid, it->objoid))
				not_freed_count++;
			pfree(pending);
		}
		else if (isCommit)
		{
			/*
			 * 删除统计对象的子事务已提交。还无法移除
			 * 统计对象，外层事务仍可能回滚。将其传
			 * 递给父事务。
			 */
			dclist_push_tail(&parent_xact_state->pending_drops, &pending->node);
		}
		else
		{
			pfree(pending);
		}
	}

	Assert(dclist_count(&xact_state->pending_drops) == 0);
	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * 在 2PC 事务预备时保存事务性统计状态。
 */
void
AtPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		AtPrepare_PgStat_Relations(xact_state);
	}
}

/*
 * 在 PREPARE 成功后清理。
 *
 * 注意：PREPARE 期间不调用 AtEOXact_PgStat。
 */
void
PostPrepare_PgStat(void)
{
	PgStat_SubXactStatus *xact_state;

	/*
	 * 不必费心释放任何事务性状态，因为它们全部
	 * 在 TopTransactionContext 中会自行消失。
	 */
	xact_state = pgStatXactStack;
	if (xact_state != NULL)
	{
		Assert(xact_state->nest_level == 1);
		Assert(xact_state->prev == NULL);

		PostPrepare_PgStat_Relations(xact_state);
	}
	pgStatXactStack = NULL;

	/* 确保丢弃任何统计快照 */
	pgstat_clear_snapshot();
}

/*
 * 确保给定 nest_level 的（子）事务栈条目存在，缺失则
 * 添加。
 */
PgStat_SubXactStatus *
pgstat_get_xact_stack_level(int nest_level)
{
	PgStat_SubXactStatus *xact_state;

	xact_state = pgStatXactStack;
	if (xact_state == NULL || xact_state->nest_level != nest_level)
	{
		xact_state = (PgStat_SubXactStatus *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(PgStat_SubXactStatus));
		dclist_init(&xact_state->pending_drops);
		xact_state->nest_level = nest_level;
		xact_state->prev = pgStatXactStack;
		xact_state->first = NULL;
		pgStatXactStack = xact_state;
	}
	return xact_state;
}

/*
 * 获取需要在提交 / 回滚时删除的统计项。
 *
 * 提交时，返回在事务中删除的对象的统计项；
 * 回滚时，返回新建对象的统计
 * 项。
 *
 * 由 COMMIT / ABORT 与 2PC PREPARE 处理在构建各自
 * 的 WAL 记录时使用，以确保崩溃时 / 备机上也会删除
 * 统计。
 *
 * 项列表在 CurrentMemoryContext 中分配，必须由调用
 * 者释放（直接或经上下文重置）。
 */
int
pgstat_get_transactional_drops(bool isCommit, xl_xact_stats_item **items)
{
	PgStat_SubXactStatus *xact_state = pgStatXactStack;
	int			nitems = 0;
	dlist_iter	iter;

	if (xact_state == NULL)
		return 0;

	/*
	 * 预期只会在子事务回滚（有 WAL 记录）时被调
	 * 用，而子事务提交（无记录）则不会。
	 */
	Assert(!isCommit || xact_state->nest_level == 1);
	Assert(!isCommit || xact_state->prev == NULL);

	*items = palloc(dclist_count(&xact_state->pending_drops)
					* sizeof(xl_xact_stats_item));

	dclist_foreach(iter, &xact_state->pending_drops)
	{
		PgStat_PendingDroppedStatsItem *pending =
			dclist_container(PgStat_PendingDroppedStatsItem, node, iter.cur);

		if (isCommit && pending->is_create)
			continue;
		if (!isCommit && !pending->is_create)
			continue;

		Assert(nitems < dclist_count(&xact_state->pending_drops));
		(*items)[nitems++] = pending->item;
	}

	return nitems;
}

/*
 * 执行提交后计划的删除。调用自 xact_redo_commit() /
 * xact_redo_abort()（恢复期间）以及 FinishPreparedTransaction()
 * （正常 2PC PREPARED 提交/中止处理）。
 */
void
pgstat_execute_transactional_drops(int ndrops, struct xl_xact_stats_item *items, bool is_redo)
{
	int			not_freed_count = 0;

	if (ndrops == 0)
		return;

	for (int i = 0; i < ndrops; i++)
	{
		xl_xact_stats_item *it = &items[i];

		if (!pgstat_drop_entry(it->kind, it->dboid, it->objoid))
			not_freed_count++;
	}

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

static void
create_drop_transactional_internal(PgStat_Kind kind, Oid dboid, Oid objoid, bool is_create)
{
	int			nest_level = GetCurrentTransactionNestLevel();
	PgStat_SubXactStatus *xact_state;
	PgStat_PendingDroppedStatsItem *drop = (PgStat_PendingDroppedStatsItem *)
		MemoryContextAlloc(TopTransactionContext, sizeof(PgStat_PendingDroppedStatsItem));

	xact_state = pgstat_get_xact_stack_level(nest_level);

	drop->is_create = is_create;
	drop->item.kind = kind;
	drop->item.dboid = dboid;
	drop->item.objoid = objoid;

	dclist_push_tail(&xact_state->pending_drops, &drop->node);
}

/*
 * 以事务性方式为新创建的数据库对象创建统计条
 * 目。
 *
 * 即若当前（子）事务回滚，该统计条目也将被删
 * 除。
 */
void
pgstat_create_transactional(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	if (pgstat_get_entry_ref(kind, dboid, objoid, false, NULL))
	{
		ereport(WARNING,
				errmsg("resetting existing statistics for kind %s, db=%u, oid=%u",
					   (pgstat_get_kind_info(kind))->name, dboid, objoid));

		pgstat_reset(kind, dboid, objoid);
	}

	create_drop_transactional_internal(kind, dboid, objoid, /* 创建 */ true);
}

/*
 * 以事务性方式删除刚删除的数据库对象的统计条
 * 目。
 *
 * 即若当前（子）事务回滚，该统计条目仍将存
 * 活。
 */
void
pgstat_drop_transactional(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	create_drop_transactional_internal(kind, dboid, objoid, /* 创建 */ false);
}
