/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  进程间发信号例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/parallel.h"
#include "port/pg_bitutils.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logicalworker.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

/*
 * SIGUSR1 信号被复用，以支持多种事件
 * 类型的发信号。具体原因经共享内存中的标志传达。
 * 每种可能的 "reason" 各保留一个布尔标志，以便不同
 * 原因可以并发地发给同一进程。（不过，若同一
 * 原因在几乎同一时刻发出不止一次，该进程可能
 * 只会观察到一次。）
 *
 * 每个想接收信号的进程都在 ProcSignalSlots 数组中
 * 登记自己的进程 ID。该数组按后端 ID 索引，以便
 * 简化槽分配，并在已知所发信号进程的后端 ID 时
 * 不必搜索数组。（我们也支持不带后端 ID 的
 * 发信号，但效率略低。）
 *
 * 这些标志实际声明为 "volatile sig_atomic_t"，以求最大
 * 可移植性。这应能保证标志值的加载与存储是原子的，
 * 从而不必再做任何显式加锁。
 *
 * pss_signalFlags 用于不需要跟踪目标进程
 * 是否已处理该信号的场合，但有时我们需要确认，
 * 例如做一次全局状态变更，在所有后端都注意到之前
 * 无法视为完成。对这类用途，我们在 pss_barrierCheckMask
 * 中置一位，然后递增当前的 "barrier generation"；
 * 当新的屏障代（或更大）出现在每个进程的
 * pss_barrierGeneration 标志中时，我们就知道
 * 该消息已在各处被收到。
 */
typedef struct
{
	volatile pid_t pss_pid;
	volatile sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];
	pg_atomic_uint64 pss_barrierGeneration;
	pg_atomic_uint32 pss_barrierCheckMask;
	ConditionVariable pss_barrierCV;
} ProcSignalSlot;

/*
 * 整个 ProcSignal 系统的全局信息
 * 可以存放在这里。
 *
 * psh_barrierGeneration 是现存最高的屏障代。
 */
typedef struct
{
	pg_atomic_uint64 psh_barrierGeneration;
	ProcSignalSlot psh_slot[FLEXIBLE_ARRAY_MEMBER];
} ProcSignalHeader;

/*
 * 我们为每个可能的 BackendId 保留一个槽，再为每种
 * 可能的辅助进程类型保留一个。（本方案假定任一
 * 辅助进程类型在同一时刻不会有多于一个。）
 */
#define NumProcSignalSlots	(MaxBackends + NUM_AUXPROCTYPES)

/* 检查标志中相应类型位是否已置位。 */
#define BARRIER_SHOULD_CHECK(flags, type) \
	(((flags) & (((uint32) 1) << (uint32) (type))) != 0)

/* 从标志中清除相应类型位。 */
#define BARRIER_CLEAR_BIT(flags, type) \
	((flags) &= ~(((uint32) 1) << (uint32) (type)))

static ProcSignalHeader *ProcSignal = NULL;
static ProcSignalSlot *MyProcSignalSlot = NULL;

static bool CheckProcSignal(ProcSignalReason reason);
static void CleanupProcSignalState(int status, Datum arg);
static void ResetProcSignalBarrierBits(uint32 flags);

/*
 * ProcSignalShmemSize
 *		计算 ProcSignal 所需的共享内存空间
 */
Size
ProcSignalShmemSize(void)
{
	Size		size;

	size = mul_size(NumProcSignalSlots, sizeof(ProcSignalSlot));
	size = add_size(size, offsetof(ProcSignalHeader, psh_slot));
	return size;
}

/*
 * ProcSignalShmemInit
 *		分配并初始化 ProcSignal 的共享内存
 */
void
ProcSignalShmemInit(void)
{
	Size		size = ProcSignalShmemSize();
	bool		found;

	ProcSignal = (ProcSignalHeader *)
		ShmemInitStruct("ProcSignal", size, &found);

	/* 若我们是第一个，就初始化。 */
	if (!found)
	{
		int			i;

		pg_atomic_init_u64(&ProcSignal->psh_barrierGeneration, 0);

		for (i = 0; i < NumProcSignalSlots; ++i)
		{
			ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

			slot->pss_pid = 0;
			MemSet(slot->pss_signalFlags, 0, sizeof(slot->pss_signalFlags));
			pg_atomic_init_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
			pg_atomic_init_u32(&slot->pss_barrierCheckMask, 0);
			ConditionVariableInit(&slot->pss_barrierCV);
		}
	}
}

/*
 * ProcSignalInit
 *		在 ProcSignal 数组中登记当前进程
 *
 * 传入的下标在进程有 BackendId 时应为它自己的 BackendId，
 * 否则为 MaxBackends 加上辅助进程类型。
 */
void
ProcSignalInit(int pss_idx)
{
	ProcSignalSlot *slot;
	uint64		barrier_generation;

	Assert(pss_idx >= 1 && pss_idx <= NumProcSignalSlots);

	slot = &ProcSignal->psh_slot[pss_idx - 1];

	/* 健全性检查 */
	if (slot->pss_pid != 0)
		elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
			 MyProcPid, pss_idx);

	/* 清除任何残留的信号原因 */
	MemSet(slot->pss_signalFlags, 0, NUM_PROCSIGNALS * sizeof(sig_atomic_t));

	/*
	 * 初始化屏障状态。既然我们是全新进程，
	 * 不应有任何需要更新的残留后端私有状态。
	 * 因此我们可以广播最新的屏障代，并忽略
	 * 先前设置的检查位。
	 *
	 * 注意：只有当这次初始化发生在启动序列中足够早、
	 * 我们尚未缓存任何可能需要失效的状态时，这才成立。
	 * 这也是这里要有内存屏障的原因，以确保此后
	 * 任何内存读取都严格发生在此之后。
	 */
	pg_atomic_write_u32(&slot->pss_barrierCheckMask, 0);
	barrier_generation =
		pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);
	pg_atomic_write_u64(&slot->pss_barrierGeneration, barrier_generation);
	pg_memory_barrier();

	/* 用我的 PID 标记槽 */
	slot->pss_pid = MyProcPid;

	/* 记住槽位置，供 CheckProcSignal 使用 */
	MyProcSignalSlot = slot;

	/* 安排在进程退出时释放该槽 */
	on_shmem_exit(CleanupProcSignalState, Int32GetDatum(pss_idx));
}

/*
 * CleanupProcSignalState
 *		把当前进程从 ProcSignal 机制中移除
 *
 * 本函数在后端关闭期间经由 on_shmem_exit() 调用。
 */
static void
CleanupProcSignalState(int status, Datum arg)
{
	int			pss_idx = DatumGetInt32(arg);
	ProcSignalSlot *slot;

	slot = &ProcSignal->psh_slot[pss_idx - 1];
	Assert(slot == MyProcSignalSlot);

	/*
	 * 清除 MyProcSignalSlot，这样此后收到的 SIGUSR1
	 * 就不会在它不再属于我们之后（甚至可能在我们已经
	 * 解除共享内存段映射之后）试图访问它。
	 */
	MyProcSignalSlot = NULL;

	/* 健全性检查 */
	if (slot->pss_pid != MyProcPid)
	{
		/*
		 * 这里不要 ERROR。我们反正正在退出，不想陷入
		 * 试图退出的无限循环
		 */
		elog(LOG, "process %d releasing ProcSignal slot %d, but it contains %d",
			 MyProcPid, pss_idx, (int) slot->pss_pid);
		return;					/* XXX 反正把槽清零是不是更好？ */
	}

	/*
	 * 让这个槽看起来已经吸收了所有可能的屏障，这样
	 * 就不会有屏障等待阻塞在它上面。
	 */
	pg_atomic_write_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
	ConditionVariableBroadcast(&slot->pss_barrierCV);

	slot->pss_pid = 0;
}

/*
 * SendProcSignal
 *		向一个 Postgres 进程发送信号
 *
 * 提供 backendId 是可选的，但能加快本操作。
 *
 * 成功（已发出信号）时返回零。
 * 出错时返回 -1，并设置 errno（通常为 ESRCH 或 EPERM）。
 *
 * 不要与 ProcSendSignal 混淆
 */
int
SendProcSignal(pid_t pid, ProcSignalReason reason, BackendId backendId)
{
	volatile ProcSignalSlot *slot;

	if (backendId != InvalidBackendId)
	{
		slot = &ProcSignal->psh_slot[backendId - 1];

		/*
		 * 注意：由于没有加锁，目标进程可能就在这次
		 * 测试之后脱离共享内存并退出，发生在我们设置标志
		 * 并发出信号之前。信号槽甚至可能被新进程回收，
		 * 因此我们远程地有可能给错误的进程设置标志。
		 * 这没有关系，所有这些信号即使误发
		 * 也不会造成损害。
		 */
		if (slot->pss_pid == pid)
		{
			/* 原子地设置相应标志 */
			slot->pss_signalFlags[reason] = true;
			/* 发送信号 */
			return kill(pid, SIGUSR1);
		}
	}
	else
	{
		/*
		 * 未提供 BackendId，因此用 pid 搜索数组。
		 * 我们从后向前搜索，以减少搜索开销。传入
		 * InvalidBackendId 意味着目标最可能是辅助进程，
		 * 其槽靠近数组末尾。
		 */
		int			i;

		for (i = NumProcSignalSlots - 1; i >= 0; i--)
		{
			slot = &ProcSignal->psh_slot[i];

			if (slot->pss_pid == pid)
			{
				/* 上面关于竞态的说明在这里同样适用 */

				/* 原子地设置相应标志 */
				slot->pss_signalFlags[reason] = true;
				/* 发送信号 */
				return kill(pid, SIGUSR1);
			}
		}
	}

	errno = ESRCH;
	return -1;
}

/*
 * EmitProcSignalBarrier
 *		向每个 Postgres 进程发送信号
 *
 * 本函数的返回值是本次操作创建的屏障 "generation"。
 * 该值可以传给 WaitForProcSignalBarrier，一直等到已知
 * ProcSignal 机制的每个参与者都已吸收该信号
 * （或在此之后才启动）。
 *
 * 用它来做频繁发生的事情不是好主意，
 * 因为打断每个后端可能造成明显的
 * 性能损失。
 *
 * 调用方可以假定本函数不会抛出 ERROR
 * 或 FATAL。
 */
uint64
EmitProcSignalBarrier(ProcSignalBarrierType type)
{
	uint32		flagbit = 1 << (uint32) type;
	uint64		generation;

	/*
	 * 设置所有标志。
	 *
	 * 注意 pg_atomic_fetch_or_u32 具有完整屏障语义，因此这与
	 * 调用方在此之前所做的任何事，以及我们在此之后所做的
	 * 任何事都是全序的。（后来对 pg_atomic_add_fetch_u64 的
	 * 调用也是如此。）
	 */
	for (int i = 0; i < NumProcSignalSlots; i++)
	{
		volatile ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

		pg_atomic_fetch_or_u32(&slot->pss_barrierCheckMask, flagbit);
	}

	/*
	 * 递增代计数器。
	 */
	generation =
		pg_atomic_add_fetch_u64(&ProcSignal->psh_barrierGeneration, 1);

	/*
	 * 向所有进程发信号，使它们更新自己公布的
	 * 屏障代。
	 *
	 * 这里并发不是问题。
	 * 已经退出的后端无关紧要，而自我们进入本函数
	 * 以来加入的新后端必定已经具有当前状态，
	 * 因为调用方有责任在首先调用本函数之前
	 * 确保相关状态完全可见。我们仍须唤醒它们
	 * ——因为无法区分这类后端和需要更新状态的
	 * 较旧后端——但它们实际上不需要改变
	 * 任何状态。
	 */
	for (int i = NumProcSignalSlots - 1; i >= 0; i--)
	{
		volatile ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		pid_t		pid = slot->pss_pid;

		if (pid != 0)
		{
			/* 详情见 SendProcSignal */
			slot->pss_signalFlags[PROCSIG_BARRIER] = true;
			kill(pid, SIGUSR1);
		}
	}

	return generation;
}

/*
 * WaitForProcSignalBarrier - 一直等到可以保证某次
 * EmitProcSignalBarrier() 调用所请求的全部变更都已生效。
 */
void
WaitForProcSignalBarrier(uint64 generation)
{
	Assert(generation <= pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration));

	elog(DEBUG1,
		 "waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	for (int i = NumProcSignalSlots - 1; i >= 0; i--)
	{
		ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		uint64		oldval;

		/*
		 * 这里只检查 pss_barrierGeneration、不检查
		 * pss_barrierCheckMask 很重要。pss_barrierCheckMask 中的位
		 * 在屏障真正被吸收之前就会被清除，而 pss_barrierGeneration
		 * 要到那之后才更新。
		 */
		oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		while (oldval < generation)
		{
			if (ConditionVariableTimedSleep(&slot->pss_barrierCV,
											5000,
											WAIT_EVENT_PROC_SIGNAL_BARRIER))
				ereport(LOG,
						(errmsg("still waiting for backend with PID %d to accept ProcSignalBarrier",
								(int) slot->pss_pid)));
			oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		}
		ConditionVariableCancelSleep();
	}

	elog(DEBUG1,
		 "finished waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	/*
	 * 调用方调用本函数，多半是想在已知所有后端
	 * 都已吸收屏障之后读取共享状态，或对共享状态
	 * 做进一步写入。然而对 pss_barrierGeneration 的读取
	 * 是不加锁进行的；插入一个内存屏障，
	 * 把它与随后的操作隔开。
	 */
	pg_memory_barrier();
}

/*
 * 处理表明全局屏障事件的中断。
 *
 * 全部实际工作推迟到 ProcessProcSignalBarrier()，因为
 * 在信号处理函数里无法安全访问屏障代：64 位原子操作
 * 即使是读取也可能使用基于自旋锁的仿真。本例程只在
 * 发出 PROCSIG_BARRIER 时被调用，因此不会造成大量
 * 不必要的工作。
 */
static void
HandleProcSignalBarrierInterrupt(void)
{
	InterruptPending = true;
	ProcSignalBarrierPending = true;
	/* 锁存器将由 procsignal_sigusr1_handler 设置 */
}

/*
 * 执行与全局屏障相关的中断检查。
 *
 * 参与 ProcSignal 发信号的任何后端都必须安排
 * 周期性地调用本函数。它从 CHECK_FOR_INTERRUPTS()
 * 调用，这对普通后端足够，但对所有类型的
 * 后台进程未必足够。
 */
void
ProcessProcSignalBarrier(void)
{
	uint64		local_gen;
	uint64		shared_gen;
	volatile uint32 flags;

	Assert(MyProcSignalSlot);

	/* 若没有工作要做就快速退出。 */
	if (!ProcSignalBarrierPending)
		return;
	ProcSignalBarrierPending = false;

	/*
	 * 在所有屏障的信号都到达之前，一次处理多个屏障
	 * 并不少见。为避免响应后续信号时做不必要的工作，
	 * 若我们已经处理完所有屏障，就提前退出。
	 * all of them.
	 */
	local_gen = pg_atomic_read_u64(&MyProcSignalSlot->pss_barrierGeneration);
	shared_gen = pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);

	Assert(local_gen <= shared_gen);

	if (local_gen == shared_gen)
		return;

	/*
	 * 取得并清除为本后端设置的标志。注意
	 * pg_atomic_exchange_u32 是完整屏障，因此可以
	 * 保证上面对屏障代的读取发生在我们原子地
	 * 取出标志之前，并且随后的任何状态变更
	 * 都发生在那之后。
	 *
	 * 注意：为避免竞态，必须先把
	 * pss_barrierCheckMask 清零，然后才尝试做
	 * 屏障处理。若顺序相反，可能有人在我们调用
	 * 屏障处理函数之后、清除该位之前，又给我们
	 * 发来某种类型的另一个屏障。那样我们就无法
	 * 知道该位需要保持置位，于是再次调用屏障
	 * 处理函数的需要就会被忘掉。因此我们先
	 * 试探性地清除所有位，再把那些没能成功
	 * 吸收屏障的位放回去。
	 */
	flags = pg_atomic_exchange_u32(&MyProcSignalSlot->pss_barrierCheckMask, 0);

	/*
	 * 若没有标志被置位，就可以跳过任何实际工作。
	 * 否则建立一个 PG_TRY 块，这样若发生 ERROR，我们不会
	 * 弄丢还需要哪些类型的屏障处理。
	 */
	if (flags != 0)
	{
		bool		success = true;

		PG_TRY();
		{
			/*
			 * 处理每种类型的屏障。屏障处理函数
			 * 通常应返回真，但若当前无法吸收该屏障
			 * 则可能返回假。这应当很少见，因为代价相当高。
			 * 每一次 CHECK_FOR_INTERRUPTS() 都会回到这里，
			 * 直到我们设法吸收该屏障，
			 * 开销会很快累加。
			 *
			 * 注意：无条件调用屏障处理函数应当也可以，
			 * 但只调用根据标志可能需要我们做点什么的那些，
			 * 效率更高。
			 */
			while (flags != 0)
			{
				ProcSignalBarrierType type;
				bool		processed = true;

				type = (ProcSignalBarrierType) pg_rightmost_one_pos32(flags);
				switch (type)
				{
					case PROCSIGNAL_BARRIER_SMGRRELEASE:
						processed = ProcessBarrierSmgrRelease();
						break;
				}

				/*
				 * 为避免无限循环，必须始终清除 flags 中的
				 * 该位。
				 */
				BARRIER_CLEAR_BIT(flags, type);

				/*
				 * 若未能处理该屏障，就复位共享位，以便稍后重试，
				 * 并设置一个标志，这样我们就不会推进
				 * 自己的代。
				 */
				if (!processed)
				{
					ResetProcSignalBarrierBits(((uint32) 1) << type);
					success = false;
				}
			}
		}
		PG_CATCH();
		{
			/*
			 * 若发生了 ERROR，稍后需要重试，以处理该屏障类型
			 * 以及任何尚未处理或未能成功吸收的
			 * 其他类型。
			 */
			ResetProcSignalBarrierBits(flags);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/*
		 * 若某些屏障类型未能成功吸收，稍后还得
		 * 重试。
		 */
		if (!success)
			return;
	}

	/*
	 * 可能已发出的所有类型屏障的相关状态变更
	 * 现在都已处理，因此可以把我们对代的认识
	 * 更新为开始这些更新之前观察到的那一个。
	 * 若此后又有变化，下次调用本函数时
	 * 会修正。
	 */
	pg_atomic_write_u64(&MyProcSignalSlot->pss_barrierGeneration, shared_gen);
	ConditionVariableBroadcast(&MyProcSignalSlot->pss_barrierCV);
}

/*
 * 若结果是我们未能吸收一种或多种屏障类型，无论是因为
 * 屏障处理函数返回了假，还是因为出错，都安排稍后
 * 重试处理。
 */
static void
ResetProcSignalBarrierBits(uint32 flags)
{
	pg_atomic_fetch_or_u32(&MyProcSignalSlot->pss_barrierCheckMask, flags);
	ProcSignalBarrierPending = true;
	InterruptPending = true;
}

/*
 * CheckProcSignal - 检查某一原因是否已被发出信号，
 * 并清除该信号标志。收到 SIGUSR1 之后应当调用。
 * SIGUSR1.
 */
static bool
CheckProcSignal(ProcSignalReason reason)
{
	volatile ProcSignalSlot *slot = MyProcSignalSlot;

	if (slot != NULL)
	{
		/* 这里要小心 --- 尚未看到标志已置位时，不要清除它 */
		if (slot->pss_signalFlags[reason])
		{
			slot->pss_signalFlags[reason] = false;
			return true;
		}
	}

	return false;
}

/*
 * procsignal_sigusr1_handler - 处理 SIGUSR1 信号。
 */
void
procsignal_sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (CheckProcSignal(PROCSIG_CATCHUP_INTERRUPT))
		HandleCatchupInterrupt();

	if (CheckProcSignal(PROCSIG_NOTIFY_INTERRUPT))
		HandleNotifyInterrupt();

	if (CheckProcSignal(PROCSIG_PARALLEL_MESSAGE))
		HandleParallelMessageInterrupt();

	if (CheckProcSignal(PROCSIG_WALSND_INIT_STOPPING))
		HandleWalSndInitStopping();

	if (CheckProcSignal(PROCSIG_BARRIER))
		HandleProcSignalBarrierInterrupt();

	if (CheckProcSignal(PROCSIG_LOG_MEMORY_CONTEXT))
		HandleLogMemoryContextInterrupt();

	if (CheckProcSignal(PROCSIG_PARALLEL_APPLY_MESSAGE))
		HandleParallelApplyMessageInterrupt();

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_DATABASE))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_DATABASE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_TABLESPACE))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_TABLESPACE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_LOCK))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_LOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);

	SetLatch(MyLatch);

	errno = save_errno;
}
