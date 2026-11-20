/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   自旋锁的硬件无关实现。
 *
 *
 *	自旋锁的硬件无关接口由
 *	typedef "slock_t" 与这些宏：
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		初始化自旋锁（至未锁定状态）。
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		获取自旋锁，必要时等待。
 *		若无法获取锁则超时并 abort()，时限为
 *		"合理"时长 —— 通常约 1 分钟。
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		解锁先前已获取的锁。
 *
 *	bool SpinLockFree(slock_t *lock)
 *		测试锁是否空闲。空闲则 true，已锁则 false。
 *		这*不*改变锁的状态。
 *
 *	调用者必须注意宏参数可能被求值多
 *	次！
 *
 *	调用代码中的装入与存储操作保证不会相对
 *	这些操作被重排，因为它们含有
 *	编译器屏障。（PostgreSQL 9.5 之前，调用者需用
 *	volatile 限定符访问自旋锁保护的数据。）
 *
 *	须记住编码规则：自旋锁切勿持有超过
 *	少数几条指令。尤其假定持有自旋锁期间
 *	不可能发生 CHECK_FOR_INTERRUPTS()，因而
 *	这些宏中不必做 HOLD/RESUME_INTERRUPTS()。
 *
 *	这些宏用 s_lock.h 提供的硬件相关宏来
 *	实现。本头文件当前未加入额外功能，
 *	但过去有过，将来某天可能
 *	再有。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/spin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/s_lock.h"
#ifndef HAVE_SPINLOCKS
#include "storage/pg_sema.h"
#endif


#define SpinLockInit(lock)	S_INIT_LOCK(lock)

#define SpinLockAcquire(lock) S_LOCK(lock)

#define SpinLockRelease(lock) S_UNLOCK(lock)

#define SpinLockFree(lock)	S_LOCK_FREE(lock)


extern int	SpinlockSemas(void);
extern Size SpinlockSemaSize(void);

#ifndef HAVE_SPINLOCKS
extern void SpinlockSemaInit(void);
extern PGDLLIMPORT PGSemaphore *SpinlockSemaArray;
#endif

#endif							/* SPIN_H */
