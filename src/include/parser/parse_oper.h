/*-------------------------------------------------------------------------
 *
 * parse_oper.h
 *		解析器中处理操作符事宜
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_oper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_OPER_H
#define PARSE_OPER_H

#include "access/htup.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"


typedef HeapTuple Operator;

/*  按名称与精确输入 type 查找操作符的例程  */
extern Oid	LookupOperName(ParseState *pstate, List *opername,
						   Oid oprleft, Oid oprright,
						   bool noError, int location);
extern Oid	LookupOperWithArgs(ObjectWithArgs *oper, bool noError);

/*  按名称与给定输入类型查找匹配操作符的例程  */
/*  注意：所选操作符可能要求强制输入类型！  */
extern Operator oper(ParseState *pstate, List *opname, Oid ltypeId,
					 Oid rtypeId, bool noError, int location);
extern Operator left_oper(ParseState *pstate, List *op, Oid arg,
						  bool noError, int location);

/*  查找无需强制的操作符的例程——即其  */
/*  输入类型恰为给定，或二进制兼容  */
extern Operator compatible_oper(ParseState *pstate, List *op,
								Oid arg1, Oid arg2,
								bool noError, int location);

/*  目前无需 compatible_left_oper/compatible_right_oper  */

/*  识别某类型的 "<"、"="、">" 操作符的例程  */
extern void get_sort_group_operators(Oid argtype,
									 bool needLT, bool needEQ, bool needGT,
									 Oid *ltOpr, Oid *eqOpr, Oid *gtOpr,
									 bool *isHashable);

/*  对上述常见调用的便捷例程  */
extern Oid	compatible_oper_opid(List *op, Oid arg1, Oid arg2, bool noError);

/*  从 Operator 元组提取操作符 OID 或底层函数 OID  */
extern Oid	oprid(Operator op);
extern Oid	oprfuncid(Operator op);

/*  为操作符调用构建表达式树  */
extern Expr *make_op(ParseState *pstate, List *opname,
					 Node *ltree, Node *rtree, Node *last_srf, int location);
extern Expr *make_scalar_array_op(ParseState *pstate, List *opname,
								  bool useOr,
								  Node *ltree, Node *rtree, int location);

#endif							/* PARSE_OPER_H */
