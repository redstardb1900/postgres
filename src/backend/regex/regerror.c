/*
 * regerror - 错误码展开
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
 * src/backend/regex/regerror.c
 *
 */

#include "regex/regguts.h"

/* 未知错误说明 */
static const char unk[] = "*** unknown regex error code 0x%x ***";

/* 在码、码名与说明间映射的结构 */
static const struct rerr
{
	int			code;
	const char *name;
	const char *explain;
}			rerrs[] =

{
	/* 实际表由 regex.h 建成 */
#include "regex/regerrs.h"		/* pgrminclude ignore */
	{
		-1, "", "oops"
	},							/* 说明在代码中特殊处理 */
};

/*
 * pg_regerror - 错误号接口
 */
/* ARGSUSED */
size_t							/* 实际所需空间（含 NUL） */
pg_regerror(int errcode,		/* 错误码或 REG_ATOI 或 REG_ITOA */
			const regex_t *preg,	/* 关联的 regex_t（目前未用） */
			char *errbuf,		/* 结果缓冲(除非 errbuf_size==0) */
			size_t errbuf_size) /* errbuf 可用空间，可为 0 */
{
	const struct rerr *r;
	const char *msg;
	char		convbuf[sizeof(unk) + 50];	/* 50 = 对 int 足够 */
	size_t		len;
	int			icode;

	switch (errcode)
	{
		case REG_ATOI:			/* 名称转数字 */
			for (r = rerrs; r->code >= 0; r++)
				if (strcmp(r->name, errbuf) == 0)
					break;
			sprintf(convbuf, "%d", r->code);	/* -1 为未知 */
			msg = convbuf;
			break;
		case REG_ITOA:			/* 数字转名称 */
			icode = atoi(errbuf);	/* 失败也非我们的问题 */
			for (r = rerrs; r->code >= 0; r++)
				if (r->code == icode)
					break;
			if (r->code >= 0)
				msg = r->name;
			else
			{					/* 未知；告知其号码 */
				sprintf(convbuf, "REG_%u", (unsigned) icode);
				msg = convbuf;
			}
			break;
		default:				/* 真实的普通错误码 */
			for (r = rerrs; r->code >= 0; r++)
				if (r->code == errcode)
					break;
			if (r->code >= 0)
				msg = r->explain;
			else
			{					/* 未知；直说 */
				sprintf(convbuf, unk, errcode);
				msg = convbuf;
			}
			break;
	}

	len = strlen(msg) + 1;		/* 所需空间，含 NUL */
	if (errbuf_size > 0)
	{
		if (errbuf_size > len)
			strcpy(errbuf, msg);
		else
		{						/* 截断以适配 */
			memcpy(errbuf, msg, errbuf_size - 1);
			errbuf[errbuf_size - 1] = '\0';
		}
	}

	return len;
}
