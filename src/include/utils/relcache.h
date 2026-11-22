/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  关系描述符缓存定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELCACHE_H
#define RELCACHE_H

#include "access/tupdesc.h"
#include "common/relpath.h"
#include "nodes/bitmapset.h"


/*
 * relcache 初始化 file 名，用于加速后端启动
 */
#define RELCACHE_INIT_FILENAME	"pg_internal.init"

typedef struct RelationData *Relation;

/* ----------------
 *		RelationPtr 在执行器中用于支持索引扫描，
 *		此时须在数组中跟踪若干索引关系。-cim 9/10/89
 *		见上。
 * ----------------
 */
typedef Relation *RelationPtr;

/*
 * 打开（查找）与关闭 relcache 条目的例程
 */
extern Relation RelationIdGetRelation(Oid relationId);
extern void RelationClose(Relation relation);

/*
 * 计算/检索额外缓存信息的例程
 */
extern List *RelationGetFKeyList(Relation relation);
extern List *RelationGetIndexList(Relation relation);
extern List *RelationGetStatExtList(Relation relation);
extern Oid	RelationGetPrimaryKeyIndex(Relation relation);
extern Oid	RelationGetReplicaIndex(Relation relation);
extern List *RelationGetIndexExpressions(Relation relation);
extern List *RelationGetDummyIndexExpressions(Relation relation);
extern List *RelationGetIndexPredicate(Relation relation);
extern Datum *RelationGetIndexRawAttOptions(Relation indexrel);
extern bytea **RelationGetIndexAttOptions(Relation relation, bool copy);

/*
 * RelationGetIndexAttrBitmap 返回哪组列。
 */
typedef enum IndexAttrBitmapKind
{
	INDEX_ATTR_BITMAP_KEY,
	INDEX_ATTR_BITMAP_PRIMARY_KEY,
	INDEX_ATTR_BITMAP_IDENTITY_KEY,
	INDEX_ATTR_BITMAP_HOT_BLOCKING,
	INDEX_ATTR_BITMAP_SUMMARIZED
} IndexAttrBitmapKind;

extern Bitmapset *RelationGetIndexAttrBitmap(Relation relation,
											 IndexAttrBitmapKind attrKind);

extern Bitmapset *RelationGetIdentityKeyBitmap(Relation relation);

extern void RelationGetExclusionInfo(Relation indexRelation,
									 Oid **operators,
									 Oid **procs,
									 uint16 **strategies);

extern void RelationInitIndexAccessInfo(Relation relation);

/* 调用方须包含 pg_publication.h */
struct PublicationDesc;
extern void RelationBuildPublicationDesc(Relation relation,
										 struct PublicationDesc *pubdesc);

extern void RelationInitTableAccessMethod(Relation relation);

/*
 * 支持 ereport() 报告关系相关错误的例程
 */
extern int	errtable(Relation rel);
extern int	errtablecol(Relation rel, int attnum);
extern int	errtablecolname(Relation rel, const char *colname);
extern int	errtableconstraint(Relation rel, const char *conname);

/*
 * 后端启动例程
 */
extern void RelationCacheInitialize(void);
extern void RelationCacheInitializePhase2(void);
extern void RelationCacheInitializePhase3(void);

/*
 * 为即将创建的关系建立 relcache 条目的例程
 */
extern Relation RelationBuildLocalRelation(const char *relname,
										   Oid relnamespace,
										   TupleDesc tupDesc,
										   Oid relid,
										   Oid accessmtd,
										   RelFileNumber relfilenumber,
										   Oid reltablespace,
										   bool shared_relation,
										   bool mapped_relation,
										   char relpersistence,
										   char relkind);

/*
 * 管理给关系分配新 relfilenumber 的例程
 */
extern void RelationSetNewRelfilenumber(Relation relation, char persistence);
extern void RelationAssumeNewRelfilelocator(Relation relation);

/*
 * 在各种场景下刷新/重建 relcache 条目的例程
 */
extern void RelationForgetRelation(Oid rid);

extern void RelationCacheInvalidateEntry(Oid relationId);

extern void RelationCacheInvalidate(bool debug_discard);

extern void RelationCloseSmgrByOid(Oid relationId);

#ifdef USE_ASSERT_CHECKING
extern void AssertPendingSyncs_RelationCache(void);
#else
#define AssertPendingSyncs_RelationCache() do {} while (0)
#endif
extern void AtEOXact_RelationCache(bool isCommit);
extern void AtEOSubXact_RelationCache(bool isCommit, SubTransactionId mySubid,
									  SubTransactionId parentSubid);

/*
 * 协助管理重建 relcache 初始化文件的例程
 */
extern bool RelationIdIsInInitFile(Oid relationId);
extern void RelationCacheInitFilePreInvalidate(void);
extern void RelationCacheInitFilePostInvalidate(void);
extern void RelationCacheInitFileRemove(void);

/* 仅应由 relcache.c 与 catcache.c 使用 */
extern PGDLLIMPORT bool criticalRelcachesBuilt;

/* 仅应由 relcache.c 与 postinit.c 使用 */
extern PGDLLIMPORT bool criticalSharedRelcachesBuilt;

#endif							/* RELCACHE_H */
