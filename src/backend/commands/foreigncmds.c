/*-------------------------------------------------------------------------
 *
 * foreigncmds.c
 *	  外部数据包装器/服务器创建和操作命令
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/foreigncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	char	   *tablename;
	char	   *cmd;
} import_error_callback_arg;

/* 内部函数 */
static void import_error_callback(void *arg);


/*
 * 将 DefElem 列表转换为 pg_foreign_data_wrapper、         
 *  pg_foreign_server、pg_user_mapping 以及                     
 * pg_foreign_table 
 *
 * 以 Datum 形式返回该数组；若列表为空，则返回     
 * PointerGetDatum(NULL)
 *
 * 注意：数组通常不经进一步处理存入数据库， 
 * 因此任何验证都应在此转换之前完成           
 * 。        
 */
static Datum
optionListToArray(List *options)
{
	ArrayBuildState *astate = NULL;
	ListCell   *cell;

	foreach(cell, options)
	{
		DefElem    *def = lfirst(cell);
		const char *name;
		const char *value;
		Size		len;
		text	   *t;

		name = def->defname;
		value = defGetString(def);

		/* 名称不能包含 "="，否则 "a=b=c" 会有歧义 */
		if (strchr(name, '=') != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid option name \"%s\": must not contain \"=\"",
							name)));

		len = VARHDRSZ + strlen(name) + 1 + strlen(value);
		/* +1 为 sprintf 的结尾 null 留出空间 */
		t = palloc(len + 1);
		SET_VARSIZE(t, len);
		sprintf(VARDATA(t), "%s=%s", name, value);

		astate = accumArrayResult(astate, PointerGetDatum(t),
								  false, TEXTOID,
								  CurrentMemoryContext);
	}

	if (astate)
		return makeArrayResult(astate, CurrentMemoryContext);

	return PointerGetDatum(NULL);
}


/*
 * 将 DefElem 列表转换为文本数组格式。这实质上与           
 *  optionListToArray() 是同一件事，只是我们识别用于修改  
 * 现有选项列表的 SET/ADD/DROP 动作，该列表以 Datum 形   
 * 式作为 oldOptions 传入。此外，如果 fdwvalidator 不是
 *  InvalidOid，则指定结果调用的验证器函数。
 *
 * 以 Datum 形式返回该数组；若列表为空，则返回     
 * PointerGetDatum(NULL)
 *
 * 它用于 CREATE/ALTER FOREIGN DATA WRAPPER/SERVER/USER                  
 *  FOREIGN TABLE
 */
Datum
transformGenericOptions(Oid catalogId,
						Datum oldOptions,
						List *options,
						Oid fdwvalidator)
{
	List	   *resultOptions = untransformRelOptions(oldOptions);
	ListCell   *optcell;
	Datum		result;

	foreach(optcell, options)
	{
		DefElem    *od = lfirst(optcell);
		ListCell   *cell;

		/*
		 * 在 resultOptions 中查找元素，所有情况都需要用于验证。
		*
		 */
		foreach(cell, resultOptions)
		{
			DefElem    *def = lfirst(cell);

			if (strcmp(def->defname, od->defname) == 0)
				break;
		}

		/*
		 * 可以对同一个选项执行多次 SET/DROP 操作。        
		 * 只要待添加的选项唯一，标准就允许这样。       
		 * 注意，未指定的操作被视为                             
		 * ADD 
		 */
		switch (od->defaction)
		{
			case DEFELEM_DROP:
				if (!cell)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("option \"%s\" not found",
									od->defname)));
				resultOptions = list_delete_cell(resultOptions, cell);
				break;

			case DEFELEM_SET:
				if (!cell)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("option \"%s\" not found",
									od->defname)));
				lfirst(cell) = od;
				break;

			case DEFELEM_ADD:
			case DEFELEM_UNSPEC:
				if (cell)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("option \"%s\" provided more than once",
									od->defname)));
				resultOptions = lappend(resultOptions, od);
				break;

			default:
				elog(ERROR, "unrecognized action %d on option \"%s\"",
					 (int) od->defaction, od->defname);
				break;
		}
	}

	result = optionListToArray(resultOptions);

	if (OidIsValid(fdwvalidator))
	{
		Datum		valarg = result;

		/*
		 * 将 null 选项列表作为空数组传递，验证器无需声明
		 * 为 non-strict 即可处理此情况。
		 */
		if (DatumGetPointer(valarg) == NULL)
			valarg = PointerGetDatum(construct_empty_array(TEXTOID));
		OidFunctionCall2(fdwvalidator, valarg, ObjectIdGetDatum(catalogId));
	}

	return result;
}


/*
 * 更改数据包装器所有者的内部实现。
 *
 * 仅超级用户可执行；新所有者也必须是超级用户。
*
 */
static void
AlterForeignDataWrapperOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_foreign_data_wrapper form;
	Datum		repl_val[Natts_pg_foreign_data_wrapper];
	bool		repl_null[Natts_pg_foreign_data_wrapper];
	bool		repl_repl[Natts_pg_foreign_data_wrapper];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	form = (Form_pg_foreign_data_wrapper) GETSTRUCT(tup);

	/* 必须是超级用户才能更改 FDW 所有者 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of foreign-data wrapper \"%s\"",
						NameStr(form->fdwname)),
				 errhint("Must be superuser to change owner of a foreign-data wrapper.")));

	/* 新所有者也必须是超级用户 */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of foreign-data wrapper \"%s\"",
						NameStr(form->fdwname)),
				 errhint("The owner of a foreign-data wrapper must be a superuser.")));

	if (form->fdwowner != newOwnerId)
	{
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_foreign_data_wrapper_fdwowner - 1] = true;
		repl_val[Anum_pg_foreign_data_wrapper_fdwowner - 1] = ObjectIdGetDatum(newOwnerId);

		aclDatum = heap_getattr(tup,
								Anum_pg_foreign_data_wrapper_fdwacl,
								RelationGetDescr(rel),
								&isNull);
		/* Null ACL 无需更改 */
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 form->fdwowner, newOwnerId);
			repl_repl[Anum_pg_foreign_data_wrapper_fdwacl - 1] = true;
			repl_val[Anum_pg_foreign_data_wrapper_fdwacl - 1] = PointerGetDatum(newAcl);
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
								repl_repl);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		/* 更新所有者依赖引用 */
		changeDependencyOnOwner(ForeignDataWrapperRelationId,
								form->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(ForeignDataWrapperRelationId,
							  form->oid, 0);
}

/*
 * 按名称更改外部数据包装器所有者
 *
 * 注意：限制见上述 "_internal" 函数。
 */
ObjectAddress
AlterForeignDataWrapperOwner(const char *name, Oid newOwnerId)
{
	Oid			fdwId;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_foreign_data_wrapper form;


	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNDATAWRAPPERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist", name)));

	form = (Form_pg_foreign_data_wrapper) GETSTRUCT(tup);
	fdwId = form->oid;

	AlterForeignDataWrapperOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, ForeignDataWrapperRelationId, fdwId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * 按 OID 更改外部数据包装器所有者
 *
 * 注意：限制见上述 "_internal" 函数。
 */
void
AlterForeignDataWrapperOwner_oid(Oid fwdId, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fwdId));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper with OID %u does not exist", fwdId)));

	AlterForeignDataWrapperOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * 更改外部服务器所有者的内部实现
 */
static void
AlterForeignServerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_foreign_server form;
	Datum		repl_val[Natts_pg_foreign_server];
	bool		repl_null[Natts_pg_foreign_server];
	bool		repl_repl[Natts_pg_foreign_server];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	form = (Form_pg_foreign_server) GETSTRUCT(tup);

	if (form->srvowner != newOwnerId)
	{
		/* 超级用户始终可以执行 */
		if (!superuser())
		{
			Oid			srvId;
			AclResult	aclresult;

			srvId = form->oid;

			/* 必须是所有者 */
			if (!object_ownercheck(ForeignServerRelationId, srvId, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
							   NameStr(form->srvname));

			/* 必须能够成为新所有者 */
			check_can_set_role(GetUserId(), newOwnerId);

			/* 新所有者必须有外部数据包装器的 USAGE 权限 */
			aclresult = object_aclcheck(ForeignDataWrapperRelationId, form->srvfdw, newOwnerId, ACL_USAGE);
			if (aclresult != ACLCHECK_OK)
			{
				ForeignDataWrapper *fdw = GetForeignDataWrapper(form->srvfdw);

				aclcheck_error(aclresult, OBJECT_FDW, fdw->fdwname);
			}
		}

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_foreign_server_srvowner - 1] = true;
		repl_val[Anum_pg_foreign_server_srvowner - 1] = ObjectIdGetDatum(newOwnerId);

		aclDatum = heap_getattr(tup,
								Anum_pg_foreign_server_srvacl,
								RelationGetDescr(rel),
								&isNull);
		/* Null ACL 无需更改 */
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 form->srvowner, newOwnerId);
			repl_repl[Anum_pg_foreign_server_srvacl - 1] = true;
			repl_val[Anum_pg_foreign_server_srvacl - 1] = PointerGetDatum(newAcl);
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
								repl_repl);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		/* 更新所有者依赖引用 */
		changeDependencyOnOwner(ForeignServerRelationId, form->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(ForeignServerRelationId,
							  form->oid, 0);
}

/*
 * 按名称更改外部服务器所有者
 */
ObjectAddress
AlterForeignServerOwner(const char *name, Oid newOwnerId)
{
	Oid			servOid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_foreign_server form;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNSERVERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", name)));

	form = (Form_pg_foreign_server) GETSTRUCT(tup);
	servOid = form->oid;

	AlterForeignServerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, ForeignServerRelationId, servOid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * 按 OID 更改外部服务器所有者
 */
void
AlterForeignServerOwner_oid(Oid srvId, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNSERVEROID, ObjectIdGetDatum(srvId));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign server with OID %u does not exist", srvId)));

	AlterForeignServerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * 将解析器传入的处理器函数名转换为 OID。
 */
static Oid
lookup_fdw_handler_func(DefElem *handler)
{
	Oid			handlerOid;

	if (handler == NULL || handler->arg == NULL)
		return InvalidOid;

	/* 处理器无参数 */
	handlerOid = LookupFuncName((List *) handler->arg, 0, NULL, false);

	/* 检查处理器返回类型是否正确 */
	if (get_func_rettype(handlerOid) != FDW_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString((List *) handler->arg), "fdw_handler")));

	return handlerOid;
}

/*
 * 将解析器传入的验证器函数名转换为 OID。
 */
static Oid
lookup_fdw_validator_func(DefElem *validator)
{
	Oid			funcargtypes[2];

	if (validator == NULL || validator->arg == NULL)
		return InvalidOid;

	/* 验证器接收 text[], oid */
	funcargtypes[0] = TEXTARRAYOID;
	funcargtypes[1] = OIDOID;

	return LookupFuncName((List *) validator->arg, 2, funcargtypes, false);
	/* 验证器返回值被忽略，不检查类型 */
}

/*
 * 处理 CREATE/ALTER FDW 的函数选项
 */
static void
parse_func_options(ParseState *pstate, List *func_options,
				   bool *handler_given, Oid *fdwhandler,
				   bool *validator_given, Oid *fdwvalidator)
{
	ListCell   *cell;

	*handler_given = false;
	*validator_given = false;
	/* 未提供则返回 InvalidOid */
	*fdwhandler = InvalidOid;
	*fdwvalidator = InvalidOid;

	foreach(cell, func_options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "handler") == 0)
		{
			if (*handler_given)
				errorConflictingDefElem(def, pstate);
			*handler_given = true;
			*fdwhandler = lookup_fdw_handler_func(def);
		}
		else if (strcmp(def->defname, "validator") == 0)
		{
			if (*validator_given)
				errorConflictingDefElem(def, pstate);
			*validator_given = true;
			*fdwvalidator = lookup_fdw_validator_func(def);
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 def->defname);
	}
}

/*
 * 创建外部数据包装器
 */
ObjectAddress
CreateForeignDataWrapper(ParseState *pstate, CreateFdwStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_foreign_data_wrapper];
	bool		nulls[Natts_pg_foreign_data_wrapper];
	HeapTuple	tuple;
	Oid			fdwId;
	bool		handler_given;
	bool		validator_given;
	Oid			fdwhandler;
	Oid			fdwvalidator;
	Datum		fdwoptions;
	Oid			ownerId;
	ObjectAddress myself;
	ObjectAddress referenced;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	/* 必须是超级用户 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create foreign-data wrapper \"%s\"",
						stmt->fdwname),
				 errhint("Must be superuser to create a foreign-data wrapper.")));

	/* 创建时暂不支持指定所有者。使用有效用户 ID。 */
	ownerId = GetUserId();

	/*
	 * 检查是否已存在同名外部数据包装器。
	 */
	if (GetForeignDataWrapperByName(stmt->fdwname, true) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" already exists",
						stmt->fdwname)));

	/*
	 * 向 pg_foreign_data_wrapper 插入元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	fdwId = GetNewOidWithIndex(rel, ForeignDataWrapperOidIndexId,
							   Anum_pg_foreign_data_wrapper_oid);
	values[Anum_pg_foreign_data_wrapper_oid - 1] = ObjectIdGetDatum(fdwId);
	values[Anum_pg_foreign_data_wrapper_fdwname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->fdwname));
	values[Anum_pg_foreign_data_wrapper_fdwowner - 1] = ObjectIdGetDatum(ownerId);

	/* 查找处理器和验证器函数（如提供） */
	parse_func_options(pstate, stmt->func_options,
					   &handler_given, &fdwhandler,
					   &validator_given, &fdwvalidator);

	values[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = ObjectIdGetDatum(fdwhandler);
	values[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = ObjectIdGetDatum(fdwvalidator);

	nulls[Anum_pg_foreign_data_wrapper_fdwacl - 1] = true;

	fdwoptions = transformGenericOptions(ForeignDataWrapperRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdwvalidator);

	if (PointerIsValid(DatumGetPointer(fdwoptions)))
		values[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = fdwoptions;
	else
		nulls[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* 记录依赖 */
	myself.classId = ForeignDataWrapperRelationId;
	myself.objectId = fdwId;
	myself.objectSubId = 0;

	if (OidIsValid(fdwhandler))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = fdwhandler;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	if (OidIsValid(fdwvalidator))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = fdwvalidator;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	recordDependencyOnOwner(ForeignDataWrapperRelationId, fdwId, ownerId);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新外部数据包装器的创建后钩子 */
	InvokeObjectPostCreateHook(ForeignDataWrapperRelationId, fdwId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * 修改外部数据包装器
 */
ObjectAddress
AlterForeignDataWrapper(ParseState *pstate, AlterFdwStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Form_pg_foreign_data_wrapper fdwForm;
	Datum		repl_val[Natts_pg_foreign_data_wrapper];
	bool		repl_null[Natts_pg_foreign_data_wrapper];
	bool		repl_repl[Natts_pg_foreign_data_wrapper];
	Oid			fdwId;
	bool		isnull;
	Datum		datum;
	bool		handler_given;
	bool		validator_given;
	Oid			fdwhandler;
	Oid			fdwvalidator;
	ObjectAddress myself;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	/* 必须是超级用户 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter foreign-data wrapper \"%s\"",
						stmt->fdwname),
				 errhint("Must be superuser to alter a foreign-data wrapper.")));

	tp = SearchSysCacheCopy1(FOREIGNDATAWRAPPERNAME,
							 CStringGetDatum(stmt->fdwname));

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist", stmt->fdwname)));

	fdwForm = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);
	fdwId = fdwForm->oid;

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	parse_func_options(pstate, stmt->func_options,
					   &handler_given, &fdwhandler,
					   &validator_given, &fdwvalidator);

	if (handler_given)
	{
		repl_val[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = ObjectIdGetDatum(fdwhandler);
		repl_repl[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = true;

		/*
		 * 新处理器可能改变访问外部表的行为，对此发出警告。
		*
		 */
		ereport(WARNING,
				(errmsg("changing the foreign-data wrapper handler can change behavior of existing foreign tables")));
	}

	if (validator_given)
	{
		repl_val[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = ObjectIdGetDatum(fdwvalidator);
		repl_repl[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = true;

		/*
		 * FDW 或依赖的 SERVER、USER MAPPING、FOREIGN TABLE 对象的
		 * 现有选项可能不再符合新验证器的要求，对此发出警告。
		*
		 */
		if (OidIsValid(fdwvalidator))
			ereport(WARNING,
					(errmsg("changing the foreign-data wrapper validator can cause "
							"the options for dependent objects to become invalid")));
	}
	else
	{
		/*
		 * 验证器未更改，但仍需用于验证选项。
		 */
		fdwvalidator = fdwForm->fdwvalidator;
	}

	/*
	 * 如果指定了选项，则进行验证和更新。
	 */
	if (stmt->options)
	{
		/* 提取当前选项 */
		datum = SysCacheGetAttr(FOREIGNDATAWRAPPEROID,
								tp,
								Anum_pg_foreign_data_wrapper_fdwoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* 转换选项 */
		datum = transformGenericOptions(ForeignDataWrapperRelationId,
										datum,
										stmt->options,
										fdwvalidator);

		if (PointerIsValid(DatumGetPointer(datum)))
			repl_val[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = datum;
		else
			repl_null[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;

		repl_repl[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;
	}

	/* 一切正常 - 更新元组 */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	heap_freetuple(tp);

	ObjectAddressSet(myself, ForeignDataWrapperRelationId, fdwId);

	/* 如果更改了函数则更新函数依赖 */
	if (handler_given || validator_given)
	{
		ObjectAddress referenced;

		/*
		 * 清空此 FDW 对函数的所有现有依赖记录；
		 * 假定除正在修复的以外没有其他依赖。
		 */
		deleteDependencyRecordsForClass(ForeignDataWrapperRelationId,
										fdwId,
										ProcedureRelationId,
										DEPENDENCY_NORMAL);

		/* 并构建新的。 */

		if (OidIsValid(fdwhandler))
		{
			referenced.classId = ProcedureRelationId;
			referenced.objectId = fdwhandler;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		if (OidIsValid(fdwvalidator))
		{
			referenced.classId = ProcedureRelationId;
			referenced.objectId = fdwvalidator;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	InvokeObjectPostAlterHook(ForeignDataWrapperRelationId, fdwId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * 创建外部服务器
 */
ObjectAddress
CreateForeignServer(CreateForeignServerStmt *stmt)
{
	Relation	rel;
	Datum		srvoptions;
	Datum		values[Natts_pg_foreign_server];
	bool		nulls[Natts_pg_foreign_server];
	HeapTuple	tuple;
	Oid			srvId;
	Oid			ownerId;
	AclResult	aclresult;
	ObjectAddress myself;
	ObjectAddress referenced;
	ForeignDataWrapper *fdw;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	/* 创建时暂不支持指定所有者。使用有效用户 ID。 */
	ownerId = GetUserId();

	/*
	 * 检查是否已存在同名外部服务器。如存在且指定了
	 * IF NOT EXISTS 则不做任何操作。
	 */
	srvId = get_foreign_server_oid(stmt->servername, true);
	if (OidIsValid(srvId))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * 如果在扩展脚本中，要求已存在对象
			 * 必须是扩展成员，以避免安全风险。
			 */
			ObjectAddressSet(myself, ForeignServerRelationId, srvId);
			checkMembershipInCurrentExtension(&myself);

			/* 可跳过 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("server \"%s\" already exists, skipping",
							stmt->servername)));
			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("server \"%s\" already exists",
							stmt->servername)));
	}

	/*
	 * 检查 FDW 是否存在且有 USAGE 权限。同时获取实际
	 * FDW 用于选项验证等。
	 */
	fdw = GetForeignDataWrapperByName(stmt->fdwname, false);

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdw->fdwid, ownerId, ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FDW, fdw->fdwname);

	/*
	 * 向 pg_foreign_server 插入元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	srvId = GetNewOidWithIndex(rel, ForeignServerOidIndexId,
							   Anum_pg_foreign_server_oid);
	values[Anum_pg_foreign_server_oid - 1] = ObjectIdGetDatum(srvId);
	values[Anum_pg_foreign_server_srvname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->servername));
	values[Anum_pg_foreign_server_srvowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_foreign_server_srvfdw - 1] = ObjectIdGetDatum(fdw->fdwid);

	/* 服务器类型（如提供） */
	if (stmt->servertype)
		values[Anum_pg_foreign_server_srvtype - 1] =
			CStringGetTextDatum(stmt->servertype);
	else
		nulls[Anum_pg_foreign_server_srvtype - 1] = true;

	/* 服务器版本（如提供） */
	if (stmt->version)
		values[Anum_pg_foreign_server_srvversion - 1] =
			CStringGetTextDatum(stmt->version);
	else
		nulls[Anum_pg_foreign_server_srvversion - 1] = true;

	/* 以空白 ACL 开始 */
	nulls[Anum_pg_foreign_server_srvacl - 1] = true;

	/* 添加服务器选项 */
	srvoptions = transformGenericOptions(ForeignServerRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(srvoptions)))
		values[Anum_pg_foreign_server_srvoptions - 1] = srvoptions;
	else
		nulls[Anum_pg_foreign_server_srvoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* 记录依赖 */
	myself.classId = ForeignServerRelationId;
	myself.objectId = srvId;
	myself.objectSubId = 0;

	referenced.classId = ForeignDataWrapperRelationId;
	referenced.objectId = fdw->fdwid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(ForeignServerRelationId, srvId, ownerId);

	/* 对扩展的依赖 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新外部服务器的创建后钩子 */
	InvokeObjectPostCreateHook(ForeignServerRelationId, srvId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * 修改外部服务器
 */
ObjectAddress
AlterForeignServer(AlterForeignServerStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Datum		repl_val[Natts_pg_foreign_server];
	bool		repl_null[Natts_pg_foreign_server];
	bool		repl_repl[Natts_pg_foreign_server];
	Oid			srvId;
	Form_pg_foreign_server srvForm;
	ObjectAddress address;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tp = SearchSysCacheCopy1(FOREIGNSERVERNAME,
							 CStringGetDatum(stmt->servername));

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", stmt->servername)));

	srvForm = (Form_pg_foreign_server) GETSTRUCT(tp);
	srvId = srvForm->oid;

	/*
	 * 仅所有者或超级用户可以 ALTER SERVER。
	 */
	if (!object_ownercheck(ForeignServerRelationId, srvId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
					   stmt->servername);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (stmt->has_version)
	{
		/*
		 * 更改服务器 VERSION 字符串。
		 */
		if (stmt->version)
			repl_val[Anum_pg_foreign_server_srvversion - 1] =
				CStringGetTextDatum(stmt->version);
		else
			repl_null[Anum_pg_foreign_server_srvversion - 1] = true;

		repl_repl[Anum_pg_foreign_server_srvversion - 1] = true;
	}

	if (stmt->options)
	{
		ForeignDataWrapper *fdw = GetForeignDataWrapper(srvForm->srvfdw);
		Datum		datum;
		bool		isnull;

		/* 提取当前 srvoptions */
		datum = SysCacheGetAttr(FOREIGNSERVEROID,
								tp,
								Anum_pg_foreign_server_srvoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* 准备选项数组 */
		datum = transformGenericOptions(ForeignServerRelationId,
										datum,
										stmt->options,
										fdw->fdwvalidator);

		if (PointerIsValid(DatumGetPointer(datum)))
			repl_val[Anum_pg_foreign_server_srvoptions - 1] = datum;
		else
			repl_null[Anum_pg_foreign_server_srvoptions - 1] = true;

		repl_repl[Anum_pg_foreign_server_srvoptions - 1] = true;
	}

	/* 一切正常 - 更新元组 */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	InvokeObjectPostAlterHook(ForeignServerRelationId, srvId, 0);

	ObjectAddressSet(address, ForeignServerRelationId, srvId);

	heap_freetuple(tp);

	table_close(rel, RowExclusiveLock);

	return address;
}


/*
 * 用户映射相关 DDL 命令的权限检查通用例程。
 * 允许服务器所有者操作任何映射，用户操作自己的映射。
*
 */
static void
user_mapping_ddl_aclcheck(Oid umuserid, Oid serverid, const char *servername)
{
	Oid			curuserid = GetUserId();

	if (!object_ownercheck(ForeignServerRelationId, serverid, curuserid))
	{
		if (umuserid == curuserid)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(ForeignServerRelationId, serverid, curuserid, ACL_USAGE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, servername);
		}
		else
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
						   servername);
	}
}


/*
 * 创建用户映射
 */
ObjectAddress
CreateUserMapping(CreateUserMappingStmt *stmt)
{
	Relation	rel;
	Datum		useoptions;
	Datum		values[Natts_pg_user_mapping];
	bool		nulls[Natts_pg_user_mapping];
	HeapTuple	tuple;
	Oid			useId;
	Oid			umId;
	ObjectAddress myself;
	ObjectAddress referenced;
	ForeignServer *srv;
	ForeignDataWrapper *fdw;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	rel = table_open(UserMappingRelationId, RowExclusiveLock);

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
		useId = get_rolespec_oid(stmt->user, false);

	/* 检查服务器是否存在。 */
	srv = GetForeignServerByName(stmt->servername, false);

	user_mapping_ddl_aclcheck(useId, srv->serverid, stmt->servername);

	/*
	 * 检查用户映射在服务器内是否唯一。
	 */
	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));

	if (OidIsValid(umId))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * 用户映射非扩展成员（见下方注释），
			 * 无需 checkMembershipInCurrentExtension。
			 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("user mapping for \"%s\" already exists for server \"%s\", skipping",
							MappingUserName(useId),
							stmt->servername)));

			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("user mapping for \"%s\" already exists for server \"%s\"",
							MappingUserName(useId),
							stmt->servername)));
	}

	fdw = GetForeignDataWrapper(srv->fdwid);

	/*
	 * 向 pg_user_mapping 插入元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	umId = GetNewOidWithIndex(rel, UserMappingOidIndexId,
							  Anum_pg_user_mapping_oid);
	values[Anum_pg_user_mapping_oid - 1] = ObjectIdGetDatum(umId);
	values[Anum_pg_user_mapping_umuser - 1] = ObjectIdGetDatum(useId);
	values[Anum_pg_user_mapping_umserver - 1] = ObjectIdGetDatum(srv->serverid);

	/* 添加用户选项 */
	useoptions = transformGenericOptions(UserMappingRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(useoptions)))
		values[Anum_pg_user_mapping_umoptions - 1] = useoptions;
	else
		nulls[Anum_pg_user_mapping_umoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* 添加对服务器的依赖 */
	myself.classId = UserMappingRelationId;
	myself.objectId = umId;
	myself.objectSubId = 0;

	referenced.classId = ForeignServerRelationId;
	referenced.objectId = srv->serverid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (OidIsValid(useId))
	{
		/* 记录映射用户依赖 */
		recordDependencyOnOwner(UserMappingRelationId, umId, useId);
	}

	/*
	 * 或许将来应在此处调用 recordDependencyOnCurrentExtension；
	 * 但角色非扩展成员，用户映射似乎也不应是。
	 * 如需更改还需扩展语法和 pg_dump。
	*
	 */

	/* 新用户映射的创建后钩子 */
	InvokeObjectPostCreateHook(UserMappingRelationId, umId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * 修改用户映射
 */
ObjectAddress
AlterUserMapping(AlterUserMappingStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Datum		repl_val[Natts_pg_user_mapping];
	bool		repl_null[Natts_pg_user_mapping];
	bool		repl_repl[Natts_pg_user_mapping];
	Oid			useId;
	Oid			umId;
	ForeignServer *srv;
	ObjectAddress address;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	rel = table_open(UserMappingRelationId, RowExclusiveLock);

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
		useId = get_rolespec_oid(stmt->user, false);

	srv = GetForeignServerByName(stmt->servername, false);

	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));
	if (!OidIsValid(umId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user mapping for \"%s\" does not exist for server \"%s\"",
						MappingUserName(useId), stmt->servername)));

	user_mapping_ddl_aclcheck(useId, srv->serverid, stmt->servername);

	tp = SearchSysCacheCopy1(USERMAPPINGOID, ObjectIdGetDatum(umId));

	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for user mapping %u", umId);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (stmt->options)
	{
		ForeignDataWrapper *fdw;
		Datum		datum;
		bool		isnull;

		/*
		 * 处理选项。
		 */

		fdw = GetForeignDataWrapper(srv->fdwid);

		datum = SysCacheGetAttr(USERMAPPINGUSERSERVER,
								tp,
								Anum_pg_user_mapping_umoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* 准备选项数组 */
		datum = transformGenericOptions(UserMappingRelationId,
										datum,
										stmt->options,
										fdw->fdwvalidator);

		if (PointerIsValid(DatumGetPointer(datum)))
			repl_val[Anum_pg_user_mapping_umoptions - 1] = datum;
		else
			repl_null[Anum_pg_user_mapping_umoptions - 1] = true;

		repl_repl[Anum_pg_user_mapping_umoptions - 1] = true;
	}

	/* 一切正常 - 更新元组 */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	InvokeObjectPostAlterHook(UserMappingRelationId,
							  umId, 0);

	ObjectAddressSet(address, UserMappingRelationId, umId);

	heap_freetuple(tp);

	table_close(rel, RowExclusiveLock);

	return address;
}


/*
 * 删除用户映射
 */
Oid
RemoveUserMapping(DropUserMappingStmt *stmt)
{
	ObjectAddress object;
	Oid			useId;
	Oid			umId;
	ForeignServer *srv;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
	{
		useId = get_rolespec_oid(stmt->user, stmt->missing_ok);
		if (!OidIsValid(useId))
		{
			/*
			 * 指定了 IF EXISTS，未找到角色且非 public。
			 * 记录并退出。
			 */
			elog(NOTICE, "role \"%s\" does not exist, skipping",
				 role->rolename);
			return InvalidOid;
		}
	}

	srv = GetForeignServerByName(stmt->servername, true);

	if (!srv)
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("server \"%s\" does not exist",
							stmt->servername)));
		/* IF EXISTS，仅记录 */
		ereport(NOTICE,
				(errmsg("server \"%s\" does not exist, skipping",
						stmt->servername)));
		return InvalidOid;
	}

	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));

	if (!OidIsValid(umId))
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("user mapping for \"%s\" does not exist for server \"%s\"",
							MappingUserName(useId), stmt->servername)));

		/* 指定了 IF EXISTS，仅记录 */
		ereport(NOTICE,
				(errmsg("user mapping for \"%s\" does not exist for server \"%s\", skipping",
						MappingUserName(useId), stmt->servername)));
		return InvalidOid;
	}

	user_mapping_ddl_aclcheck(useId, srv->serverid, srv->servername);

	/*
	 * 执行删除
	 */
	object.classId = UserMappingRelationId;
	object.objectId = umId;
	object.objectSubId = 0;

	performDeletion(&object, DROP_CASCADE, 0);

	return umId;
}


/*
 * 创建外部表
 * 在 DefineRelation() 后调用。
 */
void
CreateForeignTable(CreateForeignTableStmt *stmt, Oid relid)
{
	Relation	ftrel;
	Datum		ftoptions;
	Datum		values[Natts_pg_foreign_table];
	bool		nulls[Natts_pg_foreign_table];
	HeapTuple	tuple;
	AclResult	aclresult;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			ownerId;
	ForeignDataWrapper *fdw;
	ForeignServer *server;

	/*
	 * 推进命令计数器确保 pg_attribute 元组可见；
	 * 上一步可能已更新元组添加约束。
	 */
	CommandCounterIncrement();

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	/*
	 * 创建时暂不支持指定所有者。使用有效用户 ID。
	 */
	ownerId = GetUserId();

	/*
	 * 检查外部服务器是否存在且有 USAGE 权限。
	 * 同时获取实际 FDW 用于选项验证等。
	 */
	server = GetForeignServerByName(stmt->servername, false);
	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid, ownerId, ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	fdw = GetForeignDataWrapper(server->fdwid);

	/*
	 * 向 pg_foreign_table 插入元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_foreign_table_ftrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_foreign_table_ftserver - 1] = ObjectIdGetDatum(server->serverid);
	/* 添加表通用选项 */
	ftoptions = transformGenericOptions(ForeignTableRelationId,
										PointerGetDatum(NULL),
										stmt->options,
										fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(ftoptions)))
		values[Anum_pg_foreign_table_ftoptions - 1] = ftoptions;
	else
		nulls[Anum_pg_foreign_table_ftoptions - 1] = true;

	tuple = heap_form_tuple(ftrel->rd_att, values, nulls);

	CatalogTupleInsert(ftrel, tuple);

	heap_freetuple(tuple);

	/* 添加 pg_class 对服务器的依赖 */
	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = 0;

	referenced.classId = ForeignServerRelationId;
	referenced.objectId = server->serverid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	table_close(ftrel, RowExclusiveLock);
}

/*
 * 导入外部模式
 */
void
ImportForeignSchema(ImportForeignSchemaStmt *stmt)
{
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	FdwRoutine *fdw_routine;
	AclResult	aclresult;
	List	   *cmd_list;
	ListCell   *lc;

	/* 检查外部服务器是否存在且有 USAGE 权限 */
	server = GetForeignServerByName(stmt->server_name, false);
	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	/* 检查模式是否存在且有 CREATE 权限 */
	(void) LookupCreationNamespace(stmt->local_schema);

	/* 获取 FDW 并检查是否支持 IMPORT */
	fdw = GetForeignDataWrapper(server->fdwid);
	if (!OidIsValid(fdw->fdwhandler))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("foreign-data wrapper \"%s\" has no handler",
						fdw->fdwname)));
	fdw_routine = GetFdwRoutine(fdw->fdwhandler);
	if (fdw_routine->ImportForeignSchema == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_NO_SCHEMAS),
				 errmsg("foreign-data wrapper \"%s\" does not support IMPORT FOREIGN SCHEMA",
						fdw->fdwname)));

	/* 调用 FDW 获取命令列表 */
	cmd_list = fdw_routine->ImportForeignSchema(stmt, server->serverid);

	/* 解析并执行每条命令 */
	foreach(lc, cmd_list)
	{
		char	   *cmd = (char *) lfirst(lc);
		import_error_callback_arg callback_arg;
		ErrorContextCallback sqlerrcontext;
		List	   *raw_parsetree_list;
		ListCell   *lc2;

		/*
		 * 为 ereport() 设置错误回溯支持，以便生成的 SQL
		 * 中的任何错误都能良好显示。
		 */
		callback_arg.tablename = NULL;	/* 尚不可知 */
		callback_arg.cmd = cmd;
		sqlerrcontext.callback = import_error_callback;
		sqlerrcontext.arg = (void *) &callback_arg;
		sqlerrcontext.previous = error_context_stack;
		error_context_stack = &sqlerrcontext;

		/*
		 * 将 SQL 字符串解析为原始解析树列表。
		 */
		raw_parsetree_list = pg_parse_query(cmd);

		/*
		 * 处理每个解析树（允许 FDW 每字符串多个命令，
		 * 虽然不建议这样做）。
		 */
		foreach(lc2, raw_parsetree_list)
		{
			RawStmt    *rs = lfirst_node(RawStmt, lc2);
			CreateForeignTableStmt *cstmt = (CreateForeignTableStmt *) rs->stmt;
			PlannedStmt *pstmt;

			/*
			 * 仅允许 CreateForeignTableStmt，可跳过
			 * 解析分析、重写和计划步骤。
			 */
			if (!IsA(cstmt, CreateForeignTableStmt))
				elog(ERROR,
					 "foreign-data wrapper \"%s\" returned incorrect statement type %d",
					 fdw->fdwname, (int) nodeTag(cstmt));

			/* 忽略被过滤选项排除的表命令 */
			if (!IsImportableForeignTable(cstmt->base.relation->relname, stmt))
				continue;

			/* 启用错误时报告当前表名 */
			callback_arg.tablename = cstmt->base.relation->relname;

			/* 确保创建模式与 IMPORT 语句中指定的一致 */
			cstmt->base.relation->schemaname = pstrdup(stmt->local_schema);

			/* 无需计划，仅创建包装 PlannedStmt */
			pstmt = makeNode(PlannedStmt);
			pstmt->commandType = CMD_UTILITY;
			pstmt->canSetTag = false;
			pstmt->utilityStmt = (Node *) cstmt;
			pstmt->stmt_location = rs->stmt_location;
			pstmt->stmt_len = rs->stmt_len;

			/* 执行语句 */
			ProcessUtility(pstmt, cmd, false,
						   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
						   None_Receiver, NULL);

			/* 子命令之间务必推进命令计数器 */
			CommandCounterIncrement();

			callback_arg.tablename = NULL;
		}

		error_context_stack = sqlerrcontext.previous;
	}
}

/*
 * 错误上下文回调，提供失败 SQL 语句文本
 */
static void
import_error_callback(void *arg)
{
	import_error_callback_arg *callback_arg = (import_error_callback_arg *) arg;
	int			syntaxerrposition;

	/* 语法错误则转换为内部语法错误报告 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(callback_arg->cmd);
	}

	if (callback_arg->tablename)
		errcontext("importing foreign table \"%s\"",
				   callback_arg->tablename);
}
