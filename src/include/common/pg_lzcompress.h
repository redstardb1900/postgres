/* ----------
 * pg_lzcompress.h -
 *
 *	内置 LZ 压缩器的定义
 *
 * src/include/common/pg_lzcompress.h
 * ----------
 */

#ifndef _PG_LZCOMPRESS_H_
#define _PG_LZCOMPRESS_H_


/* ----------
 * PGLZ_MAX_OUTPUT -
 *
 *		计算 pglz_compress() 所需缓冲大小的宏。
 *		检测压缩失败前允许 4 字节越界。
 * ----------
 */
#define PGLZ_MAX_OUTPUT(_dlen)			((_dlen) + 4)


/* ----------
 * PGLZ_Strategy -
 *
 *		控制压缩算法的一些值。
 *
 *		min_input_size		考虑压缩的最小输入大小。
 *
 *		max_input_size		考虑压缩的最大输入大小。
 *
 *		min_comp_rate		要求的最小压缩率（0-99%）。
 *							不论 min_comp_rate，输出必须
 *							小于输入，否则不存压缩结果。
 *	略
 *
 *		first_success_by	若前这许多字节内找不到可压
 *							数据则放弃压缩。
 *
 *		match_size_good	开始历史查找时的初始 GOOD
 *	略
 *	略
 *	略
 *	略
 *							视为好。
 *	略
 *	略
 *	略
 *
 *		match_size_drop	每轮历史检查后降低 match_size_good
 *	略
 *							100（只检查最新历史项）。
 *	略
 * ----------
 */
typedef struct PGLZ_Strategy
{
	int32		min_input_size;
	int32		max_input_size;
	int32		min_comp_rate;
	int32		first_success_by;
	int32		match_size_good;
	int32		match_size_drop;
} PGLZ_Strategy;


/* ----------
 * 标准策略
 *
 *		PGLZ_strategy_default		TOAST 推荐的默认策略。
 *
 *		PGLZ_strategy_always		尝试压缩任意长度输入。
 *									仅当输出会大于输入时才退回
 *									未压缩存储。
 * ----------
 */
extern PGDLLIMPORT const PGLZ_Strategy *const PGLZ_strategy_default;
extern PGDLLIMPORT const PGLZ_Strategy *const PGLZ_strategy_always;


/* ----------
 * 全局函数声明
 * ----------
 */
extern int32 pglz_compress(const char *source, int32 slen, char *dest,
						   const PGLZ_Strategy *strategy);
extern int32 pglz_decompress(const char *source, int32 slen, char *dest,
							 int32 rawsize, bool check_complete);
extern int32 pglz_maximum_compressed_size(int32 rawsize,
										  int32 total_compressed_size);

#endif							/* _PG_LZCOMPRESS_H_ */
