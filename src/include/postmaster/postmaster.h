/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  导出自 postmaster/postmaster.c。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

/* GUC 选项 */
extern PGDLLIMPORT bool EnableSSL;
extern PGDLLIMPORT int SuperuserReservedConnections;
extern PGDLLIMPORT int ReservedConnections;
extern PGDLLIMPORT int PostPortNumber;
extern PGDLLIMPORT int Unix_socket_permissions;
extern PGDLLIMPORT char *Unix_socket_group;
extern PGDLLIMPORT char *Unix_socket_directories;
extern PGDLLIMPORT char *ListenAddresses;
extern PGDLLIMPORT bool ClientAuthInProgress;
extern PGDLLIMPORT int PreAuthDelay;
extern PGDLLIMPORT int AuthenticationTimeout;
extern PGDLLIMPORT bool Log_connections;
extern PGDLLIMPORT bool log_hostname;
extern PGDLLIMPORT bool enable_bonjour;
extern PGDLLIMPORT char *bonjour_name;
extern PGDLLIMPORT bool restart_after_crash;
extern PGDLLIMPORT bool remove_temp_files_after_crash;
extern PGDLLIMPORT bool send_abort_for_crash;
extern PGDLLIMPORT bool send_abort_for_kill;

#ifdef WIN32
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
extern PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * 表示 postmaster_alive_fds 中哪个由 postmaster
 * 持有、哪个由子进程用来检查 postmaster 死亡的常量。
 */
#define POSTMASTER_FD_WATCH		0	/* 子进程用来检查
									 * 死亡 */
#define POSTMASTER_FD_OWN		1	/* kept open by postmaster only */
#endif

extern PGDLLIMPORT const char *progname;

extern void PostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void ClosePostmasterPorts(bool am_syslogger);
extern void InitProcessGlobals(void);

extern int	MaxLivePostmasterChildren(void);

extern bool PostmasterMarkPIDForWorkerNotify(int);

#ifdef EXEC_BACKEND
extern pid_t postmaster_forkexec(int argc, char *argv[]);
extern void SubPostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();

extern Size ShmemBackendArraySize(void);
extern void ShmemBackendArrayAllocation(void);
#endif

/*
 * 注意：MAX_BACKENDS 限于 2^18-1，因 buf_internals.h
 * 为缓冲引用预留了该宽度。可用 64 位状态放宽，但
 * 不太值得，因 2^18-1 个后端已超出当前现实配置。
 * 即便去掉该限制，仍不能 a) 超过 2^23-1，因 inval.c
 * 把后端 ID 存为 3 字节有符号整数；b) 超过 INT_MAX/4，
 * 因有处无溢出检查地算 4*MaxBackends。相关 GUC 检查
 * 钩子与 RegisterBackgroundWorker() 中会再核验。
 * 。
 */
#define MAX_BACKENDS	0x3FFFF

#endif							/* _POSTMASTER_H */
