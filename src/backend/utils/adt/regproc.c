/*-------------------------------------------------------------------------
 *
 * regproc.c
 *	  内建类型 regproc、regclass、regtype 等类型的函数。
 *
 * 这些类型都与 Oid 二进制兼容，并依靠 Oid 来做
 * 比较之类的事情。它们唯一有趣的行为在于特殊的
 * I/O 转换例程中。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/regproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

static bool parseNumericOid(char *string, Oid *result, Node *escontext);
static bool parseDashOrOid(char *string, Oid *result, Node *escontext);
static bool parseNameAndArgTypes(const char *string, bool allowNone,
								 List **names, int *nargs, Oid *argtypes,
								 Node *escontext);


/*****************************************************************************
 *	 用户 I/O 例程									 *
 *****************************************************************************/

/*
 * regprocin		- 将 "proname" 转为过程 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_proc 条目
 */
Datum
regprocin(PG_FUNCTION_ARGS)
{
	char	   *pro_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	RegProcedure result;
	List	   *names;
	FuncCandidateList clist;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(pro_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 否则是名称，可能带模式限定 */

	/*
	 * 在 bootstrap 下绝不应走到这里，因为所有引用都应
	 * 已由 genbki.pl 解析。
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regproc values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_proc 条目。
	 */
	names = stringToQualifiedNameList(pro_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	clist = FuncnameGetCandidates(names, -1, NIL, false, false, false, true);

	if (clist == NULL)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));
	else if (clist->next != NULL)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one function named \"%s\"",
						pro_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regproc	- 把 "proname" 转为 proc OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regproc(PG_FUNCTION_ARGS)
{
	char	   *pro_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regprocin, pro_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regprocout		- 把过程 OID 转为 "pro_name"
 */
Datum
regprocout(PG_FUNCTION_ARGS)
{
	RegProcedure proid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	proctup;

	if (proid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(proid));

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);

		/*
		 * 在 bootstrap 下跳过花哨的命名空间机制，直接
		 * 返回过程名。（此路径反正仅调试输出需要）
		 *
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(proname);
		else
		{
			char	   *nspname;
			FuncCandidateList clist;

			/*
			 * regprocin 能否唯一找到它？若不能，
			 * 就限定
			 */
			clist = FuncnameGetCandidates(list_make1(makeString(proname)),
										  -1, NIL, false, false, false, false);
			if (clist != NULL && clist->next == NULL &&
				clist->oid == proid)
				nspname = NULL;
			else
				nspname = get_namespace_name(procform->pronamespace);

			result = quote_qualified_identifier(nspname, proname);
		}

		ReleaseSysCache(proctup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_proc 条目则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", proid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regprocrecv			- 把外部二进制格式转为 regproc
 */
Datum
regprocrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regprocsend			- 把 regproc 转为二进制格式
 */
Datum
regprocsend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regprocedurein		- 把 "proname(args)" 转为过程 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_proc 条目
 */
Datum
regprocedurein(PG_FUNCTION_ARGS)
{
	char	   *pro_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	RegProcedure result;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];
	FuncCandidateList clist;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(pro_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regprocedure values must be OIDs in bootstrap mode");

	/*
	 * 否则为名称和参数。解析名称与参数，在命名空间
	 * 搜索列表中查找潜在匹配，扫描找出与给定参数类型
	 * 完全匹配的那一个。（匹配不会
	 * 多于一个。）
	 */
	if (!parseNameAndArgTypes(pro_name_or_oid, false,
							  &names, &nargs, argtypes,
							  escontext))
		PG_RETURN_NULL();

	clist = FuncnameGetCandidates(names, nargs, NIL, false, false,
								  false, true);

	for (; clist; clist = clist->next)
	{
		if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (clist == NULL)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", pro_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regprocedure	- 把 "proname(args)" 转为 proc OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regprocedure(PG_FUNCTION_ARGS)
{
	char	   *pro_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regprocedurein, pro_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * format_procedure		- 把过程 OID 转为 "pro_name(args)"
 *
 * 这里导出 regprocedureout 的实用功能，供其他后端
 * 模块使用。结果是 palloc 分配的字符串。
 */
char *
format_procedure(Oid procedure_oid)
{
	return format_procedure_extended(procedure_oid, 0);
}

char *
format_procedure_qualified(Oid procedure_oid)
{
	return format_procedure_extended(procedure_oid, FORMAT_PROC_FORCE_QUALIFY);
}

/*
 * format_procedure_extended - 把过程 OID 转为 "pro_name(args)"
 *
 * 这里导出 regprocedureout 的实用功能，供其他后端
 * 模块使用。结果是 palloc 分配的字符串，或 NULL。
 *
 * 产生 regprocedure 名称例程；见上方 format_procedure。
 *
 * "flags" 中的下列位会修改行为：
 * - FORMAT_PROC_INVALID_AS_NULL
 *			若过程 OID 无效或未知，返回 NULL 而非数字
 *			OID。
 * - FORMAT_PROC_FORCE_QUALIFY
 *			无论 search_path 如何，总是对过程名加模式限定
 */
char *
format_procedure_extended(Oid procedure_oid, bits16 flags)
{
	char	   *result;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		int			i;
		char	   *nspname;
		StringInfoData buf;

		/* XXX bootstrap 下这里没有支持 */
		Assert(!IsBootstrapProcessingMode());

		initStringInfo(&buf);

		/*
		 * regprocedurein（带正确参数）能找到该过程吗？
		 * 若不能，或调用者要求，则须加限定。
		 */
		if ((flags & FORMAT_PROC_FORCE_QUALIFY) == 0 &&
			FunctionIsVisible(procedure_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name(procform->pronamespace);

		appendStringInfo(&buf, "%s(",
						 quote_qualified_identifier(nspname, proname));
		for (i = 0; i < nargs; i++)
		{
			Oid			thisargtype = procform->proargtypes.values[i];

			if (i > 0)
				appendStringInfoChar(&buf, ',');
			appendStringInfoString(&buf,
								   (flags & FORMAT_PROC_FORCE_QUALIFY) != 0 ?
								   format_type_be_qualified(thisargtype) :
								   format_type_be(thisargtype));
		}
		appendStringInfoChar(&buf, ')');

		result = buf.data;

		ReleaseSysCache(proctup);
	}
	else if ((flags & FORMAT_PROC_INVALID_AS_NULL) != 0)
	{
		/* 若对象未定义，则按调用者要求返回 NULL */
		result = NULL;
	}
	else
	{
		/* 若 OID 不匹配任何 pg_proc 条目则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", procedure_oid);
	}

	return result;
}

/*
 * 为给定 OID 的过程输出 objname/objargs 表示形式。
 * 若它不存在，则抛出错误。
 *
 * 这可用于喂给 get_object_address。
 */
void
format_procedure_parts(Oid procedure_oid, List **objnames, List **objargs,
					   bool missing_ok)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	int			nargs;
	int			i;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));

	if (!HeapTupleIsValid(proctup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for procedure with OID %u", procedure_oid);
		return;
	}

	procform = (Form_pg_proc) GETSTRUCT(proctup);
	nargs = procform->pronargs;

	*objnames = list_make2(get_namespace_name_or_temp(procform->pronamespace),
						   pstrdup(NameStr(procform->proname)));
	*objargs = NIL;
	for (i = 0; i < nargs; i++)
	{
		Oid			thisargtype = procform->proargtypes.values[i];

		*objargs = lappend(*objargs, format_type_be_qualified(thisargtype));
	}

	ReleaseSysCache(proctup);
}

/*
 * regprocedureout		- 把过程 OID 转为 "pro_name(args)"
 */
Datum
regprocedureout(PG_FUNCTION_ARGS)
{
	RegProcedure proid = PG_GETARG_OID(0);
	char	   *result;

	if (proid == InvalidOid)
		result = pstrdup("-");
	else
		result = format_procedure(proid);

	PG_RETURN_CSTRING(result);
}

/*
 *		regprocedurerecv			- 把外部二进制格式转为 regprocedure
 */
Datum
regprocedurerecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regproceduresend			- 把 regprocedure 转为二进制格式
 */
Datum
regproceduresend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regoperin		- 把 "oprname" 转为算子 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "0" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_operator 条目
 */
Datum
regoperin(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;
	FuncCandidateList clist;

	/* 处理 "0" 或数字 OID */
	if (parseNumericOid(opr_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 否则是名称，可能带模式限定 */

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regoper values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_operator 条目。
	 */
	names = stringToQualifiedNameList(opr_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	clist = OpernameGetCandidates(names, '\0', true);

	if (clist == NULL)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s", opr_name_or_oid)));
	else if (clist->next != NULL)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one operator named %s",
						opr_name_or_oid)));

	result = clist->oid;

	PG_RETURN_OID(result);
}

/*
 * to_regoper		- 把 "oprname" 转为算子 OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regoper(PG_FUNCTION_ARGS)
{
	char	   *opr_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regoperin, opr_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regoperout		- 把算子 OID 转为 "opr_name"
 */
Datum
regoperout(PG_FUNCTION_ARGS)
{
	Oid			oprid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	opertup;

	if (oprid == InvalidOid)
	{
		result = pstrdup("0");
		PG_RETURN_CSTRING(result);
	}

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oprid));

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);

		/*
		 * 在 bootstrap 下跳过花哨的命名空间机制，直接
		 * 返回算子名。（此路径反正仅调试输出需要）
		 *
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(oprname);
		else
		{
			FuncCandidateList clist;

			/*
			 * regoperin 能否唯一找到它？若不能，
			 * 就限定
			 */
			clist = OpernameGetCandidates(list_make1(makeString(oprname)),
										  '\0', false);
			if (clist != NULL && clist->next == NULL &&
				clist->oid == oprid)
				result = pstrdup(oprname);
			else
			{
				const char *nspname;

				nspname = get_namespace_name(operform->oprnamespace);
				nspname = quote_identifier(nspname);
				result = (char *) palloc(strlen(nspname) + strlen(oprname) + 2);
				sprintf(result, "%s.%s", nspname, oprname);
			}
		}

		ReleaseSysCache(opertup);
	}
	else
	{
		/*
		 * 若 OID 不匹配任何 pg_operator 条目则按数字返回
		 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", oprid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regoperrecv			- 把外部二进制格式转为 regoper
 */
Datum
regoperrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regopersend			- 把 regoper 转为二进制格式
 */
Datum
regopersend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regoperatorin		- 把 "oprname(args)" 转为算子 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "0" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_operator 条目
 */
Datum
regoperatorin(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];

	/* 处理 "0" 或数字 OID */
	if (parseNumericOid(opr_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regoperator values must be OIDs in bootstrap mode");

	/*
	 * 否则为名称和参数。解析名称与参数，在命名空间
	 * 搜索列表中查找潜在匹配，扫描找出与给定参数类型
	 * 完全匹配的那一个。（匹配不会
	 * 多于一个。）
	 */
	if (!parseNameAndArgTypes(opr_name_or_oid, true,
							  &names, &nargs, argtypes,
							  escontext))
		PG_RETURN_NULL();

	if (nargs == 1)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("missing argument"),
				 errhint("Use NONE to denote the missing argument of a unary operator.")));
	if (nargs != 2)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments"),
				 errhint("Provide two argument types for operator.")));

	result = OpernameGetOprid(names, argtypes[0], argtypes[1]);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s", opr_name_or_oid)));

	PG_RETURN_OID(result);
}

/*
 * to_regoperator	- 把 "oprname(args)" 转为算子 OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regoperator(PG_FUNCTION_ARGS)
{
	char	   *opr_name_or_oid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regoperatorin, opr_name_or_oid,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * format_operator_extended - 把算子 OID 转为 "opr_name(args)"
 *
 * 这里导出 regoperatorout 的实用功能，供其他后端
 * 模块使用。结果是 palloc 分配的字符串，或 NULL。
 *
 * "flags" 中的下列位会修改行为：
 * - FORMAT_OPERATOR_INVALID_AS_NULL
 *			若算子 OID 无效或未知，返回 NULL 而非数字
 *			OID。
 * - FORMAT_OPERATOR_FORCE_QUALIFY
 *			无论 search_path 如何，总是对算子名加模式限定
 */
char *
format_operator_extended(Oid operator_oid, bits16 flags)
{
	char	   *result;
	HeapTuple	opertup;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operator_oid));

	if (HeapTupleIsValid(opertup))
	{
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		char	   *oprname = NameStr(operform->oprname);
		char	   *nspname;
		StringInfoData buf;

		/* XXX bootstrap 下这里没有支持 */
		Assert(!IsBootstrapProcessingMode());

		initStringInfo(&buf);

		/*
		 * regoperatorin（带正确参数）能找到该算子吗？
		 * 若不能，或调用者明确要求，我们就需要加限定。
		 */
		if ((flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ||
			!OperatorIsVisible(operator_oid))
		{
			nspname = get_namespace_name(operform->oprnamespace);
			appendStringInfo(&buf, "%s.",
							 quote_identifier(nspname));
		}

		appendStringInfo(&buf, "%s(", oprname);

		if (operform->oprleft)
			appendStringInfo(&buf, "%s,",
							 (flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ?
							 format_type_be_qualified(operform->oprleft) :
							 format_type_be(operform->oprleft));
		else
			appendStringInfoString(&buf, "NONE,");

		if (operform->oprright)
			appendStringInfo(&buf, "%s)",
							 (flags & FORMAT_OPERATOR_FORCE_QUALIFY) != 0 ?
							 format_type_be_qualified(operform->oprright) :
							 format_type_be(operform->oprright));
		else
			appendStringInfoString(&buf, "NONE)");

		result = buf.data;

		ReleaseSysCache(opertup);
	}
	else if ((flags & FORMAT_OPERATOR_INVALID_AS_NULL) != 0)
	{
		/* 若对象未定义，则按调用者要求返回 NULL */
		result = NULL;
	}
	else
	{
		/*
		 * 若 OID 不匹配任何 pg_operator 条目则按数字返回
		 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", operator_oid);
	}

	return result;
}

char *
format_operator(Oid operator_oid)
{
	return format_operator_extended(operator_oid, 0);
}

char *
format_operator_qualified(Oid operator_oid)
{
	return format_operator_extended(operator_oid,
									FORMAT_OPERATOR_FORCE_QUALIFY);
}

void
format_operator_parts(Oid operator_oid, List **objnames, List **objargs,
					  bool missing_ok)
{
	HeapTuple	opertup;
	Form_pg_operator oprForm;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operator_oid));
	if (!HeapTupleIsValid(opertup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for operator with OID %u",
				 operator_oid);
		return;
	}

	oprForm = (Form_pg_operator) GETSTRUCT(opertup);
	*objnames = list_make2(get_namespace_name_or_temp(oprForm->oprnamespace),
						   pstrdup(NameStr(oprForm->oprname)));
	*objargs = NIL;
	if (oprForm->oprleft)
		*objargs = lappend(*objargs,
						   format_type_be_qualified(oprForm->oprleft));
	if (oprForm->oprright)
		*objargs = lappend(*objargs,
						   format_type_be_qualified(oprForm->oprright));

	ReleaseSysCache(opertup);
}

/*
 * regoperatorout		- 把算子 OID 转为 "opr_name(args)"
 */
Datum
regoperatorout(PG_FUNCTION_ARGS)
{
	Oid			oprid = PG_GETARG_OID(0);
	char	   *result;

	if (oprid == InvalidOid)
		result = pstrdup("0");
	else
		result = format_operator(oprid);

	PG_RETURN_CSTRING(result);
}

/*
 *		regoperatorrecv			- 把外部二进制格式转为 regoperator
 */
Datum
regoperatorrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regoperatorsend			- 把 regoperator 转为二进制格式
 */
Datum
regoperatorsend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regclassin		- 把 "classname" 转为类 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_class 条目
 */
Datum
regclassin(PG_FUNCTION_ARGS)
{
	char	   *class_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(class_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 否则是名称，可能带模式限定 */

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regclass values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_class 条目。
	 */
	names = stringToQualifiedNameList(class_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	/* 我们对这个关系可能连权限都没有；不要锁定它。 */
	result = RangeVarGetRelid(makeRangeVarFromNameList(names), NoLock, true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist",
						NameListToString(names))));

	PG_RETURN_OID(result);
}

/*
 * to_regclass		- 把 "classname" 转为类 OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regclass(PG_FUNCTION_ARGS)
{
	char	   *class_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regclassin, class_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regclassout		- 把类 OID 转为 "class_name"
 */
Datum
regclassout(PG_FUNCTION_ARGS)
{
	Oid			classid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	classtup;

	if (classid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(classid));

	if (HeapTupleIsValid(classtup))
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(classtup);
		char	   *classname = NameStr(classform->relname);

		/*
		 * 在 bootstrap 下跳过花哨的命名空间机制，直接
		 * 返回类名。（此路径反正仅调试输出需要）
		 *
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(classname);
		else
		{
			char	   *nspname;

			/*
			 * regclassin 能否找到它？若不能，则加限定
			 */
			if (RelationIsVisible(classid))
				nspname = NULL;
			else
				nspname = get_namespace_name(classform->relnamespace);

			result = quote_qualified_identifier(nspname, classname);
		}

		ReleaseSysCache(classtup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_class 条目则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", classid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regclassrecv			- 把外部二进制格式转为 regclass
 */
Datum
regclassrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regclasssend			- 把 regclass 转为二进制格式
 */
Datum
regclasssend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regcollationin		- 把 "collationname" 转为 collation OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_collation 条目
 */
Datum
regcollationin(PG_FUNCTION_ARGS)
{
	char	   *collation_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(collation_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 否则是名称，可能带模式限定 */

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regcollation values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_collation 条目。
	 */
	names = stringToQualifiedNameList(collation_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	result = get_collation_oid(names, true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" does not exist",
						NameListToString(names), GetDatabaseEncodingName())));

	PG_RETURN_OID(result);
}

/*
 * to_regcollation		- 把 "collationname" 转为 collation OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regcollation(PG_FUNCTION_ARGS)
{
	char	   *collation_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regcollationin, collation_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regcollationout		- 把 collation OID 转为 "collation_name"
 */
Datum
regcollationout(PG_FUNCTION_ARGS)
{
	Oid			collationid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	collationtup;

	if (collationid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	collationtup = SearchSysCache1(COLLOID, ObjectIdGetDatum(collationid));

	if (HeapTupleIsValid(collationtup))
	{
		Form_pg_collation collationform = (Form_pg_collation) GETSTRUCT(collationtup);
		char	   *collationname = NameStr(collationform->collname);

		/*
		 * 在 bootstrap 下跳过花哨的命名空间机制，直接
		 * 返回 collation 名。（此路径反正仅调试输出需要）
		 *
		 */
		if (IsBootstrapProcessingMode())
			result = pstrdup(collationname);
		else
		{
			char	   *nspname;

			/*
			 * regcollationin 能否找到它？若不能，
			 * 就限定
			 */
			if (CollationIsVisible(collationid))
				nspname = NULL;
			else
				nspname = get_namespace_name(collationform->collnamespace);

			result = quote_qualified_identifier(nspname, collationname);
		}

		ReleaseSysCache(collationtup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_collation 条目则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", collationid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regcollationrecv			- 把外部二进制格式转为 regcollation
 */
Datum
regcollationrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regcollationsend			- 把 regcollation 转为二进制格式
 */
Datum
regcollationsend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regtypein		- 把 "typename" 转为类型 OID
 *
 * 类型名可以用解析器识别的完整类型语法来指定；
 * 例如 DOUBLE PRECISION 和 INTEGER[] 都可行并会被
 * 转换为正确类型名。（不过，解析器生成的
 * 任何 typmod 信息）
 *
 * 为了与输出例程对称，我们也接受数字 OID，
 * 以及 bootstrap 下的可能用途。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_type 条目
 */
Datum
regtypein(PG_FUNCTION_ARGS)
{
	char	   *typ_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	int32		typmod;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(typ_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 否则是类型名，可能带模式限定或修饰 */

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regtype values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：调用完整解析器处理数组语法等特例。
	 * 无须检查 parseTypeString 是否失败，
	 * 反正我们都会返回。
	 */
	(void) parseTypeString(typ_name_or_oid, &result, &typmod, escontext);

	PG_RETURN_OID(result);
}

/*
 * to_regtype		- 把 "typename" 转为类型 OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regtype(PG_FUNCTION_ARGS)
{
	char	   *typ_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regtypein, typ_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regtypeout		- 把类型 OID 转为 "typ_name"
 */
Datum
regtypeout(PG_FUNCTION_ARGS)
{
	Oid			typid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	typetup;

	if (typid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	typetup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));

	if (HeapTupleIsValid(typetup))
	{
		Form_pg_type typeform = (Form_pg_type) GETSTRUCT(typetup);

		/*
		 * 在 bootstrap 下跳过花哨的命名空间机制，直接
		 * 返回类型名。（此路径反正仅调试输出需要）
		 *
		 */
		if (IsBootstrapProcessingMode())
		{
			char	   *typname = NameStr(typeform->typname);

			result = pstrdup(typname);
		}
		else
			result = format_type_be(typid);

		ReleaseSysCache(typetup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_type 条目则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", typid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regtyperecv			- 把外部二进制格式转为 regtype
 */
Datum
regtyperecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regtypesend			- 把 regtype 转为二进制格式
 */
Datum
regtypesend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regconfigin		- 把 "tsconfigname" 转为 tsconfig OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_ts_config 条目
 */
Datum
regconfigin(PG_FUNCTION_ARGS)
{
	char	   *cfg_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(cfg_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regconfig values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_ts_config 条目。
	 */
	names = stringToQualifiedNameList(cfg_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	result = get_ts_config_oid(names, true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(names))));

	PG_RETURN_OID(result);
}

/*
 * regconfigout		- 把 tsconfig OID 转为 "tsconfigname"
 */
Datum
regconfigout(PG_FUNCTION_ARGS)
{
	Oid			cfgid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	cfgtup;

	if (cfgid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	cfgtup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgid));

	if (HeapTupleIsValid(cfgtup))
	{
		Form_pg_ts_config cfgform = (Form_pg_ts_config) GETSTRUCT(cfgtup);
		char	   *cfgname = NameStr(cfgform->cfgname);
		char	   *nspname;

		/*
		 * regconfigin 能否找到它？若不能，则加限定
		 */
		if (TSConfigIsVisible(cfgid))
			nspname = NULL;
		else
			nspname = get_namespace_name(cfgform->cfgnamespace);

		result = quote_qualified_identifier(nspname, cfgname);

		ReleaseSysCache(cfgtup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_ts_config 行则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", cfgid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regconfigrecv			- 把外部二进制格式转为 regconfig
 */
Datum
regconfigrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regconfigsend			- 把 regconfig 转为二进制格式
 */
Datum
regconfigsend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}


/*
 * regdictionaryin		- 把 "tsdictionaryname" 转为 tsdictionary OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_ts_dict 条目
 */
Datum
regdictionaryin(PG_FUNCTION_ARGS)
{
	char	   *dict_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(dict_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regdictionary values must be OIDs in bootstrap mode");

	/*
	 * 常规情形：解析名称的各成分，看是否匹配当前
	 * 搜索路径中的 pg_ts_dict 条目。
	 */
	names = stringToQualifiedNameList(dict_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	result = get_ts_dict_oid(names, true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search dictionary \"%s\" does not exist",
						NameListToString(names))));

	PG_RETURN_OID(result);
}

/*
 * regdictionaryout		- 把 tsdictionary OID 转为 "tsdictionaryname"
 */
Datum
regdictionaryout(PG_FUNCTION_ARGS)
{
	Oid			dictid = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	dicttup;

	if (dictid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	dicttup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictid));

	if (HeapTupleIsValid(dicttup))
	{
		Form_pg_ts_dict dictform = (Form_pg_ts_dict) GETSTRUCT(dicttup);
		char	   *dictname = NameStr(dictform->dictname);
		char	   *nspname;

		/*
		 * regdictionaryin 能否找到它？若不能，则加限定
		 *
		 */
		if (TSDictionaryIsVisible(dictid))
			nspname = NULL;
		else
			nspname = get_namespace_name(dictform->dictnamespace);

		result = quote_qualified_identifier(nspname, dictname);

		ReleaseSysCache(dicttup);
	}
	else
	{
		/* 若 OID 不匹配任何 pg_ts_dict 行则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", dictid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regdictionaryrecv	- 把外部二进制格式转为 regdictionary
 */
Datum
regdictionaryrecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regdictionarysend	- 把 regdictionary 转为二进制格式
 */
Datum
regdictionarysend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}

/*
 * regrolein	- 把 "rolename" 转为角色 OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_authid 条目
 */
Datum
regrolein(PG_FUNCTION_ARGS)
{
	char	   *role_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(role_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regrole values must be OIDs in bootstrap mode");

	/* 常规情形：看名称是否匹配 pg_authid 条目。 */
	names = stringToQualifiedNameList(role_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	if (list_length(names) != 1)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_role_oid(strVal(linitial(names)), true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist",
						strVal(linitial(names)))));

	PG_RETURN_OID(result);
}

/*
 * to_regrole		- 把 "rolename" 转为角色 OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regrole(PG_FUNCTION_ARGS)
{
	char	   *role_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regrolein, role_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regroleout		- 把角色 OID 转为 "role_name"
 */
Datum
regroleout(PG_FUNCTION_ARGS)
{
	Oid			roleoid = PG_GETARG_OID(0);
	char	   *result;

	if (roleoid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	result = GetUserNameFromId(roleoid, true);

	if (result)
	{
		/* pstrdup 并非真正必需，但它避免了一个编译器警告 */
		result = pstrdup(quote_identifier(result));
	}
	else
	{
		/* 若 OID 不匹配任何角色则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", roleoid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regrolerecv - 把外部二进制格式转为 regrole
 */
Datum
regrolerecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regrolesend - 把 regrole 转为二进制格式
 */
Datum
regrolesend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}

/*
 * regnamespacein		- 把 "nspname" 转为 namespace OID
 *
 * 为了与输出例程对称，我们也接受数字 OID。
 *
 * "-" 表示未知（OID 0）。其余情形下输入必须
 * 匹配既有 pg_namespace 条目
 */
Datum
regnamespacein(PG_FUNCTION_ARGS)
{
	char	   *nsp_name_or_oid = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	Oid			result;
	List	   *names;

	/* 处理 "-" 或数字 OID */
	if (parseDashOrOid(nsp_name_or_oid, &result, escontext))
		PG_RETURN_OID(result);

	/* 这段余下部分在 bootstrap 下行不通 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "regnamespace values must be OIDs in bootstrap mode");

	/* 常规情形：看名称是否匹配 pg_namespace 条目。 */
	names = stringToQualifiedNameList(nsp_name_or_oid, escontext);
	if (names == NIL)
		PG_RETURN_NULL();

	if (list_length(names) != 1)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	result = get_namespace_oid(strVal(linitial(names)), true);

	if (!OidIsValid(result))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist",
						strVal(linitial(names)))));

	PG_RETURN_OID(result);
}

/*
 * to_regnamespace		- 把 "nspname" 转为 namespace OID
 *
 * 若找不到名称，则返回 NULL。
 */
Datum
to_regnamespace(PG_FUNCTION_ARGS)
{
	char	   *nsp_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum		result;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (!DirectInputFunctionCallSafe(regnamespacein, nsp_name,
									 InvalidOid, -1,
									 (Node *) &escontext,
									 &result))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * regnamespaceout		- 把 namespace OID 转为 "nsp_name"
 */
Datum
regnamespaceout(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	char	   *result;

	if (nspid == InvalidOid)
	{
		result = pstrdup("-");
		PG_RETURN_CSTRING(result);
	}

	result = get_namespace_name(nspid);

	if (result)
	{
		/* pstrdup 并非真正必需，但它避免了一个编译器警告 */
		result = pstrdup(quote_identifier(result));
	}
	else
	{
		/* 若 OID 不匹配任何命名空间则按数字返回 */
		result = (char *) palloc(NAMEDATALEN);
		snprintf(result, NAMEDATALEN, "%u", nspid);
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		regnamespacerecv	- 把外部二进制格式转为 regnamespace
 */
Datum
regnamespacerecv(PG_FUNCTION_ARGS)
{
	/* 与 oidrecv 完全相同，故共享代码 */
	return oidrecv(fcinfo);
}

/*
 *		regnamespacesend		- 把 regnamespace 转为二进制格式
 */
Datum
regnamespacesend(PG_FUNCTION_ARGS)
{
	/* 与 oidsend 完全相同，故共享代码 */
	return oidsend(fcinfo);
}

/*
 * text_regclass: text 转 regclass
 *
 * 本函数本可用 CoerceViaIO 替代，只是我们需要把
 * text 到 regclass 视为隐式转换，以支持旧式的 nextval()
 * 及相关函数。
 */
Datum
text_regclass(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Oid			result;
	RangeVar   *rv;

	rv = makeRangeVarFromNameList(textToQualifiedNameList(relname));

	/* 我们对这个关系可能连权限都没有；不要锁定它。 */
	result = RangeVarGetRelid(rv, NoLock, false);

	PG_RETURN_OID(result);
}


/*
 * 给定 C 字符串，解析成限定名列表。
 *
 * 若 escontext 是 ErrorSaveContext 节点，无效输入将
 * 上报到那里而不是抛出，并且我们返回 NIL。
 * （NIL 不可能作为成功返回值，空输入是一种错误。）
 */
List *
stringToQualifiedNameList(const char *string, Node *escontext)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	ListCell   *l;

	/* 需要输入串的可修改副本。 */
	rawname = pstrdup(string);

	if (!SplitIdentifierString(rawname, '.', &namelist))
		ereturn(escontext, NIL,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	if (namelist == NIL)
		ereturn(escontext, NIL,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);

		result = lappend(result, makeString(pstrdup(curname)));
	}

	pfree(rawname);
	list_free(namelist);

	return result;
}

/*****************************************************************************
 *	 支持例程													 *
 *****************************************************************************/

/*
 * 给定 C 串，看它是否全为数字（且非空）。
 * 若是，直接转换为 OID 并返回 true。
 * 若不是全数字则返回 false。
 *
 * 若 escontext 是 ErrorSaveContext 节点，oidin() 的
 * 错误会上报到那里而非抛出（但仍返回 true）。
 */
static bool
parseNumericOid(char *string, Oid *result, Node *escontext)
{
	if (string[0] >= '0' && string[0] <= '9' &&
		strspn(string, "0123456789") == strlen(string))
	{
		Datum		oid_datum;

		/* 无须在意 oidin() 在此是否失败。 */
		(void) DirectInputFunctionCallSafe(oidin, string,
										   InvalidOid, -1,
										   escontext,
										   &oid_datum);
		*result = DatumGetObjectId(oid_datum);
		return true;
	}

	/* 防止较笨的编译器发出未初始化变量警告。 */
	*result = InvalidOid;
	return false;
}

/*
 * 同上，但也接受 "-" 表示 0（InvalidOid）。
 */
static bool
parseDashOrOid(char *string, Oid *result, Node *escontext)
{
	/* '-'? */
	if (strcmp(string, "-") == 0)
	{
		*result = InvalidOid;
		return true;
	}

	/* 数字OID？ */
	return parseNumericOid(string, result, escontext);
}

/*
 * 给定 C 字符串，解析成限定的函数或算子名，
 * 其后为括起来的类型名列表。把类型名缩减
 * 为 OID 数组（返回到 *nargs 和 *argtypes；
 * argtypes 数组的长度应为 FUNC_MAX_ARGS）。函数或
 * 算子名以 String 组成的 List 返回到 *names。
 *
 * 若 allowNone 为 true，则接受 "NONE" 并将其作为 InvalidOid
 * （一元算子）
 *
 * 成功时返回 true，失败时返回 false（后者仅当
 * escontext 为 ErrorSaveContext 时）。
 */
static bool
parseNameAndArgTypes(const char *string, bool allowNone, List **names,
					 int *nargs, Oid *argtypes,
					 Node *escontext)
{
	char	   *rawname;
	char	   *ptr;
	char	   *ptr2;
	char	   *typename;
	bool		in_quote;
	bool		had_comma;
	int			paren_count;
	Oid			typeid;
	int32		typmod;

	/* 需要输入串的可修改副本。 */
	rawname = pstrdup(string);

	/* 扫描寻找预期的左括号；不得在引号内 */
	in_quote = false;
	for (ptr = rawname; *ptr; ptr++)
	{
		if (*ptr == '"')
			in_quote = !in_quote;
		else if (*ptr == '(' && !in_quote)
			break;
	}
	if (*ptr == '\0')
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a left parenthesis")));

	/* 分离出名称并把它解析成列表 */
	*ptr++ = '\0';
	*names = stringToQualifiedNameList(rawname, escontext);
	if (*names == NIL)
		return false;

	/* 检查结尾的右括号并将其移除 */
	ptr2 = ptr + strlen(ptr);
	while (--ptr2 > ptr)
	{
		if (!scanner_isspace(*ptr2))
			break;
	}
	if (*ptr2 != ')')
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("expected a right parenthesis")));

	*ptr2 = '\0';

	/* 把余下的字符串分离成逗号分隔的类型名 */
	*nargs = 0;
	had_comma = false;

	for (;;)
	{
		/* 允许前导空白 */
		while (scanner_isspace(*ptr))
			ptr++;
		if (*ptr == '\0')
		{
			/* 串已结束。除非前面有逗号否则没事 */
			if (had_comma)
				ereturn(escontext, false,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("expected a type name")));
			break;
		}
		typename = ptr;
		/* 找到类型名的末尾 —— 串尾或逗号 */
		/* ……但不能是引号/括号内的逗号 */
		in_quote = false;
		paren_count = 0;
		for (; *ptr; ptr++)
		{
			if (*ptr == '"')
				in_quote = !in_quote;
			else if (*ptr == ',' && !in_quote && paren_count == 0)
				break;
			else if (!in_quote)
			{
				switch (*ptr)
				{
					case '(':
					case '[':
						paren_count++;
						break;
					case ')':
					case ']':
						paren_count--;
						break;
				}
			}
		}
		if (in_quote || paren_count != 0)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("improper type name")));

		ptr2 = ptr;
		if (*ptr == ',')
		{
			had_comma = true;
			*ptr++ = '\0';
		}
		else
		{
			had_comma = false;
			Assert(*ptr == '\0');
		}
		/* 去掉结尾空白 */
		while (--ptr2 >= typename)
		{
			if (!scanner_isspace(*ptr2))
				break;
			*ptr2 = '\0';
		}

		if (allowNone && pg_strcasecmp(typename, "none") == 0)
		{
			/* NONE 的特殊情形 */
			typeid = InvalidOid;
			typmod = -1;
		}
		else
		{
			/* 用完整解析器来解析类型名 */
			if (!parseTypeString(typename, &typeid, &typmod, escontext))
				return false;
		}
		if (*nargs >= FUNC_MAX_ARGS)
			ereturn(escontext, false,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("too many arguments")));

		argtypes[*nargs] = typeid;
		(*nargs)++;
	}

	pfree(rawname);

	return true;
}
