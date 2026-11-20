/*
 * timeline.h
 *
 * 读写时间线历史文件的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/timeline.h
 */
#ifndef TIMELINE_H
#define TIMELINE_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

/*
 * 这些结构的列表描述服务器的时间线历史。每个
 * TimeLineHistoryEntry 表示属于该历史的一段 WAL，
 * 从新到旧。'begin' 与 'end' 之间所有 WAL 位置属于
 * 该条目所代表的时间线。所有条目的 'begin' 与 'end'
 * 指针共同形成自时间起点的连续线
 * 至无穷。
 */
typedef struct
{
	TimeLineID	tli;
	XLogRecPtr	begin;			/* 含端点 */
	XLogRecPtr	end;			/* 不含端点，InvalidXLogRecPtr 表无穷 */
} TimeLineHistoryEntry;

extern List *readTimeLineHistory(TimeLineID targetTLI);
extern bool existsTimeLineHistory(TimeLineID probeTLI);
extern TimeLineID findNewestTimeLine(TimeLineID startTLI);
extern void writeTimeLineHistory(TimeLineID newTLI, TimeLineID parentTLI,
								 XLogRecPtr switchpoint, char *reason);
extern void writeTimeLineHistoryFile(TimeLineID tli, char *content, int size);
extern void restoreTimeLineHistoryFiles(TimeLineID begin, TimeLineID end);
extern bool tliInHistory(TimeLineID tli, List *expectedTLEs);
extern TimeLineID tliOfPointInHistory(XLogRecPtr ptr, List *history);
extern XLogRecPtr tliSwitchPoint(TimeLineID tli, List *history,
								 TimeLineID *nextTLI);

#endif							/* TIMELINE_H */
