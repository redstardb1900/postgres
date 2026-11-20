/*
 * clog.h
 *
 * PostgreSQL 事务提交日志管理器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/clog.h
 */
#ifndef CLOG_H
#define CLOG_H

#include "access/xlogreader.h"
#include "storage/sync.h"
#include "lib/stringinfo.h"

/*
 * 可能的事务状态 --- 注意全零即为初始状
 * 态.
 *
 * 所谓"子已提交"事务是已提交的子事务，其父
 * 尚未提交或中止。
 */
typedef int XidStatus;

#define TRANSACTION_STATUS_IN_PROGRESS		0x00
#define TRANSACTION_STATUS_COMMITTED		0x01
#define TRANSACTION_STATUS_ABORTED			0x02
#define TRANSACTION_STATUS_SUB_COMMITTED	0x03

typedef struct xl_clog_truncate
{
	int			pageno;
	TransactionId oldestXact;
	Oid			oldestXactDb;
} xl_clog_truncate;

extern void TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
									   TransactionId *subxids, XidStatus status, XLogRecPtr lsn);
extern XidStatus TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn);

extern Size CLOGShmemBuffers(void);
extern Size CLOGShmemSize(void);
extern void CLOGShmemInit(void);
extern void BootStrapCLOG(void);
extern void StartupCLOG(void);
extern void TrimCLOG(void);
extern void CheckPointCLOG(void);
extern void ExtendCLOG(TransactionId newestXact);
extern void TruncateCLOG(TransactionId oldestXact, Oid oldestxid_datoid);

extern int	clogsyncfiletag(const FileTag *ftag, char *path);

/* XLOG杂项 */
#define CLOG_ZEROPAGE		0x00
#define CLOG_TRUNCATE		0x10

extern void clog_redo(XLogReaderState *record);
extern void clog_desc(StringInfo buf, XLogReaderState *record);
extern const char *clog_identify(uint8 info);

#endif							/* CLOG_H */
