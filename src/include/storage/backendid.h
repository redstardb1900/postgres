/*-------------------------------------------------------------------------
 *
 * backendid.h
 *	  POSTGRES 后端 ID 通信定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/backendid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKENDID_H
#define BACKENDID_H

/* ----------------
 *		-cim 8/17/90
 * ----------------
 */
typedef int BackendId;			/* 当前活动后端的唯一标识 */

#define InvalidBackendId		(-1)

extern PGDLLIMPORT BackendId MyBackendId;	/* 本后端的后端 ID */

/* 并行会话领导者的后端 ID，若无则为 InvalidBackendId */
extern PGDLLIMPORT BackendId ParallelLeaderBackendId;

/*
 * 本会话临时关系所用 BackendId 通常为我们自己的，
 * 但并行工作进程应使用其领导者 ID。
 */
#define BackendIdForTempRelations() \
	(ParallelLeaderBackendId == InvalidBackendId ? MyBackendId : ParallelLeaderBackendId)

#endif							/* BACKENDID_H */
