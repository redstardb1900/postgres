/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  转换创建命令支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/conversioncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/conversioncmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * CREATE CONVERSION
 */
ObjectAddress
CreateConversionCommand(CreateConversionStmt *stmt)
{
	Oid			namespaceId;
	char	   *conversion_name;
	AclResult	aclresult;
	int			from_encoding;
	int			to_encoding;
	Oid			funcoid;
	const char *from_encoding_name = stmt->for_encoding_name;
	const char *to_encoding_name = stmt->to_encoding_name;
	List	   *func_name = stmt->func_name;
	static const Oid funcargs[] = {INT4OID, INT4OID, CSTRINGOID, INTERNALOID, INT4OID, BOOLOID};
	char		result[1];
	Datum		funcresult;

	/* 将名称列表转换为名称和命名空间 */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name,
													&conversion_name);

	/* 检查我们是否在目标命名空间中拥有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceId));

	/* 检查编码名称 */
	from_encoding = pg_char_to_encoding(from_encoding_name);
	if (from_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("source encoding \"%s\" does not exist",
						from_encoding_name)));

	to_encoding = pg_char_to_encoding(to_encoding_name);
	if (to_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("destination encoding \"%s\" does not exist",
						to_encoding_name)));

	/*
	 * 我们认为与 SQL_ASCII 之间进行的转换没有意义。（如果你
	 * 想改变这一点，请注意 pg_do_encoding_conversion() 及其
	 * 姊妹函数为任何源或目标编码是 SQL_ASCII 的转换都硬编码了
	 * 快速路径，因此为这种情况声明的编码转换函数
	 * 将永远不会被使用。）
	 */
	if (from_encoding == PG_SQL_ASCII || to_encoding == PG_SQL_ASCII)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion to or from \"SQL_ASCII\" is not supported")));

	/*
	 * 检查转换函数是否存在。函数名可以是
	 * 限定名称。
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid),
							 funcargs, false);

	/* 检查它是否返回 int4，否则很可能就是错误的函数 */
	if (get_func_rettype(funcoid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s must return type %s",
						NameListToString(func_name), "integer")));

	/* 检查我们是否拥有该函数的 EXECUTE 权限 */
	aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(func_name));

	/*
	 * 检查转换函数是否适合所请求的源编码和目标编码。
	 * 我们通过使用空字符串调用该函数来完成检查；
	 * 如果转换函数无法执行所请求的转换，
	 * 它应当抛出错误。
	 */
	funcresult = OidFunctionCall6(funcoid,
								  Int32GetDatum(from_encoding),
								  Int32GetDatum(to_encoding),
								  CStringGetDatum(""),
								  CStringGetDatum(result),
								  Int32GetDatum(0),
								  BoolGetDatum(false));

	/*
	 * 对于空输入，该函数应当返回 0。顺便也检查一下
	 * 这一点。
	 */
	if (DatumGetInt32(funcresult) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s returned incorrect result for empty input",
						NameListToString(func_name))));

	/*
	 * 一切似乎都没问题，继续执行（可能的失败是转换名称
	 * 重复）
	 */
	return ConversionCreate(conversion_name, namespaceId, GetUserId(),
							from_encoding, to_encoding, funcoid, stmt->def);
}
