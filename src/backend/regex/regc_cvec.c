/*
 * 处理 cvec 的工具函数
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
 * src/backend/regex/regc_cvec.c
 *
 */

/*
 * 注:
 * 仅本文件中（选定的）函数应将 cvec 的 chr 数组
 * 视为非常量。
 */

/*
 * newcvec - 分配一新 cvec
 */
static struct cvec *
newcvec(int nchrs,				/* 以容纳这么多 chr... */
		int nranges)			/* 以及这么多范围 */
{
	size_t		nc = (size_t) nchrs + (size_t) nranges * 2;
	size_t		n = sizeof(struct cvec) + nc * sizeof(chr);
	struct cvec *cv = (struct cvec *) MALLOC(n);

	if (cv == NULL)
		return NULL;
	cv->chrspace = nchrs;
	cv->chrs = (chr *) (((char *) cv) + sizeof(struct cvec));
	cv->ranges = cv->chrs + nchrs;
	cv->rangespace = nranges;
	return clearcvec(cv);
}

/*
 * clearcvec - 清可能新建 cvec
 * 返回指针以作便利。
 */
static struct cvec *
clearcvec(struct cvec *cv)
{
	assert(cv != NULL);
	cv->nchrs = 0;
	cv->nranges = 0;
	cv->cclasscode = -1;
	return cv;
}

/*
 * addchr - 向 cvec 加一 chr
 */
static void
addchr(struct cvec *cv,			/* 字符向量 */
	   chr c)					/* 待加字符 */
{
	assert(cv->nchrs < cv->chrspace);
	cv->chrs[cv->nchrs++] = c;
}

/*
 * addrange - 向 cvec 加一范围
 */
static void
addrange(struct cvec *cv,		/* 字符向量 */
		 chr from,				/* 范围首字符 */
		 chr to)				/* 范围末字符 */
{
	assert(cv->nranges < cv->rangespace);
	cv->ranges[cv->nranges * 2] = from;
	cv->ranges[cv->nranges * 2 + 1] = to;
	cv->nranges++;
}

/*
 * getcvec - 取一暂态 cvec，初始化为空
 *
 * 返回的 cvec 仅在下次调用 getcvec 前有效，该调用
 * 通常会回收其空间。调用方切勿显式释放
 * 该 cvec；它会在销毁 vars 时被清理。
 *
 * 这通常用于解释括号表达式。在该
 * 用法中 cvec 仅需短暂保留至我们由其造弧，
 * 故暂态性是便利的行为。
 */
static struct cvec *
getcvec(struct vars *v,			/* 语境 */
		int nchrs,				/* 以容纳这么多 chr... */
		int nranges)			/* 以及这么多范围 */
{
	/* 若够大则回收既有暂态 cvec */
	if (v->cv != NULL && nchrs <= v->cv->chrspace &&
		nranges <= v->cv->rangespace)
		return clearcvec(v->cv);

	/* 否，另造一个 */
	if (v->cv != NULL)
		freecvec(v->cv);
	v->cv = newcvec(nchrs, nranges);
	if (v->cv == NULL)
		ERR(REG_ESPACE);

	return v->cv;
}

/*
 * freecvec - 释放 cvec
 */
static void
freecvec(struct cvec *cv)
{
	FREE(cv);
}
