/*-------------------------------------------------------------------------
 *
 * uuid.h
 *	  uuid ADT 头文件。在 C 中用名 pg_uuid_t，
 *	  以免与系统头中可能定义的 uuid_t 类型冲突。
 *	  见上。
 *
 * Copyright (c) 2007-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/uuid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UUID_H
#define UUID_H

/*  uuid 字节数  */
#define UUID_LEN 16

typedef struct pg_uuid_t
{
	unsigned char data[UUID_LEN];
} pg_uuid_t;

/*  fmgr 接口宏  */
static inline Datum
UUIDPGetDatum(const pg_uuid_t *X)
{
	return PointerGetDatum(X);
}

#define PG_RETURN_UUID_P(X)		return UUIDPGetDatum(X)

static inline pg_uuid_t *
DatumGetUUIDP(Datum X)
{
	return (pg_uuid_t *) DatumGetPointer(X);
}

#define PG_GETARG_UUID_P(X)		DatumGetUUIDP(PG_GETARG_DATUM(X))

#endif							/* UUID_H */
