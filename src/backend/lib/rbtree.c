/*-------------------------------------------------------------------------
 *
 * rbtree.c
 *	  implementation for PostgreSQL generic Red-Black binary tree package
 *	  Adopted from http://algolist.manual.ru/ds/rbtree.php
 *
 * This code comes from Thomas Niemann's "Sorting and Searching Algorithms:
 * a Cookbook".
 *
 * See http://www.cs.auckland.ac.nz/software/AlgAnim/niemann/s_man.htm for
 * license terms: "Source code, when part of a software project, may be used
 * freely without reference to the author."
 *
 * Red-black trees are a type of balanced binary tree wherein (1) any child of
 * a red node is always black, and (2) every path from root to leaf traverses
 * an equal number of black nodes.  From these properties, it follows that the
 * longest path from root to leaf is only about twice as long as the shortest,
 * so lookups are guaranteed to run in O(lg n) time.
 *
 * Copyright (c) 2009-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/rbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/rbtree.h"


/*
 * 节点颜色（RBTNode.color 的值）
 */
#define RBTBLACK	(0)
#define RBTRED		(1)

/*
 * RBTree 控制结构
 */
struct RBTree
{
	RBTNode    *root;			/* 根节点，若树为空则为 RBTNIL */

	/* 其余字段在 rbt_create 之后为常量 */

	Size		node_size;		/* 树节点的实际大小 */
	/* 调用者提供的操作函数 */
	rbt_comparator comparator;
	rbt_combiner combiner;
	rbt_allocfunc allocfunc;
	rbt_freefunc freefunc;
	/* 传给所有操作函数的透传参数 */
	void	   *arg;
};

/*
 * 所有叶子都是哨兵，使用定制的 NIL 名称以防
 * 与实际上是 NULL 的系统范围常量 NIL 冲突
 */
#define RBTNIL (&sentinel)

static RBTNode sentinel =
{
	.color = RBTBLACK,.left = RBTNIL,.right = RBTNIL,.parent = NULL
};


/*
 * rbt_create: 创建一个空的 RBTree
 *
 * 参数为：
 *	node_size: 树节点的实际大小（> sizeof(RBTNode)）
 *	操作函数：
 *	comparator: 比较两个 RBTNodes 的小于/等于/大于
 *	combiner: 把已有树条目与新条目合并
 *	allocfunc: 分配新的 RBTNode
 *	freefunc: 释放旧的 RBTNode
 *	arg: 将传给操作函数的透传指针
 *
 * 注意 combiner 的右手参数将是一个 "提议" 树节点，
 * 即 rbt_insert 的输入，其中 RBTNode 字段本身并
 * 无效。类似地，comparator 的任一输入也可能是 "提议"
 * 节点。这不应要紧，因为这些函数不应看 RBTNode
 * 字段，只看嵌入 RBTNode 的结构的额外字段。
 *
 * freefunc 应只是 pfree 或等价物；它不应试图
 * 释放任何附属数据，因为传给它的节点可能不含
 * 有效数据！若调用者不需要逐条回收空间，
 * freefunc 可以为 NULL。
 *
 * RBTree 节点在调用者的内存上下文中 palloc。注意
 * 树的所有内容实际上由调用者分配，而不是在这里。
 *
 * 由于树内容由调用者管理，目前没有显式
 * "destroy" 操作；通常通过重置或删除存放它的
 * 内存上下文来释放树。若你想，可以 pfree
 * 该 RBTree 节点。
 *
 */
RBTree *
rbt_create(Size node_size,
		   rbt_comparator comparator,
		   rbt_combiner combiner,
		   rbt_allocfunc allocfunc,
		   rbt_freefunc freefunc,
		   void *arg)
{
	RBTree	   *tree = (RBTree *) palloc(sizeof(RBTree));

	Assert(node_size > sizeof(RBTNode));

	tree->root = RBTNIL;
	tree->node_size = node_size;
	tree->comparator = comparator;
	tree->combiner = combiner;
	tree->allocfunc = allocfunc;
	tree->freefunc = freefunc;

	tree->arg = arg;

	return tree;
}

/* 把附加数据字段从一个 RBTNode 复制到另一个 */
static inline void
rbt_copy_data(RBTree *rbt, RBTNode *dest, const RBTNode *src)
{
	memcpy(dest + 1, src + 1, rbt->node_size - sizeof(RBTNode));
}

/**********************************************************************
 *						  搜索										  *
 **********************************************************************/

/*
 * rbt_find: 在 RBTree 中搜索一个值
 *
 * data 表示试图查找的值。其 RBTNode 字段不必
 * 有效，有意义的是更大结构中的额外数据。
 *
 * 返回匹配的树条目，若未找到则返回 NULL。
 */
RBTNode *
rbt_find(RBTree *rbt, const RBTNode *data)
{
	RBTNode    *node = rbt->root;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (cmp == 0)
			return node;
		else if (cmp < 0)
			node = node->left;
		else
			node = node->right;
	}

	return NULL;
}

/*
 * rbt_find_great: 在 RBTree 中搜索一个更大的值
 *
 * 若 equal_match 为 true，这将是大于或等于搜索。
 *
 * 返回匹配的树条目，若未找到则返回 NULL。
 */
RBTNode *
rbt_find_great(RBTree *rbt, const RBTNode *data, bool equal_match)
{
	RBTNode    *node = rbt->root;
	RBTNode    *greater = NULL;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (equal_match && cmp == 0)
			return node;
		else if (cmp < 0)
		{
			greater = node;
			node = node->left;
		}
		else
			node = node->right;
	}

	return greater;
}

/*
 * rbt_find_less: 在 RBTree 中搜索一个更小的值
 *
 * 若 equal_match 为 true，这将是小于或等于搜索。
 *
 * 返回匹配的树条目，若未找到则返回 NULL。
 */
RBTNode *
rbt_find_less(RBTree *rbt, const RBTNode *data, bool equal_match)
{
	RBTNode    *node = rbt->root;
	RBTNode    *lesser = NULL;

	while (node != RBTNIL)
	{
		int			cmp = rbt->comparator(data, node, rbt->arg);

		if (equal_match && cmp == 0)
			return node;
		else if (cmp > 0)
		{
			lesser = node;
			node = node->right;
		}
		else
			node = node->left;
	}

	return lesser;
}

/*
 * rbt_leftmost: 取最左（值最小）的树节点。
 * 若树为空则返回 NULL。
 *
 * 注意：原实现包含解除链接步骤，但那有点
 * 别扭。若那是你想要的，只需对结果调用 rbt_delete。
 *
 */
RBTNode *
rbt_leftmost(RBTree *rbt)
{
	RBTNode    *node = rbt->root;
	RBTNode    *leftmost = rbt->root;

	while (node != RBTNIL)
	{
		leftmost = node;
		node = node->left;
	}

	if (leftmost != RBTNIL)
		return leftmost;

	return NULL;
}

/**********************************************************************
 *							  插入									  *
 **********************************************************************/

/*
 * 把节点 x 向左旋转。
 *
 * x 的右孩子取代它在树中的位置，x 成为该
 * 节点的左孩子。
 */
static void
rbt_rotate_left(RBTree *rbt, RBTNode *x)
{
	RBTNode    *y = x->right;

	/* 建立 x->right 链接 */
	x->right = y->left;
	if (y->left != RBTNIL)
		y->left->parent = x;

	/* 建立 y->parent 链接 */
	if (y != RBTNIL)
		y->parent = x->parent;
	if (x->parent)
	{
		if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
	}
	else
	{
		rbt->root = y;
	}

	/* 链接 x 和 y */
	y->left = x;
	if (x != RBTNIL)
		x->parent = y;
}

/*
 * 把节点 x 向右旋转。
 *
 * x 的左孩子取代它在树中的位置，x 成为该
 * 节点的右孩子。
 */
static void
rbt_rotate_right(RBTree *rbt, RBTNode *x)
{
	RBTNode    *y = x->left;

	/* 建立 x->left 链接 */
	x->left = y->right;
	if (y->right != RBTNIL)
		y->right->parent = x;

	/* 建立 y->parent 链接 */
	if (y != RBTNIL)
		y->parent = x->parent;
	if (x->parent)
	{
		if (x == x->parent->right)
			x->parent->right = y;
		else
			x->parent->left = y;
	}
	else
	{
		rbt->root = y;
	}

	/* 链接 x 和 y */
	y->right = x;
	if (x != RBTNIL)
		x->parent = y;
}

/*
 * 插入节点 x 之后维持红黑树平衡。
 *
 * 新插入的节点总是最初标为红色。那可能导致
 * 红节点有红孩子，这是禁止的。我们总可以
 * 通过一系列变色和/或 "旋转" 来修复问题，
 * 把问题逐步移到树的更高处。若两个红节点
 * 之一是根，我们总可以通过把根从红变黑来
 * 修复问题。
 *
 * （这在树的更低处行不通，因为我们还必须维持
 * 每个叶子具有相等黑高这一不变量。）
 */
static void
rbt_insert_fixup(RBTree *rbt, RBTNode *x)
{
	/*
	 * x 总是红节点。最初它是新插入的节点。本循环
	 * 的每次迭代都把它移到树的更高处。
	 */
	while (x != rbt->root && x->parent->color == RBTRED)
	{
		/*
		 * x 和 x->parent 都是红色。修复取决于 x->parent 是
		 * 左孩子还是右孩子。无论哪种情况，我们把 y 定义为
		 * x 的 "uncle"（叔），即 x 祖父的另一个孩子。
		 *
		 * 若叔是红色，我们把祖父翻成红、其两个孩子翻成
		 * 黑。然后再次循环以检查祖父是否仍有问题。
		 *
		 * 若叔是黑色，我们将执行一次或两次 "旋转" 来
		 * 平衡树。x 或 x->parent 将占据祖父在树中的
		 * 位置并被重着色为黑，原来的祖父将被重着色为
		 * 红并成为该节点的孩子。这总是留给我们一棵
		 * 有效的红黑树，因此循环将终止。
 *
 *
 */
		if (x->parent == x->parent->parent->left)
		{
			RBTNode    *y = x->parent->parent->right;

			if (y->color == RBTRED)
			{
				/* 叔是 RBTRED */
				x->parent->color = RBTBLACK;
				y->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				x = x->parent->parent;
			}
			else
			{
				/* 叔是 RBTBLACK */
				if (x == x->parent->right)
				{
					/* 使 x 成为左孩子 */
					x = x->parent;
					rbt_rotate_left(rbt, x);
				}

				/* 重着色并旋转 */
				x->parent->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				rbt_rotate_right(rbt, x->parent->parent);
			}
		}
		else
		{
			/* 上面代码的镜像 */
			RBTNode    *y = x->parent->parent->left;

			if (y->color == RBTRED)
			{
				/* 叔是 RBTRED */
				x->parent->color = RBTBLACK;
				y->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				x = x->parent->parent;
			}
			else
			{
				/* 叔是 RBTBLACK */
				if (x == x->parent->left)
				{
					x = x->parent;
					rbt_rotate_right(rbt, x);
				}
				x->parent->color = RBTBLACK;
				x->parent->parent->color = RBTRED;

				rbt_rotate_left(rbt, x->parent->parent);
			}
		}
	}

	/*
	 * 根可能已经是黑色；若不是，树中每个节点的
	 * 黑高增加一。
	 */
	rbt->root->color = RBTBLACK;
}

/*
 * rbt_insert: 向树中插入一个新值。
 *
 * data 表示要插入的值。其 RBTNode 字段不必
 * 有效，有意义的是更大结构中的额外数据。
 *
 * 若 "data" 表示的值不在树中，则我们把
 * "data" 复制进新树条目并返回该节点，把 *isNew
 * 设为 true。
 *
 * 若 "data" 表示的值已存在，则我们调用
 * combiner 函数把 data 合并进已有节点，并返回
 * 已有节点，把 *isNew 设为 false。
 *
 * 无论哪种情况 "data" 都不被修改；它通常只是
 * 调用者中的局部变量。
 */
RBTNode *
rbt_insert(RBTree *rbt, const RBTNode *data, bool *isNew)
{
	RBTNode    *current,
			   *parent,
			   *x;
	int			cmp;

	/* 找出节点应属的位置 */
	current = rbt->root;
	parent = NULL;
	cmp = 0;					/* 只是为了防止编译器警告 */

	while (current != RBTNIL)
	{
		cmp = rbt->comparator(data, current, rbt->arg);
		if (cmp == 0)
		{
			/*
			 * 找到具有给定键的节点。应用 combiner。
			 */
			rbt->combiner(current, data, rbt->arg);
			*isNew = false;
			return current;
		}
		parent = current;
		current = (cmp < 0) ? current->left : current->right;
	}

	/*
	 * 值不存在，因此创建一个含 data 的新节点。
	 */
	*isNew = true;

	x = rbt->allocfunc(rbt->arg);

	x->color = RBTRED;

	x->left = RBTNIL;
	x->right = RBTNIL;
	x->parent = parent;
	rbt_copy_data(rbt, x, data);

	/* 把节点插入树中 */
	if (parent)
	{
		if (cmp < 0)
			parent->left = x;
		else
			parent->right = x;
	}
	else
	{
		rbt->root = x;
	}

	rbt_insert_fixup(rbt, x);

	return x;
}

/**********************************************************************
 *							删除									  *
 **********************************************************************/

/*
 * 删除黑节点之后维持红黑树平衡。
 */
static void
rbt_delete_fixup(RBTree *rbt, RBTNode *x)
{
	/*
	 * x 总是黑节点。最初它是被删除节点的原
	 * 孩子。本循环的每次迭代都把它移到树的
	 * 更高处。
	 */
	while (x != rbt->root && x->color == RBTBLACK)
	{
		/*
		 * 左、右情形是对称的。作为 x 的孩子的任何节点
		 * 的黑高比树中其余节点少一。我们旋转并重着色
		 * 节点以把问题沿树向上移：在某阶段我们要么修
		 * 复问题，要么到达根（那里允许黑高下降）。
 *
 */
		if (x == x->parent->left)
		{
			RBTNode    *w = x->parent->right;

			if (w->color == RBTRED)
			{
				w->color = RBTBLACK;
				x->parent->color = RBTRED;

				rbt_rotate_left(rbt, x->parent);
				w = x->parent->right;
			}

			if (w->left->color == RBTBLACK && w->right->color == RBTBLACK)
			{
				w->color = RBTRED;

				x = x->parent;
			}
			else
			{
				if (w->right->color == RBTBLACK)
				{
					w->left->color = RBTBLACK;
					w->color = RBTRED;

					rbt_rotate_right(rbt, w);
					w = x->parent->right;
				}
				w->color = x->parent->color;
				x->parent->color = RBTBLACK;
				w->right->color = RBTBLACK;

				rbt_rotate_left(rbt, x->parent);
				x = rbt->root;	/* 安排循环终止。 */
			}
		}
		else
		{
			RBTNode    *w = x->parent->left;

			if (w->color == RBTRED)
			{
				w->color = RBTBLACK;
				x->parent->color = RBTRED;

				rbt_rotate_right(rbt, x->parent);
				w = x->parent->left;
			}

			if (w->right->color == RBTBLACK && w->left->color == RBTBLACK)
			{
				w->color = RBTRED;

				x = x->parent;
			}
			else
			{
				if (w->left->color == RBTBLACK)
				{
					w->right->color = RBTBLACK;
					w->color = RBTRED;

					rbt_rotate_left(rbt, w);
					w = x->parent->left;
				}
				w->color = x->parent->color;
				x->parent->color = RBTBLACK;
				w->left->color = RBTBLACK;

				rbt_rotate_right(rbt, x->parent);
				x = rbt->root;	/* 安排循环终止。 */
			}
		}
	}
	x->color = RBTBLACK;
}

/*
 * 从树中删除节点 z。
 */
static void
rbt_delete_node(RBTree *rbt, RBTNode *z)
{
	RBTNode    *x,
			   *y;

	/* 这只是偏执：我们应只在有效节点上被调用 */
	if (!z || z == RBTNIL)
		return;

	/*
	 * y 是实际将从树中移除的节点。若 z 的孩子少
	 * 于两个，这将是 z，否则是 z 的树后继。
 *
 */
	if (z->left == RBTNIL || z->right == RBTNIL)
	{
		/* y 有一个作为孩子的 RBTNIL 节点 */
		y = z;
	}
	else
	{
		/* 找树后继 */
		y = z->right;
		while (y->left != RBTNIL)
			y = y->left;
	}

	/* x 是 y 的唯一孩子 */
	if (y->left != RBTNIL)
		x = y->left;
	else
		x = y->right;

	/* 从树中移除 y。 */
	x->parent = y->parent;
	if (y->parent)
	{
		if (y == y->parent->left)
			y->parent->left = x;
		else
			y->parent->right = x;
	}
	else
	{
		rbt->root = x;
	}

	/*
	 * 若我们移除的是 z 的树后继而不是 z 本身，则把
	 * 被移除节点的数据移到本应移除的那个节点上。
	 */
	if (y != z)
		rbt_copy_data(rbt, z, y);

	/*
	 * 移除黑节点可能使某些从根到叶的路径含有
	 * 比其他路径更少的黑节点，或使两个红节点相邻。
	 */
	if (y->color == RBTBLACK)
		rbt_delete_fixup(rbt, x);

	/* 现在可以回收 y 节点 */
	if (rbt->freefunc)
		rbt->freefunc(y, rbt->arg);
}

/*
 * rbt_delete: 移除给定的树条目
 *
 * "node" 必须先前已通过 rbt_find 或 rbt_leftmost 找到。
 * 调用者有责任在调用 rbt_delete 之前释放附着到该
 * 节点的任何附属数据。（不要试图把那责任推给
 * freefunc，因为实际被释放的可能是另一个物理节点！）
 *
 */
void
rbt_delete(RBTree *rbt, RBTNode *node)
{
	rbt_delete_node(rbt, node);
}

/**********************************************************************
 *						  遍历										  *
 **********************************************************************/

static RBTNode *
rbt_left_right_iterator(RBTreeIterator *iter)
{
	if (iter->last_visited == NULL)
	{
		iter->last_visited = iter->rbt->root;
		while (iter->last_visited->left != RBTNIL)
			iter->last_visited = iter->last_visited->left;

		return iter->last_visited;
	}

	if (iter->last_visited->right != RBTNIL)
	{
		iter->last_visited = iter->last_visited->right;
		while (iter->last_visited->left != RBTNIL)
			iter->last_visited = iter->last_visited->left;

		return iter->last_visited;
	}

	for (;;)
	{
		RBTNode    *came_from = iter->last_visited;

		iter->last_visited = iter->last_visited->parent;
		if (iter->last_visited == NULL)
		{
			iter->is_over = true;
			break;
		}

		if (iter->last_visited->left == came_from)
			break;				/* 来自左子树，返回当前
								 * 节点 */

		/* 否则来自右子树，继续向上走 */
	}

	return iter->last_visited;
}

static RBTNode *
rbt_right_left_iterator(RBTreeIterator *iter)
{
	if (iter->last_visited == NULL)
	{
		iter->last_visited = iter->rbt->root;
		while (iter->last_visited->right != RBTNIL)
			iter->last_visited = iter->last_visited->right;

		return iter->last_visited;
	}

	if (iter->last_visited->left != RBTNIL)
	{
		iter->last_visited = iter->last_visited->left;
		while (iter->last_visited->right != RBTNIL)
			iter->last_visited = iter->last_visited->right;

		return iter->last_visited;
	}

	for (;;)
	{
		RBTNode    *came_from = iter->last_visited;

		iter->last_visited = iter->last_visited->parent;
		if (iter->last_visited == NULL)
		{
			iter->is_over = true;
			break;
		}

		if (iter->last_visited->right == came_from)
			break;				/* 来自右子树，返回当前
								 * 节点 */

		/* 否则来自左子树，继续向上走 */
	}

	return iter->last_visited;
}

/*
 * rbt_begin_iterate: 准备按若干顺序之一遍历树
 *
 * 调用 rbt_begin_iterate 之后，反复调用 rbt_iterate 直到它
 * 返回 NULL 或遍历不再有意义。
 *
 * 若遍历期间树被改变，对 rbt_iterate 的后续调用结果
 * 未指定。同一树上允许多个并发迭代器。
 *
 * 迭代器状态存在 'iter' 结构中。调用者应把它当作
 * 不透明结构对待。
 *
 */
void
rbt_begin_iterate(RBTree *rbt, RBTOrderControl ctrl, RBTreeIterator *iter)
{
	/* 所有遍历顺序的公共初始化 */
	iter->rbt = rbt;
	iter->last_visited = NULL;
	iter->is_over = (rbt->root == RBTNIL);

	switch (ctrl)
	{
		case LeftRightWalk:		/* 先访问左，再自身，再右 */
			iter->iterate = rbt_left_right_iterator;
			break;
		case RightLeftWalk:		/* 先访问右，再自身，再左 */
			iter->iterate = rbt_right_left_iterator;
			break;
		default:
			elog(ERROR, "unrecognized rbtree iteration order: %d", ctrl);
	}
}

/*
 * rbt_iterate: 按遍历顺序返回下一节点，若没有更多则返回 NULL
 */
RBTNode *
rbt_iterate(RBTreeIterator *iter)
{
	if (iter->is_over)
		return NULL;

	return iter->iterate(iter);
}
