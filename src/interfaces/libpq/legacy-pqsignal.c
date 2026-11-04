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
 * 此版本的 pqsignal() 仅因为 9.3 之前的 libpq 版本   
 * 导出了 pqsignal()，而某些旧客户端程序仍依赖它  
 * 而存在。（自 9.3 起，客户端应改为从 libpgport    
 * 获取。）       
 *
 * 由于它仅用于向后兼容，                                    
 * 我们将其冻结在 9.2 的语义中；特别地，其           
 * SIGALRM 行为与 src/port/pqsignal.c 中的版本不同。  
 *
 * libpq 自身不用它，我们代码也没有其他地方使用。 
 *
 * src/include/port.h 将 #define pqsignal 定义为       
 * pqsignal_fe，但此处我们希望导出普通的 "pqsignal"。   
 * 我们也不能依赖 port.h 的外部声明。（该 #define  
 * 的目的是确保树内代码不会意外调用此版本。）    
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
