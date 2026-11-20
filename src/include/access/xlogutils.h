/*
 * xlogutils.h
 *
 * 重放 WAL 记录的实用例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogutils.h
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "access/xlogreader.h"
#include "storage/bufmgr.h"

/*
 * 8.4 以前，恢复期间的全部活动均由启动
 * 进程执行。此局部变量仍在代码多处用来
 * 标明 RecoveryManagers 所采取的动作。其他在
 * 恢复期间可能执行工作的进程应检查 RecoveryInProgress()。
 * 见 xlog.c中 XLogCtl 注。
 */
extern PGDLLIMPORT bool InRecovery;

/*
 * 与 InRecovery 一样，standbyState 仅在启动进程中有效。
 * 在所有其他进程中其值为 STANDBY_DISABLED（因此 
 * InHotStandby 读为 false）。
 *
 * DISABLED 状态下，我们正在做崩溃恢复，或热备已在
 * postgresql.conf 中禁用。
 *
 * INITIALIZED 状态下已运行 InitRecoveryTransactionEnvironment，但
 * 尚未处理 RUNNING_XACTS 或关闭检查点 WAL 记录
 * 以初始化我们的主库事务跟踪系统。
 *
 * 当事务跟踪已初始化，我们进入 SNAPSHOT_PENDING 状态。
 * 所跟踪信息可能仍不完整，故尚无法允许
 * 连接，但 redo 函数必须更新内存中状态，在
 * 适当时。
 *
 * 在 SNAPSHOT_READY 模式下，我们完全了解那些正（
 * 或曾经）在主库上于当前 WAL 位置运行的事务。快照
 * 可获取，且可运行只读查询。
 */
typedef enum
{
	STANDBY_DISABLED,
	STANDBY_INITIALIZED,
	STANDBY_SNAPSHOT_PENDING,
	STANDBY_SNAPSHOT_READY
} HotStandbyState;

extern PGDLLIMPORT HotStandbyState standbyState;

#define InHotStandby (standbyState >= STANDBY_SNAPSHOT_PENDING)


extern bool XLogHaveInvalidPages(void);
extern void XLogCheckInvalidPages(void);

extern void XLogDropRelation(RelFileLocator rlocator, ForkNumber forknum);
extern void XLogDropDatabase(Oid dbid);
extern void XLogTruncateRelation(RelFileLocator rlocator, ForkNumber forkNum,
								 BlockNumber nblocks);

/* XLogReadBufferForRedo[Extended] 的结果码 */
typedef enum
{
	BLK_NEEDS_REDO,				/* 需要应用来自该 WAL 记录的更改 */
	BLK_DONE,					/* 该块已是最新 */
	BLK_RESTORED,				/* 该块已从整页映像恢复 */
	BLK_NOTFOUND				/* 未找到该块（因而无需
								 * 被重放）*/
} XLogRedoAction;

/* read_local_xlog_page_no_wait 回调的私有数据。 */
typedef struct ReadLocalXLogPageNoWaitPrivate
{
	bool		end_of_wal;		/* true，当已到达 WAL末尾时 */
} ReadLocalXLogPageNoWaitPrivate;

extern XLogRedoAction XLogReadBufferForRedo(XLogReaderState *record,
											uint8 block_id, Buffer *buf);
extern Buffer XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id);
extern XLogRedoAction XLogReadBufferForRedoExtended(XLogReaderState *record,
													uint8 block_id,
													ReadBufferMode mode, bool get_cleanup_lock,
													Buffer *buf);

extern Buffer XLogReadBufferExtended(RelFileLocator rlocator, ForkNumber forknum,
									 BlockNumber blkno, ReadBufferMode mode,
									 Buffer recent_buffer);

extern Relation CreateFakeRelcacheEntry(RelFileLocator rlocator);
extern void FreeFakeRelcacheEntry(Relation fakerel);

extern int	read_local_xlog_page(XLogReaderState *state,
								 XLogRecPtr targetPagePtr, int reqLen,
								 XLogRecPtr targetRecPtr, char *cur_page);
extern int	read_local_xlog_page_no_wait(XLogReaderState *state,
										 XLogRecPtr targetPagePtr, int reqLen,
										 XLogRecPtr targetRecPtr,
										 char *cur_page);
extern void wal_segment_open(XLogReaderState *state,
							 XLogSegNo nextSegNo,
							 TimeLineID *tli_p);
extern void wal_segment_close(XLogReaderState *state);

extern void XLogReadDetermineTimeline(XLogReaderState *state,
									  XLogRecPtr wantPage,
									  uint32 wantLength,
									  TimeLineID currTLI);

extern void WALReadRaiseError(WALReadError *errinfo);

#endif
