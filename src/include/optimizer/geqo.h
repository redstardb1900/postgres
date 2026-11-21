/*-------------------------------------------------------------------------
 *
 * geqo.h
 *	optimizer/geqo 下各文件的原型
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo.h
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

#ifndef GEQO_H
#define GEQO_H

#include "common/pg_prng.h"
#include "nodes/pathnodes.h"
#include "optimizer/geqo_gene.h"


/* GEQO 调试 */
/*
 #define GEQO_DEBUG
 */

/* 在此选择一种重组机制 */
/*
 #define ERX
 #define PMX
 #define CX
 #define PX
 #define OX1
 #define OX2
 */
#define ERX


/*
 * 配置选项
 *
 * 若更改这些，请更新 backend/utils/misc/postgresql.conf.sample
 */
extern PGDLLIMPORT int Geqo_effort; /* 1 .. 10，用于调节
									 * 默认 */

#define DEFAULT_GEQO_EFFORT 5
#define MIN_GEQO_EFFORT 1
#define MAX_GEQO_EFFORT 10

extern PGDLLIMPORT int Geqo_pool_size;	/* 2 .. inf，或 0 用默认 */

extern PGDLLIMPORT int Geqo_generations;	/* 1 .. inf，或 0 用默认 */

extern PGDLLIMPORT double Geqo_selection_bias;

#define DEFAULT_GEQO_SELECTION_BIAS 2.0
#define MIN_GEQO_SELECTION_BIAS 1.5
#define MAX_GEQO_SELECTION_BIAS 2.0

extern PGDLLIMPORT double Geqo_seed;	/* 0 .. 1 */


/*
 * GEQO 运行私有状态——经 root->join_search_private 访问
 */
typedef struct
{
	List	   *initial_rels;	/* 正在连接的基关系 */
	pg_prng_state random_state; /* PRNG 态 */
} GeqoPrivateData;


/* geqo_main.c 例程 */
extern RelOptInfo *geqo(PlannerInfo *root,
						int number_of_rels, List *initial_rels);

/* geqo_eval.c 例程 */
extern Cost geqo_eval(PlannerInfo *root, Gene *tour, int num_gene);
extern RelOptInfo *gimme_tree(PlannerInfo *root, Gene *tour, int num_gene);

#endif							/* GEQO_H */
