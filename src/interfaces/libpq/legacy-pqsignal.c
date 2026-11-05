/*-------------------------------------------------------------------------
 *
 * legacy-pqsignal.c
 *	  reliable BSD-style signal(2) routine stolen from RWW who stole it
 *	  from Stevens...
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/legacy-pqsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <signal.h>


/*
 * 此版本的 pqsignal() 存在仅是因为 9.3 之前的       
 * libpq 版本导出了 pqsignal()，而某些旧客户端        
 * 程序仍然依赖它。（9.3 起客户端应从 libpgport     
 * 获取。）       
 *
 * 由于它仅用于向后兼容，我们将其冻结在               
 * 9.2 时的语义；特别地，它对 SIGALRM 的行为与         
 * src/port/pqsignal.c 中的版本不同。                    
 *
 * libpq 自身并不使用它，我们代码中的其他             
 *
 * 地方也不用。src/include/port.h 会将 pqsignal    
 * #define 成 pqsignal_fe，但此处我们只想导出普通        
 * 的 "pqsignal"。我们也不能依赖 port.h 的 extern       
 * 声明。（该 #define 旨在防止树内代码误调它。）    
 */
#undef pqsignal
extern pqsigfunc pqsignal(int signo, pqsigfunc func);

pqsigfunc
pqsignal(int signo, pqsigfunc func)
{
#ifndef WIN32
	struct sigaction act,
				oact;

	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (signo != SIGALRM)
		act.sa_flags |= SA_RESTART;
#ifdef SA_NOCLDSTOP
	if (signo == SIGCHLD)
		act.sa_flags |= SA_NOCLDSTOP;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else							/* WIN32 */
	return signal(signo, func);
#endif
}
