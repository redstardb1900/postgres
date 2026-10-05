/*-------------------------------------------------------------------------
 *
 * pqsignal.c
 *	  后端的 signal(2) 支持（另见 src/port/pqsignal.c）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/pqsignal.c
 *
 * ------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqsignal.h"


/* 全局变量 */
sigset_t	UnBlockSig,
			BlockSig,
			StartupBlockSig;


/*
 * 初始化 BlockSig、UnBlockSig 和 StartupBlockSig。
 *
 * BlockSig 是我们试图阻塞信号时要阻塞的信号集。它包括
 * 我们通常预期会收到的全部信号，但不包括绝不应该关掉的
 * 信号。
 *
 * StartupBlockSig 是收集启动包期间要阻塞的信号集；它
 * 基本上是 BlockSig 减去 SIGTERM、SIGQUIT、SIGALRM。
 *
 * UnBlockSig 是我们不想阻塞信号时要阻塞的信号集。
 *
 */
void
pqinitmask(void)
{
	sigemptyset(&UnBlockSig);

	/* 注意：InitializeLatchSupport() 会修改 UnBlockSig。 */

	/* 先设置全部信号，再清除其中一些。 */
	sigfillset(&BlockSig);
	sigfillset(&StartupBlockSig);

	/*
 * 取消标记那些绝不应该被阻塞的信号。其中有些信号名
 * 并非在所有平台上都存在。大多数是存在的，但为了一致
 * 不妨把它们全都放进 ifdef。
 */
#ifdef SIGTRAP
	sigdelset(&BlockSig, SIGTRAP);
	sigdelset(&StartupBlockSig, SIGTRAP);
#endif
#ifdef SIGABRT
	sigdelset(&BlockSig, SIGABRT);
	sigdelset(&StartupBlockSig, SIGABRT);
#endif
#ifdef SIGILL
	sigdelset(&BlockSig, SIGILL);
	sigdelset(&StartupBlockSig, SIGILL);
#endif
#ifdef SIGFPE
	sigdelset(&BlockSig, SIGFPE);
	sigdelset(&StartupBlockSig, SIGFPE);
#endif
#ifdef SIGSEGV
	sigdelset(&BlockSig, SIGSEGV);
	sigdelset(&StartupBlockSig, SIGSEGV);
#endif
#ifdef SIGBUS
	sigdelset(&BlockSig, SIGBUS);
	sigdelset(&StartupBlockSig, SIGBUS);
#endif
#ifdef SIGSYS
	sigdelset(&BlockSig, SIGSYS);
	sigdelset(&StartupBlockSig, SIGSYS);
#endif
#ifdef SIGCONT
	sigdelset(&BlockSig, SIGCONT);
	sigdelset(&StartupBlockSig, SIGCONT);
#endif

/* 启动阶段特有的信号 */
#ifdef SIGQUIT
	sigdelset(&StartupBlockSig, SIGQUIT);
#endif
#ifdef SIGTERM
	sigdelset(&StartupBlockSig, SIGTERM);
#endif
#ifdef SIGALRM
	sigdelset(&StartupBlockSig, SIGALRM);
#endif
}
