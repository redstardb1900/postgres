/*-------------------------------------------------------------------------
 *
 * 前端代码的查询取消支持
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/cancel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CANCEL_H
#define CANCEL_H

#include <signal.h>

#include "libpq-fe.h"

extern PGDLLIMPORT volatile sig_atomic_t CancelRequested;

extern void SetCancelConn(PGconn *conn);
extern void ResetCancelConn(void);

/*
 * 可选用回调在取消时调用。
 * 略
 */
extern void setup_cancel_handler(void (*query_cancel_callback) (void));

#endif							/* CANCEL_H */
