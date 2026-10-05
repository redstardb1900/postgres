/*-------------------------------------------------------------------------
 *
 * rewriteRemove.c
 *	  删除重写规则的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteRemove.c
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
#include "catalog/namespace.h"
#include "catalog/pg_rewrite.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * 规则删除的核心。
 */
void
RemoveRewriteRuleById(Oid ruleOid)
{
	Relation	RewriteRelation;
	ScanKeyData skey[1];
	SysScanDesc rcscan;
	Relation	event_relation;
	HeapTuple	tuple;
	Oid			eventRelationOid;

	/*
	 * 打开 pg_rewrite 关系。
	 */
	RewriteRelation = table_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * 查找目标规则的元组。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_rewrite_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleOid));

	rcscan = systable_beginscan(RewriteRelation, RewriteOidIndexId, true,
								NULL, 1, skey);

	tuple = systable_getnext(rcscan);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for rule %u", ruleOid);

	/*
	 * 我们最好取得 AccessExclusiveLock，以确保没有可能依赖
	 * 这条规则的查询正在进行。（注意：若它不是 ON SELECT
	 * 规则，较弱的锁就够了。）
	 */
	eventRelationOid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	event_relation = table_open(eventRelationOid, AccessExclusiveLock);

	if (!allowSystemTableMods && IsSystemRelation(event_relation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(event_relation))));

	/*
	 * 现在删除该规则的 pg_rewrite 元组
	 */
	CatalogTupleDelete(RewriteRelation, &tuple->t_self);

	systable_endscan(rcscan);

	table_close(RewriteRelation, RowExclusiveLock);

	/*
	 * 发出共享失效通知，迫使所有后端（包括我自己！）
	 * 用新的规则集更新 relcache 项。
	 */
	CacheInvalidateRelcache(event_relation);

	/* 关闭关系，但把锁保持到提交…… */
	table_close(event_relation, NoLock);
}
