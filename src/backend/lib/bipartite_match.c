/*-------------------------------------------------------------------------
 *
 * bipartite_match.c
 *	  二分图的 Hopcroft-Karp 最大基数算法
 *
 * 本实现依据此处的伪代码：
 *
 * https://en.wikipedia.org/w/index.php?title=Hopcroft%E2%80%93Karp_algorithm&oldid=593898016
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/bipartite_match.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "lib/bipartite_match.h"
#include "miscadmin.h"

/*
 * 很容易看出 hk_breadth_search 算出的距离从不超过
 * u_size。由于我们限制 u_size 小于 SHRT_MAX，因此
 * 可以用 SHRT_MAX 作为所需的 "无穷" 距离标记。
 */
#define HK_INFINITY  SHRT_MAX

static bool hk_breadth_search(BipartiteMatchState *state);
static bool hk_depth_search(BipartiteMatchState *state, int u);

/*
 * 给定 U 和 V 的大小（各自下标为 1..size）以及邻接
 * 表，执行匹配并返回结果状态。
 */
BipartiteMatchState *
BipartiteMatch(int u_size, int v_size, short **adjacency)
{
	BipartiteMatchState *state = palloc(sizeof(BipartiteMatchState));

	if (u_size < 0 || u_size >= SHRT_MAX ||
		v_size < 0 || v_size >= SHRT_MAX)
		elog(ERROR, "invalid set size for BipartiteMatch");

	state->u_size = u_size;
	state->v_size = v_size;
	state->adjacency = adjacency;
	state->matching = 0;
	state->pair_uv = (short *) palloc0((u_size + 1) * sizeof(short));
	state->pair_vu = (short *) palloc0((v_size + 1) * sizeof(short));
	state->distance = (short *) palloc((u_size + 1) * sizeof(short));
	state->queue = (short *) palloc((u_size + 2) * sizeof(short));

	while (hk_breadth_search(state))
	{
		int			u;

		for (u = 1; u <= u_size; u++)
		{
			if (state->pair_uv[u] == 0)
				if (hk_depth_search(state, u))
					state->matching++;
		}

		CHECK_FOR_INTERRUPTS(); /* 以防万一 */
	}

	return state;
}

/*
 * 释放 BipartiteMatch 返回的状态，但不释放原来的邻接
 * 表，它由调用者拥有。这只释放内存，因此是可选的。
 */
void
BipartiteMatchFree(BipartiteMatchState *state)
{
	/* 邻接矩阵视为由调用者拥有 */
	pfree(state->pair_uv);
	pfree(state->pair_vu);
	pfree(state->distance);
	pfree(state->queue);
	pfree(state);
}

/*
 * 执行 H-K 匹配的广度优先搜索步骤。
 * 成功则返回 true。
 */
static bool
hk_breadth_search(BipartiteMatchState *state)
{
	int			usize = state->u_size;
	short	   *queue = state->queue;
	short	   *distance = state->distance;
	int			qhead = 0;		/* 我们从不把同一节点入队超过一次 */
	int			qtail = 0;		/* 因此不必担心回绕 */
	int			u;

	distance[0] = HK_INFINITY;

	for (u = 1; u <= usize; u++)
	{
		if (state->pair_uv[u] == 0)
		{
			distance[u] = 0;
			queue[qhead++] = u;
		}
		else
			distance[u] = HK_INFINITY;
	}

	while (qtail < qhead)
	{
		u = queue[qtail++];

		if (distance[u] < distance[0])
		{
			short	   *u_adj = state->adjacency[u];
			int			i = u_adj ? u_adj[0] : 0;

			for (; i > 0; i--)
			{
				int			u_next = state->pair_vu[u_adj[i]];

				if (distance[u_next] == HK_INFINITY)
				{
					distance[u_next] = 1 + distance[u];
					Assert(qhead < usize + 2);
					queue[qhead++] = u_next;
				}
			}
		}
	}

	return (distance[0] != HK_INFINITY);
}

/*
 * 执行 H-K 匹配的深度优先搜索步骤。
 * 成功则返回 true。
 */
static bool
hk_depth_search(BipartiteMatchState *state, int u)
{
	short	   *distance = state->distance;
	short	   *pair_uv = state->pair_uv;
	short	   *pair_vu = state->pair_vu;
	short	   *u_adj = state->adjacency[u];
	int			i = u_adj ? u_adj[0] : 0;
	short		nextdist;

	if (u == 0)
		return true;
	if (distance[u] == HK_INFINITY)
		return false;
	nextdist = distance[u] + 1;

	check_stack_depth();

	for (; i > 0; i--)
	{
		int			v = u_adj[i];

		if (distance[pair_vu[v]] == nextdist)
		{
			if (hk_depth_search(state, pair_vu[v]))
			{
				pair_vu[v] = u;
				pair_uv[u] = v;
				return true;
			}
		}
	}

	distance[u] = HK_INFINITY;
	return false;
}
