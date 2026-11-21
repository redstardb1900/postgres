/*-------------------------------------------------------------------------
 *
 * bgwriter.h
 *	  导出自 postmaster/bgwriter.c 与 postmaster/checkpointer.c。
 *
 * bgwriter 进程过去也负责检查点。现已有独立进程，
 * 但我们未费心拆分本头文件。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/postmaster/bgwriter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BGWRITER_H
#define _BGWRITER_H

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"


/* GUC 选项 */
extern PGDLLIMPORT int BgWriterDelay;
extern PGDLLIMPORT int CheckPointTimeout;
extern PGDLLIMPORT int CheckPointWarning;
extern PGDLLIMPORT double CheckPointCompletionTarget;

extern void BackgroundWriterMain(void) pg_attribute_noreturn();
extern void CheckpointerMain(void) pg_attribute_noreturn();

extern void RequestCheckpoint(int flags);
extern void CheckpointWriteDelay(int flags, double progress);

extern bool ForwardSyncRequest(const FileTag *ftag, SyncRequestType type);

extern void AbsorbSyncRequests(void);

extern Size CheckpointerShmemSize(void);
extern void CheckpointerShmemInit(void);

extern bool FirstCallSinceLastCheckpoint(void);

#endif							/* _BGWRITER_H */
