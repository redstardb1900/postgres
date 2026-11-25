/*
 * src/interfaces/libpq/win32.h
 */
#ifndef __win32_h_included
#define __win32_h_included

/*
 * 一些兼容函数
 */

/* open 由别处提供 */
#define close(a) _close(a)
#define read(a,b,c) _read(a,b,c)
#define write(a,b,c) _write(a,b,c)

#undef EAGAIN					/* 不适用于套接字 */

/*
 * 处理 Windows 套接字错误的支持
 */
extern const char *winsock_strerror(int err, char *strerrbuf, size_t buflen);

#endif
