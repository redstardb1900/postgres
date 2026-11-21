/*--------------------------------------------------------------------
 * bgworker.h
 *		POSTGRES 可插拔后台工作进程接口
 *
 * 后台工作进程能运行任意用户提供的代码，
 * 包括普通事务。
 *
 * 经 shared_preload_libraries 加载的外部模块可注册
 * 工作进程。也可在运行时动态注册。无论哪种，
 * 工作进程都由 postmaster fork，并运行用户提供的
 * "main" 函数。该代码可连接数据库并跑事务。
 * 工作进程可无限期保持活跃，但关机或崩溃时
 * 会被终止。
 *
 * 若 postmaster 中 fork() 失败，稍后会重试。注意
 * 失败只能是暂时的（高负载、内存压力、进程过多等
 * 导致的 fork 失败）；更持久的问题如无法连接数据库，
 * 在工作进程稍后检测，并以正常退出处理。退出码为
 * 0 的工作进程永不重启，并从工作进程列表移除。
 * 退出码为 1 的将在配置的重启间隔后重启（除非
 * 间隔为 BGW_NEVER_RESTART）。
 * 。
 * 可用 TerminateBackgroundWorker() 终止动态注册的
 * 后台工作进程；会向其发 SIGTERM，退出后不重启。
 * 每当 postmaster 知道某工作进程不会重启时，就
 * 注销它，腾出槽位给新工作进程。
 * 。
 *
 * 注意同一数据库中可能并发有多个工作进程，且同一
 * 模块可请求多个运行相同（或不同）代码的工作进程。
 * 。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/postmaster/bgworker.h
 *--------------------------------------------------------------------
 */
#ifndef BGWORKER_H
#define BGWORKER_H

/*---------------------------------------------------------------------
 * 外部模块 API。
 *---------------------------------------------------------------------
 */

/*
 * 传此标志使工作进程能连接共享内存。
 * 此标志必须。
 */
#define BGWORKER_SHMEM_ACCESS						0x0001

/*
 * 此标志表示 bgworker 需要数据库连接。连接不会
 * 自动建立；工作进程须稍后自行建立。还需要已传
 * BGWORKER_SHMEM_ACCESS。
 */
#define BGWORKER_BACKEND_DATABASE_CONNECTION		0x0002

/*
 * 此类供并行查询内部使用，跟踪活跃并行工作进程
 * 数，确保同时启动的并行工作进程不超过
 * max_parallel_workers。第三方后台工作进程切勿
 * 使用此类。
 */
#define BGWORKER_CLASS_PARALLEL					0x0010
/* 在此添加更多 bgworker 类 */


typedef void (*bgworker_main_type) (Datum main_arg);

/*
 * bgworker 可请求被启动的时间点
 */
typedef enum
{
	BgWorkerStart_PostmasterStart,
	BgWorkerStart_ConsistentState,
	BgWorkerStart_RecoveryFinished
} BgWorkerStartTime;

#define BGW_DEFAULT_RESTART_INTERVAL	60
#define BGW_NEVER_RESTART				-1
#define BGW_MAXLEN						96
#define BGW_EXTRALEN					128

typedef struct BackgroundWorker
{
	char		bgw_name[BGW_MAXLEN];
	char		bgw_type[BGW_MAXLEN];
	int			bgw_flags;
	BgWorkerStartTime bgw_start_time;
	int			bgw_restart_time;	/* 秒，或 BGW_NEVER_RESTART */
	char		bgw_library_name[BGW_MAXLEN];
	char		bgw_function_name[BGW_MAXLEN];
	Datum		bgw_main_arg;
	char		bgw_extra[BGW_EXTRALEN];
	pid_t		bgw_notify_pid; /* 启停时 SIGUSR1 本后端 */
} BackgroundWorker;

typedef enum BgwHandleStatus
{
	BGWH_STARTED,				/* 在运行 */
	BGWH_NOT_YET_STARTED,		/* 工作进程尚未启动 */
	BGWH_STOPPED,				/* 已退出 */
	BGWH_POSTMASTER_DIED		/* postmaster 已死；状态不明 */
} BgwHandleStatus;

struct BackgroundWorkerHandle;
typedef struct BackgroundWorkerHandle BackgroundWorkerHandle;

/* 在 shared_preload_libraries 期间注册新 bgworker */
extern void RegisterBackgroundWorker(BackgroundWorker *worker);

/* 从普通后端注册新 bgworker */
extern bool RegisterDynamicBackgroundWorker(BackgroundWorker *worker,
											BackgroundWorkerHandle **handle);

/* 查询 bgworker 状态 */
extern BgwHandleStatus GetBackgroundWorkerPid(BackgroundWorkerHandle *handle,
											  pid_t *pidp);
extern BgwHandleStatus WaitForBackgroundWorkerStartup(BackgroundWorkerHandle *handle, pid_t *pidp);
extern BgwHandleStatus
			WaitForBackgroundWorkerShutdown(BackgroundWorkerHandle *);
extern const char *GetBackgroundWorkerTypeByPid(pid_t pid);

/* 终止 bgworker */
extern void TerminateBackgroundWorker(BackgroundWorkerHandle *handle);

/* 运行中工作进程内有效 */
extern PGDLLIMPORT BackgroundWorker *MyBgworkerEntry;

/*
 * 以指定用户连接指定数据库。仅注册时传了
 * BGWORKER_BACKEND_DATABASE_CONNECTION 的工作进程
 * 。
 *
 * 若 username 为 NULL，则用引导超级用户。
 * 若 dbname 为 NULL，则不连特定库，只能访问
 * 共享系统目录。
 */
extern void BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags);

/* 同上，但用 OID 指定数据库与用户。 */
extern void BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags);

/*
 * BackgroundWorkerInitializeConnection 等的标志
 *
 *
 * 连接数据库时允许绕过 datallowconn 限制
 */
#define BGWORKER_BYPASS_ALLOWCONN 1


/* 在后台工作进程中阻塞/解除阻塞信号 */
extern void BackgroundWorkerBlockSignals(void);
extern void BackgroundWorkerUnblockSignals(void);

#endif							/* BGWORKER_H */
