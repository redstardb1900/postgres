/*
 *	PostgreSQL 的 INET 与 CIDR 类型定义。
 *
 *	src/backend/utils/adt/network.c
 *
 *	纪念 Postel，1998-10-16
 */

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/ip.h"
#include "lib/hyperloglog.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"


/*
 * IPv4 网络掩码长度取值 0–32，在可能时于
 * inet/cidr 缩写键中用 6 位表示。
 *
 * IPv4 inet/cidr 缩写键最多可用 25 位表示子网
 * 分量。
 */
#define ABBREV_BITS_INET4_NETMASK_SIZE	6
#define ABBREV_BITS_INET4_SUBNET		25

/* inet/cidr 的 sortsupport */
typedef struct
{
	int64		input_count;	/* 已见到的非空值个数 */
	bool		estimating;		/* 正在估算基数时为 true */

	hyperLogLogState abbr_card; /* 基数估算器 */
} network_sortsupport_state;

static int32 network_cmp_internal(inet *a1, inet *a2);
static int	network_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool network_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum network_abbrev_convert(Datum original, SortSupport ssup);
static List *match_network_function(Node *leftop,
									Node *rightop,
									int indexarg,
									Oid funcid,
									Oid opfamily);
static List *match_network_subset(Node *leftop,
								  Node *rightop,
								  bool is_eq,
								  Oid opfamily);
static bool addressOK(unsigned char *a, int bits, int family);
static inet *internal_inetpl(inet *ip, int64 addend);


/*
 * 公用 INET/CIDR 输入例程
 */
static inet *
network_in(char *src, bool is_cidr, Node *escontext)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	/*
	 * 首先判断是 IPv6 还是 IPv4。IPv6 地址中
	 * 某处会有冒号:（实际上有好几个），若有
	 * 则假定为 V6，否则假定为 V4。
	 */

	if (strchr(src, ':') != NULL)
		ip_family(dst) = PGSQL_AF_INET6;
	else
		ip_family(dst) = PGSQL_AF_INET;

	bits = pg_inet_net_pton(ip_family(dst), src, ip_addr(dst),
							is_cidr ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > ip_maxbits(dst)))
		ereturn(escontext, NULL,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		/* translator: first %s is inet or cidr */
				 errmsg("invalid input syntax for type %s: \"%s\"",
						is_cidr ? "cidr" : "inet", src)));

	/*
	 * 错误检查：CIDR 值在 masklen 之外不得有位置位。
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(dst), bits, ip_family(dst)))
			ereturn(escontext, NULL,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid cidr value: \"%s\"", src),
					 errdetail("Value has bits set to right of mask.")));
	}

	ip_bits(dst) = bits;
	SET_INET_VARSIZE(dst);

	return dst;
}

Datum
inet_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, false, fcinfo->context));
}

Datum
cidr_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, true, fcinfo->context));
}


/*
 * 公用 INET/CIDR 输出例程
 */
static char *
network_out(inet *src, bool is_cidr)
{
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *dst;
	int			len;

	dst = pg_inet_net_ntop(ip_family(src), ip_addr(src), ip_bits(src),
						   tmp, sizeof(tmp));
	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 对 CIDR，若无 /n 则补上 */
	if (is_cidr && strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
	}

	return pstrdup(tmp);
}

Datum
inet_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, false));
}

Datum
cidr_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, true));
}


/*
 *	network_recv - 将外部二进制格式转为 inet
 *
 * 外部表示（各一字节）为
 * 族、位数、is_cidr、地址长度、网络字节序地址。
 *
 * 保留 is_cidr 多半出于历史原因，不过或许便于
 * 客户端侧代码共用。输出时正确发送，
 * 输入时忽略该值。
 */
static inet *
network_recv(StringInfo buf, bool is_cidr)
{
	inet	   *addr;
	char	   *addrptr;
	int			bits;
	int			nb,
				i;

	/* 确保 CIDR 值中未用位被清零 */
	addr = (inet *) palloc0(sizeof(inet));

	ip_family(addr) = pq_getmsgbyte(buf);
	if (ip_family(addr) != PGSQL_AF_INET &&
		ip_family(addr) != PGSQL_AF_INET6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid address family in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > ip_maxbits(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid bits in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	ip_bits(addr) = bits;
	i = pq_getmsgbyte(buf);		/* 忽略 is_cidr */
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid length in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));

	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * 错误检查：CIDR 值在 masklen 之外不得有位置位。
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(addr), bits, ip_family(addr)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid external \"cidr\" value"),
					 errdetail("Value has bits set to right of mask.")));
	}

	SET_INET_VARSIZE(addr);

	return addr;
}

Datum
inet_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, false));
}

Datum
cidr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, true));
}


/*
 *	network_send - 将 inet 转为二进制格式
 */
static bytea *
network_send(inet *addr, bool is_cidr)
{
	StringInfoData buf;
	char	   *addrptr;
	int			nb,
				i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, ip_family(addr));
	pq_sendbyte(&buf, ip_bits(addr));
	pq_sendbyte(&buf, is_cidr);
	nb = ip_addrsize(addr);
	if (nb < 0)
		nb = 0;
	pq_sendbyte(&buf, nb);
	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		pq_sendbyte(&buf, addrptr[i]);
	return pq_endtypsend(&buf);
}

Datum
inet_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, false));
}

Datum
cidr_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, true));
}


Datum
inet_to_cidr(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits;

	bits = ip_bits(src);

	/* 安全检查 */
	if ((bits < 0) || (bits > ip_maxbits(src)))
		elog(ERROR, "invalid inet bit length: %d", bits);

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

Datum
inet_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);
	inet	   *dst;

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	/* 克隆原始数据 */
	dst = (inet *) palloc(VARSIZE_ANY(src));
	memcpy(dst, src, VARSIZE_ANY(src));

	ip_bits(dst) = bits;

	PG_RETURN_INET_P(dst);
}

Datum
cidr_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

/*
 * 复制 src 并将掩码长度设为 bits（须对该族合法）
 */
inet *
cidr_set_masklen_internal(const inet *src, int bits)
{
	inet	   *dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = ip_family(src);
	ip_bits(dst) = bits;

	if (bits > 0)
	{
		Assert(bits <= ip_maxbits(dst));

		/* 克隆地址的相应字节，其余置 0 */
		memcpy(ip_addr(dst), ip_addr(src), (bits + 7) / 8);

		/* 清除末尾不完整字节中不需要的位 */
		if (bits % 8)
			ip_addr(dst)[bits / 8] &= ~(0xFF >> (bits % 8));
	}

	/* 正确设置 varlena 头 */
	SET_INET_VARSIZE(dst);

	return dst;
}

/*
 *	排序与 inet/cidr 比较的基本比较函数。
 *
 * 先比较网络部分的公共位，再比较
 * 网络部分长度，最后比较整段未掩码地址。
 * 效果是网络部分为主排序键；网络部分
 * 相同时再按主机部分排序。仅当掩码右侧
 * 地址位保证为零时，这对 CIDR 才合理；
 * 否则逻辑相等的 CIDR 也可能比出不同。
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == ip_family(a2))
	{
		int			order;

		order = bitncmp(ip_addr(a1), ip_addr(a2),
						Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return bitncmp(ip_addr(a1), ip_addr(a2), ip_maxbits(a1));
	}

	return ip_family(a1) - ip_family(a2);
}

Datum
network_cmp(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_INT32(network_cmp_internal(a1, a2));
}

/*
 * SortSupport 策略例程
 */
Datum
network_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = network_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		network_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(network_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = network_abbrev_convert;
		ssup->abbrev_abort = network_abbrev_abort;
		ssup->abbrev_full_comparator = network_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport 比较函数
 */
static int
network_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	inet	   *arg1 = DatumGetInetPP(x);
	inet	   *arg2 = DatumGetInetPP(y);

	return network_cmp_internal(arg1, arg2);
}

/*
 * 用于估算缩写键优化效果的回调。
 *
 * 我们完全不关心非缩写数据的基数，因为权威的
 * inet 比较器内没有相等快速路径。
 */
static bool
network_abbrev_abort(int memtupcount, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * 若已有 >100k 个不同值，那么即使要排序数十亿
	 * 行，我们大概率仍不吃亏，而撤销这么多行缩写
	 * 的代价很可能不值得。到这时我们停止计数，
	 * 因为已知已完全投入。
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
#endif
		uss->estimating = false;
		return false;
	}

	/*
	 * 目标最小基数为每 ~2k 个非空输入 1 个。0.5 行
	 * 容差因子使我们在真正病态数据上更早中止——
	 * 即前 2k 个(非空)行中恰好只有一个缩写
	 * 值。
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "network_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);
#endif

	return false;
}

/*
 * SortSupport 转换例程。把原始 inet/cidr 表示
 * 转为可用简单三路无符号整数比较的缩写键
 * 表示。缩写比较按编码方案遵循
 * network_cmp_internal() 的 inet/cidr 排序规则，
 * 并通过谨慎填充来规整键。
 *
 * 背景：inet 值有三大分量（例如地址
 * 1.2.3.4/24）：
 *
 *     * 网络，即经掩码位 (1.2.3.0)。
 *     * 掩码长度 (/24)。
 *     * 子网，即掩码外的位 (0.0.0.4)。
 *
 * cidr 值相同，但只有前两个分量——
 * 其子网位必须全零 (1.2.3.0/24)。
 *
 * IPv4 与 IPv6 构成相同，差别在于 IPv4 最多
 * 32 位而 IPv6 为 64 位，故在 IPv6 中
 * 各部分可能更大。
 *
 * inet/cidr 按下列规则比较。任一步发现不等即
 * 结束比较。若某规则平手，算法则落入
 * 下一条以打破平手：
 *
 *     1. IPv4 总排在 IPv6 之前。
 *     2. 比较网络位。
 *     3. 比较掩码长度。
 *     4. 比较全部位（能走到这里则两边的
 *        经掩码位与掩码长度已相等，实际只在
 *        比较子网位）。
 *
 * 为 SortSupport 生成缩写键时，尽量多装入
 * datum，并保证把这些键当整数比较时仍
 * 遵守上述规则。具体内容取决于地址族与 datum
 * 宽.
 *
 * IPv4
 * ----
 *
 * 4字节datum:
 *
 * 以 1 位地址族起头（IPv4 或 IPv6；下列各情形
 * 皆有此位），后接除 1 位外的全部经掩码位。
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits network   |     (1 bit network
 * |  family  |     (truncated)     |      omitted)
 * +----------+---------------------+
 *
 * 8字节datum:
 *
 * 有空间存放全部经掩码位，后接网络掩码长度，
 * 再接 25 位子网位（实践中 25 位通常够用）。
 * cidr 的子网位恒为零。
 *
 * +----------+-----------------------+--------------+--------------------+
 * | 1 bit IP |    32 bits network    |    6 bits    |   25 bits subnet   |
 * |  family  |        (full)         | network size |    (truncated)     |
 * +----------+-----------------------+--------------+--------------------+
 *
 * IPv6
 * ----
 *
 * 4字节datum:
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits network   |    (up to 97 bits
 * |  family  |     (truncated)     |   network omitted)
 * +----------+---------------------+
 *
 * 8字节datum:
 *
 * +----------+---------------------------------+
 * | 1 bit IP |         63 bits network         |    (up to 65 bits
 * |  family  |           (truncated)           |   network omitted)
 * +----------+---------------------------------+
 */
static Datum
network_abbrev_convert(Datum original, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	inet	   *authoritative = DatumGetInetPP(original);
	Datum		res,
				ipaddr_datum,
				subnet_bitmask,
				network;
	int			subnet_size;

	Assert(ip_family(authoritative) == PGSQL_AF_INET ||
		   ip_family(authoritative) == PGSQL_AF_INET6);

	/*
	 * 取 IP 地址前 4 或 8 字节，得到无符号整数
	 * 表示。IPv4 总取全部 4 字节。IPv6 在 8 字节
	 * datum 上取前 8 字节，否则取 4
	 * 字节。
	 *
	 * 我们消费的是无符号字符数组，小端系统须
	 * 字节交换（inet 的 ipaddr 字段最高有效字节在
	 * 前).
	 */
	if (ip_family(authoritative) == PGSQL_AF_INET)
	{
		uint32		ipaddr_datum32;

		memcpy(&ipaddr_datum32, ip_addr(authoritative), sizeof(uint32));

		/* 小端机器上必须字节交换 */
#ifndef WORDS_BIGENDIAN
		ipaddr_datum = pg_bswap32(ipaddr_datum32);
#else
		ipaddr_datum = ipaddr_datum32;
#endif

		/* 初始化结果，不置 ipfamily 位 */
		res = (Datum) 0;
	}
	else
	{
		memcpy(&ipaddr_datum, ip_addr(authoritative), sizeof(Datum));

		/* 小端机器上必须字节交换 */
		ipaddr_datum = DatumBigEndianToNative(ipaddr_datum);

		/* 初始化结果，置上 ipfamily（最高有效）位 */
		res = ((Datum) 1) << (SIZEOF_DATUM * BITS_PER_BYTE - 1);
	}

	/*
	 * ipaddr_datum 须"拆分"：高位进入缩写键的
	 * "网络"分量（常因掩码在末尾清零），
	 * 低位在有空间时进入"子网"分量。
	 * 通常先生成临时 datum 子网位掩码，生成
	 * 子网位时还可复用。注意子网位仅用于
	 * datum 为 8 字节平台上的 IPv4
	 * 值。
	 *
	 * 子网位数用于生成 datum 子网
	 * 位掩码。例如 /24 的 IPv4 有 8 位子网位，
	 * 因 32-24=8，最终子网掩码为 B'1111 1111'。
	 * 若 ipaddr 位无法全部装入 datum，须显式处理，
	 * 否则会错误地掩掉 IPv6 值的网络
	 * 分量。
	 */
	subnet_size = ip_maxbits(authoritative) - ip_bits(authoritative);
	Assert(subnet_size >= 0);
	/* 子网大小须适配前缀 ipaddr 情形 */
	subnet_size %= SIZEOF_DATUM * BITS_PER_BYTE;
	if (ip_bits(authoritative) == 0)
	{
		/* 尽量多地把 ipaddr 位装入子网 */
		subnet_bitmask = ((Datum) 0) - 1;
		network = 0;
	}
	else if (ip_bits(authoritative) < SIZEOF_DATUM * BITS_PER_BYTE)
	{
		/* 在网络与子网之间拆分 ipaddr 位 */
		subnet_bitmask = (((Datum) 1) << subnet_size) - 1;
		network = ipaddr_datum & ~subnet_bitmask;
	}
	else
	{
		/* 尽量多地把 ipaddr 位装入网络 */
		subnet_bitmask = 0;
		network = ipaddr_datum;
	}

#if SIZEOF_DATUM == 8
	if (ip_family(authoritative) == PGSQL_AF_INET)
	{
		/*
		 * 8 字节 datum 上的 IPv4：保留全部 32 经掩码位、掩码
		 * 长度及最高 25 位子网位
		 */
		Datum		netmask_size = (Datum) ip_bits(authoritative);
		Datum		subnet;

		/*
		 * 左移 31 位：6 位掩码长度 + 25 位子网位。
		 *
		 * 我们不区分因掩码而为零的网络位与"真正"/
		 * 未掩码的零位。若缩写比较靠比较非掩码非零
		 * 位与掩码/清零位决出，则实际是按
		 * ip_bits() 决出，即便比较尚未到达
		 * netmask_size
		 * 位.
		 */
		network <<= (ABBREV_BITS_INET4_NETMASK_SIZE +
					 ABBREV_BITS_INET4_SUBNET);

		/* 左移 size 为末尾子网位腾出空间 */
		netmask_size <<= ABBREV_BITS_INET4_SUBNET;

		/* 提取子网位，不对其移位 */
		subnet = ipaddr_datum & subnet_bitmask;

		/*
		 * 若子网位超过 25，则装不下全部。下移
		 * 子网，以免破坏本应只给
		 * netmask_size 的位。
		 *
		 * 像这样丢弃最低有效子网位是正确的，
		 * 因为在子网层决出的缩写比较，此前
		 * netmask_size/ip_bits() 必已相等才
		 * 能至此。
		 */
		if (subnet_size > ABBREV_BITS_INET4_SUBNET)
			subnet >>= subnet_size - ABBREV_BITS_INET4_SUBNET;

		/*
		 * 组装最终缩写键，且不破坏必须保持为
		 * 零的 ipfamily 位。
		 */
		res |= network | netmask_size | subnet;
	}
	else
#endif
	{
		/*
		 * 4 字节 datum，或 8 字节 datum 上的 IPv6：在最终
		 * 缩写键中尽量多装经掩码位。避免
		 * 破坏先前已置的 ipfamily 位。
		 */
		res |= network >> 1;
	}

	uss->input_count += 1;

	/* 哈希缩写键 */
	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	return res;
}

/*
 *	布尔序关系测试.
 */
Datum
network_lt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) < 0);
}

Datum
network_le(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) <= 0);
}

Datum
network_eq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) == 0);
}

Datum
network_ge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) >= 0);
}

Datum
network_gt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) > 0);
}

Datum
network_ne(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) != 0);
}

/*
 * MIN/MAX 支持函数。
 */
Datum
network_smaller(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) < 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

Datum
network_larger(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) > 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

/*
 * inet/cidr 哈希索引的支持函数。
 */
Datum
hashinet(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	/* XXX 假定数据结构中无填充字节 */
	return hash_any((unsigned char *) VARDATA_ANY(addr), addrsize + 2);
}

Datum
hashinetextended(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	return hash_any_extended((unsigned char *) VARDATA_ANY(addr), addrsize + 2,
							 PG_GETARG_INT64(1));
}

/*
 *	布尔网络包含测试。
 */
Datum
network_sub(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_overlap(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(bitncmp(ip_addr(a1), ip_addr(a2),
							   Min(ip_bits(a1), ip_bits(a2))) == 0);
	}

	PG_RETURN_BOOL(false);
}

/*
 * 网络子集/超网算子的规划器支持函数
 */
Datum
network_subset_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestIndexCondition))
	{
		/* 尝试把算子/函数调用转为索引条件 */
		SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;

		if (is_opclause(req->node))
		{
			OpExpr	   *clause = (OpExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
		else if (is_funcclause(req->node))	/* 防万一 */
		{
			FuncExpr   *clause = (FuncExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * match_network_function
 *	  尝试为网络子集/超网函数生成索引条件。
 *
 * 本层只负责识别函数，并在必要时交换
 * 参数。
 */
static List *
match_network_function(Node *leftop,
					   Node *rightop,
					   int indexarg,
					   Oid funcid,
					   Oid opfamily)
{
	switch (funcid)
	{
		case F_NETWORK_SUB:
			/* 索引键必须在左侧 */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, false, opfamily);

		case F_NETWORK_SUBEQ:
			/* 索引键必须在左侧 */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, true, opfamily);

		case F_NETWORK_SUP:
			/* 索引键必须在右侧 */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, false, opfamily);

		case F_NETWORK_SUPEQ:
			/* 索引键必须在右侧 */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, true, opfamily);

		default:

			/*
			 * 仅当有人把此支持函数挂到意外函数上
			 * 才会走到这里。或许该报错，但目前
			 * 先不动。
			 */
			return NIL;
	}
}

/*
 * match_network_subset
 *	  尝试为网络子集函数生成索引条件。
 */
static List *
match_network_subset(Node *leftop,
					 Node *rightop,
					 bool is_eq,
					 Oid opfamily)
{
	List	   *result;
	Datum		rightopval;
	Oid			datatype = INETOID;
	Oid			opr1oid;
	Oid			opr2oid;
	Datum		opr1right;
	Datum		opr2right;
	Expr	   *expr;

	/*
	 * 非常量或 NULL 比较值时无能为力。
	 *
	 * 由于我们只处理 RHS 为硬常量的情形，
	 * 它当然也是伪常量，不必再担心
	 * 去验证这一点。
	 */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return NIL;
	rightopval = ((Const *) rightop)->constvalue;

	/*
	 * 必须检查索引的算子族是否支持我们要用的
	 * 算子
	 *
	 * 我们坚持算子族必须是预期的那一个，否则若
	 * 有人做出带相同算子的反向排序算子族，就会
	 * 做错事。
	 */
	if (opfamily != NETWORK_BTREE_FAM_OID)
		return NIL;

	/*
	 * 生成子句 "key >= network_scan_first(rightopval)"，若
	 * 算子禁止相等则用 >.
	 *
	 * 注意：既然本函数对算子族与数据类型只支持
	 * 固定值，本可硬编码算子 OID 而不去
	 * 查找。但目前仍以通用做法为妥。
	 */
	if (is_eq)
	{
		opr1oid = get_opfamily_member(opfamily, datatype, datatype,
									  BTGreaterEqualStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no >= operator for opfamily %u", opfamily);
	}
	else
	{
		opr1oid = get_opfamily_member(opfamily, datatype, datatype,
									  BTGreaterStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no > operator for opfamily %u", opfamily);
	}

	opr1right = network_scan_first(rightopval);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* 不可排序 */
											-1, opr1right,
											false, false),
						 InvalidOid, InvalidOid);
	result = list_make1(expr);

	/* 生成子句 "key <= network_scan_last(rightopval)" */

	opr2oid = get_opfamily_member(opfamily, datatype, datatype,
								  BTLessEqualStrategyNumber);
	if (opr2oid == InvalidOid)
		elog(ERROR, "no <= operator for opfamily %u", opfamily);

	opr2right = network_scan_last(rightopval);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* 不可排序 */
											-1, opr2right,
											false, false),
						 InvalidOid, InvalidOid);
	result = lappend(result, expr);

	return result;
}


/*
 * 从网络数据类型提取数据。
 */
Datum
network_host(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *ptr;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	/* 强制按最大位数显示，无视 masklen… */
	if (pg_inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
						 tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 若有 /n 则去掉（现在不应出现） */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

/*
 * network_show 实现 inet/cidr 到 text 的强制转换。这与
 * network_out 行为并不完全相同，故不能改用
 * CoerceViaIO。
 */
Datum
network_show(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int			len;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (pg_inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
						 tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* 若无 /n 则补上(本无) */
	if (strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
	}

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
inet_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = pg_inet_net_ntop(ip_family(ip), ip_addr(ip),
						   ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
cidr_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = pg_inet_cidr_ntop(ip_family(ip), ip_addr(ip),
							ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format cidr value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
network_masklen(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	PG_RETURN_INT32(ip_bits(ip));
}

Datum
network_family(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	switch (ip_family(ip))
	{
		case PGSQL_AF_INET:
			PG_RETURN_INT32(4);
			break;
		case PGSQL_AF_INET6:
			PG_RETURN_INT32(6);
			break;
		default:
			PG_RETURN_INT32(0);
			break;
	}
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* 确保未用位被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	for (byte = 0; byte < maxbytes; byte++)
	{
		if (bits >= 8)
		{
			mask = 0x00;
			bits -= 8;
		}
		else if (bits == 0)
			mask = 0xff;
		else
		{
			mask = 0xff >> bits;
			bits = 0;
		}

		b[byte] = a[byte] | mask;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* 确保未用位被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = a[byte] & mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *b;

	/* 确保未用位被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *b;

	/* 确保未用位被清零 */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_maxbits(ip) - ip_bits(ip);
	b = ip_addr(dst);

	byte = maxbytes - 1;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff >> (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte--;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

/*
 * 若地址同族则返回 true，否则 false。用于检查
 * 能否创建同时包含这两个网络的网络。
 */
Datum
inet_same_family(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(ip_family(a1) == ip_family(a2));
}

/*
 * 返回包含两个输入的最小 CIDR。
 */
Datum
inet_merge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0),
			   *a2 = PG_GETARG_INET_PP(1);
	int			commonbits;

	if (ip_family(a1) != ip_family(a2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot merge addresses from different families")));

	commonbits = bitncommon(ip_addr(a1), ip_addr(a2),
							Min(ip_bits(a1), ip_bits(a2)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(a1, commonbits));
}

/*
 * 把网络数据类型的值转为近似标量。
 * 用于估算涉及网络类型的不等算子的
 * 选择率。
 *
 * 失败时(如不支持的 typid)将 *failure 置 true；
 * 否则不改该变量。
 */
double
convert_network_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case INETOID:
		case CIDROID:
			{
				inet	   *ip = DatumGetInetPP(value);
				int			len;
				double		res;
				int			i;

				/*
				 * 注意 IPv6 我们不用完整地址。
				 */
				if (ip_family(ip) == PGSQL_AF_INET)
					len = 4;
				else
					len = 5;

				res = ip_family(ip);
				for (i = 0; i < len; i++)
				{
					res *= 256;
					res += ip_addr(ip)[i];
				}
				return res;
			}
		case MACADDROID:
			{
				macaddr    *mac = DatumGetMacaddrP(value);
				double		res;

				res = (mac->a << 16) | (mac->b << 8) | (mac->c);
				res *= 256 * 256 * 256;
				res += (mac->d << 16) | (mac->e << 8) | (mac->f);
				return res;
			}
		case MACADDR8OID:
			{
				macaddr8   *mac = DatumGetMacaddr8P(value);
				double		res;

				res = (mac->a << 24) | (mac->b << 16) | (mac->c << 8) | (mac->d);
				res *= ((double) 256) * 256 * 256 * 256;
				res += (mac->e << 24) | (mac->f << 16) | (mac->g << 8) | (mac->h);
				return res;
			}
	}

	*failure = true;
	return 0;
}

/*
 * int
 * bitncmp(l, r, n)
 *	比较位掩码 l 与 r，共 n 位。
 * return:
 *	<0、>0 或 0，依 libc 传统。
 * note:
 *	假定网络字节序。即 192.5.5.240/28 的
 *	第四八位组为 0x11110000。
 * author:
 *	Vixie (ISC), 1996-06
 */
int
bitncmp(const unsigned char *l, const unsigned char *r, int n)
{
	unsigned int lb,
				rb;
	int			x,
				b;

	b = n / 8;
	x = memcmp(l, r, b);
	if (x || (n % 8) == 0)
		return x;

	lb = l[b];
	rb = r[b];
	for (b = n % 8; b > 0; b--)
	{
		if (IS_HIGHBIT_SET(lb) != IS_HIGHBIT_SET(rb))
		{
			if (IS_HIGHBIT_SET(lb))
				return 1;
			return -1;
		}
		lb <<= 1;
		rb <<= 1;
	}
	return 0;
}

/*
 * bitncommon：比较位掩码 l 与 r，最多 n 位。
 *
 * 返回前缀匹配的位数（0 到 n）。
 */
int
bitncommon(const unsigned char *l, const unsigned char *r, int n)
{
	int			byte,
				nbits;

	/* 末字节中待检查的位数 */
	nbits = n % 8;

	/* 检查整字节 */
	for (byte = 0; byte < n / 8; byte++)
	{
		if (l[byte] != r[byte])
		{
			/* 末字节中至少有一位不公共 */
			nbits = 7;
			break;
		}
	}

	/* 检查末尾残字节中的位 */
	if (nbits != 0)
	{
		/* 计算首个不匹配字节之差 */
		unsigned int diff = l[byte] ^ r[byte];

		/* 从高位到低位比较这些位 */
		while ((diff >> (8 - nbits)) != 0)
			nbits--;
	}

	return (8 * byte) + nbits;
}


/*
 * 验证 CIDR 地址合法（masklen 之后无位置位）
 */
static bool
addressOK(unsigned char *a, int bits, int family)
{
	int			byte;
	int			nbits;
	int			maxbits;
	int			maxbytes;
	unsigned char mask;

	if (family == PGSQL_AF_INET)
	{
		maxbits = 32;
		maxbytes = 4;
	}
	else
	{
		maxbits = 128;
		maxbytes = 16;
	}
	Assert(bits <= maxbits);

	if (bits == maxbits)
		return true;

	byte = bits / 8;

	nbits = bits % 8;
	mask = 0xff;
	if (bits != 0)
		mask >>= nbits;

	while (byte < maxbytes)
	{
		if ((a[byte] & mask) != 0)
			return false;
		mask = 0xff;
		byte++;
	}

	return true;
}


/*
 * 规划器用这些函数为子句 a << b 与 a <<= b
 * 生成索引扫描上下限
 */

/* 返回给定网络上 IP 的最小值 */
Datum
network_scan_first(Datum in)
{
	return DirectFunctionCall1(network_network, in);
}

/*
 * 返回给定网络上的"末"IP。即广播地址，
 * 但 masklen 须设为最大位数，因为
 * 192.168.0.255/24 被认为小于 192.168.0.255/32
 *
 * inet_set_masklen() 在参数为 -1 时被改成把
 * 掩码拉满：IPv6 为 128，IPv4 为 32。
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(-1));
}


/*
 * 客户端连接来源 IP（Unix 套接字则为 NULL）
 */
Datum
inet_client_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->raddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(network_in(remote_host, false, NULL));
}


/*
 * 客户端连接来源端口（Unix 套接字则为 NULL）
 */
Datum
inet_client_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(remote_port)));
}


/*
 * 服务器接受连接所用 IP（Unix 套接字则为 NULL）
 */
Datum
inet_server_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	local_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 local_host, sizeof(local_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->laddr.addr.ss_family, local_host);

	PG_RETURN_INET_P(network_in(local_host, false, NULL));
}


/*
 * 服务器接受连接所用端口（Unix 套接字则为 NULL）
 */
Datum
inet_server_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	local_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 NULL, 0,
							 local_port, sizeof(local_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(local_port)));
}


Datum
inetnot(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = ~pip[nb];
	}
	ip_bits(dst) = ip_bits(ip);

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetand(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot AND inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] & pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetor(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot OR inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] | pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


static inet *
internal_inetpl(inet *ip, int64 addend)
{
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);
		int			carry = 0;

		while (--nb >= 0)
		{
			carry = pip[nb] + (int) (addend & 0xFF) + carry;
			pdst[nb] = (unsigned char) (carry & 0xFF);
			carry >>= 8;

			/*
			 * 对 addend 右移必须小心，因为负数的
			 * 右移不可移植，而简单除以 256 也不行：
			 * 标准舍入方向不对，况且还可能有机器
			 * 舍入方式也不对。因此显式清掉低
			 * 字节，以消除对除法结果正确性的疑虑，
			 * 然后做除法而非
			 * 移位。
			 */
			addend &= ~((int64) 0xFF);
			addend /= 0x100;
		}

		/*
		 * 此时若原 addend >= 0，则 addend 与 carry 皆应为 0；
		 * 若原 addend < 0，则应为 addend=-1 且 carry=1。
		 * 其他情形均表示溢出。
		 */
		if (!((addend == 0 && carry == 0) ||
			  (addend == -1 && carry == 1)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("result is out of range")));
	}

	ip_bits(dst) = ip_bits(ip);
	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	return dst;
}


Datum
inetpl(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, addend));
}


Datum
inetmi_int8(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, -addend));
}


Datum
inetmi(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	int64		res = 0;

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot subtract inet values of different sizes")));
	else
	{
		/*
		 * 用传统的取反、加一、再加规则求差，
		 * 加一通过把进位初值设为 1 完成。
		 * 若你不认为整数运算是二进制补码，
		 * 那也没办法。
		 */
		int			nb = ip_addrsize(ip);
		int			byte = 0;
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		int			carry = 1;

		while (--nb >= 0)
		{
			int			lobyte;

			carry = pip[nb] + (~pip2[nb] & 0xFF) + carry;
			lobyte = carry & 0xFF;
			if (byte < sizeof(int64))
			{
				res |= ((int64) lobyte) << (byte * 8);
			}
			else
			{
				/*
				 * 输入宽于 int64：检查溢出。装不下的左侧
				 * 各字节应为 0 或 0xFF，取决于现已完整的
				 * 结果的符号。
				 */
				if ((res < 0) ? (lobyte != 0xFF) : (lobyte != 0))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("result is out of range")));
			}
			carry >>= 8;
			byte++;
		}

		/*
		 * 若输入窄于 int64，不可能溢出，但须做
		 * 正确的符号扩展。
		 */
		if (carry == 0 && byte < sizeof(int64))
			res |= ((uint64) (int64) -1) << (byte * 8);
	}

	PG_RETURN_INT64(res);
}


/*
 * clean_ipv6_addr --- 去掉 IPv6 地址串中的 %zone 部分
 *
 * XXX 这总有一天该删掉！
 *
 * 这是权宜之计，因存储的 inet 尚不支持 zone。
 * getnameinfo() 的结果可能含 zone 说明，
 * 凡要把其输出喂给 network_in 的地方都调用本函数
 * 去掉它。总比整段失败强。
 *
 * 另一做法是让 network_in 自己忽略 %-部分，
 * 但那会静默丢掉用户输入中的 zone 说明，
 * 似乎不太好。
 */
void
clean_ipv6_addr(int addr_family, char *addr)
{
	if (addr_family == AF_INET6)
	{
		char	   *pct = strchr(addr, '%');

		if (pct)
			*pct = '\0';
	}
}
