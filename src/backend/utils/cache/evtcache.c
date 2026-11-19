/*-------------------------------------------------------------------------
 *
 * evtcache.c
 *	  事件触发器数据专用缓存。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/evtcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "tcop/cmdtag.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/evtcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef enum
{
	ETCS_NEEDS_REBUILD,
	ETCS_REBUILD_STARTED,
	ETCS_VALID
} EventTriggerCacheStateType;

typedef struct
{
	EventTriggerEvent event;
	List	   *triggerlist;
} EventTriggerCacheEntry;

static HTAB *EventTriggerCache;
static MemoryContext EventTriggerCacheContext;
static EventTriggerCacheStateType EventTriggerCacheState = ETCS_NEEDS_REBUILD;

static void BuildEventTriggerCache(void);
static void InvalidateEventCacheCallback(Datum arg,
										 int cacheid, uint32 hashvalue);
static Bitmapset *DecodeTextArrayToBitmapset(Datum array);

/*
 * 按触发器事件搜索事件缓存。
 *
 * 注意：调用者最好把想保留的任何数据复制到另
 * 一个内存上下文中，使它能跨过任何可能触及系统目
 * 录的操作继续存留，因为缓存复位可能毁掉返回值。
 */
List *
EventCacheLookup(EventTriggerEvent event)
{
	EventTriggerCacheEntry *entry;

	if (EventTriggerCacheState != ETCS_VALID)
		BuildEventTriggerCache();
	entry = hash_search(EventTriggerCache, &event, HASH_FIND, NULL);
	return entry != NULL ? entry->triggerlist : NIL;
}

/*
 * 重建事件触发器缓存。
 */
static void
BuildEventTriggerCache(void)
{
	HASHCTL		ctl;
	HTAB	   *cache;
	MemoryContext oldcontext;
	Relation	rel;
	Relation	irel;
	SysScanDesc scan;

	if (EventTriggerCacheContext != NULL)
	{
		/*
		 * 释放 EventTriggerCacheContext 中已分配的所有内存。
		 * 这可能因先前重建失败，或因重建完成之前
		 * 发生了失效。
		 */
		MemoryContextResetAndDeleteChildren(EventTriggerCacheContext);
	}
	else
	{
		/*
		 * 这是首次尝试构建缓存，因此需设置内存上下文
		 * 并注册 syscache 回调
		 * 来捕获今后的失效事件。
		 */
		if (CacheMemoryContext == NULL)
			CreateCacheMemoryContext();
		EventTriggerCacheContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "EventTriggerCache",
								  ALLOCSET_DEFAULT_SIZES);
		CacheRegisterSyscacheCallback(EVENTTRIGGEROID,
									  InvalidateEventCacheCallback,
									  (Datum) 0);
	}

	/* 切换到正确内存上下文。 */
	oldcontext = MemoryContextSwitchTo(EventTriggerCacheContext);

	/* 防止内存上下文在我们重建期间被清掉。 */
	EventTriggerCacheState = ETCS_REBUILD_STARTED;

	/* 创建新哈希表。 */
	ctl.keysize = sizeof(EventTriggerEvent);
	ctl.entrysize = sizeof(EventTriggerCacheEntry);
	ctl.hcxt = EventTriggerCacheContext;
	cache = hash_create("Event Trigger Cache", 32, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * 准备按名称顺序扫描 pg_event_trigger。
	 */
	rel = relation_open(EventTriggerRelationId, AccessShareLock);
	irel = index_open(EventTriggerNameIndexId, AccessShareLock);
	scan = systable_beginscan_ordered(rel, irel, NULL, 0, NULL);

	/*
	 * 为每个 pg_event_trigger 元组构建一个缓存项，并把每
	 * 项追加至相应条目。
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_event_trigger form;
		char	   *evtevent;
		EventTriggerEvent event;
		EventTriggerCacheItem *item;
		Datum		evttags;
		bool		evttags_isnull;
		EventTriggerCacheEntry *entry;
		bool		found;

		/* 取下元组。 */
		tup = systable_getnext_ordered(scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tup))
			break;

		/* 禁用则跳过触发器 */
		form = (Form_pg_event_trigger) GETSTRUCT(tup);
		if (form->evtenabled == TRIGGER_DISABLED)
			continue;

		/* 解码事件名。 */
		evtevent = NameStr(form->evtevent);
		if (strcmp(evtevent, "ddl_command_start") == 0)
			event = EVT_DDLCommandStart;
		else if (strcmp(evtevent, "ddl_command_end") == 0)
			event = EVT_DDLCommandEnd;
		else if (strcmp(evtevent, "sql_drop") == 0)
			event = EVT_SQLDrop;
		else if (strcmp(evtevent, "table_rewrite") == 0)
			event = EVT_TableRewrite;
		else
			continue;

		/* 分配新缓存项。 */
		item = palloc0(sizeof(EventTriggerCacheItem));
		item->fnoid = form->evtfoid;
		item->enabled = form->evtenabled;

		/* 解码并排序tags数组 */
		evttags = heap_getattr(tup, Anum_pg_event_trigger_evttags,
							   RelationGetDescr(rel), &evttags_isnull);
		if (!evttags_isnull)
			item->tagset = DecodeTextArrayToBitmapset(evttags);

		/* 加入缓存条目 */
		entry = hash_search(cache, &event, HASH_ENTER, &found);
		if (found)
			entry->triggerlist = lappend(entry->triggerlist, item);
		else
			entry->triggerlist = list_make1(item);
	}

	/* 完成 pg_event_trigger 扫描 */
	systable_endscan_ordered(scan);
	index_close(irel, AccessShareLock);
	relation_close(rel, AccessShareLock);

	/* 恢复先前内存上下文。 */
	MemoryContextSwitchTo(oldcontext);

	/* 安装新缓存。 */
	EventTriggerCache = cache;

	/*
	 * 若缓存自进入本例程起已失效，仍会使用并返回刚
	 * 构建完的缓存以避免无限循环，但保持缓存标记为过
	 * 时状态，以便下次访问时重建。否则便把缓存标
	 * 记为有效。
	 */
	if (EventTriggerCacheState == ETCS_REBUILD_STARTED)
		EventTriggerCacheState = ETCS_VALID;
}

/*
 * text[] 解码为 CommandTags 位图集。
 *
 * 如果我们愿意复制 deconstruct_array 中的部分逻辑，
 * 本可在此省去一点开销，但这似乎不值得增加代码复
 * 杂度。
 */
static Bitmapset *
DecodeTextArrayToBitmapset(Datum array)
{
	ArrayType  *arr = DatumGetArrayTypeP(array);
	Datum	   *elems;
	Bitmapset  *bms;
	int			i;
	int			nelems;

	if (ARR_NDIM(arr) != 1 || ARR_HASNULL(arr) || ARR_ELEMTYPE(arr) != TEXTOID)
		elog(ERROR, "expected 1-D text array");
	deconstruct_array_builtin(arr, TEXTOID, &elems, NULL, &nelems);

	for (bms = NULL, i = 0; i < nelems; ++i)
	{
		char	   *str = TextDatumGetCString(elems[i]);

		bms = bms_add_member(bms, GetCommandTagEnum(str));
		pfree(str);
	}

	pfree(elems);

	return bms;
}

/*
 * pg_event_trigger 被更新时刷写所有缓存条目。
 *
 * 这种情况应当足够罕见，不必做得很细，所以我们
 * 只管把一切全部清除，这也顺带避免内存泄漏的可能
 * 性。
 */
static void
InvalidateEventCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/*
	 * 若缓存无效，则可能重建正在进行，所以无法马上
	 * 把它清掉。但可行时这样做有利，可立即释放
	 * 内存。
	 */
	if (EventTriggerCacheState == ETCS_VALID)
	{
		MemoryContextResetAndDeleteChildren(EventTriggerCacheContext);
		EventTriggerCache = NULL;
	}

	/* 标记缓存待重建 */
	EventTriggerCacheState = ETCS_NEEDS_REBUILD;
}
