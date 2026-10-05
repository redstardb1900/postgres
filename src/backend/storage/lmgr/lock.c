/*-------------------------------------------------------------------------
 *
 * lock.c
 *	  POSTGRES 主锁机制
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lock.c
 *
 * 说明（NOTES）
 *	  锁表是一张共享内存哈希表。当某个进程
 *	  试图获取一把与已有锁相冲突的类型的锁时，
 *	  会用 storage/lmgr/proc.c 中的例程
 *	  让它进入睡眠。
 *
 *	  多数情况下，这段代码应经由 lmgr.c
 *	  或其他锁管理模块调用，而不应直接调用。
 *
 *	接口：
 *
 *	InitLocks()、GetLocksMethodTable()、GetLockTagsMethodTable()、
 *	LockAcquire()、LockRelease()、LockReleaseAll()、
 *	LockCheckConflicts()、GrantLock()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner_private.h"


/* 此配置变量用于设置锁表大小 */
int			max_locks_per_xact; /* 由 guc.c 设置 */

#define NLOCKENTS() \
	mul_size(max_locks_per_xact, add_size(MaxBackends, max_prepared_xacts))


/*
 * 定义标准锁方法语义的数据结构。
 *
 * 冲突表定义了各种锁模式的语义。
 */
static const LOCKMASK LockConflicts[] = {
	0,

	/* AccessShareLock（访问共享锁） */
	LOCKBIT_ON(AccessExclusiveLock),

	/* RowShareLock（行共享锁） */
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* RowExclusiveLock（行排他锁） */
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareUpdateExclusiveLock（共享更新排他锁） */
	LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareLock（共享锁） */
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ShareRowExclusiveLock（共享行排他锁） */
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* ExclusiveLock（排他锁） */
	LOCKBIT_ON(RowShareLock) |
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock),

	/* AccessExclusiveLock（访问排他锁） */
	LOCKBIT_ON(AccessShareLock) | LOCKBIT_ON(RowShareLock) |
	LOCKBIT_ON(RowExclusiveLock) | LOCKBIT_ON(ShareUpdateExclusiveLock) |
	LOCKBIT_ON(ShareLock) | LOCKBIT_ON(ShareRowExclusiveLock) |
	LOCKBIT_ON(ExclusiveLock) | LOCKBIT_ON(AccessExclusiveLock)

};

/* 锁模式名称，供调试打印使用 */
static const char *const lock_mode_names[] =
{
	"INVALID",
	"AccessShareLock",
	"RowShareLock",
	"RowExclusiveLock",
	"ShareUpdateExclusiveLock",
	"ShareLock",
	"ShareRowExclusiveLock",
	"ExclusiveLock",
	"AccessExclusiveLock"
};

#ifndef LOCK_DEBUG
static bool Dummy_trace = false;
#endif

static const LockMethodData default_lockmethod = {
	MaxLockMode,
	LockConflicts,
	lock_mode_names,
#ifdef LOCK_DEBUG
	&Trace_locks
#else
	&Dummy_trace
#endif
};

static const LockMethodData user_lockmethod = {
	MaxLockMode,
	LockConflicts,
	lock_mode_names,
#ifdef LOCK_DEBUG
	&Trace_userlocks
#else
	&Dummy_trace
#endif
};

/*
 * 从锁方法标识到锁表数据结构的映射
 */
static const LockMethod LockMethods[] = {
	NULL,
	&default_lockmethod,
	&user_lockmethod
};


/* 把锁持久化时写入两阶段提交状态文件的记录 */
typedef struct TwoPhaseLockRecord
{
	LOCKTAG		locktag;
	LOCKMODE	lockmode;
} TwoPhaseLockRecord;


/*
 * 我们以为正在使用的快速路径锁槽个数。若另一个后端
 * 已把我们的锁转移到主锁表，这个数也许高于实际值，
 * 但绝不会低于实际值，因为只有我们自己才能
 * 代表自己去获取锁。
 */
static int	FastPathLocalUseCount = 0;

/*
 * 标志：本后端是否正持有关系扩展锁。
 * 该标志用来保证：持有关系扩展锁期间，
 * 我们不会再去获取任何其他对象上的重量级锁。这一
 * 限制意味着关系扩展锁永远不会参与
 * 死锁环，因为取得这把锁之后，我们绝不会再等待任何其他重量级
 * 锁。
 *
 * 对关系扩展锁来说，这样的限制是可以接受的，因为它们与其他
 * 重量级锁不同，并不会一直持有到事务结束。取得它们只是为了
 * 在短时间内扩展某个特定关系，然后
 * 随即释放。
 */
static bool IsRelationExtensionLockHeld PG_USED_FOR_ASSERTS_ONLY = false;

/* 操作 proc->fpLockBits 的宏 */
#define FAST_PATH_BITS_PER_SLOT			3
#define FAST_PATH_LOCKNUMBER_OFFSET		1
#define FAST_PATH_MASK					((1 << FAST_PATH_BITS_PER_SLOT) - 1)
#define FAST_PATH_GET_BITS(proc, n) \
	(((proc)->fpLockBits >> (FAST_PATH_BITS_PER_SLOT * n)) & FAST_PATH_MASK)
#define FAST_PATH_BIT_POSITION(n, l) \
	(AssertMacro((l) >= FAST_PATH_LOCKNUMBER_OFFSET), \
	 AssertMacro((l) < FAST_PATH_BITS_PER_SLOT+FAST_PATH_LOCKNUMBER_OFFSET), \
	 AssertMacro((n) < FP_LOCK_SLOTS_PER_BACKEND), \
	 ((l) - FAST_PATH_LOCKNUMBER_OFFSET + FAST_PATH_BITS_PER_SLOT * (n)))
#define FAST_PATH_SET_LOCKMODE(proc, n, l) \
	 (proc)->fpLockBits |= UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l)
#define FAST_PATH_CLEAR_LOCKMODE(proc, n, l) \
	 (proc)->fpLockBits &= ~(UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l))
#define FAST_PATH_CHECK_LOCKMODE(proc, n, l) \
	 ((proc)->fpLockBits & (UINT64CONST(1) << FAST_PATH_BIT_POSITION(n, l)))

/*
 * 快速路径锁机制只关心绑定到某个数据库的后端
 * 在非共享关系上的关系锁。该机制的存在主要是为了加快
 * 那些很少发生冲突的锁的获取与释放。因为 ShareUpdateExclusiveLock
 * 会与自身冲突，所以不能使用快速路径机制；但它也不与任何
 * 可以使用该机制的锁相冲突，因此我们可以
 * 完全忽略它。
 */
#define EligibleForRelationFastPath(locktag, mode) \
	((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD && \
	(locktag)->locktag_type == LOCKTAG_RELATION && \
	(locktag)->locktag_field1 == MyDatabaseId && \
	MyDatabaseId != InvalidOid && \
	(mode) < ShareUpdateExclusiveLock)
#define ConflictsWithRelationFastPath(locktag, mode) \
	((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD && \
	(locktag)->locktag_type == LOCKTAG_RELATION && \
	(locktag)->locktag_field1 != InvalidOid && \
	(mode) > ShareUpdateExclusiveLock)

static bool FastPathGrantRelationLock(Oid relid, LOCKMODE lockmode);
static bool FastPathUnGrantRelationLock(Oid relid, LOCKMODE lockmode);
static bool FastPathTransferRelationLocks(LockMethod lockMethodTable,
										  const LOCKTAG *locktag, uint32 hashcode);
static PROCLOCK *FastPathGetRelationLockEntry(LOCALLOCK *locallock);

/*
 * 要让快速路径锁机制奏效，必须有办法在可能存在
 * 冲突锁时阻止使用快速路径。我们把 locktag 空间划分成
 * FAST_PATH_STRONG_LOCK_HASH_PARTITIONS 个分区，
 * 并在每个分区中维护 "强" 锁持有者的整数计数。
 * 只要存在任何 "强" 锁持有者（希望这并不常见），
 * 就不能使用快速路径机制，而必须退回到较慢的方法，
 * 把匹配的锁直接推进
 * 主锁表。
 *
 * 死锁检测器对快速路径机制一无所知，
 * 因此任何可能卷入死锁的锁，都必须从
 * 快速路径队列转移到主锁表。
 */

#define FAST_PATH_STRONG_LOCK_HASH_BITS			10
#define FAST_PATH_STRONG_LOCK_HASH_PARTITIONS \
	(1 << FAST_PATH_STRONG_LOCK_HASH_BITS)
#define FastPathStrongLockHashPartition(hashcode) \
	((hashcode) % FAST_PATH_STRONG_LOCK_HASH_PARTITIONS)

typedef struct
{
	slock_t		mutex;
	uint32		count[FAST_PATH_STRONG_LOCK_HASH_PARTITIONS];
} FastPathStrongRelationLockData;

static volatile FastPathStrongRelationLockData *FastPathStrongRelationLocks;


/*
 * 指向存放锁状态的哈希表的指针
 *
 * LockMethodLockHash 与 LockMethodProcLockHash 哈希表位于
 * 共享内存中；LockMethodLocalHash 则是每个后端本地的。
 */
static HTAB *LockMethodLockHash;
static HTAB *LockMethodProcLockHash;
static HTAB *LockMethodLocalHash;


/* 供错误清理使用的私有状态 */
static LOCALLOCK *StrongLockInProgress;
static LOCALLOCK *awaitedLock;
static ResourceOwner awaitedOwner;


#ifdef LOCK_DEBUG

/*------
 * 可供锁调试使用的配置选项如下：
 *
 *	   TRACE_LOCKS		-- 大量输出本文件中正在发生的事情
 *	   TRACE_USERLOCKS	-- 同上，但针对用户锁
 *	   TRACE_LOCK_OIDMIN-- 不跟踪 oid 低于此值的表上的锁
 *						   (用于避免系统表上的输出)
 *	   TRACE_LOCK_TABLE -- 无条件跟踪此表（oid）上的锁
 *	   DEBUG_DEADLOCKS	-- 目前会在不合时宜的场合转储锁
 *
 * 此外，在 storage/lmgr/lwlock.c 中还有：
 *	   TRACE_LWLOCKS	-- 跟踪轻量级锁（相当没用）
 *
 * 编译时定义 LOCK_DEBUG 即可启用上述全部选项。
 * --------
 */

int			Trace_lock_oidmin = FirstNormalObjectId;
bool		Trace_locks = false;
bool		Trace_userlocks = false;
int			Trace_lock_table = 0;
bool		Debug_deadlocks = false;


inline static bool
LOCK_DEBUG_ENABLED(const LOCKTAG *tag)
{
	return
		(*(LockMethods[tag->locktag_lockmethodid]->trace_flag) &&
		 ((Oid) tag->locktag_field2 >= (Oid) Trace_lock_oidmin))
		|| (Trace_lock_table &&
			(tag->locktag_field2 == Trace_lock_table));
}


inline static void
LOCK_PRINT(const char *where, const LOCK *lock, LOCKMODE type)
{
	if (LOCK_DEBUG_ENABLED(&lock->tag))
		elog(LOG,
			 "%s: lock(%p) id(%u,%u,%u,%u,%u,%u) grantMask(%x) "
			 "req(%d,%d,%d,%d,%d,%d,%d)=%d "
			 "grant(%d,%d,%d,%d,%d,%d,%d)=%d wait(%d) type(%s)",
			 where, lock,
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3, lock->tag.locktag_field4,
			 lock->tag.locktag_type, lock->tag.locktag_lockmethodid,
			 lock->grantMask,
			 lock->requested[1], lock->requested[2], lock->requested[3],
			 lock->requested[4], lock->requested[5], lock->requested[6],
			 lock->requested[7], lock->nRequested,
			 lock->granted[1], lock->granted[2], lock->granted[3],
			 lock->granted[4], lock->granted[5], lock->granted[6],
			 lock->granted[7], lock->nGranted,
			 dclist_count(&lock->waitProcs),
			 LockMethods[LOCK_LOCKMETHOD(*lock)]->lockModeNames[type]);
}


inline static void
PROCLOCK_PRINT(const char *where, const PROCLOCK *proclockP)
{
	if (LOCK_DEBUG_ENABLED(&proclockP->tag.myLock->tag))
		elog(LOG,
			 "%s: proclock(%p) lock(%p) method(%u) proc(%p) hold(%x)",
			 where, proclockP, proclockP->tag.myLock,
			 PROCLOCK_LOCKMETHOD(*(proclockP)),
			 proclockP->tag.myProc, (int) proclockP->holdMask);
}
#else							/* 非 LOCK_DEBUG */

#define LOCK_PRINT(where, lock, type)  ((void) 0)
#define PROCLOCK_PRINT(where, proclockP)  ((void) 0)
#endif							/* 非 LOCK_DEBUG */


static uint32 proclock_hash(const void *key, Size keysize);
static void RemoveLocalLock(LOCALLOCK *locallock);
static PROCLOCK *SetupLockInTable(LockMethod lockMethodTable, PGPROC *proc,
								  const LOCKTAG *locktag, uint32 hashcode, LOCKMODE lockmode);
static void GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner);
static void BeginStrongLockAcquire(LOCALLOCK *locallock, uint32 fasthashcode);
static void FinishStrongLockAcquire(void);
static void WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner);
static void ReleaseLockIfHeld(LOCALLOCK *locallock, bool sessionLock);
static void LockReassignOwner(LOCALLOCK *locallock, ResourceOwner parent);
static bool UnGrantLock(LOCK *lock, LOCKMODE lockmode,
						PROCLOCK *proclock, LockMethod lockMethodTable);
static void CleanUpLock(LOCK *lock, PROCLOCK *proclock,
						LockMethod lockMethodTable, uint32 hashcode,
						bool wakeupNeeded);
static void LockRefindAndRelease(LockMethod lockMethodTable, PGPROC *proc,
								 LOCKTAG *locktag, LOCKMODE lockmode,
								 bool decrement_strong_lock_count);
static void GetSingleProcBlockerStatusData(PGPROC *blocked_proc,
										   BlockedProcsData *data);


/*
 * InitLocks -- 初始化锁管理器的数据结构。
 *
 * 由 CreateSharedMemoryAndSemaphores() 调用，更多说明见该函数。
 * 在通常的 postmaster 情形下，共享哈希表在这里创建，
 * 同时还会创建一个 locallock 哈希表，它在 postmaster 自身中
 * 保持未使用且为空。后端经由 fork() 继承指向
 * 共享表的指针，同时也继承 locallock
 * 哈希表的一份映像，并随即使用它。在 EXEC_BACKEND 情形下，每个
 * 后端会重新执行这段代码，以取得指向已经存在的
 * 共享哈希表的指针，并创建自己的 locallock 哈希表。
 */
void
InitLocks(void)
{
	HASHCTL		info;
	long		init_table_size,
				max_table_size;
	bool		found;

	/*
	 * 计算为锁哈希表请求的初始大小与最大大小。注意这些
	 * 计算必须与 LockShmemSize 一致！
	 */
	max_table_size = NLOCKENTS();
	init_table_size = max_table_size / 2;

	/*
	 * 为 LOCK 结构体分配哈希表。这里存放每个被锁对象的
	 * 信息。
	 */
	info.keysize = sizeof(LOCKTAG);
	info.entrysize = sizeof(LOCK);
	info.num_partitions = NUM_LOCK_PARTITIONS;

	LockMethodLockHash = ShmemInitHash("LOCK hash",
									   init_table_size,
									   max_table_size,
									   &info,
									   HASH_ELEM | HASH_BLOBS | HASH_PARTITION);

	/* 假定每把锁平均有 2 个持有者 */
	max_table_size *= 2;
	init_table_size *= 2;

	/*
	 * 为 PROCLOCK 结构体分配哈希表。这里存放
	 * 每把锁、每个持有者的信息。
	 */
	info.keysize = sizeof(PROCLOCKTAG);
	info.entrysize = sizeof(PROCLOCK);
	info.hash = proclock_hash;
	info.num_partitions = NUM_LOCK_PARTITIONS;

	LockMethodProcLockHash = ShmemInitHash("PROCLOCK hash",
										   init_table_size,
										   max_table_size,
										   &info,
										   HASH_ELEM | HASH_FUNCTION | HASH_PARTITION);

	/*
	 * 分配快速路径结构。
	 */
	FastPathStrongRelationLocks =
		ShmemInitStruct("Fast Path Strong Relation Lock Data",
						sizeof(FastPathStrongRelationLockData), &found);
	if (!found)
		SpinLockInit(&FastPathStrongRelationLocks->mutex);

	/*
	 * 为 LOCALLOCK 结构体分配非共享哈希表。这里存放锁
	 * 计数以及资源所有者信息。
	 *
	 * 非共享表在本进程中也许已经存在（当 postmaster 在后端崩溃后
	 * 重建共享内存时就会这样）。若是如此，就删掉并重建它。
	 * （本来也可以留着，因为在 postmaster 里它应当是空的，
	 * 但为安全起见还是清掉。）
	 */
	if (LockMethodLocalHash)
		hash_destroy(LockMethodLocalHash);

	info.keysize = sizeof(LOCALLOCKTAG);
	info.entrysize = sizeof(LOCALLOCK);

	LockMethodLocalHash = hash_create("LOCALLOCK hash",
									  16,
									  &info,
									  HASH_ELEM | HASH_BLOBS);
}


/*
 * 取出与给定锁相关联的锁方法表
 */
LockMethod
GetLocksMethodTable(const LOCK *lock)
{
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*lock);

	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));
	return LockMethods[lockmethodid];
}

/*
 * 取出与给定 locktag 相关联的锁方法表
 */
LockMethod
GetLockTagsMethodTable(const LOCKTAG *locktag)
{
	LOCKMETHODID lockmethodid = (LOCKMETHODID) locktag->locktag_lockmethodid;

	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));
	return LockMethods[lockmethodid];
}


/*
 * 计算与某个 LOCKTAG 相关联的哈希码。
 *
 * 为避免不必要地反复计算哈希码，我们尽量在每个函数里
 * 只算一次，然后按需要传递。除了把哈希码传给
 * hash_search_with_hash_value() 之外，还可以从哈希码中
 * 提取锁分区号。
 */
uint32
LockTagHashCode(const LOCKTAG *locktag)
{
	return get_hash_value(LockMethodLockHash, (const void *) locktag);
}

/*
 * 计算与某个 PROCLOCKTAG 相关联的哈希码。
 *
 * 因为我们希望 LOCKs 与 PROCLOCKs 哈希表共用同一组
 * 分区锁，所以必须保证 PROCLOCK 落入与其
 * 关联 LOCK 相同的分区号。
 * dynahash.c 期望分区号是哈希码的低位，
 * 因此 PROCLOCKTAG 的哈希码必须与关联 LOCKTAG 的
 * 哈希码具有相同的低位。我们用这个
 * 专门的哈希函数来实现这一点。
 */
static uint32
proclock_hash(const void *key, Size keysize)
{
	const PROCLOCKTAG *proclocktag = (const PROCLOCKTAG *) key;
	uint32		lockhash;
	Datum		procptr;

	Assert(keysize == sizeof(PROCLOCKTAG));

	/* 查看相关联的 LOCK 对象，并计算其哈希码 */
	lockhash = LockTagHashCode(&proclocktag->myLock->tag);

	/*
	 * 为了让哈希码也依赖于 PGPROC，我们把 proc
	 * 结构体的地址异或进哈希码，并左移，使
	 * 分区号所在的位不变。既然这只是哈希，
	 * 丢掉地址的高位也没关系；用一个
	 * 中间变量来抑制指针转换为整数的警告。
	 */
	procptr = PointerGetDatum(proclocktag->myProc);
	lockhash ^= ((uint32) procptr) << LOG2_NUM_LOCK_PARTITIONS;

	return lockhash;
}

/*
 * 在已知其底层 LOCK 的哈希码时，
 * 计算与 PROCLOCKTAG 相关联的哈希码。
 *
 * 这样做只是为了避免重复调用 LockTagHashCode()。
 */
static inline uint32
ProcLockHashCode(const PROCLOCKTAG *proclocktag, uint32 hashcode)
{
	uint32		lockhash = hashcode;
	Datum		procptr;

	/*
	 * 这里必须与 proclock_hash() 一致！
	 */
	procptr = PointerGetDatum(proclocktag->myProc);
	lockhash ^= ((uint32) procptr) << LOG2_NUM_LOCK_PARTITIONS;

	return lockhash;
}

/*
 * 给定两种锁模式，返回它们是否会冲突。
 */
bool
DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2)
{
	LockMethod	lockMethodTable = LockMethods[DEFAULT_LOCKMETHOD];

	if (lockMethodTable->conflictTab[mode1] & LOCKBIT_ON(mode2))
		return true;

	return false;
}

/*
 * LockHeldByMeExtended -- 测试锁 'locktag' 是否由当前
 *		事务持有
 *
 * 若当前事务以 'lockmode' 模式持有 'tag' 上的锁，则返回真。
 * 若 'orstronger' 为真，更强的锁模式也可以。
 * （"更强" 定义为 "数值更大"，语义上有些
 * 可疑，但对我们的用途来说没问题。）
 */
static bool
LockHeldByMeExtended(const LOCKTAG *locktag,
					 LOCKMODE lockmode, bool orstronger)
{
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;

	/*
	 * 查看是否已有此锁和锁模式的 LOCALLOCK 项
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* 必须清掉填充字节 */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	if (locallock && locallock->nLocks > 0)
		return true;

	if (orstronger)
	{
		LOCKMODE	slockmode;

		for (slockmode = lockmode + 1;
			 slockmode <= MaxLockMode;
			 slockmode++)
		{
			if (LockHeldByMeExtended(locktag, slockmode, false))
				return true;
		}
	}

	return false;
}

bool
LockHeldByMe(const LOCKTAG *locktag, LOCKMODE lockmode)
{
	return LockHeldByMeExtended(locktag, lockmode, false);
}

bool
LockOrStrongerHeldByMe(const LOCKTAG *locktag, LOCKMODE lockmode)
{
	return LockHeldByMeExtended(locktag, lockmode, true);
}

#ifdef USE_ASSERT_CHECKING
/*
 * GetLockMethodLocalHash -- 返回本地锁的哈希表，供那些
 *		根据所持全部锁来做断言的模块使用。
 */
HTAB *
GetLockMethodLocalHash(void)
{
	return LockMethodLocalHash;
}
#endif

/*
 * LockHasWaiters -- 查找 'locktag'，并检查释放这把
 *		锁是否会唤醒正在等待它的其他进程。
 */
bool
LockHasWaiters(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLock	   *partitionLock;
	bool		hasWaiters = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockHasWaiters: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/*
	 * 找出此锁和锁模式对应的 LOCALLOCK 项
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* 必须清掉填充字节 */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	/*
	 * 也让调用方打印自己的错误消息。不要 ereport(ERROR)。
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return false;
	}

	/*
	 * 检查共享锁表。
	 */
	partitionLock = LockHashPartitionLock(locallock->hashcode);

	LWLockAcquire(partitionLock, LW_SHARED);

	/*
	 * 不必重新查找 lock 或 proclock，因为我们在 locallock 表里
	 * 保存了它们的地址，而且只要我们持有其上的锁，
	 * 它们就不会被移除。
	 */
	lock = locallock->lock;
	LOCK_PRINT("LockHasWaiters: found", lock, lockmode);
	proclock = locallock->proclock;
	PROCLOCK_PRINT("LockHasWaiters: found", proclock);

	/*
	 * 再确认一次：我们确实持有想要
	 * 释放的那种类型的锁。
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("LockHasWaiters: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		RemoveLocalLock(locallock);
		return false;
	}

	/*
	 * 执行检查。
	 */
	if ((lockMethodTable->conflictTab[lockmode] & lock->waitMask) != 0)
		hasWaiters = true;

	LWLockRelease(partitionLock);

	return hasWaiters;
}

/*
 * LockAcquire -- 检查锁冲突，若发现冲突则睡眠，
 *		在没有冲突时设置锁。
 *
 * 输入：
 *	locktag：可锁对象的唯一标识
 *	lockmode：要获取的锁模式
 *	sessionLock：若为真，则为会话而不是当前事务获取锁
 *	dontWait：若为真，则不等待获取锁
 *
 * 返回下列之一：
 *		LOCKACQUIRE_NOT_AVAIL		锁不可用，且 dontWait=true
 *		LOCKACQUIRE_OK				锁已成功获取
 *		LOCKACQUIRE_ALREADY_HELD	锁已持有，计数已增加
 *		LOCKACQUIRE_ALREADY_CLEAR	锁已处于 clear 状态，计数已增加
 *
 * 在通常情形下 dontWait=false，而且调用方不需要
 * 区分新获取的锁与本事务早先已经取得的锁，
 * 此时不必查看返回值。
 *
 * 副作用：锁被获取，并记入锁表。
 *
 * 注意：若我们等待这把锁，除了中止事务之外，
 * 没有办法中止这次等待。
 */
LockAcquireResult
LockAcquire(const LOCKTAG *locktag,
			LOCKMODE lockmode,
			bool sessionLock,
			bool dontWait)
{
	return LockAcquireExtended(locktag, lockmode, sessionLock, dontWait,
							   true, NULL);
}

/*
 * LockAcquireExtended - 允许我们指定额外选项
 *
 * reportMemoryError 指定：把锁表填满的锁请求
 * 是否应当产生 ERROR。传入 "false" 允许调用方
 * 尝试从锁表已满的情形中恢复，例如强制
 * 取消其他锁持有者然后重试。不过请注意，此时的
 * 返回码是 LOCKACQUIRE_NOT_AVAIL，因此把它与
 * dontWait = true 一起使用是不安全的，因为无法区分
 * 失败原因。
 *
 * 若 locallockp 不是 NULL，成功取得锁时 *locallockp 收到指向 LOCALLOCK
 * 表项的指针，否则收到 NULL。
 */
LockAcquireResult
LockAcquireExtended(const LOCKTAG *locktag,
					LOCKMODE lockmode,
					bool sessionLock,
					bool dontWait,
					bool reportMemoryError,
					LOCALLOCK **locallockp)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	bool		found;
	ResourceOwner owner;
	uint32		hashcode;
	LWLock	   *partitionLock;
	bool		found_conflict;
	bool		log_lock = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

	if (RecoveryInProgress() && !InRecovery &&
		(locktag->locktag_type == LOCKTAG_OBJECT ||
		 locktag->locktag_type == LOCKTAG_RELATION) &&
		lockmode > RowExclusiveLock)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot acquire lock mode %s on database objects while recovery is in progress",
						lockMethodTable->lockModeNames[lockmode]),
				 errhint("Only RowExclusiveLock or less can be acquired on database objects during recovery.")));

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockAcquire: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/* 确定锁的所有者 */
	if (sessionLock)
		owner = NULL;
	else
		owner = CurrentResourceOwner;

	/*
	 * 找出或创建此锁和锁模式的 LOCALLOCK 项
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* 必须清掉填充字节 */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_ENTER, &found);

	/*
	 * 若是新建的 locallock 对象，则初始化它
	 */
	if (!found)
	{
		locallock->lock = NULL;
		locallock->proclock = NULL;
		locallock->hashcode = LockTagHashCode(&(localtag.lock));
		locallock->nLocks = 0;
		locallock->holdsStrongLockCount = false;
		locallock->lockCleared = false;
		locallock->numLockOwners = 0;
		locallock->maxLockOwners = 8;
		locallock->lockOwners = NULL;	/* 以防下一行失败 */
		locallock->lockOwners = (LOCALLOCKOWNER *)
			MemoryContextAlloc(TopMemoryContext,
							   locallock->maxLockOwners * sizeof(LOCALLOCKOWNER));
	}
	else
	{
		/* 确保有地方记住这把锁 */
		if (locallock->numLockOwners >= locallock->maxLockOwners)
		{
			int			newsize = locallock->maxLockOwners * 2;

			locallock->lockOwners = (LOCALLOCKOWNER *)
				repalloc(locallock->lockOwners,
						 newsize * sizeof(LOCALLOCKOWNER));
			locallock->maxLockOwners = newsize;
		}
	}
	hashcode = locallock->hashcode;

	if (locallockp)
		*locallockp = locallock;

	/*
	 * 若我们已经持有该锁，只需在本地增加计数。
	 *
	 * 若 lockCleared 已经置位，调用方不必操心去吸收
	 * 与该锁对象相关的 sinval 消息。
	 */
	if (locallock->nLocks > 0)
	{
		GrantLockLocal(locallock, owner);
		if (locallock->lockCleared)
			return LOCKACQUIRE_ALREADY_CLEAR;
		else
			return LOCKACQUIRE_ALREADY_HELD;
	}

	/*
	 * 持有关系扩展锁时，我们不再获取任何其他重量级锁。
	 * 允许多次获取同一把关系扩展
	 * 锁，但那种情形到不了这里。
	 */
	Assert(!IsRelationExtensionLockHeld);

	/*
	 * 若获取这把锁需要在备库上重放，
	 * 就准备发出一条 WAL 记录。
	 *
	 * 这里先做记录准备；取得锁之后再真正发出日志记录。
	 * 这样安排可以简化准备步骤失败时的
	 * 错误恢复。
	 *
	 * 只有 AccessExclusiveLocks 会与只读
	 * 事务在备库上可以获取的锁类型冲突。请确保此定义
	 * 与 GetRunningTransactionLocks() 中的定义一致。
	 */
	if (lockmode >= AccessExclusiveLock &&
		locktag->locktag_type == LOCKTAG_RELATION &&
		!RecoveryInProgress() &&
		XLogStandbyInfoActive())
	{
		LogAccessExclusiveLockPrepare();
		log_lock = true;
	}

	/*
	 * 若符合条件，就尝试经由快速路径取得锁。但如果我们记得
	 * 快速路径数组已经填满，那么在释放一些锁之前，
	 * 就不再尝试使用它。其他后端也许已经把其中一些
	 * 锁转移到了共享哈希表，从而腾出了空位，但仅仅为了
	 * 检查这一点而去获取 LWLock 并不值得。我们也有可能正在获取
	 * 已经用快速路径锁住的关系上的第二种或第三种
	 * 锁类型，不过目前
	 * 也不处理那种情形。
	 */
	if (EligibleForRelationFastPath(locktag, lockmode) &&
		FastPathLocalUseCount < FP_LOCK_SLOTS_PER_BACKEND)
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);
		bool		acquired;

		/*
		 * LWLockAcquire 充当内存定序点，因此可以安全地
		 * 假定：任何强锁持有者若其对
		 * FastPathStrongRelationLocks->counts 的增量在我们检查之后才可见，
		 * 则它尚未开始转移快速路径锁。
		 */
		LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
		if (FastPathStrongRelationLocks->count[fasthashcode] != 0)
			acquired = false;
		else
			acquired = FastPathGrantRelationLock(locktag->locktag_field2,
												 lockmode);
		LWLockRelease(&MyProc->fpInfoLock);
		if (acquired)
		{
			/*
			 * locallock 里也许留着指向某些旧共享
			 * 对象的过期指针；在认定该锁已经由快速路径
			 * 取得之前，我们必须把这些指针重置为 null。
			 */
			locallock->lock = NULL;
			locallock->proclock = NULL;
			GrantLockLocal(locallock, owner);
			return LOCKACQUIRE_OK;
		}
	}

	/*
	 * 若其他后端有可能已经经由快速路径取得了这把锁，
	 * 我们就必须（暂时）禁止对此 lock tag 继续使用
	 * 快速路径，并把已经用此方法取得的锁迁移到
	 * 主锁表。
	 */
	if (ConflictsWithRelationFastPath(locktag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		BeginStrongLockAcquire(locallock, fasthashcode);
		if (!FastPathTransferRelationLocks(lockMethodTable, locktag,
										   hashcode))
		{
			AbortStrongLockAcquire();
			if (locallock->nLocks == 0)
				RemoveLocalLock(locallock);
			if (locallockp)
				*locallockp = NULL;
			if (reportMemoryError)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory"),
						 errhint("You might need to increase %s.", "max_locks_per_transaction")));
			else
				return LOCKACQUIRE_NOT_AVAIL;
		}
	}

	/*
	 * 在 LOCALLOCK 表里没有找到这把锁，经由快速路径也没能
	 * 取得它，因此不得不去碰
	 * 共享锁表。
	 */
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 找出或创建带有此标签的 lock 与 proclock 项
	 *
	 * 注意：若 locallock 对象已经存在，它也许已经有指向
	 * 该锁的指针……但不应假定该指针仍然
	 * 有效，因为持有计数和请求计数都为零的锁对象随时可能
	 * 消失。因此即便这些指针已经设置，也必须用 SetupLockInTable()
	 * 重新计算 lock 与 proclock 指针。
	 */
	proclock = SetupLockInTable(lockMethodTable, MyProc, locktag,
								hashcode, lockmode);
	if (!proclock)
	{
		AbortStrongLockAcquire();
		LWLockRelease(partitionLock);
		if (locallock->nLocks == 0)
			RemoveLocalLock(locallock);
		if (locallockp)
			*locallockp = NULL;
		if (reportMemoryError)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase %s.", "max_locks_per_transaction")));
		else
			return LOCKACQUIRE_NOT_AVAIL;
	}
	locallock->proclock = proclock;
	lock = proclock->tag.myLock;
	locallock->lock = lock;

	/*
	 * 若所请求的锁与等待者请求的锁冲突，就必须加入
	 * 等待队列。否则，检查是否与已经持有的锁冲突。
	 * （放在最后，因为这是最复杂的检查。）
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		found_conflict = true;
	else
		found_conflict = LockCheckConflicts(lockMethodTable, lockmode,
											lock, proclock);

	if (!found_conflict)
	{
		/* 与已持有或先前请求的锁都没有冲突 */
		GrantLock(lock, proclock, lockmode);
		GrantLockLocal(locallock, owner);
	}
	else
	{
		/*
		 * 无法立刻取得锁。若调用方指定了不
		 * 阻塞，就去掉无用的表项并返回
		 * LOCKACQUIRE_NOT_AVAIL，不再等待。
		 */
		if (dontWait)
		{
			AbortStrongLockAcquire();
			if (proclock->holdMask == 0)
			{
				uint32		proclock_hashcode;

				proclock_hashcode = ProcLockHashCode(&proclock->tag, hashcode);
				dlist_delete(&proclock->lockLink);
				dlist_delete(&proclock->procLink);
				if (!hash_search_with_hash_value(LockMethodProcLockHash,
												 &(proclock->tag),
												 proclock_hashcode,
												 HASH_REMOVE,
												 NULL))
					elog(PANIC, "proclock table corrupted");
			}
			else
				PROCLOCK_PRINT("LockAcquire: NOWAIT", proclock);
			lock->nRequested--;
			lock->requested[lockmode]--;
			LOCK_PRINT("LockAcquire: conditional lock failed", lock, lockmode);
			Assert((lock->nRequested > 0) && (lock->requested[lockmode] >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LWLockRelease(partitionLock);
			if (locallock->nLocks == 0)
				RemoveLocalLock(locallock);
			if (locallockp)
				*locallockp = NULL;
			return LOCKACQUIRE_NOT_AVAIL;
		}

		/*
		 * 设置本进程在此对象上已经持有的锁的位掩码。
		 */
		MyProc->heldLocks = proclock->holdMask;

		/*
		 * 睡眠，直到有人把我唤醒。
		 */

		TRACE_POSTGRESQL_LOCK_WAIT_START(locktag->locktag_field1,
										 locktag->locktag_field2,
										 locktag->locktag_field3,
										 locktag->locktag_field4,
										 locktag->locktag_type,
										 lockmode);

		WaitOnLock(locallock, owner);

		TRACE_POSTGRESQL_LOCK_WAIT_DONE(locktag->locktag_field1,
										locktag->locktag_field2,
										locktag->locktag_field3,
										locktag->locktag_field4,
										locktag->locktag_type,
										lockmode);

		/*
		 * 注意：从这里到返回之间，不要对状态做任何实质性修改。
		 * 锁表状态所需的全部变更，都必须在锁授予我们时
		 * 已经完成 --- 见 WaitOnLock 中的说明。
		 */

		/*
		 * 检查 proclock 项的状态，以防进程间
		 * 通信有某处工作不正常。
		 */
		if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
		{
			AbortStrongLockAcquire();
			PROCLOCK_PRINT("LockAcquire: INCONSISTENT", proclock);
			LOCK_PRINT("LockAcquire: INCONSISTENT", lock, lockmode);
			/* 要重试吗？ */
			LWLockRelease(partitionLock);
			elog(ERROR, "LockAcquire failed");
		}
		PROCLOCK_PRINT("LockAcquire: granted", proclock);
		LOCK_PRINT("LockAcquire: granted", lock, lockmode);
	}

	/*
	 * 锁状态现在已经完全是最新的；若此后出错，
	 * 不需要专门的错误清理。
	 */
	FinishStrongLockAcquire();

	LWLockRelease(partitionLock);

	/*
	 * 若获取这把锁需要在备库上重放，
	 * 就发出一条 WAL 记录。
	 */
	if (log_lock)
	{
		/*
		 * 把 locktag 解码回原来的值，以免每条消息都发送
		 * 大量空字节。LOCKTAG_RELATION 如何定义 locktag，
		 * 见 lock.h。
		 */
		LogAccessExclusiveLock(locktag->locktag_field1,
							   locktag->locktag_field2);
	}

	return LOCKACQUIRE_OK;
}

/*
 * 按新锁请求的需要，找出或创建
 * LOCK 与 PROCLOCK 对象。
 *
 * 返回 PROCLOCK 对象；若因共享内存不足
 * 而未能创建这些对象，则返回 NULL。
 *
 * 进入时必须持有相应的分区锁，退出时
 * 仍然持有。
 */
static PROCLOCK *
SetupLockInTable(LockMethod lockMethodTable, PGPROC *proc,
				 const LOCKTAG *locktag, uint32 hashcode, LOCKMODE lockmode)
{
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	uint32		proclock_hashcode;
	bool		found;

	/*
	 * 找出或创建带有此标签的锁。
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
		return NULL;

	/*
	 * 若是新建的锁对象，则初始化它
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		dlist_init(&lock->procLocks);
		dclist_init(&lock->waitProcs);
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet(lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet(lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
		LOCK_PRINT("LockAcquire: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("LockAcquire: found", lock, lockmode);
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}

	/*
	 * 为 proclock 表创建哈希键。
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * 找出或创建带有此标签的 proclock 项
	 */
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														&proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* 糟糕，proclock 的共享内存不够 */
		if (lock->nRequested == 0)
		{
			/*
			 * 没有其他请求者在请求这把锁，因此回收
			 * 该锁对象。我们必须这样做，以免共享内存
			 * 永久泄漏，因为此后不会再有任何事情
			 * 促使别人释放这个锁对象。
			 */
			Assert(dlist_is_empty(&(lock->procLocks)));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		return NULL;
	}

	/*
	 * 若是新建的，则初始化新项
	 */
	if (!found)
	{
		uint32		partition = LockHashPartition(hashcode);

		/*
		 * 不持锁就访问 proclock->groupLeader 看似不安全，
		 * 其实并非如此。要么我们正在为自己
		 * 初始化一个 proclock，此时组长不会改变，
		 * 因为进程的组长只能由该进程自己更改；
		 * 要么我们正在把一把快速路径锁转移到主锁表，
		 * 此时该进程若不先释放自己的全部锁
		 * （尤其是我们正在转移的这一把），就无法更改
		 * 自己的锁组组长。
		 */
		proclock->groupLeader = proc->lockGroupLeader != NULL ?
			proc->lockGroupLeader : proc;
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* 把 proclock 加入相应的链表 */
		dlist_push_tail(&lock->procLocks, &proclock->lockLink);
		dlist_push_tail(&proc->myProcLocks[partition], &proclock->procLink);
		PROCLOCK_PRINT("LockAcquire: new", proclock);
	}
	else
	{
		PROCLOCK_PRINT("LockAcquire: found", proclock);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);

#ifdef CHECK_DEADLOCK_RISK

		/*
		 * 若我们已经在此对象上持有更低级别的锁，
		 * 却没有持有所请求级别或更高级别的锁，就发出警告。这
		 * 表明一种容易死锁的编码习惯（例如，若另一个后端
		 * 几乎同时走同一条代码路径，就会发生
		 * 死锁）。
		 *
		 * 默认不启用，因为它也许会为实际上在语境中安全的
		 * 用户级编码习惯产生日志项。
		 * 可以启用它来帮助查找系统级问题。
		 *
		 * XXX 对锁模式做数值比较是一种权宜之计；用一张
		 * 表会更好。不过目前这样也能工作。
		 */
		{
			int			i;

			for (i = lockMethodTable->numLockModes; i > 0; i--)
			{
				if (proclock->holdMask & LOCKBIT_ON(i))
				{
					if (i >= (int) lockmode)
						break;	/* 安全：我们持有不低于所请求级别的锁 */
					elog(LOG, "deadlock risk: raising lock level"
						 " from %s to %s on object %u/%u/%u",
						 lockMethodTable->lockModeNames[i],
						 lockMethodTable->lockModeNames[lockmode],
						 lock->tag.locktag_field1, lock->tag.locktag_field2,
						 lock->tag.locktag_field3);
					break;
				}
			}
		}
#endif							/* CHECK_DEADLOCK_RISK（检查死锁风险） */
	}

	/*
	 * lock->nRequested 与 lock->requested[] 统计请求总数，
	 * 无论已授予还是在等待，因此立刻增加它们。
	 * 其他计数要等到我们取得锁之后才增加。
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * 我们不应已经持有所要的锁；否则 locallock 表
	 * 就坏了。
	 */
	if (proclock->holdMask & LOCKBIT_ON(lockmode))
		elog(ERROR, "lock %s on object %u/%u/%u is already held",
			 lockMethodTable->lockModeNames[lockmode],
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3);

	return proclock;
}

/*
 * 检查并设置或复位我们持有关系扩展锁的标志。
 *
 * 调用方负责在获取或释放关系扩展锁之后
 * 调用本函数。
 *
 * 若已取得锁则传入 acquired 为真，否则为假。
 */
static inline void
CheckAndSetLockHeld(LOCALLOCK *locallock, bool acquired)
{
#ifdef USE_ASSERT_CHECKING
	if (LOCALLOCK_LOCKTAG(*locallock) == LOCKTAG_RELATION_EXTEND)
		IsRelationExtensionLockHeld = acquired;
#endif
}

/*
 * 释放一个 locallock 项的子例程
 */
static void
RemoveLocalLock(LOCALLOCK *locallock)
{
	int			i;

	for (i = locallock->numLockOwners - 1; i >= 0; i--)
	{
		if (locallock->lockOwners[i].owner != NULL)
			ResourceOwnerForgetLock(locallock->lockOwners[i].owner, locallock);
	}
	locallock->numLockOwners = 0;
	if (locallock->lockOwners != NULL)
		pfree(locallock->lockOwners);
	locallock->lockOwners = NULL;

	if (locallock->holdsStrongLockCount)
	{
		uint32		fasthashcode;

		fasthashcode = FastPathStrongLockHashPartition(locallock->hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
		FastPathStrongRelationLocks->count[fasthashcode]--;
		locallock->holdsStrongLockCount = false;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}

	if (!hash_search(LockMethodLocalHash,
					 &(locallock->tag),
					 HASH_REMOVE, NULL))
		elog(WARNING, "locallock table corrupted");

	/*
	 * 对某些类型的锁，表明该锁已释放
	 */
	CheckAndSetLockHeld(locallock, false);
}

/*
 * LockCheckConflicts -- 测试所请求的锁是否与
 *		已经授予的锁冲突
 *
 * 若冲突则返回真，否则返回假。
 *
 * 说明（NOTES）：
 *		使这件事变复杂的是：同一进程的锁彼此并不
 * 冲突，无论持有它们的目的是什么
 * （例如，会话锁与事务锁并不冲突）。同一锁组中
 * 一个进程的锁也不会与该组中另一个进程的锁冲突。
 * 因此，在判断新请求的锁是否与已持有的锁冲突时，
 * 必须先减掉这些锁。
 */
bool
LockCheckConflicts(LockMethod lockMethodTable,
				   LOCKMODE lockmode,
				   LOCK *lock,
				   PROCLOCK *proclock)
{
	int			numLockModes = lockMethodTable->numLockModes;
	LOCKMASK	myLocks;
	int			conflictMask = lockMethodTable->conflictTab[lockmode];
	int			conflictsRemaining[MAX_LOCKMODES];
	int			totalConflictsRemaining = 0;
	dlist_iter	proclock_iter;
	int			i;

	/*
	 * 先检查全局冲突：若没有任何锁与我的请求冲突，
	 * 我就得到这把锁。
	 *
	 * 冲突检查：lock->grantMask 表示当前
	 * 持有的锁类型。conflictTable[lockmode] 为每一种
	 * 与请求冲突的锁类型置一位。按位比较即可得知
	 * 是否存在冲突。
	 */
	if (!(conflictMask & lock->grantMask))
	{
		PROCLOCK_PRINT("LockCheckConflicts: no conflict", proclock);
		return false;
	}

	/*
	 * 真讨厌。有东西冲突了。但它仍可能是我自己的锁，或我的锁组中
	 * 另一成员持有的锁。首先算出减掉我自己持有的锁之后，
	 * 还剩下多少冲突。
	 */
	myLocks = proclock->holdMask;
	for (i = 1; i <= numLockModes; i++)
	{
		if ((conflictMask & LOCKBIT_ON(i)) == 0)
		{
			conflictsRemaining[i] = 0;
			continue;
		}
		conflictsRemaining[i] = lock->granted[i];
		if (myLocks & LOCKBIT_ON(i))
			--conflictsRemaining[i];
		totalConflictsRemaining += conflictsRemaining[i];
	}

	/* 若不再有冲突，我们就得到这把锁。 */
	if (totalConflictsRemaining == 0)
	{
		PROCLOCK_PRINT("LockCheckConflicts: resolved (simple)", proclock);
		return false;
	}

	/* 若没有组锁，那就肯定是冲突。 */
	if (proclock->groupLeader == MyProc && MyProc->lockGroupLeader == NULL)
	{
		Assert(proclock->tag.myProc == MyProc);
		PROCLOCK_PRINT("LockCheckConflicts: conflicting (simple)",
					   proclock);
		return true;
	}

	/*
	 * 关系扩展锁即便在组成员之间也会冲突。
	 */
	if (LOCK_LOCKTAG(*lock) == LOCKTAG_RELATION_EXTEND)
	{
		PROCLOCK_PRINT("LockCheckConflicts: conflicting (group)",
					   proclock);
		return true;
	}

	/*
	 * 我们自己的锁组成员以冲突模式持有的锁
	 * 并不是真正的冲突；可以把它们减掉，再看是否仍然
	 * 存在冲突。这相对于正在持有或
	 * 等待此对象上的锁的进程数是 O(N)。可以通过让
	 * 共享内存状态更复杂（也更大）来改进，但似乎
	 * 不值得。
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		PROCLOCK   *otherproclock =
			dlist_container(PROCLOCK, lockLink, proclock_iter.cur);

		if (proclock != otherproclock &&
			proclock->groupLeader == otherproclock->groupLeader &&
			(otherproclock->holdMask & conflictMask) != 0)
		{
			int			intersectMask = otherproclock->holdMask & conflictMask;

			for (i = 1; i <= numLockModes; i++)
			{
				if ((intersectMask & LOCKBIT_ON(i)) != 0)
				{
					if (conflictsRemaining[i] <= 0)
						elog(PANIC, "proclocks held do not match lock");
					conflictsRemaining[i]--;
					totalConflictsRemaining--;
				}
			}

			if (totalConflictsRemaining == 0)
			{
				PROCLOCK_PRINT("LockCheckConflicts: resolved (group)",
							   proclock);
				return false;
			}
		}
	}

	/* 不，这是真正的冲突。 */
	PROCLOCK_PRINT("LockCheckConflicts: conflicting (group)", proclock);
	return true;
}

/*
 * GrantLock -- 更新 lock 与 proclock 数据结构，以表明
 *		锁请求已被授予。
 *
 * 注意：若 proc 曾被阻塞，还需要把它从等待链表中移除，
 * 并清掉它的 waitLock/waitProcLock 字段。这里不做这件事。
 *
 * 注意：锁的授予还必须记入相关联的 LOCALLOCK
 * 表项；但因为我们也许正在唤醒另一个进程，这里做不到；
 * 改由 GrantLockLocal 来做。
 */
void
GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode)
{
	lock->nGranted++;
	lock->granted[lockmode]++;
	lock->grantMask |= LOCKBIT_ON(lockmode);
	if (lock->granted[lockmode] == lock->requested[lockmode])
		lock->waitMask &= LOCKBIT_OFF(lockmode);
	proclock->holdMask |= LOCKBIT_ON(lockmode);
	LOCK_PRINT("GrantLock", lock, lockmode);
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);
}

/*
 * UnGrantLock -- GrantLock 的逆操作。
 *
 * 更新 lock 与 proclock 数据结构，以表明当前持有者
 * 不再持有、也不再请求这把锁。
 *
 * 若有等待者正在等待这把锁，并且现在应当用 ProcLockWakeup
 * 唤醒它们，则返回真。
 */
static bool
UnGrantLock(LOCK *lock, LOCKMODE lockmode,
			PROCLOCK *proclock, LockMethod lockMethodTable)
{
	bool		wakeupNeeded = false;

	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));
	Assert((lock->nGranted > 0) && (lock->granted[lockmode] > 0));
	Assert(lock->nGranted <= lock->nRequested);

	/*
	 * 修正总体的锁统计
	 */
	lock->nRequested--;
	lock->requested[lockmode]--;
	lock->nGranted--;
	lock->granted[lockmode]--;

	if (lock->granted[lockmode] == 0)
	{
		/* 更改冲突掩码。不再有这种锁类型。 */
		lock->grantMask &= LOCKBIT_OFF(lockmode);
	}

	LOCK_PRINT("UnGrantLock: updated", lock, lockmode);

	/*
	 * 只有当被释放的锁与waiter 所请求的至少一种
	 * 锁类型冲突时，才需要运行 ProcLockWakeup。否则，当初让他们
	 * 等待的冲突必然仍然存在。注意：在 MVCC 之前，若
	 * lock->granted[lockmode] 仍为正，就可以跳过唤醒。但现在
	 * 不再如此，因为剩余的已授予锁也许属于
	 * 某个等待者，而他现在可以被唤醒，因为他并不与
	 * 自己的锁冲突。
	 */
	if (lockMethodTable->conflictTab[lockmode] & lock->waitMask)
		wakeupNeeded = true;

	/*
	 * 现在修正每个 proclock 的状态。
	 */
	proclock->holdMask &= LOCKBIT_OFF(lockmode);
	PROCLOCK_PRINT("UnGrantLock: updated", proclock);

	return wakeupNeeded;
}

/*
 * CleanUpLock -- 释放锁之后做清理。在可能时回收
 * proclock 与 lock 对象，并在仍有剩余请求且调用方
 * 表示可以时调用 ProcLockWakeup。（通常应在 UnGrantLock
 * 之后调用本函数，而 wakeupNeeded 就是
 * UnGrantLock 的结果。）
 *
 * 进入时必须持有相应的分区锁，退出时
 * 仍然持有。
 */
static void
CleanUpLock(LOCK *lock, PROCLOCK *proclock,
			LockMethod lockMethodTable, uint32 hashcode,
			bool wakeupNeeded)
{
	/*
	 * 若这是我在此锁上的最后一次持有，就删除我在 proclock
	 * 表中的项。
	 */
	if (proclock->holdMask == 0)
	{
		uint32		proclock_hashcode;

		PROCLOCK_PRINT("CleanUpLock: deleting", proclock);
		dlist_delete(&proclock->lockLink);
		dlist_delete(&proclock->procLink);
		proclock_hashcode = ProcLockHashCode(&proclock->tag, hashcode);
		if (!hash_search_with_hash_value(LockMethodProcLockHash,
										 &(proclock->tag),
										 proclock_hashcode,
										 HASH_REMOVE,
										 NULL))
			elog(PANIC, "proclock table corrupted");
	}

	if (lock->nRequested == 0)
	{
		/*
		 * 调用方刚刚释放了最后一把锁，因此回收该
		 * 锁对象。
		 */
		LOCK_PRINT("CleanUpLock: deleting", lock, 0);
		Assert(dlist_is_empty(&lock->procLocks));
		if (!hash_search_with_hash_value(LockMethodLockHash,
										 &(lock->tag),
										 hashcode,
										 HASH_REMOVE,
										 NULL))
			elog(PANIC, "lock table corrupted");
	}
	else if (wakeupNeeded)
	{
		/* 这把锁上有等待者，因此唤醒他们。 */
		ProcLockWakeup(lockMethodTable, lock);
	}
}

/*
 * GrantLockLocal -- 更新 locallock 数据结构，以表明
 *		锁请求已被授予。
 *
 * 我们期望 LockAcquire 已经确保有地方新增一条
 * ResourceOwner 项。
 */
static void
GrantLockLocal(LOCALLOCK *locallock, ResourceOwner owner)
{
	LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
	int			i;

	Assert(locallock->numLockOwners < locallock->maxLockOwners);
	/* 累计总数 */
	locallock->nLocks++;
	/* 累计每个所有者的锁 */
	for (i = 0; i < locallock->numLockOwners; i++)
	{
		if (lockOwners[i].owner == owner)
		{
			lockOwners[i].nLocks++;
			return;
		}
	}
	lockOwners[i].owner = owner;
	lockOwners[i].nLocks = 1;
	locallock->numLockOwners++;
	if (owner != NULL)
		ResourceOwnerRememberLock(owner, locallock);

	/* 对某些类型的锁，表明该锁已取得。 */
	CheckAndSetLockHeld(locallock, true);
}

/*
 * BeginStrongLockAcquire - 对给定 LOCALLOCK 禁止使用快速路径，
 * 并在失败时安排错误清理
 */
static void
BeginStrongLockAcquire(LOCALLOCK *locallock, uint32 fasthashcode)
{
	Assert(StrongLockInProgress == NULL);
	Assert(locallock->holdsStrongLockCount == false);

	/*
	 * 对内存位置做加法并不是原子的，因此我们拿一把自旋锁，
	 * 以确保不会与同时试图增加该计数的其他人
	 * 撞在一起。
	 *
	 * XXX：在支持原子取并加指令的体系结构上，
	 * 也许值得考虑改用它。
	 */

	SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
	FastPathStrongRelationLocks->count[fasthashcode]++;
	locallock->holdsStrongLockCount = true;
	StrongLockInProgress = locallock;
	SpinLockRelease(&FastPathStrongRelationLocks->mutex);
}

/*
 * FinishStrongLockAcquire - 一旦不再需要，就取消强锁
 * 获取的待处理清理
 */
static void
FinishStrongLockAcquire(void)
{
	StrongLockInProgress = NULL;
}

/*
 * AbortStrongLockAcquire - 撤销 BeginStrongLockAcquire
 * 所做的强锁状态变更。
 */
void
AbortStrongLockAcquire(void)
{
	uint32		fasthashcode;
	LOCALLOCK  *locallock = StrongLockInProgress;

	if (locallock == NULL)
		return;

	fasthashcode = FastPathStrongLockHashPartition(locallock->hashcode);
	Assert(locallock->holdsStrongLockCount == true);
	SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
	Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
	FastPathStrongRelationLocks->count[fasthashcode]--;
	locallock->holdsStrongLockCount = false;
	StrongLockInProgress = NULL;
	SpinLockRelease(&FastPathStrongRelationLocks->mutex);
}

/*
 * GrantAwaitedLock -- 对我们正在 WaitOnLock 的那把锁
 *		调用 GrantLockLocal。
 *
 * proc.c 在下列情形需要它：我们因超时被从锁上赶下来，
 * 却发现别人反正已经把锁授予了我们。
 *
 * 我们本可以直接导出 GrantLockLocal，但那样就需要在
 * lock.h 中包含 resowner.h，从而造成循环依赖。
 */
void
GrantAwaitedLock(void)
{
	GrantLockLocal(awaitedLock, awaitedOwner);
}

/*
 * MarkLockClear -- 把已取得的锁标记为 "clear"
 *
 * 这意味着我们知道自己已经吸收了其他会话在我们取得
 * 这把锁之前产生的全部 sinval 消息，因此可以有把握地
 * 认为自己已经知道这把锁所保护的任何目录变更。
 */
void
MarkLockClear(LOCALLOCK *locallock)
{
	Assert(locallock->nLocks > 0);
	locallock->lockCleared = true;
}

/*
 * WaitOnLock -- 等待取得一把锁
 *
 * 调用方必须已经设置 MyProc->heldLocks，以反映本进程
 * 在该可锁对象上已经持有的锁。
 *
 * 进入时必须持有相应的分区锁。
 */
static void
WaitOnLock(LOCALLOCK *locallock, ResourceOwner owner)
{
	LOCKMETHODID lockmethodid = LOCALLOCK_LOCKMETHOD(*locallock);
	LockMethod	lockMethodTable = LockMethods[lockmethodid];

	LOCK_PRINT("WaitOnLock: sleeping on lock",
			   locallock->lock, locallock->tag.mode);

	/* 调整进程标题，以表明它正在等待 */
	set_ps_display_suffix("waiting");

	awaitedLock = locallock;
	awaitedOwner = owner;

	/*
	 * 注意：不要想着在调用 ProcSleep 之后，无论正常路径还是失败路径，
	 * 再去做任何共享状态清理。锁状态必须由
	 * 授予方完全设置好，或者在我们放弃等待时由 CheckDeadLock
	 * 设置好。这是必要的，因为有可能某个取消/die 中断
	 * 会在别人已经把锁授予我们、而我们尚未注意到时
	 * 打断 ProcSleep。因此，授予之后，锁表状态必须充分反映出
	 * 我们拥有这把锁这一事实；
	 * 返回时不能再做额外工作。
	 *
	 * 我们确实可以用 PG_TRY 块在失败后尝试清理，但这仍有一个
	 * 重大限制：等待期间可能发生 elog(FATAL)（例如 "die" 中断），
	 * 那时控制流不会回到这里。所以必要状态的全部清理
	 * 都应放在 LockErrorCleanup 里，而不是这里。
	 * 我们可以用 PG_TRY 清除 "waiting" 状态标志，因为进程退出时
	 * 做不做这件事都无关紧要。
	 */
	PG_TRY();
	{
		if (ProcSleep(locallock, lockMethodTable) != PROC_WAIT_STATUS_OK)
		{
			/*
			 * 我们因死锁而失败，见 CheckDeadLock()。现在
			 * 退出。
			 */
			awaitedLock = NULL;
			LOCK_PRINT("WaitOnLock: aborting on lock",
					   locallock->lock, locallock->tag.mode);
			LWLockRelease(LockHashPartitionLock(locallock->hashcode));

			/*
			 * 既然不再持有分区锁，就可以给出包含
			 * 所检测到的死锁细节的错误报告。
			 */
			DeadLockReport();
			/* 不会到达此处 */
		}
	}
	PG_CATCH();
	{
		/* 在这条路径上，awaitedLock 一直保持设置，直到 LockErrorCleanup */

		/* 重置 ps 显示，去掉后缀 */
		set_ps_display_remove_suffix();

		/* 并传播该错误 */
		PG_RE_THROW();
	}
	PG_END_TRY();

	awaitedLock = NULL;

	/* 重置 ps 显示，去掉后缀 */
	set_ps_display_remove_suffix();

	LOCK_PRINT("WaitOnLock: wakeup on lock",
			   locallock->lock, locallock->tag.mode);
}

/*
 * 把一个 proc 从它所在的等待队列中移除（调用方必须知道它在某个队列上）。
 * 仅当该 proc 没能得到锁时才使用本函数，因此把它的
 * waitStatus 设为 PROC_WAIT_STATUS_ERROR。
 *
 * 调用方必须持有相应的分区锁。此外，若需要向该 proc
 * 发信号，也由调用方负责。
 *
 * 注意：这不会清理该锁上也许存在的任何 locallock 对象。
 */
void
RemoveFromWaitQueue(PGPROC *proc, uint32 hashcode)
{
	LOCK	   *waitLock = proc->waitLock;
	PROCLOCK   *proclock = proc->waitProcLock;
	LOCKMODE	lockmode = proc->waitLockMode;
	LOCKMETHODID lockmethodid = LOCK_LOCKMETHOD(*waitLock);

	/* 确认 proc 正在等待 */
	Assert(proc->waitStatus == PROC_WAIT_STATUS_WAITING);
	Assert(proc->links.next != NULL);
	Assert(waitLock);
	Assert(!dclist_is_empty(&waitLock->waitProcs));
	Assert(0 < lockmethodid && lockmethodid < lengthof(LockMethods));

	/* 把 proc 从锁的等待队列中移除 */
	dclist_delete_from_thoroughly(&waitLock->waitProcs, &proc->links);

	/* 撤销等待进程对请求计数的增加 */
	Assert(waitLock->nRequested > 0);
	Assert(waitLock->nRequested > proc->waitLock->nGranted);
	waitLock->nRequested--;
	Assert(waitLock->requested[lockmode] > 0);
	waitLock->requested[lockmode]--;
	/* 在适当时不要忘记清掉 waitMask 位 */
	if (waitLock->granted[lockmode] == waitLock->requested[lockmode])
		waitLock->waitMask &= LOCKBIT_OFF(lockmode);

	/* 清理 proc 自己的状态，并把成功/失败信号传给它 */
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	proc->waitStatus = PROC_WAIT_STATUS_ERROR;

	/*
	 * 若 proclock 不代表任何已经持有的锁，就立刻删除它。
	 * （现在就必须做，因为若锁的所有者决定释放它，
	 * 而请求/授予计数随后变为零，LockRelease 期望
	 * 不再剩下任何 proclock。）然后看这把锁的其他等待者
	 * 现在是否可以被唤醒。
	 */
	CleanUpLock(waitLock, proclock,
				LockMethods[lockmethodid], hashcode,
				true);
}

/*
 * LockRelease -- 查找 'locktag'，并释放其上的一把 'lockmode' 锁。
 *		若 'sessionLock' 为真则释放会话级锁，否则释放
 *		普通事务锁。
 *
 * 副作用：找出现在可以被唤醒的等待进程，
 *		把它们请求的锁授予它们并唤醒它们。
 *		（必须在这里授予锁，以免正在醒来的进程与
 *		任何新来请求这把锁的进程之间
 *		发生竞争。）
 */
bool
LockRelease(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCALLOCKTAG localtag;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	LWLock	   *partitionLock;
	bool		wakeupNeeded;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

#ifdef LOCK_DEBUG
	if (LOCK_DEBUG_ENABLED(locktag))
		elog(LOG, "LockRelease: lock [%u,%u] %s",
			 locktag->locktag_field1, locktag->locktag_field2,
			 lockMethodTable->lockModeNames[lockmode]);
#endif

	/*
	 * 找出此锁和锁模式对应的 LOCALLOCK 项
	 */
	MemSet(&localtag, 0, sizeof(localtag)); /* 必须清掉填充字节 */
	localtag.lock = *locktag;
	localtag.mode = lockmode;

	locallock = (LOCALLOCK *) hash_search(LockMethodLocalHash,
										  &localtag,
										  HASH_FIND, NULL);

	/*
	 * 也让调用方打印自己的错误消息。不要 ereport(ERROR)。
	 */
	if (!locallock || locallock->nLocks <= 0)
	{
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return false;
	}

	/*
	 * 减少资源所有者的计数。
	 */
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		ResourceOwner owner;
		int			i;

		/* 确定锁的所有者 */
		if (sessionLock)
			owner = NULL;
		else
			owner = CurrentResourceOwner;

		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == owner)
			{
				Assert(lockOwners[i].nLocks > 0);
				if (--lockOwners[i].nLocks == 0)
				{
					if (owner != NULL)
						ResourceOwnerForgetLock(owner, locallock);
					/* 把未使用的槽位压紧去掉 */
					locallock->numLockOwners--;
					if (i < locallock->numLockOwners)
						lockOwners[i] = lockOwners[locallock->numLockOwners];
				}
				break;
			}
		}
		if (i < 0)
		{
			/* 不要释放属于另一个所有者的锁 */
			elog(WARNING, "you don't own a lock of type %s",
				 lockMethodTable->lockModeNames[lockmode]);
			return false;
		}
	}

	/*
	 * 减少本地总计数。若我们仍持有该锁，事情就
	 * 做完了。
	 */
	locallock->nLocks--;

	if (locallock->nLocks > 0)
		return true;

	/*
	 * 到这一步，我们不能再认为自己已经不受与这把锁
	 * 相关的失效消息影响。虽然在本例程任何有意的返回之前，我们都会删除
	 * LOCALLOCK 对象，但现在就显式重置 lockCleared 似乎仍值得，
	 * 以防某个错误使我们
	 * 无法删除该 LOCALLOCK。
	 */
	locallock->lockCleared = false;

	/* 尝试快速释放任何符合快速路径条件的锁。 */
	if (EligibleForRelationFastPath(locktag, lockmode) &&
		FastPathLocalUseCount > 0)
	{
		bool		released;

		/*
		 * 即使当初是我们把它放进来的，这里也可能找不到这把锁。
		 * 另一个后端也许已经把它移到了主表。
		 */
		LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
		released = FastPathUnGrantRelationLock(locktag->locktag_field2,
											   lockmode);
		LWLockRelease(&MyProc->fpInfoLock);
		if (released)
		{
			RemoveLocalLock(locallock);
			return true;
		}
	}

	/*
	 * 否则我们就不得不去碰共享锁表。
	 */
	partitionLock = LockHashPartitionLock(locallock->hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 通常不必重新查找 lock 或 proclock，因为我们在 locallock
	 * 表里保存了它们的地址，而且只要我们持有其上的锁，
	 * 它们就不会被移除。但有可能这把锁当初走的是快速路径，
	 * 此后被另一个后端移到了主哈希表，此时就需要在这里
	 * 查找这些对象。我们假定若是这种情况，
	 * lock 字段为 NULL。
	 */
	lock = locallock->lock;
	if (!lock)
	{
		PROCLOCKTAG proclocktag;

		Assert(EligibleForRelationFastPath(locktag, lockmode));
		lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
													locktag,
													locallock->hashcode,
													HASH_FIND,
													NULL);
		if (!lock)
			elog(ERROR, "failed to re-find shared lock object");
		locallock->lock = lock;

		proclocktag.myLock = lock;
		proclocktag.myProc = MyProc;
		locallock->proclock = (PROCLOCK *) hash_search(LockMethodProcLockHash,
													   &proclocktag,
													   HASH_FIND,
													   NULL);
		if (!locallock->proclock)
			elog(ERROR, "failed to re-find shared proclock object");
	}
	LOCK_PRINT("LockRelease: found", lock, lockmode);
	proclock = locallock->proclock;
	PROCLOCK_PRINT("LockRelease: found", proclock);

	/*
	 * 再确认一次：我们确实持有想要
	 * 释放的那种类型的锁。
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("LockRelease: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		RemoveLocalLock(locallock);
		return false;
	}

	/*
	 * 执行释放。CleanUpLock 会唤醒现在可以被唤醒的等待者。
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lock, proclock,
				lockMethodTable, locallock->hashcode,
				wakeupNeeded);

	LWLockRelease(partitionLock);

	RemoveLocalLock(locallock);
	return true;
}

/*
 * LockReleaseAll -- 释放当前进程所持有的、
 *		指定锁方法的全部锁。
 *
 * 嗯，未必是 *全部* 锁。可用的行为是：
 *		allLocks == true：释放包括会话级锁在内的全部锁。
 *		allLocks == false：释放全部非会话级锁。
 */
void
LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks)
{
	HASH_SEQ_STATUS status;
	LockMethod	lockMethodTable;
	int			i,
				numLockModes;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	int			partition;
	bool		have_fast_path_lwlock = false;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll: lockmethod=%d", lockmethodid);
#endif

	/*
	 * 若合适，去掉我们的快速路径 VXID 锁。注意，这是
	 * 我们在自己的 VXID 上所持锁得以释放的唯一途径：
	 * 它总是、并且只在顶层事务
	 * 结束时释放。
	 */
	if (lockmethodid == DEFAULT_LOCKMETHOD)
		VirtualXactLockTableCleanup();

	numLockModes = lockMethodTable->numLockModes;

	/*
	 * 先遍历 locallock 表，去掉不需要的项，再扫描该进程的
	 * proclock 并去掉那些。分开做是因为我们也许有多个 locallock 项
	 * 指向同一个 proclock，绝不能留下悬空
	 * 指针。快速路径锁则在
	 * 扫描 locallock 表期间
	 * 清理。
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/*
		 * 若 LOCALLOCK 项未被使用，那一定是在设置这把锁时
		 * 用尽了共享内存。忘掉这条本地
		 * 项即可。
		 */
		if (locallock->nLocks == 0)
		{
			RemoveLocalLock(locallock);
			continue;
		}

		/* 忽略不属于待移除锁方法的项 */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
			continue;

		/*
		 * 若要求释放全部锁，直接清掉该项即可。
		 * 否则必须扫描，看是否有会话级锁。我们假定
		 * 会话级锁最多只有一条 lockOwners 项。
		 */
		if (!allLocks)
		{
			LOCALLOCKOWNER *lockOwners = locallock->lockOwners;

			/* 若会话级锁不在数组位置 0，就把它下移到 0 */
			for (i = 0; i < locallock->numLockOwners; i++)
			{
				if (lockOwners[i].owner == NULL)
					lockOwners[0] = lockOwners[i];
				else
					ResourceOwnerForgetLock(lockOwners[i].owner, locallock);
			}

			if (locallock->numLockOwners > 0 &&
				lockOwners[0].owner == NULL &&
				lockOwners[0].nLocks > 0)
			{
				/* 修正 locallock，使它只显示会话级锁 */
				locallock->nLocks = lockOwners[0].nLocks;
				locallock->numLockOwners = 1;
				/* 我们并不删除这个 locallock，因此到此结束 */
				continue;
			}
			else
				locallock->numLockOwners = 0;
		}

#ifdef USE_ASSERT_CHECKING

		/*
		 * 元组锁目前只在事务内的短时间里持有。
		 * 检查我们没有忘记释放某一把。
		 */
		if (LOCALLOCK_LOCKTAG(*locallock) == LOCKTAG_TUPLE && !allLocks)
			elog(WARNING, "tuple lock held at commit");
#endif

		/*
		 * 若 lock 或 proclock 指针为 NULL，这把锁是经由
		 * 关系快速路径取得的（并且不知道已经被转移）。
		 */
		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			LOCKMODE	lockmode = locallock->tag.mode;
			Oid			relid;

			/* 确认我们拿到的确实是一把快速路径锁。 */
			if (!EligibleForRelationFastPath(&locallock->tag.lock, lockmode))
				elog(PANIC, "locallock table corrupted");

			/*
			 * 若当前没有持有保护我们的
			 * 快速路径数据结构的那把 LWLock，就必须在尝试
			 * 经由快速路径释放锁之前先取得它。我们会一直
			 * 持有这把 LWLock，直到扫完 locallock 表，
			 * 除非碰到一把已被转移的快速路径锁。（XXX 这真的是
			 * 个好主意吗？项可能会非常多……）
			 */
			if (!have_fast_path_lwlock)
			{
				LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);
				have_fast_path_lwlock = true;
			}

			/* 尝试快速路径释放。 */
			relid = locallock->tag.lock.locktag_field2;
			if (FastPathUnGrantRelationLock(relid, lockmode))
			{
				RemoveLocalLock(locallock);
				continue;
			}

			/*
			 * 我们的锁当初经由快速路径取得，现在已经被
			 * 转移到主锁表。这需要一些额外工作，因此在开始之前
			 * 先释放我们的快速路径锁。
			 */
			LWLockRelease(&MyProc->fpInfoLock);
			have_fast_path_lwlock = false;

			/*
			 * 现在丢掉这把锁。这种情形下我们没有指向 LOCK 或
			 * PROCLOCK 的指针，因此处理方式必须与普通的锁释放
			 * 略有不同。不幸的是，这需要在 partitionLock 上
			 * 多做一轮 LWLock 的获取与释放，不过希望这种情况
			 * 不会经常发生。
			 */
			LockRefindAndRelease(lockMethodTable, MyProc,
								 &locallock->tag.lock, lockmode, false);
			RemoveLocalLock(locallock);
			continue;
		}

		/* 标记该 proclock，表明我们需要释放这个锁模式 */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* 并删除 locallock 哈希表项 */
		RemoveLocalLock(locallock);
	}

	/* 快速路径数据结构已经用完 */
	if (have_fast_path_lwlock)
		LWLockRelease(&MyProc->fpInfoLock);

	/*
	 * 现在，分别扫描每个锁分区。
	 */
	for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
	{
		LWLock	   *partitionLock;
		dlist_head *procLocks = &MyProc->myProcLocks[partition];
		dlist_mutable_iter proclock_iter;

		partitionLock = LockHashPartitionLockByIndex(partition);

		/*
		 * 若此分区的 proclock 链表为空，就可以跳过
		 * 获取分区锁。这项优化比看上去更棘手，
		 * 因为另一个后端可能正在把我们的某把
		 * 快速路径锁提升进 proclock 链表。不过，任何这样的锁
		 * 都必须是我们在上面决定不删除的，因此现在再跳过它也没问题；
		 * 我们只会再次决定不删除它。但是，一旦取得
		 * 分区锁，我们必须小心地重新取出链表头，
		 * 以确保指针有效且是最新的。
		 * （若指针的读取/存储是原子的，
		 * 大概没有显著风险，
		 * 但我们不想做这个假定。）
		 *
		 * XXX 这一论证假定 locallock 表正确地代表了
		 * 我们的全部快速路径锁。allLocks 模式保证，
		 * 无论 locallock 情况如何都会清理全部普通锁，
		 * 但对快速路径锁我们失去了这一保证。
		 * 这并不理想。
		 */
		if (dlist_is_empty(procLocks))
			continue;			/* 不必检查这个分区 */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_foreach_modify(proclock_iter, procLocks)
		{
			PROCLOCK   *proclock = dlist_container(PROCLOCK, procLink, proclock_iter.cur);
			bool		wakeupNeeded = false;

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* 忽略不属于待移除锁方法的项 */
			if (LOCK_LOCKMETHOD(*lock) != lockmethodid)
				continue;

			/*
			 * 在 allLocks 模式下，即使 locallock
			 * 表出了问题，也强制释放全部锁
			 */
			if (allLocks)
				proclock->releaseMask = proclock->holdMask;
			else
				Assert((proclock->releaseMask & ~proclock->holdMask) == 0);

			/*
			 * 忽略没有东西要释放的项，除非它们的
			 * holdMask == 0，因而可以回收
			 */
			if (proclock->releaseMask == 0 && proclock->holdMask != 0)
				continue;

			PROCLOCK_PRINT("LockReleaseAll", proclock);
			LOCK_PRINT("LockReleaseAll", lock, 0);
			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);

			/*
			 * 释放先前标记的锁模式
			 */
			for (i = 1; i <= numLockModes; i++)
			{
				if (proclock->releaseMask & LOCKBIT_ON(i))
					wakeupNeeded |= UnGrantLock(lock, i, proclock,
												lockMethodTable);
			}
			Assert((lock->nRequested >= 0) && (lock->nGranted >= 0));
			Assert(lock->nGranted <= lock->nRequested);
			LOCK_PRINT("LockReleaseAll: updated", lock, 0);

			proclock->releaseMask = 0;

			/* 若需要，CleanUpLock 会唤醒等待者。 */
			CleanUpLock(lock, proclock,
						lockMethodTable,
						LockTagHashCode(&lock->tag),
						wakeupNeeded);
		}						/* 遍历此分区内的 PROCLOCKs */

		LWLockRelease(partitionLock);
	}							/* 遍历各个分区 */

#ifdef LOCK_DEBUG
	if (*(lockMethodTable->trace_flag))
		elog(LOG, "LockReleaseAll done");
#endif
}

/*
 * LockReleaseSession -- 释放当前进程所持有的、
 *		指定锁方法的全部会话级锁。
 */
void
LockReleaseSession(LOCKMETHODID lockmethodid)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		/* 忽略不属于指定锁方法的项 */
		if (LOCALLOCK_LOCKMETHOD(*locallock) != lockmethodid)
			continue;

		ReleaseLockIfHeld(locallock, true);
	}
}

/*
 * LockReleaseCurrentOwner
 *		释放属于 CurrentResourceOwner 的全部锁
 *
 * 若调用方知道这些锁是哪些，可以把它们作为数组传入。
 * 当持有大量锁时，这能显著加快调用。
 * 否则，为 locallocks 传入 NULL，我们会遍历自己的哈希
 * 表去找它们。
 */
void
LockReleaseCurrentOwner(LOCALLOCK **locallocks, int nlocks)
{
	if (locallocks == NULL)
	{
		HASH_SEQ_STATUS status;
		LOCALLOCK  *locallock;

		hash_seq_init(&status, LockMethodLocalHash);

		while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
			ReleaseLockIfHeld(locallock, false);
	}
	else
	{
		int			i;

		for (i = nlocks - 1; i >= 0; i--)
			ReleaseLockIfHeld(locallocks[i], false);
	}
}

/*
 * ReleaseLockIfHeld
 *		若 sessionLock 为真，则释放此可锁对象上的任何会话级锁；
 *		否则，释放 CurrentResourceOwner 所持有的任何锁。
 *
 * 把一个 ResourceOwner 指针（会话级锁则传 NULL）传进来很诱人，
 * 但不重构 LockRelease() 的话，我们无法支持释放
 * 属于 CurrentResourceOwner 以外的资源所有者的锁。
 * 若要重构，最好顺便改成不必再对 locallock
 * 做一次哈希表查找。不过目前这个
 * 函数用得还不够多，不值得为了它的
 * 方便去重构。
 */
static void
ReleaseLockIfHeld(LOCALLOCK *locallock, bool sessionLock)
{
	ResourceOwner owner;
	LOCALLOCKOWNER *lockOwners;
	int			i;

	/* 确定锁的所有者（必须与 LockRelease 一致！） */
	if (sessionLock)
		owner = NULL;
	else
		owner = CurrentResourceOwner;

	/* 扫描，看是否有属于目标所有者的锁 */
	lockOwners = locallock->lockOwners;
	for (i = locallock->numLockOwners - 1; i >= 0; i--)
	{
		if (lockOwners[i].owner == owner)
		{
			Assert(lockOwners[i].nLocks > 0);
			if (lockOwners[i].nLocks < locallock->nLocks)
			{
				/*
				 * 忘掉这个 ResourceOwner 之后，我们仍会持有
				 * 这把锁。
				 */
				locallock->nLocks -= lockOwners[i].nLocks;
				/* 把未使用的槽位压紧去掉 */
				locallock->numLockOwners--;
				if (owner != NULL)
					ResourceOwnerForgetLock(owner, locallock);
				if (i < locallock->numLockOwners)
					lockOwners[i] = lockOwners[locallock->numLockOwners];
			}
			else
			{
				Assert(lockOwners[i].nLocks == locallock->nLocks);
				/* 我们只想调用一次 LockRelease */
				lockOwners[i].nLocks = 1;
				locallock->nLocks = 1;
				if (!LockRelease(&locallock->tag.lock,
								 locallock->tag.mode,
								 sessionLock))
					elog(WARNING, "ReleaseLockIfHeld: failed??");
			}
			break;
		}
	}
}

/*
 * LockReassignCurrentOwner
 *		把属于 CurrentResourceOwner 的全部锁改派为属于
 *		它的父资源所有者。
 *
 * 若调用方知道这些锁是哪些，可以把它们作为数组传入。
 * 当持有大量锁时，这能显著加快调用
 * （例如模式很大的 pg_dump）。否则，为 locallocks 传入 NULL，
 * 我们会遍历自己的哈希表去找它们。
 */
void
LockReassignCurrentOwner(LOCALLOCK **locallocks, int nlocks)
{
	ResourceOwner parent = ResourceOwnerGetParent(CurrentResourceOwner);

	Assert(parent != NULL);

	if (locallocks == NULL)
	{
		HASH_SEQ_STATUS status;
		LOCALLOCK  *locallock;

		hash_seq_init(&status, LockMethodLocalHash);

		while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
			LockReassignOwner(locallock, parent);
	}
	else
	{
		int			i;

		for (i = nlocks - 1; i >= 0; i--)
			LockReassignOwner(locallocks[i], parent);
	}
}

/*
 * LockReassignCurrentOwner 的子例程。把属于
 * CurrentResourceOwner 的一把给定锁改派给它的父所有者。
 */
static void
LockReassignOwner(LOCALLOCK *locallock, ResourceOwner parent)
{
	LOCALLOCKOWNER *lockOwners;
	int			i;
	int			ic = -1;
	int			ip = -1;

	/*
	 * 扫描，看是否有属于当前所有者或其
	 * 父所有者的锁
	 */
	lockOwners = locallock->lockOwners;
	for (i = locallock->numLockOwners - 1; i >= 0; i--)
	{
		if (lockOwners[i].owner == CurrentResourceOwner)
			ic = i;
		else if (lockOwners[i].owner == parent)
			ip = i;
	}

	if (ic < 0)
		return;					/* 当前没有锁 */

	if (ip < 0)
	{
		/* 父所有者没有槽位，因此直接把子所有者的槽位给它 */
		lockOwners[ic].owner = parent;
		ResourceOwnerRememberLock(parent, locallock);
	}
	else
	{
		/* 把子所有者的计数并入父所有者 */
		lockOwners[ip].nLocks += lockOwners[ic].nLocks;
		/* 把未使用的槽位压紧去掉 */
		locallock->numLockOwners--;
		if (ic < locallock->numLockOwners)
			lockOwners[ic] = lockOwners[locallock->numLockOwners];
	}
	ResourceOwnerForgetLock(CurrentResourceOwner, locallock);
}

/*
 * FastPathGrantRelationLock
 *		若有空位，就用每个后端的快速路径数组授予锁。
 */
static bool
FastPathGrantRelationLock(Oid relid, LOCKMODE lockmode)
{
	uint32		f;
	uint32		unused_slot = FP_LOCK_SLOTS_PER_BACKEND;

	/* 扫描此 relid 的已有项，同时记住空槽。 */
	for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; f++)
	{
		if (FAST_PATH_GET_BITS(MyProc, f) == 0)
			unused_slot = f;
		else if (MyProc->fpRelId[f] == relid)
		{
			Assert(!FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode));
			FAST_PATH_SET_LOCKMODE(MyProc, f, lockmode);
			return true;
		}
	}

	/* 若没有已有项，就使用任一空槽。 */
	if (unused_slot < FP_LOCK_SLOTS_PER_BACKEND)
	{
		MyProc->fpRelId[unused_slot] = relid;
		FAST_PATH_SET_LOCKMODE(MyProc, unused_slot, lockmode);
		++FastPathLocalUseCount;
		return true;
	}

	/* 既没有已有项，也没有空槽。 */
	return false;
}

/*
 * FastPathUnGrantRelationLock
 *		若存在，则释放快速路径锁。同时更新后端私有的本地
 *		使用计数。
 */
static bool
FastPathUnGrantRelationLock(Oid relid, LOCKMODE lockmode)
{
	uint32		f;
	bool		result = false;

	FastPathLocalUseCount = 0;
	for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; f++)
	{
		if (MyProc->fpRelId[f] == relid
			&& FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode))
		{
			Assert(!result);
			FAST_PATH_CLEAR_LOCKMODE(MyProc, f, lockmode);
			result = true;
			/* 继续迭代，以便更新 FastPathLocalUseCount */
		}
		if (FAST_PATH_GET_BITS(MyProc, f) != 0)
			++FastPathLocalUseCount;
	}
	return result;
}

/*
 * FastPathTransferRelationLocks
 *		把与给定锁标签匹配的锁，从每个后端的快速路径
 *		数组转移到共享哈希表。
 *
 * 成功则返回真，共享内存用尽则返回假。
 */
static bool
FastPathTransferRelationLocks(LockMethod lockMethodTable, const LOCKTAG *locktag,
							  uint32 hashcode)
{
	LWLock	   *partitionLock = LockHashPartitionLock(hashcode);
	Oid			relid = locktag->locktag_field2;
	uint32		i;

	/*
	 * 每个有可能持有快速路径锁的 PGPROC 都在
	 * ProcGlobal->allProcs 中。预备事务不在其中，但预备事务
	 * 所持有的任何未完成快速路径锁都会被
	 * 转移到主锁表。
	 */
	for (i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		uint32		f;

		LWLockAcquire(&proc->fpInfoLock, LW_EXCLUSIVE);

		/*
		 * 若目标后端引用的数据库与这把
		 * 锁不同，就完全不必检查各个关系标识；
		 * 它们都不可能相关。
		 *
		 * proc->databaseId 在后端启动时设置，此后不再
		 * 改变，因此也许可以在取得 &proc->fpInfoLock 之前
		 * 做这个测试。特别是，若目标后端持有任何快速路径锁，
		 * 它在设置 proc->databaseId 之后肯定已经做过一次内存栅栏操作
		 * （尤其是一次 LWLock 获取）。不过，我们自己的后端是否肯定
		 * 在另一个后端设置 proc->databaseId 之后做过内存
		 * 栅栏操作，就不那么清楚了。因此
		 * 目前我们在取得 LWLock 之后再做测试，
		 * 以求安全。
		 */
		if (proc->databaseId != locktag->locktag_field1)
		{
			LWLockRelease(&proc->fpInfoLock);
			continue;
		}

		for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; f++)
		{
			uint32		lockmode;

			/* 查找与给定 relid 匹配的已分配槽。 */
			if (relid != proc->fpRelId[f] || FAST_PATH_GET_BITS(proc, f) == 0)
				continue;

			/* 找出或创建锁对象。 */
			LWLockAcquire(partitionLock, LW_EXCLUSIVE);
			for (lockmode = FAST_PATH_LOCKNUMBER_OFFSET;
				 lockmode < FAST_PATH_LOCKNUMBER_OFFSET + FAST_PATH_BITS_PER_SLOT;
				 ++lockmode)
			{
				PROCLOCK   *proclock;

				if (!FAST_PATH_CHECK_LOCKMODE(proc, f, lockmode))
					continue;
				proclock = SetupLockInTable(lockMethodTable, proc, locktag,
											hashcode, lockmode);
				if (!proclock)
				{
					LWLockRelease(partitionLock);
					LWLockRelease(&proc->fpInfoLock);
					return false;
				}
				GrantLock(proclock->tag.myLock, proclock, lockmode);
				FAST_PATH_CLEAR_LOCKMODE(proc, f, lockmode);
			}
			LWLockRelease(partitionLock);

			/* 不必再检查其余槽位。 */
			break;
		}
		LWLockRelease(&proc->fpInfoLock);
	}
	return true;
}

/*
 * FastPathGetRelationLockEntry
 *		返回当初经由快速路径取得的锁的 PROCLOCK，
 *		必要时把它转移到主锁表。
 *
 * 注意：更新 locallock 对象由调用方负责。
 */
static PROCLOCK *
FastPathGetRelationLockEntry(LOCALLOCK *locallock)
{
	LockMethod	lockMethodTable = LockMethods[DEFAULT_LOCKMETHOD];
	LOCKTAG    *locktag = &locallock->tag.lock;
	PROCLOCK   *proclock = NULL;
	LWLock	   *partitionLock = LockHashPartitionLock(locallock->hashcode);
	Oid			relid = locktag->locktag_field2;
	uint32		f;

	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; f++)
	{
		uint32		lockmode;

		/* 查找与给定 relid 匹配的已分配槽。 */
		if (relid != MyProc->fpRelId[f] || FAST_PATH_GET_BITS(MyProc, f) == 0)
			continue;

		/* 若我们没有给定模式的锁，就别管它了！ */
		lockmode = locallock->tag.mode;
		if (!FAST_PATH_CHECK_LOCKMODE(MyProc, f, lockmode))
			break;

		/* 找出或创建锁对象。 */
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		proclock = SetupLockInTable(lockMethodTable, MyProc, locktag,
									locallock->hashcode, lockmode);
		if (!proclock)
		{
			LWLockRelease(partitionLock);
			LWLockRelease(&MyProc->fpInfoLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase %s.", "max_locks_per_transaction")));
		}
		GrantLock(proclock->tag.myLock, proclock, lockmode);
		FAST_PATH_CLEAR_LOCKMODE(MyProc, f, lockmode);

		LWLockRelease(partitionLock);

		/* 不必再检查其余槽位。 */
		break;
	}

	LWLockRelease(&MyProc->fpInfoLock);

	/* 锁也许已经被某个其他后端转移走了。 */
	if (proclock == NULL)
	{
		LOCK	   *lock;
		PROCLOCKTAG proclocktag;
		uint32		proclock_hashcode;

		LWLockAcquire(partitionLock, LW_SHARED);

		lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
													locktag,
													locallock->hashcode,
													HASH_FIND,
													NULL);
		if (!lock)
			elog(ERROR, "failed to re-find shared lock object");

		proclocktag.myLock = lock;
		proclocktag.myProc = MyProc;

		proclock_hashcode = ProcLockHashCode(&proclocktag, locallock->hashcode);
		proclock = (PROCLOCK *)
			hash_search_with_hash_value(LockMethodProcLockHash,
										&proclocktag,
										proclock_hashcode,
										HASH_FIND,
										NULL);
		if (!proclock)
			elog(ERROR, "failed to re-find shared proclock object");
		LWLockRelease(partitionLock);
	}

	return proclock;
}

/*
 * GetLockConflicts
 *		取得当前持有会与指定锁/锁模式
 *		冲突的锁的事务的 VirtualTransactionIds 数组。
 *		仅仅在等待这样一把锁的事务不会被报告。
 *
 * 结果数组由 palloc 分配，并以一个无效的 VXID 终止。
 * 若 *countp 非空，则更新为所设置的项数。
 *
 * 当然，结果在返回时可能已经过时，因此
 * 使用本函数必须仔细考虑。同样，没有 "lxid" 的
 * PGPROC 无论持有什么锁，都会被视为不冲突。
 * 现有调用方并不关心某个加锁者在其 pg_xact 更新完成之后的情况。
 * CommitTransaction() 在 pg_xact 更新之后、
 * 释放锁之前清除 "lxid"。
 *
 * 注意，我们从不把当前事务的 vxid 放进结果数组，
 * 因为一个事务永远不会阻塞自己。
 */
VirtualTransactionId *
GetLockConflicts(const LOCKTAG *locktag, LOCKMODE lockmode, int *countp)
{
	static VirtualTransactionId *vxids;
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LockMethod	lockMethodTable;
	LOCK	   *lock;
	LOCKMASK	conflictMask;
	dlist_iter	proclock_iter;
	PROCLOCK   *proclock;
	uint32		hashcode;
	LWLock	   *partitionLock;
	int			count = 0;
	int			fast_count = 0;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];
	if (lockmode <= 0 || lockmode > lockMethodTable->numLockModes)
		elog(ERROR, "unrecognized lock mode: %d", lockmode);

	/*
	 * 分配内存来存放结果，并用 InvalidVXID 填满。我们只
	 * 需要够放下 MaxBackends + max_prepared_xacts 再加一个终止符的空间。
	 * 在热备（InHotStandby）中，于 TopMemoryContext 里分配一次。
	 */
	if (InHotStandby)
	{
		if (vxids == NULL)
			vxids = (VirtualTransactionId *)
				MemoryContextAlloc(TopMemoryContext,
								   sizeof(VirtualTransactionId) *
								   (MaxBackends + max_prepared_xacts + 1));
	}
	else
		vxids = (VirtualTransactionId *)
			palloc0(sizeof(VirtualTransactionId) *
					(MaxBackends + max_prepared_xacts + 1));

	/* 计算哈希码和分区锁，并查找冲突模式。 */
	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);
	conflictMask = lockMethodTable->conflictTab[lockmode];

	/*
	 * 快速路径锁也许还没有进入主锁表。
	 * 若我们正在处理的锁可能与这样的锁冲突，就必须
	 * 检查每个后端的快速路径数组中的冲突。
	 */
	if (ConflictsWithRelationFastPath(locktag, lockmode))
	{
		int			i;
		Oid			relid = locktag->locktag_field2;
		VirtualTransactionId vxid;

		/*
		 * 遍历相关的 PGPROCs。预备事务所持有的任何东西
		 * 都已经转移到主锁表，
		 * 因此不必操心那些。这一切都有点含糊，因为
		 * 我们访问过某个分区之后，仍可能取得新锁，
		 * 但调用方最好反正准备好应对这种情况，
		 * 因为这些锁同样可能在我们返回该值之后、
		 * 调用方拿它做某事
		 * 之前被取得。
		 */
		for (i = 0; i < ProcGlobal->allProcCount; i++)
		{
			PGPROC	   *proc = &ProcGlobal->allProcs[i];
			uint32		f;

			/* 后端永远不会阻塞自己 */
			if (proc == MyProc)
				continue;

			LWLockAcquire(&proc->fpInfoLock, LW_SHARED);

			/*
			 * 若目标后端引用的数据库与
			 * 这把锁不同，就完全不必检查各个关系标识；
			 * 它们都不可能相关。
			 *
			 * 为什么在取得锁之后才做这个测试，见
			 * FastPathTransferRelationLocks() 中的讨论。
			 */
			if (proc->databaseId != locktag->locktag_field1)
			{
				LWLockRelease(&proc->fpInfoLock);
				continue;
			}

			for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; f++)
			{
				uint32		lockmask;

				/* 查找与给定 relid 匹配的已分配槽。 */
				if (relid != proc->fpRelId[f])
					continue;
				lockmask = FAST_PATH_GET_BITS(proc, f);
				if (!lockmask)
					continue;
				lockmask <<= FAST_PATH_LOCKNUMBER_OFFSET;

				/*
				 * 每个关系只能有一项，因此若找到了它
				 * 并且它不冲突，就可以跳过其余槽位。
				 */
				if ((lockmask & conflictMask) == 0)
					break;

				/* 冲突！ */
				GET_VXID_FROM_PGPROC(vxid, *proc);

				if (VirtualTransactionIdIsValid(vxid))
					vxids[count++] = vxid;
				/* 否则，事务已经提交或中止 */

				/* 不必再检查其余槽位。 */
				break;
			}

			LWLockRelease(&proc->fpInfoLock);
		}
	}

	/* 记住我们发现了多少快速路径冲突。 */
	fast_count = count;

	/*
	 * 查找与该标签匹配的锁对象。
	 */
	LWLockAcquire(partitionLock, LW_SHARED);

	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_FIND,
												NULL);
	if (!lock)
	{
		/*
		 * 若锁对象不存在，就没有任何东西持有
		 * 此可锁对象上的锁。
		 */
		LWLockRelease(partitionLock);
		vxids[count].backendId = InvalidBackendId;
		vxids[count].localTransactionId = InvalidLocalTransactionId;
		if (countp)
			*countp = count;
		return vxids;
	}

	/*
	 * 检查该锁的每个现有持有者（或等待者）。
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		proclock = dlist_container(PROCLOCK, lockLink, proclock_iter.cur);

		if (conflictMask & proclock->holdMask)
		{
			PGPROC	   *proc = proclock->tag.myProc;

			/* 后端永远不会阻塞自己 */
			if (proc != MyProc)
			{
				VirtualTransactionId vxid;

				GET_VXID_FROM_PGPROC(vxid, *proc);

				if (VirtualTransactionIdIsValid(vxid))
				{
					int			i;

					/* 避免重复项。 */
					for (i = 0; i < fast_count; ++i)
						if (VirtualTransactionIdEquals(vxids[i], vxid))
							break;
					if (i >= fast_count)
						vxids[count++] = vxid;
				}
				/* 否则，事务已经提交或中止 */
			}
		}
	}

	LWLockRelease(partitionLock);

	if (count > MaxBackends + max_prepared_xacts)	/* 按理不应发生 */
		elog(PANIC, "too many conflicting locks found");

	vxids[count].backendId = InvalidBackendId;
	vxids[count].localTransactionId = InvalidLocalTransactionId;
	if (countp)
		*countp = count;
	return vxids;
}

/*
 * 在共享锁表中找出一把锁并释放它。调用方有责任
 * 确认这样做是合理的。（例如，若也许仍有一个 LOCALLOCK
 * 对象带着指向它的指针，
 * 在这里释放这把锁就不好了。）
 *
 * 目前有两种使用情形：第一，提交时释放预备事务所持的锁
 * （见 lock_twophase_postcommit）；第二，释放经由快速路径取得、
 * 已转移到主哈希表、随后又被释放的锁
 * （见 LockReleaseAll）。
 */
static void
LockRefindAndRelease(LockMethod lockMethodTable, PGPROC *proc,
					 LOCKTAG *locktag, LOCKMODE lockmode,
					 bool decrement_strong_lock_count)
{
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	uint32		hashcode;
	uint32		proclock_hashcode;
	LWLock	   *partitionLock;
	bool		wakeupNeeded;

	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 重新找出锁对象（它应该在那里）。
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_FIND,
												NULL);
	if (!lock)
		elog(PANIC, "failed to re-find shared lock object");

	/*
	 * 重新找出 proclock 对象（同上）。
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														&proclocktag,
														proclock_hashcode,
														HASH_FIND,
														NULL);
	if (!proclock)
		elog(PANIC, "failed to re-find shared proclock object");

	/*
	 * 再确认一次：我们确实持有想要
	 * 释放的那种类型的锁。
	 */
	if (!(proclock->holdMask & LOCKBIT_ON(lockmode)))
	{
		PROCLOCK_PRINT("lock_twophase_postcommit: WRONGTYPE", proclock);
		LWLockRelease(partitionLock);
		elog(WARNING, "you don't own a lock of type %s",
			 lockMethodTable->lockModeNames[lockmode]);
		return;
	}

	/*
	 * 执行释放。CleanUpLock 会唤醒现在可以被唤醒的等待者。
	 */
	wakeupNeeded = UnGrantLock(lock, lockmode, proclock, lockMethodTable);

	CleanUpLock(lock, proclock,
				lockMethodTable, hashcode,
				wakeupNeeded);

	LWLockRelease(partitionLock);

	/*
	 * 递减强锁计数。这段逻辑只在两阶段提交时需要。
	 */
	if (decrement_strong_lock_count
		&& ConflictsWithRelationFastPath(locktag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		Assert(FastPathStrongRelationLocks->count[fasthashcode] > 0);
		FastPathStrongRelationLocks->count[fasthashcode]--;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}
}

/*
 * CheckForSessionAndXactLocks
 *		检查事务是否在同一对象上同时持有会话级锁和事务级
 *		锁；若是，则抛出错误。
 *
 * 若我们在同一对象上既有会话级锁又有事务级锁，
 * PREPARE TRANSACTION 必须失败。对普通锁来说这不该发生，
 * 因为我们只在 VACUUM 之类的特殊操作中才在会话级
 * 取得那些锁。不过咨询锁有可能碰上这种情况。
 *
 * 若能保留会话级持有、把事务级持有交给预备事务，那就好了。
 * 然而那需要两个 PROCLOCK 对象，而且我们无法确定在
 * PostPrepare_Locks 动手时，另一个 PROCLOCK 是否
 * 可用。因此目前趁还能安全地这样做时，
 * 直接报错退出。
 *
 * 由于 LOCALLOCK 表为每种锁模式分别存放一项，
 * 无法只孤立地检查这些项来完成这个检查。
 * 我们必须建立一个只以 locktag 为索引的临时哈希表。
 */
static void
CheckForSessionAndXactLocks(void)
{
	typedef struct
	{
		LOCKTAG		lock;		/* 标识可锁对象 */
		bool		sessLock;	/* 是否有任何锁模式在会话级被持有？ */
		bool		xactLock;	/* 是否有任何锁模式在事务级被持有？ */
	} PerLockTagEntry;

	HASHCTL		hash_ctl;
	HTAB	   *lockhtab;
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/* 创建一个只以 LOCKTAG 为键的本地哈希表 */
	hash_ctl.keysize = sizeof(LOCKTAG);
	hash_ctl.entrysize = sizeof(PerLockTagEntry);
	hash_ctl.hcxt = CurrentMemoryContext;

	lockhtab = hash_create("CheckForSessionAndXactLocks table",
						   256, /* 任意的初始大小 */
						   &hash_ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 扫描本地锁表，找出每个 LOCKTAG 的项 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		PerLockTagEntry *hentry;
		bool		found;
		int			i;

		/*
		 * 忽略 VXID 锁。我们不希望预备事务持有它们，
		 * 因为重启之后它们没有意义。
		 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* 若我们实际上并未持有该锁，则忽略它 */
		if (locallock->nLocks <= 0)
			continue;

		/* 否则，在 lockhtab 中找出或创建一项 */
		hentry = (PerLockTagEntry *) hash_search(lockhtab,
												 &locallock->tag.lock,
												 HASH_ENTER, &found);
		if (!found)				/* 若是新建的，则初始化 */
			hentry->sessLock = hentry->xactLock = false;

		/* 扫描，看我们是在会话级、事务级还是两者都持有该锁 */
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				hentry->sessLock = true;
			else
				hentry->xactLock = true;
		}

		/*
		 * 一旦看到两种锁，就可以立刻抛出错误；不必
		 * 再等着看是否还有更多违规。
		 */
		if (hentry->sessLock && hentry->xactLock)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));
	}

	/* 成功，因此做清理 */
	hash_destroy(lockhtab);
}

/*
 * AtPrepare_Locks
 *		为 PREPARE 做准备工作：为当前持有的全部锁
 *		制作两阶段提交状态文件记录。
 *
 * 会话级锁被忽略，VXID 锁也一样。
 *
 * 在很大程度上，为此不必触碰共享内存 ---
 * 所需的全部状态信息都在 locallock 表里。
 * 不过快速路径锁是个例外：在允许 PREPARE TRANSACTION
 * 成功之前，我们把任何这样的锁移到主表。
 */
void
AtPrepare_Locks(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;

	/* 首先，确认没有同时存在事务级和会话级的锁 */
	CheckForSessionAndXactLocks();

	/* 现在做每个 locallock 的清理工作 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		TwoPhaseLockRecord record;
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		bool		haveSessionLock;
		bool		haveXactLock;
		int			i;

		/*
		 * 忽略 VXID 锁。我们不希望预备事务持有它们，
		 * 因为重启之后它们没有意义。
		 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* 若我们实际上并未持有该锁，则忽略它 */
		if (locallock->nLocks <= 0)
			continue;

		/* 扫描，看我们是在会话级还是事务级持有它 */
		haveSessionLock = haveXactLock = false;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				haveSessionLock = true;
			else
				haveXactLock = true;
		}

		/* 若我们只有会话级锁，则忽略它 */
		if (!haveXactLock)
			continue;

		/* 这不可能发生，因为我们已经检查过了 */
		if (haveSessionLock)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));

		/*
		 * 若本地锁是经由快速路径取得的，就需要把它移到
		 * 主锁表，或者万一它已经被转移，
		 * 就只取得指向现有
		 * 主锁表项的指针。
		 */
		if (locallock->proclock == NULL)
		{
			locallock->proclock = FastPathGetRelationLockEntry(locallock);
			locallock->lock = locallock->proclock->tag.myLock;
		}

		/*
		 * 安排不要释放此锁项所持有的任何强锁计数。
		 * 必须保留该计数，直到预备事务被
		 * 提交或回滚。
		 */
		locallock->holdsStrongLockCount = false;

		/*
		 * 创建一条两阶段提交记录。
		 */
		memcpy(&(record.locktag), &(locallock->tag.lock), sizeof(LOCKTAG));
		record.lockmode = locallock->tag.mode;

		RegisterTwoPhaseRecord(TWOPHASE_RM_LOCK_ID, 0,
							   &record, sizeof(TwoPhaseLockRecord));
	}
}

/*
 * PostPrepare_Locks
 *		PREPARE 成功之后做清理
 *
 * 这里要把我们的锁的所有权转移给一个现在与
 * 预备事务相关联的哑 PGPROC，并清掉
 * LOCALLOCK 表中的对应项。
 *
 * 注意：去掉 LOCALLOCK 项会在事务的资源所有者里留下悬空
 * 指针。目前这没问题，因为 resowner.c 在顶层
 * 事务提交或中止时并不逐个释放锁。我们也可以把 nLocks
 * 清零，把 LOCALLOCK 项留给 LockReleaseAll 去回收，
 * 但那样大概
 * 更费周期。
 */
void
PostPrepare_Locks(TransactionId xid)
{
	PGPROC	   *newproc = TwoPhaseGetDummyProc(xid, false);
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	int			partition;

	/* 不能预备一个锁组的追随者。 */
	Assert(MyProc->lockGroupLeader == NULL ||
		   MyProc->lockGroupLeader == MyProc);

	/* 这是临界区：任何错误都意味着大麻烦 */
	START_CRIT_SECTION();

	/*
	 * 先遍历 locallock 表，去掉不需要的项，再扫描该进程的
	 * proclock，并把它们转移给目标 proc。
	 * 这两步要分开处理。
	 *
	 * 分开做是因为也许有多个 locallock 项指向同一个 proclock，
	 * 绝不能因此留下悬空
	 * 指针。
	 */
	hash_seq_init(&status, LockMethodLocalHash);

	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		LOCALLOCKOWNER *lockOwners = locallock->lockOwners;
		bool		haveSessionLock;
		bool		haveXactLock;
		int			i;

		if (locallock->proclock == NULL || locallock->lock == NULL)
		{
			/*
			 * 一定是在设置这把锁时用尽了共享内存。
			 * 忘掉这条本地项即可。
			 */
			Assert(locallock->nLocks == 0);
			RemoveLocalLock(locallock);
			continue;
		}

		/* 忽略 VXID 锁 */
		if (locallock->tag.lock.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
			continue;

		/* 扫描，看我们是在会话级还是事务级持有它 */
		haveSessionLock = haveXactLock = false;
		for (i = locallock->numLockOwners - 1; i >= 0; i--)
		{
			if (lockOwners[i].owner == NULL)
				haveSessionLock = true;
			else
				haveXactLock = true;
		}

		/* 若我们只有会话级锁，则忽略它 */
		if (!haveXactLock)
			continue;

		/* 这不可能发生，因为我们已经检查过了 */
		if (haveSessionLock)
			ereport(PANIC,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot PREPARE while holding both session-level and transaction-level locks on the same object")));

		/* 标记该 proclock，表明我们需要释放这个锁模式 */
		if (locallock->nLocks > 0)
			locallock->proclock->releaseMask |= LOCKBIT_ON(locallock->tag.mode);

		/* 并删除 locallock 哈希表项 */
		RemoveLocalLock(locallock);
	}

	/*
	 * 现在，分别扫描每个锁分区。
	 */
	for (partition = 0; partition < NUM_LOCK_PARTITIONS; partition++)
	{
		LWLock	   *partitionLock;
		dlist_head *procLocks = &(MyProc->myProcLocks[partition]);
		dlist_mutable_iter proclock_iter;

		partitionLock = LockHashPartitionLockByIndex(partition);

		/*
		 * 若此分区的 proclock 链表为空，就可以跳过
		 * 获取分区锁。这项优化比 LockReleaseAll 里的情形
		 * 更安全，因为我们在 AtPrepare_Locks 期间已经去掉了任何快速路径
		 * 锁，因此不可能再有另一个后端往我们的链表里
		 * 添加东西。不过为了安全，这里的写法与 LockReleaseAll
		 * 相同。
		 */
		if (dlist_is_empty(procLocks))
			continue;			/* 不必检查这个分区 */

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_foreach_modify(proclock_iter, procLocks)
		{
			proclock = dlist_container(PROCLOCK, procLink, proclock_iter.cur);

			Assert(proclock->tag.myProc == MyProc);

			lock = proclock->tag.myLock;

			/* 忽略 VXID 锁 */
			if (lock->tag.locktag_type == LOCKTAG_VIRTUALTRANSACTION)
				continue;

			PROCLOCK_PRINT("PostPrepare_Locks", proclock);
			LOCK_PRINT("PostPrepare_Locks", lock, 0);
			Assert(lock->nRequested >= 0);
			Assert(lock->nGranted >= 0);
			Assert(lock->nGranted <= lock->nRequested);
			Assert((proclock->holdMask & ~lock->grantMask) == 0);

			/* 若没有东西要释放，则忽略它（一定是会话级锁） */
			if (proclock->releaseMask == 0)
				continue;

			/* 否则我们应当释放全部锁 */
			if (proclock->releaseMask != proclock->holdMask)
				elog(PANIC, "we seem to have dropped a bit somewhere");

			/*
			 * 无法简单地修改 proclock->tag.myProc 来重新指定
			 * 锁的所有权，因为那是哈希键的一部分，
			 * 改了之后 proclock 就会落在错误的哈希链上。改为
			 * 使用 hash_update_hash_key。（我们以前会创建新的哈希项，
			 * 但若其他进程也在忙着创建 proclock，那就有
			 * 内存不足的风险。）还必须把 proclock 从
			 * 我们的 procLink 链上摘下，再挂到新 proc 的链上。
			 *
			 * 注意：更新后的 proclock 哈希键仍属于
			 * 同一个哈希分区，参见 proclock_hash()。因此我们已经持有的
			 * 分区锁对此足够了。
			 */
			dlist_delete(&proclock->procLink);

			/*
			 * 为 proclock 创建新的哈希键。
			 */
			proclocktag.myLock = lock;
			proclocktag.myProc = newproc;

			/*
			 * 把 groupLeader 指针更新为指向新 proc。（我们最好
			 * 不是别人的锁组的成员！）
			 */
			Assert(proclock->groupLeader == proclock->tag.myProc);
			proclock->groupLeader = newproc;

			/*
			 * 更新该 proclock。不应找到具有相同哈希键的已有项，
			 * 因为对于任何给定的锁，与我自己的 proc 搭配时
			 * 只能有一项。
			 */
			if (!hash_update_hash_key(LockMethodProcLockHash,
									  proclock,
									  &proclocktag))
				elog(PANIC, "duplicate entry found while reassigning a prepared transaction's locks");

			/* 重新链入新 proc 的 proclock 链表 */
			dlist_push_tail(&newproc->myProcLocks[partition], &proclock->procLink);

			PROCLOCK_PRINT("PostPrepare_Locks: updated", proclock);
		}						/* 遍历此分区内的 PROCLOCKs */

		LWLockRelease(partitionLock);
	}							/* 遍历各个分区 */

	END_CRIT_SECTION();
}


/*
 * 估算锁表所用的共享内存空间
 */
Size
LockShmemSize(void)
{
	Size		size = 0;
	long		max_table_size;

	/* 锁哈希表 */
	max_table_size = NLOCKENTS();
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(LOCK)));

	/* proclock 哈希表 */
	max_table_size *= 2;
	size = add_size(size, hash_estimate_size(max_table_size, sizeof(PROCLOCK)));

	/*
	 * 由于 NLOCKENTS 只是估算，加上 10% 的安全余量。
	 */
	size = add_size(size, size / 10);

	return size;
}

/*
 * GetLockStatusData - 返回锁管理器内部状态的一份摘要，
 * 供用户级报告函数使用。
 *
 * 返回数据由一个 LockInstanceData 对象数组组成，
 * 它们是 PROCLOCK 数据结构的轻度抽象，
 * 也就是说，每个唯一的锁和感兴趣的 PGPROC 有一项。
 * 若需要，由调用方负责把相关项对应起来（例如
 * 对同一可锁对象或 PGPROC 的引用）。
 *
 * 设计目标是尽可能短地持有 LWLocks；
 * 因此本函数只是复制必要的数据并释放这些锁，
 * 让调用方可以按自己的意愿
 * 去琢磨和排版数据。
 */
LockData *
GetLockStatusData(void)
{
	LockData   *data;
	PROCLOCK   *proclock;
	HASH_SEQ_STATUS seqstat;
	int			els;
	int			el;
	int			i;

	data = (LockData *) palloc(sizeof(LockData));

	/* 估计我们需要多少空间。 */
	els = MaxBackends;
	el = 0;
	data->locks = (LockInstanceData *) palloc(sizeof(LockInstanceData) * els);

	/*
	 * 首先，我们遍历每个后端的快速路径数组，一次锁住
	 * 一个。这也许会产生不一致的系统状态画面，但同时取得
	 * 所有这些 LWLocks 似乎不切实际（尤其要注意 MAX_SIMUL_LWLOCKS）。
	 * 这应该关系不大，因为这些锁反正都不可能卷入锁
	 * 冲突 --- 任何有可能卷入的都必须出现在主锁
	 * 表里。（出于同样的原因，我们也不为让 leaderPid
	 * 完全有效而操心。若不持有全部分区锁，就无法安全地解引用
	 * 另一个后端的 lockGroupLeader 字段，
	 * 因此不值得
	 * 那样做。）
	 */
	for (i = 0; i < ProcGlobal->allProcCount; ++i)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		uint32		f;

		LWLockAcquire(&proc->fpInfoLock, LW_SHARED);

		for (f = 0; f < FP_LOCK_SLOTS_PER_BACKEND; ++f)
		{
			LockInstanceData *instance;
			uint32		lockbits = FAST_PATH_GET_BITS(proc, f);

			/* 跳过未分配的槽。 */
			if (!lockbits)
				continue;

			if (el >= els)
			{
				els += MaxBackends;
				data->locks = (LockInstanceData *)
					repalloc(data->locks, sizeof(LockInstanceData) * els);
			}

			instance = &data->locks[el];
			SET_LOCKTAG_RELATION(instance->locktag, proc->databaseId,
								 proc->fpRelId[f]);
			instance->holdMask = lockbits << FAST_PATH_LOCKNUMBER_OFFSET;
			instance->waitLockMode = NoLock;
			instance->backend = proc->backendId;
			instance->lxid = proc->lxid;
			instance->pid = proc->pid;
			instance->leaderPid = proc->pid;
			instance->fastpath = true;

			/*
			 * 成功取得快速路径锁意味着当时没有
			 * 冲突的锁。
			 */
			instance->waitStart = 0;

			el++;
		}

		if (proc->fpVXIDLock)
		{
			VirtualTransactionId vxid;
			LockInstanceData *instance;

			if (el >= els)
			{
				els += MaxBackends;
				data->locks = (LockInstanceData *)
					repalloc(data->locks, sizeof(LockInstanceData) * els);
			}

			vxid.backendId = proc->backendId;
			vxid.localTransactionId = proc->fpLocalTransactionId;

			instance = &data->locks[el];
			SET_LOCKTAG_VIRTUALTRANSACTION(instance->locktag, vxid);
			instance->holdMask = LOCKBIT_ON(ExclusiveLock);
			instance->waitLockMode = NoLock;
			instance->backend = proc->backendId;
			instance->lxid = proc->lxid;
			instance->pid = proc->pid;
			instance->leaderPid = proc->pid;
			instance->fastpath = true;
			instance->waitStart = 0;

			el++;
		}

		LWLockRelease(&proc->fpInfoLock);
	}

	/*
	 * 接下来，获取整个共享锁数据结构上的锁。我们这样做，
	 * 是为了至少对主锁表中的锁，状态
	 * 是自洽的。
	 *
	 * 因为这是只读操作，我们取得共享锁而不是
	 * 排他锁。这样做意义不大，因为所有
	 * 正常操作都需要排他锁，但也没什么坏处。它至少允许两个后端
	 * 它至少允许两个后端并行执行 GetLockStatusData。
	 * 这一点只是附带的好处。
	 *
	 * 必须按分区号顺序抓取 LWLocks，以免 LWLock 死锁。
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

	/* 现在可以安全地统计 proclock 的个数 */
	data->nelements = el + hash_get_num_entries(LockMethodProcLockHash);
	if (data->nelements > els)
	{
		els = data->nelements;
		data->locks = (LockInstanceData *)
			repalloc(data->locks, sizeof(LockInstanceData) * els);
	}

	/* 现在扫描这些表以复制数据 */
	hash_seq_init(&seqstat, LockMethodProcLockHash);

	while ((proclock = (PROCLOCK *) hash_seq_search(&seqstat)))
	{
		PGPROC	   *proc = proclock->tag.myProc;
		LOCK	   *lock = proclock->tag.myLock;
		LockInstanceData *instance = &data->locks[el];

		memcpy(&instance->locktag, &lock->tag, sizeof(LOCKTAG));
		instance->holdMask = proclock->holdMask;
		if (proc->waitLock == proclock->tag.myLock)
			instance->waitLockMode = proc->waitLockMode;
		else
			instance->waitLockMode = NoLock;
		instance->backend = proc->backendId;
		instance->lxid = proc->lxid;
		instance->pid = proc->pid;
		instance->leaderPid = proclock->groupLeader->pid;
		instance->fastpath = false;
		instance->waitStart = (TimestampTz) pg_atomic_read_u64(&proc->waitStart);

		el++;
	}

	/*
	 * 然后释放锁。按相反顺序做有两个原因：(1)
	 * 其他任何需要不止一把锁的人都会试图按递增顺序锁住
	 * 它们；我们不想在对方还拿不到所需的全部锁时就放开它。
	 * (2) 这避免了 LWLockRelease 内部的 O(N^2)
	 * 行为。
	 */
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(LockHashPartitionLockByIndex(i));

	Assert(el == data->nelements);

	return data;
}

/*
 * GetBlockerStatusData - 返回锁管理器状态中与
 * 正在阻塞指定 PIDs 或其锁组任何成员的锁有关的摘要，
 * 供用户级报告函数使用。
 *
 * 对于锁组内正在等待某把重量级锁的每个 PID，
 * 返回数据包含一个 LockInstanceData 对象数组，它们与
 * GetLockStatusData 使用的数据结构相同；但与那个函数不同，
 * 本函数只报告与该 PID 被阻塞于其上的那把锁相关联的
 * PROCLOCKs。（因此，对任何一个被阻塞的 PID，所有 locktag 都应当相同。）
 * 此外，我们还返回该锁等待队列中排在被阻塞 PID 前面的那些后端的
 * PID 数组。可以把它们与 LockInstanceData 对象中的 PID 比较，
 * 以判断哪些等待者在队列中位于
 * 被阻塞 PID 的前面或后面。
 *
 * 若 blocked_pid 不是有效的后端 PID，或其锁组中没有东西
 * 在等待任何重量级锁，则返回空数组。
 *
 * 设计目标是尽可能短地持有 LWLocks；
 * 因此本函数只是复制必要的数据并释放这些锁，
 * 让调用方可以按自己的意愿
 * 去琢磨和排版数据。
 */
BlockedProcsData *
GetBlockerStatusData(int blocked_pid)
{
	BlockedProcsData *data;
	PGPROC	   *proc;
	int			i;

	data = (BlockedProcsData *) palloc(sizeof(BlockedProcsData));

	/*
	 * 估计我们需要多少空间，并预先分配。大多数时候
	 * 这样就能避免在持有 LWLocks 期间做 repalloc。（我们
	 * 假定 --- 并用 Assert 检查 --- MaxBackends 对 procs[] 数组来说足够；
	 * 另外两个数组则可能需要扩大。）
	 */
	data->nprocs = data->nlocks = data->npids = 0;
	data->maxprocs = data->maxlocks = data->maxpids = MaxBackends;
	data->procs = (BlockedProcData *) palloc(sizeof(BlockedProcData) * data->maxprocs);
	data->locks = (LockInstanceData *) palloc(sizeof(LockInstanceData) * data->maxlocks);
	data->waiter_pids = (int *) palloc(sizeof(int) * data->maxpids);

	/*
	 * 为了在 ProcArray 中搜索 blocked_pid，并假定该项
	 * 不会立刻从我们脚下消失，必须持有 ProcArrayLock。
	 * 此外，要检查任何其他后端的锁分组字段，
	 * 必须持有全部哈希分区锁。（对任何一个锁组，其中只有一把
	 * 锁真正相关，但我们无法事先知道是哪一把。）
	 * 在整个过程中持有所有这些锁相当烦人，但并不比
	 * GetLockStatusData() 更糟，而且它确实有一个好处：我们保证返回的是
	 * 一份自洽的
	 * 瞬时状态。
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	proc = BackendPidGetProcWithLock(blocked_pid);

	/* 若它已经消失，就无事可做 */
	if (proc != NULL)
	{
		/*
		 * 获取整个共享锁数据结构上的锁。见
		 * GetLockStatusData() 中的说明。
		 */
		for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
			LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

		if (proc->lockGroupLeader == NULL)
		{
			/* 简单情形：proc 不是锁组成员 */
			GetSingleProcBlockerStatusData(proc, data);
		}
		else
		{
			/* 检查 proc 锁组中的全部 proc */
			dlist_iter	iter;

			dlist_foreach(iter, &proc->lockGroupLeader->lockGroupMembers)
			{
				PGPROC	   *memberProc;

				memberProc = dlist_container(PGPROC, lockGroupLink, iter.cur);
				GetSingleProcBlockerStatusData(memberProc, data);
			}
		}

		/*
		 * 然后释放锁。见 GetLockStatusData() 中的说明。
		 */
		for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
			LWLockRelease(LockHashPartitionLockByIndex(i));

		Assert(data->nprocs <= data->maxprocs);
	}

	LWLockRelease(ProcArrayLock);

	return data;
}

/* 为 GetBlockerStatusData 积累一个也许被阻塞的 proc 的数据 */
static void
GetSingleProcBlockerStatusData(PGPROC *blocked_proc, BlockedProcsData *data)
{
	LOCK	   *theLock = blocked_proc->waitLock;
	BlockedProcData *bproc;
	dlist_iter	proclock_iter;
	dlist_iter	proc_iter;
	dclist_head *waitQueue;
	int			queue_size;

	/* 若此 proc 未被阻塞，则无事可做 */
	if (theLock == NULL)
		return;

	/* 设置一个 procs[] 元素 */
	bproc = &data->procs[data->nprocs++];
	bproc->pid = blocked_proc->pid;
	bproc->first_lock = data->nlocks;
	bproc->first_waiter = data->npids;

	/*
	 * 可以忽略该 proc 的快速路径数组，因为其中没有任何东西
	 * 会与一把被争用的锁有关。
	 */

	/* 收集与 theLock 相关联的全部 PROCLOCKs */
	dlist_foreach(proclock_iter, &theLock->procLocks)
	{
		PROCLOCK   *proclock =
			dlist_container(PROCLOCK, lockLink, proclock_iter.cur);
		PGPROC	   *proc = proclock->tag.myProc;
		LOCK	   *lock = proclock->tag.myLock;
		LockInstanceData *instance;

		if (data->nlocks >= data->maxlocks)
		{
			data->maxlocks += MaxBackends;
			data->locks = (LockInstanceData *)
				repalloc(data->locks, sizeof(LockInstanceData) * data->maxlocks);
		}

		instance = &data->locks[data->nlocks];
		memcpy(&instance->locktag, &lock->tag, sizeof(LOCKTAG));
		instance->holdMask = proclock->holdMask;
		if (proc->waitLock == lock)
			instance->waitLockMode = proc->waitLockMode;
		else
			instance->waitLockMode = NoLock;
		instance->backend = proc->backendId;
		instance->lxid = proc->lxid;
		instance->pid = proc->pid;
		instance->leaderPid = proclock->groupLeader->pid;
		instance->fastpath = false;
		data->nlocks++;
	}

	/* 若 waiter_pids[] 太小，装不下等待队列的全部 PIDs，就扩大它 */
	waitQueue = &(theLock->waitProcs);
	queue_size = dclist_count(waitQueue);

	if (queue_size > data->maxpids - data->npids)
	{
		data->maxpids = Max(data->maxpids + MaxBackends,
							data->npids + queue_size);
		data->waiter_pids = (int *) repalloc(data->waiter_pids,
											 sizeof(int) * data->maxpids);
	}

	/* 从该锁的等待队列收集 PIDs，到 blocked_proc 为止 */
	dclist_foreach(proc_iter, waitQueue)
	{
		PGPROC	   *queued_proc = dlist_container(PGPROC, links, proc_iter.cur);

		if (queued_proc == blocked_proc)
			break;
		data->waiter_pids[data->npids++] = queued_proc->pid;
		queued_proc = (PGPROC *) queued_proc->links.next;
	}

	bproc->num_locks = data->nlocks - bproc->first_lock;
	bproc->num_waiters = data->npids - bproc->first_waiter;
}

/*
 * 返回当前持有的 AccessExclusiveLocks 列表，供
 * LogStandbySnapshot() 使用。结果是一个 palloc 出来的数组，
 * 元素个数返回到 *nlocks。
 *
 * XXX 目前这会锁住锁表的全部分区，
 * 但有可能做得更好。若对锁做引用计数，并把该值存在
 * 每个后端的 ProcArray 项里，我们就能知道是否有任何
 * 锁需要记录，而不必取得分区锁并
 * 扫描锁表。不过这是否值得那份额外开销，
 * 相当可疑。
 */
xl_standby_lock *
GetRunningTransactionLocks(int *nlocks)
{
	xl_standby_lock *accessExclusiveLocks;
	PROCLOCK   *proclock;
	HASH_SEQ_STATUS seqstat;
	int			i;
	int			index;
	int			els;

	/*
	 * 获取整个共享锁数据结构上的锁。
	 *
	 * 必须按分区号顺序抓取 LWLocks，以免 LWLock 死锁。
	 */
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		LWLockAcquire(LockHashPartitionLockByIndex(i), LW_SHARED);

	/* 现在可以安全地统计 proclock 的个数 */
	els = hash_get_num_entries(LockMethodProcLockHash);

	/*
	 * 为锁表中的全部锁分配足够空间是过头了，
	 * 但比不得不扩大数组更方便，也更快。
	 */
	accessExclusiveLocks = palloc(els * sizeof(xl_standby_lock));

	/* 现在扫描这些表以复制数据 */
	hash_seq_init(&seqstat, LockMethodProcLockHash);

	/*
	 * 若锁是当前已授予的 AccessExclusiveLock，则它只会有
	 * 一个 proclock 持有者，因此在这种特定情形下锁不会被访问两次。
	 * 不要把这段代码复制到别处使用，因为在
	 * 一般情形下，查看非排他锁类型时
	 * 这会给出重复的锁。
	 */
	index = 0;
	while ((proclock = (PROCLOCK *) hash_seq_search(&seqstat)))
	{
		/* 确保此定义与 LockAcquire 中使用的定义一致 */
		if ((proclock->holdMask & LOCKBIT_ON(AccessExclusiveLock)) &&
			proclock->tag.myLock->tag.locktag_type == LOCKTAG_RELATION)
		{
			PGPROC	   *proc = proclock->tag.myProc;
			LOCK	   *lock = proclock->tag.myLock;
			TransactionId xid = proc->xid;

			/*
			 * 若我们知道事务已经发出了提交的 WAL 记录、但尚未释放
			 * 锁，就不要记录它们的锁。仍然有可能看到已经
			 * 完成的事务所持有的锁，如果它们还没有把 xid
			 * 清零。
			 */
			if (!TransactionIdIsValid(xid))
				continue;

			accessExclusiveLocks[index].xid = xid;
			accessExclusiveLocks[index].dbOid = lock->tag.locktag_field1;
			accessExclusiveLocks[index].relOid = lock->tag.locktag_field2;

			index++;
		}
	}

	Assert(index <= els);

	/*
	 * 然后释放锁。按相反顺序做有两个原因：(1)
	 * 其他任何需要不止一把锁的人都会试图按递增顺序锁住
	 * 它们；我们不想在对方还拿不到所需的全部锁时就放开它。
	 * (2) 这避免了 LWLockRelease 内部的 O(N^2)
	 * 行为。
	 */
	for (i = NUM_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(LockHashPartitionLockByIndex(i));

	*nlocks = index;
	return accessExclusiveLocks;
}

/* 提供任何锁模式的文字名称 */
const char *
GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode)
{
	Assert(lockmethodid > 0 && lockmethodid < lengthof(LockMethods));
	Assert(mode > 0 && mode <= LockMethods[lockmethodid]->numLockModes);
	return LockMethods[lockmethodid]->lockModeNames[mode];
}

#ifdef LOCK_DEBUG
/*
 * 转储给定 proc 的 myProcLocks 链表中的全部锁。
 *
 * 调用方负责已经取得适当的 LWLocks。
 */
void
DumpLocks(PGPROC *proc)
{
	int			i;

	if (proc == NULL)
		return;

	if (proc->waitLock)
		LOCK_PRINT("DumpLocks: waiting on", proc->waitLock, 0);

	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
	{
		dlist_head *procLocks = &proc->myProcLocks[i];
		dlist_iter	iter;

		dlist_foreach(iter, procLocks)
		{
			PROCLOCK   *proclock = dlist_container(PROCLOCK, procLink, iter.cur);
			LOCK	   *lock = proclock->tag.myLock;

			Assert(proclock->tag.myProc == proc);
			PROCLOCK_PRINT("DumpLocks", proclock);
			LOCK_PRINT("DumpLocks", lock, 0);
		}
	}
}

/*
 * 转储全部锁管理器锁。
 *
 * 调用方负责已经取得适当的 LWLocks。
 */
void
DumpAllLocks(void)
{
	PGPROC	   *proc;
	PROCLOCK   *proclock;
	LOCK	   *lock;
	HASH_SEQ_STATUS status;

	proc = MyProc;

	if (proc && proc->waitLock)
		LOCK_PRINT("DumpAllLocks: waiting on", proc->waitLock, 0);

	hash_seq_init(&status, LockMethodProcLockHash);

	while ((proclock = (PROCLOCK *) hash_seq_search(&status)) != NULL)
	{
		PROCLOCK_PRINT("DumpAllLocks", proclock);

		lock = proclock->tag.myLock;
		if (lock)
			LOCK_PRINT("DumpAllLocks", lock, 0);
		else
			elog(LOG, "DumpAllLocks: proclock->tag.myLock = NULL");
	}
}
#endif							/* LOCK_DEBUG（锁调试） */

/*
 * LOCK 两阶段提交资源管理器的例程
 */

/*
 * 重新获取属于某个已预备事务的锁。
 *
 * 因为本函数在数据库启动时运行，重新获取这些锁不应当
 * 与正在运行的事务冲突，因为当时一个都没有。我们
 * 假定所存储的两阶段提交文件所代表的锁状态是合法的。
 *
 * 从热备模式切换到正常运行时，这些锁将
 * 已经由启动进程持有。为新的 proc 获取锁时
 * 不检查冲突，因此启动进程与哑 proc 之间不会产生冲突，
 * 即便我们会短暂地出现两个 proc 持有同一把 AccessExclusiveLock
 * 的情形，而这通常因为冲突而不可能。
 * 若处于备库模式、恢复快照却尚未建立，
 * 则有可能并非全部、而只是其中一部分锁
 * 已经由启动进程持有。
 *
 * 这种方法简单，但也有点危险：若没有足够的共享内存
 * 来获取这些锁，就会抛出错误，并被提升为 FATAL，
 * 恢复将中止，从而拖垮 postmaster。
 * 更安全的做法是像 AtPrepare_Locks 那样转移这些锁，
 * 但话说回来，在热备模式下，只读后端反正有可能
 * 用光全部共享锁内存，以至于重放需要获取锁的
 * WAL 记录时仍会抛出错误
 * 并 PANIC。
 */
void
lock_twophase_recover(TransactionId xid, uint16 info,
					  void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid, false);
	LOCKTAG    *locktag;
	LOCKMODE	lockmode;
	LOCKMETHODID lockmethodid;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	PROCLOCKTAG proclocktag;
	bool		found;
	uint32		hashcode;
	uint32		proclock_hashcode;
	int			partition;
	LWLock	   *partitionLock;
	LockMethod	lockMethodTable;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

	hashcode = LockTagHashCode(locktag);
	partition = LockHashPartition(hashcode);
	partitionLock = LockHashPartitionLock(hashcode);

	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/*
	 * 找出或创建带有此标签的锁。
	 */
	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_ENTER_NULL,
												&found);
	if (!lock)
	{
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase %s.", "max_locks_per_transaction")));
	}

	/*
	 * 若是新建的锁对象，则初始化它
	 */
	if (!found)
	{
		lock->grantMask = 0;
		lock->waitMask = 0;
		dlist_init(&lock->procLocks);
		dclist_init(&lock->waitProcs);
		lock->nRequested = 0;
		lock->nGranted = 0;
		MemSet(lock->requested, 0, sizeof(int) * MAX_LOCKMODES);
		MemSet(lock->granted, 0, sizeof(int) * MAX_LOCKMODES);
		LOCK_PRINT("lock_twophase_recover: new", lock, lockmode);
	}
	else
	{
		LOCK_PRINT("lock_twophase_recover: found", lock, lockmode);
		Assert((lock->nRequested >= 0) && (lock->requested[lockmode] >= 0));
		Assert((lock->nGranted >= 0) && (lock->granted[lockmode] >= 0));
		Assert(lock->nGranted <= lock->nRequested);
	}

	/*
	 * 为 proclock 表创建哈希键。
	 */
	proclocktag.myLock = lock;
	proclocktag.myProc = proc;

	proclock_hashcode = ProcLockHashCode(&proclocktag, hashcode);

	/*
	 * 找出或创建带有此标签的 proclock 项
	 */
	proclock = (PROCLOCK *) hash_search_with_hash_value(LockMethodProcLockHash,
														&proclocktag,
														proclock_hashcode,
														HASH_ENTER_NULL,
														&found);
	if (!proclock)
	{
		/* 糟糕，proclock 的共享内存不够 */
		if (lock->nRequested == 0)
		{
			/*
			 * 没有其他请求者在请求这把锁，因此回收
			 * 该锁对象。我们必须这样做，以免共享内存
			 * 永久泄漏，因为此后不会再有任何事情
			 * 促使别人释放这个锁对象。
			 */
			Assert(dlist_is_empty(&lock->procLocks));
			if (!hash_search_with_hash_value(LockMethodLockHash,
											 &(lock->tag),
											 hashcode,
											 HASH_REMOVE,
											 NULL))
				elog(PANIC, "lock table corrupted");
		}
		LWLockRelease(partitionLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase %s.", "max_locks_per_transaction")));
	}

	/*
	 * 若是新建的，则初始化新项
	 */
	if (!found)
	{
		Assert(proc->lockGroupLeader == NULL);
		proclock->groupLeader = proc;
		proclock->holdMask = 0;
		proclock->releaseMask = 0;
		/* 把 proclock 加入相应的链表 */
		dlist_push_tail(&lock->procLocks, &proclock->lockLink);
		dlist_push_tail(&proc->myProcLocks[partition],
						&proclock->procLink);
		PROCLOCK_PRINT("lock_twophase_recover: new", proclock);
	}
	else
	{
		PROCLOCK_PRINT("lock_twophase_recover: found", proclock);
		Assert((proclock->holdMask & ~lock->grantMask) == 0);
	}

	/*
	 * lock->nRequested 与 lock->requested[] 统计请求总数，
	 * 无论已授予还是在等待，因此立刻增加它们。
	 */
	lock->nRequested++;
	lock->requested[lockmode]++;
	Assert((lock->nRequested > 0) && (lock->requested[lockmode] > 0));

	/*
	 * 我们不应已经持有所要的锁。
	 */
	if (proclock->holdMask & LOCKBIT_ON(lockmode))
		elog(ERROR, "lock %s on object %u/%u/%u is already held",
			 lockMethodTable->lockModeNames[lockmode],
			 lock->tag.locktag_field1, lock->tag.locktag_field2,
			 lock->tag.locktag_field3);

	/*
	 * 我们忽略任何可能的冲突，直接把锁授予自己。不仅因为
	 * 我们懒得检查，也是为了避免从备库切换到正常模式时
	 * 发生死锁。见函数注释。
	 */
	GrantLock(lock, proclock, lockmode);

	/*
	 * 增加强锁计数，以确保任何快速路径锁请求在未查阅
	 * 主锁表的情况下不会被授予。
	 */
	if (ConflictsWithRelationFastPath(&lock->tag, lockmode))
	{
		uint32		fasthashcode = FastPathStrongLockHashPartition(hashcode);

		SpinLockAcquire(&FastPathStrongRelationLocks->mutex);
		FastPathStrongRelationLocks->count[fasthashcode]++;
		SpinLockRelease(&FastPathStrongRelationLocks->mutex);
	}

	LWLockRelease(partitionLock);
}

/*
 * 在以热备模式启动时，重新获取属于某个
 * 已预备事务的锁。
 */
void
lock_twophase_standby_recover(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	LOCKTAG    *locktag;
	LOCKMODE	lockmode;
	LOCKMETHODID lockmethodid;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmode = rec->lockmode;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	if (lockmode == AccessExclusiveLock &&
		locktag->locktag_type == LOCKTAG_RELATION)
	{
		StandbyAcquireAccessExclusiveLock(xid,
										  locktag->locktag_field1 /* dboid（数据库 OID） */ ,
										  locktag->locktag_field2 /* reloid（关系 OID） */ );
	}
}


/*
 * COMMIT PREPARED 情形的两阶段提交处理例程。
 *
 * 找出并释放两阶段提交记录所指示的锁。
 */
void
lock_twophase_postcommit(TransactionId xid, uint16 info,
						 void *recdata, uint32 len)
{
	TwoPhaseLockRecord *rec = (TwoPhaseLockRecord *) recdata;
	PGPROC	   *proc = TwoPhaseGetDummyProc(xid, true);
	LOCKTAG    *locktag;
	LOCKMETHODID lockmethodid;
	LockMethod	lockMethodTable;

	Assert(len == sizeof(TwoPhaseLockRecord));
	locktag = &rec->locktag;
	lockmethodid = locktag->locktag_lockmethodid;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);
	lockMethodTable = LockMethods[lockmethodid];

	LockRefindAndRelease(lockMethodTable, proc, locktag, rec->lockmode, true);
}

/*
 * ROLLBACK PREPARED 情形的两阶段提交处理例程。
 *
 * 这实际上与 COMMIT 情形相同。
 */
void
lock_twophase_postabort(TransactionId xid, uint16 info,
						void *recdata, uint32 len)
{
	lock_twophase_postcommit(xid, info, recdata, len);
}

/*
 *		VirtualXactLockTableInsert
 *
 *		经由快速路径取得 vxid 锁。不可能有预先存在的
 *		加锁者，因为我们还没有经由 ProcArray 公布这个 vxid。
 *
 *		既然 MyProc->fpLocalTransactionId 通常会包含与
 *		MyProc->lxid 相同的数据，你也许会想两者是否真的都需要。
 *		差别在于 MyProc->lxid 的设置和清除不加锁，并且由
 *		procarray.c 检查，而 fpLocalTransactionId 由 fpInfoLock 保护，
 *		并且只被锁子系统使用。这样做更容易验证
 *		不存在古怪的竞争条件。
 *
 *		我们不费心把这把锁记入本地锁表，因为它
 *		只在事务结束时释放。取而代之的是，
 *		LockReleaseAll() 调用 VirtualXactLockTableCleanup()。
 */
void
VirtualXactLockTableInsert(VirtualTransactionId vxid)
{
	Assert(VirtualTransactionIdIsValid(vxid));

	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	Assert(MyProc->backendId == vxid.backendId);
	Assert(MyProc->fpLocalTransactionId == InvalidLocalTransactionId);
	Assert(MyProc->fpVXIDLock == false);

	MyProc->fpVXIDLock = true;
	MyProc->fpLocalTransactionId = vxid.localTransactionId;

	LWLockRelease(&MyProc->fpInfoLock);
}

/*
 *		VirtualXactLockTableCleanup
 *
 *		检查 VXID 锁是否已经被物化；若是，则释放它，
 *		从而解除等待者的阻塞。
 */
void
VirtualXactLockTableCleanup(void)
{
	bool		fastpath;
	LocalTransactionId lxid;

	Assert(MyProc->backendId != InvalidBackendId);

	/*
	 * 清理共享内存状态。
	 */
	LWLockAcquire(&MyProc->fpInfoLock, LW_EXCLUSIVE);

	fastpath = MyProc->fpVXIDLock;
	lxid = MyProc->fpLocalTransactionId;
	MyProc->fpVXIDLock = false;
	MyProc->fpLocalTransactionId = InvalidLocalTransactionId;

	LWLockRelease(&MyProc->fpInfoLock);

	/*
	 * 若 fpVXIDLock 被清掉、而 fpLocalTransactionId 未被触碰，
	 * 那就意味着有人把这把锁转移到了主锁表。
	 */
	if (!fastpath && LocalTransactionIdIsValid(lxid))
	{
		VirtualTransactionId vxid;
		LOCKTAG		locktag;

		vxid.backendId = MyBackendId;
		vxid.localTransactionId = lxid;
		SET_LOCKTAG_VIRTUALTRANSACTION(locktag, vxid);

		LockRefindAndRelease(LockMethods[DEFAULT_LOCKMETHOD], MyProc,
							 &locktag, ExclusiveLock, false);
	}
}

/*
 *		XactLockForVirtualXact
 *
 * 若 TransactionIdIsValid(xid)，这本质上就是 XactLockTableWait(xid,
 * NULL, NULL, XLTW_None) 或 ConditionalXactLockTableWait(xid)。与那些
 * 函数不同，它假定 "xid" 绝不是子事务，并且 "xid" 已经
 * 预备、提交或中止。
 *
 * 若 !TransactionIdIsValid(xid)，这就锁住每一个在其 PREPARE TRANSACTION
 * 之前曾以 "vxid" 为人所知的已预备 XID。
 */
static bool
XactLockForVirtualXact(VirtualTransactionId vxid,
					   TransactionId xid, bool wait)
{
	bool		more = false;

	/* 若你没有两阶段提交事务，就没有必要等待它们。 */
	if (max_prepared_xacts == 0)
		return true;

	do
	{
		LockAcquireResult lar;
		LOCKTAG		tag;

		/* 清除上一次迭代留下的状态。 */
		if (more)
		{
			xid = InvalidTransactionId;
			more = false;
		}

		/* 若我们没有 xid，就试着找一个。 */
		if (!TransactionIdIsValid(xid))
			xid = TwoPhaseGetXidByVirtualXID(vxid, &more);
		if (!TransactionIdIsValid(xid))
		{
			Assert(!more);
			return true;
		}

		/* 检查或等待 XID 完成。 */
		SET_LOCKTAG_TRANSACTION(tag, xid);
		lar = LockAcquire(&tag, ShareLock, false, !wait);
		if (lar == LOCKACQUIRE_NOT_AVAIL)
			return false;
		LockRelease(&tag, ShareLock, false);
	} while (more);

	return true;
}

/*
 *		VirtualXactLock
 *
 * 若 wait = true，就一直等到给定的 VXID 或同一事务取得的任何 XIDs
 * 仍在运行。然后返回真。
 *
 * 若 wait = false，只检查该 VXID 或那些 XID 之一是否仍在
 * 运行，并返回真或假。
 */
bool
VirtualXactLock(VirtualTransactionId vxid, bool wait)
{
	LOCKTAG		tag;
	PGPROC	   *proc;
	TransactionId xid = InvalidTransactionId;

	Assert(VirtualTransactionIdIsValid(vxid));

	if (VirtualTransactionIdIsRecoveredPreparedXact(vxid))
		/* 没有 vxid 锁；localTransactionId 是一个普通的、已加锁的 XID */
		return XactLockForVirtualXact(vxid, vxid.localTransactionId, wait);

	SET_LOCKTAG_VIRTUALTRANSACTION(tag, vxid);

	/*
	 * 若必须建立锁表项，这就是必须代表其行事的那个 PGPROC。
	 * 注意，事务也许会结束，或者 PGPROC 也许会在我们着手检查它之前
	 * 被重新指派给一个新后端，但这没关系。若检查时发现
	 * 相关的 lxid 已不再在这里运行，就足以证明
	 * 它已不再在
	 * 任何地方运行。
	 */
	proc = BackendIdGetProc(vxid.backendId);
	if (proc == NULL)
		return XactLockForVirtualXact(vxid, InvalidTransactionId, wait);

	/*
	 * 必须先取得这把锁，再拿 backendId 和 lxid
	 * 与我们正在等待的那些相比较。目标后端只会在持有
	 * 这把锁时设置或清除 lxid。
	 */
	LWLockAcquire(&proc->fpInfoLock, LW_EXCLUSIVE);

	if (proc->backendId != vxid.backendId
		|| proc->fpLocalTransactionId != vxid.localTransactionId)
	{
		/* VXID 已结束 */
		LWLockRelease(&proc->fpInfoLock);
		return XactLockForVirtualXact(vxid, InvalidTransactionId, wait);
	}

	/*
	 * 若并未要求我们等待，就不必建立锁表
	 * 项。事务仍在进行，因此直接返回假。
	 */
	if (!wait)
	{
		LWLockRelease(&proc->fpInfoLock);
		return false;
	}

	/*
	 * 好，我们需要在这个 VXID 上睡眠。但首先，必须在需要时
	 * 建立主锁表项（也就是把该 proc 在其 VXID 上的
	 * 快速路径锁转换成普通锁）。
	 */
	if (proc->fpVXIDLock)
	{
		PROCLOCK   *proclock;
		uint32		hashcode;
		LWLock	   *partitionLock;

		hashcode = LockTagHashCode(&tag);

		partitionLock = LockHashPartitionLock(hashcode);
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		proclock = SetupLockInTable(LockMethods[DEFAULT_LOCKMETHOD], proc,
									&tag, hashcode, ExclusiveLock);
		if (!proclock)
		{
			LWLockRelease(partitionLock);
			LWLockRelease(&proc->fpInfoLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory"),
					 errhint("You might need to increase %s.", "max_locks_per_transaction")));
		}
		GrantLock(proclock->tag.myLock, proclock, ExclusiveLock);

		LWLockRelease(partitionLock);

		proc->fpVXIDLock = false;
	}

	/*
	 * 若该 proc 现在有一个 XID，我们就可以避免一次 TwoPhaseGetXidByVirtualXID()
	 * 搜索。该 proc 也许已经分配了这个 XID 但尚未锁住它，
	 * 此时该 proc 会在释放 VXID 之前锁住这个 XID。
	 * fpInfoLock 临界区排除了 VirtualXactLockTableCleanup()，
	 * 因此我们不会保存另一个 VXID 的 XID。在建立主锁表项之前
	 * 还是之后保存这个 XID，都没有关系。
	 */
	xid = proc->xid;

	/* proc->fpLockBits 已经用完 */
	LWLockRelease(&proc->fpInfoLock);

	/* 该等待了。 */
	(void) LockAcquire(&tag, ShareLock, false, false);

	LockRelease(&tag, ShareLock, false);
	return XactLockForVirtualXact(vxid, xid, wait);
}

/*
 * LockWaiterCount
 *
 * 找出此 locktag 上的锁请求者个数
 */
int
LockWaiterCount(const LOCKTAG *locktag)
{
	LOCKMETHODID lockmethodid = locktag->locktag_lockmethodid;
	LOCK	   *lock;
	bool		found;
	uint32		hashcode;
	LWLock	   *partitionLock;
	int			waiters = 0;

	if (lockmethodid <= 0 || lockmethodid >= lengthof(LockMethods))
		elog(ERROR, "unrecognized lock method: %d", lockmethodid);

	hashcode = LockTagHashCode(locktag);
	partitionLock = LockHashPartitionLock(hashcode);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	lock = (LOCK *) hash_search_with_hash_value(LockMethodLockHash,
												locktag,
												hashcode,
												HASH_FIND,
												&found);
	if (found)
	{
		Assert(lock != NULL);
		waiters = lock->nRequested;
	}
	LWLockRelease(partitionLock);

	return waiters;
}
