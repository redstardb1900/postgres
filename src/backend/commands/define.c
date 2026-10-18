/*-------------------------------------------------------------------------
 *
 * define.c
 *	  各种对象创建的辅助例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/define.c
 *
 * DESCRIPTION
 *	  "DefineFoo" 例程取出解析树并挑选出
 *	  适当的参数/标志，把结果传给相应的
 *	  "FooDefine" 例程（位于 src/catalog），
 *	  由后者完成真正的目录修改。这些例程
 *	  还会校验用户执行该命令的权限。
 *
 * NOTES
 *	  这些东西必须按如下顺序定义和提交：
 *		"create function"：
 *			输入/输出、recv/send 过程
 *		"create type"：
 *			类型
 *		"create operator"：
 *			操作符
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "utils/builtins.h"

/*
 * 从 DefElem 中提取字符串值（否则不解释）。
 */
char *
defGetString(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return psprintf("%ld", (long) intVal(def->arg));
		case T_Float:
			return castNode(Float, def->arg)->fval;
		case T_Boolean:
			return boolVal(def->arg) ? "true" : "false";
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToString((TypeName *) def->arg);
		case T_List:
			return NameListToString((List *) def->arg);
		case T_A_Star:
			return pstrdup("*");
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	return NULL;				/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取数值（实际上是 double）。
 */
double
defGetNumeric(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (double) intVal(def->arg);
		case T_Float:
			return floatVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取布尔值。
 */
bool
defGetBoolean(DefElem *def)
{
	/*
	 * 若未给出参数值，就假定意思是 "true"。
	 */
	if (def->arg == NULL)
		return true;

	/*
	 * 允许 0、1、"true"、"false"、"on"、"off"
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			switch (intVal(def->arg))
			{
				case 0:
					return false;
				case 1:
					return true;
				default:
					/* 否则，在下面报错 */
					break;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				 * 这里接受的字符串集合应当与语法的
				 * opt_boolean_or_string 产生式相匹配。
				 */
				if (pg_strcasecmp(sval, "true") == 0)
					return true;
				if (pg_strcasecmp(sval, "false") == 0)
					return false;
				if (pg_strcasecmp(sval, "on") == 0)
					return true;
				if (pg_strcasecmp(sval, "off") == 0)
					return false;
			}
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value",
					def->defname)));
	return false;				/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取 int32 值。
 */
int32
defGetInt32(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires an integer value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int32) intVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
	}
	return 0;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取 int64 值。
 */
int64
defGetInt64(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int64) intVal(def->arg);
		case T_Float:

			/*
			 * 对 int4 来说太大的值会被词法器表示为
			 * Float 常量。若它们是合法的 int8
			 * 字符串就接受。
			 */
			return DatumGetInt64(DirectFunctionCall1(int8in,
													 CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取 OID 值。
 */
Oid
defGetObjectId(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (Oid) intVal(def->arg);
		case T_Float:

			/*
			 * 对 int4 来说太大的值会被词法器表示为
			 * Float 常量。若它们是合法的 OID
			 * 字符串就接受。
			 */
			return DatumGetObjectId(DirectFunctionCall1(oidin,
														CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取可能限定的名称（作为 String 的 List）。
 */
List *
defGetQualifiedName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return ((TypeName *) def->arg)->names;
		case T_List:
			return (List *) def->arg;
		case T_String:
			/* 为向后兼容允许带引号的名称 */
			return list_make1(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a name",
							def->defname)));
	}
	return NIL;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取 TypeName。
 *
 * 注意：这里不接受 List 参数，因为只有名称看起来像
 * 操作符名时解析器才会返回裸 List。
 */
TypeName *
defGetTypeName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return (TypeName *) def->arg;
		case T_String:
			/* 为向后兼容允许带引号的类型名 */
			return makeTypeNameFromNameList(list_make1(def->arg));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a type name",
							def->defname)));
	}
	return NULL;				/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取类型长度指示符（要么是绝对字节数，
 * 要么是表示 "variable" 的 -1）。
 */
int
defGetTypeLength(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return intVal(def->arg);
		case T_Float:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
			break;
		case T_String:
			if (pg_strcasecmp(strVal(def->arg), "variable") == 0)
				return -1;		/* 可变长度 */
			break;
		case T_TypeName:
			/* 应对语法选择相信 "variable" 是类型名的情况 */
			if (pg_strcasecmp(TypeNameToString((TypeName *) def->arg),
							  "variable") == 0)
				return -1;		/* 可变长度 */
			break;
		case T_List:
			/* 一定是操作符名 */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid argument for %s: \"%s\"",
					def->defname, defGetString(def))));
	return 0;					/* 让编译器安静 */
}

/*
 * 从 DefElem 中提取字符串值列表（否则不解释）。
 */
List *
defGetStringList(DefElem *def)
{
	ListCell   *cell;

	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	if (nodeTag(def->arg) != T_List)
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));

	foreach(cell, (List *) def->arg)
	{
		Node	   *str = (Node *) lfirst(cell);

		if (!IsA(str, String))
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(str));
	}

	return (List *) def->arg;
}

/*
 * 就冲突的 DefElem 抛出错误。
 */
void
errorConflictingDefElem(DefElem *defel, ParseState *pstate)
{
	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("conflicting or redundant options"),
			parser_errposition(pstate, defel->location));
}
