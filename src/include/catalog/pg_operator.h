/*-------------------------------------------------------------------------
 *
 * pg_operator.h
 *	  definition of the "operator" system catalog (pg_operator)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_operator.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPERATOR_H
#define PG_OPERATOR_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_operator_d.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_operator definition.  cpp turns this into
 *		typedef struct FormData_pg_operator
 * ----------------
 */
CATALOG(pg_operator,2617,OperatorRelationId)
{
	Oid			oid;			/* oid */

	/*  算子名  */
	NameData	oprname;

	/*  含本算子的命名空间 OID  */
	Oid			oprnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* operator owner */
	Oid			oprowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/* 'l' for prefix or 'b' for infix */
	char		oprkind BKI_DEFAULT(b);

	/* 可否用于归并连接? */
	bool		oprcanmerge BKI_DEFAULT(f);

	/* 可否用于哈希连接? */
	bool		oprcanhash BKI_DEFAULT(f);

	/*  左参数类型,前缀算子则为 0  */
	Oid			oprleft BKI_LOOKUP_OPT(pg_type);

	/* right arg type */
	Oid			oprright BKI_LOOKUP(pg_type);

	/*  结果数据类型;壳算子可为 0  */
	Oid			oprresult BKI_LOOKUP_OPT(pg_type);

	/*  交换算子 OID,无则为 0  */
	Oid			oprcom BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_operator);

	/*  否定算子 OID,无则为 0  */
	Oid			oprnegate BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_operator);

	/*  底层函数 OID;壳算子可为 0  */
	regproc		oprcode BKI_LOOKUP_OPT(pg_proc);

	/*  限制估算器 OID,或 0  */
	regproc		oprrest BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);

	/*  连接估算器 OID,或 0  */
	regproc		oprjoin BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);
} FormData_pg_operator;

/* ----------------
 *		Form_pg_operator corresponds to a pointer to a tuple with
 *		the format of pg_operator relation.
 * ----------------
 */
typedef FormData_pg_operator *Form_pg_operator;

DECLARE_UNIQUE_INDEX_PKEY(pg_operator_oid_index, 2688, OperatorOidIndexId, on pg_operator using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_operator_oprname_l_r_n_index, 2689, OperatorNameNspIndexId, on pg_operator using btree(oprname name_ops, oprleft oid_ops, oprright oid_ops, oprnamespace oid_ops));


extern ObjectAddress OperatorCreate(const char *operatorName,
									Oid operatorNamespace,
									Oid leftTypeId,
									Oid rightTypeId,
									Oid procedureId,
									List *commutatorName,
									List *negatorName,
									Oid restrictionId,
									Oid joinId,
									bool canMerge,
									bool canHash);

extern ObjectAddress makeOperatorDependencies(HeapTuple tuple,
											  bool makeExtensionDep,
											  bool isUpdate);

extern void OperatorUpd(Oid baseId, Oid commId, Oid negId, bool isDelete);

#endif							/* PG_OPERATOR_H */
