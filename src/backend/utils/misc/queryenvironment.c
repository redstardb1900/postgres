/*-------------------------------------------------------------------------
 *
 * queryenvironment.c
 *	  查询环境，用于存储上下文特定的值，如临时命名
 *	  关系。最初用于为来自"普通"关系的增量信息提供
 *	  命名元组存储。
 *
 * 最初的实现使用列表，因为在任一上下文中这类关系
 * 的数量预期很少。如果这成为性能问题，则实现
 * 可以改变而对调用者没有其他影响，因为这是一
 * 个不透明结构体。这也是之所以要提供创建函数的
 * 原因。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/queryenvironment.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "utils/queryenvironment.h"
#include "utils/rel.h"

/*
 * 查询环境的私有状态。
 */
struct QueryEnvironment
{
	List	   *namedRelList;
};


QueryEnvironment *
create_queryEnv(void)
{
	return (QueryEnvironment *) palloc0(sizeof(QueryEnvironment));
}

EphemeralNamedRelationMetadata
get_visible_ENR_metadata(QueryEnvironment *queryEnv, const char *refname)
{
	EphemeralNamedRelation enr;

	Assert(refname != NULL);

	if (queryEnv == NULL)
		return NULL;

	enr = get_ENR(queryEnv, refname);

	if (enr)
		return &(enr->md);

	return NULL;
}

/*
 * 注册一个供给定环境使用的命名关系。
 *
 * 如果它专用于规划目的，则 tstate 字段可以留为
 * NULL；
 */
void
register_ENR(QueryEnvironment *queryEnv, EphemeralNamedRelation enr)
{
	Assert(enr != NULL);
	Assert(get_ENR(queryEnv, enr->md.name) == NULL);

	queryEnv->namedRelList = lappend(queryEnv->namedRelList, enr);
}

/*
 * 按名称注销一个临时关系。这可能是一个很少用到
 * 的函数，但似乎应当提供它以"以防万一"。
 */
void
unregister_ENR(QueryEnvironment *queryEnv, const char *name)
{
	EphemeralNamedRelation match;

	match = get_ENR(queryEnv, name);
	if (match)
		queryEnv->namedRelList = list_delete(queryEnv->namedRelList, match);
}

/*
 * 如果在给定集合中存在名称匹配，这会返回一个
 * ENR；未找到匹配时静默返回 NULL。
 */
EphemeralNamedRelation
get_ENR(QueryEnvironment *queryEnv, const char *name)
{
	ListCell   *lc;

	Assert(name != NULL);

	if (queryEnv == NULL)
		return NULL;

	foreach(lc, queryEnv->namedRelList)
	{
		EphemeralNamedRelation enr = (EphemeralNamedRelation) lfirst(lc);

		if (strcmp(enr->md.name, name) == 0)
			return enr;
	}

	return NULL;
}

/*
 * 根据被填充的字段，获取 ENR 的 TupleDesc。
 *
 *
 * 当 TupleDesc 基于来自系统目录的关系时，我们依
 * 赖该关系同时被使用，因此相应的锁已被持有。
 * 在这里加锁反正也已经太迟了。
 */
TupleDesc
ENRMetadataGetTupDesc(EphemeralNamedRelationMetadata enrmd)
{
	TupleDesc	tupdesc;

	/* 这些字段必须且只能填充其中一个。 */
	Assert((enrmd->reliddesc == InvalidOid) != (enrmd->tupdesc == NULL));

	if (enrmd->tupdesc != NULL)
		tupdesc = enrmd->tupdesc;
	else
	{
		Relation	relation;

		relation = table_open(enrmd->reliddesc, NoLock);
		tupdesc = relation->rd_att;
		table_close(relation, NoLock);
	}

	return tupdesc;
}
