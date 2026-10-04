/*------------------------------------------------------------------------
*
* geqo_px.c
*
*	 position crossover [PX] routines;
*	 PX operator according to Syswerda
*	 (The Genetic Algorithms Handbook, L Davis, ed)
*
* src/backend/optimizer/geqo/geqo_px.c
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

/* px 算法取自 Genitor： */
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

#if defined(PX)

/* px
 *
 *	 位置交叉
 */
void
px(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring, int num_gene,
   City * city_table)
{
	int			num_positions;
	int			i,
				pos,
				tour2_index,
				offspring_index;

	/* 初始化城市表 */
	for (i = 1; i <= num_gene; i++)
		city_table[i].used = 0;

	/* 选择将从亲代直接继承的随机位置 */
	num_positions = geqo_randint(root, 2 * num_gene / 3, num_gene / 3);

	/* 选择随机位置 */
	for (i = 0; i < num_positions; i++)
	{
		pos = geqo_randint(root, num_gene - 1, 0);

		offspring[pos] = tour1[pos];	/* 把城市传给子代 */
		city_table[(int) tour1[pos]].used = 1;	/* 把城市标为已使用 */
	}

	tour2_index = 0;
	offspring_index = 0;


	/* px 主要部分 */

	while (offspring_index < num_gene)
	{

		/* 后代中下一个被填上的位置 */
		if (!city_table[(int) tour1[offspring_index]].used)
		{

			/* tour1 中下一座尚未使用的城市 */
			if (!city_table[(int) tour2[tour2_index]].used)
			{

				/* 从 tour1 继承 */
				offspring[offspring_index] = tour2[tour2_index];

				tour2_index++;
				offspring_index++;
			}
			else
			{					/* tour2 中的下一座城市已经被使用 */
				tour2_index++;
			}
		}
		else
		{						/* 后代中的下一个位置已被填上 */
			offspring_index++;
		}
	}
}

#endif							/* 当 defined(PX) */
