/*-------------------------------------------------------------------------
 *
 * nbtvalidate.c
 *	  btree 的操作符类校验器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
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
 * btree 操作符类的校验器。
 *
 * 这里的某些检查覆盖整个操作符族，因此在检查
 * 族中每个操作符类时是重复的。但它们运行时间
 * 不够长，算不上什么问题，所以我们接受这种重复，
 * 而不是把 amvalidate API 搞复杂。
 */
bool
btvalidate(Oid opclassoid)
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
	List	   *familytypes;
	int			usefulgroups;
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
			case BTORDER_PROC:
				ok = check_amproc_signature(procform->amproc, INT4OID, true,
											2, 2, procform->amproclefttype,
											procform->amprocrighttype);
				break;
			case BTSORTSUPPORT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											1, 1, INTERNALOID);
				break;
			case BTINRANGE_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											5, 5,
											procform->amproclefttype,
											procform->amproclefttype,
											procform->amprocrighttype,
											BOOLOID, BOOLOID);
				break;
			case BTEQUALIMAGE_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											1, 1, OIDOID);
				break;
			case BTOPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "btree",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* 不想再输出额外消息 */
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "btree",
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

		/* 检查只存在允许的策略编号 */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > BTMaxStrategyNumber)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "btree",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* btree 不支持 ORDER BY 操作符 */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "btree",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* 检查操作符签名 --- 对所有 btree 策略都相同 */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "btree",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* 现在检查不一致的操作符/函数组 */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	usefulgroups = 0;
	opclassgroup = NULL;
	familytypes = NIL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/*
		 * in_range 支持函数的右侧类型有可能在其他方面
		 * 与操作符族无关 --- 例如，SQL 需要 datetime_ops
		 * 操作符类提供以 interval 为偏移的范围支持。
		 * 因此，若这一组看起来只包含一个 in_range
		 * 函数，就忽略它：它并不代表一对受支持的
		 * 类型。
		 */
		if (thisgroup->operatorset == 0 &&
			thisgroup->functionset == (1 << BTINRANGE_PROC))
			continue;

		/* 否则把它算作相关的一组 */
		usefulgroups++;

		/* 记住与被测操作符类精确匹配的那一组 */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * 找出本操作符族所处理的全部不同的数据类型。这一
		 * 实现是 O(N^2)，但族中的类型不太可能多到
		 * 让这一点成为问题。
		 */
		familytypes = list_append_unique_oid(familytypes, thisgroup->lefttype);
		familytypes = list_append_unique_oid(familytypes, thisgroup->righttype);

		/*
		 * 若这一对数据类型似乎缺少完整的操作符集合
		 * 或支持函数集合，则报告问题。sortsupport、
		 * in_range 与 equalimage 函数视为可选。
		 */
		if (thisgroup->operatorset !=
			((1 << BTLessStrategyNumber) |
			 (1 << BTLessEqualStrategyNumber) |
			 (1 << BTEqualStrategyNumber) |
			 (1 << BTGreaterEqualStrategyNumber) |
			 (1 << BTGreaterStrategyNumber)))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "btree",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
		if ((thisgroup->functionset & (1 << BTORDER_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function for types %s and %s",
							opfamilyname, "btree",
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
						opclassname, "btree")));
		result = false;
	}

	/*
	 * 若操作符族并未为其支持的数据类型的全部可能组合
	 * 都提供条目，则报告问题。缺少跨类型操作符
	 * 并非致命，但会限制规划器从等价类推导额外
	 * 限定子句的能力，因此坚持所有内建 btree
	 * 操作符族都完整，看起来是合理的。
	 */
	if (usefulgroups != (list_length(familytypes) * list_length(familytypes)))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator family \"%s\" of access method %s is missing cross-type operator(s)",
						opfamilyname, "btree")));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * 向 btree 操作符族添加操作符/函数之前的预检查函数。
 */
void
btadjustmembers(Oid opfamilyoid,
				Oid opclassoid,
				List *operators,
				List *functions)
{
	Oid			opcintype;
	ListCell   *lc;

	/*
	 * btree 操作符和比较支持函数若是跨类型的，始终是
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

		if (op->is_func && op->number != BTORDER_PROC)
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
				opclassoid = opclass_for_family_datatype(BTREE_AM_OID,
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
