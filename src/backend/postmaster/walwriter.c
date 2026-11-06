/*-------------------------------------------------------------------------
 *
 * walwriter.c
 *
 * WAL 写入者后台进程是 Postgres 8.3 新增的。它尽力让普通 
 * 后端不必自己写出（并 fsync）WAL 页面。此外，它保证  
 * 那些在提交时未立即刷盘（即异步提交）的事务        
 * 提交记录，将在可知的时间内到达磁盘——            
 * 所需时间恰好至多是 wal_writer_delay 周期的三倍。       
 *                                                  
 *
 * 注意，正如 bgwriter 之于共享缓冲区，walwriter 跟不上，
 * 普通后端仍有权自行执行 WAL 写出与 fsync。这意味着     
 * WALWriter 并非必不可少的进程，被要求时可以            
 * 快速关闭。                     
 *
 * 由于 walwriter 的周期直接关系到异步提交事务获得    
 * 提交保证前的最大延迟，把额外功能加载到它上面      
 * 恐怕不明智。例如，若你想提前创建 xlog 段，             
 * 那么在 bgwriter 里做会比在 walwriter 里做更合适。            
 *                               
 *
 * walwriter 由 postmaster 在 startup 子进程结束后立即启动。      
 * 它一直存活，直到 postmaster 命令其终止。                    
 * 正常终止靠 SIGTERM，它指示 walwriter 执行 exit(0)。            
 * 紧急终止靠 SIGQUIT；与其他后端一样，walwriter 收到       
 * SIGQUIT 即中止并退出。    
 *
 * 若 walwriter 意外退出，postmaster 会将其视同                 
 * 后端崩溃：共享内存可能已损坏，因此要用 SIGQUIT        
 * 杀掉其余后端，随后启动一轮恢复周期。        
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/walwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/walwriter.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*
 * GUC 参数    
 */
int			WalWriterDelay = 200;
int			WalWriterFlushAfter = DEFAULT_WAL_WRITER_FLUSH_AFTER;

/*
 * 延长延迟时间前空转循环的次数，以及决定休眠时   
 * 对 WalWriterDelay 应用的倍率。                                
 * （或许这些应做成可配置？） 
 */
#define LOOPS_UNTIL_HIBERNATE		50
#define HIBERNATE_FACTOR			25

/* 私有函数的原型声明      */
static void HandleWalWriterInterrupts(void);

/*
 * walwriter 进程的主入口点       
 *
 * 本函数由 AuxiliaryProcessMain 调用，后者已创建              
 * 基本执行环境，但尚未启用信号。            
 */
void
WalWriterMain(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext walwriter_context;
	int			left_till_hibernate;
	bool		hibernating;

	/*
	 * 恰当地接受或忽略 postmaster 可能发来的信号     
	 *
	 * 我们目前对 SIGINT 没有特殊用途，但将其        
	 * 视同 SIGTERM 似乎合理。   
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN); /* 未使用*/

	/*
	 * 重置一些 postmaster 接受但此处不接受的信号      
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 创建一个内存上下文，我们所有工作都在其中进行。  
	 * 这样在错误恢复期间可以重置该上下文，从而避免   
	 * 可能的内存泄漏。以前这些代码直接在   
	 * TopMemoryContext 中运行，重置它会非常糟糕。        
	 */
	walwriter_context = AllocSetContextCreate(TopMemoryContext,
											  "Wal Writer",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(walwriter_context);

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
	 * 信号掩码（即 BlockSig）在 longjmp 回此时会恢复。     
	 * 于是，除 SIGQUIT 之外的信号都会被阻塞，直到      
	 * 错误恢复完成。看起来这一策略使 HOLD_INTERRUPTS()      
	 * 调用多余，实则不然：InterruptPending 可能已置位   
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
		 * 的一个极小子集。walwriter 担心的资源不多，     
		 * 但确有 LWLock，或许还有缓冲区？                     
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

		/*
		 * 现在回到正常的顶层上下文，并清空 ErrorContext    
		 * 备用。 
		 */
		MemoryContextSwitchTo(walwriter_context);
		FlushErrorState();

		/* 刷出顶层上下文里泄漏的数据        */
		MemoryContextResetAndDeleteChildren(walwriter_context);

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
		 * Windows 上保持已删除文件打开会导致各种          
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
	 * 任何错误之后重置休眠状态。 
	 */
	left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
	hibernating = false;
	SetWalWriterSleeping(false);

	/*
	 * 公布我们的锁存器，后端可于休眠时唤醒我们。    
	 *          
	 */
	ProcGlobal->walwriterLatch = &MyProc->procLatch;

	/*
	 * 无限循环
	 */
	for (;;)
	{
		long		cur_timeout;

		/*
		 * 公布本周期内我们可能休眠与否。这一步         
		 * 要在重置锁存器之前做，以确保任何异步提交    
		 * 都可能看到标志已置位，且我们不会错过         
		 * 它们发来的任何信号。（若我们在即将休眠的     
		 * 最后一个周期发现有工作可做，全局标志会被      
		 * 无谓置位，但无大碍。）但如果标志无需         
		 * 改动就不要去碰它。                           
		 */
		if (hibernating != (left_till_hibernate <= 1))
		{
			hibernating = (left_till_hibernate <= 1);
			SetWalWriterSleeping(hibernating);
		}

		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		/* 处理近期收到的任何信号     */
		HandleWalWriterInterrupts();

		/*
		 * 做本职工作；然后，若 XLogBackgroundFlush() 发现了     
		 * 有用工作，重置休眠计数。  
		 */
		if (XLogBackgroundFlush())
			left_till_hibernate = LOOPS_UNTIL_HIBERNATE;
		else if (left_till_hibernate > 0)
			left_till_hibernate--;

		/* 把待处理的统计上报给累计统计系统         */
		pgstat_report_wal(false);

		/*
		 * 休眠直到被信号唤醒或 WalWriterDelay 到期。若我们 
		 * 已有一段时间没做任何有用的事，就延长        
		 * 休眠时间，以降低服务器空闲时的功耗。         
		 */
		if (left_till_hibernate > 0)
			cur_timeout = WalWriterDelay;	/* 毫秒*/
		else
			cur_timeout = WalWriterDelay * HIBERNATE_FACTOR;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 cur_timeout,
						 WAIT_EVENT_WAL_WRITER_MAIN);
	}
}

/*
 * WAL 写入者进程主循环的中断处理器。      
 */
static void
HandleWalWriterInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending)
		proc_exit(0);

	/* 记录本进程的内存上下文                  */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}
