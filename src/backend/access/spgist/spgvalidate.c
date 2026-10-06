/*-------------------------------------------------------------------------
 *
 * spgvalidate.c
 *	  SP-GiST 的操作符类校验器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/spgist_private.h"
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
 * SP-GiST 操作符类的校验器。
 *
 * 这里做的部分检查覆盖整个操作符族，因此在检查
 * 族中每个操作符类时会有冗余。但它们跑得不够久，
 * 构不成大问题，所以我们接受这种重复，而不去
 * 把 amvalidate API 搞得更复杂。
 */
bool
spgvalidate(Oid opclassoid)
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
	spgConfigIn configIn;
	spgConfigOut configOut;
	Oid			configOutLefttype = InvalidOid;
	Oid			configOutRighttype = InvalidOid;
	Oid			configOutLeafType = InvalidOid;

	/* 获取操作符类信息 */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
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
	grouplist = identify_opfamily_groups(oprlist, proclist);

	/* 检查各个支持函数 */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/*
		 * 所有 SP-GiST 支持函数都应当以相互匹配的
		 * 左/右类型登记
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "spgist",
							format_procedure(procform->amproc))));
			result = false;
		}

		/* 检查过程编号和函数签名 */
		switch (procform->amprocnum)
		{
			case SPGIST_CONFIG_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				configIn.attType = procform->amproclefttype;
				memset(&configOut, 0, sizeof(configOut));

				OidFunctionCall2(procform->amproc,
								 PointerGetDatum(&configIn),
								 PointerGetDatum(&configOut));

				configOutLefttype = procform->amproclefttype;
				configOutRighttype = procform->amprocrighttype;

				/* 默认叶子类型是 opckeytype 或输入类型 */
				if (OidIsValid(opckeytype))
					configOutLeafType = opckeytype;
				else
					configOutLeafType = procform->amproclefttype;

				/* 若指定了其他叶子 datum 类型，则警告 */
				if (OidIsValid(configOut.leafType) &&
					configOutLeafType != configOut.leafType)
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("SP-GiST leaf data type %s does not match declared type %s",
									format_type_be(configOut.leafType),
									format_type_be(configOutLeafType))));
					result = false;
					configOutLeafType = configOut.leafType;
				}

				/*
				 * 当叶子类型与属性类型相同时，不需要
				 * compress 函数，我们会在 functionset 中置上
				 * 相应位，供稍后的组一致性检查使用。
				 */
				if (configOutLeafType == configIn.attType)
				{
					foreach(lc, grouplist)
					{
						OpFamilyOpFuncGroup *group = lfirst(lc);

						if (group->lefttype == procform->amproclefttype &&
							group->righttype == procform->amprocrighttype)
						{
							group->functionset |=
								((uint64) 1) << SPGIST_COMPRESS_PROC;
							break;
						}
					}
				}
				break;
			case SPGIST_CHOOSE_PROC:
			case SPGIST_PICKSPLIT_PROC:
			case SPGIST_INNER_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case SPGIST_LEAF_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case SPGIST_COMPRESS_PROC:
				if (configOutLefttype != procform->amproclefttype ||
					configOutRighttype != procform->amprocrighttype)
					ok = false;
				else
					ok = check_amproc_signature(procform->amproc,
												configOutLeafType, true,
												1, 1, procform->amproclefttype);
				break;
			case SPGIST_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "spgist",
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
							opfamilyname, "spgist",
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
		if (oprform->amopstrategy < 1 || oprform->amopstrategy > 63)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "spgist",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* spgist 支持 ORDER BY 操作符 */
		if (oprform->amoppurpose != AMOP_SEARCH)
		{
			/* ... 且操作符结果类型必须匹配所声明的 btree 操作符族 */
			op_rettype = get_op_rettype(oprform->amopopr);
			if (!opfamily_can_sort_type(oprform->amopsortfamily, op_rettype))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
								opfamilyname, "spgist",
								format_operator(oprform->amopopr))));
				result = false;
			}
		}
		else
			op_rettype = BOOLOID;

		/* 检查操作符签名 --- 对所有 spgist 策略都相同 */
		if (!check_amop_signature(oprform->amopopr, op_rettype,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "spgist",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* 现在检查不一致的操作符/函数组 */
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* 记住与被测操作符类精确匹配的那一组 */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * 若有任何数据类型对有函数却没有操作符，就
		 * 抱怨。就目前而言，这是我们能用来检测
		 * 缺少操作符的最好办法。
		 */
		if (thisgroup->operatorset == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "spgist",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}

		/*
		 * 若任何数据类型缺少函数就抱怨，记住
		 * SP-GiST 不使用跨类型支持函数。
		 */
		if (thisgroup->lefttype != thisgroup->righttype)
			continue;

		for (i = 1; i <= SPGISTNProc; i++)
		{
			if ((thisgroup->functionset & (((uint64) 1) << i)) != 0)
				continue;		/* 已有 */
			if (i == SPGIST_OPTIONS_PROC)
				continue;		/* 可选方法 */
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function %d for type %s",
							opfamilyname, "spgist", i,
							format_type_be(thisgroup->lefttype))));
			result = false;
		}
	}

	/* 检查原先命名的操作符类是否受支持 */
	/* （若组已存在，我们上面已充分检查过） */
	if (!opclassgroup)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "spgist")));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * 向 SP-GiST 操作符族添加操作符/函数之前的预检查函数。
 */
void
spgadjustmembers(Oid opfamilyoid,
				 Oid opclassoid,
				 List *operators,
				 List *functions)
{
	ListCell   *lc;

	/*
	 * SP-GiST 操作符族的操作符成员绝不应当有硬
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
	 * 操作符族。（鉴于 SP-GiST 操作符类通常不共享
	 * 操作符族，似乎不值得为此再费更多功夫。）
	 */
	foreach(lc, functions)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		switch (op->number)
		{
			case SPGIST_CONFIG_PROC:
			case SPGIST_CHOOSE_PROC:
			case SPGIST_PICKSPLIT_PROC:
			case SPGIST_INNER_CONSISTENT_PROC:
			case SPGIST_LEAF_CONSISTENT_PROC:
				/* 必需的支持函数 */
				op->ref_is_hard = true;
				break;
			case SPGIST_COMPRESS_PROC:
			case SPGIST_OPTIONS_PROC:
				/* 可选，因此强制它成为软的族依赖 */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("support function number %d is invalid for access method %s",
								op->number, "spgist")));
				break;
		}
	}
}
