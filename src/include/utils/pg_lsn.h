/*-------------------------------------------------------------------------
 *
 * pg_lsn.h
 *		对日志序列号（LSNs）相关运算的
 *		声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/pg_lsn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LSN_H
#define PG_LSN_H

#include "access/xlogdefs.h"
#include "fmgr.h"

static inline XLogRecPtr
DatumGetLSN(Datum X)
{
	return (XLogRecPtr) DatumGetInt64(X);
}

static inline Datum
LSNGetDatum(XLogRecPtr X)
{
	return Int64GetDatum((int64) X);
}

#define PG_GETARG_LSN(n)	 DatumGetLSN(PG_GETARG_DATUM(n))
#define PG_RETURN_LSN(x)	 return LSNGetDatum(x)

extern XLogRecPtr pg_lsn_in_internal(const char *str, bool *have_error);

#endif							/* PG_LSN_H */
