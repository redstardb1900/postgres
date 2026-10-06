/*-------------------------------------------------------------------------
 *
 * spgtextproc.c
 *	  面向文本的基数树（压缩 trie）实现。
 *
 * 在 text_ops 的 SPGiST 索引中，内部元组可以带前缀，即挂在该元组下
 * 所有已索引字符串（string）的公共前缀。节点标签表示前缀之后的下一
 * 字节。若始终使用尽可能长的前缀，我们会得到不止一个节点标
 * 签，除非前缀长度受
 * SPGIST_MAX_PREFIX_LENGTH 限制。
 *
 * 要重建任一索引项对应的已索引字符串，从根向下到叶子项，依次
 * 拼接内部元组前缀与节点标签，再追加叶子项中的
 * datum。
 * （下降树时，"level" 是迄今已重建的
 * 字节数。）
 *
 * 不过节点标签有两种特例：-1 表示当前前缀之后没有更多字节，
 * -2 表示不得不分裂已有的 allTheSame 元组（此时必须创建
 * 不对应任何字符串字节的节点标签）。两种情况下，节点标签都
 * 不对重建字符串有任何
 * 贡献。
 *
 * 以前两种特例都使用节点标签 0，但这有问题，因为无法判断
 * 在当前层结束的字符串能否下推到这样的子节点。为保持向
 * 后兼容，读取时仍支持这类节点标签；但不会再有新项被下推
 * 到标签为 0 的子节点。同样，也不会再有新项被推入标签为
 * -2 的
 * 子节点。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgtextproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"
#include "varatt.h"


/*
 * 最坏情况下，文本基数树中的内部元组可以有多达
 * 258 个节点（每个可能的字节值一个，外加两种特
 * 例）。在 MAXALIGN=8 的机器上，每个节点最多占 16 字节。
 * 内部元组必须能放进大小为 BLCKSZ 的索引页。我们不去假定已确知
 * 页头、元组头等开销的确切大小，而是为此预留 100 字节（写本
 * 文时实际开销应不超过 56 字节，因此该数字留有余量）。
 * 于是可以安全地创建最长为 BLCKSZ - 258 * 16 - 100 字节的前缀。
 * 不幸的是，因为 258 * 16 超过 4K，当 BLCKSZ 小于 8K 时不存在
 * 安全的前缀长度；若树中某处下一字节取值过于多样，仍可能
 * 出现 "SPGiST inner
 * tuple size exceeds maximum"。由于现场使用非标准块大小
 * 似乎很少见，我们暂时接受这一事实，并在 BLCKSZ 配置得
 * 小于默认值时把最大前缀选为
 * 32
 * 字节。
 */
#define SPGIST_MAX_PREFIX_LENGTH	Max((int) (BLCKSZ - 258 * 16 - 100), 32)

/*
 * 文本上排序规则感知操作符的策略号等于 btree 策略号
 * 加上 10。
 *
 * 当前排序规则感知策略及其对应的 btree 策略：
 * 11 BTLessStrategyNumber
 * 12 BTLessEqualStrategyNumber
 * 14 BTGreaterEqualStrategyNumber
 * 15 BTGreaterStrategyNumber
 */
#define SPG_STRATEGY_ADDITION	(10)
#define SPG_IS_COLLATION_AWARE_STRATEGY(s) ((s) > SPG_STRATEGY_ADDITION \
										 && (s) != RTPrefixStrategyNumber)

/* 用于在 picksplit 中对值排序的结构 */
typedef struct spgNodePtr
{
	Datum		d;
	int			i;
	int16		c;
} spgNodePtr;


Datum
spg_text_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = TEXTOID;
	cfg->labelType = INT2OID;
	cfg->canReturnData = true;
	cfg->longValuesOK = true;	/* 加后缀会缩短过长的值 */
	PG_RETURN_VOID();
}

/*
 * 由给定的不一定以空字符结尾的字符串构造 text datum，
 * 尽可能使用短 varlena 头格式
 */
static Datum
formTextDatum(const char *data, int datalen)
{
	char	   *p;

	p = (char *) palloc(datalen + VARHDRSZ);

	if (datalen + VARHDRSZ_SHORT <= VARATT_SHORT_MAX)
	{
		SET_VARSIZE_SHORT(p, datalen + VARHDRSZ_SHORT);
		if (datalen)
			memcpy(p + VARHDRSZ_SHORT, data, datalen);
	}
	else
	{
		SET_VARSIZE(p, datalen + VARHDRSZ);
		memcpy(p + VARHDRSZ, data, datalen);
	}

	return PointerGetDatum(p);
}

/*
 * 求 a 与 b 的公共前缀长度
 */
static int
commonPrefix(const char *a, const char *b, int lena, int lenb)
{
	int			i = 0;

	while (i < lena && i < lenb && *a == *b)
	{
		a++;
		b++;
		i++;
	}

	return i;
}

/*
 * 在 int16 datum 数组中二分查找与 c 匹配的项
 *
 * 成功时 *i 为匹配位置；失败时为应插入的位置
 */
static bool
searchChar(Datum *nodeLabels, int nNodes, int16 c, int *i)
{
	int			StopLow = 0,
				StopHigh = nNodes;

	while (StopLow < StopHigh)
	{
		int			StopMiddle = (StopLow + StopHigh) >> 1;
		int16		middle = DatumGetInt16(nodeLabels[StopMiddle]);

		if (c < middle)
			StopHigh = StopMiddle;
		else if (c > middle)
			StopLow = StopMiddle + 1;
		else
		{
			*i = StopMiddle;
			return true;
		}
	}

	*i = StopHigh;
	return false;
}

Datum
spg_text_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	text	   *inText = DatumGetTextPP(in->datum);
	char	   *inStr = VARDATA_ANY(inText);
	int			inSize = VARSIZE_ANY_EXHDR(inText);
	char	   *prefixStr = NULL;
	int			prefixSize = 0;
	int			commonLen = 0;
	int16		nodeChar = 0;
	int			i = 0;

	/* 检查前缀是否匹配，并将 nodeChar 设为前缀后的第一个字节 */
	if (in->hasPrefix)
	{
		text	   *prefixText = DatumGetTextPP(in->prefixDatum);

		prefixStr = VARDATA_ANY(prefixText);
		prefixSize = VARSIZE_ANY_EXHDR(prefixText);

		commonLen = commonPrefix(inStr + in->level,
								 prefixStr,
								 inSize - in->level,
								 prefixSize);

		if (commonLen == prefixSize)
		{
			if (inSize - in->level > commonLen)
				nodeChar = *(unsigned char *) (inStr + in->level + commonLen);
			else
				nodeChar = -1;
		}
		else
		{
			/* 传入值与前缀不匹配，必须分裂元组 */
			out->resultType = spgSplitTuple;

			if (commonLen == 0)
			{
				out->result.splitTuple.prefixHasPrefix = false;
			}
			else
			{
				out->result.splitTuple.prefixHasPrefix = true;
				out->result.splitTuple.prefixPrefixDatum =
					formTextDatum(prefixStr, commonLen);
			}
			out->result.splitTuple.prefixNNodes = 1;
			out->result.splitTuple.prefixNodeLabels =
				(Datum *) palloc(sizeof(Datum));
			out->result.splitTuple.prefixNodeLabels[0] =
				Int16GetDatum(*(unsigned char *) (prefixStr + commonLen));

			out->result.splitTuple.childNodeN = 0;

			if (prefixSize - commonLen == 1)
			{
				out->result.splitTuple.postfixHasPrefix = false;
			}
			else
			{
				out->result.splitTuple.postfixHasPrefix = true;
				out->result.splitTuple.postfixPrefixDatum =
					formTextDatum(prefixStr + commonLen + 1,
								  prefixSize - commonLen - 1);
			}

			PG_RETURN_VOID();
		}
	}
	else if (inSize > in->level)
	{
		nodeChar = *(unsigned char *) (inStr + in->level);
	}
	else
	{
		nodeChar = -1;
	}

	/* 在节点标签数组中查找 nodeChar */
	if (searchChar(in->nodeLabels, in->nNodes, nodeChar, &i))
	{
		/*
		 * 下降到已有节点。（若 in->allTheSame，核心代码会
		 * 忽略此处的 nodeN 指定，但这没关系。我们仍须提供正确的
		 * levelAdd 与 restDatum，且不论核心选中哪个节点，这些值都
		 * 相同。）
		 */
		int			levelAdd;

		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = i;
		levelAdd = commonLen;
		if (nodeChar >= 0)
			levelAdd++;
		out->result.matchNode.levelAdd = levelAdd;
		if (inSize - in->level - levelAdd > 0)
			out->result.matchNode.restDatum =
				formTextDatum(inStr + in->level + levelAdd,
							  inSize - in->level - levelAdd);
		else
			out->result.matchNode.restDatum =
				formTextDatum(NULL, 0);
	}
	else if (in->allTheSame)
	{
		/*
		 * 不能使用 AddNode 动作，因此分裂该元组。上元组保持
		 * 与原先相同的前缀，并对下元组使用哑节点标签 -2。
		 * 下元组没有前缀，节点标签与原元组
		 * 相同。
		 *
		 * 注意：或许会想缩短上元组的前缀（若有），再把其最后一
		 * 字节用作下元组的标签。但这并不划算，因为已知传入值匹配
		 * 整个前缀：最终只会再次分裂
		 * 下元组。
		 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = in->hasPrefix;
		out->result.splitTuple.prefixPrefixDatum = in->prefixDatum;
		out->result.splitTuple.prefixNNodes = 1;
		out->result.splitTuple.prefixNodeLabels = (Datum *) palloc(sizeof(Datum));
		out->result.splitTuple.prefixNodeLabels[0] = Int16GetDatum(-2);
		out->result.splitTuple.childNodeN = 0;
		out->result.splitTuple.postfixHasPrefix = false;
	}
	else
	{
		/* 为先前未见过的 nodeChar 值添加一个节点 */
		out->resultType = spgAddNode;
		out->result.addNode.nodeLabel = Int16GetDatum(nodeChar);
		out->result.addNode.nodeN = i;
	}

	PG_RETURN_VOID();
}

/* 按 "c" 对 spgNodePtr 结构排序的 qsort 比较函数 */
static int
cmpNodePtr(const void *a, const void *b)
{
	const spgNodePtr *aa = (const spgNodePtr *) a;
	const spgNodePtr *bb = (const spgNodePtr *) b;

	return aa->c - bb->c;
}

Datum
spg_text_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	text	   *text0 = DatumGetTextPP(in->datums[0]);
	int			i,
				commonLen;
	spgNodePtr *nodes;

	/* 确定最长公共前缀（若有） */
	commonLen = VARSIZE_ANY_EXHDR(text0);
	for (i = 1; i < in->nTuples && commonLen > 0; i++)
	{
		text	   *texti = DatumGetTextPP(in->datums[i]);
		int			tmp = commonPrefix(VARDATA_ANY(text0),
									   VARDATA_ANY(texti),
									   VARSIZE_ANY_EXHDR(text0),
									   VARSIZE_ANY_EXHDR(texti));

		if (tmp < commonLen)
			commonLen = tmp;
	}

	/*
	 * 如有必要，限制前缀长度，以确保结果
	 * 内部元组能放进一页。
	 */
	commonLen = Min(commonLen, SPGIST_MAX_PREFIX_LENGTH);

	/* 若该串非空，则将其设为节点前缀 */
	if (commonLen == 0)
	{
		out->hasPrefix = false;
	}
	else
	{
		out->hasPrefix = true;
		out->prefixDatum = formTextDatum(VARDATA_ANY(text0), commonLen);
	}

	/* 从每个值中提取节点标签（第一个非公共字节） */
	nodes = (spgNodePtr *) palloc(sizeof(spgNodePtr) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		text	   *texti = DatumGetTextPP(in->datums[i]);

		if (commonLen < VARSIZE_ANY_EXHDR(texti))
			nodes[i].c = *(unsigned char *) (VARDATA_ANY(texti) + commonLen);
		else
			nodes[i].c = -1;	/* 若字符串全是公共部分则用 -1 */
		nodes[i].i = i;
		nodes[i].d = in->datums[i];
	}

	/*
	 * 按标签值排序，以便把值归组到各节点。这也保证节点按标签值
	 * 有序，从而可以在 searchChar 中使用
	 * 二分查找。
	 */
	qsort(nodes, in->nTuples, sizeof(*nodes), cmpNodePtr);

	/* 然后输出结果 */
	out->nNodes = 0;
	out->nodeLabels = (Datum *) palloc(sizeof(Datum) * in->nTuples);
	out->mapTuplesToNodes = (int *) palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = (Datum *) palloc(sizeof(Datum) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		text	   *texti = DatumGetTextPP(nodes[i].d);
		Datum		leafD;

		if (i == 0 || nodes[i].c != nodes[i - 1].c)
		{
			out->nodeLabels[out->nNodes] = Int16GetDatum(nodes[i].c);
			out->nNodes++;
		}

		if (commonLen < VARSIZE_ANY_EXHDR(texti))
			leafD = formTextDatum(VARDATA_ANY(texti) + commonLen + 1,
								  VARSIZE_ANY_EXHDR(texti) - commonLen - 1);
		else
			leafD = formTextDatum(NULL, 0);

		out->leafTupleDatums[nodes[i].i] = leafD;
		out->mapTuplesToNodes[nodes[i].i] = out->nNodes - 1;
	}

	PG_RETURN_VOID();
}

Datum
spg_text_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	bool		collate_is_c = lc_collate_is_c(PG_GET_COLLATION());
	text	   *reconstructedValue;
	text	   *reconstrText;
	int			maxReconstrLen;
	text	   *prefixText = NULL;
	int			prefixSize = 0;
	int			i;

	/*
	 * 重建此元组所表示的值，包括父级数据、本元组的前缀（若有），
	 * 以及非哑的节点标签。in->level 应为先前已重建值的长度，
	 * 此处追加的字节数为 prefixSize 或
	 * prefixSize + 1。
	 *
	 * 注意：我们假定 in->reconstructedValue 未经 toast，且没有短
	 * varlena 头。这没问题，因为它必由本例程先前调用创建，且我们
	 * 始终发出长格式的重建
	 * 值。
	 */
	reconstructedValue = (text *) DatumGetPointer(in->reconstructedValue);
	Assert(reconstructedValue == NULL ? in->level == 0 :
		   VARSIZE_ANY_EXHDR(reconstructedValue) == in->level);

	maxReconstrLen = in->level + 1;
	if (in->hasPrefix)
	{
		prefixText = DatumGetTextPP(in->prefixDatum);
		prefixSize = VARSIZE_ANY_EXHDR(prefixText);
		maxReconstrLen += prefixSize;
	}

	reconstrText = palloc(VARHDRSZ + maxReconstrLen);
	SET_VARSIZE(reconstrText, VARHDRSZ + maxReconstrLen);

	if (in->level)
		memcpy(VARDATA(reconstrText),
			   VARDATA(reconstructedValue),
			   in->level);
	if (prefixSize)
		memcpy(((char *) VARDATA(reconstrText)) + in->level,
			   VARDATA_ANY(prefixText),
			   prefixSize);
	/* reconstrText 的最后一个字节将在下面填入 */

	/*
	 * 扫描子节点。对每个节点，补全重建值并检查是否与查询
	 * 一致。若一致，则向输出数组
	 * 发出一项。
	 */
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	out->levelAdds = (int *) palloc(sizeof(int) * in->nNodes);
	out->reconstructedValues = (Datum *) palloc(sizeof(Datum) * in->nNodes);
	out->nNodes = 0;

	for (i = 0; i < in->nNodes; i++)
	{
		int16		nodeChar = DatumGetInt16(in->nodeLabels[i]);
		int			thisLen;
		bool		res = true;
		int			j;

		/* 若 nodeChar 是哑值，则不要把它纳入数据 */
		if (nodeChar <= 0)
			thisLen = maxReconstrLen - 1;
		else
		{
			((unsigned char *) VARDATA(reconstrText))[maxReconstrLen - 1] = nodeChar;
			thisLen = maxReconstrLen;
		}

		for (j = 0; j < in->nkeys; j++)
		{
			StrategyNumber strategy = in->scankeys[j].sk_strategy;
			text	   *inText;
			int			inSize;
			int			r;

			/*
			 * 若是排序规则感知操作符，但排序规则为 C，则可按非排序规则
			 * 感知处理。在非 C 排序规则下需要遍历整棵树 :-( 因此这里
			 * 做任何检查都没有意义。（另请注意，我们的重建值很可能以
			 * 不完整的多字节字符结尾，因此对它做任何编码敏感测试
			 * 本来就有
			 * 风险。）
			 */
			if (SPG_IS_COLLATION_AWARE_STRATEGY(strategy))
			{
				if (collate_is_c)
					strategy -= SPG_STRATEGY_ADDITION;
				else
					continue;
			}

			inText = DatumGetTextPP(in->scankeys[j].sk_argument);
			inSize = VARSIZE_ANY_EXHDR(inText);

			r = memcmp(VARDATA(reconstrText), VARDATA_ANY(inText),
					   Min(inSize, thisLen));

			switch (strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					if (r > 0)
						res = false;
					break;
				case BTEqualStrategyNumber:
					if (r != 0 || inSize < thisLen)
						res = false;
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					if (r < 0)
						res = false;
					break;
				case RTPrefixStrategyNumber:
					if (r != 0)
						res = false;
					break;
				default:
					elog(ERROR, "unrecognized strategy number: %d",
						 in->scankeys[j].sk_strategy);
					break;
			}

			if (!res)
				break;			/* 无需再考虑其余条件 */
		}

		if (res)
		{
			out->nodeNumbers[out->nNodes] = i;
			out->levelAdds[out->nNodes] = thisLen - in->level;
			SET_VARSIZE(reconstrText, VARHDRSZ + thisLen);
			out->reconstructedValues[out->nNodes] =
				datumCopy(PointerGetDatum(reconstrText), false, -1);
			out->nNodes++;
		}
	}

	PG_RETURN_VOID();
}

Datum
spg_text_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	int			level = in->level;
	text	   *leafValue,
			   *reconstrValue = NULL;
	char	   *fullValue;
	int			fullLen;
	bool		res;
	int			j;

	/* 所有测试都是精确的 */
	out->recheck = false;

	leafValue = DatumGetTextPP(in->leafDatum);

	/* 如上所述，in->reconstructedValue 未经 toast，也不是短格式。 */
	if (DatumGetPointer(in->reconstructedValue))
		reconstrValue = (text *) DatumGetPointer(in->reconstructedValue);

	Assert(reconstrValue == NULL ? level == 0 :
		   VARSIZE_ANY_EXHDR(reconstrValue) == level);

	/* 重建此叶子元组所表示的完整字符串 */
	fullLen = level + VARSIZE_ANY_EXHDR(leafValue);
	if (VARSIZE_ANY_EXHDR(leafValue) == 0 && level > 0)
	{
		fullValue = VARDATA(reconstrValue);
		out->leafValue = PointerGetDatum(reconstrValue);
	}
	else
	{
		text	   *fullText = palloc(VARHDRSZ + fullLen);

		SET_VARSIZE(fullText, VARHDRSZ + fullLen);
		fullValue = VARDATA(fullText);
		if (level)
			memcpy(fullValue, VARDATA(reconstrValue), level);
		if (VARSIZE_ANY_EXHDR(leafValue) > 0)
			memcpy(fullValue + level, VARDATA_ANY(leafValue),
				   VARSIZE_ANY_EXHDR(leafValue));
		out->leafValue = PointerGetDatum(fullText);
	}

	/* 执行所需的 comparison */
	res = true;
	for (j = 0; j < in->nkeys; j++)
	{
		StrategyNumber strategy = in->scankeys[j].sk_strategy;
		text	   *query = DatumGetTextPP(in->scankeys[j].sk_argument);
		int			queryLen = VARSIZE_ANY_EXHDR(query);
		int			r;

		if (strategy == RTPrefixStrategyNumber)
		{
			/*
			 * 若 level >= 查询长度，则 reconstrValue 必以查询（前缀）串开头，
			 * 因此无需再检查。
			 */
			res = (level >= queryLen) ||
				DatumGetBool(DirectFunctionCall2Coll(text_starts_with,
													 PG_GET_COLLATION(),
													 out->leafValue,
													 PointerGetDatum(query)));

			if (!res)			/* 无需再考虑其余条件 */
				break;

			continue;
		}

		if (SPG_IS_COLLATION_AWARE_STRATEGY(strategy))
		{
			/* 排序规则感知比较 */
			strategy -= SPG_STRATEGY_ADDITION;

			/* 若启用断言，则校验重建字符串的编码 */
			Assert(pg_verifymbstr(fullValue, fullLen, false));

			r = varstr_cmp(fullValue, fullLen,
						   VARDATA_ANY(query), queryLen,
						   PG_GET_COLLATION());
		}
		else
		{
			/* 非排序规则感知比较 */
			r = memcmp(fullValue, VARDATA_ANY(query), Min(queryLen, fullLen));

			if (r == 0)
			{
				if (queryLen > fullLen)
					r = -1;
				else if (queryLen < fullLen)
					r = 1;
			}
		}

		switch (strategy)
		{
			case BTLessStrategyNumber:
				res = (r < 0);
				break;
			case BTLessEqualStrategyNumber:
				res = (r <= 0);
				break;
			case BTEqualStrategyNumber:
				res = (r == 0);
				break;
			case BTGreaterEqualStrategyNumber:
				res = (r >= 0);
				break;
			case BTGreaterStrategyNumber:
				res = (r > 0);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[j].sk_strategy);
				res = false;
				break;
		}

		if (!res)
			break;				/* 无需再考虑其余条件 */
	}

	PG_RETURN_BOOL(res);
}
