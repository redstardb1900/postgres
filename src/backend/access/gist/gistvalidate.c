/*-------------------------------------------------------------------------
 *
 * gistvalidate.c
 *	  GiST 的操作符类校验器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/gist_private.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


/*
 * GiST 操作符类的校验器。
 */
bool
gistvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	HeapTuple	familytup;
	Form_pg_opfamily familyform;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
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
	opckeytype = classform->opckeytype;
	if (!OidIsValid(opckeytype))
		opckeytype = opcintype;
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

		/*
		 * 所有 GiST 支持函数都应当以相互匹配的
		 * 左/右类型登记
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "gist",
							format_procedure(procform->amproc))));
			result = false;
		}

		/*
		 * 除了在具体的操作符类内部，我们无法检查签名，因为
		 * 许多情况下需要知道相关联的 opckeytype。
		 */
		if (procform->amproclefttype != opcintype)
			continue;

		/* 检查过程编号和函数签名 */
		switch (procform->amprocnum)
		{
			case GIST_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, false,
											5, 5, INTERNALOID, opcintype,
											INT2OID, OIDOID, INTERNALOID);
				break;
			case GIST_UNION_PROC:
				ok = check_amproc_signature(procform->amproc, opckeytype, false,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case GIST_COMPRESS_PROC:
			case GIST_DECOMPRESS_PROC:
			case GIST_FETCH_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											1, 1, INTERNALOID);
				break;
			case GIST_PENALTY_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											3, 3, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case GIST_PICKSPLIT_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case GIST_EQUAL_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, false,
											3, 3, opckeytype, opckeytype,
											INTERNALOID);
				break;
			case GIST_DISTANCE_PROC:
				ok = check_amproc_signature(procform->amproc, FLOAT8OID, false,
											5, 5, INTERNALOID, opcintype,
											INT2OID, OIDOID, INTERNALOID);
				break;
			case GIST_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			case GIST_SORTSUPPORT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											1, 1, INTERNALOID);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "gist",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* 不想再发额外消息 */
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "gist",
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}
	}

	/* 检查各个操作符 */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);
		Oid			op_rettype;

		/* TODO: 检查是否只存在允许的策略编号 */
		if (oprform->amopstrategy < 1)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "gist",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* GiST 支持 ORDER BY 操作符 */
		if (oprform->amoppurpose != AMOP_SEARCH)
		{
			/* ... 但必须有匹配的距离过程 */
			if (!OidIsValid(get_opfamily_proc(opfamilyoid,
											  oprform->amoplefttype,
											  oprform->amoplefttype,
											  GIST_DISTANCE_PROC)))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains unsupported ORDER BY specification for operator %s",
								opfamilyname, "gist",
								format_operator(oprform->amopopr))));
				result = false;
			}
			/* ... 且操作符结果类型必须匹配所声明的 btree 操作符族 */
			op_rettype = get_op_rettype(oprform->amopopr);
			if (!opfamily_can_sort_type(oprform->amopsortfamily, op_rettype))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains incorrect ORDER BY opfamily specification for operator %s",
								opfamilyname, "gist",
								format_operator(oprform->amopopr))));
				result = false;
			}
		}
		else
		{
			/* 搜索操作符必须始终返回 bool */
			op_rettype = BOOLOID;
		}

		/* 检查操作符签名 */
		if (!check_amop_signature(oprform->amopopr, op_rettype,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "gist",
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
		 * 对操作符集合能做的检查不多，因为每个
		 * GiST 操作符类大体上自成一套，而且有些只包含
		 * 与操作符类数据类型二进制兼容的操作符
		 * （这意味着空的操作符集合也可以接受）。这种情况
		 * 也意味着，除了操作符类自己的那一组之外，我们
		 * 不应坚持函数集合非空。
		 */
	}

	/* 检查原先命名的操作符类是否完整 */
	for (i = 1; i <= GISTNProcs; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((uint64) 1) << i)) != 0)
			continue;			/* 已有 */
		if (i == GIST_DISTANCE_PROC || i == GIST_FETCH_PROC ||
			i == GIST_COMPRESS_PROC || i == GIST_DECOMPRESS_PROC ||
			i == GIST_OPTIONS_PROC || i == GIST_SORTSUPPORT_PROC)
			continue;			/* 可选方法 */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d",
						opclassname, "gist", i)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * 向 GiST 操作符族添加操作符/函数之前的预检查函数。
 */
void
gistadjustmembers(Oid opfamilyoid,
				  Oid opclassoid,
				  List *operators,
				  List *functions)
{
	ListCell   *lc;

	/*
	 * GiST 操作符族的操作符成员绝不应当有硬
	 * 依赖，因为它们与操作符族的联系只取决于
	 * 支持函数的判断，而这是可以改变的。为
	 * 保持一致，我们让所有软依赖都指向操作符
	 * 族，不过在 CREATE OPERATOR CLASS 的
	 * 情形下，指向操作符类的软依赖同样可行。
	 */
	foreach(lc, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		op->ref_is_hard = false;
		op->ref_is_family = true;
		op->refobjid = opfamilyoid;
	}

	/*
	 * 必需的支持函数应当有硬依赖。最好这些只是
	 * 对操作符类的依赖，但若我们处于 ALTER
	 * OPERATOR FAMILY 之中，就让依赖指向整个
	 * 操作符族。（鉴于 GiST 操作符类通常不共享
	 * 操作符族，似乎不值得为此再费更多功夫。）
	 */
	foreach(lc, functions)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		switch (op->number)
		{
			case GIST_CONSISTENT_PROC:
			case GIST_UNION_PROC:
			case GIST_PENALTY_PROC:
			case GIST_PICKSPLIT_PROC:
			case GIST_EQUAL_PROC:
				/* 必需的支持函数 */
				op->ref_is_hard = true;
				break;
			case GIST_COMPRESS_PROC:
			case GIST_DECOMPRESS_PROC:
			case GIST_DISTANCE_PROC:
			case GIST_FETCH_PROC:
			case GIST_OPTIONS_PROC:
			case GIST_SORTSUPPORT_PROC:
				/* 可选，因此强制它成为软的族依赖 */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("support function number %d is invalid for access method %s",
								op->number, "gist")));
				break;
		}
	}
}
