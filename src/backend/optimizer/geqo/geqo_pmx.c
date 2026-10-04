/*------------------------------------------------------------------------
*
* geqo_pmx.c
*
*	 partially matched crossover [PMX] routines;
*	 PMX operator according to Goldberg & Lingle
*	 (Proc Int'l Conf on GA's)
*
* src/backend/optimizer/geqo/geqo_pmx.c
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

/* pmx 算法取自 Genitor： */
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

#if defined(PMX)

/* pmx
 *
 *	 部分匹配交叉
 */
void
pmx(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring, int num_gene)
{
	int		   *failed = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *from = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *indx = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *check_list = (int *) palloc((num_gene + 1) * sizeof(int));

	int			left,
				right,
				temp,
				i,
				j,
				k;
	int			mx_fail,
				found,
				mx_hold;


/* 不做变异，因此启动 pmx 替换算法 */
/* 初始化 failed[]、from[]、check_list[] */
	for (k = 0; k < num_gene; k++)
	{
		failed[k] = -1;
		from[k] = -1;
		check_list[k + 1] = 0;
	}

/* 定位交叉点 */
	left = geqo_randint(root, num_gene - 1, 0);
	right = geqo_randint(root, num_gene - 1, 0);

	if (left > right)
	{
		temp = left;
		left = right;
		right = temp;
	}


/* 把 tour2 复制到后代 */
	for (k = 0; k < num_gene; k++)
	{
		offspring[k] = tour2[k];
		from[k] = DAD;
		check_list[tour2[k]]++;
	}

/* 把 tour1 复制到后代 */
	for (k = left; k <= right; k++)
	{
		check_list[offspring[k]]--;
		offspring[k] = tour1[k];
		from[k] = MOM;
		check_list[tour1[k]]++;
	}


/* pmx 主要部分 */

	mx_fail = 0;

/* 步骤 1 */

	for (k = left; k <= right; k++)
	{							/* 对 tour1-2 中的全部元素 */

		if (tour1[k] == tour2[k])
			found = 1;			/* 在 tour2 中寻找匹配 */

		else
		{
			found = 0;			/* 替换元素 */

			j = 0;
			while (!(found) && (j < num_gene))
			{
				if ((offspring[j] == tour1[k]) && (from[j] == DAD))
				{

					check_list[offspring[j]]--;
					offspring[j] = tour2[k];
					found = 1;
					check_list[tour2[k]]++;
				}

				j++;
			}
		}

		if (!(found))
		{						/* 未能替换基因 */
			failed[mx_fail] = (int) tour1[k];
			indx[mx_fail] = k;
			mx_fail++;
		}
	}							/* ……对应 for */


/* 步骤 2 */

	/* 查看是否有基因无法替换 */
	if (mx_fail > 0)
	{
		mx_hold = mx_fail;

		for (k = 0; k < mx_hold; k++)
		{
			found = 0;

			j = 0;
			while (!(found) && (j < num_gene))
			{

				if ((failed[k] == (int) offspring[j]) && (from[j] == DAD))
				{
					check_list[offspring[j]]--;
					offspring[j] = tour2[indx[k]];
					check_list[tour2[indx[k]]]++;

					found = 1;
					failed[k] = -1;
					mx_fail--;
				}

				j++;
			}
		}						/* ……对应 for	 */
	}							/* ……对应 if	 */


/* 步骤 3 */

	for (k = 1; k <= num_gene; k++)
	{

		if (check_list[k] > 1)
		{
			i = 0;

			while (i < num_gene)
			{
				if ((offspring[i] == (Gene) k) && (from[i] == DAD))
				{
					j = 1;

					while (j <= num_gene)
					{
						if (check_list[j] == 0)
						{
							offspring[i] = (Gene) j;
							check_list[k]--;
							check_list[j]++;
							i = num_gene + 1;
							j = i;
						}

						j++;
					}
				}				/* ……对应 if	 */

				i++;
			}					/* while 结束 */
		}
	}							/* ……对应 for	 */

	pfree(failed);
	pfree(from);
	pfree(indx);
	pfree(check_list);
}

#endif							/* 当 defined(PMX) */
