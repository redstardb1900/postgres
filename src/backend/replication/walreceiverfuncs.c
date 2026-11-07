/*-------------------------------------------------------------------------
 *
 * walreceiverfuncs.c
 *
 * 本文件包含启动进程用来与 walreceiver                       
 * 进程通信。实现 walreceiver 本身的函数                      
 * 见 walreceiver.c。 
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiverfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "pgstat.h"
#include "postmaster/startup.h"
#include "replication/walreceiver.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

WalRcvData *WalRcv = NULL;

/*
 * 请求 postmaster 启动 walreceiver 后等待其启动的   
 * 最长时长。以秒为单位。   
 */
#define WALRCV_STARTUP_TIMEOUT 10

/* 报告 WalRcvShmemInit 所需的共享内存空间   */
Size
WalRcvShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(WalRcvData));

	return size;
}

/* 分配并初始化 walreceiver 相关的共享内存      */
void
WalRcvShmemInit(void)
{
	bool		found;

	WalRcv = (WalRcvData *)
		ShmemInitStruct("Wal Receiver Ctl", WalRcvShmemSize(), &found);

	if (!found)
	{
		/* 首次执行到此，故初始化 */
		MemSet(WalRcv, 0, WalRcvShmemSize());
		WalRcv->walRcvState = WALRCV_STOPPED;
		ConditionVariableInit(&WalRcv->walRcvStoppedCV);
		SpinLockInit(&WalRcv->mutex);
		pg_atomic_init_u64(&WalRcv->writtenUpto, 0);
		WalRcv->latch = NULL;
	}
}

/* walreceiver 在运行（或启动）？  */
bool
WalRcvRunning(void)
{
	WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * 若 walreceiver 启动耗时过长，则放弃。将状态设为      
	 * STOPPED 即可确保：若 walreceiver 之后终究启动了，     
	 * 它会发现自己并不应该运行，于是不做                
	 * 任何事便终止。  
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			bool		stopped = false;

			SpinLockAcquire(&walrcv->mutex);
			if (walrcv->walRcvState == WALRCV_STARTING)
			{
				state = walrcv->walRcvState = WALRCV_STOPPED;
				stopped = true;
			}
			SpinLockRelease(&walrcv->mutex);

			if (stopped)
				ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
		}
	}

	if (state != WALRCV_STOPPED)
		return true;
	else
		return false;
}

/*
 * walreceiver 是否运行并正在流复制（或至少在尝试连接，
 * 或启动）？ 
 */
bool
WalRcvStreaming(void)
{
	WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * 若 walreceiver 启动耗时过长，则放弃。将状态设为      
	 * STOPPED 即可确保：若 walreceiver 之后终究启动了，     
	 * 它会发现自己并不应该运行，于是不做                
	 * 任何事便终止。  
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			bool		stopped = false;

			SpinLockAcquire(&walrcv->mutex);
			if (walrcv->walRcvState == WALRCV_STARTING)
			{
				state = walrcv->walRcvState = WALRCV_STOPPED;
				stopped = true;
			}
			SpinLockRelease(&walrcv->mutex);

			if (stopped)
				ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
		}
	}

	if (state == WALRCV_STREAMING || state == WALRCV_STARTING ||
		state == WALRCV_RESTARTING)
		return true;
	else
		return false;
}

/*
 * 停止 walreceiver（若运行）并等其终止。  
 * 由启动进程执行。        
 */
void
ShutdownWalRcv(void)
{
	WalRcvData *walrcv = WalRcv;
	pid_t		walrcvpid = 0;
	bool		stopped = false;

	/*
	 * 请求 walreceiver 停止。walreceiver 完成后会切换到         
	 * WALRCV_STOPPED 模式，同时也会请求 postmaster 不要    
	 * 重启自身。
	 */
	SpinLockAcquire(&walrcv->mutex);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPED:
			break;
		case WALRCV_STARTING:
			walrcv->walRcvState = WALRCV_STOPPED;
			stopped = true;
			break;

		case WALRCV_STREAMING:
		case WALRCV_WAITING:
		case WALRCV_RESTARTING:
			walrcv->walRcvState = WALRCV_STOPPING;
			/* fall through */
		case WALRCV_STOPPING:
			walrcvpid = walrcv->pid;
			break;
	}
	SpinLockRelease(&walrcv->mutex);

	/* 非必需但保持一致。 */
	if (stopped)
		ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/*
	 * 若 walreceiver 进程仍在运行则发信号。  
	 */
	if (walrcvpid != 0)
		kill(walrcvpid, SIGTERM);

	/*
	 * 等待 walreceiver 通过将状态设为 WALRCV_STOPPED 来      
	 * 确认死亡。
	 */
	ConditionVariablePrepareToSleep(&walrcv->walRcvStoppedCV);
	while (WalRcvRunning())
		ConditionVariableSleep(&walrcv->walRcvStoppedCV,
							   WAIT_EVENT_WAL_RECEIVER_EXIT);
	ConditionVariableCancelSleep();
}

/*
 * 请求 postmaster 启动 walreceiver。 
 *
 * "recptr" 指示流复制应开始的位置。"conninfo"                  
 * 是要使用的 libpq 连接串。"slotname"（可选）是要获取     
 * 的复制槽的名称。"create_temp_slot" 表示在未给定            
 * "slotname" 时创建临时槽。             
 *
 * WAL 接收器不直接加载用于连接到主库的 GUC 参数，       
 * 而是依赖本例程的调用者向下传递的值。因此，任何   
 * 新参数的添加都应当经由                                        
 * 此代码路径。     
 */
void
RequestXLogStreaming(TimeLineID tli, XLogRecPtr recptr, const char *conninfo,
					 const char *slotname, bool create_temp_slot)
{
	WalRcvData *walrcv = WalRcv;
	bool		launch = false;
	pg_time_t	now = (pg_time_t) time(NULL);
	Latch	   *latch;

	/*
	 * 我们总是从段的开头开始。这样可以防止 XLOG 流复制  
	 * 创建出损坏的段（即前半段无记录），日后如果    
	 * 该段例如被归档，就可能会引起                            
	 * 麻烦。                   
	 */
	if (XLogSegmentOffset(recptr, wal_segment_size) != 0)
		recptr -= XLogSegmentOffset(recptr, wal_segment_size);

	SpinLockAcquire(&walrcv->mutex);

	/* 若要重启它，它最好已停止。      */
	Assert(walrcv->walRcvState == WALRCV_STOPPED ||
		   walrcv->walRcvState == WALRCV_WAITING);

	if (conninfo != NULL)
		strlcpy((char *) walrcv->conninfo, conninfo, MAXCONNINFO);
	else
		walrcv->conninfo[0] = '\0';

	/*
	 * 若存在已配置的复制槽则使用之，并忽略             
	 * create_temp_slot 的值，因为该槽名应当是持久的。         
	 * 否则，用 create_temp_slot 决定是否由该 WAL 接收器         
	 * 自行创建临时槽并使用。           
	 */
	if (slotname != NULL && slotname[0] != '\0')
	{
		strlcpy((char *) walrcv->slotname, slotname, NAMEDATALEN);
		walrcv->is_temp_slot = false;
	}
	else
	{
		walrcv->slotname[0] = '\0';
		walrcv->is_temp_slot = create_temp_slot;
	}

	if (walrcv->walRcvState == WALRCV_STOPPED)
	{
		launch = true;
		walrcv->walRcvState = WALRCV_STARTING;
	}
	else
		walrcv->walRcvState = WALRCV_RESTARTING;
	walrcv->startTime = now;

	/*
	 * 如果这是 walreceiver 首次启动（本时间线上），  
	 * 则把 flushedUpto 和 latestChunkStart 初始化为起始点。  
	 */
	if (walrcv->receiveStart == 0 || walrcv->receivedTLI != tli)
	{
		walrcv->flushedUpto = recptr;
		walrcv->receivedTLI = tli;
		walrcv->latestChunkStart = recptr;
	}
	walrcv->receiveStart = recptr;
	walrcv->receiveStartTLI = tli;

	latch = walrcv->latch;

	SpinLockRelease(&walrcv->mutex);

	if (launch)
		SendPostmasterSignal(PMSIGNAL_START_WALRECEIVER);
	else if (latch)
		SetLatch(latch);
}

/*
 * 返回 walreceiver 已刷写的 last+1 字节位置。        
 *
 * 可选地，还返回上一个 chunk 的起始位置，即          
 * 最近一次 walreceiver 刷写周期中写入的首个字节。
 * 不关心它的调用者可为 latestChunkStart 传 NULL，receiveTLI
 * 亦然。  
 */
XLogRecPtr
GetWalRcvFlushRecPtr(XLogRecPtr *latestChunkStart, TimeLineID *receiveTLI)
{
	WalRcvData *walrcv = WalRcv;
	XLogRecPtr	recptr;

	SpinLockAcquire(&walrcv->mutex);
	recptr = walrcv->flushedUpto;
	if (latestChunkStart)
		*latestChunkStart = walrcv->latestChunkStart;
	if (receiveTLI)
		*receiveTLI = walrcv->receivedTLI;
	SpinLockRelease(&walrcv->mutex);

	return recptr;
}

/*
 * 返回 walreceiver 已写入的 last+1 字节位置。        
 * 此函数不加锁即返回一个近期写入的值。      
 */
XLogRecPtr
GetWalRcvWriteRecPtr(void)
{
	WalRcvData *walrcv = WalRcv;

	return pg_atomic_read_u64(&walrcv->writtenUpto);
}

/*
 * 返回复制应用延迟的毫秒数或 -1     
 * （若无应用延迟信息）。       
 */
int
GetReplicationApplyDelay(void)
{
	WalRcvData *walrcv = WalRcv;
	XLogRecPtr	receivePtr;
	XLogRecPtr	replayPtr;
	TimestampTz chunkReplayStartTime;

	SpinLockAcquire(&walrcv->mutex);
	receivePtr = walrcv->flushedUpto;
	SpinLockRelease(&walrcv->mutex);

	replayPtr = GetXLogReplayRecPtr(NULL);

	if (receivePtr == replayPtr)
		return 0;

	chunkReplayStartTime = GetCurrentChunkReplayStartTime();

	if (chunkReplayStartTime == 0)
		return -1;

	return TimestampDifferenceMilliseconds(chunkReplayStartTime,
										   GetCurrentTimestamp());
}

/*
 * 返回以毫秒计的网络延迟，注意其中包含        
 * 服务器间时钟设置的任何差异，以及时区差异。       
 */
int
GetReplicationTransferLatency(void)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz lastMsgSendTime;
	TimestampTz lastMsgReceiptTime;

	SpinLockAcquire(&walrcv->mutex);
	lastMsgSendTime = walrcv->lastMsgSendTime;
	lastMsgReceiptTime = walrcv->lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	return TimestampDifferenceMilliseconds(lastMsgSendTime,
										   lastMsgReceiptTime);
}
