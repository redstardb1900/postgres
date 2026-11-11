/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  中断处理例程。       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"

volatile sig_atomic_t ConfigReloadPending = false;
volatile sig_atomic_t ShutdownRequestPending = false;

/*
 * 后台进程主循环用的简单中断处理器。             
 */
void
HandleMainLoopInterrupts(void)
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

	/* 记录本进程的内存上下文日志            */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();
}

/*
 * 用于触发配置重新加载的简单信号处理器。   
 *
 * 通常此处理器用于 SIGHUP。其构想是：使用它的代码   
 * 会安排在主循环便利处检查 ConfigReloadPending 标志，  
 * 否则就调用 HandleMainLoopInterrupts。                                
 */
void
SignalHandlerForConfigReload(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * 如同因崩溃而快速退出的简单信号处理器。      
 *
 * 通常它用于处理 SIGQUIT。                  
 */
void
SignalHandlerForCrashExit(SIGNAL_ARGS)
{
	/*
	 * 我们绝不想要运行 proc_exit() 或 atexit() 回调：我们    
	 * 来到这里就是因为共享内存可能已损坏，所以我们
	 * 不想试图清理事务。只需钉死窗户赶紧走人。        
	 * 反正从信号处理器里运行这些回调也不安全。       
	 *        
	 *
	 * 注意我们用 _exit(2) 而非 _exit(0)。这是为了在有人       
	 * 手动向随机后端发 SIGQUIT 时把 postmaster 逼入         
	 * 系统复位循环。这之所以必要，恰恰因为我们不    
	 * 清理共享内存状态。（pmsignal.c 的"死人开关"机制    
	 * 应能确保 postmaster 也把这视为崩溃，但多加             
	 * 保险无害。） 
	 */
	_exit(2);
}

/*
 * 用于触发长时间运行的后台进程关闭并退出的简单       
 * 信号处理器。 
 *
 * 通常此处理器用于 SIGTERM，但有些进程使用其他信号。  
 * 其中 checkpointer 与并行应用工作进程于 SIGUSR2 退出，    
 * 而 WAL writer 则在 SIGINT 或 SIGTERM 之一到达时退出。     
 *
 * 应在主循环中的便利位置检查 ShutdownRequestPending，        
 * 否则主循环就应调用 HandleMainLoopInterrupts。               
 */
void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ShutdownRequestPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}
