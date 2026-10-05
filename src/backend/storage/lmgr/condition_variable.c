/*-------------------------------------------------------------------------
 *
 * condition_variable.c
 *	  条件变量的实现。条件变量让一个进程可以
 *	  等到某个特定条件发生，
 *	  而不必知道自己正在等待的那个进程
 *	  的具体身份。对条件变量的等待可以被
 *	  中断，这一点与 LWLock 的等待不同。条件变量可以安全地
 *	  用在动态共享内存段内。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/lmgr/condition_variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/proclist.h"
#include "storage/spin.h"
#include "utils/memutils.h"

/* 一开始，我们还没准备在任何条件变量上睡眠。 */
static ConditionVariable *cv_sleep_target = NULL;

/*
 * 初始化一个条件变量。
 */
void
ConditionVariableInit(ConditionVariable *cv)
{
	SpinLockInit(&cv->mutex);
	proclist_init(&cv->wakeup);
}

/*
 * 准备在给定的条件变量上等待。
 *
 * 进入测试/睡眠循环之前，可以有选择地调用本函数。
 * 若我们至少要睡眠一次，这样做更高效。
 * 不过，若退出条件的第一次测试很可能成功，
 * 省去 ConditionVariablePrepareToSleep 调用会更高效。
 * 详见 ConditionVariableSleep 中的注释。
 *
 * 注意："进入循环之前" 意味着你必须在调用
 * ConditionVariablePrepareToSleep 与调用
 * ConditionVariableSleep 之间测试退出条件。若不方便，就不要调用
 * ConditionVariablePrepareToSleep。
 */
void
ConditionVariablePrepareToSleep(ConditionVariable *cv)
{
	int			pgprocno = MyProc->pgprocno;

	/*
	 * 若已经准备了另一次睡眠，就取消它；这是必要的，
	 * 因为我们只有一个静态变量记录已准备的睡眠，
	 * 而且我们的 PGPROC 里也只有一个 cvWaitLink。这样做没问题，
	 * 因为只要控制权回到另一个测试并睡眠的循环，
	 * 那里的 ConditionVariableSleep 调用就会重新把那次睡眠
	 * 确立为已准备好的那一次。
	 */
	if (cv_sleep_target != NULL)
		ConditionVariableCancelSleep();

	/* 记录我们将在其上睡眠的条件变量。 */
	cv_sleep_target = cv;

	/* 把自己加入等待队列。 */
	SpinLockAcquire(&cv->mutex);
	proclist_push_tail(&cv->wakeup, pgprocno, cvWaitLink);
	SpinLockRelease(&cv->mutex);
}

/*
 * 等待给定的条件变量被发信号。
 *
 * 应当在谓词循环里调用：循环测试某个特定的退出
 * 条件，否则就睡眠，像这样：
 *
 *	 ConditionVariablePrepareToSleep(cv);  // 可选
 *	 while (正在等待的条件不成立)
 *		 ConditionVariableSleep(cv, wait_event_info);
 *	 ConditionVariableCancelSleep();
 *
 * wait_event_info 应当是 pgstat.h 中某个 WaitEventXXX 枚举的
 * 值。它控制等待期间 pg_stat_activity 的
 * wait_event_type 与 wait_event 列的内容。
 */
void
ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info)
{
	(void) ConditionVariableTimedSleep(cv, -1 /* 无超时 */ ,
									   wait_event_info);
}

/*
 * 等待条件变量被发信号，或等到超时。
 *
 * 超时时返回真，否则返回假。
 *
 * 一般用法见 ConditionVariableSleep()。
 */
bool
ConditionVariableTimedSleep(ConditionVariable *cv, long timeout,
							uint32 wait_event_info)
{
	long		cur_timeout = -1;
	instr_time	start_time;
	instr_time	cur_time;
	int			wait_events;

	/*
	 * 若调用者没有显式准备睡眠，现在就准备并
	 * 立即返回。调用者的谓词循环若退出条件尚未满足，
	 * 应当立刻再次调用。这样会在我们第一次睡眠之前
	 * 把退出条件测试两次。多出来的这次
	 * 测试可以先调用 ConditionVariablePrepareToSleep(cv)
	 * 来避免。值不值得这么做，取决于你是否预期
	 * 退出条件一开始就满足：若是，建议跳过准备，
	 * 因为这样可以避免改动等待链表；若一开始
	 * 并不满足，则先准备更好，因为这样可以
	 * 避免多测试一次退出条件。
	 *
	 * 若我们当前准备在另一个条件变量上睡眠，就取消
	 * 那个并准备这一个；见 ConditionVariablePrepareToSleep。
	 */
	if (cv_sleep_target != cv)
	{
		ConditionVariablePrepareToSleep(cv);
		return false;
	}

	/*
	 * 记录当前时间，以便在被虚假唤醒时
	 * 算出剩余的超时。
	 */
	if (timeout >= 0)
	{
		INSTR_TIME_SET_CURRENT(start_time);
		Assert(timeout >= 0 && timeout <= INT_MAX);
		cur_timeout = timeout;
		wait_events = WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH;
	}
	else
		wait_events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;

	while (true)
	{
		bool		done = false;

		/*
		 * 等待锁存器被置位。（若我们因别的
		 * 原因醒来，下面的代码反正也能应付。）
		 */
		(void) WaitLatch(MyLatch, wait_events, cur_timeout, wait_event_info);

		/* 检查等待链表状态之前，先复位锁存器。 */
		ResetLatch(MyLatch);

		/*
		 * 若本进程已被移出等待链表，我们就知道
		 * 它已被 ConditionVariableSignal（或
		 * ConditionVariableBroadcast）发过信号，因此应当返回给调用者。但
		 * 这并不保证退出条件已经满足，只说明我们
		 * 应当去检查它。因此必须把本进程放回等待
		 * 链表，这样调用者检查退出条件期间若再有唤醒，
		 * 我们也不会错过。只有调用者调用
		 * ConditionVariableCancelSleep 时，
		 * 我们才能把自己移出等待链表。
		 *
		 * 若我们仍在等待链表里，则锁存器必定是被
		 * ConditionVariableSignal 以外的东西置位的；虽然我们并不
		 * 保证不会虚假返回，但会避开这个显而易见的情形。
		 */
		SpinLockAcquire(&cv->mutex);
		if (!proclist_contains(&cv->wakeup, MyProc->pgprocno, cvWaitLink))
		{
			done = true;
			proclist_push_tail(&cv->wakeup, MyProc->pgprocno, cvWaitLink);
		}
		SpinLockRelease(&cv->mutex);

		/*
		 * 检查中断；若因此当前睡眠目标变了
		 * （意味着中断处理代码等待了另一个
		 * 条件变量），就虚假返回。
		 */
		CHECK_FOR_INTERRUPTS();
		if (cv != cv_sleep_target)
			done = true;

		/* 我们已被发信号，因此返回 */
		if (done)
			return false;

		/* 若还没结束，就为下一轮迭代更新 cur_timeout */
		if (timeout >= 0)
		{
			INSTR_TIME_SET_CURRENT(cur_time);
			INSTR_TIME_SUBTRACT(cur_time, start_time);
			cur_timeout = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);

			/* 是否已经越过超时阈值？ */
			if (cur_timeout <= 0)
				return true;
		}
	}
}

/*
 * 取消任何尚未完成的睡眠操作。
 *
 * 我们只需把自己从先前已准备睡眠的
 * 任何条件变量的等待队列中移除。
 *
 * 若没有尚未完成的操作，就什么也不做；这样本函数就可以在
 * 事务中止时被调用，以清理任何未完成的条件变量睡眠。
 *
 * 若我们已被发信号，就返回真。
 */
bool
ConditionVariableCancelSleep(void)
{
	ConditionVariable *cv = cv_sleep_target;
	bool		signaled = false;

	if (cv == NULL)
		return false;

	SpinLockAcquire(&cv->mutex);
	if (proclist_contains(&cv->wakeup, MyProc->pgprocno, cvWaitLink))
		proclist_delete(&cv->wakeup, MyProc->pgprocno, cvWaitLink);
	else
		signaled = true;
	SpinLockRelease(&cv->mutex);

	cv_sleep_target = NULL;

	return signaled;
}

/*
 * 唤醒在该条件变量上睡眠最久的进程，若有的话。
 *
 * 注意：很难判断这是否真起了作用：我们知道
 * 自己是否从链表上取下了一个条目，但该条目也许只是一个
 * 哨兵。因此，在提议让本函数返回一个
 * 表明是否唤醒了某人的标志之前，要先想清楚。
 */
void
ConditionVariableSignal(ConditionVariable *cv)
{
	PGPROC	   *proc = NULL;

	/* 从唤醒队列中移除第一个进程（若有）。 */
	SpinLockAcquire(&cv->mutex);
	if (!proclist_is_empty(&cv->wakeup))
		proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
	SpinLockRelease(&cv->mutex);

	/* 若找到了正在睡眠的进程，就置位其锁存器来唤醒它。 */
	if (proc != NULL)
		SetLatch(&proc->procLatch);
}

/*
 * 唤醒在给定条件变量上睡眠的全部进程。
 *
 * 这保证唤醒调用时刻正在该条件变量上睡眠的全部进程，
 * 但在调用进行到一半时才把自己加入链表的进程
 * 通常不会被唤醒。
 */
void
ConditionVariableBroadcast(ConditionVariable *cv)
{
	int			pgprocno = MyProc->pgprocno;
	PGPROC	   *proc = NULL;
	bool		have_sentinel = false;

	/*
	 * 在某些用法里，被唤醒的进程会立刻
	 * 重新入队，这很常见。若只是径直把唤醒链表
	 * 减到空，就会和这样的进程陷入可能无休止的循环。
	 * 我们真正要的语义，只是确保已经
	 * 唤醒了进入函数时链表上的全部进程。我们可以把自己的
	 * cvWaitLink 当作哨兵，用来判断何时已经做完。
	 *
	 * 这种做法看似有个缺陷：别人可能对该条件变量
	 * 发信号，并因此移除我们的哨兵条目。但这没问题：由于
	 * 条件变量的等待者总是按顺序加入和移除，那就意味着
	 * 先前的每个等待者都已被唤醒，所以我们已经做完。我们的锁存器
	 * 会因别人那次信号而被额外 "置位" 一次，这
	 * 略微低效，但无害。
	 *
	 * 若我们的 cvWaitLink 已经在
	 * 别的 proclist 里使用，就不能再把它当作哨兵插入。虽然典型
	 * 用法并不预期如此，只要取消任何
	 * 已准备的条件变量睡眠即可处理。下一次 ConditionVariableSleep 调用会
	 * 负责把丢失的状态重新建立起来。
	 */
	if (cv_sleep_target != NULL)
		ConditionVariableCancelSleep();

	/*
	 * 查看队列状态。若为空，就无事可做。
	 * 若恰好只有一个条目，只需移除该条目并向它
	 * 发信号。否则，移除第一个条目，并插入我们的哨兵。
	 */
	SpinLockAcquire(&cv->mutex);
	/* 既然到了这里，就断言我们不在链表中。 */
	Assert(!proclist_contains(&cv->wakeup, pgprocno, cvWaitLink));

	if (!proclist_is_empty(&cv->wakeup))
	{
		proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
		if (!proclist_is_empty(&cv->wakeup))
		{
			proclist_push_tail(&cv->wakeup, pgprocno, cvWaitLink);
			have_sentinel = true;
		}
	}
	SpinLockRelease(&cv->mutex);

	/* 若有等待者，就唤醒第一个。 */
	if (proc != NULL)
		SetLatch(&proc->procLatch);

	while (have_sentinel)
	{
		/*
		 * 循环每走一轮，就取下唤醒链表的第一个条目，并且
		 * 向它发信号，除非它是我们的哨兵。只要哨兵
		 * 还留在链表里，就继续重复。
		 *
		 * 注意：若别人移除了我们的哨兵，退出前我们会再多唤醒一个
		 * 进程。这是有意的，因为若
		 * 别人对该条件变量发信号，他们可能是想唤醒某个
		 * 在我们加入哨兵之后才把自己加入链表的第三个进程。
		 * 宁可给出一次虚假唤醒（除了浪费一些周期，
		 * 应当无害），也不要漏掉一次唤醒。
		 */
		proc = NULL;
		SpinLockAcquire(&cv->mutex);
		if (!proclist_is_empty(&cv->wakeup))
			proc = proclist_pop_head_node(&cv->wakeup, cvWaitLink);
		have_sentinel = proclist_contains(&cv->wakeup, pgprocno, cvWaitLink);
		SpinLockRelease(&cv->mutex);

		if (proc != NULL && proc != MyProc)
			SetLatch(&proc->procLatch);
	}
}
