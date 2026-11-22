/*-------------------------------------------------------------------------
 *
 * execExpr.h
 *	  Low level infrastructure related to expression evaluation
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execExpr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXEC_EXPR_H
#define EXEC_EXPR_H

#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"

/*  前向引用以避免循环  */
struct ExprEvalStep;
struct SubscriptingRefState;
struct ScalarArrayOpExprHashTable;
struct JsonConstructorExprState;

/*  ExprState->flags 中的位(公共标志亦见 execnodes.h):  */
/*  表达式解释器已初始化  */
#define EEO_FLAG_INTERPRETER_INITIALIZED	(1 << 1)
/*  使用跳转穿线  */
#define EEO_FLAG_DIRECT_THREADED			(1 << 2)

/*  线外求值子程序的典型 API  */
typedef void (*ExecEvalSubroutine) (ExprState *state,
									struct ExprEvalStep *op,
									ExprContext *econtext);

/*  返回 bool 的线外求值子程序 API  */
typedef bool (*ExecEvalBoolSubroutine) (ExprState *state,
										struct ExprEvalStep *op,
										ExprContext *econtext);

/*  缓存复合类型 tupdesc 的步骤需其一  */
/*  (可内联于某些步骤,否则线外分配)  */
typedef struct ExprEvalRowtypeCache
{
	/*
	 * cacheptr points to composite type's TypeCacheEntry if tupdesc_id is not
	 * 0; or for an anonymous RECORD type, it points directly at the cached
	 * tupdesc for the type, and tupdesc_id is 0.  (We'd use separate fields
	 * if space were not at a premium.)  Initial state is cacheptr == NULL.
	 */
	void	   *cacheptr;
	uint64		tupdesc_id;		/*  上次 tupdesc 标识,或 0  */
} ExprEvalRowtypeCache;

/*
 * Discriminator for ExprEvalSteps.
 *
 * Identifies the operation to be executed and which member in the
 * ExprEvalStep->d union is valid.
 *
 * The order of entries needs to be kept in sync with the dispatch_table[]
 * array in execExprInterp.c:ExecInterpExpr().
 */
typedef enum ExprEvalOp
{
	/*  整个表达式已求值完毕,返回  */
	EEOP_DONE,

	/*  对相应元组槽应用 slot_getsomeattrs  */
	EEOP_INNER_FETCHSOME,
	EEOP_OUTER_FETCHSOME,
	EEOP_SCAN_FETCHSOME,

	/*  计算非系统 Var 值  */
	EEOP_INNER_VAR,
	EEOP_OUTER_VAR,
	EEOP_SCAN_VAR,

	/*  计算系统 Var 值  */
	EEOP_INNER_SYSVAR,
	EEOP_OUTER_SYSVAR,
	EEOP_SCAN_SYSVAR,

	/*  计算整行 Var  */
	EEOP_WHOLEROW,

	/*
	 * Compute non-system Var value, assign it into ExprState's resultslot.
	 * These are not used if a CheckVarSlotCompatibility() check would be
	 * needed.
	 */
	EEOP_ASSIGN_INNER_VAR,
	EEOP_ASSIGN_OUTER_VAR,
	EEOP_ASSIGN_SCAN_VAR,

	/*  把 resvalue/resnull 赋给 resultslot 一列  */
	EEOP_ASSIGN_TMP,
	/*  同上,并 MakeExpandedObjectReadOnly()  */
	EEOP_ASSIGN_TMP_MAKE_RO,

	/*  求值 Const 值  */
	EEOP_CONST,

	/*
	 * Evaluate function call (including OpExprs etc).  For speed, we
	 * distinguish in the opcode whether the function is strict and/or
	 * requires usage stats tracking.
	 */
	EEOP_FUNCEXPR,
	EEOP_FUNCEXPR_STRICT,
	EEOP_FUNCEXPR_FUSAGE,
	EEOP_FUNCEXPR_STRICT_FUSAGE,

	/*
	 * Evaluate boolean AND expression, one step per subexpression. FIRST/LAST
	 * subexpressions are special-cased for performance.  Since AND always has
	 * at least two subexpressions, FIRST and LAST never apply to the same
	 * subexpression.
	 */
	EEOP_BOOL_AND_STEP_FIRST,
	EEOP_BOOL_AND_STEP,
	EEOP_BOOL_AND_STEP_LAST,

	/*  布尔 OR 表达式同理  */
	EEOP_BOOL_OR_STEP_FIRST,
	EEOP_BOOL_OR_STEP,
	EEOP_BOOL_OR_STEP_LAST,

	/*  求值布尔 NOT 表达式  */
	EEOP_BOOL_NOT_STEP,

	/*  ExecQual() 用的 BOOL_AND_STEP 简化版  */
	EEOP_QUAL,

	/*  无条件跳到另一步  */
	EEOP_JUMP,

	/*  基于当前结果值的条件跳转  */
	EEOP_JUMP_IF_NULL,
	EEOP_JUMP_IF_NOT_NULL,
	EEOP_JUMP_IF_NOT_TRUE,

	/*  对标量值做 NULL 测试  */
	EEOP_NULLTEST_ISNULL,
	EEOP_NULLTEST_ISNOTNULL,

	/*  对行值做 NULL 测试  */
	EEOP_NULLTEST_ROWISNULL,
	EEOP_NULLTEST_ROWISNOTNULL,

	/*  求值 BooleanTest 表达式  */
	EEOP_BOOLTEST_IS_TRUE,
	EEOP_BOOLTEST_IS_NOT_TRUE,
	EEOP_BOOLTEST_IS_FALSE,
	EEOP_BOOLTEST_IS_NOT_FALSE,

	/*  求值 PARAM_EXEC/EXTERN 参数  */
	EEOP_PARAM_EXEC,
	EEOP_PARAM_EXTERN,
	EEOP_PARAM_CALLBACK,

	/*  返回 CaseTestExpr 值  */
	EEOP_CASE_TESTVAL,

	/*  对目标应用 MakeExpandedObjectReadOnly()  */
	EEOP_MAKE_READONLY,

	/*  求值各类专用表达式类型  */
	EEOP_IOCOERCE,
	EEOP_DISTINCT,
	EEOP_NOT_DISTINCT,
	EEOP_NULLIF,
	EEOP_SQLVALUEFUNCTION,
	EEOP_CURRENTOFEXPR,
	EEOP_NEXTVALUEEXPR,
	EEOP_ARRAYEXPR,
	EEOP_ARRAYCOERCE,
	EEOP_ROW,

	/*
	 * Compare two individual elements of each of two compared ROW()
	 * expressions.  Skip to ROWCOMPARE_FINAL if elements are not equal.
	 */
	EEOP_ROWCOMPARE_STEP,

	/*  基于先前 ROWCOMPARE_STEP 求布尔值  */
	EEOP_ROWCOMPARE_FINAL,

	/*  求值 GREATEST/LEAST  */
	EEOP_MINMAX,

	/*  求值 FieldSelect 表达式  */
	EEOP_FIELDSELECT,

	/*
	 * Deform tuple before evaluating new values for individual fields in a
	 * FieldStore expression.
	 */
	EEOP_FIELDSTORE_DEFORM,

	/*
	 * Form the new tuple for a FieldStore expression.  Individual fields will
	 * have been evaluated into columns of the tuple deformed by the preceding
	 * DEFORM step.
	 */
	EEOP_FIELDSTORE_FORM,

	/*  处理容器下标;可能短路为 NULL  */
	EEOP_SBSREF_SUBSCRIPTS,

	/*
	 * Compute old container element/slice when a SubscriptingRef assignment
	 * expression contains SubscriptingRef/FieldStore subexpressions. Value is
	 * accessed using the CaseTest mechanism.
	 */
	EEOP_SBSREF_OLD,

	/*  计算 SubscriptingRef 赋值的新值  */
	EEOP_SBSREF_ASSIGN,

	/*  计算 SubscriptingRef 取数的元素/切片  */
	EEOP_SBSREF_FETCH,

	/*  求值 CoerceToDomainValue 的值  */
	EEOP_DOMAIN_TESTVAL,

	/*  求值域的 NOT NULL 约束  */
	EEOP_DOMAIN_NOTNULL,

	/*  求值单个域 CHECK 约束  */
	EEOP_DOMAIN_CHECK,

	/*  求值各类专用表达式类型  */
	EEOP_CONVERT_ROWTYPE,
	EEOP_SCALARARRAYOP,
	EEOP_HASHED_SCALARARRAYOP,
	EEOP_XMLEXPR,
	EEOP_JSON_CONSTRUCTOR,
	EEOP_IS_JSON,
	EEOP_AGGREF,
	EEOP_GROUPING_FUNC,
	EEOP_WINDOW_FUNC,
	EEOP_SUBPLAN,

	/*  聚合相关节点  */
	EEOP_AGG_STRICT_DESERIALIZE,
	EEOP_AGG_DESERIALIZE,
	EEOP_AGG_STRICT_INPUT_CHECK_ARGS,
	EEOP_AGG_STRICT_INPUT_CHECK_NULLS,
	EEOP_AGG_PLAIN_PERGROUP_NULLCHECK,
	EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL,
	EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL,
	EEOP_AGG_PLAIN_TRANS_BYVAL,
	EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF,
	EEOP_AGG_PLAIN_TRANS_STRICT_BYREF,
	EEOP_AGG_PLAIN_TRANS_BYREF,
	EEOP_AGG_PRESORTED_DISTINCT_SINGLE,
	EEOP_AGG_PRESORTED_DISTINCT_MULTI,
	EEOP_AGG_ORDERED_TRANS_DATUM,
	EEOP_AGG_ORDERED_TRANS_TUPLE,

	/*  不存在的操作,如检查数组长度  */
	EEOP_LAST
} ExprEvalOp;


typedef struct ExprEvalStep
{
	/*
	 * Instruction to be executed.  During instruction preparation this is an
	 * enum ExprEvalOp, but later it can be changed to some other type, e.g. a
	 * pointer for computed goto (that's why it's an intptr_t).
	 */
	intptr_t	opcode;

	/*  存放本步结果的位置  */
	Datum	   *resvalue;
	bool	   *resnull;

	/*
	 * Inline data for the operation.  Inline data is faster to access, but
	 * also bloats the size of all instructions.  The union should be kept to
	 * no more than 40 bytes on 64-bit systems (so that the entire struct is
	 * no more than 64 bytes, a single cacheline on common systems).
	 */
	union
	{
		/* for EEOP_INNER/OUTER/SCAN_FETCHSOME */
		struct
		{
			/*  取到的属性号上界(含)  */
			int			last_var;
			/*  槽类型每次调用是否相同  */
			bool		fixed;
			/*  元组描述符,若已知 */
			TupleDesc	known_desc;
			/*  槽类型,仅 fixed 置位时可信  */
			const TupleTableSlotOps *kind;
		}			fetch;

		/* for EEOP_INNER/OUTER/SCAN_[SYS]VAR[_FIRST] */
		struct
		{
			/*  普通 VAR 的 attnum 为属性号减 1 ...  */
			/*  但对 SYSVAR 就是正常(负)属性号  */
			int			attnum;
			Oid			vartype;	/*  变量类型 OID  */
		}			var;

		/* for EEOP_WHOLEROW */
		struct
		{
			Var		   *var;	/*  计划树中的原始 Var  */
			bool		first;	/*  首次通过,需初始化?  */
			bool		slow;	/*  需运行时空值检查?  */
			TupleDesc	tupdesc;	/*  结果元组的描述符  */
			JunkFilter *junkFilter; /*  去 resjunk 列的 JunkFilter  */
		}			wholerow;

		/* for EEOP_ASSIGN_*_VAR */
		struct
		{
			/*  resultslot->tts_values/nulls 目标下标  */
			int			resultnum;
			/*  源属性号减 1  */
			int			attnum;
		}			assign_var;

		/* for EEOP_ASSIGN_TMP[_MAKE_RO] */
		struct
		{
			/*  resultslot->tts_values/nulls 目标下标  */
			int			resultnum;
		}			assign_tmp;

		/* for EEOP_CONST */
		struct
		{
			/*  常量的值  */
			Datum		value;
			bool		isnull;
		}			constval;

		/* for EEOP_FUNCEXPR_* / NULLIF / DISTINCT */
		struct
		{
			FmgrInfo   *finfo;	/*  函数查找数据  */
			FunctionCallInfo fcinfo_data;	/*  参数等  */
			/*  无额外间接访问更快:  */
			PGFunction	fn_addr;	/*  实际调用地址 */
			int			nargs;	/*  参数个数  */
			bool		make_ro;	/*  使 arg0 只读(仅 NULLIF)  */
		}			func;

		/* for EEOP_BOOL_*_STEP */
		struct
		{
			bool	   *anynull;	/* track if any input was NULL */
			int			jumpdone;	/*  结果已定时跳到此处  */
		}			boolexpr;

		/* for EEOP_QUAL */
		struct
		{
			int			jumpdone;	/*  假或空时跳到此处  */
		}			qualexpr;

		/* for EEOP_JUMP[_CONDITION] */
		struct
		{
			int			jumpdone;	/*  目标指令的下标  */
		}			jump;

		/* for EEOP_NULLTEST_ROWIS[NOT]NULL */
		struct
		{
			/*  复合类型缓存描述符 - 运行时填  */
			ExprEvalRowtypeCache rowcache;
		}			nulltest_row;

		/* for EEOP_PARAM_EXEC/EXTERN */
		struct
		{
			int			paramid;	/*  参数的数字 ID  */
			Oid			paramtype;	/*  参数数据类型 OID  */
		}			param;

		/* for EEOP_PARAM_CALLBACK */
		struct
		{
			ExecEvalSubroutine paramfunc;	/*  附加求值子程序  */
			void	   *paramarg;	/*  同上私有数据  */
			int			paramid;	/*  参数数字 ID  */
			Oid			paramtype;	/*  参数数据类型 OID  */
		}			cparam;

		/* for EEOP_CASE_TESTVAL/DOMAIN_TESTVAL */
		struct
		{
			Datum	   *value;	/*  返回值  */
			bool	   *isnull;
		}			casetest;

		/* for EEOP_MAKE_READONLY */
		struct
		{
			Datum	   *value;	/*  要强制为只读的值  */
			bool	   *isnull;
		}			make_readonly;

		/* for EEOP_IOCOERCE */
		struct
		{
			/*  源类型输出函数的查找与调用信息  */
			FmgrInfo   *finfo_out;
			FunctionCallInfo fcinfo_data_out;
			/*  结果类型输入函数的查找与调用信息  */
			FmgrInfo   *finfo_in;
			FunctionCallInfo fcinfo_data_in;
		}			iocoerce;

		/* for EEOP_SQLVALUEFUNCTION */
		struct
		{
			SQLValueFunction *svf;
		}			sqlvaluefunction;

		/* for EEOP_NEXTVALUEEXPR */
		struct
		{
			Oid			seqid;
			Oid			seqtypid;
		}			nextvalueexpr;

		/* for EEOP_ARRAYEXPR */
		struct
		{
			Datum	   *elemvalues; /*  元素值存于此  */
			bool	   *elemnulls;
			int			nelems; /*  上述数组的长度  */
			Oid			elemtype;	/* array element type */
			int16		elemlength; /*  数组元素类型的 typlen  */
			bool		elembyval;	/*  元素类型是否按值传递?  */
			char		elemalign;	/*  元素类型的 typalign  */
			bool		multidims;	/* is array expression multi-D? */
		}			arrayexpr;

		/* for EEOP_ARRAYCOERCE */
		struct
		{
			ExprState  *elemexprstate;	/* null if no per-element work */
			Oid			resultelemtype; /*  结果数组的元素类型 */
			struct ArrayMapState *amstate;	/*  array_map 的工作区 */
		}			arraycoerce;

		/* for EEOP_ROW */
		struct
		{
			TupleDesc	tupdesc;	/*  结果元组的描述符  */
			/*  构成行的值的工作区:  */
			Datum	   *elemvalues;
			bool	   *elemnulls;
		}			row;

		/* for EEOP_ROWCOMPARE_STEP */
		struct
		{
			/*  列比较函数的查找与调用数据  */
			FmgrInfo   *finfo;
			FunctionCallInfo fcinfo_data;
			PGFunction	fn_addr;
			/*  比较得 NULL 时的跳转目标  */
			int			jumpnull;
			/*  比较得不等时的跳转目标  */
			int			jumpdone;
		}			rowcompare_step;

		/* for EEOP_ROWCOMPARE_FINAL */
		struct
		{
			RowCompareType rctype;
		}			rowcompare_final;

		/* for EEOP_MINMAX */
		struct
		{
			/*  参数值的工作区  */
			Datum	   *values;
			bool	   *nulls;
			int			nelems;
			/*  GREATEST 还是 LEAST?  */
			MinMaxOp	op;
			/*  比较函数的查找与调用数据  */
			FmgrInfo   *finfo;
			FunctionCallInfo fcinfo_data;
		}			minmax;

		/* for EEOP_FIELDSELECT */
		struct
		{
			AttrNumber	fieldnum;	/*  要提取的字段号  */
			Oid			resulttype; /* field's type */
			/*  复合类型缓存描述符 - 运行时填  */
			ExprEvalRowtypeCache rowcache;
		}			fieldselect;

		/* for EEOP_FIELDSTORE_DEFORM / FIELDSTORE_FORM */
		struct
		{
			/*  原始表达式节点  */
			FieldStore *fstore;

			/*  复合类型缓存描述符 - 运行时填  */
			/*  DEFORM 与 FORM 对共享同一缓存  */
			ExprEvalRowtypeCache *rowcache;

			/*  列值的工作区  */
			Datum	   *values;
			bool	   *nulls;
			int			ncolumns;
		}			fieldstore;

		/* for EEOP_SBSREF_SUBSCRIPTS */
		struct
		{
			ExecEvalBoolSubroutine subscriptfunc;	/*  求值子程序  */
			/*  太大无法内联  */
			struct SubscriptingRefState *state;
			int			jumpdone;	/*  空则跳此处  */
		}			sbsref_subscript;

		/* for EEOP_SBSREF_OLD / ASSIGN / FETCH */
		struct
		{
			ExecEvalSubroutine subscriptfunc;	/*  求值子程序  */
			/*  太大无法内联  */
			struct SubscriptingRefState *state;
		}			sbsref;

		/* for EEOP_DOMAIN_NOTNULL / DOMAIN_CHECK */
		struct
		{
			/*  约束名  */
			char	   *constraintname;
			/*  CHECK 约束结果存放处  */
			Datum	   *checkvalue;
			bool	   *checknull;
			/*  域类型的 OID  */
			Oid			resulttype;
		}			domaincheck;

		/* for EEOP_CONVERT_ROWTYPE */
		struct
		{
			Oid			inputtype;	/*  输入复合类型  */
			Oid			outputtype; /*  输出复合类型  */
			/*  这三个字段在运行时填充:  */
			ExprEvalRowtypeCache *incache;	/*  输入类型缓存  */
			ExprEvalRowtypeCache *outcache; /*  输出类型缓存  */
			TupleConversionMap *map;	/*  列映射  */
		}			convert_rowtype;

		/* for EEOP_SCALARARRAYOP */
		struct
		{
			/*  element_type/typlen/typbyval/typalign 运行时填  */
			Oid			element_type;	/*  未填则为 InvalidOid  */
			bool		useOr;	/*  OR 还是 AND 语义?  */
			int16		typlen; /*  数组元素类型存储信息 */
			bool		typbyval;
			char		typalign;
			FmgrInfo   *finfo;	/*  函数查找数据  */
			FunctionCallInfo fcinfo_data;	/*  参数等  */
			/*  无额外间接访问更快:  */
			PGFunction	fn_addr;	/*  实际调用地址 */
		}			scalararrayop;

		/* for EEOP_HASHED_SCALARARRAYOP */
		struct
		{
			bool		has_nulls;
			bool		inclause;	/*  IN 为真,NOT IN 为假  */
			struct ScalarArrayOpExprHashTable *elements_tab;
			FmgrInfo   *finfo;	/*  函数查找数据  */
			FunctionCallInfo fcinfo_data;	/*  参数等  */
			ScalarArrayOpExpr *saop;
		}			hashedscalararrayop;

		/* for EEOP_XMLEXPR */
		struct
		{
			XmlExpr    *xexpr;	/*  原始表达式节点  */
			/*  命名参数求值工作区,若有  */
			Datum	   *named_argvalue;
			bool	   *named_argnull;
			/*  未命名参数求值工作区,若有  */
			Datum	   *argvalue;
			bool	   *argnull;
		}			xmlexpr;

		/* for EEOP_JSON_CONSTRUCTOR */
		struct
		{
			struct JsonConstructorExprState *jcstate;
		}			json_constructor;

		/* for EEOP_AGGREF */
		struct
		{
			int			aggno;
		}			aggref;

		/* for EEOP_GROUPING_FUNC */
		struct
		{
			List	   *clauses;	/*  列号的整数列表  */
		}			grouping_func;

		/* for EEOP_WINDOW_FUNC */
		struct
		{
			/*  线外状态,由 nodeWindowAgg.c 修改  */
			WindowFuncExprState *wfstate;
		}			window_func;

		/* for EEOP_SUBPLAN */
		struct
		{
			/*  线外状态,由 nodeSubplan.c 创建  */
			SubPlanState *sstate;
		}			subplan;

		/* for EEOP_AGG_*DESERIALIZE */
		struct
		{
			FunctionCallInfo fcinfo_data;
			int			jumpnull;
		}			agg_deserialize;

		/* for EEOP_AGG_STRICT_INPUT_CHECK_NULLS / STRICT_INPUT_CHECK_ARGS */
		struct
		{
			/*
			 * For EEOP_AGG_STRICT_INPUT_CHECK_ARGS args contains pointers to
			 * the NullableDatums that need to be checked for NULLs.
			 *
			 * For EEOP_AGG_STRICT_INPUT_CHECK_NULLS nulls contains pointers
			 * to booleans that need to be checked for NULLs.
			 *
			 * Both cases currently need to exist because sometimes the
			 * to-be-checked nulls are in TupleTableSlot.isnull array, and
			 * sometimes in FunctionCallInfoBaseData.args[i].isnull.
			 */
			NullableDatum *args;
			bool	   *nulls;
			int			nargs;
			int			jumpnull;
		}			agg_strict_input_check;

		/* for EEOP_AGG_PLAIN_PERGROUP_NULLCHECK */
		struct
		{
			int			setoff;
			int			jumpnull;
		}			agg_plain_pergroup_nullcheck;

		/* for EEOP_AGG_PRESORTED_DISTINCT_{SINGLE,MULTI} */
		struct
		{
			AggStatePerTrans pertrans;
			ExprContext *aggcontext;
			int			jumpdistinct;
		}			agg_presorted_distinctcheck;

		/* for EEOP_AGG_PLAIN_TRANS_[INIT_][STRICT_]{BYVAL,BYREF} */
		/* for EEOP_AGG_ORDERED_TRANS_{DATUM,TUPLE} */
		struct
		{
			AggStatePerTrans pertrans;
			ExprContext *aggcontext;
			int			setno;
			int			transno;
			int			setoff;
		}			agg_trans;

		/* for EEOP_IS_JSON */
		struct
		{
			JsonIsPredicate *pred;	/*  原始表达式节点  */
		}			is_json;

	}			d;
} ExprEvalStep;

/*  强制遵守上文注释中的大小规则  */
StaticAssertDecl(sizeof(ExprEvalStep) <= 64,
				 "size of ExprEvalStep exceeds 64 bytes");


/*  容器操作的非内联数据  */
typedef struct SubscriptingRefState
{
	bool		isassignment;	/*  是赋值还是仅取数?  */

	/*  类型专用下标代码的工作区  */
	void	   *workspace;

	/*  numupper 与 upperprovided[] 编译时填  */
	/*  运行时下标在 upperindex[]/upperindexnull[]  */
	int			numupper;
	bool	   *upperprovided;	/*  指示此位置是否已提供  */
	Datum	   *upperindex;
	bool	   *upperindexnull;

	/*  下界下标同理,若有  */
	int			numlower;
	bool	   *lowerprovided;
	Datum	   *lowerindex;
	bool	   *lowerindexnull;

	/*  赋值时新值求值到此处  */
	Datum		replacevalue;
	bool		replacenull;

	/*  嵌套赋值时 sbs_fetch_old 放旧值于此  */
	Datum		prevvalue;
	bool		prevnull;
} SubscriptingRefState;

/*  SubscriptingRef 使用的执行步方法  */
typedef struct SubscriptExecSteps
{
	/*  更多细节见 nodes/subscripting.h  */
	ExecEvalBoolSubroutine sbs_check_subscripts;	/*  处理下标  */
	ExecEvalSubroutine sbs_fetch;	/*  取一元素  */
	ExecEvalSubroutine sbs_assign;	/*  赋给一元素  */
	ExecEvalSubroutine sbs_fetch_old;	/*  为赋值取旧值  */
} SubscriptExecSteps;

/*  JSON_CONSTRUCTOR 状态,太大无法内联  */
typedef struct JsonConstructorExprState
{
	JsonConstructorExpr *constructor;
	Datum	   *arg_values;
	bool	   *arg_nulls;
	Oid		   *arg_types;
	struct
	{
		int			category;
		Oid			outfuncid;
	}		   *arg_type_cache; /*  datum_to_json[b]() 缓存  */
	int			nargs;
} JsonConstructorExprState;


/*  execExpr.c 中函数  */
extern void ExprEvalPushStep(ExprState *es, const ExprEvalStep *s);

/*  execExprInterp.c 中函数  */
extern void ExecReadyInterpretedExpr(ExprState *state);
extern ExprEvalOp ExecEvalStepOp(ExprState *state, ExprEvalStep *op);

extern Datum ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull);
extern void CheckExprStillValid(ExprState *state, ExprContext *econtext);

/*
 * Non fast-path execution functions. These are externs instead of statics in
 * execExprInterp.c, because that allows them to be used by other methods of
 * expression evaluation, reducing code duplication.
 */
extern void ExecEvalFuncExprFusage(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalFuncExprStrictFusage(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);
extern void ExecEvalParamExec(ExprState *state, ExprEvalStep *op,
							  ExprContext *econtext);
extern void ExecEvalParamExtern(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalSQLValueFunction(ExprState *state, ExprEvalStep *op);
extern void ExecEvalCurrentOfExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalNextValueExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalRowNull(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext);
extern void ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op,
							   ExprContext *econtext);
extern void ExecEvalArrayExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalArrayCoerce(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalRow(ExprState *state, ExprEvalStep *op);
extern void ExecEvalMinMax(ExprState *state, ExprEvalStep *op);
extern void ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalFieldStoreDeForm(ExprState *state, ExprEvalStep *op,
									 ExprContext *econtext);
extern void ExecEvalFieldStoreForm(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op);
extern void ExecEvalHashedScalarArrayOp(ExprState *state, ExprEvalStep *op,
										ExprContext *econtext);
extern void ExecEvalConstraintNotNull(ExprState *state, ExprEvalStep *op);
extern void ExecEvalConstraintCheck(ExprState *state, ExprEvalStep *op);
extern void ExecEvalXmlExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalJsonConstructor(ExprState *state, ExprEvalStep *op,
									ExprContext *econtext);
extern void ExecEvalJsonIsPredicate(ExprState *state, ExprEvalStep *op);
extern void ExecEvalGroupingFunc(ExprState *state, ExprEvalStep *op);
extern void ExecEvalSubPlan(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext);
extern void ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalSysVar(ExprState *state, ExprEvalStep *op,
						   ExprContext *econtext, TupleTableSlot *slot);

extern void ExecAggInitGroup(AggState *aggstate, AggStatePerTrans pertrans, AggStatePerGroup pergroup,
							 ExprContext *aggcontext);
extern Datum ExecAggCopyTransValue(AggState *aggstate, AggStatePerTrans pertrans,
								   Datum newValue, bool newValueIsNull,
								   Datum oldValue, bool oldValueIsNull);
extern bool ExecEvalPreOrderedDistinctSingle(AggState *aggstate,
											 AggStatePerTrans pertrans);
extern bool ExecEvalPreOrderedDistinctMulti(AggState *aggstate,
											AggStatePerTrans pertrans);
extern void ExecEvalAggOrderedTransDatum(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);
extern void ExecEvalAggOrderedTransTuple(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);

#endif							/* EXEC_EXPR_H */
