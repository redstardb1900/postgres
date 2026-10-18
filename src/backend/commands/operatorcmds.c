/*-------------------------------------------------------------------------
 *
 * operatorcmds.c
 *
 *	  操作符操纵命令的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/operatorcmds.c
 *
 * DESCRIPTION
 *	  "DefineFoo" 例程取出解析树并挑选出
 *	  适当的参数/标志，把结果传给相应的
 *	  "FooDefine" 例程（位于 src/catalog），
 *	  由后者完成真正的目录修改。这些例程
 *	  还会校验用户执行该命令的权限。
 *
 * NOTES
 *	  这些东西必须按如下顺序定义和提交：
 *		"create function"：
 *			输入/输出、recv/send 函数
 *		"create type"：
 *			类型
 *		"create operator"：
 *			操作符
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	ValidateRestrictionEstimator(List *restrictionName);
static Oid	ValidateJoinEstimator(List *joinName);

/*
 * DefineOperator
 *		此函数从解析器生成的参数列表中提取
 *		所有信息，然后让 OperatorCreate()
 *		完成所有实际工作。
 *
 * 'parameters' 是 DefElem 列表
 */
ObjectAddress
DefineOperator(List *names, List *parameters)
{
	char	   *oprName;
	Oid			oprNamespace;
	AclResult	aclresult;
	bool		canMerge = false;	/* 操作符可合并 */
	bool		canHash = false;	/* 操作符可哈希 */
	List	   *functionName = NIL; /* 操作符的函数 */
	TypeName   *typeName1 = NULL;	/* 第一个类型名 */
	TypeName   *typeName2 = NULL;	/* 第二个类型名 */
	Oid			typeId1 = InvalidOid;	/* 转换为 OID 的类型 */
	Oid			typeId2 = InvalidOid;
	Oid			rettype;
	List	   *commutatorName = NIL;	/* 可选的交换操作符名 */
	List	   *negatorName = NIL;	/* 可选的否定操作符名 */
	List	   *restrictionName = NIL;	/* 可选的限制选择率函数 */
	List	   *joinName = NIL; /* 可选的连接选择率函数 */
	Oid			functionOid;	/* 转换为 OID 的函数 */
	Oid			restrictionOid;
	Oid			joinOid;
	Oid			typeId[2];		/* 存放左、右参数 */
	int			nargs;
	ListCell   *pl;

	/* 将名称列表转换为名称与命名空间 */
	oprNamespace = QualifiedNameGetCreationNamespace(names, &oprName);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, oprNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(oprNamespace));

	/*
	 * 遍历定义列表，提取我们需要的信息。
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "leftarg") == 0)
		{
			typeName1 = defGetTypeName(defel);
			if (typeName1->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SETOF type not allowed for operator argument")));
		}
		else if (strcmp(defel->defname, "rightarg") == 0)
		{
			typeName2 = defGetTypeName(defel);
			if (typeName2->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SETOF type not allowed for operator argument")));
		}
		/* "function" 与 "procedure" 在这里等价 */
		else if (strcmp(defel->defname, "function") == 0)
			functionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "procedure") == 0)
			functionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "commutator") == 0)
			commutatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "negator") == 0)
			negatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "restrict") == 0)
			restrictionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "join") == 0)
			joinName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "hashes") == 0)
			canHash = defGetBoolean(defel);
		else if (strcmp(defel->defname, "merges") == 0)
			canMerge = defGetBoolean(defel);
		/* 这些过时选项被视为表示 canMerge */
		else if (strcmp(defel->defname, "sort1") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "sort2") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "ltcmp") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "gtcmp") == 0)
			canMerge = true;
		else
		{
			/* 出于历史向后兼容，用 WARNING 而不是 ERROR */
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" not recognized",
							defel->defname)));
		}
	}

	/*
	 * 确保我们拥有必需的定义
	 */
	if (functionName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator function must be specified")));

	/* 把类型名转换为类型 OID */
	if (typeName1)
		typeId1 = typenameTypeId(NULL, typeName1);
	if (typeName2)
		typeId2 = typenameTypeId(NULL, typeName2);

	/*
	 * 若只缺少右参数，用户很可能想创建后缀操作符，
	 * 所以给他们一个提示，说明为什么这行不通。
	 * 但若两个参数都缺少，就不要提后缀操作符，
	 * 因为用户很可能只是忘了
	 * 提参数。
	 */
	if (!OidIsValid(typeId1) && !OidIsValid(typeId2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator argument types must be specified")));
	if (!OidIsValid(typeId2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator right argument type must be specified"),
				 errdetail("Postfix operators are not supported.")));

	if (typeName1)
	{
		aclresult = object_aclcheck(TypeRelationId, typeId1, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, typeId1);
	}

	if (typeName2)
	{
		aclresult = object_aclcheck(TypeRelationId, typeId2, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, typeId2);
	}

	/*
	 * 查找操作符的底层函数。
	 */
	if (!OidIsValid(typeId1))
	{
		typeId[0] = typeId2;
		nargs = 1;
	}
	else if (!OidIsValid(typeId2))
	{
		typeId[0] = typeId1;
		nargs = 1;
	}
	else
	{
		typeId[0] = typeId1;
		typeId[1] = typeId2;
		nargs = 2;
	}
	functionOid = LookupFuncName(functionName, nargs, typeId, false);

	/*
	 * 我们需要该函数的 EXECUTE 权限。严格来说这并非必需，
	 * 因为任何对操作符的使用尝试都会检查 EXECUTE，
	 * 但无论如何这似乎是个好主意。
	 */
	aclresult = object_aclcheck(ProcedureRelationId, functionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(functionName));

	rettype = get_func_rettype(functionOid);
	aclresult = object_aclcheck(TypeRelationId, rettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, rettype);

	/*
	 * 若指定了限制和连接选择率估计函数，就查找它们
	 */
	if (restrictionName)
		restrictionOid = ValidateRestrictionEstimator(restrictionName);
	else
		restrictionOid = InvalidOid;
	if (joinName)
		joinOid = ValidateJoinEstimator(joinName);
	else
		joinOid = InvalidOid;

	/*
	 * 现在让 OperatorCreate 完成所有工作……
	 */
	return
		OperatorCreate(oprName, /* 操作符名 */
					   oprNamespace,	/* 命名空间 */
					   typeId1, /* 左类型 id */
					   typeId2, /* 右类型 id */
					   functionOid, /* 操作符的函数 */
					   commutatorName,	/* 可选的交换操作符名 */
					   negatorName, /* 可选的否定操作符名 */
					   restrictionOid,	/* 可选的限制选择率函数 */
					   joinOid, /* 可选的连接选择率函数名 */
					   canMerge,	/* 操作符可合并 */
					   canHash);	/* 操作符可哈希 */
}

/*
 * 按名称查找限制选择率估计函数，并验证它
 * 具有正确的签名，且我们有权把它
 * 附加到操作符上。
 */
static Oid
ValidateRestrictionEstimator(List *restrictionName)
{
	Oid			typeId[4];
	Oid			restrictionOid;
	AclResult	aclresult;

	typeId[0] = INTERNALOID;	/* PlannerInfo */
	typeId[1] = OIDOID;			/* 操作符 OID */
	typeId[2] = INTERNALOID;	/* 参数列表 */
	typeId[3] = INT4OID;		/* varRelid */

	restrictionOid = LookupFuncName(restrictionName, 4, typeId, false);

	/* 估计函数必须返回 float8 */
	if (get_func_rettype(restrictionOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("restriction estimator function %s must return type %s",
						NameListToString(restrictionName), "float8")));

	/* 需要估计函数的 EXECUTE 权限 */
	aclresult = object_aclcheck(ProcedureRelationId, restrictionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(restrictionName));

	return restrictionOid;
}

/*
 * 按名称查找连接选择率估计函数，并验证它
 * 具有正确的签名，且我们有权把它
 * 附加到操作符上。
 */
static Oid
ValidateJoinEstimator(List *joinName)
{
	Oid			typeId[5];
	Oid			joinOid;
	Oid			joinOid2;
	AclResult	aclresult;

	typeId[0] = INTERNALOID;	/* PlannerInfo */
	typeId[1] = OIDOID;			/* 操作符 OID */
	typeId[2] = INTERNALOID;	/* 参数列表 */
	typeId[3] = INT2OID;		/* jointype */
	typeId[4] = INTERNALOID;	/* SpecialJoinInfo */

	/*
	 * 自 Postgres 8.4 起，连接估计函数的首选签名
	 * 有 5 个参数，但我们仍允许旧的 4 参数形式。
	 * 若两种形式都存在，就抱怨歧义。
	 */
	joinOid = LookupFuncName(joinName, 5, typeId, true);
	joinOid2 = LookupFuncName(joinName, 4, typeId, true);
	if (OidIsValid(joinOid))
	{
		if (OidIsValid(joinOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("join estimator function %s has multiple matches",
							NameListToString(joinName))));
	}
	else
	{
		joinOid = joinOid2;
		/* 若未找到，就在错误消息中引用 5 参数签名 */
		if (!OidIsValid(joinOid))
			joinOid = LookupFuncName(joinName, 5, typeId, false);
	}

	/* 估计函数必须返回 float8 */
	if (get_func_rettype(joinOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("join estimator function %s must return type %s",
						NameListToString(joinName), "float8")));

	/* 需要估计函数的 EXECUTE 权限 */
	aclresult = object_aclcheck(ProcedureRelationId, joinOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(joinName));

	return joinOid;
}

/*
 * 操作符删除的核心。
 */
void
RemoveOperatorById(Oid operOid)
{
	Relation	relation;
	HeapTuple	tup;
	Form_pg_operator op;

	relation = table_open(OperatorRelationId, RowExclusiveLock);

	tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
	if (!HeapTupleIsValid(tup)) /* 不应该发生 */
		elog(ERROR, "cache lookup failed for operator %u", operOid);
	op = (Form_pg_operator) GETSTRUCT(tup);

	/*
	 * 若有，重置来自交换和否定操作符的链接。
	 * 若是自交换或自否定，这意味着我们必须重新取回
	 * 更新后的元组。（我们可以优化掉对即将删除的
	 * 元组的更新，但似乎不值得为此把逻辑搞复杂。）
	 */
	if (OidIsValid(op->oprcom) || OidIsValid(op->oprnegate))
	{
		OperatorUpd(operOid, op->oprcom, op->oprnegate, true);
		if (operOid == op->oprcom || operOid == op->oprnegate)
		{
			ReleaseSysCache(tup);
			tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
			if (!HeapTupleIsValid(tup)) /* 不应该发生 */
				elog(ERROR, "cache lookup failed for operator %u", operOid);
		}
	}

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}

/*
 * AlterOperator
 *		实现 ALTER OPERATOR <operator> SET (option = ...) 的例程。
 *
 * 目前，只有 RESTRICT 和 JOIN 估计函数可以更改。
 */
ObjectAddress
AlterOperator(AlterOperatorStmt *stmt)
{
	ObjectAddress address;
	Oid			oprId;
	Relation	catalog;
	HeapTuple	tup;
	Form_pg_operator oprForm;
	int			i;
	ListCell   *pl;
	Datum		values[Natts_pg_operator];
	bool		nulls[Natts_pg_operator];
	bool		replaces[Natts_pg_operator];
	List	   *restrictionName = NIL;	/* 可选的限制选择率函数 */
	bool		updateRestriction = false;
	Oid			restrictionOid;
	List	   *joinName = NIL; /* 可选的连接选择率函数 */
	bool		updateJoin = false;
	Oid			joinOid;

	/* 查找操作符 */
	oprId = LookupOperWithArgs(stmt->opername, false);
	catalog = table_open(OperatorRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(oprId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator %u", oprId);
	oprForm = (Form_pg_operator) GETSTRUCT(tup);

	/* 处理选项 */
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		List	   *param;

		if (defel->arg == NULL)
			param = NIL;		/* NONE，移除该函数 */
		else
			param = defGetQualifiedName(defel);

		if (strcmp(defel->defname, "restrict") == 0)
		{
			restrictionName = param;
			updateRestriction = true;
		}
		else if (strcmp(defel->defname, "join") == 0)
		{
			joinName = param;
			updateJoin = true;
		}

		/*
		 * CREATE 接受的其他选项不可更改。
		 * 检查它们，以便给出有意义的错误消息。
		 */
		else if (strcmp(defel->defname, "leftarg") == 0 ||
				 strcmp(defel->defname, "rightarg") == 0 ||
				 strcmp(defel->defname, "function") == 0 ||
				 strcmp(defel->defname, "procedure") == 0 ||
				 strcmp(defel->defname, "commutator") == 0 ||
				 strcmp(defel->defname, "negator") == 0 ||
				 strcmp(defel->defname, "hashes") == 0 ||
				 strcmp(defel->defname, "merges") == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" cannot be changed",
							defel->defname)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* 检查权限。必须是所有者。 */
	if (!object_ownercheck(OperatorRelationId, oprId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
					   NameStr(oprForm->oprname));

	/*
	 * 若指定了限制和连接选择率估计函数，就查找它们
	 */
	if (restrictionName)
		restrictionOid = ValidateRestrictionEstimator(restrictionName);
	else
		restrictionOid = InvalidOid;
	if (joinName)
		joinOid = ValidateJoinEstimator(joinName);
	else
		joinOid = InvalidOid;

	/* 像 OperatorCreate 那样执行附加检查 */
	if (!(OidIsValid(oprForm->oprleft) && OidIsValid(oprForm->oprright)))
	{
		/* 若不是二元操作符，这些东西绝不能设置： */
		if (OidIsValid(joinOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have join selectivity")));
	}

	if (oprForm->oprresult != BOOLOID)
	{
		if (OidIsValid(restrictionOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have restriction selectivity")));
		if (OidIsValid(joinOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have join selectivity")));
	}

	/* 更新元组 */
	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) 0;
		replaces[i] = false;
		nulls[i] = false;
	}
	if (updateRestriction)
	{
		replaces[Anum_pg_operator_oprrest - 1] = true;
		values[Anum_pg_operator_oprrest - 1] = restrictionOid;
	}
	if (updateJoin)
	{
		replaces[Anum_pg_operator_oprjoin - 1] = true;
		values[Anum_pg_operator_oprjoin - 1] = joinOid;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(catalog),
							values, nulls, replaces);

	CatalogTupleUpdate(catalog, &tup->t_self, tup);

	address = makeOperatorDependencies(tup, false, true);

	InvokeObjectPostAlterHook(OperatorRelationId, oprId, 0);

	table_close(catalog, NoLock);

	return address;
}
