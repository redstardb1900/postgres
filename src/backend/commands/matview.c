/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  物化视图支持
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "commands/cluster.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


typedef struct
{
	DestReceiver pub;			/* 公开的函数指针 */
	Oid			transientoid;	/* 要存储到的新堆的 OID */
	/* 这些字段由 transientrel_startup 填充： */
	Relation	transientrel;	/* 要写入的关系 */
	CommandId	output_cid;		/* 插入输出元组的 cmin */
	int			ti_options;		/* table_tuple_insert 的性能选项 */
	BulkInsertState bistate;	/* 批量插入状态 */
} DR_transientrel;

static int	matview_maintenance_depth = 0;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static uint64 refresh_matview_datafill(DestReceiver *dest, Query *query,
									   const char *queryString);
static char *make_temptable_name_n(char *tempname, int n);
static void refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
								   int save_sec_context);
static void refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence);
static bool is_usable_unique_index(Relation indexRel);
static void OpenMatViewIncrementalMaintenance(void);
static void CloseMatViewIncrementalMaintenance(void);

/*
 * SetMatViewPopulatedState
 *		把物化视图标记为已填充，或未填充。
 *
 * 注意：调用方必须持有该关系上的适当锁。
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * 更新关系的 pg_class 条目。关键副作用：其他后端
	 * （还有这个后端！）会收到 SI 消息，使它们
	 * 重建 relcache 条目。
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	((Form_pg_class) GETSTRUCT(tuple))->relispopulated = newstate;

	CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * 推进命令计数器，使更新后的 pg_class 行
	 * 在本地可见。
	 */
	CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- 执行 REFRESH MATERIALIZED VIEW 命令
 *
 * 它通过创建新表并交换新表与旧物化视图的
 * relfilenumber 来刷新物化视图，从而保留原物化视图的
 * OID。这样我们不会丢失 GRANT 或对该物化视图的
 * 引用。
 *
 * 若指定了 WITH NO DATA，这实际上相当于 TRUNCATE；
 * 否则相当于 TRUNCATE 后用与该物化视图关联的
 * SELECT 语句做 INSERT。语句节点的 skipData 字段
 * 表明该子句是否被使用。
 *
 * 索引也会通过 REINDEX 重建。由于我们实际上是在
 * 批量装载新堆，装载完成后创建索引
 * 比边装载边增量填充更好。
 *
 * 物化视图的 "populated" 状态根据其内容是否反映
 * 物化视图查询的结果集而改变。
 */
ObjectAddress
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   ParamListInfo params, QueryCompletion *qc)
{
	Oid			matviewOid;
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			relowner;
	Oid			OIDNewHeap;
	DestReceiver *dest;
	uint64		processed = 0;
	bool		concurrent;
	LOCKMODE	lockmode;
	char		relpersistence;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	ObjectAddress address;

	/* 确定所需锁的强度。 */
	concurrent = stmt->concurrent;
	lockmode = concurrent ? ExclusiveLock : AccessExclusiveLock;

	/*
	 * 获取锁直到事务结束。
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  lockmode, 0,
										  RangeVarCallbackOwnsTable, NULL);
	matviewRel = table_open(matviewOid, NoLock);
	relowner = matviewRel->rd_rel->relowner;

	/*
	 * 切换到所有者的 userid，使任何函数都以该用户
	 * 身份运行。同时锁定安全受限操作，并安排
	 * 让 GUC 变量更改仅在本命令内生效。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/* 确保它是物化视图。 */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/* 若未填充，检查没有指定 CONCURRENTLY。 */
	if (concurrent && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY cannot be used when the materialized view is not populated")));

	/* 检查没有指定冲突的选项。 */
	if (concurrent && stmt->skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s and %s options cannot be used together",
						"CONCURRENTLY", "WITH NO DATA")));

	/*
	 * 检查刷新所需的一切都正确。此时的问题
	 * 是内部错误，所以用 elog 就够了。
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * 若指定了 CONCURRENTLY，检查物化视图的一列或多列上
	 * 存在不带 WHERE 子句的唯一索引。
	 */
	if (concurrent)
	{
		List	   *indexoidlist = RelationGetIndexList(matviewRel);
		ListCell   *indexoidscan;
		bool		hasUniqueIndex = false;

		foreach(indexoidscan, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(indexoidscan);
			Relation	indexRel;

			indexRel = index_open(indexoid, AccessShareLock);
			hasUniqueIndex = is_usable_unique_index(indexRel);
			index_close(indexRel, AccessShareLock);
			if (hasUniqueIndex)
				break;
		}

		list_free(indexoidlist);

		if (!hasUniqueIndex)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently",
							quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
													   RelationGetRelationName(matviewRel))),
					 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));
	}

	/*
	 * 存储的查询在 MV 定义时已经重写过，
	 * 但未被规划器涂改。
	 */
	dataQuery = linitial_node(Query, actions);

	/*
	 * 检查当前事务中该关系是否有活跃使用，
	 * 比如打开的扫描。
	 *
	 * 注意：我们指望它保护我们免受使用
	 * TABLE_INSERT_FROZEN 刷新数据时的问题。
	 */
	CheckTableNotInUse(matviewRel, "REFRESH MATERIALIZED VIEW");

	/*
	 * 暂时把物化视图标记为已填充或未填充
	 * （若后面失败会回滚）。
	 */
	SetMatViewPopulatedState(matviewRel, !stmt->skipData);

	/* 并发刷新在临时表空间中构建新数据，并做 diff。 */
	if (concurrent)
	{
		tableSpace = GetDefaultTablespace(RELPERSISTENCE_TEMP, false);
		relpersistence = RELPERSISTENCE_TEMP;
	}
	else
	{
		tableSpace = matviewRel->rd_rel->reltablespace;
		relpersistence = matviewRel->rd_rel->relpersistence;
	}

	/*
	 * 创建将接收重新生成的数据的临时表。锁定它，
	 * 禁止任何其他进程访问，直到提交
	 * （那时它已消失）。
	 */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace,
							   matviewRel->rd_rel->relam,
							   relpersistence, ExclusiveLock);
	LockRelationOid(OIDNewHeap, AccessExclusiveLock);
	dest = CreateTransientRelDestReceiver(OIDNewHeap);

	/* 若需要，生成数据。 */
	if (!stmt->skipData)
		processed = refresh_matview_datafill(dest, dataQuery, queryString);

	/* 让物化视图匹配新生成的数据。 */
	if (concurrent)
	{
		int			old_depth = matview_maintenance_depth;

		PG_TRY();
		{
			refresh_by_match_merge(matviewOid, OIDNewHeap, relowner,
								   save_sec_context);
		}
		PG_CATCH();
		{
			matview_maintenance_depth = old_depth;
			PG_RE_THROW();
		}
		PG_END_TRY();
		Assert(matview_maintenance_depth == old_depth);
	}
	else
	{
		refresh_by_heap_swap(matviewOid, OIDNewHeap, relpersistence);

		/*
		 * 把我们的活动告知累计统计系统：基本上，
		 * 我们截断了物化视图并插入了一些新数据。
		 * （上面的并发代码路径不需要担心这一点，因为它发出的
		 * 插入和删除会被底层代码统计。）
		 */
		pgstat_count_truncate(matviewRel);
		if (!stmt->skipData)
			pgstat_count_heap_insert(matviewRel, processed);
	}

	table_close(matviewRel, NoLock);

	/* 回滚任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	ObjectAddressSet(address, RelationRelationId, matviewOid);

	/*
	 * 保存行数，让 pg_stat_statements 能跟踪 REFRESH
	 * MATERIALIZED VIEW 命令处理的总行数。注意我们
	 * 仍不在命令完成标签输出中显示行数，也就是说，
	 * CMDTAG_REFRESH_MATERIALIZED_VIEW 命令标签的
	 * display_rowcount 标志在 cmdtaglist.h 中保持 false。
	 * 否则，完成标签输出的变化可能破坏使用它的应用。
	 */
	if (qc)
		SetQueryCompletion(qc, CMDTAG_REFRESH_MATERIALIZED_VIEW, processed);

	return address;
}

/*
 * refresh_matview_datafill
 *
 * 执行给定查询，把结果行发送到 "dest"
 * （它会把它们插入目标物化视图）。
 *
 * 返回插入的行数。
 */
static uint64
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *copied_query;
	uint64		processed;

	/* 加锁并重写，用副本保留原始查询。 */
	copied_query = copyObject(query);
	AcquireRewriteLocks(copied_query, true, false);
	rewritten = QueryRewrite(copied_query);

	/* SELECT 重写后应该不多不少恰为一个 SELECT 查询 */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* 检查用户请求的中止。 */
	CHECK_FOR_INTERRUPTS();

	/* 规划将生成刷新数据的查询。 */
	plan = pg_plan_query(query, queryString, CURSOR_OPT_PARALLEL_OK, NULL);

	/*
	 * 使用带有更新后的命令 ID 的快照，确保本查询能看到
	 * 任何先前执行的查询的结果。（只有当规划器执行了
	 * 某个号称稳定的函数、而它改变了数据库内容时
	 * 这才重要，但为了保险我们还是照做。）
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* 创建 QueryDesc，把输出重定向到我们的元组接收器 */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, NULL, 0);

	/* 调用 ExecutorStart 为执行准备计划 */
	ExecutorStart(queryDesc, 0);

	/* 运行计划 */
	ExecutorRun(queryDesc, ForwardScanDirection, 0, true);

	processed = queryDesc->estate->es_processed;

	/* 然后清理 */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	return processed;
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = (DR_transientrel *) palloc0(sizeof(DR_transientrel));

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- 执行器启动
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = table_open(myState->transientoid, NoLock);

	/*
	 * 填充 myState 的私有字段，供后续例程使用
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM | TABLE_INSERT_FROZEN;
	myState->bistate = GetBulkInsertState();

	/*
	 * 有效的 smgr_targblock 意味着已有东西写过该关系。
	 * 这也许无害，但本函数并未为此做准备。
	 */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- 接收一个元组
 */
static bool
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	/*
	 * 注意，输入槽的类型可能不同于目标关系的类型。
	 * table_tuple_insert() 支持这一点，但比用正确的槽
	 * 插入效率略低——不过另一种做法是复制进类型正确
	 * 的槽，那也不便宜。这同样不允许访问每个 AM 的
	 * 数据（比如元组的 xmin），但既然我们在这里
	 * 不那样做……
	 */

	table_tuple_insert(myState->transientrel,
					   slot,
					   myState->output_cid,
					   myState->ti_options,
					   myState->bistate);

	/* 我们知道这是新建的关系，因此没有索引 */

	return true;
}

/*
 * transientrel_shutdown --- 执行器结束
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	table_finish_bulk_insert(myState->transientrel, myState->ti_options);

	/* 关闭 transientrel，但把锁保留到提交 */
	table_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- 释放 DestReceiver 对象
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}


/*
 * 给定限定的临时表名，追加下划线加给定整数，
 * 基于旧名构造新表名。
 * 结果是 palloc 分配的字符串。
 *
 * 按当前写法，若给定名称是 "FOO"."BAR" 之类，
 * 这将无法构成合法的 SQL 名。目前，输入的表名部分
 * 永远不会带双引号，因为它的形式是 "pg_temp_NNN"，
 * 参见 make_new_heap()。但总有一天我们可能要更费劲些。
 */
static char *
make_temptable_name_n(char *tempname, int n)
{
	StringInfoData namebuf;

	initStringInfo(&namebuf);
	appendStringInfoString(&namebuf, tempname);
	appendStringInfo(&namebuf, "_%d", n);
	return namebuf.data;
}

/*
 * refresh_by_match_merge
 *
 * 以事务语义刷新物化视图，同时允许
 * 并发读取。
 *
 * 在新版本数据已在临时表中创建之后调用。它对
 * 旧版本数据做全外连接，产生 "diff" 结果。若
 * 新旧版本中有任何重复行（即两行之间每一列
 * 都比较为相等），这个连接就无法工作。对于
 * 至少有一个 NULL 值、其余非 NULL 列都相等的
 * 行，它能正确工作。NULL 在相等测试和 UNIQUE
 * 索引上的行为在这里恰好非常方便；我们需要
 * 做的测试与默认行为一致。若物化视图上至少有
 * 一个 UNIQUE 索引，我们恰好拥有所需的
 * 保证。
 *
 * 用于保存 diff 结果的临时表只包含旧记录的 TID
 * （若匹配）和新表的 ROW，后者是复杂记录类型的
 * 单列（若匹配）。
 *
 * 有了 diff 表之后，我们对物化视图执行基于集合的
 * DELETE 和 INSERT 操作，然后丢弃两个
 * 临时表。
 *
 * 从生成新数据到应用差异，一切都在
 * ExclusiveLock 的掩护下进行，因为我们似乎不仅想
 * 禁止并发的 REFRESH 操作，还想禁止增量维护。
 * 允许对本命令更新或删除的行做 SELECT FOR UPDATE
 * 或 SELECT FOR SHARE 似乎也不合理
 * 或不安全。
 */
static void
refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context)
{
	StringInfoData querybuf;
	Relation	matviewRel;
	Relation	tempRel;
	char	   *matviewname;
	char	   *tempname;
	char	   *diffname;
	TupleDesc	tupdesc;
	bool		foundUniqueIndex;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int16		relnatts;
	Oid		   *opUsedForQual;

	initStringInfo(&querybuf);
	matviewRel = table_open(matviewOid, NoLock);
	matviewname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
											 RelationGetRelationName(matviewRel));
	tempRel = table_open(tempOid, NoLock);
	tempname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(tempRel)),
										  RelationGetRelationName(tempRel));
	diffname = make_temptable_name_n(tempname, 2);

	relnatts = RelationGetNumberOfAttributes(matviewRel);

	/* 打开 SPI 上下文。 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* 分析含有新内容的临时表。 */
	appendStringInfo(&querybuf, "ANALYZE %s", tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * 在能依赖 "diff" 结果之前，我们需要确保新数据集中
	 * 没有不带 NULL 的重复行。以能显示找到的第一个
	 * 重复行的方式检查。即使通过了这个测试，物化视图
	 * 上的唯一索引仍可能发现
	 * 重复键问题。
	 *
	 * 注意：这里和下面，我们用 "tablename.*::tablerowtype"
	 * 作为技巧，防止 ".*" 在 SELECT 列表中被展开为
	 * 多列。比较 ruleutils.c 的 get_variable()。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "SELECT newdata.*::%s FROM %s newdata "
					 "WHERE newdata.* IS NOT NULL AND EXISTS "
					 "(SELECT 1 FROM %s newdata2 WHERE newdata2.* IS NOT NULL "
					 "AND newdata2.* OPERATOR(pg_catalog.*=) newdata.* "
					 "AND newdata2.ctid OPERATOR(pg_catalog.<>) "
					 "newdata.ctid)",
					 tempname, tempname, tempname);
	if (SPI_execute(querybuf.data, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	if (SPI_processed > 0)
	{
		/*
		 * 注意这个 ereport() 正在向用户返回数据。一般来说，
		 * 我们要确保用户已被授予访问这些数据的权限。
		 * 不过，REFRESH MAT VIEW 只能由物化视图的所有者
		 * （或超级用户）运行，因此没有必要检查对
		 * 物化视图数据的访问。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
						RelationGetRelationName(matviewRel)),
				 errdetail("Row: %s",
						   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));
	}

	/*
	 * 创建临时 "diff" 表。
	 *
	 * 暂时切出 SECURITY_RESTRICTED_OPERATION 上下文，
	 * 因为在 SRO 上下文中无法创建临时表。为格外
	 * 多疑起见，只在切回 SRO 上下文之后
	 * 才添加复合类型列。
	 */
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "CREATE TEMP TABLE %s (tid pg_catalog.tid)",
					 diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "ALTER TABLE %s ADD COLUMN newdata %s",
					 diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* 开始构建填充 diff 表的查询。 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s "
					 "SELECT mv.ctid AS tid, newdata.*::%s AS newdata "
					 "FROM %s mv FULL JOIN %s newdata ON (",
					 diffname, tempname, matviewname, tempname);

	/*
	 * 从 relcache 获取表的索引 OID 列表，并在 pg_index
	 * syscache 中查找每一个。我们将对所有唯一索引中
	 * 出现的所有列做相等性测试，这些索引只引用列
	 * 且包含所有行。
	 */
	tupdesc = matviewRel->rd_att;
	opUsedForQual = (Oid *) palloc0(sizeof(Oid) * relnatts);
	foundUniqueIndex = false;

	indexoidlist = RelationGetIndexList(matviewRel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;

		indexRel = index_open(indexoid, RowExclusiveLock);
		if (is_usable_unique_index(indexRel))
		{
			Form_pg_index indexStruct = indexRel->rd_index;
			int			indnkeyatts = indexStruct->indnkeyatts;
			oidvector  *indclass;
			Datum		indclassDatum;
			int			i;

			/* 必须用麻烦的方式获取 indclass。 */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
												   indexRel->rd_indextuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/* 为该索引的所有列添加 quals。 */
			for (i = 0; i < indnkeyatts; i++)
			{
				int			attnum = indexStruct->indkey.values[i];
				Oid			opclass = indclass->values[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
				Oid			attrtype = attr->atttypid;
				HeapTuple	cla_ht;
				Form_pg_opclass cla_tup;
				Oid			opfamily;
				Oid			opcintype;
				Oid			op;
				const char *leftop;
				const char *rightop;

				/*
				 * 找出与该索引列关联的相等操作符。
				 * 首先我们需要查找该列的 opclass。
				 */
				cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(cla_ht))
					elog(ERROR, "cache lookup failed for opclass %u", opclass);
				cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
				Assert(cla_tup->opcmethod == BTREE_AM_OID);
				opfamily = cla_tup->opcfamily;
				opcintype = cla_tup->opcintype;
				ReleaseSysCache(cla_ht);

				op = get_opfamily_member(opfamily, opcintype, opcintype,
										 BTEqualStrategyNumber);
				if (!OidIsValid(op))
					elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
						 BTEqualStrategyNumber, opcintype, opcintype, opfamily);

				/*
				 * 若我们在多个索引中发现具有相同相等语义的
				 * 同一列，就只需要输出一次
				 * 相等子句。
				 *
				 * 由于我们只记住最后一个相等操作符，在多个索引
				 * 使用若干不同 opclass 的情况下，这段代码可能
				 * 被愚弄而输出重复子句 …… 但那种情况太
				 * 不可能，似乎不值得多花
				 * 代码去避免。
				 */
				if (opUsedForQual[attnum - 1] == op)
					continue;
				opUsedForQual[attnum - 1] = op;

				/*
				 * 实际添加该 qual，与其他的做 AND。
				 */
				if (foundUniqueIndex)
					appendStringInfoString(&querybuf, " AND ");

				leftop = quote_qualified_identifier("newdata",
													NameStr(attr->attname));
				rightop = quote_qualified_identifier("mv",
													 NameStr(attr->attname));

				generate_operator_clause(&querybuf,
										 leftop, attrtype,
										 op,
										 rightop, attrtype);

				foundUniqueIndex = true;
			}
		}

		/* 保留锁，因为我们即将运行需要它们的 DML。 */
		index_close(indexRel, NoLock);
	}

	list_free(indexoidlist);

	/*
	 * 物化视图上必须至少有一个可用的唯一索引。
	 *
	 * ExecRefreshMatView() 在取得物化视图上的排他锁之后
	 * 检查了这一点。因此这里保证至少存在一个唯一索引，
	 * 因为锁仍被持有。（一个已知的例外是：刷新
	 * 物化视图时调用的某个函数删除了该索引。
	 * 那是相当愚蠢的行为。）
	 */
	if (!foundUniqueIndex)
		elog(ERROR, "could not find suitable unique index on materialized view");

	appendStringInfoString(&querybuf,
						   " AND newdata.* OPERATOR(pg_catalog.*=) mv.*) "
						   "WHERE newdata.* IS NULL OR mv.* IS NULL "
						   "ORDER BY tid");

	/* 填充临时 "diff" 表。 */
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * "full-data" 临时表的数据我们不再使用，但必须
	 * 保留它，因为 diff 表引用了它的类型。
	 */

	/* 分析 diff 表。 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "ANALYZE %s", diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	OpenMatViewIncrementalMaintenance();

	/* 删除必须先于插入；先做删除。 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "DELETE FROM %s mv WHERE ctid OPERATOR(pg_catalog.=) ANY "
					 "(SELECT diff.tid FROM %s diff "
					 "WHERE diff.tid IS NOT NULL "
					 "AND diff.newdata IS NULL)",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_DELETE)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* 插入放在最后。 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s SELECT (diff.newdata).* "
					 "FROM %s diff WHERE tid IS NULL",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* 物化视图维护完成。 */
	CloseMatViewIncrementalMaintenance();
	table_close(tempRel, NoLock);
	table_close(matviewRel, NoLock);

	/* 清理临时表。 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "DROP TABLE %s, %s", diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* 关闭 SPI 上下文。 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

/*
 * 交换目标表与临时表的物理文件，然后重建目标的索引
 * 并丢弃临时表。安全上下文交换由被调用的函数处理，
 * 所以这里不需要。
 */
static void
refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence)
{
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 RecentXmin, ReadNextMultiXactId(), relpersistence);
}

/*
 * 检查指定索引是否可用于匹配合并。
 */
static bool
is_usable_unique_index(Relation indexRel)
{
	Form_pg_index indexStruct = indexRel->rd_index;

	/*
	 * 必须是唯一的、有效的、即时的、非部分的，并且定义在
	 * 普通用户列上（不是表达式）。我们还需要它是
	 * btree。即使我们有其他类型的唯一索引，我们也不知道
	 * 如何找出相应的相等操作符，也无法确定规划器能用
	 * 非 btree 操作符实现所需的
	 * FULL JOIN。
	 */
	if (indexStruct->indisunique &&
		indexStruct->indimmediate &&
		indexRel->rd_rel->relam == BTREE_AM_OID &&
		indexStruct->indisvalid &&
		RelationGetIndexPredicate(indexRel) == NIL &&
		indexStruct->indnatts > 0)
	{
		/*
		 * 逐个检查索引列的意义在于同时拒绝索引表达式
		 * 和系统列。目前，物化视图不可能有 OID 列，
		 * 所以无法在系统列上创建索引；但也许
		 * 有一天这不再成立，所以
		 * 还是保险些好。
		 */
		int			numatts = indexStruct->indnatts;
		int			i;

		for (i = 0; i < numatts; i++)
		{
			int			attnum = indexStruct->indkey.values[i];

			if (attnum <= 0)
				return false;
		}
		return true;
	}
	return false;
}


/*
 * 这应用于测试后端是否处于允许 DML 语句修改
 * 物化视图的上下文中。我们只想对由物化视图定义
 * 驱动的内部代码允许这一点，而不是任意的
 * 用户提供代码。
 *
 * 虽然函数名反映其主要预期用途是物化视图的
 * 增量维护（响应被引用关系中数据的变化），
 * 它们最初用于允许 REFRESH
 * 而不阻塞并发读取。
 */
bool
MatViewIncrementalMaintenanceIsEnabled(void)
{
	return matview_maintenance_depth > 0;
}

static void
OpenMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth++;
}

static void
CloseMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth--;
	Assert(matview_maintenance_depth >= 0);
}
