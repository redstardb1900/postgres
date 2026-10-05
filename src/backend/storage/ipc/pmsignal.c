/*-------------------------------------------------------------------------
 *
 * pmsignal.c
 *	  postmaster 与其子进程之间的发信号例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/pmsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/memutils.h"


/*
 * 子进程通过发送 SIGUSR1 向 postmaster 发信号。具体
 * 原因经共享内存中的标志传达。每种可能的 "reason"
 * 各保留一个布尔标志，以便不同后端可以同时发出
 * 不同原因的信号。（不过，若同一原因同时发出
 * 不止一次，postmaster 只会观察到一次。）
 *
 * 这些标志实际声明为 "volatile sig_atomic_t"，以求最大
 * 可移植性。这应能保证标志值的加载与存储是原子的，
 * 从而不必再做任何显式加锁。
 *
 * 除按原因划分的标志外，我们还保存一组
 * 按子进程划分的标志，目前只用来检测某个后端
 * 是否未经正常关闭就已退出。按子进程划分的标志
 * 有三种可能状态：UNUSED、ASSIGNED、ACTIVE。
 * UNUSED 槽可供分配。ASSIGNED 槽已关联到
 * 一个 postmaster 子进程，但该进程要么尚未
 * 接触共享内存，要么已自行清理完毕。ACTIVE 槽
 * 表示进程正在主动使用共享内存。
 * 这些槽随机分配给子进程。
 * 由 postmaster.c 负责记录
 * 哪个槽对应哪个 PID。
 *
 * 实际上还有第四种状态 WALSENDER。它与 ACTIVE 类似，
 * 但额外表明该子进程是 WAL 发送进程。
 * WAL 发送进程同样从 ACTIVE 状态开始，一旦开始流式
 * 传输 WAL 就切换到 WALSENDER（此后不再回到 ACTIVE）。
 *
 * 还有一个共享内存字段用于相反方向的通信，即从
 * postmaster 到子进程：若 postmaster 确已广播
 * SIGQUIT 信号，该字段说明其原因。
 */

#define PM_CHILD_UNUSED		0	/* 这些值必须能放进 sig_atomic_t */
#define PM_CHILD_ASSIGNED	1
#define PM_CHILD_ACTIVE		2
#define PM_CHILD_WALSENDER	3

/* "typedef struct PMSignalData PMSignalData" 出现在 pmsignal.h 中 */
struct PMSignalData
{
	/* 用于向 postmaster 发信号的按原因标志 */
	sig_atomic_t PMSignalFlags[NUM_PMSIGNALS];
	/* postmaster 向子进程发信号所用的全局标志 */
	QuitSignalReason sigquit_reason;	/* 为何发送了 SIGQUIT */
	/* 按子进程划分的标志 */
	int			num_child_flags;	/* PMChildFlags[] 中的条目数 */
	sig_atomic_t PMChildFlags[FLEXIBLE_ARRAY_MEMBER];
};

/* PMSignalState 指针在 postmaster 与子进程中都有效 */
NON_EXEC_STATIC volatile PMSignalData *PMSignalState = NULL;

/*
 * 这些静态变量只在 postmaster 中有效。我们保留一份
 * 重复的私有数组，这样即使某个出错的子进程破坏了
 * 共享内存中的 PMSignalData 结构，我们仍能信任其状态。
 */
static int	num_child_inuse;	/* PMChildInUse[] 中的条目数 */
static int	next_child_inuse;	/* 下一个尝试分配的槽 */
static bool *PMChildInUse;		/* 若第 i 个标志槽已分配则为真 */

/*
 * 在 postmaster 死亡时得到通知的信号处理函数。
 */
#ifdef USE_POSTMASTER_DEATH_SIGNAL
volatile sig_atomic_t postmaster_possibly_dead = false;

static void
postmaster_death_handler(SIGNAL_ARGS)
{
	postmaster_possibly_dead = true;
}

/*
 * 可用信号取决于操作系统。SIGUSR1 与 SIGUSR2 已经
 * 用于其他用途，因此另选一个。
 *
 * 目前我们假定总能找到一个可用信号。对所有
 * 足够现代、具备父进程死亡发信号机制的平台来说，
 * 这个假定似乎合理。
 */
#if defined(SIGINFO)
#define POSTMASTER_DEATH_SIGNAL SIGINFO
#elif defined(SIGPWR)
#define POSTMASTER_DEATH_SIGNAL SIGPWR
#else
#error "cannot find a signal to use for postmaster death"
#endif

#endif							/* 宏 USE_POSTMASTER_DEATH_SIGNAL */

/*
 * PMSignalShmemSize
 *		计算 pmsignal.c 所需的共享内存空间
 */
Size
PMSignalShmemSize(void)
{
	Size		size;

	size = offsetof(PMSignalData, PMChildFlags);
	size = add_size(size, mul_size(MaxLivePostmasterChildren(),
								   sizeof(sig_atomic_t)));

	return size;
}

/*
 * PMSignalShmemInit - 在创建共享内存期间初始化
 */
void
PMSignalShmemInit(void)
{
	bool		found;

	PMSignalState = (PMSignalData *)
		ShmemInitStruct("PMSignalState", PMSignalShmemSize(), &found);

	if (!found)
	{
		/* 把所有标志初始化为零 */
		MemSet(unvolatize(PMSignalData *, PMSignalState), 0, PMSignalShmemSize());
		num_child_inuse = MaxLivePostmasterChildren();
		PMSignalState->num_child_flags = num_child_inuse;

		/*
		 * 同时分配 postmaster 私有的 PMChildInUse[] 数组。
		 * 上一轮共享内存创建中可能已经做过，此时释放旧数组
		 * 以免泄漏。（这样做是为了支持
		 * MaxLivePostmasterChildren 可能已改变的情况。）
		 * 独立后端不需要这个数组。
		 */
		if (PostmasterContext != NULL)
		{
			if (PMChildInUse)
				pfree(PMChildInUse);
			PMChildInUse = (bool *)
				MemoryContextAllocZero(PostmasterContext,
									   num_child_inuse * sizeof(bool));
		}
		next_child_inuse = 0;
	}
}

/*
 * SendPostmasterSignal - 从子进程向 postmaster 发信号
 */
void
SendPostmasterSignal(PMSignalReason reason)
{
	/* 若在独立后端中调用，则什么也不做 */
	if (!IsUnderPostmaster)
		return;
	/* 原子地设置相应标志 */
	PMSignalState->PMSignalFlags[reason] = true;
	/* 向 postmaster 发送信号 */
	kill(PostmasterPid, SIGUSR1);
}

/*
 * CheckPostmasterSignal - 检查某一原因是否已被发出信号，
 * 并清除该信号标志。postmaster 收到 SIGUSR1 之后
 * 应当调用本函数。
 */
bool
CheckPostmasterSignal(PMSignalReason reason)
{
	/* 这里要小心 --- 尚未看到标志已置位时，不要清除它 */
	if (PMSignalState->PMSignalFlags[reason])
	{
		PMSignalState->PMSignalFlags[reason] = false;
		return true;
	}
	return false;
}

/*
 * SetQuitSignalReason - 广播系统关闭的原因。
 * 应当由 postmaster 在向子进程发送 SIGQUIT 之前调用。
 *
 * 注意：在崩溃后重启的情形下，重建共享内存时
 * 会清除 "reason" 字段；postmaster 不必
 * 再显式去做。
 */
void
SetQuitSignalReason(QuitSignalReason reason)
{
	PMSignalState->sigquit_reason = reason;
}

/*
 * GetQuitSignalReason - 取得系统关闭的原因。
 * 子进程收到 SIGQUIT 时调用。
 * 若 postmaster 实际上并未发送 SIGQUIT，将返回 PMQUIT_NOT_SENT。
 */
QuitSignalReason
GetQuitSignalReason(void)
{
	/* 本函数在信号处理函数中调用，因此要格外小心。 */
	if (!IsUnderPostmaster || PMSignalState == NULL)
		return PMQUIT_NOT_SENT;
	return PMSignalState->sigquit_reason;
}


/*
 * AssignPostmasterChildSlot - 为新的 postmaster 子进程
 * 选择一个未使用的槽，并将其状态设为 ASSIGNED。返回槽号
 * （一到 N）。
 *
 * 只有 postmaster 可以执行本例程，因此不需要
 * 特殊加锁。
 */
int
AssignPostmasterChildSlot(void)
{
	int			slot = next_child_inuse;
	int			n;

	/*
	 * 扫描一个空闲槽。注意，我们不信任
	 * PMSignalState 的内容，这项决定只用 postmaster 本地数据。
	 * 我们记录上次分配的槽，以免反复重扫
	 * 编号较小的槽而浪费时间。
	 */
	for (n = num_child_inuse; n > 0; n--)
	{
		if (--slot < 0)
			slot = num_child_inuse - 1;
		if (!PMChildInUse[slot])
		{
			PMChildInUse[slot] = true;
			PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
			next_child_inuse = slot;
			return slot + 1;
		}
	}

	/* 槽已用尽 ... 按理不该发生，否则就是 postmaster.c 弄错了 */
	elog(FATAL, "no free slots in PMChildFlags array");
	return 0;					/* 消除编译器告警 */
}

/*
 * ReleasePostmasterChildSlot - 在 postmaster 子进程死亡后
 * 释放槽。必须在 postmaster 进程中调用。
 *
 * 若该槽此前处于 ASSIGNED 状态（预期情况）则返回真，
 * 否则返回假（意味着该子进程未能自行清理）。
 */
bool
ReleasePostmasterChildSlot(int slot)
{
	bool		result;

	Assert(slot > 0 && slot <= num_child_inuse);
	slot--;

	/*
	 * 注意：槽状态可能已经是未使用，因为 postmaster.c 的逻辑
	 * 使得子进程崩溃时本函数可能被调用两次。因此我们不试图
	 * 对状态做任何 Assert。
	 */
	result = (PMSignalState->PMChildFlags[slot] == PM_CHILD_ASSIGNED);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_UNUSED;
	PMChildInUse[slot] = false;
	return result;
}

/*
 * IsPostmasterChildWalSender - 检查给定槽是否正由
 * walsender 进程使用。只由 postmaster 调用。
 */
bool
IsPostmasterChildWalSender(int slot)
{
	Assert(slot > 0 && slot <= num_child_inuse);
	slot--;

	if (PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER)
		return true;
	else
		return false;
}

/*
 * MarkPostmasterChildActive - 把 postmaster 子进程标记为即将
 * 开始主动使用共享内存。在子进程中调用。
 */
void
MarkPostmasterChildActive(void)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ASSIGNED);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ACTIVE;
}

/*
 * MarkPostmasterChildWalSender - 把 postmaster 子进程标记为 WAL
 * 发送进程。在子进程中调用，且在把该子进程标记为
 * 活动之后的某个时刻调用。
 */
void
MarkPostmasterChildWalSender(void)
{
	int			slot = MyPMChildSlot;

	Assert(am_walsender);

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ACTIVE);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_WALSENDER;
}

/*
 * MarkPostmasterChildInactive - 把 postmaster 子进程标记为
 * 已用完共享内存。在子进程中调用。
 */
void
MarkPostmasterChildInactive(void)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ACTIVE ||
		   PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
}


/*
 * PostmasterIsAliveInternal - 检查 postmaster 进程是否仍然存活
 *
 * 这是 PostmasterIsAlive() 的慢路径，调用方已经检查过
 * 'postmaster_possibly_dead'。（在不支持父进程死亡信号的
 * 平台上，PostmasterIsAlive() 只是本函数的别名。）
 */
bool
PostmasterIsAliveInternal(void)
{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
	/*
	 * 检查之前先复位该标志，这样若 postmaster 恰在检查之后
	 * 死亡，我们就不会漏掉信号。若 postmaster 确实已死，
	 * 返回调用方之前会重新置位。
	 */
	postmaster_possibly_dead = false;
#endif

#ifndef WIN32
	{
		char		c;
		ssize_t		rc;

		rc = read(postmaster_alive_fds[POSTMASTER_FD_WATCH], &c, 1);

		/*
		 * 通常情况下 postmaster 仍然存活，管道中没有
		 * 数据。
		 */
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		else
		{
			/*
			 * postmaster 已死，或者 read() 调用出了
			 * 问题。
			 */

#ifdef USE_POSTMASTER_DEATH_SIGNAL
			postmaster_possibly_dead = true;
#endif

			if (rc < 0)
				elog(FATAL, "read on postmaster death monitoring pipe failed: %m");
			else if (rc > 0)
				elog(FATAL, "unexpected data in postmaster death monitoring pipe");

			return false;
		}
	}

#else							/* 平台宏 WIN32 */
	if (WaitForSingleObject(PostmasterHandle, 0) == WAIT_TIMEOUT)
		return true;
	else
	{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
		postmaster_possibly_dead = true;
#endif
		return false;
	}
#endif							/* 平台宏 WIN32 */
}

/*
 * PostmasterDeathSignalInit - 尽可能请求在 postmaster 死亡时发信号
 */
void
PostmasterDeathSignalInit(void)
{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
	int			signum = POSTMASTER_DEATH_SIGNAL;

	/* 登记我们的信号处理函数。 */
	pqsignal(signum, postmaster_death_handler);

	/* 请求在父进程退出时发信号。 */
#if defined(PR_SET_PDEATHSIG)
	if (prctl(PR_SET_PDEATHSIG, signum) < 0)
		elog(ERROR, "could not request parent death signal: %m");
#elif defined(PROC_PDEATHSIG_CTL)
	if (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &signum) < 0)
		elog(ERROR, "could not request parent death signal: %m");
#else
#error "USE_POSTMASTER_DEATH_SIGNAL set, but there is no mechanism to request the signal"
#endif

	/*
	 * 以防万一父进程已经消失而我们错过了，最好在
	 * 第一次调用时走慢路径检查。
	 */
	postmaster_possibly_dead = true;
#endif							/* 宏 USE_POSTMASTER_DEATH_SIGNAL */
}
