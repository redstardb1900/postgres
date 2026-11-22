/*-------------------------------------------------------------------------
 *
 * resowner.h
 *	  POSTGRES 资源所有者定义。
 *
 * 查询寿命资源通过与 ResourceOwner 对象关联来
 * 跟踪。这提供了在适当时机释放此类资源的简单
 * 机制。详见 utils/resowner/README。
 * 见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/resowner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESOWNER_H
#define RESOWNER_H


/*
 * ResourceOwner 仅在 resowner.c 内可知为不透明结构。
 * resowner.c.
 */
typedef struct ResourceOwnerData *ResourceOwner;


/*
 * 全局已知 ResourceOwners
 */
extern PGDLLIMPORT ResourceOwner CurrentResourceOwner;
extern PGDLLIMPORT ResourceOwner CurTransactionResourceOwner;
extern PGDLLIMPORT ResourceOwner TopTransactionResourceOwner;
extern PGDLLIMPORT ResourceOwner AuxProcessResourceOwner;

/*
 * 资源释放分三阶段：锁前、锁、锁后。锁前阶段
 * 须释放对其他后端可见的资源（如钉住的缓冲）；
 * 这确保当我们释放其他后端可能正在等待的锁时，
 * 对方会看到我们已完全退出事务。锁后阶段用于
 * 后端内部清理。
 * 见上。
 */
typedef enum
{
	RESOURCE_RELEASE_BEFORE_LOCKS,
	RESOURCE_RELEASE_LOCKS,
	RESOURCE_RELEASE_AFTER_LOCKS
} ResourceReleasePhase;

/*
 *	动态加载模块可在 ResourceOwnerRelease 期间
 *	借此形式回调取得控制。
 */
typedef void (*ResourceReleaseCallback) (ResourceReleasePhase phase,
										 bool isCommit,
										 bool isTopLevel,
										 void *arg);


/*
 * resowner.c 中的函数
 */

/* 通用例程 */
extern ResourceOwner ResourceOwnerCreate(ResourceOwner parent,
										 const char *name);
extern void ResourceOwnerRelease(ResourceOwner owner,
								 ResourceReleasePhase phase,
								 bool isCommit,
								 bool isTopLevel);
extern void ResourceOwnerReleaseAllPlanCacheRefs(ResourceOwner owner);
extern void ResourceOwnerDelete(ResourceOwner owner);
extern ResourceOwner ResourceOwnerGetParent(ResourceOwner owner);
extern void ResourceOwnerNewParent(ResourceOwner owner,
								   ResourceOwner newparent);
extern void RegisterResourceReleaseCallback(ResourceReleaseCallback callback,
											void *arg);
extern void UnregisterResourceReleaseCallback(ResourceReleaseCallback callback,
											  void *arg);
extern void CreateAuxProcessResourceOwner(void);
extern void ReleaseAuxProcessResources(bool isCommit);

#endif							/* RESOWNER_H */
