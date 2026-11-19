/*
 * brin_page.h
 *		BRIN 页布局的原型与定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_page.h
 *
 * NOTES
 *
 * 这些结构本当私有于特定 BRIN 文件，但
 * 放此处供 pageinspect 及类似工具之
 * 用。
 */
#ifndef BRIN_PAGE_H
#define BRIN_PAGE_H

#include "storage/block.h"
#include "storage/itemptr.h"

/*
 * BRIN 页的特殊空间。
 *
 * 以这种奇特方式定义，使其恒占每页的最后
 * MAXALIGN 大小的元素。
 */
typedef struct BrinSpecialSpace
{
	uint16		vector[MAXALIGN(1) / sizeof(uint16)];
} BrinSpecialSpace;

/*
 * 使页类型为页上最后半字，供
 * pg_filedump 及类似工具使用。我们不太关心
 * "flags" 半字的位置，但对二者用一致规则更
 * 简单。
 *
 * 见上文 GinPageOpaqueData 注释。
 */
#define BrinPageType(page)		\
	(((BrinSpecialSpace *)		\
	  PageGetSpecialPointer(page))->vector[MAXALIGN(1) / sizeof(uint16) - 1])

#define BrinPageFlags(page)		\
	(((BrinSpecialSpace *)		\
	  PageGetSpecialPointer(page))->vector[MAXALIGN(1) / sizeof(uint16) - 2])

/* 所有 BRIN 页特殊空间存有"type"标识符 */
#define		BRIN_PAGETYPE_META			0xF091
#define		BRIN_PAGETYPE_REVMAP		0xF092
#define		BRIN_PAGETYPE_REGULAR		0xF093

#define BRIN_IS_META_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_META)
#define BRIN_IS_REVMAP_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_REVMAP)
#define BRIN_IS_REGULAR_PAGE(page) (BrinPageType(page) == BRIN_PAGETYPE_REGULAR)

/* BrinSpecialSpace 的标志 */
#define		BRIN_EVACUATE_PAGE			(1 << 0)


/* 元页定义 */
typedef struct BrinMetaPageData
{
	uint32		brinMagic;
	uint32		brinVersion;
	BlockNumber pagesPerRange;
	BlockNumber lastRevmapPage;
} BrinMetaPageData;

#define BRIN_CURRENT_VERSION		1
#define BRIN_META_MAGIC			0xA8109CFA

#define BRIN_METAPAGE_BLKNO		0

/* 范围映射页的定义 */
typedef struct RevmapContents
{
	/*
	 * 此数组将填满页上全部可用空间。应
	 * 声明为 [FLEXIBLE_ARRAY_MEMBER]，但因故无法在别无
	 * 成员的结构中如此。
	 */
	ItemPointerData rm_tids[1];
} RevmapContents;

#define REVMAP_CONTENT_SIZE \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
	 offsetof(RevmapContents, rm_tids) - \
	 MAXALIGN(sizeof(BrinSpecialSpace)))
/* 数组中的最大项数 */
#define REVMAP_PAGE_MAXITEMS \
	(REVMAP_CONTENT_SIZE / sizeof(ItemPointerData))

#endif							/* BRIN_PAGE_H */
