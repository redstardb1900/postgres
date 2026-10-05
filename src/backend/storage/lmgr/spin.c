/*-------------------------------------------------------------------------
 *
 * spin.c
 *	   与硬件无关的自旋锁实现。
 *
 *
 * 对于具备测试并设置（TAS）指令的机器，由 s_lock.h/.c
 * 定义自旋锁的实现。本文件只包含借助 PGSemaphores 的
 * 自旋锁桩实现。除非信号量的实现
 * 不涉及内核调用，否则这种做法
 * 慢得实在没什么用 :-(
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/spin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/pg_sema.h"
#include "storage/shmem.h"
#include "storage/spin.h"


#ifndef HAVE_SPINLOCKS

/*
 * 没有 TAS，因此自旋锁实现为 PGSemaphores。
 */

#ifndef HAVE_ATOMICS
#define NUM_EMULATION_SEMAPHORES (NUM_SPINLOCK_SEMAPHORES + NUM_ATOMICS_SEMAPHORES)
#else
#define NUM_EMULATION_SEMAPHORES (NUM_SPINLOCK_SEMAPHORES)
#endif							/* HAVE_ATOMICS（原子操作） */

PGSemaphore *SpinlockSemaArray;

#else							/* !HAVE_SPINLOCKS（无自旋锁） */

#define NUM_EMULATION_SEMAPHORES 0

#endif							/* HAVE_SPINLOCKS（有自旋锁） */

/*
 * 报告为支持自旋锁而存放信号量所需的
 * 共享内存大小。
 */
Size
SpinlockSemaSize(void)
{
	return NUM_EMULATION_SEMAPHORES * sizeof(PGSemaphore);
}

/*
 * 报告支持自旋锁所需的信号量个数。
 */
int
SpinlockSemas(void)
{
	return NUM_EMULATION_SEMAPHORES;
}

#ifndef HAVE_SPINLOCKS

/*
 * 初始化自旋锁模拟。
 *
 * 必须在 PGReserveSemaphores() 之后调用。
 */
void
SpinlockSemaInit(void)
{
	PGSemaphore *spinsemas;
	int			nsemas = SpinlockSemas();
	int			i;

	/*
	 * 必须使用 ShmemAllocUnlocked()，因为保护
	 * ShmemAlloc() 的那把自旋锁显然还不可能就绪。
	 */
	spinsemas = (PGSemaphore *) ShmemAllocUnlocked(SpinlockSemaSize());
	for (i = 0; i < nsemas; ++i)
		spinsemas[i] = PGSemaphoreCreate();
	SpinlockSemaArray = spinsemas;
}

/*
 * s_lock.h 用信号量来模拟硬件自旋锁
 *
 * 我们把全部自旋锁映射到 NUM_EMULATION_SEMAPHORES 个信号量上。把
 * 多把自旋锁映射到同一个信号量上是可以的，因为任何进程都不应当
 * 同时持有多于一把。只要信号量够多，就不至于
 * 由此增加太多额外竞争。
 *
 * 一次只持有一把自旋锁，这一限制有一个例外：
 * 模拟的原子操作嵌套在
 * 自旋锁内部是允许的。为避免自旋锁与原子操作共用同一个信号量，
 * 我们确保 "普通" 自旋锁和由自旋锁支撑的原子操作使用
 * 彼此不同的信号量（见 s_init_lock_sema 的 nested 参数）。
 *
 * 在本实现里 slock_t 只是一个 int；它保存自旋锁
 * 编号，范围是 1..NUM_EMULATION_SEMAPHORES。我们有意保证 0
 * 不是有效值，这样用这段代码来测试，就有助于发现
 * 自旋锁没有初始化的问题。
 */

static inline void
s_check_valid(int lockndx)
{
	if (unlikely(lockndx <= 0 || lockndx > NUM_EMULATION_SEMAPHORES))
		elog(ERROR, "invalid spinlock number: %d", lockndx);
}

void
s_init_lock_sema(volatile slock_t *lock, bool nested)
{
	static uint32 counter = 0;
	uint32		offset;
	uint32		sema_total;
	uint32		idx;

	if (nested)
	{
		/*
		 * 为了让原子操作能嵌套在持有自旋锁的区段里，要使用
		 * 另一把自旋锁。见上面的注释。
		 */
		offset = 1 + NUM_SPINLOCK_SEMAPHORES;
		sema_total = NUM_ATOMICS_SEMAPHORES;
	}
	else
	{
		offset = 1;
		sema_total = NUM_SPINLOCK_SEMAPHORES;
	}

	idx = (counter++ % sema_total) + offset;

	/* 再核对一次我们是否做对了 */
	s_check_valid(idx);

	*lock = idx;
}

void
s_unlock_sema(volatile slock_t *lock)
{
	int			lockndx = *lock;

	s_check_valid(lockndx);

	PGSemaphoreUnlock(SpinlockSemaArray[lockndx - 1]);
}

bool
s_lock_free_sema(volatile slock_t *lock)
{
	/* 反正我们目前并不使用 S_LOCK_FREE */
	elog(ERROR, "spin.c does not support S_LOCK_FREE()");
	return false;
}

int
tas_sema(volatile slock_t *lock)
{
	int			lockndx = *lock;

	s_check_valid(lockndx);

	/* 注意：TAS 宏在成功时返回 0 */
	return !PGSemaphoreTryLock(SpinlockSemaArray[lockndx - 1]);
}

#endif							/* !HAVE_SPINLOCKS（无自旋锁） */
