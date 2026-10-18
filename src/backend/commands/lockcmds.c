/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK 命令支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/lockcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait);
static AclResult LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid);
static void RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid,
										 Oid oldrelid, void *arg);
static void LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
							List *ancestor_views);

/*
 * LOCK TABLE
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*
	 * 遍历该列表，一次处理一个被命名的关系
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(p);
		bool		recurse = rv->inh;
		Oid			reloid;

		reloid = RangeVarGetRelidExtended(rv, lockstmt->mode,
										  lockstmt->nowait ? RVR_NOWAIT : 0,
										  RangeVarCallbackForLockTable,
										  (void *) &lockstmt->mode);

		if (get_rel_relkind(reloid) == RELKIND_VIEW)
			LockViewRecurse(reloid, lockstmt->mode, lockstmt->nowait, NIL);
		else if (recurse)
			LockTableRecurse(reloid, lockstmt->mode, lockstmt->nowait);
	}
}

/*
 * 在获取被命名表的表锁之前，检查我们是否
 * 有权这样做。
 */
static void
RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid, Oid oldrelid,
							 void *arg)
{
	LOCKMODE	lockmode = *(LOCKMODE *) arg;
	char		relkind;
	char		relpersistence;
	AclResult	aclresult;

	if (!OidIsValid(relid))
		return;					/* 不存在，因此无需权限检查 */
	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;					/* 糟糕，关系已被并发删除；无需权限
								 * 检查 */

	/* 目前，我们只允许锁定普通表或视图 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot lock relation \"%s\"",
						rv->relname),
				 errdetail_relkind_not_supported(relkind)));

	/*
	 * 如果在此事务中访问了临时关系，
	 * 则做记录。
	 */
	relpersistence = get_rel_persistence(relid);
	if (relpersistence == RELPERSISTENCE_TEMP)
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/* 检查权限。 */
	aclresult = LockTableAclCheck(relid, lockmode, GetUserId());
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
}

/*
 * 在继承树上递归应用 LOCK TABLE
 *
 * 这不会检查对子表执行 LOCK TABLE 的权限，
 * 因为能走到这里意味着用户有权限锁定
 * 父表，而这已经足够了。
 */
static void
LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait)
{
	List	   *children;
	ListCell   *lc;

	children = find_all_inheritors(reloid, NoLock, NULL);

	foreach(lc, children)
	{
		Oid			childreloid = lfirst_oid(lc);

		/* 父表已锁定。 */
		if (childreloid == reloid)
			continue;

		if (!nowait)
			LockRelationOid(childreloid, lockmode);
		else if (!ConditionalLockRelationOid(childreloid, lockmode))
		{
			/* 尝试按名称抛出错误；关系可能已被删除... */
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* 子关系已被并发删除，直接跳过它 */
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on relation \"%s\"",
							relname)));
		}

		/*
		 * 即使我们获取了锁，子关系也可能已被并发
		 * 删除。如果是这样，我们可以跳过它。
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(childreloid)))
		{
			/* 释放无用的锁 */
			UnlockRelationOid(childreloid, lockmode);
			continue;
		}
	}
}

/*
 * 在视图上递归应用 LOCK TABLE
 *
 * 视图定义查询中出现的所有表和视图都会以相同的锁模式
 * 被递归锁定。
 */

typedef struct
{
	LOCKMODE	lockmode;		/* 要使用的锁模式 */
	bool		nowait;			/* 无等待模式 */
	Oid			check_as_user;	/* 用于检查权限的用户 */
	Oid			viewoid;		/* 要锁定的视图的 OID */
	List	   *ancestor_views; /* 祖先视图的 OIDs */
} LockViewRecurse_context;

static bool
LockViewRecurse_walker(Node *node, LockViewRecurse_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);
			AclResult	aclresult;

			Oid			relid = rte->relid;
			char		relkind = rte->relkind;
			char	   *relname = get_rel_name(relid);

			/* 目前，我们只允许锁定普通表或视图。 */
			if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
				relkind != RELKIND_VIEW)
				continue;

			/*
			 * 我们可能正在处理一个自引用视图。如果是这样，
			 * 我们只需停止递归，因为我们已经锁定了它。
			 */
			if (list_member_oid(context->ancestor_views, relid))
				continue;

			/*
			 * 以指定用户的身份检查权限。该用户要么是
			 * 视图所有者，要么是当前用户。
			 */
			aclresult = LockTableAclCheck(relid, context->lockmode,
										  context->check_as_user);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(relkind), relname);

			/* 我们有足够的权限锁定该关系；执行锁定。 */
			if (!context->nowait)
				LockRelationOid(relid, context->lockmode);
			else if (!ConditionalLockRelationOid(relid, context->lockmode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));

			if (relkind == RELKIND_VIEW)
				LockViewRecurse(relid, context->lockmode, context->nowait,
								context->ancestor_views);
			else if (rte->inh)
				LockTableRecurse(relid, context->lockmode, context->nowait);
		}

		return query_tree_walker(query,
								 LockViewRecurse_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  LockViewRecurse_walker,
								  context);
}

static void
LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
				List *ancestor_views)
{
	LockViewRecurse_context context;
	Relation	view;
	Query	   *viewquery;

	/* 调用方已经锁定了该视图 */
	view = table_open(reloid, NoLock);
	viewquery = get_view_query(view);

	/*
	 * 如果视图设置了 security_invoker 属性，则以当前用户的身份
	 * 检查权限。否则，以视图所有者的身份检查权限。
	 */
	context.lockmode = lockmode;
	context.nowait = nowait;
	if (RelationHasSecurityInvoker(view))
		context.check_as_user = GetUserId();
	else
		context.check_as_user = view->rd_rel->relowner;
	context.viewoid = reloid;
	context.ancestor_views = lappend_oid(ancestor_views, reloid);

	LockViewRecurse_walker((Node *) viewquery, &context);

	context.ancestor_views = list_delete_last(context.ancestor_views);

	table_close(view, NoLock);
}

/*
 * 检查当前用户是否被允许锁定该关系。
 */
static AclResult
LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid)
{
	AclResult	aclresult;
	AclMode		aclmask;

	/* 这些权限中的任何一个都允许任意锁模式 */
	aclmask = ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE;

	/* SELECT 权限还允许 ACCESS SHARE 及以下级别的锁模式 */
	if (lockmode <= AccessShareLock)
		aclmask |= ACL_SELECT;

	/* INSERT 权限还允许 ROW EXCLUSIVE 及以下级别的锁模式 */
	if (lockmode <= RowExclusiveLock)
		aclmask |= ACL_INSERT;

	aclresult = pg_class_aclcheck(reloid, userid, aclmask);

	return aclresult;
}
