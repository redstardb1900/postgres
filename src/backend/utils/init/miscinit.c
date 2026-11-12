/*-------------------------------------------------------------------------
 *
 * miscinit.c
 *	  杂项初始化支持例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/miscinit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/param.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <utime.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/file_perm.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


#define DIRECTORY_LOCK_FILE		"postmaster.pid"

ProcessingMode Mode = InitProcessing;

BackendType MyBackendType;

/* 进程退出时要移除的锁文件列表 */
static List *lock_files = NIL;

static Latch LocalLatchData;

/* ----------------------------------------------------------------
 *		忽略系统索引支持例程
 *
 * 注意："忽略系统索引"意味着我们在查找时（无论硬
 * 编码目录访问还是计划器生成的计划）不使用系统
 * 索引。但目录被修改时我们仍会更新索引。
 * 
 * ----------------------------------------------------------------
 */

bool		IgnoreSystemIndexes = false;


/* ----------------------------------------------------------------
 *	common process startup code
 * ----------------------------------------------------------------
 */

/*
 * 为 postmaster 子进程初始化基本环境。
 *
 * 应在子进程启动后尽早调用。但在 EXEC_BACKEND 构建
 * 上确实需要放在 read_backend_variables() 之后。
 */
void
InitPostmasterChild(void)
{
	IsUnderPostmaster = true;	/* 现为 postmaster 子进程 */

	/*
	 * 启动我们的 win32 信号实现。这必须在读取完 backend
	 * 变量之后进行，因为我们需要从父进程拿到信号管
道。	 * 。
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	/*
	 * 为栈深度检查设基准点。在 !EXEC_BACKEND 构建
	 * 看似多余，但无论是否使用该构建选项，深度逻
辑都保持	 * 一致会更好。
	 */
	(void) set_stack_base();

	InitProcessGlobals();

	/*
	 * 在可能写入 stderr 之前先设为二进制模式，
	 * 以防它其实是 syslogger 管道，这样管道分块
	 * 协议不会被打乱。非 logpipe 数据在重定向时
	 * （如 pg_ctl -l）反正会转换。
	 */
#ifdef WIN32
	_setmode(fileno(stderr), _O_BINARY);
#endif

	/* 不要 postmaster 的 proc_exit() 处理器 */
	on_exit_reset();

	/* 在 EXEC_BACKEND 情况下我们不会继承 BlockSig 等值 */
#ifdef EXEC_BACKEND
	pqinitmask();
#endif

	/* 初始化进程本地锁存器支持 */
	InitializeLatchSupport();
	InitProcessLocalLatch();
	InitializeLatchWaitSet();

	/*
	 * 如有可能，让本进程成为组长，这样 postmaster 也能向
	 * 任意子进程发信号。并非所有进程都有子进程，
	 * 但为一致性，所有 postmaster 子进程都这么做。
	 * 
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/*
	 * 每个 postmaster 子进程都应随时迅速响应 SIGQUIT。
	 * 因此我们集中地把 SIGQUIT 移出 BlockSig，并
	 * 装上合适的信号处理器。（面向客户端的进程可
	 * 把它换成 quickdie()。）其余可阻塞信号暂保持
	 * 暂时保持阻塞。
	 */
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);

	sigdelset(&BlockSig, SIGQUIT);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/* 如可能，postmaster 死亡时请求一个信号 */
	PostmasterDeathSignalInit();

	/* 不要把管道交给被执行的子程序 */
#ifndef WIN32
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFD, FD_CLOEXEC) < 0)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to FD_CLOEXEC mode: %m")));
#endif
}

/*
 * 为独立进程初始化基本环境。
 *
 * argv0 须适合用于找到程序的可执行文件。
 */
void
InitStandaloneProcess(const char *argv0)
{
	Assert(!IsPostmasterEnvironment);

	MyBackendType = B_STANDALONE_BACKEND;

	/*
	 * 启动我们的 win32 信号实现
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	InitProcessGlobals();

	/* 初始化进程本地锁存器支持 */
	InitializeLatchSupport();
	InitProcessLocalLatch();
	InitializeLatchWaitSet();

	/*
	 * 为了与 InitPostmasterChild 一致，在此初始化信号掩码。
	 * 但我们不解除 SIGQUIT 阻塞，也不给它配默认处
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/* 计算路径；没有可继承的 postmaster */
	if (my_exec_path[0] == '\0')
	{
		if (find_my_exec(argv0, my_exec_path) < 0)
			elog(FATAL, "%s: could not locate my own executable path",
				 argv0);
	}

	if (pkglib_path[0] == '\0')
		get_pkglib_path(my_exec_path, pkglib_path);
}

void
SwitchToSharedLatch(void)
{
	Assert(MyLatch == &LocalLatchData);
	Assert(MyProc != NULL);

	MyLatch = &MyProc->procLatch;

	if (FeBeWaitSet)
		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetLatchPos, WL_LATCH_SET,
						MyLatch);

	/*
	 * 设置共享锁存器，本地那个可能已被设置。通常
	 * 无需如此，代码本该在等待前先查条件，但小心
	 * 一点总没坏处。
	 */
	SetLatch(MyLatch);
}

void
InitProcessLocalLatch(void)
{
	MyLatch = &LocalLatchData;
	InitLatch(MyLatch);
}

void
SwitchBackToLocalLatch(void)
{
	Assert(MyLatch != &LocalLatchData);
	Assert(MyProc != NULL && MyLatch == &MyProc->procLatch);

	MyLatch = &LocalLatchData;

	if (FeBeWaitSet)
		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetLatchPos, WL_LATCH_SET,
						MyLatch);

	SetLatch(MyLatch);
}

const char *
GetBackendTypeDesc(BackendType backendType)
{
	const char *backendDesc = "unknown process type";

	switch (backendType)
	{
		case B_INVALID:
			backendDesc = "not initialized";
			break;
		case B_ARCHIVER:
			backendDesc = "archiver";
			break;
		case B_AUTOVAC_LAUNCHER:
			backendDesc = "autovacuum launcher";
			break;
		case B_AUTOVAC_WORKER:
			backendDesc = "autovacuum worker";
			break;
		case B_BACKEND:
			backendDesc = "client backend";
			break;
		case B_BG_WORKER:
			backendDesc = "background worker";
			break;
		case B_BG_WRITER:
			backendDesc = "background writer";
			break;
		case B_CHECKPOINTER:
			backendDesc = "checkpointer";
			break;
		case B_LOGGER:
			backendDesc = "logger";
			break;
		case B_STANDALONE_BACKEND:
			backendDesc = "standalone backend";
			break;
		case B_STARTUP:
			backendDesc = "startup";
			break;
		case B_WAL_RECEIVER:
			backendDesc = "walreceiver";
			break;
		case B_WAL_SENDER:
			backendDesc = "walsender";
			break;
		case B_WAL_WRITER:
			backendDesc = "walwriter";
			break;
	}

	return backendDesc;
}

/* ----------------------------------------------------------------
 *				database path / name support stuff
 * ----------------------------------------------------------------
 */

void
SetDatabasePath(const char *path)
{
	/* 每个进程只应发生一次 */
	Assert(!DatabasePath);
	DatabasePath = MemoryContextStrdup(TopMemoryContext, path);
}

/*
 * 校验所提议的数据目录。
 *
 * 同时初始化文件与目录的创建模式和掩码。
 */
void
checkDataDir(void)
{
	struct stat stat_buf;

	Assert(DataDir);

	if (stat(DataDir, &stat_buf) != 0)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("data directory \"%s\" does not exist",
							DataDir)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read permissions of directory \"%s\": %m",
							DataDir)));
	}

	/* 稍后 chdir 会失败，但仍测一下 ... */
	if (!S_ISDIR(stat_buf.st_mode))
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("specified data directory \"%s\" is not a directory",
						DataDir)));

	/*
	 * 检查该目录是否属于我的用户 ID；否则拒绝。
	 *
	 * 这一检查是防止两个 postmaster 在同一目录中启动的
	 * 互锁机制的重要部分（见 CreateLockFile()）。不要
	 * 移除或削弱它。
	 *
	 * XXX Windows 上能安全启用吗？
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_uid != geteuid())
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has wrong ownership",
						DataDir),
				 errhint("The server must be started by the user that owns the data directory.")));
#endif

	/*
	 * 检查目录权限是否正确。若不正确则拒绝。
	 *
	 * 只允许两种模式：0700 和 0750。后者表示应允许对
	 * 所有新建文件和目录进行组读/执行。
	 * 。
	 *
	 * XXX 在 Windows 上暂时跳过该检查，因为可能缺少对
	 * Unix 风格文件权限的适当支持。需要想出在 Windows
	 * 上适用的合理检查。
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (stat_buf.st_mode & PG_MODE_MASK_GROUP)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has invalid permissions",
						DataDir),
				 errdetail("Permissions should be u=rwx (0700) or u=rwx,g=rx (0750).")));
#endif

	/*
	 * 根据数据目录的模式重置创建模式与掩码。
	 *
	 * 掩码在启动早期设为不许组权限作用于新建文件
	 * 和目录。但若数据目录有组读/执行权限，则修改
	 * 创建模式与掩码，以允许对新建文件和目录的组读/
执行，	 * 并设置 data_directory_mode GUC。
	 * 。
	 *
	 * 在 Windows 上跳过，因为可能缺少对 Unix 风格文件权
	 * 限的适当支持。
	 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	SetDataDirectoryCreatePerm(stat_buf.st_mode);

	umask(pg_mode_mask);
	data_directory_mode = pg_dir_create_mode;
#endif

	/* 检查 PG_VERSION */
	ValidatePgVersion(DataDir);
}

/*
 * 设置数据目录，但须确保是绝对路径。用本函数，
 * 勿直接设置 DataDir。
 */
void
SetDataDir(const char *dir)
{
	char	   *new;

	Assert(dir);

	/* 若路径是相对路径则转为绝对路径 */
	new = make_absolute_path(dir);

	free(DataDir);
	DataDir = new;
}

/*
 * 把工作目录切换到 DataDir。postmaster 和 backend 的
 * 大部分代码都假定我们位于 DataDir，以便用相对路径访
 * 问数据目录内及之下的内容。不过在路径设置阶
 * 了方便，我们不在 SetDataDir 中强制执行 chdir。
 */
void
ChangeToDataDir(void)
{
	Assert(DataDir);

	if (chdir(DataDir) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not change directory to \"%s\": %m",
						DataDir)));
}


/* ----------------------------------------------------------------
 *	用户状态
 *
 * 我们必须跟踪与"用户 ID"概念相关的若干不同值。
 * "用户 ID"
 *
 * AuthenticatedUserId 在连接开始时确定，永不改变。
 *
 * SessionUserId 初始时与 AuthenticatedUserId 相同，但
 * 可由 SET SESSION AUTHORIZATION 改变（若
 * AuthenticatedUserIsSuperuser）。这就是 SESSION_USER
 *
 * OuterUserId 是"外层"（任何事务或函数之外）生效的
 * 当前用户 ID。它初始时与 SessionUserId 相同，但可以
 * 通过 SET ROLE 改为 SessionUserId 所属的任何角色。
 * （XXX 改名为 CurrentRoleId 之类？）
 *
 * CurrentUserId 是当前生效的用户 ID；所有常规权限检查
 * 用的都是它。在外层它与 OuterUserId 相同，但在调用
 * SECURITY DEFINER 函数期间会改变，某些特殊命令中
 * 也会局部改变。
 *
 * SecurityRestrictionContext 保存标志，指明更改
 * CurrentUserId 的原因。某些情况下我们需要锁死一些
 * 不直接受权限设置控制的操作，这提供了一种便
捷方式。 * 。
 * ----------------------------------------------------------------
 */
static Oid	AuthenticatedUserId = InvalidOid;
static Oid	SessionUserId = InvalidOid;
static Oid	OuterUserId = InvalidOid;
static Oid	CurrentUserId = InvalidOid;
static const char *SystemUser = NULL;

/* 我们还要记住其中某些层级的超级用户状态 */
static bool AuthenticatedUserIsSuperuser = false;
static bool SessionUserIsSuperuser = false;

static int	SecurityRestrictionContext = 0;

/* 还要记住当前是否有 SET ROLE 在活动 */
static bool SetRoleIsActive = false;

/*
 * GetUserId - 获取当前生效的用户 ID。
 *
 * 已无 SetUserId()；请用 SetUserIdAndSecContext()
 */
Oid
GetUserId(void)
{
	Assert(OidIsValid(CurrentUserId));
	return CurrentUserId;
}


/*
 * GetOuterUserId/SetOuterUserId - 获取/设置外层用户 ID。
 */
Oid
GetOuterUserId(void)
{
	Assert(OidIsValid(OuterUserId));
	return OuterUserId;
}


static void
SetOuterUserId(Oid userid, bool is_superuser)
{
	Assert(SecurityRestrictionContext == 0);
	Assert(OidIsValid(userid));
	OuterUserId = userid;

	/* 我们也强制生效用户 ID 与之匹配 */
	CurrentUserId = userid;

	/* 同时更新 is_superuser GUC 以匹配 OuterUserId 的属性 */
	SetConfigOption("is_superuser",
					is_superuser ? "on" : "off",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
}


/*
 * GetSessionUserId/SetSessionUserId - 读/写会话用户 ID
 */
Oid
GetSessionUserId(void)
{
	Assert(OidIsValid(SessionUserId));
	return SessionUserId;
}

bool
GetSessionUserIsSuperuser(void)
{
	Assert(OidIsValid(SessionUserId));
	return SessionUserIsSuperuser;
}

static void
SetSessionUserId(Oid userid, bool is_superuser)
{
	Assert(SecurityRestrictionContext == 0);
	Assert(OidIsValid(userid));
	SessionUserId = userid;
	SessionUserIsSuperuser = is_superuser;
}

/*
 * 返回代表已认证身份的系统用户。
 * 在 InitializeSystemUser() 中定义为 auth_method
 */
const char *
GetSystemUser(void)
{
	return SystemUser;
}

/*
 * GetAuthenticatedUserId/SetAuthenticatedUserId - 获取/设置
 * 的 ID
 */
Oid
GetAuthenticatedUserId(void)
{
	Assert(OidIsValid(AuthenticatedUserId));
	return AuthenticatedUserId;
}

/*
 * 返回已认证用户在连接开始时是否为超级用户。
 */
bool
GetAuthenticatedUserIsSuperuser(void)
{
	Assert(OidIsValid(AuthenticatedUserId));
	return AuthenticatedUserIsSuperuser;
}

void
SetAuthenticatedUserId(Oid userid, bool is_superuser)
{
	Assert(OidIsValid(userid));

	/* 仅调一次 */
	Assert(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = userid;
	AuthenticatedUserIsSuperuser = is_superuser;

	/* 用已认证用户 ID 标记 PGPROC 条目 */
	/* （假定原子存储，无需锁） */
	MyProc->roleId = userid;
}


/*
 * GetUserIdAndSecContext/SetUserIdAndSecContext - 获取/设置
 * 和 SecurityRestrictionContext 标志
 *
 * 目前 SecurityRestrictionContext 中有三个有效位：
 *
 * SECURITY_LOCAL_USERID_CHANGE 表示我们正处于通过
 * 这些函数临时更改 CurrentUserId 的操作中。需要它来
 * 表明 CurrentUserId 的实际值与 guc.c 的内部状态不同
 * 步，因此必须禁止 SET ROLE。
 *
 * SECURITY_RESTRICTED_OPERATION 表示我们正处于完全
 * 不信任被调用的用户自定义函数的操作中。策略是
在诸如 * autovacuum 和 REINDEX 这类会枚举数据库或 schema 的
 * 关系并对每个关系运行相关函数，操作前使用它，
 * 关系所有者就是新的用户 ID。应在锁定关系之后尽快
 * 设置它；并在关闭关系之前尽可能晚地恢复旧用户 ID；
 * 在关闭后不久恢复也可以容忍。如果某条命令同时具有
 * 枚举与非枚举两种模式（如 ANALYZE），两种模式
 * 都要设置此位。此位不仅禁止 SET ROLE，还禁止对会话
 * 状态的其他更改——它们通常不受保护，但可能被用来
 * 在日后颠覆调用方会话。一个例子是用新代码替换现有
 * 预备语句，下次使用该预备语句时就会以外层会话的
 * 权限执行新代码。这些限制相当严苛，但枚举关系操
作中 * 被调用的函数本来就应该没有副作用。
 * SECURITY_NOFORCE_RLS 表示我们正处于应忽略每表
 * FORCE RLS 指示的操作。用它来
 *
 * 确保 FORCE RLS 不会错误地破坏引用完整性检查。注意
 * 它只在以表所有者身份运行时才会被检查（引用完整性
 * 检查本应总是如此）。
 * 与 GetUserId 不同，GetUserIdAndSecContext *不会*
 * 断言 CurrentUserId 的当前值有效；SetUserIdAndSecContext
 * 本应如此。
 *
 * 错误。因为 StartTransaction 和 AbortTransaction 用
 * 它们保存/恢复设置；backend 首事务中该值
 * 确实无效，或许还要恢复。即便 InitPostgres 失败，
 * 也必须能不触发断言地走完
 * AbortTransaction。
 * 
 * 
 * 
 */
void
GetUserIdAndSecContext(Oid *userid, int *sec_context)
{
	*userid = CurrentUserId;
	*sec_context = SecurityRestrictionContext;
}

void
SetUserIdAndSecContext(Oid userid, int sec_context)
{
	CurrentUserId = userid;
	SecurityRestrictionContext = sec_context;
}


/*
 * InLocalUserIdChange - 正局部改 UID？
 */
bool
InLocalUserIdChange(void)
{
	return (SecurityRestrictionContext & SECURITY_LOCAL_USERID_CHANGE) != 0;
}

/*
 * InSecurityRestrictedOperation - 我们正处于安全受限命令中吗？
 */
bool
InSecurityRestrictedOperation(void)
{
	return (SecurityRestrictionContext & SECURITY_RESTRICTED_OPERATION) != 0;
}

/*
 * InNoForceRLSOperation - 忽略 FORCE RLS？
 */
bool
InNoForceRLSOperation(void)
{
	return (SecurityRestrictionContext & SECURITY_NOFORCE_RLS) != 0;
}


/*
 * 这些是 Get/SetUserIdAndSecContext 的过时版本，仅用于
 * 与 pljava 中可疑代码保持缺陷兼容。允许设置
 * 设置 userid，但仅限于不在安全限制上下文内时。
 * 
 */
void
GetUserIdAndContext(Oid *userid, bool *sec_def_context)
{
	*userid = CurrentUserId;
	*sec_def_context = InLocalUserIdChange();
}

void
SetUserIdAndContext(Oid userid, bool sec_def_context)
{
	/* 抛出与 SET ROLE 相同的错误。 */
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot set parameter \"%s\" within security-restricted operation",
						"role")));
	CurrentUserId = userid;
	if (sec_def_context)
		SecurityRestrictionContext |= SECURITY_LOCAL_USERID_CHANGE;
	else
		SecurityRestrictionContext &= ~SECURITY_LOCAL_USERID_CHANGE;
}


/*
 * 检查指定角色是否具有显式 REPLICATION 权限
 */
bool
has_rolreplication(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolreplication;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * 在常规 backend 启动期间初始化用户身份
 */
void
InitializeSessionUserId(const char *rolename, Oid roleid)
{
	HeapTuple	roleTup;
	Form_pg_authid rform;
	char	   *rname;
	bool		is_superuser;

	/*
	 * 并行 worker 不需要在这里做任何事。
	 * ParallelWorkerMain 已设置好我们的输出变量，而且
我们	 * 不打算强制 rolcanlogin 或 rolconnlimit。此外，我们
	 * 并不真想为该角色做目录查找：若它已被删除，
	 * 我们不想因此失败。
	 */
	if (InitializingParallelWorker)
		return;

	/*
	 * 引导期间不做扫描：系统目录还不存在，而且它
们本来	 * 也应归 postgres 所有。
	 */
	Assert(!IsBootstrapProcessingMode());

	/*
	 * 确保 syscache 条目因最近的目录更改而被清空。这使我
	 * 们能找到在认证期间即时创建的角色。
	 * 
	 */
	AcceptInvalidationMessages();

	/*
	 * 查该角色：给名按名查，否则按 OID 查。
	 */
	if (rolename != NULL)
	{
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(rolename));
		if (!HeapTupleIsValid(roleTup))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role \"%s\" does not exist", rolename)));
	}
	else
	{
		roleTup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
		if (!HeapTupleIsValid(roleTup))
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role with OID %u does not exist", roleid)));
	}

	rform = (Form_pg_authid) GETSTRUCT(roleTup);
	roleid = rform->oid;
	rname = NameStr(rform->rolname);
	is_superuser = rform->rolsuper;

	SetAuthenticatedUserId(roleid, is_superuser);

	/*
	 * 通过 GUC 机制设置 SessionUserId 及相关变量，包括
	 * "role"。
	 *
	 * 注意：理想上这里应用 PGC_S_DYNAMIC_DEFAULT，
	 * 以便 session_authorization 之后仍可被
	 * 条目更改。但实际上 pg_db_role_setting 中的
	 * session_authorization 不生效。要改变这一点需要解决
	 * 个问题：
	 *
	 * 1. 如果 pg_db_role_setting 同时含 session_authorization
	 * 和 role 的值，我们无法确定二者的应用顺序，而顺序
	 * 又有影响。
	 *
	 * 2. 一些站点可能有多年前的 session_authorization 条
	 * 目。没有特别理由要删它们。让这些条目
	 * 重新生效可能大改应用行为，只有大版本发布
	 * 才应该那么做。
	 */
	SetConfigOption("session_authorization", rname,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/*
	 * 以下检查在独立模式下不强制执行，这样才有办
法从	 * 诸如 "UPDATE pg_authid SET rolcanlogin = false;" 这类
	 * 糊涂操作的恢复
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * 该角色允许登录吗。
		 */
		if (!rform->rolcanlogin)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("role \"%s\" is not permitted to log in",
							rname)));

		/*
		 * 检查此角色连接上限。只对常规 backend 强制，
		 * 限制，因为其他进程类型有各自的 PGPROC 池。
		 * 有竞态：
		 *
		 * 自己的 PGPROC。若两个 backend 几乎同时这样，
		 * 它们可能都自以为超限，而理想应一个成功一个
		 * 失败。做得恰如其分似得不偿失；故我们只在
		 * 干脆在文档中说明连接上限是近似的。
		 * 。
		 * 。
		 */
		if (rform->rolconnlimit >= 0 &&
			AmRegularBackendProcess() &&
			!is_superuser &&
			CountUserBackends(roleid) > rform->rolconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for role \"%s\"",
							rname)));
	}

	ReleaseSysCache(roleTup);
}


/*
 * 在特殊 backend 启动期间初始化用户身份
 */
void
InitializeSessionUserIdStandalone(void)
{
	/*
	 * 本函数只应在单用户模式、autovacuum worker 和后台工
	 * 作者中被调用。
	 */
	Assert(!IsUnderPostmaster || IsAutoVacuumWorkerProcess() || IsBackgroundWorker);

	/* 仅调一次 */
	Assert(!OidIsValid(AuthenticatedUserId));

	AuthenticatedUserId = BOOTSTRAP_SUPERUSERID;
	AuthenticatedUserIsSuperuser = true;

	/*
	 * XXX 理想情况下应通过 SetConfigOption("session_authorization")
	 * 来做，但我们缺少所需的角色名，而且也没法取
到它，	 * 因为这个特例的存在理由之一就是即便 BOOTSTRAP_
	 * SUPERUSERID 的 pg_authid 行出了问题也能启动。由于
	 * 我们不亲自设置该 GUC，C 代码看到的值是 NULL，
	 * current_setting() 在本会话中将报告空串。
	 */
	SetSessionAuthorization(BOOTSTRAP_SUPERUSERID, true);

	/* 可用 SetConfigOption("role")，但求一致 */
	SetCurrentRoleId(InvalidOid, false);
}

/*
 * 初始化系统用户。
 *
 * 它构造成 auth_method:authn_id。
 */
void
InitializeSystemUser(const char *authn_id, const char *auth_method)
{
	char	   *system_user;

	/* 仅调一次 */
	Assert(SystemUser == NULL);

	/*
	 * InitializeSystemUser 只应在 authn_id 非 NULL 时调用，
	 * 这意味着 auth_method 有效。
	 */
	Assert(authn_id != NULL);

	system_user = psprintf("%s:%s", auth_method, authn_id);

	/* 把 SystemUser 存入长寿命存储 */
	SystemUser = MemoryContextStrdup(TopMemoryContext, system_user);
	pfree(system_user);
}

/*
 * SQL 函数 SYSTEM_USER
 */
Datum
system_user(PG_FUNCTION_ARGS)
{
	const char *sysuser = GetSystemUser();

	if (sysuser)
		PG_RETURN_DATUM(CStringGetTextDatum(sysuser));
	else
		PG_RETURN_NULL();
}

/*
 * 运行期间更改会话认证 ID。
 *
 * SQL 标准说 SET SESSION AUTHORIZATION 蕴含 SET ROLE
 *  NONE。我们在更高层面实现这一点，而不是在这
里，因 * 为它是 "session_authorization" 的 GUC 赋值钩子，必须
 * 与 SetCurrentRoleId（"role" 的钩子）可交换，因为
 * guc.c 不保证在事务回滚等情况下谁先运行。因此，只有
 * 在 !SetRoleIsActive 时我们才更新派生状态
 * （见上）
 */
void
SetSessionAuthorization(Oid userid, bool is_superuser)
{
	SetSessionUserId(userid, is_superuser);

	if (!SetRoleIsActive)
		SetOuterUserId(userid, is_superuser);
}

/*
 * 报告当前角色 ID
 *		 遵循 SET ROLE 的语义，即返回外层 ID 而非当前
 *		生效 ID，并且当设置在逻辑上是 SET ROLE NONE 时返回
 *		 InvalidOid。
 */
Oid
GetCurrentRoleId(void)
{
	if (SetRoleIsActive)
		return OuterUserId;
	else
		return InvalidOid;
}

/*
 * 运行中更改角色 ID（SET ROLE）
 *
 * 若 roleid 为 InvalidOid，我们执行的是 SET ROLE NONE：
 * 恢复为会话用户授权。此时 is_superuser 参数被忽
略。 * 。
 *
 * 当 roleid 不是 InvalidOid 时，调用者必须已经检查过
 * 会话用户是否有权成为该角色。（这里无法检查，
 * 因为本例程必须能在失败的事务中执行，以恢复 ROLE
 * GUC 变量的先前值。
 */
void
SetCurrentRoleId(Oid roleid, bool is_superuser)
{
	/*
	 * 若为 SET ROLE NONE 则取信息
	 *
	 * 如果 SessionUserId 尚未设置，除了更新 SetRoleIsActive
	 * 之外什么都不做 —— 随后的 SetSessionAuthorization
	 * 调用会更新派生状态。GUC 初始化期间就会
	 * 我们就会被调用。
	 */
	if (!OidIsValid(roleid))
	{
		SetRoleIsActive = false;

		if (!OidIsValid(SessionUserId))
			return;

		roleid = SessionUserId;
		is_superuser = SessionUserIsSuperuser;
	}
	else
		SetRoleIsActive = true;

	SetOuterUserId(roleid, is_superuser);
}


/*
 * 从用户 OID 取用户名；若 noerr 为真，角色不存在时
 * 。
 */
char *
GetUserNameFromId(Oid roleid, bool noerr)
{
	HeapTuple	tuple;
	char	   *result;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
	{
		if (!noerr)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("invalid role OID: %u", roleid)));
		result = NULL;
	}
	else
	{
		result = pstrdup(NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname));
		ReleaseSysCache(tuple);
	}
	return result;
}

/* ------------------------------------------------------------------------
 *				与并行 worker 共享的客户端连接状态
 *
 * ClientConnectionInfo 包含若干关于客户端的信息，需要在
 * 并行 worker 初始化时同步给它们。
 *-------------------------------------------------------------------------
 */

ClientConnectionInfo MyClientConnectionInfo;

/*
 * ClientConnectionInfo 的中间表示，便于序列化。
 * 变长字段紧接在此头部之后分配。
 * 
 */
typedef struct SerializedClientConnectionInfo
{
	int32		authn_id_len;	/* strlen(authn_id) 或 -1 */
	UserAuth	auth_method;
} SerializedClientConnectionInfo;

/*
 * 计算序列化 MyClientConnectionInfo 所需的空间。
 */
Size
EstimateClientConnectionInfoSpace(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(SerializedClientConnectionInfo));

	if (MyClientConnectionInfo.authn_id)
		size = add_size(size, strlen(MyClientConnectionInfo.authn_id) + 1);

	return size;
}

/*
 * 序列化 MyClientConnectionInfo 供并行 worker 使用。
 */
void
SerializeClientConnectionInfo(Size maxsize PG_USED_FOR_ASSERTS_ONLY,
							  char *start_address)
{
	SerializedClientConnectionInfo serialized = {0};

	serialized.authn_id_len = -1;
	serialized.auth_method = MyClientConnectionInfo.auth_method;

	if (MyClientConnectionInfo.authn_id)
		serialized.authn_id_len = strlen(MyClientConnectionInfo.authn_id);

	/* 把序列化表示复制进缓冲区 */
	Assert(maxsize >= sizeof(serialized));
	memcpy(start_address, &serialized, sizeof(serialized));

	maxsize -= sizeof(serialized);
	start_address += sizeof(serialized);

	/* 把 authn_id 复制到其后 */
	if (serialized.authn_id_len >= 0)
	{
		Assert(maxsize >= (serialized.authn_id_len + 1));
		memcpy(start_address,
			   MyClientConnectionInfo.authn_id,
		/* 包含 NULL 终止符以便反序列化 */
			   serialized.authn_id_len + 1);
	}
}

/*
 * 从序列化表示恢复 MyClientConnectionInfo。
 */
void
RestoreClientConnectionInfo(char *conninfo)
{
	SerializedClientConnectionInfo serialized;

	memcpy(&serialized, conninfo, sizeof(serialized));

	/* 把字段复制回原位 */
	MyClientConnectionInfo.authn_id = NULL;
	MyClientConnectionInfo.auth_method = serialized.auth_method;

	if (serialized.authn_id_len >= 0)
	{
		char	   *authn_id;

		authn_id = conninfo + sizeof(serialized);
		MyClientConnectionInfo.authn_id = MemoryContextStrdup(TopMemoryContext,
															  authn_id);
	}
}


/*-------------------------------------------------------------------------
 *				互锁文件支持
 *
 * 这些例程既用于创建数据目录锁文件
 * （$DATADIR/postmaster.pid），也用于创建 Unix 套接字
 * 文件锁文件（$SOCKFILE.lock）。两种文件最初包含相同
 * 的信息，不过数据目录锁文件创建之后还可以用
 * AddToDataDirLockFile() 添加更多信息。内容说明见
 * 信息可后补
 *
 * 锁文件创建成功时，会自动创建一个移除该锁文
件的 * proc_exit 回调。
 *-------------------------------------------------------------------------
 */

/*
 * 移除锁文件的 proc_exit 回调。
 */
static void
UnlinkLockFiles(int status, Datum arg)
{
	ListCell   *l;

	foreach(l, lock_files)
	{
		char	   *curfile = (char *) lfirst(l);

		unlink(curfile);
		/* unlink 失败要抱怨？ */
	}
	/* 反正马上就要退出了，无需回收存储 */
	lock_files = NIL;

	/*
	 * 锁文件移除应当总是 postmaster 或独立 backend 最后的
	 * 对外可见动作，而退出 postmaster 子进程时根本不会走
	 * 到这里。故这里是记录关机完成的好地方。另一
	 * 是教 proc_exit() 来做，但那似乎更丑。独立 backend
 中	 * 用 NOTICE 级别，少些聒噪。
	 * 。
	 */
	ereport(IsPostmasterEnvironment ? LOG : NOTICE,
			(errmsg("database system is shut down")));
}

/*
 * 创建锁文件。
 *
 * filename 是要创建的锁文件的路径名。
 * amPostmaster 用于决定输出 PID 的编码方式。
 * socketDir 是要写入的 Unix 套接字目录路径（可能为
空）。 * isDDLock 和 refName 用于决定产生什么错误消息。
 */
static void
CreateLockFile(const char *filename, bool amPostmaster,
			   const char *socketDir,
			   bool isDDLock, const char *refName)
{
	int			fd;
	char		buffer[MAXPGPATH * 2 + 256];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		other_pid;
	pid_t		my_pid,
				my_p_pid,
				my_gp_pid;
	const char *envvar;

	/*
	 * 若锁文件中的 PID 是我们自己、父进程或祖父
	 * 进程的 PID，则该文件必然失效（多半是上次系
	 * 统启动周期所留）。须做此检查，因重启后很可
	 * 能分配出与上次相同的 PID，或只大一两个数，
	 * 于是锁文件的 PID 现在指向的是某个祖先
	 * shell 进程。我们允许 pg_ctl 通过环境变量
	 * PID 传下它的父 shell PID（我们的祖父）；
	 * 让经 pg_ctl 启动 postmaster 与直启一样可靠。
	 * 没有机制检测更远祖先；但 init 脚本若写得仔
	 * 细，除直接父 shell 外的进程都归 root 所有，
	 * shell 之外的进程都会归 root 所有，kill 测试
	 * 会以 EPERM 失败。注意这种方法不会有假阴性：
	 * 现存 postmaster 绝不会直接启动竞争进程。
	 * 
	 * 
	 * 
	 */
	my_pid = getpid();

#ifndef WIN32
	my_p_pid = getppid();
#else

	/*
	 * Windows 没有 getppid()，但它也不需要，因为它同样
	 * 不用真 kill() ...
	 */
	my_p_pid = 0;
#endif

	envvar = getenv("PG_GRANDPARENT_PID");
	if (envvar)
		my_gp_pid = atoi(envvar);
	else
		my_gp_pid = 0;

	/*
	 * 这里需要循环，因为有竞态。但别无限循环（如
	 * 不可写的 $PGDATA 目录可能导致无法消除的失败）。
	 * 100 次尝试应该足够了。
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * 尝试创建锁文件 —— O_EXCL 保证原子。
		 *
		 * 不要想把文件保护改得比 0600/0640 更弱，见下
		 * 。
		 */
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL, pg_file_create_mode);
		if (fd >= 0)
			break;				/* 成功；退出重试循环 */

		/*
		 * 无法创建 pid 文件。多半是它已存在。
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not create lock file \"%s\": %m",
							filename)));

		/*
		 * 读该文件以取得旧所有者的 PID。注意这里有
竞态：		 * 自我们尝试创建以来文件可能已被删除。
		 */
		fd = open(filename, O_RDONLY, pg_file_create_mode);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* 竞态；重试 */
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open lock file \"%s\": %m",
							filename)));
		}
		pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_READ);
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read lock file \"%s\": %m",
							filename)));
		pgstat_report_wait_end();
		close(fd);

		if (len == 0)
		{
			ereport(FATAL,
					(errcode(ERRCODE_LOCK_FILE_EXISTS),
					 errmsg("lock file \"%s\" is empty", filename),
					 errhint("Either another server is starting, or the lock file is the remnant of a previous server startup crash.")));
		}

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		/* 若 pid < 0，属 postgres */
		other_pid = (pid_t) (encoded_pid < 0 ? -encoded_pid : encoded_pid);

		if (other_pid <= 0)
			elog(FATAL, "bogus data in lock file \"%s\": \"%s\"",
				 filename, buffer);

		/*
		 * 检查另一进程是否还存在
		 *
		 * 按上文讨论，my_pid、my_p_pid 和 my_gp_pid 可作为
		 * 假匹配而忽略。
		 *
		 * 通常若给定 PID 不存在，kill() 会以 ESRCH 失败。
		 * 。
		 *
		 * EPERM 错误情形可视为没问题，因为该错误
		 * 意味着现存进程用户 ID 与我们不同，即它不可
		 * 能是竞争的 postmaster。postmaster 无法成功附加
		 * 属于其用户 ID 的数据目录，checkDataDir() 强制。
		 * 另外，我们以 0600/0640 模式建锁文件，若锁
		 * 文件属于其他用户 ID，上面就已失败 —— 即
		 * kill() 报告的进程并非造锁文件者。（注意：
		 * 最后这点是唯一能防止我们删掉他人运行的
		 * Postgres 实例的 Unix 套接字文件的因素，
		 * 的因素，至少在没有 sticky 位的 /tmp 上
		 * 是如此。
		 * 。
		 */
		if (other_pid != my_pid && other_pid != my_p_pid &&
			other_pid != my_gp_pid)
		{
			if (kill(other_pid, 0) == 0 ||
				(errno != ESRCH && errno != EPERM))
			{
				/* 锁文件属存活进程 */
				ereport(FATAL,
						(errcode(ERRCODE_LOCK_FILE_EXISTS),
						 errmsg("lock file \"%s\" already exists",
								filename),
						 isDDLock ?
						 (encoded_pid < 0 ?
						  errhint("Is another postgres (PID %d) running in data directory \"%s\"?",
								  (int) other_pid, refName) :
						  errhint("Is another postmaster (PID %d) running in data directory \"%s\"?",
								  (int) other_pid, refName)) :
						 (encoded_pid < 0 ?
						  errhint("Is another postgres (PID %d) using socket file \"%s\"?",
								  (int) other_pid, refName) :
						  errhint("Is another postmaster (PID %d) using socket file \"%s\"?",
								  (int) other_pid, refName))));
			}
		}

		/*
		 * 不，创建进程已不存在。但也可能 postmaster
		 * 崩溃了（更可能是被管理员 kill -9），却留下
		 * 了孤儿 backend。为此要检查是否存在仍在使用
的关联		 * 共享内存段。
		 * 。
		 *
		 * 注意：因为 postmaster.pid 分多步写入，我们可能找不
		 * 到其中的共享内存 ID 值；不能把这当作错误。
		 * 。
		 */
		if (isDDLock)
		{
			char	   *ptr = buffer;
			unsigned long id1,
						id2;
			int			lineno;

			for (lineno = 1; lineno < LOCK_FILE_LINE_SHMEM_KEY; lineno++)
			{
				if ((ptr = strchr(ptr, '\n')) == NULL)
					break;
				ptr++;
			}

			if (ptr != NULL &&
				sscanf(ptr, "%lu %lu", &id1, &id2) == 2)
			{
				if (PGSharedMemoryIsInUse(id1, id2))
					ereport(FATAL,
							(errcode(ERRCODE_LOCK_FILE_EXISTS),
							 errmsg("pre-existing shared memory block (key %lu, ID %lu) is still in use",
									id1, id2),
							 errhint("Terminate any old server processes associated with data directory \"%s\".",
									 refName)));
			}
		}

		/*
		 * 看起来没人在家。解除链接并再次尝试创建。
		 * 需要循环，因为与其他可能的创建者之间存在
竞态。		 * 。
		 */
		if (unlink(filename) < 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not remove old lock file \"%s\": %m",
							filename),
					 errhint("The file seems accidentally left over, but "
							 "it could not be removed. Please remove the file "
							 "by hand and try again.")));
	}

	/*
	 * 文件已成功创建，现在填写。内容见 pidfile.h
	 *  中的注释。注意我们把相同的前五行写入数据目录和
	 * 套接字锁文件两处；之后可能还有更多内容加进
	 * 数据目录锁文件。
	 */
	snprintf(buffer, sizeof(buffer), "%d\n%s\n" INT64_FORMAT "\n%d\n%s\n",
			 amPostmaster ? (int) my_pid : -((int) my_pid),
			 DataDir,
			 MyStartTime,
			 PostPortNumber,
			 socketDir);

	/*
	 * 在独立 backend 中，下一行（LOCK_FILE_LINE_LISTEN_
	 * ADDR）永远不会有数据，故现在填成空。
	 */
	if (isDDLock && !amPostmaster)
		strlcat(buffer, "\n", sizeof(buffer));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_WRITE);
	if (write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		/* 写操作未设 errno 时，假定是磁盘空间不足 */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_CREATE_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		errno = save_errno;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}
	pgstat_report_wait_end();
	if (close(fd) != 0)
	{
		int			save_errno = errno;

		unlink(filename);
		errno = save_errno;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not write lock file \"%s\": %m", filename)));
	}

	/*
	 * 安排在 proc_exit 时解除锁文件的链接。如果这是第一
	 * 个，就设置好做这件事的 on_proc_exit 函数；然后把这个
	 * 锁文件加入待解列表。
	 */
	if (lock_files == NIL)
		on_proc_exit(UnlinkLockFiles, 0);

	/*
	 * 使用 lcons 使锁文件按创建的逆序解除链接；这
一点	 * 至关重要！
	 */
	lock_files = lcons(pstrdup(filename), lock_files);
}

/*
 * 创建数据目录锁文件。
 *
 * 被调用时必须已把工作目录切到 DataDir，
 * 直接用相对路径即可。这有助于确保我们锁的
就是该锁 * 的目录。
 *
 * 注意套接字目录路径行最初写为空。postmaster.c 会在
 * 创建第一个 Unix 套接字时重写它。
 */
void
CreateDataDirLockFile(bool amPostmaster)
{
	CreateLockFile(DIRECTORY_LOCK_FILE, amPostmaster, "", true, DataDir);
}

/*
 * 为指定的 Unix 套接字文件创建锁文件。
 */
void
CreateSocketLockFile(const char *socketfile, bool amPostmaster,
					 const char *socketDir)
{
	char		lockfile[MAXPGPATH];

	snprintf(lockfile, sizeof(lockfile), "%s.lock", socketfile);
	CreateLockFile(lockfile, amPostmaster, socketDir, false, socketfile);
}

/*
 * TouchSocketLockFiles —— 标记套接字锁文件
 *
 * 应时常调用本例程，确保套接字锁文件的修改/
 * 访问时间较新。这能防止它们被过分热情的
 * /tmp 目录清理守护进程删除。（这也说明我们本不
该把 * 套接字文件放进 /tmp ...）
 */
void
TouchSocketLockFiles(void)
{
	ListCell   *l;

	foreach(l, lock_files)
	{
		char	   *socketLockFile = (char *) lfirst(l);

		/* 数据目录锁文件不必碰，我们信得过它 */
		if (strcmp(socketLockFile, DIRECTORY_LOCK_FILE) == 0)
			continue;

		/* 这里错误都直接忽略 */
		(void) utime(socketLockFile, NULL);
	}
}


/*
 * 在数据目录锁文件中添加（或替换）一
行。 * 给定字符串不应包含行尾换行符。
 *
 * 注意：因不截断文件，若重写某行时数据比
 * 原来少，最后一行之后就会有垃圾。虽然可以加一个
 * truncate 调用来修，但那会让文件更新变成非原子，我们
 * 宁可避免。因此调用者应努力保证行一旦写入就不缩
短。 * 。
 */
void
AddToDataDirLockFile(int target_line, const char *str)
{
	int			fd;
	int			len;
	int			lineno;
	char	   *srcptr;
	char	   *destptr;
	char		srcbuffer[BLCKSZ];
	char		destbuffer[BLCKSZ];

	fd = open(DIRECTORY_LOCK_FILE, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		return;
	}
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_READ);
	len = read(fd, srcbuffer, sizeof(srcbuffer) - 1);
	pgstat_report_wait_end();
	if (len < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return;
	}
	srcbuffer[len] = '\0';

	/*
	 * 跳过那些不打算重写的行，然后把它们复制到
	 * destbuffer
	 */
	srcptr = srcbuffer;
	for (lineno = 1; lineno < target_line; lineno++)
	{
		char	   *eol = strchr(srcptr, '\n');

		if (eol == NULL)
			break;				/* 文件中的行还不够 */
		srcptr = eol + 1;
	}
	memcpy(destbuffer, srcbuffer, srcptr - srcbuffer);
	destptr = destbuffer + (srcptr - srcbuffer);

	/*
	 * 在目标行之前补齐任何缺失的行，以防各行乱序
加入	 * 文件。
	 */
	for (; lineno < target_line; lineno++)
	{
		if (destptr < destbuffer + sizeof(destbuffer))
			*destptr++ = '\n';
	}

	/*
	 * 写入或重写目标行。
	 */
	snprintf(destptr, destbuffer + sizeof(destbuffer) - destptr, "%s\n", str);
	destptr += strlen(destptr);

	/*
	 * 若旧文件还有更多行，追加到 destbuffer。
	 */
	if ((srcptr = strchr(srcptr, '\n')) != NULL)
	{
		srcptr++;
		snprintf(destptr, destbuffer + sizeof(destbuffer) - destptr, "%s",
				 srcptr);
	}

	/*
	 * 然后重写数据。由于用单次内核调用写入，旁观
者看到的	 * 更新应是原子的。
	 */
	len = strlen(destbuffer);
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_WRITE);
	if (pg_pwrite(fd, destbuffer, len, 0) != len)
	{
		pgstat_report_wait_end();
		/* 写操作未设 errno 时，假定是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return;
	}
	pgstat_report_wait_end();
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_ADDTODATADIR_SYNC);
	if (pg_fsync(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
	pgstat_report_wait_end();
	if (close(fd) != 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
	}
}


/*
 * 复查数据目录锁文件是否仍以预期内容存在。若
锁文件 * 看起来正常则返回真，否则返回假。
 *
 * postmaster 中我们会周期性地调用它。思路是：如果锁
 * 文件被删除或被另一个 postmaster 替换，我们就应做
 * panic 级数据库关机。因此任何有疑虑的情况都应返回
 * 真：我们不想造成不必要的 panic 关机。EINTR 或
 * ENFILE 之类的瞬态失败不应导致我们失败。（如果
真有 * 问题，未来的复查会发现的。）
 */
bool
RecheckDataDirLockFile(void)
{
	int			fd;
	int			len;
	long		file_pid;
	char		buffer[BLCKSZ];

	fd = open(DIRECTORY_LOCK_FILE, O_RDWR | PG_BINARY, 0);
	if (fd < 0)
	{
		/*
		 * 有许多可预见的假阳性错误。为安全起见，只在
		 * 列举出的明显有问题的情形上失败。
		 * 
		 */
		switch (errno)
		{
			case ENOENT:
			case ENOTDIR:
				/* 灾难 */
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								DIRECTORY_LOCK_FILE)));
				return false;
			default:
				/* 非致命（目前） */
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m; continuing anyway",
								DIRECTORY_LOCK_FILE)));
				return true;
		}
	}
	pgstat_report_wait_start(WAIT_EVENT_LOCK_FILE_RECHECKDATADIR_READ);
	len = read(fd, buffer, sizeof(buffer) - 1);
	pgstat_report_wait_end();
	if (len < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						DIRECTORY_LOCK_FILE)));
		close(fd);
		return true;			/* 读失败视为非致命 */
	}
	buffer[len] = '\0';
	close(fd);
	file_pid = atol(buffer);
	if (file_pid == getpid())
		return true;			/* 正常 */

	/* 麻烦了：有人覆写了锁文件 */
	ereport(LOG,
			(errmsg("lock file \"%s\" contains wrong PID: %ld instead of %ld",
					DIRECTORY_LOCK_FILE, file_pid, (long) getpid())));
	return false;
}


/*-------------------------------------------------------------------------
 *				版本检查支持
 *-------------------------------------------------------------------------
 */

/*
 * 判断目录 `path' 中的 PG_VERSION 文件是否表明数据
 * 版本与本程序版本兼容。
 *
 * 若兼容则返回。否则 ereport(FATAL)。
 */
void
ValidatePgVersion(const char *path)
{
	char		full_path[MAXPGPATH];
	FILE	   *file;
	int			ret;
	long		file_major;
	long		my_major;
	char	   *endptr;
	char		file_version_string[64];
	const char *my_version_string = PG_VERSION;

	my_major = strtol(my_version_string, &endptr, 10);

	snprintf(full_path, sizeof(full_path), "%s/PG_VERSION", path);

	file = AllocateFile(full_path, "r");
	if (!file)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not a valid data directory",
							path),
					 errdetail("File \"%s\" is missing.", full_path)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", full_path)));
	}

	file_version_string[0] = '\0';
	ret = fscanf(file, "%63s", file_version_string);
	file_major = strtol(file_version_string, &endptr, 10);

	if (ret != 1 || endptr == file_version_string)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a valid data directory",
						path),
				 errdetail("File \"%s\" does not contain valid data.",
						   full_path),
				 errhint("You might need to initdb.")));

	FreeFile(file);

	if (my_major != file_major)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("database files are incompatible with server"),
				 errdetail("The data directory was initialized by PostgreSQL version %s, "
						   "which is not compatible with this version %s.",
						   file_version_string, my_version_string)));
}

/*-------------------------------------------------------------------------
 *				库预加载支持
 *-------------------------------------------------------------------------
 */

/*
 * GUC 变量：postmaster 启动时与 backend 启动时要预
 * 加载的库名列表
 */
char	   *session_preload_libraries_string = NULL;
char	   *shared_preload_libraries_string = NULL;
char	   *local_preload_libraries_string = NULL;

/* 标志：我们正在加载 shared_preload_libraries */
bool		process_shared_preload_libraries_in_progress = false;
bool		process_shared_preload_libraries_done = false;

shmem_request_hook_type shmem_request_hook = NULL;
bool		process_shmem_requests_in_progress = false;

/*
 * 加载 'libraries' 中列出的共享库。
 *
 * 'gucname'：GUC 变量名，用于错误报告。
 * 'restricted'：为真时强制库位于 $libdir/plugins/。
 */
static void
load_libraries(const char *libraries, const char *gucname, bool restricted)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;

	if (libraries == NULL || libraries[0] == '\0')
		return;					/* 无事可做 */

	/* 需要可改的字符串副本 */
	rawstring = pstrdup(libraries);

	/* 把字符串解析成路径列表 */
	if (!SplitDirectoriesString(rawstring, ',', &elemlist))
	{
		/* 列表有语法错 */
		list_free_deep(elemlist);
		pfree(rawstring);
		ereport(LOG,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid list syntax in parameter \"%s\"",
						gucname)));
		return;
	}

	foreach(l, elemlist)
	{
		/* 注意 filename 已被规范化 */
		char	   *filename = (char *) lfirst(l);
		char	   *expanded = NULL;

		/* 若在限制模式下且未提及，则插入 $libdir/plugins */
		if (restricted && first_dir_separator(filename) == NULL)
		{
			expanded = psprintf("$libdir/plugins/%s", filename);
			filename = expanded;
		}
		load_file(filename, restricted);
		ereport(DEBUG1,
				(errmsg_internal("loaded library \"%s\"", filename)));
		if (expanded)
			pfree(expanded);
	}

	list_free_deep(elemlist);
	pfree(rawstring);
}

/*
 * 处理 postmaster 启动时应预加载的库
 */
void
process_shared_preload_libraries(void)
{
	process_shared_preload_libraries_in_progress = true;
	load_libraries(shared_preload_libraries_string,
				   "shared_preload_libraries",
				   false);
	process_shared_preload_libraries_in_progress = false;
	process_shared_preload_libraries_done = true;
}

/*
 * 处理 backend 启动时应预加载的库
 */
void
process_session_preload_libraries(void)
{
	load_libraries(session_preload_libraries_string,
				   "session_preload_libraries",
				   false);
	load_libraries(local_preload_libraries_string,
				   "local_preload_libraries",
				   true);
}

/*
 * 处理预加载库发来的共享内存请求
 */
void
process_shmem_requests(void)
{
	process_shmem_requests_in_progress = true;
	if (shmem_request_hook)
		shmem_request_hook();
	process_shmem_requests_in_progress = false;
}

void
pg_bindtextdomain(const char *domain)
{
#ifdef ENABLE_NLS
	if (my_exec_path[0] != '\0')
	{
		char		locale_path[MAXPGPATH];

		get_locale_path(my_exec_path, locale_path);
		bindtextdomain(domain, locale_path);
		pg_bind_textdomain_codeset(domain);
	}
#endif
}
