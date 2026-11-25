/*-------------------------------------------------------------------------
 *
 * libpq-events.h
 *	本文件所含定义对调用
 *	libpq 事件 API 的应用程序有用，但对 libpq
 *	普通用户无兴趣。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/libpq-events.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_EVENTS_H
#define LIBPQ_EVENTS_H

#include "libpq-fe.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 回调事件标识 */
typedef enum
{
	PGEVT_REGISTER,
	PGEVT_CONNRESET,
	PGEVT_CONNDESTROY,
	PGEVT_RESULTCREATE,
	PGEVT_RESULTCOPY,
	PGEVT_RESULTDESTROY
} PGEventId;

typedef struct
{
	PGconn	   *conn;
} PGEventRegister;

typedef struct
{
	PGconn	   *conn;
} PGEventConnReset;

typedef struct
{
	PGconn	   *conn;
} PGEventConnDestroy;

typedef struct
{
	PGconn	   *conn;
	PGresult   *result;
} PGEventResultCreate;

typedef struct
{
	const PGresult *src;
	PGresult   *dest;
} PGEventResultCopy;

typedef struct
{
	PGresult   *result;
} PGEventResultDestroy;

typedef int (*PGEventProc) (PGEventId evtId, void *evtInfo, void *passThrough);

/* 向给定 PGconn 注册一个事件过程。 */
extern int	PQregisterEventProc(PGconn *conn, PGEventProc proc,
								const char *name, void *passThrough);

/* 把所给过程的 PGconn 实例数据设为 data。 */
extern int	PQsetInstanceData(PGconn *conn, PGEventProc proc, void *data);

/* 取所给过程的 PGconn 实例数据。 */
extern void *PQinstanceData(const PGconn *conn, PGEventProc proc);

/* 把所给过程的 PGresult 实例数据设为 data。 */
extern int	PQresultSetInstanceData(PGresult *result, PGEventProc proc, void *data);

/* 取所给过程的 PGresult 实例数据。 */
extern void *PQresultInstanceData(const PGresult *result, PGEventProc proc);

/* 为应用程序所建 PGresult 触发 RESULTCREATE 事件。 */
extern int	PQfireResultCreateEvents(PGconn *conn, PGresult *res);

#ifdef __cplusplus
}
#endif

#endif							/* LIBPQ_EVENTS_H */
