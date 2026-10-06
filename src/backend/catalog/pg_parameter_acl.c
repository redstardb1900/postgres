/*-------------------------------------------------------------------------
 *
 * pg_parameter_acl.c
 *	  支持操作 pg_parameter_acl 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_parameter_acl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_parameter_acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * ParameterAclLookup - 给定配置参数名，
 * 查找关联的配置参数 ACL 的 OID。
 *
 * 若 missing_ok 为 false，找不到 ACL 条目时抛错。若
 * 为 true，则仅返回 InvalidOid。
 */
Oid
ParameterAclLookup(const char *parameter, bool missing_ok)
{
	Oid			oid;
	char	   *parname;

	/* 把名称转换成应在 pg_parameter_acl 中使用的形式... */
	parname = convert_GUC_name_for_parameter_acl(parameter);

	/* ...然后查找它 */
	oid = GetSysCacheOid1(PARAMETERACLNAME, Anum_pg_parameter_acl_oid,
						  PointerGetDatum(cstring_to_text(parname)));

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("parameter ACL \"%s\" does not exist", parameter)));

	pfree(parname);

	return oid;
}

/*
 * ParameterAclCreate
 *
 * 向 pg_parameter_acl 添加新元组。
 *
 * parameter: 要为其创建条目的参数名。
 * 调用者应已确认尚无此类条目。
 *
 * 返回新条目的 OID。
 */
Oid
ParameterAclCreate(const char *parameter)
{
	Oid			parameterId;
	char	   *parname;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tuple;
	Datum		values[Natts_pg_parameter_acl] = {0};
	bool		nulls[Natts_pg_parameter_acl] = {0};

	/*
	 * 为避免用无用条目弄乱 pg_parameter_acl，要求
	 * 名称必须有效。
	 */
	if (!check_GUC_name_for_parameter_acl(parameter))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid parameter name \"%s\"",
						parameter)));

	/* 把名称转换成应在 pg_parameter_acl 中使用的形式。 */
	parname = convert_GUC_name_for_parameter_acl(parameter);

	/*
	 * 创建并插入一条含空 ACL 的新记录。
	 *
	 * 我们不取足以阻止并发插入的强锁，
	 * 而是依赖唯一索引。
	 */
	rel = table_open(ParameterAclRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);
	parameterId = GetNewOidWithIndex(rel,
									 ParameterAclOidIndexId,
									 Anum_pg_parameter_acl_oid);
	values[Anum_pg_parameter_acl_oid - 1] = ObjectIdGetDatum(parameterId);
	values[Anum_pg_parameter_acl_parname - 1] =
		PointerGetDatum(cstring_to_text(parname));
	nulls[Anum_pg_parameter_acl_paracl - 1] = true;
	tuple = heap_form_tuple(tupDesc, values, nulls);
	CatalogTupleInsert(rel, tuple);

	/* 关闭 pg_parameter_acl，但保持锁直到提交。 */
	heap_freetuple(tuple);
	table_close(rel, NoLock);

	return parameterId;
}
