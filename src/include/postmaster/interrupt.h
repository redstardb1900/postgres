/*-------------------------------------------------------------------------
 *
 * interrupt.h
 *	  中断处理例程。
 *
 * 对中断的响应相当多样，多种后端有各自实现，
 * 但此处提供一些通用设施以便代码复用。
 * 以便代码复用。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/postmaster/interrupt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <signal.h>

extern PGDLLIMPORT volatile sig_atomic_t ConfigReloadPending;
extern PGDLLIMPORT volatile sig_atomic_t ShutdownRequestPending;

extern void HandleMainLoopInterrupts(void);
extern void SignalHandlerForConfigReload(SIGNAL_ARGS);
extern void SignalHandlerForCrashExit(SIGNAL_ARGS);
extern void SignalHandlerForShutdownRequest(SIGNAL_ARGS);

#endif
