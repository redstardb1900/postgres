/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  POSTGRES 进程间通信相关定义。
 *
 * 本文件名已不太贴切：这里几乎不再有与 IPC 直接
 * 相关的内容。此处功能关注的是为 postmaster 或
 * backend 管理退出时的清理工作。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/ipc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IPC_H
#define IPC_H

typedef void (*pg_on_exit_callback) (int code, Datum arg);
typedef void (*shmem_startup_hook_type) (void);

/*----------
 * 用于处理代码块因 ereport(ERROR) 或
 * ereport(FATAL) 退出时必须发生的清理。（典型例子是
 * 撤销对共享内存状态的短暂更改。）
 *
 *		PG_ENSURE_ERROR_CLEANUP(cleanup_function, arg);
 *		{
 *			... 可能抛出 ereport(ERROR) 或 ereport(FATAL) 代码 ...
 *		}
 *		PG_END_ENSURE_ERROR_CLEANUP(cleanup_function, arg);
 *
 * 其中清理代码在按 pg_on_exit_callback 声明的函数中。
 * Datum 值 "arg" 可携带清理函数所需的任何信息。
 * needs.
 *
 * 该构造确保 cleanup_function() 会在 ERROR 或
 * FATAL 退出时被调用。从受控代码成功退出时
 * 不会调用它。（若成功退出时也要调用，
 * 在构造结束后自行调用该函数即可。）
 *
 * 注意：宏参数会被多次求值，故切勿有副作用。
 *----------
 */
#define PG_ENSURE_ERROR_CLEANUP(cleanup_function, arg)	\
	do { \
		before_shmem_exit(cleanup_function, arg); \
		PG_TRY()

#define PG_END_ENSURE_ERROR_CLEANUP(cleanup_function, arg)	\
		cancel_before_shmem_exit(cleanup_function, arg); \
		PG_CATCH(); \
		{ \
			cancel_before_shmem_exit(cleanup_function, arg); \
			cleanup_function (0, arg); \
			PG_RE_THROW(); \
		} \
		PG_END_TRY(); \
	} while (0)


/* ipc.c */
extern PGDLLIMPORT bool proc_exit_inprogress;
extern PGDLLIMPORT bool shmem_exit_inprogress;

extern void proc_exit(int code) pg_attribute_noreturn();
extern void shmem_exit(int code);
extern void on_proc_exit(pg_on_exit_callback function, Datum arg);
extern void on_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void before_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void cancel_before_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void on_exit_reset(void);
extern void check_on_shmem_exit_lists_are_empty(void);

/* ipci.c */
extern PGDLLIMPORT shmem_startup_hook_type shmem_startup_hook;

extern Size CalculateShmemSize(int *num_semaphores);
extern void CreateSharedMemoryAndSemaphores(void);
extern void InitializeShmemGUCs(void);

#endif							/* IPC_H */
