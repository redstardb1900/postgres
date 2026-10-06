/*-------------------------------------------------------------------------
 *
 * pg_attrdef.c
 *	  支持操作 pg_attrdef 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_attrdef.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "executor/executor.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 为关系 rel 的列 attnum 存储默认值表达式。
 *
 * 返回新 pg_attrdef 元组的 OID。
 *
 * 若正在为新属性存储默认值，add_column_mode 必须为 true；
 * 若属性已存在则为 false。原因
 * 是缺失值一旦设置就绝不能再更新，
 * 而这只能在向表添加列时发生。否则我们就会
 * 实际上在改变已有元组。
 */
Oid
StoreAttrDefault(Relation rel, AttrNumber attnum,
				 Node *expr, bool is_internal, bool add_column_mode)
{
	char	   *adbin;
	Relation	adrel;
	HeapTuple	tuple;
	Datum		values[4];
	static bool nulls[4] = {false, false, false, false};
	Relation	attrrel;
	HeapTuple	atttup;
	Form_pg_attribute attStruct;
	char		attgenerated;
	Oid			attrdefOid;
	ObjectAddress colobject,
				defobject;

	adrel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/*
	 * 将表达式展平为字符串形式以便存储。
	 */
	adbin = nodeToString(expr);

	/*
	 * 创建 pg_attrdef 条目。
	 */
	attrdefOid = GetNewOidWithIndex(adrel, AttrDefaultOidIndexId,
									Anum_pg_attrdef_oid);
	values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(attrdefOid);
	values[Anum_pg_attrdef_adrelid - 1] = RelationGetRelid(rel);
	values[Anum_pg_attrdef_adnum - 1] = attnum;
	values[Anum_pg_attrdef_adbin - 1] = CStringGetTextDatum(adbin);

	tuple = heap_form_tuple(adrel->rd_att, values, nulls);
	CatalogTupleInsert(adrel, tuple);

	defobject.classId = AttrDefaultRelationId;
	defobject.objectId = attrdefOid;
	defobject.objectSubId = 0;

	table_close(adrel, RowExclusiveLock);

	/* 现在可以释放上面分配的一些东西了 */
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	heap_freetuple(tuple);
	pfree(adbin);

	/*
	 * 更新该列的 pg_attribute 条目，以表明存在
	 * 默认值。
	 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheCopy2(ATTNUM,
								 ObjectIdGetDatum(RelationGetRelid(rel)),
								 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	attgenerated = attStruct->attgenerated;
	if (!attStruct->atthasdef)
	{
		Form_pg_attribute defAttStruct;

		ExprState  *exprState;
		Expr	   *expr2 = (Expr *) expr;
		EState	   *estate = NULL;
		ExprContext *econtext;
		Datum		valuesAtt[Natts_pg_attribute] = {0};
		bool		nullsAtt[Natts_pg_attribute] = {0};
		bool		replacesAtt[Natts_pg_attribute] = {0};
		Datum		missingval = (Datum) 0;
		bool		missingIsNull = true;

		valuesAtt[Anum_pg_attribute_atthasdef - 1] = true;
		replacesAtt[Anum_pg_attribute_atthasdef - 1] = true;

		/*
		 * 注意：就核心 Postgres 而言，这段代码已经是死代码，
		 * 因为不再有调用方传入 add_column_mode = true。我们在
		 * 后备分支中保留它，以防某个扩展
		 * 仍在依赖它。
		 */
		if (rel->rd_rel->relkind == RELKIND_RELATION && add_column_mode &&
			!attgenerated)
		{
			expr2 = expression_planner(expr2);
			estate = CreateExecutorState();
			exprState = ExecPrepareExpr(expr2, estate);
			econtext = GetPerTupleExprContext(estate);

			missingval = ExecEvalExpr(exprState, econtext,
									  &missingIsNull);

			FreeExecutorState(estate);

			defAttStruct = TupleDescAttr(rel->rd_att, attnum - 1);

			if (missingIsNull)
			{
				/* 若默认值求值为 NULL，则只存储一个 NULL 数组 */
				missingval = (Datum) 0;
			}
			else
			{
				/* 否则构造仅含该值的单元素数组 */
				missingval = PointerGetDatum(construct_array(&missingval,
															 1,
															 defAttStruct->atttypid,
															 defAttStruct->attlen,
															 defAttStruct->attbyval,
															 defAttStruct->attalign));
			}

			valuesAtt[Anum_pg_attribute_atthasmissing - 1] = !missingIsNull;
			replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = missingIsNull;
		}
		atttup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
								   valuesAtt, nullsAtt, replacesAtt);

		CatalogTupleUpdate(attrrel, &atttup->t_self, atttup);

		if (!missingIsNull)
			pfree(DatumGetPointer(missingval));
	}
	table_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);

	/*
	 * 建立依赖，使列（或整张表）被删除时
	 * pg_attrdef 条目也随之消失。对于生成列，
	 * 做成内部依赖，以防止默认值表达式被
	 * 单独删除。
	 */
	colobject.classId = RelationRelationId;
	colobject.objectId = RelationGetRelid(rel);
	colobject.objectSubId = attnum;

	recordDependencyOn(&defobject, &colobject,
					   attgenerated ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

	/*
	 * 也记录对表达式中所用对象的依赖。
	 */
	recordDependencyOnSingleRelExpr(&defobject, expr, RelationGetRelid(rel),
									DEPENDENCY_NORMAL,
									DEPENDENCY_NORMAL, false);

	/*
	 * 属性默认值的创建后钩子。
	 *
	 * XXX. ALTER TABLE ALTER COLUMN SET/DROP DEFAULT 是通过对
	 * 该属性的默认值条目做几次删除/创建来实现的，因此
	 * 若被调用方需要区分，应检查是否存在该条目的
	 * 较旧版本。
	 */
	InvokeObjectPostCreateHookArg(AttrDefaultRelationId,
								  RelationGetRelid(rel), attnum, is_internal);

	return attrdefOid;
}


/*
 *		RemoveAttrDefault
 *
 * 若指定关系/属性有默认值，则移除它。
 * （若无默认值，complain 为 true 时报错，否则安静返回。）
 */
void
RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain, bool internal)
{
	Relation	attrdef_rel;
	ScanKeyData scankeys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&scankeys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(attrdef_rel, AttrDefaultIndexId, true,
							  NULL, 2, scankeys);

	/* 最多应有一个匹配元组，但我们仍循环 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress object;
		Form_pg_attrdef attrtuple = (Form_pg_attrdef) GETSTRUCT(tuple);

		object.classId = AttrDefaultRelationId;
		object.objectId = attrtuple->oid;
		object.objectSubId = 0;

		performDeletion(&object, behavior,
						internal ? PERFORM_DELETION_INTERNAL : 0);

		found = true;
	}

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	if (complain && !found)
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 relid, attnum);
}

/*
 *		RemoveAttrDefaultById
 *
 * 按 OID 移除 pg_attrdef 条目。这是
 * 属性默认值移除的核心。注意应通过 performDeletion
 * 调用，而非直接调用。
 */
void
RemoveAttrDefaultById(Oid attrdefId)
{
	Relation	attrdef_rel;
	Relation	attr_rel;
	Relation	myrel;
	ScanKeyData scankeys[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			myrelid;
	AttrNumber	myattnum;

	/* 在 pg_attrdef 关系上取得适当的锁 */
	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/* 找到 pg_attrdef 元组 */
	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefId));

	scan = systable_beginscan(attrdef_rel, AttrDefaultOidIndexId, true,
							  NULL, 1, scankeys);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for attrdef %u", attrdefId);

	myrelid = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
	myattnum = ((Form_pg_attrdef) GETSTRUCT(tuple))->adnum;

	/* 在拥有该属性的关系上取得排他锁 */
	myrel = relation_open(myrelid, AccessExclusiveLock);

	/* 现在可以删除 pg_attrdef 行了 */
	CatalogTupleDelete(attrdef_rel, &tuple->t_self);

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	/* 修正 pg_attribute 行 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(myrelid),
								Int16GetDatum(myattnum));
	if (!HeapTupleIsValid(tuple))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 myattnum, myrelid);

	((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = false;

	CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

	/*
	 * 我们对 pg_attribute 行的更新会强制重建 relcache，因此
	 * 这里没有别的事情要做。
	 */
	table_close(attr_rel, RowExclusiveLock);

	/* 在事务结束前保持对属性所属关系的锁 */
	relation_close(myrel, NoLock);
}


/*
 * 获取由关系 OID 和列号标识的列的
 * 默认值表达式的 pg_attrdef OID。
 *
 * 若没有这样的 pg_attrdef 条目则返回 InvalidOid。
 */
Oid
GetAttrDefaultOid(Oid relid, AttrNumber attnum)
{
	Oid			result = InvalidOid;
	Relation	attrdef;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&keys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber,
				F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(attrdef, AttrDefaultIndexId, true,
							  NULL, 2, keys);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result = atdform->oid;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}

/*
 * 给定 pg_attrdef OID，返回所属列的关系 OID 和列号
 * （为方便起见表示为 ObjectAddress）。
 *
 * 若没有这样的 pg_attrdef 条目则返回 InvalidObjectAddress。
 */
ObjectAddress
GetAttrDefaultColumnAddress(Oid attrdefoid)
{
	ObjectAddress result = InvalidObjectAddress;
	Relation	attrdef;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&skey[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefoid));
	scan = systable_beginscan(attrdef, AttrDefaultOidIndexId, true,
							  NULL, 1, skey);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result.classId = RelationRelationId;
		result.objectId = atdform->adrelid;
		result.objectSubId = atdform->adnum;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}
