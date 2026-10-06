/*-------------------------------------------------------------------------
 *
 * pg_conversion.c
 *	  支持操作 pg_conversion 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_conversion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * ConversionCreate
 *
 * 向 pg_conversion 添加新元组。
 */
ObjectAddress
ConversionCreate(const char *conname, Oid connamespace,
				 Oid conowner,
				 int32 conforencoding, int32 contoencoding,
				 Oid conproc, bool def)
{
	int			i;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Oid			oid;
	bool		nulls[Natts_pg_conversion];
	Datum		values[Natts_pg_conversion];
	NameData	cname;
	ObjectAddress myself,
				referenced;

	/* 健全性检查 */
	if (!conname)
		elog(ERROR, "no conversion name supplied");

	/* 确保不存在同名转换 */
	if (SearchSysCacheExists2(CONNAMENSP,
							  PointerGetDatum(conname),
							  ObjectIdGetDatum(connamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("conversion \"%s\" already exists", conname)));

	if (def)
	{
		/*
		 * 确保此命名空间中不存在默认的
		 * <源编码><目标编码> 对
		 */
		if (FindDefaultConversion(connamespace,
								  conforencoding,
								  contoencoding))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("default conversion for %s to %s already exists",
							pg_encoding_to_char(conforencoding),
							pg_encoding_to_char(contoencoding))));
	}

	/* 打开 pg_conversion */
	rel = table_open(ConversionRelationId, RowExclusiveLock);
	tupDesc = rel->rd_att;

	/* 初始化 nulls 与 values */
	for (i = 0; i < Natts_pg_conversion; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	/* 构造元组 */
	namestrcpy(&cname, conname);
	oid = GetNewOidWithIndex(rel, ConversionOidIndexId,
							 Anum_pg_conversion_oid);
	values[Anum_pg_conversion_oid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_conversion_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_conversion_connamespace - 1] = ObjectIdGetDatum(connamespace);
	values[Anum_pg_conversion_conowner - 1] = ObjectIdGetDatum(conowner);
	values[Anum_pg_conversion_conforencoding - 1] = Int32GetDatum(conforencoding);
	values[Anum_pg_conversion_contoencoding - 1] = Int32GetDatum(contoencoding);
	values[Anum_pg_conversion_conproc - 1] = ObjectIdGetDatum(conproc);
	values[Anum_pg_conversion_condefault - 1] = BoolGetDatum(def);

	tup = heap_form_tuple(tupDesc, values, nulls);

	/* 插入新元组 */
	CatalogTupleInsert(rel, tup);

	myself.classId = ConversionRelationId;
	myself.objectId = oid;
	myself.objectSubId = 0;

	/* 创建对转换过程的依赖 */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = conproc;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 创建对命名空间的依赖 */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = connamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 创建对所有者的依赖 */
	recordDependencyOnOwner(ConversionRelationId, oid, conowner);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新转换的创建后钩子 */
	InvokeObjectPostCreateHook(ConversionRelationId, oid, 0);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * FindDefaultConversion
 *
 * 在给定命名空间中按 for_encoding 与 to_encoding
 * 查找"默认"转换过程。
 *
 * 若找到，返回过程的 oid，否则返回 InvalidOid。注意
 * 得到的是过程的 OID，而不是转换的 OID！
 */
Oid
FindDefaultConversion(Oid name_space, int32 for_encoding, int32 to_encoding)
{
	CatCList   *catlist;
	HeapTuple	tuple;
	Form_pg_conversion body;
	Oid			proc = InvalidOid;
	int			i;

	catlist = SearchSysCacheList3(CONDEFAULT,
								  ObjectIdGetDatum(name_space),
								  Int32GetDatum(for_encoding),
								  Int32GetDatum(to_encoding));

	for (i = 0; i < catlist->n_members; i++)
	{
		tuple = &catlist->members[i]->tuple;
		body = (Form_pg_conversion) GETSTRUCT(tuple);
		if (body->condefault)
		{
			proc = body->conproc;
			break;
		}
	}
	ReleaseSysCacheList(catlist);
	return proc;
}
