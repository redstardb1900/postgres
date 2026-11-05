/*-------------------------------------------------------------------------
 *
 * functioncmds.c
 *
 *	  Routines for CREATE and DROP FUNCTION commands and CREATE and DROP
 *	  CAST commands.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/functioncmds.c
 *
 * DESCRIPTION
 *	  These routines take the parse tree and pick out the
 *	  appropriate arguments/flags, and pass the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * 检查 CREATE FUNCTION 语句的 RETURNS 子句，
 * 并将信息作为 *prorettype_p 和 *returnsSet 返回。
 *
 * 这比普通的类型名查找更复杂，因为我们希望允许使用壳
 * 类型，甚至在指定的返回类型尚不存在时创建它。
 * （没有这个机制，就无法为新类型定义 I/O 过程。）
 * 但如果目标语言是 SQL 则报错，因为 SQL 函数创建
 * 无法处理这种情况。（我们在这里处理，而不是在
 * SQL 函数校验器中，以避免对同一情况先产生 NOTICE
 * 后又产生 ERROR。）
 */
static void
compute_return_type(TypeName *returnType, Oid languageOid,
					Oid *prorettype_p, bool *returnsSet_p)
{
	Oid			rettype;
	Type		typtup;
	AclResult	aclresult;

	typtup = LookupTypeName(NULL, returnType, NULL, false);

	if (typtup)
	{
		if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
		{
			if (languageOid == SQLlanguageId)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function cannot return shell type %s",
								TypeNameToString(returnType))));
			else
				ereport(NOTICE,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("return type %s is only a shell",
								TypeNameToString(returnType))));
		}
		rettype = typeTypeId(typtup);
		ReleaseSysCache(typtup);
	}
	else
	{
		char	   *typnam = TypeNameToString(returnType);
		Oid			namespaceId;
		char	   *typname;
		ObjectAddress address;

		/*
		 * 只有 C 编码的函数才能作为 I/O 函数。我们在这里强制
		 * 此限制，主要是为了防止因用户定义函数中的
		 * 简单输入错误而导致系统目录被壳类型污染。
		 *
		 */
		if (languageOid != INTERNALlanguageId &&
			languageOid != ClanguageId)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist", typnam)));

		/* 同样拒绝类型修饰符装饰 */
		if (returnType->typmods != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type modifier cannot be specified for shell type \"%s\"",
							typnam)));

		/* 否则，继续创建一个壳类型 */
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is not yet defined", typnam),
				 errdetail("Creating a shell type definition.")));
		namespaceId = QualifiedNameGetCreationNamespace(returnType->names,
														&typname);
		aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
		address = TypeShellMake(typname, namespaceId, GetUserId());
		rettype = address.objectId;
		Assert(OidIsValid(rettype));
	}

	aclresult = object_aclcheck(TypeRelationId, rettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, rettype);

	*prorettype_p = rettype;
	*returnsSet_p = returnType->setof;
}

/*
 * 解释 CREATE FUNCTION、CREATE PROCEDURE 或 CREATE AGGREGATE
 * 语句的函数参数列表。
 *
 * 输入参数：
 * parameters：FunctionParameter 结构体的列表
 * languageOid：函数语言的 OID（若是 CREATE AGGREGATE 则为 InvalidOid）
 * objtype：标识正在创建的对象的类型
 *
 * 结果存储在输出参数中。parameterTypes 必须始终被创建，
 * 但如果不需要，其他数组/列表可以为 NULL 指针。
 * 如果存在 VARIADIC 参数，variadicArgType 会设置为可变数组
 * 类型（只能有一个）；否则设置为 InvalidOid。
 * 如果没有 OUT 参数，requiredResultType 设置为 InvalidOid，
 * 否则设置为隐含结果类型的 OID。
 */
void
interpret_function_parameter_list(ParseState *pstate,
								  List *parameters,
								  Oid languageOid,
								  ObjectType objtype,
								  oidvector **parameterTypes,
								  List **parameterTypes_list,
								  ArrayType **allParameterTypes,
								  ArrayType **parameterModes,
								  ArrayType **parameterNames,
								  List **inParameterNames_list,
								  List **parameterDefaults,
								  Oid *variadicArgType,
								  Oid *requiredResultType)
{
	int			parameterCount = list_length(parameters);
	Oid		   *inTypes;
	int			inCount = 0;
	Datum	   *allTypes;
	Datum	   *paramModes;
	Datum	   *paramNames;
	int			outCount = 0;
	int			varCount = 0;
	bool		have_names = false;
	bool		have_defaults = false;
	ListCell   *x;
	int			i;

	*variadicArgType = InvalidOid;	/* 默认结果 */
	*requiredResultType = InvalidOid;	/* 默认结果 */

	inTypes = (Oid *) palloc(parameterCount * sizeof(Oid));
	allTypes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramModes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramNames = (Datum *) palloc0(parameterCount * sizeof(Datum));
	*parameterDefaults = NIL;

	/* 扫描列表并将数据提取到工作数组中 */
	i = 0;
	foreach(x, parameters)
	{
		FunctionParameter *fp = (FunctionParameter *) lfirst(x);
		TypeName   *t = fp->argType;
		FunctionParameterMode fpmode = fp->mode;
		bool		isinput = false;
		Oid			toid;
		Type		typtup;
		AclResult	aclresult;

		/* 在此处的处理中，默认模式规范等同于 IN */
		if (fpmode == FUNC_PARAM_DEFAULT)
			fpmode = FUNC_PARAM_IN;

		typtup = LookupTypeName(NULL, t, NULL, false);
		if (typtup)
		{
			if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
			{
				/* 同上，如果语言是 SQL 则报硬错误 */
				if (languageOid == SQLlanguageId)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("SQL function cannot accept shell type %s",
									TypeNameToString(t))));
				/* 我们也不允许在壳类型上创建聚合 */
				else if (objtype == OBJECT_AGGREGATE)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("aggregate cannot accept shell type %s",
									TypeNameToString(t))));
				else
					ereport(NOTICE,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("argument type %s is only a shell",
									TypeNameToString(t))));
			}
			toid = typeTypeId(typtup);
			ReleaseSysCache(typtup);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type %s does not exist",
							TypeNameToString(t))));
			toid = InvalidOid;	/* 让编译器安静 */
		}

		aclresult = object_aclcheck(TypeRelationId, toid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, toid);

		if (t->setof)
		{
			if (objtype == OBJECT_AGGREGATE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregates cannot accept set arguments")));
			else if (objtype == OBJECT_PROCEDURE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedures cannot accept set arguments")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("functions cannot accept set arguments")));
		}

		/* 处理输入参数 */
		if (fpmode != FUNC_PARAM_OUT && fpmode != FUNC_PARAM_TABLE)
		{
			/* 其他输入参数不能位于 VARIADIC 参数之后 */
			if (varCount > 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("VARIADIC parameter must be the last input parameter")));
			inTypes[inCount++] = toid;
			isinput = true;
			if (parameterTypes_list)
				*parameterTypes_list = lappend_oid(*parameterTypes_list, toid);
		}

		/* 处理输出参数 */
		if (fpmode != FUNC_PARAM_IN && fpmode != FUNC_PARAM_VARIADIC)
		{
			if (objtype == OBJECT_PROCEDURE)
			{
				/*
				 * 仅对过程禁止 VARIADIC 后跟 OUT 参数。虽然这种情况
				 * 在普通函数调用中不会造成混淆，但在 CALL 语句中
				 * 会造成混淆。
				 */
				if (varCount > 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("VARIADIC parameter must be the last parameter")));
				/* 带有输出参数的过程始终返回 RECORD */
				*requiredResultType = RECORDOID;
			}
			else if (outCount == 0) /* 保存第一个输出参数的类型 */
				*requiredResultType = toid;
			outCount++;
		}

		if (fpmode == FUNC_PARAM_VARIADIC)
		{
			*variadicArgType = toid;
			varCount++;
			/* 验证可变参数类型 */
			switch (toid)
			{
				case ANYARRAYOID:
				case ANYCOMPATIBLEARRAYOID:
				case ANYOID:
					/* 允许 */
					break;
				default:
					if (!OidIsValid(get_element_type(toid)))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC parameter must be an array")));
					break;
			}
		}

		allTypes[i] = ObjectIdGetDatum(toid);

		paramModes[i] = CharGetDatum(fpmode);

		if (fp->name && fp->name[0])
		{
			ListCell   *px;

			/*
			 * 从 Postgres 9.0 开始，我们禁止对两个输入或两个输出
			 * 函数参数使用相同的名称。依赖函数的语言，冲突的
			 * 输入和输出名称也可能不好，但我们将其留给 PL 来
			 * 抱怨。
			 */
			foreach(px, parameters)
			{
				FunctionParameter *prevfp = (FunctionParameter *) lfirst(px);
				FunctionParameterMode prevfpmode;

				if (prevfp == fp)
					break;
				/* 同上，默认模式为 IN */
				prevfpmode = prevfp->mode;
				if (prevfpmode == FUNC_PARAM_DEFAULT)
					prevfpmode = FUNC_PARAM_IN;
				/* 纯 IN 不与纯 OUT 冲突 */
				if ((fpmode == FUNC_PARAM_IN ||
					 fpmode == FUNC_PARAM_VARIADIC) &&
					(prevfpmode == FUNC_PARAM_OUT ||
					 prevfpmode == FUNC_PARAM_TABLE))
					continue;
				if ((prevfpmode == FUNC_PARAM_IN ||
					 prevfpmode == FUNC_PARAM_VARIADIC) &&
					(fpmode == FUNC_PARAM_OUT ||
					 fpmode == FUNC_PARAM_TABLE))
					continue;
				if (prevfp->name && prevfp->name[0] &&
					strcmp(prevfp->name, fp->name) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("parameter name \"%s\" used more than once",
									fp->name)));
			}

			paramNames[i] = CStringGetTextDatum(fp->name);
			have_names = true;
		}

		if (inParameterNames_list)
			*inParameterNames_list = lappend(*inParameterNames_list, makeString(fp->name ? fp->name : pstrdup("")));

		if (fp->defexpr)
		{
			Node	   *def;

			if (!isinput)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("only input parameters can have default values")));

			def = transformExpr(pstate, fp->defexpr,
								EXPR_KIND_FUNCTION_DEFAULT);
			def = coerce_to_specific_type(pstate, def, toid, "DEFAULT");
			assign_expr_collations(pstate, def);

			/*
			 * 确保没有引用变量（鉴于 add_missing_from 已成为
			 * 历史，这可能已是死代码）。
			 */
			if (pstate->p_rtable != NIL ||
				contain_var_clause(def))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("cannot use table references in parameter default value")));

			/*
			 * transformExpr() 应该已根据默认表达式
			 * 的 EXPR_KIND_ 拒绝了子查询、
			 * 聚合和窗口函数。
			 *
			 * 它也不能返回集合 --- 但 coerce_to_specific_type
			 * 已经帮我们检查了这一点。
			 *
			 * 注意：这些限制的目的是确保表面上
			 * 没有子计划、聚合等的表达式，
			 * 在插入函数默认参数后
			 * 不会突然拥有它们。
			 */

			*parameterDefaults = lappend(*parameterDefaults, def);
			have_defaults = true;
		}
		else
		{
			if (isinput && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("input parameters after one with a default value must also have defaults")));

			/*
			 * 对于过程，我们也不允许在带有默认值的参数之后
			 * 出现 OUT 参数，因为 CALL 语句中会出现相同
			 * 类型的混淆。
			 */
			if (objtype == OBJECT_PROCEDURE && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedure OUT parameters cannot appear after one with a default value")));
		}

		i++;
	}

	/* 现在根据需要构造适当的输出 */
	*parameterTypes = buildoidvector(inTypes, inCount);

	if (outCount > 0 || varCount > 0)
	{
		*allParameterTypes = construct_array_builtin(allTypes, parameterCount, OIDOID);
		*parameterModes = construct_array_builtin(paramModes, parameterCount, CHAROID);
		if (outCount > 1)
			*requiredResultType = RECORDOID;
		/* 否则我们在上面已正确设置了 requiredResultType */
	}
	else
	{
		*allParameterTypes = NULL;
		*parameterModes = NULL;
	}

	if (have_names)
	{
		for (i = 0; i < parameterCount; i++)
		{
			if (paramNames[i] == PointerGetDatum(NULL))
				paramNames[i] = CStringGetTextDatum("");
		}
		*parameterNames = construct_array_builtin(paramNames, parameterCount, TEXTOID);
	}
	else
		*parameterNames = NULL;
}


/*
 * 识别可同时传递给 CREATE FUNCTION 和 ALTER FUNCTION
 * 的选项之一，并通过输出参数
 * 返回。如果传入的选项被识别，则返回 true。
 * 如果要赋值的输出参数指向非 NULL，
 * 则引发重复子句错误。（但我们不会尝试
 * 检测重复的 SET 参数 --- 如果重复，最后一个生效。）
 */
static bool
compute_common_attribute(ParseState *pstate,
						 bool is_procedure,
						 DefElem *defel,
						 DefElem **volatility_item,
						 DefElem **strict_item,
						 DefElem **security_item,
						 DefElem **leakproof_item,
						 List **set_items,
						 DefElem **cost_item,
						 DefElem **rows_item,
						 DefElem **support_item,
						 DefElem **parallel_item)
{
	if (strcmp(defel->defname, "volatility") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*volatility_item)
			errorConflictingDefElem(defel, pstate);

		*volatility_item = defel;
	}
	else if (strcmp(defel->defname, "strict") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*strict_item)
			errorConflictingDefElem(defel, pstate);

		*strict_item = defel;
	}
	else if (strcmp(defel->defname, "security") == 0)
	{
		if (*security_item)
			errorConflictingDefElem(defel, pstate);

		*security_item = defel;
	}
	else if (strcmp(defel->defname, "leakproof") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*leakproof_item)
			errorConflictingDefElem(defel, pstate);

		*leakproof_item = defel;
	}
	else if (strcmp(defel->defname, "set") == 0)
	{
		*set_items = lappend(*set_items, defel->arg);
	}
	else if (strcmp(defel->defname, "cost") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*cost_item)
			errorConflictingDefElem(defel, pstate);

		*cost_item = defel;
	}
	else if (strcmp(defel->defname, "rows") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*rows_item)
			errorConflictingDefElem(defel, pstate);

		*rows_item = defel;
	}
	else if (strcmp(defel->defname, "support") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*support_item)
			errorConflictingDefElem(defel, pstate);

		*support_item = defel;
	}
	else if (strcmp(defel->defname, "parallel") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*parallel_item)
			errorConflictingDefElem(defel, pstate);

		*parallel_item = defel;
	}
	else
		return false;

	/* 识别到一个选项 */
	return true;

procedure_error:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("invalid attribute in procedure definition"),
			 parser_errposition(pstate, defel->location)));
	return false;
}

static char
interpret_func_volatility(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "immutable") == 0)
		return PROVOLATILE_IMMUTABLE;
	else if (strcmp(str, "stable") == 0)
		return PROVOLATILE_STABLE;
	else if (strcmp(str, "volatile") == 0)
		return PROVOLATILE_VOLATILE;
	else
	{
		elog(ERROR, "invalid volatility \"%s\"", str);
		return 0;				/* 让编译器安静 */
	}
}

static char
interpret_func_parallel(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "safe") == 0)
		return PROPARALLEL_SAFE;
	else if (strcmp(str, "unsafe") == 0)
		return PROPARALLEL_UNSAFE;
	else if (strcmp(str, "restricted") == 0)
		return PROPARALLEL_RESTRICTED;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"parallel\" must be SAFE, RESTRICTED, or UNSAFE")));
		return PROPARALLEL_UNSAFE;	/* 让编译器安静 */
	}
}

/*
 * 根据 VariableSetStmt 项列表更新 proconfig 值。
 *
 * 输入和结果可以为 NULL，表示空条目。
 */
static ArrayType *
update_proconfig_value(ArrayType *a, List *set_items)
{
	ListCell   *l;

	foreach(l, set_items)
	{
		VariableSetStmt *sstmt = lfirst_node(VariableSetStmt, l);

		if (sstmt->kind == VAR_RESET_ALL)
			a = NULL;
		else
		{
			char	   *valuestr = ExtractSetVariableArgs(sstmt);

			if (valuestr)
				a = GUCArrayAdd(a, sstmt->name, valuestr);
			else				/* RESET */
				a = GUCArrayDelete(a, sstmt->name);
		}
	}

	return a;
}

static Oid
interpret_func_support(DefElem *defel)
{
	List	   *procName = defGetQualifiedName(defel);
	Oid			procOid;
	Oid			argList[1];

	/*
	 * 支持函数始终接受一个 INTERNAL 参数并返回
	 * INTERNAL。
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procName, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procName, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("support function %s must return type %s",
						NameListToString(procName), "internal")));

	/*
	 * 也许有一天我们会想要在这里进行 ACL 检查；但目前，
	 * 我们坚持必须是超级用户才能指定支持函数，因此
	 * 支持函数的权限问题不重要。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to specify a support function")));

	return procOid;
}


/*
 * 将 gram.y 中组装的选项列表分解为函数
 * 属性。
 */
static void
compute_function_attributes(ParseState *pstate,
							bool is_procedure,
							List *options,
							List **as,
							char **language,
							Node **transform,
							bool *windowfunc_p,
							char *volatility_p,
							bool *strict_p,
							bool *security_definer,
							bool *leakproof_p,
							ArrayType **proconfig,
							float4 *procost,
							float4 *prorows,
							Oid *prosupport,
							char *parallel_p)
{
	ListCell   *option;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	DefElem    *transform_item = NULL;
	DefElem    *windowfunc_item = NULL;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_item = NULL;
	DefElem    *leakproof_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;
	DefElem    *support_item = NULL;
	DefElem    *parallel_item = NULL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				errorConflictingDefElem(defel, pstate);
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				errorConflictingDefElem(defel, pstate);
			language_item = defel;
		}
		else if (strcmp(defel->defname, "transform") == 0)
		{
			if (transform_item)
				errorConflictingDefElem(defel, pstate);
			transform_item = defel;
		}
		else if (strcmp(defel->defname, "window") == 0)
		{
			if (windowfunc_item)
				errorConflictingDefElem(defel, pstate);
			if (is_procedure)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("invalid attribute in procedure definition"),
						 parser_errposition(pstate, defel->location)));
			windowfunc_item = defel;
		}
		else if (compute_common_attribute(pstate,
										  is_procedure,
										  defel,
										  &volatility_item,
										  &strict_item,
										  &security_item,
										  &leakproof_item,
										  &set_items,
										  &cost_item,
										  &rows_item,
										  &support_item,
										  &parallel_item))
		{
			/* 已识别通用选项 */
			continue;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (as_item)
		*as = (List *) as_item->arg;
	if (language_item)
		*language = strVal(language_item->arg);
	if (transform_item)
		*transform = transform_item->arg;
	if (windowfunc_item)
		*windowfunc_p = boolVal(windowfunc_item->arg);
	if (volatility_item)
		*volatility_p = interpret_func_volatility(volatility_item);
	if (strict_item)
		*strict_p = boolVal(strict_item->arg);
	if (security_item)
		*security_definer = boolVal(security_item->arg);
	if (leakproof_item)
		*leakproof_p = boolVal(leakproof_item->arg);
	if (set_items)
		*proconfig = update_proconfig_value(NULL, set_items);
	if (cost_item)
	{
		*procost = defGetNumeric(cost_item);
		if (*procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		*prorows = defGetNumeric(rows_item);
		if (*prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
	}
	if (support_item)
		*prosupport = interpret_func_support(support_item);
	if (parallel_item)
		*parallel_p = interpret_func_parallel(parallel_item);
}


/*
 * 对于动态链接的 C 语言对象，子句的形式为
 *
 *	   AS <目标文件名> [, <链接符号名> ]
 *
 * 在所有其他情况下：
 *
 *	   AS <对象引用或 SQL 代码>
 */
static void
interpret_AS_clause(Oid languageOid, const char *languageName,
					char *funcname, List *as, Node *sql_body_in,
					List *parameterTypes, List *inParameterNames,
					char **prosrc_str_p, char **probin_str_p,
					Node **sql_body_out,
					const char *queryString)
{
	if (!sql_body_in && !as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("no function body specified")));

	if (sql_body_in && as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("duplicate function body specified")));

	if (sql_body_in && languageOid != SQLlanguageId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("inline SQL function body only valid for language SQL")));

	*sql_body_out = NULL;

	if (languageOid == ClanguageId)
	{
		/*
		 * 对于 "C" 语言，将文件名存储在 probin 中，
		 * 并在提供时将链接符号名存储在 prosrc 中。
		 * 如果链接符号被省略，则替换为过程名。
		 * 我们也允许将链接符号指定为 "-"，
		 * 因为在 8.4 之前的 PG 版本中这是惯例，
		 * 并且可能仍有旧的转储文件未经处理。
		 */
		*probin_str_p = strVal(linitial(as));
		if (list_length(as) == 1)
			*prosrc_str_p = funcname;
		else
		{
			*prosrc_str_p = strVal(lsecond(as));
			if (strcmp(*prosrc_str_p, "-") == 0)
				*prosrc_str_p = funcname;
		}
	}
	else if (sql_body_in)
	{
		SQLFunctionParseInfoPtr pinfo;

		pinfo = (SQLFunctionParseInfoPtr) palloc0(sizeof(SQLFunctionParseInfo));

		pinfo->fname = funcname;
		pinfo->nargs = list_length(parameterTypes);
		pinfo->argtypes = (Oid *) palloc(pinfo->nargs * sizeof(Oid));
		pinfo->argnames = (char **) palloc(pinfo->nargs * sizeof(char *));
		for (int i = 0; i < list_length(parameterTypes); i++)
		{
			char	   *s = strVal(list_nth(inParameterNames, i));

			pinfo->argtypes[i] = list_nth_oid(parameterTypes, i);
			if (IsPolymorphicType(pinfo->argtypes[i]))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function with unquoted function body cannot have polymorphic arguments")));

			if (s[0] != '\0')
				pinfo->argnames[i] = s;
			else
				pinfo->argnames[i] = NULL;
		}

		if (IsA(sql_body_in, List))
		{
			List	   *stmts = linitial_node(List, castNode(List, sql_body_in));
			ListCell   *lc;
			List	   *transformed_stmts = NIL;

			foreach(lc, stmts)
			{
				Node	   *stmt = lfirst(lc);
				Query	   *q;
				ParseState *pstate = make_parsestate(NULL);

				pstate->p_sourcetext = queryString;
				sql_fn_parser_setup(pstate, pinfo);
				q = transformStmt(pstate, stmt);
				if (q->commandType == CMD_UTILITY)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("%s is not yet supported in unquoted SQL function body",
								   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
				transformed_stmts = lappend(transformed_stmts, q);
				free_parsestate(pstate);
			}

			*sql_body_out = (Node *) list_make1(transformed_stmts);
		}
		else
		{
			Query	   *q;
			ParseState *pstate = make_parsestate(NULL);

			pstate->p_sourcetext = queryString;
			sql_fn_parser_setup(pstate, pinfo);
			q = transformStmt(pstate, sql_body_in);
			if (q->commandType == CMD_UTILITY)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("%s is not yet supported in unquoted SQL function body",
							   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
			free_parsestate(pstate);

			*sql_body_out = (Node *) q;
		}

		/*
		 * 我们必须在 prosrc 中放入一些内容。目前，只记录
		 * 一个空字符串。存储 CREATE FUNCTION 语句的原始文本
		 * 可能有用 --- 但在错误报告中实际使用时，我们还
		 * 需要调整 readfuncs.c，使其在读取 prosqlbody 时
		 * 不丢弃节点位置字段。
		 */
		*prosrc_str_p = pstrdup("");

		/* 但我们绝对不需要 probin。 */
		*probin_str_p = NULL;
	}
	else
	{
		/* 其他所有情况都需要将给定字符串放在 prosrc 中。 */
		*prosrc_str_p = strVal(linitial(as));
		*probin_str_p = NULL;

		if (list_length(as) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only one AS item needed for language \"%s\"",
							languageName)));

		if (languageOid == INTERNALlanguageId)
		{
			/*
			 * 在 6.5 之前的 PostgreSQL 版本中，创建的函数的
			 * SQL 名称不能与内部名称不同，且不使用
			 * "prosrc"。因此有些代码会执行
			 * CREATE FUNCTION xyz AS '' LANGUAGE internal。
			 * 为保持一定程度的向后兼容性，接受空的
			 * "prosrc" 值，其含义等同于提供的 SQL 函数名称。
			 */
			if (strlen(*prosrc_str_p) == 0)
				*prosrc_str_p = funcname;
		}
	}
}


/*
 * CreateFunction
 *	 执行 CREATE FUNCTION（或 CREATE PROCEDURE）工具语句。
 */
ObjectAddress
CreateFunction(ParseState *pstate, CreateFunctionStmt *stmt)
{
	char	   *probin_str;
	char	   *prosrc_str;
	Node	   *prosqlbody;
	Oid			prorettype;
	bool		returnsSet;
	char	   *language;
	Oid			languageOid;
	Oid			languageValidator;
	Node	   *transformDefElem = NULL;
	char	   *funcname;
	Oid			namespaceId;
	AclResult	aclresult;
	oidvector  *parameterTypes;
	List	   *parameterTypes_list = NIL;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *inParameterNames_list = NIL;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	List	   *trftypes_list = NIL;
	ArrayType  *trftypes;
	Oid			requiredResultType;
	bool		isWindowFunc,
				isStrict,
				security,
				isLeakProof;
	char		volatility;
	ArrayType  *proconfig;
	float4		procost;
	float4		prorows;
	Oid			prosupport;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	List	   *as_clause;
	char		parallel;

	/* 将名称列表转换为名称和命名空间 */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->funcname,
													&funcname);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceId));

	/* 设置默认属性 */
	as_clause = NIL;
	language = NULL;
	isWindowFunc = false;
	isStrict = false;
	security = false;
	isLeakProof = false;
	volatility = PROVOLATILE_VOLATILE;
	proconfig = NULL;
	procost = -1;				/* 表示未设置 */
	prorows = -1;				/* 表示未设置 */
	prosupport = InvalidOid;
	parallel = PROPARALLEL_UNSAFE;

	/* 从 stmt->options 列表中提取非默认属性 */
	compute_function_attributes(pstate,
								stmt->is_procedure,
								stmt->options,
								&as_clause, &language, &transformDefElem,
								&isWindowFunc, &volatility,
								&isStrict, &security, &isLeakProof,
								&proconfig, &procost, &prorows,
								&prosupport, &parallel);

	if (!language)
	{
		if (stmt->sql_body)
			language = "sql";
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("no language specified")));
	}

	/* 查找语言并验证权限 */
	languageTuple = SearchSysCache1(LANGNAME, PointerGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", language),
				 (extension_file_exists(language) ?
				  errhint("Use CREATE EXTENSION to load the language into the database.") : 0)));

	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
	languageOid = languageStruct->oid;

	if (languageStruct->lanpltrusted)
	{
		/* 如果是受信语言，需要 USAGE 权限 */
		aclresult = object_aclcheck(LanguageRelationId, languageOid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}
	else
	{
		/* 如果是非受信语言，必须是超级用户 */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}

	languageValidator = languageStruct->lanvalidator;

	ReleaseSysCache(languageTuple);

	/*
	 * 只允许超级用户创建防泄漏函数，因为防泄漏函数
	 * 可以看到尚未被安全屏障视图或行级安全策略过滤
	 * 掉的元组。
	 */
	if (isLeakProof && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can define a leakproof function")));

	if (transformDefElem)
	{
		ListCell   *lc;

		foreach(lc, castNode(List, transformDefElem))
		{
			Oid			typeid = typenameTypeId(NULL,
												lfirst_node(TypeName, lc));
			Oid			elt = get_base_element_type(typeid);

			typeid = elt ? elt : typeid;

			get_transform_oid(typeid, languageOid, false);
			trftypes_list = lappend_oid(trftypes_list, typeid);
		}
	}

	/*
	 * 将 CREATE 的剩余参数转换为 ProcedureCreate 所需的
	 * 形式。
	 */
	interpret_function_parameter_list(pstate,
									  stmt->parameters,
									  languageOid,
									  stmt->is_procedure ? OBJECT_PROCEDURE : OBJECT_FUNCTION,
									  &parameterTypes,
									  &parameterTypes_list,
									  &allParameterTypes,
									  &parameterModes,
									  &parameterNames,
									  &inParameterNames_list,
									  &parameterDefaults,
									  &variadicArgType,
									  &requiredResultType);

	if (stmt->is_procedure)
	{
		Assert(!stmt->returnType);
		prorettype = requiredResultType ? requiredResultType : VOIDOID;
		returnsSet = false;
	}
	else if (stmt->returnType)
	{
		/* 显式 RETURNS 子句 */
		compute_return_type(stmt->returnType, languageOid,
							&prorettype, &returnsSet);
		if (OidIsValid(requiredResultType) && prorettype != requiredResultType)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("function result type must be %s because of OUT parameters",
							format_type_be(requiredResultType))));
	}
	else if (OidIsValid(requiredResultType))
	{
		/* 来自 OUT 参数的默认 RETURNS 子句 */
		prorettype = requiredResultType;
		returnsSet = false;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function result type must be specified")));
		/* 另一种可能：默认使用 RETURNS VOID */
		prorettype = VOIDOID;
		returnsSet = false;
	}

	if (trftypes_list != NIL)
	{
		ListCell   *lc;
		Datum	   *arr;
		int			i;

		arr = palloc(list_length(trftypes_list) * sizeof(Datum));
		i = 0;
		foreach(lc, trftypes_list)
			arr[i++] = ObjectIdGetDatum(lfirst_oid(lc));
		trftypes = construct_array_builtin(arr, list_length(trftypes_list), OIDOID);
	}
	else
	{
		/* 存储 SQL NULL 而非空数组 */
		trftypes = NULL;
	}

	interpret_AS_clause(languageOid, language, funcname, as_clause, stmt->sql_body,
						parameterTypes_list, inParameterNames_list,
						&prosrc_str, &probin_str, &prosqlbody,
						pstate->p_sourcetext);

	/*
	 * 根据其他参数设置 COST 和 ROWS 的默认值；如果不是
	 * returnsSet 则拒绝 ROWS。注意：pg_dump 知道这些默认值，
	 * 如果更改它们请保持同步。
	 */
	if (procost < 0)
	{
		/* SQL 和 PL 语言函数默认代价更高 */
		if (languageOid == INTERNALlanguageId ||
			languageOid == ClanguageId)
			procost = 1;
		else
			procost = 100;
	}
	if (prorows < 0)
	{
		if (returnsSet)
			prorows = 1000;
		else
			prorows = 0;		/* 若非 returnsSet 则为虚拟值 */
	}
	else if (!returnsSet)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ROWS is not applicable when function does not return a set")));

	/*
	 * 现在我们已经拥有了所有参数，并且知道我们被允许这样
	 * 做，继续创建函数。
	 */
	return ProcedureCreate(funcname,
						   namespaceId,
						   stmt->replace,
						   returnsSet,
						   prorettype,
						   GetUserId(),
						   languageOid,
						   languageValidator,
						   prosrc_str,	/* 稍后转换为 text */
						   probin_str,	/* 稍后转换为 text */
						   prosqlbody,
						   stmt->is_procedure ? PROKIND_PROCEDURE : (isWindowFunc ? PROKIND_WINDOW : PROKIND_FUNCTION),
						   security,
						   isLeakProof,
						   isStrict,
						   volatility,
						   parallel,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   PointerGetDatum(trftypes),
						   PointerGetDatum(proconfig),
						   prosupport,
						   procost,
						   prorows);
}

/*
	 * 函数删除的核心逻辑。
	 *
	 * 注意：此函数也用于聚合删除，因为函数和聚合的 OID
	 * 都指向 pg_proc。
 */
void
RemoveFunctionById(Oid funcOid)
{
	Relation	relation;
	HeapTuple	tup;
	char		prokind;

	/*
	 * 删除 pg_proc 元组。
	 */
	relation = table_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	prokind = ((Form_pg_proc) GETSTRUCT(tup))->prokind;

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);

	pgstat_drop_function(funcOid);

	/*
	 * 如果存在 pg_aggregate 元组，也将其删除。
	 */
	if (prokind == PROKIND_AGGREGATE)
	{
		relation = table_open(AggregateRelationId, RowExclusiveLock);

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcOid));
		if (!HeapTupleIsValid(tup)) /* 不应发生 */
			elog(ERROR, "cache lookup failed for pg_aggregate tuple for function %u", funcOid);

		CatalogTupleDelete(relation, &tup->t_self);

		ReleaseSysCache(tup);

		table_close(relation, RowExclusiveLock);
	}
}

/*
 * 实现 ALTER FUNCTION 工具命令
 * （除了 RENAME 和 OWNER 子句，
 * 这些子句作为通用 ALTER 框架的一部分处理）。
 */
ObjectAddress
AlterFunction(ParseState *pstate, AlterFunctionStmt *stmt)
{
	HeapTuple	tup;
	Oid			funcOid;
	Form_pg_proc procForm;
	bool		is_procedure;
	Relation	rel;
	ListCell   *l;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_def_item = NULL;
	DefElem    *leakproof_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;
	DefElem    *support_item = NULL;
	DefElem    *parallel_item = NULL;
	ObjectAddress address;

	rel = table_open(ProcedureRelationId, RowExclusiveLock);

	funcOid = LookupFuncWithArgs(stmt->objtype, stmt->func, false);

	ObjectAddressSet(address, ProcedureRelationId, funcOid);

	tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	procForm = (Form_pg_proc) GETSTRUCT(tup);

	/* 权限检查：必须拥有该函数 */
	if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, stmt->objtype,
					   NameListToString(stmt->func->objname));

	if (procForm->prokind == PROKIND_AGGREGATE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(stmt->func->objname))));

	is_procedure = (procForm->prokind == PROKIND_PROCEDURE);

	/* 检查请求的操作。 */
	foreach(l, stmt->actions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (compute_common_attribute(pstate,
									 is_procedure,
									 defel,
									 &volatility_item,
									 &strict_item,
									 &security_def_item,
									 &leakproof_item,
									 &set_items,
									 &cost_item,
									 &rows_item,
									 &support_item,
									 &parallel_item) == false)
			elog(ERROR, "option \"%s\" not recognized", defel->defname);
	}

	if (volatility_item)
		procForm->provolatile = interpret_func_volatility(volatility_item);
	if (strict_item)
		procForm->proisstrict = boolVal(strict_item->arg);
	if (security_def_item)
		procForm->prosecdef = boolVal(security_def_item->arg);
	if (leakproof_item)
	{
		procForm->proleakproof = boolVal(leakproof_item->arg);
		if (procForm->proleakproof && !superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("only superuser can define a leakproof function")));
	}
	if (cost_item)
	{
		procForm->procost = defGetNumeric(cost_item);
		if (procForm->procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		procForm->prorows = defGetNumeric(rows_item);
		if (procForm->prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
		if (!procForm->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS is not applicable when function does not return a set")));
	}
	if (support_item)
	{
		/* interpret_func_support 处理权限检查 */
		Oid			newsupport = interpret_func_support(support_item);

		/* 添加或替换对支持函数的依赖 */
		if (OidIsValid(procForm->prosupport))
			changeDependencyFor(ProcedureRelationId, funcOid,
								ProcedureRelationId, procForm->prosupport,
								newsupport);
		else
		{
			ObjectAddress referenced;

			referenced.classId = ProcedureRelationId;
			referenced.objectId = newsupport;
			referenced.objectSubId = 0;
			recordDependencyOn(&address, &referenced, DEPENDENCY_NORMAL);
		}

		procForm->prosupport = newsupport;
	}
	if (parallel_item)
		procForm->proparallel = interpret_func_parallel(parallel_item);
	if (set_items)
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *a;
		Datum		repl_val[Natts_pg_proc];
		bool		repl_null[Natts_pg_proc];
		bool		repl_repl[Natts_pg_proc];

		/* 提取现有的 proconfig 设置 */
		datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proconfig, &isnull);
		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		/* 按从左到右的顺序，根据每个 SET 或 RESET 项更新 */
		a = update_proconfig_value(a, set_items);

		/* 更新元组 */
		memset(repl_repl, false, sizeof(repl_repl));
		repl_repl[Anum_pg_proc_proconfig - 1] = true;

		if (a == NULL)
		{
			repl_val[Anum_pg_proc_proconfig - 1] = (Datum) 0;
			repl_null[Anum_pg_proc_proconfig - 1] = true;
		}
		else
		{
			repl_val[Anum_pg_proc_proconfig - 1] = PointerGetDatum(a);
			repl_null[Anum_pg_proc_proconfig - 1] = false;
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								repl_val, repl_null, repl_repl);
	}
	/* 请勿在此下方继续触碰 procForm；它现在是悬空的。 */

	/* 执行更新 */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(ProcedureRelationId, funcOid, 0);

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}


/*
 * CREATE CAST
 */
ObjectAddress
CreateCast(CreateCastStmt *stmt)
{
	Oid			sourcetypeid;
	Oid			targettypeid;
	char		sourcetyptype;
	char		targettyptype;
	Oid			funcid;
	Oid			incastid = InvalidOid;
	Oid			outcastid = InvalidOid;
	int			nargs;
	char		castcontext;
	char		castmethod;
	HeapTuple	tuple;
	AclResult	aclresult;
	ObjectAddress myself;

	sourcetypeid = typenameTypeId(NULL, stmt->sourcetype);
	targettypeid = typenameTypeId(NULL, stmt->targettype);
	sourcetyptype = get_typtype(sourcetypeid);
	targettyptype = get_typtype(targettypeid);

	/* 不允许伪类型 */
	if (sourcetyptype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("source data type %s is a pseudo-type",
						TypeNameToString(stmt->sourcetype))));

	if (targettyptype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("target data type %s is a pseudo-type",
						TypeNameToString(stmt->targettype))));

	/* 权限检查 */
	if (!object_ownercheck(TypeRelationId, sourcetypeid, GetUserId())
		&& !object_ownercheck(TypeRelationId, targettypeid, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of type %s or type %s",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));

	aclresult = object_aclcheck(TypeRelationId, sourcetypeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, sourcetypeid);

	aclresult = object_aclcheck(TypeRelationId, targettypeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettypeid);

	/* 出于历史原因允许域类型，但我们给出警告 */
	if (sourcetyptype == TYPTYPE_DOMAIN)
		ereport(WARNING,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cast will be ignored because the source data type is a domain")));

	else if (targettyptype == TYPTYPE_DOMAIN)
		ereport(WARNING,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cast will be ignored because the target data type is a domain")));

	/* 确定转换方法 */
	if (stmt->func != NULL)
		castmethod = COERCION_METHOD_FUNCTION;
	else if (stmt->inout)
		castmethod = COERCION_METHOD_INOUT;
	else
		castmethod = COERCION_METHOD_BINARY;

	if (castmethod == COERCION_METHOD_FUNCTION)
	{
		Form_pg_proc procstruct;

		funcid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->func, false);

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", funcid);

		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		nargs = procstruct->pronargs;
		if (nargs < 1 || nargs > 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must take one to three arguments")));
		if (!IsBinaryCoercibleWithCast(sourcetypeid,
									   procstruct->proargtypes.values[0],
									   &incastid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("argument of cast function must match or be binary-coercible from source data type")));
		if (nargs > 1 && procstruct->proargtypes.values[1] != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("second argument of cast function must be type %s",
							"integer")));
		if (nargs > 2 && procstruct->proargtypes.values[2] != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("third argument of cast function must be type %s",
							"boolean")));
		if (!IsBinaryCoercibleWithCast(procstruct->prorettype,
									   targettypeid,
									   &outcastid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of cast function must match or be binary-coercible to target data type")));

		/*
		 * 限制转换函数的易变性在理论上可能是个
		 * 好主意，也可能不是，但它确实会破坏许多旧的
		 * 用户定义类型。禁用此检查 --- tgl 2/1/03
		 */
#ifdef NOT_USED
		if (procstruct->provolatile == PROVOLATILE_VOLATILE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not be volatile")));
#endif
		if (procstruct->prokind != PROKIND_FUNCTION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must be a normal function")));
		if (procstruct->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not return a set")));

		ReleaseSysCache(tuple);
	}
	else
	{
		funcid = InvalidOid;
		nargs = 0;
	}

	if (castmethod == COERCION_METHOD_BINARY)
	{
		int16		typ1len;
		int16		typ2len;
		bool		typ1byval;
		bool		typ2byval;
		char		typ1align;
		char		typ2align;

		/*
		 * 必须是超级用户才能创建二进制兼容转换，因为错误的
		 * 转换很容易使后端崩溃。
		 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to create a cast WITHOUT FUNCTION")));

		/*
		 * 此外，坚持要求类型在大小、对齐和按值传递属性上
		 * 匹配；这至少提供了一个粗略的检查，确保它们具有
		 * 相似的表示。未通过此测试的类型对绝对不应该
		 * 被视为等同。
		 */
		get_typlenbyvalalign(sourcetypeid, &typ1len, &typ1byval, &typ1align);
		get_typlenbyvalalign(targettypeid, &typ2len, &typ2byval, &typ2align);
		if (typ1len != typ2len ||
			typ1byval != typ2byval ||
			typ1align != typ2align)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("source and target data types are not physically compatible")));

		/*
		 * 我们知道复合类型、枚举类型和数组类型之间永远
		 * 不是二进制兼容的。它们都嵌入了 OID。
				 *
		 * 理论上你可以构建一个与复合类型、枚举类型或
		 * 数组类型二进制兼容的用户定义基础类型。但我们
		 * 也不允许这样做，因为实际上这种转换肯定是错误。
		 * 你总是可以通过编写转换函数来解决这个问题。
		 */
		if (sourcetyptype == TYPTYPE_COMPOSITE ||
			targettyptype == TYPTYPE_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("composite data types are not binary-compatible")));

		if (sourcetyptype == TYPTYPE_ENUM ||
			targettyptype == TYPTYPE_ENUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("enum data types are not binary-compatible")));

		if (OidIsValid(get_element_type(sourcetypeid)) ||
			OidIsValid(get_element_type(targettypeid)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("array data types are not binary-compatible")));

		/*
		 * 我们也禁止创建涉及域的二进制兼容转换。
		 * 从域到其基础类型的转换已经允许，而反向转换
		 * 应该通过域强制转换来允许约束检查。同样，
		 * 如果你坚持要有自己的语义，请创建一个空操作
		 * 转换函数。
		 *
		 * 注意：如果要放宽此限制，上述对复合类型等
		 * 的检查必须修改为穿透域类型
		 * 到达其基础类型。
		 */
		if (sourcetyptype == TYPTYPE_DOMAIN ||
			targettyptype == TYPTYPE_DOMAIN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("domain data types must not be marked binary-compatible")));
	}

	/*
	 * 仅对长度强转函数允许源类型和目标类型相同。
	 * 我们假定多参数函数执行长度强转。
	 */
	if (sourcetypeid == targettypeid && nargs < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("source data type and target data type are the same")));

	/* 将 CoercionContext 枚举转换为 castcontext 的 char 值 */
	switch (stmt->context)
	{
		case COERCION_IMPLICIT:
			castcontext = COERCION_CODE_IMPLICIT;
			break;
		case COERCION_ASSIGNMENT:
			castcontext = COERCION_CODE_ASSIGNMENT;
			break;
			/* COERCION_PLPGSQL 有意不在此处处理 */
		case COERCION_EXPLICIT:
			castcontext = COERCION_CODE_EXPLICIT;
			break;
		default:
			elog(ERROR, "unrecognized CoercionContext: %d", stmt->context);
			castcontext = 0;	/* 让编译器安静 */
			break;
	}

	myself = CastCreate(sourcetypeid, targettypeid, funcid, incastid, outcastid,
						castcontext, castmethod, DEPENDENCY_NORMAL);
	return myself;
}


static void
check_transform_function(Form_pg_proc procstruct)
{
	if (procstruct->provolatile == PROVOLATILE_VOLATILE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must not be volatile")));
	if (procstruct->prokind != PROKIND_FUNCTION)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must be a normal function")));
	if (procstruct->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must not return a set")));
	if (procstruct->pronargs != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must take one argument")));
	if (procstruct->proargtypes.values[0] != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("first argument of transform function must be type %s",
						"internal")));
}


/*
 * CREATE TRANSFORM
 */
ObjectAddress
CreateTransform(CreateTransformStmt *stmt)
{
	Oid			typeid;
	char		typtype;
	Oid			langid;
	Oid			fromsqlfuncid;
	Oid			tosqlfuncid;
	AclResult	aclresult;
	Form_pg_proc procstruct;
	Datum		values[Natts_pg_transform];
	bool		nulls[Natts_pg_transform] = {0};
	bool		replaces[Natts_pg_transform] = {0};
	Oid			transformid;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Relation	relation;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;
	bool		is_replace;

	/*
	 * 获取类型
	 */
	typeid = typenameTypeId(NULL, stmt->type_name);
	typtype = get_typtype(typeid);

	if (typtype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("data type %s is a pseudo-type",
						TypeNameToString(stmt->type_name))));

	if (typtype == TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("data type %s is a domain",
						TypeNameToString(stmt->type_name))));

	if (!object_ownercheck(TypeRelationId, typeid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeid);

	aclresult = object_aclcheck(TypeRelationId, typeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, typeid);

	/*
	 * 获取语言
	 */
	langid = get_language_oid(stmt->lang, false);

	aclresult = object_aclcheck(LanguageRelationId, langid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_LANGUAGE, stmt->lang);

	/*
	 * 获取函数
	 */
	if (stmt->fromsql)
	{
		fromsqlfuncid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->fromsql, false);

		if (!object_ownercheck(ProcedureRelationId, fromsqlfuncid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, NameListToString(stmt->fromsql->objname));

		aclresult = object_aclcheck(ProcedureRelationId, fromsqlfuncid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, NameListToString(stmt->fromsql->objname));

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(fromsqlfuncid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", fromsqlfuncid);
		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		if (procstruct->prorettype != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of FROM SQL function must be %s",
							"internal")));
		check_transform_function(procstruct);
		ReleaseSysCache(tuple);
	}
	else
		fromsqlfuncid = InvalidOid;

	if (stmt->tosql)
	{
		tosqlfuncid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->tosql, false);

		if (!object_ownercheck(ProcedureRelationId, tosqlfuncid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, NameListToString(stmt->tosql->objname));

		aclresult = object_aclcheck(ProcedureRelationId, tosqlfuncid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, NameListToString(stmt->tosql->objname));

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(tosqlfuncid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", tosqlfuncid);
		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		if (procstruct->prorettype != typeid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of TO SQL function must be the transform data type")));
		check_transform_function(procstruct);
		ReleaseSysCache(tuple);
	}
	else
		tosqlfuncid = InvalidOid;

	/*
	 * 准备就绪
	 */
	values[Anum_pg_transform_trftype - 1] = ObjectIdGetDatum(typeid);
	values[Anum_pg_transform_trflang - 1] = ObjectIdGetDatum(langid);
	values[Anum_pg_transform_trffromsql - 1] = ObjectIdGetDatum(fromsqlfuncid);
	values[Anum_pg_transform_trftosql - 1] = ObjectIdGetDatum(tosqlfuncid);

	relation = table_open(TransformRelationId, RowExclusiveLock);

	tuple = SearchSysCache2(TRFTYPELANG,
							ObjectIdGetDatum(typeid),
							ObjectIdGetDatum(langid));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_transform form = (Form_pg_transform) GETSTRUCT(tuple);

		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("transform for type %s language \"%s\" already exists",
							format_type_be(typeid),
							stmt->lang)));

		replaces[Anum_pg_transform_trffromsql - 1] = true;
		replaces[Anum_pg_transform_trftosql - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation), values, nulls, replaces);
		CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);

		transformid = form->oid;
		ReleaseSysCache(tuple);
		is_replace = true;
	}
	else
	{
		transformid = GetNewOidWithIndex(relation, TransformOidIndexId,
										 Anum_pg_transform_oid);
		values[Anum_pg_transform_oid - 1] = ObjectIdGetDatum(transformid);
		newtuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);
		CatalogTupleInsert(relation, newtuple);
		is_replace = false;
	}

	if (is_replace)
		deleteDependencyRecordsFor(TransformRelationId, transformid, true);

	addrs = new_object_addresses();

	/* 建立依赖条目 */
	ObjectAddressSet(myself, TransformRelationId, transformid);

	/* 对语言的依赖 */
	ObjectAddressSet(referenced, LanguageRelationId, langid);
	add_exact_object_address(&referenced, addrs);

	/* 对类型的依赖 */
	ObjectAddressSet(referenced, TypeRelationId, typeid);
	add_exact_object_address(&referenced, addrs);

	/* 对函数的依赖 */
	if (OidIsValid(fromsqlfuncid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, fromsqlfuncid);
		add_exact_object_address(&referenced, addrs);
	}
	if (OidIsValid(tosqlfuncid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, tosqlfuncid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, is_replace);

	/* 新转换的创建后钩子 */
	InvokeObjectPostCreateHook(TransformRelationId, transformid, 0);

	heap_freetuple(newtuple);

	table_close(relation, RowExclusiveLock);

	return myself;
}


/*
 * get_transform_oid - 给定类型 OID 和语言 OID，查找转换 OID
 *
 * 如果 missing_ok 为 false 且未找到转换，则抛出错误。
 * 如果为 true，则仅返回 InvalidOid。
 */
Oid
get_transform_oid(Oid type_id, Oid lang_id, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid2(TRFTYPELANG, Anum_pg_transform_oid,
						  ObjectIdGetDatum(type_id),
						  ObjectIdGetDatum(lang_id));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("transform for type %s language \"%s\" does not exist",
						format_type_be(type_id),
						get_language_name(lang_id, false))));
	return oid;
}


/*
 * ALTER FUNCTION/AGGREGATE SET SCHEMA/RENAME 的子过程
 *
 * 给定的命名空间中是否已存在具有给定名称和签名
 * 的函数？如果是，则引发相应的错误消息。
 */
void
IsThereFunctionInNamespace(const char *proname, int pronargs,
						   oidvector *proargtypes, Oid nspOid)
{
	/* 检查重复名称（比唯一索引失败更友好） */
	if (SearchSysCacheExists3(PROCNAMEARGSNSP,
							  CStringGetDatum(proname),
							  PointerGetDatum(proargtypes),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(proname, pronargs,
												  NIL, proargtypes->values),
						get_namespace_name(nspOid))));
}

/*
 * ExecuteDoStmt
 *		执行内联过程语言代码
 *
 * 关于 atomic 参数请参阅 ExecuteCallStmt()。
 */
void
ExecuteDoStmt(ParseState *pstate, DoStmt *stmt, bool atomic)
{
	InlineCodeBlock *codeblock = makeNode(InlineCodeBlock);
	ListCell   *arg;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	char	   *language;
	Oid			laninline;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;

	/* 处理从 gram.y 获取的选项 */
	foreach(arg, stmt->args)
	{
		DefElem    *defel = (DefElem *) lfirst(arg);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				errorConflictingDefElem(defel, pstate);
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				errorConflictingDefElem(defel, pstate);
			language_item = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (as_item)
		codeblock->source_text = strVal(as_item->arg);
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("no inline code specified")));

	/* 如果未指定 LANGUAGE 选项，使用默认值 */
	if (language_item)
		language = strVal(language_item->arg);
	else
		language = "plpgsql";

	/* 查找语言并验证权限 */
	languageTuple = SearchSysCache1(LANGNAME, PointerGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", language),
				 (extension_file_exists(language) ?
				  errhint("Use CREATE EXTENSION to load the language into the database.") : 0)));

	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
	codeblock->langOid = languageStruct->oid;
	codeblock->langIsTrusted = languageStruct->lanpltrusted;
	codeblock->atomic = atomic;

	if (languageStruct->lanpltrusted)
	{
		/* 如果是受信语言，需要 USAGE 权限 */
		AclResult	aclresult;

		aclresult = object_aclcheck(LanguageRelationId, codeblock->langOid, GetUserId(),
									ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}
	else
	{
		/* 如果是非受信语言，必须是超级用户 */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}

	/* 获取处理器函数的 OID */
	laninline = languageStruct->laninline;
	if (!OidIsValid(laninline))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("language \"%s\" does not support inline code execution",
						NameStr(languageStruct->lanname))));

	ReleaseSysCache(languageTuple);

	/* 执行内联处理器 */
	OidFunctionCall1(laninline, PointerGetDatum(codeblock));
}

/*
 * 执行 CALL 语句    
 *
 * 在顶层 CALL 语句内部，允许使用事务终止类的命令，      
 * 例如 COMMIT 或 PL 特定的等价命令。SQL 标准中的术           
 * 语是：CALL 建立一个非原子的执行上下文。大多数其   
 * 他命令建立的是原子执行上下文，在其中不允许事务     
 * 控制动作。如果存在 CALL 的嵌套执行，我们希望递归    
 * 地跟踪执行上下文，以便嵌套的 CALL 也能进行事务    
 * 控制。但请注意，例如在 CALL -> SELECT -> CALL 中，           
 * 第二个调用不能进行事务控制，因为中间的 SELECT      
 * 建立了一个原子执行上下文。                               
 *
 * 因此当 ExecuteCallStmt() 从顶层被调用时，我们传入           
 * atomic = false（回想一下，那意味着允许事务）。我   
 * 们随后创建一个内容为 atomic = false 的 CallContext       
 * 节点，经 fcinfo->context 字段传入过程调用。语言  
 * 实现随后应采取适当措施，依据该信息允许或禁止事   
 * 务命令，例如 SPI_connect_ext(SPI_OPT_NONATOMIC)。  
 * 语言还应把 atomic 标志传递给任何嵌套的 CALL 调用。     
 *                                               
 *
 * 我们在本函数内创建的表达式数据结构和执行上下 
 * 文，是 CALL 实用语句所运行的 Portal 的 portalContext    
 * 的子对象。因此，传给过程的任何按引用传递的     
 * 值都能在过程内部可能发生的事务提交之后幸存下 
 * 来。                                        
 */
void
ExecuteCallStmt(CallStmt *stmt, ParamListInfo params, bool atomic, DestReceiver *dest)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	ListCell   *lc;
	FuncExpr   *fexpr;
	int			nargs;
	int			i;
	AclResult	aclresult;
	FmgrInfo	flinfo;
	CallContext *callcontext;
	EState	   *estate;
	ExprContext *econtext;
	HeapTuple	tp;
	PgStat_FunctionCallUsage fcusage;
	Datum		retval;

	fexpr = stmt->funcexpr;
	Assert(fexpr);
	Assert(IsA(fexpr, FuncExpr));

	aclresult = object_aclcheck(ProcedureRelationId, fexpr->funcid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_PROCEDURE, get_func_name(fexpr->funcid));

	/* 准备我们将传递给过程的上下文对象 */
	callcontext = makeNode(CallContext);
	callcontext->atomic = atomic;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * 如果设置了 proconfig，由于 GUC 堆叠的工作方式，
	 * 我们不能允许事务命令：事务边界必须将 proconfig
	 * 设置从栈中弹出。通过稍微重新设计 GUC 嵌套机制
	 * 可以解除此限制。
	 */
	if (!heap_attisnull(tp, Anum_pg_proc_proconfig, NULL))
		callcontext->atomic = true;

	/*
	 * 在 SECURITY DEFINER 过程中，我们不能允许事务命令。
	 * StartTransaction() 坚持安全上下文栈为空，而
	 * AbortTransaction() 会重置安全上下文。这可以重新
	 * 组织，但目前它不能工作。
	 */
	if (((Form_pg_proc) GETSTRUCT(tp))->prosecdef)
		callcontext->atomic = true;

	ReleaseSysCache(tp);

	/* 安全检查；参见 ExecInitFunc() */
	nargs = list_length(fexpr->args);
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a procedure",
							   "cannot pass more than %d arguments to a procedure",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* 初始化函数调用结构 */
	InvokeFunctionExecuteHook(fexpr->funcid);
	fmgr_info(fexpr->funcid, &flinfo);
	fmgr_info_set_expr((Node *) fexpr, &flinfo);
	InitFunctionCallInfoData(*fcinfo, &flinfo, nargs, fexpr->inputcollid,
							 (Node *) callcontext, NULL);

	/*
	 * 在适当的执行上下文中求值过程参数。注意，直到过程
	 * 返回我们才能释放此上下文。
	 */
	estate = CreateExecutorState();
	estate->es_param_list_info = params;
	econtext = CreateExprContext(estate);

	/*
	 * 如果我们在非原子上下文中被调用，我们还必须确保
	 * 参数表达式使用最新的快照运行。我们的调用者会在
	 * 原子上下文中提供当前快照，但不会在非原子上下文
	 * 中提供，因为 COMMIT/ROLLBACK 可能破坏快照使得
	 * 高层管理过于复杂。
	 */
	if (!atomic)
		PushActiveSnapshot(GetTransactionSnapshot());

	i = 0;
	foreach(lc, fexpr->args)
	{
		ExprState  *exprstate;
		Datum		val;
		bool		isnull;

		exprstate = ExecPrepareExpr(lfirst(lc), estate);

		val = ExecEvalExprSwitchContext(exprstate, econtext, &isnull);

		fcinfo->args[i].value = val;
		fcinfo->args[i].isnull = isnull;

		i++;
	}

	/* 清除参数的临时快照（如果我们创建过一个） */
	if (!atomic)
		PopActiveSnapshot();

	/* 在这里我们实际调用过程 */
	pgstat_init_function_usage(fcinfo, &fcusage);
	retval = FunctionCallInvoke(fcinfo);
	pgstat_end_function_usage(&fcusage, true);

	/* 处理过程的输出 */
	if (fexpr->funcresulttype == VOIDOID)
	{
		/* 无操作 */
	}
	else if (fexpr->funcresulttype == RECORDOID)
	{
		/* 发送元组到客户端 */
		HeapTupleHeader td;
		Oid			tupType;
		int32		tupTypmod;
		TupleDesc	retdesc;
		HeapTupleData rettupdata;
		TupOutputState *tstate;
		TupleTableSlot *slot;

		if (fcinfo->isnull)
			elog(ERROR, "procedure returned null record");

		/*
		 * 在执行操作时确保有活动快照。
		 * 注意，这*不*足以使返回数据中包含的 TOAST 指针
		 * 变得安全：引用的
		 * 数据可能在我们不持有快照期间被清除了。因此，
		 * 能够执行 COMMIT/ROLLBACK 的 PL 有责任不返回
		 * TOAST 指针，除非这些指针是在过程中最后一次
		 * COMMIT/ROLLBACK 之后获取的。
		 *
		 * XXX 这是一个非常讨厌的、难以测试的需求。有没有
		 * 办法移除它？
		 */
		EnsurePortalSnapshotExists();

		td = DatumGetHeapTupleHeader(retval);
		tupType = HeapTupleHeaderGetTypeId(td);
		tupTypmod = HeapTupleHeaderGetTypMod(td);
		retdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

		tstate = begin_tup_output_tupdesc(dest, retdesc,
										  &TTSOpsHeapTuple);

		rettupdata.t_len = HeapTupleHeaderGetDatumLength(td);
		ItemPointerSetInvalid(&(rettupdata.t_self));
		rettupdata.t_tableOid = InvalidOid;
		rettupdata.t_data = td;

		slot = ExecStoreHeapTuple(&rettupdata, tstate->slot, false);
		tstate->dest->receiveSlot(slot, tstate->dest);

		end_tup_output(tstate);

		ReleaseTupleDesc(retdesc);
	}
	else
		elog(ERROR, "unexpected result type for procedure: %u",
			 fexpr->funcresulttype);

	FreeExecutorState(estate);
}

/*
 * 为 CALL 语句返回值构造元组描述符
 */
TupleDesc
CallStmtResultDesc(CallStmt *stmt)
{
	FuncExpr   *fexpr;
	HeapTuple	tuple;
	TupleDesc	tupdesc;

	fexpr = stmt->funcexpr;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for procedure %u", fexpr->funcid);

	tupdesc = build_function_result_tupdesc_t(tuple);

	ReleaseSysCache(tuple);

	/*
	 * build_function_result_tupdesc_t 的结果具有正确的列名称，
	 * 但它只有声明的输出参数类型，在多态情况下这是错误的。
	 * 通过检查 stmt->outargs 获取正确的类型。我们有意将
	 * atttypmod 保持为 -1，attcollation 保持为类型的默认值，
	 * 因为这对函数输出总是合适的；没理由考虑 outargs 中
	 * 可用的任何额外信息。
	 * 注意，如果没有 outargs，
	 * tupdesc 为 null。
	 */
	if (tupdesc)
	{
		Assert(tupdesc->natts == list_length(stmt->outargs));
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			Node	   *outarg = (Node *) list_nth(stmt->outargs, i);

			TupleDescInitEntry(tupdesc,
							   i + 1,
							   NameStr(att->attname),
							   exprType(outarg),
							   -1,
							   0);
		}
	}

	return tupdesc;
}
