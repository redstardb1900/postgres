/*-------------------------------------------------------------------------
 *
 * llvmjit_types.c
 *	  JIT 发码所需的类型列表。
 *
 * JIT 发码常要访问结构体成员、按正确签名创建函数
 * 等。为使这些类型易于与定义同步、降低走样风险，
 * 本文件列出需从 LLVM 直接访问的类型与
 * 函数。
 *
 * backend 首次使用 LLVM 时，会加载本文件的 bitcode
 * 版本。所需类型与签名存入 Struct*、
 * Type*、Func* 变量。
 *
 * 注意：本文件不会链进服务器，只是转成 bitcode。
 *
 *
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_types.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_attribute.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/memnodes.h"
#include "utils/expandeddatum.h"
#include "utils/palloc.h"


/*
 * JITing 所需类型列表。必须非 static，否则
 * clang/LLVM 会省略它们。本文件永不链进任何
 * 东西，故无害。
 */
PGFunction	TypePGFunction;
size_t		TypeSizeT;
bool		TypeStorageBool;

ExecEvalSubroutine TypeExecEvalSubroutine;
ExecEvalBoolSubroutine TypeExecEvalBoolSubroutine;

NullableDatum StructNullableDatum;
AggState	StructAggState;
AggStatePerGroupData StructAggStatePerGroupData;
AggStatePerTransData StructAggStatePerTransData;
ExprContext StructExprContext;
ExprEvalStep StructExprEvalStep;
ExprState	StructExprState;
FunctionCallInfoBaseData StructFunctionCallInfoData;
HeapTupleData StructHeapTupleData;
HeapTupleHeaderData StructHeapTupleHeaderData;
MemoryContextData StructMemoryContextData;
TupleTableSlot StructTupleTableSlot;
HeapTupleTableSlot StructHeapTupleTableSlot;
MinimalTupleTableSlot StructMinimalTupleTableSlot;
TupleDescData StructTupleDescData;
PlanState	StructPlanState;
MinimalTupleData StructMinimalTupleData;


/*
 * 为判定函数须带哪些属性（取决于编译器版本
 * 与设置等）才能兼容内联，我们只是
 * 复制本函数的属性。
 */
extern Datum AttributeTemplate(PG_FUNCTION_ARGS);
Datum
AttributeTemplate(PG_FUNCTION_ARGS)
{
	AssertVariableIsOfType(&AttributeTemplate, PGFunction);

	PG_RETURN_NULL();
}

/*
 * 再给一些"templates"，作为函数指针类型所对应
 * 函数类型的示例。
 */

extern void ExecEvalSubroutineTemplate(ExprState *state,
									   struct ExprEvalStep *op,
									   ExprContext *econtext);
void
ExecEvalSubroutineTemplate(ExprState *state,
						   struct ExprEvalStep *op,
						   ExprContext *econtext)
{
	AssertVariableIsOfType(&ExecEvalSubroutineTemplate,
						   ExecEvalSubroutine);
}

extern bool ExecEvalBoolSubroutineTemplate(ExprState *state,
										   struct ExprEvalStep *op,
										   ExprContext *econtext);
bool
ExecEvalBoolSubroutineTemplate(ExprState *state,
							   struct ExprEvalStep *op,
							   ExprContext *econtext)
{
	AssertVariableIsOfType(&ExecEvalBoolSubroutineTemplate,
						   ExecEvalBoolSubroutine);

	return false;
}

/*
 * Clang 对函数返回的 stdbool.h 风格布尔
 * 表示成 i1，与存储的（i8）不同。因此不但需要
 * TypeBool（见上），还要能判定返回整数的
 * 宽度。这样可与不用 stdbool 的架构
 * 兼容。
 */
extern bool FunctionReturningBool(void);
bool
FunctionReturningBool(void)
{
	return false;
}

/*
 * 为强制 JITing 所用函数的签名存在，
 * 引用所需函数。同样必须非 static，以免被
 * 当作无用而删掉。
 */
void	   *referenced_functions[] =
{
	ExecAggInitGroup,
	ExecAggCopyTransValue,
	ExecEvalPreOrderedDistinctSingle,
	ExecEvalPreOrderedDistinctMulti,
	ExecEvalAggOrderedTransDatum,
	ExecEvalAggOrderedTransTuple,
	ExecEvalArrayCoerce,
	ExecEvalArrayExpr,
	ExecEvalConstraintCheck,
	ExecEvalConstraintNotNull,
	ExecEvalConvertRowtype,
	ExecEvalCurrentOfExpr,
	ExecEvalFieldSelect,
	ExecEvalFieldStoreDeForm,
	ExecEvalFieldStoreForm,
	ExecEvalFuncExprFusage,
	ExecEvalFuncExprStrictFusage,
	ExecEvalGroupingFunc,
	ExecEvalMinMax,
	ExecEvalNextValueExpr,
	ExecEvalParamExec,
	ExecEvalParamExtern,
	ExecEvalRow,
	ExecEvalRowNotNull,
	ExecEvalRowNull,
	ExecEvalSQLValueFunction,
	ExecEvalScalarArrayOp,
	ExecEvalHashedScalarArrayOp,
	ExecEvalSubPlan,
	ExecEvalSysVar,
	ExecEvalWholeRowVar,
	ExecEvalXmlExpr,
	ExecEvalJsonConstructor,
	ExecEvalJsonIsPredicate,
	MakeExpandedObjectReadOnlyInternal,
	slot_getmissingattrs,
	slot_getsomeattrs_int,
	strlen,
	varsize_any,
	ExecInterpExprStillValid,
};
