/*
 * brin_inclusion.c
 *		BRIN 的 inclusion 操作符类实现
 *
 * 本模块为 "inclusion"
 * 操作符类提供框架级 BRIN 支持函数。每个操作符类还需要
 * 若干 SQL 层支持函数。
 *
 * "inclusion" 这一 BRIN 策略适用于支持 R-Tree
 * 操作的类型。本实现把那些操作直接映射
 * 到 BRIN 的块范围特性上，但有两处例外：(a) 我们显式
 * 支持 "空" 元素：至少对范围类型，需要把
 * 空与常规 R-Tree 策略分开考虑；(b) 还需要
 * 考虑 "不可合并" 元素，即一组元素，它们的并
 * 没有表示形式。截至目前，唯一出现这种情况的
 * 是 INET 类型，其中 IPv6 值无法与 IPv4
 * 值合并。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_inclusion.c
 */
#include "postgres.h"

#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/genam.h"
#include "access/skey.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 额外的 SQL 层支持函数
 *
 * 过程编号不得使用 BRIN 自身保留的值；参见
 * brin_internal.h。
 */
#define		INCLUSION_MAX_PROCNUMS	4	/* 我们需要的支持过程上限 */
#define		PROCNUM_MERGE			11	/* 必需 */
#define		PROCNUM_MERGEABLE		12	/* 可选 */
#define		PROCNUM_CONTAINS		13	/* 可选 */
#define		PROCNUM_EMPTY			14	/* 可选 */


/*
 * 从 procnum 减去此值，得到 InclusionOpaque 数组中的下标
 * （必须等于私有过程编号的最小值）。
 */
#define		PROCNUM_BASE			11

/*-
 * bv_values 数组中存储的值分别对应：
 *
 * INCLUSION_UNION
 *		页范围中各值的并
 * INCLUSION_UNMERGEABLE
 *		页范围中的值是否无法合并
 *		（例如 IPv4 地址之中夹着一个 IPv6 地址）
 * INCLUSION_CONTAINS_EMPTY
 *		页范围内是否有任何元组
 *		含有空元素
 */
#define INCLUSION_UNION				0
#define INCLUSION_UNMERGEABLE		1
#define INCLUSION_CONTAINS_EMPTY	2


typedef struct InclusionOpaque
{
	FmgrInfo	extra_procinfos[INCLUSION_MAX_PROCNUMS];
	bool		extra_proc_missing[INCLUSION_MAX_PROCNUMS];
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[RTMaxStrategyNumber];
} InclusionOpaque;

static FmgrInfo *inclusion_get_procinfo(BrinDesc *bdesc, uint16 attno,
										uint16 procnum, bool missing_ok);
static FmgrInfo *inclusion_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno,
												 Oid subtype, uint16 strategynum);


/*
 * BRIN inclusion 的 OpcInfo 函数
 */
Datum
brin_inclusion_opcinfo(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);
	BrinOpcInfo *result;
	TypeCacheEntry *bool_typcache = lookup_type_cache(BOOLOID, 0);

	/*
	 * opaque 的所有成员都惰性初始化；两个 procinfo 数组
	 * 起初都未初始化：fn_oid 为 InvalidOid，并且
	 * "missing" 为 false，这里靠清零做到。strategy_procinfos 元素可
	 * 在 cached_subtype 变化时把 fn_oid 清零而失效。
	 * extra_procinfo 条目从不失效，但若查找失败
	 * （这是预期的），就把 extra_proc_missing 设为 true，表示不要
	 * 再查找它。
	 */
	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(3)) + sizeof(InclusionOpaque));
	result->oi_nstored = 3;
	result->oi_regular_nulls = true;
	result->oi_opaque = (InclusionOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(3));

	/* 并 */
	result->oi_typcache[INCLUSION_UNION] =
		lookup_type_cache(typoid, 0);

	/* 含有不可合并的元素 */
	result->oi_typcache[INCLUSION_UNMERGEABLE] = bool_typcache;

	/* 含有空元素 */
	result->oi_typcache[INCLUSION_CONTAINS_EMPTY] = bool_typcache;

	PG_RETURN_POINTER(result);
}

/*
 * BRIN inclusion 的 add value 函数
 *
 * 检查给定索引元组（它含有某个
 * 页范围的部分状态）：把它与来自另一个堆
 * 元组的给定值相比较。若新值落在现有
 * 元组值所指定的并之外，则更新索引元组并返回 true。否则，返回
 * false，且在这种情况下不做修改。
 */
Datum
brin_inclusion_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_BOOL(3);
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *finfo;
	Datum		result;
	bool		new = false;
	AttrNumber	attno;
	Form_pg_attribute attr;

	Assert(!isnull);

	attno = column->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/*
	 * 若已记录的值为空，则复制新值（已知为
	 * 非空），至此基本完成。
	 */
	if (column->bv_allnulls)
	{
		column->bv_values[INCLUSION_UNION] =
			datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(false);
		column->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(false);
		column->bv_allnulls = false;
		new = true;
	}

	/*
	 * 若该页范围已标记为
	 * 含有不可合并的值，则无需继续处理。
	 */
	if (DatumGetBool(column->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_BOOL(false);

	/*
	 * 若操作符类支持空元素这一概念，则检测传入的
	 * 新值是否为空；若返回 true，就需要设置
	 * 元素上的 "含空" 标志（除非已经设置）。
	 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_EMPTY, true);
	if (finfo != NULL && DatumGetBool(FunctionCall1Coll(finfo, colloid, newval)))
	{
		if (!DatumGetBool(column->bv_values[INCLUSION_CONTAINS_EMPTY]))
		{
			column->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(true);
			PG_RETURN_BOOL(true);
		}

		PG_RETURN_BOOL(false);
	}

	if (new)
		PG_RETURN_BOOL(true);

	/* 检查新值是否已被包含。 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_CONTAINS, true);
	if (finfo != NULL &&
		DatumGetBool(FunctionCall2Coll(finfo, colloid,
									   column->bv_values[INCLUSION_UNION],
									   newval)))
		PG_RETURN_BOOL(false);

	/*
	 * 检查新值能否合并进现有的并。若不能，
	 * 就把该值标为含有不可合并元素，然后返回。
	 *
	 * 注意：此时可以把该值从并中去掉，因为
	 * 它以后不会再被使用。不过 BRIN 框架
	 * 不允许该值不存在。将来再改进。
	 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGEABLE, true);
	if (finfo != NULL &&
		!DatumGetBool(FunctionCall2Coll(finfo, colloid,
										column->bv_values[INCLUSION_UNION],
										newval)))
	{
		column->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_BOOL(true);
	}

	/* 最后，把新值合并进现有的并。 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGE, false);
	result = FunctionCall2Coll(finfo, colloid,
							   column->bv_values[INCLUSION_UNION], newval);
	if (!attr->attbyval &&
		DatumGetPointer(result) != DatumGetPointer(column->bv_values[INCLUSION_UNION]))
	{
		pfree(DatumGetPointer(column->bv_values[INCLUSION_UNION]));

		if (result == newval)
			result = datumCopy(result, attr->attbyval, attr->attlen);
	}
	column->bv_values[INCLUSION_UNION] = result;

	PG_RETURN_BOOL(true);
}

/*
 * BRIN inclusion 的 consistent 函数
 *
 * consistent 函数里不再处理 NULL 键，那现在
 * 由 AM 代码处理。因此也不应遇到全 NULL 页范围，
 * 因为它们不可能与普通（非 [IS] NULL）键一致。
 *
 * 所有策略都是可选的。
 */
Datum
brin_inclusion_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	Datum		unionval;
	AttrNumber	attno;
	Datum		query;
	FmgrInfo   *finfo;
	Datum		result;

	/* 此操作符类使用只有三个参数的旧签名。 */
	Assert(PG_NARGS() == 3);

	/* 不应处理全 NULL 页范围。 */
	Assert(!column->bv_allnulls);

	/* 必须检查它是否含有不可合并的元素。 */
	if (DatumGetBool(column->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_BOOL(true);

	attno = key->sk_attno;
	subtype = key->sk_subtype;
	query = key->sk_argument;
	unionval = column->bv_values[INCLUSION_UNION];
	switch (key->sk_strategy)
	{
			/*
			 * 位置策略
			 *
			 * 这些策略通过对逆位置操作符的结果
 * 做逻辑否定来实现；为此，逆操作符
 * 必须属于该操作符类。若所需策略
 * 不属于该操作符类，inclusion_get_strategy_procinfo()
 * 就会报错。
			 *
			 * 任一参数为空时它们都返回 false，因此
			 * 无需检查空元素。
			 */

		case RTLeftStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverLeftStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverRightStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTRightStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTBelowStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverAboveStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverBelowStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTAboveStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverAboveStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTBelowStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTAboveStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverBelowStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

			/*
			 * 重叠与包含策略
			 *
			 * 这些策略足够简单，直接调用
			 * 操作符并返回其结果即可。空元素不会改变
			 * 结果。
			 */

		case RTOverlapStrategyNumber:
		case RTContainsStrategyNumber:
		case RTContainsElemStrategyNumber:
		case RTSubStrategyNumber:
		case RTSubEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													key->sk_strategy);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_DATUM(result);

			/*
			 * 被包含策略
			 *
			 * 对 "被包含" 策略无法直接调用原来的操作符，
			 * 因为即使并本身不被包含，某些元素仍可能被包含；
			 * 因此改用重叠操作符。
			 *
			 * 空元素要单独检查：它们不会被合并
			 * 进并，但会被任何值包含。
			 */

		case RTContainedByStrategyNumber:
		case RTSuperStrategyNumber:
		case RTSuperEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverlapStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

			/*
			 * 相邻策略
			 *
			 * 我们先检测重叠，但为稳妥起见
			 * 还要调用真正的相邻操作符。
			 *
			 * 空元素不可能与任何其他元素相邻，因此
			 * 无需检查它。
			 */

		case RTAdjacentStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverlapStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTAdjacentStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_DATUM(result);

			/*
			 * 基本比较策略
			 *
			 * 用包含操作符支持相等策略很直接。
			 * 一般而言，不等策略对将要配合
			 * inclusion 这一 BRIN 操作符类族使用的类型
			 * 意义不大，但仍可以用位于左侧
			 * 与位于右侧操作符的逻辑否定来
			 * 实现它们。
			 *
			 * 注意：这些策略无法用于按面积比较的几何数据类型！
			 * 唯一例外是 "same"
			 * 策略。
			 *
			 * 空元素被视为小于其他元素。我们
			 * 无法用 empty 支持函数来检查查询值是否为
			 * 空元素，因为查询值的数据类型可能不同于
			 * empty 支持函数的参数。因此只要空元素
			 * 有可能改变
			 * 结果，我们就返回 true。
			 */

		case RTLessStrategyNumber:
		case RTLessEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (!DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTSameStrategyNumber:
		case RTEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTContainsStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTGreaterEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (!DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTGreaterStrategyNumber:
			/* 无需检查空元素 */
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		default:
			/* 不应发生 */
			elog(ERROR, "invalid strategy number %d", key->sk_strategy);
			PG_RETURN_BOOL(false);
	}
}

/*
 * BRIN inclusion 的 union 函数
 *
 * 给定两个 BrinValues，把第一个更新为二者所含汇总
 * 值的并。第二个保持不动。
 */
Datum
brin_inclusion_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Form_pg_attribute attr;
	FmgrInfo   *finfo;
	Datum		result;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	attno = col_a->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/* 若 B 含有空元素，则在需要时把 A 同样标上。 */
	if (!DatumGetBool(col_a->bv_values[INCLUSION_CONTAINS_EMPTY]) &&
		DatumGetBool(col_b->bv_values[INCLUSION_CONTAINS_EMPTY]))
		col_a->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(true);

	/* 检查 A 是否含有不可合并的元素。 */
	if (DatumGetBool(col_a->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_VOID();

	/* 若 B 含有不可合并的元素，则把 A 同样标上。 */
	if (DatumGetBool(col_b->bv_values[INCLUSION_UNMERGEABLE]))
	{
		col_a->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_VOID();
	}

	/* 检查 A 与 B 能否合并；若不能，则把 A 标为不可合并。 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGEABLE, true);
	if (finfo != NULL &&
		!DatumGetBool(FunctionCall2Coll(finfo, colloid,
										col_a->bv_values[INCLUSION_UNION],
										col_b->bv_values[INCLUSION_UNION])))
	{
		col_a->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_VOID();
	}

	/* 最后，把 B 合并进 A。 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGE, false);
	result = FunctionCall2Coll(finfo, colloid,
							   col_a->bv_values[INCLUSION_UNION],
							   col_b->bv_values[INCLUSION_UNION]);
	if (!attr->attbyval &&
		DatumGetPointer(result) != DatumGetPointer(col_a->bv_values[INCLUSION_UNION]))
	{
		pfree(DatumGetPointer(col_a->bv_values[INCLUSION_UNION]));

		if (result == col_b->bv_values[INCLUSION_UNION])
			result = datumCopy(result, attr->attbyval, attr->attlen);
	}
	col_a->bv_values[INCLUSION_UNION] = result;

	PG_RETURN_VOID();
}

/*
 * 缓存并返回 inclusion 操作符类的支持过程
 *
 * 返回给定支持函数编号对应的过程；
 * 若不存在则返回 null。若 missing_ok 为 true 且该过程
 * 未在此操作符类上设置，则返回 NULL，而不是报错。
 */
static FmgrInfo *
inclusion_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum,
					   bool missing_ok)
{
	InclusionOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * 我们把这些缓存在 opaque 结构中，以避免反复的系统缓存
	 * 查找。
	 */
	opaque = (InclusionOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * 若已经查找过该过程且未找到，就不必
	 * 再查找。
	 */
	if (opaque->extra_proc_missing[basenum])
		return NULL;

	if (opaque->extra_procinfos[basenum].fn_oid == InvalidOid)
	{
		if (RegProcedureIsValid(index_getprocid(bdesc->bd_index, attno,
												procnum)))
			fmgr_info_copy(&opaque->extra_procinfos[basenum],
						   index_getprocinfo(bdesc->bd_index, attno, procnum),
						   bdesc->bd_context);
		else
		{
			if (!missing_ok)
				ereport(ERROR,
						errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						errmsg_internal("invalid opclass definition"),
						errdetail_internal("The operator class is missing support function %d for column %d.",
										   procnum, attno));

			opaque->extra_proc_missing[basenum] = true;
			return NULL;
		}
	}

	return &opaque->extra_procinfos[basenum];
}

/*
 * 缓存并返回给定策略对应的过程
 *
 * 返回给定子类型和策略
 * 编号对应的过程。索引的数据类型用作操作符的左侧，
 * 给定子类型用作右侧。
 * 若 pg_amop 行不存在则报错，但配置正确的操作符类
 * 不应出现这种情况。
 *
 * 当操作符类的数据类型与
 * 列或表达式的数据类型不同时总会报错。这发生在
 * 列数据类型到操作符类数据类型存在隐式转换时。我们不
 * 费心做类型转换，因为把存储数据类型设成操作符类的
 * 类型就能轻易避免。右侧数据类型没有同样的问题，因为
 * ScanKey 中的类型
 * 总是与操作符类的类型一致。
 *
 * 注意：本函数与 minmax_get_strategy_procinfo 结构相同；若此处
 * 有改动，也要看那个函数。
 */
static FmgrInfo *
inclusion_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
								uint16 strategynum)
{
	InclusionOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= RTMaxStrategyNumber);

	opaque = (InclusionOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * 我们把上一子类型的过程缓存在 opaque 结构中，以
	 * 避免反复的系统缓存查找。若子类型变了，
	 * 就使全部缓存条目失效。
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= RTMaxStrategyNumber; i++)
			opaque->strategy_procinfos[i - 1].fn_oid = InvalidOid;
		opaque->cached_subtype = subtype;
	}

	if (opaque->strategy_procinfos[strategynum - 1].fn_oid == InvalidOid)
	{
		Form_pg_attribute attr;
		HeapTuple	tuple;
		Oid			opfamily,
					oprid;

		opfamily = bdesc->bd_index->rd_opfamily[attno - 1];
		attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);
		tuple = SearchSysCache4(AMOPSTRATEGY, ObjectIdGetDatum(opfamily),
								ObjectIdGetDatum(attr->atttypid),
								ObjectIdGetDatum(subtype),
								Int16GetDatum(strategynum));

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategynum, attr->atttypid, subtype, opfamily);

		oprid = DatumGetObjectId(SysCacheGetAttrNotNull(AMOPSTRATEGY, tuple,
														Anum_pg_amop_amopopr));
		ReleaseSysCache(tuple);
		Assert(RegProcedureIsValid(oprid));

		fmgr_info_cxt(get_opcode(oprid),
					  &opaque->strategy_procinfos[strategynum - 1],
					  bdesc->bd_context);
	}

	return &opaque->strategy_procinfos[strategynum - 1];
}
