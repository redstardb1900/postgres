/*-------------------------------------------------------------------------
 *
 * ilist.h
 *		integrated/inline doubly- and singly-linked lists
 *
 * These list types are useful when there are only a predetermined set of
 * lists that an object could be in.  List links are embedded directly into
 * the objects, and thus no extra memory management overhead is required.
 * (Of course, if only a small proportion of existing objects are in a list,
 * the link fields in the remainder would be wasted space.  But usually,
 * it saves space to not have separately-allocated list nodes.)
 *
 * The doubly-linked list comes in 2 forms.  dlist_head defines a head of a
 * doubly-linked list of dlist_nodes, whereas dclist_head defines the head of
 * a doubly-linked list of dlist_nodes with an additional 'count' field to
 * keep track of how many items are contained within the given list.  For
 * simplicity, dlist_head and dclist_head share the same node and iterator
 * types.  The functions to manipulate a dlist_head always have a name
 * starting with "dlist", whereas functions to manipulate a dclist_head have a
 * name starting with "dclist".  dclist_head comes with an additional function
 * (dclist_count) to return the number of entries in the list.  dclists are
 * able to store a maximum of PG_UINT32_MAX elements.  It is up to the caller
 * to ensure no more than this many items are added to a dclist.
 *
 * None of the functions here allocate any memory; they just manipulate
 * externally managed memory.  With the exception doubly-linked count lists
 * providing the ability to obtain the number of items in the list, the APIs
 * for singly and both doubly linked lists are identical as far as
 * capabilities of both allow.
 *
 * Each list has a list header, which exists even when the list is empty.
 * An empty singly-linked list has a NULL pointer in its header.
 *
 * For both doubly-linked list types, there are two valid ways to represent an
 * empty list.  The head's 'next' pointer can either be NULL or the head's
 * 'next' and 'prev' links can both point back to the list head (circular).
 * (If a dlist is modified and then all its elements are deleted, it will be
 * in the circular state.).  We prefer circular dlists because there are some
 * operations that can be done without branches (and thus faster) on lists
 * that use circular representation.  However, it is often convenient to
 * initialize list headers to zeroes rather than setting them up with an
 * explicit initialization function, so we also allow the NULL initialization.
 *
 * EXAMPLES
 *
 * Here's a simple example demonstrating how this can be used.  Let's assume
 * we want to store information about the tables contained in a database.
 *
 * #include "lib/ilist.h"
 *
 * // Define struct for the databases including a list header that will be
 * // used to access the nodes in the table list later on.
 * typedef struct my_database
 * {
 *		char	   *datname;
 *		dlist_head	tables;
 *		// ...
 * } my_database;
 *
 * // Define struct for the tables.  Note the list_node element which stores
 * // prev/next list links.  The list_node element need not be first.
 * typedef struct my_table
 * {
 *		char	   *tablename;
 *		dlist_node	list_node;
 *		perm_t		permissions;
 *		// ...
 * } my_table;
 *
 * // create a database
 * my_database *db = create_database();
 *
 * // and add a few tables to its table list
 * dlist_push_head(&db->tables, &create_table(db, "a")->list_node);
 * ...
 * dlist_push_head(&db->tables, &create_table(db, "b")->list_node);
 *
 *
 * To iterate over the table list, we allocate an iterator variable and use
 * a specialized looping construct.  Inside a dlist_foreach, the iterator's
 * 'cur' field can be used to access the current element.  iter.cur points to
 * a 'dlist_node', but most of the time what we want is the actual table
 * information; dlist_container() gives us that, like so:
 *
 * dlist_iter	iter;
 * dlist_foreach(iter, &db->tables)
 * {
 *		my_table   *tbl = dlist_container(my_table, list_node, iter.cur);
 *		printf("we have a table: %s in database %s\n",
 *			   tbl->tablename, db->datname);
 * }
 *
 *
 * While a simple iteration is useful, we sometimes also want to manipulate
 * the list while iterating.  There is a different iterator element and looping
 * construct for that.  Suppose we want to delete tables that meet a certain
 * criterion:
 *
 * dlist_mutable_iter miter;
 * dlist_foreach_modify(miter, &db->tables)
 * {
 *		my_table   *tbl = dlist_container(my_table, list_node, miter.cur);
 *
 *		if (!tbl->to_be_deleted)
 *			continue;		// don't touch this one
 *
 *		// unlink the current table from the linked list
 *		dlist_delete(miter.cur);
 *		// as these lists never manage memory, we can still access the table
 *		// after it's been unlinked
 *		drop_table(db, tbl);
 * }
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/lib/ilist.h
 *-------------------------------------------------------------------------
 */
#ifndef ILIST_H
#define ILIST_H

/*
 * 启用额外调试。这相当昂贵，因此即使在
 * USE_ASSERT_CHECKING 下默认也不启用。
 */
/* #define ILIST_DEBUG */

/*
 * 双向链表的节点。
 *
 * 把它嵌入需要成为双向链表一部分的结构体。
 */
typedef struct dlist_node dlist_node;
struct dlist_node
{
	dlist_node *prev;
	dlist_node *next;
};

/*
 * 双向链表的表头。
 *
 * 非空链表在内部是环形链接的。环形链表的好处是，
 * 最常见的链表操作不需要任何分支。空链表也可以
 * 表示为一对 NULL 指针，从而使初始化更容易。
 *
 */
typedef struct dlist_head
{
	/*
 * head.next 要么指向链表的第一个元素；若是环形空链表
 * 则指向 &head；若为空且非环形则指向 NULL。
 *
 * head.prev 要么指向链表的最后一个元素；若是环形空链表
 * 则指向 &head；若为空且非环形则指向 NULL。
 */
	dlist_node	head;
} dlist_head;


/*
 * 用于 dlist_head 与 dclist_head 类型的双向链表迭代器。
 *
 * 在 dlist_foreach() 与 dlist_reverse_foreach()（以及
 * 对应的 dclist 变体）中用作状态。
 *
 * 要取得迭代的当前元素，使用成员 'cur'。
 *
 * 使用本迭代器时，*不得*在迭代期间改动链表！
 *
 * 注意：这里多用一个 "end" 字段，是为了避免在
 * dlist_foreach() 与 dclist_foreach() 宏中多次求值参数。
 */
typedef struct dlist_iter
{
	dlist_node *cur;			/* 当前元素 */
	dlist_node *end;			/* 将迭代到的最后节点 */
} dlist_iter;

/*
 * 用于 dlist_head 与 dclist_head 两种类型的双向链表迭代器。
 * 这种迭代器允许在迭代期间做一些修改。
 *
 * 在 dlist_foreach_modify() 与 dclist_foreach_modify() 中用作状态。
 *
 * 要取得迭代的当前元素，使用成员 'cur'。
 *
 * 使用本迭代器时，只允许在当前迭代点改动链表。删除当前
 * 节点没问题，但插入或删除相邻节点则不行。
 *
 * 注意：可变迭代需要单独的类型，以便在当前节点被删除或
 * 修改时保存它的 'next' 节点。
 *
 */
typedef struct dlist_mutable_iter
{
	dlist_node *cur;			/* 当前元素 */
	dlist_node *next;			/* 将迭代到的下一节点 */
	dlist_node *end;			/* 将迭代到的最后节点 */
} dlist_mutable_iter;

/*
 * 带元素个数计数的双向链表表头。
 *
 * 内部用 dlist 实现真正的链表。向链表添加或移除
 * 元素时，计数会更新以反映链表中的当前元素个数。
 *
 */
typedef struct dclist_head
{
	dlist_head	dlist;			/* 真正的链表表头 */
	uint32		count;			/* 链表中的元素个数 */
} dclist_head;

/*
 * 单向链表的节点。
 *
 * 把它嵌入需要成为单向链表一部分的结构体。
 */
typedef struct slist_node slist_node;
struct slist_node
{
	slist_node *next;
};

/*
 * 单向链表的表头。
 *
 * 与双向链表不同，单向链表不是环形链接的；为空时
 * 只把 head.next 设为 NULL。这在通常的操作中不会
 * 引入额外分支。
 */
typedef struct slist_head
{
	slist_node	head;
} slist_head;

/*
 * 单向链表迭代器。
 *
 * 在 slist_foreach() 中用作状态。要取得迭代的当前
 * 元素，使用成员 'cur'。
 *
 * 允许在迭代期间修改链表，但删除迭代器当前节点除外；
 * 若之后还要继续迭代，删除该节点需要小心。（这样做，
 * 同时还删除或插入相邻链表元素，可能行为异常；另外，
 * 若使用者释放了当前节点的存储，继续迭代就不安全。）
 *
 * 注意：这其实不必是额外的结构体，可以直接用
 * slist_node *。我们更喜欢单独的类型，以求一致。
 *
 *
 */
typedef struct slist_iter
{
	slist_node *cur;
} slist_iter;

/*
 * 允许在迭代期间做一些修改的单向链表迭代器。
 *
 * 在 slist_foreach_modify() 中用作状态。要取得迭代的
 * 当前元素，使用成员 'cur'。
 *
 * 迭代期间唯一允许的链表修改，是通过 slist_delete_current()
 * 移除当前节点（*不是* slist_delete()）。插入或删除与当前
 * 节点相邻的节点会行为异常。
 */
typedef struct slist_mutable_iter
{
	slist_node *cur;			/* 当前元素 */
	slist_node *next;			/* 将迭代到的下一节点 */
	slist_node *prev;			/* 前一节点，供删除使用 */
} slist_mutable_iter;


/* 静态初始化器 */
#define DLIST_STATIC_INIT(name) {{&(name).head, &(name).head}}
#define DCLIST_STATIC_INIT(name) {{{&(name).dlist.head, &(name).dlist.head}}, 0}
#define SLIST_STATIC_INIT(name) {{NULL}}


/* 因体积太大而不做成内联的函数原型 */

/* 注意：这是 O(n)；考虑改用 slist_delete_current()。 */
extern void slist_delete(slist_head *head, const slist_node *node);

#ifdef ILIST_DEBUG
extern void dlist_member_check(const dlist_head *head, const dlist_node *node);
extern void dlist_check(const dlist_head *head);
extern void slist_check(const slist_head *head);
#else
/*
 * 这些看似无用的转换成 void，是为了在非调试编译中让
 * 编译器不再抱怨参数未使用。在那些函数里，传入链表
 * 表头指针的唯一目的就是能够运行这些检查。
 *
 */
#define dlist_member_check(head, node) ((void) (head))
#define dlist_check(head)	((void) (head))
#define slist_check(head)	((void) (head))
#endif							/* ILIST_DEBUG */

/* 双向链表实现 */

/*
 * 初始化双向链表。
 * 先前状态会被丢弃，不做任何清理。
 */
static inline void
dlist_init(dlist_head *head)
{
	head->head.next = head->head.prev = &head->head;
}

/*
 * 初始化双向链表元素。
 *
 * 只有可能需要 dlist_node_is_detached() 时才有必要。
 */
static inline void
dlist_node_init(dlist_node *node)
{
	node->next = node->prev = NULL;
}

/*
 * 链表是否为空？
 *
 * 空链表的第一个 'next' 指针要么为 NULL，要么指向自身。
 */
static inline bool
dlist_is_empty(const dlist_head *head)
{
	dlist_check(head);

	return head->head.next == NULL || head->head.next == &(head->head);
}

/*
 * 在链表开头插入一个节点。
 */
static inline void
dlist_push_head(dlist_head *head, dlist_node *node)
{
	if (head->head.next == NULL)	/* 把 NULL 表头转成环形 */
		dlist_init(head);

	node->next = head->head.next;
	node->prev = &head->head;
	node->next->prev = node;
	head->head.next = node;

	dlist_check(head);
}

/*
 * 在链表末尾插入一个节点。
 */
static inline void
dlist_push_tail(dlist_head *head, dlist_node *node)
{
	if (head->head.next == NULL)	/* 把 NULL 表头转成环形 */
		dlist_init(head);

	node->next = &head->head;
	node->prev = head->head.prev;
	node->prev->next = node;
	head->head.prev = node;

	dlist_check(head);
}

/*
 * 在同一链表中的另一个节点之后插入节点。
 */
static inline void
dlist_insert_after(dlist_node *after, dlist_node *node)
{
	node->prev = after;
	node->next = after->next;
	after->next = node;
	node->next->prev = node;
}

/*
 * 在同一链表中的另一个节点之前插入节点。
 */
static inline void
dlist_insert_before(dlist_node *before, dlist_node *node)
{
	node->prev = before->prev;
	node->next = before;
	before->prev = node;
	node->prev->next = node;
}

/*
 * 从其所在链表中删除 'node'（它必须在某个链表中）。
 */
static inline void
dlist_delete(dlist_node *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

/*
 * 同 dlist_delete()，但还会把 next/prev 设为 NULL，以表示
 * 不在任何链表中。
 */
static inline void
dlist_delete_thoroughly(dlist_node *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->next = NULL;
	node->prev = NULL;
}

/*
 * 同 dlist_delete，但在 ILIST_DEBUG 构建中做检查，确保
 * 'node' 属于 'head'。
 */
static inline void
dlist_delete_from(dlist_head *head, dlist_node *node)
{
	dlist_member_check(head, node);
	dlist_delete(node);
}

/*
 * 同 dlist_delete_from，但还会把 next/prev 设为 NULL，以表示
 * 不在任何链表中。
 */
static inline void
dlist_delete_from_thoroughly(dlist_head *head, dlist_node *node)
{
	dlist_member_check(head, node);
	dlist_delete_thoroughly(node);
}

/*
 * 移除并返回链表的第一个节点（必须存在一个）。
 */
static inline dlist_node *
dlist_pop_head_node(dlist_head *head)
{
	dlist_node *node;

	Assert(!dlist_is_empty(head));
	node = head->head.next;
	dlist_delete(node);
	return node;
}

/*
 * 把元素从链表中的当前位置移到同一链表的表头位置。
 *
 * 若 'node' 尚不是该链表的一部分，则行为未定义。
 *
 */
static inline void
dlist_move_head(dlist_head *head, dlist_node *node)
{
	/* 若已在表头则走快速路径 */
	if (head->head.next == node)
		return;

	dlist_delete(node);
	dlist_push_head(head, node);

	dlist_check(head);
}

/*
 * 把元素从链表中的当前位置移到同一链表的表尾位置。
 *
 * 若 'node' 尚不是该链表的一部分，则行为未定义。
 *
 */
static inline void
dlist_move_tail(dlist_head *head, dlist_node *node)
{
	/* 若已在表尾则走快速路径 */
	if (head->head.prev == node)
		return;

	dlist_delete(node);
	dlist_push_tail(head, node);

	dlist_check(head);
}

/*
 * 检查 'node' 是否有后继节点。
 * 注意：若 'node' 不在链表中，结果不可靠。
 */
static inline bool
dlist_has_next(const dlist_head *head, const dlist_node *node)
{
	return node->next != &head->head;
}

/*
 * 检查 'node' 是否有前驱节点。
 * 注意：若 'node' 不在链表中，结果不可靠。
 */
static inline bool
dlist_has_prev(const dlist_head *head, const dlist_node *node)
{
	return node->prev != &head->head;
}

/*
 * 检查节点是否已脱离链表。节点只有在用 dlist_init_node()
 * 初始化之后，或用 dlist_delete_thoroughly() /
 * dlist_delete_from_thoroughly() / dclist_delete_from_thoroughly()
 * 删除之后，才算脱离。
 */
static inline bool
dlist_node_is_detached(const dlist_node *node)
{
	Assert((node->next == NULL && node->prev == NULL) ||
		   (node->next != NULL && node->prev != NULL));

	return node->next == NULL;
}

/*
 * 返回链表中的下一节点（必须存在一个）。
 */
static inline dlist_node *
dlist_next_node(dlist_head *head, dlist_node *node)
{
	Assert(dlist_has_next(head, node));
	return node->next;
}

/*
 * 返回链表中的前一节点（必须存在一个）。
 */
static inline dlist_node *
dlist_prev_node(dlist_head *head, dlist_node *node)
{
	Assert(dlist_has_prev(head, node));
	return node->prev;
}

/* 内部辅助函数，取得头元素结构体的地址 */
static inline void *
dlist_head_element_off(dlist_head *head, size_t off)
{
	Assert(!dlist_is_empty(head));
	return (char *) head->head.next - off;
}

/*
 * 返回链表中的第一个节点（必须存在一个）。
 */
static inline dlist_node *
dlist_head_node(dlist_head *head)
{
	return (dlist_node *) dlist_head_element_off(head, 0);
}

/* 内部辅助函数，取得尾元素结构体的地址 */
static inline void *
dlist_tail_element_off(dlist_head *head, size_t off)
{
	Assert(!dlist_is_empty(head));
	return (char *) head->head.prev - off;
}

/*
 * 返回链表中的最后一个节点（必须存在一个）。
 */
static inline dlist_node *
dlist_tail_node(dlist_head *head)
{
	return (dlist_node *) dlist_tail_element_off(head, 0);
}

/*
 * 返回 'type' 的外层结构体，其中 'membername' 是
 * 'ptr' 所指的 dlist_node。
 *
 * 用来把 dlist_node * 转回其外层结构体。
 */
#define dlist_container(type, membername, ptr)								\
	(AssertVariableIsOfTypeMacro(ptr, dlist_node *),						\
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) ((char *) (ptr) - offsetof(type, membername))))

/*
 * 返回链表中第一个元素的地址。
 *
 * 链表不得为空。
 */
#define dlist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 (type *) dlist_head_element_off(lhead, offsetof(type, membername)))

/*
 * 返回链表中最后一个元素的地址。
 *
 * 链表不得为空。
 */
#define dlist_tail_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) dlist_tail_element_off(lhead, offsetof(type, membername))))

/*
 * 遍历 'lhead' 所指的链表，把状态存在 'iter' 中。
 *
 * 用 iter.cur 访问当前元素。
 *
 * 迭代期间*不得*改动链表。
 */
#define dlist_foreach(iter, lhead)											\
	for (AssertVariableIsOfTypeMacro(iter, dlist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->next ? (iter).end->next : (iter).end;		\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).cur->next)

/*
 * 遍历 'lhead' 所指的链表，把状态存在 'iter' 中。
 *
 * 用 iter.cur 访问当前元素。
 *
 * 使用本迭代时，只允许在当前迭代点改动链表。删除当前
 * 节点没问题，但插入或删除相邻节点则不行。
 *
 */
#define dlist_foreach_modify(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, dlist_mutable_iter),				\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->next ? (iter).end->next : (iter).end,		\
		 (iter).next = (iter).cur->next;									\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).next, (iter).next = (iter).cur->next)

/*
 * 按相反顺序遍历链表。
 *
 * 迭代期间*不得*改动链表。
 */
#define dlist_reverse_foreach(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, dlist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, dlist_head *),					\
		 (iter).end = &(lhead)->head,										\
		 (iter).cur = (iter).end->prev ? (iter).end->prev : (iter).end;		\
		 (iter).cur != (iter).end;											\
		 (iter).cur = (iter).cur->prev)

/* 带计数的双向链表实现 */

/*
 * dclist_init
 *		初始化带计数的双向链表。
 *
 * 先前状态会被丢弃，不做任何清理。
 */
static inline void
dclist_init(dclist_head *head)
{
	dlist_init(&head->dlist);
	head->count = 0;
}

/*
 * dclist_is_empty
 *		链表为空则返回 true，否则返回 false。
 */
static inline bool
dclist_is_empty(const dclist_head *head)
{
	Assert(dlist_is_empty(&head->dlist) == (head->count == 0));
	return (head->count == 0);
}

/*
 * dclist_push_head
 *		在链表开头插入一个节点。
 */
static inline void
dclist_push_head(dclist_head *head, dlist_node *node)
{
	if (head->dlist.head.next == NULL)	/* 把 NULL 表头转成环形 */
		dclist_init(head);

	dlist_push_head(&head->dlist, node);
	head->count++;

	Assert(head->count > 0);	/* 计数溢出检查 */
}

/*
 * dclist_push_tail
 *		在链表末尾插入一个节点。
 */
static inline void
dclist_push_tail(dclist_head *head, dlist_node *node)
{
	if (head->dlist.head.next == NULL)	/* 把 NULL 表头转成环形 */
		dclist_init(head);

	dlist_push_tail(&head->dlist, node);
	head->count++;

	Assert(head->count > 0);	/* 计数溢出检查 */
}

/*
 * dclist_insert_after
 *		在同一链表中的另一个节点之后插入节点。
 *
 * 注意：'after' 必须是 'head' 的成员。
 */
static inline void
dclist_insert_after(dclist_head *head, dlist_node *after, dlist_node *node)
{
	dlist_member_check(&head->dlist, after);
	Assert(head->count > 0);	/* 至少已经有 1 个 */

	dlist_insert_after(after, node);
	head->count++;

	Assert(head->count > 0);	/* 计数溢出检查 */
}

/*
 * dclist_insert_before
 *		在同一链表中的另一个节点之前插入节点。
 *
 * 注意：'before' 必须是 'head' 的成员。
 */
static inline void
dclist_insert_before(dclist_head *head, dlist_node *before, dlist_node *node)
{
	dlist_member_check(&head->dlist, before);
	Assert(head->count > 0);	/* 至少已经有 1 个 */

	dlist_insert_before(before, node);
	head->count++;

	Assert(head->count > 0);	/* 计数溢出检查 */
}

/*
 * dclist_delete_from
 *		从 'head' 中删除 'node'。
 *
 * 注意：'node' 必须是 'head' 的成员。
 */
static inline void
dclist_delete_from(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	dlist_delete_from(&head->dlist, node);
	head->count--;
}

/*
 * 同 dclist_delete_from()，但还会把 next/prev 设为 NULL，
 * 以表示不在任何链表中。
 */
static inline void
dclist_delete_from_thoroughly(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	dlist_delete_from_thoroughly(&head->dlist, node);
	head->count--;
}

/*
 * dclist_pop_head_node
 *		移除并返回链表的第一个节点（必须存在一个）。
 */
static inline dlist_node *
dclist_pop_head_node(dclist_head *head)
{
	dlist_node *node;

	Assert(head->count > 0);

	node = dlist_pop_head_node(&head->dlist);
	head->count--;
	return node;
}

/*
 * dclist_move_head
 *		把 'node' 从链表中的当前位置移到 'head' 中的
 *		表头位置。
 *
 * 注意：'node' 必须是 'head' 的成员。
 */
static inline void
dclist_move_head(dclist_head *head, dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	dlist_move_head(&head->dlist, node);
}

/*
 * dclist_move_tail
 *		把 'node' 从链表中的当前位置移到 'head' 中的
 *		表尾位置。
 *
 * 注意：'node' 必须是 'head' 的成员。
 */
static inline void
dclist_move_tail(dclist_head *head, dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	dlist_move_tail(&head->dlist, node);
}

/*
 * dclist_has_next
 *		检查 'node' 是否有后继节点。
 *
 * 注意：'node' 必须是 'head' 的成员。
 */
static inline bool
dclist_has_next(const dclist_head *head, const dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	return dlist_has_next(&head->dlist, node);
}

/*
 * dclist_has_prev
 *		检查 'node' 是否有前驱节点。
 *
 * 注意：'node' 必须是 'head' 的成员。
 */
static inline bool
dclist_has_prev(const dclist_head *head, const dlist_node *node)
{
	dlist_member_check(&head->dlist, node);
	Assert(head->count > 0);

	return dlist_has_prev(&head->dlist, node);
}

/*
 * dclist_next_node
 *		返回链表中的下一节点（必须存在一个）。
 */
static inline dlist_node *
dclist_next_node(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	return dlist_next_node(&head->dlist, node);
}

/*
 * dclist_prev_node
 *		返回链表中的前一节点（必须存在一个）。
 */
static inline dlist_node *
dclist_prev_node(dclist_head *head, dlist_node *node)
{
	Assert(head->count > 0);

	return dlist_prev_node(&head->dlist, node);
}

/* 内部辅助函数，取得头元素结构体的地址 */
static inline void *
dclist_head_element_off(dclist_head *head, size_t off)
{
	Assert(!dclist_is_empty(head));

	return (char *) head->dlist.head.next - off;
}

/*
 * dclist_head_node
 *		返回链表中的第一个节点（必须存在一个）。
 */
static inline dlist_node *
dclist_head_node(dclist_head *head)
{
	Assert(head->count > 0);

	return (dlist_node *) dlist_head_element_off(&head->dlist, 0);
}

/* 内部辅助函数，取得尾元素结构体的地址 */
static inline void *
dclist_tail_element_off(dclist_head *head, size_t off)
{
	Assert(!dclist_is_empty(head));

	return (char *) head->dlist.head.prev - off;
}

/*
 * 返回链表中的最后一个节点（必须存在一个）。
 */
static inline dlist_node *
dclist_tail_node(dclist_head *head)
{
	Assert(head->count > 0);

	return (dlist_node *) dlist_tail_element_off(&head->dlist, 0);
}

/*
 * dclist_count
 *		返回 'head' 中存储的条目数。
 */
static inline uint32
dclist_count(const dclist_head *head)
{
	Assert(dlist_is_empty(&head->dlist) == (head->count == 0));

	return head->count;
}

/*
 * 返回 'type' 的外层结构体，其中 'membername' 是
 * 'ptr' 所指的 dlist_node。
 *
 * 用来把 dlist_node * 转回其外层结构体。
 *
 * 注意：这实际上与 dlist_container 相同，因此复用它。
 */
#define dclist_container(type, membername, ptr) \
		dlist_container(type, membername, ptr)

 /*
 * 返回链表中第一个元素的地址。
 *
 * 链表不得为空。
 */
#define dclist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 (type *) dclist_head_element_off(lhead, offsetof(type, membername)))

 /*
 * 返回链表中最后一个元素的地址。
 *
 * 链表不得为空。
 */
#define dclist_tail_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, dlist_node),	\
	 ((type *) dclist_tail_element_off(lhead, offsetof(type, membername))))


/* dclist 的迭代器 */
#define dclist_foreach(iter, lhead) \
	dlist_foreach(iter, &((lhead)->dlist))

#define dclist_foreach_modify(iter, lhead) \
	dlist_foreach_modify(iter, &((lhead)->dlist))

#define dclist_reverse_foreach(iter, lhead) \
	dlist_reverse_foreach(iter, &((lhead)->dlist))

/* 单向链表实现 */

/*
 * 初始化单向链表。
 * 先前状态会被丢弃，不做任何清理。
 */
static inline void
slist_init(slist_head *head)
{
	head->head.next = NULL;
}

/*
 * 链表是否为空？
 */
static inline bool
slist_is_empty(const slist_head *head)
{
	slist_check(head);

	return head->head.next == NULL;
}

/*
 * 在链表开头插入一个节点。
 */
static inline void
slist_push_head(slist_head *head, slist_node *node)
{
	node->next = head->head.next;
	head->head.next = node;

	slist_check(head);
}

/*
 * 在同一链表中的另一个节点之后插入节点。
 */
static inline void
slist_insert_after(slist_node *after, slist_node *node)
{
	node->next = after->next;
	after->next = node;
}

/*
 * 移除并返回链表的第一个节点（必须存在一个）。
 */
static inline slist_node *
slist_pop_head_node(slist_head *head)
{
	slist_node *node;

	Assert(!slist_is_empty(head));
	node = head->head.next;
	head->head.next = node->next;
	slist_check(head);
	return node;
}

/*
 * 检查 'node' 是否有后继节点。
 */
static inline bool
slist_has_next(const slist_head *head, const slist_node *node)
{
	slist_check(head);

	return node->next != NULL;
}

/*
 * 返回链表中的下一节点（必须存在一个）。
 */
static inline slist_node *
slist_next_node(slist_head *head, slist_node *node)
{
	Assert(slist_has_next(head, node));
	return node->next;
}

/* 内部辅助函数，取得头元素结构体的地址 */
static inline void *
slist_head_element_off(slist_head *head, size_t off)
{
	Assert(!slist_is_empty(head));
	return (char *) head->head.next - off;
}

/*
 * 返回链表中的第一个节点（必须存在一个）。
 */
static inline slist_node *
slist_head_node(slist_head *head)
{
	return (slist_node *) slist_head_element_off(head, 0);
}

/*
 * 删除迭代器当前指向的链表元素。
 *
 * 注意：这会修改 iter->cur，因此在当前这轮循环中不要
 * 再使用它。
 */
static inline void
slist_delete_current(slist_mutable_iter *iter)
{
	/*
 * 更新前一元素的前向链接。若迭代位于第一个链表元素，
 * iter->prev 会指向链表表头的 "head" 字段，因此不需要
 * 为这种情况写专门分支。
 */
	iter->prev->next = iter->next;

	/*
 * 把 cur 重置为 prev，这样在 slist_foreach_modify() 前进到
 * 下一个元素之后，prev 仍指向先前有效的链表元素。
 */
	iter->cur = iter->prev;
}

/*
 * 返回 'type' 的外层结构体，其中 'membername' 是
 * 'ptr' 所指的 slist_node。
 *
 * 用来把 slist_node * 转回其外层结构体。
 */
#define slist_container(type, membername, ptr)								\
	(AssertVariableIsOfTypeMacro(ptr, slist_node *),						\
	 AssertVariableIsOfTypeMacro(((type *) NULL)->membername, slist_node),	\
	 ((type *) ((char *) (ptr) - offsetof(type, membername))))

/*
 * 返回链表中第一个元素的地址。
 *
 * 链表不得为空。
 */
#define slist_head_element(type, membername, lhead)							\
	(AssertVariableIsOfTypeMacro(((type *) NULL)->membername, slist_node),	\
	 (type *) slist_head_element_off(lhead, offsetof(type, membername)))

/*
 * 遍历 'lhead' 所指的链表，把状态存在 'iter' 中。
 *
 * 用 iter.cur 访问当前元素。
 *
 * 允许在迭代期间修改链表，但删除迭代器当前节点除外；
 * 若之后还要继续迭代，删除该节点需要小心。（这样做，
 * 同时还删除或插入相邻链表元素，可能行为异常；另外，
 * 若使用者释放了当前节点的存储，继续迭代就不安全。）
 *
 *
 */
#define slist_foreach(iter, lhead)											\
	for (AssertVariableIsOfTypeMacro(iter, slist_iter),						\
		 AssertVariableIsOfTypeMacro(lhead, slist_head *),					\
		 (iter).cur = (lhead)->head.next;									\
		 (iter).cur != NULL;												\
		 (iter).cur = (iter).cur->next)

/*
 * 遍历 'lhead' 所指的链表，把状态存在 'iter' 中。
 *
 * 用 iter.cur 访问当前元素。
 *
 * 迭代期间唯一允许的链表修改，是通过 slist_delete_current()
 * 移除当前节点（*不是* slist_delete()）。插入或删除与当前
 * 节点相邻的节点会行为异常。
 */
#define slist_foreach_modify(iter, lhead)									\
	for (AssertVariableIsOfTypeMacro(iter, slist_mutable_iter),				\
		 AssertVariableIsOfTypeMacro(lhead, slist_head *),					\
		 (iter).prev = &(lhead)->head,										\
		 (iter).cur = (iter).prev->next,									\
		 (iter).next = (iter).cur ? (iter).cur->next : NULL;				\
		 (iter).cur != NULL;												\
		 (iter).prev = (iter).cur,											\
		 (iter).cur = (iter).next,											\
		 (iter).next = (iter).next ? (iter).next->next : NULL)

#endif							/* ILIST_H */
