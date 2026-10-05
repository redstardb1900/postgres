/*-------------------------------------------------------------------------
 *
 * hyperloglog.c
 *	  HyperLogLog 基数估计器
 *
 * Portions Copyright (c) 2014-2023, PostgreSQL Global Development Group
 *
 * 基于 Hideaki Ohno 的 C++ 实现。它大概并不理想地
 * 适合估计非常大的集合的基数；尤其是，我们
 * 并未尝试按下列论文所述进一步优化该实现：
 * the Heule, Nunkesser and Hall paper "HyperLogLog in Practice: Algorithmic
 * Engineering of a State of The Art Cardinality Estimation Algorithm".
 *
 * 采用 HyperLogLog 状态的稀疏表示，空间开销
 * 是固定的。
 *
 * The copyright terms of Ohno's original version (the MIT license) follow.
 *
 * IDENTIFICATION
 *	  src/backend/lib/hyperloglog.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Copyright (c) 2013 Hideaki Ohno <hide.o.j55{at}gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the 'Software'), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "postgres.h"

#include <math.h>

#include "lib/hyperloglog.h"
#include "port/pg_bitutils.h"

#define POW_2_32			(4294967296.0)
#define NEG_POW_2_32		(-4294967296.0)

static inline uint8 rho(uint32 x, uint8 b);

/*
 * 按位宽初始化 HyperLogLog 跟踪状态
 *
 * bwidth 是位宽（因此寄存器大小将是 2 的 bwidth 次
 * 幂）。必须在 4 到 16 之间（含）。
 */
void
initHyperLogLog(hyperLogLogState *cState, uint8 bwidth)
{
	double		alpha;

	if (bwidth < 4 || bwidth > 16)
		elog(ERROR, "bit width must be between 4 and 16 inclusive");

	cState->registerWidth = bwidth;
	cState->nRegisters = (Size) 1 << bwidth;
	cState->arrSize = sizeof(uint8) * cState->nRegisters + 1;

	/*
	 * 按 HyperLogLog 论文中对优惠券收集问题的讨论，
	 * 把 hashes 数组初始化为零，而不是负无穷
	 */
	cState->hashesArr = palloc0(cState->arrSize);

	/*
	 * "alpha" 是对每种可能的寄存器数 (m) 用来校正
	 * m ^ 2 Z 中存在的系统性乘性偏差的值（Z 是
	 * "指示函数"，我们最终通过它计算 E，即估计
	 * 基数）。
	 */
	switch (cState->nRegisters)
	{
		case 16:
			alpha = 0.673;
			break;
		case 32:
			alpha = 0.697;
			break;
		case 64:
			alpha = 0.709;
			break;
		default:
			alpha = 0.7213 / (1.0 + 1.079 / cState->nRegisters);
	}

	/*
	 * 预计算 alpha m ^ 2，稍后用来生成 "raw"
	 * HyperLogLog 估计 E
	 */
	cState->alphaMM = alpha * cState->nRegisters * cState->nRegisters;
}

/*
 * 按误差率初始化 HyperLogLog 跟踪状态
 *
 * 不指定 bwidth（用于寻址寄存器的位数），本方法
 * 允许用论文中的简单公式按特定误差率为计数器
 * 定大小：
 *
 *	 e = 1.04 / sqrt(m)
 *
 * 其中 'm' 是寄存器数，即 (2^bwidth)。本方法找出
 * 'e' 低于所请求误差率的最低 bwidth，然后用它
 * 初始化计数器。
 *
 * 由于 bwidth 必须在 4 到 16 之间，最坏可能误差率
 * 在约 25% (bwidth=4) 和 0.4% (bwidth=16) 之间。
 */
void
initHyperLogLogError(hyperLogLogState *cState, double error)
{
	uint8		bwidth = 4;

	while (bwidth < 16)
	{
		double		m = (Size) 1 << bwidth;

		if (1.04 / sqrt(m) < error)
			break;
		bwidth++;
	}

	initHyperLogLog(cState, bwidth);
}

/*
 * 释放 HyperLogLog 跟踪状态
 *
 * 释放已分配资源，但不释放状态本身（以防它不是
 * 由 palloc 分配的）。
 */
void
freeHyperLogLog(hyperLogLogState *cState)
{
	Assert(cState->hashesArr != NULL);
	pfree(cState->hashesArr);
}

/*
 * 从调用者提供的哈希向估计器添加元素。
 *
 * 传入的哈希值必须是真正的哈希值，这一点至关重要，
 * 通常用 hash_any() 生成。算法依赖与随机平均相结合
 * 可观察到的特定位模式。对每个观察到的不同
 * 原始值，哈希值中的位必须均匀
 * 分布。
 */
void
addHyperLogLog(hyperLogLogState *cState, uint32 hash)
{
	uint8		count;
	uint32		index;

	/* 用前 "k" (registerWidth) 位作为从零开始的下标 */
	index = hash >> (BITS_PER_BYTE * sizeof(uint32) - cState->registerWidth);

	/* 计算剩余 32 - "k" (registerWidth) 位的秩 */
	count = rho(hash << cState->registerWidth,
				BITS_PER_BYTE * sizeof(uint32) - cState->registerWidth);

	cState->hashesArr[index] = Max(count, cState->hashesArr[index]);
}

/*
 * 根据迄今加入的元素估计基数
 */
double
estimateHyperLogLog(hyperLogLogState *cState)
{
	double		result;
	double		sum = 0.0;
	int			i;

	for (i = 0; i < cState->nRegisters; i++)
	{
		sum += 1.0 / pow(2.0, cState->hashesArr[i]);
	}

	/* 结果设为 "raw" HyperLogLog 估计（HyperLogLog 论文中的 E） */
	result = cState->alphaMM / sum;

	if (result <= (5.0 / 2.0) * cState->nRegisters)
	{
		/* 小范围校正 */
		int			zero_count = 0;

		for (i = 0; i < cState->nRegisters; i++)
		{
			if (cState->hashesArr[i] == 0)
				zero_count++;
		}

		if (zero_count != 0)
			result = cState->nRegisters * log((double) cState->nRegisters /
											  zero_count);
	}
	else if (result > (1.0 / 30.0) * POW_2_32)
	{
		/* 大范围校正 */
		result = NEG_POW_2_32 * log(1.0 - (result / POW_2_32));
	}

	return result;
}

/*
 * addHyperLogLog() 的辅助函数。
 *
 * 计算 x 参数前 b 位中第一个置位的位置，从第一位
 * 开始，从最高有效位读到最低有效
 * 位。
 *
 * 例子（考虑 x 的前 10 位时）：
 *
 * rho(x = 0b1000000000)   returns 1
 * rho(x = 0b0010000000)   returns 3
 * rho(x = 0b0000000000)   returns b + 1
 *
 * "The binary address determined by the first b bits of x"
 *
 * 返回值 "j" 用来索引要观察的位模式。
 */
static inline uint8
rho(uint32 x, uint8 b)
{
	uint8		j = 1;

	if (x == 0)
		return b + 1;

	j = 32 - pg_leftmost_one_pos32(x);

	if (j > b)
		return b + 1;

	return j;
}
