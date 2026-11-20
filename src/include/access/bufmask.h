/*-------------------------------------------------------------------------
 *
 * bufmask.h
 *	  缓冲区屏蔽例程的定义，用于屏蔽页中某些位，
 *	  这些位在生成 WAL 时与应用 WAL 时
 *	  可能不同。这本是各个 rmgr 的职责，但
 *	  我们提供一些公共例程，以处理多个 
 *	  rmgr 中会出现的情形。
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/access/bufmask.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFMASK_H
#define BUFMASK_H

#include "storage/block.h"
#include "storage/bufmgr.h"

/* 用于一致地屏蔽页的标记 */
#define MASK_MARKER		0

extern void mask_page_lsn_and_checksum(Page page);
extern void mask_page_hint_bits(Page page);
extern void mask_unused_space(Page page);
extern void mask_lp_flags(Page page);
extern void mask_page_content(Page page);

#endif
