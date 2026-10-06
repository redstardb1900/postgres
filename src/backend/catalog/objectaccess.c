/* -------------------------------------------------------------------------
 *
 * objectaccess.c
 *		各类事件上 object_access_hook 的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"

/*
 * 对象访问钩子。用作安全与日志插件的
 * 基础设施。
 */
object_access_hook_type object_access_hook = NULL;
object_access_hook_type_str object_access_hook_str = NULL;


/*
 * RunObjectPostCreateHook
 *
 * 基于对象 ID 的 OAT_POST_CREATE 事件钩子入口点
 */
void
RunObjectPostCreateHook(Oid classId, Oid objectId, int subId,
						bool is_internal)
{
	ObjectAccessPostCreate pc_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	memset(&pc_arg, 0, sizeof(ObjectAccessPostCreate));
	pc_arg.is_internal = is_internal;

	(*object_access_hook) (OAT_POST_CREATE,
						   classId, objectId, subId,
						   (void *) &pc_arg);
}

/*
 * RunObjectDropHook
 *
 * 基于对象 ID 的 OAT_DROP 事件钩子入口点
 */
void
RunObjectDropHook(Oid classId, Oid objectId, int subId,
				  int dropflags)
{
	ObjectAccessDrop drop_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	memset(&drop_arg, 0, sizeof(ObjectAccessDrop));
	drop_arg.dropflags = dropflags;

	(*object_access_hook) (OAT_DROP,
						   classId, objectId, subId,
						   (void *) &drop_arg);
}

/*
 * RunObjectTruncateHook
 *
 * 基于对象 ID 的 OAT_TRUNCATE 事件钩子入口点
 */
void
RunObjectTruncateHook(Oid objectId)
{
	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	(*object_access_hook) (OAT_TRUNCATE,
						   RelationRelationId, objectId, 0,
						   NULL);
}

/*
 * RunObjectPostAlterHook
 *
 * 基于对象 ID 的 OAT_POST_ALTER 事件钩子入口点
 */
void
RunObjectPostAlterHook(Oid classId, Oid objectId, int subId,
					   Oid auxiliaryId, bool is_internal)
{
	ObjectAccessPostAlter pa_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	memset(&pa_arg, 0, sizeof(ObjectAccessPostAlter));
	pa_arg.auxiliary_id = auxiliaryId;
	pa_arg.is_internal = is_internal;

	(*object_access_hook) (OAT_POST_ALTER,
						   classId, objectId, subId,
						   (void *) &pa_arg);
}

/*
 * RunNamespaceSearchHook
 *
 * 基于对象 ID 的 OAT_NAMESPACE_SEARCH 事件钩子入口点
 */
bool
RunNamespaceSearchHook(Oid objectId, bool ereport_on_violation)
{
	ObjectAccessNamespaceSearch ns_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	memset(&ns_arg, 0, sizeof(ObjectAccessNamespaceSearch));
	ns_arg.ereport_on_violation = ereport_on_violation;
	ns_arg.result = true;

	(*object_access_hook) (OAT_NAMESPACE_SEARCH,
						   NamespaceRelationId, objectId, 0,
						   (void *) &ns_arg);

	return ns_arg.result;
}

/*
 * RunFunctionExecuteHook
 *
 * 基于对象 ID 的 OAT_FUNCTION_EXECUTE 事件钩子入口点
 */
void
RunFunctionExecuteHook(Oid objectId)
{
	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook != NULL);

	(*object_access_hook) (OAT_FUNCTION_EXECUTE,
						   ProcedureRelationId, objectId, 0,
						   NULL);
}

/* 字符串版本 */


/*
 * RunObjectPostCreateHookStr
 *
 * 基于对象名的 OAT_POST_CREATE 事件钩子入口点
 */
void
RunObjectPostCreateHookStr(Oid classId, const char *objectName, int subId,
						   bool is_internal)
{
	ObjectAccessPostCreate pc_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	memset(&pc_arg, 0, sizeof(ObjectAccessPostCreate));
	pc_arg.is_internal = is_internal;

	(*object_access_hook_str) (OAT_POST_CREATE,
							   classId, objectName, subId,
							   (void *) &pc_arg);
}

/*
 * RunObjectDropHookStr
 *
 * 基于对象名的 OAT_DROP 事件钩子入口点
 */
void
RunObjectDropHookStr(Oid classId, const char *objectName, int subId,
					 int dropflags)
{
	ObjectAccessDrop drop_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	memset(&drop_arg, 0, sizeof(ObjectAccessDrop));
	drop_arg.dropflags = dropflags;

	(*object_access_hook_str) (OAT_DROP,
							   classId, objectName, subId,
							   (void *) &drop_arg);
}

/*
 * RunObjectTruncateHookStr
 *
 * 基于对象名的 OAT_TRUNCATE 事件钩子入口点
 */
void
RunObjectTruncateHookStr(const char *objectName)
{
	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	(*object_access_hook_str) (OAT_TRUNCATE,
							   RelationRelationId, objectName, 0,
							   NULL);
}

/*
 * RunObjectPostAlterHookStr
 *
 * 基于对象名的 OAT_POST_ALTER 事件钩子入口点
 */
void
RunObjectPostAlterHookStr(Oid classId, const char *objectName, int subId,
						  Oid auxiliaryId, bool is_internal)
{
	ObjectAccessPostAlter pa_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	memset(&pa_arg, 0, sizeof(ObjectAccessPostAlter));
	pa_arg.auxiliary_id = auxiliaryId;
	pa_arg.is_internal = is_internal;

	(*object_access_hook_str) (OAT_POST_ALTER,
							   classId, objectName, subId,
							   (void *) &pa_arg);
}

/*
 * RunNamespaceSearchHookStr
 *
 * 基于对象名的 OAT_NAMESPACE_SEARCH 事件钩子入口点
 */
bool
RunNamespaceSearchHookStr(const char *objectName, bool ereport_on_violation)
{
	ObjectAccessNamespaceSearch ns_arg;

	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	memset(&ns_arg, 0, sizeof(ObjectAccessNamespaceSearch));
	ns_arg.ereport_on_violation = ereport_on_violation;
	ns_arg.result = true;

	(*object_access_hook_str) (OAT_NAMESPACE_SEARCH,
							   NamespaceRelationId, objectName, 0,
							   (void *) &ns_arg);

	return ns_arg.result;
}

/*
 * RunFunctionExecuteHookStr
 *
 * 基于对象名的 OAT_FUNCTION_EXECUTE 事件钩子入口点
 */
void
RunFunctionExecuteHookStr(const char *objectName)
{
	/* 调用者本应检查，但以防万一... */
	Assert(object_access_hook_str != NULL);

	(*object_access_hook_str) (OAT_FUNCTION_EXECUTE,
							   ProcedureRelationId, objectName, 0,
							   NULL);
}
