/*-------------------------------------------------------------------------
 *
 * jit.c
 *	  与提供方无关的 JIT 设施。
 *
 * 加载 JIT 提供方、把调用转给 JIT 提供方并做错误处理。
 * 针对某一 JIT 实现的代码不应出现于此。
 *
 *
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/jit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "executor/execExpr.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "utils/fmgrprotos.h"
#include "utils/resowner_private.h"

/* GUCs */
bool		jit_enabled = true;
char	   *jit_provider = NULL;
bool		jit_debugging_support = false;
bool		jit_dump_bitcode = false;
bool		jit_expressions = true;
bool		jit_profiling_support = false;
bool		jit_tuple_deforming = true;
double		jit_above_cost = 100000;
double		jit_inline_above_cost = 500000;
double		jit_optimize_above_cost = 500000;

static JitProviderCallbacks provider;
static bool provider_successfully_loaded = false;
static bool provider_failed_loading = false;


static bool provider_init(void);
static bool file_exists(const char *name);


/*
 * SQL 层函数：当前 backend 是否可用 JIT。
 * 必要时会尝试加载 JIT 提供方。
 */
Datum
pg_jit_available(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(provider_init());
}


/*
 * 返回 JIT 提供方是否已成功加载，并缓存结果。
 *
 */
static bool
provider_init(void)
{
	char		path[MAXPGPATH];
	JitProviderInit init;

	/* 未启用则根本不尝试加载 */
	if (!jit_enabled)
		return false;

	/*
	 * 失败后不再重试——加载 JIT 提供方并不便宜。
	 *
	 */
	if (provider_failed_loading)
		return false;
	if (provider_successfully_loaded)
		return true;

	/*
	 * 先检查共享库是否存在。在真正尝试加载
	 * 共享库（经 load_external_function()）之前做此检查，
	 * 因共享库不可用时那会直接报错退出。
	 */
	snprintf(path, MAXPGPATH, "%s/%s%s", pkglib_path, jit_provider, DLSUFFIX);
	elog(DEBUG1, "probing availability of JIT provider at %s", path);
	if (!file_exists(path))
	{
		elog(DEBUG1,
			 "provider not available, disabling JIT for current session");
		provider_failed_loading = true;
		return false;
	}

	/*
	 * 若加载函数失败则记失败。因即便上面
	 * 已检查，load_external_function() 仍可能报错，
	 * 例如库的依赖未安装。那种情况要发 ERROR
	 * 通知用户，但不要不断重试。
	 *
	 */
	provider_failed_loading = true;

	/* 并初始化 */
	init = (JitProviderInit)
		load_external_function(path, "_PG_jit_provider_init", true, NULL);
	init(&provider);

	provider_successfully_loaded = true;
	provider_failed_loading = false;

	elog(DEBUG1, "successfully loaded JIT provider in current session");

	return true;
}

/*
 * 重置 JIT 提供方的错误处理。在已抛错且主循环
 * 重新取得控制权后调用。
 */
void
jit_reset_after_error(void)
{
	if (provider_successfully_loaded)
		provider.reset_after_error();
}

/*
 * 释放一个 JIT 上下文所需的资源。
 */
void
jit_release_context(JitContext *context)
{
	if (provider_successfully_loaded)
		provider.release_context(context);

	ResourceOwnerForgetJIT(context->resowner, PointerGetDatum(context));
	pfree(context);
}

/*
 * 请提供方 JIT 编译一个表达式。
 *
 * 成功返回 true，否则 false。
 */
bool
jit_compile_expr(struct ExprState *state)
{
	/*
	 * 对没有关联 PlanState（因而也无 EState）的函数，
	 * 很容易建一次性上下文。但没有执行器关闭回调
	 * 可释放所创建的函数，它们会活到事务结束，
	 * 再由 resowner 机制清理。这会导致可观的内存
	 * 占用，更糟的是会触发 gdb 中的平方级行为。
	 * 因此至少目前，在这些情形下不要创建
	 * JIT 过的函数。
	 */
	if (!state->parent)
		return false;

	/* 若根本不应做 JIT */
	if (!(state->parent->state->es_jit_flags & PGJIT_PERFORM))
		return false;

	/* 或表达式不做 JIT */
	if (!(state->parent->state->es_jit_flags & PGJIT_EXPR))
		return false;

	/* 亦计入 !jit_enabled */
	if (provider_init())
		return provider.compile_expr(state);

	return false;
}

/* 汇总 JIT 插装信息 */
void
InstrJitAgg(JitInstrumentation *dst, JitInstrumentation *add)
{
	dst->created_functions += add->created_functions;
	INSTR_TIME_ADD(dst->generation_counter, add->generation_counter);
	INSTR_TIME_ADD(dst->inlining_counter, add->inlining_counter);
	INSTR_TIME_ADD(dst->optimization_counter, add->optimization_counter);
	INSTR_TIME_ADD(dst->emission_counter, add->emission_counter);
}

static bool
file_exists(const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}
