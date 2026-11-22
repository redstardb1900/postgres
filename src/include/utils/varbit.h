/*-------------------------------------------------------------------------
 *
 * varbit.h
 *	  SQL 数据类型 BIT() 与 BIT VARYING() 的函数。
 *
 * 代码最初由 Adriaan Joubert 贡献。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/varbit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VARBIT_H
#define VARBIT_H

#include <limits.h>

#include "fmgr.h"

/*
 * 仿 postgres.h 中的 struct varlena，但数据类型为 bits8。
 *
 * 注意：若 bit_len 不是 BITS_PER_BYTE 的倍数，则
 * bit_dat[] 末字节低位未用且必须为零。这样
 * bit_cmp() 就不必掩码末字节。
 * 此外，头长度中不应计入任何多余字节。
 */
typedef struct
{
	int32		vl_len_;		/* varlena 头（勿直接触碰！） */
	int32		bit_len;		/* 有效位数 */
	bits8		bit_dat[FLEXIBLE_ARRAY_MEMBER]; /* 位串,高位字节在前
												 * first */
} VarBit;

/*
 * fmgr 接口宏
 *
 * BIT 与 BIT VARYING 是可 toast 的 varlena 类型。
 * 表示相同，故共用一套宏。
 */
static inline VarBit *
DatumGetVarBitP(Datum X)
{
	return (VarBit *) PG_DETOAST_DATUM(X);
}

static inline VarBit *
DatumGetVarBitPCopy(Datum X)
{
	return (VarBit *) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
VarBitPGetDatum(const VarBit *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_VARBIT_P(n)	   DatumGetVarBitP(PG_GETARG_DATUM(n))
#define PG_GETARG_VARBIT_P_COPY(n) DatumGetVarBitPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_VARBIT_P(x)	   return VarBitPGetDatum(x)

/* 在 VARHDRSZ *之外* 的头开销 */
#define VARBITHDRSZ			sizeof(int32)
/* 本位串中的位数 */
#define VARBITLEN(PTR)		(((VarBit *) (PTR))->bit_len)
/* 指向含位串数据的首字节的指针 */
#define VARBITS(PTR)		(((VarBit *) (PTR))->bit_dat)
/* 位串数据区的字节数 */
#define VARBITBYTES(PTR)	(VARSIZE(PTR) - VARHDRSZ - VARBITHDRSZ)
/* 位串末尾填充（以位计） */
#define VARBITPAD(PTR)		(VARBITBYTES(PTR)*BITS_PER_BYTE - VARBITLEN(PTR))
/* 存储给定长度位串所需的字节数 */
#define VARBITTOTALLEN(BITLEN)	(((BITLEN) + BITS_PER_BYTE-1)/BITS_PER_BYTE + \
								 VARHDRSZ + VARBITHDRSZ)
/*
 * 最大位数。多处代码假定 bitlen + X 不溢出；
 * VARBITTOTALLEN() 中的 X 最大。
 */
#define VARBITMAXLEN		(INT_MAX - BITS_PER_BYTE + 1)
/* 指向位串末尾之外的指针（类似 STL 的 end()） */
#define VARBITEND(PTR)		(((bits8 *) (PTR)) + VARSIZE(PTR))
/* 恰好覆盖一字节的掩码，即 BITS_PER_BYTE 位 */
#define BITMASK 0xFF

#endif
