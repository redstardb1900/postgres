/*-------------------------------------------------------------------------
 *
 * hashvalidate.c
 *	  hash 的操作符类校验器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


static bool check_hash_func_signature(Oid funcid, int16 amprocnum, Oid argtype);


/*
 * hash 操作符类的校验器。
 *
 * 这里的某些检查覆盖整个操作符族，因此在检查
 * 族中每个操作符类时是重复的。但它们运行时间
 * 不够长，算不上什么问题，所以我们接受这种重复，
 * 而不是把 amvalidate API 搞复杂。
 */
bool
hashvalidate(Oid opclassoid)
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
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	List	   *hashabletypes = NIL;
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

		/*
		 * 所有哈希函数都应以匹配的左/右类型
		 * 注册
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "hash",
							format_procedure(procform->amproc))));
			result = false;
		}

		/* 检查过程编号和函数签名 */
		switch (procform->amprocnum)
		{
			case HASHSTANDARD_PROC:
			case HASHEXTENDED_PROC:
				if (!check_hash_func_signature(procform->amproc, procform->amprocnum,
											   procform->amproclefttype))
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
									opfamilyname, "hash",
									format_procedure(procform->amproc),
									procform->amprocnum)));
					result = false;
				}
				else
				{
					/* 记住我们可以做哈希的类型 */
					hashabletypes =
						list_append_unique_oid(hashabletypes,
											   procform->amproclefttype);
				}
				break;
			case HASHOPTIONS_PROC:
				if (!check_amoptsproc_signature(procform->amproc))
					result = false;
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "hash",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				break;
		}
	}

	/* 检查各个操作符 */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* 检查只存在允许的策略编号 */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > HTMaxStrategyNumber)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "hash",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* hash 不支持 ORDER BY 操作符 */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "hash",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* 检查操作符签名 --- 对所有 hash 策略都相同 */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "hash",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* 每种数据类型都应有相应的哈希函数 */
		if (!list_member_oid(hashabletypes, oprform->amoplefttype) ||
			!list_member_oid(hashabletypes, oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s lacks support function for operator %s",
							opfamilyname, "hash",
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
		 * 若这一数据类型对的操作符集合看起来不完整
		 * （意味着有哈希函数但没有操作符），
		 * 则报告问题。
		 */
		if (thisgroup->operatorset != (1 << HTEqualStrategyNumber))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "hash",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
	}

	/* 检查原先命名的操作符类是否受支持 */
	/* （若该组存在，上面已经充分检查过） */
	if (!opclassgroup)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "hash")));
		result = false;
	}

	/*
	 * 若操作符族并未为其支持的数据类型的全部可能组合
	 * 都提供条目，则报告问题。缺少跨类型操作符
	 * 并非致命，但坚持所有内建 hash 操作符族
	 * 都完整，看起来是合理的。
	 */
	if (list_length(grouplist) !=
		list_length(hashabletypes) * list_length(hashabletypes))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator family \"%s\" of access method %s is missing cross-type operator(s)",
						opfamilyname, "hash")));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}


/*
 * 由于核心 hash 操作符类定义中有各种特殊处理，
 * 我们需要 check_amproc_signature 的定制版本。
 */
static bool
check_hash_func_signature(Oid funcid, int16 amprocnum, Oid argtype)
{
	bool		result = true;
	Oid			restype;
	int16		nargs;
	HeapTuple	tp;
	Form_pg_proc procform;

	switch (amprocnum)
	{
		case HASHSTANDARD_PROC:
			restype = INT4OID;
			nargs = 1;
			break;

		case HASHEXTENDED_PROC:
			restype = INT8OID;
			nargs = 2;
			break;

		default:
			elog(ERROR, "invalid amprocnum");
	}

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	if (procform->prorettype != restype || procform->proretset ||
		procform->pronargs != nargs)
		result = false;

	if (!IsBinaryCoercible(argtype, procform->proargtypes.values[0]))
	{
		/*
		 * 某些内建 hash 操作符类取巧，使用与操作符类
		 * 数据类型不同但物理上兼容的哈希函数。其中一些
		 * 情况下，连 "二进制可强制转换" 检查也会失败，
		 * 因为没有相应的类型转换。目前用一份允许情形
		 * 清单来处理。要测试具体的函数身份，而不只是
		 * 其输入类型，因为 hashvarlena() 接受 INTERNAL，
		 * 允许任何这样的函数似乎过于危险。
		 */
		if ((funcid == F_HASHINT4 || funcid == F_HASHINT4EXTENDED) &&
			(argtype == DATEOID ||
			 argtype == XIDOID || argtype == CIDOID))
			 /* 可以，允许使用 hashint4() */ ;
		else if ((funcid == F_HASHINT8 || funcid == F_HASHINT8EXTENDED) &&
				 (argtype == XID8OID))
			 /* 可以，允许使用 hashint8() */ ;
		else if ((funcid == F_TIMESTAMP_HASH ||
				  funcid == F_TIMESTAMP_HASH_EXTENDED) &&
				 argtype == TIMESTAMPTZOID)
			 /* 可以，允许使用 timestamp_hash() */ ;
		else if ((funcid == F_HASHCHAR || funcid == F_HASHCHAREXTENDED) &&
				 argtype == BOOLOID)
			 /* 可以，允许使用 hashchar() */ ;
		else if ((funcid == F_HASHVARLENA || funcid == F_HASHVARLENAEXTENDED) &&
				 argtype == BYTEAOID)
			 /* 可以，允许使用 hashvarlena() */ ;
		else
			result = false;
	}

	/* 若函数接受第二个参数，它必须是 64 位盐值。 */
	if (nargs == 2 && procform->proargtypes.values[1] != INT8OID)
		result = false;

	ReleaseSysCache(tp);
	return result;
}

/*
 * 向 hash 操作符族添加操作符/函数之前的预检查函数。
 */
void
hashadjustmembers(Oid opfamilyoid,
				  Oid opclassoid,
				  List *operators,
				  List *functions)
{
	Oid			opcintype;
	ListCell   *lc;

	/*
	 * 哈希操作符和必需的支持函数若是跨类型的，始终是
	 * 操作符族的 "松散" 成员。若不是跨类型的，我们更愿
	 * 把它们绑到合适的操作符类 ... 但若用户尚未创建
	 * 操作符类，就做不到这一点，必须回退为使用操作符族
	 * 依赖。（这种情况下我们绝不能强制创建操作符类，
	 * 因为留下一个不完整的操作符类会很糟糕。抛出错误
	 * 是另一种不可取的选择。）
	 *
	 * 这种行为会带来一点转储/重新加载隐患，因为恢复
	 * 对象的顺序可能影响我们最终得到的依赖。
	 * pg_dump 的现有行为在多数情况下会保留这些
	 * 依赖选择，但若某个跨类型操作符已被紧密绑定
	 * 到操作符类中则不会。那本来就是个错误，所以
	 * 默默 "修正" 它并不糟糕。
	 *
	 * 可选的支持函数始终是 "松散" 的族成员。
	 *
	 * 为避免重复查找，我们记住最近使用的操作符类的
	 * 输入类型。
	 */
	if (OidIsValid(opclassoid))
	{
		/* CREATE OPERATOR CLASS 期间，需要 CCI 才能看到 pg_opclass 行 */
		CommandCounterIncrement();
		opcintype = get_opclass_input_type(opclassoid);
	}
	else
		opcintype = InvalidOid;

	/*
	 * 操作符和支持函数的处理几乎相同，因此与其
	 * 重复这段代码，不如直接把两个列表接起来。
	 */
	foreach(lc, list_concat_copy(operators, functions))
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		if (op->is_func && op->number != HASHSTANDARD_PROC)
		{
			/* 可选支持过程，因此始终是软的族依赖 */
			op->ref_is_hard = false;
			op->ref_is_family = true;
			op->refobjid = opfamilyoid;
		}
		else if (op->lefttype != op->righttype)
		{
			/* 跨类型，因此始终是软的族依赖 */
			op->ref_is_hard = false;
			op->ref_is_family = true;
			op->refobjid = opfamilyoid;
		}
		else
		{
			/* 不是跨类型；有合适的操作符类吗？ */
			if (op->lefttype != opcintype)
			{
				/* 避免重复这次昂贵的查找，即使它失败 */
				opcintype = op->lefttype;
				opclassoid = opclass_for_family_datatype(HASH_AM_OID,
														 opfamilyoid,
														 opcintype);
			}
			if (OidIsValid(opclassoid))
			{
				/* 对操作符类的硬依赖 */
				op->ref_is_hard = true;
				op->ref_is_family = false;
				op->refobjid = opclassoid;
			}
			else
			{
				/* 没有别的办法，因此对操作符族建立软依赖 */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
			}
		}
	}
}
