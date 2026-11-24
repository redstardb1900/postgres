/*-------------------------------------------------------------------------
 *
 * jsonpath_exec.c
 *	 SQL/JSON 路径执行例程。
 *
 * Jsonpath 在存于 JsonPathExecContext 的全局上下文中执行，
 * 该上下文几乎传给参与执行的每个函数。jsonpath
 * 执行入口是 executeJsonPath()，它会
 * 初始化执行上下文，含初始 JsonPathItem 与 JsonbValue、
 * 标志及过滤器中计算 @ 用的栈。
 *
 * jsonpath 查询执行结果为枚举 JsonPathExecResult，若成功
 * 还有写入 JsonValueList *found 的 JsonbValue 序列，该列表
 * 沿 jsonpath 各项传递。当 found == NULL 时，处于
 * exists 查询，只关心结果是否为空。此
 * 时一旦找到首个结果项即停，唯一的
 * 执行结果是 JsonPathExecResult。JsonPathExecResult 取值
 * 如下：
 * - jperOk			-- 结果序列非空
 * - jperNotFound	-- 结果序列为空
 * - jperError		-- 执行中发生错误
 *
 * Jsonpath 自首个路径项起递归执行（见 executeItem()；
 * 首项本身也可能是单独求值的算术
 * 表达式）。每步处理自前一路径项得到的单个 JsonbValue。
 * 处理结果是一个 JsonbValue 序列（可为空），再逐个
 * 传给下一路径项。若无下一路径项，则把
 * JsonbValue 加入 'found' 列表。当 found == NULL 时，
 * 执行函数只返回 jperOk（见
 * executeNextItem()）。
 *
 * 宽松模式下，许多 jsonpath 运算要求自动解包数组。
 * 因此若输入值是数组，则相应运算并非
 * 作用在数组本身，而是逐个作用在其各成员上。
 * executeItemOptUnwrapTarget() 有 'unwrap' 参数，指示
 * 是否需要解包数组。当 unwrap == true 时，数组的每个
 * 成员再以 unwrap == false 传入 executeItemOptUnwrapTarget()，
 * 以免后续再次解包数组。
 *
 * 所有布尔表达式（谓词）由 executeBoolItem()
 * 求值，返回三态 JsonPathBool。谓词执行中若
 * 出错则返回 jpbUnknown。按标准，谓词
 * 只能出现在过滤器内。但我们支持把它用作
 * jsonpath 表达式，以便实现 @@ 运算符。此时
 * 把结果 JsonPathBool 转成 jsonb 布尔或 null。
 *
 * 算术与布尔表达式自表达式树顶向下递归求值
 * 至叶。因此对二元算术表达式，我们先
 * 计算操作数，再检查结果是否为数值
 * 单例列表，算出结果并传给下一路径项。
 *
 * Copyright (c) 2019-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/jsonpath_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonpath.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/*
 * 表示 .keyvalue() 求值用的"基对象"及其 "id"。
 */
typedef struct JsonBaseObjectInfo
{
	JsonbContainer *jbc;
	int			id;
} JsonBaseObjectInfo;

/*
 * jsonpath 执行上下文。
 */
typedef struct JsonPathExecContext
{
	Jsonb	   *vars;			/* 代入 jsonpath 的变量 */
	JsonbValue *root;			/* 供 $ 求值 */
	JsonbValue *current;		/* 供 @ 求值 */
	JsonBaseObjectInfo baseObject;	/* .keyvalue() 的"基对象"
									 * 求值 */
	int			lastGeneratedObjectId;	/* .keyvalue() 的id计数器
										 * 求值 */
	int			innermostArraySize; /* 供 LAST 数组下标求值 */
	bool		laxMode;		/* "lax" 真=宽松，"strict" 假=严格
								 * mode */
	bool		ignoreStructuralErrors; /* 为 true 时结构错误如
										 * 缺所需 json 项或
										 * 项类型不符会被
										 * 忽略 */
	bool		throwErrors;	/* 为 false 时全部可抑制错误均被
								 * 抑制 */
	bool		useTz;
} JsonPathExecContext;

/* LIKE_REGEX 执行上下文。 */
typedef struct JsonLikeRegexContext
{
	text	   *regex;
	int			cflags;
} JsonLikeRegexContext;

/* jsonpath 谓词求值结果 */
typedef enum JsonPathBool
{
	jpbFalse = 0,
	jpbTrue = 1,
	jpbUnknown = 2
} JsonPathBool;

/* jsonpath 表达式求值结果 */
typedef enum JsonPathExecResult
{
	jperOk = 0,
	jperNotFound = 1,
	jperError = 2
} JsonPathExecResult;

#define jperIsError(jper)			((jper) == jperError)

/*
 * 带单值列表快捷路径的 jsonb 值列表。
 */
typedef struct JsonValueList
{
	JsonbValue *singleton;
	List	   *list;
} JsonValueList;

typedef struct JsonValueListIterator
{
	JsonbValue *value;
	List	   *list;
	ListCell   *next;
} JsonValueListIterator;

/* 严格/宽松标志分解为四[解]包/错标志 */
#define jspStrictAbsenseOfErrors(cxt)	(!(cxt)->laxMode)
#define jspAutoUnwrap(cxt)				((cxt)->laxMode)
#define jspAutoWrap(cxt)				((cxt)->laxMode)
#define jspIgnoreStructuralErrors(cxt)	((cxt)->ignoreStructuralErrors)
#define jspThrowErrors(cxt)				((cxt)->throwErrors)

/* 便捷宏：按上下文返回或抛错 */
#define RETURN_ERROR(throw_error) \
do { \
	if (jspThrowErrors(cxt)) \
		throw_error; \
	else \
		return jperError; \
} while (0)

typedef JsonPathBool (*JsonPathPredicateCallback) (JsonPathItem *jsp,
												   JsonbValue *larg,
												   JsonbValue *rarg,
												   void *param);
typedef Numeric (*BinaryArithmFunc) (Numeric num1, Numeric num2, bool *error);

static JsonPathExecResult executeJsonPath(JsonPath *path, Jsonb *vars,
										  Jsonb *json, bool throwErrors,
										  JsonValueList *result, bool useTz);
static JsonPathExecResult executeItem(JsonPathExecContext *cxt,
									  JsonPathItem *jsp, JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapTarget(JsonPathExecContext *cxt,
													 JsonPathItem *jsp, JsonbValue *jb,
													 JsonValueList *found, bool unwrap);
static JsonPathExecResult executeItemUnwrapTargetArray(JsonPathExecContext *cxt,
													   JsonPathItem *jsp, JsonbValue *jb,
													   JsonValueList *found, bool unwrapElements);
static JsonPathExecResult executeNextItem(JsonPathExecContext *cxt,
										  JsonPathItem *cur, JsonPathItem *next,
										  JsonbValue *v, JsonValueList *found, bool copy);
static JsonPathExecResult executeItemOptUnwrapResult(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbValue *jb,
													 bool unwrap, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapResultNoThrow(JsonPathExecContext *cxt, JsonPathItem *jsp,
															JsonbValue *jb, bool unwrap, JsonValueList *found);
static JsonPathBool executeBoolItem(JsonPathExecContext *cxt,
									JsonPathItem *jsp, JsonbValue *jb, bool canHaveNext);
static JsonPathBool executeNestedBoolItem(JsonPathExecContext *cxt,
										  JsonPathItem *jsp, JsonbValue *jb);
static JsonPathExecResult executeAnyItem(JsonPathExecContext *cxt,
										 JsonPathItem *jsp, JsonbContainer *jbc, JsonValueList *found,
										 uint32 level, uint32 first, uint32 last,
										 bool ignoreStructuralErrors, bool unwrapNext);
static JsonPathBool executePredicate(JsonPathExecContext *cxt,
									 JsonPathItem *pred, JsonPathItem *larg, JsonPathItem *rarg,
									 JsonbValue *jb, bool unwrapRightArg,
									 JsonPathPredicateCallback exec, void *param);
static JsonPathExecResult executeBinaryArithmExpr(JsonPathExecContext *cxt,
												  JsonPathItem *jsp, JsonbValue *jb,
												  BinaryArithmFunc func, JsonValueList *found);
static JsonPathExecResult executeUnaryArithmExpr(JsonPathExecContext *cxt,
												 JsonPathItem *jsp, JsonbValue *jb, PGFunction func,
												 JsonValueList *found);
static JsonPathBool executeStartsWith(JsonPathItem *jsp,
									  JsonbValue *whole, JsonbValue *initial, void *param);
static JsonPathBool executeLikeRegex(JsonPathItem *jsp, JsonbValue *str,
									 JsonbValue *rarg, void *param);
static JsonPathExecResult executeNumericItemMethod(JsonPathExecContext *cxt,
												   JsonPathItem *jsp, JsonbValue *jb, bool unwrap, PGFunction func,
												   JsonValueList *found);
static JsonPathExecResult executeDateTimeMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
												JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult executeKeyValueMethod(JsonPathExecContext *cxt,
												JsonPathItem *jsp, JsonbValue *jb, JsonValueList *found);
static JsonPathExecResult appendBoolResult(JsonPathExecContext *cxt,
										   JsonPathItem *jsp, JsonValueList *found, JsonPathBool res);
static void getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
							JsonbValue *value);
static void getJsonPathVariable(JsonPathExecContext *cxt,
								JsonPathItem *variable, Jsonb *vars, JsonbValue *value);
static int	JsonbArraySize(JsonbValue *jb);
static JsonPathBool executeComparison(JsonPathItem *cmp, JsonbValue *lv,
									  JsonbValue *rv, void *p);
static JsonPathBool compareItems(int32 op, JsonbValue *jb1, JsonbValue *jb2,
								 bool useTz);
static int	compareNumeric(Numeric a, Numeric b);
static JsonbValue *copyJsonbValue(JsonbValue *src);
static JsonPathExecResult getArrayIndex(JsonPathExecContext *cxt,
										JsonPathItem *jsp, JsonbValue *jb, int32 *index);
static JsonBaseObjectInfo setBaseObject(JsonPathExecContext *cxt,
										JsonbValue *jbv, int32 id);
static void JsonValueListAppend(JsonValueList *jvl, JsonbValue *jbv);
static int	JsonValueListLength(const JsonValueList *jvl);
static bool JsonValueListIsEmpty(JsonValueList *jvl);
static JsonbValue *JsonValueListHead(JsonValueList *jvl);
static List *JsonValueListGetList(JsonValueList *jvl);
static void JsonValueListInitIterator(const JsonValueList *jvl,
									  JsonValueListIterator *it);
static JsonbValue *JsonValueListNext(const JsonValueList *jvl,
									 JsonValueListIterator *it);
static int	JsonbType(JsonbValue *jb);
static JsonbValue *JsonbInitBinary(JsonbValue *jbv, Jsonb *jb);
static int	JsonbType(JsonbValue *jb);
static JsonbValue *getScalar(JsonbValue *scalar, enum jbvType type);
static JsonbValue *wrapItemsInArray(const JsonValueList *items);
static int	compareDatetime(Datum val1, Oid typid1, Datum val2, Oid typid2,
							bool useTz, bool *cast_error);

/****************** User interface to JsonPath executor ********************/

/*
 * jsonb_path_exists
 *		若 jsonpath 对指定 jsonb 值至少返回一项则
 *		为真。本函数与 jsonb_path_match() 用于
 *		实现 @? 与 @@ 运算符，而这些运算符又旨在获得
 *		索引支持。因此希望更易达成索引扫描与
 *		顺序扫描结果的一致性。
 *		故尽量少抛错。对本函数而言，
 *		此行为也与 SQL/JSON 的 JSON_EXISTS() 子句
 *		一致。至于 jsonb_path_match()，SQL/JSON 中并无
 *		对应物，故自行定义其行为。
 */
static Datum
jsonb_path_exists_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonPathExecResult res;
	Jsonb	   *vars = NULL;
	bool		silent = true;

	if (PG_NARGS() == 4)
	{
		vars = PG_GETARG_JSONB_P(2);
		silent = PG_GETARG_BOOL(3);
	}

	res = executeJsonPath(jp, vars, jb, !silent, NULL, tz);

	PG_FREE_IF_COPY(jb, 0);
	PG_FREE_IF_COPY(jp, 1);

	if (jperIsError(res))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(res == jperOk);
}

Datum
jsonb_path_exists(PG_FUNCTION_ARGS)
{
	return jsonb_path_exists_internal(fcinfo, false);
}

Datum
jsonb_path_exists_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_exists_internal(fcinfo, true);
}

/*
 * jsonb_path_exists_opr
 *		"jsonb @? jsonpath" 的实现（jsonb_path_exists()
 *		双参数版）。
 */
Datum
jsonb_path_exists_opr(PG_FUNCTION_ARGS)
{
	/* 直接调另一版——可处理两种情况 */
	return jsonb_path_exists_internal(fcinfo, false);
}

/*
 * jsonb_path_match
 *		返回指定 jsonb 值上的 jsonpath 谓词结果项。
 *		错误处理细节见 jsonb_path_exists() 注释。
 */
static Datum
jsonb_path_match_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = NULL;
	bool		silent = true;

	if (PG_NARGS() == 4)
	{
		vars = PG_GETARG_JSONB_P(2);
		silent = PG_GETARG_BOOL(3);
	}

	(void) executeJsonPath(jp, vars, jb, !silent, &found, tz);

	PG_FREE_IF_COPY(jb, 0);
	PG_FREE_IF_COPY(jp, 1);

	if (JsonValueListLength(&found) == 1)
	{
		JsonbValue *jbv = JsonValueListHead(&found);

		if (jbv->type == jbvBool)
			PG_RETURN_BOOL(jbv->val.boolean);

		if (jbv->type == jbvNull)
			PG_RETURN_NULL();
	}

	if (!silent)
		ereport(ERROR,
				(errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
				 errmsg("single boolean result is expected")));

	PG_RETURN_NULL();
}

Datum
jsonb_path_match(PG_FUNCTION_ARGS)
{
	return jsonb_path_match_internal(fcinfo, false);
}

Datum
jsonb_path_match_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_match_internal(fcinfo, true);
}

/*
 * jsonb_path_match_opr
 *		"jsonb @@ jsonpath" 的实现（jsonb_path_match()
 *		双参数版）。
 */
Datum
jsonb_path_match_opr(PG_FUNCTION_ARGS)
{
	/* 直接调另一版——可处理两种情况 */
	return jsonb_path_match_internal(fcinfo, false);
}

/*
 * jsonb_path_query
 *		对给定 jsonb 文档执行 jsonpath，结果返回为
 *		行集
 */
static Datum
jsonb_path_query_internal(FunctionCallInfo fcinfo, bool tz)
{
	FuncCallContext *funcctx;
	List	   *found;
	JsonbValue *v;
	ListCell   *c;

	if (SRF_IS_FIRSTCALL())
	{
		JsonPath   *jp;
		Jsonb	   *jb;
		MemoryContext oldcontext;
		Jsonb	   *vars;
		bool		silent;
		JsonValueList found = {0};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		jb = PG_GETARG_JSONB_P_COPY(0);
		jp = PG_GETARG_JSONPATH_P_COPY(1);
		vars = PG_GETARG_JSONB_P_COPY(2);
		silent = PG_GETARG_BOOL(3);

		(void) executeJsonPath(jp, vars, jb, !silent, &found, tz);

		funcctx->user_fctx = JsonValueListGetList(&found);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	found = funcctx->user_fctx;

	c = list_head(found);

	if (c == NULL)
		SRF_RETURN_DONE(funcctx);

	v = lfirst(c);
	funcctx->user_fctx = list_delete_first(found);

	SRF_RETURN_NEXT(funcctx, JsonbPGetDatum(JsonbValueToJsonb(v)));
}

Datum
jsonb_path_query(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_internal(fcinfo, false);
}

Datum
jsonb_path_query_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_internal(fcinfo, true);
}

/*
 * jsonb_path_query_array
 *		对给定 jsonb 执行 jsonpath，结果返回为
 *		jsonb数组
 */
static Datum
jsonb_path_query_array_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = PG_GETARG_JSONB_P(2);
	bool		silent = PG_GETARG_BOOL(3);

	(void) executeJsonPath(jp, vars, jb, !silent, &found, tz);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(wrapItemsInArray(&found)));
}

Datum
jsonb_path_query_array(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_array_internal(fcinfo, false);
}

Datum
jsonb_path_query_array_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_array_internal(fcinfo, true);
}

/*
 * jsonb_path_query_first
 *		对给定 jsonb 文档执行 jsonpath，并返回首个结果
 *		项。若无项则返回 NULL。
 */
static Datum
jsonb_path_query_first_internal(FunctionCallInfo fcinfo, bool tz)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonPath   *jp = PG_GETARG_JSONPATH_P(1);
	JsonValueList found = {0};
	Jsonb	   *vars = PG_GETARG_JSONB_P(2);
	bool		silent = PG_GETARG_BOOL(3);

	(void) executeJsonPath(jp, vars, jb, !silent, &found, tz);

	if (JsonValueListLength(&found) >= 1)
		PG_RETURN_JSONB_P(JsonbValueToJsonb(JsonValueListHead(&found)));
	else
		PG_RETURN_NULL();
}

Datum
jsonb_path_query_first(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_first_internal(fcinfo, false);
}

Datum
jsonb_path_query_first_tz(PG_FUNCTION_ARGS)
{
	return jsonb_path_query_first_internal(fcinfo, true);
}

/********************Execute functions for JsonPath**************************/

/*
 * jsonpath 执行器接口
 *
 * 'path' - 待执行的 jsonpath
 * 'vars' - 代入 jsonpath 的变量
 * 'json' - jsonpath 求值的目标文档
 * 'throwErrors' - 是否应抛出可抑制错误
 * 'result' - 存放结果项的列表
 *
 * 处理中若有可恢复错误则返回之，无错则
 * 为 NULL。
 *
 * 注意：工作期间 jsonb 与 jsonpath 值须可用且已
 * 解除 TOAST，因 JsonPathItem、JsonbValue 与结果项可能持有
 * 指向输入的指针。若调用方只需检查文档是否匹配
 * jsonpath，则不提供 result 参数。此时执行器
 * 工作到首个肯定结果即止，并尽可能不检查其余。
 * 否则尝试找出全部满足的结果项。
 */
static JsonPathExecResult
executeJsonPath(JsonPath *path, Jsonb *vars, Jsonb *json, bool throwErrors,
				JsonValueList *result, bool useTz)
{
	JsonPathExecContext cxt;
	JsonPathExecResult res;
	JsonPathItem jsp;
	JsonbValue	jbv;

	jspInit(&jsp, path);

	if (!JsonbExtractScalar(&json->root, &jbv))
		JsonbInitBinary(&jbv, json);

	if (vars && !JsonContainerIsObject(&vars->root))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"vars\" argument is not an object"),
				 errdetail("Jsonpath parameters should be encoded as key-value pairs of \"vars\" object.")));
	}

	cxt.vars = vars;
	cxt.laxMode = (path->header & JSONPATH_LAX) != 0;
	cxt.ignoreStructuralErrors = cxt.laxMode;
	cxt.root = &jbv;
	cxt.current = &jbv;
	cxt.baseObject.jbc = NULL;
	cxt.baseObject.id = 0;
	cxt.lastGeneratedObjectId = vars ? 2 : 1;
	cxt.innermostArraySize = -1;
	cxt.throwErrors = throwErrors;
	cxt.useTz = useTz;

	if (jspStrictAbsenseOfErrors(&cxt) && !result)
	{
		/*
		 * 严格模式下必须取得完整值列表，以检查
		 * 完全没有错误。
		 */
		JsonValueList vals = {0};

		res = executeItem(&cxt, &jsp, &jbv, &vals);

		if (jperIsError(res))
			return res;

		return JsonValueListIsEmpty(&vals) ? jperNotFound : jperOk;
	}

	res = executeItem(&cxt, &jsp, &jbv, result);

	Assert(!throwErrors || !jperIsError(res));

	return res;
}

/*
 * 在宽松模式下对当前项自动解包后执行 jsonpath。
 */
static JsonPathExecResult
executeItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
			JsonbValue *jb, JsonValueList *found)
{
	return executeItemOptUnwrapTarget(cxt, jsp, jb, found, jspAutoUnwrap(cxt));
}

/*
 * 主 jsonpath 执行函数：遍历 jsonpath 结构，找出
 * jsonb 相关部分并对其求值表达式。
 * 当 'unwrap' 为真且当前 SQL/JSON 项为数组时则解包。
 */
static JsonPathExecResult
executeItemOptUnwrapTarget(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonbValue *jb, JsonValueList *found, bool unwrap)
{
	JsonPathItem elem;
	JsonPathExecResult res = jperNotFound;
	JsonBaseObjectInfo baseObject;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (jsp->type)
	{
			/* 全布尔项类型： */
		case jpiAnd:
		case jpiOr:
		case jpiNot:
		case jpiIsUnknown:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiExists:
		case jpiStartsWith:
		case jpiLikeRegex:
			{
				JsonPathBool st = executeBoolItem(cxt, jsp, jb, true);

				res = appendBoolResult(cxt, jsp, found, st);
				break;
			}

		case jpiKey:
			if (JsonbType(jb) == jbvObject)
			{
				JsonbValue *v;
				JsonbValue	key;

				key.type = jbvString;
				key.val.string.val = jspGetString(jsp, &key.val.string.len);

				v = findJsonbValueFromContainer(jb->val.binary.data,
												JB_FOBJECT, &key);

				if (v != NULL)
				{
					res = executeNextItem(cxt, jsp, NULL,
										  v, found, false);

					/* 若值未加入 found 列表则释放 */
					if (jspHasNext(jsp) || !found)
						pfree(v);
				}
				else if (!jspIgnoreStructuralErrors(cxt))
				{
					Assert(found);

					if (!jspThrowErrors(cxt))
						return jperError;

					ereport(ERROR,
							(errcode(ERRCODE_SQL_JSON_MEMBER_NOT_FOUND), \
							 errmsg("JSON object does not contain key \"%s\"",
									pnstrdup(key.val.string.val,
											 key.val.string.len))));
				}
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_MEMBER_NOT_FOUND),
									  errmsg("jsonpath member accessor can only be applied to an object"))));
			}
			break;

		case jpiRoot:
			jb = cxt->root;
			baseObject = setBaseObject(cxt, jb, 0);
			res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			cxt->baseObject = baseObject;
			break;

		case jpiCurrent:
			res = executeNextItem(cxt, jsp, NULL, cxt->current,
								  found, true);
			break;

		case jpiAnyArray:
			if (JsonbType(jb) == jbvArray)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				res = executeItemUnwrapTargetArray(cxt, hasNext ? &elem : NULL,
												   jb, found, jspAutoUnwrap(cxt));
			}
			else if (jspAutoWrap(cxt))
				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			else if (!jspIgnoreStructuralErrors(cxt))
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
									  errmsg("jsonpath wildcard array accessor can only be applied to an array"))));
			break;

		case jpiIndexArray:
			if (JsonbType(jb) == jbvArray || jspAutoWrap(cxt))
			{
				int			innermostArraySize = cxt->innermostArraySize;
				int			i;
				int			size = JsonbArraySize(jb);
				bool		singleton = size < 0;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (singleton)
					size = 1;

				cxt->innermostArraySize = size; /* 供 LAST 求值 */

				for (i = 0; i < jsp->content.array.nelems; i++)
				{
					JsonPathItem from;
					JsonPathItem to;
					int32		index;
					int32		index_from;
					int32		index_to;
					bool		range = jspGetArraySubscript(jsp, &from,
															 &to, i);

					res = getArrayIndex(cxt, &from, jb, &index_from);

					if (jperIsError(res))
						break;

					if (range)
					{
						res = getArrayIndex(cxt, &to, jb, &index_to);

						if (jperIsError(res))
							break;
					}
					else
						index_to = index_from;

					if (!jspIgnoreStructuralErrors(cxt) &&
						(index_from < 0 ||
						 index_from > index_to ||
						 index_to >= size))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
											  errmsg("jsonpath array subscript is out of bounds"))));

					if (index_from < 0)
						index_from = 0;

					if (index_to >= size)
						index_to = size - 1;

					res = jperNotFound;

					for (index = index_from; index <= index_to; index++)
					{
						JsonbValue *v;
						bool		copy;

						if (singleton)
						{
							v = jb;
							copy = true;
						}
						else
						{
							v = getIthJsonbValueFromContainer(jb->val.binary.data,
															  (uint32) index);

							if (v == NULL)
								continue;

							copy = false;
						}

						if (!hasNext && !found)
							return jperOk;

						res = executeNextItem(cxt, jsp, &elem, v, found,
											  copy);

						if (jperIsError(res))
							break;

						if (res == jperOk && !found)
							break;
					}

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}

				cxt->innermostArraySize = innermostArraySize;
			}
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
									  errmsg("jsonpath array accessor can only be applied to an array"))));
			}
			break;

		case jpiLast:
			{
				JsonbValue	tmpjbv;
				JsonbValue *lastjbv;
				int			last;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (cxt->innermostArraySize < 0)
					elog(ERROR, "evaluating jsonpath LAST outside of array subscript");

				if (!hasNext && !found)
				{
					res = jperOk;
					break;
				}

				last = cxt->innermostArraySize - 1;

				lastjbv = hasNext ? &tmpjbv : palloc(sizeof(*lastjbv));

				lastjbv->type = jbvNumeric;
				lastjbv->val.numeric = int64_to_numeric(last);

				res = executeNextItem(cxt, jsp, &elem,
									  lastjbv, found, hasNext);
			}
			break;

		case jpiAnyKey:
			if (JsonbType(jb) == jbvObject)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				if (jb->type != jbvBinary)
					elog(ERROR, "invalid jsonb object type: %d", jb->type);

				return executeAnyItem
					(cxt, hasNext ? &elem : NULL,
					 jb->val.binary.data, found, 1, 1, 1,
					 false, jspAutoUnwrap(cxt));
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_SQL_JSON_OBJECT_NOT_FOUND),
									  errmsg("jsonpath wildcard member accessor can only be applied to an object"))));
			}
			break;

		case jpiAdd:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_add_opt_error, found);

		case jpiSub:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_sub_opt_error, found);

		case jpiMul:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mul_opt_error, found);

		case jpiDiv:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_div_opt_error, found);

		case jpiMod:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mod_opt_error, found);

		case jpiPlus:
			return executeUnaryArithmExpr(cxt, jsp, jb, NULL, found);

		case jpiMinus:
			return executeUnaryArithmExpr(cxt, jsp, jb, numeric_uminus,
										  found);

		case jpiFilter:
			{
				JsonPathBool st;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				jspGetArg(jsp, &elem);
				st = executeNestedBoolItem(cxt, &elem, jb);
				if (st != jpbTrue)
					res = jperNotFound;
				else
					res = executeNextItem(cxt, jsp, NULL,
										  jb, found, true);
				break;
			}

		case jpiAny:
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				/* 先试不经中间步骤 */
				if (jsp->content.anybounds.first == 0)
				{
					bool		savedIgnoreStructuralErrors;

					savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
					cxt->ignoreStructuralErrors = true;
					res = executeNextItem(cxt, jsp, &elem,
										  jb, found, true);
					cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;

					if (res == jperOk && !found)
						break;
				}

				if (jb->type == jbvBinary)
					res = executeAnyItem
						(cxt, hasNext ? &elem : NULL,
						 jb->val.binary.data, found,
						 1,
						 jsp->content.anybounds.first,
						 jsp->content.anybounds.last,
						 true, jspAutoUnwrap(cxt));
				break;
			}

		case jpiNull:
		case jpiBool:
		case jpiNumeric:
		case jpiString:
		case jpiVariable:
			{
				JsonbValue	vbuf;
				JsonbValue *v;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (!hasNext && !found && jsp->type != jpiVariable)
				{
					/*
					 * 跳过求值，但变量除外。对缺失变量
					 * 必须触发错误。
					 */
					res = jperOk;
					break;
				}

				v = hasNext ? &vbuf : palloc(sizeof(*v));

				baseObject = cxt->baseObject;
				getJsonPathItem(cxt, jsp, v);

				res = executeNextItem(cxt, jsp, &elem,
									  v, found, hasNext);
				cxt->baseObject = baseObject;
			}
			break;

		case jpiType:
			{
				JsonbValue *jbv = palloc(sizeof(*jbv));

				jbv->type = jbvString;
				jbv->val.string.val = pstrdup(JsonbTypeName(jb));
				jbv->val.string.len = strlen(jbv->val.string.val);

				res = executeNextItem(cxt, jsp, NULL, jbv,
									  found, false);
			}
			break;

		case jpiSize:
			{
				int			size = JsonbArraySize(jb);

				if (size < 0)
				{
					if (!jspAutoWrap(cxt))
					{
						if (!jspIgnoreStructuralErrors(cxt))
							RETURN_ERROR(ereport(ERROR,
												 (errcode(ERRCODE_SQL_JSON_ARRAY_NOT_FOUND),
												  errmsg("jsonpath item method .%s() can only be applied to an array",
														 jspOperationName(jsp->type)))));
						break;
					}

					size = 1;
				}

				jb = palloc(sizeof(*jb));

				jb->type = jbvNumeric;
				jb->val.numeric = int64_to_numeric(size);

				res = executeNextItem(cxt, jsp, NULL, jb, found, false);
			}
			break;

		case jpiAbs:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_abs,
											found);

		case jpiFloor:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_floor,
											found);

		case jpiCeiling:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_ceil,
											found);

		case jpiDouble:
			{
				JsonbValue	jbv;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					char	   *tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
																		  NumericGetDatum(jb->val.numeric)));
					double		val;
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					val = float8in_internal(tmp,
											NULL,
											"double precision",
											tmp,
											(Node *) &escontext);

					if (escontext.error_occurred || isinf(val) || isnan(val))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("numeric argument of jsonpath item method .%s() is out of range for type double precision",
													 jspOperationName(jsp->type)))));
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* 字符串转 double */
					double		val;
					char	   *tmp = pnstrdup(jb->val.string.val,
											   jb->val.string.len);
					ErrorSaveContext escontext = {T_ErrorSaveContext};

					val = float8in_internal(tmp,
											NULL,
											"double precision",
											tmp,
											(Node *) &escontext);

					if (escontext.error_occurred || isinf(val) || isnan(val))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
											  errmsg("string argument of jsonpath item method .%s() is not a valid representation of a double precision number",
													 jspOperationName(jsp->type)))));

					jb = &jbv;
					jb->type = jbvNumeric;
					jb->val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric,
																		  Float8GetDatum(val)));
					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
										  errmsg("jsonpath item method .%s() can only be applied to a string or numeric value",
												 jspOperationName(jsp->type)))));

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiDatetime:
			if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

			return executeDateTimeMethod(cxt, jsp, jb, found);

		case jpiKeyValue:
			if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

			return executeKeyValueMethod(cxt, jsp, jb, found);

		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", jsp->type);
	}

	return res;
}

/*
 * 解包当前数组项，并对各元素执行 jsonpath。
 */
static JsonPathExecResult
executeItemUnwrapTargetArray(JsonPathExecContext *cxt, JsonPathItem *jsp,
							 JsonbValue *jb, JsonValueList *found,
							 bool unwrapElements)
{
	if (jb->type != jbvBinary)
	{
		Assert(jb->type != jbvArray);
		elog(ERROR, "invalid jsonb array value type: %d", jb->type);
	}

	return executeAnyItem
		(cxt, jsp, jb->val.binary.data, found, 1, 1, 1,
		 false, unwrapElements);
}

/*
 * 若有下一 jsonpath 项则执行。否则若有 found
 * 则放入 v。
 */
static JsonPathExecResult
executeNextItem(JsonPathExecContext *cxt,
				JsonPathItem *cur, JsonPathItem *next,
				JsonbValue *v, JsonValueList *found, bool copy)
{
	JsonPathItem elem;
	bool		hasNext;

	if (!cur)
		hasNext = next != NULL;
	else if (next)
		hasNext = jspHasNext(cur);
	else
	{
		next = &elem;
		hasNext = jspGetNext(cur, next);
	}

	if (hasNext)
		return executeItem(cxt, next, v, found);

	if (found)
		JsonValueListAppend(found, copy ? copyJsonbValue(v) : v);

	return jperOk;
}

/*
 * 同 executeItem()，但 unwrap==true 时在宽松模式
 * 下自动解包结果序列中每个数组项。
 */
static JsonPathExecResult
executeItemOptUnwrapResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonbValue *jb, bool unwrap,
						   JsonValueList *found)
{
	if (unwrap && jspAutoUnwrap(cxt))
	{
		JsonValueList seq = {0};
		JsonValueListIterator it;
		JsonPathExecResult res = executeItem(cxt, jsp, jb, &seq);
		JsonbValue *item;

		if (jperIsError(res))
			return res;

		JsonValueListInitIterator(&seq, &it);
		while ((item = JsonValueListNext(&seq, &it)))
		{
			Assert(item->type != jbvArray);

			if (JsonbType(item) == jbvArray)
				executeItemUnwrapTargetArray(cxt, NULL, item, found, false);
			else
				JsonValueListAppend(found, item);
		}

		return jperOk;
	}

	return executeItem(cxt, jsp, jb, found);
}

/*
 * 同 executeItemOptUnwrapResult()，但抑制错误。
 */
static JsonPathExecResult
executeItemOptUnwrapResultNoThrow(JsonPathExecContext *cxt,
								  JsonPathItem *jsp,
								  JsonbValue *jb, bool unwrap,
								  JsonValueList *found)
{
	JsonPathExecResult res;
	bool		throwErrors = cxt->throwErrors;

	cxt->throwErrors = false;
	res = executeItemOptUnwrapResult(cxt, jsp, jb, unwrap, found);
	cxt->throwErrors = throwErrors;

	return res;
}

/* 执行布尔值 jsonpath 表达式。 */
static JsonPathBool
executeBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
				JsonbValue *jb, bool canHaveNext)
{
	JsonPathItem larg;
	JsonPathItem rarg;
	JsonPathBool res;
	JsonPathBool res2;

	/* 本函数会递归，可能被驱至栈溢出 */
	check_stack_depth();

	if (!canHaveNext && jspHasNext(jsp))
		elog(ERROR, "boolean jsonpath item cannot have next item");

	switch (jsp->type)
	{
		case jpiAnd:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbFalse)
				return jpbFalse;

			/*
			 * SQL/JSON 规定出错时仍应检查第二参数，
			 * jperError
			 */

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbTrue ? res : res2;

		case jpiOr:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbTrue)
				return jpbTrue;

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbFalse ? res : res2;

		case jpiNot:
			jspGetArg(jsp, &larg);

			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbUnknown)
				return jpbUnknown;

			return res == jpbTrue ? jpbFalse : jpbTrue;

		case jpiIsUnknown:
			jspGetArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);
			return res == jpbUnknown ? jpbTrue : jpbFalse;

		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
			jspGetLeftArg(jsp, &larg);
			jspGetRightArg(jsp, &rarg);
			return executePredicate(cxt, jsp, &larg, &rarg, jb, true,
									executeComparison, cxt);

		case jpiStartsWith:		/* 'whole STARTS WITH initial' */
			jspGetLeftArg(jsp, &larg);	/* 'whole' */
			jspGetRightArg(jsp, &rarg); /* 'initial' */
			return executePredicate(cxt, jsp, &larg, &rarg, jb, false,
									executeStartsWith, NULL);

		case jpiLikeRegex:		/* 'expr LIKE_REGEX pattern FLAGS flags' */
			{
				/*
				 * 'expr' 是返回序列的表达式。'pattern' 是
				 * 正则字符串字面量。SQL/JSON 标准要求 XQuery
				 * 正则，但此处用 Postgres 正则。'flags' 是在
				 * 编译期转成整数标志的字符串字面量。
				 */
				JsonLikeRegexContext lrcxt = {0};

				jspInitByBuffer(&larg, jsp->base,
								jsp->content.like_regex.expr);

				return executePredicate(cxt, jsp, &larg, NULL, jb, false,
										executeLikeRegex, &lrcxt);
			}

		case jpiExists:
			jspGetArg(jsp, &larg);

			if (jspStrictAbsenseOfErrors(cxt))
			{
				/*
				 * 严格模式下必须取得完整值列表，以
				 * 检查完全没有错误。
				 */
				JsonValueList vals = {0};
				JsonPathExecResult res =
					executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
													  false, &vals);

				if (jperIsError(res))
					return jpbUnknown;

				return JsonValueListIsEmpty(&vals) ? jpbFalse : jpbTrue;
			}
			else
			{
				JsonPathExecResult res =
					executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
													  false, NULL);

				if (jperIsError(res))
					return jpbUnknown;

				return res == jperOk ? jpbTrue : jpbFalse;
			}

		default:
			elog(ERROR, "invalid boolean jsonpath item type: %d", jsp->type);
			return jpbUnknown;
	}
}

/*
 * 执行嵌套（过滤器等）布尔表达式，把当前 SQL/JSON
 * 项压入栈。
 */
static JsonPathBool
executeNestedBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb)
{
	JsonbValue *prev;
	JsonPathBool res;

	prev = cxt->current;
	cxt->current = jb;
	res = executeBoolItem(cxt, jsp, jb, false);
	cxt->current = prev;

	return res;
}

/*
 * 若干 jsonpath 节点的实现：
 *  - jpiAny (.** 访问器)
 *  - jpiAnyKey (.* 访问器)
 *  - jpiAnyArray ([*]访问器)
 */
static JsonPathExecResult
executeAnyItem(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbContainer *jbc,
			   JsonValueList *found, uint32 level, uint32 first, uint32 last,
			   bool ignoreStructuralErrors, bool unwrapNext)
{
	JsonPathExecResult res = jperNotFound;
	JsonbIterator *it;
	int32		r;
	JsonbValue	v;

	check_stack_depth();

	if (level > last)
		return res;

	it = JsonbIteratorInit(jbc);

	/*
	 * 递归遍历 jsonb 对象/数组
	 */
	while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (r == WJB_KEY)
		{
			r = JsonbIteratorNext(&it, &v, true);
			Assert(r == WJB_VALUE);
		}

		if (r == WJB_VALUE || r == WJB_ELEM)
		{

			if (level >= first ||
				(first == PG_UINT32_MAX && last == PG_UINT32_MAX &&
				 v.type != jbvBinary))	/* 仅留所请求的 */
			{
				/* 检查表达式 */
				if (jsp)
				{
					if (ignoreStructuralErrors)
					{
						bool		savedIgnoreStructuralErrors;

						savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
						cxt->ignoreStructuralErrors = true;
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);
						cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;
					}
					else
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}
				else if (found)
					JsonValueListAppend(found, copyJsonbValue(&v));
				else
					return jperOk;
			}

			if (level < last && v.type == jbvBinary)
			{
				res = executeAnyItem
					(cxt, jsp, v.val.binary.data, found,
					 level + 1, first, last,
					 ignoreStructuralErrors, unwrapNext);

				if (jperIsError(res))
					break;

				if (res == jperOk && found == NULL)
					break;
			}
		}
	}

	return res;
}

/*
 * 执行一元或二元谓词。
 *
 * 谓词具有存在语义，因其操作数是项
 * 序列。检查左右操作数序列中的项对。
 * 仅当找到任一满足条件的对时才返回 TRUE。
 * 严格模式下，即使已找到目标对，仍须检查
 * 全部对以确认无错误。若发生任何错误，
 * 则返回 UNKNOWN（类似 SQL NULL）。
 */
static JsonPathBool
executePredicate(JsonPathExecContext *cxt, JsonPathItem *pred,
				 JsonPathItem *larg, JsonPathItem *rarg, JsonbValue *jb,
				 bool unwrapRightArg, JsonPathPredicateCallback exec,
				 void *param)
{
	JsonPathExecResult res;
	JsonValueListIterator lseqit;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonbValue *lval;
	bool		error = false;
	bool		found = false;

	/* 左参数总是自动解包。 */
	res = executeItemOptUnwrapResultNoThrow(cxt, larg, jb, true, &lseq);
	if (jperIsError(res))
		return jpbUnknown;

	if (rarg)
	{
		/* 右参数按条件自动解包。 */
		res = executeItemOptUnwrapResultNoThrow(cxt, rarg, jb,
												unwrapRightArg, &rseq);
		if (jperIsError(res))
			return jpbUnknown;
	}

	JsonValueListInitIterator(&lseq, &lseqit);
	while ((lval = JsonValueListNext(&lseq, &lseqit)))
	{
		JsonValueListIterator rseqit;
		JsonbValue *rval;
		bool		first = true;

		JsonValueListInitIterator(&rseq, &rseqit);
		if (rarg)
			rval = JsonValueListNext(&rseq, &rseqit);
		else
			rval = NULL;

		/* 遍历右参序列，否则单次遍历 */
		while (rarg ? (rval != NULL) : first)
		{
			JsonPathBool res = exec(pred, lval, rval, param);

			if (res == jpbUnknown)
			{
				if (jspStrictAbsenseOfErrors(cxt))
					return jpbUnknown;

				error = true;
			}
			else if (res == jpbTrue)
			{
				if (!jspStrictAbsenseOfErrors(cxt))
					return jpbTrue;

				found = true;
			}

			first = false;
			if (rarg)
				rval = JsonValueListNext(&rseq, &rseqit);
		}
	}

	if (found)					/* 仅严格模式可能 */
		return jpbTrue;

	if (error)					/* 仅宽松模式可能 */
		return jpbUnknown;

	return jpbFalse;
}

/*
 * 在单例数值操作数上执行二元算术表达式。
 * 宽松模式下数组操作数自动解包。
 */
static JsonPathExecResult
executeBinaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
						JsonbValue *jb, BinaryArithmFunc func,
						JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathItem elem;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonbValue *lval;
	JsonbValue *rval;
	Numeric		res;

	jspGetLeftArg(jsp, &elem);

	/*
	 * XXX: 按标准仅乘法表达式的操作数会
	 * 解包。我们把此扩展到其他二元算术表达式。
	 */
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &lseq);
	if (jperIsError(jper))
		return jper;

	jspGetRightArg(jsp, &elem);

	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &rseq);
	if (jperIsError(jper))
		return jper;

	if (JsonValueListLength(&lseq) != 1 ||
		!(lval = getScalar(JsonValueListHead(&lseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
							  errmsg("left operand of jsonpath operator %s is not a single numeric value",
									 jspOperationName(jsp->type)))));

	if (JsonValueListLength(&rseq) != 1 ||
		!(rval = getScalar(JsonValueListHead(&rseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_SQL_JSON_ITEM_REQUIRED),
							  errmsg("right operand of jsonpath operator %s is not a single numeric value",
									 jspOperationName(jsp->type)))));

	if (jspThrowErrors(cxt))
	{
		res = func(lval->val.numeric, rval->val.numeric, NULL);
	}
	else
	{
		bool		error = false;

		res = func(lval->val.numeric, rval->val.numeric, &error);

		if (error)
			return jperError;
	}

	if (!jspGetNext(jsp, &elem) && !found)
		return jperOk;

	lval = palloc(sizeof(*lval));
	lval->type = jbvNumeric;
	lval->val.numeric = res;

	return executeNextItem(cxt, jsp, &elem, lval, found, false);
}

/*
 * 对操作数序列中的每个数值项执行一元算术
 * 表达式。宽松模式下数组操作数自动解包。
 */
static JsonPathExecResult
executeUnaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
					   JsonbValue *jb, PGFunction func, JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathExecResult jper2;
	JsonPathItem elem;
	JsonValueList seq = {0};
	JsonValueListIterator it;
	JsonbValue *val;
	bool		hasNext;

	jspGetArg(jsp, &elem);
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &seq);

	if (jperIsError(jper))
		return jper;

	jper = jperNotFound;

	hasNext = jspGetNext(jsp, &elem);

	JsonValueListInitIterator(&seq, &it);
	while ((val = JsonValueListNext(&seq, &it)))
	{
		if ((val = getScalar(val, jbvNumeric)))
		{
			if (!found && !hasNext)
				return jperOk;
		}
		else
		{
			if (!found && !hasNext)
				continue;		/* 跳过非数值处理 */

			RETURN_ERROR(ereport(ERROR,
								 (errcode(ERRCODE_SQL_JSON_NUMBER_NOT_FOUND),
								  errmsg("operand of unary jsonpath operator %s is not a numeric value",
										 jspOperationName(jsp->type)))));
		}

		if (func)
			val->val.numeric =
				DatumGetNumeric(DirectFunctionCall1(func,
													NumericGetDatum(val->val.numeric)));

		jper2 = executeNextItem(cxt, jsp, &elem, val, found, false);

		if (jperIsError(jper2))
			return jper2;

		if (jper2 == jperOk)
		{
			if (!found)
				return jperOk;
			jper = jperOk;
		}
	}

	return jper;
}

/*
 * STARTS_WITH 谓词回调。
 *
 * 检查 'whole' 字符串是否以 'initial' 开头。
 */
static JsonPathBool
executeStartsWith(JsonPathItem *jsp, JsonbValue *whole, JsonbValue *initial,
				  void *param)
{
	if (!(whole = getScalar(whole, jbvString)))
		return jpbUnknown;		/* error */

	if (!(initial = getScalar(initial, jbvString)))
		return jpbUnknown;		/* error */

	if (whole->val.string.len >= initial->val.string.len &&
		!memcmp(whole->val.string.val,
				initial->val.string.val,
				initial->val.string.len))
		return jpbTrue;

	return jpbFalse;
}

/*
 * LIKE_REGEX 谓词回调。
 *
 * 检查字符串是否匹配正则模式。
 */
static JsonPathBool
executeLikeRegex(JsonPathItem *jsp, JsonbValue *str, JsonbValue *rarg,
				 void *param)
{
	JsonLikeRegexContext *cxt = param;

	if (!(str = getScalar(str, jbvString)))
		return jpbUnknown;

	/* 缓存正则文本与已转标志。 */
	if (!cxt->regex)
	{
		cxt->regex =
			cstring_to_text_with_len(jsp->content.like_regex.pattern,
									 jsp->content.like_regex.patternlen);
		(void) jspConvertRegexFlags(jsp->content.like_regex.flags,
									&(cxt->cflags), NULL);
	}

	if (RE_compile_and_execute(cxt->regex, str->val.string.val,
							   str->val.string.len,
							   cxt->cflags, DEFAULT_COLLATION_OID, 0, NULL))
		return jpbTrue;

	return jpbFalse;
}

/*
 * 用用户函数 func 执行数值项方法
 * .abs/.floor/.ceil。
 */
static JsonPathExecResult
executeNumericItemMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
						 JsonbValue *jb, bool unwrap, PGFunction func,
						 JsonValueList *found)
{
	JsonPathItem next;
	Datum		datum;

	if (unwrap && JsonbType(jb) == jbvArray)
		return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

	if (!(jb = getScalar(jb, jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_NON_NUMERIC_SQL_JSON_ITEM),
							  errmsg("jsonpath item method .%s() can only be applied to a numeric value",
									 jspOperationName(jsp->type)))));

	datum = DirectFunctionCall1(func, NumericGetDatum(jb->val.numeric));

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;

	jb = palloc(sizeof(*jb));
	jb->type = jbvNumeric;
	jb->val.numeric = DatumGetNumeric(datum);

	return executeNextItem(cxt, jsp, &next, jb, found, false);
}

/*
 * .datetime() 方法的实现。
 *
 * 把字符串转为日期/时间值。实际类型在运行时确定。
 * 若提供参数，则该参数用作模板字符串。
 * 否则选用首个合适的 ISO 格式。
 */
static JsonPathExecResult
executeDateTimeMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb, JsonValueList *found)
{
	JsonbValue	jbvbuf;
	Datum		value;
	text	   *datetime;
	Oid			collid;
	Oid			typid;
	int32		typmod = -1;
	int			tz = 0;
	bool		hasNext;
	JsonPathExecResult res = jperNotFound;
	JsonPathItem elem;

	if (!(jb = getScalar(jb, jbvString)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
							  errmsg("jsonpath item method .%s() can only be applied to a string",
									 jspOperationName(jsp->type)))));

	datetime = cstring_to_text_with_len(jb->val.string.val,
										jb->val.string.len);

	/*
	 * 将来或许希望由调用方提供排序规则，
	 * 但目前不清楚他们能否优于
	 * DEFAULT_COLLATION_OID。
	 */
	collid = DEFAULT_COLLATION_OID;

	if (jsp->content.arg)
	{
		text	   *template;
		char	   *template_str;
		int			template_len;
		ErrorSaveContext escontext = {T_ErrorSaveContext};

		jspGetArg(jsp, &elem);

		if (elem.type != jpiString)
			elog(ERROR, "invalid jsonpath item type for .datetime() argument");

		template_str = jspGetString(&elem, &template_len);

		template = cstring_to_text_with_len(template_str,
											template_len);

		value = parse_datetime(datetime, template, collid, true,
							   &typid, &typmod, &tz,
							   jspThrowErrors(cxt) ? NULL : (Node *) &escontext);

		if (escontext.error_occurred)
			res = jperError;
		else
			res = jperOk;
	}
	else
	{
		/*
		 * 按 SQL/JSON 标准枚举 ISO 格式：date、
		 * timetz/time/timestamptz/timestamp。
		 *
		 * 我们也支持时间戳的 ISO 8601 格式（带 "T"），因
		 * to_json[b]() 使用该格式。
		 */
		static const char *fmt_str[] =
		{
			"yyyy-mm-dd",		/* date */
			"HH24:MI:SS.USTZH:TZM", /* timetz */
			"HH24:MI:SS.USTZH",
			"HH24:MI:SSTZH:TZM",
			"HH24:MI:SSTZH",
			"HH24:MI:SS.US",	/* time without tz */
			"HH24:MI:SS",
			"yyyy-mm-dd HH24:MI:SS.USTZH:TZM",	/* timestamptz */
			"yyyy-mm-dd HH24:MI:SS.USTZH",
			"yyyy-mm-dd HH24:MI:SSTZH:TZM",
			"yyyy-mm-dd HH24:MI:SSTZH",
			"yyyy-mm-dd\"T\"HH24:MI:SS.USTZH:TZM",
			"yyyy-mm-dd\"T\"HH24:MI:SS.USTZH",
			"yyyy-mm-dd\"T\"HH24:MI:SSTZH:TZM",
			"yyyy-mm-dd\"T\"HH24:MI:SSTZH",
			"yyyy-mm-dd HH24:MI:SS.US", /* timestamp without tz */
			"yyyy-mm-dd HH24:MI:SS",
			"yyyy-mm-dd\"T\"HH24:MI:SS.US",
			"yyyy-mm-dd\"T\"HH24:MI:SS"
		};

		/* 格式文本缓存 */
		static text *fmt_txt[lengthof(fmt_str)] = {0};
		int			i;

		/* 循环至格式合适 */
		for (i = 0; i < lengthof(fmt_str); i++)
		{
			ErrorSaveContext escontext = {T_ErrorSaveContext};

			if (!fmt_txt[i])
			{
				MemoryContext oldcxt =
					MemoryContextSwitchTo(TopMemoryContext);

				fmt_txt[i] = cstring_to_text(fmt_str[i]);
				MemoryContextSwitchTo(oldcxt);
			}

			value = parse_datetime(datetime, fmt_txt[i], collid, true,
								   &typid, &typmod, &tz,
								   (Node *) &escontext);

			if (!escontext.error_occurred)
			{
				res = jperOk;
				break;
			}
		}

		if (res == jperNotFound)
			RETURN_ERROR(ereport(ERROR,
								 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_SQL_JSON_DATETIME_FUNCTION),
								  errmsg("datetime format is not recognized: \"%s\"",
										 text_to_cstring(datetime)),
								  errhint("Use a datetime template argument to specify the input data format."))));
	}

	pfree(datetime);

	if (jperIsError(res))
		return res;

	hasNext = jspGetNext(jsp, &elem);

	if (!hasNext && !found)
		return res;

	jb = hasNext ? &jbvbuf : palloc(sizeof(*jb));

	jb->type = jbvDatetime;
	jb->val.datetime.value = value;
	jb->val.datetime.typid = typid;
	jb->val.datetime.typmod = typmod;
	jb->val.datetime.tz = tz;

	return executeNextItem(cxt, jsp, &elem, jb, found, hasNext);
}

/*
 * .keyvalue() 方法的实现。
 *
 * .keyvalue() 方法返回对象键值对序列，格式为
 * '{ "key": key, "value": value, "id": id }'。
 *
 * "id" 字段是对象标识符，由两部分构成：
 * 基对象 id 及其在基对象 jsonb 内的偏移：
 * id = 10000000000 * base_object_id + obj_offset_in_base_object
 *
 * 10000000000 (10^10) -- 大于 2^32（jsonb 最大偏移）
 * 的首个整十进制数。用十进制乘数以改善
 * 标识符可读性。
 *
 * 基对象通常是路径的根对象：上下文项 '$' 或路径
 * 变量 '$var'；字面量目前不能产生对象。但若路径
 * 含生成对象（例如 .keyvalue() 自身），则它们
 * 成为后续 .keyvalue() 的基对象。
 *
 * '$' 的 id 为 0。'$var' 的 id 是变量列表中的正序
 * 号（见 getJsonPathVariable()）。生成对象的 id
 * 由全局计数器 JsonPathExecContext.lastGeneratedObjectId 分配。
 */
static JsonPathExecResult
executeKeyValueMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonbValue *jb, JsonValueList *found)
{
	JsonPathExecResult res = jperNotFound;
	JsonPathItem next;
	JsonbContainer *jbc;
	JsonbValue	key;
	JsonbValue	val;
	JsonbValue	idval;
	JsonbValue	keystr;
	JsonbValue	valstr;
	JsonbValue	idstr;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	int64		id;
	bool		hasNext;

	if (JsonbType(jb) != jbvObject || jb->type != jbvBinary)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SQL_JSON_OBJECT_NOT_FOUND),
							  errmsg("jsonpath item method .%s() can only be applied to an object",
									 jspOperationName(jsp->type)))));

	jbc = jb->val.binary.data;

	if (!JsonContainerSize(jbc))
		return jperNotFound;	/* 无键值对 */

	hasNext = jspGetNext(jsp, &next);

	keystr.type = jbvString;
	keystr.val.string.val = "key";
	keystr.val.string.len = 3;

	valstr.type = jbvString;
	valstr.val.string.val = "value";
	valstr.val.string.len = 5;

	idstr.type = jbvString;
	idstr.val.string.val = "id";
	idstr.val.string.len = 2;

	/* 由基对象与内部偏移构造对象 id */
	id = jb->type != jbvBinary ? 0 :
		(int64) ((char *) jbc - (char *) cxt->baseObject.jbc);
	id += (int64) cxt->baseObject.id * INT64CONST(10000000000);

	idval.type = jbvNumeric;
	idval.val.numeric = int64_to_numeric(id);

	it = JsonbIteratorInit(jbc);

	while ((tok = JsonbIteratorNext(&it, &key, true)) != WJB_DONE)
	{
		JsonBaseObjectInfo baseObject;
		JsonbValue	obj;
		JsonbParseState *ps;
		JsonbValue *keyval;
		Jsonb	   *jsonb;

		if (tok != WJB_KEY)
			continue;

		res = jperOk;

		if (!hasNext && !found)
			break;

		tok = JsonbIteratorNext(&it, &val, true);
		Assert(tok == WJB_VALUE);

		ps = NULL;
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);

		pushJsonbValue(&ps, WJB_KEY, &keystr);
		pushJsonbValue(&ps, WJB_VALUE, &key);

		pushJsonbValue(&ps, WJB_KEY, &valstr);
		pushJsonbValue(&ps, WJB_VALUE, &val);

		pushJsonbValue(&ps, WJB_KEY, &idstr);
		pushJsonbValue(&ps, WJB_VALUE, &idval);

		keyval = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);

		jsonb = JsonbValueToJsonb(keyval);

		JsonbInitBinary(&obj, jsonb);

		baseObject = setBaseObject(cxt, &obj, cxt->lastGeneratedObjectId++);

		res = executeNextItem(cxt, jsp, &next, &obj, found, true);

		cxt->baseObject = baseObject;

		if (jperIsError(res))
			return res;

		if (res == jperOk && !found)
			break;
	}

	return res;
}

/*
 * 把布尔状态 'res' 转为布尔 JSON 项并执行下一
 * jsonpath。
 */
static JsonPathExecResult
appendBoolResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
				 JsonValueList *found, JsonPathBool res)
{
	JsonPathItem next;
	JsonbValue	jbv;

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;			/* 找到单例布尔值 */

	if (res == jpbUnknown)
	{
		jbv.type = jbvNull;
	}
	else
	{
		jbv.type = jbvBool;
		jbv.val.boolean = res == jpbTrue;
	}

	return executeNextItem(cxt, jsp, &next, &jbv, found, true);
}

/*
 * 把 jsonpath 的标量或变量节点转为实际 jsonb 值。
 *
 * 若节点是变量则返回其 id，否则返回 0。
 */
static void
getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
				JsonbValue *value)
{
	switch (item->type)
	{
		case jpiNull:
			value->type = jbvNull;
			break;
		case jpiBool:
			value->type = jbvBool;
			value->val.boolean = jspGetBool(item);
			break;
		case jpiNumeric:
			value->type = jbvNumeric;
			value->val.numeric = jspGetNumeric(item);
			break;
		case jpiString:
			value->type = jbvString;
			value->val.string.val = jspGetString(item,
												 &value->val.string.len);
			break;
		case jpiVariable:
			getJsonPathVariable(cxt, item, cxt->vars, value);
			return;
		default:
			elog(ERROR, "unexpected jsonpath item type");
	}
}

/*
 * 获取传给 jsonpath 执行器的变量值
 */
static void
getJsonPathVariable(JsonPathExecContext *cxt, JsonPathItem *variable,
					Jsonb *vars, JsonbValue *value)
{
	char	   *varName;
	int			varNameLength;
	JsonbValue	tmp;
	JsonbValue *v;

	if (!vars)
	{
		value->type = jbvNull;
		return;
	}

	Assert(variable->type == jpiVariable);
	varName = jspGetString(variable, &varNameLength);
	tmp.type = jbvString;
	tmp.val.string.val = varName;
	tmp.val.string.len = varNameLength;

	v = findJsonbValueFromContainer(&vars->root, JB_FOBJECT, &tmp);

	if (v)
	{
		*value = *v;
		pfree(v);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find jsonpath variable \"%s\"",
						pnstrdup(varName, varNameLength))));
	}

	JsonbInitBinary(&tmp, vars);
	setBaseObject(cxt, &tmp, 1);
}

/**************** Support functions for JsonPath execution *****************/

/*
 * 返回数组项的大小；若不是数组则返回 -1。
 */
static int
JsonbArraySize(JsonbValue *jb)
{
	Assert(jb->type != jbvArray);

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = jb->val.binary.data;

		if (JsonContainerIsArray(jbc) && !JsonContainerIsScalar(jbc))
			return JsonContainerSize(jbc);
	}

	return -1;
}

/* 比较谓词回调。 */
static JsonPathBool
executeComparison(JsonPathItem *cmp, JsonbValue *lv, JsonbValue *rv, void *p)
{
	JsonPathExecContext *cxt = (JsonPathExecContext *) p;

	return compareItems(cmp->type, lv, rv, cxt->useTz);
}

/*
 * 对两个字符串做逐字节比较。
 */
static int
binaryCompareStrings(const char *s1, int len1,
					 const char *s2, int len2)
{
	int			cmp;

	cmp = memcmp(s1, s2, Min(len1, len2));

	if (cmp != 0)
		return cmp;

	if (len1 == len2)
		return 0;

	return len1 < len2 ? -1 : 1;
}

/*
 * 在当前服务器编码下用 Unicode 码点排序规则
 * 比两串
 */
static int
compareStrings(const char *mbstr1, int mblen1,
			   const char *mbstr2, int mblen2)
{
	if (GetDatabaseEncoding() == PG_SQL_ASCII ||
		GetDatabaseEncoding() == PG_UTF8)
	{
		/*
		 * UTF-8 字符串有一已知性质：其逐字节比较结果
		 * 与码点比较结果一致。ASCII 可视为
		 * UTF-8 的特例。
		 */
		return binaryCompareStrings(mbstr1, mblen1, mbstr2, mblen2);
	}
	else
	{
		char	   *utf8str1,
				   *utf8str2;
		int			cmp,
					utf8len1,
					utf8len2;

		/*
		 * 其他编码须先转为 UTF-8 再比较。
		 * 输入串可能非空终止，且 pg_server_to_any() 可能
		 * 原样返回。故仅真正转换时才用
		 * strlen()。
		 */
		utf8str1 = pg_server_to_any(mbstr1, mblen1, PG_UTF8);
		utf8str2 = pg_server_to_any(mbstr2, mblen2, PG_UTF8);
		utf8len1 = (mbstr1 == utf8str1) ? mblen1 : strlen(utf8str1);
		utf8len2 = (mbstr2 == utf8str2) ? mblen2 : strlen(utf8str2);

		cmp = binaryCompareStrings(utf8str1, utf8len1, utf8str2, utf8len2);

		/*
		 * 若 pg_server_to_any() 未真正转换，则实际比较的是
		 * 原始字符串，已经完成。
		 */
		if (mbstr1 == utf8str1 && mbstr2 == utf8str2)
			return cmp;

		/* 需则释放内存 */
		if (mbstr1 != utf8str1)
			pfree(utf8str1);
		if (mbstr2 != utf8str2)
			pfree(utf8str2);

		/*
		 * 当全部 Unicode 码点相等时，返回二进制比较
		 * 结果。某些边界情况下，同一字符在编码中可能有
		 * 不同表示，此时行为可能偏离
		 * 标准。但这使我们能对 "==" 运算符做简单二进制
		 * 比较，这在典型情形下对性能至关重要。
		 * 将来若要严格符合标准，可对输入
		 * JSON 字符串做规范化。
		 */
		if (cmp == 0)
			return binaryCompareStrings(mbstr1, mblen1, mbstr2, mblen2);
		else
			return cmp;
	}
}

/*
 * 用比较运算 'op' 比较两个 SQL/JSON 项。
 */
static JsonPathBool
compareItems(int32 op, JsonbValue *jb1, JsonbValue *jb2, bool useTz)
{
	int			cmp;
	bool		res;

	if (jb1->type != jb2->type)
	{
		if (jb1->type == jbvNull || jb2->type == jbvNull)

			/*
			 * null 与非 null 的相等与次序比较总是
			 * 返回 false，但不等比较返回 true。
			 */
			return op == jpiNotEqual ? jpbTrue : jpbFalse;

		/* 不同类型非 null 项不可比较。 */
		return jpbUnknown;
	}

	switch (jb1->type)
	{
		case jbvNull:
			cmp = 0;
			break;
		case jbvBool:
			cmp = jb1->val.boolean == jb2->val.boolean ? 0 :
				jb1->val.boolean ? 1 : -1;
			break;
		case jbvNumeric:
			cmp = compareNumeric(jb1->val.numeric, jb2->val.numeric);
			break;
		case jbvString:
			if (op == jpiEqual)
				return jb1->val.string.len != jb2->val.string.len ||
					memcmp(jb1->val.string.val,
						   jb2->val.string.val,
						   jb1->val.string.len) ? jpbFalse : jpbTrue;

			cmp = compareStrings(jb1->val.string.val, jb1->val.string.len,
								 jb2->val.string.val, jb2->val.string.len);
			break;
		case jbvDatetime:
			{
				bool		cast_error;

				cmp = compareDatetime(jb1->val.datetime.value,
									  jb1->val.datetime.typid,
									  jb2->val.datetime.value,
									  jb2->val.datetime.typid,
									  useTz,
									  &cast_error);

				if (cast_error)
					return jpbUnknown;
			}
			break;

		case jbvBinary:
		case jbvArray:
		case jbvObject:
			return jpbUnknown;	/* 非标量不可比较 */

		default:
			elog(ERROR, "invalid jsonb value type %d", jb1->type);
	}

	switch (op)
	{
		case jpiEqual:
			res = (cmp == 0);
			break;
		case jpiNotEqual:
			res = (cmp != 0);
			break;
		case jpiLess:
			res = (cmp < 0);
			break;
		case jpiGreater:
			res = (cmp > 0);
			break;
		case jpiLessOrEqual:
			res = (cmp <= 0);
			break;
		case jpiGreaterOrEqual:
			res = (cmp >= 0);
			break;
		default:
			elog(ERROR, "unrecognized jsonpath operation: %d", op);
			return jpbUnknown;
	}

	return res ? jpbTrue : jpbFalse;
}

/* 比较两 numeric */
static int
compareNumeric(Numeric a, Numeric b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
											 NumericGetDatum(a),
											 NumericGetDatum(b)));
}

static JsonbValue *
copyJsonbValue(JsonbValue *src)
{
	JsonbValue *dst = palloc(sizeof(*dst));

	*dst = *src;

	return dst;
}

/*
 * 执行数组下标表达式，并把所得数值项截断
 * 转换为整数类型。
 */
static JsonPathExecResult
getArrayIndex(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbValue *jb,
			  int32 *index)
{
	JsonbValue *jbv;
	JsonValueList found = {0};
	JsonPathExecResult res = executeItem(cxt, jsp, jb, &found);
	Datum		numeric_index;
	bool		have_error = false;

	if (jperIsError(res))
		return res;

	if (JsonValueListLength(&found) != 1 ||
		!(jbv = getScalar(JsonValueListHead(&found), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
							  errmsg("jsonpath array subscript is not a single numeric value"))));

	numeric_index = DirectFunctionCall2(numeric_trunc,
										NumericGetDatum(jbv->val.numeric),
										Int32GetDatum(0));

	*index = numeric_int4_opt_error(DatumGetNumeric(numeric_index),
									&have_error);

	if (have_error)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_SQL_JSON_SUBSCRIPT),
							  errmsg("jsonpath array subscript is out of integer range"))));

	return jperOk;
}

/* 保存 .keyvalue() 所需基对象及其 id。 */
static JsonBaseObjectInfo
setBaseObject(JsonPathExecContext *cxt, JsonbValue *jbv, int32 id)
{
	JsonBaseObjectInfo baseObject = cxt->baseObject;

	cxt->baseObject.jbc = jbv->type != jbvBinary ? NULL :
		(JsonbContainer *) jbv->val.binary.data;
	cxt->baseObject.id = id;

	return baseObject;
}

static void
JsonValueListAppend(JsonValueList *jvl, JsonbValue *jbv)
{
	if (jvl->singleton)
	{
		jvl->list = list_make2(jvl->singleton, jbv);
		jvl->singleton = NULL;
	}
	else if (!jvl->list)
		jvl->singleton = jbv;
	else
		jvl->list = lappend(jvl->list, jbv);
}

static int
JsonValueListLength(const JsonValueList *jvl)
{
	return jvl->singleton ? 1 : list_length(jvl->list);
}

static bool
JsonValueListIsEmpty(JsonValueList *jvl)
{
	return !jvl->singleton && (jvl->list == NIL);
}

static JsonbValue *
JsonValueListHead(JsonValueList *jvl)
{
	return jvl->singleton ? jvl->singleton : linitial(jvl->list);
}

static List *
JsonValueListGetList(JsonValueList *jvl)
{
	if (jvl->singleton)
		return list_make1(jvl->singleton);

	return jvl->list;
}

static void
JsonValueListInitIterator(const JsonValueList *jvl, JsonValueListIterator *it)
{
	if (jvl->singleton)
	{
		it->value = jvl->singleton;
		it->list = NIL;
		it->next = NULL;
	}
	else if (jvl->list != NIL)
	{
		it->value = (JsonbValue *) linitial(jvl->list);
		it->list = jvl->list;
		it->next = list_second_cell(jvl->list);
	}
	else
	{
		it->value = NULL;
		it->list = NIL;
		it->next = NULL;
	}
}

/*
 * 推进迭代器，从序列取下一项。
 */
static JsonbValue *
JsonValueListNext(const JsonValueList *jvl, JsonValueListIterator *it)
{
	JsonbValue *result = it->value;

	if (it->next)
	{
		it->value = lfirst(it->next);
		it->next = lnext(it->list, it->next);
	}
	else
	{
		it->value = NULL;
	}

	return result;
}

/*
 * 用给定 jsonb 容器初始化二进制 JsonbValue。
 */
static JsonbValue *
JsonbInitBinary(JsonbValue *jbv, Jsonb *jb)
{
	jbv->type = jbvBinary;
	jbv->val.binary.data = &jb->root;
	jbv->val.binary.len = VARSIZE_ANY_EXHDR(jb);

	return jbv;
}

/*
 * 返回 JsonbValue 的 jbv* 类型；从不以原样返回 jbvBinary。
 */
static int
JsonbType(JsonbValue *jb)
{
	int			type = jb->type;

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = (void *) jb->val.binary.data;

		/* 标量在 jsonpath 执行中总应被提取。 */
		Assert(!JsonContainerIsScalar(jbc));

		if (JsonContainerIsObject(jbc))
			type = jbvObject;
		else if (JsonContainerIsArray(jbc))
			type = jbvArray;
		else
			elog(ERROR, "invalid jsonb container type: 0x%08x", jbc->header);
	}

	return type;
}

/* 取给定类型标量，类型不符则 NULL */
static JsonbValue *
getScalar(JsonbValue *scalar, enum jbvType type)
{
	/* 标量在 jsonpath 执行中总应被提取。 */
	Assert(scalar->type != jbvBinary ||
		   !JsonContainerIsScalar(scalar->val.binary.data));

	return scalar->type == type ? scalar : NULL;
}

/* 由项列表构造 JSON 数组 */
static JsonbValue *
wrapItemsInArray(const JsonValueList *items)
{
	JsonbParseState *ps = NULL;
	JsonValueListIterator it;
	JsonbValue *jbv;

	pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);

	JsonValueListInitIterator(items, &it);
	while ((jbv = JsonValueListNext(items, &it)))
		pushJsonbValue(&ps, WJB_ELEM, jbv);

	return pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
}

/* 检查 type1→type2 转换所需时区是否在用 */
static void
checkTimezoneIsUsedForCast(bool useTz, const char *type1, const char *type2)
{
	if (!useTz)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot convert value from %s to %s without time zone usage",
						type1, type2),
				 errhint("Use *_tz() function for time zone support.")));
}

/* 把 time 转为 timetz */
static Datum
castTimeToTimeTz(Datum time, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "time", "timetz");

	return DirectFunctionCall1(time_timetz, time);
}

/*
 * 比较date与timestamp。
 * 注意这不涉及任何时区。
 */
static int
cmpDateToTimestamp(DateADT date1, Timestamp ts2, bool useTz)
{
	return date_cmp_timestamp_internal(date1, ts2);
}

/*
 * 比较date与timestamptz。
 */
static int
cmpDateToTimestampTz(DateADT date1, TimestampTz tstz2, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "date", "timestamptz");

	return date_cmp_timestamptz_internal(date1, tstz2);
}

/*
 * 比较timestamp与timestamptz。
 */
static int
cmpTimestampToTimestampTz(Timestamp ts1, TimestampTz tstz2, bool useTz)
{
	checkTimezoneIsUsedForCast(useTz, "timestamp", "timestamptz");

	return timestamp_cmp_timestamptz_internal(ts1, tstz2);
}

/*
 * 两个日期时间 SQL/JSON 项的跨类型比较。若项
 * 不可比则置 *cast_error，否则清除 *cast_error。
 * 若转换需时区但未用，则显式抛错。
 */
static int
compareDatetime(Datum val1, Oid typid1, Datum val2, Oid typid2,
				bool useTz, bool *cast_error)
{
	PGFunction	cmpfunc;

	*cast_error = false;

	switch (typid1)
	{
		case DATEOID:
			switch (typid2)
			{
				case DATEOID:
					cmpfunc = date_cmp;

					break;

				case TIMESTAMPOID:
					return cmpDateToTimestamp(DatumGetDateADT(val1),
											  DatumGetTimestamp(val2),
											  useTz);

				case TIMESTAMPTZOID:
					return cmpDateToTimestampTz(DatumGetDateADT(val1),
												DatumGetTimestampTz(val2),
												useTz);

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* 不可比较类型 */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMEOID:
			switch (typid2)
			{
				case TIMEOID:
					cmpfunc = time_cmp;

					break;

				case TIMETZOID:
					val1 = castTimeToTimeTz(val1, useTz);
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*cast_error = true; /* 不可比较类型 */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMETZOID:
			switch (typid2)
			{
				case TIMEOID:
					val2 = castTimeToTimeTz(val2, useTz);
					cmpfunc = timetz_cmp;

					break;

				case TIMETZOID:
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*cast_error = true; /* 不可比较类型 */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMESTAMPOID:
			switch (typid2)
			{
				case DATEOID:
					return -cmpDateToTimestamp(DatumGetDateADT(val2),
											   DatumGetTimestamp(val1),
											   useTz);

				case TIMESTAMPOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPTZOID:
					return cmpTimestampToTimestampTz(DatumGetTimestamp(val1),
													 DatumGetTimestampTz(val2),
													 useTz);

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* 不可比较类型 */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		case TIMESTAMPTZOID:
			switch (typid2)
			{
				case DATEOID:
					return -cmpDateToTimestampTz(DatumGetDateADT(val2),
												 DatumGetTimestampTz(val1),
												 useTz);

				case TIMESTAMPOID:
					return -cmpTimestampToTimestampTz(DatumGetTimestamp(val2),
													  DatumGetTimestampTz(val1),
													  useTz);

				case TIMESTAMPTZOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMEOID:
				case TIMETZOID:
					*cast_error = true; /* 不可比较类型 */
					return 0;

				default:
					elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u",
						 typid2);
			}
			break;

		default:
			elog(ERROR, "unrecognized SQL/JSON datetime type oid: %u", typid1);
	}

	if (*cast_error)
		return 0;				/* 转换错 */

	return DatumGetInt32(DirectFunctionCall2(cmpfunc, val1, val2));
}
