/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  针对系统目录缓存中常见查询的便利例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/lsyscache.c
 *
 * NOTES
 *	  索引信息最终也应经由这里处理。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "bootstrap/bootstrap.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* 供插件在 get_attavgwidth() 中接管的钩子 */
get_attavgwidth_hook_type get_attavgwidth_hook = NULL;


/* 				---------- AMOP 缓存 ---------- */

/*
 * op_in_opfamily
 *
 *		当算子 'opno' 属于算子族 'opfamily' 时返回 t。
 *
 * 本函数仅考虑搜索算子，而不考虑排序算子。
 */
bool
op_in_opfamily(Oid opno, Oid opfamily)
{
	return SearchSysCacheExists3(AMOPOPID,
								 ObjectIdGetDatum(opno),
								 CharGetDatum(AMOP_SEARCH),
								 ObjectIdGetDatum(opfamily));
}

/*
 * get_op_opfamily_strategy
 *
 *		取得该算子在指定算子族内的策略号，
 *		若非该算子族成员则返回 0。
 *
 * 本函数仅考虑搜索算子，而不考虑排序算子。
 */
int
get_op_opfamily_strategy(Oid opno, Oid opfamily)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	int			result;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(AMOP_SEARCH),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		return 0;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopstrategy;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_opfamily_sortfamily
 *
 *		若该算子是指定算子族内的排序算子，
 *		则返回其 amopsortfamily OID，否则 InvalidOid。
 */
Oid
get_op_opfamily_sortfamily(Oid opno, Oid opfamily)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	Oid			result;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(AMOP_ORDER),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopsortfamily;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_opfamily_properties
 *
 *		取得该算子在指定算子族内的策略号与所声明
 *		的输入数据类型。
 *
 * 调用者应当已验证 opno 是 opfamily 的成员，
 * 因此若未找到元组则报错。
 */
void
get_op_opfamily_properties(Oid opno, Oid opfamily, bool ordering_op,
						   int *strategy,
						   Oid *lefttype,
						   Oid *righttype)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(ordering_op ? AMOP_ORDER : AMOP_SEARCH),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "operator %u is not a member of opfamily %u",
			 opno, opfamily);
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	*strategy = amop_tup->amopstrategy;
	*lefttype = amop_tup->amoplefttype;
	*righttype = amop_tup->amoprighttype;
	ReleaseSysCache(tp);
}

/*
 * get_opfamily_member
 *		取得对指定算子族以指定数据类型实现指定策略
 *		的算子的 OID。
 *
 * 若给定键没有对应 pg_amop 条目，则返回 InvalidOid。
 */
Oid
get_opfamily_member(Oid opfamily, Oid lefttype, Oid righttype,
					int16 strategy)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	Oid			result;

	tp = SearchSysCache4(AMOPSTRATEGY,
						 ObjectIdGetDatum(opfamily),
						 ObjectIdGetDatum(lefttype),
						 ObjectIdGetDatum(righttype),
						 Int16GetDatum(strategy));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopopr;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_ordering_op_properties
 *		给定排序算子（btree 的 "<" 或 ">" 算子）的 OID，
 *		确定其算子族、声明的输入数据类型以及
 *		策略号（BTLessStrategyNumber 或 BTGreaterStrategyNumber）。
 *
 * 成功则返回 true；无匹配 pg_amop 条目则返回 false。
 * （这表明该算子不是有效的排序算子。）
 *
 * 注意：该算子可能注册在多个算子族中，例如
 * 若有人构建"反向排序"算子族。这会导致
 * "ORDER BY USING op" 默认取 NULLS FIRST 还是
 * NULLS LAST 拿不准，也会因无法匹配本应相同的
 * pathkey 而低效规划。因此希望这里得到确定的结果。
 * 由于 syscache 搜索的工作方式，我们将采用 OID 最小的
 * 算子族的解释，这大概足够确定了。
 * 既然已没有什么特别的理由构建反向排序算子族，
 * 为保障一致性而额外花费精力，似乎并不
 * 值得。
 */
bool
get_ordering_op_properties(Oid opno,
						   Oid *opfamily, Oid *opcintype, int16 *strategy)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* 失败时确保输出已初始化 */
	*opfamily = InvalidOid;
	*opcintype = InvalidOid;
	*strategy = 0;

	/*
	 * 在 pg_amop 中搜索目标算子是否被注册为某
	 * btree 算子族的 "<"或">" 算子。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* 必为 btree */
		if (aform->amopmethod != BTREE_AM_OID)
			continue;

		if (aform->amopstrategy == BTLessStrategyNumber ||
			aform->amopstrategy == BTGreaterStrategyNumber)
		{
			/* 找到了……且应有一致的输入类型 */
			if (aform->amoplefttype == aform->amoprighttype)
			{
				/* 找到合适算子族，返回信息 */
				*opfamily = aform->amopfamily;
				*opcintype = aform->amoplefttype;
				*strategy = aform->amopstrategy;
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_equality_op_for_ordering_op
 *		取得与排序算子（"<"或">"算子）相关联的
 *		数据类型专属 btree 相等算子的 OID。
 *
 * 若 "reverse" 非 NULL，设置 *reverse：算子为 "<" 则为 false、
 * ">" 时为 true
 *
 * 若找不到匹配的相等算子，则返回 InvalidOid。
 * （这表明该算子不是有效的排序算子。）
 */
Oid
get_equality_op_for_ordering_op(Oid opno, bool *reverse)
{
	Oid			result = InvalidOid;
	Oid			opfamily;
	Oid			opcintype;
	int16		strategy;

	/* 在 pg_amop 中查找算子 */
	if (get_ordering_op_properties(opno,
								   &opfamily, &opcintype, &strategy))
	{
		/* 找到合适的算子族，取得匹配的相等算子 */
		result = get_opfamily_member(opfamily,
									 opcintype,
									 opcintype,
									 BTEqualStrategyNumber);
		if (reverse)
			*reverse = (strategy == BTGreaterStrategyNumber);
	}

	return result;
}

/*
 * get_ordering_op_for_equality_op
 *		取得与某相等算子相关联的数据类型专属
 *		btree 排序算子的 OID。（若有多种可能，
 *		则假定任一均可。）
 *
 * 本函数用于在去重前必须先对数据排序、且不在乎
 * 用哪个排序算子的场合，只要它与目标相等算子
 * 兼容即可。由于需要的是排序算子，因此即便给定
 * 的算子是跨类型的，它也应是单一数据类型的。
 * 由调用者指定要为 LHS 还是 RHS 数据类型查找算子。
 *
 * 若找不到匹配的排序算子，则返回 InvalidOid。
 */
Oid
get_ordering_op_for_equality_op(Oid opno, bool use_lhs_type)
{
	Oid			result = InvalidOid;
	CatCList   *catlist;
	int			i;

	/*
	 * 在 pg_amop 中搜索目标算子是否被注册为某
	 * btree 算子族的"=" 算子。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* 必为 btree */
		if (aform->amopmethod != BTREE_AM_OID)
			continue;

		if (aform->amopstrategy == BTEqualStrategyNumber)
		{
			/* 找到合适的算子族，取得匹配的排序算子 */
			Oid			typid;

			typid = use_lhs_type ? aform->amoplefttype : aform->amoprighttype;
			result = get_opfamily_member(aform->amopfamily,
										 typid, typid,
										 BTLessStrategyNumber);
			if (OidIsValid(result))
				break;
			/* 失败不太可能发生，但若如此则继续查找 */
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_mergejoin_opfamilies
 *		给定一个推定可归并连接的算子，返回其表示相等
 *		的 btree 算子族 OID 列表。
 *
 * 一个算子在多个算子族中都表示相等是有可能的
 * （尽管目前少见），因此结果是一个列表。这让
 * 我们能在算子不属于任何算子族时返回 NIL。
 *
 * 规划器目前使用简单的 equal() 测试来比较本函数
 * 返回的列表，这使得列表顺序变得重要，尽管严格
 * 来说本不应如此。由于 syscache 列表搜索的处理
 * 方式，正常运行时结果会按 OID 排序，因此一切正常。
 * 若在禁用系统索引的情况下运行，结果顺序不确定，
 * 因而规划器可能无法识别优化机会……
 * 但那种场景下性能本来就很差，所以没必要
 * 为那种情况下的顺序保证而耗费代码或
 * 周期。
 */
List *
get_mergejoin_opfamilies(Oid opno)
{
	List	   *result = NIL;
	CatCList   *catlist;
	int			i;

	/*
	 * 在 pg_amop 中搜索目标算子是否被注册为某
	 * btree 算子族的"=" 算子。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* 必为 btree 相等 */
		if (aform->amopmethod == BTREE_AM_OID &&
			aform->amopstrategy == BTEqualStrategyNumber)
			result = lappend_oid(result, aform->amopfamily);
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_compatible_hash_operators
 *		取得与给定算子兼容、但作用于其 LHS 和/或 RHS
 *		数据类型的哈希相等算子的 OID。
 *
 * 若 lhs_opno 非 NULL，则查找 LHS 类型的算子并
 * 存入 *lhs_opno。类似地，若 rhs_opno 非 NULL，
 * 则查找 RHS 类型的算子并存入 *rhs_opno。
 *
 * 若给定算子不是跨类型的，结果应为同一算子；
 * 但在跨类型情形下二者会不同。
 *
 * 若能找到所请求的算子则返回 true，否则返回 false。
 * （这表明该算子本不应被标记为 oprcanhash。）
 */
bool
get_compatible_hash_operators(Oid opno,
							  Oid *lhs_opno, Oid *rhs_opno)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* 失败时确保输出参数已初始化 */
	if (lhs_opno)
		*lhs_opno = InvalidOid;
	if (rhs_opno)
		*rhs_opno = InvalidOid;

	/*
	 * 在 pg_amop 中搜索目标算子是否被注册为某
	 * 哈希算子族 "=" 算子。若它注册于多个算子族，
	 * 则假定任选其一即可。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			/* 若给定算子为单一类型则无需额外查找 */
			if (aform->amoplefttype == aform->amoprighttype)
			{
				if (lhs_opno)
					*lhs_opno = opno;
				if (rhs_opno)
					*rhs_opno = opno;
				result = true;
				break;
			}

			/*
			 * 取得匹配的单一类型算子。失败本不应发生
			 * --- 它暗示算子族有问题 --- 但若如此则
			 * 继续查找。
			 */
			if (lhs_opno)
			{
				*lhs_opno = get_opfamily_member(aform->amopfamily,
												aform->amoplefttype,
												aform->amoplefttype,
												HTEqualStrategyNumber);
				if (!OidIsValid(*lhs_opno))
					continue;
				/* 找到匹配 LHS；调用者不需 RHS 即完成 */
				if (!rhs_opno)
				{
					result = true;
					break;
				}
			}
			if (rhs_opno)
			{
				*rhs_opno = get_opfamily_member(aform->amopfamily,
												aform->amoprighttype,
												aform->amoprighttype,
												HTEqualStrategyNumber);
				if (!OidIsValid(*rhs_opno))
				{
					/* 忘掉此算子族的任何 LHS 算子 */
					if (lhs_opno)
						*lhs_opno = InvalidOid;
					continue;
				}
				/* 找到匹配 RHS，完成 */
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_op_hash_functions
 *		取得与给定算子兼容的标准哈希支持函数的 OID，
 *		按需作用于其 LHS 和/或 RHS 数据类型。
 *
 * 若 lhs_procno 非 NULL，则查找 LHS 类型的函数并
 * 存入 *lhs_procno。类似地，若 rhs_procno 非 NULL，
 * 则查找 RHS 类型的函数并存入 *rhs_procno。
 *
 * 若给定算子不是跨类型的，结果应为同一算子；
 * 但在跨类型情形下二者会不同。
 *
 * 若能找到所请求的函数则返回 true，否则返回 false。
 * （这表明该算子本不应被标记为 oprcanhash。）
 */
bool
get_op_hash_functions(Oid opno,
					  RegProcedure *lhs_procno, RegProcedure *rhs_procno)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* 失败时确保输出参数已初始化 */
	if (lhs_procno)
		*lhs_procno = InvalidOid;
	if (rhs_procno)
		*rhs_procno = InvalidOid;

	/*
	 * 在 pg_amop 中搜索目标算子是否被注册为某
	 * 哈希算子族 "=" 算子。若它注册于多个算子族，
	 * 则假定任选其一即可。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			/*
			 * 取得匹配的支持函数。失败本不应发生
			 * --- 它暗示算子族有问题 --- 但若如此则
			 * 继续查找。
			 */
			if (lhs_procno)
			{
				*lhs_procno = get_opfamily_proc(aform->amopfamily,
												aform->amoplefttype,
												aform->amoplefttype,
												HASHSTANDARD_PROC);
				if (!OidIsValid(*lhs_procno))
					continue;
				/* 找到匹配 LHS；调用者不需 RHS 即完成 */
				if (!rhs_procno)
				{
					result = true;
					break;
				}
				/* 若给定算子为单一类型则只需一次查找 */
				if (aform->amoplefttype == aform->amoprighttype)
				{
					*rhs_procno = *lhs_procno;
					result = true;
					break;
				}
			}
			if (rhs_procno)
			{
				*rhs_procno = get_opfamily_proc(aform->amopfamily,
												aform->amoprighttype,
												aform->amoprighttype,
												HASHSTANDARD_PROC);
				if (!OidIsValid(*rhs_procno))
				{
					/* 忘掉此算子族的任何 LHS 函数 */
					if (lhs_procno)
						*lhs_procno = InvalidOid;
					continue;
				}
				/* 找到匹配 RHS，完成 */
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_op_btree_interpretation
 *		给定算子的 OID，查明它属于哪些 btree 算子族，
 *		以及它在各算子族中的属性。结果以 palloc 的
 *		OpBtreeInterpretation 结构体列表返回。
 *
 * 除普通 btree 算子外，若 <> 算子的取反算子是某算子族
 * 的相等算子，我们也将其视为该算子族的"成员"。
 * 这种情形下返回的策略号为 ROWCOMPARE_NE。
 */
List *
get_op_btree_interpretation(Oid opno)
{
	List	   *result = NIL;
	OpBtreeInterpretation *thisresult;
	CatCList   *catlist;
	int			i;

	/*
	 * 找出包含该算子的所有 pg_amop 条目。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);
		StrategyNumber op_strategy;

		/* 必为 btree */
		if (op_form->amopmethod != BTREE_AM_OID)
			continue;

		/* 取得算子的 btree 策略号 */
		op_strategy = (StrategyNumber) op_form->amopstrategy;
		Assert(op_strategy >= 1 && op_strategy <= 5);

		thisresult = (OpBtreeInterpretation *)
			palloc(sizeof(OpBtreeInterpretation));
		thisresult->opfamily_id = op_form->amopfamily;
		thisresult->strategy = op_strategy;
		thisresult->oplefttype = op_form->amoplefttype;
		thisresult->oprighttype = op_form->amoprighttype;
		result = lappend(result, thisresult);
	}

	ReleaseSysCacheList(catlist);

	/*
	 * 若没找到任何包含该算子的 btree 算子族，那么它
	 * 也许是 <> 算子。看其取反算子是否在某算子族中。
	 */
	if (result == NIL)
	{
		Oid			op_negator = get_negator(opno);

		if (OidIsValid(op_negator))
		{
			catlist = SearchSysCacheList1(AMOPOPID,
										  ObjectIdGetDatum(op_negator));

			for (i = 0; i < catlist->n_members; i++)
			{
				HeapTuple	op_tuple = &catlist->members[i]->tuple;
				Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);
				StrategyNumber op_strategy;

				/* 必为 btree */
				if (op_form->amopmethod != BTREE_AM_OID)
					continue;

				/* 取得算子的 btree 策略号 */
				op_strategy = (StrategyNumber) op_form->amopstrategy;
				Assert(op_strategy >= 1 && op_strategy <= 5);

				/* 取反算子须为"=" */
				if (op_strategy != BTEqualStrategyNumber)
					continue;

				/* 好，以 ROWCOMPARE_NE 报告策略号 */
				thisresult = (OpBtreeInterpretation *)
					palloc(sizeof(OpBtreeInterpretation));
				thisresult->opfamily_id = op_form->amopfamily;
				thisresult->strategy = ROWCOMPARE_NE;
				thisresult->oplefttype = op_form->amoplefttype;
				thisresult->oprighttype = op_form->amoprighttype;
				result = lappend(result, thisresult);
			}

			ReleaseSysCacheList(catlist);
		}
	}

	return result;
}

/*
 * equality_ops_are_compatible
 *		若给定的两个相等算子具有兼容的语义，则返回
 *		true。
 *
 * 若二者是同一算子则显然成立。否则，
 * 我们查看它们是否能见于同一 btree 或哈希算子族。
 * 任一发现都让我们得以假定二者具有兼容的
 * 相等概念。（我们需要绕这些弯子的原因是，其中
 * 可能是跨类型算子；如 int24eq 与 int4eq。）
 */
bool
equality_ops_are_compatible(Oid opno1, Oid opno2)
{
	bool		result;
	CatCList   *catlist;
	int			i;

	/* 同一算子则容易 */
	if (opno1 == opno2)
		return true;

	/*
	 * 我们遍历 opno1 的所有 pg_amop 条目。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno1));

	result = false;
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);

		/* 必为btree或哈希 */
		if (op_form->amopmethod == BTREE_AM_OID ||
			op_form->amopmethod == HASH_AM_OID)
		{
			if (op_in_opfamily(opno2, op_form->amopfamily))
			{
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * comparison_ops_are_compatible
 *		若给定的两个比较算子具有兼容的语义，则返回
 *		true。
 *
 * 若二者是同一算子则显然成立。否则，
 * 我们查看它们是否能见于同一 btree 算子族。
 * 例如，若 '<' 与 '>=' 属于同一算子族，二者即匹配。
 *
 * （这与 equality_ops_are_compatible() 完全相同，只是
 * 不再费心检查哈希算子类。）
 */
bool
comparison_ops_are_compatible(Oid opno1, Oid opno2)
{
	bool		result;
	CatCList   *catlist;
	int			i;

	/* 同一算子则容易 */
	if (opno1 == opno2)
		return true;

	/*
	 * 我们遍历 opno1 的所有 pg_amop 条目。
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno1));

	result = false;
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);

		if (op_form->amopmethod == BTREE_AM_OID)
		{
			if (op_in_opfamily(opno2, op_form->amopfamily))
			{
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}


/* 				---------- AMPROC 缓存 ---------- */

/*
 * get_opfamily_proc
 *		取得指定算子族与数据类型的
 *		指定支持函数的 OID。
 *
 * 若给定键没有对应 pg_amproc 条目则返回 InvalidOid。
 */
Oid
get_opfamily_proc(Oid opfamily, Oid lefttype, Oid righttype, int16 procnum)
{
	HeapTuple	tp;
	Form_pg_amproc amproc_tup;
	RegProcedure result;

	tp = SearchSysCache4(AMPROCNUM,
						 ObjectIdGetDatum(opfamily),
						 ObjectIdGetDatum(lefttype),
						 ObjectIdGetDatum(righttype),
						 Int16GetDatum(procnum));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amproc_tup = (Form_pg_amproc) GETSTRUCT(tp);
	result = amproc_tup->amproc;
	ReleaseSysCache(tp);
	return result;
}


/* 				---------- ATTRIBUTE 缓存 ---------- */

/*
 * get_attname
 *		给定关系 OID 与属性号，以 palloc 字符串形式返回
 *		属性关系中的 "attname" 字段。
 *
 * 若该属性不存在且 missing_ok 为 true，则返回 NULL；
 * 否则抛出非面向用户使用的错误。
 */
char *
get_attname(Oid relid, AttrNumber attnum, bool missing_ok)
{
	HeapTuple	tp;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid), Int16GetDatum(attnum));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(att_tup->attname));
		ReleaseSysCache(tp);
		return result;
	}

	if (!missing_ok)
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	return NULL;
}

/*
 * get_attnum
 *
 *		给定关系 OID 与属性名，
 *		返回属性关系中的 "attnum" 字段。
 *
 *		若属性不存在（或已删除）则返回 InvalidAttrNumber。
 */
AttrNumber
get_attnum(Oid relid, const char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCacheAttName(relid, attname);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		AttrNumber	result;

		result = att_tup->attnum;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}

/*
 * get_attstattarget
 *
 *		给定关系 OID 与属性号，
 *		返回属性关系中的 "attstattarget" 字段。
 *
 *		若无则报错。
 */
int
get_attstattarget(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;
	Form_pg_attribute att_tup;
	int			result;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	att_tup = (Form_pg_attribute) GETSTRUCT(tp);
	result = att_tup->attstattarget;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_attgenerated
 *
 *		给定关系 OID 与属性号，
 *		返回属性关系中的 "attgenerated" 字段。
 *
 *		若无则报错。
 *
 *		由于非生成以 '\0' 表示，它也可用作布尔
 *		测试。
 */
char
get_attgenerated(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;
	Form_pg_attribute att_tup;
	char		result;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	att_tup = (Form_pg_attribute) GETSTRUCT(tp);
	result = att_tup->attgenerated;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_atttype
 *
 *		给定关系 OID 与其中的属性号，
 *		返回该属性的类型OID。
 */
Oid
get_atttype(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		Oid			result;

		result = att_tup->atttypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_atttypetypmodcoll
 *
 *		三合一：给定关系 OID 与属性号，
 *		一次缓存查找即取回 atttypid、atttypmod、attcollation。
 *
 * 与相似但不同的 get_atttype 不同，本例程
 * 在无法获得信息时会抛出错误。
 */
void
get_atttypetypmodcoll(Oid relid, AttrNumber attnum,
					  Oid *typid, int32 *typmod, Oid *collid)
{
	HeapTuple	tp;
	Form_pg_attribute att_tup;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	att_tup = (Form_pg_attribute) GETSTRUCT(tp);

	*typid = att_tup->atttypid;
	*typmod = att_tup->atttypmod;
	*collid = att_tup->attcollation;
	ReleaseSysCache(tp);
}

/*
 * get_attoptions
 *
 *		给定关系 OID 与属性号，
 *		若有则返回属性选项的 text[] datum。
 */
Datum
get_attoptions(Oid relid, int16 attnum)
{
	HeapTuple	tuple;
	Datum		attopts;
	Datum		result;
	bool		isnull;

	tuple = SearchSysCache2(ATTNUM,
							ObjectIdGetDatum(relid),
							Int16GetDatum(attnum));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);

	attopts = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							  &isnull);

	if (isnull)
		result = (Datum) 0;
	else
		result = datumCopy(attopts, false, -1); /* text[] */

	ReleaseSysCache(tuple);

	return result;
}

/* 				---------- PG_CAST 缓存 ---------- */

/*
 * get_cast_oid - 由两个类型 OID 查找转换 OID
 *
 * 若 missing_ok 为 false，未找到该转换则报错；为 true
 * 则仅返回 InvalidOid。
 */
Oid
get_cast_oid(Oid sourcetypeid, Oid targettypeid, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid2(CASTSOURCETARGET, Anum_pg_cast_oid,
						  ObjectIdGetDatum(sourcetypeid),
						  ObjectIdGetDatum(targettypeid));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cast from type %s to type %s does not exist",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));
	return oid;
}

/* 				---------- COLLATION 缓存 ---------- */

/*
 * get_collation_name
 *		返回给定 pg_collation 条目的名称。
 *
 * 返回字符串的 palloc 副本；无此排序规则则得 NULL。
 *
 * 注意：由于排序规则名称不唯一，将此函数用于
 * 准备错误消息之外的用途须小心。
 */
char *
get_collation_name(Oid colloid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(colloid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_collation colltup = (Form_pg_collation) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(colltup->collname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

bool
get_collation_isdeterministic(Oid colloid)
{
	HeapTuple	tp;
	Form_pg_collation colltup;
	bool		result;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(colloid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for collation %u", colloid);
	colltup = (Form_pg_collation) GETSTRUCT(tp);
	result = colltup->collisdeterministic;
	ReleaseSysCache(tp);
	return result;
}

/* 				---------- CONSTRAINT 缓存 ---------- */

/*
 * get_constraint_name
 *		返回给定 pg_constraint 条目的名称。
 *
 * 返回字符串的 palloc 副本；无此约束则得 NULL。
 *
 * 注意：由于约束名称不唯一，将此函数用于
 * 准备错误消息之外的用途须小心。
 */
char *
get_constraint_name(Oid conoid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(contup->conname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_constraint_index
 *		给定唯一、主键或排他约束的 OID，
 *		返回其底层索引的 OID。
 *
 * 若找不到该约束，则返回 InvalidOid；类型
 * 不符亦然。
 *
 * 本函数旨在返回由指定约束"所拥有"的索引。
 * 因此必须检查 contype，因为某些 pg_constraint 条目
 * （例如外键约束的条目）存储的是被引用而非被
 * 该约束拥有的索引的 OID。
 */
Oid
get_constraint_index(Oid conoid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);
		Oid			result;

		if (contup->contype == CONSTRAINT_UNIQUE ||
			contup->contype == CONSTRAINT_PRIMARY ||
			contup->contype == CONSTRAINT_EXCLUSION)
			result = contup->conindid;
		else
			result = InvalidOid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/* 				---------- LANGUAGE 缓存 ---------- */

char *
get_language_name(Oid langoid, bool missing_ok)
{
	HeapTuple	tp;

	tp = SearchSysCache1(LANGOID, ObjectIdGetDatum(langoid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_language lantup = (Form_pg_language) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(lantup->lanname));
		ReleaseSysCache(tp);
		return result;
	}

	if (!missing_ok)
		elog(ERROR, "cache lookup failed for language %u",
			 langoid);
	return NULL;
}

/* 				---------- OPCLASS 缓存 ---------- */

/*
 * get_opclass_family
 *
 *		返回该算子类所属算子族的 OID。
 */
Oid
get_opclass_family(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	Oid			result;

	tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcfamily;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_opclass_input_type
 *
 *		返回该算子类所索引数据类型的 OID。
 */
Oid
get_opclass_input_type(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	Oid			result;

	tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcintype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_opclass_opfamily_and_input_type
 *
 *		返回该算子类所属算子族的 OID，
 *				及该算子类所索引数据类型的 OID
 */
bool
get_opclass_opfamily_and_input_type(Oid opclass, Oid *opfamily, Oid *opcintype)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;

	tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(tp))
		return false;

	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	*opfamily = cla_tup->opcfamily;
	*opcintype = cla_tup->opcintype;

	ReleaseSysCache(tp);

	return true;
}

/* 				---------- OPERATOR 缓存 ---------- */

/*
 * get_opcode
 *
 *		给定算子 OID，返回实现该算子的例程的
 *		regproc 标识。
 */
RegProcedure
get_opcode(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprcode;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_opname
 *	  返回具有给定 opno 的算子的名称
 *
 * 注意：返回字符串的 palloc 副本；无此算子则返回 NULL。
 */
char *
get_opname(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(optup->oprname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_op_rettype
 *		给定算子 OID，返回该算子的结果类型。
 */
Oid
get_op_rettype(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprresult;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * op_input_types
 *
 *		返回某算子的左、右输入数据类型（
 *		不适用则 InvalidOid）。
 */
void
op_input_types(Oid opno, Oid *lefttype, Oid *righttype)
{
	HeapTuple	tp;
	Form_pg_operator optup;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tp))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for operator %u", opno);
	optup = (Form_pg_operator) GETSTRUCT(tp);
	*lefttype = optup->oprleft;
	*righttype = optup->oprright;
	ReleaseSysCache(tp);
}

/*
 * op_mergejoinable
 *
 * 若该算子潜在可归并连接则返回 true。（规划器
 * 将找不到任何归并连接计划，除非存在适合该算子
 * 及其关联 sortops 的 btree 算子族条目。pg_operator 标志
 * 只是提示规划器是否值得去查找而已。）
 *
 * 某些情形下（目前仅有 array_eq 与 record_eq），可归并
 * 性取决于调用该算子时的具体输入数据类型，因此
 * 也必须传入该类型。我们目前假定检查时只需一个
 * 输入的类型 --- 按约定传入左输入的数据类型。
 */
bool
op_mergejoinable(Oid opno, Oid inputtype)
{
	bool		result = false;
	HeapTuple	tp;
	TypeCacheEntry *typentry;

	/*
	 * 对 array_eq 或 record_eq，仅当元素或字段类型都可
	 * 排序时才能排序。相关检查我们本可全部在此实现，
	 * 但 typcache 已经做了这些检查并缓存了结果，
	 * 故依赖 typcache。
	 */
	if (opno == ARRAY_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_CMP_PROC);
		if (typentry->cmp_proc == F_BTARRAYCMP)
			result = true;
	}
	else if (opno == RECORD_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_CMP_PROC);
		if (typentry->cmp_proc == F_BTRECORDCMP)
			result = true;
	}
	else
	{
		/* 对其他所有算子，依据 pg_operator.oprcanmerge */
		tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
		if (HeapTupleIsValid(tp))
		{
			Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

			result = optup->oprcanmerge;
			ReleaseSysCache(tp);
		}
	}
	return result;
}

/*
 * op_hashjoinable
 *
 * 若该算子可哈希连接则返回 true。（若已如此标记，
 * 则必有适合该算子的哈希算子族条目。）
 *
 * 某些情形下（目前仅有 array_eq），可哈希连接性取决
 * 于调用该算子时的具体输入数据类型，因此也必须
 * 传入该类型。我们目前假定检查时只需一个输入的
 * 类型 --- 按约定传入左输入的数据类型。
 */
bool
op_hashjoinable(Oid opno, Oid inputtype)
{
	bool		result = false;
	HeapTuple	tp;
	TypeCacheEntry *typentry;

	/* 同 op_mergejoinable，难情形交给 typcache 处理 */
	if (opno == ARRAY_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_HASH_PROC);
		if (typentry->hash_proc == F_HASH_ARRAY)
			result = true;
	}
	else if (opno == RECORD_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_HASH_PROC);
		if (typentry->hash_proc == F_HASH_RECORD)
			result = true;
	}
	else
	{
		/* 对其他所有算子，依据 pg_operator.oprcanhash */
		tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
		if (HeapTupleIsValid(tp))
		{
			Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

			result = optup->oprcanhash;
			ReleaseSysCache(tp);
		}
	}
	return result;
}

/*
 * op_strict
 *
 * 取得算子底层函数的 proisstrict 标志。
 */
bool
op_strict(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_strict((Oid) funcid);
}

/*
 * op_volatile
 *
 * 取得算子底层函数的 provolatile 标志。
 */
char
op_volatile(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_volatile((Oid) funcid);
}

/*
 * get_commutator
 *
 *		返回某算子相应的交换算子。
 */
Oid
get_commutator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprcom;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_negator
 *
 *		返回某算子相应的取反算子。
 */
Oid
get_negator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprnegate;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_oprrest
 *
 *		返回用于计算某算子选择率的过程标识。
 */
RegProcedure
get_oprrest(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprrest;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_oprjoin
 *
 *		返回用于计算某连接选择率的过程标识。
 */
RegProcedure
get_oprjoin(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprjoin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/* 				---------- FUNCTION 缓存 ---------- */

/*
 * get_func_name
 *	  返回具有给定 funcid 的函数的名称
 *
 * 注意：返回字符串的 palloc 副本；无此函数则返回 NULL。
 */
char *
get_func_name(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(functup->proname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_func_namespace
 *
 *		返回与给定函数关联的 pg_namespace OID。
 */
Oid
get_func_namespace(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		Oid			result;

		result = functup->pronamespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_func_rettype
 *		给定过程标识，返回函数的结果类型。
 */
Oid
get_func_rettype(Oid funcid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prorettype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_nargs
 *		给定过程标识，返回参数个数。
 */
int
get_func_nargs(Oid funcid)
{
	HeapTuple	tp;
	int			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->pronargs;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_signature
 *		给定过程标识，返回函数的参数类型与结果类型。
 *		（返回值即结果类型。）
 *
 * 参数以 palloc 数组形式返回。
 */
Oid
get_func_signature(Oid funcid, Oid **argtypes, int *nargs)
{
	HeapTuple	tp;
	Form_pg_proc procstruct;
	Oid			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procstruct = (Form_pg_proc) GETSTRUCT(tp);

	result = procstruct->prorettype;
	*nargs = (int) procstruct->pronargs;
	Assert(*nargs == procstruct->proargtypes.dim1);
	*argtypes = (Oid *) palloc(*nargs * sizeof(Oid));
	memcpy(*argtypes, procstruct->proargtypes.values, *nargs * sizeof(Oid));

	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_variadictype
 *		给定过程标识，返回函数的 provariadic 字段。
 */
Oid
get_func_variadictype(Oid funcid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->provariadic;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_retset
 *		给定过程标识，返回函数的 proretset 标志。
 */
bool
get_func_retset(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proretset;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_strict
 *		给定过程标识，返回函数的 proisstrict 标志。
 */
bool
func_strict(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proisstrict;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_volatile
 *		给定过程标识，返回函数的 provolatile 标志。
 */
char
func_volatile(Oid funcid)
{
	HeapTuple	tp;
	char		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->provolatile;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_parallel
 *		给定过程标识，返回函数的 proparallel 标志。
 */
char
func_parallel(Oid funcid)
{
	HeapTuple	tp;
	char		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proparallel;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_prokind
 *	   给定过程标识，返回例程种类。
 */
char
get_func_prokind(Oid funcid)
{
	HeapTuple	tp;
	char		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prokind;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_leakproof
 *	   给定过程标识，返回函数的 leakproof 字段。
 */
bool
get_func_leakproof(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proleakproof;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_support
 *
 *		返回与给定函数关联的支持函数的 OID，
 *		若无则返回 InvalidOid。
 */
RegProcedure
get_func_support(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		RegProcedure result;

		result = functup->prosupport;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/* 				---------- RELATION 缓存 ---------- */

/*
 * get_relname_relid
 *		给定关系的名称与命名空间，查找其 OID。
 *
 * 若无此关系则返回 InvalidOid。
 */
Oid
get_relname_relid(const char *relname, Oid relnamespace)
{
	return GetSysCacheOid2(RELNAMENSP, Anum_pg_class_oid,
						   PointerGetDatum(relname),
						   ObjectIdGetDatum(relnamespace));
}

#ifdef NOT_USED
/*
 * get_relnatts
 *
 *		返回给定关系的属性个数。
 */
int
get_relnatts(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		int			result;

		result = reltup->relnatts;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}
#endif

/*
 * get_rel_name
 *		返回给定关系的名称。
 *
 * 返回字符串的 palloc 副本；无此关系则得 NULL。
 *
 * 注意：由于关系名称不唯一，将此函数用于
 * 准备错误消息之外的用途须小心。
 */
char *
get_rel_name(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(reltup->relname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_rel_namespace
 *
 *		返回与给定关系关联的 pg_namespace OID。
 */
Oid
get_rel_namespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->relnamespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_type_id
 *
 *		返回与给定关系关联的 pg_type OID。
 *
 * 注意：并非所有 pg_class 条目都有关联的 pg_type
 * OID；务必当心检查 InvalidOid。
 */
Oid
get_rel_type_id(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_relkind
 *
 *		返回与给定关系关联的 relkind。
 */
char
get_rel_relkind(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char		result;

		result = reltup->relkind;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * get_rel_relispartition
 *
 *		返回与给定关系关联的 relispartition 标志。
 */
bool
get_rel_relispartition(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		bool		result;

		result = reltup->relispartition;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_rel_tablespace
 *
 *		返回与给定关系关联的 pg_tablespace OID。
 *
 * 注意：InvalidOid 可能意味着我们找不到该关系，
 * 或它位于数据库的默认表空间中。
 */
Oid
get_rel_tablespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltablespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_persistence
 *
 *		返回与给定关系关联的 relpersistence。
 */
char
get_rel_persistence(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char		result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);
	result = reltup->relpersistence;
	ReleaseSysCache(tp);

	return result;
}


/* 				---------- TRANSFORM 缓存 ---------- */

Oid
get_transform_fromsql(Oid typid, Oid langid, List *trftypes)
{
	HeapTuple	tup;

	if (!list_member_oid(trftypes, typid))
		return InvalidOid;

	tup = SearchSysCache2(TRFTYPELANG, typid, langid);
	if (HeapTupleIsValid(tup))
	{
		Oid			funcid;

		funcid = ((Form_pg_transform) GETSTRUCT(tup))->trffromsql;
		ReleaseSysCache(tup);
		return funcid;
	}
	else
		return InvalidOid;
}

Oid
get_transform_tosql(Oid typid, Oid langid, List *trftypes)
{
	HeapTuple	tup;

	if (!list_member_oid(trftypes, typid))
		return InvalidOid;

	tup = SearchSysCache2(TRFTYPELANG, typid, langid);
	if (HeapTupleIsValid(tup))
	{
		Oid			funcid;

		funcid = ((Form_pg_transform) GETSTRUCT(tup))->trftosql;
		ReleaseSysCache(tup);
		return funcid;
	}
	else
		return InvalidOid;
}


/* 				---------- TYPE 缓存 ---------- */

/*
 * get_typisdefined
 *
 *		给定类型 OID，判断该类型是否已定义
 *		（否则只是壳）。
 */
bool
get_typisdefined(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typisdefined;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlen
 *
 *		给定类型 OID，返回该类型的长度。
 */
int16
get_typlen(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		int16		result;

		result = typtup->typlen;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 0;
}

/*
 * get_typbyval
 *
 *		给定类型 OID，判断该类型按值返回还是按引用
 *		返回。按值为 true，按引用为 false。
 */
bool
get_typbyval(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typbyval;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlenbyval
 *
 *		二合一：给定类型 OID，返回 typlen 与 typbyval。
 *
 *		由于拷贝 Datum 需要这两项信息，许多地方都要
 *		同时取得二者。不妨用一次缓存查找而非两次
 *		获取。此外，本例程遇到坏类型 OID 时会报错，
 *		而不是返回假值。
 */
void
get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	ReleaseSysCache(tp);
}

/*
 * get_typlenbyvalalign
 *
 *		给定类型 OID，三合一返回 typlen、typbyval、typalign。
 */
void
get_typlenbyvalalign(Oid typid, int16 *typlen, bool *typbyval,
					 char *typalign)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	*typalign = typtup->typalign;
	ReleaseSysCache(tp);
}

/*
 * getTypeIOParam
 *		给定 pg_type 行，选出要传给 I/O 函数的类型 OID
 *
 * 从前，所有 I/O 函数的第二参数都会收到pg_type.typelem，
 * 但现在对于传什么我们有了更复杂的规则。
 * 这类知识应当集中于此 --- 代码中其他地方对
 * typelem 的直接引用，若其伴随的是 I/O 调用而非
 * 真正的下标操作，那它们就是错的！（但若需要
 * 修改此处，参见 bootstrap.c boot_get_type_io_data()。）
 *
 * 自 PostgreSQL 8.1 起，输出函数只接收值本身，
 * 不再接收任何辅助参数，因此本例程的名称如今
 * 有点名不副实……它应为 getTypeInputParam。
 */
Oid
getTypeIOParam(HeapTuple typeTuple)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * 数组类型以 typelem 为参数；其他类型则以
	 * 自身 OID 为参数。
	 */
	if (OidIsValid(typeStruct->typelem))
		return typeStruct->typelem;
	else
		return typeStruct->oid;
}

/*
 * get_type_io_data
 *
 *		六合一：给定类型 OID，返回 typlen、typbyval、
 *					typalign、typdelim、typioparam 与 I/O 函数 OID。
 *					由 IOFuncSelector 控制返回哪个
 */
void
get_type_io_data(Oid typid,
				 IOFuncSelector which_func,
				 int16 *typlen,
				 bool *typbyval,
				 char *typalign,
				 char *typdelim,
				 Oid *typioparam,
				 Oid *func)
{
	HeapTuple	typeTuple;
	Form_pg_type typeStruct;

	/*
	 * 引导模式下将其转交 bootstrap.c。此变通让我们
	 * 引导期可用 array_in 与 array_out。
	 */
	if (IsBootstrapProcessingMode())
	{
		Oid			typinput;
		Oid			typoutput;

		boot_get_type_io_data(typid,
							  typlen,
							  typbyval,
							  typalign,
							  typdelim,
							  typioparam,
							  &typinput,
							  &typoutput);
		switch (which_func)
		{
			case IOFunc_input:
				*func = typinput;
				break;
			case IOFunc_output:
				*func = typoutput;
				break;
			default:
				elog(ERROR, "binary I/O not supported during bootstrap");
				break;
		}
		return;
	}

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	*typlen = typeStruct->typlen;
	*typbyval = typeStruct->typbyval;
	*typalign = typeStruct->typalign;
	*typdelim = typeStruct->typdelim;
	*typioparam = getTypeIOParam(typeTuple);
	switch (which_func)
	{
		case IOFunc_input:
			*func = typeStruct->typinput;
			break;
		case IOFunc_output:
			*func = typeStruct->typoutput;
			break;
		case IOFunc_receive:
			*func = typeStruct->typreceive;
			break;
		case IOFunc_send:
			*func = typeStruct->typsend;
			break;
	}
	ReleaseSysCache(typeTuple);
}

#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typalign;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return TYPALIGN_INT;
}
#endif

char
get_typstorage(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typstorage;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return TYPSTORAGE_PLAIN;
}

/*
 * get_typdefault
 *	  给定类型 OID，返回该类型默认值（若有）。
 *
 *	  结果为 palloc 的表达式节点树；若该数据类型
 *	  没有定义默认值则为 NULL。
 *
 * 注意：调用者应准备把结果强转到正确数据类型；
 * 返回的表达式树可能产生类型不对的东西。
 */
Node *
get_typdefault(Oid typid)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Datum		datum;
	bool		isNull;
	Node	   *expr;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	type = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * typdefault 与 typdefaultbin 可能为空，因此不要
	 * 把它们当作结构体字段访问。必须用
	 * SysCacheGetAttr
	 */
	datum = SysCacheGetAttr(TYPEOID,
							typeTuple,
							Anum_pg_type_typdefaultbin,
							&isNull);

	if (!isNull)
	{
		/* 默认值为表达式 */
		expr = stringToNode(TextDatumGetCString(datum));
	}
	else
	{
		/* 或许存在纯字面量默认值 */
		datum = SysCacheGetAttr(TYPEOID,
								typeTuple,
								Anum_pg_type_typdefault,
								&isNull);

		if (!isNull)
		{
			char	   *strDefaultVal;

			/* 文本 datum 转 C 字符串 */
			strDefaultVal = TextDatumGetCString(datum);
			/* 把 C 字符串转为给定类型的值 */
			datum = OidInputFunctionCall(type->typinput, strDefaultVal,
										 getTypeIOParam(typeTuple), -1);
			/* 构建包含该值的 Const 节点 */
			expr = (Node *) makeConst(typid,
									  -1,
									  type->typcollation,
									  type->typlen,
									  datum,
									  false,
									  type->typbyval);
			pfree(strDefaultVal);
		}
		else
		{
			/* 无 */
			expr = NULL;
		}
	}

	ReleaseSysCache(typeTuple);

	return expr;
}

/*
 * getBaseType
 *		若给定类型是域，则返回其基础类型；
 *		否则返回该类型自身OID。
 */
Oid
getBaseType(Oid typid)
{
	int32		typmod = -1;

	return getBaseTypeAndTypmod(typid, &typmod);
}

/*
 * getBaseTypeAndTypmod
 *		若给定类型是域，返回其基础类型与 typmod；
 *		否则返回该类型自身的 OID，且不改动 *typmod。
 *
 * 注意：对最底层之上的每一层级域，"已套用的
 * typmod"应为 -1；因此，若传入的 typid 确实
 * 是域，*typmod 应为 -1。
 */
Oid
getBaseTypeAndTypmod(Oid typid, int32 *typmod)
{
	/*
	 * 我们循环找出域栈中最底层的基础类型。
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* 非域，故结束 */
			ReleaseSysCache(tup);
			break;
		}

		Assert(*typmod == -1);
		typid = typTup->typbasetype;
		*typmod = typTup->typtypmod;

		ReleaseSysCache(tup);
	}

	return typid;
}

/*
 * get_typavgwidth
 *
 *	  给定类型 OID 与 typmod 值（typmod 未知时传 -1），
 *	  估算该类型值的平均宽度。规划器会用到它，
 *	  而规划器并不要求结果绝对正确；
 *	  若无法确知，猜测即可（且属预期）。
 */
int32
get_typavgwidth(Oid typid, int32 typmod)
{
	int			typlen = get_typlen(typid);
	int32		maxwidth;

	/*
	 * 定宽类型则简单
	 */
	if (typlen > 0)
		return typlen;

	/*
	 * type_maximum_size 知晓某些数据类型的 typmod 编码；
	 * 不要在此重复这些知识。
	 */
	maxwidth = type_maximum_size(typid, typmod);
	if (maxwidth > 0)
	{
		/*
		 * 对 BPCHAR 而言最大宽度也是唯一宽度。否则我们
		 * 需要根据最大值猜测典型数据宽度。按最大宽度
		 * 百分比的滑动尺度看来是合理的。
		 */
		if (typid == BPCHAROID)
			return maxwidth;
		if (maxwidth <= 32)
			return maxwidth;	/* 假定全宽 */
		if (maxwidth < 1000)
			return 32 + (maxwidth - 32) / 2;	/* 假定50% */

		/*
		 * 超过 1000 时，假定我们看到的是
		 * "varchar(10000)" 这类上限并不常真正达到的情形，
		 * 使用固定估值。
		 */
		return 32 + (1000 - 32) / 2;
	}

	/*
	 * 毫无头绪……只能瞎猜了。
	 */
	return 32;
}

/*
 * get_typtype
 *
 *		给定类型 OID，查明它是基本类型、复合类型等
 *		中的哪种；查找失败则返回空字符……
 */
char
get_typtype(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * type_is_rowtype
 *
 *		用于判断某类型 OID 是否表示"行类型"的便利
 *		函数 --- 即 RECORD 或具名复合类型
 *		（含具名复合类型之上的域）。
 */
bool
type_is_rowtype(Oid typid)
{
	if (typid == RECORDOID)
		return true;			/* 简单 */
	switch (get_typtype(typid))
	{
		case TYPTYPE_COMPOSITE:
			return true;
		case TYPTYPE_DOMAIN:
			if (get_typtype(getBaseType(typid)) == TYPTYPE_COMPOSITE)
				return true;
			break;
		default:
			break;
	}
	return false;
}

/*
 * type_is_enum
 *	  若给定类型是枚举类型则返回 true。
 */
bool
type_is_enum(Oid typid)
{
	return (get_typtype(typid) == TYPTYPE_ENUM);
}

/*
 * type_is_range
 *	  若给定类型是范围类型则返回 true。
 */
bool
type_is_range(Oid typid)
{
	return (get_typtype(typid) == TYPTYPE_RANGE);
}

/*
 * type_is_multirange
 *	  若给定类型是多范围类型则返回 true。
 */
bool
type_is_multirange(Oid typid)
{
	return (get_typtype(typid) == TYPTYPE_MULTIRANGE);
}

/*
 * get_type_category_preferred
 *
 *		给定类型 OID，取回其类别与首选类型状态。
 *		失败时抛出错误。
 */
void
get_type_category_preferred(Oid typid, char *typcategory, bool *typispreferred)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typcategory = typtup->typcategory;
	*typispreferred = typtup->typispreferred;
	ReleaseSysCache(tp);
}

/*
 * get_typ_typrelid
 *
 *		给定类型 OID，取得 typrelid，非复合类型则
 *		空。
 */
Oid
get_typ_typrelid(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typrelid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_element_type
 *
 *		给定类型 OID，取 typelem（非数组类型则 InvalidOid）。
 *
 * 注意：仅对以 array_subscript_handler 为 typsubscript 的
 * "真"数组才会成功。对其他类型，无论是否设置
 * typelem 或 typsubscript 都返回 InvalidOid。
 */
Oid
get_element_type(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		if (IsTrueArrayType(typtup))
			result = typtup->typelem;
		else
			result = InvalidOid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_array_type
 *
 *		给定类型 OID，取得对应的"真"数组类型。
 *		若找不到数组类型则返回 InvalidOid。
 */
Oid
get_array_type(Oid typid)
{
	HeapTuple	tp;
	Oid			result = InvalidOid;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_type) GETSTRUCT(tp))->typarray;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * get_promoted_array_type
 *
 *		"提升"类型即从 ARRAY(SELECT ...) 构造
 *		得到的类型，即若输入是拥有此类数组类型
 *		的标量类型，则为对应的"真"数组类型；
 *		若输入已是"真"数组类型，则仍为该类型。
 *		两规则均不满足则返回 InvalidOid。
 */
Oid
get_promoted_array_type(Oid typid)
{
	Oid			array_type = get_array_type(typid);

	if (OidIsValid(array_type))
		return array_type;
	if (OidIsValid(get_element_type(typid)))
		return typid;
	return InvalidOid;
}

/*
 * get_base_element_type
 *		给定类型 OID，取得 typelem，"穿透"任何域
 *		至其底层数组类型。
 *
 * 这等价于 get_element_type(getBaseType(typid))，
 * 但省去一次额外的缓存查找。注意它无法提供
 * 数组的 typmod 信息。
 */
Oid
get_base_element_type(Oid typid)
{
	/*
	 * 我们循环找出域栈中最底层的基础类型。
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			break;
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* 不是域，故停止下探 */
			Oid			result;

			/* 该测试须匹配 get_element_type */
			if (IsTrueArrayType(typTup))
				result = typTup->typelem;
			else
				result = InvalidOid;
			ReleaseSysCache(tup);
			return result;
		}

		typid = typTup->typbasetype;
		ReleaseSysCache(tup);
	}

	/* 同 get_element_type，坏输入静默返回 InvalidOid */
	return InvalidOid;
}

/*
 * getTypeInputInfo
 *
 *		取得把某类型的值转为内部形式所需的信息
 */
void
getTypeInputInfo(Oid type, Oid *typInput, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typinput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no input function available for type %s",
						format_type_be(type))));

	*typInput = pt->typinput;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeOutputInfo
 *
 *		取得打印某类型的值所需的信息
 */
void
getTypeOutputInfo(Oid type, Oid *typOutput, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typoutput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no output function available for type %s",
						format_type_be(type))));

	*typOutput = pt->typoutput;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryInputInfo
 *
 *		取得二进制输入某类型值所需的信息
 */
void
getTypeBinaryInputInfo(Oid type, Oid *typReceive, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typreceive))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary input function available for type %s",
						format_type_be(type))));

	*typReceive = pt->typreceive;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryOutputInfo
 *
 *		取得二进制输出某类型值所需的信息
 */
void
getTypeBinaryOutputInfo(Oid type, Oid *typSend, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typsend))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary output function available for type %s",
						format_type_be(type))));

	*typSend = pt->typsend;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}

/*
 * get_typmodin
 *
 *		给定类型 OID，返回该类型 typmodin 过程（若有）。
 */
Oid
get_typmodin(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typmodin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

#ifdef NOT_USED
/*
 * get_typmodout
 *
 *		给定类型 OID，返回该类型 typmodout 过程（若有）。
 */
Oid
get_typmodout(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typmodout;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}
#endif							/* 未用 */

/*
 * get_typcollation
 *
 *		给定类型 OID，返回该类型的 typcollation 属性。
 */
Oid
get_typcollation(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typcollation;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}


/*
 * type_is_collatable
 *
 *		返回该类型是否在意排序规则
 */
bool
type_is_collatable(Oid typid)
{
	return OidIsValid(get_typcollation(typid));
}


/*
 * get_typsubscript
 *
 *		给定类型 OID，返回该类型下标处理器的 OID（
 *		若有）。
 *
 * 若 typelemp 非 NULL，还会在该处存入类型的
 * typelem 值，并为调用者省一次查找。
 */
RegProcedure
get_typsubscript(Oid typid, Oid *typelemp)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typform = (Form_pg_type) GETSTRUCT(tp);
		RegProcedure handler = typform->typsubscript;

		if (typelemp)
			*typelemp = typform->typelem;
		ReleaseSysCache(tp);
		return handler;
	}
	else
	{
		if (typelemp)
			*typelemp = InvalidOid;
		return InvalidOid;
	}
}

/*
 * getSubscriptingRoutines
 *
 *		给定类型 OID，取回该类型的下标方法结构体。
 *		若类型不可下标则返回 NULL。
 *
 * 若 typelemp 非 NULL，还会在该处存入类型的
 * typelem 值，并为调用者省一次查找。
 */
const struct SubscriptRoutines *
getSubscriptingRoutines(Oid typid, Oid *typelemp)
{
	RegProcedure typsubscript = get_typsubscript(typid, typelemp);

	if (!OidIsValid(typsubscript))
		return NULL;

	return (const struct SubscriptRoutines *)
		DatumGetPointer(OidFunctionCall0(typsubscript));
}


/* 				---------- STATISTICS 缓存 ---------- */

/*
 * get_attavgwidth
 *
 *	  给定某列的表与属性号，取得该列条目的平均
 *	  宽度；无可用数据时返回零。
 *
 * 目前只对单个表（而非继承树）查阅此信息，
 * 因此不需要 "inh" 参数。
 *
 * 在此时点调用钩子看似有些奇怪，但却是必需
 * 的，因为优化器调用本函数时，插件没有其他
 * 途径可以控制结果。
 */
int32
get_attavgwidth(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;
	int32		stawidth;

	if (get_attavgwidth_hook)
	{
		stawidth = (*get_attavgwidth_hook) (relid, attnum);
		if (stawidth > 0)
			return stawidth;
	}
	tp = SearchSysCache3(STATRELATTINH,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum),
						 BoolGetDatum(false));
	if (HeapTupleIsValid(tp))
	{
		stawidth = ((Form_pg_statistic) GETSTRUCT(tp))->stawidth;
		ReleaseSysCache(tp);
		if (stawidth > 0)
			return stawidth;
	}
	return 0;
}

/*
 * get_attstatsslot
 *
 *		提取 pg_statistic 元组某个"槽"的内容。
 *		找到所请求槽类型则返回 true，否则 false。
 *
 * 与本文件中其他例程不同，它接收指向
 * pg_statistic 缓存中已查出元组的指针。这样做
 * 是因为多数调用者要从缓存条目提取多个值，
 * 我们不想不必要地重复缓存查找。此外，此 API
 * 还允许本例程用于统计元组：其由统计钩子
 * 提供且并非真的出自
 * pg_statistic
 *
 * sslot：输出区指针（通常是调用者的局部变量）。
 * statstuple：待查的 pg_statistic 元组。
 * reqkind：所需统计槽类型的 STAKIND 代码。
 * reqop：想要的 STAOP 值；不管则 InvalidOid。
 * flags：ATTSTATSSLOT_VALUES/ATTSTATSSLOT_NUMBERS 的位掩码。
 *
 * 若找到匹配的槽则返回 true，*sslot 按如下填充：
 * staop：接收实际 STAOP 值。
 * stacoll：接收实际 STACOLL 值。
 * valuetype：接收 stavalues 元素的实际数据类型。
 * values：接收指向槽的 stavalues 数组的指针。
 * nvalues：接收 stavalues 数目。
 * numbers：接收指向槽 stanumbers 数组（float4 型）的指针。
 * nnumbers：接收 stanumbers 数目。
 *
 * valuetype/values/nvalues 为 InvalidOid/NULL/0（未指定
 * ATTSTATSSLOT_VALUES 时）。同理，未指定
 * ATTSTATSSLOT_NUMBERS 时为 NULL/0。
 *
 * 若未找到匹配的槽则返回 false，且 *sslot 被清零。
 *
 * 注意：当前 API 不支持搜索具有特定排序规则的槽。
 * 若我们将来真要支持记录多个排序规则，就得扩展
 * API，但眼下简单为好。
 *
 * sslot 各字段所引数据为本地 palloc，与原
 * pg_statistic 元组无关。调用者用毕后，调用
 * free_attstatsslot 释放 palloc 的数据。
 *
 * 若希望能在可能未调用 get_attstatsslot 的
 * 情况下调用 free_attstatsslot，把 sslot 清零即可。
 *
 * 传 flags=0 可用于快速检查所请求的槽类型是否
 * 存在。此时不提取任何数组，故无需调用
 * 它。
 */
bool
get_attstatsslot(AttStatsSlot *sslot, HeapTuple statstuple,
				 int reqkind, Oid reqop, int flags)
{
	Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(statstuple);
	int			i;
	Datum		val;
	ArrayType  *statarray;
	Oid			arrayelemtype;
	int			narrayelem;
	HeapTuple	typeTuple;
	Form_pg_type typeForm;

	/* 正确初始化 *sslot */
	memset(sslot, 0, sizeof(AttStatsSlot));

	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		if ((&stats->stakind1)[i] == reqkind &&
			(reqop == InvalidOid || (&stats->staop1)[i] == reqop))
			break;
	}
	if (i >= STATISTIC_NUM_SLOTS)
		return false;			/* 不在 */

	sslot->staop = (&stats->staop1)[i];
	sslot->stacoll = (&stats->stacoll1)[i];

	if (flags & ATTSTATSSLOT_VALUES)
	{
		val = SysCacheGetAttrNotNull(STATRELATTINH, statstuple,
									 Anum_pg_statistic_stavalues1 + i);

		/*
		 * 必要时解 toast 数组，且无论如何都复制一份
		 * 副本归本 AttStatsSlot 控制。
		 */
		statarray = DatumGetArrayTypePCopy(val);

		/*
		 * 提取实际的数组元素类型，并传回给调用者，
		 * 以备需要。
		 */
		sslot->valuetype = arrayelemtype = ARR_ELEMTYPE(statarray);

		/* 需要元素类型的信息 */
		typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrayelemtype));
		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "cache lookup failed for type %u", arrayelemtype);
		typeForm = (Form_pg_type) GETSTRUCT(typeTuple);

		/* 把数组解构成 Datum 元素；预期不含 NULL */
		deconstruct_array(statarray,
						  arrayelemtype,
						  typeForm->typlen,
						  typeForm->typbyval,
						  typeForm->typalign,
						  &sslot->values, NULL, &sslot->nvalues);

		/*
		 * 若元素类型为按引用传递，我们手里就有一批
		 * 指向 statarray 的 Datum，因此要把它保留到
		 * free_attstatsslot 之时。否则，所有有用信息都在
		 * sslot->values[] 中，可立即释放该数组对象。
		 */
		if (!typeForm->typbyval)
			sslot->values_arr = statarray;
		else
			pfree(statarray);

		ReleaseSysCache(typeTuple);
	}

	if (flags & ATTSTATSSLOT_NUMBERS)
	{
		val = SysCacheGetAttrNotNull(STATRELATTINH, statstuple,
									 Anum_pg_statistic_stanumbers1 + i);

		/*
		 * 必要时解 toast 数组，且无论如何都复制一份
		 * 副本归本 AttStatsSlot 控制。
		 */
		statarray = DatumGetArrayTypePCopy(val);

		/*
		 * 我们预期该数组为一维 float4 数组；请加以验证。
		 * 无需使用 deconstruct_array()，因为数组数据
		 * 不过是 C 的 float4 数组而已。
		 */
		narrayelem = ARR_DIMS(statarray)[0];
		if (ARR_NDIM(statarray) != 1 || narrayelem <= 0 ||
			ARR_HASNULL(statarray) ||
			ARR_ELEMTYPE(statarray) != FLOAT4OID)
			elog(ERROR, "stanumbers is not a 1-D float4 array");

		/* 把 statarray 内部指针直接交给调用者 */
		sslot->numbers = (float4 *) ARR_DATA_PTR(statarray);
		sslot->nnumbers = narrayelem;

		/* statarray 将由 free_attstatsslot 释放 */
		sslot->numbers_arr = statarray;
	}

	return true;
}

/*
 * free_attstatsslot
 *		释放 get_attstatsslot 分配的数据
 */
void
free_attstatsslot(AttStatsSlot *sslot)
{
	/* values[] 数组由 deconstruct_array 单独 palloc 而来 */
	if (sslot->values)
		pfree(sslot->values);
	/* numbers[] 数组指向 numbers_arr 内部，勿 pfree */
	/* 释放解 toast 对象（如有） */
	if (sslot->values_arr)
		pfree(sslot->values_arr);
	if (sslot->numbers_arr)
		pfree(sslot->numbers_arr);
}

/* 				---------- PG_NAMESPACE 缓存 ---------- */

/*
 * get_namespace_name
 *		返回给定命名空间的名称
 *
 * 返回字符串的 palloc 副本；无此命名空间则得 NULL。
 */
char *
get_namespace_name(Oid nspid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nspid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_namespace nsptup = (Form_pg_namespace) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(nsptup->nspname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_namespace_name_or_temp
 *		同上，但若它是本后端的临时命名空间，则返回
 *		"pg_temp"。
 */
char *
get_namespace_name_or_temp(Oid nspid)
{
	if (isTempNamespace(nspid))
		return pstrdup("pg_temp");
	else
		return get_namespace_name(nspid);
}

/* 				---------- PG_RANGE 缓存 ---------- */

/*
 * get_range_subtype
 *		返回给定范围类型的子类型
 *
 * 若类型不是范围类型则返回 InvalidOid。
 */
Oid
get_range_subtype(Oid rangeOid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(rangeOid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_range rngtup = (Form_pg_range) GETSTRUCT(tp);
		Oid			result;

		result = rngtup->rngsubtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_range_collation
 *		返回给定范围类型的排序规则
 *
 * 若类型不是范围类型则返回 InvalidOid，
 * 或其子类型不可排序。
 */
Oid
get_range_collation(Oid rangeOid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(rangeOid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_range rngtup = (Form_pg_range) GETSTRUCT(tp);
		Oid			result;

		result = rngtup->rngcollation;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_range_multirange
 *		返回给定范围类型的多范围类型
 *
 * 若类型不是范围类型则返回 InvalidOid。
 */
Oid
get_range_multirange(Oid rangeOid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(rangeOid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_range rngtup = (Form_pg_range) GETSTRUCT(tp);
		Oid			result;

		result = rngtup->rngmultitypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_multirange_range
 *		返回给定多范围的范围类型
 *
 * 若该类型不是多范围则返回 InvalidOid。
 */
Oid
get_multirange_range(Oid multirangeOid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RANGEMULTIRANGE, ObjectIdGetDatum(multirangeOid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_range rngtup = (Form_pg_range) GETSTRUCT(tp);
		Oid			result;

		result = rngtup->rngtypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/* 				---------- PG_INDEX 缓存 ---------- */

/*
 * get_index_column_opclass
 *
 *		给定索引 OID 与列号，
 *		返回该索引列的算子类
 *			若索引未找到则返回 InvalidOid
 *				或该列不是键列。
 */
Oid
get_index_column_opclass(Oid index_oid, int attno)
{
	HeapTuple	tuple;
	Form_pg_index rd_index;
	Datum		datum;
	oidvector  *indclass;
	Oid			opclass;

	/* 首先需要知道该列的算子类。 */

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	rd_index = (Form_pg_index) GETSTRUCT(tuple);

	/* 调用者应当保证这一点 */
	Assert(attno > 0 && attno <= rd_index->indnatts);

	/* 非键属性没有算子类 */
	if (attno > rd_index->indnkeyatts)
	{
		ReleaseSysCache(tuple);
		return InvalidOid;
	}

	datum = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indclass);
	indclass = ((oidvector *) DatumGetPointer(datum));

	Assert(attno <= indclass->dim1);
	opclass = indclass->values[attno - 1];

	ReleaseSysCache(tuple);

	return opclass;
}

/*
 * get_index_isreplident
 *
 *		给定索引 OID，返回 pg_index.indisreplident。
 */
bool
get_index_isreplident(Oid index_oid)
{
	HeapTuple	tuple;
	Form_pg_index rd_index;
	bool		result;

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tuple))
		return false;

	rd_index = (Form_pg_index) GETSTRUCT(tuple);
	result = rd_index->indisreplident;
	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_index_isvalid
 *
 *		给定索引 OID，返回 pg_index.indisvalid。
 */
bool
get_index_isvalid(Oid index_oid)
{
	bool		isvalid;
	HeapTuple	tuple;
	Form_pg_index rd_index;

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", index_oid);

	rd_index = (Form_pg_index) GETSTRUCT(tuple);
	isvalid = rd_index->indisvalid;
	ReleaseSysCache(tuple);

	return isvalid;
}

/*
 * get_index_isclustered
 *
 *		给定索引 OID，返回 pg_index.indisclustered。
 */
bool
get_index_isclustered(Oid index_oid)
{
	bool		isclustered;
	HeapTuple	tuple;
	Form_pg_index rd_index;

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", index_oid);

	rd_index = (Form_pg_index) GETSTRUCT(tuple);
	isclustered = rd_index->indisclustered;
	ReleaseSysCache(tuple);

	return isclustered;
}

/*
 * get_publication_oid - 给定发布名称，查找其 OID
 *
 * missing_ok 为 false 时找不到名称即报错；true 则仅返回
 * InvalidOid。
 */
Oid
get_publication_oid(const char *pubname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(PUBLICATIONNAME, Anum_pg_publication_oid,
						  CStringGetDatum(pubname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", pubname)));
	return oid;
}

/*
 * get_publication_name - 给定发布 OID，查找其名称
 *
 * missing_ok 为 false 时找不到名称即报错；true 则仅返回
 * NULL。
 */
char *
get_publication_name(Oid pubid, bool missing_ok)
{
	HeapTuple	tup;
	char	   *pubname;
	Form_pg_publication pubform;

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for publication %u", pubid);
		return NULL;
	}

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	pubname = pstrdup(NameStr(pubform->pubname));

	ReleaseSysCache(tup);

	return pubname;
}

/*
 * get_subscription_oid - 给定订阅名称，查找其 OID
 *
 * missing_ok 为 false 时找不到名称即报错；true 则仅返回
 * InvalidOid。
 */
Oid
get_subscription_oid(const char *subname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid2(SUBSCRIPTIONNAME, Anum_pg_subscription_oid,
						  MyDatabaseId, CStringGetDatum(subname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist", subname)));
	return oid;
}

/*
 * get_subscription_name - 给定订阅 OID，查找其名称
 *
 * missing_ok 为 false 时找不到名称即报错；true 则仅返回
 * NULL。
 */
char *
get_subscription_name(Oid subid, bool missing_ok)
{
	HeapTuple	tup;
	char	   *subname;
	Form_pg_subscription subform;

	tup = SearchSysCache1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for subscription %u", subid);
		return NULL;
	}

	subform = (Form_pg_subscription) GETSTRUCT(tup);
	subname = pstrdup(NameStr(subform->subname));

	ReleaseSysCache(tup);

	return subname;
}
