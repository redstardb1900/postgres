/*-------------------------------------------------------------------------
 *
 * superuser.c
 *	  superuser() 函数。判断用户是否拥有超级用户权限。
 *
 * 所有代码都应使用这两个函数之一来查明用户
 * 是否为超级用户，而不是直接检查
 * pg_authid.rolsuper，以便内建的逃生通道在单
 * 用户场景下可用。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/superuser.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/syscache.h"

/*
 * 常见情形下同一个 roleid（即会话或当前 ID）会
 * 被反复查询。因此我们维护一个简单的单条目缓
 * 存来记录最近请求的 roleid 的状态。缓存可以
 * 通过监视 pg_authid 缓存更新事件按需刷新。
 */
static Oid	last_roleid = InvalidOid;	/* InvalidOid == 缓存无效 */
static bool last_roleid_is_super = false;
static bool roleid_callback_registered = false;

static void RoleidCallback(Datum arg, int cacheid, uint32 hashvalue);


/*
 * 运行此命令的 Postgres 用户拥有 Postgres 超级用户权限
 */
bool
superuser(void)
{
	return superuser_arg(GetUserId());
}


/*
 * 指定角色拥有 Postgres 超级用户权限
 */
bool
superuser_arg(Oid roleid)
{
	bool		result;
	HeapTuple	rtup;

	/* 缓存命中即返回 */
	if (OidIsValid(last_roleid) && last_roleid == roleid)
		return last_roleid_is_super;

	/* 在你删除了全部用户时的特殊逃生路径。 */
	if (!IsUnderPostmaster && roleid == BOOTSTRAP_SUPERUSERID)
		return true;

	/* 好，在 pg_authid 中查找该信息 */
	rtup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(rtup))
	{
		result = ((Form_pg_authid) GETSTRUCT(rtup))->rolsuper;
		ReleaseSysCache(rtup);
	}
	else
	{
		/* 对无效 roleid 报告 "非超级用户" */
		result = false;
	}

	/* 若是首次执行，则为缓存刷新注册回调 */
	if (!roleid_callback_registered)
	{
		CacheRegisterSyscacheCallback(AUTHOID,
									  RoleidCallback,
									  (Datum) 0);
		roleid_callback_registered = true;
	}

	/* 缓存结果供下次使用 */
	last_roleid = roleid;
	last_roleid_is_super = result;

	return result;
}

/*
 * RoleidCallback
 *		系统缓存失效回调函数
 */
static void
RoleidCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/* 角色的超级用户身份若变化，则使本地缓存失效 */
	last_roleid = InvalidOid;
}
