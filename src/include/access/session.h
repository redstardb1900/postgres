/*-------------------------------------------------------------------------
 *
 * session.h
 *	  用户会话的封装。
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/include/access/session.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSION_H
#define SESSION_H

#include "lib/dshash.h"

/* 避免包含 typcache.h */
struct SharedRecordTypmodRegistry;

/*
 * 封装用户会话若干要素的结构体。目前它
 * 管理适用于并行查询的状态，但原则上也可
 * 纳入目前作为全局变量的其他内容。
 */
typedef struct Session
{
	dsm_segment *segment;		/* 会话范围的 DSM 段。 */
	dsa_area   *area;			/* 会话范围的 DSA 区。 */

	/* typcache.c 所管状态。 */
	struct SharedRecordTypmodRegistry *shared_typmod_registry;
	dshash_table *shared_record_table;
	dshash_table *shared_typmod_table;
} Session;

extern void InitializeSession(void);
extern dsm_handle GetSessionDsmHandle(void);
extern void AttachSession(dsm_handle handle);
extern void DetachSession(void);

/* 当前会话，若无则为 NULL。 */
extern PGDLLIMPORT Session *CurrentSession;

#endif							/* SESSION_H */
