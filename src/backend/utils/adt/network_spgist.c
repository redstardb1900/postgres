/*-------------------------------------------------------------------------
 *
 * network_spgist.c
 *	  网络类型的 SP-GiST 支持。
 *
 * inet 索引项先按地址族(IPv4 或 IPv6)拆分。
 * 若某内节点元组下条目同属一族,则识别其公共
 * 前缀,并按地址的下一位拆分,以及掩码长是否
 * 超过公共前缀长度。
 *
 * 同时有 IPv4 与 IPv6 子项的内节点元组前缀为
 * 空,恰有两个节点:先 IPv4 后 IPv6。
 *
 * 否则前缀是表示公共前缀的 CIDR 值,恰有四个
 * 节点。节点 0/1 对应掩码长与前缀相同的地址,
 * 节点 2/3 对应更大掩码长。(不允许元组含掩码
 * 长小于其前缀的条目。) 节点 0 与 1 由公共前缀
 * 之后的下一位地址区分,2 与 3 同理。若该地址
 * 族已无更多位,则全部进节点 0(很可能导致
 * 创建 allTheSame 元组)。
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/network_spgist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>

#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "varatt.h"


static int	inet_spg_node_number(const inet *val, int commonbits);
static int	inet_spg_consistent_bitmap(const inet *prefix, int nkeys,
									   ScanKey scankeys, bool leaf);

/*
 * SP-GiST 配置函数
 */
Datum
inet_spg_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = CIDROID;
	cfg->labelType = VOIDOID;
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * SP-GiST 选择函数
 */
Datum
inet_spg_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	inet	   *val = DatumGetInetPP(in->datum),
			   *prefix;
	int			commonbits;

	/*
	 * 若看的是按地址族拆分的元组,选择合适
	 * 子节点。
	 */
	if (!in->hasPrefix)
	{
		/* 此类元组不可能 allTheSame */
		Assert(!in->allTheSame);
		Assert(in->nNodes == 2);

		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = (ip_family(val) == PGSQL_AF_INET) ? 0 : 1;
		out->result.matchNode.restDatum = InetPGetDatum(val);

		PG_RETURN_VOID();
	}

	/* 否则须按前缀拆分 */
	Assert(in->nNodes == 4 || in->allTheSame);

	prefix = DatumGetInetPP(in->prefixDatum);
	commonbits = ip_bits(prefix);

	/*
	 * 无法把不同族的地址放在同一内节点下,故新值
	 * 族不同时必须拆分。
	 */
	if (ip_family(val) != ip_family(prefix))
	{
		/* 建 2 节点元组 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = false;
		out->result.splitTuple.prefixNNodes = 2;
		out->result.splitTuple.prefixNodeLabels = NULL;

		/* 确定既有数据进入哪个节点 */
		out->result.splitTuple.childNodeN =
			(ip_family(prefix) == PGSQL_AF_INET) ? 0 : 1;

		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	/*
	 * 若新值与既有前缀不匹配,必须拆分。
	 */
	if (ip_bits(val) < commonbits ||
		bitncmp(ip_addr(prefix), ip_addr(val), commonbits) != 0)
	{
		/* 确定拆分元组的新前缀长度 */
		commonbits = bitncommon(ip_addr(prefix), ip_addr(val),
								Min(ip_bits(val), commonbits));

		/* 建 4 节点元组 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
			InetPGetDatum(cidr_set_masklen_internal(val, commonbits));
		out->result.splitTuple.prefixNNodes = 4;
		out->result.splitTuple.prefixNodeLabels = NULL;

		/* 确定既有数据进入哪个节点 */
		out->result.splitTuple.childNodeN =
			inet_spg_node_number(prefix, commonbits);

		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	/*
	 * 一切正常,选择下潜节点。(若本元组标为
	 * allTheSame,核心会忽略我们对 nodeN 的选择;但
	 * 此处不必显式处理该情形。)
	 */
	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = inet_spg_node_number(val, commonbits);
	out->result.matchNode.restDatum = InetPGetDatum(val);

	PG_RETURN_VOID();
}

/*
 * GiST 拆分方法
 */
Datum
inet_spg_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	inet	   *prefix,
			   *tmp;
	int			i,
				commonbits;
	bool		differentFamilies = false;

	/* 用第一项初始化前缀 */
	prefix = DatumGetInetPP(in->datums[0]);
	commonbits = ip_bits(prefix);

	/* 检查其余项以发现最小公共前缀长度 */
	for (i = 1; i < in->nTuples; i++)
	{
		tmp = DatumGetInetPP(in->datums[i]);

		if (ip_family(tmp) != ip_family(prefix))
		{
			differentFamilies = true;
			break;
		}

		if (ip_bits(tmp) < commonbits)
			commonbits = ip_bits(tmp);
		commonbits = bitncommon(ip_addr(prefix), ip_addr(tmp), commonbits);
		if (commonbits == 0)
			break;
	}

	/* 不需要标签;分配输出数组 */
	out->nodeLabels = NULL;
	out->mapTuplesToNodes = (int *) palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = (Datum *) palloc(sizeof(Datum) * in->nTuples);

	if (differentFamilies)
	{
		/* 建 2 节点元组 */
		out->hasPrefix = false;
		out->nNodes = 2;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);
			out->mapTuplesToNodes[i] =
				(ip_family(tmp) == PGSQL_AF_INET) ? 0 : 1;
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}
	else
	{
		/* 建 4 节点元组 */
		out->hasPrefix = true;
		out->prefixDatum =
			InetPGetDatum(cidr_set_masklen_internal(prefix, commonbits));
		out->nNodes = 4;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);
			out->mapTuplesToNodes[i] = inet_spg_node_number(tmp, commonbits);
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}

	PG_RETURN_VOID();
}

/*
 * 内节点元组的 SP-GiST 查询一致性检查
 */
Datum
inet_spg_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;
	int			which;

	if (!in->hasPrefix)
	{
		Assert(!in->allTheSame);
		Assert(in->nNodes == 2);

		/* 确定需访问哪些子节点 */
		which = 1 | (1 << 1);

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;
			inet	   *argument = DatumGetInetPP(in->scankeys[i].sk_argument);

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET)
						which &= 1;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET6)
						which &= (1 << 1);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* 其他运算只能匹配同族地址 */
					if (ip_family(argument) == PGSQL_AF_INET)
						which &= 1;
					else
						which &= (1 << 1);
					break;
			}
		}
	}
	else if (!in->allTheSame)
	{
		Assert(in->nNodes == 4);

		/* 确定需访问哪些子节点 */
		which = inet_spg_consistent_bitmap(DatumGetInetPP(in->prefixDatum),
										   in->nkeys, in->scankeys, false);
	}
	else
	{
		/* 须访问全部节点;假定少于 32 个 */
		which = ~0;
	}

	out->nNodes = 0;

	if (which)
	{
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

		for (i = 0; i < in->nNodes; i++)
		{
			if (which & (1 << i))
			{
				out->nodeNumbers[out->nNodes] = i;
				out->nNodes++;
			}
		}
	}

	PG_RETURN_VOID();
}

/*
 * 叶子元组的 SP-GiST 查询一致性检查
 */
Datum
inet_spg_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	inet	   *leaf = DatumGetInetPP(in->leafDatum);

	/* 测试皆精确。 */
	out->recheck = false;

	/* 叶子即其所是... */
	out->leafValue = InetPGetDatum(leaf);

	/* 用共用代码施加测试。 */
	PG_RETURN_BOOL(inet_spg_consistent_bitmap(leaf, in->nkeys, in->scankeys,
											  true));
}

/*
 * 计算节点号(在 4 节点、单族内索引元组内)。
 *
 * 值须与节点前缀同族,commonbits 为前缀的掩码
 * 长。按 commonbits 之后的下一位地址选用偶/奇
 * 节点,并按值的掩码长是否大于 commonbits
 * 选用低/高节点。
 *
 */
static int
inet_spg_node_number(const inet *val, int commonbits)
{
	int			nodeN = 0;

	if (commonbits < ip_maxbits(val) &&
		ip_addr(val)[commonbits / 8] & (1 << (7 - commonbits % 8)))
		nodeN |= 1;
	if (commonbits < ip_bits(val))
		nodeN |= 2;

	return nodeN;
}

/*
 * 计算与查询一致的节点号位图。
 *
 * 可用于 4 路内节点元组,也可用于叶子元组。
 * 后者应返回布尔结果(0 或 1)而非
 * 位图。
 *
 * 定义颇怪,但内外一致性检查大多共用,放在
 * 一个函数里更好。
 */
static int
inet_spg_consistent_bitmap(const inet *prefix, int nkeys, ScanKey scankeys,
						   bool leaf)
{
	int			bitmap;
	int			commonbits,
				i;

	/* 初始化结果为允许访问全部子项 */
	if (leaf)
		bitmap = 1;
	else
		bitmap = 1 | (1 << 1) | (1 << 2) | (1 << 3);

	commonbits = ip_bits(prefix);

	for (i = 0; i < nkeys; i++)
	{
		inet	   *argument = DatumGetInetPP(scankeys[i].sk_argument);
		StrategyNumber strategy = scankeys[i].sk_strategy;
		int			order;

		/*
		 * 检查 0: 地址族不同
		 *
		 * 匹配的地址族对任何策略都无帮助。
		 */
		if (ip_family(argument) != ip_family(prefix))
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) < ip_family(prefix))
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) > ip_family(prefix))
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* 其余所有情况均可断定不匹配 */
					bitmap = 0;
					break;
			}

			if (!bitmap)
				break;

			/* 族不同时其他检查无意义。 */
			continue;
		}

		/*
		 * 检查 1: 网络位数
		 *
		 * 网络位数(ip_bits)有助于在叶子上检查子网/超网
		 * 运算符。在非叶节点,已知每个子值有更大
		 * ip_bits,故某些情形也可避免下潜。
		 *
		 *
		 * 本检查比查地址位便宜,故放在前面;但对基本
		 * 比较策略须放后面,因仅当公共网络位相同
		 * 时 ip_bits 才影响其
		 * 结果。
		 */
		switch (strategy)
		{
			case RTSubStrategyNumber:
				if (commonbits <= ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;

			case RTSubEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;

			case RTSuperStrategyNumber:
				if (commonbits == ip_bits(argument) - 1)
					bitmap &= 1 | (1 << 1);
				else if (commonbits >= ip_bits(argument))
					bitmap = 0;
				break;

			case RTSuperEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				else if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else
					bitmap = 0;
				break;
		}

		if (!bitmap)
			break;

		/*
		 * 检查 2: 公共网络位
		 *
		 * 把可用的公共前缀位与查询作比较,但不越过查询
		 * 的掩码或所表示值中的最小掩码。若这些位
		 * 与查询不符,可排除部分
		 * 情形。
		 */
		order = bitncmp(ip_addr(prefix), ip_addr(argument),
						Min(commonbits, ip_bits(argument)));

		if (order != 0)
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* 其余所有情况均可断定不匹配 */
					bitmap = 0;
					break;
			}

			if (!bitmap)
				break;

			/*
			 * 公共位不匹配时其余检查无意义。
			 */
			continue;
		}

		/*
		 * 检查 3: 下一网络位
		 *
		 * 若参数的下一网络位可用,可用它过滤分支
		 * 2 或 3。
		 *
		 * 本检查影响搜索性能。没有它结果仍
		 * 正确。
		 */
		if (bitmap & ((1 << 2) | (1 << 3)) &&
			commonbits < ip_bits(argument))
		{
			int			nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
				(1 << (7 - commonbits % 8));

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 2);
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 3);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 2);
					else
						bitmap &= 1 | (1 << 1) | (1 << 3);
					break;
			}

			if (!bitmap)
				break;
		}

		/*
		 * 其余检查仅针对基本比较策略。本测试依赖
		 * stratnum.h 中定义的策略号次序。
		 */
		if (strategy < RTEqualStrategyNumber ||
			strategy > RTGreaterEqualStrategyNumber)
			continue;

		/*
		 * 检查 4: 网络位数
		 *
		 * 至此已知前缀与参数的公共网络位相同,可
		 * 继续检查 ip_bits。
		 *
		 */
		switch (strategy)
		{
			case RTLessStrategyNumber:
			case RTLessEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTGreaterEqualStrategyNumber:
			case RTGreaterStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;
		}

		if (!bitmap)
			break;

		/* ip_bits 不同时其余检查无意义。 */
		if (commonbits != ip_bits(argument))
			continue;

		/*
		 * 查 5: 下一主机位
		 *
		 * 若参数的下一主机位可用,可用它过滤分支
		 * 0 或 1。
		 *
		 * 本检查影响搜索性能。没有它结果仍正确。
		 * 对叶子无意义,因下一步须检查整个
		 * 地址。
		 */
		if (!leaf && bitmap & (1 | (1 << 1)) &&
			commonbits < ip_maxbits(argument))
		{
			int			nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
				(1 << (7 - commonbits % 8));

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | (1 << 2) | (1 << 3);
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= (1 << 1) | (1 << 2) | (1 << 3);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | (1 << 2) | (1 << 3);
					else
						bitmap &= (1 << 1) | (1 << 2) | (1 << 3);
					break;
			}

			if (!bitmap)
				break;
		}

		/*
		 * 检查 6: 整个地址
		 *
		 * 基本比较策略正确性的最后检查。仅适用于
		 * 叶子条目。
		 */
		if (leaf)
		{
			/* 用全部地址位重做次序比较 */
			order = bitncmp(ip_addr(prefix), ip_addr(argument),
							ip_maxbits(prefix));

			switch (strategy)
			{
				case RTLessStrategyNumber:
					if (order >= 0)
						bitmap = 0;
					break;

				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTEqualStrategyNumber:
					if (order != 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTGreaterStrategyNumber:
					if (order <= 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					if (order == 0)
						bitmap = 0;
					break;
			}

			if (!bitmap)
				break;
		}
	}

	return bitmap;
}
