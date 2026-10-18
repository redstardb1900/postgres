/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL LANGUAGE 支持代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/proclang.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CREATE LANGUAGE
 */
ObjectAddress
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	const char *languageName = stmt->plname;
	Oid			languageOwner = GetUserId();
	Oid			handlerOid,
				inlineOid,
				valOid;
	Oid			funcrettype;
	Oid			funcargtypes[1];
	Relation	rel;
	TupleDesc	tupDesc;
	Datum		values[Natts_pg_language];
	bool		nulls[Natts_pg_language];
	bool		replaces[Natts_pg_language];
	NameData	langname;
	HeapTuple	oldtup;
	HeapTuple	tup;
	Oid			langoid;
	bool		is_update;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	/*
	 * 检查权限
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create custom procedural language")));

	/*
	 * 查找 PL 处理器函数，并检查它是否具有预期的
	 * 返回类型
	 */
	Assert(stmt->plhandler);
	handlerOid = LookupFuncName(stmt->plhandler, 0, NULL, false);
	funcrettype = get_func_rettype(handlerOid);
	if (funcrettype != LANGUAGE_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->plhandler), "language_handler")));

	/* 验证内联函数 */
	if (stmt->plinline)
	{
		funcargtypes[0] = INTERNALOID;
		inlineOid = LookupFuncName(stmt->plinline, 1, funcargtypes, false);
		/* 返回值被忽略，因此我们不检查其类型 */
	}
	else
		inlineOid = InvalidOid;

	/* 验证验证器函数 */
	if (stmt->plvalidator)
	{
		funcargtypes[0] = OIDOID;
		valOid = LookupFuncName(stmt->plvalidator, 1, funcargtypes, false);
		/* 返回值被忽略，因此我们不检查其类型 */
	}
	else
		valOid = InvalidOid;

	/* 可以创建它 */
	rel = table_open(LanguageRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* 准备要插入的数据 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, true, sizeof(replaces));

	namestrcpy(&langname, languageName);
	values[Anum_pg_language_lanname - 1] = NameGetDatum(&langname);
	values[Anum_pg_language_lanowner - 1] = ObjectIdGetDatum(languageOwner);
	values[Anum_pg_language_lanispl - 1] = BoolGetDatum(true);
	values[Anum_pg_language_lanpltrusted - 1] = BoolGetDatum(stmt->pltrusted);
	values[Anum_pg_language_lanplcallfoid - 1] = ObjectIdGetDatum(handlerOid);
	values[Anum_pg_language_laninline - 1] = ObjectIdGetDatum(inlineOid);
	values[Anum_pg_language_lanvalidator - 1] = ObjectIdGetDatum(valOid);
	nulls[Anum_pg_language_lanacl - 1] = true;

	/* 检查是否已存在定义 */
	oldtup = SearchSysCache1(LANGNAME, PointerGetDatum(languageName));

	if (HeapTupleIsValid(oldtup))
	{
		Form_pg_language oldform = (Form_pg_language) GETSTRUCT(oldtup);

		/* 已有一个；可以替换它吗？ */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("language \"%s\" already exists", languageName)));

		/* 这目前没有意义，因为我们已经检查了超级用户 */
#ifdef NOT_USED
		if (!object_ownercheck(LanguageRelationId, oldform->oid, languageOwner))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_LANGUAGE,
						   languageName);
#endif

		/*
		 * 不要更改现有的 oid、所有权或权限。请注意，
		 * 下面的依赖更新代码必须与这一决定保持一致。
		 */
		replaces[Anum_pg_language_oid - 1] = false;
		replaces[Anum_pg_language_lanowner - 1] = false;
		replaces[Anum_pg_language_lanacl - 1] = false;

		/* 好的，执行吧... */
		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		langoid = oldform->oid;
		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* 创建新的语言 */
		langoid = GetNewOidWithIndex(rel, LanguageOidIndexId,
									 Anum_pg_language_oid);
		values[Anum_pg_language_oid - 1] = ObjectIdGetDatum(langoid);
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(rel, tup);
		is_update = false;
	}

	/*
	 * 为新语言创建依赖。如果我们正在更新
	 * 现有语言，则先删除任何现有的 pg_depend 条目。
	 * （但是，由于我们没有更改所有权或权限，
	 * 共享依赖 *不* 需要更改，我们对其保持不变。）
	 */
	myself.classId = LanguageRelationId;
	myself.objectId = langoid;
	myself.objectSubId = 0;

	if (is_update)
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);

	/* 对语言所有者的依赖 */
	if (!is_update)
		recordDependencyOnOwner(myself.classId, myself.objectId,
								languageOwner);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, is_update);

	addrs = new_object_addresses();

	/* 对 PL 处理器函数的依赖 */
	ObjectAddressSet(referenced, ProcedureRelationId, handlerOid);
	add_exact_object_address(&referenced, addrs);

	/* 对内联处理器函数的依赖（如果有的话） */
	if (OidIsValid(inlineOid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, inlineOid);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对验证器函数的依赖（如果有的话） */
	if (OidIsValid(valOid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, valOid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* 新过程语言的创建后钩子 */
	InvokeObjectPostCreateHook(LanguageRelationId, myself.objectId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * get_language_oid - 给定语言名称，查找 OID
 *
 * 如果 missing_ok 为 false，则在未找到语言名称时抛出错误。如果
 * 为 true，则只返回 InvalidOid。
 */
Oid
get_language_oid(const char *langname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(LANGNAME, Anum_pg_language_oid,
						  CStringGetDatum(langname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", langname)));
	return oid;
}
