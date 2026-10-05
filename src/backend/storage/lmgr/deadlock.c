/*-------------------------------------------------------------------------
 *
 * deadlock.c
 *	  POSTGRES 死锁检测代码
 *
 * 死锁检测与解除算法的说明见
 * src/backend/storage/lmgr/README。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/deadlock.c
 *
 *	接口：
 *
 *	DeadLockCheck()
 *	DeadLockReport()
 *	RememberSimpleDeadLock()
 *	InitDeadLockChecking()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"


/*
 * 等待图中的一条边。
 *
 * waiter 和 blocker 也许是某个锁组的成员，也许不是；只要其中
 * 一方是，它就一定是组长，而不是该锁组的其他成员。
 * 组长代表整个锁组，即便这些特定进程
 * 完全不必处于等待之中。对于给定的锁，等待队列上至少
 * 会有 waiter 所属锁组的一个成员，
 * 也许还会更多。
 */
typedef struct
{
	PGPROC	   *waiter;			/* 正在等待的锁组的组长 */
	PGPROC	   *blocker;		/* 它所等待的那个锁组的组长 */
	LOCK	   *lock;			/* 正在等待的锁 */
	int			pred;			/* TopoSort 的工作区 */
	int			link;			/* TopoSort 的工作区 */
} EDGE;

/* 某个锁的等待队列的一种可能重排 */
typedef struct
{
	LOCK	   *lock;			/* 所描述等待队列所属的锁 */
	PGPROC	  **procs;			/* 按新等待顺序排列的 PGPROC * 数组 */
	int			nProcs;
} WAIT_ORDER;

/*
 * 保存已检测到的死锁环中每条边的信息。失败时
 * 用它来打印诊断消息。
 *
 * 注意：因为我们希望在释放锁管理器的
 * 分区锁之后再查看这些信息，所以不能只保存 LOCK 与 PGPROC 指针；
 * 必须把想要打印的信息全部提取出来。
 */
typedef struct
{
	LOCKTAG		locktag;		/* 所等待锁对象的 ID */
	LOCKMODE	lockmode;		/* 我们正在等待的锁类型 */
	int			pid;			/* 被阻塞后端的 PID */
} DEADLOCK_INFO;


static bool DeadLockCheckRecurse(PGPROC *proc);
static int	TestConfiguration(PGPROC *startProc);
static bool FindLockCycle(PGPROC *checkProc,
						  EDGE *softEdges, int *nSoftEdges);
static bool FindLockCycleRecurse(PGPROC *checkProc, int depth,
								 EDGE *softEdges, int *nSoftEdges);
static bool FindLockCycleRecurseMember(PGPROC *checkProc,
									   PGPROC *checkProcLeader,
									   int depth, EDGE *softEdges, int *nSoftEdges);
static bool ExpandConstraints(EDGE *constraints, int nConstraints);
static bool TopoSort(LOCK *lock, EDGE *constraints, int nConstraints,
					 PGPROC **ordering);

#ifdef DEBUG_DEADLOCK
static void PrintLockQueue(LOCK *lock, const char *info);
#endif


/*
 * 死锁检测器的工作区
 */

/* FindLockCycle 的工作区 */
static PGPROC **visitedProcs;	/* 已访问进程的数组 */
static int	nVisitedProcs;

/* TopoSort 的工作区 */
static PGPROC **topoProcs;		/* 尚未输出的进程数组 */
static int *beforeConstraints;	/* 剩余的 "必须在前" 约束计数 */
static int *afterConstraints;	/* "必须在后" 约束的链表头 */

/* ExpandConstraints 的输出区 */
static WAIT_ORDER *waitOrders;	/* 提议的队列重排数组 */
static int	nWaitOrders;
static PGPROC **waitOrderProcs; /* waitOrders 队列内容所用的空间 */

/* 当前正在考虑的约束列表 */
static EDGE *curConstraints;
static int	nCurConstraints;
static int	maxCurConstraints;

/* 存放 FindLockCycle 结果的空间 */
static EDGE *possibleConstraints;
static int	nPossibleConstraints;
static int	maxPossibleConstraints;
static DEADLOCK_INFO *deadlockDetails;
static int	nDeadlockDetails;

/* 已找到的、正在阻塞他人的自动清理工作进程的 PGPROC 指针 */
static PGPROC *blocking_autovacuum_proc = NULL;


/*
 * InitDeadLockChecking -- 在后端启动期间初始化死锁检测器
 *
 * 这里做死锁检测器的按后端初始化；主要是
 * 为 DeadLockCheck 分配工作内存。之所以按后端分别做，
 * 是因为让内核从 postmaster 以写时复制方式
 * 继承工作区并没有好处。我们希望在启动时分配
 * 这些空间，因为 (a) 死锁检测器可能在
 * 已经没有空闲内存时被调用，并且 (b) 检测器通常运行在
 * 信号处理函数里，而在那里调用 palloc 非常危险。
 */
void
InitDeadLockChecking(void)
{
	MemoryContext oldcxt;

	/* 确保这些分配是永久的 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * FindLockCycle 在 visitedProcs[] 与 deadlockDetails[] 中
	 * 最多需要 MaxBackends 个条目。
	 */
	visitedProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));
	deadlockDetails = (DEADLOCK_INFO *) palloc(MaxBackends * sizeof(DEADLOCK_INFO));

	/*
	 * TopoSort 最多需要考虑 MaxBackends 个等待队列条目，而且
	 * 它不必与 FindLockCycle 并发运行。
	 */
	topoProcs = visitedProcs;	/* 复用这块空间 */
	beforeConstraints = (int *) palloc(MaxBackends * sizeof(int));
	afterConstraints = (int *) palloc(MaxBackends * sizeof(int));

	/*
	 * 我们最多需要考虑重排 MaxBackends/2 个等待队列
	 * （因为队列里至少要有两个等待者才能形成一条软边），
	 * 而且展开后的等待队列所涉及的等待者总数
	 * 不会超过 MaxBackends。
	 */
	waitOrders = (WAIT_ORDER *)
		palloc((MaxBackends / 2) * sizeof(WAIT_ORDER));
	waitOrderProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));

	/*
	 * 一份配置里最多允许 MaxBackends 条不同的约束。（这
	 * 够用吗？实践中看起来应该够，但我不太清楚
	 * 如何证明。如果空间用尽，即便存在可行的等待
	 * 队列重排，我们也可能找不到。）注意，这个数目
	 * 限制了 DeadLockCheckRecurse 的最大递归深度。把它
	 * 定得非常大，有可能带来栈溢出问题。
	 */
	maxCurConstraints = MaxBackends;
	curConstraints = (EDGE *) palloc(maxCurConstraints * sizeof(EDGE));

	/*
	 * 最多允许保存 3*MaxBackends 条约束，这样就不必
	 * 重新运行 TestConfiguration。（这大概绰绰有余，但即便
	 * 空间紧张，我们也可以靠额外多跑几次
	 * TestConfiguration，在每次需要时重算约束列表来撑过去。）
	 * possibleConstraints[] 里最后的 MaxBackends 个条目保留作
	 * FindLockCycle 的输出工作区。
	 */
	maxPossibleConstraints = MaxBackends * 4;
	possibleConstraints =
		(EDGE *) palloc(maxPossibleConstraints * sizeof(EDGE));

	MemoryContextSwitchTo(oldcxt);
}

/*
 * DeadLockCheck -- 检查给定进程是否陷入死锁
 *
 * 这段代码查找涉及给定进程的死锁。如果找到了
 * 任何死锁，就尝试重排锁等待队列来解除
 * 死锁。若无法解除，则返回 DS_HARD_DEADLOCK ---
 * 调用方随后应当中止给定进程的事务。
 *
 * 调用方必须已经锁住锁表的全部分区。
 *
 * 失败时，把死锁细节记入 deadlockDetails[]，供
 * 随后的 DeadLockReport() 打印。这项工作单独进行，
 * 因为 (a) 我们不想在持有所有这些 LWLocks 时做这件事，
 * 并且 (b) 我们通常是在信号处理函数内部被调用的。
 */
DeadLockState
DeadLockCheck(PGPROC *proc)
{
	/* 初始化为 "无约束" */
	nCurConstraints = 0;
	nPossibleConstraints = 0;
	nWaitOrders = 0;

	/* 初始化为未被自动清理工作进程阻塞 */
	blocking_autovacuum_proc = NULL;

	/* 查找死锁以及可能的修复 */
	if (DeadLockCheckRecurse(proc))
	{
		/*
		 * 再调用一次 FindLockCycle，以便记下正确的
		 * deadlockDetails[]，对应没有任何重排的基本状态。
		 */
		int			nSoftEdges;

		TRACE_POSTGRESQL_DEADLOCK_FOUND();

		nWaitOrders = 0;
		if (!FindLockCycle(proc, possibleConstraints, &nSoftEdges))
			elog(FATAL, "deadlock seems to have disappeared");

		return DS_HARD_DEADLOCK;	/* 无法找到未陷入死锁的状态 */
	}

	/* 实施等待队列上任何必要的重排 */
	for (int i = 0; i < nWaitOrders; i++)
	{
		LOCK	   *lock = waitOrders[i].lock;
		PGPROC	  **procs = waitOrders[i].procs;
		int			nProcs = waitOrders[i].nProcs;
		dclist_head *waitQueue = &lock->waitProcs;

		Assert(nProcs == dclist_count(waitQueue));

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "DeadLockCheck:");
#endif

		/* 重置队列，并按期望顺序重新加入各进程 */
		dclist_init(waitQueue);
		for (int j = 0; j < nProcs; j++)
			dclist_push_tail(waitQueue, &procs[j]->links);

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "rearranged to:");
#endif

		/* 看看现在是否有该锁的等待者可以被唤醒 */
		ProcLockWakeup(GetLocksMethodTable(lock), lock);
	}

	/* 返回码说明我们是否不得不摆脱一次死锁 */
	if (nWaitOrders > 0)
		return DS_SOFT_DEADLOCK;
	else if (blocking_autovacuum_proc != NULL)
		return DS_BLOCKED_BY_AUTOVACUUM;
	else
		return DS_NO_DEADLOCK;
}

/*
 * 返回正在阻塞某个进程的自动清理进程的 PGPROC。
 *
 * 指针一经交还，就把保存的那个指针清掉。
 */
PGPROC *
GetBlockingAutoVacuumPgproc(void)
{
	PGPROC	   *ptr;

	ptr = blocking_autovacuum_proc;
	blocking_autovacuum_proc = NULL;

	return ptr;
}

/*
 * DeadLockCheckRecurse -- 递归搜索有效的顺序
 *
 * curConstraints[] 保存外层递归正在考虑的
 * 当前约束集合。把本层检测到的任何环所对应的
 * 每一种可能解约束都加进这个集合。
 *
 * 若不存在解则返回 true。若能够达到无死锁
 * 状态则返回 false，此时 waitOrders[] 给出所需的
 * 锁等待队列重排（如果有的话）。
 */
static bool
DeadLockCheckRecurse(PGPROC *proc)
{
	int			nEdges;
	int			oldPossibleConstraints;
	bool		savedList;
	int			i;

	nEdges = TestConfiguration(proc);
	if (nEdges < 0)
		return true;			/* 硬死锁 --- 无解 */
	if (nEdges == 0)
		return false;			/* 找到了可行配置 */
	if (nCurConstraints >= maxCurConstraints)
		return true;			/* 活动约束的空间不够了？ */
	oldPossibleConstraints = nPossibleConstraints;
	if (nPossibleConstraints + nEdges + MaxBackends <= maxPossibleConstraints)
	{
		/* 可以把边列表保存在 possibleConstraints[] 中 */
		nPossibleConstraints += nEdges;
		savedList = true;
	}
	else
	{
		/* 空间不够；需要当场重新生成这些边 */
		savedList = false;
	}

	/*
	 * 把每条可用的软边依次作为配置的新增部分来尝试。
	 */
	for (i = 0; i < nEdges; i++)
	{
		if (!savedList && i > 0)
		{
			/* 重新生成可加入的约束列表 */
			if (nEdges != TestConfiguration(proc))
				elog(FATAL, "inconsistent results during deadlock check");
		}
		curConstraints[nCurConstraints] =
			possibleConstraints[oldPossibleConstraints + i];
		nCurConstraints++;
		if (!DeadLockCheckRecurse(proc))
			return false;		/* 找到了一个有效解！ */
		/* 放弃这条新加的约束，改为重试 */
		nCurConstraints--;
	}
	nPossibleConstraints = oldPossibleConstraints;
	return true;				/* 没有找到解 */
}


/*--------------------
 * 检验一份配置（当前约束集合）是否有效。
 *
 * 返回值：
 *		0：配置良好（无死锁）
 *	   -1：配置存在硬死锁，或者并不自洽
 *		>0：配置存在一处或多处软死锁
 *
 * 若是软死锁，则任意选取其中一个软环，
 * 并返回该环的软边列表，列表起点为
 * possibleConstraints+nPossibleConstraints。返回值是
 * 软边的条数。
 *--------------------
 */
static int
TestConfiguration(PGPROC *startProc)
{
	int			softFound = 0;
	EDGE	   *softEdges = possibleConstraints + nPossibleConstraints;
	int			nSoftEdges;
	int			i;

	/*
	 * 确保有空间存放 FindLockCycle 的输出。
	 */
	if (nPossibleConstraints + MaxBackends > maxPossibleConstraints)
		return -1;

	/*
	 * 把当前约束集合展开成等待顺序。若约束集合
	 * 并不自洽，则失败。
	 */
	if (!ExpandConstraints(curConstraints, nCurConstraints))
		return -1;

	/*
	 * 检查涉及 startProc 或约束中所提到的任何进程的环。
	 * 我们最后才检查 startProc，因为如果它还有软环
	 * 有待处理，我们希望先处理那个。
	 */
	for (i = 0; i < nCurConstraints; i++)
	{
		if (FindLockCycle(curConstraints[i].waiter, softEdges, &nSoftEdges))
		{
			if (nSoftEdges == 0)
				return -1;		/* 检测到硬死锁 */
			softFound = nSoftEdges;
		}
		if (FindLockCycle(curConstraints[i].blocker, softEdges, &nSoftEdges))
		{
			if (nSoftEdges == 0)
				return -1;		/* 检测到硬死锁 */
			softFound = nSoftEdges;
		}
	}
	if (FindLockCycle(startProc, softEdges, &nSoftEdges))
	{
		if (nSoftEdges == 0)
			return -1;			/* 检测到硬死锁 */
		softFound = nSoftEdges;
	}
	return softFound;
}


/*
 * FindLockCycle -- 死锁环的基本检查
 *
 * 从给定进程向外扫描，看等待图中是否有一个
 * 包含该进程的环。若找到环则返回 true，
 * 否则返回 false。若找到环，我们返回环中
 * 包含的 "软边" 列表（如果有）。这些边有可能
 * 通过重排等待队列来消除。我们还会把
 * 检测到的环的信息填入 deadlockDetails[]；这些信息
 * 并不供死锁算法本身使用，只用于在失败后打印一条有用的
 * 消息。
 *
 * 因为我们需要能够检查等待队列重排之后才会
 * 出现的假设配置，本函数会留意
 * waitOrders[] 里的假设队列顺序表。这些顺序
 * 优先于在锁表中看到的实际顺序。
 */
static bool
FindLockCycle(PGPROC *checkProc,
			  EDGE *softEdges,	/* 输出参数 */
			  int *nSoftEdges)	/* 输出参数 */
{
	nVisitedProcs = 0;
	nDeadlockDetails = 0;
	*nSoftEdges = 0;
	return FindLockCycleRecurse(checkProc, 0, softEdges, nSoftEdges);
}

static bool
FindLockCycleRecurse(PGPROC *checkProc,
					 int depth,
					 EDGE *softEdges,	/* 输出参数 */
					 int *nSoftEdges)	/* 输出参数 */
{
	int			i;
	dlist_iter	iter;

	/*
	 * 若该进程是锁组成员，则改为检查组长。（注意
	 * 我们自己可能就是组长，那样的话这是一次空操作。）
	 */
	if (checkProc->lockGroupLeader != NULL)
		checkProc = checkProc->lockGroupLeader;

	/*
	 * 我们是否已经见过这个进程？
	 */
	for (i = 0; i < nVisitedProcs; i++)
	{
		if (visitedProcs[i] == checkProc)
		{
			/* 若回到起点，就存在一个死锁环 */
			if (i == 0)
			{
				/*
				 * 记下环的总长度 --- 外层现在会去填写
				 * deadlockDetails[]
				 */
				Assert(depth <= MaxBackends);
				nDeadlockDetails = depth;

				return true;
			}

			/*
			 * 否则，虽然有环，但环并不包含起点，
			 * 因此报告 "无死锁"。
			 */
			return false;
		}
	}
	/* 把该进程标为已见 */
	Assert(nVisitedProcs < MaxBackends);
	visitedProcs[nVisitedProcs++] = checkProc;

	/*
	 * 若该进程正在等待，就有一条向外的等待边，指向每一个
	 * 阻塞它的进程。
	 */
	if (checkProc->links.next != NULL && checkProc->waitLock != NULL &&
		FindLockCycleRecurseMember(checkProc, checkProc, depth, softEdges,
								   nSoftEdges))
		return true;

	/*
	 * 即便该进程并不在等待，仍可能有向外的等待边，
	 * 只要它属于某个锁组，因为锁组的其他成员
	 * 也许正在等待，尽管这个进程没有。（给定锁组
	 * {A1, A2} 与 {B1, B2}，若 A1 等待 B1 且 B2 等待 A2，
	 * 即便 B1 和 A2 都没有在等待任何东西，这也是死锁。）
	 */
	dlist_foreach(iter, &checkProc->lockGroupMembers)
	{
		PGPROC	   *memberProc;

		memberProc = dlist_container(PGPROC, lockGroupLink, iter.cur);

		if (memberProc->links.next != NULL && memberProc->waitLock != NULL &&
			memberProc != checkProc &&
			FindLockCycleRecurseMember(memberProc, checkProc, depth, softEdges,
									   nSoftEdges))
			return true;
	}

	return false;
}

static bool
FindLockCycleRecurseMember(PGPROC *checkProc,
						   PGPROC *checkProcLeader,
						   int depth,
						   EDGE *softEdges, /* 输出参数 */
						   int *nSoftEdges) /* 输出参数 */
{
	PGPROC	   *proc;
	LOCK	   *lock = checkProc->waitLock;
	dlist_iter	proclock_iter;
	LockMethod	lockMethodTable;
	int			conflictMask;
	int			i;
	int			numLockModes,
				lm;

	/*
	 * 关系扩展锁永远不会参与真正的死锁
	 * 环。见 LockAcquireExtended 中的 Assert。因此，
	 * 检查从它出发的等待边并没有好处。
	 */
	if (LOCK_LOCKTAG(*lock) == LOCKTAG_RELATION_EXTEND)
		return false;

	lockMethodTable = GetLocksMethodTable(lock);
	numLockModes = lockMethodTable->numLockModes;
	conflictMask = lockMethodTable->conflictTab[checkProc->waitLockMode];

	/*
	 * 扫描已经持有冲突锁的进程。这些是等待图中的
	 * 硬边。
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		PROCLOCK   *proclock = dlist_container(PROCLOCK, lockLink, proclock_iter.cur);
		PGPROC	   *leader;

		proc = proclock->tag.myProc;
		leader = proc->lockGroupLeader == NULL ? proc : proc->lockGroupLeader;

		/* 进程从不阻塞自己，也不阻塞同锁组的其他成员 */
		if (leader != checkProcLeader)
		{
			for (lm = 1; lm <= numLockModes; lm++)
			{
				if ((proclock->holdMask & LOCKBIT_ON(lm)) &&
					(conflictMask & LOCKBIT_ON(lm)))
				{
					/* 该进程硬阻塞 checkProc */
					if (FindLockCycleRecurse(proc, depth + 1,
											 softEdges, nSoftEdges))
					{
						/* 填写 deadlockDetails[] */
						DEADLOCK_INFO *info = &deadlockDetails[depth];

						info->locktag = lock->tag;
						info->lockmode = checkProc->waitLockMode;
						info->pid = checkProc->pid;

						return true;
					}

					/*
					 * 这里没有死锁，但要看看该进程是不是正在
					 * 直接硬阻塞我们自己进程的自动清理进程。若是，
					 * 就报告它，以便调用方在合适时向它发送取消信号。
					 * 若这样的进程不止一个，究竟报告哪一个
					 * 是不确定的。
					 *
					 * 我们不去碰那些只是间接阻塞我们的自动清理进程；
					 * 该由被直接阻塞的一方采取行动。这条
					 * 规则让行为更好理解，并确保
					 * 自动清理进程不会在短于
					 * deadlock_timeout 宽限期的情况下被取消。
					 *
					 * 注意，我们读取 statusFlags 时不加任何锁。这样做
					 * 只适用于检查 PROC_IS_AUTOVACUUM 标志，
					 * 因为该标志在进程启动时设置，此后从不
					 * 复位。别处有逻辑，避免取消正在防止
					 * XID 回绕问题的自动清理进程
					 * （这需要读取 statusFlags 的另一个位），
					 * 但这里不那么做，以免获取
					 * ProcArrayLock。
					 */
					if (checkProc == MyProc &&
						proc->statusFlags & PROC_IS_AUTOVACUUM)
						blocking_autovacuum_proc = proc;

					/* 这个 proclock 已经看完 */
					break;
				}
			}
		}
	}

	/*
	 * 扫描在该锁的等待队列中排在本进程之前的进程。
	 * 其中请求与之冲突的进程会软阻塞本进程。这一步必须
	 * 放在硬阻塞搜索之后，因为若另一个进程对本进程既硬阻塞
	 * 又软阻塞，我们要把它算作硬边。
	 *
	 * 若对该锁的等待顺序已有提议的重排，就采用那个顺序，
	 * 而不是当前的等待顺序。
	 */
	for (i = 0; i < nWaitOrders; i++)
	{
		if (waitOrders[i].lock == lock)
			break;
	}

	if (i < nWaitOrders)
	{
		/* 采用给定的假设等待队列顺序 */
		PGPROC	  **procs = waitOrders[i].procs;
		int			queue_size = waitOrders[i].nProcs;

		for (i = 0; i < queue_size; i++)
		{
			PGPROC	   *leader;

			proc = procs[i];
			leader = proc->lockGroupLeader == NULL ? proc :
				proc->lockGroupLeader;

			/*
			 * TopoSort 总会返回一种组成员在等待队列里
			 * 彼此相邻的顺序（见其中的
			 * 注释）。因此，一旦我们到达与 checkProc 属于同一锁
			 * 组的进程，就说明已经找齐了所有冲突，它们都
			 * 排在由 checkProcLeader 领导的锁组的任一成员之前。
			 */
			if (leader == checkProcLeader)
				break;

			/* 与这个进程的请求冲突吗？ */
			if ((LOCKBIT_ON(proc->waitLockMode) & conflictMask) != 0)
			{
				/* 该进程软阻塞 checkProc */
				if (FindLockCycleRecurse(proc, depth + 1,
										 softEdges, nSoftEdges))
				{
					/* 填写 deadlockDetails[] */
					DEADLOCK_INFO *info = &deadlockDetails[depth];

					info->locktag = lock->tag;
					info->lockmode = checkProc->waitLockMode;
					info->pid = checkProc->pid;

					/*
					 * 把这条边加入环中的软边列表
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProcLeader;
					softEdges[*nSoftEdges].blocker = leader;
					softEdges[*nSoftEdges].lock = lock;
					(*nSoftEdges)++;
					return true;
				}
			}
		}
	}
	else
	{
		PGPROC	   *lastGroupMember = NULL;
		dlist_iter	proc_iter;
		dclist_head *waitQueue;

		/* 采用真实的锁等待队列顺序 */
		waitQueue = &lock->waitProcs;

		/*
		 * 找出出现在等待队列中的、该锁组的最后一个成员。
		 * 排在它后面的都不是软锁冲突。若没有使用
		 * 组锁，我们立刻就知道要找的是哪个进程，
		 * 否则就必须搜索等待队列，
		 * 找出实际在场的最后一个进程。
		 */
		if (checkProc->lockGroupLeader == NULL)
			lastGroupMember = checkProc;
		else
		{
			dclist_foreach(proc_iter, waitQueue)
			{
				proc = dlist_container(PGPROC, links, proc_iter.cur);

				if (proc->lockGroupLeader == checkProcLeader)
					lastGroupMember = proc;
			}
			Assert(lastGroupMember != NULL);
		}

		/*
		 * 好，现在重新扫描（或扫描）队列，以识别软冲突。
		 */
		dclist_foreach(proc_iter, waitQueue)
		{
			PGPROC	   *leader;

			proc = dlist_container(PGPROC, links, proc_iter.cur);

			leader = proc->lockGroupLeader == NULL ? proc :
				proc->lockGroupLeader;

			/* 到达目标进程即可结束 */
			if (proc == lastGroupMember)
				break;

			/* 与这个进程的请求冲突吗？ */
			if ((LOCKBIT_ON(proc->waitLockMode) & conflictMask) != 0 &&
				leader != checkProcLeader)
			{
				/* 该进程软阻塞 checkProc */
				if (FindLockCycleRecurse(proc, depth + 1,
										 softEdges, nSoftEdges))
				{
					/* 填写 deadlockDetails[] */
					DEADLOCK_INFO *info = &deadlockDetails[depth];

					info->locktag = lock->tag;
					info->lockmode = checkProc->waitLockMode;
					info->pid = checkProc->pid;

					/*
					 * 把这条边加入环中的软边列表
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProcLeader;
					softEdges[*nSoftEdges].blocker = leader;
					softEdges[*nSoftEdges].lock = lock;
					(*nSoftEdges)++;
					return true;
				}
			}
		}
	}

	/*
	 * 这里没有检测到冲突。
	 */
	return false;
}


/*
 * ExpandConstraints -- 把约束列表展开成一组
 *		受影响等待队列的具体新顺序
 *
 * 输入是一组需要反过来的软边。输出是
 * waitOrders[] 中的 nWaitOrders 个 WAIT_ORDER 结构，PGPROC 数组
 * 工作区在 waitOrderProcs[] 中。
 *
 * 若能构造出满足全部约束的顺序则返回 true，
 * 否则返回 false（存在互相矛盾的约束）。
 */
static bool
ExpandConstraints(EDGE *constraints,
				  int nConstraints)
{
	int			nWaitOrderProcs = 0;
	int			i,
				j;

	nWaitOrders = 0;

	/*
	 * 反向扫描约束列表。这是因为最后加入的
	 * 那条约束是唯一可能失败的，所以我们想先检验它
	 * 是否不一致。
	 */
	for (i = nConstraints; --i >= 0;)
	{
		LOCK	   *lock = constraints[i].lock;

		/* 是否已经为这把锁建过列表？ */
		for (j = nWaitOrders; --j >= 0;)
		{
			if (waitOrders[j].lock == lock)
				break;
		}
		if (j >= 0)
			continue;
		/* 没有，因此分配一个新列表 */
		waitOrders[nWaitOrders].lock = lock;
		waitOrders[nWaitOrders].procs = waitOrderProcs + nWaitOrderProcs;
		waitOrders[nWaitOrders].nProcs = dclist_count(&lock->waitProcs);
		nWaitOrderProcs += dclist_count(&lock->waitProcs);
		Assert(nWaitOrderProcs <= MaxBackends);

		/*
		 * 做拓扑排序。TopoSort 不必检查这条之后的约束，
		 * 因为它们必定属于不同的锁。
		 */
		if (!TopoSort(lock, constraints, i + 1,
					  waitOrders[nWaitOrders].procs))
			return false;
		nWaitOrders++;
	}
	return true;
}


/*
 * TopoSort -- 对等待队列做拓扑排序
 *
 * 生成该锁等待队列的一种重排，使之满足给定的
 * 关于某些进程必须排在另一些进程之前的约束。（每一条这样的约束
 * 都是偏序中的一个事实。）尽量减少为实现该偏序
 * 所不需要的队列重排。
 *
 * 这比 Knuth 第 1 卷里给出的拓扑排序
 * 算法简单得多，也慢得多。不过 Knuth 的方法并不
 * 试图把对现有顺序的破坏减到最小。实践中我们
 * 不大可能要处理超过几条约束，因此算法表面上的
 * 缓慢其实并不要紧。
 *
 * 初始队列顺序直接取自该锁的等待队列。
 * 输出是一个 PGPROC 指针数组，长度等于该锁的
 * 等待队列长度（由调用方负责提供这块空间）。
 * 偏序由一个 EDGE 结构数组给出。每个 EDGE
 * 都是我们需要反过来的一条边，因此输出数组里 "waiter" 必须出现在
 * "blocker" 之前。EDGE 数组里完全可能含有
 * 与其他锁相关的边；这些应当忽略。
 *
 * 若能构造出满足全部
 * 约束的顺序则返回 true，否则返回 false（存在互相矛盾的约束）。
 */
static bool
TopoSort(LOCK *lock,
		 EDGE *constraints,
		 int nConstraints,
		 PGPROC **ordering)		/* 输出参数 */
{
	dclist_head *waitQueue = &lock->waitProcs;
	int			queue_size = dclist_count(waitQueue);
	PGPROC	   *proc;
	int			i,
				j,
				jj,
				k,
				kk,
				last;
	dlist_iter	proc_iter;

	/* 首先按当前顺序把各进程填入 topoProcs[] */
	i = 0;
	dclist_foreach(proc_iter, waitQueue)
	{
		proc = dlist_container(PGPROC, links, proc_iter.cur);
		topoProcs[i++] = proc;
	}
	Assert(i == queue_size);

	/*
	 * 扫描这些约束，并对数组中的每个进程生成一个计数，
	 * 表示有多少条约束说它必须排在别的进程之前，
	 * 外加一个链表，列出那些说它必须排在别的进程
	 * 之后的约束。第 j 个进程的计数存在 beforeConstraints[j]，
	 * 其链表头存在 afterConstraints[j]。每条约束
	 * 把自己的链表链接存在 constraints[i].link 里（注意任何约束都只
	 * 会出现在一个链表中）。第 i 条约束里那个必须在前的进程，
	 * 其数组下标记在 constraints[i].pred。
	 *
	 * 注意，并非每条约束都影响
	 * 这一个特定的等待队列。在有组锁之前，一个进程最多
	 * 只能等待一把锁。但一个锁组可以等待
	 * 零把、一把或多把锁。由于 topoProcs[] 是实际正在等待的
	 * 进程数组，而 constraints[] 是组长
	 * 数组，我们对每条约束都必须扫描 topoProcs[]，
	 * 检查该组的 waiter 和 blocker 是否都
	 * 在场。若是，这条约束就与本等待队列有关；若否，
	 * 则无关。
	 */
	MemSet(beforeConstraints, 0, queue_size * sizeof(int));
	MemSet(afterConstraints, 0, queue_size * sizeof(int));
	for (i = 0; i < nConstraints; i++)
	{
		/*
		 * 找一个既在该锁队列上、又属于
		 * 正在等待的锁组的代表进程。它可能是组长，也可能不是，
		 * 而组长也许根本没在等待。若队列上还有同一锁组里的
		 * 其他进程，就把它们的 beforeConstraints 个数
		 * 设为 -1，表示它们应当与同组成员一起输出，
		 * 而不是被分开考虑。
		 *
		 * 在这个循环以及下面那个类似循环中，关键是我们
		 * 对同一个锁组始终选择同一个代表成员，
		 * 从而使全部约束都关联到同一个进程，
		 * 而 -1 只关联到非代表成员。
		 * 我们选择 topoProcs 数组中的最后一个。
		 */
		proc = constraints[i].waiter;
		Assert(proc != NULL);
		jj = -1;
		for (j = queue_size; --j >= 0;)
		{
			PGPROC	   *waiter = topoProcs[j];

			if (waiter == proc || waiter->lockGroupLeader == proc)
			{
				Assert(waiter->waitLock == lock);
				if (jj == -1)
					jj = j;
				else
				{
					Assert(beforeConstraints[j] <= 0);
					beforeConstraints[j] = -1;
				}
			}
		}

		/* 没有匹配的 waiter，则该约束与这把锁无关。 */
		if (jj < 0)
			continue;

		/*
		 * 同样，找一个既在该锁队列上、又属于
		 * 阻塞方锁组的代表进程。同样，这可以是
		 * 组长，但并不必须是。
		 */
		proc = constraints[i].blocker;
		Assert(proc != NULL);
		kk = -1;
		for (k = queue_size; --k >= 0;)
		{
			PGPROC	   *blocker = topoProcs[k];

			if (blocker == proc || blocker->lockGroupLeader == proc)
			{
				Assert(blocker->waitLock == lock);
				if (kk == -1)
					kk = k;
				else
				{
					Assert(beforeConstraints[k] <= 0);
					beforeConstraints[k] = -1;
				}
			}
		}

		/* 没有匹配的 blocker，则该约束与这把锁无关。 */
		if (kk < 0)
			continue;

		Assert(beforeConstraints[jj] >= 0);
		beforeConstraints[jj]++;	/* waiter 必须排在前面 */
		/* 把这条约束加入 blocker 的 "必须在后" 约束链表 */
		constraints[i].pred = jj;
		constraints[i].link = afterConstraints[kk];
		afterConstraints[kk] = i + 1;
	}

	/*--------------------
	 * 现在反向扫描 topoProcs 数组。每一步都输出
	 * 最后一个已经没有剩余 "必须在前" 约束的进程，以及同一锁组的
	 * 其他成员；然后对与它有约束关系的每个进程，把 beforeConstraints
	 * 计数减一。
	 * i = 这次要输出的 ordering[] 条目的下标
	 * j = 在 topoProcs[] 中搜索用的下标
	 * k = 扫描进程 j 的约束链表时用的临时变量
	 * last = topoProcs 中最后一个非空下标（避免重复搜索）
	 *--------------------
	 */
	last = queue_size - 1;
	for (i = queue_size - 1; i >= 0;)
	{
		int			c;
		int			nmatches = 0;

		/* 找出下一个待输出的候选 */
		while (topoProcs[last] == NULL)
			last--;
		for (j = last; j >= 0; j--)
		{
			if (topoProcs[j] != NULL && beforeConstraints[j] == 0)
				break;
		}

		/* 没有可用候选，拓扑排序失败 */
		if (j < 0)
			return false;

		/*
		 * 把整个锁组都输出。没有必要
		 * 输出一种同一锁组的成员在等待队列上不
		 * 连续的顺序：若另一个等待者夹在属于同一组的两个
		 * 请求之间，那么它要么与两者都冲突，
		 * 那就肯定不是解；要么至多与其中一个冲突，
		 * 于是就与组成员连续排列的某种顺序
		 * 同构。
		 */
		proc = topoProcs[j];
		if (proc->lockGroupLeader != NULL)
			proc = proc->lockGroupLeader;
		Assert(proc != NULL);
		for (c = 0; c <= last; ++c)
		{
			if (topoProcs[c] == proc || (topoProcs[c] != NULL &&
										 topoProcs[c]->lockGroupLeader == proc))
			{
				ordering[i - nmatches] = topoProcs[c];
				topoProcs[c] = NULL;
				++nmatches;
			}
		}
		Assert(nmatches > 0);
		i -= nmatches;

		/* 更新其前驱的 beforeConstraints 计数 */
		for (k = afterConstraints[j]; k > 0; k = constraints[k - 1].link)
			beforeConstraints[constraints[k - 1].pred]--;
	}

	/* 完成 */
	return true;
}

#ifdef DEBUG_DEADLOCK
static void
PrintLockQueue(LOCK *lock, const char *info)
{
	dclist_head *waitQueue = &lock->waitProcs;
	dlist_iter	proc_iter;

	printf("%s lock %p queue ", info, lock);

	dclist_foreach(proc_iter, waitQueue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, links, proc_iter.cur);

		printf(" %d", proc->pid);
	}
	printf("\n");
	fflush(stdout);
}
#endif

/*
 * 报告已检测到的死锁，并给出已有的细节。
 */
void
DeadLockReport(void)
{
	StringInfoData clientbuf;	/* 发给客户端的 errdetail */
	StringInfoData logbuf;		/* 写入服务器日志的 errdetail */
	StringInfoData locktagbuf;
	int			i;

	initStringInfo(&clientbuf);
	initStringInfo(&logbuf);
	initStringInfo(&locktagbuf);

	/* 生成发给客户端的 "等待" 行 */
	for (i = 0; i < nDeadlockDetails; i++)
	{
		DEADLOCK_INFO *info = &deadlockDetails[i];
		int			nextpid;

		/* 最后一个进程等待第一个进程... */
		if (i < nDeadlockDetails - 1)
			nextpid = info[1].pid;
		else
			nextpid = deadlockDetails[0].pid;

		/* 重置 locktagbuf，以存放下一个对象描述 */
		resetStringInfo(&locktagbuf);

		DescribeLockTag(&locktagbuf, &info->locktag);

		if (i > 0)
			appendStringInfoChar(&clientbuf, '\n');

		appendStringInfo(&clientbuf,
						 _("Process %d waits for %s on %s; blocked by process %d."),
						 info->pid,
						 GetLockmodeName(info->locktag.locktag_lockmethodid,
										 info->lockmode),
						 locktagbuf.data,
						 nextpid);
	}

	/* 把上面的内容全部复制给服务器日志 ... */
	appendBinaryStringInfo(&logbuf, clientbuf.data, clientbuf.len);

	/* ... 并补上查询字符串的信息 */
	for (i = 0; i < nDeadlockDetails; i++)
	{
		DEADLOCK_INFO *info = &deadlockDetails[i];

		appendStringInfoChar(&logbuf, '\n');

		appendStringInfo(&logbuf,
						 _("Process %d: %s"),
						 info->pid,
						 pgstat_get_backend_current_activity(info->pid, false));
	}

	pgstat_report_deadlock();

	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("deadlock detected"),
			 errdetail_internal("%s", clientbuf.data),
			 errdetail_log("%s", logbuf.data),
			 errhint("See server log for query details.")));
}

/*
 * RememberSimpleDeadLock：当 ProcSleep
 * 检测到简单的（双向）死锁时，为 DeadLockReport 准备信息。proc1 想以 lockmode
 * 在 lock 上阻塞等待，但 proc2 已在等待，并且会被 proc1 阻塞。
 */
void
RememberSimpleDeadLock(PGPROC *proc1,
					   LOCKMODE lockmode,
					   LOCK *lock,
					   PGPROC *proc2)
{
	DEADLOCK_INFO *info = &deadlockDetails[0];

	info->locktag = lock->tag;
	info->lockmode = lockmode;
	info->pid = proc1->pid;
	info++;
	info->locktag = proc2->waitLock->tag;
	info->lockmode = proc2->waitLockMode;
	info->pid = proc2->pid;
	nDeadlockDetails = 2;
}
