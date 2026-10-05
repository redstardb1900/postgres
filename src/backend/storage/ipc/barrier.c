/*-------------------------------------------------------------------------
 *
 * barrier.c
 *	  用于同步协作进程的屏障。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * 引自 Wikipedia[1]：“在并行计算中，屏障是一种
 * 同步方法。源代码中一组线程或进程的屏障意味着
 * 任一线程/进程必须在此点停下，并且不能
 * 继续，直到所有其他线程/进程都到达该屏障。”
 *
 * 本屏障实现既支持预先已知的静态参与者集合，
 * 也支持进程可随时加入或离开的动态参与者集合。
 * 在动态情形下，可用相位编号
 * 跟踪并行算法的进度；当新参与者加入时，可能需要
 * 该编号来与多阶段算法的当前阶段同步。在静态情形下，
 * 相位编号在内部使用，但客户端代码并非严格需要访问它，
 * 因为只有在已声明数量的参与者到达屏障时相位才会推进，
 * 故客户端代码在任何时刻都不应怀疑当前
 * 计算阶段。
 *
 *
 * 考虑一个涉及独立计算阶段
 * A、B、C 的并行算法，其中每一阶段的输出都是下一阶段
 * 开始前所需要的。
 *
 * 对于初始化为 4 个参与者的静态屏障，每个
 * 参与者先做阶段 A，再调用 BarrierArriveAndWait，直到
 * 全部 4 个参与者都到达该点。当 BarrierArriveAndWait
 * 返回时，每个参与者可以做 B，依此类推。因为
 * 屏障知道应期待多少参与者，计算阶段
 * 无需标签或编号，每个进程的程序计数器即隐含
 * 当前阶段。即便某些进程启动较慢、
 * 较晚开始阶段 A，其他参与者仍期待它们，并会
 * 在屏障处耐心等待。代码可写成如下形式：
 *
 *     perform_a();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_b();
 *     BarrierArriveAndWait(&barrier, ...);
 *     perform_c();
 *     BarrierArriveAndWait(&barrier, ...);
 *
 * 若预先不知道参与者数量，则需要动态屏障，
 * 初始化时应将数量设为零。新的
 * 复杂性在于：随着参与者附着与分离，数量必然随时间变化，
 * 因此在任何给定参与者开始运行并附着之前，阶段 B、C
 * 甚至处理结束都可能已经到达。因此客户端代码在附着后
 * 必须先做一次相位编号的初始检测，以便弄清
 * 已附着的参与者到达了算法的哪个阶段，从而与该工作同步。
 * 一旦程序计数器或其他表示当前进度的形式与屏障相位同步，
 * 就可以像静态情形一样使用正常控制流。我们的例子可用
 * 带贯穿执行 case 的 switch 写成如下形式：
 *
 *
 *
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
 * 静态屏障行为类似于 POSIX 的 pthread_barrier_t。动态
 * 屏障行为类似于 Java 的 java.util.concurrent.Phaser。
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
