/*-------------------------------------------------------------------------
 *
 * pairingheap.c
 *	  配对堆实现
 *
 * 配对堆是一种适合用来实现
 * 优先队列的数据结构。它实现简单，并提供摊还 O(1) 的
 * 插入与查找最小元，以及摊还 O(log n) 的删除最小元。
 *
 * 配对堆最早描述于这篇论文：
 *
 *	Michael L. Fredman, Robert Sedgewick, Daniel D. Sleator, and Robert E.
 *	 Tarjan. 1986.
 *	The pairing heap: a new form of self-adjusting heap.
 *	Algorithmica 1, 1 (January 1986), pages 111-129. DOI: 10.1007/BF01840439
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/pairingheap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/pairingheap.h"

static pairingheap_node *merge(pairingheap *heap, pairingheap_node *a,
							   pairingheap_node *b);
static pairingheap_node *merge_children(pairingheap *heap,
										pairingheap_node *children);

/*
 * pairingheap_allocate
 *
 * 返回指向新分配堆的指针，堆性质由给定比较函数
 * 定义，调用比较函数时会传入 'arg' 指定的额外
 * 参数。
 */
pairingheap *
pairingheap_allocate(pairingheap_comparator compare, void *arg)
{
	pairingheap *heap;

	heap = (pairingheap *) palloc(sizeof(pairingheap));
	heap->ph_compare = compare;
	heap->ph_arg = arg;

	heap->ph_root = NULL;

	return heap;
}

/*
 * pairingheap_free
 *
 * 释放给定 pairingheap 使用的内存。
 *
 * 注意：堆中的节点不会被释放！
 */
void
pairingheap_free(pairingheap *heap)
{
	pfree(heap);
}

/*
 * 把两个子堆合并成一个的辅助函数。
 *
 * 值较小的子堆被放成另一个的孩子（假定是
 * 最大堆）。
 *
 * 输入节点的 next_sibling 和 prev_or_parent 指针被
 * 忽略。返回时，返回节点的 next_sibling 和 prev_or_parent
 * 指针是垃圾。
 */
static pairingheap_node *
merge(pairingheap *heap, pairingheap_node *a, pairingheap_node *b)
{
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	/* 交换 'a' 和 'b'，使 'a' 是值较大的那个 */
	if (heap->ph_compare(a, b, heap->ph_arg) < 0)
	{
		pairingheap_node *tmp;

		tmp = a;
		a = b;
		b = tmp;
	}

	/* 并把 'b' 放成 'a' 的孩子 */
	if (a->first_child)
		a->first_child->prev_or_parent = b;
	b->prev_or_parent = a;
	b->next_sibling = a->first_child;
	a->first_child = b;

	return a;
}

/*
 * pairingheap_add
 *
 * 在 O(1) 时间内把给定节点加入堆。
 */
void
pairingheap_add(pairingheap *heap, pairingheap_node *node)
{
	node->first_child = NULL;

	/* 把新节点链成一棵新树 */
	heap->ph_root = merge(heap, heap->ph_root, node);
	heap->ph_root->prev_or_parent = NULL;
	heap->ph_root->next_sibling = NULL;
}

/*
 * pairingheap_first
 *
 * 返回指向堆中第一个（根、最顶）节点的指针，不
 * 修改堆。调用者必须确保不对空堆使用本例程。
 * 总是 O(1)。
 */
pairingheap_node *
pairingheap_first(pairingheap *heap)
{
	Assert(!pairingheap_is_empty(heap));

	return heap->ph_root;
}

/*
 * pairingheap_remove_first
 *
 * 移除堆中第一个（根、最顶）节点，在重新平衡堆后
 * 返回指向它的指针。调用者必须确保不对空堆使用本
 * 例程。摊还 O(log n)。
 */
pairingheap_node *
pairingheap_remove_first(pairingheap *heap)
{
	pairingheap_node *result;
	pairingheap_node *children;

	Assert(!pairingheap_is_empty(heap));

	/* 移除根，并用其孩子们形成新堆。 */
	result = heap->ph_root;
	children = result->first_child;

	heap->ph_root = merge_children(heap, children);
	if (heap->ph_root)
	{
		heap->ph_root->prev_or_parent = NULL;
		heap->ph_root->next_sibling = NULL;
	}

	return result;
}

/*
 * 从堆中移除 'node'。摊还 O(log n)。
 */
void
pairingheap_remove(pairingheap *heap, pairingheap_node *node)
{
	pairingheap_node *children;
	pairingheap_node *replacement;
	pairingheap_node *next_sibling;
	pairingheap_node **prev_ptr;

	/*
	 * 若被移除的节点碰巧是根节点，则用
	 * pairingheap_remove_first() 来做。
	 */
	if (node == heap->ph_root)
	{
		(void) pairingheap_remove_first(heap);
		return;
	}

	/*
	 * 在修改任何东西之前，记住被移除节点的 first_child 和
	 * next_sibling 指针。
	 */
	children = node->first_child;
	next_sibling = node->next_sibling;

	/*
	 * 还要找到指向被移除节点的指针：在其前一兄弟中，
	 * 若这是其父的第一个孩子，则在其父中。
	 */
	if (node->prev_or_parent->first_child == node)
		prev_ptr = &node->prev_or_parent->first_child;
	else
		prev_ptr = &node->prev_or_parent->next_sibling;
	Assert(*prev_ptr == node);

	/*
	 * 若本节点有孩子，用孩子们做一个新子堆，并把
	 * 该子堆链到被移除节点的位置。否则只是解除本
	 * 节点的链接。
	 */
	if (children)
	{
		replacement = merge_children(heap, children);

		replacement->prev_or_parent = node->prev_or_parent;
		replacement->next_sibling = node->next_sibling;
		*prev_ptr = replacement;
		if (next_sibling)
			next_sibling->prev_or_parent = replacement;
	}
	else
	{
		*prev_ptr = next_sibling;
		if (next_sibling)
			next_sibling->prev_or_parent = node->prev_or_parent;
	}
}

/*
 * 把一列子堆合并成单个堆。
 *
 * 这实现基本的两趟合并策略，先从左到右形成对，
 * 然后再合并这些对。
 */
static pairingheap_node *
merge_children(pairingheap *heap, pairingheap_node *children)
{
	pairingheap_node *curr,
			   *next;
	pairingheap_node *pairs;
	pairingheap_node *newroot;

	if (children == NULL || children->next_sibling == NULL)
		return children;

	/* 从左到右走子堆，成对合并 */
	next = children;
	pairs = NULL;
	for (;;)
	{
		curr = next;

		if (curr == NULL)
			break;

		if (curr->next_sibling == NULL)
		{
			/* 列表末尾最后一个落单（未成对）的节点 */
			curr->next_sibling = pairs;
			pairs = curr;
			break;
		}

		next = curr->next_sibling->next_sibling;

		/* 合并本子堆和下一个，并加到 'pairs' 列表。 */

		curr = merge(heap, curr, curr->next_sibling);
		curr->next_sibling = pairs;
		pairs = curr;
	}

	/*
	 * 把所有对合并在一起，形成一个堆。
	 */
	newroot = pairs;
	next = pairs->next_sibling;
	while (next)
	{
		curr = next;
		next = curr->next_sibling;

		newroot = merge(heap, newroot, curr);
	}

	return newroot;
}

/*
 * 把堆内容转储为字符串的调试函数。
 *
 * 'dumpfunc' 回调把单个节点的字符串表示追加到
 * StringInfo。'opaque' 可用来向回调传递更多
 * 信息。
 */
#ifdef PAIRINGHEAP_DEBUG
static void
pairingheap_dump_recurse(StringInfo buf,
						 pairingheap_node *node,
						 void (*dumpfunc) (pairingheap_node *node, StringInfo buf, void *opaque),
						 void *opaque,
						 int depth,
						 pairingheap_node *prev_or_parent)
{
	while (node)
	{
		Assert(node->prev_or_parent == prev_or_parent);

		appendStringInfoSpaces(buf, depth * 4);
		dumpfunc(node, buf, opaque);
		appendStringInfoChar(buf, '\n');
		if (node->first_child)
			pairingheap_dump_recurse(buf, node->first_child, dumpfunc, opaque, depth + 1, node);
		prev_or_parent = node;
		node = node->next_sibling;
	}
}

char *
pairingheap_dump(pairingheap *heap,
				 void (*dumpfunc) (pairingheap_node *node, StringInfo buf, void *opaque),
				 void *opaque)
{
	StringInfoData buf;

	if (!heap->ph_root)
		return pstrdup("(empty)");

	initStringInfo(&buf);

	pairingheap_dump_recurse(&buf, heap->ph_root, dumpfunc, opaque, 0, NULL);

	return buf.data;
}
#endif
