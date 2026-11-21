/*-------------------------------------------------------------------------
 *
 * geqo_recombination.h
 *	遗传查询优化器中重组的原型
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_recombination.h
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* -- 部分改编自 D. Whitley 的 Genitor 算法 -- */

#ifndef GEQO_RECOMBINATION_H
#define GEQO_RECOMBINATION_H

#include "optimizer/geqo.h"


extern void init_tour(PlannerInfo *root, Gene *tour, int num_gene);


/* 边重组交叉 [ERX] */

typedef struct Edge
{
	Gene		edge_list[4];	/* 边列表 */
	int			total_edges;
	int			unused_edges;
} Edge;

extern Edge *alloc_edge_table(PlannerInfo *root, int num_gene);
extern void free_edge_table(PlannerInfo *root, Edge *edge_table);

extern float gimme_edge_table(PlannerInfo *root, Gene *tour1, Gene *tour2,
							  int num_gene, Edge *edge_table);

extern int	gimme_tour(PlannerInfo *root, Edge *edge_table, Gene *new_gene,
					   int num_gene);


/* 部分匹配交叉 [PMX] */

#define DAD 1					/* 来自父方基因的指示 */
#define MOM 0					/* 来自母方基因的指示 */

extern void pmx(PlannerInfo *root,
				Gene *tour1, Gene *tour2,
				Gene *offspring, int num_gene);


typedef struct City
{
	int			tour2_position;
	int			tour1_position;
	int			used;
	int			select_list;
}			City;

extern City * alloc_city_table(PlannerInfo *root, int num_gene);
extern void free_city_table(PlannerInfo *root, City * city_table);

/* 循环交叉 [CX] */
extern int	cx(PlannerInfo *root, Gene *tour1, Gene *tour2,
			   Gene *offspring, int num_gene, City * city_table);

/* 位置交叉 [PX] */
extern void px(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring,
			   int num_gene, City * city_table);

/* 按 Davis 的顺序交叉 [OX1] */
extern void ox1(PlannerInfo *root, Gene *mom, Gene *dad, Gene *offspring,
				int num_gene, City * city_table);

/* 按 Syswerda 的顺序交叉 [OX2] */
extern void ox2(PlannerInfo *root, Gene *mom, Gene *dad, Gene *offspring,
				int num_gene, City * city_table);

#endif							/* GEQO_RECOMBINATION_H */
