/*-------------------------------------------------------------------------
 *
 * barrier.h
 *	  用于同步协作进程的屏障。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/barrier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BARRIER_H
#define BARRIER_H

/*
 * 先前称为 "barrier.h" 的头请改含 "port/atomics.h"，
 * 其中处理原子、编译器屏障与内存
 * 屏障。
 */

#include "storage/condition_variable.h"
#include "storage/spin.h"

typedef struct Barrier
{
	slock_t		mutex;
	int			phase;			/* 阶段计数 */
	int			participants;	/* 已附着的参与者数目 */
	int			arrived;		/* 已到达的参与者
								 * 数目 */
	int			elected;		/* 已选最高阶段 */
	bool		static_party;	/* 仅用于断言 */
	ConditionVariable condition_variable;
} Barrier;

extern void BarrierInit(Barrier *barrier, int participants);
extern bool BarrierArriveAndWait(Barrier *barrier, uint32 wait_event_info);
extern bool BarrierArriveAndDetach(Barrier *barrier);
extern bool BarrierArriveAndDetachExceptLast(Barrier *barrier);
extern int	BarrierAttach(Barrier *barrier);
extern bool BarrierDetach(Barrier *barrier);
extern int	BarrierPhase(Barrier *barrier);
extern int	BarrierParticipants(Barrier *barrier);

#endif							/* BARRIER_H */
