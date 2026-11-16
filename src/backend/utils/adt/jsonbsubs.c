/*-------------------------------------------------------------------------
 *
 * jsonbsubs.c
 *	  jsonb 的下标支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonbsubs.c
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
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* SubscriptingRefState.workspace，用于 jsonb 下标执行 */
typedef struct JsonbSubWorkspace
{
	bool		expectArray;	/* jsonb 根应为数组 */
	Oid		   *indexOid;		/* 转换后下标的 OID，只能
								 * 为整数或文本 */
	Datum	   *index;			/* Datum 格式的下标值 */
} JsonbSubWorkspace;


/*
 * 完成对 jsonb 的 SubscriptingRef 表达式的解析分析。
 *
 * 变换各下标表达式，并将它们强转为 text，
 * 并确定 SubscriptingRef 节点的结果类型。
 */
static void
jsonb_subscript_transform(SubscriptingRef *sbsref,
						  List *indirection,
						  ParseState *pstate,
						  bool isSlice,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	ListCell   *idx;

	/*
	 * 变换并转换各下标表达式。jsonb 下标不支持
	 * 切片、仅查找以及上界。
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);
		Node	   *subExpr;

		if (isSlice)
		{
			Node	   *expr = ai->uidx ? ai->uidx : ai->lidx;

			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(expr))));
		}

		if (ai->uidx)
		{
			Oid			subExprType = InvalidOid,
						targetType = UNKNOWNOID;

			subExpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			subExprType = exprType(subExpr);

			if (subExprType != UNKNOWNOID)
			{
				Oid			targets[2] = {INT4OID, TEXTOID};

				/*
				 * jsonb 可处理多种下标类型，但一个下标可被
				 * 强制转换为多种目标类型的情形必须避免，
				 * 这与重载函数类似。将来可能可以
				 * 用 jsonpath 加以扩展。
				 */
				for (int i = 0; i < 2; i++)
				{
					if (can_coerce_type(1, &subExprType, &targets[i], COERCION_IMPLICIT))
					{
						/*
						 * 已有一种类型成功，意味着存在两个
						 * 可能的强转目标，失败。
						 */
						if (targetType != UNKNOWNOID)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
									 errhint("jsonb subscript must be coercible to only one type, integer or text."),
									 parser_errposition(pstate, exprLocation(subExpr))));

						targetType = targets[i];
					}
				}

				/*
				 * 未找到合适类型，失败。
				 */
				if (targetType == UNKNOWNOID)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
							 errhint("jsonb subscript must be coercible to either integer or text."),
							 parser_errposition(pstate, exprLocation(subExpr))));
			}
			else
				targetType = TEXTOID;

			/*
			 * 我们从 can_coerce_type 已知强转会成功，因此
			 * 可以使用 coerce_type。注意隐式强转上下文，
			 * 处理不同类型的下标时需要用到它，这与
			 * 重载函数类似。
			 */
			subExpr = coerce_type(pstate,
								  subExpr, subExprType,
								  targetType, -1,
								  COERCION_IMPLICIT,
								  COERCE_IMPLICIT_CAST,
								  -1);
			if (subExpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("jsonb subscript must have text type"),
						 parser_errposition(pstate, exprLocation(subExpr))));
		}
		else
		{
			/*
			 * 省略上界的切片。不应发生，因为我们之前
			 * 已对切片报过错，但仍以防万一处理一下。
			 */
			Assert(isSlice && ai->is_slice);
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(ai->uidx))));
		}

		upperIndexpr = lappend(upperIndexpr, subExpr);
	}

	/* 把变换后的列表存入 SubscriptRef 节点 */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = NIL;

	/* 确定下标操作的结果类型；总是 jsonb */
	sbsref->refrestype = JSONBOID;
	sbsref->reftypmod = -1;
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
 */
static bool
jsonb_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/*
	 * 若第一个下标是整数，则源 jsonb 应当是数组。
	 * 该信息不会被直接使用，所有这类情形都在
	 * 相应的 jsonb 赋值函数内处理。但若源 jsonb 为 NULL，
	 * 按预期类型
	 * 构建空源。
	 */
	if (sbsrefstate->numupper > 0 && sbsrefstate->upperprovided[0] &&
		!sbsrefstate->upperindexnull[0] && workspace->indexOid[0] == INT4OID)
		workspace->expectArray = true;

	/* 处理上界下标 */
	for (int i = 0; i < sbsrefstate->numupper; i++)
	{
		if (sbsrefstate->upperprovided[i])
		{
			/* 索引表达式为 NULL 时结果为 NULL 或报错 */
			if (sbsrefstate->upperindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("jsonb subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}

			/*
			 * 对 jsonb 获取与赋值函数，我们需要以文本
			 * 格式提供路径。若非 text 则转换。
			 */
			if (workspace->indexOid[i] == INT4OID)
			{
				Datum		datum = sbsrefstate->upperindex[i];
				char	   *cs = DatumGetCString(DirectFunctionCall1(int4out, datum));

				workspace->index[i] = CStringGetTextDatum(cs);
			}
			else
				workspace->index[i] = sbsrefstate->upperindex[i];
		}
	}

	return true;
}

/*
 * 为 jsonb 元素求值 SubscriptingRef 获取。
 *
 * 源容器位于 step 的结果变量中（已知非 NULL，因为
 * fetch_strict 为 true）。
 */
static void
jsonb_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;

	/* 源 jsonb 或任一下标为 null 时不应走到这里 */
	Assert(!(*op->resnull));

	jsonbSource = DatumGetJsonbP(*op->resvalue);
	*op->resvalue = jsonb_get_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  op->resnull,
									  false);
}

/*
 * 对 jsonb 元素赋值求取 SubscriptingRef 赋值。
 *
 * 输入容器（可能为 null）位于结果区，替换值位于
 * SubscriptingRefState replacevalue/replacenull。
 */
static void
jsonb_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;
	JsonbValue	replacevalue;

	if (sbsrefstate->replacenull)
		replacevalue.type = jbvNull;
	else
		JsonbToJsonbValue(DatumGetJsonbP(sbsrefstate->replacevalue),
						  &replacevalue);

	/*
	 * 若输入容器为 null，则设置一个空 jsonb 并继续
	 * 进行赋值。
	 */
	if (*op->resnull)
	{
		JsonbValue	newSource;

		/*
		 * 为避免意外结果，若预期是数组（即第一个下
		 * 标是整数），则设置空 jsonb 数组，否则设置为
		 * jsonb 对象。
		 */
		if (workspace->expectArray)
		{
			newSource.type = jbvArray;
			newSource.val.array.nElems = 0;
			newSource.val.array.rawScalar = false;
		}
		else
		{
			newSource.type = jbvObject;
			newSource.val.object.nPairs = 0;
		}

		jsonbSource = JsonbValueToJsonb(&newSource);
		*op->resnull = false;
	}
	else
		jsonbSource = DatumGetJsonbP(*op->resvalue);

	*op->resvalue = jsonb_set_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  &replacevalue);
	/* 结果永不为 NULL，因此无需改动 *op->resnull */
}

/*
 * 为 SubscriptingRef 赋值表达式计算旧的 jsonb
 * 元素值。仅当新值子表达式包含 SubscriptingRef
 * 或 FieldStore 时才会被调用。这与常规获取情形
 * 相同，只是我们必须处理 null jsonb，且值要
 * 存入 SubscriptingRefState 的 prevvalue/
 * prevnull 字段。
 */
static void
jsonb_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;

	if (*op->resnull)
	{
		/* jsonb 整体为 null，元素也为 null */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		sbsrefstate->prevvalue = jsonb_get_element(jsonbSource,
												   sbsrefstate->upperindex,
												   sbsrefstate->numupper,
												   &sbsrefstate->prevnull,
												   false);
	}
}

/*
 * 为 jsonb 下标操作建立执行状态。与数组下标化不同，
 * 下标个数没有限制，因为 jsonb 类型本身没有嵌套
 * 限制。
 */
static void
jsonb_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	JsonbSubWorkspace *workspace;
	ListCell   *lc;
	int			nupper = sbsref->refupperindexpr->length;
	char	   *ptr;

	/* 分配类型专属工作区，内含各下标数据的空间 */
	workspace = palloc0(MAXALIGN(sizeof(JsonbSubWorkspace)) +
						nupper * (sizeof(Datum) + sizeof(Oid)));
	workspace->expectArray = false;
	ptr = ((char *) workspace) + MAXALIGN(sizeof(JsonbSubWorkspace));

	/*
	 * 该编码假定 sizeof(Datum) >= sizeof(Oid)，否则
	 * 会让 indexOid 指针错位
	 */
	workspace->index = (Datum *) ptr;
	ptr += nupper * sizeof(Datum);
	workspace->indexOid = (Oid *) ptr;

	sbsrefstate->workspace = workspace;

	/* 收集执行时所需的下标数据类型 */
	foreach(lc, sbsref->refupperindexpr)
	{
		Node	   *expr = lfirst(lc);
		int			i = foreach_current_index(lc);

		workspace->indexOid[i] = exprType(expr);
	}

	/*
	 * 回传指向相应步骤执行函数的指针。
	 */
	methods->sbs_check_subscripts = jsonb_subscript_check_subscripts;
	methods->sbs_fetch = jsonb_subscript_fetch;
	methods->sbs_assign = jsonb_subscript_assign;
	methods->sbs_fetch_old = jsonb_subscript_fetch_old;
}

/*
 * jsonb_subscript_handler
 *		jsonb 的下标处理器。
 *
 */
Datum
jsonb_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = jsonb_subscript_transform,
		.exec_setup = jsonb_exec_setup,
		.fetch_strict = true,	/* 输入为 NULL 时获取 NULL */
		.fetch_leakproof = true,	/* 下标不当时获取返回 NULL */
		.store_leakproof = false	/* 但赋值则抛出错误 */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
