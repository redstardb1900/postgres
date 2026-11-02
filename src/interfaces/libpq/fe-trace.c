/*-------------------------------------------------------------------------
 *
 *	fe-trace.c
 *	  functions for libpq protocol tracing
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-trace.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/* 启用跟踪 */
void
PQtrace(PGconn *conn, FILE *debug_port)
{
	if (conn == NULL)
		return;
	PQuntrace(conn);
	if (debug_port == NULL)
		return;

	conn->Pfdebug = debug_port;
	conn->traceFlags = 0;
}

/* 禁用跟踪 */
void
PQuntrace(PGconn *conn)
{
	if (conn == NULL)
		return;
	if (conn->Pfdebug)
	{
		fflush(conn->Pfdebug);
		conn->Pfdebug = NULL;
	}

	conn->traceFlags = 0;
}

/* 设置当前跟踪会话的标志 */
void
PQsetTraceFlags(PGconn *conn, int flags)
{
	if (conn == NULL)
		return;
	/* 如果 PQtrace() 失败，什么也不做。 */
	if (conn->Pfdebug == NULL)
		return;
	conn->traceFlags = flags;
}

/*
 * 将当前时间（含微秒）打印到调用者提供的
 * 缓冲区中。
 * 借鉴自 get_formatted_log_time，但简单得多。
 */
static void
pqTraceFormatTimestamp(char *timestr, size_t ts_len)
{
	struct timeval tval;
	time_t		now;

	gettimeofday(&tval, NULL);

	/*
	 * MSVC 中 timeval 使用 long 表示 tv_sec，然而
	 * localtime() 期望 time_t 指针。这里将 tv_sec
	 * 赋值给局部 time_t 变量，以便向 localtime()
	 * 传递正确的指针类型。
	 */
	now = tval.tv_sec;
	strftime(timestr, ts_len,
			 "%Y-%m-%d %H:%M:%S",
			 localtime(&now));
	/* 追加微秒 */
	snprintf(timestr + strlen(timestr), ts_len - strlen(timestr),
			 ".%06u", (unsigned int) (tval.tv_usec));
}

/*
 *   pqTraceOutputByte1: 将 1 字节消息输出到日志
 */
static void
pqTraceOutputByte1(FILE *pfdebug, const char *data, int *cursor)
{
	const char *v = data + *cursor;

	/*
	 * 以十六进制格式显示不可打印数据，包括完成
	 * ErrorResponse 和 NoticeResponse 消息的结尾 \0。
	 */
	if (!isprint((unsigned char) *v))
		fprintf(pfdebug, " \\x%02x", (unsigned char) *v);
	else
		fprintf(pfdebug, " %c", *v);
	*cursor += 1;
}

/*
 *   pqTraceOutputInt16: 将 2 字节整数消息输出到日志
 */
static int
pqTraceOutputInt16(FILE *pfdebug, const char *data, int *cursor)
{
	uint16		tmp;
	int			result;

	memcpy(&tmp, data + *cursor, 2);
	*cursor += 2;
	result = (int) pg_ntoh16(tmp);
	fprintf(pfdebug, " %d", result);

	return result;
}

/*
 *   pqTraceOutputInt32: 将 4 字节整数消息输出到日志
 *
 * 如果 'suppress' 为 true，打印字面量 NNNN 而非实际数字。
 */
static int
pqTraceOutputInt32(FILE *pfdebug, const char *data, int *cursor, bool suppress)
{
	int			result;

	memcpy(&result, data + *cursor, 4);
	*cursor += 4;
	result = (int) pg_ntoh32(result);
	if (suppress)
		fprintf(pfdebug, " NNNN");
	else
		fprintf(pfdebug, " %d", result);

	return result;
}

/*
 *   pqTraceOutputString: 将字符串消息输出到日志
 */
static void
pqTraceOutputString(FILE *pfdebug, const char *data, int *cursor, bool suppress)
{
	int			len;

	if (suppress)
	{
		fprintf(pfdebug, " \"SSSS\"");
		*cursor += strlen(data + *cursor) + 1;
	}
	else
	{
		len = fprintf(pfdebug, " \"%s\"", data + *cursor);

		/*
		 * 这是一个以空字符结尾的字符串。因此从 len
		 * 减去代表双引号和空格长度的 3 后加 1。
		 */
		*cursor += (len - 3 + 1);
	}
}

/*
 * pqTraceOutputNchar: 将恰好 len 字节的消息输出到日志
 */
static void
pqTraceOutputNchar(FILE *pfdebug, int len, const char *data, int *cursor)
{
	int			i,
				next;			/* 尚未打印的第一个字符 */
	const char *v = data + *cursor;

	fprintf(pfdebug, " \'");

	for (next = i = 0; i < len; ++i)
	{
		if (isprint((unsigned char) v[i]))
			continue;
		else
		{
			fwrite(v + next, 1, i - next, pfdebug);
			fprintf(pfdebug, "\\x%02x", (unsigned char) v[i]);
			next = i + 1;
		}
	}
	if (next < len)
		fwrite(v + next, 1, len - next, pfdebug);

	fprintf(pfdebug, "\'");
	*cursor += len;
}

/*
 * 按协议消息类型的输出函数
 */

/* NotificationResponse 通知响应 */
static void
pqTraceOutputA(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "NotificationResponse\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

/* Bind 绑定 */
static void
pqTraceOutputB(FILE *f, const char *message, int *cursor)
{
	int			nparams;

	fprintf(f, "Bind\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(f, message, cursor);

	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
	{
		int			nbytes;

		nbytes = pqTraceOutputInt32(f, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(f, nbytes, message, cursor);
	}

	nparams = pqTraceOutputInt16(f, message, cursor);
	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* Close(F) 关闭(前端) 或 CommandComplete(B) 命令完成(后端) */
static void
pqTraceOutputC(FILE *f, bool toServer, const char *message, int *cursor)
{
	if (toServer)
	{
		fprintf(f, "Close\t");
		pqTraceOutputByte1(f, message, cursor);
		pqTraceOutputString(f, message, cursor, false);
	}
	else
	{
		fprintf(f, "CommandComplete\t");
		pqTraceOutputString(f, message, cursor, false);
	}
}

/* Describe(F) 描述(前端) 或 DataRow(B) 数据行(后端) */
static void
pqTraceOutputD(FILE *f, bool toServer, const char *message, int *cursor)
{
	if (toServer)
	{
		fprintf(f, "Describe\t");
		pqTraceOutputByte1(f, message, cursor);
		pqTraceOutputString(f, message, cursor, false);
	}
	else
	{
		int			nfields;
		int			len;
		int			i;

		fprintf(f, "DataRow\t");
		nfields = pqTraceOutputInt16(f, message, cursor);
		for (i = 0; i < nfields; i++)
		{
			len = pqTraceOutputInt32(f, message, cursor, false);
			if (len == -1)
				continue;
			pqTraceOutputNchar(f, len, message, cursor);
		}
	}
}

/* NoticeResponse 通知响应 / ErrorResponse 错误响应 */
static void
pqTraceOutputNR(FILE *f, const char *type, const char *message, int *cursor,
				bool regress)
{
	fprintf(f, "%s\t", type);
	for (;;)
	{
		char		field;
		bool		suppress;

		pqTraceOutputByte1(f, message, cursor);
		field = message[*cursor - 1];
		if (field == '\0')
			break;

		suppress = regress && (field == 'L' || field == 'F' || field == 'R');
		pqTraceOutputString(f, message, cursor, suppress);
	}
}

/* Execute(F) 执行(前端) 或 ErrorResponse(B) 错误响应(后端) */
static void
pqTraceOutputE(FILE *f, bool toServer, const char *message, int *cursor, bool regress)
{
	if (toServer)
	{
		fprintf(f, "Execute\t");
		pqTraceOutputString(f, message, cursor, false);
		pqTraceOutputInt32(f, message, cursor, false);
	}
	else
		pqTraceOutputNR(f, "ErrorResponse", message, cursor, regress);
}

/* CopyFail 复制失败 */
static void
pqTraceOutputf(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "CopyFail\t");
	pqTraceOutputString(f, message, cursor, false);
}

/* FunctionCall 函数调用 */
static void
pqTraceOutputF(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;
	int			nbytes;

	fprintf(f, "FunctionCall\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);

	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		nbytes = pqTraceOutputInt32(f, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(f, nbytes, message, cursor);
	}

	pqTraceOutputInt16(f, message, cursor);
}

/* CopyInResponse 复制输入响应 */
static void
pqTraceOutputG(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyInResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* CopyOutResponse 复制输出响应 */
static void
pqTraceOutputH(FILE *f, const char *message, int *cursor)
{
	int			nfields;

	fprintf(f, "CopyOutResponse\t");
	pqTraceOutputByte1(f, message, cursor);
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(f, message, cursor);
}

/* BackendKeyData 后端密钥数据 */
static void
pqTraceOutputK(FILE *f, const char *message, int *cursor, bool regress)
{
	fprintf(f, "BackendKeyData\t");
	pqTraceOutputInt32(f, message, cursor, regress);
	pqTraceOutputInt32(f, message, cursor, regress);
}

/* Parse 解析 */
static void
pqTraceOutputP(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nparams;

	fprintf(f, "Parse\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
	nparams = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

/* Query 查询 */
static void
pqTraceOutputQ(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Query\t");
	pqTraceOutputString(f, message, cursor, false);
}

/* Authentication 认证 */
static void
pqTraceOutputR(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "Authentication\t");
	pqTraceOutputInt32(f, message, cursor, false);
}

/* ParameterStatus 参数状态 */
static void
pqTraceOutputS(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "ParameterStatus\t");
	pqTraceOutputString(f, message, cursor, false);
	pqTraceOutputString(f, message, cursor, false);
}

/* ParameterDescription 参数描述 */
static void
pqTraceOutputt(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;

	fprintf(f, "ParameterDescription\t");
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt32(f, message, cursor, regress);
}

/* RowDescription 行描述 */
static void
pqTraceOutputT(FILE *f, const char *message, int *cursor, bool regress)
{
	int			nfields;

	fprintf(f, "RowDescription\t");
	nfields = pqTraceOutputInt16(f, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		pqTraceOutputString(f, message, cursor, false);
		pqTraceOutputInt32(f, message, cursor, regress);
		pqTraceOutputInt16(f, message, cursor);
		pqTraceOutputInt32(f, message, cursor, regress);
		pqTraceOutputInt16(f, message, cursor);
		pqTraceOutputInt32(f, message, cursor, false);
		pqTraceOutputInt16(f, message, cursor);
	}
}

/* NegotiateProtocolVersion 协商协议版本 */
static void
pqTraceOutputv(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "NegotiateProtocolVersion\t");
	pqTraceOutputInt32(f, message, cursor, false);
	pqTraceOutputInt32(f, message, cursor, false);
}

/* FunctionCallResponse 函数调用响应 */
static void
pqTraceOutputV(FILE *f, const char *message, int *cursor)
{
	int			len;

	fprintf(f, "FunctionCallResponse\t");
	len = pqTraceOutputInt32(f, message, cursor, false);
	if (len != -1)
		pqTraceOutputNchar(f, len, message, cursor);
}

/* CopyBothResponse 双向复制响应 */
static void
pqTraceOutputW(FILE *f, const char *message, int *cursor, int length)
{
	fprintf(f, "CopyBothResponse\t");
	pqTraceOutputByte1(f, message, cursor);

	while (length > *cursor)
		pqTraceOutputInt16(f, message, cursor);
}

/* ReadyForQuery 就绪查询 */
static void
pqTraceOutputZ(FILE *f, const char *message, int *cursor)
{
	fprintf(f, "ReadyForQuery\t");
	pqTraceOutputByte1(f, message, cursor);
}

/*
 * 将给定消息打印到跟踪输出流。
 */
void
pqTraceOutputMessage(PGconn *conn, const char *message, bool toServer)
{
	char		id;
	int			length;
	char	   *prefix = toServer ? "F" : "B";
	int			logCursor = 0;
	bool		regress;

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		fprintf(conn->Pfdebug, "%s\t", timestr);
	}
	regress = (conn->traceFlags & PQTRACE_REGRESS_MODE) != 0;

	id = message[logCursor++];

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	/*
	 * 在回归模式下，抑制 ErrorResponse 和
	 * NoticeResponse 的长度。F（文件名）、L（行号）和 R（例程名）
	 * 字段可能随着服务器代码修改而改变，如果它们的
	 * 长度与原长度不同，会破坏测试。
	 */
	if (regress && !toServer && (id == 'E' || id == 'N'))
		fprintf(conn->Pfdebug, "%s\tNN\t", prefix);
	else
		fprintf(conn->Pfdebug, "%s\t%d\t", prefix, length);

	switch (id)
	{
		case '1':
			fprintf(conn->Pfdebug, "ParseComplete");
			/* 无消息内容 */
			break;
		case '2':
			fprintf(conn->Pfdebug, "BindComplete");
			/* 无消息内容 */
			break;
		case '3':
			fprintf(conn->Pfdebug, "CloseComplete");
			/* 无消息内容 */
			break;
		case 'A':				/* 通知响应 */
			pqTraceOutputA(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'B':				/* 绑定 */
			pqTraceOutputB(conn->Pfdebug, message, &logCursor);
			break;
		case 'c':
			fprintf(conn->Pfdebug, "CopyDone");
			/* 无消息内容 */
			break;
		case 'C':				/* 关闭(前端) 或 命令完成(后端) */
			pqTraceOutputC(conn->Pfdebug, toServer, message, &logCursor);
			break;
		case 'd':				/* 复制数据 */
			/* 丢弃 COPY 数据以减少日志开销。 */
			break;
		case 'D':				/* 描述(前端) 或 数据行(后端) */
			pqTraceOutputD(conn->Pfdebug, toServer, message, &logCursor);
			break;
		case 'E':				/* 执行(前端) 或 错误响应(后端) */
			pqTraceOutputE(conn->Pfdebug, toServer, message, &logCursor,
						   regress);
			break;
		case 'f':				/* 复制失败 */
			pqTraceOutputf(conn->Pfdebug, message, &logCursor);
			break;
		case 'F':				/* 函数调用 */
			pqTraceOutputF(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'G':				/* 开始复制输入 */
			pqTraceOutputG(conn->Pfdebug, message, &logCursor);
			break;
		case 'H':				/* 刷新(前端) 或 开始复制输出(后端) */
			if (!toServer)
				pqTraceOutputH(conn->Pfdebug, message, &logCursor);
			else
				fprintf(conn->Pfdebug, "Flush");	/* 无消息内容 */
			break;
		case 'I':
			fprintf(conn->Pfdebug, "EmptyQueryResponse");
			/* 无消息内容 */
			break;
		case 'K':				/* 来自后端的密钥数据 */
			pqTraceOutputK(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'n':
			fprintf(conn->Pfdebug, "NoData");
			/* 无消息内容 */
			break;
		case 'N':
			pqTraceOutputNR(conn->Pfdebug, "NoticeResponse", message,
							&logCursor, regress);
			break;
		case 'P':				/* 解析 */
			pqTraceOutputP(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'Q':				/* 查询 */
			pqTraceOutputQ(conn->Pfdebug, message, &logCursor);
			break;
		case 'R':				/* 认证 */
			pqTraceOutputR(conn->Pfdebug, message, &logCursor);
			break;
		case 's':
			fprintf(conn->Pfdebug, "PortalSuspended");
			/* 无消息内容 */
			break;
		case 'S':				/* 参数状态(后端) 或 同步(前端) */
			if (!toServer)
				pqTraceOutputS(conn->Pfdebug, message, &logCursor);
			else
				fprintf(conn->Pfdebug, "Sync"); /* 无消息内容 */
			break;
		case 't':				/* 参数描述 */
			pqTraceOutputt(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'T':				/* 行描述 */
			pqTraceOutputT(conn->Pfdebug, message, &logCursor, regress);
			break;
		case 'v':				/* 协商协议版本 */
			pqTraceOutputv(conn->Pfdebug, message, &logCursor);
			break;
		case 'V':				/* 函数调用响应 */
			pqTraceOutputV(conn->Pfdebug, message, &logCursor);
			break;
		case 'W':				/* 开始双向复制 */
			pqTraceOutputW(conn->Pfdebug, message, &logCursor, length);
			break;
		case 'X':
			fprintf(conn->Pfdebug, "Terminate");
			/* 无消息内容 */
			break;
		case 'Z':				/* 就绪查询 */
			pqTraceOutputZ(conn->Pfdebug, message, &logCursor);
			break;
		default:
			fprintf(conn->Pfdebug, "Unknown message: %02x", id);
			break;
	}

	fputc('\n', conn->Pfdebug);

	/*
	 * 验证打印例程是否正确。注意：1 字节消息标识符
	 * 不包含在长度中，但我们的游标包含它。
	 *
	 */
	if (logCursor - 1 != length)
		fprintf(conn->Pfdebug,
				"mismatched message length: consumed %d, expected %d\n",
				logCursor - 1, length);
}

/*
 * 将特殊消息（不含类型字节的消息）打印到跟踪输出流。
 *
 */
void
pqTraceOutputNoTypeByteMessage(PGconn *conn, const char *message)
{
	int			length;
	int			logCursor = 0;

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		fprintf(conn->Pfdebug, "%s\t", timestr);
	}

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	fprintf(conn->Pfdebug, "F\t%d\t", length);

	switch (length)
	{
		case 16:				/* 取消请求 */
			fprintf(conn->Pfdebug, "CancelRequest\t");
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			pqTraceOutputInt32(conn->Pfdebug, message, &logCursor, false);
			break;
		case 8:					/* GSSENCRequest 或 SSLRequest */
			/* 这些消息不会到达此处。 */
		default:
			fprintf(conn->Pfdebug, "Unknown message: length is %d", length);
			break;
	}

	fputc('\n', conn->Pfdebug);
}
