/*-------------------------------------------------------------------------
 *
 * usercontext.c
 *	  以其他数据库用户身份运行代码的便捷函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/usercontext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/usercontext.h"

/*
 * 临时切换到新的用户 ID。
 *
 * 如果当前用户无权对目标用户 SET ROLE，
 * 即 ERROR。
 *
 * 如果目标用户无权 SET ROLE 回当前用户，则会强制施加
 * SECURITY_RESTRICTED_OPERATION，并创建新的 GUC 嵌套
 * 层级，使任何设置变更都可以回滚。
 */
void
SwitchToUntrustedUser(Oid userid, UserContext *context)
{
	/* 获取当前用户 ID 与安全上下文。 */
	GetUserIdAndSecContext(&context->save_userid,
						   &context->save_sec_context);

	/* 检查我们是否有足够的权限来接用目标角色。 */
	if (!member_can_set_role(context->save_userid, userid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("role \"%s\" cannot SET ROLE to \"%s\"",
						GetUserNameFromId(context->save_userid, false),
						GetUserNameFromId(userid, false))));

	/*
	 * 尽力防止我们要切换到的用户冒用当前用户权限，
	 * 除非该用户本来就能对当前用户 SET ROLE。
	 * 
	 */
	if (member_can_set_role(userid, context->save_userid))
	{
		/*
		 * 两个用户彼此都能 SET ROLE，
		 * 施加安全限制没有意义。就让用户随心所欲吧
		 * 。
		 */
		SetUserIdAndSecContext(userid, context->save_sec_context);
		context->save_nestlevel = -1;
	}
	else
	{
		int			sec_context = context->save_sec_context;

		/*
		 * 本用户能对目标用户 SET ROLE，反之不行，所以
		 * 要通过设置 SECURITY_RESTRICTED_OPERATION 来防范目
		 * 标用户，阻止会话状态的某些更改。并建立新的
		 * GUC 嵌套层级，可回滚目标用户身份运行的代码
		 * 可能做出的 GUC 更改——万一它们带有恶意。
		 * 
		 */
		sec_context |= SECURITY_RESTRICTED_OPERATION;
		SetUserIdAndSecContext(userid, sec_context);
		context->save_nestlevel = NewGUCNestLevel();
	}
}

/*
 * 切换回原来的用户 ID。
 *
 * 如果我们创建了新的 GUC 嵌套层级，还要回滚
 * 变更。
 */
void
RestoreUserContext(UserContext *context)
{
	if (context->save_nestlevel != -1)
		AtEOXact_GUC(false, context->save_nestlevel);
	SetUserIdAndSecContext(context->save_userid, context->save_sec_context);
}
