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

/* 注意："typedef struct PGPROC PGPROC" 见 storage/lock.h。 */


extern PGDLLIMPORT PGPROC *MyProc;

/*
 * 整个数据库集簇只有一个 ProcGlobal 结构。
 *
 * 向 procarray 添加/删除条目须*同时*以独占模式持有
 *  ProcArrayLock 与 XidGenLock（按此顺序）。二者都要，
 * 因为密排数组（见下）会被 GetNewTransactionId()
 *  与 GetSnapshotData() 访问，我们不想让二者争用同一
 * 把锁而加重争用。添加/删除条目要少见得
 * 多。
 *
 * PGPROC 中某些字段被镜像到更密排的数组（如
 * xids），每个后端一项。这些数组只含已用
 * ProcArrayAdd() 加入共享数组的 PGPROCs
 * （对比 PGPROC 数组，其中未用 PGPROCs 穿插其间）。
 *
 * 密排数组以 PGPROC->pgxactoff 为下标。任何并发的
 *  ProcArrayAdd() / ProcArrayRemove() 都可能导致某
 * procarray 成员的 pgxactoff 变化。因此仅在持有
 *  ProcArrayLock 或 XidGenLock 时用 PGPROC->pgxactoff 
 *
 * 访问密排数组才安全。只要 PGPROC 仍在 procarray 中，
 * 镜像值须在两处一致维护。
 *
 * 单独的更密数组有三大好处：第一，访问数据的循环可
 * 尽量紧凑。第二，防止频繁变化的数据（如 xmin）更新
 * 时使同缓存行上较少变化的数据（如 xid、
 * statusFlags）失效。第三，把常访问数据压缩到
 * 尽可能少的
 * 缓存行中。
 *
 * 在密排数组与 PGPROC 间做镜像有两主因。第一，
 * 如上所述，PGPROC 的数组条目仅在持有 ProcArrayLock 或
 * XidGenLock 时可访问，而 PGPROC 字段无此需要（当然个
 * 别字段仍可能需要加锁）。这对后端高效
 * 检查自身值很重要，往往可无锁进行。第二，
 * PGPROC 字段可避免对密排数组不必要的访问与修改。
 * 后端自己的 PGPROC 更可能在本地缓存中，而密排数组
 * 的缓存行会被其他后端修改（常从其他核/插槽的
 * 缓存中剔除）。提交/中止时检查 PGPROC 值可避免
 * 访问或弄脏
 * 对应的数组
 * 值。
 * 如上所述。
 *
 * 检查单个后端数据时宜读 PGPROC，尤其已因
 * 其他原因在看该 PGPROC 时。若需查看许多/大多数条目，
 * 则宜看 "dense" 数组，因可减少间接寻址并改善
 * 跨进程缓存性。
 * 缓存友好。
 *
 * 用 ProcArrayAdd() 为 2PC 事务放入 PGPROC 时，密排
 * 数组数据在已持有 ProcArrayLock 时从该 PGPROC 
 * 初始化。
 */
typedef struct PROC_HDR
{
	/* PGPROC 结构数组（不含预备事务虚设项） */
	PGPROC	   *allProcs;

	/* 镜像当前在 procarray 中各 PGPROC.xid 的数组 */
	TransactionId *xids;

	/*
	 * 镜像当前在 procarray 中各 PGPROC.subxidStatus 的
	 * 数组。
	 */
	XidCacheStatus *subxidStates;

	/*
	 * 镜像当前在 procarray 中各 PGPROC.statusFlags 的
	 * 数组。
	 */
	uint8	   *statusFlags;

	/* allProcs 数组长度 */
	uint32		allProcCount;
	/* 空闲 PGPROC 结构链表头 */
	dlist_head	freeProcs;
	/* 自动清理空闲 PGPROC 结构链表头 */
	dlist_head	autovacFreeProcs;
	/* bgworker 空闲 PGPROC 结构链表头 */
	dlist_head	bgworkerFreeProcs;
	/* walsender 空闲 PGPROC 结构链表头 */
	dlist_head	walsenderFreeProcs;
	/* 等待组 XID 清除的首个 pgproc */
	pg_atomic_uint32 procArrayGroupFirst;
	/* 等待组事务状态更新的首个 pgproc */
	pg_atomic_uint32 clogGroupFirst;
	/* WALWriter 的锁存器 */
	Latch	   *walwriterLatch;
	/* Checkpointer 的锁存器 */
	Latch	   *checkpointerLatch;
	/* 合适的 spins_per_delay 值的当前共享估计 */
	int			spins_per_delay;
	/* Startup 进程等待 pin 的缓冲 id，或 -1 */
	int			startupBufferPinWaitBufId;
} PROC_HDR;

extern PGDLLIMPORT PROC_HDR *ProcGlobal;

extern PGDLLIMPORT PGPROC *PreparedXactProcs;

/* 由 pgprocno 取 PGPROC。 */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])

/*
 * 我们为辅助进程预留若干额外 PGPROC 结构，
 * 即非完整后端但需访问共享内存者。
 *
 * 后台写进程、检查点进程、WAL 写进程与归档进程在正常
 * 运行时使用。Startup 与 WAL 接收进程也占 2 槽，但 WAL 
 * 写进程仅在 startup 退出后启动，故只需 5 个槽。
 */
#define NUM_AUXILIARY_PROCS		5

/* 可配置选项 */
extern PGDLLIMPORT int DeadlockTimeout;
extern PGDLLIMPORT int StatementTimeout;
extern PGDLLIMPORT int LockTimeout;
extern PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern PGDLLIMPORT int IdleSessionTimeout;
extern PGDLLIMPORT bool log_lock_waits;


/*
 * 函数原型
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
