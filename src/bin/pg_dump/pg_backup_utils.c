/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.c
 *	Utility routines shared by pg_dump and pg_restore
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "parallel.h"
#include "pg_backup_utils.h"

/* 本文件导出的全局变量 */
const char *progname = NULL;

#define MAX_ON_EXIT_NICELY				20

static struct
{
	on_exit_nicely_callback function;
	void	   *arg;
}			on_exit_nicely_list[MAX_ON_EXIT_NICELY];

static int	on_exit_nicely_index;

/*
 * 解析 --section=foo 命令行参数。
 *
 * 根据 arg 设置或更新 *dumpSections 中的位掩码。
 * dumpSections 由 pg_dump 和 pg_restore 初始化为 DUMP_UNSECTIONED，
 * 这样它们可以知道该函数是否曾被调用过。
 */
void
set_dump_section(const char *arg, int *dumpSections)
{
	/* 如果是首次调用，清除所有位 */
	if (*dumpSections == DUMP_UNSECTIONED)
		*dumpSections = 0;

	if (strcmp(arg, "pre-data") == 0)
		*dumpSections |= DUMP_PRE_DATA;
	else if (strcmp(arg, "data") == 0)
		*dumpSections |= DUMP_DATA;
	else if (strcmp(arg, "post-data") == 0)
		*dumpSections |= DUMP_POST_DATA;
	else
	{
		pg_log_error("unrecognized section name: \"%s\"", arg);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}
}


/* 注册一个回调，该回调将在 exit_nicely 被调用时执行。 */
void
on_exit_nicely(on_exit_nicely_callback function, void *arg)
{
	if (on_exit_nicely_index >= MAX_ON_EXIT_NICELY)
		pg_fatal("out of on_exit_nicely slots");
	on_exit_nicely_list[on_exit_nicely_index].function = function;
	on_exit_nicely_list[on_exit_nicely_index].arg = arg;
	on_exit_nicely_index++;
}

/*
 * 按逆序运行累积的 on_exit_nicely 回调，然后退出，
 * 不打印任何消息。
 *
 * 如果在 Windows 上运行于并行工作线程中，我们仅退出线程，
 * 而非整个进程。
 *
 * 注意：在 Windows 上的并行操作中，回调将由每个线程运行，
 * 因为列表状态必然由所有线程共享；每个回调必须包含逻辑，
 * 以确保它仅对其线程执行适当的操作。在 Unix 上，回调也由
 * 每个进程运行，但仅限于我们在派生子进程之前建立的那些回调。
 * （更干净的做法是在 fork() 之后重置列表，
 * 并让每个子进程建立自己的回调；
 * 但那样的话，Windows 和 Unix 之间的行为将完全不一致。
 * 目前，只需确保在派生之前建立回调，
 * 以避免不一致。）
 */
void
exit_nicely(int code)
{
	int			i;

	for (i = on_exit_nicely_index - 1; i >= 0; i--)
		on_exit_nicely_list[i].function(code,
										on_exit_nicely_list[i].arg);

#ifdef WIN32
	if (parallel_init_done && GetCurrentThreadId() != mainThreadId)
		_endthreadex(code);
#endif

	exit(code);
}
