/*-------------------------------------------------------------------------
 *
 * atomics.c
 *	   原子操作实现中的非内联部分
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/port/atomics.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/spin.h"

#ifdef PG_HAVE_MEMORY_BARRIER_EMULATION
#ifdef WIN32
#error "barriers are required (and provided) on WIN32 platforms"
#endif
#include <signal.h>
#endif

#ifdef PG_HAVE_MEMORY_BARRIER_EMULATION
void
pg_spinlock_barrier(void)
{
	/*
	 * 注意：须可重入，因部分屏障放在信号处理函数。
	 *
	 *
	 * 回退屏障用 kill(0)：假定仍需回退屏障支持的
	 * 老系统内核，在检查 postmaster 进程号是否存在
	 * 时会插入适当的内存屏障。
	 */
	(void) kill(PostmasterPid, 0);
}
#endif

#ifdef PG_HAVE_COMPILER_BARRIER_EMULATION
void
pg_extern_compiler_barrier(void)
{
	/* 无操作 */
}
#endif


#ifdef PG_HAVE_ATOMIC_FLAG_SIMULATION

void
pg_atomic_init_flag_impl(volatile pg_atomic_flag *ptr)
{
	StaticAssertDecl(sizeof(ptr->sema) >= sizeof(slock_t),
					 "size mismatch of atomic_flag vs slock_t");

#ifndef HAVE_SPINLOCKS

	/*
	 * 注意：若用基于信号量的 TAS 仿真，须使用另一
	 * 组信号量。否则在持有自旋锁时操纵原子变量
	 * 会出问题。
	 */
	s_init_lock_sema((slock_t *) &ptr->sema, true);
#else
	SpinLockInit((slock_t *) &ptr->sema);
#endif

	ptr->value = false;
}

bool
pg_atomic_test_set_flag_impl(volatile pg_atomic_flag *ptr)
{
	uint32		oldval;

	SpinLockAcquire((slock_t *) &ptr->sema);
	oldval = ptr->value;
	ptr->value = true;
	SpinLockRelease((slock_t *) &ptr->sema);

	return oldval == 0;
}

void
pg_atomic_clear_flag_impl(volatile pg_atomic_flag *ptr)
{
	SpinLockAcquire((slock_t *) &ptr->sema);
	ptr->value = false;
	SpinLockRelease((slock_t *) &ptr->sema);
}

bool
pg_atomic_unlocked_test_flag_impl(volatile pg_atomic_flag *ptr)
{
	return ptr->value == 0;
}

#endif							/* PG_HAVE_ATOMIC_FLAG_SIMULATION */

#ifdef PG_HAVE_ATOMIC_U32_SIMULATION
void
pg_atomic_init_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val_)
{
	StaticAssertDecl(sizeof(ptr->sema) >= sizeof(slock_t),
					 "size mismatch of atomic_uint32 vs slock_t");

	/*
	 * 若用基于信号量的原子标志，须当心在持有
	 * 自旋锁时嵌套使用原子操作。
	 */
#ifndef HAVE_SPINLOCKS
	s_init_lock_sema((slock_t *) &ptr->sema, true);
#else
	SpinLockInit((slock_t *) &ptr->sema);
#endif
	ptr->value = val_;
}

void
pg_atomic_write_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val)
{
	/*
	 * 或许以为无锁写不必获取自旋锁，但那就错了。
	 * 即便无锁写也须使并发的
	 * pg_atomic_compare_exchange_u32()（等）失败。
	 */
	SpinLockAcquire((slock_t *) &ptr->sema);
	ptr->value = val;
	SpinLockRelease((slock_t *) &ptr->sema);
}

bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool		ret;

	/*
	 * 在自旋锁下做原子操作。看似锁不可用时可跳过
	 * cmpxchg，但那只是仿真"弱"比较交换，即允许
	 * 伪失败。若干算法依赖强变体，且该变体在多数主流
	 * 架构上可高效实现，故此处也按强变体
	 * 仿真。
	 * well.
	 */
	SpinLockAcquire((slock_t *) &ptr->sema);

	/* 执行比较/交换逻辑 */
	ret = ptr->value == *expected;
	*expected = ptr->value;
	if (ret)
		ptr->value = newval;

	/* 并释放锁 */
	SpinLockRelease((slock_t *) &ptr->sema);

	return ret;
}

uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	uint32		oldval;

	SpinLockAcquire((slock_t *) &ptr->sema);
	oldval = ptr->value;
	ptr->value += add_;
	SpinLockRelease((slock_t *) &ptr->sema);
	return oldval;
}

#endif							/* PG_HAVE_ATOMIC_U32_SIMULATION */


#ifdef PG_HAVE_ATOMIC_U64_SIMULATION

void
pg_atomic_init_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val_)
{
	StaticAssertDecl(sizeof(ptr->sema) >= sizeof(slock_t),
					 "size mismatch of atomic_uint64 vs slock_t");

	/*
	 * 若用基于信号量的原子标志，须当心在持有
	 * 自旋锁时嵌套使用原子操作。
	 */
#ifndef HAVE_SPINLOCKS
	s_init_lock_sema((slock_t *) &ptr->sema, true);
#else
	SpinLockInit((slock_t *) &ptr->sema);
#endif
	ptr->value = val_;
}

bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool		ret;

	/*
	 * 在自旋锁下做原子操作。看似锁不可用时可跳过
	 * cmpxchg，但那只是仿真"弱"比较交换，即允许
	 * 伪失败。若干算法依赖强变体，且该变体在多数主流
	 * 架构上可高效实现，故此处也按强变体
	 * 仿真。
	 * well.
	 */
	SpinLockAcquire((slock_t *) &ptr->sema);

	/* 执行比较/交换逻辑 */
	ret = ptr->value == *expected;
	*expected = ptr->value;
	if (ret)
		ptr->value = newval;

	/* 并释放锁 */
	SpinLockRelease((slock_t *) &ptr->sema);

	return ret;
}

uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	uint64		oldval;

	SpinLockAcquire((slock_t *) &ptr->sema);
	oldval = ptr->value;
	ptr->value += add_;
	SpinLockRelease((slock_t *) &ptr->sema);
	return oldval;
}

#endif							/* PG_HAVE_ATOMIC_U64_SIMULATION */
