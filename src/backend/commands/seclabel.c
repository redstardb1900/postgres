/* -------------------------------------------------------------------------
 *
 * seclabel.c
 *	  支持安全标签功能的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shseclabel.h"
#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef struct
{
	const char *provider_name;
	check_object_relabel_type hook;
} LabelProvider;

static List *label_provider_list = NIL;

static bool
SecLabelSupportsObjectType(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_AGGREGATE:
		case OBJECT_COLUMN:
		case OBJECT_DATABASE:
		case OBJECT_DOMAIN:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_FUNCTION:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_MATVIEW:
		case OBJECT_PROCEDURE:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_ROUTINE:
		case OBJECT_SCHEMA:
		case OBJECT_SEQUENCE:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLE:
		case OBJECT_TABLESPACE:
		case OBJECT_TYPE:
		case OBJECT_VIEW:
			return true;

		case OBJECT_ACCESS_METHOD:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EXTENSION:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_INDEX:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
			return false;

			/*
			 * 这里故意没有 default: 分支；我们希望新的 ObjectType
			 * 若未在上面处理，编译器会发出警告。
			 */
	}

	/* 不应走到这里，但若走到了，就说"不支持" */
	return false;
}

/*
 * ExecSecLabelStmt --
 *
 * 把安全标签应用到数据库对象上。
 *
 * 返回应用了该策略的对象的 ObjectAddress。
 */
ObjectAddress
ExecSecLabelStmt(SecLabelStmt *stmt)
{
	LabelProvider *provider = NULL;
	ObjectAddress address;
	Relation	relation;
	ListCell   *lc;

	/*
	 * 找到指定名称的标签提供者；若未指定，
	 * 检查是否恰好只有一个，若是就用它。
	 */
	if (stmt->provider == NULL)
	{
		if (label_provider_list == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no security label providers have been loaded")));
		if (list_length(label_provider_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("must specify provider when multiple security label providers have been loaded")));
		provider = (LabelProvider *) linitial(label_provider_list);
	}
	else
	{
		foreach(lc, label_provider_list)
		{
			LabelProvider *lp = lfirst(lc);

			if (strcmp(stmt->provider, lp->provider_name) == 0)
			{
				provider = lp;
				break;
			}
		}
		if (provider == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("security label provider \"%s\" is not loaded",
							stmt->provider)));
	}

	if (!SecLabelSupportsObjectType(stmt->objtype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("security labels are not supported for this type of object")));

	/*
	 * 把标识该对象的解析器表示转换为 ObjectAddress。
	 * get_object_address() 会在对象不存在时抛出错误，
	 * 还会在目标上获取锁，以防
	 * 并发修改。
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
			 * 只允许对表、视图、物化视图、复合类型和外部表的
			 * 列应用安全标签（只有这些 relkind 会被
			 * pg_dump 转储标签）。
			 */
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_VIEW &&
				relation->rd_rel->relkind != RELKIND_MATVIEW &&
				relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
				relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot set security label on relation \"%s\"",
								RelationGetRelationName(relation)),
						 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
			break;
		default:
			break;
	}

	/* 提供者在这里获得控制权，可抛 ERROR 否决新标签。 */
	provider->hook(&address, stmt->label);

	/* 应用新标签。 */
	SetSecurityLabel(&address, provider->provider_name, stmt->label);

	/*
	 * 若 get_object_address() 替我们打开了关系，就关闭它，
	 * 以保持引用计数正确——但我们保留 get_object_address()
	 * 获取的任何锁直到提交时刻，
	 * 以防并发活动。
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return address;
}

/*
 * GetSharedSecurityLabel 返回给定提供者下共享对象的
 * 安全标签，若没有这样的标签则返回 NULL。
 */
static char *
GetSharedSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = table_open(SharedSecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId,
							  criticalSharedRelcachesBuilt, NULL, 3, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_shseclabel_label,
							 RelationGetDescr(pg_shseclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	table_close(pg_shseclabel, AccessShareLock);

	return seclabel;
}

/*
 * GetSecurityLabel 返回给定提供者下共享或数据库对象的
 * 安全标签，若没有这样的标签则返回 NULL。
 */
char *
GetSecurityLabel(const ObjectAddress *object, const char *provider)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	char	   *seclabel = NULL;

	/* 共享对象有自己的安全标签目录。 */
	if (IsSharedRelation(object->classId))
		return GetSharedSecurityLabel(object, provider);

	/* 一定是非共享对象，所以检查 pg_seclabel。 */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = table_open(SecLabelRelationId, AccessShareLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, 4, keys);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_seclabel_label,
							 RelationGetDescr(pg_seclabel), &isnull);
		if (!isnull)
			seclabel = TextDatumGetCString(datum);
	}
	systable_endscan(scan);

	table_close(pg_seclabel, AccessShareLock);

	return seclabel;
}

/*
 * SetSharedSecurityLabel 是 SetSecurityLabel 的辅助函数，
 * 用于处理共享数据库对象。
 */
static void
SetSharedSecurityLabel(const ObjectAddress *object,
					   const char *provider, const char *label)
{
	Relation	pg_shseclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_shseclabel];
	bool		nulls[Natts_pg_shseclabel];
	bool		replaces[Natts_pg_shseclabel];

	/* 如有必要，准备构造或更新元组。 */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_shseclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_shseclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_shseclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_shseclabel_label - 1] = CStringGetTextDatum(label);

	/* 使用索引搜索匹配的旧元组 */
	ScanKeyInit(&keys[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_shseclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_shseclabel = table_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  NULL, 3, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			CatalogTupleDelete(pg_shseclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_shseclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_shseclabel),
									   values, nulls, replaces);
			CatalogTupleUpdate(pg_shseclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* 若未找到旧元组，就插入新元组 */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_shseclabel),
								 values, nulls);
		CatalogTupleInsert(pg_shseclabel, newtup);
	}

	if (newtup != NULL)
		heap_freetuple(newtup);

	table_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * SetSecurityLabel 尝试把指定对象上指定提供者的
 * 安全标签设为给定值。NULL 表示应删除
 * 任何现有标签。
 */
void
SetSecurityLabel(const ObjectAddress *object,
				 const char *provider, const char *label)
{
	Relation	pg_seclabel;
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	oldtup;
	HeapTuple	newtup = NULL;
	Datum		values[Natts_pg_seclabel];
	bool		nulls[Natts_pg_seclabel];
	bool		replaces[Natts_pg_seclabel];

	/* 共享对象有自己的安全标签目录。 */
	if (IsSharedRelation(object->classId))
	{
		SetSharedSecurityLabel(object, provider, label);
		return;
	}

	/* 如有必要，准备构造或更新元组。 */
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));
	values[Anum_pg_seclabel_objoid - 1] = ObjectIdGetDatum(object->objectId);
	values[Anum_pg_seclabel_classoid - 1] = ObjectIdGetDatum(object->classId);
	values[Anum_pg_seclabel_objsubid - 1] = Int32GetDatum(object->objectSubId);
	values[Anum_pg_seclabel_provider - 1] = CStringGetTextDatum(provider);
	if (label != NULL)
		values[Anum_pg_seclabel_label - 1] = CStringGetTextDatum(label);

	/* 使用索引搜索匹配的旧元组 */
	ScanKeyInit(&keys[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&keys[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&keys[2],
				Anum_pg_seclabel_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));
	ScanKeyInit(&keys[3],
				Anum_pg_seclabel_provider,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(provider));

	pg_seclabel = table_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, 4, keys);

	oldtup = systable_getnext(scan);
	if (HeapTupleIsValid(oldtup))
	{
		if (label == NULL)
			CatalogTupleDelete(pg_seclabel, &oldtup->t_self);
		else
		{
			replaces[Anum_pg_seclabel_label - 1] = true;
			newtup = heap_modify_tuple(oldtup, RelationGetDescr(pg_seclabel),
									   values, nulls, replaces);
			CatalogTupleUpdate(pg_seclabel, &oldtup->t_self, newtup);
		}
	}
	systable_endscan(scan);

	/* 若未找到旧元组，就插入新元组 */
	if (newtup == NULL && label != NULL)
	{
		newtup = heap_form_tuple(RelationGetDescr(pg_seclabel),
								 values, nulls);
		CatalogTupleInsert(pg_seclabel, newtup);
	}

	/* 如有必要，更新索引 */
	if (newtup != NULL)
		heap_freetuple(newtup);

	table_close(pg_seclabel, RowExclusiveLock);
}

/*
 * DeleteSharedSecurityLabel 是 DeleteSecurityLabel 的辅助函数，
 * 用于处理共享数据库对象。
 */
void
DeleteSharedSecurityLabel(Oid objectId, Oid classId)
{
	Relation	pg_shseclabel;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	oldtup;

	ScanKeyInit(&skey[0],
				Anum_pg_shseclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_shseclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));

	pg_shseclabel = table_open(SharedSecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_shseclabel, SharedSecLabelObjectIndexId, true,
							  NULL, 2, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		CatalogTupleDelete(pg_shseclabel, &oldtup->t_self);
	systable_endscan(scan);

	table_close(pg_shseclabel, RowExclusiveLock);
}

/*
 * DeleteSecurityLabel 删除一个对象（以及任何子对象，
 * 若适用）的所有安全标签。
 */
void
DeleteSecurityLabel(const ObjectAddress *object)
{
	Relation	pg_seclabel;
	ScanKeyData skey[3];
	SysScanDesc scan;
	HeapTuple	oldtup;
	int			nkeys;

	/* 共享对象有自己的安全标签目录。 */
	if (IsSharedRelation(object->classId))
	{
		Assert(object->objectSubId == 0);
		DeleteSharedSecurityLabel(object->objectId, object->classId);
		return;
	}

	ScanKeyInit(&skey[0],
				Anum_pg_seclabel_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&skey[1],
				Anum_pg_seclabel_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&skey[2],
					Anum_pg_seclabel_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	pg_seclabel = table_open(SecLabelRelationId, RowExclusiveLock);

	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true,
							  NULL, nkeys, skey);
	while (HeapTupleIsValid(oldtup = systable_getnext(scan)))
		CatalogTupleDelete(pg_seclabel, &oldtup->t_self);
	systable_endscan(scan);

	table_close(pg_seclabel, RowExclusiveLock);
}

void
register_label_provider(const char *provider_name, check_object_relabel_type hook)
{
	LabelProvider *provider;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	provider = palloc(sizeof(LabelProvider));
	provider->provider_name = pstrdup(provider_name);
	provider->hook = hook;
	label_provider_list = lappend(label_provider_list, provider);
	MemoryContextSwitchTo(oldcxt);
}
