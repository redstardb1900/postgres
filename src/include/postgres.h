/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  PostgreSQL 服务器 .c 文件的主包含文件
 *
 * PostgreSQL 后端模块应首先包含本文件。
 * 客户端代码应改为包含 postgres_fe.h。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres.h
 *
 *-------------------------------------------------------------------------
 */
/*
 *----------------------------------------------------------------
 *	 TABLE OF CONTENTS
 *
 *		向本文件添加内容时，请尽量放进
 *		相关节，或酌情新增节。
 *
 *	  节	说明
 *	  -------	------------------------------------------------
 *		1)		Datum 类型 + 支持函数
 *		2)		杂项
 *
 *	 NOTES
 *
 *	一般而言，本文件应含后端环境中广泛需要、
 *	但后端之外不关心的声明。
 *
 *	简单类型定义在 c.h，与
 *	postgres_fe.h 共享。因前端模块若要处理与
 *	后端之间的二进制数据传输也需要这些
 *	类型定义。本文件中的类型定义应针对
 *	永不逃出后端的类型，如 Datum。
 *
 *----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *				第 1 节：	Datum 类型 + 支持函数
 * ----------------------------------------------------------------
 */

/*
 * Datum 含按值传递类型的值，或指向
 * 按引用传递类型值的指针。因此要求：
 *
 * sizeof(Datum) == sizeof(void *) == 4 or 8
 *
 * 下列函数及其他类型的类似函数应用于
 * 在 Datum 与相应 C 类型之间转换。
 */

typedef uintptr_t Datum;

/*
 * NullableDatum 用于需同时存放 Datum 及其空值性的
 * 场合。因空间局部性更好，这可比分开数组存
 * Datum 与空值性更高效，尽管填充可能
 * 浪费更多空间。
 */
typedef struct NullableDatum
{
#define FIELDNO_NULLABLE_DATUM_DATUM 0
	Datum		value;
#define FIELDNO_NULLABLE_DATUM_ISNULL 1
	bool		isnull;
	/*  因对齐填充，此处可免费用于标志  */
} NullableDatum;

#define SIZEOF_DATUM SIZEOF_VOID_P

/*
 * DatumGetBool
 *		返回 datum 的布尔值。
 *
 * 注意：任何非零值皆视为真。
 */
static inline bool
DatumGetBool(Datum X)
{
	return (X != 0);
}

/*
 * BoolGetDatum
 *		返回布尔的 datum 表示。
 *
 * 注意：任何非零值皆视为真。
 */
static inline Datum
BoolGetDatum(bool X)
{
	return (Datum) (X ? 1 : 0);
}

/*
 * DatumGetChar
 *		返回 datum 的字符值。
 */
static inline char
DatumGetChar(Datum X)
{
	return (char) X;
}

/*
 * CharGetDatum
 *		返回字符的 datum 表示。
 */
static inline Datum
CharGetDatum(char X)
{
	return (Datum) X;
}

/*
 * Int8GetDatum
 *		返回 8 位整数的 datum 表示。
 */
static inline Datum
Int8GetDatum(int8 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt8
 *		返回 datum 的 8 位无符号整数值。
 */
static inline uint8
DatumGetUInt8(Datum X)
{
	return (uint8) X;
}

/*
 * UInt8GetDatum
 *		返回 8 位无符号整数的 datum 表示。
 */
static inline Datum
UInt8GetDatum(uint8 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt16
 *		返回 datum 的 16 位整数值。
 */
static inline int16
DatumGetInt16(Datum X)
{
	return (int16) X;
}

/*
 * Int16GetDatum
 *		返回 16 位整数的 datum 表示。
 */
static inline Datum
Int16GetDatum(int16 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt16
 *		返回 datum 的 16 位无符号整数值。
 */
static inline uint16
DatumGetUInt16(Datum X)
{
	return (uint16) X;
}

/*
 * UInt16GetDatum
 *		返回 16 位无符号整数的 datum 表示。
 */
static inline Datum
UInt16GetDatum(uint16 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt32
 *		返回 datum 的 32 位整数值。
 */
static inline int32
DatumGetInt32(Datum X)
{
	return (int32) X;
}

/*
 * Int32GetDatum
 *		返回 32 位整数的 datum 表示。
 */
static inline Datum
Int32GetDatum(int32 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt32
 *		返回 datum 的 32 位无符号整数值。
 */
static inline uint32
DatumGetUInt32(Datum X)
{
	return (uint32) X;
}

/*
 * UInt32GetDatum
 *		返回 32 位无符号整数的 datum 表示。
 */
static inline Datum
UInt32GetDatum(uint32 X)
{
	return (Datum) X;
}

/*
 * DatumGetObjectId
 *		返回 datum 的对象标识符值。
 */
static inline Oid
DatumGetObjectId(Datum X)
{
	return (Oid) X;
}

/*
 * ObjectIdGetDatum
 *		返回对象标识符的 datum 表示。
 */
static inline Datum
ObjectIdGetDatum(Oid X)
{
	return (Datum) X;
}

/*
 * DatumGetTransactionId
 *		返回 datum 的事务标识符值。
 */
static inline TransactionId
DatumGetTransactionId(Datum X)
{
	return (TransactionId) X;
}

/*
 * TransactionIdGetDatum
 *		返回事务标识符的 datum 表示。
 */
static inline Datum
TransactionIdGetDatum(TransactionId X)
{
	return (Datum) X;
}

/*
 * MultiXactIdGetDatum
 *		返回多事务标识符的 datum 表示。
 */
static inline Datum
MultiXactIdGetDatum(MultiXactId X)
{
	return (Datum) X;
}

/*
 * DatumGetCommandId
 *		返回 datum 的命令标识符值。
 */
static inline CommandId
DatumGetCommandId(Datum X)
{
	return (CommandId) X;
}

/*
 * CommandIdGetDatum
 *		返回命令标识符的 datum 表示。
 */
static inline Datum
CommandIdGetDatum(CommandId X)
{
	return (Datum) X;
}

/*
 * DatumGetPointer
 *		返回 datum 的指针值。
 */
static inline Pointer
DatumGetPointer(Datum X)
{
	return (Pointer) X;
}

/*
 * PointerGetDatum
 *		返回指针的 datum 表示。
 */
static inline Datum
PointerGetDatum(const void *X)
{
	return (Datum) X;
}

/*
 * DatumGetCString
 *		返回 datum 的 C 串（空终止串）值。
 *
 * 注意：C 串目前并非完整 Postgres 类型，
 * 但类型输入函数用此转换其输入。
 */
static inline char *
DatumGetCString(Datum X)
{
	return (char *) DatumGetPointer(X);
}

/*
 * CStringGetDatum
 *		返回 C 串（空终止串）的 datum 表示。
 *
 * 注意：C 串目前并非完整 Postgres 类型，
 * 但类型输出函数用此转换其输出。
 * 注意：CString 按引用传递；调用方须确保所指
 * 值存活期足够。
 */
static inline Datum
CStringGetDatum(const char *X)
{
	return PointerGetDatum(X);
}

/*
 * DatumGetName
 *		返回 datum 的 name 值。
 */
static inline Name
DatumGetName(Datum X)
{
	return (Name) DatumGetPointer(X);
}

/*
 * NameGetDatum
 *		返回 name 的 datum 表示。
 *
 * 注意：Name 按引用传递；调用方须确保所指
 * 值存活期足够。
 */
static inline Datum
NameGetDatum(const NameData *X)
{
	return CStringGetDatum(NameStr(*X));
}

/*
 * DatumGetInt64
 *		返回 datum 的 64 位整数值。
 *
 * 注意：本函数隐藏 int64 按值还是按引用传递。
 */
static inline int64
DatumGetInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (int64) X;
#else
	return *((int64 *) DatumGetPointer(X));
#endif
}

/*
 * Int64GetDatum
 *		返回 64 位整数的 datum 表示。
 *
 * 注意：若 int64 按引用传递，本函数返回指向
 * palloc'd 空间。
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Int64GetDatum(int64 X)
{
	return (Datum) X;
}
#else
extern Datum Int64GetDatum(int64 X);
#endif


/*
 * DatumGetUInt64
 *		返回 datum 的 64 位无符号整数值。
 *
 * 注意：本函数隐藏 int64 按值还是按引用传递。
 */
static inline uint64
DatumGetUInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (uint64) X;
#else
	return *((uint64 *) DatumGetPointer(X));
#endif
}

/*
 * UInt64GetDatum
 *		返回 64 位无符号整数的 datum 表示。
 *
 * 注意：若 int64 按引用传递，本函数返回指向
 * palloc'd 空间。
 */
static inline Datum
UInt64GetDatum(uint64 X)
{
#ifdef USE_FLOAT8_BYVAL
	return (Datum) X;
#else
	return Int64GetDatum((int64) X);
#endif
}

/*
 * Float <-> Datum 转换
 *
 * 按值传递时，这些须实现为内联函数而非宏，因
 * 许多机器对 int 与 float 函数参数/结果的
 * 传递方式不同；故需用联合玩些奇怪的游戏。
 */

/*
 * DatumGetFloat4
 *		返回 datum 的 4 字节浮点值。
 */
static inline float4
DatumGetFloat4(Datum X)
{
	union
	{
		int32		value;
		float4		retval;
	}			myunion;

	myunion.value = DatumGetInt32(X);
	return myunion.retval;
}

/*
 * Float4GetDatum
 *		返回 4 字节浮点数的 datum 表示。
 */
static inline Datum
Float4GetDatum(float4 X)
{
	union
	{
		float4		value;
		int32		retval;
	}			myunion;

	myunion.value = X;
	return Int32GetDatum(myunion.retval);
}

/*
 * DatumGetFloat8
 *		返回 datum 的 8 字节浮点值。
 *
 * 注意：本函数隐藏 float8 按值还是按引用传递。
 */
static inline float8
DatumGetFloat8(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	union
	{
		int64		value;
		float8		retval;
	}			myunion;

	myunion.value = DatumGetInt64(X);
	return myunion.retval;
#else
	return *((float8 *) DatumGetPointer(X));
#endif
}

/*
 * Float8GetDatum
 *		返回 8 字节浮点数的 datum 表示。
 *
 * 注意：若 float8 按引用传递，本函数返回指向
 * palloc'd 空间。
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Float8GetDatum(float8 X)
{
	union
	{
		float8		value;
		int64		retval;
	}			myunion;

	myunion.value = X;
	return Int64GetDatum(myunion.retval);
}
#else
extern Datum Float8GetDatum(float8 X);
#endif


/*
 * Int64GetDatumFast
 * Float8GetDatumFast
 *
 * 这些宏旨在编写不依赖 int64 与 float8
 * 是否按引用传递的代码，同时在按值时
 * 不牺牲性能。参数必须是在 Datum 需要期间
 * 一直存在且值不变的变量。
 * 按引用时，取变量地址作为
 * Datum。按值时，与非 Fast
 * 函数相同，只是断言变量类型正确。
 */

#ifdef USE_FLOAT8_BYVAL
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), Int64GetDatum(X))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), Float8GetDatum(X))
#else
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), PointerGetDatum(&(X)))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), PointerGetDatum(&(X)))
#endif


/* ----------------------------------------------------------------
 *				第 2 节：	杂项
 * ----------------------------------------------------------------
 */

/*
 * NON_EXEC_STATIC：有时需要定义在正常情况下为 static、
 * 但使用 EXEC_BACKEND 时为 extern 的变量或函数（见
 * pg_config_manual.h）。那时 postmaster.c 中通常会有
 * 代码用这些 extern 符号在进程间传递状态，或做
 * EXEC_BACKEND 模式下所需的其他事。
 */
#ifdef EXEC_BACKEND
#define NON_EXEC_STATIC
#else
#define NON_EXEC_STATIC static
#endif

#endif							/* POSTGRES_H */
