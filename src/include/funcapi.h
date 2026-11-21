/*-------------------------------------------------------------------------
 *
 * funcapi.h
 *	  返回复合类型和/或集合的函数定义，
 *	 或处理 VARIADIC。
 *
 * 凡定义或调用 FUNCAPI 可调函数/宏的
 * Postgres 模块皆须包含本文件。
 *
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * src/include/funcapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCAPI_H
#define FUNCAPI_H

#include "access/tupdesc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "fmgr.h"

/*-------------------------------------------------------------------------
 *	便于编写返回复合类型的函数
 *-------------------------------------------------------------------------
 *
 * 此结构保存从原始 C 串创建元组所需的
 * 各属性信息数组。亦需
 * TupleDesc 副本。此处信息
 * 源自 TupleDesc，但存此以免
 * SRF 每次调用重复消耗 CPU。
 */
typedef struct AttInMetadata
{
	/*  TupleDesc  */
	TupleDesc	tupdesc;

	/*  属性类型输入函数 finfo 数组  */
	FmgrInfo   *attinfuncs;

	/*  属性类型 i/o 参数 OIDs 数组  */
	Oid		   *attioparams;

	/*  属性 typmod 数组  */
	int32	   *atttypmods;
} AttInMetadata;

/*-------------------------------------------------------------------------
 *		便于编写返回集合函数 (SRFs) 的支持结构
 *-------------------------------------------------------------------------
 *
 * 此结构保存返回集合函数的函数上下文。
 * 用 fn_extra 跨调用保存其指针
 */
typedef struct FuncCallContext
{
	/*
	 * 此前已被调用的次数
	 *
	 * call_cntr 由 SRF_FIRSTCALL_INIT() 初始化为 0，并在
	 * 每次 SRF_RETURN_NEXT() 时为你递增。
	 */
	uint64		call_cntr;

	/*
	 * OPTIONAL 最大调用次数
	 *
	 * max_calls 仅为方便，设置可选。若
	 * 未设，须另有办法知道函数
	 * 略
	 */
	uint64		max_calls;

	/*
	 * OPTIONAL 指向用户杂项上下文的指针
	 *
	 * user_fctx 用作指向自有结构的指针，以在
	 * 函数调用间保留任意上下文信息。
	 */
	void	   *user_fctx;

	/*
	 * OPTIONAL 指向属性类型输入元数据
	 *
	 * attinmeta 用于返回元组（即复合类型），
	 * 返回基类型时不用。仅当你
	 * 打算用 BuildTupleFromCStrings() 创建返回元组时才需。
	 */
	AttInMetadata *attinmeta;

	/*
	 * 用于须跨多次调用存活的结构的内存上下文
	 *
	 * multi_call_memory_ctx 由 SRF_FIRSTCALL_INIT() 设置，并由
	 * SRF_RETURN_DONE() 清理。最适合存放
	 * 将在 SRF 多次调用间复用的
	 * 略
	 */
	MemoryContext multi_call_memory_ctx;

	/*
	 * OPTIONAL 指向元组描述结构的指针
	 *
	 * tuple_desc 用于返回元组（即复合类型），
	 * 仅当你用 heap_form_tuple() 而非
	 * BuildTupleFromCStrings() 建元组时才需。注意
	 * 此处存的 TupleDesc 指针通常应先经
	 * BlessTupleDesc()。
	 */
	TupleDesc	tuple_desc;

} FuncCallContext;

/*----------
 *	便于编写返回复合类型的函数
 *
 * 外部声明：
 * get_call_result_type:
 *		给定函数调用信息记录，判定应返回的
 *		数据类型种类。若 resultTypeId 非 NULL，*resultTypeId
 *		收到实际数据类型 OID（对标量结果
 *		类型有用）。若 resultTupleDesc 非 NULL，
 *		在结果为复合类型时收到 TupleDesc
 *		指针；标量或无法确定行类型时为
 *		NULL。注意：若需长期访问，应复制
 *		tupledesc。
 * get_expr_result_type:
 *		给定表达式节点，返回与
 *		get_call_result_type。注意：无法确定行类型的
 *		情形与 get_call_result_type 不同。
 * get_func_result_type:
 *		仅给定函数 OID，返回与
 *		get_call_result_type。注意：无法确定行类型的
 *		情形与 get_call_result_type 不同。
 *		若能用另两者则*勿*用此。
 *
 * 另见 get_expr_result_tupdesc()，它是
 * get_expr_result_type() 的便捷包装，供调用方只关心
 * 可确定行类型情形。
 *----------
 */

/*  get_call_result_type 及其兄弟的类型类别  */
typedef enum TypeFuncClass
{
	TYPEFUNC_SCALAR,			/*  标量结果  */
	TYPEFUNC_COMPOSITE,			/*  可确定行类型结果  */
	TYPEFUNC_COMPOSITE_DOMAIN,	/*  可确定行类型上的域结果  */
	TYPEFUNC_RECORD,			/*  不确定行类型结果  */
	TYPEFUNC_OTHER				/*  虚假/伪类型  */
} TypeFuncClass;

extern TypeFuncClass get_call_result_type(FunctionCallInfo fcinfo,
										  Oid *resultTypeId,
										  TupleDesc *resultTupleDesc);
extern TypeFuncClass get_expr_result_type(Node *expr,
										  Oid *resultTypeId,
										  TupleDesc *resultTupleDesc);
extern TypeFuncClass get_func_result_type(Oid functionId,
										  Oid *resultTypeId,
										  TupleDesc *resultTupleDesc);

extern TupleDesc get_expr_result_tupdesc(Node *expr, bool noError);

extern bool resolve_polymorphic_argtypes(int numargs, Oid *argtypes,
										 char *argmodes,
										 Node *call_expr);

extern int	get_func_arg_info(HeapTuple procTup,
							  Oid **p_argtypes, char ***p_argnames,
							  char **p_argmodes);

extern int	get_func_input_arg_names(Datum proargnames, Datum proargmodes,
									 char ***arg_names);

extern int	get_func_trftypes(HeapTuple procTup, Oid **p_trftypes);
extern char *get_func_result_name(Oid functionId);

extern TupleDesc build_function_result_tupdesc_d(char prokind,
												 Datum proallargtypes,
												 Datum proargmodes,
												 Datum proargnames);
extern TupleDesc build_function_result_tupdesc_t(HeapTuple procTuple);


/*----------
 *	Support to ease writing functions returning composite types
 *
 * External declarations:
 * TupleDesc BlessTupleDesc(TupleDesc tupdesc) - "Bless" a completed tuple
 *		descriptor so that it can be used to return properly labeled tuples.
 *		You need to call this if you are going to use heap_form_tuple directly.
 *		TupleDescGetAttInMetadata does it for you, however, so no need to call
 *		it if you call TupleDescGetAttInMetadata.
 * AttInMetadata *TupleDescGetAttInMetadata(TupleDesc tupdesc) - Build an
 *		AttInMetadata struct based on the given TupleDesc. AttInMetadata can
 *		be used in conjunction with C strings to produce a properly formed
 *		tuple.
 * HeapTuple BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values) -
 *		build a HeapTuple given user data in C string form. values is an array
 *		of C strings, one for each attribute of the return tuple.
 * Datum HeapTupleHeaderGetDatum(HeapTupleHeader tuple) - convert a
 *		HeapTupleHeader to a Datum.
 *
 * Inline declarations:
 * HeapTupleGetDatum(HeapTuple tuple) - convert a HeapTuple to a Datum.
 *
 * Obsolete routines and macros:
 * TupleDesc RelationNameGetTupleDesc(const char *relname) - Use to get a
 *		TupleDesc based on a named relation.
 * TupleDesc TypeGetTupleDesc(Oid typeoid, List *colaliases) - Use to get a
 *		TupleDesc based on a type OID.
 * TupleGetDatum(TupleTableSlot *slot, HeapTuple tuple) - get a Datum
 *		given a tuple and a slot.
 *----------
 */

extern TupleDesc RelationNameGetTupleDesc(const char *relname);
extern TupleDesc TypeGetTupleDesc(Oid typeoid, List *colaliases);

/* from execTuples.c */
extern TupleDesc BlessTupleDesc(TupleDesc tupdesc);
extern AttInMetadata *TupleDescGetAttInMetadata(TupleDesc tupdesc);
extern HeapTuple BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values);
extern Datum HeapTupleHeaderGetDatum(HeapTupleHeader tuple);

static inline Datum
HeapTupleGetDatum(const HeapTupleData *tuple)
{
	return HeapTupleHeaderGetDatum(tuple->t_data);
}

/*  上述的过时版本  */
#define TupleGetDatum(_slot, _tuple)	HeapTupleGetDatum(_tuple)


/*----------
 *		Support for Set Returning Functions (SRFs)
 *
 * The basic API for SRFs using ValuePerCall mode looks something like this:
 *
 * Datum
 * my_Set_Returning_Function(PG_FUNCTION_ARGS)
 * {
 *	FuncCallContext    *funcctx;
 *	Datum				result;
 *	MemoryContext		oldcontext;
 *	<user defined declarations>
 *
 *	if (SRF_IS_FIRSTCALL())
 *	{
 *		funcctx = SRF_FIRSTCALL_INIT();
 *		// switch context when allocating stuff to be used in later calls
 *		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
 *		<user defined code>
 *		<if returning composite>
 *			<build TupleDesc, and perhaps AttInMetadata>
 *		<endif returning composite>
 *		<user defined code>
 *		// return to original context when allocating transient memory
 *		MemoryContextSwitchTo(oldcontext);
 *	}
 *	<user defined code>
 *	funcctx = SRF_PERCALL_SETUP();
 *	<user defined code>
 *
 *	if (funcctx->call_cntr < funcctx->max_calls)
 *	{
 *		<user defined code>
 *		<obtain result Datum>
 *		SRF_RETURN_NEXT(funcctx, result);
 *	}
 *	else
 *		SRF_RETURN_DONE(funcctx);
 * }
 *
 * NOTE: there is no guarantee that a SRF using ValuePerCall mode will be
 * run to completion; for example, a query with LIMIT might stop short of
 * fetching all the rows.  Therefore, do not expect that you can do resource
 * cleanup just before SRF_RETURN_DONE().  You need not worry about releasing
 * memory allocated in multi_call_memory_ctx, but holding file descriptors or
 * other non-memory resources open across calls is a bug.  SRFs that need
 * such resources should not use these macros, but instead populate a
 * tuplestore during a single call, as set up by InitMaterializedSRF() (see
 * fmgr/README).  Alternatively, set up a callback to release resources
 * at query shutdown, using RegisterExprContextCallback().
 *
 *----------
 */

/* from funcapi.c */

/*  InitMaterializedSRF 标志  */
#define MAT_SRF_USE_EXPECTED_DESC	0x01	/*  expectedDesc 作 tupdesc  */
#define MAT_SRF_BLESS				0x02	/*  用 BlessTupleDesc() "祝福"
											 * BlessTupleDesc(). */
extern void InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags);

extern FuncCallContext *init_MultiFuncCall(PG_FUNCTION_ARGS);
extern FuncCallContext *per_MultiFuncCall(PG_FUNCTION_ARGS);
extern void end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx);

#define SRF_IS_FIRSTCALL() (fcinfo->flinfo->fn_extra == NULL)

#define SRF_FIRSTCALL_INIT() init_MultiFuncCall(fcinfo)

#define SRF_PERCALL_SETUP() per_MultiFuncCall(fcinfo)

#define SRF_RETURN_NEXT(_funcctx, _result) \
	do { \
		ReturnSetInfo	   *rsi; \
		(_funcctx)->call_cntr++; \
		rsi = (ReturnSetInfo *) fcinfo->resultinfo; \
		rsi->isDone = ExprMultipleResult; \
		PG_RETURN_DATUM(_result); \
	} while (0)

#define SRF_RETURN_NEXT_NULL(_funcctx) \
	do { \
		ReturnSetInfo	   *rsi; \
		(_funcctx)->call_cntr++; \
		rsi = (ReturnSetInfo *) fcinfo->resultinfo; \
		rsi->isDone = ExprMultipleResult; \
		PG_RETURN_NULL(); \
	} while (0)

#define  SRF_RETURN_DONE(_funcctx) \
	do { \
		ReturnSetInfo	   *rsi; \
		end_MultiFuncCall(fcinfo, _funcctx); \
		rsi = (ReturnSetInfo *) fcinfo->resultinfo; \
		rsi->isDone = ExprEndResult; \
		PG_RETURN_NULL(); \
	} while (0)

/*----------
 *	便于编写处理 VARIADIC 输入的函数
 *----------
 *
 * 提取给定输入函数的参数值、类型与 NULL 标记。
 * 返回一组数据：
 * - **values 含所提取的 Datum 值集。
 * - **types 各元素的数据类型 OID。
 * - **nulls 跟踪元素是否为 NULL。
 *
 * variadic_start 指出 VARIADIC 参数起始的
 * starts.
 * convert_unknown 为真时将强制把未知
 * 类型参数转为 text。
 *
 * 返回结果为所存元素个数；若为
 * 则为 -1。
 */
extern int	extract_variadic_args(FunctionCallInfo fcinfo, int variadic_start,
								  bool convert_unknown, Datum **args,
								  Oid **types, bool **nulls);

#endif							/* FUNCAPI_H */
