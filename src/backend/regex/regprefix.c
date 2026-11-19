/*-------------------------------------------------------------------------
 *
 * regprefix.c
 *	从已编译正则提取公共前缀（若有）。
 *
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/backend/regex/regprefix.c
 *
 *-------------------------------------------------------------------------
 */

#include "regex/regguts.h"


/*
 * 前向声明
 */
static int	findprefix(struct cnfa *cnfa, struct colormap *cm,
					   chr *string, size_t *slength);


/*
 * pg_regprefix - 取正则表达式的公共前缀
 *
 * 返回其一:
 *	REG_NOMATCH: 匹配该正则的串无公共前缀
 *	REG_PREFIX: 匹配该正则的串有公共前缀
 *	REG_EXACT: 满足该正则的串皆须匹配同一字符串
 *	或 REG_XXX 错误码
 *
 * 非失败情形下，*string 设为含公共前缀或精确值的
 * palloc'd 串，长度为 *slength (以 chr 计，
 * 非字节!).
 *
 * 本函数未精确分析全部复杂情形（如环视
 * 约束）。因此匹配所报前缀或精确匹配串的某些串
 * 可能并不满足该正则。但绝不应出现满足该正则的串
 * 却不匹配所报前缀或
 * 精确匹配串。
 */
int
pg_regprefix(regex_t *re,
			 chr **string,
			 size_t *slength)
{
	struct guts *g;
	struct cnfa *cnfa;
	int			st;

	/* 健全检查 */
	if (string == NULL || slength == NULL)
		return REG_INVARG;
	*string = NULL;				/* 为失败情形初始化 */
	*slength = 0;
	if (re == NULL || re->re_magic != REMAGIC)
		return REG_INVARG;
	if (re->re_csize != sizeof(chr))
		return REG_MIXED;

	/* 初始化区域设置相关支持 */
	pg_set_regex_collation(re->re_collation);

	/* setup */
	g = (struct guts *) re->re_guts;
	if (g->info & REG_UIMPOSSIBLE)
		return REG_NOMATCH;

	/*
	 * 本实现仅考虑最顶层正则树节点的搜索 NFA。
	 * 因此反向引用等约束未充分应用，按本函数
	 * API 规格这是允许的。
	 */
	assert(g->tree != NULL);
	cnfa = &g->tree->cnfa;

	/* matchall NFA 永无固定前缀 */
	if (cnfa->flags & MATCHALL)
		return REG_NOMATCH;

	/*
	 * 正确 NFA 不应含任何无出口环，故我们的遍历
	 * 不应回到先前访问过的 NFA 状态。因此输出串
	 * 至多需要 nstates 个 chr。
	 */
	*string = (chr *) MALLOC(cnfa->nstates * sizeof(chr));
	if (*string == NULL)
		return REG_ESPACE;

	/* do it */
	st = findprefix(cnfa, &g->cmap, *string, slength);

	assert(*slength <= cnfa->nstates);

	/* 清理 */
	if (st != REG_PREFIX && st != REG_EXACT)
	{
		FREE(*string);
		*string = NULL;
		*slength = 0;
	}

	return st;
}

/*
 * findprefix - 从 cNFA 提取公共前缀
 *
 * 结果写入预分配的 chr 数组 string[]，每写入一
 * chr 就递增 *slength（须预先置零）。
 */
static int						/* regprefix 返回码 */
findprefix(struct cnfa *cnfa,
		   struct colormap *cm,
		   chr *string,
		   size_t *slength)
{
	int			st;
	int			nextst;
	color		thiscolor;
	chr			c;
	struct carc *ca;

	/*
	 * 状态 "pre" 只能有 BOS/BOL 出弧，否则模式未
	 * 左锚定。若兼有 BOS 与 BOL，二者须转到同一
	 * 下一态.
	 */
	st = cnfa->pre;
	nextst = -1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co == cnfa->bos[0] || ca->co == cnfa->bos[1])
		{
			if (nextst == -1)
				nextst = ca->to;
			else if (nextst != ca->to)
				return REG_NOMATCH;
		}
		else
			return REG_NOMATCH;
	}
	if (nextst == -1)
		return REG_NOMATCH;

	/*
	 * 扫描相继状态，一发现某状态有多于一个可接受
	 * 转移字符即停（出弧上有多色，或某色有多于一
	 * 个成员 chr）。
	 *
	 * 也可能碰到多条出弧皆标为同一单点色的状态；
	 * 这来自形如 "^ab(cde|cxy)" 的模式。此时我们把
	 * chr "c" 追加到输出串，但随后以 nextst == -1 退出
	 * 循环。这留下一点余地：模式若为 "^ab(cde|cdy)"，
	 * 我们不会注意到 "d" 也可加入
	 * 前缀。但追逐多条并行状态链似乎不值得
	 * 费这功夫。
	 */
	do
	{
		st = nextst;
		nextst = -1;
		thiscolor = COLORLESS;
		for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
		{
			/* 可忽略 BOS/BOL 弧 */
			if (ca->co == cnfa->bos[0] || ca->co == cnfa->bos[1])
				continue;

			/*
			 * 但 EOS/EOL 弧会终止搜索，彩虹弧亦然
			 * 与 LACON
			 */
			if (ca->co == cnfa->eos[0] || ca->co == cnfa->eos[1] ||
				ca->co == RAINBOW || ca->co >= cnfa->ncolors)
			{
				thiscolor = COLORLESS;
				break;
			}
			if (thiscolor == COLORLESS)
			{
				/* 首条普通出弧 */
				thiscolor = ca->co;
				nextst = ca->to;
			}
			else if (thiscolor == ca->co)
			{
				/* 同色的另一条普通出弧 */
				nextst = -1;
			}
			else
			{
				/* 普通出弧色多于一种则终止搜索 */
				thiscolor = COLORLESS;
				break;
			}
		}
		/* 若普通出弧上未恰找到一色则结束 */
		if (thiscolor == COLORLESS)
			break;
		/* 该色必须是单点色 */
		if (cm->cd[thiscolor].nschrs != 1)
			break;
		/* 不得有任何高色图项 */
		if (cm->cd[thiscolor].nuchrs != 0)
			break;

		/*
		 * 找出该色唯一成员 chr 并追加到前缀
		 * 串。一般而言色图数据结构不提供查找色成员
		 * chr 的办法，除非对每个可能 chr 值试 GETCOLOR()，
		 * 这完全行不通。但对我们关心的情形，测 "firstchr" 
		 * 值应已足够，即该色曾加入的首个
		 * chr。存在它可能已不再是该色成员的情形（故
		 * 仍须测试），但它们不太可能出现在公共前缀的
		 * 成员字符上。若真碰上这种边角情形，
		 * 我们只是退出，不向该前缀串
		 * 追加任何内容。
		 */
		c = cm->cd[thiscolor].firstchr;
		if (GETCOLOR(cm, c) != thiscolor)
			break;

		string[(*slength)++] = c;

		/* 前进到下一状态，但仅当下一状态唯一时 */
	} while (nextst != -1);

	/*
	 * 若我们停在一个仅有通向 "post" 状态的 EOS/EOL
	 * 出弧的状态，则得到精确匹配串。注意即便串长
	 * 为零这也成立。
	 */
	nextst = -1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co == cnfa->eos[0] || ca->co == cnfa->eos[1])
		{
			if (nextst == -1)
				nextst = ca->to;
			else if (nextst != ca->to)
			{
				nextst = -1;
				break;
			}
		}
		else
		{
			nextst = -1;
			break;
		}
	}
	if (nextst == cnfa->post)
		return REG_EXACT;

	/*
	 * 否则，若无法识别任何前缀字符，则报
	 * NOMATCH —— 模式已左锚定，但未指定任何
	 * 特定首字符。
	 */
	if (*slength > 0)
		return REG_PREFIX;

	return REG_NOMATCH;
}
