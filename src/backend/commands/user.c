/*-------------------------------------------------------------------------
 *
 * user.c
 *    用于操作角色的命令（早期称为用户）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/user.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/user.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/*
 * 移除角色授权（或其上的 admin option）时可能递归到 
 * 依赖的授权。我们用这些值来推断在此类情况下需要   
 * 执行什么操作。 
 *
 * RRG_NOOP 表示不会被该操作修改的授权。                
 * 。       
 *
 * RRG_REMOVE_ADMIN_OPTION 表示需要被该操作把 admin_option 
 *  设置为 false 的授权。              
 *
 * 类似地，RRG_REMOVE_INHERIT_OPTION 和 RRG_REMOVE_SET_OPTION        
 * 表示需要把相应选项设置为 false 的授权。               
 *
 * RRG_DELETE_GRANT 表示需要被该操作完全删除的授权。        
 * 。              
 */
typedef enum
{
	RRG_NOOP,
	RRG_REMOVE_ADMIN_OPTION,
	RRG_REMOVE_INHERIT_OPTION,
	RRG_REMOVE_SET_OPTION,
	RRG_DELETE_GRANT
} RevokeRoleGrantAction;

/* 可由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_pg_authid_oid = InvalidOid;

typedef struct
{
	unsigned	specified;
	bool		admin;
	bool		inherit;
	bool		set;
} GrantRoleOptions;

#define GRANT_ROLE_SPECIFIED_ADMIN			0x0001
#define GRANT_ROLE_SPECIFIED_INHERIT		0x0002
#define GRANT_ROLE_SPECIFIED_SET			0x0004

/* GUC 参数 */
int			Password_encryption = PASSWORD_TYPE_SCRAM_SHA_256;
char	   *createrole_self_grant = "";
bool		createrole_self_grant_enabled = false;
GrantRoleOptions createrole_self_grant_options;

/* CreateRole() 和 AlterRole() 中用于校验密码的钩子 */
check_password_hook_type check_password_hook = NULL;

static void AddRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
						List *memberSpecs, List *memberIds,
						Oid grantorId, GrantRoleOptions *popt);
static void DelRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
						List *memberSpecs, List *memberIds,
						Oid grantorId, GrantRoleOptions *popt,
						DropBehavior behavior);
static void check_role_membership_authorization(Oid currentUserId, Oid roleid,
												bool is_grant);
static Oid	check_role_grantor(Oid currentUserId, Oid roleid, Oid grantorId,
							   bool is_grant);
static RevokeRoleGrantAction *initialize_revoke_actions(CatCList *memlist);
static bool plan_single_revoke(CatCList *memlist,
							   RevokeRoleGrantAction *actions,
							   Oid member, Oid grantor,
							   GrantRoleOptions *popt,
							   DropBehavior behavior);
static void plan_member_revoke(CatCList *memlist,
							   RevokeRoleGrantAction *actions, Oid member);
static void plan_recursive_revoke(CatCList *memlist,
								  RevokeRoleGrantAction *actions,
								  int index,
								  bool revoke_admin_option_only,
								  DropBehavior behavior);
static void InitGrantRoleOptions(GrantRoleOptions *popt);


/* 检查当前用户是否具有 createrole 权限 */
static bool
have_createrole_privilege(void)
{
	return has_createrole_privilege(GetUserId());
}


/*
 * CREATE ROLE
 */
Oid
CreateRole(ParseState *pstate, CreateRoleStmt *stmt)
{
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_authid] = {0};
	bool		new_record_nulls[Natts_pg_authid] = {0};
	Oid			currentUserId = GetUserId();
	Oid			roleid;
	ListCell   *item;
	ListCell   *option;
	char	   *password = NULL;	/* 用户密码 */
	bool		issuper = false;	/* 是否将该用户设为超级用户？ */
	bool		inherit = true; /* 是否自动继承权限？ */
	bool		createrole = false; /* 该用户是否可以创建角色？ */
	bool		createdb = false;	/* 该用户是否可以创建数据库？ */
	bool		canlogin = false;	/* 该用户是否可以登录？ */
	bool		isreplication = false;	/* 该用户是否为复制角色？ */
	bool		bypassrls = false;	/* 该用户是否为行安全启用角色？ */
	int			connlimit = -1; /* 最大允许连接数 */
	List	   *addroleto = NIL;	/* 将此角色添加为其中成员的角色列表 */
	List	   *rolemembers = NIL;	/* 将其添加为此角色成员的角色列表 */
	List	   *adminmembers = NIL; /* 将其添加为此角色管理员的角色列表 */
	char	   *validUntil = NULL;	/* 登录有效截止时间 */
	Datum		validUntil_datum;	/* 同上，以 timestamptz Datum 形式 */
	bool		validUntil_null;
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *disreplication = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *daddroleto = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dadminmembers = NULL;
	DefElem    *dvalidUntil = NULL;
	DefElem    *dbypassRLS = NULL;
	GrantRoleOptions popt;

	/* 默认值可能因原始语句类型而异 */
	switch (stmt->stmt_type)
	{
		case ROLESTMT_ROLE:
			break;
		case ROLESTMT_USER:
			canlogin = true;
			/* 将来可能希望此处 inherit 默认值为 false */
			break;
		case ROLESTMT_GROUP:
			break;
	}

	/* 从语句节点树中提取选项 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0)
		{
			if (dpassword)
				errorConflictingDefElem(defel, pstate);
			dpassword = defel;
		}
		else if (strcmp(defel->defname, "sysid") == 0)
		{
			ereport(NOTICE,
					(errmsg("SYSID can no longer be specified")));
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				errorConflictingDefElem(defel, pstate);
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				errorConflictingDefElem(defel, pstate);
			dinherit = defel;
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				errorConflictingDefElem(defel, pstate);
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				errorConflictingDefElem(defel, pstate);
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				errorConflictingDefElem(defel, pstate);
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "isreplication") == 0)
		{
			if (disreplication)
				errorConflictingDefElem(defel, pstate);
			disreplication = defel;
		}
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "addroleto") == 0)
		{
			if (daddroleto)
				errorConflictingDefElem(defel, pstate);
			daddroleto = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0)
		{
			if (drolemembers)
				errorConflictingDefElem(defel, pstate);
			drolemembers = defel;
		}
		else if (strcmp(defel->defname, "adminmembers") == 0)
		{
			if (dadminmembers)
				errorConflictingDefElem(defel, pstate);
			dadminmembers = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				errorConflictingDefElem(defel, pstate);
			dvalidUntil = defel;
		}
		else if (strcmp(defel->defname, "bypassrls") == 0)
		{
			if (dbypassRLS)
				errorConflictingDefElem(defel, pstate);
			dbypassRLS = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dissuper)
		issuper = boolVal(dissuper->arg);
	if (dinherit)
		inherit = boolVal(dinherit->arg);
	if (dcreaterole)
		createrole = boolVal(dcreaterole->arg);
	if (dcreatedb)
		createdb = boolVal(dcreatedb->arg);
	if (dcanlogin)
		canlogin = boolVal(dcanlogin->arg);
	if (disreplication)
		isreplication = boolVal(disreplication->arg);
	if (dconnlimit)
	{
		connlimit = intVal(dconnlimit->arg);
		if (connlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", connlimit)));
	}
	if (daddroleto)
		addroleto = (List *) daddroleto->arg;
	if (drolemembers)
		rolemembers = (List *) drolemembers->arg;
	if (dadminmembers)
		adminmembers = (List *) dadminmembers->arg;
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dbypassRLS)
		bypassrls = boolVal(dbypassRLS->arg);

	/* 先检查部分权限 */
	if (!superuser_arg(currentUserId))
	{
		if (!has_createrole_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles.",
							   "CREATEROLE")));
		if (issuper)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
		if (createdb && !have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "CREATEDB", "CREATEDB")));
		if (isreplication && !has_rolreplication(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "REPLICATION", "REPLICATION")));
		if (bypassrls && !has_bypassrls_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "BYPASSRLS", "BYPASSRLS")));
	}

	/*
     * 检查用户是否在尝试创建保留的 "pg_" 命名空间中的角色。
 *
     */
	if (IsReservedName(stmt->role))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						stmt->role),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	/*
     * 如果编译时启用了相应开关，当违反回归测试的角色命名约定时
     * 发出警告。
     */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->role, "regress_", 8) != 0)
		elog(WARNING, "roles created by regression test cases should have names starting with \"regress_\"");
#endif

	/*
     * 检查 pg_authid 关系以确保该角色尚不存在。
 *
     */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	if (OidIsValid(get_role_oid(stmt->role, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists",
						stmt->role)));

	/* 将 validuntil 转换为内部格式 */
	if (validUntil)
	{
		validUntil_datum = DirectFunctionCall3(timestamptz_in,
											   CStringGetDatum(validUntil),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
		validUntil_null = false;
	}
	else
	{
		validUntil_datum = (Datum) 0;
		validUntil_null = true;
	}

	/*
     * 如果定义了密码检查钩子，则调用它
     */
	if (check_password_hook && password)
		(*check_password_hook) (stmt->role,
								password,
								get_password_type(password),
								validUntil_datum,
								validUntil_null);

	/*
     * 构建要插入的元组
     */
	new_record[Anum_pg_authid_rolname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->role));
	new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(issuper);
	new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(inherit);
	new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(createrole);
	new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(createdb);
	new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(canlogin);
	new_record[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(isreplication);
	new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);

	if (password)
	{
		char	   *shadow_pass;
		const char *logdetail = NULL;

		/*
         * 不允许空密码。Libpq 将空密码视为无密码，甚至不会尝试
         * 认证。但其他客户端可能会尝试，因此允许空密码会造成困惑。
         * 当指定空字符串时清除密码，可以使所有客户端一致地锁定
         * 该账户。
         *
         * 注意：这仅涵盖存储在数据库内部的密码。认证代码中还有
         * 额外的检查，用于禁止空密码与从外部系统（如 LDAP 或 PAM）
         * 获取密码的认证方法一起使用。
 *
 *
         */
		if (password[0] == '\0' ||
			plain_crypt_verify(stmt->role, password, "", &logdetail) == STATUS_OK)
		{
			ereport(NOTICE,
					(errmsg("empty string is not a valid password, clearing password")));
			new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
		}
		else
		{
			/* 将密码加密为所请求的格式。 */
			shadow_pass = encrypt_password(Password_encryption, stmt->role,
										   password);
			new_record[Anum_pg_authid_rolpassword - 1] =
				CStringGetTextDatum(shadow_pass);
		}
	}
	else
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;

	new_record[Anum_pg_authid_rolvaliduntil - 1] = validUntil_datum;
	new_record_nulls[Anum_pg_authid_rolvaliduntil - 1] = validUntil_null;

	new_record[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(bypassrls);

	/*
     * pg_largeobject_metadata 中包含 pg_authid.oid，因此我们使用
     * 二进制升级覆盖。
     */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_authid_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_authid OID value not set when in binary upgrade mode")));

		roleid = binary_upgrade_next_pg_authid_oid;
		binary_upgrade_next_pg_authid_oid = InvalidOid;
	}
	else
	{
		roleid = GetNewOidWithIndex(pg_authid_rel, AuthIdOidIndexId,
									Anum_pg_authid_oid);
	}

	new_record[Anum_pg_authid_oid - 1] = ObjectIdGetDatum(roleid);

	tuple = heap_form_tuple(pg_authid_dsc, new_record, new_record_nulls);

	/*
     * 在 pg_authid 表中插入新记录
     */
	CatalogTupleInsert(pg_authid_rel, tuple);

	/*
     * 推进命令计数器以使新记录可见；否则 AddRoleMems 中的
     * 测试可能会失败。
     */
	if (addroleto || adminmembers || rolemembers)
		CommandCounterIncrement();

	/* 默认授权。 */
	InitGrantRoleOptions(&popt);

	/*
     * 将新角色添加到指定的已有角色中。
     */
	if (addroleto)
	{
		RoleSpec   *thisrole = makeNode(RoleSpec);
		List	   *thisrole_list = list_make1(thisrole);
		List	   *thisrole_oidlist = list_make1_oid(roleid);

		thisrole->roletype = ROLESPEC_CSTRING;
		thisrole->rolename = stmt->role;
		thisrole->location = -1;

		foreach(item, addroleto)
		{
			RoleSpec   *oldrole = lfirst(item);
			HeapTuple	oldroletup = get_rolespec_tuple(oldrole);
			Form_pg_authid oldroleform = (Form_pg_authid) GETSTRUCT(oldroletup);
			Oid			oldroleid = oldroleform->oid;
			char	   *oldrolename = NameStr(oldroleform->rolname);

			/* 只能将此角色添加到您拥有权限的角色中 */
			check_role_membership_authorization(currentUserId, oldroleid, true);
			AddRoleMems(currentUserId, oldrolename, oldroleid,
						thisrole_list,
						thisrole_oidlist,
						InvalidOid, &popt);

			ReleaseSysCache(oldroletup);
		}
	}

	/*
     * 如果当前用户不是超级用户，则使其成为新角色的管理员，
     * 以便他们可以管理刚刚创建的新对象。超级用户无论如何
     * 都能做到这一点。
     *
     * 此隐式授权的记录授权者为初始引导超级用户，这意味着
     * CREATEROLE 用户无法撤销此授权。但基于他们对该角色享有
     * ADMIN OPTION，他们可以将该角色通过其他选项重新授权给自己。
 *
     */
	if (!superuser())
	{
		RoleSpec   *current_role = makeNode(RoleSpec);
		GrantRoleOptions poptself;
		List	   *memberSpecs;
		List	   *memberIds = list_make1_oid(currentUserId);

		current_role->roletype = ROLESPEC_CURRENT_ROLE;
		current_role->location = -1;
		memberSpecs = list_make1(current_role);

		poptself.specified = GRANT_ROLE_SPECIFIED_ADMIN
			| GRANT_ROLE_SPECIFIED_INHERIT
			| GRANT_ROLE_SPECIFIED_SET;
		poptself.admin = true;
		poptself.inherit = false;
		poptself.set = false;

		AddRoleMems(BOOTSTRAP_SUPERUSERID, stmt->role, roleid,
					memberSpecs, memberIds,
					BOOTSTRAP_SUPERUSERID, &poptself);

		/*
         * 必须使隐式授权对下方代码可见，否则附加的 GRANT 将失败。
 *
         */
		CommandCounterIncrement();

		/*
         * 由于上述隐式授权，创建角色的 CREATEROLE 用户有权
         * 通过 INHERIT 或 SET 选项将该角色授权回自己，以便
         * 继承该角色的权限或能 SET ROLE 到该角色。
         * createrole_self_grant GUC 可用于自动执行此操作。
         * 这没有安全隐患，因为同一用户完全可以通过显式的
         * GRANT 语句进行相同的授权；这只是提供便利。
 *
         */
		if (createrole_self_grant_enabled)
			AddRoleMems(currentUserId, stmt->role, roleid,
						memberSpecs, memberIds,
						currentUserId, &createrole_self_grant_options);
	}

	/*
     * 将指定的成员添加到这个新角色中。adminmembers 获得 admin
     * 选项，rolemembers 不获得。
     *
     * 注意：此处不需要权限检查。如果您有足够的权限创建角色，
     * 就可以添加任何您想要的成员。
     */
	AddRoleMems(currentUserId, stmt->role, roleid,
				rolemembers, roleSpecsToIds(rolemembers),
				InvalidOid, &popt);
	popt.specified |= GRANT_ROLE_SPECIFIED_ADMIN;
	popt.admin = true;
	AddRoleMems(currentUserId, stmt->role, roleid,
				adminmembers, roleSpecsToIds(adminmembers),
				InvalidOid, &popt);

	/* 新角色的创建后钩子 */
	InvokeObjectPostCreateHook(AuthIdRelationId, roleid, 0);

	/*
     * 关闭 pg_authid，但保留锁直到提交。
     */
	table_close(pg_authid_rel, NoLock);

	return roleid;
}


/*
 * ALTER ROLE
 *
 * 注意：此处接受的 rolemembers 选项旨在支持向后兼容的
 * ALTER GROUP 语法。虽然使用 "ALTER ROLE role ROLE rolenames" 也能
 * 正常工作，但我们不对此进行文档化。
 */
Oid
AlterRole(ParseState *pstate, AlterRoleStmt *stmt)
{
	Datum		new_record[Natts_pg_authid] = {0};
	bool		new_record_nulls[Natts_pg_authid] = {0};
	bool		new_record_repl[Natts_pg_authid] = {0};
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple,
				new_tuple;
	Form_pg_authid authform;
	ListCell   *option;
	char	   *rolename;
	char	   *password = NULL;	/* 用户密码 */
	int			connlimit = -1; /* 最大允许连接数 */
	char	   *validUntil = NULL;	/* 登录有效截止时间 */
	Datum		validUntil_datum;	/* 同上，以 timestamptz Datum 形式 */
	bool		validUntil_null;
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *disreplication = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dvalidUntil = NULL;
	DefElem    *dbypassRLS = NULL;
	Oid			roleid;
	Oid			currentUserId = GetUserId();
	GrantRoleOptions popt;

	check_rolespec_name(stmt->role,
						_("Cannot alter reserved roles."));

	/* 从语句节点树中提取选项 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0)
		{
			if (dpassword)
				errorConflictingDefElem(defel, pstate);
			dpassword = defel;
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				errorConflictingDefElem(defel, pstate);
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				errorConflictingDefElem(defel, pstate);
			dinherit = defel;
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				errorConflictingDefElem(defel, pstate);
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				errorConflictingDefElem(defel, pstate);
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				errorConflictingDefElem(defel, pstate);
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "isreplication") == 0)
		{
			if (disreplication)
				errorConflictingDefElem(defel, pstate);
			disreplication = defel;
		}
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0 &&
				 stmt->action != 0)
		{
			if (drolemembers)
				errorConflictingDefElem(defel, pstate);
			drolemembers = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				errorConflictingDefElem(defel, pstate);
			dvalidUntil = defel;
		}
		else if (strcmp(defel->defname, "bypassrls") == 0)
		{
			if (dbypassRLS)
				errorConflictingDefElem(defel, pstate);
			dbypassRLS = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dconnlimit)
	{
		connlimit = intVal(dconnlimit->arg);
		if (connlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", connlimit)));
	}
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);

	/*
     * 扫描 pg_authid 关系以确认该用户存在。
     */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	tuple = get_rolespec_tuple(stmt->role);
	authform = (Form_pg_authid) GETSTRUCT(tuple);
	rolename = pstrdup(NameStr(authform->rolname));
	roleid = authform->oid;

	/* 要想以任何方式操作超级用户，您必须是超级用户。 */
	if (!superuser() && authform->rolsuper)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s attribute may alter roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));
	if (!superuser() && dissuper)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s attribute may change the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	/*
     * 对角色的大多数更改要求您同时具有 CREATEROLE 权限和
     * 对该角色的 ADMIN OPTION 权限。
     */
	if (!have_createrole_privilege() ||
		!is_admin_of_role(GetUserId(), roleid))
	{
		/* 无权限用户肯定不能做的事情 */
		if (dinherit || dcreaterole || dcreatedb || dcanlogin || dconnlimit ||
			dvalidUntil || disreplication || dbypassRLS)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may alter this role.",
							   "CREATEROLE", "ADMIN", rolename)));

		/* 无权限用户可以更改自己的密码 */
		if (dpassword && roleid != currentUserId)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("To change another role's password, the current user must have the %s attribute and the %s option on the role.",
							   "CREATEROLE", "ADMIN")));
	}
	else if (!superuser())
	{
		/*
         * 即使您对某角色同时具有 CREATEROLE 和 ADMIN OPTION，
         * 也只有在 CREATEDB、REPLICATION 或 BYPASSRLS 属性
         * 已为您自己的角色设置（或您是超级用户）的情况下，      * 才能更改这些属性。
         */
		if (dcreatedb && !have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "CREATEDB", "CREATEDB")));
		if (disreplication && !has_rolreplication(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "REPLICATION", "REPLICATION")));
		if (dbypassRLS && !has_bypassrls_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "BYPASSRLS", "BYPASSRLS")));
	}

	/* 要添加或删除成员，需要 ADMIN OPTION。 */
	if (drolemembers && !is_admin_of_role(currentUserId, roleid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s option on role \"%s\" may add or drop members.",
						   "ADMIN", rolename)));

	/* 将 validuntil 转换为内部格式 */
	if (dvalidUntil)
	{
		validUntil_datum = DirectFunctionCall3(timestamptz_in,
											   CStringGetDatum(validUntil),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
		validUntil_null = false;
	}
	else
	{
		/* 提取现有设置，以便钩子可能需要 */
		validUntil_datum = SysCacheGetAttr(AUTHNAME, tuple,
										   Anum_pg_authid_rolvaliduntil,
										   &validUntil_null);
	}

	/*
     * 如果定义了密码检查钩子，则调用它
     */
	if (check_password_hook && password)
		(*check_password_hook) (rolename,
								password,
								get_password_type(password),
								validUntil_datum,
								validUntil_null);

	/*
     * 构建更新后的元组，使用刚刚获取的信息
     */

	/*
     * issuper/createrole 等
     */
	if (dissuper)
	{
		bool		should_be_super = boolVal(dissuper->arg);

		if (!should_be_super && roleid == BOOTSTRAP_SUPERUSERID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("permission denied to alter role"),
					 errdetail("The bootstrap user must have the %s attribute.",
							   "SUPERUSER")));

		new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(should_be_super);
		new_record_repl[Anum_pg_authid_rolsuper - 1] = true;
	}

	if (dinherit)
	{
		new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(boolVal(dinherit->arg));
		new_record_repl[Anum_pg_authid_rolinherit - 1] = true;
	}

	if (dcreaterole)
	{
		new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(boolVal(dcreaterole->arg));
		new_record_repl[Anum_pg_authid_rolcreaterole - 1] = true;
	}

	if (dcreatedb)
	{
		new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(boolVal(dcreatedb->arg));
		new_record_repl[Anum_pg_authid_rolcreatedb - 1] = true;
	}

	if (dcanlogin)
	{
		new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(boolVal(dcanlogin->arg));
		new_record_repl[Anum_pg_authid_rolcanlogin - 1] = true;
	}

	if (disreplication)
	{
		new_record[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(boolVal(disreplication->arg));
		new_record_repl[Anum_pg_authid_rolreplication - 1] = true;
	}

	if (dconnlimit)
	{
		new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);
		new_record_repl[Anum_pg_authid_rolconnlimit - 1] = true;
	}

	/* 密码 */
	if (password)
	{
		char	   *shadow_pass;
		const char *logdetail = NULL;

		/* 与 CREATE USER 中一样，不允许空密码。 */
		if (password[0] == '\0' ||
			plain_crypt_verify(rolename, password, "", &logdetail) == STATUS_OK)
		{
			ereport(NOTICE,
					(errmsg("empty string is not a valid password, clearing password")));
			new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
		}
		else
		{
			/* 将密码加密为所请求的格式。 */
			shadow_pass = encrypt_password(Password_encryption, rolename,
										   password);
			new_record[Anum_pg_authid_rolpassword - 1] =
				CStringGetTextDatum(shadow_pass);
		}
		new_record_repl[Anum_pg_authid_rolpassword - 1] = true;
	}

	/* 取消设置密码 */
	if (dpassword && dpassword->arg == NULL)
	{
		new_record_repl[Anum_pg_authid_rolpassword - 1] = true;
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
	}

	/* 有效截止时间 */
	new_record[Anum_pg_authid_rolvaliduntil - 1] = validUntil_datum;
	new_record_nulls[Anum_pg_authid_rolvaliduntil - 1] = validUntil_null;
	new_record_repl[Anum_pg_authid_rolvaliduntil - 1] = true;

	if (dbypassRLS)
	{
		new_record[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(boolVal(dbypassRLS->arg));
		new_record_repl[Anum_pg_authid_rolbypassrls - 1] = true;
	}

	new_tuple = heap_modify_tuple(tuple, pg_authid_dsc, new_record,
								  new_record_nulls, new_record_repl);
	CatalogTupleUpdate(pg_authid_rel, &tuple->t_self, new_tuple);

	InvokeObjectPostAlterHook(AuthIdRelationId, roleid, 0);

	ReleaseSysCache(tuple);
	heap_freetuple(new_tuple);

	InitGrantRoleOptions(&popt);

	/*
     * 推进命令计数器以使新记录可见；否则 AddRoleMems 中的
     * 测试可能会失败。
     */
	if (drolemembers)
	{
		List	   *rolemembers = (List *) drolemembers->arg;

		CommandCounterIncrement();

		if (stmt->action == +1) /* 向角色添加成员 */
			AddRoleMems(currentUserId, rolename, roleid,
						rolemembers, roleSpecsToIds(rolemembers),
						InvalidOid, &popt);
		else if (stmt->action == -1)	/* 从角色移除成员 */
			DelRoleMems(currentUserId, rolename, roleid,
						rolemembers, roleSpecsToIds(rolemembers),
						InvalidOid, &popt, DROP_RESTRICT);
	}

	/*
     * 关闭 pg_authid，但保留锁直到提交。
     */
	table_close(pg_authid_rel, NoLock);

	return roleid;
}


/*
 * ALTER ROLE ... SET
 */
Oid
AlterRoleSet(AlterRoleSetStmt *stmt)
{
	HeapTuple	roletuple;
	Form_pg_authid roleform;
	Oid			databaseid = InvalidOid;
	Oid			roleid = InvalidOid;

	if (stmt->role)
	{
		check_rolespec_name(stmt->role,
							_("Cannot alter reserved roles."));

		roletuple = get_rolespec_tuple(stmt->role);
		roleform = (Form_pg_authid) GETSTRUCT(roletuple);
		roleid = roleform->oid;

		/*
         * 获取角色的锁，并确保它在此期间没有被删除。
 *
         */
		shdepLockAndCheckObject(AuthIdRelationId, roleid);

		/*
         * 要操作超级用户，您必须是超级用户；否则需要 CREATEROLE
         * 加上对目标角色的 admin 选项；除非您只是想更改
         * 自己的设置
         */
		if (roleform->rolsuper)
		{
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to alter role"),
						 errdetail("Only roles with the %s attribute may alter roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
		}
		else
		{
			if ((!have_createrole_privilege() ||
				 !is_admin_of_role(GetUserId(), roleid))
				&& roleid != GetUserId())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to alter role"),
						 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may alter this role.",
								   "CREATEROLE", "ADMIN", NameStr(roleform->rolname))));
		}

		ReleaseSysCache(roletuple);
	}

	/* 如果指定了数据库，则查找并锁定该数据库 */
	if (stmt->database != NULL)
	{
		databaseid = get_database_oid(stmt->database, false);
		shdepLockAndCheckObject(DatabaseRelationId, databaseid);

		if (!stmt->role)
		{
			/*
             * 如果没有指定角色，则此操作实际上等同于
             * ALTER DATABASE ... SET，因此使用相同的权限检查。
             */
			if (!object_ownercheck(DatabaseRelationId, databaseid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
							   stmt->database);
		}
	}

	if (!stmt->role && !stmt->database)
	{
		/* 必须是超级用户才能全局更改设置。 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter setting"),
					 errdetail("Only roles with the %s attribute may alter settings globally.",
							   "SUPERUSER")));
	}

	AlterSetting(databaseid, roleid, stmt->setstmt);

	return roleid;
}


/*
 * DROP ROLE
 */
void
DropRole(DropRoleStmt *stmt)
{
	Relation	pg_authid_rel,
				pg_auth_members_rel;
	ListCell   *item;
	List	   *role_oids = NIL;

	if (!have_createrole_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop role"),
				 errdetail("Only roles with the %s attribute and the %s option on the target roles may drop roles.",
						   "CREATEROLE", "ADMIN")));

	/*
     * 扫描 pg_authid 关系以找到待删除角色的 OID，
     * 并执行初步的权限和合理性检查。
     */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_auth_members_rel = table_open(AuthMemRelationId, RowExclusiveLock);

	foreach(item, stmt->roles)
	{
		RoleSpec   *rolspec = lfirst(item);
		char	   *role;
		HeapTuple	tuple,
					tmp_tuple;
		Form_pg_authid roleform;
		ScanKeyData scankey;
		SysScanDesc sscan;
		Oid			roleid;

		if (rolspec->roletype != ROLESPEC_CSTRING)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use special role specifier in DROP ROLE")));
		role = rolspec->rolename;

		tuple = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
		if (!HeapTupleIsValid(tuple))
		{
			if (!stmt->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", role)));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("role \"%s\" does not exist, skipping",
								role)));
			}

			continue;
		}

		roleform = (Form_pg_authid) GETSTRUCT(tuple);
		roleid = roleform->oid;

		if (roleid == GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetOuterUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetSessionUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("session user cannot be dropped")));

		/*
         * 为了安全起见，我们允许拥有 createrole 权限的用户删除
         * 普通角色，但不能删除超级用户角色，并且必须同时具备
         * ADMIN OPTION。
         */
		if (roleform->rolsuper && !superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop role"),
					 errdetail("Only roles with the %s attribute may drop roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
		if (!is_admin_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may drop this role.",
							   "CREATEROLE", "ADMIN", NameStr(roleform->rolname))));

		/* 被删除角色的 DROP 钩子 */
		InvokeObjectDropHook(AuthIdRelationId, roleid, 0);

		/* 不要泄漏 syscache 元组 */
		ReleaseSysCache(tuple);

		/*
         * 锁定角色，以便在我们删除它期间没有人可以添加依赖关系。
         * 我们保留此锁直到事务结束。
         */
		LockSharedObject(AuthIdRelationId, roleid, 0, AccessExclusiveLock);

		/*
         * 如果 pg_auth_members 中存在以要删除角色之一作为
         * roleid 或 member 的条目，则应静默删除；但如果
         * pg_auth_members 中存在以要删除角色之一作为 grantor
         * 的条目，则操作应失败。
         *
         * 但是，单个 pg_auth_members 条目可能属于多个类别 —
         * 例如，用户可能执行 "GRANT foo TO bar GRANTED BY baz"
         * 然后再 "DROP ROLE baz, bar"。我们希望这样的操作成功，
         * 而不管 DROP ROLE 中传入待删除角色的顺序如何。
         *
         * 为了实现这一点，我们在此循环中删除所有可以静默删除的
         * pg_auth_members 条目，然后在下面第二次遍历要删除的角色
         * 列表时，检查任何剩余的依赖关系。
 *
 *
         */
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_roleid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemRoleMemIndexId,
								   true, NULL, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			Form_pg_auth_members authmem_form;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(tmp_tuple);
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemMemRoleIndexId,
								   true, NULL, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			Form_pg_auth_members authmem_form;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(tmp_tuple);
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		/*
         * 推进命令计数器，使得此循环的后续迭代能够看到
         * 已经完成的更改。这一点很关键，例如当我们尝试同时
         * 删除一个角色及其直接成员时 — 如果尝试两次删除
         * 链接的 pg_auth_members 元组将会出错。（上面两个删除
         * 循环之间不需要 CCI，因为不允许角色直接包含自身。）
 *
 *
         */
		CommandCounterIncrement();

		/* 初步看起来可以，如果尚未存在则将其添加到列表中。 */
		role_oids = list_append_unique_oid(role_oids, roleid);
	}

	/*
     * 对要删除的角色进行第二次遍历。
     */
	foreach(item, role_oids)
	{
		Oid			roleid = lfirst_oid(item);
		HeapTuple	tuple;
		Form_pg_authid roleform;
		char	   *detail;
		char	   *detail_log;

		/*
         * 重新查找 pg_authid 元组。
         *
         * 由于我们已经对该角色 OID 进行了锁定，除非用户手动
         * 修改系统目录，否则元组不应该被删除 — 甚至不应该
         * 被更新。
         */
		tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for role %u", roleid);
		roleform = (Form_pg_authid) GETSTRUCT(tuple);

		/*
         * 检查依赖于此角色的 pg_shdepend 条目。
         *
         * 这需要在完成所有可静默删除的 pg_auth_members 条目
         * 之后进行，以避免虚假失败。详见上方的注释。
 *
         */
		if (checkSharedDependencies(AuthIdRelationId, roleid,
									&detail, &detail_log))
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("role \"%s\" cannot be dropped because some objects depend on it",
							NameStr(roleform->rolname)),
					 errdetail_internal("%s", detail),
					 errdetail_log("%s", detail_log)));

		/*
         * 从 pg_authid 表中删除该角色
         */
		CatalogTupleDelete(pg_authid_rel, &tuple->t_self);

		ReleaseSysCache(tuple);

		/*
         * 删除该角色上的任何注释或安全标签。
         */
		DeleteSharedComments(roleid, AuthIdRelationId);
		DeleteSharedSecurityLabel(roleid, AuthIdRelationId);

		/*
         * 删除该角色的设置。
         */
		DropSetting(InvalidOid, roleid);
	}

	/*
     * 现在可以清理了；但保留锁直到提交。
     */
	table_close(pg_auth_members_rel, NoLock);
	table_close(pg_authid_rel, NoLock);
}

/*
 * 重命名角色
 */
ObjectAddress
RenameRole(const char *oldname, const char *newname)
{
	HeapTuple	oldtuple,
				newtuple;
	TupleDesc	dsc;
	Relation	rel;
	Datum		datum;
	bool		isnull;
	Datum		repl_val[Natts_pg_authid];
	bool		repl_null[Natts_pg_authid];
	bool		repl_repl[Natts_pg_authid];
	int			i;
	Oid			roleid;
	ObjectAddress address;
	Form_pg_authid authform;

	rel = table_open(AuthIdRelationId, RowExclusiveLock);
	dsc = RelationGetDescr(rel);

	oldtuple = SearchSysCache1(AUTHNAME, CStringGetDatum(oldname));
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", oldname)));

	/*
     * XXX 客户端应用程序可能在某个地方存储了会话用户，
     * 因此重命名它可能会造成混淆。另一方面，除了让用户
     * 感到困惑之外，可能没有其他实际问题，因此请考虑
     * 这一点并做出决定。对于 SET ROLE 也是如此... 不过，
     * 我们不限制重命名当前有效用户 ID。
     */

	authform = (Form_pg_authid) GETSTRUCT(oldtuple);
	roleid = authform->oid;

	if (roleid == GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("session user cannot be renamed")));
	if (roleid == GetOuterUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current user cannot be renamed")));

	/*
     * 检查用户是否没有尝试重命名系统角色，也没有尝试
     * 将角色重命名为保留的 "pg_" 命名空间中的名称。
     */
	if (IsReservedName(NameStr(authform->rolname)))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						NameStr(authform->rolname)),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	if (IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						newname),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	/*
     * 如果编译时启用了相应开关，当违反回归测试的角色命名约定时
     * 发出警告。
     */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(newname, "regress_", 8) != 0)
		elog(WARNING, "roles created by regression test cases should have names starting with \"regress_\"");
#endif

	/* 确保新名称不存在 */
	if (SearchSysCacheExists1(AUTHNAME, CStringGetDatum(newname)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists", newname)));

	/*
     * 只有超级用户可以操作超级用户。否则，拥有 CREATEROLE 权限的
     * 用户可以重命名其拥有 ADMIN OPTION 的角色。
     */
	if (authform->rolsuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to rename role"),
					 errdetail("Only roles with the %s attribute may rename roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
	}
	else
	{
		if (!have_createrole_privilege() ||
			!is_admin_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to rename role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may rename this role.",
							   "CREATEROLE", "ADMIN", NameStr(authform->rolname))));
	}

	/* 好的，构造修改后的元组 */
	for (i = 0; i < Natts_pg_authid; i++)
		repl_repl[i] = false;

	repl_repl[Anum_pg_authid_rolname - 1] = true;
	repl_val[Anum_pg_authid_rolname - 1] = DirectFunctionCall1(namein,
															   CStringGetDatum(newname));
	repl_null[Anum_pg_authid_rolname - 1] = false;

	datum = heap_getattr(oldtuple, Anum_pg_authid_rolpassword, dsc, &isnull);

	if (!isnull && get_password_type(TextDatumGetCString(datum)) == PASSWORD_TYPE_MD5)
	{
		/* MD5 使用用户名作为 salt，因此重命名时只需清除它 */
		repl_repl[Anum_pg_authid_rolpassword - 1] = true;
		repl_null[Anum_pg_authid_rolpassword - 1] = true;

		ereport(NOTICE,
				(errmsg("MD5 password cleared because of role rename")));
	}

	newtuple = heap_modify_tuple(oldtuple, dsc, repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(rel, &oldtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(AuthIdRelationId, roleid, 0);

	ObjectAddressSet(address, AuthIdRelationId, roleid);

	ReleaseSysCache(oldtuple);

	/*
     * 关闭 pg_authid，但保留锁直到提交。
     */
	table_close(rel, NoLock);

	return address;
}

/*
 * GrantRoleStmt
 *
 * GRANT/REVOKE 角色到/自角色
 */
void
GrantRole(ParseState *pstate, GrantRoleStmt *stmt)
{
	Relation	pg_authid_rel;
	Oid			grantor;
	List	   *grantee_ids;
	ListCell   *item;
	GrantRoleOptions popt;
	Oid			currentUserId = GetUserId();

	/* 解析选项列表。 */
	InitGrantRoleOptions(&popt);
	foreach(item, stmt->opt)
	{
		DefElem    *opt = (DefElem *) lfirst(item);
		char	   *optval = defGetString(opt);

		if (strcmp(opt->defname, "admin") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_ADMIN;

			if (parse_bool(optval, &popt.admin))
				continue;
		}
		else if (strcmp(opt->defname, "inherit") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_INHERIT;
			if (parse_bool(optval, &popt.inherit))
				continue;
		}
		else if (strcmp(opt->defname, "set") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_SET;
			if (parse_bool(optval, &popt.set))
				continue;
		}
		else
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("unrecognized role option \"%s\"", opt->defname),
					parser_errposition(pstate, opt->location));

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized value for role option \"%s\": \"%s\"",
						opt->defname, optval),
				 parser_errposition(pstate, opt->location)));
	}

	/* 如果指定了授权者，查找其 OID。 */
	if (stmt->grantor)
		grantor = get_rolespec_oid(stmt->grantor, false);
	else
		grantor = InvalidOid;

	grantee_ids = roleSpecsToIds(stmt->grantee_roles);

	/* AccessShareLock 就足够了，因为我们不修改 pg_authid */
	pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

	/*
     * 遍历所有被授权的角色，根据需要添加、更新或删除
     * pg_auth_members 中的条目。如果 stmt->is_grant 为 true，
     * 我们正在添加新的授权，或者如果已存在则更新授权上的
     * 选项。如果 stmt->is_grant 为 false，我们正在撤销授权
     * 或移除其中的选项。
     */
	foreach(item, stmt->granted_roles)
	{
		AccessPriv *priv = (AccessPriv *) lfirst(item);
		char	   *rolename = priv->priv_name;
		Oid			roleid;

		/* 必须拒绝 priv(columns) 和 ALL PRIVILEGES(columns) */
		if (rolename == NULL || priv->cols != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("column names cannot be included in GRANT/REVOKE ROLE")));

		roleid = get_role_oid(rolename, false);
		check_role_membership_authorization(currentUserId,
											roleid, stmt->is_grant);
		if (stmt->is_grant)
			AddRoleMems(currentUserId, rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, &popt);
		else
			DelRoleMems(currentUserId, rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, &popt, stmt->behavior);
	}

	/*
     * 关闭 pg_authid，但保留锁直到提交。
     */
	table_close(pg_authid_rel, NoLock);
}

/*
 * DropOwnedObjects
 *
 * 删除给定角色列表所拥有的对象。
 */
void
DropOwnedObjects(DropOwnedStmt *stmt)
{
	List	   *role_ids = roleSpecsToIds(stmt->roles);
	ListCell   *cell;

	/* 检查权限 */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop objects"),
					 errdetail("Only roles with privileges of role \"%s\" may drop objects owned by it.",
							   GetUserNameFromId(roleid, false))));
	}

	/* 好的，执行 */
	shdepDropOwned(role_ids, stmt->behavior);
}

/*
 * ReassignOwnedObjects
 *
 * 将给定角色列表所拥有的对象转让给另一个用户。
 */
void
ReassignOwnedObjects(ReassignOwnedStmt *stmt)
{
	List	   *role_ids = roleSpecsToIds(stmt->roles);
	ListCell   *cell;
	Oid			newrole;

	/* 检查权限 */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to reassign objects"),
					 errdetail("Only roles with privileges of role \"%s\" may reassign objects owned by it.",
							   GetUserNameFromId(roleid, false))));
	}

	/* 在接收方也必须具有权限 */
	newrole = get_rolespec_oid(stmt->newrole, false);

	if (!has_privs_of_role(GetUserId(), newrole))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reassign objects"),
				 errdetail("Only roles with privileges of role \"%s\" may reassign objects to it.",
						   GetUserNameFromId(newrole, false))));

	/* 好的，执行 */
	shdepReassignOwned(role_ids, newrole);
}

/*
 * roleSpecsToIds
 *
 * 给定 RoleSpec 列表，按相同顺序生成角色 OID 列表。
 *
 * 不允许 ROLESPEC_PUBLIC。
 */
List *
roleSpecsToIds(List *memberNames)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, memberNames)
	{
		RoleSpec   *rolespec = lfirst_node(RoleSpec, l);
		Oid			roleid;

		roleid = get_rolespec_oid(rolespec, false);
		result = lappend_oid(result, roleid);
	}
	return result;
}

/*
 * AddRoleMems -- 将给定成员添加到指定角色
 *
 * currentUserId: 执行操作的角色的 OID
 * rolename: 要添加到的角色名称（仅用于错误消息）
 * roleid: 要添加到的角色的 OID
 * memberSpecs: 要添加的角色的 RoleSpec 列表（仅用于错误消息）
 * memberIds: 要添加的角色的 OID
 * grantorId: 应记录为授权该成员关系的 OID
 * （如果未显式设置则为 InvalidOid）
 * popt: 关于授权选项的信息
 */
static void
AddRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
			List *memberSpecs, List *memberIds,
			Oid grantorId, GrantRoleOptions *popt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *specitem;
	ListCell   *iditem;

	Assert(list_length(memberSpecs) == list_length(memberIds));

	/* 验证授权者（如果未指定则解析隐式授权者）。 */
	grantorId = check_role_grantor(currentUserId, roleid, grantorId, true);

	pg_authmem_rel = table_open(AuthMemRelationId, RowExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	/*
     * 一次只允许一个后端更改此角色，这样我们就可以检查
     * 完整性约束（如防止循环 ADMIN OPTION 授权），
     * 而不必担心竞争条件。
     */
	LockSharedObject(AuthIdRelationId, roleid, 0,
					 ShareUpdateExclusiveLock);

	/* 初步合理性检查。 */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst_node(RoleSpec, specitem);
		Oid			memberid = lfirst_oid(iditem);

		/*
         * pg_database_owner 从来不是角色成员。解除此限制
         * 需要对成员关系环做出策略决策。一种方案是防止环，
         * 这将包括使 "ALTER DATABASE x OWNER TO proposed_datdba"
         * 在 is_member_of_role(pg_database_owner, proposed_datdba)
         * 为真时失败。因此，获得一个成员关系可能会减少
         * 角色可以执行的操作。另一种方案是允许这些成员关系
         * 完成环。然后角色实际上可以对自己拥有 WITH ADMIN OPTION，
         * 从而引发关于 is_admin_of_role() 如何处理这种情况的决策。
         *
         * 解除此限制还对共享对象（数据库和表空间）的所有权
         * 有策略影响。我们允许这样的所有权，但可能在未来
         * 找到禁止它的理由。如果设计必须处理 pg_database_owner
         * 是拥有共享对象的角色 FOO 的成员的情况，设计这种
         * 禁止将更加棘手。（这种所有权的效果是任何其他数据库
         * 的所有者都可以作为受影响的共享对象的所有者行事。）
 *
 *
         */
		if (memberid == ROLE_PG_DATABASE_OWNER)
			ereport(ERROR,
					errmsg("role \"%s\" cannot be a member of any role",
						   get_rolespec_name(memberRole)));

		/*
         * 拒绝创建成员关系环，包括角色成为自身成员的平凡情况。
         * 我们通过检查目标角色是否已经是所提议成员角色的成员
         * 来实现。但是，我们必须忽略可能的超级用户身份，
         * 否则我们永远无法授予超级用户特权角色中的成员资格。
 *
         */
		if (is_member_of_role_nosuper(roleid, memberid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("role \"%s\" is a member of role \"%s\"",
							rolename, get_rolespec_name(memberRole))));
	}

	/*
     * 禁止尝试将 ADMIN OPTION 授权回给您授予它的用户，
     * 类似于 check_circularity 对 ACL 所做的处理。我们希望
     * 授权链保持无环，以便始终可以使用 REVOKE .. CASCADE
     * 清理所有依赖于被撤销授权的授权。
     *
     * 注意：此检查可能看起来与上面的成员关系环检查重复，
     * 但实际上不是。上面检查的是角色-成员环
     * （例如 A 是 B 的成员且 B 是 A 的成员），
     * 而此处检查的是成员-授权者环
     * （例如 A 授权了 B 对 X 的 ADMIN OPTION，而 B 没有
     * 来自其他来源的对 X 的 ADMIN OPTION，现在试图将
     * 对 X 的 ADMIN OPTION 授权回 A）。
     */
	if (popt->admin && grantorId != BOOTSTRAP_SUPERUSERID)
	{
		CatCList   *memlist;
		RevokeRoleGrantAction *actions;
		int			i;

		/* 获取此角色的成员列表。 */
		memlist = SearchSysCacheList1(AUTHMEMROLEMEM,
									  ObjectIdGetDatum(roleid));

		/*
         * 计算如果我们移除每个被请求进行新授权的角色的
         * 所有现有授权后会发生什么。
         */
		actions = initialize_revoke_actions(memlist);
		foreach(iditem, memberIds)
		{
			Oid			memberid = lfirst_oid(iditem);

			if (memberid == BOOTSTRAP_SUPERUSERID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("%s option cannot be granted back to your own grantor",
								"ADMIN")));
			plan_member_revoke(memlist, actions, memberid);
		}

		/*
         * 如果结果是授权者角色将不再具有执行授权的
         * 能力，那么提议的授权将导致循环。
 *
         */
		for (i = 0; i < memlist->n_members; ++i)
		{
			HeapTuple	authmem_tuple;
			Form_pg_auth_members authmem_form;

			authmem_tuple = &memlist->members[i]->tuple;
			authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

			if (actions[i] == RRG_NOOP &&
				authmem_form->member == grantorId &&
				authmem_form->admin_option)
				break;
		}
		if (i >= memlist->n_members)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("%s option cannot be granted back to your own grantor",
							"ADMIN")));

		ReleaseSysCacheList(memlist);
	}

	/* 现在执行目录更新。 */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst_node(RoleSpec, specitem);
		Oid			memberid = lfirst_oid(iditem);
		HeapTuple	authmem_tuple;
		HeapTuple	tuple;
		Datum		new_record[Natts_pg_auth_members] = {0};
		bool		new_record_nulls[Natts_pg_auth_members] = {0};
		bool		new_record_repl[Natts_pg_auth_members] = {0};

		/* 可能插入或更新的公共初始化 */
		new_record[Anum_pg_auth_members_roleid - 1] =
			ObjectIdGetDatum(roleid);
		new_record[Anum_pg_auth_members_member - 1] =
			ObjectIdGetDatum(memberid);
		new_record[Anum_pg_auth_members_grantor - 1] =
			ObjectIdGetDatum(grantorId);

		/* 查找任何现有元组 */
		authmem_tuple = SearchSysCache3(AUTHMEMROLEMEM,
										ObjectIdGetDatum(roleid),
										ObjectIdGetDatum(memberid),
										ObjectIdGetDatum(grantorId));

		/*
         * 如果找到了元组，则用新选项值更新它，
         * 除非没有变化，此时发出 WARNING。
         *
         * 如果没有找到元组，则直接插入一个。
         */
		if (HeapTupleIsValid(authmem_tuple))
		{
			Form_pg_auth_members authmem_form;
			bool		at_least_one_change = false;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

			if ((popt->specified & GRANT_ROLE_SPECIFIED_ADMIN) != 0
				&& authmem_form->admin_option != popt->admin)
			{
				new_record[Anum_pg_auth_members_admin_option - 1] =
					BoolGetDatum(popt->admin);
				new_record_repl[Anum_pg_auth_members_admin_option - 1] =
					true;
				at_least_one_change = true;
			}

			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0
				&& authmem_form->inherit_option != popt->inherit)
			{
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					BoolGetDatum(popt->inherit);
				new_record_repl[Anum_pg_auth_members_inherit_option - 1] =
					true;
				at_least_one_change = true;
			}

			if ((popt->specified & GRANT_ROLE_SPECIFIED_SET) != 0
				&& authmem_form->set_option != popt->set)
			{
				new_record[Anum_pg_auth_members_set_option - 1] =
					BoolGetDatum(popt->set);
				new_record_repl[Anum_pg_auth_members_set_option - 1] =
					true;
				at_least_one_change = true;
			}

			if (!at_least_one_change)
			{
				ereport(NOTICE,
						(errmsg("role \"%s\" has already been granted membership in role \"%s\" by role \"%s\"",
								get_rolespec_name(memberRole), rolename,
								GetUserNameFromId(grantorId, false))));
				ReleaseSysCache(authmem_tuple);
				continue;
			}

			tuple = heap_modify_tuple(authmem_tuple, pg_authmem_dsc,
									  new_record,
									  new_record_nulls, new_record_repl);
			CatalogTupleUpdate(pg_authmem_rel, &tuple->t_self, tuple);

			ReleaseSysCache(authmem_tuple);
		}
		else
		{
			Oid			objectId;
			Oid		   *newmembers = palloc(sizeof(Oid));

			/*
             * 这些选项的值可以直接取自 'popt'。要么它们已通过
             * 参数指定，要么由 InitGrantRoleOptions 设置的默认值
             * 是正确的。
             */
			new_record[Anum_pg_auth_members_admin_option - 1] =
				BoolGetDatum(popt->admin);
			new_record[Anum_pg_auth_members_set_option - 1] =
				BoolGetDatum(popt->set);

			/*
             * 如果用户指定了 inherit 选项的值，则使用该值。
             * 否则，根据角色级别的属性设置默认值。
 *
             */
			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0)
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					popt->inherit;
			else
			{
				HeapTuple	mrtup;
				Form_pg_authid mrform;

				mrtup = SearchSysCache1(AUTHOID, memberid);
				if (!HeapTupleIsValid(mrtup))
					elog(ERROR, "cache lookup failed for role %u", memberid);
				mrform = (Form_pg_authid) GETSTRUCT(mrtup);
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					mrform->rolinherit;
				ReleaseSysCache(mrtup);
			}

			/* 为新行获取一个 OID 并插入 */
			objectId = GetNewOidWithIndex(pg_authmem_rel, AuthMemOidIndexId,
										  Anum_pg_auth_members_oid);
			new_record[Anum_pg_auth_members_oid - 1] = objectId;
			tuple = heap_form_tuple(pg_authmem_dsc,
									new_record, new_record_nulls);
			CatalogTupleInsert(pg_authmem_rel, tuple);

			/* updateAclDependencies 想要 pfree 数组输入 */
			newmembers[0] = grantorId;
			updateAclDependencies(AuthMemRelationId, objectId,
								  0, InvalidOid,
								  0, NULL,
								  1, newmembers);
		}

		/* 每次更改后 CCI，以处理列表中的重复项 */
		CommandCounterIncrement();
	}

	/*
     * 关闭 pg_authmem，但保留锁直到提交。
     */
	table_close(pg_authmem_rel, NoLock);
}

/*
 * DelRoleMems -- 从指定角色中移除给定成员
 *
 * rolename: 要从中删除的角色名称（仅用于错误消息）
 * roleid: 要从中删除的角色的 OID
 * memberSpecs: 要删除的角色的 RoleSpec 列表（仅用于错误消息）
 * memberIds: 要删除的角色的 OID
 * grantorId: 撤销成员关系者
 * popt: 关于授权选项的信息
 * behavior: 递归删除的 RESTRICT 或 CASCADE 行为
 */
static void
DelRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
			List *memberSpecs, List *memberIds,
			Oid grantorId, GrantRoleOptions *popt, DropBehavior behavior)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *specitem;
	ListCell   *iditem;
	CatCList   *memlist;
	RevokeRoleGrantAction *actions;
	int			i;

	Assert(list_length(memberSpecs) == list_length(memberIds));

	/* 验证授权者（如果未指定则解析隐式授权者）。 */
	grantorId = check_role_grantor(currentUserId, roleid, grantorId, false);

	pg_authmem_rel = table_open(AuthMemRelationId, RowExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	/*
     * 一次只允许一个后端更改此角色，这样我们就可以检查
     * 诸如依赖权限之类的事项，而不必担心竞争条件。
 *
     */
	LockSharedObject(AuthIdRelationId, roleid, 0,
					 ShareUpdateExclusiveLock);

	memlist = SearchSysCacheList1(AUTHMEMROLEMEM, ObjectIdGetDatum(roleid));
	actions = initialize_revoke_actions(memlist);

	/*
     * 如果指定了 DROP_CASCADE，我们可能需要递归到依赖
     * 的权限；如果存在依赖权限且指定了 DROP_RESTRICT，
     * 则需要拒绝执行操作。plan_single_revoke() 将决定
     * 如何处理每个目录元组。
     */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst(specitem);
		Oid			memberid = lfirst_oid(iditem);

		if (!plan_single_revoke(memlist, actions, memberid, grantorId,
								popt, behavior))
		{
			ereport(WARNING,
					(errmsg("role \"%s\" has not been granted membership in role \"%s\" by role \"%s\"",
							get_rolespec_name(memberRole), rolename,
							GetUserNameFromId(grantorId, false))));
			continue;
		}
	}

	/*
     * 现在我们知道了如何处理每个目录元组：它应该被
     * 保持不变、删除，或者只是清除 admin_option 标志。
     * 在每种情况下执行适当的操作。
     */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		if (actions[i] == RRG_NOOP)
			continue;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (actions[i] == RRG_DELETE_GRANT)
		{
			/*
             * 完全删除条目，首先删除其依赖关系
 *
             */
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_authmem_rel, &authmem_tuple->t_self);
		}
		else
		{
			/* 仅关闭指定的选项 */
			HeapTuple	tuple;
			Datum		new_record[Natts_pg_auth_members] = {0};
			bool		new_record_nulls[Natts_pg_auth_members] = {0};
			bool		new_record_repl[Natts_pg_auth_members] = {0};

			/* 构建一个用于更新的元组 */
			if (actions[i] == RRG_REMOVE_ADMIN_OPTION)
			{
				new_record[Anum_pg_auth_members_admin_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_admin_option - 1] =
					true;
			}
			else if (actions[i] == RRG_REMOVE_INHERIT_OPTION)
			{
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_inherit_option - 1] =
					true;
			}
			else if (actions[i] == RRG_REMOVE_SET_OPTION)
			{
				new_record[Anum_pg_auth_members_set_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_set_option - 1] =
					true;
			}
			else
				elog(ERROR, "unknown role revoke action");

			tuple = heap_modify_tuple(authmem_tuple, pg_authmem_dsc,
									  new_record,
									  new_record_nulls, new_record_repl);
			CatalogTupleUpdate(pg_authmem_rel, &tuple->t_self, tuple);
		}
	}

	ReleaseSysCacheList(memlist);

	/*
     * 关闭 pg_authmem，但保留锁直到提交。
     */
	table_close(pg_authmem_rel, NoLock);
}

/*
 * 检查 currentUserId 是否有权修改 roleid 的成员列表。
 * 如果没有则抛出错误。
 */
static void
check_role_membership_authorization(Oid currentUserId, Oid roleid,
									bool is_grant)
{
	/*
     * pg_database_owner 的职责是拥有正好一个隐式的、
     * 依赖于情境的成员。这个限制没有技术上的必要性。
     * （可以解除它，并进一步使
     * object_ownercheck(DatabaseRelationId, ...) 等同于
     * has_privs_of_role(roleid, ROLE_PG_DATABASE_OWNER)，
     * 在这种情况下，显式的、不依赖于情境的成员可以充当
     * 任何数据库的所有者。）
     */
	if (is_grant && roleid == ROLE_PG_DATABASE_OWNER)
		ereport(ERROR,
				errmsg("role \"%s\" cannot have explicit members",
					   GetUserNameFromId(roleid, false)));

	/* 要操作超级用户角色，您必须是超级用户。 */
	if (superuser_arg(roleid))
	{
		if (!superuser_arg(currentUserId))
		{
			if (is_grant)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to grant role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s attribute may grant roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to revoke role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s attribute may revoke roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
		}
	}
	else
	{
		/*
         * 否则，必须对要更改的角色具有 admin 选项。
         */
		if (!is_admin_of_role(currentUserId, roleid))
		{
			if (is_grant)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to grant role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s option on role \"%s\" may grant this role.",
								   "ADMIN", GetUserNameFromId(roleid, false))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to revoke role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s option on role \"%s\" may revoke this role.",
								   "ADMIN", GetUserNameFromId(roleid, false))));
		}
	}
}

/*
 * 对 GRANT 或 REVOKE 语句（目标为角色）检查或推断    
 * 其授权者。  
 *
 * 授权者必须始终是对被授予成员资格的角色拥有          
 *  ADMIN OPTION 的角色，或者是引导超级用户。这           
 * 与 select_best_grantor 强制执行的限制类似，不同之处在  
 * 于角色没有所有者，因此我们把引导超级用户视为  
 * 隐式所有者
 *
 * 如果用户没有显式指定授权者，则 grantorId 应当传入     
 *  InvalidOid，本函数会推断出要记录为授权者的用户。
 * 在许多情况下，这将是当前用户，但当当前用户并不     
 * 拥有该角色的 ADMIN OPTION 而是依靠 SUPERUSER 特权，       
 * 或者是依靠继承了确实拥有该角色 ADMIN OPTION 的角      
 * 色的特权时，情况就会变得更复杂。详见下文            
 * 。               
 *
 * 如果授权者是由用户指定的，那么它必须是能够合法
 * 地被记录为授权者的用户，符合上述规则。            
 * 这是一条完整性约束，而不是权限检查，因此就连超  
 * 级用户也受此限制。不过，还有一道权限检查：     
 * 要把一个角色指定为授权者，当前用户就必须拥有该
 * 角色的权限。超级用户将总是能通过此检查，但对   
 * 非超级用户则可能导致错误。                    
 *
 * 返回值是执行该操作时将被视为授权者的 OID              
 * 。           
 */
static Oid
check_role_grantor(Oid currentUserId, Oid roleid, Oid grantorId, bool is_grant)
{
	/* 如果未指定授权者 ID，则选择一个。 */
	if (!OidIsValid(grantorId))
	{
		/*
         * 将授权者记录为引导超级用户的授权不依赖于任何
         * 其他现有授权，因此在可能的情况下始终默认使用
         * 这种解释。
         */
		if (superuser_arg(currentUserId))
			return BOOTSTRAP_SUPERUSERID;

		/*
         * 否则，授权者必须或者对该角色具有 ADMIN OPTION，
         * 或者继承具有 ADMIN OPTION 的角色的权限。
         * 在前一种情况下，将授权者记录为当前用户；
         * 在后一种情况下，选择当前角色"最直接"继承的
         * 角色之一（即最少的"跳数"）。
         *
         * （我们不应当找不到最佳授权者，因为我们已经
         * 确定当前用户有权执行操作。）
 *
         */
		grantorId = select_best_admin(currentUserId, roleid);
		if (!OidIsValid(grantorId))
			elog(ERROR, "no possible grantors");
		return grantorId;
	}

	/*
     * 如果指定了显式授权者，则该授权者必须是当前用户
     * 拥有其权限的角色。
     *
     * 它还应是对目标角色具有 ADMIN OPTION 的角色，
     * 但我们只在 GRANT 情况下检查此条件。对于 REVOKE，
     * 不应存在匹配的授权，但如果由于某种原因确实存在，
     * 则让用户将其删除。
     */
	if (is_grant)
	{
		if (!has_privs_of_role(currentUserId, grantorId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to grant privileges as role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("Only roles with privileges of role \"%s\" may grant privileges as this role.",
							   GetUserNameFromId(grantorId, false))));

		if (grantorId != BOOTSTRAP_SUPERUSERID &&
			select_best_admin(grantorId, roleid) != grantorId)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to grant privileges as role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("The grantor must have the %s option on role \"%s\".",
							   "ADMIN", GetUserNameFromId(roleid, false))));
	}
	else
	{
		if (!has_privs_of_role(currentUserId, grantorId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to revoke privileges granted by role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("Only roles with privileges of role \"%s\" may revoke privileges granted by this role.",
							   GetUserNameFromId(grantorId, false))));
	}

	/*
     * 如果显式指定了授权者，始终将授权归于该角色
     * （除非上面出错）。
     */
	return grantorId;
}

/*
 * 初始化 RevokeRoleGrantAction 对象数组。
 *
 * 'memlist' 应为目标角色的所有授权列表。
 *
 * 此函数构造一个数组，指示不执行任何操作；
 * 即每个元素初始化为 RRG_NOOP。
 */
static RevokeRoleGrantAction *
initialize_revoke_actions(CatCList *memlist)
{
	RevokeRoleGrantAction *result;
	int			i;

	if (memlist->n_members == 0)
		return NULL;

	result = palloc(sizeof(RevokeRoleGrantAction) * memlist->n_members);
	for (i = 0; i < memlist->n_members; i++)
		result[i] = RRG_NOOP;
	return result;
}

/*
 * 确定为了撤销一个授权（或仅撤销该授权的 admin 选项）
 * 需要做什么，考虑到可能存在依赖权限。
 *
 * 'memlist' 应为目标角色的所有授权列表。
 *
 * 任何证明是必要的操作将通过更新 'actions' 来表示。
 *
 * 如果 behavior 是 DROP_RESTRICT，则存在依赖角色成员关系
 * 授权时将发生错误；如果是 DROP_CASCADE，这些授权将被安排
 * 删除。
 *
 * 如果在列表中找到了匹配的授权，返回值为 true，
 * 否则为 false。
 *
 */
static bool
plan_single_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
				   Oid member, Oid grantor, GrantRoleOptions *popt,
				   DropBehavior behavior)
{
	int			i;

	/*
     * 如果 popt.specified == 0，我们正在完全撤销授权；
     * 否则，我们期望只设置了一个位，并且正在撤销相应的
     * 选项。截至本文撰写时，没有允许一次尝试撤销多个选项
     * 的语法，并且如果添加了这样的语法，下面的逻辑将无法
     * 正确工作，因此断言我们的调用者没有尝试这样做。
 *
     */
	Assert(pg_popcount32(popt->specified) <= 1);

	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (authmem_form->member == member &&
			authmem_form->grantor == grantor)
		{
			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0)
			{
				/*
                 * 撤销 INHERIT 选项不会改变依赖权限的
                 * 任何内容，因此不需要递归。
                 */
				actions[i] = RRG_REMOVE_INHERIT_OPTION;
			}
			else if ((popt->specified & GRANT_ROLE_SPECIFIED_SET) != 0)
			{
				/* 此处也不需要递归。 */
				actions[i] = RRG_REMOVE_SET_OPTION;
			}
			else
			{
				bool		revoke_admin_option_only;

				/*
                 * 完全撤销授权或撤销授权的 ADMIN 选项
                 * 会涉及依赖权限，因此可能需要递归。
                 */
				revoke_admin_option_only =
					(popt->specified & GRANT_ROLE_SPECIFIED_ADMIN) != 0;
				plan_recursive_revoke(memlist, actions, i,
									  revoke_admin_option_only, behavior);
			}
			return true;
		}
	}

	return false;
}

/*
 * 确定为了撤销对给定成员的所有授权需要做什么，
 * 考虑到可能存在依赖权限。
 *
 * 'memlist' 应为目标角色的所有授权列表。
 *
 * 任何证明是必要的操作将通过更新 'actions' 来表示。
 *
 */
static void
plan_member_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
				   Oid member)
{
	int			i;

	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (authmem_form->member == member)
			plan_recursive_revoke(memlist, actions, i, false, DROP_CASCADE);
	}
}

/*
 * 确定递归撤销角色授权的主力函数。
 *
 * 这类似于 recursive_revoke() 对 ACL 所做的处理。
 */
static void
plan_recursive_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
					  int index,
					  bool revoke_admin_option_only, DropBehavior behavior)
{
	bool		would_still_have_admin_option = false;
	HeapTuple	authmem_tuple;
	Form_pg_auth_members authmem_form;
	int			i;

	/* 如果已经完成了，则直接返回。 */
	if (actions[index] == RRG_DELETE_GRANT)
		return;
	if (actions[index] == RRG_REMOVE_ADMIN_OPTION &&
		revoke_admin_option_only)
		return;

	/* 定位元组数据。 */
	authmem_tuple = &memlist->members[index]->tuple;
	authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

	/*
     * 如果现有元组没有设置 admin_option，那么我们不需要
     * 递归。如果我们只是应该清除该位，那么根本不需要做
     * 任何事情；如果我们应该删除授权，那么只需要对元组
     * 执行操作，而不需要涉及其他元组。
     */
	if (!revoke_admin_option_only)
	{
		actions[index] = RRG_DELETE_GRANT;
		if (!authmem_form->admin_option)
			return;
	}
	else
	{
		if (!authmem_form->admin_option)
			return;
		actions[index] = RRG_REMOVE_ADMIN_OPTION;
	}

	/* 确定该成员是否仍将具有 ADMIN OPTION。 */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	am_cascade_tuple;
		Form_pg_auth_members am_cascade_form;

		am_cascade_tuple = &memlist->members[i]->tuple;
		am_cascade_form = (Form_pg_auth_members) GETSTRUCT(am_cascade_tuple);

		if (am_cascade_form->member == authmem_form->member &&
			am_cascade_form->admin_option && actions[i] == RRG_NOOP)
		{
			would_still_have_admin_option = true;
			break;
		}
	}

	/* 如果该成员仍将具有 ADMIN OPTION，则不需要递归。 */
	if (would_still_have_admin_option)
		return;

	/*
     * 递归到尚未计划删除的、以此成员作为授权者的授权。
 *
     */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	am_cascade_tuple;
		Form_pg_auth_members am_cascade_form;

		am_cascade_tuple = &memlist->members[i]->tuple;
		am_cascade_form = (Form_pg_auth_members) GETSTRUCT(am_cascade_tuple);

		if (am_cascade_form->grantor == authmem_form->member &&
			actions[i] != RRG_DELETE_GRANT)
		{
			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			plan_recursive_revoke(memlist, actions, i, false, behavior);
		}
	}
}

/*
 * 使用默认值初始化 GrantRoleOptions 对象。
 */
static void
InitGrantRoleOptions(GrantRoleOptions *popt)
{
	popt->specified = 0;
	popt->admin = false;
	popt->inherit = false;
	popt->set = true;
}

/*
 * createrole_self_grant 的 GUC check_hook
 */
bool
check_createrole_self_grant(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	unsigned	options = 0;
	unsigned   *result;

	/* 需要一个可修改的字符串副本 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 列表语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "SET") == 0)
			options |= GRANT_ROLE_SPECIFIED_SET;
		else if (pg_strcasecmp(tok, "INHERIT") == 0)
			options |= GRANT_ROLE_SPECIFIED_INHERIT;
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	result = (unsigned *) guc_malloc(LOG, sizeof(unsigned));
	if (!result)
		return false;
	*result = options;
	*extra = result;

	return true;
}

/*
 * createrole_self_grant 的 GUC assign_hook
 */
void
assign_createrole_self_grant(const char *newval, void *extra)
{
	unsigned	options = *(unsigned *) extra;

	createrole_self_grant_enabled = (options != 0);
	createrole_self_grant_options.specified = GRANT_ROLE_SPECIFIED_ADMIN
		| GRANT_ROLE_SPECIFIED_INHERIT
		| GRANT_ROLE_SPECIFIED_SET;
	createrole_self_grant_options.admin = false;
	createrole_self_grant_options.inherit =
		(options & GRANT_ROLE_SPECIFIED_INHERIT) != 0;
	createrole_self_grant_options.set =
		(options & GRANT_ROLE_SPECIFIED_SET) != 0;
}
