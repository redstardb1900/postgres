/*-------------------------------------------------------------------------
 *
 * catcache.c
 *	  按键匹配元组的系统目录缓存
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/catcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#ifdef CATCACHE_STATS
#include "storage/ipc.h"		/* 为 on_proc_exit */
#endif
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/syscache.h"

/*
 * 若在创建 catcache 条目（或列表）时处理了失效，
 * 它可能作用于我们正在创建的条目，使其在被插入
 * catcache 之前就失效。为捕捉这类情形，我们用一个
 * "创建中"条目栈。缓存失效除了标记实际的 CatCTup
 * 与 CatCList 条目之外，还会把栈中任何匹配的条目标为
 * 死亡。
 */
typedef struct CatCInProgress
{
	CatCache   *cache;			/* 该条目所属的缓存 */
	uint32		hash_value;		/* 条目的哈希值；列表忽略 */
	bool		list;			/* 是否列表条目 */
	bool		dead;			/* 条目被失效时置位 */
	struct CatCInProgress *next;
} CatCInProgress;

static CatCInProgress *catcache_in_progress_stack = NULL;

 /* #define CACHEDEBUG */	/* turns DEBUG elogs on */

/*
 * 给定哈希值和哈希表大小，找出
 * 该值所属的桶。哈希表元素数必须是 2 的幂，因此
 * 这只是简单的位掩码运算。
 */
#define HASH_INDEX(h, sz) ((Index) ((h) & ((sz) - 1)))


/*
 *		变量、宏和其他杂项
 */

#ifdef CACHEDEBUG
#define CACHE_elog(...)				elog(__VA_ARGS__)
#else
#define CACHE_elog(...)
#endif

/* 缓存管理头 --- 指针在创建之前为 NULL */
static CatCacheHeader *CacheHdr = NULL;

static inline HeapTuple SearchCatCacheInternal(CatCache *cache,
											   int nkeys,
											   Datum v1, Datum v2,
											   Datum v3, Datum v4);

static pg_noinline HeapTuple SearchCatCacheMiss(CatCache *cache,
												int nkeys,
												uint32 hashValue,
												Index hashIndex,
												Datum v1, Datum v2,
												Datum v3, Datum v4);

static uint32 CatalogCacheComputeHashValue(CatCache *cache, int nkeys,
										   Datum v1, Datum v2, Datum v3, Datum v4);
static uint32 CatalogCacheComputeTupleHashValue(CatCache *cache, int nkeys,
												HeapTuple tuple);
static inline bool CatalogCacheCompareTuple(const CatCache *cache, int nkeys,
											const Datum *cachekeys,
											const Datum *searchkeys);

#ifdef CATCACHE_STATS
static void CatCachePrintStats(int code, Datum arg);
#endif
static void CatCacheRemoveCTup(CatCache *cache, CatCTup *ct);
static void CatCacheRemoveCList(CatCache *cache, CatCList *cl);
static void RehashCatCache(CatCache *cp);
static void RehashCatCacheLists(CatCache *cp);
static void CatalogCacheInitializeCache(CatCache *cache);
static CatCTup *CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp,
										Datum *arguments,
										uint32 hashValue, Index hashIndex);

static void CatCacheFreeKeys(TupleDesc tupdesc, int nkeys, int *attnos,
							 Datum *keys);
static void CatCacheCopyKeys(TupleDesc tupdesc, int nkeys, int *attnos,
							 Datum *srckeys, Datum *dstkeys);


/*
 *					内部支持函数
 */

/*
 * 供缓存键字段使用的系统类型的哈希与相等性函数。
 * 某些情况下我们只调用相应数据类型的常规 SQL 函数，
 * 但这往往稍慢，而这些函数速度是性能攸关的。因此
 * 对于常充当 catcache 键的数据类型，我们在此硬编码
 * 其逻辑。避免 DirectFunctionCallN(...) 的开销就是一大赢面，
 * 而且在某些情形（如 int4）还能采用更快的
 * 哈希算法。
 */

static bool
chareqfast(Datum a, Datum b)
{
	return DatumGetChar(a) == DatumGetChar(b);
}

static uint32
charhashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetChar(datum));
}

static bool
nameeqfast(Datum a, Datum b)
{
	char	   *ca = NameStr(*DatumGetName(a));
	char	   *cb = NameStr(*DatumGetName(b));

	return strncmp(ca, cb, NAMEDATALEN) == 0;
}

static uint32
namehashfast(Datum datum)
{
	char	   *key = NameStr(*DatumGetName(datum));

	return hash_any((unsigned char *) key, strlen(key));
}

static bool
int2eqfast(Datum a, Datum b)
{
	return DatumGetInt16(a) == DatumGetInt16(b);
}

static uint32
int2hashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetInt16(datum));
}

static bool
int4eqfast(Datum a, Datum b)
{
	return DatumGetInt32(a) == DatumGetInt32(b);
}

static uint32
int4hashfast(Datum datum)
{
	return murmurhash32((int32) DatumGetInt32(datum));
}

static bool
texteqfast(Datum a, Datum b)
{
	/*
	 * 这里使用 DEFAULT_COLLATION_OID 相当随意。我们只是
	 * 想走 texteq() 中的快速"确定性"路径。
	 */
	return DatumGetBool(DirectFunctionCall2Coll(texteq, DEFAULT_COLLATION_OID, a, b));
}

static uint32
texthashfast(Datum datum)
{
	/* 此处同理，见 texteqfast() */
	return DatumGetInt32(DirectFunctionCall1Coll(hashtext, DEFAULT_COLLATION_OID, datum));
}

static bool
oidvectoreqfast(Datum a, Datum b)
{
	return DatumGetBool(DirectFunctionCall2(oidvectoreq, a, b));
}

static uint32
oidvectorhashfast(Datum datum)
{
	return DatumGetInt32(DirectFunctionCall1(hashoidvector, datum));
}

/* 某类型的查找支持函数。 */
static void
GetCCHashEqFuncs(Oid keytype, CCHashFN *hashfunc, RegProcedure *eqfunc, CCFastEqualFN *fasteqfunc)
{
	switch (keytype)
	{
		case BOOLOID:
			*hashfunc = charhashfast;
			*fasteqfunc = chareqfast;
			*eqfunc = F_BOOLEQ;
			break;
		case CHAROID:
			*hashfunc = charhashfast;
			*fasteqfunc = chareqfast;
			*eqfunc = F_CHAREQ;
			break;
		case NAMEOID:
			*hashfunc = namehashfast;
			*fasteqfunc = nameeqfast;
			*eqfunc = F_NAMEEQ;
			break;
		case INT2OID:
			*hashfunc = int2hashfast;
			*fasteqfunc = int2eqfast;
			*eqfunc = F_INT2EQ;
			break;
		case INT4OID:
			*hashfunc = int4hashfast;
			*fasteqfunc = int4eqfast;
			*eqfunc = F_INT4EQ;
			break;
		case TEXTOID:
			*hashfunc = texthashfast;
			*fasteqfunc = texteqfast;
			*eqfunc = F_TEXTEQ;
			break;
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
			*hashfunc = int4hashfast;
			*fasteqfunc = int4eqfast;
			*eqfunc = F_OIDEQ;
			break;
		case OIDVECTOROID:
			*hashfunc = oidvectorhashfast;
			*fasteqfunc = oidvectoreqfast;
			*eqfunc = F_OIDVECTOREQ;
			break;
		default:
			elog(FATAL, "type %u not supported as catcache key", keytype);
			*hashfunc = NULL;	/* 让编译器安静 */

			*eqfunc = InvalidOid;
			break;
	}
}

/*
 *		CatalogCacheComputeHashValue
 *
 * 计算与给定查找键集合相关联的哈希值
 */
static uint32
CatalogCacheComputeHashValue(CatCache *cache, int nkeys,
							 Datum v1, Datum v2, Datum v3, Datum v4)
{
	uint32		hashValue = 0;
	uint32		oneHash;
	CCHashFN   *cc_hashfunc = cache->cc_hashfunc;

	CACHE_elog(DEBUG2, "CatalogCacheComputeHashValue %s %d %p",
			   cache->cc_relname, nkeys, cache);

	switch (nkeys)
	{
		case 4:
			oneHash = (cc_hashfunc[3]) (v4);
			hashValue ^= pg_rotate_left32(oneHash, 24);
			/* FALLTHROUGH */
		case 3:
			oneHash = (cc_hashfunc[2]) (v3);
			hashValue ^= pg_rotate_left32(oneHash, 16);
			/* FALLTHROUGH */
		case 2:
			oneHash = (cc_hashfunc[1]) (v2);
			hashValue ^= pg_rotate_left32(oneHash, 8);
			/* FALLTHROUGH */
		case 1:
			oneHash = (cc_hashfunc[0]) (v1);
			hashValue ^= oneHash;
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", nkeys);
			break;
	}

	return hashValue;
}

/*
 *		CatalogCacheComputeTupleHashValue
 *
 * 计算与待缓存元组相关联的哈希值
 */
static uint32
CatalogCacheComputeTupleHashValue(CatCache *cache, int nkeys, HeapTuple tuple)
{
	Datum		v1 = 0,
				v2 = 0,
				v3 = 0,
				v4 = 0;
	bool		isNull = false;
	int		   *cc_keyno = cache->cc_keyno;
	TupleDesc	cc_tupdesc = cache->cc_tupdesc;

	/* 现在从元组抽取键字段并插入 scankey */
	switch (nkeys)
	{
		case 4:
			v4 = fastgetattr(tuple,
							 cc_keyno[3],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 3:
			v3 = fastgetattr(tuple,
							 cc_keyno[2],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 2:
			v2 = fastgetattr(tuple,
							 cc_keyno[1],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			/* FALLTHROUGH */
		case 1:
			v1 = fastgetattr(tuple,
							 cc_keyno[0],
							 cc_tupdesc,
							 &isNull);
			Assert(!isNull);
			break;
		default:
			elog(FATAL, "wrong number of hash keys: %d", nkeys);
			break;
	}

	return CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);
}

/*
 *		CatalogCacheCompareTuple
 *
 * 将元组与传入的参数相比较。
 */
static inline bool
CatalogCacheCompareTuple(const CatCache *cache, int nkeys,
						 const Datum *cachekeys,
						 const Datum *searchkeys)
{
	const CCFastEqualFN *cc_fastequal = cache->cc_fastequal;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		if (!(cc_fastequal[i]) (cachekeys[i], searchkeys[i]))
			return false;
	}
	return true;
}


#ifdef CATCACHE_STATS

static void
CatCachePrintStats(int code, Datum arg)
{
	slist_iter	iter;
	long		cc_searches = 0;
	long		cc_hits = 0;
	long		cc_neg_hits = 0;
	long		cc_newloads = 0;
	long		cc_invals = 0;
	long		cc_nlists = 0;
	long		cc_lsearches = 0;
	long		cc_lhits = 0;

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		if (cache->cc_ntup == 0 && cache->cc_searches == 0)
			continue;			/* 不打印未用缓存 */
		elog(DEBUG2, "catcache %s/%u: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %d lists, %ld lsrch, %ld lhits",
			 cache->cc_relname,
			 cache->cc_indexoid,
			 cache->cc_ntup,
			 cache->cc_searches,
			 cache->cc_hits,
			 cache->cc_neg_hits,
			 cache->cc_hits + cache->cc_neg_hits,
			 cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits - cache->cc_newloads,
			 cache->cc_searches - cache->cc_hits - cache->cc_neg_hits,
			 cache->cc_invals,
			 cache->cc_nlist,
			 cache->cc_lsearches,
			 cache->cc_lhits);
		cc_searches += cache->cc_searches;
		cc_hits += cache->cc_hits;
		cc_neg_hits += cache->cc_neg_hits;
		cc_newloads += cache->cc_newloads;
		cc_invals += cache->cc_invals;
		cc_nlists += cache->cc_nlist;
		cc_lsearches += cache->cc_lsearches;
		cc_lhits += cache->cc_lhits;
	}
	elog(DEBUG2, "catcache totals: %d tup, %ld srch, %ld+%ld=%ld hits, %ld+%ld=%ld loads, %ld invals, %ld lists, %ld lsrch, %ld lhits",
		 CacheHdr->ch_ntup,
		 cc_searches,
		 cc_hits,
		 cc_neg_hits,
		 cc_hits + cc_neg_hits,
		 cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits - cc_newloads,
		 cc_searches - cc_hits - cc_neg_hits,
		 cc_invals,
		 cc_nlists,
		 cc_lsearches,
		 cc_lhits);
}
#endif							/* CATCACHE_STATS */


/*
 *		CatCacheRemoveCTup
 *
 * 解链并删除给定缓存条目
 *
 * 注意：若它属于 CatCList，则该列表也会被删除。
 * 缓存条目与列表两者的引用计数最好都为零。
 */
static void
CatCacheRemoveCTup(CatCache *cache, CatCTup *ct)
{
	Assert(ct->refcount == 0);
	Assert(ct->my_cache == cache);

	if (ct->c_list)
	{
		/*
		 * 对此最干净的处理办法是调用 CatCacheRemoveCList，
		 * 它会递归回来，由递归调用完成实际工作。设置
		 * "死亡"标志以确保它确实会递归。
		 */
		ct->dead = true;
		CatCacheRemoveCList(cache, ct->c_list);
		return;					/* 无事可做 */
	}

	/* 从链表中解链 */
	dlist_delete(&ct->cache_elem);

	/*
	 * 处理负条目时须释放键；普通条目仅指向元组，
	 * 与 CatCTup 一同分配。
	 */
	if (ct->negative)
		CatCacheFreeKeys(cache->cc_tupdesc, cache->cc_nkeys,
						 cache->cc_keyno, ct->keys);

	pfree(ct);

	--cache->cc_ntup;
	--CacheHdr->ch_ntup;
}

/*
 *		CatCacheRemoveCList
 *
 * 解链并删除给定缓存列表条目
 *
 * 注意：不再被引用的死亡成员条目也会被删除。
 */
static void
CatCacheRemoveCList(CatCache *cache, CatCList *cl)
{
	int			i;

	Assert(cl->refcount == 0);
	Assert(cl->my_cache == cache);

	/* 从成员元组中解链 */
	for (i = cl->n_members; --i >= 0;)
	{
		CatCTup    *ct = cl->members[i];

		Assert(ct->c_list == cl);
		ct->c_list = NULL;
		/* 如果成员已死亡且不再有引用，则将其移除 */
		if (
#ifndef CATCACHE_FORCE_RELEASE
			ct->dead &&
#endif
			ct->refcount == 0)
			CatCacheRemoveCTup(cache, ct);
	}

	/* 从链表中解链 */
	dlist_delete(&cl->cache_elem);

	/* 释放相关的列数据 */
	CatCacheFreeKeys(cache->cc_tupdesc, cl->nkeys,
					 cache->cc_keyno, cl->keys);

	pfree(cl);

	--cache->cc_nlist;
}


/*
 *	CatCacheInvalidate
 *
 *	根据给定的哈希值使指定缓存中的条目失效。
 *
 *	删除匹配该哈希值的缓存条目，无论其是正条目
 *	还是负条目。我们并不关心失效是因元组插入还是
 *	删除所致。
 *
 *	过去曾尝试按 TID 匹配正条目，但对系统目录执行
 *	VACUUM FULL 之后并不安全：失效事件可能先于 VACUUM
 *	FULL 排队而在其后被处理，此时待失效的目标元组
 *	的 TID 已不同于事件创建时。故我们只比较哈希
 *	值，并接受因误匹配导致
 *	不必要失效的小风险。
 *
 *	此例程只是准公开的：只应由 inval.c 使用。
 */
void
CatCacheInvalidate(CatCache *cache, uint32 hashValue)
{
	Index		hashIndex;
	dlist_mutable_iter iter;

	CACHE_elog(DEBUG2, "CatCacheInvalidate: called");

	/*
	 * 无需费心检查缓存是否已完成初始化；若尚未完成，
	 * 其中不会有任何条目，因此不存在问题。
	 */

	/*
	 * 使缓存中*所有* CatCLists 一并失效；很难判断哪些
	 * 查找仍可能正确，索性全部清掉。
	 */
	for (int i = 0; i < cache->cc_nlbuckets; i++)
	{
		dlist_head *bucket = &cache->cc_lbucket[i];

		dlist_foreach_modify(iter, bucket)
		{
			CatCList   *cl = dlist_container(CatCList, cache_elem, iter.cur);

			if (cl->refcount > 0)
				cl->dead = true;
			else
				CatCacheRemoveCList(cache, cl);
		}
	}

	/*
	 * 在正确的哈希桶中检查元组匹配
	 */
	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);
	dlist_foreach_modify(iter, &cache->cc_bucket[hashIndex])
	{
		CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);

		if (hashValue == ct->hash_value)
		{
			if (ct->refcount > 0 ||
				(ct->c_list && ct->c_list->refcount > 0))
			{
				ct->dead = true;
				/* 列表（若有）被上方标死 */
				Assert(ct->c_list == NULL || ct->c_list->dead);
			}
			else
				CatCacheRemoveCTup(cache, ct);
			CACHE_elog(DEBUG2, "CatCacheInvalidate: invalidated");
#ifdef CATCACHE_STATS
			cache->cc_invals++;
#endif
			/* 可能有多个匹配，继续查找！ */
		}
	}

	/* 同时使所有正在构建中的条目失效 */
	for (CatCInProgress *e = catcache_in_progress_stack; e != NULL; e = e->next)
	{
		if (e->cache == cache)
		{
			if (e->list || e->hash_value == hashValue)
				e->dead = true;
		}
	}
}

/* ----------------------------------------------------------------
 *					   公共函数
 * ----------------------------------------------------------------
 */


/*
 * 若缓存上下文尚不存在，创建它的标准例程
 *
 * 有很多地方（可能远多于必要的数量）会检查
 * CacheMemoryContext 是否已存在，若无则想创建它。
 * 我们在此集中掌握创建它的确切知识。
 */
void
CreateCacheMemoryContext(void)
{
	/*
	 * 纯粹出于谨慎，检查上下文不存在；调用者多半已
	 * 检查过了。
	 */
	if (!CacheMemoryContext)
		CacheMemoryContext = AllocSetContextCreate(TopMemoryContext,
												   "CacheMemoryContext",
												   ALLOCSET_DEFAULT_SIZES);
}


/*
 *		ResetCatalogCache
 *
 * 将系统目录缓存清空。
 *
 * 若目标缓存近乎为空，这样做并不高效。
 * 但它无需高效；我们并不经常调用它。
 *
 * 若 'debug_discard' 为真，则我们是作为
 * debug_discard_caches 的一部分而被调用。此时重置
 * 缓存并非为了正确性，只是为了对缓存失效做更多
 * 测试。此时我们会跳过重置构建中的条目，否则将永无
 * 进展。
 */
static void
ResetCatalogCache(CatCache *cache, bool debug_discard)
{
	dlist_mutable_iter iter;
	int			i;

	/* 移除此缓存的每个列表，或至少标为死亡 */
	for (i = 0; i < cache->cc_nlbuckets; i++)
	{
		dlist_head *bucket = &cache->cc_lbucket[i];

		dlist_foreach_modify(iter, bucket)
		{
			CatCList   *cl = dlist_container(CatCList, cache_elem, iter.cur);

			if (cl->refcount > 0)
				cl->dead = true;
			else
				CatCacheRemoveCList(cache, cl);
		}
	}

	/* 移除此缓存的每个元组，或至少标为死亡 */
	for (i = 0; i < cache->cc_nbuckets; i++)
	{
		dlist_head *bucket = &cache->cc_bucket[i];

		dlist_foreach_modify(iter, bucket)
		{
			CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);

			if (ct->refcount > 0 ||
				(ct->c_list && ct->c_list->refcount > 0))
			{
				ct->dead = true;
				/* 列表（若有）被上方标死 */
				Assert(ct->c_list == NULL || ct->c_list->dead);
			}
			else
				CatCacheRemoveCTup(cache, ct);
#ifdef CATCACHE_STATS
			cache->cc_invals++;
#endif
		}
	}

	/* 同时使所有正在构建中的条目失效 */
	if (!debug_discard)
	{
		for (CatCInProgress *e = catcache_in_progress_stack; e != NULL; e = e->next)
		{
			if (e->cache == cache)
				e->dead = true;
		}
	}
}

/*
 *		ResetCatalogCaches
 *
 * 当共享缓存失效事件强制时重置所有缓存
 */
void
ResetCatalogCaches(void)
{
	ResetCatalogCachesExt(false);
}

void
ResetCatalogCachesExt(bool debug_discard)
{
	slist_iter	iter;

	CACHE_elog(DEBUG2, "ResetCatalogCaches called");

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		ResetCatalogCache(cache, debug_discard);
	}

	CACHE_elog(DEBUG2, "end of ResetCatalogCaches call");
}

/*
 *		CatalogCacheFlushCatalog
 *
 *	刷写来自指定系统目录的所有 catcache 条目。
 *	在对该目录 VACUUM FULL/CLUSTER 之后需要此操作，
 *	因为元组现在的 TID 很可能已不同于从前。（我们曾
 *	一度还尝试强制重新执行该目录上缓存（们）的
 *	CatalogCacheInitializeCache。这是个坏主意，因为在加载
 *	缓存条目时发生刷写会引来各种麻烦。现在我们通过
 *	把 cc_tupdesc 从 relcache 中复制出来，而不是依赖 relcache
 *	为我们保留 tupdesc。当然，这假设可缓存的系统表
 *	的 tupdesc 不会改变…）
 */
void
CatalogCacheFlushCatalog(Oid catId)
{
	slist_iter	iter;

	CACHE_elog(DEBUG2, "CatalogCacheFlushCatalog called for %u", catId);

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *cache = slist_container(CatCache, cc_next, iter.cur);

		/* 该缓存是否存储目标目录的元组？ */
		if (cache->cc_reloid == catId)
		{
			/* 是，刷写其全部内容 */
			ResetCatalogCache(cache, false);

			/* 通知 inval.c 为该缓存调用 syscache 回调 */
			CallSyscacheCallbacks(cache->id, 0);
		}
	}

	CACHE_elog(DEBUG2, "end of CatalogCacheFlushCatalog call");
}

/*
 *		InitCatCache
 *
 *	为一个系统目录关系分配并初始化缓存。实际上，
 *	缓存只做部分初始化以避免打开关系。关系的
 *	打开与缓存结构的其余会在
 *	首次访问时初始化。
 */
#ifdef CACHEDEBUG
#define InitCatCache_DEBUG2 \
do { \
	elog(DEBUG2, "InitCatCache: rel=%u ind=%u id=%d nkeys=%d size=%d", \
		 cp->cc_reloid, cp->cc_indexoid, cp->id, \
		 cp->cc_nkeys, cp->cc_nbuckets); \
} while(0)
#else
#define InitCatCache_DEBUG2
#endif

CatCache *
InitCatCache(int id,
			 Oid reloid,
			 Oid indexoid,
			 int nkeys,
			 const int *key,
			 int nbuckets)
{
	CatCache   *cp;
	MemoryContext oldcxt;
	int			i;

	/*
	 * nbuckets 是此 catcache 使用的初始哈希桶数量。如果之后
	 * 变得太满，将会被扩大。
	 *
	 * nbuckets 必须是 2 的幂。我们借 Assert 而非完整的运行
	 * 时检查来确认这一点，因为这些值将来自常量
	 * 表。
	 *
	 * 如果你对 2 的幂检查感到困惑，请参阅
	 * bitmapset.c 中的说明。
	 */
	Assert(nbuckets > 0 && (nbuckets & -nbuckets) == nbuckets);

	/*
	 * 先切换到缓存上下文，使我们的分配不会在
	 * 事务结束时消失
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 首次执行时，初始化缓存组头
	 */
	if (CacheHdr == NULL)
	{
		CacheHdr = (CatCacheHeader *) palloc(sizeof(CatCacheHeader));
		slist_init(&CacheHdr->ch_caches);
		CacheHdr->ch_ntup = 0;
#ifdef CATCACHE_STATS
		/* 设置在后端退出时转储统计 */
		on_proc_exit(CatCachePrintStats, 0);
#endif
	}

	/*
	 * 分配一个新的缓存结构，按缓存行边界对齐
	 *
	 * 注意：我们依靠清零来正确初始化所有 dlist 头部
	 */
	cp = (CatCache *) palloc_aligned(sizeof(CatCache), PG_CACHE_LINE_SIZE,
									 MCXT_ALLOC_ZERO);
	cp->cc_bucket = palloc0(nbuckets * sizeof(dlist_head));

	/*
	 * 许多 catcache 从不接收列表搜索。因此，我们要等到
	 * 有列表搜索时才分配 cc_lbuckets。
	 */
	cp->cc_lbucket = NULL;

	/*
	 * 为本缓存对应关系初始化缓存的关系信息，
	 * 并初始化新缓存的一些其他内部字段。但先不要
	 * 打开该关系。
	 */
	cp->id = id;
	cp->cc_relname = "(not known yet)";
	cp->cc_reloid = reloid;
	cp->cc_indexoid = indexoid;
	cp->cc_relisshared = false; /* 暂时 */
	cp->cc_tupdesc = (TupleDesc) NULL;
	cp->cc_ntup = 0;
	cp->cc_nlist = 0;
	cp->cc_nbuckets = nbuckets;
	cp->cc_nlbuckets = 0;
	cp->cc_nkeys = nkeys;
	for (i = 0; i < nkeys; ++i)
		cp->cc_keyno[i] = key[i];

	/*
	 * 新缓存已尽可能初始化完毕。如合适，打印一些
	 * 调试信息。
	 */
	InitCatCache_DEBUG2;

	/*
	 * 将完成的缓存加到组头列表的顶部
	 */
	slist_push_head(&CacheHdr->ch_caches, &cp->cc_next);

	/*
	 * 在返回之前回到旧上下文…
	 */
	MemoryContextSwitchTo(oldcxt);

	return cp;
}

/*
 * 扩大 catcache，使桶数量翻倍。
 */
static void
RehashCatCache(CatCache *cp)
{
	dlist_head *newbucket;
	int			newnbuckets;
	int			i;

	elog(DEBUG1, "rehashing catalog cache id %d for %s; %d tups, %d buckets",
		 cp->id, cp->cc_relname, cp->cc_ntup, cp->cc_nbuckets);

	/* 分配新的更大哈希表。 */
	newnbuckets = cp->cc_nbuckets * 2;
	newbucket = (dlist_head *) MemoryContextAllocZero(CacheMemoryContext, newnbuckets * sizeof(dlist_head));

	/* 把所有条目从旧表移到新表。 */
	for (i = 0; i < cp->cc_nbuckets; i++)
	{
		dlist_mutable_iter iter;

		dlist_foreach_modify(iter, &cp->cc_bucket[i])
		{
			CatCTup    *ct = dlist_container(CatCTup, cache_elem, iter.cur);
			int			hashIndex = HASH_INDEX(ct->hash_value, newnbuckets);

			dlist_delete(iter.cur);
			dlist_push_head(&newbucket[hashIndex], &ct->cache_elem);
		}
	}

	/* 切换到新数组。 */
	pfree(cp->cc_bucket);
	cp->cc_nbuckets = newnbuckets;
	cp->cc_bucket = newbucket;
}

/*
 * 扩大 catcache 的列表存储，使桶数量翻倍。
 */
static void
RehashCatCacheLists(CatCache *cp)
{
	dlist_head *newbucket;
	int			newnbuckets;
	int			i;

	elog(DEBUG1, "rehashing catalog cache id %d for %s; %d lists, %d buckets",
		 cp->id, cp->cc_relname, cp->cc_nlist, cp->cc_nlbuckets);

	/* 分配新的更大哈希表。 */
	newnbuckets = cp->cc_nlbuckets * 2;
	newbucket = (dlist_head *) MemoryContextAllocZero(CacheMemoryContext, newnbuckets * sizeof(dlist_head));

	/* 把所有条目从旧表移到新表。 */
	for (i = 0; i < cp->cc_nlbuckets; i++)
	{
		dlist_mutable_iter iter;

		dlist_foreach_modify(iter, &cp->cc_lbucket[i])
		{
			CatCList   *cl = dlist_container(CatCList, cache_elem, iter.cur);
			int			hashIndex = HASH_INDEX(cl->hash_value, newnbuckets);

			dlist_delete(iter.cur);
			dlist_push_head(&newbucket[hashIndex], &cl->cache_elem);
		}
	}

	/* 切换到新数组。 */
	pfree(cp->cc_lbucket);
	cp->cc_nlbuckets = newnbuckets;
	cp->cc_lbucket = newbucket;
}

/*
 *		CatalogCacheInitializeCache
 *
 * 此函数完成 catcache 的最终初始化：获取元组描述符并
 * 建立哈希与相等性函数链接。我们假定此时可以打开
 * relcache 条目！
 */
#ifdef CACHEDEBUG
#define CatalogCacheInitializeCache_DEBUG1 \
	elog(DEBUG2, "CatalogCacheInitializeCache: cache @%p rel=%u", cache, \
		 cache->cc_reloid)

#define CatalogCacheInitializeCache_DEBUG2 \
do { \
		if (cache->cc_keyno[i] > 0) { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d, %u", \
				i+1, cache->cc_nkeys, cache->cc_keyno[i], \
				 TupleDescAttr(tupdesc, cache->cc_keyno[i] - 1)->atttypid); \
		} else { \
			elog(DEBUG2, "CatalogCacheInitializeCache: load %d/%d w/%d", \
				i+1, cache->cc_nkeys, cache->cc_keyno[i]); \
		} \
} while(0)
#else
#define CatalogCacheInitializeCache_DEBUG1
#define CatalogCacheInitializeCache_DEBUG2
#endif

static void
CatalogCacheInitializeCache(CatCache *cache)
{
	Relation	relation;
	MemoryContext oldcxt;
	TupleDesc	tupdesc;
	int			i;

	CatalogCacheInitializeCache_DEBUG1;

	relation = table_open(cache->cc_reloid, AccessShareLock);

	/*
	 * 切换到缓存上下文，让我们的分配不会在事务
	 * 结束时消失
	 */
	Assert(CacheMemoryContext != NULL);

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 把 relcache 的元组描述符复制到永久缓存存储
	 */
	tupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	/*
	 * 同时保存关系名与 relisshared 标志（cc_relname 仅用于
	 * 调试目的）
	 */
	cache->cc_relname = pstrdup(RelationGetRelationName(relation));
	cache->cc_relisshared = RelationGetForm(relation)->relisshared;

	/*
	 * 返回调用者的内存上下文并关闭该关系
	 */
	MemoryContextSwitchTo(oldcxt);

	table_close(relation, AccessShareLock);

	CACHE_elog(DEBUG2, "CatalogCacheInitializeCache: %s, %d keys",
			   cache->cc_relname, cache->cc_nkeys);

	/*
	 * 初始化缓存的键信息
	 */
	for (i = 0; i < cache->cc_nkeys; ++i)
	{
		Oid			keytype;
		RegProcedure eqfunc;

		CatalogCacheInitializeCache_DEBUG2;

		if (cache->cc_keyno[i] > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc,
												   cache->cc_keyno[i] - 1);

			keytype = attr->atttypid;
			/* 缓存键列应始终为 NOT NULL */
			Assert(attr->attnotnull);
		}
		else
		{
			if (cache->cc_keyno[i] < 0)
				elog(FATAL, "sys attributes are not supported in caches");
			keytype = OIDOID;
		}

		GetCCHashEqFuncs(keytype,
						 &cache->cc_hashfunc[i],
						 &eqfunc,
						 &cache->cc_fastequal[i]);

		/*
		 * 执行相等性函数查找（假定对任何受支持的类型
		 * 无需目录查找）
		 */
		fmgr_info_cxt(eqfunc,
					  &cache->cc_skey[i].sk_func,
					  CacheMemoryContext);

		/* 为 HeapKeyTest() 与堆扫描适当地初始化 sk_attno */
		cache->cc_skey[i].sk_attno = cache->cc_keyno[i];

		/* 同时填入 sk_strategy --- 始终为标准相等性 */
		cache->cc_skey[i].sk_strategy = BTEqualStrategyNumber;
		cache->cc_skey[i].sk_subtype = InvalidOid;
		/* 若 catcache 键需排序规则，必须是 C 排序规则 */
		cache->cc_skey[i].sk_collation = C_COLLATION_OID;

		CACHE_elog(DEBUG2, "CatalogCacheInitializeCache %s %d %p",
				   cache->cc_relname, i, cache);
	}

	/*
	 * 标记此缓存已完全初始化
	 */
	cache->cc_tupdesc = tupdesc;
}

/*
 * InitCatCachePhase2 -- CatalogCacheInitializeCache 的外部接口
 *
 * 调用本例程的原因之一是确保 relcache 已为 catcache
 * 所引用的所有目录和索引都创建了条目。因此提供一个
 * 选项，在修复缓存之余也打开索引。例外是 pg_am
 * 上的索引，我们并不使用它们（参见
 * IndexScanOK）。
 */
void
InitCatCachePhase2(CatCache *cache, bool touch_index)
{
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	if (touch_index &&
		cache->id != AMOID &&
		cache->id != AMNAME)
	{
		Relation	idesc;

		/*
		 * 打开索引前必须先锁底层目录以避免死锁，
		 * 因为 index_open 可能导致读取这个相同的目录，
		 * 而若他人正以独占方式锁定该目录与索引，
		 * 他们会按那个顺序进行。
		 */
		LockRelationOid(cache->cc_reloid, AccessShareLock);
		idesc = index_open(cache->cc_indexoid, AccessShareLock);

		/*
		 * 趁索引开着，顺便检查它是否唯一（而且绝不仅
		 * 是可延迟唯一约束，多谢关照）。这只是
		 * 为了捕捉新 catcache 定义中的粗心错误，所以不
		 * 必担心 pg_am 索引未受检验。
		 */
		Assert(idesc->rd_index->indisunique &&
			   idesc->rd_index->indimmediate);

		index_close(idesc, AccessShareLock);
		UnlockRelationOid(cache->cc_reloid, AccessShareLock);
	}
}


/*
 *		IndexScanOK
 *
 *		本函数检查将由 IndexSupportInitialize()
 *		在 relcache 初始化期间为支撑关键 syscache 的
 *		系统索引所取的元组。不能用索引扫描，
 *		否则会陷入无限递归。普通堆扫描
 *		却可行。
 *		完成 relcache 初始化后（以 criticalRelcachesBuilt
 *		为标志），就无需再担心了。
 *
 *		类似地，在后端启动期间，即使还没有这些目录
 *		索引 relcache 条目，也须能使用 pg_authid、
 *		pg_auth_members 与 pg_database syscache 来
 *		做认证。
 */
static bool
IndexScanOK(CatCache *cache, ScanKey cur_skey)
{
	switch (cache->id)
	{
		case INDEXRELID:

			/*
			 * 与其跟踪哪些索引必须先加载，
			 * 才能使用索引扫描（它不时变化），我们干脆
			 * 强制所有 pg_index 搜索都走堆扫描，直到建好
			 * 关键 relcache。
			 */
			if (!criticalRelcachesBuilt)
				return false;
			break;

		case AMOID:
		case AMNAME:

			/*
			 * pg_am 中始终做堆扫描，它太小，索引扫描意义
			 * 不大。构建关键 relcache 条目之初*必须*这样
			 * 做，但我们不妨干脆在所有情况下都这样
			 * 做。
			 */
			return false;

		case AUTHNAME:
		case AUTHOID:
		case AUTHMEMMEMROLE:
		case DATABASEOID:

			/*
			 * 保护 relcache 收集共享索引条目之前的认证
			 * 查找。
			 */
			if (!criticalSharedRelcachesBuilt)
				return false;
			break;

		default:
			break;
	}

	/* 常规：允许索引扫描 */
	return true;
}

/*
 *	SearchCatCache
 *
 *		此调用在系统缓存中搜索元组，必要时打开关系
 *		（在首次访问某特定缓存时）。
 *
 *		若未找到则结果为 NULL；否则为指向缓存中
 *		HeapTuple 指针。调用者不得修改该元组，用完时
 *		必须调用 ReleaseCatCache()。
 *
 * 搜索键值应以键列数据类型的 Datum 形式表达。
 * （用不到的参数传零即可。）作为特例，NAME 列
 * 传入的键可以只是 C 字符串；调用者无需费事把它
 * 转换为完全以 null 填充的
 * NAME。
 */
HeapTuple
SearchCatCache(CatCache *cache,
			   Datum v1,
			   Datum v2,
			   Datum v3,
			   Datum v4)
{
	return SearchCatCacheInternal(cache, cache->cc_nkeys, v1, v2, v3, v4);
}


/*
 * SearchCatCacheN() 是 SearchCatCache() 针对特定参数个数
 * 的版本。编译器可以内联函数体并展开循环，使其比
 * SearchCatCache() 快一些。
 */

HeapTuple
SearchCatCache1(CatCache *cache,
				Datum v1)
{
	return SearchCatCacheInternal(cache, 1, v1, 0, 0, 0);
}


HeapTuple
SearchCatCache2(CatCache *cache,
				Datum v1, Datum v2)
{
	return SearchCatCacheInternal(cache, 2, v1, v2, 0, 0);
}


HeapTuple
SearchCatCache3(CatCache *cache,
				Datum v1, Datum v2, Datum v3)
{
	return SearchCatCacheInternal(cache, 3, v1, v2, v3, 0);
}


HeapTuple
SearchCatCache4(CatCache *cache,
				Datum v1, Datum v2, Datum v3, Datum v4)
{
	return SearchCatCacheInternal(cache, 4, v1, v2, v3, v4);
}

/*
 * SearchCatCache/SearchCatCacheN 的主力。
 */
static inline HeapTuple
SearchCatCacheInternal(CatCache *cache,
					   int nkeys,
					   Datum v1,
					   Datum v2,
					   Datum v3,
					   Datum v4)
{
	Datum		arguments[CATCACHE_MAXKEYS];
	uint32		hashValue;
	Index		hashIndex;
	dlist_iter	iter;
	dlist_head *bucket;
	CatCTup    *ct;

	/* 确保我们处于事务中，即使这最终是一次缓存命中 */
	Assert(IsTransactionState());

	Assert(cache->cc_nkeys == nkeys);

	/*
	 * 每个缓存的一次性启动开销
	 */
	if (unlikely(cache->cc_tupdesc == NULL))
		CatalogCacheInitializeCache(cache);

#ifdef CATCACHE_STATS
	cache->cc_searches++;
#endif

	/* 初始化局部参数数组 */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * 找出要在其中查找元组的哈希桶
	 */
	hashValue = CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);
	hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

	/*
	 * 扫描哈希桶，直到找到匹配或穷尽我们的元组
	 *
	 * 注意：在这里使用 dlist_foreach 没问题，即使我们
	 * 会在循环中修改 dlist，因为之后不再继续循环。
	 */
	bucket = &cache->cc_bucket[hashIndex];
	dlist_foreach(iter, bucket)
	{
		ct = dlist_container(CatCTup, cache_elem, iter.cur);

		if (ct->dead)
			continue;			/* 忽略死亡条目 */

		if (ct->hash_value != hashValue)
			continue;			/* 哈希值不符时快速跳过条目 */

		if (!CatalogCacheCompareTuple(cache, nkeys, ct->keys, arguments))
			continue;

		/*
		 * 我们在缓存中找到匹配。将其移至其哈希桶列表
		 * 的头部，以加速后续搜索。（每个哈希桶中最
		 * 常被访问的元素往往靠近该桶
		 * 列表的前端。）
		 */
		dlist_move_head(bucket, &ct->cache_elem);

		/*
		 * 若它是正条目，则增加其引用计数并返回。若
		 * 是负条目，可向调用者报告失败。
		 */
		if (!ct->negative)
		{
			ResourceOwnerEnlargeCatCacheRefs(CurrentResourceOwner);
			ct->refcount++;
			ResourceOwnerRememberCatCacheRef(CurrentResourceOwner, &ct->tuple);

			CACHE_elog(DEBUG2, "SearchCatCache(%s): found in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_hits++;
#endif

			return &ct->tuple;
		}
		else
		{
			CACHE_elog(DEBUG2, "SearchCatCache(%s): found neg entry in bucket %d",
					   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
			cache->cc_neg_hits++;
#endif

			return NULL;
		}
	}

	return SearchCatCacheMiss(cache, nkeys, hashValue, hashIndex, v1, v2, v3, v4);
}

/*
 * 搜索实际目录，而非缓存。
 *
 * 这一部分与 SearchCatCacheInternal() 分开，以保持快
 * 速路径尽可能小。为避免这份努力被好心的编译器
 * 抵消，我们尽量显式禁止内联。
 */
static pg_noinline HeapTuple
SearchCatCacheMiss(CatCache *cache,
				   int nkeys,
				   uint32 hashValue,
				   Index hashIndex,
				   Datum v1,
				   Datum v2,
				   Datum v3,
				   Datum v4)
{
	ScanKeyData cur_skey[CATCACHE_MAXKEYS];
	Relation	relation;
	SysScanDesc scandesc;
	HeapTuple	ntp;
	CatCTup    *ct;
	bool		stale;
	Datum		arguments[CATCACHE_MAXKEYS];

	/* 初始化局部参数数组 */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * 缓存中未找到元组，因此必须尝试直接从关系中取
	 * 回它。找到则加入缓存；未找到则改为添加负缓
	 * 存条目。
	 *
	 * 注意：读关系期间可能发生递归的缓存查找 ---
	 * 例如 table_open() 期间处理共享缓存失效消息所致。
	 * 这是可以的。甚至可能其中一个查找恰好找到并
	 * 录入我们正要在此获取的那个同一元组。若发生
	 * 这种情况，我们会把该元组的第二副本录入缓存。
	 * 第一份副本永不再被引用，终将老化离开缓存，
	 * 因此没有功能性问题。这种情况足够罕见，不值
	 * 得花额外周期去探
	 * 测。
	 *
	 * 另一种我们*必须*处理的情况是：元组可能在
	 * CatalogCacheCreateEntry 尝试解除 toast 时变得过时
	 * （因为 TOAST 表访问期间 AcceptInvalidationMessages 可
	 * 能运行）。我们不想返回已过时的 catcache 条目，故
	 * 若如此则回头重做表扫描。
	 */
	relation = table_open(cache->cc_reloid, AccessShareLock);

	do
	{
		/*
		 * 好，需要在关系中做查找，复制 scankey 并填写
		 * 所有每次调用的字段。（重试时须重做这步，因
		 * 为 systable_beginscan 会涂改 scankey。）
		 */
		memcpy(cur_skey, cache->cc_skey, sizeof(ScanKeyData) * nkeys);
		cur_skey[0].sk_argument = v1;
		cur_skey[1].sk_argument = v2;
		cur_skey[2].sk_argument = v3;
		cur_skey[3].sk_argument = v4;

		scandesc = systable_beginscan(relation,
									  cache->cc_indexoid,
									  IndexScanOK(cache, cur_skey),
									  NULL,
									  nkeys,
									  cur_skey);

		ct = NULL;
		stale = false;

		while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
		{
			ct = CatalogCacheCreateEntry(cache, ntp, NULL,
										 hashValue, hashIndex);
			/* 若失败，必须重新开始扫描 */
			if (ct == NULL)
			{
				stale = true;
				break;
			}
			/* 立即把引用计数设为 1 */
			ResourceOwnerEnlargeCatCacheRefs(CurrentResourceOwner);
			ct->refcount++;
			ResourceOwnerRememberCatCacheRef(CurrentResourceOwner, &ct->tuple);
			break;				/* 假定仅一个匹配 */
		}

		systable_endscan(scandesc);
	} while (stale);

	table_close(relation, AccessShareLock);

	/*
	 * 若未找到元组，则需构建负缓存条目，其中包含
	 * 伪元组。伪元组具有正确的键列，
	 * 其余均为空值。
	 *
	 * 在引导模式下，我们不构建负条目，因为缓存失效
	 * 机制未存活，无法在元组稍后创建时清除它们。
	 * （引导模式不做 UPDATE，因此为此它不需要缓存
	 * 失效。）
	 */
	if (ct == NULL)
	{
		if (IsBootstrapProcessingMode())
			return NULL;

		ct = CatalogCacheCreateEntry(cache, NULL, arguments,
									 hashValue, hashIndex);

		/* 创建负缓存条目不应失败 */
		Assert(ct != NULL);

		CACHE_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
				   cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
		CACHE_elog(DEBUG2, "SearchCatCache(%s): put neg entry in bucket %d",
				   cache->cc_relname, hashIndex);

		/*
		 * 我们不会把负条目返回给调用者，因此其引用计数
		 * 置零。
		 */

		return NULL;
	}

	CACHE_elog(DEBUG2, "SearchCatCache(%s): Contains %d/%d tuples",
			   cache->cc_relname, cache->cc_ntup, CacheHdr->ch_ntup);
	CACHE_elog(DEBUG2, "SearchCatCache(%s): put in bucket %d",
			   cache->cc_relname, hashIndex);

#ifdef CATCACHE_STATS
	cache->cc_newloads++;
#endif

	return &ct->tuple;
}

/*
 *	ReleaseCatCache
 *
 *	递减 catcache 条目的引用计数（释放成功的
 *	SearchCatCache 所获取的持有）。
 *
 *	注意：若以 -DCATCACHE_FORCE_RELEASE 编译，则 catcache
 *	条目将在引用计数降至零时立即释放。结合 aset.c 的
 *	CLOBBER_FREED_MEMORY 选项，这提供了一个很好的测
 *	试，捕捉对已释放 catcache 条目的引用。
 */
void
ReleaseCatCache(HeapTuple tuple)
{
	CatCTup    *ct = (CatCTup *) (((char *) tuple) -
								  offsetof(CatCTup, tuple));

	/* 安全检查，确保拿到的是缓存条目 */
	Assert(ct->ct_magic == CT_MAGIC);
	Assert(ct->refcount > 0);

	ct->refcount--;
	ResourceOwnerForgetCatCacheRef(CurrentResourceOwner, &ct->tuple);

	if (
#ifndef CATCACHE_FORCE_RELEASE
		ct->dead &&
#endif
		ct->refcount == 0 &&
		(ct->c_list == NULL || ct->c_list->refcount == 0))
		CatCacheRemoveCTup(ct->my_cache, ct);
}


/*
 *	GetCatCacheHashValue
 *
 *		为一组给定的搜索键计算哈希值。
 *
 * 将其作为 API 一部分公开的原因是：哈希值会在缓存
 * 失效操作中被公开，所以 catcache 代码之外也有地方
 * 需要能够计算哈希值。
 */
uint32
GetCatCacheHashValue(CatCache *cache,
					 Datum v1,
					 Datum v2,
					 Datum v3,
					 Datum v4)
{
	/*
	 * 每个缓存的一次性启动开销
	 */
	if (cache->cc_tupdesc == NULL)
		CatalogCacheInitializeCache(cache);

	/*
	 * 计算哈希值
	 */
	return CatalogCacheComputeHashValue(cache, cache->cc_nkeys, v1, v2, v3, v4);
}


/*
 *	SearchCatCacheList
 *
 *		生成所有匹配部分键的元组的列表（即只指定
 *		缓存 N 个键列中的前 K 个的键）。
 *
 *		在此指定全部键列毫无意义：因为键是唯一的，
 *		最多只有一个匹配，所以应当改用 SearchCatCache()。
 *		故本函数比 SearchCatCache() 少一个
 *		Datum 实参。
 *
 *		调用者不得修改列表对象或其指向元组，并且用完
 *		列表后必须调用 ReleaseCatCacheList()。
 */
CatCList *
SearchCatCacheList(CatCache *cache,
				   int nkeys,
				   Datum v1,
				   Datum v2,
				   Datum v3)
{
	Datum		v4 = 0;			/* 末列占位值 */
	Datum		arguments[CATCACHE_MAXKEYS];
	uint32		lHashValue;
	Index		lHashIndex;
	dlist_iter	iter;
	dlist_head *lbucket;
	CatCList   *cl;
	CatCTup    *ct;
	List	   *volatile ctlist;
	ListCell   *ctlist_item;
	int			nmembers;
	bool		ordered;
	HeapTuple	ntp;
	MemoryContext oldcxt;
	int			i;
	CatCInProgress *save_in_progress;
	CatCInProgress in_progress_ent;

	/*
	 * 每个缓存的一次性启动开销
	 */
	if (unlikely(cache->cc_tupdesc == NULL))
		CatalogCacheInitializeCache(cache);

	Assert(nkeys > 0 && nkeys < cache->cc_nkeys);

#ifdef CATCACHE_STATS
	cache->cc_lsearches++;
#endif

	/* 初始化局部参数数组 */
	arguments[0] = v1;
	arguments[1] = v2;
	arguments[2] = v3;
	arguments[3] = v4;

	/*
	 * 如果此前本缓存未做过列表搜索，则创建桶头部
	 * 数组；否则考虑是否该扩大它。
	 * it.
	 */
	if (cache->cc_lbucket == NULL)
	{
		/* 任意的初始大小 --- 必须是 2 的幂 */
		int			nbuckets = 16;

		cache->cc_lbucket = (dlist_head *)
			MemoryContextAllocZero(CacheMemoryContext,
								   nbuckets * sizeof(dlist_head));
		/* 若分配 cc_lbucket 内存耗尽，勿设置 cc_nlbuckets */
		cache->cc_nlbuckets = nbuckets;
	}
	else
	{
		/*
		 * 若哈希表太满，则扩大桶数组。我们相当随意地
		 * 在填充因子 > 2 时扩大。
		 */
		if (cache->cc_nlist > cache->cc_nlbuckets * 2)
			RehashCatCacheLists(cache);
	}

	/*
	 * 找出要在其中查找 CatCList 的哈希桶。
	 */
	lHashValue = CatalogCacheComputeHashValue(cache, nkeys, v1, v2, v3, v4);
	lHashIndex = HASH_INDEX(lHashValue, cache->cc_nlbuckets);

	/*
	 * 扫描各条目，直到找到匹配或穷尽列表
	 *
	 * 注意：在这里使用 dlist_foreach 没问题，即使我们
	 * 会在循环中修改 dlist，因为之后不再继续循环。
	 */
	lbucket = &cache->cc_lbucket[lHashIndex];
	dlist_foreach(iter, lbucket)
	{
		cl = dlist_container(CatCList, cache_elem, iter.cur);

		if (cl->dead)
			continue;			/* 忽略死亡条目 */

		if (cl->hash_value != lHashValue)
			continue;			/* 哈希值不符时快速跳过条目 */

		/*
		 * 看缓存的列表是否与键匹配。
		 */
		if (cl->nkeys != nkeys)
			continue;

		if (!CatalogCacheCompareTuple(cache, nkeys, cl->keys, arguments))
			continue;

		/*
		 * 我们找到了匹配列表。把该列表移到其哈希桶列
		 * 表的头部，以加速后续搜索。（但是我们并不把成
		 * 员移到其哈希桶列表头部，因为除非它们被单独
		 * 搜索，否则那样做毫无意
		 * 义。）
		 */
		dlist_move_head(lbucket, &cl->cache_elem);

		/* 增加列表引用计数并返回 */
		ResourceOwnerEnlargeCatCacheListRefs(CurrentResourceOwner);
		cl->refcount++;
		ResourceOwnerRememberCatCacheListRef(CurrentResourceOwner, cl);

		CACHE_elog(DEBUG2, "SearchCatCacheList(%s): found list",
				   cache->cc_relname);

#ifdef CATCACHE_STATS
		cache->cc_lhits++;
#endif

		return cl;
	}

	/*
	 * 缓存中未找到该列表，必须读关系来构建它。对
	 * 于关系中找到的每个匹配元组，尽量使用
	 * 现有缓存条目，否则构建新条目。
	 *
	 * 我们必须临时增加成员的引用计数，以确保在加载
	 * 其他成员时它们不会被从缓存中丢弃。我们使用
	 * PG_TRY 块来确保：如果在完成构建 CatCList 之前发生
	 * 错误，可以撤销那些引用计数。ctlist 必须在整个
	 * 块内有效。
	 */
	ResourceOwnerEnlargeCatCacheListRefs(CurrentResourceOwner);

	ctlist = NIL;

	/*
	 * 构建列表期间可能发生缓存失效。
	 * CatalogCacheCreateEntry() 会处理单个元组的并发失效，
	 * 但也可能并发加入一个应属于我们正在构建列表的
	 * 新条目。注册一个"进行中"条目接收该失效，
	 * 直到我们构建出最终的列表
	 * 条目为止。
	 */
	save_in_progress = catcache_in_progress_stack;
	in_progress_ent.next = catcache_in_progress_stack;
	in_progress_ent.cache = cache;
	in_progress_ent.hash_value = lHashValue;
	in_progress_ent.list = true;
	in_progress_ent.dead = false;
	catcache_in_progress_stack = &in_progress_ent;

	PG_TRY();
	{
		ScanKeyData cur_skey[CATCACHE_MAXKEYS];
		Relation	relation;
		SysScanDesc scandesc;

		relation = table_open(cache->cc_reloid, AccessShareLock);

		/*
		 * 在表中扫描匹配条目。若构建中途到达失效，就
		 * 会循环回到这里重试。
		 */
		do
		{
			/*
			 * 若正在重试，则对上一轮迭代中创建的条目释
			 * 放引用计数。若它们已无引用，也不敢
			 * 尝试释放它们，因为这样做时出错会导致下面
			 * 的 PG_CATCH 额外递减引用计数。况且下轮迭代
			 * 中我们很可能重新收养它
			 * 们，所以不值得费心去摆脱
			 * 它们。
			 */
			foreach(ctlist_item, ctlist)
			{
				ct = (CatCTup *) lfirst(ctlist_item);
				Assert(ct->c_list == NULL);
				Assert(ct->refcount > 0);
				ct->refcount--;
			}
			/* 为新一次尝试而重置 ctlist */
			ctlist = NIL;
			in_progress_ent.dead = false;

			/*
			 * 复制 scankey 并填好每次调用字段。（重试时
			 * 必须重做这一步，因为 systable_beginscan 会涂改
			 * scankey。）
			 */
			memcpy(cur_skey, cache->cc_skey, sizeof(ScanKeyData) * cache->cc_nkeys);
			cur_skey[0].sk_argument = v1;
			cur_skey[1].sk_argument = v2;
			cur_skey[2].sk_argument = v3;
			cur_skey[3].sk_argument = v4;

			scandesc = systable_beginscan(relation,
										  cache->cc_indexoid,
										  IndexScanOK(cache, cur_skey),
										  NULL,
										  nkeys,
										  cur_skey);

			/* 列表有序当且仅当我们正在做索引扫描 */
			ordered = (scandesc->irel != NULL);

			while (HeapTupleIsValid(ntp = systable_getnext(scandesc)) &&
				   !in_progress_ent.dead)
			{
				uint32		hashValue;
				Index		hashIndex;
				bool		found = false;
				dlist_head *bucket;

				/*
				 * 看看该元组是否已有对应条目。
				 */
				ct = NULL;
				hashValue = CatalogCacheComputeTupleHashValue(cache, cache->cc_nkeys, ntp);
				hashIndex = HASH_INDEX(hashValue, cache->cc_nbuckets);

				bucket = &cache->cc_bucket[hashIndex];
				dlist_foreach(iter, bucket)
				{
					ct = dlist_container(CatCTup, cache_elem, iter.cur);

					if (ct->dead || ct->negative)
						continue;	/* 忽略死亡与负条目 */

					if (ct->hash_value != hashValue)
						continue;	/* 哈希值不符时快速跳过条目 */

					if (!ItemPointerEquals(&(ct->tuple.t_self), &(ntp->t_self)))
						continue;	/* 元组不同 */

					/*
					 * 找到匹配，但若其已属其他列表则不
					 * 能使用它
					 */
					if (ct->c_list)
						continue;

					found = true;
					break;		/* A-OK */
				}

				if (!found)
				{
					/* 未找到可用条目，因此新建一个 */
					ct = CatalogCacheCreateEntry(cache, ntp, NULL,
												 hashValue, hashIndex);

					/* 若失败，必须重新开始扫描 */
					if (ct == NULL)
					{
						in_progress_ent.dead = true;
						break;
					}
				}

				/* 小心：先把条目加入 ctlist 再增其引用计数 */
				/* 这样若 lappend 内存耗尽也能保持状态正确 */
				ctlist = lappend(ctlist, ct);
				ct->refcount++;
			}

			systable_endscan(scandesc);
		} while (in_progress_ent.dead);

		table_close(relation, AccessShareLock);

		/* 现在可构建 CatCList 条目。 */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		nmembers = list_length(ctlist);
		cl = (CatCList *)
			palloc(offsetof(CatCList, members) + nmembers * sizeof(CatCTup *));

		/* 抽取键值 */
		CatCacheCopyKeys(cache->cc_tupdesc, nkeys, cache->cc_keyno,
						 arguments, cl->keys);
		MemoryContextSwitchTo(oldcxt);

		/*
		 * 现在已越过了最后一个可能在完成 CatCList 构建并
		 * 在资源所有者登记之前触发 elog 的环节。因此
		 * 落出 PG_TRY 是可以的，而且我们最好在开始标记成
		 * 员属于该列表之前就
		 * 这么做。
		 */
	}
	PG_CATCH();
	{
		Assert(catcache_in_progress_stack == &in_progress_ent);
		catcache_in_progress_stack = save_in_progress;

		foreach(ctlist_item, ctlist)
		{
			ct = (CatCTup *) lfirst(ctlist_item);
			Assert(ct->c_list == NULL);
			Assert(ct->refcount > 0);
			ct->refcount--;
			if (
#ifndef CATCACHE_FORCE_RELEASE
				ct->dead &&
#endif
				ct->refcount == 0 &&
				(ct->c_list == NULL || ct->c_list->refcount == 0))
				CatCacheRemoveCTup(cache, ct);
		}

		PG_RE_THROW();
	}
	PG_END_TRY();
	Assert(catcache_in_progress_stack == &in_progress_ent);
	catcache_in_progress_stack = save_in_progress;

	cl->cl_magic = CL_MAGIC;
	cl->my_cache = cache;
	cl->refcount = 0;			/* 暂时的 */
	cl->dead = false;
	cl->ordered = ordered;
	cl->nkeys = nkeys;
	cl->hash_value = lHashValue;
	cl->n_members = nmembers;

	i = 0;
	foreach(ctlist_item, ctlist)
	{
		cl->members[i++] = ct = (CatCTup *) lfirst(ctlist_item);
		Assert(ct->c_list == NULL);
		ct->c_list = cl;
		/* 释放成员上的临时引用计数 */
		Assert(ct->refcount > 0);
		ct->refcount--;
		/* 若有成员已死则将列表标为死亡 */
		if (ct->dead)
			cl->dead = true;
	}
	Assert(i == nmembers);

	/*
	 * 把 CatCList 加入相应的桶，并对其计数。
	 */
	dlist_push_head(lbucket, &cl->cache_elem);

	cache->cc_nlist++;

	/* 最后增加列表引用计数并返回 */
	cl->refcount++;
	ResourceOwnerRememberCatCacheListRef(CurrentResourceOwner, cl);

	CACHE_elog(DEBUG2, "SearchCatCacheList(%s): made list of %d members",
			   cache->cc_relname, nmembers);

	return cl;
}

/*
 *	ReleaseCatCacheList
 *
 *	递减 catcache 列表的引用计数。
 */
void
ReleaseCatCacheList(CatCList *list)
{
	/* 安全检查，确保拿到的是缓存条目 */
	Assert(list->cl_magic == CL_MAGIC);
	Assert(list->refcount > 0);
	list->refcount--;
	ResourceOwnerForgetCatCacheListRef(CurrentResourceOwner, list);

	if (
#ifndef CATCACHE_FORCE_RELEASE
		list->dead &&
#endif
		list->refcount == 0)
		CatCacheRemoveCList(list->my_cache, list);
}


/*
 * CatalogCacheCreateEntry
 *		创建新 CatCTup 条目，把给定 HeapTuple 和其他数据
 *		复制进去。新条目初始引用计数为 0。
 *
 * 要创建普通缓存条目，ntp 必须是刚从 scandesc 获取的
 * HeapTuple，且不使用"arguments"。要创建负缓存条目，
 * 则为 ntp 传 NULL；"arguments"即要使用的缓存键。
 * 两种情况下，hashValue/hashIndex 都是由缓存键计算
 * 出来的。
 *
 * 若我们尝试对元组解除 toast 并观察到其已过时，
 * 则返回 NULL。（负条目不可能发生这种情况。）此时
 * 调用者必须重试元组查找。
 */
static CatCTup *
CatalogCacheCreateEntry(CatCache *cache, HeapTuple ntp, Datum *arguments,
						uint32 hashValue, Index hashIndex)
{
	CatCTup    *ct;
	MemoryContext oldcxt;

	if (ntp)
	{
		int			i;
		HeapTuple	dtp = NULL;

		/*
		 * 进行中条目的失效在回归测试中几乎从不发生，
		 * 也没有为测试目的而强制其失败的简易办法。
		 * 为了确保调用者的重试路径有测试覆盖，让调试
		 * 构建在经过此代码路径时随机失败约 0.1%，即
		 * 使其中没有任何行外 toast
		 * 字段。
		 */
#ifdef USE_ASSERT_CHECKING
		if (pg_prng_uint32(&pg_global_prng_state) <= (PG_UINT32_MAX / 1000))
			return NULL;
#endif

		/*
		 * 若元组有行外 toast 字段，则将其就地展
		 * 开。这为 catcache 条目后续使用省下周期，也保护
		 * 我们免于 toast 元组在我们尝试获取它们之前被
		 * 释放的风险，以防某处使用了稍过时的
		 * catcache 条目。
		 */
		if (HeapTupleHasExternal(ntp))
		{
			CatCInProgress *save_in_progress;
			CatCInProgress in_progress_ent;

			/*
			 * 访问 toast 表时元组可能过时（因为那时可能
			 * 运行 AcceptInvalidationMessages）。该失效会
			 * 把我们的进行中条目标记为死亡。
			 */
			save_in_progress = catcache_in_progress_stack;
			in_progress_ent.next = catcache_in_progress_stack;
			in_progress_ent.cache = cache;
			in_progress_ent.hash_value = hashValue;
			in_progress_ent.list = false;
			in_progress_ent.dead = false;
			catcache_in_progress_stack = &in_progress_ent;

			PG_TRY();
			{
				dtp = toast_flatten_tuple(ntp, cache->cc_tupdesc);
			}
			PG_FINALLY();
			{
				Assert(catcache_in_progress_stack == &in_progress_ent);
				catcache_in_progress_stack = save_in_progress;
			}
			PG_END_TRY();

			if (in_progress_ent.dead)
			{
				heap_freetuple(dtp);
				return NULL;
			}
		}
		else
			dtp = ntp;

		/* 一次性为 CatCTup 与被缓存元组分配内存 */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		ct = (CatCTup *) palloc(sizeof(CatCTup) +
								MAXIMUM_ALIGNOF + dtp->t_len);
		ct->tuple.t_len = dtp->t_len;
		ct->tuple.t_self = dtp->t_self;
		ct->tuple.t_tableOid = dtp->t_tableOid;
		ct->tuple.t_data = (HeapTupleHeader)
			MAXALIGN(((char *) ct) + sizeof(CatCTup));
		/* 复制元组内容 */
		memcpy((char *) ct->tuple.t_data,
			   (const char *) dtp->t_data,
			   dtp->t_len);
		MemoryContextSwitchTo(oldcxt);

		if (dtp != ntp)
			heap_freetuple(dtp);

		/* 抽取键 --- 非按值传递时它们指向元组内部 */
		for (i = 0; i < cache->cc_nkeys; i++)
		{
			Datum		atp;
			bool		isnull;

			atp = heap_getattr(&ct->tuple,
							   cache->cc_keyno[i],
							   cache->cc_tupdesc,
							   &isnull);
			Assert(!isnull);
			ct->keys[i] = atp;
		}
	}
	else
	{
		/* 为负缓存条目设置键 */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		ct = (CatCTup *) palloc(sizeof(CatCTup));

		/*
		 * 存储键 --- 若非按值传递，它们将指向单独分配的
		 * 内存。
		 */
		CatCacheCopyKeys(cache->cc_tupdesc, cache->cc_nkeys, cache->cc_keyno,
						 arguments, ct->keys);
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 完成 CatCTup 头部初始化，并将其加入缓存链表
	 * 和计数中。
	 */
	ct->ct_magic = CT_MAGIC;
	ct->my_cache = cache;
	ct->c_list = NULL;
	ct->refcount = 0;			/* 暂时的 */
	ct->dead = false;
	ct->negative = (ntp == NULL);
	ct->hash_value = hashValue;

	dlist_push_head(&cache->cc_bucket[hashIndex], &ct->cache_elem);

	cache->cc_ntup++;
	CacheHdr->ch_ntup++;

	/*
	 * 若哈希表已变得太满，则扩大桶数组。我们相当随
	 * 意地在填充因子 > 2 时扩大。
	 */
	if (cache->cc_ntup > cache->cc_nbuckets * 2)
		RehashCatCache(cache);

	return ct;
}

/*
 * 释放 keys 数组中所存键的辅助例程。
 */
static void
CatCacheFreeKeys(TupleDesc tupdesc, int nkeys, int *attnos, Datum *keys)
{
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		int			attnum = attnos[i];
		Form_pg_attribute att;

		/* 缓存不支持系统属性 */
		Assert(attnum > 0);

		att = TupleDescAttr(tupdesc, attnum - 1);

		if (!att->attbyval)
			pfree(DatumGetPointer(keys[i]));
	}
}

/*
 * 把 srckeys 数组中的键复制到 dstkeys 数组的辅助例程，
 * 并保证这些 datum 在当前内存上下文中被完整分
 * 配。
 */
static void
CatCacheCopyKeys(TupleDesc tupdesc, int nkeys, int *attnos,
				 Datum *srckeys, Datum *dstkeys)
{
	int			i;

	/*
	 * XXX：将所有键存于一次分配，或许能改善内存与
	 * 查找性能。
	 */

	for (i = 0; i < nkeys; i++)
	{
		int			attnum = attnos[i];
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
		Datum		src = srckeys[i];
		NameData	srcname;

		/*
		 * 若调用者传入 C 字符串而所需是 NAME，必须小心：
		 * 把给定参数转换为正确填充的 NAME。否则 datumCopy()
		 * 所做的 memcpy 可能越过内存
		 * 末尾。
		 */
		if (att->atttypid == NAMEOID)
		{
			namestrcpy(&srcname, DatumGetCString(src));
			src = NameGetDatum(&srcname);
		}

		dstkeys[i] = datumCopy(src,
							   att->attbyval,
							   att->attlen);
	}
}

/*
 *	PrepareToInvalidateCacheTuple()
 *
 *	这是一串相当微妙的事件链，所以请注意：
 *
 *	插入或删除元组时，无法立即从 catcache 刷写它，
 *	原因见 cache/inval.c 开头的解释。我们必须为该元组向
 *	待处理元组失效列表中添加条目，这些失效将在命令
 *	或事务结束时执行。
 *
 *	需要刷写的元组列表由 inval.c 维护。本例程是 inval.c
 *	的辅助例程。给定属于指定关系的元组，找出它
 *	可能位于的所有 catcache，为每个这样的 catcache 计算
 *	正确的哈希值，并调指定函数将缓存 ID 和哈希值
 *	记录到 inval.c 的列表中。之后如合适，将调用
 *	SysCacheInvalidate，
 *	并使用所记录的信息。
 *
 *	对于插入或删除，tuple 是目标元组而 newtuple 为 NULL。
 *	对于更新，我们只被调用一次，tuple 为旧元组版本而
 *	newtuple 为新版本。若该元组的哈希值改变了，我们应
 *	制作两个列表条目；若未改变则只制作一个。
 *
 *	注意：给定的元组此刻是否真的已加载进 catcache 并
 *	不重要。即使现在不在，到命令结束时可能已在，或
 *	者可能存在匹配的负条目待刷写 --- 或者其他后端的缓
 *	存中可能有这样的条目 --- 所以我们必须制作列表条
 *	目，以便以后刷写它。
 *
 *	另请注意：指定关系没有 catcache 并不是错误。
 *	inval.c 并不确切知道哪些关系有 catcache --- 它会对
 *	系统关系中的任何元组调用本
 *	例程。
 */
void
PrepareToInvalidateCacheTuple(Relation relation,
							  HeapTuple tuple,
							  HeapTuple newtuple,
							  void (*function) (int, uint32, Oid))
{
	slist_iter	iter;
	Oid			reloid;

	CACHE_elog(DEBUG2, "PrepareToInvalidateCacheTuple: called");

	/*
	 * 健全检查
	 */
	Assert(RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));
	Assert(PointerIsValid(function));
	Assert(CacheHdr != NULL);

	reloid = RelationGetRelid(relation);

	/* ----------------
	 *	对每缓存
	 *	   如果该缓存包含来自指定关系的元组
	 *		   在该缓存中计算此元组的哈希值，
	 *		   并调用传入的函数登记该信息。
	 * ----------------
	 */

	slist_foreach(iter, &CacheHdr->ch_caches)
	{
		CatCache   *ccp = slist_container(CatCache, cc_next, iter.cur);
		uint32		hashvalue;
		Oid			dbid;

		if (ccp->cc_reloid != reloid)
			continue;

		/* 以防缓存尚未完成初始化… */
		if (ccp->cc_tupdesc == NULL)
			CatalogCacheInitializeCache(ccp);

		hashvalue = CatalogCacheComputeTupleHashValue(ccp, ccp->cc_nkeys, tuple);
		dbid = ccp->cc_relisshared ? (Oid) 0 : MyDatabaseId;

		(*function) (ccp->id, hashvalue, dbid);

		if (newtuple)
		{
			uint32		newhashvalue;

			newhashvalue = CatalogCacheComputeTupleHashValue(ccp, ccp->cc_nkeys, newtuple);

			if (newhashvalue != hashvalue)
				(*function) (ccp->id, newhashvalue, dbid);
		}
	}
}


/*
 * 有关引用泄漏告警的子例程。导出它们是为了让
 * resowner.c 能够调用。
 */
void
PrintCatCacheLeakWarning(HeapTuple tuple)
{
	CatCTup    *ct = (CatCTup *) (((char *) tuple) -
								  offsetof(CatCTup, tuple));

	/* 安全检查，确保拿到的是缓存条目 */
	Assert(ct->ct_magic == CT_MAGIC);

	elog(WARNING, "cache reference leak: cache %s (%d), tuple %u/%u has count %d",
		 ct->my_cache->cc_relname, ct->my_cache->id,
		 ItemPointerGetBlockNumber(&(tuple->t_self)),
		 ItemPointerGetOffsetNumber(&(tuple->t_self)),
		 ct->refcount);
}

void
PrintCatCacheListLeakWarning(CatCList *list)
{
	elog(WARNING, "cache reference leak: cache %s (%d), list %p has count %d",
		 list->my_cache->cc_relname, list->my_cache->id,
		 list, list->refcount);
}
