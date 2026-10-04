/*
 * pairingheap.h
 *
 * A Pairing Heap implementation
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * src/include/lib/pairingheap.h
 */

#ifndef PAIRINGHEAP_H
#define PAIRINGHEAP_H

#include "lib/stringinfo.h"

/* 若需要 pairingheap_dump() 调试函数则启用 */
/* #define PAIRINGHEAP_DEBUG */

/*
 * 这表示存放在堆中的一个元素。把它嵌入更大的结构体，
 * 结构体里放你实际要存的数据。
 *
 * 一个节点可以有多个孩子，它们组成双向链表。
 * first_child 指向该节点的第一个孩子，随后的孩子
 * 可顺着 next_sibling 指针找到。最后一个孩子的
 * next_sibling == NULL。prev_or_parent 指向该节点的
 * 前一个兄弟；若本节点是其父的第一个孩子，则指向
 * 父节点。
 */
typedef struct pairingheap_node
{
	struct pairingheap_node *first_child;
	struct pairingheap_node *next_sibling;
	struct pairingheap_node *prev_or_parent;
} pairingheap_node;

/*
 * 返回 'type' 的外层结构体，其中 'membername' 是
 * 'ptr' 所指的 pairingheap_node。
 *
 * 用来把 pairingheap_node * 转回其外层结构体。
 */
#define pairingheap_container(type, membername, ptr) \
	(AssertVariableIsOfTypeMacro(ptr, pairingheap_node *), \
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, pairingheap_node),  \
	 ((type *) ((char *) (ptr) - offsetof(type, membername))))

/*
 * 同 pairingheap_container，但用于指针为 'const ptr' 时。
 */
#define pairingheap_const_container(type, membername, ptr) \
	(AssertVariableIsOfTypeMacro(ptr, const pairingheap_node *), \
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, pairingheap_node),  \
	 ((const type *) ((const char *) (ptr) - offsetof(type, membername))))

/*
 * 对最大堆，比较函数必须在 a < b 时返回 <0，a == b 时返回 0，
 * a > b 时返回 >0。对最小堆，这些条件反过来。
 */
typedef int (*pairingheap_comparator) (const pairingheap_node *a,
									   const pairingheap_node *b,
									   void *arg);

/*
 * 一个配对堆。
 *
 * 可以用 pairingheap_allocate() 创建经 palloc 分配的新堆，
 * 或把本结构嵌入更大的结构体，直接设置 ph_compare 与
 * ph_arg，并把 ph_root 初始化为 NULL。
 */
typedef struct pairingheap
{
	pairingheap_comparator ph_compare;	/* 比较函数 */
	void	   *ph_arg;			/* 传给 ph_compare 的不透明参数 */
	pairingheap_node *ph_root;	/* 堆的当前根 */
} pairingheap;

extern pairingheap *pairingheap_allocate(pairingheap_comparator compare,
										 void *arg);
extern void pairingheap_free(pairingheap *heap);
extern void pairingheap_add(pairingheap *heap, pairingheap_node *node);
extern pairingheap_node *pairingheap_first(pairingheap *heap);
extern pairingheap_node *pairingheap_remove_first(pairingheap *heap);
extern void pairingheap_remove(pairingheap *heap, pairingheap_node *node);

#ifdef PAIRINGHEAP_DEBUG
extern char *pairingheap_dump(pairingheap *heap,
							  void (*dumpfunc) (pairingheap_node *node, StringInfo buf, void *opaque),
							  void *opaque);
#endif

/* 把堆重置为空。 */
#define pairingheap_reset(h)			((h)->ph_root = NULL)

/* 堆是否为空？ */
#define pairingheap_is_empty(h)			((h)->ph_root == NULL)

/* 堆中是否恰好有一个节点？ */
#define pairingheap_is_singular(h) \
	((h)->ph_root && (h)->ph_root->first_child == NULL)

#endif							/* PAIRINGHEAP_H */
