/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER一个表，基于指定索引。现在也用于 VACUUM FULL。
 *
 * Paul Brown 的原始实现已几乎没剩什么了...
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/*
 * 该结构体用于传递要聚簇的表的信息。
 * 我们需要它是因为在没有指定特定表/索引对时，
 * 可以将它们组成列表。
 */
typedef struct
{
	Oid			tableOid;
	Oid			indexOid;
} RelToCluster;


static void cluster_multiple_rels(List *rtcs, ClusterParams *params);
static void rebuild_relation(Relation OldHeap, Oid indexOid, bool verbose);
static void copy_table_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex,
							bool verbose, bool *pSwapToastByContent,
							TransactionId *pFreezeXid, MultiXactId *pCutoffMulti);
static List *get_tables_to_cluster(MemoryContext cluster_context);
static List *get_tables_to_cluster_partitioned(MemoryContext cluster_context,
											   Oid indexOid);


/*---------------------------------------------------------------------------
 * 这个 CLUSTER 代码允许同时聚簇多个表。
 * 正因为如此，我们不能在单个事务中运行所有操作，
 * 否则我们将被迫同时获取所有被聚簇表的排他锁，
 * --- 很可能导致死锁。
 *
 * 为了解决这个问题，我们采用了与 VACUUM 代码类似的策略，
 * 在单独的事务中聚簇每个关系。为此，我们需要：
 *	- 提供一个单独的内存上下文，以便我们可以以
 *	   跨事务的方式传递信息
 *	- 每次聚簇新关系时开启一个新事务
 *	- 检查待聚簇关系信息的有效性，
 *	   因为有人可能在我们背后删除了关系，
 *	   或者已经基于不同的索引聚簇了它
 *	- 结束事务
 *
 * 单关系情况不会有这样的额外开销。
 *
 * 我们也允许指定关系时不指定索引。在那种情况下，
 * 将查找 indisclustered 位，如果没有索引设置该位，
 * 则抛出 ERROR。
 *-------------------------------------
 *--------------------------------------
 */
void
cluster(ParseState *pstate, ClusterStmt *stmt, bool isTopLevel)
{
	ListCell   *lc;
	ClusterParams params = {0};
	bool		verbose = false;
	Relation	rel = NULL;
	Oid			indexOid = InvalidOid;
	MemoryContext cluster_context;
	List	   *rtcs;

	/* 解析选项列表 */
	foreach(lc, stmt->params)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized CLUSTER option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	params.options = (verbose ? CLUOPT_VERBOSE : 0);

	if (stmt->relation != NULL)
	{
		/* 这是单关系情况。 */
		Oid			tableOid;

		/*
		 * 查找、加锁并检查表的权限。
		 * 我们立即获取 AccessExclusiveLock，
		 * 以避免单事务情况下的锁升级风险。
		 */
		tableOid = RangeVarGetRelidExtended(stmt->relation,
											AccessExclusiveLock,
											0,
											RangeVarCallbackOwnsTable, NULL);
		rel = table_open(tableOid, NoLock);

		/*
		 * 拒绝聚簇远程临时表...它们的本地缓冲区
		 * 管理器无法处理。
		 */
		if (RELATION_IS_OTHER_TEMP(rel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster temporary tables of other sessions")));

		if (stmt->indexname == NULL)
		{
			ListCell   *index;

			/* 我们需要找到设置了 indisclustered 的索引。 */
			foreach(index, RelationGetIndexList(rel))
			{
				indexOid = lfirst_oid(index);
				if (get_index_isclustered(indexOid))
					break;
				indexOid = InvalidOid;
			}

			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("there is no previously clustered index for table \"%s\"",
								stmt->relation->relname)));
		}
		else
		{
			/*
			 * 索引应位于与关系相同的
			 * 命名空间中。
			 */
			indexOid = get_relname_relid(stmt->indexname,
										 rel->rd_rel->relnamespace);
			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("index \"%s\" for table \"%s\" does not exist",
								stmt->indexname, stmt->relation->relname)));
		}

		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			/* 关闭关系，保持锁直到提交 */
			table_close(rel, NoLock);

			/* 执行工作。 */
			cluster_rel(tableOid, indexOid, &params);

			return;
		}
	}

	/*
	 * 到这里，我们知道我们处于多表情况。
	 * 为了避免持有锁的时间过长，我们想要在单独的
	 * 事务中处理每个表。这迫使我们不允许在用户
	 * 事务块内运行。
	 */
	PreventInTransactionBlock(isTopLevel, "CLUSTER");

	/* 此外，我们需要一个内存上下文来保存关系列表 */
	cluster_context = AllocSetContextCreate(PortalContext,
											"Cluster",
											ALLOCSET_DEFAULT_SIZES);

	/*
	 * 我们要么在处理分区表，要么完全没有指定
	 * 表名。无论哪种情况，都会获取要处理的
	 * 关系列表。
	 *
	 * 在前者情况下，必须指定索引名，所以我们不需要
	 * 重新检查它的"indisclustered"位，但我们必须检查它
	 * 是我们可以用来聚簇的索引。在后者情况下，我们
	 * 设置选项位以验证 indisclustered。
	 *
	 * 在所有情况下，重新检查关系本身在这里都是必要的。
	 */
	params.options |= CLUOPT_RECHECK;
	if (rel != NULL)
	{
		Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
		check_index_is_clusterable(rel, indexOid, AccessShareLock);
		rtcs = get_tables_to_cluster_partitioned(cluster_context, indexOid);

		/* 关闭关系，释放父表上的锁 */
		table_close(rel, AccessExclusiveLock);
	}
	else
	{
		rtcs = get_tables_to_cluster(cluster_context);
		params.options |= CLUOPT_RECHECK_ISCLUSTERED;
	}

	/* 执行工作。 */
	cluster_multiple_rels(rtcs, &params);

	/* 为清理工作开启一个新事务。 */
	StartTransactionCommand();

	/* 清理工作存储 */
	MemoryContextDelete(cluster_context);
}

/*
 * 给定要聚簇的关系列表，在单独的事务中
 * 处理其中每一个关系。
 *
 * 我们期望在开始时处于一个事务中，但返回时
 * 没有事务。
 */
static void
cluster_multiple_rels(List *rtcs, ClusterParams *params)
{
	ListCell   *lc;

	/* 提交以退出起始事务 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* 聚簇表，每个在单独的事务中 */
	foreach(lc, rtcs)
	{
		RelToCluster *rtc = (RelToCluster *) lfirst(lc);

		/* 为每个关系开启一个新事务。 */
		StartTransactionCommand();

		/* 索引中的函数可能需要设置快照 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* 执行工作。 */
		cluster_rel(rtc->tableOid, rtc->indexOid, params);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
}

/*
 * cluster_rel
 *
 * 通过创建一个新的已聚簇表并交换新表和旧表的
 * relfilenumber 来聚簇表，以便保留原始表的 OID。
 * 因此我们不会丢失 GRANT、继承或对此表的引用
 * （这在 7.3 及之前的版本中是一个 bug）。
 *
 * 索引也通过 REINDEX 进行重建。由于我们实际上是在
 * 批量加载新表，在加载表之后创建索引比在加载时
 * 逐步填充索引更好。
 *
 * 如果 indexOid 为 InvalidOid，
 * 表将按物理顺序而非索引顺序
 * 重写。这是 VACUUM FULL 的新实现，错误消息应将操作称为
 * VACUUM 而非 CLUSTER。
 */
void
cluster_rel(Oid tableOid, Oid indexOid, ClusterParams *params)
{
	Relation	OldHeap;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	bool		verbose = ((params->options & CLUOPT_VERBOSE) != 0);
	bool		recheck = ((params->options & CLUOPT_RECHECK) != 0);

	/* 检查用户请求的中止。 */
	CHECK_FOR_INTERRUPTS();

	pgstat_progress_start_command(PROGRESS_COMMAND_CLUSTER, tableOid);
	if (OidIsValid(indexOid))
		pgstat_progress_update_param(PROGRESS_CLUSTER_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_CLUSTER);
	else
		pgstat_progress_update_param(PROGRESS_CLUSTER_COMMAND,
									 PROGRESS_CLUSTER_COMMAND_VACUUM_FULL);

	/*
	 * 我们在事务持续期间获取对目标关系和索引的
	 * 排他访问权。（这对于单事务情况是冗余的，
	 * 因为 cluster() 已经这样做了。）
	 * 索引锁在 check_index_is_clusterable 内部获取。
	 */
	OldHeap = try_relation_open(tableOid, AccessExclusiveLock);

	/* 如果表已消失，我们可以跳过对它的处理 */
	if (!OldHeap)
	{
		pgstat_progress_end_command();
		return;
	}

	/*
	 * 切换到表所有者的 userid，以便任何索引函数
	 * 作为该用户运行。同时锁定安全限制操作并
	 * 安排使 GUC 变量更改局部于此命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(OldHeap->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/*
	 * 由于我们可能为每个关系开启新事务，我们必须
	 * 检查关系仍然是我们所认为的那样。
	 *
	 * 如果这是单事务 CLUSTER，我们可以跳过这些测试。
	 * 我们 *必须* 跳过 indisclustered 检查，因为它会拒绝
	 * 聚簇一个之前未聚簇过的索引的尝试。
	 */
	if (recheck)
	{
		/* 检查用户仍然拥有该关系 */
		if (!object_ownercheck(RelationRelationId, tableOid, save_userid))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			goto out;
		}

		/*
		 * 安静地跳过远程会话的临时表。仅在"recheck"情况下
		 * 执行此检查是合适的（当前意味着有人执行整个数据库范围的
		 * CLUSTER 或对分区表的 CLUSTER）
		 * ，因为 cluster() 中
		 * 还有一个检查会阻止任何通过名称聚簇远程临时表的尝试。
		 * cluster_rel 中还有一个冗余的检查，但我们保留它
		 * 以确保额外的安全性。
		 */
		if (RELATION_IS_OTHER_TEMP(OldHeap))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			goto out;
		}

		if (OidIsValid(indexOid))
		{
			/*
			 * 检查索引是否仍然存在
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexOid)))
			{
				relation_close(OldHeap, AccessExclusiveLock);
				goto out;
			}

			/*
			 * 检查索引是否仍是设置 indisclustered 的那个，
			 * 如果需要的话。
			 */
			if ((params->options & CLUOPT_RECHECK_ISCLUSTERED) != 0 &&
				!get_index_isclustered(indexOid))
			{
				relation_close(OldHeap, AccessExclusiveLock);
				goto out;
			}
		}
	}

	/*
	 * 我们允许对共享系统目录执行 VACUUM FULL，但不允许 CLUSTER。
	 * CLUSTER 在大多数方面都可以工作，但索引只会在当前数据库中
	 * 被标记为 indisclustered，如果事后在另一个数据库中调用 CLUSTER，
	 * 将导致意想不到的行为。
	 */
	if (OidIsValid(indexOid) && OldHeap->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster a shared catalog")));

	/*
	 * 不要处理其他后端的临时表...它们的本地缓冲区
	 * 管理器无法处理。
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		if (OidIsValid(indexOid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster temporary tables of other sessions")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot vacuum temporary tables of other sessions")));
	}

	/*
	 * 同时检查当前事务中关系的活跃使用，
	 * 包括开放的扫描和待处理的 AFTER 触发器事件。
	 */
	CheckTableNotInUse(OldHeap, OidIsValid(indexOid) ? "CLUSTER" : "VACUUM");

	/* 检查堆和索引是否有效可以聚簇 */
	if (OidIsValid(indexOid))
		check_index_is_clusterable(OldHeap, indexOid, AccessExclusiveLock);

	/*
	 * 如果这是一个尚未从其查询中填充数据的物化视图，
	 * 则安静地忽略请求。这不会有任何损害，因为没有数据
	 * 需要处理，而且如果这是多关系请求的一部分
	 * -- 例如，在整个数据库上运行 CLUSTER --
	 * 我们不想抛出错误。
	 */
	if (OldHeap->rd_rel->relkind == RELKIND_MATVIEW &&
		!RelationIsPopulated(OldHeap))
	{
		relation_close(OldHeap, AccessExclusiveLock);
		goto out;
	}

	Assert(OldHeap->rd_rel->relkind == RELKIND_RELATION ||
		   OldHeap->rd_rel->relkind == RELKIND_MATVIEW ||
		   OldHeap->rd_rel->relkind == RELKIND_TOASTVALUE);

	/*
	 * 元组或页上的所有谓词锁即将变得无效，
	 * 因为我们会移动元组。将它们升级为关系
	 * 锁。索引上的谓词锁将在重建索引时升级。
	 * 
	 */
	TransferPredicateLocksToHeapRelation(OldHeap);

	/* rebuild_relation 执行所有脏活 */
	rebuild_relation(OldHeap, indexOid, verbose);

	/* 注意：rebuild_relation 会对 OldHeap 执行 table_close() */

out:
	/* 回滚索引函数所执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	pgstat_progress_end_command();
}

/*
 * 验证指定的堆和索引是否有效可以聚簇
 *
 * 副作用：获取索引上的锁。调用者在某些情况下
 * 可能已经持有表的 AccessExclusiveLock，
 * 但并非所有情况
 * 都如此，所以我们不能依赖表级锁来保护这里。
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, LOCKMODE lockmode)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, lockmode);

	/*
	 * 检查索引确实是给定关系上的索引
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != RelationGetRelid(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/* 索引访问方法必须允许聚簇 */
	if (!OldIndex->rd_indam->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	/*
	 * 不允许对不完整的索引进行聚簇（那些可能不会索引
	 * 关系中每一行的索引）。我们可以通过在表上进行单独的
	 * 顺序扫描来复制缺失的行以放宽此限制，但那样似乎
	 * 代价高且繁琐。
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * 不允许对失败的 CREATE INDEX CONCURRENTLY 留下的索引进行聚簇；
	 * 它很可能不包含每个堆行的条目，甚至可能内部
	 * 不一致。（但请注意，我们不检查 indcheckxmin；
	 * 跟随破损的 HOT 链的最坏后果只是可能将最近死亡的
	 * 元组以无序方式放入新表，这不会有什么太大的
	 * 危害。）
	 */
	if (!OldIndex->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/* 释放 OldIndex 的 relcache 引用计数，但保持锁 */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: 将指定的索引标记为被聚簇的索引
 *
 * 当 indexOid == InvalidOid 时，将将 rel 的所有索引标记为非聚簇。
 */
void
mark_index_clustered(Relation rel, Oid indexOid, bool is_internal)
{
	HeapTuple	indexTuple;
	Form_pg_index indexForm;
	Relation	pg_index;
	ListCell   *index;

	/* 不允许对分区表应用 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot mark index clustered in partitioned table")));

	/*
	 * 如果索引已经被标记为聚簇，则不需要做任何事。
	 */
	if (OidIsValid(indexOid))
	{
		if (get_index_isclustered(indexOid))
			return;
	}

	/*
	 * 检查关系的每个索引并按需要设置/清除位。
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * 如果位已设置则清除。我们知道它是错误的，
		 * 因为我们之前已经检查过了。
		 */
		if (indexForm->indisclustered)
		{
			indexForm->indisclustered = false;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}
		else if (thisIndexOid == indexOid)
		{
			/* 这之前已经检查过了，但让我们确实一点 */
			if (!indexForm->indisvalid)
				elog(ERROR, "cannot cluster on invalid index %u", indexOid);
			indexForm->indisclustered = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}

		InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
									 InvalidOid, is_internal);

		heap_freetuple(indexTuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * rebuild_relation: 按索引或物理顺序重建现有关系
 *
 * OldHeap: 要重建的表 --- 必须已打开并且已加排他锁！
 * indexOid: 用于聚簇的索引，或 InvalidOid 以按物理顺序重写。
 *
 * 注意：此例程会在适当时机关闭 OldHeap；调用者不应关闭。
 */
static void
rebuild_relation(Relation OldHeap, Oid indexOid, bool verbose)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			accessMethod = OldHeap->rd_rel->relam;
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap;
	char		relpersistence;
	bool		is_system_catalog;
	bool		swap_toast_by_content;
	TransactionId frozenXid;
	MultiXactId cutoffMulti;

	if (OidIsValid(indexOid))
		/* 将正确的索引标记为聚簇 */
		mark_index_clustered(OldHeap, indexOid, true);

	/* 在关闭 OldHeap 之前记住 rel 的信息 */
	relpersistence = OldHeap->rd_rel->relpersistence;
	is_system_catalog = IsSystemRelation(OldHeap);

	/* 关闭 relcache 条目，但保持锁直到事务提交 */
	table_close(OldHeap, NoLock);

	/* 创建将接收重新排序数据的临时表 */
	OIDNewHeap = make_new_heap(tableOid, tableSpace,
							   accessMethod,
							   relpersistence,
							   AccessExclusiveLock);

	/* 以所需顺序将堆数据复制到新表中 */
	copy_table_data(OIDNewHeap, tableOid, indexOid, verbose,
					&swap_toast_by_content, &frozenXid, &cutoffMulti);

	/*
	 * 交换目标表和临时表的物理文件，然后
	 * 重建目标表的索引并丢弃临时表。
	 */
	finish_heap_swap(tableOid, OIDNewHeap, is_system_catalog,
					 swap_toast_by_content, false, true,
					 frozenXid, cutoffMulti,
					 relpersistence);
}


/*
 * 创建临时表，该表将在 CLUSTER、ALTER TABLE 等类似操作中
 * 被填充新数据。临时表复制了 OldHeap 的逻辑结构，
 * 但将具有指定的物理存储属性 NewTableSpace、
 * NewAccessMethod 和
 * relpersistence。
 *
 * 之后，调用者应加载转移/修改后的数据到新堆，
 * 然后调用 finish_heap_swap 完成操作。
 */
Oid
make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
			  char relpersistence, LOCKMODE lockmode)
{
	TupleDesc	OldHeapDesc;
	char		NewHeapName[NAMEDATALEN];
	Oid			OIDNewHeap;
	Oid			toastid;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;
	Oid			namespaceid;

	OldHeap = table_open(OIDOldHeap, lockmode);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * 请注意，
	 * NewHeap 不会接收与 OldHeap 相关的任何默认值
	 * 或约束；我们不需要它们，而且没有理由花费周期
	 * 将它们插入到系统目录中只为了再删除它们。
	 */

	/*
	 * 但我们确实想要为新堆使用旧堆的 reloptions。
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(OIDOldHeap));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	if (relpersistence == RELPERSISTENCE_TEMP)
		namespaceid = LookupCreationNamespace("pg_temp");
	else
		namespaceid = RelationGetNamespace(OldHeap);

	/*
	 * 使用与现有表相同的命名空间中的临时名称
	 * 创建新堆。注意：与用户 relname 碰撞有一定风险。
	 * 解决这个问题似乎更麻烦，不值得；特别是，我们
	 * 不能在与旧表不同的命名空间中创建新堆，否则
	 * 我们会遇到临时表的 TEMP 状态问题。
	 *
	 * 注意：新堆不是共享关系，即使我们在重建共享
	 * 关系也是如此。但是，如果源是映射的，我们会将新堆
	 * 也设为映射的。这简化了 swap_relation_files，而且
	 * 对于重建 pg_class 是绝对必要的，原因在那里解释。
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", OIDOldHeap);

	OIDNewHeap = heap_create_with_catalog(NewHeapName,
										  namespaceid,
										  NewTableSpace,
										  InvalidOid,
										  InvalidOid,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  NewAccessMethod,
										  OldHeapDesc,
										  NIL,
										  RELKIND_RELATION,
										  relpersistence,
										  false,
										  RelationIsMapped(OldHeap),
										  ONCOMMIT_NOOP,
										  reloptions,
										  false,
										  true,
										  true,
										  OIDOldHeap,
										  NULL);
	Assert(OIDNewHeap != InvalidOid);

	ReleaseSysCache(tuple);

	/*
	 * 前进命令计数器，以便新创建关系的系统目录
	 * 元组对 table_open 可见。
	 */
	CommandCounterIncrement();

	/*
	 * 如果需要，为新关系创建 TOAST 表。
	 *
	 * 如果关系尚没有 TOAST 表，我们不可能需要为新关系
	 * 创建一个。但反过来是可能的：如果某些宽列被删除了，
	 * NewHeapCreateToastTable 可以决定新表不需要 TOAST 表。
	 *
	 * 请注意，
	 * NewHeapCreateToastTable 以 CommandCounterIncrement 结束，
	 * 因此 TOAST 表将对插入可见。
	 */
	toastid = OldHeap->rd_rel->reltoastrelid;
	if (OidIsValid(toastid))
	{
		/* 保留现有 toast 表的 reloptions，如果有的话 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);
		reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									 &isNull);
		if (isNull)
			reloptions = (Datum) 0;

		NewHeapCreateToastTable(OIDNewHeap, reloptions, lockmode, toastid);

		ReleaseSysCache(tuple);
	}

	table_close(OldHeap, NoLock);

	return OIDNewHeap;
}

/*
 * 执行表数据的物理复制。
 *
 * 有三个输出参数：
 * *pSwapToastByContent 如果 toast 表必须通过内容交换则设为 true。
 * *pFreezeXid 接收作为冻结截止点的 TransactionId。
 * *pCutoffMulti 接收作为截止点的 MultiXactId。
 */
static void
copy_table_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex, bool verbose,
				bool *pSwapToastByContent, TransactionId *pFreezeXid,
				MultiXactId *pCutoffMulti)
{
	Relation	NewHeap,
				OldHeap,
				OldIndex;
	Relation	relRelation;
	HeapTuple	reltup;
	Form_pg_class relform;
	TupleDesc	oldTupDesc PG_USED_FOR_ASSERTS_ONLY;
	TupleDesc	newTupDesc PG_USED_FOR_ASSERTS_ONLY;
	VacuumParams params;
	struct VacuumCutoffs cutoffs;
	bool		use_sort;
	double		num_tuples = 0,
				tups_vacuumed = 0,
				tups_recently_dead = 0;
	BlockNumber num_pages;
	int			elevel = verbose ? INFO : DEBUG2;
	PGRUsage	ru0;
	char	   *nspname;

	pg_rusage_init(&ru0);

	/*
	 * 打开我们需要的关系。
	 */
	NewHeap = table_open(OIDNewHeap, AccessExclusiveLock);
	OldHeap = table_open(OIDOldHeap, AccessExclusiveLock);
	if (OidIsValid(OIDOldIndex))
		OldIndex = index_open(OIDOldIndex, AccessExclusiveLock);
	else
		OldIndex = NULL;

	/* 保存命名空间名称的副本以供日志记录 */
	nspname = get_namespace_name(RelationGetNamespace(OldHeap));

	/*
	 * 它们的元组描述符应该完全相同，
	 * 但这里我们只需要假定它们有相同数量的列。
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/*
	 * 如果 OldHeap 有 toast 表，则获取 toast 表上的锁以防止它
	 * 被清理。这是必要的，因为 autovacuum 进程独立于它们的
	 * 主表处理 toast 表，没有对主表的锁。如果 autovacuum
	 * 在我们在下面计算 OldestXmin 之后开始处理 toast 表，
	 * 它将使用更晚的 OldestXmin，然后可能将属于我们认为
	 * 只是 RECENTLY_DEAD 的主表
	 * 元组对应的 toast 元组作为 DEAD
	 * 删除。那样我们在尝试复制那些元组时将会失败。
	 *
	 * 我们不需要在这里打开 toast 关系，只需要加锁。该锁
	 * 将保持到事务结束。
	 */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * 如果两个表都有 TOAST 表，则通过内容执行 toast
	 * 交换。旧表有 toast 表但新表没有是可能的，
	 * 如果 toastable 列被删除了。在那种情况下，我们必须
	 * 通过链接交换。这是可以的，因为通过内容交换仅对
	 * 系统目录至关重要，而我们不支持对它们的架构更改。
	 */
	if (OldHeap->rd_rel->reltoastrelid && NewHeap->rd_rel->reltoastrelid)
	{
		*pSwapToastByContent = true;

		/*
		 * 当通过内容交换时，写入 NewHeap 的任何 toast 指针
		 * 必须使用旧 toast 表的 OID，因为那是 toast 数据
		 * 最终所在的地方。通过设置 rd_toastoid 来配置此设置。
		 * 这也告诉 toast_save_datum() 保留 toast 值 OID，
		 * 
		 * 这是我们
		 * 希望的，以便不会使系统目录缓存中的 toast 指针
		 * 失效，并避免为单个 toast 值创建多个副本。
		 *
		 * 请注意，我们必须保持 NewHeap 打开直到我们完成写入数据，
		 * 因为一旦关系关闭，relcache 将不会保证记住此设置。
		 * 此外，这种技术依赖于没有人会在我们完成写入它
		 * 并交换关系之前尝试从 NewHeap 读取的事实 --- 否则他们
		 * 可能会将 toast 指针跟到错误的地方。（它实际上对于从
		 * 旧 toast 表复制过来的值是可以工作的，但对于我们
		 * 新 toast 的任何以前未 toast 的值则不行。）
		 */
		NewHeap->rd_toastoid = OldHeap->rd_rel->reltoastrelid;
	}
	else
		*pSwapToastByContent = false;

	/*
	 * 计算用于冻结和清除死亡元组和 multixact 的 xid。
	 * 由于我们无论如何都要重写整个表，没有理由不
	 * 在这方面积极一些。
	 */
	memset(&params, 0, sizeof(VacuumParams));
	vacuum_get_cutoffs(OldHeap, &params, &cutoffs);

	/*
	 * FreezeXid 将成为表的新 relfrozenxid，这个值不能回退，
	 * 所以取最大值。
	 */
	if (TransactionIdIsValid(OldHeap->rd_rel->relfrozenxid) &&
		TransactionIdPrecedes(cutoffs.FreezeLimit,
							  OldHeap->rd_rel->relfrozenxid))
		cutoffs.FreezeLimit = OldHeap->rd_rel->relfrozenxid;

	/*
	 * MultiXactCutoff 同样也不应回退。
	 */
	if (MultiXactIdIsValid(OldHeap->rd_rel->relminmxid) &&
		MultiXactIdPrecedes(cutoffs.MultiXactCutoff,
							OldHeap->rd_rel->relminmxid))
		cutoffs.MultiXactCutoff = OldHeap->rd_rel->relminmxid;

	/*
	 * 决定是使用索引扫描还是顺序扫描加可选排序来扫描
	 * OldHeap。我们知道如何用排序来复制 btree 索引的顺序，
	 * 并且如果计划器告诉我们更便宜的话，将对那种情况
	 * 使用顺序扫描加排序。否则，如果提供了索引则始终
	 * 使用索引扫描，否则为纯顺序扫描。
	 */
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		use_sort = plan_cluster_use_sort(OIDOldHeap, OIDOldIndex);
	else
		use_sort = false;

	/* 记录我们正在做什么 */
	if (OldIndex != NULL && !use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using index scan on \"%s\"",
						nspname,
						RelationGetRelationName(OldHeap),
						RelationGetRelationName(OldIndex))));
	else if (use_sort)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using sequential scan and sort",
						nspname,
						RelationGetRelationName(OldHeap))));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						nspname,
						RelationGetRelationName(OldHeap))));

	/*
	 * 将实际的复制工作交给访问方法特定的函数，
	 * 通用代码无法知道如何处理不同访问方法之间的
	 * 可见性。请注意，此例程允许将 FreezeXid / MultiXactCutoff
	 * 设置为不同的值（例如，因为访问方法不使用冻结）。
	 */
	table_relation_copy_for_cluster(OldHeap, NewHeap, OldIndex, use_sort,
									cutoffs.OldestXmin, &cutoffs.FreezeLimit,
									&cutoffs.MultiXactCutoff,
									&num_tuples, &tups_vacuumed,
									&tups_recently_dead);

	/* 将选定的值返回给调用者，将被设置为 relfrozenxid/minmxid */
	*pFreezeXid = cutoffs.FreezeLimit;
	*pCutoffMulti = cutoffs.MultiXactCutoff;

	/* 重置 rd_toastoid 仅为整洁 --- 不应再查看它 */
	NewHeap->rd_toastoid = InvalidOid;

	num_pages = RelationGetNumberOfBlocks(NewHeap);

	/* 记录我们做了什么 */
	ereport(elevel,
			(errmsg("\"%s.%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					nspname,
					RelationGetRelationName(OldHeap),
					tups_vacuumed, num_tuples,
					RelationGetNumberOfBlocks(OldHeap)),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "%s.",
					   tups_recently_dead,
					   pg_rusage_show(&ru0))));

	if (OldIndex != NULL)
		index_close(OldIndex, NoLock);
	table_close(OldHeap, NoLock);
	table_close(NewHeap, NoLock);

	/* 更新 pg_class 以反映页和元组的正确值。 */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(OIDNewHeap));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", OIDNewHeap);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	relform->relpages = num_pages;
	relform->reltuples = num_tuples;

	/* 不要更新 pg_class 的统计信息。参见 swap_relation_files。 */
	if (OIDOldHeap != RelationRelationId)
		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);
	else
		CacheInvalidateRelcacheByTuple(reltup);

	/* 清理。 */
	heap_freetuple(reltup);
	table_close(relRelation, RowExclusiveLock);

	/* 使更新可见 */
	CommandCounterIncrement();
}

/*
 * 交换两个给定关系的物理文件。
 *
 * 我们交换物理标识（reltablespace、relfilenumber），同时保持
 * 两个关系的逻辑标识不变。relpersistence 也被交换，
 * 这是至关重要的，因为它决定了每个关系的缓冲区存放位置。
 *
 * 我们可以用两种方式之一交换关联的 TOAST 数据：递归地交换
 * toast 表（及其索引）的物理内容，或者交换给定关系的
 * pg_class 条目中的 TOAST 链接。前者是管理共享系统目录重写
 * 所必需的（在那里我们不能更改 pg_class 链接），而后者是
 * 处理 toast 表被添加或完全删除情况的唯一方式。
 *
 * 此外，第一个关系被标记为 relfrozenxid 设置为 frozenXid。
 * 将这个放在这里似乎有点丑陋，但调用者无论如何都得做这件事，
 * 所以放在这里可以省去一次 heap_update。注意：在交换 toast 链接
 * 的情况下，我们假设不需要更改 toast 表的 relfrozenxid：
 * toast 表的新版本应已经将 relfrozenxid 设置为 RecentXmin，
 * 
 * 
 * 这就够了。
 *
 * 最后，如果 r2 及其 toast 表和 toast 索引（如果有的话）是映射的，
 * 它们的 OID 会被发布到 mapped_tables[] 中。这有点土办法，但比
 * 在 finish_heap_swap 中后续再次查找这些信息要好。
 */
static void
swap_relation_files(Oid r1, Oid r2, bool target_is_pg_class,
					bool swap_toast_by_content,
					bool is_internal,
					TransactionId frozenXid,
					MultiXactId cutoffMulti,
					Oid *mapped_tables)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	RelFileNumber relfilenumber1,
				relfilenumber2;
	RelFileNumber swaptemp;
	char		swptmpchr;
	Oid			relam1,
				relam2;

	/* 我们需要两个 pg_class 元组的可写副本。 */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r1));
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r2));
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	relfilenumber1 = relform1->relfilenode;
	relfilenumber2 = relform2->relfilenode;
	relam1 = relform1->relam;
	relam2 = relform2->relam;

	if (RelFileNumberIsValid(relfilenumber1) &&
		RelFileNumberIsValid(relfilenumber2))
	{
		/*
		 * 普通非映射关系：交换 relfilenumber、reltablespace、
		 * relpersistence
		 */
		Assert(!target_is_pg_class);

		swaptemp = relform1->relfilenode;
		relform1->relfilenode = relform2->relfilenode;
		relform2->relfilenode = swaptemp;

		swaptemp = relform1->reltablespace;
		relform1->reltablespace = relform2->reltablespace;
		relform2->reltablespace = swaptemp;

		swaptemp = relform1->relam;
		relform1->relam = relform2->relam;
		relform2->relam = swaptemp;

		swptmpchr = relform1->relpersistence;
		relform1->relpersistence = relform2->relpersistence;
		relform2->relpersistence = swptmpchr;

		/* 如果我们通过链接交换，也交换 toast 链接 */
		if (!swap_toast_by_content)
		{
			swaptemp = relform1->reltoastrelid;
			relform1->reltoastrelid = relform2->reltoastrelid;
			relform2->reltoastrelid = swaptemp;
		}
	}
	else
	{
		/*
		 * 映射关系情况。这里我们必须交换关系映射
		 * 而不是修改 pg_class 列。两者都必须是映射的。
		 */
		if (RelFileNumberIsValid(relfilenumber1) ||
			RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "cannot swap mapped relation \"%s\" with non-mapped relation",
				 NameStr(relform1->relname));

		/*
		 * 我们不能更改映射关系的表空间或持久性，
		 * 也不能处理它的 toast 链接交换，因为我们不能
		 * 对其 pg_class 行应用任何关键更改。
		 * 这些情况应由上游的
		 * 权限测试防止，所以这些检查是非用户面向的紧急后备。
		 */
		if (relform1->reltablespace != relform2->reltablespace)
			elog(ERROR, "cannot change tablespace of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relpersistence != relform2->relpersistence)
			elog(ERROR, "cannot change persistence of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relam != relform2->relam)
			elog(ERROR, "cannot change access method of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (!swap_toast_by_content &&
			(relform1->reltoastrelid || relform2->reltoastrelid))
			elog(ERROR, "cannot swap toast by links for mapped relation \"%s\"",
				 NameStr(relform1->relname));

		/*
		 * 获取映射 --- 不应失败，但保持谨慎
		 */
		relfilenumber1 = RelationMapOidToFilenumber(r1, relform1->relisshared);
		if (!RelFileNumberIsValid(relfilenumber1))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform1->relname), r1);
		relfilenumber2 = RelationMapOidToFilenumber(r2, relform2->relisshared);
		if (!RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform2->relname), r2);

		/*
		 * 向 relmapper 发送替换映射。请注意，这些映射
		 * 直到 CommandCounterIncrement 才会实际生效。
		 */
		RelationMapUpdateMap(r1, relfilenumber2, relform1->relisshared, false);
		RelationMapUpdateMap(r2, relfilenumber1, relform2->relisshared, false);

		/* 将映射的 r2 表的 OID 传递回给调用者 */
		*mapped_tables++ = r2;
	}

	/*
	 * 识别 rel1 的 relfilenumber（从 rel2 交换而来）在此
	 * 子事务中是新的。rel2 的存储（从 rel1 交换而来）
	 * 可能是新的，也可能不是。
	 */
	{
		Relation	rel1,
					rel2;

		rel1 = relation_open(r1, NoLock);
		rel2 = relation_open(r2, NoLock);
		rel2->rd_createSubid = rel1->rd_createSubid;
		rel2->rd_newRelfilelocatorSubid = rel1->rd_newRelfilelocatorSubid;
		rel2->rd_firstRelfilelocatorSubid = rel1->rd_firstRelfilelocatorSubid;
		RelationAssumeNewRelfilelocator(rel1);
		relation_close(rel1, NoLock);
		relation_close(rel2, NoLock);
	}

	/*
	 * 在共享系统目录的情况下，接下来的几个步骤只会影响
	 * 我们自己数据库的 pg_class 行；但这没关系，因为它们都是
	 * 非关键更新。这对于映射系统目录的情况也是一个重要事实，
	 * 因为可能出现我们提交了映射更改但然后无法提交
	 * pg_class 更新的情况。
	 */

	/* 设置 rel1 的冻结 Xid 和最小 MultiXid */
	if (relform1->relkind != RELKIND_INDEX)
	{
		Assert(!TransactionIdIsValid(frozenXid) ||
			   TransactionIdIsNormal(frozenXid));
		relform1->relfrozenxid = frozenXid;
		relform1->relminmxid = cutoffMulti;
	}

	/* 同时交换大小统计信息，因为新关系有新更新的统计数据 */
	{
		int32		swap_pages;
		float4		swap_tuples;
		int32		swap_allvisible;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;

		swap_allvisible = relform1->relallvisible;
		relform1->relallvisible = relform2->relallvisible;
		relform2->relallvisible = swap_allvisible;
	}

	/*
	 * 更新 pg_class 中的元组 --- 除非交换的目标关系
	 * 是 pg_class 本身。在那种情况下，进行更改毫无意义，
	 * 因为我们会更新我们即将抛弃的旧数据。因为在这里
	 * 对映射关系真正做的工作只是更改关系映射设置，
	 * 在这种情况下不更新 pg_class 行是可以的。
	 * 最重要的更改
	 * 将在后续的 finish_heap_swap() 本身中执行。
	 */
	if (!target_is_pg_class)
	{
		CatalogIndexState indstate;

		indstate = CatalogOpenIndexes(relRelation);
		CatalogTupleUpdateWithInfo(relRelation, &reltup1->t_self, reltup1,
								   indstate);
		CatalogTupleUpdateWithInfo(relRelation, &reltup2->t_self, reltup2,
								   indstate);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		/* 不更新 ... 但我们仍然需要 relcache 无效化 */
		CacheInvalidateRelcacheByTuple(reltup1);
		CacheInvalidateRelcacheByTuple(reltup2);
	}

	/*
	 * 现在 pg_class 已经更新了其与交换相关的信息，
	 * 更新关系的依赖关系以指向它们新的表访问方法，
	 * 如果它发生了变化的话。
	 */
	if (relam1 != relam2)
	{
		if (changeDependencyFor(RelationRelationId,
								r1,
								AccessMethodRelationId,
								relam1,
								relam2) != 1)
			elog(ERROR, "failed to change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r1)),
				 get_rel_name(r1));
		if (changeDependencyFor(RelationRelationId,
								r2,
								AccessMethodRelationId,
								relam2,
								relam1) != 1)
			elog(ERROR, "failed to change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r2)),
				 get_rel_name(r2));
	}

	/*
	 * 修改后关系的 Post alter 钩子。r2 的更改始终是内部的，
	 * 但 r1 取决于调用上下文。
	 */
	InvokeObjectPostAlterHookArg(RelationRelationId, r1, 0,
								 InvalidOid, is_internal);
	InvokeObjectPostAlterHookArg(RelationRelationId, r2, 0,
								 InvalidOid, true);

	/*
	 * 如果我们有与被交换的关系相关联的 toast 表，
	 * 也要处理它们。
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		if (swap_toast_by_content)
		{
			if (relform1->reltoastrelid && relform2->reltoastrelid)
			{
				/* 递归地交换 toast 表的内容 */
				swap_relation_files(relform1->reltoastrelid,
									relform2->reltoastrelid,
									target_is_pg_class,
									swap_toast_by_content,
									is_internal,
									frozenXid,
									cutoffMulti,
									mapped_tables);
			}
			else
			{
				/* 调用者搞砸了 */
				elog(ERROR, "cannot swap toast files by content when there's only one");
			}
		}
		else
		{
			/*
			 * 我们交换了所有权链接，所以我们需要更改依赖
			 * 数据以匹配。
			 *
			 * 注意：可能只有一个表有 toast 表。
			 *
			 * 注意：当前，TOAST 表的唯一依赖是对其拥有表的依赖。
			 * 如果将来创建更多的依赖，我们需要使用比
			 * deleteDependencyRecordsFor() 更有选择性的方法来只删除
			 * 我们想要的链接。
			 */
			ObjectAddress baseobject,
						toastobject;
			long		count;

			/*
			 * 我们不允许对系统目录出现这种情况，以避免
			 * 我们正在重建的目录就是依赖更改会更改的目录之一的
			 * 可能性。现在对目标目录进行任何数据更改都已经太晚了。
			 * 
			 */
			if (IsSystemClass(r1, relform1))
				elog(ERROR, "cannot swap toast files by links for system catalogs");

			/* 删除旧的依赖关系 */
			if (relform1->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform1->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}
			if (relform2->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform2->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}

			/* 注册新的依赖关系 */
			baseobject.classId = RelationRelationId;
			baseobject.objectSubId = 0;
			toastobject.classId = RelationRelationId;
			toastobject.objectSubId = 0;

			if (relform1->reltoastrelid)
			{
				baseobject.objectId = r1;
				toastobject.objectId = relform1->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}

			if (relform2->reltoastrelid)
			{
				baseobject.objectId = r2;
				toastobject.objectId = relform2->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}
		}
	}

	/*
	 * 如果我们通过内容交换两个 toast 表，也对它们的
	 * 有效索引执行相同操作。实际上，只有当关系有索引时
	 * 才能安全地执行交换。
	 */
	if (swap_toast_by_content &&
		relform1->relkind == RELKIND_TOASTVALUE &&
		relform2->relkind == RELKIND_TOASTVALUE)
	{
		Oid			toastIndex1,
					toastIndex2;

		/* 获取每个关系的有效索引 */
		toastIndex1 = toast_get_valid_index(r1,
											AccessExclusiveLock);
		toastIndex2 = toast_get_valid_index(r2,
											AccessExclusiveLock);

		swap_relation_files(toastIndex1,
							toastIndex2,
							target_is_pg_class,
							swap_toast_by_content,
							is_internal,
							InvalidTransactionId,
							InvalidMultiXactId,
							mapped_tables);
	}

	/* 清理。 */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	table_close(relRelation, RowExclusiveLock);

	/*
	 * 关闭两个 relcache 条目的 smgr 链接。我们需要这个歪招
	 * 因为两个链接都将在即将到来的 Comma
	 * ndCounterIncrement 中
	 * 被无效化。无论哪个关系第二个被清除，都会有一个
	 * 挂起的对另一个 smgr 条目的引用。与其尝试通过
	 * 精心安排操作顺序来避免这个问题，不如先关闭链接。
	 * （幸运的是，由于其中一个条目在我们的事务中是本地的，
	 * 只需要用这种方式清除我们自己的 relcache 就足够了；
	 * 当其他后端看到我们对非临时关系的更新时，
	 * 这个问题不会出现。）
	 *
	 * 警告：此步骤的位置与通过递归处理 toast rels 的决定
	 * 相互作用。当我们尝试重建 pg_class 本身时，对 pg_class
	 * 的 smgr 关闭必须在此函数中的所有访问之后发生。
	 */
	RelationCloseSmgrByOid(r1);
	RelationCloseSmgrByOid(r2);
}

/*
 * 删除由 make_new_heap 构建的临时表，并完成
 * 清理工作（包括重建旧堆上的所有索引）。
 */
void
finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
				 bool is_system_catalog,
				 bool swap_toast_by_content,
				 bool check_constraints,
				 bool is_internal,
				 TransactionId frozenXid,
				 MultiXactId cutoffMulti,
				 char newrelpersistence)
{
	ObjectAddress object;
	Oid			mapped_tables[4];
	int			reindex_flags;
	ReindexParams reindex_params = {0};
	int			i;

	/* 报告我们现在正在交换关系文件 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_SWAP_REL_FILES);

	/* 将 swapped_relation_files 的可能结果零化 */
	memset(mapped_tables, 0, sizeof(mapped_tables));

	/*
	 * 交换堆关系的内容（包括任何 toast 表）。
	 * 同时将旧堆的 relfrozenxid 设置为 frozenXid。
	 */
	swap_relation_files(OIDOldHeap, OIDNewHeap,
						(OIDOldHeap == RelationRelationId),
						swap_toast_by_content, is_internal,
						frozenXid, cutoffMulti, mapped_tables);

	/*
	 * 如果是系统目录，排队一个 sinval 消息以在我们
	 * 到达 CommandCounterIncrement 时刷新目录上的所有目录缓存。
	 */
	if (is_system_catalog)
		CacheInvalidateCatalog(OIDOldHeap);

	/*
	 * 重建关系上的每个索引（但不包括 toast 表，在这个时候
	 * 它已经是全新的）。重要的是在 DROP 步骤之前执行此操作，
	 * 因为如果我们正在处理一个将在 DROP 期间使用的系统目录，
	 * 我们希望其索引可用。另一种顺序也没有好处，因为
	 * 这一切都是事务性的，所以在提交之前没有机会回收磁盘
	 * 空间。我们不需要最后的 CommandCounterIncrement()，
	 * 因为 reindex_relation 会执行它。
	 *
	 * 注意：因为 index_build 是通过 reindex_relation 调用的，它永远不会
	 * 为索引设置 indcheckxmin 为 true。这是可以的，即使从某种意义上
	 * 说我们在构建新索引而不是重建现有索引，因为新堆
	 * 完全不会包含任何 HOT 链，更不用说破损的链了，
	 * 所以不可能需要设置 indcheckxmin。
	 */
	reindex_flags = REINDEX_REL_SUPPRESS_INDEX_USE;
	if (check_constraints)
		reindex_flags |= REINDEX_REL_CHECK_CONSTRAINTS;

	/*
	 * 确保索引具有与父关系相同的
	 * 持久性。
	 */
	if (newrelpersistence == RELPERSISTENCE_UNLOGGED)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_UNLOGGED;
	else if (newrelpersistence == RELPERSISTENCE_PERMANENT)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_PERMANENT;

	/* 报告我们现在正在重建索引 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_REBUILD_INDEX);

	reindex_relation(OIDOldHeap, reindex_flags, &reindex_params);

	/* 报告我们现在正在清理 */
	pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
								 PROGRESS_CLUSTER_PHASE_FINAL_CLEANUP);

	/*
	 * 如果被重建的关系是 pg_class，swap_relation_files()
	 * 无法更新 pg_class 自己的 pg_class 条目（参见
	 * swap_relation_files() 中的注释），因此 relfrozenxid 没有被更新。
	 * 这很烦人，因为执行 VACUUM FULL 的一个潜在原因是
	 * 即将到来或实际发生的反回绕关闭。所以，现在我们可以
	 * 使用其索引访问新关系，更新 relfrozenxid。
	 * pg_class 没有 toast 关系，所以我们不需要更新对应的
	 * toast 关系。并非所有 relfrozenxid 更新都应移到这里，
	 * 因为 swap_relation_files() 无论如何都需要为非映射关系
	 * 写入 pg_class。
	 */
	if (OIDOldHeap == RelationRelationId)
	{
		Relation	relRelation;
		HeapTuple	reltup;
		Form_pg_class relform;

		relRelation = table_open(RelationRelationId, RowExclusiveLock);

		reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(OIDOldHeap));
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
		relform = (Form_pg_class) GETSTRUCT(reltup);

		relform->relfrozenxid = frozenXid;
		relform->relminmxid = cutoffMulti;

		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);

		table_close(relRelation, RowExclusiveLock);
	}

	/* 使用旧 filenumber 销毁新堆 */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * 新关系是我们事务的本地关系，我们知道没有任何东西
	 * 依赖它，所以 DROP_RESTRICT 应该是可以的。
	 */
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* performDeletion 在末尾执行 CommandCounterIncrement */

	/*
	 * 现在我们必须删除为临时表以及其 toast 表和 toast
	 * 索引（如果有的话）设置的任何关系映射条目。如果我们
	 * 未能在提交前执行此操作，relmapper 会抱怨在 bootstrap 之后
	 * 添加了新的永久映射条目。
	 */
	for (i = 0; OidIsValid(mapped_tables[i]); i++)
		RelationMapRemoveMapping(mapped_tables[i]);

	/*
	 * 到此时，一切就绪，除了如果我们通过链接进行了
	 * toast 交换，那么 toast 表的名称对应于临时表。
	 * 名称对后端无关紧要，因为它通过 OID 引用，
	 * 但查看系统目录的用户可能会感到困惑。
	 * 重命名它以防止这个问题。
	 *
	 * 请注意，不需要对关系加锁，
	 * 因为我们已经持有它的排他锁。
	 */
	if (!swap_toast_by_content)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		if (OidIsValid(newrel->rd_rel->reltoastrelid))
		{
			Oid			toastidx;
			char		NewToastName[NAMEDATALEN];

			/* 获取要重命名的关联有效索引 */
			toastidx = toast_get_valid_index(newrel->rd_rel->reltoastrelid,
											 NoLock);

			/* 重命名 toast 表 ... */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u",
					 OIDOldHeap);
			RenameRelationInternal(newrel->rd_rel->reltoastrelid,
								   NewToastName, true, false);

			/* ... 以及其有效索引。 */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u_index",
					 OIDOldHeap);

			RenameRelationInternal(toastidx,
								   NewToastName, true, true);

			/*
			 * 重置 toast 的 relrewrite。这里需要命令计数器
			 * 增量，因为我们即将更新作为
			 * RenameRelationInternal 一部分被更新的元组。
			 */
			CommandCounterIncrement();
			ResetRelRewrite(newrel->rd_rel->reltoastrelid);
		}
		relation_close(newrel, NoLock);
	}

	/* 如果不是系统目录表，清除任何缺失属性设置 */
	if (!is_system_catalog)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
	}
}


/*
 * 获取当前用户拥有且设置了 indisclustered 的表的列表。
 * 以 RelToCluster 的 List * 格式返回列表
 * （存储在指定的内存上下文中），每个元素给出 tableOid
 * 和表已被聚簇的索引 indexOid。
 */
static List *
get_tables_to_cluster(MemoryContext cluster_context)
{
	Relation	indRelation;
	TableScanDesc scan;
	ScanKeyData entry;
	HeapTuple	indexTuple;
	Form_pg_index index;
	MemoryContext old_context;
	List	   *rtcs = NIL;

	/*
	 * 获取所有设置了 indisclustered 且由合适用户
	 * 拥有的索引。
	 */
	indRelation = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&entry,
				Anum_pg_index_indisclustered,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));
	scan = table_beginscan_catalog(indRelation, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		RelToCluster *rtc;

		index = (Form_pg_index) GETSTRUCT(indexTuple);

		if (!object_ownercheck(RelationRelationId, index->indrelid, GetUserId()))
			continue;

		/* 为结果列表使用永久性内存上下文 */
		old_context = MemoryContextSwitchTo(cluster_context);

		rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rtc->tableOid = index->indrelid;
		rtc->indexOid = index->indexrelid;
		rtcs = lappend(rtcs, rtc);

		MemoryContextSwitchTo(old_context);
	}
	table_endscan(scan);

	relation_close(indRelation, AccessShareLock);

	return rtcs;
}

/*
 * 给定分区表上的一个索引，返回所有
 * 子叶子表/索引的 RelToCluster 列表。
 *
 * 类似于 expand_vacuum_rel，但在这里调用者必须持有
 * 包含该索引的表上的 AccessExclusiveLock。
 */
static List *
get_tables_to_cluster_partitioned(MemoryContext cluster_context, Oid indexOid)
{
	List	   *inhoids;
	ListCell   *lc;
	List	   *rtcs = NIL;
	MemoryContext old_context;

	/* 在处理子表之前不要加锁 */
	inhoids = find_all_inheritors(indexOid, NoLock, NULL);

	foreach(lc, inhoids)
	{
		Oid			indexrelid = lfirst_oid(lc);
		Oid			relid = IndexGetRelation(indexrelid, false);
		RelToCluster *rtc;

		/* 仅考虑叶子索引 */
		if (get_rel_relkind(indexrelid) != RELKIND_INDEX)
			continue;

		/* 安静地跳过用户无访问权限的分区。 */
		if (!object_ownercheck(RelationRelationId, relid, GetUserId()) &&
			(!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
			 IsSharedRelation(relid)))
			continue;

		/* 为结果列表使用永久性内存上下文 */
		old_context = MemoryContextSwitchTo(cluster_context);

		rtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rtc->tableOid = relid;
		rtc->indexOid = indexrelid;
		rtcs = lappend(rtcs, rtc);

		MemoryContextSwitchTo(old_context);
	}

	return rtcs;
}
