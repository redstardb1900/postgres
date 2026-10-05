/*------------------------------------------------------------------------
 *
 * geqo_misc.c
 *	   杂项打印与调试代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_misc.c
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

#include "postgres.h"

#include "optimizer/geqo_misc.h"


#ifdef GEQO_DEBUG


/*
 * avg_pool
 */
static double
avg_pool(Pool *pool)
{
	int			i;
	double		cumulative = 0.0;

	if (pool->size <= 0)
		elog(ERROR, "pool_size is zero");

	/*
	 * 由于池里可能多次出现 DBL_MAX，求和之前先除以
	 * pool->size，而不是求和之后再除，以免溢出。这样会损失一点
	 * 速度和精度，但本例程只用于调试
	 * 打印，所以不太在意。
	 */
	for (i = 0; i < pool->size; i++)
		cumulative += pool->data[i].worth / pool->size;

	return cumulative;
}

/* print_pool
 */
void
print_pool(FILE *fp, Pool *pool, int start, int stop)
{
	int			i,
				j;

	/* 格外确认 start 和 stop 是有效输入 */

	if (start < 0)
		start = 0;
	if (stop > pool->size)
		stop = pool->size;

	if (start + stop > pool->size)
	{
		start = 0;
		stop = pool->size;
	}

	for (i = start; i < stop; i++)
	{
		fprintf(fp, "%d)\t", i);
		for (j = 0; j < pool->string_length; j++)
			fprintf(fp, "%d ", pool->data[i].string[j]);
		fprintf(fp, "%g\n", pool->data[i].worth);
	}

	fflush(fp);
}

/* print_gen
 *
 *	 染色体的打印：最佳、最差、均值、平均
 */
void
print_gen(FILE *fp, Pool *pool, int generation)
{
	int			lowest;

	/* 取得种群中排名最低的基因的下标。 */
	/* 用倒数第二个，因为最后一个是缓冲区。 */
	lowest = pool->size > 1 ? pool->size - 2 : 0;

	fprintf(fp,
			"%5d | Best: %g  Worst: %g  Mean: %g  Avg: %g\n",
			generation,
			pool->data[0].worth,
			pool->data[lowest].worth,
			pool->data[pool->size / 2].worth,
			avg_pool(pool));

	fflush(fp);
}


void
print_edge_table(FILE *fp, Edge *edge_table, int num_gene)
{
	int			i,
				j;

	fprintf(fp, "\nEDGE TABLE\n");

	for (i = 1; i <= num_gene; i++)
	{
		fprintf(fp, "%d :", i);
		for (j = 0; j < edge_table[i].unused_edges; j++)
			fprintf(fp, " %d", edge_table[i].edge_list[j]);
		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");

	fflush(fp);
}

#endif							/* 若 GEQO_DEBUG */
