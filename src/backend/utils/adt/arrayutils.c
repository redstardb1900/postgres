/*-------------------------------------------------------------------------
 *
 * arrayutils.c
 *	  本文件包含数组函数所需的一些支撑例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arrayutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "common/int.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


/*
 * 将下标列表转为线性元素编号（自 0 起）
 *
 * 我们假定调用方已对维度和下标做过范围检查，
 * 因此不可能溢出。
 */
int
ArrayGetOffset(int n, const int *dim, const int *lb, const int *indx)
{
	int			i,
				scale = 1,
				offset = 0;

	for (i = n - 1; i >= 0; i--)
	{
		offset += (indx[i] - lb[i]) * scale;
		scale *= dim[i];
	}
	return offset;
}

/*
 * 同上，但假定下标基于 0，并使用缩放数组
 * 而非原始维度数据（见 mda_get_prod 以创建缩放数组）
 */
int
ArrayGetOffset0(int n, const int *tup, const int *scale)
{
	int			i,
				lin = 0;

	for (i = 0; i < n; i++)
		lin += tup[i] * scale[i];
	return lin;
}

/*
 * 将数组维度转换为元素数量
 *
 * 这必须做溢出检查，因为它用于校验用户的维度
 * 请求不会溢出我们所能处理的范围。
 *
 * 乘法溢出检查只在具备 int64 算术的机器上有效，
 * 但如今几乎所有平台都是如此，而对不满足的平
 * 台做检查性除法又显得代价过高。
 */
int
ArrayGetNItems(int ndim, const int *dims)
{
	return ArrayGetNItemsSafe(ndim, dims, NULL);
}

/*
 * 此入口点可把错误返回到 ErrorSaveContext 中，
 * 而不是抛出异常。出错后返回 -1。
 */
int
ArrayGetNItemsSafe(int ndim, const int *dims, struct Node *escontext)
{
	int32		ret;
	int			i;

	if (ndim <= 0)
		return 0;
	ret = 1;
	for (i = 0; i < ndim; i++)
	{
		int64		prod;

		/* 负维度意味着 UB-LB 发生了溢出... */
		if (dims[i] < 0)
			ereturn(escontext, -1,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxArraySize)));

		prod = (int64) ret * (int64) dims[i];

		ret = (int32) prod;
		if ((int64) ret != prod)
			ereturn(escontext, -1,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxArraySize)));
	}
	Assert(ret >= 0);
	if ((Size) ret > MaxArraySize)
		ereturn(escontext, -1,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("array size exceeds the maximum allowed (%d)",
						(int) MaxArraySize)));
	return (int) ret;
}

/*
 * 校验数组拟定下界值的合理性
 *
 * 下界值不得大到在计算下标时引发溢出的程度，
 * 例如下界 2147483640 配长度 10 就必须禁止。我们
 * 实际上坚持要求 dims[i] + lb[i] 必须可无溢出
 * 地计算，这意味着最后下标等于 INT_MAX 的数组
 * 将被禁止。
 *
 * 假定调用方已经调用过 ArrayGetNItems，从而
 * 已排除溢出（负值）的 dims[] 值。
 */
void
ArrayCheckBounds(int ndim, const int *dims, const int *lb)
{
	(void) ArrayCheckBoundsSafe(ndim, dims, lb, NULL);
}

/*
 * 此入口点可把错误返回到 ErrorSaveContext 中，
 * 而不是抛出异常。
 */
bool
ArrayCheckBoundsSafe(int ndim, const int *dims, const int *lb,
					 struct Node *escontext)
{
	int			i;

	for (i = 0; i < ndim; i++)
	{
		/* PG_USED_FOR_ASSERTS_ONLY 防止变量未被读取的警告 */
		int32		sum PG_USED_FOR_ASSERTS_ONLY;

		if (pg_add_s32_overflow(dims[i], lb[i], &sum))
			ereturn(escontext, false,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array lower bound is too large: %d",
							lb[i])));
	}

	return true;
}

/*
 * 为数组切片计算范围（子数组维度）
 *
 * 我们假定调用方已校验切片端点，因此不可能溢出
 */
void
mda_get_range(int n, int *span, const int *st, const int *endp)
{
	int			i;

	for (i = 0; i < n; i++)
		span[i] = endp[i] - st[i] + 1;
}

/*
 * 计算数组维度的乘积，即下标的缩放因子
 *
 * 我们假定调用方已校验维度，因此不可能溢出
 */
void
mda_get_prod(int n, const int *range, int *prod)
{
	int			i;

	prod[n - 1] = 1;
	for (i = n - 2; i >= 0; i--)
		prod[i] = prod[i + 1] * range[i + 1];
}

/*
 * 由整个数组维度的乘积与子数组的跨度，
 * 计算在数组内步进子数组所需的偏移距离
 *
 * 我们假定调用方已校验维度，因此不可能溢出
 */
void
mda_get_offset_values(int n, int *dist, const int *prod, const int *span)
{
	int			i,
				j;

	dist[n - 1] = 0;
	for (j = n - 2; j >= 0; j--)
	{
		dist[j] = prod[j] - 1;
		for (i = j + 1; i < n; i++)
			dist[j] -= (span[i] - 1) * prod[i];
	}
}

/*
 * 生成比 "curr" 中当前 n 元组按字典序大 1 的元组，
 * 限制条件是 "curr" 的第 i 个元素小于 "span" 的
 * 第 i 个元素。
 *
 * 若不存在下一个元组则返回 -1，否则返回对应要
 * 前进维度的下标位置 (0..n-1)。
 *
 * 我们假定调用方已校验维度，因此不可能溢出
 */
int
mda_next_tuple(int n, int *curr, const int *span)
{
	int			i;

	if (n <= 0)
		return -1;

	curr[n - 1] = (curr[n - 1] + 1) % span[n - 1];
	for (i = n - 1; i && curr[i] == 0; i--)
		curr[i - 1] = (curr[i - 1] + 1) % span[i - 1];

	if (i)
		return i;
	if (curr[0])
		return 0;

	return -1;
}

/*
 * ArrayGetIntegerTypmods：校验参数是一维 cstring 数组，
 * 并把其内容转换为整数。返回一个 palloc'd 数组，
 * 并把长度放在 *n 处。
 */
int32 *
ArrayGetIntegerTypmods(ArrayType *arr, int *n)
{
	int32	   *result;
	Datum	   *elem_values;
	int			i;

	if (ARR_ELEMTYPE(arr) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("typmod array must be type cstring[]")));

	if (ARR_NDIM(arr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("typmod array must be one-dimensional")));

	if (array_contains_nulls(arr))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("typmod array must not contain nulls")));

	deconstruct_array_builtin(arr, CSTRINGOID, &elem_values, NULL, n);

	result = (int32 *) palloc(*n * sizeof(int32));

	for (i = 0; i < *n; i++)
		result[i] = pg_strtoint32(DatumGetCString(elem_values[i]));

	pfree(elem_values);

	return result;
}
