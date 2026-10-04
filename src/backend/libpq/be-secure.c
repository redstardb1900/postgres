/*-------------------------------------------------------------------------
 *
 * be-secure.c
 *	  functions related to setting up a secure connection to the frontend.
 *	  Secure connections are expected to provide confidentiality,
 *	  message integrity and endpoint authentication.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

char	   *ssl_library;
char	   *ssl_cert_file;
char	   *ssl_key_file;
char	   *ssl_ca_file;
char	   *ssl_crl_file;
char	   *ssl_crl_dir;
char	   *ssl_dh_params_file;
char	   *ssl_passphrase_command;
bool		ssl_passphrase_command_supports_reload;

#ifdef USE_SSL
bool		ssl_loaded_verify_locations = false;
#endif

/* 控制 SSL 密码套件列表的 GUC 变量 */
char	   *SSLCipherSuites = NULL;

/* 默认 ECHD 曲线的 GUC 变量。 */
char	   *SSLECDHCurve;

/* GUC 变量：若为假，则优先使用客户端密码套件 */
bool		SSLPreferServerCiphers;

int			ssl_min_protocol_version = PG_TLS1_2_VERSION;
int			ssl_max_protocol_version = PG_TLS_ANY;

/* ------------------------------------------------------------ */
/*			 所有安全会话共用的过程			*/
/* ------------------------------------------------------------ */

/*
 *	初始化全局上下文。
 *
 * 若 isServerStart 为真，把任何错误报告为 FATAL（这样我们就
 * 不会返回）。否则以 LOG 级别记录错误并返回 -1 表示有麻烦，
 * 若已有旧 SSL 状态则保留它。成功则返回 0。
 */
int
secure_initialize(bool isServerStart)
{
#ifdef USE_SSL
	return be_tls_init(isServerStart);
#else
	return 0;
#endif
}

/*
 *	销毁全局上下文，若有的话。
 */
void
secure_destroy(void)
{
#ifdef USE_SSL
	be_tls_destroy();
#endif
}

/*
 * 表明我们是否已加载根 CA 存储以验证证书
 */
bool
secure_loaded_verify_locations(void)
{
#ifdef USE_SSL
	return ssl_loaded_verify_locations;
#else
	return false;
#endif
}

/*
 *	尝试协商安全会话。
 */
int
secure_open_server(Port *port)
{
	int			r = 0;

#ifdef USE_SSL
	r = be_tls_open_server(port);

	ereport(DEBUG2,
			(errmsg_internal("SSL connection from DN:\"%s\" CN:\"%s\"",
							 port->peer_dn ? port->peer_dn : "(anonymous)",
							 port->peer_cn ? port->peer_cn : "(anonymous)")));
#endif

	return r;
}

/*
 *	关闭安全会话。
 */
void
secure_close(Port *port)
{
#ifdef USE_SSL
	if (port->ssl_in_use)
		be_tls_close(port);
#endif
}

/*
 *	从安全连接读取数据。
 */
ssize_t
secure_read(Port *port, void *ptr, size_t len)
{
	ssize_t		n;
	int			waitfor;

	/* 处理任何已经待处理的中断条件。 */
	ProcessClientReadInterrupt(false);

retry:
#ifdef USE_SSL
	waitfor = 0;
	if (port->ssl_in_use)
	{
		n = be_tls_read(port, ptr, len, &waitfor);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (port->gss && port->gss->enc)
	{
		n = be_gssapi_read(port, ptr, len);
		waitfor = WL_SOCKET_READABLE;
	}
	else
#endif
	{
		n = secure_raw_read(port, ptr, len);
		waitfor = WL_SOCKET_READABLE;
	}

	/* 在阻塞模式下，等到套接字就绪 */
	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		WaitEvent	event;

		Assert(waitfor);

		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, waitfor, NULL);

		WaitEventSetWait(FeBeWaitSet, -1 /* 无超时 */ , &event, 1,
						 WAIT_EVENT_CLIENT_READ);

		/*
 * 若 postmaster 已死亡，继续运行就不安全，因为若其他后端
 * 不干净地退出，该由 postmaster 来杀掉我们。而且在这种
 * 状态下我们也跑不好；walwriter 和 bgwriter 这类辅助进程
 * 会退出，性能可能很差。最后，若我们不退出，不人工干预
 * pg_ctl 就无法重启 postmaster，从而无法接受新连接。退出
 * 是为 postmaster 重启清场。
 *
 * （注意，只在本来就要在锁存器上睡眠时才做这项检查。若
 * postmaster 在查询中途被杀掉，或者我们一直不用等待读取，
 * 甚至跨多条查询，我们仍可能继续跑一会儿。我们不想为这种
 * 极罕见的情况烧掉过多周期，而这样在多数情况下应能让我们
 * 很快退出。）
 *
 *
 *
 */
		if (event.events & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to unexpected postmaster exit")));

		/* 处理中断。 */
		if (event.events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessClientReadInterrupt(true);

			/*
 * 我们将重试这次读取。它很可能会立刻返回，因为仍然没有
 * 可用数据，然后我们再等待套接字再次就绪。
 *
 */
		}
		goto retry;
	}

	/*
 * 处理在一次成功（或非阻塞、或硬失败）的读取期间发生的
 * 中断。
 */
	ProcessClientReadInterrupt(false);

	return n;
}

ssize_t
secure_raw_read(Port *port, void *ptr, size_t len)
{
	ssize_t		n;

	/*
 * 尝试不阻塞地从套接字读取。若成功就完成了，否则用锁存器
 * 机制等待套接字。
 */
#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = recv(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	return n;
}


/*
 *	向安全连接写入数据。
 */
ssize_t
secure_write(Port *port, void *ptr, size_t len)
{
	ssize_t		n;
	int			waitfor;

	/* 处理任何已经待处理的中断条件。 */
	ProcessClientWriteInterrupt(false);

retry:
	waitfor = 0;
#ifdef USE_SSL
	if (port->ssl_in_use)
	{
		n = be_tls_write(port, ptr, len, &waitfor);
	}
	else
#endif
#ifdef ENABLE_GSS
	if (port->gss && port->gss->enc)
	{
		n = be_gssapi_write(port, ptr, len);
		waitfor = WL_SOCKET_WRITEABLE;
	}
	else
#endif
	{
		n = secure_raw_write(port, ptr, len);
		waitfor = WL_SOCKET_WRITEABLE;
	}

	if (n < 0 && !port->noblock && (errno == EWOULDBLOCK || errno == EAGAIN))
	{
		WaitEvent	event;

		Assert(waitfor);

		ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, waitfor, NULL);

		WaitEventSetWait(FeBeWaitSet, -1 /* 无超时 */ , &event, 1,
						 WAIT_EVENT_CLIENT_WRITE);

		/* 见 secure_read 中的注释。 */
		if (event.events & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to unexpected postmaster exit")));

		/* 处理中断。 */
		if (event.events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			ProcessClientWriteInterrupt(true);

			/*
 * 我们将重试这次写入。它很可能会立刻返回，因为仍然没有
 * 可用的缓冲区空间，然后我们再等待套接字再次就绪。
 *
 */
		}
		goto retry;
	}

	/*
 * 处理在一次成功（或非阻塞、或硬失败）的写入期间发生的
 * 中断。
 */
	ProcessClientWriteInterrupt(false);

	return n;
}

ssize_t
secure_raw_write(Port *port, const void *ptr, size_t len)
{
	ssize_t		n;

#ifdef WIN32
	pgwin32_noblock = true;
#endif
	n = send(port->sock, ptr, len, 0);
#ifdef WIN32
	pgwin32_noblock = false;
#endif

	return n;
}
