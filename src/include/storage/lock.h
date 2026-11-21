/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES 低层锁机制
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#ifdef FRONTEND
#error "lock.h may not be included from frontend code"
#endif

#include "lib/ilist.h"
#include "storage/backendid.h"
#include "storage/lockdefs.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* struct PGPROC 在 proc.h 中声明，此处须前向引用 */
typedef struct PGPROC PGPROC;

/* GUC 变量 */
extern PGDLLIMPORT int max_locks_per_xact;

#ifdef LOCK_DEBUG
extern PGDLLIMPORT int Trace_lock_oidmin;
extern PGDLLIMPORT bool Trace_locks;
extern PGDLLIMPORT bool Trace_userlocks;
extern PGDLLIMPORT int Trace_lock_table;
extern PGDLLIMPORT bool Debug_deadlocks;
#endif							/* LOCK_DEBUG */


/*
 * 顶层事务由 VirtualTransactionIDs 标识，含 PGPROC 的
 * backendId 与 lxid。对已恢复的预备事务，
 * LocalTransactionId 是普通 XID；LOCKTAG_VIRTUALTRANSACTION
 * 从不论及那种。它们短期内唯一，但数据库重启或
 * XID 回绕后会重用；故切勿
 * 存到磁盘上。
 *
 * 注意：不能假定整个 VirtualTransactionId 结构可原子
 * 赋值。但假定 LocalTransactionId 可原子赋值，且
 * backend ID 很少变化，不成问题，故可分别取/赋这两
 * 个字段。我们刻意不在 PGPROC 内用该结构，以防编码
 * 错误对其做结构体赋值；请改用
 * 错误对其做结构体赋值；请改用
 * GET_VXID_FROM_PGPROC().
 */
typedef struct
{
	BackendId	backendId;		/* PGPROC 的 backendId */
	LocalTransactionId localTransactionId;	/* PGPROC 的 lxid */
} VirtualTransactionId;

#define InvalidLocalTransactionId		0
#define LocalTransactionIdIsValid(lxid) ((lxid) != InvalidLocalTransactionId)
#define VirtualTransactionIdIsValid(vxid) \
	(LocalTransactionIdIsValid((vxid).localTransactionId))
#define VirtualTransactionIdIsRecoveredPreparedXact(vxid) \
	((vxid).backendId == InvalidBackendId)
#define VirtualTransactionIdEquals(vxid1, vxid2) \
	((vxid1).backendId == (vxid2).backendId && \
	 (vxid1).localTransactionId == (vxid2).localTransactionId)
#define SetInvalidVirtualTransactionId(vxid) \
	((vxid).backendId = InvalidBackendId, \
	 (vxid).localTransactionId = InvalidLocalTransactionId)
#define GET_VXID_FROM_PGPROC(vxid, proc) \
	((vxid).backendId = (proc).backendId, \
	 (vxid).localTransactionId = (proc).lxid)

/* MAX_LOCKMODES 无法大于 LOCKMASK 的位数 */
#define MAX_LOCKMODES		10

#define LOCKBIT_ON(lockmode) (1 << (lockmode))
#define LOCKBIT_OFF(lockmode) (~(1 << (lockmode)))


/*
 * 本数据结构定义与某一"锁方法"相关的加锁语义。
 * 语义规定每种锁模式的含义（通过定义它与哪些
 * 锁模式冲突）。
 * 全部数据为常量，保存在 const 表中。
 *
 * numLockModes -- 本锁方法定义的锁模式数
 *		（READ、WRITE 等）。必须小于 MAX_LOCKMODES。
 *
 * conflictTab -- 锁模式冲突位掩码数组。
 *		conflictTab[i] 第 j 位为模式 i 与 j 冲突。
 *		锁模式编号为 1..numLockModes；
 *		conflictTab[0] 不用。
 *
 * lockModeNames -- 调试打印用 ID 串。
 *
 * trace_flag -- 指向本锁方法 GUC 跟踪标志的指针。
 * （GUC 变量非常量，但此处用 const 表示不能通过本
 * 引用去改它。）
 */
typedef struct LockMethodData
{
	int			numLockModes;
	const LOCKMASK *conflictTab;
	const char *const *lockModeNames;
	const bool *trace_flag;
} LockMethodData;

typedef const LockMethodData *LockMethod;

/*
 * 锁方法由 LOCKMETHODID 标识。（虽声明为 uint16，
 * 但受 LOCKTAG 布局所限，锁方法至多 256 种。）
 */
typedef uint16 LOCKMETHODID;

/* 标识已知的锁方法 */
#define DEFAULT_LOCKMETHOD	1
#define USER_LOCKMETHOD		2

/*
 * LOCKTAG 是在锁散列表中查找 LOCK 项所需的键信息。
 * 一个 LOCKTAG 值唯一标识一个可加锁对象。
 *
 * LockTagType 枚举定义我们能加锁的各类对象。
 * 最多可处理 256 种 LockTagTypes。
 */
typedef enum LockTagType
{
	LOCKTAG_RELATION,			/* 整个关系 */
	LOCKTAG_RELATION_EXTEND,	/* 扩展关系的权利 */
	LOCKTAG_DATABASE_FROZEN_IDS,	/* pg_database.datfrozenxid */
	LOCKTAG_PAGE,				/* 关系的一页 */
	LOCKTAG_TUPLE,				/* 一个物理元组 */
	LOCKTAG_TRANSACTION,		/* 事务（用于等待其完成） */
	LOCKTAG_VIRTUALTRANSACTION, /* 虚拟事务（同上） */
	LOCKTAG_SPECULATIVE_TOKEN,	/* 推测插入的 Xid 与 token */
	LOCKTAG_OBJECT,				/* 非关系的数据库对象 */
	LOCKTAG_USERLOCK,			/* 留给旧 contrib/userlock 代码 */
	LOCKTAG_ADVISORY,			/* 咨询性用户锁 */
	LOCKTAG_APPLY_TRANSACTION	/* 逻辑复制订阅端正在应用的
								 * 事务 */
} LockTagType;

#define LOCKTAG_LAST_TYPE	LOCKTAG_APPLY_TRANSACTION

extern PGDLLIMPORT const char *const LockTagTypeNames[];

/*
 * LOCKTAG 结构刻意设计为无填充正好 16 字节。注意若
 * 把 Oid、BlockNumber 或 TransactionId 加宽到超过 32 位，
 * 则须调整。
 *
 * 把 lockmethodid 放入 locktag，以便共享内存中单张
 * 散列表能存放不同锁方法的锁。
 */
typedef struct LOCKTAG
{
	uint32		locktag_field1; /* 32 位 ID 字段 */
	uint32		locktag_field2; /* 32 位 ID 字段 */
	uint32		locktag_field3; /* 32 位 ID 字段 */
	uint16		locktag_field4; /* 16 位 ID 字段 */
	uint8		locktag_type;	/* 见 LockTagType */
	uint8		locktag_lockmethodid;	/* 锁方法指示符 */
} LOCKTAG;

/*
 * 下列宏定义如何把可加锁对象的逻辑 ID 映射到
 * LOCKTAG 的物理字段。请用它们设置 LOCKTAG 值，
 * 勿直接访问字段。注意 target 可能被多次求值！
 */

/* 关系的 ID 信息为库 OID + 关系 OID；共享则库 OID=0 */
#define SET_LOCKTAG_RELATION(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 同 RELATION ID 信息 */
#define SET_LOCKTAG_RELATION_EXTEND(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION_EXTEND, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 冻结 ID 信息为库 OID */
#define SET_LOCKTAG_DATABASE_FROZEN_IDS(locktag,dboid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = 0, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_DATABASE_FROZEN_IDS, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 页的 ID 信息为 RELATION 信息 + BlockNumber */
#define SET_LOCKTAG_PAGE(locktag,dboid,reloid,blocknum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_PAGE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 元组 ID 为 PAGE 信息 + OffsetNumber */
#define SET_LOCKTAG_TUPLE(locktag,dboid,reloid,blocknum,offnum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = (offnum), \
	 (locktag).locktag_type = LOCKTAG_TUPLE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 事务的 ID 信息为其 TransactionId */
#define SET_LOCKTAG_TRANSACTION(locktag,xid) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = 0, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_TRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* 虚拟事务的 ID 信息为其 VirtualTransactionId */
#define SET_LOCKTAG_VIRTUALTRANSACTION(locktag,vxid) \
	((locktag).locktag_field1 = (vxid).backendId, \
	 (locktag).locktag_field2 = (vxid).localTransactionId, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_VIRTUALTRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * 推测插入的 ID 信息为 TRANSACTION 信息 +
 * 其推测插入计数器。
 */
#define SET_LOCKTAG_SPECULATIVE_INSERTION(locktag,xid,token) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = (token),		\
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_SPECULATIVE_TOKEN, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * 对象的 ID 信息为库 OID + CLASS OID + OBJECT OID + SUBID
 *
 * 注意：对象 ID 表示法与 pg_depend 及
 * pg_description 相同，但 SUBID 被限制为 16 位。
 * 另，共享对象（如表空间）使用库 OID = 0。
 */
#define SET_LOCKTAG_OBJECT(locktag,dboid,classoid,objoid,objsubid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (classoid), \
	 (locktag).locktag_field3 = (objoid), \
	 (locktag).locktag_field4 = (objsubid), \
	 (locktag).locktag_type = LOCKTAG_OBJECT, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_ADVISORY(locktag,id1,id2,id3,id4) \
	((locktag).locktag_field1 = (id1), \
	 (locktag).locktag_field2 = (id2), \
	 (locktag).locktag_field3 = (id3), \
	 (locktag).locktag_field4 = (id4), \
	 (locktag).locktag_type = LOCKTAG_ADVISORY, \
	 (locktag).locktag_lockmethodid = USER_LOCKMETHOD)

/*
 * 逻辑复制订阅端远程事务的 ID 信息为：库
 * OID + SUBSCRIPTION OID + TRANSACTION ID + OBJID
 */
#define SET_LOCKTAG_APPLY_TRANSACTION(locktag,dboid,suboid,xid,objid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (suboid), \
	 (locktag).locktag_field3 = (xid), \
	 (locktag).locktag_field4 = (objid), \
	 (locktag).locktag_type = LOCKTAG_APPLY_TRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * 每被锁对象的锁信息：
 *
 * tag -- 唯一标识被锁对象
 * grantMask -- 本对象上当前已授予的全部锁类型位掩码。
 * waitMask -- 本对象上当前等待的全部锁类型位掩码。
 * procLocks -- 本锁的 PROCLOCK 对象链表。
 * waitProcs -- 等待本锁的进程队列。
 * requested -- 本锁上当前请求的各锁类型计数
 *		（含已授予的请求！！）。
 * nRequested -- 全部类型请求锁的总数。
 * granted -- 本锁上当前已授予的各锁类型计数。
 * nGranted -- 全部类型已授锁总数。
 *
 * 注意：这些计数每个后端计 1。在后端内部，
 * 同一锁上可能有多次抓取，但这不会反映到
 * 共享内存中。
 */
typedef struct LOCK
{
	/* hash key */
	LOCKTAG		tag;			/* 可加锁对象的唯一标识 */

	/* data */
	LOCKMASK	grantMask;		/* 已授予锁类型的位掩码 */
	LOCKMASK	waitMask;		/* 等待中锁类型的位掩码 */
	dlist_head	procLocks;		/* 与本锁关联的 PROCLOCK 对象链表 */
	dclist_head waitProcs;		/* 等待本锁的 PGPROC 对象链表 */
	int			requested[MAX_LOCKMODES];	/* 各请求锁的计数 */
	int			nRequested;		/* requested[] 数组合计 */
	int			granted[MAX_LOCKMODES]; /* 已授锁计数 */
	int			nGranted;		/* total of granted[] array */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((LOCKMETHODID) (lock).tag.locktag_lockmethodid)
#define LOCK_LOCKTAG(lock) ((LockTagType) (lock).tag.locktag_type)


/*
 * We may have several different backends holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder/waiter
 * information for each such holder (or would-be holder).  This is kept in
 * a PROCLOCK struct.
 *
 * PROCLOCKTAG is the key information needed to look up a PROCLOCK item in the
 * proclock hashtable.  A PROCLOCKTAG value uniquely identifies the combination
 * of a lockable object and a holder/waiter for that object.  (We can use
 * pointers here because the PROCLOCKTAG need only be unique for the lifespan
 * of the PROCLOCK, and it will never outlive the lock or the proc.)
 *
 * Internally to a backend, it is possible for the same lock to be held
 * for different purposes: the backend tracks transaction locks separately
 * from session locks.  However, this is not reflected in the shared-memory
 * state: we only track which backend(s) hold the lock.  This is OK since a
 * backend can never block itself.
 *
 * The holdMask field shows the already-granted locks represented by this
 * proclock.  Note that there will be a proclock object, possibly with
 * zero holdMask, for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose holdMasks are zero are recycled
 * as soon as convenient.
 *
 * releaseMask is workspace for LockReleaseAll(): it shows the locks due
 * to be released during the current call.  This must only be examined or
 * set by the backend owning the PROCLOCK.
 *
 * Each PROCLOCK object is linked into lists for both the associated LOCK
 * object and the owning PGPROC object.  Note that the PROCLOCK is entered
 * into these lists as soon as it is created, even if no lock has yet been
 * granted.  A PGPROC that is waiting for a lock to be granted will also be
 * linked into the lock's waitProcs queue.
 */
typedef struct PROCLOCKTAG
{
	/* NB: we assume this struct contains no padding! */
	LOCK	   *myLock;			/* link to per-lockable-object information */
	PGPROC	   *myProc;			/* link to PGPROC of owning backend */
} PROCLOCKTAG;

typedef struct PROCLOCK
{
	/* tag */
	PROCLOCKTAG tag;			/* unique identifier of proclock object */

	/* data */
	PGPROC	   *groupLeader;	/* proc's lock group leader, or proc itself */
	LOCKMASK	holdMask;		/* bitmask for lock types currently held */
	LOCKMASK	releaseMask;	/* bitmask for lock types to be released */
	dlist_node	lockLink;		/* list link in LOCK's list of proclocks */
	dlist_node	procLink;		/* list link in PGPROC's list of proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
	LOCK_LOCKMETHOD(*((proclock).tag.myLock))

/*
 * Each backend also maintains a local hash table with information about each
 * lock it is currently interested in.  In particular the local table counts
 * the number of times that lock has been acquired.  This allows multiple
 * requests for the same lock to be executed without additional accesses to
 * shared memory.  We also track the number of lock acquisitions per
 * ResourceOwner, so that we can release just those locks belonging to a
 * particular ResourceOwner.
 *
 * When holding a lock taken "normally", the lock and proclock fields always
 * point to the associated objects in shared memory.  However, if we acquired
 * the lock via the fast-path mechanism, the lock and proclock fields are set
 * to NULL, since there probably aren't any such objects in shared memory.
 * (If the lock later gets promoted to normal representation, we may eventually
 * update our locallock's lock/proclock fields after finding the shared
 * objects.)
 *
 * Caution: a locallock object can be left over from a failed lock acquisition
 * attempt.  In this case its lock/proclock fields are untrustworthy, since
 * the shared lock object is neither held nor awaited, and hence is available
 * to be reclaimed.  If nLocks > 0 then these pointers must either be valid or
 * NULL, but when nLocks == 0 they should be considered garbage.
 */
typedef struct LOCALLOCKTAG
{
	LOCKTAG		lock;			/* identifies the lockable object */
	LOCKMODE	mode;			/* lock mode for this table entry */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER
{
	/*
	 * Note: if owner is NULL then the lock is held on behalf of the session;
	 * otherwise it is held on behalf of my current transaction.
	 *
	 * Must use a forward struct reference to avoid circularity.
	 */
	struct ResourceOwnerData *owner;
	int64		nLocks;			/* # of times held by this owner */
} LOCALLOCKOWNER;

typedef struct LOCALLOCK
{
	/* tag */
	LOCALLOCKTAG tag;			/* unique identifier of locallock entry */

	/* data */
	uint32		hashcode;		/* copy of LOCKTAG's hash value */
	LOCK	   *lock;			/* associated LOCK object, if any */
	PROCLOCK   *proclock;		/* associated PROCLOCK object, if any */
	int64		nLocks;			/* total number of times lock is held */
	int			numLockOwners;	/* # of relevant ResourceOwners */
	int			maxLockOwners;	/* allocated size of array */
	LOCALLOCKOWNER *lockOwners; /* dynamically resizable array */
	bool		holdsStrongLockCount;	/* bumped FastPathStrongRelationLocks */
	bool		lockCleared;	/* we read all sinval msgs for lock */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.locktag_lockmethodid)
#define LOCALLOCK_LOCKTAG(llock) ((LockTagType) (llock).tag.lock.locktag_type)


/*
 * These structures hold information passed from lmgr internals to the lock
 * listing user-level functions (in lockfuncs.c).
 */

typedef struct LockInstanceData
{
	LOCKTAG		locktag;		/* tag for locked object */
	LOCKMASK	holdMask;		/* locks held by this PGPROC */
	LOCKMODE	waitLockMode;	/* lock awaited by this PGPROC, if any */
	BackendId	backend;		/* backend ID of this PGPROC */
	LocalTransactionId lxid;	/* local transaction ID of this PGPROC */
	TimestampTz waitStart;		/* time at which this PGPROC started waiting
								 * for lock */
	int			pid;			/* pid of this PGPROC */
	int			leaderPid;		/* pid of group leader; = pid if no group */
	bool		fastpath;		/* taken via fastpath? */
} LockInstanceData;

typedef struct LockData
{
	int			nelements;		/* The length of the array */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
} LockData;

typedef struct BlockedProcData
{
	int			pid;			/* pid of a blocked PGPROC */
	/* Per-PROCLOCK information about PROCLOCKs of the lock the pid awaits */
	/* (these fields refer to indexes in BlockedProcsData.locks[]) */
	int			first_lock;		/* index of first relevant LockInstanceData */
	int			num_locks;		/* number of relevant LockInstanceDatas */
	/* PIDs of PGPROCs that are ahead of "pid" in the lock's wait queue */
	/* (these fields refer to indexes in BlockedProcsData.waiter_pids[]) */
	int			first_waiter;	/* index of first preceding waiter */
	int			num_waiters;	/* number of preceding waiters */
} BlockedProcData;

typedef struct BlockedProcsData
{
	BlockedProcData *procs;		/* Array of per-blocked-proc information */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
	int		   *waiter_pids;	/* Array of PIDs of other blocked PGPROCs */
	int			nprocs;			/* # of valid entries in procs[] array */
	int			maxprocs;		/* Allocated length of procs[] array */
	int			nlocks;			/* # of valid entries in locks[] array */
	int			maxlocks;		/* Allocated length of locks[] array */
	int			npids;			/* # of valid entries in waiter_pids[] array */
	int			maxpids;		/* Allocated length of waiter_pids[] array */
} BlockedProcsData;


/* Result codes for LockAcquire() */
typedef enum
{
	LOCKACQUIRE_NOT_AVAIL,		/* lock not available, and dontWait=true */
	LOCKACQUIRE_OK,				/* lock successfully acquired */
	LOCKACQUIRE_ALREADY_HELD,	/* incremented count for lock already held */
	LOCKACQUIRE_ALREADY_CLEAR	/* incremented count for lock already clear */
} LockAcquireResult;

/* Deadlock states identified by DeadLockCheck() */
typedef enum
{
	DS_NOT_YET_CHECKED,			/* no deadlock check has run yet */
	DS_NO_DEADLOCK,				/* no deadlock detected */
	DS_SOFT_DEADLOCK,			/* deadlock avoided by queue rearrangement */
	DS_HARD_DEADLOCK,			/* deadlock, no way out but ERROR */
	DS_BLOCKED_BY_AUTOVACUUM	/* no deadlock; queue blocked by autovacuum
								 * worker */
} DeadLockState;

/*
 * The lockmgr's shared hash tables are partitioned to reduce contention.
 * To determine which partition a given locktag belongs to, compute the tag's
 * hash code with LockTagHashCode(), then apply one of these macros.
 * NB: NUM_LOCK_PARTITIONS must be a power of 2!
 */
#define LockHashPartition(hashcode) \
	((hashcode) % NUM_LOCK_PARTITIONS)
#define LockHashPartitionLock(hashcode) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + \
		LockHashPartition(hashcode)].lock)
#define LockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

/*
 * The deadlock detector needs to be able to access lockGroupLeader and
 * related fields in the PGPROC, so we arrange for those fields to be protected
 * by one of the lock hash partition locks.  Since the deadlock detector
 * acquires all such locks anyway, this makes it safe for it to access these
 * fields without doing anything extra.  To avoid contention as much as
 * possible, we map different PGPROCs to different partition locks.  The lock
 * used for a given lock group is determined by the group leader's pgprocno.
 */
#define LockHashPartitionLockByProc(leader_pgproc) \
	LockHashPartitionLock((leader_pgproc)->pgprocno)

/*
 * function prototypes
 */
extern void InitLocks(void);
extern LockMethod GetLocksMethodTable(const LOCK *lock);
extern LockMethod GetLockTagsMethodTable(const LOCKTAG *locktag);
extern uint32 LockTagHashCode(const LOCKTAG *locktag);
extern bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2);
extern LockAcquireResult LockAcquire(const LOCKTAG *locktag,
									 LOCKMODE lockmode,
									 bool sessionLock,
									 bool dontWait);
extern LockAcquireResult LockAcquireExtended(const LOCKTAG *locktag,
											 LOCKMODE lockmode,
											 bool sessionLock,
											 bool dontWait,
											 bool reportMemoryError,
											 LOCALLOCK **locallockp);
extern void AbortStrongLockAcquire(void);
extern void MarkLockClear(LOCALLOCK *locallock);
extern bool LockRelease(const LOCKTAG *locktag,
						LOCKMODE lockmode, bool sessionLock);
extern void LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks);
extern void LockReleaseSession(LOCKMETHODID lockmethodid);
extern void LockReleaseCurrentOwner(LOCALLOCK **locallocks, int nlocks);
extern void LockReassignCurrentOwner(LOCALLOCK **locallocks, int nlocks);
extern bool LockHeldByMe(const LOCKTAG *locktag, LOCKMODE lockmode);
extern bool LockOrStrongerHeldByMe(const LOCKTAG *locktag, LOCKMODE lockmode);
#ifdef USE_ASSERT_CHECKING
extern HTAB *GetLockMethodLocalHash(void);
#endif
extern bool LockHasWaiters(const LOCKTAG *locktag,
						   LOCKMODE lockmode, bool sessionLock);
extern VirtualTransactionId *GetLockConflicts(const LOCKTAG *locktag,
											  LOCKMODE lockmode, int *countp);
extern void AtPrepare_Locks(void);
extern void PostPrepare_Locks(TransactionId xid);
extern bool LockCheckConflicts(LockMethod lockMethodTable,
							   LOCKMODE lockmode,
							   LOCK *lock, PROCLOCK *proclock);
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
extern void GrantAwaitedLock(void);
extern void RemoveFromWaitQueue(PGPROC *proc, uint32 hashcode);
extern Size LockShmemSize(void);
extern LockData *GetLockStatusData(void);
extern BlockedProcsData *GetBlockerStatusData(int blocked_pid);

extern xl_standby_lock *GetRunningTransactionLocks(int *nlocks);
extern const char *GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode);

extern void lock_twophase_recover(TransactionId xid, uint16 info,
								  void *recdata, uint32 len);
extern void lock_twophase_postcommit(TransactionId xid, uint16 info,
									 void *recdata, uint32 len);
extern void lock_twophase_postabort(TransactionId xid, uint16 info,
									void *recdata, uint32 len);
extern void lock_twophase_standby_recover(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);

extern DeadLockState DeadLockCheck(PGPROC *proc);
extern PGPROC *GetBlockingAutoVacuumPgproc(void);
extern void DeadLockReport(void) pg_attribute_noreturn();
extern void RememberSimpleDeadLock(PGPROC *proc1,
								   LOCKMODE lockmode,
								   LOCK *lock,
								   PGPROC *proc2);
extern void InitDeadLockChecking(void);

extern int	LockWaiterCount(const LOCKTAG *locktag);

#ifdef LOCK_DEBUG
extern void DumpLocks(PGPROC *proc);
extern void DumpAllLocks(void);
#endif

/* Lock a VXID (used to wait for a transaction to finish) */
extern void VirtualXactLockTableInsert(VirtualTransactionId vxid);
extern void VirtualXactLockTableCleanup(void);
extern bool VirtualXactLock(VirtualTransactionId vxid, bool wait);

#endif							/* LOCK_H_ */
