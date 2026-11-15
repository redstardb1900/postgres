/*-------------------------------------------------------------------------
 *
 * attoptcache.c
 *	  属性选项缓存管理。
 *
 * 属性选项的缓存独立于 pg_attribute 条目的定长部分，
 * 而这些条目由 relcache 处理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/attoptcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "utils/attoptcache.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "varatt.h"


/* 存储每个属性选项信息的哈希表 */
static HTAB *AttoptCacheHash = NULL;

/* attrelid 和 attnum 构成查找键，且必须排在最前 */
typedef struct
{
	Oid			attrelid;
	int			attnum;
} AttoptCacheKey;

typedef struct
{
	AttoptCacheKey key;			/* 查找键 - 须排最前 */
	AttributeOpts *opts;		/* 选项，无则为 NULL */
} AttoptCacheEntry;


/*
 * InvalidateAttoptCacheCallback
 *		pg_attribute 被更新时刷写所有缓存条目。
 *
 * 当 pg_attribute 被更新时，至少必须刷写对应
 * 属性的缓存条目。目前我们只将它们全部刷写。由于属
 * 性选项未用于性能关键路径（如查询执行），因此这
 * 样做似乎就可以了。
 */
static void
InvalidateAttoptCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	AttoptCacheEntry *attopt;

	hash_seq_init(&status, AttoptCacheHash);
	while ((attopt = (AttoptCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (attopt->opts)
			pfree(attopt->opts);
		if (hash_search(AttoptCacheHash,
						&attopt->key,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * InitializeAttoptCache
 *		初始化属性选项缓存。
 */
static void
InitializeAttoptCache(void)
{
	HASHCTL		ctl;

	/* 初始化哈希表。 */
	ctl.keysize = sizeof(AttoptCacheKey);
	ctl.entrysize = sizeof(AttoptCacheEntry);
	AttoptCacheHash =
		hash_create("Attopt cache", 256, &ctl,
					HASH_ELEM | HASH_BLOBS);

	/* 确保已初始化 CacheMemoryContext。 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/* 监听失效事件。 */
	CacheRegisterSyscacheCallback(ATTNUM,
								  InvalidateAttoptCacheCallback,
								  (Datum) 0);
}

/*
 * get_attribute_options
 *		获取指定表 OID 的属性选项。
 */
AttributeOpts *
get_attribute_options(Oid attrelid, int attnum)
{
	AttoptCacheKey key;
	AttoptCacheEntry *attopt;
	AttributeOpts *result;
	HeapTuple	tp;

	/* 查找现有缓存条目。 */
	if (!AttoptCacheHash)
		InitializeAttoptCache();
	memset(&key, 0, sizeof(key));	/* 确保填充位均未被置位 */
	key.attrelid = attrelid;
	key.attnum = attnum;
	attopt =
		(AttoptCacheEntry *) hash_search(AttoptCacheHash,
										 &key,
										 HASH_FIND,
										 NULL);

	/* Attopt 缓存未命中，构造新缓存条目。 */
	if (!attopt)
	{
		AttributeOpts *opts;

		tp = SearchSysCache2(ATTNUM,
							 ObjectIdGetDatum(attrelid),
							 Int16GetDatum(attnum));

		/*
		 * 若未找到有效的 HeapTuple，必是有人设法请求
		 * 不存在的属性的详情。
		 * 我们将这种情况视为未指定任何选项。
		 */
		if (!HeapTupleIsValid(tp))
			opts = NULL;
		else
		{
			Datum		datum;
			bool		isNull;

			datum = SysCacheGetAttr(ATTNUM,
									tp,
									Anum_pg_attribute_attoptions,
									&isNull);
			if (isNull)
				opts = NULL;
			else
			{
				bytea	   *bytea_opts = attribute_reloptions(datum, false);

				opts = MemoryContextAlloc(CacheMemoryContext,
										  VARSIZE(bytea_opts));
				memcpy(opts, bytea_opts, VARSIZE(bytea_opts));
			}
			ReleaseSysCache(tp);
		}

		/*
		 * 务必在读取 pg_attribute 后才创建实际缓存条目，
		 * 因为读取可能引发缓存刷写。
		 */
		attopt = (AttoptCacheEntry *) hash_search(AttoptCacheHash,
												  &key,
												  HASH_ENTER,
												  NULL);
		attopt->opts = opts;
	}

	/* 结果在调用者内存上下文返回。 */
	if (attopt->opts == NULL)
		return NULL;
	result = palloc(VARSIZE(attopt->opts));
	memcpy(result, attopt->opts, VARSIZE(attopt->opts));
	return result;
}
