/*
 * Windows 下替代 <sys/resource.h>。
 */
#ifndef WIN32_SYS_RESOURCE_H
#define WIN32_SYS_RESOURCE_H

#include <sys/time.h>			/*  给 timeval  */

#define RUSAGE_SELF		0
#define RUSAGE_CHILDREN (-1)

struct rusage
{
	struct timeval ru_utime;	/*  用户时间  */
	struct timeval ru_stime;	/*  系统时间  */
};

extern int	getrusage(int who, struct rusage *rusage);

#endif							/* WIN32_SYS_RESOURCE_H */
