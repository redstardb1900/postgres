/*-------------------------------------------------------------------------
 *
 * pg_sema.h
 *	  信号量的平台无关 API。
 *
 * PostgreSQL 需要计数信号量（跟踪多次 unlock，并允许
 * 随后同等次数的 lock 操作才阻塞）。底层实现
 * 并非各平台相同。本文件定义该 API，必须
 * 由每个移植
 * 提供。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pg_sema.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SEMA_H
#define PG_SEMA_H

/*
 * struct PGSemaphoreData 与指针类型 PGSemaphore 是表示
 * 单个信号量的数据结构。PGSemaphoreData 内容随实现
 * 而异，平台无关代码切勿触碰；故此处把
 * PGSemaphoreData 声明为不透明结构。
 *
 * 然而 Windows 与其他移植差异够大，似乎不值得
 * 对 Windows 也坚持 ABI 兼容。故在该平台上直接
 * 把 PGSemaphore 定义为 HANDLE。
 */
#ifndef USE_WIN32_SEMAPHORES
typedef struct PGSemaphoreData *PGSemaphore;
#else
typedef HANDLE PGSemaphore;
#endif


/* 报告所需共享内存量 */
extern Size PGSemaphoreShmemSize(int maxSemas);

/* 模块初始化（postmaster 启动或 shmem 重初始化时调用） */
extern void PGReserveSemaphores(int maxSemas);

/* 分配初始计数为 1 的 PGSemaphore 结构 */
extern PGSemaphore PGSemaphoreCreate(void);

/* 把先前已初始化的 PGSemaphore 复位为计数 0 */
extern void PGSemaphoreReset(PGSemaphore sema);

/* 锁信号量（递减计数），若计数将 < 0 则阻塞 */
extern void PGSemaphoreLock(PGSemaphore sema);

/* 解锁信号量（递增计数） */
extern void PGSemaphoreUnlock(PGSemaphore sema);

/* 仅当能不阻塞时才锁信号量 */
extern bool PGSemaphoreTryLock(PGSemaphore sema);

#endif							/* PG_SEMA_H */
