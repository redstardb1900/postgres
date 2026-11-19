#ifndef _PG_REGEX_H_
#define _PG_REGEX_H_			/* 勿再含 */
/*
 * 正则表达式
 *
 * Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
 *
 * Development of this software was funded, in part, by Cray Research Inc.,
 * UUNET Communications Services Inc., Sun Microsystems Inc., and Scriptics
 * Corporation, none of whom are responsible for the results.  The author
 * thanks all of them.
 *
 * Redistribution and use in source and binary forms -- with or without
 * modification -- are permitted for any purpose, provided that
 * redistributions in source form retain this entire copyright notice and
 * indicate the origin and nature of any modifications.
 *
 * I'd appreciate being given credit for this package in the documentation
 * of software which uses it, but that is not a requirement.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * HENRY SPENCER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * src/include/regex/regex.h
 */

/*
 * 这是 POSIX regex_t 的一种实现，故会与
 * 系统提供的 <regex.h> 头冲突。该头可能已被无意
 * 包含，故我们现于所有系统上强制先包含它，以表明
 * 我们能应对，且完全替换系统 regex 接口。
 *
 * 注意我们避免用 _REGEX_H_ 作包含守卫，因那会搞乱
 * 含 macOS 在内、用同一包含守卫的 BSD 族系统上的
 * 事。
 */
#ifndef _WIN32
#include <regex.h>
#endif

/* 避免因系统头文件导致重定义错误。 */
#undef REG_UBACKREF
#undef REG_ULOOKAROUND
#undef REG_UBOUNDS
#undef REG_UBRACES
#undef REG_UBSALNUM
#undef REG_UPBOTCH
#undef REG_UBBS
#undef REG_UNONPOSIX
#undef REG_UUNSPEC
#undef REG_UUNPORT
#undef REG_ULOCALE
#undef REG_UEMPTYMATCH
#undef REG_UIMPOSSIBLE
#undef REG_USHORTEST
#undef REG_BASIC
#undef REG_EXTENDED
#undef REG_ADVF
#undef REG_ADVANCED
#undef REG_QUOTE
#undef REG_NOSPEC
#undef REG_ICASE
#undef REG_NOSUB
#undef REG_EXPANDED
#undef REG_NLSTOP
#undef REG_NLANCH
#undef REG_NEWLINE
#undef REG_PEND
#undef REG_EXPECT
#undef REG_BOSONLY
#undef REG_DUMP
#undef REG_FAKE
#undef REG_PROGRESS
#undef REG_NOTBOL
#undef REG_NOTEOL
#undef REG_STARTEND
#undef REG_FTRACE
#undef REG_MTRACE
#undef REG_SMALL
#undef REG_OKAY
#undef REG_NOMATCH
#undef REG_BADPAT
#undef REG_ECOLLATE
#undef REG_ECTYPE
#undef REG_EESCAPE
#undef REG_ESUBREG
#undef REG_EBRACK
#undef REG_EPAREN
#undef REG_EBRACE
#undef REG_BADBR
#undef REG_ERANGE
#undef REG_ESPACE
#undef REG_BADRPT
#undef REG_ASSERT
#undef REG_INVARG
#undef REG_MIXED
#undef REG_BADOPT
#undef REG_ETOOBIG
#undef REG_ECOLORS
#undef REG_ATOI
#undef REG_ITOA
#undef REG_PREFIX
#undef REG_EXACT

/*
 * 需要时在此加入自有定义。
 */
#include "mb/pg_wchar.h"

/*
 * 接口类型等。
 */

/*
 * regoff_t 须大到足以容纳 off_t 或 ssize_t，
 * 且必须有符号；仅猜测 long 合适。
 */
typedef long pg_regoff_t;

/*
 * 其他接口类型
 */

/* 大家伙：已编译 RE（或者说其前端） */
typedef struct
{
	int			re_magic;		/* 幻数 */
	size_t		re_nsub;		/* 子表达式个数 */
	long		re_info;		/* 下列标志的位掩码： */
#define  REG_UBACKREF		000001	/* 含反向引用(\n) */
#define  REG_ULOOKAROUND	000002	/* 含前瞻/后顾约束 */
#define  REG_UBOUNDS		000004	/* 含有界量词({m,n}) */
#define  REG_UBRACES		000010	/* 含非量词起始的{ */
#define  REG_UBSALNUM		000020	/* 非 ARE 中有反斜杠字母数字 */
#define  REG_UPBOTCH		000040	/* ERE 有未匹配右括号(规范合法,
									 * 但那是个失误)*/
#define  REG_UBBS			000100	/* 方括号表达式内有反斜杠 */
#define  REG_UNONPOSIX		000200	/* 含任何扩展 POSIX 的构造 */
#define  REG_UUNSPEC		000400	/* 含 POSIX 禁止的情形，如
									 * 空分支*/
#define  REG_UUNPORT		001000	/* 有数值字符码依赖 */
#define  REG_ULOCALE		002000	/* 有区域设置依赖 */
#define  REG_UEMPTYMATCH	004000	/* 可匹配零长串 */
#define  REG_UIMPOSSIBLE	010000	/* 可证无法匹配任何内容 */
#define  REG_USHORTEST		020000	/* 含非贪婪量词 */
	int			re_csize;		/* sizeof(character) */
	char	   *re_endp;		/* 向后兼容拼凑 */
	Oid			re_collation;	/* 定义 LC_CTYPE 行为的排序规则 */
	/* 其余为指向隐藏内部的不透明指针 */
	char	   *re_guts;		/* `char *' 比 `void *' 更可移植 */
	char	   *re_fns;
} pg_regex_t;

/* 结果报告（日后或增字段） */
typedef struct
{
	pg_regoff_t rm_so;			/* 子串起点 */
	pg_regoff_t rm_eo;			/* 子串终点 */
} pg_regmatch_t;

/* 补充控制与报告 */
typedef struct
{
	pg_regmatch_t rm_extend;	/* 见 REG_EXPECT */
} rm_detail_t;



/*
 * 正则编译标志
 */
#define REG_BASIC	000000		/* BREs(便利) */
#define REG_EXTENDED	000001	/* EREs */
#define REG_ADVF	000002		/* ERE 中的高级特性 */
#define REG_ADVANCED	000003	/* ARE（亦为 ERE） */
#define REG_QUOTE	000004		/* 无特殊字符，皆无 */
#define REG_NOSPEC	REG_QUOTE	/* 历史同义名 */
#define REG_ICASE	000010		/* 忽略 case */
#define REG_NOSUB	000020		/* 调用者不需子表达式匹配 data */
#define REG_EXPANDED	000040	/* 展开格式，空白与注释 */
#define REG_NLSTOP	000100		/* \n 不匹配 . 或 [^ ] */
#define REG_NLANCH	000200		/* ^在\n后匹配,$在前 */
#define REG_NEWLINE 000300		/* 换行是行终止符 */
#define REG_PEND	000400		/* 呃——向后兼容黑招 */
#define REG_EXPECT	001000		/* 报告部分/受限匹配的细节 */
#define REG_BOSONLY 002000		/* 仅 BOS 匹配的临时拼凑 */
#define REG_DUMP	004000		/* 不关你的事 :-) */
#define REG_FAKE	010000		/* 不关你的事 :-) */
#define REG_PROGRESS	020000	/* 不关你的事 :-) */



/*
 * 正则执行标志
 */
#define REG_NOTBOL	0001		/* BOS 非 BOL */
#define REG_NOTEOL	0002		/* EOS 非 EOL */
#define REG_STARTEND	0004	/* 向后兼容拼凑 */
#define REG_FTRACE	0010		/* 不关你的事 */
#define REG_MTRACE	0020		/* 不关你的事 */
#define REG_SMALL	0040		/* 不关你的事 */


/*
 * 错误报告
 * 修改错误码列表须当心——regerror() 所用表
 * 由本文件自动生成！
 */
#define REG_OKAY	 0			/* 未检测到错误 */
#define REG_NOMATCH  1			/* 匹配失败 */
#define REG_BADPAT	 2			/* 无效正则 */
#define REG_ECOLLATE	 3		/* 无效排序元素 */
#define REG_ECTYPE	 4			/* 无效字符类 */
#define REG_EESCAPE  5			/* 无效转义 \ 序列 */
#define REG_ESUBREG  6			/* 无效反向引用号 */
#define REG_EBRACK	 7			/* 方括号[]不平衡 */
#define REG_EPAREN	 8			/* 圆括号()不平衡 */
#define REG_EBRACE	 9			/* 花括号{}不平衡 */
#define REG_BADBR	10			/* 无效重复计数 */
#define REG_ERANGE	11			/* 无效字符范围 */
#define REG_ESPACE	12			/* 内存不足 */
#define REG_BADRPT	13			/* 量词操作数无效 */
#define REG_ASSERT	15			/* 无法发生--你发现了 bug */
#define REG_INVARG	16			/* 传给 regex 函数的参数无效 */
#define REG_MIXED	17			/* 正则与串的字符宽度不同 */
#define REG_BADOPT	18			/* 无效嵌入选项 */
#define REG_ETOOBIG 19			/* 正则表达式过于复杂 */
#define REG_ECOLORS 20			/* 色过多 */
/* 调试与测试用的两个特殊码 */
#define REG_ATOI	101			/* 错误码名转数字 */
#define REG_ITOA	102			/* 错误码数字转名 */
/* pg_regprefix 的非错误结果码 */
#define REG_PREFIX	(-1)		/* 识别出公共前缀 */
#define REG_EXACT	(-2)		/* 识别出精确匹配 */


/* 把标准类型名改向我们的类型名。 */
#define regoff_t pg_regoff_t
#define regex_t pg_regex_t
#define regmatch_t pg_regmatch_t


/*
 * 导出函数的原型
 */

/* regcomp.c */
extern int	pg_regcomp(regex_t *re, const pg_wchar *string, size_t len,
					   int flags, Oid collation);
extern int	pg_regexec(regex_t *re, const pg_wchar *string, size_t len,
					   size_t search_start, rm_detail_t *details,
					   size_t nmatch, regmatch_t pmatch[], int flags);
extern int	pg_regprefix(regex_t *re, pg_wchar **string, size_t *slength);
extern void pg_regfree(regex_t *re);
extern size_t pg_regerror(int errcode, const regex_t *preg, char *errbuf,
						  size_t errbuf_size);

/* regexp.c */
extern regex_t *RE_compile_and_cache(text *text_re, int cflags, Oid collation);
extern bool RE_compile_and_execute(text *text_re, char *dat, int dat_len,
								   int cflags, Oid collation,
								   int nmatch, regmatch_t *pmatch);

#endif							/* _PG_REGEX_H_ */
