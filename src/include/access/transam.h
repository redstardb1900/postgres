/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres 事务访问方法支持代码
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/transam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "access/xlogdefs.h"


/* ----------------
 *		特殊事务 ID 值
 *
 * BootstrapTransactionId 是"引导"操作的 XID，而 
 * FrozenTransactionId 用于非常旧的元组。二者应
 * 始终视为有效。
 *
 * FirstNormalTransactionId 是首个"普通"事务 id。
 * 注意：若需更改它，则必须同时改 pg_class.h。
 * ----------------
 */
#define InvalidTransactionId		((TransactionId) 0)
#define BootstrapTransactionId		((TransactionId) 1)
#define FrozenTransactionId			((TransactionId) 2)
#define FirstNormalTransactionId	((TransactionId) 3)
#define MaxTransactionId			((TransactionId) 0xFFFFFFFF)

/* ----------------
 *		事务 ID 操纵宏
 * ----------------
 */
#define TransactionIdIsValid(xid)		((xid) != InvalidTransactionId)
#define TransactionIdIsNormal(xid)		((xid) >= FirstNormalTransactionId)
#define TransactionIdEquals(id1, id2)	((id1) == (id2))
#define TransactionIdStore(xid, dest)	(*(dest) = (xid))
#define StoreInvalidTransactionId(dest) (*(dest) = InvalidTransactionId)

#define EpochFromFullTransactionId(x)	((uint32) ((x).value >> 32))
#define XidFromFullTransactionId(x)		((uint32) (x).value)
#define U64FromFullTransactionId(x)		((x).value)
#define FullTransactionIdEquals(a, b)	((a).value == (b).value)
#define FullTransactionIdPrecedes(a, b)	((a).value < (b).value)
#define FullTransactionIdPrecedesOrEquals(a, b) ((a).value <= (b).value)
#define FullTransactionIdFollows(a, b) ((a).value > (b).value)
#define FullTransactionIdFollowsOrEquals(a, b) ((a).value >= (b).value)
#define FullTransactionIdIsValid(x)		TransactionIdIsValid(XidFromFullTransactionId(x))
#define InvalidFullTransactionId		FullTransactionIdFromEpochAndXid(0, InvalidTransactionId)
#define FirstNormalFullTransactionId	FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId)
#define FullTransactionIdIsNormal(x)	FullTransactionIdFollowsOrEquals(x, FirstNormalFullTransactionId)

/*
 * 含纪元与 TransactionId 的 64 位值。此值
 * 包在结构体中，以防与 TransactionId 隐式互转。
 * 并非所有值都表示有效普通XIDs。
 */
typedef struct FullTransactionId
{
	uint64		value;
} FullTransactionId;

static inline FullTransactionId
FullTransactionIdFromEpochAndXid(uint32 epoch, TransactionId xid)
{
	FullTransactionId result;

	result.value = ((uint64) epoch) << 32 | xid;

	return result;
}

static inline FullTransactionId
FullTransactionIdFromU64(uint64 value)
{
	FullTransactionId result;

	result.value = value;

	return result;
}

/* 推进事务 ID 变量，正确处理回卷 */
#define TransactionIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalTransactionId) \
			(dest) = FirstNormalTransactionId; \
	} while(0)

/*
 * 回退 FullTransactionId 变量，跳过那些仅在视为 
 * 32 位 XIDs 时才显得特殊的 xid。
 */
static inline void
FullTransactionIdRetreat(FullTransactionId *dest)
{
	dest->value--;

	/*
	 * 与 32 位 XIDs 不同，不跳过"真正的"特殊 xid。
	 * 对 64 位 xid，这些无法在回卷中触及，而它们
	 * 在 32 位下可以。
	 */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	/*
	 * 但我们确需跳过仅对 32 位显得特殊的 
	 * XIDs.
	 */
	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value--;
}

/*
 * 推进 FullTransactionId 变量，跳过那些仅在视为 
 * 32 位 XIDs 时才显得特殊的 xid。
 */
static inline void
FullTransactionIdAdvance(FullTransactionId *dest)
{
	dest->value++;

	/* 见 FullTransactionIdAdvance() */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value++;
}

/* 回退事务 ID 变量，正确处理回卷 */
#define TransactionIdRetreat(dest)	\
	do { \
		(dest)--; \
	} while ((dest) < FirstNormalTransactionId)

/* 比较两个已知为普通的 XIDs；此宏为求速度 */
#define NormalTransactionIdPrecedes(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) < 0)

/* 比较两个已知为普通的 XIDs；此宏为求速度 */
#define NormalTransactionIdFollows(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) > 0)

/* ----------
 *		对象 ID (OID) 零为 InvalidOid.
 *
 *		OIDs 1-9999 保留给手工指派 (见 .dat 文件于 
 *		src/include/catalog/)。其中 8000-9999 保留给
 *		开发用途（如进行中的补丁与分叉）；
 *		它们不应出现在已发布版本中。
 *
 *		OIDs 10000-11999 保留给 genbki.pl 指派，在 
 *		src/include/catalog/ 的 .dat 文件未给需 OID 的
 *		目录项指定 OID 时用。注意 genbki.pl 在
 *		各目录中独立指派这些 OID，故不保证其
 *		全局唯一。此外，引导后端与 
 *		initdb 的后引导处理也可在此范围指派 OID。
 *		常规 OID 生成逻辑会处理由此可能产生的
 *		任何 OID 冲突。
 *
 *		OIDs 12000-16383 保留给 initdb 后引导处理所
 *		创建的未钉住对象。initdb 一做好其负责的钉住
 *		对象就把 OID 生成器强制升到 12000。
 *
 *		自 16384 起的 OID 由 OID 生成器在正常
 *		多用户运行期间指派。（一进入正常运行我们即
 *		把生成器强制升到 16384。）
 *
 * 8000、10000 和 12000 的选取完全任意，且可
 * 在任一类 OID 不够用时改动。更改下列宏，
 * 并更新相关文档 (见 bki.sgml 与 RELEASE_CHANGES)，
 * 应即足以完成此事。移动 16384 这条介于 
 * initdb 指派 OID 与用户定义对象之间的边界会
 * 显著更痛，然而因部分用户定义 OID 会出现在
 * 盘上数据中；此种更改大概会破坏 pg_upgrade。
 *
 * 注意：若 OID 生成器回卷，我们跳过 OIDs 0-16383 
 * 并从 16384 继续。这把 OID 冲突几率压到最低，靠不
 * 重派可能在 initdb 期间已派过的 OID。关键是，
 * 这也确保用户创建对象皆不被视为钉住。
 * ----------
 */
#define FirstGenbkiObjectId		10000
#define FirstUnpinnedObjectId	12000
#define FirstNormalObjectId		16384

/*
 * VariableCache 是共享内存中的数据结构，用于跟踪
 * OID 与 XID 指派状态。主要出于历史原因，只有
 * 一个结构体，其不同字段受保护于不同 
 * LWLocks.
 *
 * 注意：xidWrapLimit 与 oldestXidDB 并非"活跃"值，而
 * 仅用于生成有用消息，当 xidWarnLimit 或 xidStopLimit 
 * 超限时。
 */
typedef struct VariableCacheData
{
	/*
	 * 这些字段受 OidGenLock 保护。
	 */
	Oid			nextOid;		/* 下一待指派OID */
	uint32		oidCount;		/* 须做 XLOG 工作前可用的 OIDs */

	/*
	 * 这些字段受 XidGenLock 保护。
	 */
	FullTransactionId nextXid;	/* 下一待指派XID */

	TransactionId oldestXid;	/* 集簇范围最小 datfrozenxid */
	TransactionId xidVacLimit;	/* 从此处起强制 autovacuum */
	TransactionId xidWarnLimit; /* 从此开始抱怨 */
	TransactionId xidStopLimit; /* 拒绝将 nextXid 推进超过此处 */
	TransactionId xidWrapLimit; /* 世界尽头 */
	Oid			oldestXidDB;	/* 最小 datfrozenxid 的数据库 */

	/*
	 * 这些字段受 CommitTsLock 保护
	 */
	TransactionId oldestCommitTsXid;
	TransactionId newestCommitTsXid;

	/*
	 * 这些字段受 ProcArrayLock 保护。
	 */
	FullTransactionId latestCompletedXid;	/* 最新提交或中止的
											 * 完整 XID */

	/*
	 * 带 xid 的顶层事务数（即可能已
	 * 修改数据库）且自服务器启动以来以某
	 * 种形式完成者。目前仅用于检查 
	 * GetSnapshotData() 是否需重算快照内容。
	 * 此值可能还有其他使用者。恒大于 1。
	 */
	uint64		xactCompletionCount;

	/*
	 * 这些字段受 XactTruncationLock 保护
	 */
	TransactionId oldestClogXid;	/* 可安全在 clog 查找的最老 */

} VariableCacheData;

typedef VariableCacheData *VariableCache;


/* ----------------
 *		外部声明
 * ----------------
 */

/* in transam/xact.c */
extern bool TransactionStartedDuringRecovery(void);

/* in transam/varsup.c */
extern PGDLLIMPORT VariableCache ShmemVariableCache;

/*
 * transam/transam.c 中函数的原型
 */
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern void TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids);
extern void TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids, XLogRecPtr lsn);
extern void TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids);
extern bool TransactionIdPrecedes(TransactionId id1, TransactionId id2);
extern bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollows(TransactionId id1, TransactionId id2);
extern bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2);
extern TransactionId TransactionIdLatest(TransactionId mainxid,
										 int nxids, const TransactionId *xids);
extern XLogRecPtr TransactionIdGetCommitLSN(TransactionId xid);

/* in transam/varsup.c */
extern FullTransactionId GetNewTransactionId(bool isSubXact);
extern void AdvanceNextFullTransactionIdPastXid(TransactionId xid);
extern FullTransactionId ReadNextFullTransactionId(void);
extern void SetTransactionIdLimit(TransactionId oldest_datfrozenxid,
								  Oid oldest_datoid);
extern void AdvanceOldestClogXid(TransactionId oldest_datfrozenxid);
extern bool ForceTransactionIdLimitUpdate(void);
extern Oid	GetNewObjectId(void);
extern void StopGeneratingPinnedObjectIds(void);

#ifdef USE_ASSERT_CHECKING
extern void AssertTransactionIdInAllowableRange(TransactionId xid);
#else
#define AssertTransactionIdInAllowableRange(xid) ((void)true)
#endif

/*
 * 某些前端程序包含本头文件。对即使未用也发出静态
 * 内联函数的编译器，会导致未满足的
 * 外部引用；故用 #ifndef FRONTEND 隐藏。
 */
#ifndef FRONTEND

/*
 * 供只需下一事务 ID 的 XID 部分的调用方。
 */
static inline TransactionId
ReadNextTransactionId(void)
{
	return XidFromFullTransactionId(ReadNextFullTransactionId());
}

/* 返回按量回退的事务 ID，正确处理回卷 */
static inline TransactionId
TransactionIdRetreatedBy(TransactionId xid, uint32 amount)
{
	xid -= amount;

	while (xid < FirstNormalTransactionId)
		xid--;

	return xid;
}

/* 返回两个 ID 中较老者 */
static inline TransactionId
TransactionIdOlder(TransactionId a, TransactionId b)
{
	if (!TransactionIdIsValid(a))
		return b;

	if (!TransactionIdIsValid(b))
		return a;

	if (TransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* 返回两个 ID 中较老者，假定二者皆普通 */
static inline TransactionId
NormalTransactionIdOlder(TransactionId a, TransactionId b)
{
	Assert(TransactionIdIsNormal(a));
	Assert(TransactionIdIsNormal(b));
	if (NormalTransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* 返回两个 ID 中较新者 */
static inline FullTransactionId
FullTransactionIdNewer(FullTransactionId a, FullTransactionId b)
{
	if (!FullTransactionIdIsValid(a))
		return b;

	if (!FullTransactionIdIsValid(b))
		return a;

	if (FullTransactionIdFollows(a, b))
		return a;
	return b;
}

#endif							/* FRONTEND */

#endif							/* TRANSAM_H */
