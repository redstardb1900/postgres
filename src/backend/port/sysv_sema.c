/*-------------------------------------------------------------------------
 *
 * sysv_sema.c
 *	  用 SysV 信号量设施实现 PGSemaphore
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/sysv_sema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"


typedef struct PGSemaphoreData
{
	int			semId;			/* 信号量集标识符 */
	int			semNum;			/* 集内信号量编号 */
} PGSemaphoreData;

#ifndef HAVE_UNION_SEMUN
union semun
{
	int			val;
	struct semid_ds *buf;
	unsigned short *array;
};
#endif

typedef key_t IpcSemaphoreKey;	/* 传给 semget(2) 的信号量键 */
typedef int IpcSemaphoreId;		/* semget(2) 返回的信号量 ID */

/*
 * SEMAS_PER_SET 是我们分配的每个信号量集中
 * 有用信号量个数。必须*小于*内核 SEMMSL（每集
 * 最大信号量数）参数，该值常约 25。（小于是因
 * 每集多分配一个信号量供标识用。）
 */
#define SEMAS_PER_SET	16

#define IPCProtection	(0600)	/* 仅由用户访问/修改 */

#define PGSemaMagic		537		/* 必须小于 SEMVMX */


static PGSemaphore sharedSemas; /* 共享内存中的 PGSemaphoreData 数组 */
static int	numSharedSemas;		/* 迄今已用的 PGSemaphoreData 数 */
static int	maxSharedSemas;		/* PGSemaphoreData 数组已分配大小 */
static IpcSemaphoreId *mySemaSets;	/* 已获取的信号量集 ID */
static int	numSemaSets;		/* 已获取的信号量集个数 */
static int	maxSemaSets;		/* mySemaSets 数组已分配大小 */
static IpcSemaphoreKey nextSemaKey; /* 下一待试键 */
static int	nextSemaNumber;		/* 末集下一空闲编号 */


static IpcSemaphoreId InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey,
												 int numSems, bool retry_ok);
static void IpcSemaphoreInitialize(IpcSemaphoreId semId, int semNum,
								   int value);
static void IpcSemaphoreKill(IpcSemaphoreId semId);
static int	IpcSemaphoreGetValue(IpcSemaphoreId semId, int semNum);
static pid_t IpcSemaphoreGetLastPID(IpcSemaphoreId semId, int semNum);
static IpcSemaphoreId IpcSemaphoreCreate(int numSems);
static void ReleaseSemaphores(int status, Datum arg);


/*
 * InternalIpcSemaphoreCreate
 *
 * 尝试用指定键创建新信号量集。
 * 若该集已存在则失败（返回 -1）。
 *
 * 若失败且失败码不是与已有集合冲突，
 * 则打印错误并中止。其他错误暗示不可恢复的问题
 *
 *
 * 可惜有时很难判断错误是否不可恢复。调用方
 * 跟踪继续重试是否合理；若不合理，则无论
 * errno 为何，失败即中止。
 */
static IpcSemaphoreId
InternalIpcSemaphoreCreate(IpcSemaphoreKey semKey, int numSems, bool retry_ok)
{
	int			semId;

	semId = semget(semKey, numSems, IPC_CREAT | IPC_EXCL | IPCProtection);

	if (semId < 0)
	{
		int			saved_errno = errno;

		/*
		 * 若错误暗示与已有集合冲突且调用方尚未失去
		 * 耐心，则安静失败。
		 *
		 * 既说了 IPC_EXCL，本预期 EEXIST，但或许
		 * 会得到权限违规。某些平台若已有集合信号量
		 * 太少会报告 EINVAL。
		 * 此外，旧集已排定销毁但未消失时也可能出 EIDRM。
		 *
		 *
		 * EINVAL 是我们需要调用方层循环上限的关键
		 * 原因，因其也可能表示平台 SEMMSL 小于
		 * numSems，而换键无法修复该情况。
		 */
		if (retry_ok &&
			(saved_errno == EEXIST
			 || saved_errno == EACCES
			 || saved_errno == EINVAL
#ifdef EIDRM
			 || saved_errno == EIDRM
#endif
			 ))
			return -1;

		/*
		 * 否则抱怨并中止
		 */
		ereport(FATAL,
				(errmsg("could not create semaphores: %m"),
				 errdetail("Failed system call was semget(%lu, %d, 0%o).",
						   (unsigned long) semKey, numSems,
						   IPC_CREAT | IPC_EXCL | IPCProtection),
				 (saved_errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space.  "
						 "It occurs when either the system limit for the maximum number of "
						 "semaphore sets (SEMMNI), or the system wide maximum number of "
						 "semaphores (SEMMNS), would be exceeded.  You need to raise the "
						 "respective kernel parameter.  Alternatively, reduce PostgreSQL's "
						 "consumption of semaphores by reducing its max_connections parameter.\n"
						 "The PostgreSQL documentation contains more information about "
						 "configuring your system for PostgreSQL.") : 0));
	}

	return semId;
}

/*
 * 把信号量初始化为指定值。
 */
static void
IpcSemaphoreInitialize(IpcSemaphoreId semId, int semNum, int value)
{
	union semun semun;

	semun.val = value;
	if (semctl(semId, semNum, SETVAL, semun) < 0)
	{
		int			saved_errno = errno;

		ereport(FATAL,
				(errmsg_internal("semctl(%d, %d, SETVAL, %d) failed: %m",
								 semId, semNum, value),
				 (saved_errno == ERANGE) ?
				 errhint("You possibly need to raise your kernel's SEMVMX value to be at least "
						 "%d.  Look into the PostgreSQL documentation for details.",
						 value) : 0));
	}
}

/*
 * IpcSemaphoreKill(semId)	- 移除信号量集
 */
static void
IpcSemaphoreKill(IpcSemaphoreId semId)
{
	union semun semun;

	semun.val = 0;				/* 未用，但让编译器安静 */

	if (semctl(semId, 0, IPC_RMID, semun) < 0)
		elog(LOG, "semctl(%d, 0, IPC_RMID, ...) failed: %m", semId);
}

/* 获取信号量当前值（semval） */
static int
IpcSemaphoreGetValue(IpcSemaphoreId semId, int semNum)
{
	union semun dummy;			/* for Solaris */

	dummy.val = 0;				/* unused */

	return semctl(semId, semNum, GETVAL, dummy);
}

/* 获取最后对该信号量做 semop() 的进程 PID */
static pid_t
IpcSemaphoreGetLastPID(IpcSemaphoreId semId, int semNum)
{
	union semun dummy;			/* for Solaris */

	dummy.val = 0;				/* unused */

	return semctl(semId, semNum, GETPID, dummy);
}


/*
 * 创建含给定数量有用信号量的信号量集
 * 实际多分配一个信号量用作标识。
 * 若发现已死的 Postgres 信号量集则回收，但与
 * 非 Postgres 集冲突时不失败。
 *
 * 用意是检出并重用可能由崩溃的 postmaster
 * 或 backend 分配过的键。
 */
static IpcSemaphoreId
IpcSemaphoreCreate(int numSems)
{
	int			num_tries = 0;
	IpcSemaphoreId semId;
	union semun semun;
	PGSemaphoreData mysema;

	/* 循环直至找到空闲 IPC 键 */
	for (nextSemaKey++;; nextSemaKey++, num_tries++)
	{
		pid_t		creatorPID;

		/*
		 * 尝试创建新信号量集。试过 1000 个不同 IPC
		 * 键后放弃。
		 */
		semId = InternalIpcSemaphoreCreate(nextSemaKey, numSems + 1,
										   num_tries < 1000);
		if (semId >= 0)
			break;				/* 创建成功 */

		/* 看是否像死掉的 Postgres 进程遗留 */
		semId = semget(nextSemaKey, numSems + 1, 0);
		if (semId < 0)
			continue;			/* 失败：必属其他应用 */
		if (IpcSemaphoreGetValue(semId, numSems) != PGSemaMagic)
			continue;			/* 信号量属于非 Postgres 应用 */

		/*
		 * 若创建者 PID 属我自己，或不属于任何现存进程，
		 * 则可安全干掉它。
		 */
		creatorPID = IpcSemaphoreGetLastPID(semId, numSems);
		if (creatorPID <= 0)
			continue;			/* 糟，GETPID 失败 */
		if (creatorPID != getpid())
		{
			if (kill(creatorPID, 0) == 0 || errno != ESRCH)
				continue;		/* 信号量属存活进程 */
		}

		/*
		 * 该信号量集看似来自已死 Postgres 进程，或
		 * 来自本进程先前生命周期。尽可能干掉它。
		 * 这大概不该失败；若失败，则假定信号量集
		 * 终究属于别人，安静继续。
		 */
		semun.val = 0;			/* 未用，但让编译器安静 */
		if (semctl(semId, 0, IPC_RMID, semun) < 0)
			continue;

		/*
		 * 现在再试创建该信号量集。
		 */
		semId = InternalIpcSemaphoreCreate(nextSemaKey, numSems + 1, true);
		if (semId >= 0)
			break;				/* 创建成功 */

		/*
		 * 仅当其他进程抢先用同一信号量键创建时才会
		 * 到此。让他用那个，循环试下一键。
		 *
		 */
	}

	/*
	 * 好，已创建新信号量集。标记为本进程所创。我们
	 * 把备用信号量设为 PGSemaMagic-1，再用
	 * semop() 递增。最终值为 PGSemaMagic，且
	 * sempid 引用本进程。
	 */
	IpcSemaphoreInitialize(semId, numSems, PGSemaMagic - 1);
	mysema.semId = semId;
	mysema.semNum = numSems;
	PGSemaphoreUnlock(&mysema);

	return semId;
}


/*
 * 报告信号量所需的共享内存空间
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
	return mul_size(maxSemas, sizeof(PGSemaphoreData));
}

/*
 * PGReserveSemaphores --- 初始化信号量支持
 *
 * 在 postmaster 启动或共享内存重初始化时调用。
 * 应做足准备以支持最多 maxSemas 次后续
 * PGSemaphoreCreate 调用。此外，若在此或在
 * PGSemaphoreCreate 中获取了系统资源，须注册
 * on_shmem_exit 回调。
 *
 * SysV 实现按需获取信号量集；maxSemas 参数
 * 仅用于确定数组大小。共享内存中有
 * PGSemaphoreData 结构数组，另有 postmaster
 * 本地数组，每 SysV 信号量集一项，用于完成后
 * 释放信号量集。（此设计确保 postmaster 关机
 * 不依赖共享内存内容，因失败的 backend 可能已
 * 破坏它。）
 */
void
PGReserveSemaphores(int maxSemas)
{
	struct stat statbuf;

	/*
	 * 用数据目录的 inode 号播种搜索空闲信号量
	 * 键。这可尽量减少与其他 postmaster 冲突的
	 * 几率，同时尽量提高检出并清理本目录中
	 * 崩溃 postmaster 遗留信号量的几率。
	 */
	if (stat(DataDir, &statbuf) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not stat data directory \"%s\": %m",
						DataDir)));

	/*
	 * 必须用 ShmemAllocUnlocked()，因保护
	 * ShmemAlloc() 的自旋锁尚未就绪。（用信号量
	 * 仿真自旋锁时此顺序必要。）
	 */
	sharedSemas = (PGSemaphore)
		ShmemAllocUnlocked(PGSemaphoreShmemSize(maxSemas));
	numSharedSemas = 0;
	maxSharedSemas = maxSemas;

	maxSemaSets = (maxSemas + SEMAS_PER_SET - 1) / SEMAS_PER_SET;
	mySemaSets = (IpcSemaphoreId *)
		malloc(maxSemaSets * sizeof(IpcSemaphoreId));
	if (mySemaSets == NULL)
		elog(PANIC, "out of memory");
	numSemaSets = 0;
	nextSemaKey = statbuf.st_ino;
	nextSemaNumber = SEMAS_PER_SET; /* 首次调用强制分配集 */

	on_shmem_exit(ReleaseSemaphores, 0);
}

/*
 * 关机或共享内存重初始化时释放信号量
 *
 * 注：作为 on_shmem_exit 回调，故参数列表古怪
 */
static void
ReleaseSemaphores(int status, Datum arg)
{
	int			i;

	for (i = 0; i < numSemaSets; i++)
		IpcSemaphoreKill(mySemaSets[i]);
	free(mySemaSets);
}

/*
 * PGSemaphoreCreate
 *
 * 分配初始计数为 1 的 PGSemaphore 结构
 */
PGSemaphore
PGSemaphoreCreate(void)
{
	PGSemaphore sema;

	/* 不能在 backend 中做：静态状态属 postmaster */
	Assert(!IsUnderPostmaster);

	if (nextSemaNumber >= SEMAS_PER_SET)
	{
		/* 该分配另一信号量集了 */
		if (numSemaSets >= maxSemaSets)
			elog(PANIC, "too many semaphores created");
		mySemaSets[numSemaSets] = IpcSemaphoreCreate(SEMAS_PER_SET);
		numSemaSets++;
		nextSemaNumber = 0;
	}
	/* 用下一共享 PGSemaphoreData */
	if (numSharedSemas >= maxSharedSemas)
		elog(PANIC, "too many semaphores created");
	sema = &sharedSemas[numSharedSemas++];
	/* 分配当前集中下一空闲信号量 */
	sema->semId = mySemaSets[numSemaSets - 1];
	sema->semNum = nextSemaNumber++;
	/* 初始化为计数 1 */
	IpcSemaphoreInitialize(sema->semId, sema->semNum, 1);

	return sema;
}

/*
 * PGSemaphoreReset
 *
 * 把已初始化的 PGSemaphore 复位为计数 0
 */
void
PGSemaphoreReset(PGSemaphore sema)
{
	IpcSemaphoreInitialize(sema->semId, sema->semNum, 0);
}

/*
 * PGSemaphoreLock
 *
 * 锁定信号量（减计数），若计数将 < 0 则阻塞
 */
void
PGSemaphoreLock(PGSemaphore sema)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = -1;			/* 递减 */
	sops.sem_flg = 0;
	sops.sem_num = sema->semNum;

	/*
	 * 注意：若 errStatus 为 -1 且 errno == EINTR，表示因
	 * 收到信号而过早从操作返回。于是我们
	 * 再次尝试锁定信号量。
	 *
	 * 从前在此检查中断，但那要求从信号处理函数
	 * 中直接处理中断。既难安全又难可移植。
	 *
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
}

/*
 * PGSemaphoreUnlock
 *
 * 解锁信号量（增计数）
 */
void
PGSemaphoreUnlock(PGSemaphore sema)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = 1;			/* 递增 */
	sops.sem_flg = 0;
	sops.sem_num = sema->semNum;

	/*
	 * 注意：若 errStatus 为 -1 且 errno == EINTR，表示因
	 * 收到信号而过早从操作返回。于是我们
	 * 再次尝试解锁信号量。不清楚这是否真会发生，
	 * 但不妨应对。
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
}

/*
 * PGSemaphoreTryLock
 *
 * 仅在能不阻塞时锁定信号量
 */
bool
PGSemaphoreTryLock(PGSemaphore sema)
{
	int			errStatus;
	struct sembuf sops;

	sops.sem_op = -1;			/* 递减 */
	sops.sem_flg = IPC_NOWAIT;	/* 但不阻塞 */
	sops.sem_num = sema->semNum;

	/*
	 * 注意：若 errStatus 为 -1 且 errno == EINTR，表示因
	 * 收到信号而过早从操作返回。于是我们
	 * 再次尝试锁定信号量。
	 */
	do
	{
		errStatus = semop(sema->semId, &sops, 1);
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
	{
		/* 预期 EAGAIN 或 EWOULDBLOCK（平台相关） */
#ifdef EAGAIN
		if (errno == EAGAIN)
			return false;		/* 未能锁定 */
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (errno == EWOULDBLOCK)
			return false;		/* 未能锁定 */
#endif
		/* 否则就麻烦了 */
		elog(FATAL, "semop(id=%d) failed: %m", sema->semId);
	}

	return true;
}
