/*-------------------------------------------------------------------------
 *
 * sync.h
 *	  文件同步管理代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sync.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYNC_H
#define SYNC_H

#include "storage/relfilelocator.h"

/*
 * 同步请求的类型。用来管理待处理的
 * 请求，以便下次检查点调用同步处理器的 sync 或 unlink
 * 函数。
 */
typedef enum SyncRequestType
{
	SYNC_REQUEST,				/* 安排调用 sync 函数 */
	SYNC_UNLINK_REQUEST,		/* 安排调用 unlink 函数 */
	SYNC_FORGET_REQUEST,		/* 忘记 tag 的全部调用 */
	SYNC_FILTER_REQUEST			/* 忘记满足匹配函数的调用 */
} SyncRequestType;

/*
 * 用哪套函数处理给定请求。枚举值必须
 * 与 sync.c 中函数表的下标一致。
 */
typedef enum SyncRequestHandler
{
	SYNC_HANDLER_MD = 0,
	SYNC_HANDLER_CLOG,
	SYNC_HANDLER_COMMIT_TS,
	SYNC_HANDLER_MULTIXACT_OFFSET,
	SYNC_HANDLER_MULTIXACT_MEMBER,
	SYNC_HANDLER_NONE
} SyncRequestHandler;

/*
 * 标识文件的标签。目前含 md.c 用法所需的成员，
 * 但 sync.c 不知内部结构，且它可随未来
 * 处理器需要而改变。
 */
typedef struct FileTag
{
	int16		handler;		/* SyncRequestHandler 值，节省空间 */
	int16		forknum;		/* ForkNumber，省空间 */
	RelFileLocator rlocator;
	uint32		segno;
} FileTag;

extern void InitSync(void);
extern void SyncPreCheckpoint(void);
extern void SyncPostCheckpoint(void);
extern void ProcessSyncRequests(void);
extern void RememberSyncRequest(const FileTag *ftag, SyncRequestType type);
extern bool RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
								bool retryOnError);

#endif							/* SYNC_H */
