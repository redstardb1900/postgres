/*-------------------------------------------------------------------------
 *
 * evtcache.h
 *	  事件触发器数据的专用缓存。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/evtcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVTCACHE_H
#define EVTCACHE_H

#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"

typedef enum
{
	EVT_DDLCommandStart,
	EVT_DDLCommandEnd,
	EVT_SQLDrop,
	EVT_TableRewrite
} EventTriggerEvent;

typedef struct
{
	Oid			fnoid;			/*  要调用的函数  */
	char		enabled;		/* as SESSION_REPLICATION_ROLE_* */
	Bitmapset  *tagset;			/*  命令标签；空则 NULL  */
} EventTriggerCacheItem;

extern List *EventCacheLookup(EventTriggerEvent event);

#endif							/* EVTCACHE_H */
