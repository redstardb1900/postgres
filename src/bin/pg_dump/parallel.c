/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * 并行操作的工作方式：     
 *
 * 最初的领导者进程调用 ParallelBackupStart()，由它 fork 出   
 * 所需数量的工作进程，各自进入 WaitForCommands()。            
 *
 * 领导者进程在 DispatchJobForTocEntry() 中把单个工作项         
 * 分派给某个工作进程。我们发送诸如 "DUMP 1234" 或        
 * "RESTORE 1234"，其中 1234 是 TocEntry ID。              
 * 工作进程接收并解码命令，把它传给 AH->WorkerJobDumpPtr   
 * 或 AH->WorkerJobRestorePtr 所指向的例程，这些例程属于   
 * 当前归档格式。该例程执行所需操作（导出或恢复）并
 * 返回一个整数状态码。它被传回领导者，在那里我们把 
 * 它交给传给 DispatchJobForTocEntry() 的             
 * ParallelCompletionPtr 回调函数。该回调函数为   
 * pg_backup_archiver.c 中的                                         
 * 领导者控制逻辑更新状态。                 
 *
 * 原则上，命令或工作进程状态响应中可能还需要额外的   
 * 归档格式特定信息，但迄今尚未证明有此必要，      
 * 因为工作进程拥有 ArchiveHandle/TocEntry 数据结构           
 * 的完整副本。请记住，我们是在读入系统目录之后才 fork 出 
 * 工作进程的。因此工作进程也能访问系统目录信息。       
 * （Windows 情况下，工作进程是同一进程内的线程。     
 * 为避免问题，它们使用 Archive 数据结构的克隆副本； 
 * 参见 RunWorker()。）                               
 *
 * 在领导者进程中，每个工作进程的 workerStatus 字段是    
 * 下列值之一：   
 *		WRKR_NOT_STARTED: 尚未 fork 此工作进程     
 *		WRKR_IDLE: 正在等待命令        
 *		WRKR_WORKING: 正在处理命令       
 *		WRKR_TERMINATED: 进程结束 
 * 每个工作进程的 pstate->te[] 条目当其处于 WRKR_WORKING       
 * 状态时有效，否则须为 NULL。  
 */

#include "postgres_fe.h"

#ifndef WIN32
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_utils.h"
#include "port/pg_bswap.h"

/* 用于索引 pipe(2) 返回的 fd 数组的助记宏 */
#define PIPE_READ							0
#define PIPE_WRITE							1

#define NO_SLOT (-1)			/* GetIdleWorker() 的失败结果 */

/* 工作进程状态 */
typedef enum
{
	WRKR_NOT_STARTED = 0,
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_TERMINATED
} T_WorkerStatus;

#define WORKER_IS_RUNNING(workerStatus) \
	((workerStatus) == WRKR_IDLE || (workerStatus) == WRKR_WORKING)

/*
* 每个并行工作进程的私有状态（其 typedef 在 parallel.h 中）。
*
* 其中大部分仅在领导者进程中有效（或在 Windows 上，应仅由领导线程
* 接触）。但 AH 字段应仅由工作进程接触。管道描述符在各处有效。
 *
*/
struct ParallelSlot
{
	T_WorkerStatus workerStatus;	/* 参见上述枚举 */

	/* 以下字段在 workerStatus == WRKR_WORKING 时有效： */
	ParallelCompletionPtr callback; /* 完成时调用的函数 */
	void	   *callback_data;	/* 传给它的透传数据 */

	ArchiveHandle *AH;			/* 工作进程正在使用的归档数据 */

	int			pipeRead;		/* 领导端的管道 */
	int			pipeWrite;
	int			pipeRevRead;	/* 子进程端的管道 */
	int			pipeRevWrite;

	/* 子进程/线程标识信息： */
#ifdef WIN32
	uintptr_t	hThread;
	unsigned int threadId;
#else
	pid_t		pid;
#endif
};

#ifdef WIN32

/*
* 结构体，保存 _beginthreadex() 通过其唯一允许参数传递给其调用函数的信息。
 *
*/
typedef struct
{
	ArchiveHandle *AH;			/* 领导者数据库连接 */
	ParallelSlot *slot;			/* 此工作进程的并行槽 */
} WorkerInfo;

/* Windows 管道访问实现 */
static int	pgpipe(int handles[2]);
#define piperead(a,b,c)		recv(a,b,c,0)
#define pipewrite(a,b,c)	send(a,b,c,0)

#else							/* !WIN32 */

/* 非 Windows 管道访问实现 */
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)

#endif							/* WIN32 */

/*
* archive_close_connection() 关闭回调的状态信息。
*/
typedef struct ShutdownInformation
{
	ParallelState *pstate;
	Archive    *AHX;
} ShutdownInformation;

static ShutdownInformation shutdown_info;

/*
* 信号处理的状态信息。
* 我们假设 signal_info 初始化为零。
*
* 在 Unix 上，myAH 在领导者进程中是领导者数据库连接，
* 在工作进程中则是工作进程自己的连接。在 Windows 上，
* 我们只有一个 signal_info 实例，因此 myAH 是领导者连接，
* 工作进程连接必须从 pstate->parallelSlot[] 中获取。
*/
typedef struct DumpSignalInformation
{
	ArchiveHandle *myAH;		/* 用于发送取消的数据库连接 */
	ParallelState *pstate;		/* 并行状态，如果有的话 */
	bool		handler_set;	/* 信号处理器已在本进程中设置？ */
#ifndef WIN32
	bool		am_worker;		/* 我是工作进程吗？ */
#endif
} DumpSignalInformation;

static volatile DumpSignalInformation signal_info;

#ifdef WIN32
static CRITICAL_SECTION signal_info_lock;
#endif

/*
* 向 stderr 写一个简单字符串 --- 必须在信号处理器中安全。
* 我们忽略 write() 的结果，因为对此也做不了什么。
* 某些编译器使这比应有的更困难。
*/
#define write_stderr(str) \
	do { \
		const char *str_ = (str); \
		int		rc_; \
		rc_ = write(fileno(stderr), str_, strlen(str_)); \
		(void) rc_; \
	} while (0)


#ifdef WIN32
/* 文件作用域变量 */
static DWORD tls_index;

/* 全局可见变量（exit_nicely 需要） */
bool		parallel_init_done = false;
DWORD		mainThreadId;
#endif							/* WIN32 */

/* 局部函数原型 */
static ParallelSlot *GetMyPSlot(ParallelState *pstate);
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);
static void setup_cancel_handler(void);
static void set_cancel_pstate(ParallelState *pstate);
static void set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH);
static void RunWorker(ArchiveHandle *AH, ParallelSlot *slot);
static int	GetIdleWorker(ParallelState *pstate);
static bool HasEveryWorkerTerminated(ParallelState *pstate);
static void lockTableForWorker(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, int pipefd[2]);
static bool ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate,
							bool do_wait);
static char *getMessageFromLeader(int pipefd[2]);
static void sendMessageToLeader(int pipefd[2], const char *str);
static int	select_loop(int maxFd, fd_set *workerset);
static char *getMessageFromWorker(ParallelState *pstate,
								  bool do_wait, int *worker);
static void sendMessageToWorker(ParallelState *pstate,
								int worker, const char *str);
static char *readMessageFromPipe(int fd);

#define messageStartsWith(msg, prefix) \
	(strncmp(msg, prefix, strlen(prefix)) == 0)


/*
* 初始化并行导出支持 --- 应在进程启动早期调用。
* （目前，无论是否打算并行操作都会调用此函数。）
 *
*/
void
init_parallel_dump_utils(void)
{
#ifdef WIN32
	if (!parallel_init_done)
	{
		WSADATA		wsaData;
		int			err;

		/* 为线程化操作做准备 */
		tls_index = TlsAlloc();
		mainThreadId = GetCurrentThreadId();

		/* 初始化套接字访问 */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
			pg_fatal("%s() failed: error code %d", "WSAStartup", err);

		parallel_init_done = true;
	}
#endif
}

/*
* 查找当前工作进程或线程的 ParallelSlot。
*
* 如果未找到匹配槽则返回 NULL（这意味着我们是领导者）。
*/
static ParallelSlot *
GetMyPSlot(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
#ifdef WIN32
		if (pstate->parallelSlot[i].threadId == GetCurrentThreadId())
#else
		if (pstate->parallelSlot[i].pid == getpid())
#endif
			return &(pstate->parallelSlot[i]);
	}

	return NULL;
}

/*
* getLocalPQExpBuffer() 的线程局部版本。
*
* 不可重入但减少了内存泄漏：每个线程消耗一个缓冲区，
* 这比每个 fmtId/fmtQualifiedId 调用消耗一个好得多。
*/
#ifdef WIN32
static PQExpBuffer
getThreadLocalPQExpBuffer(void)
{
	/*
	* 如果使用静态变量，TLS 代码会出错，因此我们同时提供
	* 静态和自动变量，并在使用 TLS 时忽略静态变量。
	* 我们依赖 TlsGetValue() 在值尚未设置时返回 0。
	*/
	static PQExpBuffer s_id_return = NULL;
	PQExpBuffer id_return;

	if (parallel_init_done)
		id_return = (PQExpBuffer) TlsGetValue(tls_index);
	else
		id_return = s_id_return;

	if (id_return)				/* 第一次？ */
	{
		/* 同一缓冲区，仅清除内容 */
		resetPQExpBuffer(id_return);
	}
	else
	{
		/* 新缓冲区 */
		id_return = createPQExpBuffer();
		if (parallel_init_done)
			TlsSetValue(tls_index, id_return);
		else
			s_id_return = id_return;
	}

	return id_return;
}
#endif							/* WIN32 */

/*
* pg_dump 和 pg_restore 在创建 ArchiveHandle 后立即调用此函数
* 来注册清理处理器。
*/
void
on_exit_close_archive(Archive *AHX)
{
	shutdown_info.AHX = AHX;
	on_exit_nicely(archive_close_connection, &shutdown_info);
}

/*
* 用于干净关闭数据库连接和工作进程的 on_exit_nicely 处理器。
 *
*/
static void
archive_close_connection(int code, void *arg)
{
	ShutdownInformation *si = (ShutdownInformation *) arg;

	if (si->pstate)
	{
		/* 在并行模式下，必须确定我们是谁 */
		ParallelSlot *slot = GetMyPSlot(si->pstate);

		if (!slot)
		{
			/*
			* 我们是领导者。强制关闭工作进程，然后关闭自己的数据库连接（如果有）。
			 *
			*/
			ShutdownWorkersHard(si->pstate);

			if (si->AHX)
				DisconnectDatabase(si->AHX);
		}
		else
		{
			/*
			* 我们是工作进程。关闭自己的数据库连接（如果有）。在 Windows 上，
			* 我们还必须关闭通信套接字，以模拟 Unix 上工作进程退出时的行为。
			* （如果不这样做，在提前退出的情况下，领导者将无法检测到它，
			* 因为管道另一端不会出现 EOF 条件。）
			 *
			 *
			*/
			if (slot->AH)
				DisconnectDatabase(&(slot->AH->public));

#ifdef WIN32
			closesocket(slot->pipeRevRead);
			closesocket(slot->pipeRevWrite);
#endif
		}
	}
	else
	{
		/* 非并行操作：仅关闭领导者数据库连接 */
		if (si->AHX)
			DisconnectDatabase(si->AHX);
	}
}

/*
* 强制关闭所有剩余工作进程，等待它们完成。
*
* 注意：我们不期望在正常退出期间到达这里（工作进程应早已消失，
* ParallelState 也应已消失）。我们仅在 pg_fatal() 情况下到达这里，
* 因此介入取消活跃命令是适当的。
 *
*/
static void
ShutdownWorkersHard(ParallelState *pstate)
{
	int			i;

	/*
	* 关闭套接字写端，以便等待命令的工作进程知道它们可以退出。
	* （注意：如果初始化所有工作进程失败，某些 pipeWrite 字段可能
	* 仍为零。因此，忽略这里的错误。）
	 *
	*/
	for (i = 0; i < pstate->numWorkers; i++)
		closesocket(pstate->parallelSlot[i].pipeWrite);

	/*
	* 强制提前终止任何正在进行的命令。
	*/
#ifndef WIN32
	/* 在非 Windows 上，向每个工作进程发送 SIGTERM。 */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		pid_t		pid = pstate->parallelSlot[i].pid;

		if (pid != 0)
			kill(pid, SIGTERM);
	}
#else

	/*
	* 在 Windows 上，直接向工作进程的后端发送查询取消。
	* 使用临界区确保工作线程不更改状态。
	*/
	EnterCriticalSection(&signal_info_lock);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		ArchiveHandle *AH = pstate->parallelSlot[i].AH;
		char		errbuf[1];

		if (AH != NULL && AH->connCancel != NULL)
			(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
	}
	LeaveCriticalSection(&signal_info_lock);
#endif

	/* 现在等待它们终止。 */
	WaitForTerminatingWorkers(pstate);
}

/*
* 等待所有工作进程终止。
*/
static void
WaitForTerminatingWorkers(ParallelState *pstate)
{
	while (!HasEveryWorkerTerminated(pstate))
	{
		ParallelSlot *slot = NULL;
		int			j;

#ifndef WIN32
		/* 在非 Windows 上，使用 wait() 等待下一个工作进程结束 */
		int			status;
		pid_t		pid = wait(&status);

		/* 查找已死亡工作进程的槽，并清除 PID 字段 */
		for (j = 0; j < pstate->numWorkers; j++)
		{
			slot = &(pstate->parallelSlot[j]);
			if (slot->pid == pid)
			{
				slot->pid = 0;
				break;
			}
		}
#else							/* WIN32 */
		/* 在 Windows 上，必须使用 WaitForMultipleObjects() */
		HANDLE	   *lpHandles = pg_malloc(sizeof(HANDLE) * pstate->numWorkers);
		int			nrun = 0;
		DWORD		ret;
		uintptr_t	hThread;

		for (j = 0; j < pstate->numWorkers; j++)
		{
			if (WORKER_IS_RUNNING(pstate->parallelSlot[j].workerStatus))
			{
				lpHandles[nrun] = (HANDLE) pstate->parallelSlot[j].hThread;
				nrun++;
			}
		}
		ret = WaitForMultipleObjects(nrun, lpHandles, false, INFINITE);
		Assert(ret != WAIT_FAILED);
		hThread = (uintptr_t) lpHandles[ret - WAIT_OBJECT_0];
		free(lpHandles);

		/* 查找已死亡工作进程的槽，并清除 hThread 字段 */
		for (j = 0; j < pstate->numWorkers; j++)
		{
			slot = &(pstate->parallelSlot[j]);
			if (slot->hThread == hThread)
			{
				/* 为保持整洁，关闭已死亡线程的句柄 */
				CloseHandle((HANDLE) slot->hThread);
				slot->hThread = (uintptr_t) INVALID_HANDLE_VALUE;
				break;
			}
		}
#endif							/* WIN32 */

		/* 在所有平台上，也更新 workerStatus 和 te[] */
		Assert(j < pstate->numWorkers);
		slot->workerStatus = WRKR_TERMINATED;
		pstate->te[j] = NULL;
	}
}


/*
* 响应取消中断的代码（SIGINT、control-C 等）
*
* 这不完全属于此模块，但它需要访问 ParallelState 数据，
* 因此也没有更好的地方。
*
* 当收到取消中断时，我们可以直接终止，但在 pg_restore 中
* 这可能使 SQL 命令（例如大表上的 CREATE INDEX）持续运行很长时间。
* 相反，我们尝试发送取消请求然后终止。pg_dump 可能并不真正需要此功能，
* 但我们也可以在那里使用它。注意从信号处理器直接发送取消是安全的，
* 因为 PQcancel() 被设计为可以这样做。
*
* 在 Unix 上的并行操作中，每个进程负责取消自己的连接
* （这必须是如此，因为没有其他人可以访问它）。
* 此外，领导者进程应尝试将其信号转发给每个子进程。
* 在简单的 pg_dump/pg_restore 手动使用中，不需要转发，
* 因为在控制台输入 control-C 会将 SIGINT 传递给
* 终端进程组的每个成员 --- 但在其他场景中，
* 可能只有领导者收到信号。
*
* 在 Windows 上，取消处理器在单独的线程中运行，
* 因为这是 SetConsoleCtrlHandler 的工作方式。
* 我们使其停止工作线程，在所有活跃连接上发送取消，
* 然后返回 FALSE，这将允许进程终止。为了安全起见，
* 我们使用临界区保护 PGcancel 结构，防止信号线程运行时被更改。
 *
 *
*/

#ifndef WIN32

/*
* 信号处理器（仅 Unix）
*/
static void
sigTermHandler(SIGNAL_ARGS)
{
	int			i;
	char		errbuf[1];

	/*
	* 某些平台允许传递新信号来中断活跃的信号处理器。
	* 这可能破坏我们发送 PQcancel 的尝试，因此禁用
	* setup_cancel_handler 已启用的信号。
	*/
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);

	/*
	* 如果我们在领导者中，将信号转发给所有工作进程。
	* （在 PQcancel 之前执行此操作似乎更好；终止领导者事务将导致
	* 活跃工作进程出现无效快照错误，可能通过先终止工作进程来消除。），
	* 忽略任何错误。
	*/
	if (signal_info.pstate != NULL)
	{
		for (i = 0; i < signal_info.pstate->numWorkers; i++)
		{
			pid_t		pid = signal_info.pstate->parallelSlot[i].pid;

			if (pid != 0)
				kill(pid, SIGTERM);
		}
	}

	/*
	* 如果有连接可用，发送 QueryCancel。忽略错误，
	* 反正对此我们也做不了什么。
	*/
	if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
		(void) PQcancel(signal_info.myAH->connCancel, errbuf, sizeof(errbuf));

	/*
	* 报告我们正在退出，仅使用 write(2) 这样简单的操作。
	* 在并行操作中，只有领导者进程应执行此操作。
	*/
	if (!signal_info.am_worker)
	{
		if (progname)
		{
			write_stderr(progname);
			write_stderr(": ");
		}
		write_stderr("terminated by user\n");
	}

	/*
	* 然后终止，使用 _exit() 而非 exit()，因为后者会调用 atexit 处理器，
	* 如果我们在相关代码中中断了它们，则可能会失败。
	*/
	_exit(1);
}

/*
* 启用取消中断处理器（如果尚未启用）。
*/
static void
setup_cancel_handler(void)
{
	/*
	* 在 fork 时，signal_info.handler_set 将传播到新进程，
	* 但这没问题，因为信号处理器状态也会传播。
	*/
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		pqsignal(SIGINT, sigTermHandler);
		pqsignal(SIGTERM, sigTermHandler);
		pqsignal(SIGQUIT, sigTermHandler);
	}
}

#else							/* WIN32 */

/*
* 控制台中断处理器 --- 在新启动的线程中运行。
*
* 在停止其他线程并在所有打开的连接上发送取消请求后，
* 我们返回 FALSE，这将允许执行默认的 ExitProcess() 操作。
 *
*/
static BOOL WINAPI
consoleHandler(DWORD dwCtrlType)
{
	int			i;
	char		errbuf[1];

	if (dwCtrlType == CTRL_C_EVENT ||
		dwCtrlType == CTRL_BREAK_EVENT)
	{
		/* 临界区防止更改我们在此查看的数据 */
		EnterCriticalSection(&signal_info_lock);

		/*
		* 如果在并行模式下，停止工作线程并向其连接的后端
		* 发送 QueryCancel。停止工作线程的主要目的是防止它们
		* 将查询取消报告为错误，这会使屏幕杂乱。我们不必停止
		* 领导线程，因为它无论如何也不会做什么。先执行此操作
		* 再取消主事务，否则在停止工作线程之前可能收到
		* 无效快照错误报告。忽略错误，
		* 反正对此我们也做不了什么。
		 *
		*/
		if (signal_info.pstate != NULL)
		{
			for (i = 0; i < signal_info.pstate->numWorkers; i++)
			{
				ParallelSlot *slot = &(signal_info.pstate->parallelSlot[i]);
				ArchiveHandle *AH = slot->AH;
				HANDLE		hThread = (HANDLE) slot->hThread;

				/*
				* 在此使用 TerminateThread 可能留下一些泄漏的资源，
				* 但这无关紧要，因为我们将要结束整个进程。
				 *
				*/
				if (hThread != INVALID_HANDLE_VALUE)
					TerminateThread(hThread, 0);

				if (AH != NULL && AH->connCancel != NULL)
					(void) PQcancel(AH->connCancel, errbuf, sizeof(errbuf));
			}
		}

		/*
		* 向领导者连接发送 QueryCancel（如果已启用）。忽略错误，
		* 反正对此我们也做不了什么。
		*/
		if (signal_info.myAH != NULL && signal_info.myAH->connCancel != NULL)
			(void) PQcancel(signal_info.myAH->connCancel,
							errbuf, sizeof(errbuf));

		LeaveCriticalSection(&signal_info_lock);

		/*
		* 报告我们正在退出，仅使用 write(2) 这样简单的操作。
		* （也许可以在这里使用 pg_log_*()，但由于上面不干净地
		* 终止了其他线程，假设越少越好似乎更安全。）
		 *
		*/
		if (progname)
		{
			write_stderr(progname);
			write_stderr(": ");
		}
		write_stderr("terminated by user\n");
	}

	/* 始终返回 FALSE，允许信号处理继续 */
	return FALSE;
}

/*
* 启用取消中断处理器（如果尚未启用）。
*/
static void
setup_cancel_handler(void)
{
	if (!signal_info.handler_set)
	{
		signal_info.handler_set = true;

		InitializeCriticalSection(&signal_info_lock);

		SetConsoleCtrlHandler(consoleHandler, TRUE);
	}
}

#endif							/* WIN32 */


/*
* set_archive_cancel_info
*
* 用指定数据库连接的取消信息填充 AH->connCancel；
* 如果 conn 为 NULL 则清除。
*/
void
set_archive_cancel_info(ArchiveHandle *AH, PGconn *conn)
{
	PGcancel   *oldConnCancel;

	/*
	* 激活中断处理器（如果本进程中尚未激活）。在 Windows 上，
	* 这也会初始化 signal_info_lock；因此重要的是，
	* 这至少要在 fork 出任何线程之前执行一次。
	 *
	*/
	setup_cancel_handler();

	/*
	* 在 Unix 上，我们假设存储指针值对任何可能的信号中断是原子的。
	* 在 Windows 上，使用临界区。
	*/

#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	/* 如果有旧的则释放 */
	oldConnCancel = AH->connCancel;
	/* 确保中断处理器不会在释放时使用指针 */
	AH->connCancel = NULL;

	if (oldConnCancel != NULL)
		PQfreeCancel(oldConnCancel);

	/* 设置新的（如果指定） */
	if (conn)
		AH->connCancel = PQgetCancel(conn);

	/*
	* 在 Unix 上，每个进程始终只有一个活动的 ArchiveHandle，因此
	* 我们可以直接无条件设置 signal_info.myAH。在 Windows 上，
	* 仅在主线程中执行此操作；工作线程必须确保其
	* ArchiveHandle 出现在 pstate 数据中，这在 RunWorker() 中处理。
	 *
	*/
#ifndef WIN32
	signal_info.myAH = AH;
#else
	if (mainThreadId == GetCurrentThreadId())
		signal_info.myAH = AH;
#endif

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}

/*
* set_cancel_pstate
*
* 设置 signal_info.pstate 指向指定的 ParallelState（如果有）。
* 我们需要这主要是为了对 Windows 信号线程提供互锁。
*/
static void
set_cancel_pstate(ParallelState *pstate)
{
#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	signal_info.pstate = pstate;

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}

/*
* set_cancel_slot_archive
*
* 设置 ParallelSlot 的 AH 字段指向指定的归档（如果有）。
* 我们需要这主要是为了对 Windows 信号线程提供互锁。
*/
static void
set_cancel_slot_archive(ParallelSlot *slot, ArchiveHandle *AH)
{
#ifdef WIN32
	EnterCriticalSection(&signal_info_lock);
#endif

	slot->AH = AH;

#ifdef WIN32
	LeaveCriticalSection(&signal_info_lock);
#endif
}


/*
* 此函数由 Unix 和 Windows 变体调用，用于设置和运行工作进程。
* 调用者应在返回时退出进程（或线程）。
 *
*/
static void
RunWorker(ArchiveHandle *AH, ParallelSlot *slot)
{
	int			pipefd[2];

	/* 获取管道的子端 */
	pipefd[PIPE_READ] = slot->pipeRevRead;
	pipefd[PIPE_WRITE] = slot->pipeRevWrite;

	/*
	* 克隆归档，以便拥有自己的状态和工作空间，
	* 特别是自己的数据库连接。
	*
	* 我们在 Unix 和 Windows 上都克隆，尽管从技术上讲在 Unix 上不需要，
	* 因为 fork() 已在地址空间中给我们一个副本。
	* 但 CloneArchive 会重置状态信息并克隆
	* 数据库连接，这两者似乎都很有帮助。
	*/
	AH = CloneArchive(AH);

	/* 记住克隆的归档，以便信号处理器可以找到它 */
	set_cancel_slot_archive(slot, AH);

	/*
	* 调用 ArchiveHandle 中定义的工作进程设置函数。
	*/
	(AH->SetupWorkerPtr) ((Archive *) AH);

	/*
	* 执行命令直至完成。
	*/
	WaitForCommands(AH, pipefd);

	/*
	* 断开数据库连接并清理。
	*/
	set_cancel_slot_archive(slot, NULL);
	DisconnectDatabase(&(AH->public));
	DeCloneArchive(AH);
}

/*
* Windows 线程基函数
*/
#ifdef WIN32
static unsigned __stdcall
init_spawned_worker_win32(WorkerInfo *wi)
{
	ArchiveHandle *AH = wi->AH;
	ParallelSlot *slot = wi->slot;

	/* 不再需要 WorkerInfo */
	free(wi);

	/* 运行工作进程 ... */
	RunWorker(AH, slot);

	/* 退出线程 */
	_endthreadex(0);
	return 0;
}
#endif							/* WIN32 */

/*
* 此函数通过 fork 出工作进程来启动并行导出或恢复。
* 对于 Windows，它创建多个线程；在 Unix 上，工作进程使用 fork() 创建。
 *
*/
ParallelState *
ParallelBackupStart(ArchiveHandle *AH)
{
	ParallelState *pstate;
	int			i;

	Assert(AH->public.numWorkers > 0);

	pstate = (ParallelState *) pg_malloc(sizeof(ParallelState));

	pstate->numWorkers = AH->public.numWorkers;
	pstate->te = NULL;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	/* 创建状态数组，确保将所有字段初始化为 0 */
	pstate->te = (TocEntry **)
		pg_malloc0(pstate->numWorkers * sizeof(TocEntry *));
	pstate->parallelSlot = (ParallelSlot *)
		pg_malloc0(pstate->numWorkers * sizeof(ParallelSlot));

#ifdef WIN32
	/* 使 fmtId() 和 fmtQualifiedId() 使用线程局部存储 */
	getLocalPQExpBuffer = getThreadLocalPQExpBuffer;
#endif

	/*
	* 在 shutdown_info 中设置 pstate，以告知退出处理器必须
	* 清理工作进程以及主数据库连接。但我们暂不在 signal_info 中
	* 设置此项，因为我们不希望子进程继承非 NULL 的
	* signal_info.pstate。
	*/
	shutdown_info.pstate = pstate;

	/*
	* 在领导者连接上临时禁用查询取消。这确保子进程不会继承有效的
	* AH->connCancel 设置，因此不会尝试对领导者的连接发出取消。
	* 如果在禁用期间失败也没有坏处，
	* 因为此时领导者连接是空闲的。
	 *
	*/
	set_archive_cancel_info(AH, NULL);

	/* 确保 stdio 状态在 fork 前已平稳 */
	fflush(NULL);

	/* 创建所需数量的工作进程 */
	for (i = 0; i < pstate->numWorkers; i++)
	{
#ifdef WIN32
		WorkerInfo *wi;
		uintptr_t	handle;
#else
		pid_t		pid;
#endif
		ParallelSlot *slot = &(pstate->parallelSlot[i]);
		int			pipeMW[2],
					pipeWM[2];

		/* 为此工作进程创建通信管道 */
		if (pgpipe(pipeMW) < 0 || pgpipe(pipeWM) < 0)
			pg_fatal("could not create communication channels: %m");

		/* 领导端的管道 */
		slot->pipeRead = pipeWM[PIPE_READ];
		slot->pipeWrite = pipeMW[PIPE_WRITE];
		/* 子进程端的管道 */
		slot->pipeRevRead = pipeMW[PIPE_READ];
		slot->pipeRevWrite = pipeWM[PIPE_WRITE];

#ifdef WIN32
		/* 创建临时结构以将参数传递给工作函数 */
		wi = (WorkerInfo *) pg_malloc(sizeof(WorkerInfo));

		wi->AH = AH;
		wi->slot = slot;

		handle = _beginthreadex(NULL, 0, (void *) &init_spawned_worker_win32,
								wi, 0, &(slot->threadId));
		slot->hThread = handle;
		slot->workerStatus = WRKR_IDLE;
#else							/* !WIN32 */
		pid = fork();
		if (pid == 0)
		{
			/* 我们是工作进程 */
			int			j;

			/* GetMyPSlot() 需要此项 */
			slot->pid = getpid();

			/* 通知信号处理器我们现在是工作进程 */
			signal_info.am_worker = true;

			/* 关闭工作进程 -> 领导者的读端 */
			closesocket(pipeWM[PIPE_READ]);
			/* 关闭领导者 -> 工作进程的写端 */
			closesocket(pipeMW[PIPE_WRITE]);

			/*
			* 关闭领导者与之前 fork 的工作进程通信的
			* 所有继承的 fd。
			*/
			for (j = 0; j < i; j++)
			{
				closesocket(pstate->parallelSlot[j].pipeRead);
				closesocket(pstate->parallelSlot[j].pipeWrite);
			}

			/* 运行工作进程 ... */
			RunWorker(AH, slot);

			/* 完成后可以直接 exit(0) */
			exit(0);
		}
		else if (pid < 0)
		{
			/* fork 失败 */
			pg_fatal("could not create worker process: %m");
		}

		/* 成功 fork 后，在领导者中 */
		slot->pid = pid;
		slot->workerStatus = WRKR_IDLE;

		/* 关闭领导者 -> 工作进程的读端 */
		closesocket(pipeMW[PIPE_READ]);
		/* 关闭工作进程 -> 领导者的写端 */
		closesocket(pipeWM[PIPE_WRITE]);
#endif							/* WIN32 */
	}

	/*
	* 在 fork 出工作进程后，禁用 SIGPIPE 以防领导者尝试向
	* 已死亡的工作进程发送命令时被终止。但我们不希望
	* 工作进程继承此设置。
	*/
#ifndef WIN32
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	/*
	* 在领导者连接上重新启用查询取消。
	*/
	set_archive_cancel_info(AH, AH->connection);

	/*
	* 告知取消信号处理器也转发信号给工作进程。
	* （与查询取消一样，我们此前不需要此功能，因为工作进程
	* 尚未被分配任何工作；如果在此之前终止，任何已启动的工作进程
	* 将看到 EOF 并迅速退出。）
	*/
	set_cancel_pstate(pstate);

	return pstate;
}

/*
* 关闭并行导出或恢复。
*/
void
ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate)
{
	int			i;

	/* 非并行模式无需操作 */
	if (pstate->numWorkers == 1)
		return;

	/* 不应有任何未完成的作业 */
	Assert(IsEveryWorkerIdle(pstate));

	/* 关闭套接字以便工作进程知道它们可以退出 */
	for (i = 0; i < pstate->numWorkers; i++)
	{
		closesocket(pstate->parallelSlot[i].pipeRead);
		closesocket(pstate->parallelSlot[i].pipeWrite);
	}

	/* 等待它们退出 */
	WaitForTerminatingWorkers(pstate);

	/*
	* 从 shutdown_info 中解绑 pstate，以免退出处理器尝试使用它；
	* 同样从 signal_info 中解绑。
	*/
	shutdown_info.pstate = NULL;
	set_cancel_pstate(NULL);

	/* 释放状态（仅为整洁起见，因为我们即将终止） */
	free(pstate->te);
	free(pstate->parallelSlot);
	free(pstate);
}

/*
* 以下四个函数处理并行工作进程的命令字符串和响应字符串的
* 构造和解析。
*
* 目前，无论处理哪种归档格式，这些函数都是相同的。
* 将来，我们可能希望让格式模块覆盖这些函数，
* 以向命令或响应添加格式特定数据。
*/

/*
* buildWorkerCommand: 格式化发送给工作进程的命令字符串。
*
* 字符串构建在调用者提供的大小为 buflen 的缓冲区中。
*/
static void
buildWorkerCommand(ArchiveHandle *AH, TocEntry *te, T_Action act,
				   char *buf, int buflen)
{
	if (act == ACT_DUMP)
		snprintf(buf, buflen, "DUMP %d", te->dumpId);
	else if (act == ACT_RESTORE)
		snprintf(buf, buflen, "RESTORE %d", te->dumpId);
	else
		Assert(false);
}

/*
* parseWorkerCommand: 在工作进程中解释命令字符串。
*/
static void
parseWorkerCommand(ArchiveHandle *AH, TocEntry **te, T_Action *act,
				   const char *msg)
{
	DumpId		dumpId;
	int			nBytes;

	if (messageStartsWith(msg, "DUMP "))
	{
		*act = ACT_DUMP;
		sscanf(msg, "DUMP %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else if (messageStartsWith(msg, "RESTORE "))
	{
		*act = ACT_RESTORE;
		sscanf(msg, "RESTORE %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else
		pg_fatal("unrecognized command received from leader: \"%s\"",
				 msg);
}

/*
* buildWorkerResponse: 格式化发送给领导者的响应字符串。
*
* 字符串构建在调用者提供的大小为 buflen 的缓冲区中。
*/
static void
buildWorkerResponse(ArchiveHandle *AH, TocEntry *te, T_Action act, int status,
					char *buf, int buflen)
{
	snprintf(buf, buflen, "OK %d %d %d",
			 te->dumpId,
			 status,
			 status == WORKER_IGNORED_ERRORS ? AH->public.n_errors : 0);
}

/*
* parseWorkerResponse: 解析工作进程返回的状态消息。
*
* 返回整数状态码，可能更新 AH 和/或 te 的字段。
*/
static int
parseWorkerResponse(ArchiveHandle *AH, TocEntry *te,
					const char *msg)
{
	DumpId		dumpId;
	int			nBytes,
				n_errors;
	int			status = 0;

	if (messageStartsWith(msg, "OK "))
	{
		sscanf(msg, "OK %d %d %d%n", &dumpId, &status, &n_errors, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(msg));

		AH->public.n_errors += n_errors;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	return status;
}

/*
* 向某个空闲工作进程分派作业。
*
* te 是要处理的 TocEntry，act 是要对其执行的操作。
* callback 是作业完成时调用的函数。
*
* 如果当前没有可用的工作进程，这将阻塞，
* 并且可能调用之前注册的回调函数。
*/
void
DispatchJobForTocEntry(ArchiveHandle *AH,
					   ParallelState *pstate,
					   TocEntry *te,
					   T_Action act,
					   ParallelCompletionPtr callback,
					   void *callback_data)
{
	int			worker;
	char		buf[256];

	/* 获取一个工作进程，如果没有空闲则等待 */
	while ((worker = GetIdleWorker(pstate)) == NO_SLOT)
		WaitForWorkers(AH, pstate, WFW_ONE_IDLE);

	/* 构造并发送命令字符串 */
	buildWorkerCommand(AH, te, act, buf, sizeof(buf));

	sendMessageToWorker(pstate, worker, buf);

	/* 记住工作进程忙碌，以及它正在处理哪个 TocEntry */
	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].callback = callback;
	pstate->parallelSlot[worker].callback_data = callback_data;
	pstate->te[worker] = te;
}

/*
* 查找空闲工作进程并返回其槽号。
* 如果没有空闲则返回 NO_SLOT。
*/
static int
GetIdleWorker(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_IDLE)
			return i;
	}
	return NO_SLOT;
}

/*
* 如果没有工作进程正在运行则返回 true。
*/
static bool
HasEveryWorkerTerminated(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			return false;
	}
	return true;
}

/*
* 如果每个工作进程都处于 WRKR_IDLE 状态则返回 true。
*/
bool
IsEveryWorkerIdle(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus != WRKR_IDLE)
			return false;
	}
	return true;
}

/*
* 在工作进程要导出的表上获取锁。
*
* 领导者进程已持有 ACCESS SHARE 锁。通常工作进程获取同样的锁
* 没有问题，但如果 pg_dump 之外还有其他进程运行，
* 可能出现死锁：
*
* 1) 领导者导出模式并以 ACCESS SHARE 模式锁定所有表。
* 2) 另一个进程请求 ACCESS EXCLUSIVE 锁（由于领导者持有冲突的
*    ACCESS SHARE 锁而未被授予）。
* 3) 工作进程也请求 ACCESS SHARE 锁以读取该表。
*    工作进程排在 ACCESS EXCLUSIVE 锁请求之后。
* 4) 现在出现死锁，因为领导者实际上在等待工作进程。
*    但服务器无法检测到这一点。
*
* 为了防止无限等待，在工作进程中接触表之前，
* 以 ACCESS SHARE 模式但带 NOWAIT 请求锁。如果未获得锁，
* 则知道有其他进程请求了 ACCESS EXCLUSIVE 锁，
* 于是存在死锁。此时必须使备份失败。
*/
static void
lockTableForWorker(ArchiveHandle *AH, TocEntry *te)
{
	const char *qualId;
	PQExpBuffer query;
	PGresult   *res;

	/* BLOBS 无需处理 */
	if (strcmp(te->desc, "BLOBS") == 0)
		return;

	query = createPQExpBuffer();

	qualId = fmtQualifiedId(te->namespace, te->tag);

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not obtain lock on relation \"%s\"\n"
				 "This usually means that someone requested an ACCESS EXCLUSIVE lock "
				 "on the table after the pg_dump parent process had gotten the "
				 "initial ACCESS SHARE lock on the table.", qualId);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
* WaitForCommands: 工作进程的主例程。
*
* 从领导者读取并执行命令，直到在管道上看到 EOF。
*/
static void
WaitForCommands(ArchiveHandle *AH, int pipefd[2])
{
	char	   *command;
	TocEntry   *te;
	T_Action	act;
	int			status = 0;
	char		buf[256];

	for (;;)
	{
		if (!(command = getMessageFromLeader(pipefd)))
		{
			/* EOF，因此完成 */
			return;
		}

		/* 解码命令 */
		parseWorkerCommand(AH, &te, &act, command);

		if (act == ACT_DUMP)
		{
			/* 在工作进程会话中获取此表的锁 */
			lockTableForWorker(AH, te);

			/* 执行导出命令 */
			status = (AH->WorkerJobDumpPtr) (AH, te);
		}
		else if (act == ACT_RESTORE)
		{
			/* 执行恢复命令 */
			status = (AH->WorkerJobRestorePtr) (AH, te);
		}
		else
			Assert(false);

		/* 将状态返回给领导者 */
		buildWorkerResponse(AH, te, act, status, buf, sizeof(buf));

		sendMessageToLeader(pipefd, buf);

		/* 命令是 pg_malloc'd 的，我们负责 free() 它。 */
		free(command);
	}
}

/*
 * 检查工作进程的状态消息。   
 *
 * 如果 do_wait 为 true，则等待获取状态消息；否则，若无 
 * 可用消息则立即返回。         
 *
 * 收到状态消息时，我们会把状态码传递给回调函数   
 * （由 DispatchJobForTocEntry 指定），然后将工作进程状态
 * 重置为 IDLE。     
 *
 * 收集到状态消息则返回 true，否则为 false。  
 *
 * XXX 是否值得每次调用检查多条状态消息？            
 * 多个工作进程恰好在同一时刻完成似乎不太可能。      
 *               
 */
static bool
ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait)
{
	int			worker;
	char	   *msg;

	/* 尝试收集状态消息 */
	msg = getMessageFromWorker(pstate, do_wait, &worker);

	if (!msg)
	{
		/* 如果 do_wait 为 true，则必在某个套接字上检测到了 EOF */
		if (do_wait)
			pg_fatal("a worker process died unexpectedly");
		return false;
	}

	/* 处理它并更新我们对工作进程状态的认识 */
	if (messageStartsWith(msg, "OK "))
	{
		ParallelSlot *slot = &pstate->parallelSlot[worker];
		TocEntry   *te = pstate->te[worker];
		int			status;

		status = parseWorkerResponse(AH, te, msg);
		slot->callback(AH, te, status, slot->callback_data);
		slot->workerStatus = WRKR_IDLE;
		pstate->te[worker] = NULL;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	/* 释放 getMessageFromWorker 返回的字符串 */
	free(msg);

	return true;
}

/*
* 检查工作进程的状态结果，必要时等待。
*
* 可用的等待模式有：
* WFW_NO_WAIT: 获取任何可用状态，但不阻塞
* WFW_GOT_STATUS: 等待至少一个工作进程完成
* WFW_ONE_IDLE: 等待至少一个工作进程空闲
* WFW_ALL_IDLE: 等待所有工作进程空闲
*
* 任何收到的结果都传递给 DispatchJobForTocEntry 中指定的回调。
*
* 此函数在领导者进程中执行。
 *
*/
void
WaitForWorkers(ArchiveHandle *AH, ParallelState *pstate, WFW_WaitOption mode)
{
	bool		do_wait = false;

	/*
	* 在 GOT_STATUS 模式下，始终阻塞等待消息，
	* 因为在收到消息之前无法返回。在其他模式下，
	* 第一次循环不阻塞。
	*/
	if (mode == WFW_GOT_STATUS)
	{
		/* 断言调用者知道自己在做什么 */
		Assert(!IsEveryWorkerIdle(pstate));
		do_wait = true;
	}

	for (;;)
	{
		/*
		* 检查状态消息，即使不需要阻塞。不过我们不会
		* 太努力地获取所有可用消息，因为不太可能有多个。
		 *
		*/
		if (ListenToWorkers(AH, pstate, do_wait))
		{
			/*
			* 如果收到消息，对于 GOT_STATUS 模式按定义已完成，
			* 并且我们也可以确定至少有一个空闲工作进程。
			* 因此除 ALL_IDLE 模式外都已完成。
			*/
			if (mode != WFW_ALL_IDLE)
				return;
		}

		/* 检查是否必须等待新的状态消息 */
		switch (mode)
		{
			case WFW_NO_WAIT:
				return;			/* 永远不等待 */
			case WFW_GOT_STATUS:
				Assert(false);	/* 不应执行到这里，因为我们已经等待了 */
				break;
			case WFW_ONE_IDLE:
				if (GetIdleWorker(pstate) != NO_SLOT)
					return;
				break;
			case WFW_ALL_IDLE:
				if (IsEveryWorkerIdle(pstate))
					return;
				break;
		}

		/* 循环返回，这次等待某事发生 */
		do_wait = true;
	}
}

/*
 * 从领导者读取一条命令消息，必要时阻塞直到   
 * 有可用消息为止，以 malloc'd 字符串返回。    
 * EOF 返回 NULL。  
 *
 * 此函数在工作进程中执行。          
 */
static char *
getMessageFromLeader(int pipefd[2])
{
	return readMessageFromPipe(pipefd[PIPE_READ]);
}

/*
* 向领导者发送状态消息。
*
* 此函数在工作进程中执行。
*/
static void
sendMessageToLeader(int pipefd[2], const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pipefd[PIPE_WRITE], str, len) != len)
		pg_fatal("could not write to the communication channel: %m");
}

/*
* 等待 "workerset" 中的某个描述符变为可读。
* 出错时返回 -1，否则返回可读描述符的数量。
*/
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

	for (;;)
	{
		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, NULL);

#ifndef WIN32
		if (i < 0 && errno == EINTR)
			continue;
#else
		if (i == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;
#endif
		break;
	}

	return i;
}


/*
 * 检查来自工作进程的消息。     
 *
 * 若有可用消息，将其作为 malloc'd 字符串返回，并把   
 * 发送进程的索引放入 *worker。 
 *
 * 若无消息，在 "do_wait" 为 true 时等待，否则返回 NULL. 
 *
 * 如果在任何套接字上检测到 EOF，我们将返回 NULL。它  
 * 这与无数据可用的情况难以区分，不太理想，但目前  
 * 唯一调用者可接受。      
 *
 * 此函数在领导者进程中执行。         
 */
static char *
getMessageFromWorker(ParallelState *pstate, bool do_wait, int *worker)
{
	int			i;
	fd_set		workerset;
	int			maxFd = -1;
	struct timeval nowait = {0, 0};

	/* 为 select() 构造套接字描述符位图 */
	FD_ZERO(&workerset);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		FD_SET(pstate->parallelSlot[i].pipeRead, &workerset);
		if (pstate->parallelSlot[i].pipeRead > maxFd)
			maxFd = pstate->parallelSlot[i].pipeRead;
	}

	if (do_wait)
	{
		i = select_loop(maxFd, &workerset);
		Assert(i != 0);
	}
	else
	{
		if ((i = select(maxFd + 1, &workerset, NULL, NULL, &nowait)) == 0)
			return NULL;
	}

	if (i < 0)
		pg_fatal("%s() failed: %m", "select");

	for (i = 0; i < pstate->numWorkers; i++)
	{
		char	   *msg;

		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		if (!FD_ISSET(pstate->parallelSlot[i].pipeRead, &workerset))
			continue;

		/*
		* 读取消息（如果有）。如果套接字因 EOF 而就绪，
		* 则返回 NULL（并且套接字保持就绪状态，因此
		* 条件将持续存在）。
		*
		* 注意：因为这是阻塞读取，如果只有部分消息可用则会等待。
		* 长时间等待不好，但由于工作进程状态消息很短且始终
		* 在一次操作中发送，在实践中不应成为问题。
		 *
		*/
		msg = readMessageFromPipe(pstate->parallelSlot[i].pipeRead);
		*worker = i;
		return msg;
	}
	Assert(false);
	return NULL;
}

/*
* 向指定的工作进程发送命令消息。
*
* 此函数在领导者进程中执行。
*/
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	int			len = strlen(str) + 1;

	if (pipewrite(pstate->parallelSlot[worker].pipeWrite, str, len) != len)
	{
		pg_fatal("could not write to the communication channel: %m");
	}
}

/*
 * 从指定管道（fd）读取一条消息，必要时阻塞直到   
 * 有可用消息为止，以 malloc'd 字符串返回。    
 * EOF 返回 NULL。  
 *
 * 通道上的"消息"只是以 null 结尾的字符串。    
 */
static char *
readMessageFromPipe(int fd)
{
	char	   *msg;
	int			msgsize,
				bufsize;
	int			ret;

	/*
	* 理论上，如果让 piperead() 一次读取多个字节，可能会得到
	* 多条消息的片段。（这实际上不可能发生，因为领导者和工作进程
	* 都不会在不等待回复的情况下发送多条消息，但我们不想在这里
	* 假设这一点。）为简单起见，每次读取一个字节直到遇到终止符
	* '\0'。此方法效率稍低，但由于仅用于相对较短的
	* 命令和状态字符串，应该无所谓。
	 *
	*/
	bufsize = 64;				/* 可为任意值 */
	msg = (char *) pg_malloc(bufsize);
	msgsize = 0;
	for (;;)
	{
		Assert(msgsize < bufsize);
		ret = piperead(fd, msg + msgsize, 1);
		if (ret <= 0)
			break;				/* 错误或连接关闭 */

		Assert(ret == 1);

		if (msg[msgsize] == '\0')
			return msg;			/* 收集了完整消息 */

		msgsize++;
		if (msgsize == bufsize) /* 需要时扩大缓冲区 */
		{
			bufsize += 16;		/* 可为任意值 */
			msg = (char *) pg_realloc(msg, bufsize);
		}
	}

	/* 另一端已关闭连接 */
	pg_free(msg);
	return NULL;
}

#ifdef WIN32

/*
* 这是 Windows 上 pipe(2) 的替代版本，允许管道句柄
* 在 select() 中使用。
*
* 管道上的读写必须通过 piperead()/pipewrite() 进行。
*
* 为与 Unix 一致，我们将返回的句柄声明为 "int"。
* 即使在 WIN64 上也 OK，因为系统句柄不超过 32 位宽，
* 但确实需要进行一些类型转换。
*/
static int
pgpipe(int handles[2])
{
	pgsocket	s,
				tmp_sock;
	struct sockaddr_in serv_addr;
	int			len = sizeof(serv_addr);

	/* 这里必须使用 Unix 套接字无效文件描述符值。 */
	handles[0] = handles[1] = -1;

	/*
	* 设置监听套接字
	*/
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create socket: error code %d",
					 WSAGetLastError());
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = pg_hton16(0);
	serv_addr.sin_addr.s_addr = pg_hton32(INADDR_LOOPBACK);
	if (bind(s, (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not bind: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (listen(s, 1) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not listen: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (getsockname(s, (SOCKADDR *) &serv_addr, &len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: %s() failed: error code %d", "getsockname",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}

	/*
	* 设置管道句柄
	*/
	if ((tmp_sock = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create second socket: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	handles[1] = (int) tmp_sock;

	if (connect(handles[1], (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not connect socket: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	if ((tmp_sock = accept(s, (SOCKADDR *) &serv_addr, &len)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not accept connection: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	handles[0] = (int) tmp_sock;

	closesocket(s);
	return 0;
}

#endif							/* WIN32 */
