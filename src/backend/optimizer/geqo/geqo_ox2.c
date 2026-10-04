/*------------------------------------------------------------------------
*
* geqo_ox2.c
*
*	 order crossover [OX] routines;
*	 OX2 operator according to Syswerda
*	 (The Genetic Algorithms Handbook, ed L Davis)
*
* src/backend/optimizer/geqo/geqo_ox2.c
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

/* ox 算法取自 Genitor： */
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

#if defined(OX2)

/* ox2
 *
 *	 位置交叉
 */
void
ox2(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring, int num_gene, City * city_table)
{
	int			k,
				j,
				count,
				pos,
				select,
				num_positions;

	/* 初始化城市表 */
	for (k = 1; k <= num_gene; k++)
	{
		city_table[k].used = 0;
		city_table[k - 1].select_list = -1;
	}

	/* 确定要从 tour1 继承的位置个数  */
	num_positions = geqo_randint(root, 2 * num_gene / 3, num_gene / 3);

	/* 列出被选中的城市 */
	for (k = 0; k < num_positions; k++)
	{
		pos = geqo_randint(root, num_gene - 1, 0);
		city_table[pos].select_list = (int) tour1[pos];
		city_table[(int) tour1[pos]].used = 1;	/* 标为已使用 */
	}


	count = 0;
	k = 0;

	/* 把选择列表收拢到相邻位置 */
	while (count < num_positions)
	{
		if (city_table[k].select_list == -1)
		{
			j = k + 1;
			while ((city_table[j].select_list == -1) && (j < num_gene))
				j++;

			city_table[k].select_list = city_table[j].select_list;
			city_table[j].select_list = -1;
			count++;
		}
		else
			count++;
		k++;
	}

	select = 0;

	for (k = 0; k < num_gene; k++)
	{
		if (city_table[(int) tour2[k]].used)
		{
			offspring[k] = (Gene) city_table[select].select_list;
			select++;			/* 选择列表中的下一座城市   */
		}
		else
			/* 城市尚未使用，因此从 tour2 继承 */
			offspring[k] = tour2[k];
	}
}

#endif							/* 当 defined(OX2) */
