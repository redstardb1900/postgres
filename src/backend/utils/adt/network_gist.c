/*-------------------------------------------------------------------------
 *
 * network_gist.c
 *	  网络类型的 GiST 支持。
 *
 * 读懂本代码的关键，是"并集"的定义，针对一组
 * INET/CIDR 值。规则如下：
 * 1. 若各值并非都属于同一 IP 地址族，"并集"
 * 就是哑值：族号为零、minbits 为零、commonbits 为零、
 * 地址全为零。否则：
 * 2. 并集取所有值共有的 IP 地址族号。
 * 3. 并集的 minbits 值是各输入中最小的网络掩码长度
 * （即 "ip_bits"）。
 * 4. 设 C 为各输入值公共的前导地址位数（C 的范围从 0
 * 到该族的 ip_maxbits）。
 * 5. 并集的 commonbits 值即 C。
 * 6. 并集地址值前 C 位与公共前缀相同，右侧为零。
 * 地址值的物理宽度为所属地址族的
 * ip_maxbits。
 *
 * 在叶子索引条目（代表单个键）中 commonbits 等于该地址
 * 族的 ip_maxbits；minbits 等于所表示值的 ip_bits；地址则
 * 等于所表示的地址。
 * 虽然在叶子键中存储的是并集格式、而非所表示的
 * INET/CIDR 值，看似浪费一字节，但对齐方面的考虑
 * 使这个字节实际是"免费"的。
 *
 * 注意：本设计对 minbits 与 commonbits 分开独立跟踪；在任
 * 何给定的并集值中，两者都可能比对方小。但由于 inet
 * 比较的定义，这对下潜遍历帮助不大：在非叶子节点
 * 上最多只能比较 minbits 位，即使明知这些位。
 * 不过，它大大改善了拆分决策的质量。初步测
 * 试表明搜索速度可达简单设计的两倍。那种简
 * 单设计让单个字段兼任公共前缀长度与最小
 * ip_bits 值。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "varatt.h"

/*
 * GiST inet_ops 算子类所用的算子策略号
 */
#define INETSTRAT_OVERLAPS		RTOverlapStrategyNumber
#define INETSTRAT_EQ			RTEqualStrategyNumber
#define INETSTRAT_NE			RTNotEqualStrategyNumber
#define INETSTRAT_LT			RTLessStrategyNumber
#define INETSTRAT_LE			RTLessEqualStrategyNumber
#define INETSTRAT_GT			RTGreaterStrategyNumber
#define INETSTRAT_GE			RTGreaterEqualStrategyNumber
#define INETSTRAT_SUB			RTSubStrategyNumber
#define INETSTRAT_SUBEQ			RTSubEqualStrategyNumber
#define INETSTRAT_SUP			RTSuperStrategyNumber
#define INETSTRAT_SUPEQ			RTSuperEqualStrategyNumber


/*
 * GiST INET/CIDR 索引键的表示。它与 INET/CIDR 并不完全
 * 相同，因为我们需要额外记录公共地址前缀的长度与最小
 * 掩码长度。不过只要遵循 varlena 头部规则，核心 GiST
 * 代码不会察觉差别。为简单起见，我们始终使用 1 字节头
 * varlena 格式。
 */
typedef struct GistInetKey
{
	uint8		va_header;		/* varlena 头部 —— 勿直接改动 */
	unsigned char family;		/* PGSQL_AF_INET、PGSQL_AF_INET6 或零 */
	unsigned char minbits;		/* 网络掩码的最小位数 */
	unsigned char commonbits;	/* 地址间公共前缀的位数 */
	unsigned char ipaddr[16];	/* 最多 128 位公共地址 */
} GistInetKey;

#define DatumGetInetKeyP(X) ((GistInetKey *) DatumGetPointer(X))
#define InetKeyPGetDatum(X) PointerGetDatum(X)

/*
 * 访问宏；内容平平无奇，但使用它们是为了与访问
 * INET/CIDR 值的方式保持记法上的一致。注意：族号为零的
 * 值只存 4 字节地址而非 16。
 */
#define gk_ip_family(gkptr)		((gkptr)->family)
#define gk_ip_minbits(gkptr)	((gkptr)->minbits)
#define gk_ip_commonbits(gkptr) ((gkptr)->commonbits)
#define gk_ip_addr(gkptr)		((gkptr)->ipaddr)
#define ip_family_maxbits(fam)	((fam) == PGSQL_AF_INET6 ? 128 : 32)

/* 这些宏要求族字段已被设置： */
#define gk_ip_addrsize(gkptr) \
	(gk_ip_family(gkptr) == PGSQL_AF_INET6 ? 16 : 4)
#define gk_ip_maxbits(gkptr) \
	ip_family_maxbits(gk_ip_family(gkptr))
#define SET_GK_VARSIZE(dst) \
	SET_VARSIZE_SHORT(dst, offsetof(GistInetKey, ipaddr) + gk_ip_addrsize(dst))


/*
 * GiST 查询一致性检查
 */
Datum
inet_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *ent = (GISTENTRY *) PG_GETARG_POINTER(0);
	inet	   *query = PG_GETARG_INET_PP(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistInetKey *key = DatumGetInetKeyP(ent->key);
	int			minbits,
				order;

	/* 此函数处理的算子均为精确的。 */
	*recheck = false;

	/*
	 * 检查0：地址族不同
	 *
	 * 若键表示多个地址族，其子节点可能匹配任何值，这
	 * 只可能发生在内部索引页上。
	 */
	if (gk_ip_family(key) == 0)
	{
		Assert(!GIST_LEAF(ent));
		PG_RETURN_BOOL(true);
	}

	/*
	 * 检查1：地址族不同
	 *
	 * 匹配的地址族对任何策略都无帮助。
	 */
	if (gk_ip_family(key) != ip_family(query))
	{
		switch (strategy)
		{
			case INETSTRAT_LT:
			case INETSTRAT_LE:
				if (gk_ip_family(key) < ip_family(query))
					PG_RETURN_BOOL(true);
				break;

			case INETSTRAT_GE:
			case INETSTRAT_GT:
				if (gk_ip_family(key) > ip_family(query))
					PG_RETURN_BOOL(true);
				break;

			case INETSTRAT_NE:
				PG_RETURN_BOOL(true);
		}
		/* 其余所有情况均可断定不匹配 */
		PG_RETURN_BOOL(false);
	}

	/*
	 * 检查2：网络位数
	 *
	 * 网络位数（ip_bits）有助于检查叶子是否满足子网与
	 * 超网算子。在非叶子节点，每个子值的 ip_bits 必然
	 * 不小于 gk_ip_minbits(key)，所以某些情况下可免于
	 * 下潜。
	 */
	switch (strategy)
	{
		case INETSTRAT_SUB:
			if (GIST_LEAF(ent) && gk_ip_minbits(key) <= ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUBEQ:
			if (GIST_LEAF(ent) && gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUPEQ:
		case INETSTRAT_EQ:
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUP:
			if (gk_ip_minbits(key) >= ip_bits(query))
				PG_RETURN_BOOL(false);
			break;
	}

	/*
	 * 检查3：公共网络位
	 *
	 * 把可用的公共前缀位与查询作比较，但不越过
	 * 查询的网络掩码，也不超过所表示各值中的最小网络
	 * 掩码。若这些位与查询不符，即得答案（是否还需
	 * 下潜取决于算子）。若匹配，且不在叶子上，则任何
	 * 情况下都要下潜。
	 *
	 * 注意：只考虑地址网络部分的算子，这是最后一
	 * 项检查。
	 */
	minbits = Min(gk_ip_commonbits(key), gk_ip_minbits(key));
	minbits = Min(minbits, ip_bits(query));

	order = bitncmp(gk_ip_addr(key), ip_addr(query), minbits);

	switch (strategy)
	{
		case INETSTRAT_SUB:
		case INETSTRAT_SUBEQ:
		case INETSTRAT_OVERLAPS:
		case INETSTRAT_SUPEQ:
		case INETSTRAT_SUP:
			PG_RETURN_BOOL(order == 0);

		case INETSTRAT_LT:
		case INETSTRAT_LE:
			if (order > 0)
				PG_RETURN_BOOL(false);
			if (order < 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_EQ:
			if (order != 0)
				PG_RETURN_BOOL(false);
			if (!GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_GE:
		case INETSTRAT_GT:
			if (order < 0)
				PG_RETURN_BOOL(false);
			if (order > 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_NE:
			if (order != 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;
	}

	/*
	 * 其余检查仅适用于叶子与基本比较策略。实现见
	 * network.c 的 network_cmp_internal()，我们需要与之匹配。
	 * 注意：在叶子键中 commonbits 应等于地址长度，因此
	 * 上面已比较过完整的网络部分。
	 */
	Assert(GIST_LEAF(ent));

	/*
	 * 检查4：网络位数
	 *
	 * 下一步比较掩码宽度。
	 */
	switch (strategy)
	{
		case INETSTRAT_LT:
		case INETSTRAT_LE:
			if (gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(true);
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_EQ:
			if (gk_ip_minbits(key) != ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_GE:
		case INETSTRAT_GT:
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(true);
			if (gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_NE:
			if (gk_ip_minbits(key) != ip_bits(query))
				PG_RETURN_BOOL(true);
			break;
	}

	/*
	 * 检查5：完整地址
	 *
	 * 网络掩码位数相同，因此检查全部地址位。
	 */
	order = bitncmp(gk_ip_addr(key), ip_addr(query), gk_ip_maxbits(key));

	switch (strategy)
	{
		case INETSTRAT_LT:
			PG_RETURN_BOOL(order < 0);

		case INETSTRAT_LE:
			PG_RETURN_BOOL(order <= 0);

		case INETSTRAT_EQ:
			PG_RETURN_BOOL(order == 0);

		case INETSTRAT_GE:
			PG_RETURN_BOOL(order >= 0);

		case INETSTRAT_GT:
			PG_RETURN_BOOL(order > 0);

		case INETSTRAT_NE:
			PG_RETURN_BOOL(order != 0);
	}

	elog(ERROR, "unknown strategy for inet GiST");
	PG_RETURN_BOOL(false);		/* 让编译器安静 */
}

/*
 * 计算若干 GistInetKey 并集的各参数。
 *
 * 检查 GISTENTRY 数组第 m..n 个元素（含端点）的键，
 * 并计算以下输出参数：
 * *minfamily_p = 最小 IP 地址族号
 * *maxfamily_p = 最大 IP 地址族号
 * *minbits_p = 最小掩码宽度
 * *commonbits_p = 各地址公共前导位数
 *
 * 若地址族多于一个，minbits 与 commonbits 将被
 * 强制为零。
 */
static void
calc_inet_union_params(GISTENTRY *ent,
					   int m, int n,
					   int *minfamily_p,
					   int *maxfamily_p,
					   int *minbits_p,
					   int *commonbits_p)
{
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp;
	int			i;

	/* 至少要有一个键。 */
	Assert(m <= n);

	/* 用第一个键初始化各变量。 */
	tmp = DatumGetInetKeyP(ent[m].key);
	minfamily = maxfamily = gk_ip_family(tmp);
	minbits = gk_ip_minbits(tmp);
	commonbits = gk_ip_commonbits(tmp);
	addr = gk_ip_addr(tmp);

	/* 扫描其余键。 */
	for (i = m + 1; i <= n; i++)
	{
		tmp = DatumGetInetKeyP(ent[i].key);

		/* 确定族号的取值范围 */
		if (minfamily > gk_ip_family(tmp))
			minfamily = gk_ip_family(tmp);
		if (maxfamily < gk_ip_family(tmp))
			maxfamily = gk_ip_family(tmp);

		/* 找出最小 minbits */
		if (minbits > gk_ip_minbits(tmp))
			minbits = gk_ip_minbits(tmp);

		/* 找出最小的公共位数 */
		if (commonbits > gk_ip_commonbits(tmp))
			commonbits = gk_ip_commonbits(tmp);
		if (commonbits > 0)
			commonbits = bitncommon(addr, gk_ip_addr(tmp), commonbits);
	}

	/* 若族多于一个，强制 minbits/commonbits 为零。 */
	if (minfamily != maxfamily)
		minbits = commonbits = 0;

	*minfamily_p = minfamily;
	*maxfamily_p = maxfamily;
	*minbits_p = minbits;
	*commonbits_p = commonbits;
}

/*
 * 同上，但要检查的 GISTENTRY 元素是 offsets[] 数组里
 * 列出索引的那些元素。
 */
static void
calc_inet_union_params_indexed(GISTENTRY *ent,
							   OffsetNumber *offsets, int noffsets,
							   int *minfamily_p,
							   int *maxfamily_p,
							   int *minbits_p,
							   int *commonbits_p)
{
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp;
	int			i;

	/* 至少要有一个键。 */
	Assert(noffsets > 0);

	/* 用第一个键初始化各变量。 */
	tmp = DatumGetInetKeyP(ent[offsets[0]].key);
	minfamily = maxfamily = gk_ip_family(tmp);
	minbits = gk_ip_minbits(tmp);
	commonbits = gk_ip_commonbits(tmp);
	addr = gk_ip_addr(tmp);

	/* 扫描其余键。 */
	for (i = 1; i < noffsets; i++)
	{
		tmp = DatumGetInetKeyP(ent[offsets[i]].key);

		/* 确定族号的取值范围 */
		if (minfamily > gk_ip_family(tmp))
			minfamily = gk_ip_family(tmp);
		if (maxfamily < gk_ip_family(tmp))
			maxfamily = gk_ip_family(tmp);

		/* 找出最小 minbits */
		if (minbits > gk_ip_minbits(tmp))
			minbits = gk_ip_minbits(tmp);

		/* 找出最小的公共位数 */
		if (commonbits > gk_ip_commonbits(tmp))
			commonbits = gk_ip_commonbits(tmp);
		if (commonbits > 0)
			commonbits = bitncommon(addr, gk_ip_addr(tmp), commonbits);
	}

	/* 若族多于一个，强制 minbits/commonbits 为零。 */
	if (minfamily != maxfamily)
		minbits = commonbits = 0;

	*minfamily_p = minfamily;
	*maxfamily_p = maxfamily;
	*minbits_p = minbits;
	*commonbits_p = commonbits;
}

/*
 * 构造一个表示并集值的 GistInetKey。
 *
 * 输入为要使用的 family/minbits/commonbits 值，外加指向
 * 某个并集输入的地址字段。（由于我们只拷贝公共位，
 * 拷哪一个都无妨。）
 */
static GistInetKey *
build_inet_union_key(int family, int minbits, int commonbits,
					 unsigned char *addr)
{
	GistInetKey *result;

	/* 确保无用位已清零。 */
	result = (GistInetKey *) palloc0(sizeof(GistInetKey));

	gk_ip_family(result) = family;
	gk_ip_minbits(result) = minbits;
	gk_ip_commonbits(result) = commonbits;

	/* 克隆地址中相应的字节。 */
	if (commonbits > 0)
		memcpy(gk_ip_addr(result), addr, (commonbits + 7) / 8);

	/* 清掉末尾不完整字节里多余的位。 */
	if (commonbits % 8 != 0)
		gk_ip_addr(result)[commonbits / 8] &= ~(0xFF >> (commonbits % 8));

	/* varlena 头部设置正确。 */
	SET_GK_VARSIZE(result);

	return result;
}


/*
 * GiST 并集函数
 *
 * 并集的定义见文件头部注释。
 */
Datum
inet_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GISTENTRY  *ent = entryvec->vector;
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp,
			   *result;

	/* 确定并集的各参数。 */
	calc_inet_union_params(ent, 0, entryvec->n - 1,
						   &minfamily, &maxfamily,
						   &minbits, &commonbits);

	/* 若多于一个族，输出族号零。 */
	if (minfamily != maxfamily)
		minfamily = 0;

	/* 用第一个键初始化地址。 */
	tmp = DatumGetInetKeyP(ent[0].key);
	addr = gk_ip_addr(tmp);

	/* 构造并集值。 */
	result = build_inet_union_key(minfamily, minbits, commonbits, addr);

	PG_RETURN_POINTER(result);
}

/*
 * GiST 压缩函数
 *
 * 把 inet 值转换为 GistInetKey。
 */
Datum
inet_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		retval = palloc(sizeof(GISTENTRY));
		if (DatumGetPointer(entry->key) != NULL)
		{
			inet	   *in = DatumGetInetPP(entry->key);
			GistInetKey *r;

			r = (GistInetKey *) palloc0(sizeof(GistInetKey));

			gk_ip_family(r) = ip_family(in);
			gk_ip_minbits(r) = ip_bits(in);
			gk_ip_commonbits(r) = gk_ip_maxbits(r);
			memcpy(gk_ip_addr(r), ip_addr(in), gk_ip_addrsize(r));
			SET_GK_VARSIZE(r);

			gistentryinit(*retval, PointerGetDatum(r),
						  entry->rel, entry->page,
						  entry->offset, false);
		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, false);
		}
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * 无需解压缩函数，因为其余 GiST inet 支持函数
 * 都以 GistInetKey 表示形式工作。
 */

/*
 * GiST 获取函数
 *
 * 从 GistInetKey 重建原始 inet datum。
 */
Datum
inet_gist_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GistInetKey *key = DatumGetInetKeyP(entry->key);
	GISTENTRY  *retval;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = gk_ip_family(key);
	ip_bits(dst) = gk_ip_minbits(key);
	memcpy(ip_addr(dst), gk_ip_addr(key), ip_addrsize(dst));
	SET_INET_VARSIZE(dst);

	retval = palloc(sizeof(GISTENTRY));
	gistentryinit(*retval, InetPGetDatum(dst), entry->rel, entry->page,
				  entry->offset, false);

	PG_RETURN_POINTER(retval);
}

/*
 * GiST 页拆分惩罚函数
 *
 * 地址族不匹配时记一大笔惩罚；若新值会拉低并集的
 * minbits（最小掩码宽度），则记稍小的惩罚；
 * 否则惩罚为新增公共地址位数的
 * 倒数。
 */
Datum
inet_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origent = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newent = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	GistInetKey *orig = DatumGetInetKeyP(origent->key),
			   *new = DatumGetInetKeyP(newent->key);
	int			commonbits;

	if (gk_ip_family(orig) == gk_ip_family(new))
	{
		if (gk_ip_minbits(orig) <= gk_ip_minbits(new))
		{
			commonbits = bitncommon(gk_ip_addr(orig), gk_ip_addr(new),
									Min(gk_ip_commonbits(orig),
										gk_ip_commonbits(new)));
			if (commonbits > 0)
				*penalty = 1.0f / commonbits;
			else
				*penalty = 2;
		}
		else
			*penalty = 3;
	}
	else
		*penalty = 4;

	PG_RETURN_POINTER(penalty);
}

/*
 * GiST 拆分方法
 *
 * 拆分有两种方式。第一种按地址族拆分，适用于输入
 * 中出现多个地址族的情形。
 *
 * 第二种也更常用的方式是按地址拆分。做法是：先确
 * 定所有键共享的前导位数，然后在下一位上拆分。（目
 * 前我们这样做时并不考虑网络掩码宽度；要考虑吗？）
 * 若这样得不到非平凡拆分，就拆成
 * 对半
 */
Datum
inet_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *splitvec = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	GISTENTRY  *ent = entryvec->vector;
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp,
			   *left_union,
			   *right_union;
	int			maxoff,
				nbytes;
	OffsetNumber i,
			   *left,
			   *right;

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);

	left = (OffsetNumber *) palloc(nbytes);
	right = (OffsetNumber *) palloc(nbytes);

	splitvec->spl_left = left;
	splitvec->spl_right = right;

	splitvec->spl_nleft = 0;
	splitvec->spl_nright = 0;

	/* 确定全部输入的并集参数。 */
	calc_inet_union_params(ent, FirstOffsetNumber, maxoff,
						   &minfamily, &maxfamily,
						   &minbits, &commonbits);

	if (minfamily != maxfamily)
	{
		/* 多族，按族拆分。 */
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			/*
			 * 若族多于 2，则除最大族外的族全部进入左
			 * 侧并集。这只会在输入同时含 IPv4、IPv6 以及
			 * 已经是多族并集的值时才会发生。
			 */
			tmp = DatumGetInetKeyP(ent[i].key);
			if (gk_ip_family(tmp) != maxfamily)
				left[splitvec->spl_nleft++] = i;
			else
				right[splitvec->spl_nright++] = i;
		}
	}
	else
	{
		/*
		 * 在公共位之后的下一位上拆分；若拆分平凡，
		 * 就改试右边一位。如此重复直至成功；若位用尽，
		 * 就任意做五五平分。
		 */
		int			maxbits = ip_family_maxbits(minfamily);

		while (commonbits < maxbits)
		{
			/* 用第 commonbits 位来拆分。 */
			int			bitbyte = commonbits / 8;
			int			bitmask = 0x80 >> (commonbits % 8);

			splitvec->spl_nleft = splitvec->spl_nright = 0;

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				tmp = DatumGetInetKeyP(ent[i].key);
				addr = gk_ip_addr(tmp);
				if ((addr[bitbyte] & bitmask) == 0)
					left[splitvec->spl_nleft++] = i;
				else
					right[splitvec->spl_nright++] = i;
			}

			if (splitvec->spl_nleft > 0 && splitvec->spl_nright > 0)
				break;			/* 成功 */
			commonbits++;
		}

		if (commonbits >= maxbits)
		{
			/* 失败……五五分。 */
			splitvec->spl_nleft = splitvec->spl_nright = 0;

			for (i = FirstOffsetNumber; i <= maxoff / 2; i = OffsetNumberNext(i))
			{
				left[splitvec->spl_nleft++] = i;
			}
			for (; i <= maxoff; i = OffsetNumberNext(i))
			{
				right[splitvec->spl_nright++] = i;
			}
		}
	}

	/*
	 * 从头为每侧重新计算并集值。多数情况下本可以用
	 * 已知信息近似并集值，但这样做能保证每侧的 minbits
	 * 与 commonbits 都尽可能设为
	 * 最高。
	 */
	calc_inet_union_params_indexed(ent, left, splitvec->spl_nleft,
								   &minfamily, &maxfamily,
								   &minbits, &commonbits);
	if (minfamily != maxfamily)
		minfamily = 0;
	tmp = DatumGetInetKeyP(ent[left[0]].key);
	addr = gk_ip_addr(tmp);
	left_union = build_inet_union_key(minfamily, minbits, commonbits, addr);
	splitvec->spl_ldatum = PointerGetDatum(left_union);

	calc_inet_union_params_indexed(ent, right, splitvec->spl_nright,
								   &minfamily, &maxfamily,
								   &minbits, &commonbits);
	if (minfamily != maxfamily)
		minfamily = 0;
	tmp = DatumGetInetKeyP(ent[right[0]].key);
	addr = gk_ip_addr(tmp);
	right_union = build_inet_union_key(minfamily, minbits, commonbits, addr);
	splitvec->spl_rdatum = PointerGetDatum(right_union);

	PG_RETURN_POINTER(splitvec);
}

/*
 * GiST 相同函数
 */
Datum
inet_gist_same(PG_FUNCTION_ARGS)
{
	GistInetKey *left = DatumGetInetKeyP(PG_GETARG_DATUM(0));
	GistInetKey *right = DatumGetInetKeyP(PG_GETARG_DATUM(1));
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = (gk_ip_family(left) == gk_ip_family(right) &&
			   gk_ip_minbits(left) == gk_ip_minbits(right) &&
			   gk_ip_commonbits(left) == gk_ip_commonbits(right) &&
			   memcmp(gk_ip_addr(left), gk_ip_addr(right),
					  gk_ip_addrsize(left)) == 0);

	PG_RETURN_POINTER(result);
}
