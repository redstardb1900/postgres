/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  系统目录缓存定义。
 *
 * 另见 lsyscache.h，它为常见缓存查找操作提供
 * 便捷例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/syscache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "access/attnum.h"
#include "access/htup.h"
/* 有意不在此包含 utils/catcache.h */

/*
 *		SysCache 标识符。
 *
 *		这些标识符的次序须与 syscache.c 中
 *		cacheinfo[] 数组条目次序一致。
 *		保持字母序（重编号只费一次后端重建）。
 *		见上。
 */

enum SysCacheIdentifier
{
	AGGFNOID = 0,
	AMNAME,
	AMOID,
	AMOPOPID,
	AMOPSTRATEGY,
	AMPROCNUM,
	ATTNAME,
	ATTNUM,
	AUTHMEMMEMROLE,
	AUTHMEMROLEMEM,
	AUTHNAME,
	AUTHOID,
	CASTSOURCETARGET,
	CLAAMNAMENSP,
	CLAOID,
	COLLNAMEENCNSP,
	COLLOID,
	CONDEFAULT,
	CONNAMENSP,
	CONSTROID,
	CONVOID,
	DATABASEOID,
	DEFACLROLENSPOBJ,
	ENUMOID,
	ENUMTYPOIDNAME,
	EVENTTRIGGERNAME,
	EVENTTRIGGEROID,
	FOREIGNDATAWRAPPERNAME,
	FOREIGNDATAWRAPPEROID,
	FOREIGNSERVERNAME,
	FOREIGNSERVEROID,
	FOREIGNTABLEREL,
	INDEXRELID,
	LANGNAME,
	LANGOID,
	NAMESPACENAME,
	NAMESPACEOID,
	OPERNAMENSP,
	OPEROID,
	OPFAMILYAMNAMENSP,
	OPFAMILYOID,
	PARAMETERACLNAME,
	PARAMETERACLOID,
	PARTRELID,
	PROCNAMEARGSNSP,
	PROCOID,
	PUBLICATIONNAME,
	PUBLICATIONNAMESPACE,
	PUBLICATIONNAMESPACEMAP,
	PUBLICATIONOID,
	PUBLICATIONREL,
	PUBLICATIONRELMAP,
	RANGEMULTIRANGE,
	RANGETYPE,
	RELNAMENSP,
	RELOID,
	REPLORIGIDENT,
	REPLORIGNAME,
	RULERELNAME,
	SEQRELID,
	STATEXTDATASTXOID,
	STATEXTNAMENSP,
	STATEXTOID,
	STATRELATTINH,
	SUBSCRIPTIONNAME,
	SUBSCRIPTIONOID,
	SUBSCRIPTIONRELMAP,
	TABLESPACEOID,
	TRFOID,
	TRFTYPELANG,
	TSCONFIGMAP,
	TSCONFIGNAMENSP,
	TSCONFIGOID,
	TSDICTNAMENSP,
	TSDICTOID,
	TSPARSERNAMENSP,
	TSPARSEROID,
	TSTEMPLATENAMENSP,
	TSTEMPLATEOID,
	TYPENAMENSP,
	TYPEOID,
	USERMAPPINGOID,
	USERMAPPINGUSERSERVER

#define SysCacheSize (USERMAPPINGUSERSERVER + 1)
};

extern void InitCatalogCache(void);
extern void InitCatalogCachePhase2(void);

extern HeapTuple SearchSysCache(int cacheId,
								Datum key1, Datum key2, Datum key3, Datum key4);

/*
 * 鼓励使用按参数个数特化的编号。它们更快，且
 * 使调用方免受最大键数变化的影响。
 */
extern HeapTuple SearchSysCache1(int cacheId,
								 Datum key1);
extern HeapTuple SearchSysCache2(int cacheId,
								 Datum key1, Datum key2);
extern HeapTuple SearchSysCache3(int cacheId,
								 Datum key1, Datum key2, Datum key3);
extern HeapTuple SearchSysCache4(int cacheId,
								 Datum key1, Datum key2, Datum key3, Datum key4);

extern void ReleaseSysCache(HeapTuple tuple);

extern HeapTuple SearchSysCacheLocked1(int cacheId,
									   Datum key1);

/* 便捷例程 */
extern HeapTuple SearchSysCacheCopy(int cacheId,
									Datum key1, Datum key2, Datum key3, Datum key4);
extern HeapTuple SearchSysCacheLockedCopy1(int cacheId,
										   Datum key1);
extern bool SearchSysCacheExists(int cacheId,
								 Datum key1, Datum key2, Datum key3, Datum key4);
extern Oid	GetSysCacheOid(int cacheId, AttrNumber oidcol,
						   Datum key1, Datum key2, Datum key3, Datum key4);

extern HeapTuple SearchSysCacheAttName(Oid relid, const char *attname);
extern HeapTuple SearchSysCacheCopyAttName(Oid relid, const char *attname);
extern bool SearchSysCacheExistsAttName(Oid relid, const char *attname);

extern HeapTuple SearchSysCacheAttNum(Oid relid, int16 attnum);
extern HeapTuple SearchSysCacheCopyAttNum(Oid relid, int16 attnum);

extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
							 AttrNumber attributeNumber, bool *isNull);

extern Datum SysCacheGetAttrNotNull(int cacheId, HeapTuple tup,
									AttrNumber attributeNumber);

extern uint32 GetSysCacheHashValue(int cacheId,
								   Datum key1, Datum key2, Datum key3, Datum key4);

/* 列表搜索接口。使用者还须引入 catcache.h */
struct catclist;
extern struct catclist *SearchSysCacheList(int cacheId, int nkeys,
										   Datum key1, Datum key2, Datum key3);

extern void SysCacheInvalidate(int cacheId, uint32 hashValue);

extern bool RelationInvalidatesSnapshotsOnly(Oid relid);
extern bool RelationHasSysCache(Oid relid);
extern bool RelationSupportsSysCache(Oid relid);

/*
 * 鼓励用下列宏而非直接调用对应函数，可使调用
 * 方免受最大键数变化的影响。
 * 见上。
 */
#define SearchSysCacheCopy1(cacheId, key1) \
	SearchSysCacheCopy(cacheId, key1, 0, 0, 0)
#define SearchSysCacheCopy2(cacheId, key1, key2) \
	SearchSysCacheCopy(cacheId, key1, key2, 0, 0)
#define SearchSysCacheCopy3(cacheId, key1, key2, key3) \
	SearchSysCacheCopy(cacheId, key1, key2, key3, 0)
#define SearchSysCacheCopy4(cacheId, key1, key2, key3, key4) \
	SearchSysCacheCopy(cacheId, key1, key2, key3, key4)

#define SearchSysCacheExists1(cacheId, key1) \
	SearchSysCacheExists(cacheId, key1, 0, 0, 0)
#define SearchSysCacheExists2(cacheId, key1, key2) \
	SearchSysCacheExists(cacheId, key1, key2, 0, 0)
#define SearchSysCacheExists3(cacheId, key1, key2, key3) \
	SearchSysCacheExists(cacheId, key1, key2, key3, 0)
#define SearchSysCacheExists4(cacheId, key1, key2, key3, key4) \
	SearchSysCacheExists(cacheId, key1, key2, key3, key4)

#define GetSysCacheOid1(cacheId, oidcol, key1) \
	GetSysCacheOid(cacheId, oidcol, key1, 0, 0, 0)
#define GetSysCacheOid2(cacheId, oidcol, key1, key2) \
	GetSysCacheOid(cacheId, oidcol, key1, key2, 0, 0)
#define GetSysCacheOid3(cacheId, oidcol, key1, key2, key3) \
	GetSysCacheOid(cacheId, oidcol, key1, key2, key3, 0)
#define GetSysCacheOid4(cacheId, oidcol, key1, key2, key3, key4) \
	GetSysCacheOid(cacheId, oidcol, key1, key2, key3, key4)

#define GetSysCacheHashValue1(cacheId, key1) \
	GetSysCacheHashValue(cacheId, key1, 0, 0, 0)
#define GetSysCacheHashValue2(cacheId, key1, key2) \
	GetSysCacheHashValue(cacheId, key1, key2, 0, 0)
#define GetSysCacheHashValue3(cacheId, key1, key2, key3) \
	GetSysCacheHashValue(cacheId, key1, key2, key3, 0)
#define GetSysCacheHashValue4(cacheId, key1, key2, key3, key4) \
	GetSysCacheHashValue(cacheId, key1, key2, key3, key4)

#define SearchSysCacheList1(cacheId, key1) \
	SearchSysCacheList(cacheId, 1, key1, 0, 0)
#define SearchSysCacheList2(cacheId, key1, key2) \
	SearchSysCacheList(cacheId, 2, key1, key2, 0)
#define SearchSysCacheList3(cacheId, key1, key2, key3) \
	SearchSysCacheList(cacheId, 3, key1, key2, key3)

#define ReleaseSysCacheList(x)	ReleaseCatCacheList(x)

#endif							/* SYSCACHE_H */
