/*-------------------------------------------------------------------------
 *
 * like_match.c
 *	  LIKE 模式匹配内部代码。
 *
 * 本文件被 like.c 引用四次，以提供四种匹配代码：
 * (1) 单字节编码，(2) UTF8，(3) 其他多字节编码，
 * 以及 (4) 单字节编码下大小写不敏感匹配。
 * （UTF8 是特例，因为我们可以用比通用多字节编码
 * 高效得多的 NextChar 版本。）
 *
 * 在包含之前，需要定义以下宏：
 *
 * NextChar
 * MatchText - 所需函数的名字
 * do_like_escape - 可选函数名 - 需要 CHAREQ 和 CopyAdvChar
 * MATCH_LOWER - 为情形 (4) 定义 1 字节字符的大小写折叠
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/like_match.c
 *
 *-------------------------------------------------------------------------
 */

/*
 *	Originally written by Rich $alz, mirror!rs, Wed Nov 26 19:03:17 EST 1986.
 *	Rich $alz is now <rsalz@bbn.com>.
 *	感谢 Lars Mathiesen <thorinn@diku.dk> 提供 LIKE_ABORT
 *	代码。
 *
 *	本代码由本人无耻地从 "pql" 代码中窃取而来，
 *	并稍作修改 :)
 *
 *	所有提及 "star" 一词之处均已替换为 "percent"
 *	所有提及 "wild" 之处均已替换为 "like"
 *
 *	所有 shell 正则匹配机制都换成了单纯的 "_" 和 "%"
 *
 *	由于手头没有 SQL 标准文本，我不确定是否
 *	应保留 '\' 转义字符处理。
 *
 *	Keith Parks. <keith@mtcc.demon.co.uk>
 *
 *	SQL 允许这样指定转义字符：
 *	LIKE <模式> ESCAPE <转义字符>。我们是小团队，
 *	所以强制用 '\'。 - ay 7/95
 *
 *	现在我们有 like_escape() 函数，可将带任意指定
 *	转义字符（或无）的模式转换为内部默认转义
 *	字符，仍为 '\'。 - tgl 9/2000
 *
 * 代码已重写，避免要求以空字符结尾的字符串，
 * 这又让我们得以省去一些 memcpy() 操作。
 * 此代码应更快、更省内存，但不作承诺...
 * - thomas 2000-08-06
 */


/*--------------------
 *	匹配文本与模式，返回 LIKE_TRUE/LIKE_FALSE/LIKE_ABORT。
 *
 *	LIKE_TRUE: 匹配
 *	LIKE_FALSE: 不匹配
 *	LIKE_ABORT: 不仅不匹配，而且文本太短。
 *
 * 若返回 LIKE_ABORT，则文本的任何后缀都无法再匹配
 * 该模式，上层 % 扫描可就此停止扫描。
 *--------------------
 */

#ifdef MATCH_LOWER
#define GETCHAR(t) MATCH_LOWER(t)
#else
#define GETCHAR(t) (t)
#endif

static int
MatchText(const char *t, int tlen, const char *p, int plen,
		  pg_locale_t locale, bool locale_is_c)
{
	/* 匹配一切模式的快速路径 */
	if (plen == 1 && *p == '%')
		return LIKE_TRUE;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/*
	 * 在本循环中，匹配通配符时按字符前进（因此在
	 * 递归进入本函数时字符已正确对齐）。其他情况下，
	 * 按字节前进是安全的，因为文本与模式将保持
	 * 步调一致。这让我们能对文本与模式之间的所有比较
	 * 都按逐字节方式进行，即使对多字节编码也是
	 * 如此。
	 */
	while (tlen > 0 && plen > 0)
	{
		if (*p == '\\')
		{
			/* 下一模式字节必须字面匹配，不管是什么 */
			NextByte(p, plen);
			/* 而且按 SQL 标准必须存在一个 */
			if (plen <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
						 errmsg("LIKE pattern must not end with escape character")));
			if (GETCHAR(*p) != GETCHAR(*t))
				return LIKE_FALSE;
		}
		else if (*p == '%')
		{
			char		firstpat;

			/*
			 * % 的处理本质上是在文本中搜索一个位置，
			 * 使得文本剩余部分能匹配模式剩余部分，
			 * 并用递归调用逐一检查每个可能匹配。
			 *
			 * 若 % 之后紧跟通配符，我们可以先跳过它们，
			 * 依据是：N 个 _ 加上一个或多个 % 的序列等价于
			 * N 个 _ 加一个 %（即它将匹配至少 N 个文本
			 * 字符的任意序列）。这样，递归搜索循环
			 * 将总是使用以字面待匹配字符开头的
			 * 模式片段来运行，从而不会做多于必要
			 * 的递归。
			 */
			NextByte(p, plen);

			while (plen > 0)
			{
				if (*p == '%')
					NextByte(p, plen);
				else if (*p == '_')
				{
					/* 若剩余文本不足以匹配模式，则 ABORT */
					if (tlen <= 0)
						return LIKE_ABORT;
					NextChar(t, tlen);
					NextByte(p, plen);
				}
				else
					break;		/* 到达非通配符模式字符 */
			}

			/*
			 * 若已到模式末尾，即匹配：有一个尾部 %，
			 * 它能匹配任何剩余文本。
			 */
			if (plen <= 0)
				return LIKE_TRUE;

			/*
			 * 否则，扫描文本位置，找出能从此处匹配
			 * 模式其余部分的位置。已知剩余的第一个模式
			 * 字符必为普通或转义的字面字符，因此可以
			 * 将首个模式字节与每个文本字节比较，避免
			 * 做多于必要的递归。这一点也保证我们无需
			 * 考虑文本末部零长子串的匹配情形，
			 * 即文末处。
			 */
			if (*p == '\\')
			{
				if (plen < 2)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
							 errmsg("LIKE pattern must not end with escape character")));
				firstpat = GETCHAR(p[1]);
			}
			else
				firstpat = GETCHAR(*p);

			while (tlen > 0)
			{
				if (GETCHAR(*t) == firstpat)
				{
					int			matched = MatchText(t, tlen, p, plen,
													locale, locale_is_c);

					if (matched != LIKE_FALSE)
						return matched; /* TRUE/ABORT */
				}

				NextChar(t, tlen);
			}

			/*
			 * 文本结束且无匹配，再试更晚的起点已无
			 * 意义。
			 */
			return LIKE_ABORT;
		}
		else if (*p == '_')
		{
			/* _ 匹配任意单个字符，且我们知道存在一个 */
			NextChar(t, tlen);
			NextByte(p, plen);
			continue;
		}
		else if (GETCHAR(*p) != GETCHAR(*t))
		{
			/* 非通配符模式字符匹配文本字符失败 */
			return LIKE_FALSE;
		}

		/*
		 * 模式与文本相符，前进。
		 *
		 * 此处用 NextByte 代替 NextChar 是安全的，即使对
		 * 多字节字符集也是如此，因为我们并未紧跟在
		 * 通配符之后。如果我们正处于多字节字符的
		 * 中部，那么文本与模式双方都必然已匹配过
		 * 该字符的至少一个字节；因此不会在字符边界
		 * 上失步。另外，任何后端合法编码都不会让
		 * ASCII 字符（如 '%'）出现在字符的非首字节位置，
		 * 因此我们不会误判出一个新的通配符。
		 */
		NextByte(t, tlen);
		NextByte(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* 模式已尽而文本未尽 */

	/*
	 * 文本已尽，模式未必。仅当剩余模式可以匹配
	 * 零长字符串时才匹配，也就是零个或多个 %。
	 */
	while (plen > 0 && *p == '%')
		NextByte(p, plen);
	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * 文本结束且无匹配，因此再尝试更晚的起点来
	 * 匹配已无意义。
	 */
	return LIKE_ABORT;
}								/* MatchText() */

/*
 * like_escape() --- 给定模式与 ESCAPE 串，
 * 将该模式转换为使用 Postgres 标准反斜杠转义约定。
 */
#ifdef do_like_escape

static text *
do_like_escape(text *pat, text *esc)
{
	text	   *result;
	char	   *p,
			   *e,
			   *r;
	int			plen,
				elen;
	bool		afterescape;

	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);
	e = VARDATA_ANY(esc);
	elen = VARSIZE_ANY_EXHDR(esc);

	/*
	 * 模式最坏增长 2 倍 --- 不太可能，也不值得
	 * 更精确地计算其大小。
	 */
	result = (text *) palloc(plen * 2 + VARHDRSZ);
	r = VARDATA(result);

	if (elen == 0)
	{
		/*
		 * 无需转义字符。将模式中的反斜杠加倍，
		 * 使它们表现得像普通字符。
		 */
		while (plen > 0)
		{
			if (*p == '\\')
				*r++ = '\\';
			CopyAdvChar(r, p, plen);
		}
	}
	else
	{
		/*
		 * 指定的转义字符必须只是单个字符。
		 */
		NextChar(e, elen);
		if (elen != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
					 errmsg("invalid escape string"),
					 errhint("Escape string must be empty or one character.")));

		e = VARDATA_ANY(esc);

		/*
		 * 若指定转义为 '\'，直接照抄模式即可。
		 */
		if (*e == '\\')
		{
			memcpy(result, pat, VARSIZE_ANY(pat));
			return result;
		}

		/*
		 * 否则，将出现的指定转义字符转换为 '\'，
		 * 并将出现的 '\' 加倍 --- 除非它们紧跟在
		 * 某个转义字符之后！
		 */
		afterescape = false;
		while (plen > 0)
		{
			if (CHAREQ(p, e) && !afterescape)
			{
				*r++ = '\\';
				NextChar(p, plen);
				afterescape = true;
			}
			else if (*p == '\\')
			{
				*r++ = '\\';
				if (!afterescape)
					*r++ = '\\';
				NextChar(p, plen);
				afterescape = false;
			}
			else
			{
				CopyAdvChar(r, p, plen);
				afterescape = false;
			}
		}
	}

	SET_VARSIZE(result, r - ((char *) result));

	return result;
}
#endif							/* do_like_escape */

#ifdef CHAREQ
#undef CHAREQ
#endif

#undef NextChar
#undef CopyAdvChar
#undef MatchText

#ifdef do_like_escape
#undef do_like_escape
#endif

#undef GETCHAR

#ifdef MATCH_LOWER
#undef MATCH_LOWER

#endif
