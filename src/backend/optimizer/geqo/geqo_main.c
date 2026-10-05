/*------------------------------------------------------------------------
 *
 * geqo_main.c
 *	  查询规划问题的求解
 *	  借助于遗传算法（GA）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_main.c
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

#include <math.h>

#include "optimizer/geqo_misc.h"
#include "optimizer/geqo_mutation.h"
#include "optimizer/geqo_pool.h"
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_selection.h"


/*
 * 配置选项
 */
int			Geqo_effort;
int			Geqo_pool_size;
int			Geqo_generations;
double		Geqo_selection_bias;
double		Geqo_seed;


static int	gimme_pool_size(int nr_rel);
static int	gimme_number_generations(int pool_size);

/* 若没有用 #define 指定重组机制则报错 */
#if !defined(ERX) && \
	!defined(PMX) && \
	!defined(CX)  && \
	!defined(PX)  && \
	!defined(OX1) && \
	!defined(OX2)
#error "must choose one GEQO recombination mechanism in geqo.h"
#endif


/*
 * geqo
 *	  查询优化问题的求解
 *	  类似于带约束的旅行商问题（TSP）
 */

RelOptInfo *
geqo(PlannerInfo *root, int number_of_rels, List *initial_rels)
{
	GeqoPrivateData private;
	int			generation;
	Chromosome *momma;
	Chromosome *daddy;
	Chromosome *kid;
	Pool	   *pool;
	int			pool_size,
				number_generations;

#ifdef GEQO_DEBUG
	int			status_interval;
#endif
	Gene	   *best_tour;
	RelOptInfo *best_rel;

#if defined(ERX)
	Edge	   *edge_table;		/* 边的列表 */
	int			edge_failures = 0;
#endif
#if defined(CX) || defined(PX) || defined(OX1) || defined(OX2)
	City	   *city_table;		/* 城市的列表 */
#endif
#if defined(CX)
	int			cycle_diffs = 0;
	int			mutations = 0;
#endif

/* 建立私有信息 */
	root->join_search_private = (void *) &private;
	private.initial_rels = initial_rels;

/* 初始化私有随机数生成器 */
	geqo_set_seed(root, Geqo_seed);

/* 设置 GA 参数 */
	pool_size = gimme_pool_size(number_of_rels);
	number_generations = gimme_number_generations(pool_size);
#ifdef GEQO_DEBUG
	status_interval = 10;
#endif

/* 分配基因池内存 */
	pool = alloc_pool(root, pool_size, number_of_rels);

/* 随机初始化基因池 */
	random_init_pool(root, pool);

/* 按最低代价路径作为适应度对池排序 */
	sort_pool(root, pool);		/* 只需要做一次，因为此后所有
								 * 子代都会替换掉种群里最差的个体
								 * （见 geqo_pool.c:spread_chromo） */

#ifdef GEQO_DEBUG
	elog(DEBUG1, "GEQO selected %d pool entries, best %.2f, worst %.2f",
		 pool_size,
		 pool->data[0].worth,
		 pool->data[pool_size - 1].worth);
#endif

/* 分配母本与父本染色体的内存 */
	momma = alloc_chromo(root, pool->string_length);
	daddy = alloc_chromo(root, pool->string_length);

#if defined (ERX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using edge recombination crossover [ERX]");
#endif
/* 分配边表内存 */
	edge_table = alloc_edge_table(root, pool->string_length);
#elif defined(PMX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using partially matched crossover [PMX]");
#endif
/* 分配子代染色体的内存 */
	kid = alloc_chromo(root, pool->string_length);
#elif defined(CX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using cycle crossover [CX]");
#endif
/* 分配城市表内存 */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(PX)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using position crossover [PX]");
#endif
/* 分配城市表内存 */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(OX1)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using order crossover [OX1]");
#endif
/* 分配城市表内存 */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#elif defined(OX2)
#ifdef GEQO_DEBUG
	elog(DEBUG2, "using order crossover [OX2]");
#endif
/* 分配城市表内存 */
	kid = alloc_chromo(root, pool->string_length);
	city_table = alloc_city_table(root, pool->string_length);
#endif


/* 主要部分： */
/* 迭代优化 */

	for (generation = 0; generation < number_generations; generation++)
	{
		/* SELECTION：使用线性偏置函数 */
		geqo_selection(root, momma, daddy, pool, Geqo_selection_bias);

#if defined (ERX)
		/* EDGE RECOMBINATION CROSSOVER（边重组交叉） */
		gimme_edge_table(root, momma->string, daddy->string, pool->string_length, edge_table);

		kid = momma;

		/* 是否出现边失败？ */
		edge_failures += gimme_tour(root, edge_table, kid->string, pool->string_length);
#elif defined(PMX)
		/* PARTIALLY MATCHED CROSSOVER（部分匹配交叉） */
		pmx(root, momma->string, daddy->string, kid->string, pool->string_length);
#elif defined(CX)
		/* CYCLE CROSSOVER（循环交叉） */
		cycle_diffs = cx(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
		/* 对子代做变异 */
		if (cycle_diffs == 0)
		{
			mutations++;
			geqo_mutation(root, kid->string, pool->string_length);
		}
#elif defined(PX)
		/* POSITION CROSSOVER（位置交叉） */
		px(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX1)
		/* ORDER CROSSOVER（顺序交叉） */
		ox1(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#elif defined(OX2)
		/* ORDER CROSSOVER（顺序交叉） */
		ox2(root, momma->string, daddy->string, kid->string, pool->string_length, city_table);
#endif


		/* EVALUATE FITNESS（评估适应度） */
		kid->worth = geqo_eval(root, kid->string, pool->string_length);

		/* 按其 worth 把子代推进生活的旷野 */
		spread_chromo(root, kid, pool);


#ifdef GEQO_DEBUG
		if (status_interval && !(generation % status_interval))
			print_gen(stdout, pool, generation);
#endif

	}


#if defined(ERX)
#if defined(GEQO_DEBUG)
	if (edge_failures != 0)
		elog(LOG, "[GEQO] failures: %d, average: %d",
			 edge_failures, (int) number_generations / edge_failures);
	else
		elog(LOG, "[GEQO] no edge failures detected");
#else
	/* 抑制某些编译器关于变量已赋值但未使用的告警 */
	(void) edge_failures;
#endif
#endif

#if defined(CX) && defined(GEQO_DEBUG)
	if (mutations != 0)
		elog(LOG, "[GEQO] mutations: %d, generations: %d",
			 mutations, number_generations);
	else
		elog(LOG, "[GEQO] no mutations processed");
#endif

#ifdef GEQO_DEBUG
	print_pool(stdout, pool, 0, pool_size - 1);
#endif

#ifdef GEQO_DEBUG
	elog(DEBUG1, "GEQO best is %.2f after %d generations",
		 pool->data[0].worth, number_generations);
#endif


	/*
	 * 已经得到 geqo 处理过的最低代价查询树；种群的第一个
	 * 元素表示最佳查询树
	 */
	best_tour = (Gene *) pool->data[0].string;

	best_rel = gimme_tree(root, best_tour, pool->string_length);

	if (best_rel == NULL)
		elog(ERROR, "geqo failed to make a valid plan");

	/* DBG：显示查询计划 */
#ifdef NOT_USED
	print_plan(best_plan, root);
#endif

	/* ……释放内存 */
	free_chromo(root, momma);
	free_chromo(root, daddy);

#if defined (ERX)
	free_edge_table(root, edge_table);
#elif defined(PMX)
	free_chromo(root, kid);
#elif defined(CX)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(PX)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(OX1)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#elif defined(OX2)
	free_chromo(root, kid);
	free_city_table(root, city_table);
#endif

	free_pool(root, pool);

	/* ……清掉指向私有存储的 root 指针 */
	root->join_search_private = NULL;

	return best_rel;
}


/*
 * 返回配置的池大小，或者一个合适的默认值
 *
 * 默认值基于查询规模（关系数）= 2^(QS+1)，
 * 但被限制在由 effort 值决定的范围内。
 */
static int
gimme_pool_size(int nr_rel)
{
	double		size;
	int			minsize;
	int			maxsize;

	/* 合法的池大小必须至少为 2，因此忽略选成 1 的尝试 */
	if (Geqo_pool_size >= 2)
		return Geqo_pool_size;

	size = pow(2.0, nr_rel + 1.0);

	maxsize = 50 * Geqo_effort; /* 50 到 500 个个体 */
	if (size > maxsize)
		return maxsize;

	minsize = 10 * Geqo_effort; /* 10 到 100 个个体 */
	if (size < minsize)
		return minsize;

	return (int) ceil(size);
}


/*
 * 返回配置的世代数，或者一个合适的默认值
 *
 * 默认值与池大小相同，这样可以
 * 确保适应度较低的个体在运行结束前被挤出
 * 繁殖种群。
 */
static int
gimme_number_generations(int pool_size)
{
	if (Geqo_generations > 0)
		return Geqo_generations;

	return pool_size;
}
