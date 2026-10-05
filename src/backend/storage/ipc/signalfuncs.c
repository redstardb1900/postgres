/*-------------------------------------------------------------------------
 *
 * signalfuncs.c
 *	  向后端发信号的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/signalfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/syslogger.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"


/*
 * 向另一个后端发送信号。
 *
 * 若用户是超级用户，或与被发信号的后端属于同一角色，
 * 信号就会送达。对于 "dangerous" 信号，调用本函数之前
 * 需要显式检查超级用户身份。
 *
 * 成功返回 0，一般失败返回 1，普通权限错误返回 2，
 * 调用方需要是超级用户时返回 3。
 *
 * 发生一般失败（返回码 1）时，会发出一条
 * 警告消息。权限错误则由调用方负责
 * 发出警告。
 */
#define SIGNAL_BACKEND_SUCCESS 0
#define SIGNAL_BACKEND_ERROR 1
#define SIGNAL_BACKEND_NOPERMISSION 2
#define SIGNAL_BACKEND_NOSUPERUSER 3
static int
pg_signal_backend(int pid, int sig)
{
	PGPROC	   *proc = BackendPidGetProc(pid);

	/*
	 * 若 pid 无效，BackendPidGetProc 返回 NULL；但等到
	 * 我们调用 kill() 时，这里拿到有效 proc 的进程
	 * 可能已经自行终止。无法对任意进程加锁来
	 * 阻止这一点。不过迄今本机制的所有调用方反正都是
	 * 在请求结束该进程，因此它可能先自行结束
	 * 并不是问题。
	 *
	 * 注意：若 pid 指向辅助进程或 postmaster，proc 也会
	 * 是 NULL（二者都不能通过 pg_signal_backend()
	 * 发信号）。
	 */
	if (proc == NULL)
	{
		/*
		 * 这只是一条警告，这样在遍历结果集时，若某个后端
		 * 在运行期间自行终止，也不会中止。
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process", pid)));

		return SIGNAL_BACKEND_ERROR;
	}

	/*
	 * 只允许超级用户向超级用户拥有的后端发信号。任何
	 * 未公布角色的进程，其重要性可能不亚于超级用户拥有的
	 * 后端，因此按那种方式对待。
	 */
	if ((!OidIsValid(proc->roleId) || superuser_arg(proc->roleId)) &&
		!superuser())
		return SIGNAL_BACKEND_NOSUPERUSER;

	/* 用户可以向自己具有角色成员资格的后端发信号。 */
	if (!has_privs_of_role(GetUserId(), proc->roleId) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
		return SIGNAL_BACKEND_NOPERMISSION;

	/*
	 * 上面刚验证过的进程会不会在到达这里之前结束，
	 * 随后该 pid 被回收给新进程？那样我们就会试图
	 * 杀掉错误的对象。在使用顺序 pid 分配与回绕时，
	 * 这近乎不可能。在 pid 重用随机化的系统上也许
	 * 会发生。这种竞态的可能性
	 * 低到不值得担心。
	 */

	/* 若有 setsid()，就向该后端的整个进程组发信号 */
#ifdef HAVE_SETSID
	if (kill(-pid, sig))
#else
	if (kill(pid, sig))
#endif
	{
		/* 同样，只发警告，以便循环可以继续 */
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		return SIGNAL_BACKEND_ERROR;
	}
	return SIGNAL_BACKEND_SUCCESS;
}

/*
 * 发信号以取消一个后端进程。若你是其进程正被取消的
 * 那个角色的成员，就允许这样做。
 *
 * 注意：只有超级用户可以向超级用户拥有的进程发信号。
 */
Datum
pg_cancel_backend(PG_FUNCTION_ARGS)
{
	int			r = pg_signal_backend(PG_GETARG_INT32(0), SIGINT);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to cancel query"),
				 errdetail("Only roles with the %s attribute may cancel queries of roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to cancel query"),
				 errdetail("Only roles with privileges of the role whose query is being canceled or with privileges of the \"%s\" role may cancel this query.",
						   "pg_signal_backend")));

	PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * 一直等到不存在具有给定 PID 的后端进程，然后返回真。
 * 超时时发出警告并返回假。
 */
static bool
pg_wait_until_termination(int pid, int64 timeout)
{
	/*
	 * 以 waittime 毫秒为步长等待，直到本函数退出
	 * 或超时。
	 */
	int64		waittime = 100;

	/*
	 * 最初，剩余时间就是用户指定的整个超时时间。
	 */
	int64		remainingtime = timeout;

	/*
	 * 检查后端是否存在。若后端仍然存在，就等待
	 * waittime 毫秒，然后再次检查是否存在。重复这一过程，
	 * 直到超时、发生错误，或某个待处理中断（例如查询取消）
	 * 得到处理。
	 */
	do
	{
		if (remainingtime < waittime)
			waittime = remainingtime;

		if (kill(pid, 0) == -1)
		{
			if (errno == ESRCH)
				return true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not check the existence of the backend with PID %d: %m",
								pid)));
		}

		/* 等待之前先处理中断（如有） */
		CHECK_FOR_INTERRUPTS();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 waittime,
						 WAIT_EVENT_BACKEND_TERMINATION);

		ResetLatch(MyLatch);

		remainingtime -= waittime;
	} while (remainingtime > 0);

	ereport(WARNING,
			(errmsg_plural("backend with PID %d did not terminate within %lld millisecond",
						   "backend with PID %d did not terminate within %lld milliseconds",
						   timeout,
						   pid, (long long int) timeout)));

	return false;
}

/*
 * 发信号以终止一个后端进程。若你是其进程正被
 * 终止的那个角色的成员，就允许这样做。若超时输入
 * 参数为 0，本函数只向该后端发信号并返回真。
 * 若超时非零，则一直等到没有任何进程具有给定 PID；
 * 若进程在超时内结束则返回真，若超过超时则发出
 * 警告并返回假。
 *
 * 注意：只有超级用户可以向超级用户拥有的进程发信号。
 */
Datum
pg_terminate_backend(PG_FUNCTION_ARGS)
{
	int			pid;
	int			r;
	int			timeout;		/* 毫秒 */

	pid = PG_GETARG_INT32(0);
	timeout = PG_GETARG_INT64(1);

	if (timeout < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("\"timeout\" must not be negative")));

	r = pg_signal_backend(pid, SIGTERM);

	if (r == SIGNAL_BACKEND_NOSUPERUSER)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to terminate process"),
				 errdetail("Only roles with the %s attribute may terminate processes of roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	if (r == SIGNAL_BACKEND_NOPERMISSION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to terminate process"),
				 errdetail("Only roles with privileges of the role whose process is being terminated or with privileges of the \"%s\" role may terminate this process.",
						   "pg_signal_backend")));

	/* 仅在成功且确实请求了等待时才等待 */
	if (r == SIGNAL_BACKEND_SUCCESS && timeout > 0)
		PG_RETURN_BOOL(pg_wait_until_termination(pid, timeout));
	else
		PG_RETURN_BOOL(r == SIGNAL_BACKEND_SUCCESS);
}

/*
 * 发信号以重新加载数据库配置
 *
 * 本函数的权限检查通过常规的
 * GRANT 系统管理。
 */
Datum
pg_reload_conf(PG_FUNCTION_ARGS)
{
	if (kill(PostmasterPid, SIGHUP))
	{
		ereport(WARNING,
				(errmsg("failed to send signal to postmaster: %m")));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}


/*
 * 轮换日志文件
 *
 * 保留本函数是为了支持 adminpack 1.0。
 */
Datum
pg_rotate_logfile(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rotate log files with adminpack 1.0"),
		/* translator: %s is a SQL function name */
				 errhint("Consider using %s, which is part of core, instead.",
						 "pg_logfile_rotate()")));

	if (!Logging_collector)
	{
		ereport(WARNING,
				(errmsg("rotation not possible because log collection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}

/*
 * 轮换日志文件
 *
 * 本函数的权限检查通过常规的
 * GRANT 系统管理。
 */
Datum
pg_rotate_logfile_v2(PG_FUNCTION_ARGS)
{
	if (!Logging_collector)
	{
		ereport(WARNING,
				(errmsg("rotation not possible because log collection not active")));
		PG_RETURN_BOOL(false);
	}

	SendPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE);
	PG_RETURN_BOOL(true);
}
