/*-------------------------------------------------------------------------
 *
 * proc.c
 *	  管理每进程共享内存数据结构的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/proc.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口 (a)：
 *		ProcSleep()、ProcWakeup()，
 *
 * 等待一把锁会使后端进入睡眠。释放该锁的一方
 * 会再次唤醒该进程（并给出错误码，以便它知道
 * 自己是否因错误条件而被唤醒）。
 *
 * 接口 (b)：
 *
 * ProcReleaseLocks -- 释放与当前事务相关联的锁
 *
 * ProcKill -- 销毁与该进程相关联的
 * 共享内存状态（以及锁）。
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "replication/slot.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* GUC 变量 */
int			DeadlockTimeout = 1000;
int			StatementTimeout = 0;
int			LockTimeout = 0;
int			IdleInTransactionSessionTimeout = 0;
int			IdleSessionTimeout = 0;
bool		log_lock_waits = false;

/* 指向本进程 PGPROC 结构体的指针（如果有） */
PGPROC	   *MyProc = NULL;

/*
 * 这把自旋锁保护回收所得 PGPROC 结构的空闲链表。
 * 我们无法使用 LWLock，因为 LWLock 管理器依赖于已经
 * 持有一个 PGPROC 和一把等待信号量！不过这些结构被碰触的
 * 频率相对较低（仅在后端启动或关闭时），而且持续
 * 时间不长，所以用自旋锁就可以。
 */
NON_EXEC_STATIC slock_t *ProcStructLock = NULL;

/* 指向共享内存结构的指针 */
PROC_HDR   *ProcGlobal = NULL;
NON_EXEC_STATIC PGPROC *AuxiliaryProcs = NULL;
PGPROC	   *PreparedXactProcs = NULL;

/* 若我们正在等锁，此指针指向相关联的 LOCALLOCK */
static LOCALLOCK *lockAwaited = NULL;

static DeadLockState deadlock_state = DS_NOT_YET_CHECKED;

/* 是否有待处理的死锁检查？ */
static volatile sig_atomic_t got_deadlock_timeout;

static void RemoveProcFromArray(int code, Datum arg);
static void ProcKill(int code, Datum arg);
static void AuxiliaryProcKill(int code, Datum arg);
static void CheckDeadLock(void);


/*
 * 报告 InitProcGlobal 所需的共享内存空间。
 */
Size
ProcGlobalShmemSize(void)
{
	Size		size = 0;
	Size		TotalProcs =
		add_size(MaxBackends, add_size(NUM_AUXILIARY_PROCS, max_prepared_xacts));

	/* ProcGlobal 结构 */
	size = add_size(size, sizeof(PROC_HDR));
	size = add_size(size, mul_size(TotalProcs, sizeof(PGPROC)));
	size = add_size(size, sizeof(slock_t));

	size = add_size(size, mul_size(TotalProcs, sizeof(*ProcGlobal->xids)));
	size = add_size(size, mul_size(TotalProcs, sizeof(*ProcGlobal->subxidStates)));
	size = add_size(size, mul_size(TotalProcs, sizeof(*ProcGlobal->statusFlags)));

	return size;
}

/*
 * 报告 InitProcGlobal 所需的信号量个数。
 */
int
ProcGlobalSemas(void)
{
	/*
	 * 每个后端（包括自动清理）需要一个信号量，每个
	 * 辅助进程再各需一个。
	 */
	return MaxBackends + NUM_AUXILIARY_PROCS;
}

/*
 * InitProcGlobal -
 *	  在 postmaster 或独立
 *	  后端启动期间初始化全局进程表。
 *
 *	  我们还会创建为支持所需后端数量而要用到的全部每进程信号量。
 *	  从前只在后端真正启动时才分配信号量，
 *	  那样不好，因为
 *	  会让 Postgres 在负载下失败 --- 许多 Unix 系统
 *	  （错误）配置了很小的信号量数量上限，而且
 *	  试图再启动一个后端时用尽信号量是常见故障。
 *	  因此，现在我们在初始化时立刻取得足够的信号量，以支持期望的最大
 *	  后端数量 --- 若系统管理员把
 *	  MaxConnections、max_worker_processes、max_wal_senders 或
 *	  autovacuum_max_workers 设得比其内核所能支持的更高，就会
 *	  尽早发现，而不是拖到以后。
 *
 *	  在这里创建信号量的另一个原因是，信号量
 *	  实现通常需要我们在
 *	  postmaster 中创建信号量，而不是在后端中。
 *
 * 注意：在 postmaster 之下，各个后端不会调用本函数，
 * 即便是 EXEC_BACKEND 情形也不会。ProcGlobal 与 AuxiliaryProcs
 * 指针必须为 EXEC_BACKEND 操作专门传递下去。
 */
void
InitProcGlobal(void)
{
	PGPROC	   *procs;
	int			i,
				j;
	bool		found;
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;

	/* 创建 ProcGlobal 共享结构 */
	ProcGlobal = (PROC_HDR *)
		ShmemInitStruct("Proc Header", sizeof(PROC_HDR), &found);
	Assert(!found);

	/*
	 * 初始化数据结构。
	 */
	ProcGlobal->spins_per_delay = DEFAULT_SPINS_PER_DELAY;
	dlist_init(&ProcGlobal->freeProcs);
	dlist_init(&ProcGlobal->autovacFreeProcs);
	dlist_init(&ProcGlobal->bgworkerFreeProcs);
	dlist_init(&ProcGlobal->walsenderFreeProcs);
	ProcGlobal->startupBufferPinWaitBufId = -1;
	ProcGlobal->walwriterLatch = NULL;
	ProcGlobal->checkpointerLatch = NULL;
	pg_atomic_init_u32(&ProcGlobal->procArrayGroupFirst, INVALID_PGPROCNO);
	pg_atomic_init_u32(&ProcGlobal->clogGroupFirst, INVALID_PGPROCNO);

	/*
	 * 创建并初始化我们将需要的全部 PGPROC 结构。共有
	 * 五类使用者：(1) 普通后端，(2) 自动清理工作进程
	 * 与自动清理启动器，(3) 后台工作进程，(4) 辅助
	 * 进程，以及 (5) 预备事务。每个 PGPROC 结构
	 * 只专用于其中一种用途，并且不会在
	 * 各组之间移动。
	 */
	procs = (PGPROC *) ShmemAlloc(TotalProcs * sizeof(PGPROC));
	MemSet(procs, 0, TotalProcs * sizeof(PGPROC));
	ProcGlobal->allProcs = procs;
	/* XXX allProcCount 其实并非全部；它不含预备事务 */
	ProcGlobal->allProcCount = MaxBackends + NUM_AUXILIARY_PROCS;

	/*
	 * 以紧凑方式分配镜像 PGPROC 字段的数组。见
	 * PROC_HDR。
	 *
	 * XXX：考虑到这些数组被访问得非常频繁，
	 * 加大它们的填充也许是有意义的。
	 */
	ProcGlobal->xids =
		(TransactionId *) ShmemAlloc(TotalProcs * sizeof(*ProcGlobal->xids));
	MemSet(ProcGlobal->xids, 0, TotalProcs * sizeof(*ProcGlobal->xids));
	ProcGlobal->subxidStates = (XidCacheStatus *) ShmemAlloc(TotalProcs * sizeof(*ProcGlobal->subxidStates));
	MemSet(ProcGlobal->subxidStates, 0, TotalProcs * sizeof(*ProcGlobal->subxidStates));
	ProcGlobal->statusFlags = (uint8 *) ShmemAlloc(TotalProcs * sizeof(*ProcGlobal->statusFlags));
	MemSet(ProcGlobal->statusFlags, 0, TotalProcs * sizeof(*ProcGlobal->statusFlags));

	for (i = 0; i < TotalProcs; i++)
	{
		PGPROC	   *proc = &procs[i];

		/* 所有 PGPROCs 的公共初始化，不论类型。 */

		/*
		 * 为每个 PGPROC 设置信号量、锁存器以及 fpInfoLock。预备事务的
		 * 虚设 PGPROCs 并不需要这些——它们从不关联到
		 * 真正的进程
		 */
		if (i < MaxBackends + NUM_AUXILIARY_PROCS)
		{
			proc->sem = PGSemaphoreCreate();
			InitSharedLatch(&(proc->procLatch));
			LWLockInitialize(&(proc->fpInfoLock), LWTRANCHE_LOCK_FASTPATH);
		}
		proc->pgprocno = i;

		/*
		 * 为普通后端、自动清理和后台工作进程新建的 PGPROCs
		 * 必须排入相应的空闲链表。因为辅助进程的数量
		 * 始终又少又固定，那种情况下不用空闲
		 * 链表；InitAuxiliaryProcess() 改为做
		 * 线性搜索。预备事务的 PGPROCs 由
		 * TwoPhaseShmemInit() 加入空闲链表。
		 */
		if (i < MaxConnections)
		{
			/* 普通后端的 PGPROC，加入 freeProcs 链表 */
			dlist_push_head(&ProcGlobal->freeProcs, &proc->links);
			proc->procgloballist = &ProcGlobal->freeProcs;
		}
		else if (i < MaxConnections + autovacuum_max_workers + 1)
		{
			/* 自动清理启动器/工作进程的 PGPROC，加入 autovacFreeProcs 链表 */
			dlist_push_head(&ProcGlobal->autovacFreeProcs, &proc->links);
			proc->procgloballist = &ProcGlobal->autovacFreeProcs;
		}
		else if (i < MaxConnections + autovacuum_max_workers + 1 + max_worker_processes)
		{
			/* 后台工作进程的 PGPROC，加入 bgworkerFreeProcs 链表 */
			dlist_push_head(&ProcGlobal->bgworkerFreeProcs, &proc->links);
			proc->procgloballist = &ProcGlobal->bgworkerFreeProcs;
		}
		else if (i < MaxBackends)
		{
			/* WAL 发送进程的 PGPROC，加入 walsenderFreeProcs 链表 */
			dlist_push_head(&ProcGlobal->walsenderFreeProcs, &proc->links);
			proc->procgloballist = &ProcGlobal->walsenderFreeProcs;
		}

		/* 初始化 myProcLocks[] 共享内存队列。 */
		for (j = 0; j < NUM_LOCK_PARTITIONS; j++)
			dlist_init(&(proc->myProcLocks[j]));

		/* 初始化 lockGroupMembers 链表。 */
		dlist_init(&proc->lockGroupMembers);

		/*
		 * 初始化这些原子变量，否则对于当前未在使用的
		 * 后端，访问它们将是不安全的。
		 */
		pg_atomic_init_u32(&(proc->procArrayGroupNext), INVALID_PGPROCNO);
		pg_atomic_init_u32(&(proc->clogGroupNext), INVALID_PGPROCNO);
		pg_atomic_init_u64(&(proc->waitStart), 0);
	}

	/*
	 * 保存保留给辅助进程和预备事务的
	 * 那几块 PGPROC 结构的指针。
	 */
	AuxiliaryProcs = &procs[MaxBackends];
	PreparedXactProcs = &procs[MaxBackends + NUM_AUXILIARY_PROCS];

	/* 同时创建 ProcStructLock 自旋锁 */
	ProcStructLock = (slock_t *) ShmemAlloc(sizeof(slock_t));
	SpinLockInit(ProcStructLock);
}

/*
 * InitProcess -- 为本后端初始化每进程数据结构
 */
void
InitProcess(void)
{
	dlist_head *procgloballist;

	/*
	 * ProcGlobal 应当已经设置好（若我们是后端，则经由
	 * fork() 或 EXEC_BACKEND 机制从 postmaster 继承它）。
	 */
	if (ProcGlobal == NULL)
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/* 决定由哪条链表提供我们的 PGPROC。 */
	if (IsAnyAutoVacuumProcess())
		procgloballist = &ProcGlobal->autovacFreeProcs;
	else if (IsBackgroundWorker)
		procgloballist = &ProcGlobal->bgworkerFreeProcs;
	else if (am_walsender)
		procgloballist = &ProcGlobal->walsenderFreeProcs;
	else
		procgloballist = &ProcGlobal->freeProcs;

	/*
	 * 尝试从相应的空闲链表取一个 proc 结构。若失败，
	 * 我们一定是用尽了 PGPROC 结构（更不用说信号量）。
	 *
	 * 持有 ProcStructLock 期间，也把当前共享的
	 * spins_per_delay 估计值复制到本地存储。
	 */
	SpinLockAcquire(ProcStructLock);

	set_spins_per_delay(ProcGlobal->spins_per_delay);

	if (!dlist_is_empty(procgloballist))
	{
		MyProc = (PGPROC *) dlist_pop_head_node(procgloballist);
		SpinLockRelease(ProcStructLock);
	}
	else
	{
		/*
		 * 若走到这里，全部 PGPROCs 都在使用中。这是可以
		 * 检测出 "过多后端" 的地方之一，因此给出标准
		 * 错误消息。XXX 自动清理情形下是否需要
		 * 给出不同的失败消息？
		 */
		SpinLockRelease(ProcStructLock);
		if (am_walsender)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("number of requested standby connections exceeds max_wal_senders (currently %d)",
							max_wal_senders)));
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	/*
	 * 交叉核对 PGPROC 是否为我们所期望的类型；若不是，
	 * 它会被归还到错误的链表。
	 */
	Assert(MyProc->procgloballist == procgloballist);

	/*
	 * 既然已有 PGPROC，就把自己标记为活跃的 postmaster
	 * 子进程；这样如果我们退出时没有清理，postmaster 就能发现。
	 * （XXX 自动清理启动器目前不参与
	 * 这一点；它大概应该参与。）
	 */
	if (IsUnderPostmaster && !IsAutoVacuumLauncherProcess())
		MarkPostmasterChildActive();

	/*
	 * 初始化 MyProc 的全部字段，先前已由
	 * InitProcGlobal 初始化过的那些除外。
	 */
	dlist_node_init(&MyProc->links);
	MyProc->waitStatus = PROC_WAIT_STATUS_OK;
	MyProc->lxid = InvalidLocalTransactionId;
	MyProc->fpVXIDLock = false;
	MyProc->fpLocalTransactionId = InvalidLocalTransactionId;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->pid = MyProcPid;
	/* backendId、databaseId 和 roleId 稍后填入 */
	MyProc->backendId = InvalidBackendId;
	MyProc->databaseId = InvalidOid;
	MyProc->roleId = InvalidOid;
	MyProc->tempNamespaceId = InvalidOid;
	MyProc->isBackgroundWorker = !AmRegularBackendProcess();
	MyProc->delayChkptFlags = 0;
	MyProc->statusFlags = 0;
	/* NB -- 自动清理启动器有意不设置 IS_AUTOVACUUM */
	if (IsAutoVacuumWorkerProcess())
		MyProc->statusFlags |= PROC_IS_AUTOVACUUM;
	MyProc->lwWaiting = LW_WS_NOT_WAITING;
	MyProc->lwWaitMode = 0;
	MyProc->waitLock = NULL;
	MyProc->waitProcLock = NULL;
	pg_atomic_write_u64(&MyProc->waitStart, 0);
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		/* 上一个进程应当已经释放全部锁。 */
		for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
			Assert(dlist_is_empty(&(MyProc->myProcLocks[i])));
	}
#endif
	MyProc->recoveryConflictPending = false;

	/* 初始化同步复制用的字段 */
	MyProc->waitLSN = 0;
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	dlist_node_init(&MyProc->syncRepLinks);

	/* 初始化成组 XID 清除用的字段。 */
	MyProc->procArrayGroupMember = false;
	MyProc->procArrayGroupMemberXid = InvalidTransactionId;
	Assert(pg_atomic_read_u32(&MyProc->procArrayGroupNext) == INVALID_PGPROCNO);

	/* 检查锁组字段处于正确的初始状态。 */
	Assert(MyProc->lockGroupLeader == NULL);
	Assert(dlist_is_empty(&MyProc->lockGroupMembers));

	/* 初始化等待事件信息。 */
	MyProc->wait_event_info = 0;

	/* 初始化成组事务状态更新用的字段。 */
	MyProc->clogGroupMember = false;
	MyProc->clogGroupMemberXid = InvalidTransactionId;
	MyProc->clogGroupMemberXidStatus = TRANSACTION_STATUS_IN_PROGRESS;
	MyProc->clogGroupMemberPage = -1;
	MyProc->clogGroupMemberLsn = InvalidXLogRecPtr;
	Assert(pg_atomic_read_u32(&MyProc->clogGroupNext) == INVALID_PGPROCNO);

	/*
	 * 取得该 PGPROC 锁存器的所有权，以便我们能对它使用 WaitLatch。
	 * 这样就可以把进程锁存器（此前指向进程本地的那一个）
	 * 改指到共享的这一把。
	 */
	OwnLatch(&MyProc->procLatch);
	SwitchToSharedLatch();

	/* 既然已有 proc，就把等待事件报告到共享内存 */
	pgstat_set_wait_event_storage(&MyProc->wait_event_info);

	/*
	 * 我们也许在复用曾经属于失败进程的信号量。因此
	 * 这里要小心，重新初始化它的值。（严格说来这已经
	 * 不再必要，但为了干净，似乎仍是个好主意。）
	 */
	PGSemaphoreReset(MyProc->sem);

	/*
	 * 安排在后端退出时清理。
	 */
	on_shmem_exit(ProcKill, 0);

	/*
	 * 既然已有 PGPROC，我们就可以尝试获取锁，因此初始化
	 * LWLocks 所需的本地状态，以及死锁检测器。
	 */
	InitLWLockAccess();
	InitDeadLockChecking();
}

/*
 * InitProcessPhase2 -- 使 MyProc 在共享 ProcArray 中可见。
 *
 * 本阶段与 InitProcess 分开，是因为在创建 PGPROC 之前我们无法获取 LWLocks，
 * 但在 EXEC_BACKEND 情形下，要等做完 CreateSharedMemoryAndSemaphores
 * 之后 ProcArrayAdd 才会奏效。
 */
void
InitProcessPhase2(void)
{
	Assert(MyProc != NULL);

	/*
	 * 把我们的 PGPROC 加入共享内存中的 PGPROC 数组。
	 */
	ProcArrayAdd(MyProc);

	/*
	 * 安排在后端退出时把它清理掉。
	 */
	on_shmem_exit(RemoveProcFromArray, 0);
}

/*
 * InitAuxiliaryProcess -- 创建每个辅助进程的数据结构
 *
 * 本函数由后台写入器（bgwriter）及类似进程调用，以便它们拥有
 * 一个真实到足以等待 LWLocks 的 MyProc 值。所分配的 PGPROC
 * 与信号量是在
 * InitProcGlobal 期间创建的额外那些之一。
 *
 * 目前并不预期辅助进程去等待真正的（锁管理器）
 * 锁，因此不必设置死锁检测器。它们也从不被加入
 * ProcArray 或 sinval 消息机制。它们也
 * 不会被分配 VXID，因为这只在我们真正
 * 持有锁管理器锁时才有用。
 *
 * 不过启动进程会使用锁，但从不按
 * 普通后端那种方式等待它们。启动进程也参与 sinval 消息传递，
 * 作为 sendOnly 进程，因此从不从 sinval 队列读取消息。所以
 * 启动进程确实拥有 VXID，并且会出现在 pg_locks 中。
 */
void
InitAuxiliaryProcess(void)
{
	PGPROC	   *auxproc;
	int			proctype;

	/*
	 * ProcGlobal 应当已经设置好（若我们是后端，则经由
	 * fork() 或 EXEC_BACKEND 机制从 postmaster 继承它）。
	 */
	if (ProcGlobal == NULL || AuxiliaryProcs == NULL)
		elog(PANIC, "proc header uninitialized");

	if (MyProc != NULL)
		elog(ERROR, "you already exist");

	/*
	 * 我们用 ProcStructLock 保护 AuxiliaryProcs 条目的
	 * 分配与释放。
	 *
	 * 持有 ProcStructLock 期间，也把当前共享的
	 * spins_per_delay 估计值复制到本地存储。
	 */
	SpinLockAcquire(ProcStructLock);

	set_spins_per_delay(ProcGlobal->spins_per_delay);

	/*
	 * 找一个空闲的 auxproc……如果没有就非常麻烦……
	 */
	for (proctype = 0; proctype < NUM_AUXILIARY_PROCS; proctype++)
	{
		auxproc = &AuxiliaryProcs[proctype];
		if (auxproc->pid == 0)
			break;
	}
	if (proctype >= NUM_AUXILIARY_PROCS)
	{
		SpinLockRelease(ProcStructLock);
		elog(FATAL, "all AuxiliaryProcs are in use");
	}

	/* 把辅助进程标记为由我使用 */
	/* 使用 volatile 指针以防止代码重排 */
	((volatile PGPROC *) auxproc)->pid = MyProcPid;

	MyProc = auxproc;

	SpinLockRelease(ProcStructLock);

	/*
	 * 初始化 MyProc 的全部字段，先前已由
	 * InitProcGlobal 初始化过的那些除外。
	 */
	dlist_node_init(&MyProc->links);
	MyProc->waitStatus = PROC_WAIT_STATUS_OK;
	MyProc->lxid = InvalidLocalTransactionId;
	MyProc->fpVXIDLock = false;
	MyProc->fpLocalTransactionId = InvalidLocalTransactionId;
	MyProc->xid = InvalidTransactionId;
	MyProc->xmin = InvalidTransactionId;
	MyProc->backendId = InvalidBackendId;
	MyProc->databaseId = InvalidOid;
	MyProc->roleId = InvalidOid;
	MyProc->tempNamespaceId = InvalidOid;
	MyProc->isBackgroundWorker = true;
	MyProc->delayChkptFlags = 0;
	MyProc->statusFlags = 0;
	MyProc->lwWaiting = LW_WS_NOT_WAITING;
	MyProc->lwWaitMode = 0;
	MyProc->waitLock = NULL;
	MyProc->waitProcLock = NULL;
	pg_atomic_write_u64(&MyProc->waitStart, 0);
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		/* 上一个进程应当已经释放全部锁。 */
		for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
			Assert(dlist_is_empty(&(MyProc->myProcLocks[i])));
	}
#endif

	/*
	 * 取得该 PGPROC 锁存器的所有权，以便我们能对它使用 WaitLatch。
	 * 这样就可以把进程锁存器（此前指向进程本地的那一个）
	 * 改指到共享的这一把。
	 */
	OwnLatch(&MyProc->procLatch);
	SwitchToSharedLatch();

	/* 既然已有 proc，就把等待事件报告到共享内存 */
	pgstat_set_wait_event_storage(&MyProc->wait_event_info);

	/* 检查锁组字段处于正确的初始状态。 */
	Assert(MyProc->lockGroupLeader == NULL);
	Assert(dlist_is_empty(&MyProc->lockGroupMembers));

	/*
	 * 我们也许在复用曾经属于失败进程的信号量。因此
	 * 这里要小心，重新初始化它的值。（严格说来这已经
	 * 不再必要，但为了干净，似乎仍是个好主意。）
	 */
	PGSemaphoreReset(MyProc->sem);

	/*
	 * 安排在进程退出时清理。
	 */
	on_shmem_exit(AuxiliaryProcKill, Int32GetDatum(proctype));
}

/*
 * 由 bufmgr 用来共享启动进程正在等待的那个缓冲区的值，
 * 或把该值复位为 "未在等待"（-1）。这样就能处理
 * 缓冲区钉住上的恢复冲突。赋值发生在各后端查看
 * 此值之前，因此不必加锁，尤其是因为这次赋值
 * 是一次原子整数赋值。
 */
void
SetStartupBufferPinWaitBufId(int bufid)
{
	/* 使用 volatile 指针以防止代码重排 */
	volatile PROC_HDR *procglobal = ProcGlobal;

	procglobal->startupBufferPinWaitBufId = bufid;
}

/*
 * 后端收到检查缓冲区钉住等待的请求时使用。
 */
int
GetStartupBufferPinWaitBufId(void)
{
	/* 使用 volatile 指针以防止代码重排 */
	volatile PROC_HDR *procglobal = ProcGlobal;

	return procglobal->startupBufferPinWaitBufId;
}

/*
 * 检查是否至少有 N 个空闲 PGPROC 对象。若返回 false，
 * *nfree 会被设为空闲 PGPROC 对象的个数。
 * 否则 *nfree 会被设为 n。
 *
 * 注意：本函数的设计假定 N 通常较小。
 */
bool
HaveNFreeProcs(int n, int *nfree)
{
	dlist_iter	iter;

	Assert(n > 0);
	Assert(nfree);

	SpinLockAcquire(ProcStructLock);

	*nfree = 0;
	dlist_foreach(iter, &ProcGlobal->freeProcs)
	{
		(*nfree)++;
		if (*nfree == n)
			break;
	}

	SpinLockRelease(ProcStructLock);

	return (*nfree == n);
}

/*
 * 检查当前进程是否正在等待锁。
 */
bool
IsWaitingForLock(void)
{
	if (lockAwaited == NULL)
		return false;

	return true;
}

/*
 * 中止事务时，取消任何尚未完成的锁等待，并回退
 * 正在获取的锁上已经加上的强锁计数。
 *
 * （通常只有在等待期间接受了取消/die
 * 打断才会这样；不过在锁等待之前或期间
 * ereport(ERROR) 也是可能发生的。）
 */
void
LockErrorCleanup(void)
{
	LWLock	   *partitionLock;
	DisableTimeoutParams timeouts[2];

	HOLD_INTERRUPTS();

	AbortStrongLockAcquire();

	/* 若我们当时并非在等锁，则无事可做 */
	if (lockAwaited == NULL)
	{
		RESUME_INTERRUPTS();
		return;
	}

	/*
	 * 关掉仍在运行的死锁与锁超时计时器
	 * （见 ProcSleep）。注意必须保留 LOCK_TIMEOUT
	 * 指示标志，因为响应 SIGINT 时本函数在
	 * ProcessInterrupts 之前执行；否则我们会失去
	 * 这样的信息：该 SIGINT 来自锁超时，而不是外部
	 * 来源。
	 */
	timeouts[0].id = DEADLOCK_TIMEOUT;
	timeouts[0].keep_indicator = false;
	timeouts[1].id = LOCK_TIMEOUT;
	timeouts[1].keep_indicator = true;
	disable_timeouts(timeouts, 2);

	/* 若自己还在等待队列上，就把自己摘下来（也许已经不在了！） */
	partitionLock = LockHashPartitionLock(lockAwaited->hashcode);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	if (!dlist_node_is_detached(&MyProc->links))
	{
		/* 此时还无法认为锁已经授予我们 */
		RemoveFromWaitQueue(MyProc, lockAwaited->hashcode);
	}
	else
	{
		/*
		 * 已经有人把我们从锁等待队列上踢下来了。也许他们
		 * 把锁授予了我们，也许他们检测到了死锁。若他们
		 * 确实授予了我们锁，我们最好把它记入本地锁
		 * 表。
		 */
		if (MyProc->waitStatus == PROC_WAIT_STATUS_OK)
			GrantAwaitedLock();
	}

	lockAwaited = NULL;

	LWLockRelease(partitionLock);

	RESUME_INTERRUPTS();
}


/*
 * ProcReleaseLocks() -- 在主事务提交或中止时
 *			释放与当前事务相关联的锁
 *
 * 主事务提交时，我们释放标准锁，但保留会话级锁。
 * 主事务中止时，我们释放包括会话级锁在内的全部锁。
 *
 * 咨询锁仅在它们是事务级时才释放；
 * 会话级持有仍然保留，无论这次是不是提交。
 *
 * 子事务提交时，我们不释放任何锁（因此本函数
 * 完全不需要）；释放会推迟到父事务。
 * 子事务中止时，我们释放该子事务持有的全部锁；
 * 这是在 ResourceOwner 机制的控制下
 * 逐把释放这些锁来实现的。
 */
void
ProcReleaseLocks(bool isCommit)
{
	if (!MyProc)
		return;
	/* 若正在等待，就离开等待队列（应当只在出错后才需要） */
	LockErrorCleanup();
	/* 释放标准锁；若正在中止，则包括会话级锁 */
	LockReleaseAll(DEFAULT_LOCKMETHOD, !isCommit);
	/* 释放事务级咨询锁 */
	LockReleaseAll(USER_LOCKMETHOD, false);
}


/*
 * RemoveProcFromArray() -- 把本进程从共享 ProcArray 中移除。
 */
static void
RemoveProcFromArray(int code, Datum arg)
{
	Assert(MyProc != NULL);
	ProcArrayRemove(MyProc, InvalidTransactionId);
}

/*
 * ProcKill() -- 销毁本进程的
 *		每进程数据结构。释放它持有的任何 LW 锁。
 */
static void
ProcKill(int code, Datum arg)
{
	PGPROC	   *proc;
	dlist_head *procgloballist;

	Assert(MyProc != NULL);

	/* 若由 system() 等调用 fork 出来则不安全 */
	if (MyProc->pid != (int) getpid())
		elog(PANIC, "ProcKill() called in child process");

	/* 确保我们已经不在同步复制链表中 */
	SyncRepCleanupAtProcExit();

#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		/* 上一个进程应当已经释放全部锁。 */
		for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
			Assert(dlist_is_empty(&(MyProc->myProcLocks[i])));
	}
#endif

	/*
	 * 释放我持有的任何 LW 锁。按理不该还有，但
	 * 在释放 PGPROC 从而废掉 LWLock
	 * 机制之前，再检查一次的代价很低……
	 */
	LWLockReleaseAll();

	/* 同时取消任何尚未结束的条件变量睡眠 */
	ConditionVariableCancelSleep();

	/*
	 * 从我们所属的任何锁组中脱离。若组长
	 * 在其他组成员之前退出，其 PGPROC 会一直保持已分配，
	 * 直到最后一个组进程退出；那个进程必须把
	 * 组长的 PGPROC 归还到相应链表。
	 */
	if (MyProc->lockGroupLeader != NULL)
	{
		PGPROC	   *leader = MyProc->lockGroupLeader;
		LWLock	   *leader_lwlock = LockHashPartitionLockByProc(leader);

		LWLockAcquire(leader_lwlock, LW_EXCLUSIVE);
		Assert(!dlist_is_empty(&leader->lockGroupMembers));
		dlist_delete(&MyProc->lockGroupLink);
		if (dlist_is_empty(&leader->lockGroupMembers))
		{
			leader->lockGroupLeader = NULL;
			if (leader != MyProc)
			{
				procgloballist = leader->procgloballist;

				/* 组长先退出；归还其 PGPROC。 */
				SpinLockAcquire(ProcStructLock);
				dlist_push_head(procgloballist, &leader->links);
				SpinLockRelease(ProcStructLock);
			}
		}
		else if (leader != MyProc)
			MyProc->lockGroupLeader = NULL;
		LWLockRelease(leader_lwlock);
	}

	/*
	 * 把 MyLatch 复位为进程本地的那一把。这样
	 * 信号处理函数等在共享锁存器
	 * 不再属于我们之后，仍能继续使用这把锁存器。
	 *
	 * 同样，停止向 MyProc->wait_event_info 报告等待事件。
	 *
	 * 此后清除 MyProc，并放弃共享锁存器的所有权。
	 */
	SwitchBackToLocalLatch();
	pgstat_reset_wait_event_storage();

	proc = MyProc;
	MyProc = NULL;
	DisownLatch(&proc->procLatch);

	procgloballist = proc->procgloballist;
	SpinLockAcquire(ProcStructLock);

	/*
	 * 若我们仍是某个锁组的成员，那就说明我们是组长，
	 * 并且不知怎么在子进程之前退出了。最后剩下的子进程
	 * 会释放我们的 PGPROC。否则现在就释放它。
	 */
	if (proc->lockGroupLeader == NULL)
	{
		/* 既然 lockGroupLeader 为 NULL，lockGroupMembers 应为空。 */
		Assert(dlist_is_empty(&proc->lockGroupMembers));

		/* 把 PGPROC 结构（以及信号量）归还到相应空闲链表 */
		dlist_push_tail(procgloballist, &proc->links);
	}

	/* 更新 spins_per_delay 的共享估计值 */
	ProcGlobal->spins_per_delay = update_spins_per_delay(ProcGlobal->spins_per_delay);

	SpinLockRelease(ProcStructLock);

	/*
	 * 本进程在共享内存中已经不再以任何有意义的方式
	 * 存在，因此告诉 postmaster 我们已经清理得可以接受。（XXX
	 * 自动清理启动器将来应当也被包括进来）
	 */
	if (IsUnderPostmaster && !IsAutoVacuumLauncherProcess())
		MarkPostmasterChildInactive();

	/* 必要时唤醒自动清理启动器 -- 见 FreeWorkerInfo 中的注释 */
	if (AutovacuumLauncherPid != 0)
		kill(AutovacuumLauncherPid, SIGUSR2);
}

/*
 * AuxiliaryProcKill() -- 供辅助进程使用的精简版 ProcKill
 *		（后台写入器等）。PGPROC 与信号量并不释放，只是
 *		标记为未在使用。
 */
static void
AuxiliaryProcKill(int code, Datum arg)
{
	int			proctype = DatumGetInt32(arg);
	PGPROC	   *auxproc PG_USED_FOR_ASSERTS_ONLY;
	PGPROC	   *proc;

	Assert(proctype >= 0 && proctype < NUM_AUXILIARY_PROCS);

	/* 若由 system() 等调用 fork 出来则不安全 */
	if (MyProc->pid != (int) getpid())
		elog(PANIC, "AuxiliaryProcKill() called in child process");

	auxproc = &AuxiliaryProcs[proctype];

	Assert(MyProc == auxproc);

	/* 释放我持有的任何 LW 锁（见上方说明） */
	LWLockReleaseAll();

	/* 同时取消任何尚未结束的条件变量睡眠 */
	ConditionVariableCancelSleep();

	/* 注释见对应的 ProcKill() 代码 */
	SwitchBackToLocalLatch();
	pgstat_reset_wait_event_storage();

	proc = MyProc;
	MyProc = NULL;
	DisownLatch(&proc->procLatch);

	SpinLockAcquire(ProcStructLock);

	/* 把辅助进程标记为不再使用 */
	proc->pid = 0;

	/* 更新 spins_per_delay 的共享估计值 */
	ProcGlobal->spins_per_delay = update_spins_per_delay(ProcGlobal->spins_per_delay);

	SpinLockRelease(ProcStructLock);
}

/*
 * AuxiliaryPidGetProc -- 取得辅助进程的 PGPROC，
 * 给定其 PID
 *
 * 找不到时返回 NULL。
 */
PGPROC *
AuxiliaryPidGetProc(int pid)
{
	PGPROC	   *result = NULL;
	int			index;

	if (pid == 0)				/* 绝不匹配虚设的 PGPROCs */
		return NULL;

	for (index = 0; index < NUM_AUXILIARY_PROCS; index++)
	{
		PGPROC	   *proc = &AuxiliaryProcs[index];

		if (proc->pid == pid)
		{
			result = proc;
			break;
		}
	}
	return result;
}


/*
 * ProcSleep -- 让进程在指定的锁上睡眠
 *
 * 调用方必须已设置 MyProc->heldLocks，以反映本进程
 * 在该可锁对象上已经持有的锁（在全部 XIDs 之下）。
 *
 * 进入时必须持有锁表的分区锁，退出时
 * 仍会持有。
 *
 * 结果：若取得锁则为 PROC_WAIT_STATUS_OK，否则（死锁）为 PROC_WAIT_STATUS_ERROR。
 *
 * ASSUME：在我们
 *		释放分区锁之前，不会有人去改动该队列。
 *
 * NOTES：进程队列现在是用于加锁的优先级队列。
 */
ProcWaitStatus
ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable)
{
	LOCKMODE	lockmode = locallock->tag.mode;
	LOCK	   *lock = locallock->lock;
	PROCLOCK   *proclock = locallock->proclock;
	uint32		hashcode = locallock->hashcode;
	LWLock	   *partitionLock = LockHashPartitionLock(hashcode);
	dclist_head *waitQueue = &lock->waitProcs;
	PGPROC	   *insert_before = NULL;
	LOCKMASK	myHeldLocks = MyProc->heldLocks;
	TimestampTz standbyWaitStart = 0;
	bool		early_deadlock = false;
	bool		allow_autovacuum_cancel = true;
	bool		logged_recovery_conflict = false;
	ProcWaitStatus myWaitStatus;
	PGPROC	   *leader = MyProc->lockGroupLeader;

	/*
	 * 若正在使用锁组，我所属锁组成员持有的锁
	 * 需要计入 myHeldLocks。对于在组成员之间相互冲突的关系
	 * 扩展锁，这并不是必需的。不过，把它们计入
	 * myHeldLocks 会让组成员比其他也在尝试获取
	 * 这些锁的后端更优先得到这些
	 * 锁。另一方面，我们也可以不给组成员这类锁的优先权，
	 * 但那样做似乎并没有明显的
	 * 好处。
	 */
	if (leader != NULL)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &lock->procLocks)
		{
			PROCLOCK   *otherproclock;

			otherproclock = dlist_container(PROCLOCK, lockLink, iter.cur);

			if (otherproclock->groupLeader == leader)
				myHeldLocks |= otherproclock->holdMask;
		}
	}

	/*
	 * 确定把自己加到等待队列的什么位置。
	 *
	 * 通常我应该排到队列末尾。不过，若我已经
	 * 持有与任何先前等待者的请求相冲突的锁，就把
	 * 自己插到第一个这样的等待者正前方。这一步并非
	 * 必需，因为死锁检测反正会把我移到那个
	 * 等待者之前；但立刻发现这种冲突相对便宜，
	 * 可以避免一直拖到死锁超时。
	 *
	 * 特殊情况：若发现自己应该排到某个等待者前面，就检查
	 * 自己是否与已持有的锁、或该等待者之前的请求相冲突。
	 * 若不相冲突，就立刻把所请求的锁授予自己。
	 * 这与 LockAcquire 里立即授予的测试相同，只不过
	 * 我们只考虑自己插入点之前
	 * 的那一段等待队列。
	 */
	if (myHeldLocks != 0 && !dclist_is_empty(waitQueue))
	{
		LOCKMASK	aheadRequests = 0;
		dlist_iter	iter;

		dclist_foreach(iter, waitQueue)
		{
			PGPROC	   *proc = dlist_container(PGPROC, links, iter.cur);

			/*
			 * 若我们与这个等待者属于同一个锁组，它的
			 * 锁既不与我们的锁冲突，也不计入
			 * aheadRequests。
			 */
			if (leader != NULL && leader == proc->lockGroupLeader)
				continue;

			/* 他必须等我吗？ */
			if (lockMethodTable->conflictTab[proc->waitLockMode] & myHeldLocks)
			{
				/* 我必须等他吗？ */
				if (lockMethodTable->conflictTab[lockmode] & proc->heldLocks)
				{
					/*
					 * 是的，于是我们死锁了。要把现场正确清理干净，
					 * 最简单的办法是调用 RemoveFromWaitQueue()，但我们
					 * 要等到自己已经在等待队列上才能这样做。因此设置
					 * 一个标志留待下面检查，并跳出循环。同时
					 * 记下死锁信息，供以后的消息使用。
					 */
					RememberSimpleDeadLock(MyProc, lockmode, lock, proc);
					early_deadlock = true;
					break;
				}
				/* 我必须排在这个等待者之前。检查特殊情况。 */
				if ((lockMethodTable->conflictTab[lockmode] & aheadRequests) == 0 &&
					!LockCheckConflicts(lockMethodTable, lockmode, lock,
										proclock))
				{
					/* 跳过等待，直接把锁授予自己。 */
					GrantLock(lock, proclock, lockmode);
					GrantAwaitedLock();
					return PROC_WAIT_STATUS_OK;
				}

				/* 把自己插到冲突进程之前的等待队列中 */
				insert_before = proc;
				break;
			}
			/* 不是，于是前进到下一个等待者 */
			aheadRequests |= LOCKBIT_ON(proc->waitLockMode);
		}
	}

	/*
	 * 按上面确定的位置把自己插入队列。
	 */
	if (insert_before)
		dclist_insert_before(waitQueue, &insert_before->links, &MyProc->links);
	else
		dclist_push_tail(waitQueue, &MyProc->links);

	lock->waitMask |= LOCKBIT_ON(lockmode);

	/* 同时在 PGPROC 对象中设置等待信息 */
	MyProc->waitLock = lock;
	MyProc->waitProcLock = proclock;
	MyProc->waitLockMode = lockmode;

	MyProc->waitStatus = PROC_WAIT_STATUS_WAITING;

	/*
	 * 若检测到死锁，则不再等待而直接放弃。这必须与
	 * CheckDeadLock 的恢复代码一致。
	 */
	if (early_deadlock)
	{
		RemoveFromWaitQueue(MyProc, hashcode);
		return PROC_WAIT_STATUS_ERROR;
	}

	/* 标记我们正在等待一把锁 */
	lockAwaited = locallock;

	/*
	 * 释放锁表的分区锁。
	 *
	 * NOTE：这可能也使我们退出临界区状态，从而可能
	 * 允许接受取消/die 中断。这没问题，因为我们已经
	 * 记下自己正在等锁，因此若发生取消/die，
	 * LockErrorCleanup 会负责清理。
	 */
	LWLockRelease(partitionLock);

	/*
	 * 另外，既然 ereport 之后我们能够顺利清理，就可以
	 * 安全地检查是否存在针对启动进程的缓冲区钉住死锁。
	 * 当然，只有当我们正处于热备、并且自己并不是
	 * 启动进程时，才有必要这样做。
	 */
	if (RecoveryInProgress() && !InRecovery)
		CheckRecoveryConflictDeadlock();

	/* 在启用超时处理函数之前复位 deadlock_state */
	deadlock_state = DS_NOT_YET_CHECKED;
	got_deadlock_timeout = false;

	/*
	 * 设置计时器，以便过一会儿醒来检查死锁。若
	 * 检测到死锁，处理函数会把 MyProc->waitStatus 设为
	 * PROC_WAIT_STATUS_ERROR，让我们知道必须报告失败
	 * 而不是成功。
	 *
	 * 把检查推迟到已经等待片刻之后，大多数情况下就可以避免
	 * 运行相当昂贵的死锁检测代码。
	 *
	 * 若设置了 LockTimeout，也一并启用那个超时。把两个超时源
	 * 放在一次调用里启用，可以省下几个周期。
	 *
	 * 若处于 InHotStandby，为了与其他代码对照清楚，我们会把锁等待
	 * 稍晚一点再设置。
	 */
	if (!InHotStandby)
	{
		if (LockTimeout > 0)
		{
			EnableTimeoutParams timeouts[2];

			timeouts[0].id = DEADLOCK_TIMEOUT;
			timeouts[0].type = TMPARAM_AFTER;
			timeouts[0].delay_ms = DeadlockTimeout;
			timeouts[1].id = LOCK_TIMEOUT;
			timeouts[1].type = TMPARAM_AFTER;
			timeouts[1].delay_ms = LockTimeout;
			enable_timeouts(timeouts, 2);
		}
		else
			enable_timeout_after(DEADLOCK_TIMEOUT, DeadlockTimeout);

		/*
		 * 把为死锁超时计时器取得的当前时间用作
		 * waitStart（也就是本进程开始等待该
		 * 锁的时刻）。重新取当前时间会带来开销，因此我们
		 * 复用已经取得的时间，以避免该开销。
		 *
		 * 注意，更新 waitStart 时并不持有锁表的
		 * 分区锁，以免额外获取锁带来
		 * 开销。这可能导致 pg_locks 里的 "waitstart" 在等待开始后
		 * 极短的一段时间内变成 NULL，即便当时
		 * "granted" 为 false。实践上这没问题，因为可以假定
		 * 用户多半是在长时间等待
		 * 该锁时才去看 "waitstart"。
		 */
		pg_atomic_write_u64(&MyProc->waitStart,
							get_timeout_start_time(DEADLOCK_TIMEOUT));
	}
	else if (log_recovery_conflict_waits)
	{
		/*
		 * 若启用了日志且处于热备，
		 * 则设置等待开始时间戳。
		 */
		standbyWaitStart = GetCurrentTimestamp();
	}

	/*
	 * 若有人在 LWLockRelease 与 WaitLatch 之间唤醒我们，锁存器
	 * 不会等待。但锁存器已置位并不一定意味着锁
	 * 现在空闲，因为锁存器被置位的来源很多，并不只有
	 * 有人释放了锁。
	 *
	 * 每当锁存器被置位，我们就处理中断，因此取消/die
	 * 中断会被很快处理。这意味着我们不得介意在这里失去
	 * 控制权，把它交给取消/die 中断。我们并不介意，因为我们没有
	 * 在被授予锁之后要做的共享状态变更工作（授予方
	 * 已经全部做完）。我们倒是要操心取消死锁
	 * 超时并更新 locallock 表；但若因错误而失去控制，
	 * LockErrorCleanup 会把它修好。
	 */
	do
	{
		if (InHotStandby)
		{
			bool		maybe_log_conflict =
				(standbyWaitStart != 0 && !logged_recovery_conflict);

			/* 设置计时器，并等待它到期或锁被授予 */
			ResolveRecoveryConflictWithLock(locallock->tag.lock,
											maybe_log_conflict);

			/*
			 * 若启动进程为锁上的恢复冲突等待的时间
			 * 超过 deadlock_timeout，则发出日志消息。
			 */
			if (maybe_log_conflict)
			{
				TimestampTz now = GetCurrentTimestamp();

				if (TimestampDifferenceExceeds(standbyWaitStart, now,
											   DeadlockTimeout))
				{
					VirtualTransactionId *vxids;
					int			cnt;

					vxids = GetLockConflicts(&locallock->tag.lock,
											 AccessExclusiveLock, &cnt);

					/*
					 * 记下这次恢复冲突，以及持有冲突锁的
					 * 各后端的 PIDs 列表。注意，即便此刻并没有
					 * 这样的后端，我们仍然记日志，
					 * 因为这里的启动进程已经等待得
					 * 比 deadlock_timeout 更久。
					 */
					LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_LOCK,
										standbyWaitStart, now,
										cnt > 0 ? vxids : NULL, true);
					logged_recovery_conflict = true;
				}
			}
		}
		else
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 PG_WAIT_LOCK | locallock->tag.lock.locktag_type);
			ResetLatch(MyLatch);
			/* 先检查死锁，因为那大概值得记入日志 */
			if (got_deadlock_timeout)
			{
				CheckDeadLock();
				got_deadlock_timeout = false;
			}
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * waitStatus 可能从 PROC_WAIT_STATUS_WAITING 异步地变成
		 * 别的值。每个循环只读一次，以免出现
		 * 令人意外的行为（例如漏掉日志消息）。
		 */
		myWaitStatus = *((volatile ProcWaitStatus *) &MyProc->waitStatus);

		/*
		 * 若我们并未死锁，但在等待由自动清理引起的
		 * 任务，则发送信号去中断它。
		 */
		if (deadlock_state == DS_BLOCKED_BY_AUTOVACUUM && allow_autovacuum_cancel)
		{
			PGPROC	   *autovac = GetBlockingAutoVacuumPgproc();
			uint8		statusFlags;
			uint8		lockmethod_copy;
			LOCKTAG		locktag_copy;

			/*
			 * 先取走需要的信息，然后立刻释放锁。注意这种
			 * 写法意味着有极小的可能：该进程
			 * 终止其当前事务并开始另一个事务，
			 * 发生在我们来得及发出信号之前；最坏的
			 * 后果是一次防回卷清理被取消。但
			 * 除非我们持锁调用 kill()，否则这种情况
			 * 无论如何都可能发生，而持锁调用要不可取得多。
			 */
			LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
			statusFlags = ProcGlobal->statusFlags[autovac->pgxactoff];
			lockmethod_copy = lock->tag.locktag_lockmethodid;
			locktag_copy = lock->tag;
			LWLockRelease(ProcArrayLock);

			/*
			 * 仅当该工作进程并非在防止事务 ID
			 * 回卷时才这样做。
			 */
			if ((statusFlags & PROC_IS_AUTOVACUUM) &&
				!(statusFlags & PROC_VACUUM_FOR_WRAPAROUND))
			{
				int			pid = autovac->pid;

				/* 若配置为报告，则报告这种情况 */
				if (message_level_is_interesting(DEBUG1))
				{
					StringInfoData locktagbuf;
					StringInfoData logbuf;	/* 供服务器日志使用的 errdetail */

					initStringInfo(&locktagbuf);
					initStringInfo(&logbuf);
					DescribeLockTag(&locktagbuf, &locktag_copy);
					appendStringInfo(&logbuf,
									 "Process %d waits for %s on %s.",
									 MyProcPid,
									 GetLockmodeName(lockmethod_copy, lockmode),
									 locktagbuf.data);

					ereport(DEBUG1,
							(errmsg_internal("sending cancel to blocking autovacuum PID %d",
											 pid),
							 errdetail_log("%s", logbuf.data)));

					pfree(locktagbuf.data);
					pfree(logbuf.data);
				}

				/* 把自动清理工作进程送回起点 */
				if (kill(pid, SIGINT) < 0)
				{
					/*
					 * 这里有一处竞态：一旦我们释放
					 * ProcArrayLock，自动清理工作进程就可能
					 * 收工并在我们来得及 kill() 之前退出。
					 * 因此，我们不为 "进程不存在" 而抱怨。
					 * 若内核很快回收该 PID，EPERM 之类的其他错误
					 * 按理也可能发生，但这类情况
					 * 似乎不大可能，所以若看到别的 errno，
					 * 发出警告大概是最好的。
					 */
					if (errno != ESRCH)
						ereport(WARNING,
								(errmsg("could not send signal to process %d: %m",
										pid)));
				}
			}

			/* 防止该信号被再次发送 */
			allow_autovacuum_cancel = false;
		}

		/*
		 * 若在死锁检查中断已经运行之后被唤醒，且
		 * log_lock_waits 为开，则报告这次等待。
		 */
		if (log_lock_waits && deadlock_state != DS_NOT_YET_CHECKED)
		{
			StringInfoData buf,
						lock_waiters_sbuf,
						lock_holders_sbuf;
			const char *modename;
			long		secs;
			int			usecs;
			long		msecs;
			dlist_iter	proc_iter;
			PROCLOCK   *curproclock;
			bool		first_holder = true,
						first_waiter = true;
			int			lockHoldersNum = 0;

			initStringInfo(&buf);
			initStringInfo(&lock_waiters_sbuf);
			initStringInfo(&lock_holders_sbuf);

			DescribeLockTag(&buf, &locallock->tag.lock);
			modename = GetLockmodeName(locallock->tag.lock.locktag_lockmethodid,
									   lockmode);
			TimestampDifference(get_timeout_start_time(DEADLOCK_TIMEOUT),
								GetCurrentTimestamp(),
								&secs, &usecs);
			msecs = secs * 1000 + usecs / 1000;
			usecs = usecs % 1000;

			/*
			 * 我们遍历该锁的 procLocks，收集全部
			 * 持有者与等待者的列表。这样就能为锁的调试
			 * 提供更详细的信息。
			 *
			 * lock->procLocks 包含持有或等待
			 * 这把锁的全部进程。
			 */

			LWLockAcquire(partitionLock, LW_SHARED);

			dlist_foreach(proc_iter, &lock->procLocks)
			{
				curproclock =
					dlist_container(PROCLOCK, lockLink, proc_iter.cur);

				/*
				 * 若 myProc->waitProcLock == curproclock，我们就是等待者；
				 * 若它为 NULL 或别的值，我们就是持有者
				 */
				if (curproclock->tag.myProc->waitProcLock == curproclock)
				{
					if (first_waiter)
					{
						appendStringInfo(&lock_waiters_sbuf, "%d",
										 curproclock->tag.myProc->pid);
						first_waiter = false;
					}
					else
						appendStringInfo(&lock_waiters_sbuf, ", %d",
										 curproclock->tag.myProc->pid);
				}
				else
				{
					if (first_holder)
					{
						appendStringInfo(&lock_holders_sbuf, "%d",
										 curproclock->tag.myProc->pid);
						first_holder = false;
					}
					else
						appendStringInfo(&lock_holders_sbuf, ", %d",
										 curproclock->tag.myProc->pid);

					lockHoldersNum++;
				}
			}

			LWLockRelease(partitionLock);

			if (deadlock_state == DS_SOFT_DEADLOCK)
				ereport(LOG,
						(errmsg("process %d avoided deadlock for %s on %s by rearranging queue order after %ld.%03d ms",
								MyProcPid, modename, buf.data, msecs, usecs),
						 (errdetail_log_plural("Process holding the lock: %s. Wait queue: %s.",
											   "Processes holding the lock: %s. Wait queue: %s.",
											   lockHoldersNum, lock_holders_sbuf.data, lock_waiters_sbuf.data))));
			else if (deadlock_state == DS_HARD_DEADLOCK)
			{
				/*
				 * 这条消息与随后将报告的错误有些重复，
				 * 但有些情况下错误报告
				 * 可能到不了日志（例如被
				 * 异常处理函数捕获），而我们希望确保所有长时间等待
				 * 事件都被记入日志。
				 */
				ereport(LOG,
						(errmsg("process %d detected deadlock while waiting for %s on %s after %ld.%03d ms",
								MyProcPid, modename, buf.data, msecs, usecs),
						 (errdetail_log_plural("Process holding the lock: %s. Wait queue: %s.",
											   "Processes holding the lock: %s. Wait queue: %s.",
											   lockHoldersNum, lock_holders_sbuf.data, lock_waiters_sbuf.data))));
			}

			if (myWaitStatus == PROC_WAIT_STATUS_WAITING)
				ereport(LOG,
						(errmsg("process %d still waiting for %s on %s after %ld.%03d ms",
								MyProcPid, modename, buf.data, msecs, usecs),
						 (errdetail_log_plural("Process holding the lock: %s. Wait queue: %s.",
											   "Processes holding the lock: %s. Wait queue: %s.",
											   lockHoldersNum, lock_holders_sbuf.data, lock_waiters_sbuf.data))));
			else if (myWaitStatus == PROC_WAIT_STATUS_OK)
				ereport(LOG,
						(errmsg("process %d acquired %s on %s after %ld.%03d ms",
								MyProcPid, modename, buf.data, msecs, usecs)));
			else
			{
				Assert(myWaitStatus == PROC_WAIT_STATUS_ERROR);

				/*
				 * 目前，死锁检测器总是踢自己的
				 * 进程，这意味着我们只会在
				 * deadlock_state ==
				 * DS_HARD_DEADLOCK 时看到 PROC_WAIT_STATUS_ERROR，因此不必打印冗余
				 * 消息。但为了完整、也为将来预留，若看起来是别人把我们从
				 * 这把锁上踢下来的，就
				 * 打印一条消息。
				 */
				if (deadlock_state != DS_HARD_DEADLOCK)
					ereport(LOG,
							(errmsg("process %d failed to acquire %s on %s after %ld.%03d ms",
									MyProcPid, modename, buf.data, msecs, usecs),
							 (errdetail_log_plural("Process holding the lock: %s. Wait queue: %s.",
												   "Processes holding the lock: %s. Wait queue: %s.",
												   lockHoldersNum, lock_holders_sbuf.data, lock_waiters_sbuf.data))));
			}

			/*
			 * 此时我们也许仍需等待该锁。复位
			 * 状态，以免再次打印上面的消息。
			 */
			deadlock_state = DS_NO_DEADLOCK;

			pfree(buf.data);
			pfree(lock_holders_sbuf.data);
			pfree(lock_waiters_sbuf.data);
		}
	} while (myWaitStatus == PROC_WAIT_STATUS_WAITING);

	/*
	 * 若计时器仍在运行，就关掉它们。与 LockErrorCleanup 中一样，
	 * 我们必须保留 LOCK_TIMEOUT 指示标志：若锁超时已经
	 * 使 QueryCancelPending 被置位，我们希望这次取消
	 * 被报告为锁超时，而不是用户取消。
	 */
	if (!InHotStandby)
	{
		if (LockTimeout > 0)
		{
			DisableTimeoutParams timeouts[2];

			timeouts[0].id = DEADLOCK_TIMEOUT;
			timeouts[0].keep_indicator = false;
			timeouts[1].id = LOCK_TIMEOUT;
			timeouts[1].keep_indicator = true;
			disable_timeouts(timeouts, 2);
		}
		else
			disable_timeout(DEADLOCK_TIMEOUT, false);
	}

	/*
	 * 若锁上的恢复冲突已经解决，但启动进程
	 * 为它等待的时间超过了 deadlock_timeout，则发出日志消息。
	 */
	if (InHotStandby && logged_recovery_conflict)
		LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_LOCK,
							standbyWaitStart, GetCurrentTimestamp(),
							NULL, false);

	/*
	 * 重新获取锁表的分区锁。必须这样做，才能在动
	 * lockAwaited 之前挡住取消/die 中断（否则我们
	 * 可能会漏掉或重复更新 locallock）。
	 */
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 我们不再希望 LockErrorCleanup 做任何事。
	 */
	lockAwaited = NULL;

	/*
	 * 若我们得到了锁，务必把它记入 locallock 表。
	 */
	if (MyProc->waitStatus == PROC_WAIT_STATUS_OK)
		GrantAwaitedLock();

	/*
	 * 我们不必再做别的事，因为唤醒方已经完成了
	 * 锁表和 MyProc 的全部必要更新。
	 */
	return MyProc->waitStatus;
}


/*
 * ProcWakeup -- 通过置位其锁存器来唤醒进程。
 *
 *	 同时把该进程从等待队列移除，并将其链接置为无效。
 *
 * 调用方必须持有相应的锁分区锁。
 *
 * XXX：目前这段代码只用于 "成功" 情形，并且只在
 * 该情形下才正确。失败情形下的清理还需要
 * 调整该锁的请求计数 --- 见 RemoveFromWaitQueue。
 * 因此实践上 waitStatus 参数必须是 PROC_WAIT_STATUS_OK。
 */
void
ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus)
{
	if (dlist_node_is_detached(&proc->links))
		return;

	Assert(proc->waitStatus == PROC_WAIT_STATUS_WAITING);

	/* 把进程从等待队列移除 */
	dclist_delete_from_thoroughly(&proc->waitLock->waitProcs, &proc->links);

	/* 清理进程状态，并把成功/失败信号传给它 */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	proc->waitStatus = waitStatus;
	pg_atomic_write_u64(&MyProc->waitStart, 0);

	/* 然后唤醒它 */
	SetLatch(&proc->procLatch);
}

/*
 * ProcLockWakeup -- 在锁被
 *		释放（或先前某个等待者被中止）时唤醒进程的例程。扫描该锁的全部等待者，
 *		唤醒任何不再被阻塞的等待者。
 *
 * 调用方必须持有相应的锁分区锁。
 */
void
ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock)
{
	dclist_head *waitQueue = &lock->waitProcs;
	LOCKMASK	aheadRequests = 0;
	dlist_mutable_iter miter;

	if (dclist_is_empty(waitQueue))
		return;

	dclist_foreach_modify(miter, waitQueue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, links, miter.cur);
		LOCKMODE	lockmode = proc->waitLockMode;

		/*
		 * 若 (a) 与更早等待者的请求不冲突，且
		 * (b) 与已持有的锁不冲突，则唤醒。
		 */
		if ((lockMethodTable->conflictTab[lockmode] & aheadRequests) == 0 &&
			!LockCheckConflicts(lockMethodTable, lockmode, lock,
								proc->waitProcLock))
		{
			/* 可以唤醒 */
			GrantLock(lock, proc->waitProcLock, lockmode);
			/* 把 proc 从该锁的等待进程队列中移除 */
			ProcWakeup(proc, PROC_WAIT_STATUS_OK);
		}
		else
		{
			/*
			 * 锁冲突：不要唤醒，但记住请求的模式，供
			 * 后续检查使用。
			 */
			aheadRequests |= LOCKBIT_ON(lockmode);
		}
	}
}

/*
 * CheckDeadLock
 *
 * 只有在等待其他进程释放锁期间 DEADLOCK_TIMEOUT 触发时，
 * 才会进入本例程。检查是否存在死锁；若
 * 没有，直接返回。（但若 log_lock_waits 为真，则通知 ProcSleep
 * 记一条日志。）若我们有真正的死锁，就把自己从
 * 该锁的等待队列移除，并向 ProcSleep 发出错误信号。
 */
static void
CheckDeadLock(void)
{
	int			i;

	/*
	 * 取得全部共享锁数据结构上的排他锁。必须
	 * 按分区号顺序获取 LWLocks，以免发生 LWLock 死锁。
	 *
	 * 注意，凡是本进程自己持有锁分区锁的地方，最好不要启用
	 * 死锁检查中断，否则这里会
	 * 永远等下去。还要注意 LWLockAcquire 会建立临界
 * 区，因此本例程无法被取消/die
 * 中断。
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(LockHashPartitionLockByIndex(i), LW_EXCLUSIVE);

	/*
	 * 检查在此期间是否已有人唤醒我们。
	 *
	 * 若已经唤醒，我们就可以返回并继续事务 -- 皆大欢喜。
	 * 在我们被唤醒之前，释放锁的进程已经把锁授予我们，因此
	 * 我们知道不必再等。
	 *
	 * 我们通过查看自己是否已从等待队列摘除来检查这一点。
	 * 这是安全的，因为我们持有锁分区锁。
	 */
	if (MyProc->links.prev == NULL ||
		MyProc->links.next == NULL)
		goto check_done;

#ifdef LOCK_DEBUG
	if (Debug_deadlocks)
		DumpAllLocks();
#endif

	/* 运行死锁检查，并设置 deadlock_state 供 ProcSleep 使用 */
	deadlock_state = DeadLockCheck(MyProc);

	if (deadlock_state == DS_HARD_DEADLOCK)
	{
		/*
		 * 糟糕。我们死锁了。
		 *
		 * 让本进程离开等待状态。（注意：靠 lockAwaited 可以做得
		 * 更高效，但这里这样写是为了
		 * 保留灵活性，以便弄掉的是检测到死锁的那一个以外的
		 * 其他事务。）
		 *
		 * RemoveFromWaitQueue 把 MyProc->waitStatus 设为
		 * PROC_WAIT_STATUS_ERROR，因此我们从信号处理函数
		 * 返回之后，ProcSleep 会报告错误。
		 */
		Assert(MyProc->waitLock != NULL);
		RemoveFromWaitQueue(MyProc, LockTagHashCode(&(MyProc->waitLock->tag)));

		/*
		 * 这里就做完了。ProcSleep 将抛出的错误所引起的事务中止
		 * 会使我们持有的其他锁被
		 * 释放，从而让其他进程醒来；我们不必
		 * 在这里做那件事。NOTE：有一个例外，释放我们
		 * 持有的锁时，不会考虑那些在我们刚刚没能得到的锁上、
		 * 排在我们后面而被阻塞的等待者，它们现在也许
		 * 可以唤醒了，因为我们不再挡在它们前面。不过，
		 * RemoveFromWaitQueue 已经负责唤醒任何这样的进程。
		 */
	}

	/*
	 * 然后释放锁。我们按相反顺序做，原因有二：(1)
	 * 其他任何需要其中一把以上锁的人都会按
	 * 递增顺序去锁它们；我们不想在对方能够取得它需要的全部锁之前
	 * 就放开另一个进程。(2) 这样可以避免 LWLockRelease 内部的
	 * O(N^2) 行为。
	 */
check_done:
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(LockHashPartitionLockByIndex(i));
}

/*
 * CheckDeadLockAlert - 处理 deadlock_timeout 到期。
 *
 * NB：在信号处理函数内运行，务必小心。
 */
void
CheckDeadLockAlert(void)
{
	int			save_errno = errno;

	got_deadlock_timeout = true;

	/*
	 * 必须再次置位锁存器，即便 handle_sig_alarm 已经置过。当时
	 * got_deadlock_timeout 还没被设置……这不大可能
	 * 真成问题，但再次置位一把已置位的锁存器很便宜。
	 *
	 * 注意，当本函数在 procsignal_sigusr1_handler() 内运行时，
	 * 处理函数会在此处置位锁存器之后再次置位锁存器。
	 */
	SetLatch(MyLatch);
	errno = save_errno;
}

/*
 * ProcWaitForSignal - 等待来自另一个后端的信号。
 *
 * 由于这里用的是通用进程锁存器，调用方必须能承受
 * 无关的唤醒：始终检查期望的状态是否已经发生，
 * 若没有就再等。
 */
void
ProcWaitForSignal(uint32 wait_event_info)
{
	(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
					 wait_event_info);
	ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

/*
 * ProcSendSignal - 置位由 pgprocno 标识的后端的锁存器
 */
void
ProcSendSignal(int pgprocno)
{
	if (pgprocno < 0 || pgprocno >= ProcGlobal->allProcCount)
		elog(ERROR, "pgprocno out of range");

	SetLatch(&ProcGlobal->allProcs[pgprocno].procLatch);
}

/*
 * BecomeLockGroupLeader - 把进程指定为锁组组长
 *
 * 本函数返回之后，其他进程就可以通过调用 BecomeLockGroupMember
 * 加入该锁组。
 */
void
BecomeLockGroupLeader(void)
{
	LWLock	   *leader_lwlock;

	/* 若已经做过，就不必再做一次。 */
	if (MyProc->lockGroupLeader == MyProc)
		return;

	/* 我们最好还不是跟随者。 */
	Assert(MyProc->lockGroupLeader == NULL);

	/* 创建只包含我们自己的单成员锁组。 */
	leader_lwlock = LockHashPartitionLockByProc(MyProc);
	LWLockAcquire(leader_lwlock, LW_EXCLUSIVE);
	MyProc->lockGroupLeader = MyProc;
	dlist_push_head(&MyProc->lockGroupMembers, &MyProc->lockGroupLink);
	LWLockRelease(leader_lwlock);
}

/*
 * BecomeLockGroupMember - 把进程指定为锁组成员
 *
 * 这相当直接，只是我们要加入的那个组长
 * 可能在我们完成加入之前就退出；
 * 而且该 PGPROC 可能被回收给无关进程。为避免
 * 这一点，我们需要调用方把目标 PGPROC 的 PID 作为
 * 互锁传入。若成功加入目标锁
 * 组则返回 true，否则返回 false。
 */
bool
BecomeLockGroupMember(PGPROC *leader, int pid)
{
	LWLock	   *leader_lwlock;
	bool		ok = false;

	/* 组长无法成为锁组的成员 */
	Assert(MyProc != leader);

	/* 不能已经是某个锁组的成员 */
	Assert(MyProc->lockGroupLeader == NULL);

	/* PID 必须有效。 */
	Assert(pid != 0);

	/*
	 * 取得保护锁组字段的锁。注意 LockHashPartitionLockByProc
	 * 会访问一个也许空闲的 PGPROC 里的 leader->pgprocno。这是安全的，
	 * 因为所有 PGPROCs 的 pgprocno 字段都在共享内存
	 * 初始化期间设定，此后不再改变；因此即便组长 PGPROC 正在被回收，
	 * 我们也会取得正确的锁。
	 */
	leader_lwlock = LockHashPartitionLockByProc(leader);
	LWLockAcquire(leader_lwlock, LW_EXCLUSIVE);

	/* 这是我们要找的组长吗？ */
	if (leader->pid == pid && leader->lockGroupLeader == leader)
	{
		/* 好，加入该锁组 */
		ok = true;
		MyProc->lockGroupLeader = leader;
		dlist_push_tail(&leader->lockGroupMembers, &MyProc->lockGroupLink);
	}
	LWLockRelease(leader_lwlock);

	return ok;
}
