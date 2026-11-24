/*-------------------------------------------------------------------------
 *
 * posix_sema.c
 *	  用 POSIX 信号量设施实现 PGSemaphore
 *
 * 我们更倾向无名 POSIX 信号量（用 sem_init
 * 创建的那种）。也可用 sem_open 创建的那种。
 *
 * 无论哪种实现，typedef PGSemaphore 都等价于 "sem_t *"。
 * 无名信号量时，sem_t 结构位于共享内存中的
 * 数组。有名信号量则不然，因无法让 sem_open
 * 在那里分配。因此有名信号量代码
 * *不支持 EXEC_BACKEND*。sem_t 结构仅在
 * postmaster 私有内存中，可被 fork 的 backend
 * 成功继承，但 exec 的 backend 无法访问。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/posix_sema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/shmem.h"


/* 见文件头注释 */
#if defined(USE_NAMED_POSIX_SEMAPHORES) && defined(EXEC_BACKEND)
#error cannot use named POSIX semaphores with EXEC_BACKEND
#endif

typedef union SemTPadded
{
	sem_t		pgsem;
	char		pad[PG_CACHE_LINE_SIZE];
} SemTPadded;

/* typedef PGSemaphore 等价于指向 sem_t 的指针 */
typedef struct PGSemaphoreData
{
	SemTPadded	sem_padded;
} PGSemaphoreData;

#define PG_SEM_REF(x)	(&(x)->sem_padded.pgsem)

#define IPCProtection	(0600)	/* 仅由用户访问/修改 */

#ifdef USE_NAMED_POSIX_SEMAPHORES
static sem_t **mySemPointers;	/* 跟踪已创建的信号量 */
#else
static PGSemaphore sharedSemas; /* 共享内存中的 PGSemaphoreData 数组 */
#endif
static int	numSems;			/* 迄今已获取的信号量数 */
static int	maxSems;			/* 上述数组的已分配大小 */
static int	nextSemKey;			/* 下一待试名 */


static void ReleaseSemaphores(int status, Datum arg);


#ifdef USE_NAMED_POSIX_SEMAPHORES

/*
 * PosixSemaphoreCreate
 *
 * 尝试创建新的有名信号量。
 *
 * 若失败且失败码不是与已有信号量冲突，
 * 则打印错误并中止。其他错误暗示不可恢复的
 * problems.
 */
static sem_t *
PosixSemaphoreCreate(void)
{
	int			semKey;
	char		semname[64];
	sem_t	   *mySem;

	for (;;)
	{
		semKey = nextSemKey++;

		snprintf(semname, sizeof(semname), "/pgsql-%d", semKey);

		mySem = sem_open(semname, O_CREAT | O_EXCL,
						 (mode_t) IPCProtection, (unsigned) 1);

#ifdef SEM_FAILED
		if (mySem != (sem_t *) SEM_FAILED)
			break;
#else
		if (mySem != (sem_t *) (-1))
			break;
#endif

		/* 若错误表示冲突则循环 */
		if (errno == EEXIST || errno == EACCES || errno == EINTR)
			continue;

		/*
		 * 否则抱怨并中止
		 */
		elog(FATAL, "sem_open(\"%s\") failed: %m", semname);
	}

	/*
	 * 立即 unlink 信号量，使其无法被外部访问。
	 * 这也确保崩溃时它会消失。
	 */
	sem_unlink(semname);

	return mySem;
}
#else							/* !USE_NAMED_POSIX_SEMAPHORES */

/*
 * PosixSemaphoreCreate
 *
 * 尝试创建新的无名信号量。
 */
static void
PosixSemaphoreCreate(sem_t *sem)
{
	if (sem_init(sem, 1, 1) < 0)
		elog(FATAL, "sem_init failed: %m");
}
#endif							/* USE_NAMED_POSIX_SEMAPHORES */


/*
 * PosixSemaphoreKill	- 移除信号量
 */
static void
PosixSemaphoreKill(sem_t *sem)
{
#ifdef USE_NAMED_POSIX_SEMAPHORES
	/* 有名信号量须用 sem_close */
	if (sem_close(sem) < 0)
		elog(LOG, "sem_close failed: %m");
#else
	/* 无名信号量须用 sem_destroy */
	if (sem_destroy(sem) < 0)
		elog(LOG, "sem_destroy failed: %m");
#endif
}


/*
 * 报告信号量所需的共享内存空间
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
#ifdef USE_NAMED_POSIX_SEMAPHORES
	/* 此种情况不需要共享内存 */
	return 0;
#else
	/* 每信号量一 PGSemaphoreData */
	return mul_size(maxSemas, sizeof(PGSemaphoreData));
#endif
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
 * Posix 实现按需获取信号量；maxSemas 参数
 * 仅用于确定数组大小。无名信号量时，在共享
 * 内存中有 PGSemaphoreData 结构数组。
 * 有名信号量则保留 postmaster 本地的 sem_t 指针
 * 数组，用于完成后释放信号量。
 * 此设计尽量减少 postmaster 关机对共享内存
 * 内容的依赖，因失败的 backend 可能已破坏它。
 * sem_destroy() 崩溃的可能我们无能为力，但
 * 不必把计数器暴露给其他进程。
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

#ifdef USE_NAMED_POSIX_SEMAPHORES
	mySemPointers = (sem_t **) malloc(maxSemas * sizeof(sem_t *));
	if (mySemPointers == NULL)
		elog(PANIC, "out of memory");
#else

	/*
	 * 必须用 ShmemAllocUnlocked()，因保护
	 * ShmemAlloc() 的自旋锁尚未就绪。（用信号量
	 * 仿真自旋锁时此顺序必要。）
	 */
	sharedSemas = (PGSemaphore)
		ShmemAllocUnlocked(PGSemaphoreShmemSize(maxSemas));
#endif

	numSems = 0;
	maxSems = maxSemas;
	nextSemKey = statbuf.st_ino;

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

#ifdef USE_NAMED_POSIX_SEMAPHORES
	for (i = 0; i < numSems; i++)
		PosixSemaphoreKill(mySemPointers[i]);
	free(mySemPointers);
#endif

#ifdef USE_UNNAMED_POSIX_SEMAPHORES
	for (i = 0; i < numSems; i++)
		PosixSemaphoreKill(PG_SEM_REF(sharedSemas + i));
#endif
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
	sem_t	   *newsem;

	/* 不能在 backend 中做：静态状态属 postmaster */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

#ifdef USE_NAMED_POSIX_SEMAPHORES
	newsem = PosixSemaphoreCreate();
	/* 记住新信号量供 ReleaseSemaphores */
	mySemPointers[numSems] = newsem;
	sema = (PGSemaphore) newsem;
#else
	sema = &sharedSemas[numSems];
	newsem = PG_SEM_REF(sema);
	PosixSemaphoreCreate(newsem);
#endif

	numSems++;

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
	/*
	 * POSIX 无直接 API，只好反复 trywait 把
	 * 信号量递减到 0。
	 */
	for (;;)
	{
		if (sem_trywait(PG_SEM_REF(sema)) < 0)
		{
			if (errno == EAGAIN || errno == EDEADLK)
				break;			/* 已减到 0 */
			if (errno == EINTR)
				continue;		/* 会发生吗？ */
			elog(FATAL, "sem_trywait failed: %m");
		}
	}
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

	/* 见 sysv_sema.c 中 PGSemaphoreLock 实现的说明。 */
	do
	{
		errStatus = sem_wait(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "sem_wait failed: %m");
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

	/*
	 * 注意：若 errStatus 为 -1 且 errno == EINTR，表示因
	 * 收到信号而过早从操作返回。于是我们
	 * 再次尝试解锁信号量。不清楚这是否真会发生，
	 * 但不妨应对。
	 */
	do
	{
		errStatus = sem_post(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
		elog(FATAL, "sem_post failed: %m");
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

	/*
	 * 注意：若 errStatus 为 -1 且 errno == EINTR，表示因
	 * 收到信号而过早从操作返回。于是我们
	 * 再次尝试锁定信号量。
	 */
	do
	{
		errStatus = sem_trywait(PG_SEM_REF(sema));
	} while (errStatus < 0 && errno == EINTR);

	if (errStatus < 0)
	{
		if (errno == EAGAIN || errno == EDEADLK)
			return false;		/* 未能锁定 */
		/* 否则就麻烦了 */
		elog(FATAL, "sem_trywait failed: %m");
	}

	return true;
}
