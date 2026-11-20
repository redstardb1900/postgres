/*-------------------------------------------------------------------------
 *
 * visibilitymapdefs.h
 *		访问可见性映射页内容的宏
 *
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 * src/include/access/visibilitymapdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAPDEFS_H
#define VISIBILITYMAPDEFS_H

/* 每个堆页的位数 */
#define BITS_PER_HEAPBLOCK 2

/* 位图标志 */
#define VISIBILITYMAP_ALL_VISIBLE	0x01
#define VISIBILITYMAP_ALL_FROZEN	0x02
#define VISIBILITYMAP_VALID_BITS	0x03	/* 所有有效可见性映射
											 * 位之或 */
/*
 * 为在备库上逻辑解码期间检测恢复冲突，我们需要
 * 知道某表是否为用户目录表。为此我们另加一
 * 位到 xl_heap_visible.flags，作为上述之补充。
 *
 * 注意：VISIBILITYMAP_XLOG_* 不可传给 visibilitymap_set()。
 */
#define VISIBILITYMAP_XLOG_CATALOG_REL	0x04
#define VISIBILITYMAP_XLOG_VALID_BITS	(VISIBILITYMAP_VALID_BITS | VISIBILITYMAP_XLOG_CATALOG_REL)

#endif							/* VISIBILITYMAPDEFS_H */
