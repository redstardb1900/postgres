/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  INET 数据类型操作的声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/inet.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INET_H
#define INET_H

#include "fmgr.h"

/*
 *	IP 地址内部存储格式
 *	(INET 与 CIDR)：
 */
typedef struct
{
	unsigned char family;		/* PGSQL_AF_INET/PGSQL_AF_INET6 */
	unsigned char bits;			/*  网络掩码位数  */
	unsigned char ipaddr[16];	/*  最多 128 位地址  */
} inet_struct;

/*
 * family 字段使用这些值。
 *
 * 把所有非 AF_INET 类型引用到 AF_INET，使我们能
 * 在没有相应地址族的机器上工作（如尚无 AF_INET6
 * 时的 inet6），且不引起转储/重载。7.4 前库在盘
 * 上用 AF_INET 作族类型。
 * 见上。
 */
#define PGSQL_AF_INET	(AF_INET + 0)
#define PGSQL_AF_INET6	(AF_INET + 1)

/*
 * INET 与 CIDR 在 Postgres 内都表示为 varlena
 * 对象，即上述结构前有 varlena 头。本结构描绘
 * "未压缩"情形下内存中的实际内容。注意最大数据
 * 仅 18 字节，故 INET/CIDR 几乎总用 1 字节头
 * varlena 存入元组。但仍须能应付 4 字节头格式，
 * 因各类代码可能好心"解压" 1 字节头 datum。
 * 见上。
 * 见上。
 */
typedef struct
{
	char		vl_len_[4];		/*  勿直接触碰此字段！  */
	inet_struct inet_data;
} inet;

/*
 *	访问宏。用 VARDATA_ANY 以便不 detoast 即可处理
 *	短头 varlena。这需要技巧：VARDATA_ANY 假定头已
 *	填好，而构造新值时并非如此（直到调用
 *	SET_INET_VARSIZE，通常只能在末尾做）。因此我们
 *	总用 palloc0 把新分配值清零。零长度字对
 *	VARDATA_ANY 看起来像非 1 字节情形，从而正确构
 *	造未压缩值。
 *	见上。
 *
 *	注意 ip_addrsize()、ip_maxbits() 与 SET_INET_VARSIZE()
 *	要求 family 已正确设置。
 */
#define ip_family(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->bits)

#define ip_addr(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->ipaddr)

#define ip_addrsize(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 4 : 16)

#define ip_maxbits(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 32 : 128)

#define SET_INET_VARSIZE(dst) \
	SET_VARSIZE(dst, VARHDRSZ + offsetof(inet_struct, ipaddr) + \
				ip_addrsize(dst))


/*
 *	这是 MAC 地址的内部存储格式：
 */
typedef struct macaddr
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
} macaddr;

/*
 *	这是 MAC8 地址的内部存储格式：
 */
typedef struct macaddr8
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
	unsigned char g;
	unsigned char h;
} macaddr8;

/*
 * fmgr 接口宏
 */
static inline inet *
DatumGetInetPP(Datum X)
{
	return (inet *) PG_DETOAST_DATUM_PACKED(X);
}

static inline Datum
InetPGetDatum(const inet *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_INET_PP(n) DatumGetInetPP(PG_GETARG_DATUM(n))
#define PG_RETURN_INET_P(x) return InetPGetDatum(x)

/*  过时变体  */
static inline inet *
DatumGetInetP(Datum X)
{
	return (inet *) PG_DETOAST_DATUM(X);
}
#define PG_GETARG_INET_P(n) DatumGetInetP(PG_GETARG_DATUM(n))

/*  macaddr 是定长按引用传递的数据类型  */
static inline macaddr *
DatumGetMacaddrP(Datum X)
{
	return (macaddr *) DatumGetPointer(X);
}

static inline Datum
MacaddrPGetDatum(const macaddr *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MACADDR_P(n) DatumGetMacaddrP(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR_P(x) return MacaddrPGetDatum(x)

/*  macaddr8 是定长按引用传递的数据类型  */
static inline macaddr8 *
DatumGetMacaddr8P(Datum X)
{
	return (macaddr8 *) DatumGetPointer(X);
}

static inline Datum
Macaddr8PGetDatum(const macaddr8 *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_MACADDR8_P(n) DatumGetMacaddr8P(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR8_P(x) return Macaddr8PGetDatum(x)

/*
 * network.c 中的支持函数
 */
extern inet *cidr_set_masklen_internal(const inet *src, int bits);
extern int	bitncmp(const unsigned char *l, const unsigned char *r, int n);
extern int	bitncommon(const unsigned char *l, const unsigned char *r, int n);

#endif							/* INET_H */
