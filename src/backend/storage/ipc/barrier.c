/*-------------------------------------------------------------------------
 *
 * barrier.c
 *	  Barriers for synchronizing cooperating processes.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * From Wikipedia[1]: "In parallel computing, a barrier is a type of
 * synchronization method.  A barrier for a group of threads or processes in
 * the source code means any thread/process must stop at this point and cannot
 * proceed until all other threads/processes reach this barrier."
 *
 * This implementation of barriers allows for static sets of participants
 * known up front, or dynamic sets of participants which processes can join or
 * leave at any time.  In the dynamic case, a phase number can be used to
 * track progress through a parallel algorithm, and may be necessary to
 * synchronize with the current phase of a multi-phase algorithm when a new
 * participant joins.  In the static case, the phase number is used
 * internally, but it isn't strictly necessary for client code to access it
 * because the phase can only advance when the declared number of participants
 * reaches the barrier, so client code should be in no doubt about the current
 * phase of computation at all times.
 *
 * Consider a parallel algorithm that involves separate phases of computation
 * A, B and C where the output of each phase is needed before the next phase
 * can begin.
 *
 * In the case of a static barrier initialized with 4 participants, each
 * participant works on phase A, then calls BarrierArriveAndWait to wait until
 * all 4 participants have reached that point.  When BarrierArriveAndWait
 * returns control, each participant can work on B, and so on.  Because the
 * barrier knows how many participants to expect, the phases of computation
 * don't need labels or numbers, since each process's program counter implies
 * the current phase.  Even if some of the processes are slow to start up and
 * begin running phase A, the other participants are expecting them and will
 * patiently wait at the barrier.  The code could be written as follows:
 *
 *     perform_a();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_b();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_c();
 *     BarrierArriveAndWait(&barrier, ...);
 *
 * If the number of participants is not known up front, then a dynamic barrier
 * is needed and the number should be set to zero at initialization.  New
 * complications arise because the number necessarily changes over time as
 * participants attach and detach, and therefore phases B, C or even the end
 * of processing may be reached before any given participant has started
 * running and attached.  Therefore the client code must perform an initial
 * test of the phase number after attaching, because it needs to find out
 * which phase of the algorithm has been reached by any participants that are
 * already attached in order to synchronize with that work.  Once the program
 * counter or some other representation of current progress is synchronized
 * with the barrier's phase, normal control flow can be used just as in the
 * static case.  Our example could be written using a switch statement with
 * cases that fall-through, as follows:
 *
 *     phase = BarrierAttach(&barrier);
 *     switch (phase)
 *     {
 *     case PHASE_A:
 *         perform_a();
 *         BarrierArriveAndWait(&barrier, ...);
 *     case PHASE_B:
 *         perform_b();
 *         BarrierArriveAndWait(&barrier, ...);
 *     case PHASE_C:
 *         perform_c();
 *         BarrierArriveAndWait(&barrier, ...);
 *     }
 *     BarrierDetach(&barrier);
 *
 * Static barriers behave similarly to POSIX's pthread_barrier_t.  Dynamic
 * barriers behave similarly to Java's java.util.concurrent.Phaser.
 *
 * [1] https://en.wikipedia.org/wiki/Barrier_(computer_science)
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/barrier.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "storage/barrier.h"

static inline bool BarrierDetachImpl(Barrier *barrier, bool arrive);

/*
 * 初始化此屏障。若使用静态参与者数量，给出每个阶段
 * 要等待的参与者数，表示该数目的后端已隐式挂接。
 * 若使用动态参与者数量，这里指定零，然后用
 * BarrierAttach() 与
 * BarrierDetach()/BarrierArriveAndDetach() 显式登记和
 * 注销参与者。
 */
void
BarrierInit(Barrier *barrier, int participants)
{
	SpinLockInit(&barrier->mutex);
	barrier->participants = participants;
	barrier->arrived = 0;
	barrier->phase = 0;
	barrier->elected = 0;
	barrier->static_party = participants > 0;
	ConditionVariableInit(&barrier->condition_variable);
}

/*
 * 到达此屏障，等待所有其他已挂接的参与者也到达，
 * 然后返回。当前阶段加一。调用方必须已经
 * 挂接。
 *
 * 等待期间，pg_stat_activity 显示的 wait_event_type 与 wait_event
 * 由传入的 wait_event_info 控制，该值应取自
 * pgstat.h 里的某个 WaitEventXXX 枚举。
 *
 * 在任意选出的一个参与者中返回真，在其余所有
 * 参与者中返回假。返回值可用来选出一个参与者，执行
 * 必须串行完成的一阶段工作，其余参与者则等待。
 */
bool
BarrierArriveAndWait(Barrier *barrier, uint32 wait_event_info)
{
	bool		release = false;
	bool		elected;
	int			start_phase;
	int			next_phase;

	SpinLockAcquire(&barrier->mutex);
	start_phase = barrier->phase;
	next_phase = start_phase + 1;
	++barrier->arrived;
	if (barrier->arrived == barrier->participants)
	{
		release = true;
		barrier->arrived = 0;
		barrier->phase = next_phase;
		barrier->elected = next_phase;
	}
	SpinLockRelease(&barrier->mutex);

	/*
	 * 若我们是最后一个到达的预期参与者，就可以放开
	 * 同伴并返回真，表示此后端已被选出，
	 * 去执行任何串行工作。
	 */
	if (release)
	{
		ConditionVariableBroadcast(&barrier->condition_variable);

		return true;
	}

	/*
	 * 否则必须等待最后一个参与者到达，
	 * 并推进阶段。
	 */
	elected = false;
	ConditionVariablePrepareToSleep(&barrier->condition_variable);
	for (;;)
	{
		/*
		 * 我们知道 phase 要么是 start_phase，表示我们
		 * 还要继续等待，要么是 next_phase，表示我们正在
		 * 等待的最后一个参与者已经到达或已脱离，
		 * 因而下一阶段已经开始。没有此后端参与，
		 * 阶段无法再向前推进，因为
		 * 此后端处于挂接状态。
		 */
		SpinLockAcquire(&barrier->mutex);
		Assert(barrier->phase == start_phase || barrier->phase == next_phase);
		release = barrier->phase == next_phase;
		if (release && barrier->elected != next_phase)
		{
			/*
			 * 通常由最后到达并放开其他后端的那个后端
			 * 被选出来返回真（见上文），以便它能在自己的
			 * CPU 时间片里开始处理串行工作。
			 * 不过，若屏障是因为有人脱离而推进的，那么
			 * 就得从被唤醒的后端里选出一个。
			 */
			barrier->elected = barrier->phase;
			elected = true;
		}
		SpinLockRelease(&barrier->mutex);
		if (release)
			break;
		ConditionVariableSleep(&barrier->condition_variable, wait_event_info);
	}
	ConditionVariableCancelSleep();

	return elected;
}

/*
 * 到达此屏障，但脱离而不是等待。若调用方是
 * 最后一个脱离的，则返回真。
 */
bool
BarrierArriveAndDetach(Barrier *barrier)
{
	return BarrierDetachImpl(barrier, true);
}

/*
 * 到达屏障，除最后到达者外全部脱离。若调用方是
 * 最后到达的、因而仍然挂接，则返回真。
 */
bool
BarrierArriveAndDetachExceptLast(Barrier *barrier)
{
	SpinLockAcquire(&barrier->mutex);
	if (barrier->participants > 1)
	{
		--barrier->participants;
		SpinLockRelease(&barrier->mutex);

		return false;
	}
	Assert(barrier->participants == 1);
	++barrier->phase;
	SpinLockRelease(&barrier->mutex);

	return true;
}

/*
 * 挂接到屏障。所有正在等待的参与者现在都会等待此
 * 参与者调用 BarrierArriveAndWait()、BarrierDetach() 或
 * BarrierArriveAndDetach()。返回当前阶段。
 */
int
BarrierAttach(Barrier *barrier)
{
	int			phase;

	Assert(!barrier->static_party);

	SpinLockAcquire(&barrier->mutex);
	++barrier->participants;
	phase = barrier->phase;
	SpinLockRelease(&barrier->mutex);

	return phase;
}

/*
 * 从屏障脱离。这可能把其他等待者从
 * BarrierArriveAndWait() 里放开，并在他们只是在等
 * 此后端时推进阶段。若此参与者是最后一个脱离的，返回真。
 */
bool
BarrierDetach(Barrier *barrier)
{
	return BarrierDetachImpl(barrier, false);
}

/*
 * 返回屏障的当前阶段。调用方必须已挂接。
 */
int
BarrierPhase(Barrier *barrier)
{
	/*
	 * 不加锁读取 barrier->phase 是可以的，因为它不会在
	 * 没有我们的情况下改变（我们挂接在上面），而且上次
	 * 挂接或参与改变它时，我们已经执行过一次
	 * 内存屏障。
	 */
	return barrier->phase;
}

/*
 * 返回当前挂接在此屏障上的参与者数量的瞬时快照。
 * 仅供调试。
 */
int
BarrierParticipants(Barrier *barrier)
{
	int			participants;

	SpinLockAcquire(&barrier->mutex);
	participants = barrier->participants;
	SpinLockRelease(&barrier->mutex);

	return participants;
}

/*
 * 从屏障脱离。若 arrive 为真，且没有其他参与者，
 * 则阶段也加一。若有其他参与者在等待，则当他们
 * 只是在等调用方时，阶段会推进并放开他们。
 * 若此参与者是最后一个脱离的，
 * 返回真。
 */
static inline bool
BarrierDetachImpl(Barrier *barrier, bool arrive)
{
	bool		release;
	bool		last;

	Assert(!barrier->static_party);

	SpinLockAcquire(&barrier->mutex);
	Assert(barrier->participants > 0);
	--barrier->participants;

	/*
	 * 若有其他参与者在等待，且我们是他们等待的最后一个
	 * 参与者，就放开他们。若没有其他参与者在等待，但这是
	 * 一次 BarrierArriveAndDetach() 调用，则阶段也向前推进。
	 */
	if ((arrive || barrier->participants > 0) &&
		barrier->arrived == barrier->participants)
	{
		release = true;
		barrier->arrived = 0;
		++barrier->phase;
	}
	else
		release = false;

	last = barrier->participants == 0;
	SpinLockRelease(&barrier->mutex);

	if (release)
		ConditionVariableBroadcast(&barrier->condition_variable);

	return last;
}
