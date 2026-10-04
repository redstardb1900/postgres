/*-------------------------------------------------------------------------
 *
 * rbtree.h
 *	  interface for PostgreSQL generic Red-Black binary tree package
 *
 * Copyright (c) 2009-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/lib/rbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RBTREE_H
#define RBTREE_H

/*
 * RBTNode 应作为更大结构体的第一个字段使用，其余字段
 * 携带调用者为树条目所需的负载数据。（那个更大结构体
 * 的总大小传给 rbt_create。）这里声明 RBTNode 是为了
 * 支持这种用法，但调用者必须把它当作不透明结构体。
 *
 */
typedef struct RBTNode
{
	char color;					/* 节点当前颜色，红或黑 */
	struct RBTNode *left;		/* 左孩子，若无则为 RBTNIL */
	struct RBTNode *right;		/* 右孩子，若无则为 RBTNIL */
	struct RBTNode *parent;		/* 父节点，若无则为 NULL（不是 RBTNIL！） */
} RBTNode;

/* 表示整棵树的不透明结构体 */
typedef struct RBTree RBTree;

/* 可用的树遍历顺序 */
typedef enum RBTOrderControl
{
	LeftRightWalk,				/* 中序：左孩子、节点、右孩子 */
	RightLeftWalk				/* 逆中序：右、节点、左 */
} RBTOrderControl;

/*
 * RBTreeIterator 在遍历树时保存状态。在此声明是为了
 * 让调用者能在栈上分配它，但除此之外必须当作
 * 不透明结构体。
 */
typedef struct RBTreeIterator RBTreeIterator;

struct RBTreeIterator
{
	RBTree	   *rbt;
	RBTNode    *(*iterate) (RBTreeIterator *iter);
	RBTNode    *last_visited;
	bool		is_over;
};

/* 由调用者提供的支持函数 */
typedef int (*rbt_comparator) (const RBTNode *a, const RBTNode *b, void *arg);
typedef void (*rbt_combiner) (RBTNode *existing, const RBTNode *newdata, void *arg);
typedef RBTNode *(*rbt_allocfunc) (void *arg);
typedef void (*rbt_freefunc) (RBTNode *x, void *arg);

extern RBTree *rbt_create(Size node_size,
						  rbt_comparator comparator,
						  rbt_combiner combiner,
						  rbt_allocfunc allocfunc,
						  rbt_freefunc freefunc,
						  void *arg);

extern RBTNode *rbt_find(RBTree *rbt, const RBTNode *data);
extern RBTNode *rbt_find_great(RBTree *rbt, const RBTNode *data, bool equal_match);
extern RBTNode *rbt_find_less(RBTree *rbt, const RBTNode *data, bool equal_match);
extern RBTNode *rbt_leftmost(RBTree *rbt);

extern RBTNode *rbt_insert(RBTree *rbt, const RBTNode *data, bool *isNew);
extern void rbt_delete(RBTree *rbt, RBTNode *node);

extern void rbt_begin_iterate(RBTree *rbt, RBTOrderControl ctrl,
							  RBTreeIterator *iter);
extern RBTNode *rbt_iterate(RBTreeIterator *iter);

#endif							/* RBTREE_H */
