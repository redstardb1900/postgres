/*------------------------------------------------------------------------
*
* geqo_recombination.c
*	 misc recombination procedures
*
* src/backend/optimizer/geqo/geqo_recombination.c
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

/* -- 部分内容改编自 D. Whitley 的 Genitor 算法 -- */

#include "postgres.h"

#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"


/*
 * init_tour
 *
 *	 随机生成一条合法的 "旅行商" 巡回
 *	 （即每个点只被访问一次。）
 */
void
init_tour(PlannerInfo *root, Gene *tour, int num_gene)
{
	int			i,
				j;

	/*
	 * 我们必须用 1 .. num_gene 这些数的一个随机排列填满 tour[] 数组。
	 * 可以用一遍扫描完成，采用 "inside-out"
	 * 变体的 Fisher-Yates 洗牌算法。概念上，我们把
	 * 每个新值追加到数组，再把它与随机选出的
	 * 数组元素交换（可能包括它自己，否则就产生不了
	 * 最后一座城市留在最后的那些排列）。交换这一步可以优化，
	 * 办法是把它和插入合并。
	 */
	if (num_gene > 0)
		tour[0] = (Gene) 1;

	for (i = 1; i < num_gene; i++)
	{
		j = geqo_randint(root, i, 0);
		/* i != j 的检查避免读取未初始化的数组元素 */
		if (i != j)
			tour[i] = tour[j];
		tour[j] = (Gene) (i + 1);
	}
}

/* 以下重组方法会用到城市表： */
#if defined(CX) || defined(PX) || defined(OX1) || defined(OX2)

/* alloc_city_table
 *
 *	 为城市表分配内存
 */
City *
alloc_city_table(PlannerInfo *root, int num_gene)
{
	City	   *city_table;

	/*
	 * 额外 palloc 一个位置，使编号 1..n 的节点能直接
	 * 按下标访问；0 不会被使用
	 */
	city_table = (City *) palloc((num_gene + 1) * sizeof(City));

	return city_table;
}

/* free_city_table
 *
 *	  释放城市表的内存
 */
void
free_city_table(PlannerInfo *root, City * city_table)
{
	pfree(city_table);
}

#endif							/* 当 CX || PX || OX1 || OX2 */
