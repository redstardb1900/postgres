/*
 * pg_crc.h
 *
 * PostgreSQL CRC 支持
 *
 * 参见 Ross Williams 的精彩导论
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS，见
 * http://ross.net/crc/ 或其他若干网站。
 *
 * 我们有三种略有不同的 32 位 CRC 计算变体：
 * CRC-32C（Castagnoli 多项式）、CRC-32（以太网多项式），以及
 * 一种以奇特方式使用查找表的遗留 CRC-32。
 * 共四个宏：
 *
 * INIT_<variant>(crc)
 *		初始化 CRC 累加器
 *
 * COMP_<variant>(crc, data, len)
 *		把若干(更多)字节累加进 CRC
 *
 * FIN_<variant>(crc)
 *		结束一次 CRC 计算
 *
 * EQ_<variant>(c1, c2)
 *		检查两 CRCs 是否相等。
 *
 * CRC-32C 变体在 port/pg_crc32c.h。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/pg_crc.h
 */
#ifndef PG_CRC_H
#define PG_CRC_H

typedef uint32 pg_crc32;

/*
 * CRC-32，与以太网等所用相同。
 *
 * 目前仅用于 ltree 与 hstore 贡献模块。它使用
 * 与下方遗留算法相同的查找表。新代码应改用
 * Castagnoli 版本。
 */
#define INIT_TRADITIONAL_CRC32(crc) ((crc) = 0xFFFFFFFF)
#define FIN_TRADITIONAL_CRC32(crc)	((crc) ^= 0xFFFFFFFF)
#define COMP_TRADITIONAL_CRC32(crc, data, len)	\
	COMP_CRC32_NORMAL_TABLE(crc, data, len, pg_crc32_table)
#define EQ_TRADITIONAL_CRC32(c1, c2) ((c1) == (c2))

/* Sarwate 算法，配合"正常"查找表使用 */
#define COMP_CRC32_NORMAL_TABLE(crc, data, len, table)			  \
do {															  \
	const unsigned char *__data = (const unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) (crc) ^ *__data++) & 0xFF; \
		(crc) = table[__tab_index] ^ ((crc) >> 8); \
	} \
} while (0)

/*
 * 9.5 之前版本中 WAL 等所用的 CRC 算法。
 *
 * 它酷似普通 CRC-32，但有细微差别。用 Williams
 * 的术语，我们用"正常"表，却配以"反射"代码。
 * 这是错的，但多年无人察觉。它不对应普通 CRC
 * 算法中的任何多项式，故该算法的检错性质并不
 * 清楚。见上。
 * 见上
 *
 * 仍须保留，因其用于若干需 pg_upgradeable 的
 * 磁盘结构。新代码不应使用。
 * code.
 */
#define INIT_LEGACY_CRC32(crc) ((crc) = 0xFFFFFFFF)
#define FIN_LEGACY_CRC32(crc)	((crc) ^= 0xFFFFFFFF)
#define COMP_LEGACY_CRC32(crc, data, len)	\
	COMP_CRC32_REFLECTED_TABLE(crc, data, len, pg_crc32_table)
#define EQ_LEGACY_CRC32(c1, c2) ((c1) == (c2))

/*
 * Sarwate 算法，配合"反射"查找表（但在遗留
 * 算法中我们实际用在"正常"表上，见上）
 */
#define COMP_CRC32_REFLECTED_TABLE(crc, data, len, table) \
do {															  \
	const unsigned char *__data = (const unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) ((crc) >> 24) ^ *__data++) & 0xFF; \
		(crc) = table[__tab_index] ^ ((crc) << 8); \
	} \
} while (0)

/*
 * CRC-32 多项式常量表。正常与传统变体共用
 * 同一张表。
 */
extern PGDLLIMPORT const uint32 pg_crc32_table[256];

#endif							/* PG_CRC_H */
