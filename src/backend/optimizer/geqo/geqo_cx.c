/*------------------------------------------------------------------------
*
* geqo_cx.c
*
*	 cycle crossover [CX] routines;
*	 CX operator according to Oliver et al
*	 (Proc 2nd Int'l Conf on GA's)
*
* src/backend/optimizer/geqo/geqo_cx.c
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

/* cx 算法取自 Genitor： */
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
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"

#if defined(CX)

/* cx
 *
 *	 循环交叉
 */
int
cx(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring,
   int num_gene, City * city_table)
{
	int			i,
				start_pos,
				curr_pos;
	int			count = 0;
	int			num_diffs = 0;

	/* 初始化城市表 */
	for (i = 1; i <= num_gene; i++)
	{
		city_table[i].used = 0;
		city_table[tour2[i - 1]].tour2_position = i - 1;
		city_table[tour1[i - 1]].tour1_position = i - 1;
	}

	/* 随机选择循环起点 */
	start_pos = geqo_randint(root, num_gene - 1, 0);

	/* 子代继承第一座城市  */
	offspring[start_pos] = tour1[start_pos];

	/* 从 tour1 开始循环 */
	curr_pos = start_pos;
	city_table[(int) tour1[start_pos]].used = 1;

	count++;

	/* cx 主要部分 */


/* 步骤 1 */

	while (tour2[curr_pos] != tour1[start_pos])
	{
		city_table[(int) tour2[curr_pos]].used = 1;
		curr_pos = city_table[(int) tour2[curr_pos]].tour1_position;
		offspring[curr_pos] = tour1[curr_pos];
		count++;
	}


/* 步骤 2 */

	/* 未能生成完整巡回 */
	if (count < num_gene)
	{
		for (i = 1; i <= num_gene; i++)
		{
			if (!city_table[i].used)
			{
				offspring[city_table[i].tour2_position] =
					tour2[(int) city_table[i].tour2_position];
				count++;
			}
		}
	}


/* 步骤 3 */

	/* 仍未能生成完整巡回 */
	if (count < num_gene)
	{

		/* 统计母本与后代之间的差异数 */
		for (i = 0; i < num_gene; i++)
			if (tour1[i] != offspring[i])
				num_diffs++;
	}

	return num_diffs;
}

#endif							/* 当 defined(CX) */
