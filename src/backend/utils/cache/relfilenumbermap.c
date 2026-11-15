/*-------------------------------------------------------------------------
 *
 * relfilenumbermap.c
 *	  relfilenumber-oid 映射缓存。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/relfilenumbermap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"
#include "utils/relmapper.h"

/* 存储每个 relfilenumber <-> oid 对信息的哈希表 */
static HTAB *RelfilenumberMapHash = NULL;

/* 在 InitializeRelfilenumberMap 中首次构建 */
static ScanKeyData relfilenumber_skey[2];

typedef struct
{
	Oid			reltablespace;
	RelFileNumber relfilenumber;
} RelfilenumberMapKey;

typedef struct
{
	RelfilenumberMapKey key;	/* 查找键 - 须排最前 */
	Oid			relid;			/* pg_class.oid */
} RelfilenumberMapEntry;

/*
 * RelfilenumberMapInvalidateCallback
 *		当 pg_class 以相关方式被更新时刷写映射条目。
 */
static void
RelfilenumberMapInvalidateCallback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	RelfilenumberMapEntry *entry;

	/* 回调仅在创建哈希表之后才会注册 */
	Assert(RelfilenumberMapHash != NULL);

	hash_seq_init(&status, RelfilenumberMapHash);
	while ((entry = (RelfilenumberMapEntry *) hash_seq_search(&status)) != NULL)
	{
		/*
		 * 若 relid 为 InvalidOid（表示完全复位），必须移除
		 * 所有条目，否则只移除特定关系的条目。
		 * 负缓存条目一律移除。
		 */
		if (relid == InvalidOid ||	/* 完全复位 */
			entry->relid == InvalidOid ||	/* 负缓存条目 */
			entry->relid == relid)	/* 单个已刷写关系 */
		{
			if (hash_search(RelfilenumberMapHash,
							&entry->key,
							HASH_REMOVE,
							NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/*
 * InitializeRelfilenumberMap
 *		初始化缓存，在首次使用或复位后进行。
 */
static void
InitializeRelfilenumberMap(void)
{
	HASHCTL		ctl;
	int			i;

	/* 确保已初始化 CacheMemoryContext。 */
	if (CacheMemoryContext == NULL)
		CreateCacheMemoryContext();

	/* 构建skey */
	MemSet(&relfilenumber_skey, 0, sizeof(relfilenumber_skey));

	for (i = 0; i < 2; i++)
	{
		fmgr_info_cxt(F_OIDEQ,
					  &relfilenumber_skey[i].sk_func,
					  CacheMemoryContext);
		relfilenumber_skey[i].sk_strategy = BTEqualStrategyNumber;
		relfilenumber_skey[i].sk_subtype = InvalidOid;
		relfilenumber_skey[i].sk_collation = InvalidOid;
	}

	relfilenumber_skey[0].sk_attno = Anum_pg_class_reltablespace;
	relfilenumber_skey[1].sk_attno = Anum_pg_class_relfilenode;

	/*
	 * 现在才创建 RelfilenumberMapHash，这样当上面的
	 * fmgr_info_cxt() 因内存不足 ERROR 退出时不会半初始
	 * 化。
	 */
	ctl.keysize = sizeof(RelfilenumberMapKey);
	ctl.entrysize = sizeof(RelfilenumberMapEntry);
	ctl.hcxt = CacheMemoryContext;

	RelfilenumberMapHash =
		hash_create("RelfilenumberMap cache", 64, &ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 监听失效事件。 */
	CacheRegisterRelcacheCallback(RelfilenumberMapInvalidateCallback,
								  (Datum) 0);
}

/*
 * 将关系（tablespace, relfilenumber）映射到关系 oid 并缓存
 * 结果。
 *
 * 临时关系可能与永久关系或其他后端创建的临时关系共
 * 享其 relfilenumber。要唯一地标识一个临时关系，就需
 * 要后端的 proc number，而我们并不知道这一信息。因此，
 * 本函数忽略这种情况。
 *
 * 若找不到符合条件的关系，则返回 InvalidOid。
 */
Oid
RelidByRelfilenumber(Oid reltablespace, RelFileNumber relfilenumber)
{
	RelfilenumberMapKey key;
	RelfilenumberMapEntry *entry;
	bool		found;
	SysScanDesc scandesc;
	Relation	relation;
	HeapTuple	ntp;
	ScanKeyData skey[2];
	Oid			relid;

	if (RelfilenumberMapHash == NULL)
		InitializeRelfilenumberMap();

	/* 当值实际为 MyDatabaseTableSpace 时 pg_class 将显示 0 */
	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = 0;

	MemSet(&key, 0, sizeof(key));
	key.reltablespace = reltablespace;
	key.relfilenumber = relfilenumber;

	/*
	 * 检查缓存，找到则返回条目。即使稍后找不到
	 * 目标关系，我们也会存储否定匹配并从缓存返回一
	 * 个 InvalidOid。这对性能并非必要，因为查询无效值
	 * 本不应是频繁操作，但这基本
	 * 上是免费的。
	 */
	entry = hash_search(RelfilenumberMapHash, &key, HASH_FIND, &found);

	if (found)
		return entry->relid;

	/* 好，无先前条目，用麻烦的方式做 */

	/* 在执行实际查找之前初始化空/否定缓存条目 */
	relid = InvalidOid;

	if (reltablespace == GLOBALTABLESPACE_OID)
	{
		/*
		 * 共享表，查 relmapper。
		 */
		relid = RelationMapFilenumberToOid(relfilenumber, true);
	}
	else
	{
		/*
		 * 非共享表，可能是普通关系，或是钉住的
		 * 非共享关系，例如 pg_class。
		 */

		/* 在 pg_class 中查找以检查普通关系 */
		relation = table_open(RelationRelationId, AccessShareLock);

		/* 把 scankey 复制到本地副本，扫描期间它会被修改 */
		memcpy(skey, relfilenumber_skey, sizeof(skey));

		/* 设置扫描参数 */
		skey[0].sk_argument = ObjectIdGetDatum(reltablespace);
		skey[1].sk_argument = ObjectIdGetDatum(relfilenumber);

		scandesc = systable_beginscan(relation,
									  ClassTblspcRelfilenodeIndexId,
									  true,
									  NULL,
									  2,
									  skey);

		found = false;

		while (HeapTupleIsValid(ntp = systable_getnext(scandesc)))
		{
			Form_pg_class classform = (Form_pg_class) GETSTRUCT(ntp);

			if (classform->relpersistence == RELPERSISTENCE_TEMP)
				continue;

			if (found)
				elog(ERROR,
					 "unexpected duplicate for tablespace %u, relfilenumber %u",
					 reltablespace, relfilenumber);
			found = true;

			Assert(classform->reltablespace == reltablespace);
			Assert(classform->relfilenode == relfilenumber);
			relid = classform->oid;
		}

		systable_endscan(scandesc);
		table_close(relation, AccessShareLock);

		/* 检查已映射但未共享的表 */
		if (!found)
			relid = RelationMapFilenumberToOid(relfilenumber, false);
	}

	/*
	 * 现在才将条目放入缓存：我们打开 pg_class 的动作可
	 * 能已触发缓存失效，那会删除我们若在上方放入的
	 * 新条目。
	 */
	entry = hash_search(RelfilenumberMapHash, &key, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "corrupted hashtable");
	entry->relid = relid;

	return relid;
}
