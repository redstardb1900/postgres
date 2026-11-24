/*-------------------------------------------------------------------------
 *
 * stringinfo_mb.c
 *		感知多字节编码的额外 StringInfo 设施
 *
 * 与 common/stringinfo.c 分开，使前端用户
 * 无需拉入多余的多字节编码支持代码
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/stringinfo_mb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/stringinfo_mb.h"
#include "mb/pg_wchar.h"


/*
 * appendStringInfoStringQuoted
 *
 * 自 s 向 str 追加至多 maxlen 字节；若
 * maxlen < 0 则追加整串，并加单引号且把单引号加倍。
 * 若拷贝不完整则追加省略号。
 */
void
appendStringInfoStringQuoted(StringInfo str, const char *s, int maxlen)
{
	char	   *copy = NULL;
	const char *chunk_search_start,
			   *chunk_copy_start,
			   *chunk_end;
	int			slen;
	bool		ellipsis;

	Assert(str != NULL);

	slen = strlen(s);
	if (maxlen >= 0 && maxlen < slen)
	{
		int			finallen = pg_mbcliplen(s, slen, maxlen);

		copy = pnstrdup(s, finallen);
		chunk_search_start = copy;
		chunk_copy_start = copy;

		ellipsis = true;
	}
	else
	{
		chunk_search_start = s;
		chunk_copy_start = s;

		ellipsis = false;
	}

	appendStringInfoCharMacro(str, '\'');

	while ((chunk_end = strchr(chunk_search_start, '\'')) != NULL)
	{
		/* 拷贝到（含）找到的定界 ' */
		appendBinaryStringInfoNT(str,
								 chunk_copy_start,
								 chunk_end - chunk_copy_start + 1);

		/* 为将其加倍，也把此 ' 纳入下一块 */
		chunk_copy_start = chunk_end;
		chunk_search_start = chunk_end + 1;
	}

	/* 拷贝最后一块并结束 */
	if (ellipsis)
		appendStringInfo(str, "%s...'", chunk_copy_start);
	else
		appendStringInfo(str, "%s'", chunk_copy_start);

	if (copy)
		pfree(copy);
}
