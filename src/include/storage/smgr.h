/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  存储管理器开关的公共接口声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/smgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "lib/ilist.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * smgr.c 维护一张 SMgrRelation 对象表，它们本质是
 * 缓存的文件句柄。SMgrRelation 由 smgropen() 创建
 * （若不存在），并由 smgrclose() 销毁。注意二者均
 * 不意味着 I/O，只是创建或销毁哈希表条目。
 * （但 smgrclose() 可能释放关联资源，如 OS 级文件
 * descriptors.)
 *
 * SMgrRelation 可以有一个 "owner"，只是别处指向它的
 * 指针；若 SMgrRelation 被关闭，smgr.c 会清空该指针。
 * 我们用此避免 relcache 到 smgr 的悬空指针，而无需
 * 让 smgr 显式知晓 relcache。每个 SMgrRelation 不能有
 * 多于一个 "owner" 指针，但这就
 * 够用了。
 *
 * 没有 "owner" 的 SMgrRelations 视为短暂的，
 * 并在事务结束时删除。
 */
typedef struct SMgrRelationData
{
	/* rlocator 是哈希表查找键，故必须为首！ */
	RelFileLocatorBackend smgr_rlocator;	/* 关系物理标识 */

	/* 指向拥有者指针，无则 NULL */
	struct SMgrRelationData **smgr_owner;

	/*
	 * 下列字段在缓存刷写事件时复位为 InvalidBlockNumber，
	 * 并保存各分支上次已知大小。此信息目前仅在
	 * 恢复期间可靠，因为分支扩展没有缓存
	 * 失效。
	 */
	BlockNumber smgr_targblock; /* 当前插入目标块 */
	BlockNumber smgr_cached_nblocks[MAX_FORKNUM + 1];	/* 已知大小 */

	/* 将来此处或可有更多公共字段 */

	/*
	 * 以下字段拟为 smgr.c 及其子模块私有。
	 * 切勿从别处触碰。
	 */
	int			smgr_which;		/* 存储管理器选择器 */

	/*
	 * 供 md.c；每分支的已打开段数数组
	 * (md_num_open_segs) 与段本身 (md_seg_fds)。
	 */
	int			md_num_open_segs[MAX_FORKNUM + 1];
	struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];

	/* 若无主，则链入全部无主 SMgrRelations */
	dlist_node	node;
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;

#define SmgrIsTemp(smgr) \
	RelFileLocatorBackendIsTemp((smgr)->smgr_rlocator)

extern void smgrinit(void);
extern SMgrRelation smgropen(RelFileLocator rlocator, BackendId backend);
extern bool smgrexists(SMgrRelation reln, ForkNumber forknum);
extern void smgrsetowner(SMgrRelation *owner, SMgrRelation reln);
extern void smgrclearowner(SMgrRelation *owner, SMgrRelation reln);
extern void smgrclose(SMgrRelation reln);
extern void smgrcloseall(void);
extern void smgrcloserellocator(RelFileLocatorBackend rlocator);
extern void smgrrelease(SMgrRelation reln);
extern void smgrreleaseall(void);
extern void smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void smgrdosyncall(SMgrRelation *rels, int nrels);
extern void smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo);
extern void smgrextend(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void smgrzeroextend(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, int nblocks, bool skipFsync);
extern bool smgrprefetch(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum);
extern void smgrread(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, void *buffer);
extern void smgrwrite(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void smgrwriteback(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber smgrnblocks(SMgrRelation reln, ForkNumber forknum);
extern BlockNumber smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum);
extern void smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
						 BlockNumber *nblocks);
extern void smgrtruncate2(SMgrRelation reln, ForkNumber *forknum, int nforks,
						  BlockNumber *old_nblocks,
						  BlockNumber *nblocks);
extern void smgrimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void AtEOXact_SMgr(void);
extern bool ProcessBarrierSmgrRelease(void);

#endif							/* SMGR_H */
