/*-------------------------------------------------------------------------
 *
 * execExprInterp.c
 *	  表达式步骤列表的解释执行。           
 *
 * 本文件提供表达式求值的"直接线程化"(gcc、        
 * clang 及兼容编译器）或"switch 线程化"（所有编译器）   
 * 实现。前者是已知最快的解释方法之一，                 
 * 无需汇编级工作或即时编译即可解释执行程序，      
 * 但需要支持计算 goto。                                        
 * 后者是标准 C 可做到的最快方法之一。               
 *
 * 两种情形均用 ExprEvalStep->opcode 分派至 ExecInterpExpr() 内  
 * 实现该特定操作码类型的代码块。                    
 *
 * switch 线程化用普通 switch() 语句执行分派。       
 * 分派。其优点是纯 C 实现，并且允许编译器           
 * 对某个特定操作码的实现被遗漏时发出警告。               
 * 缺点是正如编译器通常实现的那样，分派都发生     
 * 在单一位置，需要更多跳转并导致                             
 * 分支预测糟糕。 
 *
 * 直接线程化使用 gcc 的 label-as-values 扩展（也被采纳）， 
 * 一些编译器 - 把 ExprEvalStep->opcode 替换为实现该指令       
 * 的代码块地址。到下一条指令的分派由"计算 goto"完成。  
 * 这带来更好的分支预测（因为跳转来自不同位置）     
 * 以及更少的跳转，因为无需先跳到公共分派位置      
 * 做预备跳转。                                               
 *
 * 直接线程化下，ExecReadyInterpretedExpr 会把每步的     
 * opcode 字段替换为相应代码块地址，且 ExprState->flags 中  
 * 会包含 EEO_FLAG_DIRECT_THREADED 以记住已做                     
 * 此替换。     
 *
 * 对非常简单的指令，完整解释器"启动"的开销      
 * 虽小但可察觉。因此                              
 * ExecReadyInterpretedExpr 选择用快速路径例程(ExecJust*) 
 * 来实现某些简单的操作码模式。                   
 *
 * 复杂或罕见指令不在 ExecInterpExpr() 内联实现，   
 * 而是调用本文件后面出现的辅助函数。                      
 * 一方面没有明显性能收益，                                  
 * 但更重要的是这些复杂例程要在不同表达式求值        
 * 方法之间共享。例如 JIT 编译器会生成对它们的调用    
 * （这就是它们被导出而非在本文件中设为             
 * static 的原因。）                            
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExprInterp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

/*
 * 在计算 goto 可用时用基于计算 goto 的操作码分派，        
 * 但用单独符号，以便在本文件内为开发测试                 
 * 而局部调整。          
 */
#ifdef HAVE_COMPUTED_GOTO
#define EEO_USE_COMPUTED_GOTO
#endif							/* HAVE_COMPUTED_GOTO */

/*
 * 用于操作码分派的宏
 *
 * EEO_SWITCH - 不用时仅隐藏 switch。        
 * EEO_CASE - 标注命名表达式步骤类型的实现。           
 * EEO_DISPATCH - 跳转到 'op' 步骤类型的实现。              
 * EEO_OPCODE - 按所用表达式求值方法计算所需操作码。       
 * EEO_NEXT - 递增 'op' 并跳到正确的下一步类型。  
 * EEO_JUMP - 跳到当前表达式内指定的步骤编号。                
 */
#if defined(EEO_USE_COMPUTED_GOTO)

/* 跳转目标 -> 操作码查找表的结构体*/
typedef struct ExprEvalOpLookup
{
	const void *opcode;
	ExprEvalOp	op;
} ExprEvalOpLookup;

/* 使 dispatch_table 能在 ExecInterpExpr() 之外访问    */
static const void **dispatch_table = NULL;

/* 跳转目标 -> 操作码查找表 */
static ExprEvalOpLookup reverse_dispatch_table[EEOP_LAST];

#define EEO_SWITCH()
#define EEO_CASE(name)		CASE_##name:
#define EEO_DISPATCH()		goto *((void *) op->opcode)
#define EEO_OPCODE(opcode)	((intptr_t) dispatch_table[opcode])

#else							/* !EEO_USE_COMPUTED_GOTO */

#define EEO_SWITCH()		starteval: switch ((ExprEvalOp) op->opcode)
#define EEO_CASE(name)		case name:
#define EEO_DISPATCH()		goto starteval
#define EEO_OPCODE(opcode)	(opcode)

#endif							/* EEO_USE_COMPUTED_GOTO */

#define EEO_NEXT() \
	do { \
		op++; \
		EEO_DISPATCH(); \
	} while (0)

#define EEO_JUMP(stepno) \
	do { \
		op = &state->steps[stepno]; \
		EEO_DISPATCH(); \
	} while (0)


static Datum ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull);
static void ExecInitInterpreter(void);

/* 支持函数      */
static void CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype);
static void CheckOpSlotCompatibility(ExprEvalStep *op, TupleTableSlot *slot);
static TupleDesc get_cached_rowtype(Oid type_id, int32 typmod,
									ExprEvalRowtypeCache *rowcache,
									bool *changed);
static void ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
							   ExprContext *econtext, bool checkisnull);

/* 快速路径求值函数       */
static Datum ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);

/* 执行辅助函数         */
static pg_attribute_always_inline void ExecAggPlainTransByVal(AggState *aggstate,
															  AggStatePerTrans pertrans,
															  AggStatePerGroup pergroup,
															  ExprContext *aggcontext,
															  int setno);
static pg_attribute_always_inline void ExecAggPlainTransByRef(AggState *aggstate,
															  AggStatePerTrans pertrans,
															  AggStatePerGroup pergroup,
															  ExprContext *aggcontext,
															  int setno);

/*
 * ScalarArrayOpExprHashEntry
 * 		EEOP_HASHED_SCALARARRAYOP 期间用的哈希表条目类型
 */
typedef struct ScalarArrayOpExprHashEntry
{
	Datum		key;
	uint32		status;			/* 哈希状态*/
	uint32		hash;			/* 已缓存哈希值  */
} ScalarArrayOpExprHashEntry;

#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static bool saop_hash_element_match(struct saophash_hash *tb, Datum key1,
									Datum key2);
static uint32 saop_element_hash(struct saophash_hash *tb, Datum key);

/*
 * ScalarArrayOpExprHashTable
 *		EEOP_HASHED_SCALARARRAYOP 的哈希表  
 */
typedef struct ScalarArrayOpExprHashTable
{
	saophash_hash *hashtab;		/* 底层哈希表       */
	struct ExprEvalStep *op;
	FmgrInfo	hash_finfo;		/* 函数的查找数据  */
	FunctionCallInfoBaseData hash_fcinfo_data;	/* 参数等     */
} ScalarArrayOpExprHashTable;

/* 为 ScalarArrayOpExpr 哈希表代码生成定义参数。          */
#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_KEY key
#define SH_HASH_KEY(tb, key) saop_element_hash(tb, key)
#define SH_EQUAL(tb, a, b) saop_hash_element_match(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * 准备 ExprState 以进行解释执行。   
 */
void
ExecReadyInterpretedExpr(ExprState *state)
{
	/* 确保一次性解释器初始化已完成      */
	ExecInitInterpreter();

	/* 对表达式的简单有效性检查 */
	Assert(state->steps_len >= 1);
	Assert(state->steps[state->steps_len - 1].opcode == EEOP_DONE);

	/*
	 * 不要做冗余初始化。这在当前情形下不可达，          
	 * 但若存在额外的表达式求值方法，即依赖             
	 * 解释执行才能工作，则可能命中此处。
	 */
	if (state->flags & EEO_FLAG_INTERPRETER_INITIALIZED)
		return;

	/*
	 * 首次执行时检查属性是否仍匹配 Var；由于模式变更，
	 * 它可能不再匹配。为此设置一个回调，在首次调用    
	 * 时做检查，随后把 evalfunc 回调设为                    
	 * 实际执行方法。                      
	 */
	state->evalfunc = ExecInterpExprStillValid;

	/* DIRECT_THREADED 尚不应已被设置     */
	Assert((state->flags & EEO_FLAG_DIRECT_THREADED) == 0);

	/*
	 * 表达式完全初始化之前不应有任何错误；      
	 * 即便有，也只会导致表达式被放弃。              
	 * 因此可以现在就设置标志，省去一些代码。 
	 */
	state->flags |= EEO_FLAG_INTERPRETER_INITIALIZED;

	/*
	 * 为极简表达式选择快速路径 evalfunc。对它们而言，    
	 * 完整解释器的"启动"开销可测量，且这些模式       
	 * 出现够频繁，值得优化。                  
	 */
	if (state->steps_len == 3)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustScanVar;
			return;
		}
		else if (step0 == EEOP_INNER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignScanVar;
			return;
		}
		else if (step0 == EEOP_CASE_TESTVAL &&
				 step1 == EEOP_FUNCEXPR_STRICT &&
				 state->steps[0].d.casetest.value)
		{
			state->evalfunc_private = (void *) ExecJustApplyFuncToCase;
			return;
		}
	}
	else if (state->steps_len == 2)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;

		if (step0 == EEOP_CONST)
		{
			state->evalfunc_private = (void *) ExecJustConst;
			return;
		}
		else if (step0 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustScanVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignScanVarVirt;
			return;
		}
	}

#if defined(EEO_USE_COMPUTED_GOTO)

	/*
	 * 直接线程化中，把每个操作码替换为跳转地址。    
	 * （用 ExecEvalStepOp() 取回操作码。）                      
	 */
	for (int off = 0; off < state->steps_len; off++)
	{
		ExprEvalStep *op = &state->steps[off];

		op->opcode = EEO_OPCODE(op->opcode);
	}

	state->flags |= EEO_FLAG_DIRECT_THREADED;
#endif							/* EEO_USE_COMPUTED_GOTO */

	state->evalfunc_private = (void *) ExecInterpExpr;
}


/*
 * 在 "econtext" 给出的执行上下文中求值 "state"          
 * 标识的表达式。*isnull 设为结果的 is-null 标志，          
 * Datum 值即函数结果。                
 *
 * 特例：state 为 NULL 时返回分派表地址。ExecInitInterpreter  
 * 用它设置 dispatch_table 全局变量。（仅在定义了           
 * EEO_USE_COMPUTED_GOTO 时适用。）                
 */
static Datum
ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op;
	TupleTableSlot *resultslot;
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;

	/*
	 * 此数组必须与 enum ExprEvalOp 同序。              
	 */
#if defined(EEO_USE_COMPUTED_GOTO)
	static const void *const dispatch_table[] = {
		&&CASE_EEOP_DONE,
		&&CASE_EEOP_INNER_FETCHSOME,
		&&CASE_EEOP_OUTER_FETCHSOME,
		&&CASE_EEOP_SCAN_FETCHSOME,
		&&CASE_EEOP_INNER_VAR,
		&&CASE_EEOP_OUTER_VAR,
		&&CASE_EEOP_SCAN_VAR,
		&&CASE_EEOP_INNER_SYSVAR,
		&&CASE_EEOP_OUTER_SYSVAR,
		&&CASE_EEOP_SCAN_SYSVAR,
		&&CASE_EEOP_WHOLEROW,
		&&CASE_EEOP_ASSIGN_INNER_VAR,
		&&CASE_EEOP_ASSIGN_OUTER_VAR,
		&&CASE_EEOP_ASSIGN_SCAN_VAR,
		&&CASE_EEOP_ASSIGN_TMP,
		&&CASE_EEOP_ASSIGN_TMP_MAKE_RO,
		&&CASE_EEOP_CONST,
		&&CASE_EEOP_FUNCEXPR,
		&&CASE_EEOP_FUNCEXPR_STRICT,
		&&CASE_EEOP_FUNCEXPR_FUSAGE,
		&&CASE_EEOP_FUNCEXPR_STRICT_FUSAGE,
		&&CASE_EEOP_BOOL_AND_STEP_FIRST,
		&&CASE_EEOP_BOOL_AND_STEP,
		&&CASE_EEOP_BOOL_AND_STEP_LAST,
		&&CASE_EEOP_BOOL_OR_STEP_FIRST,
		&&CASE_EEOP_BOOL_OR_STEP,
		&&CASE_EEOP_BOOL_OR_STEP_LAST,
		&&CASE_EEOP_BOOL_NOT_STEP,
		&&CASE_EEOP_QUAL,
		&&CASE_EEOP_JUMP,
		&&CASE_EEOP_JUMP_IF_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_TRUE,
		&&CASE_EEOP_NULLTEST_ISNULL,
		&&CASE_EEOP_NULLTEST_ISNOTNULL,
		&&CASE_EEOP_NULLTEST_ROWISNULL,
		&&CASE_EEOP_NULLTEST_ROWISNOTNULL,
		&&CASE_EEOP_BOOLTEST_IS_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_FALSE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_FALSE,
		&&CASE_EEOP_PARAM_EXEC,
		&&CASE_EEOP_PARAM_EXTERN,
		&&CASE_EEOP_PARAM_CALLBACK,
		&&CASE_EEOP_CASE_TESTVAL,
		&&CASE_EEOP_MAKE_READONLY,
		&&CASE_EEOP_IOCOERCE,
		&&CASE_EEOP_DISTINCT,
		&&CASE_EEOP_NOT_DISTINCT,
		&&CASE_EEOP_NULLIF,
		&&CASE_EEOP_SQLVALUEFUNCTION,
		&&CASE_EEOP_CURRENTOFEXPR,
		&&CASE_EEOP_NEXTVALUEEXPR,
		&&CASE_EEOP_ARRAYEXPR,
		&&CASE_EEOP_ARRAYCOERCE,
		&&CASE_EEOP_ROW,
		&&CASE_EEOP_ROWCOMPARE_STEP,
		&&CASE_EEOP_ROWCOMPARE_FINAL,
		&&CASE_EEOP_MINMAX,
		&&CASE_EEOP_FIELDSELECT,
		&&CASE_EEOP_FIELDSTORE_DEFORM,
		&&CASE_EEOP_FIELDSTORE_FORM,
		&&CASE_EEOP_SBSREF_SUBSCRIPTS,
		&&CASE_EEOP_SBSREF_OLD,
		&&CASE_EEOP_SBSREF_ASSIGN,
		&&CASE_EEOP_SBSREF_FETCH,
		&&CASE_EEOP_DOMAIN_TESTVAL,
		&&CASE_EEOP_DOMAIN_NOTNULL,
		&&CASE_EEOP_DOMAIN_CHECK,
		&&CASE_EEOP_CONVERT_ROWTYPE,
		&&CASE_EEOP_SCALARARRAYOP,
		&&CASE_EEOP_HASHED_SCALARARRAYOP,
		&&CASE_EEOP_XMLEXPR,
		&&CASE_EEOP_JSON_CONSTRUCTOR,
		&&CASE_EEOP_IS_JSON,
		&&CASE_EEOP_AGGREF,
		&&CASE_EEOP_GROUPING_FUNC,
		&&CASE_EEOP_WINDOW_FUNC,
		&&CASE_EEOP_SUBPLAN,
		&&CASE_EEOP_AGG_STRICT_DESERIALIZE,
		&&CASE_EEOP_AGG_DESERIALIZE,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_NULLS,
		&&CASE_EEOP_AGG_PLAIN_PERGROUP_NULLCHECK,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYREF,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_SINGLE,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_MULTI,
		&&CASE_EEOP_AGG_ORDERED_TRANS_DATUM,
		&&CASE_EEOP_AGG_ORDERED_TRANS_TUPLE,
		&&CASE_EEOP_LAST
	};

	StaticAssertDecl(lengthof(dispatch_table) == EEOP_LAST + 1,
					 "dispatch_table out of whack with ExprEvalOp");

	if (unlikely(state == NULL))
		return PointerGetDatum(dispatch_table);
#else
	Assert(state != NULL);
#endif							/* EEO_USE_COMPUTED_GOTO */

	/* 建立状态*/
	op = state->steps;
	resultslot = state->resultslot;
	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

#if defined(EEO_USE_COMPUTED_GOTO)
	EEO_DISPATCH();
#endif

	EEO_SWITCH()
	{
		EEO_CASE(EEOP_DONE)
		{
			goto out;
		}

		EEO_CASE(EEOP_INNER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, innerslot);

			slot_getsomeattrs(innerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, outerslot);

			slot_getsomeattrs(outerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, scanslot);

			slot_getsomeattrs(scanslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/*
			 * 由于用 FETCHSOME 步骤提取了全部被引用列，  
			 * 可直接从 slot 的 decomposed-data 数组取值。  
			 * 但用一个 Assert 来检查                                
			 * 确实发生了这种情况。               
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			*op->resvalue = innerslot->tts_values[attnum];
			*op->resnull = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* 见 EEOP_INNER_VAR 注释   */

			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			*op->resvalue = outerslot->tts_values[attnum];
			*op->resnull = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* 见 EEOP_INNER_VAR 注释   */

			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			*op->resvalue = scanslot->tts_values[attnum];
			*op->resnull = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, innerslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, outerslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, scanslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_WHOLEROW)
		{
			/* 对内联实现而言过于复杂        */
			ExecEvalWholeRowVar(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_INNER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * 此处无需 CheckVarSlotCompatibility；编译期处理。  
			 * 但参见 EEOP_INNER_VAR 的注释。                         
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = innerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_OUTER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * 此处无需 CheckVarSlotCompatibility；编译期处理。  
			 * 但参见 EEOP_INNER_VAR 的注释。                         
			 */
			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = outerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_SCAN_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * 此处无需 CheckVarSlotCompatibility；编译期处理。  
			 * 但参见 EEOP_INNER_VAR 的注释。                         
			 */
			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = scanslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = state->resvalue;
			resultslot->tts_isnull[resultnum] = state->resnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP_MAKE_RO)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_isnull[resultnum] = state->resnull;
			if (!resultslot->tts_isnull[resultnum])
				resultslot->tts_values[resultnum] =
					MakeExpandedObjectReadOnlyInternal(state->resvalue);
			else
				resultslot->tts_values[resultnum] = state->resvalue;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONST)
		{
			*op->resnull = op->d.constval.isnull;
			*op->resvalue = op->d.constval.value;

			EEO_NEXT();
		}

		/*
		 * 函数调用的实现。参数此前已被直接求值       
		 * 到 fcinfo->args 中。              
		 *
		 * STRICT 检查和函数用法有明显性能影响，而且         
		 * 函数调用是极热路径（运算符也靠它），所以
		 * 值得设置这么多独立的操作码。              
		 *
		 * 注：此处及其他地方用临时变量 "d" 的原因是，  
		 * 某些编译器认为 "*op->resvalue = f();" 要求它们在     
		 * 调用 f() 之前先把 op->resvalue 求值入寄存器，   
		 * 以防 f() 以某种方式修改 op->resvalue。            
		 * 这多出的一行代码可省掉跨函数调用的一次无用   
		 * 寄存器溢出与重载。         
		 */
		EEO_CASE(EEOP_FUNCEXPR)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			Datum		d;

			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			NullableDatum *args = fcinfo->args;
			int			nargs = op->d.func.nargs;
			Datum		d;

			/* 严格函数，检查参数是否 NULL  */
			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
				{
					*op->resnull = true;
					goto strictfail;
				}
			}
			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

	strictfail:
			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_FUSAGE)
		{
			/* 不够常见，不必内联 */
			ExecEvalFuncExprFusage(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT_FUSAGE)
		{
			/* 不够常见，不必内联 */
			ExecEvalFuncExprStrictFusage(state, op, econtext);

			EEO_NEXT();
		}

		/*
		 * 若任一子句为 FALSE，则无论其余子句状态如何，AND 
		 * 的结果都是 FALSE，故可停止求值并立即返回 FALSE。 
		 * 若无 FALSE 但有一或多个 NULL，则返回 NULL；否则     
		 * 返回 TRUE。把 NULL 解读为"不知道"时这是合理的： 
		 * 也许某个"不知道"若已知其值本会是 FALSE。只有当 
		 * 所有输入都确知为 TRUE 时，我们才能确信          
		 * AND 的结果确实是                                           
		 *  TRUE。                 
		 */
		EEO_CASE(EEOP_BOOL_AND_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_AND_STEP_FIRST resets anynull, otherwise it's the
			 * same as EEOP_BOOL_AND_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_AND_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* 结果已设为 FALSE，无需修改               */
				/* 提前退出   */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_AND_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* 结果已设为 NULL，无需修改               */
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* 结果已设为 FALSE，无需修改               */

				/*
				 * 没必要提前跳到 jumpdone - 目标会相同         
				 * （因为这是 AND 表达式的末参数），      
				 * 只是更昂贵。    
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* 结果已设为 TRUE，无需修改               */
			}

			EEO_NEXT();
		}

		/*
		 * 若任一子句为 TRUE，则无论其余子句状态如何，OR   
		 * 的结果都是 TRUE，故可停止求值并立即返回 TRUE。
		 * 若无 TRUE 但有一或多个 NULL，则返回 NULL；否则    
		 * 返回 FALSE。把 NULL 解读为"不知道"时这是合理的： 
		 * 也许某个"不知道"若已知其值本会是 TRUE。只有当  
		 * 所有输入都确知为 FALSE 时，我们才能确信            
		 * OR 的结果确实是                                             
		 *  FALSE。            
		 */
		EEO_CASE(EEOP_BOOL_OR_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_OR_STEP_FIRST resets anynull, otherwise it's the same
			 * as EEOP_BOOL_OR_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_OR_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* 结果已设为 TRUE，无需修改               */
				/* 提前退出   */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_OR_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* 结果已设为 NULL，无需修改               */
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* 结果已设为 TRUE，无需修改               */

				/*
				 * 不必跳往 jumpdone——目标相同（AND 表达式
				 * 最后一个参数），只是                          
				 * 更昂贵。   
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* 结果已设为 FALSE，无需修改               */
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_NOT_STEP)
		{
			/*
			 * 'not' 求值很简单...若 expr 为假则返回 'true'，    
			 * 反之亦然。对名义上为 null 的值也安全，
			 * 因此忽略 resnull；这意味着 NULL 进 NULL 出，   
			 * 这正是我们想要的。                      
			 */
			*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_QUAL)
		{
			/* 供 ExecQual() 用的 BOOL_AND_STEP 简化版             */

			/* 若参数（兼结果）为 false 或 null...  */
			if (*op->resnull ||
				!DatumGetBool(*op->resvalue))
			{
				/* ... 提前退出并返回 FALSE     */
				*op->resnull = false;
				*op->resvalue = BoolGetDatum(false);
				EEO_JUMP(op->d.qualexpr.jumpdone);
			}

			/*
			 * 否则保留 TRUE 值不动，以防这是最后的 qual。  
			 * 此时 TRUE 就是正确答案。            
			 */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP)
		{
			/* 无条件跳转到目标步骤      */
			EEO_JUMP(op->d.jump.jumpdone);
		}

		EEO_CASE(EEOP_JUMP_IF_NULL)
		{
			/* 若当前结果为 null 则转移控制    */
			if (*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_NULL)
		{
			/* 若当前结果为非 null 则转移控制     */
			if (!*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_TRUE)
		{
			/* 若当前结果为 null 或 false 则转移控制   */
			if (*op->resnull || !DatumGetBool(*op->resvalue))
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNULL)
		{
			*op->resvalue = BoolGetDatum(*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNOTNULL)
		{
			*op->resvalue = BoolGetDatum(!*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNULL)
		{
			/* 转为外联实现：代码过大     */
			ExecEvalRowNull(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNOTNULL)
		{
			/* 转为外联实现：代码过大     */
			ExecEvalRowNotNull(state, op, econtext);

			EEO_NEXT();
		}

		/* 面向所有 booltesttype 的 BooleanTest 实现  */

		EEO_CASE(EEOP_BOOLTEST_IS_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			/* 否则，输入值同时就是正确输出      */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			/* 否则，输入值同时就是正确输出      */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXEC)
		{
			/* 转为外联实现：代码过大     */
			ExecEvalParamExec(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXTERN)
		{
			/* 转为外联实现：代码过大     */
			ExecEvalParamExtern(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_CALLBACK)
		{
			/* 允许扩展模块提供 PARAM_EXTERN 值                */
			op->d.cparam.paramfunc(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_CASE_TESTVAL)
		{
			/*
			 * 通常上层已设好返回值，但系统有些部分    
			 * 目前误用 {caseValue,domainValue}_{datum,isNull} 来 
			 * 设置运行时数据。若没有值被设置过，就用     
			 * ExprContext 的。                                   
			 * 这不雅观，但也没*那么*难看，而且这里       
			 * 不太可能对性能敏感到值得为一个额外       
			 * 分支而操心。          
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->caseValue_datum;
				*op->resnull = econtext->caseValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_TESTVAL)
		{
			/*
			 * 见 EEOP_CASE_TESTVAL 注释  
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->domainValue_datum;
				*op->resnull = econtext->domainValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MAKE_READONLY)
		{
			/*
			 * 强制把可能被多次读取的 varlena 值变为 R/O       
			 */
			if (!*op->d.make_readonly.isnull)
				*op->resvalue =
					MakeExpandedObjectReadOnlyInternal(*op->d.make_readonly.value);
			*op->resnull = *op->d.make_readonly.isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_IOCOERCE)
		{
			/*
			 * 求值 CoerceViaIO 节点。这可能是相当热的路径， 
			 * 因此尽可能多地内联工作。源值在我们的结果
			 * 变量中。    
			 */
			char	   *str;

			/* 调用输出函数（类似 OutputFunctionCall）    */
			if (*op->resnull)
			{
				/* 输出函数不会在 null 上被调用  */
				str = NULL;
			}
			else
			{
				FunctionCallInfo fcinfo_out;

				fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				fcinfo_out->args[0].value = *op->resvalue;
				fcinfo_out->args[0].isnull = false;

				fcinfo_out->isnull = false;
				str = DatumGetCString(FunctionCallInvoke(fcinfo_out));

				/* OutputFunctionCall 假定结果非 null      */
				Assert(!fcinfo_out->isnull);
			}

			/* 调用输入函数（类似 InputFunctionCall）   */
			if (!op->d.iocoerce.finfo_in->fn_strict || str != NULL)
			{
				FunctionCallInfo fcinfo_in;
				Datum		d;

				fcinfo_in = op->d.iocoerce.fcinfo_data_in;
				fcinfo_in->args[0].value = PointerGetDatum(str);
				fcinfo_in->args[0].isnull = *op->resnull;
				/* 第二和第三个参数已设置好          */

				fcinfo_in->isnull = false;
				d = FunctionCallInvoke(fcinfo_in);
				*op->resvalue = d;

				/* 当且仅当 str 为 NULL 时得 null 结果      */
				if (str == NULL)
				{
					Assert(*op->resnull);
					Assert(fcinfo_in->isnull);
				}
				else
				{
					Assert(!*op->resnull);
					Assert(!fcinfo_in->isnull);
				}
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DISTINCT)
		{
			/*
			 * IS DISTINCT FROM 必须求值参数（已求值到         
			 * fcinfo->args）以判断它们是否为 NULL；若任一为   
			 * NULL 则结果已定。若两者均非 NULL，则继续求值
			 * 比较函数，即该类型标准的相等运算符。我们  
			 * 无需关心函数是否严格；由于 null 处理不同，
			 * 不能直接复用                                        
			 * EEOP_FUNCEXPR。                             
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			/* 检查函数参数是否为 NULL      */
			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				/* 两者都 NULL？那么不相异...*/
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				/* 只有一个 NULL？那么相异...   */
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
			{
				/* 两者皆非 null，故应用相等函数    */
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				/* 必须反转 "=" 的结果；即使 null 也安全 */
				*op->resvalue = BoolGetDatum(!DatumGetBool(eqresult));
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		/* 注释见 EEOP_DISTINCT，此处只是反转          */
		EEO_CASE(EEOP_NOT_DISTINCT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
			{
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				*op->resvalue = eqresult;
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLIF)
		{
			/*
			 * 参数已求值到 fcinfo->args 中。                
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			Datum		save_arg0 = fcinfo->args[0].value;

			/* 若任一参数为 NULL，两者不可能相等*/
			if (!fcinfo->args[0].isnull && !fcinfo->args[1].isnull)
			{
				Datum		result;

				/*
				 * 若首参为 varlena 类型，则可能是 expanded 
				 * datum。需确保传给比较函数的值是只读指针。
				 * 但若最终返回首参，                              
				 * 则返回的将是原始读写指针，                    
				 * 若其原本为读写。                         
				 */
				if (op->d.func.make_ro)
					fcinfo->args[0].value =
						MakeExpandedObjectReadOnlyInternal(save_arg0);

				fcinfo->isnull = false;
				result = op->d.func.fn_addr(fcinfo);

				/* 若参数相等则返回 NULL          */
				if (!fcinfo->isnull && DatumGetBool(result))
				{
					*op->resvalue = (Datum) 0;
					*op->resnull = true;

					EEO_NEXT();
				}
			}

			/* 参数不相等，因此返回第一个参数   */
			*op->resvalue = save_arg0;
			*op->resnull = fcinfo->args[0].isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SQLVALUEFUNCTION)
		{
			/*
			 * 从效率角度看，似乎不值得专门              
			 * 实现内联。 
			 */
			ExecEvalSQLValueFunction(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CURRENTOFEXPR)
		{
			/* 错误调用会占用空间，且不应该发生      */
			ExecEvalCurrentOfExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEXTVALUEEXPR)
		{
			/*
			 * 从效率角度看，似乎不值得专门              
			 * 实现内联。 
			 */
			ExecEvalNextValueExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYEXPR)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalArrayExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYCOERCE)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalArrayCoerce(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROW)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalRow(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_STEP)
		{
			FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
			Datum		d;

			/* 严格函数遇 NULL 输入则强制 NULL     */
			if (op->d.rowcompare_step.finfo->fn_strict &&
				(fcinfo->args[0].isnull || fcinfo->args[1].isnull))
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}

			/* 应用比较函数        */
			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);
			*op->resvalue = d;

			/* 函数结果为 NULL 则强制结果 NULL */
			if (fcinfo->isnull)
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}
			*op->resnull = false;

			/* 若不相等，则无需比较其余列          */
			if (DatumGetInt32(*op->resvalue) != 0)
			{
				EEO_JUMP(op->d.rowcompare_step.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_FINAL)
		{
			int32		cmpresult = DatumGetInt32(*op->resvalue);
			RowCompareType rctype = op->d.rowcompare_final.rctype;

			*op->resnull = false;
			switch (rctype)
			{
					/* 此处不允许 EQ 和 NE 情形    */
				case ROWCOMPARE_LT:
					*op->resvalue = BoolGetDatum(cmpresult < 0);
					break;
				case ROWCOMPARE_LE:
					*op->resvalue = BoolGetDatum(cmpresult <= 0);
					break;
				case ROWCOMPARE_GE:
					*op->resvalue = BoolGetDatum(cmpresult >= 0);
					break;
				case ROWCOMPARE_GT:
					*op->resvalue = BoolGetDatum(cmpresult > 0);
					break;
				default:
					Assert(false);
					break;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MINMAX)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalMinMax(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSELECT)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalFieldSelect(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_DEFORM)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalFieldStoreDeForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_FORM)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalFieldStoreForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SBSREF_SUBSCRIPTS)
		{
			/* 预检查 SubscriptingRef 下标      */
			if (op->d.sbsref_subscript.subscriptfunc(state, op, econtext))
			{
				EEO_NEXT();
			}
			else
			{
				/* 下标为 NULL，将 SubscriptingRef 短路为 NULL      */
				EEO_JUMP(op->d.sbsref_subscript.jumpdone);
			}
		}

		EEO_CASE(EEOP_SBSREF_OLD)
			EEO_CASE(EEOP_SBSREF_ASSIGN)
			EEO_CASE(EEOP_SBSREF_FETCH)
		{
			/* 执行 SubscriptingRef 读取或赋值        */
			op->d.sbsref.subscriptfunc(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONVERT_ROWTYPE)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalConvertRowtype(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCALARARRAYOP)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalScalarArrayOp(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHED_SCALARARRAYOP)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalHashedScalarArrayOp(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_NOTNULL)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalConstraintNotNull(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_CHECK)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalConstraintCheck(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_XMLEXPR)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalXmlExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JSON_CONSTRUCTOR)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalJsonConstructor(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_IS_JSON)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalJsonIsPredicate(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGGREF)
		{
			/*
			 * 返回一个 Datum，其值即为给定表达式              
			 * 上下文中预计算的聚合值。  
			 */
			int			aggno = op->d.aggref.aggno;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[aggno];
			*op->resnull = econtext->ecxt_aggnulls[aggno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_GROUPING_FUNC)
		{
			/* 过于复杂/少见，不适合内联实现       */
			ExecEvalGroupingFunc(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_WINDOW_FUNC)
		{
			/*
			 * 与 Aggref 类似，从 econtext 返回预计算的值。      
			 */
			WindowFuncExprState *wfunc = op->d.window_func.wfstate;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[wfunc->wfuncno];
			*op->resnull = econtext->ecxt_aggnulls[wfunc->wfuncno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SUBPLAN)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalSubPlan(state, op, econtext);

			EEO_NEXT();
		}

		/* 求值严格聚合反序列化函数                 */
		EEO_CASE(EEOP_AGG_STRICT_DESERIALIZE)
		{
			/* 不要以 NULL 输入调用严格反序列化函数          */
			if (op->d.agg_deserialize.fcinfo_data->args[0].isnull)
				EEO_JUMP(op->d.agg_deserialize.jumpnull);

			/* fallthrough */
		}

		/* 求值聚合反序列化函数（非严格部分）              */
		EEO_CASE(EEOP_AGG_DESERIALIZE)
		{
			FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
			AggState   *aggstate = castNode(AggState, state->parent);
			MemoryContext oldContext;

			/*
			 * 我们会在每个输入元组专用的内存上下文中运  
			 * 行。  
			 */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->isnull = false;
			*op->resvalue = FunctionCallInvoke(fcinfo);
			*op->resnull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/*
		 * 检查严格聚合转换/合并函数的输入                   
		 * 不得为 NULL。 
		 */

		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_ARGS)
		{
			NullableDatum *args = op->d.agg_strict_input_check.args;
			int			nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_NULLS)
		{
			bool	   *nulls = op->d.agg_strict_input_check.nulls;
			int			nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (nulls[argno])
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		/*
		 * 检查指向各组状态的指针是否为 NULL  
		 */

		EEO_CASE(EEOP_AGG_PLAIN_PERGROUP_NULLCHECK)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerGroup pergroup_allaggs =
				aggstate->all_pergroups[op->d.agg_plain_pergroup_nullcheck.setoff];

			if (pergroup_allaggs == NULL)
				EEO_JUMP(op->d.agg_plain_pergroup_nullcheck.jumpnull);

			EEO_NEXT();
		}

		/*
		 * 不同类型的聚合转换函数各自被实现为不同类型  
		 * 的步骤，避免产生不必要的开销。按值传递  
		 * 还是按引用传递、是否需要从输入为组内首行初    
		 * 始化转换值、以及转换函数是否严格，上述条件
		 * 的每种有效组合都对应一种                             
		 * 专门的步骤类型。                    
		 *
		 * 还可进一步优化，把定长类型按引用传递  
		 * 单独拆分出来，但目前来看似乎并不值得。    
		 */

		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			if (pergroup->noTransValue)
			{
				/* 若 transValue 尚未初始化，则现在初始化    */
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
				/* 已从输入复制转换值，本轮完成     */
			}
			else if (likely(!pergroup->transValueIsNull))
			{
				/* 调用转换函数，除非因严格性而跳过           */
				ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);
			}

			EEO_NEXT();
		}

		/* 注释见上方 EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL    */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* 注释见上方 EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL    */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
								   op->d.agg_trans.aggcontext,
								   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* 注释见上方 EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL    */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			if (pergroup->noTransValue)
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
			else if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* 注释见上方 EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL    */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);
			EEO_NEXT();
		}

		/* 注释见上方 EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL    */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
								   op->d.agg_trans.aggcontext,
								   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
		{
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
			AggState   *aggstate = castNode(AggState, state->parent);

			if (ExecEvalPreOrderedDistinctSingle(aggstate, pertrans))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_MULTI)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;

			if (ExecEvalPreOrderedDistinctMulti(aggstate, pertrans))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		/* 处理单列有序聚合 Datum                */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_DATUM)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalAggOrderedTransDatum(state, op, econtext);

			EEO_NEXT();
		}

		/* 处理多列有序聚合元组               */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_TUPLE)
		{
			/* 过于复杂，不适合内联实现     */
			ExecEvalAggOrderedTransTuple(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_LAST)
		{
			/* 不可达。*/
			Assert(false);
			goto out;
		}
	}

out:
	*isnull = state->resnull;
	return state->resvalue;
}

/*
 * 表达式求值回调，会在执行表达式之前执行额外检查。  
 * 该函数声明为 extern，因此其他执行方式也能使           
 * 用 
 */
Datum
ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull)
{
	/*
	 * 首轮执行时检查属性是否仍与 Var 匹配；由于           
	 * 模式或已变，未必成立。 
	 */
	CheckExprStillValid(state, econtext);

	/* 后续执行时跳过该检查           */
	state->evalfunc = (ExprStateEvalFunc) state->evalfunc_private;

	/* 然后实际执行   */
	return state->evalfunc(state, econtext, isNull);
}

/*
 * 检查表达式在计划创建之后可能发生模式变更           
 * 的情况下是否仍然有效。       
 */
void
CheckExprStillValid(ExprState *state, ExprContext *econtext)
{
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;

	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

	for (int i = 0; i < state->steps_len; i++)
	{
		ExprEvalStep *op = &state->steps[i];

		switch (ExecEvalStepOp(state, op))
		{
			case EEOP_INNER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(innerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_OUTER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(outerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_SCAN_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(scanslot, attnum + 1, op->d.var.vartype);
					break;
				}
			default:
				break;
		}
	}
}

/*
 * 检查 slot 中的用户属性能否被 Var 表达式引用。     
 * 只要创建表达式树之后未发生模式变更，此检查       
 * 就应当成功。                         
 */
static void
CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype)
{
	/*
	 * 此处需要检查的可能性是：属性在计划树创建之  
	 * 后被删除或类型被改变。理想情况下计划会失效        
	 * 而不再被复用，但以防万一，我们保留了这些防   
	 * 御措施。幸运的是，只需在首轮执行时检查一次    
	 * 即可。                      
	 *
	 * 注：理想情况下还应同时检查 typmod 与 typid，     
	 * 但这目前似乎不切实际：许多情况下 tupdesc 是       
	 * 由 ExecTypeFromTL() 生成的，而它无法保证在                
	 * 所有情况下都能生成准确的 typmod，因为某些表         
	 * 达式节点类型并不携带 typmod。幸运的是，恰恰         
	 * 因此，不应有地方对值的 typmod 存在关键依赖。
	 *
	 * 系统属性无需检查，因为其类型永远不会改       
	 * 变。 
	 */
	if (attnum > 0)
	{
		TupleDesc	slot_tupdesc = slot->tts_tupleDescriptor;
		Form_pg_attribute attr;

		if (attnum > slot_tupdesc->natts)	/* 不应该发生     */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 attnum, slot_tupdesc->natts);

		attr = TupleDescAttr(slot_tupdesc, attnum - 1);

		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of type %s has been dropped",
							attnum, format_type_be(slot_tupdesc->tdtypeid))));

		if (vartype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d of type %s has wrong type",
							attnum, format_type_be(slot_tupdesc->tdtypeid)),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(vartype))));
	}
}

/*
 * 验证 slot 与 EEOP_*_FETCHSOME 操作是否兼容。               
 */
static void
CheckOpSlotCompatibility(ExprEvalStep *op, TupleTableSlot *slot)
{
#ifdef USE_ASSERT_CHECKING
	/* 无需检查任何内容 */
	if (!op->d.fetch.fixed)
		return;

	/*
	 * 这一点或许终应修正，但目前更简单的做法是允      
	 * 许缓冲区元组与堆元组互换使用。     
	 */
	if (slot->tts_ops == &TTSOpsBufferHeapTuple &&
		op->d.fetch.kind == &TTSOpsHeapTuple)
		return;
	if (slot->tts_ops == &TTSOpsHeapTuple &&
		op->d.fetch.kind == &TTSOpsBufferHeapTuple)
		return;

	/*
	 * 目前我们认为，用虚拟 slot 代替某种特定类型          
	 * 的 slot 是可以的，因为虚拟 slot 从不需要变形。     
	 */
	if (slot->tts_ops == &TTSOpsVirtual)
		return;

	Assert(op->d.fetch.kind == slot->tts_ops);
#endif
}

/*
 * get_cached_rowtype：查找 rowtype tupdesc 的工具函数     
 *
 * type_id, typmod：rowtype 的标识     
 * rowcache：缓存标识信息的空间   
 *		（rowcache->cacheptr 必须初始化为 NULL）
 * changed：若非 NULL，更新发生时置为 true         
 *
 * 返回的 TupleDesc 不保证处于 pinned 状态；调用者若   
 * 要在任何可能引发缓存失效的操作期间使用它，    
 * 例如对输入元组做 detoast。              
 * （TupleDesc 总是有引用计数，只需用 IncrTupleDescRefCount） 
 *
 * 注意：由于复合类型的内容可能变化，我们必须       
 * 随时准备在节点执行期间重做；不能只在表达式      
 * 初始化时调用。     
 */
static TupleDesc
get_cached_rowtype(Oid type_id, int32 typmod,
				   ExprEvalRowtypeCache *rowcache,
				   bool *changed)
{
	if (type_id != RECORDOID)
	{
		/*
		 * 这是命名的复合类型，所以使用常规 typcache。   
		 * 首轮执行或复合类型变更时进行查找。注意：       
		 * "tupdesc_id == 0" 似乎多余，但它能防止出现           
		 * 一种纯属理论上的可能：上一轮的 type_id 是        
		 * RECORDOID，导致 cacheptr 不是 TypeCacheEntry *。         
		 */
		TypeCacheEntry *typentry = (TypeCacheEntry *) rowcache->cacheptr;

		if (unlikely(typentry == NULL ||
					 rowcache->tupdesc_id == 0 ||
					 typentry->tupDesc_identifier != rowcache->tupdesc_id))
		{
			typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
			if (typentry->tupDesc == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("type %s is not composite",
								format_type_be(type_id))));
			rowcache->cacheptr = (void *) typentry;
			rowcache->tupdesc_id = typentry->tupDesc_identifier;
			if (changed)
				*changed = true;
		}
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * RECORD 类型一经注册，在后端存续期间就不会        
		 * 再变化。因此我们并不真正需要 typcache 条目，     
		 * 这反而正好，因为本来也没有这样的条目。不过  
		 * 调用者查询的类型可能已变。    
		 */
		TupleDesc	tupDesc = (TupleDesc) rowcache->cacheptr;

		if (unlikely(tupDesc == NULL ||
					 rowcache->tupdesc_id != 0 ||
					 type_id != tupDesc->tdtypeid ||
					 typmod != tupDesc->tdtypmod))
		{
			tupDesc = lookup_rowtype_tupdesc(type_id, typmod);
			/* 释放 lookup_rowtype_tupdesc 获取的 pin */
			ReleaseTupleDesc(tupDesc);
			rowcache->cacheptr = (void *) tupDesc;
			rowcache->tupdesc_id = 0;	/* 非 RECORD 时不是有效值    */
			if (changed)
				*changed = true;
		}
		return tupDesc;
	}
}


/*
 * 快速路径函数，用于极简单的表达式
 */

/* ExecJust(Inner|Outer|Scan)Var 的实现         */
static pg_attribute_always_inline Datum
ExecJustVarImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;

	CheckOpSlotCompatibility(&state->steps[0], slot);

	/*
	 * 由于使用了 slot_getattr()，我们无需显式实现            
	 * FETCHSOME 步骤，也无需断言 attnum 在有效范围              
	 * --- 一旦出问题会由 slot_getattr() 处理。
	 */
	return slot_getattr(slot, attnum, isnull);
}

/* 简单引用 inner Var        */
static Datum
ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* 简单引用 outer Var        */
static Datum
ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* 简单引用 scan Var        */
static Datum
ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* ExecJustAssign(Inner|Outer|Scan)Var 的实现         */
static pg_attribute_always_inline Datum
ExecJustAssignVarImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;

	CheckOpSlotCompatibility(&state->steps[0], inslot);

	/*
	 * 此处无需 CheckVarSlotCompatibility，这在编译                
	 * 时已处理过。  
	 *
	 * 由于使用了 slot_getattr()，我们无需显式实现            
	 * FETCHSOME 步骤，也无需断言 attnum 在有效范围              
	 * （slot_getattr() 会处理任何问题）。不过仍需检查       
	 * resultnum 是否越界。  
	 */
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);
	outslot->tts_values[resultnum] =
		slot_getattr(inslot, attnum, &outslot->tts_isnull[resultnum]);
	return 0;
}

/* 求值 inner Var 并赋给结果元组的相应列                  */
static Datum
ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* 求值 outer Var 并赋给结果元组的相应列                  */
static Datum
ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* 求值 scan Var 并赋给结果元组的相应列                  */
static Datum
ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* 求值 CASE_TESTVAL 并对其应用严格函数         */
static Datum
ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	FunctionCallInfo fcinfo;
	NullableDatum *args;
	int			nargs;
	Datum		d;

	/*
	 * XXX 若对 CaseTestExpr 机制稍作重新设计，或许           
	 * 就能免去数据搬运？    
	 */
	*op->resvalue = *op->d.casetest.value;
	*op->resnull = *op->d.casetest.isnull;

	op++;

	nargs = op->d.func.nargs;
	fcinfo = op->d.func.fcinfo_data;
	args = fcinfo->args;

	/* 严格函数，检查参数是否 NULL  */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			*isnull = true;
			return (Datum) 0;
		}
	}
	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*isnull = fcinfo->isnull;
	return d;
}

/* 简单 Const 表达式  */
static Datum
ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];

	*isnull = op->d.constval.isnull;
	return op->d.constval.value;
}

/* ExecJust(Inner|Outer|Scan)VarVirt 的实现         */
static pg_attribute_always_inline Datum
ExecJustVarVirtImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.var.attnum;

	/*
	 * 由于已保证使用虚拟 slot，因此永远不需要执行        
	 * 元组变形 （也不可能做到）。因此 execExpr.c       
	 * 并未生成 EEOP_*_FETCHSOME 步骤。请尽可能验                 
	 * 证该判断是准确的。                    
	 */
	Assert(TTS_IS_VIRTUAL(slot));
	Assert(TTS_FIXED(slot));
	Assert(attnum >= 0 && attnum < slot->tts_nvalid);

	*isnull = slot->tts_isnull[attnum];

	return slot->tts_values[attnum];
}

/* 类似 ExecJustInnerVar，为虚拟 slot 优化    */
static Datum
ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* 类似 ExecJustOuterVar，为虚拟 slot 优化    */
static Datum
ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* 类似 ExecJustScanVar，为虚拟 slot 优化    */
static Datum
ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

/* ExecJustAssign(Inner|Outer|Scan)VarVirt 的实现         */
static pg_attribute_always_inline Datum
ExecJustAssignVarVirtImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.assign_var.attnum;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;

	/* 注释见 ExecJustVarVirtImpl        */

	Assert(TTS_IS_VIRTUAL(inslot));
	Assert(TTS_FIXED(inslot));
	Assert(attnum >= 0 && attnum < inslot->tts_nvalid);
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);

	outslot->tts_values[resultnum] = inslot->tts_values[attnum];
	outslot->tts_isnull[resultnum] = inslot->tts_isnull[attnum];

	return 0;
}

/* 类似 ExecJustAssignInnerVar，为虚拟 slot 优化    */
static Datum
ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* 类似 ExecJustAssignOuterVar，为虚拟 slot 优化    */
static Datum
ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* 类似 ExecJustAssignScanVar，为虚拟 slot 优化    */
static Datum
ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

#if defined(EEO_USE_COMPUTED_GOTO)
/*
 * 在线程化分派情形下，为 ExecEvalStepOp() 构建     
 * address->opcode 查找表的比较器。       
 */
static int
dispatch_compare_ptr(const void *a, const void *b)
{
	const ExprEvalOpLookup *la = (const ExprEvalOpLookup *) a;
	const ExprEvalOpLookup *lb = (const ExprEvalOpLookup *) b;

	if (la->opcode < lb->opcode)
		return -1;
	else if (la->opcode > lb->opcode)
		return 1;
	return 0;
}
#endif

/*
 * 对解释执行机制做一次性初始化。          
 */
static void
ExecInitInterpreter(void)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	/* 设置对外可见的分派表指针                */
	if (dispatch_table == NULL)
	{
		dispatch_table = (const void **)
			DatumGetPointer(ExecInterpExpr(NULL, NULL, NULL));

		/* 构建反向查找表      */
		for (int i = 0; i < EEOP_LAST; i++)
		{
			reverse_dispatch_table[i].opcode = dispatch_table[i];
			reverse_dispatch_table[i].op = (ExprEvalOp) i;
		}

		/* 使 bsearch() 可用  */
		qsort(reverse_dispatch_table,
			  EEOP_LAST /* nmembers */ ,
			  sizeof(ExprEvalOpLookup),
			  dispatch_compare_ptr);
	}
#endif
}

/*
 * 返回表达式步骤 opcode 的函数。           
 *
 * 使用 direct-threading 时，ExprState->opcode 不易         
 * 解读。本函数会返回相应的枚举成员。             
 */
ExprEvalOp
ExecEvalStepOp(ExprState *state, ExprEvalStep *op)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	if (state->flags & EEO_FLAG_DIRECT_THREADED)
	{
		ExprEvalOpLookup key;
		ExprEvalOpLookup *res;

		key.opcode = (void *) op->opcode;
		res = bsearch(&key,
					  reverse_dispatch_table,
					  EEOP_LAST /* nmembers */ ,
					  sizeof(ExprEvalOpLookup),
					  dispatch_compare_ptr);
		Assert(res);			/* 未知 op 不应被查找           */
		return res->op;
	}
#endif
	return (ExprEvalOp) op->opcode;
}


/*
 * 复杂指令的 Out-of-line 辅助函数。           
 */

/*
 * 求值 EEOP_FUNCEXPR_FUSAGE  
 */
void
ExecEvalFuncExprFusage(ExprState *state, ExprEvalStep *op,
					   ExprContext *econtext)
{
	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	Datum		d;

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * 求值 EEOP_FUNCEXPR_STRICT_FUSAGE  
 */
void
ExecEvalFuncExprStrictFusage(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{

	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	NullableDatum *args = fcinfo->args;
	int			nargs = op->d.func.nargs;
	Datum		d;

	/* strict 函数，需检查 NULL 参数   */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			*op->resnull = true;
			return;
		}
	}

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * 求值 PARAM_EXEC 参数。     
 *
 * PARAM_EXEC 参数（执行器内部参数）存储在             
 * ecxt_param_exec_vals 数组，可用数组下标访问。      
 */
void
ExecEvalParamExec(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamExecData *prm;

	prm = &(econtext->ecxt_param_exec_vals[op->d.param.paramid]);
	if (unlikely(prm->execPlan != NULL))
	{
		/* 参数尚未求值，故现在求值     */
		ExecSetParamPlan(prm->execPlan, econtext);
		/* ExecSetParamPlan 本应已处理过该参数...      */
		Assert(prm->execPlan == NULL);
	}
	*op->resvalue = prm->value;
	*op->resnull = prm->isnull;
}

/*
 * 求值 PARAM_EXTERN 参数。     
 *
 * PARAM_EXTERN 参数必须在 ecxt_param_list_info 中查找。 
 */
void
ExecEvalParamExtern(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;
	int			paramId = op->d.param.paramid;

	if (likely(paramInfo &&
			   paramId > 0 && paramId <= paramInfo->numParams))
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		/* 参数可能为动态，让钩子函数先处理*/
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

		if (likely(OidIsValid(prm->ptype)))
		{
			/* 以防钩子做出意外行为的安全检查      */
			if (unlikely(prm->ptype != op->d.param.paramtype))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(op->d.param.paramtype))));
			*op->resvalue = prm->value;
			*op->resnull = prm->isnull;
			return;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
}

/*
 * 求值 SQLValueFunction 表达式。   
 */
void
ExecEvalSQLValueFunction(ExprState *state, ExprEvalStep *op)
{
	LOCAL_FCINFO(fcinfo, 0);
	SQLValueFunction *svf = op->d.sqlvaluefunction.svf;

	*op->resnull = false;

	/*
	 * 注意：current_schema() 可能返回 NULL。current_user() 等     
	 * 目前不能，但为安全起见不妨同样编码这些情况。 
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			*op->resvalue = DateADTGetDatum(GetSQLCurrentDate());
			break;
		case SVFOP_CURRENT_TIME:
		case SVFOP_CURRENT_TIME_N:
			*op->resvalue = TimeTzADTPGetDatum(GetSQLCurrentTime(svf->typmod));
			break;
		case SVFOP_CURRENT_TIMESTAMP:
		case SVFOP_CURRENT_TIMESTAMP_N:
			*op->resvalue = TimestampTzGetDatum(GetSQLCurrentTimestamp(svf->typmod));
			break;
		case SVFOP_LOCALTIME:
		case SVFOP_LOCALTIME_N:
			*op->resvalue = TimeADTGetDatum(GetSQLLocalTime(svf->typmod));
			break;
		case SVFOP_LOCALTIMESTAMP:
		case SVFOP_LOCALTIMESTAMP_N:
			*op->resvalue = TimestampGetDatum(GetSQLLocalTimestamp(svf->typmod));
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_user(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_SESSION_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = session_user(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_CATALOG:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_database(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_SCHEMA:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_schema(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
	}
}

/*
 * 若 CURRENT OF 表达式被求值则报错。       
 *
 * 规划器应当把 CURRENT OF 转换成 TidScan 限定条件，或在      
 * ForeignScan 节点中做其他特殊处理。因此我们必须能对     
 * CurrentOfExpr 执行 ExecInitExpr，但绝不应真正执行它。         
 * 若走到这里，我们推测必然是在处理某个外表上的        
 * CURRENT OF，其 FDW 不支持它，此时应相应报错。
 */
void
ExecEvalCurrentOfExpr(ExprState *state, ExprEvalStep *op)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("WHERE CURRENT OF is not supported for this table type")));
}

/*
 * 求值 NextValueExpr。
 */
void
ExecEvalNextValueExpr(ExprState *state, ExprEvalStep *op)
{
	int64		newval = nextval_internal(op->d.nextvalueexpr.seqid, false);

	switch (op->d.nextvalueexpr.seqtypid)
	{
		case INT2OID:
			*op->resvalue = Int16GetDatum((int16) newval);
			break;
		case INT4OID:
			*op->resvalue = Int32GetDatum((int32) newval);
			break;
		case INT8OID:
			*op->resvalue = Int64GetDatum((int64) newval);
			break;
		default:
			elog(ERROR, "unsupported sequence type %u",
				 op->d.nextvalueexpr.seqtypid);
	}
	*op->resnull = false;
}

/*
 * 对行求值 NullTest / IS NULL。   
 */
void
ExecEvalRowNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, true);
}

/*
 * 对行求值 NullTest / IS NOT NULL。   
 */
void
ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, false);
}

/* 对行值执行 IS [NOT] NULL 的公共代码*/
static void
ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
				   ExprContext *econtext, bool checkisnull)
{
	Datum		value = *op->resvalue;
	bool		isnull = *op->resnull;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	*op->resnull = false;

	/* NULL 行变量就像 NULL 标量列一样对待            */
	if (isnull)
	{
		*op->resvalue = BoolGetDatum(checkisnull);
		return;
	}

	/*
	 * SQL 标准为非空 rowtype 参数定义的 IS [NOT] NULL             
	 * 为
	 *
	 * "R IS NULL" 在每个字段均为空值时为真。  
	 *
	 * "R IS NOT NULL" 在没有字段为空值时为真。  
	 *
	 * 该定义（显然是有意地）并非递归的；因此我们对 
	 * 字段的测试只是原始的 attisnull 测试，而非递归检查   
	 * 看它们是否为全空或全非空的 rowtype。
	 *
	 * 标准没有考虑零字段的行这种情况，但我们             
	 * 在这里认为它们平凡地满足这两个谓词。     
	 */

	tuple = DatumGetHeapTupleHeader(value);

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);

	/* 若首次执行或类型变化，则查找 tupdesc      */
	tupDesc = get_cached_rowtype(tupType, tupTypmod,
								 &op->d.nulltest_row.rowcache, NULL);

	/*
	 * heap_attisnull 需要 HeapTuple 而非裸 HeapTupleHeader。
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	for (int att = 1; att <= tupDesc->natts; att++)
	{
		/* 忽略已删除的列  */
		if (TupleDescAttr(tupDesc, att - 1)->attisdropped)
			continue;
		if (heap_attisnull(&tmptup, att, tupDesc))
		{
			/* 空字段可否定 IS NOT NULL   */
			if (!checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
		else
		{
			/* 非空字段可否定 IS NULL    */
			if (checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
	}

	*op->resvalue = BoolGetDatum(true);
}

/*
 * 求值 ARRAY[] 表达式。    
 *
 * 各个数组元素（或子数组）已经被求值到                  
 * op->d.arrayexpr.elemvalues[]/elemnulls[]      
 */
void
ExecEvalArrayExpr(ExprState *state, ExprEvalStep *op)
{
	ArrayType  *result;
	Oid			element_type = op->d.arrayexpr.elemtype;
	int			nelems = op->d.arrayexpr.nelems;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	/* 将默认值设为非空*/
	*op->resnull = false;

	if (!op->d.arrayexpr.multidims)
	{
		/* 元素大概属于标量类型         */
		Datum	   *dvalues = op->d.arrayexpr.elemvalues;
		bool	   *dnulls = op->d.arrayexpr.elemnulls;

		/* 为给定长度的一维数组做设置 */
		ndims = 1;
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, dnulls, ndims, dims, lbs,
									element_type,
									op->d.arrayexpr.elemlength,
									op->d.arrayexpr.elembyval,
									op->d.arrayexpr.elemalign);
	}
	else
	{
		/* 必然是嵌套的数组表达式*/
		int			nbytes = 0;
		int			nitems;
		int			outer_nelems = 0;
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		bool		havenulls = false;
		bool		haveempty = false;
		char	  **subdata;
		bits8	  **subbitmaps;
		int		   *subbytes;
		int		   *subnitems;
		int32		dataoffset;
		char	   *dat;
		int			iitem;

		subdata = (char **) palloc(nelems * sizeof(char *));
		subbitmaps = (bits8 **) palloc(nelems * sizeof(bits8 *));
		subbytes = (int *) palloc(nelems * sizeof(int));
		subnitems = (int *) palloc(nelems * sizeof(int));

		/* 遍历并从每个元素获取数据区          */
		for (int elemoff = 0; elemoff < nelems; elemoff++)
		{
			Datum		arraydatum;
			bool		eisnull;
			ArrayType  *array;
			int			this_ndims;

			arraydatum = op->d.arrayexpr.elemvalues[elemoff];
			eisnull = op->d.arrayexpr.elemnulls[elemoff];

			/* 暂时忽略空值子数组       */
			if (eisnull)
			{
				haveempty = true;
				continue;
			}

			array = DatumGetArrayTypeP(arraydatum);

			/* 对元素类型做运行时复核     */
			if (element_type != ARR_ELEMTYPE(array))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot merge incompatible arrays"),
						 errdetail("Array with element type %s cannot be "
								   "included in ARRAY construct with element type %s.",
								   format_type_be(ARR_ELEMTYPE(array)),
								   format_type_be(element_type))));

			this_ndims = ARR_NDIM(array);
			/* 暂时忽略零维子数组                   */
			if (this_ndims <= 0)
			{
				haveempty = true;
				continue;
			}

			if (firstone)
			{
				/* 从第一个成员获取子数组详情 */
				elem_ndims = this_ndims;
				ndims = elem_ndims + 1;
				if (ndims <= 0 || ndims > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
									ndims, MAXDIM)));

				elem_dims = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_dims, ARR_DIMS(array), elem_ndims * sizeof(int));
				elem_lbs = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_lbs, ARR_LBOUND(array), elem_ndims * sizeof(int));

				firstone = false;
			}
			else
			{
				/* 检查其他子数组是否兼容     */
				if (elem_ndims != this_ndims ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
							 errmsg("multidimensional arrays must have array "
									"expressions with matching dimensions")));
			}

			subdata[outer_nelems] = ARR_DATA_PTR(array);
			subbitmaps[outer_nelems] = ARR_NULLBITMAP(array);
			subbytes[outer_nelems] = ARR_SIZE(array) - ARR_DATA_OFFSET(array);
			nbytes += subbytes[outer_nelems];
			/* 检查总请求是否溢出         */
			if (!AllocSizeIsValid(nbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
			subnitems[outer_nelems] = ArrayGetNItems(this_ndims,
													 ARR_DIMS(array));
			havenulls |= ARR_HASNULL(array);
			outer_nelems++;
		}

		/*
		 * 若所有项都是空值或空数组，则返回空数组；  
		 * 否则若有些是有些不是，则报错。（注意：我们  
		 * 必须以某种方式特判这一点，避免尝试生成由空 
		 * 数组构成的一维数组。这并不理想...）
		 */
		if (haveempty)
		{
			if (ndims == 0)		/* 未找到任何非空数组    */
			{
				*op->resvalue = PointerGetDatum(construct_empty_array(element_type));
				return;
			}
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("multidimensional arrays must have array "
							"expressions with matching dimensions")));
		}

		/* 为多维数组做设置*/
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (int i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		/* 检查下标是否溢出     */
		nitems = ArrayGetNItems(ndims, dims);
		ArrayCheckBounds(ndims, dims, lbs);

		if (havenulls)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;		/* 无空值位图的标记  */
			nbytes += ARR_OVERHEAD_NONULLS(ndims);
		}

		result = (ArrayType *) palloc0(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = ndims;
		result->dataoffset = dataoffset;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

		dat = ARR_DATA_PTR(result);
		iitem = 0;
		for (int i = 0; i < outer_nelems; i++)
		{
			memcpy(dat, subdata[i], subbytes[i]);
			dat += subbytes[i];
			if (havenulls)
				array_bitmap_copy(ARR_NULLBITMAP(result), iitem,
								  subbitmaps[i], 0,
								  subnitems[i]);
			iitem += subnitems[i];
		}
	}

	*op->resvalue = PointerGetDatum(result);
}

/*
 * 求值 ArrayCoerceExpr 表达式。    
 *
 * 源数组位于步骤的结果变量中。
 */
void
ExecEvalArrayCoerce(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Datum		arraydatum;

	/* NULL 数组则输出 NULL */
	if (*op->resnull)
		return;

	arraydatum = *op->resvalue;

	/*
	 * 若二进制兼容，则修改数组头中的元素类型，           
	 * 除此之外对数组不作任何改动。      
	 */
	if (op->d.arraycoerce.elemexprstate == NULL)
	{
		/* 必要时 detoast 输入数组，无论如何都复制 */
		ArrayType  *array = DatumGetArrayTypePCopy(arraydatum);

		ARR_ELEMTYPE(array) = op->d.arraycoerce.resultelemtype;
		*op->resvalue = PointerGetDatum(array);
		return;
	}

	/*
	 * 用 array_map 把子表达式应用到每个数组元素。     
	 */
	*op->resvalue = array_map(arraydatum,
							  op->d.arraycoerce.elemexprstate,
							  econtext,
							  op->d.arraycoerce.resultelemtype,
							  op->d.arraycoerce.amstate);
}

/*
 * 求值 ROW() 表达式。   
 *
 * 各个列已经被求值到                            
 * op->d.row.elemvalues[]/elemnulls[] 
 */
void
ExecEvalRow(ExprState *state, ExprEvalStep *op)
{
	HeapTuple	tuple;

	/* 用已求值的字段值构建元组    */
	tuple = heap_form_tuple(op->d.row.tupdesc,
							op->d.row.elemvalues,
							op->d.row.elemnulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * 求值 GREATEST()/LEAST() 表达式（注意并非 MIN()/MAX()）。       
 *
 * 所有待比较的表达式都已经被求值到                      
 * op->d.minmax.values[]/nulls[] 
 */
void
ExecEvalMinMax(ExprState *state, ExprEvalStep *op)
{
	Datum	   *values = op->d.minmax.values;
	bool	   *nulls = op->d.minmax.nulls;
	FunctionCallInfo fcinfo = op->d.minmax.fcinfo_data;
	MinMaxOp	operator = op->d.minmax.op;

	/* 在初始化时设置 */
	Assert(fcinfo->args[0].isnull == false);
	Assert(fcinfo->args[1].isnull == false);

	/* 默认结果为空值  */
	*op->resnull = true;

	for (int off = 0; off < op->d.minmax.nelems; off++)
	{
		/* 忽略 NULL 输入 */
		if (nulls[off])
			continue;

		if (*op->resnull)
		{
			/* 首个非空输入，采纳其值*/
			*op->resvalue = values[off];
			*op->resnull = false;
		}
		else
		{
			int			cmpresult;

			/* 应用比较函数        */
			fcinfo->args[0].value = *op->resvalue;
			fcinfo->args[1].value = values[off];

			fcinfo->isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(fcinfo));
			if (fcinfo->isnull) /* 大概率不应发生      */
				continue;

			if (cmpresult > 0 && operator == IS_LEAST)
				*op->resvalue = values[off];
			else if (cmpresult < 0 && operator == IS_GREATEST)
				*op->resvalue = values[off];
		}
	}
}

/*
 * 求值 FieldSelect 节点。
 *
 * 源记录位于步骤的结果变量中。 
 */
void
ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	AttrNumber	fieldnum = op->d.fieldselect.fieldnum;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	Form_pg_attribute attr;
	HeapTupleData tmptup;

	/* NULL 记录则输出 NULL  */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;

	/* 为提升速度可对展开记录做特判     */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(tupDatum)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(tupDatum);

		Assert(erh->er_magic == ER_MAGIC);

		/* 提取记录的 TupleDesc  */
		tupDesc = expanded_record_get_tupdesc(erh);

		/*
		 * 找到该字段的 attr 记录。注意我们这里不支持   
		 * 系统列：datum 元组对多数感兴趣的系统列       
		 * 反正也没有有效值。       
		 */
		if (fieldnum <= 0)		/* 不应发生        */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* 不应发生        */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* 检查已删除的列，若有则强制结果为 NULL   */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* 检查类型不匹配 --- ALTER COLUMN TYPE 后可能发生？*/
		/* CheckVarSlotCompatibility 中同样本应检查 typmod 却不能  */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* 提取该字段   */
		*op->resvalue = expanded_record_get_field(erh, fieldnum,
												  op->resnull);
	}
	else
	{
		/* 获取组合 datum 并提取其类型字段         */
		tuple = DatumGetHeapTupleHeader(tupDatum);

		tupType = HeapTupleHeaderGetTypeId(tuple);
		tupTypmod = HeapTupleHeaderGetTypMod(tuple);

		/* 若首次执行或类型变化，则查找 tupdesc      */
		tupDesc = get_cached_rowtype(tupType, tupTypmod,
									 &op->d.fieldselect.rowcache, NULL);

		/*
		 * 找到该字段的 attr 记录。注意我们这里不支持   
		 * 系统列：datum 元组对多数感兴趣的系统列       
		 * 反正也没有有效值。       
		 */
		if (fieldnum <= 0)		/* 不应发生        */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* 不应发生        */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* 检查已删除的列，若有则强制结果为 NULL   */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* 检查类型不匹配 --- ALTER COLUMN TYPE 后可能发生？*/
		/* CheckVarSlotCompatibility 中同样本应检查 typmod 却不能  */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* heap_getattr 需要 HeapTuple 而非裸 HeapTupleHeader   */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
		tmptup.t_data = tuple;

		/* 提取该字段   */
		*op->resvalue = heap_getattr(&tmptup,
									 fieldnum,
									 tupDesc,
									 op->resnull);
	}
}

/*
 * 先解构源元组，填充步骤的 values/nulls 数组，然后再  
 * 作为 FieldStore 表达式的一部分求值各个新值。        
 * 后续步骤会用新的字段值覆盖 values/nulls 数组的         
 * 个别元素，然后 FIELDSTORE_FORM 会构建                          
 * 新元组值。 
 *
 * 源记录位于步骤的结果变量中。 
 */
void
ExecEvalFieldStoreDeForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	if (*op->resnull)
	{
		/* 把空输入元组转换成全空的行        */
		memset(op->d.fieldstore.nulls, true,
			   op->d.fieldstore.ncolumns * sizeof(bool));
	}
	else
	{
		/*
		 * heap_deform_tuple 需要 HeapTuple 而非裸 HeapTupleHeader。   
		 * 以防万一，设置结构中全部字段。 
		 */
		Datum		tupDatum = *op->resvalue;
		HeapTupleHeader tuphdr;
		HeapTupleData tmptup;
		TupleDesc	tupDesc;

		tuphdr = DatumGetHeapTupleHeader(tupDatum);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = tuphdr;

		/*
		 * 若首次执行或类型变化则查找 tupdesc。由于我们   
		 * 并不固定 tupdesc，所以在 DatumGetHeapTupleHeader        
		 * 之前不能执行这次查找：解 toast 该 datum 时可能    
		 * 访问数据库。   
		 */
		tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
									 op->d.fieldstore.rowcache, NULL);

		/* 检查当前 tupdesc 的字段数是否超过已分配的数量    */
		if (unlikely(tupDesc->natts > op->d.fieldstore.ncolumns))
			elog(ERROR, "too many columns in composite type %u",
				 op->d.fieldstore.fstore->resulttype);

		heap_deform_tuple(&tmptup, tupDesc,
						  op->d.fieldstore.values,
						  op->d.fieldstore.nulls);
	}
}

/*
 * 在 FieldStore 表达式的各个单独字段值都求值后，       
 * 计算新的组合 datum。              
 */
void
ExecEvalFieldStoreForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	TupleDesc	tupDesc;
	HeapTuple	tuple;

	/* 查找 tupdesc（应当已经有效）   */
	tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
								 op->d.fieldstore.rowcache, NULL);

	tuple = heap_form_tuple(tupDesc,
							op->d.fieldstore.values,
							op->d.fieldstore.nulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * 求值 rowtype 强制转换操作。  
 * 这可能需要重新安排字段位置。   
 *
 * 源记录位于步骤的结果变量中。 
 */
void
ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	HeapTuple	result;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	HeapTupleData tmptup;
	TupleDesc	indesc,
				outdesc;
	bool		changed = false;

	/* NULL 则输出 NULL */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;
	tuple = DatumGetHeapTupleHeader(tupDatum);

	/*
	 * 若首次执行或类型变化则查找 tupdesc。我们最好        
	 * 固定它们，因为类型转换函数可能做系统表查找，   
	 * 进而导致缓存失效。    
	 */
	indesc = get_cached_rowtype(op->d.convert_rowtype.inputtype, -1,
								op->d.convert_rowtype.incache,
								&changed);
	IncrTupleDescRefCount(indesc);
	outdesc = get_cached_rowtype(op->d.convert_rowtype.outputtype, -1,
								 op->d.convert_rowtype.outcache,
								 &changed);
	IncrTupleDescRefCount(outdesc);

	/*
	 * 我们过去能够断言传入元组被标记为恰好是        
	 * indesc 的 rowtype。然而，既然 ExecEvalWholeRowVar             
	 * 可能因插入别名而把元组标记改为普通 RECORD，    
	 * 我们只能做这一弱测试：        
	 */
	Assert(HeapTupleHeaderGetTypeId(tuple) == indesc->tdtypeid ||
		   HeapTupleHeaderGetTypeId(tuple) == RECORDOID);

	/* 若首次执行或发生变化，则初始化转换映射         */
	if (changed)
	{
		MemoryContext old_cxt;

		/* 在长寿内存上下文中分配映射   */
		old_cxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		/* 准备从旧属性号到新属性号的映射 */
		op->d.convert_rowtype.map = convert_tuples_by_name(indesc, outdesc);

		MemoryContextSwitchTo(old_cxt);
	}

	/* 后续步骤需要 HeapTuple 而非裸 HeapTupleHeader      */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	if (op->d.convert_rowtype.map != NULL)
	{
		/* 需要带属性重排的完整转换                */
		result = execute_attr_map_tuple(&tmptup, op->d.convert_rowtype.map);
		/* 结果已具有合适的组合 datum 头部字段            */
		*op->resvalue = HeapTupleGetDatum(result);
	}
	else
	{
		/*
		 * 该元组本身在物理上是兼容的，但我们需要在它的 
		 * 组合 datum 头部字段中插入目标 rowtype OID，因此     
		 * 我们无论如何都得复制它。heap_copy_tuple_as_datum()   
		 * 对此很方便，因为它既做物理复制又插入正确的  
		 * 组合头部字段。注意我们并不预期要摊平任何      
		 * 被 toast 的字段：输入本是组合 datum，因此它   
		 * 不应含任何 toast 字段。所以 heap_copy_tuple_as_datum()  
		 * 是大材小用，但其对外部字段的检查很廉价。 
		 */
		*op->resvalue = heap_copy_tuple_as_datum(&tmptup, outdesc);
	}

	DecrTupleDescRefCount(indesc);
	DecrTupleDescRefCount(outdesc);
}

/*
 * 求值"标量 op ANY/ALL (array)"。 
 *
 * 源数组在我们的结果区中，标量参数已被求值到         
 * fcinfo->args[0] 
 *
 * 该操作符总是返回布尔值，我们用 OR 和 AND（分别        
 * 对应 ANY 和 ALL）组合所有数组元素上的结果。当然        
 * 一旦结果可知就立即短路。            
 */
void
ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op)
{
	FunctionCallInfo fcinfo = op->d.scalararrayop.fcinfo_data;
	bool		useOr = op->d.scalararrayop.useOr;
	bool		strictfunc = op->d.scalararrayop.finfo->fn_strict;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;

	/*
	 * 若数组为 NULL，则我们返回 NULL ---                         
	 * 即便操作符非严格，做其他事也无意义。 
	 */
	if (*op->resnull)
		return;

	/* 否则可取出并 detoast 该数组     */
	arr = DatumGetArrayTypeP(*op->resvalue);

	/*
	 * 若数组为空，则依据 useOr 标志返回 FALSE 或 TRUE。   
	 * 即使标量为 NULL，这也是正确的；因为我们会      
	 * 对操作符求值零次，故它是否想返回 NULL 都             
	 * 无关紧要。
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
	{
		*op->resvalue = BoolGetDatum(!useOr);
		*op->resnull = false;
		return;
	}

	/*
	 * 若标量为 NULL 且函数是 strict 的，则返回 NULL；     
	 * 没必要迭代循环。    
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		*op->resnull = true;
		return;
	}

	/*
	 * 我们安排每轮调用序列只查找一次元素类型信息，    
	 * 前提是元素类型不会在我们底下悄然改变。        
	 */
	if (op->d.scalararrayop.element_type != ARR_ELEMTYPE(arr))
	{
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &op->d.scalararrayop.typlen,
							 &op->d.scalararrayop.typbyval,
							 &op->d.scalararrayop.typalign);
		op->d.scalararrayop.element_type = ARR_ELEMTYPE(arr);
	}

	typlen = op->d.scalararrayop.typlen;
	typbyval = op->d.scalararrayop.typbyval;
	typalign = op->d.scalararrayop.typalign;

	/* 依据 useOr 适当地初始化结果              */
	result = BoolGetDatum(!useOr);
	resultnull = false;

	/* 遍历数组元素           */
	s = (char *) ARR_DATA_PTR(arr);
	bitmap = ARR_NULLBITMAP(arr);
	bitmask = 1;

	for (int i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* 取出数组元素，检查 NULL     */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;
		}
		else
		{
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *) att_align_nominal(s, typalign);
			fcinfo->args[1].value = elt;
			fcinfo->args[1].isnull = false;
		}

		/* 调用比较函数       */
		if (fcinfo->args[1].isnull && strictfunc)
		{
			fcinfo->isnull = true;
			thisresult = (Datum) 0;
		}
		else
		{
			fcinfo->isnull = false;
			thisresult = op->d.scalararrayop.fn_addr(fcinfo);
		}

		/* 按 OR 或 AND 语义合并结果       */
		if (fcinfo->isnull)
			resultnull = true;
		else if (useOr)
		{
			if (DatumGetBool(thisresult))
			{
				result = BoolGetDatum(true);
				resultnull = false;
				break;			/* 无须再查看更多元素       */
			}
		}
		else
		{
			if (!DatumGetBool(thisresult))
			{
				result = BoolGetDatum(false);
				resultnull = false;
				break;			/* 无须再查看更多元素       */
			}
		}

		/* 如有则前移位图指针   */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/*
 * 标量数组哈希操作的元素哈希函数   
 *
 * 我们使用元素类型的默认哈希 opclass，以及列排序规则 
 * 若类型对排序规则敏感。  
 */
static uint32
saop_element_hash(struct saophash_hash *tb, Datum key)
{
	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = &elements_tab->hash_fcinfo_data;
	Datum		hash;

	fcinfo->args[0].value = key;
	fcinfo->args[0].isnull = false;

	hash = elements_tab->hash_finfo.fn_addr(fcinfo);

	return DatumGetUInt32(hash);
}

/*
 * 标量数组哈希 op 元素的匹配函数，供哈希表查找时         
 * 所用  
 */
static bool
saop_hash_element_match(struct saophash_hash *tb, Datum key1, Datum key2)
{
	Datum		result;

	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = elements_tab->op->d.hashedscalararrayop.fcinfo_data;

	fcinfo->args[0].value = key1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = key2;
	fcinfo->args[1].isnull = false;

	result = elements_tab->op->d.hashedscalararrayop.finfo->fn_addr(fcinfo);

	return DatumGetBool(result);
}

/*
 * 求值 "scalar op ANY (const array)"。
 *
 * 与 ExecEvalScalarArrayOp 相似，但通过在首次查找时构建     
 * 哈希表来优化重复查找的速度。该哈希表将被               
 * 后续查找重用。与 ExecEvalScalarArrayOp 不同，此版本      
 * 仅支持 OR 语义。
 *
 * 源数组位于我们的结果区中，标量参数已求值到         
 * fcinfo->args[0].
 *
 * 操作符总是产生布尔值。  
 */
void
ExecEvalHashedScalarArrayOp(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ScalarArrayOpExprHashTable *elements_tab = op->d.hashedscalararrayop.elements_tab;
	FunctionCallInfo fcinfo = op->d.hashedscalararrayop.fcinfo_data;
	bool		inclause = op->d.hashedscalararrayop.inclause;
	bool		strictfunc = op->d.hashedscalararrayop.finfo->fn_strict;
	Datum		scalar = fcinfo->args[0].value;
	bool		scalar_isnull = fcinfo->args[0].isnull;
	Datum		result;
	bool		resultnull;
	bool		hashfound;

	/* 若数组常量为 null，则不建立哈希标量数组操作。   */
	Assert(!*op->resnull);

	/*
	 * 若标量为 NULL 且函数是 strict 的，则返回 NULL；     
	 * 没必要执行该搜索。   
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		*op->resnull = true;
		return;
	}

	/* 在首次求值时构建哈希表        */
	if (elements_tab == NULL)
	{
		ScalarArrayOpExpr *saop;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		int			nitems;
		bool		has_nulls = false;
		char	   *s;
		bits8	   *bitmap;
		int			bitmask;
		MemoryContext oldcontext;
		ArrayType  *arr;

		saop = op->d.hashedscalararrayop.saop;

		arr = DatumGetArrayTypeP(*op->resvalue);
		nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &typlen,
							 &typbyval,
							 &typalign);

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		elements_tab = (ScalarArrayOpExprHashTable *)
			palloc0(offsetof(ScalarArrayOpExprHashTable, hash_fcinfo_data) +
					SizeForFunctionCallInfo(1));
		op->d.hashedscalararrayop.elements_tab = elements_tab;
		elements_tab->op = op;

		fmgr_info(saop->hashfuncid, &elements_tab->hash_finfo);
		fmgr_info_set_expr((Node *) saop, &elements_tab->hash_finfo);

		InitFunctionCallInfoData(elements_tab->hash_fcinfo_data,
								 &elements_tab->hash_finfo,
								 1,
								 saop->inputcollid,
								 NULL,
								 NULL);

		/*
		 * 创建哈希表，其大小依据数组中元素的数目而定。 
		 * 这确实假定了数组中没有重复值。                    
		 * 若数组恰巧包含许多重复值，那只意味着          
		 * 我们把表建得稍大了一点而已。                
		 */
		elements_tab->hashtab = saophash_create(CurrentMemoryContext, nitems,
												elements_tab);

		MemoryContextSwitchTo(oldcontext);

		s = (char *) ARR_DATA_PTR(arr);
		bitmap = ARR_NULLBITMAP(arr);
		bitmask = 1;
		for (int i = 0; i < nitems; i++)
		{
			/* 取出数组元素，检查 NULL。   */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				has_nulls = true;
			}
			else
			{
				Datum		element;

				element = fetch_att(s, typbyval, typlen);
				s = att_addlength_pointer(s, typlen, s);
				s = (char *) att_align_nominal(s, typalign);

				saophash_insert(elements_tab->hashtab, element, &hashfound);
			}

			/* 如有则前移位图指针。 */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		/*
		 * 记住是否遇到过空值，从而得知在未找到匹配时   
		 * 是否需要以空的 lhs 值执行非 strict 函数。         
		 */
		op->d.hashedscalararrayop.has_nulls = has_nulls;
	}

	/* 在哈希表中查找是否有匹配。   */
	hashfound = NULL != saophash_lookup(elements_tab->hashtab, scalar);

	/* 结果取决于该子句是 IN 还是 NOT IN 子句           */
	if (inclause)
		result = BoolGetDatum(hashfound);	/* IN */
	else
		result = BoolGetDatum(!hashfound);	/* NOT IN */

	resultnull = false;

	/*
	 * 若未在数组中找到匹配，我们仍可能需要处理         
	 * 存在空值的可能性。我们没有把任何 NULL 放入     
	 * 哈希表，而是在建表时用 has_nulls 标记是否见过       
	 * 空值。    
	 */
	if (!hashfound && op->d.hashedscalararrayop.has_nulls)
	{
		if (strictfunc)
		{

			/*
			 * 数组中存在空值，故非空的 lhs 且无匹配必      
			 * 得 NULL。
			 */
			result = (Datum) 0;
			resultnull = true;
		}
		else
		{
			/*
			 * 以空的 rhs 只执行一次函数。   
			 *
			 * 哈希查找路径会涂改 lhs 参数，因此我们也需要 
			 * 把它设置好（即便进入本函数时                     
			 * 它已设置）。   
			 */
			fcinfo->args[0].value = scalar;
			fcinfo->args[0].isnull = scalar_isnull;
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;

			result = op->d.hashedscalararrayop.finfo->fn_addr(fcinfo);
			resultnull = fcinfo->isnull;

			/*
			 * 对 NOT IN 子句要反转结果，因为上述函数是      
			 * 相等函数，而我们需要的是不等。   
			 */
			if (!inclause)
				result = !result;
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/*
 * 求值 NOT NULL 域约束。          
 */
void
ExecEvalConstraintNotNull(ExprState *state, ExprEvalStep *op)
{
	if (*op->resnull)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("domain %s does not allow null values",
						format_type_be(op->d.domaincheck.resulttype)),
				 errdatatype(op->d.domaincheck.resulttype)));
}

/*
 * 求值 CHECK 域约束。          
 */
void
ExecEvalConstraintCheck(ExprState *state, ExprEvalStep *op)
{
	if (!*op->d.domaincheck.checknull &&
		!DatumGetBool(*op->d.domaincheck.checkvalue))
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("value for domain %s violates check constraint \"%s\"",
						format_type_be(op->d.domaincheck.resulttype),
						op->d.domaincheck.constraintname),
				 errdomainconstraint(op->d.domaincheck.resulttype,
									 op->d.domaincheck.constraintname)));
}

/*
 * 求值各种形式的 XmlExpr。      
 *
 * 参数已求值到 named_argvalue/named_argnull                
 * 和/或 argvalue/argnull 数组
 */
void
ExecEvalXmlExpr(ExprState *state, ExprEvalStep *op)
{
	XmlExpr    *xexpr = op->d.xmlexpr.xexpr;
	Datum		value;

	*op->resnull = true;		/* 直到得到结果    */
	*op->resvalue = (Datum) 0;

	switch (xexpr->op)
	{
		case IS_XMLCONCAT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				List	   *values = NIL;

				for (int i = 0; i < list_length(xexpr->args); i++)
				{
					if (!argnull[i])
						values = lappend(values, DatumGetPointer(argvalue[i]));
				}

				if (values != NIL)
				{
					*op->resvalue = PointerGetDatum(xmlconcat(values));
					*op->resnull = false;
				}
			}
			break;

		case IS_XMLFOREST:
			{
				Datum	   *argvalue = op->d.xmlexpr.named_argvalue;
				bool	   *argnull = op->d.xmlexpr.named_argnull;
				StringInfoData buf;
				ListCell   *lc;
				ListCell   *lc2;
				int			i;

				initStringInfo(&buf);

				i = 0;
				forboth(lc, xexpr->named_args, lc2, xexpr->arg_names)
				{
					Expr	   *e = (Expr *) lfirst(lc);
					char	   *argname = strVal(lfirst(lc2));

					if (!argnull[i])
					{
						value = argvalue[i];
						appendStringInfo(&buf, "<%s>%s</%s>",
										 argname,
										 map_sql_value_to_xml_value(value,
																	exprType((Node *) e), true),
										 argname);
						*op->resnull = false;
					}
					i++;
				}

				if (!*op->resnull)
				{
					text	   *result;

					result = cstring_to_text_with_len(buf.data, buf.len);
					*op->resvalue = PointerGetDatum(result);
				}

				pfree(buf.data);
			}
			break;

		case IS_XMLELEMENT:
			*op->resvalue = PointerGetDatum(xmlelement(xexpr,
													   op->d.xmlexpr.named_argvalue,
													   op->d.xmlexpr.named_argnull,
													   op->d.xmlexpr.argvalue,
													   op->d.xmlexpr.argnull));
			*op->resnull = false;
			break;

		case IS_XMLPARSE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				text	   *data;
				bool		preserve_whitespace;

				/* 参数已知为 text 与 bool        */
				Assert(list_length(xexpr->args) == 2);

				if (argnull[0])
					return;
				value = argvalue[0];
				data = DatumGetTextPP(value);

				if (argnull[1]) /* 多半不会发生    */
					return;
				value = argvalue[1];
				preserve_whitespace = DatumGetBool(value);

				*op->resvalue = PointerGetDatum(xmlparse(data,
														 xexpr->xmloption,
														 preserve_whitespace));
				*op->resnull = false;
			}
			break;

		case IS_XMLPI:
			{
				text	   *arg;
				bool		isnull;

				/* 可选参数已知为 text            */
				Assert(list_length(xexpr->args) <= 1);

				if (xexpr->args)
				{
					isnull = op->d.xmlexpr.argnull[0];
					if (isnull)
						arg = NULL;
					else
						arg = DatumGetTextPP(op->d.xmlexpr.argvalue[0]);
				}
				else
				{
					arg = NULL;
					isnull = false;
				}

				*op->resvalue = PointerGetDatum(xmlpi(xexpr->name,
													  arg,
													  isnull,
													  op->resnull));
			}
			break;

		case IS_XMLROOT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				xmltype    *data;
				text	   *version;
				int			standalone;

				/* 参数已知为 xml、text、int         */
				Assert(list_length(xexpr->args) == 3);

				if (argnull[0])
					return;
				data = DatumGetXmlP(argvalue[0]);

				if (argnull[1])
					version = NULL;
				else
					version = DatumGetTextPP(argvalue[1]);

				Assert(!argnull[2]);	/* 总是存在   */
				standalone = DatumGetInt32(argvalue[2]);

				*op->resvalue = PointerGetDatum(xmlroot(data,
														version,
														standalone));
				*op->resnull = false;
			}
			break;

		case IS_XMLSERIALIZE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* 参数类型已知为 xml        */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue =
					PointerGetDatum(xmltotext_with_options(DatumGetXmlP(value),
														   xexpr->xmloption,
														   xexpr->indent));
				*op->resnull = false;
			}
			break;

		case IS_DOCUMENT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* 可选参数已知为 xml            */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue =
					BoolGetDatum(xml_is_document(DatumGetXmlP(value)));
				*op->resnull = false;
			}
			break;

		default:
			elog(ERROR, "unrecognized XML operation");
			break;
	}
}

/*
 * 求值 JSON 构造器表达式。      
 */
void
ExecEvalJsonConstructor(ExprState *state, ExprEvalStep *op,
						ExprContext *econtext)
{
	Datum		res;
	JsonConstructorExprState *jcstate = op->d.json_constructor.jcstate;
	JsonConstructorExpr *ctor = jcstate->constructor;
	bool		is_jsonb = ctor->returning->format->format_type == JS_FORMAT_JSONB;
	bool		isnull = false;

	if (ctor->type == JSCTOR_JSON_ARRAY)
		res = (is_jsonb ?
			   jsonb_build_array_worker :
			   json_build_array_worker) (jcstate->nargs,
										 jcstate->arg_values,
										 jcstate->arg_nulls,
										 jcstate->arg_types,
										 jcstate->constructor->absent_on_null);
	else if (ctor->type == JSCTOR_JSON_OBJECT)
		res = (is_jsonb ?
			   jsonb_build_object_worker :
			   json_build_object_worker) (jcstate->nargs,
										  jcstate->arg_values,
										  jcstate->arg_nulls,
										  jcstate->arg_types,
										  jcstate->constructor->absent_on_null,
										  jcstate->constructor->unique);
	else
		elog(ERROR, "invalid JsonConstructorExpr type %d", ctor->type);

	*op->resvalue = res;
	*op->resnull = isnull;
}

/*
 * 求值 IS JSON 谓词。     
 */
void
ExecEvalJsonIsPredicate(ExprState *state, ExprEvalStep *op)
{
	JsonIsPredicate *pred = op->d.is_json.pred;
	Datum		js = *op->resvalue;
	Oid			exprtype;
	bool		res;

	if (*op->resnull)
	{
		*op->resvalue = BoolGetDatum(false);
		return;
	}

	exprtype = exprType(pred->expr);

	if (exprtype == TEXTOID || exprtype == JSONOID)
	{
		text	   *json = DatumGetTextP(js);

		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			switch (json_get_first_token(json, false))
			{
				case JSON_TOKEN_OBJECT_START:
					res = pred->item_type == JS_TYPE_OBJECT;
					break;
				case JSON_TOKEN_ARRAY_START:
					res = pred->item_type == JS_TYPE_ARRAY;
					break;
				case JSON_TOKEN_STRING:
				case JSON_TOKEN_NUMBER:
				case JSON_TOKEN_TRUE:
				case JSON_TOKEN_FALSE:
				case JSON_TOKEN_NULL:
					res = pred->item_type == JS_TYPE_SCALAR;
					break;
				default:
					res = false;
					break;
			}
		}

		/*
		 * 仅为唯一性检查或 JSON 文本校验才做完整解析   
		 * 遍历。  
		 */
		if (res && (pred->unique_keys || exprtype == TEXTOID))
			res = json_validate(json, pred->unique_keys, false);
	}
	else if (exprtype == JSONBOID)
	{
		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			Jsonb	   *jb = DatumGetJsonbP(js);

			switch (pred->item_type)
			{
				case JS_TYPE_OBJECT:
					res = JB_ROOT_IS_OBJECT(jb);
					break;
				case JS_TYPE_ARRAY:
					res = JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb);
					break;
				case JS_TYPE_SCALAR:
					res = JB_ROOT_IS_ARRAY(jb) && JB_ROOT_IS_SCALAR(jb);
					break;
				default:
					res = false;
					break;
			}
		}

		/* 对 jsonb 而言键唯一性检查多余    */
	}
	else
		res = false;

	*op->resvalue = BoolGetDatum(res);
}


/*
 * ExecEvalGroupingFunc
 *
 * 计算一个位掩码，每个（未求值的）参数表达式占一位
 * （最右参数是最低有效位）。  
 *
 * 若相应表达式不属于当前分组集的分组表达式          
 * 集合，则置位该位。                      
 */
void
ExecEvalGroupingFunc(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	int			result = 0;
	Bitmapset  *grouped_cols = aggstate->grouped_cols;
	ListCell   *lc;

	foreach(lc, op->d.grouping_func.clauses)
	{
		int			attnum = lfirst_int(lc);

		result <<= 1;

		if (!bms_is_member(attnum, grouped_cols))
			result |= 1;
	}

	*op->resvalue = Int32GetDatum(result);
	*op->resnull = false;
}

/*
 * 把子计划的求值转交给 nodeSubplan.c     
 */
void
ExecEvalSubPlan(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	SubPlanState *sstate = op->d.subplan.sstate;

	/* 可能存在嵌套，所以要确保栈空间充足            */
	check_stack_depth();

	*op->resvalue = ExecSubPlan(sstate, econtext, op->resnull);
}

/*
 * 求值 wholerow Var 表达式。   
 *
 * 返回一个 Datum，其值是给定表达式上下文下             
 * 整行范围变量的值。              
 */
void
ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Var		   *variable = op->d.wholerow.var;
	TupleTableSlot *slot;
	TupleDesc	output_tupdesc;
	MemoryContext oldcontext;
	HeapTupleHeader dtuple;
	HeapTuple	tuple;

	/* 这已由 ExecInitExpr 检查过 */
	Assert(variable->varattno == InvalidAttrNumber);

	/* 取所需的输入槽      */
	switch (variable->varno)
	{
		case INNER_VAR:
			/* 从内侧节点取元组          */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR:
			/* 从外侧节点取元组          */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR 由默认分支处理      */

		default:
			/* 从正在扫描的关系取元组             */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/* 如有 junkfilter 则应用 */
	if (op->d.wholerow.junkFilter != NULL)
		slot = ExecFilterJunk(op->d.wholerow.junkFilter, slot);

	/*
	 * 首次经过时，获取元组描述符并检查兼容性。           
	 *
	 * XXX: 若能挪到表达式初始化阶段就太好了，   
	 * 但因使用槽的缘故目前尚                                
	 * 不可行
	 */
	if (op->d.wholerow.first)
	{
		/* 乐观地假定不需要慢速路径          */
		op->d.wholerow.slow = false;

		/*
		 * 若 Var 标识的是具名组合类型，我们必须检查     
		 * 实际的元组类型与它兼容。        
		 */
		if (variable->vartype != RECORDOID)
		{
			TupleDesc	var_tupdesc;
			TupleDesc	slot_tupdesc;

			/*
			 * 我们真正在意的只是属性数目和数据类型。      
			 * 另外，对目标类型中已被删除的列，我们可以 
			 * 忽略其类型不匹配，只要 (1) 物理存储          
			 * 一致，或 (2) 实际列值为 NULL。情形 (1)          
			 * 有助于某些涉及过期缓存计划的情形，而         
			 * 情形 (2) 则是诸如向含被删列的表中 INSERT       
			 * 这类场景下的预期行为（规划器通常        
			 * 会无视被删列的类型而生成 INT4 NULL）。          
			 * 如果我们发现被删列且无法证实情形 (1)       
			 * 成立，就得对每一行用慢速路径检查 (2)。       
			 *
			 * 若 vartype 是组合上的域，只需直接看穿到       
			 * 底层组合类型。      
			 */
			var_tupdesc = lookup_rowtype_tupdesc_domain(variable->vartype,
														-1, false);

			slot_tupdesc = slot->tts_tupleDescriptor;

			if (var_tupdesc->natts != slot_tupdesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail_plural("Table row contains %d attribute, but query expects %d.",
										  "Table row contains %d attributes, but query expects %d.",
										  slot_tupdesc->natts,
										  slot_tupdesc->natts,
										  var_tupdesc->natts)));

			for (int i = 0; i < var_tupdesc->natts; i++)
			{
				Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
				Form_pg_attribute sattr = TupleDescAttr(slot_tupdesc, i);

				if (vattr->atttypid == sattr->atttypid)
					continue;	/* 无顾虑  */
				if (!vattr->attisdropped)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("table row type and query-specified row type do not match"),
							 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
									   format_type_be(sattr->atttypid),
									   i + 1,
									   format_type_be(vattr->atttypid))));

				if (vattr->attlen != sattr->attlen ||
					vattr->attalign != sattr->attalign)
					op->d.wholerow.slow = true; /* 需检查空值         */
			}

			/*
			 * 用该变量的声明行类型作为输出值的描述符。 
			 * 特别地，我们*必须*吸收任何             
			 * attisdropped 标记。
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(var_tupdesc);
			MemoryContextSwitchTo(oldcontext);

			ReleaseTupleDesc(var_tupdesc);
		}
		else
		{
			/*
			 * RECORD 情形下，我们用输入槽的行类型作为   
			 * 输出值的描述符，只是稍后可能重新指派         
			 * 新的列名。    
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
			MemoryContextSwitchTo(oldcontext);

			/*
			 * 输入槽有可能是一个关系扫描槽，因而被标以 
			 * 该关系的行类型。但我们应该返回的是           
			 * RECORD，所以重置为它。           
			 */
			output_tupdesc->tdtypeid = RECORDOID;
			output_tupdesc->tdtypmod = -1;

			/*
			 * 上面已获得正确的物理数据类型信息，但      
			 * 现在应尝试找到源 RTE 并采用其列别名，因为  
			 * 输入槽不太可能已带有想要的                 
			 * 名称。     
			 *
			 * 若找不到 RTE，则假定现有列名没问题。         
			 * （截至撰写本文时，找不到 RTE 的唯一情形   
			 * 是触发器 WHEN 子句的执行期间，此时 Var        
			 * 带有触发器关系的行类型，故名称没问题）      
			 * 另外，若 RTE 的创建者懒得填写 eref 字段，       
			 * 则假定我们的列名没问题。（这                     
			 * 发生于 COPY 或其他地方。）       
			 */
			if (econtext->ecxt_estate &&
				variable->varno <= econtext->ecxt_estate->es_range_table_size)
			{
				RangeTblEntry *rte = exec_rt_fetch(variable->varno,
												   econtext->ecxt_estate);

				if (rte->eref)
					ExecTypeSetColNames(output_tupdesc, rte->eref->colnames);
			}
		}

		/* 必要时 bless tupdesc，并保存进执行状态              */
		op->d.wholerow.tupdesc = BlessTupleDesc(output_tupdesc);

		op->d.wholerow.first = false;
	}

	/*
	 * 确保槽的所有列都能在槽的 Datum/isnull 数组中   
	 * 被访问。        
	 */
	slot_getallattrs(slot);

	if (op->d.wholerow.slow)
	{
		/* 检查是否有任何被删除的属性非空       */
		TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
		TupleDesc	var_tupdesc = op->d.wholerow.tupdesc;

		Assert(var_tupdesc->natts == tupleDesc->natts);

		for (int i = 0; i < var_tupdesc->natts; i++)
		{
			Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
			Form_pg_attribute sattr = TupleDescAttr(tupleDesc, i);

			if (!vattr->attisdropped)
				continue;		/* 已检查过非删除列         */
			if (slot->tts_isnull[i])
				continue;		/* 空值总是允许  */
			if (vattr->attlen != sattr->attlen ||
				vattr->attalign != sattr->attalign)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
								   i + 1)));
		}
	}

	/*
	 * 构建组合 datum，确保所有 TOAST 字段均被 detoast。       
	 *
	 * （注意：此处绝不可改变槽的状态。）             
	 */
	tuple = toast_build_flattened_tuple(slot->tts_tupleDescriptor,
										slot->tts_values,
										slot->tts_isnull);
	dtuple = tuple->t_data;

	/*
	 * 用先前识别的组合类型信息标记该 datum。            
	 *
	 * （注意：本可把 op->d.wholerow.tupdesc 传给元组           
	 * 构建步骤省去标记；但略冒险，先不这么做。）
	 */
	HeapTupleHeaderSetTypeId(dtuple, op->d.wholerow.tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtuple, op->d.wholerow.tupdesc->tdtypmod);

	*op->resvalue = PointerGetDatum(dtuple);
	*op->resnull = false;
}

void
ExecEvalSysVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
			   TupleTableSlot *slot)
{
	Datum		d;

	/* slot_getsysattr 对坏的 attnum 已有足够防御         */
	d = slot_getsysattr(slot,
						op->d.var.attnum,
						op->resnull);
	*op->resvalue = d;
	/* 这本应不可达，但检查它的代价足够低          */
	if (unlikely(*op->resnull))
		elog(ERROR, "failed to fetch attribute from slot");
}

/*
 * 转换值尚未初始化。这是该组的第一个非 NULL                
 * 输入值。我们把它用作 transValue 的初始值。        
 */
void
ExecAggInitGroup(AggState *aggstate, AggStatePerTrans pertrans, AggStatePerGroup pergroup,
				 ExprContext *aggcontext)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;

	/*
	 * 若 datum 是按引用传递的，必须把它复制进               
	 * aggcontext。旧 transValue 为 NULL，故无须                       
	 * pfree 它。（已查聚合输入与 transtype 二进制兼容，    
	 * 故照抄即可。）     
	 */
	oldContext = MemoryContextSwitchTo(aggcontext->ecxt_per_tuple_memory);
	pergroup->transValue = datumCopy(fcinfo->args[1].value,
									 pertrans->transtypeByVal,
									 pertrans->transtypeLen);
	pergroup->transValueIsNull = false;
	pergroup->noTransValue = false;
	MemoryContextSwitchTo(oldContext);
}

/*
 * 确保新的转换值存入 aggcontext，而不是               
 * 每元组上下文。仅当我们知道 (a) 转换数据            
 * 类型为按引用传递，且 (b) newValue 不同于             
 * oldValue 时才应调用本函数。       
 *
 * NB: 这可能会改变当前内存上下文。 
 *
 * 我们会把传入的 newValue 复制进 aggcontext，                   
 * 除非该 datum 指向一个已是 aggcontext 子对象的                
 * R/W 展开对象，此时无须复制。随后删除 oldValue（若       
 *
 * 非空）。若传入的 datum 指向某个其他上下文的            
 * R/W 展开子对象，理想情况下只需把它改挂到                 
 * aggcontext 之下。遗憾的是这不易做到，且对                
 * 感知聚合的 transfn 也无济于事。我们期望，一个           
 * 处理展开对象并知晓聚合转换值内存管理约定的        
 * transfn 会 (1) 在首次调用时返回一个已处于正确              
 * 上下文的 R/W 展开对象，使我们在此无事可做，且          
 * (2) 在后续调用中修改并返回同一对象，于是控        
 * 制根本不会到达此处。然而，如果我们的 transfn      
 * 是通用型的，返回一个新的 R/W 展开对象（多半        
 * 在每元组上下文中），对那个结果改挂父上下文            
 * 会引发问题。我们会把该 R/W 对象传给 transfn            
 * 的下一次调用，届时它可以随意修改或删除该对           
 * 象；如果它删除了该对象，那么我们随后删除已           
 * 变旧的 transvalue 的尝试就会成为双重释放。避免  
 * 此问题的办法是强制所存的 transvalue 总为扁平           
 * 非展开对象，除非 transfn 明显做聚合感知内存管理。   
 * 低效，宜写更聪明的 transfn
 */
Datum
ExecAggCopyTransValue(AggState *aggstate, AggStatePerTrans pertrans,
					  Datum newValue, bool newValueIsNull,
					  Datum oldValue, bool oldValueIsNull)
{
	Assert(newValue != oldValue);

	if (!newValueIsNull)
	{
		MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
		if (DatumIsReadWriteExpandedObject(newValue,
										   false,
										   pertrans->transtypeLen) &&
			MemoryContextGetParent(DatumGetEOHP(newValue)->eoh_context) == CurrentMemoryContext)
			 /* 空操作  */ ;
		else
			newValue = datumCopy(newValue,
								 pertrans->transtypeByVal,
								 pertrans->transtypeLen);
	}
	else
	{
		/*
		 * 确保 AggStatePerGroup->transValue 最终为 0，          
		 * 以便调用者能放心比较 newValue/oldValue，而不必  
		 * 检查各自是否为空。     
		 */
		newValue = (Datum) 0;
	}

	if (!oldValueIsNull)
	{
		if (DatumIsReadWriteExpandedObject(oldValue,
										   false,
										   pertrans->transtypeLen))
			DeleteExpandedObject(oldValue);
		else
			pfree(DatumGetPointer(oldValue));
	}

	return newValue;
}

/*
 * ExecEvalPreOrderedDistinctSingle
 *		当聚合转换值 Datum 与上一输入 Datum 不同时          
 *		返回 true，输入 Datum 与上一输入 Datum 相同             
 *		时返回 false。               
 */
bool
ExecEvalPreOrderedDistinctSingle(AggState *aggstate, AggStatePerTrans pertrans)
{
	Datum		value = pertrans->transfn_fcinfo->args[1].value;
	bool		isnull = pertrans->transfn_fcinfo->args[1].isnull;

	if (!pertrans->haslast ||
		pertrans->lastisnull != isnull ||
		(!isnull && !DatumGetBool(FunctionCall2Coll(&pertrans->equalfnOne,
													pertrans->aggCollation,
													pertrans->lastdatum, value))))
	{
		if (pertrans->haslast && !pertrans->inputtypeByVal &&
			!pertrans->lastisnull)
			pfree(DatumGetPointer(pertrans->lastdatum));

		pertrans->haslast = true;
		if (!isnull)
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);

			pertrans->lastdatum = datumCopy(value, pertrans->inputtypeByVal,
											pertrans->inputtypeLen);

			MemoryContextSwitchTo(oldContext);
		}
		else
			pertrans->lastdatum = (Datum) 0;
		pertrans->lastisnull = isnull;
		return true;
	}

	return false;
}

/*
 * ExecEvalPreOrderedDistinctMulti
 *		当聚合输入与上一输入不同时返回 true，输入与     
 *		上一输入相同时返回 false，或者没有上一输入时      
 *		也返回 false。               
 */
bool
ExecEvalPreOrderedDistinctMulti(AggState *aggstate, AggStatePerTrans pertrans)
{
	ExprContext *tmpcontext = aggstate->tmpcontext;
	bool		isdistinct = false; /* 目前  */
	TupleTableSlot *save_outer;
	TupleTableSlot *save_inner;

	for (int i = 0; i < pertrans->numTransInputs; i++)
	{
		pertrans->sortslot->tts_values[i] = pertrans->transfn_fcinfo->args[i + 1].value;
		pertrans->sortslot->tts_isnull[i] = pertrans->transfn_fcinfo->args[i + 1].isnull;
	}

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);

	/* 在覆盖它们之前先保存先前的槽       */
	save_outer = tmpcontext->ecxt_outertuple;
	save_inner = tmpcontext->ecxt_innertuple;

	tmpcontext->ecxt_outertuple = pertrans->sortslot;
	tmpcontext->ecxt_innertuple = pertrans->uniqslot;

	if (!pertrans->haslast ||
		!ExecQual(pertrans->equalfnMulti, tmpcontext))
	{
		if (pertrans->haslast)
			ExecClearTuple(pertrans->uniqslot);

		pertrans->haslast = true;
		ExecCopySlot(pertrans->uniqslot, pertrans->sortslot);

		isdistinct = true;
	}

	/* 恢复原来的槽         */
	tmpcontext->ecxt_outertuple = save_outer;
	tmpcontext->ecxt_innertuple = save_inner;

	return isdistinct;
}

/*
 * 以 datum 参数调用有序转换函数。               
 */
void
ExecEvalAggOrderedTransDatum(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	tuplesort_putdatum(pertrans->sortstates[setno],
					   *op->resvalue, *op->resnull);
}

/*
 * 以 tuple 参数调用有序转换函数。               
 */
void
ExecEvalAggOrderedTransTuple(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);
	tuplesort_puttupleslot(pertrans->sortstates[setno], pertrans->sortslot);
}

/* 按值传递类型的转换函数调用实现                    */
static pg_attribute_always_inline void
ExecAggPlainTransByVal(AggState *aggstate, AggStatePerTrans pertrans,
					   AggStatePerGroup pergroup,
					   ExprContext *aggcontext, int setno)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	/* cf. select_current_set() */
	aggstate->curaggcontext = aggcontext;
	aggstate->current_set = setno;

	/* 为 AggGetAggref() 设置 aggstate->curpertrans */
	aggstate->curpertrans = pertrans;

	/* 在每元组上下文中调用转换函数      */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;		/* 以防 transfn 不设置它         */

	newVal = FunctionCallInvoke(fcinfo);

	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/* 按引用传递类型的转换函数调用实现                 */
static pg_attribute_always_inline void
ExecAggPlainTransByRef(AggState *aggstate, AggStatePerTrans pertrans,
					   AggStatePerGroup pergroup,
					   ExprContext *aggcontext, int setno)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	/* cf. select_current_set() */
	aggstate->curaggcontext = aggcontext;
	aggstate->current_set = setno;

	/* 为 AggGetAggref() 设置 aggstate->curpertrans */
	aggstate->curpertrans = pertrans;

	/* 在每元组上下文中调用转换函数      */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;		/* 以防 transfn 不设置它         */

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * 对按引用传递的类型，必须把新值复制进 aggcontext    
	 * 并释放先前的 transValue。但如果 transfn 返回的         
	 * 是其首输入，则无须做任何事。
	 *
	 * 可以不考虑 NULL 地比较 newVal 与                           
	 * pergroup->transValue，因为 ExecAggCopyTransValue                   
	 * 会在 NULL 时把 transValue 置 0。否则可能                   
	 * 出现意外地不改挂父上下文的情况：transValue      
	 * 数值上与 newValue 相同，尽管它是 NULL。这条            
	 * 路径相当热，不宜为此再设分支处理常见              
	 * 情形，即转换函数返回其（修改后的）输入参  
	 * 数。   
	 */
	if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans,
									   newVal, fcinfo->isnull,
									   pergroup->transValue,
									   pergroup->transValueIsNull);

	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}
