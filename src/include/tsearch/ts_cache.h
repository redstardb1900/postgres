/*-------------------------------------------------------------------------
 *
 * ts_cache.h
 *	  全文检索对象缓存。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tsearch/ts_cache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TS_CACHE_H
#define TS_CACHE_H

#include "fmgr.h"


/*
 * 所有 TS*CacheEntry 结构须共享此公共头
 * (见 InvalidateTSCacheCallBack)
 */
typedef struct TSAnyCacheEntry
{
	Oid			objId;
	bool		isvalid;
} TSAnyCacheEntry;


typedef struct TSParserCacheEntry
{
	/*  prsId 为散列键且必须为首  */
	Oid			prsId;			/*  解析器 OID  */
	bool		isvalid;

	Oid			startOid;
	Oid			tokenOid;
	Oid			endOid;
	Oid			headlineOid;
	Oid			lextypeOid;

	/*
	 * 最常用解析器方法的预置 fmgr 调用
	 */
	FmgrInfo	prsstart;
	FmgrInfo	prstoken;
	FmgrInfo	prsend;
	FmgrInfo	prsheadline;
} TSParserCacheEntry;

typedef struct TSDictionaryCacheEntry
{
	/*  dictId 为散列键且必须为首  */
	Oid			dictId;
	bool		isvalid;

	/*  最频 fmgr 调用  */
	Oid			lexizeOid;
	FmgrInfo	lexize;

	MemoryContext dictCtx;		/* 。 */
	void	   *dictData;
} TSDictionaryCacheEntry;

typedef struct
{
	int			len;
	Oid		   *dictIds;
} ListDictionary;

typedef struct
{
	/*  cfgId 为散列键且必须为首  */
	Oid			cfgId;
	bool		isvalid;

	Oid			prsId;

	int			lenmap;
	ListDictionary *map;
} TSConfigCacheEntry;


/*
 * 当前配置的 GUC 变量
 */
extern PGDLLIMPORT char *TSCurrentConfig;


extern TSParserCacheEntry *lookup_ts_parser_cache(Oid prsId);
extern TSDictionaryCacheEntry *lookup_ts_dictionary_cache(Oid dictId);
extern TSConfigCacheEntry *lookup_ts_config_cache(Oid cfgId);

extern Oid	getTSCurrentConfig(bool emitError);

#endif							/* TS_CACHE_H */
