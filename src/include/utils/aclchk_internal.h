/*-------------------------------------------------------------------------
 *
 * aclchk_internal.h
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/aclchk_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACLCHK_INTERNAL_H
#define ACLCHK_INTERNAL_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

/*
 * 一条 Grant/Revoke 语句的信息，内部格式：对象
 * 与被授予者名已转为 Oid，特权列表为 AclMode
 * 位掩码。若 privileges 为 ACL_NO_RIGHTS（0）且
 * all_privs 为真，则内部设为合适的
 * ACL_ALL_RIGHTS_*（依对象类型；注意会改
 * InternalGrant！）
 *
 * 注意：all_privs 与 privileges 仅表示对象级特权。
 * 还可能有列级特权规格，在 col_privs 中表示（未
 * 变换的 AccessPriv 节点列表）。列特权仅对
 * objtype OBJECT_TABLE 有效。
 */
typedef struct
{
	bool		is_grant;
	ObjectType	objtype;
	List	   *objects;
	bool		all_privs;
	AclMode		privileges;
	List	   *col_privs;
	List	   *grantees;
	bool		grant_option;
	DropBehavior behavior;
} InternalGrant;


#endif							/* ACLCHK_INTERNAL_H */
