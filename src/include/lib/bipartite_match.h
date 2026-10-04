/*
 * bipartite_match.h
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * src/include/lib/bipartite_match.h
 */
#ifndef BIPARTITE_MATCH_H
#define BIPARTITE_MATCH_H

/*
 * 给定二分图：节点 U 编号为 1..nU，节点 V 编号为
 * 1..nV，无向边的邻接映射形如
 * adjacency[u] = [k, v1, v2, v3, ... vk]。我们希望求出
 * "最大基数匹配"，定义如下：匹配是原边集的一个子集，
 * 使得没有任何节点关联多于一条边；若不再存在边数
 * 更多的其他匹配，则该匹配具有最大基数。
 *
 * 这种匹配在图论中有多种应用，这里的动机例子是
 * Dilworth 定理：偏序集可以通过二分图构造，划分成
 * 最少数量的链（即满足 x1 < x2 < x3 ... 的子集 X）。
 * 这给出了规划一组 grouping sets、且排序次数可证明
 * 最少的多项式时间解法。
 *
 *
 */
typedef struct BipartiteMatchState
{
	/* 输入： */
	int			u_size;			/* U 的大小 */
	int			v_size;			/* V 的大小 */
	short	  **adjacency;		/* 邻接：adjacency[u] = [k, v1,v2,v3,...,vk] */
	/* 输出： */
	int			matching;		/* 匹配中的边数 */
	short	   *pair_uv;		/* pair_uv[u] 指向 v */
	short	   *pair_vu;		/* pair_vu[v] 指向 u */
	/* 匹配算法的私有状态： */
	short	   *distance;		/* U 侧距离 distance[u] */
	short	   *queue;			/* 广度搜索用的队列存储 */
} BipartiteMatchState;

extern BipartiteMatchState *BipartiteMatch(int u_size, int v_size, short **adjacency);

extern void BipartiteMatchFree(BipartiteMatchState *state);

#endif							/* BIPARTITE_MATCH_H */
