/*
 * src/include/port/win32/sys/un.h
 */
#ifndef WIN32_SYS_UN_H
#define WIN32_SYS_UN_H

/*
 * Windows 在 <afunix.h> 定义此结构,但并非所有工具链
 * 都已有该头,故暂在此定义。
 */
struct sockaddr_un
{
	unsigned short sun_family;
	char		sun_path[108];
};

#endif
