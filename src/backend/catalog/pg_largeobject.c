/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  支持操作 pg_largeobject 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_largeobject.c
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
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"


/*
 * 创建具有给定 LO 标识符的大对象。
 *
 * 我们通过向 pg_largeobject_metadata 插入一条
 * 不含任何数据页的条目来创建新大对象，从而使该对象
 * 看起来存在且大小为 0。
 */
Oid
LargeObjectCreate(Oid loid)
{
	Relation	pg_lo_meta;
	HeapTuple	ntup;
	Oid			loid_new;
	Datum		values[Natts_pg_largeobject_metadata];
	bool		nulls[Natts_pg_largeobject_metadata];

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							RowExclusiveLock);

	/*
	 * 插入大对象的元数据
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	if (OidIsValid(loid))
		loid_new = loid;
	else
		loid_new = GetNewOidWithIndex(pg_lo_meta,
									  LargeObjectMetadataOidIndexId,
									  Anum_pg_largeobject_metadata_oid);

	values[Anum_pg_largeobject_metadata_oid - 1] = ObjectIdGetDatum(loid_new);
	values[Anum_pg_largeobject_metadata_lomowner - 1]
		= ObjectIdGetDatum(GetUserId());
	nulls[Anum_pg_largeobject_metadata_lomacl - 1] = true;

	ntup = heap_form_tuple(RelationGetDescr(pg_lo_meta),
						   values, nulls);

	CatalogTupleInsert(pg_lo_meta, ntup);

	heap_freetuple(ntup);

	table_close(pg_lo_meta, RowExclusiveLock);

	return loid_new;
}

/*
 * 删除具有给定 LO 标识符的大对象。数据页与
 * 元数据都必须删除。
 */
void
LargeObjectDrop(Oid loid)
{
	Relation	pg_lo_meta;
	Relation	pg_largeobject;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tuple;

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							RowExclusiveLock);

	pg_largeobject = table_open(LargeObjectRelationId,
								RowExclusiveLock);

	/*
	 * 从 pg_largeobject_metadata 删除一条条目
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  NULL, 1, skey);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));

	CatalogTupleDelete(pg_lo_meta, &tuple->t_self);

	systable_endscan(scan);

	/*
	 * 从 pg_largeobject 删除所有关联条目
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	scan = systable_beginscan(pg_largeobject,
							  LargeObjectLOidPNIndexId, true,
							  NULL, 1, skey);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		CatalogTupleDelete(pg_largeobject, &tuple->t_self);
	}

	systable_endscan(scan);

	table_close(pg_largeobject, RowExclusiveLock);

	table_close(pg_lo_meta, RowExclusiveLock);
}

/*
 * LargeObjectExists
 *
 * 我们不对大对象元数据使用系统缓存，以免
 * 占用过多本地内存。
 *
 * 本函数始终使用最新快照扫描系统目录，
 * 因此不应在以只读模式打开大对象时使用
 * （因为以只读模式打开的大对象应按调用者的
 * 快照查看，而以读写模式打开时则相对于
 * 当前快照）。
 */
bool
LargeObjectExists(Oid loid)
{
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;
	bool		retval = false;

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	sd = systable_beginscan(pg_lo_meta,
							LargeObjectMetadataOidIndexId, true,
							NULL, 1, skey);

	tuple = systable_getnext(sd);
	if (HeapTupleIsValid(tuple))
		retval = true;

	systable_endscan(sd);

	table_close(pg_lo_meta, AccessShareLock);

	return retval;
}
