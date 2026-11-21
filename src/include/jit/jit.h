/*-------------------------------------------------------------------------
 * jit.h
 *	  与提供方无关的 JIT 设施。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/jit/jit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JIT_H
#define JIT_H

#include "executor/instrument.h"
#include "utils/resowner.h"


/*  决定执行何种 JIT 操作的标志  */
#define PGJIT_NONE     0
#define PGJIT_PERFORM  (1 << 0)
#define PGJIT_OPT3     (1 << 1)
#define PGJIT_INLINE   (1 << 2)
#define PGJIT_EXPR	   (1 << 3)
#define PGJIT_DEFORM   (1 << 4)


typedef struct JitInstrumentation
{
	/*  已发出的函数个数  */
	size_t		created_functions;

	/*  生成代码累计耗时  */
	instr_time	generation_counter;

	/*  内联累计耗时  */
	instr_time	inlining_counter;

	/*  优化累计耗时  */
	instr_time	optimization_counter;

	/*  代码发出累计耗时  */
	instr_time	emission_counter;
} JitInstrumentation;

/*
 * 累计所有工作进程 JIT 插装的 DSM 结构。
 */
typedef struct SharedJitInstrumentation
{
	int			num_workers;
	JitInstrumentation jit_instr[FLEXIBLE_ARRAY_MEMBER];
} SharedJitInstrumentation;

typedef struct JitContext
{
	/*  见 PGJIT_*  */
	int			flags;

	ResourceOwner resowner;

	JitInstrumentation instr;
} JitContext;

typedef struct JitProviderCallbacks JitProviderCallbacks;

extern PGDLLEXPORT void _PG_jit_provider_init(JitProviderCallbacks *cb);
typedef void (*JitProviderInit) (JitProviderCallbacks *cb);
typedef void (*JitProviderResetAfterErrorCB) (void);
typedef void (*JitProviderReleaseContextCB) (JitContext *context);
struct ExprState;
typedef bool (*JitProviderCompileExprCB) (struct ExprState *state);

struct JitProviderCallbacks
{
	JitProviderResetAfterErrorCB reset_after_error;
	JitProviderReleaseContextCB release_context;
	JitProviderCompileExprCB compile_expr;
};


/* GUCs */
extern PGDLLIMPORT bool jit_enabled;
extern PGDLLIMPORT char *jit_provider;
extern PGDLLIMPORT bool jit_debugging_support;
extern PGDLLIMPORT bool jit_dump_bitcode;
extern PGDLLIMPORT bool jit_expressions;
extern PGDLLIMPORT bool jit_profiling_support;
extern PGDLLIMPORT bool jit_tuple_deforming;
extern PGDLLIMPORT double jit_above_cost;
extern PGDLLIMPORT double jit_inline_above_cost;
extern PGDLLIMPORT double jit_optimize_above_cost;


extern void jit_reset_after_error(void);
extern void jit_release_context(JitContext *context);

/*
 * 尝试 JIT 代码的函数。调用者须接受其可能
 * 无法执行 JIT（即返回 false）。
 */
extern bool jit_compile_expr(struct ExprState *state);
extern void InstrJitAgg(JitInstrumentation *dst, JitInstrumentation *add);


#endif							/* JIT_H */
