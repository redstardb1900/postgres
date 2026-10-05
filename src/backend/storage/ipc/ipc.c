/*-------------------------------------------------------------------------
 *
 * ipc.c
 *	  POSTGRES 进程间通信相关定义。
 *
 * 本文件名已不太贴切：这里几乎不再有与 IPC 直接
 * 相关的内容。此处功能关注的是为 postmaster 或
 * backend 管理退出时的清理工作。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "miscadmin.h"
#ifdef PROFILE_PID_DIR
#include "postmaster/autovacuum.h"
#endif
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"


/*
 * 这个标志在 proc_exit() 期间设置，用来改变 ereport() 的行为，
 * 使 on_proc_exit 例程里的 ereport() 无法把我们拽出
 * 退出过程。我们不想回到空闲循环……
 */
bool		proc_exit_inprogress = false;

/*
 * 在 shmem_exit() 进行期间设置。
 */
bool		shmem_exit_inprogress = false;

/*
 * 这个标志记录当前进程（或父 postmaster）里
 * 是否已经调用过 atexit()。
 */
static bool atexit_callback_setup = false;

/* 本地函数 */
static void proc_exit_prepare(int code);


/* ----------------------------------------------------------------
 *						exit() 处理相关的东西。
 *
 * 这些函数大体与 atexit() 同一思路，
 * 但提供了我们需要的一些额外特性。特别是，
 * 我们希望登记回调，以便在与损坏的共享内存上下文
 * 断开、但 postmaster 并不退出时调用。
 *
 * 回调函数可以接受零个、一个或两个参数：第一个传入
 * 参数是整数退出码，第二个是登记回调时
 * 提供的 Datum。
 * ----------------------------------------------------------------
 */

#define MAX_ON_EXITS 20

struct ONEXIT
{
	pg_on_exit_callback function;
	Datum		arg;
};

static struct ONEXIT on_proc_exit_list[MAX_ON_EXITS];
static struct ONEXIT on_shmem_exit_list[MAX_ON_EXITS];
static struct ONEXIT before_shmem_exit_list[MAX_ON_EXITS];

static int	on_proc_exit_index,
			on_shmem_exit_index,
			before_shmem_exit_index;


/* ----------------------------------------------------------------
 *		proc_exit
 *
 *		本函数调用所有为它登记的回调
 *		（用以释放资源），然后调用 exit。
 *
 *		这应当是唯一调用 exit() 的函数
 *		（-cim 2/6/90）。
 *
 *		可惜我们没法真正保证附加代码
 *		遵守不直接调用 exit() 的规则。因此，虽然
 *		这是退出系统的首选方式，我们还登记了一个
 *		atexit 回调，以确保清理一定会发生。
 * ----------------------------------------------------------------
 */
void
proc_exit(int code)
{
	/* 若由 system() 等派生出来则不安全。 */
	if (MyProcPid != (int) getpid())
		elog(PANIC, "proc_exit() called in child process");

	/* 清理一切必须清理的东西 */
	proc_exit_prepare(code);

#ifdef PROFILE_PID_DIR
	{
		/*
		 * 若我们正在对自己做性能剖析，gprof 的 mcleanup() 即将
		 * 把剖析结果写到 ./gmon.out。由于 mcleanup() 总是使用
		 * 固定文件名，每个后端都会覆盖先前的剖析文件。为了
		 * 解决这个问题，我们为每个后端建立单独的子目录
		 * （./gprof/pid），并在调用 exit() 退出前 cd 到该子目录。
		 * 这迫使 mcleanup() 把每份剖析写进它自己的目录。我们
		 * 最终会得到类似这样的结果：$PGDATA/gprof/8829/gmon.out
		 * $PGDATA/gprof/8845/gmon.out ……。
		 *
		 * 为避免磁盘空间不受欢迎地膨胀，自动清理工作进程受到
		 * 区别对待：它们的 gmon.out 全部进入同一个
		 * 子目录。否则，一个只是闲置在那里的安装
		 * 每隔几秒也会吃掉数兆字节的磁盘空间。
		 *
		 * 注意，我们在这里做这件事，而不是放在 on_proc_exit() 回调里，
		 * 因为我们想确保这段代码最后执行。我们不想
		 * 干扰任何其他 on_proc_exit() 回调。出于同样的原因，
		 * 我们不把它放进 proc_exit_prepare ……因此若你
		 * 以错误的方式退出，剖析文件就不会落在
		 * 一个合适的地方。
		 */
		char		gprofDirName[32];

		if (IsAutoVacuumWorkerProcess())
			snprintf(gprofDirName, 32, "gprof/avworker");
		else
			snprintf(gprofDirName, 32, "gprof/%d", (int) getpid());

		/*
		 * 这里用 mkdir() 而不是 MakePGDirectory()，因为我们
		 * 并不是在创建 PG 目录。
		 */
		mkdir("gprof", S_IRWXU | S_IRWXG | S_IRWXO);
		mkdir(gprofDirName, S_IRWXU | S_IRWXG | S_IRWXO);
		chdir(gprofDirName);
	}
#endif

	elog(DEBUG3, "exit(%d)", code);

	exit(code);
}

/*
 * proc_exit 与 atexit 处理函数共用的代码。注意，经
 * proc_exit 的正常退出实际上会调用它两次……
 * 但第二次将无事可做。
 */
static void
proc_exit_prepare(int code)
{
	/*
	 * 一旦设置这个标志，我们就决心退出。任何 ereport()
	 * 都不会把控制交回主循环，而是直接回到这里。
	 */
	proc_exit_inprogress = true;

	/*
	 * 忘掉任何待处理的取消或死亡请求；我们已经在尽力
	 * 收摊了。注意，既然 proc_exit_inprogress 已设置，
	 * 信号处理函数不会再设置这些标志。
	 */
	InterruptPending = false;
	ProcDiePending = false;
	QueryCancelPending = false;
	InterruptHoldoffCount = 1;
	CritSectionCount = 0;

	/*
	 * 同时清空错误上下文栈，以免 proc_exit 期间的 elog/ereport
	 * 调用去调用错误回调。它们想提供的上下文多半已经
	 * 无关，而且在我们已经做过诸如中止任何打开的事务之后，
	 * 它们很可能直接失败。（在正常退出情形下，上下文
	 * 栈本来就应当是空的，但在例如
	 * elog(FATAL) 的情况下
	 * 可能不是。）
	 */
	error_context_stack = NULL;
	/* 出于同样的原因，在 debug_query_string 被破坏之前把它重置 */
	debug_query_string = NULL;

	/* 先做我们的共享内存退出 */
	shmem_exit(code);

	elog(DEBUG3, "proc_exit(%d): %d callbacks to make",
		 code, on_proc_exit_index);

	/*
	 * 调用所有已登记的回调。
	 *
	 * 注意，由于我们每次都递减 on_proc_exit_index，若某个
	 * 回调调用 ereport(ERROR) 或 ereport(FATAL)，控制回到
	 * 这里时就不会再调用它（先前已完成的回调也不会）。
	 * 因此不应当可能出现
	 * 无限循环。
	 */
	while (--on_proc_exit_index >= 0)
		on_proc_exit_list[on_proc_exit_index].function(code,
													   on_proc_exit_list[on_proc_exit_index].arg);

	on_proc_exit_index = 0;
}

/* ------------------
 * 运行全部 on_shmem_exit 例程，但并不真正退出。
 * postmaster 用它在某个后端惨死之后重新初始化共享内存和
 * 信号量。与 proc_exit() 一样，我们在调用每个回调之前
 * 先把它从列表里去掉，以免
 * 出错时无限循环。
 * ------------------
 */
void
shmem_exit(int code)
{
	shmem_exit_inprogress = true;

	/*
	 * 调用 before_shmem_exit 回调。
	 *
	 * 这些应当是仍需要系统大部分还在运行的事情，
	 * 例如清理临时关系，这需要访问系统目录；
	 * 或者是必须完成的事情，因为后面的清理步骤
	 * 依赖它们，例如释放 lwlocks。
	 */
	elog(DEBUG3, "shmem_exit(%d): %d before_shmem_exit callbacks to make",
		 code, before_shmem_exit_index);
	while (--before_shmem_exit_index >= 0)
		before_shmem_exit_list[before_shmem_exit_index].function(code,
																 before_shmem_exit_list[before_shmem_exit_index].arg);
	before_shmem_exit_index = 0;

	/*
	 * 调用动态共享内存回调。
	 *
	 * 它们的作用与后期回调相同，但针对的是动态共享内存
	 * 段，而不是主共享内存段。
	 * dsm_backend_shutdown() 有着与主共享内存段相同的
	 * 渐进逻辑：它在调用每个回调之前先注销它，
	 * 这样若其中某个回调抛出 ERROR 或 FATAL，
	 * 我们就不会陷在无限循环里。
	 *
	 * 注意，在这里显式调用该函数，与把它登记为
	 * on_shmem_exit 回调相当不同，原因正是：
	 * 若一个动态共享内存回调出错，其余
	 * 回调仍会被调用。因此把这次调用写死，就使它
	 * 与主共享内存段的回调处于同等地位。
	 */
	dsm_backend_shutdown();

	/*
	 * 调用 on_shmem_exit 回调。
	 *
	 * 这些通常是在释放低层共享内存资源。有些情况下，
	 * 这是对早期回调本身可能失败、导致重新进入本例程的
	 * 一种兜底；另一些情况下，这是只在进程退出时
	 * 才做的清理。
	 */
	elog(DEBUG3, "shmem_exit(%d): %d on_shmem_exit callbacks to make",
		 code, on_shmem_exit_index);
	while (--on_shmem_exit_index >= 0)
		on_shmem_exit_list[on_shmem_exit_index].function(code,
														 on_shmem_exit_list[on_shmem_exit_index].arg);
	on_shmem_exit_index = 0;

	shmem_exit_inprogress = false;
}

/* ----------------------------------------------------------------
 *		atexit_callback
 *
 *		兜底，确保有人直接调用 exit() 退出时不会把我们搞乱。
 *
 * 特别不合作的人可以调用 _exit()，
 * 但那种情况我们有一个死人开关，会让
 * postmaster 把它当成崩溃来处理。见 pmsignal.c。
 * ----------------------------------------------------------------
 */
static void
atexit_callback(void)
{
	/* 清理一切必须清理的东西 */
	/* ……可惜我们不知道真正的退出码…… */
	proc_exit_prepare(-1);
}

/* ----------------------------------------------------------------
 *		on_proc_exit
 *
 *		本函数把一个回调加进
 *		proc_exit() 所调用的函数列表。   -cim 2/6/90
 * ----------------------------------------------------------------
 */
void
on_proc_exit(pg_on_exit_callback function, Datum arg)
{
	if (on_proc_exit_index >= MAX_ON_EXITS)
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("out of on_proc_exit slots")));

	on_proc_exit_list[on_proc_exit_index].function = function;
	on_proc_exit_list[on_proc_exit_index].arg = arg;

	++on_proc_exit_index;

	if (!atexit_callback_setup)
	{
		atexit(atexit_callback);
		atexit_callback_setup = true;
	}
}

/* ----------------------------------------------------------------
 *		before_shmem_exit
 *
 *		登记早期回调，以便在开始关闭
 *		低层子系统之前做用户级清理，
 *		例如中止事务。
 * ----------------------------------------------------------------
 */
void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	if (before_shmem_exit_index >= MAX_ON_EXITS)
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("out of before_shmem_exit slots")));

	before_shmem_exit_list[before_shmem_exit_index].function = function;
	before_shmem_exit_list[before_shmem_exit_index].arg = arg;

	++before_shmem_exit_index;

	if (!atexit_callback_setup)
	{
		atexit(atexit_callback);
		atexit_callback_setup = true;
	}
}

/* ----------------------------------------------------------------
 *		on_shmem_exit
 *
 *		登记普通回调，做低层关闭
 *		（例如释放我们的 PGPROC）；在 before_shmem_exit
 *		回调之后、on_proc_exit 回调之前运行。
 * ----------------------------------------------------------------
 */
void
on_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	if (on_shmem_exit_index >= MAX_ON_EXITS)
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("out of on_shmem_exit slots")));

	on_shmem_exit_list[on_shmem_exit_index].function = function;
	on_shmem_exit_list[on_shmem_exit_index].arg = arg;

	++on_shmem_exit_index;

	if (!atexit_callback_setup)
	{
		atexit(atexit_callback);
		atexit_callback_setup = true;
	}
}

/* ----------------------------------------------------------------
 *		cancel_before_shmem_exit
 *
 *		本函数去掉一个先前登记的 before_shmem_exit
 *		回调。我们只查看最新的一项来删除，因为我们
 * 		预期调用方以严格的后进先出顺序添加和删除
 * 		临时的 before_shmem_exit 回调。
 * ----------------------------------------------------------------
 */
void
cancel_before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	if (before_shmem_exit_index > 0 &&
		before_shmem_exit_list[before_shmem_exit_index - 1].function
		== function &&
		before_shmem_exit_list[before_shmem_exit_index - 1].arg == arg)
		--before_shmem_exit_index;
	else
		elog(ERROR, "before_shmem_exit callback (%p,0x%llx) is not the latest entry",
			 function, (long long) arg);
}

/* ----------------------------------------------------------------
 *		on_exit_reset
 *
 *		本函数清除所有 on_proc_exit() 和 on_shmem_exit()
 *		已登记的函数。这用在刚 fork 出一个后端之后，
 *		使该后端不会认为自己退出时应当调用
 *		postmaster 的退出例程……
 * ----------------------------------------------------------------
 */
void
on_exit_reset(void)
{
	before_shmem_exit_index = 0;
	on_shmem_exit_index = 0;
	on_proc_exit_index = 0;
	reset_on_dsm_detach();
}

/* ----------------------------------------------------------------
 *		check_on_shmem_exit_lists_are_empty
 *
 *		调试检查：当前进程没有过早登记
 *		共享内存清理处理函数。
 * ----------------------------------------------------------------
 */
void
check_on_shmem_exit_lists_are_empty(void)
{
	if (before_shmem_exit_index)
		elog(FATAL, "before_shmem_exit has been called prematurely");
	if (on_shmem_exit_index)
		elog(FATAL, "on_shmem_exit has been called prematurely");
	/* 鉴于上面的检查，再查动态共享内存脱离状态似乎没有必要 */
}
