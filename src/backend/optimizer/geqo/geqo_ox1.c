/*------------------------------------------------------------------------
*
* geqo_ox1.c
*
*	 order crossover [OX] routines;
*	 OX1 operator according to Davis
*	 (Proc Int'l Joint Conf on AI)
*
* src/backend/optimizer/geqo/geqo_ox1.c
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

#if defined(OX1)

/* ox1
 *
 *	 位置交叉
 */
void
ox1(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring, int num_gene,
	City * city_table)
{
	int			left,
				right,
				k,
				p,
				temp;

	/* 初始化城市表 */
	for (k = 1; k <= num_gene; k++)
		city_table[k].used = 0;

	/* 选择要从 tour1 复制的那一段 */
	left = geqo_randint(root, num_gene - 1, 0);
	right = geqo_randint(root, num_gene - 1, 0);

	if (left > right)
	{
		temp = left;
		left = right;
		right = temp;
	}

	/* 把这一段从 tour1 复制到后代 */
	for (k = left; k <= right; k++)
	{
		offspring[k] = tour1[k];
		city_table[(int) tour1[k]].used = 1;
	}

	k = (right + 1) % num_gene; /* 后代中的下标 */
	p = k;						/* tour2 中的下标 */

	/* 把 tour2 中的内容复制到后代 */
	while (k != left)
	{
		if (!city_table[(int) tour2[p]].used)
		{
			offspring[k] = tour2[p];
			k = (k + 1) % num_gene;
			city_table[(int) tour2[p]].used = 1;
		}
		p = (p + 1) % num_gene; /* 递增 tour2 的下标 */
	}
}

#endif							/* 当 defined(OX1) */
