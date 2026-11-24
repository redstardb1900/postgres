/*-------------------------------------------------------------------------
 *
 * socket.c
 *	  Microsoft Windows Win32 套接字函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/socket.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * 指示 pgwin32_recv() 与 pgwin32_send() 是否应
 * 非阻塞模式。
 *
 * 套接字仿真层总把实际套接字设为非阻塞以便
 * 投递信号，若确需非阻塞操作，须用单独标志
 * 指明。
 * operation.
 *
 * 此标志*全局*改变所有套接字操作的行为，
 * 故只应设极短时间。
 */
int			pgwin32_noblock = 0;

/* 取消 win32.h 中定义的宏，以便访问系统函数 */
#undef socket
#undef bind
#undef listen
#undef accept
#undef connect
#undef select
#undef recv
#undef send

/*
 * 阻塞套接字函数实现为同时监听套接字与
 * 信号事件，信号处理需要如此。
 */

/*
 * 把最后的套接字错误码转为 errno
 *
 * 注意：若 WSAxxx 错误码与 Berkeley 错误符号有直接
 * 对应，此映射实际为空操作，因在 win32_port.h 中
 * 我们把网络相关 Berkeley 错误符号重定义为
 * 其 WSAxxx 对应值。switch 的要点主要是把
 * 近似错误码翻译成 Berkeley 体系中说得通的
 * 形式。
 */
static void
TranslateSocketError(void)
{
	switch (WSAGetLastError())
	{
		case WSAEINVAL:
		case WSANOTINITIALISED:
		case WSAEINVALIDPROVIDER:
		case WSAEINVALIDPROCTABLE:
		case WSAEDESTADDRREQ:
			errno = EINVAL;
			break;
		case WSAEINPROGRESS:
			errno = EINPROGRESS;
			break;
		case WSAEFAULT:
			errno = EFAULT;
			break;
		case WSAEISCONN:
			errno = EISCONN;
			break;
		case WSAEMSGSIZE:
			errno = EMSGSIZE;
			break;
		case WSAEAFNOSUPPORT:
			errno = EAFNOSUPPORT;
			break;
		case WSAEMFILE:
			errno = EMFILE;
			break;
		case WSAENOBUFS:
			errno = ENOBUFS;
			break;
		case WSAEPROTONOSUPPORT:
		case WSAEPROTOTYPE:
		case WSAESOCKTNOSUPPORT:
			errno = EPROTONOSUPPORT;
			break;
		case WSAECONNABORTED:
			errno = ECONNABORTED;
			break;
		case WSAECONNREFUSED:
			errno = ECONNREFUSED;
			break;
		case WSAECONNRESET:
			errno = ECONNRESET;
			break;
		case WSAEINTR:
			errno = EINTR;
			break;
		case WSAENOTSOCK:
			errno = ENOTSOCK;
			break;
		case WSAEOPNOTSUPP:
			errno = EOPNOTSUPP;
			break;
		case WSAEWOULDBLOCK:
			errno = EWOULDBLOCK;
			break;
		case WSAEACCES:
			errno = EACCES;
			break;
		case WSAEADDRINUSE:
			errno = EADDRINUSE;
			break;
		case WSAEADDRNOTAVAIL:
			errno = EADDRNOTAVAIL;
			break;
		case WSAEHOSTDOWN:
			errno = EHOSTDOWN;
			break;
		case WSAEHOSTUNREACH:
		case WSAHOST_NOT_FOUND:
			errno = EHOSTUNREACH;
			break;
		case WSAENETDOWN:
			errno = ENETDOWN;
			break;
		case WSAENETUNREACH:
			errno = ENETUNREACH;
			break;
		case WSAENETRESET:
			errno = ENETRESET;
			break;
		case WSAENOTCONN:
		case WSAESHUTDOWN:
		case WSAEDISCON:
			errno = ENOTCONN;
			break;
		case WSAETIMEDOUT:
			errno = ETIMEDOUT;
			break;
		default:
			ereport(NOTICE,
					(errmsg_internal("unrecognized win32 socket error code: %d",
									 WSAGetLastError())));
			errno = EINVAL;
			break;
	}
}

static int
pgwin32_poll_signals(void)
{
	if (UNBLOCKED_SIGNAL_QUEUE())
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return 1;
	}
	return 0;
}

static int
isDataGram(SOCKET s)
{
	int			type;
	int			typelen = sizeof(type);

	if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *) &type, &typelen))
		return 1;

	return (type == SOCK_DGRAM) ? 1 : 0;
}

int
pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout)
{
	static HANDLE waitevent = INVALID_HANDLE_VALUE;
	static SOCKET current_socket = INVALID_SOCKET;
	static int	isUDP = 0;
	HANDLE		events[2];
	int			r;

	/* 事件对象只创建一次，此后调用都复用 */
	if (waitevent == INVALID_HANDLE_VALUE)
	{
		waitevent = CreateEvent(NULL, TRUE, FALSE, NULL);

		if (waitevent == INVALID_HANDLE_VALUE)
			ereport(ERROR,
					(errmsg_internal("could not create socket waiting event: error code %lu", GetLastError())));
	}
	else if (!ResetEvent(waitevent))
		ereport(ERROR,
				(errmsg_internal("could not reset socket waiting event: error code %lu", GetLastError())));

	/*
	 * 跟踪套接字是否为 UDP。（注意：这多半既无用
	 * 又错误；没有理由认为 WSAEventSelect 对 TCP
	 * 与 UDP 行为不同。
	 */
	if (current_socket != s)
		isUDP = isDataGram(s);
	current_socket = s;

	/*
	 * 附事件到套接字。注意：返回前必须再卸下，
	 * 因其他代码可能把别的事件附到该套接字。
	 *
	 */
	if (WSAEventSelect(s, waitevent, what) != 0)
	{
		TranslateSocketError();
		return 0;
	}

	events[0] = pgwin32_signal_event;
	events[1] = waitevent;

	/*
	 * 高负载下写 UDP 套接字时未知锁问题的变通：
	 * 客户端 pgsql 后端在 WaitForMultipleObjectsEx
	 * 中无限睡眠，pgstat 进程在 pgwin32_select() 中睡眠。
	 * 因此用短超时（0.1 秒）等待；若套接字仍阻塞，
	 * 试 WSASend（见 pgwin32_select 注释）再等。
	 */
	if ((what & FD_WRITE) && isUDP)
	{
		for (;;)
		{
			r = WaitForMultipleObjectsEx(2, events, FALSE, 100, TRUE);

			if (r == WAIT_TIMEOUT)
			{
				char		c;
				WSABUF		buf;
				DWORD		sent;

				buf.buf = &c;
				buf.len = 0;

				r = WSASend(s, &buf, 1, &sent, 0, NULL, NULL);
				if (r == 0)		/* 已完成——表示正常！ */
				{
					WSAEventSelect(s, NULL, 0);
					return 1;
				}
				else if (WSAGetLastError() != WSAEWOULDBLOCK)
				{
					TranslateSocketError();
					WSAEventSelect(s, NULL, 0);
					return 0;
				}
			}
			else
				break;
		}
	}
	else
		r = WaitForMultipleObjectsEx(2, events, FALSE, timeout, TRUE);

	WSAEventSelect(s, NULL, 0);

	if (r == WAIT_OBJECT_0 || r == WAIT_IO_COMPLETION)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return 0;
	}
	if (r == WAIT_OBJECT_0 + 1)
		return 1;
	if (r == WAIT_TIMEOUT)
	{
		errno = EWOULDBLOCK;
		return 0;
	}
	ereport(ERROR,
			(errmsg_internal("unrecognized return value from WaitForMultipleObjects: %d (error code %lu)", r, GetLastError())));
	return 0;
}

/*
 * 创建套接字，设为重叠且非阻塞
 */
SOCKET
pgwin32_socket(int af, int type, int protocol)
{
	SOCKET		s;
	unsigned long on = 1;

	s = WSASocket(af, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (s == INVALID_SOCKET)
	{
		TranslateSocketError();
		return INVALID_SOCKET;
	}

	if (ioctlsocket(s, FIONBIO, &on))
	{
		TranslateSocketError();
		closesocket(s);
		return INVALID_SOCKET;
	}
	errno = 0;

	return s;
}

int
pgwin32_bind(SOCKET s, struct sockaddr *addr, int addrlen)
{
	int			res;

	res = bind(s, addr, addrlen);
	if (res < 0)
		TranslateSocketError();
	return res;
}

int
pgwin32_listen(SOCKET s, int backlog)
{
	int			res;

	res = listen(s, backlog);
	if (res < 0)
		TranslateSocketError();
	return res;
}

SOCKET
pgwin32_accept(SOCKET s, struct sockaddr *addr, int *addrlen)
{
	SOCKET		rs;

	/*
	 * 轮询信号，但不以 EINTR 返回，因 pqcomm.c
	 * 不处理它
	 */
	pgwin32_poll_signals();

	rs = WSAAccept(s, addr, addrlen, NULL, 0);
	if (rs == INVALID_SOCKET)
	{
		TranslateSocketError();
		return INVALID_SOCKET;
	}
	return rs;
}


/* connect 期间不投递信号。 */
int
pgwin32_connect(SOCKET s, const struct sockaddr *addr, int addrlen)
{
	int			r;

	r = WSAConnect(s, addr, addrlen, NULL, NULL, NULL, NULL);
	if (r == 0)
		return 0;

	if (WSAGetLastError() != WSAEWOULDBLOCK)
	{
		TranslateSocketError();
		return -1;
	}

	while (pgwin32_waitforsinglesocket(s, FD_CONNECT, INFINITE) == 0)
	{
		/* 只要只是在投递信号就无限循环 */
	}

	return 0;
}

int
pgwin32_recv(SOCKET s, char *buf, int len, int f)
{
	WSABUF		wbuf;
	int			r;
	DWORD		b;
	DWORD		flags = f;
	int			n;

	if (pgwin32_poll_signals())
		return -1;

	wbuf.len = len;
	wbuf.buf = buf;

	r = WSARecv(s, &wbuf, 1, &b, &flags, NULL, NULL);
	if (r != SOCKET_ERROR)
		return b;				/* 成功 */

	if (WSAGetLastError() != WSAEWOULDBLOCK)
	{
		TranslateSocketError();
		return -1;
	}

	if (pgwin32_noblock)
	{
		/*
		 * 未收到数据，且处于"仿真非阻塞模式"，故
		 * 返回表示若继续则会阻塞。
		 */
		errno = EWOULDBLOCK;
		return -1;
	}

	/* 阻塞模式，故等待数据 */

	for (n = 0; n < 5; n++)
	{
		if (pgwin32_waitforsinglesocket(s, FD_READ | FD_CLOSE | FD_ACCEPT,
										INFINITE) == 0)
			return -1;			/* errno 已设置 */

		r = WSARecv(s, &wbuf, 1, &b, &flags, NULL, NULL);
		if (r != SOCKET_ERROR)
			return b;			/* 成功 */
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			TranslateSocketError();
			return -1;
		}

		/*
		 * 至少在 win2k 上，即便 pgwin32_waitforsinglesocket
		 * 声称套接字可读，WSARecv 仍可能返回
		 * WSAEWOULDBLOCK。此时稍睡片刻再试。
		 * 最多试 5 次——失败更多则不太可能再
		 * 恢复。
		 */
		pg_usleep(10000);
	}
	ereport(NOTICE,
			(errmsg_internal("could not read from ready socket (after retries)")));
	errno = EWOULDBLOCK;
	return -1;
}

/*
 * SUS 把 send() 第二参定义为 "const void *"，
 * 此处用相同签名，以便处理调用方时编译器
 * 保持安静。
 *
 * 但 WSABUF 的 buf 成员定义为 "char *"，故赋值时
 * 把第二参转成该类型，同样为让编译器安静。
 *
 */

int
pgwin32_send(SOCKET s, const void *buf, int len, int flags)
{
	WSABUF		wbuf;
	int			r;
	DWORD		b;

	if (pgwin32_poll_signals())
		return -1;

	wbuf.len = len;
	wbuf.buf = (char *) buf;

	/*
	 * UDP 套接字可发送的就绪可能不真：套接字可能
	 * 又变忙！故循环直至发送成功或出错。
	 */
	for (;;)
	{
		r = WSASend(s, &wbuf, 1, &b, flags, NULL, NULL);
		if (r != SOCKET_ERROR && b > 0)
			/* 写入立刻成功 */
			return b;

		if (r == SOCKET_ERROR &&
			WSAGetLastError() != WSAEWOULDBLOCK)
		{
			TranslateSocketError();
			return -1;
		}

		if (pgwin32_noblock)
		{
			/*
			 * 未发送数据，且处于"仿真非阻塞模式"，故
			 * 返回表示若继续则会阻塞。
			 */
			errno = EWOULDBLOCK;
			return -1;
		}

		/* 无错，零字节 */

		if (pgwin32_waitforsinglesocket(s, FD_WRITE | FD_CLOSE, INFINITE) == 0)
			return -1;
	}

	return -1;
}


/*
 * 等待一个或多个套接字活动。
 * 等待时允许信号运行
 *
 * 注意！当前未实现 exceptfds 检查，
 * 因 postgresql 未使用它！
 */
int
pgwin32_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout)
{
	WSAEVENT	events[FD_SETSIZE * 2]; /* worst case is readfds totally
										 * different from writefds, so
										 * 2*FD_SETSIZE sockets */
	SOCKET		sockets[FD_SETSIZE * 2];
	int			numevents = 0;
	int			i;
	int			r;
	DWORD		timeoutval = WSA_INFINITE;
	FD_SET		outreadfds;
	FD_SET		outwritefds;
	int			nummatches = 0;

	Assert(exceptfds == NULL);

	if (pgwin32_poll_signals())
		return -1;

	FD_ZERO(&outreadfds);
	FD_ZERO(&outwritefds);

	/*
	 * Windows 不保证记录 FD_WRITE 网络事件以表示
	 * 可再发送，除非先前 send() 以 WSAEWOULDBLOCK
	 * 失败。调用方或许做过这种调用，但此处不能
	 * 假定。因此若等待写就绪，用哑 send() 强制。
	 * 若哑 send() 成功，则假定套接字确实写就绪并
	 * 立即返回。若失败且原因不是 WSAEWOULDBLOCK，
	 * 也返回写就绪指示，让调用方处理该
	 * 错误条件。
	 */
	if (writefds != NULL)
	{
		for (i = 0; i < writefds->fd_count; i++)
		{
			char		c;
			WSABUF		buf;
			DWORD		sent;

			buf.buf = &c;
			buf.len = 0;

			r = WSASend(writefds->fd_array[i], &buf, 1, &sent, 0, NULL, NULL);
			if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
				FD_SET(writefds->fd_array[i], &outwritefds);
		}

		/* 若找到任何写就绪套接字，立即返回它们 */
		if (outwritefds.fd_count > 0)
		{
			memcpy(writefds, &outwritefds, sizeof(fd_set));
			if (readfds)
				FD_ZERO(readfds);
			return outwritefds.fd_count;
		}
	}


	/* 现在设置实际的 select */

	if (timeout != NULL)
	{
		/* timeoutval 以毫秒计 */
		timeoutval = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
	}

	if (readfds != NULL)
	{
		for (i = 0; i < readfds->fd_count; i++)
		{
			events[numevents] = WSACreateEvent();
			sockets[numevents] = readfds->fd_array[i];
			numevents++;
		}
	}
	if (writefds != NULL)
	{
		for (i = 0; i < writefds->fd_count; i++)
		{
			if (!readfds ||
				!FD_ISSET(writefds->fd_array[i], readfds))
			{
				/* 若套接字不在读列表中 */
				events[numevents] = WSACreateEvent();
				sockets[numevents] = writefds->fd_array[i];
				numevents++;
			}
		}
	}

	for (i = 0; i < numevents; i++)
	{
		int			flags = 0;

		if (readfds && FD_ISSET(sockets[i], readfds))
			flags |= FD_READ | FD_ACCEPT | FD_CLOSE;

		if (writefds && FD_ISSET(sockets[i], writefds))
			flags |= FD_WRITE | FD_CLOSE;

		if (WSAEventSelect(sockets[i], events[i], flags) != 0)
		{
			TranslateSocketError();
			/* 释放已分配的事件对象 */
			while (--i >= 0)
				WSAEventSelect(sockets[i], NULL, 0);
			for (i = 0; i < numevents; i++)
				WSACloseEvent(events[i]);
			return -1;
		}
	}

	events[numevents] = pgwin32_signal_event;
	r = WaitForMultipleObjectsEx(numevents + 1, events, FALSE, timeoutval, TRUE);
	if (r != WAIT_TIMEOUT && r != WAIT_IO_COMPLETION && r != (WAIT_OBJECT_0 + numevents))
	{
		/*
		 * 扫描全部事件，包括未发信号的，以防多个
		 * 事件已标记而 Wait.. 只能返回一个。
		 */
		WSANETWORKEVENTS resEvents;

		for (i = 0; i < numevents; i++)
		{
			ZeroMemory(&resEvents, sizeof(resEvents));
			if (WSAEnumNetworkEvents(sockets[i], events[i], &resEvents) != 0)
				elog(ERROR, "failed to enumerate network events: error code %d",
					 WSAGetLastError());
			/* 读活动？ */
			if (readfds && FD_ISSET(sockets[i], readfds))
			{
				if ((resEvents.lNetworkEvents & FD_READ) ||
					(resEvents.lNetworkEvents & FD_ACCEPT) ||
					(resEvents.lNetworkEvents & FD_CLOSE))
				{
					FD_SET(sockets[i], &outreadfds);

					nummatches++;
				}
			}
			/* 写活动？ */
			if (writefds && FD_ISSET(sockets[i], writefds))
			{
				if ((resEvents.lNetworkEvents & FD_WRITE) ||
					(resEvents.lNetworkEvents & FD_CLOSE))
				{
					FD_SET(sockets[i], &outwritefds);

					nummatches++;
				}
			}
		}
	}

	/* 清理全部事件对象 */
	for (i = 0; i < numevents; i++)
	{
		WSAEventSelect(sockets[i], NULL, 0);
		WSACloseEvent(events[i]);
	}

	if (r == WSA_WAIT_TIMEOUT)
	{
		if (readfds)
			FD_ZERO(readfds);
		if (writefds)
			FD_ZERO(writefds);
		return 0;
	}

	/* 类信号事件。 */
	if (r == WAIT_OBJECT_0 + numevents || r == WAIT_IO_COMPLETION)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		if (readfds)
			FD_ZERO(readfds);
		if (writefds)
			FD_ZERO(writefds);
		return -1;
	}

	/* 用结果值覆盖套接字集合 */
	if (readfds)
		memcpy(readfds, &outreadfds, sizeof(fd_set));
	if (writefds)
		memcpy(writefds, &outwritefds, sizeof(fd_set));
	return nummatches;
}
