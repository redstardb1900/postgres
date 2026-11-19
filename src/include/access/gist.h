/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  GiST 索引的公开 API。本 API 暴露给
 *	  实现 GiST 索引的个人，故不向后兼容的
 *	  变更应谨慎为之。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "access/itup.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/*
 * GiST 索引的 amproc 编号。
 */
#define GIST_CONSISTENT_PROC			1
#define GIST_UNION_PROC					2
#define GIST_COMPRESS_PROC				3
#define GIST_DECOMPRESS_PROC			4
#define GIST_PENALTY_PROC				5
#define GIST_PICKSPLIT_PROC				6
#define GIST_EQUAL_PROC					7
#define GIST_DISTANCE_PROC				8
#define GIST_FETCH_PROC					9
#define GIST_OPTIONS_PROC				10
#define GIST_SORTSUPPORT_PROC			11
#define GISTNProcs					11

/*
 * GiST 索引页之不透明数据。
 */
#define F_LEAF				(1 << 0)	/* 叶子页 */
#define F_DELETED			(1 << 1)	/* 该页已被删除 */
#define F_TUPLES_DELETED	(1 << 2)	/* 页上部分元组已
										 * 删除*/
#define F_FOLLOW_RIGHT		(1 << 3)	/* 右侧页无下行指针 */
#define F_HAS_GARBAGE		(1 << 4)	/* 页上部分元组已死，
										 * 但尚未删除*/

/*
 * NSN（节点序列号）是存于每页
 * GISTPageOpaqueData 中的专用 LSN，仅在页分裂时更新。借
 * 由在 GISTSearchItem.parentlsn 中记录父的 LSN，可
 * 通过检查 parentlsn < 子的 NSN 来检测并发子页分裂，
 * 并妥善处理。子页的 LSN 不足以达此目的，因
 * 它会在每次页变更时更新。
 */
typedef XLogRecPtr GistNSN;

/*
 * 索引构建期间使用的假 LSN / NSN 值。必须小于索引
 * 构建完成后生成的任何真或假（未记录）LSN，以便
 * 所有分裂都被视为完成。
 */
#define GistBuildLSN	((XLogRecPtr) 1)

/*
 * 为与 pre-9.3 服务器盘上兼容，NSN 存为两个
 * 磁盘上 32 位字段，同 LSNs。
 */
typedef PageXLogRecPtr PageGistNSN;

typedef struct GISTPageOpaqueData
{
	PageGistNSN nsn;			/* 此值必须在页分裂时更改 */
	BlockNumber rightlink;		/* 下一页若有 */
	uint16		flags;			/* 见上方的位定义 */
	uint16		gist_page_id;	/* 用于识别 GiST 索引 */
} GISTPageOpaqueData;

typedef GISTPageOpaqueData *GISTPageOpaque;

/*
 * GiST 索引元组与索引键的最大可能尺寸。计算基于
 * GiST 页应至少容纳 4 个元组的假定。理论上，
 * 页能容纳 3 个元组时 GiST 索引也可工作。但那看似
 * 相当低效，故我们采用略保守的估计。
 *
 * 索引键最大尺寸对单列索引成立。因此，本
 * 估计应用于弄清 GiST 索引键的何种最大尺寸
 * 才有意义。对多列索引，用户或可用操作符类
 * 参数调节键尺寸。
 */
#define GISTMaxIndexTupleSize	\
	MAXALIGN_DOWN((BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)) / \
				  4 - sizeof(ItemIdData))

#define GISTMaxIndexKeySize	\
	(GISTMaxIndexTupleSize - MAXALIGN(sizeof(IndexTupleData)))

/*
 * 页 ID 是为方便 pg_filedump 及类似工具，
 * 否则它们很难区分不同索引类型的
 * 页。它应是页上最后 2 字节。这因对齐考
 * 虑而或多或少是"免费"的。
 */
#define GIST_PAGE_ID		0xFF81

/*
 * 这是由 PickSplit 方法返回的分裂向量。
 * PickSplit 应把送往左侧的元组下标填入
 * spl_left[]，送往右侧的填入 spl_right[]（注意该方法
 * 负责对这两个数组做 palloc！）。元组计数写入
 * spl_nleft/spl_nright，且 spl_ldatum/spl_rdatum 必须设为
 * 各侧的联合键。
 *
 * 若 spl_ldatum_exists 与 spl_rdatum_exists 为真，则我们正进行
 * 非第一索引列的"二次分裂"。此时关于
 * 页分裂的部分决定已做出，传给 PickSplit 的
 * 元组集只是我们尚犹豫未决的那些元组。
 * 此时 spl_ldatum/spl_rdatum 含已选定送左或右的
 * 元组的联合键。理想情况下 PickSplit 方法应在决定
 * 如何处理剩余元组时考虑这些键，亦即
 * 应尝试从那些联合"外扩"，以尽量少地扩展
 * 它们。若如此做，应把给定元组的键并入既有
 * spl_ldatum/spl_rdatum 值，而非从零设定那些值，
 * 然后把 spl_ldatum_exists/spl_rdatum_exists 置为 false 以
 * 表明已这样做。
 *
 * 若 PickSplit 方法未能清除 spl_ldatum_exists/spl_rdatum_exists，
 * 核心 GiST 代码将自行决定如何把二次分裂
 * 结果与先前选定的元组合并，然后从零
 * 重新计算联合键。这是可行但往往非
 * 最优做法。
 */
typedef struct GIST_SPLITVEC
{
	OffsetNumber *spl_left;		/* 送往左侧的条目数组 */
	int			spl_nleft;		/* 本数组的大小 */
	Datum		spl_ldatum;		/* spl_left 中键的联合 */
	bool		spl_ldatum_exists;	/* 若 spl_ldatum 已存在则真。 */

	OffsetNumber *spl_right;	/* 送往右侧的条目数组 */
	int			spl_nright;		/* 本数组大小 */
	Datum		spl_rdatum;		/* spl_right 中键的联合 */
	bool		spl_rdatum_exists;	/* 若 spl_rdatum 已存在则真。 */
} GIST_SPLITVEC;

/*
 * GiST 节点上的条目。含键及其自身
 * 位置 (rel,page,offset)，可提供匹配的指针。
 * leafkey 标志告诉我们条目是否在叶子节点。
 */
typedef struct GISTENTRY
{
	Datum		key;
	Relation	rel;
	Page		page;
	OffsetNumber offset;
	bool		leafkey;
} GISTENTRY;

#define GistPageGetOpaque(page) ( (GISTPageOpaque) PageGetSpecialPointer(page) )

#define GistPageIsLeaf(page)	( GistPageGetOpaque(page)->flags & F_LEAF)
#define GIST_LEAF(entry) (GistPageIsLeaf((entry)->page))

#define GistPageIsDeleted(page) ( GistPageGetOpaque(page)->flags & F_DELETED)

#define GistTuplesDeleted(page) ( GistPageGetOpaque(page)->flags & F_TUPLES_DELETED)
#define GistMarkTuplesDeleted(page) ( GistPageGetOpaque(page)->flags |= F_TUPLES_DELETED)
#define GistClearTuplesDeleted(page)	( GistPageGetOpaque(page)->flags &= ~F_TUPLES_DELETED)

#define GistPageHasGarbage(page) ( GistPageGetOpaque(page)->flags & F_HAS_GARBAGE)
#define GistMarkPageHasGarbage(page) ( GistPageGetOpaque(page)->flags |= F_HAS_GARBAGE)
#define GistClearPageHasGarbage(page)	( GistPageGetOpaque(page)->flags &= ~F_HAS_GARBAGE)

#define GistFollowRight(page) ( GistPageGetOpaque(page)->flags & F_FOLLOW_RIGHT)
#define GistMarkFollowRight(page) ( GistPageGetOpaque(page)->flags |= F_FOLLOW_RIGHT)
#define GistClearFollowRight(page)	( GistPageGetOpaque(page)->flags &= ~F_FOLLOW_RIGHT)

#define GistPageGetNSN(page) ( PageXLogRecPtrGet(GistPageGetOpaque(page)->nsn))
#define GistPageSetNSN(page, val) ( PageXLogRecPtrSet(GistPageGetOpaque(page)->nsn, val))


/*
 * 在已删页上，我们存储此结构。已删页不含任何
 * 元组，故不用带行指针的常规页布局。而是把
 * 此结构紧接标准页头存放。pd_lower 指向
 * 本结构末尾。若将来向此结构添加字段，我们
 * 可通过 pd_lower 区分新旧格式。
 */
typedef struct GISTDeletedPageContents
{
	/* 扫描中还能看见本页的最后 xid */
	FullTransactionId deleteXid;
} GISTDeletedPageContents;

static inline void
GistPageSetDeleted(Page page, FullTransactionId deletexid)
{
	Assert(PageIsEmpty(page));

	GistPageGetOpaque(page)->flags |= F_DELETED;
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + sizeof(GISTDeletedPageContents);

	((GISTDeletedPageContents *) PageGetContents(page))->deleteXid = deletexid;
}

static inline FullTransactionId
GistPageGetDeleteXid(Page page)
{
	Assert(GistPageIsDeleted(page));

	/* deleteXid 字段是否存在？ */
	if (((PageHeader) page)->pd_lower >= MAXALIGN(SizeOfPageHeaderData) +
		offsetof(GISTDeletedPageContents, deleteXid) + sizeof(FullTransactionId))
	{
		return ((GISTDeletedPageContents *) PageGetContents(page))->deleteXid;
	}
	else
		return FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
}

/*
 * GISTENTRY 结构的向量；用户定义的 union 与 picksplit
 * 方法以其为参数之一
 */
typedef struct
{
	int32		n;				/* 元素个数 */
	GISTENTRY	vector[FLEXIBLE_ARRAY_MEMBER];
} GistEntryVector;

#define GEVHDRSZ	(offsetof(GistEntryVector, vector))

/*
 * 初始化 GISTENTRY 的宏
 */
#define gistentryinit(e, k, r, pg, o, l) \
	do { (e).key = (k); (e).rel = (r); (e).page = (pg); \
		 (e).offset = (o); (e).leafkey = (l); } while (0)

#endif							/* GIST_H */
