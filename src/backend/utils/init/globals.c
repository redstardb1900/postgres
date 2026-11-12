/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  全局变量声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * 注
 *	  到处使用的全局变量应在此声明，
 *	  而非别处。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/file_perm.h"
#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "storage/backendid.h"


ProtocolVersion FrontendProtocol;

volatile sig_atomic_t InterruptPending = false;
volatile sig_atomic_t QueryCancelPending = false;
volatile sig_atomic_t ProcDiePending = false;
volatile sig_atomic_t CheckClientConnectionPending = false;
volatile sig_atomic_t ClientConnectionLost = false;
volatile sig_atomic_t IdleInTransactionSessionTimeoutPending = false;
volatile sig_atomic_t IdleSessionTimeoutPending = false;
volatile sig_atomic_t ProcSignalBarrierPending = false;
volatile sig_atomic_t LogMemoryContextPending = false;
volatile sig_atomic_t IdleStatsUpdateTimeoutPending = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 QueryCancelHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

int			MyProcPid;
pg_time_t	MyStartTime;
TimestampTz MyStartTimestamp;
struct Port *MyProcPort;
int32		MyCancelKey;
int			MyPMChildSlot;

/*
 * MyLatch 指向当前进程应使用的信号处理锁存器。若当
 * 时进程没有 PGPROC 条目，它就指向进程本地锁存器，
 * 否则指向 PGPROC->procLatch。因此它总能在信号处理
 * 处理函数中使用，
 * 无需检查其是否存在。
 */
struct Latch *MyLatch;

/*
 * DataDir 是 PGDATA 目录树顶层的绝对路径。除早期启
 * 动外，这也是服务器的工作目录；因此多数代码可以
 * 直接使用相对路径而不显式引用 DataDir。
 * 
 */
char	   *DataDir = NULL;

/*
 * 数据目录的权限模式。默认为 0700，但如果数据目录
 * 的实际模式如此，checkDataDir() 可将其改为 0750。
 */
int			data_directory_mode = PG_DIR_MODE_OWNER;

char		OutputFileName[MAXPGPATH];	/* 调试输出文件 */

char		my_exec_path[MAXPGPATH];	/* 本可执行文件路径 */
char		pkglib_path[MAXPGPATH]; /* lib 目录的完整路径 */

#ifdef EXEC_BACKEND
char		postgres_exec_path[MAXPGPATH];	/* backend 路径 */

/* 注意：目前在 backend 进程中此值无效 */
#endif

BackendId	MyBackendId = InvalidBackendId;

BackendId	ParallelLeaderBackendId = InvalidBackendId;

Oid			MyDatabaseId = InvalidOid;

Oid			MyDatabaseTableSpace = InvalidOid;

/*
 * DatabasePath 是我数据库主目录的路径（相对于
 * DataDir），即它在默认表空间中的目录。
 */
char	   *DatabasePath = NULL;

pid_t		PostmasterPid = 0;

/*
 * IsPostmasterEnvironment 在 postmaster 进程及任何
 * postmaster 子进程中为真；在独立进程（bootstrap 或
 * 独立 backend）中为假。IsUnderPostmaster 在 postmaster
 * 子进程中为真。注意"子进程"包括所有子进程，而不
 * 只是常规 backend。这些标志应尽早正确设置，
 * 以便初始化期间万一出错，错误处理能做对的事
 * 。
 *
 * bootstrap/独立运行情形下它们按此初始化。
 */
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;
bool		IsBinaryUpgrade = false;
bool		IsBackgroundWorker = false;

bool		ExitOnAnyError = false;

int			DateStyle = USE_ISO_DATES;
int			DateOrder = DATEORDER_MDY;
int			IntervalStyle = INTSTYLE_POSTGRES;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			work_mem = 4096;
double		hash_mem_multiplier = 2.0;
int			maintenance_work_mem = 65536;
int			max_parallel_maintenance_workers = 2;

/*
 * 共享内存结构大小的主要决定因素。
 *
 * MaxBackends 由 PostmasterMain 在各模块注册后台
 * 工作者之后计算。
 */
int			NBuffers = 16384;
int			MaxConnections = 100;
int			max_worker_processes = 8;
int			max_parallel_workers = 8;
int			MaxBackends = 0;

/* vacuum 的 GUC 参数 */
int			VacuumBufferUsageLimit = 256;

int			VacuumCostPageHit = 1;
int			VacuumCostPageMiss = 2;
int			VacuumCostPageDirty = 20;
int			VacuumCostLimit = 200;
double		VacuumCostDelay = 0;

int64		VacuumPageHit = 0;
int64		VacuumPageMiss = 0;
int64		VacuumPageDirty = 0;

int			VacuumCostBalance = 0;	/* vacuum 的工作状态 */
bool		VacuumCostActive = false;
