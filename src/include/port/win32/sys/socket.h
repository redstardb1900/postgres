/*
 * src/include/port/win32/sys/socket.h
 */
#ifndef WIN32_SYS_SOCKET_H
#define WIN32_SYS_SOCKET_H

/*
 * 不巧 VC++ 的 <wingdi.h> 也定义了 ERROR。
 * 为免冲突,在此含 <windows.h> 并立刻取消定义
 * ERROR。
 *
 * 注意:勿直接含 <wingdi.h>。会导致编译错误。
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#undef ERROR
#undef small

/*  恢复旧 ERROR 值  */
#ifdef PGERROR
#define ERROR PGERROR
#endif

/*
 * 不用 Windows gai_strerror[A],因其非线程安全。
 * 我们在 src/port/win32gai_strerror.c 自行定义。
 */
#undef gai_strerror

extern const char *gai_strerror(int ecode);

#endif							/* WIN32_SYS_SOCKET_H */
