/*-------------------------------------------------------------------------
 *
 * pl_scanner.c
 *	PL/pgSQL 的词法扫描
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/pl_scanner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "parser/scanner.h"

#include "plpgsql.h"
#include "pl_gram.h"			/* 必须在 parser/scanner.h 后 */


/* 告知扫描器如何查找标识符的凑合标志 */
IdentifierLookup plpgsql_IdentifierLookup = IDENTIFIER_LOOKUP_NORMAL;

/*
 * 关于关键字：
 *
 * 我们把保留与非保留关键字分放在各自头文件。注意
 * 不要把同一个词放进两个头文件。还要确保 pl_gram.y 的
 * unreserved_keyword 产生式与非保留头文件一致。保留
 * 关键字会传给核心扫描器，因此它们会
 * 在任何变量名之前（并取而代之）被识别。非保留词
 * 另行检查，通常在确定该标识符
 * 不是已知变量名后。若 plpgsql_IdentifierLookup 为 DECLARE 则
 * 不会识别任何变量名，因此非保留词始终有效。
 * 尤其注意，这有助于我们避免保留仅
 * DECLARE 段所需的关键字。
 *
 * 在某些情形下，宜优先识别非保留
 * 关键字，而非识别变量名。尤其在语句
 * 开头我们应优先非保留关键字，除非该语句
 * 看起来像赋值（即首个记号后跟 ':=' 或 '['）。
 * 此规则让多数引入语句的关键字保持非保留。
 * 我们仍须保留可能紧跟块标签的
 * 起始关键字，可惜所用方法判断我们是否
 * 处于语句开头时认不出这类情形。我们还得
 * 保留任何可合法后跟 ':=' 或 '[' 的关键字。
 * 另有一些情形在 pl_gram.y 里用 tok_is_keyword() 处理。
 *
 * 我们尽量避免保留超出必要的关键字；但
 * 若某词已在核心语法里保留，再不保留也没什么意义。
 * 目前下列词在此处保留，但在核心语法里不保留：
 * BEGIN BY DECLARE EXECUTE FOREACH IF LOOP STRICT WHILE
 */

/* ScanKeywordList：PL/pgSQL 关键字查找数据 */
#include "pl_reserved_kwlist_d.h"
#include "pl_unreserved_kwlist_d.h"

/* PL/pgSQL 关键字的记号码 */
#define PG_KEYWORD(kwname, value) value,

static const uint16 ReservedPLKeywordTokens[] = {
#include "pl_reserved_kwlist.h"
};

static const uint16 UnreservedPLKeywordTokens[] = {
#include "pl_unreserved_kwlist.h"
};

#undef PG_KEYWORD

/*
 * 本宏必须识别一切可紧挨在一条
 * PL/pgSQL 可执行语句前的记号（即语法里
 * 的 proc_sect 或 proc_stmt）。所幸不多，故这样硬编码
 * 看起来就够。
 */
#define AT_STMT_START(prev_token) \
	((prev_token) == ';' || \
	 (prev_token) == K_BEGIN || \
	 (prev_token) == K_THEN || \
	 (prev_token) == K_ELSE || \
	 (prev_token) == K_LOOP)


/* 记号的辅助数据（不含记号类型） */
typedef struct
{
	YYSTYPE		lval;			/* 语义信息 */
	YYLTYPE		lloc;			/* scanbuf 中偏移 */
	int			leng;			/* 字节长度 */
} TokenAuxData;

/*
 * 扫描器工作状态。将来我们也许想把这些全部
 * 折进一个 YY_EXTRA 结构。眼下 plpgsql 的
 * 词法器可重入并无必要，且传递 yyscanner
 * 指针的记法负担已大到没有必要就不想做。
 */

/* 核心词法器所需的东西 */
static core_yyscan_t yyscanner = NULL;
static core_yy_extra_type core_yy;

/* 原始输入字符串 */
static const char *scanorig;

/* 当前记号长度（对应 plpgsql_yylval 与 plpgsql_yylloc） */
static int	plpgsql_yyleng;

/* 当前记号码（对应 plpgsql_yylval 与 plpgsql_yylloc） */
static int	plpgsql_yytoken;

/* 记号推回栈 */
#define MAX_PUSHBACKS 4

static int	num_pushbacks;
static int	pushback_token[MAX_PUSHBACKS];
static TokenAuxData pushback_auxdata[MAX_PUSHBACKS];

/* plpgsql_location_to_lineno() 的状态 */
static const char *cur_line_start;
static const char *cur_line_end;
static int	cur_line_num;

/* 内部函数 */
static int	internal_yylex(TokenAuxData *auxdata);
static void push_back_token(int token, TokenAuxData *auxdata);
static void location_lineno_init(void);


/*
 * 这是 PL/pgSQL 语法所调用的 yylex 例程。
 * 它是核心词法器的包装，能识别
 * PL/pgSQL 变量，并以特殊 T_DATUM 记号返回。若某个
 * 词或复合词不匹配任何变量名，或若匹配
 * 被 plpgsql_IdentifierLookup 关掉，就返回为
 * T_WORD 或 T_CWORD（分别），或匹配则作为
 * 非保留关键字。
 */
int
plpgsql_yylex(void)
{
	int			tok1;
	TokenAuxData aux1;
	int			kwnum;

	tok1 = internal_yylex(&aux1);
	if (tok1 == IDENT || tok1 == PARAM)
	{
		int			tok2;
		TokenAuxData aux2;

		tok2 = internal_yylex(&aux2);
		if (tok2 == '.')
		{
			int			tok3;
			TokenAuxData aux3;

			tok3 = internal_yylex(&aux3);
			if (tok3 == IDENT)
			{
				int			tok4;
				TokenAuxData aux4;

				tok4 = internal_yylex(&aux4);
				if (tok4 == '.')
				{
					int			tok5;
					TokenAuxData aux5;

					tok5 = internal_yylex(&aux5);
					if (tok5 == IDENT)
					{
						if (plpgsql_parse_tripword(aux1.lval.str,
												   aux3.lval.str,
												   aux5.lval.str,
												   &aux1.lval.wdatum,
												   &aux1.lval.cword))
							tok1 = T_DATUM;
						else
							tok1 = T_CWORD;
						/* 调整记号长度以含 A.B.C */
						aux1.leng = aux5.lloc - aux1.lloc + aux5.leng;
					}
					else
					{
						/* 非 A.B.C，故只处理 A.B */
						push_back_token(tok5, &aux5);
						push_back_token(tok4, &aux4);
						if (plpgsql_parse_dblword(aux1.lval.str,
												  aux3.lval.str,
												  &aux1.lval.wdatum,
												  &aux1.lval.cword))
							tok1 = T_DATUM;
						else
							tok1 = T_CWORD;
						/* 调整记号长度以含 A.B */
						aux1.leng = aux3.lloc - aux1.lloc + aux3.leng;
					}
				}
				else
				{
					/* 非 A.B.C，故只处理 A.B */
					push_back_token(tok4, &aux4);
					if (plpgsql_parse_dblword(aux1.lval.str,
											  aux3.lval.str,
											  &aux1.lval.wdatum,
											  &aux1.lval.cword))
						tok1 = T_DATUM;
					else
						tok1 = T_CWORD;
					/* 调整记号长度以含 A.B */
					aux1.leng = aux3.lloc - aux1.lloc + aux3.leng;
				}
			}
			else
			{
				/* 非 A.B，故只处理 A */
				push_back_token(tok3, &aux3);
				push_back_token(tok2, &aux2);
				if (plpgsql_parse_word(aux1.lval.str,
									   core_yy.scanbuf + aux1.lloc,
									   true,
									   &aux1.lval.wdatum,
									   &aux1.lval.word))
					tok1 = T_DATUM;
				else if (!aux1.lval.word.quoted &&
						 (kwnum = ScanKeywordLookup(aux1.lval.word.ident,
													&UnreservedPLKeywords)) >= 0)
				{
					aux1.lval.keyword = GetScanKeyword(kwnum,
													   &UnreservedPLKeywords);
					tok1 = UnreservedPLKeywordTokens[kwnum];
				}
				else
					tok1 = T_WORD;
			}
		}
		else
		{
			/* 非 A.B，故只处理 A */
			push_back_token(tok2, &aux2);

			/*
			 * 看它是否匹配变量名，除了这种情形：
			 * 我们处于语句开头且下一记号不是
			 * 赋值或 '['。此时它不可能合法地是
			 * 变量名。跳过查找则允许使用会与
			 * 引入语句的 plpgsql 或核心关键字相冲突的
			 * 变量名（例如 "comment"）。没有这套特殊
			 * 逻辑，每个引入语句的关键字实际上就会
			 * 在 PL/pgSQL 里被保留，那会很难受。
			 *
			 * 若它不是变量名，就试着匹配非保留
			 * 的 plpgsql 关键字。若也不是，则为 T_WORD。
			 *
			 * 注意：即使不做变量查找，也必须调
			 * plpgsql_parse_word，因它设 aux1.lval.word
			 * 供非变量情形
			 */
			if (plpgsql_parse_word(aux1.lval.str,
								   core_yy.scanbuf + aux1.lloc,
								   (!AT_STMT_START(plpgsql_yytoken) ||
									(tok2 == '=' || tok2 == COLON_EQUALS ||
									 tok2 == '[')),
								   &aux1.lval.wdatum,
								   &aux1.lval.word))
				tok1 = T_DATUM;
			else if (!aux1.lval.word.quoted &&
					 (kwnum = ScanKeywordLookup(aux1.lval.word.ident,
												&UnreservedPLKeywords)) >= 0)
			{
				aux1.lval.keyword = GetScanKeyword(kwnum,
												   &UnreservedPLKeywords);
				tok1 = UnreservedPLKeywordTokens[kwnum];
			}
			else
				tok1 = T_WORD;
		}
	}
	else
	{
		/*
		 * 不是可能的 plpgsql 变量名，直接返回数据。
		 *
		 * 注意：若语法先前推回过一个
		 * T_DATUM、T_CWORD、T_WORD 或非保留关键字记号，
		 * 它是上一轮查找返回的；因此推回不会多花查找
		 * 工作，因为同一记号绝不会把上面代码做两遍。
		 * 此性质也让人能放心依赖上面那个
		 * 是否语句开头测试里 plpgsql_yytoken 的旧值。
		 */
	}

	plpgsql_yylval = aux1.lval;
	plpgsql_yylloc = aux1.lloc;
	plpgsql_yyleng = aux1.leng;
	plpgsql_yytoken = tok1;
	return tok1;
}

/*
 * 返回 plpgsql_yylex() 上次所返回记号的长度。
 *
 * 若是复合记号，长度包含所有各部分。
 */
int
plpgsql_token_length(void)
{
	return plpgsql_yyleng;
}

/*
 * 内部 yylex 函数。它包装核心词法器，并加一项功能：
 * 即记号推回栈。我们还做几处平凡的单记号
 * 翻译，把核心词法器的结果改成我们要的，尤其是
 * 从 core_YYSTYPE 接到 YYSTYPE 联合。
 */
static int
internal_yylex(TokenAuxData *auxdata)
{
	int			token;
	const char *yytext;

	if (num_pushbacks > 0)
	{
		num_pushbacks--;
		token = pushback_token[num_pushbacks];
		*auxdata = pushback_auxdata[num_pushbacks];
	}
	else
	{
		token = core_yylex(&auxdata->lval.core_yystype,
						   &auxdata->lloc,
						   yyscanner);

		/* 在 yytext 被改掉之前记住它的长度 */
		yytext = core_yy.scanbuf + auxdata->lloc;
		auxdata->leng = strlen(yytext);

		/* 检查 << >> 与 #，核心把它们当成运算符 */
		if (token == Op)
		{
			if (strcmp(auxdata->lval.str, "<<") == 0)
				token = LESS_LESS;
			else if (strcmp(auxdata->lval.str, ">>") == 0)
				token = GREATER_GREATER;
			else if (strcmp(auxdata->lval.str, "#") == 0)
				token = '#';
		}

		/* 核心以 ival 返回 PARAM，但我们当 IDENT 处理 */
		else if (token == PARAM)
		{
			auxdata->lval.str = pstrdup(yytext);
		}
	}

	return token;
}

/*
 * 推回一记号，供下次 internal_yylex() 重读。
 */
static void
push_back_token(int token, TokenAuxData *auxdata)
{
	if (num_pushbacks >= MAX_PUSHBACKS)
		elog(ERROR, "too many tokens pushed back");
	pushback_token[num_pushbacks] = token;
	pushback_auxdata[num_pushbacks] = *auxdata;
	num_pushbacks++;
}

/*
 * 推回单个记号，供下次 plpgsql_yylex() 调用重读。
 *
 * 注意：这不会让 yylval 或 yylloc "回退"。另外，
 * 推回一个不是你读到的记号码并不是好主意。
 */
void
plpgsql_push_back_token(int token)
{
	TokenAuxData auxdata;

	auxdata.lval = plpgsql_yylval;
	auxdata.lloc = plpgsql_yylloc;
	auxdata.leng = plpgsql_yyleng;
	push_back_token(token, &auxdata);
}

/*
 * 判断记号是否为非保留关键字。
 *
 * 若是，其小写形式已作为记号值返回，故我们
 * 不必在此再提供那份数据。
 */
bool
plpgsql_token_is_unreserved_keyword(int token)
{
	int			i;

	for (i = 0; i < lengthof(UnreservedPLKeywordTokens); i++)
	{
		if (UnreservedPLKeywordTokens[i] == token)
			return true;
	}
	return false;
}

/*
 * 把函数文本从 startlocation 起延伸到
 * 不含 endlocation，接到 "buf" 现有内容之后。
 */
void
plpgsql_append_source_text(StringInfo buf,
						   int startlocation, int endlocation)
{
	Assert(startlocation <= endlocation);
	appendBinaryStringInfo(buf, scanorig + startlocation,
						   endlocation - startlocation);
}

/*
 * 在输入流中向前看一个记号。只提供记号码，
 * 不提供位置之类的任何辅助信息。
 *
 * 注意：此处不做变量或非保留关键字查找，它们会
 * 被返回为 IDENT。保留关键字照常解析。
 */
int
plpgsql_peek(void)
{
	int			tok1;
	TokenAuxData aux1;

	tok1 = internal_yylex(&aux1);
	push_back_token(tok1, &aux1);
	return tok1;
}

/*
 * 在输入流中向前看两个记号。第一个记号及其
 * 在查询中的位置经 *tok1_p 与 *tok1_loc 返回，第二个记号
 * 及其位置也经 *tok2_p、*tok2_loc。
 *
 * 注意：此处不做变量或非保留关键字查找，它们会
 * 被返回为 IDENT。保留关键字照常解析。
 */
void
plpgsql_peek2(int *tok1_p, int *tok2_p, int *tok1_loc, int *tok2_loc)
{
	int			tok1,
				tok2;
	TokenAuxData aux1,
				aux2;

	tok1 = internal_yylex(&aux1);
	tok2 = internal_yylex(&aux2);

	*tok1_p = tok1;
	if (tok1_loc)
		*tok1_loc = aux1.lloc;
	*tok2_p = tok2;
	if (tok2_loc)
		*tok2_loc = aux2.lloc;

	push_back_token(tok2, &aux2);
	push_back_token(tok1, &aux1);
}

/*
 * plpgsql_scanner_errposition
 *	若可能，报告错误光标位置。
 *
 * 预期在 ereport() 调用里使用。返回值
 * 是虚值，其实恒为 0。
 *
 * 注意：这只能用于初次解析 plpgsql 函数时
 * 发出的消息，因为它需要 scanorig
 * 串仍然可用。
 */
int
plpgsql_scanner_errposition(int location)
{
	int			pos;

	if (location < 0 || scanorig == NULL)
		return 0;				/* 位置未知时为空操作 */

	/* 把字节偏移换成字符序号 */
	pos = pg_mbstrlen_with_len(scanorig, location) + 1;
	/* 并把它传给 ereport 机制 */
	(void) internalerrposition(pos);
	/* 也传入函数体字符串 */
	return internalerrquery(scanorig);
}

/*
 * plpgsql_yyerror
 *	报告词法器或语法错误。
 *
 * 消息的光标位置指当前记号（即
 * plpgsql_yylex() 刚返回的）。
 * 这对 Bison 解析器的语法错误消息是可以的，因为 Bison
 * 解析器一遇到首个无法解析的记号就报错。
 * 别为其他目的使用 yyerror，因为光标位置可能
 * 有误导！
 */
void
plpgsql_yyerror(const char *message)
{
	char	   *yytext = core_yy.scanbuf + plpgsql_yylloc;

	if (*yytext == '\0')
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: %s is typically the translation of "syntax error" */
				 errmsg("%s at end of input", _(message)),
				 plpgsql_scanner_errposition(plpgsql_yylloc)));
	}
	else
	{
		/*
		 * 若我们做过任何向前看，flex 会已恢复
		 * 记号结束之后的那个字符。再清掉，以便只报
		 * 这里的单个记号。这会改 scanbuf，但我们不再
		 * 管它了。
		 */
		yytext[plpgsql_yyleng] = '\0';

		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: first %s is typically the translation of "syntax error" */
				 errmsg("%s at or near \"%s\"", _(message), yytext),
				 plpgsql_scanner_errposition(plpgsql_yylloc)));
	}
}

/*
 * 给定位置（函数源文本中的字节偏移），
 * 返回一个行号。
 *
 * 我们预期这通常是对一串递增的
 * 位置值来调用，故相应优化：跟踪
 * 当前行的两端。
 */
int
plpgsql_location_to_lineno(int location)
{
	const char *loc;

	if (location < 0 || scanorig == NULL)
		return 0;				/* 垃圾进，垃圾出 */
	loc = scanorig + location;

	/* 若输入位置往回走，则正确但不快 */
	if (loc < cur_line_start)
		location_lineno_init();

	while (cur_line_end != NULL && loc > cur_line_end)
	{
		cur_line_start = cur_line_end + 1;
		cur_line_num++;
		cur_line_end = strchr(cur_line_start, '\n');
	}

	return cur_line_num;
}

/* 初始化或重置 plpgsql_location_to_lineno 的状态 */
static void
location_lineno_init(void)
{
	cur_line_start = scanorig;
	cur_line_num = 1;

	cur_line_end = strchr(cur_line_start, '\n');
}

/* 返回最近算出的行号 */
int
plpgsql_latest_lineno(void)
{
	return cur_line_num;
}


/*
 * 尚未做任何实际解析时调用
 *
 * 注意：传入的 "str" 必须有效到 plpgsql_scanner_finish()。
 * 虽然并不直接送给 flex，我们仍需要原始字符串
 * 供错误消息引用。
 */
void
plpgsql_scanner_init(const char *str)
{
	/* 启动核心扫描器 */
	yyscanner = scanner_init(str, &core_yy,
							 &ReservedPLKeywords, ReservedPLKeywordTokens);

	/*
	 * scanorig 指向原始字符串，与扫描器的
	 * scanbuf 不同，不会被 flex 当场修改。注意虽然
	 * yytext 指向 scanbuf，我们仍能把这些位置
	 * 从串首起的偏移同样用于 scanorig。
	 */
	scanorig = str;

	/* 另设置 */
	plpgsql_IdentifierLookup = IDENTIFIER_LOOKUP_NORMAL;
	plpgsql_yytoken = 0;

	num_pushbacks = 0;

	location_lineno_init();
}

/*
 * 解析完成后调用，以清理 plpgsql_scanner_init() 的后续
 */
void
plpgsql_scanner_finish(void)
{
	/* 释放存储 */
	scanner_finish(yyscanner);
	/* 避免留下任何悬空指针 */
	yyscanner = NULL;
	scanorig = NULL;
}
