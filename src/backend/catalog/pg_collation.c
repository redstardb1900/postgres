/*-------------------------------------------------------------------------
 *
 * pg_collation.c
 *	  支持操作 pg_collation 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_collation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CollationCreate
 *
 * 向 pg_collation 添加新元组。
 *
 * if_not_exists: 若为 true，重名时不失败，仅打印 notice
 * 并返回 InvalidOid。
 * quiet: 若为 true，重名时不失败，仅静默返回
 * InvalidOid（覆盖 if_not_exists）。
 */
Oid
CollationCreate(const char *collname, Oid collnamespace,
				Oid collowner,
				char collprovider,
				bool collisdeterministic,
				int32 collencoding,
				const char *collcollate, const char *collctype,
				const char *colliculocale,
				const char *collicurules,
				const char *collversion,
				bool if_not_exists,
				bool quiet)
{
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Datum		values[Natts_pg_collation];
	bool		nulls[Natts_pg_collation];
	NameData	name_name;
	Oid			oid;
	ObjectAddress myself,
				referenced;

	Assert(collname);
	Assert(collnamespace);
	Assert(collowner);
	Assert((collcollate && collctype) || colliculocale);

	/*
	 * 确保不存在同名且同编码的排序规则。
	 *
	 * 唯一索引反正也会捕获；我们只是给出更
	 * 友好的错误消息。唯一索引为竞态条件提供
	 * 后盾。
	 */
	oid = GetSysCacheOid3(COLLNAMEENCNSP,
						  Anum_pg_collation_oid,
						  PointerGetDatum(collname),
						  Int32GetDatum(collencoding),
						  ObjectIdGetDatum(collnamespace));
	if (OidIsValid(oid))
	{
		if (quiet)
			return InvalidOid;
		else if (if_not_exists)
		{
			/*
			 * 若处于扩展脚本中，要求预先存在的
			 * 对象是该扩展的成员，以避免安全风险。
			 */
			ObjectAddressSet(myself, CollationRelationId, oid);
			checkMembershipInCurrentExtension(&myself);

			/* 可以跳过 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 collencoding == -1
					 ? errmsg("collation \"%s\" already exists, skipping",
							  collname)
					 : errmsg("collation \"%s\" for encoding \"%s\" already exists, skipping",
							  collname, pg_encoding_to_char(collencoding))));
			return InvalidOid;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 collencoding == -1
					 ? errmsg("collation \"%s\" already exists",
							  collname)
					 : errmsg("collation \"%s\" for encoding \"%s\" already exists",
							  collname, pg_encoding_to_char(collencoding))));
	}

	/* 打开 pg_collation；锁级别见下文 */
	rel = table_open(CollationRelationId, ShareRowExclusiveLock);

	/*
	 * 也禁止特定编码的排序规则遮蔽任意编码的
	 * 排序规则，或任意编码的排序规则被遮蔽（见
 * get_collation_name()）。此检查没有唯一索引作后盾，
 * 因此我们更早取得 ShareRowExclusiveLock，以防
 * 并发更改愚弄此检查。
 */
	if (collencoding == -1)
		oid = GetSysCacheOid3(COLLNAMEENCNSP,
							  Anum_pg_collation_oid,
							  PointerGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(collnamespace));
	else
		oid = GetSysCacheOid3(COLLNAMEENCNSP,
							  Anum_pg_collation_oid,
							  PointerGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(collnamespace));
	if (OidIsValid(oid))
	{
		if (quiet)
		{
			table_close(rel, NoLock);
			return InvalidOid;
		}
		else if (if_not_exists)
		{
			/*
			 * 若处于扩展脚本中，要求预先存在的
			 * 对象是该扩展的成员，以避免安全风险。
			 */
			ObjectAddressSet(myself, CollationRelationId, oid);
			checkMembershipInCurrentExtension(&myself);

			/* 可以跳过 */
			table_close(rel, NoLock);
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("collation \"%s\" already exists, skipping",
							collname)));
			return InvalidOid;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("collation \"%s\" already exists",
							collname)));
	}

	tupDesc = RelationGetDescr(rel);

	/* 构造元组 */
	memset(nulls, 0, sizeof(nulls));

	namestrcpy(&name_name, collname);
	oid = GetNewOidWithIndex(rel, CollationOidIndexId,
							 Anum_pg_collation_oid);
	values[Anum_pg_collation_oid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_collation_collname - 1] = NameGetDatum(&name_name);
	values[Anum_pg_collation_collnamespace - 1] = ObjectIdGetDatum(collnamespace);
	values[Anum_pg_collation_collowner - 1] = ObjectIdGetDatum(collowner);
	values[Anum_pg_collation_collprovider - 1] = CharGetDatum(collprovider);
	values[Anum_pg_collation_collisdeterministic - 1] = BoolGetDatum(collisdeterministic);
	values[Anum_pg_collation_collencoding - 1] = Int32GetDatum(collencoding);
	if (collcollate)
		values[Anum_pg_collation_collcollate - 1] = CStringGetTextDatum(collcollate);
	else
		nulls[Anum_pg_collation_collcollate - 1] = true;
	if (collctype)
		values[Anum_pg_collation_collctype - 1] = CStringGetTextDatum(collctype);
	else
		nulls[Anum_pg_collation_collctype - 1] = true;
	if (colliculocale)
		values[Anum_pg_collation_colliculocale - 1] = CStringGetTextDatum(colliculocale);
	else
		nulls[Anum_pg_collation_colliculocale - 1] = true;
	if (collicurules)
		values[Anum_pg_collation_collicurules - 1] = CStringGetTextDatum(collicurules);
	else
		nulls[Anum_pg_collation_collicurules - 1] = true;
	if (collversion)
		values[Anum_pg_collation_collversion - 1] = CStringGetTextDatum(collversion);
	else
		nulls[Anum_pg_collation_collversion - 1] = true;

	tup = heap_form_tuple(tupDesc, values, nulls);

	/* 插入新元组 */
	CatalogTupleInsert(rel, tup);
	Assert(OidIsValid(oid));

	/* 为新排序规则建立依赖 */
	myself.classId = CollationRelationId;
	myself.objectId = oid;
	myself.objectSubId = 0;

	/* 创建对命名空间的依赖 */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = collnamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 创建对所有者的依赖 */
	recordDependencyOnOwner(CollationRelationId, oid, collowner);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新排序规则的创建后钩子 */
	InvokeObjectPostCreateHook(CollationRelationId, oid, 0);

	heap_freetuple(tup);
	table_close(rel, NoLock);

	return oid;
}
