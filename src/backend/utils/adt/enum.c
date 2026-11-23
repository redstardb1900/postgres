/*-------------------------------------------------------------------------
 *
 * enum.c
 *	  枚举类型的 I/O、运算符、聚集等
 *
 * Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/enum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_enum.h"
#include "libpq/pqformat.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Oid	enum_endpoint(Oid enumtypoid, ScanDirection direction);
static ArrayType *enum_range_internal(Oid enumtypoid, Oid lower, Oid upper);


/*
 * 禁止使用未提交的 pg_enum 元组。
 *
 * 须确保未提交的枚举值不进入索引。若进入后又
 * 回滚了 pg_enum 的添加,索引会损坏,因无底层
 * pg_enum 项时值比较不可靠。注意:仅删除含枚举
 * 值的堆项不足以保证它不出现在索引上层。为此
 * 禁止未提交行用于任何 SQL 层用途。这比必要更
 * 严,因该值未必插入表中,或其列上可能无索引,
 * 但集中强制很容易。
 *
 *
 *
 * 但允许使用同事务内创建的枚举类型所属的未提交
 * 值,因那时相关索引也是新建的,回滚会一并消失。
 * 尚未完全实现该放宽,但允许自由使用 CREATE TYPE
 * AS ENUM 期间创建的枚举值,寿命与类型相同,此情形
 * 为 pg_restore -1 所需。ALTER TYPE ADD VALUE 所加
 * 值目前仍受限,但若能证明枚举类型在同事务更早
 * 创建则可放宽。注意比较元组 xmin 不行,因类型
 * 元组可能已在当前事务中更新。子事务也带来须
 * 计入的风险。
 *
 *
 *
 * 下列任何可能把枚举值返回给 SQL 操作的函数
 * 都须直接或间接调用本函数。
 */
static void
check_safe_enum_use(HeapTuple enumval_tup)
{
	TransactionId xmin;
	Form_pg_enum en = (Form_pg_enum) GETSTRUCT(enumval_tup);

	/*
	 * 若行已提示为已提交,则肯定安全。这为全部
	 * 正常用例提供快路径。
	 */
	if (HeapTupleHeaderXminCommitted(enumval_tup->t_data))
		return;

	/*
	 * 通常行在被读入或载入 syscache 时会得到已提交
	 * 提示;若未提示,则直接检查 xmin。
	 */
	xmin = HeapTupleHeaderGetXmin(enumval_tup->t_data);
	if (!TransactionIdIsInProgress(xmin) &&
		TransactionIdDidCommit(xmin))
		return;

	/*
	 * 检查枚举值是否未提交。若非未提交则安全,因其在
	 * CREATE TYPE AS ENUM 期间产生,寿命不短于所属
	 * 类型。他事务产生的值也会为假;但前面测试
	 * 应已处理那些情况。
	 */
	if (!EnumUncommitted(en->oid))
		return;

	/*
	 * 或许还可做其他测试以收窄不安全条件,
	 * 但目前直接抛异常。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_UNSAFE_NEW_ENUM_VALUE_USAGE),
			 errmsg("unsafe use of new value \"%s\" of enum type %s",
					NameStr(en->enumlabel),
					format_type_be(en->enumtypid)),
			 errhint("New enum values must be committed before they can be used.")));
}


/* 基本 I/O 支持 */

Datum
enum_in(PG_FUNCTION_ARGS)
{
	char	   *name = PG_GETARG_CSTRING(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Node	   *escontext = fcinfo->context;
	Oid			enumoid;
	HeapTuple	tup;

	/* 须检查长度以防 SearchSysCache 内 Assert 失败 */
	if (strlen(name) >= NAMEDATALEN)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/*
	 * 检查在 SQL 中使用是否安全。或许应以软方式报告
	 * "不安全使用";但不清楚是否值得费事,也不清楚
	 * 那究竟是正当的坏输入情形,还是实现上的
	 * 不足。
	 */
	check_safe_enum_use(tup);

	/*
	 * 来自 pg_enum.oid,把系统 oid 存入用户表。此
	 * oid 须由二进制升级保留。
	 */
	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	PG_RETURN_OID(enumoid);
}

Datum
enum_out(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	result = pstrdup(NameStr(en->enumlabel));

	ReleaseSysCache(tup);

	PG_RETURN_CSTRING(result);
}

/* 二进制 I/O */
Datum
enum_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Oid			enumoid;
	HeapTuple	tup;
	char	   *name;
	int			nbytes;

	name = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/* 须检查长度以防 SearchSysCache 内 Assert 失败 */
	if (strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/* 检查可否用于 SQL */
	check_safe_enum_use(tup);

	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	pfree(name);

	PG_RETURN_OID(enumoid);
}

Datum
enum_send(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	pq_begintypsend(&buf);
	pq_sendtext(&buf, NameStr(en->enumlabel), strlen(NameStr(en->enumlabel)));

	ReleaseSysCache(tup);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* 比较函数及相关 */

/*
 * enum_cmp_internal 是除 enum_eq/enum_ne 外全部可见
 * 比较函数的共用引擎;后两者可直接检查 OID
 * 相等。
 */
static int
enum_cmp_internal(Oid arg1, Oid arg2, FunctionCallInfo fcinfo)
{
	TypeCacheEntry *tcache;

	/*
	 * 除有希望少见的单方或双方 OID 为奇数外,不需要
	 * typcache。故对未把 flinfo 传给枚举比较函数的代码
	 * 做粗测可能发现不了疏漏。为使此类错误更明显,即便
	 * 走快路径退出也 Assert 有缓存
	 * 位置。
	 */
	Assert(fcinfo->flinfo != NULL);

	/* OID 相等则无论怎样都相等 */
	if (arg1 == arg2)
		return 0;

	/* 快路径:偶数 OID 已知比较正确 */
	if ((arg1 & 1) == 0 && (arg2 & 1) == 0)
	{
		if (arg1 < arg2)
			return -1;
		else
			return 1;
	}

	/* 定位该枚举类型的 typcache 项 */
	tcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (tcache == NULL)
	{
		HeapTuple	enum_tup;
		Form_pg_enum en;
		Oid			typeoid;

		/* 取含 arg1 的枚举类型 OID */
		enum_tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(arg1));
		if (!HeapTupleIsValid(enum_tup))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid internal value for enum: %u",
							arg1)));
		en = (Form_pg_enum) GETSTRUCT(enum_tup);
		typeoid = en->enumtypid;
		ReleaseSysCache(enum_tup);
		/* 定位并记住 typcache 项 */
		tcache = lookup_type_cache(typeoid, 0);
		fcinfo->flinfo->fn_extra = (void *) tcache;
	}

	/* 其余比较逻辑在 typcache.c */
	return compare_values_of_enum(tcache, arg1, arg2);
}

Datum
enum_lt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) < 0);
}

Datum
enum_le(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) <= 0);
}

Datum
enum_eq(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a == b);
}

Datum
enum_ne(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a != b);
}

Datum
enum_ge(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) >= 0);
}

Datum
enum_gt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) > 0);
}

Datum
enum_smaller(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) < 0 ? a : b);
}

Datum
enum_larger(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) > 0 ? a : b);
}

Datum
enum_cmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_INT32(enum_cmp_internal(a, b, fcinfo));
}

/* 枚举编程支持函数 */

/*
 * enum_endpoint: enum_first/enum_last 的共用代码
 */
static Oid
enum_endpoint(Oid enumtypoid, ScanDirection direction)
{
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Oid			minmax;

	/*
	 * 用 pg_enum_typid_sortorder_index 找首/末枚举成员。
	 * 注意不可用 syscache。详见 catalog/pg_enum.c 中
	 * RenumberEnumType 的注释。
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL,
										   1, &skey);

	enum_tuple = systable_getnext_ordered(enum_scan, direction);
	if (HeapTupleIsValid(enum_tuple))
	{
		/* 检查可否用于 SQL */
		check_safe_enum_use(enum_tuple);
		minmax = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;
	}
	else
	{
		/* 仅应发生在空枚举 */
		minmax = InvalidOid;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	return minmax;
}

Datum
enum_first(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			min;

	/*
	 * 依赖能从调用表达式树取得具体枚举类型。
	 * 注意完全不查看实参的实际值;尤其它可能
	 * 为 NULL。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* 用索引取 OID */
	min = enum_endpoint(enumtypoid, ForwardScanDirection);

	if (!OidIsValid(min))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(min);
}

Datum
enum_last(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			max;

	/*
	 * 依赖能从调用表达式树取得具体枚举类型。
	 * 注意完全不查看实参的实际值;尤其它可能
	 * 为 NULL。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* 用索引取 OID */
	max = enum_endpoint(enumtypoid, BackwardScanDirection);

	if (!OidIsValid(max))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(max);
}

/* enum_range 的双参数变体 */
Datum
enum_range_bounds(PG_FUNCTION_ARGS)
{
	Oid			lower;
	Oid			upper;
	Oid			enumtypoid;

	if (PG_ARGISNULL(0))
		lower = InvalidOid;
	else
		lower = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		upper = InvalidOid;
	else
		upper = PG_GETARG_OID(1);

	/*
	 * 依赖能从调用表达式树取得具体枚举类型。
	 * 通用类型机制应已确保两者类型
	 * 相同。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid, lower, upper));
}

/* enum_range 的单参数变体 */
Datum
enum_range_all(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;

	/*
	 * 依赖能从调用表达式树取得具体枚举类型。
	 * 注意完全不查看实参的实际值;尤其它可能
	 * 为 NULL。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid,
											  InvalidOid, InvalidOid));
}

static ArrayType *
enum_range_internal(Oid enumtypoid, Oid lower, Oid upper)
{
	ArrayType  *result;
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Datum	   *elems;
	int			max,
				cnt;
	bool		left_found;

	/*
	 * 用 pg_enum_typid_sortorder_index 按序扫描枚举成员。
	 * 注意不可用 syscache。详见 catalog/pg_enum.c 中
	 * RenumberEnumType 的注释。
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL, 1, &skey);

	max = 64;
	elems = (Datum *) palloc(max * sizeof(Datum));
	cnt = 0;
	left_found = !OidIsValid(lower);

	while (HeapTupleIsValid(enum_tuple = systable_getnext_ordered(enum_scan, ForwardScanDirection)))
	{
		Oid			enum_oid = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;

		if (!left_found && lower == enum_oid)
			left_found = true;

		if (left_found)
		{
			/* 检查可否用于 SQL */
			check_safe_enum_use(enum_tuple);

			if (cnt >= max)
			{
				max *= 2;
				elems = (Datum *) repalloc(elems, max * sizeof(Datum));
			}

			elems[cnt++] = ObjectIdGetDatum(enum_oid);
		}

		if (OidIsValid(upper) && upper == enum_oid)
			break;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	/* 并构建结果数组 */
	/* 注意此处写死了 Oid 表示的若干细节 */
	result = construct_array(elems, cnt, enumtypoid,
							 sizeof(Oid), true, TYPALIGN_INT);

	pfree(elems);

	return result;
}
