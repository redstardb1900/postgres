/*
 * brin_minmax.c
 *		BRIN 的 Min/Max 操作符类实现
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_minmax.c
 */
#include "postgres.h"

#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/genam.h"
#include "access/stratnum.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct MinmaxOpaque
{
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[BTMaxStrategyNumber];
} MinmaxOpaque;

static FmgrInfo *minmax_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno,
											  Oid subtype, uint16 strategynum);


Datum
brin_minmax_opcinfo(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos 惰性初始化；这里由 palloc0 置为
	 * 全部未初始化，它会把 fn_oid 设为 InvalidOid。
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(2)) +
					 sizeof(MinmaxOpaque));
	result->oi_nstored = 2;
	result->oi_regular_nulls = true;
	result->oi_opaque = (MinmaxOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(2));
	result->oi_typcache[0] = result->oi_typcache[1] =
		lookup_type_cache(typoid, 0);

	PG_RETURN_POINTER(result);
}

/*
 * 检查给定索引元组（它含有某个页范围的部分状态）：
 * 把它与来自另一个堆元组的给定值相比较。若新值超出
 * 现有元组值所指定的 minmax 值域，则更新该索引元组
 * 并返回 true。否则在这种情况下返回 false，
 * 且不做修改。
 */
Datum
brin_minmax_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(3);
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *cmpFn;
	Datum		compar;
	bool		updated = false;
	Form_pg_attribute attr;
	AttrNumber	attno;

	Assert(!isnull);

	attno = column->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/*
	 * 若已记录的值为空，则把新值（已知为非空）同时存为
	 * 最小值和最大值，至此完成。
	 */
	if (column->bv_allnulls)
	{
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_allnulls = false;
		PG_RETURN_BOOL(true);
	}

	/*
	 * 否则，需要把新值与现有边界比较，
	 * 并相应更新。先检查它是否小于
	 * 现有最小值。
	 */
	cmpFn = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTLessStrategyNumber);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[0]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[0]));
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	/*
	 * 现在把它与现有最大值比较。
	 */
	cmpFn = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTGreaterStrategyNumber);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[1]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[1]));
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	PG_RETURN_BOOL(updated);
}

/*
 * 给定对应于某个页范围的索引元组和一个扫描键，
 * 返回该扫描键是否与索引元组的 minmax 值一致。
 * 若一致则返回 true，否则返回 false。
 *
 * consistent 函数里不再处理 NULL 键，那现在由 AM 代码
 * 处理。因此也不应遇到全 NULL 页范围，因为它们不可能
 * 与普通（非 [IS] NULL）键一致。
 */
Datum
brin_minmax_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	AttrNumber	attno;
	Datum		value;
	Datum		matches;
	FmgrInfo   *finfo;

	/* 此操作符类使用只有三个参数的旧签名。 */
	Assert(PG_NARGS() == 3);

	/* 不应处理全 NULL 页范围。 */
	Assert(!column->bv_allnulls);

	attno = key->sk_attno;
	subtype = key->sk_subtype;
	value = key->sk_argument;
	switch (key->sk_strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 key->sk_strategy);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[0],
										value);
			break;
		case BTEqualStrategyNumber:

			/*
			 * 在相等情形（WHERE col = someval）下，若页范围内的
			 * 最小值 <= 扫描键，且最大值 >= 扫描键，
			 * 就要返回当前页范围。
			 */
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 BTLessEqualStrategyNumber);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[0],
										value);
			if (!DatumGetBool(matches))
				break;
			/* max() >= 扫描键 */
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 BTGreaterEqualStrategyNumber);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[1],
										value);
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 key->sk_strategy);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[1],
										value);
			break;
		default:
			/* 不应发生 */
			elog(ERROR, "invalid strategy number %d", key->sk_strategy);
			matches = 0;
			break;
	}

	PG_RETURN_DATUM(matches);
}

/*
 * 给定两个 BrinValues，把第一个更新为二者所含汇总信息
 * 的并。第二个保持不动。
 */
Datum
brin_minmax_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Form_pg_attribute attr;
	FmgrInfo   *finfo;
	bool		needsadj;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	attno = col_a->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/* 若 B 的最小值小于 A 的最小值，则调整最小值 */
	finfo = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTLessStrategyNumber);
	needsadj = FunctionCall2Coll(finfo, colloid, col_b->bv_values[0],
								 col_a->bv_values[0]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[0]));
		col_a->bv_values[0] = datumCopy(col_b->bv_values[0],
										attr->attbyval, attr->attlen);
	}

	/* 若 B 的最大值大于 A 的最大值，则调整最大值 */
	finfo = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTGreaterStrategyNumber);
	needsadj = FunctionCall2Coll(finfo, colloid, col_b->bv_values[1],
								 col_a->bv_values[1]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[1]));
		col_a->bv_values[1] = datumCopy(col_b->bv_values[1],
										attr->attbyval, attr->attlen);
	}

	PG_RETURN_VOID();
}

/*
 * 缓存并返回给定策略对应的过程。
 *
 * 注意：本函数与 inclusion_get_strategy_procinfo 结构相同；参见
 * 那里的注释。若此处有改动，也要看那个函数。
 */
static FmgrInfo *
minmax_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
							 uint16 strategynum)
{
	MinmaxOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= BTMaxStrategyNumber);

	opaque = (MinmaxOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * 我们把上一子类型的过程缓存在 opaque 结构中，
	 * 以避免反复的系统缓存查找。若子类型变了，
	 * 就使全部缓存条目失效。
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= BTMaxStrategyNumber; i++)
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
