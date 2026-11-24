/*-------------------------------------------------------------------------
 *
 * llvmjit_expr.c
 *	  JIT 编译表达式。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_expr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/execdebug.h"
#include "executor/nodeAgg.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrtab.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

typedef struct CompiledExprState
{
	LLVMJitContext *context;
	const char *funcname;
} CompiledExprState;


static Datum ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull);

static LLVMValueRef BuildV1Call(LLVMJitContext *context, LLVMBuilderRef b,
								LLVMModuleRef mod, FunctionCallInfo fcinfo,
								LLVMValueRef *v_fcinfo_isnull);
static LLVMValueRef build_EvalXFuncInt(LLVMBuilderRef b, LLVMModuleRef mod,
									   const char *funcname,
									   LLVMValueRef v_state,
									   ExprEvalStep *op,
									   int natts, LLVMValueRef *v_args);
static LLVMValueRef create_LifetimeEnd(LLVMModuleRef mod);

/* 便于调用 ExecEval* 的宏 */
#define build_EvalXFunc(b, mod, funcname, v_state, op, ...) \
	build_EvalXFuncInt(b, mod, funcname, v_state, op, \
					   lengthof(((LLVMValueRef[]){__VA_ARGS__})), \
					   ((LLVMValueRef[]){__VA_ARGS__}))


/*
 * JIT 编译表达式。
 */
bool
llvm_compile_expr(ExprState *state)
{
	PlanState  *parent = state->parent;
	char	   *funcname;

	LLVMJitContext *context = NULL;

	LLVMBuilderRef b;
	LLVMModuleRef mod;
	LLVMContextRef lc;
	LLVMValueRef eval_fn;
	LLVMBasicBlockRef entry;
	LLVMBasicBlockRef *opblocks;

	/* 状态本身 */
	LLVMValueRef v_state;
	LLVMValueRef v_econtext;
	LLVMValueRef v_parent;

	/* 返回值 */
	LLVMValueRef v_isnullp;

	/* 状态内临时 */
	LLVMValueRef v_tmpvaluep;
	LLVMValueRef v_tmpisnullp;

	/* slots */
	LLVMValueRef v_innerslot;
	LLVMValueRef v_outerslot;
	LLVMValueRef v_scanslot;
	LLVMValueRef v_resultslot;

	/* 槽的 nulls/values */
	LLVMValueRef v_innervalues;
	LLVMValueRef v_innernulls;
	LLVMValueRef v_outervalues;
	LLVMValueRef v_outernulls;
	LLVMValueRef v_scanvalues;
	LLVMValueRef v_scannulls;
	LLVMValueRef v_resultvalues;
	LLVMValueRef v_resultnulls;

	/* econtext 内 */
	LLVMValueRef v_aggvalues;
	LLVMValueRef v_aggnulls;

	instr_time	starttime;
	instr_time	endtime;

	llvm_enter_fatal_on_oom();

	/*
	 * 目前不支持编译没有 parent 的表达式，因为需要
	 * 访问 EState。
	 */
	Assert(parent);

	/* 取或建 JIT 上下文 */
	if (parent->state->es_jit)
		context = (LLVMJitContext *) parent->state->es_jit;
	else
	{
		context = llvm_create_context(parent->state->es_jit_flags);
		parent->state->es_jit = &context->base;
	}

	INSTR_TIME_SET_CURRENT(starttime);

	mod = llvm_mutable_module(context);
	lc = LLVMGetModuleContext(mod);

	b = LLVMCreateBuilderInContext(lc);

	funcname = llvm_expand_funcname(context, "evalexpr");

	/* 创建函数 */
	eval_fn = LLVMAddFunction(mod, funcname,
							  llvm_pg_var_func_type("ExecInterpExprStillValid"));
	LLVMSetLinkage(eval_fn, LLVMExternalLinkage);
	LLVMSetVisibility(eval_fn, LLVMDefaultVisibility);
	llvm_copy_attributes(AttributeTemplate, eval_fn);

	entry = LLVMAppendBasicBlockInContext(lc, eval_fn, "entry");

	/* 建状态 */
	v_state = LLVMGetParam(eval_fn, 0);
	v_econtext = LLVMGetParam(eval_fn, 1);
	v_isnullp = LLVMGetParam(eval_fn, 2);

	LLVMPositionBuilderAtEnd(b, entry);

	v_tmpvaluep = l_struct_gep(b,
							   StructExprState,
							   v_state,
							   FIELDNO_EXPRSTATE_RESVALUE,
							   "v.state.resvalue");
	v_tmpisnullp = l_struct_gep(b,
								StructExprState,
								v_state,
								FIELDNO_EXPRSTATE_RESNULL,
								"v.state.resnull");
	v_parent = l_load_struct_gep(b,
								 StructExprState,
								 v_state,
								 FIELDNO_EXPRSTATE_PARENT,
								 "v.state.parent");

	/* 建全局槽 */
	v_scanslot = l_load_struct_gep(b,
								   StructExprContext,
								   v_econtext,
								   FIELDNO_EXPRCONTEXT_SCANTUPLE,
								   "v_scanslot");
	v_innerslot = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_INNERTUPLE,
									"v_innerslot");
	v_outerslot = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_OUTERTUPLE,
									"v_outerslot");
	v_resultslot = l_load_struct_gep(b,
									 StructExprState,
									 v_state,
									 FIELDNO_EXPRSTATE_RESULTSLOT,
									 "v_resultslot");

	/* 建全局 values/isnull 指针 */
	v_scanvalues = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_scanslot,
									 FIELDNO_TUPLETABLESLOT_VALUES,
									 "v_scanvalues");
	v_scannulls = l_load_struct_gep(b,
									StructTupleTableSlot,
									v_scanslot,
									FIELDNO_TUPLETABLESLOT_ISNULL,
									"v_scannulls");
	v_innervalues = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_innerslot,
									  FIELDNO_TUPLETABLESLOT_VALUES,
									  "v_innervalues");
	v_innernulls = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_innerslot,
									 FIELDNO_TUPLETABLESLOT_ISNULL,
									 "v_innernulls");
	v_outervalues = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_outerslot,
									  FIELDNO_TUPLETABLESLOT_VALUES,
									  "v_outervalues");
	v_outernulls = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_outerslot,
									 FIELDNO_TUPLETABLESLOT_ISNULL,
									 "v_outernulls");
	v_resultvalues = l_load_struct_gep(b,
									   StructTupleTableSlot,
									   v_resultslot,
									   FIELDNO_TUPLETABLESLOT_VALUES,
									   "v_resultvalues");
	v_resultnulls = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_resultslot,
									  FIELDNO_TUPLETABLESLOT_ISNULL,
									  "v_resultnulls");

	/* aggvalues/aggnulls */
	v_aggvalues = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_AGGVALUES,
									"v.econtext.aggvalues");
	v_aggnulls = l_load_struct_gep(b,
								   StructExprContext,
								   v_econtext,
								   FIELDNO_EXPRCONTEXT_AGGNULLS,
								   "v.econtext.aggnulls");

	/* 预先为每个 op 分配块，便于跳转 */
	opblocks = palloc(sizeof(LLVMBasicBlockRef) * state->steps_len);
	for (int opno = 0; opno < state->steps_len; opno++)
		opblocks[opno] = l_bb_append_v(eval_fn, "b.op.%d.start", opno);

	/* 从入口跳到第一块 */
	LLVMBuildBr(b, opblocks[0]);

	for (int opno = 0; opno < state->steps_len; opno++)
	{
		ExprEvalStep *op;
		ExprEvalOp	opcode;
		LLVMValueRef v_resvaluep;
		LLVMValueRef v_resnullp;

		LLVMPositionBuilderAtEnd(b, opblocks[opno]);

		op = &state->steps[opno];
		opcode = ExecEvalStepOp(state, op);

		v_resvaluep = l_ptr_const(op->resvalue, l_ptr(TypeSizeT));
		v_resnullp = l_ptr_const(op->resnull, l_ptr(TypeStorageBool));

		switch (opcode)
		{
			case EEOP_DONE:
				{
					LLVMValueRef v_tmpisnull;
					LLVMValueRef v_tmpvalue;

					v_tmpvalue = l_load(b, TypeSizeT, v_tmpvaluep, "");
					v_tmpisnull = l_load(b, TypeStorageBool, v_tmpisnullp, "");

					LLVMBuildStore(b, v_tmpisnull, v_isnullp);

					LLVMBuildRet(b, v_tmpvalue);
					break;
				}

			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
				{
					TupleDesc	desc = NULL;
					LLVMValueRef v_slot;
					LLVMBasicBlockRef b_fetch;
					LLVMValueRef v_nvalid;
					LLVMValueRef l_jit_deform = NULL;
					const TupleTableSlotOps *tts_ops = NULL;

					b_fetch = l_bb_before_v(opblocks[opno + 1],
											"op.%d.fetch", opno);

					if (op->d.fetch.known_desc)
						desc = op->d.fetch.known_desc;

					if (op->d.fetch.fixed)
						tts_ops = op->d.fetch.kind;

					/* 不应生成过该步骤 */
					Assert(tts_ops != &TTSOpsVirtual);

					if (opcode == EEOP_INNER_FETCHSOME)
						v_slot = v_innerslot;
					else if (opcode == EEOP_OUTER_FETCHSOME)
						v_slot = v_outerslot;
					else
						v_slot = v_scanslot;

					/*
					 * 检查所需属性是否都可用，或是否
					 * 需要变形。
					 */
					v_nvalid =
						l_load_struct_gep(b,
										  StructTupleTableSlot,
										  v_slot,
										  FIELDNO_TUPLETABLESLOT_NVALID,
										  "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntUGE, v_nvalid,
												  l_int16_const(lc, op->d.fetch.last_var),
												  ""),
									opblocks[opno + 1], b_fetch);

					LLVMPositionBuilderAtEnd(b, b_fetch);

					/*
					 * 若待变形元组的 tupledesc 已知，且
					 * 启用变形 JIT，则按 tupledesc 与待提取
					 * 属性的确切个数生成专用变形函数。
					 *
					 */
					if (tts_ops && desc && (context->base.flags & PGJIT_DEFORM))
					{
						l_jit_deform =
							slot_compile_deform(context, desc,
												tts_ops,
												op->d.fetch.last_var);
					}

					if (l_jit_deform)
					{
						LLVMValueRef params[1];

						params[0] = v_slot;

						l_call(b,
							   LLVMGetFunctionType(l_jit_deform),
							   l_jit_deform,
							   params, lengthof(params), "");
					}
					else
					{
						LLVMValueRef params[2];

						params[0] = v_slot;
						params[1] = l_int32_const(lc, op->d.fetch.last_var);

						l_call(b,
							   llvm_pg_var_func_type("slot_getsomeattrs_int"),
							   llvm_pg_func(mod, "slot_getsomeattrs_int"),
							   params, lengthof(params), "");
					}

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
				{
					LLVMValueRef value,
								isnull;
					LLVMValueRef v_attnum;
					LLVMValueRef v_values;
					LLVMValueRef v_nulls;

					if (opcode == EEOP_INNER_VAR)
					{
						v_values = v_innervalues;
						v_nulls = v_innernulls;
					}
					else if (opcode == EEOP_OUTER_VAR)
					{
						v_values = v_outervalues;
						v_nulls = v_outernulls;
					}
					else
					{
						v_values = v_scanvalues;
						v_nulls = v_scannulls;
					}

					v_attnum = l_int32_const(lc, op->d.var.attnum);
					value = l_load_gep1(b, TypeSizeT, v_values, v_attnum, "");
					isnull = l_load_gep1(b, TypeStorageBool, v_nulls, v_attnum, "");
					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
				{
					LLVMValueRef v_slot;

					if (opcode == EEOP_INNER_SYSVAR)
						v_slot = v_innerslot;
					else if (opcode == EEOP_OUTER_SYSVAR)
						v_slot = v_outerslot;
					else
						v_slot = v_scanslot;

					build_EvalXFunc(b, mod, "ExecEvalSysVar",
									v_state, op, v_econtext, v_slot);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_WHOLEROW:
				build_EvalXFunc(b, mod, "ExecEvalWholeRowVar",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
				{
					LLVMValueRef v_value;
					LLVMValueRef v_isnull;
					LLVMValueRef v_rvaluep;
					LLVMValueRef v_risnullp;
					LLVMValueRef v_attnum;
					LLVMValueRef v_resultnum;
					LLVMValueRef v_values;
					LLVMValueRef v_nulls;

					if (opcode == EEOP_ASSIGN_INNER_VAR)
					{
						v_values = v_innervalues;
						v_nulls = v_innernulls;
					}
					else if (opcode == EEOP_ASSIGN_OUTER_VAR)
					{
						v_values = v_outervalues;
						v_nulls = v_outernulls;
					}
					else
					{
						v_values = v_scanvalues;
						v_nulls = v_scannulls;
					}

					/* 取数据 */
					v_attnum = l_int32_const(lc, op->d.assign_var.attnum);
					v_value = l_load_gep1(b, TypeSizeT, v_values, v_attnum, "");
					v_isnull = l_load_gep1(b, TypeStorageBool, v_nulls, v_attnum, "");

					/* 计算目标地址 */
					v_resultnum = l_int32_const(lc, op->d.assign_var.resultnum);
					v_rvaluep = l_gep(b,
									  TypeSizeT,
									  v_resultvalues,
									  &v_resultnum, 1, "");
					v_risnullp = l_gep(b,
									   TypeStorageBool,
									   v_resultnulls,
									   &v_resultnum, 1, "");

					/* 并存储 */
					LLVMBuildStore(b, v_value, v_rvaluep);
					LLVMBuildStore(b, v_isnull, v_risnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
				{
					LLVMValueRef v_value,
								v_isnull;
					LLVMValueRef v_rvaluep,
								v_risnullp;
					LLVMValueRef v_resultnum;
					size_t		resultnum = op->d.assign_tmp.resultnum;

					/* 取数据 */
					v_value = l_load(b, TypeSizeT, v_tmpvaluep, "");
					v_isnull = l_load(b, TypeStorageBool, v_tmpisnullp, "");

					/* 计算目标地址 */
					v_resultnum = l_int32_const(lc, resultnum);
					v_rvaluep =
						l_gep(b, TypeSizeT, v_resultvalues, &v_resultnum, 1, "");
					v_risnullp =
						l_gep(b, TypeStorageBool, v_resultnulls, &v_resultnum, 1, "");

					/* 存空值性 */
					LLVMBuildStore(b, v_isnull, v_risnullp);

					/* 必要时把值改成只读 */
					if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
					{
						LLVMBasicBlockRef b_notnull;
						LLVMValueRef v_params[1];

						b_notnull = l_bb_before_v(opblocks[opno + 1],
												  "op.%d.assign_tmp.notnull", opno);

						/* 检查值是否 NULL */
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_isnull,
													  l_sbool_const(0), ""),
										b_notnull, opblocks[opno + 1]);

						/* 若非空，转成 RO datum */
						LLVMPositionBuilderAtEnd(b, b_notnull);
						v_params[0] = v_value;
						v_value =
							l_call(b,
								   llvm_pg_var_func_type("MakeExpandedObjectReadOnlyInternal"),
								   llvm_pg_func(mod, "MakeExpandedObjectReadOnlyInternal"),
								   v_params, lengthof(v_params), "");

						/*
						 * 出 if 时 builder 在 b_notnull，无妨上面已
						 * 存过空值。
						 */
					}

					/* 最后存储结果 */
					LLVMBuildStore(b, v_value, v_rvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CONST:
				{
					LLVMValueRef v_constvalue,
								v_constnull;

					v_constvalue = l_sizet_const(op->d.constval.value);
					v_constnull = l_sbool_const(op->d.constval.isnull);

					LLVMBuildStore(b, v_constvalue, v_resvaluep);
					LLVMBuildStore(b, v_constnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_retval;

					if (opcode == EEOP_FUNCEXPR_STRICT)
					{
						LLVMBasicBlockRef b_nonull;
						LLVMBasicBlockRef *b_checkargnulls;
						LLVMValueRef v_fcinfo;

						/*
						 * 若参数非空，则真正调用函数的块
						 *
						 */
						b_nonull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.no-null-args", opno);

						/* 应确保事先已优化 */
						if (op->d.func.nargs == 0)
							elog(ERROR, "argumentless strict functions are pointless");

						v_fcinfo =
							l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

						/*
						 * resnull 置 true；若真正调用函数，再
						 * 复位
						 */
						LLVMBuildStore(b, l_sbool_const(1), v_resnullp);

						/* 为检查各参数各建一块 */
						b_checkargnulls =
							palloc(sizeof(LLVMBasicBlockRef *) * op->d.func.nargs);
						for (int argno = 0; argno < op->d.func.nargs; argno++)
							b_checkargnulls[argno] =
								l_bb_before_v(b_nonull, "b.%d.isnull.%d", opno,
											  argno);

						/* 跳到检查第一个参数 */
						LLVMBuildBr(b, b_checkargnulls[0]);

						/* 逐个检查参数 NULLness */
						for (int argno = 0; argno < op->d.func.nargs; argno++)
						{
							LLVMValueRef v_argisnull;
							LLVMBasicBlockRef b_argnotnull;

							LLVMPositionBuilderAtEnd(b, b_checkargnulls[argno]);

							/*
							 * 计算参数非空时要跳到的块。
							 *
							 */
							if (argno + 1 == op->d.func.nargs)
								b_argnotnull = b_nonull;
							else
								b_argnotnull = b_checkargnulls[argno + 1];

							/* 最后加载并检查参数 NULLness */
							v_argisnull = l_funcnull(b, v_fcinfo, argno);
							LLVMBuildCondBr(b,
											LLVMBuildICmp(b, LLVMIntEQ,
														  v_argisnull,
														  l_sbool_const(1),
														  ""),
											opblocks[opno + 1],
											b_argnotnull);
						}

						LLVMPositionBuilderAtEnd(b, b_nonull);
					}

					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR_FUSAGE:
				build_EvalXFunc(b, mod, "ExecEvalFuncExprFusage",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;


			case EEOP_FUNCEXPR_STRICT_FUSAGE:
				build_EvalXFunc(b, mod, "ExecEvalFuncExprStrictFusage",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

				/*
				 * 同等对待，优化器可去掉冗余。发码
				 * 优化或许也值得。
				 *
				 */
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;
					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolcheckfalse;
					LLVMBasicBlockRef b_boolisfalse;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolisnull", opno);
					b_boolcheckfalse = l_bb_before_v(opblocks[opno + 1],
													 "b.%d.boolcheckfalse", opno);
					b_boolisfalse = l_bb_before_v(opblocks[opno + 1],
												  "b.%d.boolisfalse", opno);
					b_boolisanynull = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolisanynull", opno);
					b_boolcont = l_bb_before_v(opblocks[opno + 1],
											   "b.%d.boolcont", opno);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					if (opcode == EEOP_BOOL_AND_STEP_FIRST)
						LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);

					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* resnull 置为 boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* resvalue 置 boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					/* 检查当前输入是否 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolcheckfalse);

					/* 建把 anynull 置位的块 */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* boolanynull 置 true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* 并跳到下一块 */
					LLVMBuildBr(b, b_boolcont);

					/* 建检查 false 的块 */
					LLVMPositionBuilderAtEnd(b, b_boolcheckfalse);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(0), ""),
									b_boolisfalse,
									b_boolcont);

					/*
					 * 处理 FALSE 的块。值为假，故短路。
					 *
					 */
					LLVMPositionBuilderAtEnd(b, b_boolisfalse);
					/* 结果已是 FALSE，不必改 */
					/* 并跳到 AND 表达式末尾 */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* 建布尔为 TRUE 时继续的块。 */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = l_load(b, TypeStorageBool, v_boolanynullp, "");

					/* 若先前有 NULL 则把值置 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* resnull 置 true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* 复位resvalue */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

				/*
				 * 同等对待，优化器可去掉冗余。发码
				 * 优化或许也值得。
				 *
				 */
			case EEOP_BOOL_OR_STEP_FIRST:
			case EEOP_BOOL_OR_STEP:
			case EEOP_BOOL_OR_STEP_LAST:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;

					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolchecktrue;
					LLVMBasicBlockRef b_boolistrue;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolisnull", opno);
					b_boolchecktrue = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolchecktrue", opno);
					b_boolistrue = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolistrue", opno);
					b_boolisanynull = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolisanynull", opno);
					b_boolcont = l_bb_before_v(opblocks[opno + 1],
											   "b.%d.boolcont", opno);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					if (opcode == EEOP_BOOL_OR_STEP_FIRST)
						LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);
					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* resnull 置为 boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* resvalue 置 boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolchecktrue);

					/* 建把 anynull 置位的块 */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* boolanynull 置 true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* 并跳到下一块 */
					LLVMBuildBr(b, b_boolcont);

					/* 建检查 true 的块 */
					LLVMPositionBuilderAtEnd(b, b_boolchecktrue);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(1), ""),
									b_boolistrue,
									b_boolcont);

					/*
					 * 处理 True 的块。值为真，故短路。
					 *
					 */
					LLVMPositionBuilderAtEnd(b, b_boolistrue);
					/* 结果已是 TRUE，不必改 */
					/* 并跳到 OR 表达式末尾 */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* 建布尔为 FALSE 时继续的块 */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = l_load(b, TypeStorageBool, v_boolanynullp, "");

					/* 若先前有 NULL 则把值置 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* resnull 置 true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* 复位resvalue */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_BOOL_NOT_STEP:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_negbool;

					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					v_negbool = LLVMBuildZExt(b,
											  LLVMBuildICmp(b, LLVMIntEQ,
															v_boolvalue,
															l_sizet_const(0),
															""),
											  TypeSizeT, "");
					/* resnull 置为 boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* resvalue 置 !boolvalue */
					LLVMBuildStore(b, v_negbool, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_QUAL:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;
					LLVMBasicBlockRef b_qualfail;

					b_qualfail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.qualfail", opno);

					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									b_qualfail,
									opblocks[opno + 1]);

					/* 建处理 NULL 或 false 的块 */
					LLVMPositionBuilderAtEnd(b, b_qualfail);
					/* resnull 置 false */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					/* resvalue 置 false */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					/* 并跳出 */
					LLVMBuildBr(b, opblocks[op->d.qualexpr.jumpdone]);
					break;
				}

			case EEOP_JUMP:
				{
					LLVMBuildBr(b, opblocks[op->d.jump.jumpdone]);
					break;
				}

			case EEOP_JUMP_IF_NULL:
				{
					LLVMValueRef v_resnull;

					/* 当前结果为空则转移控制 */

					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_JUMP_IF_NOT_NULL:
				{
					LLVMValueRef v_resnull;

					/* 当前结果非空则转移控制 */

					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(0), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}


			case EEOP_JUMP_IF_NOT_TRUE:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;

					/* 当前结果为空或假则转移控制 */

					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNULL:
				{
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(1),
										l_sizet_const(0),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNOTNULL:
				{
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(0),
										l_sizet_const(1),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ROWISNULL:
				build_EvalXFunc(b, mod, "ExecEvalRowNull",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NULLTEST_ROWISNOTNULL:
				build_EvalXFunc(b, mod, "ExecEvalRowNotNull",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_BOOLTEST_IS_TRUE:
			case EEOP_BOOLTEST_IS_NOT_FALSE:
			case EEOP_BOOLTEST_IS_FALSE:
			case EEOP_BOOLTEST_IS_NOT_TRUE:
				{
					LLVMBasicBlockRef b_isnull,
								b_notnull;
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					b_isnull = l_bb_before_v(opblocks[opno + 1],
											 "op.%d.isnull", opno);
					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.isnotnull", opno);

					/* 检查值是否 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_isnull, b_notnull);

					/* 若值为 NULL，返回 false */
					LLVMPositionBuilderAtEnd(b, b_isnull);

					/* 结果非空 */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_FALSE)
					{
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					}
					else
					{
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					}

					LLVMBuildBr(b, opblocks[opno + 1]);

					LLVMPositionBuilderAtEnd(b, b_notnull);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_NOT_FALSE)
					{
						/*
						 * 若值非 NULL，返回该值（已设置）。
						 *
						 */
					}
					else
					{
						LLVMValueRef v_value =
							l_load(b, TypeSizeT, v_resvaluep, "");

						v_value = LLVMBuildZExt(b,
												LLVMBuildICmp(b, LLVMIntEQ,
															  v_value,
															  l_sizet_const(0),
															  ""),
												TypeSizeT, "");
						LLVMBuildStore(b, v_value, v_resvaluep);
					}
					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_PARAM_EXEC:
				build_EvalXFunc(b, mod, "ExecEvalParamExec",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_EXTERN:
				build_EvalXFunc(b, mod, "ExecEvalParamExtern",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_CALLBACK:
				{
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];

					v_func = l_ptr_const(op->d.cparam.paramfunc,
										 llvm_pg_var_type("TypeExecEvalSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					l_call(b,
						   LLVMGetFunctionType(ExecEvalSubroutineTemplate),
						   v_func,
						   v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SBSREF_SUBSCRIPTS:
				{
					int			jumpdone = op->d.sbsref_subscript.jumpdone;
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];
					LLVMValueRef v_ret;

					v_func = l_ptr_const(op->d.sbsref_subscript.subscriptfunc,
										 llvm_pg_var_type("TypeExecEvalBoolSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					v_ret = l_call(b,
								   LLVMGetFunctionType(ExecEvalBoolSubroutineTemplate),
								   v_func,
								   v_params, lengthof(v_params), "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdone]);
					break;
				}

			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
				{
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];

					v_func = l_ptr_const(op->d.sbsref.subscriptfunc,
										 llvm_pg_var_type("TypeExecEvalSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					l_call(b,
						   LLVMGetFunctionType(ExecEvalSubroutineTemplate),
						   v_func,
						   v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CASE_TESTVAL:
				{
					LLVMBasicBlockRef b_avail,
								b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casenullp,
								v_casenull;
					LLVMValueRef v_casevaluenull;

					b_avail = l_bb_before_v(opblocks[opno + 1],
											"op.%d.avail", opno);
					b_notavail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.notavail", opno);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(TypeSizeT));
					v_casenullp = l_ptr_const(op->d.casetest.isnull,
											  l_ptr(TypeStorageBool));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b, v_casevaluenull, b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = l_load(b, TypeSizeT, v_casevaluep, "");
					v_casenull = l_load(b, TypeStorageBool, v_casenullp, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_CASEDATUM, "");
					v_casenull =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_CASENULL, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MAKE_READONLY:
				{
					LLVMBasicBlockRef b_notnull;
					LLVMValueRef v_params[1];
					LLVMValueRef v_ret;
					LLVMValueRef v_nullp;
					LLVMValueRef v_valuep;
					LLVMValueRef v_null;
					LLVMValueRef v_value;

					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.readonly.notnull", opno);

					v_nullp = l_ptr_const(op->d.make_readonly.isnull,
										  l_ptr(TypeStorageBool));

					v_null = l_load(b, TypeStorageBool, v_nullp, "");

					/* 把 isnull 存进结果 */
					LLVMBuildStore(b, v_null, v_resnullp);

					/* 检查值是否 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_null,
												  l_sbool_const(1), ""),
									opblocks[opno + 1], b_notnull);

					/* 若非空，转成 RO datum */
					LLVMPositionBuilderAtEnd(b, b_notnull);

					v_valuep = l_ptr_const(op->d.make_readonly.value,
										   l_ptr(TypeSizeT));

					v_value = l_load(b, TypeSizeT, v_valuep, "");

					v_params[0] = v_value;
					v_ret =
						l_call(b,
							   llvm_pg_var_func_type("MakeExpandedObjectReadOnlyInternal"),
							   llvm_pg_func(mod, "MakeExpandedObjectReadOnlyInternal"),
							   v_params, lengthof(v_params), "");
					LLVMBuildStore(b, v_ret, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_IOCOERCE:
				{
					FunctionCallInfo fcinfo_out,
								fcinfo_in;
					LLVMValueRef v_fn_out,
								v_fn_in;
					LLVMValueRef v_fcinfo_out,
								v_fcinfo_in;
					LLVMValueRef v_fcinfo_in_isnullp;
					LLVMValueRef v_retval;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_resnull;

					LLVMValueRef v_output_skip;
					LLVMValueRef v_output;

					LLVMBasicBlockRef b_skipoutput;
					LLVMBasicBlockRef b_calloutput;
					LLVMBasicBlockRef b_input;
					LLVMBasicBlockRef b_inputcall;

					fcinfo_out = op->d.iocoerce.fcinfo_data_out;
					fcinfo_in = op->d.iocoerce.fcinfo_data_in;

					b_skipoutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.skipoutputnull", opno);
					b_calloutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.calloutput", opno);
					b_input = l_bb_before_v(opblocks[opno + 1],
											"op.%d.input", opno);
					b_inputcall = l_bb_before_v(opblocks[opno + 1],
												"op.%d.inputcall", opno);

					v_fn_out = llvm_function_reference(context, b, mod, fcinfo_out);
					v_fn_in = llvm_function_reference(context, b, mod, fcinfo_in);
					v_fcinfo_out = l_ptr_const(fcinfo_out, l_ptr(StructFunctionCallInfoData));
					v_fcinfo_in = l_ptr_const(fcinfo_in, l_ptr(StructFunctionCallInfoData));

					v_fcinfo_in_isnullp =
						l_struct_gep(b,
									 StructFunctionCallInfoData,
									 v_fcinfo_in,
									 FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
									 "v_fcinfo_in_isnull");

					/* 输出函数不会对空值调用 */
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_skipoutput,
									b_calloutput);

					LLVMPositionBuilderAtEnd(b, b_skipoutput);
					v_output_skip = l_sizet_const(0);
					LLVMBuildBr(b, b_input);

					LLVMPositionBuilderAtEnd(b, b_calloutput);
					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* 置 arg[0] */
					LLVMBuildStore(b,
								   v_resvalue,
								   l_funcvaluep(b, v_fcinfo_out, 0));
					LLVMBuildStore(b,
								   l_sbool_const(0),
								   l_funcnullp(b, v_fcinfo_out, 0));
					/* 并调用输出函数（永不返回 NULL） */
					v_output = l_call(b,
									  LLVMGetFunctionType(v_fn_out),
									  v_fn_out, &v_fcinfo_out,
									  1, "funccall_coerce_out");
					LLVMBuildBr(b, b_input);

					/* 建处理输入函数调用的块 */
					LLVMPositionBuilderAtEnd(b, b_input);

					/* resnull 与输出函数调用分支之间的 phi */
					{
						LLVMValueRef incoming_values[2];
						LLVMBasicBlockRef incoming_blocks[2];

						incoming_values[0] = v_output_skip;
						incoming_blocks[0] = b_skipoutput;

						incoming_values[1] = v_output;
						incoming_blocks[1] = b_calloutput;

						v_output = LLVMBuildPhi(b, TypeSizeT, "output");
						LLVMAddIncoming(v_output,
										incoming_values, incoming_blocks,
										lengthof(incoming_blocks));
					}

					/*
					 * 若输入函数 strict，输入串为 NULL 则跳过
					 *
					 */
					if (op->d.iocoerce.finfo_in->fn_strict)
					{
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_output,
													  l_sizet_const(0), ""),
										opblocks[opno + 1],
										b_inputcall);
					}
					else
					{
						LLVMBuildBr(b, b_inputcall);
					}

					LLVMPositionBuilderAtEnd(b, b_inputcall);
					/* 设置参数 */
					/* arg0: 输出 */
					LLVMBuildStore(b, v_output,
								   l_funcvaluep(b, v_fcinfo_in, 0));
					LLVMBuildStore(b, v_resnull,
								   l_funcnullp(b, v_fcinfo_in, 0));

					/* arg1 为 ioparam，见 execExpr.c */
					/* arg2 为 typmod，见 execExpr.c  */

					/* 清 fcinfo_in->isnull */
					LLVMBuildStore(b, l_sbool_const(0), v_fcinfo_in_isnullp);
					/* 并调用函数 */
					v_retval = l_call(b,
									  LLVMGetFunctionType(v_fn_in),
									  v_fn_in, &v_fcinfo_in, 1,
									  "funccall_iocoerce_in");

					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;

					LLVMValueRef v_argnull0,
								v_argisnull0;
					LLVMValueRef v_argnull1,
								v_argisnull1;

					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_bothargisnull;

					LLVMValueRef v_result;

					LLVMBasicBlockRef b_noargnull;
					LLVMBasicBlockRef b_checkbothargnull;
					LLVMBasicBlockRef b_bothargnull;
					LLVMBasicBlockRef b_anyargnull;

					b_noargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.noargnull", opno);
					b_checkbothargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.checkbothargnull", opno);
					b_bothargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.bothargnull", opno);
					b_anyargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.anyargnull", opno);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* 加载两参数的 args[0|1].isnull */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argisnull0 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												 l_sbool_const(1), "");
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);
					v_argisnull1 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												 l_sbool_const(1), "");

					v_anyargisnull = LLVMBuildOr(b, v_argisnull0, v_argisnull1, "");
					v_bothargisnull = LLVMBuildAnd(b, v_argisnull0, v_argisnull1, "");

					/*
					 * 查参数 NULLness：任一为 NULL 则看是否
					 * 两参数都是 NULL。否则调用比较器。
					 *
					 */
					LLVMBuildCondBr(b, v_anyargisnull, b_checkbothargnull,
									b_noargnull);

					/*
					 * 建检查是否有参数为 null 的块
					 */
					LLVMPositionBuilderAtEnd(b, b_checkbothargnull);
					LLVMBuildCondBr(b, v_bothargisnull, b_bothargnull,
									b_anyargnull);


					/* 都是 NULL？则并非 distinct */
					LLVMPositionBuilderAtEnd(b, b_bothargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);

					/* 仅一个是 NULL？则是 distinct */
					LLVMPositionBuilderAtEnd(b, b_anyargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* 两参数都非空：比较 */
					LLVMPositionBuilderAtEnd(b, b_noargnull);

					v_result = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);

					if (opcode == EEOP_DISTINCT)
					{
						/* 须把 = 的结果取反 */
						v_result =
							LLVMBuildZExt(b,
										  LLVMBuildICmp(b, LLVMIntEQ,
														v_result,
														l_sizet_const(0), ""),
										  TypeSizeT, "");
					}

					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLIF:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_argnull0;
					LLVMValueRef v_argnull1;
					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_arg0;
					LLVMBasicBlockRef b_hasnull;
					LLVMBasicBlockRef b_nonull;
					LLVMBasicBlockRef b_argsequal;
					LLVMValueRef v_retval;
					LLVMValueRef v_argsequal;

					b_hasnull = l_bb_before_v(opblocks[opno + 1],
											  "b.%d.null-args", opno);
					b_nonull = l_bb_before_v(opblocks[opno + 1],
											 "b.%d.no-null-args", opno);
					b_argsequal = l_bb_before_v(opblocks[opno + 1],
												"b.%d.argsequal", opno);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* 保存原 arg[0] */
					v_arg0 = l_funcvalue(b, v_fcinfo, 0);

					/* 任一参数为 NULL 则不可能相等 */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);

					v_anyargisnull =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												  l_sbool_const(1), ""),
									"");

					LLVMBuildCondBr(b, v_anyargisnull, b_hasnull, b_nonull);

					/* 一个（或两个）参数为空，返回 arg[0] */
					LLVMPositionBuilderAtEnd(b, b_hasnull);
					LLVMBuildStore(b, v_argnull0, v_resnullp);
					LLVMBuildStore(b, v_arg0, v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* 建调用函数并检查结果的块 */
					LLVMPositionBuilderAtEnd(b, b_nonull);

					/*
					 * 第一参数若为 varlena 类型，可能是扩展
					 * datum。须保证比较函数拿到只读指针
					 * 但若最终返回第一参数且是读写指针
					 * 则仍返回原来的读写指针。
					 *
					 *
					 */
					if (op->d.func.make_ro)
					{
						LLVMValueRef v_params[1];
						LLVMValueRef v_arg0_ro;

						v_params[0] = v_arg0;
						v_arg0_ro =
							l_call(b,
								   llvm_pg_var_func_type("MakeExpandedObjectReadOnlyInternal"),
								   llvm_pg_func(mod, "MakeExpandedObjectReadOnlyInternal"),
								   v_params, lengthof(v_params), "");
						LLVMBuildStore(b, v_arg0_ro,
									   l_funcvaluep(b, v_fcinfo, 0));
					}

					v_retval = BuildV1Call(context, b, mod, fcinfo, &v_fcinfo_isnull);

					/*
					 * 若结果非空且参数相等返回空，否则返回
					 * arg[0](与有 NULL 时同，故复用 b_hasnull)
					 *
					 */
					v_argsequal = LLVMBuildAnd(b,
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_fcinfo_isnull,
															 l_sbool_const(0),
															 ""),
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_retval,
															 l_sizet_const(1),
															 ""),
											   "");
					LLVMBuildCondBr(b, v_argsequal, b_argsequal, b_hasnull);

					/* 参数相等时把结果置 NULL 的块 */
					LLVMPositionBuilderAtEnd(b, b_argsequal);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SQLVALUEFUNCTION:
				build_EvalXFunc(b, mod, "ExecEvalSQLValueFunction",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CURRENTOFEXPR:
				build_EvalXFunc(b, mod, "ExecEvalCurrentOfExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NEXTVALUEEXPR:
				build_EvalXFunc(b, mod, "ExecEvalNextValueExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYEXPR:
				build_EvalXFunc(b, mod, "ExecEvalArrayExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYCOERCE:
				build_EvalXFunc(b, mod, "ExecEvalArrayCoerce",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ROW:
				build_EvalXFunc(b, mod, "ExecEvalRow",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ROWCOMPARE_STEP:
				{
					FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
					LLVMValueRef v_fcinfo_isnull;
					LLVMBasicBlockRef b_null;
					LLVMBasicBlockRef b_compare;
					LLVMBasicBlockRef b_compare_result;

					LLVMValueRef v_retval;

					b_null = l_bb_before_v(opblocks[opno + 1],
										   "op.%d.row-null", opno);
					b_compare = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.row-compare", opno);
					b_compare_result =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.row-compare-result",
									  opno);

					/*
					 * 若函数是 strict 且任一参数为空则结束
					 *
					 */
					if (op->d.rowcompare_step.finfo->fn_strict)
					{
						LLVMValueRef v_fcinfo;
						LLVMValueRef v_argnull0;
						LLVMValueRef v_argnull1;
						LLVMValueRef v_anyargisnull;

						v_fcinfo = l_ptr_const(fcinfo,
											   l_ptr(StructFunctionCallInfoData));

						v_argnull0 = l_funcnull(b, v_fcinfo, 0);
						v_argnull1 = l_funcnull(b, v_fcinfo, 1);

						v_anyargisnull =
							LLVMBuildOr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argnull0,
													  l_sbool_const(1),
													  ""),
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_argnull1,
													  l_sbool_const(1), ""),
										"");

						LLVMBuildCondBr(b, v_anyargisnull, b_null, b_compare);
					}
					else
					{
						LLVMBuildBr(b, b_compare);
					}

					/* 建调用比较函数的块 */
					LLVMPositionBuilderAtEnd(b, b_compare);

					/* 调用函数 */
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					/* 若函数结果为 NULL，强制结果为 NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_fcinfo_isnull,
												  l_sbool_const(0),
												  ""),
									b_compare_result,
									b_null);

					/* 建分析非空比较器结果的块 */
					LLVMPositionBuilderAtEnd(b, b_compare_result);

					/* 若结果相等则比较下一对，否则完 */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_retval,
												  l_sizet_const(0), ""),
									opblocks[opno + 1],
									opblocks[op->d.rowcompare_step.jumpdone]);

					/*
					 * 处理 NULL 输入或比较器结果为 NULL 块
					 *
					 */
					LLVMPositionBuilderAtEnd(b, b_null);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildBr(b, opblocks[op->d.rowcompare_step.jumpnull]);

					break;
				}

			case EEOP_ROWCOMPARE_FINAL:
				{
					RowCompareType rctype = op->d.rowcompare_final.rctype;

					LLVMValueRef v_cmpresult;
					LLVMValueRef v_result;
					LLVMIntPredicate predicate;

					/*
					 * Btree 比较器返回 32 位，须当心符号(
					 * 64 位值用会错）。
					 *
					 */
					v_cmpresult =
						LLVMBuildTrunc(b,
									   l_load(b, TypeSizeT, v_resvaluep, ""),
									   LLVMInt32TypeInContext(lc), "");

					switch (rctype)
					{
						case ROWCOMPARE_LT:
							predicate = LLVMIntSLT;
							break;
						case ROWCOMPARE_LE:
							predicate = LLVMIntSLE;
							break;
						case ROWCOMPARE_GT:
							predicate = LLVMIntSGT;
							break;
						case ROWCOMPARE_GE:
							predicate = LLVMIntSGE;
							break;
						default:
							/* 此处不允许 EQ 与 NE */
							Assert(false);
							predicate = 0;	/* 防止编译器警告 */
							break;
					}

					v_result = LLVMBuildICmp(b,
											 predicate,
											 v_cmpresult,
											 l_int32_const(lc, 0),
											 "");
					v_result = LLVMBuildZExt(b, v_result, TypeSizeT, "");

					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MINMAX:
				build_EvalXFunc(b, mod, "ExecEvalMinMax",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSELECT:
				build_EvalXFunc(b, mod, "ExecEvalFieldSelect",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_DEFORM:
				build_EvalXFunc(b, mod, "ExecEvalFieldStoreDeForm",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_FORM:
				build_EvalXFunc(b, mod, "ExecEvalFieldStoreForm",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DOMAIN_TESTVAL:
				{
					LLVMBasicBlockRef b_avail,
								b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casenullp,
								v_casenull;
					LLVMValueRef v_casevaluenull;

					b_avail = l_bb_before_v(opblocks[opno + 1],
											"op.%d.avail", opno);
					b_notavail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.notavail", opno);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(TypeSizeT));
					v_casenullp = l_ptr_const(op->d.casetest.isnull,
											  l_ptr(TypeStorageBool));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b,
									v_casevaluenull,
									b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = l_load(b, TypeSizeT, v_casevaluep, "");
					v_casenull = l_load(b, TypeStorageBool, v_casenullp, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_DOMAINDATUM,
										  "");
					v_casenull =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_DOMAINNULL,
										  "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_DOMAIN_NOTNULL:
				build_EvalXFunc(b, mod, "ExecEvalConstraintNotNull",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DOMAIN_CHECK:
				build_EvalXFunc(b, mod, "ExecEvalConstraintCheck",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CONVERT_ROWTYPE:
				build_EvalXFunc(b, mod, "ExecEvalConvertRowtype",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SCALARARRAYOP:
				build_EvalXFunc(b, mod, "ExecEvalScalarArrayOp",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_HASHED_SCALARARRAYOP:
				build_EvalXFunc(b, mod, "ExecEvalHashedScalarArrayOp",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_XMLEXPR:
				build_EvalXFunc(b, mod, "ExecEvalXmlExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_JSON_CONSTRUCTOR:
				build_EvalXFunc(b, mod, "ExecEvalJsonConstructor",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_IS_JSON:
				build_EvalXFunc(b, mod, "ExecEvalJsonIsPredicate",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGGREF:
				{
					LLVMValueRef v_aggno;
					LLVMValueRef value,
								isnull;

					v_aggno = l_int32_const(lc, op->d.aggref.aggno);

					/* 加载聚合值/空 */
					value = l_load_gep1(b, TypeSizeT, v_aggvalues, v_aggno, "aggvalue");
					isnull = l_load_gep1(b, TypeStorageBool, v_aggnulls, v_aggno, "aggnull");

					/* 并存储结果 */
					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_GROUPING_FUNC:
				build_EvalXFunc(b, mod, "ExecEvalGroupingFunc",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_WINDOW_FUNC:
				{
					WindowFuncExprState *wfunc = op->d.window_func.wfstate;
					LLVMValueRef v_wfuncnop;
					LLVMValueRef v_wfuncno;
					LLVMValueRef value,
								isnull;

					/*
					 * 此时 aggref->wfuncno 尚未设置
					 * 由 ExecInitWindowAgg() 初始化后设置
					 * 故每次从内存加载。
					 */
					v_wfuncnop = l_ptr_const(&wfunc->wfuncno,
											 l_ptr(LLVMInt32TypeInContext(lc)));
					v_wfuncno = l_load(b, LLVMInt32TypeInContext(lc), v_wfuncnop, "v_wfuncno");

					/* 加载窗口函数值/空值 */
					value = l_load_gep1(b, TypeSizeT, v_aggvalues, v_wfuncno,
										"windowvalue");
					isnull = l_load_gep1(b, TypeStorageBool, v_aggnulls, v_wfuncno,
										 "windownull");

					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SUBPLAN:
				build_EvalXFunc(b, mod, "ExecEvalSubPlan",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_STRICT_DESERIALIZE:
			case EEOP_AGG_DESERIALIZE:
				{
					AggState   *aggstate;
					FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

					LLVMValueRef v_retval;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_tmpcontext;
					LLVMValueRef v_oldcontext;

					if (opcode == EEOP_AGG_STRICT_DESERIALIZE)
					{
						LLVMValueRef v_fcinfo;
						LLVMValueRef v_argnull0;
						LLVMBasicBlockRef b_deserialize;

						b_deserialize = l_bb_before_v(opblocks[opno + 1],
													  "op.%d.deserialize", opno);

						v_fcinfo = l_ptr_const(fcinfo,
											   l_ptr(StructFunctionCallInfoData));
						v_argnull0 = l_funcnull(b, v_fcinfo, 0);

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argnull0,
													  l_sbool_const(1),
													  ""),
										opblocks[op->d.agg_deserialize.jumpnull],
										b_deserialize);
						LLVMPositionBuilderAtEnd(b, b_deserialize);
					}

					aggstate = castNode(AggState, state->parent);
					fcinfo = op->d.agg_deserialize.fcinfo_data;

					v_tmpcontext =
						l_ptr_const(aggstate->tmpcontext->ecxt_per_tuple_memory,
									l_ptr(StructMemoryContextData));
					v_oldcontext = l_mcxt_switch(mod, b, v_tmpcontext);
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					l_mcxt_switch(mod, b, v_oldcontext);

					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
				{
					int			nargs = op->d.agg_strict_input_check.nargs;
					NullableDatum *args = op->d.agg_strict_input_check.args;
					bool	   *nulls = op->d.agg_strict_input_check.nulls;
					int			jumpnull;

					LLVMValueRef v_argsp;
					LLVMValueRef v_nullsp;
					LLVMBasicBlockRef *b_checknulls;

					Assert(nargs > 0);

					jumpnull = op->d.agg_strict_input_check.jumpnull;
					v_argsp = l_ptr_const(args, l_ptr(StructNullableDatum));
					v_nullsp = l_ptr_const(nulls, l_ptr(TypeStorageBool));

					/* 为检查参数创建块 */
					b_checknulls = palloc(sizeof(LLVMBasicBlockRef) * nargs);
					for (int argno = 0; argno < nargs; argno++)
					{
						b_checknulls[argno] =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.check-null.%d",
										  opno, argno);
					}

					LLVMBuildBr(b, b_checknulls[0]);

					/* strict 函数，检查 NULL 参数 */
					for (int argno = 0; argno < nargs; argno++)
					{
						LLVMValueRef v_argno = l_int32_const(lc, argno);
						LLVMValueRef v_argisnull;
						LLVMBasicBlockRef b_argnotnull;

						LLVMPositionBuilderAtEnd(b, b_checknulls[argno]);

						if (argno + 1 == nargs)
							b_argnotnull = opblocks[opno + 1];
						else
							b_argnotnull = b_checknulls[argno + 1];

						if (opcode == EEOP_AGG_STRICT_INPUT_CHECK_NULLS)
							v_argisnull = l_load_gep1(b, TypeStorageBool, v_nullsp, v_argno, "");
						else
						{
							LLVMValueRef v_argn;

							v_argn = l_gep(b, StructNullableDatum, v_argsp, &v_argno, 1, "");
							v_argisnull =
								l_load_struct_gep(b, StructNullableDatum, v_argn,
												  FIELDNO_NULLABLE_DATUM_ISNULL,
												  "");
						}

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argisnull,
													  l_sbool_const(1), ""),
										opblocks[jumpnull],
										b_argnotnull);
					}

					break;
				}

			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
				{
					int			jumpnull;
					LLVMValueRef v_aggstatep;
					LLVMValueRef v_allpergroupsp;
					LLVMValueRef v_pergroup_allaggs;
					LLVMValueRef v_setoff;

					jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

					/*
					 * pergroup_allaggs = aggstate->all_pergroups
					 * [op->d.agg_plain_pergroup_nullcheck.setoff];
					 */
					v_aggstatep = LLVMBuildBitCast(b, v_parent,
												   l_ptr(StructAggState), "");

					v_allpergroupsp = l_load_struct_gep(b,
														StructAggState,
														v_aggstatep,
														FIELDNO_AGGSTATE_ALL_PERGROUPS,
														"aggstate.all_pergroups");

					v_setoff = l_int32_const(lc, op->d.agg_plain_pergroup_nullcheck.setoff);

					v_pergroup_allaggs = l_load_gep1(b, l_ptr(StructAggStatePerGroupData),
													 v_allpergroupsp, v_setoff, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ,
												  LLVMBuildPtrToInt(b, v_pergroup_allaggs, TypeSizeT, ""),
												  l_sizet_const(0), ""),
									opblocks[jumpnull],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_BYREF:
				{
					AggState   *aggstate;
					AggStatePerTrans pertrans;
					FunctionCallInfo fcinfo;

					LLVMValueRef v_aggstatep;
					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;

					LLVMValueRef v_transvaluep;
					LLVMValueRef v_transnullp;

					LLVMValueRef v_setoff;
					LLVMValueRef v_transno;

					LLVMValueRef v_aggcontext;

					LLVMValueRef v_allpergroupsp;
					LLVMValueRef v_current_setp;
					LLVMValueRef v_current_pertransp;
					LLVMValueRef v_curaggcontext;

					LLVMValueRef v_pertransp;

					LLVMValueRef v_pergroupp;

					LLVMValueRef v_retval;

					LLVMValueRef v_tmpcontext;
					LLVMValueRef v_oldcontext;

					aggstate = castNode(AggState, state->parent);
					pertrans = op->d.agg_trans.pertrans;

					fcinfo = pertrans->transfn_fcinfo;

					v_aggstatep =
						LLVMBuildBitCast(b, v_parent, l_ptr(StructAggState), "");
					v_pertransp = l_ptr_const(pertrans,
											  l_ptr(StructAggStatePerTransData));

					/*
					 * pergroup = &aggstate->all_pergroups
					 * [op->d.agg_trans.setoff] [op->d.agg_trans.transno];
					 */
					v_allpergroupsp =
						l_load_struct_gep(b,
										  StructAggState,
										  v_aggstatep,
										  FIELDNO_AGGSTATE_ALL_PERGROUPS,
										  "aggstate.all_pergroups");
					v_setoff = l_int32_const(lc, op->d.agg_trans.setoff);
					v_transno = l_int32_const(lc, op->d.agg_trans.transno);
					v_pergroupp =
						l_gep(b,
							  StructAggStatePerGroupData,
							  l_load_gep1(b, l_ptr(StructAggStatePerGroupData),
										  v_allpergroupsp, v_setoff, ""),
							  &v_transno, 1, "");


					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
					{
						LLVMValueRef v_notransvalue;
						LLVMBasicBlockRef b_init;
						LLVMBasicBlockRef b_no_init;

						v_notransvalue =
							l_load_struct_gep(b,
											  StructAggStatePerGroupData,
											  v_pergroupp,
											  FIELDNO_AGGSTATEPERGROUPDATA_NOTRANSVALUE,
											  "notransvalue");

						b_init = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.inittrans", opno);
						b_no_init = l_bb_before_v(opblocks[opno + 1],
												  "op.%d.no_inittrans", opno);

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_notransvalue,
													  l_sbool_const(1), ""),
										b_init,
										b_no_init);

						/* 必要时初始化转移值的块 */
						{
							LLVMValueRef params[4];

							LLVMPositionBuilderAtEnd(b, b_init);

							v_aggcontext = l_ptr_const(op->d.agg_trans.aggcontext,
													   l_ptr(StructExprContext));

							params[0] = v_aggstatep;
							params[1] = v_pertransp;
							params[2] = v_pergroupp;
							params[3] = v_aggcontext;

							l_call(b,
								   llvm_pg_var_func_type("ExecAggInitGroup"),
								   llvm_pg_func(mod, "ExecAggInitGroup"),
								   params, lengthof(params),
								   "");

							LLVMBuildBr(b, opblocks[opno + 1]);
						}

						LLVMPositionBuilderAtEnd(b, b_no_init);
					}

					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
					{
						LLVMValueRef v_transnull;
						LLVMBasicBlockRef b_strictpass;

						b_strictpass = l_bb_before_v(opblocks[opno + 1],
													 "op.%d.strictpass", opno);
						v_transnull =
							l_load_struct_gep(b,
											  StructAggStatePerGroupData,
											  v_pergroupp,
											  FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL,
											  "transnull");

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_transnull,
													  l_sbool_const(1), ""),
										opblocks[opno + 1],
										b_strictpass);

						LLVMPositionBuilderAtEnd(b, b_strictpass);
					}


					v_fcinfo = l_ptr_const(fcinfo,
										   l_ptr(StructFunctionCallInfoData));
					v_aggcontext = l_ptr_const(op->d.agg_trans.aggcontext,
											   l_ptr(StructExprContext));

					v_current_setp =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURRENT_SET,
									 "aggstate.current_set");
					v_curaggcontext =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURAGGCONTEXT,
									 "aggstate.curaggcontext");
					v_current_pertransp =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURPERTRANS,
									 "aggstate.curpertrans");

					/* 设 aggstate 全局 */
					LLVMBuildStore(b, v_aggcontext, v_curaggcontext);
					LLVMBuildStore(b, l_int32_const(lc, op->d.agg_trans.setno),
								   v_current_setp);
					LLVMBuildStore(b, v_pertransp, v_current_pertransp);

					/* 在 per-tuple 上下文中调用转移函数 */
					v_tmpcontext =
						l_ptr_const(aggstate->tmpcontext->ecxt_per_tuple_memory,
									l_ptr(StructMemoryContextData));
					v_oldcontext = l_mcxt_switch(mod, b, v_tmpcontext);

					/* transvalue 存入 fcinfo->args[0] */
					v_transvaluep =
						l_struct_gep(b,
									 StructAggStatePerGroupData,
									 v_pergroupp,
									 FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUE,
									 "transvalue");
					v_transnullp =
						l_struct_gep(b,
									 StructAggStatePerGroupData,
									 v_pergroupp,
									 FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL,
									 "transnullp");
					LLVMBuildStore(b,
								   l_load(b,
										  TypeSizeT,
										  v_transvaluep,
										  "transvalue"),
								   l_funcvaluep(b, v_fcinfo, 0));
					LLVMBuildStore(b,
								   l_load(b, TypeStorageBool, v_transnullp, "transnull"),
								   l_funcnullp(b, v_fcinfo, 0));

					/* 并调用转移函数 */
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);

					/*
					 * 按引用类型须拷新值进 aggcontext 并释放
					 * 先前 transValue。若 transfn 返回指向其
					 * 第一输入指针则不必做事。若 transfn 返回
					 * 已是 aggcontext 子对象的读写扩展
					 * 对象指针，则假定可收养该值
					 * 不必拷贝。
					 *
					 */
					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_BYREF)
					{
						LLVMBasicBlockRef b_call;
						LLVMBasicBlockRef b_nocall;
						LLVMValueRef v_fn;
						LLVMValueRef v_transvalue;
						LLVMValueRef v_transnull;
						LLVMValueRef v_newval;
						LLVMValueRef params[6];

						b_call = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.transcall", opno);
						b_nocall = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.transnocall", opno);

						v_transvalue = l_load(b, TypeSizeT, v_transvaluep, "");
						v_transnull = l_load(b, TypeStorageBool, v_transnullp, "");

						/*
						 * DatumGetPointer(newVal) !=
						 * DatumGetPointer(pergroup->transValue))
						 */
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_transvalue,
													  v_retval, ""),
										b_nocall, b_call);

						/* 返回 datum 不是传入的，须改父 */
						LLVMPositionBuilderAtEnd(b, b_call);

						params[0] = v_aggstatep;
						params[1] = v_pertransp;
						params[2] = v_retval;
						params[3] = LLVMBuildTrunc(b, v_fcinfo_isnull,
												   TypeParamBool, "");
						params[4] = v_transvalue;
						params[5] = LLVMBuildTrunc(b, v_transnull,
												   TypeParamBool, "");

						v_fn = llvm_pg_func(mod, "ExecAggCopyTransValue");
						v_newval =
							l_call(b,
								   LLVMGetFunctionType(v_fn),
								   v_fn,
								   params, lengthof(params),
								   "");

						/* 存 trans 值 */
						LLVMBuildStore(b, v_newval, v_transvaluep);
						LLVMBuildStore(b, v_fcinfo_isnull, v_transnullp);

						l_mcxt_switch(mod, b, v_oldcontext);
						LLVMBuildBr(b, opblocks[opno + 1]);

						/* 返回的 datum 即传入的，不必改父 */
						LLVMPositionBuilderAtEnd(b, b_nocall);
					}

					/* 存 trans 值 */
					LLVMBuildStore(b, v_retval, v_transvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_transnullp);

					l_mcxt_switch(mod, b, v_oldcontext);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
				{
					AggState   *aggstate = castNode(AggState, state->parent);
					AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
					int			jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;

					LLVMValueRef v_fn = llvm_pg_func(mod, "ExecEvalPreOrderedDistinctSingle");
					LLVMValueRef v_args[2];
					LLVMValueRef v_ret;

					v_args[0] = l_ptr_const(aggstate, l_ptr(StructAggState));
					v_args[1] = l_ptr_const(pertrans, l_ptr(StructAggStatePerTransData));

					v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, v_args, 2, "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdistinct]);
					break;
				}

			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
				{
					AggState   *aggstate = castNode(AggState, state->parent);
					AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
					int			jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;

					LLVMValueRef v_fn = llvm_pg_func(mod, "ExecEvalPreOrderedDistinctMulti");
					LLVMValueRef v_args[2];
					LLVMValueRef v_ret;

					v_args[0] = l_ptr_const(aggstate, l_ptr(StructAggState));
					v_args[1] = l_ptr_const(pertrans, l_ptr(StructAggStatePerTransData));

					v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, v_args, 2, "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdistinct]);
					break;
				}

			case EEOP_AGG_ORDERED_TRANS_DATUM:
				build_EvalXFunc(b, mod, "ExecEvalAggOrderedTransDatum",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_ORDERED_TRANS_TUPLE:
				build_EvalXFunc(b, mod, "ExecEvalAggOrderedTransTuple",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_LAST:
				Assert(false);
				break;
		}
	}

	LLVMDisposeBuilder(b);

	/*
	 * 不立刻发函数，等表达式真正求值时第一次再发
	 * 这样可把许多函数一起发出，避免反复 llvm 与
	 * 内存重映射开销。
	 *
	 */
	{

		CompiledExprState *cstate = palloc0(sizeof(CompiledExprState));

		cstate->context = context;
		cstate->funcname = funcname;

		state->evalfunc = ExecRunCompiledExpr;
		state->evalfunc_private = cstate;
	}

	llvm_leave_fatal_on_oom();

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.generation_counter,
						  endtime, starttime);

	return true;
}

/*
 * 运行已编译表达式
 *
 * 仅在 JIT 表达式第一次被调用时走这里。先确认表达式
 * 仍是最新的，再取已发函数的指针。后者可能是第一次
 * 真正触发优化并发出全部已生成函数。
 *
 */
static Datum
ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull)
{
	CompiledExprState *cstate = state->evalfunc_private;
	ExprStateEvalFunc func;

	CheckExprStillValid(state, econtext);

	llvm_enter_fatal_on_oom();
	func = (ExprStateEvalFunc) llvm_get_function(cstate->context,
												 cstate->funcname);
	llvm_leave_fatal_on_oom();
	Assert(func);

	/* 以后调用不再经本函数间接 */
	state->evalfunc = func;

	return func(state, econtext, isNull);
}

static LLVMValueRef
BuildV1Call(LLVMJitContext *context, LLVMBuilderRef b,
			LLVMModuleRef mod, FunctionCallInfo fcinfo,
			LLVMValueRef *v_fcinfo_isnull)
{
	LLVMContextRef lc;
	LLVMValueRef v_fn;
	LLVMValueRef v_fcinfo_isnullp;
	LLVMValueRef v_retval;
	LLVMValueRef v_fcinfo;

	lc = LLVMGetModuleContext(mod);

	v_fn = llvm_function_reference(context, b, mod, fcinfo);

	v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));
	v_fcinfo_isnullp = l_struct_gep(b,
									StructFunctionCallInfoData,
									v_fcinfo,
									FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
									"v_fcinfo_isnull");
	LLVMBuildStore(b, l_sbool_const(0), v_fcinfo_isnullp);

	v_retval = l_call(b, LLVMGetFunctionType(AttributeTemplate), v_fn, &v_fcinfo, 1, "funccall");

	if (v_fcinfo_isnull)
		*v_fcinfo_isnull = l_load(b, TypeStorageBool, v_fcinfo_isnullp, "");

	/*
	 * 加 lifetime-end 注解，表明对内存的写不必保留
	 * 对内联潜力很重要。
	 */
	{
		LLVMValueRef v_lifetime = create_LifetimeEnd(mod);
		LLVMValueRef params[2];

		params[0] = l_int64_const(lc, sizeof(NullableDatum) * fcinfo->nargs);
		params[1] = l_ptr_const(fcinfo->args, l_ptr(LLVMInt8TypeInContext(lc)));
		l_call(b, LLVMGetFunctionType(v_lifetime), v_lifetime, params, lengthof(params), "");

		params[0] = l_int64_const(lc, sizeof(fcinfo->isnull));
		params[1] = l_ptr_const(&fcinfo->isnull, l_ptr(LLVMInt8TypeInContext(lc)));
		l_call(b, LLVMGetFunctionType(v_lifetime), v_lifetime, params, lengthof(params), "");
	}

	return v_retval;
}

/*
 * 通过调用 funcname 实现一个表达式步骤。
 */
static LLVMValueRef
build_EvalXFuncInt(LLVMBuilderRef b, LLVMModuleRef mod, const char *funcname,
				   LLVMValueRef v_state, ExprEvalStep *op,
				   int nargs, LLVMValueRef *v_args)
{
	LLVMValueRef v_fn = llvm_pg_func(mod, funcname);
	LLVMValueRef *params;
	int			argno = 0;
	LLVMValueRef v_ret;

	/* 廉价预检查，因 llvm 只会断言 */
	if (LLVMCountParams(v_fn) != (nargs + 2))
		elog(ERROR, "parameter mismatch: %s expects %d passed %d",
			 funcname, LLVMCountParams(v_fn), nargs + 2);

	params = palloc(sizeof(LLVMValueRef) * (2 + nargs));

	params[argno++] = v_state;
	params[argno++] = l_ptr_const(op, l_ptr(StructExprEvalStep));

	for (int i = 0; i < nargs; i++)
		params[argno++] = v_args[i];

	v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, params, argno, "");

	pfree(params);

	return v_ret;
}

static LLVMValueRef
create_LifetimeEnd(LLVMModuleRef mod)
{
	LLVMTypeRef sig;
	LLVMValueRef fn;
	LLVMTypeRef param_types[2];
	LLVMContextRef lc;

	/* LLVM 5+ 有可变参数指针 */
#if LLVM_VERSION_MAJOR < 5
	const char *nm = "llvm.lifetime.end";
#else
	const char *nm = "llvm.lifetime.end.p0i8";
#endif

	fn = LLVMGetNamedFunction(mod, nm);
	if (fn)
		return fn;

	lc = LLVMGetModuleContext(mod);
	param_types[0] = LLVMInt64TypeInContext(lc);
	param_types[1] = l_ptr(LLVMInt8TypeInContext(lc));

	sig = LLVMFunctionType(LLVMVoidTypeInContext(lc), param_types,
						   lengthof(param_types), false);
	fn = LLVMAddFunction(mod, nm, sig);

	LLVMSetFunctionCallConv(fn, LLVMCCallConv);

	Assert(LLVMGetIntrinsicID(fn));

	return fn;
}
