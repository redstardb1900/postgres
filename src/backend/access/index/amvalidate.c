/*-------------------------------------------------------------------------
 *
 * amvalidate.c
 *	  索引访问方法 amvalidate 与
 *	  amadjustmembers 函数的支持例程。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/amvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/syscache.h"


/*
 * identify_opfamily_groups() 返回 OpFamilyOpFuncGroup 结构的 List，
 * 族内操作符与支持函数列表中，每种 lefttype/righttype
 * 组合各一个。若该数据类型组合存在 amopstrategy K，
 * 则在 operatorset 中设置位 1 << K，支持函数同理。
 * 借助 uint64 字段，操作符与函数编号最大可到 63，
 * 对可预见的将来足够了。
 *
 * 给定的 CatCLists 应表示从 AMOPSTRATEGY 与 AMPROCNUM
 * 缓存取出的单个操作符族，因此按这些缓存的
 * 第二、第三缓存键（即数据类型）排序。
 */
List *
identify_opfamily_groups(CatCList *oprlist, CatCList *proclist)
{
	List	   *result = NIL;
	OpFamilyOpFuncGroup *thisgroup;
	Form_pg_amop oprform;
	Form_pg_amproc procform;
	int			io,
				ip;

	/* 这些列表必须有序；正常运行时应如此 */
	if (!oprlist->ordered || !proclist->ordered)
		elog(ERROR, "cannot validate operator family without ordered data");

	/*
	 * 并发地沿两个列表前进。得益于排序，
	 * 给定数据类型对的全部操作符和函数
	 * 应当连续出现。
	 */
	thisgroup = NULL;
	io = ip = 0;
	if (io < oprlist->n_members)
	{
		oprform = (Form_pg_amop) GETSTRUCT(&oprlist->members[io]->tuple);
		io++;
	}
	else
		oprform = NULL;
	if (ip < proclist->n_members)
	{
		procform = (Form_pg_amproc) GETSTRUCT(&proclist->members[ip]->tuple);
		ip++;
	}
	else
		procform = NULL;

	while (oprform || procform)
	{
		if (oprform && thisgroup &&
			oprform->amoplefttype == thisgroup->lefttype &&
			oprform->amoprighttype == thisgroup->righttype)
		{
			/* 操作符属于当前组；纳入并前进 */

			/* 忽略超出支持范围的策略号 */
			if (oprform->amopstrategy > 0 && oprform->amopstrategy < 64)
				thisgroup->operatorset |= ((uint64) 1) << oprform->amopstrategy;

			if (io < oprlist->n_members)
			{
				oprform = (Form_pg_amop) GETSTRUCT(&oprlist->members[io]->tuple);
				io++;
			}
			else
				oprform = NULL;
			continue;
		}

		if (procform && thisgroup &&
			procform->amproclefttype == thisgroup->lefttype &&
			procform->amprocrighttype == thisgroup->righttype)
		{
			/* 过程属于当前组；纳入并前进 */

			/* 忽略超出支持范围的函数号 */
			if (procform->amprocnum > 0 && procform->amprocnum < 64)
				thisgroup->functionset |= ((uint64) 1) << procform->amprocnum;

			if (ip < proclist->n_members)
			{
				procform = (Form_pg_amproc) GETSTRUCT(&proclist->members[ip]->tuple);
				ip++;
			}
			else
				procform = NULL;
			continue;
		}

		/* 该新建一组了 */
		thisgroup = (OpFamilyOpFuncGroup *) palloc(sizeof(OpFamilyOpFuncGroup));
		if (oprform &&
			(!procform ||
			 (oprform->amoplefttype < procform->amproclefttype ||
			  (oprform->amoplefttype == procform->amproclefttype &&
			   oprform->amoprighttype < procform->amprocrighttype))))
		{
			thisgroup->lefttype = oprform->amoplefttype;
			thisgroup->righttype = oprform->amoprighttype;
		}
		else
		{
			thisgroup->lefttype = procform->amproclefttype;
			thisgroup->righttype = procform->amprocrighttype;
		}
		thisgroup->operatorset = thisgroup->functionset = 0;
		result = lappend(result, thisgroup);
	}

	return result;
}

/*
 * 校验操作符类支持函数的签名（参数类型与结果类型）。
 * 正确则返回 true，否则返回 false。
 *
 * "..." 表示 maxargs 个参数类型 OIDs。若 exact 为真，它们必须与函数
 * 参数类型精确匹配，否则只需二进制可强制转换。无论如何，函数结果类型
 * 都必须与 restype 精确匹配。
 */
bool
check_amproc_signature(Oid funcid, Oid restype, bool exact,
					   int minargs, int maxargs,...)
{
	bool		result = true;
	HeapTuple	tp;
	Form_pg_proc procform;
	va_list		ap;
	int			i;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	if (procform->prorettype != restype || procform->proretset ||
		procform->pronargs < minargs || procform->pronargs > maxargs)
		result = false;

	va_start(ap, maxargs);
	for (i = 0; i < maxargs; i++)
	{
		Oid			argtype = va_arg(ap, Oid);

		if (i >= procform->pronargs)
			continue;
		if (exact ? (argtype != procform->proargtypes.values[i]) :
			!IsBinaryCoercible(argtype, procform->proargtypes.values[i]))
			result = false;
	}
	va_end(ap);

	ReleaseSysCache(tp);
	return result;
}

/*
 * 校验操作符类选项支持函数的签名，它应为
 * 'void(internal)'。
 */
bool
check_amoptsproc_signature(Oid funcid)
{
	return check_amproc_signature(funcid, VOIDOID, true, 1, 1, INTERNALOID);
}

/*
 * 校验操作符类操作符的签名（参数类型与结果类型）。
 * 正确则返回 true，否则返回 false。
 *
 * 目前可以写死为只接受二元操作符。另外，可以坚持类型精确匹配，
 * 因为给定的 lefttype/righttype 来自 pg_amop，
 * 应当始终与该操作符精确匹配。
 */
bool
check_amop_signature(Oid opno, Oid restype, Oid lefttype, Oid righttype)
{
	bool		result = true;
	HeapTuple	tp;
	Form_pg_operator opform;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tp))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for operator %u", opno);
	opform = (Form_pg_operator) GETSTRUCT(tp);

	if (opform->oprresult != restype || opform->oprkind != 'b' ||
		opform->oprleft != lefttype || opform->oprright != righttype)
		result = false;

	ReleaseSysCache(tp);
	return result;
}

/*
 * 取得属于某操作符族、且接受指定类型作为输入类型的操作符类
 * OID。若没有这样的操作符类，则返回 InvalidOid。
 *
 * 若有多个这样的操作符类，得到的是其中随机一个。
 * 这种情况本不该发生，因此不浪费周期去检查。
 *
 * 本可以从操作符族查出 AM 的 OID，但现有调用方都已知道，
 * 或无需额外查找就能得到，所以让它们传入。
 */
Oid
opclass_for_family_datatype(Oid amoid, Oid opfamilyoid, Oid datatypeoid)
{
	Oid			result = InvalidOid;
	CatCList   *opclist;
	int			i;

	/*
	 * 遍历该 AM 的全部操作符类，看是否有匹配的。这有点低效，
	 * 但没有更好的索引可用。这样也省去显式检查
	 * 操作符族是否属于该 AM。
	 */
	opclist = SearchSysCacheList1(CLAAMNAMENSP, ObjectIdGetDatum(amoid));

	for (i = 0; i < opclist->n_members; i++)
	{
		HeapTuple	classtup = &opclist->members[i]->tuple;
		Form_pg_opclass classform = (Form_pg_opclass) GETSTRUCT(classtup);

		if (classform->opcfamily == opfamilyoid &&
			classform->opcintype == datatypeoid)
		{
			result = classform->oid;
			break;
		}
	}

	ReleaseCatCacheList(opclist);

	return result;
}

/*
 * 该数据类型是否为 btree 操作符族的合法输入类型？
 */
bool
opfamily_can_sort_type(Oid opfamilyoid, Oid datatypeoid)
{
	return OidIsValid(opclass_for_family_datatype(BTREE_AM_OID,
												  opfamilyoid,
												  datatypeoid));
}
