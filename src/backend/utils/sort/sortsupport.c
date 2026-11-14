/*-------------------------------------------------------------------------
 *
 * sortsupport.c
 *	  加速排序的支持例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/sortsupport.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"


/* 将旧式比较函数用作排序比较器所需的信息 */
typedef struct
{
	FmgrInfo	flinfo;			/* 用于查找比较函数的数据 */
	FunctionCallInfoBaseData fcinfo;	/* 可复用 callinfo 结构 */
} SortShimExtra;

#define SizeForSortShimExtra(nargs) (offsetof(SortShimExtra, fcinfo) + SizeForFunctionCallInfo(nargs))

/*
 * 调用旧式比较器的垫片函数
 *
 * 这实质上是 FunctionCall2Coll() 的内联版本，只不过
 * 我们假定 FunctionCallInfoBaseData 多半已设置好，见
 * PrepareSortSupportComparisonShim
 */
static int
comparison_shim(Datum x, Datum y, SortSupport ssup)
{
	SortShimExtra *extra = (SortShimExtra *) ssup->ssup_extra;
	Datum		result;

	extra->fcinfo.args[0].value = x;
	extra->fcinfo.args[1].value = y;

	/* 出于稳妥起见，我们每次都重置 isnull */
	extra->fcinfo.isnull = false;

	result = FunctionCallInvoke(&extra->fcinfo);

	/* 检查空结果，因为调用者显然不期望 NULL */
	if (extra->fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", extra->flinfo.fn_oid);

	return result;
}

/*
 * 设置垫片函数，以便将旧式 btree 比较函数
 * 当作排序支持比较器来使用。
 */
void
PrepareSortSupportComparisonShim(Oid cmpFunc, SortSupport ssup)
{
	SortShimExtra *extra;

	extra = (SortShimExtra *) MemoryContextAlloc(ssup->ssup_cxt,
												 SizeForSortShimExtra(2));

	/* 查找比较函数 */
	fmgr_info_cxt(cmpFunc, &extra->flinfo, ssup->ssup_cxt);

	/* callinfo 只需初始化一次即可复用 */
	InitFunctionCallInfoData(extra->fcinfo, &extra->flinfo, 2,
							 ssup->ssup_collation, NULL, NULL);
	extra->fcinfo.args[0].isnull = false;
	extra->fcinfo.args[1].isnull = false;

	ssup->ssup_extra = extra;
	ssup->comparator = comparison_shim;
}

/*
 * 查找并调用 sortsupport 函数来设置 SortSupport 比较器；
 * 若该函数不存在或拒绝设置相应的状态，
 * 则准备合适的垫片。
 */
static void
FinishSortSupportFunction(Oid opfamily, Oid opcintype, SortSupport ssup)
{
	Oid			sortSupportFunction;

	/* 查找排序支持函数 */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											BTSORTSUPPORT_PROC);
	if (OidIsValid(sortSupportFunction))
	{
		/*
		 * 排序支持函数可以提供比较器，但它也可以
		 * 选择不提供（如基于所选的排序规则）。
		 */
		OidFunctionCall1(sortSupportFunction, PointerGetDatum(ssup));
	}

	if (ssup->comparator == NULL)
	{
		Oid			sortFunction;

		sortFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
										 BTORDER_PROC);

		if (!OidIsValid(sortFunction))
			elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
				 BTORDER_PROC, opcintype, opcintype, opfamily);

		/* 我们将使用垫片调用旧式 btree 比较器 */
		PrepareSortSupportComparisonShim(sortFunction, ssup);
	}
}

/*
 * 根据排序算子（btree "<" 或 ">" 算子）填写 SortSupport。
 *
 * 调用者必须已将 SortSupportData 结构清零，然后填入
 * ssup_cxt、ssup_collation 和 ssup_nulls_first。这将
 * 填入 ssup_reverse 以及比较器函数指针。
 */
void
PrepareSortSupportFromOrderingOp(Oid orderingOp, SortSupport ssup)
{
	Oid			opfamily;
	Oid			opcintype;
	int16		strategy;

	Assert(ssup->comparator == NULL);

	/* 在 pg_amop 中查找算子 */
	if (!get_ordering_op_properties(orderingOp, &opfamily, &opcintype,
									&strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 orderingOp);
	ssup->ssup_reverse = (strategy == BTGreaterStrategyNumber);

	FinishSortSupportFunction(opfamily, opcintype, ssup);
}

/*
 * 根据索引关系、属性和策略填写 SortSupport。
 *
 * 调用者必须已将 SortSupportData 结构清零，然后填入
 * ssup_cxt、ssup_attno、ssup_collation 和 ssup_nulls_first。这将
 * 会填入 ssup_reverse（依据给定的策略）以及
 * 比较器函数指针。
 */
void
PrepareSortSupportFromIndexRel(Relation indexRel, int16 strategy,
							   SortSupport ssup)
{
	Oid			opfamily = indexRel->rd_opfamily[ssup->ssup_attno - 1];
	Oid			opcintype = indexRel->rd_opcintype[ssup->ssup_attno - 1];

	Assert(ssup->comparator == NULL);

	if (indexRel->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "unexpected non-btree AM: %u", indexRel->rd_rel->relam);
	if (strategy != BTGreaterStrategyNumber &&
		strategy != BTLessStrategyNumber)
		elog(ERROR, "unexpected sort support strategy: %d", strategy);
	ssup->ssup_reverse = (strategy == BTGreaterStrategyNumber);

	FinishSortSupportFunction(opfamily, opcintype, ssup);
}

/*
 * 根据 GiST 索引关系填写 SortSupport
 *
 * 调用者必须已将 SortSupportData 结构清零，然后填入
 * ssup_cxt、ssup_attno、ssup_collation 和 ssup_nulls_first。这将
 * 会填入 ssup_reverse（GiST 索引构建时恒为假）以及
 * 比较器函数指针。
 */
void
PrepareSortSupportFromGistIndexRel(Relation indexRel, SortSupport ssup)
{
	Oid			opfamily = indexRel->rd_opfamily[ssup->ssup_attno - 1];
	Oid			opcintype = indexRel->rd_opcintype[ssup->ssup_attno - 1];
	Oid			sortSupportFunction;

	Assert(ssup->comparator == NULL);

	if (indexRel->rd_rel->relam != GIST_AM_OID)
		elog(ERROR, "unexpected non-gist AM: %u", indexRel->rd_rel->relam);
	ssup->ssup_reverse = false;

	/*
	 * 查找排序支持函数。此处比 B-tree 索引更简单，
	 * 因为我们不支持旧式 btree 比较器。
	 */
	sortSupportFunction = get_opfamily_proc(opfamily, opcintype, opcintype,
											GIST_SORTSUPPORT_PROC);
	if (!OidIsValid(sortSupportFunction))
		elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
			 GIST_SORTSUPPORT_PROC, opcintype, opcintype, opfamily);
	OidFunctionCall1(sortSupportFunction, PointerGetDatum(ssup));
}
