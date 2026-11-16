/*-------------------------------------------------------------------------
 *
 * arraysubs.c
 *	  数组下标支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arraysubs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execExpr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* 用于数组下标执行的 SubscriptingRefState.workspace */
typedef struct ArraySubWorkspace
{
	/* 表达式编译期间确定的值 */
	Oid			refelemtype;	/* 数组元素类型的 OID */
	int16		refattrlength;	/* 数组类型 typlen */
	int16		refelemlength;	/* 数组元素类型的 typlen */
	bool		refelembyval;	/* 元素类型是否按值传递？ */
	char		refelemalign;	/* 元素类型的 typalign */

	/*
	 * 下标值转换为整数。注意即便处理更少的下标，这些
	 * 数组长度也必须是 MAXDIM，因为 array_get/set_slice
	 * 可能会涂写额外条目。
	 */
	int			upperindex[MAXDIM];
	int			lowerindex[MAXDIM];
} ArraySubWorkspace;


/*
 * 完成对数组的 SubscriptingRef 表达式的解析分析。
 *
 * 变换各下标表达式，并将它们强转为整数，
 * 并确定 SubscriptingRef 节点的结果类型。
 */
static void
array_subscript_transform(SubscriptingRef *sbsref,
						  List *indirection,
						  ParseState *pstate,
						  bool isSlice,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;
	ListCell   *idx;

	/*
	 * 转换下标表达式，并把上界与下界拆
	 * 分成两个列表。
	 *
	 * 若有容器切片表达式，我们把任何非切片的间接
	 * 项转换为切片：把单个下标视为上界，并提供
	 * 下界 1。
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);
		Node	   *subexpr;

		if (isSlice)
		{
			if (ai->lidx)
			{
				subexpr = transformExpr(pstate, ai->lidx, pstate->p_expr_kind);
				/* 若尚非 int4，尝试强转 */
				subexpr = coerce_to_target_type(pstate,
												subexpr, exprType(subexpr),
												INT4OID, -1,
												COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST,
												-1);
				if (subexpr == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("array subscript must have type integer"),
							 parser_errposition(pstate, exprLocation(ai->lidx))));
			}
			else if (!ai->is_slice)
			{
				/* 生成常量 1 */
				subexpr = (Node *) makeConst(INT4OID,
											 -1,
											 InvalidOid,
											 sizeof(int32),
											 Int32GetDatum(1),
											 false,
											 true); /* 按值传递 */
			}
			else
			{
				/* 省略下界的切片，把 NULL 放入列表 */
				subexpr = NULL;
			}
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		else
			Assert(ai->lidx == NULL && !ai->is_slice);

		if (ai->uidx)
		{
			subexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			/* 若尚非 int4，尝试强转 */
			subexpr = coerce_to_target_type(pstate,
											subexpr, exprType(subexpr),
											INT4OID, -1,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
			if (subexpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("array subscript must have type integer"),
						 parser_errposition(pstate, exprLocation(ai->uidx))));
		}
		else
		{
			/* 省略上界的切片，把 NULL 放入列表 */
			Assert(isSlice && ai->is_slice);
			subexpr = NULL;
		}
		upperIndexpr = lappend(upperIndexpr, subexpr);
	}

	/* ……并把转换后的列表存入 SubscriptRef 节点 */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = lowerIndexpr;

	/* 校验下标列表长度在实现限制之内 */
	if (list_length(upperIndexpr) > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						list_length(upperIndexpr), MAXDIM)));
	/* 无需单独检查 lowerIndexpr */

	/*
	 * 确定下标操作的结果类型。若为切片，结果类型与
	 * 数组类型相同，否则为元素类型。两种情况下
	 * typmod 都与数组相同，因此我们无需改变
	 * reftypmod。
	 */
	if (isSlice)
		sbsref->refrestype = sbsref->refcontainertype;
	else
		sbsref->refrestype = sbsref->refelemtype;
}

/*
 * 执行期间，处理 SubscriptingRef 表达式中的各下标。
 *
 * 各下标表达式已在 SubscriptingRefState 的数组中
 * 以 Datum 形式完成求值。按需检查并转换。
 *
 * 若任一下标为 NULL，则在赋值情形下抛错，或在
 * 获取情形下把结果置为 NULL 并返回 false（指示调用方
 * 跳过该序列的其余部分）。
 *
 * 我们把全部下标转换为普通整数，并保存进
 * sbsrefstate->workspace 数组
 */
static bool
array_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* 处理上界下标 */
	for (int i = 0; i < sbsrefstate->numupper; i++)
	{
		if (sbsrefstate->upperprovided[i])
		{
			/* 任一下标表达式为 NULL 则结果 NULL 或报错 */
			if (sbsrefstate->upperindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("array subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}
			workspace->upperindex[i] = DatumGetInt32(sbsrefstate->upperindex[i]);
		}
	}

	/* 下界下标同理 */
	for (int i = 0; i < sbsrefstate->numlower; i++)
	{
		if (sbsrefstate->lowerprovided[i])
		{
			/* 任一下标表达式为 NULL 则结果 NULL 或报错 */
			if (sbsrefstate->lowerindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("array subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}
			workspace->lowerindex[i] = DatumGetInt32(sbsrefstate->lowerindex[i]);
		}
	}

	return true;
}

/*
 * 为数组元素求值 SubscriptingRef 获取。
 *
 * 源容器位于 step 的结果变量中（已知非 NULL，因为
 * fetch_strict 为 true），且下标已求值到 workspace
 * 数组中。
 */
static void
array_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* 源数组或任一下标为 null 时不应走到这里 */
	Assert(!(*op->resnull));

	*op->resvalue = array_get_element(*op->resvalue,
									  sbsrefstate->numupper,
									  workspace->upperindex,
									  workspace->refattrlength,
									  workspace->refelemlength,
									  workspace->refelembyval,
									  workspace->refelemalign,
									  op->resnull);
}

/*
 * 为数组切片求值 SubscriptingRef 获取。
 *
 * 源容器位于 step 的结果变量中（已知非 NULL，因为
 * fetch_strict 为 true），且下标已求值到 workspace
 * 数组中。
 */
static void
array_subscript_fetch_slice(ExprState *state,
							ExprEvalStep *op,
							ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* 源数组或任一下标为 null 时不应走到这里 */
	Assert(!(*op->resnull));

	*op->resvalue = array_get_slice(*op->resvalue,
									sbsrefstate->numupper,
									workspace->upperindex,
									workspace->lowerindex,
									sbsrefstate->upperprovided,
									sbsrefstate->lowerprovided,
									workspace->refattrlength,
									workspace->refelemlength,
									workspace->refelembyval,
									workspace->refelemalign);
	/* 切片从不为 NULL，故无需改动 *op->resnull */
}

/*
 * 对数组元素赋值求取 SubscriptingRef 赋值。
 *
 * 输入容器（可能为 null）位于结果区，替换值位于
 * SubscriptingRefState replacevalue/replacenull。
 */
static void
array_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;
	Datum		arraySource = *op->resvalue;

	/*
	 * 对定长数组类型赋值时，原数组与待赋入的值都必
	 * 须非 NULL，否则我们放弃并
	 * 返回原数组。
	 */
	if (workspace->refattrlength > 0)
	{
		if (*op->resnull || sbsrefstate->replacenull)
			return;
	}

	/*
	 * 对 varlena 数组赋值时，我们这样处理 NULL 原
	 * 数组：以空（零维）数组替代之；插入新元素后将
	 * 得到单元素数组值。新元素是否为 NULL 并
	 * 不重要。
	 */
	if (*op->resnull)
	{
		arraySource = PointerGetDatum(construct_empty_array(workspace->refelemtype));
		*op->resnull = false;
	}

	*op->resvalue = array_set_element(arraySource,
									  sbsrefstate->numupper,
									  workspace->upperindex,
									  sbsrefstate->replacevalue,
									  sbsrefstate->replacenull,
									  workspace->refattrlength,
									  workspace->refelemlength,
									  workspace->refelembyval,
									  workspace->refelemalign);
	/* 结果永不为 NULL，因此无需改动 *op->resnull */
}

/*
 * 对数组切片赋值求取 SubscriptingRef 赋值。
 *
 * 输入容器（可能为 null）位于结果区，替换值位于
 * SubscriptingRefState replacevalue/replacenull。
 */
static void
array_subscript_assign_slice(ExprState *state,
							 ExprEvalStep *op,
							 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;
	Datum		arraySource = *op->resvalue;

	/*
	 * 对定长数组类型赋值时，原数组与待赋入的值都必
	 * 须非 NULL，否则我们放弃并
	 * 返回原数组。
	 */
	if (workspace->refattrlength > 0)
	{
		if (*op->resnull || sbsrefstate->replacenull)
			return;
	}

	/*
	 * 对 varlena 数组赋值时，我们这样处理 NULL 原
	 * 数组：以空（零维）数组替代之；插入新元素后将
	 * 得到单元素数组值。新元素是否为 NULL 并
	 * 不重要。
	 */
	if (*op->resnull)
	{
		arraySource = PointerGetDatum(construct_empty_array(workspace->refelemtype));
		*op->resnull = false;
	}

	*op->resvalue = array_set_slice(arraySource,
									sbsrefstate->numupper,
									workspace->upperindex,
									workspace->lowerindex,
									sbsrefstate->upperprovided,
									sbsrefstate->lowerprovided,
									sbsrefstate->replacevalue,
									sbsrefstate->replacenull,
									workspace->refattrlength,
									workspace->refelemlength,
									workspace->refelembyval,
									workspace->refelemalign);
	/* 结果永不为 NULL，因此无需改动 *op->resnull */
}

/*
 * 为 SubscriptingRef 赋值表达式计算旧的数组
 * 元素值。仅当新值子表达式包含
 * SubscriptingRef 或 FieldStore 时才会被调用。这与
 * 常规获取情形相同，但必须处理 null 数组，
 * 且值要存入 SubscriptingRefState 的
 * prevvalue/prevnull 字段
 */
static void
array_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	if (*op->resnull)
	{
		/* 数组为空，故任一元素亦为空 */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
		sbsrefstate->prevvalue = array_get_element(*op->resvalue,
												   sbsrefstate->numupper,
												   workspace->upperindex,
												   workspace->refattrlength,
												   workspace->refelemlength,
												   workspace->refelembyval,
												   workspace->refelemalign,
												   &sbsrefstate->prevnull);
}

/*
 * 为 SubscriptingRef 赋值表达式计算旧的数组
 * 切片值。仅当新值子表达式包含
 * SubscriptingRef 或 FieldStore 时才会被调用。这与
 * 常规获取情形相同，但必须处理 null 数组，
 * 且值要存入 SubscriptingRefState 的
 * prevvalue/prevnull 字段
 *
 * 注意：当前是死代码，因为切片的新值必须是
 * 数组，所以它不能直接包含 FieldStore；也不能包含
 * SubscriptingRef 赋值，因为我们把相邻的下标视为
 * 对一个多维数组进行索引，而非嵌套的数组类型，
 * 不过将来的泛化或许会使
 * 其重新可达。
 */
static void
array_subscript_fetch_old_slice(ExprState *state,
								ExprEvalStep *op,
								ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	if (*op->resnull)
	{
		/* 数组为空，故任一切片亦为空 */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		sbsrefstate->prevvalue = array_get_slice(*op->resvalue,
												 sbsrefstate->numupper,
												 workspace->upperindex,
												 workspace->lowerindex,
												 sbsrefstate->upperprovided,
												 sbsrefstate->lowerprovided,
												 workspace->refattrlength,
												 workspace->refelemlength,
												 workspace->refelembyval,
												 workspace->refelemalign);
		/* 非空数组的切片永不为空 */
		sbsrefstate->prevnull = false;
	}
}

/*
 * 为数组下标操作建立执行状态。
 */
static void
array_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	bool		is_slice = (sbsrefstate->numlower != 0);
	ArraySubWorkspace *workspace;

	/*
	 * 强制实施数组下标数量的实现限制。该检查与解析
	 * 期间的检查并非完全冗余；表达式可能是由具有不
	 * 同 MAXDIM 值的后端存储的。
	 */
	if (sbsrefstate->numupper > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						sbsrefstate->numupper, MAXDIM)));

	/* 若解析器正常则不应发生，但仍作检查： */
	if (sbsrefstate->numlower != 0 &&
		sbsrefstate->numupper != sbsrefstate->numlower)
		elog(ERROR, "upper and lower index lists are not same length");

	/*
	 * 分配类型专属工作区。
	 */
	workspace = (ArraySubWorkspace *) palloc(sizeof(ArraySubWorkspace));
	sbsrefstate->workspace = workspace;

	/*
	 * 收集执行时所需的数据类型细节。
	 */
	workspace->refelemtype = sbsref->refelemtype;
	workspace->refattrlength = get_typlen(sbsref->refcontainertype);
	get_typlenbyvalalign(sbsref->refelemtype,
						 &workspace->refelemlength,
						 &workspace->refelembyval,
						 &workspace->refelemalign);

	/*
	 * 回传指向相应步骤执行函数的指针。
	 */
	methods->sbs_check_subscripts = array_subscript_check_subscripts;
	if (is_slice)
	{
		methods->sbs_fetch = array_subscript_fetch_slice;
		methods->sbs_assign = array_subscript_assign_slice;
		methods->sbs_fetch_old = array_subscript_fetch_old_slice;
	}
	else
	{
		methods->sbs_fetch = array_subscript_fetch;
		methods->sbs_assign = array_subscript_assign;
		methods->sbs_fetch_old = array_subscript_fetch_old;
	}
}

/*
 * array_subscript_handler
 *		标准 varlena 数组的下标处理器。
 *
 * 本函数应仅用于"真"数组类型，即拥有 varlena 数组
 * 例程所能理解的数组头部、且由元素类型的
 * pg_type.typarray 字段引用的类型
 */
Datum
array_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = array_subscript_transform,
		.exec_setup = array_exec_setup,
		.fetch_strict = true,	/* 输入为 NULL 时获取 NULL */
		.fetch_leakproof = true,	/* 下标不当时获取返回 NULL */
		.store_leakproof = false	/* 但赋值则抛出错误 */
	};

	PG_RETURN_POINTER(&sbsroutines);
}

/*
 * raw_array_subscript_handler
 *		"raw" 数组的下标处理器。
 *
 * "raw" 数组只是包含元素类型的 N 个独立实例。
 * 当前我们要求元素类型与数组类型都是定长的，
 * 但放宽对数组类型的要求并不太难。
 *
 * 目前，全部支持代码都与标准 varlena 数组共用。
 * 我们或许会把它们拆成独立的代码路径，但这多半
 * 只能带来边际的性能提升。设置独立处理器的主要
 * 目的在于让 pg_type.typsubscript 明确表明类型的语义。
 */
Datum
raw_array_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = array_subscript_transform,
		.exec_setup = array_exec_setup,
		.fetch_strict = true,	/* 输入为 NULL 时获取 NULL */
		.fetch_leakproof = true,	/* 下标不当时获取返回 NULL */
		.store_leakproof = false	/* 但赋值则抛出错误 */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
