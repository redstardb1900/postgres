/*-------------------------------------------------------------------------
 *
 * lwlock.c
 *	  轻量锁管理器
 *
 * 轻量锁主要用于为访问共享内存数据结构
 * 提供互斥。因此它同时提供排他与共享两种锁模式
 * （以支持对共享对象的读写与只读
 * 访问）。除此之外几乎没有别的花样。用户级
 * 加锁应当使用完整的锁管理器 --- 而它依赖
 * LWLocks 来保护自己的共享状态。
 *
 * 除排他与共享模式外，轻量锁还可以用来
 * 等待某个变量改变取值。用 LWLockAcquire 取得锁时，该变量
 * 起初并不被重新设置，也就是说它仍保持为
 * 上次释放锁时所设的值；不释放锁也可以
 * 调用 LWLockUpdateVar 来更新它。LWLockWaitForVar
 * 等待该变量被更新，或直到锁空闲。用
 * LWLockReleaseClearVar() 释放锁时，可以把该值设成适合
 * 空闲锁的取值。变量的含义由调用者决定，
 * 轻量锁代码只负责赋值和比较。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/lwlock.c
 *
 * 说明（NOTES）：
 *
 * 这里从前是一套相当直截了当的读写锁
 * 实现，其内部状态由一把
 * 自旋锁保护。不幸的是，对以共享模式
 * 取得非常频繁的工作负载/锁而言，获取自旋锁的开销
 * 太高了。我们常常在（显然是排他的）自旋锁里空转，
 * 而实际上想取得的那把共享锁是空闲的。
 *
 * 于是设计了新的实现，使并未被排他锁定的锁
 * 可以无等待地取得共享锁。
 *
 * 基本想法是用单个原子变量 'lockcount' 取代
 * 从前分开的共享计数与排他计数，并用原子
 * 操作来取得锁。对普通的
 * 读写自旋锁这相当容易，但对像 LWLocks 这种要在
 * 操作系统中等待的东西就难得多。
 *
 * 取得锁时，我们对 lockcount
 * 变量做一次原子比较并交换。排他锁换入一个哨兵值
 * (LW_VAL_EXCLUSIVE)，共享锁则统计持有者个数。
 *
 * 释放锁时，用原子递减来释放锁。若
 * 新值为零（这一点是原子得到的），我们就知道可以、也必须释放
 * 等待者。
 *
 * 显然，排他锁所用的哨兵值
 * 不得与可能的共享加锁者最大数目相冲突 ---
 * 好在 MAX_BACKENDS 使这一点很容易做到。
 *
 *
 * 细心的读者也许已经注意到，朴素地按上面做会有一个
 * 显眼的竞态：我们用原子操作尝试加锁，
 * 发现必须等待。不幸的是，等我们排完
 * 队时，先前的持锁者很可能已经做完了它的
 * 工作。这就麻烦了，因为我们现在卡在操作系统内部等待。

 * 为缓解这些竞态，我们分阶段尝试加锁：
 *	 阶段 1：尝试原子地完成，成功就好
 *	 阶段 2：把自己加入该锁的等待队列
 *	 阶段 3：再次尝试夺取锁，若成功，把自己从
 *			  队列中移除
 *	 阶段 4：睡眠直到被唤醒，转到阶段 1
 *
 * 这样就挡住了上面的问题，因为在我们入队之前，没人能释放得
 *	  太快：阶段 2 之后我们已经在队列中了。
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/proclist.h"
#include "storage/spin.h"
#include "utils/memutils.h"

#ifdef LWLOCK_STATS
#include "utils/hsearch.h"
#endif


/* 用 ShmemLock 自旋锁保护 LWLockCounter */
extern slock_t *ShmemLock;

#define LW_FLAG_HAS_WAITERS			((uint32) 1 << 30)
#define LW_FLAG_RELEASE_OK			((uint32) 1 << 29)
#define LW_FLAG_LOCKED				((uint32) 1 << 28)

#define LW_VAL_EXCLUSIVE			((uint32) 1 << 24)
#define LW_VAL_SHARED				1

#define LW_LOCK_MASK				((uint32) ((1 << 25)-1))
/* 必须大于 MAX_BACKENDS（即 2^23-1），因此没问题。 */
#define LW_SHARED_MASK				((uint32) ((1 << 24)-1))

StaticAssertDecl(LW_VAL_EXCLUSIVE > (uint32) MAX_BACKENDS,
				 "MAX_BACKENDS too big for lwlock.c");

/*
 * LWLock 的 "tranche"（分组）有三类：
 *
 * 1. lwlocknames.h 里逐个命名的锁各自拥有自己的
 * 分组。这些分组的名字出现在 IndividualLWLockNames[]
 * 中，该数组位于 lwlocknames.c。
 *
 * 2. 还有一些为内建锁组预定义的分组。
 * 它们列在 lwlock.h 的枚举 BuiltinTrancheIds 里，名字
 * 则出现在下面的 BuiltinTrancheNames[] 中。
 *
 * 3. 扩展可以通过 RequestNamedLWLockTranche
 * 或 LWLockRegisterTranche 创建新分组。当前
 * 进程已知的这些名字出现在 LWLockTrancheNames[] 中。
 *
 * 这些名字都会作为等待事件名对用户可见，选取时要谨慎
 * ... 并且别忘了更新文档里的等待事件列表。
 */
extern const char *const IndividualLWLockNames[];	/* 位于 lwlocknames.c */

static const char *const BuiltinTrancheNames[] = {
	/* LWTRANCHE_XACT_BUFFER（事务缓冲） */
	"XactBuffer",
	/* LWTRANCHE_COMMITTS_BUFFER（提交时间戳缓冲） */
	"CommitTsBuffer",
	/* LWTRANCHE_SUBTRANS_BUFFER（子事务缓冲） */
	"SubtransBuffer",
	/* LWTRANCHE_MULTIXACTOFFSET_BUFFER（多事务偏移缓冲） */
	"MultiXactOffsetBuffer",
	/* LWTRANCHE_MULTIXACTMEMBER_BUFFER（多事务成员缓冲） */
	"MultiXactMemberBuffer",
	/* LWTRANCHE_NOTIFY_BUFFER（通知缓冲） */
	"NotifyBuffer",
	/* LWTRANCHE_SERIAL_BUFFER（可串行化缓冲） */
	"SerialBuffer",
	/* LWTRANCHE_WAL_INSERT（WAL 插入） */
	"WALInsert",
	/* LWTRANCHE_BUFFER_CONTENT（缓冲内容） */
	"BufferContent",
	/* LWTRANCHE_REPLICATION_ORIGIN_STATE（复制源状态） */
	"ReplicationOriginState",
	/* LWTRANCHE_REPLICATION_SLOT_IO（复制槽 I/O） */
	"ReplicationSlotIO",
	/* LWTRANCHE_LOCK_FASTPATH（锁快速路径） */
	"LockFastPath",
	/* LWTRANCHE_BUFFER_MAPPING（缓冲映射） */
	"BufferMapping",
	/* LWTRANCHE_LOCK_MANAGER（锁管理器） */
	"LockManager",
	/* LWTRANCHE_PREDICATE_LOCK_MANAGER（谓词锁管理器） */
	"PredicateLockManager",
	/* LWTRANCHE_PARALLEL_HASH_JOIN（并行哈希连接） */
	"ParallelHashJoin",
	/* LWTRANCHE_PARALLEL_QUERY_DSA（并行查询 DSA） */
	"ParallelQueryDSA",
	/* LWTRANCHE_PER_SESSION_DSA（每会话 DSA） */
	"PerSessionDSA",
	/* LWTRANCHE_PER_SESSION_RECORD_TYPE（每会话记录类型） */
	"PerSessionRecordType",
	/* LWTRANCHE_PER_SESSION_RECORD_TYPMOD（每会话记录类型修饰符） */
	"PerSessionRecordTypmod",
	/* LWTRANCHE_SHARED_TUPLESTORE（共享元组存储） */
	"SharedTupleStore",
	/* LWTRANCHE_SHARED_TIDBITMAP（共享 TID 位图） */
	"SharedTidBitmap",
	/* LWTRANCHE_PARALLEL_APPEND（并行追加） */
	"ParallelAppend",
	/* LWTRANCHE_PER_XACT_PREDICATE_LIST（每事务谓词列表） */
	"PerXactPredicateList",
	/* LWTRANCHE_PGSTATS_DSA（统计信息 DSA） */
	"PgStatsDSA",
	/* LWTRANCHE_PGSTATS_HASH（统计信息哈希） */
	"PgStatsHash",
	/* LWTRANCHE_PGSTATS_DATA（统计信息数据） */
	"PgStatsData",
	/* LWTRANCHE_LAUNCHER_DSA（启动器 DSA） */
	"LogicalRepLauncherDSA",
	/* LWTRANCHE_LAUNCHER_HASH（启动器哈希） */
	"LogicalRepLauncherHash",
};

StaticAssertDecl(lengthof(BuiltinTrancheNames) ==
				 LWTRANCHE_FIRST_USER_DEFINED - NUM_INDIVIDUAL_LWLOCKS,
				 "missing entries in BuiltinTrancheNames[]");

/*
 * 下标等于分组 ID 减去 LWTRANCHE_FIRST_USER_DEFINED，用来
 * 存放当前进程已知的、全部动态创建的分组名。
 * 数组里未使用的项为 NULL。
 */
static const char **LWLockTrancheNames = NULL;
static int	LWLockTrancheNamesAllocated = 0;

/*
 * 指向共享内存中 LWLocks 的主数组。后端经由
 * fork 从 postmaster 继承该指针（EXEC_BACKEND 情形除外，
 * 那时要用专门的办法把它传下去）。
 */
LWLockPadded *MainLWLockArray = NULL;

/*
 * 用这个结构记录已锁定的 LWLocks，以便在
 * 错误恢复时释放。通常同时只持有少数几把，但
 * 偶尔数量会高得多；例如 pg_buffercache
 * 扩展会同时锁住全部缓冲分区。
 */
#define MAX_SIMUL_LWLOCKS	200

/* 表示我们正持有的那些 LWLocks 的结构 */
typedef struct LWLockHandle
{
	LWLock	   *lock;
	LWLockMode	mode;
} LWLockHandle;

static int	num_held_lwlocks = 0;
static LWLockHandle held_lwlocks[MAX_SIMUL_LWLOCKS];

/* 表示具名分组的 LWLock 分组请求的结构 */
typedef struct NamedLWLockTrancheRequest
{
	char		tranche_name[NAMEDATALEN];
	int			num_lwlocks;
} NamedLWLockTrancheRequest;

static NamedLWLockTrancheRequest *NamedLWLockTrancheRequestArray = NULL;
static int	NamedLWLockTrancheRequestsAllocated = 0;

/*
 * NamedLWLockTrancheRequests 既是请求数组的有效长度，
 * 也是稍后共享内存里 NamedLWLockTrancheArray 的长度。
 * 该变量与 NamedLWLockTrancheArray 不是 static，以便
 * postmaster.c 能在 EXEC_BACKEND 构建里把它们复制给子进程。
 */
int			NamedLWLockTrancheRequests = 0;

/* 指向共享内存中的数据： */
NamedLWLockTranche *NamedLWLockTrancheArray = NULL;

static void InitializeLWLocks(void);
static inline void LWLockReportWaitStart(LWLock *lock);
static inline void LWLockReportWaitEnd(void);
static const char *GetLWTrancheName(uint16 trancheId);

#define T_NAME(lock) \
	GetLWTrancheName((lock)->tranche)

#ifdef LWLOCK_STATS
typedef struct lwlock_stats_key
{
	int			tranche;
	void	   *instance;
}			lwlock_stats_key;

typedef struct lwlock_stats
{
	lwlock_stats_key key;
	int			sh_acquire_count;
	int			ex_acquire_count;
	int			block_count;
	int			dequeue_self_count;
	int			spin_delay_count;
}			lwlock_stats;

static HTAB *lwlock_stats_htab;
static lwlock_stats lwlock_stats_dummy;
#endif

#ifdef LOCK_DEBUG
bool		Trace_lwlocks = false;

inline static void
PRINT_LWDEBUG(const char *where, LWLock *lock, LWLockMode mode)
{
	/* 在此隐藏语句与上下文，否则日志过于冗长 */
	if (Trace_lwlocks)
	{
		uint32		state = pg_atomic_read_u32(&lock->state);

		ereport(LOG,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("%d: %s(%s %p): excl %u shared %u haswaiters %u waiters %u rOK %d",
								 MyProcPid,
								 where, T_NAME(lock), lock,
								 (state & LW_VAL_EXCLUSIVE) != 0,
								 state & LW_SHARED_MASK,
								 (state & LW_FLAG_HAS_WAITERS) != 0,
								 pg_atomic_read_u32(&lock->nwaiters),
								 (state & LW_FLAG_RELEASE_OK) != 0)));
	}
}

inline static void
LOG_LWDEBUG(const char *where, LWLock *lock, const char *msg)
{
	/* 在此隐藏语句与上下文，否则日志过于冗长 */
	if (Trace_lwlocks)
	{
		ereport(LOG,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("%s(%s %p): %s", where,
								 T_NAME(lock), lock, msg)));
	}
}

#else							/* 非 LOCK_DEBUG */
#define PRINT_LWDEBUG(a,b,c) ((void)0)
#define LOG_LWDEBUG(a,b,c) ((void)0)
#endif							/* LOCK_DEBUG（锁调试） */

#ifdef LWLOCK_STATS

static void init_lwlock_stats(void);
static void print_lwlock_stats(int code, Datum arg);
static lwlock_stats * get_lwlock_stats_entry(LWLock *lock);

static void
init_lwlock_stats(void)
{
	HASHCTL		ctl;
	static MemoryContext lwlock_stats_cxt = NULL;
	static bool exit_registered = false;

	if (lwlock_stats_cxt != NULL)
		MemoryContextDelete(lwlock_stats_cxt);

	/*
	 * LWLock 统计会在临界区内更新，而这
	 * 需要分配新的哈希项。临界区之内
	 * 通常不允许分配，因为内存不足会导致
	 * PANIC，但 LWLOCK_STATS 是调试代码，正常情况下
	 * 不会在生产环境中打开，所以这个风险可以接受。这些哈希项
	 * 很小，因此实践中内存不足的风险极低。
	 */
	lwlock_stats_cxt = AllocSetContextCreate(TopMemoryContext,
											 "LWLock stats",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(lwlock_stats_cxt, true);

	ctl.keysize = sizeof(lwlock_stats_key);
	ctl.entrysize = sizeof(lwlock_stats);
	ctl.hcxt = lwlock_stats_cxt;
	lwlock_stats_htab = hash_create("lwlock stats", 16384, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	if (!exit_registered)
	{
		on_shmem_exit(print_lwlock_stats, 0);
		exit_registered = true;
	}
}

static void
print_lwlock_stats(int code, Datum arg)
{
	HASH_SEQ_STATUS scan;
	lwlock_stats *lwstats;

	hash_seq_init(&scan, lwlock_stats_htab);

	/* 取得一把 LWLock，免得不同后端的报告混在一起 */
	LWLockAcquire(&MainLWLockArray[0].lock, LW_EXCLUSIVE);

	while ((lwstats = (lwlock_stats *) hash_seq_search(&scan)) != NULL)
	{
		fprintf(stderr,
				"PID %d lwlock %s %p: shacq %u exacq %u blk %u spindelay %u dequeue self %u\n",
				MyProcPid, GetLWTrancheName(lwstats->key.tranche),
				lwstats->key.instance, lwstats->sh_acquire_count,
				lwstats->ex_acquire_count, lwstats->block_count,
				lwstats->spin_delay_count, lwstats->dequeue_self_count);
	}

	LWLockRelease(&MainLWLockArray[0].lock);
}

static lwlock_stats *
get_lwlock_stats_entry(LWLock *lock)
{
	lwlock_stats_key key;
	lwlock_stats *lwstats;
	bool		found;

	/*
	 * 共享内存初始化期间，哈希表还不存在。
	 * 这一阶段的统计没什么意思，因此把所有锁上的操作
	 * 都记到同一个虚设项里。
	 */
	if (lwlock_stats_htab == NULL)
		return &lwlock_stats_dummy;

	/* 取出或创建该项。 */
	MemSet(&key, 0, sizeof(key));
	key.tranche = lock->tranche;
	key.instance = lock;
	lwstats = hash_search(lwlock_stats_htab, &key, HASH_ENTER, &found);
	if (!found)
	{
		lwstats->sh_acquire_count = 0;
		lwstats->ex_acquire_count = 0;
		lwstats->block_count = 0;
		lwstats->dequeue_self_count = 0;
		lwstats->spin_delay_count = 0;
	}
	return lwstats;
}
#endif							/* LWLOCK_STATS（轻量锁统计） */


/*
 * 计算具名分组所需的 LWLocks 数量。它们将
 * 分配在主数组中。
 */
static int
NumLWLocksForNamedTranches(void)
{
	int			numLocks = 0;
	int			i;

	for (i = 0; i < NamedLWLockTrancheRequests; i++)
		numLocks += NamedLWLockTrancheRequestArray[i].num_lwlocks;

	return numLocks;
}

/*
 * 计算 LWLocks 与具名分组所需的共享内存空间。
 */
Size
LWLockShmemSize(void)
{
	Size		size;
	int			i;
	int			numLocks = NUM_FIXED_LWLOCKS;

	/* 计算主数组中所需锁的总数。 */
	numLocks += NumLWLocksForNamedTranches();

	/* LWLock 数组所需的空间。 */
	size = mul_size(numLocks, sizeof(LWLockPadded));

	/* 动态分配计数器所需空间，外加对齐余量。 */
	size = add_size(size, sizeof(int) + LWLOCK_PADDED_SIZE);

	/* 具名分组所需空间。 */
	size = add_size(size, mul_size(NamedLWLockTrancheRequests, sizeof(NamedLWLockTranche)));

	/* 每个分组的名字所需空间。 */
	for (i = 0; i < NamedLWLockTrancheRequests; i++)
		size = add_size(size, strlen(NamedLWLockTrancheRequestArray[i].tranche_name) + 1);

	return size;
}

/*
 * 为主 LWLock 数组及全部分组分配共享内存并
 * 加以初始化。此处也注册扩展的 LWLock 分组。
 */
void
CreateLWLocks(void)
{
	if (!IsUnderPostmaster)
	{
		Size		spaceLocks = LWLockShmemSize();
		int		   *LWLockCounter;
		char	   *ptr;

		/* 分配空间 */
		ptr = (char *) ShmemAlloc(spaceLocks);

		/* 为分组的动态分配留出余地 */
		ptr += sizeof(int);

		/* 保证 LWLock 数组达到所需对齐 */
		ptr += LWLOCK_PADDED_SIZE - ((uintptr_t) ptr) % LWLOCK_PADDED_SIZE;

		MainLWLockArray = (LWLockPadded *) ptr;

		/*
		 * 初始化分组的动态分配计数器，它存放在
		 * 第一把 LWLock 之前。
		 */
		LWLockCounter = (int *) ((char *) MainLWLockArray - sizeof(int));
		*LWLockCounter = LWTRANCHE_FIRST_USER_DEFINED;

		/* 初始化全部 LWLocks */
		InitializeLWLocks();
	}

	/* 在当前进程中注册具名扩展 LWLock 分组。 */
	for (int i = 0; i < NamedLWLockTrancheRequests; i++)
		LWLockRegisterTranche(NamedLWLockTrancheArray[i].trancheId,
							  NamedLWLockTrancheArray[i].trancheName);
}

/*
 * 初始化固定的 LWLocks，以及属于具名分组的那些。
 */
static void
InitializeLWLocks(void)
{
	int			numNamedLocks = NumLWLocksForNamedTranches();
	int			id;
	int			i;
	int			j;
	LWLockPadded *lock;

	/* 初始化主数组中全部单独的 LWLocks */
	for (id = 0, lock = MainLWLockArray; id < NUM_INDIVIDUAL_LWLOCKS; id++, lock++)
		LWLockInitialize(&lock->lock, id);

	/* 初始化主数组中的缓冲映射 LWLocks */
	lock = MainLWLockArray + BUFFER_MAPPING_LWLOCK_OFFSET;
	for (id = 0; id < NUM_BUFFER_PARTITIONS; id++, lock++)
		LWLockInitialize(&lock->lock, LWTRANCHE_BUFFER_MAPPING);

	/* 初始化主数组中锁管理器的 LWLocks */
	lock = MainLWLockArray + LOCK_MANAGER_LWLOCK_OFFSET;
	for (id = 0; id < NUM_LOCK_PARTITIONS; id++, lock++)
		LWLockInitialize(&lock->lock, LWTRANCHE_LOCK_MANAGER);

	/* 初始化主数组中谓词锁管理器的 LWLocks */
	lock = MainLWLockArray + PREDICATELOCK_MANAGER_LWLOCK_OFFSET;
	for (id = 0; id < NUM_PREDICATELOCK_PARTITIONS; id++, lock++)
		LWLockInitialize(&lock->lock, LWTRANCHE_PREDICATE_LOCK_MANAGER);

	/*
	 * 把任何具名分组的信息复制到共享内存（以便
	 * 其他进程能够看见），并初始化所请求的 LWLocks。
	 */
	if (NamedLWLockTrancheRequests > 0)
	{
		char	   *trancheNames;

		NamedLWLockTrancheArray = (NamedLWLockTranche *)
			&MainLWLockArray[NUM_FIXED_LWLOCKS + numNamedLocks];

		trancheNames = (char *) NamedLWLockTrancheArray +
			(NamedLWLockTrancheRequests * sizeof(NamedLWLockTranche));
		lock = &MainLWLockArray[NUM_FIXED_LWLOCKS];

		for (i = 0; i < NamedLWLockTrancheRequests; i++)
		{
			NamedLWLockTrancheRequest *request;
			NamedLWLockTranche *tranche;
			char	   *name;

			request = &NamedLWLockTrancheRequestArray[i];
			tranche = &NamedLWLockTrancheArray[i];

			name = trancheNames;
			trancheNames += strlen(request->tranche_name) + 1;
			strcpy(name, request->tranche_name);
			tranche->trancheId = LWLockNewTrancheId();
			tranche->trancheName = name;

			for (j = 0; j < request->num_lwlocks; j++, lock++)
				LWLockInitialize(&lock->lock, tranche->trancheId);
		}
	}
}

/*
 * InitLWLockAccess - 初始化持有 LWLocks 所需的后端本地状态
 */
void
InitLWLockAccess(void)
{
#ifdef LWLOCK_STATS
	init_lwlock_stats();
#endif
}

/*
 * GetNamedLWLockTranche - 返回指定分组中 LWLock 的
 *		基地址。
 *
 * 调用者需从本 API 返回的基锁地址起，取出所请求数量的 LWLocks。
 * 可用于经由 RequestNamedLWLockTranche() 这个 API
 * 请求到的分组。
 */
LWLockPadded *
GetNamedLWLockTranche(const char *tranche_name)
{
	int			lock_pos;
	int			i;

	/*
	 * 在 MainLWLockArray 中，取得属于所请求
	 * tranche_name 的 LWLock 基地址位置。具名分组的 LWLocks 放在
	 * 固定锁之后的 MainLWLockArray 里。
	 */
	lock_pos = NUM_FIXED_LWLOCKS;
	for (i = 0; i < NamedLWLockTrancheRequests; i++)
	{
		if (strcmp(NamedLWLockTrancheRequestArray[i].tranche_name,
				   tranche_name) == 0)
			return &MainLWLockArray[lock_pos];

		lock_pos += NamedLWLockTrancheRequestArray[i].num_lwlocks;
	}

	elog(ERROR, "requested tranche is not registered");

	/* 只为让编译器不再报警 */
	return NULL;
}

/*
 * 分配一个新的分组 ID。
 */
int
LWLockNewTrancheId(void)
{
	int			result;
	int		   *LWLockCounter;

	LWLockCounter = (int *) ((char *) MainLWLockArray - sizeof(int));
	SpinLockAcquire(ShmemLock);
	result = (*LWLockCounter)++;
	SpinLockRelease(ShmemLock);

	return result;
}

/*
 * 在当前进程的查找表中注册一个动态分组名。
 *
 * 本例程会保存传入的分组名指针，
 * 因此该名字应当分配在与后端同寿命的上下文中
 * （共享内存、TopMemoryContext、静态常量或类似之处）。
 *
 * 分组名会作为等待事件名对用户可见，因此尽量
 * 选用符合那些名字风格的名称。
 */
void
LWLockRegisterTranche(int tranche_id, const char *tranche_name)
{
	/* 只应用于用户定义的分组。 */
	if (tranche_id < LWTRANCHE_FIRST_USER_DEFINED)
		return;

	/* 换算成数组下标。 */
	tranche_id -= LWTRANCHE_FIRST_USER_DEFINED;

	/* 必要时创建或扩大数组。 */
	if (tranche_id >= LWLockTrancheNamesAllocated)
	{
		int			newalloc;

		newalloc = pg_nextpower2_32(Max(8, tranche_id + 1));

		if (LWLockTrancheNames == NULL)
			LWLockTrancheNames = (const char **)
				MemoryContextAllocZero(TopMemoryContext,
									   newalloc * sizeof(char *));
		else
			LWLockTrancheNames =
				repalloc0_array(LWLockTrancheNames, const char *, LWLockTrancheNamesAllocated, newalloc);
		LWLockTrancheNamesAllocated = newalloc;
	}

	LWLockTrancheNames[tranche_id] = tranche_name;
}

/*
 * RequestNamedLWLockTranche
 *		请求在 postmaster
 *		启动期间额外分配 LWLocks。
 *
 * 只能通过某个库的 shmem_request_hook 来调用，该库须经
 * shared_preload_libraries 载入 postmaster。从
 * 别处调用则会失败。
 *
 * 分组名会作为等待事件名对用户可见，因此尽量
 * 选用符合那些名字风格的名称。
 */
void
RequestNamedLWLockTranche(const char *tranche_name, int num_lwlocks)
{
	NamedLWLockTrancheRequest *request;

	if (!process_shmem_requests_in_progress)
		elog(FATAL, "cannot request additional LWLocks outside shmem_request_hook");

	if (NamedLWLockTrancheRequestArray == NULL)
	{
		NamedLWLockTrancheRequestsAllocated = 16;
		NamedLWLockTrancheRequestArray = (NamedLWLockTrancheRequest *)
			MemoryContextAlloc(TopMemoryContext,
							   NamedLWLockTrancheRequestsAllocated
							   * sizeof(NamedLWLockTrancheRequest));
	}

	if (NamedLWLockTrancheRequests >= NamedLWLockTrancheRequestsAllocated)
	{
		int			i = pg_nextpower2_32(NamedLWLockTrancheRequests + 1);

		NamedLWLockTrancheRequestArray = (NamedLWLockTrancheRequest *)
			repalloc(NamedLWLockTrancheRequestArray,
					 i * sizeof(NamedLWLockTrancheRequest));
		NamedLWLockTrancheRequestsAllocated = i;
	}

	request = &NamedLWLockTrancheRequestArray[NamedLWLockTrancheRequests];
	Assert(strlen(tranche_name) + 1 <= NAMEDATALEN);
	strlcpy(request->tranche_name, tranche_name, NAMEDATALEN);
	request->num_lwlocks = num_lwlocks;
	NamedLWLockTrancheRequests++;
}

/*
 * LWLockInitialize - 初始化一把新的轻量锁；起初是未锁定的
 */
void
LWLockInitialize(LWLock *lock, int tranche_id)
{
	pg_atomic_init_u32(&lock->state, LW_FLAG_RELEASE_OK);
#ifdef LOCK_DEBUG
	pg_atomic_init_u32(&lock->nwaiters, 0);
#endif
	lock->tranche = tranche_id;
	proclist_init(&lock->waiters);
}

/*
 * 报告轻量锁等待事件的开始。
 *
 * 所有需要等待才能取得锁的轻量锁调用都会用到本函数。
 * 本函数按分组和锁 id 区分等待
 * 事件。
 */
static inline void
LWLockReportWaitStart(LWLock *lock)
{
	pgstat_report_wait_start(PG_WAIT_LWLOCK | lock->tranche);
}

/*
 * 报告轻量锁等待事件的结束。
 */
static inline void
LWLockReportWaitEnd(void)
{
	pgstat_report_wait_end();
}

/*
 * 返回某个 LWLock 分组的名字。
 */
static const char *
GetLWTrancheName(uint16 trancheId)
{
	/* 单独的 LWLock？ */
	if (trancheId < NUM_INDIVIDUAL_LWLOCKS)
		return IndividualLWLockNames[trancheId];

	/* 内建分组？ */
	if (trancheId < LWTRANCHE_FIRST_USER_DEFINED)
		return BuiltinTrancheNames[trancheId - NUM_INDIVIDUAL_LWLOCKS];

	/*
	 * 这是扩展分组，因此到 LWLockTrancheNames[] 里查找。不过，
	 * 该分组可能从未在当前
	 * 进程中注册过，那时就放弃并返回 "extension"。
	 */
	trancheId -= LWTRANCHE_FIRST_USER_DEFINED;

	if (trancheId >= LWLockTrancheNamesAllocated ||
		LWLockTrancheNames[trancheId] == NULL)
		return "extension";

	return LWLockTrancheNames[trancheId];
}

/*
 * 根据等待类别与事件，返回某把 LWLock 的标识。
 */
const char *
GetLWLockIdentifier(uint32 classId, uint16 eventId)
{
	Assert(classId == PG_WAIT_LWLOCK);
	/* 事件 ID 就是分组编号。 */
	return GetLWTrancheName(eventId);
}

/*
 * 内部函数：尝试以传入的模式
 * 原子地取得这把轻量锁。
 *
 * 本函数不会阻塞着等待锁变空闲 --- 那是
 * 调用者的事。
 *
 * 若锁不空闲、因而需要等待，则返回 true。
 */
static bool
LWLockAttemptLock(LWLock *lock, LWLockMode mode)
{
	uint32		old_state;

	Assert(mode == LW_EXCLUSIVE || mode == LW_SHARED);

	/*
	 * 在循环外先读一次，后续迭代经由比较并交换
	 * 取得更新后的值。
	 */
	old_state = pg_atomic_read_u32(&lock->state);

	/* 循环，直到确定能否取得这把锁 */
	while (true)
	{
		uint32		desired_state;
		bool		lock_free;

		desired_state = old_state;

		if (mode == LW_EXCLUSIVE)
		{
			lock_free = (old_state & LW_LOCK_MASK) == 0;
			if (lock_free)
				desired_state += LW_VAL_EXCLUSIVE;
		}
		else
		{
			lock_free = (old_state & LW_VAL_EXCLUSIVE) == 0;
			if (lock_free)
				desired_state += LW_VAL_SHARED;
		}

		/*
		 * 尝试换入我们所期望的状态。若没有看到
		 * 锁是空闲的，那就是旧值。若看到它空闲，
		 * 就尝试把它标记为已取得。我们总是把值
		 * 换进去，是因为这同时充当内存屏障。也可以试着
		 * 更取巧，只在看到锁空闲时才换入新值，
		 * 但迄今的基准测试还没表明那样有好处。
		 *
		 * 若自上次查看以来值已经变了，就重试。
		 */
		if (pg_atomic_compare_exchange_u32(&lock->state,
										   &old_state, desired_state))
		{
			if (lock_free)
			{
				/* 太好了！拿到锁了。 */
#ifdef LOCK_DEBUG
				if (mode == LW_EXCLUSIVE)
					lock->owner = MyProc;
#endif
				return false;
			}
			else
				return true;	/* 锁在别人手里 */
		}
	}
	pg_unreachable();
}

/*
 * 锁住这把 LWLock 的等待队列，免得并发地改动它。
 *
 * 注意：即便等待队列已经上锁，不冲突的锁操作
 * 仍然可能并发进行。
 *
 * 持有互斥锁的时间应当很短！
 */
static void
LWLockWaitListLock(LWLock *lock)
{
	uint32		old_state;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;
	uint32		delays = 0;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	while (true)
	{
		/* 总是先直接尝试取得锁一次 */
		old_state = pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_LOCKED);
		if (!(old_state & LW_FLAG_LOCKED))
			break;				/* 已取得锁 */

		/* 然后在不做原子操作的情况下自旋，直到锁被释放 */
		{
			SpinDelayStatus delayStatus;

			init_local_spin_delay(&delayStatus);

			while (old_state & LW_FLAG_LOCKED)
			{
				perform_spin_delay(&delayStatus);
				old_state = pg_atomic_read_u32(&lock->state);
			}
#ifdef LWLOCK_STATS
			delays += delayStatus.delays;
#endif
			finish_spin_delay(&delayStatus);
		}

		/*
		 * 重试。等我们着手再取它时，锁显然
		 * 可能已经被重新取得。
		 */
	}

#ifdef LWLOCK_STATS
	lwstats->spin_delay_count += delays;
#endif
}

/*
 * 解开这把 LWLock 的等待队列锁。
 *
 * 注意：在一次原子操作里改标志并释放
 * 这些锁，有时会更高效。
 */
static void
LWLockWaitListUnlock(LWLock *lock)
{
	uint32		old_state PG_USED_FOR_ASSERTS_ONLY;

	old_state = pg_atomic_fetch_and_u32(&lock->state, ~LW_FLAG_LOCKED);

	Assert(old_state & LW_FLAG_LOCKED);
}

/*
 * 唤醒当前有机会取得这把锁的全部加锁者。
 */
static void
LWLockWakeup(LWLock *lock)
{
	bool		new_release_ok;
	bool		wokeup_somebody = false;
	proclist_head wakeup;
	proclist_mutable_iter iter;

	proclist_init(&wakeup);

	new_release_ok = true;

	/* 收集要唤醒的后端时，锁住等待队列 */
	LWLockWaitListLock(lock);

	proclist_foreach_modify(iter, &lock->waiters, lwWaitLink)
	{
		PGPROC	   *waiter = GetPGProcByNumber(iter.cur);

		if (wokeup_somebody && waiter->lwWaitMode == LW_EXCLUSIVE)
			continue;

		proclist_delete(&lock->waiters, iter.cur, lwWaitLink);
		proclist_push_tail(&wakeup, iter.cur, lwWaitLink);

		if (waiter->lwWaitMode != LW_WAIT_UNTIL_FREE)
		{
			/*
			 * 在重试者得以运行之前，阻止额外的唤醒。那些
			 * 只是在等锁变空闲的后端不会自动
			 * 重试。
			 */
			new_release_ok = false;

			/*
			 * 不要（再）唤醒排他锁。
			 */
			wokeup_somebody = true;
		}

		/*
		 * 表明该进程已经不在等待队列上。这样
		 * LWLockDequeueSelf() 就可以用一次
		 * proclist_delete() 把自己从等待队列移除，而不必再检查自己是否已经
		 * 被从链表里删掉。
		 */
		Assert(waiter->lwWaiting == LW_WS_WAITING);
		waiter->lwWaiting = LW_WS_PENDING_WAKEUP;

		/*
		 * 一旦已经唤醒了一把排他锁，再去唤醒
		 * 其他等待者就没有意义了。
		 */
		if (waiter->lwWaitMode == LW_EXCLUSIVE)
			break;
	}

	Assert(proclist_is_empty(&wakeup) || pg_atomic_read_u32(&lock->state) & LW_FLAG_HAS_WAITERS);

	/* 一次做完：清除所需标志，并释放锁 */
	{
		uint32		old_state;
		uint32		desired_state;

		old_state = pg_atomic_read_u32(&lock->state);
		while (true)
		{
			desired_state = old_state;

			/* 计算所期望的标志 */

			if (new_release_ok)
				desired_state |= LW_FLAG_RELEASE_OK;
			else
				desired_state &= ~LW_FLAG_RELEASE_OK;

			if (proclist_is_empty(&wakeup))
				desired_state &= ~LW_FLAG_HAS_WAITERS;

			desired_state &= ~LW_FLAG_LOCKED;	/* 释放锁 */

			if (pg_atomic_compare_exchange_u32(&lock->state, &old_state,
											   desired_state))
				break;
		}
	}

	/* 唤醒我从队列里移除的那些等待者。 */
	proclist_foreach_modify(iter, &wakeup, lwWaitLink)
	{
		PGPROC	   *waiter = GetPGProcByNumber(iter.cur);

		LOG_LWDEBUG("LWLockRelease", lock, "release waiter");
		proclist_delete(&wakeup, iter.cur, lwWaitLink);

		/*
		 * 保证 lwWaiting 被清除这件事，只有在从链表
		 * 摘除完成之后才变得可见。否则目标后端
		 * 可能因别的原因被唤醒，并为一把新锁入队 --- 若
		 * 这发生在链表摘除之前，链表最终会被
		 * 破坏。
		 *
		 * 该屏障与为另一把锁入队时的 LWLockWaitListLock()
		 * 配对。
		 */
		pg_write_barrier();
		waiter->lwWaiting = LW_WS_NOT_WAITING;
		PGSemaphoreUnlock(waiter->sem);
	}
}

/*
 * 把自己加到队列末尾。
 *
 * 注意：此处的模式可以是 LW_WAIT_UNTIL_FREE！
 */
static void
LWLockQueueSelf(LWLock *lock, LWLockMode mode)
{
	/*
	 * 如果没有 PGPROC 结构，就没法等待。这
	 * 不应当发生，因为 MyProc 只应在共享
	 * 内存初始化期间为 null。
	 */
	if (MyProc == NULL)
		elog(PANIC, "cannot wait without a PGPROC structure");

	if (MyProc->lwWaiting != LW_WS_NOT_WAITING)
		elog(PANIC, "queueing for lock while waiting on another one");

	LWLockWaitListLock(lock);

	/* 设置该标志受到自旋锁保护 */
	pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_HAS_WAITERS);

	MyProc->lwWaiting = LW_WS_WAITING;
	MyProc->lwWaitMode = mode;

	/* LW_WAIT_UNTIL_FREE 等待者总是排在队列最前面 */
	if (mode == LW_WAIT_UNTIL_FREE)
		proclist_push_head(&lock->waiters, MyProc->pgprocno, lwWaitLink);
	else
		proclist_push_tail(&lock->waiters, MyProc->pgprocno, lwWaitLink);

	/* 现在可以释放互斥锁了 */
	LWLockWaitListUnlock(lock);

#ifdef LOCK_DEBUG
	pg_atomic_fetch_add_u32(&lock->nwaiters, 1);
#endif
}

/*
 * 把自己从等待队列中移除。
 *
 * 这用于如下情况：我们以为需要睡眠，所以把自己入了队，
 * 但进一步检查之后，发现其实并不需要
 * 这么做。
 */
static void
LWLockDequeueSelf(LWLock *lock)
{
	bool		on_waitlist;

#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);

	lwstats->dequeue_self_count++;
#endif

	LWLockWaitListLock(lock);

	/*
	 * 把自己从等待队列移除，除非我们已经不在上面。
	 * 移除是在持有等待队列锁的情况下做的，因此这次检查
	 * 没有竞态。
	 */
	on_waitlist = MyProc->lwWaiting == LW_WS_WAITING;
	if (on_waitlist)
		proclist_delete(&lock->waiters, MyProc->pgprocno, lwWaitLink);

	if (proclist_is_empty(&lock->waiters) &&
		(pg_atomic_read_u32(&lock->state) & LW_FLAG_HAS_WAITERS) != 0)
	{
		pg_atomic_fetch_and_u32(&lock->state, ~LW_FLAG_HAS_WAITERS);
	}

	/* XXX：要和上面的 fetch_and 合并吗？ */
	LWLockWaitListUnlock(lock);

	/* 再次清除等待状态，便于调试 */
	if (on_waitlist)
		MyProc->lwWaiting = LW_WS_NOT_WAITING;
	else
	{
		int			extraWaits = 0;

		/*
		 * 别人已经把我们移出队列，并且已经或即将唤醒我们。要处理
		 * 这次被多余吸收掉的唤醒。
		 */

		/*
		 * 若有人在我们把自己移除之前就唤醒了我们，则重置 RELEASE_OK 标志
		 * --- 他们会把它设成 false。
		 */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

		/*
		 * 现在去等那次已经排好的唤醒，否则我们的 ->lwWaiting 会在
		 * 稍后某个不合适的时刻被重置。多数情况下这里
		 * 会立刻返回。
		 */
		for (;;)
		{
			PGSemaphoreLock(MyProc->sem);
			if (MyProc->lwWaiting == LW_WS_NOT_WAITING)
				break;
			extraWaits++;
		}

		/*
		 * 修正进程等待信号量的计数，补上被吸收的唤醒。
		 */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(MyProc->sem);
	}

#ifdef LOCK_DEBUG
	{
		/* 不再等待 */
		uint32		nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

		Assert(nwaiters < MAX_BACKENDS);
	}
#endif
}

/*
 * LWLockAcquire - 以指定模式取得一把轻量锁
 *
 * 若锁不可用，就睡到它可用为止。若锁
 * 立即可用则返回 true，若不得不睡眠则返回 false。
 *
 * 副作用：在释放锁之前，取消/die 中断被拦住。
 */
bool
LWLockAcquire(LWLock *lock, LWLockMode mode)
{
	PGPROC	   *proc = MyProc;
	bool		result = true;
	int			extraWaits = 0;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	Assert(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockAcquire", lock, mode);

#ifdef LWLOCK_STATS
	/* 统计取得锁的尝试次数 */
	if (mode == LW_EXCLUSIVE)
		lwstats->ex_acquire_count++;
	else
		lwstats->sh_acquire_count++;
#endif							/* LWLOCK_STATS（轻量锁统计） */

	/*
	 * 没有 PGPROC 就不能等待。这只应发生在
	 * 引导或共享内存初始化期间。这里放一条 Assert，
	 * 用来抓住不安全的写法。
	 */
	Assert(!(proc == NULL && IsUnderPostmaster));

	/* 确保还有位置记住这把锁 */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * 拦住取消/die 中断，直到退出由
	 * LWLock 保护的代码段。这样中断就不会干扰
	 * 对共享内存中数据结构的操作。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 在此循环：每次被
	 * LWLockRelease 发信号之后，都再尝试取得锁。
	 *
	 * NOTE：看起来让 LWLockRelease 直接把锁授给我们
	 * 更好，而不是重试、还可能再次进入睡眠。但
	 * 实践中那样不好，因为每当两个或多个进程争用同一把
	 * 锁时，每次取得锁都意味着一次进程切换。
	 * 由于 LWLocks 通常用来保护不太长的
	 * 计算片段，进程需要能在单个 CPU 时间片内多次取得并
	 * 释放同一把锁，即便
	 * 存在竞争。能够这样做的效率
	 * 胜过有时浪费一次进程调度
	 * 周期的低效：被释放的等待者终于运行时，锁可能已经不空闲。
	 * 参见 pgsql-hackers 归档 29-Dec-01。
	 */
	for (;;)
	{
		bool		mustwait;

		/*
		 * 第一次尝试夺取锁。此时我们还不在等待队列里，
		 * 或者已经不在了。
		 */
		mustwait = LWLockAttemptLock(lock, mode);

		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockAcquire", lock, "immediately acquired lock");
			break;				/* 已取得锁 */
		}

		/*
		 * 好，此刻第一次没能夺取这把锁。我们
		 * 无法简单地把自己排到链表末尾然后等着被
		 * 唤醒，因为此时锁可能早已被释放。
		 * 改为先把自己加入队列，再尝试夺取锁。若我们
		 * 成功，就撤销入队即可；否则就
		 * 重新检查这把锁。若仍然夺不到，我们就知道另一位
		 * 持锁者在释放时会看见我们的队列项，因为这些项
		 * 在我们检查锁之前就已经存在。
		 */

		/* 加入队列 */
		LWLockQueueSelf(lock, mode);

		/* 现在已经保证：必要时我们会被唤醒 */
		mustwait = LWLockAttemptLock(lock, mode);

		/* 好，第二轮拿到了锁，需要撤销入队 */
		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockAcquire", lock, "acquired, undoing queue");

			LWLockDequeueSelf(lock);
			break;
		}

		/*
		 * 一直等到被唤醒。
		 *
		 * 我们可能因为并非由
		 * LWLockRelease 发信号的原因而被唤醒。若如此，回到循环再等。一旦
		 * 已经取得这把 LWLock，就按额外收到的信号数
		 * 把信号量计数加回去。
		 */
		LOG_LWDEBUG("LWLockAcquire", lock, "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		LWLockReportWaitStart(lock);
		if (TRACE_POSTGRESQL_LWLOCK_WAIT_START_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), mode);

		for (;;)
		{
			PGSemaphoreLock(proc->sem);
			if (proc->lwWaiting == LW_WS_NOT_WAITING)
				break;
			extraWaits++;
		}

		/* 正在重试，允许 LWLockRelease 再次释放等待者。 */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

#ifdef LOCK_DEBUG
		{
			/* 不再等待 */
			uint32		nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

			Assert(nwaiters < MAX_BACKENDS);
		}
#endif

		if (TRACE_POSTGRESQL_LWLOCK_WAIT_DONE_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), mode);
		LWLockReportWaitEnd();

		LOG_LWDEBUG("LWLockAcquire", lock, "awakened");

		/* 现在回到循环，再次尝试取得锁。 */
		result = false;
	}

	if (TRACE_POSTGRESQL_LWLOCK_ACQUIRE_ENABLED())
		TRACE_POSTGRESQL_LWLOCK_ACQUIRE(T_NAME(lock), mode);

	/* 把锁加入本后端所持锁的列表 */
	held_lwlocks[num_held_lwlocks].lock = lock;
	held_lwlocks[num_held_lwlocks++].mode = mode;

	/*
	 * 修正进程等待信号量的计数，补上被吸收的唤醒。
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(proc->sem);

	return result;
}

/*
 * LWLockConditionalAcquire - 以指定模式取得一把轻量锁
 *
 * 若锁不可用，返回 false，并且没有副作用。
 *
 * 若成功，则在释放锁之前拦住取消/die 中断。
 */
bool
LWLockConditionalAcquire(LWLock *lock, LWLockMode mode)
{
	bool		mustwait;

	Assert(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockConditionalAcquire", lock, mode);

	/* 确保还有位置记住这把锁 */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * 拦住取消/die 中断，直到退出由
	 * LWLock 保护的代码段。这样中断就不会干扰
	 * 对共享内存中数据结构的操作。
	 */
	HOLD_INTERRUPTS();

	/* 检查这把锁 */
	mustwait = LWLockAttemptLock(lock, mode);

	if (mustwait)
	{
		/* 未能取得锁，于是解除中断拦截 */
		RESUME_INTERRUPTS();

		LOG_LWDEBUG("LWLockConditionalAcquire", lock, "failed");
		if (TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE_FAIL_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE_FAIL(T_NAME(lock), mode);
	}
	else
	{
		/* 把锁加入本后端所持锁的列表 */
		held_lwlocks[num_held_lwlocks].lock = lock;
		held_lwlocks[num_held_lwlocks++].mode = mode;
		if (TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_CONDACQUIRE(T_NAME(lock), mode);
	}
	return !mustwait;
}

/*
 * LWLockAcquireOrWait - 取得锁，或一直等到它空闲
 *
 * 本函数的语义有点古怪。若锁当前
 * 空闲，就按给定模式取得，函数返回 true。若
 * 锁并非立即空闲，函数会等到它被释放
 * 后返回 false，但并不取得该锁。
 *
 * 目前用于 WALWriteLock：某个后端刷出 WAL 时
 * 持有 WALWriteLock，副作用是可能刷出许多其他
 * 后端的提交记录。那些后端需要等到
 * 刷出结束，但不再需要取得这把锁。它们只需
 * 醒来，看到自己的记录已经刷出，然后返回。
 */
bool
LWLockAcquireOrWait(LWLock *lock, LWLockMode mode)
{
	PGPROC	   *proc = MyProc;
	bool		mustwait;
	int			extraWaits = 0;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	Assert(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	PRINT_LWDEBUG("LWLockAcquireOrWait", lock, mode);

	/* 确保还有位置记住这把锁 */
	if (num_held_lwlocks >= MAX_SIMUL_LWLOCKS)
		elog(ERROR, "too many LWLocks taken");

	/*
	 * 拦住取消/die 中断，直到退出由
	 * LWLock 保护的代码段。这样中断就不会干扰
	 * 对共享内存中数据结构的操作。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 注意：我们用的几乎就是与 LWLockAcquire() 相同的、
	 * 连续两次取得锁的办法。细节见其注释。
	 */
	mustwait = LWLockAttemptLock(lock, mode);

	if (mustwait)
	{
		LWLockQueueSelf(lock, LW_WAIT_UNTIL_FREE);

		mustwait = LWLockAttemptLock(lock, mode);

		if (mustwait)
		{
			/*
			 * 一直等到被唤醒。与 LWLockAcquire 里一样，要防备
			 * 虚假唤醒。
			 */
			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "waiting");

#ifdef LWLOCK_STATS
			lwstats->block_count++;
#endif

			LWLockReportWaitStart(lock);
			if (TRACE_POSTGRESQL_LWLOCK_WAIT_START_ENABLED())
				TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), mode);

			for (;;)
			{
				PGSemaphoreLock(proc->sem);
				if (proc->lwWaiting == LW_WS_NOT_WAITING)
					break;
				extraWaits++;
			}

#ifdef LOCK_DEBUG
			{
				/* 不再等待 */
				uint32		nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

				Assert(nwaiters < MAX_BACKENDS);
			}
#endif
			if (TRACE_POSTGRESQL_LWLOCK_WAIT_DONE_ENABLED())
				TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), mode);
			LWLockReportWaitEnd();

			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "awakened");
		}
		else
		{
			LOG_LWDEBUG("LWLockAcquireOrWait", lock, "acquired, undoing queue");

			/*
			 * 第二次尝试拿到了锁，撤销入队。必须把
			 * 这视为已经成功取得了锁，否则我们
			 * 未必会去唤醒那些被我们挡住、因而没能取得
			 * 这把锁的人。
			 */
			LWLockDequeueSelf(lock);
		}
	}

	/*
	 * 修正进程等待信号量的计数，补上被吸收的唤醒。
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(proc->sem);

	if (mustwait)
	{
		/* 未能取得锁，于是解除中断拦截 */
		RESUME_INTERRUPTS();
		LOG_LWDEBUG("LWLockAcquireOrWait", lock, "failed");
		if (TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT_FAIL_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT_FAIL(T_NAME(lock), mode);
	}
	else
	{
		LOG_LWDEBUG("LWLockAcquireOrWait", lock, "succeeded");
		/* 把锁加入本后端所持锁的列表 */
		held_lwlocks[num_held_lwlocks].lock = lock;
		held_lwlocks[num_held_lwlocks++].mode = mode;
		if (TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_ACQUIRE_OR_WAIT(T_NAME(lock), mode);
	}

	return !mustwait;
}

/*
 * 按这把轻量锁的当前状态，是否需要等待变量的值
 * 发生变化？
 *
 * 若不必等待，而且原因是变量的值已经
 * 变了，就把当前值存进 newval。
 *
 * 若锁空闲，则把 *result 设为 true，否则设为 false。
 */
static bool
LWLockConflictsWithVar(LWLock *lock,
					   uint64 *valptr, uint64 oldval, uint64 *newval,
					   bool *result)
{
	bool		mustwait;
	uint64		value;

	/*
	 * 先看这个槽眼下是否空闲。
	 *
	 * XXX：调用者在此之前用了自旋锁，就当前用法而言，这里不需要内存
	 * 屏障。但一般说来
	 * 那样未必安全。
	 */
	mustwait = (pg_atomic_read_u32(&lock->state) & LW_VAL_EXCLUSIVE) != 0;

	if (!mustwait)
	{
		*result = true;
		return false;
	}

	*result = false;

	/*
	 * 在这把轻量锁的等待队列锁保护下读取该值，因为一般不能
	 * 依赖原子的 64 位读/存。TODO：在能够
	 * 做原子 64 位读/写的平台上，这把自旋锁应当被优化掉。
	 */
	LWLockWaitListLock(lock);
	value = *valptr;
	LWLockWaitListUnlock(lock);

	if (value != oldval)
	{
		mustwait = false;
		*newval = value;
	}
	else
	{
		mustwait = true;
	}

	return mustwait;
}

/*
 * LWLockWaitForVar - 等到锁空闲，或某个变量被更新。
 *
 * 若锁被持有且 *valptr 等于 oldval，就一直等到锁被
 * 释放，或者持锁者调用
 * LWLockUpdateVar 更新了 *valptr。若退出时锁是空闲的（立刻如此，或经过
 * 等待之后），返回 true。若锁仍被持有，但 *valptr 已不再
 * 与 oldval 相符，则返回 false，并把 *newval 设为
 * *valptr 里的当前值。
 *
 * 注意：本函数忽略共享锁的持有者；若锁以
 * 共享模式被持有，则返回 'true'。
 */
bool
LWLockWaitForVar(LWLock *lock, uint64 *valptr, uint64 oldval, uint64 *newval)
{
	PGPROC	   *proc = MyProc;
	int			extraWaits = 0;
	bool		result = false;
#ifdef LWLOCK_STATS
	lwlock_stats *lwstats;

	lwstats = get_lwlock_stats_entry(lock);
#endif

	PRINT_LWDEBUG("LWLockWaitForVar", lock, LW_WAIT_UNTIL_FREE);

	/*
	 * 在这把锁上睡眠时，拦住取消/die 中断。如果我们被
	 * 中断，并没有清理机制能把我们从等待队列
	 * 里去掉。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 在此循环：每次收到信号后，都检查这把锁的状态。
	 */
	for (;;)
	{
		bool		mustwait;

		mustwait = LWLockConflictsWithVar(lock, valptr, oldval, newval,
										  &result);

		if (!mustwait)
			break;				/* 锁已经空闲，或者值已经对不上 */

		/*
		 * 把自己加入等待队列。注意这里有竞态，别人
		 * 可能在我们完成入队之前就被唤醒。注意：我们用的几乎就是
		 * 与 LWLockAcquire() 相同的连续两次
		 * 取得锁的办法。细节见其注释。唯一
		 * 差别是，在检查锁状态的同时，还要检查变量的
		 * 值。
		 */
		LWLockQueueSelf(lock, LW_WAIT_UNTIL_FREE);

		/*
		 * 设置 RELEASE_OK 标志，以确保锁一被释放
		 * 我们就被唤醒。
		 */
		pg_atomic_fetch_or_u32(&lock->state, LW_FLAG_RELEASE_OK);

		/*
		 * 现在已经保证：必要时我们会被唤醒。重新检查锁
		 * 以及变量的状态。
		 */
		mustwait = LWLockConflictsWithVar(lock, valptr, oldval, newval,
										  &result);

		/* 好，入队之后已经没有冲突。撤销入队。 */
		if (!mustwait)
		{
			LOG_LWDEBUG("LWLockWaitForVar", lock, "free, undoing queue");

			LWLockDequeueSelf(lock);
			break;
		}

		/*
		 * 一直等到被唤醒。
		 *
		 * 我们可能因为并非由
		 * LWLockRelease 发信号的原因而被唤醒。若如此，回到循环再等。一旦
		 * 已经取得这把 LWLock，就按额外收到的信号数
		 * 把信号量计数加回去。
		 */
		LOG_LWDEBUG("LWLockWaitForVar", lock, "waiting");

#ifdef LWLOCK_STATS
		lwstats->block_count++;
#endif

		LWLockReportWaitStart(lock);
		if (TRACE_POSTGRESQL_LWLOCK_WAIT_START_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_WAIT_START(T_NAME(lock), LW_EXCLUSIVE);

		for (;;)
		{
			PGSemaphoreLock(proc->sem);
			if (proc->lwWaiting == LW_WS_NOT_WAITING)
				break;
			extraWaits++;
		}

#ifdef LOCK_DEBUG
		{
			/* 不再等待 */
			uint32		nwaiters PG_USED_FOR_ASSERTS_ONLY = pg_atomic_fetch_sub_u32(&lock->nwaiters, 1);

			Assert(nwaiters < MAX_BACKENDS);
		}
#endif

		if (TRACE_POSTGRESQL_LWLOCK_WAIT_DONE_ENABLED())
			TRACE_POSTGRESQL_LWLOCK_WAIT_DONE(T_NAME(lock), LW_EXCLUSIVE);
		LWLockReportWaitEnd();

		LOG_LWDEBUG("LWLockWaitForVar", lock, "awakened");

		/* 现在回到循环，再次检查这把锁的状态。 */
	}

	/*
	 * 修正进程等待信号量的计数，补上被吸收的唤醒。
	 */
	while (extraWaits-- > 0)
		PGSemaphoreUnlock(proc->sem);

	/*
	 * 现在可以允许取消/die 中断了。
	 */
	RESUME_INTERRUPTS();

	return result;
}


/*
 * LWLockUpdateVar - 原子地更新一个变量并唤醒等待者
 *
 * 把 *valptr 设为 'val'，并唤醒所有正通过
 * LWLockWaitForVar() 等待我们的进程。设值与唤醒这些进程是
 * 原子完成的，因此任何在同一把锁上调用 LWLockWaitForVar() 的进程
 * 都保证能看见新值，并据此行动。
 *
 * 调用者必须以排他模式持有该锁。
 */
void
LWLockUpdateVar(LWLock *lock, uint64 *valptr, uint64 val)
{
	proclist_head wakeup;
	proclist_mutable_iter iter;

	PRINT_LWDEBUG("LWLockUpdateVar", lock, LW_EXCLUSIVE);

	proclist_init(&wakeup);

	LWLockWaitListLock(lock);

	Assert(pg_atomic_read_u32(&lock->state) & LW_VAL_EXCLUSIVE);

	/* 更新这把锁的值 */
	*valptr = val;

	/*
	 * 看看有没有需要被唤醒的 LW_WAIT_UNTIL_FREE 等待者。
	 * 他们总是排在队列的最前面。
	 */
	proclist_foreach_modify(iter, &lock->waiters, lwWaitLink)
	{
		PGPROC	   *waiter = GetPGProcByNumber(iter.cur);

		if (waiter->lwWaitMode != LW_WAIT_UNTIL_FREE)
			break;

		proclist_delete(&lock->waiters, iter.cur, lwWaitLink);
		proclist_push_tail(&wakeup, iter.cur, lwWaitLink);

		/* 见 LWLockWakeup() */
		Assert(waiter->lwWaiting == LW_WS_WAITING);
		waiter->lwWaiting = LW_WS_PENDING_WAKEUP;
	}

	/* 锁本身的共享状态已经更新完毕。 */
	LWLockWaitListUnlock(lock);

	/*
	 * 唤醒我从队列里移除的那些等待者。
	 */
	proclist_foreach_modify(iter, &wakeup, lwWaitLink)
	{
		PGPROC	   *waiter = GetPGProcByNumber(iter.cur);

		proclist_delete(&wakeup, iter.cur, lwWaitLink);
		/* 关于这个屏障，见 LWLockWakeup() 里的注释 */
		pg_write_barrier();
		waiter->lwWaiting = LW_WS_NOT_WAITING;
		PGSemaphoreUnlock(waiter->sem);
	}
}


/*
 * LWLockRelease - 释放先前取得的锁
 */
void
LWLockRelease(LWLock *lock)
{
	LWLockMode	mode;
	uint32		oldstate;
	bool		check_waiters;
	int			i;

	/*
	 * 把这把锁从持有列表里去掉。通常（但并不总是）它就是
	 * 最近取得的那一把，所以从数组末尾向前找。
	 */
	for (i = num_held_lwlocks; --i >= 0;)
		if (lock == held_lwlocks[i].lock)
			break;

	if (i < 0)
		elog(ERROR, "lock %s is not held", T_NAME(lock));

	mode = held_lwlocks[i].mode;

	num_held_lwlocks--;
	for (; i < num_held_lwlocks; i++)
		held_lwlocks[i] = held_lwlocks[i + 1];

	PRINT_LWDEBUG("LWLockRelease", lock, mode);

	/*
	 * 放开我对这把锁的持有。此后别人可以立刻取得它，
	 * 即便我们还得去唤醒其他等待者。
	 */
	if (mode == LW_EXCLUSIVE)
		oldstate = pg_atomic_sub_fetch_u32(&lock->state, LW_VAL_EXCLUSIVE);
	else
		oldstate = pg_atomic_sub_fetch_u32(&lock->state, LW_VAL_SHARED);

	/* 别人不可能还持有这种锁 */
	Assert(!(oldstate & LW_VAL_EXCLUSIVE));

	if (TRACE_POSTGRESQL_LWLOCK_RELEASE_ENABLED())
		TRACE_POSTGRESQL_LWLOCK_RELEASE(T_NAME(lock));

	/*
	 * 那些后端还在等着被调度上来，不要再次
	 * 唤醒它们。
	 */
	if ((oldstate & (LW_FLAG_HAS_WAITERS | LW_FLAG_RELEASE_OK)) ==
		(LW_FLAG_HAS_WAITERS | LW_FLAG_RELEASE_OK) &&
		(oldstate & LW_LOCK_MASK) == 0)
		check_waiters = true;
	else
		check_waiters = false;

	/*
	 * 唤醒等待者需要先取得自旋锁，因此只在
	 * 有必要时才做。
	 */
	if (check_waiters)
	{
		/* XXX：提交前要删掉吗？ */
		LOG_LWDEBUG("LWLockRelease", lock, "releasing waiters");
		LWLockWakeup(lock);
	}

	/*
	 * 现在可以允许取消/die 中断了。
	 */
	RESUME_INTERRUPTS();
}

/*
 * LWLockReleaseClearVar - 释放先前取得的锁，并重置变量
 */
void
LWLockReleaseClearVar(LWLock *lock, uint64 *valptr, uint64 val)
{
	LWLockWaitListLock(lock);

	/*
	 * 释放锁之前先设置变量的值，以免出现这样的
	 * 竞态：新的加锁者已经取得锁，但还没有
	 * 设置变量的值。
	 */
	*valptr = val;
	LWLockWaitListUnlock(lock);

	LWLockRelease(lock);
}


/*
 * LWLockReleaseAll - 释放当前持有的全部锁
 *
 * 用于 ereport(ERROR) 之后做清理。本函数与
 * 逐次调用 LWLockRelease 的重要差别在于，此操作不改变
 * InterruptHoldoffCount。这是必要的，因为在错误恢复的更早阶段，
 * InterruptHoldoffCount 已被设到合适的水平。若允许它随每把被释放的锁下降，
 * 我们就可能把它减到零以下！
 */
void
LWLockReleaseAll(void)
{
	while (num_held_lwlocks > 0)
	{
		HOLD_INTERRUPTS();		/* 与随后的 RESUME_INTERRUPTS 配对 */

		LWLockRelease(held_lwlocks[num_held_lwlocks - 1].lock);
	}
}


/*
 * LWLockHeldByMe - 测试本进程是否以任意模式持有某把锁
 *
 * 仅供调试之用。
 */
bool
LWLockHeldByMe(LWLock *lock)
{
	int			i;

	for (i = 0; i < num_held_lwlocks; i++)
	{
		if (held_lwlocks[i].lock == lock)
			return true;
	}
	return false;
}

/*
 * LWLockHeldByMe - 测试本进程是否持有某个锁数组中的任意一把
 *
 * 仅供调试之用。
 */
bool
LWLockAnyHeldByMe(LWLock *lock, int nlocks, size_t stride)
{
	char	   *held_lock_addr;
	char	   *begin;
	char	   *end;
	int			i;

	begin = (char *) lock;
	end = begin + nlocks * stride;
	for (i = 0; i < num_held_lwlocks; i++)
	{
		held_lock_addr = (char *) held_lwlocks[i].lock;
		if (held_lock_addr >= begin &&
			held_lock_addr < end &&
			(held_lock_addr - begin) % stride == 0)
			return true;
	}
	return false;
}

/*
 * LWLockHeldByMeInMode - 测试本进程是否以给定模式持有某把锁
 *
 * 仅供调试之用。
 */
bool
LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode)
{
	int			i;

	for (i = 0; i < num_held_lwlocks; i++)
	{
		if (held_lwlocks[i].lock == lock && held_lwlocks[i].mode == mode)
			return true;
	}
	return false;
}
