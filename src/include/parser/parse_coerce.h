/*-------------------------------------------------------------------------
 *
 * parse_coerce.h
 *	类型强制转换例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_coerce.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_COERCE_H
#define PARSE_COERCE_H

#include "parser/parse_node.h"


/*  类型类别（见 catalog/pg_type.h 中 TYPCATEGORY_xxx）  */
typedef char TYPCATEGORY;

/*  find_coercion_pathway 的结果码  */
typedef enum CoercionPathType
{
	COERCION_PATH_NONE,			/*  未找到任何强制路径  */
	COERCION_PATH_FUNC,			/*  应用指定的强制函数  */
	COERCION_PATH_RELABELTYPE,	/*  二进制兼容造型，无函数  */
	COERCION_PATH_ARRAYCOERCE,	/*  需 ArrayCoerceExpr 节点  */
	COERCION_PATH_COERCEVIAIO	/*  CoerceViaIO 节点  */
} CoercionPathType;


extern bool IsBinaryCoercible(Oid srctype, Oid targettype);
extern bool IsBinaryCoercibleWithCast(Oid srctype, Oid targettype,
									  Oid *castoid);
extern bool IsPreferredType(TYPCATEGORY category, Oid type);
extern TYPCATEGORY TypeCategory(Oid type);

extern Node *coerce_to_target_type(ParseState *pstate,
								   Node *expr, Oid exprtype,
								   Oid targettype, int32 targettypmod,
								   CoercionContext ccontext,
								   CoercionForm cformat,
								   int location);
extern bool can_coerce_type(int nargs, const Oid *input_typeids, const Oid *target_typeids,
							CoercionContext ccontext);
extern Node *coerce_type(ParseState *pstate, Node *node,
						 Oid inputTypeId, Oid targetTypeId, int32 targetTypeMod,
						 CoercionContext ccontext, CoercionForm cformat, int location);
extern Node *coerce_to_domain(Node *arg, Oid baseTypeId, int32 baseTypeMod,
							  Oid typeId,
							  CoercionContext ccontext, CoercionForm cformat, int location,
							  bool hideInputCoercion);

extern Node *coerce_to_boolean(ParseState *pstate, Node *node,
							   const char *constructName);
extern Node *coerce_to_specific_type(ParseState *pstate, Node *node,
									 Oid targetTypeId,
									 const char *constructName);

extern Node *coerce_to_specific_type_typmod(ParseState *pstate, Node *node,
											Oid targetTypeId, int32 targetTypmod,
											const char *constructName);

extern Node *coerce_null_to_domain(Oid typid, int32 typmod, Oid collation,
								   int typlen, bool typbyval);

extern int	parser_coercion_errposition(ParseState *pstate,
										int coerce_location,
										Node *input_expr);

extern Oid	select_common_type(ParseState *pstate, List *exprs,
							   const char *context, Node **which_expr);
extern Node *coerce_to_common_type(ParseState *pstate, Node *node,
								   Oid targetTypeId,
								   const char *context);
extern bool verify_common_type(Oid common_type, List *exprs);

extern int32 select_common_typmod(ParseState *pstate, List *exprs, Oid common_type);

extern bool check_generic_type_consistency(const Oid *actual_arg_types,
										   const Oid *declared_arg_types,
										   int nargs);
extern Oid	enforce_generic_type_consistency(const Oid *actual_arg_types,
											 Oid *declared_arg_types,
											 int nargs,
											 Oid rettype,
											 bool allow_poly);

extern char *check_valid_polymorphic_signature(Oid ret_type,
											   const Oid *declared_arg_types,
											   int nargs);
extern char *check_valid_internal_signature(Oid ret_type,
											const Oid *declared_arg_types,
											int nargs);

extern CoercionPathType find_coercion_pathway(Oid targetTypeId,
											  Oid sourceTypeId,
											  CoercionContext ccontext,
											  Oid *funcid);
extern CoercionPathType find_typmod_coercion_function(Oid typeId,
													  Oid *funcid);

#endif							/* PARSE_COERCE_H */
