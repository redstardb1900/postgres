/*--------------------------------------------------------------------------
 * ginblock.h
 *	  GIN 索引块内所存结构的细节
 *
 *	Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *	src/include/access/ginblock.h
 *--------------------------------------------------------------------------
 */
#ifndef GINBLOCK_H
#define GINBLOCK_H

#include "access/transam.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/off.h"

/*
 * 倒排索引页中的页不透明数据。
 *
 * 注意: GIN 不像其他索引类型那样含页 ID 字。
 * 这没问题，因不透明数据仅 8 字节，故能可靠地
 * 按大小区分。若该大小增大则须再审视。
 * 再注意: 自 9.2 起 SP-GiST 亦用 8 字节特殊空间，
 * BRIN 自 9.5 起亦然。仍没问题，只要 GIN 未用尽其
 * flags 字的高位，因如此 flags 字就无法
 * 匹配 SP-GiST 与 BRIN 所用的页 ID。
 */
typedef struct GinPageOpaqueData
{
	BlockNumber rightlink;		/* 下一页,若有 */
	OffsetNumber maxoff;		/* PostingItems 数(GIN_DATA &
								 * ~GIN_LEAF 页)。GIN_LIST 页上则为
								 * 堆元组。*/
	uint16		flags;			/* 见下方位定义 */
} GinPageOpaqueData;

typedef GinPageOpaqueData *GinPageOpaque;

#define GIN_DATA		  (1 << 0)
#define GIN_LEAF		  (1 << 1)
#define GIN_DELETED		  (1 << 2)
#define GIN_META		  (1 << 3)
#define GIN_LIST		  (1 << 4)
#define GIN_LIST_FULLROW  (1 << 5)	/* 仅对 GIN_LIST 页有意义 */
#define GIN_INCOMPLETE_SPLIT (1 << 6)	/* 页已分裂,但父尚未
										 * 更新*/
#define GIN_COMPRESSED	  (1 << 7)

/* 固定位置页的页号 */
#define GIN_METAPAGE_BLKNO	(0)
#define GIN_ROOT_BLKNO		(1)

typedef struct GinMetaPageData
{
	/*
	 * 指向挂起列表头与尾的指针；该列表由 GIN_LIST
	 * 页组成。存放尚未迁入常规 GIN 结构的快速插入
	 * 条目。
	 */
	BlockNumber head;
	BlockNumber tail;

	/*
	 * 挂起列表尾页中的空闲空间字节数。
	 */
	uint32		tailFreeSize;

	/*
	 * 我们同时存放页数与堆元组数，二者皆在
	 * 挂起列表
	 */
	BlockNumber nPendingPages;
	int64		nPendingHeapTuples;

	/*
	 * 供规划器使用的统计（准至上次 VACUUM）
	 */
	BlockNumber nTotalPages;
	BlockNumber nEntryPages;
	BlockNumber nDataPages;
	int64		nEntries;

	/*
	 * GIN 版本号（理想上应放在最前，但现在已太
	 * 晚。勿再移动！）
	 *
	 * 当前为 2（9.4 或更晚初始化的索引）
	 *
	 * 版本 1（在 9.1、9.2 或 9.3 中初始化的索引）
	 * 兼容，但可能含未压缩倒排树（叶）页与
	 * 倒排列表。将在修改时转为压缩
	 * 格式。
	 *
	 * 版本 0（9.0 或更早初始化的索引）兼容，但可能
	 * 缺少空条目，包括空键与占位二者。
	 * 拒绝对此类索引做完整索引扫描。
	 */
	int32		ginVersion;
} GinMetaPageData;

#define GIN_CURRENT_VERSION		2

#define GinPageGetMeta(p) \
	((GinMetaPageData *) PageGetContents(p))

/*
 * 访问 GIN 索引页不透明数据的宏
 */
#define GinPageGetOpaque(page) ( (GinPageOpaque) PageGetSpecialPointer(page) )

#define GinPageIsLeaf(page)    ( (GinPageGetOpaque(page)->flags & GIN_LEAF) != 0 )
#define GinPageSetLeaf(page)   ( GinPageGetOpaque(page)->flags |= GIN_LEAF )
#define GinPageSetNonLeaf(page)    ( GinPageGetOpaque(page)->flags &= ~GIN_LEAF )
#define GinPageIsData(page)    ( (GinPageGetOpaque(page)->flags & GIN_DATA) != 0 )
#define GinPageSetData(page)   ( GinPageGetOpaque(page)->flags |= GIN_DATA )
#define GinPageIsList(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST) != 0 )
#define GinPageSetList(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST )
#define GinPageHasFullRow(page)    ( (GinPageGetOpaque(page)->flags & GIN_LIST_FULLROW) != 0 )
#define GinPageSetFullRow(page)   ( GinPageGetOpaque(page)->flags |= GIN_LIST_FULLROW )
#define GinPageIsCompressed(page)	 ( (GinPageGetOpaque(page)->flags & GIN_COMPRESSED) != 0 )
#define GinPageSetCompressed(page)	 ( GinPageGetOpaque(page)->flags |= GIN_COMPRESSED )

#define GinPageIsDeleted(page) ( (GinPageGetOpaque(page)->flags & GIN_DELETED) != 0 )
#define GinPageSetDeleted(page)    ( GinPageGetOpaque(page)->flags |= GIN_DELETED)
#define GinPageSetNonDeleted(page) ( GinPageGetOpaque(page)->flags &= ~GIN_DELETED)
#define GinPageIsIncompleteSplit(page) ( (GinPageGetOpaque(page)->flags & GIN_INCOMPLETE_SPLIT) != 0 )

#define GinPageRightMost(page) ( GinPageGetOpaque(page)->rightlink == InvalidBlockNumber)

/*
 * 仅当删除前已启动的每笔事务皆已结束后，才应
 * 回收已删页。
 */
#define GinPageGetDeleteXid(page) ( ((PageHeader) (page))->pd_prune_xid )
#define GinPageSetDeleteXid(page, xid) ( ((PageHeader) (page))->pd_prune_xid = xid)
extern bool GinPageIsRecyclable(Page page);

/*
 * 我们自用 ItemPointerGet(BlockNumber|OffsetNumber)
 * 以避免 Assert，因有时 ip_posid 并非"合法"
 */
#define GinItemPointerGetBlockNumber(pointer) \
	(ItemPointerGetBlockNumberNoCheck(pointer))

#define GinItemPointerGetOffsetNumber(pointer) \
	(ItemPointerGetOffsetNumberNoCheck(pointer))

#define GinItemPointerSetBlockNumber(pointer, blkno) \
	(ItemPointerSetBlockNumber((pointer), (blkno)))

#define GinItemPointerSetOffsetNumber(pointer, offnum) \
	(ItemPointerSetOffsetNumber((pointer), (offnum)))


/*
 * GIN 搜索逻辑所需的特殊情形项指针值。
 *	MIN: 排序小于任何合法项指针
 *	MAX: 排序大于任何合法项指针
 *	LOSSY PAGE: 表示整张堆页，排在该页普通项
 *	指针之后
 * 注意它们皆可与"无效"项指针区分开
 * （即 InvalidBlockNumber/0），亦可与全部普通项
 * 指针区分（项号范围为 1..MaxHeapTuplesPerPage）。
 */
#define ItemPointerSetMin(p)  \
	ItemPointerSet((p), (BlockNumber)0, (OffsetNumber)0)
#define ItemPointerIsMin(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0 && \
	 GinItemPointerGetBlockNumber(p) == (BlockNumber)0)
#define ItemPointerSetMax(p)  \
	ItemPointerSet((p), InvalidBlockNumber, (OffsetNumber)0xffff)
#define ItemPointerSetLossyPage(p, b)  \
	ItemPointerSet((p), (b), (OffsetNumber)0xffff)
#define ItemPointerIsLossyPage(p)  \
	(GinItemPointerGetOffsetNumber(p) == (OffsetNumber)0xffff && \
	 GinItemPointerGetBlockNumber(p) != InvalidBlockNumber)

/*
 * 非叶倒排树页中的倒排项
 */
typedef struct
{
	/* 用 BlockIdData 而非 BlockNumber 以免填充浪费空间 */
	BlockIdData child_blkno;
	ItemPointerData key;
} PostingItem;

#define PostingItemGetBlockNumber(pointer) \
	BlockIdGetBlockNumber(&(pointer)->child_blkno)

#define PostingItemSetBlockNumber(pointer, blockNumber) \
	BlockIdSet(&((pointer)->child_blkno), (blockNumber))

/*
 * 用于区分占位空值与普通 NULL 键的类别码。
 *
 * 前两个码值选成与 bool 型 isNull 标志的惯用用法
 * 兼容。但在 bool 与 GinNullCategory 间强制转换有
 * 风险，因位模式与类型大小可能不同，
 * 故不再如此做。
 *
 * GIN_CAT_EMPTY_QUERY 从不存入索引；并请注意它被
 * 选成排在常规键值之前而非之后。
 */
typedef signed char GinNullCategory;

#define GIN_CAT_NORM_KEY		0	/* 普通非空键值 */
#define GIN_CAT_NULL_KEY		1	/* 空键值 */
#define GIN_CAT_EMPTY_ITEM		2	/* 零键项的占位 */
#define GIN_CAT_NULL_ITEM		3	/* 空项的占位 */
#define GIN_CAT_EMPTY_QUERY		(-1)	/* 全扫描查询的占位 */

/*
 * 条目元组中空类别字节的访问宏
 */
#define GinCategoryOffset(itup,ginstate) \
	(IndexInfoFindDataOffset((itup)->t_info) + \
	 ((ginstate)->oneCol ? 0 : sizeof(int16)))
#define GinGetNullCategory(itup,ginstate) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))))
#define GinSetNullCategory(itup,ginstate,c) \
	(*((GinNullCategory *) ((char*)(itup) + GinCategoryOffset(itup,ginstate))) = (c))

/*
 * 叶页条目元组的访问宏（见 README 讨论）
 */
#define GinGetNPosting(itup)	GinItemPointerGetOffsetNumber(&(itup)->t_tid)
#define GinSetNPosting(itup,n)	ItemPointerSetOffsetNumber(&(itup)->t_tid,n)
#define GIN_TREE_POSTING		((OffsetNumber)0xffff)
#define GinIsPostingTree(itup)	(GinGetNPosting(itup) == GIN_TREE_POSTING)
#define GinSetPostingTree(itup, blkno)	( GinSetNPosting((itup),GIN_TREE_POSTING), ItemPointerSetBlockNumber(&(itup)->t_tid, blkno) )
#define GinGetPostingTree(itup) GinItemPointerGetBlockNumber(&(itup)->t_tid)

#define GIN_ITUP_COMPRESSED		(1U << 31)
#define GinGetPostingOffset(itup)	(GinItemPointerGetBlockNumber(&(itup)->t_tid) & (~GIN_ITUP_COMPRESSED))
#define GinSetPostingOffset(itup,n) ItemPointerSetBlockNumber(&(itup)->t_tid,(n)|GIN_ITUP_COMPRESSED)
#define GinGetPosting(itup)			((Pointer) ((char*)(itup) + GinGetPostingOffset(itup)))
#define GinItupIsCompressed(itup)	((GinItemPointerGetBlockNumber(&(itup)->t_tid) & GIN_ITUP_COMPRESSED) != 0)

/*
 * 条目树页上一项的最大尺寸。确保每页至少放得下
 * 三项。（在常规 B-tree 索引上，我们必须至少放得下
 * 三项：两个数据项与"高键"。在 GIN 条目树中，我们
 * 当前不显式存储高键，只用页上最右项，
 * 故实际能放下两项即够。）
 */
#define GinMaxItemSize \
	Min(INDEX_SIZE_MASK, \
		MAXALIGN_DOWN(((BLCKSZ - \
						MAXALIGN(SizeOfPageHeaderData + 3 * sizeof(ItemIdData)) - \
						MAXALIGN(sizeof(GinPageOpaqueData))) / 3)))

/*
 * 非叶条目元组的访问宏
 */
#define GinGetDownlink(itup)	GinItemPointerGetBlockNumber(&(itup)->t_tid)
#define GinSetDownlink(itup,blkno)	ItemPointerSet(&(itup)->t_tid, blkno, InvalidOffsetNumber)


/*
 * 数据（倒排树）页
 *
 * 倒排树页不存常规元组。非叶页含
 * PostingItems，即 ItemPointers 与子块号的对。
 * 叶页含 GinPostingLists 与未压缩的项
 * 指针。
 *
 * 在叶页中，压缩倒排列表紧挨常规
 * 页头之后相继存放。虽不存常规元组，
 * 仍用 pd_lower 标示倒排列表末尾。其后是空闲
 * 空间。此布局与 bufpage.h 所述"标准"堆与
 * 索引页布局兼容，故例如可在写 WAL 记录时设
 * buffer_std。
 *
 * 特殊空间中是 GinPageOpaque 结构。
 */
#define GinDataLeafPageGetPostingList(page) \
	(GinPostingList *) ((PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData))))
#define GinDataLeafPageGetPostingListSize(page) \
	(((PageHeader) page)->pd_lower - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(ItemPointerData)))

#define GinDataLeafPageIsEmpty(page) \
	(GinPageIsCompressed(page) ? (GinDataLeafPageGetPostingListSize(page) == 0) : (GinPageGetOpaque(page)->maxoff < FirstOffsetNumber))

#define GinDataLeafPageGetFreeSpace(page) PageGetExactFreeSpace(page)

#define GinDataPageGetRightBound(page)	((ItemPointer) PageGetContents(page))
/*
 * 指向倒排树页数据部分的指针。对内部页，
 * 即 PostingItems 数组之始。对压缩叶
 * 页，是首个压缩倒排列表。对未压缩（9.4 前）叶
 * 页，则是 ItemPointer 数组之始。
 */
#define GinDataPageGetData(page)	\
	(PageGetContents(page) + MAXALIGN(sizeof(ItemPointerData)))
/* 非叶页含 PostingItems */
#define GinDataPageGetPostingItem(page, i)	\
	((PostingItem *) (GinDataPageGetData(page) + ((i)-1) * sizeof(PostingItem)))

/*
 * 注意: 无 GinDataPageGetDataSize 宏，因在版本
 * 9.4 前我们未在数据页上设 pd_lower。索引中可能有
 * 经二进制升级自更早版本、仍带无效
 * pd_lower 的页，故一般无法信任它。压缩倒排树叶
 * 页是 9.4 新出现的，故可信任；见
 * GinDataLeafPageGetPostingListSize.
 */
#define GinDataPageSetDataSize(page, size) \
	{ \
		Assert(size <= GinDataPageMaxDataSize); \
		((PageHeader) page)->pd_lower = (size) + MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(ItemPointerData)); \
	}

#define GinNonLeafDataPageGetFreeSpace(page)	\
	(GinDataPageMaxDataSize - \
	 GinPageGetOpaque(page)->maxoff * sizeof(PostingItem))

#define GinDataPageMaxDataSize	\
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
	 - MAXALIGN(sizeof(ItemPointerData)) \
	 - MAXALIGN(sizeof(GinPageOpaqueData)))

/*
 * 列表页
 */
#define GinListPageSize  \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GinPageOpaqueData)) )

/*
 * 压缩倒排列表。
 *
 * 注意: 这需要 2 字节对齐。
 */
typedef struct
{
	ItemPointerData first;		/* 本倒排列表首项（未打包） */
	uint16		nbytes;			/* 后续字节数 */
	unsigned char bytes[FLEXIBLE_ARRAY_MEMBER]; /* varbyte 编码的项 */
} GinPostingList;

#define SizeOfGinPostingList(plist) (offsetof(GinPostingList, bytes) + SHORTALIGN((plist)->nbytes) )
#define GinNextPostingListSegment(cur) ((GinPostingList *) (((char *) (cur)) + SizeOfGinPostingList((cur))))

#endif							/* GINBLOCK_H */
