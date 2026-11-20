/*
 * xlog.h
 *
 * PostgreSQL 预写日志管理器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog.h
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/xlogbackup.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"


/* 同步方法 */
#define SYNC_METHOD_FSYNC		0
#define SYNC_METHOD_FDATASYNC	1
#define SYNC_METHOD_OPEN		2	/* 供 O_SYNC */
#define SYNC_METHOD_FSYNC_WRITETHROUGH	3
#define SYNC_METHOD_OPEN_DSYNC	4	/* 供 O_DSYNC */
extern PGDLLIMPORT int sync_method;

extern PGDLLIMPORT XLogRecPtr ProcLastRecPtr;
extern PGDLLIMPORT XLogRecPtr XactLastRecEnd;
extern PGDLLIMPORT XLogRecPtr XactLastCommitEnd;

/* 这些变量是与 XLOG 相关的 GUC 参数 */
extern PGDLLIMPORT int wal_segment_size;
extern PGDLLIMPORT int min_wal_size_mb;
extern PGDLLIMPORT int max_wal_size_mb;
extern PGDLLIMPORT int wal_keep_size_mb;
extern PGDLLIMPORT int max_slot_wal_keep_size_mb;
extern PGDLLIMPORT int XLOGbuffers;
extern PGDLLIMPORT int XLogArchiveTimeout;
extern PGDLLIMPORT int wal_retrieve_retry_interval;
extern PGDLLIMPORT char *XLogArchiveCommand;
extern PGDLLIMPORT bool EnableHotStandby;
extern PGDLLIMPORT bool fullPageWrites;
extern PGDLLIMPORT bool wal_log_hints;
extern PGDLLIMPORT int wal_compression;
extern PGDLLIMPORT bool wal_init_zero;
extern PGDLLIMPORT bool wal_recycle;
extern PGDLLIMPORT bool *wal_consistency_checking;
extern PGDLLIMPORT char *wal_consistency_checking_string;
extern PGDLLIMPORT bool log_checkpoints;
extern PGDLLIMPORT bool track_wal_io_timing;
extern PGDLLIMPORT int wal_decode_buffer_size;

extern PGDLLIMPORT int CheckPointSegments;

/* 归档模式 */
typedef enum ArchiveMode
{
	ARCHIVE_MODE_OFF = 0,		/* 禁用 */
	ARCHIVE_MODE_ON,			/* 服务器正常运行时启用 */
	ARCHIVE_MODE_ALWAYS			/* 始终启用(即使在恢复期间) */
} ArchiveMode;
extern PGDLLIMPORT int XLogArchiveMode;

/* WAL 级别 */
typedef enum WalLevel
{
	WAL_LEVEL_MINIMAL = 0,
	WAL_LEVEL_REPLICA,
	WAL_LEVEL_LOGICAL
} WalLevel;

/* WAL 的压缩算法 */
typedef enum WalCompression
{
	WAL_COMPRESSION_NONE = 0,
	WAL_COMPRESSION_PGLZ,
	WAL_COMPRESSION_LZ4,
	WAL_COMPRESSION_ZSTD
} WalCompression;

/* 恢复状态 */
typedef enum RecoveryState
{
	RECOVERY_STATE_CRASH = 0,	/* 崩溃恢复 */
	RECOVERY_STATE_ARCHIVE,		/* 归档恢复 */
	RECOVERY_STATE_DONE			/* 当前处于生产中 */
} RecoveryState;

extern PGDLLIMPORT int wal_level;

/* WAL 归档是否启用（始终，或仅在服务器正常运行时）？ */
#define XLogArchivingActive() \
	(AssertMacro(XLogArchiveMode == ARCHIVE_MODE_OFF || wal_level >= WAL_LEVEL_REPLICA), XLogArchiveMode > ARCHIVE_MODE_OFF)
/* WAL 归档是否始终启用(即使在恢复期间)？ */
#define XLogArchivingAlways() \
	(AssertMacro(XLogArchiveMode == ARCHIVE_MODE_OFF || wal_level >= WAL_LEVEL_REPLICA), XLogArchiveMode == ARCHIVE_MODE_ALWAYS)

/*
 * 归档或日志传送是否必须记 WAL，或能否在提交
 * 前 fsync() 数据从而跳过记 WAL？
 */
#define XLogIsNeeded() (wal_level >= WAL_LEVEL_REPLICA)

/*
 * 提示位更新是否需要整页映像？
 *
 * 通常我们不把提示位更新记入 WAL，但若启用了校验和，
 * 必须防范撕裂页写入。当只设置页上的个别位时，
 * 无论哪些位组合落到磁盘上都仍一致，但校验和会
 * 对不上。若被 wal_log_hints=on 强制，也要把它们
 * 记入 WAL。
 */
#define XLogHintBitIsNeeded() (DataChecksumsEnabled() || wal_log_hints)

/* 是否需要把仅热备与逻辑复制所需的信息记入 WAL？ */
#define XLogStandbyInfoActive() (wal_level >= WAL_LEVEL_REPLICA)

/* 是否需要把仅逻辑复制所需的信息记入 WAL？ */
#define XLogLogicalInfoActive() (wal_level >= WAL_LEVEL_LOGICAL)

#ifdef WAL_DEBUG
extern PGDLLIMPORT bool XLOG_DEBUG;
#endif

/*
 * 检查点可 OR 的请求标志位。"原因" 位仅用于
 * 日志记录。注意：标志必须定义得可以合理地把
 * 来自不同请求方的请求标志 OR 在一起。
 */

/* 这些直接影响 CreateCheckPoint 及其附属的行为 */
#define CHECKPOINT_IS_SHUTDOWN	0x0001	/* 关闭检查点 */
#define CHECKPOINT_END_OF_RECOVERY	0x0002	/* 类似关闭检查点，但
											 * 在 WAL 恢复结束时发出 */
#define CHECKPOINT_IMMEDIATE	0x0004	/* 不加延迟 */
#define CHECKPOINT_FORCE		0x0008	/* 即使无活动也强制 */
#define CHECKPOINT_FLUSH_ALL	0x0010	/* 刷写所有页，包括属于
										 * 未日志表的页 */
/* 这些对 RequestCheckpoint 很重要 */
#define CHECKPOINT_WAIT			0x0020	/* 等待完成 */
#define CHECKPOINT_REQUESTED	0x0040	/* 已发出检查点请求 */
/* 这些表示检查点请求的原因 */
#define CHECKPOINT_CAUSE_XLOG	0x0080	/* XLOG 消耗 */
#define CHECKPOINT_CAUSE_TIME	0x0100	/* 已过时间 */

/*
 * 正在插入的记录的标志位，用 XLogSetRecordFlags() 设置。
 */
#define XLOG_INCLUDE_ORIGIN		0x01	/* 包含复制源 */
#define XLOG_MARK_UNIMPORTANT	0x02	/* 记录对持久性不重要 */


/* 检查点统计 */
typedef struct CheckpointStatsData
{
	TimestampTz ckpt_start_t;	/* 检查点开始 */
	TimestampTz ckpt_write_t;	/* 开始刷写缓冲区 */
	TimestampTz ckpt_sync_t;	/* 开始 fsync */
	TimestampTz ckpt_sync_end_t;	/* fsync 结束 */
	TimestampTz ckpt_end_t;		/* 检查点结束 */

	int			ckpt_bufs_written;	/* 已写缓冲区数 */

	int			ckpt_segs_added;	/* 新创建的 xlog 段数 */
	int			ckpt_segs_removed;	/* 已删 xlog 段数 */
	int			ckpt_segs_recycled; /* 已回收 xlog 段数 */

	int			ckpt_sync_rels; /* 已同步关系数 */
	uint64		ckpt_longest_sync;	/* 单关系最长同步时间 */
	uint64		ckpt_agg_sync_time; /* 各次单独同步
									 * 时间之和，不一定等于
									 * 整个同步阶段的
									 * 总耗时。 */
} CheckpointStatsData;

extern PGDLLIMPORT CheckpointStatsData CheckpointStats;

/*
 * GetWALAvailability 的返回码
 */
typedef enum WALAvailability
{
	WALAVAIL_INVALID_LSN,		/* 参数错误 */
	WALAVAIL_RESERVED,			/* WAL 段在 max_wal_size 以内 */
	WALAVAIL_EXTENDED,			/* WAL 段由槽保留，或 
								 * wal_keep_size */
	WALAVAIL_UNRESERVED,		/* 不再保留，但尚未移除 */
	WALAVAIL_REMOVED			/* WAL 段已被移除 */
} WALAvailability;

struct XLogRecData;
struct XLogReaderState;

extern XLogRecPtr XLogInsertRecord(struct XLogRecData *rdata,
								   XLogRecPtr fpw_lsn,
								   uint8 flags,
								   int num_fpi,
								   bool topxid_included);
extern void XLogFlush(XLogRecPtr record);
extern bool XLogBackgroundFlush(void);
extern bool XLogNeedsFlush(XLogRecPtr record);
extern int	XLogFileInit(XLogSegNo logsegno, TimeLineID logtli);
extern int	XLogFileOpen(XLogSegNo segno, TimeLineID tli);

extern void CheckXLogRemoved(XLogSegNo segno, TimeLineID tli);
extern XLogSegNo XLogGetLastRemovedSegno(void);
extern void XLogSetAsyncXactLSN(XLogRecPtr asyncXactLSN);
extern void XLogSetReplicationSlotMinimumLSN(XLogRecPtr lsn);

extern void xlog_redo(struct XLogReaderState *record);
extern void xlog_desc(StringInfo buf, struct XLogReaderState *record);
extern const char *xlog_identify(uint8 info);

extern void issue_xlog_fsync(int fd, XLogSegNo segno, TimeLineID tli);

extern bool RecoveryInProgress(void);
extern RecoveryState GetRecoveryState(void);
extern bool XLogInsertAllowed(void);
extern XLogRecPtr GetXLogInsertRecPtr(void);
extern XLogRecPtr GetXLogWriteRecPtr(void);

extern uint64 GetSystemIdentifier(void);
extern char *GetMockAuthenticationNonce(void);
extern bool DataChecksumsEnabled(void);
extern XLogRecPtr GetFakeLSNForUnloggedRel(void);
extern Size XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void BootStrapXLOG(void);
extern void InitializeWalConsistencyChecking(void);
extern void LocalProcessControlFile(bool reset);
extern WalLevel GetActiveWalLevelOnStandby(void);
extern void StartupXLOG(void);
extern void ShutdownXLOG(int code, Datum arg);
extern void CreateCheckPoint(int flags);
extern bool CreateRestartPoint(int flags);
extern WALAvailability GetWALAvailability(XLogRecPtr targetLSN);
extern void XLogPutNextOid(Oid nextOid);
extern XLogRecPtr XLogRestorePoint(const char *rpName);
extern void UpdateFullPageWrites(void);
extern void GetFullPageWriteInfo(XLogRecPtr *RedoRecPtr_p, bool *doPageWrites_p);
extern XLogRecPtr GetRedoRecPtr(void);
extern XLogRecPtr GetInsertRecPtr(void);
extern XLogRecPtr GetFlushRecPtr(TimeLineID *insertTLI);
extern TimeLineID GetWALInsertionTimeLine(void);
extern XLogRecPtr GetLastImportantRecPtr(void);

extern void SetWalWriterSleeping(bool sleeping);

/*
 * 供 xlogrecovery.c 在恢复期间回调 xlog.c 的例程。
 */
extern void RemoveNonParentXlogFiles(XLogRecPtr switchpoint, TimeLineID newTLI);
extern bool XLogCheckpointNeeded(XLogSegNo new_segno);
extern void SwitchIntoArchiveRecovery(XLogRecPtr EndRecPtr, TimeLineID replayTLI);
extern void ReachedEndOfBackup(XLogRecPtr EndRecPtr, TimeLineID tli);
extern void SetInstallXLogFileSegmentActive(void);
extern bool IsInstallXLogFileSegmentActive(void);
extern void ResetInstallXLogFileSegmentActive(void);
extern void XLogShutdownWalRcv(void);

/*
 * 启动、停止与查询基础备份状态的例程。
 */

/*
 * 基础备份的会话级状态
 *
 * 与共享内存状态并行使用，以控制给定会话中
 * 基础备份函数的并行执行，无论是专用于复制的
 * 后端，还是连到数据库的普通后端。会话级状态
 * 的更新与共享内存计数器同时发生，以保持
 * 正在运行的备份之全局与局部状态
 * 一致
 */
typedef enum SessionBackupState
{
	SESSION_BACKUP_NONE,
	SESSION_BACKUP_RUNNING,
} SessionBackupState;

extern void do_pg_backup_start(const char *backupidstr, bool fast,
							   List **tablespaces, BackupState *state,
							   StringInfo tblspcmapfile);
extern void do_pg_backup_stop(BackupState *state, bool waitforarchive);
extern void do_pg_abort_backup(int code, Datum arg);
extern void register_persistent_abort_backup_handler(void);
extern SessionBackupState get_backup_status(void);

/* 文件路径名（均相对 $PGDATA） */
#define RECOVERY_SIGNAL_FILE	"recovery.signal"
#define STANDBY_SIGNAL_FILE		"standby.signal"
#define BACKUP_LABEL_FILE		"backup_label"
#define BACKUP_LABEL_OLD		"backup_label.old"

#define TABLESPACE_MAP			"tablespace_map"
#define TABLESPACE_MAP_OLD		"tablespace_map.old"

/* 升主信号文件 */
#define PROMOTE_SIGNAL_FILE		"promote"

#endif							/* XLOG_H */
