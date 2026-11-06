/*-------------------------------------------------------------------------
 *
 * autovacuum.c
 *
 * PostgreSQL 集成式自动清理进程 
 *
 * 自动清理系统由两种不同的进程构成：自动清理 launcher    
 * 与自动清理 worker。launcher 是一个常驻进程，在     
 * 设置了 autovacuum GUC 参数后由 postmaster 启动。launcher       
 * 会在恰当时机调度启动自动清理 worker。真正执行清理    
 * 工作的是 worker 进程：它们按 launcher 的决定连接到某个 
 * 数据库；连接后即检查目录，从中选出要清理的表。   
 *                                                                         
 *
 * 自动清理 launcher 无法自行启动 worker 进程，因为     
 * 会带来健壮性问题（即异常情况下无法将它们关闭，  
 * 而且 launcher 连接着共享内存，可能受共享内存损坏 
 * 影响，不如 postmaster 健壮）。所以它把这项任务留给    
 * postmaster 完成。                                                        
 *
 * 有个自动清理共享内存区，launcher 在其中存放它想  
 * 让其被清理的数据库的信息。当它想启动一个新 worker
 * 时，它会在共享内存中设置一个标志，并向 postmaster      
 * 发送信号。此时 postmaster 只知道自己必须启动一个          
 * worker；于是它 fork 出一个新的子进程，使之变成      
 * worker。新进程连接共享内存，即可在其中查看 launcher 所设
 * 的信息。        
 *
 * 如果 postmaster 中的 fork() 调用失败，它会在共享内       
 * 存区中设一个标志，并向 launcher 发送信号。launcher  
 * 注意到该标志后，便会重发信号，再次尝试启动该   
 * worker。注意失败只能是暂态的（负载过高、内存紧张、 
 * 进程过多等）；更持久的问题如连接数据库失败，  
 * 会在稍后的 worker 中被检测到，处理办法就是让 worker     
 * 正常退出。launcher 会在稍后按调度安排再启动一个          
 * 新 worker。                                      
 *
 * worker 完成清理后会向 launcher 发送 SIGUSR2。随后           
 * launcher 会被唤醒，若调度排得太紧、需要立即启动新       
 * worker 时便能立刻启动它。此时 launcher 还可以重新     
 * 平衡其余各个 worker 的基于代价的清理延迟                 
 * 特性设置。                 
 *
 * 注意同一个数据库中可以有多个 worker 并发运行。      
 * 它们会把当前正在清理的表存入共享内存，这样其他      
 * worker 就不必在等待该表的清理锁时被阻塞。此外，        
 * 它们还会在清理每张表之前，从 pgstats 获取该表上    
 * 次被清理的时间，以免清理一张刚被另一个 worker 清     
 * 理完、已不再记录于共享内存中的表。但由于尚未持   
 * 有关系锁，存在一个小窗口，其间 worker 可能选中一     
 * 张已被清理过的表；这是当前设计中的一个缺陷。          
 *                                                       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/autovacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "lib/ilist.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"


/*
 * GUC 参数    
 */
bool		autovacuum_start_daemon = false;
int			autovacuum_max_workers;
int			autovacuum_work_mem = -1;
int			autovacuum_naptime;
int			autovacuum_vac_thresh;
double		autovacuum_vac_scale;
int			autovacuum_vac_ins_thresh;
double		autovacuum_vac_ins_scale;
int			autovacuum_anl_thresh;
double		autovacuum_anl_scale;
int			autovacuum_freeze_max_age;
int			autovacuum_multixact_freeze_max_age;

double		autovacuum_vac_cost_delay;
int			autovacuum_vac_cost_limit;

int			Log_autovacuum_min_duration = 600000;

/* launcher 两次唤醒之间允许的最短时间                */
#define MIN_AUTOVAC_SLEEPTIME 100.0 /* 毫秒       */
#define MAX_AUTOVAC_SLEEPTIME 300	/* 秒     */

/* 用于指示我们是否处于自动清理进程 */
static bool am_autovacuum_launcher = false;
static bool am_autovacuum_worker = false;

/*
 * 用于保存代价相关存储参数的变量，对象是本 worker  
 * 当前正在清理的关系。利用它们即可在重新加载       
 * 配置文件后不覆盖 vacuum_cost_delay 和               
 * vacuum_cost_limit 的值。它们被初始化为"invalid"         
 * 值，表示未指定任何代价相关存储参数；随后会由      
 * do_autovacuum() 在检查过 table_recheck_autovac()                       
 * 中的存储参数后设置。                    
 */
static double av_storage_param_cost_delay = -1;
static int	av_storage_param_cost_limit = -1;

/* 信号处理器所设标志  */
static volatile sig_atomic_t got_SIGUSR2 = false;

/* 用于判断是否超出 freeze_max_age 的比较基准点           */
static TransactionId recentXid;
static MultiXactId recentMulti;

/* 自动清理所用的默认冻结期限（因数据库而异）*/
static int	default_freeze_min_age;
static int	default_freeze_table_age;
static int	default_multixact_freeze_min_age;
static int	default_multixact_freeze_table_age;

/* 长寿命数据的内存上下文  */
static MemoryContext AutovacMemCxt;

/* launcher 用于跟踪数据库的 struct      */
typedef struct avl_dbase
{
	Oid			adl_datid;		/* 哈希键，须居首位  */
	TimestampTz adl_next_worker;
	int			adl_score;
	dlist_node	adl_node;
} avl_dbase;

/* worker 用于跟踪数据库的 struct      */
typedef struct avw_dbase
{
	Oid			adw_datid;
	char	   *adw_name;
	TransactionId adw_frozenxid;
	MultiXactId adw_minmulti;
	PgStat_StatDBEntry *adw_entry;
} avw_dbase;

/* 用于跟踪第一轮中待清理和/或分析的表的 struct       */
typedef struct av_relation
{
	Oid			ar_toastrelid;	/* 哈希键，须居首位 */
	Oid			ar_relid;
	bool		ar_hasrelopts;
	AutoVacOpts ar_reloptions;	/* copy of AutoVacOpts from the main table's
								 * reloptions, or NULL if none */
} av_relation;

/* 经复核后用于跟踪待清理和/或分析的表的 struct            */
typedef struct autovac_table
{
	Oid			at_relid;
	VacuumParams at_params;
	double		at_storage_param_vac_cost_delay;
	int			at_storage_param_vac_cost_limit;
	bool		at_dobalance;
	bool		at_sharedrel;
	char	   *at_relname;
	char	   *at_nspname;
	char	   *at_datname;
} autovac_table;

/*-------------
 * 该 struct 记录单个 worker 的行踪信息。我们在共享内存     
 * 中维护这类 struct 的数组，其大小依据    
 * autovacuum_max_workers 
 *
 * wi_links		空闲链表或运行链表条目   
 * wi_dboid		该 worker 应处理的数据库的 OID               
 * wi_tableoid	当前正在清理的表的 OID（如有）      
 * wi_sharedrel 标志，指示表是否标记为 relisshared      
 * wi_proc		运行中 worker 的 PGPROC 指针；未启动为 NULL      
 * wi_launchtime 此 worker 被启动的时刻         
 * wi_dobalance 本 worker 是否应被纳入平衡计算                     
 *
 * 除 wi_tableoid 和 wi_sharedrel 由 AutovacuumScheduleLock           
 * 保护之外，其余字段均由 AutovacuumLock 保护（注意      
 * 这两个字段对除该 worker 自身外的所有人只读）。
 *-------------
 */
typedef struct WorkerInfoData
{
	dlist_node	wi_links;
	Oid			wi_dboid;
	Oid			wi_tableoid;
	PGPROC	   *wi_proc;
	TimestampTz wi_launchtime;
	pg_atomic_flag wi_dobalance;
	bool		wi_sharedrel;
} WorkerInfoData;

typedef struct WorkerInfoData *WorkerInfo;

/*
 * launcher 可能从远程进程收到的信号。它们以原子方式      
 * 存放在共享内存中，其他进程无需加锁即可设置。     
 *                 
 */
typedef enum
{
	AutoVacForkFailed,			/* 尝试启动 worker 失败      */
	AutoVacRebalance,			/* 重新平衡代价限制  */
	AutoVacNumSignals			/* 必须居末 */
}			AutoVacuumSignal;

/*
 * 自动清理工作项数组，存于 AutoVacuumShmem->av_workItems。    
 * 此列表大体上由 AutovacuumLock 保护；但若某项被标记  
 * 为"active"，则其他进程不得修改标识该工作的成员。
 *         
 */
typedef struct AutoVacuumWorkItem
{
	AutoVacuumWorkItemType avw_type;
	bool		avw_used;		/* 以下数据有效  */
	bool		avw_active;		/* 处理中       */
	Oid			avw_database;
	Oid			avw_relation;
	BlockNumber avw_blockNumber;
} AutoVacuumWorkItem;

#define NUM_WORKITEMS	256

/*-------------
 * 自动清理主共享内存 struct。共享内存中存放这个主   
 * struct 与 WorkerInfo struct 数组。该 struct 保存：     
 *
 * av_signal		由其他进程设置，指示各种状态           
 * av_launcherpid	自动清理 launcher 的 PID     
 * av_freeWorkers	WorkerInfo 空闲链表
 * av_runningWorkers WorkerInfo 非空闲队列   
 * av_startingWorker 指向当前正在启动的 WorkerInfo 的指针（由   
 *					worker 自己在其就绪运行后清除）      
 * av_workItems		工作项数组
 * av_nworkersForBalance 计算每 worker 代价限制时使用的   
 * 					自动清理 worker 数目           
 *
 * 该 struct 由 AutovacuumLock 保护，av_signal 及 worker 链表的     
 * 部分除外（见上文）。 
 *-------------
 */
typedef struct
{
	sig_atomic_t av_signal[AutoVacNumSignals];
	pid_t		av_launcherpid;
	dlist_head	av_freeWorkers;
	dlist_head	av_runningWorkers;
	WorkerInfo	av_startingWorker;
	AutoVacuumWorkItem av_workItems[NUM_WORKITEMS];
	pg_atomic_uint32 av_nworkersForBalance;
} AutoVacuumShmemStruct;

static AutoVacuumShmemStruct *AutoVacuumShmem;

/*
 * launcher 中的数据库列表（avl_dbase 元素），以及容纳       
 * 它的上下文 
 */
static dlist_head DatabaseList = DLIST_STATIC_INIT(DatabaseList);
static MemoryContext DatabaseListCxt = NULL;

/* 本进程的 WorkerInfo 指针，worker 内有效  */
static WorkerInfo MyWorkerInfo = NULL;

/* launcher 的 PID，仅在 worker 关闭期间有效       */
int			AutovacuumLauncherPid = 0;

#ifdef EXEC_BACKEND
static pid_t avlauncher_forkexec(void);
static pid_t avworker_forkexec(void);
#endif
NON_EXEC_STATIC void AutoVacWorkerMain(int argc, char *argv[]) pg_attribute_noreturn();
NON_EXEC_STATIC void AutoVacLauncherMain(int argc, char *argv[]) pg_attribute_noreturn();

static Oid	do_start_worker(void);
static void HandleAutoVacLauncherInterrupts(void);
static void AutoVacLauncherShutdown(void) pg_attribute_noreturn();
static void launcher_determine_sleep(bool canlaunch, bool recursing,
									 struct timeval *nap);
static void launch_worker(TimestampTz now);
static List *get_database_list(void);
static void rebuild_database_list(Oid newdb);
static int	db_comparator(const void *a, const void *b);
static void autovac_recalculate_workers_for_balance(void);

static void do_autovacuum(void);
static void FreeWorkerInfo(int code, Datum arg);

static autovac_table *table_recheck_autovac(Oid relid, HTAB *table_toast_map,
											TupleDesc pg_class_desc,
											int effective_multixact_freeze_max_age);
static void recheck_relation_needs_vacanalyze(Oid relid, AutoVacOpts *avopts,
											  Form_pg_class classForm,
											  int effective_multixact_freeze_max_age,
											  bool *dovacuum, bool *doanalyze, bool *wraparound);
static void relation_needs_vacanalyze(Oid relid, AutoVacOpts *relopts,
									  Form_pg_class classForm,
									  PgStat_StatTabEntry *tabentry,
									  int effective_multixact_freeze_max_age,
									  bool *dovacuum, bool *doanalyze, bool *wraparound);

static void autovacuum_do_vac_analyze(autovac_table *tab,
									  BufferAccessStrategy bstrategy);
static AutoVacOpts *extract_autovac_opts(HeapTuple tup,
										 TupleDesc pg_class_desc);
static void perform_work_item(AutoVacuumWorkItem *workitem);
static void autovac_report_activity(autovac_table *tab);
static void autovac_report_workitem(AutoVacuumWorkItem *workitem,
									const char *nspname, const char *relname);
static void avl_sigusr2_handler(SIGNAL_ARGS);



/********************************************************************
 *					  AUTOVACUUM LAUNCHER CODE
 ********************************************************************/

#ifdef EXEC_BACKEND
/*
 * 自动清理 launcher 进程的 forkexec 例程。   
 *
 * 填好参数表，然后 fork 并 exec。 
 */
static pid_t
avlauncher_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkavlauncher";
	av[ac++] = NULL;			/* 由 postmaster_forkexec 填充   */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * 需在调用 InitProcess 之前从外部设置此值           
 */
void
AutovacuumLauncherIAm(void)
{
	am_autovacuum_launcher = true;
}
#endif

/*
 * 自动清理 launcher 进程的主入口点，需由 postmaster        
 * 调用。  
 */
int
StartAutoVacLauncher(void)
{
	pid_t		AutoVacPID;

#ifdef EXEC_BACKEND
	switch ((AutoVacPID = avlauncher_forkexec()))
#else
	switch ((AutoVacPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork autovacuum launcher process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* postmaster 子进程    */
			InitPostmasterChild();

			/* 关闭 postmaster 的套接字 */
			ClosePostmasterPorts(false);

			AutoVacLauncherMain(0, NULL);
			break;
#endif
		default:
			return (int) AutoVacPID;
	}

	/* 不应到达此处 */
	return 0;
}

/*
 * 自动清理 launcher 进程的主循环。   
 */
NON_EXEC_STATIC void
AutoVacLauncherMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	am_autovacuum_launcher = true;

	MyBackendType = B_AUTOVAC_LAUNCHER;
	init_ps_display(NULL);

	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher started")));

	if (PostAuthDelay)
		pg_usleep(PostAuthDelay * 1000000L);

	SetProcessingMode(InitProcessing);

	/*
	 * 设置信号处理器。我们操作数据库的方式与常规     
	 * 后端相近，因此使用相同的信号处理。等效代码见  
	 * tcop/postgres.c 
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */

	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, avl_sigusr2_handler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 在共享内存中创建每个后端的 PGPROC struct；但       
	 * EXEC_BACKEND 情况下例外，那已在 SubPostmasterMain            
	 * 中完成。必须先做这一步才能使用 LWLocks（当然         
	 * EXEC_BACKEND 下已用过）。    
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/* 早期初始化      */
	BaseInit();

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, false, false, NULL);

	SetProcessingMode(NormalProcessing);

	/*
	 * 创建一个我们将在其中完成所有工作的内存上下文。  
	 * 这样做是为了在错误恢复期间重置该上下文，从而   
	 * 避免内存泄漏。 
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum Launcher",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/*
	 * 若遇到异常，处理将在此处恢复。           
	 *
	 * 这段代码是 PostgresMain 错误恢复逻辑的精简版。      
	 *
	 * 注意我们使用 sigsetjmp(..., 1)，这样在 longjmp 回到这里 
	 * 时会恢复原先的信号掩码（即 BlockSig）。因此在     
	 * 完成错误恢复前，SIGQUIT 以外的信号都被阻塞。    
	 * 看上去这一策略使 HOLD_INTERRUPTS() 调用显得多余，     
	 * 其实不然，因为 InterruptPending 可能已被置位。     
	 *         
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* 未使用 PG_TRY，必须手动重置错误栈         */
		error_context_stack = NULL;

		/* 清理期间阻止中断发生        */
		HOLD_INTERRUPTS();

		/* 忘掉所有未决的 QueryCancel 或超时请求 */
		disable_all_timeouts(false);
		QueryCancelPending = false; /* 第二执行以免竞态条件 */

		/* 将错误报告到服务器日志  */
		EmitErrorReport();

		/* 中止当前事务以便进行恢复              */
		AbortCurrentTransaction();

		/*
		 * 释放其他所有资源，应对不在事务中的情形。    
		 *             
		 */
		LWLockReleaseAll();
		pgstat_report_wait_end();
		UnlockBuffers();
		/* 多半是死代码，但求稳妥：           */
		if (AuxProcessResourceOwner)
			ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * 现在回到正常顶层上下文，为下次清空 ErrorContext 
		 *           
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		FlushErrorState();

		/* 清刷顶层上下文中一切泄漏的数据  */
		MemoryContextResetAndDeleteChildren(AutovacMemCxt);

		/* 别留下指向已释放内存的悬空指针 */
		DatabaseListCxt = NULL;
		dlist_init(&DatabaseList);

		/* 现在可以重新允许中断了 */
		RESUME_INTERRUPTS();

		/* 若处于关闭模式，无需再做任何事；就此离开    */
		if (ShutdownRequestPending)
			AutoVacLauncherShutdown();

		/*
		 * 出错后至少睡 1 秒。我们不想以最快速度       
		 * 把错误日志塞满。                 
		 */
		pg_usleep(1000000L);
	}

	/* 此时可处理 ereport(ERROR)   */
	PG_exception_stack = &local_sigjmp_buf;

	/* 调用 rebuild_database_list 前必须解除信号阻塞  */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 设置始终安全的搜索路径。launcher 并不连接数据库，  
	 * 因此无效果。    
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 在自动清理进程中强制将 zero_damaged_pages 关闭，        
	 * 即使它在 postgresql.conf 中被设置。这种危险选项         
	 * 不宜非交互套用。  
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 强制关闭可设置的超时，以免这些设置阻碍常规    
	 * 维护工作的执行。                
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 强制 default_transaction_isolation 为 READ COMMITTED。           
	 * 我们既不想承担可串行化模式的开销，也不想增加   
	 * 死锁或延误其他事务的风险。  
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 即使系统被配置为其他获取一致性，对自动清理     
	 * 而言也总要用新鲜统计。      
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 紧急模式下，直接启动一个 worker（除非已请求关闭）
	 * 随即离开
	 */
	if (!AutoVacuumingActive())
	{
		if (!ShutdownRequestPending)
			do_start_worker();
		proc_exit(0);			/* done */
	}

	AutoVacuumShmem->av_launcherpid = MyProcPid;

	/*
	 * 创建初始数据库列表。希望该列表保持的不变式是：
	 * 按 next_time 递减排序。一旦某条目被更新为更高的      
	 * 时间，它就移到最前（这没错，唯一操作是向        
	 * 条目加上 autovacuum_naptime，且时间总在递增）            
	 *                                   
	 */
	rebuild_database_list(InvalidOid);

	/* 循环直至请求关闭    */
	while (!ShutdownRequestPending)
	{
		struct timeval nap;
		TimestampTz current_time = 0;
		bool		can_launch;

		/*
		 * 这个循环与 WaitLatch 的常规用法稍有不同，因为
		 * 我们想在第一次启动子进程前先睡。所以是先 
		 * WaitLatch，然后 ResetLatch，再检查唤醒条件。    
		 *                     
		 */

		launcher_determine_sleep(!dlist_is_empty(&AutoVacuumShmem->av_freeWorkers),
								 false, &nap);

		/*
		 * 等待休眠期结束或收到信号（各信号处理器都会  
		 * 通过调用 SetLatch 来唤醒我们）。       
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 (nap.tv_sec * 1000L) + (nap.tv_usec / 1000L),
						 WAIT_EVENT_AUTOVACUUM_MAIN);

		ResetLatch(MyLatch);

		HandleAutoVacLauncherInterrupts();

		/*
		 * 某个 worker 完成，或 postmaster 通知启动 worker 失败   
		 */
		if (got_SIGUSR2)
		{
			got_SIGUSR2 = false;

			/* 必要时重新平衡代价限制*/
			if (AutoVacuumShmem->av_signal[AutoVacRebalance])
			{
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
				AutoVacuumShmem->av_signal[AutoVacRebalance] = false;
				autovac_recalculate_workers_for_balance();
				LWLockRelease(AutovacuumLock);
			}

			if (AutoVacuumShmem->av_signal[AutoVacForkFailed])
			{
				/*
				 * 若 postmaster 启动新 worker 失败，就稍睡      
				 * 片刻后重发信号。新 worker 状态仍在内存中，
				 * 因此这就足够了。之后我们重启主循环。 
				 *                                
				 *
				 * XXX：是否应限制重试次数？我看意义不大， 
				 * 因为以后启动 worker 仍会以同样方式失败。 
				 *                                                   
				 */
				AutoVacuumShmem->av_signal[AutoVacForkFailed] = false;
				pg_usleep(1000000L);	/* 1s */
				SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);
				continue;
			}
		}

		/*
		 * 尝试启动 worker 之前需要检查一些条件。首先，  
		 * 要确保有可用的 worker 槽位。其次，要确保没有     
		 * 其他 worker 在启动过程中失败。                         
		 *                          
		 */

		current_time = GetCurrentTimestamp();
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		can_launch = !dlist_is_empty(&AutoVacuumShmem->av_freeWorkers);

		if (AutoVacuumShmem->av_startingWorker != NULL)
		{
			int			waittime;
			WorkerInfo	worker = AutoVacuumShmem->av_startingWorker;

			/*
			 * 另一 worker 仍在启动（或启动时已失败），
			 * 无法再启动新 worker，因此再多睡一会儿；该     
			 * worker 一就绪就会再次唤醒我们。                   
			 * 但为此最多只等 autovacuum_naptime 秒（至多        
			 * 60 秒）。注意，连接某特定数据库失败在这里 
			 * 不是问题，因为 worker 在尝试连接之前会把  
			 * 自己从 startingWorker 指针移除。postmaster       
			 * 出的问题（如 fork() 失败）另行上报处理。   
			 * 可能让这段代码触发的唯一问题，出现在 worker 
			 * 把 WorkerInfo 移出 startingWorker 指针之前的       
			 * AutoVacWorkerMain 前段代码中。                         
			 *                                                  
			 *                        
			 */
			waittime = Min(autovacuum_naptime, 60) * 1000;
			if (TimestampDifferenceExceeds(worker->wi_launchtime, current_time,
										   waittime))
			{
				LWLockRelease(AutovacuumLock);
				LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

				/*
				 * 没有其他进程能把 worker 置为启动态，所以 
				 * 交换锁之后 startingWorker 仍为 INVALID，我们    
				 * 就认定它还是上面那个（故不复查        
				 * 启动时间）。       
				 */
				if (AutoVacuumShmem->av_startingWorker != NULL)
				{
					worker = AutoVacuumShmem->av_startingWorker;
					worker->wi_dboid = InvalidOid;
					worker->wi_tableoid = InvalidOid;
					worker->wi_sharedrel = false;
					worker->wi_proc = NULL;
					worker->wi_launchtime = 0;
					dlist_push_head(&AutoVacuumShmem->av_freeWorkers,
									&worker->wi_links);
					AutoVacuumShmem->av_startingWorker = NULL;
					ereport(WARNING,
							errmsg("autovacuum worker took too long to start; canceled"));
				}
			}
			else
				can_launch = false;
		}
		LWLockRelease(AutovacuumLock);	/* 共享或独占均可      */

		/* 如果无事可做，就回去继续睡觉     */
		if (!can_launch)
			continue;

		/* 可以启动新 worker 了     */

		if (dlist_is_empty(&DatabaseList))
		{
			/*
			 * 列表为空的特殊情况：立刻启动一个 worker。     
			 * 覆盖 pgstats 中尚无数据库的初始情形（此时   
			 * 列表为空）。launcher_determine_sleep 中的      
			 * 约束使我们不会过快启动 worker（列表为空时
			 * 至多每隔 autovacuum_naptime 启动一次）。             
			 *        
			 */
			launch_worker(current_time);
		}
		else
		{
			/*
			 * 因为 rebuild_database_list 构造的列表把最远    
			 * adl_next_worker 排在最前，所以我们从列表尾部    
			 * 取数据库。  
			 */
			avl_dbase  *avdb;

			avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

			/*
			 * 若 next_worker 就是现在或已过时，则启动 worker
			 *     
			 */
			if (TimestampDifferenceExceeds(avdb->adl_next_worker,
										   current_time, 0))
				launch_worker(current_time);
		}
	}

	AutoVacLauncherShutdown();
}

/*
 * 处理任何新中断。   
 */
static void
HandleAutoVacLauncherInterrupts(void)
{
	/* 正常的关闭情形    */
	if (ShutdownRequestPending)
		AutoVacLauncherShutdown();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/* 配置文件中请求了关闭？  */
		if (!AutoVacuumingActive())
			AutoVacLauncherShutdown();

		/* 以防休眠时间有变，重建该列表   */
		rebuild_database_list(InvalidOid);
	}

	/* 处理屏障事件。  */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* 记录本进程的内存上下文。               */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	/* 处理睡眠期间发生的 sinval 追赶中断。             */
	ProcessCatchupInterrupt();
}

/*
 * 从 autovac launcher 正常退出。            
 */
static void
AutoVacLauncherShutdown(void)
{
	ereport(DEBUG1,
			(errmsg_internal("autovacuum launcher shutting down")));
	AutoVacuumShmem->av_launcherpid = 0;

	proc_exit(0);				/* 完  */
}

/*
 * 根据数据库列表确定睡眠时间。              
 *
 * "canlaunch" 参数指示当前是否可以立即启动 worker，            
 * 例如因 worker 全部繁忙时。若此值为假，我们将           
 * 引起长睡眠，会在 worker 退出时被中断。             
 */
static void
launcher_determine_sleep(bool canlaunch, bool recursing, struct timeval *nap)
{
	/*
	 * 我们睡到下次计划的清理为止。我们相信在构建  
	 * 数据库列表时已注意确保没有任何表项的时间         
	 * 落在过去；若第一个表项的 next_worker 值太近，或       
	 * 时间在过去，则只睡短暂的名义时长。  
	 */
	if (!canlaunch)
	{
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}
	else if (!dlist_is_empty(&DatabaseList))
	{
		TimestampTz current_time = GetCurrentTimestamp();
		TimestampTz next_wakeup;
		avl_dbase  *avdb;
		long		secs;
		int			usecs;

		avdb = dlist_tail_element(avl_dbase, adl_node, &DatabaseList);

		next_wakeup = avdb->adl_next_worker;
		TimestampDifference(current_time, next_wakeup, &secs, &usecs);

		nap->tv_sec = secs;
		nap->tv_usec = usecs;
	}
	else
	{
		/* 列表为空，睡满整个 autovacuum_naptime 秒。      */
		nap->tv_sec = autovacuum_naptime;
		nap->tv_usec = 0;
	}

	/*
	 * 若结果恰好为零，说明某个数据库曾有表项的        
	 * 时间在过去。重建列表，使各数据库重新均匀        
	 * 分布，并重新计算睡眠时间。这种情况可能             
	 * 发生在需要清理的表多于 worker 且它们都耗               
	 * 时超过 autovacuum_naptime。          
	 *
	 * 我们只递归一次。rebuild_database_list 总是应                
	 * 返回未来时间，但最好不要过分信赖这一点。   
	 */
	if (nap->tv_sec == 0 && nap->tv_usec == 0 && !recursing)
	{
		rebuild_database_list(InvalidOid);
		launcher_determine_sleep(canlaunch, true, nap);
		return;
	}

	/* 允许 launcher 睡眠的最短时间。             */
	if (nap->tv_sec <= 0 && nap->tv_usec <= MIN_AUTOVAC_SLEEPTIME * 1000)
	{
		nap->tv_sec = 0;
		nap->tv_usec = MIN_AUTOVAC_SLEEPTIME * 1000;
	}

	/*
	 * 若睡眠时间太长，则钳制到一个任意最大值（加上    
	 * 零头秒数，以求简单）。这避免了基本                 
	 * 无限的睡眠，比如系统时钟倒退几年这种                 
	 * 怪事。 
	 */
	if (nap->tv_sec > MAX_AUTOVAC_SLEEPTIME)
		nap->tv_sec = MAX_AUTOVAC_SLEEPTIME;
}

/*
 * 构建更新后的 DatabaseList。它只能包含出现在                
 * pgstats 中的数据库，且必须按 next_worker 从高到         
 * 低排序，均匀分布于下一个 autovacuum_naptime 周期。 
 *
 * 接收触发本列表生成的数据库的 Oid（我们称其为          
 * "新"数据库，因为当该数据库已在列表中时，              
 * 我们预期根本不会调用此函数）。                     
 * 已有列表（若存在）将用于保持                         
 * 数据库在 autovacuum_naptime 周期内的顺序。新库放在          
 * 该区间末尾。实际值不保存，这应该                          
 * 不是大问题。
 */
static void
rebuild_database_list(Oid newdb)
{
	List	   *dblist;
	ListCell   *cell;
	MemoryContext newcxt;
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	HASHCTL		hctl;
	int			score;
	int			nelems;
	HTAB	   *dbhash;
	dlist_iter	iter;

	newcxt = AllocSetContextCreate(AutovacMemCxt,
								   "Autovacuum database list",
								   ALLOCSET_DEFAULT_SIZES);
	tmpcxt = AllocSetContextCreate(newcxt,
								   "Autovacuum database list (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * 实现这一点并不像听起来那么简单，因为我们需要把  
	 * 新数据库放在列表末尾；接着是曾在                     
	 * 列表上的数据库，最后（在列表尾部）是            
	 * 所有不在现有列表上的其他数据库。  
	 *
	 * 为此构建一个空的评分数据库哈希表。我们先给       
	 * 新数据库最低分（零），然后                      
	 * 按顺序为现有列表中的数据库递增分数，最后           
	 * 为经由 get_database_list() 得到、              
	 * 且不在哈希中的所有库递增分数。        
	 *
	 * 然后把所有哈希元素放入一个数组，按分数              
	 * 排序数组，最后把数组元素放入新的双向链表。     
	 *      
	 */
	hctl.keysize = sizeof(Oid);
	hctl.entrysize = sizeof(avl_dbase);
	hctl.hcxt = tmpcxt;
	dbhash = hash_create("autovacuum db hash", 20, &hctl,	/* magic number here
															 * FIXME */
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 先插入新数据库。            */
	score = 0;
	if (OidIsValid(newdb))
	{
		avl_dbase  *db;
		PgStat_StatDBEntry *entry;

		/* 仅当该数据库有 pgstat 表项时才考虑它   */
		entry = pgstat_fetch_stat_dbentry(newdb);
		if (entry != NULL)
		{
			/* 我们假定找不到它，因为哈希表刚创建        */
			db = hash_search(dbhash, &newdb, HASH_ENTER, NULL);

			/* 键已由 hash_search 填好          */
			db->adl_score = score++;
			/* next_worker 稍后填写       */
		}
	}

	/* 现在插入来自现有列表的数据库      */
	dlist_foreach(iter, &DatabaseList)
	{
		avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/*
		 * 跳过没有统计表项的数据库——尤其是除掉          
		 * 已删除的库。  
		 */
		entry = pgstat_fetch_stat_dbentry(avdb->adl_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adl_datid), HASH_ENTER, &found);

		if (!found)
		{
			/* 键已由 hash_search 填好          */
			db->adl_score = score++;
			/* next_worker 稍后填写       */
		}
	}

	/* 最后，插入所有之前未插入的合格数据库           */
	dblist = get_database_list();
	foreach(cell, dblist)
	{
		avw_dbase  *avdb = lfirst(cell);
		avl_dbase  *db;
		bool		found;
		PgStat_StatDBEntry *entry;

		/* 仅考虑有 pgstat 表项的数据库      */
		entry = pgstat_fetch_stat_dbentry(avdb->adw_datid);
		if (entry == NULL)
			continue;

		db = hash_search(dbhash, &(avdb->adw_datid), HASH_ENTER, &found);
		/* 仅当数据库不在哈希中时才更新其分数。            */
		if (!found)
		{
			/* 键已由 hash_search 填好          */
			db->adl_score = score++;
			/* next_worker 稍后填写       */
		}
	}
	nelems = score;

	/* 从此处起，分配的内存归新列表所有           */
	MemoryContextSwitchTo(newcxt);
	dlist_init(&DatabaseList);

	if (nelems > 0)
	{
		TimestampTz current_time;
		int			millis_increment;
		avl_dbase  *dbary;
		avl_dbase  *db;
		HASH_SEQ_STATUS seq;
		int			i;

		/* 把所有哈希元素放入一个数组 */
		dbary = palloc(nelems * sizeof(avl_dbase));

		i = 0;
		hash_seq_init(&seq, dbhash);
		while ((db = hash_seq_search(&seq)) != NULL)
			memcpy(&(dbary[i++]), db, sizeof(avl_dbase));

		/* 对数组排序*/
		qsort(dbary, nelems, sizeof(avl_dbase), db_comparator);

		/*
		 * 确定调度中各数据库之间的时间间隔。若           
		 * 发现配置的 naptime 会使睡眠时间低于               
		 * 最短睡眠时间（launcher_determine_sleep 的编码         
		 * 不允许这样），则静默用更大的 naptime（但不        
		 * 改 GUC）。     
		 */
		millis_increment = 1000.0 * autovacuum_naptime / nelems;
		if (millis_increment <= MIN_AUTOVAC_SLEEPTIME)
			millis_increment = MIN_AUTOVAC_SLEEPTIME * 1.1;

		current_time = GetCurrentTimestamp();

		/*
		 * 把数组元素移入 dlist，并在遍历数组时         
		 * 设置 next_worker                 
		 */
		for (i = 0; i < nelems; i++)
		{
			db = &(dbary[i]);

			current_time = TimestampTzPlusMilliseconds(current_time,
													   millis_increment);
			db->adl_next_worker = current_time;

			/* 越靠后的元素应越靠近链表头部              */
			dlist_push_head(&DatabaseList, &db->adl_node);
		}
	}

	/* 完成后清理内存     */
	if (DatabaseListCxt != NULL)
		MemoryContextDelete(DatabaseListCxt);
	MemoryContextDelete(tmpcxt);
	DatabaseListCxt = newcxt;
	MemoryContextSwitchTo(oldcxt);
}

/* avl_dbase 的 qsort 比较器，使用 adl_score*/
static int
db_comparator(const void *a, const void *b)
{
	if (((const avl_dbase *) a)->adl_score == ((const avl_dbase *) b)->adl_score)
		return 0;
	else
		return (((const avl_dbase *) a)->adl_score < ((const avl_dbase *) b)->adl_score) ? 1 : -1;
}

/*
 * do_start_worker
 *
 * 由 launcher 启动 autovacuum worker 的最简流程。                 
 * 它确定要处理哪个数据库，设置共享内存相关内容     
 * 并通知 postmaster 启动该 worker。若调用时 autovacuum_workers     
 * 已活跃，也会稳妥地失败。  
 *
 * 返回值是 worker 将要处理的数据库的 OID，                      
 * 若实际未启动 worker 则为 InvalidOid。  
 */
static Oid
do_start_worker(void)
{
	List	   *dblist;
	ListCell   *cell;
	TransactionId xidForceLimit;
	MultiXactId multiForceLimit;
	bool		for_xid_wrap;
	bool		for_multi_wrap;
	avw_dbase  *avdb;
	TimestampTz current_time;
	bool		skipit = false;
	Oid			retval = InvalidOid;
	MemoryContext tmpcxt,
				oldcxt;

	/* 没有空闲 worker 时快速返回           */
	LWLockAcquire(AutovacuumLock, LW_SHARED);
	if (dlist_is_empty(&AutoVacuumShmem->av_freeWorkers))
	{
		LWLockRelease(AutovacuumLock);
		return InvalidOid;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * 创建并切换到临时内存上下文，以免泄漏              
	 * 数据库列表的内存。     
	 */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Autovacuum start worker (tmp)",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* 获取数据库列表   */
	dblist = get_database_list();

	/*
	 * 确定我们允许在未强制清理的情况下通过的           
	 * datfrozenxid/relfrozenxid。（此限制对特定               
	 * 表可收紧，但不能放松。） 
	 */
	recentXid = ReadNextTransactionId();
	xidForceLimit = recentXid - autovacuum_freeze_max_age;
	/* 确保其为"普通"XID，否则 TransactionIdPrecedes 异常。  */
	/* 这可能导致该限制倒退 3，但没有问题           */
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;

	/* 同时确定我们要考虑的最老 datminmxid。     */
	recentMulti = ReadNextMultiXactId();
	multiForceLimit = recentMulti - MultiXactMemberFreezeThreshold();
	if (multiForceLimit < FirstMultiXactId)
		multiForceLimit -= FirstMultiXactId;

	/*
	 * 选择要连接的数据库。我们挑选最久未被               
	 * 自动清理过的，或为防止 XID 回卷导致                
	 * 数据丢失而需清理的库。若发现有库有                  
	 * XID 回卷风险，我们就挑 datfrozenxid 最老              
	 * 的那个，而不看自动清理时间；同样，若                
	 * 有库处于 MultiXactId 回卷，则挑 datminmxid 最老的。注意 
	 * XID 回卷危险的库优先级高于处于 multi 回卷危险的。 
	 *
	 * 注意没有统计表项                                              
	 * 的库不予考虑，XID 回卷情形除外。理论是        
	 * 若自统计上次初始化以来无人连接过它，就免清理。 
	 *           
	 *
	 * XXX 若有更多信息判断连接前是否需要                    
	 * 清理就更好了。也许可以翻查该库的 pgstats            
	 * 数据？一个思路是在 pgstats 中跟踪每个              
	 * 数据库的新元组和死元组数量。但目前               
	 * 尚不清楚如何构造一个既能量测它又不使                
	 * 较空闲库挨饿的度量。     
	 */
	avdb = NULL;
	for_xid_wrap = false;
	for_multi_wrap = false;
	current_time = GetCurrentTimestamp();
	foreach(cell, dblist)
	{
		avw_dbase  *tmp = lfirst(cell);
		dlist_iter	iter;

		/* 检查该库是否面临回卷风险              */
		if (TransactionIdPrecedes(tmp->adw_frozenxid, xidForceLimit))
		{
			if (avdb == NULL ||
				TransactionIdPrecedes(tmp->adw_frozenxid,
									  avdb->adw_frozenxid))
				avdb = tmp;
			for_xid_wrap = true;
			continue;
		}
		else if (for_xid_wrap)
			continue;			/* 忽略无风险库     */
		else if (MultiXactIdPrecedes(tmp->adw_minmulti, multiForceLimit))
		{
			if (avdb == NULL ||
				MultiXactIdPrecedes(tmp->adw_minmulti, avdb->adw_minmulti))
				avdb = tmp;
			for_multi_wrap = true;
			continue;
		}
		else if (for_multi_wrap)
			continue;			/* 忽略无风险库     */

		/* 有则查 pgstat 表项  */
		tmp->adw_entry = pgstat_fetch_stat_dbentry(tmp->adw_datid);

		/*
		 * 跳过没有 pgstat 表项的数据库；即它从未            
		 * 有活动
		 */
		if (!tmp->adw_entry)
			continue;

		/*
		 * 另外，跳过在数据库列表上显示为最近              
		 * 已被处理过的库（不足 autovacuum_naptime 秒）。        
		 * 这样做是为了不选中我们刚刚选过的           
		 * 数据库，只是 pgstat 还没来得及更新上次              
		 * 清理时间。     
		 */
		skipit = false;

		dlist_reverse_foreach(iter, &DatabaseList)
		{
			avl_dbase  *dbp = dlist_container(avl_dbase, adl_node, iter.cur);

			if (dbp->adl_datid == tmp->adw_datid)
			{
				/*
				 * 若该库的 next_worker 值落在当前时间到        
				 * 当前时间加 naptime 之间则跳过它。      
				 */
				if (!TimestampDifferenceExceeds(dbp->adl_next_worker,
												current_time, 0) &&
					!TimestampDifferenceExceeds(current_time,
												dbp->adl_next_worker,
												autovacuum_naptime * 1000))
					skipit = true;

				break;
			}
		}
		if (skipit)
			continue;

		/*
		 * 记住自动清理时间最老的库。（走到这里时，    
		 * tmp->entry 与 db->entry 必非空。）   
		 */
		if (avdb == NULL ||
			tmp->adw_entry->last_autovac_time < avdb->adw_entry->last_autovac_time)
			avdb = tmp;
	}

	/* 找到数据库——处理它 */
	if (avdb != NULL)
	{
		WorkerInfo	worker;
		dlist_node *wptr;

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * 从空闲链表取一个 worker 表项。上面已检查过，   
		 * 确实应有空闲槽位。  
		 */
		wptr = dlist_pop_head_node(&AutoVacuumShmem->av_freeWorkers);

		worker = dlist_container(WorkerInfoData, wi_links, wptr);
		worker->wi_dboid = avdb->adw_datid;
		worker->wi_proc = NULL;
		worker->wi_launchtime = GetCurrentTimestamp();

		AutoVacuumShmem->av_startingWorker = worker;

		LWLockRelease(AutovacuumLock);

		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);

		retval = avdb->adw_datid;
	}
	else if (skipit)
	{
		/*
		 * 如果列表上所有数据库都被跳过，则重建         
		 * 它，因为可能含已删除库。 
		 */
		rebuild_database_list(InvalidOid);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return retval;
}

/*
 * launch_worker
 *
 * 由 launcher 启动 worker 的包装例程。除了实际启动            
 * 之外，还会更新数据库列表，以反映下次需要               
 * 在所选数据库上再次启动 worker 的时间。实际数据库选择 
 * 留给do_start_worker。
 *
 * 此外，若所选数据库原先不在列表中，本例程              
 * 还应向数据库列表插入一个表项。             
 */
static void
launch_worker(TimestampTz now)
{
	Oid			dbid;
	dlist_iter	iter;

	dbid = do_start_worker();
	if (OidIsValid(dbid))
	{
		bool		found = false;

		/*
		 * 遍历数据库列表并更新相应表项。如果               
		 * 数据库不在列表中，我们将重建列表。  
		 */
		dlist_foreach(iter, &DatabaseList)
		{
			avl_dbase  *avdb = dlist_container(avl_dbase, adl_node, iter.cur);

			if (avdb->adl_datid == dbid)
			{
				found = true;

				/*
				 * 把 autovacuum_naptime 秒加到当前时间上，并把   
				 * 结果作为该库新的 "next_worker" 字段。      
				 */
				avdb->adl_next_worker =
					TimestampTzPlusMilliseconds(now, autovacuum_naptime * 1000);

				dlist_move_head(&DatabaseList, iter.cur);
				break;
			}
		}

		/*
		 * 如果数据库不在数据库列表中，我们重建          
		 * 列表。该库可能仍然进不了列表，比如说          
		 * 它是一个没有                                             
		 * pgstat 表项的数据库，但这不成问题，因为我们   
		 * 本就不想让 worker 定期轮到这些库。   
		 */
		if (!found)
			rebuild_database_list(dbid);
	}
}

/*
 * 由 postmaster 调用，报告 fork 成为 worker 的进程失败。  
 * postmaster 应在调用本函数后不久                        
 * 发 SIGUSR2 给 launcher。 
 */
void
AutoVacWorkerFailed(void)
{
	AutoVacuumShmem->av_signal[AutoVacForkFailed] = true;
}

/* SIGUSR2：worker 已启动运行，或刚结束，或 fork 失败        */
static void
avl_sigusr2_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGUSR2 = true;
	SetLatch(MyLatch);

	errno = save_errno;
}


/********************************************************************
 *					  AUTOVACUUM WORKER CODE
 ********************************************************************/

#ifdef EXEC_BACKEND
/*
 * autovacuum worker 的 forkexec 例程。    
 *
 * 组织参数列表，然后 fork/exec。  
 */
static pid_t
avworker_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkavworker";
	av[ac++] = NULL;			/* 由 postmaster_forkexec 填写   */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * 我们需要在调用 InitProcess 之前从外部设置        
 */
void
AutovacuumWorkerIAm(void)
{
	am_autovacuum_worker = true;
}
#endif

/*
 * autovacuum worker 进程的主入口。        
 *
 * 本代码大量仿照 pgarch.c。           
 */
int
StartAutoVacWorker(void)
{
	pid_t		worker_pid;

#ifdef EXEC_BACKEND
	switch ((worker_pid = avworker_forkexec()))
#else
	switch ((worker_pid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork autovacuum worker process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* postmaster 子进程内 */
			InitPostmasterChild();

			/* 关闭 postmaster 的套接字 */
			ClosePostmasterPorts(false);

			AutoVacWorkerMain(0, NULL);
			break;
#endif
		default:
			return (int) worker_pid;
	}

	/* 不应到达此处 */
	return 0;
}

/*
 * AutoVacWorkerMain
 */
NON_EXEC_STATIC void
AutoVacWorkerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;
	Oid			dbid;

	am_autovacuum_worker = true;

	MyBackendType = B_AUTOVAC_WORKER;
	init_ps_display(NULL);

	SetProcessingMode(InitProcessing);

	/*
	 * 设置信号处理器。我们像普通后端一样在              
	 * 数据库上操作，故用同样的信号处理，等效代码见  
	 * tcop/postgres.c 
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);

	/*
	 * SIGINT 用于通知取消当前表的清理；SIGTERM                 
	 * 表示中止并干净退出，而 SIGQUIT 表示弃船。    
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */

	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 在共享内存中创建每后端 PGPROC 结构，但             
	 * EXEC_BACKEND 情形除外——那时这一步已在                  
	 * SubPostmasterMain 中完成。我们必须先做这步才能用        
	 * LWLocks（后者已用过）。    
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/* 早期初始化      */
	BaseInit();

	/*
	 * 如果遇到异常，处理会在此恢复。           
	 *
	 * 与多数辅助进程不同，出错后我们不尝试       
	 * 继续处理；只是清理后退出。稍后由 autovac          
	 * launcher 负责再派生一个 worker。                  
	 *
	 * 注意我们用 sigsetjmp(..., 1)，这样当前信号掩码          
	 * （即 BlockSig）会在 longjmp 回到这里时恢复。因此     
	 * 在退出前 SIGQUIT 以外的信号都被阻塞。看起来       
	 * 这一策略似乎使 HOLD_INTERRUPTS() 调用多余，              
	 * 但 InterruptPending 可能已置位，故仍需。   
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* 因为没用 PG_TRY，必须手工重置错误栈      */
		error_context_stack = NULL;

		/* 清理期间禁止中断              */
		HOLD_INTERRUPTS();

		/* 把错误报告到服务器日志  */
		EmitErrorReport();

		/*
		 * 现在可以消失了。注意因为我们调用过            
		 * InitProcess，已注册 ProcKill 回调，它会清理     
		 * 必要状态。 
		 */
		proc_exit(0);
	}

	/* 此后能处理 ereport(ERROR)   */
	PG_exception_stack = &local_sigjmp_buf;

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 设置永远安全的搜索路径，使恶意用户无法重定向   
	 * 用户代码（如 pg_index.indexprs）。（它在 
	 * SECURITY_RESTRICTED_OPERATION 沙箱中运行，恶意用户       
	 * 也无法控制整个 autovacuum worker。）             
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 在 autovac 进程中强制 zero_damaged_pages 为 OFF，             
	 * 即使 postgresql.conf 已设置。我们不想让这种危险选项   
	 * 非交互地生效。     
	 */
	SetConfigOption("zero_damaged_pages", "false", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 强制关闭可设置的超时，以免这些设置阻碍          
	 * 常规维护的执行。                
	 */
	SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("lock_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("idle_in_transaction_session_timeout", "0",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 强制 default_transaction_isolation 为 READ COMMITTED。           
	 * 我们不想支付可串行化模式的开销，也不想            
	 * 招致死锁或拖延别的事务。     
	 */
	SetConfigOption("default_transaction_isolation", "read committed",
					PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 强制关闭同步复制，使得即便在等待                      
	 * 备机连接时也能进行常规维护。这很重要，             
	 * 以确保我们不被阻止执行防回卷任务。  
	 */
	if (synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)
		SetConfigOption("synchronous_commit", "local",
						PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 即使系统配置为使用不同的获取一致性，              
	 * autovac 总要最新统计信息。    
	 */
	SetConfigOption("stats_fetch_consistency", "none", PGC_SUSET, PGC_S_OVERRIDE);

	/*
	 * 获取将要处理的数据库的信息。             
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * 注意 startingWorker 可能为 INVALID；通常不应如此， 
	 * 但若 worker 在 fork 后、此处前失败，launcher 可能  
	 * 已决定将其移出队列并重新启动。                    
	 *       
	 */
	if (AutoVacuumShmem->av_startingWorker != NULL)
	{
		MyWorkerInfo = AutoVacuumShmem->av_startingWorker;
		dbid = MyWorkerInfo->wi_dboid;
		MyWorkerInfo->wi_proc = MyProc;

		/* 插入运行链表           */
		dlist_push_head(&AutoVacuumShmem->av_runningWorkers,
						&MyWorkerInfo->wi_links);

		/*
		 * 把它从 "starting" 指针中移除，这样 launcher 就能     
		 * 按需启动新 worker  
		 */
		AutoVacuumShmem->av_startingWorker = NULL;
		LWLockRelease(AutovacuumLock);

		on_shmem_exit(FreeWorkerInfo, 0);

		/* 唤醒 launcher      */
		if (AutoVacuumShmem->av_launcherpid != 0)
			kill(AutoVacuumShmem->av_launcherpid, SIGUSR2);
	}
	else
	{
		/* 无对应 worker 项，退出   */
		elog(WARNING, "autovacuum worker started without a worker entry");
		dbid = InvalidOid;
		LWLockRelease(AutovacuumLock);
	}

	if (OidIsValid(dbid))
	{
		char		dbname[NAMEDATALEN];

		/*
		 * 把 autovac 启动上报给累计统计系统。我们    
		 * 特意在 InitPostgres 之前这样做，以便即使
		 * 连接尝试失败，last_autovac_time 也会得到更新。     
		 * 以免 autovac 反复选中打不开的数据库而"卡死"，    
		 * 而不是对它能连接的内容取得任何进展。           
		 *                            
		 */
		pgstat_report_autovac(dbid);

		/*
		 * 连接到选定的数据库，不指定具体用户，          
		 * 并忽略 datallowconn。收集数据库名供显示。    
		 *         
		 *
		 * 注意：若选中的是刚被删除的库（因统计信息   
		 * 过期），我们会失败并退出。     
		 */
		InitPostgres(NULL, dbid, NULL, InvalidOid, false, true,
					 dbname);
		SetProcessingMode(NormalProcessing);
		set_ps_display(dbname);
		ereport(DEBUG1,
				(errmsg_internal("autovacuum: processing database \"%s\"", dbname)));

		if (PostAuthDelay)
			pg_usleep(PostAuthDelay * 1000000L);

		/* 然后做适当数量的工作       */
		recentXid = ReadNextTransactionId();
		recentMulti = ReadNextMultiXactId();
		do_autovacuum();
	}

	/*
	 * launcher 会在 ProcKill 中收到我死亡的通知，*前提是*    
	 * 拿到过 worker 槽位    
	 */

	/* 完成，退出   */
	proc_exit(0);
}

/*
 * 将 WorkerInfo 归还空闲链表   
 */
static void
FreeWorkerInfo(int code, Datum arg)
{
	if (MyWorkerInfo != NULL)
	{
		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/*
		 * 唤醒 launcher，使其能在需要时立即启动新 worker。   
		 * 这里只在本地内存中保存 launcher 的 PID；              
		 * 真正的信号会在 PGPROC 被回收时发出。注意         
		 * 我们总是这样做，以便 launcher 能重新平衡其余      
		 * worker 的代价限制设置。        
		 *
		 * 我们在一定程度上忽略了 launcher 在我们读取它
		 * 与实际 kill 之间改变其 PID 的风险；我们预期        
		 * ProcKill 将紧随我们被调用，并假定进程退出后       
		 * PID 不会很快重用。     
		 */
		AutovacuumLauncherPid = AutoVacuumShmem->av_launcherpid;

		dlist_delete(&MyWorkerInfo->wi_links);
		MyWorkerInfo->wi_dboid = InvalidOid;
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		MyWorkerInfo->wi_proc = NULL;
		MyWorkerInfo->wi_launchtime = 0;
		pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);
		dlist_push_head(&AutoVacuumShmem->av_freeWorkers,
						&MyWorkerInfo->wi_links);
		/* 已不属于我  */
		MyWorkerInfo = NULL;

		/*
		 * 我们已不活跃，触发重新平衡存活的 worker      
		 *        
		 */
		AutoVacuumShmem->av_signal[AutoVacRebalance] = true;
		LWLockRelease(AutovacuumLock);
	}
}

/*
 * 根据相关 GUC 与全局状态的值，为自动清理 worker 以及执行
 * VACUUM 或 ANALYZE 的后端，更新 vacuum 代价延迟相关参数。 
 * 必须在 vacuum 设置期间以及每次配置重载后调用，        
 * 以确保值保持最新。               
 */
void
VacuumUpdateCosts(void)
{
	if (MyWorkerInfo)
	{
		if (av_storage_param_cost_delay >= 0)
			vacuum_cost_delay = av_storage_param_cost_delay;
		else if (autovacuum_vac_cost_delay >= 0)
			vacuum_cost_delay = autovacuum_vac_cost_delay;
		else
			/* 回退到 VacuumCostDelay    */
			vacuum_cost_delay = VacuumCostDelay;

		AutoVacuumUpdateCostLimit();
	}
	else
	{
		/* 必须为显式 VACUUM 或 ANALYZE */
		vacuum_cost_delay = VacuumCostDelay;
		vacuum_cost_limit = VacuumCostLimit;
	}

	/*
	 * 如果允许配置变更影响 VacuumCostActive，则确保          
	 * 它已更新。    
	 */
	if (VacuumFailsafeActive)
		Assert(!VacuumCostActive);
	else if (vacuum_cost_delay > 0)
		VacuumCostActive = true;
	else
	{
		VacuumCostActive = false;
		VacuumCostBalance = 0;
	}

	/*
	 * 由于代价日志记录需要加锁，因此当所用消息级别     
	 * 不会发出日志时，避免构造该日志消息。                 
	 */
	if (MyWorkerInfo && message_level_is_interesting(DEBUG2))
	{
		Oid			dboid,
					tableoid;

		Assert(!LWLockHeldByMe(AutovacuumLock));

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		dboid = MyWorkerInfo->wi_dboid;
		tableoid = MyWorkerInfo->wi_tableoid;
		LWLockRelease(AutovacuumLock);

		elog(DEBUG2,
			 "Autovacuum VacuumUpdateCosts(db=%u, rel=%u, dobalance=%s, cost_limit=%d, cost_delay=%g active=%s failsafe=%s)",
			 dboid, tableoid, pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance) ? "no" : "yes",
			 vacuum_cost_limit, vacuum_cost_delay,
			 vacuum_cost_delay > 0 ? "yes" : "no",
			 VacuumFailsafeActive ? "yes" : "no");
	}
}

/*
 * 为自动清理 worker 把 vacuum_cost_limit 更新为正确值，       
 * 该值依据其他相关代价限制参数以及需要在多少个       
 * worker 之间分摊而得出。自动清理 worker 必须               
 * 定期调用它，以防 av_nworkersForBalance 已被其他          
 * worker 或自动清理 launcher 更新；配置重载后也必须调用。  
 *               
 */
void
AutoVacuumUpdateCostLimit(void)
{
	if (!MyWorkerInfo)
		return;

	/*
	 * 注意：cost_limit 中的零也表示使用别处的值，因为     
	 * 零不是有效值。     
	 */

	if (av_storage_param_cost_limit > 0)
		vacuum_cost_limit = av_storage_param_cost_limit;
	else
	{
		int			nworkers_for_balance;

		if (autovacuum_vac_cost_limit > 0)
			vacuum_cost_limit = autovacuum_vac_cost_limit;
		else
			vacuum_cost_limit = VacuumCostLimit;

		/* 仅当未指定代价相关存储参数时才平衡限制值       */
		if (pg_atomic_unlocked_test_flag(&MyWorkerInfo->wi_dobalance))
			return;

		Assert(vacuum_cost_limit > 0);

		nworkers_for_balance = pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

		/* 至少 1 个自动清理 worker（本 worker）  */
		if (nworkers_for_balance <= 0)
			elog(ERROR, "nworkers_for_balance must be > 0");

		vacuum_cost_limit = Max(vacuum_cost_limit / nworkers_for_balance, 1);
	}
}

/*
 * autovac_recalculate_workers_for_balance
 *		根据代价相关存储参数以及当前活跃 worker 数量，
 *		重新计算要纳入考量的 worker 数目。             
 *
 * 调用者须至少以共享模式持有 AutovacuumLock 才能访问  
 * worker->wi_proc 
 */
static void
autovac_recalculate_workers_for_balance(void)
{
	dlist_iter	iter;
	int			orig_nworkers_for_balance;
	int			nworkers_for_balance = 0;

	Assert(LWLockHeldByMe(AutovacuumLock));

	orig_nworkers_for_balance =
		pg_atomic_read_u32(&AutoVacuumShmem->av_nworkersForBalance);

	dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
	{
		WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

		if (worker->wi_proc == NULL ||
			pg_atomic_unlocked_test_flag(&worker->wi_dobalance))
			continue;

		nworkers_for_balance++;
	}

	if (nworkers_for_balance != orig_nworkers_for_balance)
		pg_atomic_write_u32(&AutoVacuumShmem->av_nworkersForBalance,
							nworkers_for_balance);
}

/*
 * get_database_list
 *		返回 pg_database 中的所有数据库列表。   
 *
 * 列表及其关联数据分配在调用者的内存上下文中，       
 * 由它负责确保事后得到妥善清理。                           
 *
 * 注意：这是自动清理 launcher 唯一使用事务的函数。    
 * 虽然我们未依附于任何具体数据库，因而无法访问      
 * 大多数目录，但确实拥有足够的基础设施来对          
 * pg_database 做顺序扫描。 
 */
static List *
get_database_list(void)
{
	List	   *dblist = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* 这是我们将用来分配输出数据的内存上下文    */
	resultcxt = CurrentMemoryContext;

	/*
	 * 启动一个事务以便访问 pg_database，并获取快照。      
	 * 快照本身对我们没用，但我们感兴趣的是它带来的  
	 * 附带效果：设置 RecentGlobalXmin。（这对任何读取        
	 * 堆页面的东西都至关重要，因为 HOT 可能决定修剪  
	 * 页面，即使该进程并不打算修改任何元组。）   
	 *
	 * FIXME: 此注释不准确/代码有缺陷。未推入/未激活的    
	 * 快照不能可靠地阻止 HOT 修剪（例如处理缓存失效    
	 * 时 ->xmin 可能被清除）。                        
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);
		avw_dbase  *avdb;
		MemoryContext oldcxt;

		/*
		 * 若数据库已被部分删除，无法也无须清理它。  
		 *           
		 */
		if (database_is_invalid_form(pgdatabase))
		{
			elog(DEBUG2,
				 "autovacuum: skipping invalid database \"%s\"",
				 NameStr(pgdatabase->datname));
			continue;
		}

		/*
		 * 结果分配在调用者的上下文里，           
		 * 而非事务上下文。我们在循环内做此事，并在最后 
		 * 恢复原上下文，这样 heap_getnext() 这类泄漏性调用
		 * 不会发生在可能长存的上下文中。  
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		avdb = (avw_dbase *) palloc(sizeof(avw_dbase));

		avdb->adw_datid = pgdatabase->oid;
		avdb->adw_name = pstrdup(NameStr(pgdatabase->datname));
		avdb->adw_frozenxid = pgdatabase->datfrozenxid;
		avdb->adw_minmulti = pgdatabase->datminmxid;
		/* 此项稍后设置：*/
		avdb->adw_entry = NULL;

		dblist = lappend(dblist, avdb);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	/* 务必恢复调用者的内存上下文    */
	MemoryContextSwitchTo(resultcxt);

	return dblist;
}

/*
 * 逐表处理一个数据库      
 *
 * 注意 CHECK_FOR_INTERRUPTS 应当用在某些特定位置，            
 * 以免过久忽略关机命令。                  
 */
static void
do_autovacuum(void)
{
	Relation	classRel;
	HeapTuple	tuple;
	TableScanDesc relScan;
	Form_pg_database dbForm;
	List	   *table_oids = NIL;
	List	   *orphan_oids = NIL;
	HASHCTL		ctl;
	HTAB	   *table_toast_map;
	ListCell   *volatile cell;
	BufferAccessStrategy bstrategy;
	ScanKeyData key;
	TupleDesc	pg_class_desc;
	int			effective_multixact_freeze_max_age;
	bool		did_vacuum = false;
	bool		found_concurrent_worker = false;
	int			i;

	/*
	 * StartTransactionCommand 与 CommitTransactionCommand                   
	 * 会自动切换到别的上下文。需要用它跨事务保存
	 * 待清理/分析的关系列表。              
	 */
	AutovacMemCxt = AllocSetContextCreate(TopMemoryContext,
										  "Autovacuum worker",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(AutovacMemCxt);

	/* 启动一个事务，让我们的命令有事务可用。  */
	StartTransactionCommand();

	/*
	 * 计算需要紧急冻结的 multixact 年龄。该值通常     
	 * 为 autovacuum_multixact_freeze_max_age，但若 multixact             
	 * 成员空间不足时更小。  
	 */
	effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold();

	/*
	 * 查找 pg_database 条目并选定默认冻结年龄。对模板库  
	 * 和非可连接数据库使用零，否则用系统级默认值。 
	 *         
	 */
	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbForm = (Form_pg_database) GETSTRUCT(tuple);

	if (dbForm->datistemplate || !dbForm->datallowconn)
	{
		default_freeze_min_age = 0;
		default_freeze_table_age = 0;
		default_multixact_freeze_min_age = 0;
		default_multixact_freeze_table_age = 0;
	}
	else
	{
		default_freeze_min_age = vacuum_freeze_min_age;
		default_freeze_table_age = vacuum_freeze_table_age;
		default_multixact_freeze_min_age = vacuum_multixact_freeze_min_age;
		default_multixact_freeze_table_age = vacuum_multixact_freeze_table_age;
	}

	ReleaseSysCache(tuple);

	/* 已被 StartTransactionCommand 改变     */
	MemoryContextSwitchTo(AutovacMemCxt);

	classRel = table_open(RelationRelationId, AccessShareLock);

	/* 建立副本以便关闭 pg_class 后仍可使用     */
	pg_class_desc = CreateTupleDescCopy(RelationGetDescr(classRel));

	/* 为 toast 与主表 relid 映射创建哈希表    */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(av_relation);

	table_toast_map = hash_create("TOAST to main relid map",
								  100,
								  &ctl,
								  HASH_ELEM | HASH_BLOBS);

	/*
	 * 扫描 pg_class 以确定要清理哪些表。    
	 *
	 * 此工作分两遍完成：第一遍收集普通关系与              
	 * 物化视图的列表，第二遍收集 TOAST 表。              
	 * 之所以要进行第二遍，是因为在此期间，若 TOAST        
	 * 表本身没有任何 reloptions，我们想使用主关系的        
	 * pg_class.reloptions 条目；而若不知道主表 OID，      
	 * 就无法得到它。                
	 *
	 * 之所以要单独检查 TOAST 表，是因为对于矮宽型表，  
	 * 其 TOAST 表中的活动比例可能远大于父表。            
	 *                                
	 */
	relScan = table_beginscan_catalog(classRel, 0, NULL);

	/*
	 * 第一遍中，我们收集要清理的主表，以及主表          
	 * relid 到 TOAST relid 的映射。 
	 */
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		AutoVacOpts *relopts;
		Oid			relid;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW)
			continue;

		relid = classForm->oid;

		/*
		 * 检查它是否为临时表（多半属其他后端）。         
		 * 无法安全地处理其他后端的临时表。     
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
		{
			/*
			 * 若拥有的后端仍活跃且在使用临时模式，     
			 * 我们直接忽略它。此外为安全起见，若命名空间
			 * 不存在或终究不是临时命名空间，也忽略之。
			 */
			if (checkTempNamespaceStatus(classForm->relnamespace) == TEMP_NAMESPACE_IDLE)
			{
				/*
				 * 该表看似孤儿——尽管也可能是拥有后端     
				 * 已删除它并退出；我们的 pg_class 扫描快照  
				 * 未必最新，我们看到的可能是已提交死 
				 * 条目。记住它，以便稍后尝试删除。          
				 *                                              
				 */
				orphan_oids = lappend_oid(orphan_oids, relid);
			}
			continue;
		}

		/* 获取该表的 reloptions 与 pgstat 条目         */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		/* 看它需要 vacuum 还是 analyze  */
		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* 需要处理的关系会被加入 table_oids     */
		if (dovacuum || doanalyze)
			table_oids = lappend_oid(table_oids, relid);

		/*
		 * 为第二遍记住 TOAST 关联。注意：无论该表是否     
		 * 将被清理都必须这样做，因为我们不会随父表     
		 * 自动清理其 toast 表。                                   
		 */
		if (OidIsValid(classForm->reltoastrelid))
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map,
								 &classForm->reltoastrelid,
								 HASH_ENTER, &found);

			if (!found)
			{
				/* 键已由 hash_search 填好          */
				hentry->ar_relid = relid;
				hentry->ar_hasrelopts = false;
				if (relopts != NULL)
				{
					hentry->ar_hasrelopts = true;
					memcpy(&hentry->ar_reloptions, relopts,
						   sizeof(AutoVacOpts));
				}
			}
		}

		/* 释放资源，避免逐关系泄漏        */
		if (relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);

	/* 第二遍：检查 TOAST 表    */
	ScanKeyInit(&key,
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_TOASTVALUE));

	relScan = table_beginscan_catalog(classRel, 1, &key);
	while ((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatTabEntry *tabentry;
		Oid			relid;
		AutoVacOpts *relopts;
		bool		free_relopts = false;
		bool		dovacuum;
		bool		doanalyze;
		bool		wraparound;

		/*
		 * 无法安全处理其他后端的临时表，故跳过它们。   
		 */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		relid = classForm->oid;

		/*
		 * 获取 reloptions——若此 toast 表没有，则试主表       
		 *         
		 */
		relopts = extract_autovac_opts(tuple, pg_class_desc);
		if (relopts)
			free_relopts = true;
		else
		{
			av_relation *hentry;
			bool		found;

			hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
			if (found && hentry->ar_hasrelopts)
				relopts = &hentry->ar_reloptions;
		}

		/* 获取该表的 pgstat 条目         */
		tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
												  relid);

		relation_needs_vacanalyze(relid, relopts, classForm, tabentry,
								  effective_multixact_freeze_max_age,
								  &dovacuum, &doanalyze, &wraparound);

		/* 对 toast 表忽略 analyze     */
		if (dovacuum)
			table_oids = lappend_oid(table_oids, relid);

		/* 释放资源以免泄漏       */
		if (free_relopts)
			pfree(relopts);
		if (tabentry)
			pfree(tabentry);
	}

	table_endscan(relScan);
	table_close(classRel, AccessShareLock);

	/*
	 * 再次检查孤儿临时表，若仍像孤儿则删除。             
	 * 每删除一张表要耗用一个事务，这看似过度，      
	 * 但我们只应在前一个后端崩溃之后才需要做         
	 * 任何事，所以不应频繁到值得为此"优化"。         
	 * 使用分开的事务可确保：若有很多临时表待删除，
	 * 锁表不会膨胀；且若删除尝试失败，也不会丢失        
	 * 已完成的工作。                                              
	 */
	foreach(cell, orphan_oids)
	{
		Oid			relid = lfirst_oid(cell);
		Form_pg_class classForm;
		ObjectAddress object;

		/*
		 * 检查用户请求的中止。 
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 尝试锁定该表。若无法立即拿到锁，说明别人 
		 * 正在使用（或删除）此表，它已与我们无关。   
		 * 持有锁可防止下文的竞态条件。                       
		 */
		if (!ConditionalLockRelationOid(relid, AccessExclusiveLock))
			continue;

		/*
		 * 重新获取 pg_class 元组并复查它是否仍像              
		 * 一张孤儿临时表。若它不在了或不再是同一          
		 * 关系，跳过。  
		 */
		tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
		{
			/* 务必释放无用锁，以免锁表膨胀                */
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * 重复上面循环中做过的全部相同测试。发生       
		 * OID 计数器回卷时，现在的 pg_class 条目可能   
		 * 与之前看到的完全无关。             
		 */
		if (!((classForm->relkind == RELKIND_RELATION ||
			   classForm->relkind == RELKIND_MATVIEW) &&
			  classForm->relpersistence == RELPERSISTENCE_TEMP))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		if (checkTempNamespaceStatus(classForm->relnamespace) != TEMP_NAMESPACE_IDLE)
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/*
		 * 也要尝试锁定临时命名空间。即使已锁定该表，  
		 * 仍存在与某个进来的后端死锁的风险——该后端 
		 * 正试图清理临时命名空间，万一该表有依赖         
		 * （如序列），performDeletion 调用可能         
		 * 以不同顺序访问这些依赖。若能拿到命名空间     
		 * 的 AccessShareLock，就足以确保我们不会与             
		 * RemoveTempRelations 并发运行。若拿不到，则退避， 
		 * 让 RemoveTempRelations 去做它的处理。            
		 */
		if (!ConditionalLockDatabaseObject(NamespaceRelationId,
										   classForm->relnamespace, 0,
										   AccessShareLock))
		{
			UnlockRelationOid(relid, AccessExclusiveLock);
			continue;
		}

		/* 好，删除它     */
		ereport(LOG,
				(errmsg("autovacuum: dropping orphan temp table \"%s.%s.%s\"",
						get_database_name(MyDatabaseId),
						get_namespace_name(classForm->relnamespace),
						NameStr(classForm->relname))));

		/*
		 * 删除可能涉及 TOAST 表访问，因此确保存在       
		 * 有效快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		object.classId = RelationRelationId;
		object.objectId = relid;
		object.objectSubId = 0;
		performDeletion(&object, DROP_CASCADE,
						PERFORM_DELETION_INTERNAL |
						PERFORM_DELETION_QUIETLY |
						PERFORM_DELETION_SKIP_EXTENSIONS);

		/*
		 * 为提交此次删除，结束当前事务并启动新事务。
		 * 注意这也会释放我们取得的锁。      
		 */
		PopActiveSnapshot();
		CommitTransactionCommand();
		StartTransactionCommand();

		/* 内存上下文已被 StartTransactionCommand 改变   */
		MemoryContextSwitchTo(AutovacMemCxt);
	}

	/*
	 * 可选地为 VACUUM 建一个缓冲区访问策略对象。          
	 * 该 worker 清理的所有表都共用同一个 BufferAccessStrategy  
	 * 对象，以防自动清理撑爆共享缓冲区。               
	 *
	 * VacuumBufferUsageLimit 设为 0 会使          
	 * GetAccessStrategyWithSize 返回 NULL，实际上意味着          
	 * 最多可用尽共享缓冲区  
	 *
	 * 若稍后在被清理的任何表上进入 failsafe 模式，          
	 * 我们将仅对该表停止使用 BufferAccessStrategy。      
	 *
	 * XXX：若 VacuumBufferUsageLimit 改变，是否应考虑       
	 * 增加代码调整其大小？ 
	 */
	bstrategy = GetAccessStrategyWithSize(BAS_VACUUM, VacuumBufferUsageLimit);

	/*
	 * 创建一个内存上下文充当假的 PortalContext，这样   
	 * 清理代码中创建的上下文会针对每张表得到清理。
	 */
	PortalContext = AllocSetContextCreate(AutovacMemCxt,
										  "Autovacuum Portal",
										  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 对收集到的表执行操作。      
	 */
	foreach(cell, table_oids)
	{
		Oid			relid = lfirst_oid(cell);
		HeapTuple	classTup;
		autovac_table *tab;
		bool		isshared;
		bool		skipit;
		dlist_iter	iter;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 在处理每张收集到的表之前检查配置变更。       
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * 若见自动清理已禁用，你可能想就此退出。   
			 * 必须抵制该诱惑——它可能是回卷紧急      
			 * worker，那种情况下这样做完全不合适。         
			 *                        
			 */
		}

		/*
		 * 查明该表是否为共享表。（仅仅为此就去取   
		 * 系统缓存条目确实有点烦人，但典型情况下         
		 * 代价很小，因为 table_recheck_autovac 反正会        
		 * 重新取该条目。我们本可以在这里复制元组并    
		 * 把它传给 table_recheck_autovac 来换回这点开销， 
		 * 但那会增加该函数用到过期数据的几率。）    
		 */
		classTup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(classTup))
			continue;			/* 该关系已被别人删除，放弃*/
		isshared = ((Form_pg_class) GETSTRUCT(classTup))->relisshared;
		ReleaseSysCache(classTup);

		/*
		 * 从这里起持有调度锁，直到我们认领该表。      
		 * 遍历 worker 数组还需要 AutovacuumLock，不过该锁        
		 * 用共享锁即可。     
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		LWLockAcquire(AutovacuumLock, LW_SHARED);

		/*
		 * 检查该表是否正被其他 worker 并发清理。            
		 *        
		 */
		skipit = false;
		dlist_foreach(iter, &AutoVacuumShmem->av_runningWorkers)
		{
			WorkerInfo	worker = dlist_container(WorkerInfoData, wi_links, iter.cur);

			/* 忽略自身  */
			if (worker == MyWorkerInfo)
				continue;

			/* 忽略别的数据库的 worker（除非表共享）       */
			if (!worker->wi_sharedrel && worker->wi_dboid != MyDatabaseId)
				continue;

			if (worker->wi_tableoid == relid)
			{
				skipit = true;
				found_concurrent_worker = true;
				break;
			}
		}
		LWLockRelease(AutovacuumLock);
		if (skipit)
		{
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * 在释放调度锁之前，先把该表的 OID 存入共享
		 * 内存，这样其他 worker 就不会并发清理它。   
		 * （在此认领，以免复查统计信息期间  
		 * 持有 AutovacuumScheduleLock。）                
		 */
		MyWorkerInfo->wi_tableoid = relid;
		MyWorkerInfo->wi_sharedrel = isshared;
		LWLockRelease(AutovacuumScheduleLock);

		/*
		 * 检查 pgstat 数据是否仍表明我们需要清理该表。    
		 * 若趁我们不备时有别的什么处理了该表，数据     
		 * 可能已经变化。这并不能完全消除竞态条件，       
		 * 但窗口非常小。
		 */
		MemoryContextSwitchTo(AutovacMemCxt);
		tab = table_recheck_autovac(relid, table_toast_map, pg_class_desc,
									effective_multixact_freeze_max_age);
		if (tab == NULL)
		{
			/* 表已被别人清理过，或者表已消失    */
			LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
			MyWorkerInfo->wi_tableoid = InvalidOid;
			MyWorkerInfo->wi_sharedrel = false;
			LWLockRelease(AutovacuumScheduleLock);
			continue;
		}

		/*
		 * 把代价相关存储参数值保存进全局变量，以便在   
		 * 清理该表期间更新 vacuum_cost_delay                         
		 * 及 vacuum_cost_limit。    
		 */
		av_storage_param_cost_delay = tab->at_storage_param_vac_cost_delay;
		av_storage_param_cost_limit = tab->at_storage_param_vac_cost_limit;

		/*
		 * 我们只期望本 worker 会设置该标志，因此不必     
		 * 检查返回值。应该不需要重试。            
		 */
		if (tab->at_dobalance)
			pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
		else
			pg_atomic_clear_flag(&MyWorkerInfo->wi_dobalance);

		LWLockAcquire(AutovacuumLock, LW_SHARED);
		autovac_recalculate_workers_for_balance();
		LWLockRelease(AutovacuumLock);

		/*
		 * 我们等到此刻才更新代价延迟与代价限制      
		 * 值，尽管上文已重载过配置文件，这样才能       
		 * 把代价相关存储参数考虑进去。                        
		 */
		VacuumUpdateCosts();


		/* 每轮迭代之前清理内存        */
		MemoryContextResetAndDeleteChildren(PortalContext);

		/*
		 * 保存关系名以备可能的错误消息使用，避免      
		 * 出错时再做目录查找。若任何一个返回 NULL，      
		 * 说明该关系自上次检查后已被删除；跳过它。      
		 * 注意：它们必须保存在长寿命内存上下文中，因为 
		 * 清理与分析在不同事务中调用。   
		 */

		tab->at_relname = get_rel_name(tab->at_relid);
		tab->at_nspname = get_namespace_name(get_rel_namespace(tab->at_relid));
		tab->at_datname = get_database_name(MyDatabaseId);
		if (!tab->at_relname || !tab->at_nspname || !tab->at_datname)
			goto deleted;

		/*
		 * 若有什么出错，我们将中止对当前表的清理，      
		 * 并继续处理调度中的下一张表；特别是当         
		 * 我们被 SIGINT 中断时会这样。    
		 */
		PG_TRY();
		{
			/* 每表分配一律使用 PortalContext          */
			MemoryContextSwitchTo(PortalContext);

			/* 开干     */
			autovacuum_do_vac_analyze(tab, bstrategy);

			/*
			 * 清除可能存在的查询取消信号，以免对清理     
			 * 当前表时自动发出的信号反应滞后（表已  
			 * 处理完毕，此刻再取消毫无意义。）               
			 *                       
			 */
			QueryCancelPending = false;
		}
		PG_CATCH();
		{
			/*
			 * 中止事务并另起一个，接着处理列表中的      
			 * 下一张表。        
			 */
			HOLD_INTERRUPTS();
			if (tab->at_params.options & VACOPT_VACUUM)
				errcontext("automatic vacuum of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			else
				errcontext("automatic analyze of table \"%s.%s.%s\"",
						   tab->at_datname, tab->at_nspname, tab->at_relname);
			EmitErrorReport();

			/* 这同时复位 ProcGlobal->statusFlags[i] */
			AbortOutOfAnyTransaction();
			FlushErrorState();
			MemoryContextResetAndDeleteChildren(PortalContext);

			/* 为后续操作重启我们的事务                 */
			StartTransactionCommand();
			RESUME_INTERRUPTS();
		}
		PG_END_TRY();

		/* 确保回到 AutovacMemCxt 中        */
		MemoryContextSwitchTo(AutovacMemCxt);

		did_vacuum = true;

		/* ProcGlobal->statusFlags[i] 将在下次事务结束时复位 */

		/* be tidy */
deleted:
		if (tab->at_datname != NULL)
			pfree(tab->at_datname);
		if (tab->at_nspname != NULL)
			pfree(tab->at_nspname);
		if (tab->at_relname != NULL)
			pfree(tab->at_relname);
		pfree(tab);

		/*
		 * 从共享内存中移除本 worker 的信息。设置         
		 * wi_dobalance 的前提是：下一张要清理的表多半不带  
		 * 代价类存储参数，故要尽快认领 I/O 份额，以免  
		 * 抖动全局 balance。                                       
		 *         
		 */
		LWLockAcquire(AutovacuumScheduleLock, LW_EXCLUSIVE);
		MyWorkerInfo->wi_tableoid = InvalidOid;
		MyWorkerInfo->wi_sharedrel = false;
		LWLockRelease(AutovacuumScheduleLock);
		pg_atomic_test_set_flag(&MyWorkerInfo->wi_dobalance);
	}

	list_free(table_oids);

	/*
	 * 按后端的要求执行附加的工作项。           
	 */
	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (!workitem->avw_used)
			continue;
		if (workitem->avw_active)
			continue;
		if (workitem->avw_database != MyDatabaseId)
			continue;

		/* 认领此项，并在执行期间释放锁           */
		workitem->avw_active = true;
		LWLockRelease(AutovacuumLock);

		PushActiveSnapshot(GetTransactionSnapshot());
		perform_work_item(workitem);
		if (ActiveSnapshotSet())	/* 事务可能已中止          */
			PopActiveSnapshot();

		/*
		 * 在为后续作业取锁之前，先检查配置有无变化。 
		 */
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			VacuumUpdateCosts();
		}

		LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

		/* 并标记完成  */
		workitem->avw_active = false;
		workitem->avw_used = false;
	}
	LWLockRelease(AutovacuumLock);

	/*
	 * 我们会泄漏 table_toast_map（以及其他一些东西），  
	 * 但进程即将退出，无碍。   
	 */

	/*
	 * 更新 pg_database.datfrozenxid，并尽可能截断 pg_xact。      
	 * 只需做一次，不必每张表后都做。   
	 *
	 * 即使没有清理任何表，这一步仍然可能是必要的，
	 * 因为 vac_update_datfrozenxid() 的一个间接效果是           
	 * 更新 ShmemVariableCache->xidVacLimit。即便本次没有        
	 * 清理任何表，也可能仍需更新：因为带有            
	 * relfrozenxid 值更旧的关系，或 datfrozenxid 值更旧的其他
	 * 数据库可能已被删除，让 xidVacLimit 前进。  
	 *
	 * 但另一方面，也不能不分场合一概执行：当        
	 * autovacuum=off 时，这一步会重新启动 autovacuum              
	 * launcher。稍有不慎就会死循环：worker 发现无活         
	 * 可干，于是重启 launcher，后者又在同一数据库启动    
	 * 另一个同样无活可干的 worker。为避免这种情况，     
	 * 我们跳过这一步：（1）本次没发现要做的工作，  
	 * 且（2）因并发自动清理跳过至少一张表。另一个     
	 * worker 已经做过这件事，或将在结束时去做。 
	 */
	if (did_vacuum || !found_concurrent_worker)
		vac_update_datfrozenxid();

	/* 最后结束最后一个事务。       */
	CommitTransactionCommand();
}

/*
 * 执行一个此前注册的工作项。   
 */
static void
perform_work_item(AutoVacuumWorkItem *workitem)
{
	char	   *cur_datname = NULL;
	char	   *cur_nspname = NULL;
	char	   *cur_relname = NULL;

	/*
	 * 注意我们不把表信息存入 MyWorkerInfo，因为这并非  
	 * 真正清理。  
	 */

	/*
	 * 保存关系名，以备出错时给出错误消息，省去目录     
	 * 查找的开销。若任一调用返回 NULL，则关系自上次 
	 * 检查后已被删除；跳过它。                     
	 */
	Assert(CurrentMemoryContext == AutovacMemCxt);

	cur_relname = get_rel_name(workitem->avw_relation);
	cur_nspname = get_namespace_name(get_rel_namespace(workitem->avw_relation));
	cur_datname = get_database_name(MyDatabaseId);
	if (!cur_relname || !cur_nspname || !cur_datname)
		goto deleted2;

	autovac_report_workitem(workitem, cur_nspname, cur_relname);

	/* 每个工作项之前先清理内存  */
	MemoryContextResetAndDeleteChildren(PortalContext);

	/*
	 * 若出错就中止当前工作项，接着做下一个；尤其 
	 * 在收到 SIGINT 中断时便是如此。注意这意味着工作
	 * 项列表可能是有损耗的。                                     
	 *              
	 */
	PG_TRY();
	{
		/* 每工作项的分配都使用 PortalContext        */
		MemoryContextSwitchTo(PortalContext);

		/*
		 * 放手干活吧。此处调用的函数自行负责所需的用户 
		 * 切换与沙盒。      
		 */
		switch (workitem->avw_type)
		{
			case AVW_BRINSummarizeRange:
				DirectFunctionCall2(brin_summarize_range,
									ObjectIdGetDatum(workitem->avw_relation),
									Int64GetDatum((int64) workitem->avw_blockNumber));
				break;
			default:
				elog(WARNING, "unrecognized work item found: type %d",
					 workitem->avw_type);
				break;
		}

		/*
		 * 清除可能有的查询取消信号，以免对自动发出的  
		 * 信号反应过迟：该信号因清理当前表而发送（我们 
		 * 已处理完该表，此时取消已毫无意义。）          
		 *        
		 */
		QueryCancelPending = false;
	}
	PG_CATCH();
	{
		/*
		 * 中止事务并另起一个，接着处理列表中的           
		 * 下一张表。   
		 */
		HOLD_INTERRUPTS();
		errcontext("processing work entry for relation \"%s.%s.%s\"",
				   cur_datname, cur_nspname, cur_relname);
		EmitErrorReport();

		/* 这同时复位 ProcGlobal->statusFlags[i] */
		AbortOutOfAnyTransaction();
		FlushErrorState();
		MemoryContextResetAndDeleteChildren(PortalContext);

		/* 为后续操作重启我们的事务                 */
		StartTransactionCommand();
		RESUME_INTERRUPTS();
	}
	PG_END_TRY();

	/* 确保回到 AutovacMemCxt 中        */
	MemoryContextSwitchTo(AutovacMemCxt);

	/* 此处故意不设置 did_vacuum            */

	/* be tidy */
deleted2:
	if (cur_datname)
		pfree(cur_datname);
	if (cur_nspname)
		pfree(cur_nspname);
	if (cur_relname)
		pfree(cur_relname);
}

/*
 * extract_autovac_opts
 *
 * 给定关系的 pg_class 元组，若已设置 reloptions，    
 * 则返回 AutoVacOpts 部分的 palloc 副本；否则为 NULL。 
 *
 * 注意：此时调用者并未持有该表的关系锁，            
 * 所以在取得 pg_class 行之后，该表可能已被删除，其   
 * 目录行也随之消失。若 pg_class 有 TOAST 表，这将          
 * 会成为风险；幸好没有。  
 */
static AutoVacOpts *
extract_autovac_opts(HeapTuple tup, TupleDesc pg_class_desc)
{
	bytea	   *relopts;
	AutoVacOpts *av;

	Assert(((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_RELATION ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_MATVIEW ||
		   ((Form_pg_class) GETSTRUCT(tup))->relkind == RELKIND_TOASTVALUE);

	relopts = extractRelOptions(tup, pg_class_desc, NULL);
	if (relopts == NULL)
		return NULL;

	av = palloc(sizeof(AutoVacOpts));
	memcpy(av, &(((StdRdOptions *) relopts)->autovacuum), sizeof(AutoVacOpts));
	pfree(relopts);

	return av;
}


/*
 * table_recheck_autovac
 *
 * 重新检查一张表是否仍需要清理或分析。若需要则       
 * 返回有效 autovac_table 指针，否则为 NULL。  
 *
 * 注意返回的 autovac_table 并未设置名称字段。              
 */
static autovac_table *
table_recheck_autovac(Oid relid, HTAB *table_toast_map,
					  TupleDesc pg_class_desc,
					  int effective_multixact_freeze_max_age)
{
	Form_pg_class classForm;
	HeapTuple	classTup;
	bool		dovacuum;
	bool		doanalyze;
	autovac_table *tab = NULL;
	bool		wraparound;
	AutoVacOpts *avopts;
	bool		free_avopts = false;

	/* 取关系的 relcache 条目        */
	classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classTup))
		return NULL;
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	/*
	 * 取得适用的 reloptions。若是 TOAST 表，则在其自身       
	 * 没有时，尝试取主表的 reloptions。                 
	 */
	avopts = extract_autovac_opts(classTup, pg_class_desc);
	if (avopts)
		free_avopts = true;
	else if (classForm->relkind == RELKIND_TOASTVALUE &&
			 table_toast_map != NULL)
	{
		av_relation *hentry;
		bool		found;

		hentry = hash_search(table_toast_map, &relid, HASH_FIND, &found);
		if (found && hentry->ar_hasrelopts)
			avopts = &hentry->ar_reloptions;
	}

	recheck_relation_needs_vacanalyze(relid, avopts, classForm,
									  effective_multixact_freeze_max_age,
									  &dovacuum, &doanalyze, &wraparound);

	/* 好，这表需要处理。 */
	if (doanalyze || dovacuum)
	{
		int			freeze_min_age;
		int			freeze_table_age;
		int			multixact_freeze_min_age;
		int			multixact_freeze_table_age;
		int			log_min_duration;

		/*
		 * 计算清理代价参数与冻结年龄。若 pg_class.reloptions  
		 * 中设置了选项则用之；toast 表则再试主表。         
		 * 否则使用 GUC 默认值，先 autovacuum 自身的，   
		 * 后普通清理的。                                    
		 */

		/* 自动清理中 -1 表示用 log_autovacuum_min_duration    */
		log_min_duration = (avopts && avopts->log_min_duration >= 0)
			? avopts->log_min_duration
			: Log_autovacuum_min_duration;

		/* 这些没有自动清理专属设置           */
		freeze_min_age = (avopts && avopts->freeze_min_age >= 0)
			? avopts->freeze_min_age
			: default_freeze_min_age;

		freeze_table_age = (avopts && avopts->freeze_table_age >= 0)
			? avopts->freeze_table_age
			: default_freeze_table_age;

		multixact_freeze_min_age = (avopts &&
									avopts->multixact_freeze_min_age >= 0)
			? avopts->multixact_freeze_min_age
			: default_multixact_freeze_min_age;

		multixact_freeze_table_age = (avopts &&
									  avopts->multixact_freeze_table_age >= 0)
			? avopts->multixact_freeze_table_age
			: default_multixact_freeze_table_age;

		tab = palloc(sizeof(autovac_table));
		tab->at_relid = relid;
		tab->at_sharedrel = classForm->relisshared;

		/*
		 * 选择 VACUUM 选项。注意我们不设 VACOPT_PROCESS_TOAST， 
		 * 这样 vacuum() 便会跳过 toast 关系。另注意我们让     
		 * vacuum() 跳过 vac_update_datfrozenxid()；那步另做 
		 */
		tab->at_params.options =
			(dovacuum ? (VACOPT_VACUUM |
						 VACOPT_PROCESS_MAIN |
						 VACOPT_SKIP_DATABASE_STATS) : 0) |
			(doanalyze ? VACOPT_ANALYZE : 0) |
			(!wraparound ? VACOPT_SKIP_LOCKED : 0);

		/*
		 * 自动清理中 index_cleanup 与 truncate 起初未指定，     
		 * 稍后将依据其 reloptions（或 reloption 默认值）      
		 * 填入可用值。            
		 */
		tab->at_params.index_cleanup = VACOPTVALUE_UNSPECIFIED;
		tab->at_params.truncate = VACOPTVALUE_UNSPECIFIED;
		/* 目前自动清理还不支持并行清理                 */
		tab->at_params.nworkers = -1;
		tab->at_params.freeze_min_age = freeze_min_age;
		tab->at_params.freeze_table_age = freeze_table_age;
		tab->at_params.multixact_freeze_min_age = multixact_freeze_min_age;
		tab->at_params.multixact_freeze_table_age = multixact_freeze_table_age;
		tab->at_params.is_wraparound = wraparound;
		tab->at_params.log_min_duration = log_min_duration;
		tab->at_storage_param_vac_cost_limit = avopts ?
			avopts->vacuum_cost_limit : 0;
		tab->at_storage_param_vac_cost_delay = avopts ?
			avopts->vacuum_cost_delay : -1;
		tab->at_relname = NULL;
		tab->at_nspname = NULL;
		tab->at_datname = NULL;

		/*
		 * 若该表的任一代价延迟参数单独设置过，则禁用  
		 * 平衡算法。                             
		 */
		tab->at_dobalance =
			!(avopts && (avopts->vacuum_cost_limit > 0 ||
						 avopts->vacuum_cost_delay >= 0));
	}

	if (free_avopts)
		pfree(avopts);
	heap_freetuple(classTup);
	return tab;
}

/*
 * recheck_relation_needs_vacanalyze
 *
 * table_recheck_autovac 子例程。   
 *
 * 获取某关系的 pgstat，重新检查其是否需要清理
 * 或分析。                     
 */
static void
recheck_relation_needs_vacanalyze(Oid relid,
								  AutoVacOpts *avopts,
								  Form_pg_class classForm,
								  int effective_multixact_freeze_max_age,
								  bool *dovacuum,
								  bool *doanalyze,
								  bool *wraparound)
{
	PgStat_StatTabEntry *tabentry;

	/* 取表的 pgstat 条目      */
	tabentry = pgstat_fetch_stat_tabentry_ext(classForm->relisshared,
											  relid);

	relation_needs_vacanalyze(relid, avopts, classForm, tabentry,
							  effective_multixact_freeze_max_age,
							  dovacuum, doanalyze, wraparound);

	/* 释放 tabentry 以免泄漏      */
	if (tabentry)
		pfree(tabentry);

	/* toast 表忽略 ANALYZE         */
	if (classForm->relkind == RELKIND_TOASTVALUE)
		*doanalyze = false;
}

/*
 * relation_needs_vacanalyze
 *
 * 检查某关系是否需要清理或分析；结果分别放入            
 * "dovacuum" 与 "doanalyze"。同时返回清理是否因                    
 * Xid 或 multixact 回卷而被强制执行。       
 *
 * relopts 是指向 AutoVacOpts 选项的指针（普通表时指          
 * 向自身；TOAST 表时指向自身或其父表），没有则为         
 * NULL；tabentry 是 pgstats 条目，可以为 NULL。                      
 *      
 *
 * 若死元组数超过某个阈值，表就需要清理。该阈值 
 * 的计算公式为                         
 *
 * threshold = vac_base_thresh + vac_scale_factor * reltuples
 *
 * 对于分析，所做的分析是：自上次分析以来插入、删
 * 除与更新的元组数超过一个按上述同样方式算出的       
 * 阈值。注意累积统计系统存储的是截至上次分析时         
 * 存在的元组数（活元组与死元组皆计）。这与            
 * VACUUM 的情形不对称。                    
 *
 * 此外，若表的 relfrozenxid 落后超过 freeze_max_age 个事务，    
 * 且 relminmxid 落后超过 multixact_freeze_max_age 
 * 个 multixact 即须强制。            
 *
 * autovacuum_enabled 为 false 的表自动跳过    
 * （除非因 freeze_max_age 而必须清理它）。                      
 * 因此可以针对特定的表禁用自动清理。另外，当累积         
 * 统计系统没有某表的数据时，该表也会被跳过。   
 *
 * 若表的 vac_base_thresh 值 < 0，则基准值取自                  
 * autovacuum_vacuum_threshold GUC 变量。类似地，                   
 * vac_scale_factor 值 < 0 时会代入     
 * autovacuum_vacuum_scale_factor 的值；分析同理。         
 */
static void
relation_needs_vacanalyze(Oid relid,
						  AutoVacOpts *relopts,
						  Form_pg_class classForm,
						  PgStat_StatTabEntry *tabentry,
						  int effective_multixact_freeze_max_age,
 /* 输出参数如下  */
						  bool *dovacuum,
						  bool *doanalyze,
						  bool *wraparound)
{
	bool		force_vacuum;
	bool		av_enabled;
	float4		reltuples;		/* pg_class.reltuples */

	/* 取自 reloptions 或 GUC 变量的常量  */
	int			vac_base_thresh,
				vac_ins_base_thresh,
				anl_base_thresh;
	float4		vac_scale_factor,
				vac_ins_scale_factor,
				anl_scale_factor;

	/* 由上述常量算出的阈值             */
	float4		vacthresh,
				vacinsthresh,
				anlthresh;

	/* 当前清理（或分析）元组数                 */
	float4		vactuples,
				instuples,
				anltuples;

	/* 冻结参数      */
	int			freeze_max_age;
	int			multixact_freeze_max_age;
	TransactionId xidForceLimit;
	MultiXactId multiForceLimit;

	Assert(classForm != NULL);
	Assert(OidIsValid(relid));

	/*
	 * 确定清理/分析方程的参数。来源有两个：传入的   
	 * reloptions（主表或 toast 表的），或 autovacuum 的           
	 * GUC 变量。                           
	 */

	/* 自动清理中 -1 表示用普通 vacuum_scale_factor    */
	vac_scale_factor = (relopts && relopts->vacuum_scale_factor >= 0)
		? relopts->vacuum_scale_factor
		: autovacuum_vac_scale;

	vac_base_thresh = (relopts && relopts->vacuum_threshold >= 0)
		? relopts->vacuum_threshold
		: autovacuum_vac_thresh;

	vac_ins_scale_factor = (relopts && relopts->vacuum_ins_scale_factor >= 0)
		? relopts->vacuum_ins_scale_factor
		: autovacuum_vac_ins_scale;

	/* -1 用于禁用插入清理          */
	vac_ins_base_thresh = (relopts && relopts->vacuum_ins_threshold >= -1)
		? relopts->vacuum_ins_threshold
		: autovacuum_vac_ins_thresh;

	anl_scale_factor = (relopts && relopts->analyze_scale_factor >= 0)
		? relopts->analyze_scale_factor
		: autovacuum_anl_scale;

	anl_base_thresh = (relopts && relopts->analyze_threshold >= 0)
		? relopts->analyze_threshold
		: autovacuum_anl_thresh;

	freeze_max_age = (relopts && relopts->freeze_max_age >= 0)
		? Min(relopts->freeze_max_age, autovacuum_freeze_max_age)
		: autovacuum_freeze_max_age;

	multixact_freeze_max_age = (relopts && relopts->multixact_freeze_max_age >= 0)
		? Min(relopts->multixact_freeze_max_age, effective_multixact_freeze_max_age)
		: effective_multixact_freeze_max_age;

	av_enabled = (relopts ? relopts->enabled : true);

	/* 若表有回卷风险则强制清理           */
	xidForceLimit = recentXid - freeze_max_age;
	if (xidForceLimit < FirstNormalTransactionId)
		xidForceLimit -= FirstNormalTransactionId;
	force_vacuum = (TransactionIdIsNormal(classForm->relfrozenxid) &&
					TransactionIdPrecedes(classForm->relfrozenxid,
										  xidForceLimit));
	if (!force_vacuum)
	{
		multiForceLimit = recentMulti - multixact_freeze_max_age;
		if (multiForceLimit < FirstMultiXactId)
			multiForceLimit -= FirstMultiXactId;
		force_vacuum = MultiXactIdIsValid(classForm->relminmxid) &&
			MultiXactIdPrecedes(classForm->relminmxid, multiForceLimit);
	}
	*wraparound = force_vacuum;

	/* 用户在 pg_class.reloptions 禁用它？（有风险则忽略）*/
	if (!av_enabled && !force_vacuum)
	{
		*doanalyze = false;
		*dovacuum = false;
		return;
	}

	/*
	 * 如果在统计中找到了该表，且当前                        
	 * autovacuum 开启，则按阈值决定是否清理                   
	 * 和/或分析。若自动清理已关闭，我们在此只为防     
	 * 回卷清理：凡未被强制的不清理（不分析）。          
	 *        
	 */
	if (PointerIsValid(tabentry) && AutoVacuumingActive())
	{
		reltuples = classForm->reltuples;
		vactuples = tabentry->dead_tuples;
		instuples = tabentry->ins_since_vacuum;
		anltuples = tabentry->mod_since_analyze;

		/* 若表从未被清理过，则把 reltuples 当作零         */
		if (reltuples < 0)
			reltuples = 0;

		vacthresh = (float4) vac_base_thresh + vac_scale_factor * reltuples;
		vacinsthresh = (float4) vac_ins_base_thresh + vac_ins_scale_factor * reltuples;
		anlthresh = (float4) anl_base_thresh + anl_scale_factor * reltuples;

		/*
		 * 注意无需特别考虑统计重置，因为一旦发生     
		 * 重置，上次清理和分析的计数也会                     
		 * 一并归零。   
		 */
		if (vac_ins_base_thresh >= 0)
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: %.0f (threshold %.0f), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, instuples, vacinsthresh, anltuples, anlthresh);
		else
			elog(DEBUG3, "%s: vac: %.0f (threshold %.0f), ins: (disabled), anl: %.0f (threshold %.0f)",
				 NameStr(classForm->relname),
				 vactuples, vacthresh, anltuples, anlthresh);

		/* 判断该表是否需要清理或分析。       */
		*dovacuum = force_vacuum || (vactuples > vacthresh) ||
			(vac_ins_base_thresh >= 0 && instuples > vacinsthresh);
		*doanalyze = (anltuples > anlthresh);
	}
	else
	{
		/*
		 * 跳过统计哈希中没找到的表，除非为防回卷必须    
		 * 强制清理。既然无人处理它，也就无需清理。       
		 *           
		 */
		*dovacuum = force_vacuum;
		*doanalyze = false;
	}

	/* ANALYZE 拒绝处理 pg_statistic         */
	if (relid == StatisticRelationId)
		*doanalyze = false;
}

/*
 * autovacuum_do_vac_analyze
 *		对指定表执行清理和/或分析    
 *
 * 预期调用者已切换到不会随事务提交而消失的内存    
 * 上下文中。                 
 */
static void
autovacuum_do_vac_analyze(autovac_table *tab, BufferAccessStrategy bstrategy)
{
	RangeVar   *rangevar;
	VacuumRelation *rel;
	List	   *rel_list;
	MemoryContext vac_context;
	MemoryContext old_context;

	/* 告知 pgstat 我们在做什么 */
	autovac_report_activity(tab);

	/* 创建一个供 vacuum() 用作跨事务存储的上下文          */
	vac_context = AllocSetContextCreate(CurrentMemoryContext,
										"Vacuum",
										ALLOCSET_DEFAULT_SIZES);

	/* 为 vacuum() 建一个以 OID 标识的 VacuumRelation 目标     */
	old_context = MemoryContextSwitchTo(vac_context);
	rangevar = makeRangeVar(tab->at_nspname, tab->at_relname, -1);
	rel = makeVacuumRelation(rangevar, tab->at_relid, NIL);
	rel_list = list_make1(rel);
	MemoryContextSwitchTo(old_context);

	vacuum(rel_list, &tab->at_params, bstrategy, vac_context, true);

	MemoryContextDelete(vac_context);
}

/*
 * autovac_report_activity
 *		向 pgstat 报告自动清理的动向   
 *
 * 我们发送一条 SQL 字符串，相当于用户手动执行      
 * 等价命令时会看到的内容。         
 *
 * 注意：我们假定当前命令一完成就会上报下一条，          
 * 最后一条做完就退出，所以懒得上报"<IDLE>"之类的        
 * 状态。                              
 */
static void
autovac_report_activity(autovac_table *tab)
{
#define MAX_AUTOVAC_ACTIV_LEN (NAMEDATALEN * 2 + 56)
	char		activity[MAX_AUTOVAC_ACTIV_LEN];
	int			len;

	/* 上报命令及可能的选项          */
	if (tab->at_params.options & VACOPT_VACUUM)
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: VACUUM%s",
				 tab->at_params.options & VACOPT_ANALYZE ? " ANALYZE" : "");
	else
		snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
				 "autovacuum: ANALYZE");

	/*
	 * 上报关系的限定名。               
	 */
	len = strlen(activity);

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", tab->at_nspname, tab->at_relname,
			 tab->at_params.is_wraparound ? " (to prevent wraparound)" : "");

	/* pg_stat_activity：statement_timestamp() 设为当前时间    */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * autovac_report_workitem
 *		向 pgstat 报告自动清理正在处理工作项        
 */
static void
autovac_report_workitem(AutoVacuumWorkItem *workitem,
						const char *nspname, const char *relname)
{
	char		activity[MAX_AUTOVAC_ACTIV_LEN + 12 + 2];
	char		blk[12 + 2];
	int			len;

	switch (workitem->avw_type)
	{
		case AVW_BRINSummarizeRange:
			snprintf(activity, MAX_AUTOVAC_ACTIV_LEN,
					 "autovacuum: BRIN summarize");
			break;
	}

	/*
	 * 上报关系的限定名，如有块号则一并上报                
	 */
	len = strlen(activity);

	if (BlockNumberIsValid(workitem->avw_blockNumber))
		snprintf(blk, sizeof(blk), " %u", workitem->avw_blockNumber);
	else
		blk[0] = '\0';

	snprintf(activity + len, MAX_AUTOVAC_ACTIV_LEN - len,
			 " %s.%s%s", nspname, relname, blk);

	/* pg_stat_activity：statement_timestamp() 设为当前时间    */
	SetCurrentStatementStartTimestamp();

	pgstat_report_activity(STATE_RUNNING, activity);
}

/*
 * AutoVacuumingActive
 *		检查 GUC 变量并报告自动清理进程是否应当运行    
 *		        
 */
bool
AutoVacuumingActive(void)
{
	if (!autovacuum_start_daemon || !pgstat_track_counts)
		return false;
	return true;
}

/*
 * 为下一次处理我们数据库的自动清理运行请求一个       
 * 工作项，无法记录时返回 false。    
 */
bool
AutoVacuumRequestWork(AutoVacuumWorkItemType type, Oid relationId,
					  BlockNumber blkno)
{
	int			i;
	bool		result = false;

	LWLockAcquire(AutovacuumLock, LW_EXCLUSIVE);

	/*
	 * 找一个空闲工作项，填入给定数据。           
	 */
	for (i = 0; i < NUM_WORKITEMS; i++)
	{
		AutoVacuumWorkItem *workitem = &AutoVacuumShmem->av_workItems[i];

		if (workitem->avw_used)
			continue;

		workitem->avw_used = true;
		workitem->avw_active = false;
		workitem->avw_type = type;
		workitem->avw_database = MyDatabaseId;
		workitem->avw_relation = relationId;
		workitem->avw_blockNumber = blkno;
		result = true;

		/* done */
		break;
	}

	LWLockRelease(AutovacuumLock);

	return result;
}

/*
 * autovac_init
 *		在 postmaster 初始化时被调用。     
 *
 * 这里只是用户配错时烦他一下。          
 */
void
autovac_init(void)
{
	if (autovacuum_start_daemon && !pgstat_track_counts)
		ereport(WARNING,
				(errmsg("autovacuum not started because of misconfiguration"),
				 errhint("Enable the \"track_counts\" option.")));
}

/*
 * IsAutoVacuum 函数   
 *		返回当前是否为自动清理 launcher 或 worker 进程。        
 *		        
 */
bool
IsAutoVacuumLauncherProcess(void)
{
	return am_autovacuum_launcher;
}

bool
IsAutoVacuumWorkerProcess(void)
{
	return am_autovacuum_worker;
}


/*
 * AutoVacuumShmemSize
 *		计算自动清理相关共享内存所需的空间      
 */
Size
AutoVacuumShmemSize(void)
{
	Size		size;

	/*
	 * 需要固定结构体和 WorkerInfoData 数组。     
	 */
	size = sizeof(AutoVacuumShmemStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(autovacuum_max_workers,
								   sizeof(WorkerInfoData)));
	return size;
}

/*
 * AutoVacuumShmemInit
 *		分配并初始化自动清理相关共享内存        
 */
void
AutoVacuumShmemInit(void)
{
	bool		found;

	AutoVacuumShmem = (AutoVacuumShmemStruct *)
		ShmemInitStruct("AutoVacuum Data",
						AutoVacuumShmemSize(),
						&found);

	if (!IsUnderPostmaster)
	{
		WorkerInfo	worker;
		int			i;

		Assert(!found);

		AutoVacuumShmem->av_launcherpid = 0;
		dlist_init(&AutoVacuumShmem->av_freeWorkers);
		dlist_init(&AutoVacuumShmem->av_runningWorkers);
		AutoVacuumShmem->av_startingWorker = NULL;
		memset(AutoVacuumShmem->av_workItems, 0,
			   sizeof(AutoVacuumWorkItem) * NUM_WORKITEMS);

		worker = (WorkerInfo) ((char *) AutoVacuumShmem +
							   MAXALIGN(sizeof(AutoVacuumShmemStruct)));

		/* 初始化 WorkerInfo 空闲链表   */
		for (i = 0; i < autovacuum_max_workers; i++)
		{
			dlist_push_head(&AutoVacuumShmem->av_freeWorkers,
							&worker[i].wi_links);
			pg_atomic_init_flag(&worker[i].wi_dobalance);
		}

		pg_atomic_init_u32(&AutoVacuumShmem->av_nworkersForBalance, 0);

	}
	else
		Assert(found);
}

/*
 * autovacuum_work_mem 的 GUC check_hook
 */
bool
check_autovacuum_work_mem(int *newval, void **extra, GucSource source)
{
	/*
	 * -1 表示回退。    
	 *
	 * 若还未改动 -1 的 boot_val 默认值，就让它保持原样。 
	 * 自动清理会转用 maintenance_work_mem。        
	 */
	if (*newval == -1)
		return true;

	/*
	 * 手动设置的值钳制到至少 1MB。因为      
	 * maintenance_work_mem 总是至少设为该值，这里照做。       
	 *      
	 */
	if (*newval < 1024)
		*newval = 1024;

	return true;
}
