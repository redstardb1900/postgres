/*
 * fork_process.c
 *	 fork() 之上的简单包装器。此函数不处理       
 *	 EXEC_BACKEND 情形；可以扩展去支持它，但那会       
 *	 复杂得多。           
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/fork_process.c
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/fork_process.h"

#ifndef WIN32
/*
 * fork() 的包装器。返回值与 fork() 的返回值相同：    
 * fork 失败则为 -1，子进程中为 0，父进程中则为    
 * 子进程的 PID。fork 期间信号被阻塞，因此子进程    
 * 必须解除阻塞。  
 */
pid_t
fork_process(void)
{
	pid_t		result;
	const char *oomfilename;
	sigset_t	save_mask;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * fork 前一刻刷新 stdio 通道，避免双重输出问题。        
	 */
	fflush(NULL);

#ifdef LINUX_PROFILE

	/*
	 * Linux 的 fork() 会重置子进程中的剖析计时器。如果要 
	 * 剖析子进程，就需保存并恢复计时器设置。           
	 * 但不剖析时这样做浪费时间，所以只在用特定         
	 * -DLINUX_PROFILE 开关指定时才执行。                
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	/*
	 * 我们启动 postmaster 子进程时让信号阻塞。这使它们     
	 * 能先安装自己的处理器再解除阻塞，以免竞态中        
	 * 它们可能运行 postmaster 处理器而漏掉重要          
	 * 的控制信号。经过更多分析后，这有望放宽。 
	 */
	sigprocmask(SIG_SETMASK, &BlockSig, &save_mask);
	result = fork();
	if (result == 0)
	{
		/* 子进程内 fork 成功 */
		MyProcPid = getpid();
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

		/*
		 * 默认时 Linux 在 OOM 情况下常会杀掉 postmaster，    
		 * 因为它把子进程尺寸总和（*含共享内存*）都      
		 * 算在 postmaster 头上。（这愚蠢得难以置信，      
		 * 但内核黑客们似乎无意改进它。）                     
		 * 因此用如下方法保护 postmaster 常是好主意：    
		 * 把它的 OOM score adjustment 设为负值（这必须在        
		 * root 属主的开机脚本中完成）。由于该调整值会被  
		 * 子进程继承，通常即所有 postmaster 的        
		 * 子进程都同等地防 OOM kill，这未必是好主意。       
		 * 所以我们提供这段代码，让子进程能重新修改  
		 * 它们自己的 OOM score adjustment。写入的目标文件   
		 * 名与写入值均由环境变量控制，可由当初做     
		 * 原始调整的那个开机脚本设置。                        
		 *                                  
		 */
		oomfilename = getenv("PG_OOM_ADJUST_FILE");

		if (oomfilename != NULL)
		{
			/*
			 * 用 open() 而非 stdio，以便掌控打开标志。某些    
			 * Linux 安全环境拒绝 O_WRONLY 以外的一切。     
			 */
			int			fd = open(oomfilename, O_WRONLY, 0);

			/* 忽略所有错误   */
			if (fd >= 0)
			{
				const char *oomvalue = getenv("PG_OOM_ADJUST_VALUE");
				int			rc;

				if (oomvalue == NULL)	/* 提供有用的默认值*/
					oomvalue = "0";

				rc = write(fd, oomvalue, strlen(oomvalue));
				(void) rc;
				close(fd);
			}
		}

		/* 为随机数生成做 fork 后初始化                  */
		pg_strong_random_init();
	}
	else
	{
		/* 父进程中恢复信号掩码 */
		sigprocmask(SIG_SETMASK, &save_mask, NULL);
	}

	return result;
}

#endif							/* ! WIN32 */
