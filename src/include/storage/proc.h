/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  每进程共享内存数据结构
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/clog.h"
#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"

/*
 * 每个后端最多公布 PGPROC_MAX_CACHED_SUBXIDS 个 TransactionIds
 * （当前顶层事务中未中止的子事务）。其他
 * 后端必须把它们视为运行中的 XIDs。
 *
 * 我们还跟踪缓存是否已溢出（即该事务至少生成过一个
 * 未装入缓存的子事务）。若皆未溢出，则可假定
 * 未出现在任何 PGPROC 数组中的 XID 不是运行中事务。
 * 否则必须查看 
 * pg_subtrans。
 *
 * 若改此值，见 src/test/isolation/specs/subxid-overflow.spec。
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64	/* XXX 猜测值 */

typedef struct XidCacheStatus
{
	/* 已缓存 subxid 数，从不超过 PGPROC_MAX_CACHED_SUBXIDS */
	uint8		count;
	/* PGPROC->subxids 溢出? */
	bool		overflowed;
} XidCacheStatus;

struct XidCache
{
	TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * PGPROC->statusFlags 与 PROC_HDR->statusFlags[] 的标志
 */
#define		PROC_IS_AUTOVACUUM	0x01	/* 自动清理工作者？ */
#define		PROC_IN_VACUUM		0x02	/* 正在跑惰性 vacuum */
#define		PROC_IN_SAFE_IC		0x04	/* 正在跑 CREATE INDEX 
										 * CONCURRENTLY/REINDEX
										 * CONCURRENTLY 于非表达式、
										 * 非部分索引) */
#define		PROC_VACUUM_FOR_WRAPAROUND	0x08	/* 仅 autovac 设置 */
#define		PROC_IN_LOGICAL_DECODING	0x10	/* 正在事务外逻辑
												 * 解码 */
#define		PROC_AFFECTS_ALL_HORIZONS	0x20	/* 本进程 xmin 须计入
												 * 所有库的 vacuum
												 * 地平线 */

/* EOXact 复位标志 */
#define		PROC_VACUUM_STATE_MASK \
	(PROC_IN_VACUUM | PROC_IN_SAFE_IC | PROC_VACUUM_FOR_WRAPAROUND)

/*
 * 与 Xmin 相关的标志。凡影响 VACUUM 如何解释进程
 * Xmin 值的标志都须列入此处。
 */
#define		PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)

/*
 * 允许把少量"弱"关系锁（AccessShareLock、
 * RowShareLock、RowExclusiveLock）记在 PGPROC 结构中
 * 而非主锁表。这可减轻锁管理器
 * LWLocks 上的争用。详见 storage/lmgr/README。
 */
#define		FP_LOCK_SLOTS_PER_BACKEND 16

/*
 * 无效的 pgprocno。必须大于我们可能拥有的
 * PGPROC 结构最大数目。见 MAX_BACKENDS 注释。
 */
#define INVALID_PGPROCNO		PG_INT32_MAX

/*
 * PGPROC.delayChkptFlags 的标志
 *
 * 这些标志可用于短暂延迟检查点的启动或完成。
 * 若任后端 PGPROC 中对应位被置位，则该标志
 * 生效。
 *
 * 此处把检查点分为三阶段：(1) 确定 redo 指针将
 * 移到的位置，(2) 把全部数据持久写到磁盘，
 * (3) 把检查点记入 WAL。
 *
 * 置 DELAY_CHKPT_START 可阻止系统从阶段 1 进入
 * 阶段 2。当我们正在做将在阶段 2 刷盘的、记入
 *  WAL 的数据修改时很有用。在写 WAL 前置位，并在
 * 既写完 WAL 又完成对应修改后清除，即可保证：若
 * 该 WAL 记录插在新 redo 点之前，对应数据变更也
 * 会在检查点完成前刷到磁盘。（极常见情形：待改
 * 数据在共享缓冲区中，且写 WAL 前已对相关缓冲
 * 取得独占内容锁，则无需此机制，因阶段 2 会阻塞
 * 到我们释放内容锁并刷写修改后的数据到
 * 磁盘为止。）
 * 阻塞至刷盘完成。
 * disk.)
 *
 * 置 DELAY_CHKPT_COMPLETE 可阻止系统从阶段 2 进入
 * 阶段 3。当我们正在做可能使缓冲失效的、记入 WAL 
 * 的操作（如关系截断）时很有用。此时必须确保那些
 * 已失效因而未被检查点刷写的缓冲，确实在磁盘上被
 * 销毁。重放能应付文件或块不存在，但不能应付
 * 具有错误内容的
 * 块。
 */
#define DELAY_CHKPT_START		(1<<0)
#define DELAY_CHKPT_COMPLETE	(1<<1)

typedef enum
{
	PROC_WAIT_STATUS_OK,
	PROC_WAIT_STATUS_WAITING,
	PROC_WAIT_STATUS_ERROR,
} ProcWaitStatus;

/*
 * 每个后端在共享内存中有一个 PGPROC 结构。另有一张
 * 当前未用的 PGPROC 结构链表，将再分配给新后端。
 *
 * links：PGPROC 所在任意链表的链节。等待锁时，
 * PGPROC 链入该锁的 waitProcs 队列。回收的 PGPROC
 * 链入 ProcGlobal 的 freeProcs。
 *
 * 注意：twophase.c 也会为每个当前预备事务建立虚设
 * PGPROC 结构。这些 PGPROCs 出现在 ProcArray 中，
 * 使预备事务看似仍在运行并正确显示持锁。
 * 必要时可用 pid == 0 把它与真正的 PGPROC 区分开。
 * 预备事务 PGPROC 的信号量与锁活动字段闲置
 * 不用，但其 myProcLocks[] 链表仍然
 * 有效。
 *
 * 本结构许多字段允许无锁访问，如 delayChkptFlags
 * 与 isBackgroundWorker。但须牢记：写镜像字段（见下）
 * 至少要以共享模式持有 ProcArrayLock 或 XidGenLock，
 * 以免 pgxactoff 并发变化。
 *
 * 镜像字段：
 *
 * PGPROC 中某些字段（见 "镜像于 ..." 注释）被镜像到
 * 更密排的 ProcGlobal 数组元素。这些数组以
 * PGPROC->pgxactoff 为下标。两份拷贝须保持一致。
 *
 * 注意：经 pgxactoff 索引的值*绝不可*在未持锁时
 * locks.
 *
 * 详见 PROC_HDR。
 */
struct PGPROC
{
	/* proc->links MUST BE FIRST IN STRUCT (见 ProcSleep,ProcWakeup) */
	dlist_node	links;			/* 在链表中时的链节 */
	dlist_head *procgloballist; /* 拥有本 PGPROC 的链表 */

	PGSemaphore sem;			/* 睡眠用信号量 */
	ProcWaitStatus waitStatus;

	Latch		procLatch;		/* 进程通用锁存器 */


	TransactionId xid;			/* 正在执行的顶层 xid（若运行且
								 * 已分配 XID；否则为
								 * InvalidTransactionId。
								 * 镜像于 ProcGlobal->xids[pgxactoff] */

	TransactionId xmin;			/* 启动事务所见最小运行 XID，
								 * 不含 LAZY VACUUM：vacuum 切勿删除
								 * 由 xid >= xmin 删除的
								 * 元组！ */

	LocalTransactionId lxid;	/* 正在执行的顶层事务本地 ID（若
								 * 运行；否则为
								 * InvalidLocalTransactionId */
	int			pid;			/* 后端进程 ID；预备事务则为 0 */

	int			pgxactoff;		/* 镜像数据在 ProcGlobal 数组中的
								 * PGPROC 之数据的偏移 */

	int			pgprocno;		/* 本 PGPROC 在
								 * ProcGlobal->allProcs 之编号，由
								 * InitProcGlobal()。
								 * allProcs[n].pgprocno == n */

	/* 后端仍在启动时这些字段为零： */
	BackendId	backendId;		/* 本后端 backend ID（若已分配） */
	Oid			databaseId;		/* 本后端正在使用的数据库 OID */
	Oid			roleId;			/* 使用本后端的角色 OID */

	Oid			tempNamespaceId;	/* 本后端所用临时模式
									 * OID */

	bool		isBackgroundWorker; /* 若非普通后端则为真 */

	/*
	 * 热备模式下表示已为当前事务发送冲突信号。
	 * 在持有 ProcArrayLock 时置位/清除（虽非必须）。
	 * 需要时可无锁访问。
	 */
	bool		recoveryConflictPending;

	/* 进程当前等待的 LWLock 信息（若有） */
	uint8		lwWaiting;		/* 见 LWLockWaitState */
	uint8		lwWaitMode;		/* 等待中的 lwlock 模式 */
	proclist_node lwWaitLink;	/* 轻量锁等待列表位置 */

	/* 条件变量支持。 */
	proclist_node cvWaitLink;	/* CV 等待列表位置 */

	/* 进程当前等待的锁信息（若有） */
	/* 未在等待时 waitLock 与 waitProcLock 为 NULL */
	LOCK	   *waitLock;		/* 正在睡眠的锁对象…… */
	PROCLOCK   *waitProcLock;	/* 所待锁的每持有者信息 */
	LOCKMODE	waitLockMode;	/* 正在等待的锁类型 */
	LOCKMASK	heldLocks;		/* 本后端在该锁对象上已持
								 * 有的锁类型位掩码 */
	pg_atomic_uint64 waitStart; /* 开始等待获取锁的
								 * 时刻 */

	int			delayChkptFlags;	/* DELAY_CHKPT_* 标志 */

	uint8		statusFlags;	/* 本后端状态标志，见 PROC_*。
								 * 镜像于
								 * ProcGlobal->statusFlags[pgxactoff] */

	/*
	 * 供在需要时等待同步复制的信息。
	 * 未等待时 waitLSN 为 InvalidXLogRecPtr；仅用户后端设置。
	 * 除拥有进程或 WALSender 外切勿触碰 syncRepState。
	 * 仅持 SyncRepLock 时用 syncRepLinks。
	 */
	XLogRecPtr	waitLSN;		/* 正在等待该 LSN 或更高 */
	int			syncRepState;	/* 同步复制等待态 */
	dlist_node	syncRepLinks;	/* 若在 syncrep 队列中则为链节 */

	/*
	 * 本后端所持有或等待的全部 PROCLOCK，按所属锁
	 * 的分区号链入下列某一
	 * 链表。
	 */
	dlist_head	myProcLocks[NUM_LOCK_PARTITIONS];

	XidCacheStatus subxidStatus;	/* 镜像于
									 * ProcGlobal->subxidStates[i] */
	struct XidCache subxids;	/* 子事务 XIDs 缓存 */

	/* 组 XID 清除支持。 */
	/* 若为等待 XID 清除的 ProcArray 组成员则为真 */
	bool		procArrayGroupMember;
	/* 下一个等待 XID 清除的 ProcArray 组成员 */
	pg_atomic_uint32 procArrayGroupNext;

	/*
	 * 事务主 XID 与子事务中最新的
	 * 事务 ID
	 */
	TransactionId procArrayGroupMemberXid;

	uint32		wait_event_info;	/* 进程等待信息 */

	/* 组事务状态更新支持。 */
	bool		clogGroupMember;	/* 若属 clog 组则为真 */
	pg_atomic_uint32 clogGroupNext; /* 下一 clog 组成员 */
	TransactionId clogGroupMemberXid;	/* clog 组成员的事务 ID */
	XidStatus	clogGroupMemberXidStatus;	/* clog 组成员的事务
											 * 状态 */
	int			clogGroupMemberPage;	/* 对应组成员 xid 的
										 * clog 页 */
	XLogRecPtr	clogGroupMemberLsn; /* clog 组成员提交记录的
									 * WAL 位置 */

	/* 锁管理器数据，记录本后端取得的快速路径锁。 */
	LWLock		fpInfoLock;		/* 保护每后端快速路径状态 */
	uint64		fpLockBits;		/* 各快速路径槽所持锁模式 */
	Oid			fpRelId[FP_LOCK_SLOTS_PER_BACKEND]; /* 关系 OID 槽 */
	bool		fpVXIDLock;		/* 是否持有快速路径 VXID 锁？ */
	LocalTransactionId fpLocalTransactionId;	/* 快速路径 VXID 的
												 * lxid */

	/*
	 * 锁组支持。对组领导者使用 LockHashPartitionLockByProc
	 * 以取得保护这些字段的 LWLock。
	 */
	PGPROC	   *lockGroupLeader;	/* 锁组领导（若我是成员） */
	dlist_head	lockGroupMembers;	/* 成员列表（若为领导） */
	dlist_node	lockGroupLink;	/* 我的成员链节（成员） */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */


extern PGDLLIMPORT PGPROC *MyProc;

/*
 * There is one ProcGlobal struct for the whole database cluster.
 *
 * Adding/Removing an entry into the procarray requires holding *both*
 * ProcArrayLock and XidGenLock in exclusive mode (in that order). Both are
 * needed because the dense arrays (see below) are accessed from
 * GetNewTransactionId() and GetSnapshotData(), and we don't want to add
 * further contention by both using the same lock. Adding/Removing a procarray
 * entry is much less frequent.
 *
 * Some fields in PGPROC are mirrored into more densely packed arrays (e.g.
 * xids), with one entry for each backend. These arrays only contain entries
 * for PGPROCs that have been added to the shared array with ProcArrayAdd()
 * (in contrast to PGPROC array which has unused PGPROCs interspersed).
 *
 * The dense arrays are indexed by PGPROC->pgxactoff. Any concurrent
 * ProcArrayAdd() / ProcArrayRemove() can lead to pgxactoff of a procarray
 * member to change.  Therefore it is only safe to use PGPROC->pgxactoff to
 * access the dense array while holding either ProcArrayLock or XidGenLock.
 *
 * As long as a PGPROC is in the procarray, the mirrored values need to be
 * maintained in both places in a coherent manner.
 *
 * The denser separate arrays are beneficial for three main reasons: First, to
 * allow for as tight loops accessing the data as possible. Second, to prevent
 * updates of frequently changing data (e.g. xmin) from invalidating
 * cachelines also containing less frequently changing data (e.g. xid,
 * statusFlags). Third to condense frequently accessed data into as few
 * cachelines as possible.
 *
 * There are two main reasons to have the data mirrored between these dense
 * arrays and PGPROC. First, as explained above, a PGPROC's array entries can
 * only be accessed with either ProcArrayLock or XidGenLock held, whereas the
 * PGPROC entries do not require that (obviously there may still be locking
 * requirements around the individual field, separate from the concerns
 * here). That is particularly important for a backend to efficiently checks
 * it own values, which it often can safely do without locking.  Second, the
 * PGPROC fields allow to avoid unnecessary accesses and modification to the
 * dense arrays. A backend's own PGPROC is more likely to be in a local cache,
 * whereas the cachelines for the dense array will be modified by other
 * backends (often removing it from the cache for other cores/sockets). At
 * commit/abort time a check of the PGPROC value can avoid accessing/dirtying
 * the corresponding array value.
 *
 * Basically it makes sense to access the PGPROC variable when checking a
 * single backend's data, especially when already looking at the PGPROC for
 * other reasons already.  It makes sense to look at the "dense" arrays if we
 * need to look at many / most entries, because we then benefit from the
 * reduced indirection and better cross-process cache-ability.
 *
 * When entering a PGPROC for 2PC transactions with ProcArrayAdd(), the data
 * in the dense arrays is initialized from the PGPROC while it already holds
 * ProcArrayLock.
 */
typedef struct PROC_HDR
{
	/* Array of PGPROC structures (not including dummies for prepared txns) */
	PGPROC	   *allProcs;

	/* Array mirroring PGPROC.xid for each PGPROC currently in the procarray */
	TransactionId *xids;

	/*
	 * Array mirroring PGPROC.subxidStatus for each PGPROC currently in the
	 * procarray.
	 */
	XidCacheStatus *subxidStates;

	/*
	 * Array mirroring PGPROC.statusFlags for each PGPROC currently in the
	 * procarray.
	 */
	uint8	   *statusFlags;

	/* Length of allProcs array */
	uint32		allProcCount;
	/* Head of list of free PGPROC structures */
	dlist_head	freeProcs;
	/* Head of list of autovacuum's free PGPROC structures */
	dlist_head	autovacFreeProcs;
	/* Head of list of bgworker free PGPROC structures */
	dlist_head	bgworkerFreeProcs;
	/* Head of list of walsender free PGPROC structures */
	dlist_head	walsenderFreeProcs;
	/* First pgproc waiting for group XID clear */
	pg_atomic_uint32 procArrayGroupFirst;
	/* First pgproc waiting for group transaction status update */
	pg_atomic_uint32 clogGroupFirst;
	/* WALWriter process's latch */
	Latch	   *walwriterLatch;
	/* Checkpointer process's latch */
	Latch	   *checkpointerLatch;
	/* Current shared estimate of appropriate spins_per_delay value */
	int			spins_per_delay;
	/* Buffer id of the buffer that Startup process waits for pin on, or -1 */
	int			startupBufferPinWaitBufId;
} PROC_HDR;

extern PGDLLIMPORT PROC_HDR *ProcGlobal;

extern PGDLLIMPORT PGPROC *PreparedXactProcs;

/* Accessor for PGPROC given a pgprocno. */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])

/*
 * We set aside some extra PGPROC structures for auxiliary processes,
 * ie things that aren't full-fledged backends but need shmem access.
 *
 * Background writer, checkpointer, WAL writer and archiver run during normal
 * operation.  Startup process and WAL receiver also consume 2 slots, but WAL
 * writer is launched only after startup has exited, so we only need 5 slots.
 */
#define NUM_AUXILIARY_PROCS		5

/* configurable options */
extern PGDLLIMPORT int DeadlockTimeout;
extern PGDLLIMPORT int StatementTimeout;
extern PGDLLIMPORT int LockTimeout;
extern PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern PGDLLIMPORT int IdleSessionTimeout;
extern PGDLLIMPORT bool log_lock_waits;


/*
 * Function Prototypes
 */
extern int	ProcGlobalSemas(void);
extern Size ProcGlobalShmemSize(void);
extern void InitProcGlobal(void);
extern void InitProcess(void);
extern void InitProcessPhase2(void);
extern void InitAuxiliaryProcess(void);

extern void SetStartupBufferPinWaitBufId(int bufid);
extern int	GetStartupBufferPinWaitBufId(void);

extern bool HaveNFreeProcs(int n, int *nfree);
extern void ProcReleaseLocks(bool isCommit);

extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock, LockMethod lockMethodTable);
extern void ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
extern void CheckDeadLockAlert(void);
extern bool IsWaitingForLock(void);
extern void LockErrorCleanup(void);

extern void ProcWaitForSignal(uint32 wait_event_info);
extern void ProcSendSignal(int pgprocno);

extern PGPROC *AuxiliaryPidGetProc(int pid);

extern void BecomeLockGroupLeader(void);
extern bool BecomeLockGroupMember(PGPROC *leader, int pid);

#endif							/* _PROC_H_ */
