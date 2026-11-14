/*-------------------------------------------------------------------------
 *
 * sampling.c
 *	  Relation block sampling routines.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/sampling.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "utils/sampling.h"


/*
 * BlockSampler_Init —— 为块号随机采样做准备
 *
 * BlockSampler 提供对关系进行块级采样的算法
 * 见 pgsql-hackers 2004-04-02 讨论（主题 "Large DB"）
 * 它选出由 samplesize 个块组成的随机样本，
 * 出自表中 nblocks 个块。若表的块数少于
 * samplesize 个块则全选。
 *
 * 由于我们预先知道块的总数，可以使用
 * Knuth 3.4.2 中简单直接的 Algorithm S，而非 Vitter 的
 * 算法。
 *
 * 返回 BlockSampler_Next 将返回的块数。
 */
BlockNumber
BlockSampler_Init(BlockSampler bs, BlockNumber nblocks, int samplesize,
				  uint32 randseed)
{
	bs->N = nblocks;			/* 测得的表大小 */

	/*
	 * 若决定对块数少于或仅略多于 samplesize 的表减小
	 * samplesize，就在此处处理。
	 */
	bs->n = samplesize;
	bs->t = 0;					/* 已扫描的块数 */
	bs->m = 0;					/* 已选中的块数 */

	sampler_random_init_state(randseed, &bs->randstate);

	return Min(bs->n, bs->N);
}

bool
BlockSampler_HasMore(BlockSampler bs)
{
	return (bs->t < bs->N) && (bs->m < bs->n);
}

BlockNumber
BlockSampler_Next(BlockSampler bs)
{
	BlockNumber K = bs->N - bs->t;	/* 剩余块数 */
	int			k = bs->n - bs->m;	/* 尚待采样的块数 */
	double		p;				/* 跳过块的概率 */
	double		V;				/* 随机 */

	Assert(BlockSampler_HasMore(bs));	/* 故 K > 0 且 k > 0 */

	if ((BlockNumber) k >= K)
	{
		/* 取剩余全部 */
		bs->m++;
		return bs->t++;
	}

	/*----------
	 * 这段代码与 Knuth 的 Algorithm S 相符并不显然。
	 * Knuth 说以 1 - k/K 的概率跳过当前块。
	 * 若要跳过，就应推进 t（从而减小 K），并
	 * 再对下一块重复同样的概率测试。朴素的
	 * 实现因此需要对每个
	 * 块号调用一次 sampler_random_fract()，但可将其降为
	 * 每个选中块仅一次调用，注意到每次 while 测试
	 * 成功时，我们都可把 V 重新解释为区间
	 * 0 到 p 内的均匀随机数。因此不必另选 V，只需把 p
	 * 调整为原值的适当比例，下一轮循环就会
	 * 作出相应的概率测试。
	 *
	 * 初始时 K > k > 0。若循环把 K 减到与 k 相等，
	 * 下轮 while 测试必失败，因为 p 将恰好变为零
	 * （我们假设除法不会产生舍入误差）。
	 * （注：Knuth 建议 "<=" 循环条件，但我们用 "<" 仅
	 * 是为了对舍入误差加倍保险。）因此 K 不会变
	 * 得小于 k，这意味着我们必定能选够所需的块。
	 *----------
	 */
	V = sampler_random_fract(&bs->randstate);
	p = 1.0 - (double) k / (double) K;
	while (V < p)
	{
		/* skip */
		bs->t++;
		K--;					/* 令 K == N - t */

		/* 把 p 调整为缩小后区间中的新截断点 */
		p *= 1.0 - (double) k / (double) K;
	}

	/* 选取 */
	bs->m++;
	return bs->t++;
}

/*
 * 这两个例程体现了 "Random sampling with a
 * reservoir"（Jeffrey S. Vitter 著）一文中的 Algorithm Z，
 * （ACM Trans. Math. Softw. 11, 1，1985 年 3 月），
 * 第 37-57 页。Vitter 将该算法描述为：处理下一条
 * 记录前要跳过的记录数 S。它主要依据已读记录数 t。
 * 调用间唯一需要的额外状态是随机状态变量 W。
 *
 * reservoir_init_selection_state 计算初始 W 值。
 *
 * 设已读取 t 条记录（t >= n），reservoir_get_next_S
 * 会确定在处理下一条记录之前要跳过的记录数。
 *
 */
void
reservoir_init_selection_state(ReservoirState rs, int n)
{
	/*
	 * 凡是需要可重复结果之处均未使用蓄水池采样，
	 * 因此我们可以随机初始化它。
	 */
	sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
							  &rs->randstate);

	/* W 的初始值（首次应用 Algorithm Z 时使用） */
	rs->W = exp(-log(sampler_random_fract(&rs->randstate)) / n);
}

double
reservoir_get_next_S(ReservoirState rs, double t, int n)
{
	double		S;

	/* 这里的魔数就是 Vitter 论文中的 T */
	if (t <= (22.0 * n))
	{
		/* 用 Algorithm X 处理记录，直到 t 足够大 */
		double		V,
					quot;

		V = sampler_random_fract(&rs->randstate);	/* 生成 V */
		S = 0;
		t += 1;
		/* 注：Vitter 代码中的 "num" 总等于 t - n */
		quot = (t - (double) n) / t;
		/* 求满足 (4.1) 的最小 S */
		while (quot > V)
		{
			S += 1;
			t += 1;
			quot *= (t - (double) n) / t;
		}
	}
	else
	{
		/* 现改用 Algorithm Z */
		double		W = rs->W;
		double		term = t - (double) n + 1;

		for (;;)
		{
			double		numer,
						numer_lim,
						denom;
			double		U,
						X,
						lhs,
						rhs,
						y,
						tmp;

			/* 生成 U 和 X */
			U = sampler_random_fract(&rs->randstate);
			X = t * (W - 1.0);
			S = floor(X);		/* S 暂定为 floor(X) */
			/* 按 (6.3) 的方式测试 U <= h(S)/cg(X) */
			tmp = (t + 1) / term;
			lhs = exp(log(((U * tmp * tmp) * (term + S)) / (t + X)) / n);
			rhs = (((t + X) / (term + S)) * term) / t;
			if (lhs <= rhs)
			{
				W = rhs / lhs;
				break;
			}
			/* 测试 U <= f(S)/cg(X) */
			y = (((U * (t + 1)) / term) * (t + S + 1)) / (t + X);
			if ((double) n < S)
			{
				denom = t;
				numer_lim = term + S;
			}
			else
			{
				denom = t - (double) n + S;
				numer_lim = t + 1;
			}
			for (numer = t + S; numer >= numer_lim; numer -= 1)
			{
				y *= numer / denom;
				denom -= 1;
			}
			W = exp(-log(sampler_random_fract(&rs->randstate)) / n);	/* 预先生成 W */
			if (exp(log(y) / n) <= (t + X) / t)
				break;
		}
		rs->W = W;
	}
	return S;
}


/*----------
 * 采样使用的随机数生成器
 *----------
 */
void
sampler_random_init_state(uint32 seed, pg_prng_state *randstate)
{
	pg_prng_seed(randstate, (uint64) seed);
}

/* 选取均匀分布于 (0 - 1) 的随机值 R */
double
sampler_random_fract(pg_prng_state *randstate)
{
	double		res;

	/* pg_prng_double 返回 [0.0 - 1.0) 内的值，因此必须拒绝 0.0 */
	do
	{
		res = pg_prng_double(randstate);
	} while (unlikely(res == 0.0));
	return res;
}


/*
 * 面向块采样的向后兼容 API
 *
 * 这段代码现已弃用，但由于仍有许多 FDWs 在使用，
 * 我们至少应在一段时间内保留它。其功能与
 * sampler_random_fract/reservoir_init_selection_state/reservoir_get_next_S,
 * 只是所有调用方共用同一个随机状态。
 */
static ReservoirStateData oldrs;
static bool oldrs_initialized = false;

double
anl_random_fract(void)
{
	/* 首次经过时初始化 */
	if (unlikely(!oldrs_initialized))
	{
		sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
								  &oldrs.randstate);
		oldrs_initialized = true;
	}

	/* 并计算一个随机分数 */
	return sampler_random_fract(&oldrs.randstate);
}

double
anl_init_selection_state(int n)
{
	/* 首次经过时初始化 */
	if (unlikely(!oldrs_initialized))
	{
		sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
								  &oldrs.randstate);
		oldrs_initialized = true;
	}

	/* W 的初始值（首次应用 Algorithm Z 时使用） */
	return exp(-log(sampler_random_fract(&oldrs.randstate)) / n);
}

double
anl_get_next_S(double t, int n, double *stateptr)
{
	double		result;

	oldrs.W = *stateptr;
	result = reservoir_get_next_S(&oldrs, t, n);
	*stateptr = oldrs.W;
	return result;
}
