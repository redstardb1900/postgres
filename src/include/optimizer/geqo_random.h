/*-------------------------------------------------------------------------
 *
 * geqo_random.h
 *	随机数生成器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_random.h
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

#ifndef GEQO_RANDOM_H
#define GEQO_RANDOM_H

#include <math.h>

#include "optimizer/geqo.h"


extern void geqo_set_seed(PlannerInfo *root, double seed);

/* geqo_rand 返回 [0.0, 1.0) 内的随机浮点 */
extern double geqo_rand(PlannerInfo *root);

/* geqo_randint 返回含上下界的整数 */
extern int	geqo_randint(PlannerInfo *root, int upper, int lower);

#endif							/* GEQO_RANDOM_H */
