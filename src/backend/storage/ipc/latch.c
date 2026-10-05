/*-------------------------------------------------------------------------
 *
 * latch.c
 *	  Routines for inter-process latches
 *
 * The poll() implementation uses the so-called self-pipe trick to overcome the
 * race condition involved with poll() and setting a global flag in the signal
 * handler. When a latch is set and the current process is waiting for it, the
 * signal handler wakes up the poll() in WaitLatch by writing a byte to a pipe.
 * A signal by itself doesn't interrupt poll() on all platforms, and even on
 * platforms where it does, a signal that arrives just before the poll() call
 * does not prevent poll() from entering sleep. An incoming byte on a pipe
 * however reliably interrupts the sleep, and causes poll() to return
 * immediately even if the signal arrives before poll() begins.
 *
 * The epoll() implementation overcomes the race with a different technique: it
 * keeps SIGURG blocked and consumes from a signalfd() descriptor instead.  We
 * don't need to register a signal handler or create our own self-pipe.  We
 * assume that any system that has Linux epoll() also has Linux signalfd().
 *
 * The kqueue() implementation waits for SIGURG with EVFILT_SIGNAL.
 *
 * The Windows implementation uses Windows events that are inherited by all
 * postmaster child processes. There's no need for the self-pipe trick there.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/latch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#ifdef HAVE_SYS_EPOLL_H
#include <sys/epoll.h>
#endif
#ifdef HAVE_SYS_EVENT_H
#include <sys/event.h>
#endif
#ifdef HAVE_SYS_SIGNALFD_H
#include <sys/signalfd.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/memutils.h"

/*
 * 选择要使用的文件描述符就绪原语。通常会用操作系统
 * 所支持的最现代原语，但为了测试，手动指定
 * 所用原语会有用。若想这么做，只要在这个块之前
 * 某处加一个 define。
 */
#if defined(WAIT_USE_EPOLL) || defined(WAIT_USE_POLL) || \
	defined(WAIT_USE_KQUEUE) || defined(WAIT_USE_WIN32)
/* 不要覆盖手动选择 */
#elif defined(HAVE_SYS_EPOLL_H)
#define WAIT_USE_EPOLL
#elif defined(HAVE_KQUEUE)
#define WAIT_USE_KQUEUE
#elif defined(HAVE_POLL)
#define WAIT_USE_POLL
#elif WIN32
#define WAIT_USE_WIN32
#else
#error "no wait set implementation available"
#endif

/*
 * 默认情况下，若可用，我们对 poll() 使用自管道，对 epoll()
 * 使用 signalfd。基于问题报告，目前在 illumos 上避免 signalfd。
 * 为了测试，这个选择也可以手动指定。
 */
#if defined(WAIT_USE_POLL) || defined(WAIT_USE_EPOLL)
#if defined(WAIT_USE_SELF_PIPE) || defined(WAIT_USE_SIGNALFD)
/* 不要覆盖手动选择 */
#elif defined(WAIT_USE_EPOLL) && defined(HAVE_SYS_SIGNALFD_H) && \
	!defined(__illumos__)
#define WAIT_USE_SIGNALFD
#else
#define WAIT_USE_SELF_PIPE
#endif
#endif

/* 锁存器头文件 latch.h 中的 typedef */
struct WaitEventSet
{
	int			nevents;		/* 已登记事件的数量 */
	int			nevents_space;	/* 本集合中事件的最大数量 */

	/*
	 * 长度为 nevents_space 的数组，存放本集合正在等待的
	 * 事件定义。
	 */
	WaitEvent  *events;

	/*
	 * 若任一等待事件里指定了 WL_LATCH_SET，锁存器就是指向
	 * 所说锁存器的指针，latch_pos 则是 ->events 数组中的偏移。
	 * 这很有用，因为在做与等待有关的系统调用之前，
	 * 我们会先检查锁存器的状态。
	 */
	Latch	   *latch;
	int			latch_pos;

	/*
	 * WL_EXIT_ON_PM_DEATH 被转换成 WL_POSTMASTER_DEATH，但会设置
	 * 这个标志，以便一旦发现 postmaster 死亡就立即退出，
	 * 而不是返回。
	 */
	bool		exit_on_postmaster_death;

#if defined(WAIT_USE_EPOLL)
	int			epoll_fd;
	/* epoll_wait 把事件返回到用户提供的数组里，分配一次 */
	struct epoll_event *epoll_ret_events;
#elif defined(WAIT_USE_KQUEUE)
	int			kqueue_fd;
	/* kevent 把事件返回到用户提供的数组里，分配一次 */
	struct kevent *kqueue_ret_events;
	bool		report_postmaster_not_running;
#elif defined(WAIT_USE_POLL)
	/* poll 预期每次 poll() 调用都要给出待等事件，预先准备一次 */
	struct pollfd *pollfds;
#elif defined(WAIT_USE_WIN32)

	/*
	 * Windows 事件数组。第一个元素始终是
	 * pgwin32_signal_event，因此其余元素偏移一（即
	 * event->pos + 1）。
	 */
	HANDLE	   *handles;
#endif
};

/* 用来实现 WaitLatch() 的一个共用 WaitEventSet */
static WaitEventSet *LatchWaitSet;

/* 锁存器在 LatchWaitSet 中的位置。 */
#define LatchWaitSetLatchPos 0

#ifndef WIN32
/* 我们当前是否在 WaitLatch 里？信号处理函数想知道。 */
static volatile sig_atomic_t waiting = false;
#endif

#ifdef WAIT_USE_SIGNALFD
/* 在 Linux 上，我们将经 signalfd 文件描述符收到 SIGURG。 */
static int	signal_fd = -1;
#endif

#ifdef WAIT_USE_SELF_PIPE
/* 自管道的读端和写端 */
static int	selfpipe_readfd = -1;
static int	selfpipe_writefd = -1;

/* 拥有自管道的进程，检查时需要 */
static int	selfpipe_owner_pid = 0;

/* 私有函数原型 */
static void latch_sigurg_handler(SIGNAL_ARGS);
static void sendSelfPipeByte(void);
#endif

#if defined(WAIT_USE_SELF_PIPE) || defined(WAIT_USE_SIGNALFD)
static void drain(void);
#endif

#if defined(WAIT_USE_EPOLL)
static void WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event, int action);
#elif defined(WAIT_USE_KQUEUE)
static void WaitEventAdjustKqueue(WaitEventSet *set, WaitEvent *event, int old_events);
#elif defined(WAIT_USE_POLL)
static void WaitEventAdjustPoll(WaitEventSet *set, WaitEvent *event);
#elif defined(WAIT_USE_WIN32)
static void WaitEventAdjustWin32(WaitEventSet *set, WaitEvent *event);
#endif

static inline int WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
										WaitEvent *occurred_events, int nevents);

/*
 * 初始化进程本地的锁存器基础设施。
 *
 * 任何可能在锁存器上等待的进程，都必须在启动时调用一次，
 * 并且要在发出任何 InitLatch() 或 OwnLatch() 调用之前。
 */
void
InitializeLatchSupport(void)
{
#if defined(WAIT_USE_SELF_PIPE)
	int			pipefd[2];

	if (IsUnderPostmaster)
	{
		/*
		 * 我们可能继承了 postmaster 创建的自管道连接。
		 * 子进程当然必须创建自己的自管道，而且为了安全，
		 * 我们非常希望它们关掉继承来的
		 * 文件描述符。
		 */
		if (selfpipe_owner_pid != 0)
		{
			/* 断言子进程里只经过这里一次 */
			Assert(selfpipe_owner_pid != MyProcPid);
			/* 释放 postmaster 的管道文件描述符；忽略任何错误 */
			(void) close(selfpipe_readfd);
			(void) close(selfpipe_writefd);
			/* 清理一下，只为安全；下面会重新设置这些 */
			selfpipe_readfd = selfpipe_writefd = -1;
			selfpipe_owner_pid = 0;
			/* 让 fd.c 的记账保持正确 */
			ReleaseExternalFD();
			ReleaseExternalFD();
		}
		else
		{
			/*
			 * postmaster 没有创建自管道……或者我们处于
			 * EXEC_BACKEND 构建中，那种情况下也没关系，因为
			 * postmaster 的管道文件描述符已被 FD_CLOEXEC 的作用关掉。
			 * fd.c 也不会有需要清理的状态。
			 */
			Assert(selfpipe_readfd == -1);
		}
	}
	else
	{
		/* 在 postmaster 或独立后端里，断言只做一次 */
		Assert(selfpipe_readfd == -1);
		Assert(selfpipe_owner_pid == 0);
	}

	/*
	 * 建立自管道，使信号处理函数能唤醒
	 * WaitLatch 里的 poll()/epoll_wait()。把写端设成非阻塞，这样
	 * 若事件已被设置多次、填满了内核缓冲区，SetLatch 就不会阻塞。
	 * 读端也设成非阻塞，这样我们可以读到 EAGAIN 或 EWOULDBLOCK
	 * 为止，轻松清空管道。
	 * 另外，两个文件描述符都设成执行时关闭，因为我们
	 * 绝不希望子进程去动它们。
	 */
	if (pipe(pipefd) < 0)
		elog(FATAL, "pipe() failed: %m");
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == -1)
		elog(FATAL, "fcntl(F_SETFL) failed on read-end of self-pipe: %m");
	if (fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1)
		elog(FATAL, "fcntl(F_SETFL) failed on write-end of self-pipe: %m");
	if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1)
		elog(FATAL, "fcntl(F_SETFD) failed on read-end of self-pipe: %m");
	if (fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1)
		elog(FATAL, "fcntl(F_SETFD) failed on write-end of self-pipe: %m");

	selfpipe_readfd = pipefd[0];
	selfpipe_writefd = pipefd[1];
	selfpipe_owner_pid = MyProcPid;

	/* 把这两个长寿文件描述符告诉 fd.c */
	ReserveExternalFD();
	ReserveExternalFD();

	pqsignal(SIGURG, latch_sigurg_handler);
#endif

#ifdef WAIT_USE_SIGNALFD
	sigset_t	signalfd_mask;

	if (IsUnderPostmaster)
	{
		/*
		 * 复用继承来的 signalfd 大概是安全的，因为
		 * signalfd 只看当前进程的待处理信号，但关掉它
		 * 再创建我们自己的，似乎更不令人意外。
		 */
		if (signal_fd != -1)
		{
			/* 释放 postmaster 的信号文件描述符；忽略任何错误 */
			(void) close(signal_fd);
			signal_fd = -1;
			ReleaseExternalFD();
		}
	}

	/* 阻塞 SIGURG，因为我们将经 signalfd 接收它。 */
	sigaddset(&UnBlockSig, SIGURG);

	/* 建立 signalfd 以接收 SIGURG 通知。 */
	sigemptyset(&signalfd_mask);
	sigaddset(&signalfd_mask, SIGURG);
	signal_fd = signalfd(-1, &signalfd_mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (signal_fd < 0)
		elog(FATAL, "signalfd() failed");
	ReserveExternalFD();
#endif

#ifdef WAIT_USE_KQUEUE
	/* 忽略 SIGURG，因为我们将经 kqueue 接收它。 */
	pqsignal(SIGURG, SIG_IGN);
#endif
}

void
InitializeLatchWaitSet(void)
{
	int			latch_pos PG_USED_FOR_ASSERTS_ONLY;

	Assert(LatchWaitSet == NULL);

	/* 建立 WaitLatch() 所用的 WaitEventSet。 */
	LatchWaitSet = CreateWaitEventSet(TopMemoryContext, 2);
	latch_pos = AddWaitEventToSet(LatchWaitSet, WL_LATCH_SET, PGINVALID_SOCKET,
								  MyLatch, NULL);
	if (IsUnderPostmaster)
		AddWaitEventToSet(LatchWaitSet, WL_EXIT_ON_PM_DEATH,
						  PGINVALID_SOCKET, NULL, NULL);

	Assert(latch_pos == LatchWaitSetLatchPos);
}

void
ShutdownLatchSupport(void)
{
#if defined(WAIT_USE_POLL)
	pqsignal(SIGURG, SIG_IGN);
#endif

	if (LatchWaitSet)
	{
		FreeWaitEventSet(LatchWaitSet);
		LatchWaitSet = NULL;
	}

#if defined(WAIT_USE_SELF_PIPE)
	close(selfpipe_readfd);
	close(selfpipe_writefd);
	selfpipe_readfd = -1;
	selfpipe_writefd = -1;
	selfpipe_owner_pid = InvalidPid;
#endif

#if defined(WAIT_USE_SIGNALFD)
	close(signal_fd);
	signal_fd = -1;
#endif
}

/*
 * 初始化一个进程本地锁存器。
 */
void
InitLatch(Latch *latch)
{
	latch->is_set = false;
	latch->maybe_sleeping = false;
	latch->owner_pid = MyProcPid;
	latch->is_shared = false;

#if defined(WAIT_USE_SELF_PIPE)
	/* 断言本进程已调用 InitializeLatchSupport */
	Assert(selfpipe_readfd >= 0 && selfpipe_owner_pid == MyProcPid);
#elif defined(WAIT_USE_SIGNALFD)
	/* 断言本进程已调用 InitializeLatchSupport */
	Assert(signal_fd >= 0);
#elif defined(WAIT_USE_WIN32)
	latch->event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (latch->event == NULL)
		elog(ERROR, "CreateEvent failed: error code %lu", GetLastError());
#endif							/* 平台宏 WIN32 */
}

/*
 * 初始化一个可由其他进程设置的共享锁存器。该锁存器
 * 最初不属于任何人；用 OwnLatch 把它关联到
 * 当前进程。
 *
 * InitSharedLatch 需要在 postmaster 派生子进程之前调用，
 * 通常就在用 ShmemInitStruct 分配包含该锁存器的
 * 共享内存块之后。（Unix 实现实际上并不
 * 以此为必须，但 Windows 实现以此为必须。）由于
 * 这一限制，这里不用担心并发问题。
 *
 * 注意，本模块里创建的其他句柄从不标成可继承。
 * 因此不必担心清理子进程对 postmaster 私有锁存器或
 * WaitEventSets 的引用。
 */
void
InitSharedLatch(Latch *latch)
{
#ifdef WIN32
	SECURITY_ATTRIBUTES sa;

	/*
	 * 设置安全属性，指明这些事件可被继承。
	 */
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	latch->event = CreateEvent(&sa, TRUE, FALSE, NULL);
	if (latch->event == NULL)
		elog(ERROR, "CreateEvent failed: error code %lu", GetLastError());
#endif

	latch->is_set = false;
	latch->maybe_sleeping = false;
	latch->owner_pid = 0;
	latch->is_shared = true;
}

/*
 * 把共享锁存器关联到当前进程，使它可以
 * 在该锁存器上等待。
 *
 * 虽然对锁存器已被占用有健全性检查，这里并不
 * 做任何上锁，这意味着若两个进程几乎同时试图
 * 占用同一个锁存器，我们可能发现不了错误。若
 * 存在这种风险，调用方必须提供互锁来防止它。
 */
void
OwnLatch(Latch *latch)
{
	int			owner_pid;

	/* 健全性检查 */
	Assert(latch->is_shared);

#if defined(WAIT_USE_SELF_PIPE)
	/* 断言本进程已调用 InitializeLatchSupport */
	Assert(selfpipe_readfd >= 0 && selfpipe_owner_pid == MyProcPid);
#elif defined(WAIT_USE_SIGNALFD)
	/* 断言本进程已调用 InitializeLatchSupport */
	Assert(signal_fd >= 0);
#endif

	owner_pid = latch->owner_pid;
	if (owner_pid != 0)
		elog(PANIC, "latch already owned by PID %d", owner_pid);

	latch->owner_pid = MyProcPid;
}

/*
 * 解除当前进程对共享锁存器的占有。
 */
void
DisownLatch(Latch *latch)
{
	Assert(latch->is_shared);
	Assert(latch->owner_pid == MyProcPid);

	latch->owner_pid = 0;
}

/*
 * 等待给定锁存器被设置，或等待 postmaster 死亡，或直到超时
 * 被超过。wakeEvents 是位掩码，指定要等待其中哪些
 * 事件。若锁存器已经设置（且给出了 WL_LATCH_SET），
 * 函数立即返回。
 *
 * timeout 以毫秒给出。若给出 WL_TIMEOUT 标志，它必须
 * >= 0。虽然声明为 long，我们实际上不支持
 * 长于 INT_MAX 毫秒的超时。注意，给出 WL_TIMEOUT 时
 * 会有一些额外开销，所以能不用超时就不用。
 *
 * 锁存器必须归当前进程所有，也就是必须是用 InitLatch
 * 初始化的进程本地锁存器，或是通过调用 OwnLatch
 * 关联到当前进程的共享锁存器。
 *
 * 返回位掩码，表示哪些 condition 导致了唤醒。注意
 * 若多个唤醒条件同时为真，并不保证一次调用
 * 会全部返回，但至少会返回一个。
 */
int
WaitLatch(Latch *latch, int wakeEvents, long timeout,
		  uint32 wait_event_info)
{
	WaitEvent	event;

	/* 由 postmaster 管理的调用方必须以某种方式处理 postmaster 死亡。 */
	Assert(!IsUnderPostmaster ||
		   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
		   (wakeEvents & WL_POSTMASTER_DEATH));

	/*
	 * 有些调用方可能有 MyLatch 以外的锁存器，或根本没有锁存器，
	 * 或想以不同方式处理 postmaster 死亡。赋值很便宜，
	 * 所以每次都做。
	 */
	if (!(wakeEvents & WL_LATCH_SET))
		latch = NULL;
	ModifyWaitEvent(LatchWaitSet, LatchWaitSetLatchPos, WL_LATCH_SET, latch);
	LatchWaitSet->exit_on_postmaster_death =
		((wakeEvents & WL_EXIT_ON_PM_DEATH) != 0);

	if (WaitEventSetWait(LatchWaitSet,
						 (wakeEvents & WL_TIMEOUT) ? timeout : -1,
						 &event, 1,
						 wait_event_info) == 0)
		return WL_TIMEOUT;
	else
		return event.events;
}

/*
 * 与 WaitLatch 类似，但多一个套接字参数，用于 WL_SOCKET_
 * 条件。
 *
 * 在套接字上等待时，EOF 和错误条件总会使该套接字
 * 被报告为可读、可写或已连接，以便调用方
 * 处理该条件。
 *
 * wakeEvents 必须包含 WL_EXIT_ON_PM_DEATH，以便 postmaster
 * 死亡时自动退出，或者包含 WL_POSTMASTER_DEATH，以便
 * postmaster 死亡时在返回值里设置标志。后者用于
 * 少数需要立即退出以外行为的情形。
 *
 * 注意：如今这只是 WaitEventSet API 的一层包装。若
 * 非常频繁地使用锁存器，考虑创建一个寿命更长的
 * WaitEventSet；那样更有效率。
 */
int
WaitLatchOrSocket(Latch *latch, int wakeEvents, pgsocket sock,
				  long timeout, uint32 wait_event_info)
{
	int			ret = 0;
	int			rc;
	WaitEvent	event;
	WaitEventSet *set = CreateWaitEventSet(CurrentMemoryContext, 3);

	PG_TRY();
	{
		if (wakeEvents & WL_TIMEOUT)
			Assert(timeout >= 0);
		else
			timeout = -1;

		if (wakeEvents & WL_LATCH_SET)
			AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET,
							  latch, NULL);

		/* 由 postmaster 管理的调用方必须以某种方式处理 postmaster 死亡。 */
		Assert(!IsUnderPostmaster ||
			   (wakeEvents & WL_EXIT_ON_PM_DEATH) ||
			   (wakeEvents & WL_POSTMASTER_DEATH));

		if ((wakeEvents & WL_POSTMASTER_DEATH) && IsUnderPostmaster)
			AddWaitEventToSet(set, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);

		if ((wakeEvents & WL_EXIT_ON_PM_DEATH) && IsUnderPostmaster)
			AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);

		if (wakeEvents & WL_SOCKET_MASK)
		{
			int			ev;

			ev = wakeEvents & WL_SOCKET_MASK;
			AddWaitEventToSet(set, ev, sock, NULL, NULL);
		}

		rc = WaitEventSetWait(set, timeout, &event, 1, wait_event_info);

		if (rc == 0)
			ret |= WL_TIMEOUT;
		else
		{
			ret |= event.events & (WL_LATCH_SET |
								   WL_POSTMASTER_DEATH |
								   WL_SOCKET_MASK);
		}
	}
	PG_FINALLY();
	{
		FreeWaitEventSet(set);
	}
	PG_END_TRY();

	return ret;
}

/*
 * 设置锁存器，并唤醒正在等它的人。
 *
 * 若锁存器已经设置，这很便宜，否则就不那么便宜。
 *
 * 注意：在信号处理函数里调用时，务必在其前后保存并恢复
 * errno。这在大多数信号处理函数里是标准做法，
 * 当然，但我们以前在只设置标志的处理函数里省掉了它。
 *
 * 注意：本函数会从临界区和信号处理函数里调用，所以
 * 抛出错误不是好主意。
 */
void
SetLatch(Latch *latch)
{
#ifndef WIN32
	pid_t		owner_pid;
#else
	HANDLE		handle;
#endif

	/*
	 * 内存屏障必须放在这里，以确保本进程可能改过的
	 * 任何标志变量都已刷到主存，然后我们才检查或设置
	 * is_set。
	 */
	pg_memory_barrier();

	/* 若已经设置则快速退出 */
	if (latch->is_set)
		return;

	latch->is_set = true;

	pg_memory_barrier();
	if (!latch->maybe_sleeping)
		return;

#ifndef WIN32

	/*
	 * 看看有没有人在等这个锁存器。若我们在信号处理函数里，
	 * 那可以是当前进程。这种情况下我们用自管道或向自己发
	 * SIGURG，无竞争地唤醒 WaitEventSetWaitBlock()。若是
	 * 另一个进程，就发一个信号。
	 *
	 * 只取一次 owner_pid，以防该锁存器正被并发占用或卸下。
	 * XXX：这里假定 pid_t 是原子的，这一点并无保证！
	 * 实践中 pid_t 的有效范围放得进 32 位整数，因此应当
	 * 是原子的。最坏情况下，我们可能向错误的进程发信号。
	 * 即便如此，若那个假 pid 对应的进程存在且属于
	 * Postgres，你也非常不走运；而且 PG 数据库进程反正应当
	 * 能毫无问题地处理多余的 SIGUSR1
	 * 中断。
	 *
	 * 这里还可能有另一种竞争：我们查看之后，新进程
	 * 立刻占用该锁存器，于是我们没有向它发信号。只要
	 * ResetLatch/WaitLatch 的所有调用方都遵循标准写法，
	 * 在循环底部而不是顶部等待，这样它们就能正确处理
	 * 进入循环之前发生的锁存器设置事件，
	 * 这就没问题。
	 */
	owner_pid = latch->owner_pid;
	if (owner_pid == 0)
		return;
	else if (owner_pid == MyProcPid)
	{
#if defined(WAIT_USE_SELF_PIPE)
		if (waiting)
			sendSelfPipeByte();
#else
		if (waiting)
			kill(MyProcPid, SIGURG);
#endif
	}
	else
		kill(owner_pid, SIGURG);

#else

	/*
	 * 看看有没有人在等这个锁存器。若我们在信号处理函数里，
	 * 那可以是当前进程。
	 *
	 * 这里用局部变量，以防有人并发修改事件字段
	 * （这实在不应当发生）。
	 */
	handle = latch->event;
	if (handle)
	{
		SetEvent(handle);

		/*
		 * 注意，我们静默地忽略任何错误。我们可能在信号处理
		 * 函数或其他临界路径上，那里调用 elog() 不安全。
		 */
	}
#endif
}

/*
 * 清除锁存器。此后调用 WaitLatch 会睡眠，除非
 * 在 WaitLatch 调用之前锁存器再次被设置。
 */
void
ResetLatch(Latch *latch)
{
	/* 只有所有者应当重置该锁存器 */
	Assert(latch->owner_pid == MyProcPid);
	Assert(latch->maybe_sleeping == false);

	latch->is_set = false;

	/*
	 * 确保对 is_set 的写入在我们检查任何标志变量之前
	 * 已刷到主存。否则并发的 SetLatch 可能错误地认为
	 * 不必向我们发信号，即使我们错过了 SetLatch 本应
	 * 通知我们的某些标志更新。
	 */
	pg_memory_barrier();
}

/*
 * 创建一个 WaitEventSet，其中有空间等待 nevents 个不同事件。
 *
 * 然后可以用 WaitEventSetWait() 把这些事件
 * 高效地放在一起等待。
 */
WaitEventSet *
CreateWaitEventSet(MemoryContext context, int nevents)
{
	WaitEventSet *set;
	char	   *data;
	Size		sz = 0;

	/*
	 * 使用 MAXALIGN 的大小和对齐，以保证后来使用的内存
	 * 对齐正确。例如在某些平台上 epoll_event 可能需要
	 * 8 字节对齐，但更早的分配如 WaitEventSet 和 WaitEvent
	 * 若纯粹用 sizeof() 来定大小，未必能保证这一点。
	 */
	sz += MAXALIGN(sizeof(WaitEventSet));
	sz += MAXALIGN(sizeof(WaitEvent) * nevents);

#if defined(WAIT_USE_EPOLL)
	sz += MAXALIGN(sizeof(struct epoll_event) * nevents);
#elif defined(WAIT_USE_KQUEUE)
	sz += MAXALIGN(sizeof(struct kevent) * nevents);
#elif defined(WAIT_USE_POLL)
	sz += MAXALIGN(sizeof(struct pollfd) * nevents);
#elif defined(WAIT_USE_WIN32)
	/* 需要为 pgwin32_signal_event 留出空间 */
	sz += MAXALIGN(sizeof(HANDLE) * (nevents + 1));
#endif

	data = (char *) MemoryContextAllocZero(context, sz);

	set = (WaitEventSet *) data;
	data += MAXALIGN(sizeof(WaitEventSet));

	set->events = (WaitEvent *) data;
	data += MAXALIGN(sizeof(WaitEvent) * nevents);

#if defined(WAIT_USE_EPOLL)
	set->epoll_ret_events = (struct epoll_event *) data;
	data += MAXALIGN(sizeof(struct epoll_event) * nevents);
#elif defined(WAIT_USE_KQUEUE)
	set->kqueue_ret_events = (struct kevent *) data;
	data += MAXALIGN(sizeof(struct kevent) * nevents);
#elif defined(WAIT_USE_POLL)
	set->pollfds = (struct pollfd *) data;
	data += MAXALIGN(sizeof(struct pollfd) * nevents);
#elif defined(WAIT_USE_WIN32)
	set->handles = (HANDLE) data;
	data += MAXALIGN(sizeof(HANDLE) * nevents);
#endif

	set->latch = NULL;
	set->nevents_space = nevents;
	set->exit_on_postmaster_death = false;

#if defined(WAIT_USE_EPOLL)
	if (!AcquireExternalFD())
	{
		/* 把这当作 epoll_create1 本身返回了 EMFILE */
		elog(ERROR, "epoll_create1 failed: %m");
	}
	set->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (set->epoll_fd < 0)
	{
		ReleaseExternalFD();
		elog(ERROR, "epoll_create1 failed: %m");
	}
#elif defined(WAIT_USE_KQUEUE)
	if (!AcquireExternalFD())
	{
		/* 把这当作 kqueue 本身返回了 EMFILE */
		elog(ERROR, "kqueue failed: %m");
	}
	set->kqueue_fd = kqueue();
	if (set->kqueue_fd < 0)
	{
		ReleaseExternalFD();
		elog(ERROR, "kqueue failed: %m");
	}
	if (fcntl(set->kqueue_fd, F_SETFD, FD_CLOEXEC) == -1)
	{
		int			save_errno = errno;

		close(set->kqueue_fd);
		ReleaseExternalFD();
		errno = save_errno;
		elog(ERROR, "fcntl(F_SETFD) failed on kqueue descriptor: %m");
	}
	set->report_postmaster_not_running = false;
#elif defined(WAIT_USE_WIN32)

	/*
	 * 为了在等待时处理信号，需要加入一个 win32 特有事件。
	 * 我们在本例程顶部已经为这个额外事件留了位置。详见
	 * port/win32/signal.c。
	 *
	 * 注意：pgwin32_signal_event 应当放在第一位，以确保多个
	 * 事件同时设置时它会被报告。我们想保证
	 * 待处理信号得到服务。
	 */
	set->handles[0] = pgwin32_signal_event;
	StaticAssertStmt(WSA_INVALID_EVENT == NULL, "");
#endif

	return set;
}

/*
 * 释放先前创建的 WaitEventSet。
 *
 * 注意：最好不要让它释放任何可能跨 exec() 继承的
 * 资源。若会那样，很多情形下我们都可能泄漏那些资源。
 * 对 epoll，我们在创建文件描述符时设置 EPOLL_CLOEXEC
 * 来保证这一点。对 Windows，我们假定所涉及的句柄
 * 不可继承。
 */
void
FreeWaitEventSet(WaitEventSet *set)
{
#if defined(WAIT_USE_EPOLL)
	close(set->epoll_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_KQUEUE)
	close(set->kqueue_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_WIN32)
	WaitEvent  *cur_event;

	for (cur_event = set->events;
		 cur_event < (set->events + set->nevents);
		 cur_event++)
	{
		if (cur_event->events & WL_LATCH_SET)
		{
			/* 使用该锁存器的 HANDLE */
		}
		else if (cur_event->events & WL_POSTMASTER_DEATH)
		{
			/* 使用 PostmasterHandle */
		}
		else
		{
			/* 清理我们为套接字创建的事件对象 */
			WSAEventSelect(cur_event->fd, NULL, 0);
			WSACloseEvent(set->handles[cur_event->pos + 1]);
		}
	}
#endif

	pfree(set);
}

/*
 * 在 fork() 之后，于子进程中释放先前创建的 WaitEventSet。
 */
void
FreeWaitEventSetAfterFork(WaitEventSet *set)
{
#if defined(WAIT_USE_EPOLL)
	close(set->epoll_fd);
	ReleaseExternalFD();
#elif defined(WAIT_USE_KQUEUE)
	/* kqueue 通常不会被子进程继承 */
	ReleaseExternalFD();
#endif

	pfree(set);
}

/* ---
 * 向集合加入一个事件。可能的事件有：
 * - WL_LATCH_SET：等待锁存器被设置
 * - WL_POSTMASTER_DEATH：等待 postmaster 死亡
 * - WL_SOCKET_READABLE：等待套接字变为可读，
 *	 可与其他 WL_SOCKET_ 事件组合在同一个事件里
 * - WL_SOCKET_WRITEABLE：等待套接字变为可写，
 *	 可与其他 WL_SOCKET_ 事件组合
 * - WL_SOCKET_CONNECTED：等待套接字连接建立，
 *	 可与其他 WL_SOCKET_ 事件组合（在非 Windows
 *	 平台上，这与 WL_SOCKET_WRITEABLE 相同）
 * - WL_SOCKET_ACCEPT：等待服务器套接字上的新连接，
 *	 可与其他 WL_SOCKET_ 事件组合（在非 Windows
 *	 平台上，这与 WL_SOCKET_READABLE 相同）
 * - WL_SOCKET_CLOSED：等待套接字被远端对等方关闭。
 * - WL_EXIT_ON_PM_DEATH：若 postmaster 死亡则立即退出。
 *
 * 返回 WaitEventSet->events 中的偏移（从 0 起），可用于
 * 用 ModifyWaitEvent() 修改先前加入的等待事件。
 *
 * 在 WL_LATCH_SET 情形下，锁存器必须归当前进程所有，
 * 也就是必须是用 InitLatch 初始化的进程本地锁存器，或
 * 通过调用 OwnLatch 关联到当前进程的共享锁存器。
 *
 * 在 WL_SOCKET_READABLE/WRITEABLE/CONNECTED/ACCEPT 情形下，EOF
 * 和错误条件会使套接字被报告为可读、可写或已连接，
 * 以便调用方处理该条件。
 *
 * 这里指定的 user_data 指针会被设到 WaitEventSetWait()
 * 返回的事件上，从而容易把额外数据与
 * 事件关联起来。
 */
int
AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd, Latch *latch,
				  void *user_data)
{
	WaitEvent  *event;

	/* 空间不够 */
	Assert(set->nevents < set->nevents_space);

	if (events == WL_EXIT_ON_PM_DEATH)
	{
		events = WL_POSTMASTER_DEATH;
		set->exit_on_postmaster_death = true;
	}

	if (latch)
	{
		if (latch->owner_pid != MyProcPid)
			elog(ERROR, "cannot wait on a latch owned by another process");
		if (set->latch)
			elog(ERROR, "cannot wait on more than one latch");
		if ((events & WL_LATCH_SET) != WL_LATCH_SET)
			elog(ERROR, "latch events only support being set");
	}
	else
	{
		if (events & WL_LATCH_SET)
			elog(ERROR, "cannot wait on latch without a specified latch");
	}

	/* 没有套接字却等待套接字就绪，说明有缺陷 */
	if (fd == PGINVALID_SOCKET && (events & WL_SOCKET_MASK))
		elog(ERROR, "cannot wait on socket event without a socket");

	event = &set->events[set->nevents];
	event->pos = set->nevents++;
	event->fd = fd;
	event->events = events;
	event->user_data = user_data;
#ifdef WIN32
	event->reset = false;
#endif

	if (events == WL_LATCH_SET)
	{
		set->latch = latch;
		set->latch_pos = event->pos;
#if defined(WAIT_USE_SELF_PIPE)
		event->fd = selfpipe_readfd;
#elif defined(WAIT_USE_SIGNALFD)
		event->fd = signal_fd;
#else
		event->fd = PGINVALID_SOCKET;
#ifdef WAIT_USE_EPOLL
		return event->pos;
#endif
#endif
	}
	else if (events == WL_POSTMASTER_DEATH)
	{
#ifndef WIN32
		event->fd = postmaster_alive_fds[POSTMASTER_FD_WATCH];
#endif
	}

	/* 如有需要，执行等待原语特有的初始化 */
#if defined(WAIT_USE_EPOLL)
	WaitEventAdjustEpoll(set, event, EPOLL_CTL_ADD);
#elif defined(WAIT_USE_KQUEUE)
	WaitEventAdjustKqueue(set, event, 0);
#elif defined(WAIT_USE_POLL)
	WaitEventAdjustPoll(set, event);
#elif defined(WAIT_USE_WIN32)
	WaitEventAdjustWin32(set, event);
#endif

	return event->pos;
}

/*
 * 改变事件掩码，并在 WL_LATCH_SET 情形下改变与
 * WaitEvent 关联的锁存器。可以把锁存器改成 NULL 以暂时禁用
 * 该锁存器，以后再设回一个锁存器。
 *
 * 'pos' 是 AddWaitEventToSet 返回的标识。
 */
void
ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, Latch *latch)
{
	WaitEvent  *event;
#if defined(WAIT_USE_KQUEUE)
	int			old_events;
#endif

	Assert(pos < set->nevents);

	event = &set->events[pos];
#if defined(WAIT_USE_KQUEUE)
	old_events = event->events;
#endif

	/*
	 * 若事件掩码和关联的锁存器都没变，就提早
	 * 返回。对某些套接字这是重要的加快，它们
	 * 频繁用 ModifyWaitEvent 从等待读切换到
	 * 等待写。
	 */
	if (events == event->events &&
		(!(event->events & WL_LATCH_SET) || set->latch == latch))
		return;

	if (event->events & WL_LATCH_SET &&
		events != event->events)
	{
		elog(ERROR, "cannot modify latch event");
	}

	if (event->events & WL_POSTMASTER_DEATH)
	{
		elog(ERROR, "cannot modify postmaster death event");
	}

	/* FIXME：校验事件掩码 */
	event->events = events;

	if (events == WL_LATCH_SET)
	{
		if (latch && latch->owner_pid != MyProcPid)
			elog(ERROR, "cannot wait on a latch owned by another process");
		set->latch = latch;

		/*
		 * 在 Unix 上不必修改内核对象，因为底层管道
		 * （若有的话）对所有锁存器都相同，所以可以
		 * 立即返回。在 Windows 上需要更新句柄数组，
		 * 但我们把旧的留在原地，并在锁存器被禁用时
		 * 容忍虚假唤醒。
		 */
#if defined(WAIT_USE_WIN32)
		if (!latch)
			return;
#else
		return;
#endif
	}

#if defined(WAIT_USE_EPOLL)
	WaitEventAdjustEpoll(set, event, EPOLL_CTL_MOD);
#elif defined(WAIT_USE_KQUEUE)
	WaitEventAdjustKqueue(set, event, old_events);
#elif defined(WAIT_USE_POLL)
	WaitEventAdjustPoll(set, event);
#elif defined(WAIT_USE_WIN32)
	WaitEventAdjustWin32(set, event);
#endif
}

#if defined(WAIT_USE_EPOLL)
/*
 * action 可以是 EPOLL_CTL_ADD、EPOLL_CTL_MOD 或 EPOLL_CTL_DEL 之一
 */
static void
WaitEventAdjustEpoll(WaitEventSet *set, WaitEvent *event, int action)
{
	struct epoll_event epoll_ev;
	int			rc;

	/* 指向我们事件的指针，由 epoll_wait 返回 */
	epoll_ev.data.ptr = event;
	/* 始终等待错误 */
	epoll_ev.events = EPOLLERR | EPOLLHUP;

	/* 预先准备一次 pollfd 项 */
	if (event->events == WL_LATCH_SET)
	{
		Assert(set->latch != NULL);
		epoll_ev.events |= EPOLLIN;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		epoll_ev.events |= EPOLLIN;
	}
	else
	{
		Assert(event->fd != PGINVALID_SOCKET);
		Assert(event->events & (WL_SOCKET_READABLE |
								WL_SOCKET_WRITEABLE |
								WL_SOCKET_CLOSED));

		if (event->events & WL_SOCKET_READABLE)
			epoll_ev.events |= EPOLLIN;
		if (event->events & WL_SOCKET_WRITEABLE)
			epoll_ev.events |= EPOLLOUT;
		if (event->events & WL_SOCKET_CLOSED)
			epoll_ev.events |= EPOLLRDHUP;
	}

	/*
	 * 即便用不到，若 action 传入 EPOLL_CTL_DEL，我们仍把
	 * epoll_ev 作为 data 参数传过去。以前有个 epoll 缺陷
	 * 要求那样，而且这实际上让代码更简单……
	 */
	rc = epoll_ctl(set->epoll_fd, action, event->fd, &epoll_ev);

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("%s() failed: %m",
						"epoll_ctl")));
}
#endif

#if defined(WAIT_USE_POLL)
static void
WaitEventAdjustPoll(WaitEventSet *set, WaitEvent *event)
{
	struct pollfd *pollfd = &set->pollfds[event->pos];

	pollfd->revents = 0;
	pollfd->fd = event->fd;

	/* 预先准备一次 pollfd 项 */
	if (event->events == WL_LATCH_SET)
	{
		Assert(set->latch != NULL);
		pollfd->events = POLLIN;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		pollfd->events = POLLIN;
	}
	else
	{
		Assert(event->events & (WL_SOCKET_READABLE |
								WL_SOCKET_WRITEABLE |
								WL_SOCKET_CLOSED));
		pollfd->events = 0;
		if (event->events & WL_SOCKET_READABLE)
			pollfd->events |= POLLIN;
		if (event->events & WL_SOCKET_WRITEABLE)
			pollfd->events |= POLLOUT;
#ifdef POLLRDHUP
		if (event->events & WL_SOCKET_CLOSED)
			pollfd->events |= POLLRDHUP;
#endif
	}

	Assert(event->fd != PGINVALID_SOCKET);
}
#endif

#if defined(WAIT_USE_KQUEUE)

/*
 * 在大多数 BSD 家族系统上，struct kevent 的 udata 成员类型是
 * void *，因此可以直接与 WaitEvent * 互转。可惜
 * NetBSD 把它做成 intptr_t，所以这里用左值强制转换
 * 遮住这个差别。
 */
#define AccessWaitEvent(k_ev) (*((WaitEvent **)(&(k_ev)->udata)))

static inline void
WaitEventAdjustKqueueAdd(struct kevent *k_ev, int filter, int action,
						 WaitEvent *event)
{
	k_ev->ident = event->fd;
	k_ev->filter = filter;
	k_ev->flags = action;
	k_ev->fflags = 0;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

static inline void
WaitEventAdjustKqueueAddPostmaster(struct kevent *k_ev, WaitEvent *event)
{
	/* 目前 postmaster 死亡只能加入，不能去掉。 */
	k_ev->ident = PostmasterPid;
	k_ev->filter = EVFILT_PROC;
	k_ev->flags = EV_ADD;
	k_ev->fflags = NOTE_EXIT;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

static inline void
WaitEventAdjustKqueueAddLatch(struct kevent *k_ev, WaitEvent *event)
{
	/* 目前锁存器只能加入，不能去掉。 */
	k_ev->ident = SIGURG;
	k_ev->filter = EVFILT_SIGNAL;
	k_ev->flags = EV_ADD;
	k_ev->fflags = 0;
	k_ev->data = 0;
	AccessWaitEvent(k_ev) = event;
}

/*
 * old_events 是先前的事件掩码，用来计算变了什么。
 */
static void
WaitEventAdjustKqueue(WaitEventSet *set, WaitEvent *event, int old_events)
{
	int			rc;
	struct kevent k_ev[2];
	int			count = 0;
	bool		new_filt_read = false;
	bool		old_filt_read = false;
	bool		new_filt_write = false;
	bool		old_filt_write = false;

	if (old_events == event->events)
		return;

	Assert(event->events != WL_LATCH_SET || set->latch != NULL);
	Assert(event->events == WL_LATCH_SET ||
		   event->events == WL_POSTMASTER_DEATH ||
		   (event->events & (WL_SOCKET_READABLE |
							 WL_SOCKET_WRITEABLE |
							 WL_SOCKET_CLOSED)));

	if (event->events == WL_POSTMASTER_DEATH)
	{
		/*
		 * 与所有其他实现不同，我们用进程通知来检测
		 * postmaster 死亡，而不是在 postmaster 的存活管道上
		 * 等待。
		 */
		WaitEventAdjustKqueueAddPostmaster(&k_ev[count++], event);
	}
	else if (event->events == WL_LATCH_SET)
	{
		/* 我们用信号事件来检测锁存器唤醒。 */
		WaitEventAdjustKqueueAddLatch(&k_ev[count++], event);
	}
	else
	{
		/*
		 * 我们需要算出从旧事件掩码变到新事件掩码所需的
		 * 增加和删除，因为 kevent 把可读和可写当作
		 * 分开的事件。
		 */
		if (old_events & (WL_SOCKET_READABLE | WL_SOCKET_CLOSED))
			old_filt_read = true;
		if (event->events & (WL_SOCKET_READABLE | WL_SOCKET_CLOSED))
			new_filt_read = true;
		if (old_events & WL_SOCKET_WRITEABLE)
			old_filt_write = true;
		if (event->events & WL_SOCKET_WRITEABLE)
			new_filt_write = true;
		if (old_filt_read && !new_filt_read)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_READ, EV_DELETE,
									 event);
		else if (!old_filt_read && new_filt_read)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_READ, EV_ADD,
									 event);
		if (old_filt_write && !new_filt_write)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_WRITE, EV_DELETE,
									 event);
		else if (!old_filt_write && new_filt_write)
			WaitEventAdjustKqueueAdd(&k_ev[count++], EVFILT_WRITE, EV_ADD,
									 event);
	}

	/* 对 WL_SOCKET_READ 到 WL_SOCKET_CLOSED，不需要改变。 */
	if (count == 0)
		return;

	Assert(count <= 2);

	rc = kevent(set->kqueue_fd, &k_ev[0], count, NULL, 0, NULL);

	/*
	 * 加入 postmaster 的 pid 时，必须考虑到它可能
	 * 已经退出，甚至已被另一个具有相同 pid 的进程替换。
	 * 若是这样，我们必须推迟到下一次调用
	 * WaitEventSetWaitBlock() 时才把这报告为事件。
	 */

	if (rc < 0)
	{
		if (event->events == WL_POSTMASTER_DEATH &&
			(errno == ESRCH || errno == EACCES))
			set->report_postmaster_not_running = true;
		else
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"kevent")));
	}
	else if (event->events == WL_POSTMASTER_DEATH &&
			 PostmasterPid != getppid() &&
			 !PostmasterIsAlive())
	{
		/*
		 * 额外的 PostmasterIsAliveInternal() 检查，防止在被
		 * 调试器跟踪时 getppid() 给出不同值的系统上
		 * 出现假警报。
		 */
		set->report_postmaster_not_running = true;
	}
}

#endif

#if defined(WAIT_USE_WIN32)
static void
WaitEventAdjustWin32(WaitEventSet *set, WaitEvent *event)
{
	HANDLE	   *handle = &set->handles[event->pos + 1];

	if (event->events == WL_LATCH_SET)
	{
		Assert(set->latch != NULL);
		*handle = set->latch->event;
	}
	else if (event->events == WL_POSTMASTER_DEATH)
	{
		*handle = PostmasterHandle;
	}
	else
	{
		int			flags = FD_CLOSE;	/* 始终检查错误和 EOF */

		if (event->events & WL_SOCKET_READABLE)
			flags |= FD_READ;
		if (event->events & WL_SOCKET_WRITEABLE)
			flags |= FD_WRITE;
		if (event->events & WL_SOCKET_CONNECTED)
			flags |= FD_CONNECT;
		if (event->events & WL_SOCKET_ACCEPT)
			flags |= FD_ACCEPT;

		if (*handle == WSA_INVALID_EVENT)
		{
			*handle = WSACreateEvent();
			if (*handle == WSA_INVALID_EVENT)
				elog(ERROR, "failed to create event for socket: error code %d",
					 WSAGetLastError());
		}
		if (WSAEventSelect(event->fd, *handle, flags) != 0)
			elog(ERROR, "failed to set up event for socket: error code %d",
				 WSAGetLastError());

		Assert(event->fd != PGINVALID_SOCKET);
	}
}
#endif

/*
 * 等待加入集合的事件发生，或直到到达
 * 超时。最多返回 nevents 个已发生的事件。
 *
 * 若 timeout = -1，阻塞直到某事件发生；若为 0，检查套接字
 * 是否就绪，但不阻塞；若 > 0，最多阻塞 timeout 毫秒。
 *
 * 返回已发生的事件数；若到达超时则返回 0。
 *
 * 返回的事件会把 fd、pos、user_data 字段设成
 * 与所登记事件关联的值。
 */
int
WaitEventSetWait(WaitEventSet *set, long timeout,
				 WaitEvent *occurred_events, int nevents,
				 uint32 wait_event_info)
{
	int			returned_events = 0;
	instr_time	start_time;
	instr_time	cur_time;
	long		cur_timeout = -1;

	Assert(nevents > 0);

	/*
	 * 若请求了超时，就初始化它。我们必须记下当前时间，
	 * 以便被打断时能算出剩余超时。
	 */
	if (timeout >= 0)
	{
		INSTR_TIME_SET_CURRENT(start_time);
		Assert(timeout >= 0 && timeout <= INT_MAX);
		cur_timeout = timeout;
	}
	else
		INSTR_TIME_SET_ZERO(start_time);

	pgstat_report_wait_start(wait_event_info);

#ifndef WIN32
	waiting = true;
#else
	/* 确保即使锁存器已经设置，信号也会得到处理 */
	pgwin32_dispatch_queued_signals();
#endif
	while (returned_events == 0)
	{
		int			rc;

		/*
		 * 先检查锁存器是否已经设置。若已设置，我们要么立即
		 * 退出，要么不经等待就向内核询问此刻可用的更多
		 * 事件，取决于调用方想要多少事件。
		 *
		 * 若有人在这与下面的 WaitEventSetWaitBlock() 之间
		 * 设置了锁存器，设置者会向管道写一个字节（或向我们
		 * 发信号，由信号处理函数来写），就绪例程会
		 * 立即返回。
		 *
		 * 在 unix 上，若自管道里有待处理字节，阻塞时就会
		 * 注意到。只在那种情况下清空管道，就不必每次使用
		 * WaitLatchOrSocket() 都排空它。万一管道缓冲区满了
		 * 也仍然没问题，因为管道处于非阻塞模式。这不太
		 * 可能发生，因为只有在阻塞时（waiting = true），或在
		 * latch_sigurg_handler() 内部的信号处理函数里，
		 * 自管道才会被填满。
		 *
		 * 在 windows 上，阻塞时若该锁存器有待处理事件也会
		 * 注意到，但没有把什么东西填满的危险，
		 * 因为已经设置的事件再设置一次没有效果。
		 *
		 * 注意：我们假定锁存器管理所涉及的内核调用
		 * 在弱内存序的机器上会提供足够的同步，因此若
		 * 通知已经入队，我们就无法错过
		 * 看到 is_set。
		 */
		if (set->latch && !set->latch->is_set)
		{
			/* 即将在锁存器上睡眠 */
			set->latch->maybe_sleeping = true;
			pg_memory_barrier();
			/* 并再检查一次 */
		}

		if (set->latch && set->latch->is_set)
		{
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->pos = set->latch_pos;
			occurred_events->user_data =
				set->events[set->latch_pos].user_data;
			occurred_events->events = WL_LATCH_SET;
			occurred_events++;
			returned_events++;

			/* 可能已在上面被设置 */
			set->latch->maybe_sleeping = false;

			if (returned_events == nevents)
				break;			/* 输出缓冲区已经满了 */

			/*
			 * 即便已经有一个事件，我们仍会以零超时
			 * 再轮询一次，看看同时还能把哪些非锁存器事件
			 * 放进输出缓冲区。
			 */
			cur_timeout = 0;
			timeout = 0;
		}

		/*
		 * 用本文件顶部选定的就绪原语等待事件。若返回
		 * -1，表示发生了超时；若返回 0，我们就得重试；
		 * 大于等于 1 的值是返回的事件数。
		 */
		rc = WaitEventSetWaitBlock(set, cur_timeout,
								   occurred_events, nevents - returned_events);

		if (set->latch &&
			set->latch->maybe_sleeping)
			set->latch->maybe_sleeping = false;

		if (rc == -1)
			break;				/* 发生了超时 */
		else
			returned_events += rc;

		/* 若还没完，为下一次迭代更新 cur_timeout */
		if (returned_events == 0 && timeout >= 0)
		{
			INSTR_TIME_SET_CURRENT(cur_time);
			INSTR_TIME_SUBTRACT(cur_time, start_time);
			cur_timeout = timeout - (long) INSTR_TIME_GET_MILLISEC(cur_time);
			if (cur_timeout <= 0)
				break;
		}
	}
#ifndef WIN32
	waiting = false;
#endif

	pgstat_report_wait_end();

	return returned_events;
}


#if defined(WAIT_USE_EPOLL)

/*
 * 用 Linux 的 epoll_wait(2) 等待。
 *
 * 这是更可取的等待方法，因为若干就绪通知会一起
 * 送达，不必遍历 set->events 的全部。返回的
 * epoll_event 结构里有指向我们事件的指针，关联
 * 起来很容易。
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct epoll_event *cur_epoll_event;

	/* 睡眠 */
	rc = epoll_wait(set->epoll_fd, set->epoll_ret_events,
					Min(nevents, set->nevents_space), cur_timeout);

	/* 检查返回码 */
	if (rc < 0)
	{
		/* EINTR 没问题，否则就抱怨 */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"epoll_wait")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* 超过了超时 */
		return -1;
	}

	/*
	 * 至少发生了一个事件，遍历返回的 epoll 事件，
	 * 直到它们全部处理完，或者我们已经返回了调用方
	 * 想要的全部事件。
	 */
	for (cur_epoll_event = set->epoll_ret_events;
		 cur_epoll_event < (set->epoll_ret_events + rc) &&
		 returned_events < nevents;
		 cur_epoll_event++)
	{
		/* epoll 的 data 指针被设成关联的 WaitEvent */
		cur_event = (WaitEvent *) cur_epoll_event->data.ptr;

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_LATCH_SET &&
			cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))
		{
			/* 排空 signalfd。 */
			drain();

			if (set->latch && set->latch->maybe_sleeping && set->latch->is_set)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_LATCH_SET;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP))
		{
			/*
			 * 我们预期远端关闭时会出现 EPOLLHUP，但因为
			 * 我们也不预期管道变为可读或出现任何
			 * 错误，那些情形也当作 postmaster 死亡。
			 *
			 * 对把 postmaster 标成已死的虚假事件要多留神。
			 * 有报告说较旧的原语（具体说是 select(2)）发生过
			 * 这种情况，而一次虚假的 WL_POSTMASTER_DEATH 事件
			 * 会很痛苦。再检查一次
			 * 代价不大。
			 */
			if (!PostmasterIsAliveInternal())
			{
				if (set->exit_on_postmaster_death)
					proc_exit(1);
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_POSTMASTER_DEATH;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			Assert(cur_event->fd != PGINVALID_SOCKET);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_epoll_event->events & (EPOLLIN | EPOLLERR | EPOLLHUP)))
			{
				/* 套接字上有数据，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_epoll_event->events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
			{
				/* 可写，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_epoll_event->events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)))
			{
				/* 远端对等方关闭，或出错 */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}

	return returned_events;
}

#elif defined(WAIT_USE_KQUEUE)

/*
 * 在 BSD 家族系统和 macOS 上用 kevent(2) 等待。
 *
 * 目前这照搬 epoll 代码，但将来可以在同一次 kevent
 * 调用里修改文件描述符集合，而不是用单独的
 * 系统调用来做等待。
 */
static int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct kevent *cur_kqueue_event;
	struct timespec timeout;
	struct timespec *timeout_p;

	if (cur_timeout < 0)
		timeout_p = NULL;
	else
	{
		timeout.tv_sec = cur_timeout / 1000;
		timeout.tv_nsec = (cur_timeout % 1000) * 1000000;
		timeout_p = &timeout;
	}

	/*
	 * 报告由 WaitEventAdjustKqueue() 或更早一次
	 * WaitEventSetWait() 调用发现的 postmaster 事件。
	 */
	if (unlikely(set->report_postmaster_not_running))
	{
		if (set->exit_on_postmaster_death)
			proc_exit(1);
		occurred_events->fd = PGINVALID_SOCKET;
		occurred_events->events = WL_POSTMASTER_DEATH;
		return 1;
	}

	/* 睡眠 */
	rc = kevent(set->kqueue_fd, NULL, 0,
				set->kqueue_ret_events,
				Min(nevents, set->nevents_space),
				timeout_p);

	/* 检查返回码 */
	if (rc < 0)
	{
		/* EINTR 没问题，否则就抱怨 */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"kevent")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* 超过了超时 */
		return -1;
	}

	/*
	 * 至少发生了一个事件，遍历返回的 kqueue 事件，
	 * 直到它们全部处理完，或者我们已经返回了调用方
	 * 想要的全部事件。
	 */
	for (cur_kqueue_event = set->kqueue_ret_events;
		 cur_kqueue_event < (set->kqueue_ret_events + rc) &&
		 returned_events < nevents;
		 cur_kqueue_event++)
	{
		/* kevent 的 udata 指向关联的 WaitEvent */
		cur_event = AccessWaitEvent(cur_kqueue_event);

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_LATCH_SET &&
			cur_kqueue_event->filter == EVFILT_SIGNAL)
		{
			if (set->latch && set->latch->maybe_sleeping && set->latch->is_set)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_LATCH_SET;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 cur_kqueue_event->filter == EVFILT_PROC &&
				 (cur_kqueue_event->fflags & NOTE_EXIT) != 0)
		{
			/*
			 * 内核对这个 kqueue 对象只会告知一次
			 * postmaster 的退出，所以记住这一点供下次使用，
			 * 以便提供电平触发语义。
			 */
			set->report_postmaster_not_running = true;

			if (set->exit_on_postmaster_death)
				proc_exit(1);
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->events = WL_POSTMASTER_DEATH;
			occurred_events++;
			returned_events++;
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			Assert(cur_event->fd >= 0);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_kqueue_event->filter == EVFILT_READ))
			{
				/* 可读，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_kqueue_event->filter == EVFILT_READ) &&
				(cur_kqueue_event->flags & EV_EOF))
			{
				/* 远端对等方已经关闭 */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_kqueue_event->filter == EVFILT_WRITE))
			{
				/* 可写，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}

	return returned_events;
}

#elif defined(WAIT_USE_POLL)

/*
 * 用 poll(2) 等待。
 *
 * 这可以一次收到多个事件的就绪通知，
 * 但需要遍历 set->pollfds 的全部。
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	int			rc;
	WaitEvent  *cur_event;
	struct pollfd *cur_pollfd;

	/* 睡眠 */
	rc = poll(set->pollfds, set->nevents, (int) cur_timeout);

	/* 检查返回码 */
	if (rc < 0)
	{
		/* EINTR 没问题，否则就抱怨 */
		if (errno != EINTR)
		{
			waiting = false;
			ereport(ERROR,
					(errcode_for_socket_access(),
					 errmsg("%s() failed: %m",
							"poll")));
		}
		return 0;
	}
	else if (rc == 0)
	{
		/* 超过了超时 */
		return -1;
	}

	for (cur_event = set->events, cur_pollfd = set->pollfds;
		 cur_event < (set->events + set->nevents) &&
		 returned_events < nevents;
		 cur_event++, cur_pollfd++)
	{
		/* 这个文件描述符上没有活动，跳过 */
		if (cur_pollfd->revents == 0)
			continue;

		occurred_events->pos = cur_event->pos;
		occurred_events->user_data = cur_event->user_data;
		occurred_events->events = 0;

		if (cur_event->events == WL_LATCH_SET &&
			(cur_pollfd->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
		{
			/* 自管道里有数据，把它清掉。 */
			drain();

			if (set->latch && set->latch->maybe_sleeping && set->latch->is_set)
			{
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_LATCH_SET;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events == WL_POSTMASTER_DEATH &&
				 (cur_pollfd->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
		{
			/*
			 * 我们预期远端关闭时会出现 POLLHUP，但因为
			 * 我们也不预期管道变为可读或出现任何
			 * 错误，那些情形也当作 postmaster 死亡。
			 *
			 * 对把 postmaster 标成已死的虚假事件要多留神。
			 * 有报告说较旧的原语（具体说是 select(2)）发生过
			 * 这种情况，而一次虚假的 WL_POSTMASTER_DEATH 事件
			 * 会很痛苦。再检查一次
			 * 代价不大。
			 */
			if (!PostmasterIsAliveInternal())
			{
				if (set->exit_on_postmaster_death)
					proc_exit(1);
				occurred_events->fd = PGINVALID_SOCKET;
				occurred_events->events = WL_POSTMASTER_DEATH;
				occurred_events++;
				returned_events++;
			}
		}
		else if (cur_event->events & (WL_SOCKET_READABLE |
									  WL_SOCKET_WRITEABLE |
									  WL_SOCKET_CLOSED))
		{
			int			errflags = POLLHUP | POLLERR | POLLNVAL;

			Assert(cur_event->fd >= PGINVALID_SOCKET);

			if ((cur_event->events & WL_SOCKET_READABLE) &&
				(cur_pollfd->revents & (POLLIN | errflags)))
			{
				/* 套接字上有数据，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_READABLE;
			}

			if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
				(cur_pollfd->revents & (POLLOUT | errflags)))
			{
				/* 可写，或遇到 EOF */
				occurred_events->events |= WL_SOCKET_WRITEABLE;
			}

#ifdef POLLRDHUP
			if ((cur_event->events & WL_SOCKET_CLOSED) &&
				(cur_pollfd->revents & (POLLRDHUP | errflags)))
			{
				/* 远端对等方已关闭，或出错 */
				occurred_events->events |= WL_SOCKET_CLOSED;
			}
#endif

			if (occurred_events->events != 0)
			{
				occurred_events->fd = cur_event->fd;
				occurred_events++;
				returned_events++;
			}
		}
	}
	return returned_events;
}

#elif defined(WAIT_USE_WIN32)

/*
 * 用 Windows 的 WaitForMultipleObjects() 等待。
 *
 * 可惜这永远一次只返回一个就绪通知。
 * 注意，官方文档对 WaitForMultipleObjects 在
 * 多个事件被一次 bWaitAll = FALSE 调用所消耗
 * 这一点含糊其辞，
 * https://blogs.msdn.microsoft.com/oldnewthing/20150409-00/?p=44273 证实
 * 只有一个事件被消耗。
 */
static inline int
WaitEventSetWaitBlock(WaitEventSet *set, int cur_timeout,
					  WaitEvent *occurred_events, int nevents)
{
	int			returned_events = 0;
	DWORD		rc;
	WaitEvent  *cur_event;

	/* 重置任何需要重置的等待事件 */
	for (cur_event = set->events;
		 cur_event < (set->events + set->nevents);
		 cur_event++)
	{
		if (cur_event->reset)
		{
			WaitEventAdjustWin32(set, cur_event);
			cur_event->reset = false;
		}

		/*
		 * 我们为每个 WaitEventSet 把套接字关联到一个新的
		 * 事件句柄。若另一端体面地关闭，FD_CLOSE 只产生
		 * 一次。因此若它在我们停止等待之后被投递给
		 * 另一个事件，我们可能错过 FD_CLOSE 通知。
		 * 在设置好这个句柄以接收通知之后、进入睡眠之前，
		 * 用窥视 EOF 的办法堵住这个竞争，
		 * 再进入睡眠。
		 *
		 * XXX 若套接字的整个生命期只用一个事件句柄，
		 * 就不需要这个了。
		 */
		if (cur_event->events & WL_SOCKET_READABLE)
		{
			char		c;
			WSABUF		buf;
			DWORD		received;
			DWORD		flags;

			buf.buf = &c;
			buf.len = 1;
			flags = MSG_PEEK;
			if (WSARecv(cur_event->fd, &buf, 1, &received, &flags, NULL, NULL) == 0)
			{
				occurred_events->pos = cur_event->pos;
				occurred_events->user_data = cur_event->user_data;
				occurred_events->events = WL_SOCKET_READABLE;
				occurred_events->fd = cur_event->fd;
				return 1;
			}
		}

		/*
		 * Windows 并不保证会记录 FD_WRITE 网络事件来表明
		 * 可以发送更多数据，除非上一次 send() 因
		 * WSAEWOULDBLOCK 失败。调用方很可能做过这样的调用，
		 * 但这里无法作此假定。因此，若在等待写就绪，就用
		 * 一次空的 send() 来迫使问题出现。若这次空 send()
		 * 成功，就认为套接字事实上写就绪，并立即
		 * 返回。另外，若它因 WSAEWOULDBLOCK 以外的原因失败，
		 * 就返回写就绪指示，让调用方
		 * 去处理错误条件。
		 */
		if (cur_event->events & WL_SOCKET_WRITEABLE)
		{
			char		c;
			WSABUF		buf;
			DWORD		sent;
			int			r;

			buf.buf = &c;
			buf.len = 0;

			r = WSASend(cur_event->fd, &buf, 1, &sent, 0, NULL, NULL);
			if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			{
				occurred_events->pos = cur_event->pos;
				occurred_events->user_data = cur_event->user_data;
				occurred_events->events = WL_SOCKET_WRITEABLE;
				occurred_events->fd = cur_event->fd;
				return 1;
			}
		}
	}

	/*
	 * 睡眠。
	 *
	 * 需要等待 ->nevents + 1，因为信号句柄在 [0]。
	 */
	rc = WaitForMultipleObjects(set->nevents + 1, set->handles, FALSE,
								cur_timeout);

	/* 检查返回码 */
	if (rc == WAIT_FAILED)
		elog(ERROR, "WaitForMultipleObjects() failed: error code %lu",
			 GetLastError());
	else if (rc == WAIT_TIMEOUT)
	{
		/* 超过了超时 */
		return -1;
	}

	if (rc == WAIT_OBJECT_0)
	{
		/* 处理新到达的信号 */
		pgwin32_dispatch_queued_signals();
		return 0;				/* 进行重试 */
	}

	/*
	 * 由于始终存在 pgwin32_signal_event，偏移为 1，
	 * 句柄偏移直接对应一个等待事件。
	 */
	cur_event = (WaitEvent *) &set->events[rc - WAIT_OBJECT_0 - 1];

	occurred_events->pos = cur_event->pos;
	occurred_events->user_data = cur_event->user_data;
	occurred_events->events = 0;

	if (cur_event->events == WL_LATCH_SET)
	{
		/*
		 * 若现在并不在等待这个锁存器，就无法用
		 * set->latch->event 来重置已触发的事件。
		 */
		if (!ResetEvent(set->handles[cur_event->pos + 1]))
			elog(ERROR, "ResetEvent failed: error code %lu", GetLastError());

		if (set->latch && set->latch->maybe_sleeping && set->latch->is_set)
		{
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->events = WL_LATCH_SET;
			occurred_events++;
			returned_events++;
		}
	}
	else if (cur_event->events == WL_POSTMASTER_DEATH)
	{
		/*
		 * postmaster 显然已死。由于错误地返回
		 * WL_POSTMASTER_DEATH 的后果可能相当不愉快，我们不怕
		 * 麻烦，用 PostmasterIsAlive() 正面核实，即便并没有
		 * 已知理由认为该事件在 Windows 上会被
		 * 错误地设置。
		 */
		if (!PostmasterIsAliveInternal())
		{
			if (set->exit_on_postmaster_death)
				proc_exit(1);
			occurred_events->fd = PGINVALID_SOCKET;
			occurred_events->events = WL_POSTMASTER_DEATH;
			occurred_events++;
			returned_events++;
		}
	}
	else if (cur_event->events & WL_SOCKET_MASK)
	{
		WSANETWORKEVENTS resEvents;
		HANDLE		handle = set->handles[cur_event->pos + 1];

		Assert(cur_event->fd);

		occurred_events->fd = cur_event->fd;

		ZeroMemory(&resEvents, sizeof(resEvents));
		if (WSAEnumNetworkEvents(cur_event->fd, handle, &resEvents) != 0)
			elog(ERROR, "failed to enumerate network events: error code %d",
				 WSAGetLastError());
		if ((cur_event->events & WL_SOCKET_READABLE) &&
			(resEvents.lNetworkEvents & FD_READ))
		{
			/* 套接字上有数据 */
			occurred_events->events |= WL_SOCKET_READABLE;

			/*------
			 * WaitForMultipleObjects doesn't guarantee that a read event will
			 * be returned if the latch is set at the same time.  Even if it
			 * did, the caller might drop that event expecting it to reoccur
			 * on next call.  So, we must force the event to be reset if this
			 * WaitEventSet is used again in order to avoid an indefinite
			 * hang.  Refer https://msdn.microsoft.com/en-us/library/windows/desktop/ms741576(v=vs.85).aspx
			 * for the behavior of socket events.
			 *------
			 */
			cur_event->reset = true;
		}
		if ((cur_event->events & WL_SOCKET_WRITEABLE) &&
			(resEvents.lNetworkEvents & FD_WRITE))
		{
			/* 可写 */
			occurred_events->events |= WL_SOCKET_WRITEABLE;
		}
		if ((cur_event->events & WL_SOCKET_CONNECTED) &&
			(resEvents.lNetworkEvents & FD_CONNECT))
		{
			/* 已连接 */
			occurred_events->events |= WL_SOCKET_CONNECTED;
		}
		if ((cur_event->events & WL_SOCKET_ACCEPT) &&
			(resEvents.lNetworkEvents & FD_ACCEPT))
		{
			/* 可以接受入站连接 */
			occurred_events->events |= WL_SOCKET_ACCEPT;
		}
		if (resEvents.lNetworkEvents & FD_CLOSE)
		{
			/* 遇到 EOF 或错误，因此置上调用方请求的全部套接字标志 */
			occurred_events->events |= (cur_event->events & WL_SOCKET_MASK);
		}

		if (occurred_events->events != 0)
		{
			occurred_events++;
			returned_events++;
		}
	}

	return returned_events;
}
#endif

/*
 * 返回当前构建选项能否报告 WL_SOCKET_CLOSED。
 */
bool
WaitEventSetCanReportClosed(void)
{
#if (defined(WAIT_USE_POLL) && defined(POLLRDHUP)) || \
	defined(WAIT_USE_EPOLL) || \
	defined(WAIT_USE_KQUEUE)
	return true;
#else
	return false;
#endif
}

/*
 * 取得给定 WaitEventSet 里登记的等待事件数量。
 */
int
GetNumRegisteredWaitEvents(WaitEventSet *set)
{
	return set->nevents;
}

#if defined(WAIT_USE_SELF_PIPE)

/*
 * SetLatch 用 SIGURG 唤醒正在该锁存器上等待的进程。
 *
 * 若我们正在等待，就唤醒 WaitLatch。
 */
static void
latch_sigurg_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (waiting)
		sendSelfPipeByte();

	errno = save_errno;
}

/* 向自管道发送一个字节，以唤醒 WaitLatch */
static void
sendSelfPipeByte(void)
{
	int			rc;
	char		dummy = 0;

retry:
	rc = write(selfpipe_writefd, &dummy, 1);
	if (rc < 0)
	{
		/* 若被信号打断，就重试 */
		if (errno == EINTR)
			goto retry;

		/*
		 * 若管道已满，就不必重试，那里已有的数据
		 * 已经足够唤醒 WaitLatch。
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		/*
		 * 糟了，write() 因别的原因失败。我们可能在
		 * 信号处理函数里，所以调用 elog() 不安全。我们只好
		 * 静默地忽略这个错误。
		 */
		return;
	}
}

#endif

#if defined(WAIT_USE_SELF_PIPE) || defined(WAIT_USE_SIGNALFD)

/*
 * 读出自管道或 signalfd 上所有可用数据。
 *
 * 注意：只在 waiting = true 时调用。若失败且不
 * 返回，必须先重置该标志（不过理想情况下，这
 * 永远不会发生）。
 */
static void
drain(void)
{
	char		buf[1024];
	int			rc;
	int			fd;

#ifdef WAIT_USE_SELF_PIPE
	fd = selfpipe_readfd;
#else
	fd = signal_fd;
#endif

	for (;;)
	{
		rc = read(fd, buf, sizeof(buf));
		if (rc < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;			/* 该描述符是空的 */
			else if (errno == EINTR)
				continue;		/* 继续重试 */
			else
			{
				waiting = false;
#ifdef WAIT_USE_SELF_PIPE
				elog(ERROR, "read() on self-pipe failed: %m");
#else
				elog(ERROR, "read() on signalfd failed: %m");
#endif
			}
		}
		else if (rc == 0)
		{
			waiting = false;
#ifdef WAIT_USE_SELF_PIPE
			elog(ERROR, "unexpected EOF on self-pipe");
#else
			elog(ERROR, "unexpected EOF on signalfd");
#endif
		}
		else if (rc < sizeof(buf))
		{
			/* 已成功排空管道；不必再 read() */
			break;
		}
		/* 否则缓冲区不够大，于是再读一次 */
	}
}

#endif
