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
 * Float 内部以字符串表示。用 T_Float 作节点类型
 * 仅表明字符串内容看起来像合法数值字面量。
 * 该值最终或转为 NUMERIC，故不能
 * 内部存为 C double，否则可能丢精度。因
 * 这些节点通常仅用于解析过程，非运行时
 * 数据，宜用更通用表示。
 *
 * 注意：形似整数的串若过大放不进 int，
 * 则词法识别为 T_Float。
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
