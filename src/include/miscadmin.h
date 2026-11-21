/*-------------------------------------------------------------------------
 *
 * miscadmin.h
 *	  一般 postgres 管理与初始化内容，
 *	  原先分散在下列文件中：
 *		globals.h						全局变量
 *		pdir.h							目录路径相关
 *		pinit.h							postgres 初始化
 *		pmod.h							处理模式
 *	  久而久之，这里也成了广为人知的
 *	资源限制首选处，如 work_mem、check_stack_depth()。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/miscadmin.h
 *
 * NOTES
 *	  本文件中某些信息应迁到其他文件。
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H

#include <signal.h>

#include "datatype/timestamp.h" /* for TimestampTz */
#include "pgtime.h"				/* for pg_time_t */


#define InvalidPid				(-1)


/*****************************************************************************
 *	  系统中断与临界区处理
 *
 * 运行中的后端须在不弄乱状态的前提下接受两类中断：
 * QueryCancel (SIGINT) 与 ProcDie (SIGTERM)。
 * 两种情况下都须能优雅清理当前事务，
 * 故不能瞬时响应中断——
 * 无法保证在任意瞬间打断时内部数据结构
 * 自洽。取而代之，信号
 * 处理程序设置标志，在执行中周期性检查。
 *
 * CHECK_FOR_INTERRUPTS() 宏在策略性位置调用，
 * 通常在那里可安全接受取消或死亡中断。有时
 * 在低层子例程内调用 CHECK_FOR_INTERRUPTS()，而
 * 那些上下文*不*希望允许取消
 * 或死亡中断。HOLD_INTERRUPTS() 与 RESUME_INTERRUPTS() 宏
 * 可确保即使子例程调用了 CHECK_FOR_INTERRUPTS()
 * 也不接受取消或死亡中断。中断
 * 将推迟到在任何 HOLD_INTERRUPTS()...
 * RESUME_INTERRUPTS() 段之外做完检查。
 *
 * 另有机制可阻止查询取消中断，仍
 * 允许死亡中断：HOLD_CANCEL_INTERRUPTS() 与
 * RESUME_CANCEL_INTERRUPTS().
 *
 * 注意 ProcessInterrupts() 还承担若干未必
 * 导致查询取消或死亡响应的任务。故
 * 可能只是清除 InterruptPending 就返回。
 *
 * 可用 INTERRUPTS_PENDING_CONDITION() 检查是否有
 * 中断待服务，而不立即尝试服务。
 * 有些调用方也关心 INTERRUPTS_CAN_BE_PROCESSED()，
 * 它告知 ProcessInterrupts 是否必能清除该中断。
 *
 * 另有特殊机制使在等待锁或等待命令输入时
 * 可接受中断（当然，仅当
 * 中断推迟计数器为零时）。详见
 * 相关代码。
 *
 * 连接丢失的处理类似，尽管丢连接
 * 不产生信号，而是在向套接字写
 * 失败时检出。若有断连信号，可在
 * 处理程序中设 ClientConnectionLost。
 *
 * 另有概念上不同但相关的"临界区"
 * 机制。临界区不仅推迟取消/死亡中断，
 * 且使 ereport(ERROR) 或 ereport(FATAL) 变成 ereport(PANIC)
 * ——即强制系统范围重置。不言而喻，只有真正
 * *关键*的代码才应标为临界区！当前此
 * 机制仅用于与 XLOG 相关的代码。
 *
 *****************************************************************************/

/* in globals.c */
/* these are marked volatile because they are set by signal handlers: */
extern PGDLLIMPORT volatile sig_atomic_t InterruptPending;
extern PGDLLIMPORT volatile sig_atomic_t QueryCancelPending;
extern PGDLLIMPORT volatile sig_atomic_t ProcDiePending;
extern PGDLLIMPORT volatile sig_atomic_t IdleInTransactionSessionTimeoutPending;
extern PGDLLIMPORT volatile sig_atomic_t IdleSessionTimeoutPending;
extern PGDLLIMPORT volatile sig_atomic_t ProcSignalBarrierPending;
extern PGDLLIMPORT volatile sig_atomic_t LogMemoryContextPending;
extern PGDLLIMPORT volatile sig_atomic_t IdleStatsUpdateTimeoutPending;

extern PGDLLIMPORT volatile sig_atomic_t CheckClientConnectionPending;
extern PGDLLIMPORT volatile sig_atomic_t ClientConnectionLost;

/* these are marked volatile because they are examined by signal handlers: */
extern PGDLLIMPORT volatile uint32 InterruptHoldoffCount;
extern PGDLLIMPORT volatile uint32 QueryCancelHoldoffCount;
extern PGDLLIMPORT volatile uint32 CritSectionCount;

/* in tcop/postgres.c */
extern void ProcessInterrupts(void);

/* Test whether an interrupt is pending */
#ifndef WIN32
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(InterruptPending))
#else
#define INTERRUPTS_PENDING_CONDITION() \
	(unlikely(UNBLOCKED_SIGNAL_QUEUE()) ? \
	 pgwin32_dispatch_queued_signals() : (void) 0, \
	 unlikely(InterruptPending))
#endif

/* Service interrupt, if one is pending and it's safe to service it now */
#define CHECK_FOR_INTERRUPTS() \
do { \
	if (INTERRUPTS_PENDING_CONDITION()) \
		ProcessInterrupts(); \
} while(0)

/* Is ProcessInterrupts() guaranteed to clear InterruptPending? */
#define INTERRUPTS_CAN_BE_PROCESSED() \
	(InterruptHoldoffCount == 0 && CritSectionCount == 0 && \
	 QueryCancelHoldoffCount == 0)

#define HOLD_INTERRUPTS()  (InterruptHoldoffCount++)

#define RESUME_INTERRUPTS() \
do { \
	Assert(InterruptHoldoffCount > 0); \
	InterruptHoldoffCount--; \
} while(0)

#define HOLD_CANCEL_INTERRUPTS()  (QueryCancelHoldoffCount++)

#define RESUME_CANCEL_INTERRUPTS() \
do { \
	Assert(QueryCancelHoldoffCount > 0); \
	QueryCancelHoldoffCount--; \
} while(0)

#define START_CRIT_SECTION()  (CritSectionCount++)

#define END_CRIT_SECTION() \
do { \
	Assert(CritSectionCount > 0); \
	CritSectionCount--; \
} while(0)


/*****************************************************************************
 *	  globals.h --															 *
 *****************************************************************************/

/*
 * from utils/init/globals.c
 */
extern PGDLLIMPORT pid_t PostmasterPid;
extern PGDLLIMPORT bool IsPostmasterEnvironment;
extern PGDLLIMPORT bool IsUnderPostmaster;
extern PGDLLIMPORT bool IsBackgroundWorker;
extern PGDLLIMPORT bool IsBinaryUpgrade;

extern PGDLLIMPORT bool ExitOnAnyError;

extern PGDLLIMPORT char *DataDir;
extern PGDLLIMPORT int data_directory_mode;

extern PGDLLIMPORT int NBuffers;
extern PGDLLIMPORT int MaxBackends;
extern PGDLLIMPORT int MaxConnections;
extern PGDLLIMPORT int max_worker_processes;
extern PGDLLIMPORT int max_parallel_workers;

extern PGDLLIMPORT int MyProcPid;
extern PGDLLIMPORT pg_time_t MyStartTime;
extern PGDLLIMPORT TimestampTz MyStartTimestamp;
extern PGDLLIMPORT struct Port *MyProcPort;
extern PGDLLIMPORT struct Latch *MyLatch;
extern PGDLLIMPORT int32 MyCancelKey;
extern PGDLLIMPORT int MyPMChildSlot;

extern PGDLLIMPORT char OutputFileName[];
extern PGDLLIMPORT char my_exec_path[];
extern PGDLLIMPORT char pkglib_path[];

#ifdef EXEC_BACKEND
extern PGDLLIMPORT char postgres_exec_path[];
#endif

/*
 * 暂在 storage/backendid.h。
 *
 * extern BackendId    MyBackendId;
 */
extern PGDLLIMPORT Oid MyDatabaseId;

extern PGDLLIMPORT Oid MyDatabaseTableSpace;

/*
 * 日期/时间配置
 *
 * DateStyle 定义日期/时间类型的输出格式选择：
 *	USE_POSTGRES_DATES 指定传统 Postgres 格式
 *	USE_ISO_DATES 指定 ISO 兼容格式
 *	USE_SQL_DATES 指定 Oracle/Ingres 兼容格式
 *	USE_GERMAN_DATES 指定德式 dd.mm/yyyy
 *
 * DateOrder 定义读取歧义日期时假定的字段顺序
 * （非 YYYY-MM-DD、且非四位
 * 年字段在前）皆视为歧义）：
 *	DATEORDER_YMD 指定字段序 yy-mm-dd
 *	DATEORDER_DMY 指定字段序 dd-mm-yy（"欧洲"惯例）
 *	DATEORDER_MDY 指定字段序 mm-dd-yy（"美国"惯例）
 *
 * 在 Postgres 与 SQL DateStyles 下，DateOrder 也选择输出字段
 * 顺序：DMY 风格日在月前，否则月在日前。
 *
 * 用户可见的 "DateStyle" 运行时参数涵盖上述两者。
 */

/*  合法 DateStyle 值  */
#define USE_POSTGRES_DATES		0
#define USE_ISO_DATES			1
#define USE_SQL_DATES			2
#define USE_GERMAN_DATES		3
#define USE_XSD_DATES			4

/*  合法 DateOrder 值  */
#define DATEORDER_YMD			0
#define DATEORDER_DMY			1
#define DATEORDER_MDY			2

extern PGDLLIMPORT int DateStyle;
extern PGDLLIMPORT int DateOrder;

/*
 * IntervalStyles
 *	 INTSTYLE_POSTGRES			   似 PG<8.4 且 DateStyle='iso'
 *	 INTSTYLE_POSTGRES_VERBOSE	   似 PG<8.4 且 DateStyle!='iso'
 *	 INTSTYLE_SQL_STANDARD		   SQL 标准区间字面量
 *	 INTSTYLE_ISO_8601			   ISO-8601 基本格式区间
 */
#define INTSTYLE_POSTGRES			0
#define INTSTYLE_POSTGRES_VERBOSE	1
#define INTSTYLE_SQL_STANDARD		2
#define INTSTYLE_ISO_8601			3

extern PGDLLIMPORT int IntervalStyle;

#define MAXTZLEN		10		/*  时区名最大长(不含空)  */

extern PGDLLIMPORT bool enableFsync;
extern PGDLLIMPORT bool allowSystemTableMods;
extern PGDLLIMPORT int work_mem;
extern PGDLLIMPORT double hash_mem_multiplier;
extern PGDLLIMPORT int maintenance_work_mem;
extern PGDLLIMPORT int max_parallel_maintenance_workers;

/*
 * VacuumBufferUsageLimit GUC 与 BUFFER_USAGE_LIMIT 选项所指定的
 * 缓冲访问策略环大小的上下硬限制，
 * 供 VACUUM/ANALYZE。
 */
#define MIN_BAS_VAC_RING_SIZE_KB 128
#define MAX_BAS_VAC_RING_SIZE_KB (16 * 1024 * 1024)

extern PGDLLIMPORT int VacuumBufferUsageLimit;
extern PGDLLIMPORT int VacuumCostPageHit;
extern PGDLLIMPORT int VacuumCostPageMiss;
extern PGDLLIMPORT int VacuumCostPageDirty;
extern PGDLLIMPORT int VacuumCostLimit;
extern PGDLLIMPORT double VacuumCostDelay;

extern PGDLLIMPORT int64 VacuumPageHit;
extern PGDLLIMPORT int64 VacuumPageMiss;
extern PGDLLIMPORT int64 VacuumPageDirty;

extern PGDLLIMPORT int VacuumCostBalance;
extern PGDLLIMPORT bool VacuumCostActive;


/* in tcop/postgres.c */

typedef char *pg_stack_base_t;

extern pg_stack_base_t set_stack_base(void);
extern void restore_stack_base(pg_stack_base_t base);
extern void check_stack_depth(void);
extern bool stack_is_too_deep(void);

/* in tcop/utility.c */
extern void PreventCommandIfReadOnly(const char *cmdname);
extern void PreventCommandIfParallelMode(const char *cmdname);
extern void PreventCommandDuringRecovery(const char *cmdname);

/* in utils/misc/guc_tables.c */
extern PGDLLIMPORT int trace_recovery_messages;
extern int	trace_recovery(int trace_level);

/*****************************************************************************
 *	  pdir.h --
 *			POSTGRES 目录路径定义。
 *****************************************************************************/

/*  组成 sec_context 的 OR 标志  */
#define SECURITY_LOCAL_USERID_CHANGE	0x0001
#define SECURITY_RESTRICTED_OPERATION	0x0002
#define SECURITY_NOFORCE_RLS			0x0004

extern PGDLLIMPORT char *DatabasePath;

/* now in utils/init/miscinit.c */
extern void InitPostmasterChild(void);
extern void InitStandaloneProcess(const char *argv0);
extern void InitProcessLocalLatch(void);
extern void SwitchToSharedLatch(void);
extern void SwitchBackToLocalLatch(void);

typedef enum BackendType
{
	B_INVALID = 0,
	B_ARCHIVER,
	B_AUTOVAC_LAUNCHER,
	B_AUTOVAC_WORKER,
	B_BACKEND,
	B_BG_WORKER,
	B_BG_WRITER,
	B_CHECKPOINTER,
	B_LOGGER,
	B_STANDALONE_BACKEND,
	B_STARTUP,
	B_WAL_RECEIVER,
	B_WAL_SENDER,
	B_WAL_WRITER,
} BackendType;

#define BACKEND_NUM_TYPES (B_WAL_WRITER + 1)

extern PGDLLIMPORT BackendType MyBackendType;

#define AmRegularBackendProcess()	(MyBackendType == B_BACKEND)

extern const char *GetBackendTypeDesc(BackendType backendType);

extern void SetDatabasePath(const char *path);
extern void checkDataDir(void);
extern void SetDataDir(const char *dir);
extern void ChangeToDataDir(void);

extern char *GetUserNameFromId(Oid roleid, bool noerr);
extern Oid	GetUserId(void);
extern Oid	GetOuterUserId(void);
extern Oid	GetSessionUserId(void);
extern bool GetSessionUserIsSuperuser(void);
extern Oid	GetAuthenticatedUserId(void);
extern bool GetAuthenticatedUserIsSuperuser(void);
extern void SetAuthenticatedUserId(Oid userid, bool is_superuser);
extern void GetUserIdAndSecContext(Oid *userid, int *sec_context);
extern void SetUserIdAndSecContext(Oid userid, int sec_context);
extern bool InLocalUserIdChange(void);
extern bool InSecurityRestrictedOperation(void);
extern bool InNoForceRLSOperation(void);
extern void GetUserIdAndContext(Oid *userid, bool *sec_def_context);
extern void SetUserIdAndContext(Oid userid, bool sec_def_context);
extern void InitializeSessionUserId(const char *rolename, Oid roleid);
extern void InitializeSessionUserIdStandalone(void);
extern void SetSessionAuthorization(Oid userid, bool is_superuser);
extern Oid	GetCurrentRoleId(void);
extern void SetCurrentRoleId(Oid roleid, bool is_superuser);
extern void InitializeSystemUser(const char *authn_id,
								 const char *auth_method);
extern const char *GetSystemUser(void);

/* in utils/misc/superuser.c */
extern bool superuser(void);	/*  当前为超级用户  */
extern bool superuser_arg(Oid roleid);	/* given user is superuser */


/*****************************************************************************
 *	  pmod.h --
 *			POSTGRES 处理模式定义。
 *****************************************************************************/

/*
 * 说明：
 *		POSTGRES 有三种处理模式。它们是
 * BootstrapProcessing（"引导"）、InitProcessing（
 * "初始化"）与 NormalProcessing（"正常"）。
 *
 * 前两种用于特殊时期。当系统
 * 状态表明引导处理时，事务皆被赋予
 * 事务号"一"，因而保证提交。此模式
 * 用于模板库的初始生成。
 *
 * 初始化模式：用于启动后端期间，直至所有正常
 * 初始化完成。某些代码在此模式执行时
 * 行为不同，以便系统引导。
 *
 * 若 POSTGRES 后端进程处于正常模式，则所有代码可
 * 正常执行。
 */

typedef enum ProcessingMode
{
	BootstrapProcessing,		/*  引导创建模板库  */
	InitProcessing,				/* initializing system */
	NormalProcessing			/*  正常处理  */
} ProcessingMode;

extern PGDLLIMPORT ProcessingMode Mode;

#define IsBootstrapProcessingMode() (Mode == BootstrapProcessing)
#define IsInitProcessingMode()		(Mode == InitProcessing)
#define IsNormalProcessingMode()	(Mode == NormalProcessing)

#define GetProcessingMode() Mode

#define SetProcessingMode(mode) \
	do { \
		Assert((mode) == BootstrapProcessing || \
				  (mode) == InitProcessing || \
				  (mode) == NormalProcessing); \
		Mode = (mode); \
	} while(0)


/*
 * 辅助进程类型标识符。原在 bootstrap.h，
 * 但放在此处与 ProcessingMode 一起更合理。
 * MyAuxProcType 全局在 auxprocess.c 中定义与设置。
 *
 * 在此添加的项务必列入术语表。
 */

typedef enum
{
	NotAnAuxProcess = -1,
	StartupProcess = 0,
	BgWriterProcess,
	ArchiverProcess,
	CheckpointerProcess,
	WalWriterProcess,
	WalReceiverProcess,

	NUM_AUXPROCTYPES			/* Must be last! */
} AuxProcType;

extern PGDLLIMPORT AuxProcType MyAuxProcType;

#define AmStartupProcess()			(MyAuxProcType == StartupProcess)
#define AmBackgroundWriterProcess() (MyAuxProcType == BgWriterProcess)
#define AmArchiverProcess()			(MyAuxProcType == ArchiverProcess)
#define AmCheckpointerProcess()		(MyAuxProcType == CheckpointerProcess)
#define AmWalWriterProcess()		(MyAuxProcType == WalWriterProcess)
#define AmWalReceiverProcess()		(MyAuxProcType == WalReceiverProcess)


/*****************************************************************************
 *	  pinit.h --
 *			POSTGRES 初始化与清理定义。
 *****************************************************************************/

/* in utils/init/postinit.c */
extern void pg_split_opts(char **argv, int *argcp, const char *optstr);
extern void InitializeMaxBackends(void);
extern void InitPostgres(const char *in_dbname, Oid dboid,
						 const char *username, Oid useroid,
						 bool load_session_libraries,
						 bool override_allow_connections,
						 char *out_dbname);
extern void BaseInit(void);

/* in utils/init/miscinit.c */
extern PGDLLIMPORT bool IgnoreSystemIndexes;
extern PGDLLIMPORT bool process_shared_preload_libraries_in_progress;
extern PGDLLIMPORT bool process_shared_preload_libraries_done;
extern PGDLLIMPORT bool process_shmem_requests_in_progress;
extern PGDLLIMPORT char *session_preload_libraries_string;
extern PGDLLIMPORT char *shared_preload_libraries_string;
extern PGDLLIMPORT char *local_preload_libraries_string;

extern void CreateDataDirLockFile(bool amPostmaster);
extern void CreateSocketLockFile(const char *socketfile, bool amPostmaster,
								 const char *socketDir);
extern void TouchSocketLockFiles(void);
extern void AddToDataDirLockFile(int target_line, const char *str);
extern bool RecheckDataDirLockFile(void);
extern void ValidatePgVersion(const char *path);
extern void process_shared_preload_libraries(void);
extern void process_session_preload_libraries(void);
extern void process_shmem_requests(void);
extern void pg_bindtextdomain(const char *domain);
extern bool has_rolreplication(Oid roleid);

typedef void (*shmem_request_hook_type) (void);
extern PGDLLIMPORT shmem_request_hook_type shmem_request_hook;

extern Size EstimateClientConnectionInfoSpace(void);
extern void SerializeClientConnectionInfo(Size maxsize, char *start_address);
extern void RestoreClientConnectionInfo(char *conninfo);

/* in executor/nodeHash.c */
extern size_t get_hash_memory_limit(void);

#endif							/* MISCADMIN_H */
