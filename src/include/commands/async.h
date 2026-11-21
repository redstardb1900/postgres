/*-------------------------------------------------------------------------
 *
 * async.h
 *	异步通知：NOTIFY、LISTEN、UNLISTEN
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/async.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_H
#define ASYNC_H

#include <signal.h>

/*
 * 通知队列所用的 SLRU 页缓冲个数。
 */
#define NUM_NOTIFY_BUFFERS	8

extern PGDLLIMPORT bool Trace_notify;
extern PGDLLIMPORT volatile sig_atomic_t notifyInterruptPending;

extern Size AsyncShmemSize(void);
extern void AsyncShmemInit(void);

extern void NotifyMyFrontEnd(const char *channel,
							 const char *payload,
							 int32 srcPid);

/* notify 相关 SQL 语句 */
extern void Async_Notify(const char *channel, const char *payload);
extern void Async_Listen(const char *channel);
extern void Async_Unlisten(const char *channel);
extern void Async_UnlistenAll(void);

/* 事务提交时执行（或取消）外发 notify 处理 */
extern void PreCommit_Notify(void);
extern void AtCommit_Notify(void);
extern void AtAbort_Notify(void);
extern void AtSubCommit_Notify(void);
extern void AtSubAbort_Notify(void);
extern void AtPrepare_Notify(void);

/* 入站 notify 的信号处理（PROCSIG_NOTIFY_INTERRUPT） */
extern void HandleNotifyInterrupt(void);

/* 处理中断 */
extern void ProcessNotifyInterrupt(bool flush);

#endif							/* ASYNC_H */
