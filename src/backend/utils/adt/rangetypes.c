/*-------------------------------------------------------------------------
 *
 * rangetypes.c
 *	  范围类型的 I/O 函数、算子与支持函数。
 *
 * 范围值的存储（序列化）格式如下：
 *
 *	4 字节：varlena 头
 *	4 字节：范围 OID
 *	下界值（若有），按子类型 typalign 对齐
 *	上界值（若有），按子类型 typalign 对齐
 *	1 字节标志
 *
 * 选用此表示是为避免下界值前需要填充，即便其要求
 * 双对齐。可期望 varlena 头以适当对齐边界交给我们
 * 可能经解 TOAST 后，于是下界亦已对齐。
 * 注意这意味着不能处理打包（短 varlena 头）
 * 值；必须先解 TOAST。
 * 
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/timestamp.h"
#include "varatt.h"


/* 某个范围 I/O 函数的 fn_extra 缓存条目 */
typedef struct RangeIOData
{
	TypeCacheEntry *typcache;	/* 范围 typcache 条目 */
	FmgrInfo	typioproc;		/* 元素类型的 I/O 函数 */
	Oid			typioparam;		/* 元素类型的 I/O 参数 */
} RangeIOData;


static RangeIOData *get_range_io_data(FunctionCallInfo fcinfo, Oid rngtypid,
									  IOFuncSelector func);
static char range_parse_flags(const char *flags_str);
static bool range_parse(const char *string, char *flags, char **lbound_str,
						char **ubound_str, Node *escontext);
static const char *range_parse_bound(const char *string, const char *ptr,
									 char **bound_str, bool *infinite,
									 Node *escontext);
static char *range_deparse(char flags, const char *lbound_str,
						   const char *ubound_str);
static char *range_bound_escape(const char *value);
static Size datum_compute_size(Size data_length, Datum val, bool typbyval,
							   char typalign, int16 typlen, char typstorage);
static Pointer datum_write(Pointer ptr, Datum datum, bool typbyval,
						   char typalign, int16 typlen, char typstorage);


/*
 *----------------------------------------------------------
 * I/O 函数
 *----------------------------------------------------------
 */

Datum
range_in(PG_FUNCTION_ARGS)
{
	char	   *input_str = PG_GETARG_CSTRING(0);
	Oid			rngtypoid = PG_GETARG_OID(1);
	Oid			typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	RangeType  *range;
	RangeIOData *cache;
	char		flags;
	char	   *lbound_str;
	char	   *ubound_str;
	RangeBound	lower;
	RangeBound	upper;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	cache = get_range_io_data(fcinfo, rngtypoid, IOFunc_input);

	/*解析*/
	if (!range_parse(input_str, &flags, &lbound_str, &ubound_str, escontext))
		PG_RETURN_NULL();

	/* 调用元素类型的输入函数 */
	if (RANGE_HAS_LBOUND(flags))
		if (!InputFunctionCallSafe(&cache->typioproc, lbound_str,
								   cache->typioparam, typmod,
								   escontext, &lower.val))
			PG_RETURN_NULL();
	if (RANGE_HAS_UBOUND(flags))
		if (!InputFunctionCallSafe(&cache->typioproc, ubound_str,
								   cache->typioparam, typmod,
								   escontext, &upper.val))
			PG_RETURN_NULL();

	lower.infinite = (flags & RANGE_LB_INF) != 0;
	lower.inclusive = (flags & RANGE_LB_INC) != 0;
	lower.lower = true;
	upper.infinite = (flags & RANGE_UB_INF) != 0;
	upper.inclusive = (flags & RANGE_UB_INC) != 0;
	upper.lower = false;

	/* 序列化并规范化 */
	range = make_range(cache->typcache, &lower, &upper,
					   flags & RANGE_EMPTY, escontext);

	PG_RETURN_RANGE_P(range);
}

Datum
range_out(PG_FUNCTION_ARGS)
{
	RangeType  *range = PG_GETARG_RANGE_P(0);
	char	   *output_str;
	RangeIOData *cache;
	char		flags;
	char	   *lbound_str = NULL;
	char	   *ubound_str = NULL;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	cache = get_range_io_data(fcinfo, RangeTypeGetOid(range), IOFunc_output);

	/*反序列化*/
	range_deserialize(cache->typcache, range, &lower, &upper, &empty);
	flags = range_get_flags(range);

	/* 调用元素类型的输出函数 */
	if (RANGE_HAS_LBOUND(flags))
		lbound_str = OutputFunctionCall(&cache->typioproc, lower.val);
	if (RANGE_HAS_UBOUND(flags))
		ubound_str = OutputFunctionCall(&cache->typioproc, upper.val);

	/* 构造结果字符串 */
	output_str = range_deparse(flags, lbound_str, ubound_str);

	PG_RETURN_CSTRING(output_str);
}

/*
 * 二进制表示：首字节为标志，随后是下界（若有），
 * 再是上界（若有）。每个边界由
 * 4 字节长度头与该边界的二进制表示组成（即
 * 调用子类型 send 函数的返回值）。
 */

Datum
range_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			rngtypoid = PG_GETARG_OID(1);
	int32		typmod = PG_GETARG_INT32(2);
	RangeType  *range;
	RangeIOData *cache;
	char		flags;
	RangeBound	lower;
	RangeBound	upper;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	cache = get_range_io_data(fcinfo, rngtypoid, IOFunc_receive);

	/* 接收标志… */
	flags = (unsigned char) pq_getmsgbyte(buf);

	/*
	 * 屏蔽不支持的标志，尤其 RANGE_xB_NULL，以免干扰
	 * 后续测试。注意 range_serialize 会清理其余标志中
	 * 的任何不一致。
	 */
	flags &= (RANGE_EMPTY |
			  RANGE_LB_INC |
			  RANGE_LB_INF |
			  RANGE_UB_INC |
			  RANGE_UB_INF);

	/* 接收边界… */
	if (RANGE_HAS_LBOUND(flags))
	{
		uint32		bound_len = pq_getmsgint(buf, 4);
		const char *bound_data = pq_getmsgbytes(buf, bound_len);
		StringInfoData bound_buf;

		initStringInfo(&bound_buf);
		appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

		lower.val = ReceiveFunctionCall(&cache->typioproc,
										&bound_buf,
										cache->typioparam,
										typmod);
		pfree(bound_buf.data);
	}
	else
		lower.val = (Datum) 0;

	if (RANGE_HAS_UBOUND(flags))
	{
		uint32		bound_len = pq_getmsgint(buf, 4);
		const char *bound_data = pq_getmsgbytes(buf, bound_len);
		StringInfoData bound_buf;

		initStringInfo(&bound_buf);
		appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

		upper.val = ReceiveFunctionCall(&cache->typioproc,
										&bound_buf,
										cache->typioparam,
										typmod);
		pfree(bound_buf.data);
	}
	else
		upper.val = (Datum) 0;

	pq_getmsgend(buf);

	/* 完成构造 RangeBound 表示 */
	lower.infinite = (flags & RANGE_LB_INF) != 0;
	lower.inclusive = (flags & RANGE_LB_INC) != 0;
	lower.lower = true;
	upper.infinite = (flags & RANGE_UB_INF) != 0;
	upper.inclusive = (flags & RANGE_UB_INC) != 0;
	upper.lower = false;

	/* 序列化并规范化 */
	range = make_range(cache->typcache, &lower, &upper,
					   flags & RANGE_EMPTY, NULL);

	PG_RETURN_RANGE_P(range);
}

Datum
range_send(PG_FUNCTION_ARGS)
{
	RangeType  *range = PG_GETARG_RANGE_P(0);
	StringInfo	buf = makeStringInfo();
	RangeIOData *cache;
	char		flags;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	cache = get_range_io_data(fcinfo, RangeTypeGetOid(range), IOFunc_send);

	/*反序列化*/
	range_deserialize(cache->typcache, range, &lower, &upper, &empty);
	flags = range_get_flags(range);

	/* 构造输出 */
	pq_begintypsend(buf);

	pq_sendbyte(buf, flags);

	if (RANGE_HAS_LBOUND(flags))
	{
		Datum		bound = PointerGetDatum(SendFunctionCall(&cache->typioproc,
															 lower.val));
		uint32		bound_len = VARSIZE(bound) - VARHDRSZ;
		char	   *bound_data = VARDATA(bound);

		pq_sendint32(buf, bound_len);
		pq_sendbytes(buf, bound_data, bound_len);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		Datum		bound = PointerGetDatum(SendFunctionCall(&cache->typioproc,
															 upper.val));
		uint32		bound_len = VARSIZE(bound) - VARHDRSZ;
		char	   *bound_data = VARDATA(bound);

		pq_sendint32(buf, bound_len);
		pq_sendbytes(buf, bound_data, bound_len);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * get_range_io_data：获取范围类型 I/O 所需的缓存信息
 *
 * 范围 I/O 函数比其他范围函数需要稍多缓存信息，故
 * 在 fn_extra 中存 RangeIOData 结构，而非仅存类型
 * 缓存条目指针。
 */
static RangeIOData *
get_range_io_data(FunctionCallInfo fcinfo, Oid rngtypid, IOFuncSelector func)
{
	RangeIOData *cache = (RangeIOData *) fcinfo->flinfo->fn_extra;

	if (cache == NULL || cache->typcache->type_id != rngtypid)
	{
		int16		typlen;
		bool		typbyval;
		char		typalign;
		char		typdelim;
		Oid			typiofunc;

		cache = (RangeIOData *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												   sizeof(RangeIOData));
		cache->typcache = lookup_type_cache(rngtypid, TYPECACHE_RANGE_INFO);
		if (cache->typcache->rngelemtype == NULL)
			elog(ERROR, "type %u is not a range type", rngtypid);

		/* get_type_io_data 所做超出所需，但很方便 */
		get_type_io_data(cache->typcache->rngelemtype->type_id,
						 func,
						 &typlen,
						 &typbyval,
						 &typalign,
						 &typdelim,
						 &cache->typioparam,
						 &typiofunc);

		if (!OidIsValid(typiofunc))
		{
			/* 这只可能发生在 receive 或 send */
			if (func == IOFunc_receive)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary input function available for type %s",
								format_type_be(cache->typcache->rngelemtype->type_id))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary output function available for type %s",
								format_type_be(cache->typcache->rngelemtype->type_id))));
		}
		fmgr_info_cxt(typiofunc, &cache->typioproc,
					  fcinfo->flinfo->fn_mcxt);

		fcinfo->flinfo->fn_extra = (void *) cache;
	}

	return cache;
}


/*
 *----------------------------------------------------------
 * 通用函数
 *----------------------------------------------------------
 */

/* 由两参数构造标准形式范围值 */
Datum
range_constructor2(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	typcache = range_get_typcache(fcinfo, rngtypid);

	lower.val = PG_ARGISNULL(0) ? (Datum) 0 : arg1;
	lower.infinite = PG_ARGISNULL(0);
	lower.inclusive = true;
	lower.lower = true;

	upper.val = PG_ARGISNULL(1) ? (Datum) 0 : arg2;
	upper.infinite = PG_ARGISNULL(1);
	upper.inclusive = false;
	upper.lower = false;

	range = make_range(typcache, &lower, &upper, false, NULL);

	PG_RETURN_RANGE_P(range);
}

/* 由三参数构造一般范围值 */
Datum
range_constructor3(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	RangeType  *range;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	char		flags;

	typcache = range_get_typcache(fcinfo, rngtypid);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("range constructor flags argument must not be null")));

	flags = range_parse_flags(text_to_cstring(PG_GETARG_TEXT_PP(2)));

	lower.val = PG_ARGISNULL(0) ? (Datum) 0 : arg1;
	lower.infinite = PG_ARGISNULL(0);
	lower.inclusive = (flags & RANGE_LB_INC) != 0;
	lower.lower = true;

	upper.val = PG_ARGISNULL(1) ? (Datum) 0 : arg2;
	upper.infinite = PG_ARGISNULL(1);
	upper.inclusive = (flags & RANGE_UB_INC) != 0;
	upper.lower = false;

	range = make_range(typcache, &lower, &upper, false, NULL);

	PG_RETURN_RANGE_P(range);
}


/* 范围 -> 子类型函数 */

/* 提取下界值 */
Datum
range_lower(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	range_deserialize(typcache, r1, &lower, &upper, &empty);

	/* 若无有限下界则返回 NULL */
	if (empty || lower.infinite)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(lower.val);
}

/* 提取上界值 */
Datum
range_upper(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	range_deserialize(typcache, r1, &lower, &upper, &empty);

	/* 若无有限上界则返回 NULL */
	if (empty || upper.infinite)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(upper.val);
}


/* 范围 -> bool 函数 */

/* 范围为空？ */
Datum
range_empty(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	char		flags = range_get_flags(r1);

	PG_RETURN_BOOL(flags & RANGE_EMPTY);
}

/* 下界含端点？ */
Datum
range_lower_inc(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	char		flags = range_get_flags(r1);

	PG_RETURN_BOOL(flags & RANGE_LB_INC);
}

/* 上界含端点？ */
Datum
range_upper_inc(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	char		flags = range_get_flags(r1);

	PG_RETURN_BOOL(flags & RANGE_UB_INC);
}

/* 下界无限？ */
Datum
range_lower_inf(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	char		flags = range_get_flags(r1);

	PG_RETURN_BOOL(flags & RANGE_LB_INF);
}

/* 上界无限？ */
Datum
range_upper_inf(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	char		flags = range_get_flags(r1);

	PG_RETURN_BOOL(flags & RANGE_UB_INF);
}


/* 范围、元素 -> bool 函数 */

/* 包含？ */
Datum
range_contains_elem(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	Datum		val = PG_GETARG_DATUM(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	PG_RETURN_BOOL(range_contains_elem_internal(typcache, r, val));
}

/* 被包含？ */
Datum
elem_contained_by_range(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	PG_RETURN_BOOL(range_contains_elem_internal(typcache, r, val));
}


/* 范围、范围 -> bool 函数 */

/* 相等（内部版本） */
bool
range_eq_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	if (empty1 && empty2)
		return true;
	if (empty1 != empty2)
		return false;

	if (range_cmp_bounds(typcache, &lower1, &lower2) != 0)
		return false;

	if (range_cmp_bounds(typcache, &upper1, &upper2) != 0)
		return false;

	return true;
}

/* 相等 */
Datum
range_eq(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_eq_internal(typcache, r1, r2));
}

/* 不等（内部版本） */
bool
range_ne_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	return (!range_eq_internal(typcache, r1, r2));
}

/* 不等 */
Datum
range_ne(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_ne_internal(typcache, r1, r2));
}

/* 包含？ */
Datum
range_contains(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_contains_internal(typcache, r1, r2));
}

/* 被包含？ */
Datum
range_contained_by(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_contained_by_internal(typcache, r1, r2));
}

/* 严格在左？（内部版） */
bool
range_before_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围既不在任何其他范围之前也不在其后 */
	if (empty1 || empty2)
		return false;

	return (range_cmp_bounds(typcache, &upper1, &lower2) < 0);
}

/* 严格在左？ */
Datum
range_before(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_before_internal(typcache, r1, r2));
}

/* 严格在右？（内部版） */
bool
range_after_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围既不在任何其他范围之前也不在其后 */
	if (empty1 || empty2)
		return false;

	return (range_cmp_bounds(typcache, &lower1, &upper2) > 0);
}

/* 严格在右？ */
Datum
range_after(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_after_internal(typcache, r1, r2));
}

/*
 * 检查两边界 A 与 B 是否"相邻"，其中 A 为上界、B
 * 为下界。相邻要求每个子类型值恰满足两界之一：无
 * 值同时满足两界（即小于 A 且大于 B）；亦无值
 * 两界都不满足（即大于 A 且小于 B）。
 * 
 *
 * 对离散范围，依赖规范化函数查看 A..B 是否规范
 * 为空。（若无规范化函数，此类范围不可能规范为
 * 空，故不必尝试。）
 * 
 *
 * 若 A == B，仅当边界含端标志不同时两范围才相邻
 * 即恰有一个范围包含该公共边界点。
 * 
 *
 * 若 A > B，则按此顺序两范围不相邻。
 */
bool
bounds_adjacent(TypeCacheEntry *typcache, RangeBound boundA, RangeBound boundB)
{
	int			cmp;

	Assert(!boundA.lower && boundB.lower);

	cmp = range_cmp_bound_values(typcache, &boundA, &boundB);
	if (cmp < 0)
	{
		RangeType  *r;

		/*
		 * 边界不重叠；查看中间是否有点。
		 */

		/* 连续子类型假定两界之间有点 */
		if (!OidIsValid(typcache->rng_canonical_finfo.fn_oid))
			return false;

		/*
		 * 边界属离散范围类型；故构造范围 A..B 并查看
		 * 是否为空。
		 */

		/* 翻转含端标志 */
		boundA.inclusive = !boundA.inclusive;
		boundB.inclusive = !boundB.inclusive;
		/* 改上/下界标签以免 Assert 失败 */
		boundA.lower = true;
		boundB.lower = false;
		r = make_range(typcache, &boundA, &boundB, false, NULL);
		return RangeIsEmpty(r);
	}
	else if (cmp == 0)
		return boundA.inclusive != boundB.inclusive;
	else
		return false;			/* 边界重叠 */
}

/* 相邻（但不重叠）？（内部版） */
bool
range_adjacent_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围不与任何其他范围相邻 */
	if (empty1 || empty2)
		return false;

	/*
	 * 给定两范围 A..B 与 C..D，相邻当且仅当 B 与 C
	 * 相邻，或 D 与 A 相邻。
	 */
	return (bounds_adjacent(typcache, upper1, lower2) ||
			bounds_adjacent(typcache, upper2, lower1));
}

/* 相邻（但不重叠）？ */
Datum
range_adjacent(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_adjacent_internal(typcache, r1, r2));
}

/* 重叠？（内部版） */
bool
range_overlaps_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围不与任何其他范围重叠 */
	if (empty1 || empty2)
		return false;

	if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0 &&
		range_cmp_bounds(typcache, &lower1, &upper2) <= 0)
		return true;

	if (range_cmp_bounds(typcache, &lower2, &lower1) >= 0 &&
		range_cmp_bounds(typcache, &lower2, &upper1) <= 0)
		return true;

	return false;
}

/* 重叠？ */
Datum
range_overlaps(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_overlaps_internal(typcache, r1, r2));
}

/* 是否不延伸到右侧？（内部版） */
bool
range_overleft_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围既不在任何其他范围之前也不在其后 */
	if (empty1 || empty2)
		return false;

	if (range_cmp_bounds(typcache, &upper1, &upper2) <= 0)
		return true;

	return false;
}

/* 是否不延伸到右侧？ */
Datum
range_overleft(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_overleft_internal(typcache, r1, r2));
}

/* 是否不延伸到左侧？（内部版） */
bool
range_overright_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 空范围既不在任何其他范围之前也不在其后 */
	if (empty1 || empty2)
		return false;

	if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0)
		return true;

	return false;
}

/* 是否不延伸到左侧？ */
Datum
range_overright(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_BOOL(range_overright_internal(typcache, r1, r2));
}


/* 范围、范围 -> 范围函数 */

/* 差集 */
Datum
range_minus(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	RangeType  *ret;
	TypeCacheEntry *typcache;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	ret = range_minus_internal(typcache, r1, r2);
	if (ret)
		PG_RETURN_RANGE_P(ret);
	else
		PG_RETURN_NULL();
}

RangeType *
range_minus_internal(TypeCacheEntry *typcache, RangeType *r1, RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			cmp_l1l2,
				cmp_l1u2,
				cmp_u1l2,
				cmp_u1u2;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 若任一为空，则 r1 即为正确结果 */
	if (empty1 || empty2)
		return r1;

	cmp_l1l2 = range_cmp_bounds(typcache, &lower1, &lower2);
	cmp_l1u2 = range_cmp_bounds(typcache, &lower1, &upper2);
	cmp_u1l2 = range_cmp_bounds(typcache, &upper1, &lower2);
	cmp_u1u2 = range_cmp_bounds(typcache, &upper1, &upper2);

	if (cmp_l1l2 < 0 && cmp_u1u2 > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("result of range difference would not be contiguous")));

	if (cmp_l1u2 > 0 || cmp_u1l2 < 0)
		return r1;

	if (cmp_l1l2 >= 0 && cmp_u1u2 <= 0)
		return make_empty_range(typcache);

	if (cmp_l1l2 <= 0 && cmp_u1l2 >= 0 && cmp_u1u2 <= 0)
	{
		lower2.inclusive = !lower2.inclusive;
		lower2.lower = false;	/* 它将成为上界 */
		return make_range(typcache, &lower1, &lower2, false, NULL);
	}

	if (cmp_l1l2 >= 0 && cmp_u1u2 >= 0 && cmp_l1u2 <= 0)
	{
		upper2.inclusive = !upper2.inclusive;
		upper2.lower = true;	/* 它将成为下界 */
		return make_range(typcache, &upper2, &upper1, false, NULL);
	}

	elog(ERROR, "unexpected case in range_minus");
	return NULL;
}

/*
 * 并集。若 strict 为真，两输入范围不相邻或不重叠
 * 即为错误。
 */
RangeType *
range_union_internal(TypeCacheEntry *typcache, RangeType *r1, RangeType *r2,
					 bool strict)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 若任一为空，则另一个即为正确结果 */
	if (empty1)
		return r2;
	if (empty2)
		return r1;

	if (strict &&
		!DatumGetBool(range_overlaps_internal(typcache, r1, r2)) &&
		!DatumGetBool(range_adjacent_internal(typcache, r1, r2)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("result of range union would not be contiguous")));

	if (range_cmp_bounds(typcache, &lower1, &lower2) < 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(typcache, &upper1, &upper2) > 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	return make_range(typcache, result_lower, result_upper, false, NULL);
}

Datum
range_union(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_RANGE_P(range_union_internal(typcache, r1, r2, true));
}

/*
 * range merge：类似并集，亦允许并处理不相邻
 * 输入范围
 */
Datum
range_merge(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_RANGE_P(range_union_internal(typcache, r1, r2, false));
}

/* 交集 */
Datum
range_intersect(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	PG_RETURN_RANGE_P(range_intersect_internal(typcache, r1, r2));
}

RangeType *
range_intersect_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	RangeBound *result_lower;
	RangeBound *result_upper;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	if (empty1 || empty2 || !range_overlaps_internal(typcache, r1, r2))
		return make_empty_range(typcache);

	if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0)
		result_lower = &lower1;
	else
		result_lower = &lower2;

	if (range_cmp_bounds(typcache, &upper1, &upper2) <= 0)
		result_upper = &upper1;
	else
		result_upper = &upper2;

	return make_range(typcache, result_lower, result_upper, false, NULL);
}

/* 范围、范围 -> 范围对函数 */

/*
 * range_split_internal - 若 r2 交于 r1 中部且两侧
 * 留下非空范围，则返回真并把 output1/output2 设为
 * r1 - r2 的结果（按序）。否则返回假且不设置它们。
 * 两输入范围都不应为空。
 */
bool
range_split_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2,
					 RangeType **output1, RangeType **output2)
{
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	if (range_cmp_bounds(typcache, &lower1, &lower2) < 0 &&
		range_cmp_bounds(typcache, &upper1, &upper2) > 0)
	{
		/*
		 * 须反转 lower2 与 upper2 点的含端/开端。它们不
		 * 可能无限。允许覆写这些 RangeBounds，因其仅局部
		 * 存在。
		 */
		lower2.inclusive = !lower2.inclusive;
		lower2.lower = false;
		upper2.inclusive = !upper2.inclusive;
		upper2.lower = true;

		*output1 = make_range(typcache, &lower1, &lower2, false, NULL);
		*output2 = make_range(typcache, &upper2, &upper1, false, NULL);
		return true;
	}

	return false;
}

/* 范围 -> 范围聚集函数 */

Datum
range_intersect_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			rngtypoid;
	TypeCacheEntry *typcache;
	RangeType  *result;
	RangeType  *current;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "range_intersect_agg_transfn called in non-aggregate context");

	rngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_range(rngtypoid))
		elog(ERROR, "range_intersect_agg must be called with a range");

	typcache = range_get_typcache(fcinfo, rngtypoid);

	/* 严格性保证这些非 NULL */
	result = PG_GETARG_RANGE_P(0);
	current = PG_GETARG_RANGE_P(1);

	result = range_intersect_internal(typcache, result, current);
	PG_RETURN_RANGE_P(result);
}


/* Btree 支持 */

/* btree 比较器 */
Datum
range_cmp(PG_FUNCTION_ARGS)
{
	RangeType  *r1 = PG_GETARG_RANGE_P(0);
	RangeType  *r2 = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			cmp;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/*供 b-tree 使用时，空范围排在一切之前*/
	if (empty1 && empty2)
		cmp = 0;
	else if (empty1)
		cmp = -1;
	else if (empty2)
		cmp = 1;
	else
	{
		cmp = range_cmp_bounds(typcache, &lower1, &lower2);
		if (cmp == 0)
			cmp = range_cmp_bounds(typcache, &upper1, &upper2);
	}

	PG_FREE_IF_COPY(r1, 0);
	PG_FREE_IF_COPY(r2, 1);

	PG_RETURN_INT32(cmp);
}

/* 使用 range_cmp 函数的不等算子 */
Datum
range_lt(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
range_le(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
range_ge(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
range_gt(PG_FUNCTION_ARGS)
{
	int			cmp = range_cmp(fcinfo);

	PG_RETURN_BOOL(cmp > 0);
}

/* Hash 支持 */

/* 哈希范围值 */
Datum
hash_range(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	uint32		result;
	TypeCacheEntry *typcache;
	TypeCacheEntry *scache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	char		flags;
	uint32		lower_hash;
	uint32		upper_hash;

	check_stack_depth();		/* 子类型为范围类型时会递归 */

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	/*反序列化*/
	range_deserialize(typcache, r, &lower, &upper, &empty);
	flags = range_get_flags(r);

	/*
	 * 若尚未完成，则查找元素类型的哈希函数。
	 */
	scache = typcache->rngelemtype;
	if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id, TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	/*
	 * 对每个边界应用哈希函数。
	 */
	if (RANGE_HAS_LBOUND(flags))
		lower_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
													  typcache->rng_collation,
													  lower.val));
	else
		lower_hash = 0;

	if (RANGE_HAS_UBOUND(flags))
		upper_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
													  typcache->rng_collation,
													  upper.val));
	else
		upper_hash = 0;

	/* 合并标志与边界的哈希 */
	result = hash_uint32((uint32) flags);
	result ^= lower_hash;
	result = pg_rotate_left32(result, 1);
	result ^= upper_hash;

	PG_RETURN_INT32(result);
}

/*
 * 用种子把值哈希为 64 位值并返回。其余方面类似
 * hash_range。
 */
Datum
hash_range_extended(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	Datum		seed = PG_GETARG_DATUM(1);
	uint64		result;
	TypeCacheEntry *typcache;
	TypeCacheEntry *scache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	char		flags;
	uint64		lower_hash;
	uint64		upper_hash;

	check_stack_depth();

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	range_deserialize(typcache, r, &lower, &upper, &empty);
	flags = range_get_flags(r);

	scache = typcache->rngelemtype;
	if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id,
								   TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	if (RANGE_HAS_LBOUND(flags))
		lower_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
													  typcache->rng_collation,
													  lower.val,
													  seed));
	else
		lower_hash = 0;

	if (RANGE_HAS_UBOUND(flags))
		upper_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
													  typcache->rng_collation,
													  upper.val,
													  seed));
	else
		upper_hash = 0;

	/* 合并标志与边界的哈希 */
	result = DatumGetUInt64(hash_uint32_extended((uint32) flags,
												 DatumGetInt64(seed)));
	result ^= lower_hash;
	result = ROTATE_HIGH_AND_LOW_32BITS(result);
	result ^= upper_hash;

	PG_RETURN_UINT64(result);
}

/*
 *----------------------------------------------------------
 * 规范化函数
 *
 *	 特定内置范围类型的函数。
 *----------------------------------------------------------
 */

Datum
int4range_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	Node	   *escontext = fcinfo->context;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	range_deserialize(typcache, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE_P(r);

	if (!lower.infinite && !lower.inclusive)
	{
		int32		bnd = DatumGetInt32(lower.val);

		/* 手动处理可能的溢出 */
		if (unlikely(bnd == PG_INT32_MAX))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		lower.val = Int32GetDatum(bnd + 1);
		lower.inclusive = true;
	}

	if (!upper.infinite && upper.inclusive)
	{
		int32		bnd = DatumGetInt32(upper.val);

		/* 手动处理可能的溢出 */
		if (unlikely(bnd == PG_INT32_MAX))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		upper.val = Int32GetDatum(bnd + 1);
		upper.inclusive = false;
	}

	PG_RETURN_RANGE_P(range_serialize(typcache, &lower, &upper,
									  false, escontext));
}

Datum
int8range_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	Node	   *escontext = fcinfo->context;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	range_deserialize(typcache, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE_P(r);

	if (!lower.infinite && !lower.inclusive)
	{
		int64		bnd = DatumGetInt64(lower.val);

		/* 手动处理可能的溢出 */
		if (unlikely(bnd == PG_INT64_MAX))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		lower.val = Int64GetDatum(bnd + 1);
		lower.inclusive = true;
	}

	if (!upper.infinite && upper.inclusive)
	{
		int64		bnd = DatumGetInt64(upper.val);

		/* 手动处理可能的溢出 */
		if (unlikely(bnd == PG_INT64_MAX))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		upper.val = Int64GetDatum(bnd + 1);
		upper.inclusive = false;
	}

	PG_RETURN_RANGE_P(range_serialize(typcache, &lower, &upper,
									  false, escontext));
}

Datum
daterange_canonical(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	Node	   *escontext = fcinfo->context;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;

	typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

	range_deserialize(typcache, r, &lower, &upper, &empty);

	if (empty)
		PG_RETURN_RANGE_P(r);

	if (!lower.infinite && !DATE_NOT_FINITE(DatumGetDateADT(lower.val)) &&
		!lower.inclusive)
	{
		DateADT		bnd = DatumGetDateADT(lower.val);

		/* 检查溢出——注意已排除 PG_INT32_MAX */
		bnd++;
		if (unlikely(!IS_VALID_DATE(bnd)))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("date out of range")));
		lower.val = DateADTGetDatum(bnd);
		lower.inclusive = true;
	}

	if (!upper.infinite && !DATE_NOT_FINITE(DatumGetDateADT(upper.val)) &&
		upper.inclusive)
	{
		DateADT		bnd = DatumGetDateADT(upper.val);

		/* 检查溢出——注意已排除 PG_INT32_MAX */
		bnd++;
		if (unlikely(!IS_VALID_DATE(bnd)))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("date out of range")));
		upper.val = DateADTGetDatum(bnd);
		upper.inclusive = false;
	}

	PG_RETURN_RANGE_P(range_serialize(typcache, &lower, &upper,
									  false, escontext));
}

/*
 *----------------------------------------------------------
 * SUBTYPE_DIFF 函数
 *
 * 特定内置范围类型的函数。
 *
 * 注意 subtype_diff 返回差值本身而非绝对值，且须
 * 小心避免溢出。
 * 注意：numrange_subdiff 有风险...
 *----------------------------------------------------------
 */

Datum
int4range_subdiff(PG_FUNCTION_ARGS)
{
	int32		v1 = PG_GETARG_INT32(0);
	int32		v2 = PG_GETARG_INT32(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
int8range_subdiff(PG_FUNCTION_ARGS)
{
	int64		v1 = PG_GETARG_INT64(0);
	int64		v2 = PG_GETARG_INT64(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
numrange_subdiff(PG_FUNCTION_ARGS)
{
	Datum		v1 = PG_GETARG_DATUM(0);
	Datum		v2 = PG_GETARG_DATUM(1);
	Datum		numresult;
	float8		floatresult;

	numresult = DirectFunctionCall2(numeric_sub, v1, v2);

	floatresult = DatumGetFloat8(DirectFunctionCall1(numeric_float8,
													 numresult));

	PG_RETURN_FLOAT8(floatresult);
}

Datum
daterange_subdiff(PG_FUNCTION_ARGS)
{
	int32		v1 = PG_GETARG_INT32(0);
	int32		v2 = PG_GETARG_INT32(1);

	PG_RETURN_FLOAT8((float8) v1 - (float8) v2);
}

Datum
tsrange_subdiff(PG_FUNCTION_ARGS)
{
	Timestamp	v1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	v2 = PG_GETARG_TIMESTAMP(1);
	float8		result;

	result = ((float8) v1 - (float8) v2) / USECS_PER_SEC;
	PG_RETURN_FLOAT8(result);
}

Datum
tstzrange_subdiff(PG_FUNCTION_ARGS)
{
	Timestamp	v1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	v2 = PG_GETARG_TIMESTAMP(1);
	float8		result;

	result = ((float8) v1 - (float8) v2) / USECS_PER_SEC;
	PG_RETURN_FLOAT8(result);
}

/*
 *----------------------------------------------------------
 * 支持函数
 *
 *	 这些函数不在 pg_proc，但对在 C 中定义
 *	 新的通用范围函数很有用。
 *----------------------------------------------------------
 */

/*
 * range_get_typcache：获取范围类型的缓存信息
 *
 * 供遵循如下约定的范围相关函数使用：把 fn_extra
 * 字段用作该范围类型的类型缓存条目指针。需缓存
 * 更多信息的函数须自行处理。
 * 
 */
TypeCacheEntry *
range_get_typcache(FunctionCallInfo fcinfo, Oid rngtypid)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;

	if (typcache == NULL ||
		typcache->type_id != rngtypid)
	{
		typcache = lookup_type_cache(rngtypid, TYPECACHE_RANGE_INFO);
		if (typcache->rngelemtype == NULL)
			elog(ERROR, "type %u is not a range type", rngtypid);
		fcinfo->flinfo->fn_extra = (void *) typcache;
	}

	return typcache;
}

/*
 * range_serialize：由边界与空标志构造范围值
 *
 * 本函数不强制规范化范围值。多数情况下，外部
 * 调用方应仅为规范化函数。注意我们仍会做一些
 * 与数据类型无关的规范化检查。
 */
RangeType *
range_serialize(TypeCacheEntry *typcache, RangeBound *lower, RangeBound *upper,
				bool empty, struct Node *escontext)
{
	RangeType  *range;
	int			cmp;
	Size		msize;
	Pointer		ptr;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typstorage;
	char		flags = 0;

	/*
	 * 验证范围表面有效，并构造标志值，阻止无限+含端
	 * 等非规范组合。
	 */
	Assert(lower->lower);
	Assert(!upper->lower);

	if (empty)
		flags |= RANGE_EMPTY;
	else
	{
		cmp = range_cmp_bound_values(typcache, lower, upper);

		/* 错误检查：下界值高于上界则有误 */
		if (cmp > 0)
			ereturn(escontext, NULL,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("range lower bound must be less than or equal to range upper bound")));

		/* 边界相等且非都含端则范围为空 */
		if (cmp == 0 && !(lower->inclusive && upper->inclusive))
			flags |= RANGE_EMPTY;
		else
		{
			/* 无限边界永不含端 */
			if (lower->infinite)
				flags |= RANGE_LB_INF;
			else if (lower->inclusive)
				flags |= RANGE_LB_INC;
			if (upper->infinite)
				flags |= RANGE_UB_INF;
			else if (upper->inclusive)
				flags |= RANGE_UB_INC;
		}
	}

	/* 获取范围元素类型的信息 */
	typlen = typcache->rngelemtype->typlen;
	typbyval = typcache->rngelemtype->typbyval;
	typalign = typcache->rngelemtype->typalign;
	typstorage = typcache->rngelemtype->typstorage;

	/* 为 varlena 头与范围 OID 计空间 */
	msize = sizeof(RangeType);
	Assert(msize == MAXALIGN(msize));

	/* 为边界计空间 */
	if (RANGE_HAS_LBOUND(flags))
	{
		/*
		 * 确保待插入项未经 TOAST。切不可把行外 toast
		 * 值指针插入范围对象，理由同数组与记录。
		 * 存行内压缩值也可行，但我们更倾向先解压，
		 * 再必要时压缩整个范围对象。不过与数组不同，
		 * 允许短头 varlena 对象保持原样。
		 * 
		 * 
		 */
		if (typlen == -1)
			lower->val = PointerGetDatum(PG_DETOAST_DATUM_PACKED(lower->val));

		msize = datum_compute_size(msize, lower->val, typbyval, typalign,
								   typlen, typstorage);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		/* 确保待插入项未经 TOAST */
		if (typlen == -1)
			upper->val = PointerGetDatum(PG_DETOAST_DATUM_PACKED(upper->val));

		msize = datum_compute_size(msize, upper->val, typbyval, typalign,
								   typlen, typstorage);
	}

	/*为标志字节加空间*/
	msize += sizeof(char);

	/* 注意：此处须零填充，与堆元组相同 */
	range = (RangeType *) palloc0(msize);
	SET_VARSIZE(range, msize);

	/* 现在填入 datum */
	range->rangetypid = typcache->type_id;

	ptr = (char *) (range + 1);

	if (RANGE_HAS_LBOUND(flags))
	{
		Assert(lower->lower);
		ptr = datum_write(ptr, lower->val, typbyval, typalign, typlen,
						  typstorage);
	}

	if (RANGE_HAS_UBOUND(flags))
	{
		Assert(!upper->lower);
		ptr = datum_write(ptr, upper->val, typbyval, typalign, typlen,
						  typstorage);
	}

	*((char *) ptr) = flags;

	return range;
}

/*
 * range_deserialize：拆解范围值
 *
 * 注意：给定范围对象必须已完全解 TOAST；无法带
 * 短 varlena 头。
 *
 * 若元素类型为传引用，则 RangeBound 结构中的
 * datum 将是指向给定范围对象的指针。
 */
void
range_deserialize(TypeCacheEntry *typcache, const RangeType *range,
				  RangeBound *lower, RangeBound *upper, bool *empty)
{
	char		flags;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Pointer		ptr;
	Datum		lbound;
	Datum		ubound;

	/* 断言传入正确 typcache 条目 */
	Assert(RangeTypeGetOid(range) == typcache->type_id);

	/* 从 datum 末字节取标志 */
	flags = *((const char *) range + VARSIZE(range) - 1);

	/* 获取范围元素类型的信息 */
	typlen = typcache->rngelemtype->typlen;
	typbyval = typcache->rngelemtype->typbyval;
	typalign = typcache->rngelemtype->typalign;

	/* 数据指针初始化到范围 OID 后 */
	ptr = (Pointer) (range + 1);

	/* 取下界（若有） */
	if (RANGE_HAS_LBOUND(flags))
	{
		/* 此处不可能需要 att_align_pointer */
		lbound = fetch_att(ptr, typbyval, typlen);
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	}
	else
		lbound = (Datum) 0;

	/* 取上界（若有） */
	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ubound = fetch_att(ptr, typbyval, typlen);
		/* 无需 att_addlength_pointer */
	}
	else
		ubound = (Datum) 0;

	/* 输出结果 */

	*empty = (flags & RANGE_EMPTY) != 0;

	lower->val = lbound;
	lower->infinite = (flags & RANGE_LB_INF) != 0;
	lower->inclusive = (flags & RANGE_LB_INC) != 0;
	lower->lower = true;

	upper->val = ubound;
	upper->infinite = (flags & RANGE_UB_INF) != 0;
	upper->inclusive = (flags & RANGE_UB_INC) != 0;
	upper->lower = false;
}

/*
 * range_get_flags：仅从 RangeType 值取标志。
 *
 * 在只需标志而不需 range_deserialize 全部结果的
 * 场合经常有用。
 */
char
range_get_flags(const RangeType *range)
{
	/* 从 datum 末字节取标志 */
	return *((char *) range + VARSIZE(range) - 1);
}

/*
 * range_set_contain_empty：在值中设置 RANGE_CONTAIN_EMPTY 位。
 *
 * 仅 GiST 操作需要，故未在 range_serialize 中提供设置
 * 途径；须在其后应用本函数。
 * 
 */
void
range_set_contain_empty(RangeType *range)
{
	char	   *flagsp;

	/*标志字节为 datum 末字节*/
	flagsp = (char *) range + VARSIZE(range) - 1;

	*flagsp |= RANGE_CONTAIN_EMPTY;
}

/*
 * 同时序列化并（若适用）规范化范围。
 * 多数调用方应使用本函数。
 */
RangeType *
make_range(TypeCacheEntry *typcache, RangeBound *lower, RangeBound *upper,
		   bool empty, struct Node *escontext)
{
	RangeType  *range;

	range = range_serialize(typcache, lower, upper, empty, escontext);

	if (SOFT_ERROR_OCCURRED(escontext))
		return NULL;

	/* 空范围无需调用 canonical … */
	if (OidIsValid(typcache->rng_canonical_finfo.fn_oid) &&
		!RangeIsEmpty(range))
	{
		/* 用此麻烦方式以便传 escontext */
		LOCAL_FCINFO(fcinfo, 1);
		Datum		result;

		InitFunctionCallInfoData(*fcinfo, &typcache->rng_canonical_finfo, 1,
								 InvalidOid, escontext, NULL);

		fcinfo->args[0].value = RangeTypePGetDatum(range);
		fcinfo->args[0].isnull = false;

		result = FunctionCallInvoke(fcinfo);

		if (SOFT_ERROR_OCCURRED(escontext))
			return NULL;

		/* 无错误时不应得空结果 */
		if (fcinfo->isnull)
			elog(ERROR, "function %u returned NULL",
				 typcache->rng_canonical_finfo.fn_oid);

		range = DatumGetRangeTypeP(result);
	}

	return range;
}

/*
 * 比较两范围边界点，按 b1 小于、等于或大于 b2
 * 返回 <0、0 或 >0。
 *
 * 边界可以是上界与下界的任意组合；故对多种算子
 * 都有用。
 *
 * 简单情形是 b1 与 b2 都有限且含端，此时结果就是
 * 对 b1、b2 所持值的比较。
 *
 * 若边界为开端，则需知其为下界还是上界：下界时把
 * 边界点视为"刚大于"所持值；上界时则视为
 * 所谓"刚小于"所持值。
 * 
 *
 * 若边界无限，作为下界表示负无穷（小于其他一切
 * 点）；作为上界表示正无穷（大于其他一切点）。
 * 
 *
 * 仅有一种情形两边界比较相等但并非同一：两界都含
 * 端且持相同有限值，但一个是上界、另一个是下界。
 * 
 */
int
range_cmp_bounds(TypeCacheEntry *typcache, const RangeBound *b1, const RangeBound *b2)
{
	int32		result;

	/*
	 * 先处理涉及无穷的情形，无需调用比较过程。
	 * 
	 */
	if (b1->infinite && b2->infinite)
	{
		/*
		 * 两者皆无穷故相等，除非一为下界而另一不是。
		 * 
		 */
		if (b1->lower == b2->lower)
			return 0;
		else
			return b1->lower ? -1 : 1;
	}
	else if (b1->infinite)
		return b1->lower ? -1 : 1;
	else if (b2->infinite)
		return b2->lower ? 1 : -1;

	/*
	 * 两边界都有限，故比较所持值。
	 */
	result = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											 typcache->rng_collation,
											 b1->val, b2->val));

	/*
	 * 若比较结果非相等则完成。若相等，仍须考虑
	 * 边界是含端还是开端。
	 * 
	 */
	if (result == 0)
	{
		if (!b1->inclusive && !b2->inclusive)
		{
			/* 两者都开端 */
			if (b1->lower == b2->lower)
				return 0;
			else
				return b1->lower ? 1 : -1;
		}
		else if (!b1->inclusive)
			return b1->lower ? 1 : -1;
		else if (!b2->inclusive)
			return b2->lower ? -1 : 1;
		else
		{
			/*
			 * 两者都含端且所持值相等，故无论上界、下界
			 * 或混合都相等。
			 * 
			 */
			return 0;
		}
	}

	return result;
}

/*
 * 比较两范围边界点值，按 b1 小于、等于或大于 b2
 * 返回 <0、0 或 >0。
 *
 * 类似 range_cmp_bounds() 但更简。只比较 b1、b2
 * 所持值，忽略含端/开端标志。下/上界标志仅对无
 * 限有意义，用于区分正负无穷。
 * 
 */
int
range_cmp_bound_values(TypeCacheEntry *typcache, const RangeBound *b1,
					   const RangeBound *b2)
{
	/*
	 * 先处理涉及无穷的情形，无需调用比较过程。
	 * 
	 */
	if (b1->infinite && b2->infinite)
	{
		/*
		 * 两者皆无穷故相等，除非一为下界而另一不是。
		 * 
		 */
		if (b1->lower == b2->lower)
			return 0;
		else
			return b1->lower ? -1 : 1;
	}
	else if (b1->infinite)
		return b1->lower ? -1 : 1;
	else if (b2->infinite)
		return b2->lower ? 1 : -1;

	/*
	 * 两边界都有限，故比较所持值。
	 */
	return DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
										   typcache->rng_collation,
										   b1->val, b2->val));
}

/*
 * 排序范围的 qsort 回调。
 *
 * 两空范围比较相等；空范围排在任何非空范围之
 * 左。两非空范围先按下界、再按上界排序。
 * 
 */
int
range_compare(const void *key1, const void *key2, void *arg)
{
	RangeType  *r1 = *(RangeType **) key1;
	RangeType  *r2 = *(RangeType **) key2;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;
	RangeBound	lower1;
	RangeBound	upper1;
	RangeBound	lower2;
	RangeBound	upper2;
	bool		empty1;
	bool		empty2;
	int			cmp;

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	if (empty1 && empty2)
		cmp = 0;
	else if (empty1)
		cmp = -1;
	else if (empty2)
		cmp = 1;
	else
	{
		cmp = range_cmp_bounds(typcache, &lower1, &lower2);
		if (cmp == 0)
			cmp = range_cmp_bounds(typcache, &upper1, &upper2);
	}

	return cmp;
}

/*
 * 按 typcache 条目所示类型构造空范围值。
 */
RangeType *
make_empty_range(TypeCacheEntry *typcache)
{
	RangeBound	lower;
	RangeBound	upper;

	lower.val = (Datum) 0;
	lower.infinite = false;
	lower.inclusive = false;
	lower.lower = true;

	upper.val = (Datum) 0;
	upper.infinite = false;
	upper.inclusive = false;
	upper.lower = false;

	return make_range(typcache, &lower, &upper, true, NULL);
}


/*
 *----------------------------------------------------------
 * 静态函数
 *----------------------------------------------------------
 */

/*
 * 给定表示范围类型标志的字符串，返回以 char 表
 * 示的标志。
 */
static char
range_parse_flags(const char *flags_str)
{
	char		flags = 0;

	if (flags_str[0] == '\0' ||
		flags_str[1] == '\0' ||
		flags_str[2] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid range bound flags"),
				 errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));

	switch (flags_str[0])
	{
		case '[':
			flags |= RANGE_LB_INC;
			break;
		case '(':
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid range bound flags"),
					 errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
	}

	switch (flags_str[1])
	{
		case ']':
			flags |= RANGE_UB_INC;
			break;
		case ')':
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid range bound flags"),
					 errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
	}

	return flags;
}

/*
 * 解析范围输入
 *
 * 输入参数：
 *	string: 待解析的输入串
 * 输出参数：
 *	*flags: 接收标志位掩码
 *	*lbound_str: 接收 palloc 的下界串，无则 NULL
 *	*ubound_str: 接收 palloc 的上界串，无则 NULL
 *
 * 本函数多少仿照 rowtypes.c 中的 record_in。
 * 输入语法为：
 *	<range>   := EMPTY
 *			   | <lb-inc> <string>, <string> <ub-inc>
 *	<lb-inc>  := '[' | '('
 *	<ub-inc>  := ']' | ')'
 *
 * <range> 前后空白忽略。<string> 内空白按字面纳入
 * 该边界的输入字符串。
 *
 * 长度为 0 的 <string> 视为"无限"（即无界），除非
 * 被双引号包围，此时为字面空串。
 * 
 *
 * 在 <string> 内，特殊字符（逗号、括号等）可
 * 用双引号括起或以反斜杠转义。双引号内的双引号
 * 可用双引号或反斜杠转义。
 *
 * 成功返回真，失败返回假（仅 escontext 为
 * ErrorSaveContext时返回）。
 */
static bool
range_parse(const char *string, char *flags, char **lbound_str,
			char **ubound_str, Node *escontext)
{
	const char *ptr = string;
	bool		infinite;

	*flags = 0;

	/* 消耗空白 */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	/* 检查空范围 */
	if (pg_strncasecmp(ptr, RANGE_EMPTY_LITERAL,
					   strlen(RANGE_EMPTY_LITERAL)) == 0)
	{
		*flags = RANGE_EMPTY;
		*lbound_str = NULL;
		*ubound_str = NULL;

		ptr += strlen(RANGE_EMPTY_LITERAL);

		/* 其余应为空白 */
		while (*ptr != '\0' && isspace((unsigned char) *ptr))
			ptr++;

		/* 应已消耗一切 */
		if (*ptr != '\0')
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed range literal: \"%s\"",
							string),
					 errdetail("Junk after \"empty\" key word.")));

		return true;
	}

	if (*ptr == '[')
	{
		*flags |= RANGE_LB_INC;
		ptr++;
	}
	else if (*ptr == '(')
		ptr++;
	else
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Missing left parenthesis or bracket.")));

	ptr = range_parse_bound(string, ptr, lbound_str, &infinite, escontext);
	if (ptr == NULL)
		return false;
	if (infinite)
		*flags |= RANGE_LB_INF;

	if (*ptr == ',')
		ptr++;
	else
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Missing comma after lower bound.")));

	ptr = range_parse_bound(string, ptr, ubound_str, &infinite, escontext);
	if (ptr == NULL)
		return false;
	if (infinite)
		*flags |= RANGE_UB_INF;

	if (*ptr == ']')
	{
		*flags |= RANGE_UB_INC;
		ptr++;
	}
	else if (*ptr == ')')
		ptr++;
	else						/* 必为逗号 */
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Too many commas.")));

	/* 消耗空白 */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr != '\0')
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed range literal: \"%s\"",
						string),
				 errdetail("Junk after right parenthesis or bracket.")));

	return true;
}

/*
 * range_parse 辅助：解析并去引号一个边界串。
 *
 * 扫描至逗号、右圆括号或右方括号。
 *
 * 输入参数：
 *	string: 整个输入串（仅用于错误报告）
 *	ptr: 开始解析边界之处
 * 输出参数：
 *	*bound_str: 接收 palloc 的边界串，无则 NULL
 *	*infinite: 无界则真，否则假
 *
 * 返回值是越过边界串后的扫描 ptr。
 * 但若 escontext 为 ErrorSaveContext，失败返回 NULL。
 */
static const char *
range_parse_bound(const char *string, const char *ptr,
				  char **bound_str, bool *infinite, Node *escontext)
{
	StringInfoData buf;

	/* 检查空：全空输入表示 null */
	if (*ptr == ',' || *ptr == ')' || *ptr == ']')
	{
		*bound_str = NULL;
		*infinite = true;
	}
	else
	{
		/* 提取此边界的字符串 */
		bool		inquote = false;

		initStringInfo(&buf);
		while (inquote || !(*ptr == ',' || *ptr == ')' || *ptr == ']'))
		{
			char		ch = *ptr++;

			if (ch == '\0')
				ereturn(escontext, NULL,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed range literal: \"%s\"",
								string),
						 errdetail("Unexpected end of input.")));
			if (ch == '\\')
			{
				if (*ptr == '\0')
					ereturn(escontext, NULL,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed range literal: \"%s\"",
									string),
							 errdetail("Unexpected end of input.")));
				appendStringInfoChar(&buf, *ptr++);
			}
			else if (ch == '"')
			{
				if (!inquote)
					inquote = true;
				else if (*ptr == '"')
				{
					/* 引号序列内的成对引号 */
					appendStringInfoChar(&buf, *ptr++);
				}
				else
					inquote = false;
			}
			else
				appendStringInfoChar(&buf, ch);
		}

		*bound_str = buf.data;
		*infinite = false;
	}

	return ptr;
}

/*
 * 把反序列化的范围值转为文本形式
 *
 * 输入为标志字节，以及已转为文本（尚未加引号）的
 * 两边界值。若无边界值则传 NULL。
 *
 * 结果为 palloc 字符串
 */
static char *
range_deparse(char flags, const char *lbound_str, const char *ubound_str)
{
	StringInfoData buf;

	if (flags & RANGE_EMPTY)
		return pstrdup(RANGE_EMPTY_LITERAL);

	initStringInfo(&buf);

	appendStringInfoChar(&buf, (flags & RANGE_LB_INC) ? '[' : '(');

	if (RANGE_HAS_LBOUND(flags))
		appendStringInfoString(&buf, range_bound_escape(lbound_str));

	appendStringInfoChar(&buf, ',');

	if (RANGE_HAS_UBOUND(flags))
		appendStringInfoString(&buf, range_bound_escape(ubound_str));

	appendStringInfoChar(&buf, (flags & RANGE_UB_INC) ? ']' : ')');

	return buf.data;
}

/*
 * range_deparse 辅助：按需为边界值加引号
 *
 * 结果为 palloc 字符串
 */
static char *
range_bound_escape(const char *value)
{
	bool		nq;
	const char *ptr;
	StringInfoData buf;

	initStringInfo(&buf);

	/* 检测此值是否需双引号 */
	nq = (value[0] == '\0');	/* 空串强制加引号 */
	for (ptr = value; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"' || ch == '\\' ||
			ch == '(' || ch == ')' ||
			ch == '[' || ch == ']' ||
			ch == ',' ||
			isspace((unsigned char) ch))
		{
			nq = true;
			break;
		}
	}

	/*然后发出字符串*/
	if (nq)
		appendStringInfoChar(&buf, '"');
	for (ptr = value; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"' || ch == '\\')
			appendStringInfoChar(&buf, ch);
		appendStringInfoChar(&buf, ch);
	}
	if (nq)
		appendStringInfoChar(&buf, '"');

	return buf.data;
}

/*
 * 测试范围 r1 是否包含范围 r2。
 *
 * 调用方已确认二者为同一范围类型，并已查找必要的
 * typcache 条目。
 */
bool
range_contains_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	RangeBound	lower1;
	RangeBound	upper1;
	bool		empty1;
	RangeBound	lower2;
	RangeBound	upper2;
	bool		empty2;

	/* 不同类型应由 ANYRANGE 匹配规则阻止 */
	if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
		elog(ERROR, "range types do not match");

	range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
	range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

	/* 若任一范围为空则答案易得 */
	if (empty2)
		return true;
	else if (empty1)
		return false;

	/* 否则须 lower1<=lower2 且 upper1>=upper2 */
	if (range_cmp_bounds(typcache, &lower1, &lower2) > 0)
		return false;
	if (range_cmp_bounds(typcache, &upper1, &upper2) < 0)
		return false;

	return true;
}

bool
range_contained_by_internal(TypeCacheEntry *typcache, const RangeType *r1, const RangeType *r2)
{
	return range_contains_internal(typcache, r2, r1);
}

/*
 * 测试范围 r 是否包含特定元素值。
 */
bool
range_contains_elem_internal(TypeCacheEntry *typcache, const RangeType *r, Datum val)
{
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	int32		cmp;

	range_deserialize(typcache, r, &lower, &upper, &empty);

	if (empty)
		return false;

	if (!lower.infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  lower.val, val));
		if (cmp > 0)
			return false;
		if (cmp == 0 && !lower.inclusive)
			return false;
	}

	if (!upper.infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  upper.val, val));
		if (cmp < 0)
			return false;
		if (cmp == 0 && !upper.inclusive)
			return false;
	}

	return true;
}


/*
 * datum_compute_size() 与 datum_write() 用于把边界值
 * 插入范围对象。它们仿照 heaptuple.c 的
 * heap_compute_data_size() 与 heap_fill_tuple()，但此
 * 处无需处理空值。TYPE_IS_PACKABLE 须测试与
 * heaptuple.c 的 ATT_IS_PACKABLE 宏相同条件。详见彼处。
 * 
 */

/* 数据类型允许打包为 1 字节头 varlena 吗？ */
#define TYPE_IS_PACKABLE(typlen, typstorage) \
	((typlen) == -1 && (typstorage) != TYPSTORAGE_PLAIN)

/*
 * 按 datum 所需空间（含任何前置对齐填充）递增
 * data_length。
 */
static Size
datum_compute_size(Size data_length, Datum val, bool typbyval, char typalign,
				   int16 typlen, char typstorage)
{
	if (TYPE_IS_PACKABLE(typlen, typstorage) &&
		VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
	{
		/*
		 * 预期将转为短 varlena 头，故调整长度且不计
		 * 任何对齐
		 */
		data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
	}
	else
	{
		data_length = att_align_datum(data_length, typalign, typlen, val);
		data_length = att_addlength_datum(data_length, typlen, val);
	}

	return data_length;
}

/*
 * 从 ptr 起写入给定 datum（必要时先推进到正确
 * 对齐）。返回按所用空间递增后的指针。
 */
static Pointer
datum_write(Pointer ptr, Datum datum, bool typbyval, char typalign,
			int16 typlen, char typstorage)
{
	Size		data_length;

	if (typbyval)
	{
		/* 传值 */
		ptr = (char *) att_align_nominal(ptr, typalign);
		store_att_byval(ptr, datum, typlen);
		data_length = typlen;
	}
	else if (typlen == -1)
	{
		/* varlena */
		Pointer		val = DatumGetPointer(datum);

		if (VARATT_IS_EXTERNAL(val))
		{
			/*
			 * 抛错，因绝不可把 toast 指针放入范围对象。
			 * 调用方应已解 TOAST。
			 */
			elog(ERROR, "cannot store a toast pointer inside a range");
			data_length = 0;	/* 让编译器安静 */
		}
		else if (VARATT_IS_SHORT(val))
		{
			/* 短 varlena 无需对齐 */
			data_length = VARSIZE_SHORT(val);
			memcpy(ptr, val, data_length);
		}
		else if (TYPE_IS_PACKABLE(typlen, typstorage) &&
				 VARATT_CAN_MAKE_SHORT(val))
		{
			/* 转为短 varlena--无对齐 */
			data_length = VARATT_CONVERTED_SHORT_SIZE(val);
			SET_VARSIZE_SHORT(ptr, data_length);
			memcpy(ptr + 1, VARDATA(val), data_length - 1);
		}
		else
		{
			/* 完整 4 字节头 varlena */
			ptr = (char *) att_align_nominal(ptr, typalign);
			data_length = VARSIZE(val);
			memcpy(ptr, val, data_length);
		}
	}
	else if (typlen == -2)
	{
		/* cstring 从无需对齐 */
		Assert(typalign == TYPALIGN_CHAR);
		data_length = strlen(DatumGetCString(datum)) + 1;
		memcpy(ptr, DatumGetPointer(datum), data_length);
	}
	else
	{
		/* 定长传引用 */
		ptr = (char *) att_align_nominal(ptr, typalign);
		Assert(typlen > 0);
		data_length = typlen;
		memcpy(ptr, DatumGetPointer(datum), data_length);
	}

	ptr += data_length;

	return ptr;
}
