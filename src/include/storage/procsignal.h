/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  进程间发信号例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

#include "storage/backendid.h"


/*
 * 向 Postgres 子进程（后端或辅助进程，如
 * checkpointer）发信号的原因。我们能应对并发的不同
 * 原因的信号。但若同一原因在短时间内被发信号多次，
 * 该进程多半只会观察到一次通知。
 * 对目前的用途这没问题。
 *
 * 另因竞态，必须把所有信号定义成即便进程误收
 * 一条也无害。
 */
typedef enum
{
	PROCSIG_CATCHUP_INTERRUPT,	/* sinval 追赶中断 */
	PROCSIG_NOTIFY_INTERRUPT,	/* listen/notify 中断 */
	PROCSIG_PARALLEL_MESSAGE,	/* 来自协作并行后端的消息 */
	PROCSIG_WALSND_INIT_STOPPING,	/* 请 walsenders 准备关闭  */
	PROCSIG_BARRIER,			/* 全局屏障中断  */
	PROCSIG_LOG_MEMORY_CONTEXT, /* 请后端记录内存上下文 */
	PROCSIG_PARALLEL_APPLY_MESSAGE, /* 来自并行应用 worker 的消息 */

	/* 恢复冲突原因 */
	PROCSIG_RECOVERY_CONFLICT_DATABASE,
	PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
	PROCSIG_RECOVERY_CONFLICT_LOCK,
	PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
	PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
	PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
	PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,

	NUM_PROCSIGNALS				/* 须为末项! */
} ProcSignalReason;

typedef enum
{
	PROCSIGNAL_BARRIER_SMGRRELEASE	/* 请 smgr 关闭文件 */
} ProcSignalBarrierType;

/*
 * procsignal.c 中函数的原型
 */
extern Size ProcSignalShmemSize(void);
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(int pss_idx);
extern int	SendProcSignal(pid_t pid, ProcSignalReason reason,
						   BackendId backendId);

extern uint64 EmitProcSignalBarrier(ProcSignalBarrierType type);
extern void WaitForProcSignalBarrier(uint64 generation);
extern void ProcessProcSignalBarrier(void);

extern void procsignal_sigusr1_handler(SIGNAL_ARGS);

#endif							/* PROCSIGNAL_H */
