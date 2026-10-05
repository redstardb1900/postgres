/*-------------------------------------------------------------------------
 *
 * brin_validate.c
 *	  BRIN 的操作符类校验器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_validate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/brin_internal.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * BRIN 操作符类的校验器。
 *
 * 这里的某些检查覆盖整个操作符族，因此在检查
 * 族中每个操作符类时是重复的。但它们运行时间
 * 不够长，算不上什么问题，所以我们接受这种重复，
 * 而不是把 amvalidate API 搞复杂。
 */
bool
brinvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	char	   *opclassname;
	HeapTuple	familytup;
	Form_pg_opfamily familyform;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	uint64		allfuncs = 0;
	uint64		allops = 0;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	int			i;
	ListCell   *lc;

	/* 获取操作符类信息 */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opclassname = NameStr(classform->opcname);

	/* 获取操作符族信息 */
	familytup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamilyoid));
	if (!HeapTupleIsValid(familytup))
		elog(ERROR, "cache lookup failed for operator family %u", opfamilyoid);
	familyform = (Form_pg_opfamily) GETSTRUCT(familytup);

	opfamilyname = NameStr(familyform->opfname);

	/* 获取该操作符族的全部操作符和支持函数 */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/* 检查各个支持函数 */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/* 检查过程编号和函数签名 */
		switch (procform->amprocnum)
		{
			case BRIN_PROCNUM_OPCINFO:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											1, 1, INTERNALOID);
				break;
			case BRIN_PROCNUM_ADDVALUE:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											4, 4, INTERNALOID, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case BRIN_PROCNUM_CONSISTENT:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											3, 4, INTERNALOID, INTERNALOID,
											INTERNALOID, INT4OID);
				break;
			case BRIN_PROCNUM_UNION:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											3, 3, INTERNALOID, INTERNALOID,
											INTERNALOID);
				break;
			case BRIN_PROCNUM_OPTIONS:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				/* 若不是有效的可选过程编号则报错 */
				if (procform->amprocnum < BRIN_FIRST_OPTIONAL_PROCNUM ||
					procform->amprocnum > BRIN_LAST_OPTIONAL_PROCNUM)
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
									opfamilyname, "brin",
									format_procedure(procform->amproc),
									procform->amprocnum)));
					result = false;
					continue;	/* 把坏的过程编号从 allfuncs 中略去 */
				}
				/* 无法检查可选过程的签名，故假定没问题 */
				ok = true;
				break;
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "brin",
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}

		/* 记录在操作符族中见过的全部有效过程编号 */
		allfuncs |= ((uint64) 1) << procform->amprocnum;
	}

	/* 检查各个操作符 */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* 检查只存在允许的策略编号 */
		if (oprform->amopstrategy < 1 || oprform->amopstrategy > 63)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "brin",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}
		else
		{
			/*
			 * 各 BRIN 操作符族所提供的操作符集合并不相同。
			 * 我们的计划是找出该操作符族中用到的全部
			 * 操作符策略编号，再对缺少任一操作符（operator(s)）的
			 * 数据类型组合报错。不过只考虑出现在某些
			 * 非跨类型情形中的编号，因为跨类型操作符
			 * 可能拥有独有的策略。（这并不是很好的
			 * 启发式：尤其是跨类型操作符里用错的编号
			 * 不会被注意到；但核心 BRIN
			 * 操作符族已经乱到有必要这样做。）
			 */
			if (oprform->amoplefttype == oprform->amoprighttype)
				allops |= ((uint64) 1) << oprform->amopstrategy;
		}

		/* brin 不支持 ORDER BY 操作符 */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "brin",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* 检查操作符签名 --- 对所有 brin 策略都相同 */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "brin",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* 现在检查不一致的操作符/函数组 */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* 记住与被测操作符类精确匹配的那一组 */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * 有些 BRIN 操作符族预期存在跨类型支持函数，
		 * 有些则不预期。我们并不确切知道是哪些，因此若
		 * 发现某个跨类型操作符完全没有支持函数，
		 * 就放行。（也不要预期此类跨类型情形下
		 * 全部操作符都存在。）
		 */
		if (thisgroup->functionset == 0 &&
			thisgroup->lefttype != thisgroup->righttype)
			continue;

		/*
		 * 否则，若这一数据类型对的操作符集合或支持函数
		 * 集合看起来不完整，则报错。
		 */
		if (thisgroup->operatorset != allops)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "brin",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
		if (thisgroup->functionset != allfuncs)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function(s) for types %s and %s",
							opfamilyname, "brin",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
	}

	/* 检查原先命名的操作符类是否完整 */
	if (!opclassgroup || opclassgroup->operatorset != allops)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "brin")));
		result = false;
	}
	for (i = 1; i <= BRIN_MANDATORY_NPROCS; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((int64) 1) << i)) != 0)
			continue;			/* 已有 */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d",
						opclassname, "brin", i)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}
