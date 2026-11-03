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
/*存所有活跃线程句柄。无间隙/零以便传 WaitForMultipleObjects。双数组使 thread_handles 可传。
 *	
 *	
 *	
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

/*与 exec_prog API同，但并行执行故必须报错且不返错误状态。
 *	
 *
 *	
 *	
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

			/*安全和性能：整个进程生命周期保持参数分配，不在不同线程释放。
			 * 
			 * 
			 * 
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

		/* fork 前寂 stdio                */
		parallel_jobs++;

		/* 用 _exit 跳 atexit()                        */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
			/* fork 败                             */
			_exit(!exec_prog(log_file, opt_log_file, true, true, "%s", cmd));
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s", strerror(errno));
#else
		/* 仅传一指故用 struct                 */
		new_arg = exec_thread_args[parallel_jobs - 1];

		/* 终止线程                                                 */
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

	/* terminates thread */
	return ret;
}
#endif


/*并行
 *	
 *
 *	
 *	
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
		/* parallel */
#ifdef WIN32
		if (thread_handles == NULL)
			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));

		if (transfer_thread_args == NULL)
		{
			int			i;

			transfer_thread_args = pg_malloc(user_opts.jobs * sizeof(transfer_thread_arg *));

			/*回收死亡子进程
			 * 
			 * 
			 * 
			                                                                                                                                                        */
			for (i = 0; i < user_opts.jobs; i++)
				transfer_thread_args[i] = pg_malloc0(sizeof(transfer_thread_arg));
		}

		cur_thread_args = (void **) transfer_thread_args;
#endif
		/* 需等亡子进程？     */
		while (reap_child(false) == true)
			;

		/* fork 前寂 stdio              */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* 其他退出路非零            */
		parallel_jobs++;

		/* 用 _exit 跳 atexit()                        */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
		{
			transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata,
								 old_tablespace);
			/* fork 败                                          */
			/* 空数组元总在尾                */
			_exit(0);
		}
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s", strerror(errno));
#else
		/* 终止线程                              */
		new_arg = transfer_thread_args[parallel_jobs - 1];

		/* 收集已完成子进程状态                               */
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

	/* 无子或亡子   */
	return 0;
}
#endif


/*等待完成一个
 *	
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
		return false;			/* 在 active_threads 计线程索  */
	if (work_status != 0)
		pg_fatal("child process exited abnormally: status %d", work_status);
#else
	/* 取结果              */
	thread_num = WaitForMultipleObjects(parallel_jobs, thread_handles,
										false, wait_for_child ? INFINITE : 0);

	if (thread_num == WAIT_TIMEOUT || thread_num == WAIT_FAILED)
		return false;

	/* 处置句柄防泄                     */
	thread_num -= WAIT_OBJECT_0;

	/* get the result */
	GetExitCodeThread(thread_handles[thread_num], &res);
	if (res != 0)
		pg_fatal("child worker exited abnormally: %s", strerror(errno));

	/* dispose of handle to stop leaks */
	CloseHandle(thread_handles[thread_num]);

	/* 作业移除后执行                     */
	if (thread_num != parallel_jobs - 1)
	{
		void	   *tmp_args;

		thread_handles[thread_num] = thread_handles[parallel_jobs - 1];

		/*
		 * Move last active thread arg struct into the now-dead slot, and the
		 * now-dead slot to the end for reuse by the next thread. Though the
		 * thread struct is in use by another thread, we can safely swap the
		 * struct pointers within the array.
		 */
		tmp_args = cur_thread_args[thread_num];
		cur_thread_args[thread_num] = cur_thread_args[parallel_jobs - 1];
		cur_thread_args[parallel_jobs - 1] = tmp_args;
	}
#endif

	/* do this after job has been removed */
	parallel_jobs--;

	return true;
}
