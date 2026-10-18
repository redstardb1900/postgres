/*-------------------------------------------------------------------------
 *
 * dropcmds.c
 *	  处理各种 "DROP" 操作
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dropcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void does_not_exist_skipping(ObjectType objtype,
									Node *object);
static bool owningrel_does_not_exist_skipping(List *object,
											  const char **msg, char **name);
static bool schema_does_not_exist_skipping(List *object,
										   const char **msg, char **name);
static bool type_in_list_does_not_exist_skipping(List *typenames,
												 const char **msg, char **name);


/*
 * 删除一个或多个对象。
 *
 * 我们目前并不在这里处理所有对象类型。例如，关系
 * 需要特殊处理，因为（例如）索引有额外的
 * 锁要求。
 *
 * 我们先查找所有对象，然后在一次 performMultipleDeletions()
 * 调用中删除它们。若对象之间存在依赖，这可避免
 * 不必要的 DROP RESTRICT 错误。
 */
void
RemoveObjects(DropStmt *stmt)
{
	ObjectAddresses *objects;
	ListCell   *cell1;

	objects = new_object_addresses();

	foreach(cell1, stmt->objects)
	{
		ObjectAddress address;
		Node	   *object = lfirst(cell1);
		Relation	relation = NULL;
		Oid			namespaceId;

		/* 获取对象的 ObjectAddress。 */
		address = get_object_address(stmt->removeType,
									 object,
									 &relation,
									 AccessExclusiveLock,
									 stmt->missing_ok);

		/*
		 * 若未找到提供的对象就发出 NOTICE。注意这只与
		 * missing_ok 情形相关，因为否则
		 * get_object_address 早已抛出错误。
		 */
		if (!OidIsValid(address.objectId))
		{
			Assert(stmt->missing_ok);
			does_not_exist_skipping(stmt->removeType, object);
			continue;
		}

		/*
		 * 虽然 COMMENT ON FUNCTION、SECURITY LABEL ON FUNCTION 等
		 * 乐意像对待其他函数一样操作聚合，但我们历来
		 * 不允许 DROP FUNCTION 这样做。
		 */
		if (stmt->removeType == OBJECT_FUNCTION)
		{
			if (get_func_prokind(address.objectId) == PROKIND_AGGREGATE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is an aggregate function",
								NameListToString(castNode(ObjectWithArgs, object)->objname)),
						 errhint("Use DROP AGGREGATE to drop aggregate functions.")));
		}

		/* 检查权限。 */
		namespaceId = get_object_namespace(&address);
		if (!OidIsValid(namespaceId) ||
			!object_ownercheck(NamespaceRelationId, namespaceId, GetUserId()))
			check_object_ownership(GetUserId(), stmt->removeType, address,
								   object, relation);

		/*
		 * 若本事务中访问过临时命名空间，
		 * 就做记录。
		 */
		if (OidIsValid(namespaceId) && isTempNamespace(namespaceId))
			MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

		/* 释放任何 relcache 引用计数，但把锁保留到提交。 */
		if (relation)
			table_close(relation, NoLock);

		add_exact_object_address(&address, objects);
	}

	/* 这里我们真正删除它们。 */
	performMultipleDeletions(objects, stmt->behavior, 0);

	free_object_addresses(objects);
}

/*
 * owningrel_does_not_exist_skipping
 *		RemoveObjects 的子例程
 *
 * 在确定规则或触发器的规格返回"指定对象不存在"后，
 * 测试其所属关系及其模式是否存在；若存在，返回 false ——
 * 缺少的是触发器或规则本身。若所属关系或其模式
 * 不存在，填充错误消息格式字符串与名称，
 * 并返回 true。
 */
static bool
owningrel_does_not_exist_skipping(List *object, const char **msg, char **name)
{
	List	   *parent_object;
	RangeVar   *parent_rel;

	parent_object = list_copy_head(object, list_length(object) - 1);

	if (schema_does_not_exist_skipping(parent_object, msg, name))
		return true;

	parent_rel = makeRangeVarFromNameList(parent_object);

	if (!OidIsValid(RangeVarGetRelid(parent_rel, NoLock, true)))
	{
		*msg = gettext_noop("relation \"%s\" does not exist, skipping");
		*name = NameListToString(parent_object);

		return true;
	}

	return false;
}

/*
 * schema_does_not_exist_skipping
 *		RemoveObjects 的子例程
 *
 * 在确定某个可带模式限定的对象的规格
 * 指向不存在的对象后，测试指定模式是否存在。
 * 若未指定模式，或模式确实存在，返回 false ——
 * 缺少的是对象本身。若指定的模式不存在，
 * 填充错误消息格式字符串和指定模式名，
 * 并返回 true。
 */
static bool
schema_does_not_exist_skipping(List *object, const char **msg, char **name)
{
	RangeVar   *rel;

	rel = makeRangeVarFromNameList(object);

	if (rel->schemaname != NULL &&
		!OidIsValid(LookupNamespaceNoError(rel->schemaname)))
	{
		*msg = gettext_noop("schema \"%s\" does not exist, skipping");
		*name = rel->schemaname;

		return true;
	}

	return false;
}

/*
 * type_in_list_does_not_exist_skipping
 *		RemoveObjects 的子例程
 *
 * 在确定函数、强制转换、聚合或操作符的规格返回
 * "指定对象不存在"后，测试涉及的数据类型及其
 * 模式是否存在；若存在，返回 false —— 缺少的是
 * 原始对象本身。若数据类型或模式不存在，
 * 填充错误消息格式字符串和缺少的名称，
 * 并返回 true。
 *
 * 第一个参数是 TypeNames 列表。
 */
static bool
type_in_list_does_not_exist_skipping(List *typenames, const char **msg,
									 char **name)
{
	ListCell   *l;

	foreach(l, typenames)
	{
		TypeName   *typeName = lfirst_node(TypeName, l);

		if (typeName != NULL)
		{
			if (!OidIsValid(LookupTypeNameOid(NULL, typeName, true)))
			{
				/* 类型不存在，试着找原因 */
				if (schema_does_not_exist_skipping(typeName->names, msg, name))
					return true;

				*msg = gettext_noop("type \"%s\" does not exist, skipping");
				*name = TypeNameToString(typeName);

				return true;
			}
		}
	}

	return false;
}

/*
 * does_not_exist_skipping
 *		RemoveObjects 的子例程
 *
 * 生成一条 NOTICE，说明未找到指定名称的对象且已跳过
 * 它。这只与使用 "IF EXISTS" 时相关；否则 RemoveObjects
 * 中的 get_object_address() 早已抛出 ERROR。
 */
static void
does_not_exist_skipping(ObjectType objtype, Node *object)
{
	const char *msg = NULL;
	char	   *name = NULL;
	char	   *args = NULL;

	switch (objtype)
	{
		case OBJECT_ACCESS_METHOD:
			msg = gettext_noop("access method \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			{
				TypeName   *typ = castNode(TypeName, object);

				if (!schema_does_not_exist_skipping(typ->names, &msg, &name))
				{
					msg = gettext_noop("type \"%s\" does not exist, skipping");
					name = TypeNameToString(typ);
				}
			}
			break;
		case OBJECT_COLLATION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("collation \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_CONVERSION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("conversion \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_SCHEMA:
			msg = gettext_noop("schema \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_STATISTIC_EXT:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("statistics object \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSPARSER:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search parser \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSDICTIONARY:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search dictionary \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSTEMPLATE:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search template \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSCONFIGURATION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search configuration \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_EXTENSION:
			msg = gettext_noop("extension \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_FUNCTION:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("function %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_PROCEDURE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("procedure %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_ROUTINE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("routine %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_AGGREGATE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("aggregate %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_OPERATOR:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("operator %s does not exist, skipping");
					name = NameListToString(owa->objname);
				}
				break;
			}
		case OBJECT_LANGUAGE:
			msg = gettext_noop("language \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_CAST:
			{
				if (!type_in_list_does_not_exist_skipping(list_make1(linitial(castNode(List, object))), &msg, &name) &&
					!type_in_list_does_not_exist_skipping(list_make1(lsecond(castNode(List, object))), &msg, &name))
				{
					/* XXX 加引号还是不加引号？ */
					msg = gettext_noop("cast from type %s to type %s does not exist, skipping");
					name = TypeNameToString(linitial_node(TypeName, castNode(List, object)));
					args = TypeNameToString(lsecond_node(TypeName, castNode(List, object)));
				}
			}
			break;
		case OBJECT_TRANSFORM:
			if (!type_in_list_does_not_exist_skipping(list_make1(linitial(castNode(List, object))), &msg, &name))
			{
				msg = gettext_noop("transform for type %s language \"%s\" does not exist, skipping");
				name = TypeNameToString(linitial_node(TypeName, castNode(List, object)));
				args = strVal(lsecond(castNode(List, object)));
			}
			break;
		case OBJECT_TRIGGER:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("trigger \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_POLICY:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("policy \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_EVENT_TRIGGER:
			msg = gettext_noop("event trigger \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_RULE:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("rule \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_FDW:
			msg = gettext_noop("foreign-data wrapper \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_FOREIGN_SERVER:
			msg = gettext_noop("server \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_OPCLASS:
			{
				List	   *opcname = list_copy_tail(castNode(List, object), 1);

				if (!schema_does_not_exist_skipping(opcname, &msg, &name))
				{
					msg = gettext_noop("operator class \"%s\" does not exist for access method \"%s\", skipping");
					name = NameListToString(opcname);
					args = strVal(linitial(castNode(List, object)));
				}
			}
			break;
		case OBJECT_OPFAMILY:
			{
				List	   *opfname = list_copy_tail(castNode(List, object), 1);

				if (!schema_does_not_exist_skipping(opfname, &msg, &name))
				{
					msg = gettext_noop("operator family \"%s\" does not exist for access method \"%s\", skipping");
					name = NameListToString(opfname);
					args = strVal(linitial(castNode(List, object)));
				}
			}
			break;
		case OBJECT_PUBLICATION:
			msg = gettext_noop("publication \"%s\" does not exist, skipping");
			name = strVal(object);
			break;

		case OBJECT_COLUMN:
		case OBJECT_DATABASE:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_ROLE:
		case OBJECT_SEQUENCE:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLE:
		case OBJECT_TABLESPACE:
		case OBJECT_VIEW:

			/*
			 * 这些在其他地方处理，所以若有人走到这里，
			 * 代码可能有问题或应该重新审视。
			 */
			elog(ERROR, "unsupported object type: %d", (int) objtype);
			break;

		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_LARGEOBJECT:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_USER_MAPPING:
			/* 这些目前未使用或不需要。 */
			elog(ERROR, "unsupported object type: %d", (int) objtype);
			break;

			/* 没有 default，让编译器对缺少的 case 发出警告 */
	}
	if (!msg)
		elog(ERROR, "unrecognized object type: %d", (int) objtype);

	if (!args)
		ereport(NOTICE, (errmsg(msg, name)));
	else
		ereport(NOTICE, (errmsg(msg, name, args)));
}
