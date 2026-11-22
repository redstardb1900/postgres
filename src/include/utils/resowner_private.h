/*-------------------------------------------------------------------------
 *
 * resowner_private.h
 *	  POSTGRES 资源所有者私有定义。
 *
 * 详见 utils/resowner/README。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/resowner_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESOWNER_PRIVATE_H
#define RESOWNER_PRIVATE_H

#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "utils/catcache.h"
#include "utils/plancache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"


/* 缓冲区引用计数管理支持 */
extern void ResourceOwnerEnlargeBuffers(ResourceOwner owner);
extern void ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer);
extern void ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer);

/* 进行中 IO 管理支持 */
extern void ResourceOwnerEnlargeBufferIOs(ResourceOwner owner);
extern void ResourceOwnerRememberBufferIO(ResourceOwner owner, Buffer buffer);
extern void ResourceOwnerForgetBufferIO(ResourceOwner owner, Buffer buffer);

/* 本地锁管理支持 */
extern void ResourceOwnerRememberLock(ResourceOwner owner, LOCALLOCK *locallock);
extern void ResourceOwnerForgetLock(ResourceOwner owner, LOCALLOCK *locallock);

/* catcache 引用计数管理支持 */
extern void ResourceOwnerEnlargeCatCacheRefs(ResourceOwner owner);
extern void ResourceOwnerRememberCatCacheRef(ResourceOwner owner,
											 HeapTuple tuple);
extern void ResourceOwnerForgetCatCacheRef(ResourceOwner owner,
										   HeapTuple tuple);
extern void ResourceOwnerEnlargeCatCacheListRefs(ResourceOwner owner);
extern void ResourceOwnerRememberCatCacheListRef(ResourceOwner owner,
												 CatCList *list);
extern void ResourceOwnerForgetCatCacheListRef(ResourceOwner owner,
											   CatCList *list);

/* relcache 引用计数管理支持 */
extern void ResourceOwnerEnlargeRelationRefs(ResourceOwner owner);
extern void ResourceOwnerRememberRelationRef(ResourceOwner owner,
											 Relation rel);
extern void ResourceOwnerForgetRelationRef(ResourceOwner owner,
										   Relation rel);

/* plancache 引用计数管理支持 */
extern void ResourceOwnerEnlargePlanCacheRefs(ResourceOwner owner);
extern void ResourceOwnerRememberPlanCacheRef(ResourceOwner owner,
											  CachedPlan *plan);
extern void ResourceOwnerForgetPlanCacheRef(ResourceOwner owner,
											CachedPlan *plan);

/* tupledesc 引用计数管理支持 */
extern void ResourceOwnerEnlargeTupleDescs(ResourceOwner owner);
extern void ResourceOwnerRememberTupleDesc(ResourceOwner owner,
										   TupleDesc tupdesc);
extern void ResourceOwnerForgetTupleDesc(ResourceOwner owner,
										 TupleDesc tupdesc);

/* snapshot 引用计数管理支持 */
extern void ResourceOwnerEnlargeSnapshots(ResourceOwner owner);
extern void ResourceOwnerRememberSnapshot(ResourceOwner owner,
										  Snapshot snapshot);
extern void ResourceOwnerForgetSnapshot(ResourceOwner owner,
										Snapshot snapshot);

/* 临时文件管理支持 */
extern void ResourceOwnerEnlargeFiles(ResourceOwner owner);
extern void ResourceOwnerRememberFile(ResourceOwner owner,
									  File file);
extern void ResourceOwnerForgetFile(ResourceOwner owner,
									File file);

/* 动态共享内存管理支持 */
extern void ResourceOwnerEnlargeDSMs(ResourceOwner owner);
extern void ResourceOwnerRememberDSM(ResourceOwner owner,
									 dsm_segment *);
extern void ResourceOwnerForgetDSM(ResourceOwner owner,
								   dsm_segment *);

/* JITContext 管理支持 */
extern void ResourceOwnerEnlargeJIT(ResourceOwner owner);
extern void ResourceOwnerRememberJIT(ResourceOwner owner,
									 Datum handle);
extern void ResourceOwnerForgetJIT(ResourceOwner owner,
								   Datum handle);

/* cryptohash 上下文管理支持 */
extern void ResourceOwnerEnlargeCryptoHash(ResourceOwner owner);
extern void ResourceOwnerRememberCryptoHash(ResourceOwner owner,
											Datum handle);
extern void ResourceOwnerForgetCryptoHash(ResourceOwner owner,
										  Datum handle);

/* HMAC 上下文管理支持 */
extern void ResourceOwnerEnlargeHMAC(ResourceOwner owner);
extern void ResourceOwnerRememberHMAC(ResourceOwner owner,
									  Datum handle);
extern void ResourceOwnerForgetHMAC(ResourceOwner owner,
									Datum handle);

#endif							/* RESOWNER_PRIVATE_H */
