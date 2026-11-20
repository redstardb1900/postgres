/*-------------------------------------------------------------------------
 *
 * proclist_types.h
 *		pgprocno 的双向链表
 *
 * 操作这些类型的函数见 proclist.h。
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist_types.h
 *-------------------------------------------------------------------------
 */

#ifndef PROCLIST_TYPES_H
#define PROCLIST_TYPES_H

/*
 * 进程双向链表中的节点。链接字段存放
 * 下一与上一进程的从 0 计的 PGPROC 下标，或
 * 在末节点的 next-link 与首节点的 prev-link 中为
 * INVALID_PGPROCNO。当前不在任何链表中的节点
 * 应有 next == prev == 0；这不是在链表中的节点
 * 可能有的状态，因为我们禁止成环。
 */
typedef struct proclist_node
{
	int			next;			/* 下一 PGPROC 的 pgprocno */
	int			prev;			/* 上一 PGPROC 的 pgprocno */
} proclist_node;

/*
 * 以 pgprocno 标识的 PGPROCs 双向链表表头。
 * 空表表示为 head == tail == INVALID_PGPROCNO。
 */
typedef struct proclist_head
{
	int			head;			/* 头 PGPROC 的 pgprocno */
	int			tail;			/* 尾 PGPROC 的 pgprocno */
} proclist_head;

/*
 * 允许在迭代中做某些修改的链表迭代器。
 */
typedef struct proclist_mutable_iter
{
	int			cur;			/* 当前 PGPROC 的 pgprocno */
	int			next;			/* 下一 PGPROC 的 pgprocno */
} proclist_mutable_iter;

#endif							/* PROCLIST_TYPES_H */
