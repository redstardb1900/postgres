/*-------------------------------------------------------------------------
 *
 * tsvector.c
 *	  tsvector 的 I/O 函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "nodes/miscnodes.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"

typedef struct
{
	WordEntry	entry;			/* 须居首，见 compareentry */
	WordEntryPos *pos;
	int			poslen;			/* pos 的元素个数 */
} WordEntryIN;


/* 为 qsort 比较两个 WordEntryPos 值 */
int
compareWordEntryPos(const void *a, const void *b)
{
	int			apos = WEP_GETPOS(*(const WordEntryPos *) a);
	int			bpos = WEP_GETPOS(*(const WordEntryPos *) b);

	if (apos == bpos)
		return 0;
	return (apos > bpos) ? 1 : -1;
}

/*
 * 去除重复的 pos 条目。若两条目的 pos 相同但权重
 * 不同，则保留权重较高者，因此不能在此用
 * qunique。
 *
 * 返回新长度。
 */
static int
uniquePos(WordEntryPos *a, int l)
{
	WordEntryPos *ptr,
			   *res;

	if (l <= 1)
		return l;

	qsort(a, l, sizeof(WordEntryPos), compareWordEntryPos);

	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		if (WEP_GETPOS(*ptr) != WEP_GETPOS(*res))
		{
			res++;
			*res = *ptr;
			if (res - a >= MAXNUMPOS - 1 ||
				WEP_GETPOS(*res) == MAXENTRYPOS - 1)
				break;
		}
		else if (WEP_GETWEIGHT(*ptr) > WEP_GETWEIGHT(*res))
			WEP_SETWEIGHT(*res, WEP_GETWEIGHT(*ptr));
		ptr++;
	}

	return res + 1 - a;
}

/*
 * 为 qsort_arg 比较两个 WordEntry 结构体。本函数
 * 亦可用于 WordEntryIN 结构体，因其首字段即 WordEntry。
 */
static int
compareentry(const void *va, const void *vb, void *arg)
{
	const WordEntry *a = (const WordEntry *) va;
	const WordEntry *b = (const WordEntry *) vb;
	char	   *BufferStr = (char *) arg;

	return tsCompareString(&BufferStr[a->pos], a->len,
						   &BufferStr[b->pos], b->len,
						   false);
}

/*
 * 为 WordEntryIN 数组排序去重。
 * *outbuflen 接收词位与位置所需的空间量。
 */
static int
uniqueentry(WordEntryIN *a, int l, char *buf, int *outbuflen)
{
	int			buflen;
	WordEntryIN *ptr,
			   *res;

	Assert(l >= 1);

	if (l > 1)
		qsort_arg(a, l, sizeof(WordEntryIN), compareentry, buf);

	buflen = 0;
	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		if (!(ptr->entry.len == res->entry.len &&
			  strncmp(&buf[ptr->entry.pos], &buf[res->entry.pos],
					  res->entry.len) == 0))
		{
			/* *res 数据已收集完毕，统计所需空间 */
			buflen += res->entry.len;
			if (res->entry.haspos)
			{
				res->poslen = uniquePos(res->pos, res->poslen);
				buflen = SHORTALIGN(buflen);
				buflen += res->poslen * sizeof(WordEntryPos) + sizeof(uint16);
			}
			res++;
			if (res != ptr)
				memcpy(res, ptr, sizeof(WordEntryIN));
		}
		else if (ptr->entry.haspos)
		{
			if (res->entry.haspos)
			{
				/* 把 ptr 的位置追加到 res 的位置 */
				int			newlen = ptr->poslen + res->poslen;

				res->pos = (WordEntryPos *)
					repalloc(res->pos, newlen * sizeof(WordEntryPos));
				memcpy(&res->pos[res->poslen], ptr->pos,
					   ptr->poslen * sizeof(WordEntryPos));
				res->poslen = newlen;
				pfree(ptr->pos);
			}
			else
			{
				/* 直接给 pos 赋 ptr 的位置 */
				res->entry.haspos = 1;
				res->pos = ptr->pos;
				res->poslen = ptr->poslen;
			}
		}
		ptr++;
	}

	/* 统计最后一项所需空间 */
	buflen += res->entry.len;
	if (res->entry.haspos)
	{
		res->poslen = uniquePos(res->pos, res->poslen);
		buflen = SHORTALIGN(buflen);
		buflen += res->poslen * sizeof(WordEntryPos) + sizeof(uint16);
	}

	*outbuflen = buflen;
	return res + 1 - a;
}


Datum
tsvectorin(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	TSVectorParseState state;
	WordEntryIN *arr;
	int			totallen;
	int			arrlen;			/* arr 已分配大小 */
	WordEntry  *inarr;
	int			len = 0;
	TSVector	in;
	int			i;
	char	   *token;
	int			toklen;
	WordEntryPos *pos;
	int			poslen;
	char	   *strbuf;
	int			stroff;

	/*
	 * 词位会追加到 tmpbuf，cur 是已用空间的
	 * 末端指针。
	 */
	char	   *tmpbuf;
	char	   *cur;
	int			buflen = 256;	/* tmpbuf 已分配大小 */

	state = init_tsvector_parser(buf, 0, escontext);

	arrlen = 64;
	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * arrlen);
	cur = tmpbuf = (char *) palloc(buflen);

	while (gettoken_tsvector(state, &token, &toklen, &pos, &poslen, NULL))
	{
		if (toklen >= MAXSTRLEN)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long (%ld bytes, max %ld bytes)",
							(long) toklen,
							(long) (MAXSTRLEN - 1))));

		if (cur - tmpbuf > MAXSTRPOS)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("string is too long for tsvector (%ld bytes, max %ld bytes)",
							(long) (cur - tmpbuf), (long) MAXSTRPOS)));

		/*
		 * 按需扩容缓冲区
		 */
		if (len >= arrlen)
		{
			arrlen *= 2;
			arr = (WordEntryIN *)
				repalloc(arr, sizeof(WordEntryIN) * arrlen);
		}
		while ((cur - tmpbuf) + toklen >= buflen)
		{
			int			dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc(tmpbuf, buflen);
			cur = tmpbuf + dist;
		}
		arr[len].entry.len = toklen;
		arr[len].entry.pos = cur - tmpbuf;
		memcpy(cur, token, toklen);
		cur += toklen;

		if (poslen != 0)
		{
			arr[len].entry.haspos = 1;
			arr[len].pos = pos;
			arr[len].poslen = poslen;
		}
		else
		{
			arr[len].entry.haspos = 0;
			arr[len].pos = NULL;
			arr[len].poslen = 0;
		}
		len++;
	}

	close_tsvector_parser(state);

	/* gettoken_tsvector 失败? */
	if (SOFT_ERROR_OCCURRED(escontext))
		PG_RETURN_NULL();

	if (len > 0)
		len = uniqueentry(arr, len, tmpbuf, &buflen);
	else
		buflen = 0;

	if (buflen > MAXSTRPOS)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", buflen, MAXSTRPOS)));

	totallen = CALCDATASIZE(len, buflen);
	in = (TSVector) palloc0(totallen);
	SET_VARSIZE(in, totallen);
	in->size = len;
	inarr = ARRPTR(in);
	strbuf = STRPTR(in);
	stroff = 0;
	for (i = 0; i < len; i++)
	{
		memcpy(strbuf + stroff, &tmpbuf[arr[i].entry.pos], arr[i].entry.len);
		arr[i].entry.pos = stroff;
		stroff += arr[i].entry.len;
		if (arr[i].entry.haspos)
		{
			/* 受 MAXNUMPOS 限制，这里应该不可到达 */
			if (arr[i].poslen > 0xFFFF)
				elog(ERROR, "positions array too long");

			/* 复制位置个数 */
			stroff = SHORTALIGN(stroff);
			*(uint16 *) (strbuf + stroff) = (uint16) arr[i].poslen;
			stroff += sizeof(uint16);

			/* 复制位置 */
			memcpy(strbuf + stroff, arr[i].pos, arr[i].poslen * sizeof(WordEntryPos));
			stroff += arr[i].poslen * sizeof(WordEntryPos);

			pfree(arr[i].pos);
		}
		inarr[i] = arr[i].entry;
	}

	Assert((strbuf + stroff - (char *) in) == totallen);

	PG_RETURN_TSVECTOR(in);
}

Datum
tsvectorout(PG_FUNCTION_ARGS)
{
	TSVector	out = PG_GETARG_TSVECTOR(0);
	char	   *outbuf;
	int32		i,
				lenbuf = 0,
				pp;
	WordEntry  *ptr = ARRPTR(out);
	char	   *curbegin,
			   *curin,
			   *curout;

	lenbuf = out->size * 2 /* '' */ + out->size - 1 /* space */ + 2 /* \0 */ ;
	for (i = 0; i < out->size; i++)
	{
		lenbuf += ptr[i].len * 2 * pg_database_encoding_max_length() /* 转义用 */ ;
		if (ptr[i].haspos)
			lenbuf += 1 /* : */ + 7 /* int2 + , + weight */ * POSDATALEN(out, &(ptr[i]));
	}

	curout = outbuf = (char *) palloc(lenbuf);
	for (i = 0; i < out->size; i++)
	{
		curbegin = curin = STRPTR(out) + ptr->pos;
		if (i != 0)
			*curout++ = ' ';
		*curout++ = '\'';
		while (curin - curbegin < ptr->len)
		{
			int			len = pg_mblen(curin);

			if (t_iseq(curin, '\''))
				*curout++ = '\'';
			else if (t_iseq(curin, '\\'))
				*curout++ = '\\';

			while (len--)
				*curout++ = *curin++;
		}

		*curout++ = '\'';
		if ((pp = POSDATALEN(out, ptr)) != 0)
		{
			WordEntryPos *wptr;

			*curout++ = ':';
			wptr = POSDATAPTR(out, ptr);
			while (pp)
			{
				curout += sprintf(curout, "%d", WEP_GETPOS(*wptr));
				switch (WEP_GETWEIGHT(*wptr))
				{
					case 3:
						*curout++ = 'A';
						break;
					case 2:
						*curout++ = 'B';
						break;
					case 1:
						*curout++ = 'C';
						break;
					case 0:
					default:
						break;
				}

				if (pp > 1)
					*curout++ = ',';
				pp--;
				wptr++;
			}
		}
		ptr++;
	}

	*curout = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_CSTRING(outbuf);
}

/*
 * 二进制输入/输出函数。二进制格式如下：
 *
 * uint32	词位个数
 *
 * 每个词位：
 *		词位文本（客户端编码），NUL 结尾
 *		uint16	位置个数
 *		对每个位置：
 *			uint16 WordEntryPos
 */

Datum
tsvectorsend(PG_FUNCTION_ARGS)
{
	TSVector	vec = PG_GETARG_TSVECTOR(0);
	StringInfoData buf;
	int			i,
				j;
	WordEntry  *weptr = ARRPTR(vec);

	pq_begintypsend(&buf);

	pq_sendint32(&buf, vec->size);
	for (i = 0; i < vec->size; i++)
	{
		uint16		npos;

		/*
		 * TSVector 数组中的字符串不以空字符结尾，所以
		 * 必须单独发送空终止符
		 */
		pq_sendtext(&buf, STRPTR(vec) + weptr->pos, weptr->len);
		pq_sendbyte(&buf, '\0');

		npos = POSDATALEN(vec, weptr);
		pq_sendint16(&buf, npos);

		if (npos > 0)
		{
			WordEntryPos *wepptr = POSDATAPTR(vec, weptr);

			for (j = 0; j < npos; j++)
				pq_sendint16(&buf, wepptr[j]);
		}
		weptr++;
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsvectorrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSVector	vec;
	int			i;
	int32		nentries;
	int			datalen;		/* 固定长度 TSVector 头和
								 * WordEntries 之后的变长区域已用
								 * 字节数 */
	Size		hdrlen;
	Size		len;			/* vec 已分配大小 */
	bool		needSort = false;

	nentries = pq_getmsgint(buf, sizeof(int32));
	if (nentries < 0 || nentries > (MaxAllocSize / sizeof(WordEntry)))
		elog(ERROR, "invalid size of tsvector");

	hdrlen = DATAHDRSIZE + sizeof(WordEntry) * nentries;

	len = hdrlen * 2;			/* 翻倍以为词位留出空间 */
	vec = (TSVector) palloc0(len);
	vec->size = nentries;

	datalen = 0;
	for (i = 0; i < nentries; i++)
	{
		const char *lexeme;
		uint16		npos;
		size_t		lex_len;

		lexeme = pq_getmsgstring(buf);
		npos = (uint16) pq_getmsgint(buf, sizeof(uint16));

		/* 合法检查 */

		lex_len = strlen(lexeme);
		if (lex_len > MAXSTRLEN)
			elog(ERROR, "invalid tsvector: lexeme too long");

		if (datalen > MAXSTRPOS)
			elog(ERROR, "invalid tsvector: maximum total lexeme length exceeded");

		if (npos > MAXNUMPOS)
			elog(ERROR, "unexpected number of tsvector positions");

		/*
		 * 看起来有效。填充 WordEntry 并复制词位。
		 *
		 * 但首先要确保缓冲区足够大。
		 */
		while (hdrlen + SHORTALIGN(datalen + lex_len) +
			   sizeof(uint16) + npos * sizeof(WordEntryPos) >= len)
		{
			len *= 2;
			vec = (TSVector) repalloc(vec, len);
		}

		vec->entries[i].haspos = (npos > 0) ? 1 : 0;
		vec->entries[i].len = lex_len;
		vec->entries[i].pos = datalen;

		memcpy(STRPTR(vec) + datalen, lexeme, lex_len);

		datalen += lex_len;

		if (i > 0 && compareentry(&vec->entries[i],
								  &vec->entries[i - 1],
								  STRPTR(vec)) <= 0)
			needSort = true;

		/* 接收位置 */
		if (npos > 0)
		{
			uint16		j;
			WordEntryPos *wepptr;

			/*
			 * 必要时填充到 2 字节对齐。初始分配虽用了
			 * palloc0，但后续经 repalloc 扩大的内存区域
			 * 并未初始化为零。
			 */
			if (datalen != SHORTALIGN(datalen))
			{
				*(STRPTR(vec) + datalen) = '\0';
				datalen = SHORTALIGN(datalen);
			}

			memcpy(STRPTR(vec) + datalen, &npos, sizeof(uint16));

			wepptr = POSDATAPTR(vec, &vec->entries[i]);
			for (j = 0; j < npos; j++)
			{
				wepptr[j] = (WordEntryPos) pq_getmsgint(buf, sizeof(WordEntryPos));
				if (j > 0 && WEP_GETPOS(wepptr[j]) <= WEP_GETPOS(wepptr[j - 1]))
					elog(ERROR, "position information is misordered");
			}

			datalen += sizeof(uint16) + npos * sizeof(WordEntryPos);
		}
	}

	SET_VARSIZE(vec, hdrlen + datalen);

	if (needSort)
		qsort_arg(ARRPTR(vec), vec->size, sizeof(WordEntry),
				  compareentry, STRPTR(vec));

	PG_RETURN_TSVECTOR(vec);
}
