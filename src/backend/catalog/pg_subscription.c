/*-------------------------------------------------------------------------
 *
 * pg_subscription.c
 *		复制订阅
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_subscription.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static List *textarray_to_stringlist(ArrayType *textarray);

/*
 * 从系统缓存获取订阅。
 */
Subscription *
GetSubscription(Oid subid, bool missing_ok)
{
	HeapTuple	tup;
	Subscription *sub;
	Form_pg_subscription subform;
	Datum		datum;
	bool		isnull;

	tup = SearchSysCache1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
			return NULL;

		elog(ERROR, "cache lookup failed for subscription %u", subid);
	}

	subform = (Form_pg_subscription) GETSTRUCT(tup);

	sub = (Subscription *) palloc(sizeof(Subscription));
	sub->oid = subid;
	sub->dbid = subform->subdbid;
	sub->skiplsn = subform->subskiplsn;
	sub->name = pstrdup(NameStr(subform->subname));
	sub->owner = subform->subowner;
	sub->enabled = subform->subenabled;
	sub->binary = subform->subbinary;
	sub->stream = subform->substream;
	sub->twophasestate = subform->subtwophasestate;
	sub->disableonerr = subform->subdisableonerr;
	sub->passwordrequired = subform->subpasswordrequired;
	sub->runasowner = subform->subrunasowner;

	/* 获取 conninfo */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subconninfo);
	sub->conninfo = TextDatumGetCString(datum);

	/* 获取 slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID,
							tup,
							Anum_pg_subscription_subslotname,
							&isnull);
	if (!isnull)
		sub->slotname = pstrdup(NameStr(*DatumGetName(datum)));
	else
		sub->slotname = NULL;

	/* 获取 synccommit */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subsynccommit);
	sub->synccommit = TextDatumGetCString(datum);

	/* 获取 publications */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subpublications);
	sub->publications = textarray_to_stringlist(DatumGetArrayTypeP(datum));

	/* 获取 origin */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_suborigin);
	sub->origin = TextDatumGetCString(datum);

	ReleaseSysCache(tup);

	return sub;
}

/*
 * 返回给定数据库中已定义的订阅数量。
 * 由 dropdb() 用来检查数据库是否确实可以删除。
 */
int
CountDBSubscriptions(Oid dbid)
{
	int			nsubs = 0;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_pg_subscription_subdbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		nsubs++;

	systable_endscan(scan);

	table_close(rel, NoLock);

	return nsubs;
}

/*
 * 释放订阅结构分配的内存。
 */
void
FreeSubscription(Subscription *sub)
{
	pfree(sub->name);
	pfree(sub->conninfo);
	if (sub->slotname)
		pfree(sub->slotname);
	list_free_deep(sub->publications);
	pfree(sub);
}

/*
 * 禁用给定订阅。
 */
void
DisableSubscription(Oid subid)
{
	Relation	rel;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	HeapTuple	tup;

	/* 在目录中查找该订阅 */
	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for subscription %u", subid);

	LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);

	/* 构造新元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	/* 将订阅设为禁用。 */
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(false);
	replaces[Anum_pg_subscription_subenabled - 1] = true;

	/* 更新目录 */
	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);
	heap_freetuple(tup);

	table_close(rel, NoLock);
}

/*
 * 将文本数组转换为字符串列表。
 *
 * 注意：结果字符串列表在此 palloc。
 */
static List *
textarray_to_stringlist(ArrayType *textarray)
{
	Datum	   *elems;
	int			nelems,
				i;
	List	   *res = NIL;

	deconstruct_array_builtin(textarray, TEXTOID, &elems, NULL, &nelems);

	if (nelems == 0)
		return NIL;

	for (i = 0; i < nelems; i++)
		res = lappend(res, makeString(TextDatumGetCString(elems[i])));

	return res;
}

/*
 * 为订阅表添加新的状态记录。
 */
void
AddSubscriptionRelState(Oid subid, Oid relid, char state,
						XLogRecPtr sublsn)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription_rel];
	Datum		values[Natts_pg_subscription_rel];

	LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);

	rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

	/* 尝试查找已有映射。 */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONRELMAP,
							  ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(subid));
	if (HeapTupleIsValid(tup))
		elog(ERROR, "subscription table %u in subscription %u already exists",
			 relid, subid);

	/* 构造元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_subscription_rel_srsubid - 1] = ObjectIdGetDatum(subid);
	values[Anum_pg_subscription_rel_srrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_subscription_rel_srsubstate - 1] = CharGetDatum(state);
	if (sublsn != InvalidXLogRecPtr)
		values[Anum_pg_subscription_rel_srsublsn - 1] = LSNGetDatum(sublsn);
	else
		nulls[Anum_pg_subscription_rel_srsublsn - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* 将元组插入目录。 */
	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/* 清理。 */
	table_close(rel, NoLock);
}

/*
 * 更新订阅表的状态。
 */
void
UpdateSubscriptionRelStateEx(Oid subid, Oid relid, char state,
							 XLogRecPtr sublsn, bool already_locked)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription_rel];
	Datum		values[Natts_pg_subscription_rel];
	bool		replaces[Natts_pg_subscription_rel];

	if (already_locked)
	{
#ifdef USE_ASSERT_CHECKING
		LOCKTAG		tag;

		Assert(CheckRelationOidLockedByMe(SubscriptionRelRelationId,
										  RowExclusiveLock, true));
		SET_LOCKTAG_OBJECT(tag, InvalidOid, SubscriptionRelationId, subid, 0);
		Assert(LockHeldByMe(&tag, AccessShareLock));
#endif

		rel = table_open(SubscriptionRelRelationId, NoLock);
	}
	else
	{
		LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);
		rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);
	}

	/* 尝试查找已有映射。 */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONRELMAP,
							  ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(subid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "subscription table %u in subscription %u does not exist",
			 relid, subid);

	/* 更新元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	replaces[Anum_pg_subscription_rel_srsubstate - 1] = true;
	values[Anum_pg_subscription_rel_srsubstate - 1] = CharGetDatum(state);

	replaces[Anum_pg_subscription_rel_srsublsn - 1] = true;
	if (sublsn != InvalidXLogRecPtr)
		values[Anum_pg_subscription_rel_srsublsn - 1] = LSNGetDatum(sublsn);
	else
		nulls[Anum_pg_subscription_rel_srsublsn - 1] = true;

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* 更新目录。 */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* 清理。 */
	table_close(rel, NoLock);
}

/*
 * 更新订阅表的状态。
 */
void
UpdateSubscriptionRelState(Oid subid, Oid relid, char state,
						   XLogRecPtr sublsn)
{
	UpdateSubscriptionRelStateEx(subid, relid, state, sublsn, false);
}

/*
 * 获取订阅表的状态。
 *
 * 当表不在订阅中时返回 SUBREL_STATE_UNKNOWN。
 */
char
GetSubscriptionRelState(Oid subid, Oid relid, XLogRecPtr *sublsn)
{
	HeapTuple	tup;
	char		substate;
	bool		isnull;
	Datum		d;
	Relation	rel;

	/*
	 * 这是为了避免与试图移除此 relstate 的
	 * AlterSubscription 发生竞态。
	 */
	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	/* 尝试查找映射。 */
	tup = SearchSysCache2(SUBSCRIPTIONRELMAP,
						  ObjectIdGetDatum(relid),
						  ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, AccessShareLock);
		*sublsn = InvalidXLogRecPtr;
		return SUBREL_STATE_UNKNOWN;
	}

	/* 获取状态。 */
	substate = ((Form_pg_subscription_rel) GETSTRUCT(tup))->srsubstate;

	/* 获取 LSN */
	d = SysCacheGetAttr(SUBSCRIPTIONRELMAP, tup,
						Anum_pg_subscription_rel_srsublsn, &isnull);
	if (isnull)
		*sublsn = InvalidXLogRecPtr;
	else
		*sublsn = DatumGetLSN(d);

	/* 清理 */
	ReleaseSysCache(tup);

	table_close(rel, AccessShareLock);

	return substate;
}

/*
 * 删除订阅关系映射。可以针对特定
 * 订阅，或特定关系，或两者。
 */
void
RemoveSubscriptionRel(Oid subid, Oid relid)
{
	Relation	rel;
	TableScanDesc scan;
	ScanKeyData skey[2];
	HeapTuple	tup;
	int			nkeys = 0;

	rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

	if (OidIsValid(subid))
	{
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srsubid,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(subid));
	}

	if (OidIsValid(relid))
	{
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srrelid,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(relid));
	}

	/* 执行搜索并删除找到的内容。 */
	scan = table_beginscan_catalog(rel, nkeys, skey);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription_rel subrel;

		subrel = (Form_pg_subscription_rel) GETSTRUCT(tup);

		/*
		 * 当表同步正在进行时，我们不允许删除关系
		 * 映射，除非调用方也更新了
		 * 相应的订阅。这是为了确保在删除相应表时
		 * 不会在系统中留下 tablesync 槽或
		 * origin。
		 */
		if (!OidIsValid(subid) && subrel->srsubstate != SUBREL_STATE_READY)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not drop relation mapping for subscription \"%s\"",
							get_subscription_name(subrel->srsubid, false)),
					 errdetail("Table synchronization for relation \"%s\" is in progress and is in state \"%c\".",
							   get_rel_name(relid), subrel->srsubstate),

			/*
			 * translator: first %s is a SQL ALTER command and second %s is a
			 * SQL DROP command
			 */
					 errhint("Use %s to enable subscription if not already enabled or use %s to drop the subscription.",
							 "ALTER SUBSCRIPTION ... ENABLE",
							 "DROP SUBSCRIPTION ...")));
		}

		CatalogTupleDelete(rel, &tup->t_self);
	}
	table_endscan(scan);

	table_close(rel, RowExclusiveLock);
}

/*
 * 订阅是否有任何关系？
 *
 * 仅在需要知道真/假、且不需要
 * GetSubscriptionRelations 返回的 List 时使用本函数。
 */
bool
HasSubscriptionRelations(Oid subid)
{
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	bool		has_subrels;

	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_subscription_rel_srsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(subid));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, skey);

	/* 只要存在哪怕一个元组，订阅就有表。 */
	has_subrels = HeapTupleIsValid(systable_getnext(scan));

	/* 清理 */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return has_subrels;
}

/*
 * 获取订阅的关系。
 *
 * 若 not_ready 为 true，仅返回不处于 ready
 * 状态的关系，否则返回订阅的全部关系。
 * 返回的列表在当前内存上下文中 palloc。
 */
List *
GetSubscriptionRelations(Oid subid, bool not_ready)
{
	List	   *res = NIL;
	Relation	rel;
	HeapTuple	tup;
	int			nkeys = 0;
	ScanKeyData skey[2];
	SysScanDesc scan;

	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	ScanKeyInit(&skey[nkeys++],
				Anum_pg_subscription_rel_srsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(subid));

	if (not_ready)
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srsubstate,
					BTEqualStrategyNumber, F_CHARNE,
					CharGetDatum(SUBREL_STATE_READY));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, nkeys, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_subscription_rel subrel;
		SubscriptionRelState *relstate;
		Datum		d;
		bool		isnull;

		subrel = (Form_pg_subscription_rel) GETSTRUCT(tup);

		relstate = (SubscriptionRelState *) palloc(sizeof(SubscriptionRelState));
		relstate->relid = subrel->srrelid;
		relstate->state = subrel->srsubstate;
		d = SysCacheGetAttr(SUBSCRIPTIONRELMAP, tup,
							Anum_pg_subscription_rel_srsublsn, &isnull);
		if (isnull)
			relstate->lsn = InvalidXLogRecPtr;
		else
			relstate->lsn = DatumGetLSN(d);

		res = lappend(res, relstate);
	}

	/* 清理 */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return res;
}
