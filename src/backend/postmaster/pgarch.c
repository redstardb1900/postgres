/*-------------------------------------------------------------------------
 *
 * pgarch.c
 *
 *	PostgreSQL WAL归档器
 *
 *	archiver 相关的全部函数收录在本文件中 
 *
 *	- archiver 进程执行的所有函数      
 *
 *	- archiver 由 postmaster fork，二者随后    
 *	通过信号通信。postmaster 执行的所有函数  
 *	都收录在本文件之中。                   
 *
 *	最初作者：Simon Riggs		simon@2ndquadrant.com 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pgarch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "lib/binaryheap.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/* ----------
 * 定时器定义。
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL 60 /* How often to force a poll of the
									 * archive status directory; in seconds. */
#define PGARCH_RESTART_INTERVAL 10	/* How often to attempt to restart a
									 * failed archiver; in seconds. */

/*
 * 归档 WAL 文件时允许的最大重试次数。                
 *      
 */
#define NUM_ARCHIVE_RETRIES 3

/*
 * 尝试删除孤儿 archive status 文件时允许的          
 * 最大重试次数。      
 */
#define NUM_ORPHAN_CLEANUP_RETRIES 3

/*
 * 每轮目录扫描最多收集的 .ready 文件数。       
 */
#define NUM_FILES_PER_DIRECTORY_SCAN 64

/* archiver 进程的共享内存区       */
typedef struct PgArchData
{
	int			pgprocno;		/* archiver 进程的 pgprocno  */

	/*
	 * 强制 pgarch_readyXlog() 进行目录扫描。由 arch_lck 保护。 
	 */
	bool		force_dir_scan;

	slock_t		arch_lck;
} PgArchData;

char	   *XLogArchiveLibrary = "";


/* ----------
 * Local data
 * ----------
 */
static time_t last_sigterm_time = 0;
static PgArchData *PgArch = NULL;
static const ArchiveModuleCallbacks *ArchiveCallbacks;
static ArchiveModuleState *archive_module_state;


/*
 * 用于跟踪 archive_status 每轮扫描中发现的          
 * 多个待归档文件。尽量减少目录扫描的次数，在待归档
 * 文件很多时可显著提高归档速度。                 
 *
 * arch_heap 是一个最大堆，目录扫描期间用它跟踪          
 * 优先级最高的待归档文件。扫描完成后，文件按 
 * 优先级升序存入 arch_files。pgarch_readyXlog()                 
 * 会不断从 arch_files 返回文件，直至取空，此时就       
 * 必须再执行一次目录扫描。                              
 *
 * 这些数据只在 archiver 进程中用到，因此把它做成     
 * palloc 的 struct，而非静态数组。   
 */
struct arch_files_state
{
	binaryheap *arch_heap;
	int			arch_files_size;	/* arch_files[] 中的有效条目数     */
	char	   *arch_files[NUM_FILES_PER_DIRECTORY_SCAN];
	/* 堆底层缓冲区，后来即 arch_files[] 的条目：  */
	char		arch_filenames[NUM_FILES_PER_DIRECTORY_SCAN][MAX_XFN_CHARS + 1];
};

static struct arch_files_state *arch_files = NULL;

/*
 * 由中断处理程序设置、供主循环稍后处理的标志。 
 */
static volatile sig_atomic_t ready_to_stop = false;

/* ----------
 * 局部函数前置声明           
 * ----------
 */
static void pgarch_waken_stop(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static bool pgarch_archiveXlog(char *xlog);
static bool pgarch_readyXlog(char *xlog);
static void pgarch_archiveDone(char *xlog);
static void pgarch_die(int code, Datum arg);
static void HandlePgArchInterrupts(void);
static int	ready_file_comparator(Datum a, Datum b, void *arg);
static void LoadArchiveLibrary(void);
static void pgarch_call_module_shutdown_cb(int code, Datum arg);

/* 报告 PgArchShmemInit 所需的共享内存空间   */
Size
PgArchShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(PgArchData));

	return size;
}

/* 分配并初始化 archiver 相关的共享内存      */
void
PgArchShmemInit(void)
{
	bool		found;

	PgArch = (PgArchData *)
		ShmemInitStruct("Archiver Data", PgArchShmemSize(), &found);

	if (!found)
	{
		/* 首次运行，执行初始化。 */
		MemSet(PgArch, 0, PgArchShmemSize());
		PgArch->pgprocno = INVALID_PGPROCNO;
		SpinLockInit(&PgArch->arch_lck);
	}
}

/*
 * PgArchCanRestart
 *
 * 若距上次启动已达 PGARCH_RESTART_INTERVAL，              
 * 则返回 true，表示 archiver 可重启；否则返回          
 * false。               
 *
 * 这是一个安全阀，用于防止 archiver 一启动就立刻            
 * 死亡时不断尝试重新拉起。注意，由于我们会从          
 * postmaster 主循环里重试拉起 archiver，后面还有           
 * 机会。    
 */
bool
PgArchCanRestart(void)
{
	static time_t last_pgarch_start_time = 0;
	time_t		curtime = time(NULL);

	/*
	 * 若距 archiver 上次启动过近，则返回 false 且不重启。   
	 *       
	 */
	if ((unsigned int) (curtime - last_pgarch_start_time) <
		(unsigned int) PGARCH_RESTART_INTERVAL)
		return false;

	last_pgarch_start_time = curtime;
	return true;
}


/* archiver 进程的主入口           */
void
PgArchiverMain(void)
{
	/*
	 * 忽略 postmaster 里通常绑定了动作的所有信号，      
	 * SIGHUP、SIGTERM、SIGUSR1、SIGUSR2、SIGQUIT 除外。  
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT 处理程序已由 InitPostmasterChild 设置好  */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, pgarch_waken_stop);

	/* 重置一些 postmaster 接受但此处不接受的信号       */
	pqsignal(SIGCHLD, SIG_DFL);

	/* 解除信号阻塞（postmaster fork 我们时它们被阻塞）  */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* 非必要不应启动本进程。       */
	Assert(XLogArchivingActive());

	/* 安排在 archiver 退出时清理   */
	on_shmem_exit(pgarch_die, 0);

	/*
	 * 公开本进程的 pgprocno，让后端在我们休眠时用           
	 * latch 唤醒我们。
	 */
	PgArch->pgprocno = MyProc->pgprocno;

	/* 为 pgarch_readyXlog() 创建工作区  */
	arch_files = palloc(sizeof(struct arch_files_state));
	arch_files->arch_files_size = 0;

	/* 初始化最大堆，为待归档文件排优先级。     */
	arch_files->arch_heap = binaryheap_allocate(NUM_FILES_PER_DIRECTORY_SCAN,
												ready_file_comparator, NULL);

	/* 加载 archive_library。 */
	LoadArchiveLibrary();

	pgarch_MainLoop();

	proc_exit(0);
}

/*
 * 唤醒 archiver。  
 */
void
PgArchWakeup(void)
{
	int			arch_pgprocno = PgArch->pgprocno;

	/*
	 * 这里不加 ProcArrayLock。这样做其实没问题，因为  
	 * procLatch 从不释放，我们最多可能设错进程             
	 * （或不设任何进程）的 latch。即便如此，archiver 也   
	 * 很快就会被重新拉起并开始归档。  
	 */
	if (arch_pgprocno != INVALID_PGPROCNO)
		SetLatch(&ProcGlobal->allProcs[arch_pgprocno].procLatch);
}


/* archiver 进程的 SIGUSR2 处理程序     */
static void
pgarch_waken_stop(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/* 设置标志做最后一轮归档，然后关闭。   */
	ready_to_stop = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * pgarch_MainLoop
 *
 * archiver 的主循环 
 */
static void
pgarch_MainLoop(void)
{
	bool		time_to_stop;

	/*
	 * 除了等待信号之外，archiver 本没有什么要做的           
	 * 事情……但 archiver 的职责就是保护数据，因此它  
	 * 会时不时醒来，让自己保持主动。         
	 */
	do
	{
		ResetLatch(MyLatch);

		/* 收到 SIGUSR2 后，再做一轮归档，然后退出       */
		time_to_stop = ready_to_stop;

		/* 检查屏障事件与配置更新          */
		HandlePgArchInterrupts();

		/*
		 * 收到 SIGTERM 后，在 SIGUSR2 到来之前，我们通常      
		 * 只会干坐着。但一次随意的 SIGTERM 就可能      
		 * 让归档无限期停摆，这不太好。若 SIGTERM 过后  
		 * 超过 60 秒，无论如何都会退出，以便 postmaster 在  
		 * 需要时能启动新的 archiver。                   
		 */
		if (ShutdownRequestPending)
		{
			time_t		curtime = time(NULL);

			if (last_sigterm_time == 0)
				last_sigterm_time = curtime;
			else if ((unsigned int) (curtime - last_sigterm_time) >=
					 (unsigned int) 60)
				break;
		}

		/* 干我们该干的事  */
		pgarch_ArchiverCopyLoop();

		/*
		 * 休眠，直到收到信号、PGARCH_AUTOWAKE_INTERVAL        
		 * 强制轮询，或 postmaster 死亡。            
		 */
		if (!time_to_stop)		/* 最后一轮不再等待         */
		{
			int			rc;

			rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   PGARCH_AUTOWAKE_INTERVAL * 1000L,
						   WAIT_EVENT_ARCHIVER_MAIN);
			if (rc & WL_POSTMASTER_DEATH)
				time_to_stop = true;
		}

		/*
		 * archiver 退出有两种情形：postmaster 死亡（不应      
		 * 发生），或收到 SIGUSR2 后再归档一轮并退出。
		 *         
		 */
	} while (!time_to_stop);
}

/*
 * pgarch_ArchiverCopyLoop
 *
 * 归档所有未处理的 xlog 后返回    
 */
static void
pgarch_ArchiverCopyLoop(void)
{
	char		xlog[MAX_XFN_CHARS + 1];

	/* 让首次 pgarch_readyXlog() 调用强制扫描目录        */
	arch_files->arch_files_size = 0;

	/*
	 * 遍历所有 archive_status 为 .ready 的 xlog 并归档，    
	 * 多数时候预期只有一个文件，但仍可能有后端趁        
	 * 我们复制之前归档时，把文件加进待归档列表          
	 *                                            
	 */
	while (pgarch_readyXlog(xlog))
	{
		int			failures = 0;
		int			failures_orphan = 0;

		for (;;)
		{
			struct stat stat_buf;
			char		pathname[MAXPGPATH];

			/*
			 * 收到 SIGTERM 之后不再发起归档命令，         
			 * postmaster 意外死亡后也不发起。前一条件是    
			 * 尽量避免 init 用 SIGKILL 杀掉该命令，后一条件  
			 * 则是避免与更新的 postmaster 拉起的另一       
			 * archiver 发生冲突。               
			 */
			if (ShutdownRequestPending || !PostmasterIsAlive())
				return;

			/*
			 * 检查屏障事件与配置更新。这样 archive_command  
			 * 的新设置能尽快生效，即使还有大量文件  
			 * 等待归档也是如此。                                  
			 */
			HandlePgArchInterrupts();

			/* 若未配置则什么也做不了…… */
			if (ArchiveCallbacks->check_configured_cb != NULL &&
				!ArchiveCallbacks->check_configured_cb(archive_module_state))
			{
				ereport(WARNING,
						(errmsg("archive_mode enabled, yet archiving is not configured")));
				return;
			}

			/*
			 * 由于 archive status 文件的删除不保证持久，        
			 * 系统崩溃可能为已回收或已删除的 WAL 段留下    
			 * .ready 文件。这种情况直接删除孤儿状态文件
			 * 并继续即可。这里用 unlink() 是因为，即便后续 
			 * 再崩溃时同样的孤儿文件也会被删掉，因此    
			 * 无需担心持久性。                             
			 *            
			 */
			snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
			if (stat(pathname, &stat_buf) != 0 && errno == ENOENT)
			{
				char		xlogready[MAXPGPATH];

				StatusFilePath(xlogready, xlog, ".ready");
				if (unlink(xlogready) == 0)
				{
					ereport(WARNING,
							(errmsg("removed orphan archive status file \"%s\"",
									xlogready)));

					/* 跳出循环，处理下一个状态文件  */
					break;
				}

				if (++failures_orphan >= NUM_ORPHAN_CLEANUP_RETRIES)
				{
					ereport(WARNING,
							(errmsg("removal of orphan archive status file \"%s\" failed too many times, will try again later",
									xlogready)));

					/* 放弃清理孤儿状态文件         */
					return;
				}

				/* 稍候片刻再重试      */
				pg_usleep(1000000L);
				continue;
			}

			if (pgarch_archiveXlog(xlog))
			{
				/* 成功     */
				pgarch_archiveDone(xlog);

				/*
				 * 将成功归档的 WAL 文件上报给累计统计系统   
				 *                      
				 */
				pgstat_report_archiver(xlog, false);

				break;			/* 跳出内层循环      */
			}
			else
			{
				/*
				 * 把归档失败的 WAL 文件上报给累计统计系统   
				 *                  
				 */
				pgstat_report_archiver(xlog, true);

				if (++failures >= NUM_ARCHIVE_RETRIES)
				{
					ereport(WARNING,
							(errmsg("archiving write-ahead log file \"%s\" failed too many times, will try again later",
									xlog)));
					return;		/* 暂时放弃归档        */
				}
				pg_usleep(1000000L);	/* 稍候片刻再重试      */
			}
		}
	}
}

/*
 * pgarch_archiveXlog
 *
 * 调用 archive_file_cb 把一个归档文件复制到应去的地方     
 *
 * 成功则返回 true。   
 */
static bool
pgarch_archiveXlog(char *xlog)
{
	char		pathname[MAXPGPATH];
	char		activitymsg[MAXFNAMELEN + 16];
	bool		ret;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);

	/* 在 PS 显示中报告归档活动    */
	snprintf(activitymsg, sizeof(activitymsg), "archiving %s", xlog);
	set_ps_display(activitymsg);

	ret = ArchiveCallbacks->archive_file_cb(archive_module_state, xlog, pathname);
	if (ret)
		snprintf(activitymsg, sizeof(activitymsg), "last was %s", xlog);
	else
		snprintf(activitymsg, sizeof(activitymsg), "failed on %s", xlog);
	set_ps_display(activitymsg);

	return ret;
}

/*
 * pgarch_readyXlog
 *
 * 返回尚未归档的最旧 xlog 文件的名字。                
 * 并不设置文件归档已开始的通知，因此              
 * 若要创建多个并发归档任务，这里需要扩展。  
 * 一旦失败，下次有机会时我们会                 
 * 把该文件完整重拷一遍。                  
 *
 * 必须返回最旧的文件，这样才能按写入顺序          
 * 归档 xlog，原因有二：                    
 * 1) 维护恢复所需的 xlog 顺序链；                        
 * 2) 最旧的文件会最先成为检查点时               
 * 回收的候选。             
 *
 * 注意："最旧"比较会把任何 .history 文件视为比             
 * 除其他 .history 文件外任何文件都更旧。较小 ID            
 * 时间线上的段都比较大 ID 时间线上的所有段            
 * 更旧；最终效果是过去时间线在归档时获得更高     
 * 的优先级。这似乎没问题，或至少并非显然值得          
 * 改。   
 */
static bool
pgarch_readyXlog(char *xlog)
{
	char		XLogArchiveStatusDir[MAXPGPATH];
	DIR		   *rldir;
	struct dirent *rlde;
	bool		force_dir_scan;

	/*
	 * 若请求过目录扫描，就清空已存文件名并继续。   
	 *         
	 */
	SpinLockAcquire(&PgArch->arch_lck);
	force_dir_scan = PgArch->force_dir_scan;
	PgArch->force_dir_scan = false;
	SpinLockRelease(&PgArch->arch_lck);

	if (force_dir_scan)
		arch_files->arch_files_size = 0;

	/*
	 * 若上一轮目录扫描留下的文件名还在，                 
	 * 就尝试返回其中之一。我们会检查状态文件            
	 * 是否仍存在，前一文件的 archive_command 可能已       
	 * 把它标为 done。   
	 */
	while (arch_files->arch_files_size > 0)
	{
		struct stat st;
		char		status_file[MAXPGPATH];
		char	   *arch_file;

		arch_files->arch_files_size--;
		arch_file = arch_files->arch_files[arch_files->arch_files_size];
		StatusFilePath(status_file, arch_file, ".ready");

		if (stat(status_file, &st) == 0)
		{
			strcpy(xlog, arch_file);
			return true;
		}
		else if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", status_file)));
	}

	/* arch_heap 大概为空，但仍确认一下      */
	binaryheap_reset(arch_files->arch_heap);

	/*
	 * 打开 archive status 目录并遍历其中的文件列表，       
	 * 找出带 .ready 后缀的最旧文件。              
	 */
	snprintf(XLogArchiveStatusDir, MAXPGPATH, XLOGDIR "/archive_status");
	rldir = AllocateDir(XLogArchiveStatusDir);

	while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL)
	{
		int			basenamelen = (int) strlen(rlde->d_name) - 6;
		char		basename[MAX_XFN_CHARS + 1];
		char	   *arch_file;

		/* 忽略字符数不合预期的条目                */
		if (basenamelen < MIN_XFN_CHARS ||
			basenamelen > MAX_XFN_CHARS)
			continue;

		/* 忽略含意外字符的条目            */
		if (strspn(rlde->d_name, VALID_XFN_CHARS) < basenamelen)
			continue;

		/* 忽略不带 .ready 后缀的条目      */
		if (strcmp(rlde->d_name + basenamelen, ".ready") != 0)
			continue;

		/* 截掉 .ready 后缀    */
		memcpy(basename, rlde->d_name, basenamelen);
		basename[basenamelen] = '\0';

		/*
		 * 若优先级够高，就把该文件存入我们的最大堆。 
		 */
		if (arch_files->arch_heap->bh_size < NUM_FILES_PER_DIRECTORY_SCAN)
		{
			/* 若堆未满，直接快速加入。        */
			arch_file = arch_files->arch_filenames[arch_files->arch_heap->bh_size];
			strcpy(arch_file, basename);
			binaryheap_add_unordered(arch_files->arch_heap, CStringGetDatum(arch_file));

			/* 若刚填满堆，就把它整理成有效堆。 */
			if (arch_files->arch_heap->bh_size == NUM_FILES_PER_DIRECTORY_SCAN)
				binaryheap_build(arch_files->arch_heap);
		}
		else if (ready_file_comparator(binaryheap_first(arch_files->arch_heap),
									   CStringGetDatum(basename), NULL) > 0)
		{
			/*
			 * 移除优先级最低者，把当前文件加入堆中。     
			 *      
			 */
			arch_file = DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));
			strcpy(arch_file, basename);
			binaryheap_add(arch_files->arch_heap, CStringGetDatum(arch_file));
		}
	}
	FreeDir(rldir);

	/* 若没找到文件，直接返回。   */
	if (arch_files->arch_heap->bh_size == 0)
		return false;

	/*
	 * 若没填满堆，就还没把它变成有效堆，现在补上。 
	 *     
	 */
	if (arch_files->arch_heap->bh_size < NUM_FILES_PER_DIRECTORY_SCAN)
		binaryheap_build(arch_files->arch_heap);

	/*
	 * 把待归档文件按优先级升序填入 arch_files 数组。      
	 *          
	 */
	arch_files->arch_files_size = arch_files->arch_heap->bh_size;
	for (int i = 0; i < arch_files->arch_files_size; i++)
		arch_files->arch_files[i] = DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));

	/* 返回优先级最高的文件。 */
	arch_files->arch_files_size--;
	strcpy(xlog, arch_files->arch_files[arch_files->arch_files_size]);

	return true;
}

/*
 * ready_file_comparator
 *
 * 比较待归档文件的归档优先级。若 "a" 的优先级       
 * 高于 "b"，则返回负值。若 "b" 的优先级高于             
 * "a"，则返回正值。若 "a" 与 "b" 优先级相同，            
 * 则返回 0。                                            
 */
static int
ready_file_comparator(Datum a, Datum b, void *arg)
{
	char	   *a_str = DatumGetCString(a);
	char	   *b_str = DatumGetCString(b);
	bool		a_history = IsTLHistoryFileName(a_str);
	bool		b_history = IsTLHistoryFileName(b_str);

	/* 时间线历史文件的优先级总是最高。         */
	if (a_history != b_history)
		return a_history ? -1 : 1;

	/* 越旧的文件优先级越高。 */
	return strcmp(a_str, b_str);
}

/*
 * PgArchForceDirScan
 *
 * 调用本函数后，下一次 pgarch_readyXlog() 调用将    
 * 执行目录扫描。这有助于确保时间线历史文件等       
 * 重要文件尽快归档。                                   
 */
void
PgArchForceDirScan(void)
{
	SpinLockAcquire(&PgArch->arch_lck);
	PgArch->force_dir_scan = true;
	SpinLockRelease(&PgArch->arch_lck);
}

/*
 * pgarch_archiveDone
 *
 * 发出 xlog 文件已成功归档的通知。                      
 * 做法是把状态文件从 NNN.ready 重命名为 NNN.done。    
 * 最终 checkpointer 进程会注意到这一点，并删除          
 * NNN.done 与 xlog 文件本身。      
 */
static void
pgarch_archiveDone(char *xlog)
{
	char		rlogready[MAXPGPATH];
	char		rlogdone[MAXPGPATH];

	StatusFilePath(rlogready, xlog, ".ready");
	StatusFilePath(rlogdone, xlog, ".done");

	/*
	 * 为避免额外开销，我们不把 .ready 文件持久地        
	 * 重命名为 .done。归档命令和库必须能优雅地处理       
	 * 重新归档文件的尝试（例如服务器恰在本函数     
	 * 调用前崩溃），所以 .ready 文件崩溃后重新出现        
	 * 也无妨。  
	 */
	if (rename(rlogready, rlogdone) < 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						rlogready, rlogdone)));
}


/*
 * pgarch_die
 *
 * 退出时的清理例程 
 */
static void
pgarch_die(int code, Datum arg)
{
	PgArch->pgprocno = INVALID_PGPROCNO;
}

/*
 * WAL archiver 进程中断处理程序。   
 *
 * 本函数在 pgarch_MainLoop 与 pgarch_ArchiverCopyLoop                
 * 循环中调用。它检查屏障事件、配置更新以及          
 * 打印内存上下文的请求；关闭请求除外，因为   
 * 两个循环处理它的方式不同。           
 */
static void
HandlePgArchInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* 打印本进程的内存上下文                  */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	if (ConfigReloadPending)
	{
		char	   *archiveLib = pstrdup(XLogArchiveLibrary);
		bool		archiveLibChanged;

		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("both archive_command and archive_library set"),
					 errdetail("Only one of archive_command, archive_library may be set.")));

		archiveLibChanged = strcmp(XLogArchiveLibrary, archiveLib) != 0;
		pfree(archiveLib);

		if (archiveLibChanged)
		{
			/*
			 * 理想情况下，只需卸载旧的归档模块再加载新   
			 * 模块即可，但目前没有卸载库的机制         
			 * （见上文 internal_load_library()      
			 * 处的注释）。为解决此问题，我们直接重启      
			 * archiver。新的归档模块会在新 archiver 进程       
			 * 启动时加载。注意这会触发模块的关闭回调   
			 * （若已定义）。                  
			 */
			ereport(LOG,
					(errmsg("restarting archiver process because value of "
							"\"archive_library\" was changed")));

			proc_exit(0);
		}
	}
}

/*
 * LoadArchiveLibrary
 *
 * 把归档回调加载进本地的 ArchiveCallbacks。         
 */
static void
LoadArchiveLibrary(void)
{
	ArchiveModuleInit archive_init;

	if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("both archive_command and archive_library set"),
				 errdetail("Only one of archive_command, archive_library may be set.")));

	/*
	 * 若启用了 shell 归档，就使用我们专用的初始化函数。 
	 * 否则加载库并调用其 _PG_archive_module_init()。           
	 */
	if (XLogArchiveLibrary[0] == '\0')
		archive_init = shell_archive_init;
	else
		archive_init = (ArchiveModuleInit)
			load_external_function(XLogArchiveLibrary,
								   "_PG_archive_module_init", false, NULL);

	if (archive_init == NULL)
		ereport(ERROR,
				(errmsg("archive modules have to define the symbol %s", "_PG_archive_module_init")));

	ArchiveCallbacks = (*archive_init) ();

	if (ArchiveCallbacks->archive_file_cb == NULL)
		ereport(ERROR,
				(errmsg("archive modules must register an archive callback")));

	archive_module_state = (ArchiveModuleState *) palloc0(sizeof(ArchiveModuleState));
	if (ArchiveCallbacks->startup_cb != NULL)
		ArchiveCallbacks->startup_cb(archive_module_state);

	before_shmem_exit(pgarch_call_module_shutdown_cb, 0);
}

/*
 * 调用已加载归档模块的关闭回调（若已定义）。     
 */
static void
pgarch_call_module_shutdown_cb(int code, Datum arg)
{
	if (ArchiveCallbacks->shutdown_cb != NULL)
		ArchiveCallbacks->shutdown_cb(archive_module_state);
}
