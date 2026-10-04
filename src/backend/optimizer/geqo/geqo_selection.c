/*-------------------------------------------------------------------------
 *
 * geqo_selection.c
 *	  linear selection scheme for the genetic query optimizer
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_selection.c
 *
 *-------------------------------------------------------------------------
 */

/* 贡献者：
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * 自动控制研究所	   *
   =							 = 矿业技术大学 =
   *  utesch@aut.tu-freiberg.de  * 德国弗赖贝格				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* 本实现取自 D. Whitley 的 Genitor 算法 */

/*************************************************************/
/*															 */
/*	Copyright (c) 1990										 */
/*	Darrell L. Whitley										 */
/*	Computer Science Department								 */
/*	Colorado State University								 */
/*															 */
/*	Permission is hereby granted to copy all or any part of  */
/*	this program for free distribution.   The author's name  */
/*	and this copyright notice must be included in any copy.  */
/*															 */
/*************************************************************/

#include "postgres.h"

#include <math.h>

#include "optimizer/geqo_copy.h"
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_selection.h"

static int	linear_rand(PlannerInfo *root, int pool_size, double bias);


/*
 * geqo_selection
 *	 按输入参数描述的偏置，
 *	 从池中选出第一和第二个基因
 */
void
geqo_selection(PlannerInfo *root, Chromosome *momma, Chromosome *daddy,
			   Pool *pool, double bias)
{
	int			first,
				second;

	first = linear_rand(root, pool->size, bias);
	second = linear_rand(root, pool->size, bias);

	/*
	 * 确保选出的是不同基因，除非池大小只有
	 * 一个，那种情况下无法做到。
	 */
	if (pool->size > 1)
	{
		while (first == second)
			second = linear_rand(root, pool->size, bias);
	}

	geqo_copy(root, momma, &pool->data[first], pool->string_length);
	geqo_copy(root, daddy, &pool->data[second], pool->string_length);
}

/*
 * linear_rand
 *	  生成 0 到输入最大数之间的随机整数，
 *	  使用输入的线性偏置
 *
 *	  bias 是线性分布的 y 轴截距
 *
 *	  概率分布函数为：f(x) = bias - 2(bias - 1)x
 *			 bias = (第一条规则的概率) / (中间规则的概率)
 */
static int
linear_rand(PlannerInfo *root, int pool_size, double bias)
{
	double		index;			/* 下标介于 0 与 pool_size 之间 */
	double		max = (double) pool_size;

	/*
	 * geqo_rand() 按理不应返回 1.0，但如果返回了，这个等式就会
	 * 得到恰好为 max 的结果，而我们需要 0 <= index < max。
	 * 舍入误差似乎也可能给出
	 * 略微超出范围的值；尤其要避免把略小于 0 的值传给
	 * sqrt()。若得到坏值，就再重试一次。
	 */
	do
	{
		double		sqrtval;

		sqrtval = (bias * bias) - 4.0 * (bias - 1.0) * geqo_rand(root);
		if (sqrtval > 0.0)
			sqrtval = sqrt(sqrtval);
		index = max * (bias - sqrtval) / 2.0 / (bias - 1.0);
	} while (index < 0.0 || index >= max);

	return (int) index;
}
