/*-------------------------------------------------------------------------
 *
 * 前端代码的查询结果打印支持
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/print.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRINT_H
#define PRINT_H

#include <signal.h>

#include "libpq-fe.h"


/*  放这里不算特别合适……  */
#ifndef __CYGWIN__
#define DEFAULT_PAGER "more"
#else
#define DEFAULT_PAGER "less"
#endif

enum printFormat
{
	PRINT_NOTHING = 0,			/*  确保有人初始化此项  */
	PRINT_ALIGNED,
	PRINT_ASCIIDOC,
	PRINT_CSV,
	PRINT_HTML,
	PRINT_LATEX,
	PRINT_LATEX_LONGTABLE,
	PRINT_TROFF_MS,
	PRINT_UNALIGNED,
	PRINT_WRAPPED
	/* 略 */
};

typedef struct printTextLineFormat
{
	/*  各上下文所用的线绘字符  */
	const char *hrule;			/*  水平线字符  */
	const char *leftvrule;		/*  左竖线（+水平）  */
	const char *midvrule;		/*  列间竖线（+水平）  */
	const char *rightvrule;		/*  右竖线（+水平）  */
} printTextLineFormat;

typedef enum printTextRule
{
	/*  选择线绘字符的附加上下文  */
	PRINT_RULE_TOP,				/*  顶水平线  */
	PRINT_RULE_MIDDLE,			/*  数据内水平线  */
	PRINT_RULE_BOTTOM,			/*  底水平线  */
	PRINT_RULE_DATA				/* 略 */
} printTextRule;

typedef enum printTextLineWrap
{
	/*  换行条件  */
	PRINT_LINE_WRAP_NONE,		/*  不换行  */
	PRINT_LINE_WRAP_WRAP,		/*  因行过长而回绕  */
	PRINT_LINE_WRAP_NEWLINE		/* 略 */
} printTextLineWrap;

typedef enum printXheaderWidthType
{
	/*  展开表头行宽变体  */
	PRINT_XHEADER_FULL,			/*  不截断表头行（此为
								 * 默认 */
	PRINT_XHEADER_COLUMN,		/*  仅在第一列上方打印
								 * 首列 */
	PRINT_XHEADER_PAGE,			/*  表头行不得长于
								 * 终端宽度 */
	PRINT_XHEADER_EXACT_WIDTH,	/*  显式指定宽度  */
} printXheaderWidthType;

typedef struct printTextFormat
{
	/*  完整线样式  */
	const char *name;			/*  供显示用  */
	printTextLineFormat lrule[4];	/* 略 */
	const char *midvrule_nl;	/*  换行后续的竖线  */
	const char *midvrule_wrap;	/*  回绕数据的竖线  */
	const char *midvrule_blank; /*  空白数据的竖线  */
	const char *header_nl_left; /*  换行后的左标记  */
	const char *header_nl_right;	/*  换行的右标记  */
	const char *nl_left;		/*  换行后的左标记  */
	const char *nl_right;		/*  换行的右标记  */
	const char *wrap_left;		/* 略 */
	const char *wrap_right;		/*  回绕数据的右标记  */
	bool		wrap_right_border;	/* 略
									 * 右边框？ */
} printTextFormat;

typedef enum unicode_linestyle
{
	UNICODE_LINESTYLE_SINGLE = 0,
	UNICODE_LINESTYLE_DOUBLE
} unicode_linestyle;

struct separator
{
	char	   *separator;
	bool		separator_zero;
};

typedef struct printTableOpt
{
	enum printFormat format;	/* 略 */
	unsigned short int expanded;	/*  展开/垂直输出（若输出格式
									 * 支持）；0=否,1=是,2=自动 */
	printXheaderWidthType expanded_header_width_type;	/* 略
														 * 行宽类型 */
	int			expanded_header_exact_width;	/*  展开模式下表头
												 * 行显式宽 */
	unsigned short int border;	/*  打印表边框。0=无,
								 * 1=分隔线,2=完整 */
	unsigned short int pager;	/* 略
								 * 为 tty） */
	int			pager_min_lines;	/*  除非至少有这么多行，
									 * 至少这么多行 */
	bool		tuples_only;	/*  不输出表头、行数等  */
	bool		start_table;	/*  打印起始装饰，如 <table>  */
	bool		stop_table;		/* 略 */
	bool		default_footer; /* 略 */
	unsigned long prior_records;	/*  记录计数器起始偏移  */
	const printTextFormat *line_style;	/* 略 */
	struct separator fieldSep;	/* 略 */
	struct separator recordSep; /* 略 */
	char		csvFieldSep[2]; /*  csv 格式的字段分隔符  */
	bool		numericLocale;	/*  区域感知的数值千分位与
								 * 小数标记 */
	char	   *tableAttr;		/*  HTML <table ...> 的属性  */
	int			encoding;		/*  字符编码  */
	int			env_columns;	/*  $COLUMNS，0 未设  */
	int			columns;		/*  回绕格式的目标宽度  */
	unicode_linestyle unicode_border_linestyle;
	unicode_linestyle unicode_column_linestyle;
	unicode_linestyle unicode_header_linestyle;
} printTableOpt;

/*
 * 表页脚实现为单链表。
 *
 * 这样初始化 printTableContent 时不必知道页脚
 * 个数，准备复杂页脚时很方便
 * （如 describeOneTableDetails）。
 */
typedef struct printTableFooter
{
	char	   *data;
	struct printTableFooter *next;
} printTableFooter;

/*
 * 表内容结构保存 printTable() 将显示的全部
 * 信息。
 */
typedef struct printTableContent
{
	const printTableOpt *opt;
	const char *title;			/* May be NULL */
	int			ncolumns;		/*  Init() 中指定  */
	int			nrows;			/*  Init() 中指定  */
	const char **headers;		/*  以空结尾的表头串数组  */
	const char **header;		/*  指向最后添加的表头  */
	const char **cells;			/*  以空结尾的单元格内容
								 * 串 */
	const char **cell;			/* 略 */
	long		cellsadded;		/* 略 */
	bool	   *cellmustfree;	/*  需 free() 的单元格为真  */
	printTableFooter *footers;	/*  指向第一个页脚  */
	printTableFooter *footer;	/*  指向最后添加的页脚  */
	char	   *aligns;			/*  对齐说明符数组；'l' 或 'r'，
								 * 每列一个 */
	char	   *align;			/*  指向最后添加的对齐  */
} printTableContent;

typedef struct printQueryOpt
{
	printTableOpt topt;			/*  上方选项  */
	char	   *nullPrint;		/*  如何打印空实体  */
	char	   *title;			/*  覆盖标题  */
	char	  **footers;		/*  覆盖页脚（默认 "(xx rows)"）  */
	bool		translate_header;	/*  对列表头做 gettext  */
	const bool *translate_columns;	/*  translate_columns[i-1] => 对列 i
									 * 列 i */
	int			n_translate_columns;	/*  translate_columns[] 长度  */
} printQueryOpt;


extern PGDLLIMPORT volatile sig_atomic_t cancel_pressed;

extern PGDLLIMPORT const printTextFormat pg_asciiformat;
extern PGDLLIMPORT const printTextFormat pg_asciiformat_old;
extern PGDLLIMPORT printTextFormat pg_utf8format;	/*  理想应为 const，
													 * 但是 */


extern void disable_sigpipe_trap(void);
extern void restore_sigpipe_trap(void);
extern void set_sigpipe_trap_state(bool ignore);

extern FILE *PageOutput(int lines, const printTableOpt *topt);
extern void ClosePager(FILE *pagerpipe);

extern void html_escaped_print(const char *in, FILE *fout);

extern void printTableInit(printTableContent *const content,
						   const printTableOpt *opt, const char *title,
						   const int ncolumns, const int nrows);
extern void printTableAddHeader(printTableContent *const content,
								char *header, const bool translate, const char align);
extern void printTableAddCell(printTableContent *const content,
							  char *cell, const bool translate, const bool mustfree);
extern void printTableAddFooter(printTableContent *const content,
								const char *footer);
extern void printTableSetFooter(printTableContent *const content,
								const char *footer);
extern void printTableCleanup(printTableContent *const content);
extern void printTable(const printTableContent *cont,
					   FILE *fout, bool is_pager, FILE *flog);
extern void printQuery(const PGresult *result, const printQueryOpt *opt,
					   FILE *fout, bool is_pager, FILE *flog);

extern char column_type_alignment(Oid);

extern void setDecimalLocale(void);
extern const printTextFormat *get_line_style(const printTableOpt *opt);
extern void refresh_utf8format(const printTableOpt *opt);

#endif							/* PRINT_H */
