/*-------------------------------------------------------------------------
 *
 * lmgr.c
 *	  POSTGRES 锁管理器代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "utils/inval.h"


/*
 * 每个后端用于生成推测插入令牌的计数器。
 *
 * 它可能回绕，但这没关系，因为它只用在
 * 插入元组与确认没有违反（唯一）约束
 * 的短暂期间。理论上，一个后端
 * 可能看见另一个后端推测插入的元组，但在开始
 * 等待该令牌之前，另一个后端已经完成插入，
 * 接着又做了 2^32 次无关插入。这一切之后，
 * 第一个后端终于调用 SpeculativeInsertionLockAcquire()，
 * 本意是等待第一次插入完成，结果却在等待
 * 最近一次无关插入。即便如此也不会特别糟糕：
 * 最坏情况下它们会死锁，导致其中一个
 * 事务中止。
 */
static uint32 speculativeInsertionToken = 0;


/*
 * 保存事务锁等待的上下文信息的结构体。
 *
 * 'oper' 是需要等待另一事务的操作；'rel'
 * 与 'ctid' 指定所等待元组的地址。
 */
typedef struct XactLockTableWaitInfo
{
	XLTW_Oper	oper;
	Relation	rel;
	ItemPointer ctid;
} XactLockTableWaitInfo;

static void XactLockTableWaitErrorCb(void *arg);

/*
 * RelationInitLockInfo
 *		初始化关系描述符中的锁信息。
 *
 *		relcache.c 创建任何 reldesc 时必须调用本函数。
 */
void
RelationInitLockInfo(Relation relation)
{
	Assert(RelationIsValid(relation));
	Assert(OidIsValid(RelationGetRelid(relation)));

	relation->rd_lockInfo.lockRelId.relId = RelationGetRelid(relation);

	if (relation->rd_rel->relisshared)
		relation->rd_lockInfo.lockRelId.dbId = InvalidOid;
	else
		relation->rd_lockInfo.lockRelId.dbId = MyDatabaseId;
}

/*
 * SetLocktagRelationOid
 *		只根据关系 OID 为该关系设置 locktag
 */
static inline void
SetLocktagRelationOid(LOCKTAG *tag, Oid relid)
{
	Oid			dbid;

	if (IsSharedRelation(relid))
		dbid = InvalidOid;
	else
		dbid = MyDatabaseId;

	SET_LOCKTAG_RELATION(*tag, dbid, relid);
}

/*
 *		LockRelationOid
 *
 * 只根据 OID 给关系加锁。一般应在尝试打开
 * 该关系的 relcache 项之前使用。
 */
void
LockRelationOid(Oid relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SetLocktagRelationOid(&tag, relid);

	res = LockAcquireExtended(&tag, lockmode, false, false, true, &locallock);

	/*
	 * 拿到锁之后，检查失效消息，以便在使用之前
	 * 更新或清除任何过期的 relcache 项。
	 * RangeVarGetRelid() 专门依赖我们完成这件事。若已经持有
	 * 所请求的同一种锁（这种情况并不少见），就可以跳过，
	 * 因为此时别人不可能以不当方式修改
	 * relcache 项。（若是本事务自己修改了该关系，
	 * relcache 更新经由
	 * CommandCounterIncrement 完成，而不是在这里。）
	 *
	 * 不过，当代码递归地操作表（通常是系统目录）时，
	 * 极端情况下，可能在本函数或兄弟函数的外层执行
	 * 仍在处理失效消息时又进入这里。锁的
	 * "cleared" 状态表明我们是否真的已经
	 * 吸收完相关的失效消息。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}
}

/*
 *		ConditionalLockRelationOid
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 当且仅当已获得锁时返回真。
 *
 * 注意：目前并不需要为本文件里全部
 * LockXXX 例程提供条件版本，但需要时可以很容易补上。
 */
bool
ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SetLocktagRelationOid(&tag, relid);

	res = LockAcquireExtended(&tag, lockmode, false, true, true, &locallock);

	if (res == LOCKACQUIRE_NOT_AVAIL)
		return false;

	/*
	 * 拿到锁之后检查失效消息；说明见
	 * LockRelationOid。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}

	return true;
}

/*
 *		LockRelationId
 *
 * 根据 LockRelId 加锁。与 LockRelationOid 相同，但以 LockRelId 作为
 * 输入。
 */
void
LockRelationId(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

	res = LockAcquireExtended(&tag, lockmode, false, false, true, &locallock);

	/*
	 * 拿到锁之后检查失效消息；说明见
	 * LockRelationOid。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}
}

/*
 *		UnlockRelationId
 *
 * 根据 LockRelId 解锁。出于速度原因，优先于 UnlockRelationOid
 * 使用本函数。
 */
void
UnlockRelationId(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

	LockRelease(&tag, lockmode, false);
}

/*
 *		UnlockRelationOid
 *
 * 只根据关系 OID 解锁。若可以，请改用 UnlockRelationId。
 */
void
UnlockRelationOid(Oid relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SetLocktagRelationOid(&tag, relid);

	LockRelease(&tag, lockmode, false);
}

/*
 *		LockRelation
 *
 * 这是在已经打开的关系上再取一把锁的便捷例程。切勿先调用
 * "relation_open(foo, NoLock)"，
 * 然后再用本函数加锁。
 */
void
LockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SET_LOCKTAG_RELATION(tag,
						 relation->rd_lockInfo.lockRelId.dbId,
						 relation->rd_lockInfo.lockRelId.relId);

	res = LockAcquireExtended(&tag, lockmode, false, false, true, &locallock);

	/*
	 * 拿到锁之后检查失效消息；说明见
	 * LockRelationOid。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}
}

/*
 *		ConditionalLockRelation
 *
 * 这是在已经打开的关系上再取一把锁的便捷例程。切勿先调用
 * "relation_open(foo, NoLock)"，
 * 然后再用本函数加锁。
 */
bool
ConditionalLockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SET_LOCKTAG_RELATION(tag,
						 relation->rd_lockInfo.lockRelId.dbId,
						 relation->rd_lockInfo.lockRelId.relId);

	res = LockAcquireExtended(&tag, lockmode, false, true, true, &locallock);

	if (res == LOCKACQUIRE_NOT_AVAIL)
		return false;

	/*
	 * 拿到锁之后检查失效消息；说明见
	 * LockRelationOid。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}

	return true;
}

/*
 *		UnlockRelation
 *
 * 这是只解锁关系、并不一并
 * 关闭它的便捷例程。
 */
void
UnlockRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag,
						 relation->rd_lockInfo.lockRelId.dbId,
						 relation->rd_lockInfo.lockRelId.relId);

	LockRelease(&tag, lockmode, false);
}

/*
 *		CheckRelationLockedByMe
 *
 * 若当前事务以 'lockmode' 模式持有 'relation' 上的锁，则返回真。
 * 若 'orstronger' 为真，更强的锁模式也可以。
 * （"更强" 定义为 "数值更高"，语义上
 * 略有可疑，但对我们的用途来说没有问题。）
 */
bool
CheckRelationLockedByMe(Relation relation, LOCKMODE lockmode, bool orstronger)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag,
						 relation->rd_lockInfo.lockRelId.dbId,
						 relation->rd_lockInfo.lockRelId.relId);

	return (orstronger ?
			LockOrStrongerHeldByMe(&tag, lockmode) :
			LockHeldByMe(&tag, lockmode));
}

/*
 *		CheckRelationOidLockedByMe
 *
 * 与上面类似，但参数是 OID。
 */
bool
CheckRelationOidLockedByMe(Oid relid, LOCKMODE lockmode, bool orstronger)
{
	LOCKTAG		tag;

	SetLocktagRelationOid(&tag, relid);

	return (orstronger ?
			LockOrStrongerHeldByMe(&tag, lockmode) :
			LockHeldByMe(&tag, lockmode));
}

/*
 *		LockHasWaitersRelation
 *
 * 本函数检查是否有别人正在等待
 * 我们当前持有的那把锁。
 */
bool
LockHasWaitersRelation(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag,
						 relation->rd_lockInfo.lockRelId.dbId,
						 relation->rd_lockInfo.lockRelId.relId);

	return LockHasWaiters(&tag, lockmode, false);
}

/*
 *		LockRelationIdForSession
 *
 * 本例程获取目标关系上的会话级锁。该
 * 会话锁跨越事务边界持续存在。它会在调用
 * UnlockRelationIdForSession()、发生 ereport(ERROR)，
 * 或后端退出时被去掉。
 *
 * 注意：在实际使用该关系的任何事务中，还应再获取
 * 一把事务级锁，以确保
 * relcache 项是最新的。
 */
void
LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

	(void) LockAcquire(&tag, lockmode, true, false);
}

/*
 *		UnlockRelationIdForSession -- 释放会话级关系锁
 */
void
UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag, relid->dbId, relid->relId);

	LockRelease(&tag, lockmode, true);
}

/*
 *		LockRelationForExtension
 *
 * 此锁标签用来互锁向关系追加页面的操作。
 * 需要这种锁定，是因为 bufmgr/smgr 对 P_NEW 的定义
 * 并不能防止竞态条件。
 *
 * 假定调用者已经持有该关系上某种常规锁，
 * 因此这里不必调用 AcceptInvalidationMessages。
 */
void
LockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION_EXTEND(tag,
								relation->rd_lockInfo.lockRelId.dbId,
								relation->rd_lockInfo.lockRelId.relId);

	(void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *		ConditionalLockRelationForExtension
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 当且仅当已获得锁时返回真。
 */
bool
ConditionalLockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION_EXTEND(tag,
								relation->rd_lockInfo.lockRelId.dbId,
								relation->rd_lockInfo.lockRelId.relId);

	return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *		RelationExtensionLockWaiterCount
 *
 * 统计正在等待给定关系扩展锁的进程数。
 */
int
RelationExtensionLockWaiterCount(Relation relation)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION_EXTEND(tag,
								relation->rd_lockInfo.lockRelId.dbId,
								relation->rd_lockInfo.lockRelId.relId);

	return LockWaiterCount(&tag);
}

/*
 *		UnlockRelationForExtension -- 释放关系扩展锁
 */
void
UnlockRelationForExtension(Relation relation, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION_EXTEND(tag,
								relation->rd_lockInfo.lockRelId.dbId,
								relation->rd_lockInfo.lockRelId.relId);

	LockRelease(&tag, lockmode, false);
}

/*
 *		LockDatabaseFrozenIds
 *
 * 允许每个数据库有一个后端执行 vac_update_datfrozenxid()。
 */
void
LockDatabaseFrozenIds(LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_DATABASE_FROZEN_IDS(tag, MyDatabaseId);

	(void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *		LockPage
 *
 * 获取页级锁。目前某些索引访问
 * 方法用它锁定各个索引页。
 */
void
LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag,
					 relation->rd_lockInfo.lockRelId.dbId,
					 relation->rd_lockInfo.lockRelId.relId,
					 blkno);

	(void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *		ConditionalLockPage
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 当且仅当已获得锁时返回真。
 */
bool
ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag,
					 relation->rd_lockInfo.lockRelId.dbId,
					 relation->rd_lockInfo.lockRelId.relId,
					 blkno);

	return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *		UnlockPage -- 释放页级锁
 */
void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag,
					 relation->rd_lockInfo.lockRelId.dbId,
					 relation->rd_lockInfo.lockRelId.relId,
					 blkno);

	LockRelease(&tag, lockmode, false);
}

/*
 *		LockTuple
 *
 * 获取元组级锁。用法并不那么直观，
 * 因为我们无法为每个元组在共享内存里单独放一把锁。
 * 使用之前请先看 heap_lock_tuple！
 */
void
LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TUPLE(tag,
					  relation->rd_lockInfo.lockRelId.dbId,
					  relation->rd_lockInfo.lockRelId.relId,
					  ItemPointerGetBlockNumber(tid),
					  ItemPointerGetOffsetNumber(tid));

	(void) LockAcquire(&tag, lockmode, false, false);
}

/*
 *		ConditionalLockTuple
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 当且仅当已获得锁时返回真。
 */
bool
ConditionalLockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TUPLE(tag,
					  relation->rd_lockInfo.lockRelId.dbId,
					  relation->rd_lockInfo.lockRelId.relId,
					  ItemPointerGetBlockNumber(tid),
					  ItemPointerGetOffsetNumber(tid));

	return (LockAcquire(&tag, lockmode, false, true) != LOCKACQUIRE_NOT_AVAIL);
}

/*
 *		UnlockTuple -- 释放元组级锁
 */
void
UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TUPLE(tag,
					  relation->rd_lockInfo.lockRelId.dbId,
					  relation->rd_lockInfo.lockRelId.relId,
					  ItemPointerGetBlockNumber(tid),
					  ItemPointerGetOffsetNumber(tid));

	LockRelease(&tag, lockmode, false);
}

/*
 *		XactLockTableInsert
 *
 * 插入一把锁，表明给定的事务 ID 正在运行——
 * 当事务或子事务获得 XID 时做这件事。
 * 随后可以用这把锁等待该事务结束。
 */
void
XactLockTableInsert(TransactionId xid)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TRANSACTION(tag, xid);

	(void) LockAcquire(&tag, ExclusiveLock, false, false);
}

/*
 *		XactLockTableDelete
 *
 * 删掉表明给定事务 ID 正在运行的那把锁。
 * （主事务 ID 从不用它；那些锁只在
 * 事务结束时隐式释放。但子事务 ID 会用到。）
 */
void
XactLockTableDelete(TransactionId xid)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TRANSACTION(tag, xid);

	LockRelease(&tag, ExclusiveLock, false);
}

/*
 *		XactLockTableWait
 *
 * 等待指定事务提交或中止。若指定了某项操作，
 * 就建立错误上下文回调。若传入的 'oper' 为
 * None，则不建立错误上下文回调。
 *
 * 注意：这对子事务也能正确处理：若等待的是
 * 子事务，则它一中止或其顶层父事务一提交，我们就会退出。
 * 为保证这一点需要多做一些工作，因为为节省共享内存，
 * 子事务收尾时就会释放它的 XID 锁，无论
 * 成功还是失败。因此必须检查它是否 "仍在运行"，
 * 若是则转而等待其父事务。
 */
void
XactLockTableWait(TransactionId xid, Relation rel, ItemPointer ctid,
				  XLTW_Oper oper)
{
	LOCKTAG		tag;
	XactLockTableWaitInfo info;
	ErrorContextCallback callback;
	bool		first = true;

	/*
	 * 若指定了操作，就建立我们的详细错误上下文
	 * 回调。
	 */
	if (oper != XLTW_None)
	{
		Assert(RelationIsValid(rel));
		Assert(ItemPointerIsValid(ctid));

		info.rel = rel;
		info.ctid = ctid;
		info.oper = oper;

		callback.callback = XactLockTableWaitErrorCb;
		callback.arg = &info;
		callback.previous = error_context_stack;
		error_context_stack = &callback;
	}

	for (;;)
	{
		Assert(TransactionIdIsValid(xid));
		Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

		SET_LOCKTAG_TRANSACTION(tag, xid);

		(void) LockAcquire(&tag, ShareLock, false, false);

		LockRelease(&tag, ShareLock, false);

		if (!TransactionIdIsInProgress(xid))
			break;

		/*
		 * 若该 Xid 属于某个子事务，锁会在它
		 * 一结束就消失；为了得到正确的元组可见性，
		 * 正确的做法是等待其父事务消失。
		 * 但不必逐级向上，可以直接等待
		 * 最顶层事务结束，最终结果相同，而且
		 * 锁表访问量也更少。
		 *
		 * 本函数的某些用途并不涉及元组可见性，例如
		 * 为逻辑解码构建快照时。有可能在事务
		 * 向锁表登记自己之前，就在 ProcArray 中看到它。
		 * 此时最顶层事务就是同一个 xid，
		 * 于是短暂睡眠后重试。（第一次经过时不要睡眠，
		 * 以免拖慢正常情况。）
		 */
		if (!first)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
		}
		first = false;
		xid = SubTransGetTopmostTransaction(xid);
	}

	if (oper != XLTW_None)
		error_context_stack = callback.previous;
}

/*
 *		ConditionalXactLockTableWait
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 若已获得锁则返回真。
 */
bool
ConditionalXactLockTableWait(TransactionId xid)
{
	LOCKTAG		tag;
	bool		first = true;

	for (;;)
	{
		Assert(TransactionIdIsValid(xid));
		Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

		SET_LOCKTAG_TRANSACTION(tag, xid);

		if (LockAcquire(&tag, ShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
			return false;

		LockRelease(&tag, ShareLock, false);

		if (!TransactionIdIsInProgress(xid))
			break;

		/* 这种情况见 XactLockTableWait */
		if (!first)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
		}
		first = false;
		xid = SubTransGetTopmostTransaction(xid);
	}

	return true;
}

/*
 *		SpeculativeInsertionLockAcquire
 *
 * 插入一把锁，表明给定事务 ID 正在插入元组，
 * 但还没决定是否保留它。随后可以用这把锁
 * 来等待那个决定：继续完成插入，还是
 * 将这次插入中止。
 *
 * 令牌用来区分同一事务的多次
 * 插入。它会返回给调用者。
 */
uint32
SpeculativeInsertionLockAcquire(TransactionId xid)
{
	LOCKTAG		tag;

	speculativeInsertionToken++;

	/*
	 * 检查回绕。零表示没有持有令牌，因此不要用零。
	 */
	if (speculativeInsertionToken == 0)
		speculativeInsertionToken = 1;

	SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, speculativeInsertionToken);

	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	return speculativeInsertionToken;
}

/*
 *		SpeculativeInsertionLockRelease
 *
 * 删掉表明给定事务正在做推测
 * 插入元组的那把锁。
 */
void
SpeculativeInsertionLockRelease(TransactionId xid)
{
	LOCKTAG		tag;

	SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, speculativeInsertionToken);

	LockRelease(&tag, ExclusiveLock, false);
}

/*
 *		SpeculativeInsertionWait
 *
 * 等待指定事务完成或中止对一个
 * 元组的插入。
 */
void
SpeculativeInsertionWait(TransactionId xid, uint32 token)
{
	LOCKTAG		tag;

	SET_LOCKTAG_SPECULATIVE_INSERTION(tag, xid, token);

	Assert(TransactionIdIsValid(xid));
	Assert(token != 0);

	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

/*
 * XactLockTableWaitErrorCb
 *		事务锁等待的错误上下文回调。
 */
static void
XactLockTableWaitErrorCb(void *arg)
{
	XactLockTableWaitInfo *info = (XactLockTableWaitInfo *) arg;

	/*
	 * 我们也想打印模式名，但那需要一次
	 * 系统缓存查找。
	 */
	if (info->oper != XLTW_None &&
		ItemPointerIsValid(info->ctid) && RelationIsValid(info->rel))
	{
		const char *cxt;

		switch (info->oper)
		{
			case XLTW_Update:
				cxt = gettext_noop("while updating tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_Delete:
				cxt = gettext_noop("while deleting tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_Lock:
				cxt = gettext_noop("while locking tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_LockUpdated:
				cxt = gettext_noop("while locking updated version (%u,%u) of tuple in relation \"%s\"");
				break;
			case XLTW_InsertIndex:
				cxt = gettext_noop("while inserting index tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_InsertIndexUnique:
				cxt = gettext_noop("while checking uniqueness of tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_FetchUpdated:
				cxt = gettext_noop("while rechecking updated tuple (%u,%u) in relation \"%s\"");
				break;
			case XLTW_RecheckExclusionConstr:
				cxt = gettext_noop("while checking exclusion constraint on tuple (%u,%u) in relation \"%s\"");
				break;

			default:
				return;
		}

		errcontext(cxt,
				   ItemPointerGetBlockNumber(info->ctid),
				   ItemPointerGetOffsetNumber(info->ctid),
				   RelationGetRelationName(info->rel));
	}
}

/*
 * WaitForLockersMultiple
 *		一直等到没有事务以给定 lockmode 持有
 *		与给定 locktags 冲突的锁。
 *
 * 做法是取得当前持锁者列表，等待它们的 VXIDs，
 * 直到这些持锁者结束。
 *
 * 注意：我们并不去获取给定 locktags 上的锁，只用
 * 持锁者的 VXIDs 与 XIDs；若有人在我们取得初始
 * 持锁者列表之后，又在这些对象上拿到冲突锁，我们不会
 * 等待他们。
 */
void
WaitForLockersMultiple(List *locktags, LOCKMODE lockmode, bool progress)
{
	List	   *holders = NIL;
	ListCell   *lc;
	int			total = 0;
	int			done = 0;

	/* 若没有需要等待的锁，则直接返回 */
	if (locktags == NIL)
		return;

	/* 收集需要等待的那些事务 */
	foreach(lc, locktags)
	{
		LOCKTAG    *locktag = lfirst(lc);
		int			count;

		holders = lappend(holders,
						  GetLockConflicts(locktag, lockmode,
										   progress ? &count : NULL));
		if (progress)
			total += count;
	}

	if (progress)
		pgstat_progress_update_param(PROGRESS_WAITFOR_TOTAL, total);

	/*
	 * 注意：GetLockConflicts() 从不报告我们自己的 xid，因此不必
	 * 为此检查。预备事务也会被报告并等待。
	 */

	/* 最后等待每一个这样的事务完成 */
	foreach(lc, holders)
	{
		VirtualTransactionId *lockholders = lfirst(lc);

		while (VirtualTransactionIdIsValid(*lockholders))
		{
			/* 若有请求，就公布我们将要等待的是谁。 */
			if (progress)
			{
				PGPROC	   *holder = BackendIdGetProc(lockholders->backendId);

				if (holder)
					pgstat_progress_update_param(PROGRESS_WAITFOR_CURRENT_PID,
												 holder->pid);
			}
			VirtualXactLock(*lockholders, true);
			lockholders++;

			if (progress)
				pgstat_progress_update_param(PROGRESS_WAITFOR_DONE, ++done);
		}
	}
	if (progress)
	{
		const int	index[] = {
			PROGRESS_WAITFOR_TOTAL,
			PROGRESS_WAITFOR_DONE,
			PROGRESS_WAITFOR_CURRENT_PID
		};
		const int64 values[] = {
			0, 0, 0
		};

		pgstat_progress_update_multi_param(3, index, values);
	}

	list_free_deep(holders);
}

/*
 * WaitForLockers
 *
 * 与 WaitForLockersMultiple 相同，但只针对单个锁标签。
 */
void
WaitForLockers(LOCKTAG heaplocktag, LOCKMODE lockmode, bool progress)
{
	List	   *l;

	l = list_make1(&heaplocktag);
	WaitForLockersMultiple(l, lockmode, progress);
	list_free(l);
}


/*
 *		LockDatabaseObject
 *
 * 获取当前数据库中一般对象上的锁。不要把它用于
 * 共享对象（例如表空间）。用于关系也不适宜，
 * 因为这样取得的锁不会与经由 LockRelation
 * 及其同类函数取得的锁冲突。
 */
void
LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
				   LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   MyDatabaseId,
					   classid,
					   objid,
					   objsubid);

	(void) LockAcquire(&tag, lockmode, false, false);

	/* 确保系统缓存反映我们所等待的那些变更 */
	AcceptInvalidationMessages();
}

/*
 *		ConditionalLockDatabaseObject
 *
 * 同上，但仅在不阻塞就能获得锁时才加锁。
 * 当且仅当已获得锁时返回真。
 */
bool
ConditionalLockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
							  LOCKMODE lockmode)
{
	LOCKTAG		tag;
	LOCALLOCK  *locallock;
	LockAcquireResult res;

	SET_LOCKTAG_OBJECT(tag,
					   MyDatabaseId,
					   classid,
					   objid,
					   objsubid);

	res = LockAcquireExtended(&tag, lockmode, false, true, true, &locallock);

	if (res == LOCKACQUIRE_NOT_AVAIL)
		return false;

	/*
	 * 拿到锁之后检查失效消息；说明见
	 * LockRelationOid。
	 */
	if (res != LOCKACQUIRE_ALREADY_CLEAR)
	{
		AcceptInvalidationMessages();
		MarkLockClear(locallock);
	}

	return true;
}

/*
 *		UnlockDatabaseObject -- 释放数据库对象锁
 */
void
UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
					 LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   MyDatabaseId,
					   classid,
					   objid,
					   objsubid);

	LockRelease(&tag, lockmode, false);
}

/*
 *		LockSharedObject
 *
 * 获取跨数据库共享的对象上的锁。
 */
void
LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
				 LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   InvalidOid,
					   classid,
					   objid,
					   objsubid);

	(void) LockAcquire(&tag, lockmode, false, false);

	/* 确保系统缓存反映我们所等待的那些变更 */
	AcceptInvalidationMessages();
}

/*
 *		UnlockSharedObject -- 释放共享对象锁
 */
void
UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
				   LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   InvalidOid,
					   classid,
					   objid,
					   objsubid);

	LockRelease(&tag, lockmode, false);
}

/*
 *		LockSharedObjectForSession
 *
 * 获取跨数据库共享对象上的会话级锁。
 * 关于会话级锁的说明见 LockRelationIdForSession。
 */
void
LockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
						   LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   InvalidOid,
					   classid,
					   objid,
					   objsubid);

	(void) LockAcquire(&tag, lockmode, true, false);
}

/*
 *		UnlockSharedObjectForSession -- 释放会话级共享对象锁
 */
void
UnlockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
							 LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag,
					   InvalidOid,
					   classid,
					   objid,
					   objsubid);

	LockRelease(&tag, lockmode, true);
}

/*
 *		LockApplyTransactionForSession
 *
 * 获取正在逻辑复制订阅者上
 * 应用的事务的会话级锁。会话级锁的说明
 * 见 LockRelationIdForSession。
 */
void
LockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
							   LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_APPLY_TRANSACTION(tag,
								  MyDatabaseId,
								  suboid,
								  xid,
								  objid);

	(void) LockAcquire(&tag, lockmode, true, false);
}

/*
 *		UnlockApplyTransactionForSession -- 释放会话级应用事务锁
 */
void
UnlockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
								 LOCKMODE lockmode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_APPLY_TRANSACTION(tag,
								  MyDatabaseId,
								  suboid,
								  xid,
								  objid);

	LockRelease(&tag, lockmode, true);
}

/*
 * 把可加锁对象的描述追加到 buf。
 *
 * 理想情况下会为这些数值打印名称，但那需要
 * 获取系统表上的锁，而这可能出问题，因为本函数
 * 通常用来报告死锁情况。
 */
void
DescribeLockTag(StringInfo buf, const LOCKTAG *tag)
{
	switch ((LockTagType) tag->locktag_type)
	{
		case LOCKTAG_RELATION:
			appendStringInfo(buf,
							 _("relation %u of database %u"),
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_RELATION_EXTEND:
			appendStringInfo(buf,
							 _("extension of relation %u of database %u"),
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_DATABASE_FROZEN_IDS:
			appendStringInfo(buf,
							 _("pg_database.datfrozenxid of database %u"),
							 tag->locktag_field1);
			break;
		case LOCKTAG_PAGE:
			appendStringInfo(buf,
							 _("page %u of relation %u of database %u"),
							 tag->locktag_field3,
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_TUPLE:
			appendStringInfo(buf,
							 _("tuple (%u,%u) of relation %u of database %u"),
							 tag->locktag_field3,
							 tag->locktag_field4,
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_TRANSACTION:
			appendStringInfo(buf,
							 _("transaction %u"),
							 tag->locktag_field1);
			break;
		case LOCKTAG_VIRTUALTRANSACTION:
			appendStringInfo(buf,
							 _("virtual transaction %d/%u"),
							 tag->locktag_field1,
							 tag->locktag_field2);
			break;
		case LOCKTAG_SPECULATIVE_TOKEN:
			appendStringInfo(buf,
							 _("speculative token %u of transaction %u"),
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_OBJECT:
			appendStringInfo(buf,
							 _("object %u of class %u of database %u"),
							 tag->locktag_field3,
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		case LOCKTAG_USERLOCK:
			/* 保留给旧的 contrib 代码，现位于 pgfoundry */
			appendStringInfo(buf,
							 _("user lock [%u,%u,%u]"),
							 tag->locktag_field1,
							 tag->locktag_field2,
							 tag->locktag_field3);
			break;
		case LOCKTAG_ADVISORY:
			appendStringInfo(buf,
							 _("advisory lock [%u,%u,%u,%u]"),
							 tag->locktag_field1,
							 tag->locktag_field2,
							 tag->locktag_field3,
							 tag->locktag_field4);
			break;
		case LOCKTAG_APPLY_TRANSACTION:
			appendStringInfo(buf,
							 _("remote transaction %u of subscription %u of database %u"),
							 tag->locktag_field3,
							 tag->locktag_field2,
							 tag->locktag_field1);
			break;
		default:
			appendStringInfo(buf,
							 _("unrecognized locktag type %d"),
							 (int) tag->locktag_type);
			break;
	}
}

/*
 * GetLockNameFromTagType
 *
 *	给定 locktag 类型，返回对应的锁名称。
 */
const char *
GetLockNameFromTagType(uint16 locktag_type)
{
	if (locktag_type > LOCKTAG_LAST_TYPE)
		return "???";
	return LockTagTypeNames[locktag_type];
}
