/*
 * NFA 工具。
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
 * src/backend/regex/regc_nfa.c
 *
 *
 * 有一两处按理本应在此的内容
 * 实际在 color.c，归因于色链中若干纠缠
 * 的关系。
 */

#define NISERR()	VISERR(nfa->v)
#define NERR(e)		VERR(nfa->v, (e))


/*
 * newnfa - 建立 NFA
 */
static struct nfa *				/* 该 NFA 或 NULL */
newnfa(struct vars *v,
	   struct colormap *cm,
	   struct nfa *parent)		/* 主 NFA 则为 NULL */
{
	struct nfa *nfa;

	nfa = (struct nfa *) MALLOC(sizeof(struct nfa));
	if (nfa == NULL)
	{
		ERR(REG_ESPACE);
		return NULL;
	}

	/* 使 NFA 最低限度有效，以便 freenfa() 行为正常 */
	nfa->states = NULL;
	nfa->slast = NULL;
	nfa->freestates = NULL;
	nfa->freearcs = NULL;
	nfa->lastsb = NULL;
	nfa->lastab = NULL;
	nfa->lastsbused = 0;
	nfa->lastabused = 0;
	nfa->nstates = 0;
	nfa->cm = cm;
	nfa->v = v;
	nfa->bos[0] = nfa->bos[1] = COLORLESS;
	nfa->eos[0] = nfa->eos[1] = COLORLESS;
	nfa->flags = 0;
	nfa->minmatchall = nfa->maxmatchall = -1;
	nfa->parent = parent;		/* 在 newfstate 前使 parent 有效。 */

	/* 创建所需基础设施 */
	nfa->post = newfstate(nfa, '@');	/* 编号 0 */
	nfa->pre = newfstate(nfa, '>'); /* 编号 1 */
	nfa->init = newstate(nfa);	/* 稍后可能失效 */
	nfa->final = newstate(nfa);
	if (ISERR())
	{
		freenfa(nfa);
		return NULL;
	}
	rainbow(nfa, nfa->cm, PLAIN, COLORLESS, nfa->pre, nfa->init);
	newarc(nfa, '^', 1, nfa->pre, nfa->init);
	newarc(nfa, '^', 0, nfa->pre, nfa->init);
	rainbow(nfa, nfa->cm, PLAIN, COLORLESS, nfa->final, nfa->post);
	newarc(nfa, '$', 1, nfa->final, nfa->post);
	newarc(nfa, '$', 0, nfa->final, nfa->post);

	if (ISERR())
	{
		freenfa(nfa);
		return NULL;
	}
	return nfa;
}

/*
 * freenfa - 释放整个 NFA
 */
static void
freenfa(struct nfa *nfa)
{
	struct statebatch *sb;
	struct statebatch *sbnext;
	struct arcbatch *ab;
	struct arcbatch *abnext;

	for (sb = nfa->lastsb; sb != NULL; sb = sbnext)
	{
		sbnext = sb->next;
		nfa->v->spaceused -= STATEBATCHSIZE(sb->nstates);
		FREE(sb);
	}
	nfa->lastsb = NULL;
	for (ab = nfa->lastab; ab != NULL; ab = abnext)
	{
		abnext = ab->next;
		nfa->v->spaceused -= ARCBATCHSIZE(ab->narcs);
		FREE(ab);
	}
	nfa->lastab = NULL;

	nfa->nstates = -1;
	FREE(nfa);
}

/*
 * newstate - 分配 flag 为零的 NFA 状态
 */
static struct state *			/* 出错则NULL */
newstate(struct nfa *nfa)
{
	struct state *s;

	/*
	 * 此处便于在正则编译期间检查操作取消，
	 * 因无路径会很久不新建
	 * 状态或弧
	 */
	INTERRUPT(nfa->v->re);

	/* 先回收 freelist 上的任何项 */
	if (nfa->freestates != NULL)
	{
		s = nfa->freestates;
		nfa->freestates = s->next;
	}
	/* 否则，最后一个 statebatch 还有剩余吗？ */
	else if (nfa->lastsb != NULL && nfa->lastsbused < nfa->lastsb->nstates)
	{
		s = &nfa->lastsb->s[nfa->lastsbused++];
	}
	/* 否则，需要分配新的 statebatch */
	else
	{
		struct statebatch *newSb;
		size_t		nstates;

		if (nfa->v->spaceused >= REG_MAX_COMPILE_SPACE)
		{
			NERR(REG_ETOOBIG);
			return NULL;
		}
		nstates = (nfa->lastsb != NULL) ? nfa->lastsb->nstates * 2 : FIRSTSBSIZE;
		if (nstates > MAXSBSIZE)
			nstates = MAXSBSIZE;
		newSb = (struct statebatch *) MALLOC(STATEBATCHSIZE(nstates));
		if (newSb == NULL)
		{
			NERR(REG_ESPACE);
			return NULL;
		}
		nfa->v->spaceused += STATEBATCHSIZE(nstates);
		newSb->nstates = nstates;
		newSb->next = nfa->lastsb;
		nfa->lastsb = newSb;
		nfa->lastsbused = 1;
		s = &newSb->s[0];
	}

	assert(nfa->nstates >= 0);
	s->no = nfa->nstates++;
	s->flag = 0;
	if (nfa->states == NULL)
		nfa->states = s;
	s->nins = 0;
	s->ins = NULL;
	s->nouts = 0;
	s->outs = NULL;
	s->tmp = NULL;
	s->next = NULL;
	if (nfa->slast != NULL)
	{
		assert(nfa->slast->next == NULL);
		nfa->slast->next = s;
	}
	s->prev = nfa->slast;
	nfa->slast = s;
	return s;
}

/*
 * newfstate - 分配带指定 flag 值的 NFA 状态
 */
static struct state *			/* 出错则NULL */
newfstate(struct nfa *nfa, int flag)
{
	struct state *s;

	s = newstate(nfa);
	if (s != NULL)
		s->flag = (char) flag;
	return s;
}

/*
 * dropstate - 删除状态的 inarcs 与 outarcs 并释放
 */
static void
dropstate(struct nfa *nfa,
		  struct state *s)
{
	struct arc *a;

	while ((a = s->ins) != NULL)
		freearc(nfa, a);
	while ((a = s->outs) != NULL)
		freearc(nfa, a);
	freestate(nfa, s);
}

/*
 * freestate - 释放无入弧与出弧的状态
 */
static void
freestate(struct nfa *nfa,
		  struct state *s)
{
	assert(s != NULL);
	assert(s->nins == 0 && s->nouts == 0);

	s->no = FREESTATE;
	s->flag = 0;
	if (s->next != NULL)
		s->next->prev = s->prev;
	else
	{
		assert(s == nfa->slast);
		nfa->slast = s->prev;
	}
	if (s->prev != NULL)
		s->prev->next = s->next;
	else
	{
		assert(s == nfa->states);
		nfa->states = s->next;
	}
	s->prev = NULL;
	s->next = nfa->freestates;	/* 不删除，放入空闲链表 */
	nfa->freestates = s;
}

/*
 * newarc - 在 NFA 内建立新弧
 *
 * 本函数确保不会创建重复弧。
 * 通常我们从不希望有重复。
 *
 * 但是：原则上，彩虹弧与任何普通弧均冗余，
 * 除非该弧针对伪色；但我们不去识别
 * 该冗余，无论此处还是 moveins() 等同类操作。
 * 伪色因素使代价高于看似值得的程度。
 */
static void
newarc(struct nfa *nfa,
	   int t,
	   color co,
	   struct state *from,
	   struct state *to)
{
	struct arc *a;

	assert(from != NULL && to != NULL);

	/*
	 * 此处便于在正则编译期间检查操作取消，
	 * 因无路径会很久不新建
	 * 状态或弧
	 */
	INTERRUPT(nfa->v->re);

	/* 查重复弧，选用较短的那条链 */
	if (from->nouts <= to->nins)
	{
		for (a = from->outs; a != NULL; a = a->outchain)
			if (a->to == to && a->co == co && a->type == t)
				return;
	}
	else
	{
		for (a = to->ins; a != NULL; a = a->inchain)
			if (a->from == from && a->co == co && a->type == t)
				return;
	}

	/* 无重复故建弧 */
	createarc(nfa, t, co, from, to);
}

/*
 * createarc - 在 NFA 内创建新弧
 *
 * 本函数*仅*可在已确认不存在
 * 相同弧(同type/色/from/to)后用。
 */
static void
createarc(struct nfa *nfa,
		  int t,
		  color co,
		  struct state *from,
		  struct state *to)
{
	struct arc *a;

	a = allocarc(nfa);
	if (NISERR())
		return;
	assert(a != NULL);

	a->type = t;
	a->co = co;
	a->to = to;
	a->from = from;

	/*
	 * 把新弧放在链首而非链尾；此处更
	 * 简单，且 freearc() 代价相同。另见 
	 * moveins() 及其同伴与 fixempties() 的逻辑。
	 */
	a->inchain = to->ins;
	a->inchainRev = NULL;
	if (to->ins)
		to->ins->inchainRev = a;
	to->ins = a;
	a->outchain = from->outs;
	a->outchainRev = NULL;
	if (from->outs)
		from->outs->outchainRev = a;
	from->outs = a;

	from->nouts++;
	to->nins++;

	if (COLORED(a) && nfa->parent == NULL)
		colorchain(nfa->cm, a);
}

/*
 * allocarc - 在 NFA 内分配新弧
 */
static struct arc *				/* 失败则NULL */
allocarc(struct nfa *nfa)
{
	struct arc *a;

	/* 先回收 freelist 上的任何项 */
	if (nfa->freearcs != NULL)
	{
		a = nfa->freearcs;
		nfa->freearcs = a->freechain;
	}
	/* 否则，最后一个 arcbatch 还有剩余吗？ */
	else if (nfa->lastab != NULL && nfa->lastabused < nfa->lastab->narcs)
	{
		a = &nfa->lastab->a[nfa->lastabused++];
	}
	/* 否则，需要分配新的 arcbatch */
	else
	{
		struct arcbatch *newAb;
		size_t		narcs;

		if (nfa->v->spaceused >= REG_MAX_COMPILE_SPACE)
		{
			NERR(REG_ETOOBIG);
			return NULL;
		}
		narcs = (nfa->lastab != NULL) ? nfa->lastab->narcs * 2 : FIRSTABSIZE;
		if (narcs > MAXABSIZE)
			narcs = MAXABSIZE;
		newAb = (struct arcbatch *) MALLOC(ARCBATCHSIZE(narcs));
		if (newAb == NULL)
		{
			NERR(REG_ESPACE);
			return NULL;
		}
		nfa->v->spaceused += ARCBATCHSIZE(narcs);
		newAb->narcs = narcs;
		newAb->next = nfa->lastab;
		nfa->lastab = newAb;
		nfa->lastabused = 1;
		a = &newAb->a[0];
	}

	return a;
}

/*
 * freearc - 释放弧
 */
static void
freearc(struct nfa *nfa,
		struct arc *victim)
{
	struct state *from = victim->from;
	struct state *to = victim->to;
	struct arc *predecessor;

	assert(victim->type != 0);

	/* 必要时从色链取下 */
	if (COLORED(victim) && nfa->parent == NULL)
		uncolorchain(nfa->cm, victim);

	/* 从源的出链取下 */
	assert(from != NULL);
	predecessor = victim->outchainRev;
	if (predecessor == NULL)
	{
		assert(from->outs == victim);
		from->outs = victim->outchain;
	}
	else
	{
		assert(predecessor->outchain == victim);
		predecessor->outchain = victim->outchain;
	}
	if (victim->outchain != NULL)
	{
		assert(victim->outchain->outchainRev == victim);
		victim->outchain->outchainRev = predecessor;
	}
	from->nouts--;

	/* 从目标的入链取下 */
	assert(to != NULL);
	predecessor = victim->inchainRev;
	if (predecessor == NULL)
	{
		assert(to->ins == victim);
		to->ins = victim->inchain;
	}
	else
	{
		assert(predecessor->inchain == victim);
		predecessor->inchain = victim->inchain;
	}
	if (victim->inchain != NULL)
	{
		assert(victim->inchain->inchainRev == victim);
		victim->inchain->inchainRev = predecessor;
	}
	to->nins--;

	/* 清理并放入 NFA 空闲链表 */
	victim->type = 0;
	victim->from = NULL;		/* 防范… */
	victim->to = NULL;
	victim->inchain = NULL;
	victim->inchainRev = NULL;
	victim->outchain = NULL;
	victim->outchainRev = NULL;
	victim->freechain = nfa->freearcs;
	nfa->freearcs = victim;
}

/*
 * changearcsource - 将弧改接到不同 from 状态
 *
 * 调用方须已确认不存在预先的重复弧。
 */
static void
changearcsource(struct arc *a, struct state *newfrom)
{
	struct state *oldfrom = a->from;
	struct arc *predecessor;

	assert(oldfrom != newfrom);

	/* 从旧源的出链取下 */
	assert(oldfrom != NULL);
	predecessor = a->outchainRev;
	if (predecessor == NULL)
	{
		assert(oldfrom->outs == a);
		oldfrom->outs = a->outchain;
	}
	else
	{
		assert(predecessor->outchain == a);
		predecessor->outchain = a->outchain;
	}
	if (a->outchain != NULL)
	{
		assert(a->outchain->outchainRev == a);
		a->outchain->outchainRev = predecessor;
	}
	oldfrom->nouts--;

	a->from = newfrom;

	/* 把它接到新源出链前端 */
	a->outchain = newfrom->outs;
	a->outchainRev = NULL;
	if (newfrom->outs)
		newfrom->outs->outchainRev = a;
	newfrom->outs = a;
	newfrom->nouts++;
}

/*
 * changearctarget - 将弧改接到不同 to 状态
 *
 * 调用方须已确认不存在预先的重复弧。
 */
static void
changearctarget(struct arc *a, struct state *newto)
{
	struct state *oldto = a->to;
	struct arc *predecessor;

	assert(oldto != newto);

	/* 从旧目标的入链取下 */
	assert(oldto != NULL);
	predecessor = a->inchainRev;
	if (predecessor == NULL)
	{
		assert(oldto->ins == a);
		oldto->ins = a->inchain;
	}
	else
	{
		assert(predecessor->inchain == a);
		predecessor->inchain = a->inchain;
	}
	if (a->inchain != NULL)
	{
		assert(a->inchain->inchainRev == a);
		a->inchain->inchainRev = predecessor;
	}
	oldto->nins--;

	a->to = newto;

	/* 把它接到新目标入链前端 */
	a->inchain = newto->ins;
	a->inchainRev = NULL;
	if (newto->ins)
		newto->ins->inchainRev = a;
	newto->ins = a;
	newto->nins++;
}

/*
 * hasnonemptyout - 状态有非 EMPTY 出弧吗？
 */
static int
hasnonemptyout(struct state *s)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->type != EMPTY)
			return 1;
	}
	return 0;
}

/*
 * findarc - 自给定源找指定 type 与色的弧（若有）
 * 若有多条此类弧，结果是随机的。
 */
static struct arc *
findarc(struct state *s,
		int type,
		color co)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
		if (a->type == type && a->co == co)
			return a;
	return NULL;
}

/*
 * cparc - 在 NFA 内分配新弧，从旧弧复制细节
 */
static void
cparc(struct nfa *nfa,
	  struct arc *oa,
	  struct state *from,
	  struct state *to)
{
	newarc(nfa, oa->type, oa->co, from, to);
}

/*
 * sortins - 按 from/色/type 排序状态的入弧
 */
static void
sortins(struct nfa *nfa,
		struct state *s)
{
	struct arc **sortarray;
	struct arc *a;
	int			n = s->nins;
	int			i;

	if (n <= 1)
		return;					/* 无事可做 */
	/* 制作弧指针数组并 */
	sortarray = (struct arc **) MALLOC(n * sizeof(struct arc *));
	if (sortarray == NULL)
	{
		NERR(REG_ESPACE);
		return;
	}
	i = 0;
	for (a = s->ins; a != NULL; a = a->inchain)
		sortarray[i++] = a;
	assert(i == n);
	/* 排序该数组 */
	qsort(sortarray, n, sizeof(struct arc *), sortins_cmp);
	/* 再按序重建弧表 */
	/* 对首尾项做特例处理似乎值得，以简化循环 */
	a = sortarray[0];
	s->ins = a;
	a->inchain = sortarray[1];
	a->inchainRev = NULL;
	for (i = 1; i < n - 1; i++)
	{
		a = sortarray[i];
		a->inchain = sortarray[i + 1];
		a->inchainRev = sortarray[i - 1];
	}
	a = sortarray[i];
	a->inchain = NULL;
	a->inchainRev = sortarray[i - 1];
	FREE(sortarray);
}

static int
sortins_cmp(const void *a, const void *b)
{
	const struct arc *aa = *((const struct arc *const *) a);
	const struct arc *bb = *((const struct arc *const *) b);

	/* 按最可能不同的次序检查各字段 */
	if (aa->from->no < bb->from->no)
		return -1;
	if (aa->from->no > bb->from->no)
		return 1;
	if (aa->co < bb->co)
		return -1;
	if (aa->co > bb->co)
		return 1;
	if (aa->type < bb->type)
		return -1;
	if (aa->type > bb->type)
		return 1;
	return 0;
}

/*
 * sortouts - 按 to/色/type 排序状态的出弧
 */
static void
sortouts(struct nfa *nfa,
		 struct state *s)
{
	struct arc **sortarray;
	struct arc *a;
	int			n = s->nouts;
	int			i;

	if (n <= 1)
		return;					/* 无事可做 */
	/* 制作弧指针数组并 */
	sortarray = (struct arc **) MALLOC(n * sizeof(struct arc *));
	if (sortarray == NULL)
	{
		NERR(REG_ESPACE);
		return;
	}
	i = 0;
	for (a = s->outs; a != NULL; a = a->outchain)
		sortarray[i++] = a;
	assert(i == n);
	/* 排序该数组 */
	qsort(sortarray, n, sizeof(struct arc *), sortouts_cmp);
	/* 再按序重建弧表 */
	/* 对首尾项做特例处理似乎值得，以简化循环 */
	a = sortarray[0];
	s->outs = a;
	a->outchain = sortarray[1];
	a->outchainRev = NULL;
	for (i = 1; i < n - 1; i++)
	{
		a = sortarray[i];
		a->outchain = sortarray[i + 1];
		a->outchainRev = sortarray[i - 1];
	}
	a = sortarray[i];
	a->outchain = NULL;
	a->outchainRev = sortarray[i - 1];
	FREE(sortarray);
}

static int
sortouts_cmp(const void *a, const void *b)
{
	const struct arc *aa = *((const struct arc *const *) a);
	const struct arc *bb = *((const struct arc *const *) b);

	/* 按最可能不同的次序检查各字段 */
	if (aa->to->no < bb->to->no)
		return -1;
	if (aa->to->no > bb->to->no)
		return 1;
	if (aa->co < bb->co)
		return -1;
	if (aa->co > bb->co)
		return 1;
	if (aa->type < bb->type)
		return -1;
	if (aa->type > bb->type)
		return 1;
	return 0;
}

/*
 * 关于用逐弧操作还是排序/归并的
 * 共同判定逻辑。源弧很少时，无论目标弧表多大
 * 都收不回排序目标弧表的开销。
 * 否则，把逐弧比较次数限到约 1000（
 * 此值有些任意，但盈亏平衡点大概
 * 本就随机器而异）。
 */
#define BULK_ARC_OP_USE_SORT(nsrcarcs, ndestarcs) \
	((nsrcarcs) < 4 ? 0 : ((nsrcarcs) > 32 || (ndestarcs) > 32))

/*
 * moveins - 把一状态全部入弧移到另一状态
 *
 * 或许以为就地更新现有弧更好，
 * 若非需要抑制重复，你是对的；
 * 抑制重复使直接新建更省事，
 * 以利用 newarc 内建的抑制。
 *
 * 但若要处理的弧极多，逐条查重
 * 会太慢。那时我们改为排序并归并
 * 弧表，然后确可就地更新这些弧。
 *
 * 另一方面，也常带着
 * 尚无既有入弧的全新 newState 调用。那时
 * 不必去重，可盲目移动全部弧。
 */
static void
moveins(struct nfa *nfa,
		struct state *oldState,
		struct state *newState)
{
	assert(oldState != newState);

	if (newState->nins == 0)
	{
		/* 无需去重 */
		struct arc *a;

		while ((a = oldState->ins) != NULL)
		{
			createarc(nfa, a->type, a->co, a->from, newState);
			freearc(nfa, a);
		}
	}
	else if (!BULK_ARC_OP_USE_SORT(oldState->nins, newState->nins))
	{
		/* 弧不太多时，逐条处理即可 */
		struct arc *a;

		while ((a = oldState->ins) != NULL)
		{
			cparc(nfa, a, a->from, newState);
			freearc(nfa, a);
		}
	}
	else
	{
		/*
		 * 弧很多时用排序-归并。注意 changearctarget()
		 * 会把弧接到 newState 链前端，故不
		 * 打断我们对链上已排序部分的遍历。
		 */
		struct arc *oa;
		struct arc *na;

		/*
		 * 本路径绕过 newarc()，最好纳入
		 * 取消检查
		 */
		INTERRUPT(nfa->v->re);

		sortins(nfa, oldState);
		sortins(nfa, newState);
		if (NISERR())
			return;				/* 排序可能已失败 */
		oa = oldState->ins;
		na = newState->ins;
		while (oa != NULL && na != NULL)
		{
			struct arc *a = oa;

			switch (sortins_cmp(&oa, &na))
			{
				case -1:
					/* newState 无与 oa 匹配者 */
					oa = oa->inchain;

					/*
					 * 与其 createarc+freearc，不如直接拆链
					 * 再挂回既有弧结构。
					 */
					changearctarget(a, newState);
					break;
				case 0:
					/* 匹配，两表都前进 */
					oa = oa->inchain;
					na = na->inchain;
					/* 并丢掉 oldState 的重复弧 */
					freearc(nfa, a);
					break;
				case +1:
					/* 只推进 na；oa 稍后可能匹配 */
					na = na->inchain;
					break;
				default:
					assert(NOTREACHED);
			}
		}
		while (oa != NULL)
		{
			/* newState 无与 oa 匹配者 */
			struct arc *a = oa;

			oa = oa->inchain;
			changearctarget(a, newState);
		}
	}

	assert(oldState->nins == 0);
	assert(oldState->ins == NULL);
}

/*
 * copyins - 复制状态入弧到另一状态
 *
 * moveins() 的注释此处同样适用。但按当前
 * 用法，*仅*对全新目标状态调用，故
 * 只会走到"无需去重"那条路径。
 * 其余以 #ifdef 关掉，以备日后需要。
 */
static void
copyins(struct nfa *nfa,
		struct state *oldState,
		struct state *newState)
{
	assert(oldState != newState);
	assert(newState->nins == 0);	/* 见上方注释 */

	if (newState->nins == 0)
	{
		/* 无需去重 */
		struct arc *a;

		for (a = oldState->ins; a != NULL; a = a->inchain)
			createarc(nfa, a->type, a->co, a->from, newState);
	}
#ifdef NOT_USED					/* 见上方注释 */
	else if (!BULK_ARC_OP_USE_SORT(oldState->nins, newState->nins))
	{
		/* 弧不太多时，逐条处理即可 */
		struct arc *a;

		for (a = oldState->ins; a != NULL; a = a->inchain)
			cparc(nfa, a, a->from, newState);
	}
	else
	{
		/*
		 * 弧很多时用排序-归并。注意 createarc()
		 * 会把新弧接到 newState 链前端，故不
		 * 打断我们对链上已排序部分的遍历。
		 */
		struct arc *oa;
		struct arc *na;

		/*
		 * 本路径绕过 newarc()，最好纳入
		 * 取消检查
		 */
		INTERRUPT(nfa->v->re);

		sortins(nfa, oldState);
		sortins(nfa, newState);
		if (NISERR())
			return;				/* 排序可能已失败 */
		oa = oldState->ins;
		na = newState->ins;
		while (oa != NULL && na != NULL)
		{
			struct arc *a = oa;

			switch (sortins_cmp(&oa, &na))
			{
				case -1:
					/* newState 无与 oa 匹配者 */
					oa = oa->inchain;
					createarc(nfa, a->type, a->co, a->from, newState);
					break;
				case 0:
					/* 匹配，两表都前进 */
					oa = oa->inchain;
					na = na->inchain;
					break;
				case +1:
					/* 只推进 na；oa 稍后可能匹配 */
					na = na->inchain;
					break;
				default:
					assert(NOTREACHED);
			}
		}
		while (oa != NULL)
		{
			/* newState 无与 oa 匹配者 */
			struct arc *a = oa;

			oa = oa->inchain;
			createarc(nfa, a->type, a->co, a->from, newState);
		}
	}
#endif							/* NOT_USED */
}

/*
 * mergeins - 把一组 inarcs 归并入一状态
 *
 * 很像 copyins，但源弧列在数组中，
 * 且不保证唯一。可以改写数组内容。
 */
static void
mergeins(struct nfa *nfa,
		 struct state *s,
		 struct arc **arcarray,
		 int arccount)
{
	struct arc *na;
	int			i;
	int			j;

	if (arccount <= 0)
		return;

	/*
	 * 本路径绕过 newarc()，最好纳入
	 * 取消检查
	 */
	INTERRUPT(nfa->v->re);

	/* 排序既有 inarcs 与拟新增者 */
	sortins(nfa, s);
	if (NISERR())
		return;					/* 排序可能已失败 */

	qsort(arcarray, arccount, sizeof(struct arc *), sortins_cmp);

	/*
	 * arcarray 很可能含重复，必须消除。（这
	 * 本可并入下一循环，但不值得费事。）
	 */
	j = 0;
	for (i = 1; i < arccount; i++)
	{
		switch (sortins_cmp(&arcarray[j], &arcarray[i]))
		{
			case -1:
				/* 非dup */
				arcarray[++j] = arcarray[i];
				break;
			case 0:
				/* dup */
				break;
			default:
				/* 出错 */
				assert(NOTREACHED);
		}
	}
	arccount = j + 1;

	/*
	 * 现归并入 s 的入链。注意 createarc() 会把
	 * 新弧接到 s 链前端，故不打断我们对
	 * 已排链段的遍历。
	 */
	i = 0;
	na = s->ins;
	while (i < arccount && na != NULL)
	{
		struct arc *a = arcarray[i];

		switch (sortins_cmp(&a, &na))
		{
			case -1:
				/* s 无与 a 匹配者 */
				createarc(nfa, a->type, a->co, a->from, s);
				i++;
				break;
			case 0:
				/* 匹配，两表都前进 */
				i++;
				na = na->inchain;
				break;
			case +1:
				/* 只推进 na；数组稍后可能匹配 */
				na = na->inchain;
				break;
			default:
				assert(NOTREACHED);
		}
	}
	while (i < arccount)
	{
		/* s 无与 a 匹配者 */
		struct arc *a = arcarray[i];

		createarc(nfa, a->type, a->co, a->from, s);
		i++;
	}
}

/*
 * moveouts - 把一状态全部出弧移到另一状态
 *
 * 见 moveins() 注释
 */
static void
moveouts(struct nfa *nfa,
		 struct state *oldState,
		 struct state *newState)
{
	assert(oldState != newState);

	if (newState->nouts == 0)
	{
		/* 无需去重 */
		struct arc *a;

		while ((a = oldState->outs) != NULL)
		{
			createarc(nfa, a->type, a->co, newState, a->to);
			freearc(nfa, a);
		}
	}
	else if (!BULK_ARC_OP_USE_SORT(oldState->nouts, newState->nouts))
	{
		/* 弧不太多时，逐条处理即可 */
		struct arc *a;

		while ((a = oldState->outs) != NULL)
		{
			cparc(nfa, a, newState, a->to);
			freearc(nfa, a);
		}
	}
	else
	{
		/*
		 * 弧很多时用排序归并。注意 changearcsource()
		 * 会把该弧放到 newState 链首，因而不会
		 * 打断我们对链上已排序部分的遍历。
		 */
		struct arc *oa;
		struct arc *na;

		/*
		 * 本路径绕过 newarc()，最好纳入
		 * 取消检查
		 */
		INTERRUPT(nfa->v->re);

		sortouts(nfa, oldState);
		sortouts(nfa, newState);
		if (NISERR())
			return;				/* 排序可能已失败 */
		oa = oldState->outs;
		na = newState->outs;
		while (oa != NULL && na != NULL)
		{
			struct arc *a = oa;

			switch (sortouts_cmp(&oa, &na))
			{
				case -1:
					/* newState 无与 oa 匹配者 */
					oa = oa->outchain;

					/*
					 * 与其 createarc+freearc，不如直接拆链
					 * 再挂回既有弧结构。
					 */
					changearcsource(a, newState);
					break;
				case 0:
					/* 匹配，两表都前进 */
					oa = oa->outchain;
					na = na->outchain;
					/* 并丢掉 oldState 的重复弧 */
					freearc(nfa, a);
					break;
				case +1:
					/* 只推进 na；oa 稍后可能匹配 */
					na = na->outchain;
					break;
				default:
					assert(NOTREACHED);
			}
		}
		while (oa != NULL)
		{
			/* newState 无与 oa 匹配者 */
			struct arc *a = oa;

			oa = oa->outchain;
			changearcsource(a, newState);
		}
	}

	assert(oldState->nouts == 0);
	assert(oldState->outs == NULL);
}

/*
 * copyouts - 把一状态出弧复制到另一状态
 *
 * 见 copyins() 的注释
 */
static void
copyouts(struct nfa *nfa,
		 struct state *oldState,
		 struct state *newState)
{
	assert(oldState != newState);
	assert(newState->nouts == 0);	/* 见上方注释 */

	if (newState->nouts == 0)
	{
		/* 无需去重 */
		struct arc *a;

		for (a = oldState->outs; a != NULL; a = a->outchain)
			createarc(nfa, a->type, a->co, newState, a->to);
	}
#ifdef NOT_USED					/* 见上方注释 */
	else if (!BULK_ARC_OP_USE_SORT(oldState->nouts, newState->nouts))
	{
		/* 弧不太多时，逐条处理即可 */
		struct arc *a;

		for (a = oldState->outs; a != NULL; a = a->outchain)
			cparc(nfa, a, newState, a->to);
	}
	else
	{
		/*
		 * 弧很多时用排序-归并。注意 createarc()
		 * 会把新弧接到 newState 链前端，故不
		 * 打断我们对链上已排序部分的遍历。
		 */
		struct arc *oa;
		struct arc *na;

		/*
		 * 本路径绕过 newarc()，最好纳入
		 * 取消检查
		 */
		INTERRUPT(nfa->v->re);

		sortouts(nfa, oldState);
		sortouts(nfa, newState);
		if (NISERR())
			return;				/* 排序可能已失败 */
		oa = oldState->outs;
		na = newState->outs;
		while (oa != NULL && na != NULL)
		{
			struct arc *a = oa;

			switch (sortouts_cmp(&oa, &na))
			{
				case -1:
					/* newState 无与 oa 匹配者 */
					oa = oa->outchain;
					createarc(nfa, a->type, a->co, newState, a->to);
					break;
				case 0:
					/* 匹配，两表都前进 */
					oa = oa->outchain;
					na = na->outchain;
					break;
				case +1:
					/* 只推进 na；oa 稍后可能匹配 */
					na = na->outchain;
					break;
				default:
					assert(NOTREACHED);
			}
		}
		while (oa != NULL)
		{
			/* newState 无与 oa 匹配者 */
			struct arc *a = oa;

			oa = oa->outchain;
			createarc(nfa, a->type, a->co, newState, a->to);
		}
	}
#endif							/* NOT_USED */
}

/*
 * cloneouts - 把一状态出弧复制到另一状态对并改类型
 *
 * 仅用于把 PLAIN 弧转为 AHEAD/BEHIND 弧，二者
 * 对 "co" 释义相同；用于 LACON 则不合理。
 */
static void
cloneouts(struct nfa *nfa,
		  struct state *old,
		  struct state *from,
		  struct state *to,
		  int type)
{
	struct arc *a;

	assert(old != from);
	assert(type == AHEAD || type == BEHIND);

	for (a = old->outs; a != NULL; a = a->outchain)
	{
		assert(a->type == PLAIN);
		newarc(nfa, type, a->co, from, to);
	}
}

/*
 * delsub - 删除子 NFA，必要时更新 subre 指针
 *
 * 对子 NFA 做递归遍历，用其 tmp 指针标记
 * 已见过的状态。
 */
static void
delsub(struct nfa *nfa,
	   struct state *lp,		/* 子 NFA 由此开始... */
	   struct state *rp)		/* 到此，*不含*终点 */
{
	assert(lp != rp);

	rp->tmp = rp;				/* 标末 */

	deltraverse(nfa, lp, lp);
	if (NISERR())
		return;					/* 失败后断言未必成立 */
	assert(lp->nouts == 0 && rp->nins == 0);	/* 已办妥 */
	assert(lp->no != FREESTATE && rp->no != FREESTATE); /* 已无 */

	rp->tmp = NULL;				/* 消末标 */
	lp->tmp = NULL;				/* 及起点，deltraverse 所标 */
}

/*
 * deltraverse - delsub 的递归核心
 * 本例程基本职责是销毁该状态的全部出弧。
 */
static void
deltraverse(struct nfa *nfa,
			struct state *leftend,
			struct state *s)
{
	struct arc *a;
	struct state *to;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->nouts == 0)
		return;					/* 无事可做 */
	if (s->tmp != NULL)
		return;					/* 已在进行中 */

	s->tmp = s;					/* 标为进行中 */

	while ((a = s->outs) != NULL)
	{
		to = a->to;
		deltraverse(nfa, leftend, to);
		if (NISERR())
			return;				/* 失败后断言未必成立 */
		assert(to->nouts == 0 || to->tmp != NULL);
		freearc(nfa, a);
		if (to->nins == 0 && to->tmp == NULL)
		{
			assert(to->nouts == 0);
			freestate(nfa, to);
		}
	}

	assert(s->no != FREESTATE); /* 我们仍在此 */
	assert(s == leftend || s->nins != 0);	/* 且仍可达 */
	assert(s->nouts == 0);		/* 但已无出弧 */

	s->tmp = NULL;				/* 此处已完成 */
}

/*
 * dupnfa - 复制子 NFA
 *
 * 又一次递归遍历；这次用 tmp 指向副本
 * 并标记已见状态。（你早知它为何是
 * 状态指针了吧？:-))
 */
static void
dupnfa(struct nfa *nfa,
	   struct state *start,		/* 在此起始的子 NFA 之副本 */
	   struct state *stop,		/* 并在此结束 */
	   struct state *from,		/* 把副本串接自此 */
	   struct state *to)		/* 至此 */
{
	if (start == stop)
	{
		newarc(nfa, EMPTY, 0, from, to);
		return;
	}

	stop->tmp = to;
	duptraverse(nfa, start, from);
	/* 完成，只差清掉 tmp 指针 */

	stop->tmp = NULL;
	cleartraverse(nfa, start);
}

/*
 * duptraverse - dupnfa 的递归核心
 */
static void
duptraverse(struct nfa *nfa,
			struct state *s,
			struct state *stmp) /* s 的副本，或 NULL */
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != NULL)
		return;					/* 已完成 */

	s->tmp = (stmp == NULL) ? newstate(nfa) : stmp;
	if (s->tmp == NULL)
	{
		assert(NISERR());
		return;
	}

	for (a = s->outs; a != NULL && !NISERR(); a = a->outchain)
	{
		duptraverse(nfa, a->to, (struct state *) NULL);
		if (NISERR())
			break;
		assert(a->to->tmp != NULL);
		cparc(nfa, a, s->tmp, a->to->tmp);
	}
}

/*
 * removeconstraints - 去掉 NFA 中任何约束
 *
 * 约束弧换成空弧，实质视全部约束
 * 为已自动满足。
 */
static void
removeconstraints(struct nfa *nfa,
				  struct state *start,	/* 处理自此起的子 NFA */
				  struct state *stop)	/* 并在此结束 */
{
	if (start == stop)
		return;

	stop->tmp = stop;
	removetraverse(nfa, start);
	/* 完成，只差清掉 tmp 指针 */

	stop->tmp = NULL;
	cleartraverse(nfa, start);
}

/*
 * removetraverse - removeconstraints 的递归核心
 */
static void
removetraverse(struct nfa *nfa,
			   struct state *s)
{
	struct arc *a;
	struct arc *oa;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != NULL)
		return;					/* 已完成 */

	s->tmp = s;
	for (a = s->outs; a != NULL && !NISERR(); a = oa)
	{
		removetraverse(nfa, a->to);
		if (NISERR())
			break;
		oa = a->outchain;
		switch (a->type)
		{
			case PLAIN:
			case EMPTY:
			case CANTMATCH:
				/* 无事可做 */
				break;
			case AHEAD:
			case BEHIND:
			case '^':
			case '$':
			case LACON:
				/* 替换之 */
				newarc(nfa, EMPTY, 0, s, a->to);
				freearc(nfa, a);
				break;
			default:
				NERR(REG_ASSERT);
				break;
		}
	}
}

/*
 * cleartraverse - 为留下 tmp 指针的算法做递归清理
 */
static void
cleartraverse(struct nfa *nfa,
			  struct state *s)
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp == NULL)
		return;
	s->tmp = NULL;

	for (a = s->outs; a != NULL; a = a->outchain)
		cleartraverse(nfa, a->to);
}

/*
 * single_color_transition - 从 s1 到 s2 是否只跨一条 PLAIN 弧？
 *
 * 若从 s1 到 s2 只需一次 PLAIN 匹配（可为某
 * 组色中任一），则返回出弧表仅含这些
 * 色 PLAIN 弧之状态；否则返回 NULL。
 *
 * 在优化 NFA 前调用，故可能有 EMPTY 弧须
 * 忽略；正因可能有 EMPTY，结果状态才可能
 * 不同于 s1。
 *
 * 值得处理多条并行 PLAIN 弧，因为
 * 如 [abc] 这类括号构造可能产生一条或多条
 * 并行 PLAIN 弧，取决于表达式中更早的原子。我们宁可
 * 不让该实现细节造成用户可见的性能差异。
 */
static struct state *
single_color_transition(struct state *s1, struct state *s2)
{
	struct arc *a;

	/* 忽略前导 EMPTY 弧(若有) */
	if (s1->nouts == 1 && s1->outs->type == EMPTY)
		s1 = s1->outs->to;
	/* 对尾随 EMPTY 弧同样处理 */
	if (s2->nins == 1 && s2->ins->type == EMPTY)
		s2 = s2->ins->from;
	/* 中间或有单状态环，若有则拒绝 */
	if (s1 == s2)
		return NULL;
	/* s1 至少须有一条出弧... */
	if (s1->outs == NULL)
		return NULL;
	/* 且须全是指向 s2 的 PLAIN 弧 */
	for (a = s1->outs; a != NULL; a = a->outchain)
	{
		if (a->type != PLAIN || a->to != s2)
			return NULL;
	}
	/* 可返回 s1 作为相关出弧的持有者 */
	return s1;
}

/*
 * specialcolors - 为 NFA 填入特殊色
 */
static void
specialcolors(struct nfa *nfa)
{
	/* BOS/BOL/EOS/EOL 的假色 */
	if (nfa->parent == NULL)
	{
		nfa->bos[0] = pseudocolor(nfa->cm);
		nfa->bos[1] = pseudocolor(nfa->cm);
		nfa->eos[0] = pseudocolor(nfa->cm);
		nfa->eos[1] = pseudocolor(nfa->cm);
	}
	else
	{
		assert(nfa->parent->bos[0] != COLORLESS);
		nfa->bos[0] = nfa->parent->bos[0];
		assert(nfa->parent->bos[1] != COLORLESS);
		nfa->bos[1] = nfa->parent->bos[1];
		assert(nfa->parent->eos[0] != COLORLESS);
		nfa->eos[0] = nfa->parent->eos[0];
		assert(nfa->parent->eos[1] != COLORLESS);
		nfa->eos[1] = nfa->parent->eos[1];
	}
}

/*
 * optimize - 优化 NFA
 *
 * 本函数主旨与其说是"优化"（虽也
 * 会去掉无用 NFA 状态）不如说是把 NFA 化简为
 * 正则执行器能处理的形态。执行器及作为其
 * 输入的 cNFA 格式，都只能处理 PLAIN 与 LACON 弧。
 * 正则解析器输出还有 EMPTY（无操作）弧，以及
 * ^、$、AHEAD 与 BEHIND 约束弧，此处必须除掉。
 * 我们先去掉 EMPTY 弧，再处理约束弧。
 * 两件事最难的都是除掉目标弧类型的环形回路。
 * 不过无论如何都得做：否则这样的环会让
 * 执行器在环中无限打转，却在输入串上
 * 毫无进展。
 */
static long						/* re_info 位 */
optimize(struct nfa *nfa,
		 FILE *f)				/* 调试输出；NULL 即无 */
{
#ifdef REG_DEBUG
	int			verbose = (f != NULL) ? 1 : 0;

	if (verbose)
		fprintf(f, "\ninitial cleanup:\n");
#endif
	/* 若有 CANTMATCH 弧则丢弃；但较少见 */
	if (nfa->flags & HASCANTMATCH)
	{
		removecantmatch(nfa);
		nfa->flags &= ~HASCANTMATCH;
	}
	cleanup(nfa);				/* 或可简化局面 */
#ifdef REG_DEBUG
	if (verbose)
		dumpnfa(nfa, f);
	if (verbose)
		fprintf(f, "\nempties:\n");
#endif
	fixempties(nfa, f);			/* 去掉 EMPTY 弧 */
#ifdef REG_DEBUG
	if (verbose)
		fprintf(f, "\nconstraints:\n");
#endif
	fixconstraintloops(nfa, f); /* 去掉约束环 */
	pullback(nfa, f);			/* 把约束向后拉 */
	pushfwd(nfa, f);			/* 把约束向前推 */
#ifdef REG_DEBUG
	if (verbose)
		fprintf(f, "\nfinal cleanup:\n");
#endif
	cleanup(nfa);				/* 最终整理 */
#ifdef REG_DEBUG
	if (verbose)
		dumpnfa(nfa, f);
#endif
	return analyze(nfa);		/* 并分析 */
}

/*
 * pullback - 向后拉约束以消除之
 */
static void
pullback(struct nfa *nfa,
		 FILE *f)				/* 调试输出；NULL 即无 */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	struct state *intermediates;
	int			progress;

	/* 不断查找并拉直至没有 */
	do
	{
		progress = 0;
		for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
		{
			nexts = s->next;
			intermediates = NULL;
			for (a = s->outs; a != NULL && !NISERR(); a = nexta)
			{
				nexta = a->outchain;
				if (a->type == '^' || a->type == BEHIND)
					if (pull(nfa, a, &intermediates))
						progress = 1;
			}
			/* 清掉此处所建中间状态的 tmp 字段 */
			while (intermediates != NULL)
			{
				struct state *ns = intermediates->tmp;

				intermediates->tmp = NULL;
				intermediates = ns;
			}
			/* 若 s 现已无用则除掉 */
			if ((s->nins == 0 || s->nouts == 0) && !s->flag)
				dropstate(nfa, s);
		}
		if (progress && f != NULL)
			dumpnfa(nfa, f);
	} while (progress && !NISERR());
	if (NISERR())
		return;

	/*
	 * 凡能拉到起始状态的 ^ 约束，现在都可
	 * 换成引用 BOS 或 BOL 色的 PLAIN 弧。不应再
	 * 有其他 ^ 或 BEHIND 弧留在 NFA 中，但此处
	 * 不查（有则 compact() 失败）。
	 */
	for (a = nfa->pre->outs; a != NULL; a = nexta)
	{
		nexta = a->outchain;
		if (a->type == '^')
		{
			assert(a->co == 0 || a->co == 1);
			newarc(nfa, PLAIN, nfa->bos[a->co], a->from, a->to);
			freearc(nfa, a);
		}
	}
}

/*
 * pull - 把后向约束向后拉过其源状态
 *
 * 成功则返回 1（除非源是起始状态或有
 * 内部错误），若无事发生则返回 0。
 *
 * 本函数的重要性质是：不删除任何既有
 * 状态，也不删除该约束 from 状态上除给定
 * 约束弧以外的出弧。这使 pullback() 中的循环
 * 安全，代价是可能留下无用状态。故留给
 * pullback() 去删。
 *
 * 若 from 有多条后向约束出弧，且/或多条
 * 兼容约束入弧，则对每个前驱与后继状态
 * 组合只需建一个新中间状态。*intermediates
 * 指向该 from 的此类中间状态列表（经其
 * tmp 字段相链）。
 */
static int
pull(struct nfa *nfa,
	 struct arc *con,
	 struct state **intermediates)
{
	struct state *from = con->from;
	struct state *to = con->to;
	struct arc *a;
	struct arc *nexta;
	struct state *s;

	assert(from != to);			/* 本应更早除掉 */
	if (from->flag)				/* 不能拉过起点 */
		return 0;
	if (from->nins == 0)
	{							/* 不可达 */
		freearc(nfa, con);
		return 1;
	}

	/*
	 * 首先，必要时克隆 from 以免有其他出弧。这或
	 * 看似浪费，但可简化逻辑，且我们会在
	 * 底端再去掉克隆状态。
	 */
	if (from->nouts > 1)
	{
		s = newstate(nfa);
		if (NISERR())
			return 0;
		copyins(nfa, from, s);	/* 复制入弧 */
		cparc(nfa, con, s, to); /* 移动约束弧 */
		freearc(nfa, con);
		if (NISERR())
			return 0;
		from = s;
		con = from->outs;
	}
	assert(from->nouts == 1);

	/* 把约束传播进 from 状态的入弧 */
	for (a = from->ins; a != NULL && !NISERR(); a = nexta)
	{
		nexta = a->inchain;
		switch (combine(nfa, con, a))
		{
			case INCOMPATIBLE:	/* 销毁该弧 */
				freearc(nfa, a);
				break;
			case SATISFIED:		/* 无需动作 */
				break;
			case COMPATIBLE:	/* 大体交换这两条弧 */
				/* 需要中间状态，但或许已有 */
				for (s = *intermediates; s != NULL; s = s->tmp)
				{
					assert(s->nins > 0 && s->nouts > 0);
					if (s->ins->from == a->from && s->outs->to == to)
						break;
				}
				if (s == NULL)
				{
					s = newstate(nfa);
					if (NISERR())
						return 0;
					s->tmp = *intermediates;
					*intermediates = s;
				}
				cparc(nfa, con, a->from, s);
				cparc(nfa, a, s, to);
				freearc(nfa, a);
				break;
			case REPLACEARC:	/* 替换弧的色 */
				newarc(nfa, a->type, con->co, a->from, to);
				freearc(nfa, a);
				break;
			default:
				assert(NOTREACHED);
				break;
		}
	}

	/* 若有剩余入弧，纳入该约束 */
	moveins(nfa, from, to);
	freearc(nfa, con);
	/* from 状态现已无用，留待 pullback() 清理 */
	return 1;
}

/*
 * pushfwd - 把前向约束向前推以消除之
 */
static void
pushfwd(struct nfa *nfa,
		FILE *f)				/* 调试输出；NULL 即无 */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	struct state *intermediates;
	int			progress;

	/* 查找并前推直至没有更多 */
	do
	{
		progress = 0;
		for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
		{
			nexts = s->next;
			intermediates = NULL;
			for (a = s->ins; a != NULL && !NISERR(); a = nexta)
			{
				nexta = a->inchain;
				if (a->type == '$' || a->type == AHEAD)
					if (push(nfa, a, &intermediates))
						progress = 1;
			}
			/* 清掉此处所建中间状态的 tmp 字段 */
			while (intermediates != NULL)
			{
				struct state *ns = intermediates->tmp;

				intermediates->tmp = NULL;
				intermediates = ns;
			}
			/* 若 s 现已无用则除掉 */
			if ((s->nins == 0 || s->nouts == 0) && !s->flag)
				dropstate(nfa, s);
		}
		if (progress && f != NULL)
			dumpnfa(nfa, f);
	} while (progress && !NISERR());
	if (NISERR())
		return;

	/*
	 * 凡能推到 post 状态的 $ 约束，现可换成引用
	 * EOS 或 EOL 色的 PLAIN 弧。NFA 中应不再有其
	 * 它 $ 或 AHEAD 弧，不过此处不检查
	 * （若有则 compact() 会失败）。
	 */
	for (a = nfa->post->ins; a != NULL; a = nexta)
	{
		nexta = a->inchain;
		if (a->type == '$')
		{
			assert(a->co == 0 || a->co == 1);
			newarc(nfa, PLAIN, nfa->eos[a->co], a->from, a->to);
			freearc(nfa, a);
		}
	}
}

/*
 * push - 把前向约束推过其目的状态
 *
 * 成功返回 1（几乎总是；除非目的是
 * post 状态或有内部错误），无事发生则返回 0。
 *
 * 本函数一重要性质是：不删除任何既有
 * 状态，也不删除约束 to 状态上除给定
 * 约束弧外的入弧。这使 pushfwd() 循环安全，代价是
 * 可能留下无用状态。因此留给 pushfwd()
 * 删除此类状态。
 *
 * 若 to 状态有多条前向约束入弧，和/或多条
 * 相容约束出弧，则对每对前驱与后继状态组合
 * 只需创建一个新中间状态。*intermediates
 * 指向该 to 状态的此类中间状态列表（经其
 * tmp 字段串联）。
 */
static int
push(struct nfa *nfa,
	 struct arc *con,
	 struct state **intermediates)
{
	struct state *from = con->from;
	struct state *to = con->to;
	struct arc *a;
	struct arc *nexta;
	struct state *s;

	assert(to != from);			/* 本应更早除掉 */
	if (to->flag)				/* 不能前推越过终点 */
		return 0;
	if (to->nouts == 0)
	{							/* 死路 */
		freearc(nfa, con);
		return 1;
	}

	/*
	 * 首先必要时克隆 to 状态以避免其它入弧。这或
	 * 看似浪费，但简化逻辑，且我们会在
	 * 底部再弃克隆状态。
	 */
	if (to->nins > 1)
	{
		s = newstate(nfa);
		if (NISERR())
			return 0;
		copyouts(nfa, to, s);	/* 复制出弧 */
		cparc(nfa, con, from, s);	/* 移动约束弧 */
		freearc(nfa, con);
		if (NISERR())
			return 0;
		to = s;
		con = to->ins;
	}
	assert(to->nins == 1);

	/* 把约束传播到 to 状态的出弧 */
	for (a = to->outs; a != NULL && !NISERR(); a = nexta)
	{
		nexta = a->outchain;
		switch (combine(nfa, con, a))
		{
			case INCOMPATIBLE:	/* 销毁该弧 */
				freearc(nfa, a);
				break;
			case SATISFIED:		/* 无需动作 */
				break;
			case COMPATIBLE:	/* 大体交换这两条弧 */
				/* 需要中间状态，但或许已有 */
				for (s = *intermediates; s != NULL; s = s->tmp)
				{
					assert(s->nins > 0 && s->nouts > 0);
					if (s->ins->from == from && s->outs->to == a->to)
						break;
				}
				if (s == NULL)
				{
					s = newstate(nfa);
					if (NISERR())
						return 0;
					s->tmp = *intermediates;
					*intermediates = s;
				}
				cparc(nfa, con, s, a->to);
				cparc(nfa, a, from, s);
				freearc(nfa, a);
				break;
			case REPLACEARC:	/* 替换弧的色 */
				newarc(nfa, a->type, con->co, from, a->to);
				freearc(nfa, a);
				break;
			default:
				assert(NOTREACHED);
				break;
		}
	}

	/* 若有剩余出弧，纳入该约束 */
	moveouts(nfa, to, from);
	freearc(nfa, con);
	/* to 状态现已无用，留待 pushfwd() 清理 */
	return 1;
}

/*
 * combine - 约束落到弧上会怎样？
 *
 * #def INCOMPATIBLE	1	// 销毁弧
 * #def SATISFIED		2	// 约束已满足
 * #def COMPATIBLE		3	// 相容但尚未满足
 * #def REPLACEARC		4	// 用约束色替换弧的色
 */
static int
combine(struct nfa *nfa,
		struct arc *con,
		struct arc *a)
{
#define  CA(ct,at)	 (((ct)<<CHAR_BIT) | (at))

	switch (CA(con->type, a->type))
	{
		case CA('^', PLAIN):	/* 换行符另行处理 */
		case CA('$', PLAIN):
			return INCOMPATIBLE;
			break;
		case CA(AHEAD, PLAIN):	/* 色约束与色相遇 */
		case CA(BEHIND, PLAIN):
			if (con->co == a->co)
				return SATISFIED;
			if (con->co == RAINBOW)
			{
				/* 除非弧色为伪色，否则 con 已满足 */
				if (!(nfa->cm->cd[a->co].flags & PSEUDO))
					return SATISFIED;
			}
			else if (a->co == RAINBOW)
			{
				/* 若 con 针对伪色则不相容 */
				/* （假设情形；现今不做此类约束） */
				if (nfa->cm->cd[con->co].flags & PSEUDO)
					return INCOMPATIBLE;
				/* 否则约束把弧限定为其色 */
				return REPLACEARC;
			}
			return INCOMPATIBLE;
			break;
		case CA('^', '^'):		/* 碰撞，同类约束 */
		case CA('$', '$'):
			if (con->co == a->co)	/* 真重复 */
				return SATISFIED;
			return INCOMPATIBLE;
			break;
		case CA(AHEAD, AHEAD):	/* 碰撞，同类约束 */
		case CA(BEHIND, BEHIND):
			if (con->co == a->co)	/* 真重复 */
				return SATISFIED;
			if (con->co == RAINBOW)
			{
				/* 除非弧色为伪色，否则 con 已满足 */
				if (!(nfa->cm->cd[a->co].flags & PSEUDO))
					return SATISFIED;
			}
			else if (a->co == RAINBOW)
			{
				/* 若 con 针对伪色则不相容 */
				/* （假设情形；现今不做此类约束） */
				if (nfa->cm->cd[con->co].flags & PSEUDO)
					return INCOMPATIBLE;
				/* 否则约束把弧限定为其色 */
				return REPLACEARC;
			}
			return INCOMPATIBLE;
			break;
		case CA('^', BEHIND):	/* 碰撞，异类约束 */
		case CA(BEHIND, '^'):
		case CA('$', AHEAD):
		case CA(AHEAD, '$'):
			return INCOMPATIBLE;
			break;
		case CA('^', '$'):		/* 约束彼此交错穿过 */
		case CA('^', AHEAD):
		case CA(BEHIND, '$'):
		case CA(BEHIND, AHEAD):
		case CA('$', '^'):
		case CA('$', BEHIND):
		case CA(AHEAD, '^'):
		case CA(AHEAD, BEHIND):
		case CA('^', LACON):
		case CA(BEHIND, LACON):
		case CA('$', LACON):
		case CA(AHEAD, LACON):
			return COMPATIBLE;
			break;
	}
	assert(NOTREACHED);
	return INCOMPATIBLE;		/* 为盲目编译器着想 */
}

/*
 * fixempties - 去掉 EMPTY 弧
 */
static void
fixempties(struct nfa *nfa,
		   FILE *f)				/* 调试输出；NULL 即无 */
{
	struct state *s;
	struct state *s2;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			totalinarcs;
	struct arc **inarcsorig;
	struct arc **arcarray;
	int			arccount;
	int			prevnins;
	int			nskip;

	/*
	 * 先去掉唯一出弧为 EMPTY 的状态，因其
	 * 基本只是后继的别名。解析
	 * 算法会产生很多，值得特判。
	 */
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		if (s->flag || s->nouts != 1)
			continue;
		a = s->outs;
		assert(a != NULL && a->outchain == NULL);
		if (a->type != EMPTY)
			continue;
		if (s != a->to)
			moveins(nfa, s, a->to);
		dropstate(nfa, s);
	}

	/*
	 * 同理，去掉仅有一条 EMPTY 入弧的状态，并入
	 * 其前驱。
	 */
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		/* 顺便确保下一步用的 tmp 字段已清空 */
		assert(s->tmp == NULL);
		if (s->flag || s->nins != 1)
			continue;
		a = s->ins;
		assert(a != NULL && a->inchain == NULL);
		if (a->type != EMPTY)
			continue;
		if (s != a->from)
			moveouts(nfa, s, a->from);
		dropstate(nfa, s);
	}

	if (NISERR())
		return;

	/*
	 * 对每个剩余 NFA 状态，找出所有能经一条或
	 * 多条 EMPTY 弧链到达它的其它状态。再生成新弧
	 * 以消除对每条此类链的依赖。
	 *
	 * 替换从 "from" 到 "to" 的 EMPTY 弧链时，既可
	 * 把非 EMPTY 弧前推（直接把 "from" 的前驱
	 * 连到 "to"），也可后拉（直接把 "from" 连到
	 * "to" 的后继）。我们总选前者；此选择
	 * 有些任意，但下方做法要求我们一律
	 * 只做其中一种。
	 *
	 * 设有 N 条连续 EMPTY 弧的链（N 很容易接近
	 * NFA 规模）。所有中间状态必须另有
	 * 入弧与出弧，否则早已被上面步骤去掉。
	 * 假定其入弧大多非 EMPTY，我们将向 NFA 加入
	 * O(N^2) 条弧，因为通向链中任一状态的非 EMPTY
	 * 入弧都必须复制以通向其全部后继
	 * 状态。因此工作量不可能低于 O(N^2)；
	 * 但仍应尽量不让 big-O 代价变得更糟，
	 * 稍不小心就很容易发生。具
	 * 体地，若把 S1 的全部入弧前拷到 S2，再拷到
	 * S3，之后又考虑把 S2 的入弧前推到
	 * S3，若把已从 S1 拷来的弧也算进去，就会做
	 * O(N^3) 的工作。（newarc() 及其同伴内建的
	 * 去重会丢掉多余弧，但并非没有代价。）
	 *
	 * 为避免此代价，只把本阶段开始时已存在的弧
	 * 当作可前推候选。为识别它们，我们记下
	 * 各状态起初的第一条入弧。我们依赖
	 * newarc() 等把新弧插到其 to 状态入链
	 * 前端，且本阶段从不删弧，因此
	 * 原弧必在其 to 状态入链的末尾。
	 *
	 * 于是此处流程是：对 NFA 中每个状态，收集所有
	 * 能经 EMPTY 弧到达目标的状态的非 EMPTY
	 * 入弧。再排序、去重，并合并进目标状态的
	 * 入链。（只要在加弧后更新各状态的原弧
	 * 指针，就可以安全用归并；mergeins 的排序
	 * 步很可能已打乱了旧弧。）
	 *
	 *
	 * 另一值得做的细化是：因本阶段只加非 EMPTY
	 * 弧，且所加弧的 from 与被克隆的非 EMPTY 弧
	 * 相同，故可预先知道：仅有 EMPTY 出弧的
	 * 状态在丢掉 EMPTY 弧后会因无出弧而无用。
	 * （若起初没有非 EMPTY 出弧，就不可能再获得。）
	 * 因此完全不必更新此类状态的
	 * 入链。
	 */

	/* 记住各状态最初的第一条入弧 */
	/* ……并顺便统计旧入弧总数 */
	inarcsorig = (struct arc **) MALLOC(nfa->nstates * sizeof(struct arc *));
	if (inarcsorig == NULL)
	{
		NERR(REG_ESPACE);
		return;
	}
	totalinarcs = 0;
	for (s = nfa->states; s != NULL; s = s->next)
	{
		inarcsorig[s->no] = s->ins;
		totalinarcs += s->nins;
	}

	/*
	 * 创建工作区以累积将加入当前目标状态的
	 * 入弧。totalinarcs 多半高估了所需
	 * 空间，但此时 NFA 不太可能大到值得
	 * 做得更精明。
	 */
	arcarray = (struct arc **) MALLOC(totalinarcs * sizeof(struct arc *));
	if (arcarray == NULL)
	{
		NERR(REG_ESPACE);
		FREE(inarcsorig);
		return;
	}

	/* 再遍历各目标状态 */
	for (s = nfa->states; s != NULL && !NISERR(); s = s->next)
	{
		/* 按上文，忽略无非 EMPTY 出弧的目标状态 */
		if (!s->flag && !hasnonemptyout(s))
			continue;

		/* 找前驱状态并累积其原始入弧 */
		arccount = 0;
		for (s2 = emptyreachable(nfa, s, s, inarcsorig); s2 != s; s2 = nexts)
		{
			/* 把 s2 的原始入弧加入 arcarray[]，忽略空弧 */
			for (a = inarcsorig[s2->no]; a != NULL; a = a->inchain)
			{
				if (a->type != EMPTY)
					arcarray[arccount++] = a;
			}

			/* 回走时复位 tmp 字段 */
			nexts = s2->tmp;
			s2->tmp = NULL;
		}
		s->tmp = NULL;
		assert(arccount <= totalinarcs);

		/* 记住该状态有多少条原始入弧 */
		prevnins = s->nins;

		/* 把非重复入弧加入目标状态 */
		mergeins(nfa, s, arcarray, arccount);

		/* 现在必须更新该状态的 inarcsorig 指针 */
		nskip = s->nins - prevnins;
		a = s->ins;
		while (nskip-- > 0)
			a = a->inchain;
		inarcsorig[s->no] = a;
	}

	FREE(arcarray);
	FREE(inarcsorig);

	if (NISERR())
		return;

	/*
	 * 现已不需要，去掉全部 EMPTY 弧。
	 */
	for (s = nfa->states; s != NULL; s = s->next)
	{
		for (a = s->outs; a != NULL; a = nexta)
		{
			nexta = a->outchain;
			if (a->type == EMPTY)
				freearc(nfa, a);
		}
	}

	/*
	 * 再去掉已变得无用的状态。（此清理并不
	 * 彻底；若与上一步合并会更不彻底；但
	 * cleanup() 会处理我们漏掉的一切。）
	 */
	for (s = nfa->states; s != NULL; s = nexts)
	{
		nexts = s->next;
		if ((s->nins == 0 || s->nouts == 0) && !s->flag)
			dropstate(nfa, s);
	}

	if (f != NULL)
		dumpnfa(nfa, f);
}

/*
 * emptyreachable - 递归找出能经 EMPTY 弧到达 s 的全部状态
 *
 * 返回值是找到的最后一个此类状态。其 tmp 回链到
 * 倒数第二个，如此直到 s，从而无需搜索整个
 * NFA 即可定位这些状态。
 *
 * 因仅用于 fixempties()，传入该函数维护的
 * inarcsorig[] 数组。这让我们跳过全部新入弧，它们
 * 肯定不是 EMPTY 弧。
 *
 * 此处最大递归深度等于最长无环 EMPTY 弧链
 * 的长度，肯定不超过 NFA 规模……但那仍可能
 * 足以惹麻烦。
 */
static struct state *
emptyreachable(struct nfa *nfa,
			   struct state *s,
			   struct state *lastfound,
			   struct arc **inarcsorig)
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return lastfound;
	}

	s->tmp = lastfound;
	lastfound = s;
	for (a = inarcsorig[s->no]; a != NULL; a = a->inchain)
	{
		if (a->type == EMPTY && a->from->tmp == NULL)
			lastfound = emptyreachable(nfa, a->from, lastfound, inarcsorig);
	}
	return lastfound;
}

/*
 * isconstraintarc - 检测弧是否为约束类型
 */
static inline int
isconstraintarc(struct arc *a)
{
	switch (a->type)
	{
		case '^':
		case '$':
		case BEHIND:
		case AHEAD:
		case LACON:
			return 1;
	}
	return 0;
}

/*
 * hasconstraintout - 状态是否有约束出弧？
 */
static int
hasconstraintout(struct state *s)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (isconstraintarc(a))
			return 1;
	}
	return 0;
}

/*
 * fixconstraintloops - 去掉只含约束弧的环
 *
 * 只含约束弧的状态环毫无用处，因为绕环
 * 并不代表前进。而且它会让
 * pullback/pushfwd 无限循环，故须在那之前
 * 去掉此类环。
 */
static void
fixconstraintloops(struct nfa *nfa,
				   FILE *f)		/* 调试输出；NULL 即无 */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			hasconstraints;

	/*
	 * 对状态自环这种平凡情形，可直接丢掉
	 * 约束弧。值得特判，因为此类环远比
	 * 含多状态的环常见。顺便记下是否还有
	 * 约束弧幸存。
	 */
	hasconstraints = 0;
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		/* 顺便确保下一步用的 tmp 字段已清空 */
		assert(s->tmp == NULL);
		for (a = s->outs; a != NULL && !NISERR(); a = nexta)
		{
			nexta = a->outchain;
			if (isconstraintarc(a))
			{
				if (a->to == s)
					freearc(nfa, a);
				else
					hasconstraints = 1;
			}
		}
		/* 若已去掉全部出弧，状态便无用。 */
		if (s->nouts == 0 && !s->flag)
			dropstate(nfa, s);
	}

	/* 若无剩余约束弧则无事可做 */
	if (NISERR() || !hasconstraints)
		return;

	/*
	 * 从每个剩余 NFA 状态出发，向外搜索约束
	 * 环。若找到则打破它，再重新开始搜索。
	 * （或许能保留首次扫描的部分状态，但会大大
	 * 复杂化，且多状态约束环很少见，不值得
	 * 为此优化。）
	 */
restart:
	for (s = nfa->states; s != NULL && !NISERR(); s = s->next)
	{
		if (findconstraintloop(nfa, s))
			goto restart;
	}

	if (NISERR())
		return;

	/*
	 * 再去掉已变得无用的状态。（此清理并不
	 * 彻底；若与上一步合并会更不彻底；但
	 * cleanup() 会处理我们漏掉的一切。）
	 *
	 * 因 findconstraintloop 有意不复位全部 tmp，
	 * 做完后必须清零。这里也方便
	 * 一并做。
	 */
	for (s = nfa->states; s != NULL; s = nexts)
	{
		nexts = s->next;
		s->tmp = NULL;
		if ((s->nins == 0 || s->nouts == 0) && !s->flag)
			dropstate(nfa, s);
	}

	if (f != NULL)
		dumpnfa(nfa, f);
}

/*
 * findconstraintloop - 递归查找约束弧环
 *
 * 若找到环，调用 breakconstraintloop() 打破，然后
 * 返回 1；否则返回 0。
 *
 * 成功返回时状态 tmp 保证全为 NULL，因为
 * breakconstraintloop 会清零。失败返回后，已知
 * 不在环中的状态标为 s->tmp == s；这样稍后
 * 调用不必再证明这一点。（此约定可行，
 * 因为单状态环已被消除。）
 *
 * 注意找到的环未必包含我们首次被调用的
 * 状态。从该状态可达的任何环都行。
 *
 * 此处最大递归深度比最长无环约束弧链长度
 * 多一，肯定不超过 NFA 规模……但那仍可能
 * 足以惹麻烦。
 */
static int
findconstraintloop(struct nfa *nfa, struct state *s)
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return 1;				/* 尽快退出 */
	}

	if (s->tmp != NULL)
	{
		/* 已证明无趣？ */
		if (s->tmp == s)
			return 0;
		/* 找到含 s 的环 */
		breakconstraintloop(nfa, s);
		/* tmp 字段已由 breakconstraintloop 清理 */
		return 1;
	}
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (isconstraintarc(a))
		{
			struct state *sto = a->to;

			assert(sto != s);
			s->tmp = sto;
			if (findconstraintloop(nfa, sto))
				return 1;
		}
	}

	/*
	 * 若到此，则不存在从 s 出发的约束环。标为
	 * s->tmp == s，以免稍后再次发现这一事实。
	 */
	s->tmp = s;
	return 0;
}

/*
 * breakconstraintloop - 打破约束弧环
 *
 * sinitial 是环中任一成员状态。各成员的 tmp
 * 字段链到环内后继。（注意本函数会把
 * 全部 tmp 字段复位为 NULL。）
 *
 * 打破环的办法：对环中任一状态 S1，克隆其环
 * 后继 S2（以及可能的后续状态），再把全部
 * S1->S2 约束弧改指向克隆的 S2。克隆的 S2 应
 * 复制 S2 的非约束出弧。指向回 S1 的约束出弧
 * 应丢掉，否则需复制为通向同样克隆的
 * S3、S4 等的弧。一般地，各克隆状态复制
 * 非约束出弧，丢掉通向自身或更早克隆状态的
 * 约束出弧，并把其它约束出弧送往新克隆
 * 状态。克隆状态的入弧只会是约束弧，且只来自
 * S1 或更早克隆的状态。丢掉约束回弧没
 * 问题，因其不会带到尚未到过的状态；故
 * 不会产生新约束环。这样我们得到仍能表示
 * 每一有用状态序列的改写 NFA，但不表示
 * 不消耗输入的状态环序列。注意克隆集合
 * 必定包含除 S1 外的全部环成员，也可能包含
 * 从 S2 经约束弧可达的非环状态。这很重要，
 * 因为不保证 findconstraintloop 找到的是
 * 极大环（搜索极大环是 NP 难的，别试）。
 * 这些"非环状态"常常其实属于我们没注意的
 * 更大环，且可能有多个重叠环。此技法在
 * 此类情形保证收敛，而只考虑原先找到的
 * 环则
 * 不能。
 *
 * 若只有一条 S1->S2 约束弧，则进入任一克隆
 * 状态时该约束必已满足。这意味着在许多约束
 * 弧标签相同的常见情形下，可用同样标签的
 * 约束把相链的克隆状态合并：若能到第一个就
 * 必能到第二个，无需区分。这大大减少所需
 * 新状态数，故我们优先在满足此条件的
 * 状态对处打破给定
 * 环。
 *
 * 此外，常会发现克隆后继状态没有任何出弧，
 * 尤其当我们较积极地去掉不必要出弧时。若
 * 如此，则经前驱的环弧根本到不了任何有趣
 * 状态，这意味着只需去掉那些环弧即可打破
 * 环，不必添加任何新状态。
 */
static void
breakconstraintloop(struct nfa *nfa, struct state *sinitial)
{
	struct state *s;
	struct state *shead;
	struct state *stail;
	struct state *sclone;
	struct state *nexts;
	struct arc *refarc;
	struct arc *a;
	struct arc *nexta;

	/*
	 * 先确定要在环的哪一步打破。
	 * 优先选只有一条约束弧的那步。（XXX 还有
	 * 其它想用的次要启发式吗？）若有单独约束
	 * 弧，令 refarc 指向它。
	 */
	refarc = NULL;
	s = sinitial;
	do
	{
		nexts = s->tmp;
		assert(nexts != s);		/* 不应见到单元素环 */
		if (refarc == NULL)
		{
			int			narcs = 0;

			for (a = s->outs; a != NULL; a = a->outchain)
			{
				if (a->to == nexts && isconstraintarc(a))
				{
					refarc = a;
					narcs++;
				}
			}
			assert(narcs > 0);
			if (narcs > 1)
				refarc = NULL;	/* 此处有多条约束弧，不行 */
		}
		s = nexts;
	} while (s != sinitial);

	if (refarc)
	{
		/* refarc 处打破 */
		shead = refarc->from;
		stail = refarc->to;
		assert(stail == shead->tmp);
	}
	else
	{
		/* 不妨在 sinitial 之后打破 */
		shead = sinitial;
		stail = sinitial->tmp;
	}

	/*
	 * 复位 tmp 字段，以便在 clonesuccessorstates
	 * 中作局部存储。（findconstraintloop 不介意，
	 * 因为它反正要放弃搜索。）
	 */
	for (s = nfa->states; s != NULL; s = s->next)
		s->tmp = NULL;

	/*
	 * 按需递归构建克隆状态。
	 */
	sclone = newstate(nfa);
	if (sclone == NULL)
	{
		assert(NISERR());
		return;
	}

	clonesuccessorstates(nfa, stail, sclone, shead, refarc,
						 NULL, NULL, nfa->nstates);

	if (NISERR())
		return;

	/*
	 * sclone 可能完全没有出弧，那样它就无用。
	 * （此处不极力清除无用状态，但本情形简单
	 * 且相当常见。）
	 */
	if (sclone->nouts == 0)
	{
		freestate(nfa, sclone);
		sclone = NULL;
	}

	/*
	 * 把 shead 的约束环弧改指向 sclone，若发现不需要
	 * sclone 则直接丢掉。
	 */
	for (a = shead->outs; a != NULL; a = nexta)
	{
		nexta = a->outchain;
		if (a->to == stail && isconstraintarc(a))
		{
			if (sclone)
				cparc(nfa, a, shead, sclone);
			freearc(nfa, a);
			if (NISERR())
				break;
		}
	}
}

/*
 * clonesuccessorstates - 创建约束弧后继状态树
 *
 * ssource 是待克隆状态，sclone 是承接其出弧的
 * 状态。sclone 的入弧若有则应已设好。
 *
 * spredecessor 是我们要为之构建后继的原前驱
 * 状态（未必是 ssource 的直接前驱）。
 * refarc 若非 NULL，是已知已从 spredecessor 走出
 * 以到达后继的原约束弧。
 *
 * 对每个克隆后继状态，我们临时创建 "donemap"，
 * 即布尔数组，标明本克隆已访问过哪些源状态。
 * 这防止无限递归以及无用的重复访问同一状态
 * 子树（累积很快，因典型 NFA 有多条冗余弧
 * 通路）。每个 donemap 是按状态号索引的
 * char 数组。所有 donemap 大小同为 "nstates"，
 * 即递归开始时的 nfa->nstates。这足以容纳
 * 全部既有状态的项，但*没有*递归中创建的
 * 克隆状态的项。没关系，因为我们无需
 * 标之。
 *
 * curdonemap 在递归到新 sclone 时为 NULL，或在
 * 未建新状态而递归时为 sclone 的 donemap（当
 * 我们决定把后继并入当前克隆状态时如此）。
 * outerdonemap 在顶层为 NULL，否则为父
 * 克隆的 donemap。
 *
 * 此处创建并填充的后继状态形成严格树结构，
 * 除顶层外每状态恰有一个前驱；顶层尚无
 * 入弧（breakconstraintloop 会在我们做完后从
 * spredecessor 加入）。因此可沿 sclone 入弧回
 * 到根，再加上 refarc（若有），识别当前点已
 * 知有效的约束集。这让我们避免生成多余
 * 后继状态。
 */
static void
clonesuccessorstates(struct nfa *nfa,
					 struct state *ssource,
					 struct state *sclone,
					 struct state *spredecessor,
					 struct arc *refarc,
					 char *curdonemap,
					 char *outerdonemap,
					 int nstates)
{
	char	   *donemap;
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	/* 若本状态尚无 donemap，则创建一个 */
	donemap = curdonemap;
	if (donemap == NULL)
	{
		donemap = (char *) MALLOC(nstates * sizeof(char));
		if (donemap == NULL)
		{
			NERR(REG_ESPACE);
			return;
		}

		if (outerdonemap != NULL)
		{
			/*
			 * 非最外层递归，故复制外层的
			 * donemap；这确保我们把外层正在访问、或已
			 * 并入前驱状态的状态，视为不应再
			 * 回访的。
			 */
			memcpy(donemap, outerdonemap, nstates * sizeof(char));
		}
		else
		{
			/* 最外层仅 spredecessor 为禁区 */
			memset(donemap, 0, nstates * sizeof(char));
			assert(spredecessor->no < nstates);
			donemap[spredecessor->no] = 1;
		}
	}

	/* 标记 ssource 已访问 */
	assert(ssource->no < nstates);
	assert(donemap[ssource->no] == 0);
	donemap[ssource->no] = 1;

	/*
	 * 我们先克隆 ssource 的全部出弧，按需创建新
	 * 克隆状态但暂不做更多。再在第二遍递归处理
	 * 子克隆状态。这让每个可达源状态只有一个
	 * 子克隆，即使有多条出弧通向同一状态。另
	 * 外，访问子状态时其入弧集已知，从而可
	 * 安全应用"约束已检查"优化。这也确保在
	 * 递归到任何子节点前，已把能合并的都并入
	 * 当前克隆，从而可能使它们免于再为那些状态
	 * 制作额外
	 * 映像。
	 *
	 * 本函数运行时，当前状态的子克隆用其 tmp
	 * 指向所克隆的原状态来标记。这样可检测通向
	 * 同一状态的多条出弧，也便于区分克隆状态与
	 * 原状态（原状态的
	 * tmp == NULL）。
	 */
	for (a = ssource->outs; a != NULL && !NISERR(); a = a->outchain)
	{
		struct state *sto = a->to;

		/*
		 * 我们不考虑克隆没有约束出弧的后继状态；
		 * 直接按原样链接。它们不可能属于约束
		 * 环，故无需复制。尤其是，此规则阻止我们
		 * 去克隆 post 状态，那
		 * 很糟糕。
		 */
		if (isconstraintarc(a) && hasconstraintout(sto))
		{
			struct state *prevclone;
			int			canmerge;
			struct arc *a2;

			/*
			 * 反向约束弧不得跟随。也无需再访
			 * 已并入本克隆的状态。
			 */
			assert(sto->no < nstates);
			if (donemap[sto->no] != 0)
				continue;

			/*
			 * 检查是否已有此源状态的子
			 * 克隆态。
			 */
			prevclone = NULL;
			for (a2 = sclone->outs; a2 != NULL; a2 = a2->outchain)
			{
				if (a2->to->tmp == sto)
				{
					prevclone = a2->to;
					break;
				}
			}

			/*
			 * 若此弧标签与 refarc 相同，或与到达 sclone
			 * 必经的任一弧相同，则到达 sto 无需额外
			 * 约束，故应直接把它的出弧
			 * 并入 sclone。
			 */
			if (refarc && a->type == refarc->type && a->co == refarc->co)
				canmerge = 1;
			else
			{
				struct state *s;

				canmerge = 0;
				for (s = sclone; s->ins; s = s->ins->from)
				{
					if (s->nins == 1 &&
						a->type == s->ins->type && a->co == s->ins->co)
					{
						canmerge = 1;
						break;
					}
				}
			}

			if (canmerge)
			{
				/*
				 * 可以并入 sclone。若先前建了子克隆状态，
				 * 丢掉它；无需再访。（若 ssource 有多条
				 * 通向 sto 的路径，而我们此刻才找到一条
				 * 可证为无操作的路径，就会发生。）
				 */
				if (prevclone)
					dropstate(nfa, prevclone);	/* 连出弧一并杀 */

				/* 递归把 sto 的出弧并入 sclone */
				clonesuccessorstates(nfa,
									 sto,
									 sclone,
									 spredecessor,
									 refarc,
									 donemap,
									 outerdonemap,
									 nstates);
				/* sto 现应已标为先前访问过 */
				assert(NISERR() || donemap[sto->no] == 1);
			}
			else if (prevclone)
			{
				/*
				 * 对此后继已有克隆状态，故只需
				 * 再向其连一条弧
				 */
				cparc(nfa, a, sclone, prevclone);
			}
			else
			{
				/*
				 * 需要创建新的后继克隆状态。
				 */
				struct state *stoclone;

				stoclone = newstate(nfa);
				if (stoclone == NULL)
				{
					assert(NISERR());
					break;
				}
				/* 标明它克隆自何状态 */
				stoclone->tmp = sto;
				/* 并加上通向它的出弧 */
				cparc(nfa, a, sclone, stoclone);
			}
		}
		else
		{
			/*
			 * 非约束出弧直接复制到 sclone，通向无
			 * 约束出弧之状态的出弧亦然。
			 */
			cparc(nfa, a, sclone, sto);
		}
	}

	/*
	 * 若处于此克隆状态的外层，则递归到其所有子
	 * 克隆状态，并沿途清掉其 tmp 字段。（若非 
	 * sclone 的最外层，则留给外层调用去做。）
	 * 注意：若有多条出弧通向同一克隆状态，
	 * 则只会递归进入一次。
	 */
	if (curdonemap == NULL)
	{
		for (a = sclone->outs; a != NULL && !NISERR(); a = a->outchain)
		{
			struct state *stoclone = a->to;
			struct state *sto = stoclone->tmp;

			if (sto != NULL)
			{
				stoclone->tmp = NULL;
				clonesuccessorstates(nfa,
									 sto,
									 stoclone,
									 spredecessor,
									 refarc,
									 NULL,
									 donemap,
									 nstates);
			}
		}

		/* 用完后别忘了释放 sclone 的 donemap */
		FREE(donemap);
	}
}

/*
 * removecantmatch - 移除 CANTMATCH 弧，这些弧在
 * 解析阶段结束后便不再有用。（需要它们只为
 * 在解析期间保持 NFA 子图的连通性。）
 */
static void
removecantmatch(struct nfa *nfa)
{
	struct state *s;

	for (s = nfa->states; s != NULL; s = s->next)
	{
		struct arc *a;
		struct arc *nexta;

		for (a = s->outs; a != NULL; a = nexta)
		{
			nexta = a->outchain;
			if (a->type == CANTMATCH)
			{
				freearc(nfa, a);
				if (NISERR())
					return;
			}
		}
	}
}

/*
 * cleanup - 优化后清理 NFA
 */
static void
cleanup(struct nfa *nfa)
{
	struct state *s;
	struct state *nexts;
	int			n;

	if (NISERR())
		return;

	/* 清除不可达或死端状态 */
	/* 用 pre 标可达，再用 post 标可到 post 者 */
	markreachable(nfa, nfa->pre, (struct state *) NULL, nfa->pre);
	markcanreach(nfa, nfa->post, nfa->pre, nfa->post);
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		if (s->tmp != nfa->post && !s->flag)
			dropstate(nfa, s);
	}
	assert(NISERR() || nfa->post->nins == 0 || nfa->post->tmp == nfa->post);
	cleartraverse(nfa, nfa->pre);
	assert(NISERR() || nfa->post->nins == 0 || nfa->post->tmp == NULL);
	/* nins==0（最终不可达）情形稍后会抓住 */

	/* 幸存状态重编号 */
	n = 0;
	for (s = nfa->states; s != NULL; s = s->next)
		s->no = n++;
	nfa->nstates = n;
}

/*
 * markreachable - 递归标记可达状态
 */
static void
markreachable(struct nfa *nfa,
			  struct state *s,
			  struct state *okay,	/* 只考虑带此标记的状态 */
			  struct state *mark)	/* 用于标记的值 */
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != okay)
		return;
	s->tmp = mark;

	for (a = s->outs; a != NULL; a = a->outchain)
		markreachable(nfa, a->to, okay, mark);
}

/*
 * markcanreach - 递归标记能抵达此处的状态
 */
static void
markcanreach(struct nfa *nfa,
			 struct state *s,
			 struct state *okay,	/* 只考虑带此标记的状态 */
			 struct state *mark)	/* 用于标记的值 */
{
	struct arc *a;

	/* 因递归，可能被推至栈溢出 */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != okay)
		return;
	s->tmp = mark;

	for (a = s->ins; a != NULL; a = a->inchain)
		markcanreach(nfa, a->from, okay, mark);
}

/*
 * analyze - 查明优化后 NFA 的潜在有用事实
 */
static long						/* 要并入的 re_info 位 */
analyze(struct nfa *nfa)
{
	struct arc *a;
	struct arc *aa;

	if (NISERR())
		return 0;

	/* 查 NFA 是否无法匹配任何内容 */
	if (nfa->pre->outs == NULL)
		return REG_UIMPOSSIBLE;

	/* 检测 NFA 是否匹配所有字符串（可带长度界） */
	checkmatchall(nfa);

	/* 检测 NFA 是否可能匹配零长度字符串 */
	for (a = nfa->pre->outs; a != NULL; a = a->outchain)
		for (aa = a->to->outs; aa != NULL; aa = aa->outchain)
			if (aa->to == nfa->post)
				return REG_UEMPTYMATCH;
	return 0;
}

/*
 * checkmatchall - 该 NFA 是否仅表示串长度检验？
 *
 * 若是，正确置 nfa->minmatchall 与 nfa->maxmatchall（初值 -1）
 * 并在 nfa->flags 中设置 MATCHALL 位。
 *
 * 要成功，我们要求所有弧都是 PLAIN 彩虹弧，除了
 * 伪色弧（即 BOS/BOL/EOS/EOL）。须能经彩虹弧抵达 
 * post 状态；图中若有环，则它们必须是
 * 自环弧，以确保每次环迭代都消耗
 * 恰好一个字符。（更长环成问题，因其造成
 * 非连续的可能匹配长度；我们没有好办法表示 
 * DUPINF 限度以外的那种长度情形。）
 *
 * 伪色弧会使情况稍复杂。已知它们只能
 * 作为 pre 状态出弧（BOS/BOL）或 post 状态入弧（
 * EOS/EOL）。在那里它们必须精确复制并行的彩虹弧，
 * 例如 pre 状态有一条到状态 2 的彩虹出弧，则必须有 BOS
 * 与 BOL 出弧到状态 2，且别无其他。缺失或多余的伪色
 * 弧可能出现，意味着 NFA 对相邻字符带有某种
 * 约束，因而它不是 matchall NFA。
 */
static void
checkmatchall(struct nfa *nfa)
{
	bool	  **haspaths;
	struct state *s;
	int			i;

	/*
	 * 若状态太多，就不必费心去检测 matchall。
	 * 此限度用于约束下面可能消耗的时间与内存。
	 * 注意：即便图全是彩虹弧，若状态数明显多于
	 * DUPINF，则很可能存在长度超过 DUPINF 的路径，
	 * 这反正会迫使我们失败。实践中，编写最大有限
	 * 路径长度为 K 的 matchall 正则的合理写法，
	 * 状态数往往不会比 K 多出很多。
	 */
	if (nfa->nstates > DUPINF * 2)
		return;

	/*
	 * 首先扫描所有状态，核实只出现彩虹弧，以及
	 * 紧邻 pre/post 状态的伪色弧。这让我们能快速
	 * 排除大多数并非 matchall 的 NFA。
	 */
	for (s = nfa->states; s != NULL; s = s->next)
	{
		struct arc *a;

		for (a = s->outs; a != NULL; a = a->outchain)
		{
			if (a->type != PLAIN)
				return;			/* 有 LACON 即非 matchall */
			if (a->co != RAINBOW)
			{
				if (nfa->cm->cd[a->co].flags & PSEUDO)
				{
					/*
					 * 伪色弧：核实它处于合法位置（虽似
					 * 很难失败，但还是确认一下）。
					 */
					if (s == nfa->pre &&
						(a->co == nfa->bos[0] || a->co == nfa->bos[1]))
						 /* okay BOS/BOL 弧 */ ;
					else if (a->to == nfa->post &&
							 (a->co == nfa->eos[0] || a->co == nfa->eos[1]))
						 /* okay EOS/EOL 弧 */ ;
					else
						return; /* 意外的伪色弧 */
					/* 下面还会再检查这些弧。 */
				}
				else
					return;		/* 任何其它色都使其非 matchall */
			}
		}
		/* 还要断言 tmp 字段可供使用。 */
		assert(s->tmp == NULL);
	}

	/*
	 * 下一步最便宜的检查是核实 pre 状态的 BOS/BOL
	 * 出弧与其彩虹出弧到达相同状态。
	 * 若否，则 NFA 对匹配串之前的字符表达了某种
	 * 约束，从而不是 matchall。同理，post 状态的
	 * EOS/EOL 入弧必须与其彩虹入弧一致。
	 */
	if (!check_out_colors_match(nfa->pre, RAINBOW, nfa->bos[0]) ||
		!check_out_colors_match(nfa->pre, RAINBOW, nfa->bos[1]) ||
		!check_in_colors_match(nfa->post, RAINBOW, nfa->eos[0]) ||
		!check_in_colors_match(nfa->post, RAINBOW, nfa->eos[1]))
		return;

	/*
	 * 初始化路径长度数组的数组，供
	 * checkmatchall_recurse 返回每状态结果。这让我们
	 * 能记忆化递归搜索，避免指数级耗时。
	 */
	haspaths = (bool **) MALLOC(nfa->nstates * sizeof(bool *));
	if (haspaths == NULL)
		return;					/* 静默失败 */
	memset(haspaths, 0, nfa->nstates * sizeof(bool *));

	/*
	 * 递归搜索图中通向 "post" 状态的全彩虹路径，
	 * 从 "pre" 状态出发，并计算这些路径的长度。
	 * （鉴于前述检查，应至少有一条这样的路径。
	 * 但仍可能得到假结果，若存在多状态环、
	 * 长度超过 DUPINF+1 的路径，或非算法性
	 * 失败如 ENOMEM。）
	 */
	if (checkmatchall_recurse(nfa, nfa->pre, haspaths))
	{
		/* 有用结果是 pre 状态的路径长度数组 */
		bool	   *haspath = haspaths[nfa->pre->no];
		int			minmatch,
					maxmatch,
					morematch;

		assert(haspath != NULL);

		/*
		 * haspath[] 现表示可能的路径长度集合；但我们
		 * 想把它收成最小与最大值，因为似乎不值得为
		 * 处理非连续可能匹配长度而复杂化 regexec.c。
		 * 先取第一段连续长度的最小与最大，再
		 * 核实不存在非连续长度。
		 */
		for (minmatch = 0; minmatch <= DUPINF + 1; minmatch++)
		{
			if (haspath[minmatch])
				break;
		}
		assert(minmatch <= DUPINF + 1); /* 否则该函数撒了谎 */
		for (maxmatch = minmatch; maxmatch < DUPINF + 1; maxmatch++)
		{
			if (!haspath[maxmatch + 1])
				break;
		}
		for (morematch = maxmatch + 1; morematch <= DUPINF + 1; morematch++)
		{
			if (haspath[morematch])
			{
				haspath = NULL; /* 失败，存在非连续长度 */
				break;
			}
		}

		if (haspath != NULL)
		{
			/*
			 * 成功，于是记录信息。此处有一细节：从 
			 * pre 状态起算的路径长度含 pre 到 initial 的
			 * 转移，故比实际匹配串长度多一。
			 * （我们在 checkmatchall_recurse 内未计入 
			 * final 到 post 的转移，但计入了这一步。）因此 
			 * checkmatchall_recurse 允许多一层路径长度，看似
			 * 超乎必要。此次减一也负责把 
			 * checkmatchall_recurse 的 "无穷" 定义 
			 * "DUPINF+1" 转为通常的 "DUPINF" 表示。
			 */
			assert(minmatch > 0);	/* 否则 pre 与 post 状态相邻 */
			nfa->minmatchall = minmatch - 1;
			nfa->maxmatchall = maxmatch - 1;
			nfa->flags |= MATCHALL;
		}
	}

	/* 清理 */
	for (i = 0; i < nfa->nstates; i++)
	{
		if (haspaths[i] != NULL)
			FREE(haspaths[i]);
	}
	FREE(haspaths);
}

/*
 * checkmatchall_recurse - 供 checkmatchall 的递归搜索
 *
 * s 是本层递归要检查的状态。
 * haspaths[] 是每状态出口路径长度数组的数组。
 *
 * 搜索成功则返回 true；若因多状态环或其它
 * 内部原因不得不失败则返回 false。
 * （因无法抵达 post 的 "死" 状态已被消除，
 * 且已核实只存在彩虹弧与匹配的
 * 伪色弧，每个状态都应有通向 post 的彩虹
 * 路径。故对递归调用得到 false 时，我们
 * 视为最好整体失败，而非仅该
 * 特定状态无法抵达 post 状态。）
 *
 * 成功时，在 haspaths[s->no] 存入 malloc 的结果数组，
 * 示出从 s 到 post 状态的可能路径长度。
 * 每状态 haspath[] 长度为 DUPINF+2。下标 
 * k=0 到 DUPINF 为真表示有长度为 k 的全彩虹路径
 * 从此状态到串尾。haspath[DUPINF+1] 为真表示所有
 * 长度 >= DUPINF+1 皆可能。（无法按这些规则
 * 表示的情形会导致失败。）
 *
 * 最终释放 haspath[] 数组由 checkmatchall 负责。
 */
static bool
checkmatchall_recurse(struct nfa *nfa, struct state *s, bool **haspaths)
{
	bool		result = false;
	bool		foundloop = false;
	bool	   *haspath;
	struct arc *a;

	/*
	 * 由于本函数会递归，可能被驱动到栈溢出。但我们
	 * 不必把它当作硬失败；只需认定 NFA 非 matchall。
	 */
	if (STACK_TOO_DEEP(nfa->v->re))
		return false;

	/* 万一搜索耗时很长，检查是否取消 */
	INTERRUPT(nfa->v->re);

	/* 为本状态创建 haspath 数组 */
	haspath = (bool *) MALLOC((DUPINF + 2) * sizeof(bool));
	if (haspath == NULL)
		return false;			/* 再视为非 matchall */
	memset(haspath, 0, (DUPINF + 2) * sizeof(bool));

	/* 标记本状态正在被访问 */
	assert(s->tmp == NULL);
	s->tmp = s;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->co != RAINBOW)
			continue;			/* 忽略伪色弧 */
		if (a->to == nfa->post)
		{
			/* 找到通向 post 状态的全彩虹路径 */
			result = true;

			/*
			 * 将本状态标为距串尾零步（
			 * 到 post 状态的转移不计入）。
			 */
			haspath[0] = true;
		}
		else if (a->to == s)
		{
			/* 发现长度为 1 的环，下面再处理。 */
			foundloop = true;
		}
		else if (a->to->tmp != NULL)
		{
			/* 它正忙，故发现长度 > 1 的环，失败。 */
			result = false;
			break;
		}
		else
		{
			/* 考虑经此 to 状态向前的路径。 */
			bool	   *nexthaspath;
			int			i;

			/* 若 to 状态尚未访问过，则递归 */
			if (haspaths[a->to->no] == NULL)
			{
				result = checkmatchall_recurse(nfa, a->to, haspaths);
				/* 有递归路径失败则失败 */
				if (!result)
					break;
			}
			else
			{
				/* 先前访问必已找到通向终点的路径 */
				result = true;
			}
			assert(a->to->tmp == NULL);
			nexthaspath = haspaths[a->to->no];
			assert(nexthaspath != NULL);

			/*
			 * 于是，对每条从 a->to 到串尾长度为 i 的路径，
			 * 都有一条从 s 到串尾长度为 i+1 的路径。
			 */
			if (nexthaspath[DUPINF] != nexthaspath[DUPINF + 1])
			{
				/*
				 * a->to 有长度恰为 DUPINF 的路径但无更长；
				 * 或有所有长度 > DUPINF 的路径却没有
				 * 恰好该长度的。无论哪种，我们都无法
				 * 正确表示自 s 可能路径长度，故失败。
				 */
				result = false;
				break;
			}
			/* 把这些路径长度的知识并入已有结果 */
			for (i = 0; i < DUPINF; i++)
				haspath[i + 1] |= nexthaspath[i];
			/* 无穷加一仍是无穷 */
			haspath[DUPINF + 1] |= nexthaspath[DUPINF + 1];
		}
	}

	if (result && foundloop)
	{
		/*
		 * 若本状态有长度为 1 的环，则找出到终点的最短
		 * 已知路径长度。该环意味着每个更大的
		 * 路径长度也都可能。（至于更长的长度是否
		 * 本已知为可能，并不重要。）
		 */
		int			i;

		for (i = 0; i <= DUPINF; i++)
		{
			if (haspath[i])
				break;
		}
		for (i++; i <= DUPINF + 1; i++)
			haspath[i] = true;
	}

	/* 报告完成的路径长度图 */
	assert(s->no < nfa->nstates);
	assert(haspaths[s->no] == NULL);
	haspaths[s->no] = haspath;

	/* 标记状态不再忙碌 */
	s->tmp = NULL;

	return result;
}

/*
 * check_out_colors_match - checkmatchall 的子例程
 *
 * 检查从 s 经色 co1 的弧可达的状态集合
 * 是否等价于经色 co2 的弧可达的集合。
 * checkmatchall 已核实本 NFA 的弧全是 PLAIN，
 * 故此处不必再查弧类型。
 */
static bool
check_out_colors_match(struct state *s, color co1, color co2)
{
	bool		result = true;
	struct arc *a;

	/*
	 * 为在线性时间完成，假定 NFA 不含重复
	 * 弧。遍历出弧，标记经色 co1 的弧可达的状态。
	 * 再遍历一遍，取消经色 co2 可达状态的标记；
	 * 若见到未标记状态，便知此 co2 弧
	 * 无匹配。然后再遍历，检查仍带标记的状态，
	 * 并无论如何把所有 tmp 字段复位为 NULL。
	 */
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->co == co1)
		{
			assert(a->to->tmp == NULL);
			a->to->tmp = a->to;
		}
	}
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->co == co2)
		{
			if (a->to->tmp != NULL)
				a->to->tmp = NULL;
			else
				result = false; /* 未匹配 co2 弧 */
		}
	}
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->co == co1)
		{
			if (a->to->tmp != NULL)
			{
				result = false; /* 未匹配 co1 弧 */
				a->to->tmp = NULL;
			}
		}
	}
	return result;
}

/*
 * check_in_colors_match - checkmatchall 的子例程
 *
 * 检查经色 co1 的弧能抵达 s 的状态集合
 * 是否等价于经色 co2 的弧能抵达 s 的集合。
 * checkmatchall 已核实本 NFA 的弧全是 PLAIN，
 * 故此处不必再查弧类型。
 */
static bool
check_in_colors_match(struct state *s, color co1, color co2)
{
	bool		result = true;
	struct arc *a;

	/*
	 * 算法与 check_out_colors_match 相同，只是改查
	 * s 入弧 from 态之色
	 */
	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->co == co1)
		{
			assert(a->from->tmp == NULL);
			a->from->tmp = a->from;
		}
	}
	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->co == co2)
		{
			if (a->from->tmp != NULL)
				a->from->tmp = NULL;
			else
				result = false; /* 未匹配 co2 弧 */
		}
	}
	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->co == co1)
		{
			if (a->from->tmp != NULL)
			{
				result = false; /* 未匹配 co1 弧 */
				a->from->tmp = NULL;
			}
		}
	}
	return result;
}

/*
 * compact - 构造 NFA 的紧凑表示
 */
static void
compact(struct nfa *nfa,
		struct cnfa *cnfa)
{
	struct state *s;
	struct arc *a;
	size_t		nstates;
	size_t		narcs;
	struct carc *ca;
	struct carc *first;

	assert(!NISERR());

	nstates = 0;
	narcs = 0;
	for (s = nfa->states; s != NULL; s = s->next)
	{
		nstates++;
		narcs += s->nouts + 1;	/* 需多一个作结束标记 */
	}

	cnfa->stflags = (char *) MALLOC(nstates * sizeof(char));
	cnfa->states = (struct carc **) MALLOC(nstates * sizeof(struct carc *));
	cnfa->arcs = (struct carc *) MALLOC(narcs * sizeof(struct carc));
	if (cnfa->stflags == NULL || cnfa->states == NULL || cnfa->arcs == NULL)
	{
		if (cnfa->stflags != NULL)
			FREE(cnfa->stflags);
		if (cnfa->states != NULL)
			FREE(cnfa->states);
		if (cnfa->arcs != NULL)
			FREE(cnfa->arcs);
		NERR(REG_ESPACE);
		return;
	}
	cnfa->nstates = nstates;
	cnfa->pre = nfa->pre->no;
	cnfa->post = nfa->post->no;
	cnfa->bos[0] = nfa->bos[0];
	cnfa->bos[1] = nfa->bos[1];
	cnfa->eos[0] = nfa->eos[0];
	cnfa->eos[1] = nfa->eos[1];
	cnfa->ncolors = maxcolor(nfa->cm) + 1;
	cnfa->flags = nfa->flags;
	cnfa->minmatchall = nfa->minmatchall;
	cnfa->maxmatchall = nfa->maxmatchall;

	ca = cnfa->arcs;
	for (s = nfa->states; s != NULL; s = s->next)
	{
		assert((size_t) s->no < nstates);
		cnfa->stflags[s->no] = 0;
		cnfa->states[s->no] = ca;
		first = ca;
		for (a = s->outs; a != NULL; a = a->outchain)
			switch (a->type)
			{
				case PLAIN:
					ca->co = a->co;
					ca->to = a->to->no;
					ca++;
					break;
				case LACON:
					assert(s->no != cnfa->pre);
					assert(a->co >= 0);
					ca->co = (color) (cnfa->ncolors + a->co);
					ca->to = a->to->no;
					ca++;
					cnfa->flags |= HASLACONS;
					break;
				default:
					NERR(REG_ASSERT);
					return;
			}
		carcsort(first, ca - first);
		ca->co = COLORLESS;
		ca->to = 0;
		ca++;
	}
	assert(ca == &cnfa->arcs[narcs]);
	assert(cnfa->nstates != 0);

	/* 标记无进展状态 */
	for (a = nfa->pre->outs; a != NULL; a = a->outchain)
		cnfa->stflags[a->to->no] = CNFA_NOPROGRESS;
	cnfa->stflags[nfa->pre->no] = CNFA_NOPROGRESS;
}

/*
 * carcsort - 按色排序紧凑 NFA 弧
 */
static void
carcsort(struct carc *first, size_t n)
{
	if (n > 1)
		qsort(first, n, sizeof(struct carc), carc_cmp);
}

static int
carc_cmp(const void *a, const void *b)
{
	const struct carc *aa = (const struct carc *) a;
	const struct carc *bb = (const struct carc *) b;

	if (aa->co < bb->co)
		return -1;
	if (aa->co > bb->co)
		return +1;
	if (aa->to < bb->to)
		return -1;
	if (aa->to > bb->to)
		return +1;
	/* 不会走到这里：此时不应再有重复弧： */
	return 0;
}

/*
 * freecnfa - 释放紧凑 NFA
 */
static void
freecnfa(struct cnfa *cnfa)
{
	assert(!NULLCNFA(*cnfa));	/* 不应已为空 */
	FREE(cnfa->stflags);
	FREE(cnfa->states);
	FREE(cnfa->arcs);
	ZAPCNFA(*cnfa);
}

/*
 * dumpnfa - 以人类可读形式转储 NFA
 */
static void
dumpnfa(struct nfa *nfa,
		FILE *f)
{
#ifdef REG_DEBUG
	struct state *s;
	int			nstates = 0;
	int			narcs = 0;

	fprintf(f, "pre %d, post %d", nfa->pre->no, nfa->post->no);
	if (nfa->bos[0] != COLORLESS)
		fprintf(f, ", bos [%ld]", (long) nfa->bos[0]);
	if (nfa->bos[1] != COLORLESS)
		fprintf(f, ", bol [%ld]", (long) nfa->bos[1]);
	if (nfa->eos[0] != COLORLESS)
		fprintf(f, ", eos [%ld]", (long) nfa->eos[0]);
	if (nfa->eos[1] != COLORLESS)
		fprintf(f, ", eol [%ld]", (long) nfa->eos[1]);
	if (nfa->flags & HASLACONS)
		fprintf(f, ", haslacons");
	if (nfa->flags & HASCANTMATCH)
		fprintf(f, ", hascantmatch");
	if (nfa->flags & MATCHALL)
	{
		fprintf(f, ", minmatchall %d", nfa->minmatchall);
		if (nfa->maxmatchall == DUPINF)
			fprintf(f, ", maxmatchall inf");
		else
			fprintf(f, ", maxmatchall %d", nfa->maxmatchall);
	}
	fprintf(f, "\n");
	for (s = nfa->states; s != NULL; s = s->next)
	{
		dumpstate(s, f);
		nstates++;
		narcs += s->nouts;
	}
	fprintf(f, "total of %d states, %d arcs\n", nstates, narcs);
	if (nfa->parent == NULL)
		dumpcolors(nfa->cm, f);
	fflush(f);
#endif
}

#ifdef REG_DEBUG				/* dumpnfa 的下属例程 */

/*
 * dumpstate - 以人类可读形式转储 NFA 状态
 */
static void
dumpstate(struct state *s,
		  FILE *f)
{
	struct arc *a;

	fprintf(f, "%d%s%c", s->no, (s->tmp != NULL) ? "T" : "",
			(s->flag) ? s->flag : '.');
	if (s->prev != NULL && s->prev->next != s)
		fprintf(f, "\tstate chain bad\n");
	if (s->nouts == 0)
		fprintf(f, "\tno out arcs\n");
	else
		dumparcs(s, f);
	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->to != s)
			fprintf(f, "\tlink from %d to %d on %d's in-chain\n",
					a->from->no, a->to->no, s->no);
	}
	fflush(f);
}

/*
 * dumparcs - 以人类可读形式转储出弧
 */
static void
dumparcs(struct state *s,
		 FILE *f)
{
	int			pos;
	struct arc *a;

	/* 通常先打印最旧的弧更清晰 */
	a = s->outs;
	assert(a != NULL);
	while (a->outchain != NULL)
		a = a->outchain;
	pos = 1;
	do
	{
		dumparc(a, s, f);
		if (pos == 5)
		{
			fprintf(f, "\n");
			pos = 1;
		}
		else
			pos++;
		a = a->outchainRev;
	} while (a != NULL);
	if (pos != 1)
		fprintf(f, "\n");
}

/*
 * dumparc - 转储一条出弧可读形式，含前缀制表符
 */
static void
dumparc(struct arc *a,
		struct state *s,
		FILE *f)
{
	struct arc *aa;

	fprintf(f, "\t");
	switch (a->type)
	{
		case PLAIN:
			if (a->co == RAINBOW)
				fprintf(f, "[*]");
			else
				fprintf(f, "[%ld]", (long) a->co);
			break;
		case AHEAD:
			if (a->co == RAINBOW)
				fprintf(f, ">*>");
			else
				fprintf(f, ">%ld>", (long) a->co);
			break;
		case BEHIND:
			if (a->co == RAINBOW)
				fprintf(f, "<*<");
			else
				fprintf(f, "<%ld<", (long) a->co);
			break;
		case LACON:
			fprintf(f, ":%ld:", (long) a->co);
			break;
		case '^':
		case '$':
			fprintf(f, "%c%d", a->type, (int) a->co);
			break;
		case EMPTY:
			break;
		case CANTMATCH:
			fprintf(f, "X");
			break;
		default:
			fprintf(f, "0x%x/0%lo", a->type, (long) a->co);
			break;
	}
	if (a->from != s)
		fprintf(f, "?%d?", a->from->no);
	for (aa = a->from->outs; aa != NULL; aa = aa->outchain)
		if (aa == a)
			break;				/* 注意跳出 */
	if (aa == NULL)
		fprintf(f, "?!?");		/* 出链中缺失 */
	fprintf(f, "->");
	if (a->to == NULL)
	{
		fprintf(f, "NULL");
		return;
	}
	fprintf(f, "%d", a->to->no);
	for (aa = a->to->ins; aa != NULL; aa = aa->inchain)
		if (aa == a)
			break;				/* 注意跳出 */
	if (aa == NULL)
		fprintf(f, "?!?");		/* 入链中缺失 */
}
#endif							/* REG_DEBUG */

/*
 * dumpcnfa - 以人类可读形式转储紧凑 NFA
 */
#ifdef REG_DEBUG
static void
dumpcnfa(struct cnfa *cnfa,
		 FILE *f)
{
	int			st;

	fprintf(f, "pre %d, post %d", cnfa->pre, cnfa->post);
	if (cnfa->bos[0] != COLORLESS)
		fprintf(f, ", bos [%ld]", (long) cnfa->bos[0]);
	if (cnfa->bos[1] != COLORLESS)
		fprintf(f, ", bol [%ld]", (long) cnfa->bos[1]);
	if (cnfa->eos[0] != COLORLESS)
		fprintf(f, ", eos [%ld]", (long) cnfa->eos[0]);
	if (cnfa->eos[1] != COLORLESS)
		fprintf(f, ", eol [%ld]", (long) cnfa->eos[1]);
	if (cnfa->flags & HASLACONS)
		fprintf(f, ", haslacons");
	if (cnfa->flags & MATCHALL)
	{
		fprintf(f, ", minmatchall %d", cnfa->minmatchall);
		if (cnfa->maxmatchall == DUPINF)
			fprintf(f, ", maxmatchall inf");
		else
			fprintf(f, ", maxmatchall %d", cnfa->maxmatchall);
	}
	fprintf(f, "\n");
	for (st = 0; st < cnfa->nstates; st++)
		dumpcstate(st, cnfa, f);
	fflush(f);
}
#endif

#ifdef REG_DEBUG				/* dumpcnfa 的下属例程 */

/*
 * dumpcstate - 以人类可读形式转储紧凑 NFA 状态
 */
static void
dumpcstate(int st,
		   struct cnfa *cnfa,
		   FILE *f)
{
	struct carc *ca;
	int			pos;

	fprintf(f, "%d%s", st, (cnfa->stflags[st] & CNFA_NOPROGRESS) ? ":" : ".");
	pos = 1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co == RAINBOW)
			fprintf(f, "\t[*]->%d", ca->to);
		else if (ca->co < cnfa->ncolors)
			fprintf(f, "\t[%ld]->%d", (long) ca->co, ca->to);
		else
			fprintf(f, "\t:%ld:->%d", (long) (ca->co - cnfa->ncolors), ca->to);
		if (pos == 5)
		{
			fprintf(f, "\n");
			pos = 1;
		}
		else
			pos++;
	}
	if (ca == cnfa->states[st] || pos != 1)
		fprintf(f, "\n");
	fflush(f);
}

#endif							/* REG_DEBUG */
