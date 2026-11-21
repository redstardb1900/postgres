/*-------------------------------------------------------------------------
 *
 * psqlscan_int.h
 *	  词法扫描器内部声明
 *
 * 本文件声明 psqlscan.l 所用、并与兼容词法器
 * （如 psqlscanslash.l）共享的 PsqlScanStateData。
 *
 * 难点之一是须在非 ASCII 安全的多字节编码下
 * 工作。"安全"编码指多字节字符的每个字节高位
 * 均置位（>=0x80）。因词法规则对高位置位字节
 * 一视同仁，不必关心其是否属序列一部分。
 * "不安全"编码中仍期望多字节序列首字节 >=0x80，
 * 但后续字节可能不是。若原样扫描，规则易把
 * 此类字节误匹配为普通 ASCII。做法是在交给
 * flex 的数据中把非首字节换成 0xFF，规则会
 * 原样放过 FF。psqlscan_emit() 再对照原串把
 * FF 换回对应原字节。
 * 略
 * 略
 * 略
 *
 * 另一有趣处是用物理分离的 flex 词法器（即写在
 * 不同 .l 文件中）扫描同一输入的不同部分。可行
 * 是因 flex 词法器持久状态中依赖解析规则表的
 * 只有起始状态号，易管理——换词法器时常只需
 * 设为 INITIAL。但这要求用可重入词法器，使相关
 * 状态都在附于 PsqlScanState 的 yyscan_t 中；
 * 若用带独立静态状态的词法器，很快会悬空缓冲
 * 指针。另若各词法器非同一 flex 版本或选项，
 * 也不太可能工作良好。
 * 略
 * 略
 * 略
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/psqlscan_int.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLSCAN_INT_H
#define PSQLSCAN_INT_H

#include "fe_utils/psqlscan.h"

/*
 * 仅为使本文件可单独编译做头有效性检查；实际
 * 使用时应始终从 flex 文件体包含，彼处这些
 * 符号业已定义。
 */
#ifndef YY_TYPEDEF_YY_BUFFER_STATE
#define YY_TYPEDEF_YY_BUFFER_STATE
typedef struct yy_buffer_state *YY_BUFFER_STATE;
#endif
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

/*
 * 用 flex 缓冲栈处理 psql 变量替换。每层栈缓冲
 * 含一个 psql 变量中尚未读完的文本。栈全部弹出
 * 后，继续从 scanbufhandle 标识的外层缓冲读取。
 * 略
 */
typedef struct StackElem
{
	YY_BUFFER_STATE buf;		/*  flex 输入控制结构  */
	char	   *bufstring;		/*  flex 实际正在扫描的数据  */
	char	   *origstring;		/*  原数据副本（若需要）  */
	char	   *varname;		/*  提供数据的变量名，或 NULL  */
	struct StackElem *next;
} StackElem;

/*
 * 词法器全部工作状态须存在 PsqlScanStateData 中，
 * 以便多次打开词法操作（嵌套 include 需要）。
 * 词法器本身不可递归，但必须可重入。
 * 略
 */
typedef struct PsqlScanStateData
{
	yyscan_t	scanner;		/*  本 PsqlScanState 的 Flex 状态  */

	PQExpBuffer output_buf;		/*  当前输出缓冲  */

	StackElem  *buffer_stack;	/*  变量展开缓冲栈  */

	/*
	 * 这些变量始终指外层缓冲，从不指任何栈上的
	 * 变量展开缓冲。
	 */
	YY_BUFFER_STATE scanbufhandle;
	char	   *scanbuf;		/*  外层输入缓冲起点  */
	const char *scanline;		/*  外层当前输入行  */

	/*  safe_encoding、curline、refline 供 emit() 替换 FF  */
	int			encoding;		/*  当前所用编码  */
	bool		safe_encoding;	/* 略 */
	bool		std_strings;	/* 略 */
	const char *curline;		/* 略 */
	const char *refline;		/*  当前缓冲的原数据  */

	/*
	 * 这些状态跨连续输入行存活，直至 psql_scan_reset
	 * 显式复位。yylex() 入口采用 start_state，退出时
	 * 用结束状态更新之。
	 */
	int			start_state;	/*  yylex 的起始/结束状态  */
	int			state_before_str_stop;	/* 略 */
	int			paren_depth;	/*  括号嵌套深度  */
	int			xcdepth;		/*  斜杠星注释嵌套深度  */
	char	   *dolqstart;		/*  当前 $foo$ 引号起始串  */

	/*
	 * 跟踪函数定义中 BEGIN...END 块边界，以免
	 * 分号过早发送查询。
	 */
	int			identifier_count;	/*  自语句起的标识符  */
	char		identifiers[4]; /*  记录前几个标识符  */
	int			begin_depth;	/*  begin/end 对深度  */

	/*
	 * 使用词法器的程序提供的回调，外加 void*
	 * 回调透传参数。
	 */
	const PsqlScanCallbacks *callbacks;
	void	   *cb_passthrough;
} PsqlScanStateData;


/*
 * psqlscan.l 导出、但仅供兼容词法器内部使用的
 * 函数。
 */
extern void psqlscan_push_new_buffer(PsqlScanState state,
									 const char *newstr, const char *varname);
extern void psqlscan_pop_buffer_stack(PsqlScanState state);
extern void psqlscan_select_top_buffer(PsqlScanState state);
extern bool psqlscan_var_is_current_source(PsqlScanState state,
										   const char *varname);
extern YY_BUFFER_STATE psqlscan_prepare_buffer(PsqlScanState state,
											   const char *txt, int len,
											   char **txtcopy);
extern void psqlscan_emit(PsqlScanState state, const char *txt, int len);
extern char *psqlscan_extract_substring(PsqlScanState state,
										const char *txt, int len);
extern void psqlscan_escape_variable(PsqlScanState state,
									 const char *txt, int len,
									 PsqlScanQuoteType quote);
extern void psqlscan_test_variable(PsqlScanState state,
								   const char *txt, int len);

#endif							/* PSQLSCAN_INT_H */
