/* -------------------------------------------------------------------------
 *
 * rowsecurity.h
 *
 *	  rewrite/rowsecurity.c 的原型，以及在 relcache 中管理
 *	  关系行安全性策略的结构。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef ROWSECURITY_H
#define ROWSECURITY_H

#include "nodes/parsenodes.h"
#include "utils/array.h"
#include "utils/relcache.h"

typedef struct RowSecurityPolicy
{
	char	   *policy_name;	/* 策略名称 */
	char		polcmd;			/* 策略适用的命令类型 */
	ArrayType  *roles;			/* 策略适用的角色数组 */
	bool		permissive;		/* 限制性或宽松性策略 */
	Expr	   *qual;			/* 过滤行的表达式 */
	Expr	   *with_check_qual;	/* 限制允许行的表达式 */
	bool		hassublinks;	/* 若任一表达式含 sublink */
} RowSecurityPolicy;

typedef struct RowSecurityDesc
{
	MemoryContext rscxt;		/* 行安全性内存上下文 */
	List	   *policies;		/* 行安全性策略列表 */
} RowSecurityDesc;

typedef List *(*row_security_policy_hook_type) (CmdType cmdtype,
												Relation relation);

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook_permissive;

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook_restrictive;

extern void get_row_security_policies(Query *root,
									  RangeTblEntry *rte, int rt_index,
									  List **securityQuals, List **withCheckOptions,
									  bool *hasRowSecurity, bool *hasSubLinks);

#endif							/* ROWSECURITY_H */
