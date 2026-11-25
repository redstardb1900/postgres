/*-------------------------------------------------------------------------
 *
 * fe-auth.h
 *
 *	网络认证例程的定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_AUTH_H
#define FE_AUTH_H

#include "libpq-fe.h"
#include "libpq-int.h"


/* fe-auth.c 中函数的原型 */
extern int	pg_fe_sendauth(AuthRequest areq, int payloadlen, PGconn *conn);
extern char *pg_fe_getusername(uid_t user_id, PQExpBuffer errorMessage);
extern char *pg_fe_getauthname(PQExpBuffer errorMessage);

/* fe-auth-scram.c 中的机制 */
extern const pg_fe_sasl_mech pg_scram_mech;
extern char *pg_fe_scram_build_secret(const char *password,
									  int iterations,
									  const char **errstr);

#endif							/* FE_AUTH_H */
