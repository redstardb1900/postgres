/*-------------------------------------------------------------------------
 *
 * proclist.h
 *		pgprocno 双向链表上的操作
 *
 * 该接口类似 ilist.h 的 dlist，但用 pgprocno 而非
 * 指针。这允许把 proclist_head 映射到不同地址，于
 * 不同后端中。
 *
 * 这些函数所操作的结构见 proclist_types.h。分开是为
 * 打破与 proc.h 的头文件依赖环。
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist.h
 *-------------------------------------------------------------------------
 */
#ifndef PROCLIST_H
#define PROCLIST_H

#include "storage/proc.h"
#include "storage/proclist_types.h"

/*
 * 初始化 proclist。
 */
static inline void
proclist_init(proclist_head *list)
{
	list->head = list->tail = INVALID_PGPROCNO;
}

/*
 * 链表为空？
 */
static inline bool
proclist_is_empty(const proclist_head *list)
{
	return list->head == INVALID_PGPROCNO;
}

/*
 * 给定 procno 及 proclist_node 字段在 PGPROC 结构内的
 * 偏移，取得 PGPROC 内该节点的指针。
 */
static inline proclist_node *
proclist_node_get(int procno, size_t node_offset)
{
	char	   *entry = (char *) GetPGProcByNumber(procno);

	return (proclist_node *) (entry + node_offset);
}

/*
 * 把进程插入链表开头。
 */
static inline void
proclist_push_head_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next == 0 && node->prev == 0);

	if (list->head == INVALID_PGPROCNO)
	{
		Assert(list->tail == INVALID_PGPROCNO);
		node->next = node->prev = INVALID_PGPROCNO;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->tail != INVALID_PGPROCNO);
		Assert(list->head != procno);
		Assert(list->tail != procno);
		node->next = list->head;
		proclist_node_get(node->next, node_offset)->prev = procno;
		node->prev = INVALID_PGPROCNO;
		list->head = procno;
	}
}

/*
 * 把进程插入链表末尾。
 */
static inline void
proclist_push_tail_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next == 0 && node->prev == 0);

	if (list->tail == INVALID_PGPROCNO)
	{
		Assert(list->head == INVALID_PGPROCNO);
		node->next = node->prev = INVALID_PGPROCNO;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->head != INVALID_PGPROCNO);
		Assert(list->head != procno);
		Assert(list->tail != procno);
		node->prev = list->tail;
		proclist_node_get(node->prev, node_offset)->next = procno;
		node->next = INVALID_PGPROCNO;
		list->tail = procno;
	}
}

/*
 * 从链表删除进程——它必须在链表中！
 */
static inline void
proclist_delete_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next != 0 || node->prev != 0);

	if (node->prev == INVALID_PGPROCNO)
	{
		Assert(list->head == procno);
		list->head = node->next;
	}
	else
		proclist_node_get(node->prev, node_offset)->next = node->next;

	if (node->next == INVALID_PGPROCNO)
	{
		Assert(list->tail == procno);
		list->tail = node->prev;
	}
	else
		proclist_node_get(node->next, node_offset)->prev = node->prev;

	node->next = node->prev = 0;
}

/*
 * 检查进程当前是否在某链表中。必须已知该
 * 进程不在任何使用同一 proclist_node 的 _other_
 * proclist 中，故只可能在本链表或都不在。
 */
static inline bool
proclist_contains_offset(const proclist_head *list, int procno,
						 size_t node_offset)
{
	const proclist_node *node = proclist_node_get(procno, node_offset);

	/* 若不在任何链表中，则肯定不在本链表。 */
	if (node->prev == 0 && node->next == 0)
		return false;

	/*
	 * 它必须在本链表中。理想地，在启用断言的构建里
	 * 我们会验证这一点。但本函数通常在持有
	 * 自旋锁时使用，遍历整条链表不可接受。不过，
	 * 当节点是链表头或尾时，可在 O(1) 时间内校验，
	 * 这值得做，因为实践中这往往
	 * 足以抓住错误。
	 */
	Assert(node->prev != INVALID_PGPROCNO || list->head == procno);
	Assert(node->next != INVALID_PGPROCNO || list->tail == procno);

	return true;
}

/*
 * 移除并返回链表的第一个进程（必须有一个）。
 */
static inline PGPROC *
proclist_pop_head_node_offset(proclist_head *list, size_t node_offset)
{
	PGPROC	   *proc;

	Assert(!proclist_is_empty(list));
	proc = GetPGProcByNumber(list->head);
	proclist_delete_offset(list, list->head, node_offset);
	return proc;
}

/*
 * 辅助宏，避免重复 offsetof(PGPROC, <member>)。
 * 'link_member' 是 PGPROC 中 proclist_node 成员的名字。
 */
#define proclist_delete(list, procno, link_member) \
	proclist_delete_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_head(list, procno, link_member) \
	proclist_push_head_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_tail(list, procno, link_member) \
	proclist_push_tail_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_pop_head_node(list, link_member) \
	proclist_pop_head_node_offset((list), offsetof(PGPROC, link_member))
#define proclist_contains(list, procno, link_member) \
	proclist_contains_offset((list), (procno), offsetof(PGPROC, link_member))

/*
 * 遍历 'lhead' 所指链表，把当前位置存入
 * 'iter'。'link_member' 是 PGPROC 中某 proclist_node 成员的
 * 名字。用 iter.cur 访问当前位置。
 *
 * 迭代期间唯一允许的链表修改是删除当前节点，调用
 * proclist_delete(list, iter.cur, node_offset)。
 */
#define proclist_foreach_modify(iter, lhead, link_member)					\
	for (AssertVariableIsOfTypeMacro(iter, proclist_mutable_iter),			\
		 AssertVariableIsOfTypeMacro(lhead, proclist_head *),				\
		 (iter).cur = (lhead)->head,										\
		 (iter).next = (iter).cur == INVALID_PGPROCNO ? INVALID_PGPROCNO :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next;		\
		 (iter).cur != INVALID_PGPROCNO;									\
		 (iter).cur = (iter).next,											\
		 (iter).next = (iter).cur == INVALID_PGPROCNO ? INVALID_PGPROCNO :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next)

#endif							/* PROCLIST_H */
