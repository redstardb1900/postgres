/*-------------------------------------------------------------------------
 *
 *	 FILE
 *		fe-misc.c
 *
 *	 DESCRIPTION
 *		 miscellaneous useful functions
 *
 * The communication routines here are analogous to the ones in
 * backend/libpq/pqcomm.c and backend/libpq/pqformat.c, but operate
 * in the considerably different environment of the frontend libpq.
 * In particular, we work with a bare nonblock-mode socket, rather than
 * a stdio stream, so that we can avoid unwanted blocking of the application.
 *
 * XXX: MOVE DEBUG PRINTOUT TO HIGHER LEVEL.  As is, block and restart
 * will cause repeat printouts.
 *
 * We must speak the same transmitted data representations as the backend
 * routines.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-misc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <time.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"
#include "pg_config_paths.h"
#include "port/pg_bswap.h"

static int	pqPutMsgBytes(const void *buf, size_t len, PGconn *conn);
static int	pqSendSome(PGconn *conn, int len);
static int	pqSocketCheck(PGconn *conn, int forRead, int forWrite,
						  time_t end_time);
static int	pqSocketPoll(int sock, int forRead, int forWrite, time_t end_time);

/*
 * PQlibVersion: 返回 libpq 版本号
 */
int
PQlibVersion(void)
{
	return PG_VERSION_NUM;
}


/*
 * pqGetc: 从连接获取 1 个字符
 *
 *	成功返回 0，错误返回 EOF。
 *	注意：Get 例程返回 EOF 仅表示缓冲区数据
 *	不足，不一定表示硬错误。
 */
int
pqGetc(char *result, PGconn *conn)
{
	if (conn->inCursor >= conn->inEnd)
		return EOF;

	*result = conn->inBuffer[conn->inCursor++];

	return 0;
}


/*
 * pqPutc: 向当前消息写入 1 个字符
 */
int
pqPutc(char c, PGconn *conn)
{
	if (pqPutMsgBytes(&c, 1, conn))
		return EOF;

	return 0;
}


/*
 * pqGets[_append]:
 * 从连接获取以空字符结尾的字符串，
 * 并存入可扩展的 PQExpBuffer。
 * 若内存耗尽，字符串仍会被完整读取，
 * 但超出部分被静默丢弃。
 */
static int
pqGets_internal(PQExpBuffer buf, PGconn *conn, bool resetbuffer)
{
	/* 将连接数据复制到局部变量以加速搜索 */
	char	   *inBuffer = conn->inBuffer;
	int			inCursor = conn->inCursor;
	int			inEnd = conn->inEnd;
	int			slen;

	while (inCursor < inEnd && inBuffer[inCursor])
		inCursor++;

	if (inCursor >= inEnd)
		return EOF;

	slen = inCursor - conn->inCursor;

	if (resetbuffer)
		resetPQExpBuffer(buf);

	appendBinaryPQExpBuffer(buf, inBuffer + conn->inCursor, slen);

	conn->inCursor = ++inCursor;

	return 0;
}

int
pqGets(PQExpBuffer buf, PGconn *conn)
{
	return pqGets_internal(buf, conn, true);
}

int
pqGets_append(PQExpBuffer buf, PGconn *conn)
{
	return pqGets_internal(buf, conn, false);
}


/*
 * pqPuts: 向当前消息写入以空字符结尾的字符串
 */
int
pqPuts(const char *s, PGconn *conn)
{
	if (pqPutMsgBytes(s, strlen(s) + 1, conn))
		return EOF;

	return 0;
}

/*
 * pqGetnchar:
 *	从缓冲区 s 获取恰好 len 字节，不追加空终止符
 */
int
pqGetnchar(char *s, size_t len, PGconn *conn)
{
	if (len > (size_t) (conn->inEnd - conn->inCursor))
		return EOF;

	memcpy(s, conn->inBuffer + conn->inCursor, len);
	/* 不追加空终止符 */

	conn->inCursor += len;

	return 0;
}

/*
 * pqSkipnchar:
 *	跳过输入缓冲区中 len 字节。
 *
 * 主要用途：其调试输出应与 pqGetnchar 完全相同。
 * 我们假设这些数据最终会被使用，只是尚未复制到任何地方。
 *
 */
int
pqSkipnchar(size_t len, PGconn *conn)
{
	if (len > (size_t) (conn->inEnd - conn->inCursor))
		return EOF;

	conn->inCursor += len;

	return 0;
}

/*
 * pqPutnchar:
 *	向当前消息写入恰好 len 字节
 */
int
pqPutnchar(const char *s, size_t len, PGconn *conn)
{
	if (pqPutMsgBytes(s, len, conn))
		return EOF;

	return 0;
}

/*
 * pqGetInt
 *	读取 2 或 4 字节整数，从网络字节序
 *	转换为本地字节序
 */
int
pqGetInt(int *result, size_t bytes, PGconn *conn)
{
	uint16		tmp2;
	uint32		tmp4;

	switch (bytes)
	{
		case 2:
			if (conn->inCursor + 2 > conn->inEnd)
				return EOF;
			memcpy(&tmp2, conn->inBuffer + conn->inCursor, 2);
			conn->inCursor += 2;
			*result = (int) pg_ntoh16(tmp2);
			break;
		case 4:
			if (conn->inCursor + 4 > conn->inEnd)
				return EOF;
			memcpy(&tmp4, conn->inBuffer + conn->inCursor, 4);
			conn->inCursor += 4;
			*result = (int) pg_ntoh32(tmp4);
			break;
		default:
			pqInternalNotice(&conn->noticeHooks,
							 "integer of size %lu not supported by pqGetInt",
							 (unsigned long) bytes);
			return EOF;
	}

	return 0;
}

/*
 * pqPutInt
 * 写入 2 或 4 字节整数，从主机字节序
 * 转换为网络字节序。
 */
int
pqPutInt(int value, size_t bytes, PGconn *conn)
{
	uint16		tmp2;
	uint32		tmp4;

	switch (bytes)
	{
		case 2:
			tmp2 = pg_hton16((uint16) value);
			if (pqPutMsgBytes((const char *) &tmp2, 2, conn))
				return EOF;
			break;
		case 4:
			tmp4 = pg_hton32((uint32) value);
			if (pqPutMsgBytes((const char *) &tmp4, 4, conn))
				return EOF;
			break;
		default:
			pqInternalNotice(&conn->noticeHooks,
							 "integer of size %lu not supported by pqPutInt",
							 (unsigned long) bytes);
			return EOF;
	}

	return 0;
}

/*
 * 确保 conn 输出缓冲区能容纳 bytes_needed 字节
 * （调用者须将已存数据计入该值！）
 *
 * 成功返回 0，扩缓冲区失败返回 EOF
 */
int
pqCheckOutBufferSpace(size_t bytes_needed, PGconn *conn)
{
	int			newsize = conn->outBufSize;
	char	   *newbuf;

	/* 空间足够则快速退出 */
	if (bytes_needed <= (size_t) newsize)
		return 0;

	/*
	 * 需扩缓冲区时先尝试翻倍；若不行则以 8K 倍数扩。
	 * 这避免了反复小幅扩大导致的 malloc 池抖动。
	 *
	 * 注意：newsize > 0 检查用于捕获整数溢出。
	 *
	 */
	do
	{
		newsize *= 2;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = realloc(conn->outBuffer, newsize);
		if (newbuf)
		{
			/* realloc 成功 */
			conn->outBuffer = newbuf;
			conn->outBufSize = newsize;
			return 0;
		}
	}

	newsize = conn->outBufSize;
	do
	{
		newsize += 8192;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = realloc(conn->outBuffer, newsize);
		if (newbuf)
		{
			/* realloc 成功 */
			conn->outBuffer = newbuf;
			conn->outBufSize = newsize;
			return 0;
		}
	}

	/* realloc 失败。可能内存不足 */
	appendPQExpBufferStr(&conn->errorMessage,
						 "cannot allocate memory for output buffer\n");
	return EOF;
}

/*
 * 确保 conn 输入缓冲区能容纳 bytes_needed 字节
 * （调用者须将已存数据计入该值！）
 *
 * 成功返回 0，扩缓冲区失败返回 EOF
 */
int
pqCheckInBufferSpace(size_t bytes_needed, PGconn *conn)
{
	int			newsize = conn->inBufSize;
	char	   *newbuf;

	/* 空间足够则快速退出 */
	if (bytes_needed <= (size_t) newsize)
		return 0;

	/*
	 * 在确定需要扩大缓冲区之前，先左对齐缓冲区内容并
	 * 重新检查。调用者的 bytes_needed 包含 inStart 左侧的数据，
	 * 但我们可以优先删除这些而非扩大缓冲区。
	 * 让这个函数来做这件事虽然不太优雅，但比让
	 * 调用者自己操心要好。
	 */
	bytes_needed -= conn->inStart;

	if (conn->inStart < conn->inEnd)
	{
		if (conn->inStart > 0)
		{
			memmove(conn->inBuffer, conn->inBuffer + conn->inStart,
					conn->inEnd - conn->inStart);
			conn->inEnd -= conn->inStart;
			conn->inCursor -= conn->inStart;
			conn->inStart = 0;
		}
	}
	else
	{
		/* 缓冲区逻辑为空，重置 */
		conn->inStart = conn->inCursor = conn->inEnd = 0;
	}

	/* 重新检查空间是否足够 */
	if (bytes_needed <= (size_t) newsize)
		return 0;

	/*
	 * 需扩缓冲区时先尝试翻倍；若不行则以 8K 倍数扩。
	 * 这避免了反复小幅扩大导致的 malloc 池抖动。
	 *
	 * 注意：newsize > 0 检查用于捕获整数溢出。
	 *
	 */
	do
	{
		newsize *= 2;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = realloc(conn->inBuffer, newsize);
		if (newbuf)
		{
			/* realloc 成功 */
			conn->inBuffer = newbuf;
			conn->inBufSize = newsize;
			return 0;
		}
	}

	newsize = conn->inBufSize;
	do
	{
		newsize += 8192;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = realloc(conn->inBuffer, newsize);
		if (newbuf)
		{
			/* realloc 成功 */
			conn->inBuffer = newbuf;
			conn->inBufSize = newsize;
			return 0;
		}
	}

	/* realloc 失败。可能内存不足 */
	appendPQExpBufferStr(&conn->errorMessage,
						 "cannot allocate memory for input buffer\n");
	return EOF;
}

/*
 * pqPutMsgStart: 开始构造发往服务器的消息
 *
 * msg_type 是消息类型字节，无类型字节时取 0
 * （仅启动消息没有类型字节）
 *
 * 成功返回 0，错误返回 EOF
 *
 * 基本意图：在 conn->outBuffer 中构造消息，
 * 从 outBuffer 已有数据之后开始（即 outBuffer+outCount 处）。
 * 缓冲区会根据需要扩大以容纳消息。
 * 消息构造完成后填入长度字（如需要），然后将 outCount
 * 推进到消息末尾，使其可以发送。
 *
 * 状态变量 conn->outMsgStart 指向未完成消息的长度字位置：
 * 取决于是否有类型字节，为 outCount 或 outCount+1。
 * 状态变量 conn->outMsgEnd 是
 * 目前已收集数据的末尾。
 */
int
pqPutMsgStart(char msg_type, PGconn *conn)
{
	int			lenPos;
	int			endPos;

	/* 为消息类型字节预留空间 */
	if (msg_type)
		endPos = conn->outCount + 1;
	else
		endPos = conn->outCount;

	/* 是否需要长度字？ */
	lenPos = endPos;
	/* 为消息长度预留空间 */
	endPos += 4;

	/* 确保消息头有足够空间 */
	if (pqCheckOutBufferSpace(endPos, conn))
		return EOF;
	/* 如果有类型字节则保存 */
	if (msg_type)
		conn->outBuffer[conn->outCount] = msg_type;
	/* 设置消息指针 */
	conn->outMsgStart = lenPos;
	conn->outMsgEnd = endPos;
	/* 长度字（如需要）由 pqPutMsgEnd 填入 */

	return 0;
}

/*
 * pqPutMsgBytes: 向未完成的消息追加字节
 *
 * 成功返回 0，错误返回 EOF
 */
static int
pqPutMsgBytes(const void *buf, size_t len, PGconn *conn)
{
	/* 确保有足够空间 */
	if (pqCheckOutBufferSpace(conn->outMsgEnd + len, conn))
		return EOF;
	/* 保存数据 */
	memcpy(conn->outBuffer + conn->outMsgEnd, buf, len);
	conn->outMsgEnd += len;
	/* 此处不调用 Pfdebug，由调用者负责 */
	return 0;
}

/*
 * pqPutMsgEnd: 完成构造消息并可能发送
 *
 * 成功返回 0，错误返回 EOF
 *
 * 我们在这里不会立即发送，除非已累积至少 8K
 * 的数据（Unix 系统管道缓冲区的典型大小）。
 * 这避免了发送小部分包。调用者必须使用 pqFlush
 * 当需要确保所有数据都发送到服务器时。
 */
int
pqPutMsgEnd(PGconn *conn)
{
	/* 如需要则填入长度字 */
	if (conn->outMsgStart >= 0)
	{
		uint32		msgLen = conn->outMsgEnd - conn->outMsgStart;

		msgLen = pg_hton32(msgLen);
		memcpy(conn->outBuffer + conn->outMsgStart, &msgLen, 4);
	}

	/* 跟踪客户端到服务器的消息 */
	if (conn->Pfdebug)
	{
		if (conn->outCount < conn->outMsgStart)
			pqTraceOutputMessage(conn, conn->outBuffer + conn->outCount, true);
		else
			pqTraceOutputNoTypeByteMessage(conn,
										   conn->outBuffer + conn->outMsgStart);
	}

	/* 使消息可以发送 */
	conn->outCount = conn->outMsgEnd;

	/* 如适当，尝试推送一些数据 */
	if (conn->outCount >= 8192)
	{
		int			toSend = conn->outCount;

		/*
		 * 在 Unix 管道连接上，似乎更有利于发送
		 * 管道缓冲区大小的包而非随机大小的包，因此
		 * 暂时保留最后一个不完整的 8K 块在缓冲区中。在 TCP 连接上，
		 * 这种做法的优势远不那么明显。此外，在使用
		 * SSL 或 GSSAPI 时这样做绝对不安全，因为那些代码路径的
		 * API 规定，如果上次写操作未能发送所有提供的数据，
		 * 我们就不能在本次写操作中提供更少的数据。
		 * 上次写操作可能是 pqFlush 尝试发送缓冲区中的所有内容，
		 * 所以我们现在不能提供更少。（目前，我们不会在
		 * Unix 连接上尝试使用 SSL 或 GSSAPI，所以这些检查只是
		 * 断言。如果将来改变这一点，它们将需要成为
		 * 常规 if 测试的一部分。）
		 */
		if (conn->raddr.addr.ss_family == AF_UNIX)
		{
#ifdef USE_SSL
			Assert(!conn->ssl_in_use);
#endif
#ifdef ENABLE_GSS
			Assert(!conn->gssenc);
#endif
			toSend -= toSend % 8192;
		}

		if (pqSendSome(conn, toSend) < 0)
			return EOF;
		/* 非阻塞模式下，无法全部发送也不抱怨 */
	}

	return 0;
}

/* ----------
 * pqReadData: 读取更多数据（如果有可用的话）
 * 可能的返回值：
 *	 1: 成功加载了至少一个字节
 *	 0: 当前没有可用数据，但未检测到错误
 *	-1: 检测到错误（包括 EOF = 连接关闭）；
 *		conn->errorMessage 已设置
 * 注意：调用者不能假定 conn->inBuffer 中的指针或
 * 索引在此调用之后仍然有效！
 * ----------
 */
int
pqReadData(PGconn *conn)
{
	int			someread = 0;
	int			nread;

	if (conn->sock == PGINVALID_SOCKET)
	{
		libpq_append_conn_error(conn, "connection not open");
		return -1;
	}

	/* 将缓冲区中的数据左对齐以腾出空间 */
	if (conn->inStart < conn->inEnd)
	{
		if (conn->inStart > 0)
		{
			memmove(conn->inBuffer, conn->inBuffer + conn->inStart,
					conn->inEnd - conn->inStart);
			conn->inEnd -= conn->inStart;
			conn->inCursor -= conn->inStart;
			conn->inStart = 0;
		}
	}
	else
	{
		/* 缓冲区逻辑为空，重置 */
		conn->inStart = conn->inCursor = conn->inEnd = 0;
	}

	/*
	 * 如果缓冲区基本满了，扩大它。我们需要能够扩大
	 * 缓冲区，以便在单个消息超过初始缓冲区大小时能处理。我们
	 * 在完全填满缓冲区之前扩大，以避免向内核
	 * 请求不完整的包。这里的魔术常量应足够大以容纳
	 * 一个 TCP 包或 Unix 管道缓冲区负载。8K 是通常的管道
	 * 缓冲区大小，所以…
	 */
	if (conn->inBufSize - conn->inEnd < 8192)
	{
		if (pqCheckInBufferSpace(conn->inEnd + (size_t) 8192, conn))
		{
			/*
			 * 我们不坚持扩大必须成功，但我们需要一些空间
			 */
			if (conn->inBufSize - conn->inEnd < 100)
				return -1;		/* errorMessage 已设置 */
		}
	}

	/* 好，尝试读取一些数据 */
retry3:
	nread = pqsecure_read(conn, conn->inBuffer + conn->inEnd,
						  conn->inBufSize - conn->inEnd);
	if (nread < 0)
	{
		switch (SOCK_ERRNO)
		{
			case EINTR:
				goto retry3;

				/* 某些系统对无数据返回 EAGAIN/EWOULDBLOCK */
#ifdef EAGAIN
			case EAGAIN:
				return someread;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
				return someread;
#endif

				/* 连接失败时可能收到 ECONNRESET 等 */
			case ALL_CONNECTION_FAILURE_ERRNOS:
				goto definitelyFailed;

			default:
				/* pqsecure_read 已为我们设置错误消息 */
				return -1;
		}
	}
	if (nread > 0)
	{
		conn->inEnd += nread;

		/*
		 * 用于处理某些内核每次 recv() 调用
		 * 只返回 1 个包的问题，即使我们请求更多且更多内容
		 * 可用。如果看起来我们正在读取一个长消息，
		 * 立即回到 recv() 再次循环，直到数据或缓冲区
		 * 空间耗尽为止。没有这个处理，libpq 高层的
		 * 阻塞和重启行为会导致长消息的 O(N^2) 性能问题。
		 *
		 * 由于我们在上面左对齐了数据，conn->inEnd 给出了
		 * 当前消息中已读取的数据量。我们认为
		 * 获得 32k 之后消息就是"长"的…
		 */
		if (conn->inEnd > 32768 &&
			(conn->inBufSize - conn->inEnd) >= 8192)
		{
			someread = 1;
			goto retry3;
		}
		return 1;
	}

	if (someread)
		return 1;				/* 成功尝试后读取到零字节 */

	/*
	 * 返回值 0 可能仅表示当前没有可用数据，也
	 * 可能表示 EOF ——即服务器已关闭连接。
	 * 由于我们将套接字设为非阻塞模式，区别两者的唯一方法
	 * 是看 select() 是否表示文件已经可读。
	 * 啧。幸运的是，我们不期望这条路径被频繁使用，
	 * 因为在正常实践中，我们应该不会尝试读取数据，
	 * 除非文件已经选择为可读。
	 *
	 * 在 SSL 模式下更糟：SSL_read() 可能说 WANT_READ，
	 * 然后数据可能在我们执行 pqReadReady() 测试之前到达，
	 * 但第二次 SSL_read() 仍可能说 WANT_READ，因为收到的数据
	 * 不是完整的 SSL 记录。因此我们必须装傻并假设有更多
	 * 数据，依赖 SSL 层来检测真正的 EOF。
	 */

#ifdef USE_SSL
	if (conn->ssl_in_use)
		return 0;
#endif

	switch (pqReadReady(conn))
	{
		case 0:
			/* 确定没有可用数据 */
			return 0;
		case 1:
			/* 可以读取 */
			break;
		default:
			/* 我们用更有用的消息覆盖 pqReadReady 的消息 */
			goto definitelyEOF;
	}

	/*
	 * 仍不确定是 EOF，因为可能刚有一些数据
	 * 到达。
	 */
retry4:
	nread = pqsecure_read(conn, conn->inBuffer + conn->inEnd,
						  conn->inBufSize - conn->inEnd);
	if (nread < 0)
	{
		switch (SOCK_ERRNO)
		{
			case EINTR:
				goto retry4;

				/* 某些系统对无数据返回 EAGAIN/EWOULDBLOCK */
#ifdef EAGAIN
			case EAGAIN:
				return 0;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
				return 0;
#endif

				/* 连接失败时可能收到 ECONNRESET 等 */
			case ALL_CONNECTION_FAILURE_ERRNOS:
				goto definitelyFailed;

			default:
				/* pqsecure_read 已为我们设置错误消息 */
				return -1;
		}
	}
	if (nread > 0)
	{
		conn->inEnd += nread;
		return 1;
	}

	/*
	 * 好吧，即使 select() 说就绪，我们也收到了零字节读取。
	 * 这意味着连接已被关闭。勉为其难吧。
	 */
definitelyEOF:
	libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
							"\tThis probably means the server terminated abnormally\n"
							"\tbefore or while processing the request.");

	/* 如果低层代码已设置了合适的 errorMessage，到这里来 */
definitelyFailed:
	/* 千万不要丢弃已读取的数据；调用者仍需要它 */
	pqDropConnection(conn, false);
	conn->status = CONNECTION_BAD;	/* 不再有到后端的连接 */
	return -1;
}

/*
 * pqSendSome: 发送输出缓冲区中等待的数据。
 *
 * len 是尝试发送的量（通常等于 outCount，但可能
 * 更少）。
 *
 * 成功返回 0，失败返回 -1，当套接字会阻塞且
 * 连接为非阻塞模式时无法发送所有数据则返回 1。
 *
 * 注意：这个函数还负责在无法立即发送所有
 * 指定数据的情况下从套接字消耗数据
 * （将其放入 conn->inBuffer）。
 *
 * 如果发生套接字级别的写失败，设置 conn->write_failed
 * 并将错误消息保存在 conn->write_err_msg 中，但仍然
 * 清除输出缓冲区并返回零；这是因为调用者应该
 * 坚持读取服务器发来的内容并检查错误消息。
 * 只有在无法先获取服务器错误时才应报告
 * write_err_msg。这些行为的大部分在更低层实现，
 * 但此函数处理一些边界情况。
 */
static int
pqSendSome(PGconn *conn, int len)
{
	char	   *ptr = conn->outBuffer;
	int			remaining = conn->outCount;
	int			result = 0;

	/*
	 * 如果已经有写失败，我们将不会再尝试在该连接上
	 * 发送数据。即使内核允许，我们可能也已经
	 * 失去了与服务器的消息边界同步。因此
	 * conn->write_failed 将持续到连接被重置为止，
	 * 我们只是丢弃所有提交给写入的数据。
	 * 然而，只要还有有效的套接字，我们应该继续吸收
	 * 后端发来的数据，以便收集任何最终的错误消息。
	 */
	if (conn->write_failed)
	{
		/* conn->write_err_msg 应已设置好 */
		conn->outCount = 0;
		/* 吸收输入数据（如果有的话），并检测套接字关闭 */
		if (conn->sock != PGINVALID_SOCKET)
		{
			if (pqReadData(conn) < 0)
				return -1;
		}
		return 0;
	}

	if (conn->sock == PGINVALID_SOCKET)
	{
		conn->write_failed = true;
		/* 将错误消息存储到 conn->write_err_msg，如果可能的话 */
		/* （strdup 失败没关系，我们稍后处理） */
		conn->write_err_msg = strdup(libpq_gettext("connection not open\n"));
		/* 丢弃队列中的数据；没有机会发送它们了 */
		conn->outCount = 0;
		return 0;
	}

	/* 当还有数据要发送时 */
	while (len > 0)
	{
		int			sent;

#ifndef WIN32
		sent = pqsecure_write(conn, ptr, len);
#else

		/*
		 * Windows 在大数据发送时可能失败，参见 KB 文章 Q201213。
		 * 失败点在不同版本的 Windows 中似乎不同，但
		 * 64k 应始终是安全的。
		 */
		sent = pqsecure_write(conn, ptr, Min(len, 65536));
#endif

		if (sent < 0)
		{
			/* 除 EAGAIN/EWOULDBLOCK/EINTR 之外都是问题 */
			switch (SOCK_ERRNO)
			{
#ifdef EAGAIN
				case EAGAIN:
					break;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
					break;
#endif
				case EINTR:
					continue;

				default:
					/* 丢弃队列中的数据；没有机会发送它们了 */
					conn->outCount = 0;

					/* 吸收输入数据（如果有的话），并检测套接字关闭 */
					if (conn->sock != PGINVALID_SOCKET)
					{
						if (pqReadData(conn) < 0)
							return -1;
					}

					/*
				 * 低层代码应已填充
				 * conn->write_err_msg（并设置 conn->write_failed）或
				 * conn->errorMessage。在前者情况下，我们假装
				 * 没有问题；write_failed 条件将在之后处理。
				 * 否则，现在报告错误。
				 */
					if (conn->write_failed)
						return 0;
					else
						return -1;
			}
		}
		else
		{
			ptr += sent;
			len -= sent;
			remaining -= sent;
		}

		if (len > 0)
		{
			/*
			 * 我们没有全部发送，等待能够发送更多。
			 *
			 * 有些情况下我们无法发送数据，因为通信信道已满，
			 * 但我们不能期望服务器最终清空信道，因为它正
			 * 被阻塞在试图向我们发送数据中。（这可能发生在
			 * 我们正在发送大量 COPY 数据时，服务器已经生成了
			 * 大量 NOTICE 响应。）为了避免死锁情况，我们必须准备
			 * 在再次尝试之前接受并缓冲进来的数据。
			 * 此外，这些进来的数据可能在我们已经睡眠后才到达。
			 * 因此，我们等待读就绪或写就绪。
			 *
			 * 在非阻塞模式下，我们不在这里直接等待，而是返回 1
			 * 表示数据仍在等待。调用者应该等待读和写就绪条件，
			 * 并在读就绪时调用 PQconsumeInput()，但为了以防万一，
			 * 我们在返回之前自己调用 pqReadData()。
			 * 如果数据还没有到达，这不够，但已经是我们能做的
			 * 最好的了，并且在实践中效果很好。（文档过去
			 * 说只需要等待写就绪，所以仍有很ve多
			 * 应用程序是这样做的。）
			 *
			 * 注意：此处的错误不会导致 write_failed
			 * 被设置。
			 *
			 *
			 *
			 */
			if (pqReadData(conn) < 0)
			{
				result = -1;	/* 错误消息已设置 */
				break;
			}

			if (pqIsnonblocking(conn))
			{
				result = 1;
				break;
			}

			if (pqWait(true, true, conn))
			{
				result = -1;
				break;
			}
		}
	}

	/* 移动缓冲区剩余内容 */
	if (remaining > 0)
		memmove(conn->outBuffer, ptr, remaining);
	conn->outCount = remaining;

	return result;
}


/*
 * pqFlush: 发送输出缓冲区中等待的任何数据
 *
 * 成功返回 0，失败返回 -1，当套接字会阻塞且
 * 连接为非阻塞模式时无法发送所有数据则返回 1。
 * （参见 pqSendSome 关于如何处理失败的注释。）
 */
int
pqFlush(PGconn *conn)
{
	if (conn->outCount > 0)
	{
		if (conn->Pfdebug)
			fflush(conn->Pfdebug);

		return pqSendSome(conn, conn->outCount);
	}

	return 0;
}


/*
 * pqWait: 等待直到可以读取或写入连接套接字
 *
 * JAB: 如果启用且使用了 SSL，并为读取目的，
 * 缓冲的字节会短路 select() 的调用。
 *
 * 如果内核在套接字上标记了异常条件，我们也停止等待并返回。
 * 实际的错误条件将在调用者尝试读取或写入套接字时
 * 被检测和报告。
 */
int
pqWait(int forRead, int forWrite, PGconn *conn)
{
	return pqWaitTimed(forRead, forWrite, conn, (time_t) -1);
}

/*
 * pqWaitTimed: 等待，但不超过 finish_time。
 *
 * finish_time = ((time_t) -1) 禁用等待限制。
 *
 * 失败返回 -1，套接字可读/可写时返回 0，超时返回 1。
 */
int
pqWaitTimed(int forRead, int forWrite, PGconn *conn, time_t finish_time)
{
	int			result;

	result = pqSocketCheck(conn, forRead, forWrite, finish_time);

	if (result < 0)
		return -1;				/* errorMessage 已设置 */

	if (result == 0)
	{
		libpq_append_conn_error(conn, "timeout expired");
		return 1;
	}

	return 0;
}

/*
 * pqReadReady: select() 是否表示文件已可读？
 * 失败返回 -1，未就绪返回 0，就绪返回 1。
 */
int
pqReadReady(PGconn *conn)
{
	return pqSocketCheck(conn, 1, 0, (time_t) 0);
}

/*
 * pqWriteReady: select() 是否表示文件已可写？
 * 失败返回 -1，未就绪返回 0，就绪返回 1。
 */
int
pqWriteReady(PGconn *conn)
{
	return pqSocketCheck(conn, 0, 1, (time_t) 0);
}

/*
 * 检查套接字，使用 poll 或 select，用于数据读取、写入
 * 或两者。如果一个或多个条件满足则返回 >0，
 * 超时返回 0，发生错误返回 -1。
 *
 * 如果使用了 SSL，在直接检查套接字之前先检查 SSL 缓冲区
 * 中的读取数据。
 */
static int
pqSocketCheck(PGconn *conn, int forRead, int forWrite, time_t end_time)
{
	int			result;

	if (!conn)
		return -1;
	if (conn->sock == PGINVALID_SOCKET)
	{
		libpq_append_conn_error(conn, "invalid socket");
		return -1;
	}

#ifdef USE_SSL
	/* 检查 SSL 库是否缓冲了读取字节 */
	if (forRead && conn->ssl_in_use && pgtls_read_pending(conn))
	{
		/* 绕过 select */
		return 1;
	}
#endif

	/* 只要收到 EINTR 就会重试 */
	do
		result = pqSocketPoll(conn->sock, forRead, forWrite, end_time);
	while (result < 0 && SOCK_ERRNO == EINTR);

	if (result < 0)
	{
		char		sebuf[PG_STRERROR_R_BUFLEN];

		libpq_append_conn_error(conn, "%s() failed: %s", "select",
								SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
	}

	return result;
}


/*
 * 检查文件描述符的读和/或写数据，可能等待。
 * 如果 neither forRead nor forWrite 都未设置，立即返回超时
 * 条件（不等待）。如果条件满足返回 >0，
 * 如果超时返回 0，如果发生错误或中断返回 -1。
 *
 * 如果 end_time 为 -1，超时无限（永不阻塞）。
 * 如果 end_time 为 0（或任何早于当前时间的时间），超时即时。
 */
static int
pqSocketPoll(int sock, int forRead, int forWrite, time_t end_time)
{
	/* 如果可用则使用 poll(2)，否则使用 select(2) */
#ifdef HAVE_POLL
	struct pollfd input_fd;
	int			timeout_ms;

	if (!forRead && !forWrite)
		return 0;

	input_fd.fd = sock;
	input_fd.events = POLLERR;
	input_fd.revents = 0;

	if (forRead)
		input_fd.events |= POLLIN;
	if (forWrite)
		input_fd.events |= POLLOUT;

	/* 计算合适的超时间隔 */
	if (end_time == ((time_t) -1))
		timeout_ms = -1;
	else
	{
		time_t		now = time(NULL);

		if (end_time > now)
			timeout_ms = (end_time - now) * 1000;
		else
			timeout_ms = 0;
	}

	return poll(&input_fd, 1, timeout_ms);
#else							/* !HAVE_POLL */

	fd_set		input_mask;
	fd_set		output_mask;
	fd_set		except_mask;
	struct timeval timeout;
	struct timeval *ptr_timeout;

	if (!forRead && !forWrite)
		return 0;

	FD_ZERO(&input_mask);
	FD_ZERO(&output_mask);
	FD_ZERO(&except_mask);
	if (forRead)
		FD_SET(sock, &input_mask);

	if (forWrite)
		FD_SET(sock, &output_mask);
	FD_SET(sock, &except_mask);

	/* 计算合适的超时间隔 */
	if (end_time == ((time_t) -1))
		ptr_timeout = NULL;
	else
	{
		time_t		now = time(NULL);

		if (end_time > now)
			timeout.tv_sec = end_time - now;
		else
			timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		ptr_timeout = &timeout;
	}

	return select(sock + 1, &input_mask, &output_mask,
				  &except_mask, ptr_timeout);
#endif							/* HAVE_POLL */
}


/*
 * 一些"杂项"多字节相关函数。它们原来
 * 在 fe-print.c 中，但那个文件已经注定要被淘汰了。
 */

/*
 * 类似 pg_encoding_mblen()。对于希望使用动态链接的 libpq
 * 编码立场的调用者，请使用此函数，即使这意味着
 * 在不同的可执行文件启动中表现不同。
 */
int
PQmblen(const char *s, int encoding)
{
	return pg_encoding_mblen(encoding, s);
}

/*
 * 类似 pg_encoding_mblen_bounded()。对于希望使用动态链接的
 * libpq 编码立场的调用者，请使用此函数，即使这意味着
 * 在不同的可执行文件启动中表现不同。
 */
int
PQmblenBounded(const char *s, int encoding)
{
	return strnlen(s, pg_encoding_mblen(encoding, s));
}

/*
 * 返回从 s 开始的字符的显示长度，使用
 * 指定的编码。
 */
int
PQdsplen(const char *s, int encoding)
{
	return pg_encoding_dsplen(encoding, s);
}

/*
 * 从环境变量 PGCLIENTENCODING 获取编码 id。
 */
int
PQenv2encoding(void)
{
	char	   *str;
	int			encoding = PG_SQL_ASCII;

	str = getenv("PGCLIENTENCODING");
	if (str && *str != '\0')
	{
		encoding = pg_char_to_encoding(str);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;
	}
	return encoding;
}


#ifdef ENABLE_NLS

static void
libpq_binddomain(void)
{
	/*
	 * 至少在 Windows 上，有一些 gettext 实现如果
	 * 多个线程同时调用 bindtextdomain() 会失败。使用互斥锁和
	 * 标志变量来确保每个进程只调用一次。尚不确定
	 * 非 Windows 平台上是否存在类似的 bug，但我们
	 * 不妨在所有地方都以相同的方式做。
	 */
	static volatile bool already_bound = false;
	static pthread_mutex_t binddomain_mutex = PTHREAD_MUTEX_INITIALIZER;

	if (!already_bound)
	{
		/* bindtextdomain() 不保留 errno */
#ifdef WIN32
		int			save_errno = GetLastError();
#else
		int			save_errno = errno;
#endif

		(void) pthread_mutex_lock(&binddomain_mutex);

		if (!already_bound)
		{
			const char *ldir;

			/*
			 * 这里不进行可重定位查找，因为调用可执行文件
			 * 可能在任意位置
			 */
			ldir = getenv("PGLOCALEDIR");
			if (!ldir)
				ldir = LOCALEDIR;
			bindtextdomain(PG_TEXTDOMAIN("libpq"), ldir);
			already_bound = true;
		}

		(void) pthread_mutex_unlock(&binddomain_mutex);

#ifdef WIN32
		SetLastError(save_errno);
#else
		errno = save_errno;
#endif
	}
}

char *
libpq_gettext(const char *msgid)
{
	libpq_binddomain();
	return dgettext(PG_TEXTDOMAIN("libpq"), msgid);
}

char *
libpq_ngettext(const char *msgid, const char *msgid_plural, unsigned long n)
{
	libpq_binddomain();
	return dngettext(PG_TEXTDOMAIN("libpq"), msgid, msgid_plural, n);
}

#endif							/* ENABLE_NLS */


/*
 * 向给定缓冲区追加格式化字符串，翻译后追加。
 * 自动追加换行符；格式不应以换行符结尾。
 *
 */
void
libpq_append_error(PQExpBuffer errorMessage, const char *fmt,...)
{
	int			save_errno = errno;
	bool		done;
	va_list		args;

	Assert(fmt[strlen(fmt) - 1] != '\n');

	if (PQExpBufferBroken(errorMessage))
		return;					/* 已经失败 */

	/* 循环以便在扩大缓冲区后重试 */
	do
	{
		errno = save_errno;
		va_start(args, fmt);
		done = appendPQExpBufferVA(errorMessage, libpq_gettext(fmt), args);
		va_end(args);
	} while (!done);

	appendPQExpBufferChar(errorMessage, '\n');
}

/*
 * 向给定连接的错误消息缓冲区追加格式化字符串，
 * 翻译后追加。自动追加换行符；格式不应以
 * 换行符结尾。
 */
void
libpq_append_conn_error(PGconn *conn, const char *fmt,...)
{
	int			save_errno = errno;
	bool		done;
	va_list		args;

	Assert(fmt[strlen(fmt) - 1] != '\n');

	if (PQExpBufferBroken(&conn->errorMessage))
		return;					/* 已经失败 */

	/* 循环以便在扩大缓冲区后重试 */
	do
	{
		errno = save_errno;
		va_start(args, fmt);
		done = appendPQExpBufferVA(&conn->errorMessage, libpq_gettext(fmt), args);
		va_end(args);
	} while (!done);

	appendPQExpBufferChar(&conn->errorMessage, '\n');
}
