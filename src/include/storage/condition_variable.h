/*-------------------------------------------------------------------------
 *
 * condition_variable.h
 *	  条件变量。
 *
 * 条件变量是等待某条件变为真的方法。惯例上，条件
 * 变量支持三种操作：(1) 睡眠；(2) 信号，唤醒一个
 * 在该条件变量上睡眠的进程；(3) 广播，唤醒所有
 * 在该条件变量上睡眠的进程。本实现中，条件变量
 * 使进程进入可中断睡眠（故可在条件满足前被
 * 取消），并且内部不使用指针（因而它们
 * 可安全地用于
 * DSMs 内）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/condition_variable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONDITION_VARIABLE_H
#define CONDITION_VARIABLE_H

#include "storage/proclist_types.h"
#include "storage/spin.h"

typedef struct
{
	slock_t		mutex;			/* 保护唤醒链表的自旋锁 */
	proclist_head wakeup;		/* 可唤醒进程链表 */
} ConditionVariable;

/*
 * 把条件变量填充到 2 的幂大小，使条件变量数组
 * 不跨越缓存行边界。
 */
#define CV_MINIMAL_SIZE		(sizeof(ConditionVariable) <= 16 ? 16 : 32)
typedef union ConditionVariableMinimallyPadded
{
	ConditionVariable cv;
	char		pad[CV_MINIMAL_SIZE];
} ConditionVariableMinimallyPadded;

/* 初始化条件变量。 */
extern void ConditionVariableInit(ConditionVariable *cv);

/*
 * 要在条件变量上睡眠，进程应使用循环：先检查条件，
 * 若已满足则退出循环，然后调用
 * ConditionVariableSleep。可能有伪唤醒，但应不
 * 频繁。退出循环后，必须调用 ConditionVariableCancelSleep
 * 以确保进程不再位于该条件变量的等待
 * 链表中。
 */
extern void ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info);
extern bool ConditionVariableTimedSleep(ConditionVariable *cv, long timeout,
										uint32 wait_event_info);
extern bool ConditionVariableCancelSleep(void);

/*
 * 也可在进入上述测试并睡眠循环之前调用
 * ConditionVariablePrepareToSleep。若至少要睡一次则更
 * 高效；若第一次测试条件已为真而无需睡眠则
 * 不调用更高效。
 */
extern void ConditionVariablePrepareToSleep(ConditionVariable *cv);

/* 唤醒一名等待者（signal）或全部（broadcast）。 */
extern void ConditionVariableSignal(ConditionVariable *cv);
extern void ConditionVariableBroadcast(ConditionVariable *cv);

#endif							/* CONDITION_VARIABLE_H */
