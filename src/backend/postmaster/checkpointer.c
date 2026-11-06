/*-------------------------------------------------------------------------
 *
 * checkpointer.c
 *
 * 检查点进程是 Postgres 9.2 新增的，负责所有检查点。    
 * 检查点会在距上次检查点经过一定时间后自动触发，      
 * 也可通过信号触发执行所请求的检查点。                 
 * （每隔若干 WAL 段强制检查点的 GUC 参数，其实现方式是
 * 由后端在填满 WAL 段时发送信号，检查点进程并不监视
 * 这一情况。）                                              
 *            
 *
 * 正常终止靠 SIGUSR2，它指示检查点进程                   
 * 先执行关闭检查点再 exit(0)。（必须先停掉所有后端  
 * 再发 SIGUSR2！）紧急终止则靠 SIGQUIT；                       
 * 与其他后端一样，收到 SIGQUIT 即中止并退出。             
 *
 * 若检查点进程意外退出，postmaster 会将其视同后端崩溃： 
 * 共享内存可能已损坏，因此要用 SIGQUIT 杀掉其余后端， 
 * 随后启动一轮恢复周期。（即便                              
 * 共享内存并未损坏，我们也丢失了哪些文件          
 * 需要为下次检查点 fsync 的信息，故须强制重启） 
 *                             
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/checkpointer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>
#include <time.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "replication/syncrep.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*----------
 * 检查点进程与后端之间通信的共享内存区                
 *
 * ckpt 计数器让后端能够监视其发出的检查点                
 * 请求何时完成。机制如下：    
 *	* 检查点开始时，检查点进程读取（并清除）请求      
 *	  标志并递增 ckpt_started，同时持有 ckpt_lck。   
 *	* 检查点完成时，把 ckpt_done 设                       
 *	  为 ckpt_started。
 *	* 检查点失败时，检查点进程递增 ckpt_failed         
 *	  并把 ckpt_done 置为 ckpt_started。  
 *
 * 后端侧的算法是：      
 *	1. 记录 ckpt_failed 与 ckpt_started 的当前值，       
 *	   持有 ckpt_lck 时设置请求标志。  
 *	2. 发信号请求检查点。       
 *	3. 休眠直到 ckpt_started 变化。此时你知道自           
 *	   你启动本算法以来已有检查点开始（尽管未必      
 *	   正是你的信号触发），且该检查点正使用你的标志。   
 *	4. 记录 ckpt_started 的新值。 
 *	5. 休眠直到 ckpt_done >= ckpt_started 的保存值               
 *	   。（用模运算防计数器回绕。）                    
 *	   此时已知检查点开始并完成，但不知是否      
 *	   成功。  
 *	6. 若 ckpt_failed 与最初保存的值不同，              
 *	   则认为请求失败；否则必定成功。                 
 *
 * ckpt_flags 保存自上次检查点开始以来所有              
 * 请求后端所发检查点请求标志的按位或。这些       
 * 标志的设计使按位或成为合并多个请求的正确方式。 
 *
 * num_backend_writes 用于统计用户后端进程执行的               
 * 缓冲区写出次数。该计数器应足够宽，确保在单个    
 * 处理周期内不会溢出。num_backend_fsync 统计             
 * 其中必须自行执行 fsync 的那部分写出，                  
 * 因为检查点进程未能吸收其请求。           
 *
 * requests 数组保存后端已发出但尚未被检查点           
 * 进程吸收的 fsync 请求 
 *
 * 与检查点字段不同，num_backend_writes、                        
 * 及 requests 字段由 CheckpointerCommLock 保护。     
 *----------
 */
typedef struct
{
	SyncRequestType type;		/* 请求类型 */
	FileTag		ftag;			/* 文件标识符 */
} CheckpointerRequest;

typedef struct
{
	pid_t		checkpointer_pid;	/* PID（未启动为 0）*/

	slock_t		ckpt_lck;		/* 保护所有 ckpt_* 字段     */

	int			ckpt_started;	/* 检查点开始时递增        */
	int			ckpt_done;		/* 检查点完成时递增      */
	int			ckpt_failed;	/* 检查点失败时递增       */

	int			ckpt_flags;		/* 检查点标志，定义见 xlog.h     */

	ConditionVariable start_cv; /* ckpt_started 前进时发信号     */
	ConditionVariable done_cv;	/* ckpt_done 前进时发信号     */

	uint32		num_backend_writes; /* 统计用户后端缓冲区写出 */
	uint32		num_backend_fsync;	/* 统计用户后端 fsync 调用 */

	int			num_requests;	/* 当前请求数       */
	int			max_requests;	/* 已分配数组大小*/
	CheckpointerRequest requests[FLEXIBLE_ARRAY_MEMBER];
} CheckpointerShmemStruct;

static CheckpointerShmemStruct *CheckpointerShmem;

/* 在 CheckpointWriteDelay 中调用 AbsorbSyncRequests 的间隔 */
#define WRITES_PER_ABSORB		1000

/* 检查点进程请求队列可容纳的最大请求数         */
#define MAX_CHECKPOINT_REQUESTS 10000000

/*
 * GUC 参数    
 */
int			CheckPointTimeout = 300;
int			CheckPointWarning = 30;
double		CheckPointCompletionTarget = 0.9;

/*
 * 私有状态 
 */
static bool ckpt_active = false;

/* 这些值仅在 ckpt_active 为真时有效：   */
static pg_time_t ckpt_start_time;
static XLogRecPtr ckpt_start_recptr;
static double ckpt_cached_elapsed;

static pg_time_t last_checkpoint_time;
static pg_time_t last_xlog_switch_time;

/* 私有函数的原型声明      */

static void HandleCheckpointerInterrupts(void);
static void CheckArchiveTimeout(void);
static bool IsCheckpointOnSchedule(double progress);
static bool ImmediateCheckpointRequested(void);
static bool CompactCheckpointerRequestQueue(void);
static void UpdateSharedMemoryConfig(void);

/* 信号处理器 */
static void ReqCheckpointHandler(SIGNAL_ARGS);


/*
 * 检查点进程的主入口点           
 *
 * 本函数由 AuxiliaryProcessMain 调用，后者已创建              
 * 基本执行环境，但尚未启用信号。            
 */
void
CheckpointerMain(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext checkpointer_context;

	CheckpointerShmem->checkpointer_pid = MyProcPid;

	/*
	 * 恰当地接受或忽略 postmaster 可能发来的信号     
	 *
	 * 注意：我们有意忽略 SIGTERM，因为在标准 Unix          
	 * 系统关闭流程中，init 会一次性 SIGTERM 所有进程。  
	 * 我们要等后端退出，届时 postmaster 会                    
	 * 通知我们可以关闭（经 SIGUSR2）。 
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, ReqCheckpointHandler); /* 请求检查点    */
	pqsignal(SIGTERM, SIG_IGN); /* 忽略 SIGTERM */
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置        */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

	/*
	 * 重置一些 postmaster 接受但此处不接受的信号      
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 初始化以使首个时间驱动事件在正确的时间发生。     
	 */
	last_checkpoint_time = last_xlog_switch_time = (pg_time_t) time(NULL);

	/*
	 * 关闭后写出统计。正常关闭期间必须恰好由一个       
	 * 进程调用，而检查点进程的关闭时机非常晚……      
	 *             
	 *
	 * walsender 在检查点进程之后关闭，但目前并不          
	 * 上报统计。若这一点改变，就需要更复杂的方案。 
	 */
	before_shmem_exit(pgstat_before_server_shutdown, 0);

	/*
	 * 创建一个内存上下文，我们所有工作都在其中进行。  
	 * 这样在错误恢复期间可以重置该上下文，从而避免   
	 * 可能的内存泄漏。以前这些代码直接在   
	 * TopMemoryContext 中运行，重置它会非常糟糕。        
	 */
	checkpointer_context = AllocSetContextCreate(TopMemoryContext,
												 "Checkpointer",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(checkpointer_context);

	/*
	 * 若遇到异常，处理将在此处恢复。           
	 *
	 * 你可能奇怪为什么不用围绕 PG_TRY 构造的             
	 * 无限循环来编写。原因在于这里是异常栈的      
	 * 底部，若用 PG_TRY 则在 CATCH 部分                             
	 * 期间将完全没有异常处理程序生效。让最外层           
	 * setjmp 始终处于活动状态，我们至少还有机会从          
	 * 错误恢复中恢复过来。（若因此陷入无限循环，     
	 * 它很快会因 elog.c 内部状态栈溢出而停止。）        
	 *
	 * 注意我们使用 sigsetjmp(..., 1)，使当前生效的             
	 * 信号掩码（即 BlockSig）在 longjmp 回到这里被恢复。  
	 * 于是，除 SIGQUIT 之外的信号都会被阻塞，直到      
	 * 错误恢复完成。看起来这一策略使 HOLD_INTERRUPTS()      
	 * 调用看似多余，但 InterruptPending 可能早已置位     
	 *         
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* 由于未用 PG_TRY，必须手动重置错误栈      */
		error_context_stack = NULL;

		/* 清理期间禁止中断             */
		HOLD_INTERRUPTS();

		/* 向服务器日志报告该错误  */
		EmitErrorReport();

		/*
		 * 这些操作其实只是 AbortTransaction()         
		 * 的一个极小子集。检查点进程中需要担心的      
		 * 资源不多，但有 LWLock、缓冲区和临时文件。      
		 *       
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/* 告知所有等待中的后端检查点已失败。   */
		if (ckpt_active)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_failed++;
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			ckpt_active = false;
		}

		/*
		 * 现在回到正常的顶层上下文，并清空 ErrorContext    
		 * 备用。 
		 */
		MemoryContextSwitchTo(checkpointer_context);
		FlushErrorState();

		/* 刷出顶层上下文里泄漏的数据        */
		MemoryContextResetAndDeleteChildren(checkpointer_context);

		/* 现在可以重新允许中断了 */
		RESUME_INTERRUPTS();

		/*
		 * 任何错误之后至少休眠 1 秒。写错误很可能反复  
		 * 出现，我们不想以最快速度塞满错误日志。        
		 *                
		 */
		pg_usleep(1000000L);

		/*
		 * 任何错误之后关闭所有打开的文件。这有助于      
		 * 规避 Windows 上保持已删除文件打开导致的各种   
		 * 奇怪错误。别处未必需要，但应无害。     
		 */
		smgrcloseall();
	}

	/* 现在能处理 ereport(ERROR)   */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * 解除信号阻塞（postmaster fork 我们时阻塞了信号） 
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 确保所有共享内存值都针对该配置正确设置。           
	 * 在此处执行确保不会与其他并发更新者产生竞争。  
	 */
	UpdateSharedMemoryConfig();

	/*
	 * 公布我们的锁存器，后端可在我们休眠时用它       
	 * 唤醒   
	 */
	ProcGlobal->checkpointerLatch = &MyProc->procLatch;

	/*
	 * 无限循环
	 */
	for (;;)
	{
		bool		do_checkpoint = false;
		int			flags = 0;
		pg_time_t	now;
		int			elapsed_secs;
		int			cur_timeout;

		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		/*
		 * 处理近期收到的任何请求或信号。     
		 */
		AbsorbSyncRequests();
		HandleCheckpointerInterrupts();

		/*
		 * 通过检查共享内存中的标志字是否非零，来        
		 * 检测是否有挂起的检查点请求。此时无需获取       
		 * ckpt_lck。       
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
		{
			do_checkpoint = true;
			PendingCheckpointerStats.requested_checkpoints++;
		}

		/*
		 * 若距上次检查点已过去太长时间，则强制执行       
		 * 检查点。注意：只有当定时检查点在没有外部 
		 * 请求的情况下发生时才计入统计，但即使同时      
		 * 也会设置 CAUSE_TIME 标志位。          
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
		{
			if (!do_checkpoint)
				PendingCheckpointerStats.timed_checkpoints++;
			do_checkpoint = true;
			flags |= CHECKPOINT_CAUSE_TIME;
		}

		/*
		 * 有请求则做检查点。  
		 */
		if (do_checkpoint)
		{
			bool		ckpt_performed = false;
			bool		do_restartpoint;

			/* 检查我们应执行检查点还是重启点。           */
			do_restartpoint = RecoveryInProgress();

			/*
			 * 原子地获取请求标志，以确定应执行哪种         
			 * 检查点，并递增 started 计数器以表明              
			 * 已开始新的检查点。                        
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			flags |= CheckpointerShmem->ckpt_flags;
			CheckpointerShmem->ckpt_flags = 0;
			CheckpointerShmem->ckpt_started++;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->start_cv);

			/*
			 * 恢复结束时的检查点是在我们仍处于恢复    
			 * 期间执行的真实检查点。       
			 */
			if (flags & CHECKPOINT_END_OF_RECOVERY)
				do_restartpoint = false;

			/*
			 * 以下情况会警告：（a）距上次检查点过近     
			 * （无论起因）；（b）自上次检查点以来设置过 
			 * CHECKPOINT_CAUSE_XLOG 标志。特别注意：                
			 * 本实现不会产生由 CheckPointTimeout         
			 * < CheckPointWarning 所致警告。   
			 */
			if (!do_restartpoint &&
				(flags & CHECKPOINT_CAUSE_XLOG) &&
				elapsed_secs < CheckPointWarning)
				ereport(LOG,
						(errmsg_plural("checkpoints are occurring too frequently (%d second apart)",
									   "checkpoints are occurring too frequently (%d seconds apart)",
									   elapsed_secs,
									   elapsed_secs),
						 errhint("Consider increasing the configuration parameter \"max_wal_size\".")));

			/*
			 * 初始化检查点期间所用的进程私有变量  
			 *            
			 */
			ckpt_active = true;
			if (do_restartpoint)
				ckpt_start_recptr = GetXLogReplayRecPtr(NULL);
			else
				ckpt_start_recptr = GetInsertRecPtr();
			ckpt_start_time = now;
			ckpt_cached_elapsed = 0;

			/*
			 * 做检查点。   
			 */
			if (!do_restartpoint)
			{
				CreateCheckPoint(flags);
				ckpt_performed = true;
			}
			else
				ckpt_performed = CreateRestartPoint(flags);

			/*
			 * 任何检查点之后都关闭所有 smgr 文件，       
			 * 以免无限期地保留对已删除文件的 smgr 引用。  
			 */
			smgrcloseall();

			/*
			 * 向所有等待中的后端告知检查点已完成。 
			 */
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			CheckpointerShmem->ckpt_done = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			ConditionVariableBroadcast(&CheckpointerShmem->done_cv);

			if (ckpt_performed)
			{
				/*
				 * 注意：我们记录为 last_checkpoint_time 的是    
				 * 开始时间而非结束时间，这样定时     
				 * 检查点以可预期的间隔发生。     
				 */
				last_checkpoint_time = now;
			}
			else
			{
				/*
				 * 我们未能执行重启点（检查点在出错时会   
				 * 抛出 ERROR）。最可能的原因是自上次         
				 * 重启点以来未收到新的检查点 WAL 记录。     
				 * 15 秒后重试。                   
				 */
				last_checkpoint_time = now - CheckPointTimeout + 15;
			}

			ckpt_active = false;

			/* 检查点期间我们可能已收到中断。            */
			HandleCheckpointerInterrupts();
		}

		/* 检查 archive_timeout，必要时切换 xlog 文件。       */
		CheckArchiveTimeout();

		/* 将待处理的统计上报给累计统计系统。      */
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/*
		 * 如果设置了任何检查点标志，就重新循环以便      
		 * 不休眠处理检查点。 
		 */
		if (((volatile CheckpointerShmemStruct *) CheckpointerShmem)->ckpt_flags)
			continue;

		/*
		 * 休眠，直到被信号唤醒，或到了下一次检查点或   
		 * xlog 切换。   
		 */
		now = (pg_time_t) time(NULL);
		elapsed_secs = now - last_checkpoint_time;
		if (elapsed_secs >= CheckPointTimeout)
			continue;			/* 不休眠……     */
		cur_timeout = CheckPointTimeout - elapsed_secs;
		if (XLogArchiveTimeout > 0 && !RecoveryInProgress())
		{
			elapsed_secs = now - last_xlog_switch_time;
			if (elapsed_secs >= XLogArchiveTimeout)
				continue;		/* 不休眠……     */
			cur_timeout = Min(cur_timeout, XLogArchiveTimeout - elapsed_secs);
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout * 1000L /* 化为毫秒  */ ,
						 WAIT_EVENT_CHECKPOINTER_MAIN);
	}
}

/*
 * 处理新的中断。      
 */
static void
HandleCheckpointerInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/*
		 * 检查点进程是最后一个关闭的进程，因此我们让它 
		 * 负责保管一系列其他任务所需的钥匙，其中大多
		 * 数与检查点毫无关系。          
		 *
		 * 由于种种原因，某些配置值会动态变化，因此     
		 * 它们的主副本保存在共享内存中，以确保所有      
		 * 后端看到相同的值。我们让 Checkpointer 负责          
		 * 在参数设置因 SIGHUP 而改变时更新共享内存        
		 * 副本。         
		 */
		UpdateSharedMemoryConfig();
	}
	if (ShutdownRequestPending)
	{
		/*
		 * 从这里开始，elog(ERROR) 应以 exit(1) 结束，而不是    
		 * 交回上面 sigsetjmp 块       
		 */
		ExitOnAnyError = true;

		/*
		 * 关闭数据库。      
		 *
		 * 由于 ShutdownXLOG() 会创建重启点或检查点，     
		 * 并更新统计，所以要递增检查点请求，并刷出      
		 * 待处理统计。    
		 */
		PendingCheckpointerStats.requested_checkpoints++;
		ShutdownXLOG(0, 0);
		pgstat_report_checkpointer();
		pgstat_report_wal(true);

		/* 检查点进程的正常退出在此处   */
		proc_exit(0);			/* 完  */
	}

	/* 记录本进程的内存上下文日志            */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * CheckArchiveTimeout —— 检查 archive_timeout 并切换           
 *
 * 若当前 WAL 文件中记录了有意义的活动，这会切换    
 * 到新 WAL 文件并强制写出归档文件。这包括大多数          
 * 写出，包括仅仅一条检查点记录的写出，但排除带         
 * 有 XLOG_MARK_UNIMPORTANT 标志插入的 WAL 记录                  
 * （如运行中事务的快照）。这类记录按配置定期
 * 出现且不含重要信息。这样可以避免生成               
 * 只含少量不重要记录的归档。                             
 *         
 */
static void
CheckArchiveTimeout(void)
{
	pg_time_t	now;
	pg_time_t	last_time;
	XLogRecPtr	last_switch_lsn;

	if (XLogArchiveTimeout <= 0 || RecoveryInProgress())
		return;

	now = (pg_time_t) time(NULL);

	/* 首先用可能过期的本地状态做快速检查。      */
	if ((int) (now - last_xlog_switch_time) < XLogArchiveTimeout)
		return;

	/*
	 * 更新本地状态……注意 last_xlog_switch_time 是               
	 * 切换执行或被请求的时间。  
	 */
	last_time = GetLastSegSwitchData(&last_switch_lsn);

	last_xlog_switch_time = Max(last_xlog_switch_time, last_time);

	/* 现在能做真正的检查   */
	if ((int) (now - last_xlog_switch_time) >= XLogArchiveTimeout)
	{
		/*
		 * 只有自上次段切换以来记录了"重要" WAL 时才        
		 * 切换。（last_switch_lsn 指向发生切换的段         
		 * 的末尾）。     
		 */
		if (GetLastImportantRecPtr() > last_switch_lsn)
		{
			XLogRecPtr	switchpoint;

			/* 把切换标记为不重要，避免触发检查点       */
			switchpoint = RequestXLogSwitch(true);

			/*
			 * 如果返回的指针恰好指向段边界，                
			 * 认为无事发生。   
			 */
			if (XLogSegmentOffset(switchpoint, wal_segment_size) != 0)
				elog(DEBUG1, "write-ahead log switch forced (archive_timeout=%d)",
					 XLogArchiveTimeout);
		}

		/*
		 * 无论何种情况都更新状态，以免系统空闲时      
		 * 不断重试。
		 */
		last_xlog_switch_time = now;
	}
}

/*
 * 如果有立即检查点请求处于挂起状态则返回真。        
 * （注意：这检查的不是当前检查点的 IMMEDIATE 标志，      
 * 而是后面还有请求。）  
 */
static bool
ImmediateCheckpointRequested(void)
{
	volatile CheckpointerShmemStruct *cps = CheckpointerShmem;

	/*
	 * 这种情况下无需获取 ckpt_lck，因为我们只               
	 * 查看单个标志位。     
	 */
	if (cps->ckpt_flags & CHECKPOINT_IMMEDIATE)
		return true;
	return false;
}

/*
 * CheckpointWriteDelay —— 控制检查点速率 
 *
 * 本函数在 BufferSync() 每次执行页面写出之后调用。       
 * 它负责限制 BufferSync() 的写出速率，以达到         
 * checkpoint_completion_target 
 *
 * 检查点请求标志应由调用方传入；目前唯一会            
 * 检查的是 CHECKPOINT_IMMEDIATE，它取消写出之间的延迟。  
 *
 * 'progress' 是对已完成工作量占比的估计，                
 * 介于 0.0（未完成）到 1.0（全部完成）之间。 
 */
void
CheckpointWriteDelay(int flags, double progress)
{
	static int	absorb_counter = WRITES_PER_ABSORB;

	/* 若检查点正由非检查点进程执行则不做任何事           */
	if (!AmCheckpointerProcess())
		return;

	/*
	 * 执行常规职责并小睡片刻，除非进度已落后，          
	 * 若落后则尽快设法赶上。                            
	 */
	if (!(flags & CHECKPOINT_IMMEDIATE) &&
		!ShutdownRequestPending &&
		!ImmediateCheckpointRequested() &&
		IsCheckpointOnSchedule(progress))
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			/* 更新配置变量的共享内存副本 */
			UpdateSharedMemoryConfig();
		}

		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;

		CheckArchiveTimeout();

		/* 将中间统计信息上报给累计统计系统         */
		pgstat_report_checkpointer();

		/*
		 * 此睡眠过去与 bgwriter_delay 挂钩，典型值为 200ms。   
		 * 若没什么工作要做，会导致更频繁的唤醒。     
		 * 检查点进程与 bgwriter 已不再相关，故睡长觉。  
		 *       
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT,
				  100,
				  WAIT_EVENT_CHECKPOINT_WRITE_DELAY);
		ResetLatch(MyLatch);
	}
	else if (--absorb_counter <= 0)
	{
		/*
		 * 每 WRITES_PER_ABSORB 次写操作就吸收待处理的 fsync   
		 * 请求，即便不睡眠也如此，以防 fsync 请求        
		 * 队列溢出。     
		 */
		AbsorbSyncRequests();
		absorb_counter = WRITES_PER_ABSORB;
	}

	/* 检查屏障事件。     */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();
}

/*
 * IsCheckpointOnSchedule —— 能否按计划及时完成此检查点 
 *		 （或重启点）？     
 *
 * 将当前进度与上次检查点以来流逝的时间/段数对比，    
 * 若当前进度超过已流逝的时间/段数，则返回真。           
 *                                
 */
static bool
IsCheckpointOnSchedule(double progress)
{
	XLogRecPtr	recptr;
	struct timeval now;
	double		elapsed_xlogs,
				elapsed_time;

	Assert(ckpt_active);

	/* 按 checkpoint_completion_target 缩放进度。          */
	progress *= CheckPointCompletionTarget;

	/*
	 * 先比较缓存值；只有到达先前算出的目标，才做 
	 * 更昂贵的计算。时间和 WAL 插入位置均不会后退， 
	 * 故新算出的值只会大于等于缓存值。             
	 *                                                                        
	 */
	if (progress < ckpt_cached_elapsed)
		return false;

	/*
	 * 对照已写的 WAL 段数和 CheckPointSegments 检查进度。   
	 *
	 * 我们将当前 WAL 插入位置与调用                      
	 * CreateCheckPoint 之前算出的位置比较。                      
	 * XLogInsert 中超出 CheckPointSegments 时真正触发检查点  
	 * 的代码比较的是 RedoRecPtr，因此并不完全精确。  
	 * 不过对我们的目的而言已足够，反正只是估算。      
	 *                 
	 *
	 * 恢复期间，我们将最后重放的 WAL 记录位置与调用   
	 * CreateRestartPoint 前算出的位置比较。这与正常运行时    
	 * 检查点节奏保持一致，但可能超出 max_wal_size 不少。  
	 * 这是因为检查点的 redo 指针与其检查点记录之间        
	 * 可能相距很远，并且只有看到检查点记录之后，      
	 * 我们才会启动重启点。（该间隔通常最大可达          
	 * CheckPointSegments * checkpoint_completion_target，               
	 * 其中的值是生成该 WAL 时生效的取值）。                 
	 *                                                      
	 */
	if (RecoveryInProgress())
		recptr = GetXLogReplayRecPtr(NULL);
	else
		recptr = GetInsertRecPtr();
	elapsed_xlogs = (((double) (recptr - ckpt_start_recptr)) /
					 wal_segment_size) / CheckPointSegments;

	if (progress < elapsed_xlogs)
	{
		ckpt_cached_elapsed = elapsed_xlogs;
		return false;
	}

	/*
	 * 对照流逝时间与 checkpoint_timeout 检查进度。   
	 */
	gettimeofday(&now, NULL);
	elapsed_time = ((double) ((pg_time_t) now.tv_sec - ckpt_start_time) +
					now.tv_usec / 1000000.0) / CheckPointTimeout;

	if (progress < elapsed_time)
	{
		ckpt_cached_elapsed = elapsed_time;
		return false;
	}

	/* 看来进度正常。            */
	return true;
}


/* --------------------------------
 *		信号处理例程     
 * --------------------------------
 */

/* SIGINT：置标志以立刻执行一次普通检查点 */
static void
ReqCheckpointHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * 发信号的进程应已把 ckpt_flags 置为非零，我们只需 
	 * 确保主循环被踢出任何等待状态即可。              
	 */
	SetLatch(MyLatch);

	errno = save_errno;
}


/* --------------------------------
 *		与后端的通信         
 * --------------------------------
 */

/*
 * CheckpointerShmemSize
 *		计算检查点进程相关共享内存所需的空间     
 */
Size
CheckpointerShmemSize(void)
{
	Size		size;

	/*
	 * requests[] 数组的大小被任意地设为等于 NBuffers。        
	 * 但存在 MAX_CHECKPOINT_REQUESTS 上限，以防在环形缓冲区  
	 * 中累积过多检查点请求。               
	 */
	size = offsetof(CheckpointerShmemStruct, requests);
	size = add_size(size, mul_size(Min(NBuffers,
									   MAX_CHECKPOINT_REQUESTS),
								   sizeof(CheckpointerRequest)));

	return size;
}

/*
 * CheckpointerShmemInit
 *		分配并初始化检查点进程相关的共享内存    
 */
void
CheckpointerShmemInit(void)
{
	Size		size = CheckpointerShmemSize();
	bool		found;

	CheckpointerShmem = (CheckpointerShmemStruct *)
		ShmemInitStruct("Checkpointer Data",
						size,
						&found);

	if (!found)
	{
		/*
		 * 首次经过，故进行初始化。注意我们将整个      
		 * 数组清零；这是为了让 CompactCheckpointerRequestQueue     
		 * 能假定请求结构中的填充字节全为零。         
		 */
		MemSet(CheckpointerShmem, 0, size);
		SpinLockInit(&CheckpointerShmem->ckpt_lck);
		CheckpointerShmem->max_requests = Min(NBuffers, MAX_CHECKPOINT_REQUESTS);
		ConditionVariableInit(&CheckpointerShmem->start_cv);
		ConditionVariableInit(&CheckpointerShmem->done_cv);
	}
}

/*
 * RequestCheckpoint
 *		在后端进程中调用以请求检查点         
 *
 * flags 是下列各项的按位或：   
 *	CHECKPOINT_IS_SHUTDOWN：因数据库关闭而设。        
 *	CHECKPOINT_END_OF_RECOVERY：因 WAL 恢复结束而设。        
 *	CHECKPOINT_IMMEDIATE：尽快完成检查点，  
 *		忽略 checkpoint_completion_target 参数。   
 *	CHECKPOINT_FORCE：即使自上次检查点以来无 XLOG 活动，       
 *		也强制执行，隐含于 CHECKPOINT_IS_SHUTDOWN 或  
 *		CHECKPOINT_END_OF_RECOVERY  
 *	CHECKPOINT_WAIT：返回前等待完成（否则，              
 *		仅通知检查点进程执行并返回）。  
 *	CHECKPOINT_CAUSE_XLOG：因 xlog 填满而请求检查点。       
 *		（这影响日志记录，尤其启用 CheckPointWarning。）     
 */
void
RequestCheckpoint(int flags)
{
	int			ntries;
	int			old_failed,
				old_started;

	/*
	 * 若处于单机后端，就自己动手执行。 
	 */
	if (!IsPostmasterEnvironment)
	{
		/*
		 * 在单机后端中做慢速检查点毫无意义，                
		 * 因为没有可供检查点打扰的其他后端。            
		 */
		CreateCheckPoint(flags | CHECKPOINT_IMMEDIATE);

		/*
		 * 每次检查点之后，关闭所有 smgr 文件。这样我们 
		 * 不会无限期保留已删文件的 smgr 引用。     
		 */
		smgrcloseall();

		return;
	}

	/*
	 * 原子地设置请求标志，并对计数器取一个快照。       
	 * 当我们看到 ckpt_started > old_started 时，就知道           
	 * 其已被检查点进程看到 
	 *
	 * 注意我们将标志与已有标志按位或，以免覆盖          
	 * 其他后端"更强"的请求。各标志的极性须            
	 * 选对方能奏效！    
	 */
	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);

	old_failed = CheckpointerShmem->ckpt_failed;
	old_started = CheckpointerShmem->ckpt_started;
	CheckpointerShmem->ckpt_flags |= (flags | CHECKPOINT_REQUESTED);

	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	/*
	 * 发送信号以请求检查点。检查点进程可能                 
	 * 尚未启动，或正在重启，因此必要时我们会重试       
	 * 几次。（实际上不止几次，因为在缓慢或过载的      
	 * buildfarm 机器上，曾观察到检查点进程要花      
	 * 几秒钟才能启动。）但是，若未被告知要等待          
	 * 检查点发生，我们会把发送信号失败视为              
	 * 非致命，仅记入 LOG。检查点进程无论是否              
	 * 收到信号，启动后都会看到该请求。                 
	 */
#define MAX_SIGNAL_TRIES 600	/* 至多等 60 秒  */
	for (ntries = 0;; ntries++)
	{
		if (CheckpointerShmem->checkpointer_pid == 0)
		{
			if (ntries >= MAX_SIGNAL_TRIES || !(flags & CHECKPOINT_WAIT))
			{
				elog((flags & CHECKPOINT_WAIT) ? ERROR : LOG,
					 "could not signal for checkpoint: checkpointer is not running");
				break;
			}
		}
		else if (kill(CheckpointerShmem->checkpointer_pid, SIGINT) != 0)
		{
			if (ntries >= MAX_SIGNAL_TRIES || !(flags & CHECKPOINT_WAIT))
			{
				elog((flags & CHECKPOINT_WAIT) ? ERROR : LOG,
					 "could not signal for checkpoint: %m");
				break;
			}
		}
		else
			break;				/* 信号发送成功       */

		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000L);		/* 等 0.1 秒后重试     */
	}

	/*
	 * 若被要求，则等待完成。我们按前述算法               
	 * 检测完成。           
	 */
	if (flags & CHECKPOINT_WAIT)
	{
		int			new_started,
					new_failed;

		/* 等待新检查点启动。         */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->start_cv);
		for (;;)
		{
			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_started = CheckpointerShmem->ckpt_started;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_started != old_started)
				break;

			ConditionVariableSleep(&CheckpointerShmem->start_cv,
								   WAIT_EVENT_CHECKPOINT_START);
		}
		ConditionVariableCancelSleep();

		/*
		 * 我们等待 ckpt_done >= new_started（按模意义）。     
		 */
		ConditionVariablePrepareToSleep(&CheckpointerShmem->done_cv);
		for (;;)
		{
			int			new_done;

			SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
			new_done = CheckpointerShmem->ckpt_done;
			new_failed = CheckpointerShmem->ckpt_failed;
			SpinLockRelease(&CheckpointerShmem->ckpt_lck);

			if (new_done - new_started >= 0)
				break;

			ConditionVariableSleep(&CheckpointerShmem->done_cv,
								   WAIT_EVENT_CHECKPOINT_DONE);
		}
		ConditionVariableCancelSleep();

		if (new_failed != old_failed)
			ereport(ERROR,
					(errmsg("checkpoint request failed"),
					 errhint("Consult recent messages in the server log for details.")));
	}
}

/*
 * ForwardSyncRequest
 *		将文件 fsync 请求从后端转发给检查点进程        
 *
 * 每当后端被迫直接写某个关系时                     
 * （如果后台写入进程尽职，这应当很少发生），            
 * 后端就调用本例程，告知该关系是脏的、                 
 * 必须在下次检查点前做 fsync。我们还借机                
 * 为此类写入计数以供统计。                      
 *
 * 为避免不必要地长时间持锁，我们通常直接             
 * 写入 requests[] 队列而不查重。检查点进程                    
 * 反正也得在内部去重。但若我们发现                       
 * 队列已满，就会遍历整个队列做一次压缩。              
 * 这开销不小，但另一种选择是后端自己                    
 * 执行 fsync，实践中开销大得多。理论上，                
 * 若队列已满且不含重复条目，后端自行                   
 * fsync 仍可能有必要。那种情况下我们通过                
 * 返回 false 告知后端。            
 */
bool
ForwardSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	CheckpointerRequest *request;
	bool		too_full;

	if (!IsUnderPostmaster)
		return false;			/* 大概根本不该到达这里   */

	if (AmCheckpointerProcess())
		elog(ERROR, "ForwardSyncRequest must not be called in checkpointer");

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/* 无论是否放得进队列，都统计所有后端写入       */
	if (!AmBackgroundWriterProcess())
		CheckpointerShmem->num_backend_writes++;

	/*
	 * 若检查点进程未在运行或请求队列已满，             
	 * 后端将不得不自行执行 fsync 请求。但在强令             
	 * 此事发生前，可先尝试压缩请求队列。     
	 */
	if (CheckpointerShmem->checkpointer_pid == 0 ||
		(CheckpointerShmem->num_requests >= CheckpointerShmem->max_requests &&
		 !CompactCheckpointerRequestQueue()))
	{
		/*
		 * 统计后端不得不自行执行 fsync 的那部分写入    
		 *      
		 */
		if (!AmBackgroundWriterProcess())
			CheckpointerShmem->num_backend_fsync++;
		LWLockRelease(CheckpointerCommLock);
		return false;
	}

	/* 好，插入请求 */
	request = &CheckpointerShmem->requests[CheckpointerShmem->num_requests++];
	request->ftag = *ftag;
	request->type = type;

	/* 若队列超过半满，就轻推检查点进程将其清空        */
	too_full = (CheckpointerShmem->num_requests >=
				CheckpointerShmem->max_requests / 2);

	LWLockRelease(CheckpointerCommLock);

	/* ……但要等到释放锁之后          */
	if (too_full && ProcGlobal->checkpointerLatch)
		SetLatch(ProcGlobal->checkpointerLatch);

	return true;
}

/*
 * CompactCheckpointerRequestQueue
 *		从请求队列中去除重复项以避免后端 fsync。        
 *		若移除了任何条目则返回 "true"。
 *
 * 虽然 fsync 请求队列满员并不常见，但一旦发生           
 * 就可能导致严重的性能问题。迄今这种情况            
 * 仅在系统承受沉重写负载时被观察到，                   
 * 尤其在检查点的 "sync" 阶段。若没有这段                
 * 逻辑，每个后端都会对每个写入的块执行                  
 * fsync，其代价高昂，可拖慢整个系统。     
 *
 * 每次队列满员都这么做可能白费功夫，若            
 * 没有可移除的条目。但这在实践中应当极为            
 * 罕见：每个共享缓冲区一个队列条目。 
 */
static bool
CompactCheckpointerRequestQueue(void)
{
	struct CheckpointerSlotMapping
	{
		CheckpointerRequest request;
		int			slot;
	};

	int			n,
				preserve_count;
	int			num_skipped = 0;
	HASHCTL		ctl;
	HTAB	   *htab;
	bool	   *skip_slot;

	/* 必须以独占模式持有 CheckpointerCommLock */
	Assert(LWLockHeldByMe(CheckpointerCommLock));

	/* 避免在临界区内进行内存分配。      */
	if (CritSectionCount > 0)
		return false;

	/* 初始化 skip_slot 数组 */
	skip_slot = palloc0(sizeof(bool) * CheckpointerShmem->num_requests);

	/* 初始化临时哈希表        */
	ctl.keysize = sizeof(CheckpointerRequest);
	ctl.entrysize = sizeof(struct CheckpointerSlotMapping);
	ctl.hcxt = CurrentMemoryContext;

	htab = hash_create("CompactCheckpointerRequestQueue",
					   CheckpointerShmem->num_requests,
					   &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * 这里的基本想法是：若某请求之后跟着相同的         
	 * 更晚请求，即可跳过该请求。从队尾倒着检查       
	 * 某请求前面是否有更早的相同请求看似更明智，   
	 * *可减少拷贝。但若中间夹有 SYNC_FORGET_REQUEST 或          
	 * SYNC_FILTER_REQUEST，语义就可能改变，故我们     
	 * 采用这种做法。若让下面的代码理解这类请求       
	 * 的特定语义，本可做得更聪明（可清除反正             
	 * 终将被取消的前导条目），但并不清楚这种              
	 * 额外复杂度能带来什么好处。                             
	 *                                                                
	 */
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		CheckpointerRequest *request;
		struct CheckpointerSlotMapping *slotmap;
		bool		found;

		/*
		 * 我们直接用请求结构作为哈希表键。这         
		 * 假定结构中的填充字节始终一致，这应当            
		 * 没有问题，因为我们在                      
		 * CheckpointerShmemInit 清零。另请注意 RelFileLocator      
		 * 最好无填充字节
		 */
		request = &CheckpointerShmem->requests[n];
		slotmap = hash_search(htab, request, HASH_ENTER, &found);
		if (found)
		{
			/* 重复项，故将上一次出现标记为可跳过     */
			skip_slot[slotmap->slot] = true;
			num_skipped++;
		}
		/* 记住包含该请求值最新一次出现的槽位              */
		slotmap->slot = n;
	}

	/* 哈希表处理完毕。  */
	hash_destroy(htab);

	/* 若无重复项则无计可施。    */
	if (!num_skipped)
	{
		pfree(skip_slot);
		return false;
	}

	/* 发现重复项；将其移除。      */
	preserve_count = 0;
	for (n = 0; n < CheckpointerShmem->num_requests; n++)
	{
		if (skip_slot[n])
			continue;
		CheckpointerShmem->requests[preserve_count++] = CheckpointerShmem->requests[n];
	}
	ereport(DEBUG1,
			(errmsg_internal("compacted fsync request queue from %d entries to %d entries",
							 CheckpointerShmem->num_requests, preserve_count)));
	CheckpointerShmem->num_requests = preserve_count;

	/* 清理。*/
	pfree(skip_slot);
	return true;
}

/*
 * AbsorbSyncRequests
 *		取回排队的同步请求并转交给同步机制。        
 *
 * 导出它是因为必须在 CreateCheckPoint 期间调用；       
 * 我们必须确保在刚开始 fsync 之前，                      
 * 已接受全部待处理请求。由于 CreateCheckPoint     
 * 有时运行于非检查点进程，非检查点无操作  
 */
void
AbsorbSyncRequests(void)
{
	CheckpointerRequest *requests = NULL;
	CheckpointerRequest *request;
	int			n;

	if (!AmCheckpointerProcess())
		return;

	LWLockAcquire(CheckpointerCommLock, LW_EXCLUSIVE);

	/* 把统计计数转移到待发的 pgstats 消息中*/
	PendingCheckpointerStats.buf_written_backend
		+= CheckpointerShmem->num_backend_writes;
	PendingCheckpointerStats.buf_fsync_backend
		+= CheckpointerShmem->num_backend_fsync;

	CheckpointerShmem->num_backend_writes = 0;
	CheckpointerShmem->num_backend_fsync = 0;

	/*
	 * 为尽量避免长时间持锁，我们先复制请求                 
	 * 数组，然后释放锁再处理这些请求。            
	 *
	 * 一旦我们把请求从共享内存中清除，之后若             
	 * 吸收失败（例如哈希表内存耗尽），就必须            
	 * PANIC。因为若无法 fsync 被告知要 fsync 的内容，          
	 * 系统就无法安全运行。幸运的是，该哈希表            
	 * 非常小，该问题在实践中极难出现。                    
	 */
	n = CheckpointerShmem->num_requests;
	if (n > 0)
	{
		requests = (CheckpointerRequest *) palloc(n * sizeof(CheckpointerRequest));
		memcpy(requests, CheckpointerShmem->requests, n * sizeof(CheckpointerRequest));
	}

	START_CRIT_SECTION();

	CheckpointerShmem->num_requests = 0;

	LWLockRelease(CheckpointerCommLock);

	for (request = requests; n > 0; request++, n--)
		RememberSyncRequest(&request->ftag, request->type);

	END_CRIT_SECTION();

	if (requests)
		pfree(requests);
}

/*
 * 根据配置参数更新所有共享内存配置                  
 */
static void
UpdateSharedMemoryConfig(void)
{
	/* 更新 sync rep 全局 shmem 状态    */
	SyncRepUpdateSyncStandbysDefined();

	/*
	 * 若 full_page_writes 被 SIGHUP 改动，我们会更新共享        
	 * 内存并写入 XLOG_FPW_CHANGE 记录。  
	 */
	UpdateFullPageWrites();

	elog(DEBUG2, "checkpointer updated shared memory configuration values");
}

/*
 * FirstCallSinceLastCheckpoint 允许进程在每个检查点         
 * 周期内执行一次动作：异步检查检查点是否完成。        
 */
bool
FirstCallSinceLastCheckpoint(void)
{
	static int	ckpt_done = 0;
	int			new_done;
	bool		FirstCall = false;

	SpinLockAcquire(&CheckpointerShmem->ckpt_lck);
	new_done = CheckpointerShmem->ckpt_done;
	SpinLockRelease(&CheckpointerShmem->ckpt_lck);

	if (new_done != ckpt_done)
		FirstCall = true;

	ckpt_done = new_done;

	return FirstCall;
}
