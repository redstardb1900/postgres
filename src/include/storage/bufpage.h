/*-------------------------------------------------------------------------
 *
 * bufpage.h
 *	  标准 POSTGRES 缓冲页定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufpage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/item.h"
#include "storage/off.h"

/*
 * postgres 磁盘页是叠在 postgres 磁盘块之上的抽象（
 * 磁盘块只是 I/O 单位，见 block.h）。
 *
 * 具体而言，磁盘块可以未格式化，而 postgres
 * 磁盘页始终是如下形式的槽式页：
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp1/linp2/linp3 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...                         |
 * +-------------+------------------+-----------------+
 * |	   ... tuple3/tuple2/tuple1 | "特殊空间"  |
 * +--------------------------------+-----------------+
 *									^ pd_special
 *
 * 当 pd_lower 与 pd_upper 间无法再添加时，页即
 * 满。
 *
 * 访问方法写出的所有块都必须是磁盘页。
 *
 * EXCEPTIONS:
 *
 * 显然，页在由 PageInit 初始化之前尚未
 * 格式化。
 *
 * NOTES:
 *
 * linp1..N 构成 ItemId（行指针）数组。ItemPointers 指向
 * 物理块号及该块/页内的逻辑偏移（行指针
 * 号）。注意 OffsetNumbers
 * 按惯例从 1 起，而非 0。
 *
 * tuple1..N 在页上"反向"添加。由于 ItemPointer
 * 偏移用于访问 ItemId 条目而非实际
 * 字节偏移位置，元组可在需要时在页上
 * 物理挪动。此间接也使崩溃恢复
 * 相对简单，因为页空间管理的低层细节
 * 可在记日志及恢复期间由标准缓冲页代码
 * 控制。
 *
 * AM 通用的每页信息保存在 PageHeaderData 中。
 *
 * AM 特定的每页数据（若有）放在标为"特殊
 * 空间"的区域；每个 AM 都有某处定义的"opaque"结构，
 * 存为页尾。访问方法应始终
 * 用 PageInit 初始化其页，再设置自己的 opaque
 * 字段
 */

typedef Pointer Page;


/*
 * 页内位置（字节偏移）。
 *
 * 注意实际限于 2^15，因 ItemIdData.lp_off 与
 * ItemIdData.lp_len 限制为 15 位（见 itemid.h）。
 */
typedef uint16 LocationIndex;


/*
 * 出于历史原因，64 位 LSN 存成两个 32 位
 * 值。
 */
typedef struct
{
	uint32		xlogid;			/* 高位 */
	uint32		xrecoff;		/* 低位 */
} PageXLogRecPtr;

static inline XLogRecPtr
PageXLogRecPtrGet(PageXLogRecPtr val)
{
	return (uint64) val.xlogid << 32 | val.xrecoff;
}

#define PageXLogRecPtrSet(ptr, lsn) \
	((ptr).xlogid = (uint32) ((lsn) >> 32), (ptr).xrecoff = (uint32) (lsn))

/*
 * 磁盘页组织
 *
 * 对任何页通用的空间管理信息
 *
 *		pd_lsn		- 标识本页上次更改的 xlog 记录。
 *		pd_checksum - 页校验，若设。
 *		pd_flags	- 标志位
 *		pd_lower	- 空闲空间起始偏移。
 *		pd_upper	- 空闲空间结束偏移。
 *		pd_special	- 特殊空间起始偏移。
 *		pd_pagesize_version - 字节大小与页布局版本号。
 *		pd_prune_xid - 页上潜在可剪除元组中最旧 XID。
 *
 * 缓冲管理器用 LSN 强制 WAL 的基本规则："
 * 先写 xlog 再写数据"。脏缓冲区无法刷到
 * 磁盘，直到 xlog 至少已刷到该页的 LSN。
 *
 * pd_checksum 存储页校验和（若本页已设置）；
 * 零也是合法校验和值。若未使用校验和，则
 * 该字段保持未设置。这通常意味着字段为零，
 * 不过若数据库从 9.3 之前版本经
 * pg_upgraded 自 9.3 前，非零值可能出现，同一字节偏移
 * 用于存放页上次更新时的当前 timelineid。
 * 注意页上没有指示校验和是否
 * 有效，这是有意的设计选择，以免
 * 依赖页内容来决定是否校验。因此
 * 没有与校验和相关的标志位。
 *
 * pd_prune_xid 是提示字段，帮助判断剪除是否
 * 有用。目前在索引页上未使用。
 *
 * 页版本号与页大小打包进单个
 * uint16 字段。这是历史原因：PostgreSQL 7.3 之前
 * 没有页版本号的概念，这样做可让我们
 * 假装 pre-7.3 数据库的页版本号为零。
 * 我们约束页大小为 256 的倍数，低八
 * 位留给版本号。
 *
 * 最小可能页大小或为 64B，以放下页头、opaque 空间
 * 和最小元组；当然实际希望大得多，故
 * pagesize 模 256 的约束并非重要限制。
 * 高端我们只能支持到 32KB 的页，因为 lp_off/lp_len
 * 为 15 位
 */

typedef struct PageHeaderData
{
	/* XXX LSN 是*任何*块的成员，不限于按页组织者 */
	PageXLogRecPtr pd_lsn;		/* LSN: 本页上次更改之 xlog 记录
								 * 最后一字节后的下一字节 */
	uint16		pd_checksum;	/* 校验 */
	uint16		pd_flags;		/* 标志位，见下 */
	LocationIndex pd_lower;		/* 空闲空间起始偏移 */
	LocationIndex pd_upper;		/* 空闲空间结束偏移 */
	LocationIndex pd_special;	/* 特殊空间起始偏移 */
	uint16		pd_pagesize_version;
	TransactionId pd_prune_xid; /* 最旧可剪除 XID，若无为零 */
	ItemIdData	pd_linp[FLEXIBLE_ARRAY_MEMBER]; /* 行指针数组 */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

/*
 * pd_flags 含下列标志位。未定义位初始化为
 * 零，将来可能使用。
 *
 * 若 pd_lower 之前有任何 LP_UNUSED 行指针则设置
 * PD_HAS_FREE_LINES。应视为提示而非真相，因其
 * 更改不记 WAL。
 *
 * 若 UPDATE 在页中找不到足够空闲空间容纳其
 * 新元组版本则设置 PD_PAGE_FULL；这暗示需要剪除。
 * 同样只是提示。
 */
#define PD_HAS_FREE_LINES	0x0001	/* 有未用行指针吗？ */
#define PD_PAGE_FULL		0x0002	/* 空闲不够放新元组？ */
#define PD_ALL_VISIBLE		0x0004	/* 页上所有元组对所有人
									 * 可见 */

#define PD_VALID_FLAG_BITS	0x0007	/* 合法 pd_flags 位之 OR */

/*
 * 页布局版本号 0 用于 7.3 之前的 Postgres 发行。
 * 7.3 与 7.4 用 1，表示新的 HeapTupleHeader 布局。
 * 8.0 用 2；再次更改了 HeapTupleHeader 布局。
 * 8.1 用 3；重定义了 HeapTupleHeader infomask 位。
 * 8.3 用 4；再次更改 HeapTupleHeader 布局，并
 *		加入 pd_flags 字段（从 pd_tli 偷了若干位），
 *		以及 pd_prune_xid 字段（这会增大页头）。
 *
 * 自 9.3 起，处理页时还必须考虑
 * 校验和版本
 */
#define PG_PAGE_LAYOUT_VERSION		4
#define PG_DATA_CHECKSUM_VERSION	1

/* ----------------------------------------------------------------
 *						页支持函数
 * ----------------------------------------------------------------
 */

/*
 * 行指针 pointer 不计为页头的一部分
 */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

/*
 * PageIsEmpty
 *		当且仅当页上尚未分配 itemid 则为真
 */
static inline bool
PageIsEmpty(Page page)
{
	return ((PageHeader) page)->pd_lower <= SizeOfPageHeaderData;
}

/*
 * PageIsNew
 *		当且仅当页尚未初始化（由 PageInit）则为真
 */
static inline bool
PageIsNew(Page page)
{
	return ((PageHeader) page)->pd_upper == 0;
}

/*
 * PageGetItemId
 *		返回页的项标识。
 */
static inline ItemId
PageGetItemId(Page page, OffsetNumber offsetNumber)
{
	return &((PageHeader) page)->pd_linp[offsetNumber - 1];
}

/*
 * PageGetContents
 *		用于页不含行指针的情形。
 *
 * 注意：8.3 之前不保证产生 MAXALIGN 对齐的结果。
 * 现在保证。当心旧代码可能以为内容偏移
 * 只是 SizeOfPageHeaderData 而非 MAXALIGN(SizeOfPageHeaderData)。
 */
static inline char *
PageGetContents(Page page)
{
	return (char *) page + MAXALIGN(SizeOfPageHeaderData);
}

/* ----------------
 *		访问页大小信息的函数
 * ----------------
 */

/*
 * PageGetPageSize
 *		返回页的大小。
 *
 * 只能在已格式化的页上调用（不同于
 * BufferGetPageSize，后者可在未格式化页上调用）。
 * 但可以在未存于缓冲区的页上调用。
 */
static inline Size
PageGetPageSize(Page page)
{
	return (Size) (((PageHeader) page)->pd_pagesize_version & (uint16) 0xFF00);
}

/*
 * PageGetPageLayoutVersion
 *		返回页的页布局版本。
 */
static inline uint8
PageGetPageLayoutVersion(Page page)
{
	return (((PageHeader) page)->pd_pagesize_version & 0x00FF);
}

/*
 * PageSetPageSizeAndVersion
 *		设置页的页大小与页布局版本号。
 *
 * 我们可以支持分开设置这两个值，但目前
 * 并无实际需要。
 */
static inline void
PageSetPageSizeAndVersion(Page page, Size size, uint8 version)
{
	Assert((size & 0xFF00) == size);
	Assert((version & 0x00FF) == version);

	((PageHeader) page)->pd_pagesize_version = size | version;
}

/* ----------------
 *		页特殊数据函数
 * ----------------
 */
/*
 * PageGetSpecialSize
 *		返回页上特殊空间的大小。
 */
static inline uint16
PageGetSpecialSize(Page page)
{
	return (PageGetPageSize(page) - ((PageHeader) page)->pd_special);
}

/*
 * 用断言校验页特殊指针是否正常。
 *
 * 旨在捕获页初始化之前使用该指针。
 */
static inline void
PageValidateSpecialPointer(Page page)
{
	Assert(page);
	Assert(((PageHeader) page)->pd_special <= BLCKSZ);
	Assert(((PageHeader) page)->pd_special >= SizeOfPageHeaderData);
}

/*
 * PageGetSpecialPointer
 *		返回指向页上特殊空间的指针。
 */
static inline char *
PageGetSpecialPointer(Page page)
{
	PageValidateSpecialPointer(page);
	return (char *) page + ((PageHeader) page)->pd_special;
}

/*
 * PageGetItem
 *		取回给定页上的一项。
 *
 * Note:
 *		这不改变所传任何资源的状态。
 *		语义将来可能改变。
 */
static inline Item
PageGetItem(Page page, ItemId itemId)
{
	Assert(page);
	Assert(ItemIdHasStorage(itemId));

	return (Item) (((char *) page) + ItemIdGetOffset(itemId));
}

/*
 * PageGetMaxOffsetNumber
 *		返回给定页所用的最大偏移号。
 *		因偏移号从 1 起，这也是页上的
 *		项数。
 *
 *		注意：若页未初始化（pd_lower == 0），必须
 *		返回零以确保行为正常。
 */
static inline OffsetNumber
PageGetMaxOffsetNumber(Page page)
{
	PageHeader	pageheader = (PageHeader) page;

	if (pageheader->pd_lower <= SizeOfPageHeaderData)
		return 0;
	else
		return (pageheader->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
}

/*
 * 访问页头的附加函数。
 */
static inline XLogRecPtr
PageGetLSN(Page page)
{
	return PageXLogRecPtrGet(((PageHeader) page)->pd_lsn);
}
static inline void
PageSetLSN(Page page, XLogRecPtr lsn)
{
	PageXLogRecPtrSet(((PageHeader) page)->pd_lsn, lsn);
}

static inline bool
PageHasFreeLinePointers(Page page)
{
	return ((PageHeader) page)->pd_flags & PD_HAS_FREE_LINES;
}
static inline void
PageSetHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags |= PD_HAS_FREE_LINES;
}
static inline void
PageClearHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_HAS_FREE_LINES;
}

static inline bool
PageIsFull(Page page)
{
	return ((PageHeader) page)->pd_flags & PD_PAGE_FULL;
}
static inline void
PageSetFull(Page page)
{
	((PageHeader) page)->pd_flags |= PD_PAGE_FULL;
}
static inline void
PageClearFull(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_PAGE_FULL;
}

static inline bool
PageIsAllVisible(Page page)
{
	return ((PageHeader) page)->pd_flags & PD_ALL_VISIBLE;
}
static inline void
PageSetAllVisible(Page page)
{
	((PageHeader) page)->pd_flags |= PD_ALL_VISIBLE;
}
static inline void
PageClearAllVisible(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_ALL_VISIBLE;
}

/*
 * 这两项需要 "access/transam.h"，故留作宏。
 */
#define PageSetPrunable(page, xid) \
do { \
	Assert(TransactionIdIsNormal(xid)); \
	if (!TransactionIdIsValid(((PageHeader) (page))->pd_prune_xid) || \
		TransactionIdPrecedes(xid, ((PageHeader) (page))->pd_prune_xid)) \
		((PageHeader) (page))->pd_prune_xid = (xid); \
} while (0)
#define PageClearPrunable(page) \
	(((PageHeader) (page))->pd_prune_xid = InvalidTransactionId)


/* ----------------------------------------------------------------
 *		外部声明
 * ----------------------------------------------------------------
 */

/* PageAddItemExtended() 的标志 */
#define PAI_OVERWRITE			(1 << 0)
#define PAI_IS_HEAP				(1 << 1)

/* PageIsVerifiedExtended() 的标志 */
#define PIV_LOG_WARNING			(1 << 0)
#define PIV_REPORT_STAT			(1 << 1)

#define PageAddItem(page, item, size, offsetNumber, overwrite, is_heap) \
	PageAddItemExtended(page, item, size, offsetNumber, \
						((overwrite) ? PAI_OVERWRITE : 0) | \
						((is_heap) ? PAI_IS_HEAP : 0))

#define PageIsVerified(page, blkno) \
	PageIsVerifiedExtended(page, blkno, \
						   PIV_LOG_WARNING | PIV_REPORT_STAT)

/*
 * 检查 BLCKSZ 是否为 sizeof(size_t) 的倍数。在
 * PageIsVerifiedExtended() 中，用本机字长检查页是否
 * 全为零要快得多。注意把此断言
 * 放在头文件中，以确保 StaticAssertDecl() 在
 * 各种平台与编译器组合下可用。
 */
StaticAssertDecl(BLCKSZ == ((BLCKSZ / sizeof(size_t)) * sizeof(size_t)),
				 "BLCKSZ has to be a multiple of sizeof(size_t)");

extern void PageInit(Page page, Size pageSize, Size specialSize);
extern bool PageIsVerifiedExtended(Page page, BlockNumber blkno, int flags);
extern OffsetNumber PageAddItemExtended(Page page, Item item, Size size,
										OffsetNumber offsetNumber, int flags);
extern Page PageGetTempPage(Page page);
extern Page PageGetTempPageCopy(Page page);
extern Page PageGetTempPageCopySpecial(Page page);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);
extern void PageRepairFragmentation(Page page);
extern void PageTruncateLinePointerArray(Page page);
extern Size PageGetFreeSpace(Page page);
extern Size PageGetFreeSpaceForMultipleTuples(Page page, int ntups);
extern Size PageGetExactFreeSpace(Page page);
extern Size PageGetHeapFreeSpace(Page page);
extern void PageIndexTupleDelete(Page page, OffsetNumber offnum);
extern void PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems);
extern void PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum);
extern bool PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
									Item newtup, Size newsize);
extern char *PageSetChecksumCopy(Page page, BlockNumber blkno);
extern void PageSetChecksumInplace(Page page, BlockNumber blkno);

#endif							/* BUFPAGE_H */
