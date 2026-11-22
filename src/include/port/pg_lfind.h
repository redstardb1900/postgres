/*-------------------------------------------------------------------------
 *
 * pg_lfind.h
 *	  Optimized linear search routines using SIMD intrinsics where
 *	  available.
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/port/pg_lfind.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LFIND_H
#define PG_LFIND_H

#include "port/simd.h"

/*
 * pg_lfind8
 *
 * Return true if there is an element in 'base' that equals 'key', otherwise
 * return false.
 */
static inline bool
pg_lfind8(uint8 key, uint8 *base, uint32 nelem)
{
	uint32		i;

	/*  向下取整到向量长度的倍数  */
	uint32		tail_idx = nelem & ~(sizeof(Vector8) - 1);
	Vector8		chunk;

	for (i = 0; i < tail_idx; i += sizeof(Vector8))
	{
		vector8_load(&chunk, &base[i]);
		if (vector8_has(chunk, key))
			return true;
	}

	/*  逐个处理剩余元素。  */
	for (; i < nelem; i++)
	{
		if (key == base[i])
			return true;
	}

	return false;
}

/*
 * pg_lfind8_le
 *
 * Return true if there is an element in 'base' that is less than or equal to
 * 'key', otherwise return false.
 */
static inline bool
pg_lfind8_le(uint8 key, uint8 *base, uint32 nelem)
{
	uint32		i;

	/*  向下取整到向量长度的倍数  */
	uint32		tail_idx = nelem & ~(sizeof(Vector8) - 1);
	Vector8		chunk;

	for (i = 0; i < tail_idx; i += sizeof(Vector8))
	{
		vector8_load(&chunk, &base[i]);
		if (vector8_has_le(chunk, key))
			return true;
	}

	/*  逐个处理剩余元素。  */
	for (; i < nelem; i++)
	{
		if (base[i] <= key)
			return true;
	}

	return false;
}

/*
 * pg_lfind32
 *
 * Return true if there is an element in 'base' that equals 'key', otherwise
 * return false.
 */
static inline bool
pg_lfind32(uint32 key, uint32 *base, uint32 nelem)
{
	uint32		i = 0;

#ifndef USE_NO_SIMD

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.  Testing for SSE2 has showed this is ~40%
	 * faster than using a block of two registers.
	 */
	const Vector32 keys = vector32_broadcast(key);	/* load copies of key */
	const uint32 nelem_per_vector = sizeof(Vector32) / sizeof(uint32);
	const uint32 nelem_per_iteration = 4 * nelem_per_vector;

	/*  向下取整到每次迭代元素数的倍数  */
	const uint32 tail_idx = nelem & ~(nelem_per_iteration - 1);

#if defined(USE_ASSERT_CHECKING)
	bool		assert_result = false;

	/*  预计算断言检查用的结果  */
	for (i = 0; i < nelem; i++)
	{
		if (key == base[i])
		{
			assert_result = true;
			break;
		}
	}
#endif

	for (i = 0; i < tail_idx; i += nelem_per_iteration)
	{
		Vector32	vals1,
					vals2,
					vals3,
					vals4,
					result1,
					result2,
					result3,
					result4,
					tmp1,
					tmp2,
					result;

		/*  将下一块载入 4 个寄存器  */
		vector32_load(&vals1, &base[i]);
		vector32_load(&vals2, &base[i + nelem_per_vector]);
		vector32_load(&vals3, &base[i + nelem_per_vector * 2]);
		vector32_load(&vals4, &base[i + nelem_per_vector * 3]);

		/*  将每个值与 key 比较  */
		result1 = vector32_eq(keys, vals1);
		result2 = vector32_eq(keys, vals2);
		result3 = vector32_eq(keys, vals3);
		result4 = vector32_eq(keys, vals4);

		/*  将结果合并到单一变量  */
		tmp1 = vector32_or(result1, result2);
		tmp2 = vector32_or(result3, result4);
		result = vector32_or(tmp1, tmp2);

		/*  查看是否有匹配  */
		if (vector32_is_highbit_set(result))
		{
			Assert(assert_result == true);
			return true;
		}
	}
#endif							/* ! USE_NO_SIMD */

	/*  逐个处理剩余元素。  */
	for (; i < nelem; i++)
	{
		if (key == base[i])
		{
#ifndef USE_NO_SIMD
			Assert(assert_result == true);
#endif
			return true;
		}
	}

#ifndef USE_NO_SIMD
	Assert(assert_result == false);
#endif
	return false;
}

#endif							/* PG_LFIND_H */
