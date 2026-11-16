/*-------------------------------------------------------------------------
 *
 * pg_lsn.c
 *	  pg_lsn 数据类型的操作。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_lsn.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"

#define MAXPG_LSNLEN			17
#define MAXPG_LSNCOMPONENT	8

/*----------------------------------------------------------
 * 格式化与转换例程。
 *---------------------------------------------------------*/

XLogRecPtr
pg_lsn_in_internal(const char *str, bool *have_error)
{
	int			len1,
				len2;
	uint32		id,
				off;
	XLogRecPtr	result;

	Assert(have_error != NULL);
	*have_error = false;

	/* 校验输入格式。 */
	len1 = strspn(str, "0123456789abcdefABCDEF");
	if (len1 < 1 || len1 > MAXPG_LSNCOMPONENT || str[len1] != '/')
	{
		*have_error = true;
		return InvalidXLogRecPtr;
	}
	len2 = strspn(str + len1 + 1, "0123456789abcdefABCDEF");
	if (len2 < 1 || len2 > MAXPG_LSNCOMPONENT || str[len1 + 1 + len2] != '\0')
	{
		*have_error = true;
		return InvalidXLogRecPtr;
	}

	/* 解码结果 */
	id = (uint32) strtoul(str, NULL, 16);
	off = (uint32) strtoul(str + len1 + 1, NULL, 16);
	result = ((uint64) id << 32) | off;

	return result;
}

Datum
pg_lsn_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	XLogRecPtr	result;
	bool		have_error = false;

	result = pg_lsn_in_internal(str, &have_error);
	if (have_error)
		ereturn(fcinfo->context, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"pg_lsn", str)));

	PG_RETURN_LSN(result);
}

Datum
pg_lsn_out(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	char		buf[MAXPG_LSNLEN + 1];
	char	   *result;

	snprintf(buf, sizeof buf, "%X/%X", LSN_FORMAT_ARGS(lsn));
	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

Datum
pg_lsn_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	XLogRecPtr	result;

	result = pq_getmsgint64(buf);
	PG_RETURN_LSN(result);
}

Datum
pg_lsn_send(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, lsn);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 *	PostgreSQL LSN 算子
 *---------------------------------------------------------*/

Datum
pg_lsn_eq(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 == lsn2);
}

Datum
pg_lsn_ne(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 != lsn2);
}

Datum
pg_lsn_lt(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 < lsn2);
}

Datum
pg_lsn_gt(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 > lsn2);
}

Datum
pg_lsn_le(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 <= lsn2);
}

Datum
pg_lsn_ge(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(lsn1 >= lsn2);
}

Datum
pg_lsn_larger(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_LSN((lsn1 > lsn2) ? lsn1 : lsn2);
}

Datum
pg_lsn_smaller(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);

	PG_RETURN_LSN((lsn1 < lsn2) ? lsn1 : lsn2);
}

/* btree 索引算子类支持 */
Datum
pg_lsn_cmp(PG_FUNCTION_ARGS)
{
	XLogRecPtr	a = PG_GETARG_LSN(0);
	XLogRecPtr	b = PG_GETARG_LSN(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

/* hash 索引算子类支持 */
Datum
pg_lsn_hash(PG_FUNCTION_ARGS)
{
	/* 可直接使用 hashint8 */
	return hashint8(fcinfo);
}

Datum
pg_lsn_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}


/*----------------------------------------------------------
 *	PostgreSQL LSN 上的算术算子。
 *---------------------------------------------------------*/

Datum
pg_lsn_mi(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn1 = PG_GETARG_LSN(0);
	XLogRecPtr	lsn2 = PG_GETARG_LSN(1);
	char		buf[256];
	Datum		result;

	/* 输出可能大到正负 2^63 - 1。 */
	if (lsn1 < lsn2)
		snprintf(buf, sizeof buf, "-" UINT64_FORMAT, lsn2 - lsn1);
	else
		snprintf(buf, sizeof buf, UINT64_FORMAT, lsn1 - lsn2);

	/* 转为 numeric。 */
	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(buf),
								 ObjectIdGetDatum(0),
								 Int32GetDatum(-1));

	return result;
}

/*
 * 把字节数加到 pg_lsn 上，得到新的 pg_lsn。
 * 必须同时处理正负两种字节数。
 */
Datum
pg_lsn_pli(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	Numeric		nbytes = PG_GETARG_NUMERIC(1);
	Datum		num;
	Datum		res;
	char		buf[32];

	if (numeric_is_nan(nbytes))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot add NaN to pg_lsn")));

	/* 转为 numeric */
	snprintf(buf, sizeof(buf), UINT64_FORMAT, lsn);
	num = DirectFunctionCall3(numeric_in,
							  CStringGetDatum(buf),
							  ObjectIdGetDatum(0),
							  Int32GetDatum(-1));

	/* numeric 相加 */
	res = DirectFunctionCall2(numeric_add,
							  num,
							  NumericGetDatum(nbytes));

	/* 转为 pg_lsn */
	return DirectFunctionCall1(numeric_pg_lsn, res);
}

/*
 * 从 pg_lsn 中减去字节数，得到新的 pg_lsn。
 * 必须同时处理正负两种字节数。
 */
Datum
pg_lsn_mii(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = PG_GETARG_LSN(0);
	Numeric		nbytes = PG_GETARG_NUMERIC(1);
	Datum		num;
	Datum		res;
	char		buf[32];

	if (numeric_is_nan(nbytes))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot subtract NaN from pg_lsn")));

	/* 转为 numeric */
	snprintf(buf, sizeof(buf), UINT64_FORMAT, lsn);
	num = DirectFunctionCall3(numeric_in,
							  CStringGetDatum(buf),
							  ObjectIdGetDatum(0),
							  Int32GetDatum(-1));

	/* numeric 相减 */
	res = DirectFunctionCall2(numeric_sub,
							  num,
							  NumericGetDatum(nbytes));

	/* 转为 pg_lsn */
	return DirectFunctionCall1(numeric_pg_lsn, res);
}
