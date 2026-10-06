/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		两阶段提交支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/access/transam/twophase.c
 *
 * NOTES
 *		每个全局事务都关联一个全局事务
 *		标识符（GID）。客户端通过 PREPARE TRANSACTION
 *		命令为 postgres 事务指定 GID。
 *
 *		我们把所有活动的全局事务保存在共享内存数组中。
 *		发出 PREPARE TRANSACTION 命令时，会在数组中为
 *		该事务预留 GID。这必须在写入 WAL 记录之前完成，
 *		因为预留会检查重复 GIDs；若已有相同 GID 的全局
 *		事务处于预备状态，则中止当前
 *		事务。
 *
 *		全局事务（gxact）还有哑元 PGPROC；正是它让
 *		TransactionIdIsInProgress 仍将该 XID 视为在运行。它也
 *		便于作为挂接 gxact 锁的 PGPROC。
 *
 *		崩溃时用于恢复预备事务的信息，常见情况下
 *		现在保存在 WAL 中。有时从预备某个 GXACT 到提交/中止
 *		之间会隔很长时间，这时需要把预备事务数据
 *		另行记录到永久存储。这包括加锁信息、挂起的
 *		通知等。所有这些状态信息都写入
 *		pg_twophase 目录中按事务划分的状态
 *		文件。关闭前会写出所有预备
 *		事务。
 *
 *		状态数据的生命周期如下：
 *
 *		* 在 PREPARE TRANSACTION 时，后端只把状态数据写入 WAL，并
 *		  把指向该 WAL 记录起点的指针存到
 *		  gxact->prepare_start_lsn。
 *		* 若 COMMIT 发生在检查点之前，后端用 prepare_start_lsn
 *		  从 WAL 读取数据。
 *		* 检查点时把状态数据复制到 pg_twophase 目录中的文件并
 *		  fsync
 *		* 若 COMMIT 发生在检查点之后，后端从文件
 *		  读取状态数据
 *
 *		重放与复制期间，TwoPhaseState 还保存尚未落盘的
 *		活动预备事务信息。
 *
 *		twophase 记录按以下规则重放：
 *
 *		* 恢复开始时扫描一次 pg_twophase，向
 *		  TwoPhaseState 填入带 gxact->inredo 与
 *		  gxact->ondisk 标记的项。早于 redo 位置 XID 地平线的
 *		  两阶段文件数据会被丢弃。
 *		* PREPARE 的 redo 时，事务加入 TwoPhaseState->prepXacts。
 *		  这类项的 gxact->inredo 置为 true。
 *		* 检查点时遍历 TwoPhaseState->prepXacts 中
 *		  gxact->inredo 已置位且落后于 redo_horizon 的项。我们
 *		  将其保存到磁盘，再把 gxact->ondisk 改为 true。
 *		* COMMIT/ABORT 时从 TwoPhaseState->prepXacts 删除该项。
 *		  若 gxact->ondisk 为 true，还要删除磁盘上
 *		  对应项。
 *		* RecoverPreparedTransactions()、StandbyRecoverPreparedTransactions()
 *		  与 PrescanPreparedTransactions() 已改为也会遍历
 *		  尚未落盘的 gxact->inredo 项。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "replication/origin.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*
 * PGDATA 内两阶段提交文件所在的目录
 */
#define TWOPHASE_DIR "pg_twophase"

/* GUC 变量，启动后不能更改 */
int			max_prepared_xacts = 0;

/*
 * 本结构描述一个处于预备状态、或正试图成为预备状态的
 * 全局事务。
 *
 * 全局事务的生命周期是：
 *
 * 1. 检查所请求的 GID 未被占用后，在
 * TwoPhaseState->prepXacts 数组中建立一项，填入正确 GID 且 valid = false，
 * 并标记为由本后端锁定。
 *
 * 2. 成功完成预备后，将 valid = true，并把所引用的
 * PGPROC 放入全局 ProcArray。
 *
 * 3. 开始 COMMIT PREPARED 或 ROLLBACK PREPARED 时，检查该项
 * 有效且未锁定，然后把当前后端 ID 存入 locking_backend
 * 以标记锁定。这可防止并发地尝试
 * 提交或回滚同一预备事务。
 *
 * 4. COMMIT PREPARED 或 ROLLBACK PREPARED 完成后，从
 * ProcArray 与 TwoPhaseState->prepXacts 数组中移除该项，并将其归还
 * 空闲列表。
 *
 * 注意：若预备中的事务在步骤 1 与 2 之间失败，必须
 * 移除该项，以便 GID 与 GlobalTransaction 结构
 * 可被复用。参见 AtAbort_Twophase()。
 *
 * typedef struct GlobalTransactionData *GlobalTransaction 出现在
 * twophase.h
 */

typedef struct GlobalTransactionData
{
	GlobalTransaction next;		/* 空闲列表的链表链接 */
	int			pgprocno;		/* 关联哑元 PGPROC 的 ID */
	BackendId	dummyBackendId; /* 类似于后端的 backend id */
	TimestampTz prepared_at;	/* 预备时间 */

	/*
	 * 注意：每个 GXACT 需要跟踪两个 LSNs。我们跟踪
	 * 起始 LSN，因为提交预备 GXACT 时必须用该地址
	 * 从 WAL 读回状态数据。我们跟踪
	 * 结束 LSN，因为提交前需要等待到
	 * 该 LSN。
	 */
	XLogRecPtr	prepare_start_lsn;	/* 预备记录起点的 XLOG 偏移 */
	XLogRecPtr	prepare_end_lsn;	/* 预备记录终点的 XLOG 偏移 */
	TransactionId xid;			/* 该 GXACT 的 id */

	Oid			owner;			/* 执行该事务的用户 ID */
	BackendId	locking_backend;	/* 当前正在处理该事务的后端 */
	bool		valid;			/* 为 true 表示 PGPROC 项已在 proc array 中 */
	bool		ondisk;			/* 为 true 表示预备状态文件已在磁盘上 */
	bool		inredo;			/* 为 true 表示该项经由 xlog_redo 加入 */
	char		gid[GIDSIZE];	/* 赋给该预备事务的 GID */
}			GlobalTransactionData;

/*
 * 两阶段提交共享状态。对该结构的访问受
 * TwoPhaseStateLock 保护。
 */
typedef struct TwoPhaseStateData
{
	/* 空闲 GlobalTransactionData 结构链表的头 */
	GlobalTransaction freeGXacts;

	/* 有效的 prepXacts 项数量。 */
	int			numPrepXacts;

	/* 本数组有 max_prepared_xacts 个元素 */
	GlobalTransaction prepXacts[FLEXIBLE_ARRAY_MEMBER];
} TwoPhaseStateData;

static TwoPhaseStateData *TwoPhaseState;

/*
 * 当前被我们锁定的全局事务项（若有）。注意：对该变量
 * 所指向项的任何访问都必须受
 * TwoPhaseStateLock 保护，不过指针本身显然不必
 * （因为它只是本地内存）。
 */
static GlobalTransaction MyLockedGxact = NULL;

static bool twophaseExitRegistered = false;

static void RecordTransactionCommitPrepared(TransactionId xid,
											int nchildren,
											TransactionId *children,
											int nrels,
											RelFileLocator *rels,
											int nstats,
											xl_xact_stats_item *stats,
											int ninvalmsgs,
											SharedInvalidationMessage *invalmsgs,
											bool initfileinval,
											const char *gid);
static void RecordTransactionAbortPrepared(TransactionId xid,
										   int nchildren,
										   TransactionId *children,
										   int nrels,
										   RelFileLocator *rels,
										   int nstats,
										   xl_xact_stats_item *stats,
										   const char *gid);
static void ProcessRecords(char *bufptr, TransactionId xid,
						   const TwoPhaseCallback callbacks[]);
static void RemoveGXact(GlobalTransaction gxact);

static void XlogReadTwoPhaseData(XLogRecPtr lsn, char **buf, int *len);
static char *ProcessTwoPhaseBuffer(TransactionId xid,
								   XLogRecPtr prepare_start_lsn,
								   bool fromdisk, bool setParent, bool setNextXid);
static void MarkAsPreparingGuts(GlobalTransaction gxact, TransactionId xid,
								const char *gid, TimestampTz prepared_at, Oid owner,
								Oid databaseid);
static void RemoveTwoPhaseFile(TransactionId xid, bool giveWarning);
static void RecreateTwoPhaseFile(TransactionId xid, void *content, int len);

/*
 * 共享内存的初始化
 */
Size
TwoPhaseShmemSize(void)
{
	Size		size;

	/* 需要固定结构、指针数组以及 GTD 结构 */
	size = offsetof(TwoPhaseStateData, prepXacts);
	size = add_size(size, mul_size(max_prepared_xacts,
								   sizeof(GlobalTransaction)));
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_prepared_xacts,
								   sizeof(GlobalTransactionData)));

	return size;
}

void
TwoPhaseShmemInit(void)
{
	bool		found;

	TwoPhaseState = ShmemInitStruct("Prepared Transaction Table",
									TwoPhaseShmemSize(),
									&found);
	if (!IsUnderPostmaster)
	{
		GlobalTransaction gxacts;
		int			i;

		Assert(!found);
		TwoPhaseState->freeGXacts = NULL;
		TwoPhaseState->numPrepXacts = 0;

		/*
		 * 初始化空闲 GlobalTransactionData 结构的链表
		 */
		gxacts = (GlobalTransaction)
			((char *) TwoPhaseState +
			 MAXALIGN(offsetof(TwoPhaseStateData, prepXacts) +
					  sizeof(GlobalTransaction) * max_prepared_xacts));
		for (i = 0; i < max_prepared_xacts; i++)
		{
			/* 插入链表 */
			gxacts[i].next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = &gxacts[i];

			/* 与 InitProcGlobal 分配的 PGPROC 关联 */
			gxacts[i].pgprocno = PreparedXactProcs[i].pgprocno;

			/*
			 * 为每个哑元进程分配唯一 ID，使哑元
			 * 后端 ID 的范围紧接在普通
			 * 后端 ID 范围之后。我们不敢给哑元进程分配真正的
			 * 后端 ID，因为预备事务并不像真正的后端 ID 那样
			 * 参与缓存失效，但为它们保留
			 * 唯一 ID 仍然很方便。这种安排
			 * 让你可以分配大小为 (MaxBackends +
			 * max_prepared_xacts + 1) 的数组，并为每个后端和
			 * 预备事务各留一个槽。目前 multixact.c 使用
			 * 该技术。
			 */
			gxacts[i].dummyBackendId = MaxBackends + 1 + i;
		}
	}
	else
		Assert(found);
}

/*
 * 退出钩子：解锁我们正在处理的全局事务项。
 */
static void
AtProcExit_Twophase(int code, Datum arg)
{
	/* 与中止相同的逻辑 */
	AtAbort_Twophase();
}

/*
 * 中止钩子：解锁我们正在处理的全局事务项。
 */
void
AtAbort_Twophase(void)
{
	if (MyLockedGxact == NULL)
		return;

	/*
	 * 对已锁定的全局事务项该怎么办？若我们正处在
	 * 预备事务过程中，但尚未写入 WAL
	 * 记录与状态文件，则该事务不得被视为
	 * 已预备。同样，若我们正处在结束一个
	 * 已预备事务的过程中，且在已把第二阶段提交或回滚
	 * 记录写入 WAL 之后失败，则该事务也不应
	 * 再被视为已预备。这些情况下，只需从共享内存
	 * 移除该项。
	 *
	 * 否则，必须保留该项以便稍后完成该事务，
	 * 因此只需解锁。
	 *
	 * 若我们在预备期间、已写入 WAL 记录之后中止，
	 * 可能尚未把全部锁及其他状态转移到预备
	 * 事务。同样，若我们在提交或回滚期间、
	 * 已写入 WAL 记录之后中止，可能尚未释放该事务所持有的
	 * 全部资源。这些情况下，内存中的
	 * 状态可能不正确，但已经来不及撤销。
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	if (!MyLockedGxact->valid)
		RemoveGXact(MyLockedGxact);
	else
		MyLockedGxact->locking_backend = InvalidBackendId;
	LWLockRelease(TwoPhaseStateLock);

	MyLockedGxact = NULL;
}

/*
 * 在我们把状态转移到预备的 PGPROC 项之后
 * 调用。
 */
void
PostPrepare_Twophase(void)
{
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	MyLockedGxact->locking_backend = InvalidBackendId;
	LWLockRelease(TwoPhaseStateLock);

	MyLockedGxact = NULL;
}


/*
 * MarkAsPreparing
 *		为给定事务预留 GID。
 */
GlobalTransaction
MarkAsPreparing(TransactionId xid, const char *gid,
				TimestampTz prepared_at, Oid owner, Oid databaseid)
{
	GlobalTransaction gxact;
	int			i;

	if (strlen(gid) >= GIDSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("transaction identifier \"%s\" is too long",
						gid)));

	/* 若功能已禁用则立即失败 */
	if (max_prepared_xacts == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("prepared transactions are disabled"),
				 errhint("Set max_prepared_transactions to a nonzero value.")));

	/* 首次调用时注册退出钩子 */
	if (!twophaseExitRegistered)
	{
		before_shmem_exit(AtProcExit_Twophase, 0);
		twophaseExitRegistered = true;
	}

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	/* 检查冲突的 GID */
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];
		if (strcmp(gxact->gid, gid) == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("transaction identifier \"%s\" is already in use",
							gid)));
		}
	}

	/* 从空闲列表取一个空闲 gxact */
	if (TwoPhaseState->freeGXacts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached"),
				 errhint("Increase max_prepared_transactions (currently %d).",
						 max_prepared_xacts)));
	gxact = TwoPhaseState->freeGXacts;
	TwoPhaseState->freeGXacts = gxact->next;

	MarkAsPreparingGuts(gxact, xid, gid, prepared_at, owner, databaseid);

	gxact->ondisk = false;

	/* 并将其插入活动数组 */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	LWLockRelease(TwoPhaseStateLock);

	return gxact;
}

/*
 * MarkAsPreparingGuts
 *
 * 使用一个 gxact 结构并将其放入活动数组。
 * 注意：崩溃后重新加载 gxact 时也会用到；因此避免
 * 假定可以使用很多后端上下文。
 *
 * 注意：调用本函数时应持有适当的锁。
 */
static void
MarkAsPreparingGuts(GlobalTransaction gxact, TransactionId xid, const char *gid,
					TimestampTz prepared_at, Oid owner, Oid databaseid)
{
	PGPROC	   *proc;
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	Assert(gxact != NULL);
	proc = &ProcGlobal->allProcs[gxact->pgprocno];

	/* 初始化 PGPROC 项 */
	MemSet(proc, 0, sizeof(PGPROC));
	proc->pgprocno = gxact->pgprocno;
	dlist_node_init(&proc->links);
	proc->waitStatus = PROC_WAIT_STATUS_OK;
	if (LocalTransactionIdIsValid(MyProc->lxid))
	{
		/* 克隆 VXID，供 TwoPhaseGetXidByVirtualXID() 查找 */
		proc->lxid = MyProc->lxid;
		proc->backendId = MyBackendId;
	}
	else
	{
		Assert(AmStartupProcess() || !IsPostmasterEnvironment);
		/* GetLockConflicts() 用此指定在该 XID 上等待 */
		proc->lxid = xid;
		proc->backendId = InvalidBackendId;
	}
	proc->xid = xid;
	Assert(proc->xmin == InvalidTransactionId);
	proc->delayChkptFlags = 0;
	proc->statusFlags = 0;
	proc->pid = 0;
	proc->databaseId = databaseid;
	proc->roleId = owner;
	proc->tempNamespaceId = InvalidOid;
	proc->isBackgroundWorker = true;
	proc->lwWaiting = LW_WS_NOT_WAITING;
	proc->lwWaitMode = 0;
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	pg_atomic_init_u64(&proc->waitStart, 0);
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		dlist_init(&proc->myProcLocks[i]);
	/* subxid 数据必须稍后由 GXactLoadSubxactData 填充 */
	proc->subxidStatus.overflowed = false;
	proc->subxidStatus.count = 0;

	gxact->prepared_at = prepared_at;
	gxact->xid = xid;
	gxact->owner = owner;
	gxact->locking_backend = MyBackendId;
	gxact->valid = false;
	gxact->inredo = false;
	strcpy(gxact->gid, gid);

	/*
	 * 记住我们已为此 GlobalTransaction 项加上锁定。若在此之后
	 * 中止，必须释放它。
	 */
	MyLockedGxact = gxact;
}

/*
 * GXactLoadSubxactData
 *
 * 若正在持久化的事务有任何子事务，必须在
 * MarkAsPrepared() 之前调用本函数，把信息载入哑元
 * PGPROC。
 */
static void
GXactLoadSubxactData(GlobalTransaction gxact, int nsubxacts,
					 TransactionId *children)
{
	PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];

	/* GXACT 尚未有效，无需额外加锁 */
	if (nsubxacts > PGPROC_MAX_CACHED_SUBXIDS)
	{
		proc->subxidStatus.overflowed = true;
		nsubxacts = PGPROC_MAX_CACHED_SUBXIDS;
	}
	if (nsubxacts > 0)
	{
		memcpy(proc->subxids.xids, children,
			   nsubxacts * sizeof(TransactionId));
		proc->subxidStatus.count = nsubxacts;
	}
}

/*
 * MarkAsPrepared
 *		将 GXACT 标记为完全有效，并将其放入全局 ProcArray。
 *
 * lock_held 表示调用方是否已持有 TwoPhaseStateLock。
 */
static void
MarkAsPrepared(GlobalTransaction gxact, bool lock_held)
{
	/* 这里加锁可能过头，但我还没被说服…… */
	if (!lock_held)
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	Assert(!gxact->valid);
	gxact->valid = true;
	if (!lock_held)
		LWLockRelease(TwoPhaseStateLock);

	/*
	 * 放入全局 ProcArray，使 TransactionIdIsInProgress 仍将该
	 * XID 视为在运行。
	 */
	ProcArrayAdd(&ProcGlobal->allProcs[gxact->pgprocno]);
}

/*
 * LockGXact
 *		定位预备事务，并将其标记为忙于 COMMIT 或 PREPARE。
 */
static GlobalTransaction
LockGXact(const char *gid, Oid user)
{
	int			i;

	/* 首次调用时注册退出钩子 */
	if (!twophaseExitRegistered)
	{
		before_shmem_exit(AtProcExit_Twophase, 0);
		twophaseExitRegistered = true;
	}

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];

		/* 忽略尚未有效的 GIDs */
		if (!gxact->valid)
			continue;
		if (strcmp(gxact->gid, gid) != 0)
			continue;

		/* 找到了，但是否已被别人锁定？ */
		if (gxact->locking_backend != InvalidBackendId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("prepared transaction with identifier \"%s\" is busy",
							gid)));

		if (user != gxact->owner && !superuser_arg(user))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to finish prepared transaction"),
					 errhint("Must be superuser or the user that prepared the transaction.")));

		/*
		 * 注意：或许可以从另一个数据库提交；
		 * 但目前已知 NOTIFY 无法工作，
		 * 还可能有其他问题。因此先禁止，
		 * 直到有人有动力把它做出来。
		 */
		if (MyDatabaseId != proc->databaseId)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prepared transaction belongs to another database"),
					 errhint("Connect to the database where the transaction was prepared to finish it.")));

		/* 可以由我锁定它 */
		gxact->locking_backend = MyBackendId;
		MyLockedGxact = gxact;

		LWLockRelease(TwoPhaseStateLock);

		return gxact;
	}

	LWLockRelease(TwoPhaseStateLock);

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("prepared transaction with identifier \"%s\" does not exist",
					gid)));

	/* NOTREACHED */
	return NULL;
}

/*
 * RemoveGXact
 *		从共享内存数组中移除预备事务。
 *
 * 注意：调用方应已从 ProcArray 中移除它
 */
static void
RemoveGXact(GlobalTransaction gxact)
{
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		if (gxact == TwoPhaseState->prepXacts[i])
		{
			/* 从活动数组中移除 */
			TwoPhaseState->numPrepXacts--;
			TwoPhaseState->prepXacts[i] = TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts];

			/* 并放回空闲列表 */
			gxact->next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = gxact;

			return;
		}
	}

	elog(ERROR, "failed to find %p in GlobalTransaction array", gxact);
}

/*
 * 返回用户级函数 pg_prepared_xact
 * 所需的全部预备事务数组。
 *
 * 返回的数组及其全部元素都是内部数据结构的副本，
 * 以尽量缩短持有 TwoPhaseStateLock 的时间。
 *
 * WARNING —— 我们甚至会返回那些尚未完全预备好的
 * 事务。调用方若不需要它们，应自行过滤。
 *
 * 返回的数组由 palloc 分配。
 */
static int
GetPreparedTransactionList(GlobalTransaction *gxacts)
{
	GlobalTransaction array;
	int			num;
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	if (TwoPhaseState->numPrepXacts == 0)
	{
		LWLockRelease(TwoPhaseStateLock);

		*gxacts = NULL;
		return 0;
	}

	num = TwoPhaseState->numPrepXacts;
	array = (GlobalTransaction) palloc(sizeof(GlobalTransactionData) * num);
	*gxacts = array;
	for (i = 0; i < num; i++)
		memcpy(array + i, TwoPhaseState->prepXacts[i],
			   sizeof(GlobalTransactionData));

	LWLockRelease(TwoPhaseStateLock);

	return num;
}


/* pg_prepared_xact 的工作状态 */
typedef struct
{
	GlobalTransaction array;
	int			ngxacts;
	int			currIdx;
} Working_State;

/*
 * pg_prepared_xact
 *		生成每个预备事务一行的视图。
 *
 * 本函数放在这里，是为了不必导出
 * GlobalTransactionData 结构定义。
 */
Datum
pg_prepared_xact(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Working_State *status;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* 创建跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为结果元组构建 tupdesc */
		/* 这最好与 system_views.sql 中的 pg_prepared_xacts 视图一致 */
		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "transaction",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "gid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "prepared",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ownerid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "dbid",
						   OIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * 收集全部将格式化并作为结果集发出的
		 * 2PC 状态信息。
		 */
		status = (Working_State *) palloc(sizeof(Working_State));
		funcctx->user_fctx = (void *) status;

		status->ngxacts = GetPreparedTransactionList(&status->array);
		status->currIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	status = (Working_State *) funcctx->user_fctx;

	while (status->array != NULL && status->currIdx < status->ngxacts)
	{
		GlobalTransaction gxact = &status->array[status->currIdx++];
		PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];
		Datum		values[5] = {0};
		bool		nulls[5] = {0};
		HeapTuple	tuple;
		Datum		result;

		if (!gxact->valid)
			continue;

		/*
		 * 用适当数据形成元组。
		 */

		values[0] = TransactionIdGetDatum(proc->xid);
		values[1] = CStringGetTextDatum(gxact->gid);
		values[2] = TimestampTzGetDatum(gxact->prepared_at);
		values[3] = ObjectIdGetDatum(gxact->owner);
		values[4] = ObjectIdGetDatum(proc->databaseId);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * TwoPhaseGetGXact
 *		按 XID 获取预备事务的
 *		GlobalTransaction 结构
 *
 * 若 lock_held 为 true，则不获取 TwoPhaseStateLock，因此
 * 调用方最好已持有它。
 */
static GlobalTransaction
TwoPhaseGetGXact(TransactionId xid, bool lock_held)
{
	GlobalTransaction result = NULL;
	int			i;

	static TransactionId cached_xid = InvalidTransactionId;
	static GlobalTransaction cached_gxact = NULL;

	Assert(!lock_held || LWLockHeldByMe(TwoPhaseStateLock));

	/*
	 * 在恢复、COMMIT PREPARED 或 ABORT PREPARED 期间，会对同一
	 * XID 反复调用我们。可用简单缓存节省工作。
	 */
	if (xid == cached_xid)
		return cached_gxact;

	if (!lock_held)
		LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		if (gxact->xid == xid)
		{
			result = gxact;
			break;
		}
	}

	if (!lock_held)
		LWLockRelease(TwoPhaseStateLock);

	if (result == NULL)			/* 不应发生 */
		elog(ERROR, "failed to find GlobalTransaction for xid %u", xid);

	cached_xid = xid;
	cached_gxact = result;

	return result;
}

/*
 * TwoPhaseGetXidByVirtualXID
 *		在自上次启动以来预备的事务中查找 VXID。
 *
 * （找不到已恢复的事务。）若有多个匹配，返回任意一个
 * 并将 "have_more" 置为 true。要看到多个匹配，单个
 * BackendId 必须消耗 2^32 个 LXIDs，且中间没有数据库重启。
 */
TransactionId
TwoPhaseGetXidByVirtualXID(VirtualTransactionId vxid,
						   bool *have_more)
{
	int			i;
	TransactionId result = InvalidTransactionId;

	Assert(VirtualTransactionIdIsValid(vxid));
	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGPROC	   *proc;
		VirtualTransactionId proc_vxid;

		if (!gxact->valid)
			continue;
		proc = &ProcGlobal->allProcs[gxact->pgprocno];
		GET_VXID_FROM_PGPROC(proc_vxid, *proc);
		if (VirtualTransactionIdEquals(vxid, proc_vxid))
		{
			/* 启动进程把 proc->backendId 设为 InvalidBackendId。 */
			Assert(!gxact->inredo);

			if (result != InvalidTransactionId)
			{
				*have_more = true;
				break;
			}
			result = gxact->xid;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	return result;
}

/*
 * TwoPhaseGetDummyBackendId
 *		按 XID 获取预备事务的哑元后端 ID
 *
 * 哑元后端 ID 类似于真正后端的真正后端 ID。
 * 它们从 MaxBackends + 1 起，并在当前全部活动的
 * 真正后端与预备事务中唯一。若 lock_held 为 true，
 * 则不获取 TwoPhaseStateLock，因此调用方最好已持有它。
 */
BackendId
TwoPhaseGetDummyBackendId(TransactionId xid, bool lock_held)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid, lock_held);

	return gxact->dummyBackendId;
}

/*
 * TwoPhaseGetDummyProc
 *		按 XID 获取表示预备事务的 PGPROC
 *
 * 若 lock_held 为 true，则不获取 TwoPhaseStateLock，因此
 * 调用方最好已持有它。
 */
PGPROC *
TwoPhaseGetDummyProc(TransactionId xid, bool lock_held)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid, lock_held);

	return &ProcGlobal->allProcs[gxact->pgprocno];
}

/************************************************************************/
/* 状态文件支持													*/
/************************************************************************/

#define TwoPhaseFilePath(path, xid) \
	snprintf(path, MAXPGPATH, TWOPHASE_DIR "/%08X", xid)

/*
 * 2PC 状态文件格式：
 *
 *	1. TwoPhaseFileHeader
 *	2. TransactionId[]（子事务）
 *	3. RelFileLocator[]（提交时要删除的文件）
 *	4. RelFileLocator[]（中止时要删除的文件）
 *	5. SharedInvalidationMessage[]（提交时要发送的失效消息）
 *	6. TwoPhaseRecordOnDisk
 *	7. ...
 *	8. TwoPhaseRecordOnDisk（结束哨兵，rmid == TWOPHASE_RM_END_ID）
 *	9. 校验和（CRC-32C）
 *
 * 除最后的校验和外，每段都按 MAXALIGN 对齐。
 */

/*
 * 2PC 状态文件头
 */
#define TWOPHASE_MAGIC	0x57F94534	/* 格式标识符 */

typedef xl_xact_prepare TwoPhaseFileHeader;

/*
 * 状态文件中每条记录的头部
 *
 * 注意：len 只计 rmgr 数据，不含 TwoPhaseRecordOnDisk 头。
 * rmgr 数据将从 MAXALIGN 边界开始存放。
 */
typedef struct TwoPhaseRecordOnDisk
{
	uint32		len;			/* rmgr 数据长度 */
	TwoPhaseRmgrId rmid;		/* 本记录的资源管理器 */
	uint16		info;			/* 供 rmgr 使用的标志位 */
} TwoPhaseRecordOnDisk;

/*
 * 预备期间，状态文件先在内存中组装，再写入
 * WAL 与实际状态文件。为此我们使用一串
 * StateFileChunk 块。
 */
typedef struct StateFileChunk
{
	char	   *data;
	uint32		len;
	struct StateFileChunk *next;
} StateFileChunk;

static struct xllist
{
	StateFileChunk *head;		/* 链中第一个数据块 */
	StateFileChunk *tail;		/* 链中最后一块 */
	uint32		num_chunks;
	uint32		bytes_free;		/* 尾块中剩余的空闲字节 */
	uint32		total_len;		/* 链中数据字节总数 */
}			records;


/*
 * 向 records 数据结构追加一块数据。
 *
 * 注意：每块都填充到 MAXALIGN 的倍数。稍后
 * 读取文件时必须计入这一点！
 *
 * 数据会被复制，因此调用方之后可自由修改它。
 */
static void
save_state_data(const void *data, uint32 len)
{
	uint32		padlen = MAXALIGN(len);

	if (padlen > records.bytes_free)
	{
		records.tail->next = palloc0(sizeof(StateFileChunk));
		records.tail = records.tail->next;
		records.tail->len = 0;
		records.tail->next = NULL;
		records.num_chunks++;

		records.bytes_free = Max(padlen, 512);
		records.tail->data = palloc(records.bytes_free);
	}

	memcpy(((char *) records.tail->data) + records.tail->len, data, len);
	records.tail->len += padlen;
	records.bytes_free -= padlen;
	records.total_len += padlen;
}

/*
 * 开始预备状态文件。
 *
 * 初始化数据结构并插入 2PC 文件头记录。
 */
void
StartPrepare(GlobalTransaction gxact)
{
	PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];
	TransactionId xid = gxact->xid;
	TwoPhaseFileHeader hdr;
	TransactionId *children;
	RelFileLocator *commitrels;
	RelFileLocator *abortrels;
	xl_xact_stats_item *abortstats = NULL;
	xl_xact_stats_item *commitstats = NULL;
	SharedInvalidationMessage *invalmsgs;

	/* 初始化链表 */
	records.head = palloc0(sizeof(StateFileChunk));
	records.head->len = 0;
	records.head->next = NULL;

	records.bytes_free = Max(sizeof(TwoPhaseFileHeader), 512);
	records.head->data = palloc(records.bytes_free);

	records.tail = records.head;
	records.num_chunks = 1;

	records.total_len = 0;

	/* 创建头部 */
	hdr.magic = TWOPHASE_MAGIC;
	hdr.total_len = 0;			/* EndPrepare 会填写此项 */
	hdr.xid = xid;
	hdr.database = proc->databaseId;
	hdr.prepared_at = gxact->prepared_at;
	hdr.owner = gxact->owner;
	hdr.nsubxacts = xactGetCommittedChildren(&children);
	hdr.ncommitrels = smgrGetPendingDeletes(true, &commitrels);
	hdr.nabortrels = smgrGetPendingDeletes(false, &abortrels);
	hdr.ncommitstats =
		pgstat_get_transactional_drops(true, &commitstats);
	hdr.nabortstats =
		pgstat_get_transactional_drops(false, &abortstats);
	hdr.ninvalmsgs = xactGetCommittedInvalidationMessages(&invalmsgs,
														  &hdr.initfileinval);
	hdr.gidlen = strlen(gxact->gid) + 1;	/* 包含 '\0' */
	/* EndPrepare 在必要时会填写 origin 数据 */
	hdr.origin_lsn = InvalidXLogRecPtr;
	hdr.origin_timestamp = 0;

	save_state_data(&hdr, sizeof(TwoPhaseFileHeader));
	save_state_data(gxact->gid, hdr.gidlen);

	/*
	 * 追加关于子事务、可删除文件与缓存
	 * 失效消息的附加信息。
	 */
	if (hdr.nsubxacts > 0)
	{
		save_state_data(children, hdr.nsubxacts * sizeof(TransactionId));
		/* 既然已有子事务数据，也塞进 gxact */
		GXactLoadSubxactData(gxact, hdr.nsubxacts, children);
	}
	if (hdr.ncommitrels > 0)
	{
		save_state_data(commitrels, hdr.ncommitrels * sizeof(RelFileLocator));
		pfree(commitrels);
	}
	if (hdr.nabortrels > 0)
	{
		save_state_data(abortrels, hdr.nabortrels * sizeof(RelFileLocator));
		pfree(abortrels);
	}
	if (hdr.ncommitstats > 0)
	{
		save_state_data(commitstats,
						hdr.ncommitstats * sizeof(xl_xact_stats_item));
		pfree(commitstats);
	}
	if (hdr.nabortstats > 0)
	{
		save_state_data(abortstats,
						hdr.nabortstats * sizeof(xl_xact_stats_item));
		pfree(abortstats);
	}
	if (hdr.ninvalmsgs > 0)
	{
		save_state_data(invalmsgs,
						hdr.ninvalmsgs * sizeof(SharedInvalidationMessage));
		pfree(invalmsgs);
	}
}

/*
 * 完成状态数据的预备并将其写入 WAL。
 */
void
EndPrepare(GlobalTransaction gxact)
{
	TwoPhaseFileHeader *hdr;
	StateFileChunk *record;
	bool		replorigin;

	/* 向 2PC 记录列表添加结束哨兵 */
	RegisterTwoPhaseRecord(TWOPHASE_RM_END_ID, 0,
						   NULL, 0);

	/* 回填文件头记录中的 total_len */
	hdr = (TwoPhaseFileHeader *) records.head->data;
	Assert(hdr->magic == TWOPHASE_MAGIC);
	hdr->total_len = records.total_len + sizeof(pg_crc32c);

	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	if (replorigin)
	{
		hdr->origin_lsn = replorigin_session_origin_lsn;
		hdr->origin_timestamp = replorigin_session_origin_timestamp;
	}

	/*
	 * 若数据大小超过 MaxAllocSize，ReadTwoPhaseFile 将无法读入。
	 * 现在就检查，以免先写入文件、
	 * 再在提交时重读时才失败。
	 */
	if (hdr->total_len > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("two-phase state file maximum length exceeded")));

	/*
	 * 现在把 2PC 状态数据写入 WAL。我们让 WAL 的 CRC 保护
	 * 覆盖我们，因此无需单独计算 CRC。
	 *
	 * 这里也必须设置 DELAY_CHKPT_START；否则在 WAL 记录
	 * 刚插入后立即开始的检查点可能在未对我们的状态文件
	 * 做 fsync 的情况下完成。（这本质上与
	 * RecordTransactionCommit 为 COMMIT-到-clog-写入
	 * 使用 DELAY_CHKPT_START 的竞态同类；见那里的说明。）
	 *
	 * 我们把 PREPARE 记录的位置保存在 gxact 中，供稍后
	 * CheckPointTwoPhase 使用。
	 */
	XLogEnsureRecordSpace(0, records.num_chunks);

	START_CRIT_SECTION();

	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	XLogBeginInsert();
	for (record = records.head; record != NULL; record = record->next)
		XLogRegisterData(record->data, record->len);

	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	gxact->prepare_end_lsn = XLogInsert(RM_XACT_ID, XLOG_XACT_PREPARE);

	if (replorigin)
	{
		/* 为该复制源推进 LSNs */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   gxact->prepare_end_lsn);
	}

	XLogFlush(gxact->prepare_end_lsn);

	/* 若现在崩溃，我们已经预备好：WAL 重放会修好 */

	/* 保存记录起点，供稍后提交时读取 */
	gxact->prepare_start_lsn = ProcLastRecPtr;

	/*
	 * 将预备事务标记为有效。一旦 xact.c 把 MyProc
	 * 标记为不再运行我们的 XID（本函数返回后会立即
	 * 这样做），其他人就可以提交/回滚该事务。
	 *
	 * 注意：副作用是为预备 XID 建立哑元 ProcArray 项。
	 * 这必须在我们从 MyProc /
	 * ProcGlobal->xids[] 清除该 XID 之前发生，否则会出现窗口，使
	 * TransactionIdIsInProgress 认为该 XID 未在运行，旁观者有权
	 * 假定事务已崩溃。相反，我们会有一个同一
	 * XID 在 ProcArray 中出现两次的窗口，这是可以的。
	 */
	MarkAsPrepared(gxact, false);

	/*
	 * 现在可以把我们自己标为已离开提交临界区：在此之后
	 * 开始的检查点肯定会把该 gxact 视为
	 * fsync 候选。
	 */
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

	/*
	 * 记住我们已为此 GlobalTransaction 项加上锁定。若在此点之后
	 * 崩溃，中止已太晚，但我们必须解锁，
	 * 以便预备事务仍可被提交或回滚。
	 */
	MyLockedGxact = gxact;

	END_CRIT_SECTION();

	/*
	 * 若需要，等待同步复制。
	 *
	 * 注意：此阶段我们已标记预备，但在 procarray 中仍显示为
	 * 在运行（两次！），并继续持有锁。
	 */
	SyncRepWaitForLSN(gxact->prepare_end_lsn, false);

	records.tail = records.head = NULL;
	records.num_chunks = 0;
}

/*
 * 注册一条将写入状态文件的 2PC 记录。
 */
void
RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, uint16 info,
					   const void *data, uint32 len)
{
	TwoPhaseRecordOnDisk record;

	record.rmid = rmid;
	record.info = info;
	record.len = len;
	save_state_data(&record, sizeof(TwoPhaseRecordOnDisk));
	if (len > 0)
		save_state_data(data, len);
}


/*
 * 读取并验证 xid 的状态文件。
 *
 * 若看起来正常（有有效魔数与 CRC），返回文件内容的
 * palloc 副本；发现损坏数据时报告错误。若
 * missing_ok 为 true，表示可安全忽略缺失文件，
 * 则返回 NULL。恢复时可能到达这种状态。
 */
static char *
ReadTwoPhaseFile(TransactionId xid, bool missing_ok)
{
	char		path[MAXPGPATH];
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			fd;
	struct stat stat;
	uint32		crc_offset;
	pg_crc32c	calc_crc,
				file_crc;
	int			r;

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (missing_ok && errno == ENOENT)
			return NULL;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	/*
	 * 检查文件长度。下界很容易确定。我们
	 * 设上界以避免损坏文件上的 palloc() 失败，不过
	 * 即使是有效文件，也不能保证不会
	 * 内存不足。
	 */
	if (fstat(fd, &stat))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	if (stat.st_size < (MAXALIGN(sizeof(TwoPhaseFileHeader)) +
						MAXALIGN(sizeof(TwoPhaseRecordOnDisk)) +
						sizeof(pg_crc32c)) ||
		stat.st_size > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_plural("incorrect size of file \"%s\": %lld byte",
							   "incorrect size of file \"%s\": %lld bytes",
							   (long long int) stat.st_size, path,
							   (long long int) stat.st_size)));

	crc_offset = stat.st_size - sizeof(pg_crc32c);
	if (crc_offset != MAXALIGN(crc_offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("incorrect alignment of CRC offset for file \"%s\"",
						path)));

	/*
	 * 好，把文件一口气读进来。
	 */
	buf = (char *) palloc(stat.st_size);

	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_READ);
	r = read(fd, buf, stat.st_size);
	if (r != stat.st_size)
	{
		if (r < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(ERROR,
					(errmsg("could not read file \"%s\": read %d of %lld",
							path, r, (long long int) stat.st_size)));
	}

	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	hdr = (TwoPhaseFileHeader *) buf;
	if (hdr->magic != TWOPHASE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid magic number stored in file \"%s\"",
						path)));

	if (hdr->total_len != stat.st_size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid size stored in file \"%s\"",
						path)));

	INIT_CRC32C(calc_crc);
	COMP_CRC32C(calc_crc, buf, crc_offset);
	FIN_CRC32C(calc_crc);

	file_crc = *((pg_crc32c *) (buf + crc_offset));

	if (!EQ_CRC32C(calc_crc, file_crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("calculated CRC checksum does not match value stored in file \"%s\"",
						path)));

	return buf;
}


/*
 * 从 xlog 读取 2PC 数据。检查点期间这些数据会移到
 * twophase 文件，届时应改用 ReadTwoPhaseFile。
 *
 * 请清楚注意：本函数在正常运行期间也可访问 WAL，
 * 与 WALSender 或逻辑解码的做法类似。
 */
static void
XlogReadTwoPhaseData(XLogRecPtr lsn, char **buf, int *len)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;

	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &read_local_xlog_page,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									NULL);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	XLogBeginRead(xlogreader, lsn);
	record = XLogReadRecord(xlogreader, &errormsg);

	if (record == NULL)
	{
		if (errormsg)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read two-phase state from WAL at %X/%X: %s",
							LSN_FORMAT_ARGS(lsn), errormsg)));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read two-phase state from WAL at %X/%X",
							LSN_FORMAT_ARGS(lsn))));
	}

	if (XLogRecGetRmid(xlogreader) != RM_XACT_ID ||
		(XLogRecGetInfo(xlogreader) & XLOG_XACT_OPMASK) != XLOG_XACT_PREPARE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("expected two-phase state data is not present in WAL at %X/%X",
						LSN_FORMAT_ARGS(lsn))));

	if (len != NULL)
		*len = XLogRecGetDataLen(xlogreader);

	*buf = palloc(sizeof(char) * XLogRecGetDataLen(xlogreader));
	memcpy(*buf, XLogRecGetData(xlogreader), sizeof(char) * XLogRecGetDataLen(xlogreader));

	XLogReaderFree(xlogreader);
}


/*
 * 在恢复期间确认某个 xid 已预备
 */
bool
StandbyTransactionIdIsPrepared(TransactionId xid)
{
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	bool		result;

	Assert(TransactionIdIsValid(xid));

	if (max_prepared_xacts <= 0)
		return false;			/* 无事可做 */

	/* 读取并验证文件 */
	buf = ReadTwoPhaseFile(xid, true);
	if (buf == NULL)
		return false;

	/* 也检查头部 */
	hdr = (TwoPhaseFileHeader *) buf;
	result = TransactionIdEquals(hdr->xid, xid);
	pfree(buf);

	return result;
}

/*
 * FinishPreparedTransaction：执行 COMMIT PREPARED 或 ROLLBACK PREPARED
 */
void
FinishPreparedTransaction(const char *gid, bool isCommit)
{
	GlobalTransaction gxact;
	PGPROC	   *proc;
	TransactionId xid;
	bool		ondisk;
	char	   *buf;
	char	   *bufptr;
	TwoPhaseFileHeader *hdr;
	TransactionId latestXid;
	TransactionId *children;
	RelFileLocator *commitrels;
	RelFileLocator *abortrels;
	RelFileLocator *delrels;
	int			ndelrels;
	xl_xact_stats_item *commitstats;
	xl_xact_stats_item *abortstats;
	SharedInvalidationMessage *invalmsgs;

	/*
	 * 验证 GID，并锁定 GXACT，确保两个后端不会
	 * 同时尝试提交同一 GID。
	 */
	gxact = LockGXact(gid, GetUserId());
	proc = &ProcGlobal->allProcs[gxact->pgprocno];
	xid = gxact->xid;

	/*
	 * 读取并验证 2PC 状态数据。若 LSN 在最后一条检查点记录之后，
	 * 状态数据通常存在 WAL 文件中；若因某种原因存活很久，
	 * 则会移到磁盘。
	 */
	if (gxact->ondisk)
		buf = ReadTwoPhaseFile(xid, false);
	else
		XlogReadTwoPhaseData(gxact->prepare_start_lsn, &buf, NULL);


	/*
	 * 拆解头部区域
	 */
	hdr = (TwoPhaseFileHeader *) buf;
	Assert(TransactionIdEquals(hdr->xid, xid));
	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
	bufptr += MAXALIGN(hdr->gidlen);
	children = (TransactionId *) bufptr;
	bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
	commitrels = (RelFileLocator *) bufptr;
	bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileLocator));
	abortrels = (RelFileLocator *) bufptr;
	bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileLocator));
	commitstats = (xl_xact_stats_item *) bufptr;
	bufptr += MAXALIGN(hdr->ncommitstats * sizeof(xl_xact_stats_item));
	abortstats = (xl_xact_stats_item *) bufptr;
	bufptr += MAXALIGN(hdr->nabortstats * sizeof(xl_xact_stats_item));
	invalmsgs = (SharedInvalidationMessage *) bufptr;
	bufptr += MAXALIGN(hdr->ninvalmsgs * sizeof(SharedInvalidationMessage));

	/* 在全部子事务中计算 latestXid */
	latestXid = TransactionIdLatest(xid, hdr->nsubxacts, children);

	/* 清理期间阻止 cancel/die 中断 */
	HOLD_INTERRUPTS();

	/*
	 * 这里的操作顺序至关重要：先为提交或中止做
	 * XLOG 项，再在 pg_xact 中把事务标记为已提交或已中止，
	 * 然后从全局 ProcArray 移除其 PGPROC（这意味着
	 * TransactionIdIsInProgress 将不再说该预备事务
	 * 在运行），再运行提交后或中止后回调。
	 * 回调会释放该事务所持有的锁。
	 */
	if (isCommit)
		RecordTransactionCommitPrepared(xid,
										hdr->nsubxacts, children,
										hdr->ncommitrels, commitrels,
										hdr->ncommitstats,
										commitstats,
										hdr->ninvalmsgs, invalmsgs,
										hdr->initfileinval, gid);
	else
		RecordTransactionAbortPrepared(xid,
									   hdr->nsubxacts, children,
									   hdr->nabortrels, abortrels,
									   hdr->nabortstats,
									   abortstats,
									   gid);

	ProcArrayRemove(proc, latestXid);

	/*
	 * 若在运行回调时失败，将 gxact 标为无效，以免
	 * 别人再尝试提交/回滚，并使我们在此点之后失败时
	 * 可回收它。它仍由我们的后端锁定，因此
	 * 暂时不会消失。
	 *
	 * （我们假定不拿 TwoPhaseStateLock 这样做是安全的。）
	 */
	gxact->valid = false;

	/*
	 * 必须删除任何本应被删除的文件。为与常规
	 * xact.c 代码路径保持一致，必须在释放锁之前做，
	 * 因此要在运行回调之前做。
	 *
	 * 注意：本代码知道我们不可能在删除任何临时关系……
	 */
	if (isCommit)
	{
		delrels = commitrels;
		ndelrels = hdr->ncommitrels;
	}
	else
	{
		delrels = abortrels;
		ndelrels = hdr->nabortrels;
	}

	/* 确保本应删除的文件已被删除 */
	DropRelationFiles(delrels, ndelrels, false);

	if (isCommit)
		pgstat_execute_transactional_drops(hdr->ncommitstats, commitstats, false);
	else
		pgstat_execute_transactional_drops(hdr->nabortstats, abortstats, false);

	/*
	 * 处理缓存失效消息。
	 *
	 * Relcache 初始化文件失效需要在发送 SI 消息之前与之后
	 * 都处理，且仅在提交时。参见
	 * AtEOXact_Inval()。
	 */
	if (isCommit)
	{
		if (hdr->initfileinval)
			RelationCacheInitFilePreInvalidate();
		SendSharedInvalidMessages(invalmsgs, hdr->ninvalmsgs);
		if (hdr->initfileinval)
			RelationCacheInitFilePostInvalidate();
	}

	/*
	 * 获取两阶段锁。我们希望在持有它的同时处理
	 * 两阶段回调，以避免与试图使用同一 GID 的其他事务
	 * 潜在冲突，因此在共享内存状态清除后
	 * 再释放该锁。
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	/* 现在执行回调 */
	if (isCommit)
		ProcessRecords(bufptr, xid, twophase_postcommit_callbacks);
	else
		ProcessRecords(bufptr, xid, twophase_postabort_callbacks);

	PredicateLockTwoPhaseFinish(xid, isCommit);

	/*
	 * 在持有两阶段锁时读取该值，因为磁盘上的 2PC
	 * 文件在锁释放后才会被物理删除。
	 */
	ondisk = gxact->ondisk;

	/* 清除共享内存状态 */
	RemoveGXact(gxact);

	/*
	 * 全部回调已调用且共享内存清理完成后，
	 * 释放该锁。
	 */
	LWLockRelease(TwoPhaseStateLock);

	/* 把该预备事务计为已提交或已中止 */
	AtEOXact_PgStat(isCommit, false);

	/*
	 * 现在可以清理我们可能留下的任何文件。
	 */
	if (ondisk)
		RemoveTwoPhaseFile(xid, true);

	MyLockedGxact = NULL;

	RESUME_INTERRUPTS();

	pfree(buf);
}

/*
 * 扫描内存中的 2PC 状态数据，并对每条 2PC 记录调用指定的回调。
 */
static void
ProcessRecords(char *bufptr, TransactionId xid,
			   const TwoPhaseCallback callbacks[])
{
	for (;;)
	{
		TwoPhaseRecordOnDisk *record = (TwoPhaseRecordOnDisk *) bufptr;

		Assert(record->rmid <= TWOPHASE_RM_MAX_ID);
		if (record->rmid == TWOPHASE_RM_END_ID)
			break;

		bufptr += MAXALIGN(sizeof(TwoPhaseRecordOnDisk));

		if (callbacks[record->rmid] != NULL)
			callbacks[record->rmid] (xid, record->info,
									 (void *) bufptr, record->len);

		bufptr += MAXALIGN(record->len);
	}
}

/*
 * 删除指定 XID 的 2PC 文件。
 *
 * 若 giveWarning 为 false，则不对"文件不存在"抱怨；
 * 这在 WAL 重放期间是预期情况。
 */
static void
RemoveTwoPhaseFile(TransactionId xid, bool giveWarning)
{
	char		path[MAXPGPATH];

	TwoPhaseFilePath(path, xid);
	if (unlink(path))
		if (errno != ENOENT || giveWarning)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
}

/*
 * 重新创建状态文件。用于 WAL 重放以及
 * 创建检查点期间。
 *
 * 注意：content 与 len 不含 CRC。
 */
static void
RecreateTwoPhaseFile(TransactionId xid, void *content, int len)
{
	char		path[MAXPGPATH];
	pg_crc32c	statefile_crc;
	int			fd;

	/* 重新计算 CRC */
	INIT_CRC32C(statefile_crc);
	COMP_CRC32C(statefile_crc, content, len);
	FIN_CRC32C(statefile_crc);

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path,
						   O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m", path)));

	/* 写入内容与 CRC */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_WRITE);
	if (write(fd, content, len) != len)
	{
		/* 若 write 未设置 errno，则假定问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	if (write(fd, &statefile_crc, sizeof(pg_crc32c)) != sizeof(pg_crc32c))
	{
		/* 若 write 未设置 errno，则假定问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();

	/*
	 * 我们必须 fsync 该文件，因为重放结束时的检查点不会
	 * 这样做——共享内存中尚无 GXACT 可告知它。
	 */
	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/*
 * CheckPointTwoPhase -- 处理检查点中的 2PC 部分。
 *
 * 我们必须对任何有效的、或在 redo 期间生成的、且
 * PREPARE LSN <= 检查点 redo 地平线的 GXACT 的状态文件
 * 做 fsync。（若 gxact 尚未有效、未在 redo 中生成，
 * 或 LSN 更晚，则本检查点不负责
 * 对其 fsync。）
 *
 * 这有意放在检查点序列尽可能靠后的位置，
 * 因为 GXACTs 通常寿命很短，因此若稍等片刻，
 * 检查点开始时有效的 GXACTs 很可能已不
 * 存在。以典型检查点设置为准，在线检查点大约
 * 3 分钟，因此我们预期不会有需要复制到磁盘的
 * GXACT。
 *
 * 若某个 GXACT 在多次检查点间仍保持有效，它应已
 * 在磁盘上，我们就不再重复写入。
 */
void
CheckPointTwoPhase(XLogRecPtr redo_horizon)
{
	int			i;
	int			serialized_xacts = 0;

	if (max_prepared_xacts <= 0)
		return;					/* 无事可做 */

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_START();

	/*
	 * 我们预期需要复制到磁盘的 GXACTs 为零，
	 * 因此为简单起见，在持有 TwoPhaseStateLock 期间执行全部 I/O。
	 * 这会阻止任何新事务在此期间预备，
	 * 这不应成问题，因为长期存活的预备事务
	 * 表明事务管理器并不活跃。
	 *
	 * 也可以把 I/O 移出锁外，但每次出错我们
	 * 都应检查是否有人在不同后端提交了我们的事务。
	 * 把这项优化留给将来，若有人发现这里
	 * 成为瓶颈再说。
	 *
	 * 注意：不可能存在 prepare_end_lsn 设在上一检查点
	 * 之前却被标为无效的 GXACT，
	 * 这得益于 delayChkptFlags 的努力。
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		/*
		 * 注意：我们用的是 gxact 而非 PGPROC，因此在恢复中
		 * 也能工作
		 */
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		if ((gxact->valid || gxact->inredo) &&
			!gxact->ondisk &&
			gxact->prepare_end_lsn <= redo_horizon)
		{
			char	   *buf;
			int			len;

			XlogReadTwoPhaseData(gxact->prepare_start_lsn, &buf, &len);
			RecreateTwoPhaseFile(gxact->xid, buf, len);
			gxact->ondisk = true;
			gxact->prepare_start_lsn = InvalidXLogRecPtr;
			gxact->prepare_end_lsn = InvalidXLogRecPtr;
			pfree(buf);
			serialized_xacts++;
		}
	}
	LWLockRelease(TwoPhaseStateLock);

	/*
	 * 无条件刷新父目录，使任何信息在磁盘上
	 * 持久化。两阶段文件可能已被删除，这些
	 * 删除以及自上一检查点以来新创建的任何文件
	 * 也都需要持久化。
	 */
	fsync_fname(TWOPHASE_DIR, true);

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_DONE();

	if (log_checkpoints && serialized_xacts > 0)
		ereport(LOG,
				(errmsg_plural("%u two-phase state file was written "
							   "for a long-running prepared transaction",
							   "%u two-phase state files were written "
							   "for long-running prepared transactions",
							   serialized_xacts,
							   serialized_xacts)));
}

/*
 * restoreTwoPhaseData
 *
 * 扫描 pg_twophase，并根据磁盘数据填充 TwoPhaseState。
 * 在恢复开始时调用一次，以免将来再做额外
 * 查找。途中会丢弃新于最小 XID 地平线的
 * 两阶段文件。
 */
void
restoreTwoPhaseData(void)
{
	DIR		   *cldir;
	struct dirent *clde;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	cldir = AllocateDir(TWOPHASE_DIR);
	while ((clde = ReadDir(cldir, TWOPHASE_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char	   *buf;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			buf = ProcessTwoPhaseBuffer(xid, InvalidXLogRecPtr,
										true, false, false);
			if (buf == NULL)
				continue;

			PrepareRedoAdd(buf, InvalidXLogRecPtr,
						   InvalidXLogRecPtr, InvalidRepOriginId);
		}
	}
	LWLockRelease(TwoPhaseStateLock);
	FreeDir(cldir);
}

/*
 * PrescanPreparedTransactions
 *
 * 扫描 TwoPhaseState 的共享内存项，并确定其中
 * 有效 XIDs 的范围。这在数据库启动期间、我们
 * 读完 WAL 之后运行。ShmemVariableCache->nextXid 已设为
 * WAL 中有证据的最高 XID 再加一。
 *
 * 我们丢弃主 XID 超出 nextXid 的任何预备事务——若存在，
 * 说明 DBA 做了到更早点的 PITR 恢复，却未
 * 清理 pg_twophase。我们不敢尝试恢复此类预备事务，
 * 因为它们很可能依赖现在已不存在的数据库
 * 状态。
 *
 * 不过，我们会把 nextXid 推进到超过属于有效预备事务的
 * 任何子事务 XID。需要这样做是因为子事务提交不
 * 写 WAL 项，因而 WAL 中可能没有那些
 * 子事务 XID 的证据。
 *
 * 遇到损坏的两阶段文件时立即失败。保留破损项
 * 并让重放继续会对系统造成损害，应换入新的
 * 备份。
 *
 * 我们的另一职责是确定并返回预备事务中
 * 最老的有效 XID（若无则返回 ShmemVariableCache->nextXid）。
 * 这是为了正确同步 pg_subtrans 的启动。
 *
 * 若 xids_p 与 nxids_p 非 NULL，则把全部顶层 xid 的
 * palloc 数组指针存入 *xids_p。数组中的项数
 * 返回在 *nxids_p 中。
 */
TransactionId
PrescanPreparedTransactions(TransactionId **xids_p, int *nxids_p)
{
	FullTransactionId nextXid = ShmemVariableCache->nextXid;
	TransactionId origNextXid = XidFromFullTransactionId(nextXid);
	TransactionId result = origNextXid;
	TransactionId *xids = NULL;
	int			nxids = 0;
	int			allocsize = 0;
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		Assert(gxact->inredo);

		xid = gxact->xid;

		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, false, true);

		if (buf == NULL)
			continue;

		/*
		 * 好，我们认为此文件有效。把 xid 纳入
		 * 滚动最小值结果。
		 */
		if (TransactionIdPrecedes(xid, result))
			result = xid;

		if (xids_p)
		{
			if (nxids == allocsize)
			{
				if (nxids == 0)
				{
					allocsize = 10;
					xids = palloc(allocsize * sizeof(TransactionId));
				}
				else
				{
					allocsize = allocsize * 2;
					xids = repalloc(xids, allocsize * sizeof(TransactionId));
				}
			}
			xids[nxids++] = xid;
		}

		pfree(buf);
	}
	LWLockRelease(TwoPhaseStateLock);

	if (xids_p)
	{
		*xids_p = xids;
		*nxids_p = nxids;
	}

	return result;
}

/*
 * StandbyRecoverPreparedTransactions
 *
 * 扫描 TwoPhaseState 的共享内存项，并建立全部所需信息，
 * 使备库查询仍把预备事务视为
 * 活动。
 *
 * 恢复结束时从不调用本函数——那时我们使用
 * RecoverPreparedTransactions()。
 *
 * 这会更新 pg_subtrans，使任何子事务在恢复期间取得的
 * 快照中被正确视为进行中。
 */
void
StandbyRecoverPreparedTransactions(void)
{
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		Assert(gxact->inredo);

		xid = gxact->xid;

		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, true, false);
		if (buf != NULL)
			pfree(buf);
	}
	LWLockRelease(TwoPhaseStateLock);
}

/*
 * RecoverPreparedTransactions
 *
 * 扫描 TwoPhaseState 的共享内存项，并为每个预备事务
 * 重新加载状态（重新获取锁等）。
 *
 * 这在恢复结束时运行，但在允许后端写
 * WAL 之前。
 *
 * 恢复结束时我们获取快照的方式会改变。现在需要
 * 用完整的 SubTransSetParent() 信息标记所有运行中的事务，
 * 以便快照溢出时普通快照仍能正确工作。
 * 放在这里是因为按定义预备事务是仍在运行的唯一
 * 写事务类型，因此这样做既必要又
 * 完整。
 */
void
RecoverPreparedTransactions(void)
{
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		char	   *bufptr;
		TwoPhaseFileHeader *hdr;
		TransactionId *subxids;
		const char *gid;

		xid = gxact->xid;

		/*
		 * 为该事务重建 subtrans 状态——需要这样做是因为
		 * pg_subtrans 不会跨重启保留。注意：我们把
		 * 全部子事务直接链接到顶层 XID；
		 * 原先可能有更复杂的层次，但
		 * 不必精确恢复。若预备事务生成过
		 * xid 分配记录，SubTransSetParent 可能此前已被
		 * 设置过。
		 */
		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, true, false);
		if (buf == NULL)
			continue;

		ereport(LOG,
				(errmsg("recovering prepared transaction %u from shared memory", xid)));

		hdr = (TwoPhaseFileHeader *) buf;
		Assert(TransactionIdEquals(hdr->xid, xid));
		bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
		gid = (const char *) bufptr;
		bufptr += MAXALIGN(hdr->gidlen);
		subxids = (TransactionId *) bufptr;
		bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
		bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileLocator));
		bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileLocator));
		bufptr += MAXALIGN(hdr->ncommitstats * sizeof(xl_xact_stats_item));
		bufptr += MAXALIGN(hdr->nabortstats * sizeof(xl_xact_stats_item));
		bufptr += MAXALIGN(hdr->ninvalmsgs * sizeof(SharedInvalidationMessage));

		/*
		 * 重新创建其 GXACT 与哑元 PGPROC。不过要检查它是否
		 * 在 redo 中加入且已有共享内存项。
		 */
		MarkAsPreparingGuts(gxact, xid, gid,
							hdr->prepared_at,
							hdr->owner, hdr->database);

		/* 已恢复，因此复位 redo 生成项的标志 */
		gxact->inredo = false;

		GXactLoadSubxactData(gxact, hdr->nsubxacts, subxids);
		MarkAsPrepared(gxact, true);

		LWLockRelease(TwoPhaseStateLock);

		/*
		 * 用资源管理器恢复其他状态（尤其是锁）。
		 */
		ProcessRecords(bufptr, xid, twophase_recover_callbacks);

		/*
		 * 处理完每个预备事务后，释放备库进程
		 * 所持有的锁。这样我们就不必在任一时刻持有
		 * 过多额外的锁。
		 */
		if (InHotStandby)
			StandbyReleaseLockTree(xid, hdr->nsubxacts, subxids);

		/*
		 * 本事务恢复完毕。清除 MyLockedGxact，
		 * 如同正常操作中 PrepareTransaction() 所做。
		 */
		PostPrepare_Twophase();

		pfree(buf);

		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	}

	LWLockRelease(TwoPhaseStateLock);
}

/*
 * ProcessTwoPhaseBuffer
 *
 * 给定事务 id，从磁盘读取，或用所提供的
 * "prepare_start_lsn" 经共享内存 xlog 记录指针直接读取。
 *
 * 若 setParent 为 true，建立子事务的父链接。
 *
 * 若 setNextXid 为 true，把 ShmemVariableCache->nextXid 设为所扫描到的
 * 最新值。
 */
static char *
ProcessTwoPhaseBuffer(TransactionId xid,
					  XLogRecPtr prepare_start_lsn,
					  bool fromdisk,
					  bool setParent, bool setNextXid)
{
	FullTransactionId nextXid = ShmemVariableCache->nextXid;
	TransactionId origNextXid = XidFromFullTransactionId(nextXid);
	TransactionId *subxids;
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	if (!fromdisk)
		Assert(prepare_start_lsn != InvalidXLogRecPtr);

	/* 已经处理过？ */
	if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
	{
		if (fromdisk)
		{
			ereport(WARNING,
					(errmsg("removing stale two-phase state file for transaction %u",
							xid)));
			RemoveTwoPhaseFile(xid, true);
		}
		else
		{
			ereport(WARNING,
					(errmsg("removing stale two-phase state from memory for transaction %u",
							xid)));
			PrepareRedoRemove(xid, true);
		}
		return NULL;
	}

	/* 若 XID 太新则拒绝 */
	if (TransactionIdFollowsOrEquals(xid, origNextXid))
	{
		if (fromdisk)
		{
			ereport(WARNING,
					(errmsg("removing future two-phase state file for transaction %u",
							xid)));
			RemoveTwoPhaseFile(xid, true);
		}
		else
		{
			ereport(WARNING,
					(errmsg("removing future two-phase state from memory for transaction %u",
							xid)));
			PrepareRedoRemove(xid, true);
		}
		return NULL;
	}

	if (fromdisk)
	{
		/* 读取并验证文件 */
		buf = ReadTwoPhaseFile(xid, false);
	}
	else
	{
		/* 读取 xlog 数据 */
		XlogReadTwoPhaseData(prepare_start_lsn, &buf, NULL);
	}

	/* 拆解头部 */
	hdr = (TwoPhaseFileHeader *) buf;
	if (!TransactionIdEquals(hdr->xid, xid))
	{
		if (fromdisk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted two-phase state file for transaction %u",
							xid)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted two-phase state in memory for transaction %u",
							xid)));
	}

	/*
	 * 检查子事务 XIDs……它们都应跟在主 XID 之后，并且
	 * 可能迫使我们推进 nextXid。
	 */
	subxids = (TransactionId *) (buf +
								 MAXALIGN(sizeof(TwoPhaseFileHeader)) +
								 MAXALIGN(hdr->gidlen));
	for (i = 0; i < hdr->nsubxacts; i++)
	{
		TransactionId subxid = subxids[i];

		Assert(TransactionIdFollows(subxid, xid));

		/* 若需要则更新 nextXid */
		if (setNextXid)
			AdvanceNextFullTransactionIdPastXid(subxid);

		if (setParent)
			SubTransSetParent(subxid, xid);
	}

	return buf;
}


/*
 *	RecordTransactionCommitPrepared
 *
 * 这基本上与 RecordTransactionCommit 相同（若改本函数请对照
 * 后者）：尤其是必须设置 DELAY_CHKPT_START 以避免
 * 竞态。
 *
 * 我们知道该事务至少写过一条 XLOG 项（其 PREPARE），
 * 因此绝不可能优化掉提交记录。
 */
static void
RecordTransactionCommitPrepared(TransactionId xid,
								int nchildren,
								TransactionId *children,
								int nrels,
								RelFileLocator *rels,
								int nstats,
								xl_xact_stats_item *stats,
								int ninvalmsgs,
								SharedInvalidationMessage *invalmsgs,
								bool initfileinval,
								const char *gid)
{
	XLogRecPtr	recptr;
	TimestampTz committs = GetCurrentTimestamp();
	bool		replorigin;

	/*
	 * 我们是否在使用复制源功能？或者说，我们是否在
	 * 重放远程动作？
	 */
	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	START_CRIT_SECTION();

	/* 见 RecordTransactionCommit 中的说明 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/*
	 * 发出 XLOG 提交记录。注意：我们把 2PC 提交标记为
	 * 可能持有 AccessExclusiveLocks，因为我们不知道它们
	 * 是否持有。
	 */
	recptr = XactLogCommitRecord(committs,
								 nchildren, children, nrels, rels,
								 nstats, stats,
								 ninvalmsgs, invalmsgs,
								 initfileinval,
								 MyXactFlags | XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK,
								 xid, gid);


	if (replorigin)
		/* 为该复制源推进 LSNs */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   XactLastRecEnd);

	/*
	 * 记录提交时间戳。若未启用 replorigin，取值来自
	 * 普通提交时间戳；否则 replorigin 已在
	 * replorigin_session_origin_timestamp 中为我们设好。
	 *
	 * 这里无需再写 WAL，因为上面写出的提交记录
	 * 已包含这些数据。
	 */
	if (!replorigin || replorigin_session_origin_timestamp == 0)
		replorigin_session_origin_timestamp = committs;

	TransactionTreeSetCommitTsData(xid, nchildren, children,
								   replorigin_session_origin_timestamp,
								   replorigin_session_origin);

	/*
	 * 我们目前不在刷盘前尝试休眠……也没有任何
	 * 对预备事务异步提交的支持（这想法本身大概
	 * 就是矛盾）
	 */

	/* 把 XLOG 刷到磁盘 */
	XLogFlush(recptr);

	/* 在 pg_xact 中把事务标记为已提交 */
	TransactionIdCommitTree(xid, nchildren, children);

	/* 检查点现在可以继续 */
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

	END_CRIT_SECTION();

	/*
	 * 若需要，等待同步复制。
	 *
	 * 注意：此阶段我们已标记 clog，但在 procarray 中仍显示为
	 * 在运行，并继续持有锁。
	 */
	SyncRepWaitForLSN(recptr, true);
}

/*
 *	RecordTransactionAbortPrepared
 *
 * 这基本上与 RecordTransactionAbort 相同。
 *
 * 我们知道该事务至少写过一条 XLOG 项（其 PREPARE），
 * 因此绝不可能优化掉中止记录。
 */
static void
RecordTransactionAbortPrepared(TransactionId xid,
							   int nchildren,
							   TransactionId *children,
							   int nrels,
							   RelFileLocator *rels,
							   int nstats,
							   xl_xact_stats_item *stats,
							   const char *gid)
{
	XLogRecPtr	recptr;
	bool		replorigin;

	/*
	 * 我们是否在使用复制源功能？或者说，我们是否在
	 * 重放远程动作？
	 */
	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	/*
	 * 捕捉我们在 RecordTransactionCommitPrepared
	 * 中途中止的情形……
	 */
	if (TransactionIdDidCommit(xid))
		elog(PANIC, "cannot abort transaction %u, it was already committed",
			 xid);

	START_CRIT_SECTION();

	/*
	 * 发出 XLOG 提交记录。注意：我们把 2PC 中止标记为
	 * 可能持有 AccessExclusiveLocks，因为我们不知道它们
	 * 是否持有。
	 */
	recptr = XactLogAbortRecord(GetCurrentTimestamp(),
								nchildren, children,
								nrels, rels,
								nstats, stats,
								MyXactFlags | XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK,
								xid, gid);

	if (replorigin)
		/* 为该复制源推进 LSNs */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   XactLastRecEnd);

	/* 总是刷盘，因为我们即将删除 2PC 状态文件 */
	XLogFlush(recptr);

	/*
	 * 在 clog 中把事务标记为已中止。这并非绝对必要，
	 * 但既然在这里，不妨一并做了。
	 */
	TransactionIdAbortTree(xid, nchildren, children);

	END_CRIT_SECTION();

	/*
	 * 若需要，等待同步复制。
	 *
	 * 注意：此阶段我们已标记 clog，但在 procarray 中仍显示为
	 * 在运行，并继续持有锁。
	 */
	SyncRepWaitForLSN(recptr, false);
}

/*
 * PrepareRedoAdd
 *
 * 把 WAL 记录起止指针连同 xid 存入共享内存
 * TwoPhaseState 结构中的 gxact 项。若调用方
 * 把用于获取两阶段数据的 WAL 位置指定为 InvalidXLogRecPtr，
 * 则该项标记为位于磁盘上。
 */
void
PrepareRedoAdd(char *buf, XLogRecPtr start_lsn,
			   XLogRecPtr end_lsn, RepOriginId origin_id)
{
	TwoPhaseFileHeader *hdr = (TwoPhaseFileHeader *) buf;
	char	   *bufptr;
	const char *gid;
	GlobalTransaction gxact;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
	gid = (const char *) bufptr;

	/*
	 * 在 redo 代码路径中为给定事务预留 GID。
	 *
	 * 这会创建 gxact 结构并将其放入活动数组。
	 *
	 * 在 redo 中，该结构主要用于在共享内存中跟踪
	 * PREPARE/COMMIT 项。因此这里只填入最少量内容。
	 * gxact 还会把 gxact->inredo 置为 true，以表明
	 * 它是在 redo 阶段加入的
	 */

	/*
	 * 若检查点运行期间发生崩溃，有可能部分
	 * 两阶段数据已落到磁盘，而其对应记录仍需在后续
	 * 恢复中重放。由于 2PC 数据已在磁盘上，恢复开始时
	 * restoreTwoPhaseData() 已恢复过它，因此跳过本记录以免
	 * TwoPhaseState 中出现重复。若已到达一致状态，
	 * 该记录会加入 TwoPhaseState，且它在 pg_twophase 中
	 * 不应有
	 * 对应文件。
	 */
	if (!XLogRecPtrIsInvalid(start_lsn))
	{
		char		path[MAXPGPATH];

		TwoPhaseFilePath(path, hdr->xid);

		if (access(path, F_OK) == 0)
		{
			ereport(reachedConsistency ? ERROR : WARNING,
					(errmsg("could not recover two-phase state file for transaction %u",
							hdr->xid),
					 errdetail("Two-phase state file has been found in WAL record %X/%X, but this transaction has already been restored from disk.",
							   LSN_FORMAT_ARGS(start_lsn))));
			return;
		}

		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access file \"%s\": %m", path)));
	}

	/* 从空闲列表取一个空闲 gxact */
	if (TwoPhaseState->freeGXacts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached"),
				 errhint("Increase max_prepared_transactions (currently %d).",
						 max_prepared_xacts)));
	gxact = TwoPhaseState->freeGXacts;
	TwoPhaseState->freeGXacts = gxact->next;

	gxact->prepared_at = hdr->prepared_at;
	gxact->prepare_start_lsn = start_lsn;
	gxact->prepare_end_lsn = end_lsn;
	gxact->xid = hdr->xid;
	gxact->owner = hdr->owner;
	gxact->locking_backend = InvalidBackendId;
	gxact->valid = false;
	gxact->ondisk = XLogRecPtrIsInvalid(start_lsn);
	gxact->inredo = true;		/* 是的，在 redo 中加入 */
	strcpy(gxact->gid, gid);

	/* 并将其插入活动数组 */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	if (origin_id != InvalidRepOriginId)
	{
		/* 恢复应用进度 */
		replorigin_advance(origin_id, hdr->origin_lsn, end_lsn,
						   false /* 向后 */ , false /* WAL */ );
	}

	elog(DEBUG2, "added 2PC data in shared memory for transaction %u", gxact->xid);
}

/*
 * PrepareRedoRemove
 *
 * 从 TwoPhaseState 移除对应的 gxact 项。若预备事务曾经由
 * 更早的检查点保存，也删除 2PC 文件。
 *
 * 调用方必须以排他模式持有 TwoPhaseStateLock，因为会更新
 * TwoPhaseState。
 */
void
PrepareRedoRemove(TransactionId xid, bool giveWarning)
{
	GlobalTransaction gxact = NULL;
	int			i;
	bool		found = false;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];

		if (gxact->xid == xid)
		{
			Assert(gxact->inredo);
			found = true;
			break;
		}
	}

	/*
	 * 若什么都没有就直接离开，这在 WAL 重放期间是预期的。
	 */
	if (!found)
		return;

	/*
	 * 现在可以清理我们可能留下的任何文件。
	 */
	elog(DEBUG2, "removing 2PC data for transaction %u", xid);
	if (gxact->ondisk)
		RemoveTwoPhaseFile(xid, giveWarning);
	RemoveGXact(gxact);
}

/*
 * LookupGXact
 *		检查具有给定 GID、lsn 与时间戳的预备事务
 *		是否存在。
 *
 * 注意：我们总是与预备结束处的 LSN 比较，因为那是
 * 2PC 文件中存为 origin_lsn 的值。
 *
 * 本函数主要用于检查从上游（远程节点）收到的
 * 预备事务是否已存在。仅检查 GID
 * 不够，因为同一节点上可能存在具有相同 GID 的不同预备事务。
 * 因此我们确保匹配预备事务的 origin_lsn 与
 * origin_timestamp，以避免匹配到来自两个不同节点的
 * 预备事务。
 */
bool
LookupGXact(const char *gid, XLogRecPtr prepare_end_lsn,
			TimestampTz origin_prepare_timestamp)
{
	int			i;
	bool		found = false;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		/* 忽略尚未有效的 GIDs。 */
		if (gxact->valid && strcmp(gxact->gid, gid) == 0)
		{
			char	   *buf;
			TwoPhaseFileHeader *hdr;

			/*
			 * 我们并不预期发布者与订阅者之间会有
			 * GXACTs（相同 gid）碰撞，因此为简单起见在持有
			 * TwoPhaseStateLock 期间执行全部 I/O。
			 *
			 * 要把 I/O 移出锁外，需要确保其间没有
			 * 其他后端提交该预备事务。若发布者与订阅者之间
			 * 遇到很多 GID 碰撞，我们再做这项
			 * 优化。
			 */
			if (gxact->ondisk)
				buf = ReadTwoPhaseFile(gxact->xid, false);
			else
			{
				Assert(gxact->prepare_start_lsn);
				XlogReadTwoPhaseData(gxact->prepare_start_lsn, &buf, NULL);
			}

			hdr = (TwoPhaseFileHeader *) buf;

			if (hdr->origin_lsn == prepare_end_lsn &&
				hdr->origin_timestamp == origin_prepare_timestamp)
			{
				found = true;
				pfree(buf);
				break;
			}

			pfree(buf);
		}
	}
	LWLockRelease(TwoPhaseStateLock);
	return found;
}
