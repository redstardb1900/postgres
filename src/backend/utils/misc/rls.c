/*-------------------------------------------------------------------------
 *
 * rls.c
 *		  与RLS相关的实用函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/utils/misc/rls.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * check_enable_rls
 *
 * 根据关系、row_security 设置和当前角色，判断
 * RLS 是否适用于本查询。RLS_NONE_ENV 表示虽然
 * 本查询不加 RLS，但环境变化可能改变这一点。
 * RLS_NONE 表示该关系上完全没有 RLS，因此我们
 * 无须为它操心。RLS_ENABLED 表示应为该表实现
 * RLS，且若环境变化则计划缓存需要失效。
 *
 *
 * 支持通过 checkAsUser 以另一角色检查（视图等）
 * 传 InvalidOid 则查当前用户。
 *
 * 若 noError 被设为 'true'，则当用户试图绕过 RLS
 * 而又不被允许时，我们直接返回 RLS_ENABLED 而
 * 不是发起 ereport()。这让用户得以检查 RLS 是否
 * 已启用而不必应对真正的错误情形（例如：
 * 需判断是否把关系数据作为错误一部分返回给用户）。
 */
int
check_enable_rls(Oid relid, Oid checkAsUser, bool noError)
{
	Oid			user_id = OidIsValid(checkAsUser) ? checkAsUser : GetUserId();
	HeapTuple	tuple;
	Form_pg_class classform;
	bool		relrowsecurity;
	bool		relforcerowsecurity;
	bool		amowner;

	/* 内建关系无需任何处理 */
	if (relid < (Oid) FirstNormalObjectId)
		return RLS_NONE;

	/* 取关系的 relrowsecurity 与 relforcerowsecurity 标志 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return RLS_NONE;
	classform = (Form_pg_class) GETSTRUCT(tuple);

	relrowsecurity = classform->relrowsecurity;
	relforcerowsecurity = classform->relforcerowsecurity;

	ReleaseSysCache(tuple);

	/* 若关系不带 RLS 则无需处理 */
	if (!relrowsecurity)
		return RLS_NONE;

	/*
	 * BYPASSRLS 用户总是绕过 RLS。注意超级用户总是
	 * 被视为拥有 BYPASSRLS。
	 *
	 * 返回 RLS_NONE_ENV 以表明该决定取决于环境（在
	 * 本例中即 user_id）。
	 */
	if (has_bypassrls_privilege(user_id))
		return RLS_NONE_ENV;

	/*
	 * 表所有者通常绕过 RLS，除非该表已被（所有者）设置
	 * 为 FORCE ROW SECURITY，且这不是引用完整性
	 * 检查。
	 *
	 * 返回 RLS_NONE_ENV 以表明该决定取决于环境（在
	 * 本例中即 user_id）。
	 */
	amowner = object_ownercheck(RelationRelationId, relid, user_id);
	if (amowner)
	{
		/*
		 * 若已在关系上设置 FORCE ROW LEVEL SECURITY，
		 * 则应返回 RLS_ENABLED 以表明应施加 RLS。若
		 * 未设置，或我们在 InNoForceRLSOperation 上下文则返回
		 * RLS_NONE_ENV
		 *
		 * InNoForceRLSOperation 表示即使表已设置 FORCE
		 * RLS，前提是当前用户是所有者。这是专门为确保
		 * 引用完整性检查
		 * 仍能正确运行而设的。
		 *
		 * 这是有意只在确认用户是表所有者后才做的，对引
		 * 用完整性检查来说这应当总是成立的。
		 *
		 */
		if (!relforcerowsecurity || InNoForceRLSOperation())
			return RLS_NONE_ENV;
	}

	/*
	 * 我们应当施加 RLS。不过用户可关闭 row_security
	 * GUC 来换取强制报错。
	 */
	if (!row_security && !noError)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("query would be affected by row-level security policy for table \"%s\"",
						get_rel_name(relid)),
				 amowner ? errhint("To disable the policy for the table's owner, use ALTER TABLE NO FORCE ROW LEVEL SECURITY.") : 0));

	/* 对该关系应完全启用 RLS。 */
	return RLS_ENABLED;
}

/*
 * row_security_active
 *
 * check_enable_rls 包装成可由 SQL 调用的函数，
 * 对本用途而言 RLS_NONE_ENV 与 RLS_NONE 同义。
 */
Datum
row_security_active(PG_FUNCTION_ARGS)
{
	/* 按OID */
	Oid			tableoid = PG_GETARG_OID(0);
	int			rls_status;

	rls_status = check_enable_rls(tableoid, InvalidOid, true);
	PG_RETURN_BOOL(rls_status == RLS_ENABLED);
}

Datum
row_security_active_name(PG_FUNCTION_ARGS)
{
	/* 按限定名 */
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	RangeVar   *tablerel;
	Oid			tableoid;
	int			rls_status;

	/* 查找表名。不能对其加锁，因为我们可能无权限。 */
	tablerel = makeRangeVarFromNameList(textToQualifiedNameList(tablename));
	tableoid = RangeVarGetRelid(tablerel, NoLock, false);

	rls_status = check_enable_rls(tableoid, InvalidOid, true);
	PG_RETURN_BOOL(rls_status == RLS_ENABLED);
}
