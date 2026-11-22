/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/ascii.h
 *
 *-----------------------------------------------------------------------
 */

#ifndef _ASCII_H_
#define _ASCII_H_

#include "port/simd.h"

extern void ascii_safe_strlcpy(char *dest, const char *src, size_t destsiz);

/*
 * 校验一块字节是否合法 ASCII。
 *
 * 若输入含任何零字节或高位被置位的字节则返回
 * false。输入长度须为块大小（8 或 16）的倍数。
 */
static inline bool
is_valid_ascii(const unsigned char *s, int len)
{
	const unsigned char *const s_end = s + len;
	Vector8		chunk;
	Vector8		highbit_cum = vector8_broadcast(0);
#ifdef USE_NO_SIMD
	Vector8		zero_cum = vector8_broadcast(0x80);
#endif

	Assert(len % sizeof(chunk) == 0);

	while (s < s_end)
	{
		vector8_load(&chunk, s);

		/* 捕获本块中的任何零字节。 */
#ifdef USE_NO_SIMD

		/*
		 * 首先给每字节加 0x7f。这会置起每字节高位，
		 * 除非原值为零。若结果中任何高位为零，则零
		 * 累加器中对应高位会被清掉。
		 *
		 * 若块中没有任何字节高位被置，则加法后每字节
		 * 最大值可为 0x7f + 0x7f = 0xfe，不必担心进位
		 * 到下一字节。若有输入字节高位被置，则无所谓，
		 * 因为我们会另行检查那些字节。
		 * 见上。
		 */
		zero_cum &= (chunk + vector8_broadcast(0x7F));
#else

		/*
		 * 在高位累加器各通道中，对输入字节为零的位置
		 * 置满位。
		 */
		highbit_cum = vector8_or(highbit_cum,
								 vector8_eq(chunk, vector8_broadcast(0)));
#endif

		/* 捕获本块中所有置位。 */
		highbit_cum = vector8_or(highbit_cum, chunk);

		s += sizeof(chunk);
	}

	/* 检查高位累加器中是否有任何高位被置。 */
	if (vector8_is_highbit_set(highbit_cum))
		return false;

#ifdef USE_NO_SIMD
	/* 检查零累加器中是否有任何高位被清。 */
	if (zero_cum != vector8_broadcast(0x80))
		return false;
#endif

	return true;
}

#endif							/* _ASCII_H_ */
