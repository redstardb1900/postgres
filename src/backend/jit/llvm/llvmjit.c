/*-------------------------------------------------------------------------
 *
 * llvmjit.c
 *	  LLVM JIT 提供方的核心。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#if LLVM_VERSION_MAJOR > 16
#include <llvm-c/Transforms/PassBuilder.h>
#endif
#if LLVM_VERSION_MAJOR > 11
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/LLJIT.h>
#else
#include <llvm-c/OrcBindings.h>
#endif
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#if LLVM_VERSION_MAJOR < 17
#include <llvm-c/Transforms/IPO.h>
#include <llvm-c/Transforms/PassManagerBuilder.h>
#include <llvm-c/Transforms/Scalar.h>
#if LLVM_VERSION_MAJOR > 6
#include <llvm-c/Transforms/Utils.h>
#endif
#endif

#include "jit/llvmjit.h"
#include "jit/llvmjit_backport.h"
#include "jit/llvmjit_emit.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"

#define LLVMJIT_LLVM_CONTEXT_REUSE_MAX 100

/* 经 ORC JIT 发出的模块句柄 */
typedef struct LLVMJitHandle
{
#if LLVM_VERSION_MAJOR > 11
	LLVMOrcLLJITRef lljit;
	LLVMOrcResourceTrackerRef resource_tracker;
#else
	LLVMOrcJITStackRef stack;
	LLVMOrcModuleHandle orc_handle;
#endif
} LLVMJitHandle;


/* JITing 常用的类型与函数 */
LLVMTypeRef TypeSizeT;
LLVMTypeRef TypeParamBool;
LLVMTypeRef TypeStorageBool;
LLVMTypeRef TypePGFunction;
LLVMTypeRef StructNullableDatum;
LLVMTypeRef StructHeapTupleFieldsField3;
LLVMTypeRef StructHeapTupleFields;
LLVMTypeRef StructHeapTupleHeaderData;
LLVMTypeRef StructHeapTupleDataChoice;
LLVMTypeRef StructHeapTupleData;
LLVMTypeRef StructMinimalTupleData;
LLVMTypeRef StructItemPointerData;
LLVMTypeRef StructBlockId;
LLVMTypeRef StructFormPgAttribute;
LLVMTypeRef StructTupleConstr;
LLVMTypeRef StructTupleDescData;
LLVMTypeRef StructTupleTableSlot;
LLVMTypeRef StructHeapTupleTableSlot;
LLVMTypeRef StructMinimalTupleTableSlot;
LLVMTypeRef StructMemoryContextData;
LLVMTypeRef StructPGFinfoRecord;
LLVMTypeRef StructFmgrInfo;
LLVMTypeRef StructFunctionCallInfoData;
LLVMTypeRef StructExprContext;
LLVMTypeRef StructExprEvalStep;
LLVMTypeRef StructExprState;
LLVMTypeRef StructAggState;
LLVMTypeRef StructAggStatePerGroupData;
LLVMTypeRef StructAggStatePerTransData;
LLVMTypeRef StructPlanState;

LLVMValueRef AttributeTemplate;
LLVMValueRef ExecEvalSubroutineTemplate;
LLVMValueRef ExecEvalBoolSubroutineTemplate;

LLVMModuleRef llvm_types_module = NULL;

static bool llvm_session_initialized = false;
static size_t llvm_generation = 0;

/* 当前仍在使用的 LLVMJitContext 个数 */
static size_t llvm_jit_context_in_use_count = 0;

/* 当前 LLVMContextRef 已被复用多少次 */
static size_t llvm_llvm_context_reuse_count = 0;
static const char *llvm_triple = NULL;
static const char *llvm_layout = NULL;
static LLVMContextRef llvm_context;


static LLVMTargetRef llvm_targetref;
#if LLVM_VERSION_MAJOR > 11
static LLVMOrcThreadSafeContextRef llvm_ts_context;
static LLVMOrcLLJITRef llvm_opt0_orc;
static LLVMOrcLLJITRef llvm_opt3_orc;
#else							/* LLVM_VERSION_MAJOR > 11 */
static LLVMOrcJITStackRef llvm_opt0_orc;
static LLVMOrcJITStackRef llvm_opt3_orc;
#endif							/* LLVM_VERSION_MAJOR > 11 */


static void llvm_release_context(JitContext *context);
static void llvm_session_initialize(void);
static void llvm_shutdown(int code, Datum arg);
static void llvm_compile_module(LLVMJitContext *context);
static void llvm_optimize_module(LLVMJitContext *context, LLVMModuleRef module);

static void llvm_create_types(void);
static void llvm_set_target(void);
static void llvm_recreate_llvm_context(void);
static uint64_t llvm_resolve_symbol(const char *name, void *ctx);

#if LLVM_VERSION_MAJOR > 11
static LLVMOrcLLJITRef llvm_create_jit_instance(LLVMTargetMachineRef tm);
static char *llvm_error_message(LLVMErrorRef error);
#endif							/* LLVM_VERSION_MAJOR > 11 */

PG_MODULE_MAGIC;


/*
 * 初始化 LLVM JIT 提供方
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = llvm_reset_after_error;
	cb->release_context = llvm_release_context;
	cb->compile_expr = llvm_compile_expr;
}


/*
 * 偶尔新建 LLVMContextRef。不幸的是，每轮内联
 * 类型可能"泄漏"（仍可经上下文找到/使用，但下次
 * 再内联会创建新类型）。为免内存积到成问题的
 * 程度，重建所用的 LLVMContextRef。不宜太勤，因为
 * 有开销（尤其重新加载模块摘要/模块相当贵）。
 * 将来 TODO：更细：仅在确知发生过内联时才丢弃/
 * 重建 LLVMContextRef。若能从 LLVM 取得上下文大小，
 * 用它决定何时丢弃/重建，会优于现在的
 * usagecount 启发式。
 *
 *
 */
static void
llvm_recreate_llvm_context(void)
{
	if (!llvm_context)
		elog(ERROR, "Trying to recreate a non-existing context");

	/*
	 * 仅当没有其他代码正在 JIT 时才能安全重建 LLVM
	 * 上下文，否则会释放那些正在用的类型。
	 */
	if (llvm_jit_context_in_use_count > 0)
	{
		llvm_llvm_context_reuse_count++;
		return;
	}

	if (llvm_llvm_context_reuse_count <= LLVMJIT_LLVM_CONTEXT_REUSE_MAX)
	{
		llvm_llvm_context_reuse_count++;
		return;
	}

	/*
	 * 丢弃上下文前须重置内联代码所缓存的模块。
	 * LLVM 模块存在于特定 LLVM 上下文中，先丢上下文
	 * 再清缓存会留下指向模块的悬空指针。
	 *
	 */
	llvm_inline_reset_caches();

	LLVMContextDispose(llvm_context);
	llvm_context = LLVMContextCreate();
	llvm_llvm_context_reuse_count = 0;

	/*
	 * 重建缓存的类型信息，让发码代码可以依赖其
	 * 存在（也避免变量变成悬空引用）。
	 *
	 */
	llvm_create_types();
}


/*
 * 为 JIT 工作创建上下文。
 *
 * 上下文及其附属资源会在显式释放时、或
 * CurrentResourceOwner 生命结束时（通常是当前
 * [子]事务结束）清理。
 */
LLVMJitContext *
llvm_create_context(int jitFlags)
{
	LLVMJitContext *context;

	llvm_assert_in_fatal_section();

	llvm_session_initialize();

	llvm_recreate_llvm_context();

	ResourceOwnerEnlargeJIT(CurrentResourceOwner);

	context = MemoryContextAllocZero(TopMemoryContext,
									 sizeof(LLVMJitContext));
	context->base.flags = jitFlags;

	/* 确保清理 */
	context->base.resowner = CurrentResourceOwner;
	ResourceOwnerRememberJIT(CurrentResourceOwner, PointerGetDatum(context));

	llvm_jit_context_in_use_count++;

	return context;
}

/*
 * 释放一个 llvm 上下文所需的资源。
 */
static void
llvm_release_context(JitContext *context)
{
	LLVMJitContext *llvm_jit_context = (LLVMJitContext *) context;
	ListCell   *lc;

	/*
	 * 即便下面跳过清理也视为已清，以便核对跟踪是否
	 * 正确（见 llvm_shutdown()）。
	 */
	llvm_jit_context_in_use_count--;

	/*
	 * 本 backend 正在退出时不要清理 LLVM。错误可能
	 * 源自 LLVM 内部，不想冒险重入。资源清理将随
	 * 进程退出完成。
	 */
	if (proc_exit_inprogress)
		return;

	llvm_enter_fatal_on_oom();

	if (llvm_jit_context->module)
	{
		LLVMDisposeModule(llvm_jit_context->module);
		llvm_jit_context->module = NULL;
	}

	foreach(lc, llvm_jit_context->handles)
	{
		LLVMJitHandle *jit_handle = (LLVMJitHandle *) lfirst(lc);

#if LLVM_VERSION_MAJOR > 11
		{
			LLVMOrcExecutionSessionRef ee;
			LLVMOrcSymbolStringPoolRef sp;

			LLVMOrcResourceTrackerRemove(jit_handle->resource_tracker);
			LLVMOrcReleaseResourceTracker(jit_handle->resource_tracker);

			/*
			 * 不触发字符串池清理会漏内存。其实不必
			 * 每次都做，但实验里耗时很小，故一律做。
			 *
			 *
			 */
			ee = LLVMOrcLLJITGetExecutionSession(jit_handle->lljit);
			sp = LLVMOrcExecutionSessionGetSymbolStringPool(ee);
			LLVMOrcSymbolStringPoolClearDeadEntries(sp);
		}
#else							/* LLVM_VERSION_MAJOR > 11 */
		{
			LLVMOrcRemoveModule(jit_handle->stack, jit_handle->orc_handle);
		}
#endif							/* LLVM_VERSION_MAJOR > 11 */

		pfree(jit_handle);
	}
	list_free(llvm_jit_context->handles);
	llvm_jit_context->handles = NIL;

	llvm_leave_fatal_on_oom();
}

/*
 * 返回可修改的模块，例如可在其中创建新函数。
 */
LLVMModuleRef
llvm_mutable_module(LLVMJitContext *context)
{
	llvm_assert_in_fatal_section();

	/*
	 * 若没有进行中的模块，则新建一个。
	 */
	if (!context->module)
	{
		context->compiled = false;
		context->module_generation = llvm_generation++;
		context->module = LLVMModuleCreateWithNameInContext("pg", llvm_context);
		LLVMSetTarget(context->module, llvm_triple);
		LLVMSetDataLayout(context->module, llvm_layout);
	}

	return context->module;
}

/*
 * 把函数名扩成不冲突的。发码时向模块添加新的
 * 对外可见函数定义，应使用本函数。
 *
 */
char *
llvm_expand_funcname(struct LLVMJitContext *context, const char *basename)
{
	Assert(context->module != NULL);

	context->base.instr.created_functions++;

	/*
	 * 以前用点分隔，但有些工具（如 GDB）不喜欢，
	 * 会截断名字。
	 */
	return psprintf("%s_%zu_%d",
					basename,
					context->module_generation,
					context->counter++);
}

/*
 * 返回必须存在的函数 funcname 的指针。若有待优化
 * 并发出的代码，先做完。
 */
void *
llvm_get_function(LLVMJitContext *context, const char *funcname)
{
#if LLVM_VERSION_MAJOR > 11 || \
	defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
	ListCell   *lc;
#endif

	llvm_assert_in_fatal_section();

	/*
	 * 若有挂起/未发出的模块，现在编译并发出。
	 * 否则可能找不到正确的函数。
	 */
	if (!context->compiled)
	{
		llvm_compile_module(context);
	}

	/*
	 * ORC 符号表是未修饰符号，故此处不必修饰。
	 *
	 */

#if LLVM_VERSION_MAJOR > 11
	foreach(lc, context->handles)
	{
		LLVMJitHandle *handle = (LLVMJitHandle *) lfirst(lc);
		instr_time	starttime;
		instr_time	endtime;
		LLVMErrorRef error;
		LLVMOrcJITTargetAddress addr;

		INSTR_TIME_SET_CURRENT(starttime);

		addr = 0;
		error = LLVMOrcLLJITLookup(handle->lljit, &addr, funcname);
		if (error)
			elog(ERROR, "failed to look up symbol \"%s\": %s",
				 funcname, llvm_error_message(error));

		/*
		 * LLJIT 仅在首次引用符号时才发码。故查找
		 * 时间计入发出时间。比旧 LLVM 多计一点，但不太
		 * 会要紧。
		 *
		 */
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(context->base.instr.emission_counter,
							  endtime, starttime);

		if (addr)
			return (void *) (uintptr_t) addr;
	}
#elif defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
	foreach(lc, context->handles)
	{
		LLVMOrcTargetAddress addr;
		LLVMJitHandle *handle = (LLVMJitHandle *) lfirst(lc);

		addr = 0;
		if (LLVMOrcGetSymbolAddressIn(handle->stack, &addr, handle->orc_handle, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
	}
#elif LLVM_VERSION_MAJOR < 5
	{
		LLVMOrcTargetAddress addr;

		if ((addr = LLVMOrcGetSymbolAddress(llvm_opt0_orc, funcname)))
			return (void *) (uintptr_t) addr;
		if ((addr = LLVMOrcGetSymbolAddress(llvm_opt3_orc, funcname)))
			return (void *) (uintptr_t) addr;
	}
#else
	{
		LLVMOrcTargetAddress addr;

		if (LLVMOrcGetSymbolAddress(llvm_opt0_orc, &addr, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
		if (LLVMOrcGetSymbolAddress(llvm_opt3_orc, &addr, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
	}
#endif

	elog(ERROR, "failed to JIT: %s", funcname);

	return NULL;
}

/*
 * 返回 llvmjit_types.c 中某变量的类型。便于让普通 C
 * 与 JIT 相关代码的类型保持同步。
 */
LLVMTypeRef
llvm_pg_var_type(const char *varname)
{
	LLVMValueRef v_srcvar;
	LLVMTypeRef typ;

	/* 这会返回指向该全局的指针 */
	v_srcvar = LLVMGetNamedGlobal(llvm_types_module, varname);
	if (!v_srcvar)
		elog(ERROR, "variable %s not in llvmjit_types.c", varname);

	typ = LLVMGlobalGetValueType(v_srcvar);

	return typ;
}

/*
 * 返回 llvmjit_types.c 中某变量的函数类型。便于让 C
 * 与 JIT 代码的函数类型保持同步。
 */
LLVMTypeRef
llvm_pg_var_func_type(const char *varname)
{
	LLVMValueRef v_srcvar;
	LLVMTypeRef typ;

	v_srcvar = LLVMGetNamedFunction(llvm_types_module, varname);
	if (!v_srcvar)
		elog(ERROR, "function %s not in llvmjit_types.c", varname);

	typ = LLVMGetFunctionType(v_srcvar);

	return typ;
}

/*
 * 返回 llvmjit_types.c 中引用的函数声明，必要时加到
 * 模块中。
 *
 * 用来让 llvm_create_types() 发现的函数为当前正在
 * 处理的模块所知。
 */
LLVMValueRef
llvm_pg_func(LLVMModuleRef mod, const char *funcname)
{
	LLVMValueRef v_srcfn;
	LLVMValueRef v_fn;

	/* 不要重复添加函数 */
	v_fn = LLVMGetNamedFunction(mod, funcname);
	if (v_fn)
		return v_fn;

	v_srcfn = LLVMGetNamedFunction(llvm_types_module, funcname);

	if (!v_srcfn)
		elog(ERROR, "function %s not in llvmjit_types.c", funcname);

	v_fn = LLVMAddFunction(mod,
						   funcname,
						   LLVMGetFunctionType(v_srcfn));
	llvm_copy_attributes(v_srcfn, v_fn);

	return v_fn;
}

/*
 * 按指定索引把属性从一函数拷到另一函数（索引可
 * 指返回值、函数及参数属性）。
 */
static void
llvm_copy_attributes_at_index(LLVMValueRef v_from, LLVMValueRef v_to, uint32 index)
{
	int			num_attributes;
	LLVMAttributeRef *attrs;

	num_attributes = LLVMGetAttributeCountAtIndexPG(v_from, index);

	/*
	 * 不只为效率：LLVM <= 3.9 在
	 * 对 0 属性的索引调 LLVMGetAttributesAtIndex() 会崩。
	 */
	if (num_attributes == 0)
		return;

	attrs = palloc(sizeof(LLVMAttributeRef) * num_attributes);
	LLVMGetAttributesAtIndex(v_from, index, attrs);

	for (int attno = 0; attno < num_attributes; attno++)
		LLVMAddAttributeAtIndex(v_to, index, attrs[attno]);

	pfree(attrs);
}

/*
 * 拷贝一函数的全部属性，即函数、返回值与参数。
 *
 */
void
llvm_copy_attributes(LLVMValueRef v_from, LLVMValueRef v_to)
{
	uint32		param_count;

	/* 拷贝函数属性 */
	llvm_copy_attributes_at_index(v_from, v_to, LLVMAttributeFunctionIndex);

	if (LLVMGetTypeKind(LLVMGetFunctionReturnType(v_to)) != LLVMVoidTypeKind)
	{
		/* 以及返回值属性 */
		llvm_copy_attributes_at_index(v_from, v_to, LLVMAttributeReturnIndex);
	}

	/* 以及每个函数参数的属性 */
	param_count = LLVMCountParams(v_from);

	for (int paramidx = 1; paramidx <= param_count; paramidx++)
		llvm_copy_attributes_at_index(v_from, v_to, paramidx);
}

/*
 * 返回 fcinfo 可调用 LLVMValueRef。
 */
LLVMValueRef
llvm_function_reference(LLVMJitContext *context,
						LLVMBuilderRef builder,
						LLVMModuleRef mod,
						FunctionCallInfo fcinfo)
{
	char	   *modname;
	char	   *basename;
	char	   *funcname;

	LLVMValueRef v_fn;

	fmgr_symbol(fcinfo->flinfo->fn_oid, &modname, &basename);

	if (modname != NULL && basename != NULL)
	{
		/* 可加载库中的外部函数 */
		funcname = psprintf("pgextern.%s.%s", modname, basename);
	}
	else if (basename != NULL)
	{
		/* 内部函数 */
		funcname = pstrdup(basename);
	}
	else
	{
		/*
		 * 不知如何处理的函数则返回指针。做法是建含
		 * 函数指针的全局常量。IR 更好读。
		 *
		 */
		LLVMValueRef v_fn_addr;

		funcname = psprintf("pgoidextern.%u",
							fcinfo->flinfo->fn_oid);
		v_fn = LLVMGetNamedGlobal(mod, funcname);
		if (v_fn != 0)
			return l_load(builder, TypePGFunction, v_fn, "");

		v_fn_addr = l_ptr_const(fcinfo->flinfo->fn_addr, TypePGFunction);

		v_fn = LLVMAddGlobal(mod, TypePGFunction, funcname);
		LLVMSetInitializer(v_fn, v_fn_addr);
		LLVMSetGlobalConstant(v_fn, true);
		LLVMSetLinkage(v_fn, LLVMPrivateLinkage);
		LLVMSetUnnamedAddr(v_fn, true);

		return l_load(builder, TypePGFunction, v_fn, "");
	}

	/* 检查函数是否已添加 */
	v_fn = LLVMGetNamedFunction(mod, funcname);
	if (v_fn != 0)
		return v_fn;

	v_fn = LLVMAddFunction(mod, funcname, LLVMGetFunctionType(AttributeTemplate));

	return v_fn;
}

/*
 * 按上下文中的标志优化模块中的代码。
 */
static void
llvm_optimize_module(LLVMJitContext *context, LLVMModuleRef module)
{
#if LLVM_VERSION_MAJOR < 17
	LLVMPassManagerBuilderRef llvm_pmb;
	LLVMPassManagerRef llvm_mpm;
	LLVMPassManagerRef llvm_fpm;
	LLVMValueRef func;
	int			compile_optlevel;

	if (context->base.flags & PGJIT_OPT3)
		compile_optlevel = 3;
	else
		compile_optlevel = 0;

	/*
	 * 每次通过都须新建 PassManagerBuilder，因内联器
	 * 有每 builder 状态。否则一个函数只会在第一次被
	 * 内联。
	 */
	llvm_pmb = LLVMPassManagerBuilderCreate();
	LLVMPassManagerBuilderSetOptLevel(llvm_pmb, compile_optlevel);
	llvm_fpm = LLVMCreateFunctionPassManagerForModule(module);

	if (context->base.flags & PGJIT_OPT3)
	{
		/* TODO：阈值并非科学测定 */
		LLVMPassManagerBuilderUseInlinerWithThreshold(llvm_pmb, 512);
	}
	else
	{
		/* 我们重度依赖 mem2reg，故 O0 也发出 */
		LLVMAddPromoteMemoryToRegisterPass(llvm_fpm);
	}

	LLVMPassManagerBuilderPopulateFunctionPassManager(llvm_pmb, llvm_fpm);

	/*
	 * 做函数级优化。可挪到发函数时做，以略减内存。
	 *
	 */
	LLVMInitializeFunctionPassManager(llvm_fpm);
	for (func = LLVMGetFirstFunction(context->module);
		 func != NULL;
		 func = LLVMGetNextFunction(func))
		LLVMRunFunctionPassManager(llvm_fpm, func);
	LLVMFinalizeFunctionPassManager(llvm_fpm);
	LLVMDisposePassManager(llvm_fpm);

	/*
	 * 做模块级优化。即便非优化情形也做，以便
	 * always-inline 等函数被内联。足够便宜。
	 */
	llvm_mpm = LLVMCreatePassManager();
	LLVMPassManagerBuilderPopulateModulePassManager(llvm_pmb,
													llvm_mpm);
	/* 总用 always-inliner 遍 */
	if (!(context->base.flags & PGJIT_OPT3))
		LLVMAddAlwaysInlinerPass(llvm_mpm);
	/* 若做内联但不做昂贵优化，则加内联遍 */
	if (context->base.flags & PGJIT_INLINE
		&& !(context->base.flags & PGJIT_OPT3))
		LLVMAddFunctionInliningPass(llvm_mpm);
	LLVMRunPassManager(llvm_mpm, context->module);
	LLVMDisposePassManager(llvm_mpm);

	LLVMPassManagerBuilderDispose(llvm_pmb);
#else
	LLVMPassBuilderOptionsRef options;
	LLVMErrorRef err;
	const char *passes;

	if (context->base.flags & PGJIT_OPT3)
		passes = "default<O3>";
	else
		passes = "default<O0>,mem2reg";

	options = LLVMCreatePassBuilderOptions();

#ifdef LLVM_PASS_DEBUG
	LLVMPassBuilderOptionsSetDebugLogging(options, 1);
#endif

	LLVMPassBuilderOptionsSetInlinerThreshold(options, 512);

	err = LLVMRunPasses(module, passes, NULL, options);

	if (err)
		elog(ERROR, "failed to JIT module: %s", llvm_error_message(err));

	LLVMDisposePassBuilderOptions(options);
#endif
}

/*
 * 为当前挂起的模块发码。
 */
static void
llvm_compile_module(LLVMJitContext *context)
{
	LLVMJitHandle *handle;
	MemoryContext oldcontext;
	instr_time	starttime;
	instr_time	endtime;
#if LLVM_VERSION_MAJOR > 11
	LLVMOrcLLJITRef compile_orc;
#else
	LLVMOrcJITStackRef compile_orc;
#endif

	if (context->base.flags & PGJIT_OPT3)
		compile_orc = llvm_opt3_orc;
	else
		compile_orc = llvm_opt0_orc;

	/* 执行内联 */
	if (context->base.flags & PGJIT_INLINE)
	{
		INSTR_TIME_SET_CURRENT(starttime);
		llvm_inline(context->module);
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(context->base.instr.inlining_counter,
							  endtime, starttime);
	}

	if (jit_dump_bitcode)
	{
		char	   *filename;

		filename = psprintf("%d.%zu.bc",
							MyProcPid,
							context->module_generation);
		LLVMWriteBitcodeToFile(context->module, filename);
		pfree(filename);
	}


	/* 按所选优化设置进行优化 */
	INSTR_TIME_SET_CURRENT(starttime);
	llvm_optimize_module(context, context->module);
	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.optimization_counter,
						  endtime, starttime);

	if (jit_dump_bitcode)
	{
		char	   *filename;

		filename = psprintf("%d.%zu.optimized.bc",
							MyProcPid,
							context->module_generation);
		LLVMWriteBitcodeToFile(context->module, filename);
		pfree(filename);
	}

	handle = (LLVMJitHandle *)
		MemoryContextAlloc(TopMemoryContext, sizeof(LLVMJitHandle));

	/*
	 * 发码。注意：视优化，发码会跑低级指令合并/
	 * 选择等遍，可能明显耗资源。无优化时用更快的
	 * 指令选择机制。
	 *
	 */
	INSTR_TIME_SET_CURRENT(starttime);
#if LLVM_VERSION_MAJOR > 11
	{
		LLVMOrcThreadSafeModuleRef ts_module;
		LLVMErrorRef error;
		LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(compile_orc);

		ts_module = LLVMOrcCreateNewThreadSafeModule(context->module, llvm_ts_context);

		handle->lljit = compile_orc;
		handle->resource_tracker = LLVMOrcJITDylibCreateResourceTracker(jd);

		/*
		 * 注意：这并不真正发码。模块中某符号首次被请求
		 * 时才惰性发出。因此 llvm_get_function() 也把发出
		 * 时间计入。
		 */

		context->module = NULL; /* 将由 LLJIT 拥有 */
		error = LLVMOrcLLJITAddLLVMIRModuleWithRT(compile_orc,
												  handle->resource_tracker,
												  ts_module);

		if (error)
			elog(ERROR, "failed to JIT module: %s",
				 llvm_error_message(error));

		handle->lljit = compile_orc;

		/* LLVMOrcLLJITAddLLVMIRModuleWithRT 取得模块所有权 */
	}
#elif LLVM_VERSION_MAJOR > 6
	{
		handle->stack = compile_orc;
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &handle->orc_handle, context->module,
										llvm_resolve_symbol, NULL))
			elog(ERROR, "failed to JIT module");

		/* LLVMOrcAddEagerlyCompiledIR 取得模块所有权 */
	}
#elif LLVM_VERSION_MAJOR > 4
	{
		LLVMSharedModuleRef smod;

		smod = LLVMOrcMakeSharedModule(context->module);
		handle->stack = compile_orc;
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &handle->orc_handle, smod,
										llvm_resolve_symbol, NULL))
			elog(ERROR, "failed to JIT module");

		LLVMOrcDisposeSharedModuleRef(smod);
	}
#else							/* LLVM 4.0 与 3.9 */
	{
		handle->stack = compile_orc;
		handle->orc_handle = LLVMOrcAddEagerlyCompiledIR(compile_orc, context->module,
														 llvm_resolve_symbol, NULL);

		LLVMDisposeModule(context->module);
	}
#endif

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.emission_counter,
						  endtime, starttime);

	context->module = NULL;
	context->compiled = true;

	/* 记住已发码，供清理与查找 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	context->handles = lappend(context->handles, handle);
	MemoryContextSwitchTo(oldcontext);

	ereport(DEBUG1,
			(errmsg_internal("time to inline: %.3fs, opt: %.3fs, emit: %.3fs",
							 INSTR_TIME_GET_DOUBLE(context->base.instr.inlining_counter),
							 INSTR_TIME_GET_DOUBLE(context->base.instr.optimization_counter),
							 INSTR_TIME_GET_DOUBLE(context->base.instr.emission_counter)),
			 errhidestmt(true),
			 errhidecontext(true)));
}

/*
 * 每会话初始化。
 */
static void
llvm_session_initialize(void)
{
	MemoryContext oldcontext;
	char	   *error = NULL;
	char	   *cpu = NULL;
	char	   *features = NULL;
	LLVMTargetMachineRef opt0_tm;
	LLVMTargetMachineRef opt3_tm;

	if (llvm_session_initialized)
		return;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	if (llvm_context == NULL)
	{
		llvm_context = LLVMContextCreate();

		llvm_jit_context_in_use_count = 0;
		llvm_llvm_context_reuse_count = 0;
	}

	/*
	 * 面向 LLVM 15 时，对我们在其中建码的上下文关掉
	 * 不透明指针。其他上下文（如 llvm_ts_context）不必。
	 * IR 一旦生成即自带必要信息。
	 *
	 *
	 * 16 及以上必须用不透明指针，我们有专门代码。
	 *
	 */
#if LLVM_VERSION_MAJOR == 15
	LLVMContextSetOpaquePointers(LLVMGetGlobalContext(), false);
#endif

	/*
	 * 尽早同步类型，其中也包括推断目标三元组。
	 *
	 */
	llvm_create_types();

	/*
	 * 从已加载模块提取目标信息。
	 */
	llvm_set_target();

	if (LLVMGetTargetFromTriple(llvm_triple, &llvm_targetref, &error) != 0)
	{
		elog(FATAL, "failed to query triple %s", error);
	}

	/*
	 * 希望生成代码用上全部可用特性。故取主机 CPU
	 * 字符串并探测当前 CPU 的特性。后者有必要，因为
	 * 有些 CPU 架构默认启用并非所有 CPU 都有的特性
	 * 挺怪。
	 */
	cpu = LLVMGetHostCPUName();
	features = LLVMGetHostCPUFeatures();
	elog(DEBUG2, "LLVMJIT detected CPU \"%s\", with features \"%s\"",
		 cpu, features);

	opt0_tm =
		LLVMCreateTargetMachine(llvm_targetref, llvm_triple, cpu, features,
								LLVMCodeGenLevelNone,
								LLVMRelocDefault,
								LLVMCodeModelJITDefault);
	opt3_tm =
		LLVMCreateTargetMachine(llvm_targetref, llvm_triple, cpu, features,
								LLVMCodeGenLevelAggressive,
								LLVMRelocDefault,
								LLVMCodeModelJITDefault);

	LLVMDisposeMessage(cpu);
	cpu = NULL;
	LLVMDisposeMessage(features);
	features = NULL;

	/* 强制加载主二进制中的符号 */
	LLVMLoadLibraryPermanently(NULL);

#if LLVM_VERSION_MAJOR > 11
	{
		llvm_ts_context = LLVMOrcCreateNewThreadSafeContext();

		llvm_opt0_orc = llvm_create_jit_instance(opt0_tm);
		opt0_tm = 0;

		llvm_opt3_orc = llvm_create_jit_instance(opt3_tm);
		opt3_tm = 0;
	}
#else							/* LLVM_VERSION_MAJOR > 11 */
	{
		llvm_opt0_orc = LLVMOrcCreateInstance(opt0_tm);
		llvm_opt3_orc = LLVMOrcCreateInstance(opt3_tm);

#if defined(HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER) && HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER
		if (jit_debugging_support)
		{
			LLVMJITEventListenerRef l = LLVMCreateGDBRegistrationListener();

			LLVMOrcRegisterJITEventListener(llvm_opt0_orc, l);
			LLVMOrcRegisterJITEventListener(llvm_opt3_orc, l);
		}
#endif
#if defined(HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER) && HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER
		if (jit_profiling_support)
		{
			LLVMJITEventListenerRef l = LLVMCreatePerfJITEventListener();

			LLVMOrcRegisterJITEventListener(llvm_opt0_orc, l);
			LLVMOrcRegisterJITEventListener(llvm_opt3_orc, l);
		}
#endif
	}
#endif							/* LLVM_VERSION_MAJOR > 11 */

	on_proc_exit(llvm_shutdown, 0);

	llvm_session_initialized = true;

	MemoryContextSwitchTo(oldcontext);
}

static void
llvm_shutdown(int code, Datum arg)
{
	/*
	 * 若在 fatal-on-oom 区间内到达 llvm_shutdown()，说明
	 * LLVM 代码中途出错。回调进 LLVM 不安全（这也是
	 * 抛 FATAL 的原因）。
	 *
	 * 其他关机情形仍须关闭 LLVM，否则如剖析数据
	 * 不会写出。
	 */
	if (llvm_in_fatal_on_oom())
	{
		Assert(proc_exit_inprogress);
		return;
	}

	if (llvm_jit_context_in_use_count != 0)
		elog(PANIC, "LLVMJitContext in use count not 0 at exit (is %zu)",
			 llvm_jit_context_in_use_count);

#if LLVM_VERSION_MAJOR > 11
	{
		if (llvm_opt3_orc)
		{
			LLVMOrcDisposeLLJIT(llvm_opt3_orc);
			llvm_opt3_orc = NULL;
		}
		if (llvm_opt0_orc)
		{
			LLVMOrcDisposeLLJIT(llvm_opt0_orc);
			llvm_opt0_orc = NULL;
		}
		if (llvm_ts_context)
		{
			LLVMOrcDisposeThreadSafeContext(llvm_ts_context);
			llvm_ts_context = NULL;
		}
	}
#else							/* LLVM_VERSION_MAJOR > 11 */
	{
		/* 注销剖析支持，须刷新才有用 */

		if (llvm_opt3_orc)
		{
#if defined(HAVE_DECL_LLVMORCREGISTERPERF) && HAVE_DECL_LLVMORCREGISTERPERF
			if (jit_profiling_support)
				LLVMOrcUnregisterPerf(llvm_opt3_orc);
#endif
			LLVMOrcDisposeInstance(llvm_opt3_orc);
			llvm_opt3_orc = NULL;
		}

		if (llvm_opt0_orc)
		{
#if defined(HAVE_DECL_LLVMORCREGISTERPERF) && HAVE_DECL_LLVMORCREGISTERPERF
			if (jit_profiling_support)
				LLVMOrcUnregisterPerf(llvm_opt0_orc);
#endif
			LLVMOrcDisposeInstance(llvm_opt0_orc);
			llvm_opt0_orc = NULL;
		}
	}
#endif							/* LLVM_VERSION_MAJOR > 11 */
}

/* llvm_create_types 的辅助，返回函数的返回类型 */
static LLVMTypeRef
load_return_type(LLVMModuleRef mod, const char *name)
{
	LLVMValueRef value;
	LLVMTypeRef typ;

	/* 这会返回指向该函数的指针 */
	value = LLVMGetNamedFunction(mod, name);
	if (!value)
		elog(ERROR, "function %s is unknown", name);

	typ = LLVMGetFunctionReturnType(value); /* in llvmjit_wrap.cpp */

	return typ;
}

/*
 * 从 clang 发出的文件加载三元组与布局，以保证兼容。
 *
 */
static void
llvm_set_target(void)
{
	if (!llvm_types_module)
		elog(ERROR, "failed to extract target information, llvmjit_types.c not loaded");

	if (llvm_triple == NULL)
		llvm_triple = pstrdup(LLVMGetTarget(llvm_types_module));

	if (llvm_layout == NULL)
		llvm_layout = pstrdup(LLVMGetDataLayoutStr(llvm_types_module));
}

/*
 * 从 llvmjit_types.c 加载所需信息、类型、函数签名，
 * 放到全局变量中。
 *
 * 发码时使用这些全局变量。
 */
static void
llvm_create_types(void)
{
	char		path[MAXPGPATH];
	LLVMMemoryBufferRef buf;
	char	   *msg;

	snprintf(path, MAXPGPATH, "%s/%s", pkglib_path, "llvmjit_types.bc");

	/* 开文件 */
	if (LLVMCreateMemoryBufferWithContentsOfFile(path, &buf, &msg))
	{
		elog(ERROR, "LLVMCreateMemoryBufferWithContentsOfFile(%s) failed: %s",
			 path, msg);
	}

	/* 急切加载全部内容，都会用到 */
	if (LLVMParseBitcodeInContext2(llvm_context, buf, &llvm_types_module))
	{
		elog(ERROR, "LLVMParseBitcodeInContext2 of %s failed", path);
	}
	LLVMDisposeMemoryBuffer(buf);

	TypeSizeT = llvm_pg_var_type("TypeSizeT");
	TypeParamBool = load_return_type(llvm_types_module, "FunctionReturningBool");
	TypeStorageBool = llvm_pg_var_type("TypeStorageBool");
	TypePGFunction = llvm_pg_var_type("TypePGFunction");
	StructNullableDatum = llvm_pg_var_type("StructNullableDatum");
	StructExprContext = llvm_pg_var_type("StructExprContext");
	StructExprEvalStep = llvm_pg_var_type("StructExprEvalStep");
	StructExprState = llvm_pg_var_type("StructExprState");
	StructFunctionCallInfoData = llvm_pg_var_type("StructFunctionCallInfoData");
	StructMemoryContextData = llvm_pg_var_type("StructMemoryContextData");
	StructTupleTableSlot = llvm_pg_var_type("StructTupleTableSlot");
	StructHeapTupleTableSlot = llvm_pg_var_type("StructHeapTupleTableSlot");
	StructMinimalTupleTableSlot = llvm_pg_var_type("StructMinimalTupleTableSlot");
	StructHeapTupleData = llvm_pg_var_type("StructHeapTupleData");
	StructHeapTupleHeaderData = llvm_pg_var_type("StructHeapTupleHeaderData");
	StructTupleDescData = llvm_pg_var_type("StructTupleDescData");
	StructAggState = llvm_pg_var_type("StructAggState");
	StructAggStatePerGroupData = llvm_pg_var_type("StructAggStatePerGroupData");
	StructAggStatePerTransData = llvm_pg_var_type("StructAggStatePerTransData");
	StructPlanState = llvm_pg_var_type("StructPlanState");
	StructMinimalTupleData = llvm_pg_var_type("StructMinimalTupleData");

	AttributeTemplate = LLVMGetNamedFunction(llvm_types_module, "AttributeTemplate");
	ExecEvalSubroutineTemplate = LLVMGetNamedFunction(llvm_types_module, "ExecEvalSubroutineTemplate");
	ExecEvalBoolSubroutineTemplate = LLVMGetNamedFunction(llvm_types_module, "ExecEvalBoolSubroutineTemplate");
}

/*
 * 把符号拆成模块/函数部分。若函数在主二进制（或
 * 外部库）中，*modname 为 NULL。
 */
void
llvm_split_symbol_name(const char *name, char **modname, char **funcname)
{
	*modname = NULL;
	*funcname = NULL;

	/*
	 * 模块函数名是 pgextern.$module.$funcname
	 */
	if (strncmp(name, "pgextern.", strlen("pgextern.")) == 0)
	{
		/*
		 * 符号名不能含"."，故按首末一个点拆。
		 *
		 */
		*funcname = rindex(name, '.');
		(*funcname)++;			/* 跳过点 */

		*modname = pnstrdup(name + strlen("pgextern."),
							*funcname - name - strlen("pgextern.") - 1);
		Assert(funcname);

		*funcname = pstrdup(*funcname);
	}
	else
	{
		*modname = NULL;
		*funcname = pstrdup(name);
	}
}

/*
 * 尝试解析符号，以便 LLVM 能发出对它的引用。
 */
static uint64_t
llvm_resolve_symbol(const char *symname, void *ctx)
{
	uintptr_t	addr;
	char	   *funcname;
	char	   *modname;

	/*
	 * macOS 给所有目标级符号加下划线前缀。但 dlsym()
	 * 与 PG 内联器都不期望。故去掉。
	 */
#if defined(__darwin__)
	if (symname[0] != '_')
		elog(ERROR, "expected prefixed symbol name, but got \"%s\"", symname);
	symname++;
#endif

	llvm_split_symbol_name(symname, &modname, &funcname);

	/* 未解析成名字的函数不应走到这里 */
	Assert(funcname);

	if (modname)
		addr = (uintptr_t) load_external_function(modname, funcname,
												  true, NULL);
	else
		addr = (uintptr_t) LLVMSearchForAddressOfSymbol(symname);

	pfree(funcname);
	if (modname)
		pfree(modname);

	/* 让 LLVM 报错——不应发生 */
	if (!addr)
		elog(WARNING, "failed to resolve name %s", symname);

	return (uint64_t) addr;
}

#if LLVM_VERSION_MAJOR > 11

static LLVMErrorRef
llvm_resolve_symbols(LLVMOrcDefinitionGeneratorRef GeneratorObj, void *Ctx,
					 LLVMOrcLookupStateRef *LookupState, LLVMOrcLookupKind Kind,
					 LLVMOrcJITDylibRef JD, LLVMOrcJITDylibLookupFlags JDLookupFlags,
					 LLVMOrcCLookupSet LookupSet, size_t LookupSetSize)
{
#if LLVM_VERSION_MAJOR > 14
	LLVMOrcCSymbolMapPairs symbols = palloc0(sizeof(LLVMOrcCSymbolMapPair) * LookupSetSize);
#else
	LLVMOrcCSymbolMapPairs symbols = palloc0(sizeof(LLVMJITCSymbolMapPair) * LookupSetSize);
#endif
	LLVMErrorRef error;
	LLVMOrcMaterializationUnitRef mu;

	for (int i = 0; i < LookupSetSize; i++)
	{
		const char *name = LLVMOrcSymbolStringPoolEntryStr(LookupSet[i].Name);

#if LLVM_VERSION_MAJOR > 12
		LLVMOrcRetainSymbolStringPoolEntry(LookupSet[i].Name);
#endif
		symbols[i].Name = LookupSet[i].Name;
		symbols[i].Sym.Address = llvm_resolve_symbol(name, NULL);
		symbols[i].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
	}

	mu = LLVMOrcAbsoluteSymbols(symbols, LookupSetSize);
	error = LLVMOrcJITDylibDefine(JD, mu);
	if (error != LLVMErrorSuccess)
		LLVMOrcDisposeMaterializationUnit(mu);

	pfree(symbols);

	return error;
}

/*
 * 不能经 LLVM 抛错（至少会变成 FATAL），故此处只用
 * WARNING。无妨：顶层动作也会报错（细节较少），且
 * 带细节的错误可能被调用多次。
 *
 *
 * 正常运行很少发生，多见于符号解析损坏。故用
 * elog(WARNING) 即可。
 */
static void
llvm_log_jit_error(void *ctx, LLVMErrorRef error)
{
	elog(WARNING, "error during JITing: %s",
		 llvm_error_message(error));
}

/*
 * 创建自有对象层，以便添加事件监听器。
 */
static LLVMOrcObjectLayerRef
llvm_create_object_layer(void *Ctx, LLVMOrcExecutionSessionRef ES, const char *Triple)
{
#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
	LLVMOrcObjectLayerRef objlayer =
		LLVMOrcCreateRTDyldObjectLinkingLayerWithSafeSectionMemoryManager(ES);
#else
	LLVMOrcObjectLayerRef objlayer =
		LLVMOrcCreateRTDyldObjectLinkingLayerWithSectionMemoryManager(ES);
#endif


#if defined(HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER) && HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER
	if (jit_debugging_support)
	{
		LLVMJITEventListenerRef l = LLVMCreateGDBRegistrationListener();

		LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(objlayer, l);
	}
#endif

#if defined(HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER) && HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER
	if (jit_profiling_support)
	{
		LLVMJITEventListenerRef l = LLVMCreatePerfJITEventListener();

		LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(objlayer, l);
	}
#endif

	return objlayer;
}

/*
 * 用传入的目标机创建 LLJIT 实例。之后目标机归该
 * LLJIT 实例所有。
 */
static LLVMOrcLLJITRef
llvm_create_jit_instance(LLVMTargetMachineRef tm)
{
	LLVMOrcLLJITRef lljit;
	LLVMOrcJITTargetMachineBuilderRef tm_builder;
	LLVMOrcLLJITBuilderRef lljit_builder;
	LLVMErrorRef error;
	LLVMOrcDefinitionGeneratorRef main_gen;
	LLVMOrcDefinitionGeneratorRef ref_gen;

	lljit_builder = LLVMOrcCreateLLJITBuilder();
	tm_builder = LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(tm);
	LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(lljit_builder, tm_builder);

	LLVMOrcLLJITBuilderSetObjectLinkingLayerCreator(lljit_builder,
													llvm_create_object_layer,
													NULL);

	error = LLVMOrcCreateLLJIT(&lljit, lljit_builder);
	if (error)
		elog(ERROR, "failed to create lljit instance: %s",
			 llvm_error_message(error));

	LLVMOrcExecutionSessionSetErrorReporter(LLVMOrcLLJITGetExecutionSession(lljit),
											llvm_log_jit_error, NULL);

	/*
	 * 解析 postgres 二进制/已加载库中符号的支持。
	 *
	 */
	error = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&main_gen,
																 LLVMOrcLLJITGetGlobalPrefix(lljit),
																 0, NULL);
	if (error)
		elog(ERROR, "failed to create generator: %s",
			 llvm_error_message(error));
	LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(lljit), main_gen);

	/*
	 * 解析特殊函数的支持，例如调用 SQL 可调用函数。
	 *
	 */
#if LLVM_VERSION_MAJOR > 14
	ref_gen = LLVMOrcCreateCustomCAPIDefinitionGenerator(llvm_resolve_symbols, NULL, NULL);
#else
	ref_gen = LLVMOrcCreateCustomCAPIDefinitionGenerator(llvm_resolve_symbols, NULL);
#endif
	LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(lljit), ref_gen);

	return lljit;
}

static char *
llvm_error_message(LLVMErrorRef error)
{
	char	   *orig = LLVMGetErrorMessage(error);
	char	   *msg = pstrdup(orig);

	LLVMDisposeErrorMessage(orig);

	return msg;
}

#endif							/* LLVM_VERSION_MAJOR > 11 */
