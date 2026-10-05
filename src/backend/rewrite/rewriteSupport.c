/*-------------------------------------------------------------------------
 *
 * rewriteSupport.c
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteSupport.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_rewrite.h"
#include "rewrite/rewriteSupport.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 是否有给定名称的规则？
 */
bool
IsDefinedRewriteRule(Oid owningRel, const char *ruleName)
{
	return SearchSysCacheExists2(RULERELNAME,
								 ObjectIdGetDatum(owningRel),
								 PointerGetDatum(ruleName));
}


/*
 * SetRelationRuleStatus
 *		设置 pg_class 里该关系的 relhasrules 成员的值。
 *
 * 注意：调用者必须对该关系持有适当的锁。
 *
 * 注意：本操作的一个重要副作用是向所有后端
 * （包括我自己）发出 SI 失效消息，使 relcache
 * 项被刷新，或用该表的新规则集更新。
 * 即使我们发现 pg_class 行不需要改动，
 * 这也必须发生。
 */
void
SetRelationRuleStatus(Oid relationId, bool relHasRules)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classForm;

	/*
	 * 用系统缓存查找，在 pg_class 里找到要更新的元组。
	 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classForm = (Form_pg_class) GETSTRUCT(tuple);

	if (classForm->relhasrules != relHasRules)
	{
		/* 执行更新 */
		classForm->relhasrules = relHasRules;

		CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);
	}
	else
	{
		/* 不必改元组，但仍强制重建 relcache */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * 查找规则 oid。
 *
 * 若 missing_ok 为假，规则名未找到就报错。若
 * 为真，就只返回 InvalidOid。
 */
Oid
get_rewrite_oid(Oid relid, const char *rulename, bool missing_ok)
{
	HeapTuple	tuple;
	Form_pg_rewrite ruleform;
	Oid			ruleoid;

	/* 查找规则的 pg_rewrite 元组，取得它的 OID */
	tuple = SearchSysCache2(RULERELNAME,
							ObjectIdGetDatum(relid),
							PointerGetDatum(rulename));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						rulename, get_rel_name(relid))));
	}
	ruleform = (Form_pg_rewrite) GETSTRUCT(tuple);
	Assert(relid == ruleform->ev_class);
	ruleoid = ruleform->oid;
	ReleaseSysCache(tuple);
	return ruleoid;
}
