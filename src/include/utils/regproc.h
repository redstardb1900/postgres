/*-------------------------------------------------------------------------
 *
 * regproc.h
 *	  内建类型 regproc、regclass、regtype 等的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/regproc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REGPROC_H
#define REGPROC_H

#include "nodes/pg_list.h"

/*  format_procedure_extended 的控制标志  */
#define FORMAT_PROC_INVALID_AS_NULL	0x01	/*  未定义则空  */
#define FORMAT_PROC_FORCE_QUALIFY	0x02	/*  强制限定  */
extern char *format_procedure_extended(Oid procedure_oid, bits16 flags);

/*  format_operator_extended 的控制标志  */
#define FORMAT_OPERATOR_INVALID_AS_NULL	0x01	/*  未定义则空  */
#define FORMAT_OPERATOR_FORCE_QUALIFY	0x02	/*  强制限定  */
extern char *format_operator_extended(Oid operator_oid, bits16 flags);

extern List *stringToQualifiedNameList(const char *string, Node *escontext);
extern char *format_procedure(Oid procedure_oid);
extern char *format_procedure_qualified(Oid procedure_oid);
extern void format_procedure_parts(Oid procedure_oid, List **objnames,
								   List **objargs, bool missing_ok);

extern char *format_operator(Oid operator_oid);
extern char *format_operator_qualified(Oid operator_oid);
extern void format_operator_parts(Oid operator_oid, List **objnames,
								  List **objargs, bool missing_ok);

#endif
