/*-------------------------------------------------------------------------
 *
 * bufmask.c
 *	  缓冲区屏蔽例程。用来屏蔽页中某些位，
 *	  这些位在生成 WAL 时与应用 WAL 时
 *	  可能不同。
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * 包含屏蔽一页所需的公共例程。
 *
 * IDENTIFICATION
 *	  src/backend/access/common/bufmask.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/bufmask.h"

/*
 * mask_page_lsn_and_checksum
 *
 * 一致性检查中，所比较的两页的 LSN 很可能不同，
 * 因为生成 WAL 时有并发操作，而应用 WAL 时页的状态
 * 也不一样。同时把校验和也屏蔽掉，因为只要屏蔽了页上
 * 其他任何内容，校验和同样不会相符。
 */
void
mask_page_lsn_and_checksum(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	PageXLogRecPtrSet(phdr->pd_lsn, (uint64) MASK_MARKER);
	phdr->pd_checksum = MASK_MARKER;
}

/*
 * mask_page_hint_bits
 *
 * 屏蔽 PageHeader 中的提示位。我们要忽略提示位的差异，
 * 因为它们可以在不发出任何 WAL 的情况下被设置。
 */
void
mask_page_hint_bits(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	/* 忽略 prune_xid（它类似于提示位） */
	phdr->pd_prune_xid = MASK_MARKER;

	/* 忽略 PD_PAGE_FULL 与 PD_HAS_FREE_LINES 标志，它们只是提示。 */
	PageClearFull(page);
	PageClearHasFreeLinePointers(page);

	/*
	 * 重放期间，若页 LSN 已经推进到超过本条 XLOG 记录的 LSN，
	 * 我们就不把该页标为全可见。详见 heap_xlog_visible()
	 * 的说明。
	 */
	PageClearAllVisible(page);
}

/*
 * mask_unused_space
 *
 * 屏蔽页上 pd_lower 与 pd_upper 之间的未使用空间。
 */
void
mask_unused_space(Page page)
{
	int			pd_lower = ((PageHeader) page)->pd_lower;
	int			pd_upper = ((PageHeader) page)->pd_upper;
	int			pd_special = ((PageHeader) page)->pd_special;

	/* 健全性检查 */
	if (pd_lower > pd_upper || pd_special < pd_upper ||
		pd_lower < SizeOfPageHeaderData || pd_special > BLCKSZ)
	{
		elog(ERROR, "invalid page pd_lower %u pd_upper %u pd_special %u",
			 pd_lower, pd_upper, pd_special);
	}

	memset(page + pd_lower, MASK_MARKER, pd_upper - pd_lower);
}

/*
 * mask_lp_flags
 *
 * 在某些索引访问方法中，主库上的行指针标志可以在
 * 不发出任何 WAL 记录的情况下被修改。
 */
void
mask_lp_flags(Page page)
{
	OffsetNumber offnum,
				maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsUsed(itemId))
			itemId->lp_flags = LP_UNUSED;
	}
}

/*
 * mask_page_content
 *
 * 在某些索引访问方法中，已删除页的内容几乎需要
 * 被完全忽略。
 */
void
mask_page_content(Page page)
{
	/* 屏蔽页内容 */
	memset(page + SizeOfPageHeaderData, MASK_MARKER,
		   BLCKSZ - SizeOfPageHeaderData);

	/* 屏蔽 pd_lower 与 pd_upper */
	memset(&((PageHeader) page)->pd_lower, MASK_MARKER,
		   sizeof(uint16));
	memset(&((PageHeader) page)->pd_upper, MASK_MARKER,
		   sizeof(uint16));
}
