/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  断言支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/assert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

/*
 * ExceptionalCondition - 处理 Assert() 失败
 *
 * 我们有意不经由 elog()，理由是想把
 * 报告断言失败时必须正常工作的基础设施的数量
 * 降到最低。
 */
void
ExceptionalCondition(const char *conditionName,
					 const char *fileName,
					 int lineNumber)
{
	/* 把失败报告到 stderr（或本地等价物） */
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName))
		write_stderr("TRAP: ExceptionalCondition: bad arguments in PID %d\n",
					 (int) getpid());
	else
		write_stderr("TRAP: failed Assert(\"%s\"), File: \"%s\", Line: %d, PID: %d\n",
					 conditionName, fileName, lineNumber, (int) getpid());

	/* 通常这并不需要，但要确保消息确实已输出 */
	fflush(stderr);

	/* 若支持，则转储一个简单的回溯 */
#ifdef HAVE_BACKTRACE_SYMBOLS
	{
		void	   *buf[100];
		int			nframes;

		nframes = backtrace(buf, lengthof(buf));
		backtrace_symbols_fd(buf, nframes, fileno(stderr));
	}
#endif

	/*
	 * 若配置为这样做，则无限期休眠，以便用户能附加
	 * 调试器。这里若能用 pg_usleep() 更好，但它最多休眠
	 * 2G 微秒即约 33 分钟，似乎太短。
	 */
#ifdef SLEEP_ON_ASSERT
	sleep(1000000);
#endif

	abort();
}
