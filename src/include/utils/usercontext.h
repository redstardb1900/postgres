/*-------------------------------------------------------------------------
 *
 * usercontext.h
 *	  以便以另一数据库用户身份运行代码的便利函数。
 *
 *-------------------------------------------------------------------------
 */
#ifndef USERCONTEXT_H
#define USERCONTEXT_H

/*
 * 临时改为以另一用户运行时，本结构保存
 * 恢复原状态所需的细节。
 */
typedef struct UserContext
{
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
} UserContext;

/*  函数原型。  */
extern void SwitchToUntrustedUser(Oid userid, UserContext *context);
extern void RestoreUserContext(UserContext *context);

#endif							/* USERCONTEXT_H */
