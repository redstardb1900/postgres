/*
 * reg 包的内部接口定义等
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
 * src/include/regex/regguts.h
 */



/*
 * 环境定制。应当不必（我希望）去
 * 改你正在读的本文件 -- regcustom.h 应能全部处理，
 * 只要此处与他处留心。
 */
#include "regcustom.h"



/*
 * regcustom.h 可能覆盖之物。
 */

/* 断言 */
#ifndef assert
#ifndef REG_DEBUG
#define  NDEBUG					/* 无断言 */
#endif
#include <assert.h>
#endif

/* voids */
#ifndef DISCARD
#define DISCARD void			/* 用于丢弃值 */
#endif
#ifndef VS
#define VS(x)	((void *)(x))	/* 把某物转为通用指针 */
#endif

/* 函数指针声明符 */
#ifndef FUNCPTR
#define FUNCPTR(name, args) (*(name)) args
#endif

/* 内存分配 */
#ifndef MALLOC
#define MALLOC(n)	malloc(n)
#endif
#ifndef REALLOC
#define REALLOC(p, n)	realloc(VS(p), n)
#endif
#ifndef FREE
#define FREE(p)		free(VS(p))
#endif

/* 中断 */
#ifndef INTERRUPT
#define INTERRUPT(re)
#endif

/* 要 char 的位宽，以及有界量词中的最大值 */
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX	255 /* 通常来自 <limits.h> */
#endif



/*
 * misc
 */

#define NOTREACHED	0

#define DUPMAX	_POSIX2_RE_DUP_MAX
#define DUPINF	(DUPMAX+1)

#define REMAGIC 0xfed7			/* 主结构的魔数 */

/* 环视约束的类型码 */
#define LATYPE_AHEAD_POS	03	/* 正前瞻 */
#define LATYPE_AHEAD_NEG	02	/* 负前瞻 */
#define LATYPE_BEHIND_POS	01	/* 正后顾 */
#define LATYPE_BEHIND_NEG	00	/* 负后顾 */
#define LATYPE_IS_POS(la)	((la) & 01)
#define LATYPE_IS_AHEAD(la) ((la) & 02)


/*
 * 调试设施
 */
#ifdef REG_DEBUG
/* FDEBUG 做有限状态跟踪 */
#define FDEBUG(arglist) { if (v->eflags&REG_FTRACE) printf arglist; }
/* MDEBUG 做更高层跟踪 */
#define MDEBUG(arglist) { if (v->eflags&REG_MTRACE) printf arglist; }
#else
#define FDEBUG(arglist) {}
#define MDEBUG(arglist) {}
#endif



/*
 * 位图操作
 */
#define UBITS	(CHAR_BIT * sizeof(unsigned))
#define BSET(uv, sn)	((uv)[(sn)/UBITS] |= (unsigned)1 << ((sn)%UBITS))
#define ISBSET(uv, sn)	((uv)[(sn)/UBITS] & ((unsigned)1 << ((sn)%UBITS)))


/*
 * 已知字符类
 */
enum char_classes
{
	CC_ALNUM, CC_ALPHA, CC_ASCII, CC_BLANK, CC_CNTRL, CC_DIGIT, CC_GRAPH,
	CC_LOWER, CC_PRINT, CC_PUNCT, CC_SPACE, CC_UPPER, CC_XDIGIT, CC_WORD
};

#define NUM_CCLASSES 14


/*
 * 尽快将 chr 映射入等价类 -- "色" --
 * 其数量远更易管理。
 *
 * 为进一步减少 NFA 与 DFA 中的弧数，我们还有一
 * 种可赋给弧的特殊彩虹 "色"。这并非真
 * 正的色，因其在色图中无条目。
 */
typedef short color;			/* 字符之色 */

#define MAX_COLOR	32767		/* 最大色(须装入 'color' 类型) */
#define COLORLESS	(-1)		/* 不可能的色 */
#define RAINBOW		(-2)		/* 表示除伪色外的全部色 */
#define WHITE		0			/* 默认色，其余色之父 */
/* 注意：代码多处知道 WHITE 为零 */


/*
 * 编译期色图机制的每色数据结构
 *
 * 若 "sub" 不是 NOSUB，则它是该色当前
 * 子色编号，即我们正在把此色（字符
 * 等价类）拆成两色。见 src/backend/regex/README 对
 * 子色的讨论。
 *
 * 当前未用的色置 FREECOL 位，并用其 "sub"
 * 字段链入空闲表，但仅当其色号
 * 小于色图.max。超出 "max" 的数组项只是垃圾。
 */
struct colordesc
{
	int			nschrs;			/* 此色的简单字符数 */
	int			nuchrs;			/* 此色的高位映射项数 */
	color		sub;			/* 开放子色(若有);或空闲链指针 */
#define  NOSUB	 COLORLESS		/* 无开放子色时 "sub" 的值 */
	struct arc *arcs;			/* 此色全部弧之链 */
	chr			firstchr;		/* 首次赋给此色的简单字符 */
	int			flags;			/* 下列标志的位掩码： */
#define  FREECOL 01				/* 当前空闲 */
#define  PSEUDO  02				/* 伪色，无真实字符 */
#define  COLMARK 04				/* 若干函数中用的临时标记 */
};

#define  UNUSEDCOLOR(cd) ((cd)->flags & FREECOL)

/*
 * 色图本身
 *
 * 本结构既含仅编译期用的数据，也含 chr 到
 * 色的映射信息，编译期与运行期皆用。后者
 * 占去大部分空间，故不值得再拆出
 * 仅编译期部分。
 *
 * 理想情况下，映射数据只是按 chr 码索引的色
 * 数组；但对大字符集不现实。幸运的是，
 * 常用字符码较小，故对不大于 MAX_SIMPLE_CHR 的
 * chr 码可用简单数组，对更高的码则用更复杂
 * 的做法，而性能损失不大。所谓 "更复杂" 是一
 * 个二维色项数组，行下标对应个别 chr
 * 或正则中曾提及的 chr 范围（第零行
 * 表示其余全部 chr），列下标对应不同
 * 的区域设置相关字符类集合，如 "isalpha"。
 * 若我们不关心本正则的第 k 个字符类，则
 * classbits[k] 为零；否则它是若该字符属于该类
 * 时要 OR 进列下标的位。求高值 chr 的色时，
 * 先判明它落在哪个 colormaprange 以得
 * 行下标（皆不在则用第零行），再判明它属于
 * 哪些相关 cclass 以得列下标，然后索引
 * 二维 hicolormap 数组。
 *
 * colormapranges 须非空、不重叠，并按
 * chr 值递增顺序出现。
 */

typedef struct colormaprange
{
	chr			cmin;			/* 范围表示含端点的 cmin..cmax */
	chr			cmax;
	int			rownum;			/* hicolormap 行下标（>= 1） */
} colormaprange;

struct colormap
{
	int			magic;
#define  CMMAGIC 0x876
	struct vars *v;				/* 供编译错误报告 */
	size_t		ncds;			/* colordescs 数组的已分配长度 */
	size_t		max;			/* 当前在用的最高色号 */
	color		free;			/* 空闲链起点（若非 0） */
	struct colordesc *cd;		/* colordescs 数组指针 */
#define  CDEND(cm)	 (&(cm)->cd[(cm)->max + 1])

	/* <=MAX_SIMPLE_CHR 的 chr 映射数据： */
	color	   *locolormap;		/* 按 chr 码索引的简单数组 */

	/* >MAX_SIMPLE_CHR 的 chr 映射数据： */
	int			classbits[NUM_CCLASSES];	/* 见上文注释 */
	int			numcmranges;	/* colormapranges 的个数 */
	colormaprange *cmranges;	/* 高 chr 的范围 */
	color	   *hicolormap;		/* 色项的二维数组 */
	int			maxarrayrows;	/* 已分配的数组行数 */
	int			hiarrayrows;	/* 在用的数组行数 */
	int			hiarraycols;	/* 数组列数（2^N） */

	/* 若至多 NINLINECDS 个，则存在此处以免 malloc */
#define  NINLINECDS  ((size_t) 10)
	struct colordesc cdspace[NINLINECDS];
};

/* 取 chr 的色；当心 c 实参被多次求值 */
#define GETCOLOR(cm, c) \
	((c) <= MAX_SIMPLE_CHR ? (cm)->locolormap[(c) - CHR_MIN] : pg_reg_getcolor(cm, c))


/*
 * regc_locale.c 中区域设置接口函数的接口定义。
 */

/*
 * 字符集的表示。chrs[] 表示个别
 * 码点，ranges[] 表示含端点的 min..max 范围。
 *
 * 若 cvec 表示区域设置相关字符类，如 [[:alpha:]]，
 * 则 chrs[] 与 ranges[] 数组仅含该类中
 * 不大于 MAX_SIMPLE_CHR（含）的成员。cclasscode 设为
 * regc_locale.c 对该类的码，而非普通 cvec 的 -1。
 *
 * 注意：由 newcvec() 取得并拟由 freecvec()
 * 释放的 cvec 中，两段 chr 数组都在结构体之后，并非
 * 另行 malloc；故 chrspace 与 rangespace 实际不可变。
 */
struct cvec
{
	int			nchrs;			/* chr 个数 */
	int			chrspace;		/* chrs[] 中已分配的 chr 数 */
	chr		   *chrs;			/* chr 向量指针 */
	int			nranges;		/* 范围数（chr 对） */
	int			rangespace;		/* ranges[] 中已分配的范围数 */
	chr		   *ranges;			/* 指向 chr 对向量的指针 */
	int			cclasscode;		/* 字符类枚举值，或 -1 */
};


/*
 * NFA 内部表示的定义
 */
struct state;

struct arc
{
	int			type;			/* 0 空闲，否则 NFA 弧类型码 */
	color		co;				/* 弧所匹配的色（可能是彩虹） */
	struct state *from;			/* 来自何处 */
	struct state *to;			/* 去往何处 */
	struct arc *outchain;		/* *from 出链或空闲链中的链接 */
	struct arc *outchainRev;	/* *from 出链中的反向链接 */
#define  freechain	outchain	/* 我们不维护 "freechainRev" */
	struct arc *inchain;		/* *to 入链中的链接 */
	struct arc *inchainRev;		/* *to 入链中的反向链接 */
	/* 当 co == 彩虹时不用这些字段： */
	struct arc *colorchain;		/* 色的弧链中的链接 */
	struct arc *colorchainRev;	/* 色的弧链中的反向链接 */
};

struct arcbatch
{								/* 供弧的批量分配 */
	struct arcbatch *next;		/* 链环节 */
	size_t		narcs;			/* 本 arcbatch 中已分配的弧数 */
	struct arc	a[FLEXIBLE_ARRAY_MEMBER];
};
#define  ARCBATCHSIZE(n)  ((n) * sizeof(struct arc) + offsetof(struct arcbatch, a))
/* 首批有 FIRSTABSIZE 条弧；其后倍增直至 MAXABSIZE */
#define  FIRSTABSIZE	64
#define  MAXABSIZE		1024

struct state
{
	int			no;				/* 状态号，从零起；或 FREESTATE */
#define  FREESTATE	 (-1)
	char		flag;			/* 标记特殊状态 */
	int			nins;			/* 入弧数 */
	int			nouts;			/* 出弧数 */
	struct arc *ins;			/* 入弧链 */
	struct arc *outs;			/* 出弧链 */
	struct state *tmp;			/* 遍历算法用的临时量 */
	struct state *next;			/* 遍历全部存活状态的链 */
	/* "next" 字段也用于把空闲状态串成链 */
	struct state *prev;			/* 存活状态链中的反向链接 */
};

struct statebatch
{								/* 供状态的批量分配 */
	struct statebatch *next;	/* 链环节 */
	size_t		nstates;		/* 本批中已分配的状态数 */
	struct state s[FLEXIBLE_ARRAY_MEMBER];
};
#define  STATEBATCHSIZE(n)  ((n) * sizeof(struct state) + offsetof(struct statebatch, s))
/* 首批有 FIRSTSBSIZE 个状态；其后倍增直至 MAXSBSIZE */
#define  FIRSTSBSIZE	32
#define  MAXSBSIZE		1024

struct nfa
{
	struct state *pre;			/* 预初始状态 */
	struct state *init;			/* 初始状态 */
	struct state *final;		/* 终态 */
	struct state *post;			/* 后终结状态 */
	int			nstates;		/* 供状态编号 */
	struct state *states;		/* 存活状态链 */
	struct state *slast;		/* 链的尾部 */
	struct state *freestates;	/* 空闲状态链 */
	struct arc *freearcs;		/* 空闲弧链 */
	struct statebatch *lastsb;	/* statebatch 链 */
	struct arcbatch *lastab;	/* arcbatch 链 */
	size_t		lastsbused;		/* 从 *lastsb 已消耗的状态数 */
	size_t		lastabused;		/* 从 *lastab 已消耗的弧数 */
	struct colormap *cm;		/* 色图 */
	color		bos[2];			/* 赋给 BOS 与 BOL 的色（若有） */
	color		eos[2];			/* 赋给 EOS 与 EOL 的色（若有） */
	int			flags;			/* 转交给 cNFA 的标志 */
	int			minmatchall;	/* matchall 时最少匹配的 chr 数 */
	int			maxmatchall;	/* 最多匹配的 chr 数，或 DUPINF */
	struct vars *v;				/* 简化编译错误报告 */
	struct nfa *parent;			/* 父 NFA，若有 */
};



/*
 * 压缩 cNFA 的定义
 *
 * 压缩 cNFA 的主要空间节省来自把弧做得尽量
 * 小。每条弧我们只存转移色与下一状态号。
 * 各状态的出弧列表是自以下起的数组：
 * cnfa.states[statenumber]，以哑 carc 结构终止，其中
 * co == COLORLESS.
 *
 * 非哑 carc 结构有两类：普通弧与 LACON 弧。
 * 普通弧只把转移色号存为 "co"。LACON 弧把
 * 环视约束号加 cnfa.ncolors 存为 "co"。LACON
 * 弧与普通弧可用 co >= cnfa.ncolors 区分。
 *
 * 注意普通弧中 "co" 可为彩虹；因其为负，
 * 并不破坏识别 LACON 弧的规则。
 *
 * 我们对可匹配任意串的 "平凡" NFA 有特殊标记（
 * 可能限制其中字符数）。在此情况
 * 下，flags & MATCHALL 置位（且 HASLACONS 无法置位）。
 * 然后 minmatchall 与 maxmatchall 给出最少与最多
 * 要匹配的字符数。例如 ".*" 产生 minmatchall = 0
 * 且 maxmatchall = DUPINF，而 ".+" 产生 minmatchall = 1 且
 * maxmatchall=DUPINF.
 */
struct carc
{
	color		co;				/* COLORLESS 为列表终止符 */
	int			to;				/* 下一状态号 */
};

struct cnfa
{
	int			nstates;		/* 状态数 */
	int			ncolors;		/* 色数（在用最大色 + 1） */
	int			flags;			/* 下列标志的位掩码： */
#define  HASLACONS	01			/* 使用环视约束 */
#define  MATCHALL	02			/* 匹配某长度范围内的全部串 */
#define  HASCANTMATCH 04		/* 含 CANTMATCH 弧 */
	/* 注意：HASCANTMATCH 见于 NFA 的 flags，从不在 cNFA */
	int			pre;			/* setup 状态号 */
	int			post;			/* teardown 状态号 */
	color		bos[2];			/* 赋给 BOS 与 BOL 的色（若有） */
	color		eos[2];			/* 赋给 EOS 与 EOL 的色（若有） */
	char	   *stflags;		/* 每状态标志字节向量 */
#define  CNFA_NOPROGRESS	01	/* 无进展状态的标志位 */
	struct carc **states;		/* 出弧列表指针向量 */
	/* states[n] 指向单一 malloc 的弧数组 */
	struct carc *arcs;			/* 列表所用的区域 */
	/* 这些字段仅用于 MATCHALL NFA（否则为 -1）： */
	int			minmatchall;	/* 最少匹配的 chr 数 */
	int			maxmatchall;	/* 最多匹配的 chr 数，或 DUPINF */
};

/*
 * 调试时，未填充的 cNFA 若全为零会很有帮助。
 * 而在生产中，我们只要求 nstates 为零。
 */
#ifdef REG_DEBUG
#define ZAPCNFA(cnfa)	memset(&(cnfa), 0, sizeof(cnfa))
#else
#define ZAPCNFA(cnfa)	((cnfa).nstates = 0)
#endif
#define NULLCNFA(cnfa)	((cnfa).nstates == 0)

/*
 * 本符号限制正则编译器所用的瞬态堆空间，
 * 从而也限制我们处理的 NFA 最大复杂度。
 * 目前只把 NFA 状态与弧计入此项；其余
 * 瞬态数据通常相对它们不够大，不值一提。
 * 注意我们对最终输出数据结构不收取任何
 * 费用（压缩 NFA 与色图）。
 * 此处的换算基于经验测定：非常大的
 * NFA 大约有 4 弧/状态。
 */
#ifndef REG_MAX_COMPILE_SPACE
#define REG_MAX_COMPILE_SPACE  \
	(500000 * (sizeof(struct state) + 4 * sizeof(struct arc)))
#endif

/*
 * 子表达式树
 *
 * "op" 之一：
 *		'='  无有趣子结构的普通正则（以 DFA 实现）
 *		'b'  反向引用（亦无子结构）
 *		'('  空操作捕获节点：捕获其唯一子节点的匹配
 *		'.'  连接：先匹配第一子节点，再匹配第二子节点
 *		'|'  择一：匹配任一子节点的匹配
 *		'*'  迭代：匹配其唯一子节点若干次匹配
 *
 * 择一节点可有任意个子女（但至少两个），
 * 经其 sibling 字段相连。
 *
 * 连接节点必须恰有两个子女。支持更多或许有用，
 * 但那会使执行器复杂化。注意是
 * 第一子女的贪心性决定该节点对拆分
 * 匹配处的偏好。
 *
 * 注意：当反向引用被直接量化时，我们把 min/max 计数
 * 塞进反向引用，而非再盖一层迭代节点。这是
 * 为效率：无需搜索可能的分割点。
 */
struct subre
{
	char		op;				/* 见上文类型码 */
	char		flags;
#define  LONGER  01				/* 偏好更长匹配 */
#define  SHORTER 02				/* 偏好更短匹配 */
#define  MIXED	 04				/* 下方偏好混合 */
#define  CAP	 010			/* 此处或下方有捕获括号 */
#define  BACKR	 020			/* 此处或下方有反引 */
#define  BRUSE	 040			/* 被某反向引用所引用 */
#define  INUSE	 0100			/* 用于最终树 */
#define  UPPROP  (MIXED|CAP|BACKR)	/* 应向上传播的标志 */
#define  LMIX(f) ((f)<<2)		/* LONGER -> MIXED */
#define  SMIX(f) ((f)<<1)		/* SHORTER -> MIXED */
#define  UP(f)	 (((f)&UPPROP) | (LMIX(f) & SMIX(f) & MIXED))
#define  MESSY(f)	 ((f)&(MIXED|CAP|BACKR))
#define  PREF(f)	 ((f)&(LONGER|SHORTER))
#define  PREF2(f1, f2)	 ((PREF(f1) != 0) ? PREF(f1) : PREF(f2))
#define  COMBINE(f1, f2) (UP((f1)|(f2)) | PREF2(f1, f2))
	char		latype;			/* 若为环视约束则为 LATYPE 码 */
	int			id;				/* subre ID (1..ntree-1) */
	int			capno;			/* 捕获节点则写入的 subno */
	int			backno;			/* 反引节点则其所指 subno */
	short		min;			/* 迭代或反向引用的最少重复 */
	short		max;			/* 迭代或反向引用的最多重复 */
	struct subre *child;		/* 第一子女(若有；亦空闲链) */
	struct subre *sibling;		/* 同父的下一子女（若有） */
	struct state *begin;		/* 此处起的出弧 */
	struct state *end;			/* 止于此处的入弧 */
	struct cnfa cnfa;			/* 压缩 NFA，若有 */
	struct subre *chain;		/* 供簿记与错误清理 */
};



/*
 * 通用操作函数的函数指针表
 * regex_t 的 re_fns 指向其中之一。
 */
struct fns
{
	void		FUNCPTR(free, (regex_t *));
	int			FUNCPTR(stack_too_deep, (void));
};

#define STACK_TOO_DEEP(re)	\
	((*((struct fns *) (re)->re_fns)->stack_too_deep) ())


/*
 * regex_t 的内部，藏在 void * 之后
 */
struct guts
{
	int			magic;
#define  GUTSMAGIC	 0xfed9
	int			cflags;			/* 编译标志的副本 */
	long		info;			/* re_info 副本 */
	size_t		nsub;			/* re_nsub 副本 */
	struct subre *tree;
	struct cnfa search;			/* 供快速预搜索 */
	int			ntree;			/* subre 的个数加一 */
	struct colormap cmap;
	int			FUNCPTR(compare, (const chr *, const chr *, size_t));
	struct subre *lacons;		/* 环视约束向量 */
	int			nlacons;		/* LACON表lacons[]大小；注意只用
								 * 编号 1 .. nlacons-1 的槽*/
};


/* 从 regcomp.c 导出到 regexec.c 的函数原型 */
extern void pg_set_regex_collation(Oid collation);
extern color pg_reg_getcolor(struct colormap *cm, chr c);
