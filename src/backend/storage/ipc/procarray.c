/*-------------------------------------------------------------------------
 *
 * procarray.c
 *	  POSTGRES 进程数组代码。
 *
 *
 * 本模块维护所有活动 backend 的 PGPROC 子结构数组，以及
 * ProcGlobal 中的关联数组。用途虽多，但主要用途是确定
 * 当前正在运行的事务集合。
 *
 * 由于存在多种微妙的竞态，backend 在设置或清除其 xid（位于
 * ProcGlobal->xids[]/MyProc->xid）时必须持有正确的锁，这一点至关重要。
 * 参见
 * src/backend/access/transam/README 中的说明。
 *
 * 进程数组现在也包含表示预备
 * 事务的结构。这些结构的 xid 与 subxids 字段有效，
 * myProcLocks 列表亦然。需要时可通过检查 pid == 0
 * 将它们与普通 backend 的 PGPROC 区分开。
 *
 * 在热备期间，我们还维护一份 XID 列表，表示
 * 已知在主库上运行的事务（更准确地说，是截至
 * WAL 流当前点仍在运行的事务）。该列表保存在
 * KnownAssignedXids 数组中，并通过观察
 * 到达的 XID 序列来更新。这样做是必要的：若在为备库查询
 * 拍摄快照时漏掉这些 XID，它们会显得已经
 * 完成，从而导致 MVCC 失败。注意在热备中，PGPROC
 * 数组表示的是备库进程，按定义它们不会运行
 * 带有 XID 的事务。
 *
 * 主库上的 backend 有可能在
 * 未为其事务写入中止记录的情况下终止。虽然这本不该
 * 发生，但会无限期占用 KnownAssignedXids，因此我们在
 * 收到有效的正在运行 XID 列表时修剪该数组以保护自己。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procarray.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_authid.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_lfind.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

/* 我们的共享内存区域 */
typedef struct ProcArrayStruct
{
	int			numProcs;		/* 有效 procs 项的数目 */
	int			maxProcs;		/* procs 数组已分配的大小 */

	/*
	 * 已知已分配 XIDs 的处理
	 */
	int			maxKnownAssignedXids;	/* 数组已分配的大小 */
	int			numKnownAssignedXids;	/* 当前有效项的数目 */
	int			tailKnownAssignedXids;	/* 最旧有效元素的下标 */
	int			headKnownAssignedXids;	/* 最新元素的下标加 1 */
	slock_t		known_assigned_xids_lck;	/* 保护 head/tail 指针 */

	/*
	 * 为防止溢出而从 KnownAssignedXids 数组中移除的最高
	 * subxid；若没有则为 InvalidTransactionId。跟踪它的原因与
	 * 跟踪 PGPROC 项里溢出的已缓存 subxid 类似。修改它必须持有
	 * 排他 ProcArrayLock，读取则持有
	 * 共享锁。
	 */
	TransactionId lastOverflowedXid;

	/* 任一复制槽的最旧 xmin */
	TransactionId replication_slot_xmin;
	/* 任一复制槽的最旧系统目录 xmin */
	TransactionId replication_slot_catalog_xmin;

	/* 指向 allProcs[] 的下标，共 PROCARRAY_MAXPROCS 项 */
	int			pgprocnos[FLEXIBLE_ARRAY_MEMBER];
} ProcArrayStruct;

/*
 * 这是 GlobalVisTest 系列函数的状态。这些函数可以
 * 例如用来判断能否删掉一行而不违反
 * MVCC 语义：若被删行的 xmax 不被任何人视为
 * 仍在运行，该行就可以删掉。
 *
 * 为避免拖慢 GetSnapshotData()，构建快照时不算精确的
 * 截止 XID，因为常变的 xmin 扩展性很差。构建
 * 快照时改为计算两条边界。
 * 两条边界如下：
 *
 * 1) definitely_needed，表示由 XIDs >=
 *    definitely_needed 删除的行肯定仍然可见。
 *
 * 2) maybe_needed，表示由 XIDs < maybe_needed 删除的行
 *    肯定可以删除。
 *
 * 当被测 XID 落在两者之间（即 XID >= maybe_needed
 * 且 XID < definitely_needed）时，可用
 * ComputeXidHorizons() 重算边界以得到更准的答案。这比
 * 始终维护精确值更便宜。
 *
 * 精确边界计算不便宜，所以限制短时间内
 * 发生的次数。见 GlobalVisTestShouldUpdate()。
 *
 *
 * 该结构在后端生命周期内有三个实例，分别针对
 * 不同类型的关系做了优化。例如一个数据库里普通的用户定义表
 * 对连到另一数据库的后端不可访问，所以针对某个关系的测试
 * 可以比针对共享关系的测试更激进。
 * 目前跟踪四种不同状态：
 *
 * 1) GlobalVisSharedRels：仅当任何数据库中的快照、
 *    复制槽的 xmin，以及复制槽的 catalog_xmin 都不可能
 *    仍把该 XID 视为运行中时，才认为该 XID 的效果
 *    对所有人可见。
 *
 * 2) GlobalVisCatalogRels：仅当当前数据库中的快照、
 *    复制槽的 xmin，以及复制槽的 catalog_xmin 都不可能
 *    仍把该 XID 视为运行中时，才认为该 XID 的效果
 *    对所有人可见。
 *
 *    也就是说，与 GlobalVisSharedRels 的差别在于
 *    忽略其他数据库中的快照。
 *
 * 3) GlobalVisDataRels：仅当当前数据库中的快照以及
 *    复制槽的 xmin 都不把该 XID 视为运行中时，才认为
 *    该 XID 的效果对所有人可见。
 *
 *    也就是说，与 GlobalVisCatalogRels 的差别在于
 *    不考虑复制槽的 catalog_xmin。
 *
 * 4) GlobalVisTempRels：只考虑当前会话，因为临时表
 *    对其他会话不可见。
 *
 * GlobalVisTestFor() 返回适合该关系的
 * 状态。
 *
 * 边界用 FullTransactionIds 而不是 TransactionIds，以免
 * 回绕危险。否则例如不存在进程数组状态能
 * 防止 maybe_needed 在 GetSnapshotData()
 * 调用之后变得过旧。
 *
 * 类型定义在头文件里。
 */
struct GlobalVisState
{
	/* XIDs >= 此值的被某些后端视为运行中 */
	FullTransactionId definitely_needed;

	/* XIDs < 此值的不被任何后端视为运行中 */
	FullTransactionId maybe_needed;
};

/*
 * ComputeXidHorizons() 的结果。
 */
typedef struct ComputeXidHorizonsResult
{
	/*
	 * ComputeXidHorizons() 持有 ProcArrayLock 时
	 * ShmemVariableCache->latestCompletedXid 的值。
	 */
	FullTransactionId latest_completed;

	/*
	 * 同样适用于 procArray->replication_slot_xmin 以及
	 * procArray->replication_slot_catalog_xmin。
	 */
	TransactionId slot_xmin;
	TransactionId slot_catalog_xmin;

	/*
	 * 任何后端仍可能视为运行中的最旧 xid。这需要
	 * 包括正在运行 VACUUM 的进程，与普通可见性截止不同，
	 * 因为清理在判断可见性时需要能做 pg_subtrans
	 * 查找，但不关心高于其 xmin 的行
	 * 被删除。
	 *
	 * 这大概只在判断 pg_subtrans 能否截断时需要。出于历史原因，
	 * 它目前包含复制槽的影响。
	 * 但这一点很可能可以改掉。
	 */
	TransactionId oldest_considered_running;

	/*
	 * 共享表里需要保留已删元组的
	 * 最旧 xid。
	 *
	 * 这包括复制槽的影响。若不需要这一点，
	 * 请看 shared_oldest_nonremovable_raw。
	 */
	TransactionId shared_oldest_nonremovable;

	/*
	 * 共享表中可能必须保留的最旧 xid。这与
	 * shared_oldest_nonremovable 相同，只是不受复制槽
	 * catalog_xmin 的影响。
	 *
	 * 这主要用于能通过 hot_standby_feedback 把 catalog_xmin
	 * 发给上游流复制服务器，使它们只在访问
	 * 系统目录表时才施加该限制。
	 */
	TransactionId shared_oldest_nonremovable_raw;

	/*
	 * 非共享系统目录表中需要保留已删元组的
	 * 最旧 xid。
	 */
	TransactionId catalog_oldest_nonremovable;

	/*
	 * 普通用户定义表中需要保留已删元组的
	 * 最旧 xid。
	 */
	TransactionId data_oldest_nonremovable;

	/*
	 * 本会话临时表中需要保留已删元组的
	 * 最旧 xid。
	 */
	TransactionId temp_oldest_nonremovable;
} ComputeXidHorizonsResult;

/*
 * GlobalVisHorizonKindForRel() 的返回值。
 */
typedef enum GlobalVisHorizonKind
{
	VISHORIZON_SHARED,
	VISHORIZON_CATALOG,
	VISHORIZON_DATA,
	VISHORIZON_TEMP
} GlobalVisHorizonKind;

/*
 * KnownAssignedXidsCompress() 的原因码。
 */
typedef enum KAXCompressReason
{
	KAX_NO_SPACE,				/* 需要在数组末尾腾出空间 */
	KAX_PRUNE,					/* 刚刚裁掉了旧项 */
	KAX_TRANSACTION_END,		/* 刚刚提交或移除了一些 XIDs */
	KAX_STARTUP_PROCESS_IDLE	/* 启动进程即将睡眠 */
} KAXCompressReason;


static ProcArrayStruct *procArray;

static PGPROC *allProcs;

/*
 * 用于降低反复调用 TransactionIdIsInProgress() 的开销的缓存
 */
static TransactionId cachedXidIsNotInProgress = InvalidTransactionId;

/*
 * 跟踪恢复期间模拟事务的记账
 */
static TransactionId *KnownAssignedXids;
static bool *KnownAssignedXidsValid;
static TransactionId latestObservedXid = InvalidTransactionId;

/*
 * 若处于 STANDBY_SNAPSHOT_PENDING 状态，standbySnapshotPendingXmin 是
 * 仍可能在运行、但我们还没放进
 * KnownAssignedXids 的最高 xid。
 */
static TransactionId standbySnapshotPendingXmin;

/*
 * 不同类型关系上可见性检查的状态。详见
 * 结构 GlobalVisState。共享、系统目录、普通和临时
 * 关系可以有不同的视界，因此各有一份这样的状态。
 */
static GlobalVisState GlobalVisSharedRels;
static GlobalVisState GlobalVisCatalogRels;
static GlobalVisState GlobalVisDataRels;
static GlobalVisState GlobalVisTempRels;

/*
 * 上次重算精确 xmin 视界时，本后端的 RecentXmin；
 * 若还没算过则为 InvalidTransactionId。用来限制精确视界
 * 被重算的次数。见 GlobalVisTestShouldUpdate()。
 */
static TransactionId ComputeXidHorizonsResultLastXmin;

#ifdef XIDCACHE_DEBUG

/* XidCache 测量用的计数器 */
static long xc_by_recent_xmin = 0;
static long xc_by_known_xact = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_by_known_assigned = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()		(xc_by_recent_xmin++)
#define xc_by_known_xact_inc()		(xc_by_known_xact++)
#define xc_by_my_xact_inc()			(xc_by_my_xact++)
#define xc_by_latest_xid_inc()		(xc_by_latest_xid++)
#define xc_by_main_xid_inc()		(xc_by_main_xid++)
#define xc_by_child_xid_inc()		(xc_by_child_xid++)
#define xc_by_known_assigned_inc()	(xc_by_known_assigned++)
#define xc_no_overflow_inc()		(xc_no_overflow++)
#define xc_slow_answer_inc()		(xc_slow_answer++)

static void DisplayXidCache(void);
#else							/* 非 XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()		((void) 0)
#define xc_by_known_xact_inc()		((void) 0)
#define xc_by_my_xact_inc()			((void) 0)
#define xc_by_latest_xid_inc()		((void) 0)
#define xc_by_main_xid_inc()		((void) 0)
#define xc_by_child_xid_inc()		((void) 0)
#define xc_by_known_assigned_inc()	((void) 0)
#define xc_no_overflow_inc()		((void) 0)
#define xc_slow_answer_inc()		((void) 0)
#endif							/* 处于 XIDCACHE_DEBUG */

/* 备库上处理 KnownAssignedXids 数组的基本操作 */
static void KnownAssignedXidsCompress(KAXCompressReason reason, bool haveLock);
static void KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
								 bool exclusive_lock);
static bool KnownAssignedXidsSearch(TransactionId xid, bool remove);
static bool KnownAssignedXidExists(TransactionId xid);
static void KnownAssignedXidsRemove(TransactionId xid);
static void KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
										TransactionId *subxids);
static void KnownAssignedXidsRemovePreceding(TransactionId removeXid);
static int	KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax);
static int	KnownAssignedXidsGetAndSetXmin(TransactionId *xarray,
										   TransactionId *xmin,
										   TransactionId xmax);
static TransactionId KnownAssignedXidsGetOldestXmin(void);
static void KnownAssignedXidsDisplay(int trace_level);
static void KnownAssignedXidsReset(void);
static inline void ProcArrayEndTransactionInternal(PGPROC *proc, TransactionId latestXid);
static void ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid);
static void MaintainLatestCompletedXid(TransactionId latestXid);
static void MaintainLatestCompletedXidRecovery(TransactionId latestXid);

static inline FullTransactionId FullXidRelativeTo(FullTransactionId rel,
												  TransactionId xid);
static void GlobalVisUpdateApply(ComputeXidHorizonsResult *horizons);

/*
 * 报告 CreateSharedProcArray 所需的共享内存空间。
 */
Size
ProcArrayShmemSize(void)
{
	Size		size;

	/* ProcArray 结构本身的大小 */
#define PROCARRAY_MAXPROCS	(MaxBackends + max_prepared_xacts)

	size = offsetof(ProcArrayStruct, pgprocnos);
	size = add_size(size, mul_size(sizeof(int), PROCARRAY_MAXPROCS));

	/*
	 * 热备处理期间有一个叫 KnownAssignedXids 的数据结构，
	 * 创建在共享内存里。各个后端在 GetSnapshotData()、
	 * TransactionIdIsInProgress() 和 GetRunningTransactionData() 期间
	 * 也会创建本地数据结构。这些函数里创建的主要结构
	 * 大小必须一致，因为有时会把整个数据结构
	 * 拷来拷去。我们把这个大小称为
	 * TOTAL_MAX_CACHED_SUBXIDS。
	 *
	 * 理想情况下只在本次运行确实做热备时才创建该结构，
	 * 但设置共享内存时还不知道
	 * 这一点。
	 */
#define TOTAL_MAX_CACHED_SUBXIDS \
	((PGPROC_MAX_CACHED_SUBXIDS + 1) * PROCARRAY_MAXPROCS)

	if (EnableHotStandby)
	{
		size = add_size(size,
						mul_size(sizeof(TransactionId),
								 TOTAL_MAX_CACHED_SUBXIDS));
		size = add_size(size,
						mul_size(sizeof(bool), TOTAL_MAX_CACHED_SUBXIDS));
	}

	return size;
}

/*
 * 在 postmaster 启动期间初始化共享 PGPROC 数组。
 */
void
CreateSharedProcArray(void)
{
	bool		found;

	/* 创建或挂接到 ProcArray 共享结构 */
	procArray = (ProcArrayStruct *)
		ShmemInitStruct("Proc Array",
						add_size(offsetof(ProcArrayStruct, pgprocnos),
								 mul_size(sizeof(int),
										  PROCARRAY_MAXPROCS)),
						&found);

	if (!found)
	{
		/*
		 * 我们是第一个，进行初始化。
		 */
		procArray->numProcs = 0;
		procArray->maxProcs = PROCARRAY_MAXPROCS;
		procArray->maxKnownAssignedXids = TOTAL_MAX_CACHED_SUBXIDS;
		procArray->numKnownAssignedXids = 0;
		procArray->tailKnownAssignedXids = 0;
		procArray->headKnownAssignedXids = 0;
		SpinLockInit(&procArray->known_assigned_xids_lck);
		procArray->lastOverflowedXid = InvalidTransactionId;
		procArray->replication_slot_xmin = InvalidTransactionId;
		procArray->replication_slot_catalog_xmin = InvalidTransactionId;
		ShmemVariableCache->xactCompletionCount = 1;
	}

	allProcs = ProcGlobal->allProcs;

	/* 若需要，也创建或挂接到 KnownAssignedXids 数组 */
	if (EnableHotStandby)
	{
		KnownAssignedXids = (TransactionId *)
			ShmemInitStruct("KnownAssignedXids",
							mul_size(sizeof(TransactionId),
									 TOTAL_MAX_CACHED_SUBXIDS),
							&found);
		KnownAssignedXidsValid = (bool *)
			ShmemInitStruct("KnownAssignedXidsValid",
							mul_size(sizeof(bool), TOTAL_MAX_CACHED_SUBXIDS),
							&found);
	}
}

/*
 * 把指定的 PGPROC 加入共享数组。
 */
void
ProcArrayAdd(PGPROC *proc)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;
	int			movecount;

	/* 见 ProcGlobal 注释，说明为何两把锁都要持有 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	if (arrayP->numProcs >= arrayP->maxProcs)
	{
		/*
		 * 糟了，没有空位。（这其实不该发生，因为 PGPROC
		 * 结构的供应也是固定的，所以本应更早
		 * 失败。）
		 */
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
	}

	/*
	 * 保持 procs 数组按 (PGPROC *) 排序，以便更好地利用
	 * 引用局部性。遍历 ProcArray 时这很有用，因为更有可能
	 * 在缓存里找到下一个
	 * PGPROC 结构。
	 *
	 * 加入或移除 proc 的次数远低于对 ProcArray 本身的访问，
	 * 所以开销应当可以忽略。
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			procno PG_USED_FOR_ASSERTS_ONLY = arrayP->pgprocnos[index];

		Assert(procno >= 0 && procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[procno].pgxactoff == index);

		/* 若已在数组中找到正确位置，就跳出 */
		if (arrayP->pgprocnos[index] > proc->pgprocno)
			break;
	}

	movecount = arrayP->numProcs - index;
	memmove(&arrayP->pgprocnos[index + 1],
			&arrayP->pgprocnos[index],
			movecount * sizeof(*arrayP->pgprocnos));
	memmove(&ProcGlobal->xids[index + 1],
			&ProcGlobal->xids[index],
			movecount * sizeof(*ProcGlobal->xids));
	memmove(&ProcGlobal->subxidStates[index + 1],
			&ProcGlobal->subxidStates[index],
			movecount * sizeof(*ProcGlobal->subxidStates));
	memmove(&ProcGlobal->statusFlags[index + 1],
			&ProcGlobal->statusFlags[index],
			movecount * sizeof(*ProcGlobal->statusFlags));

	arrayP->pgprocnos[index] = proc->pgprocno;
	proc->pgxactoff = index;
	ProcGlobal->xids[index] = proc->xid;
	ProcGlobal->subxidStates[index] = proc->subxidStatus;
	ProcGlobal->statusFlags[index] = proc->statusFlags;

	arrayP->numProcs++;

	/* 调整其后所有 PGPROCs 的 pgxactoff */
	index++;
	for (; index < arrayP->numProcs; index++)
	{
		int			procno = arrayP->pgprocnos[index];

		Assert(procno >= 0 && procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[procno].pgxactoff == index - 1);

		allProcs[procno].pgxactoff = index;
	}

	/*
	 * 按与获取相反的顺序释放，以降低持有 ProcArrayLock 时
	 * 还要等待 XidGenLock 的频率。
	 */
	LWLockRelease(XidGenLock);
	LWLockRelease(ProcArrayLock);
}

/*
 * 从共享数组中移除指定的 PGPROC。
 *
 * 当 latestXid 是有效 XID 时，我们是在从数组中移除
 * 一个仍存活的两阶段 gxact，从而使它看起来不再处于运行中。
 * 这种情况下必须推进 latestCompletedXid。（这本质上与先
 * ProcArrayEndTransaction 再移除该 PGPROC 相同，但
 * ProcArrayLock 只取一次，并且不破坏 PGPROC 的内容；
 * twophase.c 依赖后者。）
 */
void
ProcArrayRemove(PGPROC *proc, TransactionId latestXid)
{
	ProcArrayStruct *arrayP = procArray;
	int			myoff;
	int			movecount;

#ifdef XIDCACHE_DEBUG
	/* 在后端关闭时转储统计，但准备事务结束时不转储 */
	if (proc->pid != 0)
		DisplayXidCache();
#endif

	/* 见 ProcGlobal 注释，说明为何两把锁都要持有 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	myoff = proc->pgxactoff;

	Assert(myoff >= 0 && myoff < arrayP->numProcs);
	Assert(ProcGlobal->allProcs[arrayP->pgprocnos[myoff]].pgxactoff == myoff);

	if (TransactionIdIsValid(latestXid))
	{
		Assert(TransactionIdIsValid(ProcGlobal->xids[myoff]));

		/* 持锁期间推进全局 latestCompletedXid */
		MaintainLatestCompletedXid(latestXid);

		/* xactCompletionCount 同样处理  */
		ShmemVariableCache->xactCompletionCount++;

		ProcGlobal->xids[myoff] = InvalidTransactionId;
		ProcGlobal->subxidStates[myoff].overflowed = false;
		ProcGlobal->subxidStates[myoff].count = 0;
	}
	else
	{
		/* 不应当在这里试图移除一个仍存活的事务 */
		Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
	}

	Assert(!TransactionIdIsValid(ProcGlobal->xids[myoff]));
	Assert(ProcGlobal->subxidStates[myoff].count == 0);
	Assert(ProcGlobal->subxidStates[myoff].overflowed == false);

	ProcGlobal->statusFlags[myoff] = 0;

	/* 保持 PGPROC 数组有序。见上面的说明 */
	movecount = arrayP->numProcs - myoff - 1;
	memmove(&arrayP->pgprocnos[myoff],
			&arrayP->pgprocnos[myoff + 1],
			movecount * sizeof(*arrayP->pgprocnos));
	memmove(&ProcGlobal->xids[myoff],
			&ProcGlobal->xids[myoff + 1],
			movecount * sizeof(*ProcGlobal->xids));
	memmove(&ProcGlobal->subxidStates[myoff],
			&ProcGlobal->subxidStates[myoff + 1],
			movecount * sizeof(*ProcGlobal->subxidStates));
	memmove(&ProcGlobal->statusFlags[myoff],
			&ProcGlobal->statusFlags[myoff + 1],
			movecount * sizeof(*ProcGlobal->statusFlags));

	arrayP->pgprocnos[arrayP->numProcs - 1] = -1;	/* 供调试 */
	arrayP->numProcs--;

	/*
	 * 调整被移除 PGPROC 之后那些 proc 的 pgxactoff（注意
	 * numProcs 已经减过了）。
	 */
	for (int index = myoff; index < arrayP->numProcs; index++)
	{
		int			procno = arrayP->pgprocnos[index];

		Assert(procno >= 0 && procno < (arrayP->maxProcs + NUM_AUXILIARY_PROCS));
		Assert(allProcs[procno].pgxactoff - 1 == index);

		allProcs[procno].pgxactoff = index;
	}

	/*
	 * 按与获取相反的顺序释放，以降低持有 ProcArrayLock 时
	 * 还要等待 XidGenLock 的频率。
	 */
	LWLockRelease(XidGenLock);
	LWLockRelease(ProcArrayLock);
}


/*
 * ProcArrayEndTransaction：把事务标为不再运行。
 *
 * 提交和中止两种情形都用它。事务的
 * 提交或中止必须已经报告给 WAL 和 pg_xact。
 *
 * proc 目前总是 MyProc，但为了灵活仍显式传入。
 * latestXid 是该事务主 XID 与子事务中最新的 Xid，
 * 若没有 XID 则为 InvalidTransactionId。（必须让
 * 调用方传入 latestXid，而不是从 PGPROC 的
 * 内容里计算，因为 PGPROC 里的 subxid 信息可能
 * 不完整。）
 */
void
ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid)
{
	if (TransactionIdIsValid(latestXid))
	{
		/*
		 * 清除对外公布的 XID 时必须锁住 ProcArrayLock，
		 * 以免在别人正在取快照时退出正在运行的
		 * 事务集合。讨论见
		 * src/backend/access/transam/README。
		 */
		Assert(TransactionIdIsValid(proc->xid));

		/*
		 * 若能立刻取得 ProcArrayLock，就清除自己的 XID
		 * 并释放锁。否则用分组清除 XID 来提高
		 * 效率。
		 */
		if (LWLockConditionalAcquire(ProcArrayLock, LW_EXCLUSIVE))
		{
			ProcArrayEndTransactionInternal(proc, latestXid);
			LWLockRelease(ProcArrayLock);
		}
		else
			ProcArrayGroupClearXid(proc, latestXid);
	}
	else
	{
		/*
		 * 若没有 XID，就不需要上锁，因为不会影响
		 * 别人计算快照。我们可能改变他们对全局 xmin 的
		 * 估计，但这没问题。
		 */
		Assert(!TransactionIdIsValid(proc->xid));
		Assert(proc->subxidStatus.count == 0);
		Assert(!proc->subxidStatus.overflowed);

		proc->lxid = InvalidLocalTransactionId;
		proc->xmin = InvalidTransactionId;

		/* 确保在中止时把它清掉 */
		proc->delayChkptFlags = 0;

		proc->recoveryConflictPending = false;

		/* 必须与 xid/xmin 一起清掉： */
		/* 避免不必要地弄脏共享缓存行 */
		if (proc->statusFlags & PROC_VACUUM_STATE_MASK)
		{
			Assert(!LWLockHeldByMe(ProcArrayLock));
			LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
			Assert(proc->statusFlags == ProcGlobal->statusFlags[proc->pgxactoff]);
			proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
			ProcGlobal->statusFlags[proc->pgxactoff] = proc->statusFlags;
			LWLockRelease(ProcArrayLock);
		}
	}
}

/*
 * 把一个写事务标为不再运行。
 *
 * 这里不做任何上锁；调用方必须处理。
 */
static inline void
ProcArrayEndTransactionInternal(PGPROC *proc, TransactionId latestXid)
{
	int			pgxactoff = proc->pgxactoff;

	/*
	 * 注意：这里需要排他锁，因为我们要改其他
	 * 进程的 PGPROC 项。
	 */
	Assert(LWLockHeldByMeInMode(ProcArrayLock, LW_EXCLUSIVE));
	Assert(TransactionIdIsValid(ProcGlobal->xids[pgxactoff]));
	Assert(ProcGlobal->xids[pgxactoff] == proc->xid);

	ProcGlobal->xids[pgxactoff] = InvalidTransactionId;
	proc->xid = InvalidTransactionId;
	proc->lxid = InvalidLocalTransactionId;
	proc->xmin = InvalidTransactionId;

	/* 确保在中止时把它清掉 */
	proc->delayChkptFlags = 0;

	proc->recoveryConflictPending = false;

	/* 必须与 xid/xmin 一起清掉： */
	/* 避免不必要地弄脏共享缓存行 */
	if (proc->statusFlags & PROC_VACUUM_STATE_MASK)
	{
		proc->statusFlags &= ~PROC_VACUUM_STATE_MASK;
		ProcGlobal->statusFlags[proc->pgxactoff] = proc->statusFlags;
	}

	/* 持锁期间也清掉子事务 XID 缓存 */
	Assert(ProcGlobal->subxidStates[pgxactoff].count == proc->subxidStatus.count &&
		   ProcGlobal->subxidStates[pgxactoff].overflowed == proc->subxidStatus.overflowed);
	if (proc->subxidStatus.count > 0 || proc->subxidStatus.overflowed)
	{
		ProcGlobal->subxidStates[pgxactoff].count = 0;
		ProcGlobal->subxidStates[pgxactoff].overflowed = false;
		proc->subxidStatus.count = 0;
		proc->subxidStatus.overflowed = false;
	}

	/* 持锁期间也推进全局 latestCompletedXid */
	MaintainLatestCompletedXid(latestXid);

	/* xactCompletionCount 同样处理  */
	ShmemVariableCache->xactCompletionCount++;
}

/*
 * ProcArrayGroupClearXid：分组清除 XID。
 *
 * 在提交时若无法立刻以排他模式取得
 * ProcArrayLock，就把自己加入需要清除 XIDs 的
 * 进程列表。第一个把自己加入列表的进程会以排他
 * 模式取得 ProcArrayLock，并代表全体成员执行
 * ProcArrayEndTransactionInternal。这样在许多进程同时
 * 提交时，可以避开 ProcArrayLock 上的大量争用，
 * 因为锁不必从一个正在提交的进程反复
 * 交到下一个。
 */
static void
ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid)
{
	PROC_HDR   *procglobal = ProcGlobal;
	uint32		nextidx;
	uint32		wakeidx;

	/* 我们肯定有一个 XID 要清除。 */
	Assert(TransactionIdIsValid(proc->xid));

	/* 把自己加入需要分组清除 XID 的进程列表。 */
	proc->procArrayGroupMember = true;
	proc->procArrayGroupMemberXid = latestXid;
	nextidx = pg_atomic_read_u32(&procglobal->procArrayGroupFirst);
	while (true)
	{
		pg_atomic_write_u32(&proc->procArrayGroupNext, nextidx);

		if (pg_atomic_compare_exchange_u32(&procglobal->procArrayGroupFirst,
										   &nextidx,
										   (uint32) proc->pgprocno))
			break;
	}

	/*
	 * 若列表原先非空，领导者会清除我们的 XID。不可能有跟随者却没有领导者，
	 * 因为第一个把自己加入列表的进程，其 nextidx 始终是
	 * INVALID_PGPROCNO。
	 * 这一点是确定的。
	 */
	if (nextidx != INVALID_PGPROCNO)
	{
		int			extraWaits = 0;

		/* 睡眠，直到领导者清除我们的 XID。 */
		pgstat_report_wait_start(WAIT_EVENT_PROCARRAY_GROUP_UPDATE);
		for (;;)
		{
			/* 充当读屏障 */
			PGSemaphoreLock(proc->sem);
			if (!proc->procArrayGroupMember)
				break;
			extraWaits++;
		}
		pgstat_report_wait_end();

		Assert(pg_atomic_read_u32(&proc->procArrayGroupNext) == INVALID_PGPROCNO);

		/* 修正被吸收的唤醒所对应的信号量计数 */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(proc->sem);
		return;
	}

	/* 我们是领导者。代表所有人取得锁。 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * 既然已经拿到锁，就清掉等待分组清除 XID 的进程列表，
	 * 并保存指向表头的指针。一次只弹出一个元素
	 * 可能导致 ABA 问题。
	 */
	nextidx = pg_atomic_exchange_u32(&procglobal->procArrayGroupFirst,
									 INVALID_PGPROCNO);

	/* 记住表头，以便放锁后再执行唤醒。 */
	wakeidx = nextidx;

	/* 遍历列表并清除所有 XIDs。 */
	while (nextidx != INVALID_PGPROCNO)
	{
		PGPROC	   *nextproc = &allProcs[nextidx];

		ProcArrayEndTransactionInternal(nextproc, nextproc->procArrayGroupMemberXid);

		/* 移到列表中的下一个 proc。 */
		nextidx = pg_atomic_read_u32(&nextproc->procArrayGroupNext);
	}

	/* 锁现在用完了。 */
	LWLockRelease(ProcArrayLock);

	/*
	 * 既然已经放锁，就回去把所有人唤醒。
	 * 不在持锁时做这件事，以便把持锁时间压到
	 * 最短。唤醒其他进程所需的系统调用
	 * 大概比持锁期间那些简单的内存写
	 * 慢得多。
	 */
	while (wakeidx != INVALID_PGPROCNO)
	{
		PGPROC	   *nextproc = &allProcs[wakeidx];

		wakeidx = pg_atomic_read_u32(&nextproc->procArrayGroupNext);
		pg_atomic_write_u32(&nextproc->procArrayGroupNext, INVALID_PGPROCNO);

		/* 确保先前的写在跟随者继续之前都可见。 */
		pg_write_barrier();

		nextproc->procArrayGroupMember = false;

		if (nextproc != MyProc)
			PGSemaphoreUnlock(nextproc->sem);
	}
}

/*
 * ProcArrayClearTransaction：清除事务字段。
 *
 * 这用在成功准备一个两阶段事务之后。我们并不是在报告
 * 该事务的 XID 不再运行，它仍会显示为运行中，因为两阶段的
 * gxact 也在 ProcArray 里。我们只需要清掉自己的
 * PGPROC。
 */
void
ProcArrayClearTransaction(PGPROC *proc)
{
	int			pgxactoff;

	/*
	 * 目前这里需要排他地锁住 ProcArrayLock，因为下面要
	 * 递增 xactCompletionCount。至少还需要共享
	 * 模式，好让 pgproc->pgxactoff 在下面保持不变。
	 *
	 * 不过，这个动作实际上并不改变任何人对正在运行的 XIDs 集合的看法
	 * （我们的项与已经插入 ProcArray 的 gxact 是重复的），
	 * 若把 xactCompletionCount 做成原子变量，就可以把锁级别降到
	 * 共享。但目前似乎不值得，因为两阶段提交本身已经足够重，
	 * 这里不会成为瓶颈。若将来成为
	 * 瓶颈，也可以考虑把它和随后的
	 * ProcArrayRemove()
	 * 合并起来。
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	pgxactoff = proc->pgxactoff;

	ProcGlobal->xids[pgxactoff] = InvalidTransactionId;
	proc->xid = InvalidTransactionId;

	proc->lxid = InvalidLocalTransactionId;
	proc->xmin = InvalidTransactionId;
	proc->recoveryConflictPending = false;

	Assert(!(proc->statusFlags & PROC_VACUUM_STATE_MASK));
	Assert(!proc->delayChkptFlags);

	/*
	 * 即便事务其实还没提交，也需要递增完成计数。原因是 GetSnapshotData()
	 * 会省略当前事务的 xid，因此若不递增，
	 * 以后就可能复用这份快照。那就糟了，
	 * 因为它可能不把已准备的事务算作运行中。
	 * 所以这里仍然要递增。
	 */
	ShmemVariableCache->xactCompletionCount++;

	/* 也清掉子事务 XID 缓存 */
	Assert(ProcGlobal->subxidStates[pgxactoff].count == proc->subxidStatus.count &&
		   ProcGlobal->subxidStates[pgxactoff].overflowed == proc->subxidStatus.overflowed);
	if (proc->subxidStatus.count > 0 || proc->subxidStatus.overflowed)
	{
		ProcGlobal->subxidStates[pgxactoff].count = 0;
		ProcGlobal->subxidStates[pgxactoff].overflowed = false;
		proc->subxidStatus.count = 0;
		proc->subxidStatus.overflowed = false;
	}

	LWLockRelease(ProcArrayLock);
}

/*
 * 若 ShmemVariableCache->latestCompletedXid 目前更旧，
 * 就把它更新为指向 latestXid。
 */
static void
MaintainLatestCompletedXid(TransactionId latestXid)
{
	FullTransactionId cur_latest = ShmemVariableCache->latestCompletedXid;

	Assert(FullTransactionIdIsValid(cur_latest));
	Assert(!RecoveryInProgress());
	Assert(LWLockHeldByMe(ProcArrayLock));

	if (TransactionIdPrecedes(XidFromFullTransactionId(cur_latest), latestXid))
	{
		ShmemVariableCache->latestCompletedXid =
			FullXidRelativeTo(cur_latest, latestXid);
	}

	Assert(IsBootstrapProcessingMode() ||
		   FullTransactionIdIsNormal(ShmemVariableCache->latestCompletedXid));
}

/*
 * 与 MaintainLatestCompletedXid 相同，但用于 WAL 重放期间。
 */
static void
MaintainLatestCompletedXidRecovery(TransactionId latestXid)
{
	FullTransactionId cur_latest = ShmemVariableCache->latestCompletedXid;
	FullTransactionId rel;

	Assert(AmStartupProcess() || !IsUnderPostmaster);
	Assert(LWLockHeldByMe(ProcArrayLock));

	/*
	 * 需要一个 FullTransactionId 来和 latestXid 比较。恢复期间不能依赖
	 * latestCompletedXid 已经初始化。但在恢复中，启动进程
	 * 可以不上锁就访问 nextXid。
	 */
	rel = ShmemVariableCache->nextXid;
	Assert(FullTransactionIdIsValid(ShmemVariableCache->nextXid));

	if (!FullTransactionIdIsValid(cur_latest) ||
		TransactionIdPrecedes(XidFromFullTransactionId(cur_latest), latestXid))
	{
		ShmemVariableCache->latestCompletedXid =
			FullXidRelativeTo(rel, latestXid);
	}

	Assert(FullTransactionIdIsNormal(ShmemVariableCache->latestCompletedXid));
}

/*
 * ProcArrayInitRecovery：初始化恢复期的 xid 管理环境。
 *
 * 记住启动进程把 CLOG 和 subtrans 初始化到了哪里，
 * 以便在恢复期间确保从那里起无间隙地初始化到
 * 所需要的位置。
 */
void
ProcArrayInitRecovery(TransactionId initializedUptoXID)
{
	Assert(standbyState == STANDBY_INITIALIZED);
	Assert(TransactionIdIsNormal(initializedUptoXID));

	/*
	 * 把 latestObservedXid 设为 SUBTRANS 已初始化到的那个 xid，
	 * 以便从此处起在 RecordKnownAssignedTransactionIds 里扩展它，
	 * 并在 ProcArrayApplyRecoveryInfo() 达到一致时
	 * 也从这里扩展。
	 */
	latestObservedXid = initializedUptoXID;
	TransactionIdRetreat(latestObservedXid);
}

/*
 * ProcArrayApplyRecoveryInfo：应用关于 xid 的恢复信息。
 *
 * 它带我们经过三个状态：已初始化、待定和就绪。
 * 正常情况是直接一路走到就绪，不过
 * 也有非典型情况需要分步走。
 *
 * 用主库上正在运行的事务的数据来建立 KnownAssignedXids 的
 * 初始状态。也用这些记录定期裁剪
 * KnownAssignedXids，因为我们知道有些事务会因 FATAL
 * 错误而没写出中止记录，最终可能导致
 * 溢出。
 *
 * 见 LogStandbySnapshot() 的注释。
 */
void
ProcArrayApplyRecoveryInfo(RunningTransactions running)
{
	TransactionId *xids;
	int			nxids;
	int			i;

	Assert(standbyState >= STANDBY_INITIALIZED);
	Assert(TransactionIdIsValid(running->nextXid));
	Assert(TransactionIdIsValid(running->oldestRunningXid));
	Assert(TransactionIdIsNormal(running->latestCompletedXid));

	/*
	 * 若有过期事务，就移除。
	 */
	ExpireOldKnownAssignedTransactionIds(running->oldestRunningXid);

	/*
	 * 若有过期锁，就移除。
	 */
	StandbyReleaseOldLocks(running->oldestRunningXid);

	/*
	 * 若快照已经有效，就没有别的事了...
	 */
	if (standbyState == STANDBY_SNAPSHOT_READY)
		return;

	/*
	 * 若最初的 RunningTransactionsData 有一份溢出的快照，
	 * 我们就知道快照里缺少一些 subxid。若此后
	 * 继续看到溢出的快照，可能永远无法启动，所以
	 * 再做一次测试，看快照现在是否有效。我们知道
	 * 缺少的 subxid 等于或早于 nextXid。初始化之后
	 * 我们在恢复期间继续应用变更，所以一旦
	 * oldestRunningXid 晚于初始快照的 nextXid，
	 * 就知道不再缺少信息，可以把
	 * 快照标为有效。
	 */
	if (standbyState == STANDBY_SNAPSHOT_PENDING)
	{
		/*
		 * 若快照没有溢出，或者它是空的，就可以重置待定
		 * 状态并改用这份快照。
		 */
		if (running->subxid_status != SUBXIDS_MISSING || running->xcnt == 0)
		{
			/*
			 * 若已经收集了已知已分配的 xid，就需要在应用恢复
			 * 快照之前把它们丢掉。
			 */
			KnownAssignedXidsReset();
			standbyState = STANDBY_INITIALIZED;
		}
		else
		{
			if (TransactionIdPrecedes(standbySnapshotPendingXmin,
									  running->oldestRunningXid))
			{
				standbyState = STANDBY_SNAPSHOT_READY;
				elog(trace_recovery(DEBUG1),
					 "recovery snapshots are now enabled");
			}
			else
				elog(trace_recovery(DEBUG1),
					 "recovery snapshot waiting for non-overflowed snapshot or "
					 "until oldest active xid on standby is at least %u (now %u)",
					 standbySnapshotPendingXmin,
					 running->oldestRunningXid);
			return;
		}
	}

	Assert(standbyState == STANDBY_INITIALIZED);

	/*
	 * 注意：这里至少可能到达两次，所以新代码必须能
	 * 对付这种情况。
	 */

	/*
	 * 还没有别人在跑，但还是把锁拿上
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * KnownAssignedXids 是有序的，所以无法直接把 xid 加进去，必须
	 * 先排序。
	 *
	 * 有些新 xid 是顶层 xid，有些是子事务。
	 * 我们不调用 SubTransSetParent，因为现在还不要紧。若没有
	 * 溢出，所有 xid 都能放进快照，于是不
	 * 需要 subtrans。若以后溢出，一条 xid 分配记录会把
	 * xid 加进 subtrans。若 RunningTransactionsData 溢出了，那么
	 * 反正也没有足够信息来正确更新 subtrans。
	 */

	/*
	 * 分配一个临时数组，以免修改作为参数传入的
	 * 那个数组。
	 */
	xids = palloc(sizeof(TransactionId) * (running->xcnt + running->subxcnt));

	/*
	 * 把尚未完成的 xid 加进临时数组。
	 */
	nxids = 0;
	for (i = 0; i < running->xcnt + running->subxcnt; i++)
	{
		TransactionId xid = running->xids[i];

		/*
		 * running-xacts 快照可能含有在取快照时
		 * 在 procarray 里仍可见、但已经作为已完成写入 WAL 的 xid。
		 * 它们不再运行，所以
		 * 忽略它们。
		 */
		if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			continue;

		xids[nxids++] = xid;
	}

	if (nxids > 0)
	{
		if (procArray->numKnownAssignedXids != 0)
		{
			LWLockRelease(ProcArrayLock);
			elog(ERROR, "KnownAssignedXids is not empty");
		}

		/*
		 * 把数组排序，以便能安全地加进
		 * KnownAssignedXids。
		 *
		 * 必须按逻辑顺序排，因为 KnownAssignedXidsAdd 里会
		 * 调用 TransactionIdFollowsOrEquals 等等。但我们知道这些 XIDs
		 * 来自 RUNNING_XACTS，也就是说只有同一纪元里的普通 XIDs，
		 * 所以这样做是安全的。
		 */
		qsort(xids, nxids, sizeof(TransactionId), xidLogicalComparator);

		/*
		 * 把排好序的快照加进 KnownAssignedXids。running-xacts
		 * 快照可能因已准备事务而含有重复 xid，
		 * 所以忽略它们。
		 */
		for (i = 0; i < nxids; i++)
		{
			if (i > 0 && TransactionIdEquals(xids[i - 1], xids[i]))
			{
				elog(DEBUG1,
					 "found duplicated transaction %u for KnownAssignedXids insertion",
					 xids[i]);
				continue;
			}
			KnownAssignedXidsAdd(xids[i], xids[i], true);
		}

		KnownAssignedXidsDisplay(trace_recovery(DEBUG3));
	}

	pfree(xids);

	/*
	 * latestObservedXid 至少被设到 SUBTRANS 启动到的位置
	 * （参见 ProcArrayInitRecovery()），或设到
	 * RecordKnownAssignedTransactionIds() 被调用过的最大 xid。从那里起
	 * 把 subtrans 初始化到 nextXid - 1。
	 *
	 * 这里需要复制 RecordKnownAssignedTransactionId() 的一部分，
	 * 因为刚加进已知已分配 xid 机制的那些 xid
	 * 并没有经过 RecordKnownAssignedTransactionId()。
	 */
	Assert(TransactionIdIsNormal(latestObservedXid));
	TransactionIdAdvance(latestObservedXid);
	while (TransactionIdPrecedes(latestObservedXid, running->nextXid))
	{
		ExtendSUBTRANS(latestObservedXid);
		TransactionIdAdvance(latestObservedXid);
	}
	TransactionIdRetreat(latestObservedXid);	/* 等于 running->nextXid - 1 */

	/* ----------
	 * 现在已经有了正在运行的 xid，需要设置那些用来
	 * 在快照继续演化时跟踪它们的全局值。
	 *
	 * - latestCompletedXid，它将成为快照的 xmax
	 * - lastOverflowedXid，它表示快照是否溢出
	 * - nextXid。
	 *
	 * 若快照溢出了，仍用已知的内容初始化，
	 * 但恢复快照还不完全有效，因为知道有些
	 * subxid 缺少。不知道具体缺少哪些 subxid，所以保守地
	 * 假定最后一个是 latestObservedXid。
	 * ----------
	 */
	if (running->subxid_status == SUBXIDS_MISSING)
	{
		standbyState = STANDBY_SNAPSHOT_PENDING;

		standbySnapshotPendingXmin = latestObservedXid;
		procArray->lastOverflowedXid = latestObservedXid;
	}
	else
	{
		standbyState = STANDBY_SNAPSHOT_READY;

		standbySnapshotPendingXmin = InvalidTransactionId;

		/*
		 * 若 xids 数组没有包含全部子事务，就必须把此后取到的
		 * 任何快照标为溢出。
		 */
		if (running->subxid_status == SUBXIDS_IN_SUBTRANS)
			procArray->lastOverflowedXid = latestObservedXid;
		else
		{
			Assert(running->subxid_status == SUBXIDS_IN_ARRAY);
			procArray->lastOverflowedXid = InvalidTransactionId;
		}
	}

	/*
	 * 若某个事务在取快照和把快照写入日志之间的空隙里写了提交记录，
	 * latestCompletedXid 可能已经高于
	 * 快照里的值，所以用传入值之前先检查。
	 * 它也可能根本还没被设置。
	 */
	MaintainLatestCompletedXidRecovery(running->latestCompletedXid);

	/*
	 * 注意：这里不需要递增 ShmemVariableCache->xactCompletionCount，
	 * 还没有人能看见它。
	 */

	LWLockRelease(ProcArrayLock);

	/* ShmemVariableCache->nextXid 必须超出任何已观察到的 xid。 */
	AdvanceNextFullTransactionIdPastXid(latestObservedXid);

	Assert(FullTransactionIdIsValid(ShmemVariableCache->nextXid));

	KnownAssignedXidsDisplay(trace_recovery(DEBUG3));
	if (standbyState == STANDBY_SNAPSHOT_READY)
		elog(trace_recovery(DEBUG1), "recovery snapshots are now enabled");
	else
		elog(trace_recovery(DEBUG1),
			 "recovery snapshot waiting for non-overflowed snapshot or "
			 "until oldest active xid on standby is at least %u (now %u)",
			 standbySnapshotPendingXmin,
			 running->oldestRunningXid);
}

/*
 * ProcArrayApplyXidAssignment
 *		处理一条 XLOG_XACT_ASSIGNMENT WAL 记录
 */
void
ProcArrayApplyXidAssignment(TransactionId topxid,
							int nsubxids, TransactionId *subxids)
{
	TransactionId max_xid;
	int			i;

	Assert(standbyState >= STANDBY_INITIALIZED);

	max_xid = TransactionIdLatest(topxid, nsubxids, subxids);

	/*
	 * 把所有子事务标为已观察到。
	 *
	 * 注意：若 subxid 里先前未观察到的 xid 过多，多到
	 * 放不进 known-assigned-xids，这里会失败。按现有代码
	 * 这不该发生，因为 xid 分配记录绝不应当包含
	 * 超过 PGPROC_MAX_CACHED_SUBXIDS 项。
	 */
	RecordKnownAssignedTransactionIds(max_xid);

	/*
	 * 注意我们用顶层 xid 而不是父 xid 来更新 pg_subtrans。
	 * 这是正常处理与恢复之间的差别，但在所有情况下仍然正确。
	 * 原因是子事务提交要到提交处理时才记入 clog，所以
	 * 所有已中止的子事务都已经在 clog 里明确标出。
	 * 因此可以直接引用顶层
	 * 事务的状态，而不必跳过子事务树里所有的中间
	 * 状态。这应当是我们第一次尝试
	 * 调用 SubTransSetParent()。
	 * 此前还没有建立过这种父子关系。
	 */
	for (i = 0; i < nsubxids; i++)
		SubTransSetParent(subxids[i], topxid);

	/* KnownAssignedXids 还没在维护，所以目前到此为止 */
	if (standbyState == STANDBY_INITIALIZED)
		return;

	/*
	 * 使用与事务提交相同的加锁
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * 从 known-assigned-xacts 中移除 subxid。
	 */
	KnownAssignedXidsRemoveTree(InvalidTransactionId, nsubxids, subxids);

	/*
	 * 把 lastOverflowedXid 至少推进到这些 subxid 中的最后一个。
	 */
	if (TransactionIdPrecedes(procArray->lastOverflowedXid, max_xid))
		procArray->lastOverflowedXid = max_xid;

	LWLockRelease(ProcArrayLock);
}

/*
 * TransactionIdIsInProgress：给定事务是否在某个后端里运行。
 *
 * 除了检查 RecentXmin 和我们自己的 Xid 这类捷径之外，
 * 寻找正在运行的事务有四种可能：
 *
 * 1. 给定 Xid 是主事务 Id。查看 ProcGlobal->xids
 * 就能便宜地发现这一点。
 *
 * 2. 给定 Xid 是 PGPROC 数组里缓存的某个子事务 Xid。
 * 这也能便宜地发现。
 *
 * 3. 在热备模式下，必须搜索 KnownAssignedXids 列表，看
 * 该 Xid 是否在主库上运行。
 *
 * 4. 搜索 SubTrans 树，找到该 Xid 的最顶层父事务，再看
 * 按 ProcGlobal->xids[] 或 KnownAssignedXids 它是否在运行。
 * 这是最慢的办法，但若其他办法都失败了，可惜总是得做，
 * 除非看到缓存的子事务集合是完整的（没有一个
 * 溢出）。
 *
 * 做 1、2、3 时必须持有 ProcArrayLock。若在做 1 和 3 时保存顶层 Xid，
 * 就可以在做 4 时释放 ProcArrayLock。
 * 这换回一些并发（而且反正也不能再从
 * ProcGlobal->xids[] 取回主 Xid；见 GetNewTransactionId）。
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{
	static TransactionId *xids = NULL;
	static TransactionId *other_xids;
	XidCacheStatus *other_subxidstates;
	int			nxids = 0;
	ProcArrayStruct *arrayP = procArray;
	TransactionId topxid;
	TransactionId latestCompletedXid;
	int			mypgxactoff;
	int			numProcs;
	int			j;

	/*
	 * 不必检查比 RecentXmin 更旧的事务；它无法
	 * 仍在运行。（特别是，这保证我们会拒绝
	 * InvalidTransactionId、FrozenTransactionId 等，认定它们不在
	 * 运行。）
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
	{
		xc_by_recent_xmin_inc();
		return false;
	}

	/*
	 * 我们可能刚查过这个事务的状态，所以若已经
	 * 知道它已完成，就可以退出，而不访问
	 * 共享内存。
	 */
	if (TransactionIdEquals(cachedXidIsNotInProgress, xid))
	{
		xc_by_known_xact_inc();
		return false;
	}

	/*
	 * 同样，自己的事务（以及子事务）也可以不访问
	 * 共享内存就处理。
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		xc_by_my_xact_inc();
		return true;
	}

	/*
	 * 若是第一次经过，就取得工作区来记住主 XIDs。
	 * 把它永久 malloc 出来，以免反复 palloc/pfree 的开销。
	 */
	if (xids == NULL)
	{
		/*
		 * 在热备模式下，留出足够空间来装下 known-assigned 列表里的
		 * 全部 xid。若以后结束恢复，就不再需要
		 * 更大的数组，但我们懒得把它缩小。
		 */
		int			maxxids = RecoveryInProgress() ? TOTAL_MAX_CACHED_SUBXIDS : arrayP->maxProcs;

		xids = (TransactionId *) malloc(maxxids * sizeof(TransactionId));
		if (xids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	other_xids = ProcGlobal->xids;
	other_subxidstates = ProcGlobal->subxidStates;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	/*
	 * 既然有了锁，就可以检查 latestCompletedXid；若目标
	 * Xid 在它之后，就肯定仍在运行。
	 */
	latestCompletedXid =
		XidFromFullTransactionId(ShmemVariableCache->latestCompletedXid);
	if (TransactionIdPrecedes(latestCompletedXid, xid))
	{
		LWLockRelease(ProcArrayLock);
		xc_by_latest_xid_inc();
		return true;
	}

	/* 没有捷径，只能把数组翻一遍 */
	mypgxactoff = MyProc->pgxactoff;
	numProcs = arrayP->numProcs;
	for (int pgxactoff = 0; pgxactoff < numProcs; pgxactoff++)
	{
		int			pgprocno;
		PGPROC	   *proc;
		TransactionId pxid;
		int			pxids;

		/* 忽略我们自己，上面已经处理过 */
		if (pgxactoff == mypgxactoff)
			continue;

		/* xid 只取一次，见 GetNewTransactionId */
		pxid = UINT32_ACCESS_ONCE(other_xids[pgxactoff]);

		if (!TransactionIdIsValid(pxid))
			continue;

		/*
		 * 第 1 步：检查主 Xid
		 */
		if (TransactionIdEquals(pxid, xid))
		{
			LWLockRelease(ProcArrayLock);
			xc_by_main_xid_inc();
			return true;
		}

		/*
		 * 可以忽略比目标 Xid 更年轻的主 Xid，因为
		 * 目标无法成为它们的子事务。
		 */
		if (TransactionIdPrecedes(xid, pxid))
			continue;

		/*
		 * 第 2 步：检查缓存的子 Xid 数组
		 */
		pxids = other_subxidstates[pgxactoff].count;
		pg_read_barrier();		/* 与 GetNewTransactionId() 里的屏障配对 */
		pgprocno = arrayP->pgprocnos[pgxactoff];
		proc = &allProcs[pgprocno];
		for (j = pxids - 1; j >= 0; j--)
		{
			/* xid 只取一次，见 GetNewTransactionId */
			TransactionId cxid = UINT32_ACCESS_ONCE(proc->subxids.xids[j]);

			if (TransactionIdEquals(cxid, xid))
			{
				LWLockRelease(ProcArrayLock);
				xc_by_child_xid_inc();
				return true;
			}
		}

		/*
		 * 为第 4 步保存主 Xid。只需要记住那些
		 * 有未缓存子事务的主 Xid。（注意：这里没有竞争
		 * 条件，因为持有 ProcArrayLock 期间，溢出标志无法被清除，只能被设置。
		 * 所以不会漏掉需要
		 * 担心的 Xid。）
		 */
		if (other_subxidstates[pgxactoff].overflowed)
			xids[nxids++] = pxid;
	}

	/*
	 * 第 3 步：在热备模式下，检查 known-assigned-xids 列表。列表里的 XIDs
	 * 必须当作正在运行。
	 */
	if (RecoveryInProgress())
	{
		/* 热备模式下不应当有 PGPROC 项带有 XIDs */
		Assert(nxids == 0);

		if (KnownAssignedXidExists(xid))
		{
			LWLockRelease(ProcArrayLock);
			xc_by_known_assigned_inc();
			return true;
		}

		/*
		 * 若 KnownAssignedXids 溢出了，还必须检查 pg_subtrans。
		 * 从 KnownAssignedXids 取出所有低于
		 * xid 的 xid，因为若 xid 是子事务，其父事务的值总是
		 * 更低。注意这里会同时收集主 xid 和子 xid，
		 * 但这是没办法的事。
		 */
		if (TransactionIdPrecedesOrEquals(xid, procArray->lastOverflowedXid))
			nxids = KnownAssignedXidsGet(xids, xid);
	}

	LWLockRelease(ProcArrayLock);

	/*
	 * 若相关缓存都没有溢出，甚至不用看 pg_subtrans
	 * 就知道该 Xid 不在运行。
	 */
	if (nxids == 0)
	{
		xc_no_overflow_inc();
		cachedXidIsNotInProgress = xid;
		return false;
	}

	/*
	 * 第 4 步：必须检查 pg_subtrans。
	 *
	 * 到这里，我们知道它要么是 xids[] 里某个 Xid 的子事务，
	 * 要么不在运行。若它是已经失败的
	 * 子事务，即使父事务可能仍在运行，我们也要说不在运行。
	 * 所以先查 pg_xact，看它是否已被中止。
	 */
	xc_slow_answer_inc();

	if (TransactionIdDidAbort(xid))
	{
		cachedXidIsNotInProgress = xid;
		return false;
	}

	/*
	 * 它没有被中止，所以检查它所属的事务树
	 * 是否仍在运行（更准确地说，是我们持有
	 * ProcArrayLock 时它是否在运行）。
	 */
	topxid = SubTransGetTopmostTransaction(xid);
	Assert(TransactionIdIsValid(topxid));
	if (!TransactionIdEquals(topxid, xid) &&
		pg_lfind32(topxid, xids, nxids))
		return true;

	cachedXidIsNotInProgress = xid;
	return false;
}

/*
 * TransactionIdIsActive：xid 是否是某个活动后端的顶层 XID？
 *
 * 它与 TransactionIdIsInProgress 的差别在于忽略已准备
 * 事务，以及在热备时忽略主库上正在运行的事务。另外，
 * 我们忽略子事务，因为当前用途
 * 不需要它们。
 */
bool
TransactionIdIsActive(TransactionId xid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	int			i;

	/*
	 * 不必检查比 RecentXmin 更旧的事务；它无法
	 * 仍在运行。
	 */
	if (TransactionIdPrecedes(xid, RecentXmin))
		return false;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (i = 0; i < arrayP->numProcs; i++)
	{
		int			pgprocno = arrayP->pgprocnos[i];
		PGPROC	   *proc = &allProcs[pgprocno];
		TransactionId pxid;

		/* xid 只取一次，见 GetNewTransactionId */
		pxid = UINT32_ACCESS_ONCE(other_xids[i]);

		if (!TransactionIdIsValid(pxid))
			continue;

		if (proc->pid == 0)
			continue;			/* 忽略已准备事务 */

		if (TransactionIdEquals(pxid, xid))
		{
			result = true;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}


/*
 * 确定 XID 视界。
 *
 * 包装函数会用到它，例如 GetOldestNonRemovableTransactionId()
 * （用于 VACUUM）、GetReplicationHorizons()（用于 hot_standby_feedback）等，
 * 以及 GlobalVisUpdate() 在内部使用（见结构
 * GlobalVisState 上面的注释）。
 *
 * 各种算出的视界见
 * ComputeXidHorizonsResult 的定义。
 *
 * 对 VACUUM 会分别计算共享表和非共享表的视界（用来决定哪些
 * 已删元组必须保留）。共享关系必须考虑所有数据库里的后端，
 * 但非共享关系则不需要，因为只有我自己数据库里的后端才可能
 * 看见其中的元组。另外，可以忽略并发运行的惰性
 * VACUUMs，因为 (a) 它们一定在处理别的表，并且 (b) 它们
 * 不需要做基于快照的
 * 查找。
 *
 * 这里也计算用来截断 pg_subtrans 的视界。为此
 * 必须考虑所有数据库里的后端，并且无法忽略并发运行的惰性
 * VACUUM，因为它们仍可能访问
 * pg_subtrans。
 *
 * 注意：我们把当前所有正在运行的 xid 都放进所考虑的 xid 集合。
 * 这保证若一个刚启动的事务还没设置快照，
 * 等它设置快照时，无法把 xmin 设得比我们算出的更小。
 * 见 src/backend/access/transam/README 里的说明。
 *
 * 注意：尽管如此，反复调用时算出的值仍可能
 * 向后移动。算出的值是保守的，所以
 * 比它更旧的东西肯定不再被任何人视为
 * 运行中，但确切算出的值取决于许多因素。例如，
 * 若当前数据库里没有正在运行的事务，
 * 普通表的视界就是 latestCompletedXid。若此后有事务
 * 开始，它的 xmin 会包含其他数据库里更早开始的进行中事务，
 * 于是另一次调用会返回更低的值。
 * 尽管如此，用第一次的结果来清理当前数据库里的表仍然是安全的。
 * 还有与复制有关的影响：walsender
 * 进程可以根据在主库上已不再运行、
 * 但在备库上仍在重放的事务来设置 xmin，从而可能
 * 使这些值向后走。这种情况下有可能
 * 丢掉备库希望保留的数据，但除非备库
 * 用复制槽让 xmin 持久化，否则我们几乎无能为力。
 * 只有 walsender 在备库执行查询期间持续运行，
 * 数据才受到保护。（热备代码处理
 * 这类情况的办法是让需要访问已被删除数据的备库查询失败，
 * 所以没有完整性缺陷。）
 *
 * 注意：近似视界（见 GlobalVisState 的定义）由
 * 这里所做的计算来更新。目前为了正确性以及一点小的加快，
 * 这是必需的。不这样做的话，堆清理对 heap_page_prune() 的调用
 * 可能用比后来决定哪些元组可以删除时更保守的视界，
 * 而代码并不预期这种情况
 * （会破坏 HOT）。
 */
static void
ComputeXidHorizons(ComputeXidHorizonsResult *h)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId kaxmin;
	bool		in_recovery = RecoveryInProgress();
	TransactionId *other_xids = ProcGlobal->xids;

	/* 在 ProcArrayLock 释放之后推断出来 */
	h->catalog_oldest_nonremovable = InvalidTransactionId;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	h->latest_completed = ShmemVariableCache->latestCompletedXid;

	/*
	 * 我们用 latestCompletedXid + 1 来初始化 MIN() 计算。这是
	 * 以后可能出现在 ProcArray 里的 XIDs 的下界，
	 * 从而防止因将来的加入而
	 * 高估结果。
	 */
	{
		TransactionId initial;

		initial = XidFromFullTransactionId(h->latest_completed);
		Assert(TransactionIdIsValid(initial));
		TransactionIdAdvance(initial);

		h->oldest_considered_running = initial;
		h->shared_oldest_nonremovable = initial;
		h->data_oldest_nonremovable = initial;

		/*
		 * 只有本后端所做的修改才会影响临时关系的视界。与其在
		 * 对所有 PGPROCs 的循环里每次检查，不如直接初始化为
		 * 当前任一顶层 xid，这样更
		 * 便宜。
		 *
		 * 若还没有分配 xid，视界可以激进到
		 * GetNewTransactionId()，但用便宜得多的
		 * latestCompletedXid + 1 就够了：若本后端在那里没有 xid，
		 * 按定义，临时表里不会有比
		 * latestCompletedXid 更新的变更。
		 */
		if (TransactionIdIsValid(MyProc->xid))
			h->temp_oldest_nonremovable = MyProc->xid;
		else
			h->temp_oldest_nonremovable = initial;
	}

	/*
	 * 在持有 ProcArrayLock 时取出槽的视界。
	 * LWLockAcquire/LWLockRelease 是屏障，保证这件事发生在
	 * 锁的内部。
	 */
	h->slot_xmin = procArray->replication_slot_xmin;
	h->slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

	for (int index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		int8		statusFlags = ProcGlobal->statusFlags[index];
		TransactionId xid;
		TransactionId xmin;

		/* xid 只取一次，见 GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);
		xmin = UINT32_ACCESS_ONCE(proc->xmin);

		/*
		 * 同时考虑该事务的 Xmin 和它的 Xid。
		 *
		 * 两者都必须检查，因为事务可能有 Xmin 但还没有
		 * Xid；反过来，若它有 Xid，那可能决定某个
		 * 尚未设置的 Xmin。
		 */
		xmin = TransactionIdOlder(xmin, xid);

		/* 若两者都没设置，这个 proc 不影响视界 */
		if (!TransactionIdIsValid(xmin))
			continue;

		/*
		 * 在判断哪些事务可能被视为运行中时，不要忽略任何 proc。
		 * 虽然复制槽应当即使没有这项检查也能保护逻辑解码
		 * 后端，但把它们也算进来
		 * 没有坏处。
		 */
		h->oldest_considered_running =
			TransactionIdOlder(h->oldest_considered_running, xmin);

		/*
		 * 跳过正在清理的后端（只要不截断 pg_subtrans，行被
		 * 删掉也没问题）或正在做逻辑
		 * 解码的后端（它另行管理 xmin，见下面的检查）。
		 */
		if (statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			continue;

		/* 共享表需要把所有数据库里的后端都考虑进去 */
		h->shared_oldest_nonremovable =
			TransactionIdOlder(h->shared_oldest_nonremovable, xmin);

		/*
		 * 通常，其他数据库里的会话除了共享视界之外
		 * 一律忽略。
		 *
		 * 不过，当 MyDatabaseId 尚未设置时要把它们算进去。正在
		 * 启动的后端不得算出过于激进的
		 * 视界，否则我们可能用它把仍然需要的数据
		 * 裁掉。若当前后端从不连接到某个
		 * 数据库，这也无害，因为 data_oldest_nonremovable 将
		 * 永远不会被用到。
		 *
		 * 另外，标了 PROC_AFFECTS_ALL_HORIZONS 的会话应当始终
		 * 被包括。（这个标志用于热备反馈，它
		 * 没法绑定到某个特定数据库。）
		 *
		 * 另外，恢复期间没法算出准确的按数据库
		 * 视界，因为所有 xid 都经由 KnownAssignedXids
		 * 机制来管理。
		 */
		if (proc->databaseId == MyDatabaseId ||
			MyDatabaseId == InvalidOid ||
			(statusFlags & PROC_AFFECTS_ALL_HORIZONS) ||
			in_recovery)
		{
			h->data_oldest_nonremovable =
				TransactionIdOlder(h->data_oldest_nonremovable, xmin);
		}
	}

	/*
	 * 若在恢复中，取出 KnownAssignedXids 里最旧的 xid，放锁之后
	 * 再应用。
	 */
	if (in_recovery)
		kaxmin = KnownAssignedXidsGetOldestXmin();

	/*
	 * 不再需要共享状态里的其他信息，立刻
	 * 放锁。其余计算可以不上锁完成。
	 */
	LWLockRelease(ProcArrayLock);

	if (in_recovery)
	{
		h->oldest_considered_running =
			TransactionIdOlder(h->oldest_considered_running, kaxmin);
		h->shared_oldest_nonremovable =
			TransactionIdOlder(h->shared_oldest_nonremovable, kaxmin);
		h->data_oldest_nonremovable =
			TransactionIdOlder(h->data_oldest_nonremovable, kaxmin);
		/* 恢复期间无法访问临时关系 */
	}

	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->shared_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->data_oldest_nonremovable));

	/*
	 * 检查是否有复制槽需要更旧的 xmin。
	 */
	h->shared_oldest_nonremovable =
		TransactionIdOlder(h->shared_oldest_nonremovable, h->slot_xmin);
	h->data_oldest_nonremovable =
		TransactionIdOlder(h->data_oldest_nonremovable, h->slot_xmin);

	/*
	 * 系统目录视界与数据视界的唯一差别是，槽的
	 * 系统目录 xmin 会作用到系统目录视界上（以便逻辑解码能访问系统目录）。
	 * 先用数据视界初始化，然后在必要时再
	 * 往回退。共享视界也得往回退，因为
	 * 它里面也可能有系统目录。
	 */
	h->shared_oldest_nonremovable_raw = h->shared_oldest_nonremovable;
	h->shared_oldest_nonremovable =
		TransactionIdOlder(h->shared_oldest_nonremovable,
						   h->slot_catalog_xmin);
	h->catalog_oldest_nonremovable = h->data_oldest_nonremovable;
	h->catalog_oldest_nonremovable =
		TransactionIdOlder(h->catalog_oldest_nonremovable,
						   h->slot_catalog_xmin);

	/*
	 * 槽有可能把视界退得比 oldest_considered_running 更旧。
	 * 修正这一点。
	 */
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->shared_oldest_nonremovable);
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->catalog_oldest_nonremovable);
	h->oldest_considered_running =
		TransactionIdOlder(h->oldest_considered_running,
						   h->data_oldest_nonremovable);

	/*
	 * 共享视界至少要和当前数据库里可见的最旧值
	 * 一样旧。
	 */
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->data_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->shared_oldest_nonremovable,
										 h->catalog_oldest_nonremovable));

	/*
	 * 视界需要保证相关后端仍然能够访问
	 * pg_subtrans。
	 */
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->shared_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->catalog_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->data_oldest_nonremovable));
	Assert(TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->temp_oldest_nonremovable));
	Assert(!TransactionIdIsValid(h->slot_xmin) ||
		   TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->slot_xmin));
	Assert(!TransactionIdIsValid(h->slot_catalog_xmin) ||
		   TransactionIdPrecedesOrEquals(h->oldest_considered_running,
										 h->slot_catalog_xmin));

	/* 用算出的视界更新近似视界 */
	GlobalVisUpdateApply(h);
}

/*
 * 判断一个关系需要使用哪种可见性视界。
 * 若 rel 为 NULL，就用最保守的视界。
 */
static inline GlobalVisHorizonKind
GlobalVisHorizonKindForRel(Relation rel)
{
	/*
	 * 其他 relkind 目前既不含 xid，也并不总有必要的
	 * 逻辑解码标记。
	 */
	Assert(!rel ||
		   rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW ||
		   rel->rd_rel->relkind == RELKIND_TOASTVALUE);

	if (rel == NULL || rel->rd_rel->relisshared || RecoveryInProgress())
		return VISHORIZON_SHARED;
	else if (IsCatalogRelation(rel) ||
			 RelationIsAccessibleInLogicalDecoding(rel))
		return VISHORIZON_CATALOG;
	else if (!RELATION_IS_LOCAL(rel))
		return VISHORIZON_DATA;
	else
		return VISHORIZON_TEMP;
}

/*
 * 返回传入表中必须保留已删元组的最旧
 * XID。
 *
 * 若 rel 不是 NULL，视界可能比否则情况下新得多
 * （也就是可删除的元组更少）。NULL 情况下会返回一个对所有关系都正确
 * （但不是最优）的视界。
 *
 * VACUUM 用它来决定传入表中哪些已删元组必须
 * 保留。
 */
TransactionId
GetOldestNonRemovableTransactionId(Relation rel)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	switch (GlobalVisHorizonKindForRel(rel))
	{
		case VISHORIZON_SHARED:
			return horizons.shared_oldest_nonremovable;
		case VISHORIZON_CATALOG:
			return horizons.catalog_oldest_nonremovable;
		case VISHORIZON_DATA:
			return horizons.data_oldest_nonremovable;
		case VISHORIZON_TEMP:
			return horizons.temp_oldest_nonremovable;
	}

	/* 只为消除编译器告警 */
	return InvalidTransactionId;
}

/*
 * 返回任何当前正在运行的后端仍可能视为运行中的最旧事务 id。
 * 不应当把它用于可见性或裁剪判断
 * （见 GetOldestNonRemovableTransactionId()），而是用于
 * 诸如 pg_subtrans 可以截断到哪里这类决定。
 */
TransactionId
GetOldestTransactionIdConsideredRunning(void)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	return horizons.oldest_considered_running;
}

/*
 * 返回热备反馈消息所用的可见性视界。
 */
void
GetReplicationHorizons(TransactionId *xmin, TransactionId *catalog_xmin)
{
	ComputeXidHorizonsResult horizons;

	ComputeXidHorizons(&horizons);

	/*
	 * 这里不要用 shared_oldest_nonremovable，因为它包含
	 * 复制槽 catalog_xmin 的影响。我们想为系统目录视界
	 * 单独发一份反馈，这样主库可以更激进地
	 * 删除数据表的内容。
	 */
	*xmin = horizons.shared_oldest_nonremovable_raw;
	*catalog_xmin = horizons.slot_catalog_xmin;
}

/*
 * GetMaxSnapshotXidCount：取得快照 XID 数组的最大尺寸。
 *
 * 必须把它导出给 snapmgr.c 使用。
 */
int
GetMaxSnapshotXidCount(void)
{
	return procArray->maxProcs;
}

/*
 * GetMaxSnapshotSubxidCount：取得快照子 XID 数组的最大尺寸。
 *
 * 必须把它导出给 snapmgr.c 使用。
 */
int
GetMaxSnapshotSubxidCount(void)
{
	return TOTAL_MAX_CACHED_SUBXIDS;
}

/*
 * 初始化新构建快照里与 old_snapshot_threshold 有关的部分。
 */
static void
GetSnapshotDataInitOldSnapshot(Snapshot snapshot)
{
	if (!OldSnapshotThresholdActive())
	{
		/*
		 * 若未使用快照过旧特性，相关字段填上
		 * 不需要任何上锁的哑值。
		 */
		snapshot->lsn = InvalidXLogRecPtr;
		snapshot->whenTaken = 0;
	}
	else
	{
		/*
		 * 记下当前时间和 WAL 流位置，以防这份
		 * 快照变得足够旧，需要退回到特殊的
		 * 旧快照逻辑。
		 */
		snapshot->lsn = GetXLogInsertRecPtr();
		snapshot->whenTaken = GetSnapshotCurrentTimestamp();
		MaintainOldSnapshotTimeMapping(snapshot->whenTaken, snapshot->xmin);
	}
}

/*
 * GetSnapshotData() 的辅助函数，检查快照里大部分
 * 可见性信息是否仍然有效。若有效，它更新
 * 需要改变的字段并返回真。否则返回
 * 假。
 *
 * 这很可能会演变成不必持有 ProcArrayLock（至少在
 * 已经持有一份快照时），但那是以后的事。
 */
static bool
GetSnapshotDataReuse(Snapshot snapshot)
{
	uint64		curXactCompletionCount;

	Assert(LWLockHeldByMe(ProcArrayLock));

	if (unlikely(snapshot->snapXactCompletionCount == 0))
		return false;

	curXactCompletionCount = ShmemVariableCache->xactCompletionCount;
	if (curXactCompletionCount != snapshot->snapXactCompletionCount)
		return false;

	/*
	 * 若当前 xactCompletionCount 仍与构建快照时相同，
	 * 就可以确定，用笨办法重建
	 * 快照内容会得到相同的快照
	 * 内容：
	 *
	 * 如 transam/README 所解释，GetSnapshotData() 视为运行中的
	 * xid 集合在持有 ProcArrayLock 期间无法改变。快照
	 * 内容只取决于带 xid 的事务，而带 xid 的事务结束时
	 * （同时排他持有 ProcArrayLock）就会递增 xactCompletionCount。
	 * 因此这项 xactCompletionCount 检查能保证我们发现
	 * 快照是否会改变。
	 *
	 * 由于快照内容与之前相同，把快照的 xmin
	 * 重新写入 PGPROC 数组是安全的。在该快照下可见的行
	 * 没有一行可能已经被删掉（那需要正在运行的事务集合
	 * 发生变化），并且它满足并发 GetSnapshotData()
	 * 调用产生相同
	 * xmin 的需要。
	 */
	if (!TransactionIdIsValid(MyProc->xmin))
		MyProc->xmin = TransactionXmin = snapshot->xmin;

	RecentXmin = snapshot->xmin;
	Assert(TransactionIdPrecedesOrEquals(TransactionXmin, RecentXmin));

	snapshot->curcid = GetCurrentCommandId(false);
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->copied = false;

	GetSnapshotDataInitOldSnapshot(snapshot);

	return true;
}

/*
 * GetSnapshotData：返回关于正在运行的事务的信息。
 *
 * 返回的快照包括 xmin（仍在运行的最低事务 ID）、
 * xmax（已完成的最高事务 ID + 1），以及范围
 * xmin <= xid < xmax 内正在运行的事务 ID 列表。用法如下：
 *		所有 < xmin 的事务 ID 视为已结束。
 *		所有 >= xmax 的事务 ID 视为仍在运行。
 *		对于 xmin <= xid < xmax 的事务 ID，查列表以判断
 *		它是否被视为运行中。
 * 这保证当前事务视为正在运行的事务集合
 * 在它取得快照之后不会改变。
 *
 * 除了惰性 VACUUM 进程之外，所有正在运行的顶层 XIDs 都包含在快照里。
 * 我们也尽量包含正在运行的子事务 XIDs，
 * 但 PGPROC 只有有限的子事务 XID 缓存区，完整
 * 信息可能不可得。若发现任何溢出的 subxid 数组，
 * 就必须把快照的 subxid 数据标为溢出，并且可能
 * 需要额外工作来判断什么在运行（见 XidInMVCCSnapshot()）。
 *
 * 我们也更新下列后端全局变量：
 *		TransactionXmin：当前事务中任何在用快照的最旧 xmin
 *			（这与 MyProc->xmin 相同）。
 *		RecentXmin：最近一份快照算出的 xmin。比这更旧的 XIDs
 *			已知不再运行。
 *
 * 并尽量推进 GlobalVis 的
 * Shared、Catalog、Data、Temp 关系边界，以利于 GlobalVisTest 系列函数。
 *
 * 注意：这个函数大概不应当用非静态分配的参数来调用
 * （见下面的 xip 分配）。
 */
Snapshot
GetSnapshotData(Snapshot snapshot)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	TransactionId xmin;
	TransactionId xmax;
	int			count = 0;
	int			subcount = 0;
	bool		suboverflowed = false;
	FullTransactionId latest_completed;
	TransactionId oldestxid;
	int			mypgxactoff;
	TransactionId myxid;
	uint64		curXactCompletionCount;

	TransactionId replication_slot_xmin = InvalidTransactionId;
	TransactionId replication_slot_catalog_xmin = InvalidTransactionId;

	Assert(snapshot != NULL);

	/*
	 * 为 maxProcs 个 xid 分配空间通常是多余的；numProcs 就
	 * 够了。但最好在不持锁时做 malloc，
	 * 所以不能去看 numProcs。同样，我们分配的
	 * subxip 存储也比大概需要的多得多。
	 *
	 * 这确实打开了避免反复 malloc/free 的可能：因为
	 * maxProcs 在运行时不变，若已有先前的
	 * xip 数组就可以直接复用。（这依赖于所有调用方传入的都是
	 * 静态的 SnapshotData 结构。）
	 */
	if (snapshot->xip == NULL)
	{
		/*
		 * 这份快照的第一次调用。无论是否
		 * 处于恢复中，快照大小都相同，见后面的注释。
		 */
		snapshot->xip = (TransactionId *)
			malloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
		if (snapshot->xip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		Assert(snapshot->subxip == NULL);
		snapshot->subxip = (TransactionId *)
			malloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
		if (snapshot->subxip == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	/*
	 * 即使要设置 MyProc->xmin，对 ProcArrayLock 取共享锁
	 * 也足够了。
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	if (GetSnapshotDataReuse(snapshot))
	{
		LWLockRelease(ProcArrayLock);
		return snapshot;
	}

	latest_completed = ShmemVariableCache->latestCompletedXid;
	mypgxactoff = MyProc->pgxactoff;
	myxid = other_xids[mypgxactoff];
	Assert(myxid == MyProc->xid);

	oldestxid = ShmemVariableCache->oldestXid;
	curXactCompletionCount = ShmemVariableCache->xactCompletionCount;

	/* xmax 始终是 latestCompletedXid + 1 */
	xmax = XidFromFullTransactionId(latest_completed);
	TransactionIdAdvance(xmax);
	Assert(TransactionIdIsNormal(xmax));

	/* 用 xmax 初始化 xmin 的计算 */
	xmin = xmax;

	/* 把自己的 xid 考虑进去，省掉循环内部的一次检查 */
	if (TransactionIdIsNormal(myxid) && NormalTransactionIdPrecedes(myxid, xmin))
		xmin = myxid;

	snapshot->takenDuringRecovery = RecoveryInProgress();

	if (!snapshot->takenDuringRecovery)
	{
		int			numProcs = arrayP->numProcs;
		TransactionId *xip = snapshot->xip;
		int		   *pgprocnos = arrayP->pgprocnos;
		XidCacheStatus *subxidStates = ProcGlobal->subxidStates;
		uint8	   *allStatusFlags = ProcGlobal->statusFlags;

		/*
		 * 先收集需要放进快照的
		 * pgxactoff/xid 集合。
		 */
		for (int pgxactoff = 0; pgxactoff < numProcs; pgxactoff++)
		{
			/* xid 只取一次，见 GetNewTransactionId */
			TransactionId xid = UINT32_ACCESS_ONCE(other_xids[pgxactoff]);
			uint8		statusFlags;

			Assert(allProcs[arrayP->pgprocnos[pgxactoff]].pgxactoff == pgxactoff);

			/*
			 * 若事务还没分配 XID，就可以跳过；它
			 * 也不会有子 XIDs。
			 */
			if (likely(xid == InvalidTransactionId))
				continue;

			/*
			 * 快照里不包含我们自己的 XIDs（若有的话）。
			 * 计算 xmin 时需要把它算进去，但我们在
			 * 循环外面已经做了。
			 */
			if (pgxactoff == mypgxactoff)
				continue;

			/*
			 * 能带着非普通 xid 走到这里的唯一途径是
			 * 引导期间，本后端使用
			 * BootstrapTransactionId。但上面的测试应当把它
			 * 滤掉。
			 */
			Assert(TransactionIdIsNormal(xid));

			/*
			 * 若 XID >= xmax，就可以跳过；这类事务反正会被
			 * 当作正在运行（并且任何子 XIDs 也会
			 * >= xmax）。
			 */
			if (!NormalTransactionIdPrecedes(xid, xmax))
				continue;

			/*
			 * 跳过另行管理 xmin 的逻辑解码后端（见下面的检查），
			 * 以及正在运行 LAZY VACUUM 的后端。
			 */
			statusFlags = allStatusFlags[pgxactoff];
			if (statusFlags & (PROC_IN_LOGICAL_DECODING | PROC_IN_VACUUM))
				continue;

			if (NormalTransactionIdPrecedes(xid, xmin))
				xmin = xid;

			/* 把 XID 加入快照。 */
			xip[count++] = xid;

			/*
			 * 若可能，保存子事务 XIDs（若已经
			 * 溢出，就没有意义了）。注意子事务 XIDs 一定
			 * 晚于其父事务，所以不必对照
			 * xmin 检查它们。可以对照 xmax 过滤，但持有 ProcArrayLock 时
			 * 最好别做那么多工作。
			 *
			 * 另一个后端可以并发地增加更多 subxid，但无法
			 * 移除任何一个。因此只取一次 nxids 很重要。
			 * 不过用 memcpy 应当是安全的。（不必担心
			 * 缺少并发加入的 xid，因为它们一定晚于
			 * xmax。）
			 *
			 * 同样，我们自己的 XIDs 不包含在快照里。
			 */
			if (!suboverflowed)
			{

				if (subxidStates[pgxactoff].overflowed)
					suboverflowed = true;
				else
				{
					int			nsubxids = subxidStates[pgxactoff].count;

					if (nsubxids > 0)
					{
						int			pgprocno = pgprocnos[pgxactoff];
						PGPROC	   *proc = &allProcs[pgprocno];

						pg_read_barrier();	/* 与 GetNewTransactionId 配对 */

						memcpy(snapshot->subxip + subcount,
							   proc->subxids.xids,
							   nsubxids * sizeof(TransactionId));
						subcount += nsubxids;
					}
				}
			}
		}
	}
	else
	{
		/*
		 * 我们处于热备，所以从 KnownAssignedXids 取 XIDs。
		 *
		 * 把所有 xid 直接存进 subxip[]。原因如下：
		 *
		 * 恢复期间我们不知道哪些 xid 是顶层、哪些是
		 * 子事务，这个设计选择大大简化了 xid 处理。
		 *
		 * 看起来我们会想只把 xid 放进 xip[]，但
		 * 那个数组相当小。要么把它加大，要么
		 * 提高 WAL 记录 xid 分配的频率；两者都
		 * 不吸引人。
		 *
		 * 可以先把 xid 存进 xip[]，过多时再存进
		 * subxip[]。这只有在快照不溢出时才行，因为那种情况下
		 * 我们不搜索 subxip[]。更简单的办法是把所有 xid
		 * 都存在 subxip 数组里，因为这是大得多的数组。
		 * xip 数组就留空。
		 *
		 * 无论哪种，都需要改变 XidInMVCCSnapshot() 的工作方式，
		 * 取决于快照何时取得，或者改变普通快照处理
		 * 使两者一致。
		 *
		 * 注意：可能在我们取完快照之前恢复就结束了，
		 * 新分配的事务 id 会被加进
		 * ProcArray。持有 ProcArrayLock 期间 xmax 无法改变，所以
		 * 那些新加的事务 id 会被滤掉，因此不必为它们
		 * 担心。
		 */
		subcount = KnownAssignedXidsGetAndSetXmin(snapshot->subxip, &xmin,
												  xmax);

		if (TransactionIdPrecedesOrEquals(xmin, procArray->lastOverflowedXid))
			suboverflowed = true;
	}


	/*
	 * 在持有 ProcArrayLock 时取到局部变量。下面的
	 * LWLockRelease 是屏障，保证这件事发生在
	 * 锁的内部。
	 */
	replication_slot_xmin = procArray->replication_slot_xmin;
	replication_slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

	if (!TransactionIdIsValid(MyProc->xmin))
		MyProc->xmin = TransactionXmin = xmin;

	LWLockRelease(ProcArrayLock);

	/* 为 GlobalVis 维护状态 */
	{
		TransactionId def_vis_xid;
		TransactionId def_vis_xid_data;
		FullTransactionId def_vis_fxid;
		FullTransactionId def_vis_fxid_data;
		FullTransactionId oldestfxid;

		/*
		 * 只有在 xid 视界无法前进时，转换 oldestXid 才安全，
		 * 也就是持有锁的时候。虽然现在已经不持锁，但所有
		 * 必要数据都是持锁时收集的。
		 */
		oldestfxid = FullXidRelativeTo(latest_completed, oldestxid);

		/* 检查是否有复制槽需要更旧的 xmin。 */
		def_vis_xid_data =
			TransactionIdOlder(xmin, replication_slot_xmin);

		/*
		 * 非共享、非系统目录表里的行，若比这个 xid 更旧，
		 * 也许可以被清理掉。
		 */
		def_vis_xid = def_vis_xid_data;

		/*
		 * 检查是否有复制槽需要更旧的系统目录
		 * xmin。
		 */
		def_vis_xid =
			TransactionIdOlder(replication_slot_catalog_xmin, def_vis_xid);

		def_vis_fxid = FullXidRelativeTo(latest_completed, def_vis_xid);
		def_vis_fxid_data = FullXidRelativeTo(latest_completed, def_vis_xid_data);

		/*
		 * 检查能否提高上界。先前的
		 * GlobalVisUpdate() 可能已经算出更激进的值，若是这样就不要
		 * 覆盖它们。
		 */
		GlobalVisSharedRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid,
								   GlobalVisSharedRels.definitely_needed);
		GlobalVisCatalogRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid,
								   GlobalVisCatalogRels.definitely_needed);
		GlobalVisDataRels.definitely_needed =
			FullTransactionIdNewer(def_vis_fxid_data,
								   GlobalVisDataRels.definitely_needed);
		/* 见 ComputeXidHorizons() 里 temp_oldest_nonremovable 的计算 */
		if (TransactionIdIsNormal(myxid))
			GlobalVisTempRels.definitely_needed =
				FullXidRelativeTo(latest_completed, myxid);
		else
		{
			GlobalVisTempRels.definitely_needed = latest_completed;
			FullTransactionIdAdvance(&GlobalVisTempRels.definitely_needed);
		}

		/*
		 * 检查是否知道可以初始化或提高下
		 * 界。目前唯一便宜的办法是用
		 * ShmemVariableCache->oldestXid 作为输入。
		 *
		 * 我们肯定能做得更好。例如可以把一个
		 * 全局下界放进 ShmemVariableCache。
		 */
		GlobalVisSharedRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisSharedRels.maybe_needed,
								   oldestfxid);
		GlobalVisCatalogRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisCatalogRels.maybe_needed,
								   oldestfxid);
		GlobalVisDataRels.maybe_needed =
			FullTransactionIdNewer(GlobalVisDataRels.maybe_needed,
								   oldestfxid);
		/* 已知精确值 */
		GlobalVisTempRels.maybe_needed = GlobalVisTempRels.definitely_needed;
	}

	RecentXmin = xmin;
	Assert(TransactionIdPrecedesOrEquals(TransactionXmin, RecentXmin));

	snapshot->xmin = xmin;
	snapshot->xmax = xmax;
	snapshot->xcnt = count;
	snapshot->subxcnt = subcount;
	snapshot->suboverflowed = suboverflowed;
	snapshot->snapXactCompletionCount = curXactCompletionCount;

	snapshot->curcid = GetCurrentCommandId(false);

	/*
	 * 这是一份新快照，所以两个引用计数都设为零，并把它标为
	 * 没有复制到持久内存中。
	 */
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->copied = false;

	GetSnapshotDataInitOldSnapshot(snapshot);

	return snapshot;
}

/*
 * ProcArrayInstallImportedXmin：把导入的 xmin 装进 MyProc->xmin。
 *
 * 在安装从另一个事务导入的快照时调用。为确保 OldestXmin
 * 不向后走，必须检查源事务仍在运行，而且最好与安装
 * 新 xmin 原子地
 * 完成。
 *
 * 成功返回真，若源事务不再运行则返回假。
 */
bool
ProcArrayInstallImportedXmin(TransactionId xmin,
							 VirtualTransactionId *sourcevxid)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	Assert(TransactionIdIsNormal(xmin));
	if (!sourcevxid)
		return false;

	/* 取得锁，使源事务不能在我们做这件事时结束 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		int			statusFlags = ProcGlobal->statusFlags[index];
		TransactionId xid;

		/* 忽略正在运行 LAZY VACUUM 的 proc */
		if (statusFlags & PROC_IN_VACUUM)
			continue;

		/* 我们只关心这个特定的虚拟事务。 */
		if (proc->backendId != sourcevxid->backendId)
			continue;
		if (proc->lxid != sourcevxid->localTransactionId)
			continue;

		/*
		 * 出于谨慎，检查该事务的数据库 ID：若它
		 * 在另一个数据库，它的 xmin 就不覆盖我们。调用方应当已经
		 * 发现了这一点，所以任何异常情况都当作
		 * 找不到事务。
		 */
		if (proc->databaseId != MyDatabaseId)
			continue;

		/*
		 * 同样，确认它的 xmin 确实覆盖我们。
		 */
		xid = UINT32_ACCESS_ONCE(proc->xmin);
		if (!TransactionIdIsNormal(xid) ||
			!TransactionIdPrecedesOrEquals(xid, xmin))
			continue;

		/*
		 * 没问题。装上新的 xmin。与 GetSnapshotData 中一样，也设置
		 * TransactionXmin。（注意因为 snapmgr.c 先调用了
		 * GetSnapshotData，这里会覆盖一个有效的 xmin，所以
		 * 我们不检查这一点。）
		 */
		MyProc->xmin = TransactionXmin = xmin;

		result = true;
		break;
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * ProcArrayInstallRestoredXmin：把恢复来的 xmin 装进 MyProc->xmin。
 *
 * 这与 ProcArrayInstallImportedXmin 类似，但我们有一个指向
 * 导入快照的那个事务的 PGPROC 的指针，而不是
 * 一个 XID。
 *
 * 注意这个函数也会从源 proc 复制 statusFlags，
 * 以免出现计算 xid 视界时需要跳过
 * MyProc 的 xmin 的情况。
 *
 * 成功返回真，若源事务不再运行则返回假。
 */
bool
ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc)
{
	bool		result = false;
	TransactionId xid;

	Assert(TransactionIdIsNormal(xmin));
	Assert(proc != NULL);

	/*
	 * 取排他锁，以便从源 proc 复制 statusFlags。
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * 要确定所引用的 PGPROC 对外公布的 xmin 不晚于
	 * 我们正在安装的那个，这样系统范围的 xmin
	 * 就不会向后走。另外，确保它在同一个数据库里运行，
	 * 这样按数据库的 xmin 无法向后走。
	 */
	xid = UINT32_ACCESS_ONCE(proc->xmin);
	if (proc->databaseId == MyDatabaseId &&
		TransactionIdIsNormal(xid) &&
		TransactionIdPrecedesOrEquals(xid, xmin))
	{
		/*
		 * 装上 xmin，并传播影响清理如何解释
		 * 该值的 statusFlags。
		 */
		MyProc->xmin = TransactionXmin = xmin;
		MyProc->statusFlags = (MyProc->statusFlags & ~PROC_XMIN_FLAGS) |
			(proc->statusFlags & PROC_XMIN_FLAGS);
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

		result = true;
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * GetRunningTransactionData：返回关于正在运行的事务的信息。
 *
 * 与 GetSnapshotData 类似，但返回更多信息。我们包括
 * 所有已分配 TransactionId 的 PGPROCs，甚至包括 VACUUM 进程和
 * 已准备事务。
 *
 * 我们取得 XidGenLock 和 ProcArrayLock，但调用方负责
 * 释放它们。取得 XidGenLock 保证在调用方把这份快照写入 WAL
 * 并释放锁之前，不会有新的 XIDs 进入 proc
 * 数组。取得 ProcArrayLock 保证在
 * 锁被释放之前没有事务提交。
 *
 * 返回的数据结构是静态分配的；调用方不应
 * 修改它，并且不得假定它在下一次调用之后仍然有效。
 *
 * 这在恢复期间从不执行，所以不必去看
 * KnownAssignedXids。
 *
 * 来自已准备事务的哑 PGPROCs 也包括在内，这意味着这里
 * 可能返回来自正在完成准备的事务的、TransactionId 值重复的项。
 * 这里不对重复项做任何处理，以免 ProcArrayLock 持有得
 * 比必要更久。
 *
 * 我们不操心更新其他计数器，希望尽量
 * 保持简单，把那部分记账留给 GetSnapshotData()
 * 作为主代码。
 *
 * 注意若任何事务的缓存子事务溢出了，
 * 就实在没有必要包含任何子事务。
 */
RunningTransactions
GetRunningTransactionData(void)
{
	/* 结果工作区 */
	static RunningTransactionsData CurrentRunningXactsData;

	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	RunningTransactions CurrentRunningXacts = &CurrentRunningXactsData;
	TransactionId latestCompletedXid;
	TransactionId oldestRunningXid;
	TransactionId *xids;
	int			index;
	int			count;
	int			subcount;
	bool		suboverflowed;

	Assert(!RecoveryInProgress());

	/*
	 * 为 maxProcs 个 xid 分配空间通常是多余的；numProcs 就
	 * 够了。但最好在不持锁时做 malloc，
	 * 所以不能去看 numProcs。同样，我们分配的
	 * subxip 存储也比大概需要的多得多。
	 *
	 * 应当只在 bgwriter 里分配，因为只在
	 * 检查点期间执行。
	 */
	if (CurrentRunningXacts->xids == NULL)
	{
		/*
		 * 第一次调用
		 */
		CurrentRunningXacts->xids = (TransactionId *)
			malloc(TOTAL_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
		if (CurrentRunningXacts->xids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	xids = CurrentRunningXacts->xids;

	count = subcount = 0;
	suboverflowed = false;

	/*
	 * 取得快照时，确保没有 xid 进入或离开
	 * procarray。
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	LWLockAcquire(XidGenLock, LW_SHARED);

	latestCompletedXid =
		XidFromFullTransactionId(ShmemVariableCache->latestCompletedXid);
	oldestRunningXid =
		XidFromFullTransactionId(ShmemVariableCache->nextXid);

	/*
	 * 遍历 procArray，收集所有 xid
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		TransactionId xid;

		/* xid 只取一次，见 GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);

		/*
		 * 还没有 TransactionId 的事务不必存储，因为它们在
		 * 备库服务器上不会显示为正在运行。
		 */
		if (!TransactionIdIsValid(xid))
			continue;

		/*
		 * 在计算 oldestRunningXid 和 suboverflowed 的值之前，小心不要排除任何 xid，
		 * 因为这些值用来清理
		 * 备库上持有的事务信息。
		 */
		if (TransactionIdPrecedes(xid, oldestRunningXid))
			oldestRunningXid = xid;

		if (ProcGlobal->subxidStates[index].overflowed)
			suboverflowed = true;

		/*
		 * 若想排除某些 xid，这里就是合适的地方。
		 * 设了 PROC_IN_VACUUM 标志的 proc 通常不分配 xid，
		 * 但它们在末尾截断时会取得锁并
		 * 截断，所以若看见它们就包含进来也没什么大问题，
		 * 包含进来更干净。
		 */

		xids[count++] = xid;
	}

	/*
	 * 遍历 procArray 收集所有 subxid，但仅当还没有
	 * 发生子事务溢出时。
	 */
	if (!suboverflowed)
	{
		XidCacheStatus *other_subxidstates = ProcGlobal->subxidStates;

		for (index = 0; index < arrayP->numProcs; index++)
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];
			int			nsubxids;

			/*
			 * 保存子事务 XIDs。持有 XidGenLock 期间，其他后端不能增加或移除
			 * 项。
			 */
			nsubxids = other_subxidstates[index].count;
			if (nsubxids > 0)
			{
				/* 屏障其实并不需要，因为持有 XidGenLock，但是... */
				pg_read_barrier();	/* 与 GetNewTransactionId 配对 */

				memcpy(&xids[count], proc->subxids.xids,
					   nsubxids * sizeof(TransactionId));
				count += nsubxids;
				subcount += nsubxids;

				/*
				 * 事务的顶层 XID 总是小于它的任何
				 * subxid，所以不必检查是否有
				 * subxid 小于 oldestRunningXid
				 */
			}
		}
	}

	/*
	 * 重要的是不要把槽所设的限制包含进来，因为
	 * snapbuild.c 用 oldestRunningXid 来管理它的 xmin 视界。若把那些
	 * 包含进来，初值就永远无法增加，因为
	 * 存在循环依赖：槽只在
	 * 正在运行的事务提高 oldestRunningXid 时才提高自己的限制，而正在运行的事务只在
	 * 槽提高时才提高。
	 */

	CurrentRunningXacts->xcnt = count - subcount;
	CurrentRunningXacts->subxcnt = subcount;
	CurrentRunningXacts->subxid_status = suboverflowed ? SUBXIDS_IN_SUBTRANS : SUBXIDS_IN_ARRAY;
	CurrentRunningXacts->nextXid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	CurrentRunningXacts->oldestRunningXid = oldestRunningXid;
	CurrentRunningXacts->latestCompletedXid = latestCompletedXid;

	Assert(TransactionIdIsValid(CurrentRunningXacts->nextXid));
	Assert(TransactionIdIsValid(CurrentRunningXacts->oldestRunningXid));
	Assert(TransactionIdIsNormal(CurrentRunningXacts->latestCompletedXid));

	/* 这里不释放锁，调用方负责 */

	return CurrentRunningXacts;
}

/*
 * GetOldestActiveTransactionId()。
 *
 * 与 GetSnapshotData 类似，但只返回 oldestActiveXid。我们包括
 * 所有已分配 TransactionId 的 PGPROCs，甚至包括 VACUUM 进程。
 * 我们查看所有数据库，不过不必包括 WALSender，
 * 因为这对热备冲突没有影响。
 *
 * 这在恢复期间从不执行，所以不必去看
 * KnownAssignedXids。
 *
 * 我们不操心更新其他计数器，希望尽量
 * 保持简单，把那部分记账留给 GetSnapshotData()
 * 作为主代码。
 */
TransactionId
GetOldestActiveTransactionId(void)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	TransactionId oldestRunningXid;
	int			index;

	Assert(!RecoveryInProgress());

	/*
	 * 读取 nextXid，作为仍活动内容的上界。
	 *
	 * 读一个 TransactionId 是原子的，但必须拿锁，以确保
	 * 在我们遍历时，所有 < nextXid 的 XIDs 都已经在 proc 数组里
	 * （或已经完成）。
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	oldestRunningXid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	LWLockRelease(XidGenLock);

	/*
	 * 遍历 procArray，收集所有 xid 和 subxid。
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (index = 0; index < arrayP->numProcs; index++)
	{
		TransactionId xid;

		/* xid 只取一次，见 GetNewTransactionId */
		xid = UINT32_ACCESS_ONCE(other_xids[index]);

		if (!TransactionIdIsNormal(xid))
			continue;

		if (TransactionIdPrecedes(xid, oldestRunningXid))
			oldestRunningXid = xid;

		/*
		 * 事务的顶层 XID 总是小于它的任何
		 * subxid，所以不必检查是否有 subxid
		 * 小于 oldestRunningXid
		 */
	}
	LWLockRelease(ProcArrayLock);

	return oldestRunningXid;
}

/*
 * GetOldestSafeDecodingTransactionId：不受清理影响的最低 xid。
 *
 * 返回我们可以保证没有受清理影响的最旧 xid，
 * 也就是说，除非事务中止，否则不会有 >= 该 xid 的行被清理掉。
 * 注意这个值可以（而且大多数时候会）比真正受清理影响的范围
 * 保守得多，但我们
 * 目前没有更好的数据可用。
 *
 * 这对初始化截止 xid 有用，新的变更集
 * 抽取复制槽可以在此之后开始解码变更。
 *
 * 调用时必须持有共享或排他的 ProcArrayLock，
 * 不过大多数调用方会想用排他模式，因为预期
 * 调用方会立刻用这个 xid 来钉住 xmin 视界。
 */
TransactionId
GetOldestSafeDecodingTransactionId(bool catalogOnly)
{
	ProcArrayStruct *arrayP = procArray;
	TransactionId oldestSafeXid;
	int			index;
	bool		recovery_in_progress = RecoveryInProgress();

	Assert(LWLockHeldByMe(ProcArrayLock));

	/*
	 * 取得 XidGenLock，这样在我们运行期间没有事务能取得 xid。
	 * 若没有带 xid 的事务并发运行，一个新 xid
	 * 可能影响 RecentXmin 等等。
	 *
	 * 把计算初始化为 nextXid，因为那保证是一个
	 * 安全、尽管偏悲观的值。
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	oldestSafeXid = XidFromFullTransactionId(ShmemVariableCache->nextXid);

	/*
	 * 若已经有槽钉住 xmin 视界，就可以从
	 * 那个值开始，它保证是安全的，因为它最初由本
	 * 例程算出，此后一直被强制执行。总是可以用
	 * 槽的一般 xmin 视界，但系统目录视界只在
	 * 只会查看系统目录数据时才可用。
	 */
	if (TransactionIdIsValid(procArray->replication_slot_xmin) &&
		TransactionIdPrecedes(procArray->replication_slot_xmin,
							  oldestSafeXid))
		oldestSafeXid = procArray->replication_slot_xmin;

	if (catalogOnly &&
		TransactionIdIsValid(procArray->replication_slot_catalog_xmin) &&
		TransactionIdPrecedes(procArray->replication_slot_catalog_xmin,
							  oldestSafeXid))
		oldestSafeXid = procArray->replication_slot_catalog_xmin;

	/*
	 * 若不在恢复中，就遍历 procarray 并收集
	 * 最低的 xid。因为调用时持有 ProcArrayLock，并且已经
	 * 取得 XidGenLock，项不会并发消失，因为
	 * ProcGlobal->xids[i] 只在持有 XidGenLock 时设置，只在持有
	 * ProcArrayLock 时清除。
	 *
	 * 恢复中我们不能把安全值降到上面算出的结果以下，
	 * 所以那里得再等一会儿。可惜不能
	 * 使用 KnownAssignedXidsGetOldestXmin()，因为 KnownAssignedXids
	 * 机制可能漏掉一些值，返回比安全值更旧的值。
	 */
	if (!recovery_in_progress)
	{
		TransactionId *other_xids = ProcGlobal->xids;

		/*
		 * 遍历 procArray，收集 min(ProcGlobal->xids[i])
		 */
		for (index = 0; index < arrayP->numProcs; index++)
		{
			TransactionId xid;

			/* xid 只取一次，见 GetNewTransactionId */
			xid = UINT32_ACCESS_ONCE(other_xids[index]);

			if (!TransactionIdIsNormal(xid))
				continue;

			if (TransactionIdPrecedes(xid, oldestSafeXid))
				oldestSafeXid = xid;
		}
	}

	LWLockRelease(XidGenLock);

	return oldestSafeXid;
}

/*
 * GetVirtualXIDsDelayingChkpt：取得因有关键动作在进行
 * 而推迟检查点的事务的 VXIDs。
 *
 * 构造当前处于提交
 * 临界区的事务的 VXIDs 数组，表现为它们的 PGPROC 里
 * 设置了指定的 delayChkptFlags 位。
 *
 * 返回一个应由调用方释放的 palloc 数组。
 * *nvxids 是有效项的数目。
 *
 * 注意，因为后端设置或清除 delayChkptFlags 时不持任何
 * 锁，结果多少是不确定的，但我们并不真的在意。即使
 * 在写共享内存有延迟的多处理器上，也可以
 * 确定：后端拿锁时，delayChkptFlags 的设置会传播到共享内存，
 * 所以若它已经插入了提交记录，我们就无法看不到这个虚拟事务的
 * delayChkptFlags。delayChkptFlags 的清除要过一会儿才传播
 * 对正确性并不重要。
 * 因此这里的结果够用。
 */
VirtualTransactionId *
GetVirtualXIDsDelayingChkpt(int *nvxids, int type)
{
	VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	Assert(type != 0);

	/* 分配肯定够用的结果空间 */
	vxids = (VirtualTransactionId *)
		palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if ((proc->delayChkptFlags & type) != 0)
		{
			VirtualTransactionId vxid;

			GET_VXID_FROM_PGPROC(vxid, *proc);
			if (VirtualTransactionIdIsValid(vxid))
				vxids[count++] = vxid;
		}
	}

	LWLockRelease(ProcArrayLock);

	*nvxids = count;
	return vxids;
}

/*
 * HaveVirtualXIDsDelayingChkpt：指定的 VXIDs 里是否还有在推迟的？
 *
 * 这与 GetVirtualXIDsDelayingChkpt 的结果一起用，看指定的
 * VXIDs 是否仍在代码的临界区里。
 *
 * 注意：按正在或曾经推迟的 vxact 数目，这是 O(N^2)，但
 * 这些数目应当小到不成问题。
 */
bool
HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids, int nvxids, int type)
{
	bool		result = false;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	Assert(type != 0);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		VirtualTransactionId vxid;

		GET_VXID_FROM_PGPROC(vxid, *proc);

		if ((proc->delayChkptFlags & type) != 0 &&
			VirtualTransactionIdIsValid(vxid))
		{
			int			i;

			for (i = 0; i < nvxids; i++)
			{
				if (VirtualTransactionIdEquals(vxid, vxids[i]))
				{
					result = true;
					break;
				}
			}
			if (result)
				break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * BackendPidGetProc：根据 PID 取得后端的 PGPROC。
 *
 * 找不到则返回 NULL。注意，要由调用方确保
 * 这个问题在答案被使用的足够长时间内
 * 仍然有意义...
 */
PGPROC *
BackendPidGetProc(int pid)
{
	PGPROC	   *result;

	if (pid == 0)				/* 永不匹配哑 PGPROCs */
		return NULL;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	result = BackendPidGetProcWithLock(pid);

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * BackendPidGetProcWithLock：根据 PID 取得后端的 PGPROC。
 *
 * 与上面相同，只是调用方必须持有 ProcArrayLock。找到的
 * 项（若有）在锁一直持有期间可以认为有效。
 */
PGPROC *
BackendPidGetProcWithLock(int pid)
{
	PGPROC	   *result = NULL;
	ProcArrayStruct *arrayP = procArray;
	int			index;

	if (pid == 0)				/* 永不匹配哑 PGPROCs */
		return NULL;

	for (index = 0; index < arrayP->numProcs; index++)
	{
		PGPROC	   *proc = &allProcs[arrayP->pgprocnos[index]];

		if (proc->pid == pid)
		{
			result = proc;
			break;
		}
	}

	return result;
}

/*
 * BackendXidGetPid：根据 XID 取得后端的 pid。
 *
 * 找不到或它是已准备事务时返回 0。注意
 * 要由调用方确保这个问题在
 * 答案被使用的足够长时间内仍然有意义。
 *
 * 只考虑主事务 Id。这个函数主要
 * 用来判断哪一个后端拥有一把锁。
 *
 * 注意并非每个事务都分配了 XID。不过，只要你
 * 只用在磁盘上找到的 XID 来调用，就是安全的。
 */
int
BackendXidGetPid(TransactionId xid)
{
	int			result = 0;
	ProcArrayStruct *arrayP = procArray;
	TransactionId *other_xids = ProcGlobal->xids;
	int			index;

	if (xid == InvalidTransactionId)	/* 永不匹配无效 xid */
		return 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (other_xids[index] == xid)
		{
			result = proc->pid;
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return result;
}

/*
 * IsBackendPid：给定 pid 是否是正在运行的后端。
 *
 * 后端自己不调用它，外部模块会调用。
 */
bool
IsBackendPid(int pid)
{
	return (BackendPidGetProc(pid) != NULL);
}


/*
 * GetCurrentVirtualXIDs：返回当前活动 VXIDs 的数组。
 *
 * 数组用 palloc 分配。有效项数目返回到 *nvxids。
 *
 * 参数用来过滤返回的 VXIDs 集合。我们自己的进程
 * 总是跳过。此外：
 *	若 limitXmin 不是 InvalidTransactionId，就跳过
 *		xmin > limitXmin 的进程。
 *	若 excludeXmin0 为真，就跳过 xmin = 0 的进程。
 *	若 allDbs 为假，就跳过挂到其他数据库的进程。
 *	若 excludeVacuum 非零，就跳过
 *		(statusFlags & excludeVacuum) 非零的进程。
 *
 * 注意：limitXmin 和 excludeXmin0 参数的目的是
 * 允许跳过那些最旧存活快照不比
 * 我们已有的某份快照更旧的后端。因为我们只持共享
 * 锁来检查 procarray，存在竞争：后端可能在我们
 * 查看之后才设置 xmin。的确，在内存序较弱的多处理器上，
 * 另一个后端可能在我们查看之前就已经设置了 xmin。不过我们知道
 * 这样的后端必须持有与我们自己持有的 ProcArrayLock 重叠的共享 ProcArrayLock，
 * 否则我们会看到它的 xmin 更新。因此，
 * 另一个后端与我们的扫描并发取得的任何快照，都无法把
 * 我们认为已提交的事务仍视为运行中
 * （因为后端提交时必须排他持有 ProcArrayLock）。
 */
VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool excludeXmin0,
					  bool allDbs, int excludeVacuum,
					  int *nvxids)
{
	VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/* 分配肯定够用的结果空间 */
	vxids = (VirtualTransactionId *)
		palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		uint8		statusFlags = ProcGlobal->statusFlags[index];

		if (proc == MyProc)
			continue;

		if (excludeVacuum & statusFlags)
			continue;

		if (allDbs || proc->databaseId == MyDatabaseId)
		{
			/* xmin 只取一次，它可能在我们脚下改变 */
			TransactionId pxmin = UINT32_ACCESS_ONCE(proc->xmin);

			if (excludeXmin0 && !TransactionIdIsValid(pxmin))
				continue;

			/*
			 * InvalidTransactionId 先于所有其他 XIDs，所以还没
			 * 设置 xmin 的 proc 不会被这项测试拒绝。
			 */
			if (!TransactionIdIsValid(limitXmin) ||
				TransactionIdPrecedesOrEquals(pxmin, limitXmin))
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);
				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	*nvxids = count;
	return vxids;
}

/*
 * GetConflictingVirtualXIDs：返回当前活动 VXIDs 的数组。
 *
 * 用途限于备库服务器上恢复期间的冲突解决。
 * limitXmin 要么作为带 snapshotConflictHorizon
 * 语义的截止值提供，要么在调用方无法准确
 * 确定安全的 snapshotConflictHorizon 值时提供 InvalidTransactionId。
 *
 * 若 limitXmin 是 InvalidTransactionId，我们就想杀掉所有人，
 * 所以不在乎他们有没有快照，持有哪种
 * 锁也其实不重要。调用方必须避免用原始执行期间
 * 被设成 InvalidTransactionId 的 snapshotConflictHorizon 风格截止值
 * 来调用这里，因为那实际上表示肯定不需要恢复冲突
 * （InvalidTransactionId 值的 snapshotConflictHorizon 约定
 * 与我们自己的相反！）。
 *
 * 现在所有检查 xmin 的调用方总会提供一个有效且有用的
 * limitXmin 值。limitXmin 总是低于尚未成为 FATAL 错误的
 * 编号最低的 KnownAssignedXid。这是
 * 因为我们只关心清理已提交事务的元组
 * 版本的清理条目。那种情况下它们只会
 * 出现在该条小于最低运行 xid 的位置。这
 * 允许我们说，若任何后端与我们并发取快照，
 * 这里做出的冲突评估绝不会包含正在
 * 导出的那份快照。所以我们对 ProcArray 取 LW_SHARED，并在
 * limitXmin 有效时允许并发快照。我们也许会想加上
 *	 Assert(limitXmin < lowest(KnownAssignedXids))
 * 但在数组里有滞后的 FATAL 错误时那并不为真，
 * 而我们已经知道那些是假的，所以跳过该项测试。
 *
 * 若 dbOid 有效，就跳过挂到其他数据库的后端。
 *
 * 小心不要 pfree 这个函数的结果。我们足够频繁地复用
 * 这个数组，所以结果用 malloc。
 */
VirtualTransactionId *
GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid)
{
	static VirtualTransactionId *vxids;
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/*
	 * 若是第一次经过，就取得工作区来记住主 XIDs。我们
	 * 把它永久 malloc 出来，以免反复 palloc/pfree 的开销。留出
	 * 结果空间，记住要给终止符留位置。
	 */
	if (vxids == NULL)
	{
		vxids = (VirtualTransactionId *)
			malloc(sizeof(VirtualTransactionId) * (arrayP->maxProcs + 1));
		if (vxids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		/* 排除已准备事务 */
		if (proc->pid == 0)
			continue;

		if (!OidIsValid(dbOid) ||
			proc->databaseId == dbOid)
		{
			/* xmin 只取一次；不会在我们脚下改变，但这是好的写法 */
			TransactionId pxmin = UINT32_ACCESS_ONCE(proc->xmin);

			/*
			 * 我们忽略无效的 pxmin，因为这意味着该后端
			 * 当前没有快照。我们持有共享锁，以免与
			 * 正在取快照的用户争用。这不是问题，因为
			 * 当前 xmin 总是至少比最近
			 * 删除的 xid 高一，所以任何新快照都不会与
			 * 这里的测试冲突。
			 */
			if (!TransactionIdIsValid(limitXmin) ||
				(TransactionIdIsValid(pxmin) && !TransactionIdFollows(pxmin, limitXmin)))
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);
				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
			}
		}
	}

	LWLockRelease(ProcArrayLock);

	/* 加上终止符 */
	vxids[count].backendId = InvalidBackendId;
	vxids[count].localTransactionId = InvalidLocalTransactionId;

	return vxids;
}

/*
 * CancelVirtualTransaction：用于恢复冲突处理。
 *
 * 返回被发信号的进程的 pid，找不到则返回 0。
 */
pid_t
CancelVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode)
{
	return SignalVirtualTransaction(vxid, sigmode, true);
}

pid_t
SignalVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode,
						 bool conflictPending)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;
	pid_t		pid = 0;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];
		VirtualTransactionId procvxid;

		GET_VXID_FROM_PGPROC(procvxid, *proc);

		if (procvxid.backendId == vxid.backendId &&
			procvxid.localTransactionId == vxid.localTransactionId)
		{
			proc->recoveryConflictPending = conflictPending;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * 若 pid 还在就杀掉它。若不在，那正是我们
				 * 想要的，所以忽略任何错误。
				 */
				(void) SendProcSignal(pid, sigmode, vxid.backendId);
			}
			break;
		}
	}

	LWLockRelease(ProcArrayLock);

	return pid;
}

/*
 * MinimumActiveBackends：统计除我之外处于
 *		活动事务中的后端。若计数超过传入的
 *		最小阈值就返回真。这用作启发式，用来决定提交期间
 *		在 XLOG 刷出之前的延迟是否值得。
 *
 * 不要统计被锁阻塞而等待的后端，因为它们
 * 要等别人提交才会运行。
 */
bool
MinimumActiveBackends(int min)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	/* 若未指定最小值，就快速短路 */
	if (min == 0)
		return true;

	/*
	 * 注意：为了速度，我们不取得 ProcArrayLock。这有一点
	 * 不严谨，但因为我们只测试字段是零还是非零，
	 * 应当可以。结果反正只用于启发式...
	 */
	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		/*
		 * 因为不持锁，必须准备对付
		 * 垃圾，因为有人可能已经增加了 numProcs 但还没
		 * 填好结构。
		 *
		 * 若有人刚减少了 numProcs，proc 也可能指向一个
		 * 已不在数组里的 PGPROC 项。它仍然指向一个
		 * PGPROC 结构，因为被释放的 PGPROC 项只是进到
		 * 空闲列表并被回收。那种情况下它的内容是无意义的，
		 * 但对这个函数可以接受。
		 */
		if (pgprocno == -1)
			continue;			/* 不要统计已删除的项 */
		if (proc == MyProc)
			continue;			/* 不要统计我自己 */
		if (proc->xid == InvalidTransactionId)
			continue;			/* 若未分配 XID 就不要统计 */
		if (proc->pid == 0)
			continue;			/* 不要统计已准备事务 */
		if (proc->waitLock != NULL)
			continue;			/* 若被锁阻塞就不要统计 */
		count++;
		if (count >= min)
			break;
	}

	return count >= min;
}

/*
 * CountDBBackends：统计正在使用指定数据库的后端
 */
int
CountDBBackends(Oid databaseid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* 不要统计已准备事务 */
		if (!OidIsValid(databaseid) ||
			proc->databaseId == databaseid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CountDBConnections：统计数据库后端（仅普通后端）
 */
int
CountDBConnections(Oid databaseid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* 不要统计已准备事务 */
		if (proc->isBackgroundWorker)
			continue;			/* 不要统计后台工作进程 */
		if (!OidIsValid(databaseid) ||
			proc->databaseId == databaseid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CancelDBBackends：取消正在使用指定数据库的后端
 */
void
CancelDBBackends(Oid databaseid, ProcSignalReason sigmode, bool conflictPending)
{
	ProcArrayStruct *arrayP = procArray;
	int			index;

	/* 通知所有后端退出 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (databaseid == InvalidOid || proc->databaseId == databaseid)
		{
			VirtualTransactionId procvxid;
			pid_t		pid;

			GET_VXID_FROM_PGPROC(procvxid, *proc);

			proc->recoveryConflictPending = conflictPending;
			pid = proc->pid;
			if (pid != 0)
			{
				/*
				 * 若 pid 还在就杀掉它。若不在，那正是我们
				 * 想要的，所以忽略任何错误。
				 */
				(void) SendProcSignal(pid, sigmode, procvxid.backendId);
			}
		}
	}

	LWLockRelease(ProcArrayLock);
}

/*
 * CountUserBackends：统计指定用户正在使用的后端
 * （仅普通后端，不包括任何类型的后台工作进程）
 */
int
CountUserBackends(Oid roleid)
{
	ProcArrayStruct *arrayP = procArray;
	int			count = 0;
	int			index;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < arrayP->numProcs; index++)
	{
		int			pgprocno = arrayP->pgprocnos[index];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->pid == 0)
			continue;			/* 不要统计已准备事务 */
		if (proc->isBackgroundWorker)
			continue;			/* 不要统计后台工作进程 */
		if (proc->roleId == roleid)
			count++;
	}

	LWLockRelease(ProcArrayLock);

	return count;
}

/*
 * CountOtherDBBackends：检查给定数据库里是否还有其他后端在运行。
 *
 * 若该数据库里还有其他后端，我们最多等 5 秒
 * 让它们退出。自动清理后端会被鼓励提早退出，办法是
 * 向它们发送 SIGTERM，但普通用户后端只是等待。
 *
 * 当前后端总是被忽略；若这一点重要，由调用方负责
 * 检查当前后端是否使用给定数据库。
 *
 * 若该数据库里（仍）有其他后端则返回真，否则返回假。
 * 另外，*nbackends 和 *nprepared 分别设为该数据库里其他后端
 * 和已准备事务的数目。
 *
 * 这个函数用来使 DROP DATABASE 及相关命令与目标数据库里
 * 仍有活动后端互锁。在活动后端还在时删除
 * 数据库会是坏事。注意我们在这里无法
 * 发现一个新启动的后端正试图
 * 连到这个注定要删除的数据库，所以后端启动期间还需要额外的互锁。
 * 调用方通常应当在调用前对目标数据库持有排他锁，
 * 这也是我们不得无限等待的
 * 原因之一。
 */
bool
CountOtherDBBackends(Oid databaseId, int *nbackends, int *nprepared)
{
	ProcArrayStruct *arrayP = procArray;

#define MAXAUTOVACPIDS	10		/* 每次迭代最多向多少个自动清理发送 SIGTERM */
	int			autovac_pids[MAXAUTOVACPIDS];
	int			tries;

	/* 50 次尝试，每次之间睡眠 100ms，总共等待 5 秒 */
	for (tries = 0; tries < 50; tries++)
	{
		int			nautovacs = 0;
		bool		found = false;
		int			index;

		CHECK_FOR_INTERRUPTS();

		*nbackends = *nprepared = 0;

		LWLockAcquire(ProcArrayLock, LW_SHARED);

		for (index = 0; index < arrayP->numProcs; index++)
		{
			int			pgprocno = arrayP->pgprocnos[index];
			PGPROC	   *proc = &allProcs[pgprocno];
			uint8		statusFlags = ProcGlobal->statusFlags[index];

			if (proc->databaseId != databaseId)
				continue;
			if (proc == MyProc)
				continue;

			found = true;

			if (proc->pid == 0)
				(*nprepared)++;
			else
			{
				(*nbackends)++;
				if ((statusFlags & PROC_IS_AUTOVACUUM) &&
					nautovacs < MAXAUTOVACPIDS)
					autovac_pids[nautovacs++] = proc->pid;
			}
		}

		LWLockRelease(ProcArrayLock);

		if (!found)
			return false;		/* 没有冲突的后端，所以完成 */

		/*
		 * 睡眠前向任何冲突的自动清理发送 SIGTERM。我们
		 * 把这一步推迟到循环之后，因为不想在
		 * 持有 ProcArrayLock 时发出 kill()。我们不知道内核里
		 * 什么可能阻塞 kill()...
		 */
		for (index = 0; index < nautovacs; index++)
			(void) kill(autovac_pids[index], SIGTERM);	/* 忽略任何错误 */

		/* 睡眠，然后重试 */
		pg_usleep(100 * 1000L); /* 100 毫秒 */
	}

	return true;				/* 超时了，仍然冲突 */
}

/*
 * 终止到指定数据库的现有连接。当用户要求强制
 * 删除数据库时，DROP DATABASE 命令使用这个例程
 * 来做这件事。
 *
 * 当前后端总是被忽略；若这一点重要，由调用方负责
 * 检查当前后端是否使用给定数据库。
 *
 * 若目标数据库有已准备事务，或某个连接的权限检查
 * 失败，则什么都不终止就失败。
 */
void
TerminateOtherDBBackends(Oid databaseId)
{
	ProcArrayStruct *arrayP = procArray;
	List	   *pids = NIL;
	int			nprepared = 0;
	int			i;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (i = 0; i < procArray->numProcs; i++)
	{
		int			pgprocno = arrayP->pgprocnos[i];
		PGPROC	   *proc = &allProcs[pgprocno];

		if (proc->databaseId != databaseId)
			continue;
		if (proc == MyProc)
			continue;

		if (proc->pid != 0)
			pids = lappend_int(pids, proc->pid);
		else
			nprepared++;
	}

	LWLockRelease(ProcArrayLock);

	if (nprepared > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being used by prepared transactions",
						get_database_name(databaseId)),
				 errdetail_plural("There is %d prepared transaction using the database.",
								  "There are %d prepared transactions using the database.",
								  nprepared,
								  nprepared)));

	if (pids)
	{
		ListCell   *lc;

		/*
		 * 权限检查在两个方面放宽 pg_terminate_backend 的检查，
		 * 都是省掉 !OidIsValid(proc->roleId) 这项检查：
		 *
		 * 接受终止自动清理工作进程，因为不带 FORCE 的 DROP DATABASE
		 * 也会终止它们。
		 *
		 * 接受终止后台工作进程。对后台工作进程的作者来说，
		 * 若工作进程意外阻塞了 DROP DATABASE，能建议使用 FORCE
		 * 会很方便。
		 *
		 * 与 pg_terminate_backend 不同，我们不发出某些警告，例如
		 * PID 不是 PostgreSQL 服务器进程，因为对我们来说已经
		 * 结束的会话不是问题。
		 */
		foreach(lc, pids)
		{
			int			pid = lfirst_int(lc);
			PGPROC	   *proc = BackendPidGetProc(pid);

			if (proc != NULL)
			{
				if (superuser_arg(proc->roleId) && !superuser())
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to terminate process"),
							 errdetail("Only roles with the %s attribute may terminate processes of roles with the %s attribute.",
									   "SUPERUSER", "SUPERUSER")));

				if (!has_privs_of_role(GetUserId(), proc->roleId) &&
					!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to terminate process"),
							 errdetail("Only roles with privileges of the role whose process is being terminated or with privileges of the \"%s\" role may terminate this process.",
									   "pg_signal_backend")));
			}
		}

		/*
		 * 这里有一个竞争：一旦释放 ProcArrayLock，
		 * 会话可能在我们发出 kill 之前就退出。这种
		 * 竞争的可能性似乎小到不必担心。见
		 * pg_signal_backend。
		 */
		foreach(lc, pids)
		{
			int			pid = lfirst_int(lc);
			PGPROC	   *proc = BackendPidGetProc(pid);

			if (proc != NULL)
			{
				/*
				 * 若有 setsid()，就向该后端的整个进程
				 * 组发信号
				 */
#ifdef HAVE_SETSID
				(void) kill(-pid, SIGTERM);
#else
				(void) kill(pid, SIGTERM);
#endif
			}
		}
	}
}

/*
 * ProcArraySetReplicationSlotXmin。
 *
 * 为将来的 xmin 视界计算装上限制，防止清理
 * 和 HOT 裁剪删掉带有复制槽的客户端仍然需要的
 * 受影响的行。
 */
void
ProcArraySetReplicationSlotXmin(TransactionId xmin, TransactionId catalog_xmin,
								bool already_locked)
{
	Assert(!already_locked || LWLockHeldByMe(ProcArrayLock));

	if (!already_locked)
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	procArray->replication_slot_xmin = xmin;
	procArray->replication_slot_catalog_xmin = catalog_xmin;

	if (!already_locked)
		LWLockRelease(ProcArrayLock);

	elog(DEBUG1, "xmin required by slots: data %u, catalog %u",
		 xmin, catalog_xmin);
}

/*
 * ProcArrayGetReplicationSlotXmin。
 *
 * 返回当前槽的 xmin 限制。这对于删除
 * 比这些限制更旧的数据很有用。
 */
void
ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
								TransactionId *catalog_xmin)
{
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	if (xmin != NULL)
		*xmin = procArray->replication_slot_xmin;

	if (catalog_xmin != NULL)
		*catalog_xmin = procArray->replication_slot_catalog_xmin;

	LWLockRelease(ProcArrayLock);
}

/*
 * XidCacheRemoveRunningXids。
 *
 * 从本后端已知正在运行的
 * 子事务列表中移除一批 TransactionIds。
 * 指定的 xid 以及 xids[] 数组（长度为 nxids）里的那些，
 * 都从 subxid 缓存中移除。latestXid 必须是这组里最新的 XID。
 */
void
XidCacheRemoveRunningXids(TransactionId xid,
						  int nxids, const TransactionId *xids,
						  TransactionId latestXid)
{
	int			i,
				j;
	XidCacheStatus *mysubxidstat;

	Assert(TransactionIdIsValid(xid));

	/*
	 * 为了从 PGPROC 数组中移除事务，必须排他持有 ProcArrayLock。
	 * （见 src/backend/access/transam/README。）可能
	 * 这一点可以放宽，因为我们知道这个例程只用于
	 * 中止子事务，但在更仔细分析之前，最好
	 * 保守一点。
	 *
	 * 注意，这里不必小心地处理我们自己的读相对于 GetNewTransactionId() 的内存序，
	 * 只有本进程能修改
	 * MyProc/ProcGlobal->xids[] 的相关字段。但我们必须
	 * 小心让自己的写有良好的顺序。
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	mysubxidstat = &ProcGlobal->subxidStates[MyProc->pgxactoff];

	/*
	 * 正常情况下 xid 和 xids[] 会按递增顺序，
	 * subxid 里的项也是。倒着扫描，以免在移除大量 xid 时出现
	 * O(N^2) 行为。
	 */
	for (i = nxids - 1; i >= 0; i--)
	{
		TransactionId anxid = xids[i];

		for (j = MyProc->subxidStatus.count - 1; j >= 0; j--)
		{
			if (TransactionIdEquals(MyProc->subxids.xids[j], anxid))
			{
				MyProc->subxids.xids[j] = MyProc->subxids.xids[MyProc->subxidStatus.count - 1];
				pg_write_barrier();
				mysubxidstat->count--;
				MyProc->subxidStatus.count--;
				break;
			}
		}

		/*
		 * 通常应当已经找到它，除非缓存
		 * 溢出了。不过这个例程也可能对同一个子事务
		 * 被调用多次，在 AbortSubTransaction 期间出错时
		 * 就是这样。所以不 Assert，而是发出一条
		 * 调试警告。
		 */
		if (j < 0 && !MyProc->subxidStatus.overflowed)
			elog(WARNING, "did not find subXID %u in MyProc", anxid);
	}

	for (j = MyProc->subxidStatus.count - 1; j >= 0; j--)
	{
		if (TransactionIdEquals(MyProc->subxids.xids[j], xid))
		{
			MyProc->subxids.xids[j] = MyProc->subxids.xids[MyProc->subxidStatus.count - 1];
			pg_write_barrier();
			mysubxidstat->count--;
			MyProc->subxidStatus.count--;
			break;
		}
	}
	/* 通常应当已经找到它，除非缓存溢出了 */
	if (j < 0 && !MyProc->subxidStatus.overflowed)
		elog(WARNING, "did not find subXID %u in MyProc", xid);

	/* 持锁期间也推进全局 latestCompletedXid */
	MaintainLatestCompletedXid(latestXid);

	/* ...以及 xactCompletionCount */
	ShmemVariableCache->xactCompletionCount++;

	LWLockRelease(ProcArrayLock);
}

#ifdef XIDCACHE_DEBUG

/*
 * 打印 XID 缓存有效性的统计
 */
static void
DisplayXidCache(void)
{
	fprintf(stderr,
			"XidCache: xmin: %ld, known: %ld, myxact: %ld, latest: %ld, mainxid: %ld, childxid: %ld, knownassigned: %ld, nooflo: %ld, slow: %ld\n",
			xc_by_recent_xmin,
			xc_by_known_xact,
			xc_by_my_xact,
			xc_by_latest_xid,
			xc_by_main_xid,
			xc_by_child_xid,
			xc_by_known_assigned,
			xc_no_overflow,
			xc_slow_answer);
}
#endif							/* 处于 XIDCACHE_DEBUG */

/*
 * 若 rel 不是 NULL，返回适合该关系的测试状态，否则
 * 返回可用于所有关系的状态。后者可能把某些 XIDs 视为
 * 尚未对所有人可见，而针对特定关系的状态
 * 已经视为对所有人可见。
 *
 * 必须在快照处于活动或已注册时调用，否则
 * 会有回绕和其他危险。
 *
 * 详见 GlobalVisState 的注释。
 */
GlobalVisState *
GlobalVisTestFor(Relation rel)
{
	GlobalVisState *state = NULL;

	/* XXX：应当断言有一份快照被压入或已注册 */
	Assert(RecentXmin);

	switch (GlobalVisHorizonKindForRel(rel))
	{
		case VISHORIZON_SHARED:
			state = &GlobalVisSharedRels;
			break;
		case VISHORIZON_CATALOG:
			state = &GlobalVisCatalogRels;
			break;
		case VISHORIZON_DATA:
			state = &GlobalVisDataRels;
			break;
		case VISHORIZON_TEMP:
			state = &GlobalVisTempRels;
			break;
	}

	Assert(FullTransactionIdIsValid(state->definitely_needed) &&
		   FullTransactionIdIsValid(state->maybe_needed));

	return state;
}

/*
 * 若值得更新精确的 maybe_needed 边界，就返回真。
 *
 * 确定 xmin 视界有些贵，所以不想在
 * 受益可能性低时反复去做。
 *
 * 当前启发式是，只有 RecentXmin 自上次更新以来变了
 * 才更新。若当前最旧的正在运行的事务还没
 * 结束，重算视界不太可能有用。
 */
static bool
GlobalVisTestShouldUpdate(GlobalVisState *state)
{
	/* 还没更新过 */
	if (!TransactionIdIsValid(ComputeXidHorizonsResultLastXmin))
		return true;

	/*
	 * 若 maybe_needed 与 definitely_needed 边界相同，
	 * 刷新边界不太可能有益。
	 */
	if (FullTransactionIdFollowsOrEquals(state->maybe_needed,
										 state->definitely_needed))
		return false;

	/* 最近构建的快照的 xmin 是否不同？ */
	return RecentXmin != ComputeXidHorizonsResultLastXmin;
}

static void
GlobalVisUpdateApply(ComputeXidHorizonsResult *horizons)
{
	GlobalVisSharedRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->shared_oldest_nonremovable);
	GlobalVisCatalogRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->catalog_oldest_nonremovable);
	GlobalVisDataRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->data_oldest_nonremovable);
	GlobalVisTempRels.maybe_needed =
		FullXidRelativeTo(horizons->latest_completed,
						  horizons->temp_oldest_nonremovable);

	/*
	 * 在运行较久的事务里，先前必须当作运行中的事务
	 * 可能已经不在了。所以更新
	 * definitely_needed，使它不早于 maybe_needed。
	 */
	GlobalVisSharedRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisSharedRels.maybe_needed,
							   GlobalVisSharedRels.definitely_needed);
	GlobalVisCatalogRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisCatalogRels.maybe_needed,
							   GlobalVisCatalogRels.definitely_needed);
	GlobalVisDataRels.definitely_needed =
		FullTransactionIdNewer(GlobalVisDataRels.maybe_needed,
							   GlobalVisDataRels.definitely_needed);
	GlobalVisTempRels.definitely_needed = GlobalVisTempRels.maybe_needed;

	ComputeXidHorizonsResultLastXmin = RecentXmin;
}

/*
 * 用 ComputeXidHorizons() 更新 GlobalVis 里
 * 共享、系统目录和数据关系的边界。
 */
static void
GlobalVisUpdate(void)
{
	ComputeXidHorizonsResult horizons;

	/* 作为副作用更新视界 */
	ComputeXidHorizons(&horizons);
}

/*
 * 若不再有快照把 fxid 视为运行中，就返回真。
 *
 * 传入的状态必须已经按 fxid 所来自的关系初始化过
 * （NULL 也可以），否则结果可能不正确。
 *
 * 详见 GlobalVisState 的注释。
 */
bool
GlobalVisTestIsRemovableFullXid(GlobalVisState *state,
								FullTransactionId fxid)
{
	/*
	 * 若 fxid 比 maybe_needed 边界更旧，它肯定对
	 * 所有人可见。
	 */
	if (FullTransactionIdPrecedes(fxid, state->maybe_needed))
		return true;

	/*
	 * 若 fxid >= definitely_needed 边界，它很有可能仍被
	 * 视为运行中。
	 */
	if (FullTransactionIdFollowsOrEquals(fxid, state->definitely_needed))
		return false;

	/*
	 * fxid 位于 maybe_needed 和 definitely_needed 之间，也就是说可能存在
	 * 也可能不存在把 fxid 视为运行中的快照。若有意义，
	 * 就更新边界并重新检查。
	 */
	if (GlobalVisTestShouldUpdate(state))
	{
		GlobalVisUpdate();

		Assert(FullTransactionIdPrecedes(fxid, state->definitely_needed));

		return FullTransactionIdPrecedes(fxid, state->maybe_needed);
	}
	else
		return false;
}

/*
 * 供 32 位 xid 使用的 GlobalVisTestIsRemovableFullXid() 包装。
 *
 * 关键是它只对被防回绕的来源中的 xid 调用
 * （例如来自表，因而受
 * relfrozenxid 保护）。
 */
bool
GlobalVisTestIsRemovableXid(GlobalVisState *state, TransactionId xid)
{
	FullTransactionId fxid;

	/*
	 * 把 32 位参数转换成 FullTransactionId。可以安全地这样做，
	 * 因为我们知道该 xid 至少必须位于
	 * [oldestXid, nextXid) 之内，也就是离 xid 不到 20 亿。为避免为确定
	 * 其中任何一个而上锁，只需与
	 * state->definitely_needed 比较，它基于构建当前快照时
	 * 的那些值。
	 */
	fxid = FullXidRelativeTo(state->definitely_needed, xid);

	return GlobalVisTestIsRemovableFullXid(state, fxid);
}

/*
 * 返回一个 FullTransactionId，低于它的所有事务都不再被视为
 * 运行中。
 *
 * 注意：这比用 GlobalVisTestIsRemovableFullXid 测试效率更低，
 * 因为它很可能需要建立一个精确的
 * 截止，即使所有拿来与截止比较的 XIDs 都在
 * [maybe_needed, definitely_needed) 之外。
 */
FullTransactionId
GlobalVisTestNonRemovableFullHorizon(GlobalVisState *state)
{
	/* 若还没做过，就取得精确视界 */
	if (GlobalVisTestShouldUpdate(state))
		GlobalVisUpdate();

	return state->maybe_needed;
}

/* GlobalVisTestNonRemovableFullHorizon 的便捷包装 */
TransactionId
GlobalVisTestNonRemovableHorizon(GlobalVisState *state)
{
	FullTransactionId cutoff;

	cutoff = GlobalVisTestNonRemovableFullHorizon(state);

	return XidFromFullTransactionId(cutoff);
}

/*
 * GlobalVisTestFor() 与 GlobalVisTestIsRemovableFullXid() 的便捷包装，
 * 见它们的注释。
 */
bool
GlobalVisCheckRemovableFullXid(Relation rel, FullTransactionId fxid)
{
	GlobalVisState *state;

	state = GlobalVisTestFor(rel);

	return GlobalVisTestIsRemovableFullXid(state, fxid);
}

/*
 * GlobalVisTestFor() 与 GlobalVisTestIsRemovableXid() 的便捷包装，
 * 见它们的注释。
 */
bool
GlobalVisCheckRemovableXid(Relation rel, TransactionId xid)
{
	GlobalVisState *state;

	state = GlobalVisTestFor(rel);

	return GlobalVisTestIsRemovableXid(state, xid);
}

/*
 * 把 32 位事务 id 转成 64 位事务 id，假定它位于
 * XidFromFullTransactionId(rel) 的 MaxTransactionId / 2 范围之内。
 *
 * 使用这个函数时要非常小心。只有在能保证 xid 位于 rel 的
 * MaxTransactionId / 2 个 xid 之内时才能安全使用。
 * 例如，若调用方保证后端持有快照，
 * 并且 xid 来自表（清理和冻结保证
 * 该 xid 必须在那个范围内），或者 xid 来自 procarray 并
 * 以此防止 xid 回绕，就可以保证。
 */
static inline FullTransactionId
FullXidRelativeTo(FullTransactionId rel, TransactionId xid)
{
	TransactionId rel_xid = XidFromFullTransactionId(rel);

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(rel_xid));

	/* 不保证能发现问题，但多半能抓住错误 */
	AssertTransactionIdInAllowableRange(xid);

	return FullTransactionIdFromU64(U64FromFullTransactionId(rel)
									+ (int32) (xid - rel_xid));
}


/* ----------------------------------------------
 *		KnownAssignedTransactionIds 子模块
 * ----------------------------------------------
 */

/*
 * 在热备模式下，我们维护一份在 WAL 当前点上于主库
 * 正在（或曾经）运行的事务列表。备库事务必须把这些 XIDs
 * 当作正在运行，即使它们不在
 * 备库服务器的 PGPROC 数组里。
 *
 * 我们记录所有已知已被分配的 XIDs。这包括在 WAL 记录里
 * 见到的全部 XIDs，加上我们能推断已被分配的所有未观察到的 XIDs。
 * 能推断未观察到的 XIDs 存在，是因为我们知道
 * XIDs 按顺序分配，没有空隙。KnownAssignedXids
 * 列表随新 XIDs 被观察到或推断出来而扩展，并在
 * 事务完成记录到达时收缩。
 *
 * 热备期间我们不太计较顶层 XIDs 和子事务 XIDs 的
 * 区别。两者一起存在 KnownAssignedXids 列表里。
 * 在后端里，这被复制进 GetSnapshotData() 的快照，
 * 利用了 XidInMVCCSnapshot()
 * 同样不在乎这个区别的事实。子事务 XIDs 实际上被当作
 * 顶层 XIDs，典型情况下不维护 pg_subtrans
 * 链接（这不影响可见性）。
 *
 * KnownAssignedXids 和快照里有空间装下 maxProcs *
 * (1 + PGPROC_MAX_CACHED_SUBXIDS) 个 XIDs，所以每个主库事务必须
 * 至少每 PGPROC_MAX_CACHED_SUBXIDS 个就在一条 WAL
 * XLOG_XACT_ASSIGNMENT 记录里报告它的子事务 XIDs。收到这样一条
 * 记录时，我们在 pg_subtrans 里把子 XIDs 标为顶层 XID 的子事务，
 * 然后把它们从 KnownAssignedXids 中移除。这防止
 * KnownAssignedXids 和快照溢出，代价是这些
 * 子 XIDs 的状态检查会走 TransactionIdIsInProgress() 这条较慢的路径。
 * 这意味着 KnownAssignedXids 对子 XIDs 不一定完整，
 * 但对顶层 XIDs 应当完整；这与正常运行时
 * PGPROC 项的情况相同。
 *
 * 从 KnownAssignedXids 丢掉子 XIDs 时，需要记住
 * 这一点，类似于跟踪 PGPROC 的 subxid 数组溢出。我们
 * 通过记住 lastOverflowedXid 来做，也就是最后丢掉的那个 subXID。
 * 只要它还在感兴趣的 XIDs 范围内，就必须假定
 * 快照里缺少 subXIDs。（注意 subXID 溢出在
 * 主库上发生于第 65 个 subXID 到达时，而在备库上发生于第 64 个
 * subXID 到达时，这不是错误。）
 *
 * 若主库上的后端在写出中止记录之前不知怎么消失了，
 * 我们就把那些 XIDs 留在 KnownAssignedXids 里。它们实际上
 * 已中止，但我们以为它们在运行；这个区别无关紧要，
 * 因为无论哪种，该事务所做的任何变更对
 * 备库里的后端都不可见。当
 * XLOG_RUNNING_XACTS 到达时我们裁剪 KnownAssignedXids，以防这种死掉的 XIDs
 * 造成数组溢出。
 */

/*
 * RecordKnownAssignedTransactionIds
 *		把给定 XID 以及任何在它之前、
 *		尚未观察到的 XIDs 记入 KnownAssignedXids。
 *
 * 应当对与事务关联的每一条 WAL 记录都运行 RecordKnownAssignedTransactionIds()。
 * 必须在执行了 StartupCLOG() 等之后对每条记录调用，因为我们必须
 * ExtendCLOG() 等等。
 *
 * 在恢复期间调用，类比并代替 GetNewTransactionId()。
 */
void
RecordKnownAssignedTransactionIds(TransactionId xid)
{
	Assert(standbyState >= STANDBY_INITIALIZED);
	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(latestObservedXid));

	elog(trace_recovery(DEBUG4), "record known xact %u latestObservedXid %u",
		 xid, latestObservedXid);

	/*
	 * 新观察到的 xid 到达时，经常出现它
	 * 并不是序列中的下一个 xid。发生这种情况时，必须把
	 * 中间的那些 xid 也当作正在运行。
	 */
	if (TransactionIdFollows(xid, latestObservedXid))
	{
		TransactionId next_expected_xid;

		/*
		 * 像正常操作中 GetNewTransactionId() 那样扩展 subtrans，
		 * 用单独的扩展步骤。注意不需要扩展 clog，因为
		 * 它的扩展记入了 WAL。
		 *
		 * 无论 standbyState 如何，这部分都必须做，因为我们
		 * 立刻开始把子事务分配给它们的顶层
		 * 事务。
		 */
		next_expected_xid = latestObservedXid;
		while (TransactionIdPrecedes(next_expected_xid, xid))
		{
			TransactionIdAdvance(next_expected_xid);
			ExtendSUBTRANS(next_expected_xid);
		}
		Assert(next_expected_xid == xid);

		/*
		 * 若 KnownAssignedXids 机制还没起来，就没有更多的事可做，
		 * 因为我们还没跟踪已分配的 xid。
		 */
		if (standbyState <= STANDBY_INITIALIZED)
		{
			latestObservedXid = xid;
			return;
		}

		/*
		 * 把 (latestObservedXid, xid] 加到 KnownAssignedXids 数组上。
		 */
		next_expected_xid = latestObservedXid;
		TransactionIdAdvance(next_expected_xid);
		KnownAssignedXidsAdd(next_expected_xid, xid, false);

		/*
		 * 现在可以推进 latestObservedXid
		 */
		latestObservedXid = xid;

		/* ShmemVariableCache->nextXid 必须超出任何已观察到的 xid */
		AdvanceNextFullTransactionIdPastXid(latestObservedXid);
	}
}

/*
 * ExpireTreeKnownAssignedTransactionIds
 *		从 KnownAssignedXids 中移除给定的 XIDs。
 *
 * 在恢复期间调用，类比并代替 ProcArrayEndTransaction()。
 */
void
ExpireTreeKnownAssignedTransactionIds(TransactionId xid, int nsubxids,
									  TransactionId *subxids, TransactionId max_xid)
{
	Assert(standbyState >= STANDBY_INITIALIZED);

	/*
	 * 使用与事务提交相同的加锁
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	KnownAssignedXidsRemoveTree(xid, nsubxids, subxids);

	/* 与 ProcArrayEndTransaction 中一样，推进 latestCompletedXid */
	MaintainLatestCompletedXidRecovery(max_xid);

	/* ...以及 xactCompletionCount */
	ShmemVariableCache->xactCompletionCount++;

	LWLockRelease(ProcArrayLock);
}

/*
 * ExpireAllKnownAssignedTransactionIds
 *		移除 KnownAssignedXids 中的全部项，并重置 lastOverflowedXid。
 */
void
ExpireAllKnownAssignedTransactionIds(void)
{
	FullTransactionId latestXid;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	KnownAssignedXidsRemovePreceding(InvalidTransactionId);

	/* 把 latestCompletedXid 重置为 nextXid - 1 */
	Assert(FullTransactionIdIsValid(ShmemVariableCache->nextXid));
	latestXid = ShmemVariableCache->nextXid;
	FullTransactionIdRetreat(&latestXid);
	ShmemVariableCache->latestCompletedXid = latestXid;

	/*
	 * 任何进行中的事务实际上都被中止了，所以
	 * 推进 xactCompletionCount。
	 */
	ShmemVariableCache->xactCompletionCount++;

	/*
	 * 重置 lastOverflowedXid。目前，在调用这个函数之后
	 * lastOverflowedXid 没有用途。但为了与
	 * ExpireOldKnownAssignedTransactionIds() 所做的统一，还是做这件事。
	 */
	procArray->lastOverflowedXid = InvalidTransactionId;
	LWLockRelease(ProcArrayLock);
}

/*
 * ExpireOldKnownAssignedTransactionIds
 *		移除给定 XID 之前的 KnownAssignedXids 项，并
 *		可能重置 lastOverflowedXid。
 */
void
ExpireOldKnownAssignedTransactionIds(TransactionId xid)
{
	TransactionId latestXid;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/* 与 ProcArrayEndTransaction 中一样，推进 latestCompletedXid */
	latestXid = xid;
	TransactionIdRetreat(latestXid);
	MaintainLatestCompletedXidRecovery(latestXid);

	/* ...以及 xactCompletionCount */
	ShmemVariableCache->xactCompletionCount++;

	/*
	 * 若知道所有可能曾在运行的事务都已经消失，就重置 lastOverflowedXid。
	 * 不这样做可能导致不正确的
	 * lastOverflowedXid 值，从而使额外的快照被标为
	 * 子事务溢出。
	 */
	if (TransactionIdPrecedes(procArray->lastOverflowedXid, xid))
		procArray->lastOverflowedXid = InvalidTransactionId;
	KnownAssignedXidsRemovePreceding(xid);
	LWLockRelease(ProcArrayLock);
}

/*
 * KnownAssignedTransactionIdsIdleMaintenance
 *		在启动进程即将空闲时，
 *		伺机做维护工作。
 */
void
KnownAssignedTransactionIdsIdleMaintenance(void)
{
	KnownAssignedXidsCompress(KAX_STARTUP_PROCESS_IDLE, false);
}


/*
 * 操作 KnownAssignedXids 的私有模块函数。
 *
 * KnownAssignedXids 数据结构有 5 种用途：
 *
 *	后端取快照：需要把所有有效 XIDs 拷出去。
 *	后端想判断某个特定 XID 是否存在。
 *	启动进程加入新的已知已分配 XIDs。
 *	启动进程在事务结束时移除特定 XIDs。
 *	启动进程在特殊 WAL 记录到达时裁剪数组。
 *
 * 这个数据结构在热备期间是已知热点，所以我们
 * 费些功夫让这些操作尽量高效，也尽量
 * 并发。
 *
 * XIDs 按排序存在数组里，确切说是 TransactionIdPrecedes
 * 序，以便对特定 XIDs 做二分查找。注意：
 * 一般而言 TransactionIdPrecedes 并不提供全序，但
 * 我们知道任一时刻存在的项不应当跨越
 * XID 空间中大到会回绕的那一部分（主库会
 * 在那发生之前很久就因担心 XID 回绕而关闭）。所以可以用
 * TransactionIdPrecedes 作为二分查找的比较器。
 *
 * 插入时维护有序很便宜，因为新的已知
 * XIDs 总是按 XID 顺序报告；我们只是把它们追加在右边。
 *
 * 为了让单次删除保持便宜，需要允许数组里有空隙。
 * 这通过并行布尔数组 KnownAssignedXidsValid[] 来实现，
 * 用它把数组元素标为有效或无效。删除时把
 * KnownAssignedXidsValid[i] 设为假，而不清除
 * XID 项本身。这保留 XID 项有序的性质，
 * 所以可以轻松做二分查找。我们定期压缩掉
 * 未使用的项；这比每次删除都立刻压缩
 * 数组要便宜得多。
 *
 * KnownAssignedXids[] 和 KnownAssignedXidsValid[] 里真正有效的项
 * 是下标 tail <= i < head 的那些；这个下标范围之外的项
 * 内容未指定。当 head 到达数组末尾时，我们
 * 强制压缩未使用的项，而不是回绕，因为
 * 允许回绕会大大复杂化查找逻辑。我们维护
 * 一个显式的 tail 指针，这样裁剪旧 XIDs 时不必
 * 立刻移动数组内容。大多数情况下，任一时刻只有一小部分
 * 数组含有有效项。
 *
 * 虽然只有启动进程会改变 KnownAssignedXids
 * 数据结构，仍然需要互锁，这样备库后端
 * 才不会观察到无效的中间状态。约定是后端
 * 检查数组时必须持有共享 ProcArrayLock。要从数组中
 * 移除 XIDs，启动进程必须排他持有 ProcArrayLock，
 * 原因是通常的事务性理由（对照正常运行期间
 * 事务的提交或中止）。把未使用的项压缩出数组
 * 同样需要排他锁。要把 XIDs 加进数组，只需把它们
 * 插到 head 指针右边的槽里，然后推进 head
 * 指针。这本来完全不需要锁，只是在
 * 内存序较弱的机器上，必须小心让其他处理器
 * 先看到数组元素的变化，再看到 head 指针的变化。
 * 我们用自旋锁来保护 head/tail 指针的读写，以此处理这一点。
 * （若去创建合适的内存访问屏障原语并改用那些，
 * 就可以不用自旋锁。）
 * 除非调用方排他持有 ProcArrayLock，否则读写 head/tail 指针
 * 都必须取得自旋锁。
 *
 * 算法分析：
 *
 * 若最多有 M 个槽，当前 N 个 XIDs 分布在
 * S 个元素上，则始终有 N <= S <= M。
 *
 *	加入一个新 XID 是 O(1)，只需要很少的锁（除非必须
 *		发生压缩）。
 *	压缩数组是 O(S)，需要排他锁。
 *	移除一个 XID 是 O(logS)，需要排他锁。
 *	取快照是 O(S)，需要共享锁。
 *	检查一个 XID 是 O(logS)，需要共享锁。
 *
 * 相比之下，若 KnownAssignedXids 用散列表，
 * 取快照就会是 O(M)。若能保持 S 远小于 M，
 * 有序数组技术会给出快得多的快照。
 * 若试图把 S 保持得太小，就会花太多时间压缩，
 * 所以对任何工作负载组合都有一个最优点。我们用启发式
 * 决定何时压缩数组，裁剪也有助于降低压缩频率。
 * 该启发式需要我们跟踪数组里当前有效
 * XIDs 的数目 (N)。除特殊情况外，S >= 2N 时就压缩。
 * 把 S 限制在 2N 又把取快照的时间限制为 O(N)，
 * 而它本来也必须是这样。
 */


/*
 * 压缩 KnownAssignedXids，把有效数据下移到数组
 * 开头，去掉任何空隙。
 *
 * 若 reason 是 KAX_NO_SPACE，就强制做一步压缩，否则
 * 只在启发式表明这是好时机时才做。
 *
 * 压缩需要以排他模式持有 ProcArrayLock。
 * 若调用方已经持有该锁，必须传入 haveLock = true。
 */
static void
KnownAssignedXidsCompress(KAXCompressReason reason, bool haveLock)
{
	ProcArrayStruct *pArray = procArray;
	int			head,
				tail,
				nelements;
	int			compress_index;
	int			i;

	/* 压缩启发式用的计数器 */
	static unsigned int transactionEndsCounter;
	static TimestampTz lastCompressTs;

	/* 调优常数 */
#define KAX_COMPRESS_FREQUENCY 128	/* 以事务计 */
#define KAX_COMPRESS_IDLE_INTERVAL 1000 /* 以毫秒计 */

	/*
	 * 因为只有启动进程会修改 head/tail 指针，这里读它们
	 * 不需要锁。
	 */
	head = pArray->headKnownAssignedXids;
	tail = pArray->tailKnownAssignedXids;
	nelements = head - tail;

	/*
	 * 若可以选择是否压缩，就用启发式来避免
	 * 压缩得太频繁或不够频繁。这里的压缩只是
	 * 把值移到数组开头，所以不像典型的数据压缩算法那样
	 * 复杂或昂贵。
	 */
	if (nelements == pArray->numKnownAssignedXids)
	{
		/*
		 * 当 head 和 tail 之间没有空隙时，就不必
		 * 压缩，除非是 KAX_NO_SPACE 情形，那时必须压缩才能
		 * 在 head 之后腾出一些空间。
		 */
		if (reason != KAX_NO_SPACE)
			return;
	}
	else if (reason == KAX_TRANSACTION_END)
	{
		/*
		 * 只在每这么多次提交时才考虑压缩一次。频率
		 * 由基准测试决定。
		 */
		if ((transactionEndsCounter++) % KAX_COMPRESS_FREQUENCY != 0)
			return;

		/*
		 * 此外，只有在数组已用部分的填充不到
		 * 50% 时才压缩（见上面的注释）。
		 */
		if (nelements < 2 * pArray->numKnownAssignedXids)
			return;
	}
	else if (reason == KAX_STARTUP_PROCESS_IDLE)
	{
		/*
		 * 因为缺少新的 WAL，我们即将空闲，所以不妨
		 * 压缩。但不要太频繁，以免与读者争用
		 * ProcArray 锁。
		 */
		if (lastCompressTs != 0)
		{
			TimestampTz compress_after;

			compress_after = TimestampTzPlusMilliseconds(lastCompressTs,
														 KAX_COMPRESS_IDLE_INTERVAL);
			if (GetCurrentTimestamp() < compress_after)
				return;
		}
	}

	/* 需要压缩，所以若还没有锁就去取。 */
	if (!haveLock)
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	/*
	 * 压缩数组的办法是从 tail 读到 head 的有效值，
	 * 把数据重新对齐到第 0 个元素。
	 */
	compress_index = 0;
	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			KnownAssignedXids[compress_index] = KnownAssignedXids[i];
			KnownAssignedXidsValid[compress_index] = true;
			compress_index++;
		}
	}
	Assert(compress_index == pArray->numKnownAssignedXids);

	pArray->tailKnownAssignedXids = 0;
	pArray->headKnownAssignedXids = compress_index;

	if (!haveLock)
		LWLockRelease(ProcArrayLock);

	/* 更新维护用的时间戳。这件事不需要持锁。 */
	lastCompressTs = GetCurrentTimestamp();
}

/*
 * 在数组头部把 xid 加入 KnownAssignedXids。
 *
 * 从 from_xid 到 to_xid（含两端）的 xid 被加入数组。
 *
 * 若 exclusive_lock 为真，调用方已经以排他模式持有 ProcArrayLock，
 * 所以这里不需要额外加锁。否则调用方不持
 * 锁，因此必须确保对并发读者维持足够的互锁。
 * （只有启动进程会调用这里，所以不必
 * 担心并发的写者。）
 */
static void
KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
					 bool exclusive_lock)
{
	ProcArrayStruct *pArray = procArray;
	TransactionId next_xid;
	int			head,
				tail;
	int			nxids;
	int			i;

	Assert(TransactionIdPrecedesOrEquals(from_xid, to_xid));

	/*
	 * 计算需要多少个数组槽。通常这很便宜；在
	 * XIDs 跨越回绕点的不常见情况下，就用笨
	 * 办法。
	 */
	if (to_xid >= from_xid)
		nxids = to_xid - from_xid + 1;
	else
	{
		nxids = 1;
		next_xid = from_xid;
		while (TransactionIdPrecedes(next_xid, to_xid))
		{
			nxids++;
			TransactionIdAdvance(next_xid);
		}
	}

	/*
	 * 因为只有启动进程会修改 head/tail 指针，这里读它们
	 * 不需要锁。
	 */
	head = pArray->headKnownAssignedXids;
	tail = pArray->tailKnownAssignedXids;

	Assert(head >= 0 && head <= pArray->maxKnownAssignedXids);
	Assert(tail >= 0 && tail < pArray->maxKnownAssignedXids);

	/*
	 * 验证插入按 TransactionId 顺序发生。注意即使
	 * 最后一个已有元素被标为无效，它仍然必须有
	 * 顺序正确的 XID 值。
	 */
	if (head > tail &&
		TransactionIdFollowsOrEquals(KnownAssignedXids[head - 1], from_xid))
	{
		KnownAssignedXidsDisplay(LOG);
		elog(ERROR, "out-of-order XID insertion in KnownAssignedXids");
	}

	/*
	 * 若我们的 xid 放不进剩余空间，就把空闲空间压缩出去
	 */
	if (head + nxids > pArray->maxKnownAssignedXids)
	{
		KnownAssignedXidsCompress(KAX_NO_SPACE, exclusive_lock);

		head = pArray->headKnownAssignedXids;
		/* 注意：我们不再关心 tail 指针 */

		/*
		 * 若仍然放不下，那就是内存不足
		 */
		if (head + nxids > pArray->maxKnownAssignedXids)
			elog(ERROR, "too many KnownAssignedXids");
	}

	/* 现在可以把 xid 插进从 head 开始的空间 */
	next_xid = from_xid;
	for (i = 0; i < nxids; i++)
	{
		KnownAssignedXids[head] = next_xid;
		KnownAssignedXidsValid[head] = true;
		TransactionIdAdvance(next_xid);
		head++;
	}

	/* 调整有效项数目的计数 */
	pArray->numKnownAssignedXids += nxids;

	/*
	 * 现在更新 head 指针。我们用自旋锁保护这个
	 * 指针，不是因为更新很可能非原子，而是为了
	 * 确保其他处理器先看到上面的数组更新，再
	 * 看到 head 指针的变化。
	 *
	 * 若正排他持有 ProcArrayLock，就不必
	 * 再取自旋锁。
	 */
	if (exclusive_lock)
		pArray->headKnownAssignedXids = head;
	else
	{
		SpinLockAcquire(&pArray->known_assigned_xids_lck);
		pArray->headKnownAssignedXids = head;
		SpinLockRelease(&pArray->known_assigned_xids_lck);
	}
}

/*
 * KnownAssignedXidsSearch。
 *
 * 在 KnownAssignedXids 里搜索特定 xid，并可选地移除它。
 * 找到返回真，否则返回假。
 *
 * 调用方必须以共享或排他模式持有 ProcArrayLock。
 * remove = true 时必须持有排他锁。
 */
static bool
KnownAssignedXidsSearch(TransactionId xid, bool remove)
{
	ProcArrayStruct *pArray = procArray;
	int			first,
				last;
	int			head;
	int			tail;
	int			result_index = -1;

	if (remove)
	{
		/* 我们排他持有 ProcArrayLock，所以不需要自旋锁 */
		tail = pArray->tailKnownAssignedXids;
		head = pArray->headKnownAssignedXids;
	}
	else
	{
		/* 取自旋锁，以确保看到最新的数组内容 */
		SpinLockAcquire(&pArray->known_assigned_xids_lck);
		tail = pArray->tailKnownAssignedXids;
		head = pArray->headKnownAssignedXids;
		SpinLockRelease(&pArray->known_assigned_xids_lck);
	}

	/*
	 * 标准二分查找。注意这里可以忽略 KnownAssignedXidsValid
	 * 数组，因为即使无效项也含有有序的 XIDs。
	 */
	first = tail;
	last = head - 1;
	while (first <= last)
	{
		int			mid_index;
		TransactionId mid_xid;

		mid_index = (first + last) / 2;
		mid_xid = KnownAssignedXids[mid_index];

		if (xid == mid_xid)
		{
			result_index = mid_index;
			break;
		}
		else if (TransactionIdPrecedes(xid, mid_xid))
			last = mid_index - 1;
		else
			first = mid_index + 1;
	}

	if (result_index < 0)
		return false;			/* 不在数组里 */

	if (!KnownAssignedXidsValid[result_index])
		return false;			/* 在数组里，但无效 */

	if (remove)
	{
		KnownAssignedXidsValid[result_index] = false;

		pArray->numKnownAssignedXids--;
		Assert(pArray->numKnownAssignedXids >= 0);

		/*
		 * 若正在移除 tail 元素，就把 tail 指针向前推进，跳过
		 * 任何无效元素。这会加快以后的查找。
		 */
		if (result_index == tail)
		{
			tail++;
			while (tail < head && !KnownAssignedXidsValid[tail])
				tail++;
			if (tail >= head)
			{
				/* 数组是空的，所以两个指针都可以重置 */
				pArray->headKnownAssignedXids = 0;
				pArray->tailKnownAssignedXids = 0;
			}
			else
			{
				pArray->tailKnownAssignedXids = tail;
			}
		}
	}

	return true;
}

/*
 * 指定的 XID 是否在 KnownAssignedXids[] 里？
 *
 * 调用方必须以共享或排他模式持有 ProcArrayLock。
 */
static bool
KnownAssignedXidExists(TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	return KnownAssignedXidsSearch(xid, false);
}

/*
 * 从 KnownAssignedXids[] 中移除指定的 XID。
 *
 * 调用方必须以排他模式持有 ProcArrayLock。
 */
static void
KnownAssignedXidsRemove(TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	elog(trace_recovery(DEBUG4), "remove KnownAssignedXid %u", xid);

	/*
	 * 注意：移除一个不存在的 XID，无法当成错误。
	 * 处理 XLOG_XACT_ASSIGNMENT 时，我们有意移除子事务 ID，
	 * 以免数组溢出。然后那些 XIDs 会在
	 * 顶层事务提交或中止时被再次移除。
	 *
	 * 也许可以跟踪这些 XIDs，以便把这种情况和
	 * 真正的错误区分开，但那会很复杂，大概不值得。
	 * 所以直接忽略查找结果。
	 */
	(void) KnownAssignedXidsSearch(xid, true);
}

/*
 * KnownAssignedXidsRemoveTree
 *		移除 xid（若它不是 InvalidTransactionId）以及所有 subxid。
 *
 * 调用方必须以排他模式持有 ProcArrayLock。
 */
static void
KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
							TransactionId *subxids)
{
	int			i;

	if (TransactionIdIsValid(xid))
		KnownAssignedXidsRemove(xid);

	for (i = 0; i < nsubxids; i++)
		KnownAssignedXidsRemove(subxids[i]);

	/* 伺机压缩数组 */
	KnownAssignedXidsCompress(KAX_TRANSACTION_END, true);
}

/*
 * 把 KnownAssignedXids 裁剪到 xid，但不包括 xid。若 xid 无效，
 * 就清空整张表。
 *
 * 调用方必须以排他模式持有 ProcArrayLock。
 */
static void
KnownAssignedXidsRemovePreceding(TransactionId removeXid)
{
	ProcArrayStruct *pArray = procArray;
	int			count = 0;
	int			head,
				tail,
				i;

	if (!TransactionIdIsValid(removeXid))
	{
		elog(trace_recovery(DEBUG4), "removing all KnownAssignedXids");
		pArray->numKnownAssignedXids = 0;
		pArray->headKnownAssignedXids = pArray->tailKnownAssignedXids = 0;
		return;
	}

	elog(trace_recovery(DEBUG4), "prune KnownAssignedXids to %u", removeXid);

	/*
	 * 从 tail 开始把项标为无效。因为数组有序，
	 * 一旦到达 >= removeXid 的项就可以停止。
	 */
	tail = pArray->tailKnownAssignedXids;
	head = pArray->headKnownAssignedXids;

	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			TransactionId knownXid = KnownAssignedXids[i];

			if (TransactionIdFollowsOrEquals(knownXid, removeXid))
				break;

			if (!StandbyTransactionIdIsPrepared(knownXid))
			{
				KnownAssignedXidsValid[i] = false;
				count++;
			}
		}
	}

	pArray->numKnownAssignedXids -= count;
	Assert(pArray->numKnownAssignedXids >= 0);

	/*
	 * 若已把 tail 项标为无效，就推进 tail 指针。
	 */
	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
			break;
	}
	if (i >= head)
	{
		/* 数组是空的，所以两个指针都可以重置 */
		pArray->headKnownAssignedXids = 0;
		pArray->tailKnownAssignedXids = 0;
	}
	else
	{
		pArray->tailKnownAssignedXids = i;
	}

	/* 伺机压缩数组 */
	KnownAssignedXidsCompress(KAX_PRUNE, true);
}

/*
 * KnownAssignedXidsGet：扫描 KnownAssignedXids 取得 xid 数组。
 * 滤掉任何 >= xmax 的项。
 *
 * 返回存进 xarray[] 的 XIDs 数目。调用方负责
 * 数组足够大。
 *
 * 调用方必须至少以共享模式持有 ProcArrayLock。
 */
static int
KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax)
{
	TransactionId xtmp = InvalidTransactionId;

	return KnownAssignedXidsGetAndSetXmin(xarray, &xtmp, xmax);
}

/*
 * KnownAssignedXidsGetAndSetXmin：与 KnownAssignedXidsGet 相同，另外
 * 若 *xmin 还不是更低，就把它降到所见的最低 xid。
 *
 * 调用方必须至少以共享模式持有 ProcArrayLock。
 */
static int
KnownAssignedXidsGetAndSetXmin(TransactionId *xarray, TransactionId *xmin,
							   TransactionId xmax)
{
	int			count = 0;
	int			head,
				tail;
	int			i;

	/*
	 * head 只取一次，因为它可能在循环期间改变。一旦到达最初看到的
	 * head 就可以停止，因为可以确定：持有 ProcArrayLock 期间，一个 xid
	 * 无法进入数组然后又离开。我们
	 * 可能漏掉新加入的 xid，但它们应当 >= xmax，所以反正
	 * 无关。
	 *
	 * 必须取自旋锁，以确保看到最新的数组内容。
	 */
	SpinLockAcquire(&procArray->known_assigned_xids_lck);
	tail = procArray->tailKnownAssignedXids;
	head = procArray->headKnownAssignedXids;
	SpinLockRelease(&procArray->known_assigned_xids_lck);

	for (i = tail; i < head; i++)
	{
		/* 跳过数组里的任何空隙 */
		if (KnownAssignedXidsValid[i])
		{
			TransactionId knownXid = KnownAssignedXids[i];

			/*
			 * 若需要就更新 xmin。只需检查第一个 XID，
			 * 因为数组是有序的。
			 */
			if (count == 0 &&
				TransactionIdPrecedes(knownXid, *xmin))
				*xmin = knownXid;

			/*
			 * 滤掉任何 >= xmax 的项，同样依赖数组的
			 * 有序性质。
			 */
			if (TransactionIdIsValid(xmax) &&
				TransactionIdFollowsOrEquals(knownXid, xmax))
				break;

			/* 把 knownXid 加入输出数组 */
			xarray[count++] = knownXid;
		}
	}

	return count;
}

/*
 * 取得 KnownAssignedXids 数组里最旧的 XID，若里面什么都没有
 * 则为 InvalidTransactionId。
 */
static TransactionId
KnownAssignedXidsGetOldestXmin(void)
{
	int			head,
				tail;
	int			i;

	/*
	 * head 只取一次，因为它可能在循环期间改变。
	 */
	SpinLockAcquire(&procArray->known_assigned_xids_lck);
	tail = procArray->tailKnownAssignedXids;
	head = procArray->headKnownAssignedXids;
	SpinLockRelease(&procArray->known_assigned_xids_lck);

	for (i = tail; i < head; i++)
	{
		/* 跳过数组里的任何空隙 */
		if (KnownAssignedXidsValid[i])
			return KnownAssignedXids[i];
	}

	return InvalidTransactionId;
}

/*
 * 显示 KnownAssignedXids，以提供调试踪迹。
 *
 * 目前只在启动进程内调用，所以不需要
 * 特殊加锁。
 *
 * 注意这相当贵，而且即使 elog 消息会被丢掉，
 * 大部分开销仍然会产生。不过目前不在任何
 * 性能关键的地方调用，所以不必那么紧张。
 */
static void
KnownAssignedXidsDisplay(int trace_level)
{
	ProcArrayStruct *pArray = procArray;
	StringInfoData buf;
	int			head,
				tail,
				i;
	int			nxids = 0;

	tail = pArray->tailKnownAssignedXids;
	head = pArray->headKnownAssignedXids;

	initStringInfo(&buf);

	for (i = tail; i < head; i++)
	{
		if (KnownAssignedXidsValid[i])
		{
			nxids++;
			appendStringInfo(&buf, "[%d]=%u ", i, KnownAssignedXids[i]);
		}
	}

	elog(trace_level, "%d KnownAssignedXids (num=%d tail=%d head=%d) %s",
		 nxids,
		 pArray->numKnownAssignedXids,
		 pArray->tailKnownAssignedXids,
		 pArray->headKnownAssignedXids,
		 buf.data);

	pfree(buf.data);
}

/*
 * KnownAssignedXidsReset
 *		把 KnownAssignedXids 重置为空
 */
static void
KnownAssignedXidsReset(void)
{
	ProcArrayStruct *pArray = procArray;

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	pArray->numKnownAssignedXids = 0;
	pArray->tailKnownAssignedXids = 0;
	pArray->headKnownAssignedXids = 0;

	LWLockRelease(ProcArrayLock);
}
