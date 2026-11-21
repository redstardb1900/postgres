/*-------------------------------------------------------------------------
 *
 * 伪随机数生成器
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 * src/include/common/pg_prng.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PRNG_H
#define PG_PRNG_H

/*
 * PRNG 生成的状态向量。调用者应视其为不透明
 * typedef，但我们公开定义以便嵌在其他结构中。
 * 略
 */
typedef struct pg_prng_state
{
	uint64		s0,
				s1;
} pg_prng_state;

/*
 * 不需本地 PRNG 序列的调用者可用此全局状态，
 * 用 pg_prng_...seed 之一初始化后即可。
 */
extern PGDLLIMPORT pg_prng_state pg_global_prng_state;

extern void pg_prng_seed(pg_prng_state *state, uint64 seed);
extern void pg_prng_fseed(pg_prng_state *state, double fseed);
extern bool pg_prng_seed_check(pg_prng_state *state);

/*
 * 从 pg_strong_random 源初始化 PRNG 状态，
 * 并确保不全为零。若返回 false，调用者应以其他
 * 随机种子初始化，用 pg_prng_[f]seed。
 * 略
 *
 * 实现为宏，使 pg_strong_random() 调用在调用方。
 * 若在 pg_prng.c，用 pg_prng 但不需强种子的程序
 * 仍会被拉入 pg_strong_random.c 以至 OpenSSL。
 * 略
 */
#define pg_prng_strong_seed(state) \
	(pg_strong_random((void *) (state), sizeof(pg_prng_state)) ? \
	 pg_prng_seed_check(state) : false)

extern uint64 pg_prng_uint64(pg_prng_state *state);
extern uint64 pg_prng_uint64_range(pg_prng_state *state, uint64 rmin, uint64 rmax);
extern int64 pg_prng_int64(pg_prng_state *state);
extern int64 pg_prng_int64p(pg_prng_state *state);
extern uint32 pg_prng_uint32(pg_prng_state *state);
extern int32 pg_prng_int32(pg_prng_state *state);
extern int32 pg_prng_int32p(pg_prng_state *state);
extern double pg_prng_double(pg_prng_state *state);
extern double pg_prng_double_normal(pg_prng_state *state);
extern bool pg_prng_bool(pg_prng_state *state);

#endif							/* PG_PRNG_H */
