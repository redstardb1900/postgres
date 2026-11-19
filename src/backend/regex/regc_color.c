/*
 * 字符之着色
 * 本文件被 regcomp.c #include。
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
 * src/backend/regex/regc_color.c
 *
 *
 * 注意：本代码与 NFA 弧维护有若干纠缠
 * 关系，或许某时该清理一下。
 */



#define CISERR()	VISERR(cm->v)
#define CERR(e)		VERR(cm->v, (e))



/*
 * initcm - 建立新色图
 */
static void
initcm(struct vars *v,
	   struct colormap *cm)
{
	struct colordesc *cd;

	cm->magic = CMMAGIC;
	cm->v = v;

	cm->ncds = NINLINECDS;
	cm->cd = cm->cdspace;
	cm->max = 0;
	cm->free = 0;

	cd = cm->cd;				/* 即 cd[WHITE] */
	cd->nschrs = MAX_SIMPLE_CHR - CHR_MIN + 1;
	cd->nuchrs = 1;
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;
	cd->flags = 0;

	cm->locolormap = (color *)
		MALLOC((MAX_SIMPLE_CHR - CHR_MIN + 1) * sizeof(color));
	if (cm->locolormap == NULL)
	{
		CERR(REG_ESPACE);
		cm->cmranges = NULL;	/* 防 freecm 时失败 */
		cm->hicolormap = NULL;
		return;
	}
	/* 此 memset 依赖 WHITE 为零： */
	memset(cm->locolormap, WHITE,
		   (MAX_SIMPLE_CHR - CHR_MIN + 1) * sizeof(color));

	memset(cm->classbits, 0, sizeof(cm->classbits));
	cm->numcmranges = 0;
	cm->cmranges = NULL;
	cm->maxarrayrows = 4;		/* 任意初始分配 */
	cm->hiarrayrows = 1;		/* 但起初只有一行/列 */
	cm->hiarraycols = 1;
	cm->hicolormap = (color *) MALLOC(cm->maxarrayrows * sizeof(color));
	if (cm->hicolormap == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	/* 将"其余字符"行初始化为 WHITE */
	cm->hicolormap[0] = WHITE;
}

/*
 * freecm - 释放色图中动态分配之物
 */
static void
freecm(struct colormap *cm)
{
	cm->magic = 0;
	if (cm->cd != cm->cdspace)
		FREE(cm->cd);
	if (cm->locolormap != NULL)
		FREE(cm->locolormap);
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	if (cm->hicolormap != NULL)
		FREE(cm->hicolormap);
}

/*
 * pg_reg_getcolor - GETCOLOR() 慢路径
 */
color
pg_reg_getcolor(struct colormap *cm, chr c)
{
	int			rownum,
				colnum,
				low,
				high;

	/* 勿用于 locolormap 中的 chr */
	assert(c > MAX_SIMPLE_CHR);

	/*
	 * 找它所在行。colormapranges 有序，故可用
	 * 二分查找
	 */
	rownum = 0;					/* 无匹配则用数组第零行 */
	low = 0;
	high = cm->numcmranges;
	while (low < high)
	{
		int			middle = low + (high - low) / 2;
		const colormaprange *cmr = &cm->cmranges[middle];

		if (c < cmr->cmin)
			high = middle;
		else if (c > cmr->cmax)
			low = middle + 1;
		else
		{
			rownum = cmr->rownum;	/* 找到匹配 */
			break;
		}
	}

	/*
	 * 找它所在列——这全是区域设置相关的。
	 */
	if (cm->hiarraycols > 1)
	{
		colnum = cclass_column_index(cm, c);
		return cm->hicolormap[rownum * cm->hiarraycols + colnum];
	}
	else
	{
		/* 无相关 cclass 则走快路径 */
		return cm->hicolormap[rownum];
	}
}

/*
 * maxcolor - 报告在用的最大色号
 */
static color
maxcolor(struct colormap *cm)
{
	if (CISERR())
		return COLORLESS;

	return (color) cm->max;
}

/*
 * newcolor - 找一新色（须立即赋用）
 * 当心：或重定位colordescs。
 */
static color					/* 出错则 COLORLESS */
newcolor(struct colormap *cm)
{
	struct colordesc *cd;
	size_t		n;

	if (CISERR())
		return COLORLESS;

	if (cm->free != 0)
	{
		assert(cm->free > 0);
		assert((size_t) cm->free < cm->ncds);
		cd = &cm->cd[cm->free];
		assert(UNUSEDCOLOR(cd));
		assert(cd->arcs == NULL);
		cm->free = cd->sub;
	}
	else if (cm->max < cm->ncds - 1)
	{
		cm->max++;
		cd = &cm->cd[cm->max];
	}
	else
	{
		/* 糟糕，须再分配 */
		struct colordesc *newCd;

		if (cm->max == MAX_COLOR)
		{
			CERR(REG_ECOLORS);
			return COLORLESS;	/* 色过多 */
		}

		n = cm->ncds * 2;
		if (n > MAX_COLOR + 1)
			n = MAX_COLOR + 1;
		if (cm->cd == cm->cdspace)
		{
			newCd = (struct colordesc *) MALLOC(n * sizeof(struct colordesc));
			if (newCd != NULL)
				memcpy(VS(newCd), VS(cm->cdspace), cm->ncds *
					   sizeof(struct colordesc));
		}
		else
			newCd = (struct colordesc *)
				REALLOC(cm->cd, n * sizeof(struct colordesc));
		if (newCd == NULL)
		{
			CERR(REG_ESPACE);
			return COLORLESS;
		}
		cm->cd = newCd;
		cm->ncds = n;
		assert(cm->max < cm->ncds - 1);
		cm->max++;
		cd = &cm->cd[cm->max];
	}

	cd->nschrs = 0;
	cd->nuchrs = 0;
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;		/* 以防从未另行设置 */
	cd->flags = 0;

	return (color) (cd - cm->cd);
}

/*
 * freecolor - 释放一色（须无弧或子色）
 */
static void
freecolor(struct colormap *cm,
		  color co)
{
	struct colordesc *cd = &cm->cd[co];
	color		pco,
				nco;			/* 供空闲扫描 */

	assert(co >= 0);
	if (co == WHITE)
		return;

	assert(cd->arcs == NULL);
	assert(cd->sub == NOSUB);
	assert(cd->nschrs == 0);
	assert(cd->nuchrs == 0);
	cd->flags = FREECOL;

	if ((size_t) co == cm->max)
	{
		while (cm->max > WHITE && UNUSEDCOLOR(&cm->cd[cm->max]))
			cm->max--;
		assert(cm->free >= 0);
		while ((size_t) cm->free > cm->max)
			cm->free = cm->cd[cm->free].sub;
		if (cm->free > 0)
		{
			assert(cm->free < cm->max);
			pco = cm->free;
			nco = cm->cd[pco].sub;
			while (nco > 0)
				if ((size_t) nco > cm->max)
				{
					/* 从空闲表取下此项 */
					nco = cm->cd[nco].sub;
					cm->cd[pco].sub = nco;
				}
				else
				{
					assert(nco < cm->max);
					pco = nco;
					nco = cm->cd[pco].sub;
				}
		}
	}
	else
	{
		cd->sub = cm->free;
		cm->free = (color) (cd - cm->cd);
	}
}

/*
 * pseudocolor - 分配一伪色，由他处管理
 */
static color
pseudocolor(struct colormap *cm)
{
	color		co;
	struct colordesc *cd;

	co = newcolor(cm);
	if (CISERR())
		return COLORLESS;
	cd = &cm->cd[co];
	cd->nschrs = 0;
	cd->nuchrs = 1;				/* 假装它在高色图中 */
	cd->sub = NOSUB;
	cd->arcs = NULL;
	cd->firstchr = CHR_MIN;
	cd->flags = PSEUDO;
	return co;
}

/*
 * subcolor - 为此 chr 分配新子色（若需要）
 *
 * 仅适用于映射入低色图的 chr。
 */
static color
subcolor(struct colormap *cm, chr c)
{
	color		co;				/* c 的当前色 */
	color		sco;			/* 新子色 */

	assert(c <= MAX_SIMPLE_CHR);

	co = cm->locolormap[c - CHR_MIN];
	sco = newsub(cm, co);
	if (CISERR())
		return COLORLESS;
	assert(sco != COLORLESS);

	if (co == sco)				/* 已在开放子色中 */
		return co;				/* 其余多余 */
	cm->cd[co].nschrs--;
	if (cm->cd[sco].nschrs == 0)
		cm->cd[sco].firstchr = c;
	cm->cd[sco].nschrs++;
	cm->locolormap[c - CHR_MIN] = sco;
	return sco;
}

/*
 * subcolorhi - 为此色图项分配新子色（若需要）
 *
 * 处理同 subcolor()，但面向高色图中的项，
 * 它们未必对应恰好一个 chr 码。
 */
static color
subcolorhi(struct colormap *cm, color *pco)
{
	color		co;				/* 项的当前色 */
	color		sco;			/* 新子色 */

	co = *pco;
	sco = newsub(cm, co);
	if (CISERR())
		return COLORLESS;
	assert(sco != COLORLESS);

	if (co == sco)				/* 已在开放子色中 */
		return co;				/* 其余多余 */
	cm->cd[co].nuchrs--;
	cm->cd[sco].nuchrs++;
	*pco = sco;
	return sco;
}

/*
 * newsub - 为一色分配新子色（若需要）
 */
static color
newsub(struct colormap *cm,
	   color co)
{
	color		sco;			/* 新子色 */

	sco = cm->cd[co].sub;
	if (sco == NOSUB)
	{							/* 色无开放子色 */
		/* 优化：单引用色无需子色化 */
		if ((cm->cd[co].nschrs + cm->cd[co].nuchrs) == 1)
			return co;
		sco = newcolor(cm);		/* 须创建子色 */
		if (sco == COLORLESS)
		{
			assert(CISERR());
			return COLORLESS;
		}
		cm->cd[co].sub = sco;
		cm->cd[sco].sub = sco;	/* 开放子色指向自身 */
	}
	assert(sco != NOSUB);

	return sco;
}

/*
 * newhicolorrow - 在 hicolormap 取新行，自 oldrow 克隆
 *
 * 返回新行的数组下标。注意数组可能移动。
 */
static int
newhicolorrow(struct colormap *cm,
			  int oldrow)
{
	int			newrow = cm->hiarrayrows;
	color	   *newrowptr;
	int			i;

	/* 分配新数组行下标，必要时扩大存储 */
	if (newrow >= cm->maxarrayrows)
	{
		color	   *newarray;

		if (cm->maxarrayrows >= INT_MAX / (cm->hiarraycols * 2))
		{
			CERR(REG_ESPACE);
			return 0;
		}
		newarray = (color *) REALLOC(cm->hicolormap,
									 cm->maxarrayrows * 2 *
									 cm->hiarraycols * sizeof(color));
		if (newarray == NULL)
		{
			CERR(REG_ESPACE);
			return 0;
		}
		cm->hicolormap = newarray;
		cm->maxarrayrows *= 2;
	}
	cm->hiarrayrows++;

	/* 复制旧行 */
	newrowptr = &cm->hicolormap[newrow * cm->hiarraycols];
	memcpy(newrowptr,
		   &cm->hicolormap[oldrow * cm->hiarraycols],
		   cm->hiarraycols * sizeof(color));

	/* 增加色引用计数以反映新色图项 */
	for (i = 0; i < cm->hiarraycols; i++)
		cm->cd[newrowptr[i]].nuchrs++;

	return newrow;
}

/*
 * newhicolorcols - 在高色图中创建新的一组列
 *
 * 实质上将二维数组向右扩展并自拷一份。
 */
static void
newhicolorcols(struct colormap *cm)
{
	color	   *newarray;
	int			r,
				c;

	if (cm->hiarraycols >= INT_MAX / (cm->maxarrayrows * 2))
	{
		CERR(REG_ESPACE);
		return;
	}
	newarray = (color *) REALLOC(cm->hicolormap,
								 cm->maxarrayrows *
								 cm->hiarraycols * 2 * sizeof(color));
	if (newarray == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	cm->hicolormap = newarray;

	/* 向右复制既有列，并增加引用计数 */
	/* 须在数组中反向操作，因我们原地 realloc'd */
	for (r = cm->hiarrayrows - 1; r >= 0; r--)
	{
		color	   *oldrowptr = &newarray[r * cm->hiarraycols];
		color	   *newrowptr = &newarray[r * cm->hiarraycols * 2];
		color	   *newrowptr2 = newrowptr + cm->hiarraycols;

		for (c = 0; c < cm->hiarraycols; c++)
		{
			color		co = oldrowptr[c];

			newrowptr[c] = newrowptr2[c] = co;
			cm->cd[co].nuchrs++;
		}
	}

	cm->hiarraycols *= 2;
}

/*
 * subcolorcvec - 为 cvec 成员分配新子色并填入弧
 *
 * 对 cvec 所代表的每个 chr c，等价于在 NFA 上做
 * 对应的 newarc 调用。
 *
 * 注意典型情况下许多子色是相同的。
 * 虽 newarc() 会丢弃重复弧请求，但我们可先省
 * 些周期，一开始就不反复调用。这
 * 靠 lastsubcolor 状态变量实现。
 */
static void
subcolorcvec(struct vars *v,
			 struct cvec *cv,
			 struct state *lp,
			 struct state *rp)
{
	struct colormap *cm = v->cm;
	color		lastsubcolor = COLORLESS;
	chr			ch,
				from,
				to;
	const chr  *p;
	int			i;

	/* 普通字符 */
	for (p = cv->chrs, i = cv->nchrs; i > 0; p++, i--)
	{
		ch = *p;
		subcoloronechr(v, ch, lp, rp, &lastsubcolor);
		NOERR();
	}

	/* 以及范围 */
	for (p = cv->ranges, i = cv->nranges; i > 0; p += 2, i--)
	{
		from = *p;
		to = *(p + 1);
		if (from <= MAX_SIMPLE_CHR)
		{
			/* 一次处理一个简单字符 */
			chr			lim = (to <= MAX_SIMPLE_CHR) ? to : MAX_SIMPLE_CHR;

			while (from <= lim)
			{
				color		sco = subcolor(cm, from);

				NOERR();
				if (sco != lastsubcolor)
				{
					newarc(v->nfa, PLAIN, sco, lp, rp);
					NOERR();
					lastsubcolor = sco;
				}
				from++;
			}
		}
		/* 处理范围中高于 MAX_SIMPLE_CHR 的部分 */
		if (from < to)
			subcoloronerange(v, from, to, lp, rp, &lastsubcolor);
		else if (from == to)
			subcoloronechr(v, from, lp, rp, &lastsubcolor);
		NOERR();
	}

	/* 再处理cclass(若有) */
	if (cv->cclasscode >= 0)
	{
		int			classbit;
		color	   *pco;
		int			r,
					c;

		/* 若无 cclass 的列位赋值则扩大数组 */
		if (cm->classbits[cv->cclasscode] == 0)
		{
			cm->classbits[cv->cclasscode] = cm->hiarraycols;
			newhicolorcols(cm);
			NOERR();
		}
		/* 对相关列各项调 subcolorhi() 并造弧 */
		classbit = cm->classbits[cv->cclasscode];
		pco = cm->hicolormap;
		for (r = 0; r < cm->hiarrayrows; r++)
		{
			for (c = 0; c < cm->hiarraycols; c++)
			{
				if (c & classbit)
				{
					color		sco = subcolorhi(cm, pco);

					NOERR();
					/* 需要时加弧 */
					if (sco != lastsubcolor)
					{
						newarc(v->nfa, PLAIN, sco, lp, rp);
						NOERR();
						lastsubcolor = sco;
					}
				}
				pco++;
			}
		}
	}
}

/*
 * subcoloronechr - 为单 chr 做 subcolorcvec 的工作
 *
 * 本可交给 subcoloronerange，但利用
 * 单 chr 情形会更高效。且调用方乐见其
 * 能同时处理低与高 chr 码。
 */
static void
subcoloronechr(struct vars *v,
			   chr ch,
			   struct state *lp,
			   struct state *rp,
			   color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	colormaprange *newranges;
	int			numnewranges;
	colormaprange *oldrange;
	int			oldrangen;
	int			newrow;

	/* 低chr易处理情形 */
	if (ch <= MAX_SIMPLE_CHR)
	{
		color		sco = subcolor(cm, ch);

		NOERR();
		if (sco != *lastsubcolor)
		{
			newarc(v->nfa, PLAIN, sco, lp, rp);
			*lastsubcolor = sco;
		}
		return;
	}

	/*
	 * 潜在地，若给定 chr 落在某既有范围中间，
	 * 我们可能比现有多需两个 colormapranges。
	 */
	newranges = (colormaprange *)
		MALLOC((cm->numcmranges + 2) * sizeof(colormaprange));
	if (newranges == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	numnewranges = 0;

	/* 目标之前的范围不变 */
	for (oldrange = cm->cmranges, oldrangen = 0;
		 oldrangen < cm->numcmranges;
		 oldrange++, oldrangen++)
	{
		if (oldrange->cmax >= ch)
			break;
		newranges[numnewranges++] = *oldrange;
	}

	/* 将目标 chr 与当前范围匹配 */
	if (oldrangen >= cm->numcmranges || oldrange->cmin > ch)
	{
		/* chr 不属于任何既有范围，造一新范围 */
		newranges[numnewranges].cmin = ch;
		newranges[numnewranges].cmax = ch;
		/* 行状态应从"其余字符"行克隆 */
		newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
		numnewranges++;
	}
	else if (oldrange->cmin == oldrange->cmax)
	{
		/* 已有匹配该 chr 的单点范围 */
		newranges[numnewranges++] = *oldrange;
		newrow = oldrange->rownum;
		/* 现已完整处理完此旧范围 */
		oldrange++, oldrangen++;
	}
	else
	{
		/* chr 是此既有范围的子集，须拆分 */
		if (ch > oldrange->cmin)
		{
			/* 放出旧范围在chr前部分 */
			newranges[numnewranges].cmin = oldrange->cmin;
			newranges[numnewranges].cmax = ch - 1;
			newranges[numnewranges].rownum = oldrange->rownum;
			numnewranges++;
		}
		/* 将 chr 作单点范围放出，初自该范围克隆 */
		newranges[numnewranges].cmin = ch;
		newranges[numnewranges].cmax = ch;
		newranges[numnewranges].rownum = newrow =
			newhicolorrow(cm, oldrange->rownum);
		numnewranges++;
		if (ch < oldrange->cmax)
		{
			/* 放出旧范围在chr后部分 */
			newranges[numnewranges].cmin = ch + 1;
			newranges[numnewranges].cmax = oldrange->cmax;
			/* 若由旧范围造两个新范围则须克隆该行 */
			newranges[numnewranges].rownum =
				(ch > oldrange->cmin) ? newhicolorrow(cm, oldrange->rownum) :
				oldrange->rownum;
			numnewranges++;
		}
		/* 现已完整处理完此旧范围 */
		oldrange++, oldrangen++;
	}

	/* 更新 newrow 中的色并按需造弧 */
	subcoloronerow(v, newrow, lp, rp, lastsubcolor);

	/* 目标之后的范围不变 */
	for (; oldrangen < cm->numcmranges; oldrange++, oldrangen++)
	{
		newranges[numnewranges++] = *oldrange;
	}

	/* 断言原先空间估计足够 */
	assert(numnewranges <= (cm->numcmranges + 2));

	/* 最后，存回更新后的范围列表 */
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	cm->cmranges = newranges;
	cm->numcmranges = numnewranges;
}

/*
 * subcoloronerange - 为高范围做 subcolorcvec 的工作
 */
static void
subcoloronerange(struct vars *v,
				 chr from,
				 chr to,
				 struct state *lp,
				 struct state *rp,
				 color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	colormaprange *newranges;
	int			numnewranges;
	colormaprange *oldrange;
	int			oldrangen;
	int			newrow;

	/* 非高范围情形应由调用方处理 */
	assert(from > MAX_SIMPLE_CHR);
	assert(from < to);

	/*
	 * 潜在地，若有 N 个不相邻范围，我们最多可能
	 * 需要 2N+1 个结果范围（新范围横跨全部时）。
	 */
	newranges = (colormaprange *)
		MALLOC((cm->numcmranges * 2 + 1) * sizeof(colormaprange));
	if (newranges == NULL)
	{
		CERR(REG_ESPACE);
		return;
	}
	numnewranges = 0;

	/* 目标之前的范围不变 */
	for (oldrange = cm->cmranges, oldrangen = 0;
		 oldrangen < cm->numcmranges;
		 oldrange++, oldrangen++)
	{
		if (oldrange->cmax >= from)
			break;
		newranges[numnewranges++] = *oldrange;
	}

	/*
	 * 处理与目标（部分）重叠的范围。每处理这样一个
	 * 范围，就增大 "from"，从目标范围中去掉已
	 * 处理的字符。
	 */
	while (oldrangen < cm->numcmranges && oldrange->cmin <= to)
	{
		if (from < oldrange->cmin)
		{
			/* 处理新范围中无对应旧范围的部分 */
			newranges[numnewranges].cmin = from;
			newranges[numnewranges].cmax = oldrange->cmin - 1;
			/* 行状态应从"其余字符"行克隆 */
			newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
			numnewranges++;
			/* 更新 newrow 中的色并按需造弧 */
			subcoloronerow(v, newrow, lp, rp, lastsubcolor);
			/* 现已完整处理新范围在旧范围之前的部分 */
			from = oldrange->cmin;
		}

		if (from <= oldrange->cmin && to >= oldrange->cmax)
		{
			/* 旧范围完全包含于新范围，就地处理 */
			newranges[numnewranges++] = *oldrange;
			newrow = oldrange->rownum;
			from = oldrange->cmax + 1;
		}
		else
		{
			/* 旧范围有部分不与新范围重叠 */
			if (from > oldrange->cmin)
			{
				/* 放出旧范围在新范围之前的部分 */
				newranges[numnewranges].cmin = oldrange->cmin;
				newranges[numnewranges].cmax = from - 1;
				newranges[numnewranges].rownum = oldrange->rownum;
				numnewranges++;
			}
			/* 放出公共子范围，初自旧范围克隆 */
			newranges[numnewranges].cmin = from;
			newranges[numnewranges].cmax =
				(to < oldrange->cmax) ? to : oldrange->cmax;
			newranges[numnewranges].rownum = newrow =
				newhicolorrow(cm, oldrange->rownum);
			numnewranges++;
			if (to < oldrange->cmax)
			{
				/* 放出旧范围在新范围后部分 */
				newranges[numnewranges].cmin = to + 1;
				newranges[numnewranges].cmax = oldrange->cmax;
				/* 若由旧范围造两个新范围则须克隆该行 */
				newranges[numnewranges].rownum =
					(from > oldrange->cmin) ? newhicolorrow(cm, oldrange->rownum) :
					oldrange->rownum;
				numnewranges++;
			}
			from = oldrange->cmax + 1;
		}
		/* 更新 newrow 中的色并按需造弧 */
		subcoloronerow(v, newrow, lp, rp, lastsubcolor);
		/* 现已完整处理完此旧范围 */
		oldrange++, oldrangen++;
	}

	if (from <= to)
	{
		/* 处理新范围中无对应旧范围的部分 */
		newranges[numnewranges].cmin = from;
		newranges[numnewranges].cmax = to;
		/* 行状态应从"其余字符"行克隆 */
		newranges[numnewranges].rownum = newrow = newhicolorrow(cm, 0);
		numnewranges++;
		/* 更新 newrow 中的色并按需造弧 */
		subcoloronerow(v, newrow, lp, rp, lastsubcolor);
	}

	/* 目标之后的范围不变 */
	for (; oldrangen < cm->numcmranges; oldrange++, oldrangen++)
	{
		newranges[numnewranges++] = *oldrange;
	}

	/* 断言原先空间估计足够 */
	assert(numnewranges <= (cm->numcmranges * 2 + 1));

	/* 最后，存回更新后的范围列表 */
	if (cm->cmranges != NULL)
		FREE(cm->cmranges);
	cm->cmranges = newranges;
	cm->numcmranges = numnewranges;
}

/*
 * subcoloronerow - 为高色图中一新行做 subcolorcvec 的工作
 */
static void
subcoloronerow(struct vars *v,
			   int rownum,
			   struct state *lp,
			   struct state *rp,
			   color *lastsubcolor)
{
	struct colormap *cm = v->cm;
	color	   *pco;
	int			i;

	/* 对行内各项调 subcolorhi() 并造弧 */
	pco = &cm->hicolormap[rownum * cm->hiarraycols];
	for (i = 0; i < cm->hiarraycols; pco++, i++)
	{
		color		sco = subcolorhi(cm, pco);

		NOERR();
		/* 需要时造弧 */
		if (sco != *lastsubcolor)
		{
			newarc(v->nfa, PLAIN, sco, lp, rp);
			NOERR();
			*lastsubcolor = sco;
		}
	}
}

/*
 * okcolors - 将子色提升为完整色
 */
static void
okcolors(struct nfa *nfa,
		 struct colormap *cm)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	struct colordesc *scd;
	struct arc *a;
	color		co;
	color		sco;

	for (cd = cm->cd, co = 0; cd < end; cd++, co++)
	{
		sco = cd->sub;
		if (UNUSEDCOLOR(cd) || sco == NOSUB)
		{
			/* 无子色，无需再处理 */
		}
		else if (sco == co)
		{
			/* 是子色，交由父色处理 */
		}
		else if (cd->nschrs == 0 && cd->nuchrs == 0)
		{
			/*
			 * 父色现已空，故仅将其全部弧改标为
			 * 子色，然后释放父色。
			 *
			 * 如此简单重标弧是否妥当并不显然；
			 * 看似有造出重复弧之虞。我们基本
			 * 依赖这一假定：处理括号表达式时，
			 * 无法在括号两端之间同时造出某色
			 * 及其子色的弧。
			 */
			cd->sub = NOSUB;
			scd = &cm->cd[sco];
			assert(scd->nschrs > 0 || scd->nuchrs > 0);
			assert(scd->sub == sco);
			scd->sub = NOSUB;
			while ((a = cd->arcs) != NULL)
			{
				assert(a->co == co);
				uncolorchain(cm, a);
				a->co = sco;
				colorchain(cm, a);
			}
			freecolor(cm, co);
		}
		else
		{
			/* 父色的弧须获得并行的子色弧 */
			cd->sub = NOSUB;
			scd = &cm->cd[sco];
			assert(scd->nschrs > 0 || scd->nuchrs > 0);
			assert(scd->sub == sco);
			scd->sub = NOSUB;
			for (a = cd->arcs; a != NULL; a = a->colorchain)
			{
				assert(a->co == co);
				newarc(nfa, a->type, sco, a->from, a->to);
			}
		}
	}
}

/*
 * colorchain - 将此弧加入其色的色链
 */
static void
colorchain(struct colormap *cm,
		   struct arc *a)
{
	struct colordesc *cd = &cm->cd[a->co];

	assert(a->co >= 0);
	if (cd->arcs != NULL)
		cd->arcs->colorchainRev = a;
	a->colorchain = cd->arcs;
	a->colorchainRev = NULL;
	cd->arcs = a;
}

/*
 * uncolorchain - 将此弧从其色的色链删除
 */
static void
uncolorchain(struct colormap *cm,
			 struct arc *a)
{
	struct colordesc *cd = &cm->cd[a->co];
	struct arc *aa = a->colorchainRev;

	assert(a->co >= 0);
	if (aa == NULL)
	{
		assert(cd->arcs == a);
		cd->arcs = a->colorchain;
	}
	else
	{
		assert(aa->colorchain == a);
		aa->colorchain = a->colorchain;
	}
	if (a->colorchain != NULL)
		a->colorchain->colorchainRev = aa;
	a->colorchain = NULL;		/* 偏执 */
	a->colorchainRev = NULL;
}

/*
 * rainbow - 在指定状态间加除一色外全部完整色的弧
 *
 * 若无例外色，现只生成一条
 * 标为彩虹的弧，省去日后大量弧折腾。
 */
static void
rainbow(struct nfa *nfa,
		struct colormap *cm,
		int type,
		color but,				/* 无例外则为 COLORLESS */
		struct state *from,
		struct state *to)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	color		co;

	if (but == COLORLESS)
	{
		newarc(nfa, type, RAINBOW, from, to);
		return;
	}

	/* 只好硬做。跳过子色、伪色与 "but" */
	for (cd = cm->cd, co = 0; cd < end && !CISERR(); cd++, co++)
		if (!UNUSEDCOLOR(cd) && cd->sub != co && co != but &&
			!(cd->flags & PSEUDO))
			newarc(nfa, type, co, from, to);
}

/*
 * colorcomplement - 加互补色的弧
 *
 * 我们加入所有非伪色、且不匹配 "of"
 * 状态任一PLAIN出弧之色的弧。
 *
 * 调用序列宜与 cloneouts() 协调。
 */
static void
colorcomplement(struct nfa *nfa,
				struct colormap *cm,
				int type,
				struct state *of,
				struct state *from,
				struct state *to)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(cm);
	color		co;
	struct arc *a;

	assert(of != from);

	/*
	 * 彩虹弧匹配全部色，使补集为空。但
	 * 无法干脆不造弧就返回，否则会使 NFA
	 * 断开，破坏日后任何 delsub()。改为造
	 * 一条 CANTMATCH 弧。并置 HASCANTMATCH 标志，
	 * 知须在 NFA 优化开头清理之。
	 */
	if (findarc(of, PLAIN, RAINBOW) != NULL)
	{
		newarc(nfa, CANTMATCH, 0, from, to);
		nfa->flags |= HASCANTMATCH;
		return;
	}

	/* 否则，暂时标记 "of" 出弧中出现的色 */
	for (a = of->outs; a != NULL; a = a->outchain)
	{
		if (a->type == PLAIN)
		{
			assert(a->co >= 0);
			cd = &cm->cd[a->co];
			assert(!UNUSEDCOLOR(cd));
			cd->flags |= COLMARK;
		}

		/*
		 * 没有再取补色集的语法，故此处无法
		 * 见到 CANTMATCH 弧。
		 */
		assert(a->type != CANTMATCH);
	}

	/* 扫描各色，清暂记，为未标记色加弧 */
	for (cd = cm->cd, co = 0; cd < end && !CISERR(); cd++, co++)
	{
		if (cd->flags & COLMARK)
			cd->flags &= ~COLMARK;
		else if (!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO))
			newarc(nfa, type, co, from, to);
	}
}


#ifdef REG_DEBUG

/*
 * dumpcolors - 调试输出
 */
static void
dumpcolors(struct colormap *cm,
		   FILE *f)
{
	struct colordesc *cd;
	struct colordesc *end;
	color		co;
	chr			c;

	fprintf(f, "max %ld\n", (long) cm->max);
	end = CDEND(cm);
	for (cd = cm->cd + 1, co = 1; cd < end; cd++, co++) /* 跳0 */
	{
		if (!UNUSEDCOLOR(cd))
		{
			assert(cd->nschrs > 0 || cd->nuchrs > 0);
			if (cd->flags & PSEUDO)
				fprintf(f, "#%2ld(ps): ", (long) co);
			else
				fprintf(f, "#%2ld(%2d): ", (long) co, cd->nschrs + cd->nuchrs);

			/*
			 * 可惜，下一段很难做得更高效。
			 */
			for (c = CHR_MIN; c <= MAX_SIMPLE_CHR; c++)
				if (GETCOLOR(cm, c) == co)
					dumpchr(c, f);
			fprintf(f, "\n");
		}
	}
	/* 若高色图含任何有趣内容则转储之 */
	if (cm->hiarrayrows > 1 || cm->hiarraycols > 1)
	{
		int			r,
					c;
		const color *rowptr;

		fprintf(f, "other:\t");
		for (c = 0; c < cm->hiarraycols; c++)
		{
			fprintf(f, "\t%ld", (long) cm->hicolormap[c]);
		}
		fprintf(f, "\n");
		for (r = 0; r < cm->numcmranges; r++)
		{
			dumpchr(cm->cmranges[r].cmin, f);
			fprintf(f, "..");
			dumpchr(cm->cmranges[r].cmax, f);
			fprintf(f, ":");
			rowptr = &cm->hicolormap[cm->cmranges[r].rownum * cm->hiarraycols];
			for (c = 0; c < cm->hiarraycols; c++)
			{
				fprintf(f, "\t%ld", (long) rowptr[c]);
			}
			fprintf(f, "\n");
		}
	}
}

/*
 * dumpchr - 打印 chr
 *
 * 偏字符中心，但对调试够用。
 */
static void
dumpchr(chr c,
		FILE *f)
{
	if (c == '\\')
		fprintf(f, "\\\\");
	else if (c > ' ' && c <= '~')
		putc((char) c, f);
	else
		fprintf(f, "\\u%04lx", (long) c);
}

#endif							/* REG_DEBUG */
