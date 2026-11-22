/*-------------------------------------------------------------------------
 *
 * catcache.h
 *	  底层系统目录缓存定义。
 *
 * 注意：每个目录缓存须在所缓存系统表上有
 * 对应的唯一索引——即索引须匹配本缓存用于查找的
 * 键。所有缓存取数（正常情况下）都用索引扫描。
 * 索引应唯一，保证一键组合至多一行匹配。
 * 见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/catcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATCACHE_H
#define CATCACHE_H

#include "access/htup.h"
#include "access/skey.h"
#include "lib/ilist.h"
#include "utils/relcache.h"

/*
 *		struct catctup:			individual tuple in the cache.
 *		struct catclist:		list of tuples matching a partial key.
 *		struct catcache:		information for managing a cache.
 *		struct catcacheheader:	information for managing all the caches.
 */

#define CATCACHE_MAXKEYS		4


/* 计算 datum 哈希的函数 */
typedef uint32 (*CCHashFN) (Datum datum);

/* 计算两 datum 是否相等的函数 */
typedef bool (*CCFastEqualFN) (Datum a, Datum b);

typedef struct catcache
{
	int			id;				/* 缓存标识符 --- 见 syscache.h */
	int			cc_nbuckets;	/* 本缓存中的哈希桶数 */
	TupleDesc	cc_tupdesc;		/* 元组描述符(自 reldesc 复制) */
	dlist_head *cc_bucket;		/* 哈希桶 */
	CCHashFN	cc_hashfunc[CATCACHE_MAXKEYS];	/* 各键的哈希函数 */
	CCFastEqualFN cc_fastequal[CATCACHE_MAXKEYS];	/* 各键的快等函数
													 * 见上 */
	int			cc_keyno[CATCACHE_MAXKEYS]; /* 各键的 AttrNumber */
	dlist_head	cc_lists;		/* CatCList 结构列表 */
	int			cc_ntup;		/* 当前在本缓存中的元组数 */
	int			cc_nkeys;		/* 键数（1..CATCACHE_MAXKEYS） */
	const char *cc_relname;		/* 元组来源关系名 */
	Oid			cc_reloid;		/* 元组来源关系的 OID */
	Oid			cc_indexoid;	/* 匹配缓存键的索引 OID */
	bool		cc_relisshared; /* 关系是否跨数据库共享？ */
	slist_node	cc_next;		/* 链表 */
	ScanKeyData cc_skey[CATCACHE_MAXKEYS];	/* 堆扫描预计算键信息
											 *  */

	/* 这些字段放此处以免 v16 中 ABI 破坏 */
	int			cc_nlist;		/* 本缓存中当前 CatCLists 数 */
	int			cc_nlbuckets;	/* 本缓存中 CatCList 哈希桶数 */
	dlist_head *cc_lbucket;		/* CatCLists 的哈希桶 */

	/*
	 * 放在末尾，以便以 CATCACHE_STATS 编译 catcache.c
	 * 时不破坏其他模块的 ABI
	 */
#ifdef CATCACHE_STATS
	long		cc_searches;	/* 对本缓存的搜索总次数 */
	long		cc_hits;		/* 命中既有条目的次数 */
	long		cc_neg_hits;	/* 命中否定条目的次数 */
	long		cc_newloads;	/* 成功装入新条目的次数 */

	/*
	 * cc_searches-(cc_hits+cc_neg_hits+cc_newloads) 为失败
	 * 搜索次数，每次都会装入一个否定条目
	 */
	long		cc_invals;		/* 自缓存失效的条目数 */
	long		cc_lsearches;	/* 列表搜索总次数 */
	long		cc_lhits;		/* 命中既有列表的次数 */
#endif
} CatCache;


typedef struct catctup
{
	int			ct_magic;		/* 用于标识 CatCTup 条目 */
#define CT_MAGIC   0x57261502

	uint32		hash_value;		/* 本元组各键的哈希值 */

	/*
	 * 条目的查找键。按引用的 datum 对正缓存条目指向
	 * 元组内，对否定条目则单独分配。
	 */
	Datum		keys[CATCACHE_MAXKEYS];

	/*
	 * 缓存中每个元组都是存储其哈希桶元素的 dlist
	 * 成员。我们按 LRU 次序维护各 dlist 以加速重复
	 * 查找.
	 */
	dlist_node	cache_elem;		/* 每桶列表的列表成员 */

	/*
	 * 标为 "dead" 的元组不得再被后续搜索返回。
	 * 但在引用计数归零前不会从缓存中物理删除。
	 * 若属某 CatCList，则该列表引用计数也须归零；
	 * 且在标记元组死时也要标记列表死。）
	 * 见上。
	 *
	 * 否定缓存条目断言没有元组匹配某特定键。就避免
	 * 目录搜索而言，这与普通条目同样有用。正条目与
	 * 否定条目的管理方式相同。
	 * 见上。
	 */
	int			refcount;		/* 活跃引用数 */
	bool		dead;			/* 已死但尚未移除？ */
	bool		negative;		/* 否定缓存条目？ */
	HeapTupleData tuple;		/* 元组管理头 */

	/*
	 * 元组还至多属于一个 CatCList。（若同一 catcache
	 * 用不同键数做列表搜索，因该限制可能得为同一元组
	 * 建多条目。当前少见，故接受潜在低效。）
	 * 见上。
	 * 见上。
	 */
	struct catclist *c_list;	/* 所属 CatCList，无则为 NULL */

	CatCache   *my_cache;		/* 链向所属 catcache */
	/* 其后为对齐的元组数据，否定条目除外 */
} CatCTup;


/*
 * CatCList 描述部分搜索结果，即仅用 N 键缓存的前
 * K 个键列搜索。把所用键存入 keys 属性以表示所存
 * 键集。CatCList 对象链向满足部分键的全部表行缓存
 * 条目。（注意：其中不会有否定缓存条目。）
 * 见上。
 *
 * CatCList 仅属每缓存列表成员；当前不把它们分入
 * 哈希桶。
 *
 * 标为 "dead" 的列表不得再被后续搜索返回。
 * 但在引用计数归零前不会从缓存中物理删除。
 * 若任何成员条目已死，则应标记列表死。
 * 见上。
 *
 * 若 ordered 为真，则成员元组按缓存底层索引次序
 * 出现。正常运行时为真，但引导或恢复操作中可能
 * 不然。（为真时 namespace.c 能省些周期。）
 * 见上。
 */
typedef struct catclist
{
	int			cl_magic;		/* 用于标识 CatCList 条目 */
#define CL_MAGIC   0x52765103

	uint32		hash_value;		/* 查找键的哈希值 */

	dlist_node	cache_elem;		/* 每缓存列表的列表成员 */

	/*
	 * 条目的查找键，前 nkeys 个元素有效。
	 * 所有按引用者均单独分配。
	 */
	Datum		keys[CATCACHE_MAXKEYS];

	int			refcount;		/* 活跃引用数 */
	bool		dead;			/* 已死但尚未移除？ */
	bool		ordered;		/* 成员按索引次序？ */
	short		nkeys;			/* 指定的查找键个数 */
	int			n_members;		/* 成员元组数 */
	CatCache   *my_cache;		/* 链向所属 catcache */
	CatCTup    *members[FLEXIBLE_ARRAY_MEMBER]; /* 成员 */
} CatCList;


typedef struct catcacheheader
{
	slist_head	ch_caches;		/* CatCache 结构列表头 */
	int			ch_ntup;		/* 全缓存元组数 */
} CatCacheHeader;


/* 此外联与 utils/memutils.h 重复... */
extern PGDLLIMPORT MemoryContext CacheMemoryContext;

extern void CreateCacheMemoryContext(void);

extern CatCache *InitCatCache(int id, Oid reloid, Oid indexoid,
							  int nkeys, const int *key,
							  int nbuckets);
extern void InitCatCachePhase2(CatCache *cache, bool touch_index);

extern HeapTuple SearchCatCache(CatCache *cache,
								Datum v1, Datum v2, Datum v3, Datum v4);
extern HeapTuple SearchCatCache1(CatCache *cache,
								 Datum v1);
extern HeapTuple SearchCatCache2(CatCache *cache,
								 Datum v1, Datum v2);
extern HeapTuple SearchCatCache3(CatCache *cache,
								 Datum v1, Datum v2, Datum v3);
extern HeapTuple SearchCatCache4(CatCache *cache,
								 Datum v1, Datum v2, Datum v3, Datum v4);
extern void ReleaseCatCache(HeapTuple tuple);

extern uint32 GetCatCacheHashValue(CatCache *cache,
								   Datum v1, Datum v2,
								   Datum v3, Datum v4);

extern CatCList *SearchCatCacheList(CatCache *cache, int nkeys,
									Datum v1, Datum v2,
									Datum v3);
extern void ReleaseCatCacheList(CatCList *list);

extern void ResetCatalogCaches(void);
extern void ResetCatalogCachesExt(bool debug_discard);
extern void CatalogCacheFlushCatalog(Oid catId);
extern void CatCacheInvalidate(CatCache *cache, uint32 hashValue);
extern void PrepareToInvalidateCacheTuple(Relation relation,
										  HeapTuple tuple,
										  HeapTuple newtuple,
										  void (*function) (int, uint32, Oid));

extern void PrintCatCacheLeakWarning(HeapTuple tuple);
extern void PrintCatCacheListLeakWarning(CatCList *list);

#endif							/* CATCACHE_H */
