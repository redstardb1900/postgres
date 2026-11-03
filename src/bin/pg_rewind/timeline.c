/*-------------------------------------------------------------------------
 *
 * timeline.c
 *	  timeline-related functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "pg_rewind.h"

/*
 * 这是从后端 readTimeLineHistory 复制粘贴而来，
 * 修改为返回 malloc 分配的数组并在无后端函数的
 * 情况下工作。
                                                               */
/*
 * 尝试读取时间线的历史文件。
 *
 * 若成功，返回组成 TLI 的列表（给定 TLI 及其
 * 祖先 TLI）。若找不到历史文件，假设该时间线
 * 没有父级，返回仅含指定时间线 ID 的列表。
                                                                                                                                                                                     */
TimeLineHistoryEntry *
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries)
{
	char	   *fline;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *entries = NULL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * 解析文件...
	           */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline)
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		fline = bufptr;
		while (*bufptr && *bufptr != '\n')
			bufptr++;
		if (!(*bufptr))
			lastline = true;
		else
			*bufptr++ = '\0';

		/* 跳过前导空白并检查 # 注释                                  */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* 期望行的第一个字段是数字时间线 ID                                  */
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a numeric timeline ID.");
			exit(1);
		}
		if (nfields != 3)
		{
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a write-ahead log switchpoint location.");
			exit(1);
		}
		if (entries && tli <= lasttli)
		{
			pg_log_error("invalid data in history file: %s", fline);
			pg_log_error_detail("Timeline IDs must be in increasing sequence.");
			exit(1);
		}

		lasttli = tli;

		nlines++;
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));

		entry = &entries[nlines - 1];
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;

		/* 忽略每行的剩余部分                            */
	}

	if (entries && targetTLI <= lasttli)
	{
		pg_log_error("invalid data in history file");
		pg_log_error_detail("Timeline IDs must be less than child timeline's ID.");
		exit(1);
	}

	/*
	 * 为时间线的"顶端"再创建一个条目，该顶端在
	 * 历史文件中没有条目。
	                                                             */
	nlines++;
	if (entries)
		entries = pg_realloc(entries, nlines * sizeof(TimeLineHistoryEntry));
	else
		entries = pg_malloc(1 * sizeof(TimeLineHistoryEntry));

	entry = &entries[nlines - 1];
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;

	*nentries = nlines;
	return entries;
}
