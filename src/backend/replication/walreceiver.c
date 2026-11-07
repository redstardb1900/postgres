/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * WAL 接收进程（walreceiver）自 Postgres 9.0 起               
 * 才出现。它是备库中负责在流复制期间从主库       
 * 接收 XLOG 记录的进程。                                  
 *
 * 当启动进程判定该开始流复制时，它会指示             
 * postmaster 启动 walreceiver。walreceiver 首先连接到             
 * 主库（将由主库中的一个 walsender 进程为其       
 * 服务），然后只要连接存活，就持续接收 XLOG      
 * 记录并将其写入磁盘。随着 XLOG 记录被接收           
 * 并刷写到磁盘，它会更新共享内存中的     
 * WalRcv->flushedUpto 变量，以告知启动进程                  
 * XLOG 重放可以进行到多远。                
 *
 * WAL 接收进程无法直接加载在建立与主库的连接            
 * 时所用的 GUC 参数。它转而依赖于在请求流复制      
 * 时由启动进程传递下来的参数值。例如，用于与         
 * 主库建立连接的复制槽和连接串就适用                  
 * 此规则。                                          
 *
 * 如果主库结束流式发送但不断开连接，walreceiver           
 * 会进入"等待"模式，等待启动进程给出新的指令。      
 * 启动进程会把这种情况等同于断开连接来       
 * 处理，并重新扫描 archive/pg_wal 目录。但当                  
 * 启动进程想再次尝试流复制时，它只会轻推一下       
 * 正在等待的既有 walreceiver 进程，而不是启动一个           
 * 新的。 
 *
 * 正常终止由 SIGTERM 完成，它会指示 walreceiver           
 * 调用 exit(0)。紧急终止由 SIGQUIT 完成；与任何            
 * postmaster 子进程一样，walreceiver 在收到 SIGQUIT 时         
 * 会中止退出。连接关闭和 FATAL 错误视为正常操作，  
 * 而非崩溃。  
 *
 * 本文件包含 walreceiver 中面向服务器的部分。libpq      
 * 相关部分在 libpqwalreceiver 模块中。它被动态     
 * 加载，以避免服务器与 libpq 链接。     
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"


/*
 * GUC 变量。（其他影响 walreceiver 的变量位于 xlog.c       
 * 中，因为它们由启动进程传递下来，以求更好    
 * 的同步。）  
 */
int			wal_receiver_status_interval;
int			wal_receiver_timeout;
bool		hot_standby_feedback;

/* libpqwalreceiver 连接     */
static WalReceiverConn *wrconn = NULL;
WalReceiverFunctionsType *WalReceiverFunctions = NULL;

/*
 * 这些变量用法与 openLogFile/SegNo 类似，但    
 * 用于 walreceiver 写入 XLOG。recvFileTLI 是与 recvFile        
 * 文件名对应的时间线 ID。      
 */
static int	recvFile = -1;
static TimeLineID recvFileTLI = 0;
static XLogSegNo recvSegNo = 0;

/*
 * LogstreamResult 指示我们已经写出或 fsync 过的         
 * 字节位置。 
 */
static struct
{
	XLogRecPtr	Write;			/* 备库中已写出的最后字节 + 1    */
	XLogRecPtr	Flush;			/* 备库中已刷写的最后字节 + 1*/
}			LogstreamResult;

/*
 * 唤醒并执行周期性任务的原因。    
 */
typedef enum WalRcvWakeupReason
{
	WALRCV_WAKEUP_TERMINATE,
	WALRCV_WAKEUP_PING,
	WALRCV_WAKEUP_REPLY,
	WALRCV_WAKEUP_HSFEEDBACK
#define NUM_WALRCV_WAKEUPS (WALRCV_WAKEUP_HSFEEDBACK + 1)
} WalRcvWakeupReason;

/*
 * 周期任务的唤醒时刻。   
 */
static TimestampTz wakeup[NUM_WALRCV_WAKEUPS];

static StringInfoData reply_message;
static StringInfoData incoming_message;

/* 私有函数的原型            */
static void WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last);
static void WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI);
static void WalRcvDie(int code, Datum arg);
static void XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len,
								 TimeLineID tli);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr,
							TimeLineID tli);
static void XLogWalRcvFlush(bool dying, TimeLineID tli);
static void XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli);
static void XLogWalRcvSendReply(bool force, bool requestReply);
static void XLogWalRcvSendHSFeedback(bool immed);
static void ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime);
static void WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now);

/*
 * 处理 walreceiver 进程可能已收到的任何中断。只要 
 * 进程的锁存器已被置位，就应当调用本函数。      
 *
 * 目前只有 SIGTERM 值得关注。我们无法直接在                
 * SIGTERM 信号处理函数中调用 exit(1)，因为信号              
 * 可能出现在某个关键操作的进行途中，比如我们正         
 * 持有自旋锁时。因此信号处理函数除了设置进程的         
 * 锁存器之外，还会设置一个标志变量。只要锁存器     
 * 已置位，就必须调用 ProcessWalRcvInterrupts 检查该标志。     
 * 可能长时间阻塞操作（如远端读取）须留意锁存器； 
 * 见 libpqrcv_PQgetResult。      
 */
void
ProcessWalRcvInterrupts(void)
{
	/*
	 * 虽然 walreceiver 的中断处理与常规后端不同，但仍      
	 * 要调用 CHECK_FOR_INTERRUPTS()，以确保我们在                
	 * Win32 上能收到任何到达的信号，并处理任何屏障    
	 * 事件。      
	 */
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(FATAL,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("terminating walreceiver process due to administrator command")));
	}
}


/* walreceiver 进程的主入口点        */
void
WalReceiverMain(void)
{
	char		conninfo[MAXCONNINFO];
	char	   *tmp_conninfo;
	char		slotname[NAMEDATALEN];
	bool		is_temp_slot;
	XLogRecPtr	startpoint;
	TimeLineID	startpointTLI;
	TimeLineID	primaryTLI;
	bool		first_stream;
	WalRcvData *walrcv = WalRcv;
	TimestampTz now;
	char	   *err;
	char	   *sender_host = NULL;
	int			sender_port = 0;

	/*
	 * WalRcv 应当已经设置好（如果我们是后端，则通过     
	 * fork() 或 EXEC_BACKEND 从 postmaster 继承它）。   
	 */
	Assert(walrcv != NULL);

	/*
	 * 标记 walreceiver 在共享内存运行。  
	 *
	 * 应尽早执行此操作，这样即便稍后失败，我们也会  
	 * 将状态置为 STOPPED。若在此之前死掉，启动进程        
	 * 会一直等待我们启动，直到超时。  
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPING:
			/* 如果已被要求停止，就不要启动。            */
			walrcv->walRcvState = WALRCV_STOPPED;
			/* fall through */

		case WALRCV_STOPPED:
			SpinLockRelease(&walrcv->mutex);
			ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
			proc_exit(1);
			break;

		case WALRCV_STARTING:
			/* 常见情形   */
			break;

		case WALRCV_WAITING:
		case WALRCV_STREAMING:
		case WALRCV_RESTARTING:
		default:
			/* 不应发生     */
			SpinLockRelease(&walrcv->mutex);
			elog(PANIC, "walreceiver still running according to shared memory state");
	}
	/* 通告我们的 PID，以便启动进程能终止我们   */
	walrcv->pid = MyProcPid;
	walrcv->walRcvState = WALRCV_STREAMING;

	/* 获取开始流复制所需的信息          */
	walrcv->ready_to_display = false;
	strlcpy(conninfo, (char *) walrcv->conninfo, MAXCONNINFO);
	strlcpy(slotname, (char *) walrcv->slotname, NAMEDATALEN);
	is_temp_slot = walrcv->is_temp_slot;
	startpoint = walrcv->receiveStart;
	startpointTLI = walrcv->receiveStartTLI;

	/*
	 * is_temp_slot 与 slotname 至多只能设置其一；否则    
	 * RequestXLogStreaming 出错。 
	 */
	Assert(!is_temp_slot || (slotname[0] == '\0'));

	/* 初始化为一个合理值  */
	now = GetCurrentTimestamp();
	walrcv->lastMsgSendTime =
		walrcv->lastMsgReceiptTime = walrcv->latestWalEndTime = now;

	/* 报告用于唤醒本进程的锁存器        */
	walrcv->latch = &MyProc->procLatch;

	SpinLockRelease(&walrcv->mutex);

	pg_atomic_write_u64(&WalRcv->writtenUpto, 0);

	/* 安排在 walreceiver 退出时清理   */
	on_shmem_exit(WalRcvDie, PointerGetDatum(&startpointTLI));

	/* 恰当接受或忽略 postmaster 可能发给我们的信号   */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* 请求关闭     */
	/* SIGQUIT 处理函数已由 InitPostmasterChild 设置好  */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/* 重置一些由 postmaster 接受但这里不接受的信号    */
	pqsignal(SIGCHLD, SIG_DFL);

	/* 加载 libpq 专有的函数      */
	load_file("libpqwalreceiver", false);
	if (WalReceiverFunctions == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	/* 解除信号阻塞（postmaster fork 我们时它们被阻塞）  */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* 建立与主库的连接以进行 XLOG 流复制           */
	wrconn = walrcv_connect(conninfo, false, false,
							cluster_name[0] ? cluster_name : "walreceiver",
							&err);
	if (!wrconn)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the primary server: %s", err)));

	/*
	 * 保存用户可见的连接串。出于安全考虑，这       
	 * 会覆盖原始 conninfo。也保存 walreceiver 所连接发送端 
	 * 服务器的主机与端口。   
	 */
	tmp_conninfo = walrcv_get_conninfo(wrconn);
	walrcv_get_senderinfo(wrconn, &sender_host, &sender_port);
	SpinLockAcquire(&walrcv->mutex);
	memset(walrcv->conninfo, 0, MAXCONNINFO);
	if (tmp_conninfo)
		strlcpy((char *) walrcv->conninfo, tmp_conninfo, MAXCONNINFO);

	memset(walrcv->sender_host, 0, NI_MAXHOST);
	if (sender_host)
		strlcpy((char *) walrcv->sender_host, sender_host, NI_MAXHOST);

	walrcv->sender_port = sender_port;
	walrcv->ready_to_display = true;
	SpinLockRelease(&walrcv->mutex);

	if (tmp_conninfo)
		pfree(tmp_conninfo);

	if (sender_host)
		pfree(sender_host);

	first_stream = true;
	for (;;)
	{
		char	   *primary_sysid;
		char		standby_sysid[32];
		WalRcvStreamOptions options;

		/*
		 * 检查所连接的服务器有效，使用的是      
		 * IDENTIFY_SYSTEM 复制命令。     
		 */
		primary_sysid = walrcv_identify_system(wrconn, &primaryTLI);

		snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
				 GetSystemIdentifier());
		if (strcmp(primary_sysid, standby_sysid) != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database system identifier differs between the primary and standby"),
					 errdetail("The primary's identifier is %s, the standby's identifier is %s.",
							   primary_sysid, standby_sysid)));
		}

		/*
		 * 确认主库的当前时间线与我们的一致，或在其   
		 * 之前。     
		 */
		if (primaryTLI < startpointTLI)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("highest timeline %u of the primary is behind recovery timeline %u",
							primaryTLI, startpointTLI)));

		/*
		 * 获取任何缺少的历史文件。我们总是执行此        
		 * 操作，即使我们对那条时间线不感兴趣，这样 
		 * 当我们稍后被提升为主库时，就不会选择当前       
		 * 主库已经用过的时间线。这并非万无一失            
		 *  - 如果需要确保每种情况下都选择唯一的          
		 * 时间线 ID，就需要一些外部软件来管理集簇，     
		 * 但让我们在能做到的范围内避免时间线 ID 冲突   
		 * 。 
		 */
		WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);

		/*
		 * 如果被要求则创建临时复制槽，并更新共享内   
		 * 存中的槽名。（注意：此时槽名不可能已被设置  
		 * 。）        
		 */
		if (is_temp_slot)
		{
			snprintf(slotname, sizeof(slotname),
					 "pg_walreceiver_%lld",
					 (long long int) walrcv_get_backend_pid(wrconn));

			walrcv_create_slot(wrconn, slotname, true, false, 0, NULL);

			SpinLockAcquire(&walrcv->mutex);
			strlcpy(walrcv->slotname, slotname, NAMEDATALEN);
			SpinLockRelease(&walrcv->mutex);
		}

		/*
		 * 开始流复制 
		 *
		 * 我们会尝试从所请求的起始点和时间线开始，即 
		 * 使它不同于服务器最新的时间线。如果我们已经  
		 * 到达旧时间线的末尾，服务器会立即结束流式发   
		 * 送，我们将返回去等待启动进程的指令。如果  
		 * recovery_target_timeline 为'latest'，启动进程会扫描   
		 * pg_wal 并找到新的历史文件，调高恢复目标时间  
		 * 线，并要求我们在新的时间线上                        
		 * 重新开始。     
		 */
		options.logical = false;
		options.startpoint = startpoint;
		options.slotname = slotname[0] != '\0' ? slotname : NULL;
		options.proto.physical.startpointTLI = startpointTLI;
		if (walrcv_startstreaming(wrconn, &options))
		{
			if (first_stream)
				ereport(LOG,
						(errmsg("started streaming WAL from primary at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			else
				ereport(LOG,
						(errmsg("restarted WAL streaming at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			first_stream = false;

			/* 初始化 LogstreamResult 与缓冲区以处理消息          */
			LogstreamResult.Write = LogstreamResult.Flush = GetXLogReplayRecPtr(NULL);
			initStringInfo(&reply_message);
			initStringInfo(&incoming_message);

			/* 初始化打盹唤醒时间  */
			now = GetCurrentTimestamp();
			for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
				WalRcvComputeNextWakeup(i, now);

			/* 发送初始的应答/反馈消息    */
			XLogWalRcvSendReply(true, false);
			XLogWalRcvSendHSFeedback(true);

			/* 循环直到流复制结束或出错 */
			for (;;)
			{
				char	   *buf;
				int			len;
				bool		endofwal = false;
				pgsocket	wait_fd = PGINVALID_SOCKET;
				int			rc;
				TimestampTz nextWakeup;
				long		nap;

				/*
				 * 如果不在恢复中，则退出 walreceiver。本不应 
				 * 发生，但仍交叉检查状态。    
				 */
				if (!RecoveryInProgress())
					ereport(FATAL,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot continue WAL streaming, recovery has already ended")));

				/* 处理最近收到的任何请求或信号        */
				ProcessWalRcvInterrupts();

				if (ConfigReloadPending)
				{
					ConfigReloadPending = false;
					ProcessConfigFile(PGC_SIGHUP);
					/* 重算唤醒时刻     */
					now = GetCurrentTimestamp();
					for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
						WalRcvComputeNextWakeup(i, now);
					XLogWalRcvSendHSFeedback(true);
				}

				/* 看看能否立即读到数据      */
				len = walrcv_receive(wrconn, &buf, &wait_fd);
				if (len != 0)
				{
					/*
					 * 处理收到的数据，以及无需阻塞即可     
					 * 读取的后续数据。  
					 */
					for (;;)
					{
						if (len > 0)
						{
							/*
							 * 已从主库收到数据，因此调整 ping  
							 * 与终止唤醒时刻。            
							 */
							now = GetCurrentTimestamp();
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_TERMINATE,
													now);
							WalRcvComputeNextWakeup(WALRCV_WAKEUP_PING, now);
							XLogWalRcvProcessMsg(buf[0], &buf[1], len - 1,
												 startpointTLI);
						}
						else if (len == 0)
							break;
						else if (len < 0)
						{
							ereport(LOG,
									(errmsg("replication terminated by primary server"),
									 errdetail("End of WAL reached on timeline %u at %X/%X.",
											   startpointTLI,
											   LSN_FORMAT_ARGS(LogstreamResult.Write))));
							endofwal = true;
							break;
						}
						len = walrcv_receive(wrconn, &buf, &wait_fd);
					}

					/* 让主库知道我们收到了一些数据。    */
					XLogWalRcvSendReply(false, false);

					/*
					 * 若写入一些记录，则将其刷写到磁盘，  
					 * 并让启动进程与主库得知这些记录        
					 * 。  
					 */
					XLogWalRcvFlush(false, startpointTLI);
				}

				/* 检查是否需要退出流复制循环。   */
				if (endofwal)
					break;

				/* 找出最早的唤醒时刻，以限制打盹。*/
				nextWakeup = TIMESTAMP_INFINITY;
				for (int i = 0; i < NUM_WALRCV_WAKEUPS; ++i)
					nextWakeup = Min(wakeup[i], nextWakeup);

				/* 计算打盹时长，必要时进行钳制。  */
				now = GetCurrentTimestamp();
				nap = TimestampDifferenceMilliseconds(now, nextWakeup);

				/*
				 * 理想情况下，我们希望在此反复使用同一 
				 * WaitEventSet 对象，以避免在 epoll 系统上       
				 * WaitLatchOrSocket 的开销，但无法确定 libpq   
				 * （或任何其他 walreceiver 实现）的套接字    
				 * 是否相同（即使 fd 编号相同，也可能自  
				 * 上次被关闭又重新打开）。将来若有        
				 * 从 WaitEventSet 移除套接字的函数，那么我   
				 * 们每次就只添加与移除该套接字，从而可     
				 * 省一些系统调用。   
				 */
				Assert(wait_fd != PGINVALID_SOCKET);
				rc = WaitLatchOrSocket(MyLatch,
									   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_LATCH_SET,
									   wait_fd,
									   nap,
									   WAIT_EVENT_WAL_RECEIVER_MAIN);
				if (rc & WL_LATCH_SET)
				{
					ResetLatch(MyLatch);
					ProcessWalRcvInterrupts();

					if (walrcv->force_reply)
					{
						/*
						 * 恢复进程要求我们发送 apply 反馈。 
						 * 发送应答前，务必确保共享内存中的  
						 * 标志确实置为 false，以免错过新的应   
						 * 答请求。                            
						 */
						walrcv->force_reply = false;
						pg_memory_barrier();
						XLogWalRcvSendReply(true, false);
					}
				}
				if (rc & WL_TIMEOUT)
				{
					/*
					 * 未收到任何新内容。若已超过            
					 * wal_receiver_timeout / 2 的时间    
					 * 未收到服务器消息，就 ping 服务器。   
					 * 此外，若距离上次发送更新已超过     
					 * wal_receiver_status_interval，也要向主库发送  
					 * 状态更新，以报告 WAL 应用方面的进展    
					 * 。 
					 */
					bool		requestReply = false;

					/*
					 * 检查距上次从主库接收是否已达       
					 * 到配置的限制。        
					 */
					now = GetCurrentTimestamp();
					if (now >= wakeup[WALRCV_WAKEUP_TERMINATE])
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("terminating walreceiver due to timeout")));

					/*
					 * 若在接收端复制超时的一半时间内没有   
					 * 收到新内容，就 ping 服务器。   
					 */
					if (now >= wakeup[WALRCV_WAKEUP_PING])
					{
						requestReply = true;
						wakeup[WALRCV_WAKEUP_PING] = TIMESTAMP_INFINITY;
					}

					XLogWalRcvSendReply(requestReply, requestReply);
					XLogWalRcvSendHSFeedback(false);
				}
			}

			/*
			 * 后端已结束流复制。我们也从己方退出流式    
			 * COPY 模式   
			 */
			walrcv_endstreaming(wrconn, &primaryTLI);

			/*
			 * 如果服务器已切换到我们开始流复制时尚不  
			 * 知道的新时间线，现在就获取其时间线历史     
			 * 文件   
			 */
			WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);
		}
		else
			ereport(LOG,
					(errmsg("primary server contains no more WAL on requested timeline %u",
							startpointTLI)));

		/*
		 * 在所请求的时间线上已到达 WAL 末尾。关闭    
		 * 最后一个段，然后等待启动进程的新指令。  
		 */
		if (recvFile >= 0)
		{
			char		xlogfname[MAXFNAMELEN];

			XLogWalRcvFlush(false, startpointTLI);
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			if (close(recvFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not close WAL segment %s: %m",
								xlogfname)));

			/*
			 * 强制创建 .done 文件，以防流式接收的段稍后     
			 * 被归档。         
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(xlogfname);
			else
				XLogArchiveNotify(xlogfname);
		}
		recvFile = -1;

		elog(DEBUG1, "walreceiver ended streaming and awaits new instructions");
		WalRcvWaitForStartPosition(&startpoint, &startpointTLI);
	}
	/* not reached */
}

/*
 * 等待启动进程设置 receiveStart 与 receiveStartTLI。     
 */
static void
WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI)
{
	WalRcvData *walrcv = WalRcv;
	int			state;

	SpinLockAcquire(&walrcv->mutex);
	state = walrcv->walRcvState;
	if (state != WALRCV_STREAMING)
	{
		SpinLockRelease(&walrcv->mutex);
		if (state == WALRCV_STOPPING)
			proc_exit(0);
		else
			elog(FATAL, "unexpected walreceiver state");
	}
	walrcv->walRcvState = WALRCV_WAITING;
	walrcv->receiveStart = InvalidXLogRecPtr;
	walrcv->receiveStartTLI = 0;
	SpinLockRelease(&walrcv->mutex);

	set_ps_display("idle");

	/*
	 * 轻推启动进程，让其注意到我们已停止流复制，     
	 * 现在正在等待指令。  
	 */
	WakeupRecovery();
	for (;;)
	{
		ResetLatch(MyLatch);

		ProcessWalRcvInterrupts();

		SpinLockAcquire(&walrcv->mutex);
		Assert(walrcv->walRcvState == WALRCV_RESTARTING ||
			   walrcv->walRcvState == WALRCV_WAITING ||
			   walrcv->walRcvState == WALRCV_STOPPING);
		if (walrcv->walRcvState == WALRCV_RESTARTING)
		{
			/*
			 * 无需在这里处理 primary_conninfo 或      
			 * primary_slot_name 变化。若这些发生改变，启   
			 * 动进程会发信号终止。 
			 */
			*startpoint = walrcv->receiveStart;
			*startpointTLI = walrcv->receiveStartTLI;
			walrcv->walRcvState = WALRCV_STREAMING;
			SpinLockRelease(&walrcv->mutex);
			break;
		}
		if (walrcv->walRcvState == WALRCV_STOPPING)
		{
			/*
			 * 如果启动进程想让我们终止，就应该已收到    
			 * SIGTERM，但在这里再检查一下。    
			 */
			SpinLockRelease(&walrcv->mutex);
			exit(1);
		}
		SpinLockRelease(&walrcv->mutex);

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_WAL_RECEIVER_WAIT_START);
	}

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "restarting at %X/%X",
				 LSN_FORMAT_ARGS(*startpoint));
		set_ps_display(activitymsg);
	}
}

/*
 * 从服务器获取 'first' 与 'last' 之间（含两端）缺     
 * 失时间线历史文件。 
 */
static void
WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last)
{
	TimeLineID	tli;

	for (tli = first; tli <= last; tli++)
	{
		/* 时间线 1 没有历史文件         */
		if (tli != 1 && !existsTimeLineHistory(tli))
		{
			char	   *fname;
			char	   *content;
			int			len;
			char		expectedfname[MAXFNAMELEN];

			ereport(LOG,
					(errmsg("fetching timeline history file for timeline %u from primary server",
							tli)));

			walrcv_readtimelinehistoryfile(wrconn, tli, &fname, &content, &len);

			/*
			 * 检查主库上的文件名与自己计算的是否   
			 * 一致。这只是合理性检查，应当总是            
			 * 匹配。    
			 */
			TLHistoryFileName(expectedfname, tli);
			if (strcmp(fname, expectedfname) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg_internal("primary reported unexpected file name for timeline history file of timeline %u",
										 tli)));

			/*
			 * 文件写入 pg_wal。   
			 */
			writeTimeLineHistoryFile(tli, content, len);

			/*
			 * archive_mode 为 always 时把流式接收历史文件  
			 * 标记为待归档。  
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(fname);
			else
				XLogArchiveNotify(fname);

			pfree(fname);
			pfree(content);
		}
	}
}

/*
 * 退出时置共享内存状态为 STOPPED   
 */
static void
WalRcvDie(int code, Datum arg)
{
	WalRcvData *walrcv = WalRcv;
	TimeLineID *startpointTLI_p = (TimeLineID *) DatumGetPointer(arg);

	Assert(*startpointTLI_p != 0);

	/* 确保已收到的所有 WAL 记录都刷写到磁盘    */
	XLogWalRcvFlush(true, *startpointTLI_p);

	/* 共享内存中标记自身为非活动  */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_STREAMING ||
		   walrcv->walRcvState == WALRCV_RESTARTING ||
		   walrcv->walRcvState == WALRCV_STARTING ||
		   walrcv->walRcvState == WALRCV_WAITING ||
		   walrcv->walRcvState == WALRCV_STOPPING);
	Assert(walrcv->pid == MyProcPid);
	walrcv->walRcvState = WALRCV_STOPPED;
	walrcv->pid = 0;
	walrcv->ready_to_display = false;
	walrcv->latch = NULL;
	SpinLockRelease(&walrcv->mutex);

	ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/* 以优雅的方式终止连接。    */
	if (wrconn != NULL)
		walrcv_disconnect(wrconn);

	/* 唤醒启动进程，让其及时察觉我们已离开         */
	WakeupRecovery();
}

/*
 * 接受来自 XLOG 流的消息并进行处理。    
 */
static void
XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len, TimeLineID tli)
{
	int			hdrlen;
	XLogRecPtr	dataStart;
	XLogRecPtr	walEnd;
	TimestampTz sendTime;
	bool		replyRequested;

	resetStringInfo(&incoming_message);

	switch (type)
	{
		case 'w':				/* WAL 记录  */
			{
				/* 消息复制到 StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* 读取各域    */
				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				buf += hdrlen;
				len -= hdrlen;
				XLogWalRcvWrite(buf, len, dataStart, tli);
				break;
			}
		case 'k':				/* keepalive */
			{
				/* 消息复制到 StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(char);
				if (len != hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid keepalive message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* 读取各域    */
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				replyRequested = pq_getmsgbyte(&incoming_message);

				ProcessWalSndrMessage(walEnd, sendTime);

				/* 如果主库要求应答，则立即发送一份       */
				if (replyRequested)
					XLogWalRcvSendReply(true, false);
				break;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_internal("invalid replication message type %d",
									 type)));
	}
}

/*
 * XLOG 数据写入磁盘 
 */
static void
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr, TimeLineID tli)
{
	int			startoff;
	int			byteswritten;

	Assert(tli != 0);

	while (nbytes > 0)
	{
		int			segbytes;

		/* 如果当前段已完成，则将其关闭  */
		if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
			XLogWalRcvClose(recptr, tli);

		if (recvFile < 0)
		{
			/* 建/用新日志文件  */
			XLByteToSeg(recptr, recvSegNo, wal_segment_size);
			recvFile = XLogFileInit(recvSegNo, tli);
			recvFileTLI = tli;
		}

		/* 计算已接收日志的起始偏移量         */
		startoff = XLogSegmentOffset(recptr, wal_segment_size);

		if (startoff + nbytes > wal_segment_size)
			segbytes = wal_segment_size - startoff;
		else
			segbytes = nbytes;

		/* 可以写入日志了*/
		errno = 0;

		byteswritten = pg_pwrite(recvFile, buf, segbytes, (off_t) startoff);
		if (byteswritten <= 0)
		{
			char		xlogfname[MAXFNAMELEN];
			int			save_errno;

			/* 若 write 未设置 errno，假定空间不足  */
			if (errno == 0)
				errno = ENOSPC;

			save_errno = errno;
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			errno = save_errno;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to WAL segment %s "
							"at offset %u, length %lu: %m",
							xlogfname, startoff, (unsigned long) segbytes)));
		}

		/* 更新写入状态     */
		recptr += byteswritten;

		nbytes -= byteswritten;
		buf += byteswritten;

		LogstreamResult.Write = recptr;
	}

	/* 更新共享内存状态    */
	pg_atomic_write_u64(&WalRcv->writtenUpto, LogstreamResult.Write);

	/*
	 * 如果在循环的上一轮中当前段已完整写完，则将        
	 * 其关闭，以便尽快创建其归档通知文件。否则，      
	 * 该段的 WAL 归档将被推迟，直到下一段中的数        
	 * 据被接收并写出为止。  
	 */
	if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
		XLogWalRcvClose(recptr, tli);
}

/*
 * 刷写日志至磁盘 
 *
 * 如果我们正处于消亡过程中，做任何可能抛出               
 * 的事都不明智，所以此时跳过应答。  
 */
static void
XLogWalRcvFlush(bool dying, TimeLineID tli)
{
	Assert(tli != 0);

	if (LogstreamResult.Flush < LogstreamResult.Write)
	{
		WalRcvData *walrcv = WalRcv;

		issue_xlog_fsync(recvFile, recvSegNo, tli);

		LogstreamResult.Flush = LogstreamResult.Write;

		/* 更新共享内存状态    */
		SpinLockAcquire(&walrcv->mutex);
		if (walrcv->flushedUpto < LogstreamResult.Flush)
		{
			walrcv->latestChunkStart = walrcv->flushedUpto;
			walrcv->flushedUpto = LogstreamResult.Flush;
			walrcv->receivedTLI = tli;
		}
		SpinLockRelease(&walrcv->mutex);

		/* 通知启动进程与 walsender 新的 WAL 已到达              */
		WakeupRecovery();
		if (AllowCascadeReplication())
			WalSndWakeup(true, false);

		/* 在 PS 显示中报告 XLOG 流式进度     */
		if (update_process_title)
		{
			char		activitymsg[50];

			snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
					 LSN_FORMAT_ARGS(LogstreamResult.Write));
			set_ps_display(activitymsg);
		}

		/* 同时让主库知道我们取得了一些进展     */
		if (!dying)
		{
			XLogWalRcvSendReply(false, false);
			XLogWalRcvSendHSFeedback(false);
		}
	}
}

/*
 * 关闭当前段。        
 *
 * 在关闭段之前先将其刷写到磁盘。否则，稍后     
 * 须重开并 fsync。     
 *
 * 由于段已知完整，因此创建一个归档通知文件。          
 */
static void
XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli)
{
	char		xlogfname[MAXFNAMELEN];

	Assert(recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size));
	Assert(tli != 0);

	/*
	 * 在切换到下一个文件之前，先 fsync() 并关闭当前      
	 * 文件。否则稍后必须重新打开以 fsync。 
	 */
	XLogWalRcvFlush(false, tli);

	XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);

	/*
	 * XLOG 段文件很快就会被启动进程中的恢复再次            
	 * 读取，因此不像 XLogFileClose() 那样建议操作系统      
	 * 释放其关联的缓存页。 
	 */
	if (close(recvFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close WAL segment %s: %m",
						xlogfname)));

	/*
	 * 强制创建 .done 文件，以防流式接收的段稍后           
	 * 被归档。   
	 */
	if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
		XLogArchiveForceDone(xlogfname);
	else
		XLogArchiveNotify(xlogfname);

	recvFile = -1;
}

/*
 * 向主库发送应答消息，指明当前的 WAL 位置、                
 * 最老 xmin 与时间。  
 *
 * 如果未设置 'force'，则仅在距离上次状态更新       
 * 已过去足够时间、达到 wal_receiver_status_interval           
 * 时才发送该消息。若整个参数被禁用且 'force' 为      
 * false，则无操作   
 *
 * 如果 'requestReply' 为 true，则请求服务器在收到            
 * 此消息后立即应答。这用于心跳，即接近               
 * wal_receiver_timeout 
 */
static void
XLogWalRcvSendReply(bool force, bool requestReply)
{
	static XLogRecPtr writePtr = 0;
	static XLogRecPtr flushPtr = 0;
	XLogRecPtr	applyPtr;
	TimestampTz now;

	/*
	 * 如果用户不想让状态被报告给主库，务必在             
	 * 做任何事之前就退出。       
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* 获取当前时间戳  */
	now = GetCurrentTimestamp();

	/*
	 * 我们无需持有任何锁，就能将写出与刷写位置       
	 * 同我们发送的上一条消息进行比较，但应用位        
	 * 置需要自旋锁，因此除非有其他内容发生变           
	 * 化，或已经过去 10 秒，否则我们不检查它。这    
	 * 意味着从主库的角度看，应用的 WAL 位置会显得      
	 * 略微滞后，但仅用于报告且只在空闲系统，应无碍  
	 * 。         
	 */
	if (!force
		&& writePtr == LogstreamResult.Write
		&& flushPtr == LogstreamResult.Flush
		&& now < wakeup[WALRCV_WAKEUP_REPLY])
		return;

	/* 确保到了该发送下一条应答的时刻会醒来     */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_REPLY, now);

	/* 构造一条新消息   */
	writePtr = LogstreamResult.Write;
	flushPtr = LogstreamResult.Flush;
	applyPtr = GetXLogReplayRecPtr(NULL);

	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'r');
	pq_sendint64(&reply_message, writePtr);
	pq_sendint64(&reply_message, flushPtr);
	pq_sendint64(&reply_message, applyPtr);
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendbyte(&reply_message, requestReply ? 1 : 0);

	/* 发送  */
	elog(DEBUG2, "sending write %X/%X flush %X/%X apply %X/%X%s",
		 LSN_FORMAT_ARGS(writePtr),
		 LSN_FORMAT_ARGS(flushPtr),
		 LSN_FORMAT_ARGS(applyPtr),
		 requestReply ? " (reply requested)" : "");

	walrcv_send(wrconn, reply_message.data, reply_message.len);
}

/*
 * 向主库发送热备反馈消息，外加当前时间，           
 * 以防对端没有监控。     
 *
 * 如果用户禁用了反馈，则发送最后一条消息，告     
 * 诉发送端忘掉本备库上的 xmin。在首次连接时我     
 * 们也会发送此消息，因为之前的连接可能已在      
 * 某个复制槽上设置了 xmin。（如果我们没有用槽， 
 * 发送显式设置 InvalidTransactionId 的反馈是无害的） 
 */
static void
XLogWalRcvSendHSFeedback(bool immed)
{
	TimestampTz now;
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		xmin_epoch,
				catalog_xmin_epoch;
	TransactionId xmin,
				catalog_xmin;

	/* 初始为真，以确保我们总是至少发送反馈消息   */
	static bool primary_has_standby_xmin = true;

	/*
	 * 如果用户不想让状态被报告给主库，务必在             
	 * 做任何事之前就退出。       
	 */
	if ((wal_receiver_status_interval <= 0 || !hot_standby_feedback) &&
		!primary_has_standby_xmin)
		return;

	/* 获取当前时间戳  */
	now = GetCurrentTimestamp();

	/* 每个 wal_receiver_status_interval 至多发送一次反馈 */
	if (!immed && now < wakeup[WALRCV_WAKEUP_HSFEEDBACK])
		return;

	/* 确保到了该再次发送反馈的时刻会醒来         */
	WalRcvComputeNextWakeup(WALRCV_WAKEUP_HSFEEDBACK, now);

	/*
	 * 如果热备尚未接受连接，则没有可发送的内          
	 * 容。在间隔到期之后再检查此项，以减少调用次数 
	 * 。   
	 *
	 * 在此处退出还确保了在我们读取自身复制槽状         
	 * 态之前不会发送反馈，因此我们不会告诉主库        
	 * 丢弃本副本上可能存在的槽所需的 xmin 或              
	 * catalog_xmin 
	 */
	if (!HotStandbyActive())
		return;

	/*
	 * 等确定所有其他项都已检查之后，再执行获取      
	 * 最老 xmin 的昂贵调用。   
	 */
	if (hot_standby_feedback)
	{
		GetReplicationHorizons(&xmin, &catalog_xmin);
	}
	else
	{
		xmin = InvalidTransactionId;
		catalog_xmin = InvalidTransactionId;
	}

	/*
	 * 获取纪元，若 nextXid 与 oldestXmin 分居纪元边界         
	 * 两侧则调整。 
	 */
	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	xmin_epoch = EpochFromFullTransactionId(nextFullXid);
	catalog_xmin_epoch = xmin_epoch;
	if (nextXid < xmin)
		xmin_epoch--;
	if (nextXid < catalog_xmin)
		catalog_xmin_epoch--;

	elog(DEBUG2, "sending hot standby feedback xmin %u epoch %u catalog_xmin %u catalog_xmin_epoch %u",
		 xmin, xmin_epoch, catalog_xmin, catalog_xmin_epoch);

	/* 构造该消息并发送。        */
	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'h');
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendint32(&reply_message, xmin);
	pq_sendint32(&reply_message, xmin_epoch);
	pq_sendint32(&reply_message, catalog_xmin);
	pq_sendint32(&reply_message, catalog_xmin_epoch);
	walrcv_send(wrconn, reply_message.data, reply_message.len);
	if (TransactionIdIsValid(xmin) || TransactionIdIsValid(catalog_xmin))
		primary_has_standby_xmin = true;
	else
		primary_has_standby_xmin = false;
}

/*
 * 从主库收到消息时更新共享内存状态。               
 *
 * 'walEnd' 与 'sendTime' 是最新消息的 WAL 末尾位置和        
 * 时间戳，主库报告。  
 */
static void
ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz lastMsgReceiptTime = GetCurrentTimestamp();

	/* 更新共享内存状态    */
	SpinLockAcquire(&walrcv->mutex);
	if (walrcv->latestWalEnd < walEnd)
		walrcv->latestWalEndTime = sendTime;
	walrcv->latestWalEnd = walEnd;
	walrcv->lastMsgSendTime = sendTime;
	walrcv->lastMsgReceiptTime = lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *sendtime;
		char	   *receipttime;
		int			applyDelay;

		/* 复制，因 timestamptz_to_str 返回静态缓冲区   */
		sendtime = pstrdup(timestamptz_to_str(sendTime));
		receipttime = pstrdup(timestamptz_to_str(lastMsgReceiptTime));
		applyDelay = GetReplicationApplyDelay();

		/* 应用延迟不可用        */
		if (applyDelay == -1)
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay (N/A) transfer latency %d ms",
				 sendtime,
				 receipttime,
				 GetReplicationTransferLatency());
		else
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay %d ms transfer latency %d ms",
				 sendtime,
				 receipttime,
				 applyDelay,
				 GetReplicationTransferLatency());

		pfree(sendtime);
		pfree(receipttime);
	}
}

/*
 * 针对给定的唤醒原因计算下一次唤醒时刻。可             
 * 以被调用来初始化某个唤醒时刻、为下一次唤     
 * 醒调整它，或者在 GUC 发生变化时重新初始化              
 * 它。我们要求调用者传入 "now" 值，这经常        
 * 能避免多次调用 GetCurrentTimestamp()。该值最好足够      
 * 新。 
 */
static void
WalRcvComputeNextWakeup(WalRcvWakeupReason reason, TimestampTz now)
{
	switch (reason)
	{
		case WALRCV_WAKEUP_TERMINATE:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout);
			break;
		case WALRCV_WAKEUP_PING:
			if (wal_receiver_timeout <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusMilliseconds(now, wal_receiver_timeout / 2);
			break;
		case WALRCV_WAKEUP_HSFEEDBACK:
			if (!hot_standby_feedback || wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
		case WALRCV_WAKEUP_REPLY:
			if (wal_receiver_status_interval <= 0)
				wakeup[reason] = TIMESTAMP_INFINITY;
			else
				wakeup[reason] = TimestampTzPlusSeconds(now, wal_receiver_status_interval);
			break;
			/* 此处有意不写 default: 分支     */
	}
}

/*
 * 唤醒 walreceiver 主循环。   
 *
 * 每当启动进程应用了值得关注的 xlog 记录时，           
 * 都会调用本函数，使 walreceiver 能检查是否需要          
 * 向主库发回应用通知；主库可能正在 COMMIT 中等待     
 * synchronous_commit = remote_apply.
 */
void
WalRcvForceReply(void)
{
	Latch	   *latch;

	WalRcv->force_reply = true;
	/* 锁存器指针的读取可能非原子，须用自旋锁       */
	SpinLockAcquire(&WalRcv->mutex);
	latch = WalRcv->latch;
	SpinLockRelease(&WalRcv->mutex);
	if (latch)
		SetLatch(latch);
}

/*
 * 返回表示状态的字符串常量。它用于系统函数 
 * 和视图，*不应*被翻译。                              
 */
static const char *
WalRcvGetStateString(WalRcvState state)
{
	switch (state)
	{
		case WALRCV_STOPPED:
			return "stopped";
		case WALRCV_STARTING:
			return "starting";
		case WALRCV_STREAMING:
			return "streaming";
		case WALRCV_WAITING:
			return "waiting";
		case WALRCV_RESTARTING:
			return "restarting";
		case WALRCV_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

/*
 * 返回 WAL 接收进程的活动，包括 pid、状态，以及          
 * 来自对端 WAL 发送进程的 xlog 位置   
 */
Datum
pg_stat_get_wal_receiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	int			pid;
	bool		ready_to_display;
	WalRcvState state;
	XLogRecPtr	receive_start_lsn;
	TimeLineID	receive_start_tli;
	XLogRecPtr	written_lsn;
	XLogRecPtr	flushed_lsn;
	TimeLineID	received_tli;
	TimestampTz last_send_time;
	TimestampTz last_receipt_time;
	XLogRecPtr	latest_end_lsn;
	TimestampTz latest_end_time;
	char		sender_host[NI_MAXHOST];
	int			sender_port = 0;
	char		slotname[NAMEDATALEN];
	char		conninfo[MAXCONNINFO];

	/* 持锁以确保值的一致性          */
	SpinLockAcquire(&WalRcv->mutex);
	pid = (int) WalRcv->pid;
	ready_to_display = WalRcv->ready_to_display;
	state = WalRcv->walRcvState;
	receive_start_lsn = WalRcv->receiveStart;
	receive_start_tli = WalRcv->receiveStartTLI;
	flushed_lsn = WalRcv->flushedUpto;
	received_tli = WalRcv->receivedTLI;
	last_send_time = WalRcv->lastMsgSendTime;
	last_receipt_time = WalRcv->lastMsgReceiptTime;
	latest_end_lsn = WalRcv->latestWalEnd;
	latest_end_time = WalRcv->latestWalEndTime;
	strlcpy(slotname, (char *) WalRcv->slotname, sizeof(slotname));
	strlcpy(sender_host, (char *) WalRcv->sender_host, sizeof(sender_host));
	sender_port = WalRcv->sender_port;
	strlcpy(conninfo, (char *) WalRcv->conninfo, sizeof(conninfo));
	SpinLockRelease(&WalRcv->mutex);

	/*
	 * 没有 WAL 接收进程（或尚未就绪），返回 NULL       
	 * 值   
	 */
	if (pid == 0 || !ready_to_display)
		PG_RETURN_NULL();

	/*
	 * 无需持有自旋锁即可读取 "writtenUpto"。注意               
	 * 它与 WAL 接收进程的其他由自旋锁保护的共享   
	 * 变量可能不一致，但不应将其用于数据完整性检        
	 * 查。 
	 */
	written_lsn = pg_atomic_read_u64(&WalRcv->writtenUpto);

	/* 确定结果类型    */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values = palloc0(sizeof(Datum) * tupdesc->natts);
	nulls = palloc0(sizeof(bool) * tupdesc->natts);

	/* 获取值    */
	values[0] = Int32GetDatum(pid);

	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		/*
		 * 仅超级用户和拥有 pg_read_all_stats 权限的角色        
		 * 才能看到详情。其他用户仅得 pid 值，得知其是    
		 * WAL 接收进程，无详情。   
		 */
		memset(&nulls[1], true, sizeof(bool) * (tupdesc->natts - 1));
	}
	else
	{
		values[1] = CStringGetTextDatum(WalRcvGetStateString(state));

		if (XLogRecPtrIsInvalid(receive_start_lsn))
			nulls[2] = true;
		else
			values[2] = LSNGetDatum(receive_start_lsn);
		values[3] = Int32GetDatum(receive_start_tli);
		if (XLogRecPtrIsInvalid(written_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(written_lsn);
		if (XLogRecPtrIsInvalid(flushed_lsn))
			nulls[5] = true;
		else
			values[5] = LSNGetDatum(flushed_lsn);
		values[6] = Int32GetDatum(received_tli);
		if (last_send_time == 0)
			nulls[7] = true;
		else
			values[7] = TimestampTzGetDatum(last_send_time);
		if (last_receipt_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(last_receipt_time);
		if (XLogRecPtrIsInvalid(latest_end_lsn))
			nulls[9] = true;
		else
			values[9] = LSNGetDatum(latest_end_lsn);
		if (latest_end_time == 0)
			nulls[10] = true;
		else
			values[10] = TimestampTzGetDatum(latest_end_time);
		if (*slotname == '\0')
			nulls[11] = true;
		else
			values[11] = CStringGetTextDatum(slotname);
		if (*sender_host == '\0')
			nulls[12] = true;
		else
			values[12] = CStringGetTextDatum(sender_host);
		if (sender_port == 0)
			nulls[13] = true;
		else
			values[13] = Int32GetDatum(sender_port);
		if (*conninfo == '\0')
			nulls[14] = true;
		else
			values[14] = CStringGetTextDatum(conninfo);
	}

	/* 返回 Datum 形式的记录*/
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
