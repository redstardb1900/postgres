/*-------------------------------------------------------------------------
 *
 * oid.c
 *	  内置类型 Oid 的函数……还包括 oidvector。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/oid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "nodes/miscnodes.h"
#include "nodes/value.h"
#include "utils/array.h"
#include "utils/builtins.h"


#define OidVectorSize(n)	(offsetof(oidvector, values) + (n) * sizeof(Oid))


/*****************************************************************************
 *	 用户 I/O 例程               *
 *****************************************************************************/

Datum
oidin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Oid			result;

	result = uint32in_subr(s, NULL, "oid", fcinfo->context);
	PG_RETURN_OID(result);
}

Datum
oidout(PG_FUNCTION_ARGS)
{
	Oid			o = PG_GETARG_OID(0);
	char	   *result = (char *) palloc(12);

	snprintf(result, 12, "%u", o);
	PG_RETURN_CSTRING(result);
}

/*
 *		oidrecv			- 将外部二进制格式转为 oid
 */
Datum
oidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_OID((Oid) pq_getmsgint(buf, sizeof(Oid)));
}

/*
 *		oidsend			- 将 oid 转为二进制格式
 */
Datum
oidsend(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * 给定 Oid 原始数组构造 oidvector
 *
 * 若 oids 为 NULL，调用方之后须填充 values[]
 */
oidvector *
buildoidvector(const Oid *oids, int n)
{
	oidvector  *result;

	result = (oidvector *) palloc0(OidVectorSize(n));

	if (n > 0 && oids)
		memcpy(result->values, oids, n * sizeof(Oid));

	/*
	 * 附加标准数组头部。由于历史原因，下标下界设为 0
	 * 而不是 1。
	 */
	SET_VARSIZE(result, OidVectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* 绝无空值 */
	result->elemtype = OIDOID;
	result->dim1 = n;
	result->lbound1 = 0;

	return result;
}

/*
 *		oidvectorin			- 将 "num num ..." 转为内部形式
 */
Datum
oidvectorin(PG_FUNCTION_ARGS)
{
	char	   *oidString = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	oidvector  *result;
	int			nalloc;
	int			n;

	nalloc = 32;				/* 任意初始大小猜测 */
	result = (oidvector *) palloc0(OidVectorSize(nalloc));

	for (n = 0;; n++)
	{
		while (*oidString && isspace((unsigned char) *oidString))
			oidString++;
		if (*oidString == '\0')
			break;

		if (n >= nalloc)
		{
			nalloc *= 2;
			result = (oidvector *) repalloc(result, OidVectorSize(nalloc));
		}

		result->values[n] = uint32in_subr(oidString, &oidString,
										  "oid", escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			PG_RETURN_NULL();
	}

	SET_VARSIZE(result, OidVectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* 绝无空值 */
	result->elemtype = OIDOID;
	result->dim1 = n;
	result->lbound1 = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorout - 将内部形式转为 "num num ..."
 */
Datum
oidvectorout(PG_FUNCTION_ARGS)
{
	oidvector  *oidArray = (oidvector *) PG_GETARG_POINTER(0);
	int			num,
				nnums = oidArray->dim1;
	char	   *rp;
	char	   *result;

	/* 假定符号、10 位、' ' */
	rp = result = (char *) palloc(nnums * 12 + 1);
	for (num = 0; num < nnums; num++)
	{
		if (num != 0)
			*rp++ = ' ';
		sprintf(rp, "%u", oidArray->values[num]);
		while (*++rp != '\0')
			;
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		oidvectorrecv			- 将外部二进制格式转为 oidvector
 */
Datum
oidvectorrecv(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 3);
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	oidvector  *result;

	/*
	 * 通常会用 DirectFunctionCall3 来调用 array_recv()，但
	 * 这行不通，因为 array_recv 要用 fcinfo->flinfo->fn_extra
	 * 缓存一些数据。因此需传入我们自己的 flinfo
	 * 参数。
	 */
	InitFunctionCallInfoData(*locfcinfo, fcinfo->flinfo, 3,
							 InvalidOid, NULL, NULL);

	locfcinfo->args[0].value = PointerGetDatum(buf);
	locfcinfo->args[0].isnull = false;
	locfcinfo->args[1].value = ObjectIdGetDatum(OIDOID);
	locfcinfo->args[1].isnull = false;
	locfcinfo->args[2].value = Int32GetDatum(-1);
	locfcinfo->args[2].isnull = false;

	result = (oidvector *) DatumGetPointer(array_recv(locfcinfo));

	Assert(!locfcinfo->isnull);

	/* 检查：oidvector 必须为 1 维、0 基、无空值 */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != OIDOID ||
		ARR_LBOUND(result)[0] != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid oidvector data")));

	PG_RETURN_POINTER(result);
}

/*
 *		oidvectorsend			- 将 oidvector 转为二进制格式
 */
Datum
oidvectorsend(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 *		oidparse				- 从 ICONST/FCONST 取 OID
 */
Oid
oidparse(Node *node)
{
	switch (nodeTag(node))
	{
		case T_Integer:
			return intVal(node);
		case T_Float:

			/*
			 * int4 容不下的值将被词法分析器表示为
			 * Float 常量。若为有效 OID 串则接
			 * 受。
			 */
			return uint32in_subr(castNode(Float, node)->fval, NULL,
								 "oid", NULL);
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
	return InvalidOid;			/* 让编译器安静 */
}

/* Oid 的 qsort 比较函数 */
int
oid_cmp(const void *p1, const void *p2)
{
	Oid			v1 = *((const Oid *) p1);
	Oid			v2 = *((const Oid *) p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}


/*****************************************************************************
 *	 公共例程                  *
 *****************************************************************************/

Datum
oideq(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
oidne(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
oidlt(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
oidle(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
oidge(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
oidgt(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
oidlarger(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_OID((arg1 > arg2) ? arg1 : arg2);
}

Datum
oidsmaller(PG_FUNCTION_ARGS)
{
	Oid			arg1 = PG_GETARG_OID(0);
	Oid			arg2 = PG_GETARG_OID(1);

	PG_RETURN_OID((arg1 < arg2) ? arg1 : arg2);
}

Datum
oidvectoreq(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp == 0);
}

Datum
oidvectorne(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp != 0);
}

Datum
oidvectorlt(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp < 0);
}

Datum
oidvectorle(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
oidvectorge(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
oidvectorgt(PG_FUNCTION_ARGS)
{
	int32		cmp = DatumGetInt32(btoidvectorcmp(fcinfo));

	PG_RETURN_BOOL(cmp > 0);
}
