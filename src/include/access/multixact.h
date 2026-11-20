/*
 * multixact.h
 *
 * PostgreSQL 多事务日志管理器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact.h
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/sync.h"


/*
 * 前两个 MultiXactId 值保留用于存储截断 Xid
 * 及首段的 epoch，故从下列值起分配 multixact
 * 2.
 */
#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 1)
#define MaxMultiXactId		((MultiXactId) 0xFFFFFFFF)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

#define MaxMultiXactOffset	((MultiXactOffset) 0xFFFFFFFF)

/* 供 multixact 使用的 SLRU 缓冲区数 */
#define NUM_MULTIXACTOFFSET_BUFFERS		8
#define NUM_MULTIXACTMEMBER_BUFFERS		16

/*
 * 可能的 multixact 锁模式("status")。前四种模式用于
 * 元组锁(FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE)；后
 * 两种用于更新与删除模式。
 */
typedef enum
{
	MultiXactStatusForKeyShare = 0x00,
	MultiXactStatusForShare = 0x01,
	MultiXactStatusForNoKeyUpdate = 0x02,
	MultiXactStatusForUpdate = 0x03,
	/* 不触及"key"列的更新 */
	MultiXactStatusNoKeyUpdate = 0x04,
	/* 其他更新，及删除 */
	MultiXactStatusUpdate = 0x05
} MultiXactStatus;

#define MaxMultiXactStatus MultiXactStatusUpdate

/* status 值是否对应元组更新？ */
#define ISUPDATE_from_mxstatus(status) \
			((status) > MultiXactStatusForUpdate)


typedef struct MultiXactMember
{
	TransactionId xid;
	MultiXactStatus status;
} MultiXactMember;


/* ----------------
 *		multixact 相关 XLOG 记录
 * ----------------
 */

#define XLOG_MULTIXACT_ZERO_OFF_PAGE	0x00
#define XLOG_MULTIXACT_ZERO_MEM_PAGE	0x10
#define XLOG_MULTIXACT_CREATE_ID		0x20
#define XLOG_MULTIXACT_TRUNCATE_ID		0x30

typedef struct xl_multixact_create
{
	MultiXactId mid;			/* 新 MultiXact的ID */
	MultiXactOffset moff;		/* 其在 members 文件起始偏移 */
	int32		nmembers;		/* 成员 XIDs 数 */
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} xl_multixact_create;

#define SizeOfMultiXactCreate (offsetof(xl_multixact_create, members))

typedef struct xl_multixact_truncate
{
	Oid			oldestMultiDB;

	/* 待截断的 multixact 偏移范围 */
	MultiXactId startTruncOff;	/* 仅为完整起见 */
	MultiXactId endTruncOff;

	/* 待截断的 multixact 成员范围 */
	MultiXactOffset startTruncMemb;
	MultiXactOffset endTruncMemb;
} xl_multixact_truncate;

#define SizeOfMultiXactTruncate (sizeof(xl_multixact_truncate))


extern MultiXactId MultiXactIdCreate(TransactionId xid1,
									 MultiXactStatus status1, TransactionId xid2,
									 MultiXactStatus status2);
extern MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
									 MultiXactStatus status);
extern MultiXactId MultiXactIdCreateFromMembers(int nmembers,
												MultiXactMember *members);

extern MultiXactId ReadNextMultiXactId(void);
extern void ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next);
extern bool MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly);
extern void MultiXactIdSetOldestMember(void);
extern int	GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
								  bool from_pgupgrade, bool isLockOnly);
extern bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
extern bool MultiXactIdPrecedesOrEquals(MultiXactId multi1,
										MultiXactId multi2);

extern int	multixactoffsetssyncfiletag(const FileTag *ftag, char *path);
extern int	multixactmemberssyncfiletag(const FileTag *ftag, char *path);

extern void AtEOXact_MultiXact(void);
extern void AtPrepare_MultiXact(void);
extern void PostPrepare_MultiXact(TransactionId xid);

extern Size MultiXactShmemSize(void);
extern void MultiXactShmemInit(void);
extern void BootStrapMultiXact(void);
extern void StartupMultiXact(void);
extern void TrimMultiXact(void);
extern void SetMultiXactIdLimit(MultiXactId oldest_datminmxid,
								Oid oldest_datoid,
								bool is_startup);
extern void MultiXactGetCheckptMulti(bool is_shutdown,
									 MultiXactId *nextMulti,
									 MultiXactOffset *nextMultiOffset,
									 MultiXactId *oldestMulti,
									 Oid *oldestMultiDB);
extern void CheckPointMultiXact(void);
extern MultiXactId GetOldestMultiXactId(void);
extern void TruncateMultiXact(MultiXactId newOldestMulti,
							  Oid newOldestMultiDB);
extern void MultiXactSetNextMXact(MultiXactId nextMulti,
								  MultiXactOffset nextMultiOffset);
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti,
									  MultiXactOffset minMultiOffset);
extern void MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB);
extern int	MultiXactMemberFreezeThreshold(void);

extern void multixact_twophase_recover(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void multixact_twophase_postcommit(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);
extern void multixact_twophase_postabort(TransactionId xid, uint16 info,
										 void *recdata, uint32 len);

extern void multixact_redo(XLogReaderState *record);
extern void multixact_desc(StringInfo buf, XLogReaderState *record);
extern const char *multixact_identify(uint8 info);
extern char *mxid_to_string(MultiXactId multi, int nmembers,
							MultiXactMember *members);

#endif							/* MULTIXACT_H */
