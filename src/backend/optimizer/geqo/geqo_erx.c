/*------------------------------------------------------------------------
*
* geqo_erx.c
*	 edge recombination crossover [ER]
*
* src/backend/optimizer/geqo/geqo_erx.c
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

/* 边重组算法取自 Genitor： */
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

#if defined(ERX)

static int	gimme_edge(PlannerInfo *root, Gene gene1, Gene gene2, Edge *edge_table);
static void remove_gene(PlannerInfo *root, Gene gene, Edge edge, Edge *edge_table);
static Gene gimme_gene(PlannerInfo *root, Edge edge, Edge *edge_table);

static Gene edge_failure(PlannerInfo *root, Gene *gene, int index, Edge *edge_table, int num_gene);


/* alloc_edge_table
 *
 *	 为边表分配内存
 *
 */

Edge *
alloc_edge_table(PlannerInfo *root, int num_gene)
{
	Edge	   *edge_table;

	/*
	 * 额外 palloc 一个位置，使编号 1..n 的节点能直接
	 * 按下标访问；0 不会被使用
	 */

	edge_table = (Edge *) palloc((num_gene + 1) * sizeof(Edge));

	return edge_table;
}

/* free_edge_table
 *
 *	  释放边表的内存
 *
 */
void
free_edge_table(PlannerInfo *root, Edge *edge_table)
{
	pfree(edge_table);
}

/* gimme_edge_table
 *
 *	 填充一个数据结构，表示显式
 *	 边的集合，这些边连接 (2) 个输入基因中的点
 *
 *	 假定巡回是环形的，边是双向的
 *
 *	 gimme_edge() 会把 "共享" 边设为负值
 *
 *	 返回每座城市的平均边数，范围 2.0 - 4.0
 *	 其中 2.0 表示同质；4.0 表示多样
 *
 */
float
gimme_edge_table(PlannerInfo *root, Gene *tour1, Gene *tour2,
				 int num_gene, Edge *edge_table)
{
	int			i,
				index1,
				index2;
	int			edge_total;		/* 两个基因中唯一边的总数 */

	/* 先清掉边表里的旧数据 */
	for (i = 1; i <= num_gene; i++)
	{
		edge_table[i].total_edges = 0;
		edge_table[i].unused_edges = 0;
	}

	/* 用新数据填充边表 */

	edge_total = 0;

	for (index1 = 0; index1 < num_gene; index1++)
	{
		/*
		 * 假定巡回是环形的，即 1->2、2->3、3->1；这个操作
		 * 把 n 映射回 1
		 */

		index2 = (index1 + 1) % num_gene;

		/*
		 * 边是双向的，即 1->2 与 2->1 相同；每条边调用 gimme_edge
		 * 两次
		 */

		edge_total += gimme_edge(root, tour1[index1], tour1[index2], edge_table);
		gimme_edge(root, tour1[index2], tour1[index1], edge_table);

		edge_total += gimme_edge(root, tour2[index1], tour2[index2], edge_table);
		gimme_edge(root, tour2[index2], tour2[index1], edge_table);
	}

	/* 返回每个下标的平均边数 */
	return ((float) (edge_total * 2) / (float) num_gene);
}

/* gimme_edge
 *
 *	  在输入的 edge_table 中登记从 city1 到 city2 的边
 *
 *	  不假定任何方向性；
 *	  因此要由调用例程
 *	  调用 gimme_edge 两次，才能做成双向边
 *	  连接 city1 与 city2；
 *	  单向边也可以（只需调用 gimme_edge
 *	  一次，方向是从 city1 到 city2）
 *
 *	  若边原先未登记、刚刚添加，则返回 1；
 *			  若边已经登记且 edge_table 未变，则返回 0
 */
static int
gimme_edge(PlannerInfo *root, Gene gene1, Gene gene2, Edge *edge_table)
{
	int			i;
	int			edges;
	int			city1 = (int) gene1;
	int			city2 = (int) gene2;


	/* 检查边 city1->city2 是否已存在 */
	edges = edge_table[city1].total_edges;

	for (i = 0; i < edges; i++)
	{
		if ((Gene) abs(edge_table[city1].edge_list[i]) == city2)
		{

			/* 把共享边标为负值 */
			edge_table[city1].edge_list[i] = 0 - city2;

			return 0;
		}
	}

	/* 添加 city1->city2； */
	edge_table[city1].edge_list[edges] = city2;

	/* 把从 city1 出发的边数加一 */
	edge_table[city1].total_edges++;
	edge_table[city1].unused_edges++;

	return 1;
}

/* gimme_tour
 *
 *	  用边表中的边生成新的巡回。
 *	  优先使用 "共享" 边（即
 *	  所有父代基因都拥有、并被标为负值
 *	  的那些边）。
 *
 */
int
gimme_tour(PlannerInfo *root, Edge *edge_table, Gene *new_gene, int num_gene)
{
	int			i;
	int			edge_failures = 0;

	/* 选取 1 到 num_gene 之间的整数 */
	new_gene[0] = (Gene) geqo_randint(root, num_gene, 1);

	for (i = 1; i < num_gene; i++)
	{
		/*
		 * 每把一个点放入巡回，就把它从边
		 * 表中去掉
		 */

		remove_gene(root, new_gene[i - 1], edge_table[(int) new_gene[i - 1]], edge_table);

		/* 为新进入的点寻找目的地 */

		if (edge_table[new_gene[i - 1]].unused_edges > 0)
			new_gene[i] = gimme_gene(root, edge_table[(int) new_gene[i - 1]], edge_table);

		else
		{						/* 处理故障 */
			edge_failures++;

			new_gene[i] = edge_failure(root, new_gene, i - 1, edge_table, num_gene);
		}

		/* 把该节点标为已并入 */
		edge_table[(int) new_gene[i - 1]].unused_edges = -1;
	}							/* 对应 for (i=1; i<num_gene; i++) */

	return edge_failures;
}

/* remove_gene
 *
 *	 从 edge_table 中去掉输入基因。
 *	 用输入边
 *	 确定边表中的删除位置。
 *
 */
static void
remove_gene(PlannerInfo *root, Gene gene, Edge edge, Edge *edge_table)
{
	int			i,
				j;
	int			possess_edge;
	int			genes_remaining;

	/*
	 * 对每个已知与输入基因有边相连的基因执行（即位于
	 * 输入边的 edge_list 中）
	 */

	for (i = 0; i < edge.unused_edges; i++)
	{
		possess_edge = abs(edge.edge_list[i]);
		genes_remaining = edge_table[possess_edge].unused_edges;

		/* 在所有 edge_lists 里找到输入基因并删掉它 */
		for (j = 0; j < genes_remaining; j++)
		{

			if ((Gene) abs(edge_table[possess_edge].edge_list[j]) == gene)
			{

				edge_table[possess_edge].unused_edges--;

				edge_table[possess_edge].edge_list[j] =
					edge_table[possess_edge].edge_list[genes_remaining - 1];

				break;
			}
		}
	}
}

/* gimme_gene
 *
 *	  优先考虑 "共享" 边
 *	  （即两个基因都拥有的边）
 *
 */
static Gene
gimme_gene(PlannerInfo *root, Edge edge, Edge *edge_table)
{
	int			i;
	Gene		friend;
	int			minimum_edges;
	int			minimum_count = -1;
	int			rand_decision;

	/*
	 * 没有任何点连向超过 4 个其他点，因此这个人为设定的
	 * 最小值会被替换掉
	 */

	minimum_edges = 5;

	/* 考虑边列表中的候选目的点 */

	for (i = 0; i < edge.unused_edges; i++)
	{
		friend = (Gene) edge.edge_list[i];

		/*
		 * 优先返回为负的共享边
		 */

		/*
		 * 负值在这里已经被截住，因此不必操心
		 * 再转换成绝对值
		 */
		if (friend < 0)
			return (Gene) abs(friend);


		/*
		 * 优先选择剩余未用边最少的候选；
		 * 先找出未用边的最小数目
		 * （minimum_edges）；若有不止一个候选具有该
		 * 最小未用边数，则记下这个数目
		 * （minimum_count）；
		 */

		/*
		 * 对 minimum_count 的测试也许以后可以去掉，
		 * 但注释应当确切说明为什么可以保证
		 * 该测试第一次就总会成功。如果它会
		 * 失败，那么代码就是错的
		 */


		if (edge_table[(int) friend].unused_edges < minimum_edges)
		{
			minimum_edges = edge_table[(int) friend].unused_edges;
			minimum_count = 1;
		}
		else if (minimum_count == -1)
			elog(ERROR, "minimum_count not set");
		else if (edge_table[(int) friend].unused_edges == minimum_edges)
			minimum_count++;
	}							/* 对应 for (i=0; i<edge.unused_edges; i++) */


	/* 在可用候选中随机决定 */
	rand_decision = geqo_randint(root, minimum_count - 1, 0);


	for (i = 0; i < edge.unused_edges; i++)
	{
		friend = (Gene) edge.edge_list[i];

		/* 返回选中的候选点 */
		if (edge_table[(int) friend].unused_edges == minimum_edges)
		{
			minimum_count--;

			if (minimum_count == rand_decision)
				return friend;
		}
	}

	/* ……不应到达此处 */
	elog(ERROR, "neither shared nor minimum number nor random edge found");
	return 0;					/* 消除编译器告警 */
}

/* edge_failure
 *
 *	  处理边失败的例程
 *
 */
static Gene
edge_failure(PlannerInfo *root, Gene *gene, int index, Edge *edge_table, int num_gene)
{
	int			i;
	Gene		fail_gene = gene[index];
	int			remaining_edges = 0;
	int			four_count = 0;
	int			rand_decision;


	/*
	 * 还剩多少条边？总（初始）边数为四的基因
	 * 还剩多少个？
	 */

	for (i = 1; i <= num_gene; i++)
	{
		if ((edge_table[i].unused_edges != -1) && (i != (int) fail_gene))
		{
			remaining_edges++;

			if (edge_table[i].total_edges == 4)
				four_count++;
		}
	}

	/*
	 * 在仍有剩余边、且其 total_edges
	 * == 4
	 */

	if (four_count != 0)
	{

		rand_decision = geqo_randint(root, four_count - 1, 0);

		for (i = 1; i <= num_gene; i++)
		{

			if ((Gene) i != fail_gene &&
				edge_table[i].unused_edges != -1 &&
				edge_table[i].total_edges == 4)
			{

				four_count--;

				if (rand_decision == four_count)
					return (Gene) i;
			}
		}

		elog(LOG, "no edge found via random decision and total_edges == 4");
	}
	else if (remaining_edges != 0)
	{
		/* 在仍有剩余边的基因中随机选取 */
		rand_decision = geqo_randint(root, remaining_edges - 1, 0);

		for (i = 1; i <= num_gene; i++)
		{

			if ((Gene) i != fail_gene &&
				edge_table[i].unused_edges != -1)
			{

				remaining_edges--;

				if (rand_decision == remaining_edges)
					return i;
			}
		}

		elog(LOG, "no edge found via random decision with remaining edges");
	}

	/*
	 * 边表看起来是空的；这有时发生在最后一点，
	 * 因为第一点即使只确定了一条边，
	 * 也会被从表中去掉
	 */

	else
	{							/* 只发生在巡回的最后一点；
								 * 只需寻找尚未
								 * 被使用的点 */

		for (i = 1; i <= num_gene; i++)
			if (edge_table[i].unused_edges >= 0)
				return (Gene) i;

		elog(LOG, "no edge found via looking for the last unused point");
	}


	/* ……不应到达此处 */
	elog(ERROR, "no edge found");
	return 0;					/* 消除编译器告警 */
}

#endif							/* 当 defined(ERX) */
