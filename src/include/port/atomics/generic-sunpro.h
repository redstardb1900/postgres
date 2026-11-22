/*-------------------------------------------------------------------------
 *
 * generic-sunpro.h
 *	  Atomic operations for solaris' CC
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * Documentation:
 * * manpage for atomic_cas(3C)
 *   http://www.unix.com/man-page/opensolaris/3c/atomic_cas/
 *   http://docs.oracle.com/cd/E23824_01/html/821-1465/atomic-cas-3c.html
 *
 * src/include/port/atomics/generic-sunpro.h
 *
 * -------------------------------------------------------------------------
 */

#if defined(HAVE_ATOMICS)

#ifdef HAVE_MBARRIER_H
#include <mbarrier.h>

#define pg_compiler_barrier_impl()	__compiler_barrier()

#ifndef pg_memory_barrier_impl
/*
 * 尽管名称如此,实为完整屏障。展开为 mfence/
 * membar #StoreStore|#LoadStore|#StoreLoad|#LoadLoad(x86/
 * sparc)。
 */
#	define pg_memory_barrier_impl()		__machine_rw_barrier()
#endif
#ifndef pg_read_barrier_impl
#	define pg_read_barrier_impl()		__machine_r_barrier()
#endif
#ifndef pg_write_barrier_impl
#	define pg_write_barrier_impl()		__machine_w_barrier()
#endif

#endif /* HAVE_MBARRIER_H */

/*  较旧编译器没有 atomic.h...  */
#ifdef HAVE_ATOMIC_H

#include <atomic.h>

#define PG_HAVE_ATOMIC_U32_SUPPORT
typedef struct pg_atomic_uint32
{
	volatile uint32 value;
} pg_atomic_uint32;

#define PG_HAVE_ATOMIC_U64_SUPPORT
typedef struct pg_atomic_uint64
{
	/*
	 * 强制变量对齐语法应为支持 atomic.h 的版本所
	 * 支持,但难找准确文档。若成问题,须为 64
	 * 位支持加更多版本
	 * 检查。
	 */
	volatile uint64 value pg_attribute_aligned(8);
} pg_atomic_uint64;

#endif /* HAVE_ATOMIC_H */

#endif /* defined(HAVE_ATOMICS) */


#if defined(HAVE_ATOMICS)

#ifdef HAVE_ATOMIC_H

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U32
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	bool	ret;
	uint32	current;

	current = atomic_cas_32(&ptr->value, *expected, newval);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#define PG_HAVE_ATOMIC_COMPARE_EXCHANGE_U64
static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	bool	ret;
	uint64	current;

	current = atomic_cas_64(&ptr->value, *expected, newval);
	ret = current == *expected;
	*expected = current;
	return ret;
}

#endif /* HAVE_ATOMIC_H */

#endif /* defined(HAVE_ATOMICS) */
