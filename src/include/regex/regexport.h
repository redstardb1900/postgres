/*-------------------------------------------------------------------------
 *
 * regexport.h
 *	  导出正则之 NFA 信息的声明(非确定
 *	  有限自动机)
 *
 * 此处声明的函数提供访问器，用以提取 NFA 状态
 * 图与已成功编译正则的色字符集。
 *
 * NFA 含一个或多个状态，编号 0..N-1。有一初始
 * 状态，以及一终结状态——到达终结状态表示
 * 成功匹配输入串。除终结状态外，每个状态
 * 有若干出弧通向后继状态，每条弧标以
 * 一种色，代表一个或多个具体字符码。
 * （一状态之出弧的色不必互异，因这是 
 * NFA 而非 DFA。）	另有"伪色"表示行/串之
 * 始与末。色编号为 0..C-1，但注意
 * 色 0 是"WHITE"（一切未用字符），一般可忽略。
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/include/regex/regexport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _REGEXPORT_H_
#define _REGEXPORT_H_

#include "regex/regex.h"

/* 这些宏须与 regguts.h 中对应者一致： */
#define COLOR_WHITE		0		/* 未出现于正则中的字符之色 */
#define COLOR_RAINBOW	(-2)	/* 表示除伪色外的全部色 */

/* 关于正则 NFA 之一条弧的信息 */
typedef struct
{
	int			co;				/* 弧的标签（字符集色） */
	int			to;				/* 下一状态号 */
} regex_arc_t;


/* 搜集 NFA 状态与弧之信息的函数 */
extern int	pg_reg_getnumstates(const regex_t *regex);
extern int	pg_reg_getinitialstate(const regex_t *regex);
extern int	pg_reg_getfinalstate(const regex_t *regex);
extern int	pg_reg_getnumoutarcs(const regex_t *regex, int st);
extern void pg_reg_getoutarcs(const regex_t *regex, int st,
							  regex_arc_t *arcs, int arcs_len);

/* 搜集色之信息的函数 */
extern int	pg_reg_getnumcolors(const regex_t *regex);
extern int	pg_reg_colorisbegin(const regex_t *regex, int co);
extern int	pg_reg_colorisend(const regex_t *regex, int co);
extern int	pg_reg_getnumcharacters(const regex_t *regex, int co);
extern void pg_reg_getcharacters(const regex_t *regex, int co,
								 pg_wchar *chars, int chars_len);

#endif							/* _REGEXPORT_H_ */
