/*-------------------------------------------------------------------------
 *
 * pg_cast.c
 *	  支持操作 pg_cast 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_cast.c
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
#include "catalog/pg_cast.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * ----------------------------------------------------------------
 *		CastCreate
 *
 * 为正在创建的新类型转换构造并插入目录元组。
 * 调用者必须已检查权限，并对给定数据类型与
 * 类型转换函数（若适用）做完一致性检查。
 *
 * 由于我们允许数据类型与类型转换函数的输入和结果
 * 二进制可强制转换，因此可能有一两个本转换所依赖的
 * WITHOUT FUNCTION 类型转换。我们不在 pg_cast 中显式
 * 记录这一点，但仍需建立对这些类型转换的依赖。
 *
 * 'behavior' 表示新类型转换将对其输入与输出类型、
 * 类型转换函数、以及（若有）其他类型转换所具有的
 * 依赖类型。
 * ----------------------------------------------------------------
 */
ObjectAddress
CastCreate(Oid sourcetypeid, Oid targettypeid,
		   Oid funcid, Oid incastid, Oid outcastid,
		   char castcontext, char castmethod, DependencyType behavior)
{
	Relation	relation;
	HeapTuple	tuple;
	Oid			castid;
	Datum		values[Natts_pg_cast];
	bool		nulls[Natts_pg_cast] = {0};
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	relation = table_open(CastRelationId, RowExclusiveLock);

	/*
	 * 检查是否重复。这只是为了给出友好的错误消息，
	 * 唯一索引反正也会捕获（因此无需担心竞态
	 * 条件）。
	 */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(sourcetypeid),
							ObjectIdGetDatum(targettypeid));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("cast from type %s to type %s already exists",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));

	/* 准备就绪 */
	castid = GetNewOidWithIndex(relation, CastOidIndexId, Anum_pg_cast_oid);
	values[Anum_pg_cast_oid - 1] = ObjectIdGetDatum(castid);
	values[Anum_pg_cast_castsource - 1] = ObjectIdGetDatum(sourcetypeid);
	values[Anum_pg_cast_casttarget - 1] = ObjectIdGetDatum(targettypeid);
	values[Anum_pg_cast_castfunc - 1] = ObjectIdGetDatum(funcid);
	values[Anum_pg_cast_castcontext - 1] = CharGetDatum(castcontext);
	values[Anum_pg_cast_castmethod - 1] = CharGetDatum(castmethod);

	tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);

	CatalogTupleInsert(relation, tuple);

	addrs = new_object_addresses();

	/* 建立依赖项 */
	ObjectAddressSet(myself, CastRelationId, castid);

	/* 对源类型的依赖 */
	ObjectAddressSet(referenced, TypeRelationId, sourcetypeid);
	add_exact_object_address(&referenced, addrs);

	/* 对目标类型的依赖 */
	ObjectAddressSet(referenced, TypeRelationId, targettypeid);
	add_exact_object_address(&referenced, addrs);

	/* 对函数的依赖 */
	if (OidIsValid(funcid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, funcid);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对函数所需类型转换的依赖 */
	if (OidIsValid(incastid))
	{
		ObjectAddressSet(referenced, CastRelationId, incastid);
		add_exact_object_address(&referenced, addrs);
	}
	if (OidIsValid(outcastid))
	{
		ObjectAddressSet(referenced, CastRelationId, outcastid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, behavior);
	free_object_addresses(addrs);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新类型转换的创建后钩子 */
	InvokeObjectPostCreateHook(CastRelationId, castid, 0);

	heap_freetuple(tuple);

	table_close(relation, RowExclusiveLock);

	return myself;
}
