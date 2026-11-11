/*-------------------------------------------------------------------------
 *
 * tsearchcmds.c
 *
 *	  文本搜索操纵命令的例程         
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tsearchcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "common/string.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* getTokenTypes() 返回的列表的单个条目   */
typedef struct
{
	int			num;			/* 词元类型编号 */
	char	   *name;			/* 词元类型名称 */
} TSTokenTypeItem;

static void MakeConfigurationMapping(AlterTSConfigurationStmt *stmt,
									 HeapTuple tup, Relation relMap);
static void DropConfigurationMapping(AlterTSConfigurationStmt *stmt,
									 HeapTuple tup, Relation relMap);
static DefElem *buildDefItem(const char *name, const char *val,
							 bool was_quoted);


/* --------------------- TS 解析器命令 ------------------------ */

/*
 * 查找解析器支持函数并返回其 OID（作为 Datum）
 *
 * attnum 是函数将要进入的 pg_ts_parser 列
               */
static Datum
get_ts_parser_func(DefElem *defel, int attnum)
{
	List	   *funcName = defGetQualifiedName(defel);
	Oid			typeId[3];
	Oid			retTypeId;
	int			nargs;
	Oid			procOid;

	retTypeId = INTERNALOID;	/* 对大多数情况正确 */
	typeId[0] = INTERNALOID;
	switch (attnum)
	{
		case Anum_pg_ts_parser_prsstart:
			nargs = 2;
			typeId[1] = INT4OID;
			break;
		case Anum_pg_ts_parser_prstoken:
			nargs = 3;
			typeId[1] = INTERNALOID;
			typeId[2] = INTERNALOID;
			break;
		case Anum_pg_ts_parser_prsend:
			nargs = 1;
			retTypeId = VOIDOID;
			break;
		case Anum_pg_ts_parser_prsheadline:
			nargs = 3;
			typeId[1] = INTERNALOID;
			typeId[2] = TSQUERYOID;
			break;
		case Anum_pg_ts_parser_prslextype:
			nargs = 1;

			/*
			 * 注意：由于 lextype 方法返回类型 internal，出于安全原因，
			 * 它必须具有 internal 类型的参数。该参数实际上未被使用，
			 * 仅作为零传递。
			         */
			break;
		default:
			/* 不应该到达这里 */
			elog(ERROR, "unrecognized attribute for text search parser: %d",
				 attnum);
			nargs = 0;			/* 让编译器安静  */
	}

	procOid = LookupFuncName(funcName, nargs, typeId, false);
	if (get_func_rettype(procOid) != retTypeId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s should return type %s",
						func_signature_string(funcName, nargs, NIL, typeId),
						format_type_be(retTypeId))));

	return ObjectIdGetDatum(procOid);
}

/*
 * 为新 pg_ts_parser 条目创建 pg_depend 条目
 *
 * 返回值是所述新条目的地址。
          */
static ObjectAddress
makeParserDependencies(HeapTuple tuple)
{
	Form_pg_ts_parser prs = (Form_pg_ts_parser) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	ObjectAddressSet(myself, TSParserRelationId, prs->oid);

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, false);

	addrs = new_object_addresses();

	/* 依赖于命名空间   */
	ObjectAddressSet(referenced, NamespaceRelationId, prs->prsnamespace);
	add_exact_object_address(&referenced, addrs);

	/* 依赖于函数           */
	ObjectAddressSet(referenced, ProcedureRelationId, prs->prsstart);
	add_exact_object_address(&referenced, addrs);

	referenced.objectId = prs->prstoken;
	add_exact_object_address(&referenced, addrs);

	referenced.objectId = prs->prsend;
	add_exact_object_address(&referenced, addrs);

	referenced.objectId = prs->prslextype;
	add_exact_object_address(&referenced, addrs);

	if (OidIsValid(prs->prsheadline))
	{
		referenced.objectId = prs->prsheadline;
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	return myself;
}

/*
 * CREATE TEXT SEARCH PARSER
 */
ObjectAddress
DefineTSParser(List *names, List *parameters)
{
	char	   *prsname;
	ListCell   *pl;
	Relation	prsRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_parser];
	bool		nulls[Natts_pg_ts_parser];
	NameData	pname;
	Oid			prsOid;
	Oid			namespaceoid;
	ObjectAddress address;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create text search parsers")));

	prsRel = table_open(TSParserRelationId, RowExclusiveLock);

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &prsname);

	/* 用名称/命名空间初始化元组字段 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	prsOid = GetNewOidWithIndex(prsRel, TSParserOidIndexId,
								Anum_pg_ts_parser_oid);
	values[Anum_pg_ts_parser_oid - 1] = ObjectIdGetDatum(prsOid);
	namestrcpy(&pname, prsname);
	values[Anum_pg_ts_parser_prsname - 1] = NameGetDatum(&pname);
	values[Anum_pg_ts_parser_prsnamespace - 1] = ObjectIdGetDatum(namespaceoid);

	/*
	 * 遍历定义列表并提取所需信息。
	                         */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "start") == 0)
		{
			values[Anum_pg_ts_parser_prsstart - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsstart);
		}
		else if (strcmp(defel->defname, "gettoken") == 0)
		{
			values[Anum_pg_ts_parser_prstoken - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prstoken);
		}
		else if (strcmp(defel->defname, "end") == 0)
		{
			values[Anum_pg_ts_parser_prsend - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsend);
		}
		else if (strcmp(defel->defname, "headline") == 0)
		{
			values[Anum_pg_ts_parser_prsheadline - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsheadline);
		}
		else if (strcmp(defel->defname, "lextypes") == 0)
		{
			values[Anum_pg_ts_parser_prslextype - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prslextype);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("text search parser parameter \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * 验证
	     */
	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsstart - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser start method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prstoken - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser gettoken method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsend - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser end method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prslextype - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser lextypes method is required")));

	/*
	 * 看起来不错，插入
	 */
	tup = heap_form_tuple(prsRel->rd_att, values, nulls);

	CatalogTupleInsert(prsRel, tup);

	address = makeParserDependencies(tup);

	/* 新文本搜索解析器的创建后钩子    */
	InvokeObjectPostCreateHook(TSParserRelationId, prsOid, 0);

	heap_freetuple(tup);

	table_close(prsRel, RowExclusiveLock);

	return address;
}

/* ---------------------- TS 词典命令 -----------------------       */

/*
 * 为新 pg_ts_dict 条目创建 pg_depend 条目
 *
 * 返回值是新条目的地址
             */
static ObjectAddress
makeDictionaryDependencies(HeapTuple tuple)
{
	Form_pg_ts_dict dict = (Form_pg_ts_dict) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	ObjectAddressSet(myself, TSDictionaryRelationId, dict->oid);

	/* 依赖于所有者  */
	recordDependencyOnOwner(myself.classId, myself.objectId, dict->dictowner);

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, false);

	addrs = new_object_addresses();

	/* 依赖于命名空间   */
	ObjectAddressSet(referenced, NamespaceRelationId, dict->dictnamespace);
	add_exact_object_address(&referenced, addrs);

	/* 依赖于模板        */
	ObjectAddressSet(referenced, TSTemplateRelationId, dict->dicttemplate);
	add_exact_object_address(&referenced, addrs);

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	return myself;
}

/*
 * 验证模板的 init 方法是否接受提议的选项列表
        */
static void
verify_dictoptions(Oid tmplId, List *dictoptions)
{
	HeapTuple	tup;
	Form_pg_ts_template tform;
	Oid			initmethod;

	/*
	 * 在独立后端中运行时抑制此测试。这是一个 hack，允许 initdb
	 * 创建在 template1 编码中可能实际上不可用的预构建词典
	 * （由于使用无法转换为 template1 编码的外部文件）。
	 * 我们无论如何都要创建它们，因为它们可能在其他数据库中
	 * 以后可用。
	                              */
	if (!IsUnderPostmaster)
		return;

	tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
	if (!HeapTupleIsValid(tup)) /* 不应该发生   */
		elog(ERROR, "cache lookup failed for text search template %u",
			 tmplId);
	tform = (Form_pg_ts_template) GETSTRUCT(tup);

	initmethod = tform->tmplinit;

	if (!OidIsValid(initmethod))
	{
		/* 如果没有 init 方法，则不允许任何选项 */
		if (dictoptions)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("text search template \"%s\" does not accept options",
							NameStr(tform->tmplname))));
	}
	else
	{
		/*
		 * 复制选项，以防 init 方法认为它可以
		 * 随意修改它们...
		       */
		dictoptions = copyObject(dictoptions);

		/*
		 * 调用 init 方法并查看它是否报错。我们不担心内存泄漏，
		 * 因为我们的命令很快就要结束了。
		          */
		(void) OidFunctionCall1(initmethod, PointerGetDatum(dictoptions));
	}

	ReleaseSysCache(tup);
}

/*
 * CREATE TEXT SEARCH DICTIONARY
 */
ObjectAddress
DefineTSDictionary(List *names, List *parameters)
{
	ListCell   *pl;
	Relation	dictRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_dict];
	bool		nulls[Natts_pg_ts_dict];
	NameData	dname;
	Oid			templId = InvalidOid;
	List	   *dictoptions = NIL;
	Oid			dictOid;
	Oid			namespaceoid;
	AclResult	aclresult;
	char	   *dictname;
	ObjectAddress address;

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &dictname);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/*
	 * 遍历定义列表并提取所需信息。
	                         */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "template") == 0)
		{
			templId = get_ts_template_oid(defGetQualifiedName(defel), false);
		}
		else
		{
			/* 假设它是词典本身的选项               */
			dictoptions = lappend(dictoptions, defel);
		}
	}

	/*
	 * 验证
	     */
	if (!OidIsValid(templId))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search template is required")));

	verify_dictoptions(templId, dictoptions);


	dictRel = table_open(TSDictionaryRelationId, RowExclusiveLock);

	/*
	 * 看起来不错，插入
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	dictOid = GetNewOidWithIndex(dictRel, TSDictionaryOidIndexId,
								 Anum_pg_ts_dict_oid);
	values[Anum_pg_ts_dict_oid - 1] = ObjectIdGetDatum(dictOid);
	namestrcpy(&dname, dictname);
	values[Anum_pg_ts_dict_dictname - 1] = NameGetDatum(&dname);
	values[Anum_pg_ts_dict_dictnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_ts_dict_dictowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_ts_dict_dicttemplate - 1] = ObjectIdGetDatum(templId);
	if (dictoptions)
		values[Anum_pg_ts_dict_dictinitoption - 1] =
			PointerGetDatum(serialize_deflist(dictoptions));
	else
		nulls[Anum_pg_ts_dict_dictinitoption - 1] = true;

	tup = heap_form_tuple(dictRel->rd_att, values, nulls);

	CatalogTupleInsert(dictRel, tup);

	address = makeDictionaryDependencies(tup);

	/* 新文本搜索词典的创建后钩子           */
	InvokeObjectPostCreateHook(TSDictionaryRelationId, dictOid, 0);

	heap_freetuple(tup);

	table_close(dictRel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TEXT SEARCH DICTIONARY
 */
ObjectAddress
AlterTSDictionary(AlterTSDictionaryStmt *stmt)
{
	HeapTuple	tup,
				newtup;
	Relation	rel;
	Oid			dictId;
	ListCell   *pl;
	List	   *dictoptions;
	Datum		opt;
	bool		isnull;
	Datum		repl_val[Natts_pg_ts_dict];
	bool		repl_null[Natts_pg_ts_dict];
	bool		repl_repl[Natts_pg_ts_dict];
	ObjectAddress address;

	dictId = get_ts_dict_oid(stmt->dictname, false);

	rel = table_open(TSDictionaryRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);

	/* 必须是所有者 */
	if (!object_ownercheck(TSDictionaryRelationId, dictId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TSDICTIONARY,
					   NameListToString(stmt->dictname));

	/* 反序列化现有的选项集          */
	opt = SysCacheGetAttr(TSDICTOID, tup,
						  Anum_pg_ts_dict_dictinitoption,
						  &isnull);
	if (isnull)
		dictoptions = NIL;
	else
		dictoptions = deserialize_deflist(opt);

	/*
	 * 根据指定的更改修改选项列表
	          */
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		ListCell   *cell;

		/*
		 * 删除任何匹配项...
		 */
		foreach(cell, dictoptions)
		{
			DefElem    *oldel = (DefElem *) lfirst(cell);

			if (strcmp(oldel->defname, defel->defname) == 0)
				dictoptions = foreach_delete_current(dictoptions, cell);
		}

		/*
		 * 如果有新值则添加
		          */
		if (defel->arg)
			dictoptions = lappend(dictoptions, defel);
	}

	/*
	 * 验证
	   */
	verify_dictoptions(((Form_pg_ts_dict) GETSTRUCT(tup))->dicttemplate,
					   dictoptions);

	/*
	 * 看起来不错，更新
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (dictoptions)
		repl_val[Anum_pg_ts_dict_dictinitoption - 1] =
			PointerGetDatum(serialize_deflist(dictoptions));
	else
		repl_null[Anum_pg_ts_dict_dictinitoption - 1] = true;
	repl_repl[Anum_pg_ts_dict_dictinitoption - 1] = true;

	newtup = heap_modify_tuple(tup, RelationGetDescr(rel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &newtup->t_self, newtup);

	InvokeObjectPostAlterHook(TSDictionaryRelationId, dictId, 0);

	ObjectAddressSet(address, TSDictionaryRelationId, dictId);

	/*
	 * 注意：因为我们只支持修改选项，不支持模板，所以无需更新
	 * 依赖关系。如果选项曾经引用数据库内部对象，则可能需要改变。
 *
	                                */

	heap_freetuple(newtup);
	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/* ---------------------- TS 模板命令 -----------------------     */

/*
 * 查找模板支持函数并返回其 OID（作为 Datum）
 *
 * attnum 是函数将要进入的 pg_ts_template 列
                    */
static Datum
get_ts_template_func(DefElem *defel, int attnum)
{
	List	   *funcName = defGetQualifiedName(defel);
	Oid			typeId[4];
	Oid			retTypeId;
	int			nargs;
	Oid			procOid;

	retTypeId = INTERNALOID;
	typeId[0] = INTERNALOID;
	typeId[1] = INTERNALOID;
	typeId[2] = INTERNALOID;
	typeId[3] = INTERNALOID;
	switch (attnum)
	{
		case Anum_pg_ts_template_tmplinit:
			nargs = 1;
			break;
		case Anum_pg_ts_template_tmpllexize:
			nargs = 4;
			break;
		default:
			/* 不应该到达这里 */
			elog(ERROR, "unrecognized attribute for text search template: %d",
				 attnum);
			nargs = 0;			/* 让编译器安静  */
	}

	procOid = LookupFuncName(funcName, nargs, typeId, false);
	if (get_func_rettype(procOid) != retTypeId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s should return type %s",
						func_signature_string(funcName, nargs, NIL, typeId),
						format_type_be(retTypeId))));

	return ObjectIdGetDatum(procOid);
}

/*
 * 为新 pg_ts_template 条目创建 pg_depend 条目
   */
static ObjectAddress
makeTSTemplateDependencies(HeapTuple tuple)
{
	Form_pg_ts_template tmpl = (Form_pg_ts_template) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	ObjectAddressSet(myself, TSTemplateRelationId, tmpl->oid);

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, false);

	addrs = new_object_addresses();

	/* 依赖于命名空间   */
	ObjectAddressSet(referenced, NamespaceRelationId, tmpl->tmplnamespace);
	add_exact_object_address(&referenced, addrs);

	/* 依赖于函数           */
	ObjectAddressSet(referenced, ProcedureRelationId, tmpl->tmpllexize);
	add_exact_object_address(&referenced, addrs);

	if (OidIsValid(tmpl->tmplinit))
	{
		referenced.objectId = tmpl->tmplinit;
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	return myself;
}

/*
 * CREATE TEXT SEARCH TEMPLATE
 */
ObjectAddress
DefineTSTemplate(List *names, List *parameters)
{
	ListCell   *pl;
	Relation	tmplRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_template];
	bool		nulls[Natts_pg_ts_template];
	NameData	dname;
	int			i;
	Oid			tmplOid;
	Oid			namespaceoid;
	char	   *tmplname;
	ObjectAddress address;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create text search templates")));

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &tmplname);

	tmplRel = table_open(TSTemplateRelationId, RowExclusiveLock);

	for (i = 0; i < Natts_pg_ts_template; i++)
	{
		nulls[i] = false;
		values[i] = ObjectIdGetDatum(InvalidOid);
	}

	tmplOid = GetNewOidWithIndex(tmplRel, TSTemplateOidIndexId,
								 Anum_pg_ts_dict_oid);
	values[Anum_pg_ts_template_oid - 1] = ObjectIdGetDatum(tmplOid);
	namestrcpy(&dname, tmplname);
	values[Anum_pg_ts_template_tmplname - 1] = NameGetDatum(&dname);
	values[Anum_pg_ts_template_tmplnamespace - 1] = ObjectIdGetDatum(namespaceoid);

	/*
	 * 遍历定义列表并提取所需信息。
	                         */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "init") == 0)
		{
			values[Anum_pg_ts_template_tmplinit - 1] =
				get_ts_template_func(defel, Anum_pg_ts_template_tmplinit);
			nulls[Anum_pg_ts_template_tmplinit - 1] = false;
		}
		else if (strcmp(defel->defname, "lexize") == 0)
		{
			values[Anum_pg_ts_template_tmpllexize - 1] =
				get_ts_template_func(defel, Anum_pg_ts_template_tmpllexize);
			nulls[Anum_pg_ts_template_tmpllexize - 1] = false;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("text search template parameter \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * 验证
	     */
	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_template_tmpllexize - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search template lexize method is required")));

	/*
	 * 看起来不错，插入
	 */
	tup = heap_form_tuple(tmplRel->rd_att, values, nulls);

	CatalogTupleInsert(tmplRel, tup);

	address = makeTSTemplateDependencies(tup);

	/* 新文本搜索模板的创建后钩子         */
	InvokeObjectPostCreateHook(TSTemplateRelationId, tmplOid, 0);

	heap_freetuple(tup);

	table_close(tmplRel, RowExclusiveLock);

	return address;
}

/* ---------------------- TS 配置命令 -----------------------          */

/*
 * 查找配置的系统缓存元组。
 * 如果没有这样的配置，返回 NULL。
 */
static HeapTuple
GetTSConfigTuple(List *names)
{
	HeapTuple	tup;
	Oid			cfgId;

	cfgId = get_ts_config_oid(names, true);
	if (!OidIsValid(cfgId))
		return NULL;

	tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));

	if (!HeapTupleIsValid(tup)) /* 不应该发生   */
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgId);

	return tup;
}

/*
 * 为新或更新的 pg_ts_config 条目创建 pg_depend 条目
 *
 * 如果配置可能有任何配置映射条目，则传递打开的 pg_ts_config_map 关系。
 *
      */
static ObjectAddress
makeConfigurationDependencies(HeapTuple tuple, bool removeOld,
							  Relation mapRel)
{
	Form_pg_ts_config cfg = (Form_pg_ts_config) GETSTRUCT(tuple);
	ObjectAddresses *addrs;
	ObjectAddress myself,
				referenced;

	myself.classId = TSConfigRelationId;
	myself.objectId = cfg->oid;
	myself.objectSubId = 0;

	/* 对于 ALTER 情况，首先刷新旧的依赖关系，扩展依赖除外 */
	if (removeOld)
	{
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);
		deleteSharedDependencyRecordsFor(myself.classId, myself.objectId, 0);
	}

	/*
	 * 我们使用 ObjectAddresses 列表来删除配置映射信息中可能的重复
	 * 依赖关系。pg_ts_config 项目不应该是重复的，但不妨将它们全部
	 * 合并到一个调用中。
	    */
	addrs = new_object_addresses();

	/* 依赖于命名空间   */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = cfg->cfgnamespace;
	referenced.objectSubId = 0;
	add_exact_object_address(&referenced, addrs);

	/* 依赖于所有者  */
	recordDependencyOnOwner(myself.classId, myself.objectId, cfg->cfgowner);

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, removeOld);

	/* 依赖于解析器   */
	referenced.classId = TSParserRelationId;
	referenced.objectId = cfg->cfgparser;
	referenced.objectSubId = 0;
	add_exact_object_address(&referenced, addrs);

	/* 依赖于配置映射中列出的词典           */
	if (mapRel)
	{
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	maptup;

		/* CCI 以确保我们可以看到调用者更改的效果 */
		CommandCounterIncrement();

		ScanKeyInit(&skey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(myself.objectId));

		scan = systable_beginscan(mapRel, TSConfigMapIndexId, true,
								  NULL, 1, &skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);

			referenced.classId = TSDictionaryRelationId;
			referenced.objectId = cfgmap->mapdict;
			referenced.objectSubId = 0;
			add_exact_object_address(&referenced, addrs);
		}

		systable_endscan(scan);
	}

	/* 记录它们（这包括去重）                */
	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);

	free_object_addresses(addrs);

	return myself;
}

/*
 * CREATE TEXT SEARCH CONFIGURATION
 */
ObjectAddress
DefineTSConfiguration(List *names, List *parameters, ObjectAddress *copied)
{
	Relation	cfgRel;
	Relation	mapRel = NULL;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_config];
	bool		nulls[Natts_pg_ts_config];
	AclResult	aclresult;
	Oid			namespaceoid;
	char	   *cfgname;
	NameData	cname;
	Oid			sourceOid = InvalidOid;
	Oid			prsOid = InvalidOid;
	Oid			cfgOid;
	ListCell   *pl;
	ObjectAddress address;

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &cfgname);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/*
	 * 遍历定义列表并提取所需信息。
	                         */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "parser") == 0)
			prsOid = get_ts_parser_oid(defGetQualifiedName(defel), false);
		else if (strcmp(defel->defname, "copy") == 0)
			sourceOid = get_ts_config_oid(defGetQualifiedName(defel), false);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("text search configuration parameter \"%s\" not recognized",
							defel->defname)));
	}

	if (OidIsValid(sourceOid) && OidIsValid(prsOid))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify both PARSER and COPY options")));

	/* 使复制的 tsconfig 对调用者可用  */
	if (copied && OidIsValid(sourceOid))
	{
		ObjectAddressSet(*copied,
						 TSConfigRelationId,
						 sourceOid);
	}

	/*
	 * 如果给定，查找源配置。
	 */
	if (OidIsValid(sourceOid))
	{
		Form_pg_ts_config cfg;

		tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(sourceOid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 sourceOid);

		cfg = (Form_pg_ts_config) GETSTRUCT(tup);

		/* 使用源的解析器 */
		prsOid = cfg->cfgparser;

		ReleaseSysCache(tup);
	}

	/*
	 * 验证
	     */
	if (!OidIsValid(prsOid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser is required")));

	cfgRel = table_open(TSConfigRelationId, RowExclusiveLock);

	/*
	 * 看起来不错，构建元组并插入
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	cfgOid = GetNewOidWithIndex(cfgRel, TSConfigOidIndexId,
								Anum_pg_ts_config_oid);
	values[Anum_pg_ts_config_oid - 1] = ObjectIdGetDatum(cfgOid);
	namestrcpy(&cname, cfgname);
	values[Anum_pg_ts_config_cfgname - 1] = NameGetDatum(&cname);
	values[Anum_pg_ts_config_cfgnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_ts_config_cfgowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_ts_config_cfgparser - 1] = ObjectIdGetDatum(prsOid);

	tup = heap_form_tuple(cfgRel->rd_att, values, nulls);

	CatalogTupleInsert(cfgRel, tup);

	if (OidIsValid(sourceOid))
	{
		/*
		 * 从源配置复制词元-词典映射
		   */
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	maptup;
		TupleDesc	mapDesc;
		TupleTableSlot **slot;
		CatalogIndexState indstate;
		int			max_slots,
					slot_init_count,
					slot_stored_count;

		mapRel = table_open(TSConfigMapRelationId, RowExclusiveLock);
		mapDesc = RelationGetDescr(mapRel);

		indstate = CatalogOpenIndexes(mapRel);

		/*
		 * 分配要使用的槽，但延迟代价高昂的初始化，直到我们知道
		 * 它们将被使用。
		 */
		max_slots = MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_ts_config_map);
		slot = palloc(sizeof(TupleTableSlot *) * max_slots);

		ScanKeyInit(&skey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(sourceOid));

		scan = systable_beginscan(mapRel, TSConfigMapIndexId, true,
								  NULL, 1, &skey);

		/* 当前存储元组的槽数              */
		slot_stored_count = 0;
		/* 当前已初始化的槽数           */
		slot_init_count = 0;

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);

			if (slot_init_count < max_slots)
			{
				slot[slot_stored_count] = MakeSingleTupleTableSlot(mapDesc,
																   &TTSOpsHeapTuple);
				slot_init_count++;
			}

			ExecClearTuple(slot[slot_stored_count]);

			memset(slot[slot_stored_count]->tts_isnull, false,
				   slot[slot_stored_count]->tts_tupleDescriptor->natts * sizeof(bool));

			slot[slot_stored_count]->tts_values[Anum_pg_ts_config_map_mapcfg - 1] = cfgOid;
			slot[slot_stored_count]->tts_values[Anum_pg_ts_config_map_maptokentype - 1] = cfgmap->maptokentype;
			slot[slot_stored_count]->tts_values[Anum_pg_ts_config_map_mapseqno - 1] = cfgmap->mapseqno;
			slot[slot_stored_count]->tts_values[Anum_pg_ts_config_map_mapdict - 1] = cfgmap->mapdict;

			ExecStoreVirtualTuple(slot[slot_stored_count]);
			slot_stored_count++;

			/* 如果槽已满，插入一批元组        */
			if (slot_stored_count == max_slots)
			{
				CatalogTuplesMultiInsertWithInfo(mapRel, slot, slot_stored_count,
												 indstate);
				slot_stored_count = 0;
			}
		}

		/* 插入缓冲区中剩余的任何元组 */
		if (slot_stored_count > 0)
			CatalogTuplesMultiInsertWithInfo(mapRel, slot, slot_stored_count,
											 indstate);

		for (int i = 0; i < slot_init_count; i++)
			ExecDropSingleTupleTableSlot(slot[i]);

		systable_endscan(scan);
		CatalogCloseIndexes(indstate);
	}

	address = makeConfigurationDependencies(tup, false, mapRel);

	/* 新文本搜索配置的创建后钩子              */
	InvokeObjectPostCreateHook(TSConfigRelationId, cfgOid, 0);

	heap_freetuple(tup);

	if (mapRel)
		table_close(mapRel, RowExclusiveLock);
	table_close(cfgRel, RowExclusiveLock);

	return address;
}

/*
 * TS 配置删除的核心实现。
  */
void
RemoveTSConfigurationById(Oid cfgId)
{
	Relation	relCfg,
				relMap;
	HeapTuple	tup;
	ScanKeyData skey;
	SysScanDesc scan;

	/* 删除 pg_ts_config 条目    */
	relCfg = table_open(TSConfigRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 cfgId);

	CatalogTupleDelete(relCfg, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relCfg, RowExclusiveLock);

	/* 删除任何 pg_ts_config_map 条目 */
	relMap = table_open(TSConfigMapRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_ts_config_map_mapcfg,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(cfgId));

	scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		CatalogTupleDelete(relMap, &tup->t_self);
	}

	systable_endscan(scan);

	table_close(relMap, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION - 主入口点
     */
ObjectAddress
AlterTSConfiguration(AlterTSConfigurationStmt *stmt)
{
	HeapTuple	tup;
	Oid			cfgId;
	Relation	relMap;
	ObjectAddress address;

	/* 查找配置           */
	tup = GetTSConfigTuple(stmt->cfgname);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(stmt->cfgname))));

	cfgId = ((Form_pg_ts_config) GETSTRUCT(tup))->oid;

	/* 必须是所有者 */
	if (!object_ownercheck(TSConfigRelationId, cfgId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TSCONFIGURATION,
					   NameListToString(stmt->cfgname));

	relMap = table_open(TSConfigMapRelationId, RowExclusiveLock);

	/* 添加或删除映射 */
	if (stmt->dicts)
		MakeConfigurationMapping(stmt, tup, relMap);
	else if (stmt->tokentype)
		DropConfigurationMapping(stmt, tup, relMap);

	/* 更新依赖关系  */
	makeConfigurationDependencies(tup, true, relMap);

	InvokeObjectPostAlterHook(TSConfigRelationId, cfgId, 0);

	ObjectAddressSet(address, TSConfigRelationId, cfgId);

	table_close(relMap, RowExclusiveLock);

	ReleaseSysCache(tup);

	return address;
}

/*
 * 检查词元类型名称是否是 TSTokenTypeItem 列表的成员。
   */
static bool
tstoken_list_member(char *token_name, List *tokens)
{
	ListCell   *c;
	bool		found = false;

	foreach(c, tokens)
	{
		TSTokenTypeItem *ts = (TSTokenTypeItem *) lfirst(c);

		if (strcmp(token_name, ts->name) == 0)
		{
			found = true;
			break;
		}
	}

	return found;
}

/*
 * 将词元类型名称列表转换为唯一的 TSTokenTypeItem 列表。
 *
 * 重复的条目列表将从 tokennames 中删除。
    */
static List *
getTokenTypes(Oid prsId, List *tokennames)
{
	TSParserCacheEntry *prs = lookup_ts_parser_cache(prsId);
	LexDescr   *list;
	List	   *result = NIL;
	int			ntoken;
	ListCell   *tn;

	ntoken = list_length(tokennames);
	if (ntoken == 0)
		return NIL;

	if (!OidIsValid(prs->lextypeOid))
		elog(ERROR, "method lextype isn't defined for text search parser %u",
			 prsId);

	/* lextype 接受一个虚拟参数 */
	list = (LexDescr *) DatumGetPointer(OidFunctionCall1(prs->lextypeOid,
														 (Datum) 0));

	foreach(tn, tokennames)
	{
		String	   *val = lfirst_node(String, tn);
		bool		found = false;
		int			j;

		/* 如果此词元已在结果中，则跳过  */
		if (tstoken_list_member(strVal(val), result))
			continue;

		j = 0;
		while (list && list[j].lexid)
		{
			if (strcmp(strVal(val), list[j].alias) == 0)
			{
				TSTokenTypeItem *ts = (TSTokenTypeItem *) palloc0(sizeof(TSTokenTypeItem));

				ts->num = list[j].lexid;
				ts->name = pstrdup(strVal(val));
				result = lappend(result, ts);
				found = true;
				break;
			}
			j++;
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("token type \"%s\" does not exist",
							strVal(val))));
	}

	return result;
}

/*
 * ALTER TEXT SEARCH CONFIGURATION ADD/ALTER MAPPING
 */
static void
MakeConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap)
{
	Form_pg_ts_config tsform;
	Oid			cfgId;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	maptup;
	int			i;
	int			j;
	Oid			prsId;
	List	   *tokens = NIL;
	int			ntoken;
	Oid		   *dictIds;
	int			ndict;
	ListCell   *c;
	CatalogIndexState indstate;

	tsform = (Form_pg_ts_config) GETSTRUCT(tup);
	cfgId = tsform->oid;
	prsId = tsform->cfgparser;

	tokens = getTokenTypes(prsId, stmt->tokentype);
	ntoken = list_length(tokens);

	if (stmt->override)
	{
		/*
		 * 如果存在且命令是 ALTER，则删除词元的映射
		  */
		foreach(c, tokens)
		{
			TSTokenTypeItem *ts = (TSTokenTypeItem *) lfirst(c);

			ScanKeyInit(&skey[0],
						Anum_pg_ts_config_map_mapcfg,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(cfgId));
			ScanKeyInit(&skey[1],
						Anum_pg_ts_config_map_maptokentype,
						BTEqualStrategyNumber, F_INT4EQ,
						Int32GetDatum(ts->num));

			scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
									  NULL, 2, skey);

			while (HeapTupleIsValid((maptup = systable_getnext(scan))))
			{
				CatalogTupleDelete(relMap, &maptup->t_self);
			}

			systable_endscan(scan);
		}
	}

	/*
	 * 将词典名称列表转换为词典 OIDs 数组      
	        */
	ndict = list_length(stmt->dicts);
	dictIds = (Oid *) palloc(sizeof(Oid) * ndict);
	i = 0;
	foreach(c, stmt->dicts)
	{
		List	   *names = (List *) lfirst(c);

		dictIds[i] = get_ts_dict_oid(names, false);
		i++;
	}

	indstate = CatalogOpenIndexes(relMap);

	if (stmt->replace)
	{
		/*
		 * 替换现有条目中的特定词典
		              */
		Oid			dictOld = dictIds[0],
					dictNew = dictIds[1];

		ScanKeyInit(&skey[0],
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));

		scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
								  NULL, 1, skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);

			/*
			 * 检查它是否是目标词元类型之一
			 */
			if (tokens)
			{
				bool		tokmatch = false;

				foreach(c, tokens)
				{
					TSTokenTypeItem *ts = (TSTokenTypeItem *) lfirst(c);

					if (cfgmap->maptokentype == ts->num)
					{
						tokmatch = true;
						break;
					}
				}
				if (!tokmatch)
					continue;
			}

			/*
			 * 如果匹配则替换词典
			 */
			if (cfgmap->mapdict == dictOld)
			{
				Datum		repl_val[Natts_pg_ts_config_map];
				bool		repl_null[Natts_pg_ts_config_map];
				bool		repl_repl[Natts_pg_ts_config_map];
				HeapTuple	newtup;

				memset(repl_val, 0, sizeof(repl_val));
				memset(repl_null, false, sizeof(repl_null));
				memset(repl_repl, false, sizeof(repl_repl));

				repl_val[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictNew);
				repl_repl[Anum_pg_ts_config_map_mapdict - 1] = true;

				newtup = heap_modify_tuple(maptup,
										   RelationGetDescr(relMap),
										   repl_val, repl_null, repl_repl);
				CatalogTupleUpdateWithInfo(relMap, &newtup->t_self, newtup, indstate);
			}
		}

		systable_endscan(scan);
	}
	else
	{
		TupleTableSlot **slot;
		int			slotCount = 0;
		int			nslots;

		/* 分配要使用的槽并初始化它们       */
		nslots = Min(ntoken * ndict,
					 MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_ts_config_map));
		slot = palloc(sizeof(TupleTableSlot *) * nslots);
		for (i = 0; i < nslots; i++)
			slot[i] = MakeSingleTupleTableSlot(RelationGetDescr(relMap),
											   &TTSOpsHeapTuple);

		/*
		 * 插入新条目
		          */
		foreach(c, tokens)
		{
			TSTokenTypeItem *ts = (TSTokenTypeItem *) lfirst(c);

			for (j = 0; j < ndict; j++)
			{
				ExecClearTuple(slot[slotCount]);

				memset(slot[slotCount]->tts_isnull, false,
					   slot[slotCount]->tts_tupleDescriptor->natts * sizeof(bool));

				slot[slotCount]->tts_values[Anum_pg_ts_config_map_mapcfg - 1] = ObjectIdGetDatum(cfgId);
				slot[slotCount]->tts_values[Anum_pg_ts_config_map_maptokentype - 1] = Int32GetDatum(ts->num);
				slot[slotCount]->tts_values[Anum_pg_ts_config_map_mapseqno - 1] = Int32GetDatum(j + 1);
				slot[slotCount]->tts_values[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictIds[j]);

				ExecStoreVirtualTuple(slot[slotCount]);
				slotCount++;

				/* 如果槽已满，插入一批元组        */
				if (slotCount == nslots)
				{
					CatalogTuplesMultiInsertWithInfo(relMap, slot, slotCount,
													 indstate);
					slotCount = 0;
				}
			}
		}

		/* 插入缓冲区中剩余的任何元组 */
		if (slotCount > 0)
			CatalogTuplesMultiInsertWithInfo(relMap, slot, slotCount,
											 indstate);

		for (i = 0; i < nslots; i++)
			ExecDropSingleTupleTableSlot(slot[i]);
	}

	/* 清理   */
	CatalogCloseIndexes(indstate);

	EventTriggerCollectAlterTSConfig(stmt, cfgId, dictIds, ndict);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION DROP MAPPING
 */
static void
DropConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap)
{
	Form_pg_ts_config tsform;
	Oid			cfgId;
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	maptup;
	Oid			prsId;
	List	   *tokens = NIL;
	ListCell   *c;

	tsform = (Form_pg_ts_config) GETSTRUCT(tup);
	cfgId = tsform->oid;
	prsId = tsform->cfgparser;

	tokens = getTokenTypes(prsId, stmt->tokentype);

	foreach(c, tokens)
	{
		TSTokenTypeItem *ts = (TSTokenTypeItem *) lfirst(c);
		bool		found = false;

		ScanKeyInit(&skey[0],
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));
		ScanKeyInit(&skey[1],
					Anum_pg_ts_config_map_maptokentype,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(ts->num));

		scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
								  NULL, 2, skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			CatalogTupleDelete(relMap, &maptup->t_self);
			found = true;
		}

		systable_endscan(scan);

		if (!found)
		{
			if (!stmt->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("mapping for token type \"%s\" does not exist",
								ts->name)));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("mapping for token type \"%s\" does not exist, skipping",
								ts->name)));
			}
		}
	}

	EventTriggerCollectAlterTSConfig(stmt, cfgId, NULL, 0);
}


/*
 * 序列化词典选项，从 DefElem 列表生成 TEXT 数据
 *
 * 这用于形成存储在 pg_ts_dict.dictinitoption 中的值。
 * 为了方便 pg_dump，输出格式与 CREATE TEXT SEARCH DICTIONARY
 * 中重现相同选项所需出现的格式完全相同。
 *
                                                   */
text *
serialize_deflist(List *deflist)
{
	text	   *result;
	StringInfoData buf;
	ListCell   *l;

	initStringInfo(&buf);

	foreach(l, deflist)
	{
		DefElem    *defel = (DefElem *) lfirst(l);
		char	   *val = defGetString(defel);

		appendStringInfo(&buf, "%s = ",
						 quote_identifier(defel->defname));

		/*
		 * 如果值是 T_Integer 或 T_Float，则不带引号发出，否则带引号。
		 * 这对于正确重构节点类型和值是必不可少的。
 *
		                                          */
		if (IsA(defel->arg, Integer) || IsA(defel->arg, Float))
			appendStringInfoString(&buf, val);
		else
		{
			/* 如果出现反斜杠，则强制使用 E 语法以安全地引用它们 */
			if (strchr(val, '\\'))
				appendStringInfoChar(&buf, ESCAPE_STRING_SYNTAX);
			appendStringInfoChar(&buf, '\'');
			while (*val)
			{
				char		ch = *val++;

				if (SQL_STR_DOUBLE(ch, true))
					appendStringInfoChar(&buf, ch);
				appendStringInfoChar(&buf, ch);
			}
			appendStringInfoChar(&buf, '\'');
		}
		if (lnext(deflist, l) != NULL)
			appendStringInfoString(&buf, ", ");
	}

	result = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);
	return result;
}

/*
 * 反序列化词典选项，从 TEXT 重构 DefElem 列表
 *
 * 这也用于 prsheadline 选项，因此为了向后兼容，我们需要接受
 * serialize_deflist() 永远不会发出的一些内容：特别是未引用和
 * 双引号字符串。
                              */
List *
deserialize_deflist(Datum txt)
{
	text	   *in = DatumGetTextPP(txt);	/* 以防它被 TOAST 处理过 */
	List	   *result = NIL;
	int			len = VARSIZE_ANY_EXHDR(in);
	char	   *ptr,
			   *endptr,
			   *workspace,
			   *wsptr = NULL,
			   *startvalue = NULL;
	typedef enum
	{
		CS_WAITKEY,
		CS_INKEY,
		CS_INQKEY,
		CS_WAITEQ,
		CS_WAITVALUE,
		CS_INSQVALUE,
		CS_INDQVALUE,
		CS_INWVALUE
	} ds_state;
	ds_state	state = CS_WAITKEY;

	workspace = (char *) palloc(len + 1);	/* 肯定有足够的空间 */
	ptr = VARDATA_ANY(in);
	endptr = ptr + len;
	for (; ptr < endptr; ptr++)
	{
		switch (state)
		{
			case CS_WAITKEY:
				if (isspace((unsigned char) *ptr) || *ptr == ',')
					continue;
				if (*ptr == '"')
				{
					wsptr = workspace;
					state = CS_INQKEY;
				}
				else
				{
					wsptr = workspace;
					*wsptr++ = *ptr;
					state = CS_INKEY;
				}
				break;
			case CS_INKEY:
				if (isspace((unsigned char) *ptr))
				{
					*wsptr++ = '\0';
					state = CS_WAITEQ;
				}
				else if (*ptr == '=')
				{
					*wsptr++ = '\0';
					state = CS_WAITVALUE;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INQKEY:
				if (*ptr == '"')
				{
					if (ptr + 1 < endptr && ptr[1] == '"')
					{
						/* 仅复制两个引号中的一个 */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						state = CS_WAITEQ;
					}
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_WAITEQ:
				if (*ptr == '=')
					state = CS_WAITVALUE;
				else if (!isspace((unsigned char) *ptr))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("invalid parameter list format: \"%s\"",
									text_to_cstring(in))));
				break;
			case CS_WAITVALUE:
				if (*ptr == '\'')
				{
					startvalue = wsptr;
					state = CS_INSQVALUE;
				}
				else if (*ptr == 'E' && ptr + 1 < endptr && ptr[1] == '\'')
				{
					ptr++;
					startvalue = wsptr;
					state = CS_INSQVALUE;
				}
				else if (*ptr == '"')
				{
					startvalue = wsptr;
					state = CS_INDQVALUE;
				}
				else if (!isspace((unsigned char) *ptr))
				{
					startvalue = wsptr;
					*wsptr++ = *ptr;
					state = CS_INWVALUE;
				}
				break;
			case CS_INSQVALUE:
				if (*ptr == '\'')
				{
					if (ptr + 1 < endptr && ptr[1] == '\'')
					{
						/* 仅复制两个引号中的一个 */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						result = lappend(result,
										 buildDefItem(workspace,
													  startvalue,
													  true));
						state = CS_WAITKEY;
					}
				}
				else if (*ptr == '\\')
				{
					if (ptr + 1 < endptr && ptr[1] == '\\')
					{
						/* 仅复制两个反斜杠中的一个 */
						*wsptr++ = *ptr++;
					}
					else
						*wsptr++ = *ptr;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INDQVALUE:
				if (*ptr == '"')
				{
					if (ptr + 1 < endptr && ptr[1] == '"')
					{
						/* 仅复制两个引号中的一个 */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						result = lappend(result,
										 buildDefItem(workspace,
													  startvalue,
													  true));
						state = CS_WAITKEY;
					}
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INWVALUE:
				if (*ptr == ',' || isspace((unsigned char) *ptr))
				{
					*wsptr++ = '\0';
					result = lappend(result,
									 buildDefItem(workspace,
												  startvalue,
												  false));
					state = CS_WAITKEY;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			default:
				elog(ERROR, "unrecognized deserialize_deflist state: %d",
					 state);
		}
	}

	if (state == CS_INWVALUE)
	{
		*wsptr++ = '\0';
		result = lappend(result,
						 buildDefItem(workspace,
									  startvalue,
									  false));
	}
	else if (state != CS_WAITKEY)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid parameter list format: \"%s\"",
						text_to_cstring(in))));

	pfree(workspace);

	return result;
}

/*
 * 为 deserialize_deflist 构建一个 DefElem
 */
static DefElem *
buildDefItem(const char *name, const char *val, bool was_quoted)
{
	/* 如果输入被引用，则始终作为字符串发出 */
	if (!was_quoted && val[0] != '\0')
	{
		int			v;
		char	   *endptr;

		/* 尝试解析为整数      */
		errno = 0;
		v = strtoint(val, &endptr, 10);
		if (errno == 0 && *endptr == '\0')
			return makeDefElem(pstrdup(name),
							   (Node *) makeInteger(v),
							   -1);
		/* 不是，那么作为浮点数呢？ */
		errno = 0;
		(void) strtod(val, &endptr);
		if (errno == 0 && *endptr == '\0')
			return makeDefElem(pstrdup(name),
							   (Node *) makeFloat(pstrdup(val)),
							   -1);

		if (strcmp(val, "true") == 0)
			return makeDefElem(pstrdup(name),
							   (Node *) makeBoolean(true),
							   -1);
		if (strcmp(val, "false") == 0)
			return makeDefElem(pstrdup(name),
							   (Node *) makeBoolean(false),
							   -1);
	}
	/* 就把它当作字符串 */
	return makeDefElem(pstrdup(name),
					   (Node *) makeString(pstrdup(val)),
					   -1);
}
