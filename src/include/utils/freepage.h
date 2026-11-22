/*-------------------------------------------------------------------------
 *
 * freepage.h
 *	  按页组织的空闲内存管理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/freepage.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FREEPAGE_H
#define FREEPAGE_H

#include "storage/lwlock.h"
#include "utils/relptr.h"

/* 前向声明。 */
typedef struct FreePageSpanLeader FreePageSpanLeader;
typedef struct FreePageBtree FreePageBtree;
typedef struct FreePageManager FreePageManager;

/*
 * PostgreSQL 多数场合用 8kB 页，但许多常见
 * 体系结构/操作系统组合的内存分配页大小为
 * 4kB，故此处也用 4kB。
 */
#define FPM_PAGE_SIZE			4096

/*
 * 除最后一个外，每个空闲列表只含特定大小的
 * 跨度。更大的都进最后一个。看似浪费，因为
 * 多数分配是几种常见大小，但这使小分配只需
 * 弹出相应列表头，而不必担心找到的对象是否
 * 恰好是正确大小（我们知道必定是）。
 * 见上。
 */
#define FPM_NUM_FREELISTS		129

/* 定义相对指针类型。 */
relptr_declare(FreePageBtree, RelptrFreePageBtree);
relptr_declare(FreePageManager, RelptrFreePageManager);
relptr_declare(FreePageSpanLeader, RelptrFreePageSpanLeader);

/* 管理空闲页所需的一切（见 freepage.c） */
struct FreePageManager
{
	RelptrFreePageManager self;
	RelptrFreePageBtree btree_root;
	RelptrFreePageSpanLeader btree_recycle;
	unsigned	btree_depth;
	unsigned	btree_recycle_count;
	Size		singleton_first_page;
	Size		singleton_npages;
	Size		contiguous_pages;
	bool		contiguous_pages_dirty;
	RelptrFreePageSpanLeader freelist[FPM_NUM_FREELISTS];
#ifdef FPM_EXTRA_ASSERTS
	/* 仅供调试：put 的页数减 get 的页数。 */
	Size		free_pages;
#endif
};

/* 在页号（以 Size 表示）与指针之间转换的宏。 */
#define fpm_page_to_pointer(base, page) \
	(AssertVariableIsOfTypeMacro(page, Size), \
	 (base) + FPM_PAGE_SIZE * (page))
#define fpm_pointer_to_page(base, ptr)		\
	(((Size) (((char *) (ptr)) - (base))) / FPM_PAGE_SIZE)

/* 把分配大小转换为页数的宏。 */
#define fpm_size_to_pages(sz) \
	(((sz) + FPM_PAGE_SIZE - 1) / FPM_PAGE_SIZE)

/* 检查绝对与相对指针对齐的宏。 */
#define fpm_pointer_is_page_aligned(base, ptr)		\
	(((Size) (((char *) (ptr)) - (base))) % FPM_PAGE_SIZE == 0)
#define fpm_relptr_is_page_aligned(base, relptr)		\
	(relptr_offset(relptr) % FPM_PAGE_SIZE == 0)

/* 求含某 FreePageManager 的段基址的宏。 */
#define fpm_segment_base(fpm)	\
	(((char *) fpm) - relptr_offset(fpm->self))

/* 访问 FreePageManager 最大连续页游程的宏。 */
#define fpm_largest(fpm) \
	(fpm->contiguous_pages)

/* 操纵空闲页映射的函数。 */
extern void FreePageManagerInitialize(FreePageManager *fpm, char *base);
extern bool FreePageManagerGet(FreePageManager *fpm, Size npages,
							   Size *first_page);
extern void FreePageManagerPut(FreePageManager *fpm, Size first_page,
							   Size npages);
extern char *FreePageManagerDump(FreePageManager *fpm);

#endif							/* FREEPAGE_H */
