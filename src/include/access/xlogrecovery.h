/*
 * xlogrecovery.h
 *
 * WAL 恢复与备库模式相关函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecovery.h
 */
#ifndef XLOGRECOVERY_H
#define XLOGRECOVERY_H

#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "lib/stringinfo.h"
#include "utils/timestamp.h"

/*
 * 恢复目标类型。
 * 仅在时间点恢复期间设置，备库模式下不设置。
 */
typedef enum
{
	RECOVERY_TARGET_UNSET,
	RECOVERY_TARGET_XID,
	RECOVERY_TARGET_TIME,
	RECOVERY_TARGET_NAME,
	RECOVERY_TARGET_LSN,
	RECOVERY_TARGET_IMMEDIATE
} RecoveryTargetType;

/*
 * 恢复目标 TimeLine 目标
 */
typedef enum
{
	RECOVERY_TARGET_TIMELINE_CONTROLFILE,
	RECOVERY_TARGET_TIMELINE_LATEST,
	RECOVERY_TARGET_TIMELINE_NUMERIC
} RecoveryTargetTimeLineGoal;

/* 恢复暂停状态 */
typedef enum RecoveryPauseState
{
	RECOVERY_NOT_PAUSED,		/* 未请求暂停 */
	RECOVERY_PAUSE_REQUESTED,	/* 已请求暂停，但尚未暂停 */
	RECOVERY_PAUSED				/* 恢复已暂停 */
} RecoveryPauseState;

/* 用户可设置的GUC 参数 */
extern PGDLLIMPORT bool recoveryTargetInclusive;
extern PGDLLIMPORT int recoveryTargetAction;
extern PGDLLIMPORT int recovery_min_apply_delay;
extern PGDLLIMPORT char *PrimaryConnInfo;
extern PGDLLIMPORT char *PrimarySlotName;
extern PGDLLIMPORT char *recoveryRestoreCommand;
extern PGDLLIMPORT char *recoveryEndCommand;
extern PGDLLIMPORT char *archiveCleanupCommand;

/* 由 GUC 系统间接设定 */
extern PGDLLIMPORT TransactionId recoveryTargetXid;
extern PGDLLIMPORT char *recovery_target_time_string;
extern PGDLLIMPORT TimestampTz recoveryTargetTime;
extern PGDLLIMPORT const char *recoveryTargetName;
extern PGDLLIMPORT XLogRecPtr recoveryTargetLSN;
extern PGDLLIMPORT RecoveryTargetType recoveryTarget;
extern PGDLLIMPORT bool wal_receiver_create_temp_slot;
extern PGDLLIMPORT RecoveryTargetTimeLineGoal recoveryTargetTimeLineGoal;
extern PGDLLIMPORT TimeLineID recoveryTargetTLIRequested;
extern PGDLLIMPORT TimeLineID recoveryTargetTLI;

/* 我们是否已达一致的数据库状态？ */
extern PGDLLIMPORT bool reachedConsistency;

/* 当前是否处于备库模式？ */
extern PGDLLIMPORT bool StandbyMode;

extern Size XLogRecoveryShmemSize(void);
extern void XLogRecoveryShmemInit(void);

extern void InitWalRecovery(ControlFileData *ControlFile,
							bool *wasShutdown_ptr, bool *haveBackupLabel_ptr,
							bool *haveTblspcMap_ptr);
extern void PerformWalRecovery(void);

/*
 * FinishWalRecovery() 返回此结构。其中含恢复结束
 * 位置及结束原因的信息。
 */
typedef struct
{
	/*
	 * 关于末条有效或已应用记录的信息，新 WAL 可在
	 * 其后追加。'lastRec' 是末条记录的起始位置，
	 * 'endOfLog' 是其终点。'lastPage' 是最后部分
	 * 页的副本，该页含 endOfLog（若 endOfLog 恰在
	 * 页边界则为 NULL）。'lastPageBeginPtr' 是末页之
	 * 始。
	 *
	 * endOfLogTLI 是含最后已应用记录的 XLOG 段文件名中的 
	 * TLI。它可能不同于 lastRecTLI，若该段中有
	 * 时间线切换，且我们当时正从属于更高时间
	 * 线的段中读取旧 WAL。
	 */
	XLogRecPtr	lastRec;		/* 末条有效或已应用记录起 */
	TimeLineID	lastRecTLI;
	XLogRecPtr	endOfLog;		/* 末条有效或已应用记录终 */
	TimeLineID	endOfLogTLI;

	XLogRecPtr	lastPageBeginPtr;	/* 含 endOfLog 的页之 LSN */
	char	   *lastPage;		/* 最后一页副本，直至 endOfLog */

	/*
	 * abortedRecPtr 是 WAL 结束处损坏记录的起始指针，
	 * 在恢复完成时设置；missingContrecPtr 是第一个丢失的 
	 * contrecord 的位置。见 CreateOverwriteContrecordRecord 之详
	 * 情。
	 */
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;

	/* 描述恢复为何结束的人类可读短串 */
	char	   *recoveryStopReason;

	/*
	 * 若存在 standby 或恢复信号文件，则这些标志相应
	 * 地设置。
	 */
	bool		standby_signal_file_found;
	bool		recovery_signal_file_found;
} EndOfWalRecoveryInfo;

extern EndOfWalRecoveryInfo *FinishWalRecovery(void);
extern void ShutdownWalRecovery(void);
extern void RemovePromoteSignalFiles(void);

extern bool HotStandbyActive(void);
extern XLogRecPtr GetXLogReplayRecPtr(TimeLineID *replayTLI);
extern RecoveryPauseState GetRecoveryPauseState(void);
extern void SetRecoveryPause(bool recoveryPause);
extern void GetXLogReceiptTime(TimestampTz *rtime, bool *fromStream);
extern TimestampTz GetLatestXTime(void);
extern TimestampTz GetCurrentChunkReplayStartTime(void);
extern XLogRecPtr GetCurrentReplayRecPtr(TimeLineID *replayEndTLI);

extern bool PromoteIsTriggered(void);
extern bool CheckPromoteSignal(void);
extern void WakeupRecovery(void);

extern void StartupRequestWalReceiverRestart(void);
extern void XLogRequestWalReceiverReply(void);

extern void RecoveryRequiresIntParameter(const char *param_name, int currValue, int minValue);

extern void xlog_outdesc(StringInfo buf, XLogReaderState *record);

#endif							/* XLOGRECOVERY_H */
