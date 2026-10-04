/*-------------------------------------------------------------------------
 *
 * pqcomm.c
 *	  Communication functions between the Frontend and the Backend
 *
 * These routines handle the low-level details of communication between
 * frontend and backend.  They just shove data across the communication
 * channel, and are ignorant of the semantics of the data.
 *
 * To emit an outgoing message, use the routines in pqformat.c to construct
 * the message in a buffer and then emit it in one call to pq_putmessage.
 * There are no functions to send raw bytes or partial messages; this
 * ensures that the channel will not be clogged by an incomplete message if
 * execution is aborted by ereport(ERROR) partway through the message.
 *
 * At one time, libpq was shared between frontend and backend, but now
 * the backend's "backend/libpq" is quite separate from "interfaces/libpq".
 * All that remains is similarities of names to trap the unwary...
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/libpq/pqcomm.c
 *
 *-------------------------------------------------------------------------
 */

/*------------------------
 * INTERFACE ROUTINES
 *
 * setup/teardown:
 *		StreamServerPort	- Open postmaster's server port
 *		StreamConnection	- Create new connection with client
 *		StreamClose			- Close a client/backend connection
 *		TouchSocketFiles	- Protect socket files against /tmp cleaners
 *		pq_init			- initialize libpq at backend startup
 *		socket_comm_reset	- reset libpq during error recovery
 *		socket_close		- shutdown libpq at backend exit
 *
 * low-level I/O:
 *		pq_getbytes		- get a known number of bytes from connection
 *		pq_getmessage	- get a message with length word from connection
 *		pq_getbyte		- get next byte from connection
 *		pq_peekbyte		- peek at next byte from connection
 *		pq_flush		- flush pending output
 *		pq_flush_if_writable - flush pending output if writable without blocking
 *		pq_getbyte_if_available - get a byte if available without blocking
 *
 * message-level I/O
 *		pq_putmessage	- send a normal message (suppressed in COPY OUT mode)
 *		pq_putmessage_noblock - buffer a normal message (suppressed in COPY OUT)
 *
 *------------------------
 */
#include "postgres.h"

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#include <signal.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <utime.h>
#ifdef WIN32
#include <mstcpip.h>
#endif

#include "common/ip.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/ipc.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"

/*
 * 应付各种平台特定的 TCP keepalive 套接字
 * 选项拼写。这不包括 Windows，它照例自行其是。
 */
#if defined(TCP_KEEPIDLE)
/* TCP_KEEPIDLE 是此选项在 Linux 和 *BSD 上的名称 */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPIDLE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPIDLE"
#elif defined(TCP_KEEPALIVE_THRESHOLD)
/* TCP_KEEPALIVE_THRESHOLD 是此选项在 Solaris >= 11 上的名称 */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE_THRESHOLD
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE_THRESHOLD"
#elif defined(TCP_KEEPALIVE) && defined(__darwin__)
/* TCP_KEEPALIVE 是此选项在 macOS 上的名称 */
/* 注意：Solaris 有这个符号，但含义不同 */
#define PG_TCP_KEEPALIVE_IDLE TCP_KEEPALIVE
#define PG_TCP_KEEPALIVE_IDLE_STR "TCP_KEEPALIVE"
#endif

/*
 * 配置选项
 */
int			Unix_socket_permissions;
char	   *Unix_socket_group;

/* Unix 套接字文件所在位置（palloc 出来的字符串列表） */
static List *sock_paths = NIL;

/*
 * 低层 I/O 用的缓冲区。
 *
 * 接收缓冲区大小固定。发送缓冲区通常是 8k，但若消息放不下，
 * 可以由 pq_putmessage_noblock() 加大。
 */

#define PQ_SEND_BUFFER_SIZE 8192
#define PQ_RECV_BUFFER_SIZE 8192

static char *PqSendBuffer;
static int	PqSendBufferSize;	/* 发送缓冲区的大小 */
static int	PqSendPointer;		/* 在 PqSendBuffer 中存入下一个字节的下标 */
static int	PqSendStart;		/* 在 PqSendBuffer 中发送下一个字节的下标 */

static char PqRecvBuffer[PQ_RECV_BUFFER_SIZE];
static int	PqRecvPointer;		/* 从 PqRecvBuffer 读取下一个字节的下标 */
static int	PqRecvLength;		/* PqRecvBuffer 中可用数据的末尾 */

/*
 * 消息状态
 */
static bool PqCommBusy;			/* 正忙着向客户端发送数据 */
static bool PqCommReadingMsg;	/* 正在读取一条消息的中途 */


/* 内部函数 */
static void socket_comm_reset(void);
static void socket_close(int code, Datum arg);
static void socket_set_nonblocking(bool nonblocking);
static int	socket_flush(void);
static int	socket_flush_if_writable(void);
static bool socket_is_send_pending(void);
static int	socket_putmessage(char msgtype, const char *s, size_t len);
static void socket_putmessage_noblock(char msgtype, const char *s, size_t len);
static int	internal_putbytes(const char *s, size_t len);
static int	internal_flush(void);

static int	Lock_AF_UNIX(const char *unixSocketDir, const char *unixSocketPath);
static int	Setup_AF_UNIX(const char *sock_path);

static const PQcommMethods PqCommSocketMethods = {
	socket_comm_reset,
	socket_flush,
	socket_flush_if_writable,
	socket_is_send_pending,
	socket_putmessage,
	socket_putmessage_noblock
};

const PQcommMethods *PqCommMethods = &PqCommSocketMethods;

WaitEventSet *FeBeWaitSet;


/* --------------------------------
 *		pq_init - 在后端启动时初始化 libpq
 * --------------------------------
 */
void
pq_init(void)
{
	int			socket_pos PG_USED_FOR_ASSERTS_ONLY;
	int			latch_pos PG_USED_FOR_ASSERTS_ONLY;

	/* 初始化状态变量 */
	PqSendBufferSize = PQ_SEND_BUFFER_SIZE;
	PqSendBuffer = MemoryContextAlloc(TopMemoryContext, PqSendBufferSize);
	PqSendPointer = PqSendStart = PqRecvPointer = PqRecvLength = 0;
	PqCommBusy = false;
	PqCommReadingMsg = false;

	/* 设置进程退出钩子以关闭套接字 */
	on_proc_exit(socket_close, 0);

	/*
	 * 在后端中（一旦 fork 出来），我们以
	 * 非阻塞模式操作底层套接字，并在需要时用锁存器
	 * 实现阻塞语义。这样就能提供可安全中断的读取和
	 * 写入。
	 */
#ifndef WIN32
	if (!pg_set_noblock(MyProcPort->sock))
		ereport(FATAL,
				(errmsg("could not set socket to nonblocking mode: %m")));
#endif

#ifndef WIN32

	/* 不要把套接字交给我们执行的任何子程序。 */
	if (fcntl(MyProcPort->sock, F_SETFD, FD_CLOEXEC) < 0)
		elog(FATAL, "fcntl(F_SETFD) failed on socket: %m");
#endif

	FeBeWaitSet = CreateWaitEventSet(TopMemoryContext, FeBeWaitSetNEvents);
	socket_pos = AddWaitEventToSet(FeBeWaitSet, WL_SOCKET_WRITEABLE,
								   MyProcPort->sock, NULL, NULL);
	latch_pos = AddWaitEventToSet(FeBeWaitSet, WL_LATCH_SET, PGINVALID_SOCKET,
								  MyLatch, NULL);
	AddWaitEventToSet(FeBeWaitSet, WL_POSTMASTER_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);

	/*
	 * 事件位置与我们添加它们的顺序一致，但还是做一次健全性
	 * 检查以确认。
	 */
	Assert(socket_pos == FeBeWaitSetSocketPos);
	Assert(latch_pos == FeBeWaitSetLatchPos);
}

/* --------------------------------
 *		socket_comm_reset - 在错误恢复期间重置 libpq
 *
 * 这在外层空闲循环的错误恢复中调用。只是为了
 * 在我们不知怎么从 pqcomm.c 例程内部 elog() 时
 * 让我们脱身（理想情况下永不发生，但是……）
 * --------------------------------
 */
static void
socket_comm_reset(void)
{
	/* 不要丢弃待处理数据，但要重置忙标志 */
	PqCommBusy = false;
}

/* --------------------------------
 *		socket_close - 在后端退出时关闭 libpq
 *
 * 这是 BackendInitialize() 期间就位的那个 pg_on_exit_callback。
 * 该函数不寻常的信号处理需要这个回调
 * 在任何时刻运行都是安全的。
 * --------------------------------
 */
static void
socket_close(int code, Datum arg)
{
	/* 独立后端中无事可做，那里 MyProcPort 为 NULL。 */
	if (MyProcPort != NULL)
	{
#ifdef ENABLE_GSS
		/*
		 * 关闭 GSSAPI 层。打断 BackendInitialize() 时这一节
		 * 什么也不做，因为 pg_GSS_recvauth() 才会首次使用
		 * "ctx" 和 "cred"。
		 *
		 * 注意我们不费心释放 MyProcPort->gss，因为反正
		 * 马上就要退出。
		 */
		if (MyProcPort->gss)
		{
			OM_uint32	min_s;

			if (MyProcPort->gss->ctx != GSS_C_NO_CONTEXT)
				gss_delete_sec_context(&min_s, &MyProcPort->gss->ctx, NULL);

			if (MyProcPort->gss->cred != GSS_C_NO_CREDENTIAL)
				gss_release_cred(&min_s, &MyProcPort->gss->cred);
		}
#endif							/* ENABLE_GSS */

		/*
		 * 干净地关闭 SSL 层。别处没有 postmaster 子进程
		 * 调用它，因此打断 BackendInitialize() 时这样做是安全的。
		 */
		secure_close(MyProcPort);

		/*
		 * 以前我们在这里显式 close()，但让套接字一直开到
		 * 进程死掉似乎更好。这样若客户端在意，就可以做
		 * 一次 "同步关闭" —— 等到传输层报告连接关闭，
		 * 就可以确信后端已经退出。
		 * 我们确实把 sock 设为 PGINVALID_SOCKET，以阻止任何
		 *
		 * 进一步的 I/O，
		 * 不过。
		 */
		MyProcPort->sock = PGINVALID_SOCKET;
	}
}



/*
 * 流 -- Unix 套接字系统调用的包装
 *
 *
 *		Stream 函数用于普通的 TCP 连接协议。
 */


/*
 * StreamServerPort -- 打开一个 "监听" 端口以接受连接。
 *
 * family 应为 AF_UNIX 或 AF_UNSPEC；portNumber 是端口号。
 * 对 AF_UNIX 端口，hostName 应为 NULL，且必须指定 unixSocketDir。
 * 对 TCP 端口，hostName 要么为 NULL（表示所有接口），要么是
 * 要监听的接口，unixSocketDir 被忽略（可以为 NULL）。
 *
 * 成功打开的套接字加入 ListenSocket[] 数组（长度为
 * MaxListen），放在第一个不是 PGINVALID_SOCKET 的位置。
 *
 * RETURNS：STATUS_OK 或 STATUS_ERROR
 */

int
StreamServerPort(int family, const char *hostName, unsigned short portNumber,
				 const char *unixSocketDir,
				 pgsocket ListenSocket[], int MaxListen)
{
	pgsocket	fd;
	int			err;
	int			maxconn;
	int			ret;
	char		portNumberStr[32];
	const char *familyDesc;
	char		familyDescBuf[64];
	const char *addrDesc;
	char		addrBuf[NI_MAXHOST];
	char	   *service;
	struct addrinfo *addrs = NULL,
			   *addr;
	struct addrinfo hint;
	int			listen_index = 0;
	int			added = 0;
	char		unixSocketPath[MAXPGPATH];
#if !defined(WIN32) || defined(IPV6_V6ONLY)
	int			one = 1;
#endif

	/* 初始化 hint 结构 */
	MemSet(&hint, 0, sizeof(hint));
	hint.ai_family = family;
	hint.ai_flags = AI_PASSIVE;
	hint.ai_socktype = SOCK_STREAM;

	if (family == AF_UNIX)
	{
		/*
		 * 由 portNumber 和 unixSocketDir 创建 unixSocketPath，并锁定
		 * 该文件路径
		 */
		UNIXSOCK_PATH(unixSocketPath, portNumber, unixSocketDir);
		if (strlen(unixSocketPath) >= UNIXSOCK_PATH_BUFLEN)
		{
			ereport(LOG,
					(errmsg("Unix-domain socket path \"%s\" is too long (maximum %d bytes)",
							unixSocketPath,
							(int) (UNIXSOCK_PATH_BUFLEN - 1))));
			return STATUS_ERROR;
		}
		if (Lock_AF_UNIX(unixSocketDir, unixSocketPath) != STATUS_OK)
			return STATUS_ERROR;
		service = unixSocketPath;
	}
	else
	{
		snprintf(portNumberStr, sizeof(portNumberStr), "%d", portNumber);
		service = portNumberStr;
	}

	ret = pg_getaddrinfo_all(hostName, service, &hint, &addrs);
	if (ret || !addrs)
	{
		if (hostName)
			ereport(LOG,
					(errmsg("could not translate host name \"%s\", service \"%s\" to address: %s",
							hostName, service, gai_strerror(ret))));
		else
			ereport(LOG,
					(errmsg("could not translate service \"%s\" to address: %s",
							service, gai_strerror(ret))));
		if (addrs)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		return STATUS_ERROR;
	}

	for (addr = addrs; addr; addr = addr->ai_next)
	{
		if (family != AF_UNIX && addr->ai_family == AF_UNIX)
		{
			/*
			 * 只有在他们真正请求时才设置 Unix 域套接字。
			 * 这种情况下服务/端口是不同的。
			 */
			continue;
		}

		/* 看看是否还有空位再加 1 个套接字。 */
		for (; listen_index < MaxListen; listen_index++)
		{
			if (ListenSocket[listen_index] == PGINVALID_SOCKET)
				break;
		}
		if (listen_index >= MaxListen)
		{
			ereport(LOG,
					(errmsg("could not bind to all requested addresses: MAXLISTEN (%d) exceeded",
							MaxListen)));
			break;
		}

		/* 为日志消息设置地址族名称 */
		switch (addr->ai_family)
		{
			case AF_INET:
				familyDesc = _("IPv4");
				break;
			case AF_INET6:
				familyDesc = _("IPv6");
				break;
			case AF_UNIX:
				familyDesc = _("Unix");
				break;
			default:
				snprintf(familyDescBuf, sizeof(familyDescBuf),
						 _("unrecognized address family %d"),
						 addr->ai_family);
				familyDesc = familyDescBuf;
				break;
		}

		/* 为日志消息设置地址的文本形式 */
		if (addr->ai_family == AF_UNIX)
			addrDesc = unixSocketPath;
		else
		{
			pg_getnameinfo_all((const struct sockaddr_storage *) addr->ai_addr,
							   addr->ai_addrlen,
							   addrBuf, sizeof(addrBuf),
							   NULL, 0,
							   NI_NUMERICHOST);
			addrDesc = addrBuf;
		}

		if ((fd = socket(addr->ai_family, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: first %s is IPv4, IPv6, or Unix */
					 errmsg("could not create %s socket for address \"%s\": %m",
							familyDesc, addrDesc)));
			continue;
		}

#ifndef WIN32

		/*
		 * 没有 SO_REUSEADDR 标志时，停止或崩溃后无法立刻
		 * 启动新的 postmaster，TCP 端口会报出 "address already in use"
		 * 错误。
		 *
		 * 不过在 win32 上，只有设置了
		 * SO_EXCLUSIVEADDRUSE 才会有这种行为。有 SO_REUSEADDR 时，win32 允许
		 * 多个服务器监听同一地址，从而导致
		 * 不可预测的行为。完全没有标志时，win32 的行为如同 Unix
		 * 带上 SO_REUSEADDR 时那样。
		 */
		if (addr->ai_family != AF_UNIX)
		{
			if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
							(char *) &one, sizeof(one))) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
				/* translator: third %s is IPv4, IPv6, or Unix */
						 errmsg("%s(%s) failed for %s address \"%s\": %m",
								"setsockopt", "SO_REUSEADDR",
								familyDesc, addrDesc)));
				closesocket(fd);
				continue;
			}
		}
#endif

#ifdef IPV6_V6ONLY
		if (addr->ai_family == AF_INET6)
		{
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
						   (char *) &one, sizeof(one)) == -1)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
				/* translator: third %s is IPv4, IPv6, or Unix */
						 errmsg("%s(%s) failed for %s address \"%s\": %m",
								"setsockopt", "IPV6_V6ONLY",
								familyDesc, addrDesc)));
				closesocket(fd);
				continue;
			}
		}
#endif

		/*
		 * 注意：在某些操作系统上这可能失败，例如早于
		 * 2.4.21-pre3 的 Linux，它们没有 IPV6_V6ONLY 套接字选项，并把
		 * ipv4 地址映射到 ipv6。所有 ipv4
		 * 连接都会显示为 ::ffff:ipv4。
		 */
		err = bind(fd, addr->ai_addr, addr->ai_addrlen);
		if (err < 0)
		{
			int			saved_errno = errno;

			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: first %s is IPv4, IPv6, or Unix */
					 errmsg("could not bind %s address \"%s\": %m",
							familyDesc, addrDesc),
					 saved_errno == EADDRINUSE ?
					 (addr->ai_family == AF_UNIX ?
					  errhint("Is another postmaster already running on port %d?",
							  (int) portNumber) :
					  errhint("Is another postmaster already running on port %d?"
							  " If not, wait a few seconds and retry.",
							  (int) portNumber)) : 0));
			closesocket(fd);
			continue;
		}

		if (addr->ai_family == AF_UNIX)
		{
			if (Setup_AF_UNIX(service) != STATUS_OK)
			{
				closesocket(fd);
				break;
			}
		}

		/*
		 * 选择合适的 accept 队列长度限制。用一个与
		 * postmaster 允许的最大子进程数相近的值
		 * 似乎是合理的。
		 */
		maxconn = MaxConnections * 2;

		err = listen(fd, maxconn);
		if (err < 0)
		{
			ereport(LOG,
					(errcode_for_socket_access(),
			/* translator: first %s is IPv4, IPv6, or Unix */
					 errmsg("could not listen on %s address \"%s\": %m",
							familyDesc, addrDesc)));
			closesocket(fd);
			continue;
		}

		if (addr->ai_family == AF_UNIX)
			ereport(LOG,
					(errmsg("listening on Unix socket \"%s\"",
							addrDesc)));
		else
			ereport(LOG,
			/* translator: first %s is IPv4 or IPv6 */
					(errmsg("listening on %s address \"%s\", port %d",
							familyDesc, addrDesc, (int) portNumber)));

		ListenSocket[listen_index] = fd;
		added++;
	}

	pg_freeaddrinfo_all(hint.ai_family, addrs);

	if (!added)
		return STATUS_ERROR;

	return STATUS_OK;
}


/*
 * Lock_AF_UNIX -- 配置 unix 套接字文件路径
 */
static int
Lock_AF_UNIX(const char *unixSocketDir, const char *unixSocketPath)
{
	/* 抽象套接字没有锁文件 */
	if (unixSocketPath[0] == '@')
		return STATUS_OK;

	/*
	 * 获取与套接字文件关联的互锁文件。
	 *
	 * 注意：使用套接字锁文件，而不是直接在套接字
	 * 本身上互锁，有两个原因。第一，可移植性好得多；
	 * 第二，我们可以没有竞态条件地删除任何已存在的
	 * 套接字文件。
	 */
	CreateSocketLockFile(unixSocketPath, true, unixSocketDir);

	/*
	 * 一旦有了互锁，就可以安全地删除任何已存在的
	 * 套接字文件，以避免 bind() 时失败。
	 */
	(void) unlink(unixSocketPath);

	/*
	 * 记住套接字文件路径名，供以后维护。
	 */
	sock_paths = lappend(sock_paths, pstrdup(unixSocketPath));

	return STATUS_OK;
}


/*
 * Setup_AF_UNIX -- 配置 unix 套接字权限
 */
static int
Setup_AF_UNIX(const char *sock_path)
{
	/* 抽象套接字没有文件系统权限 */
	if (sock_path[0] == '@')
		return STATUS_OK;

	/*
	 * 若有请求，修正套接字的所有权/权限。注意必须在
	 * listen() 之前做这件事，以免出现一个窗口，让不想要的连接
	 * 被接受。
	 */
	Assert(Unix_socket_group);
	if (Unix_socket_group[0] != '\0')
	{
#ifdef WIN32
		elog(WARNING, "configuration item unix_socket_group is not supported on this platform");
#else
		char	   *endptr;
		unsigned long val;
		gid_t		gid;

		val = strtoul(Unix_socket_group, &endptr, 10);
		if (*endptr == '\0')
		{						/* 数值形式的组 id */
			gid = val;
		}
		else
		{						/* 把组名转换成 id */
			struct group *gr;

			gr = getgrnam(Unix_socket_group);
			if (!gr)
			{
				ereport(LOG,
						(errmsg("group \"%s\" does not exist",
								Unix_socket_group)));
				return STATUS_ERROR;
			}
			gid = gr->gr_gid;
		}
		if (chown(sock_path, -1, gid) == -1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not set group of file \"%s\": %m",
							sock_path)));
			return STATUS_ERROR;
		}
#endif
	}

	if (chmod(sock_path, Unix_socket_permissions) == -1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not set permissions of file \"%s\": %m",
						sock_path)));
		return STATUS_ERROR;
	}
	return STATUS_OK;
}


/*
 * StreamConnection -- 用服务器端口与客户端建立新连接。
 *		把 port->sock 设为新连接的 FD。
 *
 * ASSUME：这不必是非阻塞的，因为
 *		Postmaster 会等到套接字准备好再 accept()。
 *
 * RETURNS：STATUS_OK 或 STATUS_ERROR
 */
int
StreamConnection(pgsocket server_fd, Port *port)
{
	/* 接受连接并填入客户端（远程）地址 */
	port->raddr.salen = sizeof(port->raddr.addr);
	if ((port->sock = accept(server_fd,
							 (struct sockaddr *) &port->raddr.addr,
							 &port->raddr.salen)) == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not accept new connection: %m")));

		/*
		 * 若 accept() 失败，postmaster.c 仍会把服务器
		 * 套接字看成可读，并立刻重试。为避免
		 * 无用地吃掉大量 CPU，重试前延迟一会儿。
		 * （最可能的失败原因是内核文件表槽位用尽；我们几乎
		 * 无能为力，只能希望有一些会被释放。）
		 */
		pg_usleep(100000L);		/* 等待 0.1 秒 */
		return STATUS_ERROR;
	}

	/* 填入服务器（本地）地址 */
	port->laddr.salen = sizeof(port->laddr.addr);
	if (getsockname(port->sock,
					(struct sockaddr *) &port->laddr.addr,
					&port->laddr.salen) < 0)
	{
		ereport(LOG,
				(errmsg("%s() failed: %m", "getsockname")));
		return STATUS_ERROR;
	}

	/* 若是 TCP 连接，选择 NODELAY 和 KEEPALIVE 选项 */
	if (port->laddr.addr.ss_family != AF_UNIX)
	{
		int			on;
#ifdef WIN32
		int			oldopt;
		int			optlen;
		int			newopt;
#endif

#ifdef	TCP_NODELAY
		on = 1;
		if (setsockopt(port->sock, IPPROTO_TCP, TCP_NODELAY,
					   (char *) &on, sizeof(on)) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "setsockopt", "TCP_NODELAY")));
			return STATUS_ERROR;
		}
#endif
		on = 1;
		if (setsockopt(port->sock, SOL_SOCKET, SO_KEEPALIVE,
					   (char *) &on, sizeof(on)) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "setsockopt", "SO_KEEPALIVE")));
			return STATUS_ERROR;
		}

#ifdef WIN32

		/*
		 * 这是一项 Win32 套接字优化。操作系统发送缓冲区应当
		 * 大到能一次发出整个 Postgres 发送缓冲区，否则
		 * 性能会受影响。若需要发送非常大的消息，Postgres 发送缓冲区
		 * 可以加大，但那种情况下我们不会尝试
		 * 加大操作系统缓冲区，因此有些任意地
		 * 确保操作系统缓冲区至少为 PQ_SEND_BUFFER_SIZE * 4。
		 * （以当前默认值，那是 32kB。）
		 *
		 * 早期 Windows 版本中默认的操作系统缓冲区大小曾是 8kB，
		 * 但在 Windows 2012 中提高到了 64kB。因此在更新的版本里
		 * 应该不必再改它。不必要地改它甚至可能
		 * 降低性能，因为在应用程序里设置
		 * SO_SNDBUF 会禁用 Windows 7 引入的 "动态发送缓冲"
		 * 功能。因此在摆弄
		 * SO_SNDBUF 之前，先检查当前缓冲区是否已经够大，
		 * 并且只在必要时才增大它。
		 *
		 * 参见 https://support.microsoft.com/kb/823764/EN-US/ 和
		 * https://msdn.microsoft.com/en-us/library/bb736549%28v=vs.85%29.aspx
		 */
		optlen = sizeof(oldopt);
		if (getsockopt(port->sock, SOL_SOCKET, SO_SNDBUF, (char *) &oldopt,
					   &optlen) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "getsockopt", "SO_SNDBUF")));
			return STATUS_ERROR;
		}
		newopt = PQ_SEND_BUFFER_SIZE * 4;
		if (oldopt < newopt)
		{
			if (setsockopt(port->sock, SOL_SOCKET, SO_SNDBUF, (char *) &newopt,
						   sizeof(newopt)) < 0)
			{
				ereport(LOG,
						(errmsg("%s(%s) failed: %m", "setsockopt", "SO_SNDBUF")));
				return STATUS_ERROR;
			}
		}
#endif

		/*
		 * 也应用当前的 keepalive 参数。若设置某个
		 * 参数失败，不要报错退出，因为它们并非普遍
		 * 支持。（注意：你也许认为这种情况下需要把 GUC
		 * 变量重置为 0，但没有必要，因为这些变量的
		 * show 钩子反正会报告真实值。）
		 */
		(void) pq_setkeepalivesidle(tcp_keepalives_idle, port);
		(void) pq_setkeepalivesinterval(tcp_keepalives_interval, port);
		(void) pq_setkeepalivescount(tcp_keepalives_count, port);
		(void) pq_settcpusertimeout(tcp_user_timeout, port);
	}

	return STATUS_OK;
}

/*
 * StreamClose -- 关闭一条客户端/后端连接
 *
 * 注意：这不是用来终止会话的；只是用来释放
 * 一个不该再打开该套接字的进程中的文件描述符。
 * （例如，postmaster 在把连接所有权交给
 * 子进程之后调用它。）预期别人仍开着该套接字。
 * 因此我们只想关闭描述符，
 * 并不想向远端发送任何东西。
 */
void
StreamClose(pgsocket sock)
{
	closesocket(sock);
}

/*
 * TouchSocketFiles -- 把套接字文件标记为最近访问过
 *
 * 应当时常调用本例程，以确保套接字
 * 文件有较新的修改时间（对套接字的普通操作通常不会
 * 改变修改时间）。这样它们就不会被过于热心的
 * /tmp 目录清理守护进程删掉。（我们本不该把套接字文件
 * 放在 /tmp 里的另一个原因……）
 */
void
TouchSocketFiles(void)
{
	ListCell   *l;

	/* 遍历所有已创建的套接字…… */
	foreach(l, sock_paths)
	{
		char	   *sock_path = (char *) lfirst(l);

		/* 忽略错误；抱怨没有意义 */
		(void) utime(sock_path, NULL);
	}
}

/*
 * RemoveSocketFiles -- 在 postmaster 关闭时 unlink 套接字文件
 */
void
RemoveSocketFiles(void)
{
	ListCell   *l;

	/* 遍历所有已创建的套接字…… */
	foreach(l, sock_paths)
	{
		char	   *sock_path = (char *) lfirst(l);

		/* 忽略任何错误。 */
		(void) unlink(sock_path);
	}
	/* 既然马上要退出，就不必回收存储 */
	sock_paths = NIL;
}


/* --------------------------------
 * 低层 I/O 例程从这里开始。
 *
 * 这些例程通过前面的例程已经建立的连接，
 * 与前端客户端通信。
 * --------------------------------
 */

/* --------------------------------
 *			  socket_set_nonblocking - 设置套接字为阻塞/非阻塞
 *
 * 若 nonblocking 为真，把套接字设为非阻塞，否则设为
 * 阻塞。
 * --------------------------------
 */
static void
socket_set_nonblocking(bool nonblocking)
{
	if (MyProcPort == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("there is no client connection")));

	MyProcPort->noblock = nonblocking;
}

/* --------------------------------
 *		pq_recvbuf - 把一些字节装入输入缓冲区
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
static int
pq_recvbuf(void)
{
	if (PqRecvPointer > 0)
	{
		if (PqRecvLength > PqRecvPointer)
		{
			/* 仍有未读数据，在缓冲区里左对齐 */
			memmove(PqRecvBuffer, PqRecvBuffer + PqRecvPointer,
					PqRecvLength - PqRecvPointer);
			PqRecvLength -= PqRecvPointer;
			PqRecvPointer = 0;
		}
		else
			PqRecvLength = PqRecvPointer = 0;
	}

	/* 确保我们处于阻塞模式 */
	socket_set_nonblocking(false);

	/* 可以从 PqRecvLength 往上填满缓冲区 */
	for (;;)
	{
		int			r;

		errno = 0;

		r = secure_read(MyProcPort, PqRecvBuffer + PqRecvLength,
						PQ_RECV_BUFFER_SIZE - PqRecvLength);

		if (r < 0)
		{
			if (errno == EINTR)
				continue;		/* 被中断也没关系 */

			/*
			 * 小心：试图写给客户端的 ereport() 会
			 * 导致递归到这里，造成栈溢出和核心
			 * 转储！这条消息必须*只*进入 postmaster 日志。
			 *
			 * 若 errno 为零，就假定是 EOF，让调用方去抱怨。
			 */
			if (errno != 0)
				ereport(COMMERROR,
						(errcode_for_socket_access(),
						 errmsg("could not receive data from client: %m")));
			return EOF;
		}
		if (r == 0)
		{
			/*
			 * 检测到 EOF。我们以前在这里写一条日志消息，但最好
			 * 预期最终的调用方去做这件事。
			 */
			return EOF;
		}
		/* r 含有读到的字节数，因此只需增加长度 */
		PqRecvLength += r;
		return 0;
	}
}

/* --------------------------------
 *		pq_getbyte - 从连接取一个字节，或返回 EOF
 * --------------------------------
 */
int
pq_getbyte(void)
{
	Assert(PqCommReadingMsg);

	while (PqRecvPointer >= PqRecvLength)
	{
		if (pq_recvbuf())		/* 若缓冲区里什么都没有，就 recv 一些 */
			return EOF;			/* 接收数据失败 */
	}
	return (unsigned char) PqRecvBuffer[PqRecvPointer++];
}

/* --------------------------------
 *		pq_peekbyte - 窥看连接上的下一个字节
 *
 *	 与 pq_getbyte() 相同，只是我们不推进指针。
 * --------------------------------
 */
int
pq_peekbyte(void)
{
	Assert(PqCommReadingMsg);

	while (PqRecvPointer >= PqRecvLength)
	{
		if (pq_recvbuf())		/* 若缓冲区里什么都没有，就 recv 一些 */
			return EOF;			/* 接收数据失败 */
	}
	return (unsigned char) PqRecvBuffer[PqRecvPointer];
}

/* --------------------------------
 *		pq_getbyte_if_available - 从连接取一个字节，
 *			若可用的话
 *
 * 收到的字节存在 *c 中。读到一个字节返回 1，
 * 没有可用数据返回 0，出问题返回 EOF。
 * --------------------------------
 */
int
pq_getbyte_if_available(unsigned char *c)
{
	int			r;

	Assert(PqCommReadingMsg);

	if (PqRecvPointer < PqRecvLength)
	{
		*c = PqRecvBuffer[PqRecvPointer++];
		return 1;
	}

	/* 把套接字设为非阻塞模式 */
	socket_set_nonblocking(true);

	errno = 0;

	r = secure_read(MyProcPort, c, 1);
	if (r < 0)
	{
		/*
		 * 没有数据可非阻塞取得，或被中断，都没关系（不过
		 * 非阻塞套接字上 EINTR 实在不该发生）。报告
		 * 其他错误。
		 */
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			r = 0;
		else
		{
			/*
			 * 小心：试图写给客户端的 ereport() 会
			 * 导致递归到这里，造成栈溢出和核心
			 * 转储！这条消息必须*只*进入 postmaster 日志。
			 *
			 * 若 errno 为零，就假定是 EOF，让调用方去抱怨。
			 */
			if (errno != 0)
				ereport(COMMERROR,
						(errcode_for_socket_access(),
						 errmsg("could not receive data from client: %m")));
			r = EOF;
		}
	}
	else if (r == 0)
	{
		/* 检测到 EOF */
		r = EOF;
	}

	return r;
}

/* --------------------------------
 *		pq_getbytes - 从连接取得已知数量的字节
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
int
pq_getbytes(char *s, size_t len)
{
	size_t		amount;

	Assert(PqCommReadingMsg);

	while (len > 0)
	{
		while (PqRecvPointer >= PqRecvLength)
		{
			if (pq_recvbuf())	/* 若缓冲区里什么都没有，就 recv 一些 */
				return EOF;		/* 接收数据失败 */
		}
		amount = PqRecvLength - PqRecvPointer;
		if (amount > len)
			amount = len;
		memcpy(s, PqRecvBuffer + PqRecvPointer, amount);
		PqRecvPointer += amount;
		s += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		pq_discardbytes - 丢掉已知数量的字节
 *
 *		与 pq_getbytes 相同，只是我们不把数据复制到任何地方。
 *		用于读错误之后重新同步。
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
static int
pq_discardbytes(size_t len)
{
	size_t		amount;

	Assert(PqCommReadingMsg);

	while (len > 0)
	{
		while (PqRecvPointer >= PqRecvLength)
		{
			if (pq_recvbuf())	/* 若缓冲区里什么都没有，就 recv 一些 */
				return EOF;		/* 接收数据失败 */
		}
		amount = PqRecvLength - PqRecvPointer;
		if (amount > len)
			amount = len;
		PqRecvPointer += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		pq_buffer_has_data - 是否有已缓冲的数据可供读取？
 *
 * 这*不会*尝试读取更多数据。
 * --------------------------------
 */
bool
pq_buffer_has_data(void)
{
	return (PqRecvPointer < PqRecvLength);
}


/* --------------------------------
 *		pq_startmsgread - 开始从客户端读取一条消息。
 *
 *		在任何 pq_get* 函数之前都必须调用它。
 * --------------------------------
 */
void
pq_startmsgread(void)
{
	/*
	 * 按理不该已经有一次读取在进行，但还是检查一下
	 * 以确认。
	 */
	if (PqCommReadingMsg)
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("terminating connection because protocol synchronization was lost")));

	PqCommReadingMsg = true;
}


/* --------------------------------
 *		pq_endmsgread - 结束读取消息。
 *
 *		用 pq_getbytes() 及其同类读完一条消息之后必须调用它，
 *		以表明我们已经读完整条消息。
 *		pq_getmessage() 会隐式做这件事。
 * --------------------------------
 */
void
pq_endmsgread(void)
{
	Assert(PqCommReadingMsg);

	PqCommReadingMsg = false;
}

/* --------------------------------
 *		pq_is_reading_msg - 我们当前是否正在读一条消息？
 *
 * 这用在外层空闲循环的错误恢复中，检测我们是否
 * 失去了协议同步，需要终止连接。pq_startmsgread()
 * 也会检查这一点，但更早发现更好。
 * --------------------------------
 */
bool
pq_is_reading_msg(void)
{
	return PqCommReadingMsg;
}

/* --------------------------------
 *		pq_getmessage - 从连接取得带长度字的消息
 *
 *		返回值放进一个可扩展的 StringInfo，调用方
 *		已经把它初始化好了。
 *		只有消息体放进 StringInfo；长度字
 *		被去掉。另外，s->cursor 被初始化为零，以便
 *		扫描消息内容。
 *
 *		maxlen 是我们愿意接受的
 *		消息长度上限。若客户端试图发送更多，我们就中止连接
 *		（通过返回 EOF）。
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
int
pq_getmessage(StringInfo s, int maxlen)
{
	int32		len;

	Assert(PqCommReadingMsg);

	resetStringInfo(s);

	/* 读取消息长度字 */
	if (pq_getbytes((char *) &len, 4) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF within message length word")));
		return EOF;
	}

	len = pg_ntoh32(len);

	if (len < 4 || len > maxlen)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid message length")));
		return EOF;
	}

	len -= 4;					/* 扣掉长度自身 */

	if (len > 0)
	{
		/*
		 * 为消息分配空间。若空间用尽（消息大得
		 * 荒唐），我们会 elog(ERROR)，但我们想丢掉
		 * 消息体，以免失去通信同步。
		 */
		PG_TRY();
		{
			enlargeStringInfo(s, len);
		}
		PG_CATCH();
		{
			if (pq_discardbytes(len) == EOF)
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("incomplete message from client")));

			/* 我们已经丢掉消息的其余部分，因此重新同步了。 */
			PqCommReadingMsg = false;
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* 然后抓取这条消息 */
		if (pq_getbytes(s->data, len) == EOF)
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete message from client")));
			return EOF;
		}
		s->len = len;
		/* 按 StringInfo 的约定放一个末尾空字符 */
		s->data[len] = '\0';
	}

	/* 消息已经读完。 */
	PqCommReadingMsg = false;

	return 0;
}


static int
internal_putbytes(const char *s, size_t len)
{
	size_t		amount;

	while (len > 0)
	{
		/* 若缓冲区已满，就把它刷出去 */
		if (PqSendPointer >= PqSendBufferSize)
		{
			socket_set_nonblocking(false);
			if (internal_flush())
				return EOF;
		}
		amount = PqSendBufferSize - PqSendPointer;
		if (amount > len)
			amount = len;
		memcpy(PqSendBuffer + PqSendPointer, s, amount);
		PqSendPointer += amount;
		s += amount;
		len -= amount;
	}
	return 0;
}

/* --------------------------------
 *		socket_flush - 刷出待处理的输出
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
static int
socket_flush(void)
{
	int			res;

	/* 若是重入调用则无操作 */
	if (PqCommBusy)
		return 0;
	PqCommBusy = true;
	socket_set_nonblocking(false);
	res = internal_flush();
	PqCommBusy = false;
	return res;
}

/* --------------------------------
 *		internal_flush - 刷出待处理的输出
 *
 * 若一切都已发送，或操作会阻塞且套接字处于非阻塞模式，
 * 则返回 0；出问题则返回 EOF。
 * --------------------------------
 */
static int
internal_flush(void)
{
	static int	last_reported_send_errno = 0;

	char	   *bufptr = PqSendBuffer + PqSendStart;
	char	   *bufend = PqSendBuffer + PqSendPointer;

	while (bufptr < bufend)
	{
		int			r;

		r = secure_write(MyProcPort, bufptr, bufend - bufptr);

		if (r <= 0)
		{
			if (errno == EINTR)
				continue;		/* 被中断也没关系 */

			/*
			 * 若没有数据可以非阻塞地写，且套接字处于
			 * 非阻塞模式，也没关系。
			 */
			if (errno == EAGAIN ||
				errno == EWOULDBLOCK)
			{
				return 0;
			}

			/*
			 * 小心：试图写给客户端的 ereport() 会
			 * 导致递归到这里，造成栈溢出和核心
			 * 转储！这条消息必须*只*进入 postmaster 日志。
			 *
			 * 若客户端在我们输出中途断开，我们可能会
			 * 写出相当多的数据，然后才到一个安全的查询
			 * 中止点。因此，抑制重复的日志消息。
			 */
			if (errno != last_reported_send_errno)
			{
				last_reported_send_errno = errno;
				ereport(COMMERROR,
						(errcode_for_socket_access(),
						 errmsg("could not send data to client: %m")));
			}

			/*
			 * 我们反正丢掉缓冲的数据，以便处理可以
			 * 继续，尽管我们可能很快就会退出。我们还设置一个
			 * 标志，使下一次 CHECK_FOR_INTERRUPTS 终止
			 * 连接。
			 */
			PqSendStart = PqSendPointer = 0;
			ClientConnectionLost = 1;
			InterruptPending = 1;
			return EOF;
		}

		last_reported_send_errno = 0;	/* 任何成功发送之后重置 */
		bufptr += r;
		PqSendStart += r;
	}

	PqSendStart = PqSendPointer = 0;
	return 0;
}

/* --------------------------------
 *		pq_flush_if_writable - 若可写且不会阻塞，就刷出待处理输出
 *
 * 成功返回 0，出问题返回 EOF。
 * --------------------------------
 */
static int
socket_flush_if_writable(void)
{
	int			res;

	/* 若无事可做就快速退出 */
	if (PqSendPointer == PqSendStart)
		return 0;

	/* 若是重入调用则无操作 */
	if (PqCommBusy)
		return 0;

	/* 暂时把套接字设为非阻塞模式 */
	socket_set_nonblocking(true);

	PqCommBusy = true;
	res = internal_flush();
	PqCommBusy = false;
	return res;
}

/* --------------------------------
 *	socket_is_send_pending - 输出缓冲区里是否有待处理数据？
 * --------------------------------
 */
static bool
socket_is_send_pending(void)
{
	return (PqSendStart < PqSendPointer);
}

/* --------------------------------
 * 消息级 I/O 例程从这里开始。
 * --------------------------------
 */


/* --------------------------------
 *		socket_putmessage - 发送一条普通消息（COPY OUT 模式下被抑制）
 *
 *		msgtype 是放在消息体之前的消息类型码。
 *
 *		len 是 *s 处消息体数据的长度。本例程插入一个
 *		消息长度字（等于 len+4，因为它也计入自身）。
 *		当 pqcomm.c 正忙时，我们抑制所产生的消息。这
 *
 *		避免了消息被插入其他消息之中的任何可能。
 *		已知的唯一麻烦是 SIGQUIT 发生在
 *		某个 pqcomm.c 例程执行期间 --- quickdie() 会试图发送
 *		一条警告消息，最合理的做法似乎是
 *		丢掉它。
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
static int
socket_putmessage(char msgtype, const char *s, size_t len)
{
	uint32		n32;

	Assert(msgtype != 0);

	if (PqCommBusy)
		return 0;
	PqCommBusy = true;
	if (internal_putbytes(&msgtype, 1))
		goto fail;

	n32 = pg_hton32((uint32) (len + 4));
	if (internal_putbytes((char *) &n32, 4))
		goto fail;

	if (internal_putbytes(s, len))
		goto fail;
	PqCommBusy = false;
	return 0;

fail:
	PqCommBusy = false;
	return EOF;
}

/* --------------------------------
 *		pq_putmessage_noblock - 与 pq_putmessage 类似，但从不阻塞
 *
 *		若输出缓冲区太小，放不下这条消息，缓冲区
 *		会被加大。
 */
static void
socket_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	int			res PG_USED_FOR_ASSERTS_ONLY;
	int			required;

	/*
	 * 确保输出缓冲区有足够空间放下消息头
	 * 以及消息本身。
	 */
	required = PqSendPointer + 1 + 4 + len;
	if (required > PqSendBufferSize)
	{
		PqSendBuffer = repalloc(PqSendBuffer, required);
		PqSendBufferSize = required;
	}
	res = pq_putmessage(msgtype, s, len);
	Assert(res == 0);			/* 当消息能放进缓冲区时，
								 * 不应失败 */
}

/* --------------------------------
 *		pq_putmessage_v2 - 按协议版本 2 发送一条消息
 *
 *		msgtype 是放在消息体之前的消息类型码。
 *
 *		我们不再支持协议版本 2，但保留了这个
 *		函数，以便若客户端试图用协议版本 2 连接，
 *		作为礼貌，我们仍能以旧格式向客户端发送
 *		"unsupported protocol version" 错误。
 *
 *		与 pq_putmessage() 中一样，当 pqcomm.c 正忙时
 *		我们抑制所产生的消息。
 *
 *		成功返回 0，出问题返回 EOF
 * --------------------------------
 */
int
pq_putmessage_v2(char msgtype, const char *s, size_t len)
{
	Assert(msgtype != 0);

	if (PqCommBusy)
		return 0;
	PqCommBusy = true;
	if (internal_putbytes(&msgtype, 1))
		goto fail;

	if (internal_putbytes(s, len))
		goto fail;
	PqCommBusy = false;
	return 0;

fail:
	PqCommBusy = false;
	return EOF;
}

/*
 * 对 TCP Keepalive 参数的支持
 */

/*
 * 在 Windows 上，我们需要同时设置 idle 和 interval。
 * 我们也无法把它们重置为默认值（设为零实际上会
 * 把它们设为零，而不是默认值），因此我们退回到
 * 出厂默认值。
 */
#if defined(WIN32) && defined(SIO_KEEPALIVE_VALS)
static int
pq_setkeepaliveswin32(Port *port, int idle, int interval)
{
	struct tcp_keepalive ka;
	DWORD		retsize;

	if (idle <= 0)
		idle = 2 * 60 * 60;		/* 默认值 = 2 小时 */
	if (interval <= 0)
		interval = 1;			/* 默认值 = 1 秒 */

	ka.onoff = 1;
	ka.keepalivetime = idle * 1000;
	ka.keepaliveinterval = interval * 1000;

	if (WSAIoctl(port->sock,
				 SIO_KEEPALIVE_VALS,
				 (LPVOID) &ka,
				 sizeof(ka),
				 NULL,
				 0,
				 &retsize,
				 NULL,
				 NULL)
		!= 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) failed: error code %d",
						"WSAIoctl", "SIO_KEEPALIVE_VALS", WSAGetLastError())));
		return STATUS_ERROR;
	}
	if (port->keepalives_idle != idle)
		port->keepalives_idle = idle;
	if (port->keepalives_interval != interval)
		port->keepalives_interval = interval;
	return STATUS_OK;
}
#endif

int
pq_getkeepalivesidle(Port *port)
{
#if defined(PG_TCP_KEEPALIVE_IDLE) || defined(SIO_KEEPALIVE_VALS)
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return 0;

	if (port->keepalives_idle != 0)
		return port->keepalives_idle;

	if (port->default_keepalives_idle == 0)
	{
#ifndef WIN32
		socklen_t	size = sizeof(port->default_keepalives_idle);

		if (getsockopt(port->sock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
					   (char *) &port->default_keepalives_idle,
					   &size) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "getsockopt", PG_TCP_KEEPALIVE_IDLE_STR)));
			port->default_keepalives_idle = -1; /* 不知道 */
		}
#else							/* WIN32 */
		/* 在 Windows 上我们无法取得默认值，因此返回 "不知道" */
		port->default_keepalives_idle = -1;
#endif							/* WIN32 */
	}

	return port->default_keepalives_idle;
#else
	return 0;
#endif
}

int
pq_setkeepalivesidle(int idle, Port *port)
{
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return STATUS_OK;

/* 在这里检查 SIO_KEEPALIVE_VALS，而不只是 WIN32，因为有些工具链没有它 */
#if defined(PG_TCP_KEEPALIVE_IDLE) || defined(SIO_KEEPALIVE_VALS)
	if (idle == port->keepalives_idle)
		return STATUS_OK;

#ifndef WIN32
	if (port->default_keepalives_idle <= 0)
	{
		if (pq_getkeepalivesidle(port) < 0)
		{
			if (idle == 0)
				return STATUS_OK;	/* 默认值已设置但未知 */
			else
				return STATUS_ERROR;
		}
	}

	if (idle == 0)
		idle = port->default_keepalives_idle;

	if (setsockopt(port->sock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
				   (char *) &idle, sizeof(idle)) < 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) failed: %m", "setsockopt", PG_TCP_KEEPALIVE_IDLE_STR)));
		return STATUS_ERROR;
	}

	port->keepalives_idle = idle;
#else							/* WIN32 */
	return pq_setkeepaliveswin32(port, idle, port->keepalives_interval);
#endif
#else
	if (idle != 0)
	{
		ereport(LOG,
				(errmsg("setting the keepalive idle time is not supported")));
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}

int
pq_getkeepalivesinterval(Port *port)
{
#if defined(TCP_KEEPINTVL) || defined(SIO_KEEPALIVE_VALS)
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return 0;

	if (port->keepalives_interval != 0)
		return port->keepalives_interval;

	if (port->default_keepalives_interval == 0)
	{
#ifndef WIN32
		socklen_t	size = sizeof(port->default_keepalives_interval);

		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPINTVL,
					   (char *) &port->default_keepalives_interval,
					   &size) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "getsockopt", "TCP_KEEPINTVL")));
			port->default_keepalives_interval = -1; /* 不知道 */
		}
#else
		/* 在 Windows 上我们无法取得默认值，因此返回 "不知道" */
		port->default_keepalives_interval = -1;
#endif							/* WIN32 */
	}

	return port->default_keepalives_interval;
#else
	return 0;
#endif
}

int
pq_setkeepalivesinterval(int interval, Port *port)
{
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return STATUS_OK;

#if defined(TCP_KEEPINTVL) || defined(SIO_KEEPALIVE_VALS)
	if (interval == port->keepalives_interval)
		return STATUS_OK;

#ifndef WIN32
	if (port->default_keepalives_interval <= 0)
	{
		if (pq_getkeepalivesinterval(port) < 0)
		{
			if (interval == 0)
				return STATUS_OK;	/* 默认值已设置但未知 */
			else
				return STATUS_ERROR;
		}
	}

	if (interval == 0)
		interval = port->default_keepalives_interval;

	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPINTVL,
				   (char *) &interval, sizeof(interval)) < 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) failed: %m", "setsockopt", "TCP_KEEPINTVL")));
		return STATUS_ERROR;
	}

	port->keepalives_interval = interval;
#else							/* WIN32 */
	return pq_setkeepaliveswin32(port, port->keepalives_idle, interval);
#endif
#else
	if (interval != 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) not supported", "setsockopt", "TCP_KEEPINTVL")));
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}

int
pq_getkeepalivescount(Port *port)
{
#ifdef TCP_KEEPCNT
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return 0;

	if (port->keepalives_count != 0)
		return port->keepalives_count;

	if (port->default_keepalives_count == 0)
	{
		socklen_t	size = sizeof(port->default_keepalives_count);

		if (getsockopt(port->sock, IPPROTO_TCP, TCP_KEEPCNT,
					   (char *) &port->default_keepalives_count,
					   &size) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "getsockopt", "TCP_KEEPCNT")));
			port->default_keepalives_count = -1;	/* 不知道 */
		}
	}

	return port->default_keepalives_count;
#else
	return 0;
#endif
}

int
pq_setkeepalivescount(int count, Port *port)
{
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return STATUS_OK;

#ifdef TCP_KEEPCNT
	if (count == port->keepalives_count)
		return STATUS_OK;

	if (port->default_keepalives_count <= 0)
	{
		if (pq_getkeepalivescount(port) < 0)
		{
			if (count == 0)
				return STATUS_OK;	/* 默认值已设置但未知 */
			else
				return STATUS_ERROR;
		}
	}

	if (count == 0)
		count = port->default_keepalives_count;

	if (setsockopt(port->sock, IPPROTO_TCP, TCP_KEEPCNT,
				   (char *) &count, sizeof(count)) < 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) failed: %m", "setsockopt", "TCP_KEEPCNT")));
		return STATUS_ERROR;
	}

	port->keepalives_count = count;
#else
	if (count != 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) not supported", "setsockopt", "TCP_KEEPCNT")));
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}

int
pq_gettcpusertimeout(Port *port)
{
#ifdef TCP_USER_TIMEOUT
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return 0;

	if (port->tcp_user_timeout != 0)
		return port->tcp_user_timeout;

	if (port->default_tcp_user_timeout == 0)
	{
		socklen_t	size = sizeof(port->default_tcp_user_timeout);

		if (getsockopt(port->sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
					   (char *) &port->default_tcp_user_timeout,
					   &size) < 0)
		{
			ereport(LOG,
					(errmsg("%s(%s) failed: %m", "getsockopt", "TCP_USER_TIMEOUT")));
			port->default_tcp_user_timeout = -1;	/* 不知道 */
		}
	}

	return port->default_tcp_user_timeout;
#else
	return 0;
#endif
}

int
pq_settcpusertimeout(int timeout, Port *port)
{
	if (port == NULL || port->laddr.addr.ss_family == AF_UNIX)
		return STATUS_OK;

#ifdef TCP_USER_TIMEOUT
	if (timeout == port->tcp_user_timeout)
		return STATUS_OK;

	if (port->default_tcp_user_timeout <= 0)
	{
		if (pq_gettcpusertimeout(port) < 0)
		{
			if (timeout == 0)
				return STATUS_OK;	/* 默认值已设置但未知 */
			else
				return STATUS_ERROR;
		}
	}

	if (timeout == 0)
		timeout = port->default_tcp_user_timeout;

	if (setsockopt(port->sock, IPPROTO_TCP, TCP_USER_TIMEOUT,
				   (char *) &timeout, sizeof(timeout)) < 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) failed: %m", "setsockopt", "TCP_USER_TIMEOUT")));
		return STATUS_ERROR;
	}

	port->tcp_user_timeout = timeout;
#else
	if (timeout != 0)
	{
		ereport(LOG,
				(errmsg("%s(%s) not supported", "setsockopt", "TCP_USER_TIMEOUT")));
		return STATUS_ERROR;
	}
#endif

	return STATUS_OK;
}

/*
 * tcp_keepalives_idle 的 GUC assign_hook
 */
void
assign_tcp_keepalives_idle(int newval, void *extra)
{
	/*
	 * 内核 API 没有办法测试一个值而不去设置它；而且
	 * 一旦设置了，我们可能无法再取消。因此对这些
	 * 变量完整实现先检查再赋值的 GUC API
	 * 似乎意义不大。我们改为按需赋值。
	 * pq_setkeepalivesidle 通过 ereport(LOG) 报告任何问题。
	 *
	 * 这种方法意味着 GUC 值可能与实际内核值
	 * 关系不大，因此我们使用 show_hook 来取得内核
	 * 值，而不是信任 GUC 的副本。
	 */
	(void) pq_setkeepalivesidle(newval, MyProcPort);
}

/*
 * tcp_keepalives_idle 的 GUC show_hook
 */
const char *
show_tcp_keepalives_idle(void)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesidle(MyProcPort));
	return nbuf;
}

/*
 * tcp_keepalives_interval 的 GUC assign_hook
 */
void
assign_tcp_keepalives_interval(int newval, void *extra)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	(void) pq_setkeepalivesinterval(newval, MyProcPort);
}

/*
 * tcp_keepalives_interval 的 GUC show_hook
 */
const char *
show_tcp_keepalives_interval(void)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivesinterval(MyProcPort));
	return nbuf;
}

/*
 * tcp_keepalives_count 的 GUC assign_hook
 */
void
assign_tcp_keepalives_count(int newval, void *extra)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	(void) pq_setkeepalivescount(newval, MyProcPort);
}

/*
 * tcp_keepalives_count 的 GUC show_hook
 */
const char *
show_tcp_keepalives_count(void)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_getkeepalivescount(MyProcPort));
	return nbuf;
}

/*
 * tcp_user_timeout 的 GUC assign_hook
 */
void
assign_tcp_user_timeout(int newval, void *extra)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	(void) pq_settcpusertimeout(newval, MyProcPort);
}

/*
 * tcp_user_timeout 的 GUC show_hook
 */
const char *
show_tcp_user_timeout(void)
{
	/* 参见 assign_tcp_keepalives_idle 中的注释 */
	static char nbuf[16];

	snprintf(nbuf, sizeof(nbuf), "%d", pq_gettcpusertimeout(MyProcPort));
	return nbuf;
}

/*
 * 检查客户端是否仍然连接着。
 */
bool
pq_check_connection(void)
{
	WaitEvent	events[FeBeWaitSetNEvents];
	int			rc;

	/*
	 * 修改套接字事件过滤器而不恢复是可以的，因为
	 * 所有 FeBeWaitSet 套接字等待点都这样做。
	 */
	ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, WL_SOCKET_CLOSED, NULL);

retry:
	rc = WaitEventSetWait(FeBeWaitSet, 0, events, lengthof(events), 0);
	for (int i = 0; i < rc; ++i)
	{
		if (events[i].events & WL_SOCKET_CLOSED)
			return false;
		if (events[i].events & WL_LATCH_SET)
		{
			/*
			 * 锁存器事件可能妨碍其他事件被
			 * 报告。重置它再轮询一次。不必恢复它，
			 * 因为不应有代码预期锁存器能跨越
			 * CHECK_FOR_INTERRUPTS() 继续存在。
			 */
			ResetLatch(MyLatch);
			goto retry;
		}
	}

	return true;
}
