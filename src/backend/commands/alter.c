/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  通用 ALTER 命令的驱动程序
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "replication/logicalworker.h"
#include "rewrite/rewriteDefine.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid);

/*
 * 报告一个错误：指定名称的对象已
 * 存在于给定命名空间中。
 */
static void
report_name_conflict(Oid classId, const char *name)
{
	char	   *msgfmt;

	switch (classId)
	{
		case EventTriggerRelationId:
			msgfmt = gettext_noop("event trigger \"%s\" already exists");
			break;
		case ForeignDataWrapperRelationId:
			msgfmt = gettext_noop("foreign-data wrapper \"%s\" already exists");
			break;
		case ForeignServerRelationId:
			msgfmt = gettext_noop("server \"%s\" already exists");
			break;
		case LanguageRelationId:
			msgfmt = gettext_noop("language \"%s\" already exists");
			break;
		case PublicationRelationId:
			msgfmt = gettext_noop("publication \"%s\" already exists");
			break;
		case SubscriptionRelationId:
			msgfmt = gettext_noop("subscription \"%s\" already exists");
			break;
		default:
			elog(ERROR, "unsupported object class: %u", classId);
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_OBJECT),
			 errmsg(msgfmt, name)));
}

static void
report_namespace_conflict(Oid classId, const char *name, Oid nspOid)
{
	char	   *msgfmt;

	Assert(OidIsValid(nspOid));

	switch (classId)
	{
		case ConversionRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("conversion \"%s\" already exists in schema \"%s\"");
			break;
		case StatisticExtRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("statistics object \"%s\" already exists in schema \"%s\"");
			break;
		case TSParserRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search parser \"%s\" already exists in schema \"%s\"");
			break;
		case TSDictionaryRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search dictionary \"%s\" already exists in schema \"%s\"");
			break;
		case TSTemplateRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search template \"%s\" already exists in schema \"%s\"");
			break;
		case TSConfigRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search configuration \"%s\" already exists in schema \"%s\"");
			break;
		default:
			elog(ERROR, "unsupported object class: %u", classId);
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_OBJECT),
			 errmsg(msgfmt, name, get_namespace_name(nspOid))));
}

/*
 * AlterObjectRename_internal
 *
 * 重命名给定对象的通用函数，适用于简单情况（不适用于表，
 * 也不适用于除了更改单个目录条目的名称列之外
 * 还需要做更多处理的情况）。
 *
 * rel: 包含对象的目录关系（调用方已加 RowExclusiveLock）
 * objectId: 要重命名的对象的 OID
 * new_name: 新名称的 CString 表示
 */
static void
AlterObjectRename_internal(Relation rel, Oid objectId, const char *new_name)
{
	Oid			classId = RelationGetRelid(rel);
	int			oidCacheId = get_object_catcache_oid(classId);
	int			nameCacheId = get_object_catcache_name(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Datum		datum;
	bool		isnull;
	Oid			namespaceId;
	Oid			ownerId;
	char	   *old_name;
	AclResult	aclresult;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;
	NameData	nameattrdata;

	oldtup = SearchSysCache1(oidCacheId, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(rel));

	datum = heap_getattr(oldtup, Anum_name,
						 RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	old_name = NameStr(*(DatumGetName(datum)));

	/* 获取命名空间的 OID */
	if (Anum_namespace > 0)
	{
		datum = heap_getattr(oldtup, Anum_namespace,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		namespaceId = DatumGetObjectId(datum);
	}
	else
		namespaceId = InvalidOid;

	/* 权限检查 ... 超级用户始终可以执行 */
	if (!superuser())
	{
		/* 如果对象没有显式所有者则失败 */
		if (Anum_owner <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to rename %s",
							getObjectDescriptionOids(classId, objectId))));

		/* 否则，必须是现有对象的所有者 */
		datum = heap_getattr(oldtup, Anum_owner,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		ownerId = DatumGetObjectId(datum);

		if (!has_privs_of_role(GetUserId(), DatumGetObjectId(ownerId)))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_object_type(classId, objectId),
						   old_name);

		/* 用户必须拥有命名空间的 CREATE 权限 */
		if (OidIsValid(namespaceId))
		{
			aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(),
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_SCHEMA,
							   get_namespace_name(namespaceId));
		}

		if (classId == SubscriptionRelationId)
		{
			Form_pg_subscription form;

			/* 必须拥有数据库的 CREATE 权限 */
			aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_DATABASE,
							   get_database_name(MyDatabaseId));

			/*
			 * 不允许非超级用户修改 password_required=false
			 * 的订阅。
			 */
			form = (Form_pg_subscription) GETSTRUCT(oldtup);
			if (!form->subpasswordrequired && !superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("password_required=false is superuser-only"),
						 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));
		}
	}

	/*
	 * 检查重复名称（比唯一索引失败更友好）。
	 * 由于这只是友好性检查，在缺乏适当支持的情况下
	 * 我们可以跳过它。
	 */
	if (classId == ProcedureRelationId)
	{
		Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(oldtup);

		IsThereFunctionInNamespace(new_name, proc->pronargs,
								   &proc->proargtypes, proc->pronamespace);
	}
	else if (classId == CollationRelationId)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(oldtup);

		IsThereCollationInNamespace(new_name, coll->collnamespace);
	}
	else if (classId == OperatorClassRelationId)
	{
		Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(oldtup);

		IsThereOpClassInNamespace(new_name, opc->opcmethod,
								  opc->opcnamespace);
	}
	else if (classId == OperatorFamilyRelationId)
	{
		Form_pg_opfamily opf = (Form_pg_opfamily) GETSTRUCT(oldtup);

		IsThereOpFamilyInNamespace(new_name, opf->opfmethod,
								   opf->opfnamespace);
	}
	else if (classId == SubscriptionRelationId)
	{
		if (SearchSysCacheExists2(SUBSCRIPTIONNAME, MyDatabaseId,
								  CStringGetDatum(new_name)))
			report_name_conflict(classId, new_name);

		/* 同时执行回归测试命名规则（如果已启用） */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
		if (strncmp(new_name, "regress_", 8) != 0)
			elog(WARNING, "subscriptions created by regression test cases should have names starting with \"regress_\"");
#endif

		/* 唤醒相关复制工作进程以快速处理此更改 */
		LogicalRepWorkersWakeupAtCommit(objectId);
	}
	else if (nameCacheId >= 0)
	{
		if (OidIsValid(namespaceId))
		{
			if (SearchSysCacheExists2(nameCacheId,
									  CStringGetDatum(new_name),
									  ObjectIdGetDatum(namespaceId)))
				report_namespace_conflict(classId, new_name, namespaceId);
		}
		else
		{
			if (SearchSysCacheExists1(nameCacheId,
									  CStringGetDatum(new_name)))
				report_name_conflict(classId, new_name);
		}
	}

	/* 构建修改后的元组 */
	values = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
	nulls = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	replaces = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	namestrcpy(&nameattrdata, new_name);
	values[Anum_name - 1] = NameGetDatum(&nameattrdata);
	replaces[Anum_name - 1] = true;
	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);

	/* 执行实际更新 */
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);

	InvokeObjectPostAlterHook(classId, objectId, 0);

	/* 释放内存 */
	pfree(values);
	pfree(nulls);
	pfree(replaces);
	heap_freetuple(newtup);

	ReleaseSysCache(oldtup);
}

/*
 * 执行 ALTER OBJECT / RENAME TO 语句。根据对象类型，
 * 调用适合该类型的函数。
 *
 * 返回值是重命名对象的地址。
 */
ObjectAddress
ExecRenameStmt(RenameStmt *stmt)
{
	switch (stmt->renameType)
	{
		case OBJECT_TABCONSTRAINT:
		case OBJECT_DOMCONSTRAINT:
			return RenameConstraint(stmt);

		case OBJECT_DATABASE:
			return RenameDatabase(stmt->subname, stmt->newname);

		case OBJECT_ROLE:
			return RenameRole(stmt->subname, stmt->newname);

		case OBJECT_SCHEMA:
			return RenameSchema(stmt->subname, stmt->newname);

		case OBJECT_TABLESPACE:
			return RenameTableSpace(stmt->subname, stmt->newname);

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_INDEX:
		case OBJECT_FOREIGN_TABLE:
			return RenameRelation(stmt);

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
			return renameatt(stmt);

		case OBJECT_RULE:
			return RenameRewriteRule(stmt->relation, stmt->subname,
									 stmt->newname);

		case OBJECT_TRIGGER:
			return renametrig(stmt);

		case OBJECT_POLICY:
			return rename_policy(stmt);

		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			return RenameType(stmt);

		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_FUNCTION:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_LANGUAGE:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_PUBLICATION:
		case OBJECT_SUBSCRIPTION:
			{
				ObjectAddress address;
				Relation	catalog;
				Relation	relation;

				address = get_object_address(stmt->renameType,
											 stmt->object,
											 &relation,
											 AccessExclusiveLock, false);
				Assert(relation == NULL);

				catalog = table_open(address.classId, RowExclusiveLock);
				AlterObjectRename_internal(catalog,
										   address.objectId,
										   stmt->newname);
				table_close(catalog, RowExclusiveLock);

				return address;
			}

		default:
			elog(ERROR, "unrecognized rename stmt type: %d",
				 (int) stmt->renameType);
			return InvalidObjectAddress;	/* 保持编译器安静 */
	}
}

/*
 * 执行 ALTER OBJECT / [NO] DEPENDS ON EXTENSION 语句。
 *
 * 返回值是被修改对象的地址。refAddress 是一个输出参数，
 * 如果不为 null，则接收被修改对象现在所依赖的
 * 对象的地址。
 */
ObjectAddress
ExecAlterObjectDependsStmt(AlterObjectDependsStmt *stmt, ObjectAddress *refAddress)
{
	ObjectAddress address;
	ObjectAddress refAddr;
	Relation	rel;

	address =
		get_object_address_rv(stmt->objectType, stmt->relation, (List *) stmt->object,
							  &rel, AccessExclusiveLock, false);

	/*
	 * 验证用户是否有权运行该命令。
	 *
	 * 我们不检查扩展的任何权限，因为那是不需要的。
	 * 对象所有者通过运行此命令来约定扩展所有者
	 * 可以随时删除该对象，
	 * 这被认为没有问题。
	 */
	check_object_ownership(GetUserId(),
						   stmt->objectType, address, stmt->object, rel);

	/*
	 * 如果涉及关系，它已经被打开并锁定。我们这里不需要
	 * 该关系，但在提交前会保留锁。
	 */
	if (rel)
		table_close(rel, NoLock);

	refAddr = get_object_address(OBJECT_EXTENSION, (Node *) stmt->extname,
								 &rel, AccessExclusiveLock, false);
	Assert(rel == NULL);
	if (refAddress)
		*refAddress = refAddr;

	if (stmt->remove)
	{
		deleteDependencyRecordsForSpecific(address.classId, address.objectId,
										   DEPENDENCY_AUTO_EXTENSION,
										   refAddr.classId, refAddr.objectId);
	}
	else
	{
		List	   *currexts;

		/* 避免重复 */
		currexts = getAutoExtensionsOfObject(address.classId,
											 address.objectId);
		if (!list_member_oid(currexts, refAddr.objectId))
			recordDependencyOn(&address, &refAddr, DEPENDENCY_AUTO_EXTENSION);
	}

	return address;
}

/*
 * 执行 ALTER OBJECT / SET SCHEMA 语句。根据对象类型，
 * 调用适合该类型的函数。
 *
 * 返回值是被修改对象的地址。
 *
 * oldSchemaAddr 是一个输出参数，如果不为 NULL，
 * 则设置为原始模式的对象地址。
 */
ObjectAddress
ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt *stmt,
						  ObjectAddress *oldSchemaAddr)
{
	ObjectAddress address;
	Oid			oldNspOid;

	switch (stmt->objectType)
	{
		case OBJECT_EXTENSION:
			address = AlterExtensionNamespace(strVal(stmt->object), stmt->newschema,
											  oldSchemaAddr ? &oldNspOid : NULL);
			break;

		case OBJECT_FOREIGN_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
			address = AlterTableNamespace(stmt,
										  oldSchemaAddr ? &oldNspOid : NULL);
			break;

		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			address = AlterTypeNamespace(castNode(List, stmt->object), stmt->newschema,
										 stmt->objectType,
										 oldSchemaAddr ? &oldNspOid : NULL);
			break;

			/* 通用代码路径 */
		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_FUNCTION:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
			{
				Relation	catalog;
				Relation	relation;
				Oid			classId;
				Oid			nspOid;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 &relation,
											 AccessExclusiveLock,
											 false);
				Assert(relation == NULL);
				classId = address.classId;
				catalog = table_open(classId, RowExclusiveLock);
				nspOid = LookupCreationNamespace(stmt->newschema);

				oldNspOid = AlterObjectNamespace_internal(catalog, address.objectId,
														  nspOid);
				table_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterObjectSchemaStmt type: %d",
				 (int) stmt->objectType);
			return InvalidObjectAddress;	/* 保持编译器安静 */
	}

	if (oldSchemaAddr)
		ObjectAddressSet(*oldSchemaAddr, NamespaceRelationId, oldNspOid);

	return address;
}

/*
 * 根据对象的 classId 和对象 OID 更改对象的命名空间。
 *
 * 没有命名空间的对象应被忽略。
 *
 * 此函数目前仅由 ALTER EXTENSION SET SCHEMA 使用，
 * 因此只需要涵盖可以作为扩展成员的对象类型，
 * 并且不需要处理某些特殊情况，例如不需要处理
 * 数组类型——这些类型无论如何都不应该是扩展的
 * 直接成员。尽管如此，我们坚持在 switch 中列出所有
 * OCLASS 类型。
 *
 * 返回对象先前命名空间的 OID，如果对象没有模式
 * 则返回 InvalidOid。
 */
Oid
AlterObjectNamespace_oid(Oid classId, Oid objid, Oid nspOid,
						 ObjectAddresses *objsMoved)
{
	Oid			oldNspOid = InvalidOid;
	ObjectAddress dep;

	dep.classId = classId;
	dep.objectId = objid;
	dep.objectSubId = 0;

	switch (getObjectClass(&dep))
	{
		case OCLASS_CLASS:
			{
				Relation	rel;

				rel = relation_open(objid, AccessExclusiveLock);
				oldNspOid = RelationGetNamespace(rel);

				AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);

				relation_close(rel, NoLock);
				break;
			}

		case OCLASS_TYPE:
			oldNspOid = AlterTypeNamespace_oid(objid, nspOid, objsMoved);
			break;

		case OCLASS_PROC:
		case OCLASS_COLLATION:
		case OCLASS_CONVERSION:
		case OCLASS_OPERATOR:
		case OCLASS_OPCLASS:
		case OCLASS_OPFAMILY:
		case OCLASS_STATISTIC_EXT:
		case OCLASS_TSPARSER:
		case OCLASS_TSDICT:
		case OCLASS_TSTEMPLATE:
		case OCLASS_TSCONFIG:
			{
				Relation	catalog;

				catalog = table_open(classId, RowExclusiveLock);

				oldNspOid = AlterObjectNamespace_internal(catalog, objid,
														  nspOid);

				table_close(catalog, RowExclusiveLock);
			}
			break;

		case OCLASS_CAST:
		case OCLASS_CONSTRAINT:
		case OCLASS_DEFAULT:
		case OCLASS_LANGUAGE:
		case OCLASS_LARGEOBJECT:
		case OCLASS_AM:
		case OCLASS_AMOP:
		case OCLASS_AMPROC:
		case OCLASS_REWRITE:
		case OCLASS_TRIGGER:
		case OCLASS_SCHEMA:
		case OCLASS_ROLE:
		case OCLASS_ROLE_MEMBERSHIP:
		case OCLASS_DATABASE:
		case OCLASS_TBLSPACE:
		case OCLASS_FDW:
		case OCLASS_FOREIGN_SERVER:
		case OCLASS_USER_MAPPING:
		case OCLASS_DEFACL:
		case OCLASS_EXTENSION:
		case OCLASS_EVENT_TRIGGER:
		case OCLASS_PARAMETER_ACL:
		case OCLASS_POLICY:
		case OCLASS_PUBLICATION:
		case OCLASS_PUBLICATION_NAMESPACE:
		case OCLASS_PUBLICATION_REL:
		case OCLASS_SUBSCRIPTION:
		case OCLASS_TRANSFORM:
			/* 忽略没有模式限定名称的对象类型 */
			break;

			/*
			 * 这里故意没有 default: 分支；我们希望编译器在
			 * 新的 OCLASS 未在上面处理时发出警告。
			 */
	}

	return oldNspOid;
}

/*
 * 更改给定对象命名空间的通用函数，适用于简单情况
 * （不适用于表，也不适用于除了更改单个目录条目的
 * 命名空间列之外还需要做更多处理的情况）。
 *
 * rel: 包含对象的目录关系（调用方已加 RowExclusiveLock）
 * objid: 要更改其命名空间的对象的 OID
 * nspOid: 新命名空间的 OID
 *
 * 返回对象先前命名空间的 OID。
 */
static Oid
AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid)
{
	Oid			classId = RelationGetRelid(rel);
	int			oidCacheId = get_object_catcache_oid(classId);
	int			nameCacheId = get_object_catcache_name(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	Oid			oldNspOid;
	Datum		name,
				namespace;
	bool		isnull;
	HeapTuple	tup,
				newtup;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;

	tup = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objid));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objid, RelationGetRelationName(rel));

	name = heap_getattr(tup, Anum_name, RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	namespace = heap_getattr(tup, Anum_namespace, RelationGetDescr(rel),
							 &isnull);
	Assert(!isnull);
	oldNspOid = DatumGetObjectId(namespace);

	/*
	 * 如果对象已经在正确的命名空间中，我们不需要做任何事，
	 * 只需触发对象访问钩子。
	 */
	if (oldNspOid == nspOid)
	{
		InvokeObjectPostAlterHook(classId, objid, 0);
		return oldNspOid;
	}

	/* 检查基本的命名空间相关问题 */
	CheckSetNamespace(oldNspOid, nspOid);

	/* 权限检查 ... 超级用户始终可以执行 */
	if (!superuser())
	{
		Datum		owner;
		Oid			ownerId;
		AclResult	aclresult;

		/* 如果对象没有显式所有者则失败 */
		if (Anum_owner <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to set schema of %s",
							getObjectDescriptionOids(classId, objid))));

		/* 否则，必须是现有对象的所有者 */
		owner = heap_getattr(tup, Anum_owner, RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		ownerId = DatumGetObjectId(owner);

		if (!has_privs_of_role(GetUserId(), ownerId))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_object_type(classId, objid),
						   NameStr(*(DatumGetName(name))));

		/* 用户必须拥有新命名空间的 CREATE 权限 */
		aclresult = object_aclcheck(NamespaceRelationId, nspOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(nspOid));
	}

	/*
	 * 检查重复名称（比唯一索引失败更友好）。
	 * 由于这只是友好性检查，在缺乏适当支持的情况下
	 * 我们可以跳过它。
	 */
	if (classId == ProcedureRelationId)
	{
		Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tup);

		IsThereFunctionInNamespace(NameStr(proc->proname), proc->pronargs,
								   &proc->proargtypes, nspOid);
	}
	else if (classId == CollationRelationId)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(tup);

		IsThereCollationInNamespace(NameStr(coll->collname), nspOid);
	}
	else if (classId == OperatorClassRelationId)
	{
		Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(tup);

		IsThereOpClassInNamespace(NameStr(opc->opcname),
								  opc->opcmethod, nspOid);
	}
	else if (classId == OperatorFamilyRelationId)
	{
		Form_pg_opfamily opf = (Form_pg_opfamily) GETSTRUCT(tup);

		IsThereOpFamilyInNamespace(NameStr(opf->opfname),
								   opf->opfmethod, nspOid);
	}
	else if (nameCacheId >= 0 &&
			 SearchSysCacheExists2(nameCacheId, name,
								   ObjectIdGetDatum(nspOid)))
		report_namespace_conflict(classId,
								  NameStr(*(DatumGetName(name))),
								  nspOid);

	/* 构建修改后的元组 */
	values = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
	nulls = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	replaces = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	values[Anum_namespace - 1] = ObjectIdGetDatum(nspOid);
	replaces[Anum_namespace - 1] = true;
	newtup = heap_modify_tuple(tup, RelationGetDescr(rel),
							   values, nulls, replaces);

	/* 执行实际更新 */
	CatalogTupleUpdate(rel, &tup->t_self, newtup);

	/* 释放内存 */
	pfree(values);
	pfree(nulls);
	pfree(replaces);

	/* 更新依赖关系以指向新模式 */
	changeDependencyFor(classId, objid,
						NamespaceRelationId, oldNspOid, nspOid);

	InvokeObjectPostAlterHook(classId, objid, 0);

	return oldNspOid;
}

/*
 * 执行 ALTER OBJECT / OWNER TO 语句。根据对象类型，
 * 调用适合该类型的函数。
 */
ObjectAddress
ExecAlterOwnerStmt(AlterOwnerStmt *stmt)
{
	Oid			newowner = get_rolespec_oid(stmt->newowner, false);

	switch (stmt->objectType)
	{
		case OBJECT_DATABASE:
			return AlterDatabaseOwner(strVal(stmt->object), newowner);

		case OBJECT_SCHEMA:
			return AlterSchemaOwner(strVal(stmt->object), newowner);

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:		/* 同 TYPE */
			return AlterTypeOwner(castNode(List, stmt->object), newowner, stmt->objectType);
			break;

		case OBJECT_FDW:
			return AlterForeignDataWrapperOwner(strVal(stmt->object),
												newowner);

		case OBJECT_FOREIGN_SERVER:
			return AlterForeignServerOwner(strVal(stmt->object),
										   newowner);

		case OBJECT_EVENT_TRIGGER:
			return AlterEventTriggerOwner(strVal(stmt->object),
										  newowner);

		case OBJECT_PUBLICATION:
			return AlterPublicationOwner(strVal(stmt->object),
										 newowner);

		case OBJECT_SUBSCRIPTION:
			return AlterSubscriptionOwner(strVal(stmt->object),
										  newowner);

			/* 通用情况 */
		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_FUNCTION:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TABLESPACE:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSCONFIGURATION:
			{
				Relation	catalog;
				Relation	relation;
				Oid			classId;
				ObjectAddress address;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 &relation,
											 AccessExclusiveLock,
											 false);
				Assert(relation == NULL);
				classId = address.classId;

				/*
				 * 对于大对象，要修改的目录是
				 * pg_largeobject_metadata。
				 */
				if (classId == LargeObjectRelationId)
					classId = LargeObjectMetadataRelationId;

				catalog = table_open(classId, RowExclusiveLock);

				AlterObjectOwner_internal(catalog, address.objectId, newowner);
				table_close(catalog, RowExclusiveLock);

				return address;
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterOwnerStmt type: %d",
				 (int) stmt->objectType);
			return InvalidObjectAddress;	/* 保持编译器安静 */
	}
}

/*
 * 更改给定对象所有权的通用函数，适用于简单情况
 * （不适用于表，也不适用于除了更改单个目录条目的
 * 所有权列之外还需要做更多处理的情况）。
 *
 * rel: 包含对象的目录关系（调用方已加 RowExclusiveLock）
 * objectId: 要更改其所有权的对象的 OID
 * new_ownerId: 新对象所有者的 OID
 */
void
AlterObjectOwner_internal(Relation rel, Oid objectId, Oid new_ownerId)
{
	Oid			classId = RelationGetRelid(rel);
	AttrNumber	Anum_oid = get_object_attnum_oid(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_acl = get_object_attnum_acl(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	HeapTuple	oldtup;
	Datum		datum;
	bool		isnull;
	Oid			old_ownerId;
	Oid			namespaceId = InvalidOid;

	/* 搜索元组并锁定它。 */
	oldtup =
		get_catalog_object_by_oid_extended(rel, Anum_oid, objectId, true);
	if (oldtup == NULL)
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(rel));

	datum = heap_getattr(oldtup, Anum_owner,
						 RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	old_ownerId = DatumGetObjectId(datum);

	if (Anum_namespace != InvalidAttrNumber)
	{
		datum = heap_getattr(oldtup, Anum_namespace,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		namespaceId = DatumGetObjectId(datum);
	}

	if (old_ownerId != new_ownerId)
	{
		AttrNumber	nattrs;
		HeapTuple	newtup;
		Datum	   *values;
		bool	   *nulls;
		bool	   *replaces;

		/* 超级用户可以绕过权限检查 */
		if (!superuser())
		{
			/* 必须是所有者 */
			if (!has_privs_of_role(GetUserId(), old_ownerId))
			{
				char	   *objname;
				char		namebuf[NAMEDATALEN];

				if (Anum_name != InvalidAttrNumber)
				{
					datum = heap_getattr(oldtup, Anum_name,
										 RelationGetDescr(rel), &isnull);
					Assert(!isnull);
					objname = NameStr(*DatumGetName(datum));
				}
				else
				{
					snprintf(namebuf, sizeof(namebuf), "%u", objectId);
					objname = namebuf;
				}
				aclcheck_error(ACLCHECK_NOT_OWNER, get_object_type(classId, objectId),
							   objname);
			}
			/* 必须能够成为新所有者 */
			check_can_set_role(GetUserId(), new_ownerId);

			/* 新所有者必须拥有命名空间的 CREATE 权限 */
			if (OidIsValid(namespaceId))
			{
				AclResult	aclresult;

				aclresult = object_aclcheck(NamespaceRelationId, namespaceId, new_ownerId,
											ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_SCHEMA,
								   get_namespace_name(namespaceId));
			}
		}

		/* 构建修改后的元组 */
		nattrs = RelationGetNumberOfAttributes(rel);
		values = palloc0(nattrs * sizeof(Datum));
		nulls = palloc0(nattrs * sizeof(bool));
		replaces = palloc0(nattrs * sizeof(bool));
		values[Anum_owner - 1] = ObjectIdGetDatum(new_ownerId);
		replaces[Anum_owner - 1] = true;

		/*
		 * 确定新所有者的修改后 ACL。这仅在 ACL
		 * 非空时才需要。
		 */
		if (Anum_acl != InvalidAttrNumber)
		{
			datum = heap_getattr(oldtup,
								 Anum_acl, RelationGetDescr(rel), &isnull);
			if (!isnull)
			{
				Acl		   *newAcl;

				newAcl = aclnewowner(DatumGetAclP(datum),
									 old_ownerId, new_ownerId);
				values[Anum_acl - 1] = PointerGetDatum(newAcl);
				replaces[Anum_acl - 1] = true;
			}
		}

		newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
								   values, nulls, replaces);

		/* 执行实际更新 */
		CatalogTupleUpdate(rel, &newtup->t_self, newtup);

		UnlockTuple(rel, &oldtup->t_self, InplaceUpdateTupleLock);

		/*
		 * 更新所有者依赖引用。当处理大对象时，
		 * 我们必须转换回 LO 的 classId 通常使用的
		 * OID。
		 */
		if (classId == LargeObjectMetadataRelationId)
			classId = LargeObjectRelationId;

		changeDependencyOnOwner(classId, objectId, new_ownerId);

		/* 释放内存 */
		pfree(values);
		pfree(nulls);
		pfree(replaces);
	}
	else
	{
		UnlockTuple(rel, &oldtup->t_self, InplaceUpdateTupleLock);

		/*
		 * 无需更改任何内容。但当处理大对象时，我们必须
		 * 转换回 LO 的 classId 通常使用的 OID，
		 * 否则 post-alter 钩子（如果有）会混乱。
		 */
		if (classId == LargeObjectMetadataRelationId)
			classId = LargeObjectRelationId;
	}

	InvokeObjectPostAlterHook(classId, objectId, 0);
}
