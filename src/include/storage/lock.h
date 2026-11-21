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
	int			nGranted;		/* granted[] 数组合计 */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((LOCKMETHODID) (lock).tag.locktag_lockmethodid)
#define LOCK_LOCKTAG(lock) ((LockTagType) (lock).tag.locktag_type)


/*
 * 多 backends 可持有或等待同一可加锁对象上的锁。
 * 我们需要为每个持有者（或潜在持有者）保存一些
 * 每持有者/等待者信息。这些保存在
 * PROCLOCK 中。
 *
 * PROCLOCKTAG 是在 proclock 散列表中查找 PROCLOCK
 * 项所需的键信息。一个 PROCLOCKTAG 值唯一标识
 * 可加锁对象与其持有者/等待者的组合。（此处可用
 * 指针，因 PROCLOCKTAG 只需在 PROCLOCK 存活期内唯一，
 * 且绝不会长于该锁或该进程。）
 *
 * 在 backend 内，同一锁可能因不同目的持有：后端把
 * 事务锁与会话锁分开跟踪。但这不反映到共享内存
 * 状态：我们只跟踪哪些后端持有该锁。这没问题，因
 * backend 永远无法
 * 阻塞自己。
 *
 * holdMask 字段显示本 proclock 所代表的已授予锁。
 * 注意：进程当前等待的任何锁都会有一个 proclock
 * 对象，其 holdMask 可能为零。否则 holdMask 为零的
 * proclock 对象会在方便时
 * 尽快回收。
 *
 * releaseMask 是 LockReleaseAll() 的工作区：显示本次
 * 调用中将释放的锁。仅拥有该 PROCLOCK 的后端可
 * 检查或设置它。
 *
 * 每个 PROCLOCK 对象都链入关联 LOCK 对象与拥有者
 * PGPROC 对象的链表。注意创建后即入链，即使尚未
 * 授予任何锁。正在等待授予锁的 PGPROC 还会
 * 链入该锁的
 * waitProcs 队列。
 */
typedef struct PROCLOCKTAG
{
	/* 注意：假定本结构无填充！ */
	LOCK	   *myLock;			/* 指向每可加锁对象信息的链接 */
	PGPROC	   *myProc;			/* 指向拥有后端 PGPROC 链接 */
} PROCLOCKTAG;

typedef struct PROCLOCK
{
	/* tag */
	PROCLOCKTAG tag;			/* proclock 对象的唯一标识 */

	/* data */
	PGPROC	   *groupLeader;	/* 进程的锁组领导，或进程自身 */
	LOCKMASK	holdMask;		/* 当前所持锁类型的位掩码 */
	LOCKMASK	releaseMask;	/* 将释放锁类型的位掩码 */
	dlist_node	lockLink;		/* LOCK 之 proclock 链表中的链节 */
	dlist_node	procLink;		/* PGPROC 之 proclock 链表中的链节 */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
	LOCK_LOCKMETHOD(*((proclock).tag.myLock))

/*
 * 每个后端还维护一张本地散列表，记录它当前关心的
 * 每把锁的信息。特别是本地表会统计该锁被取得的
 * 次数。这允许多次请求同一锁而无需额外访问共享
 * 内存。我们还按 ResourceOwner 跟踪加锁次数，
 * 以便只释放属于某一特定
 * ResourceOwner
 * 的锁。
 *
 * 以"普通"方式持有锁时，lock 与 proclock 字段总指向
 * 共享内存中的关联对象。但若经快速路径取得锁，则
 * lock 与 proclock 置为 NULL，因共享内存中多半没有
 * 此类对象。（若锁随后提升为普通表示，我们可能在
 * 找到共享对象后最终更新 locallock 的
 * lock/proclock 字段。）
 * objects.)
 *
 * 注意：失败的加锁尝试可能留下 locallock 对象。此时
 * 其 lock/proclock 字段不可信，因共享锁对象既未持有
 * 也未等待，故可被回收。若 nLocks > 0 则这些指针
 * 必须要么有效要么为 NULL；但 nLocks == 0 时应
 * 视为垃圾。
 */
typedef struct LOCALLOCKTAG
{
	LOCKTAG		lock;			/* 标识可加锁对象 */
	LOCKMODE	mode;			/* 本表项的锁模式 */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER
{
	/*
	 * 注意：若 owner 为 NULL 则锁为会话所持；
	 * 否则为我当前事务所持。
	 *
	 * 须用前向结构引用以免循环依赖。
	 */
	struct ResourceOwnerData *owner;
	int64		nLocks;			/* 本拥有者持有次数 */
} LOCALLOCKOWNER;

typedef struct LOCALLOCK
{
	/* tag */
	LOCALLOCKTAG tag;			/* locallock 条目的唯一标识 */

	/* data */
	uint32		hashcode;		/* LOCKTAG 散列值的副本 */
	LOCK	   *lock;			/* 关联 LOCK 对象（若有） */
	PROCLOCK   *proclock;		/* 关联 PROCLOCK 对象（若有） */
	int64		nLocks;			/* 持有该锁的总次数 */
	int			numLockOwners;	/* 相关 ResourceOwners 数 */
	int			maxLockOwners;	/* 数组已分配大小 */
	LOCALLOCKOWNER *lockOwners; /* 可动态调整大小数组 */
	bool		holdsStrongLockCount;	/* bumped FastPathStrongRelationLocks */
	bool		lockCleared;	/* 已读该锁全部 sinval 消息 */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.locktag_lockmethodid)
#define LOCALLOCK_LOCKTAG(llock) ((LockTagType) (llock).tag.lock.locktag_type)


/*
 * 这些结构保存从 lmgr 内部传给列锁用户级函数
 * （在 lockfuncs.c 中）的信息。
 */

typedef struct LockInstanceData
{
	LOCKTAG		locktag;		/* 被锁对象的标签 */
	LOCKMASK	holdMask;		/* 本 PGPROC 所持锁 */
	LOCKMODE	waitLockMode;	/* 本 PGPROC 等待的锁（若有） */
	BackendId	backend;		/* 本 PGPROC 的 backend ID */
	LocalTransactionId lxid;	/* 本 PGPROC 的本地事务 ID */
	TimestampTz waitStart;		/* 本 PGPROC 开始等待锁的
								 * 时刻 */
	int			pid;			/* 本 PGPROC 的 pid */
	int			leaderPid;		/* 组领导 pid；无组则 = pid */
	bool		fastpath;		/* taken via fastpath? */
} LockInstanceData;

typedef struct LockData
{
	int			nelements;		/* 数组长度 */
	LockInstanceData *locks;	/* 每 PROCLOCK 信息的数组 */
} LockData;

typedef struct BlockedProcData
{
	int			pid;			/* 被阻塞 PGPROC 的pid */
	/* 该 pid 所等锁 PROCLOCKs 的每 PROCLOCK 信息 */
	/* （这些字段引用 BlockedProcsData.locks[] 下标） */
	int			first_lock;		/* 首个相关 LockInstanceData 的下标 */
	int			num_locks;		/* 相关 LockInstanceDatas 个数 */
	/* 在该锁等待队列中排在 "pid" 前面的 PGPROCs 的 PIDs */
	/* （这些字段引用 BlockedProcsData.waiter_pids[] 下标） */
	int			first_waiter;	/* 首个前序等待者的下标 */
	int			num_waiters;	/* 前序等待者个数 */
} BlockedProcData;

typedef struct BlockedProcsData
{
	BlockedProcData *procs;		/* 每被阻塞进程信息的数组 */
	LockInstanceData *locks;	/* 每 PROCLOCK 信息的数组 */
	int		   *waiter_pids;	/* 其他被阻 PGPROCs 的 PIDs 数组 */
	int			nprocs;			/* procs[] 中有效项数 */
	int			maxprocs;		/* procs[] 已分配长度 */
	int			nlocks;			/* locks[] 中有效项数 */
	int			maxlocks;		/* locks[] 已分配长度 */
	int			npids;			/* waiter_pids[] 中有效项数 */
	int			maxpids;		/* waiter_pids[] 已分配长度 */
} BlockedProcsData;


/* LockAcquire() 的结果码 */
typedef enum
{
	LOCKACQUIRE_NOT_AVAIL,		/* 锁不可用，且 dontWait=true */
	LOCKACQUIRE_OK,				/* 成功取得锁 */
	LOCKACQUIRE_ALREADY_HELD,	/* 对已持有的锁递增计数 */
	LOCKACQUIRE_ALREADY_CLEAR	/* 对已清除的锁递增计数 */
} LockAcquireResult;

/* DeadLockCheck() 识别的死锁状态 */
typedef enum
{
	DS_NOT_YET_CHECKED,			/* 尚未运行死锁检查 */
	DS_NO_DEADLOCK,				/* 未检测到死锁 */
	DS_SOFT_DEADLOCK,			/* 通过重排队列避免了死锁 */
	DS_HARD_DEADLOCK,			/* 死锁，唯有 ERROR */
	DS_BLOCKED_BY_AUTOVACUUM	/* 无死锁；队列被自动清理
								 * worker */
} DeadLockState;

/*
 * lockmgr 的共享散列表分区以减轻争用。
 * 要确定某 locktag 属于哪一分区，用 LockTagHashCode()
 * 计算标签散列码，再套用下列宏之一。
 * NUM_LOCK_PARTITIONS 须为 2 的幂！
 */
#define LockHashPartition(hashcode) \
	((hashcode) % NUM_LOCK_PARTITIONS)
#define LockHashPartitionLock(hashcode) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + \
		LockHashPartition(hashcode)].lock)
#define LockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

/*
 * 死锁检测器需要能访问 PGPROC 中的 lockGroupLeader
 * 及相关字段，故安排由某把锁散列分区锁保护它们。
 * 因死锁检测器反正会取得全部此类锁，故可安全访问
 * 这些字段而无需额外动作。为尽量减少争用，我们把
 * 不同 PGPROCs 映射到不同分区锁。给定锁组
 * 所用的锁由组领导的
 * pgprocno 决定。
 */
#define LockHashPartitionLockByProc(leader_pgproc) \
	LockHashPartitionLock((leader_pgproc)->pgprocno)

/*
 * 函数原型
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

/* 锁住 VXID（用于等待事务结束） */
extern void VirtualXactLockTableInsert(VirtualTransactionId vxid);
extern void VirtualXactLockTableCleanup(void);
extern bool VirtualXactLock(VirtualTransactionId vxid, bool wait);

#endif							/* LOCK_H_ */
