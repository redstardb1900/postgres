/*-------------------------------------------------------------------------
 *
 * scanner.h
 *		核心扫描器 API（flex 机器）
 *
 * 核心扫描器亦被 PL/pgSQL 使用，故提供公共 API。
 * 但后端其余部分仅应使用
 * parser.h 提供的高层 API。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/scanner.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCANNER_H
#define SCANNER_H

#include "common/keywords.h"

/*
 * 扫描器经此联合类型返回所扫记号的额外数据。
 * 注意这是建于扫描器之上的 bison
 * YYSTYPE 字段子集。
 */
typedef union core_YYSTYPE
{
	int			ival;			/*  整数字面量  */
	char	   *str;			/*  标识符与非整数字面量  */
	const char *keyword;		/*  关键字的规范拼写  */
} core_YYSTYPE;

/*
 * 我们以相对源串起始的字节偏移跟踪记号位置，
 * 而非 bison 默认的列号/行号表示。
 * 并且为减小开销，每个构造只跟踪一
 * 处位置（通常是首个记号位置），而非
 * bison 默认的起止位置。因此
 * 令 YYLTYPE 为 int 即可。
 */
#define YYLTYPE  int

/*
 * Another important component of the scanner's API is the token code numbers.
 * However, those are not defined in this file, because bison insists on
 * defining them for itself.  The token codes used by the core scanner are
 * the ASCII characters plus these:
 *	%token <str>	IDENT UIDENT FCONST SCONST USCONST BCONST XCONST Op
 *	%token <ival>	ICONST PARAM
 *	%token			TYPECAST DOT_DOT COLON_EQUALS EQUALS_GREATER
 *	%token			LESS_EQUALS GREATER_EQUALS NOT_EQUALS
 * The above token definitions *must* be the first ones declared in any
 * bison parser built atop this scanner, so that they will have consistent
 * numbers assigned to them (specifically, IDENT = 258 and so on).
 */

/*
 * flex 扫描器允许我们传递的 YY_EXTRA 数据。
 * 核心扫描器所需私有状态放此。注意实际
 * yy_extra 结构可能更大并以本结构为首部，从而
 * 允许调用方解析器在 YY_EXTRA 中保留自有字段。
 */
typedef struct core_yy_extra_type
{
	/*
	 * 扫描器实际扫描的字符串。保留它主要是为了
	 * 能廉价计算当前记号（yytext）的偏移。
	 */
	char	   *scanbuf;
	Size		scanbuflen;

	/*
	 * 所用关键字表，及关联的文法记号码。
	 */
	const ScanKeywordList *keywordlist;
	const uint16 *keyword_tokens;

	/*
	 * 所用扫描器设置。由 scanner_init() 从相应
	 * GUC 变量初始化。调用方可在
	 * scanner_init() 之后修改，若不想让扫描器行为跟随
	 * 当前 GUC 设置。
	 */
	int			backslash_quote;
	bool		escape_string_warning;
	bool		standard_conforming_strings;

	/*
	 * literalbuf 用于多条规则才能解析单个字面量时
	 * 累积字面值。调用 startlit() 清空缓冲，
	 * addlit() 追加文本。注意：literalbuf 中的串未必
	 * 以空字节结尾，但总有空间在
	 * literallen 偏移处补尾空。仅在需要时才存空字节。
	 */
	char	   *literalbuf;		/*  palloc'd 可扩展缓冲  */
	int			literallen;		/*  当前实际串长  */
	int			literalalloc;	/*  当前已分配缓冲大小  */

	/*
	 * 杂项扫描器状态。
	 */
	int			state_before_str_stop;	/*  引号前起始条件  */
	int			xcdepth;		/*  斜杠星注释的嵌套深度  */
	char	   *dolqstart;		/*  当前 $foo$ 引号起始串  */
	YYLTYPE		save_yylloc;	/*  PUSH_YYLLOC() 的单元素栈  */

	/*  Unicode 转义用 UTF16 代理对的前半  */
	int32		utf16_first_part;

	/*  字面量词法警告的状态变量  */
	bool		warn_on_first_escape;
	bool		saw_non_ascii;
} core_yy_extra_type;

/*
 * yyscanner 类型在 scan.l 外不透明。
 */
typedef void *core_yyscan_t;

/*  scanner_errposition_callback 函数支持  */
typedef struct ScannerCallbackState
{
	core_yyscan_t yyscanner;
	int			location;
	ErrorContextCallback errcallback;
} ScannerCallbackState;


/*  自 parser/scan.l 导出的常量数据  */
extern PGDLLIMPORT const uint16 ScanKeywordTokens[];

/*  scan.l 入口点  */
extern core_yyscan_t scanner_init(const char *str,
								  core_yy_extra_type *yyext,
								  const ScanKeywordList *keywordlist,
								  const uint16 *keyword_tokens);
extern void scanner_finish(core_yyscan_t yyscanner);
extern int	core_yylex(core_YYSTYPE *yylval_param, YYLTYPE *yylloc_param,
					   core_yyscan_t yyscanner);
extern int	scanner_errposition(int location, core_yyscan_t yyscanner);
extern void setup_scanner_errposition_callback(ScannerCallbackState *scbstate,
											   core_yyscan_t yyscanner,
											   int location);
extern void cancel_scanner_errposition_callback(ScannerCallbackState *scbstate);
extern void scanner_yyerror(const char *message, core_yyscan_t yyscanner) pg_attribute_noreturn();

#endif							/* SCANNER_H */
