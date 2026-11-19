/*
 * re_*comp 诸例程 - 编译 RE。
 * 本文件 #include 若干其他文件（见底）。
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
 * src/backend/regex/regcomp.c
 *
 */

#include "regex/regguts.h"

/*
 * 前向声明，放此处以便前向数据类型等尽早定义
 */
/* === regcomp.c === */
static void moresubs(struct vars *v, int wanted);
static int	freev(struct vars *v, int err);
static void makesearch(struct vars *v, struct nfa *nfa);
static struct subre *parse(struct vars *v, int stopper, int type,
						   struct state *init, struct state *final);
static struct subre *parsebranch(struct vars *v, int stopper, int type,
								 struct state *left, struct state *right,
								 int partial);
static struct subre *parseqatom(struct vars *v, int stopper, int type,
								struct state *lp, struct state *rp,
								struct subre *top);
static void nonword(struct vars *v, int dir, struct state *lp,
					struct state *rp);
static void word(struct vars *v, int dir, struct state *lp, struct state *rp);
static void charclass(struct vars *v, enum char_classes cls, struct state *lp,
					  struct state *rp);
static void charclasscomplement(struct vars *v, enum char_classes cls,
								struct state *lp, struct state *rp);
static int	scannum(struct vars *v);
static void repeat(struct vars *v, struct state *lp, struct state *rp,
				   int m, int n);
static void bracket(struct vars *v, struct state *lp, struct state *rp);
static void cbracket(struct vars *v, struct state *lp, struct state *rp);
static void brackpart(struct vars *v, struct state *lp, struct state *rp,
					  bool *have_cclassc);
static const chr *scanplain(struct vars *v);
static void onechr(struct vars *v, chr c, struct state *lp, struct state *rp);
static void optimizebracket(struct vars *v, struct state *lp, struct state *rp);
static void wordchrs(struct vars *v);
static void processlacon(struct vars *v, struct state *begin,
						 struct state *end, int latype,
						 struct state *lp, struct state *rp);
static struct subre *subre(struct vars *v, int op, int flags,
						   struct state *begin, struct state *end);
static void freesubre(struct vars *v, struct subre *sr);
static void freesubreandsiblings(struct vars *v, struct subre *sr);
static void freesrnode(struct vars *v, struct subre *sr);
static void removecaptures(struct vars *v, struct subre *t);
static int	numst(struct subre *t, int start);
static void markst(struct subre *t);
static void cleanst(struct vars *v);
static long nfatree(struct vars *v, struct subre *t, FILE *f);
static long nfanode(struct vars *v, struct subre *t,
					int converttosearch, FILE *f);
static int	newlacon(struct vars *v, struct state *begin, struct state *end,
					 int latype);
static void freelacons(struct subre *subs, int n);
static void rfree(regex_t *re);
static int	rstacktoodeep(void);

#ifdef REG_DEBUG
static void dump(regex_t *re, FILE *f);
static void dumpst(struct subre *t, FILE *f, int nfapresent);
static void stdump(struct subre *t, FILE *f, int nfapresent);
static const char *stid(struct subre *t, char *buf, size_t bufsize);
#endif
/* === regc_lex.c === */
static void lexstart(struct vars *v);
static void prefixes(struct vars *v);
static int	next(struct vars *v);
static int	lexescape(struct vars *v);
static chr	lexdigits(struct vars *v, int base, int minlen, int maxlen);
static int	brenext(struct vars *v, chr c);
static void skip(struct vars *v);
static chr	newline(void);
static chr	chrnamed(struct vars *v, const chr *startp, const chr *endp,
					 chr lastresort);

/* === regc_color.c === */
static void initcm(struct vars *v, struct colormap *cm);
static void freecm(struct colormap *cm);
static color maxcolor(struct colormap *cm);
static color newcolor(struct colormap *cm);
static void freecolor(struct colormap *cm, color co);
static color pseudocolor(struct colormap *cm);
static color subcolor(struct colormap *cm, chr c);
static color subcolorhi(struct colormap *cm, color *pco);
static color newsub(struct colormap *cm, color co);
static int	newhicolorrow(struct colormap *cm, int oldrow);
static void newhicolorcols(struct colormap *cm);
static void subcolorcvec(struct vars *v, struct cvec *cv, struct state *lp,
						 struct state *rp);
static void subcoloronechr(struct vars *v, chr ch, struct state *lp,
						   struct state *rp, color *lastsubcolor);
static void subcoloronerange(struct vars *v, chr from, chr to,
							 struct state *lp, struct state *rp,
							 color *lastsubcolor);
static void subcoloronerow(struct vars *v, int rownum, struct state *lp,
						   struct state *rp, color *lastsubcolor);
static void okcolors(struct nfa *nfa, struct colormap *cm);
static void colorchain(struct colormap *cm, struct arc *a);
static void uncolorchain(struct colormap *cm, struct arc *a);
static void rainbow(struct nfa *nfa, struct colormap *cm, int type, color but,
					struct state *from, struct state *to);
static void colorcomplement(struct nfa *nfa, struct colormap *cm, int type,
							struct state *of, struct state *from,
							struct state *to);

#ifdef REG_DEBUG
static void dumpcolors(struct colormap *cm, FILE *f);
static void dumpchr(chr c, FILE *f);
#endif
/* === regc_nfa.c === */
static struct nfa *newnfa(struct vars *v, struct colormap *cm,
						  struct nfa *parent);
static void freenfa(struct nfa *nfa);
static struct state *newstate(struct nfa *nfa);
static struct state *newfstate(struct nfa *nfa, int flag);
static void dropstate(struct nfa *nfa, struct state *s);
static void freestate(struct nfa *nfa, struct state *s);
static void newarc(struct nfa *nfa, int t, color co,
				   struct state *from, struct state *to);
static void createarc(struct nfa *nfa, int t, color co,
					  struct state *from, struct state *to);
static struct arc *allocarc(struct nfa *nfa);
static void freearc(struct nfa *nfa, struct arc *victim);
static void changearcsource(struct arc *a, struct state *newfrom);
static void changearctarget(struct arc *a, struct state *newto);
static int	hasnonemptyout(struct state *s);
static struct arc *findarc(struct state *s, int type, color co);
static void cparc(struct nfa *nfa, struct arc *oa,
				  struct state *from, struct state *to);
static void sortins(struct nfa *nfa, struct state *s);
static int	sortins_cmp(const void *a, const void *b);
static void sortouts(struct nfa *nfa, struct state *s);
static int	sortouts_cmp(const void *a, const void *b);
static void moveins(struct nfa *nfa, struct state *oldState,
					struct state *newState);
static void copyins(struct nfa *nfa, struct state *oldState,
					struct state *newState);
static void mergeins(struct nfa *nfa, struct state *s,
					 struct arc **arcarray, int arccount);
static void moveouts(struct nfa *nfa, struct state *oldState,
					 struct state *newState);
static void copyouts(struct nfa *nfa, struct state *oldState,
					 struct state *newState);
static void cloneouts(struct nfa *nfa, struct state *old, struct state *from,
					  struct state *to, int type);
static void delsub(struct nfa *nfa, struct state *lp, struct state *rp);
static void deltraverse(struct nfa *nfa, struct state *leftend,
						struct state *s);
static void dupnfa(struct nfa *nfa, struct state *start, struct state *stop,
				   struct state *from, struct state *to);
static void duptraverse(struct nfa *nfa, struct state *s, struct state *stmp);
static void removeconstraints(struct nfa *nfa, struct state *start, struct state *stop);
static void removetraverse(struct nfa *nfa, struct state *s);
static void cleartraverse(struct nfa *nfa, struct state *s);
static struct state *single_color_transition(struct state *s1,
											 struct state *s2);
static void specialcolors(struct nfa *nfa);
static long optimize(struct nfa *nfa, FILE *f);
static void pullback(struct nfa *nfa, FILE *f);
static int	pull(struct nfa *nfa, struct arc *con,
				 struct state **intermediates);
static void pushfwd(struct nfa *nfa, FILE *f);
static int	push(struct nfa *nfa, struct arc *con,
				 struct state **intermediates);

#define INCOMPATIBLE	1		/* 销毁该弧 */
#define SATISFIED	2			/* 约束已满足 */
#define COMPATIBLE	3			/* 相容但尚未满足 */
#define REPLACEARC	4			/* 用约束色替换弧的色 */
static int	combine(struct nfa *nfa, struct arc *con, struct arc *a);
static void fixempties(struct nfa *nfa, FILE *f);
static struct state *emptyreachable(struct nfa *nfa, struct state *s,
									struct state *lastfound,
									struct arc **inarcsorig);
static int	isconstraintarc(struct arc *a);
static int	hasconstraintout(struct state *s);
static void fixconstraintloops(struct nfa *nfa, FILE *f);
static int	findconstraintloop(struct nfa *nfa, struct state *s);
static void breakconstraintloop(struct nfa *nfa, struct state *sinitial);
static void clonesuccessorstates(struct nfa *nfa, struct state *ssource,
								 struct state *sclone,
								 struct state *spredecessor,
								 struct arc *refarc, char *curdonemap,
								 char *outerdonemap, int nstates);
static void removecantmatch(struct nfa *nfa);
static void cleanup(struct nfa *nfa);
static void markreachable(struct nfa *nfa, struct state *s,
						  struct state *okay, struct state *mark);
static void markcanreach(struct nfa *nfa, struct state *s, struct state *okay,
						 struct state *mark);
static long analyze(struct nfa *nfa);
static void checkmatchall(struct nfa *nfa);
static bool checkmatchall_recurse(struct nfa *nfa, struct state *s,
								  bool **haspaths);
static bool check_out_colors_match(struct state *s, color co1, color co2);
static bool check_in_colors_match(struct state *s, color co1, color co2);
static void compact(struct nfa *nfa, struct cnfa *cnfa);
static void carcsort(struct carc *first, size_t n);
static int	carc_cmp(const void *a, const void *b);
static void freecnfa(struct cnfa *cnfa);
static void dumpnfa(struct nfa *nfa, FILE *f);

#ifdef REG_DEBUG
static void dumpstate(struct state *s, FILE *f);
static void dumparcs(struct state *s, FILE *f);
static void dumparc(struct arc *a, struct state *s, FILE *f);
static void dumpcnfa(struct cnfa *cnfa, FILE *f);
static void dumpcstate(int st, struct cnfa *cnfa, FILE *f);
#endif
/* === regc_cvec.c === */
static struct cvec *newcvec(int nchrs, int nranges);
static struct cvec *clearcvec(struct cvec *cv);
static void addchr(struct cvec *cv, chr c);
static void addrange(struct cvec *cv, chr from, chr to);
static struct cvec *getcvec(struct vars *v, int nchrs, int nranges);
static void freecvec(struct cvec *cv);

/* === regc_pg_locale.c === */
static int	pg_wc_isdigit(pg_wchar c);
static int	pg_wc_isalpha(pg_wchar c);
static int	pg_wc_isalnum(pg_wchar c);
static int	pg_wc_isword(pg_wchar c);
static int	pg_wc_isupper(pg_wchar c);
static int	pg_wc_islower(pg_wchar c);
static int	pg_wc_isgraph(pg_wchar c);
static int	pg_wc_isprint(pg_wchar c);
static int	pg_wc_ispunct(pg_wchar c);
static int	pg_wc_isspace(pg_wchar c);
static pg_wchar pg_wc_toupper(pg_wchar c);
static pg_wchar pg_wc_tolower(pg_wchar c);

/* === regc_locale.c === */
static chr	element(struct vars *v, const chr *startp, const chr *endp);
static struct cvec *range(struct vars *v, chr a, chr b, int cases);
static int	before(chr x, chr y);
static struct cvec *eclass(struct vars *v, chr c, int cases);
static enum char_classes lookupcclass(struct vars *v, const chr *startp,
									  const chr *endp);
static struct cvec *cclasscvec(struct vars *v, enum char_classes cclasscode,
							   int cases);
static int	cclass_column_index(struct colormap *cm, chr c);
static struct cvec *allcases(struct vars *v, chr c);
static int	cmp(const chr *x, const chr *y, size_t len);
static int	casecmp(const chr *x, const chr *y, size_t len);


/* 内部变量，捆在一起便于传递 */
struct vars
{
	regex_t    *re;
	const chr  *now;			/* 串内扫描指针 */
	const chr  *stop;			/* 字符串末 */
	int			err;			/* 错误码（无则0） */
	int			cflags;			/* 编译标志副本 */
	int			lasttype;		/* 上一记号类型 */
	int			nexttype;		/* 下一记号类型 */
	chr			nextvalue;		/* 下一记号值（若有） */
	int			lexcon;			/* 词法上下文类型(见 regc_lex.c) */
	int			nsubexp;		/* 子表达式计数 */
	struct subre **subs;		/* subRE 指针向量 */
	size_t		nsubs;			/* 向量长度 */
	struct subre *sub10[10];	/* 初始向量，多数够用 */
	struct nfa *nfa;			/* 该 NFA */
	struct colormap *cm;		/* 字符色图 */
	color		nlcolor;		/* 换行符之色 */
	struct state *wordchrs;		/* NFA 中持有词字符出弧的状态 */
	struct subre *tree;			/* 子表达式树 */
	struct subre *treechain;	/* 已分配全部树节点 */
	struct subre *treefree;		/* 空闲树节点 */
	int			ntree;			/* 树节点数加一 */
	struct cvec *cv;			/* 接口 cvec */
	struct cvec *cv2;			/* 工具 cvec */
	struct subre *lacons;		/* 环视约束向量 */
	int			nlacons;		/* lacons[](LACON)大小；注意仅
								 * 编号1..nlacons-1的槽在用 */
	size_t		spaceused;		/* 编译约占用的空间 */
};

/* 解析宏；多数知 `v' 为 vars 结构指针 */
#define NEXT()	(next(v))		/* 前进一个记号 */
#define SEE(t)	(v->nexttype == (t))	/* 下一是此吗？ */
#define EAT(t)	(SEE(t) && next(v)) /* 若下一是此则吞之 */
#define VISERR(vv)	((vv)->err != 0)	/* 是否已见错误？ */
#define ISERR() VISERR(v)
#define VERR(vv,e)	((vv)->nexttype = EOS, \
					 (vv)->err = ((vv)->err ? (vv)->err : (e)))
#define ERR(e)	VERR(v, e)		/* 记录错误 */
#define NOERR() {if (ISERR()) return;}	/* 若已见错则返回 */
#define NOERRN()	{if (ISERR()) return NULL;} /* NOERR+返回值 */
#define NOERRZ()	{if (ISERR()) return 0;}	/* NOERR+返回值 */
#define INSIST(c, e) do { if (!(c)) ERR(e); } while (0) /* c 假则报错 */
#define NOTE(b) (v->re->re_info |= (b)) /* 记下可见条件 */
#define EMPTYARC(x, y)	newarc(v->nfa, EMPTY, 0, x, y)

/* 记号类型码，有的亦作 NFA 弧类型 */
#define EMPTY	'n'				/* 当前无记号 */
#define EOS 'e'					/* 字符串末 */
#define PLAIN	'p'				/* 普通字符 */
#define DIGIT	'd'				/* 界内数字 */
#define BACKREF 'b'				/* 反向引用 */
#define COLLEL	'I'				/* [. 之始 */
#define ECLASS	'E'				/* [= 之始 */
#define CCLASS	'C'				/* [: 之始 */
#define END 'X'					/* [.[=[: 之末 */
#define CCLASSS	's'				/* 字符类速记转义 */
#define CCLASSC	'c'				/* 补字符类速记转义 */
#define RANGE	'R'				/* [] 内可能作范围界的 - */
#define LACON	'L'				/* 环视约束 subRE */
#define AHEAD	'a'				/* 色前瞻弧 */
#define BEHIND	'r'				/* 色后顾弧 */
#define WBDRY	'w'				/* 词边界约束 */
#define NWBDRY	'W'				/* 非词边界约束 */
#define CANTMATCH 'x'			/* 无法匹配任何内容的弧 */
#define SBEGIN	'A'				/* 字符串始（即使非 BOL） */
#define SEND	'Z'				/* 字符串末（即使非 EOL） */

/* 弧是否已着色，因而应属于某色链？ */
/* 对 "co" 的检测排除彩虹弧，我们不费心将其入链 */
#define COLORED(a) \
	((a)->co >= 0 && \
	 ((a)->type == PLAIN || (a)->type == AHEAD || (a)->type == BEHIND))


/* 静态函数列表 */
static const struct fns functions = {
	rfree,						/* regfree 内部 */
	rstacktoodeep				/* 检查栈是否危险地过深 */
};



/*
 * pg_regcomp - 编译正则表达式
 *
 * 注意：失败时不再占用资源，故 pg_regfree()
 * 无需再作用于 re。
 */
int
pg_regcomp(regex_t *re,
		   const chr *string,
		   size_t len,
		   int flags,
		   Oid collation)
{
	struct vars var;
	struct vars *v = &var;
	struct guts *g;
	int			i;
	size_t		j;

#ifdef REG_DEBUG
	FILE	   *debug = (flags & REG_PROGRESS) ? stdout : (FILE *) NULL;
#else
	FILE	   *debug = (FILE *) NULL;
#endif

#define  CNOERR()	 { if (ISERR()) return freev(v, v->err); }

	/* 健全检查 */

	if (re == NULL || string == NULL)
		return REG_INVARG;
	if ((flags & REG_QUOTE) &&
		(flags & (REG_ADVANCED | REG_EXPANDED | REG_NEWLINE)))
		return REG_INVARG;
	if (!(flags & REG_EXTENDED) && (flags & REG_ADVF))
		return REG_INVARG;

	/* 初始化区域设置相关支持 */
	pg_set_regex_collation(collation);

	/* 初始设置（之后可调用 freev()） */
	v->re = re;
	v->now = string;
	v->stop = v->now + len;
	v->err = 0;
	v->cflags = flags;
	v->nsubexp = 0;
	v->subs = v->sub10;
	v->nsubs = 10;
	for (j = 0; j < v->nsubs; j++)
		v->subs[j] = NULL;
	v->nfa = NULL;
	v->cm = NULL;
	v->nlcolor = COLORLESS;
	v->wordchrs = NULL;
	v->tree = NULL;
	v->treechain = NULL;
	v->treefree = NULL;
	v->cv = NULL;
	v->cv2 = NULL;
	v->lacons = NULL;
	v->nlacons = 0;
	v->spaceused = 0;
	re->re_magic = REMAGIC;
	re->re_info = 0;			/* 解析期间置位各位 */
	re->re_csize = sizeof(chr);
	re->re_collation = collation;
	re->re_guts = NULL;
	re->re_fns = VS(&functions);

	/* 更复杂的设置，须 malloc 者 */
	re->re_guts = VS(MALLOC(sizeof(struct guts)));
	if (re->re_guts == NULL)
		return freev(v, REG_ESPACE);
	g = (struct guts *) re->re_guts;
	g->tree = NULL;
	initcm(v, &g->cmap);
	v->cm = &g->cmap;
	g->lacons = NULL;
	g->nlacons = 0;
	ZAPCNFA(g->search);
	v->nfa = newnfa(v, v->cm, (struct nfa *) NULL);
	CNOERR();
	/* 为 getcvec 建合理大小的暂态 cvec */
	v->cv = newcvec(100, 20);
	if (v->cv == NULL)
		return freev(v, REG_ESPACE);

	/* 解析 */
	lexstart(v);				/* 也处理前缀 */
	if ((v->cflags & REG_NLSTOP) || (v->cflags & REG_NLANCH))
	{
		/* 给换行符分配唯一色 */
		v->nlcolor = subcolor(v->cm, newline());
		okcolors(v->nfa, v->cm);
	}
	CNOERR();
	v->tree = parse(v, EOS, PLAIN, v->nfa->init, v->nfa->final);
	assert(SEE(EOS));			/* 即使出错；ISERR() => SEE(EOS) */
	CNOERR();
	assert(v->tree != NULL);

	/* 完成 NFA 及其 subre 树设置 */
	specialcolors(v->nfa);
	CNOERR();
#ifdef REG_DEBUG
	if (debug != NULL)
	{
		fprintf(debug, "\n\n\n========= RAW ==========\n");
		dumpnfa(v->nfa, debug);
		dumpst(v->tree, debug, 1);
	}
#endif
	if (v->cflags & REG_NOSUB)
		removecaptures(v, v->tree);
	v->ntree = numst(v->tree, 1);
	markst(v->tree);
	cleanst(v);
#ifdef REG_DEBUG
	if (debug != NULL)
	{
		fprintf(debug, "\n\n\n========= TREE FIXED ==========\n");
		dumpst(v->tree, debug, 1);
	}
#endif

	/* 为树与 lacons(LACON) 建紧凑 NFA */
	re->re_info |= nfatree(v, v->tree, debug);
	CNOERR();
	assert(v->nlacons == 0 || v->lacons != NULL);
	for (i = 1; i < v->nlacons; i++)
	{
		struct subre *lasub = &v->lacons[i];

#ifdef REG_DEBUG
		if (debug != NULL)
			fprintf(debug, "\n\n\n========= LA%d ==========\n", i);
#endif

		/* 若是后顾 LACON 则在模式前加 .* */
		nfanode(v, lasub, !LATYPE_IS_AHEAD(lasub->latype), debug);
	}
	CNOERR();
	if (v->tree->flags & SHORTER)
		NOTE(REG_USHORTEST);

	/* 为树、lacons(LACON)、快搜建紧凑 NFA */
#ifdef REG_DEBUG
	if (debug != NULL)
		fprintf(debug, "\n\n\n========= SEARCH ==========\n");
#endif
	/* 主 NFA 现可牺牲，故用作工作区 */
	(DISCARD) optimize(v->nfa, debug);
	CNOERR();
	makesearch(v, v->nfa);
	CNOERR();
	compact(v->nfa, &g->search);
	CNOERR();

	/* 无误，打包收工 */
	re->re_nsub = v->nsubexp;
	v->re = NULL;				/* freev 不再释放 re */
	g->magic = GUTSMAGIC;
	g->cflags = v->cflags;
	g->info = re->re_info;
	g->nsub = re->re_nsub;
	g->tree = v->tree;
	v->tree = NULL;
	g->ntree = v->ntree;
	g->compare = (v->cflags & REG_ICASE) ? casecmp : cmp;
	g->lacons = v->lacons;
	v->lacons = NULL;
	g->nlacons = v->nlacons;

#ifdef REG_DEBUG
	if (flags & REG_DUMP)
	{
		dump(re, stdout);
		fflush(stdout);
	}
#endif

	assert(v->err == 0);
	return freev(v, 0);
}

/*
 * moresubs - 扩大 subRE 向量
 */
static void
moresubs(struct vars *v,
		 int wanted)			/* 要为这一项留足空间 */
{
	struct subre **p;
	size_t		n;

	assert(wanted > 0 && (size_t) wanted >= v->nsubs);
	n = (size_t) wanted * 3 / 2 + 1;

	if (v->subs == v->sub10)
	{
		p = (struct subre **) MALLOC(n * sizeof(struct subre *));
		if (p != NULL)
			memcpy(VS(p), VS(v->subs),
				   v->nsubs * sizeof(struct subre *));
	}
	else
		p = (struct subre **) REALLOC(v->subs, n * sizeof(struct subre *));
	if (p == NULL)
	{
		ERR(REG_ESPACE);
		return;
	}
	v->subs = p;
	for (p = &v->subs[v->nsubs]; v->nsubs < n; p++, v->nsubs++)
		*p = NULL;
	assert(v->nsubs == n);
	assert((size_t) wanted < v->nsubs);
}

/*
 * freev - 必要时释放 vars 结构的子结构
 *
 * 可选地设置错误号，并始终返回错误码
 * 有则返回，使错误处理代码更简。
 */
static int
freev(struct vars *v,
	  int err)
{
	if (v->re != NULL)
		rfree(v->re);
	if (v->subs != v->sub10)
		FREE(v->subs);
	if (v->nfa != NULL)
		freenfa(v->nfa);
	if (v->tree != NULL)
		freesubre(v, v->tree);
	if (v->treechain != NULL)
		cleanst(v);
	if (v->cv != NULL)
		freecvec(v->cv);
	if (v->cv2 != NULL)
		freecvec(v->cv2);
	if (v->lacons != NULL)
		freelacons(v->lacons, v->nlacons);
	ERR(err);					/* err==0则nop */

	return v->err;
}

/*
 * makesearch - 把 NFA 变为搜索 NFA（隐式前置 .*?）
 * NFA 须已先经 optimize()
 */
static void
makesearch(struct vars *v,
		   struct nfa *nfa)
{
	struct arc *a;
	struct arc *b;
	struct state *pre = nfa->pre;
	struct state *s;
	struct state *s2;
	struct state *slist;

	/* 若已锚定则无需任何环 */
	for (a = pre->outs; a != NULL; a = a->outchain)
	{
		assert(a->type == PLAIN);
		if (a->co != nfa->bos[0] && a->co != nfa->bos[1])
			break;
	}
	if (a != NULL)
	{
		/* 在前面加隐式 .* */
		rainbow(nfa, v->cm, PLAIN, COLORLESS, pre, pre);

		/* 以及 ^* 与 \A*——未必总需要，但无害 */
		newarc(nfa, PLAIN, nfa->bos[0], pre, pre);
		newarc(nfa, PLAIN, nfa->bos[1], pre, pre);

		/*
		 * 若原先是 MATCHALL 则仍是，但最大匹配
		 * 长度现为无穷。
		 */
		if (nfa->flags & MATCHALL)
			nfa->maxmatchall = DUPINF;
	}

	/*
	 * 此处微妙。因许多 RE 没有后顾
	 * 约束，常知身在 pre 态也几乎无信息；
	 * 有信息的是下一状态。但其中有些
	 * 可能另有入弧，即或可先真正推进
	 * 再回到其中之一。对此须去优化，
	 * 把每态拆成有推进与无推进两态。
	 */

	/* 先列出既从 pre 可达又从别处可达的状态 */
	slist = NULL;
	for (a = pre->outs; a != NULL; a = a->outchain)
	{
		s = a->to;
		for (b = s->ins; b != NULL; b = b->inchain)
		{
			if (b->from != pre)
				break;
		}

		/*
		 * 要用非 NULL 的 tmp 标出已在表中的状态，
		 * 但无法把旧 slist 值直接存进 tmp，
		 * 因对第一个这样的状态行不通。改为
		 * 首个表项的 tmp 存其自身地址。
		 */
		if (b != NULL && s->tmp == NULL)
		{
			s->tmp = (slist != NULL) ? slist : s;
			slist = s;
		}
	}

	/* 执行拆分 */
	for (s = slist; s != NULL; s = s2)
	{
		s2 = newstate(nfa);
		NOERR();
		copyouts(nfa, s, s2);
		NOERR();
		for (a = s->ins; a != NULL; a = b)
		{
			b = a->inchain;
			if (a->from != pre)
			{
				cparc(nfa, a, a->from, s2);
				freearc(nfa, a);
			}
		}
		s2 = (s->tmp != s) ? s->tmp : NULL;
		s->tmp = NULL;			/* 顺手一并清理 */
	}
}

/*
 * parse - 解析 RE
 *
 * 其实只是顶层：解析若干用 '|' 连起的
 * 分支。若多于一条，它们在树中作为
 * 一个 '|' subre 的子节点。
 */
static struct subre *
parse(struct vars *v,
	  int stopper,				/* EOS或')' */
	  int type,					/* LACON(环视subRE)或PLAIN */
	  struct state *init,		/* 初始状态 */
	  struct state *final)		/* 终态 */
{
	struct subre *branches;		/* 顶层 */
	struct subre *lastbranch;	/* 最近分支 */

	assert(stopper == ')' || stopper == EOS);

	branches = subre(v, '|', LONGER, init, final);
	NOERRN();
	lastbranch = NULL;
	do
	{							/* 分支 */
		struct subre *branch;
		struct state *left;		/* 分支用脚手架 */
		struct state *right;

		left = newstate(v->nfa);
		right = newstate(v->nfa);
		NOERRN();
		EMPTYARC(init, left);
		EMPTYARC(right, final);
		NOERRN();
		branch = parsebranch(v, stopper, type, left, right, 0);
		NOERRN();
		if (lastbranch)
			lastbranch->sibling = branch;
		else
			branches->child = branch;
		branches->flags |= UP(branches->flags | branch->flags);
		lastbranch = branch;
	} while (EAT('|'));
	assert(SEE(stopper) || SEE(EOS));

	if (!SEE(stopper))
	{
		assert(stopper == ')' && SEE(EOS));
		ERR(REG_EPAREN);
	}

	/* 优化掉简单情形 */
	if (lastbranch == branches->child)
	{							/* 仅一条分支 */
		assert(lastbranch->sibling == NULL);
		freesrnode(v, branches);
		branches = lastbranch;
	}
	else if (!MESSY(branches->flags))
	{							/* 无有趣内核 */
		freesubreandsiblings(v, branches->child);
		branches->child = NULL;
		branches->op = '=';
	}

	return branches;
}

/*
 * parsebranch - 解析 RE 的一条分支
 *
 * 主要管拼接，并与 parseqatom() 紧密配合。
 * 拼接物尽量打成一捆，仅当因有子结构而
 * 必须时才另引入 '.' 节点。
 */
static struct subre *
parsebranch(struct vars *v,
			int stopper,		/* EOS或')' */
			int type,			/* LACON(环视subRE)或PLAIN */
			struct state *left, /* 最左状态 */
			struct state *right,	/* 最右状态 */
			int partial)		/* 是否只是分支的部分？ */
{
	struct state *lp;			/* 当前构造的左端 */
	int			seencontent;	/* 本分支里是否已有内容？ */
	struct subre *t;

	lp = left;
	seencontent = 0;
	t = subre(v, '=', 0, left, right);	/* op '=' 暂定 */
	NOERRN();
	while (!SEE('|') && !SEE(stopper) && !SEE(EOS))
	{
		if (seencontent)
		{						/* 隐式拼接运算符 */
			lp = newstate(v->nfa);
			NOERRN();
			moveins(v->nfa, right, lp);
		}
		seencontent = 1;

		/* 注意：parseqatom() 递归或会吞掉余下分支 */
		t = parseqatom(v, stopper, type, lp, right, t);
		NOERRN();
	}

	if (!seencontent)
	{							/* 空分支 */
		if (!partial)
			NOTE(REG_UUNSPEC);
		assert(lp == left);
		EMPTYARC(left, right);
	}

	return t;
}

/*
 * parseqatom - 解析 RE 的量化原子或约束
 *
 * 末段簿记与 parsebranch() 配合极紧；
 * 尤其内含可继续解析余下分支的递归，
 * 使本函数名略有名不副实。
 *
 * 通常返回值就是 "top"，但有时在已
 * 解析完余下分支后，会视 "top" 为多余并
 * 释放它，改返回某个子 subre。
 */
static struct subre *
parseqatom(struct vars *v,
		   int stopper,			/* EOS或')' */
		   int type,			/* LACON(环视subRE)或PLAIN */
		   struct state *lp,	/* 挂接用的左状态 */
		   struct state *rp,	/* 挂接用的右状态 */
		   struct subre *top)	/* 子树顶 */
{
	struct state *s;			/* 新状态用的临时量 */
	struct state *s2;

#define  ARCV(t, val)	 newarc(v->nfa, t, val, lp, rp)
	int			m,
				n;
	struct subre *atom;			/* 原子子树 */
	struct subre *t;
	int			cap;			/* 捕获括号？ */
	int			latype;			/* 环视约束类型 */
	int			subno;			/* 捕获括号或反向引用编号 */
	int			atomtype;
	int			qprefer;		/* 量词偏短/偏长偏好 */
	int			f;
	struct subre **atomp;		/* atom 指针所在处 */

	/* 初始簿记 */
	atom = NULL;
	assert(lp->nouts == 0);		/* 须串接新代码 */
	assert(rp->nins == 0);		/* 介于 lp 与 rp */
	subno = 0;					/* 仅求安抚 lint */

	/* 原子或约束…… */
	atomtype = v->nexttype;
	switch (atomtype)
	{
			/* 先处理约束，它们以 return 结束 */
		case '^':
			ARCV('^', 1);
			if (v->cflags & REG_NLANCH)
				ARCV(BEHIND, v->nlcolor);
			NEXT();
			return top;
			break;
		case '$':
			ARCV('$', 1);
			if (v->cflags & REG_NLANCH)
				ARCV(AHEAD, v->nlcolor);
			NEXT();
			return top;
			break;
		case SBEGIN:
			ARCV('^', 1);		/* BOL */
			ARCV('^', 0);		/* 或BOS */
			NEXT();
			return top;
			break;
		case SEND:
			ARCV('$', 1);		/* EOL */
			ARCV('$', 0);		/* 或EOS */
			NEXT();
			return top;
			break;
		case '<':
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case '>':
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case WBDRY:
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case NWBDRY:
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case LACON:				/* 环视约束 */
			latype = v->nextvalue;
			NEXT();
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERRN();
			t = parse(v, ')', LACON, s, s2);
			freesubre(v, t);	/* 内部结构无关紧要 */
			NOERRN();
			assert(SEE(')'));
			NEXT();
			processlacon(v, s, s2, latype, lp, rp);
			return top;
			break;
			/* 再处理错误，好先清出场 */
		case '*':
		case '+':
		case '?':
		case '{':
			ERR(REG_BADRPT);
			return top;
			break;
		default:
			ERR(REG_ASSERT);
			return top;
			break;
			/* 再是普通字符，及其轻微变体 */
		case ')':				/* 括号不配 */
			if ((v->cflags & REG_ADVANCED) != REG_EXTENDED)
			{
				ERR(REG_EPAREN);
				return top;
			}
			/* 因规范疏漏，在 ERE 中合法 */
			NOTE(REG_UPBOTCH);
			/* 落入 case PLAIN */
			/* FALLTHROUGH */
		case PLAIN:
			onechr(v, v->nextvalue, lp, rp);
			okcolors(v->nfa, v->cm);
			NOERRN();
			NEXT();
			break;
		case '[':
			if (v->nextvalue == 1)
				bracket(v, lp, rp);
			else
				cbracket(v, lp, rp);
			assert(SEE(']') || ISERR());
			NEXT();
			break;
		case CCLASSS:
			charclass(v, (enum char_classes) v->nextvalue, lp, rp);
			okcolors(v->nfa, v->cm);
			NEXT();
			break;
		case CCLASSC:
			charclasscomplement(v, (enum char_classes) v->nextvalue, lp, rp);
			/* charclasscomplement() 内部已调 okcolors() */
			NEXT();
			break;
		case '.':
			rainbow(v->nfa, v->cm, PLAIN,
					(v->cflags & REG_NLSTOP) ? v->nlcolor : COLORLESS,
					lp, rp);
			NEXT();
			break;
			/* 最后是难缠的部分 */
		case '(':				/* value 标志捕获或非捕获 */
			cap = (type == LACON) ? 0 : v->nextvalue;
			if (cap)
			{
				v->nsubexp++;
				subno = v->nsubexp;
				if ((size_t) subno >= v->nsubs)
					moresubs(v, subno);
			}
			else
				atomtype = PLAIN;	/* 不是 '(' 的东西 */
			NEXT();

			/*
			 * 另建端点状态，使此子 NFA 有别于
			 * 周围部分。须确保为反向引用复制
			 * 子 NFA 时只得到正确的状态/弧，
			 * 别无其它。尤其，若让反向引用从
			 * lp 到 rp 复制子 NFA 会大错特错，
			 * 因下文或会在此原子外再加量化
			 * 上层结构。或许非捕获括号可省
			 * 这些额外状态，但似乎不值得费事。
			 */
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERRN();
			/* 这些弧未必需要，但暂保持连通 */
			EMPTYARC(lp, s);
			EMPTYARC(s2, rp);
			NOERRN();
			atom = parse(v, ')', type, s, s2);
			assert(SEE(')') || ISERR());
			NEXT();
			NOERRN();
			if (cap)
			{
				if (atom->capno == 0)
				{
					/* 常态：仅把该原子标为捕获 */
					atom->flags |= CAP;
					atom->capno = subno;
				}
				else
				{
					/* 生成无操作包装节点以处理 "((x))" */
					t = subre(v, '(', atom->flags | CAP, s, s2);
					NOERRN();
					t->capno = subno;
					t->child = atom;
					atom = t;
				}
				assert(v->subs[subno] == NULL);
				v->subs[subno] = atom;
			}
			/* 推迟其余一切，以应对可能的 {0} */
			break;
		case BACKREF:			/* 黑湖来客式特性 */
			INSIST(type != LACON, REG_ESUBREG);
			subno = v->nextvalue;
			assert(subno > 0);
			INSIST(subno < v->nsubs, REG_ESUBREG);
			NOERRN();
			INSIST(v->subs[subno] != NULL, REG_ESUBREG);
			NOERRN();
			atom = subre(v, 'b', BACKR, lp, rp);
			NOERRN();
			atom->backno = subno;
			v->subs[subno]->flags |= BRUSE;
			EMPTYARC(lp, rp);	/* 临时的，好有个着落 */
			NEXT();
			break;
	}

	/* 以及原子后还可跟量词 */
	switch (v->nexttype)
	{
		case '*':
			m = 0;
			n = DUPINF;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '+':
			m = 1;
			n = DUPINF;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '?':
			m = 0;
			n = 1;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '{':
			NEXT();
			m = scannum(v);
			if (EAT(','))
			{
				if (SEE(DIGIT))
					n = scannum(v);
				else
					n = DUPINF;
				if (m > n)
				{
					ERR(REG_BADBR);
					return top;
				}
				/* {m,n} 会动用偏好，即便是 {m,m} */
				qprefer = (v->nextvalue) ? LONGER : SHORTER;
			}
			else
			{
				n = m;
				/* {m} 把操作数的偏好原样传出 */
				qprefer = 0;
			}
			if (!SEE('}'))
			{					/* 也拦住错误 */
				ERR(REG_BADBR);
				return top;
			}
			NEXT();
			break;
		default:				/* 无量词 */
			m = n = 1;
			qprefer = 0;
			break;
	}

	/* 讨厌特判：{0} 或 {0,0} 取消一切 */
	if (m == 0 && n == 0)
	{
		/*
		 * 若原子内有捕获子表达式，我们不想
		 * 毁掉它们，因对其反向引用合法（虽无用）。
		 * 故只把原子从 lp/rp 解链，然后忽略。
		 */
		if (atom != NULL && (atom->flags & CAP))
		{
			delsub(v->nfa, lp, atom->begin);
			delsub(v->nfa, atom->end, rp);
		}
		else
		{
			/* 否则可清掉我们建的任何 subre 设施 */
			if (atom != NULL)
				freesubre(v, atom);
			delsub(v->nfa, lp, rp);
		}
		EMPTYARC(lp, rp);
		return top;
	}

	/* 若非棘手情形，可避开难段 */
	assert(!MESSY(top->flags));
	f = top->flags | qprefer | ((atom != NULL) ? atom->flags : 0);
	if (atomtype != '(' && atomtype != BACKREF && !MESSY(UP(f)))
	{
		if (!(m == 1 && n == 1))
			repeat(v, lp, rp, m, n);
		if (atom != NULL)
			freesubre(v, atom);
		top->flags = f;
		return top;
	}

	/*
	 * 难段：有棘手之处
	 *
	 * 即捕获括号、反向引用、短/长冲突，或原子
	 * 的子结构含上述之一。
	 */

	/* 即使内容平淡，现也需为其建 subre */
	if (atom == NULL)
	{
		atom = subre(v, '=', 0, lp, rp);
		NOERRN();
	}

	/*
	 * 此后需要原子自有 begin/end 状态，
	 * 且与 lp/rp 相异，以便在其外裹上迭代结构。
	 * 上方括号原子情形已造出合用状态，
	 * 且不想改捕获 subre，因其已记入
	 * v->subs[]。否则还需更多状态。
	 */
	if (atom->begin == lp || atom->end == rp)
	{
		s = newstate(v->nfa);
		s2 = newstate(v->nfa);
		NOERRN();
		moveouts(v->nfa, lp, s);
		moveins(v->nfa, rp, s2);
		atom->begin = s;
		atom->end = s2;
	}
	else
	{
		/* 原子没问题，但须暂时把它与 lp/rp 断开 */
		/* 去掉上面造的 EMPTY 弧 */
		delsub(v->nfa, lp, atom->begin);
		delsub(v->nfa, atom->end, rp);
	}

	/*----------
	 * Prepare a general-purpose state skeleton.
	 *
	 * In the no-backrefs case, we want this:
	 *
	 * [lp] ---> [s] ---prefix---> ---atom---> ---rest---> [rp]
	 *
	 * where prefix is some repetitions of atom, and "rest" is the remainder
	 * of the branch.  In the general case we need:
	 *
	 * [lp] ---> [s] ---iterator---> [s2] ---rest---> [rp]
	 *
	 * where the iterator wraps around the atom.
	 *
	 * We make the s state here for both cases; s2 is made below if needed
	 *----------
	 */
	s = newstate(v->nfa);		/* 建立起始状态 */
	NOERRN();
	EMPTYARC(lp, s);
	NOERRN();

	/* 把剩余 subRE 拆成 x{...} 与其后内容 */
	t = subre(v, '.', COMBINE(qprefer, atom->flags), lp, rp);
	NOERRN();
	t->child = atom;
	atomp = &t->child;

	/*
	 * 此处本应递归填 t->child->sibling ……但须
	 * 推迟到末尾。一因 t->child 下方可能被替换，
	 * 我们不想操心其 sibling 链。
	 */

	/*
	 * 把 top 节点改成前缀（top->child，涵盖此前已
	 * 解析部分）与剩余 (t) 的拼接。注意前缀可能为空，
	 * 此时该拼接节点并不必要。为保持简单，现先
	 * 按通用方式操作，稍后去掉不必要的
	 * subre。
	 */
	assert(top->op == '=' && top->child == NULL);
	top->child = subre(v, '=', top->flags, top->begin, lp);
	NOERRN();
	top->op = '.';
	top->child->sibling = t;
	/* top->flags 稍后会更新 */

	/* 若是反向引用，现在该复制 subNFA 了 */
	if (atomtype == BACKREF)
	{
		assert(atom->begin->nouts == 1);	/* 仅 EMPTY */
		delsub(v->nfa, atom->begin, atom->end);
		assert(v->subs[subno] != NULL);

		/*
		 * 而这正是推迟递归的原因：须等骨架填好，
		 * 因其可能碰到想复制已填好骨架的
		 * 反向引用。
		 */
		dupnfa(v->nfa, v->subs[subno]->begin, v->subs[subno]->end,
			   atom->begin, atom->end);
		NOERRN();

		/* 反向引用节点的 NFA 不应施加任何约束 */
		removeconstraints(v->nfa, atom->begin, atom->end);
		NOERRN();
	}

	/*
	 * 轮到量词了。若原子只是反向引用，就让它自行
	 * 内部处理量词。
	 */
	if (atomtype == BACKREF)
	{
		/* 特判：反向引用有内部量词 */
		EMPTYARC(s, atom->begin);	/* 空前缀 */
		/* 把一切都塞进 atom */
		repeat(v, atom->begin, atom->end, m, n);
		atom->min = (short) m;
		atom->max = (short) n;
		atom->flags |= COMBINE(qprefer, atom->flags);
		/* 分支其余可自 atom->end 起串接 */
		s2 = atom->end;
	}
	else if (m == 1 && n == 1 &&
			 (qprefer == 0 ||
			  (atom->flags & (LONGER | SHORTER | MIXED)) == 0 ||
			  qprefer == (atom->flags & (LONGER | SHORTER | MIXED))))
	{
		/* 无/空洞量词：完成 */
		EMPTYARC(s, atom->begin);	/* 空前缀 */
		/* 分支其余可自 atom->end 起串接 */
		s2 = atom->end;
	}
	else if (!(atom->flags & (CAP | BACKR)))
	{
		/*
		 * 若被重复的原子内无捕获也无反向引用，我们
		 * 并不真正关心迭代的子匹配落在何处，故不
		 * 需要迭代节点。改做一个普通 DFA 节点。
		 */
		EMPTYARC(s, atom->begin);	/* 空前缀 */
		repeat(v, atom->begin, atom->end, m, n);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '=', f, atom->begin, atom->end);
		NOERRN();
		freesubre(v, atom);
		*atomp = t;
		/* 分支其余可自 t->end 起串接 */
		s2 = t->end;
	}
	else if (m > 0 && !(atom->flags & BACKR))
	{
		/*
		 * 若不涉及反向引用，可将 x{m,n} 变成 
		 * x{m-1,n-1}x，且仅第二份 x 含捕获括号。这合
		 * 法，因我们只关心量词最后一次迭代的捕获
		 * 匹配。划算之处是：无反向引用的左侧可做成
		 * 普通 DFA 节点，因我们并不真正关心其
		 * 子匹配落在何处。
		 */
		dupnfa(v->nfa, atom->begin, atom->end, s, atom->begin);
		assert(m >= 1 && m != DUPINF && n >= 1);
		repeat(v, s, atom->begin, m - 1, (n == DUPINF) ? n : n - 1);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '.', f, s, atom->end); /* 前缀与原子 */
		NOERRN();
		t->child = subre(v, '=', PREF(f), s, atom->begin);
		NOERRN();
		t->child->sibling = atom;
		*atomp = t;
		/* 分支其余可自 atom->end 起串接 */
		s2 = atom->end;
	}
	else
	{
		/* 一般情形：需要迭代节点 */
		s2 = newstate(v->nfa);
		NOERRN();
		moveouts(v->nfa, atom->end, s2);
		NOERRN();
		dupnfa(v->nfa, atom->begin, atom->end, s, s2);
		repeat(v, s, s2, m, n);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '*', f, s, s2);
		NOERRN();
		t->min = (short) m;
		t->max = (short) n;
		t->child = atom;
		*atomp = t;
		/* 分支其余要从迭代的结束状态串起 */
	}

	/* 最后处理那次推迟的递归 */
	t = top->child->sibling;
	if (!(SEE('|') || SEE(stopper) || SEE(EOS)))
	{
		/* 解析分支其余全部，插入 t->child->sibling */
		t->child->sibling = parsebranch(v, stopper, type, s2, rp, 1);
		NOERRN();
		assert(SEE('|') || SEE(stopper) || SEE(EOS));

		/* 此处兑现承诺的 flags 更新 */
		t->flags |= COMBINE(t->flags, t->child->sibling->flags);
		top->flags |= COMBINE(top->flags, t->flags);

		/* t 与 top 迄今都还不能直接标为捕获 */
		assert(t->capno == 0);
		assert(top->capno == 0);

		/*
		 * 此时 top 与 t 都是拼接（op == '.'）subre，
		 * 且 top->child = 分支前缀，top->child->sibling = t，
		 * t->child = 棘手原子（必要时带量词上层），
		 * t->child->sibling = 分支其余。
		 *
		 * 若棘手原子是分支中的第一项，则
		 * top->child 为空洞，可去掉一层
		 * 拼接。
		 */
		assert(top->child->op == '=');
		if (top->child->begin == top->child->end)
		{
			assert(!MESSY(top->child->flags));
			freesubre(v, top->child);
			top->child = t->child;
			freesrnode(v, t);
		}

		/*
		 * 否则，t->child 本身可能并不棘手，只因
		 * 其贪婪性与先前冲突，我们才视之为棘手。
		 * 那时也可能 t->child 与分支其余的组合同样
		 * 不棘手，此时可通过合并 t->child 与分支其余
		 * 为一个普通 DFA 节点，去掉
		 * 子拼接。
		 */
		else if (t->child->op == '=' &&
				 t->child->sibling->op == '=' &&
				 !MESSY(UP(t->child->flags | t->child->sibling->flags)))
		{
			t->op = '=';
			t->flags = COMBINE(t->child->flags, t->child->sibling->flags);
			freesubreandsiblings(v, t->child);
			t->child = NULL;
		}
	}
	else
	{
		/*
		 * 分支已无剩余，故不需要第二个拼接
		 * 节点 t。直接把 s2 连到 rp。
		 */
		EMPTYARC(s2, rp);
		top->child->sibling = t->child;
		top->flags |= COMBINE(top->flags, top->child->sibling->flags);
		freesrnode(v, t);

		/*
		 * 同样，top->child 也可能为空洞（若棘手原子
		 * 实际上是分支中唯一内容）。那时完全不需要
		 * 拼接；只需用 top->child->sibling 替换 top。
		 */
		assert(top->child->op == '=');
		if (top->child->begin == top->child->end)
		{
			assert(!MESSY(top->child->flags));
			t = top->child->sibling;
			top->child->sibling = NULL;
			freesubre(v, top);
			top = t;
		}
	}

	return top;
}

/*
 * nonword - 为前方或后方非单词字符生成弧
 */
static void
nonword(struct vars *v,
		int dir,				/* AHEAD 或BEHIND */
		struct state *lp,
		struct state *rp)
{
	int			anchor = (dir == AHEAD) ? '$' : '^';

	assert(dir == AHEAD || dir == BEHIND);
	newarc(v->nfa, anchor, 1, lp, rp);
	newarc(v->nfa, anchor, 0, lp, rp);
	colorcomplement(v->nfa, v->cm, dir, v->wordchrs, lp, rp);
	/* 无需特殊照顾 \n */
}

/*
 * word - 为前方或后方单词字符生成弧
 */
static void
word(struct vars *v,
	 int dir,					/* AHEAD 或BEHIND */
	 struct state *lp,
	 struct state *rp)
{
	assert(dir == AHEAD || dir == BEHIND);
	cloneouts(v->nfa, v->wordchrs, lp, rp, dir);
	/* 无需特殊照顾 \n */
}

/*
 * charclass - 为字符类生成弧
 *
 * 用于原子（\w 及同类转义）以及括号表达式
 * 的元素。调用方负责在处理完原子或括号之
 * 末调用 okcolors()。
 */
static void
charclass(struct vars *v,
		  enum char_classes cls,
		  struct state *lp,
		  struct state *rp)
{
	struct cvec *cv;

	/* 取可能已缓存的字符类 cvec */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, cls, (v->cflags & REG_ICASE));
	NOERR();

	/* 建弧；这可能引起色分裂 */
	subcolorcvec(v, cv, lp, rp);
}

/*
 * charclasscomplement - 为补集字符类生成弧
 *
 * 用于原子（\W 及同类转义）以及括号表达式
 * 的元素。在括号表达式中，调用方须保证调用
 * 时不存在任何未关闭的子色。
 */
static void
charclasscomplement(struct vars *v,
					enum char_classes cls,
					struct state *lp,
					struct state *rp)
{
	struct state *cstate;
	struct cvec *cv;

	/* 造哑状态以挂临时弧 */
	cstate = newstate(v->nfa);
	NOERR();

	/* 取可能已缓存的字符类 cvec */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, cls, (v->cflags & REG_ICASE));
	NOERR();

	/* 为字符类建弧；这可能引起色分裂 */
	subcolorcvec(v, cv, cstate, cstate);
	NOERR();

	/* 清理弧集中的任何子色 */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* 现为字符类的补集建输出弧 */
	colorcomplement(v->nfa, v->cm, PLAIN, cstate, lp, rp);
	NOERR();

	/* 清理哑状态 */
	dropstate(v->nfa, cstate);
}

/*
 * scannum - 扫描数
 */
static int						/* 值，<= DUPMAX */
scannum(struct vars *v)
{
	int			n = 0;

	while (SEE(DIGIT) && n < DUPMAX)
	{
		n = n * 10 + v->nextvalue;
		NEXT();
	}
	if (SEE(DIGIT) || n > DUPMAX)
	{
		ERR(REG_BADBR);
		return 0;
	}
	return n;
}

/*
 * repeat - 为量词复制 subNFA 
 *
 * 从 lp 串到 rp 的子 NFA 被改成表示其初始内容
 * 的 m 到 n 次重复。
 *
 * 此处选用的复制序列经过精心选择，使任何起初
 * 指向子表达式内部的指针最终都指向最后一次
 * 出现。（但注意，未必仍串在同一左右端状态
 * 之间！）这对 subRE 树曾经重要，尽管关键部分
 * 现已由 parse() 内联代码处理；调用本函数时
 * 已不再要紧。
 */
static void
repeat(struct vars *v,
	   struct state *lp,
	   struct state *rp,
	   int m,
	   int n)
{
#define  SOME	 2
#define  INF	 3
#define  PAIR(x, y)  ((x)*4 + (y))
#define  REDUCE(x)	 ( ((x) == DUPINF) ? INF : (((x) > 1) ? SOME : (x)) )
	const int	rm = REDUCE(m);
	const int	rn = REDUCE(n);
	struct state *s;
	struct state *s2;

	switch (PAIR(rm, rn))
	{
		case PAIR(0, 0):		/* 空串 */
			delsub(v->nfa, lp, rp);
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, 1):		/* 做 x| */
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, SOME):		/* 做 x{1,n}| */
			repeat(v, lp, rp, 1, n);
			NOERR();
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, INF):		/* 将 x 绕环 */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			moveins(v->nfa, rp, s);
			EMPTYARC(lp, s);
			EMPTYARC(s, rp);
			break;
		case PAIR(1, 1):		/* 无需动作 */
			break;
		case PAIR(1, SOME):		/* 做 x{0,n-1}x=(x{1,n-1}|)x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, 1, n - 1);
			NOERR();
			EMPTYARC(lp, s);
			break;
		case PAIR(1, INF):		/* 加回环弧 */
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			moveins(v->nfa, rp, s2);
			EMPTYARC(lp, s);
			EMPTYARC(s2, rp);
			EMPTYARC(s2, s);
			break;
		case PAIR(SOME, SOME):	/* 做 x{m-1,n-1}x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, m - 1, n - 1);
			break;
		case PAIR(SOME, INF):	/* 做 x{m-1,}x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, m - 1, n);
			break;
		default:
			ERR(REG_ASSERT);
			break;
	}
}

/*
 * bracket - 处理非补集括号表达式
 *
 * 也被 cbracket 调用来处理补集括号表达式。
 */
static void
bracket(struct vars *v,
		struct state *lp,
		struct state *rp)
{
	/*
	 * 扫描括号表达式时无法立即处理补集字符类
	 * 如 \W，否则色记账会乱。改为在
	 * have_cclassc[] 中记下是否见过，并在
	 * 末尾处理。
	 */
	bool		have_cclassc[NUM_CCLASSES];
	bool		any_cclassc;
	int			i;

	memset(have_cclassc, false, sizeof(have_cclassc));

	assert(SEE('['));
	NEXT();
	while (!SEE(']') && !SEE(EOS))
		brackpart(v, lp, rp, have_cclassc);
	assert(SEE(']') || ISERR());

	/* 关闭正括号元素留下的未关闭子色 */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* 现处理任何补集元素 */
	any_cclassc = false;
	for (i = 0; i < NUM_CCLASSES; i++)
	{
		if (have_cclassc[i])
		{
			charclasscomplement(v, (enum char_classes) i, lp, rp);
			NOERR();
			any_cclassc = true;
		}
	}

	/*
	 * 若有任何补集元素，看能否把括号优化成
	 * 彩虹。因补集元素是 WHITE 弧进入结果的
	 * 唯一途径，否则不必检查。
	 */
	if (any_cclassc)
		optimizebracket(v, lp, rp);
}

/*
 * cbracket - 处理取补的括号表达式
 *
 * 做法是用虚端点调用 bracket()，再对结果
 * 取补。另一做法是调用彩虹 rainbow()，再随见到
 * 括号表达式删弧……但那样会很乱，而且现已
 * 不可行，因 rainbow() 只放出一条 RAINBOW 弧。
 */
static void
cbracket(struct vars *v,
		 struct state *lp,
		 struct state *rp)
{
	struct state *left = newstate(v->nfa);
	struct state *right = newstate(v->nfa);

	NOERR();
	bracket(v, left, right);

	/* NLSTOP 模式下确保换行不在结果集中 */
	if (v->cflags & REG_NLSTOP)
		newarc(v->nfa, PLAIN, v->nlcolor, left, right);
	NOERR();

	assert(lp->nouts == 0);		/* 出弧皆属本方 */

	/*
	 * 取补的容易部分，也是 MCCE 代码移除后
	 * 仅需做的事。注意 colorcomplement() 的结果
	 * 不可能是彩虹，因不允许空括号；故无需再
	 * 调用 optimizebracket()。
	 */
	colorcomplement(v->nfa, v->cm, PLAIN, left, lp, rp);
	NOERR();
	dropstate(v->nfa, left);
	assert(right->nins == 0);
	freestate(v->nfa, right);
}

/*
 * brackpart - 处理括号表达式内一项（或范围）
 */
static void
brackpart(struct vars *v,
		  struct state *lp,
		  struct state *rp,
		  bool *have_cclassc)
{
	chr			startc;
	chr			endc;
	struct cvec *cv;
	enum char_classes cls;
	const chr  *startp;
	const chr  *endp;

	/* 解析内容，去掉特例，走捷径 */
	switch (v->nexttype)
	{
		case RANGE:				/* a-b-c 等拙作 */
			ERR(REG_ERANGE);
			return;
			break;
		case PLAIN:
			startc = v->nextvalue;
			NEXT();
			/* 普通字符捷径（非范围） */
			if (!SEE(RANGE))
			{
				onechr(v, startc, lp, rp);
				return;
			}
			NOERR();
			break;
		case COLLEL:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECOLLATE);
			NOERR();
			startc = element(v, startp, endp);
			NOERR();
			break;
		case ECLASS:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECOLLATE);
			NOERR();
			startc = element(v, startp, endp);
			NOERR();
			cv = eclass(v, startc, (v->cflags & REG_ICASE));
			NOERR();
			subcolorcvec(v, cv, lp, rp);
			return;
			break;
		case CCLASS:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECTYPE);
			NOERR();
			cls = lookupcclass(v, startp, endp);
			NOERR();
			charclass(v, cls, lp, rp);
			return;
			break;
		case CCLASSS:
			charclass(v, (enum char_classes) v->nextvalue, lp, rp);
			NEXT();
			return;
			break;
		case CCLASSC:
			/* 无法立即调用 charclasscomplement() */
			have_cclassc[v->nextvalue] = true;
			NEXT();
			return;
			break;
		default:
			ERR(REG_ASSERT);
			return;
			break;
	}

	if (SEE(RANGE))
	{
		NEXT();
		switch (v->nexttype)
		{
			case PLAIN:
			case RANGE:
				endc = v->nextvalue;
				NEXT();
				NOERR();
				break;
			case COLLEL:
				startp = v->now;
				endp = scanplain(v);
				INSIST(startp < endp, REG_ECOLLATE);
				NOERR();
				endc = element(v, startp, endp);
				NOERR();
				break;
			default:
				ERR(REG_ERANGE);
				return;
				break;
		}
	}
	else
		endc = startc;

	/*
	 * 范围不可移植。其实标准 C 保证数字是连续的，
	 * 但把这当例外会太过复杂。
	 */
	if (startc != endc)
		NOTE(REG_UUNPORT);
	cv = range(v, startc, endc, (v->cflags & REG_ICASE));
	NOERR();
	subcolorcvec(v, cv, lp, rp);
}

/*
 * scanplain - 扫 [. 等内 PLAIN 内容
 *
 * regc_lex.c 中若干巧技依赖本代码不会试图
 * 窥视 [. 等最终括号之后。
 */
static const chr *				/* 恰在序列结束之后 */
scanplain(struct vars *v)
{
	const chr  *endp;

	assert(SEE(COLLEL) || SEE(ECLASS) || SEE(CCLASS));
	NEXT();

	endp = v->now;
	while (SEE(PLAIN))
	{
		endp = v->now;
		NEXT();
	}

	assert(SEE(END) || ISERR());
	NEXT();

	return endp;
}

/*
 * onechr - 为普通字符及可能的大小写补填弧
 * 这主要是处理常见情形的高效捷径。
 */
static void
onechr(struct vars *v,
	   chr c,
	   struct state *lp,
	   struct state *rp)
{
	if (!(v->cflags & REG_ICASE))
	{
		color		lastsubcolor = COLORLESS;

		subcoloronechr(v, c, lp, rp, &lastsubcolor);
		return;
	}

	/* 糟，还得走一般情形 */
	subcolorcvec(v, allcases(v, c), lp, rp);
}

/*
 * optimizebracket - 看括号表达式能否化为彩虹
 *
 * 如 "[\s\S]" 可产生覆盖全体色的弧集，我们
 * 可换成单条 RAINBOW 弧以提高效率。（这看来
 * 像写 "." 的蠢办法，但在其它正则风味里似乎
 * 是常见惯用写法，故仍费心予以妥善支持。）
 */
static void
optimizebracket(struct vars *v,
				struct state *lp,
				struct state *rp)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(v->cm);
	struct arc *a;
	bool		israinbow;

	/*
	 * 扫描 lp 的出弧并临时标记所涉色。我们
	 * 预期 lp 的出弧全是通向 rp 的普通非彩虹弧。
	 * 注：此时尚不应有伪色，但仍检查。
	 */
	for (a = lp->outs; a != NULL; a = a->outchain)
	{
		assert(a->type == PLAIN);
		assert(a->co >= 0);		/* 即非彩虹 */
		assert(a->to == rp);
		cd = &v->cm->cd[a->co];
		assert(!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO));
		cd->flags |= COLMARK;
	}

	/* 扫描各色，清临时标记，查未标记的活色 */
	israinbow = true;
	for (cd = v->cm->cd; cd < end; cd++)
	{
		if (cd->flags & COLMARK)
			cd->flags &= ~COLMARK;
		else if (!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO))
			israinbow = false;
	}

	/* 若并非所有色都有弧则无能为力 */
	if (!israinbow)
		return;

	/* 好，丢掉现有弧并换成一条彩虹 */
	while ((a = lp->outs) != NULL)
		freearc(v->nfa, a);
	newarc(v->nfa, PLAIN, RAINBOW, lp, rp);
}

/*
 * wordchrs - 若需要则为词界准备词字符列表
 *
 * 列表以一批环弧挂在一个另无他用的状态上。
 *
 * 注意：存在任何未关闭子色时不可调用，
 * 否则构造该列表会搅乱色簿记。
 * 因此目前无法在 charclass[complement]()
 * 中做类似优化，因那些须能用于括号
 * 表达式。
 */
static void
wordchrs(struct vars *v)
{
	struct state *cstate;
	struct cvec *cv;

	if (v->wordchrs != NULL)
		return;					/* 业已完成 */

	/* 造虚状态以挂接缓存弧 */
	cstate = newstate(v->nfa);
	NOERR();

	/* 取得 \w 字符可能已缓存的 cvec */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, CC_WORD, (v->cflags & REG_ICASE));
	NOERR();

	/* 建弧；这可能引起色分裂 */
	subcolorcvec(v, cv, cstate, cstate);
	NOERR();

	/* 关闭新开的子色，确保缓存项自洽 */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* 成功！保存缓存指针 */
	v->wordchrs = cstate;
}

/*
 * processlacon - 生成 LACON 的 NFA 表示
 *
 * 一般情形只是 newlacon()+newarc()，但有些
 * 可优化。
 */
static void
processlacon(struct vars *v,
			 struct state *begin,	/* 已解析 LACON 子式起点 */
			 struct state *end, /* 已解析 LACON 终点 */
			 int latype,
			 struct state *lp,	/* 挂接用的左状态 */
			 struct state *rp)	/* 挂接用的右状态 */
{
	struct state *s1;
	int			n;

	/*
	 * 检查环视 RE 是否仅由单条普通色弧（或一组
	 * 弧）构成；典型为简单字符或括号表达式。
	 */
	s1 = single_color_transition(begin, end);
	switch (latype)
	{
		case LATYPE_AHEAD_POS:
			/* 若前瞻 RE 恰为色集 C，则化为 AHEAD(C) */
			if (s1 != NULL)
			{
				cloneouts(v->nfa, s1, lp, rp, AHEAD);
				return;
			}
			break;
		case LATYPE_AHEAD_NEG:
			/* 若前瞻 RE 恰为色集 C，则化为 AHEAD(^C)|$ */
			if (s1 != NULL)
			{
				colorcomplement(v->nfa, v->cm, AHEAD, s1, lp, rp);
				newarc(v->nfa, '$', 1, lp, rp);
				newarc(v->nfa, '$', 0, lp, rp);
				return;
			}
			break;
		case LATYPE_BEHIND_POS:
			/* 若后顾 RE 恰为色集 C，则化为 BEHIND(C) */
			if (s1 != NULL)
			{
				cloneouts(v->nfa, s1, lp, rp, BEHIND);
				return;
			}
			break;
		case LATYPE_BEHIND_NEG:
			/* 若后顾 RE 恰为色集 C，则化为 BEHIND(^C)|^ */
			if (s1 != NULL)
			{
				colorcomplement(v->nfa, v->cm, BEHIND, s1, lp, rp);
				newarc(v->nfa, '^', 1, lp, rp);
				newarc(v->nfa, '^', 0, lp, rp);
				return;
			}
			break;
		default:
			assert(NOTREACHED);
	}

	/* 一般情形：需要 LACON subre 与弧 */
	n = newlacon(v, begin, end, latype);
	newarc(v->nfa, LACON, n, lp, rp);
}

/*
 * subre - 分配 subre
 */
static struct subre *
subre(struct vars *v,
	  int op,
	  int flags,
	  struct state *begin,
	  struct state *end)
{
	struct subre *ret = v->treefree;

	/*
	 * 此处检查栈溢出足以保护 parse() 及其
	 * 递归子例程。
	 */
	if (STACK_TOO_DEEP(v->re))
	{
		ERR(REG_ETOOBIG);
		return NULL;
	}

	if (ret != NULL)
		v->treefree = ret->child;
	else
	{
		ret = (struct subre *) MALLOC(sizeof(struct subre));
		if (ret == NULL)
		{
			ERR(REG_ESPACE);
			return NULL;
		}
		ret->chain = v->treechain;
		v->treechain = ret;
	}

	assert(strchr("=b|.*(", op) != NULL);

	ret->op = op;
	ret->flags = flags;
	ret->latype = (char) -1;
	ret->id = 0;				/* 稍后才会赋值 */
	ret->capno = 0;
	ret->backno = 0;
	ret->min = ret->max = 1;
	ret->child = NULL;
	ret->sibling = NULL;
	ret->begin = begin;
	ret->end = end;
	ZAPCNFA(ret->cnfa);

	return ret;
}

/*
 * freesubre - 释放 subRE 子树
 *
 * 同时释放该 subRE 的子节点，
 * 但不含其兄弟。
 */
static void
freesubre(struct vars *v,		/* 可为 NULL */
		  struct subre *sr)
{
	if (sr == NULL)
		return;

	if (sr->child != NULL)
		freesubreandsiblings(v, sr->child);

	freesrnode(v, sr);
}

/*
 * freesubreandsiblings - 释放 subRE 子树
 *
 * 同时释放该 subRE 的子节点，
 * 以及其后的任何兄弟。
 */
static void
freesubreandsiblings(struct vars *v,	/* 可为 NULL */
					 struct subre *sr)
{
	while (sr != NULL)
	{
		struct subre *next = sr->sibling;

		freesubre(v, sr);
		sr = next;
	}
}

/*
 * freesrnode - 释放 subRE 子树中一节点
 */
static void
freesrnode(struct vars *v,		/* 可为 NULL */
		   struct subre *sr)
{
	if (sr == NULL)
		return;

	if (!NULLCNFA(sr->cnfa))
		freecnfa(&sr->cnfa);
	sr->flags = 0;				/* 尤其是，非 INUSE */
	sr->child = sr->sibling = NULL;
	sr->begin = sr->end = NULL;

	if (v != NULL && v->treechain != NULL)
	{
		/* 仍在解析，或许能复用该 subre */
		sr->child = v->treefree;
		v->treefree = sr;
	}
	else
		FREE(sr);
}

/*
 * removecaptures - 去掉不必要的捕获 subRE
 *
 * 若调用方表示不关心子表达式匹配数据，
 * 则可删去无反向引用指向的 subRE 上的
 * 捕获标记，再简化已变得非杂乱者。
 * 仅在置 REG_NOSUB 时调用。
 */
static void
removecaptures(struct vars *v,
			   struct subre *t)
{
	struct subre *t2;

	assert(t != NULL);

	/*
	 * 若本身不是反向引用目标，则清 capno 并暂且
	 * 清除 CAP。
	 */
	if (!(t->flags & BRUSE))
	{
		t->capno = 0;
		t->flags &= ~CAP;
	}

	/* 再递归到子节点 */
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
	{
		removecaptures(v, t2);
		/* 若子节点 CAP 仍置位则向上回传 */
		if (t2->flags & CAP)
			t->flags |= CAP;
	}

	/*
	 * 若 t 现既无捕获也无反向引用，便不必再关心
	 * 其子匹配边界在何处，因而可把它收成
	 * 简单 DFA 节点。（尤须注意 MIXED 子贪婪性
	 * 在此无妨，故不用 MESSY() 宏。）
	 */
	if ((t->flags & (CAP | BACKR)) == 0)
	{
		if (t->child)
			freesubreandsiblings(v, t->child);
		t->child = NULL;
		t->op = '=';
		t->flags &= ~MIXED;
	}
}

/*
 * numst - 给树节点编号（赋 "id" 下标）
 */
static int						/* 下一号 */
numst(struct subre *t,
	  int start)				/* 子树编号的起点 */
{
	int			i;
	struct subre *t2;

	assert(t != NULL);

	i = start;
	t->id = i++;
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		i = numst(t2, i);
	return i;
}

/*
 * markst - 将树节点标为 INUSE
 *
 * 注意：这远比看起来微妙。在正则的初始
 * 解析期间，全部 subre 都链入 treechain 表；
 * 丢弃者也链入 treefree 表以供可能复用。
 * 为正则建完所需全部 subre 后，先跑 markst()
 * 再跑 cleanst()，结果丢弃一切自 v->tree
 * 不可达的 subre。随后清 v->treechain，表示
 * 须自 v->tree 下行查找 subre。这改变了
 * freesubre() 的行为：此后对无用 subre 做 FREE()，
 * 而不再放入 treefree 表。（若更早这么做会在
 * treechain 表中留下悬空链。）这一切意味着
 * freev() 在 markst()+cleanst() 前或后调用均可正确清理；但
 * 若在此状态转换中途调用则不行，故不可于
 * 此二函数内或之间报错。
 */
static void
markst(struct subre *t)
{
	struct subre *t2;

	assert(t != NULL);

	t->flags |= INUSE;
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		markst(t2);
}

/*
 * cleanst - 释放未标 INUSE 的树节点
 */
static void
cleanst(struct vars *v)
{
	struct subre *t;
	struct subre *next;

	for (t = v->treechain; t != NULL; t = next)
	{
		next = t->chain;
		if (!(t->flags & INUSE))
			FREE(t);
	}
	v->treechain = NULL;
	v->treefree = NULL;			/* 仅出于一般原则 */
}

/*
 * nfatree - 把 subRE 子树变成紧凑 NFA 树
 */
static long						/* 顶节点的优化结果 */
nfatree(struct vars *v,
		struct subre *t,
		FILE *f)				/* 供调试输出 */
{
	struct subre *t2;

	assert(t != NULL && t->begin != NULL);

	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		(DISCARD) nfatree(v, t2, f);

	return nfanode(v, t, 0, f);
}

/*
 * nfanode - 为 nfatree/LACON 做一 NFA
 *
 * converttosearch 为真时对 NFA 做 makesearch()。
 */
static long						/* 优化结果 */
nfanode(struct vars *v,
		struct subre *t,
		int converttosearch,
		FILE *f)				/* 供调试输出 */
{
	struct nfa *nfa;
	long		ret = 0;

	assert(t->begin != NULL);

#ifdef REG_DEBUG
	if (f != NULL)
	{
		char		idbuf[50];

		fprintf(f, "\n\n\n========= TREE NODE %s ==========\n",
				stid(t, idbuf, sizeof(idbuf)));
	}
#endif
	nfa = newnfa(v, v->cm, v->nfa);
	NOERRZ();
	dupnfa(nfa, t->begin, t->end, nfa->init, nfa->final);
	nfa->flags = v->nfa->flags;
	if (!ISERR())
		specialcolors(nfa);
	if (!ISERR())
		ret = optimize(nfa, f);
	if (converttosearch && !ISERR())
		makesearch(v, nfa);
	if (!ISERR())
		compact(nfa, &t->cnfa);

	freenfa(nfa);
	return ret;
}

/*
 * newlacon - 分配 LACON（环视约束）subRE
 */
static int						/* LACON 编号 */
newlacon(struct vars *v,
		 struct state *begin,
		 struct state *end,
		 int latype)
{
	int			n;
	struct subre *newlacons;
	struct subre *sub;

	if (v->nlacons == 0)
	{
		n = 1;					/* 跳过0 */
		newlacons = (struct subre *) MALLOC(2 * sizeof(struct subre));
	}
	else
	{
		n = v->nlacons;
		newlacons = (struct subre *) REALLOC(v->lacons,
											 (n + 1) * sizeof(struct subre));
	}
	if (newlacons == NULL)
	{
		ERR(REG_ESPACE);
		return 0;
	}
	v->lacons = newlacons;
	v->nlacons = n + 1;
	sub = &v->lacons[n];
	sub->begin = begin;
	sub->end = end;
	sub->latype = latype;
	ZAPCNFA(sub->cnfa);
	return n;
}

/*
 * freelacons - 释放 LACON（环视约束）向量
 */
static void
freelacons(struct subre *subs,
		   int n)
{
	struct subre *sub;
	int			i;

	assert(n > 0);
	for (sub = subs + 1, i = n - 1; i > 0; sub++, i--)	/* 无0 */
		if (!NULLCNFA(sub->cnfa))
			freecnfa(&sub->cnfa);
	FREE(subs);
}

/*
 * rfree - 释放整个 RE（regfree 内部）
 */
static void
rfree(regex_t *re)
{
	struct guts *g;

	if (re == NULL || re->re_magic != REMAGIC)
		return;

	re->re_magic = 0;			/* 使 RE 失效 */
	g = (struct guts *) re->re_guts;
	re->re_guts = NULL;
	re->re_fns = NULL;
	if (g != NULL)
	{
		g->magic = 0;
		freecm(&g->cmap);
		if (g->tree != NULL)
			freesubre((struct vars *) NULL, g->tree);
		if (g->lacons != NULL)
			freelacons(g->lacons, g->nlacons);
		if (!NULLCNFA(g->search))
			freecnfa(&g->search);
		FREE(g);
	}
}

/*
 * rstacktoodeep - 检查栈是否深得危险
 *
 * 返回非零则以错误码 REG_ETOOBIG 使操作失败，
 * 零则继续
 *
 * 当前实现专属于 Postgres。若有朝一日把
 * 正则代码拆成独立库，将需要某种 API 让
 * 应用程序为此定义回调函数。
 */
static int
rstacktoodeep(void)
{
	return stack_is_too_deep();
}

#ifdef REG_DEBUG

/*
 * dump - 以人类可读形式转储 RE
 */
static void
dump(regex_t *re,
	 FILE *f)
{
	struct guts *g;
	int			i;

	if (re->re_magic != REMAGIC)
		fprintf(f, "bad magic number (0x%x not 0x%x)\n", re->re_magic,
				REMAGIC);
	if (re->re_guts == NULL)
	{
		fprintf(f, "NULL guts!!!\n");
		return;
	}
	g = (struct guts *) re->re_guts;
	if (g->magic != GUTSMAGIC)
		fprintf(f, "bad guts magic number (0x%x not 0x%x)\n", g->magic,
				GUTSMAGIC);

	fprintf(f, "\n\n\n========= DUMP ==========\n");
	fprintf(f, "nsub %d, info 0%lo, csize %d, ntree %d\n",
			(int) re->re_nsub, re->re_info, re->re_csize, g->ntree);

	dumpcolors(&g->cmap, f);
	if (!NULLCNFA(g->search))
	{
		fprintf(f, "\nsearch:\n");
		dumpcnfa(&g->search, f);
	}
	for (i = 1; i < g->nlacons; i++)
	{
		struct subre *lasub = &g->lacons[i];
		const char *latype;

		switch (lasub->latype)
		{
			case LATYPE_AHEAD_POS:
				latype = "positive lookahead";
				break;
			case LATYPE_AHEAD_NEG:
				latype = "negative lookahead";
				break;
			case LATYPE_BEHIND_POS:
				latype = "positive lookbehind";
				break;
			case LATYPE_BEHIND_NEG:
				latype = "negative lookbehind";
				break;
			default:
				latype = "???";
				break;
		}
		fprintf(f, "\nla%d (%s):\n", i, latype);
		dumpcnfa(&lasub->cnfa, f);
	}
	fprintf(f, "\n");
	dumpst(g->tree, f, 0);
}

/*
 * dumpst - 转储 subRE 树
 */
static void
dumpst(struct subre *t,
	   FILE *f,
	   int nfapresent)			/* 原来的 NFA 是否还在？ */
{
	if (t == NULL)
		fprintf(f, "null tree\n");
	else
		stdump(t, f, nfapresent);
	fflush(f);
}

/*
 * stdump - dumpst 的递归内核
 */
static void
stdump(struct subre *t,
	   FILE *f,
	   int nfapresent)			/* 原来的 NFA 是否还在？ */
{
	char		idbuf[50];
	struct subre *t2;

	fprintf(f, "%s. `%c'", stid(t, idbuf, sizeof(idbuf)), t->op);
	if (t->flags & LONGER)
		fprintf(f, " longest");
	if (t->flags & SHORTER)
		fprintf(f, " shortest");
	if (t->flags & MIXED)
		fprintf(f, " hasmixed");
	if (t->flags & CAP)
		fprintf(f, " hascapture");
	if (t->flags & BACKR)
		fprintf(f, " hasbackref");
	if (t->flags & BRUSE)
		fprintf(f, " isreferenced");
	if (!(t->flags & INUSE))
		fprintf(f, " UNUSED");
	if (t->latype != (char) -1)
		fprintf(f, " latype(%d)", t->latype);
	if (t->capno != 0)
		fprintf(f, " capture(%d)", t->capno);
	if (t->backno != 0)
		fprintf(f, " backref(%d)", t->backno);
	if (t->min != 1 || t->max != 1)
	{
		fprintf(f, " {%d,", t->min);
		if (t->max != DUPINF)
			fprintf(f, "%d", t->max);
		fprintf(f, "}");
	}
	if (nfapresent)
		fprintf(f, " %ld-%ld", (long) t->begin->no, (long) t->end->no);
	if (t->child != NULL)
		fprintf(f, " C:%s", stid(t->child, idbuf, sizeof(idbuf)));
	/* 打印第二子节点非必需，但往往有帮助 */
	if (t->child != NULL && t->child->sibling != NULL)
		fprintf(f, " C2:%s", stid(t->child->sibling, idbuf, sizeof(idbuf)));
	if (t->sibling != NULL)
		fprintf(f, " S:%s", stid(t->sibling, idbuf, sizeof(idbuf)));
	if (!NULLCNFA(t->cnfa))
	{
		fprintf(f, "\n");
		dumpcnfa(&t->cnfa, f);
	}
	fprintf(f, "\n");
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		stdump(t2, f, nfapresent);
}

/*
 * stid - 为转储标识一个子树节点
 */
static const char *				/* 指向 buf 或常串 */
stid(struct subre *t,
	 char *buf,
	 size_t bufsize)
{
	/* 够装十六进制或十进制 id？ */
	if (bufsize < sizeof(void *) * 2 + 3 || bufsize < sizeof(t->id) * 3 + 1)
		return "unable";
	if (t->id != 0)
		sprintf(buf, "%d", t->id);
	else
		sprintf(buf, "%p", t);
	return buf;
}
#endif							/* REG_DEBUG */


#include "regc_lex.c"
#include "regc_color.c"
#include "regc_nfa.c"
#include "regc_cvec.c"
#include "regc_pg_locale.c"
#include "regc_locale.c"
