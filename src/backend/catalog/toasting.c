/*-------------------------------------------------------------------------
 *
 * toasting.c
 *	  本文件包含支持创建 toast 表的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/toasting.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void CheckAndCreateToastTable(Oid relOid, Datum reloptions,
									 LOCKMODE lockmode, bool check,
									 Oid OIDOldToast);
static bool create_toast_table(Relation rel, Oid toastOid, Oid toastIndexOid,
							   Datum reloptions, LOCKMODE lockmode, bool check,
							   Oid OIDOldToast);
static bool needs_toast_table(Relation rel);


/*
 * CreateToastTable 变体
 *		若表需要 toast 表且尚未拥有，
 *		则为它创建一个 toast 表。
 *
 * 也可以传入 toast 表的 reloptions。默认 reloptions
 * 请传入 (Datum) 0。
 *
 * 我们期望调用方已验证该关系是表，并已
 * 完成任何必要的权限检查。调用方期望本函数
 * 若有任何更改则以 CommandCounterIncrement 结束。
 */
void
AlterTableCreateToastTable(Oid relOid, Datum reloptions, LOCKMODE lockmode)
{
	CheckAndCreateToastTable(relOid, reloptions, lockmode, true, InvalidOid);
}

void
NewHeapCreateToastTable(Oid relOid, Datum reloptions, LOCKMODE lockmode,
						Oid OIDOldToast)
{
	CheckAndCreateToastTable(relOid, reloptions, lockmode, false, OIDOldToast);
}

void
NewRelationCreateToastTable(Oid relOid, Datum reloptions)
{
	CheckAndCreateToastTable(relOid, reloptions, AccessExclusiveLock, false,
							 InvalidOid);
}

static void
CheckAndCreateToastTable(Oid relOid, Datum reloptions, LOCKMODE lockmode,
						 bool check, Oid OIDOldToast)
{
	Relation	rel;

	rel = table_open(relOid, lockmode);

	/* 全部工作由 create_toast_table 完成 */
	(void) create_toast_table(rel, InvalidOid, InvalidOid, reloptions, lockmode,
							  check, OIDOldToast);

	table_close(rel, NoLock);
}

/*
 * 在引导期间创建 toast 表
 *
 * 这里需要预先指定 toast 表及其索引的 OIDs
 */
void
BootstrapToastTable(char *relName, Oid toastOid, Oid toastIndexOid)
{
	Relation	rel;

	rel = table_openrv(makeRangeVar(NULL, relName, -1), AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
		elog(ERROR, "\"%s\" is not a table or materialized view",
			 relName);

	/* 全部工作由 create_toast_table 完成 */
	if (!create_toast_table(rel, toastOid, toastIndexOid, (Datum) 0,
							AccessExclusiveLock, false, InvalidOid))
		elog(ERROR, "\"%s\" does not require a toast table",
			 relName);

	table_close(rel, NoLock);
}


/*
 * create_toast_table --- 内部工作函数
 *
 * rel 已打开并加锁
 * toastOid 和 toastIndexOid 通常为 InvalidOid，但在
 * 引导期间可为非零以指定手工分配的 OIDs
 */
static bool
create_toast_table(Relation rel, Oid toastOid, Oid toastIndexOid,
				   Datum reloptions, LOCKMODE lockmode, bool check,
				   Oid OIDOldToast)
{
	Oid			relOid = RelationGetRelid(rel);
	HeapTuple	reltup;
	TupleDesc	tupdesc;
	bool		shared_relation;
	bool		mapped_relation;
	Relation	toast_rel;
	Relation	class_rel;
	Oid			toast_relid;
	Oid			namespaceid;
	char		toast_relname[NAMEDATALEN];
	char		toast_idxname[NAMEDATALEN];
	IndexInfo  *indexInfo;
	Oid			collationObjectId[2];
	Oid			classObjectId[2];
	int16		coloptions[2];
	ObjectAddress baseobject,
				toastobject;

	/*
	 * 是否已经有 toast？
	 */
	if (rel->rd_rel->reltoastrelid != InvalidOid)
		return false;

	/*
	 * 检查该表是否确实需要 TOAST 表。
	 */
	if (!IsBinaryUpgrade)
	{
		/* 普通模式，普通检查 */
		if (!needs_toast_table(rel))
			return false;
	}
	else
	{
		/*
		 * 在 binary-upgrade 模式下，当且仅当
		 * pg_upgrade 告诉我们要创建时才创建 TOAST 表（即已提供 TOAST 表 OID）。
		 *
		 * 这表明旧集簇中当前表有
		 * TOAST 表。我们必须创建 TOAST 表以接收旧的
		 * TOAST 文件，即使该表看起来并不需要。
		 *
		 * 反之，若旧集簇没有 TOAST 表，则
		 * 即便新版本的 needs_toast_table 规则建议应当有，
		 * 我们也应能不创建而继续。从我们会创建 TOAST 表的位置到
		 * 真正必须有一张以避免失败的位置之间，余地很大，
		 * 因此何时创建的启发式在跨版本上有小差异
		 * 不应成为问题。
		 * 若我们仍尝试创建 TOAST 表，就会有
		 * 占用某个 OID、与尚未见到的旧集簇表
		 * 冲突的问题。
		 */
		if (!OidIsValid(binary_upgrade_next_toast_pg_class_oid))
			return false;
	}

	/*
	 * 若请求则检查 lockmode 是否足够。这是对
	 * 先前代码中错误或冲突决策的交叉检查。
	 */
	if (check && lockmode != AccessExclusiveLock)
		elog(ERROR, "AccessExclusiveLock required to add toast table.");

	/*
	 * 创建 toast 表及其索引
	 */
	snprintf(toast_relname, sizeof(toast_relname),
			 "pg_toast_%u", relOid);
	snprintf(toast_idxname, sizeof(toast_idxname),
			 "pg_toast_%u_index", relOid);

	/* 这相当痛苦……需要一个元组描述符 */
	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "chunk_id",
					   OIDOID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "chunk_seq",
					   INT4OID,
					   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "chunk_data",
					   BYTEAOID,
					   -1, 0);

	/*
	 * 确保 toast 表自身不会再被 toast，否则我们就
	 * 完蛋了 :-(。这对 chunk_data 至关重要，因为类型 bytea 是
	 * 可 toast 的；另外两个也一并处理以确保无虞。
	 */
	TupleDescAttr(tupdesc, 0)->attstorage = TYPSTORAGE_PLAIN;
	TupleDescAttr(tupdesc, 1)->attstorage = TYPSTORAGE_PLAIN;
	TupleDescAttr(tupdesc, 2)->attstorage = TYPSTORAGE_PLAIN;

	/* Toast 字段不应被压缩 */
	TupleDescAttr(tupdesc, 0)->attcompression = InvalidCompressionMethod;
	TupleDescAttr(tupdesc, 1)->attcompression = InvalidCompressionMethod;
	TupleDescAttr(tupdesc, 2)->attcompression = InvalidCompressionMethod;

	/*
	 * 普通关系的 toast 表放在 pg_toast 中；临时
	 * 关系的则放入每后端的 temp-toast-table 命名空间。
	 */
	if (isTempOrTempToastNamespace(rel->rd_rel->relnamespace))
		namespaceid = GetTempToastNamespace();
	else
		namespaceid = PG_TOAST_NAMESPACE;

	/* Toast 表当且仅当其父表是共享的时才是共享的。 */
	shared_relation = rel->rd_rel->relisshared;

	/* 当且仅当其父表是 mapped 时，它也是 mapped */
	mapped_relation = RelationIsMapped(rel);

	toast_relid = heap_create_with_catalog(toast_relname,
										   namespaceid,
										   rel->rd_rel->reltablespace,
										   toastOid,
										   InvalidOid,
										   InvalidOid,
										   rel->rd_rel->relowner,
										   table_relation_toast_am(rel),
										   tupdesc,
										   NIL,
										   RELKIND_TOASTVALUE,
										   rel->rd_rel->relpersistence,
										   shared_relation,
										   mapped_relation,
										   ONCOMMIT_NOOP,
										   reloptions,
										   false,
										   true,
										   true,
										   OIDOldToast,
										   NULL);
	Assert(toast_relid != InvalidOid);

	/* 使 toast 关系可见，否则 table_open 会失败 */
	CommandCounterIncrement();

	/* 这里其实不需要 ShareLock，但还是取得它 */
	toast_rel = table_open(toast_relid, ShareLock);

	/*
	 * 在 chunk_id、chunk_seq 上创建唯一索引。
	 *
	 * 注意：普通 TOAST 访问例程实际上只需
	 * 仅在 chunk_id 上的单列 column 索引即可工作。不过，切片访问
	 * 例程用这两列以便更快访问单个 chunk。此外，
	 * 我们希望它是唯一的，以防止出现
	 * 重复的 TOAST chunk OIDs。这样索引可能也更
	 * 高效一些，因为 btree 不太喜欢大量
	 * 相等键。
	 */

	indexInfo = makeNode(IndexInfo);
	indexInfo->ii_NumIndexAttrs = 2;
	indexInfo->ii_NumIndexKeyAttrs = 2;
	indexInfo->ii_IndexAttrNumbers[0] = 1;
	indexInfo->ii_IndexAttrNumbers[1] = 2;
	indexInfo->ii_Expressions = NIL;
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_Predicate = NIL;
	indexInfo->ii_PredicateState = NULL;
	indexInfo->ii_ExclusionOps = NULL;
	indexInfo->ii_ExclusionProcs = NULL;
	indexInfo->ii_ExclusionStrats = NULL;
	indexInfo->ii_OpclassOptions = NULL;
	indexInfo->ii_Unique = true;
	indexInfo->ii_NullsNotDistinct = false;
	indexInfo->ii_ReadyForInserts = true;
	indexInfo->ii_CheckedUnchanged = false;
	indexInfo->ii_IndexUnchanged = false;
	indexInfo->ii_Concurrent = false;
	indexInfo->ii_BrokenHotChain = false;
	indexInfo->ii_ParallelWorkers = 0;
	indexInfo->ii_Am = BTREE_AM_OID;
	indexInfo->ii_AmCache = NULL;
	indexInfo->ii_Context = CurrentMemoryContext;

	collationObjectId[0] = InvalidOid;
	collationObjectId[1] = InvalidOid;

	classObjectId[0] = OID_BTREE_OPS_OID;
	classObjectId[1] = INT4_BTREE_OPS_OID;

	coloptions[0] = 0;
	coloptions[1] = 0;

	index_create(toast_rel, toast_idxname, toastIndexOid, InvalidOid,
				 InvalidOid, InvalidOid,
				 indexInfo,
				 list_make2("chunk_id", "chunk_seq"),
				 BTREE_AM_OID,
				 rel->rd_rel->reltablespace,
				 collationObjectId, classObjectId, coloptions, (Datum) 0,
				 INDEX_CREATE_IS_PRIMARY, 0, true, true, NULL);

	table_close(toast_rel, NoLock);

	/*
	 * 把 toast 表的 OID 存入父关系的 pg_class 行
	 */
	class_rel = table_open(RelationRelationId, RowExclusiveLock);

	if (!IsBootstrapProcessingMode())
	{
		/* 普通情况，使用事务性更新 */
		reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relOid));
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", relOid);

		((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid = toast_relid;

		CatalogTupleUpdate(class_rel, &reltup->t_self, reltup);
	}
	else
	{
		/* 引导期间无法 UPDATE，因此就地覆写 */

		ScanKeyData key[1];
		void	   *state;

		ScanKeyInit(&key[0],
					Anum_pg_class_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(relOid));
		systable_inplace_update_begin(class_rel, ClassOidIndexId, true,
									  NULL, 1, key, &reltup, &state);
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", relOid);

		((Form_pg_class) GETSTRUCT(reltup))->reltoastrelid = toast_relid;

		systable_inplace_update_finish(state, reltup);
	}

	heap_freetuple(reltup);

	table_close(class_rel, RowExclusiveLock);

	/*
	 * 注册从 toast 表到主表的依赖，以便主表
	 * 删除时 toast 表也被删除。引导模式下跳过。
	 */
	if (!IsBootstrapProcessingMode())
	{
		baseobject.classId = RelationRelationId;
		baseobject.objectId = relOid;
		baseobject.objectSubId = 0;
		toastobject.classId = RelationRelationId;
		toastobject.objectId = toast_relid;
		toastobject.objectSubId = 0;

		recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);
	}

	/*
	 * 使更改可见
	 */
	CommandCounterIncrement();

	return true;
}

/*
 * 检查该表是否需要 TOAST 表。
 */
static bool
needs_toast_table(Relation rel)
{
	/*
	 * 分区表无需创建 TOAST 表。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/*
	 * 在 initdb 之后不允许对共享关系做 toast（因为
	 * 无法在其他数据库的 pg_class 中标记它已 toast）。
	 */
	if (rel->rd_rel->relisshared && !IsBootstrapProcessingMode())
		return false;

	/*
	 * 忽略在 initdb 之后对目录表创建 toast 表的尝试。
	 * 哪些目录有 toast 表在 catalog/pg_*.h 中显式选定。
	 * （若目录没有 toast 表，我们可能经由某条 ALTER TABLE
	 * 命令到达这里。）
	 */
	if (IsCatalogRelation(rel) && !IsBootstrapProcessingMode())
		return false;

	/* 否则，让 AM 决定。 */
	return table_relation_needs_toast_table(rel);
}
