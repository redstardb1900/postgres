/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  轻量锁管理器
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lwlock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

#ifdef FRONTEND
#error "lwlock.h may not be included from frontend code"
#endif

#include "port/atomics.h"
#include "storage/proclist_types.h"

struct PGPROC;

/* 后端等待进程处于何种状态 */
typedef enum LWLockWaitState
{
	LW_WS_NOT_WAITING,			/* 当前未等待 / 已唤醒 */
	LW_WS_WAITING,				/* 当前在等待 */
	LW_WS_PENDING_WAKEUP,		/* 已移出等待链表，尚未发
								 * 信号 */
}			LWLockWaitState;

/*
 * lwlock.c 外的代码不应直接操纵本结构的
 * 内容，但我们必须在此声明，以便 LWLocks 能被
 * 嵌入其他数据结构。
 */
typedef struct LWLock
{
	uint16		tranche;		/* tranche ID */
	pg_atomic_uint32 state;		/* 排他/非排他加锁者的状态 */
	proclist_head waiters;		/* 等待 PGPROCs 链表 */
#ifdef LOCK_DEBUG
	pg_atomic_uint32 nwaiters;	/* 等待者数量 */
	struct PGPROC *owner;		/* 锁的上次排他持有者 */
#endif
} LWLock;

/*
 * 多数情况下，宜强制每组 LWLocks 的 tranche 对齐到
 * 缓存行边界，且数组步长为 2 的幂。这能节省
 * 索引的若干周期，更重要的是确保单个
 * LWLocks 不跨越缓存行边界。这会减少缓存争用
 * 问题，尤其在 AMD Opterons 上。有时宜再加
 * 更多填充，使每个 LWLock 占满整条缓存行；例如
 * 在主 LWLock 数组中有用，那里锁的总数
 * 较少但有些争用很重。
 */
#define LWLOCK_PADDED_SIZE	PG_CACHE_LINE_SIZE

StaticAssertDecl(sizeof(LWLock) <= LWLOCK_PADDED_SIZE,
				 "Miscalculated LWLock padding");

/* LWLock，填充到整条缓存行大小 */
typedef union LWLockPadded
{
	LWLock		lock;
	char		pad[LWLOCK_PADDED_SIZE];
} LWLockPadded;

extern PGDLLIMPORT LWLockPadded *MainLWLockArray;

/* 存放具名 tranche 信息的结构 */
typedef struct NamedLWLockTranche
{
	int			trancheId;
	char	   *trancheName;
} NamedLWLockTranche;

extern PGDLLIMPORT NamedLWLockTranche *NamedLWLockTrancheArray;
extern PGDLLIMPORT int NamedLWLockTrancheRequests;

/* 固定 lwlock 的名称 */
#include "storage/lwlocknames.h"

/*
 * 在此声明 NUM_BUFFER_PARTITIONS 与 NUM_LOCK_PARTITIONS
 * 有点怪，但我们需要它们来算 MainLWLockArray 内偏移，且
 * 本文件去包含 lock.h 或 bufmgr.h 会颠倒依赖。
 */

/* 共享缓冲区映射哈希表的分区数 */
#define NUM_BUFFER_PARTITIONS  128

/* 共享锁表划分成的分区数 */
#define LOG2_NUM_LOCK_PARTITIONS  4
#define NUM_LOCK_PARTITIONS  (1 << LOG2_NUM_LOCK_PARTITIONS)

/* 共享谓词锁表划分成的分区数 */
#define LOG2_NUM_PREDICATELOCK_PARTITIONS  4
#define NUM_PREDICATELOCK_PARTITIONS  (1 << LOG2_NUM_PREDICATELOCK_PARTITIONS)

/* 预分配 lwlock 各块的偏移。 */
#define BUFFER_MAPPING_LWLOCK_OFFSET	NUM_INDIVIDUAL_LWLOCKS
#define LOCK_MANAGER_LWLOCK_OFFSET		\
	(BUFFER_MAPPING_LWLOCK_OFFSET + NUM_BUFFER_PARTITIONS)
#define PREDICATELOCK_MANAGER_LWLOCK_OFFSET \
	(LOCK_MANAGER_LWLOCK_OFFSET + NUM_LOCK_PARTITIONS)
#define NUM_FIXED_LWLOCKS \
	(PREDICATELOCK_MANAGER_LWLOCK_OFFSET + NUM_PREDICATELOCK_PARTITIONS)

typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED,
	LW_WAIT_UNTIL_FREE			/* PGPROC->lwWaitMode 的特殊模式，
								 * 在等待锁变为空闲时。切勿
								 * 用作 LWLockAcquire 的参数 */
} LWLockMode;


#ifdef LOCK_DEBUG
extern PGDLLIMPORT bool Trace_lwlocks;
#endif

extern bool LWLockAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockAcquireOrWait(LWLock *lock, LWLockMode mode);
extern void LWLockRelease(LWLock *lock);
extern void LWLockReleaseClearVar(LWLock *lock, uint64 *valptr, uint64 val);
extern void LWLockReleaseAll(void);
extern bool LWLockHeldByMe(LWLock *lock);
extern bool LWLockAnyHeldByMe(LWLock *lock, int nlocks, size_t stride);
extern bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode);

extern bool LWLockWaitForVar(LWLock *lock, uint64 *valptr, uint64 oldval, uint64 *newval);
extern void LWLockUpdateVar(LWLock *lock, uint64 *valptr, uint64 val);

extern Size LWLockShmemSize(void);
extern void CreateLWLocks(void);
extern void InitLWLockAccess(void);

extern const char *GetLWLockIdentifier(uint32 classId, uint16 eventId);

/*
 * 扩展（或核心代码）可在 postmaster 启动期
 * 间调用 RequestNamedLWLockTranche() 取得 LWLocks。随后
 * 调用 GetNamedLWLockTranche() 取得指向数组的指针，其中含
 * 所请求的 LWLock 数量。
 */
extern void RequestNamedLWLockTranche(const char *tranche_name, int num_lwlocks);
extern LWLockPadded *GetNamedLWLockTranche(const char *tranche_name);

/*
 * 另有更灵活的取得 lwlock 办法。先调用
 * LWLockNewTrancheId 一次以取得 tranche ID；这从
 * 共享计数器分配。随后，使用该 tranche 的每个进程应
 * 调用 LWLockRegisterTranche() 把该 tranche ID 与名称关联。
 * 最后，每个 lwlock 应恰好调用一次 LWLockInitialize，传入
 * tranche ID 作为参数。
 *
 * 每个使用该 tranche 的进程必须分别注册，看似奇怪，
 * 但动态共享内存段不保证在所有协作后端中
 * 映射到同一地址，故把注册信息存进主共享
 * 内存段对该情形无效。
 */
extern int	LWLockNewTrancheId(void);
extern void LWLockRegisterTranche(int tranche_id, const char *tranche_name);
extern void LWLockInitialize(LWLock *lock, int tranche_id);

/*
 * 凡小于 NUM_INDIVIDUAL_LWLOCKS 的 tranche ID 均保留；另
 * 为未列入单独 LWLocks 集合的内建 tranche 再保留
 * tranche ID。调用 LWLockNewTrancheId 永远不会
 * 返回小于 LWTRANCHE_FIRST_USER_DEFINED 的值。
 */
typedef enum BuiltinTrancheIds
{
	LWTRANCHE_XACT_BUFFER = NUM_INDIVIDUAL_LWLOCKS,
	LWTRANCHE_COMMITTS_BUFFER,
	LWTRANCHE_SUBTRANS_BUFFER,
	LWTRANCHE_MULTIXACTOFFSET_BUFFER,
	LWTRANCHE_MULTIXACTMEMBER_BUFFER,
	LWTRANCHE_NOTIFY_BUFFER,
	LWTRANCHE_SERIAL_BUFFER,
	LWTRANCHE_WAL_INSERT,
	LWTRANCHE_BUFFER_CONTENT,
	LWTRANCHE_REPLICATION_ORIGIN_STATE,
	LWTRANCHE_REPLICATION_SLOT_IO,
	LWTRANCHE_LOCK_FASTPATH,
	LWTRANCHE_BUFFER_MAPPING,
	LWTRANCHE_LOCK_MANAGER,
	LWTRANCHE_PREDICATE_LOCK_MANAGER,
	LWTRANCHE_PARALLEL_HASH_JOIN,
	LWTRANCHE_PARALLEL_QUERY_DSA,
	LWTRANCHE_PER_SESSION_DSA,
	LWTRANCHE_PER_SESSION_RECORD_TYPE,
	LWTRANCHE_PER_SESSION_RECORD_TYPMOD,
	LWTRANCHE_SHARED_TUPLESTORE,
	LWTRANCHE_SHARED_TIDBITMAP,
	LWTRANCHE_PARALLEL_APPEND,
	LWTRANCHE_PER_XACT_PREDICATE_LIST,
	LWTRANCHE_PGSTATS_DSA,
	LWTRANCHE_PGSTATS_HASH,
	LWTRANCHE_PGSTATS_DATA,
	LWTRANCHE_LAUNCHER_DSA,
	LWTRANCHE_LAUNCHER_HASH,
	LWTRANCHE_FIRST_USER_DEFINED
}			BuiltinTrancheIds;

/*
 * PostgreSQL 9.4 之前，我们用名为 LWLockId 的枚举类型来指
 * LWLocks。新代码应改用 LWLock *。但为方便
 * 第三方代码，我们纳入以下 typedef。
 */
typedef LWLock *LWLockId;

#endif							/* LWLOCK_H */
