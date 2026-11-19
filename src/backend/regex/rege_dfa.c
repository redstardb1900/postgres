/*
 * DFA 例程
 * 本文件由 regexec.c #include。
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
 * src/backend/regex/rege_dfa.c
 *
 */

/*
 * longest - 最长优先匹配引擎
 *
 * 成功时返回匹配终点地址。无匹配则返回 NULL。
 * 内部错误亦返回 NULL，并置 v->err。
 */
static chr *
longest(struct vars *v,
		struct dfa *d,
		chr *start,				/* 匹配应起始之处 */
		chr *stop,				/* 匹配须在此或之前结束 */
		int *hitstopp)			/* 非 NULL 时记录是否触及 v->stop */
{
	chr		   *cp;
	chr		   *realstop = (stop == v->stop) ? stop : stop + 1;
	color		co;
	struct sset *css;
	struct sset *ss;
	chr		   *post;
	int			i;
	struct colormap *cm = d->cm;

	/* 防止"未初始化变量"警告 */
	if (hitstopp != NULL)
		*hitstopp = 0;

	/* 若此为指向已知串的反向引用，则只与之匹配 */
	if (d->backno >= 0)
	{
		assert((size_t) d->backno < v->nmatch);
		if (v->pmatch[d->backno].rm_so >= 0)
		{
			cp = dfa_backref(v, d, start, start, stop, false);
			if (cp == v->stop && stop == v->stop && hitstopp != NULL)
				*hitstopp = 1;
			return cp;
		}
	}

	/* matchall NFA 快路径 */
	if (d->cnfa->flags & MATCHALL)
	{
		size_t		nchr = stop - start;
		size_t		maxmatchall = d->cnfa->maxmatchall;

		if (nchr < d->cnfa->minmatchall)
			return NULL;
		if (maxmatchall == DUPINF)
		{
			if (stop == v->stop && hitstopp != NULL)
				*hitstopp = 1;
		}
		else
		{
			if (stop == v->stop && nchr <= maxmatchall + 1 && hitstopp != NULL)
				*hitstopp = 1;
			if (nchr > maxmatchall)
				return start + maxmatchall;
		}
		return stop;
	}

	/* 初始化 */
	css = initialize(v, d, start);
	if (css == NULL)
		return NULL;
	cp = start;

	/* 启动 */
	FDEBUG(("+++ startup +++\n"));
	if (cp == v->start)
	{
		co = d->cnfa->bos[(v->eflags & REG_NOTBOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));
	}
	else
	{
		co = GETCOLOR(cm, *(cp - 1));
		FDEBUG(("char %c, color %ld\n", (char) *(cp - 1), (long) co));
	}
	css = miss(v, d, css, co, cp, start);
	if (css == NULL)
		return NULL;
	css->lastseen = cp;

	/*
	 * 这是主文本扫描循环。值得备两份副本
	 * 以免此处 REG_FTRACE 测试开销，即便在 REG_DEBUG
	 * 构建中，于未主动跟踪时。
	 */
#ifdef REG_DEBUG
	if (v->eflags & REG_FTRACE)
	{
		while (cp < realstop)
		{
			FDEBUG(("+++ at c%d +++\n", (int) (css - d->ssets)));
			co = GETCOLOR(cm, *cp);
			FDEBUG(("char %c, color %ld\n", (char) *cp, (long) co));
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
		}
	}
	else
#endif
	{
		while (cp < realstop)
		{
			co = GETCOLOR(cm, *cp);
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
		}
	}

	if (ISERR())
		return NULL;

	/* 收尾 */
	FDEBUG(("+++ shutdown at c%d +++\n", (int) (css - d->ssets)));
	if (cp == v->stop && stop == v->stop)
	{
		if (hitstopp != NULL)
			*hitstopp = 1;
		co = d->cnfa->eos[(v->eflags & REG_NOTEOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));
		ss = miss(v, d, css, co, cp, start);
		if (ISERR())
			return NULL;
		/* 特例：匹配止于 eol？ */
		if (ss != NULL && (ss->flags & POSTSTATE))
			return cp;
		else if (ss != NULL)
			ss->lastseen = cp;	/* 为整洁 */
	}

	/* 找最后匹配若有 */
	post = d->lastpost;
	for (ss = d->ssets, i = d->nssused; i > 0; ss++, i--)
		if ((ss->flags & POSTSTATE) && post != ss->lastseen &&
			(post == NULL || post < ss->lastseen))
			post = ss->lastseen;
	if (post != NULL)			/* 找到了 */
		return post - 1;

	return NULL;
}

/*
 * shortest - 最短优先匹配引擎
 *
 * 成功时返回匹配终点地址。无匹配则返回 NULL。
 * 内部错误亦返回 NULL，并置 v->err。
 */
static chr *
shortest(struct vars *v,
		 struct dfa *d,
		 chr *start,			/* 匹配应起始之处 */
		 chr *min,				/* 匹配须在此或之后结束 */
		 chr *max,				/* 匹配须在此或之前结束 */
		 chr **coldp,			/* 非 NULL 则在此存 coldstart 指针 */
		 int *hitstopp)			/* 非 NULL 时记录是否触及 v->stop */
{
	chr		   *cp;
	chr		   *realmin = (min == v->stop) ? min : min + 1;
	chr		   *realmax = (max == v->stop) ? max : max + 1;
	color		co;
	struct sset *css;
	struct sset *ss;
	struct colormap *cm = d->cm;

	/* 防止"未初始化变量"警告 */
	if (coldp != NULL)
		*coldp = NULL;
	if (hitstopp != NULL)
		*hitstopp = 0;

	/* 若此为指向已知串的反向引用，则只与之匹配 */
	if (d->backno >= 0)
	{
		assert((size_t) d->backno < v->nmatch);
		if (v->pmatch[d->backno].rm_so >= 0)
		{
			cp = dfa_backref(v, d, start, min, max, true);
			if (cp != NULL && coldp != NULL)
				*coldp = start;
			/* 无应置 *hitstopp 的情形 */
			return cp;
		}
	}

	/* matchall NFA 快路径 */
	if (d->cnfa->flags & MATCHALL)
	{
		size_t		nchr = min - start;

		if (d->cnfa->maxmatchall != DUPINF &&
			nchr > d->cnfa->maxmatchall)
			return NULL;
		if ((max - start) < d->cnfa->minmatchall)
			return NULL;
		if (nchr < d->cnfa->minmatchall)
			min = start + d->cnfa->minmatchall;
		if (coldp != NULL)
			*coldp = start;
		/* 无应置 *hitstopp 的情形 */
		return min;
	}

	/* 初始化 */
	css = initialize(v, d, start);
	if (css == NULL)
		return NULL;
	cp = start;

	/* 启动 */
	FDEBUG(("--- startup ---\n"));
	if (cp == v->start)
	{
		co = d->cnfa->bos[(v->eflags & REG_NOTBOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));
	}
	else
	{
		co = GETCOLOR(cm, *(cp - 1));
		FDEBUG(("char %c, color %ld\n", (char) *(cp - 1), (long) co));
	}
	css = miss(v, d, css, co, cp, start);
	if (css == NULL)
		return NULL;
	css->lastseen = cp;
	ss = css;

	/*
	 * 这是主文本扫描循环。值得备两份副本
	 * 以免此处 REG_FTRACE 测试开销，即便在 REG_DEBUG
	 * 构建中，于未主动跟踪时。
	 */
#ifdef REG_DEBUG
	if (v->eflags & REG_FTRACE)
	{
		while (cp < realmax)
		{
			FDEBUG(("--- at c%d ---\n", (int) (css - d->ssets)));
			co = GETCOLOR(cm, *cp);
			FDEBUG(("char %c, color %ld\n", (char) *cp, (long) co));
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
			if ((ss->flags & POSTSTATE) && cp >= realmin)
				break;			/* 注意跳出 */
		}
	}
	else
#endif
	{
		while (cp < realmax)
		{
			co = GETCOLOR(cm, *cp);
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
			if ((ss->flags & POSTSTATE) && cp >= realmin)
				break;			/* 注意跳出 */
		}
	}

	if (ss == NULL)
		return NULL;

	if (coldp != NULL)			/* 报告最后无进展状态集，若有 */
		*coldp = lastcold(v, d);

	if ((ss->flags & POSTSTATE) && cp > min)
	{
		assert(cp >= realmin);
		cp--;
	}
	else if (cp == v->stop && max == v->stop)
	{
		co = d->cnfa->eos[(v->eflags & REG_NOTEOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));
		ss = miss(v, d, css, co, cp, start);
		/* 匹配或已止于 eol */
		if ((ss == NULL || !(ss->flags & POSTSTATE)) && hitstopp != NULL)
			*hitstopp = 1;
	}

	if (ss == NULL || !(ss->flags & POSTSTATE))
		return NULL;

	return cp;
}

/*
 * matchuntil - 增量匹配引擎
 *
 * 供搜索型 NFA 使用（即该模式已知会
 * 表现为带前导 .*）。判定是否存在自
 * v->start 起、止于 probe 的匹配。多次调用
 * 仅需 O(N) 而非 O(N^2)，只要各 probe 值
 * 非递减。*lastcss 与 *lastcp 须先初始化为 NULL，
 * 再开始一系列调用。
 *
 * 有匹配返回 1，否则返回 0。
 * 内部错误亦返回 0，并置 v->err。
 */
static int
matchuntil(struct vars *v,
		   struct dfa *d,
		   chr *probe,			/* 欲知匹配是否止于此 */
		   struct sset **lastcss,	/* 跨调用的状态存储 */
		   chr **lastcp)		/* 跨调用的状态存储 */
{
	chr		   *cp = *lastcp;
	color		co;
	struct sset *css = *lastcss;
	struct sset *ss;
	struct colormap *cm = d->cm;

	/* matchall NFA 快路径 */
	if (d->cnfa->flags & MATCHALL)
	{
		size_t		nchr = probe - v->start;

		if (nchr < d->cnfa->minmatchall)
			return 0;
		/* maxmatchall 恒为无穷，参见 makesearch() */
		assert(d->cnfa->maxmatchall == DUPINF);
		return 1;
	}

	/* 必要时初始化并启动，或重启 */
	if (cp == NULL || cp > probe)
	{
		cp = v->start;
		css = initialize(v, d, cp);
		if (css == NULL)
			return 0;

		FDEBUG((">>> startup >>>\n"));
		co = d->cnfa->bos[(v->eflags & REG_NOTBOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));

		css = miss(v, d, css, co, cp, v->start);
		if (css == NULL)
			return 0;
		css->lastseen = cp;
	}
	else if (css == NULL)
	{
		/* 先前已判明 *lastcp 之后不可能匹配 */
		return 0;
	}
	ss = css;

	/*
	 * 这是主文本扫描循环。值得备两份副本
	 * 以免此处 REG_FTRACE 测试开销，即便在 REG_DEBUG
	 * 构建中，于未主动跟踪时。
	 */
#ifdef REG_DEBUG
	if (v->eflags & REG_FTRACE)
	{
		while (cp < probe)
		{
			FDEBUG((">>> at c%d >>>\n", (int) (css - d->ssets)));
			co = GETCOLOR(cm, *cp);
			FDEBUG(("char %c, color %ld\n", (char) *cp, (long) co));
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, v->start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
		}
	}
	else
#endif
	{
		while (cp < probe)
		{
			co = GETCOLOR(cm, *cp);
			ss = css->outs[co];
			if (ss == NULL)
			{
				ss = miss(v, d, css, co, cp + 1, v->start);
				if (ss == NULL)
					break;		/* 注意跳出 */
			}
			cp++;
			ss->lastseen = cp;
			css = ss;
		}
	}

	*lastcss = ss;
	*lastcp = cp;

	if (ss == NULL)
		return 0;				/* 不可能匹配，或内部错误 */

	/* 还需再处理一个 chr，或 EOS 符号，以检查匹配 */
	if (cp < v->stop)
	{
		FDEBUG((">>> at c%d >>>\n", (int) (css - d->ssets)));
		co = GETCOLOR(cm, *cp);
		FDEBUG(("char %c, color %ld\n", (char) *cp, (long) co));
		ss = css->outs[co];
		if (ss == NULL)
			ss = miss(v, d, css, co, cp + 1, v->start);
	}
	else
	{
		assert(cp == v->stop);
		co = d->cnfa->eos[(v->eflags & REG_NOTEOL) ? 0 : 1];
		FDEBUG(("color %ld\n", (long) co));
		ss = miss(v, d, css, co, cp, v->start);
	}

	if (ss == NULL || !(ss->flags & POSTSTATE))
		return 0;

	return 1;
}

/*
 * dfa_backref - 为已知反向引用串求最佳DFA匹配长度
 *
 * 当反向引用的参照已可用时，我们能以远少于运行
 * 该反向引用节点 NFA 的工作量给出精确答案。
 *
 * 返回最长或最短有效重复匹配的终点，
 * 或无有效匹配时返回 NULL。
 *
 * 应与 cbrdissect() 保持同步，虽其任务不同：
 * 检查对串中预定区段的匹配。
 */
static chr *
dfa_backref(struct vars *v,
			struct dfa *d,
			chr *start,			/* 匹配应起始之处 */
			chr *min,			/* 匹配须在此或之后结束 */
			chr *max,			/* 匹配须在此或之前结束 */
			bool shortest)
{
	int			n = d->backno;
	int			backmin = d->backmin;
	int			backmax = d->backmax;
	size_t		numreps;
	size_t		minreps;
	size_t		maxreps;
	size_t		brlen;
	chr		   *brstring;
	chr		   *p;

	/* 取被反向引用的串（调用方应已检查） */
	if (v->pmatch[n].rm_so == -1)
		return NULL;
	brstring = v->start + v->pmatch[n].rm_so;
	brlen = v->pmatch[n].rm_eo - v->pmatch[n].rm_so;

	/* 特判零长反向引用以免除以零 */
	if (brlen == 0)
	{
		/*
		 * 仅匹配零长串，但任意次重复
		 * 都可视为存在
		 */
		if (min == start && backmin <= backmax)
			return start;
		return NULL;
	}

	/*
	 * 把 min、max 换成反向引用串可能的重复次数，
	 * 并适当取整
	 */
	if (min <= start)
		minreps = 0;
	else
		minreps = (min - start - 1) / brlen + 1;
	maxreps = (max - start) / brlen;

	/* 应用边界，再看是否有允许的匹配长度 */
	if (minreps < backmin)
		minreps = backmin;
	if (backmax != DUPINF && maxreps > backmax)
		maxreps = backmax;
	if (maxreps < minreps)
		return NULL;

	/* 零次重复匹配有效且更优则速退 */
	if (shortest && minreps == 0)
		return start;

	/* 好，比较实际串内容 */
	p = start;
	numreps = 0;
	while (numreps < maxreps)
	{
		if ((*v->g->compare) (brstring, p, brlen) != 0)
			break;
		p += brlen;
		numreps++;
		if (shortest && numreps >= minreps)
			break;
	}

	if (numreps >= minreps)
		return p;
	return NULL;
}

/*
 * lastcold - 判定此前无进展的最后位置
 */
static chr *					/* 端点，或 NULL */
lastcold(struct vars *v,
		 struct dfa *d)
{
	struct sset *ss;
	chr		   *nopr;
	int			i;

	nopr = d->lastnopr;
	if (nopr == NULL)
		nopr = v->start;
	for (ss = d->ssets, i = d->nssused; i > 0; ss++, i--)
		if ((ss->flags & NOPROGRESS) && nopr < ss->lastseen)
			nopr = ss->lastseen;
	return nopr;
}

/*
 * newdfa - 建立全新 DFA
 *
 * 失败时返回 NULL（并置 v->err）。
 */
static struct dfa *
newdfa(struct vars *v,
	   struct cnfa *cnfa,
	   struct colormap *cm,
	   struct smalldfa *sml)	/* 预分配空间，可为 NULL */
{
	struct dfa *d;
	size_t		nss = cnfa->nstates * 2;
	int			wordsper = (cnfa->nstates + UBITS - 1) / UBITS;
	bool		ismalloced = false;

	assert(cnfa != NULL && cnfa->nstates != 0);

	if (nss <= FEWSTATES && cnfa->ncolors <= FEWCOLORS)
	{
		assert(wordsper == 1);
		if (sml == NULL)
		{
			sml = (struct smalldfa *) MALLOC(sizeof(struct smalldfa));
			if (sml == NULL)
			{
				ERR(REG_ESPACE);
				return NULL;
			}
			ismalloced = true;
		}
		d = &sml->dfa;
		d->ssets = sml->ssets;
		d->statesarea = sml->statesarea;
		d->work = &d->statesarea[nss];
		d->outsarea = sml->outsarea;
		d->incarea = sml->incarea;
		d->ismalloced = ismalloced;
		d->arraysmalloced = false;	/* 反正非单独分配 */
	}
	else
	{
		d = (struct dfa *) MALLOC(sizeof(struct dfa));
		if (d == NULL)
		{
			ERR(REG_ESPACE);
			return NULL;
		}
		d->ssets = (struct sset *) MALLOC(nss * sizeof(struct sset));
		d->statesarea = (unsigned *) MALLOC((nss + WORK) * wordsper *
											sizeof(unsigned));
		d->work = &d->statesarea[nss * wordsper];
		d->outsarea = (struct sset **) MALLOC(nss * cnfa->ncolors *
											  sizeof(struct sset *));
		d->incarea = (struct arcp *) MALLOC(nss * cnfa->ncolors *
											sizeof(struct arcp));
		d->ismalloced = true;
		d->arraysmalloced = true;
		/* 此后 freedfa() 会正常表现 */
		if (d->ssets == NULL || d->statesarea == NULL ||
			d->outsarea == NULL || d->incarea == NULL)
		{
			freedfa(d);
			ERR(REG_ESPACE);
			return NULL;
		}
	}

	d->nssets = (v->eflags & REG_SMALL) ? 7 : nss;
	d->nssused = 0;
	d->nstates = cnfa->nstates;
	d->ncolors = cnfa->ncolors;
	d->wordsper = wordsper;
	d->cnfa = cnfa;
	d->cm = cm;
	d->lastpost = NULL;
	d->lastnopr = NULL;
	d->search = d->ssets;
	d->backno = -1;				/* 调用方可设 */
	d->backmin = d->backmax = 0;

	/* sset 字段按需初始化 */

	return d;
}

/*
 * freedfa - 释放 DFA
 */
static void
freedfa(struct dfa *d)
{
	if (d->arraysmalloced)
	{
		if (d->ssets != NULL)
			FREE(d->ssets);
		if (d->statesarea != NULL)
			FREE(d->statesarea);
		if (d->outsarea != NULL)
			FREE(d->outsarea);
		if (d->incarea != NULL)
			FREE(d->incarea);
	}

	if (d->ismalloced)
		FREE(d);
}

/*
 * hash - 为位向量构造散列码
 *
 * 或有更好办法，但更昂贵。
 */
static unsigned
hash(unsigned *uv,
	 int n)
{
	int			i;
	unsigned	h;

	h = 0;
	for (i = 0; i < n; i++)
		h ^= uv[i];
	return h;
}

/*
 * initialize - 为启动手制缓存项，否则做好准备
 */
static struct sset *
initialize(struct vars *v,
		   struct dfa *d,
		   chr *start)
{
	struct sset *ss;
	int			i;

	/* 前一项还在吗？ */
	if (d->nssused > 0 && (d->ssets[0].flags & STARTER))
		ss = &d->ssets[0];
	else
	{							/* 否，须(再)建之 */
		ss = getvacant(v, d, start, start);
		if (ss == NULL)
			return NULL;
		for (i = 0; i < d->wordsper; i++)
			ss->states[i] = 0;
		BSET(ss->states, d->cnfa->pre);
		ss->hash = HASH(ss->states, d->wordsper);
		assert(d->cnfa->pre != d->cnfa->post);
		ss->flags = STARTER | LOCKED | NOPROGRESS;
		/* lastseen 在下方处理 */
	}

	for (i = 0; i < d->nssused; i++)
		d->ssets[i].lastseen = NULL;
	ss->lastseen = start;		/* 或非真，但无害 */
	d->lastpost = NULL;
	d->lastnopr = NULL;
	return ss;
}

/*
 * miss - 处理状态集缓存未中
 *
 * css 为当前状态集，co 为当前输入字符之色，
 * cp 指向其后字符（即我们可能需在此测
 * LACON）。start 不影响匹配行为，但供 pickss
 * 之启发，以判定替换哪个状态集缓存项。
 *
 * 通常返回下一状态集的地址（即消费该输入
 * 字符后有效者）。若无有效 NFA 状态剩余则
 * 返回 NULL，即已确定匹配失败。
 * 内部错误亦返回 NULL，并置 v->err。
 */
static struct sset *
miss(struct vars *v,
	 struct dfa *d,
	 struct sset *css,
	 color co,
	 chr *cp,					/* 次 chr */
	 chr *start)				/* 尝试起始之处 */
{
	struct cnfa *cnfa = d->cnfa;
	int			i;
	unsigned	h;
	struct carc *ca;
	struct sset *p;
	int			ispseudocolor;
	int			ispost;
	int			noprogress;
	int			gotstate;
	int			dolacons;
	int			sawlacons;

	/* 为方便，就算不是未命中也可调用 */
	if (css->outs[co] != NULL)
	{
		FDEBUG(("hit\n"));
		return css->outs[co];
	}
	FDEBUG(("miss\n"));

	/*
	 * 在内层文本搜索循环中检查操作取消似乎
	 * 过于昂贵。折中办法是在缓存未命中时检查。
	 */
	INTERRUPT(v->re);

	/*
	 * 消费 co 字符后会落入哪组状态？
	 * 先考虑消费该字符的 PLAIN 弧，再看消费后
	 * 有哪些 LACON 弧可穿越。
	 */
	for (i = 0; i < d->wordsper; i++)
		d->work[i] = 0;			/* 在 d->work 中建新状态集位图 */
	ispseudocolor = d->cm->cd[co].flags & PSEUDO;
	ispost = 0;
	noprogress = 1;
	gotstate = 0;
	for (i = 0; i < d->nstates; i++)
		if (ISBSET(css->states, i))
			for (ca = cnfa->states[i]; ca->co != COLORLESS; ca++)
				if (ca->co == co ||
					(ca->co == RAINBOW && !ispseudocolor))
				{
					BSET(d->work, ca->to);
					gotstate = 1;
					if (ca->to == cnfa->post)
						ispost = 1;
					if (!(cnfa->stflags[ca->to] & CNFA_NOPROGRESS))
						noprogress = 0;
					FDEBUG(("%d -> %d\n", i, ca->to));
				}
	if (!gotstate)
		return NULL;			/* 字符无法抵达任何新状态 */
	dolacons = (cnfa->flags & HASLACONS);
	sawlacons = 0;
	/* 外层循环处理经 LACON 可达状态的传递闭包 */
	while (dolacons)
	{
		dolacons = 0;
		for (i = 0; i < d->nstates; i++)
			if (ISBSET(d->work, i))
				for (ca = cnfa->states[i]; ca->co != COLORLESS; ca++)
				{
					if (ca->co < cnfa->ncolors)
						continue;	/* 非 LACON 弧 */
					if (ISBSET(d->work, ca->to))
						continue;	/* 弧本即空操作 */
					sawlacons = 1;	/* 此 LACON 影响结果 */
					if (!lacon(v, cnfa, cp, ca->co))
					{
						if (ISERR())
							return NULL;
						continue;	/* LACON 弧无法穿越 */
					}
					if (ISERR())
						return NULL;
					BSET(d->work, ca->to);
					dolacons = 1;
					if (ca->to == cnfa->post)
						ispost = 1;
					if (!(cnfa->stflags[ca->to] & CNFA_NOPROGRESS))
						noprogress = 0;
					FDEBUG(("%d :> %d\n", i, ca->to));
				}
	}
	h = HASH(d->work, d->wordsper);

	/* 此状态集已在缓存中吗？ */
	for (p = d->ssets, i = d->nssused; i > 0; p++, i--)
		if (HIT(h, d->work, p, d->wordsper))
		{
			FDEBUG(("cached c%d\n", (int) (p - d->ssets)));
			break;				/* 注意跳出 */
		}
	if (i == 0)
	{							/* 否，需要新缓存项 */
		p = getvacant(v, d, cp, start);
		if (p == NULL)
			return NULL;
		assert(p != css);
		for (i = 0; i < d->wordsper; i++)
			p->states[i] = d->work[i];
		p->hash = h;
		p->flags = (ispost) ? POSTSTATE : 0;
		if (noprogress)
			p->flags |= NOPROGRESS;
		/* lastseen 由调用方处理 */
	}

	/*
	 * 把新状态集链到旧的，除非某 LACON 影响了结果，
	 * 此时不建链。这迫使今后经同一弧（同一先前
	 * 状态集与字符色）的转移再走 miss()，以便
	 * 重新检查 LACON（们），它们或通过或不通过，
	 * 因上下文将不同。
	 */
	if (!sawlacons)
	{
		FDEBUG(("c%d[%d]->c%d\n",
				(int) (css - d->ssets), co, (int) (p - d->ssets)));
		css->outs[co] = p;
		css->inchain[co] = p->ins;
		p->ins.ss = css;
		p->ins.co = co;
	}
	return p;
}

/*
 * lacon - 供 miss() 的环视约束检查器
 */
static int						/* 谓词：约束已满足？ */
lacon(struct vars *v,
	  struct cnfa *pcnfa,		/* 父 cNFA */
	  chr *cp,
	  color co)					/* 环视约束之"色" */
{
	int			n;
	struct subre *sub;
	struct dfa *d;
	chr		   *end;
	int			satisfied;

	/* 因递归，可能被逼到栈溢出 */
	if (STACK_TOO_DEEP(v->re))
	{
		ERR(REG_ETOOBIG);
		return 0;
	}

	n = co - pcnfa->ncolors;
	assert(n > 0 && n < v->g->nlacons && v->g->lacons != NULL);
	FDEBUG(("=== testing lacon %d\n", n));
	sub = &v->g->lacons[n];
	d = getladfa(v, n);
	if (d == NULL)
		return 0;
	if (LATYPE_IS_AHEAD(sub->latype))
	{
		/* 曾用 longest()，但 shortest() 可能便宜得多 */
		end = shortest(v, d, cp, cp, v->stop,
					   (chr **) NULL, (int *) NULL);
		satisfied = LATYPE_IS_POS(sub->latype) ? (end != NULL) : (end == NULL);
	}
	else
	{
		/*
		 * 为避免在 N 字符的串上反复测试后顾
		 * 约束时做 O(N^2) 工作，我们用 matchuntil()，它可
		 * 跨调用缓存 DFA 状态。仅当探测点回退时才需重
		 * 启，这并不常见。所用 NFA 是搜索型 NFA，故不介意
		 * 扫过名义匹配之前的
		 * 内容。
		 */
		satisfied = matchuntil(v, d, cp, &v->lblastcss[n], &v->lblastcp[n]);
		if (!LATYPE_IS_POS(sub->latype))
			satisfied = !satisfied;
	}
	FDEBUG(("=== lacon %d satisfied %d\n", n, satisfied));
	return satisfied;
}

/*
 * getvacant - 取空闲状态集
 *
 * 本例程清掉入弧与出弧，但不另行
 * 清理状态集内部——那由调用方负责。
 */
static struct sset *
getvacant(struct vars *v,
		  struct dfa *d,
		  chr *cp,
		  chr *start)
{
	int			i;
	struct sset *ss;
	struct sset *p;
	struct arcp ap;
	color		co;

	ss = pickss(v, d, cp, start);
	if (ss == NULL)
		return NULL;
	assert(!(ss->flags & LOCKED));

	/* 清其入弧，含自指者 */
	ap = ss->ins;
	while ((p = ap.ss) != NULL)
	{
		co = ap.co;
		FDEBUG(("zapping c%d's %ld outarc\n", (int) (p - d->ssets), (long) co));
		p->outs[co] = NULL;
		ap = p->inchain[co];
		p->inchain[co].ss = NULL;	/* 偏执 */
	}
	ss->ins.ss = NULL;

	/* 将其从出弧所达 sset 的入弧链上摘下 */
	for (i = 0; i < d->ncolors; i++)
	{
		p = ss->outs[i];
		assert(p != ss);		/* 非自指 */
		if (p == NULL)
			continue;			/* 注意继续 */
		FDEBUG(("del outarc %d from c%d's in chn\n", i, (int) (p - d->ssets)));
		if (p->ins.ss == ss && p->ins.co == i)
			p->ins = ss->inchain[i];
		else
		{
			struct arcp lastap = {NULL, 0};

			assert(p->ins.ss != NULL);
			for (ap = p->ins; ap.ss != NULL &&
				 !(ap.ss == ss && ap.co == i);
				 ap = ap.ss->inchain[ap.co])
				lastap = ap;
			assert(ap.ss != NULL);
			lastap.ss->inchain[lastap.co] = ss->inchain[i];
		}
		ss->outs[i] = NULL;
		ss->inchain[i].ss = NULL;
	}

	/* 若 ss 曾是成功状态，或需记住位置 */
	if ((ss->flags & POSTSTATE) && ss->lastseen != d->lastpost &&
		(d->lastpost == NULL || d->lastpost < ss->lastseen))
		d->lastpost = ss->lastseen;

	/* 对无进展状态亦然 */
	if ((ss->flags & NOPROGRESS) && ss->lastseen != d->lastnopr &&
		(d->lastnopr == NULL || d->lastnopr < ss->lastseen))
		d->lastnopr = ss->lastseen;

	return ss;
}

/*
 * pickss - 挑选下一要用的状态集
 */
static struct sset *
pickss(struct vars *v,
	   struct dfa *d,
	   chr *cp,
	   chr *start)
{
	int			i;
	struct sset *ss;
	struct sset *end;
	chr		   *ancient;

	/* 缓存未满时的捷径 */
	if (d->nssused < d->nssets)
	{
		i = d->nssused;
		d->nssused++;
		ss = &d->ssets[i];
		FDEBUG(("new c%d\n", i));
		/* 设置内部 */
		ss->states = &d->statesarea[i * d->wordsper];
		ss->flags = 0;
		ss->ins.ss = NULL;
		ss->ins.co = WHITE;		/* 赋个值 */
		ss->outs = &d->outsarea[i * d->ncolors];
		ss->inchain = &d->incarea[i * d->ncolors];
		for (i = 0; i < d->ncolors; i++)
		{
			ss->outs[i] = NULL;
			ss->inchain[i].ss = NULL;
		}
		return ss;
	}

	/* 找最旧的，或至少够旧的 */
	if (cp - start > d->nssets * 2 / 3) /* 最旧 33% 可丢弃 */
		ancient = cp - d->nssets * 2 / 3;
	else
		ancient = start;
	for (ss = d->search, end = &d->ssets[d->nssets]; ss < end; ss++)
		if ((ss->lastseen == NULL || ss->lastseen < ancient) &&
			!(ss->flags & LOCKED))
		{
			d->search = ss + 1;
			FDEBUG(("replacing c%d\n", (int) (ss - d->ssets)));
			return ss;
		}
	for (ss = d->ssets, end = d->search; ss < end; ss++)
		if ((ss->lastseen == NULL || ss->lastseen < ancient) &&
			!(ss->flags & LOCKED))
		{
			d->search = ss + 1;
			FDEBUG(("replacing c%d\n", (int) (ss - d->ssets)));
			return ss;
		}

	/* 竟然没够旧的？！——事态严重不对 */
	FDEBUG(("cannot find victim to replace!\n"));
	ERR(REG_ASSERT);
	return NULL;
}
