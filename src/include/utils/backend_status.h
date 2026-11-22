/* ----------
 * backend_status.h
 *	  与后端状态报告相关的定义
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/backend_status.h
 * ----------
 */
#ifndef BACKEND_STATUS_H
#define BACKEND_STATUS_H

#include "datatype/timestamp.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"			/* for BackendType */
#include "storage/backendid.h"
#include "utils/backend_progress.h"


/* ----------
 * 后端状态
 * ----------
 */
typedef enum BackendState
{
	STATE_UNDEFINED,
	STATE_IDLE,
	STATE_RUNNING,
	STATE_IDLEINTRANSACTION,
	STATE_FASTPATH,
	STATE_IDLEINTRANSACTION_ABORTED,
	STATE_DISABLED
} BackendState;


/* ----------
 * 共享内存数据结构
 * ----------
 */

/*
 * PgBackendSSLStatus
 *
 * 对每个后端，我们把 SSL 状态放在单独结构体中，
 * 仅在启用 SSL 时填充。
 *
 * 所有 char 数组必须空字符结尾.
 */
typedef struct PgBackendSSLStatus
{
	/*  关于 SSL 连接的信息  */
	int			ssl_bits;
	char		ssl_version[NAMEDATALEN];
	char		ssl_cipher[NAMEDATALEN];
	char		ssl_client_dn[NAMEDATALEN];

	/*
	 * serial number is max "20 octets" per RFC 5280, so this size should be
	 * fine
	 */
	char		ssl_client_serial[NAMEDATALEN];

	char		ssl_issuer_dn[NAMEDATALEN];
} PgBackendSSLStatus;

/*
 * PgBackendGSSStatus
 *
 * 对每个后端，我们把 GSS 状态放在单独结构体中，
 * 仅在启用 GSS 时填充。
 *
 * 所有 char 数组必须空字符结尾.
 */
typedef struct PgBackendGSSStatus
{
	/*  关于 GSSAPI 连接的信息  */
	char		gss_princ[NAMEDATALEN]; /*  认证用 GSSAPI 主体  */
	bool		gss_auth;		/*  若使用了 GSSAPI 认证  */
	bool		gss_enc;		/*  若正在使用加密  */
	bool		gss_delegation; /*  若委托了凭证  */

} PgBackendGSSStatus;


/* ----------
 * PgBackendStatus
 *
 * Each live backend maintains a PgBackendStatus struct in shared memory
 * showing its current activity.  (The structs are allocated according to
 * BackendId, but that is not critical.)  Note that this is unrelated to the
 * cumulative stats system (i.e. pgstat.c et al).
 *
 * Each auxiliary process also maintains a PgBackendStatus struct in shared
 * memory.
 * ----------
 */
typedef struct PgBackendStatus
{
	/*
	 * To avoid locking overhead, we use the following protocol: a backend
	 * increments st_changecount before modifying its entry, and again after
	 * finishing a modification.  A would-be reader should note the value of
	 * st_changecount, copy the entry into private memory, then check
	 * st_changecount again.  If the value hasn't changed, and if it's even,
	 * the copy is valid; otherwise start over.  This makes updates cheap
	 * while reads are potentially expensive, but that's the tradeoff we want.
	 *
	 * The above protocol needs memory barriers to ensure that the apparent
	 * order of execution is as it desires.  Otherwise, for example, the CPU
	 * might rearrange the code so that st_changecount is incremented twice
	 * before the modification on a machine with weak memory ordering.  Hence,
	 * use the macros defined below for manipulating st_changecount, rather
	 * than touching it directly.
	 */
	int			st_changecount;

	/*  条目在 st_procpid > 0 时有效，== 0 时未用  */
	int			st_procpid;

	/*  后端类型  */
	BackendType st_backendType;

	/*  当前后端、事务与活动开始的时刻  */
	TimestampTz st_proc_start_timestamp;
	TimestampTz st_xact_start_timestamp;
	TimestampTz st_activity_start_timestamp;
	TimestampTz st_state_start_timestamp;

	/*  数据库 OID、属主用户 OID、连接客户端地址  */
	Oid			st_databaseid;
	Oid			st_userid;
	SockAddr	st_clientaddr;
	char	   *st_clienthostname;	/*  必须空字符结尾  */

	/*  关于 SSL 连接的信息  */
	bool		st_ssl;
	PgBackendSSLStatus *st_sslstatus;

	/*  关于 GSSAPI 连接的信息  */
	bool		st_gss;
	PgBackendGSSStatus *st_gssstatus;

	/*  当前状态 */
	BackendState st_state;

	/*  应用名；必须空字符结尾  */
	char	   *st_appname;

	/*
	 * Current command string; MUST be null-terminated. Note that this string
	 * possibly is truncated in the middle of a multi-byte character. As
	 * activity strings are stored more frequently than read, that allows to
	 * move the cost of correct truncation to the display side. Use
	 * pgstat_clip_activity() to truncate correctly.
	 */
	char	   *st_activity_raw;

	/*
	 * Command progress reporting.  Any command which wishes can advertise
	 * that it is running by setting st_progress_command,
	 * st_progress_command_target, and st_progress_param[].
	 * st_progress_command_target should be the OID of the relation which the
	 * command targets (we assume there's just one, as this is meant for
	 * utility commands), but the meaning of each element in the
	 * st_progress_param array is command-specific.
	 */
	ProgressCommandType st_progress_command;
	Oid			st_progress_command_target;
	int64		st_progress_param[PGSTAT_NUM_PROGRESS_PARAM];

	/*  查询标识，可选用 post_parse_analyze_hook 计算  */
	uint64		st_query_id;
} PgBackendStatus;


/*
 * Macros to load and store st_changecount with appropriate memory barriers.
 *
 * Use PGSTAT_BEGIN_WRITE_ACTIVITY() before, and PGSTAT_END_WRITE_ACTIVITY()
 * after, modifying the current process's PgBackendStatus data.  Note that,
 * since there is no mechanism for cleaning up st_changecount after an error,
 * THESE MACROS FORM A CRITICAL SECTION.  Any error between them will be
 * promoted to PANIC, causing a database restart to clean up shared memory!
 * Hence, keep the critical section as short and straight-line as possible.
 * Aside from being safer, that minimizes the window in which readers will
 * have to loop.
 *
 * Reader logic should follow this sketch:
 *
 *		for (;;)
 *		{
 *			int before_ct, after_ct;
 *
 *			pgstat_begin_read_activity(beentry, before_ct);
 *			... copy beentry data to local memory ...
 *			pgstat_end_read_activity(beentry, after_ct);
 *			if (pgstat_read_activity_complete(before_ct, after_ct))
 *				break;
 *			CHECK_FOR_INTERRUPTS();
 *		}
 *
 * For extra safety, we generally use volatile beentry pointers, although
 * the memory barriers should theoretically be sufficient.
 */
#define PGSTAT_BEGIN_WRITE_ACTIVITY(beentry) \
	do { \
		START_CRIT_SECTION(); \
		(beentry)->st_changecount++; \
		pg_write_barrier(); \
	} while (0)

#define PGSTAT_END_WRITE_ACTIVITY(beentry) \
	do { \
		pg_write_barrier(); \
		(beentry)->st_changecount++; \
		Assert(((beentry)->st_changecount & 1) == 0); \
		END_CRIT_SECTION(); \
	} while (0)

#define pgstat_begin_read_activity(beentry, before_changecount) \
	do { \
		(before_changecount) = (beentry)->st_changecount; \
		pg_read_barrier(); \
	} while (0)

#define pgstat_end_read_activity(beentry, after_changecount) \
	do { \
		pg_read_barrier(); \
		(after_changecount) = (beentry)->st_changecount; \
	} while (0)

#define pgstat_read_activity_complete(before_changecount, after_changecount) \
	((before_changecount) == (after_changecount) && \
	 ((before_changecount) & 1) == 0)


/* ----------
 * LocalPgBackendStatus
 *
 * 构建后端状态数组时用 LocalPgBackendStatus，以便在需
 * 时向结构体加新值而无须给共享内存加新字段。其后端
 * 状态作为第一个成员。
 * ----------
 */
typedef struct LocalPgBackendStatus
{
	/*
	 * 后端状态条目的本地版本。
	 */
	PgBackendStatus backendStatus;

	/*
	 * 后端 ID。对辅助进程，该值会大于 MaxBackends（因辅助
	 * 进程没有正规后端 ID）。
	 * 见上。
	 */
	BackendId	backend_id;

	/*
	 * The xid of the current transaction if available, InvalidTransactionId
	 * if not.
	 */
	TransactionId backend_xid;

	/*
	 * The xmin of the current session if available, InvalidTransactionId if
	 * not.
	 */
	TransactionId backend_xmin;

	/*
	 * 当前会话中已缓存子事务的个数。
	 */
	int			backend_subxact_count;

	/*
	 * 当前会话中超出缓存子事务上限的
	 * 子事务个数。
	 */
	bool		backend_subxact_overflowed;
} LocalPgBackendStatus;


/* ----------
 * GUC 参数
 * ----------
 */
extern PGDLLIMPORT bool pgstat_track_activities;
extern PGDLLIMPORT int pgstat_track_activity_query_size;


/* ----------
 * 其他全局变量
 * ----------
 */
extern PGDLLIMPORT PgBackendStatus *MyBEEntry;


/* ----------
 * 由 postmaster 调用的函数
 * ----------
 */
extern Size BackendStatusShmemSize(void);
extern void CreateSharedBackendStatus(void);


/* ----------
 * 由后端调用的函数
 * ----------
 */

/*  初始化函数  */
extern void pgstat_beinit(void);
extern void pgstat_bestart(void);

extern void pgstat_clear_backend_activity_snapshot(void);

/*  活动报告函数  */
extern void pgstat_report_activity(BackendState state, const char *cmd_str);
extern void pgstat_report_query_id(uint64 query_id, bool force);
extern void pgstat_report_tempfile(size_t filesize);
extern void pgstat_report_appname(const char *appname);
extern void pgstat_report_xact_timestamp(TimestampTz tstamp);
extern const char *pgstat_get_backend_current_activity(int pid, bool checkUser);
extern const char *pgstat_get_crashed_backend_activity(int pid, char *buffer,
													   int buflen);
extern uint64 pgstat_get_my_query_id(void);


/* ----------
 * Support functions for the SQL-callable functions to
 * generate the pgstat* views.
 * ----------
 */
extern int	pgstat_fetch_stat_numbackends(void);
extern PgBackendStatus *pgstat_get_beentry_by_backend_id(BackendId beid);
extern LocalPgBackendStatus *pgstat_get_local_beentry_by_backend_id(BackendId beid);
extern LocalPgBackendStatus *pgstat_get_local_beentry_by_index(int idx);
extern char *pgstat_clip_activity(const char *raw_activity);


#endif							/* BACKEND_STATUS_H */
