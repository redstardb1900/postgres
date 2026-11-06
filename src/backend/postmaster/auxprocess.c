/*-------------------------------------------------------------------------
 * auxprocess.c
 *	  与辅助进程相关的函数。        
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/auxprocess.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walwriter.h"
#include "replication/walreceiver.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"


static void ShutdownAuxiliaryProcess(int code, Datum arg);


/* ----------------
 *		全局变量    
 * ----------------
 */

AuxProcType MyAuxProcType = NotAnAuxProcess;	/* miscadmin.h 中声明   */


/*
 *	 AuxiliaryProcessMain
 *
 *	 bgwriter、walwriter、walreceiver、bootstrapper 和共享内存  
 *	 检查代码等辅助进程的主入口点。                           
 *
 *	 这段代码放在这里只是出于历史原因。  
 */
void
AuxiliaryProcessMain(AuxProcType auxtype)
{
	Assert(IsUnderPostmaster);

	MyAuxProcType = auxtype;

	switch (MyAuxProcType)
	{
		case StartupProcess:
			MyBackendType = B_STARTUP;
			break;
		case ArchiverProcess:
			MyBackendType = B_ARCHIVER;
			break;
		case BgWriterProcess:
			MyBackendType = B_BG_WRITER;
			break;
		case CheckpointerProcess:
			MyBackendType = B_CHECKPOINTER;
			break;
		case WalWriterProcess:
			MyBackendType = B_WAL_WRITER;
			break;
		case WalReceiverProcess:
			MyBackendType = B_WAL_RECEIVER;
			break;
		default:
			elog(PANIC, "unrecognized process type: %d", (int) MyAuxProcType);
			MyBackendType = B_INVALID;
	}

	init_ps_display(NULL);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	/*
	 * 作为辅助进程，我们不做全套 InitPostgres 准备工作，
	 * 但有少数几件事即使在辅助进程中也需要点亮。       
	 *                         
	 */

	/*
	 * 创建一个 PGPROC 以便使用 LWLock。在 EXEC_BACKEND 下，     
	 * 这已由 SubPostmasterMain() 做过。 
	 */
#ifndef EXEC_BACKEND
	InitAuxiliaryProcess();
#endif

	BaseInit();

	/*
	 * 为辅助进程指派 ProcSignalSlot。由于它没有 BackendId，  
	 * 槽位按辅助进程类型（MyAuxProcType）静态分配。  
	 * 后端槽位的编号为 1 到 MaxBackends（含），故我们用    
	 * MaxBackends + AuxProcType + 1 作为辅助进程槽位的           
	 * 索引。                                                         
	 *
	 * 如果将来想要某一特定类型的多个辅助进程，           
	 * 就得重新思考。  
	 */
	ProcSignalInit(MaxBackends + MyAuxProcType + 1);

	/*
	 * 辅助进程不运行事务，但可能仍需资源所有者   
	 * 来管理事务外获取的缓冲区 pin（也许还有     
	 * 将来的其他东西）。                         
	 */
	CreateAuxProcessResourceOwner();


	/* 初始化后端状态信息           */
	pgstat_beinit();
	pgstat_bestart();

	/* 注册一个 LWLock 清理用的关闭前回调        */
	before_shmem_exit(ShutdownAuxiliaryProcess, 0);

	SetProcessingMode(NormalProcessing);

	switch (MyAuxProcType)
	{
		case StartupProcess:
			StartupProcessMain();
			proc_exit(1);

		case ArchiverProcess:
			PgArchiverMain();
			proc_exit(1);

		case BgWriterProcess:
			BackgroundWriterMain();
			proc_exit(1);

		case CheckpointerProcess:
			CheckpointerMain();
			proc_exit(1);

		case WalWriterProcess:
			WalWriterMain();
			proc_exit(1);

		case WalReceiverProcess:
			WalReceiverMain();
			proc_exit(1);

		default:
			elog(PANIC, "unrecognized process type: %d", (int) MyAuxProcType);
			proc_exit(1);
	}
}

/*
 * 开始辅助进程的关闭。这大致相当于 postinit.c 中的           
 * ShutdownPostgres()。辅助进程无法运行事务，因此         
 * AbortTransaction() 的大部分工作都无需执行，                    
 * 但确实要确保释放我们持有的所有 LWLock。               
 * （这仅在错误退出时才关键。）   
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}
