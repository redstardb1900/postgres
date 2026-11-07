/*-------------------------------------------------------------------------
 *
 * parse_type.c
 *		解析器的类型操作处理   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_type.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static int32 typenameTypeMod(ParseState *pstate, const TypeName *typeName,
							 Type typ);


/*
 * LookupTypeName
 *		典型情况包装器。 
 */
Type
LookupTypeName(ParseState *pstate, const TypeName *typeName,
			   int32 *typmod_p, bool missing_ok)
{
	return LookupTypeNameExtended(pstate,
								  typeName, typmod_p, true, missing_ok);
}

/*
 * LookupTypeNameExtended
 *		给定 TypeName，查找其类型的 pg_type 系统缓存条目。     
 *		找不到该类型则返回 NULL；若找到，                  
 *		则计算 TypeName 结构体中表示的 typmod 值，             
 *		并存入 *typmod_p。
 *
 * 注意：成功时调用者用完类型元组后必须 ReleaseSysCache  
 *         
 *
 * 注意：直接调用者必须先检查 typisdefined，才能假定        
 * 该类型完全有效。多数代码应经由 typenameType             
 * 或 typenameTypeId。     
 *
 * 若调用者不关心 typmod 值，可将 typmod_p 传为 NULL，     
 * 但 typmod 修饰（若有）仍会被校验，                           
 * 但类型未找到的情形除外。注意若类型                      
 * 已找到但只是 shell 且带有 typmod 修饰，则会抛出错误 
 * --- 这是有意为之。      
 *
 * temp_ok 为 false 时忽略临时命名空间中的类型。调用者将 
 * 需按拟合优度标准自行判断 typeName 是类型还是别的     
 * 东西时传 false。若 typeName 总是表示类型                      
 * （或无所指）则传 true。       
 *
 * pstate 仅用于错误位置信息，可为 NULL。          
 */
Type
LookupTypeNameExtended(ParseState *pstate,
					   const TypeName *typeName, int32 *typmod_p,
					   bool temp_ok, bool missing_ok)
{
	Oid			typoid;
	HeapTuple	tup;
	int32		typmod;

	if (typeName->names == NIL)
	{
		/* 若为内部生成的 TypeName 则已有其 OID                  */
		typoid = typeName->typeOid;
	}
	else if (typeName->pct_type)
	{
		/* 处理对既有字段类型的 %TYPE 引用。      */
		RangeVar   *rel = makeRangeVar(NULL, NULL, typeName->location);
		char	   *field = NULL;
		Oid			relid;
		AttrNumber	attnum;

		/* 分解名称列表。     */
		switch (list_length(typeName->names))
		{
			case 1:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("improper %%TYPE reference (too few dotted names): %s",
								NameListToString(typeName->names)),
						 parser_errposition(pstate, typeName->location)));
				break;
			case 2:
				rel->relname = strVal(linitial(typeName->names));
				field = strVal(lsecond(typeName->names));
				break;
			case 3:
				rel->schemaname = strVal(linitial(typeName->names));
				rel->relname = strVal(lsecond(typeName->names));
				field = strVal(lthird(typeName->names));
				break;
			case 4:
				rel->catalogname = strVal(linitial(typeName->names));
				rel->schemaname = strVal(lsecond(typeName->names));
				rel->relname = strVal(lthird(typeName->names));
				field = strVal(lfourth(typeName->names));
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("improper %%TYPE reference (too many dotted names): %s",
								NameListToString(typeName->names)),
						 parser_errposition(pstate, typeName->location)));
				break;
		}

		/*
		 * 查找该字段。
		 *
		 * XXX：此处未加锁，存在并发 DDL 时可能失败。      
		 * 但加锁会带来性能开销，                           
		 * 且还需要进行权限检查。                  
		 */
		relid = RangeVarGetRelid(rel, NoLock, missing_ok);
		attnum = get_attnum(relid, field);
		if (attnum == InvalidAttrNumber)
		{
			if (missing_ok)
				typoid = InvalidOid;
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								field, rel->relname),
						 parser_errposition(pstate, typeName->location)));
		}
		else
		{
			typoid = get_atttype(relid, attnum);

			/* 此构造绝不应带有数组指示符。          */
			Assert(typeName->arrayBounds == NIL);

			/* 发出提示性通知（有意不设置 errposition） */
			ereport(NOTICE,
					(errmsg("type reference %s converted to %s",
							TypeNameToString(typeName),
							format_type_be(typoid))));
		}
	}
	else
	{
		/* 对类型名的普通引用。  */
		char	   *schemaname;
		char	   *typname;

		/* 分解名称列表。     */
		DeconstructQualifiedName(typeName->names, &schemaname, &typname);

		if (schemaname)
		{
			/* 仅在特定模式中查找  */
			Oid			namespaceId;
			ParseCallbackState pcbstate;

			setup_parser_errposition_callback(&pcbstate, pstate, typeName->location);

			namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
			if (OidIsValid(namespaceId))
				typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
										 PointerGetDatum(typname),
										 ObjectIdGetDatum(namespaceId));
			else
				typoid = InvalidOid;

			cancel_parser_errposition_callback(&pcbstate);
		}
		else
		{
			/* 非限定类型名，故沿搜索路径查找。 */
			typoid = TypenameGetTypidExtended(typname, temp_ok);
		}

		/* 若为数组引用，则改为返回数组类型。  */
		if (typeName->arrayBounds != NIL)
			typoid = get_array_type(typoid);
	}

	if (!OidIsValid(typoid))
	{
		if (typmod_p)
			*typmod_p = -1;
		return NULL;
	}

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));
	if (!HeapTupleIsValid(tup)) /* 不应发生。   */
		elog(ERROR, "cache lookup failed for type %u", typoid);

	typmod = typenameTypeMod(pstate, typeName, (Type) tup);

	if (typmod_p)
		*typmod_p = typmod;

	return (Type) tup;
}

/*
 * LookupTypeNameOid
 *		给定 TypeName，查找其类型的 pg_type 系统缓存条目。     
 *		找不到该类型则返回 InvalidOid；若找到则返回其 OID。  
 *		               
 *
 * 注意：本函数的直接调用者需了解所返回的类型 OID   
 * 可能对应 shell 类型。多数代码应经由                    
 * typenameTypeId。      
 *
 * pstate 仅用于错误位置信息，可为 NULL。          
 */
Oid
LookupTypeNameOid(ParseState *pstate, const TypeName *typeName, bool missing_ok)
{
	Oid			typoid;
	Type		tup;

	tup = LookupTypeName(pstate, typeName, NULL, missing_ok);
	if (tup == NULL)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist",
							TypeNameToString(typeName)),
					 parser_errposition(pstate, typeName->location)));

		return InvalidOid;
	}

	typoid = ((Form_pg_type) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);

	return typoid;
}

/*
 * typenameType - 给定 TypeName，返回 Type 结构与 typmod      
 *
 * 本函数等效于 LookupTypeName，区别在于找不到类型   
 * 或未定义时会报告合适的错误消息。                       
 * 因此调用者可假定结果是完全有效的类型。             
 */
Type
typenameType(ParseState *pstate, const TypeName *typeName, int32 *typmod_p)
{
	Type		tup;

	tup = LookupTypeName(pstate, typeName, typmod_p, false);
	if (tup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typeName)),
				 parser_errposition(pstate, typeName->location)));
	if (!((Form_pg_type) GETSTRUCT(tup))->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is only a shell",
						TypeNameToString(typeName)),
				 parser_errposition(pstate, typeName->location)));
	return tup;
}

/*
 * typenameTypeId - 给定 TypeName，返回类型 OID     
 *
 * 这与 typenameType 类似，但只交回类型 OID，             
 * 而非缓存条目。  
 */
Oid
typenameTypeId(ParseState *pstate, const TypeName *typeName)
{
	Oid			typoid;
	Type		tup;

	tup = typenameType(pstate, typeName, NULL);
	typoid = ((Form_pg_type) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);

	return typoid;
}

/*
 * typenameTypeIdAndMod - 给定 TypeName，返回类型 OID 与 typmod     
 *
 * 这与 typenameType 等效，但只交回类型 OID 与 typmod，     
 * 而非系统缓存条目。        
 */
void
typenameTypeIdAndMod(ParseState *pstate, const TypeName *typeName,
					 Oid *typeid_p, int32 *typmod_p)
{
	Type		tup;

	tup = typenameType(pstate, typeName, typmod_p);
	*typeid_p = ((Form_pg_type) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
}

/*
 * typenameTypeMod - 给定 TypeName，返回内部 typmod 值         
 *
 * 若 TypeName 含有对该数据类型非法的类型                     
 * 修饰符则抛错。     
 *
 * TypeName 所表示的实际类型 OID 必须已被查出来，        
 * 并以 "typ" 传入。            
 *
 * pstate 仅用于错误位置信息，可为 NULL。          
 */
static int32
typenameTypeMod(ParseState *pstate, const TypeName *typeName, Type typ)
{
	int32		result;
	Oid			typmodin;
	Datum	   *datums;
	int			n;
	ListCell   *l;
	ArrayType  *arrtypmod;
	ParseCallbackState pcbstate;

	/* 无 typmod 表达式时返回预指定的 typmod。 */
	if (typeName->typmods == NIL)
		return typeName->typemod;

	/*
	 * 否则该类型最好应接受 typmod。对 shell 类型情形我们  
	 * 会给出专门的错误消息，因为 shell 不可能有     
	 * typmodin 函数。
	 */
	if (!((Form_pg_type) GETSTRUCT(typ))->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type modifier cannot be specified for shell type \"%s\"",
						TypeNameToString(typeName)),
				 parser_errposition(pstate, typeName->location)));

	typmodin = ((Form_pg_type) GETSTRUCT(typ))->typmodin;

	if (typmodin == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type modifier is not allowed for type \"%s\"",
						TypeNameToString(typeName)),
				 parser_errposition(pstate, typeName->location)));

	/*
	 * 把原始文法输出表达式列表转换为 cstring 数组。       
	 * 目前允许简单数值常量、字符串字面量和            
	 * 标识符；此列表将来或可扩展。        
	 */
	datums = (Datum *) palloc(list_length(typeName->typmods) * sizeof(Datum));
	n = 0;
	foreach(l, typeName->typmods)
	{
		Node	   *tm = (Node *) lfirst(l);
		char	   *cstr = NULL;

		if (IsA(tm, A_Const))
		{
			A_Const    *ac = (A_Const *) tm;

			if (IsA(&ac->val, Integer))
			{
				cstr = psprintf("%ld", (long) intVal(&ac->val));
			}
			else if (IsA(&ac->val, Float))
			{
				/* 我们可以直接使用其字符串表示。       */
				cstr = ac->val.fval.fval;
			}
			else if (IsA(&ac->val, String))
			{
				/* 我们可以直接使用其字符串表示。       */
				cstr = strVal(&ac->val);
			}
		}
		else if (IsA(tm, ColumnRef))
		{
			ColumnRef  *cr = (ColumnRef *) tm;

			if (list_length(cr->fields) == 1 &&
				IsA(linitial(cr->fields), String))
				cstr = strVal(linitial(cr->fields));
		}
		if (!cstr)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type modifiers must be simple constants or identifiers"),
					 parser_errposition(pstate, typeName->location)));
		datums[n++] = CStringGetDatum(cstr);
	}

	arrtypmod = construct_array_builtin(datums, n, CSTRINGOID);

	/* 若类型的 typmodin 函数失败则安排报告位置。   */
	setup_parser_errposition_callback(&pcbstate, pstate, typeName->location);

	result = DatumGetInt32(OidFunctionCall1(typmodin,
											PointerGetDatum(arrtypmod)));

	cancel_parser_errposition_callback(&pcbstate);

	pfree(datums);
	pfree(arrtypmod);

	return result;
}

/*
 * appendTypeNameToBuffer
 *		把表示 TypeName 名称的字符串附加到 StringInfo。        
 *		是 TypeNameToString 与 TypeNameListToString 的共享主体。     
 *
 * 注意：必须能处理不描述任何实际类型的 TypeName，   
 * 其主要用于报告查找错误。          
 */
static void
appendTypeNameToBuffer(const TypeName *typeName, StringInfo string)
{
	if (typeName->names != NIL)
	{
		/* 输出可能带限定的名称。  */
		ListCell   *l;

		foreach(l, typeName->names)
		{
			if (l != list_head(typeName->names))
				appendStringInfoChar(string, '.');
			appendStringInfoString(string, strVal(lfirst(l)));
		}
	}
	else
	{
		/* 查找内部指定的类型。    */
		appendStringInfoString(string, format_type_be(typeName->typeOid));
	}

	/*
	 * 按需加修饰，但仅限以下函数考虑的字段：  
	 * LookupTypeName
	 */
	if (typeName->pct_type)
		appendStringInfoString(string, "%TYPE");

	if (typeName->arrayBounds != NIL)
		appendStringInfoString(string, "[]");
}

/*
 * TypeNameToString
 *		产出一个表示 TypeName 名称的字符串。    
 *
 * 注意：必须能处理不描述任何实际类型的 TypeName，   
 * 其主要用于报告查找错误。          
 */
char *
TypeNameToString(const TypeName *typeName)
{
	StringInfoData string;

	initStringInfo(&string);
	appendTypeNameToBuffer(typeName, &string);
	return string.data;
}

/*
 * TypeNameListToString
 *		产出一个表示 TypeName 列表各名称的字符串。      
 */
char *
TypeNameListToString(List *typenames)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);
	foreach(l, typenames)
	{
		TypeName   *typeName = lfirst_node(TypeName, l);

		if (l != list_head(typenames))
			appendStringInfoChar(&string, ',');
		appendTypeNameToBuffer(typeName, &string);
	}
	return string.data;
}

/*
 * LookupCollation
 *
 * 按名称查找排序规则并返回 OID，支持错误定位。       
 */
Oid
LookupCollation(ParseState *pstate, List *collnames, int location)
{
	Oid			colloid;
	ParseCallbackState pcbstate;

	if (pstate)
		setup_parser_errposition_callback(&pcbstate, pstate, location);

	colloid = get_collation_oid(collnames, false);

	if (pstate)
		cancel_parser_errposition_callback(&pcbstate);

	return colloid;
}

/*
 * GetColumnDefCollation
 *
 * 给定 ColumnDef 节点和先前确定的列类型 OID，取正在
 * 定义的列要用的排序规则。                         
 *
 * pstate 仅用于错误定位目的，可为 NULL。              
 */
Oid
GetColumnDefCollation(ParseState *pstate, ColumnDef *coldef, Oid typeOid)
{
	Oid			result;
	Oid			typcollation = get_typcollation(typeOid);
	int			location = coldef->location;

	if (coldef->collClause)
	{
		/* 有原始 COLLATE 子句，故查找排序规则。    */
		location = coldef->collClause->location;
		result = LookupCollation(pstate, coldef->collClause->collname,
								 location);
	}
	else if (OidIsValid(coldef->collOid))
	{
		/* 用预定的排序规则。        */
		result = coldef->collOid;
	}
	else
	{
		/* 有则使用类型默认排序规则。 */
		result = typcollation;
	}

	/* 若 COLLATE 用在不可排序类型上则报错。    */
	if (OidIsValid(result) && !OidIsValid(typcollation))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(typeOid)),
				 parser_errposition(pstate, location)));

	return result;
}

/* 给定类型 OID，返回 Type 结构。 */
/* 注意：调用者用完类型元组后必须 ReleaseSysCache。 */
Type
typeidType(Oid id)
{
	HeapTuple	tup;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(id));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", id);
	return (Type) tup;
}

/* 给定类型（Type 结构），返回其 OID。 */
Oid
typeTypeId(Type tp)
{
	if (tp == NULL)				/* 可能无用。  */
		elog(ERROR, "typeTypeId() called with NULL type struct");
	return ((Form_pg_type) GETSTRUCT(tp))->oid;
}

/* 给定类型（Type 结构），返回类型长度。  */
int16
typeLen(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typlen;
}

/* 给定类型（Type 结构），返回其 'byval' 属性  */
bool
typeByVal(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	return typ->typbyval;
}

/* 给定类型（Type 结构），返回类型名。  */
char *
typeTypeName(Type t)
{
	Form_pg_type typ;

	typ = (Form_pg_type) GETSTRUCT(t);
	/* 此处 pstrdup，因结果可能要比缓存条目存活更久。  */
	return pstrdup(NameStr(typ->typname));
}

/* 给定类型（Type 结构），返回其 'typrelid' 属性  */
Oid
typeTypeRelid(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);
	return typtup->typrelid;
}

/* 给定类型（Type 结构），返回其 'typcollation' 属性  */
Oid
typeTypeCollation(Type typ)
{
	Form_pg_type typtup;

	typtup = (Form_pg_type) GETSTRUCT(typ);
	return typtup->typcollation;
}

/*
 * 给定类型结构和字符串，返回其内部表示形式。         
 * 其中 "string" 可为 NULL，表示要转换 NULL 值                   
 * 此时若输入函数拒绝 NULL 则会失败。                     
 */
Datum
stringTypeDatum(Type tp, char *string, int32 atttypmod)
{
	Form_pg_type typform = (Form_pg_type) GETSTRUCT(tp);
	Oid			typinput = typform->typinput;
	Oid			typioparam = getTypeIOParam(tp);

	return OidInputFunctionCall(typinput, string, typioparam, atttypmod);
}

/*
 * 给定类型 OID，返回其 typrelid（关联关系），若有。      
 * 若类型不是组合类型则返回 InvalidOid。 
 */
Oid
typeidTypeRelid(Oid type_id)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Oid			result;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type_id);
	type = (Form_pg_type) GETSTRUCT(typeTuple);
	result = type->typrelid;
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * 给定类型 OID，返回其 typrelid（关联关系），若有。      
 * 若类型不是组合类型或其上的域则返回 InvalidOid。       
 * 这与 typeidTypeRelid(getBaseType(type_id)) 相同，但更快。    
 */
Oid
typeOrDomainTypeRelid(Oid type_id)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Oid			result;

	for (;;)
	{
		typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "cache lookup failed for type %u", type_id);
		type = (Form_pg_type) GETSTRUCT(typeTuple);
		if (type->typtype != TYPTYPE_DOMAIN)
		{
			/* 不是域，域的穿透查找到此为止。 */
			break;
		}
		/* 是域，因此改为检查其基类型。       */
		type_id = type->typbasetype;
		ReleaseSysCache(typeTuple);
	}
	result = type->typrelid;
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * parseTypeString() 解析失败时的错误上下文回调        
 */
static void
pts_error_callback(void *arg)
{
	const char *str = (const char *) arg;

	errcontext("invalid type name \"%s\"", str);
}

/*
 * 给定一个应为 SQL 兼容类型声明的字符串，                
 * 例如 "int4"、"integer" 或 "character varying(32)"，     
 * 解析字符串并返回 TypeName 结果。    
 *
 * 若字符串无法解析为类型，则抛出错误；       
 * 除非 escontext 是 ErrorSaveContext 节点，此时我们可    
 * 填充它并返回 NULL。但注意 ErrorSaveContext 选项          
 * 目前多半只是愿景：由主文法而非此处           
 * 检测到的错误仍会被抛出。            
 */
TypeName *
typeStringToTypeName(const char *str, Node *escontext)
{
	List	   *raw_parsetree_list;
	TypeName   *typeName;
	ErrorContextCallback ptserrcontext;

	/* 确保对空输入给出有用的错误。     */
	if (strspn(str, " \t\n\r\f") == strlen(str))
		goto fail;

	/*
	 * 为解析期间的 ereport() 设置错误回溯支持。       
	 */
	ptserrcontext.callback = pts_error_callback;
	ptserrcontext.arg = unconstify(char *, str);
	ptserrcontext.previous = error_context_stack;
	error_context_stack = &ptserrcontext;

	raw_parsetree_list = raw_parser(str, RAW_PARSE_TYPE_NAME);

	error_context_stack = ptserrcontext.previous;

	/* 我们应正好取回一个 TypeName 节点。*/
	Assert(list_length(raw_parsetree_list) == 1);
	typeName = linitial_node(TypeName, raw_parsetree_list);

	/* 文法允许 TypeName 中有 SETOF，但此处不接受。         */
	if (typeName->setof)
		goto fail;

	return typeName;

fail:
	ereturn(escontext, NULL,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid type name \"%s\"", str)));
}

/*
 * 给定一个应为 SQL 兼容类型声明的字符串，                
 * 例如 "int4"、"integer" 或 "character varying(32)"，     
 * 把字符串转换为类型 OID 和类型修饰符。     
 *
 * 若 escontext 是 ErrorSaveContext 节点，则错误通过          
 * 填充 escontext 并返回 false 来报告，而非抛出。    
 */
bool
parseTypeString(const char *str, Oid *typeid_p, int32 *typmod_p,
				Node *escontext)
{
	TypeName   *typeName;
	Type		tup;

	typeName = typeStringToTypeName(str, escontext);
	if (typeName == NULL)
		return false;

	tup = LookupTypeName(NULL, typeName, typmod_p,
						 (escontext && IsA(escontext, ErrorSaveContext)));
	if (tup == NULL)
	{
		ereturn(escontext, false,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typeName))));
	}
	else
	{
		Form_pg_type typ = (Form_pg_type) GETSTRUCT(tup);

		if (!typ->typisdefined)
		{
			ReleaseSysCache(tup);
			ereturn(escontext, false,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							TypeNameToString(typeName))));
		}
		*typeid_p = typ->oid;
		ReleaseSysCache(tup);
	}

	return true;
}
