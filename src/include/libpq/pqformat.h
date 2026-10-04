/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		Definitions for formatting and parsing frontend/backend messages
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"

extern void pq_beginmessage(StringInfo buf, char msgtype);
extern void pq_beginmessage_reuse(StringInfo buf, char msgtype);
extern void pq_endmessage(StringInfo buf);
extern void pq_endmessage_reuse(StringInfo buf);

extern void pq_sendbytes(StringInfo buf, const void *data, int datalen);
extern void pq_sendcountedtext(StringInfo buf, const char *str, int slen,
							   bool countincludesself);
extern void pq_sendtext(StringInfo buf, const char *str, int slen);
extern void pq_sendstring(StringInfo buf, const char *str);
extern void pq_send_ascii_string(StringInfo buf, const char *str);
extern void pq_sendfloat4(StringInfo buf, float4 f);
extern void pq_sendfloat8(StringInfo buf, float8 f);

/*
 * 向已经预分配了足够空间的 StringInfo 缓冲区追加一个
 * [u]int8。
 *
 * 使用 pg_restrict 让编译器可以基于 buf、buf->len、
 * buf->data 和 *buf->data 互不重叠这一假设来优化代码。
 * 没有这个标注，buf->len 等就无法在随后的 pq_writeintN
 * 调用期间保存在寄存器里。
 *
 * 使用 StringInfoData * 而不是 StringInfo，是因为 MSVC
 * 过于挑剔，要求在 restrict 前面有一个 *。
 */
static inline void
pq_writeint8(StringInfoData *pg_restrict buf, uint8 i)
{
	uint8		ni = i;

	Assert(buf->len + (int) sizeof(uint8) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint8));
	buf->len += sizeof(uint8);
}

/*
 * 向已经预分配了足够空间的 StringInfo 缓冲区追加一个
 * [u]int16。
 */
static inline void
pq_writeint16(StringInfoData *pg_restrict buf, uint16 i)
{
	uint16		ni = pg_hton16(i);

	Assert(buf->len + (int) sizeof(uint16) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint16));
	buf->len += sizeof(uint16);
}

/*
 * 向已经预分配了足够空间的 StringInfo 缓冲区追加一个
 * [u]int32。
 */
static inline void
pq_writeint32(StringInfoData *pg_restrict buf, uint32 i)
{
	uint32		ni = pg_hton32(i);

	Assert(buf->len + (int) sizeof(uint32) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint32));
	buf->len += sizeof(uint32);
}

/*
 * 向已经预分配了足够空间的 StringInfo 缓冲区追加一个
 * [u]int64。
 */
static inline void
pq_writeint64(StringInfoData *pg_restrict buf, uint64 i)
{
	uint64		ni = pg_hton64(i);

	Assert(buf->len + (int) sizeof(uint64) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint64));
	buf->len += sizeof(uint64);
}

/*
 * 向已预分配空间的缓冲区追加一个以空字符结尾的文本串
 * （带转换）。
 *
 * 注意：预分配的空间必须足以放下转换成客户端编码之后
 * 的字符串。
 *
 * 注意：传入的文本串必须以空字符结尾，发给前端的数据
 * 也同样如此。
 */
static inline void
pq_writestring(StringInfoData *pg_restrict buf, const char *pg_restrict str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 实际转换已经完成？ */
		slen = strlen(p);

	Assert(buf->len + slen + 1 <= buf->maxlen);

	memcpy(((char *pg_restrict) buf->data + buf->len), p, slen + 1);
	buf->len += slen + 1;

	if (p != str)
		pfree(p);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int8 */
static inline void
pq_sendint8(StringInfo buf, uint8 i)
{
	enlargeStringInfo(buf, sizeof(uint8));
	pq_writeint8(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int16 */
static inline void
pq_sendint16(StringInfo buf, uint16 i)
{
	enlargeStringInfo(buf, sizeof(uint16));
	pq_writeint16(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int32 */
static inline void
pq_sendint32(StringInfo buf, uint32 i)
{
	enlargeStringInfo(buf, sizeof(uint32));
	pq_writeint32(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int64 */
static inline void
pq_sendint64(StringInfo buf, uint64 i)
{
	enlargeStringInfo(buf, sizeof(uint64));
	pq_writeint64(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制字节 */
static inline void
pq_sendbyte(StringInfo buf, uint8 byt)
{
	pq_sendint8(buf, byt);
}

/*
 * 向 StringInfo 缓冲区追加一个二进制整数
 *
 * 本函数已弃用；请优先使用上面的函数。
 */
static inline void
pq_sendint(StringInfo buf, uint32 i, int b)
{
	switch (b)
	{
		case 1:
			pq_sendint8(buf, (uint8) i);
			break;
		case 2:
			pq_sendint16(buf, (uint16) i);
			break;
		case 4:
			pq_sendint32(buf, (uint32) i);
			break;
		default:
			elog(ERROR, "unsupported integer size %d", b);
			break;
	}
}


extern void pq_begintypsend(StringInfo buf);
extern bytea *pq_endtypsend(StringInfo buf);

extern void pq_puttextmessage(char msgtype, const char *str);
extern void pq_putemptymessage(char msgtype);

extern int	pq_getmsgbyte(StringInfo msg);
extern unsigned int pq_getmsgint(StringInfo msg, int b);
extern int64 pq_getmsgint64(StringInfo msg);
extern float4 pq_getmsgfloat4(StringInfo msg);
extern float8 pq_getmsgfloat8(StringInfo msg);
extern const char *pq_getmsgbytes(StringInfo msg, int datalen);
extern void pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
extern char *pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes);
extern const char *pq_getmsgstring(StringInfo msg);
extern const char *pq_getmsgrawstring(StringInfo msg);
extern void pq_getmsgend(StringInfo msg);

#endif							/* PQFORMAT_H */
