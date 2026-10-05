/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES 共享缓存失效数据管理器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinvaladt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/transam.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"

/*
 * 概念上，共享缓存失效消息存放在一个
 * 无限数组里：maxMsgNum 是下一个用来存放
 * 已提交消息的数组下标，minMsgNum 是仍含有
 * 尚未被所有后端读过的消息的最小下标，并且始终有 maxMsgNum >=
 * minMsgNum。（没有待处理消息时二者相等。）对每个
 * 活动后端，都有一个 nextMsgNum 指针，指出它下一步
 * 要读的消息；每个后端都满足
 * maxMsgNum >= nextMsgNum >= minMsgNum。
 *
 * （在当前实现里，minMsgNum 是各进程
 * nextMsgNum 值的下界，但并不严格保持等于
 * 最小的 nextMsgNum——它可能滞后。我们只在
 * 调用 SICleanupQueue 时更新它，并且尽量不经常这么做。）
 *
 * 实际上，消息存放在有 MAXNUMMESSAGES
 * 项的环形缓冲区里。我们把 MsgNum 值换算成环形缓冲区下标，
 * 算法是 MsgNum % MAXNUMMESSAGES（只要
 * MAXNUMMESSAGES 是常量且为 2 的幂，这一步就应当很快）。只要 maxMsgNum
 * 超出 minMsgNum 不超过 MAXNUMMESSAGES，缓冲区里就有足够空间。
 * 若缓冲区确实溢出，恢复办法是给每个
 * 落后过多的后端设置 "reset" 标志。处于
 * "reset" 状态的后端在确定 minMsgNum 时被忽略。当
 * 它终于尝试接收失效消息时，必须丢掉全部
 * 可失效状态，因为它无从知道自己错过了什么。
 *
 * 为降低需要重置的概率，我们向任何看起来
 * 落后得不合理的后端发送 "catchup" 中断。
 * 正常行为是同一时刻最多有一个这样的中断在途；
 * 后端处理完一次跟上中断后，会执行
 * SICleanupQueue，必要时再向次落后的后端
 * 发信号。这样避免多个后端同时试图
 * 跟上而造成过度争用。不过，最落后的后端可能卡在
 * 无法跟上的状态里。最终它会被重置，于是
 * 除了它自己之外不会再给别人添麻烦。但我们不希望
 * 发现一批其他后端已经贴近重置
 * 阈值、来不及挽救。因此 SICleanupQueue 被设计成偶尔
 * 在队列变满时额外发送跟上中断，发给那些
 * 落后很多且尚未收到中断的后端。只要没有很多
 * "卡住" 的后端，就不需要很多额外中断，因为没卡住的
 * 会把中断传给下一个。
 *
 * 若 MsgNum 值溢出整数就会出问题，因此
 * 每当 minMsgNum 超过 MSGNUMWRAPAROUND，我们就从
 * 所有 MsgNum 变量中同时减去 MSGNUMWRAPAROUND。MSGNUMWRAPAROUND 可以
 * 很大，这样就不需要经常做这件事。它必须是
 * MAXNUMMESSAGES 的倍数，这样已有的环形缓冲区项
 * 在这样做时不必移动。
 *
 * 对共享 sinval 数组的访问由两把锁保护：SInvalReadLock
 * 和 SInvalWriteLock。读者以共享模式取得 SInvalReadLock；这
 * 授权它们修改自己的 ProcState，但不能修改甚至
 * 查看别人的。当需要做全数组更新时，
 * 例如在 SICleanupQueue 中，我们以排他模式取得 SInvalReadLock，
 * 把所有读者锁在外面。写者取得 SInvalWriteLock（始终为排他
 * 模式），以便串行化向队列添加消息。注意写者
 * 可以与一个或多个读者并行工作，因为写者
 * 无需触碰任何人的 ProcState，除了偶尔
 * 需要 SICleanupQueue 的情形。唯一的重叠点是
 * 写者要改 maxMsgNum，而读者需要读它。
 * 我们用一把自旋锁来处理：读者只在
 * 读取 maxMsgNum 的短暂期间持有它，写者只在
 * 写入 maxMsgNum 的短暂期间持有它。（确切规则是：
 * 若未持有 SInvalWriteLock，读 maxMsgNum 需要自旋锁；
 * 除非同时持有两把锁，写 maxMsgNum 也需要自旋锁。）
 *
 * 注意：既然 maxMsgNum 是 int，因而大概可以原子地读/
 * 写，自旋锁似乎没有必要。需要它的原因
 * 是提供内存屏障：我们必须确保写入
 * 数组的消息在 maxMsgNum 增加之前确实已经就位，并且
 * 读者取到 maxMsgNum 之后能看见那些数据。内存序保证
 * 较弱的多处理器，若没有自旋锁序列里包含的内存
 * 屏障指令，就可能出错。
 */


/*
 * 可配置参数。
 *
 * MAXNUMMESSAGES：能够缓冲的共享失效消息最大条数。
 * 为了速度，必须是 2 的幂。
 *
 * MSGNUMWRAPAROUND：为避免溢出，隔多久把 MsgNum 变量缩小一次。
 * 必须是 MAXNUMMESSAGES 的倍数。应当取大。
 *
 * CLEANUP_MIN：缓冲区里至少要有这么多条消息，
 * 我们才费心调用 SICleanupQueue。
 *
 * CLEANUP_QUANTUM：一旦超过 CLEANUP_MIN，每隔多少条消息
 * （以消息条数计）调用一次 SICleanupQueue。为了速度，应当是 2 的幂。
 *
 * SIG_THRESHOLD：后端至少落后这么多条消息，
 * 我们才会向它发送 PROCSIG_CATCHUP_INTERRUPT。
 *
 * WRITE_QUANTUM：SIInsertDataEntries 每次迭代最多压入缓冲区的
 * 消息条数。并非关键，但应当小于
 * CLEANUP_QUANTUM，因为每次迭代只考虑调用一次
 * SICleanupQueue。
 */

#define MAXNUMMESSAGES 4096
#define MSGNUMWRAPAROUND (MAXNUMMESSAGES * 262144)
#define CLEANUP_MIN (MAXNUMMESSAGES / 2)
#define CLEANUP_QUANTUM (MAXNUMMESSAGES / 16)
#define SIG_THRESHOLD (MAXNUMMESSAGES / 2)
#define WRITE_QUANTUM 64

/* 共享失效结构中的每后端状态 */
typedef struct ProcState
{
	/* 不活动的 ProcState 数组项里 procPid 为零。 */
	pid_t		procPid;		/* 后端 PID，用于发信号 */
	PGPROC	   *proc;			/* 后端的 PGPROC */
	/* 若 procPid == 0 或 resetState 为真，nextMsgNum 无意义。 */
	int			nextMsgNum;		/* 下一步要读的消息号 */
	bool		resetState;		/* 后端需要重置自己的状态 */
	bool		signaled;		/* 已向后端发送跟上信号 */
	bool		hasMessages;	/* 后端有未读消息 */

	/*
	 * 后端只发送失效消息，从不接收。这只对
	 * 恢复期间的启动进程有意义，因为它不维护
	 * relcache，却要发出失效消息，好让查询后端看见
	 * 模式变更。
	 */
	bool		sendOnly;		/* 后端只发送，从不接收 */

	/*
	 * 每个空闲后端槽下一个要用的 LocalTransactionId。放在
	 * 这里是因为它按 BackendId 索引，而且在设置 MyBackendId 时
	 * 便于在本地内存与这里之间拷贝。活动的 ProcState
	 * 项里它没有意义。
	 */
	LocalTransactionId nextLXID;
} ProcState;

/* 共享缓存失效内存段 */
typedef struct SISeg
{
	/*
	 * 一般状态信息
	 */
	int			minMsgNum;		/* 仍需要的最旧消息 */
	int			maxMsgNum;		/* 下一个待分配的消息号 */
	int			nextThreshold;	/* 调用 SICleanupQueue 的消息条数 */
	int			lastBackend;	/* 最后一个活动 procState 项的下标，加 1 */
	int			maxBackends;	/* procState 数组的大小 */

	slock_t		msgnumLock;		/* 保护 maxMsgNum 的自旋锁 */

	/*
	 * 存放共享失效消息的环形缓冲区
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];

	/*
	 * 每后端失效状态信息（有 MaxBackends 项）。
	 */
	ProcState	procState[FLEXIBLE_ARRAY_MEMBER];
} SISeg;

static SISeg *shmInvalBuffer;	/* 指向共享失效缓冲区的指针 */


static LocalTransactionId nextLocalTransactionId;

static void CleanupInvalidationState(int status, Datum arg);


/*
 * SInvalShmemSize --- 返回所需的共享内存空间
 */
Size
SInvalShmemSize(void)
{
	Size		size;

	size = offsetof(SISeg, procState);

	/*
	 * 在热备模式下，启动进程通过
	 * InitRecoveryTransactionEnvironment() 申请一个 procState 数组
	 * 槽。尽管 MaxBackends 没有把启动进程计算在内，它仍保证
	 * 能拿到一个空闲槽。这是因为计入 MaxBackends 的自动清理
	 * 启动器和工作进程在热备模式下
	 * 不会启动。
	 */
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	return size;
}

/*
 * CreateSharedInvalidationState
 *		创建并初始化 SI 消息缓冲区
 */
void
CreateSharedInvalidationState(void)
{
	int			i;
	bool		found;

	/* 在共享内存中分配空间 */
	shmInvalBuffer = (SISeg *)
		ShmemInitStruct("shmInvalBuffer", SInvalShmemSize(), &found);
	if (found)
		return;

	/* 清零消息计数器，保存 procState 数组大小，初始化自旋锁 */
	shmInvalBuffer->minMsgNum = 0;
	shmInvalBuffer->maxMsgNum = 0;
	shmInvalBuffer->nextThreshold = CLEANUP_MIN;
	shmInvalBuffer->lastBackend = 0;
	shmInvalBuffer->maxBackends = MaxBackends;
	SpinLockInit(&shmInvalBuffer->msgnumLock);

	/* buffer[] 数组一开始全部未使用，因此不必填充 */

	/* 把所有后端标为不活动，并初始化 nextLXID */
	for (i = 0; i < shmInvalBuffer->maxBackends; i++)
	{
		shmInvalBuffer->procState[i].procPid = 0;	/* 不活动 */
		shmInvalBuffer->procState[i].proc = NULL;
		shmInvalBuffer->procState[i].nextMsgNum = 0;	/* 无意义 */
		shmInvalBuffer->procState[i].resetState = false;
		shmInvalBuffer->procState[i].signaled = false;
		shmInvalBuffer->procState[i].hasMessages = false;
		shmInvalBuffer->procState[i].nextLXID = InvalidLocalTransactionId;
	}
}

/*
 * SharedInvalBackendInit
 *		初始化一个新后端，使之在 sinval 缓冲区上工作
 */
void
SharedInvalBackendInit(bool sendOnly)
{
	int			index;
	ProcState  *stateP = NULL;
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * 本函数可以与读操作并行，但不能与写操作并行，
	 * 因为 SIInsertDataEntries 依靠 lastBackend 来恰当地设置
	 * hasMessages。
	 */
	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	/* 在 procState 数组里找一个空闲项 */
	for (index = 0; index < segP->lastBackend; index++)
	{
		if (segP->procState[index].procPid == 0)	/* 不活动的槽？ */
		{
			stateP = &segP->procState[index];
			break;
		}
	}

	if (stateP == NULL)
	{
		if (segP->lastBackend < segP->maxBackends)
		{
			stateP = &segP->procState[segP->lastBackend];
			Assert(stateP->procPid == 0);
			segP->lastBackend++;
		}
		else
		{
			/*
			 * procState 槽用尽：超过了 MaxBackends——按正常方式报告
			 */
			MyBackendId = InvalidBackendId;
			LWLockRelease(SInvalWriteLock);
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
		}
	}

	MyBackendId = (stateP - &segP->procState[0]) + 1;

	/* 在 MyProc 中公布已分配的后端 ID */
	MyProc->backendId = MyBackendId;

	/* 把下一个本地事务 ID 取到本地内存 */
	nextLocalTransactionId = stateP->nextLXID;

	/* 把自己标为活动，现存消息都视为已读 */
	stateP->procPid = MyProcPid;
	stateP->proc = MyProc;
	stateP->nextMsgNum = segP->maxMsgNum;
	stateP->resetState = false;
	stateP->signaled = false;
	stateP->hasMessages = false;
	stateP->sendOnly = sendOnly;

	LWLockRelease(SInvalWriteLock);

	/* 登记退出例程，以便退出时把自己的项标为不活动 */
	on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP));

	elog(DEBUG4, "my backend ID is %d", MyBackendId);
}

/*
 * CleanupInvalidationState
 *		把当前后端标为不再活动。
 *
 * 本函数在后端关闭期间经由 on_shmem_exit() 调用。
 *
 * arg 的实际类型是 "SISeg*"。
 */
static void
CleanupInvalidationState(int status, Datum arg)
{
	SISeg	   *segP = (SISeg *) DatumGetPointer(arg);
	ProcState  *stateP;
	int			i;

	Assert(PointerIsValid(segP));

	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

	stateP = &segP->procState[MyBackendId - 1];

	/* 为下一个占用此后端 ID 的持有者更新下一个本地事务 ID */
	stateP->nextLXID = nextLocalTransactionId;

	/* 把自己标为不活动 */
	stateP->procPid = 0;
	stateP->proc = NULL;
	stateP->nextMsgNum = 0;
	stateP->resetState = false;
	stateP->signaled = false;

	/* 重新计算最后一个活动后端的下标 */
	for (i = segP->lastBackend; i > 0; i--)
	{
		if (segP->procState[i - 1].procPid != 0)
			break;
	}
	segP->lastBackend = i;

	LWLockRelease(SInvalWriteLock);
}

/*
 * BackendIdGetProc
 *		给定后端 ID，取得该后端的 PGPROC 结构。
 *		结果可能很快就过时，因此调用方必须
 *		小心使用这一信息。若后端不活动，
 *		返回 NULL。
 */
PGPROC *
BackendIdGetProc(int backendID)
{
	PGPROC	   *result = NULL;
	SISeg	   *segP = shmInvalBuffer;

	/* 需要挡住后端的加入和移除 */
	LWLockAcquire(SInvalWriteLock, LW_SHARED);

	if (backendID > 0 && backendID <= segP->lastBackend)
	{
		ProcState  *stateP = &segP->procState[backendID - 1];

		result = stateP->proc;
	}

	LWLockRelease(SInvalWriteLock);

	return result;
}

/*
 * BackendIdGetTransactionIds
 *		取得该后端的 xid、xmin、nsubxid 和溢出状态。
 *		结果可能很快就过时，因此调用方必须
 *		小心使用这一信息。
 */
void
BackendIdGetTransactionIds(int backendID, TransactionId *xid,
						   TransactionId *xmin, int *nsubxid, bool *overflowed)
{
	SISeg	   *segP = shmInvalBuffer;

	*xid = InvalidTransactionId;
	*xmin = InvalidTransactionId;
	*nsubxid = 0;
	*overflowed = false;

	/* 需要挡住后端的加入和移除 */
	LWLockAcquire(SInvalWriteLock, LW_SHARED);

	if (backendID > 0 && backendID <= segP->lastBackend)
	{
		ProcState  *stateP = &segP->procState[backendID - 1];
		PGPROC	   *proc = stateP->proc;

		if (proc != NULL)
		{
			*xid = proc->xid;
			*xmin = proc->xmin;
			*nsubxid = proc->subxidStatus.count;
			*overflowed = proc->subxidStatus.overflowed;
		}
	}

	LWLockRelease(SInvalWriteLock);
}

/*
 * SIInsertDataEntries
 *		把新的失效 message(s) 加入缓冲区。
 */
void
SIInsertDataEntries(const SharedInvalidationMessage *data, int n)
{
	SISeg	   *segP = shmInvalBuffer;

	/*
	 * N 可以任意大。我们把工作分成每组不超过
	 * WRITE_QUANTUM 条消息，以确保不会把锁
	 * 持有不合理地久。（这与其说是为了放进其他写者，
	 * 不如说是因为某个刚刚跟上的后端可能正要做
	 * SICleanupQueue 以传递它的信号，我们不希望它
	 * 等很久。）另外，我们需要不时考虑调用
	 * SICleanupQueue。
	 */
	while (n > 0)
	{
		int			nthistime = Min(n, WRITE_QUANTUM);
		int			numMsgs;
		int			max;
		int			i;

		n -= nthistime;

		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);

		/*
		 * 若缓冲区已满，就 *必须* 腾出一些空间。清理
		 * 队列，并重置任何妨碍空间被释放的人。
		 * 否则，只在超过下一个
		 * 满度阈值时才清理队列。每次调用 SICleanupQueue 之后，都必须循环
		 * 并重新检查缓冲区状态。
		 */
		for (;;)
		{
			numMsgs = segP->maxMsgNum - segP->minMsgNum;
			if (numMsgs + nthistime > MAXNUMMESSAGES ||
				numMsgs >= segP->nextThreshold)
				SICleanupQueue(true, nthistime);
			else
				break;
		}

		/*
		 * 把新 message(s) 插入环形缓冲区的适当槽
		 */
		max = segP->maxMsgNum;
		while (nthistime-- > 0)
		{
			segP->buffer[max % MAXNUMMESSAGES] = *data++;
			max++;
		}

		/* 用自旋锁更新 maxMsgNum 的当前值 */
		SpinLockAcquire(&segP->msgnumLock);
		segP->maxMsgNum = max;
		SpinLockRelease(&segP->msgnumLock);

		/*
		 * 既然 maxMsgNum 的变更已经全局可见，就给每个人
		 * 推一把，确保他们读到新加入的消息。
		 * 释放 SInvalWriteLock 会强制一道完整的内存屏障，因此
		 * 这些（未加锁的）修改会在我们退出
		 * 本函数之前提交到内存。
		 */
		for (i = 0; i < segP->lastBackend; i++)
		{
			ProcState  *stateP = &segP->procState[i];

			stateP->hasMessages = true;
		}

		LWLockRelease(SInvalWriteLock);
	}
}

/*
 * SIGetDataEntries
 *		为当前后端取下一个 SI message(s)，若有的话
 *
 * 可能的返回值：
 *	0:	 没有可用的 SI 消息
 *	n>0: 下 n 条 SI 消息已抽取到 data[]
 * -1:	 抽到了 SI 重置消息
 *
 * 若返回值小于数组大小 "datasize"，调用方
 * 可以认为在已返回的 one(s) 之后没有更多 SI 消息。
 * 否则需要再调用一次来收集更多消息。
 *
 * 注意：本函数可以与代表其他后端执行的其他 SIGetDataEntries
 * 实例并行运行，因为每个实例只会修改
 * 自己后端的 ProcState 字段，并且任何实例都不会查看
 * 其他后端 ProcStates 的字段。我们用共享模式取得 SInvalReadLock
 * 来表达这一点。注意这并不完全是共享锁通常的（只读）
 * 含义！在因任何其他理由以共享模式取得 SInvalReadLock 之前，
 * 请仔细查看这些交互！
 *
 * 注意：本函数也可以与 SIInsertDataEntries 并行运行。并不
 * 保证会返回进入本例程之后才加入的
 * 消息。
 *
 * 又注：我们假定 "datasize" 没有大到有必要
 * 把对 SInvalReadLock 的持有拆成几段。
 */
int
SIGetDataEntries(SharedInvalidationMessage *data, int datasize)
{
	SISeg	   *segP;
	ProcState  *stateP;
	int			max;
	int			n;

	segP = shmInvalBuffer;
	stateP = &segP->procState[MyBackendId - 1];

	/*
	 * 开始拿锁之前，先做一次快速的无锁测试，看是否
	 * 可能有东西可读。在多处理器系统上，
	 * 这次加载可能向后迁移，发生在
	 * 真正进入本函数之前，于是我们可能错过刚刚被
	 * 其他处理器加入的 sinval 消息。但它们无法向后迁移，
	 * 越过前面的一次获锁，所以应当没问题。若我们
	 * 还没有拿到一把能挡住后续相关
	 * 失效的锁，这类情况与失效消息一开始就稍晚到达
	 * 并没有太大差别。
	 */
	if (!stateP->hasMessages)
		return 0;

	LWLockAcquire(SInvalReadLock, LW_SHARED);

	/*
	 * 必须在确定要读多少条消息之前重置 hasMessages。
	 * 这样，若在我们确定要读多少条之后又有新消息到达，
	 * 该标志会被重新置上，我们会在中途
	 * 注意到那些消息。
	 *
	 * 注意，若最终没有读完所有消息，退出前
	 * 务必把这个标志重新置上！
	 */
	stateP->hasMessages = false;

	/* 用自旋锁读取 maxMsgNum 的当前值 */
	SpinLockAcquire(&segP->msgnumLock);
	max = segP->maxMsgNum;
	SpinLockRelease(&segP->msgnumLock);

	if (stateP->resetState)
	{
		/*
		 * 强制重置。可以说重置之后加入的任何消息
		 * 我们也已经处理过了；这意味着还应当清除
		 * signaled 标志。
		 */
		stateP->nextMsgNum = max;
		stateP->resetState = false;
		stateP->signaled = false;
		LWLockRelease(SInvalReadLock);
		return -1;
	}

	/*
	 * 取出消息并推进后端的计数器，直到 data 数组
	 * 满了，或者没有更多消息。
	 *
	 * 可能还有其他后端没读这些 message(s)，因此这里
	 * 无法删除它们。SICleanupQueue() 最终会把它们
	 * 从队列里移除。
	 */
	n = 0;
	while (n < datasize && stateP->nextMsgNum < max)
	{
		data[n++] = segP->buffer[stateP->nextMsgNum % MAXNUMMESSAGES];
		stateP->nextMsgNum++;
	}

	/*
	 * 若已经完全跟上，就重置我们的 "signaled" 标志，以便
	 * 再次落后时还能再收到信号。
	 *
	 * 若还没有完全跟上，就重置 hasMessages 标志，以便
	 * 下次能看见剩余的消息。
	 */
	if (stateP->nextMsgNum >= max)
		stateP->signaled = false;
	else
		stateP->hasMessages = true;

	LWLockRelease(SInvalReadLock);
	return n;
}

/*
 * SICleanupQueue
 *		移除已被所有活动后端消费的消息
 *
 * 若调用方正持有 SInvalWriteLock，则 callerHasWriteLock 为真。
 * minFree 是至少要腾出的消息槽数。
 *
 * 本例程可能的副作用包括：把数组里的一个或多个
 * 后端标为 "reset"，以及向某个似乎落后过多的后端
 * 发送 PROCSIG_CATCHUP_INTERRUPT。同一时刻最多向一个后端发信号，
 * 原因见文件开头的说明。
 *
 * 小心：因为必须向其他后端发信号时会暂时释放写锁，
 * 退出时并不保证仍然有 minFree
 * 个空闲消息槽。调用方必须重新检查，也许还要重试。
 */
void
SICleanupQueue(bool callerHasWriteLock, int minFree)
{
	SISeg	   *segP = shmInvalBuffer;
	int			min,
				minsig,
				lowbound,
				numMsgs,
				i;
	ProcState  *needSig = NULL;

	/* 把所有写者和读者都锁在外面 */
	if (!callerHasWriteLock)
		LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	LWLockAcquire(SInvalReadLock, LW_EXCLUSIVE);

	/*
	 * 重新计算 minMsgNum = 所有后端 nextMsgNum 的最小值，找出
	 * 需要发信号的最落后后端（若有），并重置任何
	 * 落后过多的后端。注意，因为这里忽略 sendOnly
	 * 后端，所以即使它们是唯一的活动后端，也可以继续发送消息而
	 * 不会出问题。
	 */
	min = segP->maxMsgNum;
	minsig = min - SIG_THRESHOLD;
	lowbound = min - MAXNUMMESSAGES + minFree;

	for (i = 0; i < segP->lastBackend; i++)
	{
		ProcState  *stateP = &segP->procState[i];
		int			n = stateP->nextMsgNum;

		/* 若不活动，或已经处于 reset 状态，则忽略 */
		if (stateP->procPid == 0 || stateP->resetState || stateP->sendOnly)
			continue;

		/*
		 * 若必须腾出一些空间，而这个后端妨碍了这件事，就迫使
		 * 它进入 reset 状态，然后忽略它，直到它跟上。
		 */
		if (n < lowbound)
		{
			stateP->resetState = true;
			/* 给它发信号没有意义 ... */
			continue;
		}

		/* 跟踪全局最小的 nextMsgNum */
		if (n < min)
			min = n;

		/* 同时看看未发过信号的后端里谁最落后 */
		if (n < minsig && !stateP->signaled)
		{
			minsig = n;
			needSig = stateP;
		}
	}
	segP->minMsgNum = min;

	/*
	 * 当 minMsgNum 变得非常大时，把所有消息计数器都减小，
	 * 以防止计数器溢出。这件事很少发生，
	 * 把它折进前面的循环里并不划算。
	 */
	if (min >= MSGNUMWRAPAROUND)
	{
		segP->minMsgNum -= MSGNUMWRAPAROUND;
		segP->maxMsgNum -= MSGNUMWRAPAROUND;
		for (i = 0; i < segP->lastBackend; i++)
		{
			/* 这里懒得跳过不活动的项 */
			segP->procState[i].nextMsgNum -= MSGNUMWRAPAROUND;
		}
	}

	/*
	 * 确定队列里还剩多少条消息，并设置
	 * 应当再次调用 SICleanupQueue() 的阈值。
	 */
	numMsgs = segP->maxMsgNum - segP->minMsgNum;
	if (numMsgs < CLEANUP_MIN)
		segP->nextThreshold = CLEANUP_MIN;
	else
		segP->nextThreshold = (numMsgs / CLEANUP_QUANTUM + 1) * CLEANUP_QUANTUM;

	/*
	 * 最后，向任何需要跟上中断的人发信号。由于
	 * SendProcSignal() 未必很快，执行它时
	 * 不想持有锁。
	 */
	if (needSig)
	{
		pid_t		his_pid = needSig->procPid;
		BackendId	his_backendId = (needSig - &segP->procState[0]) + 1;

		needSig->signaled = true;
		LWLockRelease(SInvalReadLock);
		LWLockRelease(SInvalWriteLock);
		elog(DEBUG4, "sending sinval catchup signal to PID %d", (int) his_pid);
		SendProcSignal(his_pid, PROCSIG_CATCHUP_INTERRUPT, his_backendId);
		if (callerHasWriteLock)
			LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	}
	else
	{
		LWLockRelease(SInvalReadLock);
		if (!callerHasWriteLock)
			LWLockRelease(SInvalWriteLock);
	}
}

/*
 * SIResetAll
 *		把所有活动后端标为 "reset"
 *
 * 在不知道需要失效什么时使用。它相当于
 * 整个数据库集簇范围的 InvalidateSystemCaches()。这是只在旧分支上的补救，
 * 用来避免 WAL 格式变更。
 *
 * 实现类似于 SICleanupQueue(false, MAXNUMMESSAGES + 1)，并
 * 多做一件事。SICleanupQueue() 假定 minFree << MAXNUMMESSAGES，因此它
 * 假定自己重置的任何后端都有 hasMessages==true。我们连
 * 已经完全跟上的后端也要重置，所以要设置 hasMessages。
 */
void
SIResetAll(void)
{
	SISeg	   *segP = shmInvalBuffer;
	int			i;

	LWLockAcquire(SInvalWriteLock, LW_EXCLUSIVE);
	LWLockAcquire(SInvalReadLock, LW_EXCLUSIVE);

	for (i = 0; i < segP->lastBackend; i++)
	{
		ProcState  *stateP = &segP->procState[i];

		if (stateP->procPid == 0 || stateP->sendOnly)
			continue;

		/* 消费这次重置会更新 "nextMsgNum" 和 "signaled"。 */
		stateP->resetState = true;
		stateP->hasMessages = true;
	}

	segP->minMsgNum = segP->maxMsgNum;
	segP->nextThreshold = CLEANUP_MIN;

	LWLockRelease(SInvalReadLock);
	LWLockRelease(SInvalWriteLock);
}


/*
 * GetNextLocalTransactionId --- 分配一个新的 LocalTransactionId
 *
 * 我们把 VirtualTransactionIds 拆成两部分，以便
 * 不必争用共享内存就能分配一个新的，只是
 * 在后端启动/关闭期间多一点点开销。
 * VirtualTransactionId 的高位部分是 BackendId，
 * 低位部分是 LocalTransactionId，由本地
 * 计数器分配。为避免 VirtualTransactionId 在
 * 很短的间隔内被重用，占用同一个后端 ID
 * 槽的前后进程应当使用连续的本地 ID 序列，这是通过
 * 拷贝上面所见的 nextLocalTransactionId 实现的。
 */
LocalTransactionId
GetNextLocalTransactionId(void)
{
	LocalTransactionId result;

	/* 循环，以免在回绕时返回 InvalidLocalTransactionId */
	do
	{
		result = nextLocalTransactionId++;
	} while (!LocalTransactionIdIsValid(result));

	return result;
}
