/*--------------------------------------------------------------------
 * bgworker_internals.h
 *		POSTGRES 可插拔后台工作进程内部
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/postmaster/bgworker_internals.h
 *--------------------------------------------------------------------
 */
#ifndef BGWORKER_INTERNALS_H
#define BGWORKER_INTERNALS_H

#include "datatype/timestamp.h"
#include "lib/ilist.h"
#include "postmaster/bgworker.h"

/* GUC 选项 */

/*
 * 并行工作进程的最大可能值。
 */
#define MAX_PARALLEL_WORKER_LIMIT 1024

/*
 * 后台工作进程列表，postmaster 私有。
 *
 * 注册时请求数据库连接的工作进程会设置
 * rw_backend，并出现在 BackendList。注意：切勿依赖
 * 连共享内存的工作进程 rw_backend 非 NULL！
 */
typedef struct RegisteredBgWorker
{
	BackgroundWorker rw_worker; /* 其注册表项 */
	struct bkend *rw_backend;	/* 其 BackendList 项，或 NULL */
	pid_t		rw_pid;			/* 未运行则 0 */
	int			rw_child_slot;
	TimestampTz rw_crashed_at;	/* 若非 0，上次崩溃时间 */
	int			rw_shmem_slot;
	bool		rw_terminate;
	slist_node	rw_lnode;		/* 链表节 */
} RegisteredBgWorker;

extern PGDLLIMPORT slist_head BackgroundWorkerList;

extern Size BackgroundWorkerShmemSize(void);
extern void BackgroundWorkerShmemInit(void);
extern void BackgroundWorkerStateChange(bool allow_new_workers);
extern void ForgetBackgroundWorker(slist_mutable_iter *cur);
extern void ReportBackgroundWorkerPID(RegisteredBgWorker *);
extern void ReportBackgroundWorkerExit(slist_mutable_iter *cur);
extern void BackgroundWorkerStopNotifications(pid_t pid);
extern void ForgetUnstartedBackgroundWorkers(void);
extern void ResetBackgroundWorkerCrashTimes(void);

/* 启动后台工作进程的函数，由 postmaster.c 调用 */
extern void StartBackgroundWorker(void) pg_attribute_noreturn();

#ifdef EXEC_BACKEND
extern BackgroundWorker *BackgroundWorkerEntry(int slotno);
#endif

#endif							/* BGWORKER_INTERNALS_H */
