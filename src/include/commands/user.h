/*-------------------------------------------------------------------------
 *
 * user.h
 *	操纵角色（昔称用户）的命令。
 *
 *
 * src/include/commands/user.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef USER_H
#define USER_H

#include "catalog/objectaddress.h"
#include "libpq/crypt.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/guc.h"

/* GUC */
extern PGDLLIMPORT int Password_encryption; /* PasswordType 枚举的取值 */
extern PGDLLIMPORT char *createrole_self_grant;

/* 在 CreateRole()/AlterRole() 中检查口令的钩子 */
typedef void (*check_password_hook_type) (const char *username, const char *shadow_pass, PasswordType password_type, Datum validuntil_time, bool validuntil_null);

extern PGDLLIMPORT check_password_hook_type check_password_hook;

extern Oid	CreateRole(ParseState *pstate, CreateRoleStmt *stmt);
extern Oid	AlterRole(ParseState *pstate, AlterRoleStmt *stmt);
extern Oid	AlterRoleSet(AlterRoleSetStmt *stmt);
extern void DropRole(DropRoleStmt *stmt);
extern void GrantRole(ParseState *pstate, GrantRoleStmt *stmt);
extern ObjectAddress RenameRole(const char *oldname, const char *newname);
extern void DropOwnedObjects(DropOwnedStmt *stmt);
extern void ReassignOwnedObjects(ReassignOwnedStmt *stmt);
extern List *roleSpecsToIds(List *memberNames);

extern bool check_createrole_self_grant(char **newval, void **extra,
										GucSource source);
extern void assign_createrole_self_grant(const char *newval, void *extra);

#endif							/* USER_H */
