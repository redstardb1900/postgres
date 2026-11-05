/*
 *	parallel.c
 *
 *	multi-process support
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/parallel.c
 */

#include "postgres_fe.h"

#include <sys/wait.h>
#ifdef WIN32
#include <io.h>
#endif

#include "pg_upgrade.h"

static int	parallel_jobs;

#ifdef WIN32
/*
 *	保存所有活动线程的数组，不能有任何空隙/零，   
 *	这样才能传给 WaitForMultipleObjects()。我们用两个   
 *	数组，让 thread_handles 数组传给 WaitForMultipleObjects()。  
 */
HANDLE	   *thread_handles;

typedef struct
{
	char	   *log_file;
	char	   *opt_log_file;
	char	   *cmd;
} exec_thread_arg;

typedef struct
{
	DbInfoArr  *old_db_arr;
	DbInfoArr  *new_db_arr;
	char	   *old_pgdata;
	char	   *new_pgdata;
	char	   *old_tablespace;
} transfer_thread_arg;

exec_thread_arg **exec_thread_args;
transfer_thread_arg **transfer_thread_args;

/* 跟踪当前 thread_args 使 reap_child 适所有况                       */
void	  **cur_thread_args;

DWORD		win32_exec_prog(exec_thread_arg *args);
DWORD		win32_transfer_all_new_dbs(transfer_thread_arg *args);
#endif

/*
 *	parallel_exec_prog
 *
 *	此函数与 exec_prog 具有相同的 API，但它并行执行，    
 *	因此必须抛出错误，并且不返回错误状态。          
 */
void
parallel_exec_prog(const char *log_file, const char *opt_log_file,
				   const char *fmt,...)
{
	va_list		args;
	char		cmd[MAX_STRING];

#ifndef WIN32
	pid_t		child;
#else
	HANDLE		child;
	exec_thread_arg *new_arg;
#endif

	va_start(args, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, args);
	va_end(args);

	if (user_opts.jobs <= 1)
		/* exit_on_error 须 true 允作业         */
		exec_prog(log_file, opt_log_file, true, true, "%s", cmd);
	else
	{
		/* 并行   */
#ifdef WIN32
		if (thread_handles == NULL)
			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));

		if (exec_thread_args == NULL)
		{
			int			i;

			exec_thread_args = pg_malloc(user_opts.jobs * sizeof(exec_thread_arg *));

			/*
			 * 出于安全与性能考虑，我们在进程整个生命期 
			 * 内保持这些参数的分配，并且不会在与分配线程
			 * 不同的线程中释放它们。               
			 */
			for (i = 0; i < user_opts.jobs; i++)
				exec_thread_args[i] = pg_malloc0(sizeof(exec_thread_arg));
		}

		cur_thread_args = (void **) exec_thread_args;
#endif
		/* 回收亡子进程        */
		while (reap_child(false) == true)
			;

		/* 需等亡子进程？          */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* 在开始作业前设置此项   */
		parallel_jobs++;

		/* 在 fork 前确保 stdio 状态已静默      */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
			/* 用 _exit 跳过 atexit() 函数     */
			_exit(!exec_prog(log_file, opt_log_file, true, true, "%s", cmd));
		else if (child < 0)
			/* fork 失败 */
			pg_fatal("could not create worker process: %s", strerror(errno));
#else
		/* 空数组元素总是位于末尾         */
		new_arg = exec_thread_args[parallel_jobs - 1];

		/* 只能向函数传入一个指针，因此使用结构体    */
		pg_free(new_arg->log_file);
		new_arg->log_file = pg_strdup(log_file);
		pg_free(new_arg->opt_log_file);
		new_arg->opt_log_file = opt_log_file ? pg_strdup(opt_log_file) : NULL;
		pg_free(new_arg->cmd);
		new_arg->cmd = pg_strdup(cmd);

		child = (HANDLE) _beginthreadex(NULL, 0, (void *) win32_exec_prog,
										new_arg, 0, NULL);
		if (child == 0)
			pg_fatal("could not create worker thread: %s", strerror(errno));

		thread_handles[parallel_jobs - 1] = child;
#endif
	}
}


#ifdef WIN32
DWORD
win32_exec_prog(exec_thread_arg *args)
{
	int			ret;

	ret = !exec_prog(args->log_file, args->opt_log_file, true, true, "%s", args->cmd);

	/* 终止线程      */
	return ret;
}
#endif


/*
 *	parallel_transfer_all_new_dbs
 *
 *	接口与 transfer_all_new_dbs 相同，但它会并行执行，                
 *	同时并行传输多个表空间               
 */
void
parallel_transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
							  char *old_pgdata, char *new_pgdata,
							  char *old_tablespace)
{
#ifndef WIN32
	pid_t		child;
#else
	HANDLE		child;
	transfer_thread_arg *new_arg;
#endif

	if (user_opts.jobs <= 1)
		transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata, NULL);
	else
	{
		/* 并行   */
#ifdef WIN32
		if (thread_handles == NULL)
			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));

		if (transfer_thread_args == NULL)
		{
			int			i;

			transfer_thread_args = pg_malloc(user_opts.jobs * sizeof(transfer_thread_arg *));

			/*
			 * 出于安全与性能考虑，我们在进程的整个       
			 * 生存期内保持参数处于已分配状态，并且         
			 * 不在分配它的线程之外释放参数      
			 */
			for (i = 0; i < user_opts.jobs; i++)
				transfer_thread_args[i] = pg_malloc0(sizeof(transfer_thread_arg));
		}

		cur_thread_args = (void **) transfer_thread_args;
#endif
		/* 回收死亡子进程     */
		while (reap_child(false) == true)
			;

		/* 要等待死亡子进程吗？ */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* 在开始作业前设置此项   */
		parallel_jobs++;

		/* 在 fork 前确保 stdio 状态已静默      */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
		{
			transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata,
								 old_tablespace);
			/* 若走其他退出路径，其值将为非零     */
			/* 用 _exit 跳过 atexit() 函数     */
			_exit(0);
		}
		else if (child < 0)
			/* fork 失败 */
			pg_fatal("could not create worker process: %s", strerror(errno));
#else
		/* 空数组元素总是位于末尾         */
		new_arg = transfer_thread_args[parallel_jobs - 1];

		/* 只能向函数传入一个指针，因此使用结构体    */
		new_arg->old_db_arr = old_db_arr;
		new_arg->new_db_arr = new_db_arr;
		pg_free(new_arg->old_pgdata);
		new_arg->old_pgdata = pg_strdup(old_pgdata);
		pg_free(new_arg->new_pgdata);
		new_arg->new_pgdata = pg_strdup(new_pgdata);
		pg_free(new_arg->old_tablespace);
		new_arg->old_tablespace = old_tablespace ? pg_strdup(old_tablespace) : NULL;

		child = (HANDLE) _beginthreadex(NULL, 0, (void *) win32_transfer_all_new_dbs,
										new_arg, 0, NULL);
		if (child == 0)
			pg_fatal("could not create worker thread: %s", strerror(errno));

		thread_handles[parallel_jobs - 1] = child;
#endif
	}
}


#ifdef WIN32
DWORD
win32_transfer_all_new_dbs(transfer_thread_arg *args)
{
	transfer_all_new_dbs(args->old_db_arr, args->new_db_arr, args->old_pgdata,
						 args->new_pgdata, args->old_tablespace);

	/* 终止线程      */
	return 0;
}
#endif


/*
 *	从已完成的工作子进程收集状态  
 */
bool
reap_child(bool wait_for_child)
{
#ifndef WIN32
	int			work_status;
	pid_t		child;
#else
	int			thread_num;
	DWORD		res;
#endif

	if (user_opts.jobs <= 1 || parallel_jobs == 0)
		return false;

#ifndef WIN32
	child = waitpid(-1, &work_status, wait_for_child ? 0 : WNOHANG);
	if (child == (pid_t) -1)
		pg_fatal("%s() failed: %s", "waitpid", strerror(errno));
	if (child == 0)
		return false;			/* 无子进程或无死亡子进程*/
	if (work_status != 0)
		pg_fatal("child process exited abnormally: status %d", work_status);
#else
	/* 等其中一个结束  */
	thread_num = WaitForMultipleObjects(parallel_jobs, thread_handles,
										false, wait_for_child ? INFINITE : 0);

	if (thread_num == WAIT_TIMEOUT || thread_num == WAIT_FAILED)
		return false;

	/* 计算 active_threads 的线程索引  */
	thread_num -= WAIT_OBJECT_0;

	/* 获取结果   */
	GetExitCodeThread(thread_handles[thread_num], &res);
	if (res != 0)
		pg_fatal("child worker exited abnormally: %s", strerror(errno));

	/* 释放句柄以避免泄漏     */
	CloseHandle(thread_handles[thread_num]);

	/* 把末尾槽位移入死亡子进程处   */
	if (thread_num != parallel_jobs - 1)
	{
		void	   *tmp_args;

		thread_handles[thread_num] = thread_handles[parallel_jobs - 1];

		/*
		 * 把最后一个活动线程的参数结构移入刚失效的      
		 * 槽位，并把该槽位移到末尾供下一个线程复用。  
		 * 虽然该结构正被另一个线程使用，但仍可安全地  
		 * 交换数组内的结构指针。
		 */
		tmp_args = cur_thread_args[thread_num];
		cur_thread_args[thread_num] = cur_thread_args[parallel_jobs - 1];
		cur_thread_args[parallel_jobs - 1] = tmp_args;
	}
#endif

	/* 作业移除后再执行此操作  */
	parallel_jobs--;

	return true;
}
