/*
 * binaryheap.h
 *
 * A simple binary heap implementation
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * src/include/lib/binaryheap.h
 */

#ifndef BINARYHEAP_H
#define BINARYHEAP_H

/*
 * 对最大堆，比较函数必须在 a < b 时返回 <0，a == b 时返回 0，
 * a > b 时返回 >0。对最小堆，这些条件反过来。
 */
typedef int (*binaryheap_comparator) (Datum a, Datum b, void *arg);

/*
 * binaryheap
 *
 *		bh_size			"nodes" 中当前的节点数
 *		bh_space		"nodes" 中可存放的节点数
 *		bh_has_heap_property	自上次建堆以来没有无序操作
 *		bh_compare		定义堆性质的比较函数
 *		bh_arg			比较函数的用户数据
 *		bh_nodes		长度为 "space" 的变长节点数组
 */
typedef struct binaryheap
{
	int			bh_size;
	int			bh_space;
	bool		bh_has_heap_property;	/* 调试用交叉检查 */
	binaryheap_comparator bh_compare;
	void	   *bh_arg;
	Datum		bh_nodes[FLEXIBLE_ARRAY_MEMBER];
} binaryheap;

extern binaryheap *binaryheap_allocate(int capacity,
									   binaryheap_comparator compare,
									   void *arg);
extern void binaryheap_reset(binaryheap *heap);
extern void binaryheap_free(binaryheap *heap);
extern void binaryheap_add_unordered(binaryheap *heap, Datum d);
extern void binaryheap_build(binaryheap *heap);
extern void binaryheap_add(binaryheap *heap, Datum d);
extern Datum binaryheap_first(binaryheap *heap);
extern Datum binaryheap_remove_first(binaryheap *heap);
extern void binaryheap_replace_first(binaryheap *heap, Datum d);

#define binaryheap_empty(h)			((h)->bh_size == 0)

#endif							/* BINARYHEAP_H */
