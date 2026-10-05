/*-------------------------------------------------------------------------
 *
 * relation.c
 *	  与关系相关的通用例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/relation.c
 *
 * 说明
 *	  本文件包含实现关系访问的 relation_ 例程
 *	  （表、索引等）。针对关系子类型的支持
 *	  应放进各自的文件，而不是这里。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/inval.h"
#include "utils/syscache.h"


/* ----------------
 *		relation_open - 按关系 OID 打开任意关系
 *
 *		若 lockmode 不是 "NoLock"，就在该关系上取得
 *		指定种类的锁。（一般而言，只有当调用方知道
 *		自己已经持有该关系上某种适当的锁时，
 *		才应使用 NoLock。）
 *
 *		若关系不存在，则报错。
 *
 *		注意：所谓 "关系"，是指任何拥有 pg_class 项的对象。调用方
 *		应当检查 relkind 是否是自己能够处理的种类。
 * ----------------
 */
Relation
relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* 在尝试打开 relcache 项之前先取得锁 */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/* relcache 完成全部实际工作... */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	/*
	 * 若锁不是我们自己取得的，则断言调用方持有一把锁，
	 * 引导模式除外，那种模式下不使用锁。
	 */
	Assert(lockmode != NoLock ||
		   IsBootstrapProcessingMode() ||
		   CheckRelationLockedByMe(r, AccessShareLock, true));

	/* 记下我们访问过临时关系 */
	if (RelationUsesLocalBuffers(r))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	pgstat_init_relation(r);

	return r;
}

/* ----------------
 *		try_relation_open - 按关系 OID 打开任意关系
 *
 *		与 relation_open 相同，只是关系不存在时
 *		返回 NULL，而不是失败。
 * ----------------
 */
Relation
try_relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* 先取得锁 */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/*
	 * 既然已经持有锁，就探查该关系
	 * 是否真的存在。
	 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
	{
		/* 释放无用的锁 */
		if (lockmode != NoLock)
			UnlockRelationOid(relationId, lockmode);

		return NULL;
	}

	/* 现在做 relcache 装载应当是安全的 */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	/* 若锁不是我们自己取得的，则断言调用方持有一把锁 */
	Assert(lockmode != NoLock ||
		   CheckRelationLockedByMe(r, AccessShareLock, true));

	/* 记下我们访问过临时关系 */
	if (RelationUsesLocalBuffers(r))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	pgstat_init_relation(r);

	return r;
}

/* ----------------
 *		relation_openrv - 打开由 RangeVar 指定的任意关系
 *
 *		与 relation_open 相同，但关系由 RangeVar 指定。
 * ----------------
 */
Relation
relation_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Oid			relOid;

	/*
	 * 在尝试打开关系之前，先检查共享缓存失效消息。
	 * 即使我们已经持有该关系上的锁，这也是需要的，
	 * 因为 GRANT/REVOKE 执行时并不对目标关系加任何锁，
	 * 而我们要确保看到当前的 ACL 信息。若请求的是 NoLock，
	 * 可以跳过，假定这样的调用并非当前命令中的第一次，
	 * 因此我们应当已经相当新了。（XXX 这一切都值得
	 * 重新设计，但目前我们仍按历来的做法
	 * 继续做。）
	 * done historically.)
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* 用名字空间搜索查找并锁定相应的关系 */
	relOid = RangeVarGetRelid(relation, lockmode, false);

	/* 其余工作交给 relation_open */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_openrv_extended - 打开由 RangeVar 指定的任意关系
 *
 *		与 relation_openrv 相同，但多一个 missing_ok 参数，
 *		允许在找不到关系时返回 NULL 而不是报错。
 *		（注意，其他原因，例如权限问题，
 *		仍会导致 ereport。）
 * ----------------
 */
Relation
relation_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
						 bool missing_ok)
{
	Oid			relOid;

	/*
	 * 在尝试打开关系之前，先检查共享缓存失效消息。
	 * 见 relation_openrv() 中的注释。
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* 用名字空间搜索查找并锁定相应的关系 */
	relOid = RangeVarGetRelid(relation, lockmode, missing_ok);

	/* 找不到时返回 NULL */
	if (!OidIsValid(relOid))
		return NULL;

	/* 其余工作交给 relation_open */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_close - 关闭任意关系
 *
 *		若 lockmode 不是 "NoLock"，随后就释放指定的锁。
 *
 *		注意，把锁保持到 relation_close 之后常常是合理的；
 *		那种情况下，锁会在事务结束时自动释放。
 * ----------------
 */
void
relation_close(Relation relation, LOCKMODE lockmode)
{
	LockRelId	relid = relation->rd_lockInfo.lockRelId;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* relcache 完成实际工作... */
	RelationClose(relation);

	if (lockmode != NoLock)
		UnlockRelationId(&relid, lockmode);
}
