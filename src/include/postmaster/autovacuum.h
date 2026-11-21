/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  集成自动清理守护进程的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/autovacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

#include "storage/block.h"

/*
 * 其他进程可向自动清理请求工作，由 AutoVacuumWorkItem
 * 标识。
 */
typedef enum
{
	AVW_BRINSummarizeRange
} AutoVacuumWorkItemType;


/* GUC 变量 */
extern PGDLLIMPORT bool autovacuum_start_daemon;
extern PGDLLIMPORT int autovacuum_max_workers;
extern PGDLLIMPORT int autovacuum_work_mem;
extern PGDLLIMPORT int autovacuum_naptime;
extern PGDLLIMPORT int autovacuum_vac_thresh;
extern PGDLLIMPORT double autovacuum_vac_scale;
extern PGDLLIMPORT int autovacuum_vac_ins_thresh;
extern PGDLLIMPORT double autovacuum_vac_ins_scale;
extern PGDLLIMPORT int autovacuum_anl_thresh;
extern PGDLLIMPORT double autovacuum_anl_scale;
extern PGDLLIMPORT int autovacuum_freeze_max_age;
extern PGDLLIMPORT int autovacuum_multixact_freeze_max_age;
extern PGDLLIMPORT double autovacuum_vac_cost_delay;
extern PGDLLIMPORT int autovacuum_vac_cost_limit;

/* 自动清理启动器 PID，仅在工作进程关闭时有效 */
extern PGDLLIMPORT int AutovacuumLauncherPid;

extern PGDLLIMPORT int Log_autovacuum_min_duration;

/* 状态查询函数 */
extern bool AutoVacuumingActive(void);
extern bool IsAutoVacuumLauncherProcess(void);
extern bool IsAutoVacuumWorkerProcess(void);

#define IsAnyAutoVacuumProcess() \
	(IsAutoVacuumLauncherProcess() || IsAutoVacuumWorkerProcess())

/* 启动自动清理进程的函数，由 postmaster 调用 */
extern void autovac_init(void);
extern int	StartAutoVacLauncher(void);
extern int	StartAutoVacWorker(void);

/* 工作进程无法 fork 时由 postmaster 调用 */
extern void AutoVacWorkerFailed(void);

#ifdef EXEC_BACKEND
extern void AutoVacLauncherMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void AutoVacWorkerMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void AutovacuumWorkerIAm(void);
extern void AutovacuumLauncherIAm(void);
#endif

extern bool AutoVacuumRequestWork(AutoVacuumWorkItemType type,
								  Oid relationId, BlockNumber blkno);

/* 共享内存相关 */
extern Size AutoVacuumShmemSize(void);
extern void AutoVacuumShmemInit(void);

#endif							/* AUTOVACUUM_H */
