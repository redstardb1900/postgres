/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1996,1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *	  src/backend/utils/adt/inet_cidr_ntop.c
 */

#if defined(LIBC_SCCS) && !defined(lint)
static const char rcsid[] = "Id: inet_net_ntop.c,v 1.1.2.2 2004/03/09 09:17:27 marka Exp $";
#endif

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "utils/builtins.h"
#include "utils/inet.h"


#ifdef SPRINTF_CHAR
#define SPRINTF(x) strlen(sprintf/**/x)
#else
#define SPRINTF(x) ((size_t)sprintf x)
#endif

static char *inet_cidr_ntop_ipv4(const u_char *src, int bits,
								 char *dst, size_t size);
static char *inet_cidr_ntop_ipv6(const u_char *src, int bits,
								 char *dst, size_t size);

/*
 * char *
 * pg_inet_cidr_ntop(af, src, bits, dst, size)
 *	将网络号由网络格式转换为呈现格式。
 *	总生成 CIDR 风格结果。
 * 返回:
 *	指向 dst 的指针，出错时为 NULL (检查 errno)。
 * 作者:
 *	Paul Vixie (ISC), July 1996
 */
char *
pg_inet_cidr_ntop(int af, const void *src, int bits, char *dst, size_t size)
{
	switch (af)
	{
		case PGSQL_AF_INET:
			return inet_cidr_ntop_ipv4(src, bits, dst, size);
		case PGSQL_AF_INET6:
			return inet_cidr_ntop_ipv6(src, bits, dst, size);
		default:
			errno = EAFNOSUPPORT;
			return NULL;
	}
}


/*
 * static char *
 * inet_cidr_ntop_ipv4(src, bits, dst, size)
 *	将 IPv4 网络号由网络格式转换为呈现格式。
 *	总生成 CIDR 风格结果。
 * 返回:
 *	指向 dst 的指针，出错时为 NULL (检查 errno)。
 * note:
 *	假定网络字节序，即 192.5.5.240/28 的
 *	第四字节为 0b11110000。
 * 作者:
 *	Paul Vixie (ISC), July 1996
 */
static char *
inet_cidr_ntop_ipv4(const u_char *src, int bits, char *dst, size_t size)
{
	char	   *odst = dst;
	char	   *t;
	u_int		m;
	int			b;

	if (bits < 0 || bits > 32)
	{
		errno = EINVAL;
		return NULL;
	}

	if (bits == 0)
	{
		if (size < sizeof "0")
			goto emsgsize;
		*dst++ = '0';
		size--;
		*dst = '\0';
	}

	/* 输出整八位组 */
	for (b = bits / 8; b > 0; b--)
	{
		if (size <= sizeof "255.")
			goto emsgsize;
		t = dst;
		dst += SPRINTF((dst, "%u", *src++));
		if (b > 1)
		{
			*dst++ = '.';
			*dst = '\0';
		}
		size -= (size_t) (dst - t);
	}

	/* 输出部分八位组 */
	b = bits % 8;
	if (b > 0)
	{
		if (size <= sizeof ".255")
			goto emsgsize;
		t = dst;
		if (dst != odst)
			*dst++ = '.';
		m = ((1 << b) - 1) << (8 - b);
		dst += SPRINTF((dst, "%u", *src & m));
		size -= (size_t) (dst - t);
	}

	/* 输出 CIDR 宽度 */
	if (size <= sizeof "/32")
		goto emsgsize;
	dst += SPRINTF((dst, "/%u", bits));
	return odst;

emsgsize:
	errno = EMSGSIZE;
	return NULL;
}

/*
 * static char *
 * inet_cidr_ntop_ipv6(src, bits, dst, size)
 *	将 IPv6 网络号由网络格式转换为呈现格式。
 *	总生成 CIDR 风格结果，选取最短表示，
 *	除非 IP 实为 IPv4。
 *	总是输出指定位数 (bits)。
 * 返回:
 *	指向 dst 的指针，出错时为 NULL (检查 errno)。
 * note:
 *	假定网络字节序，即 192.5.5.240/28 的
 *	第四字节为 0x11110000。
 * 作者:
 *	Vadim Kogan (UCB), June 2001
 *	Original version (IPv4) by Paul Vixie (ISC), July 1996
 */

static char *
inet_cidr_ntop_ipv6(const u_char *src, int bits, char *dst, size_t size)
{
	u_int		m;
	int			b;
	int			p;
	int			zero_s,
				zero_l,
				tmp_zero_s,
				tmp_zero_l;
	int			i;
	int			is_ipv4 = 0;
	unsigned char inbuf[16];
	char		outbuf[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *cp;
	int			words;
	u_char	   *s;

	if (bits < 0 || bits > 128)
	{
		errno = EINVAL;
		return NULL;
	}

	cp = outbuf;

	if (bits == 0)
	{
		*cp++ = ':';
		*cp++ = ':';
		*cp = '\0';
	}
	else
	{
		/* 复制入私有缓冲，主机部清零。 */
		p = (bits + 7) / 8;
		memcpy(inbuf, src, p);
		memset(inbuf + p, 0, 16 - p);
		b = bits % 8;
		if (b != 0)
		{
			m = ((u_int) ~0) << (8 - b);
			inbuf[p - 1] &= m;
		}

		s = inbuf;

		/* 计算输出中需显示的组数。 */
		words = (bits + 15) / 16;
		if (words == 1)
			words = 2;

		/* 查找最长的零子串。 */
		zero_s = zero_l = tmp_zero_s = tmp_zero_l = 0;
		for (i = 0; i < (words * 2); i += 2)
		{
			if ((s[i] | s[i + 1]) == 0)
			{
				if (tmp_zero_l == 0)
					tmp_zero_s = i / 2;
				tmp_zero_l++;
			}
			else
			{
				if (tmp_zero_l && zero_l < tmp_zero_l)
				{
					zero_s = tmp_zero_s;
					zero_l = tmp_zero_l;
					tmp_zero_l = 0;
				}
			}
		}

		if (tmp_zero_l && zero_l < tmp_zero_l)
		{
			zero_s = tmp_zero_s;
			zero_l = tmp_zero_l;
		}

		if (zero_l != words && zero_s == 0 && ((zero_l == 6) ||
											   ((zero_l == 5 && s[10] == 0xff && s[11] == 0xff) ||
												((zero_l == 7 && s[14] != 0 && s[15] != 1)))))
			is_ipv4 = 1;

		/* 输出完整组。 */
		for (p = 0; p < words; p++)
		{
			if (zero_l != 0 && p >= zero_s && p < zero_s + zero_l)
			{
				/* 跳过零串。 */
				if (p == zero_s)
					*cp++ = ':';
				if (p == words - 1)
					*cp++ = ':';
				s++;
				s++;
				continue;
			}

			if (is_ipv4 && p > 5)
			{
				*cp++ = (p == 6) ? ':' : '.';
				cp += SPRINTF((cp, "%u", *s++));
				/* 或许可省略最后一个八位组 */
				if (p != 7 || bits > 120)
				{
					*cp++ = '.';
					cp += SPRINTF((cp, "%u", *s++));
				}
			}
			else
			{
				if (cp != outbuf)
					*cp++ = ':';
				cp += SPRINTF((cp, "%x", *s * 256 + s[1]));
				s += 2;
			}
		}
	}
	/* 输出 CIDR 宽度 */
	(void) SPRINTF((cp, "/%u", bits));
	if (strlen(outbuf) + 1 > size)
		goto emsgsize;
	strcpy(dst, outbuf);

	return dst;

emsgsize:
	errno = EMSGSIZE;
	return NULL;
}
