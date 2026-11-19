/*
 * re_*exec 诸例程 - 匹配 RE
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
 * src/backend/regex/regexec.c
 *
 */

#include "regex/regguts.h"



/* 惰性 DFA 表示 */
struct arcp
{								/* 出弧的"指针" */
	struct sset *ss;
	color		co;
};

struct sset
{								/* 状态集 */
	unsigned   *states;			/* 位向量指针 */
	unsigned	hash;			/* 位向量散列 */
#define  HASH(bv, nw)	 (((nw) == 1) ? *(bv) : hash(bv, nw))
#define  HIT(h,bv,ss,nw) ((ss)->hash == (h) && ((nw) == 1 || \
		memcmp(VS(bv), VS((ss)->states), (nw)*sizeof(unsigned)) == 0))
	int			flags;
#define  STARTER	 01			/* 初始状态集 */
#define  POSTSTATE	 02			/* 含目标状态 */
#define  LOCKED		 04			/* 已锁入缓存 */
#define  NOPROGRESS  010		/* 零进展状态集 */
	struct arcp ins;			/* 指向此处的入弧链 */
	chr		   *lastseen;		/* 抵此时最后进入位置 */
	struct sset **outs;			/* 按色索引的出弧向量 */
	struct arcp *inchain;		/* 出弧的链指针向量 */
};

struct dfa
{
	int			nssets;			/* 缓存大小 */
	int			nssused;		/* 已占用条目数 */
	int			nstates;		/* 状态数 */
	int			ncolors;		/* 出弧与 inchain 向量长度 */
	int			wordsper;		/* 状态集位向量长度 */
	struct sset *ssets;			/* 状态集缓存 */
	unsigned   *statesarea;		/* 位向量存储 */
	unsigned   *work;			/* statesarea 内工作区指针 */
	struct sset **outsarea;		/* 出弧向量存储 */
	struct arcp *incarea;		/* inchain 存储 */
	struct cnfa *cnfa;
	struct colormap *cm;
	chr		   *lastpost;		/* 上次缓存冲刷成功的位置 */
	chr		   *lastnopr;		/* 上次缓存冲刷 NOPROGRESS 的位置 */
	struct sset *search;		/* 替换搜索指针的记忆 */
	int			backno;			/* 若为反向引用 DFA，其所指 subno */
	short		backmin;		/* 反向引用最小重复 */
	short		backmax;		/* 反向引用最大重复 */
	bool		ismalloced;		/* 是否应释放此 struct DFA？ */
	bool		arraysmalloced; /* 是否应释放其附属数组？ */
};

#define WORK	1				/* 所需工作位向量数 */

/* 小情形非 malloc 分配的设置 */
#define FEWSTATES	20			/* 须小于 UBITS */
#define FEWCOLORS	15
struct smalldfa
{
	struct dfa	dfa;			/* 必须为首 */
	struct sset ssets[FEWSTATES * 2];
	unsigned	statesarea[FEWSTATES * 2 + WORK];
	struct sset *outsarea[FEWSTATES * 2 * FEWCOLORS];
	struct arcp incarea[FEWSTATES * 2 * FEWCOLORS];
};

#define DOMALLOC	((struct smalldfa *)NULL)	/* 须 malloc */



/* 内部变量，捆在一起便于传递 */
struct vars
{
	regex_t    *re;
	struct guts *g;
	int			eflags;			/* 参数副本 */
	size_t		nmatch;
	regmatch_t *pmatch;
	rm_detail_t *details;
	chr		   *start;			/* 字符串始 */
	chr		   *search_start;	/* 串的搜索起点 */
	chr		   *stop;			/* 刚过字符串末 */
	int			err;			/* 错误码（无则0） */
	struct dfa **subdfas;		/* 每树subre之DFA */
	struct dfa **ladfas;		/* 每lacon-subre DFA */
	struct sset **lblastcss;	/* 每 lacon-subre 的后顾重启数据 */
	chr		  **lblastcp;		/* 每 lacon-subre 的后顾重启数据 */
	struct smalldfa dfa1;
	struct smalldfa dfa2;
};

#define VISERR(vv)	((vv)->err != 0)	/* 是否已见错误？ */
#define ISERR() VISERR(v)
#define VERR(vv,e)	((vv)->err = ((vv)->err ? (vv)->err : (e)))
#define ERR(e)	VERR(v, e)		/* 记录错误 */
#define NOERR() {if (ISERR()) return v->err;}	/* 若已见错则返回之 */
#define OFF(p)	((p) - v->start)
#define LOFF(p) ((long)OFF(p))



/*
 * 前向声明
 */
/* === regexec.c === */
static struct dfa *getsubdfa(struct vars *v, struct subre *t);
static struct dfa *getladfa(struct vars *v, int n);
static int	find(struct vars *v, struct cnfa *cnfa, struct colormap *cm);
static int	cfind(struct vars *v, struct cnfa *cnfa, struct colormap *cm);
static int	cfindloop(struct vars *v, struct cnfa *cnfa, struct colormap *cm,
					  struct dfa *d, struct dfa *s, chr **coldp);
static void zapallsubs(regmatch_t *p, size_t n);
static void zaptreesubs(struct vars *v, struct subre *t);
static void subset(struct vars *v, struct subre *sub, chr *begin, chr *end);
static int	cdissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	ccondissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	crevcondissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	cbrdissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	caltdissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	citerdissect(struct vars *v, struct subre *t, chr *begin, chr *end);
static int	creviterdissect(struct vars *v, struct subre *t, chr *begin, chr *end);

/* === rege_dfa.c === */
static chr *longest(struct vars *v, struct dfa *d,
					chr *start, chr *stop, int *hitstopp);
static chr *shortest(struct vars *v, struct dfa *d, chr *start, chr *min,
					 chr *max, chr **coldp, int *hitstopp);
static int	matchuntil(struct vars *v, struct dfa *d, chr *probe,
					   struct sset **lastcss, chr **lastcp);
static chr *dfa_backref(struct vars *v, struct dfa *d, chr *start,
						chr *min, chr *max, bool shortest);
static chr *lastcold(struct vars *v, struct dfa *d);
static struct dfa *newdfa(struct vars *v, struct cnfa *cnfa,
						  struct colormap *cm, struct smalldfa *sml);
static void freedfa(struct dfa *d);
static unsigned hash(unsigned *uv, int n);
static struct sset *initialize(struct vars *v, struct dfa *d, chr *start);
static struct sset *miss(struct vars *v, struct dfa *d, struct sset *css,
						 color co, chr *cp, chr *start);
static int	lacon(struct vars *v, struct cnfa *pcnfa, chr *cp, color co);
static struct sset *getvacant(struct vars *v, struct dfa *d, chr *cp,
							  chr *start);
static struct sset *pickss(struct vars *v, struct dfa *d, chr *cp,
						   chr *start);


/*
 * pg_regexec - 匹配正则表达式
 */
int
pg_regexec(regex_t *re,
		   const chr *string,
		   size_t len,
		   size_t search_start,
		   rm_detail_t *details,
		   size_t nmatch,
		   regmatch_t pmatch[],
		   int flags)
{
	struct vars var;
	struct vars *v = &var;
	int			st;
	size_t		n;
	size_t		i;
	int			backref;

#define  LOCALMAT	 20
	regmatch_t	mat[LOCALMAT];

#define  LOCALDFAS	 40
	struct dfa *subdfas[LOCALDFAS];

	/* 健全检查 */
	if (re == NULL || string == NULL || re->re_magic != REMAGIC)
		return REG_INVARG;
	if (re->re_csize != sizeof(chr))
		return REG_MIXED;
	if (search_start > len)
		return REG_NOMATCH;

	/* 初始化区域设置相关支持 */
	pg_set_regex_collation(re->re_collation);

	/* init */
	v->re = re;
	v->g = (struct guts *) re->re_guts;
	if ((v->g->cflags & REG_EXPECT) && details == NULL)
		return REG_INVARG;
	if (v->g->info & REG_UIMPOSSIBLE)
		return REG_NOMATCH;
	backref = (v->g->info & REG_UBACKREF) ? 1 : 0;
	v->eflags = flags;
	if (backref && nmatch <= v->g->nsub)
	{
		/* 需更大工作区 */
		v->nmatch = v->g->nsub + 1;
		if (v->nmatch <= LOCALMAT)
			v->pmatch = mat;
		else
			v->pmatch = (regmatch_t *) MALLOC(v->nmatch * sizeof(regmatch_t));
		if (v->pmatch == NULL)
			return REG_ESPACE;
		zapallsubs(v->pmatch, v->nmatch);
	}
	else
	{
		/* 结果可直接存入调用方的数组 */
		v->pmatch = pmatch;
		/* 确保调用方数组的多余项填为 -1 */
		if (nmatch > 0)
			zapallsubs(pmatch, nmatch);
		/* 然后忘掉多余项，以免 find() 做无用功 */
		if (nmatch > v->g->nsub + 1)
			nmatch = v->g->nsub + 1;
		v->nmatch = nmatch;
	}
	v->details = details;
	v->start = (chr *) string;
	v->search_start = (chr *) string + search_start;
	v->stop = (chr *) string + len;
	v->err = 0;
	v->subdfas = NULL;
	v->ladfas = NULL;
	v->lblastcss = NULL;
	v->lblastcp = NULL;
	/* 此后清理出口将行为正常 */

	assert(v->g->ntree >= 0);
	n = (size_t) v->g->ntree;
	if (n <= LOCALDFAS)
		v->subdfas = subdfas;
	else
	{
		v->subdfas = (struct dfa **) MALLOC(n * sizeof(struct dfa *));
		if (v->subdfas == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
	}
	for (i = 0; i < n; i++)
		v->subdfas[i] = NULL;

	assert(v->g->nlacons >= 0);
	n = (size_t) v->g->nlacons;
	if (n > 0)
	{
		v->ladfas = (struct dfa **) MALLOC(n * sizeof(struct dfa *));
		if (v->ladfas == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
		for (i = 0; i < n; i++)
			v->ladfas[i] = NULL;
		v->lblastcss = (struct sset **) MALLOC(n * sizeof(struct sset *));
		v->lblastcp = (chr **) MALLOC(n * sizeof(chr *));
		if (v->lblastcss == NULL || v->lblastcp == NULL)
		{
			st = REG_ESPACE;
			goto cleanup;
		}
		for (i = 0; i < n; i++)
		{
			v->lblastcss[i] = NULL;
			v->lblastcp[i] = NULL;
		}
	}

	/* run */
	assert(v->g->tree != NULL);
	if (backref)
		st = cfind(v, &v->g->tree->cnfa, &v->g->cmap);
	else
		st = find(v, &v->g->tree->cnfa, &v->g->cmap);

	/* 成功时确保调用方的匹配向量填正确 */
	if (st == REG_OKAY && nmatch > 0)
	{
		if (v->pmatch != pmatch)
		{
			/* 从（更大）工作区拷贝匹配向量的片段 */
			assert(nmatch <= v->nmatch);
			memcpy(VS(pmatch), VS(v->pmatch), nmatch * sizeof(regmatch_t));
		}
		if (v->g->cflags & REG_NOSUB)
		{
			/* 勿向调用方暴露可能不完整的子匹配结果 */
			zapallsubs(pmatch, nmatch);
		}
	}

	/* 清理 */
cleanup:
	if (v->pmatch != pmatch && v->pmatch != mat)
		FREE(v->pmatch);
	if (v->subdfas != NULL)
	{
		n = (size_t) v->g->ntree;
		for (i = 0; i < n; i++)
		{
			if (v->subdfas[i] != NULL)
				freedfa(v->subdfas[i]);
		}
		if (v->subdfas != subdfas)
			FREE(v->subdfas);
	}
	if (v->ladfas != NULL)
	{
		n = (size_t) v->g->nlacons;
		for (i = 0; i < n; i++)
		{
			if (v->ladfas[i] != NULL)
				freedfa(v->ladfas[i]);
		}
		FREE(v->ladfas);
	}
	if (v->lblastcss != NULL)
		FREE(v->lblastcss);
	if (v->lblastcp != NULL)
		FREE(v->lblastcp);

#ifdef REG_DEBUG
	if (v->eflags & (REG_FTRACE | REG_MTRACE))
		fflush(stdout);
#endif

	return st;
}

/*
 * getsubdfa - 创建或重取树 subre 节点的 DFA
 *
 * 整体正则执行期间只需创建一次 DFA。
 * 该 DFA 由 pg_regexec() 的清理步骤释放。
 */
static struct dfa *
getsubdfa(struct vars *v,
		  struct subre *t)
{
	struct dfa *d = v->subdfas[t->id];

	if (d == NULL)
	{
		d = newdfa(v, &t->cnfa, &v->g->cmap, DOMALLOC);
		if (d == NULL)
			return NULL;
		/* 若是反向引用节点则设附加信息 */
		if (t->op == 'b')
		{
			d->backno = t->backno;
			d->backmin = t->min;
			d->backmax = t->max;
		}
		v->subdfas[t->id] = d;
	}
	return d;
}

/*
 * getladfa - 创建或重取 LACON subre 节点的 DFA
 *
 * 同上，但针对 LACON。
 */
static struct dfa *
getladfa(struct vars *v,
		 int n)
{
	assert(n > 0 && n < v->g->nlacons && v->g->lacons != NULL);

	if (v->ladfas[n] == NULL)
	{
		struct subre *sub = &v->g->lacons[n];

		v->ladfas[n] = newdfa(v, &sub->cnfa, &v->g->cmap, DOMALLOC);
		/* LACON 无法含反向引用，故无他事 */
	}
	return v->ladfas[n];
}

/*
 * find - 为主 NFA 找匹配（无并发症情形）
 */
static int
find(struct vars *v,
	 struct cnfa *cnfa,
	 struct colormap *cm)
{
	struct dfa *s;
	struct dfa *d;
	chr		   *begin;
	chr		   *end = NULL;
	chr		   *cold;
	chr		   *open;			/* 可能起点范围的开与闭 */
	chr		   *close;
	int			hitend;
	int			shorter = (v->g->tree->flags & SHORTER) ? 1 : 0;

	/* 先用搜索 RE 试一枪 */
	s = newdfa(v, &v->g->search, cm, &v->dfa1);
	if (s == NULL)
		return v->err;
	MDEBUG(("\nsearch at %ld\n", LOFF(v->start)));
	cold = NULL;
	close = shortest(v, s, v->search_start, v->search_start, v->stop,
					 &cold, (int *) NULL);
	freedfa(s);
	NOERR();
	if (v->g->cflags & REG_EXPECT)
	{
		assert(v->details != NULL);
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop); /* 未知 */
	}
	if (close == NULL)			/* 未找到 */
		return REG_NOMATCH;
	if (v->nmatch == 0)			/* 已找到，无需精确位置 */
		return REG_OKAY;

	/* 找起点并匹配 */
	assert(cold != NULL);
	open = cold;
	cold = NULL;
	MDEBUG(("between %ld and %ld\n", LOFF(open), LOFF(close)));
	d = newdfa(v, cnfa, cm, &v->dfa1);
	if (d == NULL)
		return v->err;
	for (begin = open; begin <= close; begin++)
	{
		MDEBUG(("\nfind trying at %ld\n", LOFF(begin)));
		if (shorter)
			end = shortest(v, d, begin, begin, v->stop,
						   (chr **) NULL, &hitend);
		else
			end = longest(v, d, begin, v->stop, &hitend);
		if (ISERR())
		{
			freedfa(d);
			return v->err;
		}
		if (hitend && cold == NULL)
			cold = begin;
		if (end != NULL)
			break;				/* 注意跳出 */
	}
	assert(end != NULL);		/* 搜索 RE 已成功故循环应 */
	freedfa(d);

	/* 并落实细节 */
	assert(v->nmatch > 0);
	v->pmatch[0].rm_so = OFF(begin);
	v->pmatch[0].rm_eo = OFF(end);
	if (v->g->cflags & REG_EXPECT)
	{
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop); /* 未知 */
	}
	if (v->nmatch == 1)			/* 无需子匹配 */
		return REG_OKAY;

	/* 找子匹配 */
	return cdissect(v, v->g->tree, begin, end);
}

/*
 * cfind - 为主 NFA 找匹配（有并发症）
 */
static int
cfind(struct vars *v,
	  struct cnfa *cnfa,
	  struct colormap *cm)
{
	struct dfa *s;
	struct dfa *d;
	chr		   *cold;
	int			ret;

	s = newdfa(v, &v->g->search, cm, &v->dfa1);
	if (s == NULL)
		return v->err;
	d = newdfa(v, cnfa, cm, &v->dfa2);
	if (d == NULL)
	{
		freedfa(s);
		return v->err;
	}

	ret = cfindloop(v, cnfa, cm, d, s, &cold);

	freedfa(d);
	freedfa(s);
	NOERR();
	if (v->g->cflags & REG_EXPECT)
	{
		assert(v->details != NULL);
		if (cold != NULL)
			v->details->rm_extend.rm_so = OFF(cold);
		else
			v->details->rm_extend.rm_so = OFF(v->stop);
		v->details->rm_extend.rm_eo = OFF(v->stop); /* 未知 */
	}
	return ret;
}

/*
 * cfindloop - cfind 之心
 */
static int
cfindloop(struct vars *v,
		  struct cnfa *cnfa,
		  struct colormap *cm,
		  struct dfa *d,
		  struct dfa *s,
		  chr **coldp)			/* 存放 coldstart 指针之处 */
{
	chr		   *begin;
	chr		   *end;
	chr		   *cold;
	chr		   *open;			/* 可能起点范围的开与闭 */
	chr		   *close;
	chr		   *estart;
	chr		   *estop;
	int			er;
	int			shorter = v->g->tree->flags & SHORTER;
	int			hitend;

	assert(d != NULL && s != NULL);
	cold = NULL;
	close = v->search_start;
	do
	{
		/* 用搜索 RE 在 "close" 及之后找匹配范围 */
		MDEBUG(("\ncsearch at %ld\n", LOFF(close)));
		close = shortest(v, s, close, close, v->stop, &cold, (int *) NULL);
		if (ISERR())
		{
			*coldp = cold;
			return v->err;
		}
		if (close == NULL)
			break;				/* 再无任何可能匹配 */
		assert(cold != NULL);
		open = cold;
		cold = NULL;
		/* 在 "open" 与 "close" 含端间搜索起点匹配 */
		MDEBUG(("cbetween %ld and %ld\n", LOFF(open), LOFF(close)));
		for (begin = open; begin <= close; begin++)
		{
			MDEBUG(("\ncfind trying at %ld\n", LOFF(begin)));
			estart = begin;
			estop = v->stop;
			for (;;)
			{
				/* 此处我们用顶节点的详细 RE */
				if (shorter)
					end = shortest(v, d, begin, estart,
								   estop, (chr **) NULL, &hitend);
				else
					end = longest(v, d, begin, estop,
								  &hitend);
				if (ISERR())
				{
					*coldp = cold;
					return v->err;
				}
				if (hitend && cold == NULL)
					cold = begin;
				if (end == NULL)
					break;		/* 此起点无匹配，试下一 */
				MDEBUG(("tentative end %ld\n", LOFF(end)));
				/* 剖析该潜在匹配以看是否真匹配 */
				er = cdissect(v, v->g->tree, begin, end);
				if (er == REG_OKAY)
				{
					if (v->nmatch > 0)
					{
						v->pmatch[0].rm_so = OFF(begin);
						v->pmatch[0].rm_eo = OFF(end);
					}
					*coldp = cold;
					return REG_OKAY;
				}
				if (er != REG_NOMATCH)
				{
					ERR(er);
					*coldp = cold;
					return er;
				}
				/* 同起点试下一更长/更短匹配 */
				if (shorter)
				{
					if (end == estop)
						break;	/* 没有了，故试下一起点 */
					estart = end + 1;
				}
				else
				{
					if (end == begin)
						break;	/* 没有了，故试下一起点 */
					estop = end - 1;
				}
			}					/* 终点位置循环结束 */
		}						/* 起点位置循环结束 */

		/*
		 * 若到此，则在 "close" 及之前无任何可能
		 * 起点的匹配，故考虑其后之匹配。我们将用搜索
		 * RE 做一次新搜索以找新的有望匹配范围。
		 */
		close++;
	} while (close < v->stop);

	*coldp = cold;
	return REG_NOMATCH;
}

/*
 * zapallsubs - 将全部子表达式匹配置为"无匹配"
 *
 * 注意 p[0]（整体匹配位置）不被改动。
 */
static void
zapallsubs(regmatch_t *p,
		   size_t n)
{
	size_t		i;

	for (i = n - 1; i > 0; i--)
	{
		p[i].rm_so = -1;
		p[i].rm_eo = -1;
	}
}

/*
 * zaptreesubs - 将子树内子表达式初始化为"无匹配"
 */
static void
zaptreesubs(struct vars *v,
			struct subre *t)
{
	int			n = t->capno;
	struct subre *t2;

	if (n > 0)
	{
		if ((size_t) n < v->nmatch)
		{
			v->pmatch[n].rm_so = -1;
			v->pmatch[n].rm_eo = -1;
		}
	}

	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		zaptreesubs(v, t2);
}

/*
 * subset - 为成功的 subre 设置子表达式匹配数据
 */
static void
subset(struct vars *v,
	   struct subre *sub,
	   chr *begin,
	   chr *end)
{
	int			n = sub->capno;

	assert(n > 0);
	if ((size_t) n >= v->nmatch)
		return;

	MDEBUG(("%d: setting %d = %ld-%ld\n", sub->id, n, LOFF(begin), LOFF(end)));
	v->pmatch[n].rm_so = OFF(begin);
	v->pmatch[n].rm_eo = OFF(end);
}

/*
 * cdissect - 检查反向引用并确定子表达式匹配
 *
 * cdissect 递归处理 subre 树以检查反向引用的匹配
 * 和/或识别捕获节点的子匹配边界。提议的匹配
 * 从 "begin" 到 "end"（不含 "end"），我们基本上是在
 * 剖析它以查看子匹配在何处。
 *
 * 在调用任一层 cdissect 前，调用方须已跑过该节点的
 * DFA 并确认提议子串满足该 DFA。（我们让
 * 调用方做此事，因在连接与迭代节点中，对
 * 全部子串先用子 DFA 检查会快得多，然后再
 * 递归.)
 *
 * 成功匹配的副作用是把捕获子表达式的匹配
 * 位置存入 v->pmatch[]。这有点棘手，
 * 故我们订立如下规则：
 * 1. 初次进入 cdissect 前，全部匹配数据须已
 *    清零（由 zapallsubs 负责）。
 * 2. 任何递归进入 cdissect 前，该子表达式
 *    树的匹配数据须保证已清（见 zaptreesubs）。
 * 3. 返回 REG_OKAY 时，每层 cdissect 将已保存
 *    任何相关匹配位置。
 * 4. 返回 REG_NOMATCH 时，每层 cdissect 将保证
 *    其子表达式匹配位置再次清空。
 * 5. 错误情形（即其他结果码）不做保证。
 * 6. 当某层 cdissect 放弃一次成功的子匹配时，它会
 *    在对该子树或其右侧任何子树尝试新的
 *    DFA 匹配或 cdissect 调用前，用 zaptreesubs
 *    清该子树匹配位置（即凡可能对已弃匹配有
 * 反向引用）。
 * 这看似过于复杂，但难以简化，因规定匹配
 * 位置须在任何新的 DFA 匹配前重置（此规则
 * 为使 dfa_backref 安全所必需）。这意味着
 * 无法仅在每层 cdissect 开头重置相关匹配
 * 位置。
 */
static int						/* regexec 返回码 */
cdissect(struct vars *v,
		 struct subre *t,
		 chr *begin,			/* 相关子串之始 */
		 chr *end)				/* 同段末 */
{
	int			er;

	assert(t != NULL);
	MDEBUG(("%d: cdissect %c %ld-%ld\n", t->id, t->op, LOFF(begin), LOFF(end)));

	/* 检查操作取消的便利处 */
	INTERRUPT(v->re);
	/* 以及栈溢出 */
	if (STACK_TOO_DEEP(v->re))
		return REG_ETOOBIG;

	switch (t->op)
	{
		case '=':				/* 终结节点 */
			assert(t->child == NULL);
			er = REG_OKAY;		/* 无动作，父节点已做完 */
			break;
		case 'b':				/* 反向引用 */
			assert(t->child == NULL);
			er = cbrdissect(v, t, begin, end);
			break;
		case '.':				/* 连接 */
			assert(t->child != NULL);
			if (t->child->flags & SHORTER)	/* 反向扫描 */
				er = crevcondissect(v, t, begin, end);
			else
				er = ccondissect(v, t, begin, end);
			break;
		case '|':				/* 交替 */
			assert(t->child != NULL);
			er = caltdissect(v, t, begin, end);
			break;
		case '*':				/* 迭代 */
			assert(t->child != NULL);
			if (t->child->flags & SHORTER)	/* 反向扫描 */
				er = creviterdissect(v, t, begin, end);
			else
				er = citerdissect(v, t, begin, end);
			break;
		case '(':				/* 空操作捕获点 */
			assert(t->child != NULL);
			er = cdissect(v, t->child, begin, end);
			break;
		default:
			er = REG_ASSERT;
			break;
	}

	/*
	 * 我们不应有匹配失败，除非下面潜伏反向引用；
	 * 否则，要么调用方未检查 DFA，要么 DFA 与节点
	 * 内部有某种不一致。
	 */
	assert(er != REG_NOMATCH || (t->flags & BACKR));

	/*
	 * 若此节点标为捕获，则保存成功匹配的位置。
	 */
	if (t->capno > 0 && er == REG_OKAY)
		subset(v, t, begin, end);

	return er;
}

/*
 * ccondissect - 剖析连接节点的匹配
 */
static int						/* regexec 返回码 */
ccondissect(struct vars *v,
			struct subre *t,
			chr *begin,			/* 相关子串之始 */
			chr *end)			/* 同段末 */
{
	struct subre *left = t->child;
	struct subre *right = left->sibling;
	struct dfa *d;
	struct dfa *d2;
	chr		   *mid;
	int			er;

	assert(t->op == '.');
	assert(left != NULL && left->cnfa.nstates > 0);
	assert(right != NULL && right->cnfa.nstates > 0);
	assert(right->sibling == NULL);
	assert(!(left->flags & SHORTER));

	d = getsubdfa(v, left);
	NOERR();
	d2 = getsubdfa(v, right);
	NOERR();
	MDEBUG(("%d: ccondissect %ld-%ld\n", t->id, LOFF(begin), LOFF(end)));

	/* 试取暂定中点 */
	mid = longest(v, d, begin, end, (int *) NULL);
	NOERR();
	if (mid == NULL)
		return REG_NOMATCH;
	MDEBUG(("%d: tentative midpoint %ld\n", t->id, LOFF(mid)));

	/* 迭代至满足或失败 */
	for (;;)
	{
		/* 试此中点是否合用 */
		if (longest(v, d2, mid, end, (int *) NULL) == end)
		{
			er = cdissect(v, left, begin, mid);
			if (er == REG_OKAY)
			{
				er = cdissect(v, right, mid, end);
				if (er == REG_OKAY)
				{
					/* 满足 */
					MDEBUG(("%d: successful\n", t->id));
					return REG_OKAY;
				}
				/* 重置左侧的匹配（右侧应已自理） */
				zaptreesubs(v, left);
			}
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		/* 该中点不行，找新的 */
		if (mid == begin)
		{
			/* 所有可能已穷尽 */
			MDEBUG(("%d: no midpoint\n", t->id));
			return REG_NOMATCH;
		}
		mid = longest(v, d, begin, mid - 1, (int *) NULL);
		NOERR();
		if (mid == NULL)
		{
			/* 未能找到新的 */
			MDEBUG(("%d: failed midpoint\n", t->id));
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->id, LOFF(mid)));
	}

	/* 无法至此 */
	return REG_ASSERT;
}

/*
 * crevcondissect - 剖析连接节点匹配，最短优先
 */
static int						/* regexec 返回码 */
crevcondissect(struct vars *v,
			   struct subre *t,
			   chr *begin,		/* 相关子串之始 */
			   chr *end)		/* 同段末 */
{
	struct subre *left = t->child;
	struct subre *right = left->sibling;
	struct dfa *d;
	struct dfa *d2;
	chr		   *mid;
	int			er;

	assert(t->op == '.');
	assert(left != NULL && left->cnfa.nstates > 0);
	assert(right != NULL && right->cnfa.nstates > 0);
	assert(right->sibling == NULL);
	assert(left->flags & SHORTER);

	d = getsubdfa(v, left);
	NOERR();
	d2 = getsubdfa(v, right);
	NOERR();
	MDEBUG(("%d: crevcondissect %ld-%ld\n", t->id, LOFF(begin), LOFF(end)));

	/* 试取暂定中点 */
	mid = shortest(v, d, begin, begin, end, (chr **) NULL, (int *) NULL);
	NOERR();
	if (mid == NULL)
		return REG_NOMATCH;
	MDEBUG(("%d: tentative midpoint %ld\n", t->id, LOFF(mid)));

	/* 迭代至满足或失败 */
	for (;;)
	{
		/* 试此中点是否合用 */
		if (longest(v, d2, mid, end, (int *) NULL) == end)
		{
			er = cdissect(v, left, begin, mid);
			if (er == REG_OKAY)
			{
				er = cdissect(v, right, mid, end);
				if (er == REG_OKAY)
				{
					/* 满足 */
					MDEBUG(("%d: successful\n", t->id));
					return REG_OKAY;
				}
				/* 重置左侧的匹配（右侧应已自理） */
				zaptreesubs(v, left);
			}
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		/* 该中点不行，找新的 */
		if (mid == end)
		{
			/* 所有可能已穷尽 */
			MDEBUG(("%d: no midpoint\n", t->id));
			return REG_NOMATCH;
		}
		mid = shortest(v, d, begin, mid + 1, end, (chr **) NULL, (int *) NULL);
		NOERR();
		if (mid == NULL)
		{
			/* 未能找到新的 */
			MDEBUG(("%d: failed midpoint\n", t->id));
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->id, LOFF(mid)));
	}

	/* 无法至此 */
	return REG_ASSERT;
}

/*
 * cbrdissect - 剖析反向引用节点匹配
 *
 * 反向引用匹配或许已由 DFA 侧 dfa_backref() 验证，
 * 但我们并不确定，故须在此再查。
 */
static int						/* regexec 返回码 */
cbrdissect(struct vars *v,
		   struct subre *t,
		   chr *begin,			/* 相关子串之始 */
		   chr *end)			/* 同段末 */
{
	int			n = t->backno;
	size_t		numreps;
	size_t		tlen;
	size_t		brlen;
	chr		   *brstring;
	chr		   *p;
	int			min = t->min;
	int			max = t->max;

	assert(t != NULL);
	assert(t->op == 'b');
	assert(n >= 0);
	assert((size_t) n < v->nmatch);

	MDEBUG(("%d: cbrdissect %d{%d-%d} %ld-%ld\n", t->id, n, min, max,
			LOFF(begin), LOFF(end)));

	/* 取被反向引用的串 */
	if (v->pmatch[n].rm_so == -1)
		return REG_NOMATCH;
	brstring = v->start + v->pmatch[n].rm_so;
	brlen = v->pmatch[n].rm_eo - v->pmatch[n].rm_so;

	/* 零长串的特例 */
	if (brlen == 0)
	{
		/*
		 * 仅当目标为零长才匹配，但任意次数的
		 * 重复都可视为已出现
		 */
		if (begin == end && min <= max)
		{
			MDEBUG(("%d: backref matched trivially\n", t->id));
			return REG_OKAY;
		}
		return REG_NOMATCH;
	}
	if (begin == end)
	{
		/* 仅当允许零次重复才匹配 */
		if (min == 0)
		{
			MDEBUG(("%d: backref matched trivially\n", t->id));
			return REG_OKAY;
		}
		return REG_NOMATCH;
	}

	/*
	 * 检查目标长度，看它是否可能是 brstring 允许的
	 * 重复次数
	 */
	assert(end > begin);
	tlen = end - begin;
	if (tlen % brlen != 0)
		return REG_NOMATCH;
	numreps = tlen / brlen;
	if (numreps < min || (numreps > max && max != DUPINF))
		return REG_NOMATCH;

	/* 好，比较实际串内容 */
	p = begin;
	while (numreps-- > 0)
	{
		if ((*v->g->compare) (brstring, p, brlen) != 0)
			return REG_NOMATCH;
		p += brlen;
	}

	MDEBUG(("%d: backref matched\n", t->id));
	return REG_OKAY;
}

/*
 * caltdissect - 剖析交替节点的匹配
 */
static int						/* regexec 返回码 */
caltdissect(struct vars *v,
			struct subre *t,
			chr *begin,			/* 相关子串之始 */
			chr *end)			/* 同段末 */
{
	struct dfa *d;
	int			er;

	assert(t->op == '|');

	t = t->child;
	/* 应至少有 2 个备选 */
	assert(t != NULL && t->sibling != NULL);

	while (t != NULL)
	{
		assert(t->cnfa.nstates > 0);

		MDEBUG(("%d: caltdissect %ld-%ld\n", t->id, LOFF(begin), LOFF(end)));

		d = getsubdfa(v, t);
		NOERR();
		if (longest(v, d, begin, end, (int *) NULL) == end)
		{
			MDEBUG(("%d: caltdissect matched\n", t->id));
			er = cdissect(v, t, begin, end);
			if (er != REG_NOMATCH)
				return er;
		}
		NOERR();

		t = t->sibling;
	}

	return REG_NOMATCH;
}

/*
 * citerdissect - 剖析迭代节点的匹配
 */
static int						/* regexec 返回码 */
citerdissect(struct vars *v,
			 struct subre *t,
			 chr *begin,		/* 相关子串之始 */
			 chr *end)			/* 同段末 */
{
	struct dfa *d;
	chr		  **endpts;
	chr		   *limit;
	int			min_matches;
	size_t		max_matches;
	int			nverified;
	int			k;
	int			i;
	int			er;

	assert(t->op == '*');
	assert(t->child != NULL && t->child->cnfa.nstates > 0);
	assert(!(t->child->flags & SHORTER));
	assert(begin <= end);

	MDEBUG(("%d: citerdissect %ld-%ld\n", t->id, LOFF(begin), LOFF(end)));

	/*
	 * 眼下假定最少匹配次数为 1。若允许零次
	 * 匹配，且目标串为空，则不论 iter 节点内容
	 * 无论如何都允许匹配——但更希望匹配一次，
	 * 以便捕获括号被设置。（此处关切的例子是
	 * 形如 "()*\1" 的模式，历史上本代码曾许其成功。）
	 * 因此，我们在底部处理零次匹配情形，即在找
	 * 不到其他匹配方式之后。
	 */
	min_matches = t->min;
	if (min_matches <= 0)
		min_matches = 1;

	/*
	 * 我们需要工作区来跟踪各子匹配的端点。通常
	 * 只考虑非零长子匹配，故至多有 end-begin 个。
	 * 然而，若 min 大于该值，我们也将考虑零长
	 * 子匹配以便找到足够的匹配。
	 *
	 * 为方便，endpts[0] 含 "begin" 指针，子匹配
	 * 端点存于 endpts[1..max_matches]。
	 */
	max_matches = end - begin;
	if (max_matches > t->max && t->max != DUPINF)
		max_matches = t->max;
	if (max_matches < min_matches)
		max_matches = min_matches;
	endpts = (chr **) MALLOC((max_matches + 1) * sizeof(chr *));
	if (endpts == NULL)
		return REG_ESPACE;
	endpts[0] = begin;

	d = getsubdfa(v, t->child);
	if (ISERR())
	{
		FREE(endpts);
		return v->err;
	}

	/*
	 * 我们的策略是先找一组按子节点 DFA 判定为
	 * 有效的子匹配端点，再递归剖析各子匹配以确认
	 * 有效性。若任何有效性检查失败，则回溯该
	 * 子匹配再试。并且，下次做有效性检查时，
	 * 不必重查我们未移动端点的、已成功验证的
	 * 子匹配。nverified 记住当前已知无误的
	 * 子匹配个数。
	 */

	/* 初始化以考虑第一个子匹配 */
	nverified = 0;
	k = 1;
	limit = end;

	/* 迭代至满足或失败 */
	while (k > 0)
	{
		/* 试为第 k 个子匹配找一端点 */
		endpts[k] = longest(v, d, endpts[k - 1], limit, (int *) NULL);
		if (ISERR())
		{
			FREE(endpts);
			return v->err;
		}
		if (endpts[k] == NULL)
		{
			/* 无匹配可能，故看能否缩短前一个 */
			k--;
			goto backtrack;
		}
		MDEBUG(("%d: working endpoint %d: %ld\n",
				t->id, k, LOFF(endpts[k])));

		/* 第 k 个子匹配不再可视为已验证 */
		if (nverified >= k)
			nverified = k - 1;

		if (endpts[k] != end)
		{
			/* 尚未到末尾，若允许则再试一次迭代 */
			if (k >= max_matches)
			{
				/* 须尝试缩短某个先前匹配 */
				k--;
				goto backtrack;
			}

			/* 拒绝零长匹配，除非为实现 min 所必需 */
			if (endpts[k] == endpts[k - 1] &&
				(k >= min_matches || min_matches - k < end - endpts[k]))
				goto backtrack;

			k++;
			limit = end;
			continue;
		}

		/*
		 * 我们已找到一种把串分成 k 个子匹配的方式，
		 * 就子 DFA 所能判断而言可行。若 k 是允许的
		 * 匹配次数，则开始慢部分：递归验证各子匹配。
		 * 我们总有 k <= max_matches，无需检查。
		 */
		if (k < min_matches)
			goto backtrack;

		MDEBUG(("%d: verifying %d..%d\n", t->id, nverified + 1, k));

		for (i = nverified + 1; i <= k; i++)
		{
			/* 清除非末次迭代的任何匹配数据 */
			zaptreesubs(v, t->child);
			er = cdissect(v, t->child, endpts[i - 1], endpts[i]);
			if (er == REG_OKAY)
			{
				nverified = i;
				continue;
			}
			if (er == REG_NOMATCH)
				break;
			/* 有东西失败了 */
			FREE(endpts);
			return er;
		}

		if (i > k)
		{
			/* 满足 */
			MDEBUG(("%d: successful\n", t->id));
			FREE(endpts);
			return REG_OKAY;
		}

		/* 第 i 次匹配验证失败，故回溯之 */
		k = i;

backtrack:

		/*
		 * 须考虑第 k 个子匹配的更短版本。然而，
		 * 仅在必要时才要求零长匹配。
		 */
		while (k > 0)
		{
			chr		   *prev_end = endpts[k - 1];

			if (endpts[k] > prev_end)
			{
				limit = endpts[k] - 1;
				if (limit > prev_end ||
					(k < min_matches && min_matches - k >= end - prev_end))
				{
					/* 跳出回溯环，继续外层循环 */
					break;
				}
			}
			/* 无法再缩短第 k 个子匹配，考虑前一个 */
			k--;
		}
	}

	/* 所有可能已穷尽 */
	FREE(endpts);

	/*
	 * 现在考虑用零次重复匹配到零长串的
	 * 可能性。
	 */
	if (t->min == 0 && begin == end)
	{
		MDEBUG(("%d: allowing zero matches\n", t->id));
		return REG_OKAY;
	}

	MDEBUG(("%d: failed\n", t->id));
	return REG_NOMATCH;
}

/*
 * creviterdissect - 剖析迭代节点匹配，最短优先
 */
static int						/* regexec 返回码 */
creviterdissect(struct vars *v,
				struct subre *t,
				chr *begin,		/* 相关子串之始 */
				chr *end)		/* 同段末 */
{
	struct dfa *d;
	chr		  **endpts;
	chr		   *limit;
	int			min_matches;
	size_t		max_matches;
	int			nverified;
	int			k;
	int			i;
	int			er;

	assert(t->op == '*');
	assert(t->child != NULL && t->child->cnfa.nstates > 0);
	assert(t->child->flags & SHORTER);
	assert(begin <= end);

	MDEBUG(("%d: creviterdissect %ld-%ld\n", t->id, LOFF(begin), LOFF(end)));

	/*
	 * 若允许零次匹配且目标串为空，则直接宣告
	 * 胜利。反之，若目标串非空，零次匹配无法奏效，
	 * 故佯装 min 为 1。
	 */
	min_matches = t->min;
	if (min_matches <= 0)
	{
		if (begin == end)
		{
			MDEBUG(("%d: allowing zero matches\n", t->id));
			return REG_OKAY;
		}
		min_matches = 1;
	}

	/*
	 * 我们需要工作区来跟踪各子匹配的端点。通常
	 * 只考虑非零长子匹配，故至多有 end-begin 个。
	 * 然而，若 min 大于该值，我们也将考虑零长
	 * 子匹配以便找到足够的匹配。
	 *
	 * 为方便，endpts[0] 含 "begin" 指针，子匹配
	 * 端点存于 endpts[1..max_matches]。
	 */
	max_matches = end - begin;
	if (max_matches > t->max && t->max != DUPINF)
		max_matches = t->max;
	if (max_matches < min_matches)
		max_matches = min_matches;
	endpts = (chr **) MALLOC((max_matches + 1) * sizeof(chr *));
	if (endpts == NULL)
		return REG_ESPACE;
	endpts[0] = begin;

	d = getsubdfa(v, t->child);
	if (ISERR())
	{
		FREE(endpts);
		return v->err;
	}

	/*
	 * 我们的策略是先找一组按子节点 DFA 判定为
	 * 有效的子匹配端点，再递归剖析各子匹配以确认
	 * 有效性。若任何有效性检查失败，则回溯该
	 * 子匹配再试。并且，下次做有效性检查时，
	 * 不必重查我们未移动端点的、已成功验证的
	 * 子匹配。nverified 记住当前已知无误的
	 * 子匹配个数。
	 */

	/* 初始化以考虑第一个子匹配 */
	nverified = 0;
	k = 1;
	limit = begin;

	/* 迭代至满足或失败 */
	while (k > 0)
	{
		/* 禁止零长匹配，除非为实现 min 所必需 */
		if (limit == endpts[k - 1] &&
			limit != end &&
			(k >= min_matches || min_matches - k < end - limit))
			limit++;

		/* 若这是最后一个允许的子匹配，它必须抵达末尾 */
		if (k >= max_matches)
			limit = end;

		/* 试为第 k 个子匹配找一端点 */
		endpts[k] = shortest(v, d, endpts[k - 1], limit, end,
							 (chr **) NULL, (int *) NULL);
		if (ISERR())
		{
			FREE(endpts);
			return v->err;
		}
		if (endpts[k] == NULL)
		{
			/* 无匹配可能，故看能否加长前一个 */
			k--;
			goto backtrack;
		}
		MDEBUG(("%d: working endpoint %d: %ld\n",
				t->id, k, LOFF(endpts[k])));

		/* 第 k 个子匹配不再可视为已验证 */
		if (nverified >= k)
			nverified = k - 1;

		if (endpts[k] != end)
		{
			/* 尚未到末尾，若允许则再试一次迭代 */
			if (k >= max_matches)
			{
				/* 须尝试加长某个先前匹配 */
				k--;
				goto backtrack;
			}

			k++;
			limit = endpts[k - 1];
			continue;
		}

		/*
		 * 我们已找到一种把串分成 k 个子匹配的方式，
		 * 就子 DFA 所能判断而言可行。若 k 是允许的
		 * 匹配次数，则开始慢部分：递归验证各子匹配。
		 * 我们总有 k <= max_matches，无需检查。
		 */
		if (k < min_matches)
			goto backtrack;

		MDEBUG(("%d: verifying %d..%d\n", t->id, nverified + 1, k));

		for (i = nverified + 1; i <= k; i++)
		{
			/* 清除非末次迭代的任何匹配数据 */
			zaptreesubs(v, t->child);
			er = cdissect(v, t->child, endpts[i - 1], endpts[i]);
			if (er == REG_OKAY)
			{
				nverified = i;
				continue;
			}
			if (er == REG_NOMATCH)
				break;
			/* 有东西失败了 */
			FREE(endpts);
			return er;
		}

		if (i > k)
		{
			/* 满足 */
			MDEBUG(("%d: successful\n", t->id));
			FREE(endpts);
			return REG_OKAY;
		}

		/* 第 i 次匹配验证失败，故回溯之 */
		k = i;

backtrack:

		/*
		 * 须考虑第 k 个子匹配的更长版本。
		 */
		while (k > 0)
		{
			if (endpts[k] < end)
			{
				limit = endpts[k] + 1;
				/* 跳出回溯环，继续外层循环 */
				break;
			}
			/* 无法再加长第 k 个子匹配，考虑前一个 */
			k--;
		}
	}

	/* 所有可能已穷尽 */
	MDEBUG(("%d: failed\n", t->id));
	FREE(endpts);
	return REG_NOMATCH;
}



#include "rege_dfa.c"
