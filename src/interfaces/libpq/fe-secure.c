/*-------------------------------------------------------------------------
 *
 * fe-secure.c
 *	  functions related to setting up a secure connection to the backend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>

#ifdef WIN32
#include "win32.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include <sys/stat.h>

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#endif

#include "fe-auth.h"
#include "libpq-fe.h"
#include "libpq-int.h"

/*用于禁用然后恢复 SIGPIPE 处理状态的宏。
 * 在 Windows 上，这些都是空操作，因为不存在 SIGPIPE。
 * 
              */

#ifndef WIN32

#define SIGPIPE_MASKED(conn)	((conn)->sigpipe_so || (conn)->sigpipe_flag)

#ifdef ENABLE_THREAD_SAFETY

struct sigpipe_info
{
	sigset_t	oldsigmask;
	bool		sigpipe_pending;
	bool		got_epipe;
};

#define DECLARE_SIGPIPE_INFO(spinfo) struct sigpipe_info spinfo

#define DISABLE_SIGPIPE(conn, spinfo, failaction) \
	do { \
		(spinfo).got_epipe = false; \
		if (!SIGPIPE_MASKED(conn)) \
		{ \
			if (pq_block_sigpipe(&(spinfo).oldsigmask, \
								 &(spinfo).sigpipe_pending) < 0) \
				failaction; \
		} \
	} while (0)

#define REMEMBER_EPIPE(spinfo, cond) \
	do { \
		if (cond) \
			(spinfo).got_epipe = true; \
	} while (0)

#define RESTORE_SIGPIPE(conn, spinfo) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			pq_reset_sigpipe(&(spinfo).oldsigmask, (spinfo).sigpipe_pending, \
							 (spinfo).got_epipe); \
	} while (0)
#else							/* !ENABLE_THREAD_SAFETY */

#define DECLARE_SIGPIPE_INFO(spinfo) pqsigfunc spinfo = NULL

#define DISABLE_SIGPIPE(conn, spinfo, failaction) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			spinfo = pqsignal(SIGPIPE, SIG_IGN); \
	} while (0)

#define REMEMBER_EPIPE(spinfo, cond)

#define RESTORE_SIGPIPE(conn, spinfo) \
	do { \
		if (!SIGPIPE_MASKED(conn)) \
			pqsignal(SIGPIPE, spinfo); \
	} while (0)
#endif							/* ENABLE_THREAD_SAFETY */
#else							/* WIN32 */

#define DECLARE_SIGPIPE_INFO(spinfo)
#define DISABLE_SIGPIPE(conn, spinfo, failaction)
#define REMEMBER_EPIPE(spinfo, cond)
#define RESTORE_SIGPIPE(conn, spinfo)
#endif							/* WIN32 */

/* ------------------------------------------------------------ */
/*    所有安全会话通用的过程          */
/* ------------------------------------------------------------ */


int
PQsslInUse(PGconn *conn)
{
	if (!conn)
		return 0;
	return conn->ssl_in_use;
}

/*导出的函数，允许应用程序告诉我们它已经初始化了 OpenSSL。
 *	
 *	
   */
void
PQinitSSL(int do_init)
{
#ifdef USE_SSL
	pgtls_init_library(do_init, do_init);
#endif
}

/*导出的函数，允许应用程序告诉我们它已经初始化了 OpenSSL 和/或 libcrypto。
 *	
 *	
  */
void
PQinitOpenSSL(int do_ssl, int do_crypto)
{
#ifdef USE_SSL
	pgtls_init_library(do_ssl, do_crypto);
#endif
}

/*初始化全局 SSL 上下文
 *	
 */
int
pqsecure_initialize(PGconn *conn, bool do_ssl, bool do_crypto)
{
	int			r = 0;

#ifdef USE_SSL
	r = pgtls_init(conn, do_ssl, do_crypto);
#endif

	return r;
}

/*开始或继续协商安全会话。
 *	
            */
PostgresPollingStatusType
pqsecure_open_client(PGconn *conn)
{
#ifdef USE_SSL
	return pgtls_open_client(conn);
#else
	/* 不应到达此处 */
	return PGRES_POLLING_FAILED;
#endif
}

/*关闭安全会话。
 *	
 */
void
pqsecure_close(PGconn *conn)
{
#ifdef USE_SSL
	pgtls_close(conn);
#endif
}

/*从安全连接读取数据。
 *	
 *失败时，此函数负责向 conn->errorMessage 追加适当的消息。
 * 调用者仍必须检查 errno，但仅用于确定出错后是继续还是重试。
 * 
 * 
                                         */
ssize_t
pqsecure_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (conn->ssl_in_use)
	{
		n = pgtls_read(conn, ptr, len);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (conn->gssenc)
	{
		n = pg_GSS_read(conn, ptr, len);
	}
	else
#endif
	{
		n = pqsecure_raw_read(conn, ptr, len);
	}

	return n;
}

ssize_t
pqsecure_raw_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];

	SOCK_ERRNO_SET(0);

	n = recv(conn->sock, ptr, len, 0);

	if (n < 0)
	{
		result_errno = SOCK_ERRNO;

		/* 在合适时设置错误消息   */
		switch (result_errno)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				/* 无错误消息，调用者应重试          */
				break;

			case EPIPE:
			case ECONNRESET:
				libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
										"\tThis probably means the server terminated abnormally\n"
										"\tbefore or while processing the request.");
				break;

			case 0:
				/* 如果 errno 未设置，将其视为常规 EOF  */
				n = 0;
				break;

			default:
				libpq_append_conn_error(conn, "could not receive data from server: %s",
										SOCK_STRERROR(result_errno,
													  sebuf, sizeof(sebuf)));
				break;
		}
	}

	/* 确保将预期的 errno 返回给调用者   */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/*向安全连接写入数据。
 *	
 *返回写入的字节数，失败时返回负值（并设置 errno）。
 * 写入计数可能少于请求的数量。
 * 
 *注意套接字级别的硬故障对调用者隐藏，而是设置
 * conn->write_failed 并将错误消息存储在 conn->write_err_msg
 * 中；参见 pqsecure_raw_write。这允许我们推迟报告写入失败，
 * 直到我们确定服务器没有可用的错误消息。
 * 
 * 然而，在 SSL 或 GSS 管理层级检测到的错误通过负结果报告，
 *消息追加到 conn->errorMessage。通常不清楚这些错误应被视
 * 为读错误还是写错误，因此我们不尝试推迟报告它们。
 * 
 * 调用者仍必须在失败时检查 errno，但仅用于确定是继续还是
 * 重试；无论如何消息已保存在某处。
 *
 * 
 * 
                                                                                                                      */
ssize_t
pqsecure_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;

#ifdef USE_SSL
	if (conn->ssl_in_use)
	{
		n = pgtls_write(conn, ptr, len);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (conn->gssenc)
	{
		n = pg_GSS_write(conn, ptr, len);
	}
	else
#endif
	{
		n = pqsecure_raw_write(conn, ptr, len);
	}

	return n;
}

/*pqsecure_write 的低级实现。
 * 
 *对于未加密连接直接使用。对于加密连接，它代表 pgtls_write
 * 或 pg_GSS_write 执行物理 I/O。
 * 
 * 此函数仅对可重试错误（如 EINTR）报告失败（即返回负值）。
 *此类情况的循环应在某个外部层级处理，可能一直到应用程序。
 * 对于硬故障，我们设置 conn->write_failed 并在
 * conn->write_err_msg 中存储错误消息，但随后声称已完成写入。
 * 这是因为只要有可能从服务器读取并获得解释连接为何断开的
 * 错误消息，我们就不想报告写入失败。许多 TCP 栈存在竞争
 * 条件，使得写入失败可能在所有传入数据被读取之前或之后
 * 才被报告。
 * 
 * 注意使用 SSL 时，此错误行为发生在 SSL 管理层级之下。这是
 * 因为至少某些版本的 OpenSSL 在仍有可能从服务器获取更有用
 * 的错误时过于迅速地报告写入失败。
 *
 * 
 * 
 * 
 * 
                                                                                                                                                                                                                  */
ssize_t
pqsecure_raw_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			flags = 0;
	int			result_errno = 0;
	char		msgbuf[1024];
	char		sebuf[PG_STRERROR_R_BUFLEN];

	DECLARE_SIGPIPE_INFO(spinfo);

	/*如果已经发生写入失败，我们永远不会再次尝试在该连接上发送
	 * 数据。即使内核允许，我们也可能已经与服务器失去了消息边界
	 * 同步。因此 conn->write_failed 持续到连接重置，我们只是
	 * 丢弃所有要写入的数据。
	 * 
	 * 
	                                 */
	if (conn->write_failed)
		return len;

#ifdef MSG_NOSIGNAL
	if (conn->sigpipe_flag)
		flags |= MSG_NOSIGNAL;

retry_masked:
#endif							/* MSG_NOSIGNAL */

	DISABLE_SIGPIPE(conn, spinfo, return -1);

	n = send(conn->sock, ptr, len, flags);

	if (n < 0)
	{
		result_errno = SOCK_ERRNO;

		/*如果看到 EINVAL，可能是因为此机器上 MSG_NOSIGNAL 不可用。
	因此清除 sigpipe_flag 以便不再尝试该标志，然后重试
	send()。
	
	                      */
#ifdef MSG_NOSIGNAL
		if (flags != 0 && result_errno == EINVAL)
		{
			conn->sigpipe_flag = false;
			flags = 0;
			goto retry_masked;
		}
#endif							/* MSG_NOSIGNAL */

		/* 在合适时设置错误消息   */
		switch (result_errno)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				/* 无错误消息，调用者应重试          */
				break;

			case EPIPE:
				/* 设置EPIPE标志  */
				REMEMBER_EPIPE(spinfo, true);

				/* FALL THRU */

			case ECONNRESET:
				conn->write_failed = true;
				/* 可能时将错误消息存入 conn->write_err_msg      */
				/* strdup 失败也没关系，稍后处理 */
				snprintf(msgbuf, sizeof(msgbuf),
						 libpq_gettext("server closed the connection unexpectedly\n"
									   "\tThis probably means the server terminated abnormally\n"
									   "\tbefore or while processing the request."));
				/* 换行符不放进翻译字符串     */
				strlcat(msgbuf, "\n", sizeof(msgbuf));
				conn->write_err_msg = strdup(msgbuf);
				/* 现在声称写入成功      */
				n = len;
				break;

			default:
				conn->write_failed = true;
				/* 可能时将错误消息存入 conn->write_err_msg      */
				/* strdup 失败也没关系，稍后处理 */
				snprintf(msgbuf, sizeof(msgbuf),
						 libpq_gettext("could not send data to server: %s"),
						 SOCK_STRERROR(result_errno,
									   sebuf, sizeof(sebuf)));
				/* 换行符不放进翻译字符串     */
				strlcat(msgbuf, "\n", sizeof(msgbuf));
				conn->write_err_msg = strdup(msgbuf);
				/* 现在声称写入成功      */
				n = len;
				break;
		}
	}

	RESTORE_SIGPIPE(conn, spinfo);

	/* 确保将预期的 errno 返回给调用者   */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

/* 无 SSL 支持构建时的 SSL 信息函数桩版本                 */
#ifndef USE_SSL

void *
PQgetssl(PGconn *conn)
{
	return NULL;
}

void *
PQsslStruct(PGconn *conn, const char *struct_name)
{
	return NULL;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
	return NULL;
}

const char *const *
PQsslAttributeNames(PGconn *conn)
{
	static const char *const result[] = {NULL};

	return result;
}
#endif							/* USE_SSL */

/*无 OpenSSL 构建时的 OpenSSL 密钥密码钩子函数桩版本。
 * 
 * 
             */
#ifndef USE_OPENSSL

PQsslKeyPassHook_OpenSSL_type
PQgetSSLKeyPassHook_OpenSSL(void)
{
	return NULL;
}

void
PQsetSSLKeyPassHook_OpenSSL(PQsslKeyPassHook_OpenSSL_type hook)
{
	return;
}

int
PQdefaultSSLKeyPassHook_OpenSSL(char *buf, int size, PGconn *conn)
{
	return 0;
}
#endif							/* USE_OPENSSL */

/* 无 GSS 支持构建时的 GSSAPI 信息函数桩版本                       */
#ifndef ENABLE_GSS

void *
PQgetgssctx(PGconn *conn)
{
	return NULL;
}

int
PQgssEncInUse(PGconn *conn)
{
	return 0;
}

#endif							/* ENABLE_GSS */


#if defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)

/*为此线程阻塞 SIGPIPE。这可以防止 send()/write() 退出应用程序。
 *	
 *	
         */
int
pq_block_sigpipe(sigset_t *osigset, bool *sigpipe_pending)
{
	sigset_t	sigpipe_sigset;
	sigset_t	sigset;

	sigemptyset(&sigpipe_sigset);
	sigaddset(&sigpipe_sigset, SIGPIPE);

	/* 阻塞 SIGPIPE 并保存原掩码供稍后恢复     */
	SOCK_ERRNO_SET(pthread_sigmask(SIG_BLOCK, &sigpipe_sigset, osigset));
	if (SOCK_ERRNO)
		return -1;

	/* 只有之前被阻塞时，才可能有待处理的 SIGPIPE */
	if (sigismember(osigset, SIGPIPE))
	{
		/* 有待处理的 SIGPIPE？  */
		if (sigpending(&sigset) != 0)
			return -1;

		if (sigismember(&sigset, SIGPIPE))
			*sigpipe_pending = true;
		else
			*sigpipe_pending = false;
	}
	else
		*sigpipe_pending = false;

	return 0;
}

/*丢弃任何待处理的 SIGPIPE 并重置信号掩码。
 *	
 *注意：我们实际上假设 C 库不会排队多个 SIGPIPE 事件。如果
 * 它会，那么当事件已待处理而我们又收到另一个时，我们会意外
 * 地将我们的事件留在队列中。只要它不排队多个事件就没问题，
 * 因为调用者无法区分。
 * 
 * 如果调用者确定没有收到 EPIPE 错误，应设 got_epipe = false；
 *在这种情况下，我们将跳过清除操作，一切都肯定没问题，无论
 * 是否排队。如果收到或可能收到，则传递 got_epipe = true。
 * 
 * 我们不希望这改变 errno，因为如果改变了，可能丢失之前
 * send() 的错误码。我们基本上假设如果能够执行
 *pq_block_sigpipe()，这就不会失败。
 * 
 * 
 * 
                                                    */
void
pq_reset_sigpipe(sigset_t *osigset, bool sigpipe_pending, bool got_epipe)
{
	int			save_errno = SOCK_ERRNO;
	int			signo;
	sigset_t	sigset;

	/* 无待处理 SIGPIPE 时才清除      */
	if (got_epipe && !sigpipe_pending)
	{
		if (sigpending(&sigset) == 0 &&
			sigismember(&sigset, SIGPIPE))
		{
			sigset_t	sigpipe_sigset;

			sigemptyset(&sigpipe_sigset);
			sigaddset(&sigpipe_sigset, SIGPIPE);

			sigwait(&sigpipe_sigset, &signo);
		}
	}

	/* 恢复先前阻塞掩码 */
	pthread_sigmask(SIG_SETMASK, osigset, NULL);

	SOCK_ERRNO_SET(save_errno);
}

#endif							/* ENABLE_THREAD_SAFETY && !WIN32 */
