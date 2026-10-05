/*-------------------------------------------------------------------------
 *
 * extensible.c
 *	  对可扩展节点类型的支持
 *
 * 可加载模块可以用本文件中的例程定义实际上的
 * 新节点类型。所有这类节点都标为 T_ExtensibleNode，
 * 并以 extnodename 字段区分具体类型。用
 * RegisterExtensibleNodeMethods 注册一种新的可扩展节点，
 * 用 GetExtensibleNodeMethods 取得先前已经
 * 注册的可扩展节点类型的信息。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/extensible.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "utils/hsearch.h"

static HTAB *extensible_node_methods = NULL;
static HTAB *custom_scan_methods = NULL;

typedef struct
{
	char		extnodename[EXTNODENAME_MAX_LEN];
	const void *extnodemethods;
} ExtensibleNodeEntry;

/*
 * 注册新回调结构的内部函数
 */
static void
RegisterExtensibleNodeEntry(HTAB **p_htable, const char *htable_label,
							const char *extnodename,
							const void *extnodemethods)
{
	ExtensibleNodeEntry *entry;
	bool		found;

	if (*p_htable == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = EXTNODENAME_MAX_LEN;
		ctl.entrysize = sizeof(ExtensibleNodeEntry);

		*p_htable = hash_create(htable_label, 100, &ctl,
								HASH_ELEM | HASH_STRINGS);
	}

	if (strlen(extnodename) >= EXTNODENAME_MAX_LEN)
		elog(ERROR, "extensible node name is too long");

	entry = (ExtensibleNodeEntry *) hash_search(*p_htable,
												extnodename,
												HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extensible node type \"%s\" already exists",
						extnodename)));

	entry->extnodemethods = extnodemethods;
}

/*
 * 注册一种新的可扩展节点类型。
 */
void
RegisterExtensibleNodeMethods(const ExtensibleNodeMethods *methods)
{
	RegisterExtensibleNodeEntry(&extensible_node_methods,
								"Extensible Node Methods",
								methods->extnodename,
								methods);
}

/*
 * 注册一种新的自定义扫描节点
 */
void
RegisterCustomScanMethods(const CustomScanMethods *methods)
{
	RegisterExtensibleNodeEntry(&custom_scan_methods,
								"Custom Scan Methods",
								methods->CustomName,
								methods);
}

/*
 * 按给定标识符取得 ExtensibleNodeEntry 的内部例程
 */
static const void *
GetExtensibleNodeEntry(HTAB *htable, const char *extnodename, bool missing_ok)
{
	ExtensibleNodeEntry *entry = NULL;

	if (htable != NULL)
		entry = (ExtensibleNodeEntry *) hash_search(htable,
													extnodename,
													HASH_FIND, NULL);
	if (!entry)
	{
		if (missing_ok)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("ExtensibleNodeMethods \"%s\" was not registered",
						extnodename)));
	}

	return entry->extnodemethods;
}

/*
 * 取得给定可扩展节点类型的方法。
 */
const ExtensibleNodeMethods *
GetExtensibleNodeMethods(const char *extnodename, bool missing_ok)
{
	return (const ExtensibleNodeMethods *)
		GetExtensibleNodeEntry(extensible_node_methods,
							   extnodename,
							   missing_ok);
}

/*
 * 按给定名字取得 CustomScanMethods 的方法
 */
const CustomScanMethods *
GetCustomScanMethods(const char *CustomName, bool missing_ok)
{
	return (const CustomScanMethods *)
		GetExtensibleNodeEntry(custom_scan_methods,
							   CustomName,
							   missing_ok);
}
