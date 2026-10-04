/*------------------------------------------------------------------------
*
* geqo_mutation.c
*
*	 TSP mutation routines
*
* src/backend/optimizer/geqo/geqo_mutation.c
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

/* 本实现取自 Genitor： */
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
#include "optimizer/geqo_mutation.h"
#include "optimizer/geqo_random.h"

#if defined(CX)					/* 目前只在 CX 模式下使用 */

void
geqo_mutation(PlannerInfo *root, Gene *tour, int num_gene)
{
	int			swap1;
	int			swap2;
	int			num_swaps = geqo_randint(root, num_gene / 3, 0);
	Gene		temp;


	while (num_swaps > 0)
	{
		swap1 = geqo_randint(root, num_gene - 1, 0);
		swap2 = geqo_randint(root, num_gene - 1, 0);

		while (swap1 == swap2)
			swap2 = geqo_randint(root, num_gene - 1, 0);

		temp = tour[swap1];
		tour[swap1] = tour[swap2];
		tour[swap2] = temp;


		num_swaps -= 1;
	}
}

#endif							/* 当 defined(CX) */
