/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  postmaster 与其子进程之间的发信号例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pmsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PMSIGNAL_H
#define PMSIGNAL_H

#include <signal.h>

#ifdef HAVE_SYS_PRCTL_H
#include "sys/prctl.h"
#endif

#ifdef HAVE_SYS_PROCCTL_H
#include "sys/procctl.h"
#endif

/*
 * 向 postmaster 发信号的原因。我们能应对同时出现的
 * 不同原因的信号。但若同一原因在短时间内被发信号
 * 多次，postmaster 多半只会观察到一次通知。
 * 对目前的用途这没问题。
 */
typedef enum
{
	PMSIGNAL_RECOVERY_STARTED,	/* 恢复已开始 */
	PMSIGNAL_BEGIN_HOT_STANDBY, /* 开始热备 */
	PMSIGNAL_ROTATE_LOGFILE,	/* 向 syslogger 发 SIGUSR1 以轮转日志 */
	PMSIGNAL_START_AUTOVAC_LAUNCHER,	/* 启动自动清理 launcher */
	PMSIGNAL_START_AUTOVAC_WORKER,	/* 启动自动清理 worker */
	PMSIGNAL_BACKGROUND_WORKER_CHANGE,	/* 背景工作进程状态变化 */
	PMSIGNAL_START_WALRECEIVER, /* 启动 walreceiver */
	PMSIGNAL_ADVANCE_STATE_MACHINE, /* 推进 postmaster 的状态机 */

	NUM_PMSIGNALS				/* 必须为枚举末项！ */
} PMSignalReason;

/*
 * postmaster 向其子进程发送 SIGQUIT 的原因。
 */
typedef enum
{
	PMQUIT_NOT_SENT = 0,		/* postmaster 未发送 SIGQUIT */
	PMQUIT_FOR_CRASH,			/* 另有后端崩溃 */
	PMQUIT_FOR_STOP				/* 接到立即停止命令 */
} QuitSignalReason;

/* PMSignalData 是不透明结构，细节仅 pmsignal.c 内可知 */
typedef struct PMSignalData PMSignalData;

/*
 * pmsignal.c 中函数的原型
 */
extern Size PMSignalShmemSize(void);
extern void PMSignalShmemInit(void);
extern void SendPostmasterSignal(PMSignalReason reason);
extern bool CheckPostmasterSignal(PMSignalReason reason);
extern void SetQuitSignalReason(QuitSignalReason reason);
extern QuitSignalReason GetQuitSignalReason(void);
extern int	AssignPostmasterChildSlot(void);
extern bool ReleasePostmasterChildSlot(int slot);
extern bool IsPostmasterChildWalSender(int slot);
extern void MarkPostmasterChildActive(void);
extern void MarkPostmasterChildInactive(void);
extern void MarkPostmasterChildWalSender(void);
extern bool PostmasterIsAliveInternal(void);
extern void PostmasterDeathSignalInit(void);


/*
 * 是否有办法在父进程死亡时请求信号？
 *
 * 若有，pmsignal.c 会设置信号处理函数，在父进程死亡时
 * 置位标志。先检查该标志使 PostmasterIsAlive() 在
 * postmaster 通常仍活着时开销小得多。
 */
#if (defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_PDEATHSIG)) || \
	(defined(HAVE_SYS_PROCCTL_H) && defined(PROC_PDEATHSIG_CTL))
#define USE_POSTMASTER_DEATH_SIGNAL
#endif

#ifdef USE_POSTMASTER_DEATH_SIGNAL
extern PGDLLIMPORT volatile sig_atomic_t postmaster_possibly_dead;

static inline bool
PostmasterIsAlive(void)
{
	if (likely(!postmaster_possibly_dead))
		return true;
	return PostmasterIsAliveInternal();
}
#else
#define PostmasterIsAlive() PostmasterIsAliveInternal()
#endif

#endif							/* PMSIGNAL_H */
