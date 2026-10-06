/*-------------------------------------------------------------------------
 *
 * pg_publication.c
 *		发布 C API 操作
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_publication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_type.h"
#include "commands/publicationcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* 记录发布与已发布表之间的关联 */
typedef struct
{
	Oid			relid;			/* 已发布表的 OID */
	Oid			pubid;			/* 发布此表的发布的 OID
								 * 。 */
} published_rel;

static void publication_translate_columns(Relation targetrel, List *columns,
										  int *natts, AttrNumber **attrs);

/*
 * 检查关系是否可以加入给定发布，若不可以则抛出适当
 * 错误。
 */
static void
check_publication_add_relation(Relation targetrel)
{
	/* 必须是普通表或分区表 */
	if (RelationGetForm(targetrel)->relkind != RELKIND_RELATION &&
		RelationGetForm(targetrel)->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail_relkind_not_supported(RelationGetForm(targetrel)->relkind)));

	/* 不能是系统表 */
	if (IsCatalogRelation(targetrel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for system tables.")));

	/* UNLOGGED 与 TEMP 关系无法成为发布的一部分。 */
	if (targetrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for temporary tables.")));
	else if (targetrel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for unlogged tables.")));
}

/*
 * 检查模式是否可以加入给定发布，若不可以则抛出适当
 * 错误。
 */
static void
check_publication_add_schema(Oid schemaid)
{
	/* 不能是系统命名空间 */
	if (IsCatalogNamespace(schemaid) || IsToastNamespace(schemaid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add schema \"%s\" to publication",
						get_namespace_name(schemaid)),
				 errdetail("This operation is not supported for system schemas.")));

	/* 不能是临时命名空间 */
	if (isAnyTempNamespace(schemaid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add schema \"%s\" to publication",
						get_namespace_name(schemaid)),
				 errdetail("Temporary schemas cannot be replicated.")));
}

/*
 * 返回由 oid 与 Form_pg_class 条目表示的关系
 * 是否可发布。
 *
 * 做与上面 check_publication_add_relation() 相同的检查，但不需要
 * 打开关系，也不抛出错误。
 *
 * XXX  这也排除所有 relid < FirstNormalObjectId 的表，
 * 即 initdb 期间创建的所有表。这主要影响预安装的
 * information_schema。IsCatalogRelationOid() 只排除
 * relid < FirstUnpinnedObjectId 的表，使该测试相当多余，
 * 但我们其实应该去掉 FirstNormalObjectId 测试而不是
 * IsCatalogRelationOid。今天还不能这么做，因为我们不希望
 * information_schema 表被视为可发布；但该测试
 * 对此其实不够，因为 information_schema 可以被
 * 删除并重新加载，然后就会被视为可发布。最好的
 * 长期解决方案可能是给 pg_class 增加一个 "relispublishable" bool，
 * 并依赖它而不是 OID 检查。
 */
static bool
is_publishable_class(Oid relid, Form_pg_class reltuple)
{
	return (reltuple->relkind == RELKIND_RELATION ||
			reltuple->relkind == RELKIND_PARTITIONED_TABLE) &&
		!IsCatalogRelationOid(relid) &&
		reltuple->relpersistence == RELPERSISTENCE_PERMANENT &&
		relid >= FirstNormalObjectId;
}

/*
 * is_publishable_class() 的另一变体，接受 Relation。
 */
bool
is_publishable_relation(Relation rel)
{
	return is_publishable_class(RelationGetRelid(rel), rel->rd_rel);
}

/*
 * 上述函数的 SQL 可调用变体
 *
 * 当关系不存在时返回 null。这旨在用于例如
 * psql，以避免在有并发目录更改时出现
 * 无谓错误。
 */
Datum
pg_relation_is_publishable(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	result = is_publishable_class(relid, (Form_pg_class) GETSTRUCT(tuple));
	ReleaseSysCache(tuple);
	PG_RETURN_BOOL(result);
}

/*
 * 若祖先在已发布关系列表中则返回 true。
 * 否则返回 false。
 */
static bool
is_ancestor_member_tableinfos(Oid ancestor, List *table_infos)
{
	ListCell   *lc;

	foreach(lc, table_infos)
	{
		Oid			relid = ((published_rel *) lfirst(lc))->relid;

		if (relid == ancestor)
			return true;
	}

	return false;
}

/*
 * 过滤掉其父表也出现在列表中的分区。
 */
static void
filter_partitions(List *table_infos)
{
	ListCell   *lc;

	foreach(lc, table_infos)
	{
		bool		skip = false;
		List	   *ancestors = NIL;
		ListCell   *lc2;
		published_rel *table_info = (published_rel *) lfirst(lc);

		if (get_rel_relispartition(table_info->relid))
			ancestors = get_partition_ancestors(table_info->relid);

		foreach(lc2, ancestors)
		{
			Oid			ancestor = lfirst_oid(lc2);

			if (is_ancestor_member_tableinfos(ancestor, table_infos))
			{
				skip = true;
				break;
			}
		}

		if (skip)
			table_infos = foreach_delete_current(table_infos, lc);
	}
}

/*
 * 若有任何模式与该发布关联则返回 true，若没有模式与该
 * 发布关联则返回 false。
 */
bool
is_schema_publication(Oid pubid)
{
	Relation	pubschsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		result = false;

	pubschsrel = table_open(PublicationNamespaceRelationId, AccessShareLock);
	ScanKeyInit(&scankey,
				Anum_pg_publication_namespace_pnpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubschsrel,
							  PublicationNamespacePnnspidPnpubidIndexId,
							  true, NULL, 1, &scankey);
	tup = systable_getnext(scan);
	result = HeapTupleIsValid(tup);

	systable_endscan(scan);
	table_close(pubschsrel, AccessShareLock);

	return result;
}

/*
 * 根据指定关系的发布分区选项取得
 * 关系。
 */
List *
GetPubPartitionOptionRelations(List *result, PublicationPartOpt pub_partopt,
							   Oid relid)
{
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE &&
		pub_partopt != PUBLICATION_PART_ROOT)
	{
		List	   *all_parts = find_all_inheritors(relid, NoLock,
													NULL);

		if (pub_partopt == PUBLICATION_PART_ALL)
			result = list_concat(result, all_parts);
		else if (pub_partopt == PUBLICATION_PART_LEAF)
		{
			ListCell   *lc;

			foreach(lc, all_parts)
			{
				Oid			partOid = lfirst_oid(lc);

				if (get_rel_relkind(partOid) != RELKIND_PARTITIONED_TABLE)
					result = lappend_oid(result, partOid);
			}
		}
		else
			Assert(false);
	}
	else
		result = lappend_oid(result, relid);

	return result;
}

/*
 * 若有通过本发布发布的最顶层祖先，则返回其
 * relid，并将其祖先层级设为 ancestor_level，
 * 否则返回 InvalidOid。
 *
 * ancestor_level 值允许我们比较多个发布的结果，
 * 并决定哪个值更高。
 *
 * 注意祖先列表应按最顶层祖先位于列表末尾的
 * 顺序排列。
 */
Oid
GetTopMostAncestorInPublication(Oid puboid, List *ancestors, int *ancestor_level)
{
	ListCell   *lc;
	Oid			topmost_relid = InvalidOid;
	int			level = 0;

	/*
	 * 找到位于本发布中的"最顶层"祖先。
	 */
	foreach(lc, ancestors)
	{
		Oid			ancestor = lfirst_oid(lc);
		List	   *apubids = GetRelationPublications(ancestor);
		List	   *aschemaPubids = NIL;

		level++;

		if (list_member_oid(apubids, puboid))
		{
			topmost_relid = ancestor;

			if (ancestor_level)
				*ancestor_level = level;
		}
		else
		{
			aschemaPubids = GetSchemaPublications(get_rel_namespace(ancestor));
			if (list_member_oid(aschemaPubids, puboid))
			{
				topmost_relid = ancestor;

				if (ancestor_level)
					*ancestor_level = level;
			}
		}

		list_free(apubids);
		list_free(aschemaPubids);
	}

	return topmost_relid;
}

/*
 * 插入新的发布/关系映射。
 */
ObjectAddress
publication_add_relation(Oid pubid, PublicationRelInfo *pri,
						 bool if_not_exists)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_publication_rel];
	bool		nulls[Natts_pg_publication_rel];
	Relation	targetrel = pri->relation;
	Oid			relid = RelationGetRelid(targetrel);
	Oid			pubreloid;
	Publication *pub = GetPublication(pubid);
	AttrNumber *attarray = NULL;
	int			natts = 0;
	ObjectAddress myself,
				referenced;
	List	   *relids = NIL;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	/*
	 * 检查重复。注意这并不能真正防止
	 * 重复，它只是为常见情形提供更友好的错误消息。
	 * 真正的保护是目录上的唯一键。
	 */
	if (SearchSysCacheExists2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(pubid)))
	{
		table_close(rel, RowExclusiveLock);

		if (if_not_exists)
			return InvalidObjectAddress;

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("relation \"%s\" is already member of publication \"%s\"",
						RelationGetRelationName(targetrel), pub->name)));
	}

	check_publication_add_relation(targetrel);

	/*
	 * 把列名翻译为 attnum，并确保列列表
	 * 只包含允许的元素（无系统列或生成列等）。
	 * 同时构建 attnum 数组，以便存入目录。
	 */
	publication_translate_columns(pri->relation, pri->columns,
								  &natts, &attarray);

	/* 构造一个元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	pubreloid = GetNewOidWithIndex(rel, PublicationRelObjectIndexId,
								   Anum_pg_publication_rel_oid);
	values[Anum_pg_publication_rel_oid - 1] = ObjectIdGetDatum(pubreloid);
	values[Anum_pg_publication_rel_prpubid - 1] =
		ObjectIdGetDatum(pubid);
	values[Anum_pg_publication_rel_prrelid - 1] =
		ObjectIdGetDatum(relid);

	/* 若有则添加限定条件 */
	if (pri->whereClause != NULL)
		values[Anum_pg_publication_rel_prqual - 1] = CStringGetTextDatum(nodeToString(pri->whereClause));
	else
		nulls[Anum_pg_publication_rel_prqual - 1] = true;

	/* 若有则添加列列表 */
	if (pri->columns)
		values[Anum_pg_publication_rel_prattrs - 1] = PointerGetDatum(buildint2vector(attarray, natts));
	else
		nulls[Anum_pg_publication_rel_prattrs - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* 把元组插入目录。 */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	/* 按需注册依赖 */
	ObjectAddressSet(myself, PublicationRelRelationId, pubreloid);

	/* 添加对发布的依赖 */
	ObjectAddressSet(referenced, PublicationRelationId, pubid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 添加对关系的依赖 */
	ObjectAddressSet(referenced, RelationRelationId, relid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 添加对限定条件中提到的对象的依赖 */
	if (pri->whereClause)
		recordDependencyOnSingleRelExpr(&myself, pri->whereClause, relid,
										DEPENDENCY_NORMAL, DEPENDENCY_NORMAL,
										false);

	/* 若列出了列则添加对列的依赖 */
	for (int i = 0; i < natts; i++)
	{
		ObjectAddressSubSet(referenced, RelationRelationId, relid, attarray[i]);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* 关闭表。 */
	table_close(rel, RowExclusiveLock);

	/*
	 * 使 relcache 失效，以便重建发布信息。
	 *
	 * 对分区表，必须使各自分区层次中包含的所有分区
	 * 失效，而不仅仅是发布中显式提到的那一个。这是需要的，
	 * 因为当父表被发布时，我们隐式发布
	 * 子表。
	 */
	relids = GetPubPartitionOptionRelations(relids, PUBLICATION_PART_ALL,
											relid);

	InvalidatePublicationRels(relids);

	return myself;
}

/* attnum 的 qsort 比较器 */
static int
compare_int16(const void *a, const void *b)
{
	int			av = *(const int16 *) a;
	int			bv = *(const int16 *) b;

	/* 若 int 比 int16 宽则这不会溢出 */
	return (av - bv);
}

/*
 * 把列名列表翻译为属性编号数组
 * 以及含有它们的 Bitmapset；验证每个属性适合出现在
 * 发布列列表中（无系统或生成属性，
 * 无重复）。与副本标识相关的额外检查稍后进行；
 * 见 pub_collist_contains_invalid_column。
 *
 * 注意属性编号*不*由
 * FirstLowInvalidHeapAttributeNumber 偏移；系统列被禁止，因此这
 * 没问题。
 */
static void
publication_translate_columns(Relation targetrel, List *columns,
							  int *natts, AttrNumber **attrs)
{
	AttrNumber *attarray = NULL;
	Bitmapset  *set = NULL;
	ListCell   *lc;
	int			n = 0;
	TupleDesc	tupdesc = RelationGetDescr(targetrel);

	/* 未定义列列表时退出。 */
	if (!columns)
		return;

	/*
	 * 把列列表翻译为 attnum。我们禁止系统属性，并
	 * 确保没有重复列。
	 */
	attarray = palloc(sizeof(AttrNumber) * list_length(columns));
	foreach(lc, columns)
	{
		char	   *colname = strVal(lfirst(lc));
		AttrNumber	attnum = get_attnum(RelationGetRelid(targetrel), colname);

		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   colname, RelationGetRelationName(targetrel)));

		if (!AttrNumberIsForUserDefinedAttr(attnum))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					errmsg("cannot use system column \"%s\" in publication column list",
						   colname));

		if (TupleDescAttr(tupdesc, attnum - 1)->attgenerated)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					errmsg("cannot use generated column \"%s\" in publication column list",
						   colname));

		if (bms_is_member(attnum, set))
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_OBJECT),
					errmsg("duplicate column \"%s\" in publication column list",
						   colname));

		set = bms_add_member(set, attnum);
		attarray[n++] = attnum;
	}

	/* 保持整洁，使目录表示始终有序 */
	qsort(attarray, n, sizeof(AttrNumber), compare_int16);

	*natts = n;
	*attrs = attarray;

	bms_free(set);
}

/*
 * 把列列表（由数组 Datum 表示）转换为位图集。
 *
 * 若 columns 不为 NULL，则把列号加入该集合。
 *
 * 若 mcxt 不为 NULL，则在该上下文中构建位图集。
 */
Bitmapset *
pub_collist_to_bitmapset(Bitmapset *columns, Datum pubcols, MemoryContext mcxt)
{
	Bitmapset  *result = NULL;
	ArrayType  *arr;
	int			nelems;
	int16	   *elems;
	MemoryContext oldcxt = NULL;

	/*
	 * 若提供了已有位图则使用它。否则只用 NULL 并
	 * 构建新位图。
	 */
	if (columns)
		result = columns;

	arr = DatumGetArrayTypeP(pubcols);
	nelems = ARR_DIMS(arr)[0];
	elems = (int16 *) ARR_DATA_PTR(arr);

	/* 若指定了内存上下文，则切换到它。 */
	if (mcxt)
		oldcxt = MemoryContextSwitchTo(mcxt);

	for (int i = 0; i < nelems; i++)
		result = bms_add_member(result, elems[i]);

	if (mcxt)
		MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * 插入新的发布/模式映射。
 */
ObjectAddress
publication_add_schema(Oid pubid, Oid schemaid, bool if_not_exists)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_publication_namespace];
	bool		nulls[Natts_pg_publication_namespace];
	Oid			psschid;
	Publication *pub = GetPublication(pubid);
	List	   *schemaRels = NIL;
	ObjectAddress myself,
				referenced;

	rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);

	/*
	 * 检查重复。注意这并不能真正防止
	 * 重复，它只是为常见情形提供更友好的错误消息。
	 * 真正的保护是目录上的唯一键。
	 */
	if (SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
							  ObjectIdGetDatum(schemaid),
							  ObjectIdGetDatum(pubid)))
	{
		table_close(rel, RowExclusiveLock);

		if (if_not_exists)
			return InvalidObjectAddress;

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("schema \"%s\" is already member of publication \"%s\"",
						get_namespace_name(schemaid), pub->name)));
	}

	check_publication_add_schema(schemaid);

	/* 构造一个元组 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	psschid = GetNewOidWithIndex(rel, PublicationNamespaceObjectIndexId,
								 Anum_pg_publication_namespace_oid);
	values[Anum_pg_publication_namespace_oid - 1] = ObjectIdGetDatum(psschid);
	values[Anum_pg_publication_namespace_pnpubid - 1] =
		ObjectIdGetDatum(pubid);
	values[Anum_pg_publication_namespace_pnnspid - 1] =
		ObjectIdGetDatum(schemaid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* 把元组插入目录 */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, PublicationNamespaceRelationId, psschid);

	/* 添加对发布的依赖 */
	ObjectAddressSet(referenced, PublicationRelationId, pubid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 添加对模式的依赖 */
	ObjectAddressSet(referenced, NamespaceRelationId, schemaid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 关闭表 */
	table_close(rel, RowExclusiveLock);

	/*
	 * 使 relcache 失效，以便重建发布信息。见
	 * publication_add_relation 了解为何需要考虑所有
	 * 分区。
	 */
	schemaRels = GetSchemaPublicationRelations(schemaid,
											   PUBLICATION_PART_ALL);
	InvalidatePublicationRels(schemaRels);

	return myself;
}

/* 取得关系的发布 oid 列表 */
List *
GetRelationPublications(Oid relid)
{
	List	   *result = NIL;
	CatCList   *pubrellist;
	int			i;

	/* 查找与该关系关联的所有发布。 */
	pubrellist = SearchSysCacheList1(PUBLICATIONRELMAP,
									 ObjectIdGetDatum(relid));
	for (i = 0; i < pubrellist->n_members; i++)
	{
		HeapTuple	tup = &pubrellist->members[i]->tuple;
		Oid			pubid = ((Form_pg_publication_rel) GETSTRUCT(tup))->prpubid;

		result = lappend_oid(result, pubid);
	}

	ReleaseSysCacheList(pubrellist);

	return result;
}

/*
 * 取得发布的关系 oid 列表。
 *
 * 这只应用于 FOR TABLE 发布，FOR ALL TABLES
 * 应使用 GetAllTablesPublicationRelations()。
 */
List *
GetPublicationRelations(Oid pubid, PublicationPartOpt pub_partopt)
{
	List	   *result;
	Relation	pubrelsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* 查找与该关系关联的所有发布。 */
	pubrelsrel = table_open(PublicationRelRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_rel_prpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubrelsrel, PublicationRelPrpubidIndexId,
							  true, NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_rel pubrel;

		pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);
		result = GetPubPartitionOptionRelations(result, pub_partopt,
												pubrel->prrelid);
	}

	systable_endscan(scan);
	table_close(pubrelsrel, AccessShareLock);

	/* 现在对结果列表排序并去重 */
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	return result;
}

/*
 * 取得标记为 FOR ALL TABLES 的发布的发布 oid 列表。
 */
List *
GetAllTablesPublications(void)
{
	List	   *result;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* 查找所有标记为 for all tables 的发布。 */
	rel = table_open(PublicationRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_puballtables,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Oid			oid = ((Form_pg_publication) GETSTRUCT(tup))->oid;

		result = lappend_oid(result, oid);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * 取得由 FOR ALL TABLES publication(s) 发布的所有关系的列表。
 *
 * 若发布通过各自的根分区表发布分区更改，
 * 我们必须排除分区而改为包含
 * 根分区表。
 */
List *
GetAllTablesPublicationRelations(bool pubviaroot)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_RELATION));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = relForm->oid;

		if (is_publishable_class(relid, relForm) &&
			!(relForm->relispartition && pubviaroot))
			result = lappend_oid(result, relid);
	}

	table_endscan(scan);

	if (pubviaroot)
	{
		ScanKeyInit(&key[0],
					Anum_pg_class_relkind,
					BTEqualStrategyNumber, F_CHAREQ,
					CharGetDatum(RELKIND_PARTITIONED_TABLE));

		scan = table_beginscan_catalog(classRel, 1, key);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
			Oid			relid = relForm->oid;

			if (is_publishable_class(relid, relForm) &&
				!relForm->relispartition)
				result = lappend_oid(result, relid);
		}

		table_endscan(scan);
	}

	table_close(classRel, AccessShareLock);
	return result;
}

/*
 * 取得发布的模式 oid 列表。
 *
 * 这只应用于 FOR TABLES IN SCHEMA 发布。
 */
List *
GetPublicationSchemas(Oid pubid)
{
	List	   *result = NIL;
	Relation	pubschsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* 查找与该发布关联的所有模式 */
	pubschsrel = table_open(PublicationNamespaceRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_namespace_pnpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubschsrel,
							  PublicationNamespacePnnspidPnpubidIndexId,
							  true, NULL, 1, &scankey);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_namespace pubsch;

		pubsch = (Form_pg_publication_namespace) GETSTRUCT(tup);

		result = lappend_oid(result, pubsch->pnnspid);
	}

	systable_endscan(scan);
	table_close(pubschsrel, AccessShareLock);

	return result;
}

/*
 * 取得与指定模式关联的发布 oid 列表。
 */
List *
GetSchemaPublications(Oid schemaid)
{
	List	   *result = NIL;
	CatCList   *pubschlist;
	int			i;

	/* 查找与该模式关联的所有发布 */
	pubschlist = SearchSysCacheList1(PUBLICATIONNAMESPACEMAP,
									 ObjectIdGetDatum(schemaid));
	for (i = 0; i < pubschlist->n_members; i++)
	{
		HeapTuple	tup = &pubschlist->members[i]->tuple;
		Oid			pubid = ((Form_pg_publication_namespace) GETSTRUCT(tup))->pnpubid;

		result = lappend_oid(result, pubid);
	}

	ReleaseSysCacheList(pubschlist);

	return result;
}

/*
 * 取得指定模式的可发布关系 oid 列表。
 */
List *
GetSchemaPublicationRelations(Oid schemaid, PublicationPartOpt pub_partopt)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	Assert(OidIsValid(schemaid));

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_relnamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				schemaid);

	/* 取得指定模式中存在的所有关系 */
	scan = table_beginscan_catalog(classRel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = relForm->oid;
		char		relkind;

		if (!is_publishable_class(relid, relForm))
			continue;

		relkind = get_rel_relkind(relid);
		if (relkind == RELKIND_RELATION)
			result = lappend_oid(result, relid);
		else if (relkind == RELKIND_PARTITIONED_TABLE)
		{
			List	   *partitionrels = NIL;

			/*
			 * 某些分区很可能位于与父表不同的
			 * 模式中，因此我们需要单独取得此类
			 * 分区。
			 */
			partitionrels = GetPubPartitionOptionRelations(partitionrels,
														   pub_partopt,
														   relForm->oid);
			result = list_concat_unique_oid(result, partitionrels);
		}
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);
	return result;
}

/*
 * 取得由 FOR TABLES IN SCHEMA 发布所发布的所有关系的
 * 列表。
 */
List *
GetAllSchemaPublicationRelations(Oid pubid, PublicationPartOpt pub_partopt)
{
	List	   *result = NIL;
	List	   *pubschemalist = GetPublicationSchemas(pubid);
	ListCell   *cell;

	foreach(cell, pubschemalist)
	{
		Oid			schemaid = lfirst_oid(cell);
		List	   *schemaRels = NIL;

		schemaRels = GetSchemaPublicationRelations(schemaid, pub_partopt);
		result = list_concat(result, schemaRels);
	}

	return result;
}

/*
 * 用 oid 取得发布
 *
 * Publication 结构及其数据在此 palloc。
 */
Publication *
GetPublication(Oid pubid)
{
	HeapTuple	tup;
	Publication *pub;
	Form_pg_publication pubform;

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	pub = (Publication *) palloc(sizeof(Publication));
	pub->oid = pubid;
	pub->name = pstrdup(NameStr(pubform->pubname));
	pub->alltables = pubform->puballtables;
	pub->pubactions.pubinsert = pubform->pubinsert;
	pub->pubactions.pubupdate = pubform->pubupdate;
	pub->pubactions.pubdelete = pubform->pubdelete;
	pub->pubactions.pubtruncate = pubform->pubtruncate;
	pub->pubviaroot = pubform->pubviaroot;

	ReleaseSysCache(tup);

	return pub;
}

/*
 * 用名称取得发布。
 */
Publication *
GetPublicationByName(const char *pubname, bool missing_ok)
{
	Oid			oid;

	oid = get_publication_oid(pubname, missing_ok);

	return OidIsValid(oid) ? GetPublication(oid) : NULL;
}

/*
 * 取得给定发布数组中各表的信息。
 *
 * 对每个表返回 pubid、relid、列列表、行过滤器。
 */
Datum
pg_get_publication_tables(PG_FUNCTION_ARGS)
{
#define NUM_PUBLICATION_TABLES_ELEM	4
	FuncCallContext *funcctx;
	List	   *table_infos = NIL;

	/* 仅在函数第一次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		ArrayType  *arr;
		Datum	   *elems;
		int			nelems,
					i;
		bool		viaroot = false;

		/* 创建跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/* 切换到适合多次函数调用的内存上下文 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 把参数解构为元素，每个元素是一个
		 * 发布名。
		 */
		arr = PG_GETARG_ARRAYTYPE_P(0);
		deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
						  &elems, NULL, &nelems);

		/* 从每个发布取得表的 Oid。 */
		for (i = 0; i < nelems; i++)
		{
			Publication *pub_elem;
			List	   *pub_elem_tables = NIL;
			ListCell   *lc;

			pub_elem = GetPublicationByName(TextDatumGetCString(elems[i]), false);

			/*
			 * 发布支持分区表。若
			 * publish_via_partition_root 为 false，所有更改都使用叶分区的
			 * 标识与模式复制，因此我们只需要那些。否则，取得
			 * 分区表本身。
			 */
			if (pub_elem->alltables)
				pub_elem_tables = GetAllTablesPublicationRelations(pub_elem->pubviaroot);
			else
			{
				List	   *relids,
						   *schemarelids;

				relids = GetPublicationRelations(pub_elem->oid,
												 pub_elem->pubviaroot ?
												 PUBLICATION_PART_ROOT :
												 PUBLICATION_PART_LEAF);
				schemarelids = GetAllSchemaPublicationRelations(pub_elem->oid,
																pub_elem->pubviaroot ?
																PUBLICATION_PART_ROOT :
																PUBLICATION_PART_LEAF);
				pub_elem_tables = list_concat_unique_oid(relids, schemarelids);
			}

			/*
			 * 记录已发布表与相应发布，以便我们稍后能取得
			 * 行过滤器与列列表。
			 *
			 * 当一个表被多个发布发布时，为取得所有行过滤器与
			 * 列列表，与该表相关的结构会被记录
			 * 多次。
			 */
			foreach(lc, pub_elem_tables)
			{
				published_rel *table_info = (published_rel *) palloc(sizeof(published_rel));

				table_info->relid = lfirst_oid(lc);
				table_info->pubid = pub_elem->oid;
				table_infos = lappend(table_infos, table_info);
			}

			/* 至少一个发布在使用 publish_via_partition_root。 */
			if (pub_elem->pubviaroot)
				viaroot = true;
		}

		/*
		 * 若发布通过各自的根分区表发布分区更改，
		 * 我们必须排除分区而改为包含
		 * 根分区表。否则，函数可能同时返回子表与
		 * 父表，这可能导致子表的数据在订阅端被
		 * 双重
		 * 发布。
		 */
		if (viaroot)
			filter_partitions(table_infos);

		/* 为结果行构造元组描述符。 */
		tupdesc = CreateTemplateTupleDesc(NUM_PUBLICATION_TABLES_ELEM);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "pubid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "relid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "attrs",
						   INT2VECTOROID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "qual",
						   PG_NODE_TREEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = (void *) table_infos;

		MemoryContextSwitchTo(oldcontext);
	}

	/* 函数每次调用都做的事 */
	funcctx = SRF_PERCALL_SETUP();
	table_infos = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(table_infos))
	{
		HeapTuple	pubtuple = NULL;
		HeapTuple	rettuple;
		Publication *pub;
		published_rel *table_info = (published_rel *) list_nth(table_infos, funcctx->call_cntr);
		Oid			relid = table_info->relid;
		Oid			schemaid = get_rel_namespace(relid);
		Datum		values[NUM_PUBLICATION_TABLES_ELEM] = {0};
		bool		nulls[NUM_PUBLICATION_TABLES_ELEM] = {0};

		/*
		 * 用适当数据构造元组。
		 */

		pub = GetPublication(table_info->pubid);

		values[0] = ObjectIdGetDatum(pub->oid);
		values[1] = ObjectIdGetDatum(relid);

		/*
		 * 对 FOR ALL TABLES 或 FOR TABLES IN SCHEMA 发布，我们不考虑
		 * 行过滤器或列列表。
		 */
		if (!pub->alltables &&
			!SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								   ObjectIdGetDatum(schemaid),
								   ObjectIdGetDatum(pub->oid)))
			pubtuple = SearchSysCacheCopy2(PUBLICATIONRELMAP,
										   ObjectIdGetDatum(relid),
										   ObjectIdGetDatum(pub->oid));

		if (HeapTupleIsValid(pubtuple))
		{
			/* 查找列列表属性。 */
			values[2] = SysCacheGetAttr(PUBLICATIONRELMAP, pubtuple,
										Anum_pg_publication_rel_prattrs,
										&(nulls[2]));

			/* Null 表示无过滤器。 */
			values[3] = SysCacheGetAttr(PUBLICATIONRELMAP, pubtuple,
										Anum_pg_publication_rel_prqual,
										&(nulls[3]));
		}
		else
		{
			nulls[2] = true;
			nulls[3] = true;
		}

		/* 未指定列列表时显示所有列。 */
		if (nulls[2])
		{
			Relation	rel = table_open(relid, AccessShareLock);
			int			nattnums = 0;
			int16	   *attnums;
			TupleDesc	desc = RelationGetDescr(rel);
			int			i;

			attnums = (int16 *) palloc(desc->natts * sizeof(int16));

			for (i = 0; i < desc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(desc, i);

				if (att->attisdropped || att->attgenerated)
					continue;

				attnums[nattnums++] = att->attnum;
			}

			if (nattnums > 0)
			{
				values[2] = PointerGetDatum(buildint2vector(attnums, nattnums));
				nulls[2] = false;
			}

			table_close(rel, AccessShareLock);
		}

		rettuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(rettuple));
	}

	SRF_RETURN_DONE(funcctx);
}
