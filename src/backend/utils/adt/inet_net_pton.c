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
 *	  src/backend/utils/adt/inet_net_pton.c
 */

#if defined(LIBC_SCCS) && !defined(lint)
static const char rcsid[] = "Id: inet_net_pton.c,v 1.4.2.3 2004/03/17 00:40:11 marka Exp $";
#endif

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>

#include "utils/builtins.h" /* pgrminclude ignore */	/* 某些平台
														 * 需要 */
#include "utils/inet.h"


static int	inet_net_pton_ipv4(const char *src, u_char *dst);
static int	inet_cidr_pton_ipv4(const char *src, u_char *dst, size_t size);
static int	inet_net_pton_ipv6(const char *src, u_char *dst);
static int	inet_cidr_pton_ipv6(const char *src, u_char *dst, size_t size);


/*
 * int
 * pg_inet_net_pton(af, src, dst, size)
 *	将网络号由呈现格式转换为网络格式。
 *	接受十六/十进制八位组、十六进制串、/CIDR
 *	"size" 以字节计，描述 "dst"。
 * 返回:
 *	位数，或按类别推断而得，或由 /CIDR 明确指定，
 *	失败时为 -1（检查 errno）。ENOENT 表示它不是
 *	有效的网络规范。
 * 作者:
 *	Paul Vixie (ISC), June 1996
 *
 * 改动:
 *	我加入了 inet_cidr_pton 函数（同样来自 Paul），并
 *	更改名称以反映当前用途。
 *
 */
int
pg_inet_net_pton(int af, const char *src, void *dst, size_t size)
{
	switch (af)
	{
		case PGSQL_AF_INET:
			return size == -1 ?
				inet_net_pton_ipv4(src, dst) :
				inet_cidr_pton_ipv4(src, dst, size);
		case PGSQL_AF_INET6:
			return size == -1 ?
				inet_net_pton_ipv6(src, dst) :
				inet_cidr_pton_ipv6(src, dst, size);
		default:
			errno = EAFNOSUPPORT;
			return -1;
	}
}

/*
 * static int
 * inet_cidr_pton_ipv4(src, dst, size)
 *	将 IPv4 网络号由呈现格式转换为网络格式。
 *	接受十六/十进制八位组、十六进制串、/CIDR
 *	"size" 以字节计，描述 "dst"。
 * 返回:
 *	位数，或按类别推断而得，或由 /CIDR 明确指定，
 *	失败时为 -1（检查 errno）。ENOENT 表示它不是
 *	有效的 IPv4 网络规范。
 * note:
 *	假定网络字节序，即 192.5.5.240/28 的第四个八
 *	位组为 0b11110000。
 * 作者:
 *	Paul Vixie (ISC), June 1996
 */
static int
inet_cidr_pton_ipv4(const char *src, u_char *dst, size_t size)
{
	static const char xdigits[] = "0123456789abcdef";
	static const char digits[] = "0123456789";
	int			n,
				ch,
				tmp = 0,
				dirty,
				bits;
	const u_char *odst = dst;

	ch = *src++;
	if (ch == '0' && (src[0] == 'x' || src[0] == 'X')
		&& isxdigit((unsigned char) src[1]))
	{
		/* 十六进制：吞半字节。 */
		if (size <= 0U)
			goto emsgsize;
		dirty = 0;
		src++;					/* 跳过 x/X */
		while ((ch = *src++) != '\0' && isxdigit((unsigned char) ch))
		{
			if (isupper((unsigned char) ch))
				ch = tolower((unsigned char) ch);
			n = strchr(xdigits, ch) - xdigits;
			assert(n >= 0 && n <= 15);
			if (dirty == 0)
				tmp = n;
			else
				tmp = (tmp << 4) | n;
			if (++dirty == 2)
			{
				if (size-- <= 0U)
					goto emsgsize;
				*dst++ = (u_char) tmp;
				dirty = 0;
			}
		}
		if (dirty)
		{						/* 半字节落单？ */
			if (size-- <= 0U)
				goto emsgsize;
			*dst++ = (u_char) (tmp << 4);
		}
	}
	else if (isdigit((unsigned char) ch))
	{
		/* 十进制：吃点分数字串 */
		for (;;)
		{
			tmp = 0;
			do
			{
				n = strchr(digits, ch) - digits;
				assert(n >= 0 && n <= 9);
				tmp *= 10;
				tmp += n;
				if (tmp > 255)
					goto enoent;
			} while ((ch = *src++) != '\0' &&
					 isdigit((unsigned char) ch));
			if (size-- <= 0U)
				goto emsgsize;
			*dst++ = (u_char) tmp;
			if (ch == '\0' || ch == '/')
				break;
			if (ch != '.')
				goto enoent;
			ch = *src++;
			if (!isdigit((unsigned char) ch))
				goto enoent;
		}
	}
	else
		goto enoent;

	bits = -1;
	if (ch == '/' && isdigit((unsigned char) src[0]) && dst > odst)
	{
		/* CIDR 宽度说明符，其后不得再有 */
		ch = *src++;			/* 跳过 /。 */
		bits = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			bits *= 10;
			bits += n;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (ch != '\0')
			goto enoent;
		if (bits > 32)
			goto emsgsize;
	}

	/* 除非预取到 EOS，否则便是灭顶之灾。 */
	if (ch != '\0')
		goto enoent;

	/* 若未向目标写入任何内容，说明没找到地址。 */
	if (dst == odst)
		goto enoent;
	/* 未给定 CIDR 时，按网络类别推断宽度。 */
	if (bits == -1)
	{
		if (*odst >= 240)		/* E 类 */
			bits = 32;
		else if (*odst >= 224)	/* D 类 */
			bits = 8;
		else if (*odst >= 192)	/* C 类 */
			bits = 24;
		else if (*odst >= 128)	/* B 类 */
			bits = 16;
		else
			/* A 类 */
			bits = 8;
		/* 若推断掩码窄于指定八位组，则加宽。 */
		if (bits < ((dst - odst) * 8))
			bits = (dst - odst) * 8;

		/*
		 * 若 D 类地址没有指定附加位，则将位数
		 * 调整为 4。
		 */
		if (bits == 8 && *odst == 224)
			bits = 4;
	}
	/* 扩展网络以覆盖实际掩码。 */
	while (bits > ((dst - odst) * 8))
	{
		if (size-- <= 0U)
			goto emsgsize;
		*dst++ = '\0';
	}
	return bits;

enoent:
	errno = ENOENT;
	return -1;

emsgsize:
	errno = EMSGSIZE;
	return -1;
}

/*
 * int
 * inet_net_pton_ipv4(af, src, dst, *bits)
 *	将网络地址由呈现格式转换为网络格式。
 *	接受该 "af" 的 inet_pton() 输入外加 "/CIDR" 后缀。
 *	假定 "dst" 对其 "af" 足够大。"bits" 将被设为
 *	/CIDR 前缀长度，可有默认值（如 IPv4 的 /32）。
 * 返回:
 *	出错时为 -1（检查 errno；ENOENT 表示格式有误）。
 *	转换成功时为 0。
 * note:
 *	192.5.5.1/28 带有非零主机部分，这意味着它并不是
 *	inet_cidr_pton() 所要求的网络，但可以是带掩码的
 *	主机地址。
 * 作者:
 *	Paul Vixie (ISC), October 1998
 */
static int
inet_net_pton_ipv4(const char *src, u_char *dst)
{
	static const char digits[] = "0123456789";
	const u_char *odst = dst;
	int			n,
				ch,
				tmp,
				bits;
	size_t		size = 4;

	/* 取尾数。 */
	while (ch = *src++, isdigit((unsigned char) ch))
	{
		tmp = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			tmp *= 10;
			tmp += n;
			if (tmp > 255)
				goto enoent;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (size-- == 0)
			goto emsgsize;
		*dst++ = (u_char) tmp;
		if (ch == '\0' || ch == '/')
			break;
		if (ch != '.')
			goto enoent;
	}

	/* 取前缀长度（若有） */
	bits = -1;
	if (ch == '/' && isdigit((unsigned char) src[0]) && dst > odst)
	{
		/* CIDR 宽度说明符，其后不得再有 */
		ch = *src++;			/* 跳过 /。 */
		bits = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			bits *= 10;
			bits += n;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (ch != '\0')
			goto enoent;
		if (bits > 32)
			goto emsgsize;
	}

	/* 除非预取到 EOS，否则便是灭顶之灾。 */
	if (ch != '\0')
		goto enoent;

	/* 四个八位组全给出时，前缀长度才默认为 /32 */
	if (bits == -1)
	{
		if (dst - odst == 4)
			bits = 32;
		else
			goto enoent;
	}

	/* 若未向目标写入任何内容，说明没找到地址。 */
	if (dst == odst)
		goto enoent;

	/* 前缀长度超出尾数时，情况不妙。 */
	if ((bits / 8) > (dst - odst))
		goto enoent;

	/* 地址补足四个八位组 */
	while (size-- > 0)
		*dst++ = 0;

	return bits;

enoent:
	errno = ENOENT;
	return -1;

emsgsize:
	errno = EMSGSIZE;
	return -1;
}

static int
getbits(const char *src, int *bitsp)
{
	static const char digits[] = "0123456789";
	int			n;
	int			val;
	char		ch;

	val = 0;
	n = 0;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		pch = strchr(digits, ch);
		if (pch != NULL)
		{
			if (n++ != 0 && val == 0)	/* 不许前导零 */
				return 0;
			val *= 10;
			val += (pch - digits);
			if (val > 128)		/*范围*/
				return 0;
			continue;
		}
		return 0;
	}
	if (n == 0)
		return 0;
	*bitsp = val;
	return 1;
}

static int
getv4(const char *src, u_char *dst, int *bitsp)
{
	static const char digits[] = "0123456789";
	u_char	   *odst = dst;
	int			n;
	u_int		val;
	char		ch;

	val = 0;
	n = 0;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		pch = strchr(digits, ch);
		if (pch != NULL)
		{
			if (n++ != 0 && val == 0)	/* 不许前导零 */
				return 0;
			val *= 10;
			val += (pch - digits);
			if (val > 255)		/*范围*/
				return 0;
			continue;
		}
		if (ch == '.' || ch == '/')
		{
			if (dst - odst > 3) /* 八位组多？ */
				return 0;
			*dst++ = val;
			if (ch == '/')
				return getbits(src, bitsp);
			val = 0;
			n = 0;
			continue;
		}
		return 0;
	}
	if (n == 0)
		return 0;
	if (dst - odst > 3)			/* 八位组多？ */
		return 0;
	*dst++ = val;
	return 1;
}

static int
inet_net_pton_ipv6(const char *src, u_char *dst)
{
	return inet_cidr_pton_ipv6(src, dst, 16);
}

#define NS_IN6ADDRSZ 16
#define NS_INT16SZ 2
#define NS_INADDRSZ 4

static int
inet_cidr_pton_ipv6(const char *src, u_char *dst, size_t size)
{
	static const char xdigits_l[] = "0123456789abcdef",
				xdigits_u[] = "0123456789ABCDEF";
	u_char		tmp[NS_IN6ADDRSZ],
			   *tp,
			   *endp,
			   *colonp;
	const char *xdigits,
			   *curtok;
	int			ch,
				saw_xdigit;
	u_int		val;
	int			digits;
	int			bits;

	if (size < NS_IN6ADDRSZ)
		goto emsgsize;

	memset((tp = tmp), '\0', NS_IN6ADDRSZ);
	endp = tp + NS_IN6ADDRSZ;
	colonp = NULL;
	/* 前导 :: 需要特殊处理。 */
	if (*src == ':')
		if (*++src != ':')
			goto enoent;
	curtok = src;
	saw_xdigit = 0;
	val = 0;
	digits = 0;
	bits = -1;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
			pch = strchr((xdigits = xdigits_u), ch);
		if (pch != NULL)
		{
			val <<= 4;
			val |= (pch - xdigits);
			if (++digits > 4)
				goto enoent;
			saw_xdigit = 1;
			continue;
		}
		if (ch == ':')
		{
			curtok = src;
			if (!saw_xdigit)
			{
				if (colonp)
					goto enoent;
				colonp = tp;
				continue;
			}
			else if (*src == '\0')
				goto enoent;
			if (tp + NS_INT16SZ > endp)
				goto enoent;
			*tp++ = (u_char) (val >> 8) & 0xff;
			*tp++ = (u_char) val & 0xff;
			saw_xdigit = 0;
			digits = 0;
			val = 0;
			continue;
		}
		if (ch == '.' && ((tp + NS_INADDRSZ) <= endp) &&
			getv4(curtok, tp, &bits) > 0)
		{
			tp += NS_INADDRSZ;
			saw_xdigit = 0;
			break;				/* inet_pton4() 读到 '\0'。 */
		}
		if (ch == '/' && getbits(src, &bits) > 0)
			break;
		goto enoent;
	}
	if (saw_xdigit)
	{
		if (tp + NS_INT16SZ > endp)
			goto enoent;
		*tp++ = (u_char) (val >> 8) & 0xff;
		*tp++ = (u_char) val & 0xff;
	}
	if (bits == -1)
		bits = 128;

	endp = tmp + 16;

	if (colonp != NULL)
	{
		/*
		 * 由于某些 memmove() 实现不能正确处理重叠
		 * 区域，我们将手工移位。
		 */
		const int	n = tp - colonp;
		int			i;

		if (tp == endp)
			goto enoent;
		for (i = 1; i <= n; i++)
		{
			endp[-i] = colonp[n - i];
			colonp[n - i] = 0;
		}
		tp = endp;
	}
	if (tp != endp)
		goto enoent;

	/*
	 * 复制出结果。
	 */
	memcpy(dst, tmp, NS_IN6ADDRSZ);

	return bits;

enoent:
	errno = ENOENT;
	return -1;

emsgsize:
	errno = EMSGSIZE;
	return -1;
}
