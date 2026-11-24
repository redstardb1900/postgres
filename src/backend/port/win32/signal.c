/*-------------------------------------------------------------------------
 *
 * signal.c
 *	  Microsoft Windows Win32 信号仿真函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/signal.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"

/*
 * 这些由 UNBLOCKED_SIGNAL_QUEUE() 宏导出使用。
 * pg_signal_queue 须为 volatile，因信号线程会改它，
 * 主线程不加锁检查。pg_signal_mask 仅由主线程
 * 修改，故不必 volatile。
 */
volatile int pg_signal_queue;
int			pg_signal_mask;

HANDLE		pgwin32_signal_event;
HANDLE		pgwin32_initial_signal_pipe = INVALID_HANDLE_VALUE;

/*
 * pg_signal_crit_sec 仅保护 pg_signal_queue。那是发信号
 * 线程唯一可访问的变量！
 */
static CRITICAL_SECTION pg_signal_crit_sec;

/* 注意下标 0 未用，因其对应信号 0 */
static struct sigaction pg_signal_array[PG_SIGNAL_COUNT];
static pqsigfunc pg_signal_defaults[PG_SIGNAL_COUNT];


/* 信号处理线程函数 */
static DWORD WINAPI pg_signal_thread(LPVOID param);
static BOOL WINAPI pg_console_handler(DWORD dwCtrlType);


/*
 * pg_usleep --- 延迟指定微秒数，但若有信号到达
 * 则停止等待。
 *
 * 取代 src/port/pgsleep.c 提供的非信号感知版本。
 */
void
pg_usleep(long microsec)
{
	if (unlikely(pgwin32_signal_event == NULL))
	{
		/*
		 * 若在启动早期、信号事件尚未建立时被
		 * pgwin32_open_handle() 调到，则退回普通
		 * 不可中断休眠。
		 */
		SleepEx((microsec < 500 ? 1 : (microsec + 500) / 1000), FALSE);
		return;
	}

	if (WaitForSingleObject(pgwin32_signal_event,
							(microsec < 500 ? 1 : (microsec + 500) / 1000))
		== WAIT_OBJECT_0)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return;
	}
}


/* 初始化 */
void
pgwin32_signal_initialize(void)
{
	int			i;
	HANDLE		signal_thread_handle;

	InitializeCriticalSection(&pg_signal_crit_sec);

	for (i = 0; i < PG_SIGNAL_COUNT; i++)
	{
		pg_signal_array[i].sa_handler = SIG_DFL;
		pg_signal_array[i].sa_mask = 0;
		pg_signal_array[i].sa_flags = 0;
		pg_signal_defaults[i] = SIG_IGN;
	}
	pg_signal_mask = 0;
	pg_signal_queue = 0;

	/* 创建用于标记信号的全局事件句柄 */
	pgwin32_signal_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (pgwin32_signal_event == NULL)
		ereport(FATAL,
				(errmsg_internal("could not create signal event: error code %lu", GetLastError())));

	/* 创建处理信号的线程 */
	signal_thread_handle = CreateThread(NULL, 0, pg_signal_thread, NULL, 0, NULL);
	if (signal_thread_handle == NULL)
		ereport(FATAL,
				(errmsg_internal("could not create signal handler thread")));

	/* 创建控制台控制句柄以捕获 Ctrl-C 等 */
	if (!SetConsoleCtrlHandler(pg_console_handler, TRUE))
		ereport(FATAL,
				(errmsg_internal("could not set console control handler")));
}

/*
 * 分派当前已排队且未阻塞的全部信号
 * 被阻塞的信号忽略，将在 pqsigprocmask()
 * 调用时触发。
 */
void
pgwin32_dispatch_queued_signals(void)
{
	int			exec_mask;

	Assert(pgwin32_signal_event != NULL);
	EnterCriticalSection(&pg_signal_crit_sec);
	while ((exec_mask = UNBLOCKED_SIGNAL_QUEUE()) != 0)
	{
		/* 有一个或多个未阻塞信号排队待执行 */
		int			i;

		for (i = 1; i < PG_SIGNAL_COUNT; i++)
		{
			if (exec_mask & sigmask(i))
			{
				/* 执行此信号 */
				struct sigaction *act = &pg_signal_array[i];
				pqsigfunc	sig = act->sa_handler;

				if (sig == SIG_DFL)
					sig = pg_signal_defaults[i];
				pg_signal_queue &= ~sigmask(i);
				if (sig != SIG_ERR && sig != SIG_IGN && sig != SIG_DFL)
				{
					sigset_t	block_mask;
					sigset_t	save_mask;

					LeaveCriticalSection(&pg_signal_crit_sec);

					block_mask = act->sa_mask;
					if ((act->sa_flags & SA_NODEFER) == 0)
						block_mask |= sigmask(i);

					sigprocmask(SIG_BLOCK, &block_mask, &save_mask);
					sig(i);
					sigprocmask(SIG_SETMASK, &save_mask, NULL);

					EnterCriticalSection(&pg_signal_crit_sec);
					break;		/* Restart outer loop, in case signal mask or
								 * queue has been modified inside signal
								 * handler */
				}
			}
		}
	}
	ResetEvent(pgwin32_signal_event);
	LeaveCriticalSection(&pg_signal_crit_sec);
}

/* 信号屏蔽。仅在主线程调用，无需同步 */
int
pqsigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
	if (oset)
		*oset = pg_signal_mask;

	if (!set)
		return 0;

	switch (how)
	{
		case SIG_BLOCK:
			pg_signal_mask |= *set;
			break;
		case SIG_UNBLOCK:
			pg_signal_mask &= ~*set;
			break;
		case SIG_SETMASK:
			pg_signal_mask = *set;
			break;
		default:
			errno = EINVAL;
			return -1;
	}

	/*
	 * 立刻分派已排队信号，以防我们刚解除阻塞了
	 * 先前排队的一个或多个信号
	 */
	pgwin32_dispatch_queued_signals();

	return 0;
}

/*
 * 类 Unix 信号处理函数安装
 *
 * 仅在主线程调用，无需同步
 */
int
pqsigaction(int signum, const struct sigaction *act,
			struct sigaction *oldact)
{
	if (signum >= PG_SIGNAL_COUNT || signum < 0)
	{
		errno = EINVAL;
		return -1;
	}
	if (oldact)
		*oldact = pg_signal_array[signum];
	if (act)
		pg_signal_array[signum] = *act;
	return 0;
}

/* 为指定 PID 创建信号监听管道 */
HANDLE
pgwin32_create_signal_listener(pid_t pid)
{
	char		pipename[128];
	HANDLE		pipe;

	snprintf(pipename, sizeof(pipename), "\\\\.\\pipe\\pgsignal_%u", (int) pid);

	pipe = CreateNamedPipe(pipename, PIPE_ACCESS_DUPLEX,
						   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
						   PIPE_UNLIMITED_INSTANCES, 16, 16, 1000, NULL);

	if (pipe == INVALID_HANDLE_VALUE)
		ereport(ERROR,
				(errmsg("could not create signal listener pipe for PID %d: error code %lu",
						(int) pid, GetLastError())));

	return pipe;
}


/*
 * 以下全部函数在信号处理线程上执行
 * 且必须按此同步！
 * 注意！唯一可用的全局变量是
 * pg_signal_queue!
 */


/*
 * 通过置标志位与事件，为主线程排队信号。
 */
void
pg_queue_signal(int signum)
{
	Assert(pgwin32_signal_event != NULL);
	if (signum >= PG_SIGNAL_COUNT || signum <= 0)
		return;					/* 忽略坏信号编号 */

	EnterCriticalSection(&pg_signal_crit_sec);
	pg_signal_queue |= sigmask(signum);
	LeaveCriticalSection(&pg_signal_crit_sec);

	SetEvent(pgwin32_signal_event);
}

/* 信号处理线程 */
static DWORD WINAPI
pg_signal_thread(LPVOID param)
{
	char		pipename[128];
	HANDLE		pipe = pgwin32_initial_signal_pipe;

	/* 设置管道名，以防须重建管道。 */
	snprintf(pipename, sizeof(pipename), "\\\\.\\pipe\\pgsignal_%lu", GetCurrentProcessId());

	for (;;)
	{
		BOOL		fConnected;

		/* 若尚无管道实例则新建一个。 */
		if (pipe == INVALID_HANDLE_VALUE)
		{
			pipe = CreateNamedPipe(pipename, PIPE_ACCESS_DUPLEX,
								   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
								   PIPE_UNLIMITED_INSTANCES, 16, 16, 1000, NULL);

			if (pipe == INVALID_HANDLE_VALUE)
			{
				write_stderr("could not create signal listener pipe: error code %lu; retrying\n", GetLastError());
				SleepEx(500, FALSE);
				continue;
			}
		}

		/*
		 * 等待客户端连接。若在到达此处前已有连接，
		 * 会得到带 ERROR_PIPE_CONNECTED 的"失败"，
		 * 实际却是成功（干得好，Microsoft）。
		 */
		fConnected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (fConnected)
		{
			/*
			 * 有潜在信号发送方的连接。处理它。
			 */
			BYTE		sigNum;
			DWORD		bytes;

			if (ReadFile(pipe, &sigNum, 1, &bytes, NULL) &&
				bytes == 1)
			{
				/*
				 * 应答前先排队。这样可保证一旦
				 * 发送方的 kill() 返回，接收方下次
				 * CHECK_FOR_INTERRUPTS() 就能看见该信号。
				 * 比 POSIX 更强；或许不必？但
				 * 若无此保证，Windows 上曾见时序 bug，
				 * 而在任何已知 Unix 上都不表现。
				 *
				 */
				pg_queue_signal(sigNum);

				/*
				 * 向客户端写回一点数据，允许其
				 * CallNamedPipe() 结束。
				 */
				WriteFile(pipe, &sigNum, 1, &bytes, NULL);	/* Don't care if it
															 * works or not */

				/*
				 * 断开前须等客户端读完，否则数据丢失。
				 * WriteFile 失败则缓冲为空，故不
				 * 阻塞。
				 *
				 */
				FlushFileBuffers(pipe);
			}
			else
			{
				/*
				 * 若未能从客户端读到一字节，假定是客户端
				 * 的问题而什么也不做。或许更好的做法是
				 * 强制关闭并重开管道？
				 */
			}

			/* 与客户端断开以便重用管道。 */
			DisconnectNamedPipe(pipe);
		}
		else
		{
			/*
			 * 连接失败。清理后重试。
			 *
			 * 这不应发生。若发生，重建管道前会有窗口
			 * 漏掉信号。但再试同一管道大概也行不通，
			 * 因此我们几乎
			 * 别无选择。
			 */
			CloseHandle(pipe);
			pipe = INVALID_HANDLE_VALUE;
		}
	}
	return 0;
}


/* 控制台处理函数将在调用时由 OS 所建
   的线程上执行 */
static BOOL WINAPI
pg_console_handler(DWORD dwCtrlType)
{
	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT ||
		dwCtrlType == CTRL_CLOSE_EVENT ||
		dwCtrlType == CTRL_SHUTDOWN_EVENT)
	{
		pg_queue_signal(SIGINT);
		return TRUE;
	}
	return FALSE;
}
