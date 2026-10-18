/*-------------------------------------------------------------------------
 *
 * schemacmds.c
 *	  模式的创建/操纵命令
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/schemacmds.c
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
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/schemacmds.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "parser/scansup.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void AlterSchemaOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId);

/*
 * CREATE SCHEMA
 *
 * 注意：调用方应为整个 CREATE SCHEMA 语句传入
 * 位置信息，我们再把它作为各组成命令的位置
 * 传下去。这符合我们的总体计划：即使执行的
 * 是子查询，也报告整个命令的
 * location/len。
 */
Oid
CreateSchemaCommand(CreateSchemaStmt *stmt, const char *queryString,
					int stmt_location, int stmt_len)
{
	const char *schemaName = stmt->schemaname;
	Oid			namespaceId;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	Oid			owner_uid;
	Oid			saved_uid;
	int			save_sec_context;
	int			save_nestlevel;
	char	   *nsp = namespace_search_path;
	AclResult	aclresult;
	ObjectAddress address;
	StringInfoData pathbuf;

	GetUserIdAndSecContext(&saved_uid, &save_sec_context);

	/*
	 * 谁应该拥有新模式？
	 */
	if (stmt->authrole)
		owner_uid = get_rolespec_oid(stmt->authrole, false);
	else
		owner_uid = saved_uid;

	/* 若未指定，就用用户名填充模式名 */
	if (!schemaName)
	{
		HeapTuple	tuple;

		tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(owner_uid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for role %u", owner_uid);
		schemaName =
			pstrdup(NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname));
		ReleaseSysCache(tuple);
	}

	/*
	 * 要创建模式，必须在当前数据库上拥有模式创建权限，
	 * 并且必须能够成为目标角色（这并不意味着目标角色本身
	 * 必须拥有模式创建权限）。后一条规定用于防范
	 * "giveaway" 攻击。注意超级用户必然永远拥有
	 * 这两项权限。
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, saved_uid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	check_can_set_role(saved_uid, owner_uid);

	/* 保护保留模式名的额外检查 */
	if (!allowSystemTableMods && IsReservedName(schemaName))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", schemaName),
				 errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/*
	 * 若给出了 if_not_exists 且模式已存在，就退出。
	 * （注意：未给出 if_not_exists 时无需检查，因为 NamespaceCreate
	 * 反正会抱怨。）我们本可以在做权限检查之前先做这件事，
	 * 但既然 CREATE TABLE IF NOT EXISTS 先做创建权限检查，
	 * 我们也照做。
	 */
	if (stmt->if_not_exists)
	{
		namespaceId = get_namespace_oid(schemaName, true);
		if (OidIsValid(namespaceId))
		{
			/*
			 * 若我们处于扩展脚本中，坚持要求先前存在的对象
			 * 是该扩展的成员，以避免安全风险。
			 */
			ObjectAddressSet(address, NamespaceRelationId, namespaceId);
			checkMembershipInCurrentExtension(&address);

			/* 可以跳过 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_SCHEMA),
					 errmsg("schema \"%s\" already exists, skipping",
							schemaName)));
			return InvalidOid;
		}
	}

	/*
	 * 若请求的授权不同于当前用户，就暂时设置当前用户，
	 * 使 object(s) 以正确的所有权被
	 * 创建。
	 *
	 * （该设置将在本例程结束时恢复，或者出错时
	 * 事务中止会清理一切。）
	 */
	if (saved_uid != owner_uid)
		SetUserIdAndSecContext(owner_uid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/* 创建模式的命名空间 */
	namespaceId = NamespaceCreate(schemaName, owner_uid, false);

	/* 推进命令计数器，使命名空间可见 */
	CommandCounterIncrement();

	/*
	 * 把新模式前插到当前搜索路径。
	 *
	 * 我们使用函数 SET 选项的等价物，让设置恰好
	 * 在模式创建期间持续。guc.c 还会在出错时
	 * 负责撤销该设置。
	 */
	save_nestlevel = NewGUCNestLevel();

	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));

	while (scanner_isspace(*nsp))
		nsp++;

	if (*nsp != '\0')
		appendStringInfo(&pathbuf, ", %s", nsp);

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * 把新模式报告给可能感兴趣的事件触发器。注意我们
	 * 必须在这里而不是在 ProcessUtilitySlow 中做这件事，因为否则
	 * 下面创建的对象会先于模式被报告，
	 * 那就错了。
	 */
	ObjectAddressSet(address, NamespaceRelationId, namespaceId);
	EventTriggerCollectSimpleCommand(address, InvalidObjectAddress,
									 (Node *) stmt);

	/*
	 * 检查嵌在 CREATE SCHEMA 命令中的命令列表，把它们重排
	 * 成没有前向引用的顺序可执行序列。注意结果
	 * 仍是原始解析树列表 —— 一般来说，我们必须实际执行
	 * 过前面的语句，才能对某条语句做
	 * 解析分析。
	 */
	parsetree_list = transformCreateSchemaStmtElements(stmt->schemaElts,
													   schemaName);

	/*
	 * 执行 CREATE SCHEMA 中包含的每条命令。由于语法只允许
	 * 工具命令出现在 CREATE SCHEMA 中，没有必要让它们经过
	 * parse_analyze_*() 或重写器；我们可以直接把它们
	 * 交给 ProcessUtility。
	 */
	foreach(parsetree_item, parsetree_list)
	{
		Node	   *stmt = (Node *) lfirst(parsetree_item);
		PlannedStmt *wrapper;

		/* 需要构造一个包装 PlannedStmt */
		wrapper = makeNode(PlannedStmt);
		wrapper->commandType = CMD_UTILITY;
		wrapper->canSetTag = false;
		wrapper->utilityStmt = stmt;
		wrapper->stmt_location = stmt_location;
		wrapper->stmt_len = stmt_len;

		/* 执行这一步 */
		ProcessUtility(wrapper,
					   queryString,
					   false,
					   PROCESS_UTILITY_SUBCOMMAND,
					   NULL,
					   NULL,
					   None_Receiver,
					   NULL);

		/* 确保后面的步骤能看到这里创建的对象 */
		CommandCounterIncrement();
	}

	/*
	 * 恢复我们在上面设置的 GUC 变量 search_path。
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/* 重置当前用户和安全上下文 */
	SetUserIdAndSecContext(saved_uid, save_sec_context);

	return namespaceId;
}


/*
 * 重命名模式
 */
ObjectAddress
RenameSchema(const char *oldname, const char *newname)
{
	Oid			nspOid;
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;
	ObjectAddress address;
	Form_pg_namespace nspform;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(NAMESPACENAME, CStringGetDatum(oldname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", oldname)));

	nspform = (Form_pg_namespace) GETSTRUCT(tup);
	nspOid = nspform->oid;

	/* 确保新名称不存在 */
	if (OidIsValid(get_namespace_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", newname)));

	/* 必须是所有者 */
	if (!object_ownercheck(NamespaceRelationId, nspOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
					   oldname);

	/* 必须在数据库上拥有 CREATE 权限 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable schema name \"%s\"", newname),
				 errdetail("The prefix \"pg_\" is reserved for system schemas.")));

	/* 重命名 */
	namestrcpy(&nspform->nspname, newname);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(NamespaceRelationId, nspOid, 0);

	ObjectAddressSet(address, NamespaceRelationId, nspOid);

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}

void
AlterSchemaOwner_oid(Oid schemaoid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(schemaoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for schema %u", schemaoid);

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}


/*
 * 更改模式所有者
 */
ObjectAddress
AlterSchemaOwner(const char *name, Oid newOwnerId)
{
	Oid			nspOid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_namespace nspform;

	rel = table_open(NamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(NAMESPACENAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", name)));

	nspform = (Form_pg_namespace) GETSTRUCT(tup);
	nspOid = nspform->oid;

	AlterSchemaOwner_internal(tup, rel, newOwnerId);

	ObjectAddressSet(address, NamespaceRelationId, nspOid);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

static void
AlterSchemaOwner_internal(HeapTuple tup, Relation rel, Oid newOwnerId)
{
	Form_pg_namespace nspForm;

	Assert(tup->t_tableOid == NamespaceRelationId);
	Assert(RelationGetRelid(rel) == NamespaceRelationId);

	nspForm = (Form_pg_namespace) GETSTRUCT(tup);

	/*
	 * 若新所有者与现有所有者相同，就认为命令
	 * 已成功。这是为了转储恢复的目的。
	 */
	if (nspForm->nspowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_namespace];
		bool		repl_null[Natts_pg_namespace];
		bool		repl_repl[Natts_pg_namespace];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;
		AclResult	aclresult;

		/* 否则，必须是现有对象的所有者 */
		if (!object_ownercheck(NamespaceRelationId, nspForm->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
						   NameStr(nspForm->nspname));

		/* 必须能够成为新所有者 */
		check_can_set_role(GetUserId(), newOwnerId);

		/*
		 * 必须拥有创建模式的权利
		 *
		 * 注意：这不同于其他 alter-owner 检查之处在于，
		 * 检查的是当前用户的创建权限，而不是目标
		 * 所有者的。这与模式的 CREATE 情形
		 * 一致。因为超级用户永远拥有这项权利，我们
		 * 不需要为它们做特殊处理。
		 */
		aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_namespace_nspowner - 1] = true;
		repl_val[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * 为新所有者确定修改后的 ACL。仅当 ACL
		 * 非空时才有必要。
		 */
		aclDatum = SysCacheGetAttr(NAMESPACENAME, tup,
								   Anum_pg_namespace_nspacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 nspForm->nspowner, newOwnerId);
			repl_repl[Anum_pg_namespace_nspacl - 1] = true;
			repl_val[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);

		/* 更新所有者依赖引用 */
		changeDependencyOnOwner(NamespaceRelationId, nspForm->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(NamespaceRelationId,
							  nspForm->oid, 0);
}
