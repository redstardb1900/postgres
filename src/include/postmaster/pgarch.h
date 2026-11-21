/*-------------------------------------------------------------------------
 *
 * pgarch.h
 *	  导出自 postmaster/pgarch.c。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/pgarch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGARCH_H
#define _PGARCH_H

/* ----------
 * 归档控制信息。
 *
 * 我们期望 pg_wal 内可归档文件名长度在
 * MIN_XFN_CHARS 与 MAX_XFN_CHARS 之间，且仅含
 * VALID_XFN_CHARS 中的字符。archive_status 中的
 * 状态文件附加 ".ready" 或 ".done"。
 * ----------
 */
#define MIN_XFN_CHARS	16
#define MAX_XFN_CHARS	40
#define VALID_XFN_CHARS "0123456789ABCDEF.history.backup.partial"

extern Size PgArchShmemSize(void);
extern void PgArchShmemInit(void);
extern bool PgArchCanRestart(void);
extern void PgArchiverMain(void) pg_attribute_noreturn();
extern void PgArchWakeup(void);
extern void PgArchForceDirScan(void);

#endif							/* _PGARCH_H */
