/*-------------------------------------------------------------------------
 *
 * qunique.h
 *		inline array unique functions
 * Portions Copyright (c) 2019-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/lib/qunique.h
 *-------------------------------------------------------------------------
 */

#ifndef QUNIQUE_H
#define QUNIQUE_H

/*
 * 按调用者提供的比较函数，从已预先排序的数组中去掉
 * 重复项。数组通常应已用相同参数调用 qsort() 排好序。
 * 返回新的大小。
 */
static inline size_t
qunique(void *array, size_t elements, size_t width,
		int (*compare) (const void *, const void *))
{
	char	   *bytes = (char *) array;
	size_t		i,
				j;

	if (elements <= 1)
		return elements;

	for (i = 1, j = 0; i < elements; ++i)
	{
		if (compare(bytes + i * width, bytes + j * width) != 0 &&
			++j != i)
			memcpy(bytes + j * width, bytes + i * width, width);
	}

	return j + 1;
}

/*
 * 同 qunique()，但比较函数多一个用户数据参数，该参数
 * 会被原样传入，以便与 qsort_arg() 兼容。
 */
static inline size_t
qunique_arg(void *array, size_t elements, size_t width,
			int (*compare) (const void *, const void *, void *),
			void *arg)
{
	char	   *bytes = (char *) array;
	size_t		i,
				j;

	if (elements <= 1)
		return elements;

	for (i = 1, j = 0; i < elements; ++i)
	{
		if (compare(bytes + i * width, bytes + j * width, arg) != 0 &&
			++j != i)
			memcpy(bytes + j * width, bytes + i * width, width);
	}

	return j + 1;
}

#endif							/* QUNIQUE_H */
