/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres 事务（提交）日志接口例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/transam.c
 *
 * NOTES
 *	  本文件包含面向事务系统的
 *	  高层访问方法接口。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "utils/snapmgr.h"

/*
 * TransactionLogFetch 结果的单项缓存。值得设置
 * 此类缓存，因为我们经常反复检查
 * 同一个 XID，例如在批量插入、
 * 更新或删除之后扫描表时。
 */
static TransactionId cachedFetchXid = InvalidTransactionId;
static XidStatus cachedFetchXidStatus;
static XLogRecPtr cachedCommitLSN;

/* 本地函数 */
static XidStatus TransactionLogFetch(TransactionId transactionId);


/* ----------------------------------------------------------------
 *		Postgres 日志访问方法接口
 *
 *		TransactionLogFetch
 * ----------------------------------------------------------------
 */

/*
 * TransactionLogFetch --- 获取指定事务 id 的提交状态
 */
static XidStatus
TransactionLogFetch(TransactionId transactionId)
{
	XidStatus	xidstatus;
	XLogRecPtr	xidlsn;

	/*
	 * 在访问提交日志管理器之前，先检查单项缓存，
	 * 看是否刚才已经查过该事务状态。
	 */
	if (TransactionIdEquals(transactionId, cachedFetchXid))
		return cachedFetchXidStatus;

	/*
	 * 同时检查该事务 ID 是否为永久 ID。
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		return TRANSACTION_STATUS_ABORTED;
	}

	/*
	 * 获取事务状态。
	 */
	xidstatus = TransactionIdGetStatus(transactionId, &xidlsn);

	/*
	 * 缓存结果，但切勿缓存未完成或子提交
	 * 事务的状态！我们只缓存保证不会再变化的状态。
	 */
	if (xidstatus != TRANSACTION_STATUS_IN_PROGRESS &&
		xidstatus != TRANSACTION_STATUS_SUB_COMMITTED)
	{
		cachedFetchXid = transactionId;
		cachedFetchXidStatus = xidstatus;
		cachedCommitLSN = xidlsn;
	}

	return xidstatus;
}

/* ----------------------------------------------------------------
 *						接口函数
 *
 *		TransactionIdDidCommit
 *		TransactionIdDidAbort
 *		========
 *		   这些函数测试指定
 *		   事务 id 的事务状态。
 *
 *		TransactionIdCommitTree
 *		TransactionIdAsyncCommitTree
 *		TransactionIdAbortTree
 *		========
 *		   这些函数设置指定
 *		   事务树的事务状态。
 *
 * 另见 TransactionIdIsInProgress（曾在本模块中，
 * 现位于 procarray.c），以及 heapam_visibility.c
 * 顶部说明各部分如何协同工作的注释。
 * ----------------------------------------------------------------
 */

/*
 * TransactionIdDidCommit
 *		当与该标识符关联的事务已提交时为真。
 *
 * 注意：
 *		假定事务标识符有效且存在于 clog 中。
 */
bool							/* 若给定事务已提交则为真 */
TransactionIdDidCommit(TransactionId transactionId)
{
	XidStatus	xidstatus;

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * 若已标记为已提交，则已提交。
	 */
	if (xidstatus == TRANSACTION_STATUS_COMMITTED)
		return true;

	/*
	 * 若已标记为子提交，则必须递归检查父事务。
	 * 但如果它早于 TransactionXmin，就无法查看
	 * pg_subtrans；此时假定父事务崩溃且未清理
	 * 其子事务。
	 *
	 * 原先我们对 SubTransGetParent 的结果非零做 Assert。
	 * 但引入预备事务后，数据库刚启动后可能有一段
	 * 窗口，pg_subtrans 对 TransactionXmin 之后的事务
	 * 并不完整。StartupSUBTRANS() 已确保缺少的信息会被
	 * 清零。由于正常情况下不应发生，对该情况发出
	 * WARNING 似乎合理。
	 *
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return false;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return false;
		}
		return TransactionIdDidCommit(parentXid);
	}

	/*
	 * 未提交。
	 */
	return false;
}

/*
 * TransactionIdDidAbort
 *		当与该标识符关联的事务已中止时为真。
 *
 * 注意：
 *		假定事务标识符有效且存在于 clog 中。
 *
 *		仅对显式中止的事务返回真；因崩溃而
 *		隐式中止的事务在 clog 中通常仍显示为
 *		进行中。多数情况下应使用 TransactionIdDidCommit()，
 *		并在之前配合 TransactionIdIsInProgress() 检查，
 *		而不是使用 TransactionIdDidAbort()。
 */
bool							/* 若给定事务已中止则为真 */
TransactionIdDidAbort(TransactionId transactionId)
{
	XidStatus	xidstatus;

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * 若已标记为已中止，则已中止。
	 */
	if (xidstatus == TRANSACTION_STATUS_ABORTED)
		return true;

	/*
	 * 若已标记为子提交，则必须递归检查父事务。
	 * 但如果它早于 TransactionXmin，就无法查看
	 * pg_subtrans；此时假定父事务崩溃且未清理
	 * 其子事务。
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return true;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			/* 见 TransactionIdDidCommit 中的说明 */
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return true;
		}
		return TransactionIdDidAbort(parentXid);
	}

	/*
	 * 未中止。
	 */
	return false;
}

/*
 * TransactionIdCommitTree
 *		将给定事务及其子事务标记为已提交
 *
 * "xid" 是顶层事务提交，xids 数组包含其
 * 已提交的子事务。
 *
 * 此提交操作不保证原子，但若非原子，子 xid
 * 会先被正确标记为子提交。
 */
void
TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED,
							   InvalidXLogRecPtr);
}

/*
 * TransactionIdAsyncCommitTree
 *		同上，但用于异步提交。需要提交记录的 LSN。
 */
void
TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids,
							 XLogRecPtr lsn)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED, lsn);
}

/*
 * TransactionIdAbortTree
 *		将给定事务及其子事务标记为已中止。
 *
 * "xid" 是顶层事务提交，xids 数组包含其
 * 已提交的子事务。
 *
 * 无需担心非原子行为，因为旁观者
 * 本来就会把这些事务都视为尚未提交。
 */
void
TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_ABORTED, InvalidXLogRecPtr);
}

/*
 * TransactionIdPrecedes --- id1 在逻辑上是否 < id2？
 */
bool
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{
	/*
	 * 若任一 ID 为永久 XID，则可直接做无符号
	 * 比较。若两者都是普通 XID，则做模 2^32 比较。
	 */
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 < id2);

	diff = (int32) (id1 - id2);
	return (diff < 0);
}

/*
 * TransactionIdPrecedesOrEquals --- id1 在逻辑上是否 <= id2？
 */
bool
TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 <= id2);

	diff = (int32) (id1 - id2);
	return (diff <= 0);
}

/*
 * TransactionIdFollows --- id1 在逻辑上是否 > id2？
 */
bool
TransactionIdFollows(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 > id2);

	diff = (int32) (id1 - id2);
	return (diff > 0);
}

/*
 * TransactionIdFollowsOrEquals --- id1 在逻辑上是否 >= id2？
 */
bool
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 >= id2);

	diff = (int32) (id1 - id2);
	return (diff >= 0);
}


/*
 * TransactionIdLatest --- 在主事务及其子事务中取最新 XID
 */
TransactionId
TransactionIdLatest(TransactionId mainxid,
					int nxids, const TransactionId *xids)
{
	TransactionId result;

	/*
	 * 实践中 xids[] 数组极有可能已排序，因此
	 * 只需取最后一个子 XID 即可节省周期，但这一点
	 * 大概还没关键到值得依赖该假设的程度。
	 * 不过为表明我们并非完全不动脑，从后往前扫描
	 * 数组以避免无用赋值。
	 */
	result = mainxid;
	while (--nxids >= 0)
	{
		if (TransactionIdPrecedes(result, xids[nxids]))
			result = xids[nxids];
	}
	return result;
}


/*
 * TransactionIdGetCommitLSN
 *
 * 本函数返回一个足够靠后的 LSN，以保证
 * 若我们刷写到所返回的 LSN，则已将该
 * 事务的提交记录刷写到磁盘。
 *
 * 结果未必恰好是该事务提交记录的
 * 精确 LSN！例如，对很久以前的事务（其
 * clog 页已落到磁盘），我们将返回 InvalidXLogRecPtr。
 * 另外，由于我们把同一 clog 页上的事务分组以节省
 * 存储，也可能返回落入同一组的稍后事务的
 * LSN。
 */
XLogRecPtr
TransactionIdGetCommitLSN(TransactionId xid)
{
	XLogRecPtr	result;

	/*
	 * 目前，本函数的所有用法都针对刚被
	 * TransactionLogFetch 报告为已提交的 xid，因此通常
	 * 检查 TransactionLogFetch 的缓存即可成功，避免
	 * 额外访问共享内存。
	 */
	if (TransactionIdEquals(xid, cachedFetchXid))
		return cachedCommitLSN;

	/* 特殊 XIDs 始终视为已提交 */
	if (!TransactionIdIsNormal(xid))
		return InvalidXLogRecPtr;

	/*
	 * 获取事务状态。
	 */
	(void) TransactionIdGetStatus(xid, &result);

	return result;
}
