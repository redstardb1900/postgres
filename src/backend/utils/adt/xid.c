/*-------------------------------------------------------------------------
 *
 * xid.c
 *	  POSTGRES 事务标识符与命令标识符数据类型。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/xid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/xid8.h"

#define PG_GETARG_COMMANDID(n)		DatumGetCommandId(PG_GETARG_DATUM(n))
#define PG_RETURN_COMMANDID(x)		return CommandIdGetDatum(x)


Datum
xidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	TransactionId result;

	result = uint32in_subr(str, NULL, "xid", fcinfo->context);
	PG_RETURN_TRANSACTIONID(result);
}

Datum
xidout(PG_FUNCTION_ARGS)
{
	TransactionId transactionId = PG_GETARG_TRANSACTIONID(0);
	char	   *result = (char *) palloc(16);

	snprintf(result, 16, "%lu", (unsigned long) transactionId);
	PG_RETURN_CSTRING(result);
}

/*
 *		xidrecv			- 将外部二进制格式转为 xid
 */
Datum
xidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_TRANSACTIONID((TransactionId) pq_getmsgint(buf, sizeof(TransactionId)));
}

/*
 *		xidsend		- 将 xid 转为二进制格式
 */
Datum
xidsend(PG_FUNCTION_ARGS)
{
	TransactionId arg1 = PG_GETARG_TRANSACTIONID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		xideq		- 两个 xid 相等？
 */
Datum
xideq(PG_FUNCTION_ARGS)
{
	TransactionId xid1 = PG_GETARG_TRANSACTIONID(0);
	TransactionId xid2 = PG_GETARG_TRANSACTIONID(1);

	PG_RETURN_BOOL(TransactionIdEquals(xid1, xid2));
}

/*
 *		xidneq			- 两个 xid 不同？
 */
Datum
xidneq(PG_FUNCTION_ARGS)
{
	TransactionId xid1 = PG_GETARG_TRANSACTIONID(0);
	TransactionId xid2 = PG_GETARG_TRANSACTIONID(1);

	PG_RETURN_BOOL(!TransactionIdEquals(xid1, xid2));
}

/*
 *		xid_age			- 计算 XID 的年龄（相对于最新稳定 xid）
 */
Datum
xid_age(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	TransactionId now = GetStableLatestTransactionId();

	/* 永久 XID 总是无限古老 */
	if (!TransactionIdIsNormal(xid))
		PG_RETURN_INT32(INT_MAX);

	PG_RETURN_INT32((int32) (now - xid));
}

/*
 *		mxid_age			- 计算 multi XID 的年龄（相对最新稳定 mxid）
 */
Datum
mxid_age(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	MultiXactId now = ReadNextMultiXactId();

	if (!MultiXactIdIsValid(xid))
		PG_RETURN_INT32(INT_MAX);

	PG_RETURN_INT32((int32) (now - xid));
}

/*
 * xidComparator
 *		XID 的 qsort 比较函数
 *
 * 对 XID 不能使用回绕比较，因为它不满足
 * 三角不等式！任何排序结果都可以。
 */
int
xidComparator(const void *arg1, const void *arg2)
{
	TransactionId xid1 = *(const TransactionId *) arg1;
	TransactionId xid2 = *(const TransactionId *) arg2;

	if (xid1 > xid2)
		return 1;
	if (xid1 < xid2)
		return -1;
	return 0;
}

/*
 * xidLogicalComparator
 *		XID 的 qsort 比较函数
 *
 * 本函数只用于比较同一纪元的 XID（例如同时运行
 * 的后端）。因此只会是普通 XID，故不存在三角不等式
 * 的问题。
 */
int
xidLogicalComparator(const void *arg1, const void *arg2)
{
	TransactionId xid1 = *(const TransactionId *) arg1;
	TransactionId xid2 = *(const TransactionId *) arg2;

	Assert(TransactionIdIsNormal(xid1));
	Assert(TransactionIdIsNormal(xid2));

	if (TransactionIdPrecedes(xid1, xid2))
		return -1;

	if (TransactionIdPrecedes(xid2, xid1))
		return 1;

	return 0;
}

Datum
xid8toxid(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);

	PG_RETURN_TRANSACTIONID(XidFromFullTransactionId(fxid));
}

Datum
xid8in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	uint64		result;

	result = uint64in_subr(str, NULL, "xid8", fcinfo->context);
	PG_RETURN_FULLTRANSACTIONID(FullTransactionIdFromU64(result));
}

Datum
xid8out(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);
	char	   *result = (char *) palloc(21);

	snprintf(result, 21, UINT64_FORMAT, U64FromFullTransactionId(fxid));
	PG_RETURN_CSTRING(result);
}

Datum
xid8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint64		value;

	value = (uint64) pq_getmsgint64(buf);
	PG_RETURN_FULLTRANSACTIONID(FullTransactionIdFromU64(value));
}

Datum
xid8send(PG_FUNCTION_ARGS)
{
	FullTransactionId arg1 = PG_GETARG_FULLTRANSACTIONID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, (uint64) U64FromFullTransactionId(arg1));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
xid8eq(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdEquals(fxid1, fxid2));
}

Datum
xid8ne(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(!FullTransactionIdEquals(fxid1, fxid2));
}

Datum
xid8lt(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdPrecedes(fxid1, fxid2));
}

Datum
xid8gt(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdFollows(fxid1, fxid2));
}

Datum
xid8le(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdPrecedesOrEquals(fxid1, fxid2));
}

Datum
xid8ge(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdFollowsOrEquals(fxid1, fxid2));
}

Datum
xid8cmp(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	if (FullTransactionIdFollows(fxid1, fxid2))
		PG_RETURN_INT32(1);
	else if (FullTransactionIdEquals(fxid1, fxid2))
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

Datum
xid8_larger(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	if (FullTransactionIdFollows(fxid1, fxid2))
		PG_RETURN_FULLTRANSACTIONID(fxid1);
	else
		PG_RETURN_FULLTRANSACTIONID(fxid2);
}

Datum
xid8_smaller(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	if (FullTransactionIdPrecedes(fxid1, fxid2))
		PG_RETURN_FULLTRANSACTIONID(fxid1);
	else
		PG_RETURN_FULLTRANSACTIONID(fxid2);
}

/*****************************************************************************
 *	 命令标识符例程						 *
 *****************************************************************************/

/*
 *		cidin	- 将 CommandId 转为内部表示。
 */
Datum
cidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	CommandId	result;

	result = uint32in_subr(str, NULL, "cid", fcinfo->context);
	PG_RETURN_COMMANDID(result);
}

/*
 *		cidout	- 将 cid 转为外部表示。
 */
Datum
cidout(PG_FUNCTION_ARGS)
{
	CommandId	c = PG_GETARG_COMMANDID(0);
	char	   *result = (char *) palloc(16);

	snprintf(result, 16, "%lu", (unsigned long) c);
	PG_RETURN_CSTRING(result);
}

/*
 *		cidrecv			- 将外部二进制格式转为 cid
 */
Datum
cidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_COMMANDID((CommandId) pq_getmsgint(buf, sizeof(CommandId)));
}

/*
 *		cidsend		- 将 cid 转为二进制格式
 */
Datum
cidsend(PG_FUNCTION_ARGS)
{
	CommandId	arg1 = PG_GETARG_COMMANDID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
cideq(PG_FUNCTION_ARGS)
{
	CommandId	arg1 = PG_GETARG_COMMANDID(0);
	CommandId	arg2 = PG_GETARG_COMMANDID(1);

	PG_RETURN_BOOL(arg1 == arg2);
}
