/*-------------------------------------------------------------------------
 *
 * bool.c
 *	  内置类型 "bool" 的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/bool.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "libpq/pqformat.h"
#include "utils/builtins.h"

/*
 * 尝试将 value 解释为布尔值。有效值有：true、
 * false、yes、no、on、off、1、0 或其唯一前缀。
 * 串解析成功返回 true，否则 false。
 * 若成功且 result 非 NULL，则以 *result 返回。
 */
bool
parse_bool(const char *value, bool *result)
{
	return parse_bool_with_len(value, strlen(value), result);
}

bool
parse_bool_with_len(const char *value, size_t len, bool *result)
{
	switch (*value)
	{
		case 't':
		case 'T':
			if (pg_strncasecmp(value, "true", len) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case 'f':
		case 'F':
			if (pg_strncasecmp(value, "false", len) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case 'y':
		case 'Y':
			if (pg_strncasecmp(value, "yes", len) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case 'n':
		case 'N':
			if (pg_strncasecmp(value, "no", len) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case 'o':
		case 'O':
			/* 'o' 区分度不足 */
			if (pg_strncasecmp(value, "on", (len > 2 ? len : 2)) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			else if (pg_strncasecmp(value, "off", (len > 2 ? len : 2)) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case '1':
			if (len == 1)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case '0':
			if (len == 1)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		default:
			break;
	}

	if (result)
		*result = false;		/* 抑制编译器警告 */
	return false;
}

/*****************************************************************************
 *	 用户 I/O 例程               *
 *****************************************************************************/

/*
 *		boolin			- 将 "t" 或 "f" 转为 1/0
 *
 * 显式检查 "true/false" 与 TRUE/FALSE、1/0、YES/NO、ON/OFF。
 * 拒绝其他值。
 *
 * 在 switch 语句中，先检查最常用的取值。
 */
Datum
boolin(PG_FUNCTION_ARGS)
{
	const char *in_str = PG_GETARG_CSTRING(0);
	const char *str;
	size_t		len;
	bool		result;

	/*
	 * 跳过前导与尾随空白
	 */
	str = in_str;
	while (isspace((unsigned char) *str))
		str++;

	len = strlen(str);
	while (len > 0 && isspace((unsigned char) str[len - 1]))
		len--;

	if (parse_bool_with_len(str, len, &result))
		PG_RETURN_BOOL(result);

	ereturn(fcinfo->context, (Datum) 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"boolean", in_str)));
}

/*
 *		boolout			- 将 1/0 转为 "t"/"f"
 */
Datum
boolout(PG_FUNCTION_ARGS)
{
	bool		b = PG_GETARG_BOOL(0);
	char	   *result = (char *) palloc(2);

	result[0] = (b) ? 't' : 'f';
	result[1] = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		boolrecv			- 将外部二进制格式转为 bool
 *
 * 外部表示为一个字节。任何非零值都被当作
 * "true"。
 */
Datum
boolrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			ext;

	ext = pq_getmsgbyte(buf);
	PG_RETURN_BOOL(ext != 0);
}

/*
 *		boolsend			- 将 bool 转为二进制格式
 */
Datum
boolsend(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, arg1 ? 1 : 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		booltext			- bool => text 的 cast 函数
 *
 * 我们需要它，因为其行为与 boolout() 不同；
 * 该函数遵循 SQL 规范的结果（但产出为小写）。
 */
Datum
booltext(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	const char *str;

	if (arg1)
		str = "true";
	else
		str = "false";

	PG_RETURN_TEXT_P(cstring_to_text(str));
}


/*****************************************************************************
 *	 公共例程                  *
 *****************************************************************************/

Datum
booleq(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
boolne(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
boollt(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
boolgt(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
boolle(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
boolge(PG_FUNCTION_ARGS)
{
	bool		arg1 = PG_GETARG_BOOL(0);
	bool		arg2 = PG_GETARG_BOOL(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

/*
 * 布尔 AND 和 OR 聚合函数。
 */

/*
 * 实现 SQL 2003 标准 EVERY 聚合的函数。
 * 为保持一致，该聚合被命名为 bool_and。
 *
 * 注意：仅用于普通聚合模式，而非移动聚合模式。
 */
Datum
booland_statefunc(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(PG_GETARG_BOOL(0) && PG_GETARG_BOOL(1));
}

/*
 * 实现 SQL 2003 标准 ANY/SOME 聚合的函数。
 * 该聚合被命名为 bool_or，因为 ANY/SOME 有解析冲突。
 *
 * 注意：仅用于普通聚合模式，而非移动聚合模式。
 */
Datum
boolor_statefunc(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(PG_GETARG_BOOL(0) || PG_GETARG_BOOL(1));
}

typedef struct BoolAggState
{
	int64		aggcount;		/* 已聚合的非空值个数 */
	int64		aggtrue;		/* 已聚合值中为 true 的个数 */
} BoolAggState;

static BoolAggState *
makeBoolAggState(FunctionCallInfo fcinfo)
{
	BoolAggState *state;
	MemoryContext agg_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (BoolAggState *) MemoryContextAlloc(agg_context,
												sizeof(BoolAggState));
	state->aggcount = 0;
	state->aggtrue = 0;

	return state;
}

Datum
bool_accum(PG_FUNCTION_ARGS)
{
	BoolAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (BoolAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makeBoolAggState(fcinfo);

	if (!PG_ARGISNULL(1))
	{
		state->aggcount++;
		if (PG_GETARG_BOOL(1))
			state->aggtrue++;
	}

	PG_RETURN_POINTER(state);
}

Datum
bool_accum_inv(PG_FUNCTION_ARGS)
{
	BoolAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (BoolAggState *) PG_GETARG_POINTER(0);

	/* bool_accum 应已创建状态数据 */
	if (state == NULL)
		elog(ERROR, "bool_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
		state->aggcount--;
		if (PG_GETARG_BOOL(1))
			state->aggtrue--;
	}

	PG_RETURN_POINTER(state);
}

Datum
bool_alltrue(PG_FUNCTION_ARGS)
{
	BoolAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (BoolAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空值则返回 NULL */
	if (state == NULL || state->aggcount == 0)
		PG_RETURN_NULL();

	/* 全部非空值为 true 时为 true */
	PG_RETURN_BOOL(state->aggtrue == state->aggcount);
}

Datum
bool_anytrue(PG_FUNCTION_ARGS)
{
	BoolAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (BoolAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空值则返回 NULL */
	if (state == NULL || state->aggcount == 0)
		PG_RETURN_NULL();

	/* 任一非空值为 true 则 true */
	PG_RETURN_BOOL(state->aggtrue > 0);
}
