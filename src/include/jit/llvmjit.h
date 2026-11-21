/*-------------------------------------------------------------------------
 * llvmjit.h
 *	  LLVM JIT。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/jit/llvmjit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LLVMJIT_H
#define LLVMJIT_H

/*
 * 为免破坏 cpluspluscheck，即使没有 LLVM
 * 也允许。
 */
#ifdef USE_LLVM

#include "jit/llvmjit_backport.h"

#include <llvm-c/Types.h>
#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
#include <llvm-c/OrcEE.h>
#endif


/*
 * 本文件须可被 C 与 C++ 代码包含，且所含其他头
 * 亦然。故在 C++ 模式下用自有 extern "C" 包裹
 * C 部分。
 */
#ifdef __cplusplus
extern "C"
{
#endif

#include "access/tupdesc.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "nodes/pg_list.h"

typedef struct LLVMJitContext
{
	JitContext	base;

	/*  已创建的模块数  */
	size_t		module_generation;

	/*
	 * 本 JIT 上下文所用的 LLVM Context。一个 LLVM 上下文可
	 * 跨多次编译复用，但偶发重置以防因类型不断
	 * 累积而占用过多内存。
	 */
	LLVMContextRef llvm_context;

	/*  当前 "可写" 模块  */
	LLVMModuleRef module;

	/*  是否有待发出的代码  */
	bool		compiled;

	/*  已发出对象数，用于生成不冲突的名字  */
	int			counter;

	/*  经 Orc 发出代码的句柄列表  */
	List	   *handles;
} LLVMJitContext;

/*  含类型信息的 llvm 模块  */
extern PGDLLIMPORT LLVMModuleRef llvm_types_module;

/*  类型与结构体定义  */
extern PGDLLIMPORT LLVMTypeRef TypeParamBool;
extern PGDLLIMPORT LLVMTypeRef TypePGFunction;
extern PGDLLIMPORT LLVMTypeRef TypeSizeT;
extern PGDLLIMPORT LLVMTypeRef TypeStorageBool;

extern PGDLLIMPORT LLVMTypeRef StructNullableDatum;
extern PGDLLIMPORT LLVMTypeRef StructTupleDescData;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleData;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleHeaderData;
extern PGDLLIMPORT LLVMTypeRef StructMinimalTupleData;
extern PGDLLIMPORT LLVMTypeRef StructTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructMinimalTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructMemoryContextData;
extern PGDLLIMPORT LLVMTypeRef StructFunctionCallInfoData;
extern PGDLLIMPORT LLVMTypeRef StructExprContext;
extern PGDLLIMPORT LLVMTypeRef StructExprEvalStep;
extern PGDLLIMPORT LLVMTypeRef StructExprState;
extern PGDLLIMPORT LLVMTypeRef StructAggState;
extern PGDLLIMPORT LLVMTypeRef StructAggStatePerTransData;
extern PGDLLIMPORT LLVMTypeRef StructAggStatePerGroupData;
extern PGDLLIMPORT LLVMTypeRef StructPlanState;

extern PGDLLIMPORT LLVMValueRef AttributeTemplate;
extern PGDLLIMPORT LLVMValueRef ExecEvalBoolSubroutineTemplate;
extern PGDLLIMPORT LLVMValueRef ExecEvalSubroutineTemplate;


extern void llvm_enter_fatal_on_oom(void);
extern void llvm_leave_fatal_on_oom(void);
extern bool llvm_in_fatal_on_oom(void);
extern void llvm_reset_after_error(void);
extern void llvm_assert_in_fatal_section(void);

extern LLVMJitContext *llvm_create_context(int jitFlags);
extern LLVMModuleRef llvm_mutable_module(LLVMJitContext *context);
extern char *llvm_expand_funcname(LLVMJitContext *context, const char *basename);
extern void *llvm_get_function(LLVMJitContext *context, const char *funcname);
extern void llvm_split_symbol_name(const char *name, char **modname, char **funcname);
extern LLVMTypeRef llvm_pg_var_type(const char *varname);
extern LLVMTypeRef llvm_pg_var_func_type(const char *varname);
extern LLVMValueRef llvm_pg_func(LLVMModuleRef mod, const char *funcname);
extern void llvm_copy_attributes(LLVMValueRef from, LLVMValueRef to);
extern LLVMValueRef llvm_function_reference(LLVMJitContext *context,
						LLVMBuilderRef builder,
						LLVMModuleRef mod,
						FunctionCallInfo fcinfo);

extern void llvm_inline_reset_caches(void);
extern void llvm_inline(LLVMModuleRef mod);

/*
 ****************************************************************************
 * 代码生成函数。
 ****************************************************************************
 */
extern bool llvm_compile_expr(struct ExprState *state);
struct TupleTableSlotOps;
extern LLVMValueRef slot_compile_deform(struct LLVMJitContext *context, TupleDesc desc,
										const struct TupleTableSlotOps *ops, int natts);

/*
 ****************************************************************************
 * LLVM C API 的扩展/向后兼容部分
 * 错误处理相关函数。
 ****************************************************************************
 */
#if defined(HAVE_DECL_LLVMGETHOSTCPUNAME) && !HAVE_DECL_LLVMGETHOSTCPUNAME
/** 以字符串取得主机 CPU。结果须用
  LLVMDisposeMessage. */
extern char *LLVMGetHostCPUName(void);
#endif

#if defined(HAVE_DECL_LLVMGETHOSTCPUFEATURES) && !HAVE_DECL_LLVMGETHOSTCPUFEATURES
/** 以字符串取得主机 CPU 特性。结果须用
  with LLVMDisposeMessage. */
extern char *LLVMGetHostCPUFeatures(void);
#endif

extern unsigned LLVMGetAttributeCountAtIndexPG(LLVMValueRef F, uint32 Idx);
extern LLVMTypeRef LLVMGetFunctionReturnType(LLVMValueRef r);
extern LLVMTypeRef LLVMGetFunctionType(LLVMValueRef r);
#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
extern LLVMOrcObjectLayerRef LLVMOrcCreateRTDyldObjectLinkingLayerWithSafeSectionMemoryManager(LLVMOrcExecutionSessionRef ES);
#endif

#if LLVM_MAJOR_VERSION < 8
extern LLVMTypeRef LLVMGlobalGetValueType(LLVMValueRef g);
#endif

#ifdef __cplusplus
} /* 。 */
#endif

#endif							/* USE_LLVM */
#endif							/* LLVMJIT_H */
