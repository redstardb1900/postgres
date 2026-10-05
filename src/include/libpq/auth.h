/*-------------------------------------------------------------------------
 *
 * auth.h
 *	  网络认证例程的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_H
#define AUTH_H

#include "libpq/libpq-be.h"

extern PGDLLIMPORT char *pg_krb_server_keyfile;
extern PGDLLIMPORT bool pg_krb_caseins_users;
extern PGDLLIMPORT bool pg_gss_accept_delegation;

extern void ClientAuthentication(Port *port);
extern void sendAuthRequest(Port *port, AuthRequest areq, const char *extradata,
							int extralen);

/* 供插件在 ClientAuthentication() 中取得控制权的钩子 */
typedef void (*ClientAuthentication_hook_type) (Port *, int);
extern PGDLLIMPORT ClientAuthentication_hook_type ClientAuthentication_hook;

/* 口令变换器的钩子类型 */
typedef char *(*auth_password_hook_typ) (char *input);

/* 默认 LDAP 口令变换钩子，可被共享库覆盖 */
extern PGDLLIMPORT auth_password_hook_typ ldap_password_hook;

#endif							/* AUTH_H */
