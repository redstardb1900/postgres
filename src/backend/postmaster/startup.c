/*-------------------------------------------------------------------------
 *
 * startup.c
 *
 * startup 进程负责初始化服务器，并执行已指定的      
 * 各种恢复动作。注意 startup 进程没有"主循环"，       
 * 因为初始化一完成它就结束了。（在备库模式下，   
 * 可以把重放循环视为主循环。）                        
 *         
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timeout.h"


#ifndef USE_POSTMASTER_DEATH_SIGNAL
/*
 * 在需要用系统调用检查 postmaster 是否已消失的系统上，    
 * 唯有每第 N 次调用 HandleStartupProcInterrupts() 才真正执行。   
 * 这只影响我们忙于重放 WAL 时发现该状况所需的时长。   
 * 而锁存器等待等仍会经由惯用手段立即响应，               
 * 不受此限制。           
 */
#define POSTMASTER_POLL_RATE_LIMIT 1024
#endif

/*
 * 由中断处理器设置、供重放循环稍后处理的标志。 
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;
static volatile sig_atomic_t promote_signaled = false;

/*
 * 正在执行 restore 命令时置位，告知 SIGTERM 信号处理器    
 * 此时直接 proc_exit 即可。 
 */
static volatile sig_atomic_t in_restore_command = false;

/*
 * 最近一次 startup 操作的开始时间。           
 */
static TimestampTz startup_progress_phase_start_time;

/*
 * 指示用户所设 startup 进度间隔是否已过去。               
 * 超时发生则为 TRUE，否则为 FALSE。              
 */
static volatile sig_atomic_t startup_progress_timer_expired = false;

/*
 * 长时间运行的 startup 操作的进度更新间隔。         
 */
int			log_startup_progress_interval = 10000;	/* 10 秒 */

/* 信号处理器 */
static void StartupProcTriggerHandler(SIGNAL_ARGS);
static void StartupProcSigHupHandler(SIGNAL_ARGS);

/* 回调    */
static void StartupProcExit(int code, Datum arg);


/* --------------------------------
 *		信号处理器例程  
 * --------------------------------
 */

/* SIGUSR2: 置位标志以结束恢复 */
static void
StartupProcTriggerHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	promote_signaled = true;
	WakeupRecovery();

	errno = save_errno;
}

/* SIGHUP: 置位标志，待方便时重新读取配置文件     */
static void
StartupProcSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	WakeupRecovery();

	errno = save_errno;
}

/* SIGTERM: 置标志中止重放并退出  */
static void
StartupProcShutdownHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (in_restore_command)
	{
		/*
		 * 若我们在子进程中（例如 RestoreArchivedFile()  
		 * 里的 system() fork），就不应调用任何退出回调，   
		 * 父进程会负责处理。       
		 */
		if (MyProcPid == (int) getpid())
			proc_exit(1);
		else
		{
			write_stderr_signal_safe("StartupProcShutdownHandler() called in child process\n");
			_exit(1);
		}
	}
	else
		shutdown_requested = true;
	WakeupRecovery();

	errno = save_errno;
}

/*
 * 重读配置文件。   
 *
 * 若关键 walreceiver 选项之一发生变化，就通知 xlog.c  
 * 重启它。  
 */
static void
StartupRereadConfig(void)
{
	char	   *conninfo = pstrdup(PrimaryConnInfo);
	char	   *slotname = pstrdup(PrimarySlotName);
	bool		tempSlot = wal_receiver_create_temp_slot;
	bool		conninfoChanged;
	bool		slotnameChanged;
	bool		tempSlotChanged = false;

	ProcessConfigFile(PGC_SIGHUP);

	conninfoChanged = strcmp(conninfo, PrimaryConnInfo) != 0;
	slotnameChanged = strcmp(slotname, PrimarySlotName) != 0;

	/*
	 * wal_receiver_create_temp_slot 仅在我们没有配置         
	 * 复制槽时才用。若改动无实效，则无需跟踪。         
	 */
	if (!slotnameChanged && strcmp(PrimarySlotName, "") == 0)
		tempSlotChanged = tempSlot != wal_receiver_create_temp_slot;
	pfree(conninfo);
	pfree(slotname);

	if (conninfoChanged || slotnameChanged || tempSlotChanged)
		StartupRequestWalReceiverRestart();
}

/* 处理可能发给 startup 进程的各种信号                 */
void
HandleStartupProcInterrupts(void)
{
#ifdef POSTMASTER_POLL_RATE_LIMIT
	static uint32 postmaster_poll_count = 0;
#endif

	/*
	 * 处理近期收到的任何请求或信号。     
	 */
	if (got_SIGHUP)
	{
		got_SIGHUP = false;
		StartupRereadConfig();
	}

	/*
	 * 检查是否被要求在不完成恢复的情况下退出。  
	 */
	if (shutdown_requested)
		proc_exit(1);

	/*
	 * 若 postmaster 已死亡则紧急退出。这是为了避免   
	 * 手动清理所有 postmaster 子进程的麻烦。                   
	 * 没有廉价信号机制的系统上此项检查做得较少。   
	 *       
	 */
	if (IsUnderPostmaster &&
#ifdef POSTMASTER_POLL_RATE_LIMIT
		postmaster_poll_count++ % POSTMASTER_POLL_RATE_LIMIT == 0 &&
#endif
		!PostmasterIsAlive())
		exit(1);

	/* 处理屏障事件     */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* 记录本进程的内存上下文                  */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}


/* --------------------------------
 *		信号处理器例程  
 * --------------------------------
 */
static void
StartupProcExit(int code, Datum arg)
{
	/* 关闭恢复环境                */
	if (standbyState != STANDBY_DISABLED)
		ShutdownRecoveryTransactionEnvironment();
}


/* ----------------------------------
 *	startup 进程主入口点      
 * ----------------------------------
 */
void
StartupProcessMain(void)
{
	/* 安排在 startup 进程退出时清理     */
	on_shmem_exit(StartupProcExit, 0);

	/*
	 * 恰当地接受或忽略 postmaster 可能发来的信号。   
	 */
	pqsignal(SIGHUP, StartupProcSigHupHandler); /* 重载配置文件 */
	pqsignal(SIGINT, SIG_IGN);	/* 忽略查询取消  */
	pqsignal(SIGTERM, StartupProcShutdownHandler);	/* 请求关闭     */
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */
	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, StartupProcTriggerHandler);

	/*
	 * 重置一些 postmaster 接受但此处不接受的信号      
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 注册备库模式所需的超时        
	 */
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_TIMEOUT, StandbyTimeoutHandler);
	RegisterTimeout(STANDBY_LOCK_TIMEOUT, StandbyLockTimeoutHandler);

	/*
	 * 解除信号阻塞（postmaster fork 我们时阻塞了信号） 
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 做本职工作。  
	 */
	StartupXLOG();

	/*
	 * 正常退出。退出码 0 告知 postmaster 恢复成功完成。    
	 *              
	 */
	proc_exit(0);
}

void
PreRestoreCommand(void)
{
	/*
	 * 置位 in_restore_command，告知信号处理器收到 SIGTERM      
	 * 时应立即退出。我们知道自己正处于可这样做         
	 * 的安全点。还要检查是否已收到过该信号，以免     
	 * 漏掉在此之前收到的关闭请求。 
	 */
	in_restore_command = true;
	if (shutdown_requested)
		proc_exit(1);
}

void
PostRestoreCommand(void)
{
	in_restore_command = false;
}

bool
IsPromoteSignaled(void)
{
	return promote_signaled;
}

void
ResetPromoteSignaled(void)
{
	promote_signaled = false;
}

/*
 * 置位标志，表明到了记录进度报告的时机。     
 */
void
startup_progress_timeout_handler(void)
{
	startup_progress_timer_expired = true;
}

void
disable_startup_progress_timeout(void)
{
	/* 特性已禁用。   */
	if (log_startup_progress_interval == 0)
		return;

	disable_timeout(STARTUP_PROGRESS_TIMEOUT, false);
	startup_progress_timer_expired = false;
}

/*
 * 设置当前操作的开始时间戳并启用超时。                  
 */
void
enable_startup_progress_timeout(void)
{
	TimestampTz fin_time;

	/* 特性已禁用。   */
	if (log_startup_progress_interval == 0)
		return;

	startup_progress_phase_start_time = GetCurrentTimestamp();
	fin_time = TimestampTzPlusMilliseconds(startup_progress_phase_start_time,
										   log_startup_progress_interval);
	enable_timeout_every(STARTUP_PROGRESS_TIMEOUT, fin_time,
						 log_startup_progress_interval);
}

/*
 * 一个轻量封装：先禁用再启用 startup 进度超时。     
 *         
 */
void
begin_startup_progress_phase(void)
{
	/* 特性已禁用。   */
	if (log_startup_progress_interval == 0)
		return;

	disable_startup_progress_timeout();
	enable_startup_progress_timeout();
}

/*
 * 报告 startup 进度超时是否发生。是则重置定时器标志，  
 * 把经过时间写入出参并返回 true，否则返回 false。     
 *                        
 */
bool
has_startup_progress_timeout_expired(long *secs, int *usecs)
{
	long		seconds;
	int			useconds;
	TimestampTz now;

	/* 未发生超时。       */
	if (!startup_progress_timer_expired)
		return false;

	/* 计算经过的时间。    */
	now = GetCurrentTimestamp();
	TimestampDifference(startup_progress_phase_start_time, now, &seconds, &useconds);

	*secs = seconds;
	*usecs = useconds;
	startup_progress_timer_expired = false;

	return true;
}
