/*-------------------------------------------------------------------------
 *
 * bgwriter.c
 *
 * 后台写入进程（bgwriter）为 Postgres 8.0 新增。它尽力     
 * 让普通后端不必写出脏的共享缓冲区（只有当后端    
 * 需要腾出共享缓冲区读入另一页时才会这样做）。       
 * 最理想的情形下，共享缓冲区的所有写出都由后台写入
 * 进程发出。不过，若 bgwriter 未能维持足够的干净共享    
 * 缓冲区，普通后端仍有权自行发出写出。                  
 *                      
 *
 * 从 Postgres 9.2 起，bgwriter 不再负责检查点。      
 *
 * 正常终止靠 SIGTERM，它指示 bgwriter 执行 exit(0)。            
 * 紧急终止靠 SIGQUIT；与其他后端一样，bgwriter 收到       
 * SIGQUIT 即中止并退出。    
 *
 * 若 bgwriter 意外退出，postmaster 会将其视同                 
 * 后端崩溃：共享内存可能已损坏，因此要用 SIGQUIT        
 * 杀掉其余后端，随后启动一轮恢复周期。        
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgwriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "storage/buf_internals.h"
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
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/*
 * GUC 参数    
 */
int			BgWriterDelay = 200;

/*
 * 决定休眠时对 BgWriterDelay 应用的倍率。              
 * （或许应做成可配置？）       
 */
#define HIBERNATE_FACTOR			50

/*
 * 备库快照写入 WAL 流的时间间隔，单位为                 
 * 毫秒。    
 */
#define LOG_SNAPSHOT_INTERVAL_MS 15000

/*
 * 上次调用 LogStandbySnapshot() 时的 LSN 与时间戳，用于避免  
 * 过于频繁地或在系统没有其他写活动时反复调用。        
 *               
 */
static TimestampTz last_snapshot_ts;
static XLogRecPtr last_snapshot_lsn = InvalidXLogRecPtr;


/*
 * bgwriter 进程的主入口点       
 *
 * 本函数由 AuxiliaryProcessMain 调用，后者已创建              
 * 基本执行环境，但尚未启用信号。            
 */
void
BackgroundWriterMain(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;
	bool		prev_hibernate;
	WritebackContext wb_context;

	/*
	 * 恰当地接受或忽略可能发给我们的信号。     
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * 重置一些 postmaster 接受但此处不接受的信号      
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 我们刚刚启动，假定此前发生的是关闭或     
	 * 恢复结束快照。    
	 */
	last_snapshot_ts = GetCurrentTimestamp();

	/*
	 * 创建一个内存上下文，我们所有工作都在其中进行。  
	 * 这样在错误恢复期间可以重置该上下文，从而避免   
	 * 可能的内存泄漏。以前这些代码直接在   
	 * TopMemoryContext 中运行，重置它会非常糟糕。        
	 */
	bgwriter_context = AllocSetContextCreate(TopMemoryContext,
											 "Background Writer",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgwriter_context);

	WritebackContextInit(&wb_context, &bgwriter_flush_after);

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
	 * 调用多余，实则不然：InterruptPending 可能已置位。
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
		 * 的一个极小子集。bgwriter 担心的资源不多，      
		 * 但有 LWLock、缓冲区和临时文件。                        
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
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
		MemoryContextSwitchTo(bgwriter_context);
		FlushErrorState();

		/* 刷出顶层上下文里泄漏的数据        */
		MemoryContextResetAndDeleteChildren(bgwriter_context);

		/* 重新初始化以免重复错误造成问题           */
		WritebackContextInit(&wb_context, &bgwriter_flush_after);

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

		/* 在此报告等待结束，此后已无再次等待的可能       */
		pgstat_report_wait_end();
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
	prev_hibernate = false;

	/*
	 * 无限循环
	 */
	for (;;)
	{
		bool		can_hibernate;
		int			rc;

		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		HandleMainLoopInterrupts();

		/*
		 * 执行一轮脏缓冲区写出。    
		 */
		can_hibernate = BgBufferSync(&wb_context);

		/* 把待处理的统计上报给累计统计系统         */
		pgstat_report_bgwriter();
		pgstat_report_wal(true);

		if (FirstCallSinceLastCheckpoint())
		{
			/*
			 * 检查点之后关闭所有 smgr 文件，以免          
			 * 无限期保留对已删除文件的 smgr 引用。           
			 */
			smgrcloseall();
		}

		/*
		 * 不定期记录一条新的 xl_running_xacts，让复制        
		 * 更快达到一致状态（想想子溢出快照），    
		 * 并更频繁地清理资源（锁、KnownXids*）。      
		 * 这样做的成本相对较低，每分钟做 4 次            
		 * （LOG_SNAPSHOT_INTERVAL_MS）就挺好。           
		 *
		 * 我们假定写入 xl_running_xacts 的间隔         
		 * 远大于 BgWriterDelay，因此不把超时处理搞复杂，     
		 * 而是只管假定我们会被足够频繁地调用，            
		 * 即便休眠模式激活也无妨。严格满足              
		 * LOG_SNAPSHOT_INTERVAL_MS 并非那么重要。为了            
		 * 确保在空闲系统上不会不必要地唤醒磁盘，        
		 * 我们检查自上次记录 running xacts 以来是否有      
		 * WAL 被插入。                  
		 *
		 * 这项记录放在 bgwriter 中做，因为它是唯一一个    
		 * 定期运行且总是回到主循环的进程。例如         
		 * Checkpointer 活动时几乎从不在主循环里，因此        
		 * 难以定期记录。          
		 */
		if (XLogStandbyInfoActive() && !RecoveryInProgress())
		{
			TimestampTz timeout = 0;
			TimestampTz now = GetCurrentTimestamp();

			timeout = TimestampTzPlusMilliseconds(last_snapshot_ts,
												  LOG_SNAPSHOT_INTERVAL_MS);

			/*
			 * 只有经过足够时间、且上一快照后插入过         
			 * 有意义的记录才记录。必须用 <= 而非 < 比较，  
			 * 因为 GetLastImportantRecPtr() 指向记录的开头，   
			 * 而 last_snapshot_lsn 指向的却是记录结尾之后      
			 * 一个字节。       
			 */
			if (now >= timeout &&
				last_snapshot_lsn <= GetLastImportantRecPtr())
			{
				last_snapshot_lsn = LogStandbySnapshot();
				last_snapshot_ts = now;
			}
		}

		/*
		 * 休眠直到被信号唤醒或 BgWriterDelay 到期。   
		 *
		 * 注意：BgBufferSync() 中的反馈控制环路期望我们     
		 * 每 BgWriterDelay 毫秒调用它一次。虽然这对正确性    
		 * 并非关键，但偏离太远反馈环路可能失常。        
		 * 因此，避免让本进程承载正常操作期间              
		 * 很可能频繁发生的锁存器事件。                        
		 *                  
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   BgWriterDelay /* ms */ , WAIT_EVENT_BGWRITER_MAIN);

		/*
		 * 若没有锁存器事件、且 BgBufferSync 说毫无动静，     
		 * 就把休眠延长到"冬眠"模式，睡得远比             
		 * bgwriter_delay 所说的更久。更少的唤醒节省电能。   
		 * 当后端重新开始使用缓冲区时，它会设置           
		 * 我们的锁存器唤醒我们。由于额外休眠只在没有    
		 * 缓冲区分配发生期间持续，所以它不应严重扭曲   
		 * BgBufferSync 的控制环路；本质上，它会认为          
		 * 系统级空闲时段从未存在过。         
		 *
		 * 这里存在竞争条件：后端可能在                        
		 * BgBufferSync 看到分配计数为零的时刻与我们调用   
		 * StrategyNotifyBgWriter 的时刻之间分配缓冲区。    
		 * 虽然不冬眠也并非关键，但我们仍设法降低这种    
		 * 概率：只在 BgBufferSync 连续两个周期都报告           
		 * 毫无动静时才冬眠。另外，我们通过不永久  
		 * 冬眠来缓解错过唤醒的一切可能后果。        
		 */
		if (rc == WL_TIMEOUT && can_hibernate && prev_hibernate)
		{
			/* 请求下次缓冲区分配时得到通知     */
			StrategyNotifyBgWriter(MyProc->pgprocno);
			/* 休眠... */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 BgWriterDelay * HIBERNATE_FACTOR,
							 WAIT_EVENT_BGWRITER_HIBERNATE);
			/* 若已超时则重置通知请求                   */
			StrategyNotifyBgWriter(-1);
		}

		prev_hibernate = can_hibernate;
	}
}
