/*-------------------------------------------------------------------------
 *
 * pg_namespace.c
 *	  支持操作 pg_namespace 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_namespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* ----------------
 * NamespaceCreate
 *
 * 用给定名称与所有者 OID 创建命名空间（模式）。
 *
 * 若 isTemp 为 true，则该模式是每个后端用于存放
 * 临时表的模式。当前用于防止它被
 * 链接为任何活动扩展的成员。（若有人在扩展脚本中
 * 执行 CREATE TEMP TABLE，我们不希望临时模式
 * 成为扩展的一部分）。以及避免为临时命名空间
 * 检查默认 ACL（因为没有必要）。
 * ---------------
 */
Oid
NamespaceCreate(const char *nspName, Oid ownerId, bool isTemp)
{
	Relation	nspdesc;
	HeapTuple	tup;
	Oid			nspoid;
	bool		nulls[Natts_pg_namespace];
	Datum		values[Natts_pg_namespace];
	NameData	nname;
	TupleDesc	tupDesc;
	ObjectAddress myself;
	int			i;
	Acl		   *nspacl;

	/* 健全性检查 */
	if (!nspName)
		elog(ERROR, "no namespace name supplied");

	/* 确保不存在同名命名空间 */
	if (SearchSysCacheExists1(NAMESPACENAME, PointerGetDatum(nspName)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", nspName)));

	if (!isTemp)
		nspacl = get_user_default_acl(OBJECT_SCHEMA, ownerId,
									  InvalidOid);
	else
		nspacl = NULL;

	nspdesc = table_open(NamespaceRelationId, RowExclusiveLock);
	tupDesc = nspdesc->rd_att;

	/* 初始化 nulls 与 values */
	for (i = 0; i < Natts_pg_namespace; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	nspoid = GetNewOidWithIndex(nspdesc, NamespaceOidIndexId,
								Anum_pg_namespace_oid);
	values[Anum_pg_namespace_oid - 1] = ObjectIdGetDatum(nspoid);
	namestrcpy(&nname, nspName);
	values[Anum_pg_namespace_nspname - 1] = NameGetDatum(&nname);
	values[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(ownerId);
	if (nspacl != NULL)
		values[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(nspacl);
	else
		nulls[Anum_pg_namespace_nspacl - 1] = true;


	tup = heap_form_tuple(tupDesc, values, nulls);

	CatalogTupleInsert(nspdesc, tup);
	Assert(OidIsValid(nspoid));

	table_close(nspdesc, RowExclusiveLock);

	/* 记录依赖 */
	myself.classId = NamespaceRelationId;
	myself.objectId = nspoid;
	myself.objectSubId = 0;

	/* 对所有者的依赖 */
	recordDependencyOnOwner(NamespaceRelationId, nspoid, ownerId);

	/* 对默认 ACL 中提及的角色的依赖 */
	recordDependencyOnNewAcl(NamespaceRelationId, nspoid, 0, ownerId, nspacl);

	/* 对扩展的依赖……但对魔术临时模式除外 */
	if (!isTemp)
		recordDependencyOnCurrentExtension(&myself, false);

	/* 新模式的创建后钩子 */
	InvokeObjectPostCreateHook(NamespaceRelationId, nspoid, 0);

	return nspoid;
}
