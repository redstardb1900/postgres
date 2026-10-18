/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  检查访问控制权限的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/aclchk.c
 *
 * NOTES
 *	  参见 acl.h。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/aclchk_internal.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * ALTER DEFAULT PRIVILEGES 使用的内部格式。
 */
typedef struct
{
	Oid			roleid;			/* 所属角色 */
	Oid			nspid;			/* 命名空间，若无则为 InvalidOid */
	/* 其余字段与 InternalGrant 中相同： */
	bool		is_grant;
	ObjectType	objtype;
	bool		all_privs;
	AclMode		privileges;
	List	   *grantees;
	bool		grant_option;
	DropBehavior behavior;
} InternalDefaultACL;

/*
 * 执行二进制升级时，pg_dump 会调用函数来设置
 * 该变量，以便我们得知：在此变量为 true 期间
 * 需要为 GRANT/REVOKE 命令填充 pg_init_privs 表。
 */
bool		binary_upgrade_record_init_privs = false;

static void ExecGrantStmt_oids(InternalGrant *istmt);
static void ExecGrant_Relation(InternalGrant *istmt);
static void ExecGrant_common(InternalGrant *istmt, Oid classid, AclMode default_privs,
							 void (*object_check) (InternalGrant *istmt, HeapTuple tuple));
static void ExecGrant_Language_check(InternalGrant *istmt, HeapTuple tuple);
static void ExecGrant_Largeobject(InternalGrant *istmt);
static void ExecGrant_Type_check(InternalGrant *istmt, HeapTuple tuple);
static void ExecGrant_Parameter(InternalGrant *istmt);

static void SetDefaultACLsInSchemas(InternalDefaultACL *iacls, List *nspnames);
static void SetDefaultACL(InternalDefaultACL *iacls);

static List *objectNamesToOids(ObjectType objtype, List *objnames,
							   bool is_grant);
static List *objectsInSchemaToOids(ObjectType objtype, List *nspnames);
static List *getRelationsInNamespace(Oid namespaceId, char relkind);
static void expand_col_privileges(List *colnames, Oid table_oid,
								  AclMode this_privileges,
								  AclMode *col_privileges,
								  int num_col_privileges);
static void expand_all_col_privileges(Oid table_oid, Form_pg_class classForm,
									  AclMode this_privileges,
									  AclMode *col_privileges,
									  int num_col_privileges);
static AclMode string_to_privilege(const char *privname);
static const char *privilege_to_string(AclMode privilege);
static AclMode restrict_and_check_grant(bool is_grant, AclMode avail_goptions,
										bool all_privs, AclMode privileges,
										Oid objectId, Oid grantorId,
										ObjectType objtype, const char *objname,
										AttrNumber att_number, const char *colname);
static AclMode pg_aclmask(ObjectType objtype, Oid object_oid, AttrNumber attnum,
						  Oid roleid, AclMode mask, AclMaskHow how);
static AclMode object_aclmask(Oid classid, Oid objectid, Oid roleid,
							  AclMode mask, AclMaskHow how);
static AclMode pg_attribute_aclmask(Oid table_oid, AttrNumber attnum,
									Oid roleid, AclMode mask, AclMaskHow how);
static AclMode pg_attribute_aclmask_ext(Oid table_oid, AttrNumber attnum,
										Oid roleid, AclMode mask,
										AclMaskHow how, bool *is_missing);
static AclMode pg_class_aclmask_ext(Oid table_oid, Oid roleid,
									AclMode mask, AclMaskHow how,
									bool *is_missing);
static AclMode pg_parameter_acl_aclmask(Oid acl_oid, Oid roleid,
										AclMode mask, AclMaskHow how);
static AclMode pg_largeobject_aclmask_snapshot(Oid lobj_oid, Oid roleid,
											   AclMode mask, AclMaskHow how, Snapshot snapshot);
static AclMode pg_namespace_aclmask(Oid nsp_oid, Oid roleid,
									AclMode mask, AclMaskHow how);
static AclMode pg_type_aclmask(Oid type_oid, Oid roleid,
							   AclMode mask, AclMaskHow how);
static void recordExtensionInitPriv(Oid objoid, Oid classoid, int objsubid,
									Acl *new_acl);
static void recordExtensionInitPrivWorker(Oid objoid, Oid classoid, int objsubid,
										  Acl *new_acl);


/*
 * 若 is_grant 为 true，则把给定被授权者的权限
 * 添加到已有的 old_acl。若 is_grant 为 false，
 * 则从 old_acl 中移除给定被授权者的权限。
 *
 * 注意：原 old_acl 会被 pfree 释放。
 */
static Acl *
merge_acl_with_grant(Acl *old_acl, bool is_grant,
					 bool grant_option, DropBehavior behavior,
					 List *grantees, AclMode privileges,
					 Oid grantorId, Oid ownerId)
{
	unsigned	modechg;
	ListCell   *j;
	Acl		   *new_acl;

	modechg = is_grant ? ACL_MODECHG_ADD : ACL_MODECHG_DEL;

	new_acl = old_acl;

	foreach(j, grantees)
	{
		AclItem		aclitem;
		Acl		   *newer_acl;

		aclitem.ai_grantee = lfirst_oid(j);

		/*
		 * 授权选项只能授予单个角色，不能授予 PUBLIC。
		 * 原因是：若用户重新授予其通过 PUBLIC
		 * 获得的权限，之后该用户被移除，
		 * 这种情形将无法清理。
		 */
		if (is_grant && grant_option && aclitem.ai_grantee == ACL_ID_PUBLIC)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("grant options can only be granted to roles")));

		aclitem.ai_grantor = grantorId;

		/*
		 * 这里的条件不对称源于标准规定。在
		 * GRANT 中，grant_option 标志表示 WITH GRANT OPTION，
		 * 即同时授予基本权限及其授权选项。但在
		 * REVOKE 中，普通 revoke 会同时撤销基本权限及其授权
		 * 选项，而 REVOKE GRANT OPTION 只撤销授权选项。
		 */
		ACLITEM_SET_PRIVS_GOPTIONS(aclitem,
								   (is_grant || !grant_option) ? privileges : ACL_NO_RIGHTS,
								   (!is_grant || grant_option) ? privileges : ACL_NO_RIGHTS);

		newer_acl = aclupdate(new_acl, &aclitem, modechg, ownerId, behavior);

		/* 当被授权者很多时避免内存泄漏 */
		pfree(new_acl);
		new_acl = newer_acl;
	}

	return new_acl;
}

/*
 * 把权限限制为我们实际能授予的部分，并发出
 * 标准要求的警告和错误消息。
 */
static AclMode
restrict_and_check_grant(bool is_grant, AclMode avail_goptions, bool all_privs,
						 AclMode privileges, Oid objectId, Oid grantorId,
						 ObjectType objtype, const char *objname,
						 AttrNumber att_number, const char *colname)
{
	AclMode		this_privileges;
	AclMode		whole_mask;

	switch (objtype)
	{
		case OBJECT_COLUMN:
			whole_mask = ACL_ALL_RIGHTS_COLUMN;
			break;
		case OBJECT_TABLE:
			whole_mask = ACL_ALL_RIGHTS_RELATION;
			break;
		case OBJECT_SEQUENCE:
			whole_mask = ACL_ALL_RIGHTS_SEQUENCE;
			break;
		case OBJECT_DATABASE:
			whole_mask = ACL_ALL_RIGHTS_DATABASE;
			break;
		case OBJECT_FUNCTION:
			whole_mask = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case OBJECT_LANGUAGE:
			whole_mask = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			whole_mask = ACL_ALL_RIGHTS_LARGEOBJECT;
			break;
		case OBJECT_SCHEMA:
			whole_mask = ACL_ALL_RIGHTS_SCHEMA;
			break;
		case OBJECT_TABLESPACE:
			whole_mask = ACL_ALL_RIGHTS_TABLESPACE;
			break;
		case OBJECT_FDW:
			whole_mask = ACL_ALL_RIGHTS_FDW;
			break;
		case OBJECT_FOREIGN_SERVER:
			whole_mask = ACL_ALL_RIGHTS_FOREIGN_SERVER;
			break;
		case OBJECT_EVENT_TRIGGER:
			elog(ERROR, "grantable rights not supported for event triggers");
			/* 不会走到这里，但让编译器安静 */
			return ACL_NO_RIGHTS;
		case OBJECT_TYPE:
			whole_mask = ACL_ALL_RIGHTS_TYPE;
			break;
		case OBJECT_PARAMETER_ACL:
			whole_mask = ACL_ALL_RIGHTS_PARAMETER_ACL;
			break;
		default:
			elog(ERROR, "unrecognized object type: %d", objtype);
			/* 不会走到这里，但让编译器安静 */
			return ACL_NO_RIGHTS;
	}

	/*
	 * 若未找到任何授权选项，考虑是否要发出硬错误。
	 * 按标准，只要在该对象上拥有任意权限即可
	 * 通过此处。
	 */
	if (avail_goptions == ACL_NO_RIGHTS)
	{
		if (pg_aclmask(objtype, objectId, att_number, grantorId,
					   whole_mask | ACL_GRANT_OPTION_FOR(whole_mask),
					   ACLMASK_ANY) == ACL_NO_RIGHTS)
		{
			if (objtype == OBJECT_COLUMN && colname)
				aclcheck_error_col(ACLCHECK_NO_PRIV, objtype, objname, colname);
			else
				aclcheck_error(ACLCHECK_NO_PRIV, objtype, objname);
		}
	}

	/*
	 * 把操作限制为我们实际能授予或撤销的部分，
	 * 并在适当时发出警告。（对 REVOKE 而言这并不完全
	 * 符合标准：标准似乎只在 ACL 中没有任何权限位实际
	 * 变化时才要求警告。实际上该行为显得过于嘈杂，
	 * 也与 GRANT 情形不一致。）
	 */
	this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
	if (is_grant)
	{
		if (this_privileges == 0)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted for \"%s\"",
								objname)));
		}
		else if (!all_privs && this_privileges != privileges)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted for \"%s\"",
								objname)));
		}
	}
	else
	{
		if (this_privileges == 0)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked for \"%s\"",
								objname)));
		}
		else if (!all_privs && this_privileges != privileges)
		{
			if (objtype == OBJECT_COLUMN && colname)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked for column \"%s\" of relation \"%s\"",
								colname, objname)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked for \"%s\"",
								objname)));
		}
	}

	return this_privileges;
}

/*
 * 用于执行工具命令 GRANT 和 REVOKE
 */
void
ExecuteGrantStmt(GrantStmt *stmt)
{
	InternalGrant istmt;
	ListCell   *cell;
	const char *errormsg;
	AclMode		all_privileges;

	if (stmt->grantor)
	{
		Oid			grantor;

		grantor = get_rolespec_oid(stmt->grantor, false);

		/*
		 * 目前该子句只是为了 SQL 兼容性，除此之外
		 * 并无多大意义。
		 */
		if (grantor != GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("grantor must be current user")));
	}

	/*
	 * 把常规的 GrantStmt 转换为 InternalGrant 形式。
	 */
	istmt.is_grant = stmt->is_grant;
	istmt.objtype = stmt->objtype;

	/* 收集目标对象的 OID */
	switch (stmt->targtype)
	{
		case ACL_TARGET_OBJECT:
			istmt.objects = objectNamesToOids(stmt->objtype, stmt->objects,
											  stmt->is_grant);
			break;
		case ACL_TARGET_ALL_IN_SCHEMA:
			istmt.objects = objectsInSchemaToOids(stmt->objtype, stmt->objects);
			break;
			/* 此处不应出现 ACL_TARGET_DEFAULTS */
		default:
			elog(ERROR, "unrecognized GrantStmt.targtype: %d",
				 (int) stmt->targtype);
	}

	/* all_privs 将在下面填充 */
	/* privileges 将在下面填充 */
	istmt.col_privs = NIL;		/* 可能在下面填充 */
	istmt.grantees = NIL;		/* 在下面填充 */
	istmt.grant_option = stmt->grant_option;
	istmt.behavior = stmt->behavior;

	/*
	 * 把 RoleSpec 列表转换为 Oid 列表。注意此时我们
	 * 会在适当时插入 ACL_ID_PUBLIC，因此下游
	 * 支持该情形无需再做额外工作。
	 */
	foreach(cell, stmt->grantees)
	{
		RoleSpec   *grantee = (RoleSpec *) lfirst(cell);
		Oid			grantee_uid;

		switch (grantee->roletype)
		{
			case ROLESPEC_PUBLIC:
				grantee_uid = ACL_ID_PUBLIC;
				break;
			default:
				grantee_uid = get_rolespec_oid(grantee, false);
				break;
		}
		istmt.grantees = lappend_oid(istmt.grantees, grantee_uid);
	}

	/*
	 * 把 stmt->privileges（AccessPriv 节点列表）转换为 AclMode
	 * 位掩码。注意：objtype 不能是 OBJECT_COLUMN。
	 */
	switch (stmt->objtype)
	{
		case OBJECT_TABLE:

			/*
			 * 由于这可能是序列，我们同时测试关系位和
			 * 序列位，之后再在已知对象类型时
			 * 做更受限的测试。
			 */
			all_privileges = ACL_ALL_RIGHTS_RELATION | ACL_ALL_RIGHTS_SEQUENCE;
			errormsg = gettext_noop("invalid privilege type %s for relation");
			break;
		case OBJECT_SEQUENCE:
			all_privileges = ACL_ALL_RIGHTS_SEQUENCE;
			errormsg = gettext_noop("invalid privilege type %s for sequence");
			break;
		case OBJECT_DATABASE:
			all_privileges = ACL_ALL_RIGHTS_DATABASE;
			errormsg = gettext_noop("invalid privilege type %s for database");
			break;
		case OBJECT_DOMAIN:
			all_privileges = ACL_ALL_RIGHTS_TYPE;
			errormsg = gettext_noop("invalid privilege type %s for domain");
			break;
		case OBJECT_FUNCTION:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for function");
			break;
		case OBJECT_LANGUAGE:
			all_privileges = ACL_ALL_RIGHTS_LANGUAGE;
			errormsg = gettext_noop("invalid privilege type %s for language");
			break;
		case OBJECT_LARGEOBJECT:
			all_privileges = ACL_ALL_RIGHTS_LARGEOBJECT;
			errormsg = gettext_noop("invalid privilege type %s for large object");
			break;
		case OBJECT_SCHEMA:
			all_privileges = ACL_ALL_RIGHTS_SCHEMA;
			errormsg = gettext_noop("invalid privilege type %s for schema");
			break;
		case OBJECT_PROCEDURE:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for procedure");
			break;
		case OBJECT_ROUTINE:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for routine");
			break;
		case OBJECT_TABLESPACE:
			all_privileges = ACL_ALL_RIGHTS_TABLESPACE;
			errormsg = gettext_noop("invalid privilege type %s for tablespace");
			break;
		case OBJECT_TYPE:
			all_privileges = ACL_ALL_RIGHTS_TYPE;
			errormsg = gettext_noop("invalid privilege type %s for type");
			break;
		case OBJECT_FDW:
			all_privileges = ACL_ALL_RIGHTS_FDW;
			errormsg = gettext_noop("invalid privilege type %s for foreign-data wrapper");
			break;
		case OBJECT_FOREIGN_SERVER:
			all_privileges = ACL_ALL_RIGHTS_FOREIGN_SERVER;
			errormsg = gettext_noop("invalid privilege type %s for foreign server");
			break;
		case OBJECT_PARAMETER_ACL:
			all_privileges = ACL_ALL_RIGHTS_PARAMETER_ACL;
			errormsg = gettext_noop("invalid privilege type %s for parameter");
			break;
		default:
			elog(ERROR, "unrecognized GrantStmt.objtype: %d",
				 (int) stmt->objtype);
			/* 让编译器安静 */
			all_privileges = ACL_NO_RIGHTS;
			errormsg = NULL;
	}

	if (stmt->privileges == NIL)
	{
		istmt.all_privs = true;

		/*
		 * 内部例程会根据对象类型
		 * 将其转换为 ACL_ALL_RIGHTS_*
		 */
		istmt.privileges = ACL_NO_RIGHTS;
	}
	else
	{
		istmt.all_privs = false;
		istmt.privileges = ACL_NO_RIGHTS;

		foreach(cell, stmt->privileges)
		{
			AccessPriv *privnode = (AccessPriv *) lfirst(cell);
			AclMode		priv;

			/*
			 * 若是列级指定，我们暂且将其存入 col_privs；
			 * 但坚持要求它针对的是关系。
			 */
			if (privnode->cols)
			{
				if (stmt->objtype != OBJECT_TABLE)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_GRANT_OPERATION),
							 errmsg("column privileges are only valid for relations")));
				istmt.col_privs = lappend(istmt.col_privs, privnode);
				continue;
			}

			if (privnode->priv_name == NULL)	/* 解析器出错？ */
				elog(ERROR, "AccessPriv node must specify privilege or columns");
			priv = string_to_privilege(privnode->priv_name);

			if (priv & ~((AclMode) all_privileges))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg(errormsg, privilege_to_string(priv))));

			istmt.privileges |= priv;
		}
	}

	ExecGrantStmt_oids(&istmt);
}

/*
 * ExecGrantStmt_oids
 *
 * 授予和撤销权限的内部入口点。
 */
static void
ExecGrantStmt_oids(InternalGrant *istmt)
{
	switch (istmt->objtype)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
			ExecGrant_Relation(istmt);
			break;
		case OBJECT_DATABASE:
			ExecGrant_common(istmt, DatabaseRelationId, ACL_ALL_RIGHTS_DATABASE, NULL);
			break;
		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			ExecGrant_common(istmt, TypeRelationId, ACL_ALL_RIGHTS_TYPE, ExecGrant_Type_check);
			break;
		case OBJECT_FDW:
			ExecGrant_common(istmt, ForeignDataWrapperRelationId, ACL_ALL_RIGHTS_FDW, NULL);
			break;
		case OBJECT_FOREIGN_SERVER:
			ExecGrant_common(istmt, ForeignServerRelationId, ACL_ALL_RIGHTS_FOREIGN_SERVER, NULL);
			break;
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			ExecGrant_common(istmt, ProcedureRelationId, ACL_ALL_RIGHTS_FUNCTION, NULL);
			break;
		case OBJECT_LANGUAGE:
			ExecGrant_common(istmt, LanguageRelationId, ACL_ALL_RIGHTS_LANGUAGE, ExecGrant_Language_check);
			break;
		case OBJECT_LARGEOBJECT:
			ExecGrant_Largeobject(istmt);
			break;
		case OBJECT_SCHEMA:
			ExecGrant_common(istmt, NamespaceRelationId, ACL_ALL_RIGHTS_SCHEMA, NULL);
			break;
		case OBJECT_TABLESPACE:
			ExecGrant_common(istmt, TableSpaceRelationId, ACL_ALL_RIGHTS_TABLESPACE, NULL);
			break;
		case OBJECT_PARAMETER_ACL:
			ExecGrant_Parameter(istmt);
			break;
		default:
			elog(ERROR, "unrecognized GrantStmt.objtype: %d",
				 (int) istmt->objtype);
	}

	/*
	 * 把刚执行的 GRANT 信息传给事件触发器。注意
	 * 我们倾向于在实际执行之后进行，因为这样
	 * 能让函数有机会根据实际授予的权限
	 * 调整 istmt。
	 */
	if (EventTriggerSupportsObjectType(istmt->objtype))
		EventTriggerCollectGrant(istmt);
}

/*
 * objectNamesToOids
 *
 * 把给定类型的对象名列表转换为 Oid 列表。
 *
 * XXX：本函数不对所查找名称的对象施加任何
 * 锁。面对并发 DDL，我们很容易锁定到
 * 对象的旧版本，导致 GRANT 或 REVOKE 语句
 * 失败。
 */
static List *
objectNamesToOids(ObjectType objtype, List *objnames, bool is_grant)
{
	List	   *objects = NIL;
	ListCell   *cell;

	Assert(objnames != NIL);

	switch (objtype)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
			foreach(cell, objnames)
			{
				RangeVar   *relvar = (RangeVar *) lfirst(cell);
				Oid			relOid;

				relOid = RangeVarGetRelid(relvar, NoLock, false);
				objects = lappend_oid(objects, relOid);
			}
			break;
		case OBJECT_DATABASE:
			foreach(cell, objnames)
			{
				char	   *dbname = strVal(lfirst(cell));
				Oid			dbid;

				dbid = get_database_oid(dbname, false);
				objects = lappend_oid(objects, dbid);
			}
			break;
		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			foreach(cell, objnames)
			{
				List	   *typname = (List *) lfirst(cell);
				Oid			oid;

				oid = typenameTypeId(NULL, makeTypeNameFromNameList(typname));
				objects = lappend_oid(objects, oid);
			}
			break;
		case OBJECT_FUNCTION:
			foreach(cell, objnames)
			{
				ObjectWithArgs *func = (ObjectWithArgs *) lfirst(cell);
				Oid			funcid;

				funcid = LookupFuncWithArgs(OBJECT_FUNCTION, func, false);
				objects = lappend_oid(objects, funcid);
			}
			break;
		case OBJECT_LANGUAGE:
			foreach(cell, objnames)
			{
				char	   *langname = strVal(lfirst(cell));
				Oid			oid;

				oid = get_language_oid(langname, false);
				objects = lappend_oid(objects, oid);
			}
			break;
		case OBJECT_LARGEOBJECT:
			foreach(cell, objnames)
			{
				Oid			lobjOid = oidparse(lfirst(cell));

				if (!LargeObjectExists(lobjOid))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("large object %u does not exist",
									lobjOid)));

				objects = lappend_oid(objects, lobjOid);
			}
			break;
		case OBJECT_SCHEMA:
			foreach(cell, objnames)
			{
				char	   *nspname = strVal(lfirst(cell));
				Oid			oid;

				oid = get_namespace_oid(nspname, false);
				objects = lappend_oid(objects, oid);
			}
			break;
		case OBJECT_PROCEDURE:
			foreach(cell, objnames)
			{
				ObjectWithArgs *func = (ObjectWithArgs *) lfirst(cell);
				Oid			procid;

				procid = LookupFuncWithArgs(OBJECT_PROCEDURE, func, false);
				objects = lappend_oid(objects, procid);
			}
			break;
		case OBJECT_ROUTINE:
			foreach(cell, objnames)
			{
				ObjectWithArgs *func = (ObjectWithArgs *) lfirst(cell);
				Oid			routid;

				routid = LookupFuncWithArgs(OBJECT_ROUTINE, func, false);
				objects = lappend_oid(objects, routid);
			}
			break;
		case OBJECT_TABLESPACE:
			foreach(cell, objnames)
			{
				char	   *spcname = strVal(lfirst(cell));
				Oid			spcoid;

				spcoid = get_tablespace_oid(spcname, false);
				objects = lappend_oid(objects, spcoid);
			}
			break;
		case OBJECT_FDW:
			foreach(cell, objnames)
			{
				char	   *fdwname = strVal(lfirst(cell));
				Oid			fdwid = get_foreign_data_wrapper_oid(fdwname, false);

				objects = lappend_oid(objects, fdwid);
			}
			break;
		case OBJECT_FOREIGN_SERVER:
			foreach(cell, objnames)
			{
				char	   *srvname = strVal(lfirst(cell));
				Oid			srvid = get_foreign_server_oid(srvname, false);

				objects = lappend_oid(objects, srvid);
			}
			break;
		case OBJECT_PARAMETER_ACL:
			foreach(cell, objnames)
			{
				/*
				 * 在此代码中，我们用 GUC 在
				 * pg_parameter_acl 中条目的 OID 来表示该 GUC；
				 * 若尚不存在则须在此创建。（这确实是个
				 * hack，但它避免了与所有期望以 OID 表示
				 * 对象标识的 GRANT/REVOKE 基础设施纠缠。）
				 * 不过，若是 REVOKE，我们反而可以忽略任何
				 * 没有此类条目的 GUC，因为它们必然没有
				 * 需要移除的权限。
				 */
				char	   *parameter = strVal(lfirst(cell));
				Oid			parameterId = ParameterAclLookup(parameter, true);

				if (!OidIsValid(parameterId) && is_grant)
				{
					parameterId = ParameterAclCreate(parameter);

					/*
					 * 防止处理重复对象时出错，
					 * 并让新条目可见，以便
					 * ExecGrant_Parameter 能更新它。
					 */
					CommandCounterIncrement();
				}
				if (OidIsValid(parameterId))
					objects = lappend_oid(objects, parameterId);
			}
			break;
		default:
			elog(ERROR, "unrecognized GrantStmt.objtype: %d",
				 (int) objtype);
	}

	return objects;
}

/*
 * objectsInSchemaToOids
 *
 * 查找指定模式中给定类型的所有对象，并列出
 * 其 Oid。我们会检查模式的 USAGE 权限，但此处
 * 不对单个对象做权限检查。
 */
static List *
objectsInSchemaToOids(ObjectType objtype, List *nspnames)
{
	List	   *objects = NIL;
	ListCell   *cell;

	foreach(cell, nspnames)
	{
		char	   *nspname = strVal(lfirst(cell));
		Oid			namespaceId;
		List	   *objs;

		namespaceId = LookupExplicitNamespace(nspname, false);

		switch (objtype)
		{
			case OBJECT_TABLE:
				objs = getRelationsInNamespace(namespaceId, RELKIND_RELATION);
				objects = list_concat(objects, objs);
				objs = getRelationsInNamespace(namespaceId, RELKIND_VIEW);
				objects = list_concat(objects, objs);
				objs = getRelationsInNamespace(namespaceId, RELKIND_MATVIEW);
				objects = list_concat(objects, objs);
				objs = getRelationsInNamespace(namespaceId, RELKIND_FOREIGN_TABLE);
				objects = list_concat(objects, objs);
				objs = getRelationsInNamespace(namespaceId, RELKIND_PARTITIONED_TABLE);
				objects = list_concat(objects, objs);
				break;
			case OBJECT_SEQUENCE:
				objs = getRelationsInNamespace(namespaceId, RELKIND_SEQUENCE);
				objects = list_concat(objects, objs);
				break;
			case OBJECT_FUNCTION:
			case OBJECT_PROCEDURE:
			case OBJECT_ROUTINE:
				{
					ScanKeyData key[2];
					int			keycount;
					Relation	rel;
					TableScanDesc scan;
					HeapTuple	tuple;

					keycount = 0;
					ScanKeyInit(&key[keycount++],
								Anum_pg_proc_pronamespace,
								BTEqualStrategyNumber, F_OIDEQ,
								ObjectIdGetDatum(namespaceId));

					if (objtype == OBJECT_FUNCTION)
						/* 包括聚合函数和窗口函数 */
						ScanKeyInit(&key[keycount++],
									Anum_pg_proc_prokind,
									BTEqualStrategyNumber, F_CHARNE,
									CharGetDatum(PROKIND_PROCEDURE));
					else if (objtype == OBJECT_PROCEDURE)
						ScanKeyInit(&key[keycount++],
									Anum_pg_proc_prokind,
									BTEqualStrategyNumber, F_CHAREQ,
									CharGetDatum(PROKIND_PROCEDURE));

					rel = table_open(ProcedureRelationId, AccessShareLock);
					scan = table_beginscan_catalog(rel, keycount, key);

					while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
					{
						Oid			oid = ((Form_pg_proc) GETSTRUCT(tuple))->oid;

						objects = lappend_oid(objects, oid);
					}

					table_endscan(scan);
					table_close(rel, AccessShareLock);
				}
				break;
			default:
				/* 不应发生 */
				elog(ERROR, "unrecognized GrantStmt.objtype: %d",
					 (int) objtype);
		}
	}

	return objects;
}

/*
 * getRelationsInNamespace
 *
 * 返回给定命名空间中按关系类型过滤的关系 Oid 列表
 */
static List *
getRelationsInNamespace(Oid namespaceId, char relkind)
{
	List	   *relations = NIL;
	ScanKeyData key[2];
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key[0],
				Anum_pg_class_relnamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceId));
	ScanKeyInit(&key[1],
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(relkind));

	rel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 2, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			oid = ((Form_pg_class) GETSTRUCT(tuple))->oid;

		relations = lappend_oid(relations, oid);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return relations;
}


/*
 * ALTER DEFAULT PRIVILEGES 语句
 */
void
ExecAlterDefaultPrivilegesStmt(ParseState *pstate, AlterDefaultPrivilegesStmt *stmt)
{
	GrantStmt  *action = stmt->action;
	InternalDefaultACL iacls;
	ListCell   *cell;
	List	   *rolespecs = NIL;
	List	   *nspnames = NIL;
	DefElem    *drolespecs = NULL;
	DefElem    *dnspnames = NULL;
	AclMode		all_privileges;
	const char *errormsg;

	/* 解析语句的 "options" 部分 */
	foreach(cell, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(cell);

		if (strcmp(defel->defname, "schemas") == 0)
		{
			if (dnspnames)
				errorConflictingDefElem(defel, pstate);
			dnspnames = defel;
		}
		else if (strcmp(defel->defname, "roles") == 0)
		{
			if (drolespecs)
				errorConflictingDefElem(defel, pstate);
			drolespecs = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized", defel->defname);
	}

	if (dnspnames)
		nspnames = (List *) dnspnames->arg;
	if (drolespecs)
		rolespecs = (List *) drolespecs->arg;

	/* 准备语句的 InternalDefaultACL 表示 */
	/* roleid 将在下面填充 */
	/* nspid 将在 SetDefaultACLsInSchemas 中填充 */
	iacls.is_grant = action->is_grant;
	iacls.objtype = action->objtype;
	/* all_privs 将在下面填充 */
	/* privileges 将在下面填充 */
	iacls.grantees = NIL;		/* 在下面填充 */
	iacls.grant_option = action->grant_option;
	iacls.behavior = action->behavior;

	/*
	 * 把 RoleSpec 列表转换为 Oid 列表。注意此时我们
	 * 会在适当时插入 ACL_ID_PUBLIC，因此下游
	 * 支持该情形无需再做额外工作。
	 */
	foreach(cell, action->grantees)
	{
		RoleSpec   *grantee = (RoleSpec *) lfirst(cell);
		Oid			grantee_uid;

		switch (grantee->roletype)
		{
			case ROLESPEC_PUBLIC:
				grantee_uid = ACL_ID_PUBLIC;
				break;
			default:
				grantee_uid = get_rolespec_oid(grantee, false);
				break;
		}
		iacls.grantees = lappend_oid(iacls.grantees, grantee_uid);
	}

	/*
	 * 把 action->privileges（权限字符串列表）转换为
	 * AclMode 位掩码。
	 */
	switch (action->objtype)
	{
		case OBJECT_TABLE:
			all_privileges = ACL_ALL_RIGHTS_RELATION;
			errormsg = gettext_noop("invalid privilege type %s for relation");
			break;
		case OBJECT_SEQUENCE:
			all_privileges = ACL_ALL_RIGHTS_SEQUENCE;
			errormsg = gettext_noop("invalid privilege type %s for sequence");
			break;
		case OBJECT_FUNCTION:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for function");
			break;
		case OBJECT_PROCEDURE:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for procedure");
			break;
		case OBJECT_ROUTINE:
			all_privileges = ACL_ALL_RIGHTS_FUNCTION;
			errormsg = gettext_noop("invalid privilege type %s for routine");
			break;
		case OBJECT_TYPE:
			all_privileges = ACL_ALL_RIGHTS_TYPE;
			errormsg = gettext_noop("invalid privilege type %s for type");
			break;
		case OBJECT_SCHEMA:
			all_privileges = ACL_ALL_RIGHTS_SCHEMA;
			errormsg = gettext_noop("invalid privilege type %s for schema");
			break;
		default:
			elog(ERROR, "unrecognized GrantStmt.objtype: %d",
				 (int) action->objtype);
			/* 让编译器安静 */
			all_privileges = ACL_NO_RIGHTS;
			errormsg = NULL;
	}

	if (action->privileges == NIL)
	{
		iacls.all_privs = true;

		/*
		 * 内部例程会根据对象类型
		 * 将其转换为 ACL_ALL_RIGHTS_*
		 */
		iacls.privileges = ACL_NO_RIGHTS;
	}
	else
	{
		iacls.all_privs = false;
		iacls.privileges = ACL_NO_RIGHTS;

		foreach(cell, action->privileges)
		{
			AccessPriv *privnode = (AccessPriv *) lfirst(cell);
			AclMode		priv;

			if (privnode->cols)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("default privileges cannot be set for columns")));

			if (privnode->priv_name == NULL)	/* 解析器出错？ */
				elog(ERROR, "AccessPriv node must specify privilege");
			priv = string_to_privilege(privnode->priv_name);

			if (priv & ~((AclMode) all_privileges))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg(errormsg, privilege_to_string(priv))));

			iacls.privileges |= priv;
		}
	}

	if (rolespecs == NIL)
	{
		/* 为自己设置权限 */
		iacls.roleid = GetUserId();

		SetDefaultACLsInSchemas(&iacls, nspnames);
	}
	else
	{
		/* 查找角色 OID 并进行权限检查 */
		ListCell   *rolecell;

		foreach(rolecell, rolespecs)
		{
			RoleSpec   *rolespec = lfirst(rolecell);

			iacls.roleid = get_rolespec_oid(rolespec, false);

			if (!has_privs_of_role(GetUserId(), iacls.roleid))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to change default privileges")));

			SetDefaultACLsInSchemas(&iacls, nspnames);
		}
	}
}

/*
 * 为一组目标模式处理 ALTER DEFAULT PRIVILEGES
 *
 * *iacls 中除 nspid 外的所有字段均已填充
 */
static void
SetDefaultACLsInSchemas(InternalDefaultACL *iacls, List *nspnames)
{
	if (nspnames == NIL)
	{
		/* 若未指定模式，则设置数据库范围的权限 */
		iacls->nspid = InvalidOid;

		SetDefaultACL(iacls);
	}
	else
	{
		/* 查找模式 OID 并为每个模式设置权限 */
		ListCell   *nspcell;

		foreach(nspcell, nspnames)
		{
			char	   *nspname = strVal(lfirst(nspcell));

			iacls->nspid = get_namespace_oid(nspname, false);

			/*
			 * 我们过去坚持要求目标角色在该模式上拥有 CREATE
			 * 权限，因为没有该权限就无法创建
			 * 这些默认权限所适用的对象。
			 * 但事实证明这一检查带来的困惑多于帮助，
			 * 而且它还会导致某些数据库状态
			 * 无法转储/恢复，因为撤销 CREATE 并不会
			 * 使该模式的默认权限消失。因此现在我们只是
			 * 允许该 ALTER；若用户缺少 CREATE，
			 * 他会在尝试创建对象时发现。
			 */

			SetDefaultACL(iacls);
		}
	}
}


/*
 * 创建或更新 pg_default_acl 条目
 */
static void
SetDefaultACL(InternalDefaultACL *iacls)
{
	AclMode		this_privileges = iacls->privileges;
	char		objtype;
	Relation	rel;
	HeapTuple	tuple;
	bool		isNew;
	Acl		   *def_acl;
	Acl		   *old_acl;
	Acl		   *new_acl;
	HeapTuple	newtuple;
	int			noldmembers;
	int			nnewmembers;
	Oid		   *oldmembers;
	Oid		   *newmembers;

	rel = table_open(DefaultAclRelationId, RowExclusiveLock);

	/*
	 * 全局条目的默认值是特定对象类型的硬编码
	 * 默认 ACL。非全局条目的默认值是空
	 * ACL。必须如此，因为全局条目会替换硬编码
	 * 默认值，而其他条目则是附加其上。
	 */
	if (!OidIsValid(iacls->nspid))
		def_acl = acldefault(iacls->objtype, iacls->roleid);
	else
		def_acl = make_empty_acl();

	/*
	 * 把 ACL 对象类型转换为 pg_default_acl 对象类型，
	 * 并处理 all_privs 选项
	 */
	switch (iacls->objtype)
	{
		case OBJECT_TABLE:
			objtype = DEFACLOBJ_RELATION;
			if (iacls->all_privs && this_privileges == ACL_NO_RIGHTS)
				this_privileges = ACL_ALL_RIGHTS_RELATION;
			break;

		case OBJECT_SEQUENCE:
			objtype = DEFACLOBJ_SEQUENCE;
			if (iacls->all_privs && this_privileges == ACL_NO_RIGHTS)
				this_privileges = ACL_ALL_RIGHTS_SEQUENCE;
			break;

		case OBJECT_FUNCTION:
			objtype = DEFACLOBJ_FUNCTION;
			if (iacls->all_privs && this_privileges == ACL_NO_RIGHTS)
				this_privileges = ACL_ALL_RIGHTS_FUNCTION;
			break;

		case OBJECT_TYPE:
			objtype = DEFACLOBJ_TYPE;
			if (iacls->all_privs && this_privileges == ACL_NO_RIGHTS)
				this_privileges = ACL_ALL_RIGHTS_TYPE;
			break;

		case OBJECT_SCHEMA:
			if (OidIsValid(iacls->nspid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("cannot use IN SCHEMA clause when using GRANT/REVOKE ON SCHEMAS")));
			objtype = DEFACLOBJ_NAMESPACE;
			if (iacls->all_privs && this_privileges == ACL_NO_RIGHTS)
				this_privileges = ACL_ALL_RIGHTS_SCHEMA;
			break;

		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) iacls->objtype);
			objtype = 0;		/* 让编译器安静 */
			break;
	}

	/* 在目录中查找该对象类型的现有行 */
	tuple = SearchSysCache3(DEFACLROLENSPOBJ,
							ObjectIdGetDatum(iacls->roleid),
							ObjectIdGetDatum(iacls->nspid),
							CharGetDatum(objtype));

	if (HeapTupleIsValid(tuple))
	{
		Datum		aclDatum;
		bool		isNull;

		aclDatum = SysCacheGetAttr(DEFACLROLENSPOBJ, tuple,
								   Anum_pg_default_acl_defaclacl,
								   &isNull);
		if (!isNull)
			old_acl = DatumGetAclPCopy(aclDatum);
		else
			old_acl = NULL;		/* 这种情况大概不应发生 */
		isNew = false;
	}
	else
	{
		old_acl = NULL;
		isNew = true;
	}

	if (old_acl != NULL)
	{
		/*
		 * 我们需要新旧 ACL 两者的成员，以便修正
		 * 共享依赖信息。要在 merge_acl_with_grant
		 * 丢弃 old_acl 之前收集数据。
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);
	}
	else
	{
		/* 若无条目或条目为 null，则以默认 ACL 值开始 */
		old_acl = aclcopy(def_acl);
		/* 根据目录，没有旧的成员角色 */
		noldmembers = 0;
		oldmembers = NULL;
	}

	/*
	 * 生成新 ACL。权限的授权者始终与目标
	 * 角色相同。
	 */
	new_acl = merge_acl_with_grant(old_acl,
								   iacls->is_grant,
								   iacls->grant_option,
								   iacls->behavior,
								   iacls->grantees,
								   this_privileges,
								   iacls->roleid,
								   iacls->roleid);

	/*
	 * 若结果与默认值相同，则不需要显式的
	 * pg_default_acl 条目，实际上若该条目存在
	 * 还应将其删除。必须对两个数组排序才能正确比较。
	 */
	aclitemsort(new_acl);
	aclitemsort(def_acl);
	if (aclequal(new_acl, def_acl))
	{
		/* 若有旧条目，则删除它 */
		if (!isNew)
		{
			ObjectAddress myself;

			/*
			 * 依赖机制会负责移除所有
			 * 关联的依赖条目。我们使用 DROP_RESTRICT，
			 * 因为不应有任何东西依赖该条目。
			 */
			myself.classId = DefaultAclRelationId;
			myself.objectId = ((Form_pg_default_acl) GETSTRUCT(tuple))->oid;
			myself.objectSubId = 0;

			performDeletion(&myself, DROP_RESTRICT, 0);
		}
	}
	else
	{
		Datum		values[Natts_pg_default_acl] = {0};
		bool		nulls[Natts_pg_default_acl] = {0};
		bool		replaces[Natts_pg_default_acl] = {0};
		Oid			defAclOid;

		if (isNew)
		{
			/* 插入新条目 */
			defAclOid = GetNewOidWithIndex(rel, DefaultAclOidIndexId,
										   Anum_pg_default_acl_oid);
			values[Anum_pg_default_acl_oid - 1] = ObjectIdGetDatum(defAclOid);
			values[Anum_pg_default_acl_defaclrole - 1] = ObjectIdGetDatum(iacls->roleid);
			values[Anum_pg_default_acl_defaclnamespace - 1] = ObjectIdGetDatum(iacls->nspid);
			values[Anum_pg_default_acl_defaclobjtype - 1] = CharGetDatum(objtype);
			values[Anum_pg_default_acl_defaclacl - 1] = PointerGetDatum(new_acl);

			newtuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
			CatalogTupleInsert(rel, newtuple);
		}
		else
		{
			defAclOid = ((Form_pg_default_acl) GETSTRUCT(tuple))->oid;

			/* 更新现有条目 */
			values[Anum_pg_default_acl_defaclacl - 1] = PointerGetDatum(new_acl);
			replaces[Anum_pg_default_acl_defaclacl - 1] = true;

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
										 values, nulls, replaces);
			CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);
		}

		/* 更新时这些依赖不变 */
		if (isNew)
		{
			/* 对角色依赖 */
			recordDependencyOnOwner(DefaultAclRelationId, defAclOid,
									iacls->roleid);

			/* 对命名空间依赖 */
			if (OidIsValid(iacls->nspid))
			{
				ObjectAddress myself,
							referenced;

				myself.classId = DefaultAclRelationId;
				myself.objectId = defAclOid;
				myself.objectSubId = 0;

				referenced.classId = NamespaceRelationId;
				referenced.objectId = iacls->nspid;
				referenced.objectSubId = 0;

				recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
			}
		}

		/*
		 * 更新共享依赖 ACL 信息
		 */
		nnewmembers = aclmembers(new_acl, &newmembers);

		updateAclDependencies(DefaultAclRelationId,
							  defAclOid, 0,
							  iacls->roleid,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		if (isNew)
			InvokeObjectPostCreateHook(DefaultAclRelationId, defAclOid, 0);
		else
			InvokeObjectPostAlterHook(DefaultAclRelationId, defAclOid, 0);
	}

	if (HeapTupleIsValid(tuple))
		ReleaseSysCache(tuple);

	table_close(rel, RowExclusiveLock);

	/* 防止处理重复对象时出错 */
	CommandCounterIncrement();
}


/*
 * RemoveRoleFromObjectACL
 *
 * 供 shdepDropOwned 用来移除 ACL 中对某个角色的引用
 */
void
RemoveRoleFromObjectACL(Oid roleid, Oid classid, Oid objid)
{
	if (classid == DefaultAclRelationId)
	{
		InternalDefaultACL iacls;
		Form_pg_default_acl pg_default_acl_tuple;
		Relation	rel;
		ScanKeyData skey[1];
		SysScanDesc scan;
		HeapTuple	tuple;

		/* 先获取 SetDefaultACL 所需的信息 */
		rel = table_open(DefaultAclRelationId, AccessShareLock);

		ScanKeyInit(&skey[0],
					Anum_pg_default_acl_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objid));

		scan = systable_beginscan(rel, DefaultAclOidIndexId, true,
								  NULL, 1, skey);

		tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for default ACL %u", objid);

		pg_default_acl_tuple = (Form_pg_default_acl) GETSTRUCT(tuple);

		iacls.roleid = pg_default_acl_tuple->defaclrole;
		iacls.nspid = pg_default_acl_tuple->defaclnamespace;

		switch (pg_default_acl_tuple->defaclobjtype)
		{
			case DEFACLOBJ_RELATION:
				iacls.objtype = OBJECT_TABLE;
				break;
			case DEFACLOBJ_SEQUENCE:
				iacls.objtype = OBJECT_SEQUENCE;
				break;
			case DEFACLOBJ_FUNCTION:
				iacls.objtype = OBJECT_FUNCTION;
				break;
			case DEFACLOBJ_TYPE:
				iacls.objtype = OBJECT_TYPE;
				break;
			case DEFACLOBJ_NAMESPACE:
				iacls.objtype = OBJECT_SCHEMA;
				break;
			default:
				/* 不应到达此处 */
				elog(ERROR, "unexpected default ACL type: %d",
					 (int) pg_default_acl_tuple->defaclobjtype);
				break;
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);

		iacls.is_grant = false;
		iacls.all_privs = true;
		iacls.privileges = ACL_NO_RIGHTS;
		iacls.grantees = list_make1_oid(roleid);
		iacls.grant_option = false;
		iacls.behavior = DROP_CASCADE;

		/* 执行 */
		SetDefaultACL(&iacls);
	}
	else
	{
		InternalGrant istmt;

		switch (classid)
		{
			case RelationRelationId:
				/* 对序列使用 TABLE 是可以的 */
				istmt.objtype = OBJECT_TABLE;
				break;
			case DatabaseRelationId:
				istmt.objtype = OBJECT_DATABASE;
				break;
			case TypeRelationId:
				istmt.objtype = OBJECT_TYPE;
				break;
			case ProcedureRelationId:
				istmt.objtype = OBJECT_ROUTINE;
				break;
			case LanguageRelationId:
				istmt.objtype = OBJECT_LANGUAGE;
				break;
			case LargeObjectRelationId:
				istmt.objtype = OBJECT_LARGEOBJECT;
				break;
			case NamespaceRelationId:
				istmt.objtype = OBJECT_SCHEMA;
				break;
			case TableSpaceRelationId:
				istmt.objtype = OBJECT_TABLESPACE;
				break;
			case ForeignServerRelationId:
				istmt.objtype = OBJECT_FOREIGN_SERVER;
				break;
			case ForeignDataWrapperRelationId:
				istmt.objtype = OBJECT_FDW;
				break;
			case ParameterAclRelationId:
				istmt.objtype = OBJECT_PARAMETER_ACL;
				break;
			default:
				elog(ERROR, "unexpected object class %u", classid);
				break;
		}
		istmt.is_grant = false;
		istmt.objects = list_make1_oid(objid);
		istmt.all_privs = true;
		istmt.privileges = ACL_NO_RIGHTS;
		istmt.col_privs = NIL;
		istmt.grantees = list_make1_oid(roleid);
		istmt.grant_option = false;
		istmt.behavior = DROP_CASCADE;

		ExecGrantStmt_oids(&istmt);
	}
}


/*
 * expand_col_privileges
 *
 * 把指定的权限按位或进每个指定属性对应的
 * 按列数组项中。该按列数组的索引从
 * FirstLowInvalidHeapAttributeNumber 开始，直到关系的最后一个属性。
 */
static void
expand_col_privileges(List *colnames, Oid table_oid,
					  AclMode this_privileges,
					  AclMode *col_privileges,
					  int num_col_privileges)
{
	ListCell   *cell;

	foreach(cell, colnames)
	{
		char	   *colname = strVal(lfirst(cell));
		AttrNumber	attnum;

		attnum = get_attnum(table_oid, colname);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname, get_rel_name(table_oid))));
		attnum -= FirstLowInvalidHeapAttributeNumber;
		if (attnum <= 0 || attnum >= num_col_privileges)
			elog(ERROR, "column number out of range");	/* 安全检查 */
		col_privileges[attnum] |= this_privileges;
	}
}

/*
 * expand_all_col_privileges
 *
 * 把指定的权限按位或进关系每个有效属性对应的
 * 按列数组项中。该按列数组的索引从
 * FirstLowInvalidHeapAttributeNumber 开始，直到关系的最后一个属性。
 */
static void
expand_all_col_privileges(Oid table_oid, Form_pg_class classForm,
						  AclMode this_privileges,
						  AclMode *col_privileges,
						  int num_col_privileges)
{
	AttrNumber	curr_att;

	Assert(classForm->relnatts - FirstLowInvalidHeapAttributeNumber < num_col_privileges);
	for (curr_att = FirstLowInvalidHeapAttributeNumber + 1;
		 curr_att <= classForm->relnatts;
		 curr_att++)
	{
		HeapTuple	attTuple;
		bool		isdropped;

		if (curr_att == InvalidAttrNumber)
			continue;

		/* 视图根本没有系统列 */
		if (classForm->relkind == RELKIND_VIEW && curr_att < 0)
			continue;

		attTuple = SearchSysCache2(ATTNUM,
								   ObjectIdGetDatum(table_oid),
								   Int16GetDatum(curr_att));
		if (!HeapTupleIsValid(attTuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 curr_att, table_oid);

		isdropped = ((Form_pg_attribute) GETSTRUCT(attTuple))->attisdropped;

		ReleaseSysCache(attTuple);

		/* 忽略已删除的列 */
		if (isdropped)
			continue;

		col_privileges[curr_att - FirstLowInvalidHeapAttributeNumber] |= this_privileges;
	}
}

/*
 *	这里处理属性，但预期由 ExecGrant_Relation
 *	调用，而不是直接由 ExecuteGrantStmt 调用。
 */
static void
ExecGrant_Attribute(InternalGrant *istmt, Oid relOid, const char *relname,
					AttrNumber attnum, Oid ownerId, AclMode col_privileges,
					Relation attRelation, const Acl *old_rel_acl)
{
	HeapTuple	attr_tuple;
	Form_pg_attribute pg_attribute_tuple;
	Acl		   *old_acl;
	Acl		   *new_acl;
	Acl		   *merged_acl;
	Datum		aclDatum;
	bool		isNull;
	Oid			grantorId;
	AclMode		avail_goptions;
	bool		need_update;
	HeapTuple	newtuple;
	Datum		values[Natts_pg_attribute] = {0};
	bool		nulls[Natts_pg_attribute] = {0};
	bool		replaces[Natts_pg_attribute] = {0};
	int			noldmembers;
	int			nnewmembers;
	Oid		   *oldmembers;
	Oid		   *newmembers;

	attr_tuple = SearchSysCache2(ATTNUM,
								 ObjectIdGetDatum(relOid),
								 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(attr_tuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relOid);
	pg_attribute_tuple = (Form_pg_attribute) GETSTRUCT(attr_tuple);

	/*
	 * 获取现有 ACL 的工作副本。若没有 ACL，则替换为
	 * 适当的默认值。
	 */
	aclDatum = SysCacheGetAttr(ATTNUM, attr_tuple, Anum_pg_attribute_attacl,
							   &isNull);
	if (isNull)
	{
		old_acl = acldefault(OBJECT_COLUMN, ownerId);
		/* 根据目录，没有旧的成员角色 */
		noldmembers = 0;
		oldmembers = NULL;
	}
	else
	{
		old_acl = DatumGetAclPCopy(aclDatum);
		/* 获取现有 ACL 中提到的角色 */
		noldmembers = aclmembers(old_acl, &oldmembers);
	}

	/*
	 * 在 select_best_grantor 中，我们应考虑现有的表级 ACL 位
	 * 以及按列 ACL。构造一个新 ACL，作为二者的
	 * 拼接。（与无重复地正确合并相比这有点
	 * 粗劣，但这正是我们此处所需的。）
	 */
	merged_acl = aclconcat(old_rel_acl, old_acl);

	/* 确定以哪个 ID 执行授权，以及可用的授权选项 */
	select_best_grantor(GetUserId(), col_privileges,
						merged_acl, ownerId,
						&grantorId, &avail_goptions);

	pfree(merged_acl);

	/*
	 * 把权限限制为我们实际能授予的部分，并发出
	 * 标准要求的警告和错误消息。注意：我们不追踪
	 * 用户是否对每列实际使用了 ALL PRIVILEGES(columns)
	 * 语法；我们只是根据当前是否指定了所有可能的
	 * 权限来近似判断。由于 all_privs 标志只决定
	 * 是否发出警告，这样近似似乎足够。
	 */
	col_privileges =
		restrict_and_check_grant(istmt->is_grant, avail_goptions,
								 (col_privileges == ACL_ALL_RIGHTS_COLUMN),
								 col_privileges,
								 relOid, grantorId, OBJECT_COLUMN,
								 relname, attnum,
								 NameStr(pg_attribute_tuple->attname));

	/*
	 * 生成新 ACL。
	 */
	new_acl = merge_acl_with_grant(old_acl, istmt->is_grant,
								   istmt->grant_option,
								   istmt->behavior, istmt->grantees,
								   col_privileges, grantorId,
								   ownerId);

	/*
	 * 我们需要新旧 ACL 两者的成员，以便修正
	 * 共享依赖信息。
	 */
	nnewmembers = aclmembers(new_acl, &newmembers);

	/* 新 ACL 值已构建完成，现在插入它 */

	/*
	 * 若更新后的 ACL 为空，我们可以把 attacl 置为 null，甚至
	 * 可以避免更新 pg_attribute 行。这值得测试，因为
	 * 对任何关系级 REVOKE，我们会多次经过这里，
	 * 即使从未有过任何列 GRANT。注意我们假定列的
	 * "default" ACL 状态为空。
	 */
	if (ACL_NUM(new_acl) > 0)
	{
		values[Anum_pg_attribute_attacl - 1] = PointerGetDatum(new_acl);
		need_update = true;
	}
	else
	{
		nulls[Anum_pg_attribute_attacl - 1] = true;
		need_update = !isNull;
	}
	replaces[Anum_pg_attribute_attacl - 1] = true;

	if (need_update)
	{
		newtuple = heap_modify_tuple(attr_tuple, RelationGetDescr(attRelation),
									 values, nulls, replaces);

		CatalogTupleUpdate(attRelation, &newtuple->t_self, newtuple);

		/* 更新扩展的初始权限 */
		recordExtensionInitPriv(relOid, RelationRelationId, attnum,
								ACL_NUM(new_acl) > 0 ? new_acl : NULL);

		/* 更新共享依赖 ACL 信息 */
		updateAclDependencies(RelationRelationId, relOid, attnum,
							  ownerId,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);
	}

	pfree(new_acl);

	ReleaseSysCache(attr_tuple);
}

/*
 *	这里同时处理序列和非序列。
 */
static void
ExecGrant_Relation(InternalGrant *istmt)
{
	Relation	relation;
	Relation	attRelation;
	ListCell   *cell;

	relation = table_open(RelationRelationId, RowExclusiveLock);
	attRelation = table_open(AttributeRelationId, RowExclusiveLock);

	foreach(cell, istmt->objects)
	{
		Oid			relOid = lfirst_oid(cell);
		Datum		aclDatum;
		Form_pg_class pg_class_tuple;
		bool		isNull;
		AclMode		this_privileges;
		AclMode    *col_privileges;
		int			num_col_privileges;
		bool		have_col_privileges;
		Acl		   *old_acl;
		Acl		   *old_rel_acl;
		int			noldmembers;
		Oid		   *oldmembers;
		Oid			ownerId;
		HeapTuple	tuple;
		ListCell   *cell_colprivs;

		tuple = SearchSysCacheLocked1(RELOID, ObjectIdGetDatum(relOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relOid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		/* 在索引上授权没有意义 */
		if (pg_class_tuple->relkind == RELKIND_INDEX ||
			pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is an index",
							NameStr(pg_class_tuple->relname))));

		/* 复合类型也不是表 */
		if (pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							NameStr(pg_class_tuple->relname))));

		/* 在非序列上使用了 GRANT SEQUENCE？ */
		if (istmt->objtype == OBJECT_SEQUENCE &&
			pg_class_tuple->relkind != RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a sequence",
							NameStr(pg_class_tuple->relname))));

		/* 根据对象类型调整默认权限 */
		if (istmt->all_privs && istmt->privileges == ACL_NO_RIGHTS)
		{
			if (pg_class_tuple->relkind == RELKIND_SEQUENCE)
				this_privileges = ACL_ALL_RIGHTS_SEQUENCE;
			else
				this_privileges = ACL_ALL_RIGHTS_RELATION;
		}
		else
			this_privileges = istmt->privileges;

		/*
		 * GRANT TABLE 语法可用于序列和非序列，
		 * 因此我们必须查看 relkind 以确定所支持的
		 * 权限。表权限与序列权限的按位或已经
		 * 检查过了。
		 */
		if (istmt->objtype == OBJECT_TABLE)
		{
			if (pg_class_tuple->relkind == RELKIND_SEQUENCE)
			{
				/*
				 * 为向后兼容，在使用非序列
				 * GRANT 语法时，对无效的序列权限
				 * 仅发出警告。
				 */
				if (this_privileges & ~((AclMode) ACL_ALL_RIGHTS_SEQUENCE))
				{
					/*
					 * 指出对象名，因为用户需要知道
					 * 哪些操作成功了。这是必需的，因为
					 * WARNING 允许命令继续执行。
					 */
					ereport(WARNING,
							(errcode(ERRCODE_INVALID_GRANT_OPERATION),
							 errmsg("sequence \"%s\" only supports USAGE, SELECT, and UPDATE privileges",
									NameStr(pg_class_tuple->relname))));
					this_privileges &= (AclMode) ACL_ALL_RIGHTS_SEQUENCE;
				}
			}
			else
			{
				if (this_privileges & ~((AclMode) ACL_ALL_RIGHTS_RELATION))
				{
					/*
					 * USAGE 是序列支持而非序列不支持的唯一
					 * 权限。不要指出对象名，
					 * 因为我们在 TABLE | SEQUENCE 组合
					 * 检查中也没有指出。
					 */
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_GRANT_OPERATION),
							 errmsg("invalid privilege type %s for table",
									"USAGE")));
				}
			}
		}

		/*
		 * 建立数组，用于累积需要修改的任何列权限
		 * 位。该数组的索引方式使得项 [0]
		 * 对应 FirstLowInvalidHeapAttributeNumber。
		 */
		num_col_privileges = pg_class_tuple->relnatts - FirstLowInvalidHeapAttributeNumber + 1;
		col_privileges = (AclMode *) palloc0(num_col_privileges * sizeof(AclMode));
		have_col_privileges = false;

		/*
		 * 若我们撤销的关系权限同时也是列
		 * 权限，则按 SQL 标准，我们必须隐式地从每列
		 * 也撤销它们。（在 GRANT 期间我们无需隐式添加
		 * 列权限，因为权限检查代码始终会检查
		 * 关系权限和按列权限两者。）
		 */
		if (!istmt->is_grant &&
			(this_privileges & ACL_ALL_RIGHTS_COLUMN) != 0)
		{
			expand_all_col_privileges(relOid, pg_class_tuple,
									  this_privileges & ACL_ALL_RIGHTS_COLUMN,
									  col_privileges,
									  num_col_privileges);
			have_col_privileges = true;
		}

		/*
		 * 获取现有 ACL 的工作副本。若没有 ACL，则替换为
		 * 适当的默认值。
		 */
		ownerId = pg_class_tuple->relowner;
		aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
								   &isNull);
		if (isNull)
		{
			switch (pg_class_tuple->relkind)
			{
				case RELKIND_SEQUENCE:
					old_acl = acldefault(OBJECT_SEQUENCE, ownerId);
					break;
				default:
					old_acl = acldefault(OBJECT_TABLE, ownerId);
					break;
			}
			/* 根据目录，没有旧的成员角色 */
			noldmembers = 0;
			oldmembers = NULL;
		}
		else
		{
			old_acl = DatumGetAclPCopy(aclDatum);
			/* 获取现有 ACL 中提到的角色 */
			noldmembers = aclmembers(old_acl, &oldmembers);
		}

		/* 需要为列处理保留一份原始关系 ACL 的副本 */
		old_rel_acl = aclcopy(old_acl);

		/*
		 * 处理关系级权限（若有指定）
		 */
		if (this_privileges != ACL_NO_RIGHTS)
		{
			AclMode		avail_goptions;
			Acl		   *new_acl;
			Oid			grantorId;
			HeapTuple	newtuple;
			Datum		values[Natts_pg_class] = {0};
			bool		nulls[Natts_pg_class] = {0};
			bool		replaces[Natts_pg_class] = {0};
			int			nnewmembers;
			Oid		   *newmembers;
			ObjectType	objtype;

			/* 确定以哪个 ID 执行授权，以及可用的授权选项 */
			select_best_grantor(GetUserId(), this_privileges,
								old_acl, ownerId,
								&grantorId, &avail_goptions);

			switch (pg_class_tuple->relkind)
			{
				case RELKIND_SEQUENCE:
					objtype = OBJECT_SEQUENCE;
					break;
				default:
					objtype = OBJECT_TABLE;
					break;
			}

			/*
			 * 把权限限制为我们实际能授予的部分，
			 * 并发出标准要求的警告和错误消息。
			 */
			this_privileges =
				restrict_and_check_grant(istmt->is_grant, avail_goptions,
										 istmt->all_privs, this_privileges,
										 relOid, grantorId, objtype,
										 NameStr(pg_class_tuple->relname),
										 0, NULL);

			/*
			 * 生成新 ACL。
			 */
			new_acl = merge_acl_with_grant(old_acl,
										   istmt->is_grant,
										   istmt->grant_option,
										   istmt->behavior,
										   istmt->grantees,
										   this_privileges,
										   grantorId,
										   ownerId);

			/*
			 * 我们需要新旧 ACL 两者的成员，以便修正
			 * 共享依赖信息。
			 */
			nnewmembers = aclmembers(new_acl, &newmembers);

			/* 新 ACL 值已构建完成，现在插入它 */
			replaces[Anum_pg_class_relacl - 1] = true;
			values[Anum_pg_class_relacl - 1] = PointerGetDatum(new_acl);

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation),
										 values, nulls, replaces);

			CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);
			UnlockTuple(relation, &tuple->t_self, InplaceUpdateTupleLock);

			/* 更新扩展的初始权限 */
			recordExtensionInitPriv(relOid, RelationRelationId, 0, new_acl);

			/* 更新共享依赖 ACL 信息 */
			updateAclDependencies(RelationRelationId, relOid, 0,
								  ownerId,
								  noldmembers, oldmembers,
								  nnewmembers, newmembers);

			pfree(new_acl);
		}
		else
			UnlockTuple(relation, &tuple->t_self, InplaceUpdateTupleLock);

		/*
		 * 处理列级权限（若有指定或隐含）。
		 * 我们先把用户指定的列权限展开到
		 * 数组中，然后遍历所有非空数组项。
		 */
		foreach(cell_colprivs, istmt->col_privs)
		{
			AccessPriv *col_privs = (AccessPriv *) lfirst(cell_colprivs);

			if (col_privs->priv_name == NULL)
				this_privileges = ACL_ALL_RIGHTS_COLUMN;
			else
				this_privileges = string_to_privilege(col_privs->priv_name);

			if (this_privileges & ~((AclMode) ACL_ALL_RIGHTS_COLUMN))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for column",
								privilege_to_string(this_privileges))));

			if (pg_class_tuple->relkind == RELKIND_SEQUENCE &&
				this_privileges & ~((AclMode) ACL_SELECT))
			{
				/*
				 * 序列上唯一允许的列权限是 SELECT。
				 * 这是警告而非错误，因为我们
				 * 对关系级权限也是这样做的。
				 */
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("sequence \"%s\" only supports SELECT column privileges",
								NameStr(pg_class_tuple->relname))));

				this_privileges &= (AclMode) ACL_SELECT;
			}

			expand_col_privileges(col_privs->cols, relOid,
								  this_privileges,
								  col_privileges,
								  num_col_privileges);
			have_col_privileges = true;
		}

		if (have_col_privileges)
		{
			AttrNumber	i;

			for (i = 0; i < num_col_privileges; i++)
			{
				if (col_privileges[i] == ACL_NO_RIGHTS)
					continue;
				ExecGrant_Attribute(istmt,
									relOid,
									NameStr(pg_class_tuple->relname),
									i + FirstLowInvalidHeapAttributeNumber,
									ownerId,
									col_privileges[i],
									attRelation,
									old_rel_acl);
			}
		}

		pfree(old_rel_acl);
		pfree(col_privileges);

		ReleaseSysCache(tuple);

		/* 防止处理重复对象时出错 */
		CommandCounterIncrement();
	}

	table_close(attRelation, RowExclusiveLock);
	table_close(relation, RowExclusiveLock);
}

static void
ExecGrant_common(InternalGrant *istmt, Oid classid, AclMode default_privs,
				 void (*object_check) (InternalGrant *istmt, HeapTuple tuple))
{
	int			cacheid;
	Relation	relation;
	ListCell   *cell;

	if (istmt->all_privs && istmt->privileges == ACL_NO_RIGHTS)
		istmt->privileges = default_privs;

	cacheid = get_object_catcache_oid(classid);

	relation = table_open(classid, RowExclusiveLock);

	foreach(cell, istmt->objects)
	{
		Oid			objectid = lfirst_oid(cell);
		Datum		aclDatum;
		Datum		nameDatum;
		bool		isNull;
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	tuple;
		HeapTuple	newtuple;
		Datum	   *values = palloc0_array(Datum, RelationGetDescr(relation)->natts);
		bool	   *nulls = palloc0_array(bool, RelationGetDescr(relation)->natts);
		bool	   *replaces = palloc0_array(bool, RelationGetDescr(relation)->natts);
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		tuple = SearchSysCacheLocked1(cacheid, ObjectIdGetDatum(objectid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for %s %u", get_object_class_descr(classid), objectid);

		/*
		 * 针对特定对象类型的附加检查
		 */
		if (object_check)
			object_check(istmt, tuple);

		/*
		 * 获取现有 ACL 的工作副本。若没有 ACL，则替换为
		 * 适当的默认值。
		 */
		ownerId = DatumGetObjectId(SysCacheGetAttrNotNull(cacheid,
														  tuple,
														  get_object_attnum_owner(classid)));
		aclDatum = SysCacheGetAttr(cacheid,
								   tuple,
								   get_object_attnum_acl(classid),
								   &isNull);
		if (isNull)
		{
			old_acl = acldefault(get_object_type(classid, objectid), ownerId);
			/* 根据目录，没有旧的成员角色 */
			noldmembers = 0;
			oldmembers = NULL;
		}
		else
		{
			old_acl = DatumGetAclPCopy(aclDatum);
			/* 获取现有 ACL 中提到的角色 */
			noldmembers = aclmembers(old_acl, &oldmembers);
		}

		/* 确定以哪个 ID 执行授权，以及可用的授权选项 */
		select_best_grantor(GetUserId(), istmt->privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		nameDatum = SysCacheGetAttrNotNull(cacheid, tuple,
										   get_object_attnum_name(classid));

		/*
		 * 把权限限制为我们实际能授予的部分，
		 * 并发出标准要求的警告和错误消息。
		 */
		this_privileges =
			restrict_and_check_grant(istmt->is_grant, avail_goptions,
									 istmt->all_privs, istmt->privileges,
									 objectid, grantorId, get_object_type(classid, objectid),
									 NameStr(*DatumGetName(nameDatum)),
									 0, NULL);

		/*
		 * 生成新 ACL。
		 */
		new_acl = merge_acl_with_grant(old_acl, istmt->is_grant,
									   istmt->grant_option, istmt->behavior,
									   istmt->grantees, this_privileges,
									   grantorId, ownerId);

		/*
		 * 我们需要新旧 ACL 两者的成员，以便修正
		 * 共享依赖信息。
		 */
		nnewmembers = aclmembers(new_acl, &newmembers);

		/* 新 ACL 值已构建完成，现在插入它 */
		replaces[get_object_attnum_acl(classid) - 1] = true;
		values[get_object_attnum_acl(classid) - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation), values,
									 nulls, replaces);

		CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);
		UnlockTuple(relation, &tuple->t_self, InplaceUpdateTupleLock);

		/* 更新扩展的初始权限 */
		recordExtensionInitPriv(objectid, classid, 0, new_acl);

		/* 更新共享依赖 ACL 信息 */
		updateAclDependencies(classid,
							  objectid, 0,
							  ownerId,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);

		pfree(new_acl);

		/* 防止处理重复对象时出错 */
		CommandCounterIncrement();
	}

	table_close(relation, RowExclusiveLock);
}

static void
ExecGrant_Language_check(InternalGrant *istmt, HeapTuple tuple)
{
	Form_pg_language pg_language_tuple;

	pg_language_tuple = (Form_pg_language) GETSTRUCT(tuple);

	if (!pg_language_tuple->lanpltrusted)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("language \"%s\" is not trusted",
						NameStr(pg_language_tuple->lanname)),
				 errdetail("GRANT and REVOKE are not allowed on untrusted languages, "
						   "because only superusers can use untrusted languages.")));
}

static void
ExecGrant_Largeobject(InternalGrant *istmt)
{
	Relation	relation;
	ListCell   *cell;

	if (istmt->all_privs && istmt->privileges == ACL_NO_RIGHTS)
		istmt->privileges = ACL_ALL_RIGHTS_LARGEOBJECT;

	relation = table_open(LargeObjectMetadataRelationId,
						  RowExclusiveLock);

	foreach(cell, istmt->objects)
	{
		Oid			loid = lfirst_oid(cell);
		Form_pg_largeobject_metadata form_lo_meta;
		char		loname[NAMEDATALEN];
		Datum		aclDatum;
		bool		isNull;
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_largeobject_metadata] = {0};
		bool		nulls[Natts_pg_largeobject_metadata] = {0};
		bool		replaces[Natts_pg_largeobject_metadata] = {0};
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;
		ScanKeyData entry[1];
		SysScanDesc scan;
		HeapTuple	tuple;

		/* pg_largeobject_metadata 没有系统缓存 */
		ScanKeyInit(&entry[0],
					Anum_pg_largeobject_metadata_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(loid));

		scan = systable_beginscan(relation,
								  LargeObjectMetadataOidIndexId, true,
								  NULL, 1, entry);

		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for large object %u", loid);

		form_lo_meta = (Form_pg_largeobject_metadata) GETSTRUCT(tuple);

		/*
		 * 获取现有 ACL 的工作副本。若没有 ACL，则替换为
		 * 适当的默认值。
		 */
		ownerId = form_lo_meta->lomowner;
		aclDatum = heap_getattr(tuple,
								Anum_pg_largeobject_metadata_lomacl,
								RelationGetDescr(relation), &isNull);
		if (isNull)
		{
			old_acl = acldefault(OBJECT_LARGEOBJECT, ownerId);
			/* 根据目录，没有旧的成员角色 */
			noldmembers = 0;
			oldmembers = NULL;
		}
		else
		{
			old_acl = DatumGetAclPCopy(aclDatum);
			/* 获取现有 ACL 中提到的角色 */
			noldmembers = aclmembers(old_acl, &oldmembers);
		}

		/* 确定以哪个 ID 执行授权，以及可用的授权选项 */
		select_best_grantor(GetUserId(), istmt->privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * 把权限限制为我们实际能授予的部分，
		 * 并发出标准要求的警告和错误消息。
		 */
		snprintf(loname, sizeof(loname), "large object %u", loid);
		this_privileges =
			restrict_and_check_grant(istmt->is_grant, avail_goptions,
									 istmt->all_privs, istmt->privileges,
									 loid, grantorId, OBJECT_LARGEOBJECT,
									 loname, 0, NULL);

		/*
		 * 生成新 ACL。
		 */
		new_acl = merge_acl_with_grant(old_acl, istmt->is_grant,
									   istmt->grant_option, istmt->behavior,
									   istmt->grantees, this_privileges,
									   grantorId, ownerId);

		/*
		 * 我们需要新旧 ACL 两者的成员，以便修正
		 * 共享依赖信息。
		 */
		nnewmembers = aclmembers(new_acl, &newmembers);

		/* 新 ACL 值已构建完成，现在插入它 */
		replaces[Anum_pg_largeobject_metadata_lomacl - 1] = true;
		values[Anum_pg_largeobject_metadata_lomacl - 1]
			= PointerGetDatum(new_acl);

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation),
									 values, nulls, replaces);

		CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);

		/* 更新扩展的初始权限 */
		recordExtensionInitPriv(loid, LargeObjectRelationId, 0, new_acl);

		/* 更新共享依赖 ACL 信息 */
		updateAclDependencies(LargeObjectRelationId,
							  form_lo_meta->oid, 0,
							  ownerId,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		systable_endscan(scan);

		pfree(new_acl);

		/* 防止处理重复对象时出错 */
		CommandCounterIncrement();
	}

	table_close(relation, RowExclusiveLock);
}

static void
ExecGrant_Type_check(InternalGrant *istmt, HeapTuple tuple)
{
	Form_pg_type pg_type_tuple;

	pg_type_tuple = (Form_pg_type) GETSTRUCT(tuple);

	if (IsTrueArrayType(pg_type_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_GRANT_OPERATION),
				 errmsg("cannot set privileges of array types"),
				 errhint("Set the privileges of the element type instead.")));

	/* 在非域上使用了 GRANT DOMAIN？ */
	if (istmt->objtype == OBJECT_DOMAIN &&
		pg_type_tuple->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a domain",
						NameStr(pg_type_tuple->typname))));
}

static void
ExecGrant_Parameter(InternalGrant *istmt)
{
	Relation	relation;
	ListCell   *cell;

	if (istmt->all_privs && istmt->privileges == ACL_NO_RIGHTS)
		istmt->privileges = ACL_ALL_RIGHTS_PARAMETER_ACL;

	relation = table_open(ParameterAclRelationId, RowExclusiveLock);

	foreach(cell, istmt->objects)
	{
		Oid			parameterId = lfirst_oid(cell);
		Datum		nameDatum;
		const char *parname;
		Datum		aclDatum;
		bool		isNull;
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	tuple;
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		tuple = SearchSysCache1(PARAMETERACLOID, ObjectIdGetDatum(parameterId));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for parameter ACL %u",
				 parameterId);

		/* 我们需要 GUC 的名称 */
		nameDatum = SysCacheGetAttrNotNull(PARAMETERACLOID, tuple,
										   Anum_pg_parameter_acl_parname);
		parname = TextDatumGetCString(nameDatum);

		/* 将所有参数视为属于 bootstrap 超级用户。 */
		ownerId = BOOTSTRAP_SUPERUSERID;

		/*
		 * 获取现有 ACL 的工作副本。若没有 ACL，则替换为
		 * 适当的默认值。
		 */
		aclDatum = SysCacheGetAttr(PARAMETERACLOID, tuple,
								   Anum_pg_parameter_acl_paracl,
								   &isNull);

		if (isNull)
		{
			old_acl = acldefault(istmt->objtype, ownerId);
			/* 根据目录，没有旧的成员角色 */
			noldmembers = 0;
			oldmembers = NULL;
		}
		else
		{
			old_acl = DatumGetAclPCopy(aclDatum);
			/* 获取现有 ACL 中提到的角色 */
			noldmembers = aclmembers(old_acl, &oldmembers);
		}

		/* 确定以哪个 ID 执行授权，以及可用的授权选项 */
		select_best_grantor(GetUserId(), istmt->privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * 把权限限制为我们实际能授予的部分，
		 * 并发出标准要求的警告和错误消息。
		 */
		this_privileges =
			restrict_and_check_grant(istmt->is_grant, avail_goptions,
									 istmt->all_privs, istmt->privileges,
									 parameterId, grantorId,
									 OBJECT_PARAMETER_ACL,
									 parname,
									 0, NULL);

		/*
		 * 生成新 ACL。
		 */
		new_acl = merge_acl_with_grant(old_acl, istmt->is_grant,
									   istmt->grant_option, istmt->behavior,
									   istmt->grantees, this_privileges,
									   grantorId, ownerId);

		/*
		 * 我们需要新旧 ACL 两者的成员，以便修正
		 * 共享依赖信息。
		 */
		nnewmembers = aclmembers(new_acl, &newmembers);

		/*
		 * 若新 ACL 等于默认值，我们就不再需要该目录
		 * 条目。应删除它而不是更新它，以避免
		 * 留下退化的条目。
		 */
		if (aclequal(new_acl, acldefault(istmt->objtype, ownerId)))
		{
			CatalogTupleDelete(relation, &tuple->t_self);
		}
		else
		{
			/* 新 ACL 值已构建完成，现在插入它 */
			HeapTuple	newtuple;
			Datum		values[Natts_pg_parameter_acl] = {0};
			bool		nulls[Natts_pg_parameter_acl] = {0};
			bool		replaces[Natts_pg_parameter_acl] = {0};

			replaces[Anum_pg_parameter_acl_paracl - 1] = true;
			values[Anum_pg_parameter_acl_paracl - 1] = PointerGetDatum(new_acl);

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation),
										 values, nulls, replaces);

			CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);
		}

		/* 更新扩展的初始权限 */
		recordExtensionInitPriv(parameterId, ParameterAclRelationId, 0,
								new_acl);

		/* 更新共享依赖 ACL 信息 */
		updateAclDependencies(ParameterAclRelationId, parameterId, 0,
							  ownerId,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);
		pfree(new_acl);

		/* 防止处理重复对象时出错 */
		CommandCounterIncrement();
	}

	table_close(relation, RowExclusiveLock);
}


static AclMode
string_to_privilege(const char *privname)
{
	if (strcmp(privname, "insert") == 0)
		return ACL_INSERT;
	if (strcmp(privname, "select") == 0)
		return ACL_SELECT;
	if (strcmp(privname, "update") == 0)
		return ACL_UPDATE;
	if (strcmp(privname, "delete") == 0)
		return ACL_DELETE;
	if (strcmp(privname, "truncate") == 0)
		return ACL_TRUNCATE;
	if (strcmp(privname, "references") == 0)
		return ACL_REFERENCES;
	if (strcmp(privname, "trigger") == 0)
		return ACL_TRIGGER;
	if (strcmp(privname, "execute") == 0)
		return ACL_EXECUTE;
	if (strcmp(privname, "usage") == 0)
		return ACL_USAGE;
	if (strcmp(privname, "create") == 0)
		return ACL_CREATE;
	if (strcmp(privname, "temporary") == 0)
		return ACL_CREATE_TEMP;
	if (strcmp(privname, "temp") == 0)
		return ACL_CREATE_TEMP;
	if (strcmp(privname, "connect") == 0)
		return ACL_CONNECT;
	if (strcmp(privname, "set") == 0)
		return ACL_SET;
	if (strcmp(privname, "alter system") == 0)
		return ACL_ALTER_SYSTEM;
	if (strcmp(privname, "rule") == 0)
		return 0;				/* 忽略旧的 RULE 权限 */
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("unrecognized privilege type \"%s\"", privname)));
	return 0;					/* 安抚编译器 */
}

static const char *
privilege_to_string(AclMode privilege)
{
	switch (privilege)
	{
		case ACL_INSERT:
			return "INSERT";
		case ACL_SELECT:
			return "SELECT";
		case ACL_UPDATE:
			return "UPDATE";
		case ACL_DELETE:
			return "DELETE";
		case ACL_TRUNCATE:
			return "TRUNCATE";
		case ACL_REFERENCES:
			return "REFERENCES";
		case ACL_TRIGGER:
			return "TRIGGER";
		case ACL_EXECUTE:
			return "EXECUTE";
		case ACL_USAGE:
			return "USAGE";
		case ACL_CREATE:
			return "CREATE";
		case ACL_CREATE_TEMP:
			return "TEMP";
		case ACL_CONNECT:
			return "CONNECT";
		case ACL_SET:
			return "SET";
		case ACL_ALTER_SYSTEM:
			return "ALTER SYSTEM";
		default:
			elog(ERROR, "unrecognized privilege: %d", (int) privilege);
	}
	return NULL;				/* 安抚编译器 */
}

/*
 * aclcheck 权限失败的标准报告。
 *
 * 注意：我们不给下面的 %s 加双引号，因为许多调用者
 * 提供的字符串可能已经带引号。
 */
void
aclcheck_error(AclResult aclerr, ObjectType objtype,
			   const char *objectname)
{
	switch (aclerr)
	{
		case ACLCHECK_OK:
			/* 无错误，返回给调用者 */
			break;
		case ACLCHECK_NO_PRIV:
			{
				const char *msg = "???";

				switch (objtype)
				{
					case OBJECT_AGGREGATE:
						msg = gettext_noop("permission denied for aggregate %s");
						break;
					case OBJECT_COLLATION:
						msg = gettext_noop("permission denied for collation %s");
						break;
					case OBJECT_COLUMN:
						msg = gettext_noop("permission denied for column %s");
						break;
					case OBJECT_CONVERSION:
						msg = gettext_noop("permission denied for conversion %s");
						break;
					case OBJECT_DATABASE:
						msg = gettext_noop("permission denied for database %s");
						break;
					case OBJECT_DOMAIN:
						msg = gettext_noop("permission denied for domain %s");
						break;
					case OBJECT_EVENT_TRIGGER:
						msg = gettext_noop("permission denied for event trigger %s");
						break;
					case OBJECT_EXTENSION:
						msg = gettext_noop("permission denied for extension %s");
						break;
					case OBJECT_FDW:
						msg = gettext_noop("permission denied for foreign-data wrapper %s");
						break;
					case OBJECT_FOREIGN_SERVER:
						msg = gettext_noop("permission denied for foreign server %s");
						break;
					case OBJECT_FOREIGN_TABLE:
						msg = gettext_noop("permission denied for foreign table %s");
						break;
					case OBJECT_FUNCTION:
						msg = gettext_noop("permission denied for function %s");
						break;
					case OBJECT_INDEX:
						msg = gettext_noop("permission denied for index %s");
						break;
					case OBJECT_LANGUAGE:
						msg = gettext_noop("permission denied for language %s");
						break;
					case OBJECT_LARGEOBJECT:
						msg = gettext_noop("permission denied for large object %s");
						break;
					case OBJECT_MATVIEW:
						msg = gettext_noop("permission denied for materialized view %s");
						break;
					case OBJECT_OPCLASS:
						msg = gettext_noop("permission denied for operator class %s");
						break;
					case OBJECT_OPERATOR:
						msg = gettext_noop("permission denied for operator %s");
						break;
					case OBJECT_OPFAMILY:
						msg = gettext_noop("permission denied for operator family %s");
						break;
					case OBJECT_PARAMETER_ACL:
						msg = gettext_noop("permission denied for parameter %s");
						break;
					case OBJECT_POLICY:
						msg = gettext_noop("permission denied for policy %s");
						break;
					case OBJECT_PROCEDURE:
						msg = gettext_noop("permission denied for procedure %s");
						break;
					case OBJECT_PUBLICATION:
						msg = gettext_noop("permission denied for publication %s");
						break;
					case OBJECT_ROUTINE:
						msg = gettext_noop("permission denied for routine %s");
						break;
					case OBJECT_SCHEMA:
						msg = gettext_noop("permission denied for schema %s");
						break;
					case OBJECT_SEQUENCE:
						msg = gettext_noop("permission denied for sequence %s");
						break;
					case OBJECT_STATISTIC_EXT:
						msg = gettext_noop("permission denied for statistics object %s");
						break;
					case OBJECT_SUBSCRIPTION:
						msg = gettext_noop("permission denied for subscription %s");
						break;
					case OBJECT_TABLE:
						msg = gettext_noop("permission denied for table %s");
						break;
					case OBJECT_TABLESPACE:
						msg = gettext_noop("permission denied for tablespace %s");
						break;
					case OBJECT_TSCONFIGURATION:
						msg = gettext_noop("permission denied for text search configuration %s");
						break;
					case OBJECT_TSDICTIONARY:
						msg = gettext_noop("permission denied for text search dictionary %s");
						break;
					case OBJECT_TYPE:
						msg = gettext_noop("permission denied for type %s");
						break;
					case OBJECT_VIEW:
						msg = gettext_noop("permission denied for view %s");
						break;
						/* 这些目前未被使用 */
					case OBJECT_ACCESS_METHOD:
					case OBJECT_AMOP:
					case OBJECT_AMPROC:
					case OBJECT_ATTRIBUTE:
					case OBJECT_CAST:
					case OBJECT_DEFAULT:
					case OBJECT_DEFACL:
					case OBJECT_DOMCONSTRAINT:
					case OBJECT_PUBLICATION_NAMESPACE:
					case OBJECT_PUBLICATION_REL:
					case OBJECT_ROLE:
					case OBJECT_RULE:
					case OBJECT_TABCONSTRAINT:
					case OBJECT_TRANSFORM:
					case OBJECT_TRIGGER:
					case OBJECT_TSPARSER:
					case OBJECT_TSTEMPLATE:
					case OBJECT_USER_MAPPING:
						elog(ERROR, "unsupported object type: %d", objtype);
				}

				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg(msg, objectname)));
				break;
			}
		case ACLCHECK_NOT_OWNER:
			{
				const char *msg = "???";

				switch (objtype)
				{
					case OBJECT_AGGREGATE:
						msg = gettext_noop("must be owner of aggregate %s");
						break;
					case OBJECT_COLLATION:
						msg = gettext_noop("must be owner of collation %s");
						break;
					case OBJECT_CONVERSION:
						msg = gettext_noop("must be owner of conversion %s");
						break;
					case OBJECT_DATABASE:
						msg = gettext_noop("must be owner of database %s");
						break;
					case OBJECT_DOMAIN:
						msg = gettext_noop("must be owner of domain %s");
						break;
					case OBJECT_EVENT_TRIGGER:
						msg = gettext_noop("must be owner of event trigger %s");
						break;
					case OBJECT_EXTENSION:
						msg = gettext_noop("must be owner of extension %s");
						break;
					case OBJECT_FDW:
						msg = gettext_noop("must be owner of foreign-data wrapper %s");
						break;
					case OBJECT_FOREIGN_SERVER:
						msg = gettext_noop("must be owner of foreign server %s");
						break;
					case OBJECT_FOREIGN_TABLE:
						msg = gettext_noop("must be owner of foreign table %s");
						break;
					case OBJECT_FUNCTION:
						msg = gettext_noop("must be owner of function %s");
						break;
					case OBJECT_INDEX:
						msg = gettext_noop("must be owner of index %s");
						break;
					case OBJECT_LANGUAGE:
						msg = gettext_noop("must be owner of language %s");
						break;
					case OBJECT_LARGEOBJECT:
						msg = gettext_noop("must be owner of large object %s");
						break;
					case OBJECT_MATVIEW:
						msg = gettext_noop("must be owner of materialized view %s");
						break;
					case OBJECT_OPCLASS:
						msg = gettext_noop("must be owner of operator class %s");
						break;
					case OBJECT_OPERATOR:
						msg = gettext_noop("must be owner of operator %s");
						break;
					case OBJECT_OPFAMILY:
						msg = gettext_noop("must be owner of operator family %s");
						break;
					case OBJECT_PROCEDURE:
						msg = gettext_noop("must be owner of procedure %s");
						break;
					case OBJECT_PUBLICATION:
						msg = gettext_noop("must be owner of publication %s");
						break;
					case OBJECT_ROUTINE:
						msg = gettext_noop("must be owner of routine %s");
						break;
					case OBJECT_SEQUENCE:
						msg = gettext_noop("must be owner of sequence %s");
						break;
					case OBJECT_SUBSCRIPTION:
						msg = gettext_noop("must be owner of subscription %s");
						break;
					case OBJECT_TABLE:
						msg = gettext_noop("must be owner of table %s");
						break;
					case OBJECT_TYPE:
						msg = gettext_noop("must be owner of type %s");
						break;
					case OBJECT_VIEW:
						msg = gettext_noop("must be owner of view %s");
						break;
					case OBJECT_SCHEMA:
						msg = gettext_noop("must be owner of schema %s");
						break;
					case OBJECT_STATISTIC_EXT:
						msg = gettext_noop("must be owner of statistics object %s");
						break;
					case OBJECT_TABLESPACE:
						msg = gettext_noop("must be owner of tablespace %s");
						break;
					case OBJECT_TSCONFIGURATION:
						msg = gettext_noop("must be owner of text search configuration %s");
						break;
					case OBJECT_TSDICTIONARY:
						msg = gettext_noop("must be owner of text search dictionary %s");
						break;

						/*
						 * 特殊情况：对这些情况，错误消息谈论的是
						 * "relation"，因为所有权就附加在
						 * 关系上。另见
						 * check_object_ownership()。
						 */
					case OBJECT_COLUMN:
					case OBJECT_POLICY:
					case OBJECT_RULE:
					case OBJECT_TABCONSTRAINT:
					case OBJECT_TRIGGER:
						msg = gettext_noop("must be owner of relation %s");
						break;
						/* 这些目前未被使用 */
					case OBJECT_ACCESS_METHOD:
					case OBJECT_AMOP:
					case OBJECT_AMPROC:
					case OBJECT_ATTRIBUTE:
					case OBJECT_CAST:
					case OBJECT_DEFAULT:
					case OBJECT_DEFACL:
					case OBJECT_DOMCONSTRAINT:
					case OBJECT_PARAMETER_ACL:
					case OBJECT_PUBLICATION_NAMESPACE:
					case OBJECT_PUBLICATION_REL:
					case OBJECT_ROLE:
					case OBJECT_TRANSFORM:
					case OBJECT_TSPARSER:
					case OBJECT_TSTEMPLATE:
					case OBJECT_USER_MAPPING:
						elog(ERROR, "unsupported object type: %d", objtype);
				}

				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg(msg, objectname)));
				break;
			}
		default:
			elog(ERROR, "unrecognized AclResult: %d", (int) aclerr);
			break;
	}
}


void
aclcheck_error_col(AclResult aclerr, ObjectType objtype,
				   const char *objectname, const char *colname)
{
	switch (aclerr)
	{
		case ACLCHECK_OK:
			/* 无错误，返回给调用者 */
			break;
		case ACLCHECK_NO_PRIV:
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for column \"%s\" of relation \"%s\"",
							colname, objectname)));
			break;
		case ACLCHECK_NOT_OWNER:
			/* 使用 relation 消息没问题，因为列没有单独的所有者 */
			aclcheck_error(aclerr, objtype, objectname);
			break;
		default:
			elog(ERROR, "unrecognized AclResult: %d", (int) aclerr);
			break;
	}
}


/*
 * 针对类型的特殊通用处理：使用元素类型而不是数组类型，
 * 并整齐地格式化
 */
void
aclcheck_error_type(AclResult aclerr, Oid typeOid)
{
	Oid			element_type = get_element_type(typeOid);

	aclcheck_error(aclerr, OBJECT_TYPE, format_type_be(element_type ? element_type : typeOid));
}


/*
 * 根据对象种类转发到各种 pg_*_mask 例程
 */
static AclMode
pg_aclmask(ObjectType objtype, Oid object_oid, AttrNumber attnum, Oid roleid,
		   AclMode mask, AclMaskHow how)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return
				pg_class_aclmask(object_oid, roleid, mask, how) |
				pg_attribute_aclmask(object_oid, attnum, roleid, mask, how);
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
			return pg_class_aclmask(object_oid, roleid, mask, how);
		case OBJECT_DATABASE:
			return object_aclmask(DatabaseRelationId, object_oid, roleid, mask, how);
		case OBJECT_FUNCTION:
			return object_aclmask(ProcedureRelationId, object_oid, roleid, mask, how);
		case OBJECT_LANGUAGE:
			return object_aclmask(LanguageRelationId, object_oid, roleid, mask, how);
		case OBJECT_LARGEOBJECT:
			return pg_largeobject_aclmask_snapshot(object_oid, roleid,
												   mask, how, NULL);
		case OBJECT_PARAMETER_ACL:
			return pg_parameter_acl_aclmask(object_oid, roleid, mask, how);
		case OBJECT_SCHEMA:
			return object_aclmask(NamespaceRelationId, object_oid, roleid, mask, how);
		case OBJECT_STATISTIC_EXT:
			elog(ERROR, "grantable rights not supported for statistics objects");
			/* 不会走到这里，但让编译器安静 */
			return ACL_NO_RIGHTS;
		case OBJECT_TABLESPACE:
			return object_aclmask(TableSpaceRelationId, object_oid, roleid, mask, how);
		case OBJECT_FDW:
			return object_aclmask(ForeignDataWrapperRelationId, object_oid, roleid, mask, how);
		case OBJECT_FOREIGN_SERVER:
			return object_aclmask(ForeignServerRelationId, object_oid, roleid, mask, how);
		case OBJECT_EVENT_TRIGGER:
			elog(ERROR, "grantable rights not supported for event triggers");
			/* 不会走到这里，但让编译器安静 */
			return ACL_NO_RIGHTS;
		case OBJECT_TYPE:
			return object_aclmask(TypeRelationId, object_oid, roleid, mask, how);
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) objtype);
			/* 不会走到这里，但让编译器安静 */
			return ACL_NO_RIGHTS;
	}
}


/* ****************************************************************
 * 用于检查用户对各种对象权限的导出例程
 *
 * 这些函数通用 API 的说明见 aclmask()。
 *
 * 注意：对查找失败我们给予完整的 ereport 处理，因为
 * has_xxx_privilege() 系列函数允许用户向这些函数
 * 传入任意 OID。
 *  ****************************************************************
 */

/*
 * 用于检查用户对对象权限的通用例程
 */
static AclMode
object_aclmask(Oid classid, Oid objectid, Oid roleid,
			   AclMode mask, AclMaskHow how)
{
	int			cacheid;
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* 特殊情况 */
	switch (classid)
	{
		case NamespaceRelationId:
			return pg_namespace_aclmask(objectid, roleid, mask, how);
		case TypeRelationId:
			return pg_type_aclmask(objectid, roleid, mask, how);
	}

	/* 更特殊的情况 */
	Assert(classid != RelationRelationId);	/* 应改用 pg_class_acl* */
	Assert(classid != LargeObjectMetadataRelationId);	/* 应改用
														 * pg_largeobject_acl* */

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * 从对象的目录中获取其 ACL
	 */

	cacheid = get_object_catcache_oid(classid);

	tuple = SearchSysCache1(cacheid, ObjectIdGetDatum(objectid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("%s with OID %u does not exist", get_object_class_descr(classid), objectid)));

	ownerId = DatumGetObjectId(SysCacheGetAttrNotNull(cacheid,
													  tuple,
													  get_object_attnum_owner(classid)));

	aclDatum = SysCacheGetAttr(cacheid, tuple, get_object_attnum_acl(classid),
							   &isNull);
	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		acl = acldefault(get_object_type(classid, objectid), ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * 用于检查用户对列权限的例程
 *
 * 注意：这里只考虑专门授予该列的权限。
 * 调用者有责任在适当时把关系级权限纳入考虑。
 * （出于同样的原因，我们在这里对超级用户
 * 身份不做特殊处理。）
 */
static AclMode
pg_attribute_aclmask(Oid table_oid, AttrNumber attnum, Oid roleid,
					 AclMode mask, AclMaskHow how)
{
	return pg_attribute_aclmask_ext(table_oid, attnum, roleid,
									mask, how, NULL);
}

/*
 * Routine for examining a user's privileges for a column
 *
 * Does the bulk of the work for pg_attribute_aclmask(), and allows other
 * callers to avoid the missing attribute ERROR when is_missing is non-NULL.
 */
static AclMode
pg_attribute_aclmask_ext(Oid table_oid, AttrNumber attnum, Oid roleid,
						 AclMode mask, AclMaskHow how, bool *is_missing)
{
	AclMode		result;
	HeapTuple	classTuple;
	HeapTuple	attTuple;
	Form_pg_class classForm;
	Form_pg_attribute attributeForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * 首先，从列的 pg_attribute 条目获取其 ACL
	 */
	attTuple = SearchSysCache2(ATTNUM,
							   ObjectIdGetDatum(table_oid),
							   Int16GetDatum(attnum));
	if (!HeapTupleIsValid(attTuple))
	{
		if (is_missing != NULL)
		{
			/* 返回 "no privileges" 而不是抛出错误 */
			*is_missing = true;
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of relation with OID %u does not exist",
							attnum, table_oid)));
	}

	attributeForm = (Form_pg_attribute) GETSTRUCT(attTuple);

	/* 也检查已删除的列 */
	if (attributeForm->attisdropped)
	{
		if (is_missing != NULL)
		{
			/* 返回 "no privileges" 而不是抛出错误 */
			*is_missing = true;
			ReleaseSysCache(attTuple);
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of relation with OID %u does not exist",
							attnum, table_oid)));
	}

	aclDatum = SysCacheGetAttr(ATTNUM, attTuple, Anum_pg_attribute_attacl,
							   &isNull);

	/*
	 * 这里我们硬编码了这样的认识：列的默认 ACL 不授予任何
	 * 权限，以便在 attacl 为 null 这一非常常见的
	 * 情形下快速返回。
	 */
	if (isNull)
	{
		ReleaseSysCache(attTuple);
		return 0;
	}

	/*
	 * 必须从 pg_class 获取关系的 ownerId。既然我们已经找到
	 * 了 pg_attribute 条目，这里最可能的失败原因是
	 * 此后有并发的 DROP relation 提交了（这只有在我们
	 * 未持有该关系的锁时才会发生）。这种情况下我们倾向于报告
	 * "no privileges" 而不是失败，以避免
	 * has_column_privilege() 测试中出现不想要的失败。
	 */
	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(classTuple))
	{
		ReleaseSysCache(attTuple);
		return 0;
	}
	classForm = (Form_pg_class) GETSTRUCT(classTuple);

	ownerId = classForm->relowner;

	ReleaseSysCache(classTuple);

	/* 必要时解 TOAST 列的 ACL */
	acl = DatumGetAclP(aclDatum);

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(attTuple);

	return result;
}

/*
 * 用于检查用户对表权限的导出例程
 */
AclMode
pg_class_aclmask(Oid table_oid, Oid roleid,
				 AclMode mask, AclMaskHow how)
{
	return pg_class_aclmask_ext(table_oid, roleid, mask, how, NULL);
}

/*
 * 用于检查用户对表权限的例程
 *
 * 承担 pg_class_aclmask() 的大部分工作，并允许其他
 * 调用者在 is_missing 非 NULL 时避免 missing relation ERROR。
 */
static AclMode
pg_class_aclmask_ext(Oid table_oid, Oid roleid, AclMode mask,
					 AclMaskHow how, bool *is_missing)
{
	AclMode		result;
	HeapTuple	tuple;
	Form_pg_class classForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * 必须从 pg_class 获取关系的元组
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(tuple))
	{
		if (is_missing != NULL)
		{
			/* 返回 "no privileges" 而不是抛出错误 */
			*is_missing = true;
			return 0;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation with OID %u does not exist",
							table_oid)));
	}

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * 除非设置了 pg_authid.rolsuper，否则不允许任何人
	 * 更新系统目录。
	 *
	 * 自 7.4 起我们有一些可更新的系统视图；那些不应
	 * 以这种方式保护。假定视图规则能自行
	 * 处理。ACL_USAGE 用于万一我们有系统序列的情形。
	 */
	if ((mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE | ACL_USAGE)) &&
		IsSystemClass(table_oid, classForm) &&
		classForm->relkind != RELKIND_VIEW &&
		!superuser_arg(roleid))
		mask &= ~(ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE | ACL_USAGE);

	/*
	 * 否则，超级用户绕过所有权限检查。
	 */
	if (superuser_arg(roleid))
	{
		ReleaseSysCache(tuple);
		return mask;
	}

	/*
	 * 常规情况：从 pg_class 获取关系的 ACL
	 */
	ownerId = classForm->relowner;

	aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		switch (classForm->relkind)
		{
			case RELKIND_SEQUENCE:
				acl = acldefault(OBJECT_SEQUENCE, ownerId);
				break;
			default:
				acl = acldefault(OBJECT_TABLE, ownerId);
				break;
		}
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST 关系的 ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	/*
	 * 检查是否正在检查 ACL_SELECT，若是，且尚未作为
	 * 结果的一部分设置，则检查用户是否是
	 * pg_read_all_data 角色的成员，该角色允许读取访问所有关系。
	 */
	if (mask & ACL_SELECT && !(result & ACL_SELECT) &&
		has_privs_of_role(roleid, ROLE_PG_READ_ALL_DATA))
		result |= ACL_SELECT;

	/*
	 * 检查是否正在检查 ACL_INSERT、ACL_UPDATE 或 ACL_DELETE，若是，
	 * 且尚未作为结果的一部分设置，则检查用户
	 * 是否是 pg_write_all_data 角色的成员，该角色允许
	 * 对所有关系（系统目录除外，那些需要超级用户，见上文）
	 * 进行 INSERT/UPDATE/DELETE 访问。
	 */
	if (mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE) &&
		!(result & (ACL_INSERT | ACL_UPDATE | ACL_DELETE)) &&
		has_privs_of_role(roleid, ROLE_PG_WRITE_ALL_DATA))
		result |= (mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE));

	return result;
}

/*
 * 用于检查用户对配置参数（GUC）权限的例程，
 * 以 GUC 名称标识。
 */
static AclMode
pg_parameter_aclmask(const char *name, Oid roleid, AclMode mask, AclMaskHow how)
{
	AclMode		result;
	char	   *parname;
	text	   *partext;
	HeapTuple	tuple;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return mask;

	/* 把名称转换为它在 pg_parameter_acl 中应有的形式... */
	parname = convert_GUC_name_for_parameter_acl(name);
	partext = cstring_to_text(parname);

	/* ... 然后查找它 */
	tuple = SearchSysCache1(PARAMETERACLNAME, PointerGetDatum(partext));

	if (!HeapTupleIsValid(tuple))
	{
		/* 若无条目，则该 GUC 对非超级用户没有权限 */
		result = ACL_NO_RIGHTS;
	}
	else
	{
		Datum		aclDatum;
		bool		isNull;
		Acl		   *acl;

		aclDatum = SysCacheGetAttr(PARAMETERACLNAME, tuple,
								   Anum_pg_parameter_acl_paracl,
								   &isNull);
		if (isNull)
		{
			/* 没有 ACL，因此构建默认 ACL */
			acl = acldefault(OBJECT_PARAMETER_ACL, BOOTSTRAP_SUPERUSERID);
			aclDatum = (Datum) 0;
		}
		else
		{
			/* 必要时解 TOAST ACL */
			acl = DatumGetAclP(aclDatum);
		}

		result = aclmask(acl, roleid, BOOTSTRAP_SUPERUSERID, mask, how);

		/* 若有解 TOAST 后的副本，则释放它 */
		if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
			pfree(acl);

		ReleaseSysCache(tuple);
	}

	pfree(parname);
	pfree(partext);

	return result;
}

/*
 * 用于检查用户对配置参数（GUC）权限的例程，
 * 以其 pg_parameter_acl 条目的 OID 标识。
 */
static AclMode
pg_parameter_acl_aclmask(Oid acl_oid, Oid roleid, AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return mask;

	/* 从 pg_parameter_acl 获取 ACL */
	tuple = SearchSysCache1(PARAMETERACLOID, ObjectIdGetDatum(acl_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("parameter ACL with OID %u does not exist",
						acl_oid)));

	aclDatum = SysCacheGetAttr(PARAMETERACLOID, tuple,
							   Anum_pg_parameter_acl_paracl,
							   &isNull);
	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		acl = acldefault(OBJECT_PARAMETER_ACL, BOOTSTRAP_SUPERUSERID);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, BOOTSTRAP_SUPERUSERID, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * 用于检查用户对大对象权限的例程
 *
 * 当大对象以读取方式打开时，它相对于调用者的
 * 快照打开；但当以写入方式打开时，将使用当前的
 * MVCC 快照。参见 doc/src/sgml/lobj.sgml。本函数
 * 接收一个快照参数，以便权限检查可以
 * 相对于读取底层数据时将使用的同一
 * 快照进行。调用者实际上会为瞬时 MVCC
 * 快照传入 NULL，因为我们对快照参数所做的
 * 全部事情就是将其传给 systable_beginscan()。
 */
static AclMode
pg_largeobject_aclmask_snapshot(Oid lobj_oid, Oid roleid,
								AclMode mask, AclMaskHow how,
								Snapshot snapshot)
{
	AclMode		result;
	Relation	pg_lo_meta;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * 从 pg_largeobject_metadata 获取大对象的 ACL
	 */
	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(lobj_oid));

	scan = systable_beginscan(pg_lo_meta,
							  LargeObjectMetadataOidIndexId, true,
							  snapshot, 1, entry);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobj_oid)));

	ownerId = ((Form_pg_largeobject_metadata) GETSTRUCT(tuple))->lomowner;

	aclDatum = heap_getattr(tuple, Anum_pg_largeobject_metadata_lomacl,
							RelationGetDescr(pg_lo_meta), &isNull);

	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		acl = acldefault(OBJECT_LARGEOBJECT, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	systable_endscan(scan);

	table_close(pg_lo_meta, AccessShareLock);

	return result;
}

/*
 * 用于检查用户对命名空间权限的例程
 */
static AclMode
pg_namespace_aclmask(Oid nsp_oid, Oid roleid,
					 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * 若该命名空间被分配给我们作为临时命名空间，则检查
	 * 以确保我们在数据库上有 CREATE TEMP 权限；若有，则视同
	 * 我们在该命名空间上拥有所有标准权限（但不含 GRANT OPTION）。
	 * 若我们没有 CREATE TEMP，则视同我们
	 * 只有 USAGE（而没有 CREATE）权限。
	 *
	 * 鉴于 InitTempTableNamespace 中的检查，这似乎多余，
	 * 但实际并非如此，因为此后当前用户 ID 可能已改变。这种
	 * 行为的结果是：SECURITY DEFINER 函数可以创建
	 * 临时表，然后同一会话中那些没有创建临时
	 * 表权限的代码可以（在授权允许时）
	 * 访问这些临时表。
	 *
	 * XXX 像 InitTempTableNamespace 那样 ereport 一条特殊错误消息
	 * 是否安全？这里返回零意味着我们会得到一条
	 * 通用的 "permission denied for schema pg_temp_N" 消息，这并不
	 * 特别用户友好。
	 */
	if (isTempNamespace(nsp_oid))
	{
		if (object_aclcheck(DatabaseRelationId, MyDatabaseId, roleid,
							ACL_CREATE_TEMP) == ACLCHECK_OK)
			return mask & ACL_ALL_RIGHTS_SCHEMA;
		else
			return mask & ACL_USAGE;
	}

	/*
	 * 从 pg_namespace 获取模式的 ACL
	 */
	tuple = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	aclDatum = SysCacheGetAttr(NAMESPACEOID, tuple, Anum_pg_namespace_nspacl,
							   &isNull);
	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		acl = acldefault(OBJECT_SCHEMA, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	/*
	 * 检查是否正在检查 ACL_USAGE，若是，且尚未作为
	 * 结果的一部分设置，则检查用户是否是
	 * pg_read_all_data 或 pg_write_all_data 角色的成员，这些角色允许
	 * 对所有模式的 usage 访问。
	 */
	if (mask & ACL_USAGE && !(result & ACL_USAGE) &&
		(has_privs_of_role(roleid, ROLE_PG_READ_ALL_DATA) ||
		 has_privs_of_role(roleid, ROLE_PG_WRITE_ALL_DATA)))
		result |= ACL_USAGE;
	return result;
}

/*
 * 用于检查用户对类型权限的例程。
 */
static AclMode
pg_type_aclmask(Oid type_oid, Oid roleid, AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	Form_pg_type typeForm;

	/* 对超级用户绕过权限检查 */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * 必须从 pg_type 获取类型的元组
	 */
	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist",
						type_oid)));
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * "真正的" 数组类型不管理自身权限；
	 * 而是改为查阅元素类型。
	 */
	if (IsTrueArrayType(typeForm))
	{
		Oid			elttype_oid = typeForm->typelem;

		ReleaseSysCache(tuple);

		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elttype_oid));
		/* 这种情况不是面向用户的错误，所以用 elog 而不是 ereport */
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for type %u", elttype_oid);
		typeForm = (Form_pg_type) GETSTRUCT(tuple);
	}

	/*
	 * 现在从元组获取类型的所有者和 ACL
	 */
	ownerId = typeForm->typowner;

	aclDatum = SysCacheGetAttr(TYPEOID, tuple,
							   Anum_pg_type_typacl, &isNull);
	if (isNull)
	{
		/* 没有 ACL，因此构建默认 ACL */
		acl = acldefault(OBJECT_TYPE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* 必要时解 TOAST 关系的 ACL */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* 若有解 TOAST 后的副本，则释放它 */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * 用于检查用户对对象访问权限的导出通用例程
 */
AclResult
object_aclcheck(Oid classid, Oid objectid, Oid roleid, AclMode mode)
{
	if (object_aclmask(classid, objectid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a column
 *
 * Returns ACLCHECK_OK if the user has any of the privileges identified by
 * 'mode'; otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 *
 * As with pg_attribute_aclmask, only privileges granted directly on the
 * column are considered here.
 */
AclResult
pg_attribute_aclcheck(Oid table_oid, AttrNumber attnum,
					  Oid roleid, AclMode mode)
{
	return pg_attribute_aclcheck_ext(table_oid, attnum, roleid, mode, NULL);
}


/*
 * Exported routine for checking a user's access privileges to a column
 *
 * Does the bulk of the work for pg_attribute_aclcheck(), and allows other
 * callers to avoid the missing attribute ERROR when is_missing is non-NULL.
 */
AclResult
pg_attribute_aclcheck_ext(Oid table_oid, AttrNumber attnum,
						  Oid roleid, AclMode mode, bool *is_missing)
{
	if (pg_attribute_aclmask_ext(table_oid, attnum, roleid, mode,
								 ACLMASK_ANY, is_missing) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to any/all columns
 *
 * If 'how' is ACLMASK_ANY, then returns ACLCHECK_OK if user has any of the
 * privileges identified by 'mode' on any non-dropped column in the relation;
 * otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 *
 * If 'how' is ACLMASK_ALL, then returns ACLCHECK_OK if user has any of the
 * privileges identified by 'mode' on each non-dropped column in the relation
 * (and there must be at least one such column); otherwise returns a suitable
 * error code (in practice, always ACLCHECK_NO_PRIV).
 *
 * As with pg_attribute_aclmask, only privileges granted directly on the
 * column(s) are considered here.
 *
 * Note: system columns are not considered here; there are cases where that
 * might be appropriate but there are also cases where it wouldn't.
 */
AclResult
pg_attribute_aclcheck_all(Oid table_oid, Oid roleid, AclMode mode,
						  AclMaskHow how)
{
	AclResult	result;
	HeapTuple	classTuple;
	Form_pg_class classForm;
	AttrNumber	nattrs;
	AttrNumber	curr_att;

	/*
	 * Must fetch pg_class row to check number of attributes.  As in
	 * pg_attribute_aclmask, we prefer to return "no privileges" instead of
	 * throwing an error if we get any unexpected lookup errors.
	 */
	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
	if (!HeapTupleIsValid(classTuple))
		return ACLCHECK_NO_PRIV;
	classForm = (Form_pg_class) GETSTRUCT(classTuple);

	nattrs = classForm->relnatts;

	ReleaseSysCache(classTuple);

	/*
	 * 初始化结果，以防没有未删除的列。这种情况下
	 * 我们希望报告失败，无论 'how' 取何值。
	 */
	result = ACLCHECK_NO_PRIV;

	for (curr_att = 1; curr_att <= nattrs; curr_att++)
	{
		HeapTuple	attTuple;
		AclMode		attmask;

		attTuple = SearchSysCache2(ATTNUM,
								   ObjectIdGetDatum(table_oid),
								   Int16GetDatum(curr_att));
		if (!HeapTupleIsValid(attTuple))
			continue;

		/* 忽略已删除的列 */
		if (((Form_pg_attribute) GETSTRUCT(attTuple))->attisdropped)
		{
			ReleaseSysCache(attTuple);
			continue;
		}

		/*
		 * 这里我们硬编码了这样的认识：列的默认 ACL
		 * 不授予任何权限，以便在 attacl 为 null 这一
		 * 非常常见的情形下快速返回。
		 */
		if (heap_attisnull(attTuple, Anum_pg_attribute_attacl, NULL))
			attmask = 0;
		else
			attmask = pg_attribute_aclmask(table_oid, curr_att, roleid,
										   mode, ACLMASK_ANY);

		ReleaseSysCache(attTuple);

		if (attmask != 0)
		{
			result = ACLCHECK_OK;
			if (how == ACLMASK_ANY)
				break;			/* 任一成功则成功 */
		}
		else
		{
			result = ACLCHECK_NO_PRIV;
			if (how == ACLMASK_ALL)
				break;			/* 任一失败则失败 */
		}
	}

	return result;
}

/*
 * 用于检查用户对表访问权限的导出例程
 *
 * 若用户拥有由 'mode' 标识的任一权限，则返回 ACLCHECK_OK；
 * 否则返回适当的错误码（实际上总是
 * ACLCHECK_NO_PRIV）。
 */
AclResult
pg_class_aclcheck(Oid table_oid, Oid roleid, AclMode mode)
{
	return pg_class_aclcheck_ext(table_oid, roleid, mode, NULL);
}

/*
 * 用于检查用户对表访问权限的导出例程
 *
 * 承担 pg_class_aclcheck() 的大部分工作，并允许其他
 * 调用者在 is_missing 非 NULL 时避免 missing relation ERROR。
 */
AclResult
pg_class_aclcheck_ext(Oid table_oid, Oid roleid,
					  AclMode mode, bool *is_missing)
{
	if (pg_class_aclmask_ext(table_oid, roleid, mode,
							 ACLMASK_ANY, is_missing) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * 用于检查用户对配置参数（GUC）访问权限的导出例程，
 * 以 GUC 名称标识。
 */
AclResult
pg_parameter_aclcheck(const char *name, Oid roleid, AclMode mode)
{
	if (pg_parameter_aclmask(name, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * 用于检查用户对大对象访问权限的导出例程
 */
AclResult
pg_largeobject_aclcheck_snapshot(Oid lobj_oid, Oid roleid, AclMode mode,
								 Snapshot snapshot)
{
	if (pg_largeobject_aclmask_snapshot(lobj_oid, roleid, mode,
										ACLMASK_ANY, snapshot) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * 对象的通用所有权检查
 */
bool
object_ownercheck(Oid classid, Oid objectid, Oid roleid)
{
	int			cacheid;
	Oid			ownerId;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return true;

	/* 对大对象，要查阅的目录是 pg_largeobject_metadata */
	if (classid == LargeObjectRelationId)
		classid = LargeObjectMetadataRelationId;

	cacheid = get_object_catcache_oid(classid);
	if (cacheid != -1)
	{
		/* 我们可以从系统缓存获取对象的元组 */
		HeapTuple	tuple;

		tuple = SearchSysCache1(cacheid, ObjectIdGetDatum(objectid));
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("%s with OID %u does not exist", get_object_class_descr(classid), objectid)));

		ownerId = DatumGetObjectId(SysCacheGetAttrNotNull(cacheid,
														  tuple,
														  get_object_attnum_owner(classid)));
		ReleaseSysCache(tuple);
	}
	else
	{
		/* 用于没有合适系统缓存的目录 */
		Relation	rel;
		ScanKeyData entry[1];
		SysScanDesc scan;
		HeapTuple	tuple;
		bool		isnull;

		rel = table_open(classid, AccessShareLock);

		ScanKeyInit(&entry[0],
					get_object_attnum_oid(classid),
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectid));

		scan = systable_beginscan(rel,
								  get_object_oid_index(classid), true,
								  NULL, 1, entry);

		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("%s with OID %u does not exist", get_object_class_descr(classid), objectid)));

		ownerId = DatumGetObjectId(heap_getattr(tuple,
												get_object_attnum_owner(classid),
												RelationGetDescr(rel),
												&isnull));
		Assert(!isnull);

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	return has_privs_of_role(roleid, ownerId);
}

/*
 * 检查指定角色是否拥有 CREATEROLE 权限（或是超级用户）
 *
 * 注意：角色本身没有所有者；相反，我们在需要对角色进行
 * 类似所有权的权限测试之处使用本测试。
 * 务必将其应用于正在尝试执行操作的角色，而不是
 * 被操作的角色！另请注意，若目标角色是超级用户，
 * 这通常不应被视为足够的权限。
 * （我们在此不处理该项考虑，因为对这种情况我们想给出
 * 单独的错误消息，所以必须由调用者应对。）
 */
bool
has_createrole_privilege(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreaterole;
		ReleaseSysCache(utup);
	}
	return result;
}

bool
has_bypassrls_privilege(Oid roleid)
{
	bool		result = false;
	HeapTuple	utup;

	/* 超级用户绕过所有权限检查。 */
	if (superuser_arg(roleid))
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolbypassrls;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * 获取给定角色、命名空间和对象类型的 pg_default_acl 条目
 * （对象类型必须以 pg_default_acl 的编码给出）。
 * 若无此类条目则返回 NULL。
 */
static Acl *
get_default_acl_internal(Oid roleId, Oid nsp_oid, char objtype)
{
	Acl		   *result = NULL;
	HeapTuple	tuple;

	tuple = SearchSysCache3(DEFACLROLENSPOBJ,
							ObjectIdGetDatum(roleId),
							ObjectIdGetDatum(nsp_oid),
							CharGetDatum(objtype));

	if (HeapTupleIsValid(tuple))
	{
		Datum		aclDatum;
		bool		isNull;

		aclDatum = SysCacheGetAttr(DEFACLROLENSPOBJ, tuple,
								   Anum_pg_default_acl_defaclacl,
								   &isNull);
		if (!isNull)
			result = DatumGetAclPCopy(aclDatum);
		ReleaseSysCache(tuple);
	}

	return result;
}

/*
 * 获取给定模式中新建对象的默认权限
 *
 * 若应使用内置系统默认值则返回 NULL。
 *
 * 若结果不为 NULL，调用者必须在得知新对象的
 * OID 后调用 recordDependencyOnNewAcl。
 */
Acl *
get_user_default_acl(ObjectType objtype, Oid ownerId, Oid nsp_oid)
{
	Acl		   *result;
	Acl		   *glob_acl;
	Acl		   *schema_acl;
	Acl		   *def_acl;
	char		defaclobjtype;

	/*
	 * 在 bootstrap 期间使用 NULL，因为 pg_default_acl 可能
	 * 尚不存在。
	 */
	if (IsBootstrapProcessingMode())
		return NULL;

	/* 检查 pg_default_acl 是否支持该对象类型 */
	switch (objtype)
	{
		case OBJECT_TABLE:
			defaclobjtype = DEFACLOBJ_RELATION;
			break;

		case OBJECT_SEQUENCE:
			defaclobjtype = DEFACLOBJ_SEQUENCE;
			break;

		case OBJECT_FUNCTION:
			defaclobjtype = DEFACLOBJ_FUNCTION;
			break;

		case OBJECT_TYPE:
			defaclobjtype = DEFACLOBJ_TYPE;
			break;

		case OBJECT_SCHEMA:
			defaclobjtype = DEFACLOBJ_NAMESPACE;
			break;

		default:
			return NULL;
	}

	/* 查找相关的 pg_default_acl 条目 */
	glob_acl = get_default_acl_internal(ownerId, InvalidOid, defaclobjtype);
	schema_acl = get_default_acl_internal(ownerId, nsp_oid, defaclobjtype);

	/* 若两个条目都不存在则快速返回 */
	if (glob_acl == NULL && schema_acl == NULL)
		return NULL;

	/* 我们也需要知道硬编码的默认值 */
	def_acl = acldefault(objtype, ownerId);

	/* 若无全局条目，则替换为硬编码的默认值 */
	if (glob_acl == NULL)
		glob_acl = def_acl;

	/* 合并任何按模式权限 */
	result = aclmerge(glob_acl, schema_acl, ownerId);

	/*
	 * 为效率起见，若结果等于默认值我们希望返回 NULL。
	 * 这需要对两个数组排序才能得到准确的比较。
	 */
	aclitemsort(result);
	aclitemsort(def_acl);
	if (aclequal(result, def_acl))
		result = NULL;

	return result;
}

/*
 * 记录对新对象 ACL 中提到的角色的依赖。
 */
void
recordDependencyOnNewAcl(Oid classId, Oid objectId, int32 objsubId,
						 Oid ownerId, Acl *acl)
{
	int			nmembers;
	Oid		   *members;

	/* 若 ACL 为默认值则无事可做 */
	if (acl == NULL)
		return;

	/* 提取 ACL 中提到的角色 */
	nmembers = aclmembers(acl, &members);

	/* 更新共享依赖 ACL 信息 */
	updateAclDependencies(classId, objectId, objsubId,
						  ownerId,
						  0, NULL,
						  nmembers, members);
}

/*
 * 记录传入的顶层对象的初始权限。
 *
 * 对于传入的对象，这会把它的 ACL（若有）以及任何
 * 子对象（例如列）的 ACL 记录到 pg_init_privs。
 */
void
recordExtObjInitPriv(Oid objoid, Oid classoid)
{
	/*
	 * pg_class / pg_attribute
	 *
	 * 若这是一个关系，则我们需要查看它是否有任何子对象
	 * （例如列），若有，务必为每个子对象调用
	 * recordExtensionInitPrivWorker()。
	 */
	if (classoid == RelationRelationId)
	{
		Form_pg_class pg_class_tuple;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", objoid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * 索引没有权限，复合类型的 pg_class 行也没有。
		 * （鉴于 ALTER EXTENSION ADD 中的限制，
		 * 这些情况不可达，但我们还是检查一下。）
		 */
		if (pg_class_tuple->relkind == RELKIND_INDEX ||
			pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX ||
			pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
		{
			ReleaseSysCache(tuple);
			return;
		}

		/*
		 * 若这不是序列，那么它可能会有
		 * 与之关联的列级 ACL。
		 */
		if (pg_class_tuple->relkind != RELKIND_SEQUENCE)
		{
			AttrNumber	curr_att;
			AttrNumber	nattrs = pg_class_tuple->relnatts;

			for (curr_att = 1; curr_att <= nattrs; curr_att++)
			{
				HeapTuple	attTuple;
				Datum		attaclDatum;

				attTuple = SearchSysCache2(ATTNUM,
										   ObjectIdGetDatum(objoid),
										   Int16GetDatum(curr_att));

				if (!HeapTupleIsValid(attTuple))
					continue;

				/* 忽略已删除的列 */
				if (((Form_pg_attribute) GETSTRUCT(attTuple))->attisdropped)
				{
					ReleaseSysCache(attTuple);
					continue;
				}

				attaclDatum = SysCacheGetAttr(ATTNUM, attTuple,
											  Anum_pg_attribute_attacl,
											  &isNull);

				/* 对 NULL ACL 无需做任何事 */
				if (isNull)
				{
					ReleaseSysCache(attTuple);
					continue;
				}

				recordExtensionInitPrivWorker(objoid, classoid, curr_att,
											  DatumGetAclP(attaclDatum));

				ReleaseSysCache(attTuple);
			}
		}

		aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
								   &isNull);

		/* 添加顶层对象的记录（若有） */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
	else if (classoid == LargeObjectRelationId)
	{
		/* 对大对象，我们必须查阅 pg_largeobject_metadata */
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;
		ScanKeyData entry[1];
		SysScanDesc scan;
		Relation	relation;

		/*
		 * 注意：鉴于我们不允许把大对象
		 * 设为扩展成员，这是死代码。但似乎值得保留，以防
		 * 本函数未来的某个调用者需要它。
		 */
		relation = table_open(LargeObjectMetadataRelationId, RowExclusiveLock);

		/* pg_largeobject_metadata 没有系统缓存 */
		ScanKeyInit(&entry[0],
					Anum_pg_largeobject_metadata_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objoid));

		scan = systable_beginscan(relation,
								  LargeObjectMetadataOidIndexId, true,
								  NULL, 1, entry);

		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for large object %u", objoid);

		aclDatum = heap_getattr(tuple,
								Anum_pg_largeobject_metadata_lomacl,
								RelationGetDescr(relation), &isNull);

		/* 添加顶层对象的记录（若有） */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		systable_endscan(scan);
	}
	/* 对不支持的 classoid 会报错。 */
	else if (get_object_attnum_acl(classoid) != InvalidAttrNumber)
	{
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	tuple;

		tuple = SearchSysCache1(get_object_catcache_oid(classoid),
								ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for %s %u",
				 get_object_class_descr(classoid), objoid);

		aclDatum = SysCacheGetAttr(get_object_catcache_oid(classoid), tuple,
								   get_object_attnum_acl(classoid),
								   &isNull);

		/* 添加顶层对象的记录（若有） */
		if (!isNull)
			recordExtensionInitPrivWorker(objoid, classoid, 0,
										  DatumGetAclP(aclDatum));

		ReleaseSysCache(tuple);
	}
}

/*
 * 对于传入的对象，从 pg_init_privs 移除它的 ACL 以及任何对象 subId
 * 的 ACL（通过 recordExtensionInitPrivWorker()）。
 */
void
removeExtObjInitPriv(Oid objoid, Oid classoid)
{
	/*
	 * 若这是一个关系，则我们需要查看它是否有任何子对象
	 * （例如列），若有，务必为每个子对象调用
	 * recordExtensionInitPrivWorker()。
	 */
	if (classoid == RelationRelationId)
	{
		Form_pg_class pg_class_tuple;
		HeapTuple	tuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(objoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", objoid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * 索引没有权限，复合类型的 pg_class 行也没有。
		 * （鉴于 ALTER EXTENSION DROP 中的限制，
		 * 这些情况不可达，但我们还是检查一下。）
		 */
		if (pg_class_tuple->relkind == RELKIND_INDEX ||
			pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX ||
			pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
		{
			ReleaseSysCache(tuple);
			return;
		}

		/*
		 * 若这不是序列，那么它可能会有
		 * 与之关联的列级 ACL。
		 */
		if (pg_class_tuple->relkind != RELKIND_SEQUENCE)
		{
			AttrNumber	curr_att;
			AttrNumber	nattrs = pg_class_tuple->relnatts;

			for (curr_att = 1; curr_att <= nattrs; curr_att++)
			{
				HeapTuple	attTuple;

				attTuple = SearchSysCache2(ATTNUM,
										   ObjectIdGetDatum(objoid),
										   Int16GetDatum(curr_att));

				if (!HeapTupleIsValid(attTuple))
					continue;

				/* 移除时，移除所有条目，即使是已删除的列 */

				recordExtensionInitPrivWorker(objoid, classoid, curr_att, NULL);

				ReleaseSysCache(attTuple);
			}
		}

		ReleaseSysCache(tuple);
	}

	/* 移除顶层对象的记录（若有） */
	recordExtensionInitPrivWorker(objoid, classoid, 0, NULL);
}

/*
 * 记录扩展对象的初始 ACL
 *
 * 可在任意时候调用；我们会检查 'creating_extension' 是否已设置，
 * 若未设置则立即退出。
 *
 * 传入对象 OID、类 OID（即定义该对象的表
 * 的 OID）以及对象的 'sub' id（objsubid，若
 * 有）。若没有 'sub' id（它们目前只用于表的
 * 列），则传入 '0'。最后，传入要存储的完整 ACL。
 *
 * 若该对象/子对象已存在 ACL，我们将用传入的
 * 值替换它。
 *
 * 为 'new_acl' 传入 NULL 将导致该对象的条目被
 * 移除（若找到）。
 */
static void
recordExtensionInitPriv(Oid objoid, Oid classoid, int objsubid, Acl *new_acl)
{
	/*
	 * 通常我们只在创建扩展时记录初始权限，
	 * 但由于在使用 pg_upgrade 进行二进制升级期间
	 * 我们并不实际使用 CREATE EXTENSION，因此有一个变量让
	 * 我们得知：在此变量为 true 期间发出的 GRANT 和 REVOKE 语句
	 * 是为了该扩展对象的初始权限，
	 * 因此我们需要记录它们。
	 */
	if (!creating_extension && !binary_upgrade_record_init_privs)
		return;

	recordExtensionInitPrivWorker(objoid, classoid, objsubid, new_acl);
}

/*
 * 记录扩展对象的初始 ACL，worker。
 *
 * 这将对传入对象的整个 ACL 进行整体替换，
 * 因此务必传入要使用的完整新 ACL。
 *
 * 一般来说，请 *不要* 直接使用本函数，而应使用
 * recordExtensionInitPriv()，它会检查 'creating_extension' 是否已设置。
 * 本函数 *不* 检查 'creating_extension' 是否已设置，因为它也
 * 用于通过 ALTER EXTENSION ... ADD/DROP 将对象
 * 添加到扩展或从扩展移除时。
 */
static void
recordExtensionInitPrivWorker(Oid objoid, Oid classoid, int objsubid, Acl *new_acl)
{
	Relation	relation;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	HeapTuple	oldtuple;

	relation = table_open(InitPrivsRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_init_privs_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objoid));
	ScanKeyInit(&key[1],
				Anum_pg_init_privs_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classoid));
	ScanKeyInit(&key[2],
				Anum_pg_init_privs_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(objsubid));

	scan = systable_beginscan(relation, InitPrivsObjIndexId, true,
							  NULL, 3, key);

	/* 应当只存在一个条目或没有条目。 */
	oldtuple = systable_getnext(scan);

	/* 若找到条目，则用最新 ACL 更新它。 */
	if (HeapTupleIsValid(oldtuple))
	{
		Datum		values[Natts_pg_init_privs] = {0};
		bool		nulls[Natts_pg_init_privs] = {0};
		bool		replace[Natts_pg_init_privs] = {0};

		/* 若有要设置的新 ACL，则用它更新该行。 */
		if (new_acl)
		{
			values[Anum_pg_init_privs_initprivs - 1] = PointerGetDatum(new_acl);
			replace[Anum_pg_init_privs_initprivs - 1] = true;

			oldtuple = heap_modify_tuple(oldtuple, RelationGetDescr(relation),
										 values, nulls, replace);

			CatalogTupleUpdate(relation, &oldtuple->t_self, oldtuple);
		}
		else
		{
			/* new_acl 为 NULL，因此删除我们找到的条目。 */
			CatalogTupleDelete(relation, &oldtuple->t_self);
		}
	}
	else
	{
		Datum		values[Natts_pg_init_privs] = {0};
		bool		nulls[Natts_pg_init_privs] = {0};

		/*
		 * Only add a new entry if the new ACL is non-NULL.
		 *
		 * If we are passed in a NULL ACL and no entry exists, we can just
		 * fall through and do nothing.
		 */
		if (new_acl)
		{
			/* 未找到条目，因此添加它。 */
			values[Anum_pg_init_privs_objoid - 1] = ObjectIdGetDatum(objoid);
			values[Anum_pg_init_privs_classoid - 1] = ObjectIdGetDatum(classoid);
			values[Anum_pg_init_privs_objsubid - 1] = Int32GetDatum(objsubid);

			/* 本函数只处理扩展的初始权限 */
			values[Anum_pg_init_privs_privtype - 1] =
				CharGetDatum(INITPRIVS_EXTENSION);

			values[Anum_pg_init_privs_initprivs - 1] = PointerGetDatum(new_acl);

			tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);

			CatalogTupleInsert(relation, tuple);
		}
	}

	systable_endscan(scan);

	/* 防止多次处理对象时出错 */
	CommandCounterIncrement();

	table_close(relation, RowExclusiveLock);
}
