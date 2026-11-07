/*-------------------------------------------------------------------------
 * relation.c
 *	   PostgreSQL 逻辑复制关系映射缓存            
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/relation.c
 *
 * NOTES
 *	  本文件的例程主要处理                                     
 *	  将本地复制目标关系的属性映射到其远端对应物 
 *	  的属性。       
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "replication/logicalrelation.h"
#include "replication/worker_internal.h"
#include "utils/inval.h"


static MemoryContext LogicalRepRelMapContext = NULL;

static HTAB *LogicalRepRelMap = NULL;

/*
 * 分区映射(LogicalRepPartMap)  
 *
 * 当分区表作为复制目标时，被复制的操作实际是在
 * 其叶子分区上执行的，这需要这些分区也要映射到远端
 * 关系上。父条目（LogicalRepRelMapEntry）无法原样用于      
 * 所有分区，因为各分区的属性编号可能不同，这意味着
 * 与远端关系属性的属性映射必须                             
 * 为每个分区单独                                                
 * 维护。                     
 */
static MemoryContext LogicalRepPartMapContext = NULL;
static HTAB *LogicalRepPartMap = NULL;
typedef struct LogicalRepPartMapEntry
{
	Oid			partoid;		/* LogicalRepPartMap 的键*/
	LogicalRepRelMapEntry relmapentry;
} LogicalRepPartMapEntry;

static Oid	FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
									 AttrMap *attrMap);

/*
 * 我们的关系映射缓存的 relcache 失效回调。   
 */
static void
logicalrep_relmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepRelMapEntry *entry;

	/* 以防万一。  */
	if (LogicalRepRelMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		/* TODO，用反向查找哈希表？  */
		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->localreloid == reloid)
			{
				entry->localrelvalid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* 使所有缓存条目失效  */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
			entry->localrelvalid = false;
	}
}

/*
 * 初始化关系映射缓存。    
 */
static void
logicalrep_relmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepRelMapContext)
		LogicalRepRelMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepRelMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* 初始化关系哈希表。         */
	ctl.keysize = sizeof(LogicalRepRelId);
	ctl.entrysize = sizeof(LogicalRepRelMapEntry);
	ctl.hcxt = LogicalRepRelMapContext;

	LogicalRepRelMap = hash_create("logicalrep relation map cache", 128, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 留意失效事件。          */
	CacheRegisterRelcacheCallback(logicalrep_relmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * 释放关系映射缓存的条目。   
 */
static void
logicalrep_relmap_free_entry(LogicalRepRelMapEntry *entry)
{
	LogicalRepRelation *remoterel;

	remoterel = &entry->remoterel;

	pfree(remoterel->nspname);
	pfree(remoterel->relname);

	if (remoterel->natts > 0)
	{
		int			i;

		for (i = 0; i < remoterel->natts; i++)
			pfree(remoterel->attnames[i]);

		pfree(remoterel->attnames);
		pfree(remoterel->atttyps);
	}
	bms_free(remoterel->attkeys);

	if (entry->attrmap)
		free_attrmap(entry->attrmap);
}

/*
 * 在关系映射缓存中添加新条目或更新现有条目。  
 *
 * 当发布者发送新的关系映射时调用，以更新我们对 
 * 来自该发布者数据的预期视图。             
 */
void
logicalrep_relmap_update(LogicalRepRelation *remoterel)
{
	MemoryContext oldctx;
	LogicalRepRelMapEntry *entry;
	bool		found;
	int			i;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/*
	 * HASH_ENTER 若条目存在则返回它，否则新建条目。        
	 */
	entry = hash_search(LogicalRepRelMap, &remoterel->remoteid,
						HASH_ENTER, &found);

	if (found)
		logicalrep_relmap_free_entry(entry);

	memset(entry, 0, sizeof(LogicalRepRelMapEntry));

	/* 为数据建立缓存副本  */
	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
	entry->remoterel.remoteid = remoterel->remoteid;
	entry->remoterel.nspname = pstrdup(remoterel->nspname);
	entry->remoterel.relname = pstrdup(remoterel->relname);
	entry->remoterel.natts = remoterel->natts;
	entry->remoterel.attnames = palloc(remoterel->natts * sizeof(char *));
	entry->remoterel.atttyps = palloc(remoterel->natts * sizeof(Oid));
	for (i = 0; i < remoterel->natts; i++)
	{
		entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
		entry->remoterel.atttyps[i] = remoterel->atttyps[i];
	}
	entry->remoterel.replident = remoterel->replident;
	entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	MemoryContextSwitchTo(oldctx);
}

/*
 * 在 TupleDesc 结构中按属性名查找属性下标。   
 *
 * 未找到时返回 -1。
 */
static int
logicalrep_rel_att_by_name(LogicalRepRelation *remoterel, const char *attname)
{
	int			i;

	for (i = 0; i < remoterel->natts; i++)
	{
		if (strcmp(remoterel->attnames[i], attname) == 0)
			return i;
	}

	return -1;
}

/*
 * 如存在缺少的本地关系列，则以这些列名报错。         
 */
static void
logicalrep_report_missing_attrs(LogicalRepRelation *remoterel,
								Bitmapset *missingatts)
{
	if (!bms_is_empty(missingatts))
	{
		StringInfoData missingattsbuf;
		int			missingattcnt = 0;
		int			i;

		initStringInfo(&missingattsbuf);

		i = -1;
		while ((i = bms_next_member(missingatts, i)) >= 0)
		{
			missingattcnt++;
			if (missingattcnt == 1)
				appendStringInfo(&missingattsbuf, _("\"%s\""),
								 remoterel->attnames[i]);
			else
				appendStringInfo(&missingattsbuf, _(", \"%s\""),
								 remoterel->attnames[i]);
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg_plural("logical replication target relation \"%s.%s\" is missing replicated column: %s",
							   "logical replication target relation \"%s.%s\" is missing replicated columns: %s",
							   missingattcnt,
							   remoterel->nspname,
							   remoterel->relname,
							   missingattsbuf.data)));
	}
}

/*
 * 检查复制标识是否匹配，并标记 updatable 标志。
 *
 * 订阅者的复制标识可以更严格（列更少），因为这不  
 * 会妨碍我们找到唯一元组。即，若发布者的复制标识
 * 为 (id,timestamp) 而订阅者只有 (id)，这不成问题，但 
 * 反过来则会出问题。                   
 *
 * 若发现本地复制标识不足以应用更新/删除（插     
 * 入不在乎！），只将关系条目标记为不可更
 * 新，若需要则留给                             
 * check_relation_updatable() 去抛出实际错误。            
 */
static void
logicalrep_rel_mark_updatable(LogicalRepRelMapEntry *entry)
{
	Bitmapset  *idkey;
	LogicalRepRelation *remoterel = &entry->remoterel;
	int			i;

	entry->updatable = true;

	idkey = RelationGetIndexAttrBitmap(entry->localrel,
									   INDEX_ATTR_BITMAP_IDENTITY_KEY);
	/* 无复制标识时回退到主键     */
	if (idkey == NULL)
	{
		idkey = RelationGetIndexAttrBitmap(entry->localrel,
										   INDEX_ATTR_BITMAP_PRIMARY_KEY);

		/*
		 * 若无复制标识索引和主键，则发布表必须具有    
		 * REPLICA IDENTITY FULL。   
		 */
		if (idkey == NULL && remoterel->replident != REPLICA_IDENTITY_FULL)
			entry->updatable = false;
	}

	i = -1;
	while ((i = bms_next_member(idkey, i)) >= 0)
	{
		int			attnum = i + FirstLowInvalidHeapAttributeNumber;

		if (!AttrNumberIsForUserDefinedAttr(attnum))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" uses "
							"system columns in REPLICA IDENTITY index",
							remoterel->nspname, remoterel->relname)));

		attnum = AttrNumberGetAttrOffset(attnum);

		if (entry->attrmap->attnums[attnum] < 0 ||
			!bms_is_member(entry->attrmap->attnums[attnum], remoterel->attkeys))
		{
			entry->updatable = false;
			break;
		}
	}
}

/*
 * 打开与远端关系相关联的本地关系。       
 *
 * 若 Relcache 映射因本地 DDL 而失效，则将其重建。  
 */
LogicalRepRelMapEntry *
logicalrep_rel_open(LogicalRepRelId remoteid, LOCKMODE lockmode)
{
	LogicalRepRelMapEntry *entry;
	bool		found;
	LogicalRepRelation *remoterel;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* 搜索现有条目。      */
	entry = hash_search(LogicalRepRelMap, &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no relation map entry for remote relation ID %u",
			 remoteid);

	remoterel = &entry->remoterel;

	/* 确保不泄漏 relcache 引用计数。  */
	if (entry->localrel)
		elog(ERROR, "remote relation ID %u is already open", remoteid);

	/*
	 * 打开并锁定关系时，待处理失效消息会被处理，这    
	 * 可能使关系失效。因此，如果条目被视为有效，     
	 * 那就尝试通过 OID 打开本地关系，看看是否随之       
	 * 发生失效。            
	 */
	if (entry->localrelvalid)
	{
		entry->localrel = try_table_open(entry->localreloid, lockmode);
		if (!entry->localrel)
		{
			/* 表被重命名或已删除。*/
			entry->localrelvalid = false;
		}
		else if (!entry->localrelvalid)
		{
			/* 注意此处释放已不再有用的锁。      */
			table_close(entry->localrel, lockmode);
			entry->localrel = NULL;
		}
	}

	/*
	 * 若条目自我们上次持锁之后已被标记为失效，      
	 * 按名称重新打开本地关系并重建所有派生数据。 
	 */
	if (!entry->localrelvalid)
	{
		Oid			relid;
		TupleDesc	desc;
		MemoryContext oldctx;
		int			i;
		Bitmapset  *missingatts;

		/* 释放已不再有用的 attrmap（如有）  */
		if (entry->attrmap)
		{
			free_attrmap(entry->attrmap);
			entry->attrmap = NULL;
		}

		/* 尝试按名称查找并锁定关系。    */
		relid = RangeVarGetRelid(makeRangeVar(remoterel->nspname,
											  remoterel->relname, -1),
								 lockmode, true);
		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" does not exist",
							remoterel->nspname, remoterel->relname)));
		entry->localrel = table_open(relid, NoLock);
		entry->localreloid = relid;

		/* 检查受支持的 relkind。*/
		CheckSubscriptionRelkind(entry->localrel->rd_rel->relkind,
								 remoterel->nspname, remoterel->relname);

		/*
		 * 构建本地属性编号到远端属性                         
		 * 编号的映射，并校验没有遗漏任何被复制的列，  
		 * 否则可能导致不必要的数据丢失。       
		 */
		desc = RelationGetDescr(entry->localrel);
		oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
		entry->attrmap = make_attrmap(desc->natts);
		MemoryContextSwitchTo(oldctx);

		/* 检查并报告缺少属性，若有   */
		missingatts = bms_add_range(NULL, 0, remoterel->natts - 1);
		for (i = 0; i < desc->natts; i++)
		{
			int			attnum;
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			if (attr->attisdropped || attr->attgenerated)
			{
				entry->attrmap->attnums[i] = -1;
				continue;
			}

			attnum = logicalrep_rel_att_by_name(remoterel,
												NameStr(attr->attname));

			entry->attrmap->attnums[i] = attnum;
			if (attnum >= 0)
				missingatts = bms_del_member(missingatts, attnum);
		}

		logicalrep_report_missing_attrs(remoterel, missingatts);

		/* 清理  */
		bms_free(missingatts);

		/*
		 * 设置表的复制标识是否足以应用            
		 * 更新/删除 
		 */
		logicalrep_rel_mark_updatable(entry);

		/*
		 * 寻找可用索引并不常见。它在首次对关系执行   
		 * 操作时，或在此关系的缓存条目失效                   
		 * 之后进行，如 ANALYZE 或在关系上 CREATE/DROP 索引    
		 * 时发生。     
		 */
		entry->localindexoid = FindLogicalRepLocalIndex(entry->localrel, remoterel,
														entry->attrmap);

		entry->localrelvalid = true;
	}

	if (entry->state != SUBREL_STATE_READY)
		entry->state = GetSubscriptionRelState(MySubscription->oid,
											   entry->localreloid,
											   &entry->statelsn);

	return entry;
}

/*
 * 关闭先前打开的逻辑关系。         
 */
void
logicalrep_rel_close(LogicalRepRelMapEntry *rel, LOCKMODE lockmode)
{
	table_close(rel->localrel, lockmode);
	rel->localrel = NULL;
}

/*
 * 分区缓存：查找分区 LogicalRepRelMapEntry 条目  
 *
 * 与关系映射缓存不同，此缓存以分区 OID 而非远端    
 * 关系 OID 为键，因为只有当分区未直接映射到任何远  
 * 端关系时才需使用此缓存，如复制以其某个祖先作     
 * 为目标时。                                                
 */

/*
 * relcache 失效回调         
 */
static void
logicalrep_partmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepPartMapEntry *entry;

	/* 以防万一。  */
	if (LogicalRepPartMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		/* TODO，用反向查找哈希表？  */
		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->relmapentry.localreloid == reloid)
			{
				entry->relmapentry.localrelvalid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* 使所有缓存条目失效  */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
			entry->relmapentry.localrelvalid = false;
	}
}

/*
 * 重置分区映射中引用 remoterel 的条目。             
 *
 * 当发布者发送新的关系映射时调用，以更新我们对来  
 * 自该发布者的传入数据的预期视图。   
 *
 * 注意我们不会在此更新条目中的 remoterel 信息，        
 * 而是会在 logicalrep_partition_open 中更新该信息，以免  
 * 做无用功。  
 */
void
logicalrep_partmap_reset_relmap(LogicalRepRelation *remoterel)
{
	HASH_SEQ_STATUS status;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelMapEntry *entry;

	if (LogicalRepPartMap == NULL)
		return;

	hash_seq_init(&status, LogicalRepPartMap);
	while ((part_entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
	{
		entry = &part_entry->relmapentry;

		if (entry->remoterel.remoteid != remoterel->remoteid)
			continue;

		logicalrep_relmap_free_entry(entry);

		memset(entry, 0, sizeof(LogicalRepRelMapEntry));
	}
}

/*
 * 初始化分区映射缓存。     
 */
static void
logicalrep_partmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepPartMapContext)
		LogicalRepPartMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepPartMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* 初始化关系哈希表。         */
	ctl.keysize = sizeof(Oid);	/* 分区 OID    */
	ctl.entrysize = sizeof(LogicalRepPartMapEntry);
	ctl.hcxt = LogicalRepPartMapContext;

	LogicalRepPartMap = hash_create("logicalrep partition map cache", 64, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 留意失效事件。          */
	CacheRegisterRelcacheCallback(logicalrep_partmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * logicalrep_partition_open
 *
 * 返回的条目复用根表条目的大部分值，但属性映射除   
 * 外——它对分区可能不同。不过，我们必须实际复制 
 * 所有数据，以防根表的条目被释放                       
 * /重建。         
 *
 * 注意不存在 logicalrep_partition_close，因为组件关系由调用 
 * 者关闭。       
 */
LogicalRepRelMapEntry *
logicalrep_partition_open(LogicalRepRelMapEntry *root,
						  Relation partrel, AttrMap *map)
{
	LogicalRepRelMapEntry *entry;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelation *remoterel = &root->remoterel;
	Oid			partOid = RelationGetRelid(partrel);
	AttrMap    *attrmap = root->attrmap;
	bool		found;
	MemoryContext oldctx;

	if (LogicalRepPartMap == NULL)
		logicalrep_partmap_init();

	/* 搜索现有条目。      */
	part_entry = (LogicalRepPartMapEntry *) hash_search(LogicalRepPartMap,
														&partOid,
														HASH_ENTER, &found);

	entry = &part_entry->relmapentry;

	/*
	 * 我们必须始终用最新的分区 Relation 指针覆盖        
	 * entry->localrel，因为旧值所指向的 Relation 可能在调      
	 * 用者上次使用此条目之后关闭分区关系时已被清   
	 * 除。注意 localrelvalid 仅由 relcache 失效回调更新，      
	 * 因此无论 localrel 所指向的 Relation 是否                    
	 * 已被清除，该值仍可能                                      
	 * 为真。           
	 */
	if (found && entry->localrelvalid)
	{
		entry->localrel = partrel;
		return entry;
	}

	/* 切换到长寿命上下文。  */
	oldctx = MemoryContextSwitchTo(LogicalRepPartMapContext);

	if (!found)
	{
		memset(part_entry, 0, sizeof(LogicalRepPartMapEntry));
		part_entry->partoid = partOid;
	}

	/* 释放已不再有用的 attrmap（如有）  */
	if (entry->attrmap)
	{
		free_attrmap(entry->attrmap);
		entry->attrmap = NULL;
	}

	if (!entry->remoterel.remoteid)
	{
		int			i;

		/* 远端关系自根条目原样复制而来。        */
		entry = &part_entry->relmapentry;
		entry->remoterel.remoteid = remoterel->remoteid;
		entry->remoterel.nspname = pstrdup(remoterel->nspname);
		entry->remoterel.relname = pstrdup(remoterel->relname);
		entry->remoterel.natts = remoterel->natts;
		entry->remoterel.attnames = palloc(remoterel->natts * sizeof(char *));
		entry->remoterel.atttyps = palloc(remoterel->natts * sizeof(Oid));
		for (i = 0; i < remoterel->natts; i++)
		{
			entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
			entry->remoterel.atttyps[i] = remoterel->atttyps[i];
		}
		entry->remoterel.replident = remoterel->replident;
		entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	}

	entry->localrel = partrel;
	entry->localreloid = partOid;

	/*
	 * 如果分区的属性与根关系的不匹配，就需要新建一  
	 * 个 attrmap，把分区属性编号映射到 remoterel 属          
	 * 性，而非原来那个把根关系属性编号映射到      
	 * remoterel 的映射。           
	 *
	 * 注意来自元组路由数据结构的 'map'                    
	 * 包含从 1 开始的属性编号（父关系的）。不过，       
	 * 而 'entry'（逻辑复制数据结构）中的映射则包含    
	 * 从 0 开始的属性编号（远端关系的）。
	 */
	if (map)
	{
		AttrNumber	attno;

		entry->attrmap = make_attrmap(map->maplen);
		for (attno = 0; attno < entry->attrmap->maplen; attno++)
		{
			AttrNumber	root_attno = map->attnums[attno];

			/* 0 表示该属性已删除。参见 AttrMap 顶部注释。   */
			if (root_attno == 0)
				entry->attrmap->attnums[attno] = -1;
			else
				entry->attrmap->attnums[attno] = attrmap->attnums[root_attno - 1];
		}
	}
	else
	{
		/* 缺少 copy_attmap，只能用笨办法。 */
		entry->attrmap = make_attrmap(attrmap->maplen);
		memcpy(entry->attrmap->attnums, attrmap->attnums,
			   attrmap->maplen * sizeof(AttrNumber));
	}

	/* 设置表的复制标识是否足以应用更新/删除。            */
	logicalrep_rel_mark_updatable(entry);

	/* state 和 statelsn 保持为 0。     */
	MemoryContextSwitchTo(oldctx);

	/*
	 * 寻找可用索引并不常见。它在首次对关系执行   
	 * 操作时，或在此关系的缓存条目失效之                   
	 * 后进行，如 ANALYZE 或在该关系上 CREATE/DROP 索引        
	 * 时。    
	 *
	 * 我们还倾向于在 oldctx 上运行此代码，以免任何内容
	 * 泄漏到 LogicalRepPartMapContext（故 CacheMemoryContext）。   
	 */
	entry->localindexoid = FindLogicalRepLocalIndex(partrel, remoterel,
													entry->attrmap);

	entry->localrelvalid = true;

	return entry;
}

/*
 * 返回应用工作进程可用于扫描关系的索引                  
 * 的 oid。   
 *
 * 我们期望在远端关系定义了 REPLICA IDENTITY FULL 时调用     
 * 此函数。        
 *
 * 若找不到合适的索引，返回 InvalidOid。
 */
static Oid
FindUsableIndexForReplicaIdentityFull(Relation localrel, AttrMap *attrmap)
{
	List	   *idxlist = RelationGetIndexList(localrel);
	ListCell   *lc;

	foreach(lc, idxlist)
	{
		Oid			idxoid = lfirst_oid(lc);
		bool		isUsableIdx;
		Relation	idxRel;
		IndexInfo  *idxInfo;

		idxRel = index_open(idxoid, AccessShareLock);
		idxInfo = BuildIndexInfo(idxRel);
		isUsableIdx = IsIndexUsableForReplicaIdentityFull(idxInfo, attrmap);
		index_close(idxRel, AccessShareLock);

		/* 返回找到的第一个合格索引  */
		if (isUsableIdx)
			return idxoid;
	}

	return InvalidOid;
}

/*
 * 若索引可用于复制标识 FULL，则返回真。         
 *
 * 该索引必须是 btree、非部分索引，且最左字段必须是 
 * 引用远端关系列的列（而非表达式）。                   
 * 这些限制有助于让索引扫描近似于 PK/RI 索引扫       
 * 描。
 *
 * attrmap 是本地属性到远端属性的映射。我们可以查       
 * 阅该映射，以检查本地索引属性是否有对应的远端       
 * 属性。 
 *
 * 注意，复制标识 FULL 的索引扫描限制只是                 
 * PK/RI 各项限制的一个子集。例如，我们支持标记为      
 * [NULL] 的列，或者我们不关心此处约束的 [NOT             
 * DEFERRABLE] 方面。这对我们可行，因为我们总是在          
 * 对非 PK/RI 索引扫描比较元组。参见   
 * RelationFindReplTupleByIndex() 
 *
 * XXX：支持非 btree 索引不存在根本性的问题。              
 * 我们主要需要放宽 RelationFindReplTupleByIndex() 中的限         
 * 制。而对于部分索引，所需的改动可能更大。如果   
 * 没有任何元组满足索引扫描的表达式，我们会回退        
 * 到顺序执行，这在某些情况下可能不是个好主意。    
 */
bool
IsIndexUsableForReplicaIdentityFull(IndexInfo *indexInfo, AttrMap *attrmap)
{
	AttrNumber	keycol;

	/* 该索引必须是 Btree 索引 */
	if (indexInfo->ii_Am != BTREE_AM_OID)
		return false;

	/* 该索引必须是非部分索引     */
	if (indexInfo->ii_Predicate != NIL)
		return false;

	Assert(indexInfo->ii_NumIndexAttrs >= 1);

	/* 最左侧索引字段必须为列（非表达式）*/
	keycol = indexInfo->ii_IndexAttrNumbers[0];
	if (!AttributeNumberIsValid(keycol))
		return false;

	/*
	 * 此外，最左侧的索引字段必须引用远端关系的列。     
	 * 这是因为如果不这样，顺序扫描在大多数情况下都  
	 * 优于索引扫描。    
	 */
	if (attrmap->maplen <= AttrNumberGetAttrOffset(keycol) ||
		attrmap->attnums[AttrNumberGetAttrOffset(keycol)] < 0)
		return false;

	return true;
}

/*
 * 获取复制标识索引；若未定义则获取主键。        
 *
 * 若均未定义，则返回 InvalidOid   
 */
Oid
GetRelationIdentityOrPK(Relation rel)
{
	Oid			idxoid;

	idxoid = RelationGetReplicaIndex(rel);

	if (!OidIsValid(idxoid))
		idxoid = RelationGetPrimaryKeyIndex(rel);

	return idxoid;
}

/*
 * 若我们能为订阅者使用索引，则返回索引的 oid。否则 
 * 为 InvalidOid。  
 */
static Oid
FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
						 AttrMap *attrMap)
{
	Oid			idxoid;

	/*
	 * 分区表永远不需要索引 oid，始终依赖叶子分区的   
	 * 索引。         
	 */
	if (localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return InvalidOid;

	/*
	 * 简单情形：我们已经有了主键或复制标识索引。        
	 */
	idxoid = GetRelationIdentityOrPK(localrel);
	if (OidIsValid(idxoid))
		return idxoid;

	if (remoterel->replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * 我们仍在寻找使用索引的又一机会。如果本地  
		 * 关系上定义了任何索引，则尝试挑选一个合适的   
		 * 索引。      
		 *
		 * 索引选择可以放心地假定，鉴于远端关系具有复  
		 * 制标识 FULL，索引扫描所需的全部列都将            
		 * 可用。             
		 *
		 * 注意，我们不使用规划器来寻找扫描关系的最廉价
		 * 方法，因为那需要我们要么使用更低层的规划器  
		 * 函数——长远来看那会成为维护负担——要么使用
		 * 功能完备的规划器，那可能造成                
		 * 开销。
		 */
		return FindUsableIndexForReplicaIdentityFull(localrel, attrMap);
	}

	return InvalidOid;
}
