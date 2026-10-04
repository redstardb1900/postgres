/*------------------------------------------------------------------------
 *
 * geqo_pool.c
 *	  Genetic Algorithm (GA) pool stuff
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_pool.c
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

#include <float.h>
#include <limits.h>
#include <math.h>

#include "optimizer/geqo_copy.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_recombination.h"


static int	compare(const void *arg1, const void *arg2);

/*
 * alloc_pool
 *		为 GA 池分配内存
 */
Pool *
alloc_pool(PlannerInfo *root, int pool_size, int string_length)
{
	Pool	   *new_pool;
	Chromosome *chromo;
	int			i;

	/* 池 */
	new_pool = (Pool *) palloc(sizeof(Pool));
	new_pool->size = (int) pool_size;
	new_pool->string_length = (int) string_length;

	/* 全部染色体 */
	new_pool->data = (Chromosome *) palloc(pool_size * sizeof(Chromosome));

	/* 全部基因 */
	chromo = (Chromosome *) new_pool->data; /* 全部染色体的向量 */
	for (i = 0; i < pool_size; i++)
		chromo[i].string = palloc((string_length + 1) * sizeof(Gene));

	return new_pool;
}

/*
 * free_pool
 *		释放 GA 池的内存
 */
void
free_pool(PlannerInfo *root, Pool *pool)
{
	Chromosome *chromo;
	int			i;

	/* 全部基因 */
	chromo = (Chromosome *) pool->data; /* 全部染色体的向量 */
	for (i = 0; i < pool->size; i++)
		pfree(chromo[i].string);

	/* 全部染色体 */
	pfree(pool->data);

	/* 池 */
	pfree(pool);
}

/*
 * random_init_pool
 *		初始化基因池
 */
void
random_init_pool(PlannerInfo *root, Pool *pool)
{
	Chromosome *chromo = (Chromosome *) pool->data;
	int			i;
	int			bad = 0;

	/*
	 * 立刻丢掉任何无效个体（即 geqo_eval
	 * 对其返回 DBL_MAX 的那些），以免浪费池空间。
	 *
	 * 若尝试 10000 次仍造不出任何有效个体，就放弃；
	 * 这多半说明某处坏了，不应当就这样
	 * 让自己陷在无限循环里。
	 */
	i = 0;
	while (i < pool->size)
	{
		init_tour(root, chromo[i].string, pool->string_length);
		pool->data[i].worth = geqo_eval(root, chromo[i].string,
										pool->string_length);
		if (pool->data[i].worth < DBL_MAX)
			i++;
		else
		{
			bad++;
			if (i == 0 && bad >= 10000)
				elog(ERROR, "geqo failed to make a valid plan");
		}
	}

#ifdef GEQO_DEBUG
	if (bad > 0)
		elog(DEBUG1, "%d invalid tours found while selecting %d pool entries",
			 bad, pool->size);
#endif
}

/*
 * sort_pool
 *	 按 worth 对输入池排序，从小到大
 *
 *	 若要不同的次序，也许得修改 compare() ...
 */
void
sort_pool(PlannerInfo *root, Pool *pool)
{
	qsort(pool->data, pool->size, sizeof(Chromosome), compare);
}

/*
 * compare
 *	 供 sort_pool 使用的 qsort 比较函数
 */
static int
compare(const void *arg1, const void *arg2)
{
	const Chromosome *chromo1 = (const Chromosome *) arg1;
	const Chromosome *chromo2 = (const Chromosome *) arg2;

	if (chromo1->worth == chromo2->worth)
		return 0;
	else if (chromo1->worth > chromo2->worth)
		return 1;
	else
		return -1;
}

/* alloc_chromo
 *	  分配一条染色体及其串空间
 */
Chromosome *
alloc_chromo(PlannerInfo *root, int string_length)
{
	Chromosome *chromo;

	chromo = (Chromosome *) palloc(sizeof(Chromosome));
	chromo->string = (Gene *) palloc((string_length + 1) * sizeof(Gene));

	return chromo;
}

/* free_chromo
 *	  释放一条染色体及其串空间
 */
void
free_chromo(PlannerInfo *root, Chromosome *chromo)
{
	pfree(chromo->string);
	pfree(chromo);
}

/* spread_chromo
 *	 把新染色体插入池中，挤掉池里最差的基因
 *	 假定 最好->最差 = 最小->最大
 */
void
spread_chromo(PlannerInfo *root, Chromosome *chromo, Pool *pool)
{
	int			top,
				mid,
				bot;
	int			i,
				index;
	Chromosome	swap_chromo,
				tmp_chromo;

	/* 新染色体太差，无法使用 */
	if (chromo->worth > pool->data[pool->size - 1].worth)
		return;

	/* 用二分查找确定新染色体的下标 */

	top = 0;
	mid = pool->size / 2;
	bot = pool->size - 1;
	index = -1;

	while (index == -1)
	{
		/* 这 4 种情况找到了新位置 */

		if (chromo->worth <= pool->data[top].worth)
			index = top;
		else if (chromo->worth == pool->data[mid].worth)
			index = mid;
		else if (chromo->worth == pool->data[bot].worth)
			index = bot;
		else if (bot - top <= 1)
			index = bot;


		/*
		 * 这两种情况会移动查找下标，因为新位置还
		 * 没有找到。
		 */

		else if (chromo->worth < pool->data[mid].worth)
		{
			bot = mid;
			mid = top + ((bot - top) / 2);
		}
		else
		{						/* 对应 (chromo->worth > pool->data[mid].worth) */
			top = mid;
			mid = top + ((bot - top) / 2);
		}
	}							/* ……对应 while */

	/* 现在已经得到 chromo 的下标 */

	/*
	 * 把从该下标起的每个基因下移一格，给 chromo 腾出位置
	 */

	/*
	 * 把新基因复制进池存储；总是替换池里最差的基因
	 */

	geqo_copy(root, &pool->data[pool->size - 1], chromo, pool->string_length);

	swap_chromo.string = pool->data[pool->size - 1].string;
	swap_chromo.worth = pool->data[pool->size - 1].worth;

	for (i = index; i < pool->size; i++)
	{
		tmp_chromo.string = pool->data[i].string;
		tmp_chromo.worth = pool->data[i].worth;

		pool->data[i].string = swap_chromo.string;
		pool->data[i].worth = swap_chromo.worth;

		swap_chromo.string = tmp_chromo.string;
		swap_chromo.worth = tmp_chromo.worth;
	}
}
