/*-------------------------------------------------------------------------
 *
 * parser.c
 *		PostgreSQL 语法的主入口/驱动          
 *
 * 注意：语法不允许执行任何表访问操作，          
 * 因为即使事务已中止，也须能完成基本解析，      
 * 因此，语法所返回的数据结构是"原始"解析树，   
 * 尚需由 analyze.c 及相关文件加以分析。                
 *                             
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "gramparse.h"
#include "parser/parser.h"
#include "parser/scansup.h"

static bool check_uescapechar(unsigned char escape);
static char *str_udeescape(const char *str, char escape,
						   int position, core_yyscan_t yyscanner);


/*
 * raw_parser
 *		对字符串形式的查询进行词法和语法分析。         
 *
 * 返回原始（未经分析的）解析树的列表。列表内容   
 * 符合指定 RawParseMode 所要求的形式。           
 */
List *
raw_parser(const char *str, RawParseMode mode)
{
	core_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int			yyresult;

	/* 初始化 flex 扫描器    */
	yyscanner = scanner_init(str, &yyextra.core_yy_extra,
							 &ScanKeywords, ScanKeywordTokens);

	/* base_yylex() 只需我们初始化前瞻词元（如有）           */
	if (mode == RAW_PARSE_DEFAULT)
		yyextra.have_lookahead = false;
	else
	{
		/* 此数组按 RawParseMode 枚举索引     */
		static const int mode_token[] = {
			0,					/* RAW_PARSE_DEFAULT */
			MODE_TYPE_NAME,		/* RAW_PARSE_TYPE_NAME */
			MODE_PLPGSQL_EXPR,	/* RAW_PARSE_PLPGSQL_EXPR */
			MODE_PLPGSQL_ASSIGN1,	/* RAW_PARSE_PLPGSQL_ASSIGN1 */
			MODE_PLPGSQL_ASSIGN2,	/* RAW_PARSE_PLPGSQL_ASSIGN2 */
			MODE_PLPGSQL_ASSIGN3	/* RAW_PARSE_PLPGSQL_ASSIGN3 */
		};

		yyextra.have_lookahead = true;
		yyextra.lookahead_token = mode_token[mode];
		yyextra.lookahead_yylloc = 0;
		yyextra.lookahead_end = NULL;
	}

	/* 初始化 bison 解析器   */
	parser_init(&yyextra);

	/* 解析 */
	yyresult = base_yyparse(yyscanner);

	/* 清理（释放内存）  */
	scanner_finish(yyscanner);

	if (yyresult)				/* 出错*/
		return NIL;

	return yyextra.parsetree;
}


/*
 * 解析器与核心 lexer（scan.l 的 core_yylex）之间的过滤器。 
 *
 * 需要此过滤器，因为标准 SQL 语法有时需要            
 * 前瞻不止一个词元。我们将其归约为单词元前瞻，         
 * 做法是就地替换词元，以保持语法为 LALR(1)。              
 *
 * 使用过滤器，比设法直接在 scan.l 里识别多词词元  
 * 要简单得多，因为那样必须允许单词之间存在注释。  
 * 另外，尚不清楚如何在不重新引入扫描器回退的情况下
 * 做到这一点，而回退的性能代价将超过此过滤器层。
 *            
 *
 * 我们还用此过滤器把 UIDENT 与 USCONST 序列转换为      
 * 普通 IDENT 与 SCONST 词元。虽然也可以在主语法里          
 * 增加产生式来处理，但像这样做效率更高。               
 *
 * 此过滤器还提供了一处便利的位置，可在          
 * core_YYSTYPE 与 YYSTYPE 表示之间进行转换（两者实质  
 * 上是同一事物，只是记法不同）。          
 */
int
base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner)
{
	base_yy_extra_type *yyextra = pg_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	int			cur_token_length;
	YYLTYPE		cur_yylloc;

	/* 取下一个词元 --- 可能已有缓存   */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		lvalp->core_yystype = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		if (yyextra->lookahead_end)
			*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

	/*
	 * 若此词元无需前瞻，直接返回即可。若需要，           
	 * 先确定该词元的长度。（本可以用 strlen() 获取，但    
	 * 由于可选情形很少，硬编码似乎可行且更高效     
	 * --- 至少对定长情形确实如此）。                          
	 */
	switch (cur_token)
	{
		case FORMAT:
			cur_token_length = 6;
			break;
		case NOT:
			cur_token_length = 3;
			break;
		case NULLS_P:
			cur_token_length = 5;
			break;
		case WITH:
			cur_token_length = 4;
			break;
		case UIDENT:
		case USCONST:
			cur_token_length = strlen(yyextra->core_yy_extra.scanbuf + *llocp);
			break;
		case WITHOUT:
			cur_token_length = 7;
			break;
		default:
			return cur_token;
	}

	/*
	 * 确定当前词元 end+1 的位置。core_yylex() 在此临时存入   
	 * 一个 '\0'，下次再调用时会撤销。故需重新截断，    
	 * 以完全撤销前瞻调用，便于错误报告。                
	 */
	yyextra->lookahead_end = yyextra->core_yy_extra.scanbuf +
		*llocp + cur_token_length;
	Assert(*(yyextra->lookahead_end) == '\0');

	/*
	 * 调用前后保存并恢复 *llocp。看似可以省去此步，     
	 * 只需把 &lookahead_yylloc 传给 core_yylex() 即可，但这       
	 * 行不通，因为 flex 会在内部持有上次传入的指针，    
	 * 并将其用于报错。须让错误指向当前词元，            
	 * 而非下一个。                                      
	 */
	cur_yylloc = *llocp;

	/* 取下一个词元，把输出存入前瞻变量        */
	next_token = core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);
	yyextra->lookahead_token = next_token;
	yyextra->lookahead_yylloc = *llocp;

	*llocp = cur_yylloc;

	/* 现在重新截断当前词元                    */
	yyextra->lookahead_hold_char = *(yyextra->lookahead_end);
	*(yyextra->lookahead_end) = '\0';

	yyextra->have_lookahead = true;

	/* 根据前瞻词元，必要时替换 cur_token  */
	switch (cur_token)
	{
		case FORMAT:
			/* 若 FORMAT 后跟 JSON，则替换为 FORMAT_LA      */
			switch (next_token)
			{
				case JSON:
					cur_token = FORMAT_LA;
					break;
			}
			break;

		case NOT:
			/* 若 NOT 后跟 BETWEEN、IN 等，则替换为 NOT_LA      */
			switch (next_token)
			{
				case BETWEEN:
				case IN_P:
				case LIKE:
				case ILIKE:
				case SIMILAR:
					cur_token = NOT_LA;
					break;
			}
			break;

		case NULLS_P:
			/* 若 NULLS_P 后跟 FIRST 或 LAST，则替换为 NULLS_LA     */
			switch (next_token)
			{
				case FIRST_P:
				case LAST_P:
					cur_token = NULLS_LA;
					break;
			}
			break;

		case WITH:
			/* 若 WITH 后跟 TIME 或 ORDINALITY，则替换为 WITH_LA     */
			switch (next_token)
			{
				case TIME:
				case ORDINALITY:
					cur_token = WITH_LA;
					break;
			}
			break;

		case WITHOUT:
			/* 若 WITHOUT 后跟 TIME，则替换为 WITHOUT_LA      */
			switch (next_token)
			{
				case TIME:
					cur_token = WITHOUT_LA;
					break;
			}
			break;

		case UIDENT:
		case USCONST:
			/* 向前查看 UESCAPE   */
			if (next_token == UESCAPE)
			{
				/* 没错，故取第三词元，它应是 SCONST      */
				const char *escstr;

				/* 再次保存/恢复 *llocp    */
				cur_yylloc = *llocp;

				/* 解除当前词元截断，让错误指向第三词元   */
				*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;

				/* 取第三词元 */
				next_token = core_yylex(&(yyextra->lookahead_yylval),
										llocp, yyscanner);

				/* 若在此报错，将指向第三个词元           */
				if (next_token != SCONST)
					scanner_yyerror("UESCAPE must be followed by a simple string literal",
									yyscanner);

				escstr = yyextra->lookahead_yylval.str;
				if (strlen(escstr) != 1 || !check_uescapechar(escstr[0]))
					scanner_yyerror("invalid Unicode escape character",
									yyscanner);

				/* 现在恢复 *llocp；错误将指向第一个词元 */
				*llocp = cur_yylloc;

				/* 执行 Unicode 转换    */
				lvalp->core_yystype.str =
					str_udeescape(lvalp->core_yystype.str,
								  escstr[0],
								  *llocp,
								  yyscanner);

				/*
				 * 无须恢复 UESCAPE 的截断。我们要做的是清除  
				 * have_lookahead，从而用掉全部 3 个词元            
				 *                  
				 */
				yyextra->have_lookahead = false;
			}
			else
			{
				/* 无 UESCAPE，故用默认转义字符转换          */
				lvalp->core_yystype.str =
					str_udeescape(lvalp->core_yystype.str,
								  '\\',
								  *llocp,
								  yyscanner);
			}

			if (cur_token == UIDENT)
			{
				/* 它是标识符，故按需截断              */
				truncate_identifier(lvalp->core_yystype.str,
									strlen(lvalp->core_yystype.str),
									true);
				cur_token = IDENT;
			}
			else if (cur_token == USCONST)
			{
				cur_token = SCONST;
			}
			break;
	}

	return cur_token;
}

/* 转换十六进制数字为数值（调用者应已验证）  */
static unsigned int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	elog(ERROR, "invalid hexadecimal digit");
	return 0;					/* 不应到达*/
}

/* Unicode 码点可否接受？     */
static void
check_unicode_value(pg_wchar c)
{
	if (!is_valid_unicode_codepoint(c))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid Unicode escape value")));
}

/* 'escape' 作 Unicode 转义字符可否接受（UESCAPE 语法）？  */
static bool
check_uescapechar(unsigned char escape)
{
	if (isxdigit(escape)
		|| escape == '+'
		|| escape == '\''
		|| escape == '"'
		|| scanner_isspace(escape))
		return false;
	else
		return true;
}

/*
 * 处理 "str" 的 Unicode 转义，产出 palloc 的普通字符串 
 *
 * escape 为所用的转义字符    
 * position 为 U&'' 或 U&"" 词元的起始位置     
 * yyscanner 是报错所需的上下文信息            
 */
static char *
str_udeescape(const char *str, char escape,
			  int position, core_yyscan_t yyscanner)
{
	const char *in;
	char	   *new,
			   *out;
	size_t		new_len;
	pg_wchar	pair_first = 0;
	ScannerCallbackState scbstate;

	/*
	 * 粗略估计结果不会比输入长，但要为 Unicode              
	 * 转换预留足够空间。    
	 */
	new_len = strlen(str) + MAX_UNICODE_EQUIVALENT_STRING + 1;
	new = palloc(new_len);

	in = str;
	out = new;
	while (*in)
	{
		/* 必要时扩大字符串 */
		size_t		out_dist = out - new;

		if (out_dist > new_len - (MAX_UNICODE_EQUIVALENT_STRING + 1))
		{
			new_len *= 2;
			new = repalloc(new, new_len);
			out = new + out_dist;
		}

		if (in[0] == escape)
		{
			/*
			 * 处理此转义序列时报告的任何错误，都           
			 * 会有指向该转义的错误游标。     
			 */
			setup_scanner_errposition_callback(&scbstate, yyscanner,
											   in - str + position + 3);	/* U&" 3 字 */
			if (in[1] == escape)
			{
				if (pair_first)
					goto invalid_pair;
				*out++ = escape;
				in += 2;
			}
			else if (isxdigit((unsigned char) in[1]) &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[1]) << 12) +
					(hexval(in[2]) << 8) +
					(hexval(in[3]) << 4) +
					hexval(in[4]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 5;
			}
			else if (in[1] == '+' &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]) &&
					 isxdigit((unsigned char) in[5]) &&
					 isxdigit((unsigned char) in[6]) &&
					 isxdigit((unsigned char) in[7]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[2]) << 20) +
					(hexval(in[3]) << 16) +
					(hexval(in[4]) << 12) +
					(hexval(in[5]) << 8) +
					(hexval(in[6]) << 4) +
					hexval(in[7]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 8;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid Unicode escape"),
						 errhint("Unicode escapes must be \\XXXX or \\+XXXXXX.")));

			cancel_scanner_errposition_callback(&scbstate);
		}
		else
		{
			if (pair_first)
				goto invalid_pair;

			*out++ = *in++;
		}
	}

	/* 代理对未完成？      */
	if (pair_first)
		goto invalid_pair;

	*out = '\0';
	return new;

	/*
	 * 此处错误回调可能已激活，也可能未激活。      
	 * 调用 scanner_errposition 以确保出现错误游标；       
	 * 若回调已激活，此举重复但无害。        
	 */
invalid_pair:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid Unicode surrogate pair"),
			 scanner_errposition(in - str + position + 3,	/* U&" 3 字 */
								 yyscanner)));
	return NULL;				/* 使编译器安静  */
}
