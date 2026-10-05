/*-------------------------------------------------------------------------
 *
 * ilist.c
 *	  为集成/内联的双向链表与单向链表提供支持
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/lib/ilist.c
 *
 * 说明
 *	  本文件只收录那些太大、不适合
 *	  内联的函数。大部分内容见 ilist.h。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/ilist.h"

/*
 * 从列表中删除 'node'。
 *
 * 不允许删除不在列表 'head' 中的 'node'
 *
 * 注意：这是 O(n)；考虑改用 slist_delete_current()。
 */
void
slist_delete(slist_head *head, const slist_node *node)
{
	slist_node *last = &head->head;
	slist_node *cur;
	bool		found PG_USED_FOR_ASSERTS_ONLY = false;

	while ((cur = last->next) != NULL)
	{
		if (cur == node)
		{
			last->next = cur->next;
#ifdef USE_ASSERT_CHECKING
			found = true;
#endif
			break;
		}
		last = cur;
	}
	Assert(found);

	slist_check(head);
}

#ifdef ILIST_DEBUG
/*
 * dlist_member_check
 *		验证 'node' 是 'head' 的成员
 */
void
dlist_member_check(const dlist_head *head, const dlist_node *node)
{
	const dlist_node *cur;

	/* 因使用 const 而展开写循环 */
	for (cur = head->head.next; cur != &head->head; cur = cur->next)
	{
		if (cur == node)
			return;
	}
	elog(ERROR, "double linked list member check failure");
}

/*
 * 验证双向链表的完整性
 */
void
dlist_check(const dlist_head *head)
{
	dlist_node *cur;

	if (head == NULL)
		elog(ERROR, "doubly linked list head address is NULL");

	if (head->head.next == NULL && head->head.prev == NULL)
		return;					/* 正常，已初始化为零 */

	/* 向前迭代 */
	for (cur = head->head.next; cur != &head->head; cur = cur->next)
	{
		if (cur == NULL ||
			cur->next == NULL ||
			cur->prev == NULL ||
			cur->prev->next != cur ||
			cur->next->prev != cur)
			elog(ERROR, "doubly linked list is corrupted");
	}

	/* 向后迭代 */
	for (cur = head->head.prev; cur != &head->head; cur = cur->prev)
	{
		if (cur == NULL ||
			cur->next == NULL ||
			cur->prev == NULL ||
			cur->prev->next != cur ||
			cur->next->prev != cur)
			elog(ERROR, "doubly linked list is corrupted");
	}
}

/*
 * 验证单向链表的完整性
 */
void
slist_check(const slist_head *head)
{
	slist_node *cur;

	if (head == NULL)
		elog(ERROR, "singly linked list head address is NULL");

	/*
	 * 对单向链表能测的不多，除了它最终会结束，
	 * 也就是没有引入环或类似问题
	 */
	for (cur = head->head.next; cur != NULL; cur = cur->next)
		;
}

#endif							/* ILIST_DEBUG */
