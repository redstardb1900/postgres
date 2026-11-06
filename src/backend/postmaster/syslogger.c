/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * 系统日志进程（syslogger）出现于 Postgres 8.0。            
 * 它经由管道重定向，捕获 postmaster、后端及其他     
 * 子进程的全部 stderr 输出，并写入一组日志     
 * postgresql.conf 中可配置日志文件的大小和时效限制。 
 * 达到或超过限制后，当前日志文件即被关闭     
 * 并创建新文件（即轮转）。                         
 * 日志文件存于子目录中（可在 postgresql.conf   
 * 里配置），命名方案可由用户选择。        
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Copyright (c) 2004-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/syslogger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtime.h"
#include "port/pg_bitutils.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"

/*
 * 我们 read() 到大小两倍于块的临时缓冲，这样处理后的   
 * 残留片段可以移到开头，且仍有空间读入完整的一块。 
 *                           
 */
#define READ_BUF_SIZE (2 * PIPE_CHUNK_SIZE)

/* 日志轮转信号文件路径，相对于 $PGDATA */
#define LOGROTATE_SIGNAL_FILE	"logrotate"


/*
 * GUC 参数。logging_collector 在 postmaster 启动后无法        
 * 更改，其余可在 SIGHUP 时改。   
 */
bool		Logging_collector = false;
int			Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
int			Log_RotationSize = 10 * 1024;
char	   *Log_directory = NULL;
char	   *Log_filename = NULL;
bool		Log_truncate_on_rotation = false;
int			Log_file_mode = S_IRUSR | S_IWUSR;

extern bool redirection_done;

/*
 * 私有状态 
 */
static pg_time_t next_rotation_time;
static bool pipe_eof_seen = false;
static bool rotation_disabled = false;
static FILE *syslogFile = NULL;
static FILE *csvlogFile = NULL;
static FILE *jsonlogFile = NULL;
NON_EXEC_STATIC pg_time_t first_syslogger_file_time = 0;
static char *last_sys_file_name = NULL;
static char *last_csv_file_name = NULL;
static char *last_json_file_name = NULL;

/*
 * 保存来自不同后端的残缺消息的缓冲区。      
 *
 * 这些缓冲区保存在 NBUFFER_LISTS 个链表中，给定来源 pid  
 * 的表项位于编号 (pid % NBUFFER_LISTS) 的链表内，以便     
 * 减少处理任一到来消息所需检查的表项数量。          
 * 同一个来源 pid 绝不能存在多个表项。              
 *
 * 非活动缓冲区不从链表移除，只是留着供复用。       
 * 非活动缓冲区的 pid 为 0，data 内容未定义。      
 */
typedef struct
{
	int32		pid;			/* 源进程的 pid      */
	StringInfoData data;		/* 累积数据，StringInfo 类型  */
} save_buffer;

#define NBUFFER_LISTS 256
static List *buffer_lists[NBUFFER_LISTS];

/* EXEC_BACKEND 情形下必须导出……有点烦人      */
#ifndef WIN32
int			syslogPipe[2] = {-1, -1};
#else
HANDLE		syslogPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle = 0;
static CRITICAL_SECTION sysloggerSection;
#endif

/*
 * 由中断处理程序置位的标志，供主循环稍后处理。 
 */
static volatile sig_atomic_t rotation_requested = false;


/* 本地子例程   */
#ifdef EXEC_BACKEND
static int	syslogger_fdget(FILE *file);
static FILE *syslogger_fdopen(int fd);
static pid_t syslogger_forkexec(void);
static void syslogger_parseArgs(int argc, char *argv[]);
#endif
NON_EXEC_STATIC void SysLoggerMain(int argc, char *argv[]) pg_attribute_noreturn();
static void process_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static void flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer);
static FILE *logfile_open(const char *filename, const char *mode,
						  bool allow_errors);

#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(bool time_based_rotation, int size_rotation_for);
static bool logfile_rotate_dest(bool time_based_rotation,
								int size_rotation_for, pg_time_t fntime,
								int target_dest, char **last_file_name,
								FILE **logFile);
static char *logfile_getname(pg_time_t timestamp, const char *suffix);
static void set_next_rotation_time(void);
static void sigUsr1Handler(SIGNAL_ARGS);
static void update_metainfo_datafile(void);


/*
 * syslogger 进程的主入口点       
 * argc/argv 参数仅在 EXEC_BACKEND 情形下有效。   
 */
NON_EXEC_STATIC void
SysLoggerMain(int argc, char *argv[])
{
#ifndef WIN32
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;
#endif
	char	   *currentLogDir;
	char	   *currentLogFilename;
	int			currentLogRotationAge;
	pg_time_t	now;
	WaitEventSet *wes;

	now = MyStartTime;

#ifdef EXEC_BACKEND
	syslogger_parseArgs(argc, argv);
#endif							/* EXEC_BACKEND */

	MyBackendType = B_LOGGER;
	init_ps_display(NULL);

	/*
	 * 如果我们是重启而来，stderr 已经被重定向             
	 * 进自己的输入管道。这毫无用处，更会干扰      
	 * 管道 EOF 的检测。把 stderr 指向 /dev/null。这里假定   
	 * syslogger 产生的所有重要消息都经过 elog.c                
	 * 并被送往 write_syslogger_file。                         
	 */
	if (redirection_done)
	{
		int			fd = open(DEVNULL, O_WRONLY, 0);

		/*
		 * 这些 close 看似多余，实则不然：即使 open 失败，
		 * 我们也要确保管道被关闭。stderr 指向空处尚可  
		 * 活得下去，但绝不能让多余的管道输入描述符     
		 * 悬在那里。                                     
		 *
		 * 既然只是想把它们重置为 DEVNULL，就无需            
		 * 检查这里 close/dup2 调用的失败：就算失败，         
		 * 描述符多半已被关闭，后续写入反正都会          
		 * 被丢弃。                                 
		 */
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		if (fd != -1)
		{
			(void) dup2(fd, STDOUT_FILENO);
			(void) dup2(fd, STDERR_FILENO);
			close(fd);
		}
	}

	/*
	 * syslogger 自身的 stderr 无法用作 syslogPipe，若没有        
	 * 刚刚关闭它，就设回文本。（SubPostmasterMain 
	 * 内设二进制） 
	 */
#ifdef WIN32
	else
		_setmode(STDERR_FILENO, _O_TEXT);
#endif

	/*
	 * 同时关闭我们自己持有的管道写端。这样做是        
	 * 为了确保能正确检测管道 EOF。（但注意在重启          
	 * 场景下，postmaster 已做过。） 
	 */
#ifndef WIN32
	if (syslogPipe[1] >= 0)
		close(syslogPipe[1]);
	syslogPipe[1] = -1;
#else
	if (syslogPipe[1])
		CloseHandle(syslogPipe[1]);
	syslogPipe[1] = 0;
#endif

	/*
	 * 恰当地接受或忽略 postmaster 可能发来的信号     
	 *
	 * 注意：我们忽略所有终止信号，只有当所有上游        
	 * 进程都消失时才退出，以确保不会漏掉崩溃后端        
	 * 的临终遗言…
	 */

	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, sigUsr1Handler);	/* 请求轮转日志   */
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * 重置若干 postmaster 接受而此处不接受的信号      
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

#ifdef WIN32
	/* 启动单独的数据传输线程     */
	InitializeCriticalSection(&sysloggerSection);
	EnterCriticalSection(&sysloggerSection);

	threadHandle = (HANDLE) _beginthreadex(NULL, 0, pipeThread, NULL, 0, NULL);
	if (threadHandle == 0)
		elog(FATAL, "could not create syslogger data transfer thread: %m");
#endif							/* WIN32 */

	/*
	 * 记住活动日志文件的文件名。这些名字根据参考时间  
	 * 重新推算，因为在 EXEC_BACKEND 情形下只传 pg_time_t   
	 * 比传整个文件路径便宜得多。            
	 */
	last_sys_file_name = logfile_getname(first_syslogger_file_time, NULL);
	if (csvlogFile != NULL)
		last_csv_file_name = logfile_getname(first_syslogger_file_time, ".csv");
	if (jsonlogFile != NULL)
		last_json_file_name = logfile_getname(first_syslogger_file_time, ".json");

	/* 记住活动日志文件参数     */
	currentLogDir = pstrdup(Log_directory);
	currentLogFilename = pstrdup(Log_filename);
	currentLogRotationAge = Log_RotationAge;
	/* 设定下次计划轮转时间 */
	set_next_rotation_time();
	update_metainfo_datafile();

	/*
	 * 像 postmaster 那样重置 whereToSendOutput（但在我们被 fork  
	 * 出来的时刻它尚未这么做）。这可避免 syslogger 自身的
	 * 消息重复输出。 
	 */
	whereToSendOutput = DestNone;

	/*
	 * 建立可复用的 WaitEventSet 对象，等待我们的 latch        
	 * （Windows 除外）与套接字。
	 *
	 * 与其他 postmaster 子进程不同，我们忽略 postmaster      
	 * 死亡，因为要收集完所有后端的最终日志输出，        
	 * 最后退出。做法是持续运行，直至 syslog 管道      
	 * 出现 EOF——这意味着包括 postmaster 在内的       
	 * 其余后端均已退出。
	 */
	wes = CreateWaitEventSet(CurrentMemoryContext, 2);
	AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
#ifndef WIN32
	AddWaitEventToSet(wes, WL_SOCKET_READABLE, syslogPipe[0], NULL, NULL);
#endif

	/* 主工作循环  */
	for (;;)
	{
		bool		time_based_rotation = false;
		int			size_rotation_for = 0;
		long		cur_timeout;
		WaitEvent	event;

#ifndef WIN32
		int			rc;
#endif

		/* 清掉任何已挂起的唤醒    */
		ResetLatch(MyLatch);

		/*
		 * 处理最近收到的请求或信号。           
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * 检查 postgresql.conf 的日志目录或文件名模式 
			 * 有无变化。若有则强制轮转，确保日志写到
			 * 正确的位置。                      
			 */
			if (strcmp(Log_directory, currentLogDir) != 0)
			{
				pfree(currentLogDir);
				currentLogDir = pstrdup(Log_directory);
				rotation_requested = true;

				/*
				 * 另外，若目录不存在则新建；忽略错误     
				 */
				(void) MakePGDirectory(Log_directory);
			}
			if (strcmp(Log_filename, currentLogFilename) != 0)
			{
				pfree(currentLogFilename);
				currentLogFilename = pstrdup(Log_filename);
				rotation_requested = true;
			}

			/*
			 * 如果 CSVLOG 输出刚被打开或关闭，则强制轮转， 
			 * 以便相应地打开或关闭 csvlogFile。    
			 */
			if (((Log_destination & LOG_DESTINATION_CSVLOG) != 0) !=
				(csvlogFile != NULL))
				rotation_requested = true;

			/*
			 * 若 JSONLOG 输出刚被打开或关闭，则强制轮转，
			 * 以便相应地打开或关闭 jsonlogFile。        
			 */
			if (((Log_destination & LOG_DESTINATION_JSONLOG) != 0) !=
				(jsonlogFile != NULL))
				rotation_requested = true;

			/*
			 * 若轮转时间参数有变，重置下一次轮转时间， 
			 * 但不立即强制轮转。            
			 */
			if (currentLogRotationAge != Log_RotationAge)
			{
				currentLogRotationAge = Log_RotationAge;
				set_next_rotation_time();
			}

			/*
			 * 如果之前发生过导致轮转被禁用的失败，则 
			 * SIGHUP 后重启轮转尝试，并立即强制。
			 */
			if (rotation_disabled)
			{
				rotation_disabled = false;
				rotation_requested = true;
			}

			/*
			 * 重新加载配置时强制重写最后一个日志文件名。
			 * 即使 rotation_requested 为假，log_destination 也可能 
			 * 已经改变，我们不想等到下一次文件轮转。     
			 */
			update_metainfo_datafile();
		}

		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			/* 到时间就做日志轮转        */
			now = (pg_time_t) time(NULL);
			if (now >= next_rotation_time)
				rotation_requested = time_based_rotation = true;
		}

		if (!rotation_requested && Log_RotationSize > 0 && !rotation_disabled)
		{
			/* 文件太大就做轮转         */
			if (ftell(syslogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_STDERR;
			}
			if (csvlogFile != NULL &&
				ftell(csvlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_CSVLOG;
			}
			if (jsonlogFile != NULL &&
				ftell(jsonlogFile) >= Log_RotationSize * 1024L)
			{
				rotation_requested = true;
				size_rotation_for |= LOG_DESTINATION_JSONLOG;
			}
		}

		if (rotation_requested)
		{
			/*
			 * 两个值都为零时强制轮转。这意味着请求来自  
			 * pg_rotate_logfile() 或 "pg_ctl logrotate"。         
			 */
			if (!time_based_rotation && size_rotation_for == 0)
				size_rotation_for = LOG_DESTINATION_STDERR |
					LOG_DESTINATION_CSVLOG |
					LOG_DESTINATION_JSONLOG;
			logfile_rotate(time_based_rotation, size_rotation_for);
		}

		/*
		 * 计算下次按时间轮转还有多久，以免睡过头。  
		 * 我们假定上面取到的 "now" 值仍然足够接近。    
		 * 注意必须等 logfile_rotate() 调用后才能计算，        
		 * 因为它会推进 next_rotation_time。                   
		 *                    
		 *
		 * 另注意计算超时值时要提防溢出：若 Log_RotationAge  
		 * 设得很大，next_rotation_time 可能位于 INT_MAX 毫秒      
		 * 之后的未来。那种情况下最多等 INT_MAX 毫秒，      
		 * 然后重试。                               
		 */
		if (Log_RotationAge > 0 && !rotation_disabled)
		{
			pg_time_t	delay;

			delay = next_rotation_time - now;
			if (delay > 0)
			{
				if (delay > INT_MAX / 1000)
					delay = INT_MAX / 1000;
				cur_timeout = delay * 1000L;	/* msec */
			}
			else
				cur_timeout = 0;
		}
		else
			cur_timeout = -1L;

		/*
		 * 睡到有事可做为止           
		 */
#ifndef WIN32
		rc = WaitEventSetWait(wes, cur_timeout, &event, 1,
							  WAIT_EVENT_SYSLOGGER_MAIN);

		if (rc == 1 && event.events == WL_SOCKET_READABLE)
		{
			int			bytesRead;

			bytesRead = read(syslogPipe[0],
							 logbuffer + bytes_in_logbuffer,
							 sizeof(logbuffer) - bytes_in_logbuffer);
			if (bytesRead < 0)
			{
				if (errno != EINTR)
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from logger pipe: %m")));
			}
			else if (bytesRead > 0)
			{
				bytes_in_logbuffer += bytesRead;
				process_pipe_input(logbuffer, &bytes_in_logbuffer);
				continue;
			}
			else
			{
				/*
				 * select() 报告可读却读到零字节即管道 EOF： 
				 * 也就是说不再有进程持有管道写端。因此     
				 * postmaster 与所有后端均已关闭，工作到此   
				 * 结束。                                       
				 */
				pipe_eof_seen = true;

				/* 若还有残留数据就立刻刷出去        */
				flush_pipe_input(logbuffer, &bytes_in_logbuffer);
			}
		}
#else							/* WIN32 */

		/*
		 * 在 Windows 上，数据传输与管道 EOF 检测交给        
		 * 单独的线程。主线程只是醒来处理 SIGHUP            
		 * 和轮转条件。      
		 *
		 * 服务端代码一般不是线程安全的，只要不在睡眠， 
		 * 我们就进入临界区，保证任一时刻只有一个线程    
		 * 处于活动状态。       
		 */
		LeaveCriticalSection(&sysloggerSection);

		(void) WaitEventSetWait(wes, cur_timeout, &event, 1,
								WAIT_EVENT_SYSLOGGER_MAIN);

		EnterCriticalSection(&sysloggerSection);
#endif							/* WIN32 */

		if (pipe_eof_seen)
		{
			/*
			 * 在真正的 stderr 上看到这条消息很烦人，          
			 * 故定为 DEBUG1 平时不输出。 
			 */
			ereport(DEBUG1,
					(errmsg_internal("logger shutting down")));

			/*
			 * syslogger 的正常退出在这里。注意我们    
			 * 故意不在退出前关闭 syslogFile；这是为了          
			 * 给 proc_exit 内部可能产生的 elog 消息           
			 * 留有余地。常规的 exit() 会负责刷新             
			 * 并关闭 stdio 通道。  
			 */
			proc_exit(0);
		}
	}
}

/*
 * postmaster 启动 syslogger 子进程的子例程。  
 */
int
SysLogger_Start(void)
{
	pid_t		sysloggerPid;
	char	   *filename;

	if (!Logging_collector)
		return 0;

	/*
	 * 首次执行时，创建用于接收 stderr 输出的管道。  
	 *        
	 *
	 * 如果 syslogger 崩溃而需要重启，我们继续使用           
	 * 同一条管道（实际上也必须如此：现存后端仍在        
	 * 写它）。    
	 *
	 * 这意味着 postmaster 必须一直持有管道读端的          
	 * 打开状态，以便把它传给重生的 syslogger。              
	 * 这有点笨拙，但别无选择。     
	 *
	 * 另注意我们懒得调用 Reserve/ReleaseExternalFD          
	 * 来清点管道占用的 FD。在 postmaster 或 syslogger          
	 * 进程中没必要精确记账，而且管道两端                    
	 * 在所有其他 postmaster 子进程中都会被关闭。 
	 */
#ifndef WIN32
	if (syslogPipe[0] < 0)
	{
		if (pipe(syslogPipe) < 0)
			ereport(FATAL,
					(errcode_for_socket_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#else
	if (!syslogPipe[0])
	{
		SECURITY_ATTRIBUTES sa;

		memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&syslogPipe[0], &syslogPipe[1], &sa, 32768))
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create pipe for syslog: %m")));
	}
#endif

	/*
	 * 若日志目录不存在则创建；忽略错误  
	 */
	(void) MakePGDirectory(Log_directory);

	/*
	 * 初始日志文件直接由 postmaster 创建，以验证             
	 * Log_directory 可写。我们保存参考时间，                    
	 * 供 syslogger 子进程重新计算该文件名。   
	 *
	 * 在 syslogger 重启期间重做这件事看似有点奇怪，       
	 * 但必须如此，因为 postmaster 在上次 fork 之后         
	 * 已关闭 syslogFile（而且记住旧文件本来也不对）。      
	 * 注意我们这里总是追加，不会覆盖任何已有文件。     
	 * 这与常规规则一致，因为按定义这不是                   
	 * 按时间的轮转。 
	 */
	first_syslogger_file_time = time(NULL);

	filename = logfile_getname(first_syslogger_file_time, NULL);

	syslogFile = logfile_open(filename, "a", false);

	pfree(filename);

	/*
	 * 若启用 CSV 日志，初始 CSV 日志文件亦然。（注意     
	 * 即使名义上只启用 CSV，我们也会打开 syslogFile，   
	 * 因为某些代码路径仍会写入 syslogFile。）  
	 */
	if (Log_destination & LOG_DESTINATION_CSVLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".csv");

		csvlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

	/*
	 * 若启用 JSON 日志，初始 JSON 日志文件亦然。（注意    
	 * 即使名义上只启用 JSON，我们也会打开 syslogFile，   
	 * 因为某些代码路径仍会写入 syslogFile。）  
	 */
	if (Log_destination & LOG_DESTINATION_JSONLOG)
	{
		filename = logfile_getname(first_syslogger_file_time, ".json");

		jsonlogFile = logfile_open(filename, "a", false);

		pfree(filename);
	}

#ifdef EXEC_BACKEND
	switch ((sysloggerPid = syslogger_forkexec()))
#else
	switch ((sysloggerPid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork system logger: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* postmaster 子进程… */
			InitPostmasterChild();

			/* 关闭 postmaster 的套接字 */
			ClosePostmasterPorts(true);

			/* 同时断开我们与 postmaster 共享内存的连接     */
			dsm_detach_all();
			PGSharedMemoryDetach();

			/* 执行工作*/
			SysLoggerMain(0, NULL);
			break;
#endif

		default:
			/* postmaster 中成功   */

			/* 现在重定向 stderr（若尚未做）    */
			if (!redirection_done)
			{
#ifdef WIN32
				int			fd;
#endif

				/*
				 * 重定向时留下踪迹，以防用户忘记重定向     
				 * 已生效，而只去查看原始的 stderr             
				 * 目标文件。             
				 */
				ereport(LOG,
						(errmsg("redirecting log output to logging collector process"),
						 errhint("Future log output will appear in directory \"%s\".",
								 Log_directory)));

#ifndef WIN32
				fflush(stdout);
				if (dup2(syslogPipe[1], STDOUT_FILENO) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stdout: %m")));
				fflush(stderr);
				if (dup2(syslogPipe[1], STDERR_FILENO) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				/* 现在管道写端已用完。                  */
				close(syslogPipe[1]);
				syslogPipe[1] = -1;
#else

				/*
				 * 以二进制模式打开管道，并确保 stderr 被      
				 * dup 进来之后仍是二进制，以免扰乱管道     
				 * 分块协议。   
				 */
				fflush(stderr);
				fd = _open_osfhandle((intptr_t) syslogPipe[1],
									 _O_APPEND | _O_BINARY);
				if (dup2(fd, STDERR_FILENO) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				close(fd);
				_setmode(STDERR_FILENO, _O_BINARY);

				/*
				 * 现在管道写端已用完。                 
				 * 绝不能调用 CloseHandle()，因为前面的       
				 * close() 已关闭底层句柄。     
				 */
				syslogPipe[1] = 0;
#endif
				redirection_done = true;
			}

			/* postmaster 从不写这些文件；把它们关掉 */
			fclose(syslogFile);
			syslogFile = NULL;
			if (csvlogFile != NULL)
			{
				fclose(csvlogFile);
				csvlogFile = NULL;
			}
			if (jsonlogFile != NULL)
			{
				fclose(jsonlogFile);
				jsonlogFile = NULL;
			}
			return (int) sysloggerPid;
	}

	/* 绝不应走到这里      */
	return 0;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_fdget() -
 *
 * 工具性包装：获取已打开错误输出文件的描述符。   
 * 在构建 fork 日志收集器的命令时使用。                 
 */
static int
syslogger_fdget(FILE *file)
{
#ifndef WIN32
	if (file != NULL)
		return fileno(file);
	else
		return -1;
#else
	if (file != NULL)
		return (int) _get_osfhandle(_fileno(file));
	else
		return 0;
#endif							/* WIN32 */
}

/*
 * syslogger_fdopen() -
 *
 * 工具性包装函数：用给定的文件描述符重新打开错误
 * 输出文件。在 fork 出的日志收集器解析参数时使用。  
 */
static FILE *
syslogger_fdopen(int fd)
{
	FILE	   *file = NULL;

#ifndef WIN32
	if (fd != -1)
	{
		file = fdopen(fd, "a");
		setvbuf(file, NULL, PG_IOLBF, 0);
	}
#else							/* WIN32 */
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND | _O_TEXT);
		if (fd > 0)
		{
			file = fdopen(fd, "a");
			setvbuf(file, NULL, PG_IOLBF, 0);
		}
	}
#endif							/* WIN32 */

	return file;
}

/*
 * syslogger_forkexec() -
 *
 * 为 syslogger 进程准备参数表，然后 fork 并 exec        
 */
static pid_t
syslogger_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;
	char		filenobuf[32];
	char		csvfilenobuf[32];
	char		jsonfilenobuf[32];

	av[ac++] = "postgres";
	av[ac++] = "--forklog";
	av[ac++] = NULL;			/* 由 postmaster_forkexec 填写   */

	/* 静态变量（write_backend_variables 不传递的那些）   */
	snprintf(filenobuf, sizeof(filenobuf), "%d",
			 syslogger_fdget(syslogFile));
	av[ac++] = filenobuf;
	snprintf(csvfilenobuf, sizeof(csvfilenobuf), "%d",
			 syslogger_fdget(csvlogFile));
	av[ac++] = csvfilenobuf;
	snprintf(jsonfilenobuf, sizeof(jsonfilenobuf), "%d",
			 syslogger_fdget(jsonlogFile));
	av[ac++] = jsonfilenobuf;

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * syslogger_parseArgs() -
 *
 * 从参数表提取 exec 启动的 syslogger 的数据      
 */
static void
syslogger_parseArgs(int argc, char *argv[])
{
	int			fd;

	Assert(argc == 6);
	argv += 3;

	/*
	 * 重新打开由 SysLogger_Start() 打开的错误输出文件。     
	 *
	 * 我们预期这总会成功，未免过于乐观；但即便失败， 
	 * 我们也没什么办法报告问题。按现在的写法，       
	 * 失败后我们会因空指针解引用而直接崩溃……          
	 */
	fd = atoi(*argv++);
	syslogFile = syslogger_fdopen(fd);
	fd = atoi(*argv++);
	csvlogFile = syslogger_fdopen(fd);
	fd = atoi(*argv++);
	jsonlogFile = syslogger_fdopen(fd);
}
#endif							/* EXEC_BACKEND */


/* --------------------------------
 *		管道协议处理    
 * --------------------------------
 */

/*
 * 处理经由 syslogger 管道收到的数据。  
 *
 * 本例程解释日志管道协议：日志消息以（但愿原子的） 
 * 块为单位发送——块的识别与重组都在这里完成。        
 *
 * 协议头部以两个 nul 字节开头，随后是 16 位长度、         
 * 发送进程的 pid，以及指示是否为消息最后一块的       
 * 标志。不完整的块会保存下来等待继续读取，               
 * 非末块则持续累积，直到收到末块。                        
 *
 * 一切只为避开两个问题：  
 * . 残缺消息被写进日志文件（搅乱轮转），           
 * . 不同后端的消息交错（消息变乱）。                      
 *
 * 任何非协议消息都会直接写出。不过它们应当只           
 * 来自非 PostgreSQL 来源（如往 stderr 写的第三方库）。       
 *         
 *
 * logbuffer 是数据输入缓冲，*bytes_in_logbuffer 是                
 * 其中的字节数。退出时，尚未吃掉的数据会左移至     
 * logbuffer 开头，并更新其字节数。   
 */
static void
process_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	char	   *cursor = logbuffer;
	int			count = *bytes_in_logbuffer;
	int			dest = LOG_DESTINATION_STDERR;

	/* 只要数据够一个头部就继续处理……   */
	while (count >= (int) (offsetof(PipeProtoHeader, data) + 1))
	{
		PipeProtoHeader p;
		int			chunklen;
		bits8		dest_flags;

		/* 头部有效吗？         */
		memcpy(&p, cursor, offsetof(PipeProtoHeader, data));
		dest_flags = p.flags & (PIPE_PROTO_DEST_STDERR |
								PIPE_PROTO_DEST_CSVLOG |
								PIPE_PROTO_DEST_JSONLOG);
		if (p.nuls[0] == '\0' && p.nuls[1] == '\0' &&
			p.len > 0 && p.len <= PIPE_MAX_PAYLOAD &&
			p.pid != 0 &&
			pg_popcount((char *) &dest_flags, 1) == 1)
		{
			List	   *buffer_list;
			ListCell   *cell;
			save_buffer *existing_slot = NULL,
					   *free_slot = NULL;
			StringInfo	str;

			chunklen = PIPE_HEADER_SIZE + p.len;

			/* 若整块尚未收齐则跳出循环                  */
			if (count < chunklen)
				break;

			if ((p.flags & PIPE_PROTO_DEST_STDERR) != 0)
				dest = LOG_DESTINATION_STDERR;
			else if ((p.flags & PIPE_PROTO_DEST_CSVLOG) != 0)
				dest = LOG_DESTINATION_CSVLOG;
			else if ((p.flags & PIPE_PROTO_DEST_JSONLOG) != 0)
				dest = LOG_DESTINATION_JSONLOG;
			else
			{
				/* 经过头部校验后这绝不应再发生           */
				Assert(false);
			}

			/* 为该来源 pid 找到已有的缓冲区      */
			buffer_list = buffer_lists[p.pid % NBUFFER_LISTS];
			foreach(cell, buffer_list)
			{
				save_buffer *buf = (save_buffer *) lfirst(cell);

				if (buf->pid == p.pid)
				{
					existing_slot = buf;
					break;
				}
				if (buf->pid == 0 && free_slot == NULL)
					free_slot = buf;
			}

			if ((p.flags & PIPE_PROTO_IS_LAST) == 0)
			{
				/*
				 * 把完整非末块存入按 pid 分的缓冲区    
				 */
				if (existing_slot != NULL)
				{
					/* 把该块并入前面各块的数据    */
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
				else
				{
					/* 消息的首块，保存到新缓冲区中   */
					if (free_slot == NULL)
					{
						/*
						 * 需空槽而链表没有时，新建一个并     
						 * 用它扩展链表。                           
						 */
						free_slot = palloc(sizeof(save_buffer));
						buffer_list = lappend(buffer_list, free_slot);
						buffer_lists[p.pid % NBUFFER_LISTS] = buffer_list;
					}
					free_slot->pid = p.pid;
					str = &(free_slot->data);
					initStringInfo(str);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
				}
			}
			else
			{
				/*
				 * 末块——并入该 pid 已保存的数据，无论     
				 * 哪种情况都把整体写出。    
				 */
				if (existing_slot != NULL)
				{
					str = &(existing_slot->data);
					appendBinaryStringInfo(str,
										   cursor + PIPE_HEADER_SIZE,
										   p.len);
					write_syslogger_file(str->data, str->len, dest);
					/* 标记缓冲区未用，回收字符串存储      */
					existing_slot->pid = 0;
					pfree(str->data);
				}
				else
				{
					/* 显然整条消息就是一块。           */
					write_syslogger_file(cursor + PIPE_HEADER_SIZE, p.len,
										 dest);
				}
			}

			/* 本块处理完毕             */
			cursor += chunklen;
			count -= chunklen;
		}
		else
		{
			/* 处理非协议数据     */

			/*
			 * 寻找协议头部的起点。若找到，把之前的数据 
			 * 全部写出并重复循环。若没找到，则全部写出
			 * 并跳出循环。（注意：要尽可能全部写出，     
			 * 以免非协议消息被分散到多个日志文件。        
			 * 我们预期在许多场景中，非协议消息会        
			 * 随一次 read() 全部到达，只要可能就尊重        
			 * read() 边界。）              
			 */
			for (chunklen = 1; chunklen < count; chunklen++)
			{
				if (cursor[chunklen] == '\0')
					break;
			}
			/* 退回到 stderr 日志作为目标            */
			write_syslogger_file(cursor, chunklen, LOG_DESTINATION_STDERR);
			cursor += chunklen;
			count -= chunklen;
		}
	}

	/* 没有收齐整块时，把缓冲中剩余内容左对齐            */
	if (count > 0 && cursor != logbuffer)
		memmove(logbuffer, cursor, count);
	*bytes_in_logbuffer = count;
}

/*
 * 刷出所有缓冲数据   
 *
 * 目前这只在 syslogger 关闭时使用，但其他时候            
 * 也许用得上，所以它小心地把状态留在干净的样子。    
 */
static void
flush_pipe_input(char *logbuffer, int *bytes_in_logbuffer)
{
	int			i;

	/* 倾倒任何未完成的协议消息  */
	for (i = 0; i < NBUFFER_LISTS; i++)
	{
		List	   *list = buffer_lists[i];
		ListCell   *cell;

		foreach(cell, list)
		{
			save_buffer *buf = (save_buffer *) lfirst(cell);

			if (buf->pid != 0)
			{
				StringInfo	str = &(buf->data);

				write_syslogger_file(str->data, str->len,
									 LOG_DESTINATION_STDERR);
				/* 标记缓冲区未用，回收字符串存储      */
				buf->pid = 0;
				pfree(str->data);
			}
		}
	}

	/*
	 * 把管道中剩余的数据原样刷出；懒得费心去掉      
	 * 其中可能存在的协议头部。             
	 */
	if (*bytes_in_logbuffer > 0)
		write_syslogger_file(logbuffer, *bytes_in_logbuffer,
							 LOG_DESTINATION_STDERR);
	*bytes_in_logbuffer = 0;
}


/* --------------------------------
 *		日志例程    
 * --------------------------------
 */

/*
 * 向当前日志文件写入文本       
 *
 * 导出此函数是为了让 elog.c 在 MyBackendType 为 B_LOGGER          
 * 时调用。syslogger 进程由此能记录自己的 elog 消息，  
 * 尽管它的 stderr 并不指向 syslog 管道。        
 */
void
write_syslogger_file(const char *buffer, int count, int destination)
{
	int			rc;
	FILE	   *logfile;

	/*
	 * 若要求写入结构化日志文件，但它尚未打开，       
	 * 就转储到 syslogFile（它总是打开）。这可能             
	 * 发生在 postmaster 启动后才启用结构化输出、而我们     
	 * 一直无法打开 logFile 的情形。参数变更期间还存在   
	 * 竞态：后端可能在我们打开 logFile 之前或关闭     
	 * 之后发来结构化输出。把格式化输出写进普通       
	 * 日志文件并不理想，但总好过把日志输出               
	 * 直接丢掉。     
	 *
	 * 别想通过即时打开 logFile 来改进这一点。              
	 * 任何此类失败都会导致递归。 
	 */
	if ((destination & LOG_DESTINATION_CSVLOG) && csvlogFile != NULL)
		logfile = csvlogFile;
	else if ((destination & LOG_DESTINATION_JSONLOG) && jsonlogFile != NULL)
		logfile = jsonlogFile;
	else
		logfile = syslogFile;

	rc = fwrite(buffer, 1, count, logfile);

	/*
	 * 设法报告失败。绝不能用 ereport，因为它会直接     
	 * 递归回到这里；但 write_stderr 没问题：它只会写到  
	 * postmaster 原始的 stderr 或 /dev/null，绝不会写              
	 * 入我们的输入管道，那会导致另一种死循环。        
	 */
	if (rc != count)
		write_stderr("could not write to log file: %s\n", strerror(errno));
}

#ifdef WIN32

/*
 * 把数据从管道传输到当前日志文件的工作线程。     
 *
 * 必须有这个线程：Windows 上 WaitForMultipleObjects              
 * 不能用于无名管道：它总报告"已通知"，故阻塞的 ReadFile 
 * 无法响应 SIGHUP；select 仅限套接字。  
 */
static unsigned int __stdcall
pipeThread(void *arg)
{
	char		logbuffer[READ_BUF_SIZE];
	int			bytes_in_logbuffer = 0;

	for (;;)
	{
		DWORD		bytesRead;
		BOOL		result;

		result = ReadFile(syslogPipe[0],
						  logbuffer + bytes_in_logbuffer,
						  sizeof(logbuffer) - bytes_in_logbuffer,
						  &bytesRead, 0);

		/*
		 * 做任何可能触及主线程共享的全局状态之前， 
		 * 先进入临界区。尤其是用                         
		 * palloc()/pfree() 的操作在临界区之外都不安全。     
		 *         
		 */
		EnterCriticalSection(&sysloggerSection);
		if (!result)
		{
			DWORD		error = GetLastError();

			if (error == ERROR_HANDLE_EOF ||
				error == ERROR_BROKEN_PIPE)
				break;
			_dosmaperr(error);
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read from logger pipe: %m")));
		}
		else if (bytesRead > 0)
		{
			bytes_in_logbuffer += bytesRead;
			process_pipe_input(logbuffer, &bytes_in_logbuffer);
		}

		/*
		 * 如果当前日志文件已写满，轻推主线程去            
		 * 做轮转。 
		 */
		if (Log_RotationSize > 0)
		{
			if (ftell(syslogFile) >= Log_RotationSize * 1024L ||
				(csvlogFile != NULL && ftell(csvlogFile) >= Log_RotationSize * 1024L) ||
				(jsonlogFile != NULL && ftell(jsonlogFile) >= Log_RotationSize * 1024L))
				SetLatch(MyLatch);
		}
		LeaveCriticalSection(&sysloggerSection);
	}

	/* 仅在检测到管道 EOF 时才退出上述循环  */
	pipe_eof_seen = true;

	/* 若还有残留数据就立刻刷出去        */
	flush_pipe_input(logbuffer, &bytes_in_logbuffer);

	/* 设置 latch 唤醒主线程，后者将退出          */
	SetLatch(MyLatch);

	LeaveCriticalSection(&sysloggerSection);
	_endthread();
	return 0;
}
#endif							/* WIN32 */

/*
 * 以正确的权限和缓冲选项打开新的日志文件。     
 *
 * 若 allow_errors 为真，则只记录打开失败并返回 NULL     
 * （errno 仍保持 fopen 失败时的值）。   
 * 否则，错误按致命处理。      
 */
static FILE *
logfile_open(const char *filename, const char *mode, bool allow_errors)
{
	FILE	   *fh;
	mode_t		oumask;

	/*
	 * 注意我们不让 Log_file_mode 取消 IWUSR，因为                 
	 * 我们当然要能写这些文件。    
	 */
	oumask = umask((mode_t) ((~(Log_file_mode | S_IWUSR)) & (S_IRWXU | S_IRWXG | S_IRWXO)));
	fh = fopen(filename, mode);
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* Windows 下用 CRLF 行尾       */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		int			save_errno = errno;

		ereport(allow_errors ? LOG : FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open log file \"%s\": %m",
						filename)));
		errno = save_errno;
	}

	return fh;
}

/*
 * 对 target_dest 指定的单一目标做日志文件轮转。             
 * 成功轮转后，*last_file_name 与 *logFile 中保存的信息     
 * 会被更新。          
 *
 * 若轮转已被中止则返回假；否则返回真，继续处理   
 * 其他格式。                 
 */
static bool
logfile_rotate_dest(bool time_based_rotation, int size_rotation_for,
					pg_time_t fntime, int target_dest,
					char **last_file_name, FILE **logFile)
{
	char	   *logFileExt = NULL;
	char	   *filename;
	FILE	   *fh;

	/*
	 * 若目标刚被停用，则关闭旧文件并注销其数据。       
	 * stderr 不会出现这种情况：即使 log_destination 停用了   
	 * stderr，syslogFile 也被视为始终打开。             
	 *                 
	 */
	if ((Log_destination & target_dest) == 0 &&
		target_dest != LOG_DESTINATION_STDERR)
	{
		if (*logFile != NULL)
			fclose(*logFile);
		*logFile = NULL;
		if (*last_file_name != NULL)
			pfree(*last_file_name);
		*last_file_name = NULL;
		return true;
	}

	/*
	 * 若未到轮转时间，或目标基于文件大小不需要轮转，  
	 * 则直接返回。                                     
	 */
	if (!time_based_rotation && (size_rotation_for & target_dest) == 0)
		return true;

	/* 文件扩展名取决于目标类型           */
	if (target_dest == LOG_DESTINATION_STDERR)
		logFileExt = NULL;
	else if (target_dest == LOG_DESTINATION_CSVLOG)
		logFileExt = ".csv";
	else if (target_dest == LOG_DESTINATION_JSONLOG)
		logFileExt = ".json";
	else
	{
		/* 不会发生  */
		Assert(false);
	}

	/* 构造新文件名      */
	filename = logfile_getname(fntime, logFileExt);

	/*
	 * 决定覆盖还是追加。能覆盖的条件：(a) 设置了  
	 * Log_truncate_on_rotation，(b) 轮转由时间到点触发        
	 * 而非其他原因，且 (c) 算出的文件名与之前写入的     
	 * 文件不同。                                     
	 */
	if (Log_truncate_on_rotation && time_based_rotation &&
		*last_file_name != NULL &&
		strcmp(filename, *last_file_name) != 0)
		fh = logfile_open(filename, "w", true);
	else
		fh = logfile_open(filename, "a", true);

	if (!fh)
	{
		/*
		 * 在繁忙的系统上 ENFILE/EMFILE 并不稀奇；继续       
		 * 用旧文件，直到成功拿到新文件。否则，         
		 * 视 Log_directory 有问题，停止尝试创建文件。     
		 *              
		 */
		if (errno != ENFILE && errno != EMFILE)
		{
			ereport(LOG,
					(errmsg("disabling automatic rotation (use SIGHUP to re-enable)")));
			rotation_disabled = true;
		}

		if (filename)
			pfree(filename);
		return false;
	}

	/* 填入新信息             */
	if (*logFile != NULL)
		fclose(*logFile);
	*logFile = fh;

	/* 不 pfree filename，而是把它留给下次使用      */
	if (*last_file_name != NULL)
		pfree(*last_file_name);
	*last_file_name = filename;

	return true;
}

/*
 * 执行日志文件轮转
 */
static void
logfile_rotate(bool time_based_rotation, int size_rotation_for)
{
	pg_time_t	fntime;

	rotation_requested = false;

	/*
	 * 做基于时间的轮转时，以计划轮转时间而非当前时间 
	 * 推导新文件名，以免不立即执行轮转时文件名           
	 * 出现"滑动"。                                   
	 */
	if (time_based_rotation)
		fntime = next_rotation_time;
	else
		fntime = time(NULL);

	/* stderr 的文件轮转   */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_STDERR, &last_sys_file_name,
							 &syslogFile))
		return;

	/* csvlog 的文件轮转   */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_CSVLOG, &last_csv_file_name,
							 &csvlogFile))
		return;

	/* jsonlog 的文件轮转   */
	if (!logfile_rotate_dest(time_based_rotation, size_rotation_for, fntime,
							 LOG_DESTINATION_JSONLOG, &last_json_file_name,
							 &jsonlogFile))
		return;

	update_metainfo_datafile();

	set_next_rotation_time();
}


/*
 * 用时间戳信息构造日志文件名           
 *
 * 若 suffix 非 NULL，就把它附到名字末尾，替换模式 
 * 中已有的 ".log"。     
 *
 * palloc 分配。   
 */
static char *
logfile_getname(pg_time_t timestamp, const char *suffix)
{
	char	   *filename;
	int			len;

	filename = palloc(MAXPGPATH);

	snprintf(filename, MAXPGPATH, "%s/", Log_directory);

	len = strlen(filename);

	/* 把 Log_filename 当作 strftime 模式  */
	pg_strftime(filename + len, MAXPGPATH - len, Log_filename,
				pg_localtime(&timestamp, log_timezone));

	if (suffix != NULL)
	{
		len = strlen(filename);
		if (len > 4 && (strcmp(filename + (len - 4), ".log") == 0))
			len -= 4;
		strlcpy(filename + len, suffix, MAXPGPATH - len);
	}

	return filename;
}

/*
 * 确定下一次计划轮转时间，并存入 next_rotation_time。       
 */
static void
set_next_rotation_time(void)
{
	pg_time_t	now;
	struct pg_tm *tm;
	int			rotinterval;

	/* 若按时间轮转已禁用则无事可做       */
	if (Log_RotationAge <= 0)
		return;

	/*
	 * 这里的要求是选出大于 now、且是日志轮转间隔    
	 * "倍数"的下一个时刻。对"倍数"可以理解得                
	 * 相当宽松。本版本对齐到 log_timezone 而非 GMT。         
	 *     
	 */
	rotinterval = Log_RotationAge * SECS_PER_MINUTE;	/* 换算成秒       */
	now = (pg_time_t) time(NULL);
	tm = pg_localtime(&now, log_timezone);
	now += tm->tm_gmtoff;
	now -= now % rotinterval;
	now += rotinterval;
	now -= tm->tm_gmtoff;
	next_rotation_time = now;
}

/*
 * 存储启用日志收集器时写入日志消息的文件名。            
 * 在存在按时间轮转时，可用于查找当前日志               
 * 文件的文件名。文件名先写入临时文件，               
 * 再原子性地改名到最终位置。文件以与数据         
 * 目录中创建物相同的权限打开，并采用                  
 * 恰当的缓冲选项。                                       
 */
static void
update_metainfo_datafile(void)
{
	FILE	   *fh;
	mode_t		oumask;

	if (!(Log_destination & LOG_DESTINATION_STDERR) &&
		!(Log_destination & LOG_DESTINATION_CSVLOG) &&
		!(Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							LOG_METAINFO_DATAFILE)));
		return;
	}

	/* 新文件使用与数据目录相同的权限                   */
	oumask = umask(pg_mode_mask);
	fh = fopen(LOG_METAINFO_DATAFILE_TMP, "w");
	umask(oumask);

	if (fh)
	{
		setvbuf(fh, NULL, PG_IOLBF, 0);

#ifdef WIN32
		/* Windows 下用 CRLF 行尾       */
		_setmode(_fileno(fh), _O_TEXT);
#endif
	}
	else
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP)));
		return;
	}

	if (last_sys_file_name && (Log_destination & LOG_DESTINATION_STDERR))
	{
		if (fprintf(fh, "stderr %s\n", last_sys_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_csv_file_name && (Log_destination & LOG_DESTINATION_CSVLOG))
	{
		if (fprintf(fh, "csvlog %s\n", last_csv_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}

	if (last_json_file_name && (Log_destination & LOG_DESTINATION_JSONLOG))
	{
		if (fprintf(fh, "jsonlog %s\n", last_json_file_name) < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							LOG_METAINFO_DATAFILE_TMP)));
			fclose(fh);
			return;
		}
	}
	fclose(fh);

	if (rename(LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						LOG_METAINFO_DATAFILE_TMP, LOG_METAINFO_DATAFILE)));
}

/* --------------------------------
 *		信号处理例程     
 * --------------------------------
 */

/*
 * 检查是否有日志轮转请求到来。postmaster 在      
 * 收到 SIGUSR1 之后应调用它。         
 */
bool
CheckLogrotateSignal(void)
{
	struct stat stat_buf;

	if (stat(LOGROTATE_SIGNAL_FILE, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * 移除指示日志轮转请求的信号文件。 
 */
void
RemoveLogrotateSignalFiles(void)
{
	unlink(LOGROTATE_SIGNAL_FILE);
}

/* SIGUSR1：置日志轮转标志     */
static void
sigUsr1Handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	rotation_requested = true;
	SetLatch(MyLatch);

	errno = save_errno;
}
