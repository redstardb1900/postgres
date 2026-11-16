/*-------------------------------------------------------------------------
 *
 * mac8.c
 *	  8 字节（EUI-64）MAC 地址的 PostgreSQL 类型定义
 *
 * 接受 EUI-48（6 字节）MAC 地址作为输入，并以 EUI-64
 * 格式存储，其中第 4、5 字节分别置为 FF 和 FE。
 *
 * 输出总是 8 字节（EUI-64）格式。
 *
 * 以下代码在编写时假定 OUI 字段大小
 * 为 24 位。
 *
 * Portions Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/utils/adt/mac8.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/inet.h"

/*
 *	用于排序和比较的实用宏：
 */
#define hibits(addr) \
  ((unsigned long)(((addr)->a<<24) | ((addr)->b<<16) | ((addr)->c<<8) | ((addr)->d)))

#define lobits(addr) \
  ((unsigned long)(((addr)->e<<24) | ((addr)->f<<16) | ((addr)->g<<8) | ((addr)->h)))

static unsigned char hex2_to_uchar(const unsigned char *ptr, bool *badhex);

static const signed char hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/*
 * hex2_to_uchar - 将 2 个十六进制数字转为一个字节
 *
 * 若到达串末尾（遇到 '\0'）或任一字符不是有效十六进制
 * 数字，则将 *badhex 置为 true。
 */
static inline unsigned char
hex2_to_uchar(const unsigned char *ptr, bool *badhex)
{
	unsigned char ret;
	signed char lookup;

	/* 处理第一个字符 */
	if (*ptr > 127)
		goto invalid_input;

	lookup = hexlookup[*ptr];
	if (lookup < 0)
		goto invalid_input;

	ret = lookup << 4;

	/* 移到第二个字符 */
	ptr++;

	if (*ptr > 127)
		goto invalid_input;

	lookup = hexlookup[*ptr];
	if (lookup < 0)
		goto invalid_input;

	ret += lookup;

	return ret;

invalid_input:
	*badhex = true;
	return 0;
}

/*
 * MAC 地址（EUI-48 与 EUI-64）读取器。接受几种常见记法。
 */
Datum
macaddr8_in(PG_FUNCTION_ARGS)
{
	const unsigned char *str = (unsigned char *) PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	const unsigned char *ptr = str;
	bool		badhex = false;
	macaddr8   *result;
	unsigned char a = 0,
				b = 0,
				c = 0,
				d = 0,
				e = 0,
				f = 0,
				g = 0,
				h = 0;
	int			count = 0;
	unsigned char spacer = '\0';

	/* 跳过前导空格 */
	while (*ptr && isspace(*ptr))
		ptr++;

	/* 数字必须成对出现 */
	while (*ptr && *(ptr + 1))
	{
		/*
		 * 尝试解码各字节，字节必须为连续两个十六进制
		 * 数字。任一数字非十六进制时，hex2_to_uchar 会代
		 * 为抛出 ereport()。支持6/8 字节 MAC 地址。
		 */

		/* 尝试收集一个字节 */
		count++;

		switch (count)
		{
			case 1:
				a = hex2_to_uchar(ptr, &badhex);
				break;
			case 2:
				b = hex2_to_uchar(ptr, &badhex);
				break;
			case 3:
				c = hex2_to_uchar(ptr, &badhex);
				break;
			case 4:
				d = hex2_to_uchar(ptr, &badhex);
				break;
			case 5:
				e = hex2_to_uchar(ptr, &badhex);
				break;
			case 6:
				f = hex2_to_uchar(ptr, &badhex);
				break;
			case 7:
				g = hex2_to_uchar(ptr, &badhex);
				break;
			case 8:
				h = hex2_to_uchar(ptr, &badhex);
				break;
			default:
				/* 是尾部垃圾…… */
				goto fail;
		}

		if (badhex)
			goto fail;

		/* 前进到下一个字节应在的位置 */
		ptr += 2;

		/* 检查分隔符，这些字符有效，其余都无效 */
		if (*ptr == ':' || *ptr == '-' || *ptr == '.')
		{
			/* 记住所用的分隔符，若发生变化则无效 */
			if (spacer == '\0')
				spacer = *ptr;

			/* 全程必须使用同一分隔符 */
			else if (spacer != *ptr)
				goto fail;

			/* 越过分隔符 */
			ptr++;
		}

		/* 若已有 6 或 8 字节，允许其后有尾随空白 */
		if (count == 6 || count == 8)
		{
			if (isspace(*ptr))
			{
				while (*++ptr && isspace(*ptr));

				/* 若先遇到空格后又遇非空格，则无效 */
				if (*ptr)
					goto fail;
			}
		}
	}

	/* 将6字节 MAC 地址转换为 macaddr8 */
	if (count == 6)
	{
		h = f;
		g = e;
		f = d;

		d = 0xFF;
		e = 0xFE;
	}
	else if (count != 8)
		goto fail;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = a;
	result->b = b;
	result->c = c;
	result->d = d;
	result->e = e;
	result->f = f;
	result->g = g;
	result->h = h;

	PG_RETURN_MACADDR8_P(result);

fail:
	ereturn(escontext, (Datum) 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"", "macaddr8",
					str)));
}

/*
 * MAC8 地址（EUI-64）的定长输出函数。
 */
Datum
macaddr8_out(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	char	   *result;

	result = (char *) palloc(32);

	snprintf(result, 32, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
			 addr->a, addr->b, addr->c, addr->d,
			 addr->e, addr->f, addr->g, addr->h);

	PG_RETURN_CSTRING(result);
}

/*
 * macaddr8_recv - 将外部二进制格式(EUI-48 与 EUI-64)转成 macaddr8
 *
 * 外部表示就是这八个字节，MSB 在前。
 */
Datum
macaddr8_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	macaddr8   *addr;

	addr = (macaddr8 *) palloc0(sizeof(macaddr8));

	addr->a = pq_getmsgbyte(buf);
	addr->b = pq_getmsgbyte(buf);
	addr->c = pq_getmsgbyte(buf);

	if (buf->len == 6)
	{
		addr->d = 0xFF;
		addr->e = 0xFE;
	}
	else
	{
		addr->d = pq_getmsgbyte(buf);
		addr->e = pq_getmsgbyte(buf);
	}

	addr->f = pq_getmsgbyte(buf);
	addr->g = pq_getmsgbyte(buf);
	addr->h = pq_getmsgbyte(buf);

	PG_RETURN_MACADDR8_P(addr);
}

/*
 * macaddr8_send - 将 macaddr8(EUI-64) 转成二进制
 */
Datum
macaddr8_send(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, addr->a);
	pq_sendbyte(&buf, addr->b);
	pq_sendbyte(&buf, addr->c);
	pq_sendbyte(&buf, addr->d);
	pq_sendbyte(&buf, addr->e);
	pq_sendbyte(&buf, addr->f);
	pq_sendbyte(&buf, addr->g);
	pq_sendbyte(&buf, addr->h);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * macaddr8_cmp_internal - 用于排序的比较函数：
 */
static int32
macaddr8_cmp_internal(macaddr8 *a1, macaddr8 *a2)
{
	if (hibits(a1) < hibits(a2))
		return -1;
	else if (hibits(a1) > hibits(a2))
		return 1;
	else if (lobits(a1) < lobits(a2))
		return -1;
	else if (lobits(a1) > lobits(a2))
		return 1;
	else
		return 0;
}

Datum
macaddr8_cmp(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_INT32(macaddr8_cmp_internal(a1, a2));
}

/*
 * 布尔比较函数。
 */

Datum
macaddr8_lt(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) < 0);
}

Datum
macaddr8_le(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) <= 0);
}

Datum
macaddr8_eq(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) == 0);
}

Datum
macaddr8_ge(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) >= 0);
}

Datum
macaddr8_gt(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) > 0);
}

Datum
macaddr8_ne(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) != 0);
}

/*
 * macaddr8 哈希索引的支持函数。
 */
Datum
hashmacaddr8(PG_FUNCTION_ARGS)
{
	macaddr8   *key = PG_GETARG_MACADDR8_P(0);

	return hash_any((unsigned char *) key, sizeof(macaddr8));
}

Datum
hashmacaddr8extended(PG_FUNCTION_ARGS)
{
	macaddr8   *key = PG_GETARG_MACADDR8_P(0);

	return hash_any_extended((unsigned char *) key, sizeof(macaddr8),
							 PG_GETARG_INT64(1));
}

/*
 * 算术函数：按位 NOT、AND、OR。
 */
Datum
macaddr8_not(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = ~addr->a;
	result->b = ~addr->b;
	result->c = ~addr->c;
	result->d = ~addr->d;
	result->e = ~addr->e;
	result->f = ~addr->f;
	result->g = ~addr->g;
	result->h = ~addr->h;

	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8_and(PG_FUNCTION_ARGS)
{
	macaddr8   *addr1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *addr2 = PG_GETARG_MACADDR8_P(1);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = addr1->a & addr2->a;
	result->b = addr1->b & addr2->b;
	result->c = addr1->c & addr2->c;
	result->d = addr1->d & addr2->d;
	result->e = addr1->e & addr2->e;
	result->f = addr1->f & addr2->f;
	result->g = addr1->g & addr2->g;
	result->h = addr1->h & addr2->h;

	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8_or(PG_FUNCTION_ARGS)
{
	macaddr8   *addr1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *addr2 = PG_GETARG_MACADDR8_P(1);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = addr1->a | addr2->a;
	result->b = addr1->b | addr2->b;
	result->c = addr1->c | addr2->c;
	result->d = addr1->d | addr2->d;
	result->e = addr1->e | addr2->e;
	result->f = addr1->f | addr2->f;
	result->g = addr1->g | addr2->g;
	result->h = addr1->h | addr2->h;

	PG_RETURN_MACADDR8_P(result);
}

/*
 * 截断函数，用于比较 macaddr8 制造商。
 */
Datum
macaddr8_trunc(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = 0;
	result->e = 0;
	result->f = 0;
	result->g = 0;
	result->h = 0;

	PG_RETURN_MACADDR8_P(result);
}

/*
 * 为 IPv6 用的修改版 EUI-64 置第 7 位。
 */
Datum
macaddr8_set7bit(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr->a | 0x02;
	result->b = addr->b;
	result->c = addr->c;
	result->d = addr->d;
	result->e = addr->e;
	result->f = addr->f;
	result->g = addr->g;
	result->h = addr->h;

	PG_RETURN_MACADDR8_P(result);
}

/*----------------------------------------------------------
 *	转换算子。
 *---------------------------------------------------------*/

Datum
macaddrtomacaddr8(PG_FUNCTION_ARGS)
{
	macaddr    *addr6 = PG_GETARG_MACADDR_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr6->a;
	result->b = addr6->b;
	result->c = addr6->c;
	result->d = 0xFF;
	result->e = 0xFE;
	result->f = addr6->d;
	result->g = addr6->e;
	result->h = addr6->f;


	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8tomacaddr(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr    *result;

	result = (macaddr *) palloc0(sizeof(macaddr));

	if ((addr->d != 0xFF) || (addr->e != 0xFE))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("macaddr8 data out of range to convert to macaddr"),
				 errhint("Only addresses that have FF and FE as values in the "
						 "4th and 5th bytes from the left, for example "
						 "xx:xx:xx:ff:fe:xx:xx:xx, are eligible to be converted "
						 "from macaddr8 to macaddr.")));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = addr->f;
	result->e = addr->g;
	result->f = addr->h;

	PG_RETURN_MACADDR_P(result);
}
