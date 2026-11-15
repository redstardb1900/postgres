/*-------------------------------------------------------------------------
 *
 * ts_cache.c
 *	  Tsearch 相关对象缓存。
 *
 * Tsearch 性能对解析器、词典和映射的性能非常敏
 * 感，因此查找应当尽可能多地
 * 被缓存。
 *
 * 一旦某后端为特定 TS 对象 OID 创建了缓存条目，该条
 * 目将在后端生命周期内一直存在；因此在做可能导
 * 致识别出缓存失效之事，仍持有一个指向缓存条目的
 * 指针是安全的。但注意，一旦发生缓存失效并重验，
 * 辅助信息可能被删除并在别处重新分配！
 * 这看起来不会是大问题，
 * 只要解析器和词典方法不尝试进行任何
 * 数据库访问。
 *
 *
 * Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/ts_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "tsearch/ts_cache.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


/*
 * MAXTOKENTYPE/MAXDICTSPERTT 是任意设定的上限，用于
 * lookup_ts_config_cache() 的工作空间。本可以通过让工
 * 作空间可动态扩容来避免硬编码上限，但似乎不
 * 值得这么麻烦。
 */
#define MAXTOKENTYPE	256
#define MAXDICTSPERTT	100


static HTAB *TSParserCacheHash = NULL;
static TSParserCacheEntry *lastUsedParser = NULL;

static HTAB *TSDictionaryCacheHash = NULL;
static TSDictionaryCacheEntry *lastUsedDictionary = NULL;

static HTAB *TSConfigCacheHash = NULL;
static TSConfigCacheEntry *lastUsedConfig = NULL;

/*
 * GUC default_text_search_config，以及当前配置 OID 的缓存
 */
char	   *TSCurrentConfig = NULL;

static Oid	TSCurrentConfigCache = InvalidOid;


/*
 * 我们用这个 syscache 回调来侦测 TS 目录条目何时发生
 * 可见更改，无论来自我们自己后端还是其他后端。
 *
 * 原则上本可以只刷写发生更改的那个缓存条目，但
 * 考虑到 TS 配置更改多半不频繁，为此费神似乎不
 * 得；我们只管刷写相关哈希表的全部
 * 条目。
 *
 * 通过传入哈希表地址作为"arg"，同一函数可用于所
 * 有 TS 缓存。
 */
static void
InvalidateTSCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	HTAB	   *hash = (HTAB *) DatumGetPointer(arg);
	HASH_SEQ_STATUS status;
	TSAnyCacheEntry *entry;

	hash_seq_init(&status, hash);
	while ((entry = (TSAnyCacheEntry *) hash_seq_search(&status)) != NULL)
		entry->isvalid = false;

	/* 若为 pg_ts_config 还要失效当前配置缓存 */
	if (hash == TSConfigCacheHash)
		TSCurrentConfigCache = InvalidOid;
}

/*
 * 取解析器缓存条目
 */
TSParserCacheEntry *
lookup_ts_parser_cache(Oid prsId)
{
	TSParserCacheEntry *entry;

	if (TSParserCacheHash == NULL)
	{
		/* 首次运行时：初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TSParserCacheEntry);
		TSParserCacheHash = hash_create("Tsearch parser cache", 4,
										&ctl, HASH_ELEM | HASH_BLOBS);
		/* pg_ts_parser 更改时刷写缓存 */
		CacheRegisterSyscacheCallback(TSPARSEROID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSParserCacheHash));

		/* 同时确保 CacheMemoryContext 存在 */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* 检查单条目缓存 */
	if (lastUsedParser && lastUsedParser->prsId == prsId &&
		lastUsedParser->isvalid)
		return lastUsedParser;

	/* 尝试查找现有条目 */
	entry = (TSParserCacheEntry *) hash_search(TSParserCacheHash,
											   &prsId,
											   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * 若没找到就想新建。但先要查找该对象以确保 OID
		 * 是真实的。
		 */
		HeapTuple	tp;
		Form_pg_ts_parser prs;

		tp = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(prsId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for text search parser %u",
				 prsId);
		prs = (Form_pg_ts_parser) GETSTRUCT(tp);

		/*
		 * 健全检查
		 */
		if (!OidIsValid(prs->prsstart))
			elog(ERROR, "text search parser %u has no prsstart method", prsId);
		if (!OidIsValid(prs->prstoken))
			elog(ERROR, "text search parser %u has no prstoken method", prsId);
		if (!OidIsValid(prs->prsend))
			elog(ERROR, "text search parser %u has no prsend method", prsId);

		if (entry == NULL)
		{
			bool		found;

			/* 现在创建缓存条目 */
			entry = (TSParserCacheEntry *)
				hash_search(TSParserCacheHash, &prsId, HASH_ENTER, &found);
			Assert(!found);		/* 刚才它还不在那里 */
		}

		MemSet(entry, 0, sizeof(TSParserCacheEntry));
		entry->prsId = prsId;
		entry->startOid = prs->prsstart;
		entry->tokenOid = prs->prstoken;
		entry->endOid = prs->prsend;
		entry->headlineOid = prs->prsheadline;
		entry->lextypeOid = prs->prslextype;

		ReleaseSysCache(tp);

		fmgr_info_cxt(entry->startOid, &entry->prsstart, CacheMemoryContext);
		fmgr_info_cxt(entry->tokenOid, &entry->prstoken, CacheMemoryContext);
		fmgr_info_cxt(entry->endOid, &entry->prsend, CacheMemoryContext);
		if (OidIsValid(entry->headlineOid))
			fmgr_info_cxt(entry->headlineOid, &entry->prsheadline,
						  CacheMemoryContext);

		entry->isvalid = true;
	}

	lastUsedParser = entry;

	return entry;
}

/*
 * 获取词典缓存条目
 */
TSDictionaryCacheEntry *
lookup_ts_dictionary_cache(Oid dictId)
{
	TSDictionaryCacheEntry *entry;

	if (TSDictionaryCacheHash == NULL)
	{
		/* 首次运行时：初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TSDictionaryCacheEntry);
		TSDictionaryCacheHash = hash_create("Tsearch dictionary cache", 8,
											&ctl, HASH_ELEM | HASH_BLOBS);
		/* pg_ts_dict 和 pg_ts_template 更改时刷写缓存 */
		CacheRegisterSyscacheCallback(TSDICTOID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSDictionaryCacheHash));
		CacheRegisterSyscacheCallback(TSTEMPLATEOID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSDictionaryCacheHash));

		/* 同时确保 CacheMemoryContext 存在 */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* 检查单条目缓存 */
	if (lastUsedDictionary && lastUsedDictionary->dictId == dictId &&
		lastUsedDictionary->isvalid)
		return lastUsedDictionary;

	/* 尝试查找现有条目 */
	entry = (TSDictionaryCacheEntry *) hash_search(TSDictionaryCacheHash,
												   &dictId,
												   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * 若没找到就想新建。但先要查找该对象以确保 OID
		 * 是真实的。
		 */
		HeapTuple	tpdict,
					tptmpl;
		Form_pg_ts_dict dict;
		Form_pg_ts_template template;
		MemoryContext saveCtx;

		tpdict = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
		if (!HeapTupleIsValid(tpdict))
			elog(ERROR, "cache lookup failed for text search dictionary %u",
				 dictId);
		dict = (Form_pg_ts_dict) GETSTRUCT(tpdict);

		/*
		 * 健全检查
		 */
		if (!OidIsValid(dict->dicttemplate))
			elog(ERROR, "text search dictionary %u has no template", dictId);

		/*
		 * 取回词典的模板
		 */
		tptmpl = SearchSysCache1(TSTEMPLATEOID,
								 ObjectIdGetDatum(dict->dicttemplate));
		if (!HeapTupleIsValid(tptmpl))
			elog(ERROR, "cache lookup failed for text search template %u",
				 dict->dicttemplate);
		template = (Form_pg_ts_template) GETSTRUCT(tptmpl);

		/*
		 * 健全检查
		 */
		if (!OidIsValid(template->tmpllexize))
			elog(ERROR, "text search template %u has no lexize method",
				 template->tmpllexize);

		if (entry == NULL)
		{
			bool		found;

			/* 现在创建缓存条目 */
			entry = (TSDictionaryCacheEntry *)
				hash_search(TSDictionaryCacheHash,
							&dictId,
							HASH_ENTER, &found);
			Assert(!found);		/* 刚才它还不在那里 */

			/* 首次运行时创建私有内存上下文 */
			saveCtx = AllocSetContextCreate(CacheMemoryContext,
											"TS dictionary",
											ALLOCSET_SMALL_SIZES);
			MemoryContextCopyAndSetIdentifier(saveCtx, NameStr(dict->dictname));
		}
		else
		{
			/* 清空现有条目的私有上下文 */
			saveCtx = entry->dictCtx;
			/* 重置期间别让上下文的 ident 指针悬空 */
			MemoryContextSetIdentifier(saveCtx, NULL);
			MemoryContextReset(saveCtx);
			MemoryContextCopyAndSetIdentifier(saveCtx, NameStr(dict->dictname));
		}

		MemSet(entry, 0, sizeof(TSDictionaryCacheEntry));
		entry->dictId = dictId;
		entry->dictCtx = saveCtx;

		entry->lexizeOid = template->tmpllexize;

		if (OidIsValid(template->tmplinit))
		{
			List	   *dictoptions;
			Datum		opt;
			bool		isnull;
			MemoryContext oldcontext;

			/*
			 * Init 方法运行于词典私有内存上下文，选项
			 * 也存放在那里。
			 */
			oldcontext = MemoryContextSwitchTo(entry->dictCtx);

			opt = SysCacheGetAttr(TSDICTOID, tpdict,
								  Anum_pg_ts_dict_dictinitoption,
								  &isnull);
			if (isnull)
				dictoptions = NIL;
			else
				dictoptions = deserialize_deflist(opt);

			entry->dictData =
				DatumGetPointer(OidFunctionCall1(template->tmplinit,
												 PointerGetDatum(dictoptions)));

			MemoryContextSwitchTo(oldcontext);
		}

		ReleaseSysCache(tptmpl);
		ReleaseSysCache(tpdict);

		fmgr_info_cxt(entry->lexizeOid, &entry->lexize, entry->dictCtx);

		entry->isvalid = true;
	}

	lastUsedDictionary = entry;

	return entry;
}

/*
 * 初始化配置缓存并准备回调。这段逻辑从
 *  lookup_ts_config_cache 中拆出，因为我们也需要在缓存
 * TSCurrentConfigCache 前激活。
 */
static void
init_ts_config_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TSConfigCacheEntry);
	TSConfigCacheHash = hash_create("Tsearch configuration cache", 16,
									&ctl, HASH_ELEM | HASH_BLOBS);
	/* pg_ts_config 和 pg_ts_config_map 更改时刷写缓存 */
	CacheRegisterSyscacheCallback(TSCONFIGOID, InvalidateTSCacheCallBack,
								  PointerGetDatum(TSConfigCacheHash));
	CacheRegisterSyscacheCallback(TSCONFIGMAP, InvalidateTSCacheCallBack,
								  PointerGetDatum(TSConfigCacheHash));

	/* 同时确保 CacheMemoryContext 存在 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();
}

/*
 * 获取配置缓存条目
 */
TSConfigCacheEntry *
lookup_ts_config_cache(Oid cfgId)
{
	TSConfigCacheEntry *entry;

	if (TSConfigCacheHash == NULL)
	{
		/* 首次运行时：初始化哈希表 */
		init_ts_config_cache();
	}

	/* 检查单条目缓存 */
	if (lastUsedConfig && lastUsedConfig->cfgId == cfgId &&
		lastUsedConfig->isvalid)
		return lastUsedConfig;

	/* 尝试查找现有条目 */
	entry = (TSConfigCacheEntry *) hash_search(TSConfigCacheHash,
											   &cfgId,
											   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * 若没找到就想新建。但先要查找该对象以确保 OID
		 * 是真实的。
		 */
		HeapTuple	tp;
		Form_pg_ts_config cfg;
		Relation	maprel;
		Relation	mapidx;
		ScanKeyData mapskey;
		SysScanDesc mapscan;
		HeapTuple	maptup;
		ListDictionary maplists[MAXTOKENTYPE + 1];
		Oid			mapdicts[MAXDICTSPERTT];
		int			maxtokentype;
		int			ndicts;
		int			i;

		tp = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 cfgId);
		cfg = (Form_pg_ts_config) GETSTRUCT(tp);

		/*
		 * 健全检查
		 */
		if (!OidIsValid(cfg->cfgparser))
			elog(ERROR, "text search configuration %u has no parser", cfgId);

		if (entry == NULL)
		{
			bool		found;

			/* 现在创建缓存条目 */
			entry = (TSConfigCacheEntry *)
				hash_search(TSConfigCacheHash,
							&cfgId,
							HASH_ENTER, &found);
			Assert(!found);		/* 刚才它还不在那里 */
		}
		else
		{
			/* 清理旧内容 */
			if (entry->map)
			{
				for (i = 0; i < entry->lenmap; i++)
					if (entry->map[i].dictIds)
						pfree(entry->map[i].dictIds);
				pfree(entry->map);
			}
		}

		MemSet(entry, 0, sizeof(TSConfigCacheEntry));
		entry->cfgId = cfgId;
		entry->prsId = cfg->cfgparser;

		ReleaseSysCache(tp);

		/*
		 * 扫描 pg_ts_config_map 收集每个令牌类型的词典列表
		 *
		 * 由于索引建在（mapcfg, maptokentype, mapseqno）上，
		 * 条目按 maptokentype 排序，且每个令牌类型内按
		 * mapseqno 顺序出现，即便我们并未显式要求如此。
		 */
		MemSet(maplists, 0, sizeof(maplists));
		maxtokentype = 0;
		ndicts = 0;

		ScanKeyInit(&mapskey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));

		maprel = table_open(TSConfigMapRelationId, AccessShareLock);
		mapidx = index_open(TSConfigMapIndexId, AccessShareLock);
		mapscan = systable_beginscan_ordered(maprel, mapidx,
											 NULL, 1, &mapskey);

		while ((maptup = systable_getnext_ordered(mapscan, ForwardScanDirection)) != NULL)
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);
			int			toktype = cfgmap->maptokentype;

			if (toktype <= 0 || toktype > MAXTOKENTYPE)
				elog(ERROR, "maptokentype value %d is out of range", toktype);
			if (toktype < maxtokentype)
				elog(ERROR, "maptokentype entries are out of order");
			if (toktype > maxtokentype)
			{
				/* 开始新的令牌类型，但先保存先前的数据 */
				if (ndicts > 0)
				{
					maplists[maxtokentype].len = ndicts;
					maplists[maxtokentype].dictIds = (Oid *)
						MemoryContextAlloc(CacheMemoryContext,
										   sizeof(Oid) * ndicts);
					memcpy(maplists[maxtokentype].dictIds, mapdicts,
						   sizeof(Oid) * ndicts);
				}
				maxtokentype = toktype;
				mapdicts[0] = cfgmap->mapdict;
				ndicts = 1;
			}
			else
			{
				/* 当前令牌类型的后续数据 */
				if (ndicts >= MAXDICTSPERTT)
					elog(ERROR, "too many pg_ts_config_map entries for one token type");
				mapdicts[ndicts++] = cfgmap->mapdict;
			}
		}

		systable_endscan_ordered(mapscan);
		index_close(mapidx, AccessShareLock);
		table_close(maprel, AccessShareLock);

		if (ndicts > 0)
		{
			/* 保存最后一个令牌类型的词典 */
			maplists[maxtokentype].len = ndicts;
			maplists[maxtokentype].dictIds = (Oid *)
				MemoryContextAlloc(CacheMemoryContext,
								   sizeof(Oid) * ndicts);
			memcpy(maplists[maxtokentype].dictIds, mapdicts,
				   sizeof(Oid) * ndicts);
			/* 再保存整体映射 */
			entry->lenmap = maxtokentype + 1;
			entry->map = (ListDictionary *)
				MemoryContextAlloc(CacheMemoryContext,
								   sizeof(ListDictionary) * entry->lenmap);
			memcpy(entry->map, maplists,
				   sizeof(ListDictionary) * entry->lenmap);
		}

		entry->isvalid = true;
	}

	lastUsedConfig = entry;

	return entry;
}


/*---------------------------------------------------
 * GUC 变量 "default_text_search_config"
 *---------------------------------------------------
 */

Oid
getTSCurrentConfig(bool emitError)
{
	List	   *namelist;

	/* 若已有缓存值则返回它 */
	if (OidIsValid(TSCurrentConfigCache))
		return TSCurrentConfigCache;

	/* 若 GUC 尚未设置则失败 */
	if (TSCurrentConfig == NULL || *TSCurrentConfig == '\0')
	{
		if (emitError)
			elog(ERROR, "text search configuration isn't set");
		else
			return InvalidOid;
	}

	if (TSConfigCacheHash == NULL)
	{
		/* 首次运行时：初始化 tsconfig 失效回调 */
		init_ts_config_cache();
	}

	/* 查找该配置 */
	if (emitError)
	{
		namelist = stringToQualifiedNameList(TSCurrentConfig, NULL);
		TSCurrentConfigCache = get_ts_config_oid(namelist, false);
	}
	else
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};

		namelist = stringToQualifiedNameList(TSCurrentConfig,
											 (Node *) &escontext);
		if (namelist != NIL)
			TSCurrentConfigCache = get_ts_config_oid(namelist, true);
		else
			TSCurrentConfigCache = InvalidOid;	/*名字表语法有误*/
	}

	return TSCurrentConfigCache;
}

/* default_text_search_config 的 GUC check_hook */
bool
check_default_text_search_config(char **newval, void **extra, GucSource source)
{
	/*
	 * 若不在事务中或未连接数据库，则无法执行验证
	 * 配置名称所必需的目录访问。
	 * 只能凭信任接受。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		List	   *namelist;
		Oid			cfgId;
		HeapTuple	tuple;
		Form_pg_ts_config cfg;
		char	   *buf;

		namelist = stringToQualifiedNameList(*newval,
											 (Node *) &escontext);
		if (namelist != NIL)
			cfgId = get_ts_config_oid(namelist, true);
		else
			cfgId = InvalidOid; /*名字表语法有误*/

		/*
		 *  source == PGC_S_TEST 时，不要对不存在的配
		 * 置抛硬错误，只发 NOTICE。参见 guc.h 中注释。
		 */
		if (!OidIsValid(cfgId))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("text search configuration \"%s\" does not exist", *newval)));
				return true;
			}
			else
				return false;
		}

		/*
		 * 把实际存储的值改成完全限定形式，以确保之后
		 * search_path 的更改不影响它。
		 */
		tuple = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 cfgId);
		cfg = (Form_pg_ts_config) GETSTRUCT(tuple);

		buf = quote_qualified_identifier(get_namespace_name(cfg->cfgnamespace),
										 NameStr(cfg->cfgname));

		ReleaseSysCache(tuple);

		/* GUC 要求用 guc_malloc 而非 palloc */
		guc_free(*newval);
		*newval = guc_strdup(LOG, buf);
		pfree(buf);
		if (!*newval)
			return false;
	}

	return true;
}

/* default_text_search_config 的 GUC assign_hook */
void
assign_default_text_search_config(const char *newval, void *extra)
{
	/* 直接重置缓存以强制首次使用时查找 */
	TSCurrentConfigCache = InvalidOid;
}
