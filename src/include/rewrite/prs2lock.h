/*-------------------------------------------------------------------------
 *
 * prs2lock.h
 *	  POSTGRES 规则系统 II 的数据结构（仅改写规则）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/prs2lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRS2LOCK_H
#define PRS2LOCK_H

#include "access/attnum.h"
#include "nodes/pg_list.h"

/*
 * RewriteRule -
 *	  保存改写规则信息
 *
 */
typedef struct RewriteRule
{
	Oid			ruleId;
	CmdType		event;
	Node	   *qual;
	List	   *actions;
	char		enabled;
	bool		isInstead;
} RewriteRule;

/*
 * RuleLock -
 *	  适用于某关系的全部规则。虽只剩改写规则系统，
 *	  且这些并非真正的"锁"，名称因历史原因保留。
 *	  名称因历史原因保留。
 */
typedef struct RuleLock
{
	int			numLocks;
	RewriteRule **rules;
} RuleLock;

#endif							/* PRS2LOCK_H */
