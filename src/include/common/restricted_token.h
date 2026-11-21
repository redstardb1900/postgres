/*
 *	restricted_token.h
 *		确保 Windows 上使用受限令牌的辅助例程
 *
 *	Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/restricted_token.h
 */
#ifndef COMMON_RESTRICTED_TOKEN_H
#define COMMON_RESTRICTED_TOKEN_H

/*
 * 在 Windows 上确保以受限令牌运行；
 * 其他平台无操作。
 */
void		get_restricted_token(void);

#ifdef WIN32
/*  创建受限令牌并用它执行指定进程。  */
HANDLE		CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo);
#endif

#endif							/* COMMON_RESTRICTED_TOKEN_H */
