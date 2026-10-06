/*-------------------------------------------------------------------------
 *
 * pg_proc.c
 *	  支持操作 pg_proc 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_proc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	char	   *proname;
	char	   *prosrc;
} parse_error_callback_arg;

static void sql_function_parse_error_callback(void *arg);
static int	match_prosrc_to_query(const char *prosrc, const char *queryText,
								  int cursorpos);
static bool match_prosrc_to_literal(const char *prosrc, const char *literal,
									int cursorpos, int *newcursorpos);


/* ----------------------------------------------------------------
 *		ProcedureCreate
 *
 * 注意：allParameterTypes、parameterModes、parameterNames、trftypes 与 proconfig
 * 要么是适当类型的数组，要么为 NULL。我们将其声明为 Datum，
 * 而不是 "ArrayType *"，以免把 array.h 引入 pg_proc.h。
 * ----------------------------------------------------------------
 */
ObjectAddress
ProcedureCreate(const char *procedureName,
				Oid procNamespace,
				bool replace,
				bool returnsSet,
				Oid returnType,
				Oid proowner,
				Oid languageObjectId,
				Oid languageValidator,
				const char *prosrc,
				const char *probin,
				Node *prosqlbody,
				char prokind,
				bool security_definer,
				bool isLeakProof,
				bool isStrict,
				char volatility,
				char parallel,
				oidvector *parameterTypes,
				Datum allParameterTypes,
				Datum parameterModes,
				Datum parameterNames,
				List *parameterDefaults,
				Datum trftypes,
				Datum proconfig,
				Oid prosupport,
				float4 procost,
				float4 prorows)
{
	Oid			retval;
	int			parameterCount;
	int			allParamCount;
	Oid		   *allParams;
	char	   *paramModes = NULL;
	Oid			variadicType = InvalidOid;
	Acl		   *proacl = NULL;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	oldtup;
	bool		nulls[Natts_pg_proc];
	Datum		values[Natts_pg_proc];
	bool		replaces[Natts_pg_proc];
	NameData	procname;
	TupleDesc	tupDesc;
	bool		is_update;
	ObjectAddress myself,
				referenced;
	char	   *detailmsg;
	int			i;
	Oid			trfid;
	ObjectAddresses *addrs;

	/*
	 * 健全性检查
	 */
	Assert(PointerIsValid(prosrc));

	parameterCount = parameterTypes->dim1;
	if (parameterCount < 0 || parameterCount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("functions cannot have more than %d argument",
							   "functions cannot have more than %d arguments",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));
	/* 注意：上面是正确的，我们不统计输出参数 */

	/* 拆解数组输入 */
	if (allParameterTypes != PointerGetDatum(NULL))
	{
		/*
		 * 我们期望该数组为一维 OID 数组；验证这一点。我们不
		 * 需要使用 deconstruct_array()，因为数组数据看起来就
		 * 像 OID 值的 C 数组。
		 */
		ArrayType  *allParamArray = (ArrayType *) DatumGetPointer(allParameterTypes);

		allParamCount = ARR_DIMS(allParamArray)[0];
		if (ARR_NDIM(allParamArray) != 1 ||
			allParamCount <= 0 ||
			ARR_HASNULL(allParamArray) ||
			ARR_ELEMTYPE(allParamArray) != OIDOID)
			elog(ERROR, "allParameterTypes is not a 1-D Oid array");
		allParams = (Oid *) ARR_DATA_PTR(allParamArray);
		Assert(allParamCount >= parameterCount);
		/* 假定调用方已把内容弄对 */
	}
	else
	{
		allParamCount = parameterCount;
		allParams = parameterTypes->values;
	}

	if (parameterModes != PointerGetDatum(NULL))
	{
		/*
		 * 我们期望该数组为一维 CHAR 数组；验证这一点。我们不
		 * 需要使用 deconstruct_array()，因为数组数据看起来就
		 * 像 char 值的 C 数组。
		 */
		ArrayType  *modesArray = (ArrayType *) DatumGetPointer(parameterModes);

		if (ARR_NDIM(modesArray) != 1 ||
			ARR_DIMS(modesArray)[0] != allParamCount ||
			ARR_HASNULL(modesArray) ||
			ARR_ELEMTYPE(modesArray) != CHAROID)
			elog(ERROR, "parameterModes is not a 1-D char array");
		paramModes = (char *) ARR_DATA_PTR(modesArray);
	}

	/*
	 * 不允许多态返回类型，除非存在可用于推断实际返回类型的
	 * 多态输入参数。
	 */
	detailmsg = check_valid_polymorphic_signature(returnType,
												  parameterTypes->values,
												  parameterCount);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine result data type"),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * 同样，除非至少一个输入参数为 INTERNAL，否则不允许返回
	 * 类型为 INTERNAL。
	 */
	detailmsg = check_valid_internal_signature(returnType,
											   parameterTypes->values,
											   parameterCount);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of pseudo-type \"internal\""),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * 对任何 OUT 参数应用相同的检查。
	 */
	if (allParameterTypes != PointerGetDatum(NULL))
	{
		for (i = 0; i < allParamCount; i++)
		{
			if (paramModes == NULL ||
				paramModes[i] == PROARGMODE_IN ||
				paramModes[i] == PROARGMODE_VARIADIC)
				continue;		/* 忽略仅输入参数 */

			detailmsg = check_valid_polymorphic_signature(allParams[i],
														  parameterTypes->values,
														  parameterCount);
			if (detailmsg)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot determine result data type"),
						 errdetail_internal("%s", detailmsg)));
			detailmsg = check_valid_internal_signature(allParams[i],
													   parameterTypes->values,
													   parameterCount);
			if (detailmsg)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("unsafe use of pseudo-type \"internal\""),
						 errdetail_internal("%s", detailmsg)));
		}
	}

	/* 识别可变参数类型（若有） */
	if (paramModes != NULL)
	{
		/*
		 * 只有最后一个输入参数可以是可变参数；若是，保存其
		 * 元素类型。此处错误仅用 elog，因为调用方本应已
		 * 检查过。
		 */
		for (i = 0; i < allParamCount; i++)
		{
			switch (paramModes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_INOUT:
					if (OidIsValid(variadicType))
						elog(ERROR, "variadic parameter must be last");
					break;
				case PROARGMODE_OUT:
					if (OidIsValid(variadicType) && prokind == PROKIND_PROCEDURE)
						elog(ERROR, "variadic parameter must be last");
					break;
				case PROARGMODE_TABLE:
					/* 没问题 */
					break;
				case PROARGMODE_VARIADIC:
					if (OidIsValid(variadicType))
						elog(ERROR, "variadic parameter must be last");
					switch (allParams[i])
					{
						case ANYOID:
							variadicType = ANYOID;
							break;
						case ANYARRAYOID:
							variadicType = ANYELEMENTOID;
							break;
						case ANYCOMPATIBLEARRAYOID:
							variadicType = ANYCOMPATIBLEOID;
							break;
						default:
							variadicType = get_element_type(allParams[i]);
							if (!OidIsValid(variadicType))
								elog(ERROR, "variadic parameter is not an array");
							break;
					}
					break;
				default:
					elog(ERROR, "invalid parameter mode '%c'", paramModes[i]);
					break;
			}
		}
	}

	/*
	 * 一切看起来都没问题；准备要插入 pg_proc 的数据。
	 */

	for (i = 0; i < Natts_pg_proc; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) 0;
		replaces[i] = true;
	}

	namestrcpy(&procname, procedureName);
	values[Anum_pg_proc_proname - 1] = NameGetDatum(&procname);
	values[Anum_pg_proc_pronamespace - 1] = ObjectIdGetDatum(procNamespace);
	values[Anum_pg_proc_proowner - 1] = ObjectIdGetDatum(proowner);
	values[Anum_pg_proc_prolang - 1] = ObjectIdGetDatum(languageObjectId);
	values[Anum_pg_proc_procost - 1] = Float4GetDatum(procost);
	values[Anum_pg_proc_prorows - 1] = Float4GetDatum(prorows);
	values[Anum_pg_proc_provariadic - 1] = ObjectIdGetDatum(variadicType);
	values[Anum_pg_proc_prosupport - 1] = ObjectIdGetDatum(prosupport);
	values[Anum_pg_proc_prokind - 1] = CharGetDatum(prokind);
	values[Anum_pg_proc_prosecdef - 1] = BoolGetDatum(security_definer);
	values[Anum_pg_proc_proleakproof - 1] = BoolGetDatum(isLeakProof);
	values[Anum_pg_proc_proisstrict - 1] = BoolGetDatum(isStrict);
	values[Anum_pg_proc_proretset - 1] = BoolGetDatum(returnsSet);
	values[Anum_pg_proc_provolatile - 1] = CharGetDatum(volatility);
	values[Anum_pg_proc_proparallel - 1] = CharGetDatum(parallel);
	values[Anum_pg_proc_pronargs - 1] = UInt16GetDatum(parameterCount);
	values[Anum_pg_proc_pronargdefaults - 1] = UInt16GetDatum(list_length(parameterDefaults));
	values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(returnType);
	values[Anum_pg_proc_proargtypes - 1] = PointerGetDatum(parameterTypes);
	if (allParameterTypes != PointerGetDatum(NULL))
		values[Anum_pg_proc_proallargtypes - 1] = allParameterTypes;
	else
		nulls[Anum_pg_proc_proallargtypes - 1] = true;
	if (parameterModes != PointerGetDatum(NULL))
		values[Anum_pg_proc_proargmodes - 1] = parameterModes;
	else
		nulls[Anum_pg_proc_proargmodes - 1] = true;
	if (parameterNames != PointerGetDatum(NULL))
		values[Anum_pg_proc_proargnames - 1] = parameterNames;
	else
		nulls[Anum_pg_proc_proargnames - 1] = true;
	if (parameterDefaults != NIL)
		values[Anum_pg_proc_proargdefaults - 1] = CStringGetTextDatum(nodeToString(parameterDefaults));
	else
		nulls[Anum_pg_proc_proargdefaults - 1] = true;
	if (trftypes != PointerGetDatum(NULL))
		values[Anum_pg_proc_protrftypes - 1] = trftypes;
	else
		nulls[Anum_pg_proc_protrftypes - 1] = true;
	values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(prosrc);
	if (probin)
		values[Anum_pg_proc_probin - 1] = CStringGetTextDatum(probin);
	else
		nulls[Anum_pg_proc_probin - 1] = true;
	if (prosqlbody)
		values[Anum_pg_proc_prosqlbody - 1] = CStringGetTextDatum(nodeToString(prosqlbody));
	else
		nulls[Anum_pg_proc_prosqlbody - 1] = true;
	if (proconfig != PointerGetDatum(NULL))
		values[Anum_pg_proc_proconfig - 1] = proconfig;
	else
		nulls[Anum_pg_proc_proconfig - 1] = true;
	/* proacl 稍后再确定 */

	rel = table_open(ProcedureRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* 检查是否已有定义 */
	oldtup = SearchSysCache3(PROCNAMEARGSNSP,
							 PointerGetDatum(procedureName),
							 PointerGetDatum(parameterTypes),
							 ObjectIdGetDatum(procNamespace));

	if (HeapTupleIsValid(oldtup))
	{
		/* 已有一个；可以替换吗？ */
		Form_pg_proc oldproc = (Form_pg_proc) GETSTRUCT(oldtup);
		Datum		proargnames;
		bool		isnull;
		const char *dropcmd;

		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FUNCTION),
					 errmsg("function \"%s\" already exists with same argument types",
							procedureName)));
		if (!object_ownercheck(ProcedureRelationId, oldproc->oid, proowner))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
						   procedureName);

		/* 不允许更改例程种类 */
		if (oldproc->prokind != prokind)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change routine kind"),
					 (oldproc->prokind == PROKIND_AGGREGATE ?
					  errdetail("\"%s\" is an aggregate function.", procedureName) :
					  oldproc->prokind == PROKIND_FUNCTION ?
					  errdetail("\"%s\" is a function.", procedureName) :
					  oldproc->prokind == PROKIND_PROCEDURE ?
					  errdetail("\"%s\" is a procedure.", procedureName) :
					  oldproc->prokind == PROKIND_WINDOW ?
					  errdetail("\"%s\" is a window function.", procedureName) :
					  0)));

		dropcmd = (prokind == PROKIND_PROCEDURE ? "DROP PROCEDURE" :
				   prokind == PROKIND_AGGREGATE ? "DROP AGGREGATE" :
				   "DROP FUNCTION");

		/*
		 * 不允许更改既有过程的返回类型，因为已有的规则、视图等
		 * 可能依赖该返回类型。
		 *
		 * 对过程而言，返回类型变化意味着过程是否有输出参数发生了
		 * 变化。由于没有用户可见的返回类型，我们给出更具体的
		 * 错误消息。
		 */
		if (returnType != oldproc->prorettype ||
			returnsSet != oldproc->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 prokind == PROKIND_PROCEDURE
					 ? errmsg("cannot change whether a procedure has output parameters")
					 : errmsg("cannot change return type of existing function"),

			/*
			 * translator: first %s is DROP FUNCTION, DROP PROCEDURE, or DROP
			 * AGGREGATE
			 */
					 errhint("Use %s %s first.",
							 dropcmd,
							 format_procedure(oldproc->oid))));

		/*
		 * 若返回 RECORD，检查 OUT 参数是否隐含了记录类型的
		 * 可能变化
		 */
		if (returnType == RECORDOID)
		{
			TupleDesc	olddesc;
			TupleDesc	newdesc;

			olddesc = build_function_result_tupdesc_t(oldtup);
			newdesc = build_function_result_tupdesc_d(prokind,
													  allParameterTypes,
													  parameterModes,
													  parameterNames);
			if (olddesc == NULL && newdesc == NULL)
				 /* 没问题，两者都是运行时定义的 RECORD */ ;
			else if (olddesc == NULL || newdesc == NULL ||
					 !equalTupleDescs(olddesc, newdesc))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot change return type of existing function"),
						 errdetail("Row type defined by OUT parameters is different."),
				/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
						 errhint("Use %s %s first.",
								 dropcmd,
								 format_procedure(oldproc->oid))));
		}

		/*
		 * 若曾有命名输入参数，检查以确保
		 * 名称未被更改，因为这可能破坏既有调用。我们
		 * 仍允许给原先未命名的参数添加名称。
		 */
		proargnames = SysCacheGetAttr(PROCNAMEARGSNSP, oldtup,
									  Anum_pg_proc_proargnames,
									  &isnull);
		if (!isnull)
		{
			Datum		proargmodes;
			char	  **old_arg_names;
			char	  **new_arg_names;
			int			n_old_arg_names;
			int			n_new_arg_names;
			int			j;

			proargmodes = SysCacheGetAttr(PROCNAMEARGSNSP, oldtup,
										  Anum_pg_proc_proargmodes,
										  &isnull);
			if (isnull)
				proargmodes = PointerGetDatum(NULL);	/* 只是为了保险 */

			n_old_arg_names = get_func_input_arg_names(proargnames,
													   proargmodes,
													   &old_arg_names);
			n_new_arg_names = get_func_input_arg_names(parameterNames,
													   parameterModes,
													   &new_arg_names);
			for (j = 0; j < n_old_arg_names; j++)
			{
				if (old_arg_names[j] == NULL)
					continue;
				if (j >= n_new_arg_names || new_arg_names[j] == NULL ||
					strcmp(old_arg_names[j], new_arg_names[j]) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("cannot change name of input parameter \"%s\"",
									old_arg_names[j]),
					/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
							 errhint("Use %s %s first.",
									 dropcmd,
									 format_procedure(oldproc->oid))));
			}
		}

		/*
		 * 若已有默认值，检查兼容性：重定义
		 * 不得移除任何默认值，也不得更改其类型。（移除
		 * 默认值可能导致函数不再满足既有调用。
		 * 更改类型仅在相关参数为
		 * 多态时才可能，且在此类情况下更改默认值类型可能改变
		 * 既有调用所解析出的输出类型。）
		 */
		if (oldproc->pronargdefaults != 0)
		{
			Datum		proargdefaults;
			List	   *oldDefaults;
			ListCell   *oldlc;
			ListCell   *newlc;

			if (list_length(parameterDefaults) < oldproc->pronargdefaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("cannot remove parameter defaults from existing function"),
				/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
						 errhint("Use %s %s first.",
								 dropcmd,
								 format_procedure(oldproc->oid))));

			proargdefaults = SysCacheGetAttrNotNull(PROCNAMEARGSNSP, oldtup,
													Anum_pg_proc_proargdefaults);
			oldDefaults = castNode(List, stringToNode(TextDatumGetCString(proargdefaults)));
			Assert(list_length(oldDefaults) == oldproc->pronargdefaults);

			/* 新列表可以比旧列表有更多默认值，跳过它们 */
			newlc = list_nth_cell(parameterDefaults,
								  list_length(parameterDefaults) -
								  oldproc->pronargdefaults);

			foreach(oldlc, oldDefaults)
			{
				Node	   *oldDef = (Node *) lfirst(oldlc);
				Node	   *newDef = (Node *) lfirst(newlc);

				if (exprType(oldDef) != exprType(newDef))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("cannot change data type of existing parameter default value"),
					/* translator: first %s is DROP FUNCTION or DROP PROCEDURE */
							 errhint("Use %s %s first.",
									 dropcmd,
									 format_procedure(oldproc->oid))));
				newlc = lnext(parameterDefaults, newlc);
			}
		}

		/*
		 * 也不要更改既有 oid、所有权或权限。注意下面的
		 * 依赖更新代码必须与此决定一致。
		 */
		replaces[Anum_pg_proc_oid - 1] = false;
		replaces[Anum_pg_proc_proowner - 1] = false;
		replaces[Anum_pg_proc_proacl - 1] = false;

		/* 好，动手吧... */
		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* 创建新过程 */
		Oid			newOid;

		/* 首先，取得默认权限并设置 proacl */
		proacl = get_user_default_acl(OBJECT_FUNCTION, proowner,
									  procNamespace);
		if (proacl != NULL)
			values[Anum_pg_proc_proacl - 1] = PointerGetDatum(proacl);
		else
			nulls[Anum_pg_proc_proacl - 1] = true;

		newOid = GetNewOidWithIndex(rel, ProcedureOidIndexId,
									Anum_pg_proc_oid);
		values[Anum_pg_proc_oid - 1] = ObjectIdGetDatum(newOid);
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(rel, tup);
		is_update = false;
	}


	retval = ((Form_pg_proc) GETSTRUCT(tup))->oid;

	/*
	 * 为新函数创建依赖。若正在更新
	 * 既有函数，先删除任何既有 pg_depend 条目。
	 * （不过，由于我们不更改所有权或权限，
	 * 共享依赖*不必*更改，我们也不动它们。）
	 */
	if (is_update)
		deleteDependencyRecordsFor(ProcedureRelationId, retval, true);

	addrs = new_object_addresses();

	ObjectAddressSet(myself, ProcedureRelationId, retval);

	/* 对命名空间的依赖 */
	ObjectAddressSet(referenced, NamespaceRelationId, procNamespace);
	add_exact_object_address(&referenced, addrs);

	/* 对实现语言的依赖 */
	ObjectAddressSet(referenced, LanguageRelationId, languageObjectId);
	add_exact_object_address(&referenced, addrs);

	/* 对返回类型的依赖 */
	ObjectAddressSet(referenced, TypeRelationId, returnType);
	add_exact_object_address(&referenced, addrs);

	/* 对返回类型所用转换的依赖（若有） */
	if ((trfid = get_transform_oid(returnType, languageObjectId, true)))
	{
		ObjectAddressSet(referenced, TransformRelationId, trfid);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对参数类型的依赖 */
	for (i = 0; i < allParamCount; i++)
	{
		ObjectAddressSet(referenced, TypeRelationId, allParams[i]);
		add_exact_object_address(&referenced, addrs);

		/* 对参数类型所用转换的依赖（若有） */
		if ((trfid = get_transform_oid(allParams[i], languageObjectId, true)))
		{
			ObjectAddressSet(referenced, TransformRelationId, trfid);
			add_exact_object_address(&referenced, addrs);
		}
	}

	/* 对支持函数的依赖（若有） */
	if (OidIsValid(prosupport))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, prosupport);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* 对 SQL 例程体的依赖 */
	if (languageObjectId == SQLlanguageId && prosqlbody)
		recordDependencyOnExpr(&myself, prosqlbody, NIL, DEPENDENCY_NORMAL);

	/* 对参数默认表达式的依赖 */
	if (parameterDefaults)
		recordDependencyOnExpr(&myself, (Node *) parameterDefaults,
							   NIL, DEPENDENCY_NORMAL);

	/* 对所有者的依赖 */
	if (!is_update)
		recordDependencyOnOwner(ProcedureRelationId, retval, proowner);

	/* 对 ACL 中提及的任何角色的依赖 */
	if (!is_update)
		recordDependencyOnNewAcl(ProcedureRelationId, retval, 0,
								 proowner, proacl);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, is_update);

	heap_freetuple(tup);

	/* 新函数的创建后钩子 */
	InvokeObjectPostCreateHook(ProcedureRelationId, retval, 0);

	table_close(rel, RowExclusiveLock);

	/* 验证函数体 */
	if (OidIsValid(languageValidator))
	{
		ArrayType  *set_items = NULL;
		int			save_nestlevel = 0;

		/* 推进命令计数器，使验证器能看到新元组 */
		CommandCounterIncrement();

		/*
		 * 设置每函数配置参数，以便验证在
		 * 函数所期望的环境中进行。不过，若
		 * check_function_bodies 关闭，我们不做此事，因为那会
		 * 制造 pg_dump 不知如何处理的转储顺序
		 * 隐患。（例如，SET 子句可能引用尚未创建的
		 * 文本搜索配置。）这意味着验证器
		 * 在 check_function_bodies 关闭时，不应抱怨任何可能依赖 GUC
		 * 参数的事情。
		 */
		if (check_function_bodies)
		{
			set_items = (ArrayType *) DatumGetPointer(proconfig);
			if (set_items)		/* 需要新的 GUC 嵌套层级 */
			{
				save_nestlevel = NewGUCNestLevel();
				ProcessGUCArray(set_items,
								(superuser() ? PGC_SUSET : PGC_USERSET),
								PGC_S_SESSION,
								GUC_ACTION_SAVE);
			}
		}

		OidFunctionCall1(languageValidator, ObjectIdGetDatum(retval));

		if (set_items)
			AtEOXact_GUC(true, save_nestlevel);
	}

	/* 确保事务中止时统计信息会被丢弃 */
	if (!is_update)
		pgstat_create_function(retval);

	return myself;
}



/*
 * 内部函数的验证器
 *
 * 检查给定的内部函数名（"prosrc" 值）是否是
 * 已知的内建函数。
 */
Datum
fmgr_internal_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Datum		tmp;
	char	   *prosrc;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/*
	 * 我们不尊重 check_function_bodies，因为若函数名现在找不到，
	 * 以后也不太可能找到。
	 */

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);

	tmp = SysCacheGetAttrNotNull(PROCOID, tuple, Anum_pg_proc_prosrc);
	prosrc = TextDatumGetCString(tmp);

	if (fmgr_internal_function(prosrc) == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("there is no built-in function named \"%s\"",
						prosrc)));

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}



/*
 * C 语言函数的验证器
 *
 * 确保库文件存在、可加载，并包含
 * 指定的链接符号。也检查是否有有效的函数
 * 信息记录。
 */
Datum
fmgr_c_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	void	   *libraryhandle;
	HeapTuple	tuple;
	Datum		tmp;
	char	   *prosrc;
	char	   *probin;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/*
	 * 若 !check_function_bodies 则跳过检查会最一致，
	 * 但该开关的目的是方便 pg_dump 加载，
	 * 而对 pg_dump 加载来说*做*检查要好得多。
	 */

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);

	tmp = SysCacheGetAttrNotNull(PROCOID, tuple, Anum_pg_proc_prosrc);
	prosrc = TextDatumGetCString(tmp);

	tmp = SysCacheGetAttrNotNull(PROCOID, tuple, Anum_pg_proc_probin);
	probin = TextDatumGetCString(tmp);

	(void) load_external_function(probin, prosrc, true, &libraryhandle);
	(void) fetch_finfo_record(libraryhandle, prosrc);

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}


/*
 * SQL 语言函数的验证器
 *
 * 在此解析以确保其中没有语法错误。
 */
Datum
fmgr_sql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *lc;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;
	parse_error_callback_arg callback_arg;
	ErrorContextCallback sqlerrcontext;
	bool		haspolyarg;
	int			i;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	/* 禁止伪类型结果 */
	/* 但 RECORD、VOID 或多态类型除外 */
	if (get_typtype(proc->prorettype) == TYPTYPE_PSEUDO &&
		proc->prorettype != RECORDOID &&
		proc->prorettype != VOIDOID &&
		!IsPolymorphicType(proc->prorettype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("SQL functions cannot return type %s",
						format_type_be(proc->prorettype))));

	/* 禁止参数中的伪类型 */
	/* 但多态类型除外 */
	haspolyarg = false;
	for (i = 0; i < proc->pronargs; i++)
	{
		if (get_typtype(proc->proargtypes.values[i]) == TYPTYPE_PSEUDO)
		{
			if (IsPolymorphicType(proc->proargtypes.values[i]))
				haspolyarg = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL functions cannot have arguments of type %s",
								format_type_be(proc->proargtypes.values[i]))));
		}
	}

	/* 若 !check_function_bodies 则推迟函数体检查 */
	if (check_function_bodies)
	{
		tmp = SysCacheGetAttrNotNull(PROCOID, tuple, Anum_pg_proc_prosrc);
		prosrc = TextDatumGetCString(tmp);

		/*
		 * 为 ereport() 设置错误回溯支持。
		 */
		callback_arg.proname = NameStr(proc->proname);
		callback_arg.prosrc = prosrc;

		sqlerrcontext.callback = sql_function_parse_error_callback;
		sqlerrcontext.arg = (void *) &callback_arg;
		sqlerrcontext.previous = error_context_stack;
		error_context_stack = &sqlerrcontext;

		/* 若有 prosqlbody，关注它而非 prosrc */
		tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosqlbody, &isnull);
		if (!isnull)
		{
			Node	   *n;
			List	   *stored_query_list;

			n = stringToNode(TextDatumGetCString(tmp));
			if (IsA(n, List))
				stored_query_list = linitial(castNode(List, n));
			else
				stored_query_list = list_make1(n);

			querytree_list = NIL;
			foreach(lc, stored_query_list)
			{
				Query	   *parsetree = lfirst_node(Query, lc);
				List	   *querytree_sublist;

				/*
				 * 通常，在解析 CREATE FUNCTION 命令的
				 * 函数体时我们就已取得锁。不过，
				 * 验证器函数无法假定它只在
				 * 那种上下文中被调用。
				 */
				AcquireRewriteLocks(parsetree, true, false);
				querytree_sublist = pg_rewrite_query(parsetree);
				querytree_list = lappend(querytree_list, querytree_sublist);
			}
		}
		else
		{
			/*
			 * 若有任何多态输入类型，我们就无法对
			 * 函数定义做完整预检查，因为表达式结果的实际
			 * 数据类型将无法解析。检查将
			 * 改在运行时进行。
			 *
			 * 不过我们可以把文本送过原始解析器；这至少能
			 * 捕获愚蠢的句法错误。
			 */
			raw_parsetree_list = pg_parse_query(prosrc);
			querytree_list = NIL;

			if (!haspolyarg)
			{
				/*
				 * 可以做完整预检查：分析并重写查询，
				 * 然后验证结果类型。
				 */
				SQLFunctionParseInfoPtr pinfo;

				/* 但首先，设置参数信息 */
				pinfo = prepare_sql_fn_parse_info(tuple, NULL, InvalidOid);

				foreach(lc, raw_parsetree_list)
				{
					RawStmt    *parsetree = lfirst_node(RawStmt, lc);
					List	   *querytree_sublist;

					querytree_sublist = pg_analyze_and_rewrite_withcb(parsetree,
																	  prosrc,
																	  (ParserSetupHook) sql_fn_parser_setup,
																	  pinfo,
																	  NULL);
					querytree_list = lappend(querytree_list,
											 querytree_sublist);
				}
			}
		}

		if (!haspolyarg)
		{
			Oid			rettype;
			TupleDesc	rettupdesc;

			check_sql_fn_statements(querytree_list);

			(void) get_func_result_type(funcoid, &rettype, &rettupdesc);

			(void) check_sql_fn_retval_ext(querytree_list,
										   rettype, rettupdesc,
										   proc->prokind,
										   false, NULL);
		}

		error_context_stack = sqlerrcontext.previous;
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}

/*
 * 处理 SQL 函数定义中错误的错误上下文回调
 */
static void
sql_function_parse_error_callback(void *arg)
{
	parse_error_callback_arg *callback_arg = (parse_error_callback_arg *) arg;

	/* 看是否为语法错误；若是，转置到 CREATE FUNCTION */
	if (!function_parse_error_transpose(callback_arg->prosrc))
	{
		/* 若不是语法错误，把信息压入上下文栈 */
		errcontext("SQL function \"%s\"", callback_arg->proname);
	}
}

/*
 * 调整发生在 CREATE FUNCTION 或 DO 命令
 * 函数体内的语法错误。这可供任何函数验证器或
 * 匿名块处理程序使用，不仅限于 SQL 语言函数。
 * 假定语法错误位置最初相对于
 * （传入的）函数体字符串。若可能，我们把位置调整为
 * 引用原始命令文本；若无法做到，则改为设置
 * "内部查询"语法错误。
 *
 * 若处理了语法错误则返回 true，否则返回 false。
 */
bool
function_parse_error_transpose(const char *prosrc)
{
	int			origerrposition;
	int			newerrposition;

	/*
	 * 除非我们处理的是带有光标位置的
	 * 语法错误，否则无事可做。
	 *
	 * 某些 PL 可能一开始就把错误位置报告为内部
	 * 错误，因此也检查这一点。
	 */
	origerrposition = geterrposition();
	if (origerrposition <= 0)
	{
		origerrposition = getinternalerrposition();
		if (origerrposition <= 0)
			return false;
	}

	/* 我们可以从活动门户取得原始查询文本（有点取巧...） */
	if (ActivePortal && ActivePortal->status == PORTAL_ACTIVE)
	{
		const char *queryText = ActivePortal->sourceText;

		/* 尝试在原始文本中定位 prosrc */
		newerrposition = match_prosrc_to_query(prosrc, queryText,
											   origerrposition);
	}
	else
	{
		/*
		 * 若没有 ActivePortal 则静默放弃。这是少见情况，
		 * 但例如在逻辑复制工作进程中可能发生。
		 */
		newerrposition = -1;
	}

	if (newerrposition > 0)
	{
		/* 成功，因此修正错误位置以引用原始查询 */
		errposition(newerrposition);
		/* 去掉任何将该错误报告为"内部查询"的信息 */
		internalerrposition(0);
		internalerrquery(NULL);
	}
	else
	{
		/*
		 * 若不成功，把位置转换为内部位置标记，
		 * 并把函数文本作为内部查询给出。
		 */
		errposition(0);
		internalerrposition(origerrposition);
		internalerrquery(prosrc);
	}

	return true;
}

/*
 * 尝试在给定的 CREATE FUNCTION 或 DO 命令文本中
 * 定位包含函数体的字符串字面量。若成功，返回
 * 命令内与字面量内给定字符索引相对应的字符
 * （而非字节）索引。若不成功，返回 0。
 */
static int
match_prosrc_to_query(const char *prosrc, const char *queryText,
					  int cursorpos)
{
	/*
	 * 我们不完全解析原始命令，而只是扫描
	 * 命令寻找 $prosrc$ 或 'prosrc'。这可能被愚弄（尽管
	 * 不是很可能的场景），因此若找到多于一个
	 * 匹配则失败。
	 */
	int			prosrclen = strlen(prosrc);
	int			querylen = strlen(queryText);
	int			matchpos = 0;
	int			curpos;
	int			newcursorpos;

	for (curpos = 0; curpos < querylen - prosrclen; curpos++)
	{
		if (queryText[curpos] == '$' &&
			strncmp(prosrc, &queryText[curpos + 1], prosrclen) == 0 &&
			queryText[curpos + 1 + prosrclen] == '$')
		{
			/*
			 * 找到 $foo$ 匹配。由于美元引用字面量中没有嵌入的引号
			 * 字符，我们不必做任何花哨的算术；只需按起始位置
			 * 偏移即可。
			 */
			if (matchpos)
				return 0;		/* 多个匹配，失败 */
			matchpos = pg_mbstrlen_with_len(queryText, curpos + 1)
				+ cursorpos;
		}
		else if (queryText[curpos] == '\'' &&
				 match_prosrc_to_literal(prosrc, &queryText[curpos + 1],
										 cursorpos, &newcursorpos))
		{
			/*
			 * 找到 'foo' 匹配。match_prosrc_to_literal() 已针对字面量中
			 * 嵌入的引号或反斜杠做过调整。
			 */
			if (matchpos)
				return 0;		/* 多个匹配，失败 */
			matchpos = pg_mbstrlen_with_len(queryText, curpos + 1)
				+ newcursorpos;
		}
	}

	return matchpos;
}

/*
 * 尝试将给定源文本与单引号字面量匹配。
 * 若成功，调整 newcursorpos 以对应源文本中
 * 与 cursorpos 对应的字符（而非字节）索引。
 *
 * 进入时，literal 指向刚过一个 ' 字符的位置。我们必须检查
 * 尾部引号。
 */
static bool
match_prosrc_to_literal(const char *prosrc, const char *literal,
						int cursorpos, int *newcursorpos)
{
	int			newcp = cursorpos;
	int			chlen;

	/*
	 * 本实现处理字符串字面量中的反斜杠与加倍
	 * 引号。它不处理跨行延续字面量的
	 * SQL 语法。
	 *
	 * 我们按字符而非按字节比较，以便能做
	 * 正确的 cursorpos 计算。
	 */
	while (*prosrc)
	{
		cursorpos--;			/* 光标前剩余的字符 */

		/*
		 * 检查字面量中的反斜杠与加倍引号；若在光标前
		 * 找到则调整 newcp。
		 */
		if (*literal == '\\')
		{
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		else if (*literal == '\'')
		{
			if (literal[1] != '\'')
				goto fail;
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		chlen = pg_mblen(prosrc);
		if (strncmp(prosrc, literal, chlen) != 0)
			goto fail;
		prosrc += chlen;
		literal += chlen;
	}

	if (*literal == '\'' && literal[1] != '\'')
	{
		/* 成功 */
		*newcursorpos = newcp;
		return true;
	}

fail:
	/* 必须设置 *newcursorpos 以抑制编译器警告 */
	*newcursorpos = newcp;
	return false;
}

List *
oid_array_to_list(Datum datum)
{
	ArrayType  *array = DatumGetArrayTypeP(datum);
	Datum	   *values;
	int			nelems;
	int			i;
	List	   *result = NIL;

	deconstruct_array_builtin(array, OIDOID, &values, NULL, &nelems);
	for (i = 0; i < nelems; i++)
		result = lappend_oid(result, values[i]);
	return result;
}
