/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES 进程间通信初始化代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipci.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/twophase.h"
#include "access/xlogprefetcher.h"
#include "access/xlogrecovery.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

/* 运行时参数 GUCs */
int			shared_memory_type = DEFAULT_SHARED_MEMORY_TYPE;

shmem_startup_hook_type shmem_startup_hook = NULL;

static Size total_addin_request = 0;

/*
 * RequestAddinShmemSpace
 *		请求为可加载模块额外分配
 *		共享内存空间。
 *
 * 这只能经由载入 postmaster 的库的 shmem_request_hook 调用，
 * 该库通过 shared_preload_libraries 载入。从其他地方
 * 调用将会失败。
 */
void
RequestAddinShmemSpace(Size size)
{
	if (!process_shmem_requests_in_progress)
		elog(FATAL, "cannot request additional shared memory outside shmem_request_hook");
	total_addin_request = add_size(total_addin_request, size);
}

/*
 * CalculateShmemSize
 *		计算所需的共享内存量和信号量数量。
 *
 * 若 num_semaphores 不是 NULL，它会被设为
 * 所需的信号量个数。
 */
Size
CalculateShmemSize(int *num_semaphores)
{
	Size		size;
	int			numSemas;

	/* 计算我们将需要的信号量个数 */
	numSemas = ProcGlobalSemas();
	numSemas += SpinlockSemas();

	/* 若调用方请求，则返回信号量个数 */
	if (num_semaphores)
		*num_semaphores = numSemas;

	/*
	 * Postgres 共享内存块的大小，是用对大户还算
	 * 准确的估计，再加上 10 万字节给那些小到
	 * 不值得估计的东西。
	 *
	 * 我们留心确保总的大小请求不会
	 * 溢出 size_t。若这一关过了，实际分配阶段
	 * 就不必这么小心。
	 */
	size = 100000;
	size = add_size(size, PGSemaphoreShmemSize(numSemas));
	size = add_size(size, SpinlockSemaSize());
	size = add_size(size, hash_estimate_size(SHMEM_INDEX_SIZE,
											 sizeof(ShmemIndexEnt)));
	size = add_size(size, dsm_estimate_size());
	size = add_size(size, BufferShmemSize());
	size = add_size(size, LockShmemSize());
	size = add_size(size, PredicateLockShmemSize());
	size = add_size(size, ProcGlobalShmemSize());
	size = add_size(size, XLogPrefetchShmemSize());
	size = add_size(size, XLOGShmemSize());
	size = add_size(size, XLogRecoveryShmemSize());
	size = add_size(size, CLOGShmemSize());
	size = add_size(size, CommitTsShmemSize());
	size = add_size(size, SUBTRANSShmemSize());
	size = add_size(size, TwoPhaseShmemSize());
	size = add_size(size, BackgroundWorkerShmemSize());
	size = add_size(size, MultiXactShmemSize());
	size = add_size(size, LWLockShmemSize());
	size = add_size(size, ProcArrayShmemSize());
	size = add_size(size, BackendStatusShmemSize());
	size = add_size(size, SInvalShmemSize());
	size = add_size(size, PMSignalShmemSize());
	size = add_size(size, ProcSignalShmemSize());
	size = add_size(size, CheckpointerShmemSize());
	size = add_size(size, AutoVacuumShmemSize());
	size = add_size(size, ReplicationSlotsShmemSize());
	size = add_size(size, ReplicationOriginShmemSize());
	size = add_size(size, WalSndShmemSize());
	size = add_size(size, WalRcvShmemSize());
	size = add_size(size, PgArchShmemSize());
	size = add_size(size, ApplyLauncherShmemSize());
	size = add_size(size, SnapMgrShmemSize());
	size = add_size(size, BTreeShmemSize());
	size = add_size(size, SyncScanShmemSize());
	size = add_size(size, AsyncShmemSize());
	size = add_size(size, StatsShmemSize());
#ifdef EXEC_BACKEND
	size = add_size(size, ShmemBackendArraySize());
#endif

	/* 计入预载库额外请求的共享内存 */
	size = add_size(size, total_addin_request);

	/* 不妨把它凑成典型页大小的整数倍 */
	size = add_size(size, 8192 - (size % 8192));

	return size;
}

/*
 * CreateSharedMemoryAndSemaphores
 *		创建并初始化共享内存和信号量。
 *
 * 由 postmaster 或独立后端调用。
 * 在 EXEC_BACKEND 情形下，也由从 postmaster fork 出的后端调用。
 * 后一种情况下，共享内存段已存在并已被物理挂接，
 * 但我们必须初始化本地内存中指向共享结构的指针，
 * 因为我们没有像 fork() 情形那样从 postmaster
 * 继承正确的指针值。最容易的做法是再走一遍
 * 与先前相同的代码。（注意，被调用的例程多半检查
 * IsUnderPostmaster，而不是 EXEC_BACKEND，来发现这种情况。
 * 这有点浪费代码，
 * 可以再整理。）
 */
void
CreateSharedMemoryAndSemaphores(void)
{
	PGShmemHeader *shim = NULL;

	if (!IsUnderPostmaster)
	{
		PGShmemHeader *seghdr;
		Size		size;
		int			numSemas;

		/* 计算共享内存块的大小 */
		size = CalculateShmemSize(&numSemas);
		elog(DEBUG3, "invoking IpcMemoryCreate(size=%zu)", size);

		/*
		 * 创建共享内存段
		 */
		seghdr = PGSharedMemoryCreate(size, &shim);

		InitShmemAccess(seghdr);

		/*
		 * 创建信号量
		 */
		PGReserveSemaphores(numSemas);

		/*
		 * 若自旋锁被禁用，就初始化仿真层（它
		 * 依赖信号量，所以这里的顺序很重要）。
		 */
#ifndef HAVE_SPINLOCKS
		SpinlockSemaInit();
#endif
	}
	else
	{
		/*
		 * 我们正在重新挂接到一段已有的共享内存。这
		 * 只应在 EXEC_BACKEND 情形下到达。
		 */
#ifndef EXEC_BACKEND
		elog(PANIC, "should be attached to shared memory already");
#endif
	}

	/*
	 * 建立共享内存分配机制
	 */
	if (!IsUnderPostmaster)
		InitShmemAllocation();

	/*
	 * 现在初始化 LWLocks，它们会做共享内存分配，并且
	 * InitShmemIndex 需要它们。
	 */
	CreateLWLocks();

	/*
	 * 建立 shmem.c 的索引哈希表
	 */
	InitShmemIndex();

	dsm_shmem_init();

	/*
	 * 建立 xlog、clog 和缓冲区
	 */
	XLOGShmemInit();
	XLogPrefetchShmemInit();
	XLogRecoveryShmemInit();
	CLOGShmemInit();
	CommitTsShmemInit();
	SUBTRANSShmemInit();
	MultiXactShmemInit();
	InitBufferPool();

	/*
	 * 建立锁管理器
	 */
	InitLocks();

	/*
	 * 建立谓词锁管理器
	 */
	InitPredicateLocks();

	/*
	 * 建立进程表
	 */
	if (!IsUnderPostmaster)
		InitProcGlobal();
	CreateSharedProcArray();
	CreateSharedBackendStatus();
	TwoPhaseShmemInit();
	BackgroundWorkerShmemInit();

	/*
	 * 建立共享失效消息传递
	 */
	CreateSharedInvalidationState();

	/*
	 * 建立进程间信号机制
	 */
	PMSignalShmemInit();
	ProcSignalShmemInit();
	CheckpointerShmemInit();
	AutoVacuumShmemInit();
	ReplicationSlotsShmemInit();
	ReplicationOriginShmemInit();
	WalSndShmemInit();
	WalRcvShmemInit();
	PgArchShmemInit();
	ApplyLauncherShmemInit();

	/*
	 * 建立其他需要一些共享内存空间的模块
	 */
	SnapMgrInit();
	BTreeShmemInit();
	SyncScanShmemInit();
	AsyncShmemInit();
	StatsShmemInit();

#ifdef EXEC_BACKEND

	/*
	 * 分配 win32 共享后端数组
	 */
	if (!IsUnderPostmaster)
		ShmemBackendArrayAllocation();
#endif

	/* 初始化动态共享内存设施。 */
	if (!IsUnderPostmaster)
		dsm_postmaster_startup(shim);

	/*
	 * 现在给可加载模块机会，建立它们的共享内存分配
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}

/*
 * InitializeShmemGUCs
 *
 * 本函数初始化与当前配置所需共享内存量
 * 有关的、运行时计算的 GUCs。
 */
void
InitializeShmemGUCs(void)
{
	char		buf[64];
	Size		size_b;
	Size		size_mb;
	Size		hp_size;

	/*
	 * 计算共享内存大小，并向上取整到最近的兆字节。
	 */
	size_b = CalculateShmemSize(NULL);
	size_mb = add_size(size_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	/*
	 * 计算所需的大页数量。
	 */
	GetHugePageSize(&hp_size, NULL);
	if (hp_size != 0)
	{
		Size		hp_required;

		hp_required = add_size(size_b / hp_size, 1);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}
}
