/*-------------------------------------------------------------------------
 *
 * knapsack.c
 *	  Knapsack problem solver
 *
 * Given input vectors of integral item weights (must be >= 0) and values
 * (double >= 0), compute the set of items which produces the greatest total
 * value without exceeding a specified total weight; each item is included at
 * most once (this is the 0/1 knapsack problem).  Weight 0 items will always be
 * included.
 *
 * The performance of this algorithm is pseudo-polynomial, O(nW) where W is the
 * weight limit.  To use with non-integral weights or approximate solutions,
 * the caller should pre-scale the input weights to a suitable range.  This
 * allows approximate solutions in polynomial time (the general case of the
 * exact problem is NP-hard).
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/knapsack.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "lib/knapsack.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/*
 * DiscreteKnapsack
 *
 * item_values 输入是可选的；若省略，则假定所有项的
 * 价值为 1。
 *
 * 返回所选纳入解中的项的 0..(n-1) 下标组成的
 * Bitmapset。
 *
 * 这使用通常的动态规划算法，改编为每趟复用
 * 内存（从较大重量做到较小重量）。在第 i 趟
 * 开始时，values[w] 数组包含只用下标 < i 的项、
 * 在总重量 <= w 下算出的最大价值；sets[w]
 * 包含实际用于该价值的项的位图。（所有
 * bitmapset 都预置了一个未用的高位，因此内存
 * 分配只做一次。）
 */
Bitmapset *
DiscreteKnapsack(int max_weight, int num_items,
				 int *item_weights, double *item_values)
{
	MemoryContext local_ctx = AllocSetContextCreate(CurrentMemoryContext,
													"Knapsack",
													ALLOCSET_SMALL_SIZES);
	MemoryContext oldctx = MemoryContextSwitchTo(local_ctx);
	double	   *values;
	Bitmapset **sets;
	Bitmapset  *result;
	int			i,
				j;

	Assert(max_weight >= 0);
	Assert(num_items > 0 && item_weights);

	values = palloc((1 + max_weight) * sizeof(double));
	sets = palloc((1 + max_weight) * sizeof(Bitmapset *));

	for (i = 0; i <= max_weight; ++i)
	{
		values[i] = 0;
		sets[i] = bms_make_singleton(num_items);
	}

	for (i = 0; i < num_items; ++i)
	{
		int			iw = item_weights[i];
		double		iv = item_values ? item_values[i] : 1;

		for (j = max_weight; j >= iw; --j)
		{
			int			ow = j - iw;

			if (values[j] <= values[ow] + iv)
			{
				/* 把 sets[ow] 复制到 sets[j]，不重新分配 */
				if (j != ow)
				{
					sets[j] = bms_del_members(sets[j], sets[j]);
					sets[j] = bms_add_members(sets[j], sets[ow]);
				}

				sets[j] = bms_add_member(sets[j], i);

				values[j] = values[ow] + iv;
			}
		}
	}

	MemoryContextSwitchTo(oldctx);

	result = bms_del_member(bms_copy(sets[max_weight]), num_items);

	MemoryContextDelete(local_ctx);

	return result;
}
