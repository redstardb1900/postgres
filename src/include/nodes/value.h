/*-------------------------------------------------------------------------
 *
 * value.h
 *	value 节点的接口
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * src/include/nodes/value.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VALUE_H
#define VALUE_H

#include "nodes/nodes.h"

/*
 * 节点类型 Integer、Float、String 与 BitString 用于在词法
 * 分析器中表示字面量，也用于在解析器中传递常量。
 * 这些节点类型与普通 int 或 char * 的一个差别是
 * 节点可放入 List。
 *
 * 曾有涵盖所有这些类型的 Value 节点,故有此文件名。
 */

typedef struct Integer
{
	pg_node_attr(special_read_write)

	NodeTag		type;
	int			ival;
} Integer;

/*
 * Float is internally represented as string.  Using T_Float as the node type
 * simply indicates that the contents of the string look like a valid numeric
 * literal.  The value might end up being converted to NUMERIC, so we can't
 * store it internally as a C double, since that could lose precision.  Since
 * these nodes are generally only used in the parsing process, not for runtime
 * data, it's better to use the more general representation.
 *
 * Note that an integer-looking string will get lexed as T_Float if the value
 * is too large to fit in an 'int'.
 */
typedef struct Float
{
	pg_node_attr(special_read_write)

	NodeTag		type;
	char	   *fval;
} Float;

typedef struct Boolean
{
	pg_node_attr(special_read_write)

	NodeTag		type;
	bool		boolval;
} Boolean;

typedef struct String
{
	pg_node_attr(special_read_write)

	NodeTag		type;
	char	   *sval;
} String;

typedef struct BitString
{
	pg_node_attr(special_read_write)

	NodeTag		type;
	char	   *bsval;
} BitString;

#define intVal(v)		(castNode(Integer, v)->ival)
#define floatVal(v)		atof(castNode(Float, v)->fval)
#define boolVal(v)		(castNode(Boolean, v)->boolval)
#define strVal(v)		(castNode(String, v)->sval)

extern Integer *makeInteger(int i);
extern Float *makeFloat(char *numericStr);
extern Boolean *makeBoolean(bool val);
extern String *makeString(char *str);
extern BitString *makeBitString(char *str);

#endif							/* VALUE_H */
