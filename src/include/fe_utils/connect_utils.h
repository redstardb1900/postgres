/*-------------------------------------------------------------------------
 *
 * 前端连接/断开数据库的设施。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/connect_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECT_UTILS_H
#define CONNECT_UTILS_H

#include "libpq-fe.h"

enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
};

/*  connectDatabase/connectMaintenanceDatabase 所需参数  */
typedef struct _connParams
{
	/*  这些字段记录实际命令行参数  */
	const char *dbname;			/* 略 */
	const char *pghost;
	const char *pgport;
	const char *pguser;
	enum trivalue prompt_password;
	/*  非 NULL 时覆盖命令行得到的库名  */
	/*  （*仅*库名，不含连接串中其他内容）  */
	const char *override_dbname;
} ConnParams;

extern PGconn *connectDatabase(const ConnParams *cparams,
							   const char *progname,
							   bool echo, bool fail_ok,
							   bool allow_password_reuse);

extern PGconn *connectMaintenanceDatabase(ConnParams *cparams,
										  const char *progname, bool echo);

extern void disconnectDatabase(PGconn *conn);

#endif							/* CONNECT_UTILS_H */
