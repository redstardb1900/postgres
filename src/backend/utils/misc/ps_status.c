/*--------------------------------------------------------------------
 * ps_status.c
 *
 * Routines to support changing the ps display of PostgreSQL backends
 * to contain some useful information. Mechanism differs wildly across
 * platforms.
 *
 * src/backend/utils/misc/ps_status.c
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 * various details abducted from various places
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#if defined(__darwin__)
#include <crt_externs.h>
#endif

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/guc.h"
#include "utils/ps_status.h"

#if !defined(WIN32) || defined(_MSC_VER)
extern char **environ;
#endif

/* GUC 变量 */
bool		update_process_title = DEFAULT_UPDATE_PROCESS_TITLE;

/*
 * 更新 ps 显示的备选方式：
 *
 * PS_USE_SETPROCTITLE_FAST
 *	   用 setproctitle_fast(const char *, ...) 函数
 *	   (FreeBSD)
 * PS_USE_SETPROCTITLE
 *	   用 setproctitle(const char *, ...) 函数
 *	   (other BSDs)
 * PS_USE_CLOBBER_ARGV
 *	   改写 argv 和环境区域
 *	   (Linux 及多数类 SysV 系统)
 * PS_USE_WIN32
 *	   把该字符串作为 Windows 事件名发出
 * PS_USE_NONE
 *	   不更新 ps 显示
 *	   （默认值，因它最安全。）
 */
#if defined(HAVE_SETPROCTITLE_FAST)
#define PS_USE_SETPROCTITLE_FAST
#elif defined(HAVE_SETPROCTITLE)
#define PS_USE_SETPROCTITLE
#elif defined(__linux__) || defined(_AIX) || defined(__sun) || defined(__darwin__)
#define PS_USE_CLOBBER_ARGV
#elif defined(WIN32)
#define PS_USE_WIN32
#else
#define PS_USE_NONE
#endif


/* 不同系统对缓冲区填充的要求不同 */
#if defined(_AIX) || defined(__linux__) || defined(__darwin__)
#define PS_PADDING '\0'
#else
#define PS_PADDING ' '
#endif


#ifndef PS_USE_NONE

#ifndef PS_USE_CLOBBER_ARGV
/* 除一个选项外都需要缓冲区来写 ps 行 */
#define PS_BUFFER_SIZE 256
static char ps_buffer[PS_BUFFER_SIZE];
static const size_t ps_buffer_size = PS_BUFFER_SIZE;
#else							/* PS_USE_CLOBBER_ARGV */
static char *ps_buffer;			/* 将指向 argv 区域 */
static size_t ps_buffer_size;	/* 空间在运行时确定 */
static size_t last_status_len;	/* 用于尽量减小覆盖长度 */
#endif							/* PS_USE_CLOBBER_ARGV */

static size_t ps_buffer_cur_len;	/* 标称 strlen(ps_buffer) */

static size_t ps_buffer_fixed_size; /* 常量前缀的大小 */

/*
 * 追加后缀之前 ps_buffer 的长度，若未设置后缀
 * 则为 0。
 */
static size_t ps_buffer_nosuffix_len;

static void flush_ps_display(void);

#endif							/* not PS_USE_NONE */

/* 在此保存原始 argv[] 位置 */
static int	save_argc;
static char **save_argv;


/*
 * 在启动早期调用它以保存原始的 argc/argv 值。
 * 必要时我们会复制原始 argv[] 数组以保存它，
 * 使其不被后续 ps_display 操作破坏。
 *
 * （原始 argv[] 不会被本例程覆盖，但可能在
 * init_ps_display 期间被覆盖。此外，环境字符串的
 * 物理位置可能被移动，所以应在任何可能试图
 * 留住 getenv() 结果的代码前调用。但参见针对 musl 的
 * hack。
 *
 * 注意失败时它无法调用 elog()，因为后者
 * 尚未初始化。我们改用 write_stderr()。
 */
char	  **
save_ps_display_args(int argc, char **argv)
{
	save_argc = argc;
	save_argv = argv;

#if defined(PS_USE_CLOBBER_ARGV)

	/*
	 * 如果打算改写 argv 区域，就统计可用空间。
	 * 同时移动环境字符串以腾出额外空间。
	 */
	{
		char	   *end_of_area = NULL;
		char	  **new_environ;
		int			i;

		/*
		 * 检查连续的 argv 字符串
		 */
		for (i = 0; i < argc; i++)
		{
			if (i == 0 || end_of_area + 1 == argv[i])
				end_of_area = argv[i] + strlen(argv[i]);
		}

		if (end_of_area == NULL)	/* 大概不会发生？ */
		{
			ps_buffer = NULL;
			ps_buffer_size = 0;
			return argv;
		}

		/*
		 * 检查 argv 之后连续的 environ 字符串
		 */
		for (i = 0; environ[i] != NULL; i++)
		{
			if (end_of_area + 1 == environ[i])
			{
				/*
				 * musl 动态链接器保存着一个静态指针，
				 * 指向 LD_LIBRARY_PATH 的初值，若该变量定义于
				 * 进程环境中。该设置的值必须保持不变，
				 * 因而 end_of_area 只能止步于它之前。
				 * musl 没有定义任何可识别的编译器符号，
				 * 所以除非见到标识已知安全 Linux libc 的
				 * 符号，否则就必须这么做。
				 */
#if defined(__linux__) && (!defined(__GLIBC__) && !defined(__UCLIBC__))
				if (strncmp(environ[i], "LD_LIBRARY_PATH=", 16) == 0)
				{
					/*
					 * 我们可以改写名称，但要在等号处停下。
					 * 后续循环迭代将找不到更多
					 * 连续空间，但我们不提前跳出，因为
					 * 我们需要统计 environ[] 条目的总数。
					 */
					end_of_area = environ[i] + 15;
				}
				else
#endif
				{
					end_of_area = environ[i] + strlen(environ[i]);
				}
			}
		}

		ps_buffer = argv[0];
		last_status_len = ps_buffer_size = end_of_area - argv[0];

		/*
		 * 把环境字符串移开
		 */
		new_environ = (char **) malloc((i + 1) * sizeof(char *));
		if (!new_environ)
		{
			write_stderr("out of memory\n");
			exit(1);
		}
		for (i = 0; environ[i] != NULL; i++)
		{
			new_environ[i] = strdup(environ[i]);
			if (!new_environ[i])
			{
				write_stderr("out of memory\n");
				exit(1);
			}
		}
		new_environ[i] = NULL;
		environ = new_environ;
	}

	/*
	 * 如果打算改动原始 argv[]，就为解析参数制作
	 * 一份副本。
	 *
	 * 注意：不要想着去掉对 argv[] 的复制，尽管
	 * postmaster.c 早在我们考虑改动 ps 显示之前就
	 * 已看完 argv[] 了。在某些平台上，getopt() 会保存
	 * 指向 argv 数组的指针，当它被重新调用以分析
	 * 子进程的参数串时，如果 argv 存储已被中途
	 * 破坏，它会变得一团糟。其他平台还有其他依赖
	 * argv[]。
	 */
	{
		char	  **new_argv;
		int			i;

		new_argv = (char **) malloc((argc + 1) * sizeof(char *));
		if (!new_argv)
		{
			write_stderr("out of memory\n");
			exit(1);
		}
		for (i = 0; i < argc; i++)
		{
			new_argv[i] = strdup(argv[i]);
			if (!new_argv[i])
			{
				write_stderr("out of memory\n");
				exit(1);
			}
		}
		new_argv[argc] = NULL;

#if defined(__darwin__)

		/*
		 * macOS 保存 argv 指针的静态副本，可用下法修复：
		 *
		 */
		*_NSGetArgv() = new_argv;
#endif

		argv = new_argv;
	}
#endif							/* PS_USE_CLOBBER_ARGV */

	return argv;
}

/*
 * 在子进程启动期间调用一次，以设置标识
 * 值。
 *
 * 若 fixed_part 为 NULL，则从 MyBackendType 获取默认值。
 *
 * 此时原始 argv[] 数组可能被覆盖。
 */
void
init_ps_display(const char *fixed_part)
{
#ifndef PS_USE_NONE
	bool		save_update_process_title;
#endif

	Assert(fixed_part || MyBackendType);
	if (!fixed_part)
		fixed_part = GetBackendTypeDesc(MyBackendType);

#ifndef PS_USE_NONE
	/* 独立后端无 ps 显示 */
	if (!IsUnderPostmaster)
		return;

	/* 若未调用 save_ps_display_args() 则无 ps 显示 */
	if (!save_argv)
		return;

#ifdef PS_USE_CLOBBER_ARGV
	/* 若 ps_buffer 是指针，它可能仍为空 */
	if (!ps_buffer)
		return;

	/* 多余 argv 槽指向 end_of_area（NUL） */
	for (int i = 1; i < save_argc; i++)
		save_argv[i] = ps_buffer + ps_buffer_size;
#endif							/* PS_USE_CLOBBER_ARGV */

	/*
	 * 构造 ps 显示固定前缀。
	 */

#if defined(PS_USE_SETPROCTITLE) || defined(PS_USE_SETPROCTITLE_FAST)

	/*
	 * 显然 setproctitle() 已在 ps 行前添加了 `progname:' 前缀
	 *
	 */
#define PROGRAM_NAME_PREFIX ""
#else
#define PROGRAM_NAME_PREFIX "postgres: "
#endif

	if (*cluster_name == '\0')
	{
		snprintf(ps_buffer, ps_buffer_size,
				 PROGRAM_NAME_PREFIX "%s ",
				 fixed_part);
	}
	else
	{
		snprintf(ps_buffer, ps_buffer_size,
				 PROGRAM_NAME_PREFIX "%s: %s ",
				 cluster_name, fixed_part);
	}

	ps_buffer_cur_len = ps_buffer_fixed_size = strlen(ps_buffer);

	/*
	 * 首次运行时强制更新。
	 */
	save_update_process_title = update_process_title;
	update_process_title = true;
	set_ps_display("");
	update_process_title = save_update_process_title;
#endif							/* not PS_USE_NONE */
}

#ifndef PS_USE_NONE
/*
 * update_ps_display_precheck
 *		辅助函数，用于判断我们是否需要更新
 *		进程标题。
 */
static bool
update_ps_display_precheck(void)
{
	/* update_process_title=off 禁用更新 */
	if (!update_process_title)
		return false;

	/* 独立后端无 ps 显示 */
	if (!IsUnderPostmaster)
		return false;

#ifdef PS_USE_CLOBBER_ARGV
	/* 若 ps_buffer 是指针，它可能仍为空 */
	if (!ps_buffer)
		return false;
#endif

	return true;
}
#endif							/* not PS_USE_NONE */

/*
 * set_ps_display_suffix
 *		调整进程标题，把 'suffix' 以空格分隔追加到末尾，
 *		使其与当前进程标题隔开。
 */
void
set_ps_display_suffix(const char *suffix)
{
#ifndef PS_USE_NONE
	size_t		len;

	/* 先检查是否需要更新进程标题 */
	if (!update_ps_display_precheck())
		return;

	/* 若已有后缀，则覆盖它 */
	if (ps_buffer_nosuffix_len > 0)
		ps_buffer_cur_len = ps_buffer_nosuffix_len;
	else
		ps_buffer_nosuffix_len = ps_buffer_cur_len;

	len = strlen(suffix);

	/* 检查是否有足够空间追加后缀 */
	if (ps_buffer_cur_len + len + 1 >= ps_buffer_size)
	{
		/* 空间不足。检查缓冲区是否已满 */
		if (ps_buffer_cur_len < ps_buffer_size - 1)
		{
			/* 在后缀前追加一个空格 */
			ps_buffer[ps_buffer_cur_len++] = ' ';

			/* 尽量追加并填满 ps_buffer */
			memcpy(ps_buffer + ps_buffer_cur_len, suffix,
				   ps_buffer_size - ps_buffer_cur_len - 1);
			ps_buffer[ps_buffer_size - 1] = '\0';
			ps_buffer_cur_len = ps_buffer_size - 1;
		}
	}
	else
	{
		ps_buffer[ps_buffer_cur_len++] = ' ';
		memcpy(ps_buffer + ps_buffer_cur_len, suffix, len + 1);
		ps_buffer_cur_len = ps_buffer_cur_len + len;
	}

	Assert(strlen(ps_buffer) == ps_buffer_cur_len);

	/* 并设置新标题 */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

/*
 * set_ps_display_remove_suffix
 *		移除由 set_ps_display_suffix 添加的进程显示后缀
 */
void
set_ps_display_remove_suffix(void)
{
#ifndef PS_USE_NONE
	/* 先检查是否需要更新进程标题 */
	if (!update_ps_display_precheck())
		return;

	/* 检查已添加后缀 */
	if (ps_buffer_nosuffix_len == 0)
		return;					/* 无后缀 */

	/* 从 ps_buffer 中移除后缀 */
	ps_buffer[ps_buffer_nosuffix_len] = '\0';
	ps_buffer_cur_len = ps_buffer_nosuffix_len;
	ps_buffer_nosuffix_len = 0;

	Assert(ps_buffer_cur_len == strlen(ps_buffer));

	/* 并设置新标题 */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

/*
 * 调用它把 ps 状态显示更新为固定前缀加上
 * 参数所传入的当前正在做什么的指示。
 *
 * 'len' 必须与 strlen(activity) 相同
 */
void
set_ps_display_with_len(const char *activity, size_t len)
{
	Assert(strlen(activity) == len);

#ifndef PS_USE_NONE
	/* 先检查是否需要更新进程标题 */
	if (!update_ps_display_precheck())
		return;

	/* 标题完全改变时清除任何后缀 */
	ps_buffer_nosuffix_len = 0;

	/* 更新 ps_buffer 以包含固定部分与活动 */
	if (ps_buffer_fixed_size + len >= ps_buffer_size)
	{
		/* 处理 ps_buffer 空间不足的情形 */
		memcpy(ps_buffer + ps_buffer_fixed_size, activity,
			   ps_buffer_size - ps_buffer_fixed_size - 1);
		ps_buffer[ps_buffer_size - 1] = '\0';
		ps_buffer_cur_len = ps_buffer_size - 1;
	}
	else
	{
		memcpy(ps_buffer + ps_buffer_fixed_size, activity, len + 1);
		ps_buffer_cur_len = ps_buffer_fixed_size + len;
	}
	Assert(strlen(ps_buffer) == ps_buffer_cur_len);

	/* 必要时把新设置传给内核 */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

#ifndef PS_USE_NONE
static void
flush_ps_display(void)
{
#ifdef PS_USE_SETPROCTITLE
	setproctitle("%s", ps_buffer);
#elif defined(PS_USE_SETPROCTITLE_FAST)
	setproctitle_fast("%s", ps_buffer);
#endif

#ifdef PS_USE_CLOBBER_ARGV
	/* 填充未用内存；只需覆盖旧状态字符串的剩余部分 */
	if (last_status_len > ps_buffer_cur_len)
		MemSet(ps_buffer + ps_buffer_cur_len, PS_PADDING,
			   last_status_len - ps_buffer_cur_len);
	last_status_len = ps_buffer_cur_len;
#endif							/* PS_USE_CLOBBER_ARGV */

#ifdef PS_USE_WIN32
	{
		/*
		 * Win32 不支持显示任何被改动的参数。为了至少
		 * 能追踪哪个后端在做什么，我们创建一个
		 * 可用 Process Explorer 之类工具查看的命名对象。
		 */
		static HANDLE ident_handle = INVALID_HANDLE_VALUE;
		char		name[PS_BUFFER_SIZE + 32];

		if (ident_handle != INVALID_HANDLE_VALUE)
			CloseHandle(ident_handle);

		sprintf(name, "pgident(%d): %s", MyProcPid, ps_buffer);

		ident_handle = CreateEvent(NULL, TRUE, FALSE, name);
	}
#endif							/* PS_USE_WIN32 */
}
#endif							/* not PS_USE_NONE */

/*
 * 返回 ps 显示中当前的内容，以备有人需要；
 * 注意只返回活动部分。在某些平台上，
 * 该字符串不会以空字符结尾，因此要把有效长度
 * 存入 *displen。
 */
const char *
get_ps_display(int *displen)
{
#ifdef PS_USE_CLOBBER_ARGV
	/* 若 ps_buffer 是指针，它可能仍为空 */
	if (!ps_buffer)
	{
		*displen = 0;
		return "";
	}
#endif

#ifndef PS_USE_NONE
	*displen = (int) (ps_buffer_cur_len - ps_buffer_fixed_size);

	return ps_buffer + ps_buffer_fixed_size;
#else
	*displen = 0;
	return "";
#endif
}
