/*-------------------------------------------------------------------------
 *
 * binaryheap.c
 *	  简单的二叉堆实现
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/binaryheap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "lib/binaryheap.h"

static void sift_down(binaryheap *heap, int node_off);
static void sift_up(binaryheap *heap, int node_off);

/*
 * binaryheap_allocate
 *
 * 返回指向新分配堆的指针，该堆有容量存放给定数量
 * 的节点，堆性质由给定比较函数定义，调用比较函数
 * 时会传入 'arg' 指定的额外
 * 参数。
 */
binaryheap *
binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg)
{
	int			sz;
	binaryheap *heap;

	sz = offsetof(binaryheap, bh_nodes) + sizeof(Datum) * capacity;
	heap = (binaryheap *) palloc(sz);
	heap->bh_space = capacity;
	heap->bh_compare = compare;
	heap->bh_arg = arg;

	heap->bh_size = 0;
	heap->bh_has_heap_property = true;

	return heap;
}

/*
 * binaryheap_reset
 *
 * 把堆重置为空状态，丢失其数据内容但不丢失
 * 分配时传入的参数。
 */
void
binaryheap_reset(binaryheap *heap)
{
	heap->bh_size = 0;
	heap->bh_has_heap_property = true;
}

/*
 * binaryheap_free
 *
 * 释放给定 binaryheap 使用的内存。
 */
void
binaryheap_free(binaryheap *heap)
{
	pfree(heap);
}

/*
 * 这些实用函数分别返回给定下标处节点的左孩子、
 * 右孩子和父节点的偏移。
 *
 * 堆表示为节点数组，根节点存在下标 0。节点 i 的
 * 左孩子在下标 2*i+1，右孩子在 2*i+2。节点 i 的
 * 父节点在下标 (i-1)/2。
 */

static inline int
left_offset(int i)
{
	return 2 * i + 1;
}

static inline int
right_offset(int i)
{
	return 2 * i + 2;
}

static inline int
parent_offset(int i)
{
	return (i - 1) / 2;
}

/*
 * binaryheap_add_unordered
 *
 * 在 O(1) 时间内把给定 datum 加到堆节点列表末尾，
 * 不保持堆性质。这是向新堆快速加元素的便利方法。
 * 要得到有效堆，之后必须调用
 * binaryheap_build()。
 */
void
binaryheap_add_unordered(binaryheap *heap, Datum d)
{
	if (heap->bh_size >= heap->bh_space)
		elog(ERROR, "out of binary heap slots");
	heap->bh_has_heap_property = false;
	heap->bh_nodes[heap->bh_size] = d;
	heap->bh_size++;
}

/*
 * binaryheap_build
 *
 * 从 binaryheap_add_unordered() 加入的节点在 O(n)
 * 时间内组装出有效堆。其他情况不需要。
 */
void
binaryheap_build(binaryheap *heap)
{
	int			i;

	for (i = parent_offset(heap->bh_size - 1); i >= 0; i--)
		sift_down(heap, i);
	heap->bh_has_heap_property = true;
}

/*
 * binaryheap_add
 *
 * 在 O(log n) 时间内把给定 datum 加入堆，同时保持
 * 堆性质。
 */
void
binaryheap_add(binaryheap *heap, Datum d)
{
	if (heap->bh_size >= heap->bh_space)
		elog(ERROR, "out of binary heap slots");
	heap->bh_nodes[heap->bh_size] = d;
	heap->bh_size++;
	sift_up(heap, heap->bh_size - 1);
}

/*
 * binaryheap_first
 *
 * 返回指向堆中第一个（根、最顶）节点的指针，
 * 不修改堆。调用者必须确保不对空堆使用本
 * 例程。总是 O(1)。
 */
Datum
binaryheap_first(binaryheap *heap)
{
	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);
	return heap->bh_nodes[0];
}

/*
 * binaryheap_remove_first
 *
 * 移除堆中第一个（根、最顶）节点，在重新平衡堆
 * 之后返回指向它的指针。调用者必须确保不对空堆
 * 使用本例程。最坏情况
 * O(log n)。
 */
Datum
binaryheap_remove_first(binaryheap *heap)
{
	Datum		result;

	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);

	/* 取出根节点，它将是返回结果 */
	result = heap->bh_nodes[0];

	/* 堆只含一个元素时很简单 */
	if (heap->bh_size == 1)
	{
		heap->bh_size--;
		return result;
	}

	/*
	 * 移除最后一个节点，把它放到腾出的根项中，并把
	 * 新根节点下滤到正确位置。
	 */
	heap->bh_nodes[0] = heap->bh_nodes[--heap->bh_size];
	sift_down(heap, 0);

	return result;
}

/*
 * binaryheap_replace_first
 *
 * 替换非空堆的最顶元素，保持堆性质。最好情况
 * O(1)，若必须回退为把新节点下滤则为
 * O(log n)。
 */
void
binaryheap_replace_first(binaryheap *heap, Datum d)
{
	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);

	heap->bh_nodes[0] = d;

	if (heap->bh_size > 1)
		sift_down(heap, 0);
}

/*
 * 按比较器把节点上滤到它能占据的
 * 最高位置。
 */
static void
sift_up(binaryheap *heap, int node_off)
{
	Datum		node_val = heap->bh_nodes[node_off];

	/*
	 * 循环内，第 node_off 个数组项是一个 "空洞"，
	 * 概念上持有 node_val，但我们直到最后才真正
	 * 把 node_val 存到那里，从而省去一些不必要的数据复制。
	 */
	while (node_off != 0)
	{
		int			cmp;
		int			parent_off;
		Datum		parent_val;

		/*
		 * 若本节点小于其父节点，则堆条件已满足，
		 * 我们完成了。
		 */
		parent_off = parent_offset(node_off);
		parent_val = heap->bh_nodes[parent_off];
		cmp = heap->bh_compare(node_val,
							   parent_val,
							   heap->bh_arg);
		if (cmp <= 0)
			break;

		/*
		 * 否则，把父值与空洞交换，并继续检查该
		 * 节点的新父节点。
		 */
		heap->bh_nodes[node_off] = parent_val;
		node_off = parent_off;
	}
	/* 填回空洞 */
	heap->bh_nodes[node_off] = node_val;
}

/*
 * 从当前位置下滤节点以满足堆
 * 性质。
 */
static void
sift_down(binaryheap *heap, int node_off)
{
	Datum		node_val = heap->bh_nodes[node_off];

	/*
	 * 循环内，第 node_off 个数组项是一个 "空洞"，
	 * 概念上持有 node_val，但我们直到最后才真正
	 * 把 node_val 存到那里，从而省去一些不必要的数据复制。
	 */
	while (true)
	{
		int			left_off = left_offset(node_off);
		int			right_off = right_offset(node_off);
		int			swap_off = 0;

		/* 左孩子是否大于父节点？ */
		if (left_off < heap->bh_size &&
			heap->bh_compare(node_val,
							 heap->bh_nodes[left_off],
							 heap->bh_arg) < 0)
			swap_off = left_off;

		/* 右孩子是否大于父节点？ */
		if (right_off < heap->bh_size &&
			heap->bh_compare(node_val,
							 heap->bh_nodes[right_off],
							 heap->bh_arg) < 0)
		{
			/* 与较大的孩子交换 */
			if (!swap_off ||
				heap->bh_compare(heap->bh_nodes[left_off],
								 heap->bh_nodes[right_off],
								 heap->bh_arg) < 0)
				swap_off = right_off;
		}

		/*
		 * 若没找到可交换的，则堆条件已满足，
		 * 我们完成了。
		 */
		if (!swap_off)
			break;

		/*
		 * 否则，把空洞与违反堆性质的孩子交换；
		 * 然后继续检查它的孩子们。
		 */
		heap->bh_nodes[node_off] = heap->bh_nodes[swap_off];
		node_off = swap_off;
	}
	/* 填回空洞 */
	heap->bh_nodes[node_off] = node_val;
}
