/*-------------------------------------------------------------------------
 *
 * comment.c
 *
 * PostgreSQL 对象注释工具代码。
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/comment.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_description.h"
#include "catalog/pg_shdescription.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * CommentObject --
 *
 * 此例程用于为给定 SQL 命令指定的对象，把相关联的注释
 * 加入 pg_description。
 */
ObjectAddress
CommentObject(CommentStmt *stmt)
{
	Relation	relation;
	ObjectAddress address = InvalidObjectAddress;

	/*
	 * 装载转储时，我们可能见到针对数据库
	 * 旧名称的 COMMENT ON DATABASE。报错会阻止
	 * pg_restore 完成（这其实是 pg_restore 的问题，
	 * 但眼下我们先在这里绕开这个问题）。共识是：
	 * 最佳修复是把错误的数据库名称当作 WARNING 而不是
	 * ERROR；因此有了下面的特殊情况。
	 */
	if (stmt->objtype == OBJECT_DATABASE)
	{
		char	   *database = strVal(stmt->object);

		if (!OidIsValid(get_database_oid(database, true)))
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", database)));
			return address;
		}
	}

	/*
	 * 把标识该对象的解析器表示转换为
	 * ObjectAddress。get_object_address() 会在对象
	 * 不存在时抛出错误，还会在目标上
	 * 获取锁，以防并发的 DROP 操作。
	 */
	address = get_object_address(stmt->objtype, stmt->object,
								 &relation, ShareUpdateExclusiveLock, false);

	/* 必须拥有目标对象的所有权。 */
	check_object_ownership(GetUserId(), stmt->objtype, address,
						   stmt->object, relation);

	/* 按需执行其他完整性检查。 */
	switch (stmt->objtype)
	{
		case OBJECT_COLUMN:

			/*
			 * 只允许对表、视图、物化视图、
			 * 复合类型和外部表的列添加注释
			 * （只有这些 relkind 会被 pg_dump 转储
			 * 逐列注释）。我们尤其希望禁止
			 * 对索引列添加注释，因为索引列的
			 * 名称可能随 PG 版本变化，所以转储
			 * 逐列注释可能造成重新装载失败。
			 */
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_VIEW &&
				relation->rd_rel->relkind != RELKIND_MATVIEW &&
				relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
				relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot set comment on relation \"%s\"",
								RelationGetRelationName(relation)),
						 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
			break;
		default:
			break;
	}

	/*
	 * 数据库、表空间和角色是集簇范围的对象，所以
	 * 这些对象上的任何注释都记录在共享的 pg_shdescription
	 * 目录中。其他所有对象的注释记录在 pg_description 中。
	 */
	if (stmt->objtype == OBJECT_DATABASE || stmt->objtype == OBJECT_TABLESPACE
		|| stmt->objtype == OBJECT_ROLE)
		CreateSharedComments(address.objectId, address.classId, stmt->comment);
	else
		CreateComments(address.objectId, address.classId, address.objectSubId,
					   stmt->comment);

	/*
	 * 若 get_object_address() 替我们打开了关系，
	 * 就关闭它，以保持引用计数正确——但我们保留
	 * get_object_address() 获取的任何锁直到提交时刻，
	 * 以防并发活动。
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return address;
}

/*
 * CreateComments --
 *
 * 为指定的对象描述符创建注释。插入一条新的
 * pg_description 元组，或替换具有相同键的现有元组。
 *
 * 若给定注释为 null 或空字符串，则改为删除
 * 该键的任何现有注释。
 */
void
CreateComments(Oid oid, Oid classoid, int32 subid, const char *comment)
{
	Relation	description;
	ScanKeyData skey[3];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	HeapTuple	newtuple = NULL;
	Datum		values[Natts_pg_description];
	bool		nulls[Natts_pg_description];
	bool		replaces[Natts_pg_description];
	int			i;

	/* 把空字符串归约为 NULL 情况 */
	if (comment != NULL && strlen(comment) == 0)
		comment = NULL;

	/* 如有必要，准备构造或更新元组 */
	if (comment != NULL)
	{
		for (i = 0; i < Natts_pg_description; i++)
		{
			nulls[i] = false;
			replaces[i] = true;
		}
		values[Anum_pg_description_objoid - 1] = ObjectIdGetDatum(oid);
		values[Anum_pg_description_classoid - 1] = ObjectIdGetDatum(classoid);
		values[Anum_pg_description_objsubid - 1] = Int32GetDatum(subid);
		values[Anum_pg_description_description - 1] = CStringGetTextDatum(comment);
	}

	/* 使用索引搜索匹配的旧元组 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&skey[2],
				Anum_pg_description_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(subid));

	description = table_open(DescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, 3, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
	{
		/* 找到了旧元组，删除或更新它 */

		if (comment == NULL)
			CatalogTupleDelete(description, &oldtuple->t_self);
		else
		{
			newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(description), values,
										 nulls, replaces);
			CatalogTupleUpdate(description, &oldtuple->t_self, newtuple);
		}

		break;					/* 假定只可能有一个匹配 */
	}

	systable_endscan(sd);

	/* 若未找到旧元组，就插入新元组 */

	if (newtuple == NULL && comment != NULL)
	{
		newtuple = heap_form_tuple(RelationGetDescr(description),
								   values, nulls);
		CatalogTupleInsert(description, newtuple);
	}

	if (newtuple != NULL)
		heap_freetuple(newtuple);

	/* 完成 */

	table_close(description, NoLock);
}

/*
 * CreateSharedComments --
 *
 * 为指定的共享对象描述符创建注释。插入一条
 * 新的 pg_shdescription 元组，或替换具有相同键的现有元组。
 *
 * 若给定注释为 null 或空字符串，则改为删除
 * 该键的任何现有注释。
 */
void
CreateSharedComments(Oid oid, Oid classoid, const char *comment)
{
	Relation	shdescription;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	HeapTuple	newtuple = NULL;
	Datum		values[Natts_pg_shdescription];
	bool		nulls[Natts_pg_shdescription];
	bool		replaces[Natts_pg_shdescription];
	int			i;

	/* 把空字符串归约为 NULL 情况 */
	if (comment != NULL && strlen(comment) == 0)
		comment = NULL;

	/* 如有必要，准备构造或更新元组 */
	if (comment != NULL)
	{
		for (i = 0; i < Natts_pg_shdescription; i++)
		{
			nulls[i] = false;
			replaces[i] = true;
		}
		values[Anum_pg_shdescription_objoid - 1] = ObjectIdGetDatum(oid);
		values[Anum_pg_shdescription_classoid - 1] = ObjectIdGetDatum(classoid);
		values[Anum_pg_shdescription_description - 1] = CStringGetTextDatum(comment);
	}

	/* 使用索引搜索匹配的旧元组 */

	ScanKeyInit(&skey[0],
				Anum_pg_shdescription_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_shdescription_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	shdescription = table_open(SharedDescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(shdescription, SharedDescriptionObjIndexId, true,
							NULL, 2, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
	{
		/* 找到了旧元组，删除或更新它 */

		if (comment == NULL)
			CatalogTupleDelete(shdescription, &oldtuple->t_self);
		else
		{
			newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(shdescription),
										 values, nulls, replaces);
			CatalogTupleUpdate(shdescription, &oldtuple->t_self, newtuple);
		}

		break;					/* 假定只可能有一个匹配 */
	}

	systable_endscan(sd);

	/* 若未找到旧元组，就插入新元组 */

	if (newtuple == NULL && comment != NULL)
	{
		newtuple = heap_form_tuple(RelationGetDescr(shdescription),
								   values, nulls);
		CatalogTupleInsert(shdescription, newtuple);
	}

	if (newtuple != NULL)
		heap_freetuple(newtuple);

	/* 完成 */

	table_close(shdescription, NoLock);
}

/*
 * DeleteComments -- 删除对象的注释
 *
 * 若 subid 非零，则只删除与之匹配的注释。
 * 若 subid 为零，则删除与 oid/classoid 匹配的所有注释
 * （这相当于删除整个对象）。
 */
void
DeleteComments(Oid oid, Oid classoid, int32 subid)
{
	Relation	description;
	ScanKeyData skey[3];
	int			nkeys;
	SysScanDesc sd;
	HeapTuple	oldtuple;

	/* 使用索引搜索所有匹配的旧元组 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	if (subid != 0)
	{
		ScanKeyInit(&skey[2],
					Anum_pg_description_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(subid));
		nkeys = 3;
	}
	else
		nkeys = 2;

	description = table_open(DescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, nkeys, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
		CatalogTupleDelete(description, &oldtuple->t_self);

	/* 完成 */

	systable_endscan(sd);
	table_close(description, RowExclusiveLock);
}

/*
 * DeleteSharedComments -- 删除共享对象的注释
 */
void
DeleteSharedComments(Oid oid, Oid classoid)
{
	Relation	shdescription;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;

	/* 使用索引搜索所有匹配的旧元组 */

	ScanKeyInit(&skey[0],
				Anum_pg_shdescription_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_shdescription_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));

	shdescription = table_open(SharedDescriptionRelationId, RowExclusiveLock);

	sd = systable_beginscan(shdescription, SharedDescriptionObjIndexId, true,
							NULL, 2, skey);

	while ((oldtuple = systable_getnext(sd)) != NULL)
		CatalogTupleDelete(shdescription, &oldtuple->t_self);

	/* 完成 */

	systable_endscan(sd);
	table_close(shdescription, RowExclusiveLock);
}

/*
 * GetComment -- 获取对象的注释，若未找到则为 null。
 */
char *
GetComment(Oid oid, Oid classoid, int32 subid)
{
	Relation	description;
	ScanKeyData skey[3];
	SysScanDesc sd;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	char	   *comment;

	/* 使用索引搜索匹配的旧元组 */

	ScanKeyInit(&skey[0],
				Anum_pg_description_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	ScanKeyInit(&skey[1],
				Anum_pg_description_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&skey[2],
				Anum_pg_description_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(subid));

	description = table_open(DescriptionRelationId, AccessShareLock);
	tupdesc = RelationGetDescr(description);

	sd = systable_beginscan(description, DescriptionObjIndexId, true,
							NULL, 3, skey);

	comment = NULL;
	while ((tuple = systable_getnext(sd)) != NULL)
	{
		Datum		value;
		bool		isnull;

		/* 找到了元组，获取 description 字段 */
		value = heap_getattr(tuple, Anum_pg_description_description, tupdesc, &isnull);
		if (!isnull)
			comment = TextDatumGetCString(value);
		break;					/* 假定只可能有一个匹配 */
	}

	systable_endscan(sd);

	/* 完成 */
	table_close(description, AccessShareLock);

	return comment;
}
