/*-------------------------------------------------------------------------
 *
 * gistxlog.h
 *	  gist xlog 例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gistxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_XLOG_H
#define GIST_XLOG_H

#include "access/gist.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

#define XLOG_GIST_PAGE_UPDATE		0x00
#define XLOG_GIST_DELETE			0x10	/* 删除叶上索引元组的
											 * 页*/
#define XLOG_GIST_PAGE_REUSE		0x20	/* 旧页即将从 FSM 被
											 * 重用*/
#define XLOG_GIST_PAGE_SPLIT		0x30
 /* #define XLOG_GIST_INSERT_COMPLETE	 0x40 */	/* not used anymore */
 /* #define XLOG_GIST_CREATE_INDEX		 0x50 */	/* not used anymore */
#define XLOG_GIST_PAGE_DELETE		0x60
#define XLOG_GIST_ASSIGN_LSN		0x70	/* 空操作,分配LSN */

/*
 * 备份块 0: 已更新页
 * 备份块 1: 若本操作借插入分裂页的
 *				 下行指针完成页分裂，则为分裂左半
 */
typedef struct gistxlogPageUpdate
{
	/* 已删除偏移数目 */
	uint16		ntodelete;
	uint16		ntoinsert;

	/*
	 * 块 0 载荷内: 1. 待删 OffsetNumbers 2. 待插入元组
	 */
} gistxlogPageUpdate;

/*
 * 备份块 0: 叶页，其上索引元组被删除。
 */
typedef struct gistxlogDelete
{
	TransactionId snapshotConflictHorizon;
	uint16		ntodelete;		/* 已删除偏移数目 */
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/

	/* TODELETE OFFSET NUMBERS */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} gistxlogDelete;

#define SizeOfGistxlogDelete	offsetof(gistxlogDelete, offsets)

/*
 * 备份块 0: 若本操作借插入分裂页的
 *				 下行指针完成页分裂，则为分裂左半
 * 备份块 1 - npage: 分裂页(1 是原页)
 */
typedef struct gistxlogPageSplit
{
	BlockNumber origrlink;		/* 分裂前该页的右链 */
	GistNSN		orignsn;		/* 分裂前该页的 NSN */
	bool		origleaf;		/* 被分裂页是否为叶页? */

	uint16		npage;			/* 分裂中的页数 */
	bool		markfollowright;	/* 设F_FOLLOW_RIGHT标志 */

	/*
	 * 随后: 1. 每页 gistxlogPage 与 IndexTupleData 数组
	 */
} gistxlogPageSplit;

/*
 * 备份块 0: 被删页。
 * 备份块 1: 父页，含指向被删页的下行指针。
 */
typedef struct gistxlogPageDelete
{
	FullTransactionId deleteXid;	/* 扫描中可见本页的最后 Xid */
	OffsetNumber downlinkOffset;	/* 下行指针之偏移，引用此
									 * 页*/
} gistxlogPageDelete;

#define SizeOfGistxlogPageDelete	(offsetof(gistxlogPageDelete, downlinkOffset) + sizeof(OffsetNumber))


/*
 * 此为页重用时热备所需知的内容。
 */
typedef struct gistxlogPageReuse
{
	RelFileLocator locator;
	BlockNumber block;
	FullTransactionId snapshotConflictHorizon;
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/
} gistxlogPageReuse;

#define SizeOfGistxlogPageReuse	(offsetof(gistxlogPageReuse, isCatalogRel) + sizeof(bool))

extern void gist_redo(XLogReaderState *record);
extern void gist_desc(StringInfo buf, XLogReaderState *record);
extern const char *gist_identify(uint8 info);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);
extern void gist_mask(char *pagedata, BlockNumber blkno);

#endif
