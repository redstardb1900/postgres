/*-------------------------------------------------------------------------
 *
 * xlogrecovery.c
 *		WAL 恢复与备库模式相关函数
 *
 * 本源文件包含控制 WAL 恢复的函数。
 * InitWalRecovery() 按配置选项以及控制文件和可选
 * 备份标签文件的状态，为崩溃恢复、归档恢复或备库
 * 模式初始化系统。PerformWalRecovery()
 * 执行实际的 WAL 重放，并调用各 rmgr 专用的 redo 例程。
 * FinishWalRecovery() 执行恢复结束检查与清理动作，
 * 并准备将 WAL 初始化为可写所需的信息。除这
 * 三个主函数外，还有一组用于查询恢复状态与
 * 控制恢复过程的函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogrecovery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogprefetcher.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "catalog/pg_control.h"
#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/guc_hooks.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/pg_rusage.h"

/* 不支持的旧恢复命令文件名（相对于 $PGDATA） */
#define RECOVERY_COMMAND_FILE	"recovery.conf"
#define RECOVERY_COMMAND_DONE	"recovery.done"

/*
 * GUC 支持
 */
const struct config_enum_entry recovery_target_action_options[] = {
	{"pause", RECOVERY_TARGET_ACTION_PAUSE, false},
	{"promote", RECOVERY_TARGET_ACTION_PROMOTE, false},
	{"shutdown", RECOVERY_TARGET_ACTION_SHUTDOWN, false},
	{NULL, 0, false}
};

/* 选项以前从 recovery.conf 获取，用于归档恢复 */
char	   *recoveryRestoreCommand = NULL;
char	   *recoveryEndCommand = NULL;
char	   *archiveCleanupCommand = NULL;
RecoveryTargetType recoveryTarget = RECOVERY_TARGET_UNSET;
bool		recoveryTargetInclusive = true;
int			recoveryTargetAction = RECOVERY_TARGET_ACTION_PAUSE;
TransactionId recoveryTargetXid;
char	   *recovery_target_time_string;
TimestampTz recoveryTargetTime;
const char *recoveryTargetName;
XLogRecPtr	recoveryTargetLSN;
int			recovery_min_apply_delay = 0;

/* 选项以前从 recovery.conf 获取，用于 XLOG 流传输 */
char	   *PrimaryConnInfo = NULL;
char	   *PrimarySlotName = NULL;
bool		wal_receiver_create_temp_slot = false;

/*
 * recoveryTargetTimeLineGoal: 用户请求的目标时间线（如果有）
 *
 * recoveryTargetTLIRequested: 请求的时间线的数值，如果是常量
 *
 * recoveryTargetTLI: 当前理解的目标时间线；会发生变化
 *
 * expectedTLEs:
 *  recoveryTargetTLI 及其已知父级时间线的 TimeLineHistoryEntries 列表，
 * 最新的在前（所以 recoveryTargetTLI 始终是列表的第一个成员）。
 * 只有这些 TLIs 期望在我们读取的 WAL 段中出现，
 * 实际上也只考虑打开这些 TLIs 的 WAL 文件。
 *
 * curFileTLI:
 *  当前输入 WAL 文件名称中显示的时间线。
 * (这不一定与我们正在重放WAL 的时间线相同，StartupXLOG 称之为 replayTLI，
 * 因为当我们创建当前文件时，
 * 可能会扫描从祖先时间线复制的数据。
 * ) 在顺序扫描期间，我们不允许此值减少。
 */
RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal = RECOVERY_TARGET_TIMELINE_LATEST;
TimeLineID	recoveryTargetTLIRequested = 0;
TimeLineID	recoveryTargetTLI = 0;
static List *expectedTLEs;
static TimeLineID curFileTLI;

/*
 * 当 ArchiveRecoveryRequested 设置时，
 * 请求了归档恢复，即存在信号文件。
 * 当 InArchiveRecovery 设置时，
 * 我们正在使用离线 XLOG 归档进行恢复。这些变量仅在启动进程中有效。
 *
 * 当 ArchiveRecoveryRequested 为真但 InArchiveRecovery 为假时，
 * 我们当前正在进行崩溃恢复，
 * 仅使用 pg_wal 中的 XLOG 文件，
 * 但在到达 pg_wal 中 WAL 的末尾时将切换到使用离线 XLOG 归档。
*/
bool		ArchiveRecoveryRequested = false;
bool		InArchiveRecovery = false;

/*
 * 当 StandbyModeRequested 设置时，请求了备库模式，即
 * standby.signal 文件存在。当 StandbyMode 设置时，我们当前
 * 处于备库模式。这些变量仅在启动进程中有效。
 * 它们的工作方式类似于 ArchiveRecoveryRequested 和 InArchiveRecovery。
 */
static bool StandbyModeRequested = false;
bool		StandbyMode = false;

/* 启动时是否存在信号文件？ */
static bool standby_signal_file_found = false;
static bool recovery_signal_file_found = false;

/*
 * CheckPointLoc 
 * 是确定从何处开始重放的检查点记录的位置。
 * 它来自备份标签文件或控制文件。
 *
 * RedoStartLSN 是检查点的 REDO 位置，
 * 同样来自备份标签文件或控制文件。
 * 在备库模式下，
 * XLOG 流传输通常从发现无效记录的位置开始。但如果我们连初始检查点记录都无法读取，
 * 我们会使用 REDO 位置而不是检查点位置作为 XLOG 流传输的起始位置。
 * 否则，
 * 在读取检查点记录后，
 * 我们必须向后跳转到 REDO 位置，因为 REDO 记录可以在检查点记录之前。
 */
static XLogRecPtr CheckPointLoc = InvalidXLogRecPtr;
static TimeLineID CheckPointTLI = 0;
static XLogRecPtr RedoStartLSN = InvalidXLogRecPtr;
static TimeLineID RedoStartTLI = 0;

/*
 * SharedHotStandbyActive 变量的本地副本。值为假实际上意味着"未知，
 * 需要检查共享状态"。
 */
static bool LocalHotStandbyActive = false;

/*
 * SharedPromoteIsTriggered 变量的本地副本。值为假实际上意味着"未知，
 * 需要检查共享状态"。
 */
static bool LocalPromoteIsTriggered = false;

/* 恢复代码是否请求了 walreceiver 唤醒？ */
static bool doRequestWalReceiverReply;

/* 用于解析 WAL 记录的 XLogReader 对象 */
static XLogReaderState *xlogreader = NULL;

/* 用于使用预读消耗 WAL 记录的 XLogPrefetcher 对象 */
static XLogPrefetcher *xlogprefetcher = NULL;

/* 从 ReadRecord 传递给 XLogPageRead 回调的参数。 */
typedef struct XLogPageReadPrivate
{
	int			emode;
	bool		fetching_ckpt;	/* 我们是否正在获取检查点记录？ */
	bool		randAccess;
	TimeLineID	replayTLI;
} XLogPageReadPrivate;

/* 用于告诉 XLogPageRead 我们已经开始重放的标志 */
static bool InRedo = false;

/*
 * 表示 WAL 记录来源的代码（在恢复期间），
 * 或尝试从何处获取 WAL。
 */
typedef enum
{
	XLOG_FROM_ANY = 0,			/* 请求从任何源读取 WAL */
	XLOG_FROM_ARCHIVE,			/* 使用 restore_command 恢复 */
	XLOG_FROM_PG_WAL,			/* pg_wal 中的现有文件 */
	XLOG_FROM_STREAM			/* 从主库流式传输 */
} XLogSource;

/* 用于调试输出的 XLogSources 的人类可读名称 */
static const char *const xlogSourceNames[] = {"any", "archive", "pg_wal", "stream"};

/*
 * readFile 是 -1 或当前打开用于读取的日志文件段的内核文件描述符。
 * readSegNo 标识该段。
 * readOff 是刚读取页面的偏移量，
 * readLen 指示已读入 readBuf 的数据量，readSource 指示当前打开文件的来源。
 *
 * 注意：我们可以使用 Reserve/ReleaseExternalFD 来跟踪此文件描述符的消耗
 * （就像 xlog.c 中的 openLogFile 那样）；但目前看来不值得这样做，
 * 因为 XLOG 不被一般用途的会话读取。
 */
static int	readFile = -1;
static XLogSegNo readSegNo = 0;
static uint32 readOff = 0;
static uint32 readLen = 0;
static XLogSource readSource = XLOG_FROM_ANY;

/*
 * 跟踪我们当前正在从哪个源读取。
 * 这与 readSource 不同，
 * 因为它始终被设置，即使我们当前没有打开的 WAL 文件。
 * 如果 lastSourceFailed 被设置，
 * 说明我们上次从 currentSource 读取的尝试失败了，下次应该尝试其他源。
 *
 * 当发生需要重启 walreceiver 的配置更改时，设置 pendingWalRcvRestart。
 * 这仅在 XLOG_FROM_STREAM 状态下有效。
 */
static XLogSource currentSource = XLOG_FROM_ANY;
static bool lastSourceFailed = false;
static bool pendingWalRcvRestart = false;

/*
 * 这些变量跟踪我们上次获取 WAL 数据进行处理的时间，
 * 以及数据的来源。（XLogReceiptSource 最初与 readSource 相同，
 * 但当我们当前没有数据需要处理时，readSource 会被重置为零。
 * 它也与 currentSource 不同，当我们尝试从某个源读取并失败时，
 * currentSource 也会改变，而 XLogReceiptSource 跟踪我们上次成功
 * 读取一些 WAL 的位置。）
 */
static TimestampTz XLogReceiptTime = 0;
static XLogSource XLogReceiptSource = XLOG_FROM_ANY;

/* WalRcv->flushedUpto 的本地副本 */
static XLogRecPtr flushedUpto = 0;
static TimeLineID receiveTLI = 0;

/*
 * 控制文件中 minRecoveryPoint 和 backupEndPoint 的副本。
 *
 * 为了达到一致性，
 * 我们必须将 WAL 重放到 minRecoveryPoint。如果 backupEndRequired 为真，
 * 我们还必须到达 backupEndPoint，
 * 或者如果它无效，则到达与 backupStartPoint 对应的备份结束记录。
 *
 * 注意：
 * 在归档恢复中，达到一致性后，
 * xlog.c 中的函数会开始更新控制文件中的 minRecoveryPoint。
 * 但此 minRecoveryPoint 变量副本反映的是恢复开始时的值，在达到一致性后*不会*更新。
 */
static XLogRecPtr minRecoveryPoint;
static TimeLineID minRecoveryPointTLI;

static XLogRecPtr backupStartPoint;
static XLogRecPtr backupEndPoint;
static bool backupEndRequired = false;

/*
 * 我们是否已经达到一致的数据库状态？在崩溃恢复中，我们必须重放所有 WAL，
 * 所以 reachedConsistency 永远不会被设置。在归档恢复期间，一旦到达
 * minRecoveryPoint，数据库就达到一致状态。
 *
 * 一致状态意味着系统内部一致，
 * 所有 WAL 已重放到某个点，
 * 重要的是，磁盘上没有后续操作的痕迹。
 */
bool		reachedConsistency = false;

/* 专用于大小为 BLCKSZ 的一致性检查的缓冲区 */
static char *replay_image_masked = NULL;
static char *primary_image_masked = NULL;


/*
 * WAL 恢复的共享内存状态。
 */
typedef struct XLogRecoveryCtlData
{
	/*
	 * SharedHotStandbyActive 指示我们是否允许运行热备查询。
	 * 由 info_lck 保护。
	 */
	bool		SharedHotStandbyActive;

	/*
	 * SharedPromoteIsTriggered 指示备库提升是否已被触发。
	 * 由 info_lck 保护。
	 */
	bool		SharedPromoteIsTriggered;

	/*
	 * recoveryWakeupLatch
	 * 用于唤醒启动进程继续 WAL 重放，
	 * 如果它正在等待 WAL 到达或提升被请求。
	 *
	 * 注意，
	 * 启动进程还使用另一个锁存器，
	 * 即其 procLatch，
	 * 用于等待恢复冲突。
	 * 如果我们取消 recoveryWakeupLatch 而使用procLatch 来通知启动进程，
	 * 这会与使用该锁存器的可能的通用信号处理程序更加一致。
	 * 但我们不应该这样做，
	 * 因为启动进程不假设在等待恢复冲突时会被 walreceiver 进程或 SIGHUP 信号处理程序唤醒。
	 * 应该分别使用独立的锁存器 recoveryWakeupLatch 和 procLatch进行 WAL 重放和恢复冲突的进程间通信。
	 */
	Latch		recoveryWakeupLatch;

	/*
	 * 最后成功重放的记录。
	 */
	XLogRecPtr	lastReplayedReadRecPtr; /* 开始位置 */
	XLogRecPtr	lastReplayedEndRecPtr;	/* 结束+1 位置 */
	TimeLineID	lastReplayedTLI;	/* 时间线 */

	/*
	 * 当我们当前正在重放记录时（即在 redo 函数中），
	 * replayEndRecPtr 指向正在重放记录的结束+1 位置，
	 * 否则它等于 lastReplayedEndRecPtr。
	 */
	XLogRecPtr	replayEndRecPtr;
	TimeLineID	replayEndTLI;
	/* 最后重放的 COMMIT/ABORT 记录的时间戳（或正在重放的记录） */
	TimestampTz recoveryLastXTime;

	/*
	 * 开始重放当前 WAL 数据块的时间戳，
	 * 仅与复制或归档恢复相关
	 */
	TimestampTz currentChunkStartTime;
	/* 恢复暂停状态 */
	RecoveryPauseState recoveryPauseState;
	ConditionVariable recoveryNotPausedCV;

	slock_t		info_lck;		/* 锁定上面显示的共享变量 */
} XLogRecoveryCtlData;

static XLogRecoveryCtlData *XLogRecoveryCtl = NULL;

/*
 * abortedRecPtr 是 WAL 结束处损坏记录的起始指针，
 * 在恢复完成时设置；
 * missingContrecPtr 是第一个丢失的 contrecord 的位置。
 * 详见 CreateOverwriteContrecordRecord。
 */
static XLogRecPtr abortedRecPtr;
static XLogRecPtr missingContrecPtr;

/*
 * 如果 recoveryStopsBefore/After 返回 true，
 * 它会在此处保存停止点的信息
 */
static TransactionId recoveryStopXid;
static TimestampTz recoveryStopTime;
static XLogRecPtr recoveryStopLSN;
static char recoveryStopName[MAXFNAMELEN];
static bool recoveryStopAfter;

/* 本地函数的原型 */
static void ApplyWalRecord(XLogReaderState *xlogreader, XLogRecord *record, TimeLineID *replayTLI);

static void EnableStandbyMode(void);
static void readRecoverySignalFile(void);
static void validateRecoveryParameters(void);
static bool read_backup_label(XLogRecPtr *checkPointLoc,
							  TimeLineID *backupLabelTLI,
							  bool *backupEndRequired, bool *backupFromStandby);
static bool read_tablespace_map(List **tablespaces);

static void xlogrecovery_redo(XLogReaderState *record, TimeLineID replayTLI);
static void CheckRecoveryConsistency(void);
static void rm_redo_error_callback(void *arg);
#ifdef WAL_DEBUG
static void xlog_outrec(StringInfo buf, XLogReaderState *record);
#endif
static void xlog_block_info(StringInfo buf, XLogReaderState *record);
static void checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI,
								TimeLineID prevTLI, TimeLineID replayTLI);
static bool getRecordTimestamp(XLogReaderState *record, TimestampTz *recordXtime);
static void verifyBackupPageConsistency(XLogReaderState *record);

static bool recoveryStopsBefore(XLogReaderState *record);
static bool recoveryStopsAfter(XLogReaderState *record);
static char *getRecoveryStopReason(void);
static void recoveryPausesHere(bool endOfRecovery);
static bool recoveryApplyDelay(XLogReaderState *record);
static void ConfirmRecoveryPaused(void);

static XLogRecord *ReadRecord(XLogPrefetcher *xlogprefetcher,
							  int emode, bool fetching_ckpt,
							  TimeLineID replayTLI);

static int	XLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
						 int reqLen, XLogRecPtr targetRecPtr, char *readBuf);
static XLogPageReadResult WaitForWALToBecomeAvailable(XLogRecPtr RecPtr,
													  bool randAccess,
													  bool fetching_ckpt,
													  XLogRecPtr tliRecPtr,
													  TimeLineID replayTLI,
													  XLogRecPtr replayLSN,
													  bool nonblocking);
static int	emode_for_corrupt_record(int emode, XLogRecPtr RecPtr);
static XLogRecord *ReadCheckpointRecord(XLogPrefetcher *xlogprefetcher,
										XLogRecPtr RecPtr, TimeLineID replayTLI);
static bool rescanLatestTimeLine(TimeLineID replayTLI, XLogRecPtr replayLSN);
static int	XLogFileRead(XLogSegNo segno, int emode, TimeLineID tli,
						 XLogSource source, bool notfoundOk);
static int	XLogFileReadAnyTLI(XLogSegNo segno, int emode, XLogSource source);

static bool CheckForStandbyTrigger(void);
static void SetPromoteIsTriggered(void);
static bool HotStandbyActiveInReplay(void);

static void SetCurrentChunkStartTime(TimestampTz xtime);
static void SetLatestXTime(TimestampTz xtime);

/*
 * WAL 恢复共享内存的初始化
 */
Size
XLogRecoveryShmemSize(void)
{
	Size		size;

	/* XLogRecoveryCtl */
	size = sizeof(XLogRecoveryCtlData);

	return size;
}

void
XLogRecoveryShmemInit(void)
{
	bool		found;

	XLogRecoveryCtl = (XLogRecoveryCtlData *)
		ShmemInitStruct("XLOG Recovery Ctl", XLogRecoveryShmemSize(), &found);
	if (found)
		return;
	memset(XLogRecoveryCtl, 0, sizeof(XLogRecoveryCtlData));

	SpinLockInit(&XLogRecoveryCtl->info_lck);
	InitSharedLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
	ConditionVariableInit(&XLogRecoveryCtl->recoveryNotPausedCV);
}

/*
 * 一个简单的包装器，
 * 用于启用 StandbyMode 并根据需要执行其他准备工作。
 */
static void
EnableStandbyMode(void)
{
	StandbyMode = true;

	/*
	 * 为避免服务器日志膨胀，我们在备库中不报告恢复进度，
	 * 因为除非被提升，否则它将始终处于恢复状态。我们在备库模式下
	 * 禁用启动进度超时，以避免不必要地调用
	 * startup_progress_timeout_handler()。
	 */
	disable_startup_progress_timeout();
}

/*
 * 如有需要，为 WAL 恢复准备系统。
 *
 * 此函数由协调服务器启动序列的 StartupXLOG()
 * 调用。
 * 此函数分析控制文件和备份标签文件（如果有），
 * 确定我们需要执行崩溃恢复还是归档恢复，
 * 以及需要重放 WAL 到多远才能达到一致状态。
 *
 * 此函数尚未更改磁盘状态，
 * 除了根据表空间映射文件创建符号链接以及获取查找检查点记录所需的 WAL 文件。
 * 进入时，
 * 调用者已将控制文件读入内存，
 * 并将其作为参数传递。
 * 此函数更新它以反映恢复状态，
 * 调用者预期在初始化其他子系统后、调用 PerformWalRecovery() 之前将其写回磁盘。
 *
 * 此函数初始化一些全局变量，如 ArchiveRecoveryRequested、
 * StandbyModeRequested 和 InRecovery。
 */
void
InitWalRecovery(ControlFileData *ControlFile, bool *wasShutdown_ptr,
				bool *haveBackupLabel_ptr, bool *haveTblspcMap_ptr)
{
	XLogPageReadPrivate *private;
	struct stat st;
	bool		wasShutdown;
	XLogRecord *record;
	DBState		dbstate_at_startup;
	bool		haveTblspcMap = false;
	bool		haveBackupLabel = false;
	CheckPoint	checkPoint;
	bool		backupFromStandby = false;

	dbstate_at_startup = ControlFile->state;

	/*
	 * 初始化时假设我们要恢复到
	 * pg_control 中显示为活动的最新时间线。
	 */
	if (ControlFile->minRecoveryPointTLI >
		ControlFile->checkPointCopy.ThisTimeLineID)
		recoveryTargetTLI = ControlFile->minRecoveryPointTLI;
	else
		recoveryTargetTLI = ControlFile->checkPointCopy.ThisTimeLineID;

	/*
	 * 检查信号文件，如果存在则设置离线恢复状态
	 */
	readRecoverySignalFile();
	validateRecoveryParameters();

	if (ArchiveRecoveryRequested)
	{
		if (StandbyModeRequested)
			ereport(LOG,
					(errmsg("entering standby mode")));
		else if (recoveryTarget == RECOVERY_TARGET_XID)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to XID %u",
							recoveryTargetXid)));
		else if (recoveryTarget == RECOVERY_TARGET_TIME)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to %s",
							timestamptz_to_str(recoveryTargetTime))));
		else if (recoveryTarget == RECOVERY_TARGET_NAME)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to \"%s\"",
							recoveryTargetName)));
		else if (recoveryTarget == RECOVERY_TARGET_LSN)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to WAL location (LSN) \"%X/%X\"",
							LSN_FORMAT_ARGS(recoveryTargetLSN))));
		else if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE)
			ereport(LOG,
					(errmsg("starting point-in-time recovery to earliest consistent point")));
		else
			ereport(LOG,
					(errmsg("starting archive recovery")));
	}

	/*
	 * 如果我们将在恢复期间休眠，
	 * 则获取唤醒锁存器的所有权。
	 */
	if (ArchiveRecoveryRequested)
		OwnLatch(&XLogRecoveryCtl->recoveryWakeupLatch);

	private = palloc0(sizeof(XLogPageReadPrivate));
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.page_read = &XLogPageRead,
									  .segment_open = NULL,
									  .segment_close = wal_segment_close),
						   private);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	xlogreader->system_identifier = ControlFile->system_identifier;

	/*
	 * 设置 WAL 解码缓冲区大小。
	 * 这限制了我们在 WAL 中可以预读多远。
	 */
	XLogReaderSetDecodeBuffer(xlogreader, NULL, wal_decode_buffer_size);

	/* 创建 WAL 预取器。 */
	xlogprefetcher = XLogPrefetcherAllocate(xlogreader);

	/*
	 * 分配两个专用于 WAL 一致性检查的页面缓冲区。我们这样做，
	 * 而不是直接创建静态数组，有两个原因：
	 * (1) 在大多数后端实例中不需要浪费存储空间；
	 * (2) 静态字符数组不能保证有任何特定的对齐方式，
	 *     而 palloc() 会提供 MAXALIGN 对齐的存储。
	 */
	replay_image_masked = (char *) palloc(BLCKSZ);
	primary_image_masked = (char *) palloc(BLCKSZ);

	if (read_backup_label(&CheckPointLoc, &CheckPointTLI, &backupEndRequired,
						  &backupFromStandby))
	{
		List	   *tablespaces = NIL;

		/*
		 * 请求了归档恢复，
		 * 多亏了备份标签文件，
		 * 我们知道需要重放到哪里才能达到一致性。直接进入归档恢复。
		 */
		InArchiveRecovery = true;
		if (StandbyModeRequested)
			EnableStandbyMode();

		/*
		 * 在创建新副本、PITR 节点等时省略 backup_label
		 * 不幸的是导致损坏的常见原因。
		 * 记录使用了 backup_label 
		 * 使排除这作为观察到的损坏原因稍微容易一些。
		 *
		 * 在我们尝试读取检查点记录（可能失败）之前这样做，
		 * 否则很难理解为什么使用 ControlFile->checkPoint 以外的
		 * 检查点。
		 */
		ereport(LOG,
				(errmsg("starting backup recovery with redo LSN %X/%X, checkpoint LSN %X/%X, on timeline ID %u",
						LSN_FORMAT_ARGS(RedoStartLSN),
						LSN_FORMAT_ARGS(CheckPointLoc),
						CheckPointTLI)));

		/*
		 * 当存在 backup_label 文件时，我们要根据它标识的检查点向前滚动，
		 * 而不是使用 pg_control。
		 */
		record = ReadCheckpointRecord(xlogprefetcher, CheckPointLoc,
									  CheckPointTLI);
		if (record != NULL)
		{
			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			wasShutdown = ((record->xl_info & ~XLR_INFO_MASK) == XLOG_CHECKPOINT_SHUTDOWN);
			ereport(DEBUG1,
					(errmsg_internal("checkpoint record is at %X/%X",
									 LSN_FORMAT_ARGS(CheckPointLoc))));
			InRecovery = true;	/* 即使处于 SHUTDOWNED 也强制恢复 */

			/*
			 * 确保 REDO 位置存在。
			 * 如果在线备份期间发生崩溃，
			 * 可能不存在，
			 * 这会留下一个 backup_label 引用已经被归档的 WAL 段。
			 */
			if (checkPoint.redo < CheckPointLoc)
			{
				XLogPrefetcherBeginRead(xlogprefetcher, checkPoint.redo);
				if (!ReadRecord(xlogprefetcher, LOG, false,
								checkPoint.ThisTimeLineID))
					ereport(FATAL,
							(errmsg("could not find redo location referenced by checkpoint record"),
							 errhint("If you are restoring from a backup, touch \"%s/recovery.signal\" and add required recovery options.\n"
									 "If you are not restoring from a backup, try removing the file \"%s/backup_label\".\n"
									 "Be careful: removing \"%s/backup_label\" will result in a corrupt cluster if restoring from a backup.",
									 DataDir, DataDir, DataDir)));
			}
		}
		else
		{
			ereport(FATAL,
					(errmsg("could not locate required checkpoint record"),
					 errhint("If you are restoring from a backup, touch \"%s/recovery.signal\" and add required recovery options.\n"
							 "If you are not restoring from a backup, try removing the file \"%s/backup_label\".\n"
							 "Be careful: removing \"%s/backup_label\" will result in a corrupt cluster if restoring from a backup.",
							 DataDir, DataDir, DataDir)));
			wasShutdown = false;	/* 让编译器安静 */
		}

		/* 如果存在 tablespace_map 文件，则读取并创建符号链接。 */
		if (read_tablespace_map(&tablespaces))
		{
			ListCell   *lc;

			foreach(lc, tablespaces)
			{
				tablespaceinfo *ti = lfirst(lc);
				char	   *linkloc;

				linkloc = psprintf("pg_tblspc/%s", ti->oid);

				/*
				 * 删除现有的符号链接（如果有），并在 PGDATA 下
				 * 创建符号链接。
				 */
				remove_tablespace_symlink(linkloc);

				if (symlink(ti->path, linkloc) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not create symbolic link \"%s\": %m",
									linkloc)));

				pfree(ti->oid);
				pfree(ti->path);
				pfree(ti);
			}

			/* 告诉调用者稍后删除它 */
			haveTblspcMap = true;
		}

		/* 告诉调用者稍后删除它 */
		haveBackupLabel = true;
	}
	else
	{
		/*
		 * 如果存在 tablespace_map 文件但没有 backup_label 文件，
		 * 这样的文件就没有用处。
		 * 保留它没有什么危害，但最好删除这个映射文件，
		 * 这样数据目录中就不会有任何冗余文件，
		 * 也能避免任何混淆。
		 * 不过，谨慎的做法是将文件重命名移走而不是完全删除，
		 * 我们也忽略重命名操作中发生的任何错误，
		 * 因为即使没有 backup_label 文件存在映射文件，它也是无害的。
		 */
		if (stat(TABLESPACE_MAP, &st) == 0)
		{
			unlink(TABLESPACE_MAP_OLD);
			if (durable_rename(TABLESPACE_MAP, TABLESPACE_MAP_OLD, DEBUG1) == 0)
				ereport(LOG,
						(errmsg("ignoring file \"%s\" because no file \"%s\" exists",
								TABLESPACE_MAP, BACKUP_LABEL_FILE),
						 errdetail("File \"%s\" was renamed to \"%s\".",
								   TABLESPACE_MAP, TABLESPACE_MAP_OLD)));
			else
				ereport(LOG,
						(errmsg("ignoring file \"%s\" because no file \"%s\" exists",
								TABLESPACE_MAP, BACKUP_LABEL_FILE),
						 errdetail("Could not rename file \"%s\" to \"%s\": %m.",
								   TABLESPACE_MAP, TABLESPACE_MAP_OLD)));
		}

		/*
		 * 可能请求了归档恢复，但我们还不知道在达到一致性之前
		 * 需要重放多少 WAL。例如，若用原子文件系统快照从正在
		 * 运行的服务器做基础备份，却未调用
		 * pg_backup_start/stop，就会发生这种情况。或者只是杀掉正在运行的
		 * 主库，并创建恢复信号文件使其进入归档
		 * 恢复。
		 * 这种情况下的策略是先做崩溃恢复，
		 *
		 * 重放 pg_wal 中现有的全部 WAL，并在那之后再进入归档
		 * 恢复。
		 * 但通常我们已经知道需要重放多远（直到
		 *
		 * minRecoveryPoint、直到 backupEndPoint，或直到看见
		 * end-of-backup 记录），因而可以直接
		 * 进入归档恢复。
		 */
		if (ArchiveRecoveryRequested &&
			(ControlFile->minRecoveryPoint != InvalidXLogRecPtr ||
			 ControlFile->backupEndRequired ||
			 ControlFile->backupEndPoint != InvalidXLogRecPtr ||
			 ControlFile->state == DB_SHUTDOWNED))
		{
			InArchiveRecovery = true;
			if (StandbyModeRequested)
				EnableStandbyMode();
		}

		/*
		 * 与带着 backup_label 启动时出于同样的原因，当我们从基础备份
		 * 继续初始化时也发出一条日志
		 * 消息。
		 */
		if (!XLogRecPtrIsInvalid(ControlFile->backupStartPoint))
			ereport(LOG,
					(errmsg("restarting backup recovery with redo LSN %X/%X",
							LSN_FORMAT_ARGS(ControlFile->backupStartPoint))));

		/* 获取最后一个有效的检查点记录。 */
		CheckPointLoc = ControlFile->checkPoint;
		CheckPointTLI = ControlFile->checkPointCopy.ThisTimeLineID;
		RedoStartLSN = ControlFile->checkPointCopy.redo;
		RedoStartTLI = ControlFile->checkPointCopy.ThisTimeLineID;
		record = ReadCheckpointRecord(xlogprefetcher, CheckPointLoc,
									  CheckPointTLI);
		if (record != NULL)
		{
			ereport(DEBUG1,
					(errmsg_internal("checkpoint record is at %X/%X",
									 LSN_FORMAT_ARGS(CheckPointLoc))));
		}
		else
		{
			/*
			 * 我们以前会尝试回到辅助检查点记录，
			 * 但只在非备库模式下。
			 * 现在如果无法读取最后一个检查点，
			 * 我们就直接失败，因为这可以简化检查点周围的处理。
			 */
			ereport(PANIC,
					(errmsg("could not locate a valid checkpoint record")));
		}
		memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
		wasShutdown = ((record->xl_info & ~XLR_INFO_MASK) == XLOG_CHECKPOINT_SHUTDOWN);
	}

	/*
	 * 如果检查点记录的位置不在请求时间线历史的预期时间线上，
	 * 我们就无法继续：
	 * 该备份不属于请求时间线的历史。
	 */
	Assert(expectedTLEs);		/* 通过读取检查点
								 * 记录完成初始化 */
	if (tliOfPointInHistory(CheckPointLoc, expectedTLEs) !=
		CheckPointTLI)
	{
		XLogRecPtr	switchpoint;

		/*
		 * 如果检查点的时间线完全不在 expectedTLEs 中，
		 * tliSwitchPoint 会抛出错误。
		 */
		switchpoint = tliSwitchPoint(CheckPointTLI, expectedTLEs, NULL);
		ereport(FATAL,
				(errmsg("requested timeline %u is not a child of this server's history",
						recoveryTargetTLI),
				 errdetail("Latest checkpoint is at %X/%X on timeline %u, but in the history of the requested timeline, the server forked off from that timeline at %X/%X.",
						   LSN_FORMAT_ARGS(CheckPointLoc),
						   CheckPointTLI,
						   LSN_FORMAT_ARGS(switchpoint))));
	}

	/*
	 * 最小恢复点也应该属
	 * 于请求时间线的历史。
	 */
	if (!XLogRecPtrIsInvalid(ControlFile->minRecoveryPoint) &&
		tliOfPointInHistory(ControlFile->minRecoveryPoint - 1, expectedTLEs) !=
		ControlFile->minRecoveryPointTLI)
		ereport(FATAL,
				(errmsg("requested timeline %u does not contain minimum recovery point %X/%X on timeline %u",
						recoveryTargetTLI,
						LSN_FORMAT_ARGS(ControlFile->minRecoveryPoint),
						ControlFile->minRecoveryPointTLI)));

	ereport(DEBUG1,
			(errmsg_internal("redo record is at %X/%X; shutdown %s",
							 LSN_FORMAT_ARGS(checkPoint.redo),
							 wasShutdown ? "true" : "false")));
	ereport(DEBUG1,
			(errmsg_internal("next transaction ID: " UINT64_FORMAT "; next OID: %u",
							 U64FromFullTransactionId(checkPoint.nextXid),
							 checkPoint.nextOid)));
	ereport(DEBUG1,
			(errmsg_internal("next MultiXactId: %u; next MultiXactOffset: %u",
							 checkPoint.nextMulti, checkPoint.nextMultiOffset)));
	ereport(DEBUG1,
			(errmsg_internal("oldest unfrozen transaction ID: %u, in database %u",
							 checkPoint.oldestXid, checkPoint.oldestXidDB)));
	ereport(DEBUG1,
			(errmsg_internal("oldest MultiXactId: %u, in database %u",
							 checkPoint.oldestMulti, checkPoint.oldestMultiDB)));
	ereport(DEBUG1,
			(errmsg_internal("commit timestamp Xid oldest/newest: %u/%u",
							 checkPoint.oldestCommitTsXid,
							 checkPoint.newestCommitTsXid)));
	if (!TransactionIdIsNormal(XidFromFullTransactionId(checkPoint.nextXid)))
		ereport(PANIC,
				(errmsg("invalid next transaction ID")));

	/* 合理性检查 */
	if (checkPoint.redo > CheckPointLoc)
		ereport(PANIC,
				(errmsg("invalid redo in checkpoint record")));

	/*
	 * 检查是否需要强制从 WAL 恢复。
	 * 如果看起来是干净关闭且我们没有恢复信号文件，
	 * 则假设不需要恢复。
	 */
	if (checkPoint.redo < CheckPointLoc)
	{
		if (wasShutdown)
			ereport(PANIC,
					(errmsg("invalid redo record in shutdown checkpoint")));
		InRecovery = true;
	}
	else if (ControlFile->state != DB_SHUTDOWNED)
		InRecovery = true;
	else if (ArchiveRecoveryRequested)
	{
		/* 由于存在恢复信号文件而强制恢复 */
		InRecovery = true;
	}

	/*
	 * 如果需要恢复，
	 * 更新 pg_control 的内存副本以显示我们正在恢复，
	 * 并显示所选的检查点作为我们的起始位置。
	 * 我们还要用从备份历史文件获得的任何最小恢复停止点来标记 pg_control。
	 *
	 * 不过我们尚未将更改写入磁盘。
	 * 只在初始化各种子系统后才这样做。
	 */
	if (InRecovery)
	{
		if (InArchiveRecovery)
		{
			ControlFile->state = DB_IN_ARCHIVE_RECOVERY;
		}
		else
		{
			ereport(LOG,
					(errmsg("database system was not properly shut down; "
							"automatic recovery in progress")));
			if (recoveryTargetTLI > ControlFile->checkPointCopy.ThisTimeLineID)
				ereport(LOG,
						(errmsg("crash recovery starts in timeline %u "
								"and has target timeline %u",
								ControlFile->checkPointCopy.ThisTimeLineID,
								recoveryTargetTLI)));
			ControlFile->state = DB_IN_CRASH_RECOVERY;
		}
		ControlFile->checkPoint = CheckPointLoc;
		ControlFile->checkPointCopy = checkPoint;
		if (InArchiveRecovery)
		{
			/* 如果尚未设置，则初始化 minRecoveryPoint */
			if (ControlFile->minRecoveryPoint < checkPoint.redo)
			{
				ControlFile->minRecoveryPoint = checkPoint.redo;
				ControlFile->minRecoveryPointTLI = checkPoint.ThisTimeLineID;
			}
		}

		/*
		 * 如果我们正在从基础备份开始恢复，则设置 backupStartPoint。
		 *
		 * 如果我们正在从备库获取的基础备份开始恢复，
		 * 还要设置backupEndPoint 并使用 minRecoveryPoint 作为备份结束位置。
		 * 在这种情况下，
		 * pg_control 中的数据库系统状态必须指示数据库已经处于恢复中。
		 * 通常是 DB_IN_ARCHIVE_RECOVERY，
		 * 但如果恢复之前在此点之前被中断（例如因为 restore_command
		 * 或primary_conninfo 有问题），
		 * 也可能是 DB_SHUTDOWNED_IN_RECOVERY。
		 *
		 * 任何其他状态都表明备份某种程度已损坏，
		 * 我们无法合理地继续恢复。
		 */
		if (haveBackupLabel)
		{
			ControlFile->backupStartPoint = checkPoint.redo;
			ControlFile->backupEndRequired = backupEndRequired;

			if (backupFromStandby)
			{
				if (dbstate_at_startup != DB_IN_ARCHIVE_RECOVERY &&
					dbstate_at_startup != DB_SHUTDOWNED_IN_RECOVERY)
					ereport(FATAL,
							(errmsg("backup_label contains data inconsistent with control file"),
							 errhint("This means that the backup is corrupted and you will "
									 "have to use another backup for recovery.")));
				ControlFile->backupEndPoint = ControlFile->minRecoveryPoint;
			}
		}
	}

	/* 记住这些，以便我们知道何时达到一致性 */
	backupStartPoint = ControlFile->backupStartPoint;
	backupEndRequired = ControlFile->backupEndRequired;
	backupEndPoint = ControlFile->backupEndPoint;
	if (InArchiveRecovery)
	{
		minRecoveryPoint = ControlFile->minRecoveryPoint;
		minRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
	}
	else
	{
		minRecoveryPoint = InvalidXLogRecPtr;
		minRecoveryPointTLI = 0;
	}

	/*
	 * 开始恢复时假设最终记录没有丢失。
	 */
	abortedRecPtr = InvalidXLogRecPtr;
	missingContrecPtr = InvalidXLogRecPtr;

	*wasShutdown_ptr = wasShutdown;
	*haveBackupLabel_ptr = haveBackupLabel;
	*haveTblspcMap_ptr = haveTblspcMap;
}

/*
 * 查看是否存在恢复信号文件，若有则设置恢复
 * 状态。
 *
 * 查看是否存在恢复命令文件（recovery.conf），若有则
 * 抛出 ERROR，因为自 PG12 起我们不再识别该文件。
 */
static void
readRecoverySignalFile(void)
{
	struct stat stat_buf;

	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 检查旧的恢复 API 文件：recovery.conf
	 */
	if (stat(RECOVERY_COMMAND_FILE, &stat_buf) == 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("using recovery command file \"%s\" is not supported",
						RECOVERY_COMMAND_FILE)));

	/*
	 * 删除未使用的 .done 文件（如果存在）。忽略不存在的文件。
	 */
	unlink(RECOVERY_COMMAND_DONE);

	/*
	 * 检查恢复信号文件，
	 * 如果找到则对其进行 fsync，
	 * 因为它们代表服务器状态信息。不过我们不太担心 fsync 失败的可能性。
	 *
	 * 如果存在，standby 信号文件优先。如果两者都不存在，
	 * 则我们不会进入归档恢复。
	 */
	if (stat(STANDBY_SIGNAL_FILE, &stat_buf) == 0)
	{
		int			fd;

		fd = BasicOpenFilePerm(STANDBY_SIGNAL_FILE, O_RDWR | PG_BINARY,
							   S_IRUSR | S_IWUSR);
		if (fd >= 0)
		{
			(void) pg_fsync(fd);
			close(fd);
		}
		standby_signal_file_found = true;
	}
	else if (stat(RECOVERY_SIGNAL_FILE, &stat_buf) == 0)
	{
		int			fd;

		fd = BasicOpenFilePerm(RECOVERY_SIGNAL_FILE, O_RDWR | PG_BINARY,
							   S_IRUSR | S_IWUSR);
		if (fd >= 0)
		{
			(void) pg_fsync(fd);
			close(fd);
		}
		recovery_signal_file_found = true;
	}

	StandbyModeRequested = false;
	ArchiveRecoveryRequested = false;
	if (standby_signal_file_found)
	{
		StandbyModeRequested = true;
		ArchiveRecoveryRequested = true;
	}
	else if (recovery_signal_file_found)
	{
		StandbyModeRequested = false;
		ArchiveRecoveryRequested = true;
	}
	else
		return;

	/*
	 * 我们不支持单用户后端中的备库模式；
	 * 这需要其他进程（如 WAL 接收器）处于活动状态。
	 */
	if (StandbyModeRequested && !IsUnderPostmaster)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("standby mode is not supported by single-user servers")));
}

static void
validateRecoveryParameters(void)
{
	if (!ArchiveRecoveryRequested)
		return;

	/*
	 * 检查必要参数
	 */
	if (StandbyModeRequested)
	{
		if ((PrimaryConnInfo == NULL || strcmp(PrimaryConnInfo, "") == 0) &&
			(recoveryRestoreCommand == NULL || strcmp(recoveryRestoreCommand, "") == 0))
			ereport(WARNING,
					(errmsg("specified neither primary_conninfo nor restore_command"),
					 errhint("The database server will regularly poll the pg_wal subdirectory to check for files placed there.")));
	}
	else
	{
		if (recoveryRestoreCommand == NULL ||
			strcmp(recoveryRestoreCommand, "") == 0)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("must specify restore_command when standby mode is not enabled")));
	}

	/*
	 * 覆盖任何不一致的请求。注意这是 9.5 版本中的行为变更；
	 * 在此之前，如果 hot_standby = off，我们只是简单地忽略暂停请求，
	 * 这是令人惊讶的行为。
	 */
	if (recoveryTargetAction == RECOVERY_TARGET_ACTION_PAUSE &&
		!EnableHotStandby)
		recoveryTargetAction = RECOVERY_TARGET_ACTION_SHUTDOWN;

	/*
	 * recovery_target_time 字符串的最终解析；
	 * 另请参阅 check_recovery_target_time()。
	 */
	if (recoveryTarget == RECOVERY_TARGET_TIME)
	{
		recoveryTargetTime = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
																	 CStringGetDatum(recovery_target_time_string),
																	 ObjectIdGetDatum(InvalidOid),
																	 Int32GetDatum(-1)));
	}

	/*
	 * 如果用户指定了 recovery_target_timeline，
	 * 则验证它或计算"latest"值。
	 * 我们必须等到获取 restore_command 并设置 InArchiveRecovery 之后才能执行此操作，
	 * 因为我们需要从归档中获取时间线历史文件。
	 */
	if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_NUMERIC)
	{
		TimeLineID	rtli = recoveryTargetTLIRequested;

		/* 时间线 1 没有历史文件，其他都应该有 */
		if (rtli != 1 && !existsTimeLineHistory(rtli))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("recovery target timeline %u does not exist",
							rtli)));
		recoveryTargetTLI = rtli;
	}
	else if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)
	{
		/* 我们从 pg_control 的时间线开始"最新"搜索 */
		recoveryTargetTLI = findNewestTimeLine(recoveryTargetTLI);
	}
	else
	{
		/*
		 * 否则我们只使用从 ControlFile
		 * 读取的 recoveryTargetTLI
		 */
		Assert(recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_CONTROLFILE);
	}
}

/*
 * read_backup_label: 检查是否存在 backup_label 文件
 *
 * 如果我们在恢复期间看到 backup_label，
 * 我们假设正在从备份转储文件恢复，
 * 因此我们从标签文件标识的检查点向前滚动，
 * 而不是 pg_control 所说的内容。
 * 这避免了 pg_control 可能在转储开始后一个或多个检查点被归档的问题，
 * 因此如果我们依赖它作为起点，我们将无法恢复到一致的数据库状态。
 *
 * 如果找到 backup_label 则返回 true（并将检查点位置和 TLI 分别填入*checkPointLoc 和 *backupLabelTLI）；
 * 如果没找到则返回 false。
 * 如果此 backup_label 来自流式备份，
 * 则 *backupEndRequired 设置为 true。
 * 如果此 backup_label 是在恢复期间创建的，则 *backupFromStandby 设置为 true。
 *
 * 同时设置全局变量 RedoStartLSN 和 RedoStartTLI
 * 为从备份文件读取的 LSN 和 TLI。
 */
static bool
read_backup_label(XLogRecPtr *checkPointLoc, TimeLineID *backupLabelTLI,
				  bool *backupEndRequired, bool *backupFromStandby)
{
	char		startxlogfilename[MAXFNAMELEN];
	TimeLineID	tli_from_walseg,
				tli_from_file;
	FILE	   *lfp;
	char		ch;
	char		backuptype[20];
	char		backupfrom[20];
	char		backuplabel[MAXPGPATH];
	char		backuptime[128];
	uint32		hi,
				lo;

	/* 抑制可能的未初始化变量警告 */
	*checkPointLoc = InvalidXLogRecPtr;
	*backupLabelTLI = 0;
	*backupEndRequired = false;
	*backupFromStandby = false;

	/*
	 * 查看标签文件是否存在
	 */
	lfp = AllocateFile(BACKUP_LABEL_FILE, "r");
	if (!lfp)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							BACKUP_LABEL_FILE)));
		return false;			/* 不存在，一切正常 */
	}

	/*
	 * 读取并解析 START WAL LOCATION
	 * 和 CHECKPOINT 行（这段代码相当粗糙，
	 * 但我们不期望文件格式有任何变化）。
	 */
	if (fscanf(lfp, "START WAL LOCATION: %X/%X (file %08X%16s)%c",
			   &hi, &lo, &tli_from_walseg, startxlogfilename, &ch) != 5 || ch != '\n')
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	RedoStartLSN = ((uint64) hi) << 32 | lo;
	RedoStartTLI = tli_from_walseg;
	if (fscanf(lfp, "CHECKPOINT LOCATION: %X/%X%c",
			   &hi, &lo, &ch) != 3 || ch != '\n')
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));
	*checkPointLoc = ((uint64) hi) << 32 | lo;
	*backupLabelTLI = tli_from_walseg;

	/*
	 * BACKUP METHOD 
	 * 让我们知道这是否是典型的备份（"streamed"，
	 * 这可能意味着使用了 pg_basebackup 或
	 * pg_backup_start/stop 方法），
	 * 或者此标签来自其他地方（目前唯一的其他选择是来自 pg_rewind）。
	 * 如果这是流式备份，
	 * 我们知道需要重放到备份期间生成的 WAL 结束位置（此时我们将达到一致性，
	 * backupEndRequired 将被重置为 false）。
	 */
	if (fscanf(lfp, "BACKUP METHOD: %19s\n", backuptype) == 1)
	{
		if (strcmp(backuptype, "streamed") == 0)
			*backupEndRequired = true;
	}

	/*
	 * BACKUP FROM 让我们知道这是来自主库还是备库。
	 * 如果来自备库，
	 * 我们将再次检查控制文件状态是否与备库匹配。
	 */
	if (fscanf(lfp, "BACKUP FROM: %19s\n", backupfrom) == 1)
	{
		if (strcmp(backupfrom, "standby") == 0)
			*backupFromStandby = true;
	}

	/*
	 * 解析 START TIME 和 LABEL。
	 * 这些不是恢复的必填字段，
	 * 但检查它们的存在对于调试和后续完整性检查很有用。
	 * 还要考虑到结果缓冲区有预分配的大小，
	 * 因此如果 backup_label 文件包含超过此处假设最大长度的字符串，
	 * 则会发生错误的解析。
	 * 这没关系，因为之后只进行次要的一致性检查。
	 */
	if (fscanf(lfp, "START TIME: %127[^\n]\n", backuptime) == 1)
		ereport(DEBUG1,
				(errmsg_internal("backup time %s in file \"%s\"",
								 backuptime, BACKUP_LABEL_FILE)));

	if (fscanf(lfp, "LABEL: %1023[^\n]\n", backuplabel) == 1)
		ereport(DEBUG1,
				(errmsg_internal("backup label %s in file \"%s\"",
								 backuplabel, BACKUP_LABEL_FILE)));

	/*
	 * START TIMELINE 是 11 版本新增的。其解析不是强制的，
	 * 但如果存在则用作完整性检查。
	 */
	if (fscanf(lfp, "START TIMELINE: %u\n", &tli_from_file) == 1)
	{
		if (tli_from_walseg != tli_from_file)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE),
					 errdetail("Timeline ID parsed is %u, but expected %u.",
							   tli_from_file, tli_from_walseg)));

		ereport(DEBUG1,
				(errmsg_internal("backup timeline %u in file \"%s\"",
								 tli_from_file, BACKUP_LABEL_FILE)));
	}

	if (ferror(lfp) || FreeFile(lfp))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						BACKUP_LABEL_FILE)));

	return true;
}

/*
 * read_tablespace_map: 检查是否存在 tablespace_map 文件
 *
 * 如果我们在恢复期间看到 tablespace_map 文件，
 * 我们假设正在从备份转储文件恢复，
 * 因此我们需要根据 tablespace_map 文件中的信息创建符号链接。
 *
 * 如果找到 tablespace_map 文件则返回 true（并将 *tablespaces
 * 填充为文件中列出的每个表空间的 tablespaceinfo 结构体）；
 * 如果没找到则返回 false。
 */
static bool
read_tablespace_map(List **tablespaces)
{
	tablespaceinfo *ti;
	FILE	   *lfp;
	char		str[MAXPGPATH];
	int			ch,
				i,
				n;
	bool		was_backslash;

	/*
	 * 查看 tablespace_map 文件是否存在
	 */
	lfp = AllocateFile(TABLESPACE_MAP, "r");
	if (!lfp)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							TABLESPACE_MAP)));
		return false;			/* 不存在，一切正常 */
	}

	/*
	 * 读取并解析 tablespace_map 文件中的链接名称和路径行
	 * （这段代码相当粗糙，但我们不期望文件格式有任何变化）。
	 * 对插入的任何反斜杠进行反转义处理。
	 */
	i = 0;
	was_backslash = false;
	while ((ch = fgetc(lfp)) != EOF)
	{
		if (!was_backslash && (ch == '\n' || ch == '\r'))
		{
			if (i == 0)
				continue;		/* \r 紧接着 \n */

			/*
			 * 反转义后的行应包含一个 OID，
			 * 后跟恰好一个空格，再跟一个路径。
			 * 路径可能以空格开头，所以不要过于宽松地解析。
			 */
			str[i] = '\0';
			n = 0;
			while (str[n] && str[n] != ' ')
				n++;
			if (n < 1 || n >= i - 1)
				ereport(FATAL,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("invalid data in file \"%s\"", TABLESPACE_MAP)));
			str[n++] = '\0';

			ti = palloc0(sizeof(tablespaceinfo));
			ti->oid = pstrdup(str);
			ti->path = pstrdup(str + n);
			*tablespaces = lappend(*tablespaces, ti);

			i = 0;
			continue;
		}
		else if (!was_backslash && ch == '\\')
			was_backslash = true;
		else
		{
			if (i < sizeof(str) - 1)
				str[i++] = ch;
			was_backslash = false;
		}
	}

	if (i != 0 || was_backslash)	/* 最后一行未终止？ */
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", TABLESPACE_MAP)));

	if (ferror(lfp) || FreeFile(lfp))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						TABLESPACE_MAP)));

	return true;
}

/*
 * 完成 WAL 恢复。
 *
 * 此函数尚未关闭 'xlogreader'，
 * 因为在某些情况下调用者仍想通过调用ReadCheckpointRecord()
 * 重新读取最后一个检查点记录。
 *
 * 返回最后一个有效或已应用记录的位置（新 WAL 应在此之后追加）、
 * 恢复结束的原因信息以及其他一些内容。
 * 详见 EndOfWalRecoveryInfo 结构体。
 */
EndOfWalRecoveryInfo *
FinishWalRecovery(void)
{
	EndOfWalRecoveryInfo *result = palloc(sizeof(EndOfWalRecoveryInfo));
	XLogRecPtr	lastRec;
	TimeLineID	lastRecTLI;
	XLogRecPtr	endOfLog;

	/*
	 * 在我们继续写入启动检查点和中止的 contrecord 记录之前，
	 * 终止 WAL 接收器（如果它仍在运行）。
	 * 如果我们开始写入 WAL 时它仍然存活，
	 * 它将覆盖这些记录和后续记录。
	 */
	XLogShutdownWalRcv();

	/*
	 * 我们现在已经完成从流中读取 xlog。
	 * 关闭流式恢复以强制从归档或 pg_wal获取文件（这在恢复结束时可能需要，
	 * 例如时间线历史文件）。
	 *
	 * 注意，必须在终止 WAL 接收器之后关闭备库模式，
	 * 即调用 XLogShutdownWalRcv()。
	 */
	Assert(!WalRcvStreaming());
	StandbyMode = false;

	/*
	 * 确定接下来从何处开始写入 WAL。
	 *
	 * 重新获取最后一个有效或最后应用的记录，
	 * 以便我们能够识别我们认为 WAL有效部分的确切终点。
	 * 之后可能有一个不完整的延续记录，
	 * 在这种情况下，
	 * 'abortedRecPtr' 和 'missingContrecPtr' 被设置，
	 * 调用者将写入一个特殊的OVERWRITE_CONTRECORD 消息来标记其余部分是有意缺失的。参见 CreateOverwriteContrecordRecord()。
	 *
	 * 这的一个重要副作用是将最后一页加载到 xlogreader 中。调用者使用它来
	 * 初始化 WAL 以进行写入。
	 */
	if (!InRecovery)
	{
		lastRec = CheckPointLoc;
		lastRecTLI = CheckPointTLI;
	}
	else
	{
		lastRec = XLogRecoveryCtl->lastReplayedReadRecPtr;
		lastRecTLI = XLogRecoveryCtl->lastReplayedTLI;
	}
	XLogPrefetcherBeginRead(xlogprefetcher, lastRec);
	(void) ReadRecord(xlogprefetcher, PANIC, false, lastRecTLI);
	endOfLog = xlogreader->EndRecPtr;

	/*
	 * 记住包含日志结尾的 XLOG
	 * 段文件名中的 TLI。
	 * 它可能与 endOfLog 名义上所属的时间线不同，
	 * 如果该段中发生了时间线切换，
	 * 而我们正在从属于更高时间线的段中读取旧 WAL。
	 */
	result->endOfLogTLI = xlogreader->seg.ws_tli;

	if (ArchiveRecoveryRequested)
	{
		/*
		 * 我们不再处于归档恢复状态。
		 *
		 * 我们现在已经完成读取旧 WAL。
		 * 如果归档获取处于活动状态，则将其关闭。
		 */
		Assert(InArchiveRecovery);
		InArchiveRecovery = false;

		/*
		 * 如果结束的日志段仍然打开，请关闭它（以避免在 Windows 上
		 * 尝试重命名或删除打开的文件时出现问题）。
		 */
		if (readFile >= 0)
		{
			close(readFile);
			readFile = -1;
		}
	}

	/*
	 * 将最后一个部分块复制给调用者，
	 * 用于初始化 WAL 缓冲区以追加新 WAL。
	 */
	if (endOfLog % XLOG_BLCKSZ != 0)
	{
		char	   *page;
		int			len;
		XLogRecPtr	pageBeginPtr;

		pageBeginPtr = endOfLog - (endOfLog % XLOG_BLCKSZ);
		Assert(readOff == XLogSegmentOffset(pageBeginPtr, wal_segment_size));

		/* 复制最后一个块的有效部分 */
		len = endOfLog % XLOG_BLCKSZ;
		page = palloc(len);
		memcpy(page, xlogreader->readBuf, len);

		result->lastPageBeginPtr = pageBeginPtr;
		result->lastPage = page;
	}
	else
	{
		/* 没有要复制的部分块。 */
		result->lastPageBeginPtr = endOfLog;
		result->lastPage = NULL;
	}

	/*
	 * 为历史文件创建注释，
	 * 以解释时间线为何以及何处发生更改。
	 */
	result->recoveryStopReason = getRecoveryStopReason();

	result->lastRec = lastRec;
	result->lastRecTLI = lastRecTLI;
	result->endOfLog = endOfLog;

	result->abortedRecPtr = abortedRecPtr;
	result->missingContrecPtr = missingContrecPtr;

	result->standby_signal_file_found = standby_signal_file_found;
	result->recovery_signal_file_found = recovery_signal_file_found;

	return result;
}

/*
 * 清理 WAL 读取器和从归档恢复 WAL 的残留文件
 */
void
ShutdownWalRecovery(void)
{
	char		recoveryPath[MAXPGPATH];

	/* pg_stat_recovery_prefetch 的最终更新。 */
	XLogPrefetcherComputeStats(xlogprefetcher);

	/* 关闭 xlogreader */
	if (readFile >= 0)
	{
		close(readFile);
		readFile = -1;
	}
	XLogReaderFree(xlogreader);
	XLogPrefetcherFree(xlogprefetcher);

	if (ArchiveRecoveryRequested)
	{
		/*
		 * 由于可能存在名为 RECOVERYXLOG 的部分 WAL 段，
		 * 将其删除。
		 */
		snprintf(recoveryPath, MAXPGPATH, XLOGDIR "/RECOVERYXLOG");
		unlink(recoveryPath);	/* 忽略任何错误 */

		/* 也要清除任何剩余的已恢复时间线历史文件 */
		snprintf(recoveryPath, MAXPGPATH, XLOGDIR "/RECOVERYHISTORY");
		unlink(recoveryPath);	/* 忽略任何错误 */
	}

	/*
	 * 我们不再需要锁存器了。
	 * 虽然严格来说不需要释放它，但为了整洁起见这样做。
	 */
	if (ArchiveRecoveryRequested)
		DisownLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
}

/*
 * 执行 WAL 恢复。
 *
 * 如果系统干净关闭，则永远不会调用此函数。
 */
void
PerformWalRecovery(void)
{
	XLogRecord *record;
	bool		reachedRecoveryTarget = false;
	TimeLineID	replayTLI;

	/*
	 * 初始化用于跟踪 WAL 重放进度的共享变量，
	 * 就像我们刚刚重放了 REDO 位置之前的记录（或者是关闭检查点，
	 * 则是检查点记录本身）。
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	if (RedoStartLSN < CheckPointLoc)
	{
		XLogRecoveryCtl->lastReplayedReadRecPtr = InvalidXLogRecPtr;
		XLogRecoveryCtl->lastReplayedEndRecPtr = RedoStartLSN;
		XLogRecoveryCtl->lastReplayedTLI = RedoStartTLI;
	}
	else
	{
		XLogRecoveryCtl->lastReplayedReadRecPtr = xlogreader->ReadRecPtr;
		XLogRecoveryCtl->lastReplayedEndRecPtr = xlogreader->EndRecPtr;
		XLogRecoveryCtl->lastReplayedTLI = CheckPointTLI;
	}
	XLogRecoveryCtl->replayEndRecPtr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	XLogRecoveryCtl->replayEndTLI = XLogRecoveryCtl->lastReplayedTLI;
	XLogRecoveryCtl->recoveryLastXTime = 0;
	XLogRecoveryCtl->currentChunkStartTime = 0;
	XLogRecoveryCtl->recoveryPauseState = RECOVERY_NOT_PAUSED;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/* 还要确保 XLogReceiptTime 具有合理的值 */
	XLogReceiptTime = GetCurrentTimestamp();

	/*
	 * 让 postmaster 知道我们现在已经开始重做，
	 * 以便它可以在必要时启动归档器。
	 */
	if (IsUnderPostmaster)
		SendPostmasterSignal(PMSIGNAL_RECOVERY_STARTED);

	/*
	 * 如果我们已经一致，则立即允许只读连接。
	 */
	CheckRecoveryConsistency();

	/*
	 * 查找逻辑上紧跟检查点的第一条记录
	 * —— 尽管它在物理上可能在检查点之前。
	 */
	if (RedoStartLSN < CheckPointLoc)
	{
		/* 回溯以查找记录 */
		replayTLI = RedoStartTLI;
		XLogPrefetcherBeginRead(xlogprefetcher, RedoStartLSN);
		record = ReadRecord(xlogprefetcher, PANIC, false, replayTLI);
	}
	else
	{
		/* 只需读取 CheckPoint 之后的下一条记录 */
		Assert(xlogreader->ReadRecPtr == CheckPointLoc);
		replayTLI = CheckPointTLI;
		record = ReadRecord(xlogprefetcher, LOG, false, replayTLI);
	}

	if (record != NULL)
	{
		TimestampTz xtime;
		PGRUsage	ru0;

		pg_rusage_init(&ru0);

		InRedo = true;

		RmgrStartup();

		ereport(LOG,
				(errmsg("redo starts at %X/%X",
						LSN_FORMAT_ARGS(xlogreader->ReadRecPtr))));

		/* 准备报告重做阶段的进度。 */
		if (!StandbyMode)
			begin_startup_progress_phase();

		/*
		 * 主重做应用循环
		 */
		do
		{
			if (!StandbyMode)
				ereport_startup_progress("redo in progress, elapsed time: %ld.%02d s, current LSN: %X/%X",
										 LSN_FORMAT_ARGS(xlogreader->ReadRecPtr));

#ifdef WAL_DEBUG
			if (XLOG_DEBUG ||
				(record->xl_rmid == RM_XACT_ID && trace_recovery_messages <= DEBUG2) ||
				(record->xl_rmid != RM_XACT_ID && trace_recovery_messages <= DEBUG3))
			{
				StringInfoData buf;

				initStringInfo(&buf);
				appendStringInfo(&buf, "REDO @ %X/%X; LSN %X/%X: ",
								 LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
								 LSN_FORMAT_ARGS(xlogreader->EndRecPtr));
				xlog_outrec(&buf, xlogreader);
				appendStringInfoString(&buf, " - ");
				xlog_outdesc(&buf, xlogreader);
				elog(LOG, "%s", buf.data);
				pfree(buf.data);
			}
#endif

			/* 处理启动进程的中断信号 */
			HandleStartupProcInterrupts();

			/*
			 * 如果热备会话通过 SetRecoveryPause()
			 * 请求暂停 WAL 重放。
			 *
			 * 注意我们故意不在此处获取 info_lck 自旋锁。
			 * 因此我们可能会读取稍微过时的
			 * recoveryPause
			 * 标志值，
			 * 但它不会非常过时（不会比我们上次获取的自旋锁更差）。
			 * 由于暂停请求本身就是相当异步的事情，
			 * 可能比原本晚一个 WAL 记录才响应它只是一个小问题，
			 * 所以不值得增加另一个自旋锁周期来防止这种情况。
			 */
			if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
				RECOVERY_NOT_PAUSED)
				recoveryPausesHere(false);

			/*
			 * 我们是否已到达恢复目标？
			 */
			if (recoveryStopsBefore(xlogreader))
			{
				reachedRecoveryTarget = true;
				break;
			}

			/*
			 * 如果我们被要求滞后于主库，
			 * 则在锁存器上等待直到足够的时间过去。
			 */
			if (recoveryApplyDelay(xlogreader))
			{
				/*
				 * 我们在此处再次测试恢复是否暂停。
				 * 如果用户设置了延迟应用，
				 * 可能是因为他们预期在出现问题时暂停恢复，
				 * 所以我们必须在此处再次测试，否则在延迟等待期间暂停将不起作用。
				 */
				if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
					RECOVERY_NOT_PAUSED)
					recoveryPausesHere(false);
			}

			/*
			 * 应用记录
			 */
			ApplyWalRecord(xlogreader, record, &replayTLI);

			/* 如果达到包含恢复目标，则退出循环 */
			if (recoveryStopsAfter(xlogreader))
			{
				reachedRecoveryTarget = true;
				break;
			}

			/* 否则，尝试获取下一个 WAL 记录 */
			record = ReadRecord(xlogprefetcher, LOG, false, replayTLI);
		} while (record != NULL);

		/*
		 * 主重做应用循环结束
		 */

		if (reachedRecoveryTarget)
		{
			if (!reachedConsistency)
				ereport(FATAL,
						(errmsg("requested recovery stop point is before consistent recovery point")));

			/*
			 * 这是最后一个可以用新的恢复目标重启恢复的点，
			 * 如果我们关闭并重新开始的话。
			 * 在此之后，
			 * 资源管理器可能会选择在恢复结束时执行永久性的纠正操作。
			 */
			switch (recoveryTargetAction)
			{
				case RECOVERY_TARGET_ACTION_SHUTDOWN:

					/*
					 * 以特殊返回码退出以请求关闭 postmaster。
					 * 日志消息由 postmaster 发出。
					 */
					proc_exit(3);

				case RECOVERY_TARGET_ACTION_PAUSE:
					SetRecoveryPause(true);
					recoveryPausesHere(true);

					/* 进入 promote */

				case RECOVERY_TARGET_ACTION_PROMOTE:
					break;
			}
		}

		RmgrCleanup();

		ereport(LOG,
				(errmsg("redo done at %X/%X system usage: %s",
						LSN_FORMAT_ARGS(xlogreader->ReadRecPtr),
						pg_rusage_show(&ru0))));
		xtime = GetLatestXTime();
		if (xtime)
			ereport(LOG,
					(errmsg("last completed transaction was at log time %s",
							timestamptz_to_str(xtime))));

		InRedo = false;
	}
	else
	{
		/* 检查点之后没有 WAL 记录 */
		ereport(LOG,
				(errmsg("redo is not required")));
	}

	/*
	 * 此检查故意放在上述日志消息之后，
	 * 这些消息指示恢复进行了多远。
	 */
	if (ArchiveRecoveryRequested &&
		recoveryTarget != RECOVERY_TARGET_UNSET &&
		!reachedRecoveryTarget)
		ereport(FATAL,
				(errmsg("recovery ended before configured recovery target was reached")));
}

/*
 * PerformWalRecovery 的子程序，用于应用一条 WAL 记录。
 */
static void
ApplyWalRecord(XLogReaderState *xlogreader, XLogRecord *record, TimeLineID *replayTLI)
{
	ErrorContextCallback errcallback;
	bool		switchedTLI = false;

	/* 为 ereport() 设置错误回溯支持 */
	errcallback.callback = rm_redo_error_callback;
	errcallback.arg = (void *) xlogreader;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * ShmemVariableCache->nextXid 必须在记录的 xid 之后。
	 */
	AdvanceNextFullTransactionIdPastXid(record->xl_xid);

	/*
	 * 在重放此记录之前，
	 * 检查此记录是否导致当前时间线发生更改。
	 * 该记录已被视为新时间线的一部分，因此我们在重放之前更新 replayTLI。
	 * 这很重要，
	 * 因为如果恢复在此记录之后停止，
	 * replayEndTLI 会被记录为最小恢复点的 TLI，所以需要正确设置。
	 */
	if (record->xl_rmid == RM_XLOG_ID)
	{
		TimeLineID	newReplayTLI = *replayTLI;
		TimeLineID	prevReplayTLI = *replayTLI;
		uint8		info = record->xl_info & ~XLR_INFO_MASK;

		if (info == XLOG_CHECKPOINT_SHUTDOWN)
		{
			CheckPoint	checkPoint;

			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			newReplayTLI = checkPoint.ThisTimeLineID;
			prevReplayTLI = checkPoint.PrevTimeLineID;
		}
		else if (info == XLOG_END_OF_RECOVERY)
		{
			xl_end_of_recovery xlrec;

			memcpy(&xlrec, XLogRecGetData(xlogreader), sizeof(xl_end_of_recovery));
			newReplayTLI = xlrec.ThisTimeLineID;
			prevReplayTLI = xlrec.PrevTimeLineID;
		}

		if (newReplayTLI != *replayTLI)
		{
			/* 检查切换到此 TLI 是否可行 */
			checkTimeLineSwitch(xlogreader->EndRecPtr,
								newReplayTLI, prevReplayTLI, *replayTLI);

			/* 后续 WAL 记录应使用新 TLI 运行 */
			*replayTLI = newReplayTLI;
			switchedTLI = true;
		}
	}

	/*
	 * 在重放此记录之前更新共享的 replayEndRecPtr，
	 * 以便 XLogFlush 正确更新 minRecoveryPoint。
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->replayEndRecPtr = xlogreader->EndRecPtr;
	XLogRecoveryCtl->replayEndTLI = *replayTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/*
	 * 若我们正尝试进入热备模式，则处理我们看到的 XIDs
	 */
	if (standbyState >= STANDBY_INITIALIZED &&
		TransactionIdIsValid(record->xl_xid))
		RecordKnownAssignedTransactionIds(record->xl_xid);

	/*
	 * 一些与恢复直接相关的 XLOG 记录类型在此处直接处理，
	 * 而不是在 xlog_redo() 中
	 */
	if (record->xl_rmid == RM_XLOG_ID)
		xlogrecovery_redo(xlogreader, *replayTLI);

	/* 现在应用 WAL 记录本身 */
	GetRmgr(record->xl_rmid).rm_redo(xlogreader);

	/*
	 * 重做之后，
	 * 检查与 WAL 记录关联的备份页是否与现有页一致。
	 * 仅当为此记录启用一致性检查时才执行此检查。
	 */
	if ((record->xl_info & XLR_CHECK_CONSISTENCY) != 0)
		verifyBackupPageConsistency(xlogreader);

	/* 弹出错误上下文堆栈 */
	error_context_stack = errcallback.previous;

	/*
	 * 在此记录成功重放后更新
	 * lastReplayedEndRecPtr。
	 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->lastReplayedReadRecPtr = xlogreader->ReadRecPtr;
	XLogRecoveryCtl->lastReplayedEndRecPtr = xlogreader->EndRecPtr;
	XLogRecoveryCtl->lastReplayedTLI = *replayTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/* ------
	 * ------唤醒 walsenders：
	 *
	 * 在备库上，
	 * WAL 首先被刷写（这只会唤醒物理 walsenders），
	 * 然后被应用，这只会唤醒逻辑 walsenders。
	 *
	 * 实际上，
	 * 备库上的逻辑 walsenders 在数据被应用之前无法解码和发送数据。
	 *
	 * 物理 walsenders 在重放期间不需要被唤醒，
	 * 除非允许级联复制且发生了时间线切换（
	 * 这样它们可以注意到自己处于新的时间线上）。
	 *
	 * 这就是为什么唤醒条件是：
	 *
	 *  - 物理 walsenders：
	 *    当有新时间线且允许级联复制时-
	 *  逻辑 walsenders：
	 *    当允许级联复制时（
	 * 否则无法创建）------
	 */
	if (AllowCascadeReplication())
		WalSndWakeup(switchedTLI, true);

	/*
	 * 如果 rm_redo 调用了 XLogRequestWalReceiverReply，
	 * 则我们唤醒接收器，
	 * 以便它注意到更新的 lastReplayedEndRecPtr 并向主库发送回复。
	 */
	if (doRequestWalReceiverReply)
	{
		doRequestWalReceiverReply = false;
		WalRcvForceReply();
	}

	/* 如果我们现在一致，则允许只读连接 */
	CheckRecoveryConsistency();

	/* 这是一个时间线切换吗？ */
	if (switchedTLI)
	{
		/*
		 * 在我们继续新时间线之前，
		 * 清理旧时间线上任何（可能虚假的）未来 WAL 段。
		 */
		RemoveNonParentXlogFiles(xlogreader->EndRecPtr, *replayTLI);

		/* 重置预取器。 */
		XLogPrefetchReconfigure();
	}
}

/*
 * 一些与 WAL 恢复直接相关的 XLOG RM 记录类型在此处处理，
 * 而不是在 xlog_redo() 中
 */
static void
xlogrecovery_redo(XLogReaderState *record, TimeLineID replayTLI)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRecPtr	lsn = record->EndRecPtr;

	Assert(XLogRecGetRmid(record) == RM_XLOG_ID);

	if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		/* 验证 XLOG_OVERWRITE_CONTRECORD 记录的有效载荷。 */
		xl_overwrite_contrecord xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_overwrite_contrecord));
		if (xlrec.overwritten_lsn != record->overwrittenRecPtr)
			elog(FATAL, "mismatching overwritten LSN %X/%X -> %X/%X",
				 LSN_FORMAT_ARGS(xlrec.overwritten_lsn),
				 LSN_FORMAT_ARGS(record->overwrittenRecPtr));

		/* 我们已经安全地跳过了中止的记录 */
		abortedRecPtr = InvalidXLogRecPtr;
		missingContrecPtr = InvalidXLogRecPtr;

		ereport(LOG,
				(errmsg("successfully skipped missing contrecord at %X/%X, overwritten at %s",
						LSN_FORMAT_ARGS(xlrec.overwritten_lsn),
						timestamptz_to_str(xlrec.overwrite_time))));

		/* 验证记录应该只发生一次 */
		record->overwrittenRecPtr = InvalidXLogRecPtr;
	}
	else if (info == XLOG_BACKUP_END)
	{
		XLogRecPtr	startpoint;

		memcpy(&startpoint, XLogRecGetData(record), sizeof(startpoint));

		if (backupStartPoint == startpoint)
		{
			/*
			 * 我们已到达基础备份的末尾，
			 * 即执行 pg_backup_stop() 的位置。
			 * 磁盘上的数据现在是一致的（假设我们也已到达 minRecoveryPoint）。
			 * 将 backupEndPoint 设置为当前 LSN，
			 * 以便下次调用 
			 * CheckRecoveryConsistency()时会注意到它并执行备份结束处理。
			 */
			elog(DEBUG1, "end of backup record reached");

			backupEndPoint = lsn;
		}
		else
			elog(DEBUG1, "saw end-of-backup record for backup starting at %X/%X, waiting for %X/%X",
				 LSN_FORMAT_ARGS(startpoint), LSN_FORMAT_ARGS(backupStartPoint));
	}
}

/*
 * 验证在非测试模式下，
 * ./pg_tblspc 不包含任何真实目录。
 *
 * 针对后来被删除的数据库的数据库创建 XLOG 记录的重放可能会在 pg_tblspc 中创建虚假目录。
 * 当达到一致性时，
 * 这些目录应该已被删除；
 * 这里我们验证这确实发生了。
 * 这应该在达到一致状态时调用。
 *
 * allow_in_place_tablespaces 将 PANIC 转换为 WARNING，
 * 这对于测试目的很有用，
 * 也可以在情况变糟时作为应急手段。
 */
static void
CheckTablespaceDirectory(void)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir("pg_tblspc");
	while ((de = ReadDir(dir, "pg_tblspc")) != NULL)
	{
		char		path[MAXPGPATH + 10];

		/* 跳过非 OID 名称的条目 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(path, sizeof(path), "pg_tblspc/%s", de->d_name);

		if (get_dirent_type(path, de, false, ERROR) != PGFILETYPE_LNK)
			ereport(allow_in_place_tablespaces ? WARNING : PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected directory entry \"%s\" found in %s",
							de->d_name, "pg_tblspc/"),
					 errdetail("All directory entries in pg_tblspc/ should be symbolic links."),
					 errhint("Remove those directories, or set allow_in_place_tablespaces to ON transiently to let recovery complete.")));
	}
}

/*
 * 检查恢复是否已达到一致状态。
 * 当达到一致性并且我们有一个有效的起始备库快照时，
 * 告诉 postmaster 它可以开始接受只读连接。
 */
static void
CheckRecoveryConsistency(void)
{
	XLogRecPtr	lastReplayedEndRecPtr;
	TimeLineID	lastReplayedTLI;

	/*
	 * 在崩溃恢复期间，
	 * 我们要重放所有 WAL 后才能达到一致状态。
	 */
	if (XLogRecPtrIsInvalid(minRecoveryPoint))
		return;

	Assert(InArchiveRecovery);

	/*
	 * 假设我们在启动进程中调用，
	 * 因此不需要锁来读取 lastReplayedEndRecPtr
	 */
	lastReplayedEndRecPtr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	lastReplayedTLI = XLogRecoveryCtl->lastReplayedTLI;

	/*
	 * 我们是否已到达基础备份完成的位置？
	 */
	if (!XLogRecPtrIsInvalid(backupEndPoint) &&
		backupEndPoint <= lastReplayedEndRecPtr)
	{
		XLogRecPtr	saveBackupStartPoint = backupStartPoint;
		XLogRecPtr	saveBackupEndPoint = backupEndPoint;

		elog(DEBUG1, "end of backup reached");

		/*
		 * 我们已经到达基础备份的末尾，如 pg_control 所指示。
		 * 相应地更新控制文件。
		 */
		ReachedEndOfBackup(lastReplayedEndRecPtr, lastReplayedTLI);
		backupStartPoint = InvalidXLogRecPtr;
		backupEndPoint = InvalidXLogRecPtr;
		backupEndRequired = false;

		ereport(LOG,
				(errmsg("completed backup recovery with redo LSN %X/%X and end LSN %X/%X",
						LSN_FORMAT_ARGS(saveBackupStartPoint),
						LSN_FORMAT_ARGS(saveBackupEndPoint))));
	}

	/*
	 * 我们是否已通过安全起始点？注意，
	 * 如果从备份恢复，
	 * minRecoveryPoint在 XLOG_BACKUP_END 到达并告知我们正确的 minRecoveryPoint 之前，
	 * 已知是错误设置的。在此之前我们只知道我们还没一致。
	 */
	if (!reachedConsistency && !backupEndRequired &&
		minRecoveryPoint <= lastReplayedEndRecPtr)
	{
		/*
		 * 检查
		 * XLOG 序列是否包含任何未解决的未初始化页面引用。
		 */
		XLogCheckInvalidPages();

		/*
		 * 检查 pg_tblspc 是否不包含任何真实目录。
		 * Database/CREATE_* 
		 * 记录的重放可能创建了虚假的表空间目录，
		 * 这些目录在达到一致性时应该已被删除。
		 */
		CheckTablespaceDirectory();

		reachedConsistency = true;
		ereport(LOG,
				(errmsg("consistent recovery state reached at %X/%X",
						LSN_FORMAT_ARGS(lastReplayedEndRecPtr))));
	}

	/*
	 * 我们是否有一个有效的起始快照以允许运行查询？如果有，
	 * 我们可以告诉 postmaster 数据库现在是一致的，
	 * 从而启用连接。
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY &&
		!LocalHotStandbyActive &&
		reachedConsistency &&
		IsUnderPostmaster)
	{
		SpinLockAcquire(&XLogRecoveryCtl->info_lck);
		XLogRecoveryCtl->SharedHotStandbyActive = true;
		SpinLockRelease(&XLogRecoveryCtl->info_lck);

		LocalHotStandbyActive = true;

		SendPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY);
	}
}

/*
 * rm_redo() 中发生错误时的错误上下文回调。
 */
static void
rm_redo_error_callback(void *arg)
{
	XLogReaderState *record = (XLogReaderState *) arg;
	StringInfoData buf;

	initStringInfo(&buf);
	xlog_outdesc(&buf, record);
	xlog_block_info(&buf, record);

	/* translator: %s is a WAL record description */
	errcontext("WAL redo at %X/%X for %s",
			   LSN_FORMAT_ARGS(record->ReadRecPtr),
			   buf.data);

	pfree(buf.data);
}

/*
 * 返回描述 XLogRecord 的字符串，
 * 由其标识可选地后跟冒号、空格和进一步描述组成。
 */
void
xlog_outdesc(StringInfo buf, XLogReaderState *record)
{
	RmgrData	rmgr = GetRmgr(XLogRecGetRmid(record));
	uint8		info = XLogRecGetInfo(record);
	const char *id;

	appendStringInfoString(buf, rmgr.rm_name);
	appendStringInfoChar(buf, '/');

	id = rmgr.rm_identify(info);
	if (id == NULL)
		appendStringInfo(buf, "UNKNOWN (%X): ", info & ~XLR_INFO_MASK);
	else
		appendStringInfo(buf, "%s: ", id);

	rmgr.rm_desc(buf, record);
}

#ifdef WAL_DEBUG

static void
xlog_outrec(StringInfo buf, XLogReaderState *record)
{
	appendStringInfo(buf, "prev %X/%X; xid %u",
					 LSN_FORMAT_ARGS(XLogRecGetPrev(record)),
					 XLogRecGetXid(record));

	appendStringInfo(buf, "; len %u",
					 XLogRecGetDataLen(record));

	xlog_block_info(buf, record);
}
#endif							/* WAL_DEBUG */

/*
 * 返回给出
 * XLogRecord 中所有块信息的字符串。
 */
static void
xlog_block_info(StringInfo buf, XLogReaderState *record)
{
	int			block_id;

	/* 解码块引用 */
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blk;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rlocator, &forknum, &blk, NULL))
			continue;

		if (forknum != MAIN_FORKNUM)
			appendStringInfo(buf, "; blkref #%d: rel %u/%u/%u, fork %u, blk %u",
							 block_id,
							 rlocator.spcOid, rlocator.dbOid,
							 rlocator.relNumber,
							 forknum,
							 blk);
		else
			appendStringInfo(buf, "; blkref #%d: rel %u/%u/%u, blk %u",
							 block_id,
							 rlocator.spcOid, rlocator.dbOid,
							 rlocator.relNumber,
							 blk);
		if (XLogRecHasBlockImage(record, block_id))
			appendStringInfoString(buf, " FPW");
	}
}


/*
 * 检查在恢复期间切换到新时间线是否可行。
 *
 * 'lsn' 是我们即将重放的关闭检查点记录的地址。
 * （目前，时间线只能在关闭检查点处更改）。
 */
static void
checkTimeLineSwitch(XLogRecPtr lsn, TimeLineID newTLI, TimeLineID prevTLI,
					TimeLineID replayTLI)
{
	/* 检查记录是否同意当前（旧）时间线是什么 */
	if (prevTLI != replayTLI)
		ereport(PANIC,
				(errmsg("unexpected previous timeline ID %u (current timeline ID %u) in checkpoint record",
						prevTLI, replayTLI)));

	/*
	 * 根据时间线历史，新时间线最好在我们期望看到的时间线列表中。
	 * 它也不应该减小。
	 */
	if (newTLI < replayTLI || !tliInHistory(newTLI, expectedTLEs))
		ereport(PANIC,
				(errmsg("unexpected timeline ID %u (after %u) in checkpoint record",
						newTLI, replayTLI)));

	/*
	 * 如果我们尚未到达最小恢复点，
	 * 且我们将要切换到一个大于最小恢复点时间线的时间线：
	 * 麻烦了。
	 * 切换到新时间线后，我们将无法再在正确的时间线上访问最小恢复点。
	 * 如果归档中有一个更新的时间线，
	 * 它分支在最小恢复点所在时间线之前，
	 * 而你尝试对新时间线进行 PITR，就会发生这种情况。
	 */
	if (!XLogRecPtrIsInvalid(minRecoveryPoint) &&
		lsn < minRecoveryPoint &&
		newTLI > minRecoveryPointTLI)
		ereport(PANIC,
				(errmsg("unexpected timeline ID %u in checkpoint record, before reaching minimum recovery point %X/%X on timeline %u",
						newTLI,
						LSN_FORMAT_ARGS(minRecoveryPoint),
						minRecoveryPointTLI)));

	/* 看起来不错 */
}


/*
 * 从 WAL 记录中提取时间戳。
 *
 * 如果记录包含时间戳，
 * 则返回 true，并将时间戳保存在 *recordXtime 中。
 * 如果记录类型没有时间戳，则返回 false。
 * 目前，只有事务提交/中止记录和恢复点包含时间戳。
 */
static bool
getRecordTimestamp(XLogReaderState *record, TimestampTz *recordXtime)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	uint8		xact_info = info & XLOG_XACT_OPMASK;
	uint8		rmid = XLogRecGetRmid(record);

	if (rmid == RM_XLOG_ID && info == XLOG_RESTORE_POINT)
	{
		*recordXtime = ((xl_restore_point *) XLogRecGetData(record))->rp_time;
		return true;
	}
	if (rmid == RM_XACT_ID && (xact_info == XLOG_XACT_COMMIT ||
							   xact_info == XLOG_XACT_COMMIT_PREPARED))
	{
		*recordXtime = ((xl_xact_commit *) XLogRecGetData(record))->xact_time;
		return true;
	}
	if (rmid == RM_XACT_ID && (xact_info == XLOG_XACT_ABORT ||
							   xact_info == XLOG_XACT_ABORT_PREPARED))
	{
		*recordXtime = ((xl_xact_abort *) XLogRecGetData(record))->xact_time;
		return true;
	}
	return false;
}

/*
 * 检查 WAL 记录中存储的当前缓冲区页和备份页是否一致。
 * 在比较两个页面之前，
 * 可以对页面应用掩码以忽略某些区域，
 * 如提示位、pd_lower 和 pd_upper
 * 之间的未使用空间等。
 * 此函数应在给定记录的 WAL 重放完成后调用。
 */
static void
verifyBackupPageConsistency(XLogReaderState *record)
{
	RmgrData	rmgr = GetRmgr(XLogRecGetRmid(record));
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	int			block_id;

	/* 没有备份块的记录不需要一致性检查。 */
	if (!XLogRecHasAnyBlockRefs(record))
		return;

	Assert((XLogRecGetInfo(record) & XLR_CHECK_CONSISTENCY) != 0);

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		Buffer		buf;
		Page		page;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rlocator, &forknum, &blkno, NULL))
		{
			/*
			 * WAL 记录不包含具有给定 id 的块引用。
			 * 不做任何操作。
			 */
			continue;
		}

		Assert(XLogRecHasBlockImage(record, block_id));

		if (XLogRecBlockImageApply(record, block_id))
		{
			/*
			 * WAL 记录已经应用了该页面，
			 * 因此跳过一致性检查，
			 * 因为这会导致将记录中存储的完整页面与自身进行比较。
			 */
			continue;
		}

		/*
		 * 从当前缓冲区读取内容并
		 * 将其存储在临时页面中。
		 */
		buf = XLogReadBufferExtended(rlocator, forknum, blkno,
									 RBM_NORMAL_NO_LOG,
									 InvalidBuffer);
		if (!BufferIsValid(buf))
			continue;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		/*
		 * 复制已应用 WAL 的本地页面，
		 * 以便在对其进行掩码之前有一个比较基础...
		 */
		memcpy(replay_image_masked, page, BLCKSZ);

		/* 复制完成后不再需要此页面 */
		UnlockReleaseBuffer(buf);

		/*
		 * 如果块 LSN 已经在此 WAL 记录之前，
		 * 我们不能期望内容匹配。
		 * 如果恢复重启，就会发生这种情况。
		 */
		if (PageGetLSN(replay_image_masked) > record->EndRecPtr)
			continue;

		/*
		 * 从 WAL 记录中存储的备份副本读取内容，
		 * 并将其存储在临时页面中。
		 * 这里不需要分配新页面，
		 * 本地缓冲区足以保存其内容，并且可以直接对其应用掩码。
		 */
		if (!RestoreBlockImage(record, block_id, primary_image_masked))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("%s", record->errormsg_buf)));

		/*
		 * 如果定义了掩码函数，
		 * 则对主映像和重放映像都进行掩码处理
		 */
		if (rmgr.rm_mask != NULL)
		{
			rmgr.rm_mask(replay_image_masked, blkno);
			rmgr.rm_mask(primary_image_masked, blkno);
		}

		/* 是时候比较主映像和重放映像了。 */
		if (memcmp(replay_image_masked, primary_image_masked, BLCKSZ) != 0)
		{
			elog(FATAL,
				 "inconsistent page found, rel %u/%u/%u, forknum %u, blkno %u",
				 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
				 forknum, blkno);
		}
	}
}

/*
 * 对于时间点恢复，
 * 此函数决定我们是否要在当前记录之前停止应用 XLOG。
 *
 * 如果我们停止，则返回 true，
 * 否则返回 false。如果停止，
 * 一些信息会保存在 recoveryStopXid 等变量中，用于注释新时间线的历史文件。
 */
static bool
recoveryStopsBefore(XLogReaderState *record)
{
	bool		stopsHere = false;
	uint8		xact_info;
	bool		isCommit;
	TimestampTz recordXtime = 0;
	TransactionId recordXid;

	/*
	 * 当不在归档恢复中时忽略恢复目标设
	 * 置（意味着我们处于崩溃恢复中）。
	 */
	if (!ArchiveRecoveryRequested)
		return false;

	/* 检查我们是否应在达到一致性时立即停止 */
	if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE && reachedConsistency)
	{
		ereport(LOG,
				(errmsg("recovery stopping after reaching consistency")));

		recoveryStopAfter = false;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		return true;
	}

	/* 检查目标 LSN 是否已到达 */
	if (recoveryTarget == RECOVERY_TARGET_LSN &&
		!recoveryTargetInclusive &&
		record->ReadRecPtr >= recoveryTargetLSN)
	{
		recoveryStopAfter = false;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = record->ReadRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		ereport(LOG,
				(errmsg("recovery stopping before WAL location (LSN) \"%X/%X\"",
						LSN_FORMAT_ARGS(recoveryStopLSN))));
		return true;
	}

	/* 否则我们只考虑在 COMMIT 或 ABORT 记录之前停止。 */
	if (XLogRecGetRmid(record) != RM_XACT_ID)
		return false;

	xact_info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT)
	{
		isCommit = true;
		recordXid = XLogRecGetXid(record);
	}
	else if (xact_info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
		xl_xact_parsed_commit parsed;

		isCommit = true;
		ParseCommitRecord(XLogRecGetInfo(record),
						  xlrec,
						  &parsed);
		recordXid = parsed.twophase_xid;
	}
	else if (xact_info == XLOG_XACT_ABORT)
	{
		isCommit = false;
		recordXid = XLogRecGetXid(record);
	}
	else if (xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
		xl_xact_parsed_abort parsed;

		isCommit = false;
		ParseAbortRecord(XLogRecGetInfo(record),
						 xlrec,
						 &parsed);
		recordXid = parsed.twophase_xid;
	}
	else
		return false;

	if (recoveryTarget == RECOVERY_TARGET_XID && !recoveryTargetInclusive)
	{
		/*
		 * 只有这一个精确事务
		 * ID 的事务结束记录
		 *
		 * 当测试 xid 时，
		 * 我们必须只测试相等性，
		 * 因为事务是按它们开始的顺序编号的，
		 * 而不是按它们完成的顺序。编号较大的 xid 大约有 50% 的时间会在你之前完成...
		 */
		stopsHere = (recordXid == recoveryTargetXid);
	}

	/*
	 * 注意：无论 recoveryTarget 设置如何，我们都必须获取 recordXtime。
	 * 我们不期望 getRecordTimestamp 会失败，因为我们已经知道这是提交或中止记录；
	 * 但还是要测试其结果。
	 */
	if (getRecordTimestamp(record, &recordXtime) &&
		recoveryTarget == RECOVERY_TARGET_TIME)
	{
		/*
		 * 可能有很多事务共享相同的提交时间，
		 * 所以如果我们是包含的，则在最后一个之后停止，
		 * 如果我们是排除的，则在第一个停止
		 */
		if (recoveryTargetInclusive)
			stopsHere = (recordXtime > recoveryTargetTime);
		else
			stopsHere = (recordXtime >= recoveryTargetTime);
	}

	if (stopsHere)
	{
		recoveryStopAfter = false;
		recoveryStopXid = recordXid;
		recoveryStopTime = recordXtime;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopName[0] = '\0';

		if (isCommit)
		{
			ereport(LOG,
					(errmsg("recovery stopping before commit of transaction %u, time %s",
							recoveryStopXid,
							timestamptz_to_str(recoveryStopTime))));
		}
		else
		{
			ereport(LOG,
					(errmsg("recovery stopping before abort of transaction %u, time %s",
							recoveryStopXid,
							timestamptz_to_str(recoveryStopTime))));
		}
	}

	return stopsHere;
}

/*
 * 与 recoveryStopsBefore 相同，但在应用记录后调用。
 *
 * 我们还在 XLogRecoveryCtl->recoveryLastXTime
 * 中跟踪最后应用的 COMMIT/ABORT 记录的时间戳。
 */
static bool
recoveryStopsAfter(XLogReaderState *record)
{
	uint8		info;
	uint8		xact_info;
	uint8		rmid;
	TimestampTz recordXtime = 0;

	/*
	 * 当不在归档恢复中时忽略恢复目标设
	 * 置（意味着我们处于崩溃恢复中）。
	 */
	if (!ArchiveRecoveryRequested)
		return false;

	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	rmid = XLogRecGetRmid(record);

	/*
	 * 可能有很多恢复点共享相同的名称；
	 * 我们在第一个停止。
	 */
	if (recoveryTarget == RECOVERY_TARGET_NAME &&
		rmid == RM_XLOG_ID && info == XLOG_RESTORE_POINT)
	{
		xl_restore_point *recordRestorePointData;

		recordRestorePointData = (xl_restore_point *) XLogRecGetData(record);

		if (strcmp(recordRestorePointData->rp_name, recoveryTargetName) == 0)
		{
			recoveryStopAfter = true;
			recoveryStopXid = InvalidTransactionId;
			recoveryStopLSN = InvalidXLogRecPtr;
			(void) getRecordTimestamp(record, &recoveryStopTime);
			strlcpy(recoveryStopName, recordRestorePointData->rp_name, MAXFNAMELEN);

			ereport(LOG,
					(errmsg("recovery stopping at restore point \"%s\", time %s",
							recoveryStopName,
							timestamptz_to_str(recoveryStopTime))));
			return true;
		}
	}

	/* 检查目标 LSN 是否已到达 */
	if (recoveryTarget == RECOVERY_TARGET_LSN &&
		recoveryTargetInclusive &&
		record->ReadRecPtr >= recoveryTargetLSN)
	{
		recoveryStopAfter = true;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopLSN = record->ReadRecPtr;
		recoveryStopTime = 0;
		recoveryStopName[0] = '\0';
		ereport(LOG,
				(errmsg("recovery stopping after WAL location (LSN) \"%X/%X\"",
						LSN_FORMAT_ARGS(recoveryStopLSN))));
		return true;
	}

	if (rmid != RM_XACT_ID)
		return false;

	xact_info = info & XLOG_XACT_OPMASK;

	if (xact_info == XLOG_XACT_COMMIT ||
		xact_info == XLOG_XACT_COMMIT_PREPARED ||
		xact_info == XLOG_XACT_ABORT ||
		xact_info == XLOG_XACT_ABORT_PREPARED)
	{
		TransactionId recordXid;

		/* 更新最后应用的事务时间戳 */
		if (getRecordTimestamp(record, &recordXtime))
			SetLatestXTime(recordXtime);

		/* 提取已提交/中止事务的 XID */
		if (xact_info == XLOG_XACT_COMMIT_PREPARED)
		{
			xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
			xl_xact_parsed_commit parsed;

			ParseCommitRecord(XLogRecGetInfo(record),
							  xlrec,
							  &parsed);
			recordXid = parsed.twophase_xid;
		}
		else if (xact_info == XLOG_XACT_ABORT_PREPARED)
		{
			xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
			xl_xact_parsed_abort parsed;

			ParseAbortRecord(XLogRecGetInfo(record),
							 xlrec,
							 &parsed);
			recordXid = parsed.twophase_xid;
		}
		else
			recordXid = XLogRecGetXid(record);

		/*
		 * 只有这一个精确事务
		 * ID 的事务结束记录
		 *
		 * 当测试 xid 时，
		 * 我们必须只测试相等性，
		 * 因为事务是按它们开始的顺序编号的，
		 * 而不是按它们完成的顺序。编号较大的 xid 大约有 50% 的时间会在你之前完成...
		 */
		if (recoveryTarget == RECOVERY_TARGET_XID && recoveryTargetInclusive &&
			recordXid == recoveryTargetXid)
		{
			recoveryStopAfter = true;
			recoveryStopXid = recordXid;
			recoveryStopTime = recordXtime;
			recoveryStopLSN = InvalidXLogRecPtr;
			recoveryStopName[0] = '\0';

			if (xact_info == XLOG_XACT_COMMIT ||
				xact_info == XLOG_XACT_COMMIT_PREPARED)
			{
				ereport(LOG,
						(errmsg("recovery stopping after commit of transaction %u, time %s",
								recoveryStopXid,
								timestamptz_to_str(recoveryStopTime))));
			}
			else if (xact_info == XLOG_XACT_ABORT ||
					 xact_info == XLOG_XACT_ABORT_PREPARED)
			{
				ereport(LOG,
						(errmsg("recovery stopping after abort of transaction %u, time %s",
								recoveryStopXid,
								timestamptz_to_str(recoveryStopTime))));
			}
			return true;
		}
	}

	/* 检查我们是否应在达到一致性时立即停止 */
	if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE && reachedConsistency)
	{
		ereport(LOG,
				(errmsg("recovery stopping after reaching consistency")));

		recoveryStopAfter = true;
		recoveryStopXid = InvalidTransactionId;
		recoveryStopTime = 0;
		recoveryStopLSN = InvalidXLogRecPtr;
		recoveryStopName[0] = '\0';
		return true;
	}

	return false;
}

/*
 * 为历史文件创建注释，
 * 以解释时间线为何以及何处发生更改。
 */
static char *
getRecoveryStopReason(void)
{
	char		reason[200];

	if (recoveryTarget == RECOVERY_TARGET_XID)
		snprintf(reason, sizeof(reason),
				 "%s transaction %u",
				 recoveryStopAfter ? "after" : "before",
				 recoveryStopXid);
	else if (recoveryTarget == RECOVERY_TARGET_TIME)
		snprintf(reason, sizeof(reason),
				 "%s %s\n",
				 recoveryStopAfter ? "after" : "before",
				 timestamptz_to_str(recoveryStopTime));
	else if (recoveryTarget == RECOVERY_TARGET_LSN)
		snprintf(reason, sizeof(reason),
				 "%s LSN %X/%X\n",
				 recoveryStopAfter ? "after" : "before",
				 LSN_FORMAT_ARGS(recoveryStopLSN));
	else if (recoveryTarget == RECOVERY_TARGET_NAME)
		snprintf(reason, sizeof(reason),
				 "at restore point \"%s\"",
				 recoveryStopName);
	else if (recoveryTarget == RECOVERY_TARGET_IMMEDIATE)
		snprintf(reason, sizeof(reason), "reached consistency");
	else
		snprintf(reason, sizeof(reason), "no recovery target specified");

	return pstrdup(reason);
}

/*
 * 等待共享 recoveryPauseState 设置为 RECOVERY_NOT_PAUSED。
 *
 * 如果达到恢复目标并且由于 recovery_target_action=pause 而在恢复结束时开始暂停状态，
 * 则 endOfRecovery 为 true，
 * 否则为 false。
 */
static void
recoveryPausesHere(bool endOfRecovery)
{
	/* 除非用户可以连接，否则不要暂停！ */
	if (!LocalHotStandbyActive)
		return;

	/* 备库提升触发后不要暂停 */
	if (LocalPromoteIsTriggered)
		return;

	if (endOfRecovery)
		ereport(LOG,
				(errmsg("pausing at the end of recovery"),
				 errhint("Execute pg_wal_replay_resume() to promote.")));
	else
		ereport(LOG,
				(errmsg("recovery has paused"),
				 errhint("Execute pg_wal_replay_resume() to continue.")));

	/* 循环直到 recoveryPauseState 设置为 RECOVERY_NOT_PAUSED */
	while (GetRecoveryPauseState() != RECOVERY_NOT_PAUSED)
	{
		HandleStartupProcInterrupts();
		if (CheckForStandbyTrigger())
			return;

		/*
		 * 若请求了恢复暂停则设为已暂停。当我们仍在
		 * 循环中时，用户可能再次恢复又暂停，因此每次都要设置。
		 */
		ConfirmRecoveryPaused();

		/*
		 * 我们等待一个条件变量，
		 * 它会在暂停结束时立即唤醒我们，
		 * 但我们使用超时以便也能定期检查上述退出条件。
		 */
		ConditionVariableTimedSleep(&XLogRecoveryCtl->recoveryNotPausedCV, 1000,
									WAIT_EVENT_RECOVERY_PAUSE);
	}
	ConditionVariableCancelSleep();
}

/*
 * 当设置了 recovery_min_apply_delay 时，我们等待足够长的时间以确保
 * 某些记录类型至少延迟该间隔后才在主库上应用。
 *
 * 如果我们等待了，则返回 true。
 *
 * 注意，
 * 延迟是在 WAL 记录日志时间和备库上当前时间之间计算的。
 * 我们更希望跟踪此备库何时接收每个 WAL 记录，
 * 这将允许更一致的方法且不受时间同步问题的影响，
 * 但这需要更多的努力和复杂性，
 * 对可用性的实际收益很小。
 */
static bool
recoveryApplyDelay(XLogReaderState *record)
{
	uint8		xact_info;
	TimestampTz xtime;
	TimestampTz delayUntil;
	long		msecs;

	/* 如果没有配置延迟，则无需操作 */
	if (recovery_min_apply_delay <= 0)
		return false;

	/* 对尚未一致的数据库不应用延迟 */
	if (!reachedConsistency)
		return false;

	/* 如果请求了崩溃恢复，则无需操作 */
	if (!ArchiveRecoveryRequested)
		return false;

	/*
	 * 是 COMMIT 记录吗？
	 *
	 * 我们故意选择不延迟中止，
	 * 因为它们对 MVCC 没有影响。
	 * 我们已经允许重放没有时间戳的记录，
	 * 所以备库上早期冲突导致的问题已经有发生的可能。
	 */
	if (XLogRecGetRmid(record) != RM_XACT_ID)
		return false;

	xact_info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	if (xact_info != XLOG_XACT_COMMIT &&
		xact_info != XLOG_XACT_COMMIT_PREPARED)
		return false;

	if (!getRecordTimestamp(record, &xtime))
		return false;

	delayUntil = TimestampTzPlusMilliseconds(xtime, recovery_min_apply_delay);

	/*
	 * 如果已经过了应用此记录的时间，
	 * 则不设置锁存器直接退出
	 */
	msecs = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), delayUntil);
	if (msecs <= 0)
		return false;

	while (true)
	{
		ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);

		/* 这可能会更改 recovery_min_apply_delay。 */
		HandleStartupProcInterrupts();

		if (CheckForStandbyTrigger())
			break;

		/*
		 * 重新计算 delayUntil，
		 * 因为 recovery_min_apply_delay 可能在此循环等待期间发生了变化。
		 */
		delayUntil = TimestampTzPlusMilliseconds(xtime, recovery_min_apply_delay);

		/*
		 * 等待 GetCurrentTimestamp() 和 delayUntil 之间的差值。
		 */
		msecs = TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												delayUntil);

		if (msecs <= 0)
			break;

		elog(DEBUG2, "recovery apply delay %ld milliseconds", msecs);

		(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 msecs,
						 WAIT_EVENT_RECOVERY_APPLY_DELAY);
	}
	return true;
}

/*
 * 获取恢复暂停请求的当前状态。
 */
RecoveryPauseState
GetRecoveryPauseState(void)
{
	RecoveryPauseState state;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	state = XLogRecoveryCtl->recoveryPauseState;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return state;
}

/*
 * 设置恢复暂停状态。
 *
 * 如果请求恢复暂停，
 * 则将恢复暂停状态设置为"暂停请求"（如果尚未"已暂停"）。
 * 否则，
 * 将其设置为"未暂停"以恢复恢复。恢复暂停将由 ConfirmRecoveryPaused 确认。
 */
void
SetRecoveryPause(bool recoveryPause)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);

	if (!recoveryPause)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_NOT_PAUSED;
	else if (XLogRecoveryCtl->recoveryPauseState == RECOVERY_NOT_PAUSED)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_PAUSE_REQUESTED;

	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (!recoveryPause)
		ConditionVariableBroadcast(&XLogRecoveryCtl->recoveryNotPausedCV);
}

/*
 * 通过将恢复暂停状态设置为
 * RECOVERY_PAUSED 来确认恢复暂停。
 */
static void
ConfirmRecoveryPaused(void)
{
	/* 如果请求恢复暂停，则将其设置为暂停 */
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	if (XLogRecoveryCtl->recoveryPauseState == RECOVERY_PAUSE_REQUESTED)
		XLogRecoveryCtl->recoveryPauseState = RECOVERY_PAUSED;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}


/*
 * 尝试读取下一条 XLOG 记录。
 *
 * 在第一次调用之前，
 * 需要通过调用 XLogPrefetcherBeginRead() 将读取器定位到第一条记录。
 *
 * 如果没有有效记录可用，则返回 NULL，
 * 或者如果 emode 为 PANIC 则失败。（emode 必须是 PANIC 或 LOG）。
 * 在备库模式下，重试直到有有效记录可用。
 */
static XLogRecord *
ReadRecord(XLogPrefetcher *xlogprefetcher, int emode,
		   bool fetching_ckpt, TimeLineID replayTLI)
{
	XLogRecord *record;
	XLogReaderState *xlogreader = XLogPrefetcherGetReader(xlogprefetcher);
	XLogPageReadPrivate *private = (XLogPageReadPrivate *) xlogreader->private_data;

	/* 传递参数到 XLogPageRead */
	private->fetching_ckpt = fetching_ckpt;
	private->emode = emode;
	private->randAccess = (xlogreader->ReadRecPtr == InvalidXLogRecPtr);
	private->replayTLI = replayTLI;

	/* 这是首次尝试读取此页面。 */
	lastSourceFailed = false;

	for (;;)
	{
		char	   *errormsg;

		record = XLogPrefetcherReadRecord(xlogprefetcher, &errormsg);
		if (record == NULL)
		{
			/*
			 * 当我们发现 WAL 以不完整的记录结束时，
			 * 跟踪该记录。
			 * 恢复完成后，
			 * 我们将写入一条记录，向下游 WAL 读取器指示该部分应被忽略。
			 *
			 * 然而，
			 * 当 ArchiveRecoveryRequested = true 时，
			 * 我们将在恢复结束时切换到新时间线。我们只会将 WAL 复制到新时间线到最后一条完整记录的末尾，
			 * 所以如果我们这样做，
			 * 稍后会在错误的位置创建覆盖 contrecord，破坏一切。
			 */
			if (!ArchiveRecoveryRequested &&
				!XLogRecPtrIsInvalid(xlogreader->abortedRecPtr))
			{
				abortedRecPtr = xlogreader->abortedRecPtr;
				missingContrecPtr = xlogreader->missingContrecPtr;
			}

			if (readFile >= 0)
			{
				close(readFile);
				readFile = -1;
			}

			/*
			 * 我们只在没有消息的情况下才会到达这里，
			 * 即 XLogPageRead() 失败时 ——在这种情况下我们已经记录了一些内容。
			 * 在 StandbyMode 下，
			 * 只有在我们被触发后才会发生这种情况，所以在这种情况下我们不应该再循环。
			 */
			if (errormsg)
				ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
						(errmsg_internal("%s", errormsg) /* 已翻译 */ ));
		}

		/*
		 * 检查页面 TLI 是否为预期值之一。
		 */
		else if (!tliInHistory(xlogreader->latestPageTLI, expectedTLEs))
		{
			char		fname[MAXFNAMELEN];
			XLogSegNo	segno;
			int32		offset;

			XLByteToSeg(xlogreader->latestPagePtr, segno, wal_segment_size);
			offset = XLogSegmentOffset(xlogreader->latestPagePtr,
									   wal_segment_size);
			XLogFileName(fname, xlogreader->seg.ws_tli, segno,
						 wal_segment_size);
			ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
					(errmsg("unexpected timeline ID %u in WAL segment %s, LSN %X/%X, offset %u",
							xlogreader->latestPageTLI,
							fname,
							LSN_FORMAT_ARGS(xlogreader->latestPagePtr),
							offset)));
			record = NULL;
		}

		if (record)
		{
			/* 很好，获取了一个记录 */
			return record;
		}
		else
		{
			/* 从此源无法获得有效记录 */
			lastSourceFailed = true;

			/*
			 * 如果请求了归档恢复，
			 * 但我们仍在进行崩溃恢复，
			 * 则切换到归档恢复并使用离线归档重试。
			 * 我们现在已经重放了 pg_wal 中的所有有效 WAL，所以现在应该是一致的。
			 *
			 * 但是，
			 * 我们要求 pg_wal 中至少有一些有效的 WAL（!fetching_ckpt）。
			 * 即使 pg_wal 完全为空，
			 * 我们也可以使用归档中的 WAL 进行恢复，
			 * 但我们不知道需要重放到多远才能达到一致性。所以为了安全起见放弃。
			 */
			if (!InArchiveRecovery && ArchiveRecoveryRequested &&
				!fetching_ckpt)
			{
				ereport(DEBUG1,
						(errmsg_internal("reached end of WAL in pg_wal, entering archive recovery")));
				InArchiveRecovery = true;
				if (StandbyModeRequested)
					EnableStandbyMode();

				SwitchIntoArchiveRecovery(xlogreader->EndRecPtr, replayTLI);
				minRecoveryPoint = xlogreader->EndRecPtr;
				minRecoveryPointTLI = replayTLI;

				CheckRecoveryConsistency();

				/*
				 * 在重试之前，重置 lastSourceFailed 和 currentSource，
				 * 以便我们接下来检查归档。
				 */
				lastSourceFailed = false;
				currentSource = XLOG_FROM_ANY;

				continue;
			}

			/* 在备库模式下，循环重试。否则，放弃。 */
			if (StandbyMode && !CheckForStandbyTrigger())
				continue;
			else
				return NULL;
		}
	}
}

/*
 * 将包含 targetPagePtr 的 XLOG 页面读入 readBuf（如果尚未读取）。如果成功读取页面，
 * 返回读取的字节数；如果出错，
 * 返回 XLREAD_FAIL。当发生错误时，
 * 会进行 ereport，但仅当它们之前未被报告时。
 *
 * 详见 XLogReaderRoutine.page_read。
 *
 * 在预取期间，可能会设置 xlogreader->nonblocking。在这种情况下，
 * 如果我们否则需要等待更多 WAL，则返回 XLREAD_WOULDBLOCK。
 *
 * 此函数负责根据需要从归档恢复文件，以及在备库模式下等待请求的
 * WAL 记录到达。
 *
 * xlogreader->private_data->emode 指定用于在归档恢复中报告"文件未找到"或"WAL 结束"情况的日志级别，
 * 或在触发提升时的备库模式下。
 * 如果设置为 WARNING 或更低，
 * XLogPageRead() 在这些情况下返回 XLREAD_FAIL，
 * 在更高的日志级别下，ereport() 不会返回。
 *
 * 在备库模式下，
 * 如果在 XLogPageRead() 成功返回后，
 * 调用者发现它感兴趣的记录已损坏，
 * 它应该使用 emode_for_corrupt_record() 确定的级别 ereport该错误，
 * 然后设置 lastSourceFailed 并使用相同的参数再次调用 XLogPageRead()。
 * 这让 XLogPageRead() 尝试从其他源获取记录，
 * 或休眠并重试。
 */
static int
XLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr, int reqLen,
			 XLogRecPtr targetRecPtr, char *readBuf)
{
	XLogPageReadPrivate *private =
		(XLogPageReadPrivate *) xlogreader->private_data;
	int			emode = private->emode;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo PG_USED_FOR_ASSERTS_ONLY;
	int			r;

	XLByteToSeg(targetPagePtr, targetSegNo, wal_segment_size);
	targetPageOff = XLogSegmentOffset(targetPagePtr, wal_segment_size);

	/*
	 * 查看是否需要切换到新段，
	 * 因为请求的记录不在当前打开的段中。
	 */
	if (readFile >= 0 &&
		!XLByteInSeg(targetPagePtr, readSegNo, wal_segment_size))
	{
		/*
		 * 如果自上次检查点以来我们重放了太多 xlog，
		 * 则请求重启点。
		 */
		if (ArchiveRecoveryRequested && IsUnderPostmaster)
		{
			if (XLogCheckpointNeeded(readSegNo))
			{
				(void) GetRedoRecPtr();
				if (XLogCheckpointNeeded(readSegNo))
					RequestCheckpoint(CHECKPOINT_CAUSE_XLOG);
			}
		}

		close(readFile);
		readFile = -1;
		readSource = XLOG_FROM_ANY;
	}

	XLByteToSeg(targetPagePtr, readSegNo, wal_segment_size);

retry:
	/* 检查是否需要检索更多数据 */
	if (readFile < 0 ||
		(readSource == XLOG_FROM_STREAM &&
		 flushedUpto < targetPagePtr + reqLen))
	{
		if (readFile >= 0 &&
			xlogreader->nonblocking &&
			readSource == XLOG_FROM_STREAM &&
			flushedUpto < targetPagePtr + reqLen)
			return XLREAD_WOULDBLOCK;

		switch (WaitForWALToBecomeAvailable(targetPagePtr + reqLen,
											private->randAccess,
											private->fetching_ckpt,
											targetRecPtr,
											private->replayTLI,
											xlogreader->EndRecPtr,
											xlogreader->nonblocking))
		{
			case XLREAD_WOULDBLOCK:
				return XLREAD_WOULDBLOCK;
			case XLREAD_FAIL:
				if (readFile >= 0)
					close(readFile);
				readFile = -1;
				readLen = 0;
				readSource = XLOG_FROM_ANY;
				return XLREAD_FAIL;
			case XLREAD_SUCCESS:
				break;
		}
	}

	/*
	 * 此时，我们已经打开了正确的段，如果我们正在流式传输，
	 * 我们知道请求的记录在其中。
	 */
	Assert(readFile != -1);

	/*
	 * 如果当前段正从主库流式传输，
	 * 计算我们已收到当前页面的多少内容。
	 * 我们知道请求的记录已收到，
	 * 但这是为了将来调用的好处，以便在此函数顶部快速退出。
	 */
	if (readSource == XLOG_FROM_STREAM)
	{
		if (((targetPagePtr) / XLOG_BLCKSZ) != (flushedUpto / XLOG_BLCKSZ))
			readLen = XLOG_BLCKSZ;
		else
			readLen = XLogSegmentOffset(flushedUpto, wal_segment_size) -
				targetPageOff;
	}
	else
		readLen = XLOG_BLCKSZ;

	/* 读取请求的页面 */
	readOff = targetPageOff;

	pgstat_report_wait_start(WAIT_EVENT_WAL_READ);
	r = pg_pread(readFile, readBuf, XLOG_BLCKSZ, (off_t) readOff);
	if (r != XLOG_BLCKSZ)
	{
		char		fname[MAXFNAMELEN];
		int			save_errno = errno;

		pgstat_report_wait_end();
		XLogFileName(fname, curFileTLI, readSegNo, wal_segment_size);
		if (r < 0)
		{
			errno = save_errno;
			ereport(emode_for_corrupt_record(emode, targetPagePtr + reqLen),
					(errcode_for_file_access(),
					 errmsg("could not read from WAL segment %s, LSN %X/%X, offset %u: %m",
							fname, LSN_FORMAT_ARGS(targetPagePtr),
							readOff)));
		}
		else
			ereport(emode_for_corrupt_record(emode, targetPagePtr + reqLen),
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read from WAL segment %s, LSN %X/%X, offset %u: read %d of %zu",
							fname, LSN_FORMAT_ARGS(targetPagePtr),
							readOff, r, (Size) XLOG_BLCKSZ)));
		goto next_record_is_invalid;
	}
	pgstat_report_wait_end();

	Assert(targetSegNo == readSegNo);
	Assert(targetPageOff == readOff);
	Assert(reqLen <= readLen);

	xlogreader->seg.ws_tli = curFileTLI;

	/*
	 * 立即检查页面头，
	 * 这样如果无效我们可以立即重试。
	 * 这可能看起来不必要，因为 ReadPageInternal() 无论如何都会验证页面头，
	 * 并将失败传播到ReadRecord()，
	 * 后者会重试。但是，
	 * 存在一个关于延续记录的边界情况，
	 * 如果一个记录跨越两个页面，我们需要从两个不同 WAL 段的不同源读取这两个页面。
	 *
	 * 第一个页面只在本地 pg_wal 中可用，
	 * 因为它在主库上已被回收。
	 * 然而，第二个页面不在 pg_wal 中，
	 * 我们应该从主库流式传输它。
	 * 但是在 pg_wal 中存在一个已回收的 WAL 段，其中包含垃圾内容。
	 * 我们会从本地 WAL 段读取第一个页面，
	 * 但在读取第二个页面时，我们会读取虚假的、已回收的 WAL 段。
	 * 如果我们不在这里捕获该情况，
	 * 我们将永远无法恢复，因为 ReadRecord() 会从头开始重试读取整个记录。
	 *
	 * 当然，
	 * 这只能捕获页面头中的错误，这正是已回收 WAL 段情况下发生的情况。
	 * 其他类型的错误或损坏仍然有同样的问题。
	 * 但这至少修复了常见情况，它可以作为正常操作的一部分发生。
	 *
	 * 验证页面头的开销足够小，
	 * 从性能角度来看做两次不应该是什么大问题。
	 *
	 * 当不在备库模式时，
	 * 无效的页面头应该导致恢复结束，而不是重试读取页面，
	 * 所以我们不需要在这里验证页面头以进行重试。
	 * 相反，ReadPageInternal()负责验证。
	 */
	if (StandbyMode &&
		(targetPagePtr % wal_segment_size) == 0 &&
		!XLogReaderValidatePageHeader(xlogreader, targetPagePtr, readBuf))
	{
		/*
		 * 立即发出此错误，然后立即重试此页面。
		 * 使用 errmsg_internal() 因为消息已经翻译过了。
		 */
		if (xlogreader->errormsg_buf[0])
			ereport(emode_for_corrupt_record(emode, xlogreader->EndRecPtr),
					(errmsg_internal("%s", xlogreader->errormsg_buf)));

		/* 重置 XLogReaderValidatePageHeader() 可能设置的任何错误 */
		XLogReaderResetError(xlogreader);
		goto next_record_is_invalid;
	}

	return readLen;

next_record_is_invalid:

	/*
	 * 如果我们正在预读，快速放弃。重试和错误报告将在恢复赶上此点时
	 * 由后续读取处理。
	 */
	if (xlogreader->nonblocking)
		return XLREAD_WOULDBLOCK;

	lastSourceFailed = true;

	if (readFile >= 0)
		close(readFile);
	readFile = -1;
	readLen = 0;
	readSource = XLOG_FROM_ANY;

	/* 在备库模式下，继续尝试 */
	if (StandbyMode)
		goto retry;
	else
		return XLREAD_FAIL;
}

/*
 * 打开包含 WAL 位置 'RecPtr' 的 WAL 段。
 *
 * 该段可以通过 restore_command 获取，
 * 或通过 walreceiver 流式传输记录，
 * 或者已经存在于 pg_wal 中。检查 pg_wal 主要用于崩溃恢复，
 * 但在备库模式下也会轮询，
 * 以防有人将新段直接复制到 pg_wal。不过这并没有被记录或推荐。
 *
 * 如果 'fetching_ckpt' 为真，我们正在获取检查点记录，应该准备在此之后
 * 从 RedoStartLSN 开始读取 WAL。
 *
 * 'RecPtr' 可能不指向我们感兴趣的记录的开头，
 * 它也可能指向页面或段头。
 * 在这种情况下，
 * 'tliRecPtr' 是我们感兴趣的 WAL 记录的位置。它用于决定从哪个时间线流式传输请求的 WAL。
 *
 * 'replayLSN' 是当前重放 LSN，
 * 这样如果我们扫描新时间线，
 * 我们可以拒绝切换到在此点之前分支的时间线。
 *
 * 如果记录不能立即可用，
 * 当不在备库模式时函数返回 false。
 * 在备库模式下，等待它变为可用。
 *
 * 当请求的记录变为可用时，
 * 函数打开包含它的文件（如果尚未打开），
 * 并返回 XLREAD_SUCCESS。
 * 当用户触发备库模式结束且没有更多 WAL 可用时，返回 XLREAD_FAIL。
 *
 * 如果 nonblocking 为真，则如果我们无法满足请求，立即放弃，
 * 返回 XLREAD_WOULDBLOCK 而不是等待。
 */
static XLogPageReadResult
WaitForWALToBecomeAvailable(XLogRecPtr RecPtr, bool randAccess,
							bool fetching_ckpt, XLogRecPtr tliRecPtr,
							TimeLineID replayTLI, XLogRecPtr replayLSN,
							bool nonblocking)
{
	static TimestampTz last_fail_time = 0;
	TimestampTz now;
	bool		streaming_reply_sent = false;

	/*-------
	 * 备库模式由状态机实现：
	 *
	 * 1. 从归档或 pg_wal 读取（XLOG_FROM_ARCHIVE），或仅从 pg_wal 读取
	 *	  （XLOG_FROM_PG_WAL）
	 * 2. 检查提升触发请求
	 * 3. 通过 walreceiver 从主库读取（XLOG_FROM_STREAM）
	 * 4. 重新扫描时间线
	 * 5. 休眠 wal_retrieve_retry_interval 毫秒，然后循环回到 1。
	 *
	 * 从当前源读取失败会使状
	 * 态机前进到下一个状态。
	 *
	 * 'currentSource' 指示当前状态。
	 * 没有"检查触发器"、"重新扫描时间线"和"休眠"状态的 currentSource 值，
	 * 这些操作在从上一个源读取失败时执行，
	 * 作为前进到下一个状态的一部分。
	 *
	 * 如果在从流读取 WAL 时关闭备库模式，
	 * 我们移动到 XLOG_FROM_ARCHIVE并重置 lastSourceFailed，
	 * 以强制从归档或 pg_wal 获取文件（这在恢复结束时需要，
	 * 例如时间线历史文件）。
	 * 我们不需要在这里终止 WAL 接收器，
	 * 因为在恢复结束时关闭备库模式时它已经停止了。
	 * -------
	 */
	if (!InArchiveRecovery)
		currentSource = XLOG_FROM_PG_WAL;
	else if (currentSource == XLOG_FROM_ANY ||
			 (!StandbyMode && currentSource == XLOG_FROM_STREAM))
	{
		lastSourceFailed = false;
		currentSource = XLOG_FROM_ARCHIVE;
	}

	for (;;)
	{
		XLogSource	oldSource = currentSource;
		bool		startWalReceiver = false;

		/*
		 * 首先检查我们是否未能从当前源读取，
		 * 如果是则推进状态机。
		 * 读取失败可能发生在此函数之外，
		 * 例如当记录上的 CRC 检查失败时，或在此循环内。
		 */
		if (lastSourceFailed)
		{
			/*
			 * 在非阻塞预读期间不
			 * 允许任何重试循环。
			 * 让调用者先处理已经解码的所有内容。
			 */
			if (nonblocking)
				return XLREAD_WOULDBLOCK;

			switch (currentSource)
			{
				case XLOG_FROM_ARCHIVE:
				case XLOG_FROM_PG_WAL:

					/*
					 * 检查是否请求了提升。
					 * 注意我们只在失败后才这样做，
					 * 所以当你提升时，
					 * 我们仍然会在故障切换前完成从归档和 pg_wal 重放尽可能多的内容。
					 */
					if (StandbyMode && CheckForStandbyTrigger())
					{
						XLogShutdownWalRcv();
						return XLREAD_FAIL;
					}

					/*
					 * 不在备库模式，
					 * 且我们已经尝试了归档和 pg_wal。
					 */
					if (!StandbyMode)
						return XLREAD_FAIL;

					/*
					 * 移动到 XLOG_FROM_STREAM 状态，
					 * 并设置必要时启动 walreceiver。
					 */
					currentSource = XLOG_FROM_STREAM;
					startWalReceiver = true;
					break;

				case XLOG_FROM_STREAM:

					/*
					 * 流式传输时的失败。
					 * 最可能的原因是流式复制被终止，
					 * 或触发了提升。
					 * 但如果我们在从主库流式传输的 WAL 中发现无效记录，也会到达这里，
					 * 这种情况下情况严重。
					 * 问题自行消失的可能性很小，
					 * 但 PANIC 对可用性也不好，
					 * 特别是在热备库模式下。所以，
					 * 我们将此视为断开连接，
					 * 并再次从归档/pg_wal 重试。
					 * 归档中的 WAL 应该与流式传输的相同，
					 * 所以它不太可能有帮助，但人们可以期待...
					 */

					/*
					 * 我们应该只能在备库模式下移动到
					 * XLOG_FROM_STREAM。
					 */
					Assert(StandbyMode);

					/*
					 * 在我们离开 XLOG_FROM_STREAM 状态之前，
					 * 确保 walreceiver不活动，
					 * 这样它就不会覆盖我们从归档恢复的 WAL。
					 *
					 * 如果 walreceiver 
					 * 正在主动流式传输（或尝试连接），
					 * 我们必须关闭它。
					 * 但如果它已经处于 WAITING 状态（例如由于时间线分歧），
					 * 我们只需要重置安装标志以允许归档恢复。
					 */
					if (WalRcvStreaming())
						XLogShutdownWalRcv();
					else
					{
						ResetInstallXLogFileSegmentActive();
					}

					/*
					 * 在休眠之前，如果我们被请求恢复到最新时间线，
					 * 则重新扫描可能的新时间线。
					 */
					if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)
					{
						if (rescanLatestTimeLine(replayTLI, replayLSN))
						{
							currentSource = XLOG_FROM_ARCHIVE;
							break;
						}
					}

					/*
					 * XLOG_FROM_STREAM 是我们状态机中的最后一个状态，
					 * 所以我们已经用尽了获取请求 WAL 的所有选项。
					 * 我们将循环回去并从归档重试，
					 * 但如果距离上次尝试时间不长，
					 * 则休眠 wal_retrieve_retry_interval
					 * 毫秒以避免忙等待。
					 */
					now = GetCurrentTimestamp();
					if (!TimestampDifferenceExceeds(last_fail_time, now,
													wal_retrieve_retry_interval))
					{
						long		wait_time;

						wait_time = wal_retrieve_retry_interval -
							TimestampDifferenceMilliseconds(last_fail_time, now);

						elog(LOG, "waiting for WAL to become available at %X/%X",
							 LSN_FORMAT_ARGS(RecPtr));

						/* 执行可能对我们后续有益的后台任务。 */
						KnownAssignedTransactionIdsIdleMaintenance();

						(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
										 WL_LATCH_SET | WL_TIMEOUT |
										 WL_EXIT_ON_PM_DEATH,
										 wait_time,
										 WAIT_EVENT_RECOVERY_RETRIEVE_RETRY_INTERVAL);
						ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
						now = GetCurrentTimestamp();

						/* 处理启动进程的中断信号 */
						HandleStartupProcInterrupts();
					}
					last_fail_time = now;
					currentSource = XLOG_FROM_ARCHIVE;
					break;

				default:
					elog(ERROR, "unexpected WAL source %d", currentSource);
			}
		}
		else if (currentSource == XLOG_FROM_PG_WAL)
		{
			/*
			 * 我们刚刚成功读取了 pg_wal 中的一个文件。我们更偏好归档中的
			 * 文件而非 pg_wal 中的文件，因此下一次先再尝试从
			 * 归档读取。
			 */
			if (InArchiveRecovery)
				currentSource = XLOG_FROM_ARCHIVE;
		}

		if (currentSource != oldSource)
			elog(DEBUG2, "switched WAL source from %s to %s after %s",
				 xlogSourceNames[oldSource], xlogSourceNames[currentSource],
				 lastSourceFailed ? "failure" : "success");

		/*
		 * 现已处理完可能的失败。尝试从选定的
		 * 源读取。
		 */
		lastSourceFailed = false;

		switch (currentSource)
		{
			case XLOG_FROM_ARCHIVE:
			case XLOG_FROM_PG_WAL:

				/*
				 * 从归档或 pg_wal 读取 WAL 时，
				 * WAL 接收器不得运行。
				 */
				Assert(!WalRcvStreaming());

				/* 关闭我们可能打开的任何旧文件。 */
				if (readFile >= 0)
				{
					close(readFile);
					readFile = -1;
				}
				/* 如果是随机获取，则重置 curFileTLI。 */
				if (randAccess)
					curFileTLI = 0;

				/*
				 * 尝试从归档恢复文件，
				 * 或从 pg_wal 读取现有文件。
				 */
				readFile = XLogFileReadAnyTLI(readSegNo, DEBUG2,
											  currentSource == XLOG_FROM_ARCHIVE ? XLOG_FROM_ANY :
											  currentSource);
				if (readFile >= 0)
					return XLREAD_SUCCESS;	/* 成功！ */

				/*
				 * 不，在归档或 pg_wal 中未找到。
				 */
				lastSourceFailed = true;
				break;

			case XLOG_FROM_STREAM:
				{
					bool		havedata;

					/*
					 * 我们应该只能在备库模式下移动到
					 * XLOG_FROM_STREAM。
					 */
					Assert(StandbyMode);

					/*
					 * 首先，
					 * 如果请求了 walreceiver 重启，
					 * 则关闭它 —— 但如果我们已经计划启动它，则没有必要。
					 */
					if (pendingWalRcvRestart && !startWalReceiver)
					{
						XLogShutdownWalRcv();

						/*
						 * 如果我们被请求恢复到最新时间线，
						 * 重新扫描可能的新时间线。
						 */
						if (recoveryTargetTimeLineGoal ==
							RECOVERY_TARGET_TIMELINE_LATEST)
							rescanLatestTimeLine(replayTLI, replayLSN);

						startWalReceiver = true;
					}
					pendingWalRcvRestart = false;

					/*
					 * 如果需要，启动 walreceiver。
					 *
					 * 如果 fetching_ckpt 为真，
					 * RecPtr 指向初始检查点位置。
					 * 在这种情况下，
					 * 我们使用 RedoStartLSN 作为流式传输起始位置而不是 RecPtr，
					 * 这样当我们稍后跳回在 RedoStartLSN 开始重做时，我们已经有了流式传输的日志。
					 */
					if (startWalReceiver &&
						PrimaryConnInfo && strcmp(PrimaryConnInfo, "") != 0)
					{
						XLogRecPtr	ptr;
						TimeLineID	tli;

						if (fetching_ckpt)
						{
							ptr = RedoStartLSN;
							tli = RedoStartTLI;
						}
						else
						{
							ptr = RecPtr;

							/*
							 * 使用记录开始位置来确定 TLI，
							 * 而不是我们正在读取的位置。
							 */
							tli = tliOfPointInHistory(tliRecPtr, expectedTLEs);

							if (curFileTLI > 0 && tli < curFileTLI)
								elog(ERROR, "according to history file, WAL location %X/%X belongs to timeline %u, but previous recovered WAL file came from timeline %u",
									 LSN_FORMAT_ARGS(tliRecPtr),
									 tli, curFileTLI);
						}
						curFileTLI = tli;
						SetInstallXLogFileSegmentActive();
						RequestXLogStreaming(tli, ptr, PrimaryConnInfo,
											 PrimarySlotName,
											 wal_receiver_create_temp_slot);
						flushedUpto = 0;
					}

					/*
					 * 检查 WAL 接收进程是否处于活动状态，或等待其启动。
					 */
					if (!WalRcvStreaming())
					{
						lastSourceFailed = true;
						break;
					}

					/*
					 * Walreceiver 是活动的，所以看看是否有新数据到达。
					 *
					 * 只有当我们从 walreceiver 获得新的 WAL 并且观察到我们已经处理了它刷新到磁盘的最新"块"之前的所有内容时，
					 * 我们才更新XLogReceiptTime。
					 * 在稳定状态下，
					 * 我们跟上传入数据，
					 * XLogReceiptTime 会在每个周期更新。
					 * 当我们落后时，
					 * XLogReceiptTime 不会前进，
					 * 因此分配给冲突查询的宽限时间会减少。
					 */
					if (RecPtr < flushedUpto)
						havedata = true;
					else
					{
						XLogRecPtr	latestChunkStart;

						flushedUpto = GetWalRcvFlushRecPtr(&latestChunkStart, &receiveTLI);
						if (RecPtr < flushedUpto && receiveTLI == curFileTLI)
						{
							havedata = true;
							if (latestChunkStart <= RecPtr)
							{
								XLogReceiptTime = GetCurrentTimestamp();
								SetCurrentChunkStartTime(XLogReceiptTime);
							}
						}
						else
							havedata = false;
					}
					if (havedata)
					{
						/*
						 * 太好了，
						 * 流式传输足够远了。
						 * 如果文件尚未打开则打开它。
						 * 如果我们还没有初始化时间线历史，
						 * 也要读取时间线历史文件；
						 * 它应该已经被流式传输并存在于 pg_wal 中。
						 * 使用 XLOG_FROM_STREAM 以便正确设置源信息且不改变XLogReceiptTime。
						 *
						 * 注意：
						 * 我们必须基于 recoveryTargetTLI 而不是 receiveTLI设置
						 * readTimeLineHistory。
						 * 通常它们相同，
						 * 但如果recovery_target_timeline 是 'latest' 且配置了归档，
						 * 那么我们可能已从归档检索到一个或多个新时间线历史文件，
						 * 更新 recoveryTargetTLI。
						 */
						if (readFile < 0)
						{
							if (!expectedTLEs)
								expectedTLEs = readTimeLineHistory(recoveryTargetTLI);
							readFile = XLogFileRead(readSegNo, PANIC,
													receiveTLI,
													XLOG_FROM_STREAM, false);
							Assert(readFile >= 0);
						}
						else
						{
							/* 只是确保源信息正确... */
							readSource = XLOG_FROM_STREAM;
							XLogReceiptSource = XLOG_FROM_STREAM;
							return XLREAD_SUCCESS;
						}
						break;
					}

					/* 在非阻塞模式下，返回而不是休眠。 */
					if (nonblocking)
						return XLREAD_WOULDBLOCK;

					/*
					 * 数据还没到。检查触发器，然后等待 walreceiver 在新 WAL 到达时
					 * 唤醒我们。
					 */
					if (CheckForStandbyTrigger())
					{
						/*
						 * 注意，
						 * 我们不会在这里立即返回 XLREAD_FAIL。
						 * 被触发后，
						 * 我们仍然想重放所有已经流式传输的 WAL。
						 * 它现在在 pg_wal 中，
						 * 所以我们只是将此视为失败，
						 * 状态机将继续从 pg_wal 重放流式传输的 WAL，然后重新检查触发器并退出重放。
						 */
						lastSourceFailed = true;
						break;
					}

					/*
					 * 由于我们已经重放了目前为止收到的所有内容，
					 * 并且即将开始等待更多 WAL，
					 * 让我们现在告诉上游服务器我们的重放位置，
					 * 以便 pg_stat_replication
					 * 不会显示过时的信息。
					 */
					if (!streaming_reply_sent)
					{
						WalRcvForceReply();
						streaming_reply_sent = true;
					}

					/* 执行可能对我们后续有益的任何后台任务。 */
					KnownAssignedTransactionIdsIdleMaintenance();

					/* 在休眠前更新 pg_stat_recovery_prefetch。 */
					XLogPrefetcherComputeStats(xlogprefetcher);

					/*
					 * 等待更多 WAL 到达，
					 * 届时我们会被 WAL 接收器立即唤醒。
					 */
					(void) WaitLatch(&XLogRecoveryCtl->recoveryWakeupLatch,
									 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
									 -1L,
									 WAIT_EVENT_RECOVERY_WAL_STREAM);
					ResetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
					break;
				}

			default:
				elog(ERROR, "unexpected WAL source %d", currentSource);
		}

		/*
		 * 在这里检查恢复暂停，
		 * 以便我们可以更快地确认请求的暂停已实际生效。
		 */
		if (((volatile XLogRecoveryCtlData *) XLogRecoveryCtl)->recoveryPauseState !=
			RECOVERY_NOT_PAUSED)
			recoveryPausesHere(false);

		/*
		 * 这个可能很长的循环需
		 * 要处理启动进程的中断。
		 */
		HandleStartupProcInterrupts();
	}

	return XLREAD_FAIL;			/* 未到达 */
}


/*
 * 确定应该使用什么日志级别来报告当前 WAL 页面中损坏的 WAL 记录，
 * 该页面之前由 XLogPageRead() 读取。
 *
 * 'emode' 是用于报告文件未找到或合法的 WAL 结束情况的错误模式。
 * 通常，
 * 我们按原样使用它，但如果我们正在重试之前尝试过的完全相同的记录，
 * 只在第一次抱怨以减少噪音。
 * 但是，
 * 我们只在从 pg_wal 读取时这样做，因为我们不期望归档中或从主库流式传输的记录中有任何无效记录。
 * 归档中的文件应该是完整的，
 * 我们永远不会遇到 WAL 结束，因为我们会在重放之前停止并等待更多 WAL 到达。
 *
 * 注意：
 * 此函数记住它上次被调用时的 RecPtr 值，
 * 以抑制关于同一记录的重复消息。
 * 只有在即将 ereport() 时才调用此函数，否则可能会导致后续消息被错误地抑制。
 */
static int
emode_for_corrupt_record(int emode, XLogRecPtr RecPtr)
{
	static XLogRecPtr lastComplaint = 0;

	if (readSource == XLOG_FROM_PG_WAL && emode == LOG)
	{
		if (RecPtr == lastComplaint)
			emode = DEBUG1;
		else
			lastComplaint = RecPtr;
	}
	return emode;
}


/*
 * 尝试获取并验证先前检查点记录的子程序。
 */
static XLogRecord *
ReadCheckpointRecord(XLogPrefetcher *xlogprefetcher, XLogRecPtr RecPtr,
					 TimeLineID replayTLI)
{
	XLogRecord *record;
	uint8		info;

	Assert(xlogreader != NULL);

	if (!XRecOffIsValid(RecPtr))
	{
		ereport(LOG,
				(errmsg("invalid checkpoint location")));
		return NULL;
	}

	XLogPrefetcherBeginRead(xlogprefetcher, RecPtr);
	record = ReadRecord(xlogprefetcher, LOG, true, replayTLI);

	if (record == NULL)
	{
		ereport(LOG,
				(errmsg("invalid checkpoint record")));
		return NULL;
	}
	if (record->xl_rmid != RM_XLOG_ID)
	{
		ereport(LOG,
				(errmsg("invalid resource manager ID in checkpoint record")));
		return NULL;
	}
	info = record->xl_info & ~XLR_INFO_MASK;
	if (info != XLOG_CHECKPOINT_SHUTDOWN &&
		info != XLOG_CHECKPOINT_ONLINE)
	{
		ereport(LOG,
				(errmsg("invalid xl_info in checkpoint record")));
		return NULL;
	}
	if (record->xl_tot_len != SizeOfXLogRecord + SizeOfXLogRecordDataHeaderShort + sizeof(CheckPoint))
	{
		ereport(LOG,
				(errmsg("invalid length of checkpoint record")));
		return NULL;
	}
	return record;
}

/*
 * 扫描自我们开始恢复以来可
 * 能出现在归档中的新时间线。
 *
 * 如果有任何新时间线，
 * 函数将恢复目标 TLI 更改为最新的并返回 'true'。
 */
static bool
rescanLatestTimeLine(TimeLineID replayTLI, XLogRecPtr replayLSN)
{
	List	   *newExpectedTLEs;
	bool		found;
	ListCell   *cell;
	TimeLineID	newtarget;
	TimeLineID	oldtarget = recoveryTargetTLI;
	TimeLineHistoryEntry *currentTle = NULL;

	newtarget = findNewestTimeLine(recoveryTargetTLI);
	if (newtarget == recoveryTargetTLI)
	{
		/* 未找到新时间线 */
		return false;
	}

	/*
	 * 确定新 TLI 的预期 TLIs 列表
	 */

	newExpectedTLEs = readTimeLineHistory(newtarget);

	/*
	 * 如果当前时间线不是新时间线历史的一部分，
	 * 我们无法切换到它。
	 */
	found = false;
	foreach(cell, newExpectedTLEs)
	{
		currentTle = (TimeLineHistoryEntry *) lfirst(cell);

		if (currentTle->tli == recoveryTargetTLI)
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		ereport(LOG,
				(errmsg("new timeline %u is not a child of database system timeline %u",
						newtarget,
						replayTLI)));
		return false;
	}

	/*
	 * 当前时间线在历史文件中找到了，
	 * 但检查下一个时间线是否在当前
	 * 恢复位置*之后*从它分支出来。
	 */
	if (currentTle->end < replayLSN)
	{
		ereport(LOG,
				(errmsg("new timeline %u forked off current database system timeline %u before current recovery point %X/%X",
						newtarget,
						replayTLI,
						LSN_FORMAT_ARGS(replayLSN))));
		return false;
	}

	/* 新时间线历史记录似乎有效。切换目标 */
	recoveryTargetTLI = newtarget;
	list_free_deep(expectedTLEs);
	expectedTLEs = newExpectedTLEs;

	/*
	 * 与 StartupXLOG() 一样，尝试确保我们在 pg_wal 中拥有旧目标和新目标
	 * 之间的所有历史文件。
	 */
	restoreTimeLineHistoryFiles(oldtarget + 1, newtarget);

	ereport(LOG,
			(errmsg("new target timeline is %u",
					recoveryTargetTLI)));

	return true;
}


/*
 * 打开日志文件段以进行读取（在恢复期间）。
 *
 * 如果 source == XLOG_FROM_ARCHIVE，则从归档检索该段。
 * 否则，假设它已经在 pg_wal 中可用。
 */
static int
XLogFileRead(XLogSegNo segno, int emode, TimeLineID tli,
			 XLogSource source, bool notfoundOk)
{
	char		xlogfname[MAXFNAMELEN];
	char		activitymsg[MAXFNAMELEN + 16];
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(xlogfname, tli, segno, wal_segment_size);

	switch (source)
	{
		case XLOG_FROM_ARCHIVE:
			/* 在进程状态显示中报告恢复进度 */
			snprintf(activitymsg, sizeof(activitymsg), "waiting for %s",
					 xlogfname);
			set_ps_display(activitymsg);

			if (!RestoreArchivedFile(path, xlogfname,
									 "RECOVERYXLOG",
									 wal_segment_size,
									 InRedo))
				return -1;
			break;

		case XLOG_FROM_PG_WAL:
		case XLOG_FROM_STREAM:
			XLogFilePath(path, tli, segno, wal_segment_size);
			break;

		default:
			elog(ERROR, "invalid XLogFileRead source %d", source);
	}

	/*
	 * 如果段是从归档存储获取的，
	 * 用归档版本替换现有的 xlog 段（如果有）。
	 */
	if (source == XLOG_FROM_ARCHIVE)
	{
		Assert(!IsInstallXLogFileSegmentActive());
		KeepFileRestoredFromArchive(path, xlogfname);

		/*
		 * 设置 path 指向 pg_wal 中的新文件。
		 */
		snprintf(path, MAXPGPATH, XLOGDIR "/%s", xlogfname);
	}

	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (fd >= 0)
	{
		/* 成功！ */
		curFileTLI = tli;

		/* 在进程状态显示中报告恢复进度 */
		snprintf(activitymsg, sizeof(activitymsg), "recovering %s",
				 xlogfname);
		set_ps_display(activitymsg);

		/* 在各种状态变量中跟踪数据源 */
		readSource = source;
		XLogReceiptSource = source;
		/* 在 FROM_STREAM 情况下，调用者跟踪接收时间，而不是我 */
		if (source != XLOG_FROM_STREAM)
			XLogReceiptTime = GetCurrentTimestamp();

		return fd;
	}
	if (errno != ENOENT || !notfoundOk) /* 意外失败？ */
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	return -1;
}

/*
 * 打开日志文件段以进行读取（在恢复期间）。
 *
 * 此版本搜索 expectedTLEs 中列出的任何 TLI 的段。
 */
static int
XLogFileReadAnyTLI(XLogSegNo segno, int emode, XLogSource source)
{
	char		path[MAXPGPATH];
	ListCell   *cell;
	int			fd;
	List	   *tles;

	/*
	 * 循环查找合适的时间线 ID：我们可能需要读取 expectedTLEs 中列出的
	 * 任何时间线。
	 *
	 * 我们期望入口处的 curFileTLI 是序列中前一个文件的 TLI，
	 * 如果没有前驱则为 0。
	 * 我们不允许 curFileTLI 回退；
	 * 这可以防止当父时间线扩展到比我们要读取
	 * 的子时间线更高的段号时拿到错误的文件。
	 *
	 * 如果我们还没有读取时间线历史文件，
	 * 现在就读取它，以便我们知道要扫描哪些 TLIs。
	 * 但是，
	 * 除非我们实际找到有效段，否则我们不将列表保存在expectedTLEs 中。
	 * 这样，
	 * 如果归档中既没有时间线历史文件也没有 WAL 段，并且设置了流式复制，
	 * 我们将在开始流式传输时读取从主库流式传输的时间线历史文件，而不是用这里生成的虚拟历史恢复。
	 */
	if (expectedTLEs)
		tles = expectedTLEs;
	else
		tles = readTimeLineHistory(recoveryTargetTLI);

	foreach(cell, tles)
	{
		TimeLineHistoryEntry *hent = (TimeLineHistoryEntry *) lfirst(cell);
		TimeLineID	tli = hent->tli;

		if (tli < curFileTLI)
			break;				/* 不要费心查看过旧的 TLIs */

		/*
		 * 跳过扫描要读取的日志文件段不属于的时间线
		 * ID
		 */
		if (hent->begin != InvalidXLogRecPtr)
		{
			XLogSegNo	beginseg = 0;

			XLByteToSeg(hent->begin, beginseg, wal_segment_size);

			/*
			 * 不属于该时间线的日志文件段比该
			 * 时间线开始或结束的段更旧或更新，
			 * 分别。
			 * 这里只检查时间线的起始段就足够了。
			 * 由于在此循环中时间线是按降序扫描的，
			 * 任何比结束段更新的段应该属于更新的时间线，
			 * 并且已经在之前被读取。
			 * 所以这里没有必要检查时间线的结束段。
			 */
			if (segno < beginseg)
				continue;
		}

		if (source == XLOG_FROM_ANY || source == XLOG_FROM_ARCHIVE)
		{
			fd = XLogFileRead(segno, emode, tli,
							  XLOG_FROM_ARCHIVE, true);
			if (fd != -1)
			{
				elog(DEBUG1, "got WAL segment from archive");
				if (!expectedTLEs)
					expectedTLEs = tles;
				return fd;
			}
		}

		if (source == XLOG_FROM_ANY || source == XLOG_FROM_PG_WAL)
		{
			fd = XLogFileRead(segno, emode, tli,
							  XLOG_FROM_PG_WAL, true);
			if (fd != -1)
			{
				if (!expectedTLEs)
					expectedTLEs = tles;
				return fd;
			}
		}
	}

	/* 找不到它。为简单起见，抱怨前端时间线 */
	XLogFilePath(path, recoveryTargetTLI, segno, wal_segment_size);
	errno = ENOENT;
	ereport(emode,
			(errcode_for_file_access(),
			 errmsg("could not open file \"%s\": %m", path)));
	return -1;
}

/*
 * 设置标志以通知 walreceiver 重启。（启动进程在注意到相关配置更改时
 * 调用此函数。）
 */
void
StartupRequestWalReceiverRestart(void)
{
	if (currentSource == XLOG_FROM_STREAM && WalRcvRunning())
	{
		ereport(LOG,
				(errmsg("WAL receiver process shutdown requested")));

		pendingWalRcvRestart = true;
	}
}


/*
 * 备库提升是否已被触发？
 *
 * 与 CheckForStandbyTrigger() 不同，
 * 这适用于任何连接到共享内存的进程。
 */
bool
PromoteIsTriggered(void)
{
	/*
	 * 我们每次都检查共享状态，
	 * 直到备库提升被触发。我们不能再次触发提升，
	 * 所以一旦共享变量被看到为真，就没有必要继续检查。
	 */
	if (LocalPromoteIsTriggered)
		return true;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	LocalPromoteIsTriggered = XLogRecoveryCtl->SharedPromoteIsTriggered;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return LocalPromoteIsTriggered;
}

static void
SetPromoteIsTriggered(void)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->SharedPromoteIsTriggered = true;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	/*
	 * 将恢复暂停状态标记为"未暂停"，
	 * 因为如果在恢复暂停时触发了提升，
	 * 暂停状态结束并继续提升。
	 * 否则 pg_get_wal_replay_pause_state() 可能在提升进行中时错误地返回"暂停"。
	 */
	SetRecoveryPause(false);

	LocalPromoteIsTriggered = true;
}

/*
 * 检查是否已到达提升请求。
 */
static bool
CheckForStandbyTrigger(void)
{
	if (LocalPromoteIsTriggered)
		return true;

	if (IsPromoteSignaled() && CheckPromoteSignal())
	{
		ereport(LOG, (errmsg("received promote request")));
		RemovePromoteSignalFiles();
		ResetPromoteSignaled();
		SetPromoteIsTriggered();
		return true;
	}

	return false;
}

/*
 * 删除信号通知备库提升请求的文件。
 */
void
RemovePromoteSignalFiles(void)
{
	unlink(PROMOTE_SIGNAL_FILE);
}

/*
 * 检查提升请求是否已到达。
 */
bool
CheckPromoteSignal(void)
{
	struct stat stat_buf;

	if (stat(PROMOTE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * 唤醒启动进程以重放新到达的 WAL，
 * 或注意到已请求故障切换。
 */
void
WakeupRecovery(void)
{
	SetLatch(&XLogRecoveryCtl->recoveryWakeupLatch);
}

/*
 * 在主恢复循环中安排 walreceiver 唤醒。
 */
void
XLogRequestWalReceiverReply(void)
{
	doRequestWalReceiverReply = true;
}

/*
 * HotStandby 是否已激活？这仅在特殊后端中重要，因为普通后端在此
 * 返回 true 之前永远无法连接。Postmaster 通过信号得知这一点，
 * 而非通过共享内存。
 *
 * 与测试 standbyState 不同，这适用于任何已连接到
 * 共享内存的进程。（并且请注意，仅看 standbyState 本身
 * 也无法反映真实情况。）
 */
bool
HotStandbyActive(void)
{
	/*
	 * 我们每次都检查共享状态，
	 * 直到热备激活。我们不能停用热备，
	 * 所以一旦共享变量被看到为真，就没有必要继续检查。
	 */
	if (LocalHotStandbyActive)
		return true;
	else
	{
		/* 自旋锁在内存排序较弱的机器上是必需的！ */
		SpinLockAcquire(&XLogRecoveryCtl->info_lck);
		LocalHotStandbyActive = XLogRecoveryCtl->SharedHotStandbyActive;
		SpinLockRelease(&XLogRecoveryCtl->info_lck);

		return LocalHotStandbyActive;
	}
}

/*
 * 类似于 HotStandbyActive()，但仅在 WAL 重放代码中使用，
 * 我们不需要询问任何其他进程状态是什么。
 */
static bool
HotStandbyActiveInReplay(void)
{
	Assert(AmStartupProcess() || !IsPostmasterEnvironment);
	return LocalHotStandbyActive;
}

/*
 * 获取最新的重做应用位置。
 *
 * 导出以允许 WALReceiver 直接读取指针。
 */
XLogRecPtr
GetXLogReplayRecPtr(TimeLineID *replayTLI)
{
	XLogRecPtr	recptr;
	TimeLineID	tli;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	recptr = XLogRecoveryCtl->lastReplayedEndRecPtr;
	tli = XLogRecoveryCtl->lastReplayedTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (replayTLI)
		*replayTLI = tli;
	return recptr;
}


/*
 * 获取最后应用或正在应用的记录的位置。
 *
 * 这与 GetXLogReplayRecPtr() 不同，如果当前正在应用 WAL 记录，
 * 这包括该记录。
 */
XLogRecPtr
GetCurrentReplayRecPtr(TimeLineID *replayEndTLI)
{
	XLogRecPtr	recptr;
	TimeLineID	tli;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	recptr = XLogRecoveryCtl->replayEndRecPtr;
	tli = XLogRecoveryCtl->replayEndTLI;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	if (replayEndTLI)
		*replayEndTLI = tli;
	return recptr;
}

/*
 * 保存最后处理的提交/中止记录的时间戳。
 *
 * 我们将其保存在 XLogRecoveryCtl 中，而不是简单的静态变量，
 * 以便启动进程以外的进程可以看到它。特别要注意，
 * CreateRestartPoint 是在检查点进程中执行的。
 */
static void
SetLatestXTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->recoveryLastXTime = xtime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}

/*
 * 获取最后处理的提交/中止记录的时间戳。
 */
TimestampTz
GetLatestXTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	xtime = XLogRecoveryCtl->recoveryLastXTime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return xtime;
}

/*
 * 保存下一块 WAL 记录应用的时间戳。
 *
 * 我们将其保存在 XLogRecoveryCtl 中，而不是简单的静态变量，
 * 以便所有后端都可以看到它。
 */
static void
SetCurrentChunkStartTime(TimestampTz xtime)
{
	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	XLogRecoveryCtl->currentChunkStartTime = xtime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);
}

/*
 * 获取最后处理的提交/中止记录的时间戳。
 * 启动进程在 XLogReceiptTime 中维护准确的本地副本
 */
TimestampTz
GetCurrentChunkReplayStartTime(void)
{
	TimestampTz xtime;

	SpinLockAcquire(&XLogRecoveryCtl->info_lck);
	xtime = XLogRecoveryCtl->currentChunkStartTime;
	SpinLockRelease(&XLogRecoveryCtl->info_lck);

	return xtime;
}

/*
 * 返回当前 XLOG 数据块的接收时间，
 * 以及它是从流式复制还是从归档接收的。
 */
void
GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream)
{
	/*
	 * 这必须在启动进程中执行，
	 * 因为我们不将相关状态导出到共享内存。
	 */
	Assert(InRecovery);

	*rtime = XLogReceiptTime;
	*fromStream = (XLogReceiptSource == XLOG_FROM_STREAM);
}

/*
 * 注意，
 * 提供的文本字段是参数名，不需要翻译
 */
void
RecoveryRequiresIntParameter(const char *param_name, int currValue, int minValue)
{
	if (currValue < minValue)
	{
		if (HotStandbyActiveInReplay())
		{
			bool		warned_for_promote = false;

			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("hot standby is not possible because of insufficient parameter settings"),
					 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
							   param_name,
							   currValue,
							   minValue)));

			SetRecoveryPause(true);

			ereport(LOG,
					(errmsg("recovery has paused"),
					 errdetail("If recovery is unpaused, the server will shut down."),
					 errhint("You can then restart the server after making the necessary configuration changes.")));

			while (GetRecoveryPauseState() != RECOVERY_NOT_PAUSED)
			{
				HandleStartupProcInterrupts();

				if (CheckForStandbyTrigger())
				{
					if (!warned_for_promote)
						ereport(WARNING,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("promotion is not possible because of insufficient parameter settings"),

						/*
						 * 重复上面的详细信息，
						 * 以便在日志中容易找到。
						 */
								 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
										   param_name,
										   currValue,
										   minValue),
								 errhint("Restart the server after making the necessary configuration changes.")));
					warned_for_promote = true;
				}

				/*
				 * 如果请求了恢复暂停，
				 * 则将其设置为已暂停。当我们处于循环中时，
				 * 用户可能会恢复并再次暂停，所以每次都设置这个。
				 */
				ConfirmRecoveryPaused();

				/*
				 * 我们等待一个条件变量，
				 * 它会在暂停结束时立即唤醒我们，
				 * 但我们使用超时以便也能定期检查上述条件。
				 */
				ConditionVariableTimedSleep(&XLogRecoveryCtl->recoveryNotPausedCV, 1000,
											WAIT_EVENT_RECOVERY_PAUSE);
			}
			ConditionVariableCancelSleep();
		}

		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("recovery aborted because of insufficient parameter settings"),
		/* 重复上面的详细信息，以便在日志中容易找到。 */
				 errdetail("%s = %d is a lower setting than on the primary server, where its value was %d.",
						   param_name,
						   currValue,
						   minValue),
				 errhint("You can restart the server after making the necessary configuration changes.")));
	}
}


/*
 * primary_slot_name 的 GUC check_hook
 */
bool
check_primary_slot_name(char **newval, void **extra, GucSource source)
{
	int			err_code;
	char	   *err_msg = NULL;
	char	   *err_hint = NULL;

	if (*newval && strcmp(*newval, "") != 0 &&
		!ReplicationSlotValidateNameInternal(*newval, &err_code, &err_msg,
											 &err_hint))
	{
		GUC_check_errcode(err_code);
		GUC_check_errdetail("%s", err_msg);
		if (err_hint != NULL)
			GUC_check_errhint("%s", err_hint);
		return false;
	}

	return true;
}

/*
 * 恢复目标设置：
 * 多个 recovery_target* 设置中只能设置一个。
 * 设置第二个会导致错误。
 * 全局变量 recoveryTarget 跟踪选择了哪种恢复目标。
 * 其他变量存储实际的目标值（例如字符串或 xid）。
 * 参数的 assign 函数检查是否已设置竞争参数。
 * 但我们想允许多次设置同一参数。
 * 我们还想允许取消设置一个参数并设置不同的参数，
 * 所以当参数设置为空字符串时，我们取消设置 recoveryTarget。
 *
 * XXX 此代码在设计上是有问题的。
 * 从 GUC assign 钩子抛出错误违反了 guc.c 的基本假设。只要所有可能发生这种情况的变量都是 PGC_POSTMASTER，
 * 后果是有限的，
 * 因为我们只会中止 postmaster 启动。尽管如此，
 * 我们可能有奇怪的行为，例如意外的 GUC 排序依赖。
 */

static void
pg_attribute_noreturn()
error_multiple_recovery_targets(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("multiple recovery targets specified"),
			 errdetail("At most one of recovery_target, recovery_target_lsn, recovery_target_name, recovery_target_time, recovery_target_xid may be set.")));
}

/*
 * recovery_target 的 GUC check_hook
 */
bool
check_recovery_target(char **newval, void **extra, GucSource source)
{
	if (strcmp(*newval, "immediate") != 0 && strcmp(*newval, "") != 0)
	{
		GUC_check_errdetail("The only allowed value is \"immediate\".");
		return false;
	}
	return true;
}

/*
 * recovery_target 的 GUC assign_hook
 */
void
assign_recovery_target(const char *newval, void *extra)
{
	if (recoveryTarget != RECOVERY_TARGET_UNSET &&
		recoveryTarget != RECOVERY_TARGET_IMMEDIATE)
		error_multiple_recovery_targets();

	if (newval && strcmp(newval, "") != 0)
		recoveryTarget = RECOVERY_TARGET_IMMEDIATE;
	else
		recoveryTarget = RECOVERY_TARGET_UNSET;
}

/*
 * recovery_target_lsn 的 GUC check_hook
 */
bool
check_recovery_target_lsn(char **newval, void **extra, GucSource source)
{
	if (strcmp(*newval, "") != 0)
	{
		XLogRecPtr	lsn;
		XLogRecPtr *myextra;
		bool		have_error = false;

		lsn = pg_lsn_in_internal(*newval, &have_error);
		if (have_error)
			return false;

		myextra = (XLogRecPtr *) guc_malloc(ERROR, sizeof(XLogRecPtr));
		*myextra = lsn;
		*extra = (void *) myextra;
	}
	return true;
}

/*
 * recovery_target_lsn 的 GUC assign_hook
 */
void
assign_recovery_target_lsn(const char *newval, void *extra)
{
	if (recoveryTarget != RECOVERY_TARGET_UNSET &&
		recoveryTarget != RECOVERY_TARGET_LSN)
		error_multiple_recovery_targets();

	if (newval && strcmp(newval, "") != 0)
	{
		recoveryTarget = RECOVERY_TARGET_LSN;
		recoveryTargetLSN = *((XLogRecPtr *) extra);
	}
	else
		recoveryTarget = RECOVERY_TARGET_UNSET;
}

/*
 * recovery_target_name 的 GUC check_hook
 */
bool
check_recovery_target_name(char **newval, void **extra, GucSource source)
{
	/* 使用 newval 的值 */
	if (strlen(*newval) >= MAXFNAMELEN)
	{
		GUC_check_errdetail("%s is too long (maximum %d characters).",
							"recovery_target_name", MAXFNAMELEN - 1);
		return false;
	}
	return true;
}

/*
 * recovery_target_name 的 GUC assign_hook
 */
void
assign_recovery_target_name(const char *newval, void *extra)
{
	if (recoveryTarget != RECOVERY_TARGET_UNSET &&
		recoveryTarget != RECOVERY_TARGET_NAME)
		error_multiple_recovery_targets();

	if (newval && strcmp(newval, "") != 0)
	{
		recoveryTarget = RECOVERY_TARGET_NAME;
		recoveryTargetName = newval;
	}
	else
		recoveryTarget = RECOVERY_TARGET_UNSET;
}

/*
 * recovery_target_time 的 GUC check_hook
 *
 * recovery_target_time
 * 字符串的解释可能取决于时区设置，
 * 所以我们需要等到所有 GUC 处理完成后才能进行字符串的最终解析。
 * 此检查函数只进行解析遍历以捕获语法错误，
 * 但我们存储字符串并在需要使用时再次解析它。
 */
bool
check_recovery_target_time(char **newval, void **extra, GucSource source)
{
	if (strcmp(*newval, "") != 0)
	{
		/* 拒绝一些特殊值 */
		if (strcmp(*newval, "now") == 0 ||
			strcmp(*newval, "today") == 0 ||
			strcmp(*newval, "tomorrow") == 0 ||
			strcmp(*newval, "yesterday") == 0)
		{
			return false;
		}

		/*
		 * 解析时间戳值（另请参阅 timestamptz_in()）
		 */
		{
			char	   *str = *newval;
			fsec_t		fsec;
			struct pg_tm tt,
					   *tm = &tt;
			int			tz;
			int			dtype;
			int			nf;
			int			dterr;
			char	   *field[MAXDATEFIELDS];
			int			ftype[MAXDATEFIELDS];
			char		workbuf[MAXDATELEN + MAXDATEFIELDS];
			DateTimeErrorExtra dtextra;
			TimestampTz timestamp;

			dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
								  field, ftype, MAXDATEFIELDS, &nf);
			if (dterr == 0)
				dterr = DecodeDateTime(field, ftype, nf,
									   &dtype, tm, &fsec, &tz, &dtextra);
			if (dterr != 0)
				return false;
			if (dtype != DTK_DATE)
				return false;

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
			{
				GUC_check_errdetail("timestamp out of range: \"%s\"", str);
				return false;
			}
		}
	}
	return true;
}

/*
 * recovery_target_time 的 GUC assign_hook
 */
void
assign_recovery_target_time(const char *newval, void *extra)
{
	if (recoveryTarget != RECOVERY_TARGET_UNSET &&
		recoveryTarget != RECOVERY_TARGET_TIME)
		error_multiple_recovery_targets();

	if (newval && strcmp(newval, "") != 0)
		recoveryTarget = RECOVERY_TARGET_TIME;
	else
		recoveryTarget = RECOVERY_TARGET_UNSET;
}

/*
 * recovery_target_timeline 的 GUC check_hook
 */
bool
check_recovery_target_timeline(char **newval, void **extra, GucSource source)
{
	RecoveryTargetTimeLineGoal rttg;
	RecoveryTargetTimeLineGoal *myextra;

	if (strcmp(*newval, "current") == 0)
		rttg = RECOVERY_TARGET_TIMELINE_CONTROLFILE;
	else if (strcmp(*newval, "latest") == 0)
		rttg = RECOVERY_TARGET_TIMELINE_LATEST;
	else
	{
		rttg = RECOVERY_TARGET_TIMELINE_NUMERIC;

		errno = 0;
		strtoul(*newval, NULL, 0);
		if (errno == EINVAL || errno == ERANGE)
		{
			GUC_check_errdetail("recovery_target_timeline is not a valid number.");
			return false;
		}
	}

	myextra = (RecoveryTargetTimeLineGoal *) guc_malloc(ERROR, sizeof(RecoveryTargetTimeLineGoal));
	*myextra = rttg;
	*extra = (void *) myextra;

	return true;
}

/*
 * recovery_target_timeline 的 GUC assign_hook
 */
void
assign_recovery_target_timeline(const char *newval, void *extra)
{
	recoveryTargetTimeLineGoal = *((RecoveryTargetTimeLineGoal *) extra);
	if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_NUMERIC)
		recoveryTargetTLIRequested = (TimeLineID) strtoul(newval, NULL, 0);
	else
		recoveryTargetTLIRequested = 0;
}

/*
 * recovery_target_xid 的 GUC check_hook
 */
bool
check_recovery_target_xid(char **newval, void **extra, GucSource source)
{
	if (strcmp(*newval, "") != 0)
	{
		TransactionId xid;
		TransactionId *myextra;

		errno = 0;
		xid = (TransactionId) strtou64(*newval, NULL, 0);
		if (errno == EINVAL || errno == ERANGE)
			return false;

		myextra = (TransactionId *) guc_malloc(ERROR, sizeof(TransactionId));
		*myextra = xid;
		*extra = (void *) myextra;
	}
	return true;
}

/*
 * recovery_target_xid 的 GUC assign_hook
 */
void
assign_recovery_target_xid(const char *newval, void *extra)
{
	if (recoveryTarget != RECOVERY_TARGET_UNSET &&
		recoveryTarget != RECOVERY_TARGET_XID)
		error_multiple_recovery_targets();

	if (newval && strcmp(newval, "") != 0)
	{
		recoveryTarget = RECOVERY_TARGET_XID;
		recoveryTargetXid = *((TransactionId *) extra);
	}
	else
		recoveryTarget = RECOVERY_TARGET_UNSET;
}
