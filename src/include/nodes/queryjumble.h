/*-------------------------------------------------------------------------
 *
 * queryjumble.h
 *	查询规范化与指纹。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/nodes/queryjumble.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUERYJUMBLE_H
#define QUERYJUMBLE_H

#include "nodes/parsenodes.h"

/*
 * 规范化期间跟踪常量位置/长度的结构
 */
typedef struct LocationLen
{
	int			location;		/* 查询文本起始偏移 */
	int			length;			/* 字节长,-1 则忽略 */
} LocationLen;

/*
 * 计算查询 jumble 并生成规范化查询串的
 * 工作状态
 */
typedef struct JumbleState
{
	/* 当前查询树的 Jumble */
	unsigned char *jumble;

	/* jumble[] 中已用字节数 */
	Size		jumble_len;

	/* 应移除的常量位置数组 */
	LocationLen *clocations;

	/* clocations 数组的已分配长度 */
	int			clocations_buf_size;

	/* clocations 数组中当前有效项数 */
	int			clocations_count;

	/* 所见最高 Param id，以便正确开始规范化 */
	int			highest_extern_param_id;
} JumbleState;

/* compute_query_id GUC 的取值 */
enum ComputeQueryIdType
{
	COMPUTE_QUERY_ID_OFF,
	COMPUTE_QUERY_ID_ON,
	COMPUTE_QUERY_ID_AUTO,
	COMPUTE_QUERY_ID_REGRESS
};

/* GUC 参数 */
extern PGDLLIMPORT int compute_query_id;


extern const char *CleanQuerytext(const char *query, int *location, int *len);
extern JumbleState *JumbleQuery(Query *query);
extern void EnableQueryId(void);

extern PGDLLIMPORT bool query_id_enabled;

/*
 * 返回是否已启用查询标识计算：直接在 GUC 中，
 * 或在设置为 auto 时由模块启用。
 */
static inline bool
IsQueryIdEnabled(void)
{
	if (compute_query_id == COMPUTE_QUERY_ID_OFF)
		return false;
	if (compute_query_id == COMPUTE_QUERY_ID_ON)
		return true;
	return query_id_enabled;
}

#endif							/* QUERYJUMBLE_H */
