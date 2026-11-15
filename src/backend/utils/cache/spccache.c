/*-------------------------------------------------------------------------
 *
 * spccache.c
 *	  表空间缓存管理。
 *
 * 我们为每个表空间缓存 spcoptions 的解析结果，以免
 * 每次查找重新解析。目前这样做尚未见可测量的性能
 * 收益，但将来随着我们添加更多选项，这可能改
 * 变。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/spccache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "varatt.h"


/* 存储每个表空间信息的哈希表 */
static HTAB *TableSpaceCacheHash = NULL;

typedef struct
{
	Oid			oid;			/* 查找键 - 须排最前 */
	TableSpaceOpts *opts;		/* 选项，无则为 NULL */
} TableSpaceCacheEntry;


/*
 * InvalidateTableSpaceCacheCallback
 *		pg_tablespace 被更新时刷写所有缓存条目。
 *
 * 当 pg_tablespace 被更新时，至少必须刷写对
 * 应表空间的缓存条目。目前一律全部刷写。这
 * 既快又省事且开销不大，表空间不该很多，也不该被
 * 频繁修改。
 */
static void
InvalidateTableSpaceCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TableSpaceCacheEntry *spc;

	hash_seq_init(&status, TableSpaceCacheHash);
	while ((spc = (TableSpaceCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (spc->opts)
			pfree(spc->opts);
		if (hash_search(TableSpaceCacheHash,
						&spc->oid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * InitializeTableSpaceCache
 *		初始化表空间缓存。
 */
static void
InitializeTableSpaceCache(void)
{
	HASHCTL		ctl;

	/* 初始化哈希表。 */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TableSpaceCacheEntry);
	TableSpaceCacheHash =
		hash_create("TableSpace cache", 16, &ctl,
					HASH_ELEM | HASH_BLOBS);

	/* 确保已初始化 CacheMemoryContext。 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/* 监听失效事件。 */
	CacheRegisterSyscacheCallback(TABLESPACEOID,
								  InvalidateTableSpaceCacheCallback,
								  (Datum) 0);
}

/*
 * get_tablespace
 *		为指定表 OID 获取 TableSpaceCacheEntry 结构。
 *
 * 本函数返回的指针不应被保存，因为缓存刷写会使之
 * 失效。
 */
static TableSpaceCacheEntry *
get_tablespace(Oid spcid)
{
	TableSpaceCacheEntry *spc;
	HeapTuple	tp;
	TableSpaceOpts *opts;

	/*
	 * 由于 spcid 总是来自 pg_class 元组，InvalidOid 即
	 * 默认
	 */
	if (spcid == InvalidOid)
		spcid = MyDatabaseTableSpace;

	/* 查找现有缓存条目。 */
	if (!TableSpaceCacheHash)
		InitializeTableSpaceCache();
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   &spcid,
											   HASH_FIND,
											   NULL);
	if (spc)
		return spc;

	/*
	 * TableSpace 缓存未命中，则查 catcache。若未找到
	 * 有效的 HeapTuple，那必是有人设法请求了不存在的
	 * 表空间详情。对此情况我们只
	 * 视同未指定选项。
	 */
	tp = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(spcid));
	if (!HeapTupleIsValid(tp))
		opts = NULL;
	else
	{
		Datum		datum;
		bool		isNull;

		datum = SysCacheGetAttr(TABLESPACEOID,
								tp,
								Anum_pg_tablespace_spcoptions,
								&isNull);
		if (isNull)
			opts = NULL;
		else
		{
			bytea	   *bytea_opts = tablespace_reloptions(datum, false);

			opts = MemoryContextAlloc(CacheMemoryContext, VARSIZE(bytea_opts));
			memcpy(opts, bytea_opts, VARSIZE(bytea_opts));
		}
		ReleaseSysCache(tp);
	}

	/*
	 * 现在创建缓存条目。务必在读取 pg_tablespace 条目
	 * 之后再进行，因为读取操作可能引发缓存刷
	 * 写。
	 */
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   &spcid,
											   HASH_ENTER,
											   NULL);
	spc->opts = opts;
	return spc;
}

/*
 * get_tablespace_page_costs
 *		返回给定表空间的随机和/或顺序页面成本。
 *
 *		该值未被事务锁定，因此当某个使用了这些值
 *		进行规划的 SELECT 仍在执行时，该值可能被改
 *		变。
 */
void
get_tablespace_page_costs(Oid spcid,
						  double *spc_random_page_cost,
						  double *spc_seq_page_cost)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	Assert(spc != NULL);

	if (spc_random_page_cost)
	{
		if (!spc->opts || spc->opts->random_page_cost < 0)
			*spc_random_page_cost = random_page_cost;
		else
			*spc_random_page_cost = spc->opts->random_page_cost;
	}

	if (spc_seq_page_cost)
	{
		if (!spc->opts || spc->opts->seq_page_cost < 0)
			*spc_seq_page_cost = seq_page_cost;
		else
			*spc_seq_page_cost = spc->opts->seq_page_cost;
	}
}

/*
 * get_tablespace_io_concurrency
 *
 *		该值未被事务锁定，因此当某个使用了这些值
 *		进行规划的 SELECT 仍在执行时，该值可能被改
 *		变。
 */
int
get_tablespace_io_concurrency(Oid spcid)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	if (!spc->opts || spc->opts->effective_io_concurrency < 0)
		return effective_io_concurrency;
	else
		return spc->opts->effective_io_concurrency;
}

/*
 * get_tablespace_maintenance_io_concurrency
 */
int
get_tablespace_maintenance_io_concurrency(Oid spcid)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	if (!spc->opts || spc->opts->maintenance_io_concurrency < 0)
		return maintenance_io_concurrency;
	else
		return spc->opts->maintenance_io_concurrency;
}
