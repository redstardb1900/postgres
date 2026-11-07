/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * WAL 发送进程（walsender）是 Postgres 9.0 新增的，          
 * 负责把 XLOG 从主库发送给单个接收方。                
 * （注意可以同时存在多个 walsender 进程。）              
 * 当备库的 walreceiver 连接到主库并请求 XLOG 流复制时，  
 * postmaster 便会启动它。                                          
 *
 * walsender 与普通后端类似：连接与 walsender 进程之间    
 * 是一对一的关系，但它处理的不是 SQL 查询，           
 * 而是理解一小组特殊的复制模式命令。             
 * START_REPLICATION 命令开始向客户端流式发送 WAL。            
 * 流式发送期间，walsender 持续从磁盘读 XLOG 记录，    
 * 并通过 COPY 协议发送给备库，直到某一方             
 * 通过退出 COPY 模式而结束复制                              
 * （或直到连接关闭）。           
 *
 * 正常终止由 SIGTERM 触发，指示 walsender 在下一个    
 * 合适的时机关闭连接并 exit(0)。紧急终止使用 SIGQUIT；   
 * 与其他后端一样，walsender 收到 SIGQUIT 即直接中止并   
 * 退出。连接关闭和 FATAL 错误不算崩溃，而近似         
 * 于正常终止；walsender 快速退出，                     
 * 不再发送任何 XLOG 记录。                                     
 *
 * 若服务器关闭，checkpointer 会在         
 * 所有普通后端退出后发来 PROCSIG_WALSND_INIT_STOPPING。       
 * 若后端空闲或执行 SQL 查询，后端会随之关闭；     
 * 若逻辑复制进行中，则先处理完所有现有 WAL 记录，    
 * 然后再关闭。否则，walsender 会切换到"stopping"状态。      
 * 在此状态下，walsender 会拒绝任何后续的复制命令。        
 * checkpointer 在确认所有 walsender 都进入停止状态后，    
 * 就会开始关闭检查点。当关闭检查点完成后，               
 * postmaster 会给我们发送 SIGUSR2，指示 walsender 发送     
 * 所有未发出的 WAL（含关闭检查点记录），等待复制     
 * 到备库，然后退出。                                        
 *
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/printtup.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/pgstat_internal.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* walsender 用于统计刷写的最小间隔（毫秒）     */
#define WALSENDER_STATS_FLUSH_INTERVAL         1000

/*
 * WAL 数据消息的最大数据载荷。必须 >= XLOG_BLCKSZ。    
 *
 * 对于什么才是合适的取值，我们没有很好的概念；   
 * 每条消息在 walsender 与 walreceiver 中都有一定开销，      
 * 但另一方面，大批量发送会降低 walsender 对信号响应 
 * 能力，因为只在消息之间才检查信号。            
 * 128kB（默认 8k 块）目前算是合理的猜测。   
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

/* 共享内存中的 WalSnds 数组 */
WalSndCtlData *WalSndCtl = NULL;

/* 共享内存数组中我的槽位  */
WalSnd	   *MyWalSnd = NULL;

/* 全局状态 */
bool		am_walsender = false;	/* 是 walsender 进程吗？*/
bool		am_cascading_walsender = false; /* Am I cascading WAL to another
											 * standby? */
bool		am_db_walsender = false;	/* 已连接到数据库？ */

/* GUC 变量    */
int			max_wal_senders = 10;	/* the maximum number of concurrent
									 * walsenders */
int			wal_sender_timeout = 60 * 1000; /* maximum time to send one WAL
											 * data message */
bool		log_replication_commands = false;

/*
 * WalSndWakeupRequest 状态   
 */
bool		wake_wal_senders = false;

/*
 * 复制用的 xlogreader。注意，物理复制的 WAL 发送进程    
 * 并不需要 xlogreader 来读 WAL，但需要一个来              
 * 记录其工作状态。 
 */
static XLogReaderState *xlogreader = NULL;

/*
 * 这些变量跟踪我们当前正在发送的时间线的状态。     
 * sendTimeLine 标识该时间线。若 sendTimeLineIsHistoric，          
 * 则它不是本服务器上最新的时间线，且本服务器         
 * 的历史自 sendTimeLineValidUpto 起与它分岔。          
 */
static TimeLineID sendTimeLine = 0;
static TimeLineID sendTimeLineNextTLI = 0;
static bool sendTimeLineIsHistoric = false;
static XLogRecPtr sendTimeLineValidUpto = InvalidXLogRecPtr;

/*
 * 我们已经把 WAL 发送到哪里了？该值也通过    
 * MyWalSnd->sentPtr 对外公布。（实为下一条待发位置）    
 */
static XLogRecPtr sentPtr = InvalidXLogRecPtr;

/* 用于构造外发消息和处理应答消息的缓冲区。              */
static StringInfoData output_message;
static StringInfoData reply_message;
static StringInfoData tmpbuf;

/* 上次 ProcessRepliesIfAny() 时间戳。*/
static TimestampTz last_processing = 0;

/*
 * 上次看到备库应答的 ProcessRepliesIfAny() 时间戳。   
 * 若 wal_sender_timeout 无需处于活动状态，则设为 0。  
 */
static TimestampTz last_reply_timestamp = 0;

/* 自上次应答以来，是否发送过请求应答的心跳？      */
static bool waiting_for_ping_response = false;

/*
 * 在 Copy 模式下流式发送 WAL 时，发送 CopyDone 之后      
 * streamingDoneSending 就被置为 true。此后不应再发送            
 * 任何 CopyData 消息。streamingDoneReceiving 则在收到              
 * 另一端 CopyDone 时置为 true。两者皆真即退出 Copy         
 */
static bool streamingDoneSending;
static bool streamingDoneReceiving;

/* 我们到了吗？*/
static bool WalSndCaughtUp = false;

/* 信号处理程序设置、供主循环稍后处理的标志*/
static volatile sig_atomic_t got_SIGUSR2 = false;
static volatile sig_atomic_t got_STOPPING = false;

/*
 * 流式发送期间置位。未置位时，      
 * PROCSIG_WALSND_INIT_STOPPING 信号按 SIGTERM 处理。置位后，       
 * 主循环负责检查 got_STOPPING，并在它置位时结束运行（     
 * 发送完剩余 WAL 之后）。             
 */
static volatile sig_atomic_t replication_active = false;

static LogicalDecodingContext *logical_decoding_ctx = NULL;

/* 将 WAL 位置与其写入时间关联起来的一个样本。    */
typedef struct
{
	XLogRecPtr	lsn;
	TimestampTz time;
} WalTimeSample;

/* 时间样本缓冲区的大小。       */
#define LAG_TRACKER_BUFFER_SIZE 8192

/* 一种用于跟踪复制延迟的机制。*/
typedef struct
{
	XLogRecPtr	last_lsn;
	WalTimeSample buffer[LAG_TRACKER_BUFFER_SIZE];
	int			write_head;
	int			read_heads[NUM_SYNC_REP_WAIT_MODE];
	WalTimeSample last_read[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * 供与写头发生碰撞的读头使用的溢出条目。        
	 *
	 * 循环缓冲区填满（写头将撞上某读头）时，我们    
	 * 把该读头的当前样本保存在这里，并把它标记为        
	 * 使用溢出（read_heads[i] = -1）。写头可继续推进，   
	 * 而溢出模式则继续计算延迟，                               
	 * 用保存的样本。  
	 *
	 * 一旦备库报告的 LSN 越过溢出条目的 LSN，我们就        
	 * 就转回正常的基于缓冲区的跟踪。      
	 */
	WalTimeSample overflowed[NUM_SYNC_REP_WAIT_MODE];
} LagTracker;

static LagTracker *lag_tracker;

/* 信号处理    */
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* 私有函数的原型            */
typedef void (*WalSndSendDataCallback) (void);
static void WalSndLoop(WalSndSendDataCallback send_data);
static void InitWalSenderSlot(void);
static void WalSndKill(int code, Datum arg);
static void WalSndShutdown(void) pg_attribute_noreturn();
static void XLogSendPhysical(void);
static void XLogSendLogical(void);
static void WalSndDone(WalSndSendDataCallback send_data);
static XLogRecPtr GetStandbyFlushRecPtr(TimeLineID *tli);
static void IdentifySystem(void);
static void ReadReplicationSlot(ReadReplicationSlotCmd *cmd);
static void CreateReplicationSlot(CreateReplicationSlotCmd *cmd);
static void DropReplicationSlot(DropReplicationSlotCmd *cmd);
static void StartReplication(StartReplicationCmd *cmd);
static void StartLogicalReplication(StartReplicationCmd *cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessRepliesIfAny(void);
static void ProcessPendingWrites(void);
static void WalSndKeepalive(bool requestReply, XLogRecPtr writePtr);
static void WalSndKeepaliveIfNecessary(void);
static void WalSndCheckTimeOut(void);
static long WalSndComputeSleeptime(TimestampTz now);
static void WalSndWait(uint32 socket_events, long timeout, uint32 wait_event);
static void WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
								 bool skipped_xact);
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc);
static void LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time);
static TimeOffset LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now);
static bool TransactionIdInRecentPast(TransactionId xid, uint32 epoch);

static void WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
							  TimeLineID *tli_p);


/* 在进入主命令循环之前初始化 walsender 进程           */
void
InitWalSender(void)
{
	am_cascading_walsender = RecoveryInProgress();

	/* 在共享内存中为每个 walsender 建数据结构  */
	InitWalSenderSlot();

	/*
	 * 目前 walsender 进程并不需要任何 ResourceOwner，但如果  
	 * 如需要，可调用 CreateAuxProcessResourceOwner。      
	 */

	/*
	 * 让 postmaster 知道自己是 WAL 发送进程。一经声明，     
	 * WAL 发送进程后，postmaster 会让我们活得比               
	 * bgwriter 更久，并在关闭序列中最后终止我们，让我们  
	 * 有机会在关闭时把所有剩余 WAL 流式发完，包括关闭   
	 * 检查点。注意，此后没有回头，无法再写 WAL 记录。   
	 */
	MarkPostmasterChildWalSender();
	SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);

	/*
	 * 若客户端未指定要连接的数据库，则在 PGPROC 中表明 
	 * 我们的 xmin 应影响所有库中清理视界。          
	 * 这样物理复制客户端就能发送热备反馈，          
	 * 从而延迟所有数据库中的清理回收工作。           
	 */
	if (MyDatabaseId == InvalidOid)
	{
		Assert(MyProc->xmin == InvalidTransactionId);
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_AFFECTS_ALL_HORIZONS;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/* 为延迟跟踪初始化空的时间戳缓冲区。 */
	lag_tracker = MemoryContextAllocZero(TopMemoryContext, sizeof(LagTracker));
}

/*
 * 错误后的清理。   
 *
 * WAL 发送进程不像普通后端那样使用事务。              
 * 本函数完成 WAL 发送进程中出错后所需的清理工作，  
 * 类似于普通后端中事务中止所做的操作。               
 */
void
WalSndErrorCleanup(void)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();

	if (xlogreader != NULL && xlogreader->seg.ws_file >= 0)
		wal_segment_close(xlogreader);

	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	ReplicationSlotCleanup();

	replication_active = false;

	/*
	 * 如果有事务正在进行，它会清理我们的        
	 * ResourceOwner，但如果某个复制命令设置资源所有者   
	 * 而没有事务，那么现在必须把它清理。   
	 */
	if (!IsTransactionOrTransactionBlock())
		WalSndResourceCleanup(false);

	if (got_STOPPING || got_SIGUSR2)
		proc_exit(0);

	/* 退回启动状态           */
	WalSndSetState(WALSNDSTATE_STARTUP);
}

/*
 * 清理所创建的 ResourceOwner。   
 */
void
WalSndResourceCleanup(bool isCommit)
{
	ResourceOwner resowner;

	if (CurrentResourceOwner == NULL)
		return;

	/*
	 * 删除 CurrentResourceOwner 是不允许的，因此我们必须先把 
	 * 指针存入局部变量并清除。   
	 */
	resowner = CurrentResourceOwner;
	CurrentResourceOwner = NULL;

	/* 现在即可释放资源并删除它。     */
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_BEFORE_LOCKS, isCommit, true);
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_LOCKS, isCommit, true);
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_AFTER_LOCKS, isCommit, true);
	ResourceOwnerDelete(resowner);
}

/*
 * 以有序的方式处理客户端连接中止。        
 */
static void
WalSndShutdown(void)
{
	/*
	 * 重置 whereToSendOutput，防止 ereport 试图向备库再发送    
	 * 任何消息。              
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	abort();					/* 让编译器保持安静*/
}

/*
 * 处理 IDENTIFY_SYSTEM 命令。   
 */
static void
IdentifySystem(void)
{
	char		sysid[32];
	char		xloc[MAXFNAMELEN];
	XLogRecPtr	logptr;
	char	   *dbname = NULL;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};
	TimeLineID	currTLI;

	/*
	 * 以单行四列的结果集应答。第一列是系统标识符，     
	 * 第二列是时间线 ID，第三列是 xlog 位置，第四列  
	 * 在已连接到数据库时包含数据库名。             
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());

	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		logptr = GetStandbyFlushRecPtr(&currTLI);
	else
		logptr = GetFlushRecPtr(&currTLI);

	snprintf(xloc, sizeof(xloc), "%X/%X", LSN_FORMAT_ARGS(logptr));

	if (MyDatabaseId != InvalidOid)
	{
		MemoryContext cur = CurrentMemoryContext;

		/* syscache 访问需要一个事务环境  */
		StartTransactionCommand();
		/* dbname 存活到事务上下文外  */
		MemoryContextSwitchTo(cur);
		dbname = get_database_name(MyDatabaseId);
		CommitTransactionCommand();
		/* CommitTransactionCommand 切换到 TopMemoryContext   */
		MemoryContextSwitchTo(cur);
	}

	dest = CreateDestReceiver(DestRemoteSimple);

	/* 需要一个表示四列的元组描述符        */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "systemid",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "timeline",
							  INT8OID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "xlogpos",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "dbname",
							  TEXTOID, -1, 0);

	/* 准备元组的投影            */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* 列 1：系统标识符     */
	values[0] = CStringGetTextDatum(sysid);

	/* 列 2：时间线  */
	values[1] = Int64GetDatum(currTLI);

	/* 列 3：WAL 位置     */
	values[2] = CStringGetTextDatum(xloc);

	/* 列 4：数据库名，若无则为 NULL */
	if (dbname)
		values[3] = CStringGetTextDatum(dbname);
	else
		nulls[3] = true;

	/* 发送到目标 */
	do_tup_output(tstate, values, nulls);

	end_tup_output(tstate);
}

/* 处理 READ_REPLICATION_SLOT 命令  */
static void
ReadReplicationSlot(ReadReplicationSlotCmd *cmd)
{
#define READ_REPLICATION_SLOT_COLS 3
	ReplicationSlot *slot;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[READ_REPLICATION_SLOT_COLS] = {0};
	bool		nulls[READ_REPLICATION_SLOT_COLS];

	tupdesc = CreateTemplateTupleDesc(READ_REPLICATION_SLOT_COLS);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_type",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "restart_lsn",
							  TEXTOID, -1, 0);
	/* TimeLineID 无符号，int4 宽度不够。         */
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "restart_tli",
							  INT8OID, -1, 0);

	memset(nulls, true, READ_REPLICATION_SLOT_COLS * sizeof(bool));

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(cmd->slotname, false);
	if (slot == NULL || !slot->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);
	}
	else
	{
		ReplicationSlot slot_contents;
		int			i = 0;

		/* 持有 spinlock 期间复制槽内容     */
		SpinLockAcquire(&slot->mutex);
		slot_contents = *slot;
		SpinLockRelease(&slot->mutex);
		LWLockRelease(ReplicationSlotControlLock);

		if (OidIsValid(slot_contents.data.database))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use %s with a logical replication slot",
						   "READ_REPLICATION_SLOT"));

		/* 槽类型 */
		values[i] = CStringGetTextDatum("physical");
		nulls[i] = false;
		i++;

		/* 起始 LSN*/
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			char		xloc[64];

			snprintf(xloc, sizeof(xloc), "%X/%X",
					 LSN_FORMAT_ARGS(slot_contents.data.restart_lsn));
			values[i] = CStringGetTextDatum(xloc);
			nulls[i] = false;
		}
		i++;

		/* 产生该 WAL 的时间线        */
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			TimeLineID	slots_position_timeline;
			TimeLineID	current_timeline;
			List	   *timeline_history = NIL;

			/*
			 * 在恢复期间，使用当前正在重放的时间线，     
			 * 以获取 LSN 位置的历史。  
			 */
			if (RecoveryInProgress())
				(void) GetXLogReplayRecPtr(&current_timeline);
			else
				current_timeline = GetWALInsertionTimeLine();

			timeline_history = readTimeLineHistory(current_timeline);
			slots_position_timeline = tliOfPointInHistory(slot_contents.data.restart_lsn,
														  timeline_history);
			values[i] = Int64GetDatum((int64) slots_position_timeline);
			nulls[i] = false;
		}
		i++;

		Assert(i == READ_REPLICATION_SLOT_COLS);
	}

	dest = CreateDestReceiver(DestRemoteSimple);
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);
}


/*
 * 处理 TIMELINE_HISTORY 命令  
 */
static void
SendTimeLineHistory(TimeLineHistoryCmd *cmd)
{
	DestReceiver *dest;
	TupleDesc	tupdesc;
	StringInfoData buf;
	char		histfname[MAXFNAMELEN];
	char		path[MAXPGPATH];
	int			fd;
	off_t		histfilelen;
	off_t		bytesleft;
	Size		len;

	dest = CreateDestReceiver(DestRemoteSimple);

	/*
	 * 以单行两列的结果集应答。第一列是历史文件名，     
	 * 第二列是其内容。                          
	 */
	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "filename", TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "content", TEXTOID, -1, 0);

	TLHistoryFileName(histfname, cmd->timeline);
	TLHistoryFilePath(path, cmd->timeline);

	/* 发送 RowDescription 消息  */
	dest->rStartup(dest, CMD_SELECT, tupdesc);

	/* 发送 DataRow 消息  */
	pq_beginmessage(&buf, 'D');
	pq_sendint16(&buf, 2);		/* 列数       */
	len = strlen(histfname);
	pq_sendint32(&buf, len);	/* 列 1 长*/
	pq_sendbytes(&buf, histfname, len);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* 确定文件长度并发送给客户端     */
	histfilelen = lseek(fd, 0, SEEK_END);
	if (histfilelen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m", path)));
	if (lseek(fd, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to beginning of file \"%s\": %m", path)));

	pq_sendint32(&buf, histfilelen);	/* 列 2 长*/

	bytesleft = histfilelen;
	while (bytesleft > 0)
	{
		PGAlignedBlock rbuf;
		int			nread;

		pgstat_report_wait_start(WAIT_EVENT_WALSENDER_TIMELINE_HISTORY_READ);
		nread = read(fd, rbuf.data, sizeof(rbuf));
		pgstat_report_wait_end();
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (nread == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, nread, (Size) bytesleft)));

		pq_sendbytes(&buf, rbuf.data, nread);
		bytesleft -= nread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	pq_endmessage(&buf);
}

/*
 * 处理 START_REPLICATION 命令  
 *
 * 目前本函数从不返回，但 ereport(ERROR) 会把我们带回       
 * 主循环。     
 */
static void
StartReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;
	XLogRecPtr	FlushPtr;
	TimeLineID	FlushTLI;

	/* 为物理复制创建 xlogreader           */
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.segment_open = WalSndSegmentOpen,
									  .segment_close = wal_segment_close),
						   NULL);

	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/*
	 * 这里我们假定 WAL 中为日志传送记录了足够信息，  
	 * 因为这一点已在 PostmasterMain() 中检查过。  
	 *
	 * 注意：wal_level 只能在关闭时更改，故多数情况下   
	 * 很难还有以 wal_level='minimal'                              
	 * 写入且仍可见数据。    
	 */

	if (cmd->slotname)
	{
		ReplicationSlotAcquire(cmd->slotname, true);
		if (SlotIsLogical(MyReplicationSlot))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a logical replication slot for physical replication")));

		/*
		 * 这里无需验证槽的 restart_lsn；相反，我们依赖   
		 * 调用者来请求使用的起始点。如果该                
		 * WAL 段不存在，稍后会失败。       
		 */
	}

	/*
	 * 选择时间线。若客户端显式给出，就用该值；      
	 * 否则使用最后重放的记录的时间线。             
	 */
	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		FlushPtr = GetStandbyFlushRecPtr(&FlushTLI);
	else
		FlushPtr = GetFlushRecPtr(&FlushTLI);

	if (cmd->timeline != 0)
	{
		XLogRecPtr	switchpoint;

		sendTimeLine = cmd->timeline;
		if (sendTimeLine == FlushTLI)
		{
			sendTimeLineIsHistoric = false;
			sendTimeLineValidUpto = InvalidXLogRecPtr;
		}
		else
		{
			List	   *timeLineHistory;

			sendTimeLineIsHistoric = true;

			/*
			 * 检查客户端所求时间线是否存在，以及请求   
			 * 起始位置是否在该时间线上。      
			 */
			timeLineHistory = readTimeLineHistory(FlushTLI);
			switchpoint = tliSwitchPoint(cmd->timeline, timeLineHistory,
										 &sendTimeLineNextTLI);
			list_free_deep(timeLineHistory);

			/*
			 * 在历史中找到请求的时间线。检查请求的 
			 * 起始点在我们历史中是否属于该时间线。  
			 *
			 * 这是有意放宽的。我们只检查自己没有         
			 * 在切换点之前从请求的时间线分岔。我们    
			 * 不检查是否在请求的起始点之前切换*到*它 
			 * 起始点。这是因为客户端完全可以合理地     
			 * 请求从含切换点的 WAL 段开头开始复制，     
			 * 但使用新时间线，从而避免拿到不完整的段。  
			 * 如果请求的起始点太旧，那么稍后当我们在    
			 * pg_wal 中找不到请求的 WAL 段时，                 
			 * 就会报错。                                  
			 *
			 * XXX：这里可以更严格一些，只允许起始点       
			 * 比切换点更早，前提是它仍位于同一个         
			 * WAL 段。  
			 */
			if (!XLogRecPtrIsInvalid(switchpoint) &&
				switchpoint < cmd->startpoint)
			{
				ereport(ERROR,
						(errmsg("requested starting point %X/%X on timeline %u is not in this server's history",
								LSN_FORMAT_ARGS(cmd->startpoint),
								cmd->timeline),
						 errdetail("This server's history forked from timeline %u at %X/%X.",
								   cmd->timeline,
								   LSN_FORMAT_ARGS(switchpoint))));
			}
			sendTimeLineValidUpto = switchpoint;
		}
	}
	else
	{
		sendTimeLine = FlushTLI;
		sendTimeLineValidUpto = InvalidXLogRecPtr;
		sendTimeLineIsHistoric = false;
	}

	streamingDoneSending = streamingDoneReceiving = false;

	/* 若无内容可流式发送，干脆不进入 COPY 模式 */
	if (!sendTimeLineIsHistoric || cmd->startpoint < sendTimeLineValidUpto)
	{
		/*
		 * 刚启动复制时，备库落后主库。对某些应用，  
		 * 例如同步复制，让这种初始追赶模式       
		 * 有一个清晰的状态，这样我们以后改变流式发送    
		 * 状态时就能触发相应动作。我们可能在这种状态 
		 * 中停留很长时间，这正是我们希望能够监控       
		 * 自己是否仍处于这一状态，                           
		 * 还在吗  
		 */
		WalSndSetState(WALSNDSTATE_CATCHUP);

		/* 发送 CopyBothResponse 消息并开始流式发送  */
		pq_beginmessage(&buf, 'W');
		pq_sendbyte(&buf, 0);
		pq_sendint16(&buf, 0);
		pq_endmessage(&buf);
		pq_flush();

		/*
		 * 不允许从本服务器尚未刷写到磁盘的 WAL 未来    
		 * 位置进行流式发送。                    
		 */
		if (FlushPtr < cmd->startpoint)
		{
			ereport(ERROR,
					(errmsg("requested starting point %X/%X is ahead of the WAL flush position of this server %X/%X",
							LSN_FORMAT_ARGS(cmd->startpoint),
							LSN_FORMAT_ARGS(FlushPtr))));
		}

		/* 从请求的位置开始流式发送     */
		sentPtr = cmd->startpoint;

		/* 同时初始化共享内存状态    */
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sentPtr = sentPtr;
		SpinLockRelease(&MyWalSnd->mutex);

		SyncRepInitConfig();

		/* walsender 的主循环 */
		replication_active = true;

		WalSndLoop(XLogSendPhysical);

		replication_active = false;
		if (got_STOPPING)
			proc_exit(0);
		WalSndSetState(WALSNDSTATE_STARTUP);

		Assert(streamingDoneSending && streamingDoneReceiving);
	}

	if (cmd->slotname)
		ReplicationSlotRelease();

	/*
	 * Copy 现已结束。发送单行结果集，指明下一条时间     
	 * 线。   
	 */
	if (sendTimeLineIsHistoric)
	{
		char		startpos_str[8 + 1 + 8 + 1];
		DestReceiver *dest;
		TupOutputState *tstate;
		TupleDesc	tupdesc;
		Datum		values[2];
		bool		nulls[2] = {0};

		snprintf(startpos_str, sizeof(startpos_str), "%X/%X",
				 LSN_FORMAT_ARGS(sendTimeLineValidUpto));

		dest = CreateDestReceiver(DestRemoteSimple);

		/*
		 * 需要一个表示两列的元组描述符。用 int8 似乎   
		 * 让人意外，但理论上 int4 的宽度不足以表达它，   
		 * 因为 TimeLineID 是无符号的。               
		 */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "next_tli",
								  INT8OID, -1, 0);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "next_tli_startpos",
								  TEXTOID, -1, 0);

		/* 准备元组的投影           */
		tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

		values[0] = Int64GetDatum((int64) sendTimeLineNextTLI);
		values[1] = CStringGetTextDatum(startpos_str);

		/* 发送到目标 */
		do_tup_output(tstate, values, nulls);

		end_tup_output(tstate);
	}

	/* 发送 CommandComplete 消息*/
	EndReplicationCommand("START_STREAMING");
}

/*
 * 作为 walsender 进程时的 XLogReaderRoutine->page_read               
 * 回调。         
 *
 * 在 walsender 内部我们可以做得比 read_local_xlog_page  
 * 更好，后者只能用简单的睡眠/忙循环，因为每次           
 * 刷 WAL 即置位锁存器。 
 */
static int
logical_read_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
					   XLogRecPtr targetRecPtr, char *cur_page)
{
	XLogRecPtr	flushptr;
	int			count;
	WALReadError errinfo;
	XLogSegNo	segno;
	TimeLineID	currTLI;

	/*
	 * 在获取当前时间线之前，先确保有足够 WAL 可用。   
	 * 这需要用来准确判定 am_cascading_walsender，                  
	 * 而判定当前时间线又需要它。           
	 */
	flushptr = WalSndWaitForWal(targetPagePtr + reqLen);

	/*
	 * 由于备库上也允许逻辑解码，我们需要检查服务器   
	 * 是否处于恢复中，以决定如何获取当前时间线标识符 
	 * （这样也才能涵盖提升或时间线变更                   
	 * ）。 
	 */
	am_cascading_walsender = RecoveryInProgress();

	if (am_cascading_walsender)
		GetXLogReplayRecPtr(&currTLI);
	else
		currTLI = GetWALInsertionTimeLine();

	XLogReadDetermineTimeline(state, targetPagePtr, reqLen, currTLI);
	sendTimeLineIsHistoric = (state->currTLI != currTLI);
	sendTimeLine = state->currTLI;
	sendTimeLineValidUpto = state->currTLIValidUntil;
	sendTimeLineNextTLI = state->nextTLI;

	/* 不满足则失败（意味着即将关闭）   */
	if (flushptr < targetPagePtr + reqLen)
		return -1;

	if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
		count = XLOG_BLCKSZ;	/* 可用数据超过一个块   */
	else
		count = flushptr - targetPagePtr;	/* 仅页的一部分可用   */

	/* 现在真正读取数据，已知其存在     */
	if (!WALRead(state,
				 cur_page,
				 targetPagePtr,
				 XLOG_BLCKSZ,
				 currTLI,		/* Pass the current TLI because only
								 * WalSndSegmentOpen 控制是否需要 TLI  
								 * is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/*
	 * 读入缓冲区后，检查我们读到的内容是否有效。这样  
	 * 做是因为，即使我们打开段文件时它还存在，在读取  
	 * 期间它可能被回收或移除。此时 read() 会成功，   
	 * 但我们尝试读取的数据可能已被新的 WAL 记录      
	 * 覆盖。                                          
	 */
	XLByteToSeg(targetPagePtr, segno, state->segcxt.ws_segsize);
	CheckXLogRemoved(segno, state->seg.ws_tli);

	return count;
}

/*
 * 处理 CREATE_REPLICATION_SLOT 的额外选项。      
 */
static void
parseCreateReplSlotOptions(CreateReplicationSlotCmd *cmd,
						   bool *reserve_wal,
						   CRSSnapshotAction *snapshot_action,
						   bool *two_phase)
{
	ListCell   *lc;
	bool		snapshot_action_given = false;
	bool		reserve_wal_given = false;
	bool		two_phase_given = false;

	/* 解析选项  */
	foreach(lc, cmd->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "snapshot") == 0)
		{
			char	   *action;

			if (snapshot_action_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			action = defGetString(defel);
			snapshot_action_given = true;

			if (strcmp(action, "export") == 0)
				*snapshot_action = CRS_EXPORT_SNAPSHOT;
			else if (strcmp(action, "nothing") == 0)
				*snapshot_action = CRS_NOEXPORT_SNAPSHOT;
			else if (strcmp(action, "use") == 0)
				*snapshot_action = CRS_USE_SNAPSHOT;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for CREATE_REPLICATION_SLOT option \"%s\": \"%s\"",
								defel->defname, action)));
		}
		else if (strcmp(defel->defname, "reserve_wal") == 0)
		{
			if (reserve_wal_given || cmd->kind != REPLICATION_KIND_PHYSICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			reserve_wal_given = true;
			*reserve_wal = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_given = true;
			*two_phase = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}
}

/*
 * 创建新的复制槽。      
 */
static void
CreateReplicationSlot(CreateReplicationSlotCmd *cmd)
{
	const char *snapshot_name = NULL;
	char		xloc[MAXFNAMELEN];
	char	   *slot_name;
	bool		reserve_wal = false;
	bool		two_phase = false;
	CRSSnapshotAction snapshot_action = CRS_EXPORT_SNAPSHOT;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};

	Assert(!MyReplicationSlot);

	parseCreateReplSlotOptions(cmd, &reserve_wal, &snapshot_action, &two_phase);

	if (cmd->kind == REPLICATION_KIND_PHYSICAL)
	{
		ReplicationSlotCreate(cmd->slotname, false,
							  cmd->temporary ? RS_TEMPORARY : RS_PERSISTENT,
							  false);
	}
	else
	{
		CheckLogicalDecodingRequirements();

		/*
		 * 先把持久槽创建为瞬态槽，这让我们能在初始化  
		 * 期间妥善处理错误，因为事务失败会丢弃它   
		 * 。最后将它变为持久槽。临时槽则可以从一         
		 * 开始就创建为临时槽，因为它们出错时同样会被   
		 * 丢弃。                         
		 */
		ReplicationSlotCreate(cmd->slotname, true,
							  cmd->temporary ? RS_TEMPORARY : RS_EPHEMERAL,
							  two_phase);
	}

	if (cmd->kind == REPLICATION_KIND_LOGICAL)
	{
		LogicalDecodingContext *ctx;
		bool		need_full_snapshot = false;

		/*
		 * 提前做选项检查，以便在调用可能耗时很久的 
		 * DecodingContextFindStartpoint 之前就退出。       
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			if (IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'export')")));

			need_full_snapshot = true;
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			if (!IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (XactIsoLevel != XACT_REPEATABLE_READ)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in REPEATABLE READ isolation mode transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));
			if (!XactReadOnly)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in a read-only transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (FirstSnapshotSet)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called before any query",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (IsSubTransaction())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called in a subtransaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			need_full_snapshot = true;
		}

		ctx = CreateInitDecodingContext(cmd->plugin, NIL, need_full_snapshot,
										InvalidXLogRecPtr,
										XL_ROUTINE(.page_read = logical_read_xlog_page,
												   .segment_open = WalSndSegmentOpen,
												   .segment_close = wal_segment_close),
										WalSndPrepareWrite, WalSndWriteData,
										WalSndUpdateProgress);

		/*
		 * 标记我们不需要超时机制。我们只是在创建  
		 * 复制槽，还不会接收反馈消息，也不发送     
		 * keepalive。因为可能需要等待更多 WAL，否则     
		 * WAL 发送进程可能会过早被终止                        
		 * 。  
		 */
		last_reply_timestamp = 0;

		/* 构建初始快照，可能耗时较久    */
		DecodingContextFindStartpoint(ctx);

		/*
		 * 若被要求这样做，则导出或使用该快照。  
		 *
		 * 注意，做这件事时我们会把 snapbuild.c 那种       
		 * 快照转成普通快照 
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			snapshot_name = SnapBuildExportSnapshot(ctx->snapshot_builder);
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			Snapshot	snap;

			snap = SnapBuildInitialSnapshot(ctx->snapshot_builder);
			RestoreTransactionSnapshot(snap, MyProc);
		}

		/* 不再需要解码上下文             */
		FreeDecodingContext(ctx);

		if (!cmd->temporary)
			ReplicationSlotPersist();
	}
	else if (cmd->kind == REPLICATION_KIND_PHYSICAL && reserve_wal)
	{
		ReplicationSlotReserveWal();

		ReplicationSlotMarkDirty();

		/* 如果这是持久槽，则将它写入磁盘。 */
		if (!cmd->temporary)
			ReplicationSlotSave();
	}

	snprintf(xloc, sizeof(xloc), "%X/%X",
			 LSN_FORMAT_ARGS(MyReplicationSlot->data.confirmed_flush));

	dest = CreateDestReceiver(DestRemoteSimple);

	/*----------
	 * 需要一个表示四列的元组描述符：     
	 * - 第一个：槽名称     
	 * - 第二个：我们达到一致性的 LSN       
	 * - 第三个：导出的快照名       
	 * - 第四个：输出插件   
	 *----------
	 */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "consistent_point",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "snapshot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "output_plugin",
							  TEXTOID, -1, 0);

	/* 为元组的投影做准备      */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* 槽名称 */
	slot_name = NameStr(MyReplicationSlot->data.name);
	values[0] = CStringGetTextDatum(slot_name);

	/* 一致性 WAL 位置    */
	values[1] = CStringGetTextDatum(xloc);

	/* 快照名称，无则为 NULL  */
	if (snapshot_name != NULL)
		values[2] = CStringGetTextDatum(snapshot_name);
	else
		nulls[2] = true;

	/* 插件，无则 NULL    */
	if (cmd->plugin != NULL)
		values[3] = CStringGetTextDatum(cmd->plugin);
	else
		nulls[3] = true;

	/* 发送给目标 */
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);

	ReplicationSlotRelease();
}

/*
 * 丢弃不再需要的复制槽。                      
 */
static void
DropReplicationSlot(DropReplicationSlotCmd *cmd)
{
	ReplicationSlotDrop(cmd->slotname, !cmd->wait);
}

/*
 * 加载先前启动的逻辑槽，准备经 WalSndLoop 发送数据      
 * 。         
 */
static void
StartLogicalReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;
	QueryCompletion qc;

	/* 确保我们的要求仍然得到满足             */
	CheckLogicalDecodingRequirements();

	Assert(!MyReplicationSlot);

	ReplicationSlotAcquire(cmd->slotname, true);

	/*
	 * 强制断开连接，这样解码代码就不需要关心         
	 * 从在恢复中运行最终切换为在正常环境中运行      
	 * 可能的切换。预期客户端代码会处理重连。        
	 */
	if (am_cascading_walsender && !RecoveryInProgress())
	{
		ereport(LOG,
				(errmsg("terminating walsender process after promotion")));
		got_STOPPING = true;
	}

	/*
	 * 创建我们的解码上下文，让它从先前已确认的位置   
	 * 开始   
	 *
	 * 在发送 CopyBothResponse 消息之前先做这件事，以便      
	 * 尽早报出错误 
	 */
	logical_decoding_ctx =
		CreateDecodingContext(cmd->startpoint, cmd->options, false,
							  XL_ROUTINE(.page_read = logical_read_xlog_page,
										 .segment_open = WalSndSegmentOpen,
										 .segment_close = wal_segment_close),
							  WalSndPrepareWrite, WalSndWriteData,
							  WalSndUpdateProgress);
	xlogreader = logical_decoding_ctx->reader;

	WalSndSetState(WALSNDSTATE_CATCHUP);

	/* 发送 CopyBothResponse，随后开始流式传输   */
	pq_beginmessage(&buf, 'W');
	pq_sendbyte(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);
	pq_flush();

	/* 从所需的最旧 WAL 开始读取。          */
	XLogBeginRead(logical_decoding_ctx->reader,
				  MyReplicationSlot->data.restart_lsn);

	/*
	 * 报告我们会在其后才发送更多提交的那个位置，即   
	 * 当前 sentPtr  
	 */
	sentPtr = MyReplicationSlot->data.confirmed_flush;

	/* 同时更新共享内存中的已发送位置状态   */
	SpinLockAcquire(&MyWalSnd->mutex);
	MyWalSnd->sentPtr = MyReplicationSlot->data.restart_lsn;
	SpinLockRelease(&MyWalSnd->mutex);

	replication_active = true;

	SyncRepInitConfig();

	/* walsender 主循环    */
	WalSndLoop(XLogSendLogical);

	FreeDecodingContext(logical_decoding_ctx);
	ReplicationSlotRelease();

	replication_active = false;
	if (got_STOPPING)
		proc_exit(0);
	WalSndSetState(WALSNDSTATE_STARTUP);

	/* 退出 COPY 模式（CommandComplete） */
	SetQueryCompletion(&qc, CMDTAG_COPY, 0);
	EndCommand(&qc, DestRemote, false);
}

/*
 * LogicalDecodingContext 'prepare_write' 回调   
 *
 * 准备写入 StringInfo。        
 *
 * 在这里不要做任何持久性的事情，数据很可能根本              
 * 用不上     
 */
static void
WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write)
{
	/* 无法让同步复制因多次发送同一 LSN 而产生混淆     */
	if (!last_write)
		lsn = InvalidXLogRecPtr;

	resetStringInfo(ctx->out);

	pq_sendbyte(ctx->out, 'w');
	pq_sendint64(ctx->out, lsn);	/* dataStart */
	pq_sendint64(ctx->out, lsn);	/* walEnd */

	/*
	 * sendtime 字段稍后再填，做法与 XLogSendPhysical 相同，     
	 * 但此处占位。 
	 */
	pq_sendint64(ctx->out, 0);	/* sendtime */
}

/*
 * LogicalDecodingContext 'write' 回调   
 *
 * 把先前由 WalSndPrepareWrite 准备好的数据真正写出          
 * 到网络。需要多久就用多久，但在此期间要处理对端  
 * 的回复并检查超时。         
 */
static void
WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				bool last_write)
{
	TimestampTz now;

	/*
	 * 把发送时间戳放在最后填写，这样它就能尽可能晚    
	 * 地被取值。这有些丑陋，但协议已经定型，因为物理  
	 * 流复制已经使用了多个发布版本。      
	 */
	resetStringInfo(&tmpbuf);
	now = GetCurrentTimestamp();
	pq_sendint64(&tmpbuf, now);
	memcpy(&ctx->out->data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	/* 在 CopyData 包中输出先前收集的数据       */
	pq_putmessage_noblock('d', ctx->out->data, ctx->out->len);

	CHECK_FOR_INTERRUPTS();

	/* 尝试把待输出数据刷写到客户端*/
	if (pq_flush_if_writable() != 0)
		WalSndShutdown();

	/* 除非临近 WAL 发送进程超时，否则尝试走快速路径  */
	if (now < TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout / 2) &&
		!pq_is_send_pending())
	{
		return;
	}

	/* 若此处有待写入，则走慢速路径     */
	ProcessPendingWrites();
}

/*
 * 等待直到没有待处理的写入。在此期间还要处理对端    
 * 的回复，并检查超时。      
 */
static void
ProcessPendingWrites(void)
{
	for (;;)
	{
		long		sleeptime;

		/* 检查来自客户端的输入  */
		ProcessRepliesIfAny();

		/* 若已到超时则退出   */
		WalSndCheckTimeOut();

		/* 到时间就发送 keepalive        */
		WalSndKeepaliveIfNecessary();

		if (!pq_is_send_pending())
			break;

		sleeptime = WalSndComputeSleeptime(GetCurrentTimestamp());

		/* 休眠直到有事发生或我们超时      */
		WalSndWait(WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE, sleeptime,
				   WAIT_EVENT_WAL_SENDER_WRITE_DATA);

		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* 处理最近收到的任何请求或信号        */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* 尝试把待输出数据刷写到客户端*/
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}

	/* 重新激活锁存器让 WalSndLoop 知道继续 */
	SetLatch(MyLatch);
}

/*
 * LogicalDecodingContext 'update_progress' 回调   
 *
 * 把当前位置写入延迟追踪器（参见 XLogSendPhysical）。 
 *
 * 跳过空事务时，若有必要则发送 keepalive 消息。          
 */
static void
WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
					 bool skipped_xact)
{
	static TimestampTz sendTime = 0;
	TimestampTz now = GetCurrentTimestamp();
	bool		pending_writes = false;
	bool		end_xact = ctx->end_xact;

	/*
	 * 每 WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS 至多追踪                  
	 * 一次滞后；提交过频会淹没滞后追踪器。   
	 *
	 * 我们无法从下游获得事务结束 LSN 之外任何 LSN 的   
	 * 确认，所以我们只追踪事务结束 LSN 的滞后      
	 * 。             
	 */
#define WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS	1000
	if (end_xact && TimestampDifferenceExceeds(sendTime, now,
											   WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS))
	{
		LagTrackerWrite(lsn, now);
		sendTime = now;
	}

	/*
	 * 在同步复制中跳过空事务时，我们会发送一条          
	 * keepalive 消息，以免延误这类事务。        
	 *
	 * 此处不加锁检查 sync_standbys_status 也是可以的，       
	 * 因为最坏的情况只是发送了一条其实并不需要的   
	 * 多余 keepalive    
	 */
	if (skipped_xact &&
		SyncRepRequested() &&
		(((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status & SYNC_STANDBY_DEFINED))
	{
		WalSndKeepalive(false, lsn);

		/* 尝试把待输出数据刷写到客户端*/
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* 如果此处有待写入，则确保它确实被刷写         */
		if (pq_is_send_pending())
			pending_writes = true;
	}

	/*
	 * 处理待处理的写入（若有），或在需要时尝试发送   
	 * keepalive。事务结束时无需尝试发送 keepalive 消息，      
	 * 因为那会在稍后的时间点完成。只有大型事务才需  
	 * 要这样：这类事务不向下游发送任何变更，接 
	 * 收方可能因此超时。                         
	 */
	if (pending_writes || (!end_xact &&
						   now >= TimestampTzPlusMilliseconds(last_reply_timestamp,
															  wal_sender_timeout / 2)))
		ProcessPendingWrites();
}

/*
 * 等待 WAL < loc 被刷写到磁盘，这样它就能被安全地         
 *
 * 发送给客户端。返回已刷写 WAL 的末尾 LSN。通常   
 * 它会 >= loc，但若来自 postmaster 或客户端的关闭     
 * 请求，则提前返回，调用方须检查。  
 */
static XLogRecPtr
WalSndWaitForWal(XLogRecPtr loc)
{
	int			wakeEvents;
	static XLogRecPtr RecentFlushPtr = InvalidXLogRecPtr;
	TimestampTz last_flush = 0;

	/*
	 * 快速路径：如果已经知道有足够可用的 WAL，就避免 
	 * 获取自旋锁。如果我们远远落后，这条路径就尤其  
	 * 有用。  
	 */
	if (RecentFlushPtr != InvalidXLogRecPtr &&
		loc <= RecentFlushPtr)
		return RecentFlushPtr;

	/* 获取更新的刷写指针。   */
	if (!RecoveryInProgress())
		RecentFlushPtr = GetFlushRecPtr(NULL);
	else
		RecentFlushPtr = GetXLogReplayRecPtr(NULL);

	for (;;)
	{
		long		sleeptime;
		TimestampTz now;

		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* 处理最近收到的任何请求或信号        */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* 检查来自客户端的输入  */
		ProcessRepliesIfAny();

		/*
		 * 若我们在关闭，则触发把待处理的 WAL 写出，   
		 * 否则我们可能最终等待永远写不出的 WAL，       
		 * 因为 walwriter 已经先关闭了。           
		 */
		if (got_STOPPING)
			XLogBackgroundFlush();

		/* 更新我们对当前已刷写位置的认知。   */
		if (!RecoveryInProgress())
			RecentFlushPtr = GetFlushRecPtr(NULL);
		else
			RecentFlushPtr = GetXLogReplayRecPtr(NULL);

		/*
		 * 若 postmaster 要求停止，就不再等待。   
		 *
		 * 这项检查必须在重新计算 RecentFlushPtr 之后   
		 * 进行，这样关闭前能把其余数据全部发出           
		 * 。  
		 */
		if (got_STOPPING)
			break;

		/*
		 * 只对完整解码的事务向客户端发常规消息，   
		 * 但同步复制和 WAL 发送进程关闭都可能正等待更    
		 * 靠后的位置。所以在休眠之前，我们发送一条包   
		 * 含刷写位置的 ping。若接收方本就空闲，这条  
		 * keepalive 将触发一条回复。处理该回复会更新这些   
		 * MyWalSnd 位置。                         
		 */
		if (MyWalSnd->flush < sentPtr &&
			MyWalSnd->write < sentPtr &&
			!waiting_for_ping_response)
			WalSndKeepalive(false, InvalidXLogRecPtr);

		/* 检查是否已完成    */
		if (loc <= RecentFlushPtr)
			break;

		/* 正在等待新 WAL。既然要等，说明已赶上进度。    */
		WalSndCaughtUp = true;

		/*
		 * 尝试把待处理输出刷写给客户端。 
		 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/*
		 * 若已从客户端收到 CopyDone，自己也发了        
		 * CopyDone，且输出缓冲区为空，就该退出流式传 
		 * 输了，因此当前 WAL 获取请求失败。  
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
			break;

		/* 若已到超时则退出   */
		WalSndCheckTimeOut();

		/* 到时间就发送 keepalive        */
		WalSndKeepaliveIfNecessary();

		/*
		 * 休眠直到有事发生或超时。若仍有待输出数据， 
		 * 则还要等套接字可写，否则在等新 WAL 生成  
		 * 期间，我们可能一直压着可发送的输出数据。    
		 * （但如果我们没有要发送的东西，就不希望因       
		 * 套接字可写而醒来）      
		 */
		now = GetCurrentTimestamp();
		sleeptime = WalSndComputeSleeptime(now);

		wakeEvents = WL_SOCKET_READABLE;

		if (pq_is_send_pending())
			wakeEvents |= WL_SOCKET_WRITEABLE;

		/* 需要时报告 IO 统计信息 */
		if (TimestampDifferenceExceeds(last_flush, now,
									   WALSENDER_STATS_FLUSH_INTERVAL))
		{
			pgstat_flush_io(false);
			last_flush = now;
		}

		WalSndWait(wakeEvents, sleeptime, WAIT_EVENT_WAL_SENDER_WAIT_WAL);
	}

	/* 重新激活锁存器让 WalSndLoop 知道继续 */
	SetLatch(MyLatch);
	return RecentFlushPtr;
}

/*
 * 执行一条传入的复制命令。    
 *
 * 如果 cmd_string 被识别为 WalSender 命令则返回真，否则     
 * 为假 
 */
bool
exec_replication_command(const char *cmd_string)
{
	int			parse_rc;
	Node	   *cmd_node;
	const char *cmdtag;
	MemoryContext cmd_context;
	MemoryContext old_context;

	/*
	 * 如果已告知 WAL 发送进程关闭即将到来，则据此        
	 * 切换其状态，以便正确处理接下来的复制命令。      
	 */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	/*
	 * 如果处于停止模式则抛出错误。在关闭检查点正在   
	 * 被写出的期间，需要阻止可能产生 WAL 的命令。     
	 * 安全起见，干脆禁止新命令。 
	 */
	if (MyWalSnd->state == WALSNDSTATE_STOPPING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot execute new commands while WAL sender is in stopping mode")));

	/*
	 * CREATE_REPLICATION_SLOT ... LOGICAL 会导出一个快               
	 * 照，保留到下一命令到来。有残留就清除。   
	 */
	SnapBuildClearExportedSnapshot();

	CHECK_FOR_INTERRUPTS();

	/*
	 * 准备解析并执行该命令。        
	 */
	cmd_context = AllocSetContextCreate(CurrentMemoryContext,
										"Replication command context",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(cmd_context);

	replication_scanner_init(cmd_string);

	/*
	 * 是 WalSender 命令？   
	 */
	if (!replication_scanner_is_replication_command())
	{
		/* 不是；清理后退出。 */
		replication_scanner_finish();

		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(cmd_context);

		/* XXX 在这里做这项检查的位置相当随意    */
		if (MyDatabaseId == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot execute SQL commands in WAL sender for physical replication")));

		/* 告诉调用者这不是一条 WalSender 命令。    */
		return false;
	}

	/*
	 * 看似 WalSender 命令，故解析。      
	 */
	parse_rc = replication_yyparse();
	if (parse_rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_internal("replication command parser returned %d",
								 parse_rc)));
	replication_scanner_finish();

	cmd_node = replication_parse_result;

	/*
	 * 向各种监控设施报告查询。为此目的，                 
	 * 我们像 SQL 命令一样报告复制命令。    
	 */
	debug_query_string = cmd_string;

	pgstat_report_activity(STATE_RUNNING, cmd_string);

	/*
	 * 若已启用 log_replication_commands 则记录复制命令。      
	 * 即便被禁用，也以 DEBUG1 级别记录该命令，以保持 
	 * 兼容性。  
	 */
	ereport(log_replication_commands ? LOG : DEBUG1,
			(errmsg("received replication command: %s", cmd_string)));

	/*
	 * 不允许在已中止的事务块中使用复制命令。   
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	CHECK_FOR_INTERRUPTS();

	/*
	 * 分配将用于每条发出与接收消息的缓冲区。        
	 * 我们每条命令只分配一次，以减少 palloc 开销。       
	 */
	initStringInfo(&output_message);
	initStringInfo(&reply_message);
	initStringInfo(&tmpbuf);

	switch (cmd_node->type)
	{
		case T_IdentifySystemCmd:
			cmdtag = "IDENTIFY_SYSTEM";
			set_ps_display(cmdtag);
			IdentifySystem();
			EndReplicationCommand(cmdtag);
			break;

		case T_ReadReplicationSlotCmd:
			cmdtag = "READ_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			ReadReplicationSlot((ReadReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_BaseBackupCmd:
			cmdtag = "BASE_BACKUP";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendBaseBackup((BaseBackupCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_CreateReplicationSlotCmd:
			cmdtag = "CREATE_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			CreateReplicationSlot((CreateReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_DropReplicationSlotCmd:
			cmdtag = "DROP_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			DropReplicationSlot((DropReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_StartReplicationCmd:
			{
				StartReplicationCmd *cmd = (StartReplicationCmd *) cmd_node;

				cmdtag = "START_REPLICATION";
				set_ps_display(cmdtag);
				PreventInTransactionBlock(true, cmdtag);

				if (cmd->kind == REPLICATION_KIND_PHYSICAL)
					StartReplication(cmd);
				else
					StartLogicalReplication(cmd);

				/* 重复，但 libpqrcv_endstreaming 需要它  */
				EndReplicationCommand(cmdtag);

				Assert(xlogreader != NULL);
				break;
			}

		case T_TimeLineHistoryCmd:
			cmdtag = "TIMELINE_HISTORY";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendTimeLineHistory((TimeLineHistoryCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_VariableShowStmt:
			{
				DestReceiver *dest = CreateDestReceiver(DestRemoteSimple);
				VariableShowStmt *n = (VariableShowStmt *) cmd_node;

				cmdtag = "SHOW";
				set_ps_display(cmdtag);

				/* syscache 访问需要一个事务环境         */
				StartTransactionCommand();
				GetPGVariable(n->name, dest);
				CommitTransactionCommand();
				EndReplicationCommand(cmdtag);
			}
			break;

		default:
			elog(ERROR, "unrecognized replication command node tag: %u",
				 cmd_node->type);
	}

	/* done */
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(cmd_context);

	/*
	 * 我们无需更新 ps display 或 pg_stat_activity；PostgresMain      
	 * 会把它们重置为"idle"。但必须重置 debug_query_string    
	 * 以确保它不会成为悬空指针。     
	 */
	debug_query_string = NULL;

	return true;
}

/*
 * 在流式传输期间处理任何收到的消息。同时检查远端   
 * 是否已关闭连接。      
 */
static void
ProcessRepliesIfAny(void)
{
	unsigned char firstchar;
	int			maxmsglen;
	int			r;
	bool		received = false;

	last_processing = GetCurrentTimestamp();

	/*
	 * 如果我们已经从前端收到 CopyDone，则后续任何消息 
	 * 都是新命令的开头，并且应当在主处理循环中         
	 * 处理。                
	 */
	while (!streamingDoneReceiving)
	{
		pq_startmsgread();
		r = pq_getbyte_if_available(&firstchar);
		if (r < 0)
		{
			/* 意外错误或 EOF     */
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on standby connection")));
			proc_exit(0);
		}
		if (r == 0)
		{
			/* 非阻塞模式下无可用数据  */
			pq_endmsgread();
			break;
		}

		/* 校验消息类型并设置包大小上限      */
		switch (firstchar)
		{
			case 'd':
				maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
				break;
			case 'c':
			case 'X':
				maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
				break;
			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby message type \"%c\"",
								firstchar)));
				maxmsglen = 0;	/* 让编译器安静  */
				break;
		}

		/* 读取消息内容        */
		resetStringInfo(&reply_message);
		if (pq_getmessage(&reply_message, maxmsglen))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on standby connection")));
			proc_exit(0);
		}

		/* 随后处理它    */
		switch (firstchar)
		{
				/*
				 * 'd' 表示封装在 CopyData 包内的备库应答。  
				 */
			case 'd':
				ProcessStandbyMessage();
				received = true;
				break;

				/*
				 * CopyDone 表示备库请求结束流式传输。         
				 * 如果我们尚未发送，则回应 CopyDone。     
				 */
			case 'c':
				if (!streamingDoneSending)
				{
					pq_putmessage_noblock('c', NULL, 0);
					streamingDoneSending = true;
				}

				streamingDoneReceiving = true;
				received = true;
				break;

				/*
				 * 'X' 表示备库正在关闭套接字。              
				 */
			case 'X':
				proc_exit(0);

			default:
				Assert(false);	/* NOT REACHED */
		}
	}

	/*
	 * 若至少收到一个应答，则保存最后应答时间戳。    
	 */
	if (received)
	{
		last_reply_timestamp = last_processing;
		waiting_for_ping_response = false;
	}
}

/*
 * 处理从备库收到的状态更新消息。         
 */
static void
ProcessStandbyMessage(void)
{
	char		msgtype;

	/*
	 * 从首字节检查消息类型。      
	 */
	msgtype = pq_getmsgbyte(&reply_message);

	switch (msgtype)
	{
		case 'r':
			ProcessStandbyReplyMessage();
			break;

		case 'h':
			ProcessStandbyHSFeedbackMessage();
			break;

		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected message type \"%c\"", msgtype)));
			proc_exit(0);
	}
}

/*
 * 记住某个 walreceiver 刚确认收到了 lsn `lsn`。        
 */
static void
PhysicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	bool		changed = false;
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(lsn != InvalidXLogRecPtr);
	SpinLockAcquire(&slot->mutex);
	if (slot->data.restart_lsn != lsn)
	{
		changed = true;
		slot->data.restart_lsn = lsn;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredLSN();
	}

	/*
	 * 有人可能会说现在就该把槽保存到磁盘，但那纯属   
	 * 白费功夫：信息丢失在此处最坏的后果是                 
	 * 在统计视图中给出错误信息，我们至多只会              
	 * 在删除文件时更为保守。      
	 *
	 * 检查点进程会特别努力把 restart_lsn 所需的 WAL 段保留  
	 * 在磁盘上。详情参见 CreateCheckPoint() 与             
	 * CreateRestartPoint() 的说明   
	 */
}

/*
 * 备库的常规应答，告知备库服务器上的 WAL 位置。      
 */
static void
ProcessStandbyReplyMessage(void)
{
	XLogRecPtr	writePtr,
				flushPtr,
				applyPtr;
	bool		replyRequested;
	TimeOffset	writeLag,
				flushLag,
				applyLag;
	bool		clearLagTimes;
	TimestampTz now;
	TimestampTz replyTime;

	static bool fullyAppliedLastTime = false;

	/* 调用者已消费 msgtype 字节            */
	writePtr = pq_getmsgint64(&reply_message);
	flushPtr = pq_getmsgint64(&reply_message);
	applyPtr = pq_getmsgint64(&reply_message);
	replyTime = pq_getmsgint64(&reply_message);
	replyRequested = pq_getmsgbyte(&reply_message);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* 复制，因 timestamptz_to_str 返回静态缓冲区   */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "write %X/%X flush %X/%X apply %X/%X%s reply_time %s",
			 LSN_FORMAT_ARGS(writePtr),
			 LSN_FORMAT_ARGS(flushPtr),
			 LSN_FORMAT_ARGS(applyPtr),
			 replyRequested ? " (reply requested)" : "",
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/* 看看我们能否为这些位置计算往返滞后。        */
	now = GetCurrentTimestamp();
	writeLag = LagTrackerRead(SYNC_REP_WAIT_WRITE, writePtr, now);
	flushLag = LagTrackerRead(SYNC_REP_WAIT_FLUSH, flushPtr, now);
	applyLag = LagTrackerRead(SYNC_REP_WAIT_APPLY, applyPtr, now);

	/*
	 * 如果备库在连续两条应答消息中均报告已          
	 * 完整重放 WAL，则第二条消息必然是备库上             
	 * wal_receiver_status_interval 到期所致。这是忘掉它          
	 * 上次写入/刷写/应用记录时测得的滞后时间，  
	 * 以免在更多 WAL 流量到来之前显示                          
	 * 陈旧的滞后数据。       
	 */
	clearLagTimes = false;
	if (applyPtr == sentPtr)
	{
		if (fullyAppliedLastTime)
			clearLagTimes = true;
		fullyAppliedLastTime = true;
	}
	else
		fullyAppliedLastTime = false;

	/* 若备库请求应答，则发送之。    */
	if (replyRequested)
		WalSndKeepalive(false, InvalidXLogRecPtr);

	/*
	 * 根据备库应答数据更新本 WalSender 进程的共享状         
	 * 态。  
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->write = writePtr;
		walsnd->flush = flushPtr;
		walsnd->apply = applyPtr;
		if (writeLag != -1 || clearLagTimes)
			walsnd->writeLag = writeLag;
		if (flushLag != -1 || clearLagTimes)
			walsnd->flushLag = flushLag;
		if (applyLag != -1 || clearLagTimes)
			walsnd->applyLag = applyLag;
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	if (!am_cascading_walsender)
		SyncRepReleaseWaiters();

	/*
	 * 客户端确认刷写后，推进本地的 xmin 视界。        
	 */
	if (MyReplicationSlot && flushPtr != InvalidXLogRecPtr)
	{
		if (SlotIsLogical(MyReplicationSlot))
			LogicalConfirmReceivedLocation(flushPtr);
		else
			PhysicalConfirmReceivedLocation(flushPtr);
	}
}

/* 如有需要，计算新的复制槽 xmin 视界    */
static void
PhysicalReplicationSlotNewXmin(TransactionId feedbackXmin, TransactionId feedbackCatalogXmin)
{
	bool		changed = false;
	ReplicationSlot *slot = MyReplicationSlot;

	SpinLockAcquire(&slot->mutex);
	MyProc->xmin = InvalidTransactionId;

	/*
	 * 物理复制不需要 xmin 与 effective_xmin 提供的              
	 * 互锁机制，因为漏掉一次增加的后果不过是         
	 * 查询取消而已，故一次性设置两者。    
	 */
	if (!TransactionIdIsNormal(slot->data.xmin) ||
		!TransactionIdIsNormal(feedbackXmin) ||
		TransactionIdPrecedes(slot->data.xmin, feedbackXmin))
	{
		changed = true;
		slot->data.xmin = feedbackXmin;
		slot->effective_xmin = feedbackXmin;
	}
	if (!TransactionIdIsNormal(slot->data.catalog_xmin) ||
		!TransactionIdIsNormal(feedbackCatalogXmin) ||
		TransactionIdPrecedes(slot->data.catalog_xmin, feedbackCatalogXmin))
	{
		changed = true;
		slot->data.catalog_xmin = feedbackCatalogXmin;
		slot->effective_catalog_xmin = feedbackCatalogXmin;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
	}
}

/*
 * 检查所给的 xmin/epoch 是否合理，即不在未来，           
 * 也未倒退到已经回绕的地步。             
 *
 * nextXid 的纪元应与备库相同；若计数器已回绕，   
 * 则比备库大 1。                   
 *
 * 这项检查完全不关心这些 xid 的 clog 是否存在     
 * 。    
 */
static bool
TransactionIdInRecentPast(TransactionId xid, uint32 epoch)
{
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		nextEpoch;

	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	nextEpoch = EpochFromFullTransactionId(nextFullXid);

	if (xid <= nextXid)
	{
		if (epoch != nextEpoch)
			return false;
	}
	else
	{
		if (epoch + 1 != nextEpoch)
			return false;
	}

	if (!TransactionIdPrecedesOrEquals(xid, nextXid))
		return false;			/* 纪元 OK，但已回绕          */

	return true;
}

/*
 * 热备反馈        
 */
static void
ProcessStandbyHSFeedbackMessage(void)
{
	TransactionId feedbackXmin;
	uint32		feedbackEpoch;
	TransactionId feedbackCatalogXmin;
	uint32		feedbackCatalogEpoch;
	TimestampTz replyTime;

	/*
	 * 解读应答消息。调用者已消费 msgtype 字节。此消息 
	 * 由 walreceiver.c 中的 XLogWalRcvSendHSFeedback()                   
	 * 构造。       
	 */
	replyTime = pq_getmsgint64(&reply_message);
	feedbackXmin = pq_getmsgint(&reply_message, 4);
	feedbackEpoch = pq_getmsgint(&reply_message, 4);
	feedbackCatalogXmin = pq_getmsgint(&reply_message, 4);
	feedbackCatalogEpoch = pq_getmsgint(&reply_message, 4);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* 复制，因 timestamptz_to_str 返回静态缓冲区   */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "hot standby feedback xmin %u epoch %u, catalog_xmin %u epoch %u reply_time %s",
			 feedbackXmin,
			 feedbackEpoch,
			 feedbackCatalogXmin,
			 feedbackCatalogEpoch,
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/*
	 * 根据备库应答数据更新本 WalSender 进程的共享状         
	 * 态。  
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	/*
	 * 若反馈消息的值无效，则清除 WalSender 的 xmin。      
	 * 当下游关闭 hot_standby_feedback 时便会如此。          
	 */
	if (!TransactionIdIsNormal(feedbackXmin)
		&& !TransactionIdIsNormal(feedbackCatalogXmin))
	{
		MyProc->xmin = InvalidTransactionId;
		if (MyReplicationSlot != NULL)
			PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
		return;
	}

	/*
	 * 检查所给的 xmin/epoch 是否合理，即不在未来，           
	 * 也未倒退到已经回绕的地步。若不合理则忽略。     
	 */
	if (TransactionIdIsNormal(feedbackXmin) &&
		!TransactionIdInRecentPast(feedbackXmin, feedbackEpoch))
		return;

	if (TransactionIdIsNormal(feedbackCatalogXmin) &&
		!TransactionIdInRecentPast(feedbackCatalogXmin, feedbackCatalogEpoch))
		return;

	/*
	 * 把 WalSender 的 xmin 设为备库请求的 xmin，这样             
	 * GetSnapshotData() / ComputeXidHorizons() 会考虑        
	 * 该 xmin。这会拖住死行的清除，从而防止在备库         
	 * 服务器上产生清理冲突                                    
	 * 。    
	 *
	 * 这里存在一个小小的竞态条件窗口：虽然我们刚刚 
	 * 检查过 feedbackXmin 位于 nextXid 之前，但在我们       
	 * 取到它并在下方应用 xmin 之间，nextXid 可能已经      
	 * 前进到使 feedbackXmin 回绕的程度。那样的话，           
	 * 我们在这里设置的 xmin 将是"未来的"并且不起作用。   
	 * 无需为此担忧，反正要保住所需数据已为时过晚。 
	 * 假设备库向我们发送的是递增的 xmin 序列，那么      
	 * 这只可能发生在第一轮应答期间，否则我们自己的  
	 * xmin 会阻止 nextXid 前进得这么远。         
	 *
	 * 我们不为此费神去拿 ProcArrayLock。设置 xmin 字段        
	 * 被假定是原子的，并且确实无需阻止并发的视界  
	 * 判定。（如果我们把 xmin 向前移动，这显然安全； 
	 * 而如果我们把它向后移动，那么数据本来就处于       
	 * 危险之中，因为 VACUUM 可能早已判定过视界。）         
	 *
	 * 如果使用复制槽，我们就通过该槽保留 xmin，    
	 * 否则通过 walsender 的 PGPROC 条目保留。只有使用    
	 * 槽时才能分开跟踪系统表 xmin，所以不使用槽时，      
	 * 保存两者中较小的一个。  
	 *
	 * XXX：也许值得泛化瞬态槽的概念，总是使用槽机制   
	 * 来处理反馈 xmin。                                   
	 */
	if (MyReplicationSlot != NULL)	/* XXX：持久性可配置吗？ */
		PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
	else
	{
		if (TransactionIdIsNormal(feedbackCatalogXmin)
			&& TransactionIdPrecedes(feedbackCatalogXmin, feedbackXmin))
			MyProc->xmin = feedbackCatalogXmin;
		else
			MyProc->xmin = feedbackXmin;
	}
}

/*
 * 计算发送/接收循环该睡多久。         
 *
 * 如果启用了 wal_sender_timeout，我们希望及时醒来      
 * 发送保活，并在 wal_sender_timeout 到期时中止连接       
 * 。     
 */
static long
WalSndComputeSleeptime(TimestampTz now)
{
	long		sleeptime = 10000;	/* 10 s */

	if (wal_sender_timeout > 0 && last_reply_timestamp > 0)
	{
		TimestampTz wakeup_time;

		/*
		 * 最迟在 wal_sender_timeout 到期时停止睡眠          
		 * 。     
		 */
		wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
												  wal_sender_timeout);

		/*
		 * 如果尚未发送过 ping，则在到点时唤醒。        
		 * WalSndKeepaliveIfNecessary() 希望在一半超时过去           
		 * 而无响应时即发送保活。     
		 */
		if (!waiting_for_ping_response)
			wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
													  wal_sender_timeout / 2);

		/* 计算距唤醒的相对时间。   */
		sleeptime = TimestampDifferenceMilliseconds(now, wakeup_time);
	}

	return sleeptime;
}

/*
 * 检查客户端在 wal_sender_timeout 内有无响应，    
 * 若无则关闭。以 last_processing 为基准点，可避免       
 * 把服务器端的停滞算到客户端头上。然而，长时间的 
 * 服务器端停滞可能使 WalSndKeepaliveIfNecessary() 把           
 * last_processing 推后超过 wal_sender_timeout。若如此，             
 * 客户端就必须几乎立即回复才能避免超时。这在默认    
 * 配置下很少成为问题：在该配置下客户端会每隔            
 * standby_message_timeout = wal_sender_timeout/6 = 10s                   
 * 自发地发送消息。在保活后 wal_sender_timeout/2        
 * 处识别超时即可消除该问题。  
 */
static void
WalSndCheckTimeOut(void)
{
	TimestampTz timeout;

	/* 如果我们正在做的事情不需要超时机制，则不要退出 */
	if (last_reply_timestamp <= 0)
		return;

	timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout);

	if (wal_sender_timeout > 0 && last_processing >= timeout)
	{
		/*
		 * 由于复制超时到期通常意味着                
		 * 通信问题，故我们不向备库发送错误消息       
		 * 。     
		 */
		ereport(COMMERROR,
				(errmsg("terminating walsender process due to replication timeout")));

		WalSndShutdown();
	}
}

/* 通过 Copy 消息流式发送 WAL 的 walsender 进程主循环。     */
static void
WalSndLoop(WalSndSendDataCallback send_data)
{
	TimestampTz last_flush = 0;

	/*
	 * 初始化最后应答时间戳。从此起即可处理超时        
	 * 。         
	 */
	last_reply_timestamp = GetCurrentTimestamp();
	waiting_for_ping_response = false;

	/*
	 * 循环，直到到达此时间线的末尾，或客户端请求停止 
	 * 传输。      
	 */
	for (;;)
	{
		/* 清除任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* 处理最近收到的任何请求或信号        */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* 检查来自客户端的输入  */
		ProcessRepliesIfAny();

		/*
		 * 如果已收到客户端的 CopyDone，而且自己也已  
		 * 发送 CopyDone，输出缓冲为空就该退流式         
		 * 发送。 
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
			break;

		/*
		 * 若输出缓冲区无待发数据，则尝试再发送一些。    
		 * 若有一些，则在刷写完之前就无需再调用         
		 * send_data ... 但我们最好假定尚未                         
		 * 追上。 
		 */
		if (!pq_is_send_pending())
			send_data();
		else
			WalSndCaughtUp = false;

		/* 尝试向客户端刷写待发输出      */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* 若眼下没有剩余待发内容……     */
		if (WalSndCaughtUp && !pq_is_send_pending())
		{
			/*
			 * 若处于追赶状态，则转入流式传输。这是   
			 * 用户须要知道的重要状态变更，因为在此      
			 * 之前，若主库死亡且需故障切换到备库，     
			 * 就可能丢失数据。该状态变更对同步复制   
			 * 也很重要，因为在那一刻开始等待的提交   
			 * 可能要等上一段时间。                         
			 */
			if (MyWalSnd->state == WALSNDSTATE_CATCHUP)
			{
				ereport(DEBUG1,
						(errmsg_internal("\"%s\" has now caught up with upstream server",
										 application_name)));
				WalSndSetState(WALSNDSTATE_STREAMING);
			}

			/*
			 * SIGUSR2 到来时，我们把所有未发的日志一直    
			 * 发送至关闭检查点记录（即最新记录），等待  
			 * 它们复制到备库，随后退出。这可能是关闭时 
			 * 的正常终止，也可能是提升，walsender 不确定   
			 * 是哪一种。   
			 */
			if (got_SIGUSR2)
				WalSndDone(send_data);
		}

		/* 检查复制是否已超时。 */
		WalSndCheckTimeOut();

		/* 如果时间已到，则发送保活*/
		WalSndKeepaliveIfNecessary();

		/*
		 * 如果有未发送的数据则阻塞。XXX 逻辑复制交给  
		 * WalSndWaitForWal() 处理其他任何阻塞；空闲的接       
		 * 收端需要它的额外操作。物理复制则在已追上时 
		 * 也阻塞；其 send_data 不阻塞。  
		 *
		 * IO 统计在 WalSndWaitForWal() 中报告，对象为逻辑 
		 * WAL 发送进程    
		 */
		if ((WalSndCaughtUp && send_data != XLogSendLogical &&
			 !streamingDoneSending) ||
			pq_is_send_pending())
		{
			long		sleeptime;
			int			wakeEvents;
			TimestampTz now;

			if (!streamingDoneReceiving)
				wakeEvents = WL_SOCKET_READABLE;
			else
				wakeEvents = 0;

			/*
			 * 用新时间戳而非 last_processing，以减少在发       
			 * 保活之前就达到 wal_sender_timeout 的机会。     
			 */
			now = GetCurrentTimestamp();
			sleeptime = WalSndComputeSleeptime(now);

			if (pq_is_send_pending())
				wakeEvents |= WL_SOCKET_WRITEABLE;

			/* 如需要则报告 IO 统计    */
			if (TimestampDifferenceExceeds(last_flush, now,
										   WALSENDER_STATS_FLUSH_INTERVAL))
			{
				pgstat_flush_io(false);
				last_flush = now;
			}

			/* 睡眠直到有事件发生或我们超时   */
			WalSndWait(wakeEvents, sleeptime, WAIT_EVENT_WAL_SENDER_MAIN);
		}
	}
}

/* 为该 walsender 进程初始化 per-walsender 数据结构          */
static void
InitWalSenderSlot(void)
{
	int			i;

	/*
	 * WalSndCtl 应当已经设置好（我们通过 fork() 或       
	 * EXEC_BACKEND 机制从 postmaster 继承） 
	 */
	Assert(WalSndCtl != NULL);
	Assert(MyWalSnd == NULL);

	/*
	 * 找一个空闲 walsender 槽并保留。InitProcess() 已先行    
	 * 检查过空闲 WAL 发送进程，故不会失败。 
	 */
	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);

		if (walsnd->pid != 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		else
		{
			/*
			 * 找到空闲槽。留给我们用。 
			 */
			walsnd->pid = MyProcPid;
			walsnd->state = WALSNDSTATE_STARTUP;
			walsnd->sentPtr = InvalidXLogRecPtr;
			walsnd->needreload = false;
			walsnd->write = InvalidXLogRecPtr;
			walsnd->flush = InvalidXLogRecPtr;
			walsnd->apply = InvalidXLogRecPtr;
			walsnd->writeLag = -1;
			walsnd->flushLag = -1;
			walsnd->applyLag = -1;
			walsnd->sync_standby_priority = 0;
			walsnd->latch = &MyProc->procLatch;
			walsnd->replyTime = 0;

			/*
			 * kind 赋值在此完成，而非在 StartReplication()        
			 * 或 StartLogicalReplication() 中。事实上，逻辑      
			 * walsender 需在槽创建期间读 WAL 记录（   
			 * 运行中事务的快照），所以它需要根据自身的   
			 * kind 被唤醒。    
			 *
			 * kind 的赋值也可以在 StartReplication()、             
			 * StartLogicalReplication() 和 CREATE_REPLICATION_SLOT 中   
			 * 完成。集中一处更好。      
			 */
			if (MyDatabaseId == InvalidOid)
				walsnd->kind = REPLICATION_KIND_PHYSICAL;
			else
				walsnd->kind = REPLICATION_KIND_LOGICAL;

			SpinLockRelease(&walsnd->mutex);
			/* 不再需要该锁          */
			MyWalSnd = (WalSnd *) walsnd;

			break;
		}
	}

	Assert(MyWalSnd != NULL);

	/* 安排在 walsender 退出时清理   */
	on_shmem_exit(WalSndKill, 0);
}

/* 销毁该 walsender 进程的 per-walsender 数据结构            */
static void
WalSndKill(int code, Datum arg)
{
	WalSnd	   *walsnd = MyWalSnd;

	Assert(walsnd != NULL);

	MyWalSnd = NULL;

	SpinLockAcquire(&walsnd->mutex);
	/* 持自旋锁时清除锁存器，使其可被安全读取        */
	walsnd->latch = NULL;
	/* 将 WalSnd 结构标记为不再使用。     */
	walsnd->pid = 0;
	SpinLockRelease(&walsnd->mutex);
}

/* XLogReaderRoutine->segment_open 回调   */
static void
WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
				  TimeLineID *tli_p)
{
	char		path[MAXPGPATH];

	/*-------
	 * 从历史时间线读取，且该段内存在时间线切换时，   
	 * 应从属于新时间线的 WAL 段读取                          
	 * 。      
	 *
	 * 例如，假设本服务器当前位于时间线 5，而我们正在 
	 * 流式发送时间线 4。从时间线 4 切换到 5 发生在         
	 * 0/13002088。pg_wal 中有这些文件：  
	 *
	 * ...
	 * 000000040000000000000012
	 * 000000040000000000000013
	 * 000000050000000000000013
	 * 000000050000000000000014
	 * ...
	 *
	 * 在这种情况下，当被要求发送时间线 4 上段 0x13 的     
	 * WAL 时，我们从文件 000000050000000000000013 读取。           
	 * 归档恢复偏好较新时间线的文件，因此若该段      
	 * 是从归档恢复到本服务器上的，那么属于旧时间线     
	 * 的文件 000000040000000000000013 可能并不存在。               
	 * 两者的内容在切换点之前相等，因为在时间线切换 
	 * 时旧段的已用部分会复制到新文件中。 -------   
	 */
	*tli_p = sendTimeLine;
	if (sendTimeLineIsHistoric)
	{
		XLogSegNo	endSegNo;

		XLByteToSeg(sendTimeLineValidUpto, endSegNo, state->segcxt.ws_segsize);
		if (nextSegNo == endSegNo)
			*tli_p = sendTimeLineNextTLI;
	}

	XLogFilePath(path, *tli_p, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (state->seg.ws_file >= 0)
		return;

	/*
	 * 如果找不到该文件，则假定是备库请求了太旧的      
	 * WAL 段，该段已被移除或回收。                      
	 */
	if (errno == ENOENT)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, *tli_p, nextSegNo, wal_segment_size);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						xlogfname)));
	}
	else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));
}

/*
 * 以正常的物理/存储形式发送 WAL。        
 *
 * 读取最多 MAX_SEND_SIZE 字节已刷写到磁盘、但尚未   
 * 发送给客户端的 WAL，缓冲到 libpq 输出缓冲区      
 * 。    
 *
 * 如果没有剩余未发送的 WAL，则 WalSndCaughtUp 被设     
 * 置为 true，否则被置为 false。   
 */
static void
XLogSendPhysical(void)
{
	XLogRecPtr	SendRqstPtr;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	Size		nbytes;
	XLogSegNo	segno;
	WALReadError errinfo;

	/* 被要求时把 WAL 发送进程切换到停止状态。  */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	if (streamingDoneSending)
	{
		WalSndCaughtUp = true;
		return;
	}

	/* 算出我们能安全发送 WAL 到多远。   */
	if (sendTimeLineIsHistoric)
	{
		/*
		 * 流式发送本服务器历史中的旧时间线，但它并非  
		 * 我们当前写入或重放的那条。可发送到          
		 * 我们离开该时间线的切换点为止。                
		 */
		SendRqstPtr = sendTimeLineValidUpto;
	}
	else if (am_cascading_walsender)
	{
		TimeLineID	SendRqstTLI;

		/*
		 * 在备库上流式发送最新时间线。 
		 *
		 * 尝试发送所有已重放的 WAL 以确认其有效。若我们 
		 * 正通过流复制接收 WAL，则发送已接收但     
		 * 尚未重放的 WAL 也是可以的                             
		 * 。              
		 *
		 * 所恢复的时间线可能改变，或我们被提升。  
		 * 无论哪种，当前时间线都将成为历史时间线。我们 
		 * 需检测到它，以免试图越过切换到另一时间线的  
		 * 点。算出 FlushPtr 后才检查提升或时间线切换，   
		 * 以免竞态条件：若时间线在检查过它仍是     
		 * 当前之后才变为历史，那么把它发送到变为        
		 * 历史之前算出的 FlushPtr 仍然是没问题的              
		 * 。                                                    
		 */
		bool		becameHistoric = false;

		SendRqstPtr = GetStandbyFlushRecPtr(&SendRqstTLI);

		if (!RecoveryInProgress())
		{
			/* 我们已被提升。  */
			SendRqstTLI = GetWALInsertionTimeLine();
			am_cascading_walsender = false;
			becameHistoric = true;
		}
		else
		{
			/*
			 * 仍是级联备库。但我们发送的时间线是不是   
			 * 恢复进程正在恢复的那条？      
			 */
			if (sendTimeLine != SendRqstTLI)
				becameHistoric = true;
		}

		if (becameHistoric)
		{
			/*
			 * 我们发送的时间线已变成历史时间线。读取 
			 * 新时间线的历史文件，以查明我们究竟在何处  
			 * 自先前发送的时间线分叉出来的。   
			 */
			List	   *history;

			history = readTimeLineHistory(SendRqstTLI);
			sendTimeLineValidUpto = tliSwitchPoint(sendTimeLine, history, &sendTimeLineNextTLI);

			Assert(sendTimeLine < sendTimeLineNextTLI);
			list_free_deep(history);

			sendTimeLineIsHistoric = true;

			SendRqstPtr = sendTimeLineValidUpto;
		}
	}
	else
	{
		/*
		 * 在主库上流式发送当前时间线。  
		 *
		 * 尝试发送所有已写出且 fsync 到磁盘的数据。  
		 * 鉴于 WALRead() 目前的实现，我们无法发送更多。     
		 * 况且无论如何，在主库上发送尚未                  
		 * 可靠落盘的 WAL 是不安全的：若主库随后崩溃     
		 * 并重启，备库必须尚未应用任何已在主库             
		 * 不复存在的 WAL。                                     
		 */
		SendRqstPtr = GetFlushRecPtr(NULL);
	}

	/*
	 * 记录当前系统时间，作为此 WAL 位置写入时刻的近似   
	 * 值，用于滞后跟踪。                                    
	 *
	 * 理论上，我们本可以让 XLogFlush() 在 WAL 刷写时           
	 * 在共享内存中记录一个时间，并在调用上面的         
	 * GetFlushRecPtr() 时取回时间和 LSN（级联备库的     
	 * 对应调用也是如此），但与其向热点 WAL 路径添加      
	 * 新代码，在这里采集时间似乎就足够了。我们应当  
	 * 是在 XLogFlush() 执行 WalSndWakeupProcessRequests() 之后         
	 * 到达这里；尽管可能耗时，读取 WAL 刷写指针和     
	 * 取时间应在此几乎同时完成，这样若仍在移动，位置  
	 * 更靠后。 
	 *
	 * 由于 LagTrackerWrite 会忽略 LSN 未前进时的样本，        
	 * 这就为此 LSN 的 WAL 刷写时间提供了一个廉价近似   
	 * 。 
	 *
	 * 注意，该 LSN 未必就是本消息所含数据对应的 LSN；    
	 * 它是 WAL 末端，可能更靠前。滞后跟踪机制只关心   
	 * 的是这个任意 LSN 何时最终被报告为已写入、已刷写  
	 * 和已应用，                                                  
	 * 以便它能测量经过的时间。             
	 */
	LagTrackerWrite(SendRqstPtr, GetCurrentTimestamp());

	/*
	 * 若这是历史时间线，且我们已到达分叉到下一条    
	 * 时间线的点，就停止流式发送。  
	 *
	 * 注意：可能已发送超过 sendTimeLineValidUpto 的 WAL。  
	 * 提升前，startup 进程通常会重放从主库收到的        
	 * 全部 WAL；但如果 WAL 流式发送止于页边界，      
	 * 时间线的有效部分可能结束于一条 WAL 记录中途。   
	 * 我们可能已把那条部分 WAL 记录的前半发送给级联    
	 * 备库，故 sentPtr > sendTimeLineValidUpto。无妨；级联备库   
	 * 也无法重放部分 WAL 记录，仍能跟随我们的               
	 * 时间线切换                                                 
	 * 。             
	 */
	if (sendTimeLineIsHistoric && sendTimeLineValidUpto <= sentPtr)
	{
		/* 关闭当前文件。   */
		if (xlogreader->seg.ws_file >= 0)
			wal_segment_close(xlogreader);

		/* 发送CopyDone*/
		pq_putmessage_noblock('c', NULL, 0);
		streamingDoneSending = true;

		WalSndCaughtUp = true;

		elog(DEBUG1, "walsender reached end of timeline at %X/%X (sent up to %X/%X)",
			 LSN_FORMAT_ARGS(sendTimeLineValidUpto),
			 LSN_FORMAT_ARGS(sentPtr));
		return;
	}

	/* 是否有工作要做？   */
	Assert(sentPtr <= SendRqstPtr);
	if (SendRqstPtr <= sentPtr)
	{
		WalSndCaughtUp = true;
		return;
	}

	/*
	 * 计算单条消息要发送多少。若待发送量不超过       
	 * MAX_SEND_SIZE 字节，就全部发送；否则发送       
	 * MAX_SEND_SIZE 字节，但回退到日志文件或页边界。  
	 *
	 * 取整不仅是为了性能。walreceiver 依赖于这一事实：     
	 * 我们绝不会把一条 WAL 记录拆到两条消息中。由于     
	 * 长的 WAL 记录会在页边界处拆成延续记录，因此页   
	 * 边界始终是安全的截断点。我们还假定              
	 * SendRqstPtr 绝不指向 WAL 记录中间。           
	 */
	startptr = sentPtr;
	endptr = startptr;
	endptr += MAX_SEND_SIZE;

	/* 若越过了 SendRqstPtr，则回退    */
	if (SendRqstPtr <= endptr)
	{
		endptr = SendRqstPtr;
		if (sendTimeLineIsHistoric)
			WalSndCaughtUp = false;
		else
			WalSndCaughtUp = true;
	}
	else
	{
		/* 向下取整到页边界。  */
		endptr -= (endptr % XLOG_BLCKSZ);
		WalSndCaughtUp = false;
	}

	nbytes = endptr - startptr;
	Assert(nbytes <= MAX_SEND_SIZE);

	/*
	 * 可读取并发送切片。   
	 */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'w');

	pq_sendint64(&output_message, startptr);	/* dataStart */
	pq_sendint64(&output_message, SendRqstPtr); /* walEnd */
	pq_sendint64(&output_message, 0);	/* sendtime, filled in last */

	/*
	 * 把日志直接读进输出缓冲区省去多余的 memcpy 调用 
	 * 。   
	 */
	enlargeStringInfo(&output_message, nbytes);

retry:
	if (!WALRead(xlogreader,
				 &output_message.data[output_message.len],
				 startptr,
				 nbytes,
				 xlogreader->seg.ws_tli,	/* Pass the current TLI because
											 * 仅 WalSndSegmentOpen 控制   
											 * whether new TLI is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/* 见 logical_read_xlog_page()  */
	XLByteToSeg(startptr, segno, xlogreader->segcxt.ws_segsize);
	CheckXLogRemoved(segno, xlogreader->seg.ws_tli);

	/*
	 * 在恢复期间，当前打开的 WAL 文件可能会被归档中      
	 * 取回的同名文件替换，因此总要在读入后检查      
	 * 读到的内容是否有效。                                     
	 * 若无效，就重新打开文件再读。      
	 */
	if (am_cascading_walsender)
	{
		WalSnd	   *walsnd = MyWalSnd;
		bool		reload;

		SpinLockAcquire(&walsnd->mutex);
		reload = walsnd->needreload;
		walsnd->needreload = false;
		SpinLockRelease(&walsnd->mutex);

		if (reload && xlogreader->seg.ws_file >= 0)
		{
			wal_segment_close(xlogreader);

			goto retry;
		}
	}

	output_message.len += nbytes;
	output_message.data[output_message.len] = '\0';

	/*
	 * 最后再填充发送时间戳，使其取值尽可能晚。          
	 */
	resetStringInfo(&tmpbuf);
	pq_sendint64(&tmpbuf, GetCurrentTimestamp());
	memcpy(&output_message.data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	pq_putmessage_noblock('d', output_message.data, output_message.len);

	sentPtr = endptr;

	/* 更新共享内存状态    */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* 在 PS 显示中报告 XLOG 流式发送进度  */
	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 LSN_FORMAT_ARGS(sentPtr));
		set_ps_display(activitymsg);
	}
}

/*
 * 流式发送逻辑解码数据。 
 */
static void
XLogSendLogical(void)
{
	XLogRecord *record;
	char	   *errm;

	/*
	 * 我们将用当前刷写位置来判断是否已经跟上。           
	 * 此变量声明为 static，以便跨调用缓存。缓存很有      
	 * 帮助，因为 GetFlushRecPtr() 需要获取竞争激烈的自旋   
	 * 锁。   
	 */
	static XLogRecPtr flushPtr = InvalidXLogRecPtr;

	/*
	 * 尚不知道是否已跟上。若确实在等待，我们会在    
	 * WalSndWaitForWal 中把 WalSndCaughtUp 设为 true。另外，若  
	 * XLogReadRecord() 被迫停止读取而 WalSndWaitForWal          
	 * 未等待也设 true（正在关闭）。   
	 */
	WalSndCaughtUp = false;

	record = XLogReadRecord(logical_decoding_ctx->reader, &errm);

	/* 该 xlog 记录无效   */
	if (errm != NULL)
		elog(ERROR, "could not find record while sending logically-decoded data: %s",
			 errm);

	if (record != NULL)
	{
		/*
		 * 注意这里没有调用 LagTrackerWrite()，该工作由         
		 * WalSndUpdateProgress 完成，由输出插件经逻辑解    
		 * 码写 API 调用。       
		 */
		LogicalDecodingProcessRecord(logical_decoding_ctx, logical_decoding_ctx->reader);

		sentPtr = logical_decoding_ctx->reader->EndRecPtr;
	}

	/*
	 * 若本会话首次执行到这里，则初始化 flushPtr；否则，  
	 * 仅当 EndRecPtr 越过它时才需更新 flushPtr。   
	 */
	if (flushPtr == InvalidXLogRecPtr ||
		logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
	{
		/*
		 * 级联逻辑 WAL 发送进程使用重放 LSN 而非刷写 LSN，  
		 * 因为备库上的逻辑解码只处理已被重放的 WAL。    
		 * 这种区别在关闭时尤其重要：新 WAL 不再重放，而 
		 * 最后一次重放的 LSN 标记着解码能推进到的最远     
		 * 位置                                                             
		 * 。     
		 */
		if (am_cascading_walsender)
			flushPtr = GetXLogReplayRecPtr(NULL);
		else
			flushPtr = GetFlushRecPtr(NULL);
	}

	/* 若 EndRecPtr 仍越过 flushPtr，说明我们已经跟上。  */
	if (logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
		WalSndCaughtUp = true;

	/*
	 * 若已跟上且被要求停止，就让 WalSndLoop() 在写出全部 
	 * 待发数据之后，以有序的方式                             
	 * 断开连接。  
	 */
	if (WalSndCaughtUp && got_STOPPING)
		got_SIGUSR2 = true;

	/* 更新共享内存状态    */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * 发送进程跟上后即关闭。   
 *
 * 注意：只应在从 postmaster 收到关闭信号之后才调用       
 * 本函数。    
 *
 * 注意：若判定还有数据要发送，本函数交还控制     
 * 给调用者。                            
 */
static void
WalSndDone(WalSndSendDataCallback send_data)
{
	XLogRecPtr	replicatedPtr;

	/* ……务必彻底确认我们已经跟上……*/
	send_data();

	/*
	 * 要确认所有 WAL 是否都已成功复制，可检查刷写位置 
	 * 有效则用刷写位置，否则用写位置。pg_receivewal 这类   
	 * 工具通常（同步模式除外）会返回无效的刷写位置。 
	 */
	replicatedPtr = XLogRecPtrIsInvalid(MyWalSnd->flush) ?
		MyWalSnd->write : MyWalSnd->flush;

	if (WalSndCaughtUp && sentPtr == replicatedPtr &&
		!pq_is_send_pending())
	{
		QueryCompletion qc;

		/* 告知备库 XLOG 流式发送已结束        */
		SetQueryCompletion(&qc, CMDTAG_COPY, 0);
		EndCommand(&qc, DestRemote, false);
		pq_flush();

		proc_exit(0);
	}
	if (!waiting_for_ping_response)
		WalSndKeepalive(true, InvalidXLogRecPtr);
}

/*
 * 返回 WAL 中已安全刷写到磁盘、且可以发送给备库的     
 * 最靠后的位置。只应在恢复期间调用本函数，也就是   
 * 即正在向级联备库流式发送。   
 *
 * 副作用：*tli 更新为最后重放的 WAL 记录的 
 * TLI。              
 */
static XLogRecPtr
GetStandbyFlushRecPtr(TimeLineID *tli)
{
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	receivePtr;
	TimeLineID	receiveTLI;
	XLogRecPtr	result;

	/*
	 * 已重放的内容可以安全发送。此外，如果 walreceiver   
	 * 正从同一时间线接收 WAL，则它已流式接收但未重放  
	 * 的内容也都可以发送。             
	 */

	receivePtr = GetWalRcvFlushRecPtr(NULL, &receiveTLI);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);

	if (tli)
		*tli = replayTLI;

	result = replayPtr;
	if (receiveTLI == replayTLI && receivePtr > replayPtr)
		result = receivePtr;

	return result;
}

/*
 * 请求 walsender 重新加载当前打开的 WAL 文件 
 */
void
WalSndRqstFileReload(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		walsnd->needreload = true;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * 处理 PROCSIG_WALSND_INIT_STOPPING 信号 
 */
void
HandleWalSndInitStopping(void)
{
	Assert(am_walsender);

	/*
	 * 若复制尚未开始，则像 SIGTERM 一样直接退出。 
	 * 若复制正在进行，则只设置标志并唤醒主循环。它  
	 * 会发出所有待发 WAL，等它复制到备库后再优雅     
	 * 地退出。                      
	 */
	if (!replication_active)
		kill(MyProcPid, SIGTERM);
	else
		got_STOPPING = true;
}

/*
 * SIGUSR2：设标志做最后一轮循环，之后关闭。            
 * 此时 WAL 发送进程应已切换到 WALSNDSTATE_STOPPING        
 * 状态。  
 */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGUSR2 = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/* 设置信号处理器  */
void
WalSndSignals(void)
{
	/* 设置信号处理器  */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);	/* 查询取消 */
	pqsignal(SIGTERM, die);		/* 请求关闭     */
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置好     */
	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and
												 * shutdown */

	/* 重置一些由 postmaster 接受但此处不接受的信号    */
	pqsignal(SIGCHLD, SIG_DFL);
}

/* 报告 WalSndShmemInit 所需的共享内存空间   */
Size
WalSndShmemSize(void)
{
	Size		size = 0;

	size = offsetof(WalSndCtlData, walsnds);
	size = add_size(size, mul_size(max_wal_senders, sizeof(WalSnd)));

	return size;
}

/* 分配并初始化 walsender 相关的共享内存      */
void
WalSndShmemInit(void)
{
	bool		found;
	int			i;

	WalSndCtl = (WalSndCtlData *)
		ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

	if (!found)
	{
		/* 首次执行，故初始化       */
		MemSet(WalSndCtl, 0, WalSndShmemSize());

		for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
			dlist_init(&(WalSndCtl->SyncRepQueue[i]));

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockInit(&walsnd->mutex);
		}

		ConditionVariableInit(&WalSndCtl->wal_flush_cv);
		ConditionVariableInit(&WalSndCtl->wal_replay_cv);
	}
}

/*
 * 唤醒物理、逻辑或两类 walsender             
 *
 * 区分物理与逻辑 walsender 的原因如下：                       
 * - 物理 walsender 在数据刷写前无法发送            
 * - 备库上的逻辑 walsender 在被应用前无法解码并             
 *   发送 
 *
 * 对于级联复制，我们需要单独唤醒物理 walsender，           
 * 使其区别于逻辑 walsender（详见 ApplyWalRecord() 中调用      
 * WalSndWakeup() 前的注释）。  
 *
 * 它会在临界区内被调用，因此抛出错误并非明智          
 * 之举。 
 */
void
WalSndWakeup(bool physical, bool logical)
{
	/*
	 * 唤醒各等待 WAL 刷写或重放的 walsender。                
	 * 注意，正在等待的 walsender 应已准备在 WalSndWait() 中    
	 * 在 CV 上休眠（即加入 CV 的等待列表），之后才     
	 * 等待。               
	 */
	if (physical)
		ConditionVariableBroadcast(&WalSndCtl->wal_flush_cv);

	if (logical)
		ConditionVariableBroadcast(&WalSndCtl->wal_replay_cv);
}

/*
 * 等待 FeBe 套接字就绪或超时；掩码应由                     
 * WL_SOCKET_WRITEABLE 和 WL_SOCKET_READABLE 标志组成。若 postmaster    
 * 死亡则退出。  
 */
static void
WalSndWait(uint32 socket_events, long timeout, uint32 wait_event)
{
	WaitEvent	event;

	ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, socket_events, NULL);

	/*
	 * 我们用一个条件变量在 WalSndWakeup() 中高效唤醒   
	 * walsender。   
	 *
	 * 每个 walsender 都准备在共享内存 CV 上休眠。注意      
	 * 它只准备在 CV 休眠（即加入 CV 等待列表），    
	 * 并不会真正在 CV 上等待（换言之，它从不调用       
	 * ConditionVariableSleep()）。它仍用 WaitEventSetWait() 等 
	 * 待，因为我们还需要等待套接字事件。想要唤醒       
	 * walsender 的进程（startup 进程、walreceiver 等）会使用   
	 * ConditionVariableBroadcast()，它进而调用 SetLatch()，帮助   
	 * walsender 脱离 WaitEventSetWait()。    
	 *
	 * 此法简单高效：不必带着自旋锁的获取与释放遍历     
	 * 每个槽位，只为唤醒正在等待的 walsender。它让    
	 * WalSndWakeup() 的调用者省心不少                      
	 * 。                                                    
	 *
	 * XXX：一个值得考虑的将来改进是为 WaitEventSetWait()   
	 * 加入 CV 的支持。  
	 *
	 * 另外，物理与逻辑 walsender 各用独立的共享内存 
	 * CV，以便选择性唤醒，详见 WalSndWakeup()。                  
	 */
	if (MyWalSnd->kind == REPLICATION_KIND_PHYSICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_flush_cv);
	else if (MyWalSnd->kind == REPLICATION_KIND_LOGICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_replay_cv);

	if (WaitEventSetWait(FeBeWaitSet, timeout, &event, 1, wait_event) == 1 &&
		(event.events & WL_POSTMASTER_DEATH))
	{
		ConditionVariableCancelSleep();
		proc_exit(1);
	}

	ConditionVariableCancelSleep();
}

/*
 * 让所有 walsender 转入 stopping 状态。   
 *
 * 这会促使 walsender 进入无法再生成 WAL 的状态。              
 * 详见本文件开头的说明。             
 */
void
WalSndInitStopping(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		pid_t		pid;

		SpinLockAcquire(&walsnd->mutex);
		pid = walsnd->pid;
		SpinLockRelease(&walsnd->mutex);

		if (pid == 0)
			continue;

		SendProcSignal(pid, PROCSIG_WALSND_INIT_STOPPING, InvalidBackendId);
	}
}

/*
 * 等待所有 WAL 发送进程退出或到达 stopping 状态。这         
 * 由检查点进程用来控制关闭检查点何时能安全执行     
 * 。                 
 */
void
WalSndWaitStopping(void)
{
	for (;;)
	{
		int			i;
		bool		all_stopped = true;

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockAcquire(&walsnd->mutex);

			if (walsnd->pid == 0)
			{
				SpinLockRelease(&walsnd->mutex);
				continue;
			}

			if (walsnd->state != WALSNDSTATE_STOPPING)
			{
				all_stopped = false;
				SpinLockRelease(&walsnd->mutex);
				break;
			}
			SpinLockRelease(&walsnd->mutex);
		}

		/* 若所有 WAL 发送进程均已确认，即可离开     */
		if (all_stopped)
			return;

		pg_usleep(10000L);		/* 等待 10 毫秒 */
	}
}

/* 设置当前 walsender 的状态（仅 walsender 调用） */
void
WalSndSetState(WalSndState state)
{
	WalSnd	   *walsnd = MyWalSnd;

	Assert(am_walsender);

	if (walsnd->state == state)
		return;

	SpinLockAcquire(&walsnd->mutex);
	walsnd->state = state;
	SpinLockRelease(&walsnd->mutex);
}

/*
 * 返回表示该状态的字符串常量。它用于          
 * 系统视图，绝不应进行翻译。         
 */
static const char *
WalSndGetStateString(WalSndState state)
{
	switch (state)
	{
		case WALSNDSTATE_STARTUP:
			return "startup";
		case WALSNDSTATE_BACKUP:
			return "backup";
		case WALSNDSTATE_CATCHUP:
			return "catchup";
		case WALSNDSTATE_STREAMING:
			return "streaming";
		case WALSNDSTATE_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

static Interval *
offset_to_interval(TimeOffset offset)
{
	Interval   *result = palloc(sizeof(Interval));

	result->month = 0;
	result->day = 0;
	result->time = offset;

	return result;
}

/*
 * 返回各 walsender 的活动，包括 pid 以及发送到备库的      
 * xlog 位置。  
 */
Datum
pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS	12
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * 取得当前活跃的同步备库。数据可能在我们用完前  
	 * 就已过时，但无论如何仍会使用。         
	 */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	sentPtr;
		XLogRecPtr	write;
		XLogRecPtr	flush;
		XLogRecPtr	apply;
		TimeOffset	writeLag;
		TimeOffset	flushLag;
		TimeOffset	applyLag;
		int			priority;
		int			pid;
		WalSndState state;
		TimestampTz replyTime;
		bool		is_sync_standby;
		Datum		values[PG_STAT_GET_WAL_SENDERS_COLS];
		bool		nulls[PG_STAT_GET_WAL_SENDERS_COLS] = {0};
		int			j;

		/* 从共享内存收集数据     */
		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		pid = walsnd->pid;
		sentPtr = walsnd->sentPtr;
		state = walsnd->state;
		write = walsnd->write;
		flush = walsnd->flush;
		apply = walsnd->apply;
		writeLag = walsnd->writeLag;
		flushLag = walsnd->flushLag;
		applyLag = walsnd->applyLag;
		priority = walsnd->sync_standby_priority;
		replyTime = walsnd->replyTime;
		SpinLockRelease(&walsnd->mutex);

		/*
		 * 检测 walsender 现在或过去是否被认定为同步。    
		 * 连同 walsnd_index 一起检查 PID，可防过期数据的  
		 * 影响。               
		 */
		is_sync_standby = false;
		for (j = 0; j < num_standbys; j++)
		{
			if (sync_standbys[j].walsnd_index == i &&
				sync_standbys[j].pid == pid)
			{
				is_sync_standby = true;
				break;
			}
		}

		values[0] = Int32GetDatum(pid);

		if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		{
			/*
			 * 仅超级用户和拥有 pg_read_all_stats 权限的角色    
			 * 才能看到详情。其他用户仅得 pid 值，得知   
			 * 是 walsender，但无详情。  
			 */
			MemSet(&nulls[1], true, PG_STAT_GET_WAL_SENDERS_COLS - 1);
		}
		else
		{
			values[1] = CStringGetTextDatum(WalSndGetStateString(state));

			if (XLogRecPtrIsInvalid(sentPtr))
				nulls[2] = true;
			values[2] = LSNGetDatum(sentPtr);

			if (XLogRecPtrIsInvalid(write))
				nulls[3] = true;
			values[3] = LSNGetDatum(write);

			if (XLogRecPtrIsInvalid(flush))
				nulls[4] = true;
			values[4] = LSNGetDatum(flush);

			if (XLogRecPtrIsInvalid(apply))
				nulls[5] = true;
			values[5] = LSNGetDatum(apply);

			/*
			 * 把 pg_basebackup 后台进程这类备库，因其总是 
			 * 返回无效刷写位置，故视为异步备库     
			 * 。                  
			 */
			priority = XLogRecPtrIsInvalid(flush) ? 0 : priority;

			if (writeLag < 0)
				nulls[6] = true;
			else
				values[6] = IntervalPGetDatum(offset_to_interval(writeLag));

			if (flushLag < 0)
				nulls[7] = true;
			else
				values[7] = IntervalPGetDatum(offset_to_interval(flushLag));

			if (applyLag < 0)
				nulls[8] = true;
			else
				values[8] = IntervalPGetDatum(offset_to_interval(applyLag));

			values[9] = Int32GetDatum(priority);

			/*
			 * 备库状态的更易理解版本，纯粹                     
			 * 供参考。  
			 *
			 * 在仲裁式同步复制中，synchronous_standby_names   
			 * 所列的每个备库角色都可能频繁变化。     
			 * 某一刻被视为 "sync" 的备库，下一刻可能变成    
			 * "potential"。所以把它们当作同步状态报告       
			 * "sync" 或 "potential" 基本没有意义。对它们只报    
			 * 告 "quorum"。                          
			 */
			if (priority == 0)
				values[10] = CStringGetTextDatum("async");
			else if (is_sync_standby)
				values[10] = SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY ?
					CStringGetTextDatum("sync") : CStringGetTextDatum("quorum");
			else
				values[10] = CStringGetTextDatum("potential");

			if (replyTime == 0)
				nulls[11] = true;
			else
				values[11] = TimestampTzGetDatum(replyTime);
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * 向备库发送 keepalive 消息。 
 *
 * 若设置了 requestReply，该消息会请求对方回发一条    
 * 消息回来用于心跳。还会设置标志，让附近的代码  
 * 知道我们正等待该响应，以免一再重复                
 * 发起请求。   
 *
 * writePtr 是 WAL 已发送到的位置。它本质上与 sentPtr       
 * 相同，但某些情况下我们需要在 sentPtr 更新之前发      
 * 送 keepalive，比如跳过空事务时。              
 */
static void
WalSndKeepalive(bool requestReply, XLogRecPtr writePtr)
{
	elog(DEBUG2, "sending replication keepalive");

	/* 构造该消息...       */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'k');
	pq_sendint64(&output_message, XLogRecPtrIsInvalid(writePtr) ? sentPtr : writePtr);
	pq_sendint64(&output_message, GetCurrentTimestamp());
	pq_sendbyte(&output_message, requestReply ? 1 : 0);

	/* ……再以 CopyData 包裹发送  */
	pq_putmessage_noblock('d', output_message.data, output_message.len);

	/* 设本地标志*/
	if (requestReply)
		waiting_for_ping_response = true;
}

/*
 * 若已耗时过长，则发送 keepalive 消息。  
 */
static void
WalSndKeepaliveIfNecessary(void)
{
	TimestampTz ping_time;

	/*
	 * 若超时被全局禁用，或所做的事不参与超时，就   
	 * 不发送 keepalive 消息。                   
	 */
	if (wal_sender_timeout <= 0 || last_reply_timestamp <= 0)
		return;

	if (waiting_for_ping_response)
		return;

	/*
	 * 如果 wal_sender_timeout 已过半而未收到任何来自备库   
	 * 的回复，则向备库发一条 keep-alive 消息，请求立即   
	 * 回复。          
	 */
	ping_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
											wal_sender_timeout / 2);
	if (last_processing >= ping_time)
	{
		WalSndKeepalive(true, InvalidXLogRecPtr);

		/* 尝试把待发输出刷写给客户端   */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}
}

/*
 * 记录 WAL 末端及其在本地被刷写的时间，以便           
 * LagTrackerRead 能在该 WAL 位置最终被备库在回复消息          
 * 中报告为已写入、已刷写并已应用时算出经过的     
 * 时间（滞后）。      
 */
static void
LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time)
{
	int			new_write_head;
	int			i;

	if (!am_walsender)
		return;

	/*
	 * 若 lsn 未比上次前进，则什么都不做。这样              
	 * 只有写入了新 WAL 才会记录新样本。           
	 */
	if (lag_tracker->last_lsn == lsn)
		return;
	lag_tracker->last_lsn = lsn;

	/*
	 * 若向前推进循环缓冲区的写头会撞上任何一个读头，  
	 * 那么缓冲区已满。换言之，                            
	 * 是最慢的读者（多半是 apply）控制着空间的             
	 * 释放   
	 */
	new_write_head = (lag_tracker->write_head + 1) % LAG_TRACKER_BUFFER_SIZE;
	for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; ++i)
	{
		/*
		 * 若缓冲区已满，则把最慢的读者移至单独的   
		 * 溢出条目并腾出缓冲区中的空间，让写头能        
		 * 推进。   
		 */
		if (new_write_head == lag_tracker->read_heads[i])
		{
			lag_tracker->overflowed[i] =
				lag_tracker->buffer[lag_tracker->read_heads[i]];
			lag_tracker->read_heads[i] = -1;
		}
	}

	/* 在当前写头位置存入一个样本。         */
	lag_tracker->buffer[lag_tracker->write_head].lsn = lsn;
	lag_tracker->buffer[lag_tracker->write_head].time = local_flush_time;
	lag_tracker->write_head = new_write_head;
}

/*
 * 计算 WAL 位置 'lsn'（或已知最高更早 LSN）本地刷写      
 * 的时刻与 'now' 之间经过了多久。备库在回复中报告       
 * 的每个已报告 LSN 位置都有一个独立的读头；'head'     
 * 决定使用哪一个读头。每当某个读头越过一条经    
 * LagTrackerWrite 记入滞后缓冲的 LSN，就能用其关联的    
 * 时间戳查明该 LSN（或更早的）本地被刷写的时间，并 
 * 据此计算出滞后。如果没有新的样本数据可用，就返  
 * 回 -1，否则返回     
 *
 * 以微秒为单位                                                     
 * 的经过时间。   
 */
static TimeOffset
LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now)
{
	TimestampTz time = 0;

	/*
	 * 若 'lsn' 还未越过溢出条目中存储的 WAL 位置，则返回 
	 * 自所存本地刷写时间起经过的时间（微秒）。若      
	 * 刷写时间在未来（因时钟漂移），就返回 -1，视作   
	 * 无有效样本。             
	 *
	 * 否则，改回用缓冲区来控制读头并计算经过的时间。  
	 * 之后读头会被重置为指向缓冲区内                         
	 * 最旧的条目。         
	 */
	if (lag_tracker->read_heads[head] == -1)
	{
		if (lag_tracker->overflowed[head].lsn > lsn)
			return (now >= lag_tracker->overflowed[head].time) ?
				now - lag_tracker->overflowed[head].time : -1;

		time = lag_tracker->overflowed[head].time;
		lag_tracker->last_read[head] = lag_tracker->overflowed[head];
		lag_tracker->read_heads[head] =
			(lag_tracker->write_head + 1) % LAG_TRACKER_BUFFER_SIZE;
	}

	/* 读到此 LSN 或缓冲区末端的所有未读样本。 */
	while (lag_tracker->read_heads[head] != lag_tracker->write_head &&
		   lag_tracker->buffer[lag_tracker->read_heads[head]].lsn <= lsn)
	{
		time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		lag_tracker->last_read[head] =
			lag_tracker->buffer[lag_tracker->read_heads[head]];
		lag_tracker->read_heads[head] =
			(lag_tracker->read_heads[head] + 1) % LAG_TRACKER_BUFFER_SIZE;
	}

	/*
	 * 若滞后跟踪器为空，说明备库已处理完我们发     
	 * 的一切内容，因此现在应清除 'last_read'。否则会      
	 * 有风险：在空闲期后的下一波 WAL 突发开始时，   
	 * 用陈旧且不相关的样本来做插值                            
	 * 。         
	 */
	if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		lag_tracker->last_read[head].time = 0;

	if (time > now)
	{
		/* 如果时钟不知为何倒退了，则视为未找到。*/
		return -1;
	}
	else if (time == 0)
	{
		/*
		 * 未跨过时间点。若存在一个我们尚未达到的   
		 * 未来样本，而且我们已经到达了至少一个样本，    
		 * 那就对本地刷写时间做插值。这主要用于          
		 * 把完全卡住的 apply 位置报告成滞后增长，  
		 * 否则就得等它最终重新移动并越过我们的某  
		 * 个样本，之后                                               
		 * 才能显示滞后在增加。 
		 */
		if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		{
			/* 没有未来样本，因此无法进行插值。      */
			return -1;
		}
		else if (lag_tracker->last_read[head].time != 0)
		{
			/* 可在 last_read 与下一个样本之间插值。        */
			double		fraction;
			WalTimeSample prev = lag_tracker->last_read[head];
			WalTimeSample next = lag_tracker->buffer[lag_tracker->read_heads[head]];

			if (lsn < prev.lsn)
			{
				/*
				 * 报告的 LSN 通常不应后退，但发生时间线  
				 * 切换时则可能。此时视为未找到             
				 * 。   
				 */
				return -1;
			}

			Assert(prev.lsn < next.lsn);

			if (prev.time > next.time)
			{
				/* 如果时钟不知为何倒退了，则视为未找到。*/
				return -1;
			}

			/* 查看我们在前后两个样本之间的位置。       */
			fraction =
				(double) (lsn - prev.lsn) / (double) (next.lsn - prev.lsn);

			/* 按比例缩放本地刷写时间。       */
			time = (TimestampTz)
				((double) prev.time + (next.time - prev.time) * fraction);
		}
		else
		{
			/*
			 * 只剩一个未来样本，说明我们此前完全         
			 * 跟上，又来了一波 WAL，备库还没处理第一  
			 * 个样本。备库到达它之前，能做的最好是   
			 * 报告该样本若当下被重放将有的假设滞后        
			 * 。                                                         
			 */
			time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		}
	}

	/* 返回自本地刷写以来经过的时间（微秒）。       */
	Assert(time != 0);
	return now - time;
}
