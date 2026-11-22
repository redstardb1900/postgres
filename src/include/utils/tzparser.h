/*-------------------------------------------------------------------------
 *
 * tzparser.h
 *	  时区偏移文件解析定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/tzparser.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TZPARSER_H
#define TZPARSER_H

#include "utils/datetime.h"

/*
 * 解析时区配置文件的结果是按 abbrev 排序的这些
 * 结构的数组。导出因 datetime.c 需要。
 * 见上。
 */
typedef struct tzEntry
{
	/* 实际数据 */
	char	   *abbrev;			/* 时区缩写（已小写） */
	char	   *zone;			/* 动态缩写则为区名，否则 NULL */
	/* 对动态缩写，不用 offset/is_dst */
	int			offset;			/* 相对 UTC 的偏移秒数 */
	bool		is_dst;			/* 若为 DST 缩写则为真 */
	/* 来源信息（供错误消息） */
	int			lineno;
	const char *filename;
} tzEntry;


extern TimeZoneAbbrevTable *load_tzoffsets(const char *filename);

#endif							/* TZPARSER_H */
