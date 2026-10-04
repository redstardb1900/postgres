/*-------------------------------------------------------------------------
 *
 * pqformat.c
 *		Routines for formatting and parsing frontend/backend messages
 *
 * Outgoing messages are built up in a StringInfo buffer (which is expansible)
 * and then sent in a single call to pq_putmessage.  This module provides data
 * formatting/conversion routines that are needed to produce valid messages.
 * Note in particular the distinction between "raw data" and "text"; raw data
 * is message protocol characters and binary values that are not subject to
 * character set conversion, while text is converted by character encoding
 * rules.
 *
 * Incoming messages are similarly read into a StringInfo buffer, via
 * pq_getmessage, and then parsed and converted from that using the routines
 * in this module.
 *
 * These same routines support reading and writing of external binary formats
 * (typsend/typreceive routines).  The conversion routines for individual
 * data types are exactly the same, only initialization and completion
 * are different.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/libpq/pqformat.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 * 消息组装与输出：
 *		pq_beginmessage - 初始化 StringInfo 缓冲区
 *		pq_sendbyte		- 向 StringInfo 缓冲区追加一个原始字节
 *		pq_sendint		- 向 StringInfo 缓冲区追加一个二进制整数
 *		pq_sendint64	- 向 StringInfo 缓冲区追加一个二进制 8 字节整数
 *		pq_sendfloat4	- 向 StringInfo 缓冲区追加一个 float4
 *		pq_sendfloat8	- 向 StringInfo 缓冲区追加一个 float8
 *		pq_sendbytes	- 向 StringInfo 缓冲区追加原始数据
 *		pq_sendcountedtext - 追加带计数的文本字符串（含字符集转换）
 *		pq_sendtext		- 追加文本字符串（含转换）
 *		pq_sendstring	- 追加以空字符结尾的文本字符串（含转换）
 *		pq_send_ascii_string - 追加以空字符结尾的文本字符串（不含转换）
 *		pq_endmessage	- 把完成的消息发给前端
 * 注意：也可以用常规 StringInfo 例程向 StringInfo 缓冲区追加数据，
 * 但不鼓励这样做，因为可能不会发生所需的
 * 字符集转换。
 *
 * typsend 支持（构造包含外部二进制数据的 bytea 值）：
 *		pq_begintypsend - 初始化 StringInfo 缓冲区
 *		pq_endtypsend	- 把完成的字符串作为 "bytea*" 返回
 *
 * 特殊情况的消息输出：
 *		pq_puttextmessage - 一步生成经过字符集转换的消息
 *		pq_putemptymessage - 空消息体的便捷例程
 *
 * 输入后的消息解析：
 *		pq_getmsgbyte	- 从消息缓冲区取一个原始字节
 *		pq_getmsgint	- 从消息缓冲区取一个二进制整数
 *		pq_getmsgint64	- 从消息缓冲区取一个二进制 8 字节整数
 *		pq_getmsgfloat4 - 从消息缓冲区取一个 float4
 *		pq_getmsgfloat8 - 从消息缓冲区取一个 float8
 *		pq_getmsgbytes	- 从消息缓冲区取原始数据
 *		pq_copymsgbytes - 从消息缓冲区复制原始数据
 *		pq_getmsgtext	- 取带计数的文本字符串（含转换）
 *		pq_getmsgstring - 取以空字符结尾的文本字符串（含转换）
 *		pq_getmsgrawstring - 取以空字符结尾的文本字符串，不做转换
 *		pq_getmsgend	- 确认消息已被完全消耗
 */

#include "postgres.h"

#include <sys/param.h>

#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"
#include "varatt.h"


/* --------------------------------
 *		pq_beginmessage		- 初始化，准备发送一条消息
 * --------------------------------
 */
void
pq_beginmessage(StringInfo buf, char msgtype)
{
	initStringInfo(buf);

	/*
 * 我们把消息类型藏进缓冲区的 cursor 字段，并预期
 * pq_sendXXX 例程不会去动它。也可以改成把它作为
 * 缓冲区内容的第一个字节，但现在这样似乎更简单。
 */
	buf->cursor = msgtype;
}

/* --------------------------------

 *		pq_beginmessage_reuse - 初始化以发送消息，复用缓冲区
 *
 * 这需要把缓冲区分配在寿命足够长的
 * 内存上下文中。
 * --------------------------------
 */
void
pq_beginmessage_reuse(StringInfo buf, char msgtype)
{
	resetStringInfo(buf);

	/*
 * 我们把消息类型藏进缓冲区的 cursor 字段，并预期
 * pq_sendXXX 例程不会去动它。也可以改成把它作为
 * 缓冲区内容的第一个字节，但现在这样似乎更简单。
 */
	buf->cursor = msgtype;
}

/* --------------------------------
 *		pq_sendbytes	- 向 StringInfo 缓冲区追加原始数据
 * --------------------------------
 */
void
pq_sendbytes(StringInfo buf, const void *data, int datalen)
{
	/* 出于谨慎，使用会维持末尾空字节的变体 */
	appendBinaryStringInfo(buf, data, datalen);
}

/* --------------------------------
 *		pq_sendcountedtext - 追加带计数的文本字符串（含字符集转换）
 *
 * 本例程发给前端的数据是一个 4 字节计数字段，
 * 后面跟着字符串。计数是否包含自身，由
 * countincludesself 标志决定（3.0 之前的协议需要它包含自身）。
 * 传入的文本字符串不必以空字符结尾，发给
 * 前端的数据也是如此。
 * --------------------------------
 */
void
pq_sendcountedtext(StringInfo buf, const char *str, int slen,
				   bool countincludesself)
{
	int			extra = countincludesself ? 4 : 0;
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 实际转换做完了吗? */
	{
		slen = strlen(p);
		pq_sendint32(buf, slen + extra);
		appendBinaryStringInfoNT(buf, p, slen);
		pfree(p);
	}
	else
	{
		pq_sendint32(buf, slen + extra);
		appendBinaryStringInfoNT(buf, str, slen);
	}
}

/* --------------------------------
 *		pq_sendtext		- 追加文本字符串（含转换）
 *
 * 传入的文本字符串不必以空字符结尾，发给
 * 前端的数据也是如此。注意，这对直接发给
 * 前端并不真正有用，因为前端没有办法
 * 确定字符串长度。但它对二进制
 * 格式转换有用。
 * --------------------------------
 */
void
pq_sendtext(StringInfo buf, const char *str, int slen)
{
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 实际转换做完了吗? */
	{
		slen = strlen(p);
		appendBinaryStringInfo(buf, p, slen);
		pfree(p);
	}
	else
		appendBinaryStringInfo(buf, str, slen);
}

/* --------------------------------
 *		pq_sendstring	- 追加以空字符结尾的文本字符串（含转换）
 *
 * 注意：传入的文本字符串必须以空字符结尾，发给
 * 前端的数据也是如此。
 * --------------------------------
 */
void
pq_sendstring(StringInfo buf, const char *str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 实际转换做完了吗? */
	{
		slen = strlen(p);
		appendBinaryStringInfoNT(buf, p, slen + 1);
		pfree(p);
	}
	else
		appendBinaryStringInfoNT(buf, str, slen + 1);
}

/* --------------------------------
 *		pq_send_ascii_string	- 追加以空字符结尾的文本字符串（不做转换）
 *
 * 本函数有意绕过编码转换，只是悄悄把任何非
 * 7-bit-ASCII 字符替换成问号。
 * 仅当我们难以用正常的本地化和编码转换
 * 向客户端发送错误消息时才使用它。调用方
 * 应当已经采取措施确保字符串只是 ASCII；
 * 这里的额外工作只是为了确保我们不会把编码错误的
 * 字符串发给客户端（客户端对这种情况也许健壮，也许不健壮）。
 *
 * 注意：传入的文本字符串必须以空字符结尾，发给
 * 前端的数据也是如此。
 * --------------------------------
 */
void
pq_send_ascii_string(StringInfo buf, const char *str)
{
	while (*str)
	{
		char		ch = *str++;

		if (IS_HIGHBIT_SET(ch))
			ch = '?';
		appendStringInfoCharMacro(buf, ch);
	}
	appendStringInfoChar(buf, '\0');
}

/* --------------------------------
 *		pq_sendfloat4	- 向 StringInfo 缓冲区追加一个 float4
 *
 * 本例程的意义是把 float4 外部二进制表示的知识集中起来，
 * float4 是好几种数据类型的组成部分。
 *
 * 目前假定 float4 应按与 int4 相同的方式做字节交换。
 * 这条规则并不完美，但能让我们在大多数使用 IEEE
 * 浮点的体系结构上可移植。
 * --------------------------------
 */
void
pq_sendfloat4(StringInfo buf, float4 f)
{
	union
	{
		float4		f;
		uint32		i;
	}			swap;

	swap.f = f;
	pq_sendint32(buf, swap.i);
}

/* --------------------------------
 *		pq_sendfloat8	- 向 StringInfo 缓冲区追加一个 float8
 *
 * 本例程的意义是把 float8 外部二进制表示的知识集中起来，
 * float8 是好几种数据类型的组成部分。
 *
 * 目前假定 float8 应按与 int8 相同的方式做字节交换。
 * 这条规则并不完美，但能让我们在大多数使用 IEEE
 * 浮点的体系结构上可移植。
 * --------------------------------
 */
void
pq_sendfloat8(StringInfo buf, float8 f)
{
	union
	{
		float8		f;
		int64		i;
	}			swap;

	swap.f = f;
	pq_sendint64(buf, swap.i);
}

/* --------------------------------
 *		pq_endmessage	- 把完成的消息发给前端
 *
 * 数据缓冲区会被 pfree()，但若 StringInfo 是用
 * makeStringInfo 分配的，调用方仍必须 pfree 它。
 * --------------------------------
 */
void
pq_endmessage(StringInfo buf)
{
	/* msgtype 保存在 cursor 字段中 */
	(void) pq_putmessage(buf->cursor, buf->data, buf->len);
	/* 不必抱怨任何失败，因为 pqcomm.c 已经处理过 */
	pfree(buf->data);
	buf->data = NULL;
}

/* --------------------------------
 *		pq_endmessage_reuse	- 把完成的消息发给前端
 *
 * 数据缓冲区并不释放，以便用
 * pq_beginmessage_reuse 复用该缓冲区。
 --------------------------------
 */

void
pq_endmessage_reuse(StringInfo buf)
{
	/* msgtype 保存在 cursor 字段中 */
	(void) pq_putmessage(buf->cursor, buf->data, buf->len);
}


/* --------------------------------
 *		pq_begintypsend		- 初始化，准备构造 bytea 结果
 * --------------------------------
 */
void
pq_begintypsend(StringInfo buf)
{
	initStringInfo(buf);
	/* 为 bytea 长度字预留四个字节 */
	appendStringInfoCharMacro(buf, '\0');
	appendStringInfoCharMacro(buf, '\0');
	appendStringInfoCharMacro(buf, '\0');
	appendStringInfoCharMacro(buf, '\0');
}

/* --------------------------------
 *		pq_endtypsend	- 完成 bytea 结果的构造
 *
 * 数据缓冲区作为 palloc 出来的 bytea 值返回。（我们预期
 * 它会为此适当对齐，因为它是 palloc 出来的。）
 * 我们假定 StringInfoData 只是调用方的局部变量，
 * 不必 pfree。
 * --------------------------------
 */
bytea *
pq_endtypsend(StringInfo buf)
{
	bytea	   *result = (bytea *) buf->data;

	/* 把正确的长度写入 bytea 长度字 */
	Assert(buf->len >= VARHDRSZ);
	SET_VARSIZE(result, buf->len);

	return result;
}


/* --------------------------------
 *		pq_puttextmessage - 一步生成经过字符集转换的消息
 *
 *		这与 pqcomm.c 中的例程 pq_putmessage 相同，只是
 *		消息体是一个以空字符结尾的字符串，要对其做编码
 *		转换。
 * --------------------------------
 */
void
pq_puttextmessage(char msgtype, const char *str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 实际转换做完了吗? */
	{
		(void) pq_putmessage(msgtype, p, strlen(p) + 1);
		pfree(p);
		return;
	}
	(void) pq_putmessage(msgtype, str, slen + 1);
}


/* --------------------------------
 *		pq_putemptymessage - 空消息体的便捷例程
 * --------------------------------
 */
void
pq_putemptymessage(char msgtype)
{
	(void) pq_putmessage(msgtype, NULL, 0);
}


/* --------------------------------
 *		pq_getmsgbyte	- 从消息缓冲区取一个原始字节
 * --------------------------------
 */
int
pq_getmsgbyte(StringInfo msg)
{
	if (msg->cursor >= msg->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("no data left in message")));
	return (unsigned char) msg->data[msg->cursor++];
}

/* --------------------------------
 *		pq_getmsgint	- 从消息缓冲区取一个二进制整数
 *
 *		值按无符号处理。
 * --------------------------------
 */
unsigned int
pq_getmsgint(StringInfo msg, int b)
{
	unsigned int result;
	unsigned char n8;
	uint16		n16;
	uint32		n32;

	switch (b)
	{
		case 1:
			pq_copymsgbytes(msg, (char *) &n8, 1);
			result = n8;
			break;
		case 2:
			pq_copymsgbytes(msg, (char *) &n16, 2);
			result = pg_ntoh16(n16);
			break;
		case 4:
			pq_copymsgbytes(msg, (char *) &n32, 4);
			result = pg_ntoh32(n32);
			break;
		default:
			elog(ERROR, "unsupported integer size %d", b);
			result = 0;			/* 让编译器安静 */
			break;
	}
	return result;
}

/* --------------------------------
 *		pq_getmsgint64	- 从消息缓冲区取一个二进制 8 字节整数
 *
 * 很想把它和 pq_getmsgint 合并，但那样所有数据宽度的
 * 结果都得是 int64 --- 在 int64 效率不高的机器上，
 * 这可能是很大的性能损失。
 * --------------------------------
 */
int64
pq_getmsgint64(StringInfo msg)
{
	uint64		n64;

	pq_copymsgbytes(msg, (char *) &n64, sizeof(n64));

	return pg_ntoh64(n64);
}

/* --------------------------------
 *		pq_getmsgfloat4 - 从消息缓冲区取一个 float4
 *
 * 参见 pq_sendfloat4 的说明。
 * --------------------------------
 */
float4
pq_getmsgfloat4(StringInfo msg)
{
	union
	{
		float4		f;
		uint32		i;
	}			swap;

	swap.i = pq_getmsgint(msg, 4);
	return swap.f;
}

/* --------------------------------
 *		pq_getmsgfloat8 - 从消息缓冲区取一个 float8
 *
 * 参见 pq_sendfloat8 的说明。
 * --------------------------------
 */
float8
pq_getmsgfloat8(StringInfo msg)
{
	union
	{
		float8		f;
		int64		i;
	}			swap;

	swap.i = pq_getmsgint64(msg);
	return swap.f;
}

/* --------------------------------
 *		pq_getmsgbytes	- 从消息缓冲区取原始数据
 *
 *		返回直接指向消息缓冲区内部的指针；注意它
 *		也许没有任何特定对齐。
 * --------------------------------
 */
const char *
pq_getmsgbytes(StringInfo msg, int datalen)
{
	const char *result;

	if (datalen < 0 || datalen > (msg->len - msg->cursor))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("insufficient data left in message")));
	result = &msg->data[msg->cursor];
	msg->cursor += datalen;
	return result;
}

/* --------------------------------
 *		pq_copymsgbytes - 从消息缓冲区复制原始数据
 *
 *		与上面相同，只是数据被复制到调用方的缓冲区。
 * --------------------------------
 */
void
pq_copymsgbytes(StringInfo msg, char *buf, int datalen)
{
	if (datalen < 0 || datalen > (msg->len - msg->cursor))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("insufficient data left in message")));
	memcpy(buf, &msg->data[msg->cursor], datalen);
	msg->cursor += datalen;
}

/* --------------------------------
 *		pq_getmsgtext	- 取带计数的文本字符串（含转换）
 *
 *		总是返回指向新 palloc 出来的结果的指针。
 *		结果带末尾空字符，*并且* 我们在 *nbytes 中返回其 strlen。
 * --------------------------------
 */
char *
pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes)
{
	char	   *str;
	char	   *p;

	if (rawbytes < 0 || rawbytes > (msg->len - msg->cursor))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("insufficient data left in message")));
	str = &msg->data[msg->cursor];
	msg->cursor += rawbytes;

	p = pg_client_to_server(str, rawbytes);
	if (p != str)				/* 实际转换做完了吗? */
		*nbytes = strlen(p);
	else
	{
		p = (char *) palloc(rawbytes + 1);
		memcpy(p, str, rawbytes);
		p[rawbytes] = '\0';
		*nbytes = rawbytes;
	}
	return p;
}

/* --------------------------------
 *		pq_getmsgstring - 取以空字符结尾的文本字符串（含转换）
 *
 *		可能返回直接指向消息缓冲区内部的指针，或指向
 *		palloc 出来的转换结果的指针。
 * --------------------------------
 */
const char *
pq_getmsgstring(StringInfo msg)
{
	char	   *str;
	int			slen;

	str = &msg->data[msg->cursor];

	/*
 * 这里用 strlen() 是安全的，因为 StringInfo 保证
 * 有一个末尾空字节。但要检查我们在消息内部
 * 找到了空字符。
 */
	slen = strlen(str);
	if (msg->cursor + slen >= msg->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid string in message")));
	msg->cursor += slen + 1;

	return pg_client_to_server(str, slen);
}

/* --------------------------------
 *		pq_getmsgrawstring - 取以空字符结尾的文本字符串，不做转换
 *
 *		返回直接指向消息缓冲区内部的指针。
 * --------------------------------
 */
const char *
pq_getmsgrawstring(StringInfo msg)
{
	char	   *str;
	int			slen;

	str = &msg->data[msg->cursor];

	/*
 * 这里用 strlen() 是安全的，因为 StringInfo 保证
 * 有一个末尾空字节。但要检查我们在消息内部
 * 找到了空字符。
 */
	slen = strlen(str);
	if (msg->cursor + slen >= msg->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid string in message")));
	msg->cursor += slen + 1;

	return str;
}

/* --------------------------------
 *		pq_getmsgend	- 确认消息已被完全消耗
 * --------------------------------
 */
void
pq_getmsgend(StringInfo msg)
{
	if (msg->cursor != msg->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid message format")));
}
