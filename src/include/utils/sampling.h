/*-------------------------------------------------------------------------
 *
 * sampling.h
 *	  采样函数的定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sampling.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "common/pg_prng.h"
#include "storage/block.h"		/* 供 typedef BlockNumber */


/* 采样代码的随机数生成器 */
extern void sampler_random_init_state(uint32 seed,
									  pg_prng_state *randstate);
extern double sampler_random_fract(pg_prng_state *randstate);

/* 块采样方法 */

/* Knuth 3.4.2 算法 S 的数据结构 */
typedef struct
{
	BlockNumber N;				/* 块数，预先已知 */
	int			n;				/* 期望样本量 */
	BlockNumber t;				/* 当前块号 */
	int			m;				/* 迄今已选块数 */
	pg_prng_state randstate;	/* 随机生成器状态 */
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler;

extern BlockNumber BlockSampler_Init(BlockSampler bs, BlockNumber nblocks,
									 int samplesize, uint32 randseed);
extern bool BlockSampler_HasMore(BlockSampler bs);
extern BlockNumber BlockSampler_Next(BlockSampler bs);

/* 蓄水池采样方法 */

typedef struct
{
	double		W;
	pg_prng_state randstate;	/* 随机生成器状态 */
} ReservoirStateData;

typedef ReservoirStateData *ReservoirState;

extern void reservoir_init_selection_state(ReservoirState rs, int n);
extern double reservoir_get_next_S(ReservoirState rs, double t, int n);

/* 旧 API，仍被若干 FDWs 使用 */
/* 为向后兼容，这些声明在 vacuum.h 中重复 */

extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif							/* SAMPLING_H */
