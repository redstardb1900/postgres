/*
 * src/pl/plpython/plpy_typeio.h
 */

#ifndef PLPY_TYPEIO_H
#define PLPY_TYPEIO_H

#include "access/htup.h"
#include "fmgr.h"
#include "plpython.h"
#include "utils/typcache.h"

struct PLyProcedure;			/* 避免此处依赖 plpy_procedure.h */


/*
 * 输入转换：PostgreSQL Datum 到 Python 对象。
 *
 * arg 是事先备好的转换数据，val 是要转换的值。
 * val 不得为 NULL
 *
 * 注意：转换数据结构应视为私有，属于
 * plpy_typeio.c。此处声明只为让其他模块能
 * 定义含有它们的结构。
 */
typedef struct PLyDatumToOb PLyDatumToOb;	/* 前向引用 */

typedef PyObject *(*PLyDatumToObFunc) (PLyDatumToOb *arg, Datum val);

typedef struct PLyScalarToOb
{
	FmgrInfo	typfunc;		/* 类型输出函数的查找信息 */
} PLyScalarToOb;

typedef struct PLyArrayToOb
{
	PLyDatumToOb *elm;			/* 数组元素类型的转换信息 */
} PLyArrayToOb;

typedef struct PLyTupleToOb
{
	/* 若处理的是 RECORD 类型，实际描述符在此： */
	TupleDesc	recdesc;
	/* 若处理的是命名复合类型，则设置这些字段： */
	TypeCacheEntry *typentry;	/* 类型的 typcache 项 */
	uint64		tupdescid;		/* typcache 中上次的 tupdesc 标识 */
	/* 未设置时这些字段为 NULL/0 */
	PLyDatumToOb *atts;			/* 每列转换信息的数组 */
	int			natts;			/* 数组长度 */
} PLyTupleToOb;

typedef struct PLyTransformToOb
{
	FmgrInfo	typtransform;	/* SQL 侧变换函数的查找信息 */
} PLyTransformToOb;

struct PLyDatumToOb
{
	PLyDatumToObFunc func;		/* 转换控制函数 */
	Oid			typoid;			/* 源类型的 OID */
	int32		typmod;			/* 源类型的 typmod */
	bool		typbyval;		/* 其物理表示的细节 */
	int16		typlen;
	char		typalign;
	MemoryContext mcxt;			/* 存放本信息的上下文 */
	union						/* 转换类型专属数据 */
	{
		PLyScalarToOb scalar;
		PLyArrayToOb array;
		PLyTupleToOb tuple;
		PLyTransformToOb transform;
	}			u;
};

/*
 * 输出转换：Python 对象到 PostgreSQL Datum。
 *
 * arg 是事先备好的转换数据，val 是要转换的值。
 *
 * *isnull 在 val 为 Py_None 时设为真，否则为假。
 * 即使对 Py_None，也必须调用转换函数，
 * 以便检查域约束。
 *
 * 值在数组(Python 列表)中则 inarray 为真
 * 某些情况下用它给出更好的错误消息。
 */
typedef struct PLyObToDatum PLyObToDatum;	/* 前向引用 */

typedef Datum (*PLyObToDatumFunc) (PLyObToDatum *arg, PyObject *val,
								   bool *isnull,
								   bool inarray);

typedef struct PLyObToScalar
{
	FmgrInfo	typfunc;		/* 类型输入函数的查找信息 */
	Oid			typioparam;		/* 传给它的参数 */
} PLyObToScalar;

typedef struct PLyObToArray
{
	PLyObToDatum *elm;			/* 数组元素类型的转换信息 */
	Oid			elmbasetype;	/* 元素基类型 */
} PLyObToArray;

typedef struct PLyObToTuple
{
	/* 若处理的是 RECORD 类型，实际描述符在此： */
	TupleDesc	recdesc;
	/* 若处理的是命名复合类型，则设置这些字段： */
	TypeCacheEntry *typentry;	/* 类型的 typcache 项 */
	uint64		tupdescid;		/* typcache 中上次的 tupdesc 标识 */
	/* 未设置时这些字段为 NULL/0 */
	PLyObToDatum *atts;			/* 每列转换信息的数组 */
	int			natts;			/* 数组长度 */
	/* 也许要用 record_in() 转换；是则把信息缓存在此 */
	FmgrInfo	recinfunc;		/* record_in 的查找信息 */
} PLyObToTuple;

typedef struct PLyObToDomain
{
	PLyObToDatum *base;			/* 域的基类型的转换信息 */
	void	   *domain_info;	/* domain_check() 的缓存空间 */
} PLyObToDomain;

typedef struct PLyObToTransform
{
	FmgrInfo	typtransform;	/* 到 SQL 的变换函数查找信息 */
} PLyObToTransform;

struct PLyObToDatum
{
	PLyObToDatumFunc func;		/* 转换控制函数 */
	Oid			typoid;			/* 目标类型的 OID */
	int32		typmod;			/* 目标类型的 typmod */
	bool		typbyval;		/* 其物理表示的细节 */
	int16		typlen;
	char		typalign;
	MemoryContext mcxt;			/* 存放本信息的上下文 */
	union						/* 转换类型专属数据 */
	{
		PLyObToScalar scalar;
		PLyObToArray array;
		PLyObToTuple tuple;
		PLyObToDomain domain;
		PLyObToTransform transform;
	}			u;
};


extern PGDLLEXPORT PyObject *PLy_input_convert(PLyDatumToOb *arg, Datum val);
extern PGDLLEXPORT Datum PLy_output_convert(PLyObToDatum *arg, PyObject *val,
											bool *isnull);

extern PGDLLEXPORT PyObject *PLy_input_from_tuple(PLyDatumToOb *arg, HeapTuple tuple,
												  TupleDesc desc, bool include_generated);

extern PGDLLEXPORT void PLy_input_setup_func(PLyDatumToOb *arg, MemoryContext arg_mcxt,
											 Oid typeOid, int32 typmod,
											 struct PLyProcedure *proc);
extern PGDLLEXPORT void PLy_output_setup_func(PLyObToDatum *arg, MemoryContext arg_mcxt,
											  Oid typeOid, int32 typmod,
											  struct PLyProcedure *proc);

extern PGDLLEXPORT void PLy_input_setup_tuple(PLyDatumToOb *arg, TupleDesc desc,
											  struct PLyProcedure *proc);
extern PGDLLEXPORT void PLy_output_setup_tuple(PLyObToDatum *arg, TupleDesc desc,
											   struct PLyProcedure *proc);

extern PGDLLEXPORT void PLy_output_setup_record(PLyObToDatum *arg, TupleDesc desc,
												struct PLyProcedure *proc);

/* 从 Python 对象到 C 字符串的转换，导出供变换使用 */
extern PGDLLEXPORT char *PLyObject_AsString(PyObject *plrv);

#endif							/* PLPY_TYPEIO_H */
