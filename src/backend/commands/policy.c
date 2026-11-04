/*-------------------------------------------------------------------------
 *
 * policy.c
 *	  策略操作命令。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/policy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_type.h"
#include "commands/policy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void RangeVarCallbackForPolicy(const RangeVar *rv,
									  Oid relid, Oid oldrelid, void *arg);
static char parse_policy_command(const char *cmd_name);
static Datum *policy_role_list_to_array(List *roles, int *num_roles);

/*
 * RangeVarGetRelidExtended() 的回调函数。
 *
 * 检查以下内容：
 *	- 指定的关系是表。
 *	- 当前用户拥有该表。
 *	- 该表不是系统表。
 *
 * 如果任一检查失败，则引发错误。
 */
static void
RangeVarCallbackForPolicy(const RangeVar *rv, Oid relid, Oid oldrelid,
						  void *arg)
{
	HeapTuple	tuple;
	Form_pg_class classform;
	char		relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;

	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* 必须拥有关系。 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	/* 除非明确允许，否则不允许修改系统表。 */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/* 关系类型必须是表。 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 * parse_policy_command -
 *	 辅助函数，将完整的命令字符串转换为其字符 
 *	 表示形式。
 *
 * cmd_name - 完整命令名字符串。有效值有 'all'、'select'、
 *			  'insert'、'update'与'delete'  
 *
 */
static char
parse_policy_command(const char *cmd_name)
{
	char		polcmd;

	if (!cmd_name)
		elog(ERROR, "unrecognized policy command");

	if (strcmp(cmd_name, "all") == 0)
		polcmd = '*';
	else if (strcmp(cmd_name, "select") == 0)
		polcmd = ACL_SELECT_CHR;
	else if (strcmp(cmd_name, "insert") == 0)
		polcmd = ACL_INSERT_CHR;
	else if (strcmp(cmd_name, "update") == 0)
		polcmd = ACL_UPDATE_CHR;
	else if (strcmp(cmd_name, "delete") == 0)
		polcmd = ACL_DELETE_CHR;
	else
		elog(ERROR, "unrecognized policy command");

	return polcmd;
}

/*
 * policy_role_list_to_array
 *	 将 RoleSpec 列表转换为角色 ID Datum 数组。
*
 */
static Datum *
policy_role_list_to_array(List *roles, int *num_roles)
{
	Datum	   *role_oids;
	ListCell   *cell;
	int			i = 0;

	/* 未传入角色则视为 public */
	if (roles == NIL)
	{
		*num_roles = 1;
		role_oids = (Datum *) palloc(*num_roles * sizeof(Datum));
		role_oids[0] = ObjectIdGetDatum(ACL_ID_PUBLIC);

		return role_oids;
	}

	*num_roles = list_length(roles);
	role_oids = (Datum *) palloc(*num_roles * sizeof(Datum));

	foreach(cell, roles)
	{
		RoleSpec   *spec = lfirst(cell);

		/*
		 * PUBLIC 涵盖所有角色，因此应单独使用。
		 */
		if (spec->roletype == ROLESPEC_PUBLIC)
		{
			if (*num_roles != 1)
			{
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("ignoring specified roles other than PUBLIC"),
						 errhint("All roles are members of the PUBLIC role.")));
				*num_roles = 1;
			}
			role_oids[0] = ObjectIdGetDatum(ACL_ID_PUBLIC);

			return role_oids;
		}
		else
			role_oids[i++] =
				ObjectIdGetDatum(get_rolespec_oid(spec, false));
	}

	return role_oids;
}

/*
 * 从系统目录加载行级安全策略，存入关系的
 * relcache 条目。
 *
 * 调用者应已验证此关系的 pg_class.relrowsecurity 为 true。
*
 */
void
RelationBuildRowSecurity(Relation relation)
{
	MemoryContext rscxt;
	MemoryContext oldcxt = CurrentMemoryContext;
	RowSecurityDesc *rsdesc;
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;

	/*
	 * 创建内存上下文存放与此关系行级安全策略关联的全部内容。
	 * 便于 relcache 刷新时清理。为应对中途出错，
	 * 完成前不使上下文长期存在。
	*
	 */
	rscxt = AllocSetContextCreate(CurrentMemoryContext,
								  "row security descriptor",
								  ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(rscxt,
									  RelationGetRelationName(relation));

	rsdesc = MemoryContextAllocZero(rscxt, sizeof(RowSecurityDesc));
	rsdesc->rscxt = rscxt;

	/*
	 * 扫描 pg_policy 中与此关系关联的 RLS 策略。
	 * 使用 (polrelid, polname) 索引按名称顺序访问策略，
	 * 系统索引未禁用时保持一致，简化 equalRSDesc()。
	*
	 */
	catalog = table_open(PolicyRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	sscan = systable_beginscan(catalog, PolicyPolrelidPolnameIndexId, true,
							   NULL, 1, &skey);

	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		Form_pg_policy policy_form = (Form_pg_policy) GETSTRUCT(tuple);
		RowSecurityPolicy *policy;
		Datum		datum;
		bool		isnull;
		char	   *str_value;

		policy = MemoryContextAllocZero(rscxt, sizeof(RowSecurityPolicy));

		/*
		 * 确保按引用传递的数据被复制到 rscxt。尽量不在
		 * 不必要的大跨度上将上下文设为当前上下文。
		*
		 */

		/* 获取策略命令 */
		policy->polcmd = policy_form->polcmd;

		/* 获取策略类型：宽松或限制 */
		policy->permissive = policy_form->polpermissive;

		/* 获取策略名称 */
		policy->policy_name =
			MemoryContextStrdup(rscxt, NameStr(policy_form->polname));

		/* 获取策略角色 */
		datum = heap_getattr(tuple, Anum_pg_policy_polroles,
							 RelationGetDescr(catalog), &isnull);
		/* 不应为 null，谨慎起见检查 */
		if (isnull)
			elog(ERROR, "unexpected null value in pg_policy.polroles");
		MemoryContextSwitchTo(rscxt);
		policy->roles = DatumGetArrayTypePCopy(datum);
		MemoryContextSwitchTo(oldcxt);

		/* 获取策略限定条件 */
		datum = heap_getattr(tuple, Anum_pg_policy_polqual,
							 RelationGetDescr(catalog), &isnull);
		if (!isnull)
		{
			str_value = TextDatumGetCString(datum);
			MemoryContextSwitchTo(rscxt);
			policy->qual = (Expr *) stringToNode(str_value);
			MemoryContextSwitchTo(oldcxt);
			pfree(str_value);
		}
		else
			policy->qual = NULL;

		/* 获取 WITH CHECK 限定条件 */
		datum = heap_getattr(tuple, Anum_pg_policy_polwithcheck,
							 RelationGetDescr(catalog), &isnull);
		if (!isnull)
		{
			str_value = TextDatumGetCString(datum);
			MemoryContextSwitchTo(rscxt);
			policy->with_check_qual = (Expr *) stringToNode(str_value);
			MemoryContextSwitchTo(oldcxt);
			pfree(str_value);
		}
		else
			policy->with_check_qual = NULL;

		/* 缓存这些表达式中是否有 SubLinks */
		policy->hassublinks = checkExprHasSubLink((Node *) policy->qual) ||
			checkExprHasSubLink((Node *) policy->with_check_qual);

		/*
		 * 将对象添加到列表。因历史原因，列表按逆序构建。
		*
		 */
		MemoryContextSwitchTo(rscxt);
		rsdesc->policies = lcons(policy, rsdesc->policies);
		MemoryContextSwitchTo(oldcxt);
	}

	systable_endscan(sscan);
	table_close(catalog, AccessShareLock);

	/*
	 * 成功。将描述符的内存上下文重挂到 CacheMemoryContext 下
	 * 使其永久存在，然后将策略描述符附加到 relcache 条目。
	*
	 */
	MemoryContextSetParent(rscxt, CacheMemoryContext);

	relation->rd_rsdesc = rsdesc;
}

/*
 * RemovePolicyById -
 *	 通过 OID 删除一个策略。如果提供的 OID 没有对应的      
 *	 策略，则会报错。     
 *
 * policy_id - 策略的 OID。      
 */
void
RemovePolicyById(Oid policy_id)
{
	Relation	pg_policy_rel;
	SysScanDesc sscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;
	Oid			relid;
	Relation	rel;

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/*
	 * 找到待删除的策略。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
							   NULL, 1, skey);

	tuple = systable_getnext(sscan);

	/* 策略存在则删除，否则报错。 */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for policy %u", policy_id);

	/*
	 * 打开策略所属的关系并对其加排他锁。（我们需要   
	 * 排他锁来阻止那些可能依赖该关系现有策略集合的   
	 * 查询；此外我们必须持有该锁直至提交。）          
	 *              
	 */
	relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;

	rel = table_open(relid, AccessExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	CatalogTupleDelete(pg_policy_rel, &tuple->t_self);

	systable_endscan(sscan);

	/*
	 * 注意，与 pg_class 中的某些其他标志不同，relrowsecurity
	 * 并不只是指示是否存在策略。当用户设置它后，        
	 * 对关系的所有访问都必须通过策略。                
	 * 如果没有为该关系定义任何策略，则会创建一个默认
	 * 拒绝策略，并过滤所有记录（来自所有者的查询除外  
	 * ）。     
	 */
	CacheInvalidateRelcache(rel);

	table_close(rel, NoLock);

	/* 清理 */
	table_close(pg_policy_rel, RowExclusiveLock);
}

/*
 * RemoveRoleFromObjectPolicy -
 *	 从策略的可应用角色列表中移除角色。
 *
 * 成功移除返回 true。返回 false 表示因移除后 polroles
 * 为空而未移除（不允许，虽然或许应该允许）。
 * 返回 false 时调用者应直接删除整个策略。
 *
 * roleid - 要移除的角色的 OID
 * classid - 应始终为 PolicyRelationId
 * policy_id - 策略的 OID。
*
 */
bool
RemoveRoleFromObjectPolicy(Oid roleid, Oid classid, Oid policy_id)
{
	Relation	pg_policy_rel;
	SysScanDesc sscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;
	Oid			relid;
	ArrayType  *policy_roles;
	Datum		roles_datum;
	Oid		   *roles;
	int			num_roles;
	Datum	   *role_oids;
	bool		attr_isnull;
	bool		keep_policy = true;
	int			i,
				j;

	Assert(classid == PolicyRelationId);

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/*
	 * 找到待更新的策略。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
							   NULL, 1, skey);

	tuple = systable_getnext(sscan);

	/* 找不到策略则报错。 */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for policy %u", policy_id);

	/* 识别策略所属关系 */
	relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;

	/* 获取当前角色集 */
	roles_datum = heap_getattr(tuple,
							   Anum_pg_policy_polroles,
							   RelationGetDescr(pg_policy_rel),
							   &attr_isnull);

	Assert(!attr_isnull);

	policy_roles = DatumGetArrayTypePCopy(roles_datum);
	roles = (Oid *) ARR_DATA_PTR(policy_roles);
	num_roles = ARR_DIMS(policy_roles)[0];

	/*
	 * 重建 polroles 数组，移除目标角色。通常只有一个，
	 * 但须处理重复引用（CREATE/ALTER POLICY 历史允许）。
	*
	 */
	role_oids = (Datum *) palloc(num_roles * sizeof(Datum));
	for (i = 0, j = 0; i < num_roles; i++)
	{
		if (roles[i] != roleid)
			role_oids[j++] = ObjectIdGetDatum(roles[i]);
	}
	num_roles = j;

	/* 有角色剩余则更新策略条目。 */
	if (num_roles > 0)
	{
		ArrayType  *role_ids;
		Datum		values[Natts_pg_policy];
		bool		isnull[Natts_pg_policy];
		bool		replaces[Natts_pg_policy];
		HeapTuple	new_tuple;
		HeapTuple	reltup;
		ObjectAddress target;
		ObjectAddress myself;

		/* 清零 */
		memset(values, 0, sizeof(values));
		memset(replaces, 0, sizeof(replaces));
		memset(isnull, 0, sizeof(isnull));

		/* 新元组的数组 */
		role_ids = construct_array_builtin(role_oids, num_roles, OIDOID);

		replaces[Anum_pg_policy_polroles - 1] = true;
		values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);

		new_tuple = heap_modify_tuple(tuple,
									  RelationGetDescr(pg_policy_rel),
									  values, isnull, replaces);
		CatalogTupleUpdate(pg_policy_rel, &new_tuple->t_self, new_tuple);

		/* 移除旧共享依赖（角色） */
		deleteSharedDependencyRecordsFor(PolicyRelationId, policy_id, 0);

		/* 记录新共享依赖（角色） */
		myself.classId = PolicyRelationId;
		myself.objectId = policy_id;
		myself.objectSubId = 0;

		target.classId = AuthIdRelationId;
		target.objectSubId = 0;
		for (i = 0; i < num_roles; i++)
		{
			target.objectId = DatumGetObjectId(role_oids[i]);
			/* public 角色无需依赖 */
			if (target.objectId != ACL_ID_PUBLIC)
				recordSharedDependencyOn(&myself, &target,
										 SHARED_DEPENDENCY_POLICY);
		}

		InvokeObjectPostAlterHook(PolicyRelationId, policy_id, 0);

		heap_freetuple(new_tuple);

		/* 使更新可见 */
		CommandCounterIncrement();

		/*
		 * 使策略所属关系的 relcache 条目失效，强制重做依赖计划。
		 * 因竞态导致关系刚被删除则无需操作。
		*
		 */
		reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (HeapTupleIsValid(reltup))
		{
			CacheInvalidateRelcacheByTuple(reltup);
			ReleaseSysCache(reltup);
		}
	}
	else
	{
		/* 无角色剩余，直接删除策略。 */
		keep_policy = false;
	}

	/* 清理。 */
	systable_endscan(sscan);

	table_close(pg_policy_rel, RowExclusiveLock);

	return keep_policy;
}

/*
 * CreatePolicy -
 *	 执行 CREATE POLICY 命令。
 *
 * stmt - 描述待创建策略的 CreatePolicyStmt。
 */
ObjectAddress
CreatePolicy(CreatePolicyStmt *stmt)
{
	Relation	pg_policy_rel;
	Oid			policy_id;
	Relation	target_table;
	Oid			table_id;
	char		polcmd;
	Datum	   *role_oids;
	int			nitems = 0;
	ArrayType  *role_ids;
	ParseState *qual_pstate;
	ParseState *with_check_pstate;
	ParseNamespaceItem *nsitem;
	Node	   *qual;
	Node	   *with_check_qual;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	Datum		values[Natts_pg_policy];
	bool		isnull[Natts_pg_policy];
	ObjectAddress target;
	ObjectAddress myself;
	int			i;

	/* 解析命令 */
	polcmd = parse_policy_command(stmt->cmd_name);

	/*
	 * 命令为 SELECT 或 DELETE 时 WITH CHECK 应为 NULL。
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("WITH CHECK cannot be applied to SELECT or DELETE")));

	/*
	 * 命令为 INSERT 时 WITH CHECK 应为唯一提供的表达式。
	*
	 */
	if (polcmd == ACL_INSERT_CHR && stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));

	/* 收集角色 ID */
	role_oids = policy_role_list_to_array(stmt->roles, &nitems);
	role_ids = construct_array_builtin(role_oids, nitems, OIDOID);

	/* 解析提供的子句 */
	qual_pstate = make_parsestate(NULL);
	with_check_pstate = make_parsestate(NULL);

	/* 清零 */
	memset(values, 0, sizeof(values));
	memset(isnull, 0, sizeof(isnull));

	/* 获取表 ID，同时处理权限检查。 */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	/* 打开 target_table 构建限定条件，无需额外锁。 */
	target_table = relation_open(table_id, NoLock);

	/* 添加常规安全限定条件 */
	nsitem = addRangeTableEntryForRelation(qual_pstate, target_table,
										   AccessShareLock,
										   NULL, false, false);
	addNSItemToQuery(qual_pstate, nsitem, false, true, true);

	/* 添加 WITH CHECK 限定条件 */
	nsitem = addRangeTableEntryForRelation(with_check_pstate, target_table,
										   AccessShareLock,
										   NULL, false, false);
	addNSItemToQuery(with_check_pstate, nsitem, false, true, true);

	qual = transformWhereClause(qual_pstate,
								stmt->qual,
								EXPR_KIND_POLICY,
								"POLICY");

	with_check_qual = transformWhereClause(with_check_pstate,
										   stmt->with_check,
										   EXPR_KIND_POLICY,
										   "POLICY");

	/* 修正排序规则信息 */
	assign_expr_collations(qual_pstate, qual);
	assign_expr_collations(with_check_pstate, with_check_qual);

	/* 打开 pg_policy 系统目录 */
	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* 设置键 - 策略的关系 ID。 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* 设置键 - 策略名称。 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* 表上策略名已存在则报错 */
	if (HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" already exists",
						stmt->policy_name, RelationGetRelationName(target_table))));

	policy_id = GetNewOidWithIndex(pg_policy_rel, PolicyOidIndexId,
								   Anum_pg_policy_oid);
	values[Anum_pg_policy_oid - 1] = ObjectIdGetDatum(policy_id);
	values[Anum_pg_policy_polrelid - 1] = ObjectIdGetDatum(table_id);
	values[Anum_pg_policy_polname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(stmt->policy_name));
	values[Anum_pg_policy_polcmd - 1] = CharGetDatum(polcmd);
	values[Anum_pg_policy_polpermissive - 1] = BoolGetDatum(stmt->permissive);
	values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);

	/* 有则添加限定条件。 */
	if (qual)
		values[Anum_pg_policy_polqual - 1] = CStringGetTextDatum(nodeToString(qual));
	else
		isnull[Anum_pg_policy_polqual - 1] = true;

	/* 有则添加 WITH CHECK 限定条件 */
	if (with_check_qual)
		values[Anum_pg_policy_polwithcheck - 1] = CStringGetTextDatum(nodeToString(with_check_qual));
	else
		isnull[Anum_pg_policy_polwithcheck - 1] = true;

	policy_tuple = heap_form_tuple(RelationGetDescr(pg_policy_rel), values,
								   isnull);

	CatalogTupleInsert(pg_policy_rel, policy_tuple);

	/* 记录依赖 */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_pstate->p_rtable,
						   DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual,
						   with_check_pstate->p_rtable, DEPENDENCY_NORMAL);

	/* 注册角色依赖 */
	target.classId = AuthIdRelationId;
	target.objectSubId = 0;
	for (i = 0; i < nitems; i++)
	{
		target.objectId = DatumGetObjectId(role_oids[i]);
		/* public 则无需依赖 */
		if (target.objectId != ACL_ID_PUBLIC)
			recordSharedDependencyOn(&myself, &target,
									 SHARED_DEPENDENCY_POLICY);
	}

	InvokeObjectPostCreateHook(PolicyRelationId, policy_id, 0);

	/* 使关系缓存失效 */
	CacheInvalidateRelcache(target_table);

	/* 清理。 */
	heap_freetuple(policy_tuple);
	free_parsestate(qual_pstate);
	free_parsestate(with_check_pstate);
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	table_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * AlterPolicy -
 *	 执行 ALTER POLICY 命令。
 *
 * stmt - 描述策略及修改方式的 AlterPolicyStmt。
 */
ObjectAddress
AlterPolicy(AlterPolicyStmt *stmt)
{
	Relation	pg_policy_rel;
	Oid			policy_id;
	Relation	target_table;
	Oid			table_id;
	Datum	   *role_oids = NULL;
	int			nitems = 0;
	ArrayType  *role_ids = NULL;
	List	   *qual_parse_rtable = NIL;
	List	   *with_check_parse_rtable = NIL;
	Node	   *qual = NULL;
	Node	   *with_check_qual = NULL;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	HeapTuple	new_tuple;
	Datum		values[Natts_pg_policy];
	bool		isnull[Natts_pg_policy];
	bool		replaces[Natts_pg_policy];
	ObjectAddress target;
	ObjectAddress myself;
	Datum		polcmd_datum;
	char		polcmd;
	bool		polcmd_isnull;
	int			i;

	/* 解析 role_ids */
	if (stmt->roles != NULL)
	{
		role_oids = policy_role_list_to_array(stmt->roles, &nitems);
		role_ids = construct_array_builtin(role_oids, nitems, OIDOID);
	}

	/* 获取表 ID，同时处理权限检查。 */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	target_table = relation_open(table_id, NoLock);

	/* 解析 USING 策略子句 */
	if (stmt->qual)
	{
		ParseNamespaceItem *nsitem;
		ParseState *qual_pstate = make_parsestate(NULL);

		nsitem = addRangeTableEntryForRelation(qual_pstate, target_table,
											   AccessShareLock,
											   NULL, false, false);

		addNSItemToQuery(qual_pstate, nsitem, false, true, true);

		qual = transformWhereClause(qual_pstate, stmt->qual,
									EXPR_KIND_POLICY,
									"POLICY");

		/* 修正排序规则信息 */
		assign_expr_collations(qual_pstate, qual);

		qual_parse_rtable = qual_pstate->p_rtable;
		free_parsestate(qual_pstate);
	}

	/* 解析 WITH CHECK 策略子句 */
	if (stmt->with_check)
	{
		ParseNamespaceItem *nsitem;
		ParseState *with_check_pstate = make_parsestate(NULL);

		nsitem = addRangeTableEntryForRelation(with_check_pstate, target_table,
											   AccessShareLock,
											   NULL, false, false);

		addNSItemToQuery(with_check_pstate, nsitem, false, true, true);

		with_check_qual = transformWhereClause(with_check_pstate,
											   stmt->with_check,
											   EXPR_KIND_POLICY,
											   "POLICY");

		/* 修正排序规则信息 */
		assign_expr_collations(with_check_pstate, with_check_qual);

		with_check_parse_rtable = with_check_pstate->p_rtable;
		free_parsestate(with_check_pstate);
	}

	/* 清零 */
	memset(values, 0, sizeof(values));
	memset(replaces, 0, sizeof(replaces));
	memset(isnull, 0, sizeof(isnull));

	/* 找到待更新策略。 */
	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* 设置键 - 策略的关系 ID。 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* 设置键 - 策略名称。 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* 检查策略是否找到，未找到则报错。 */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" does not exist",
						stmt->policy_name,
						RelationGetRelationName(target_table))));

	/* 获取策略命令 */
	polcmd_datum = heap_getattr(policy_tuple, Anum_pg_policy_polcmd,
								RelationGetDescr(pg_policy_rel),
								&polcmd_isnull);
	Assert(!polcmd_isnull);
	polcmd = DatumGetChar(polcmd_datum);

	/*
	 * 命令为 SELECT 或 DELETE 时 WITH CHECK 应为 NULL。
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only USING expression allowed for SELECT, DELETE")));

	/*
	 * 命令为 INSERT 时 WITH CHECK 应为唯一提供的表达式。
	*
	 */
	if ((polcmd == ACL_INSERT_CHR)
		&& stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));

	policy_id = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	if (role_ids != NULL)
	{
		replaces[Anum_pg_policy_polroles - 1] = true;
		values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);
	}
	else
	{
		Oid		   *roles;
		Datum		roles_datum;
		bool		attr_isnull;
		ArrayType  *policy_roles;

		/*
		 * 我们需要从目录中的内容提取此策略适用的角色    
		 * 集合，以便能为该策略正确地重建依赖关系。      
		 * 。            
		 */

		roles_datum = heap_getattr(policy_tuple, Anum_pg_policy_polroles,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		Assert(!attr_isnull);

		policy_roles = DatumGetArrayTypePCopy(roles_datum);

		roles = (Oid *) ARR_DATA_PTR(policy_roles);

		nitems = ARR_DIMS(policy_roles)[0];

		role_oids = (Datum *) palloc(nitems * sizeof(Datum));

		for (i = 0; i < nitems; i++)
			role_oids[i] = ObjectIdGetDatum(roles[i]);
	}

	if (qual != NULL)
	{
		replaces[Anum_pg_policy_polqual - 1] = true;
		values[Anum_pg_policy_polqual - 1]
			= CStringGetTextDatum(nodeToString(qual));
	}
	else
	{
		Datum		value_datum;
		bool		attr_isnull;

		/*
		 * 从系统目录提取 USING 表达式并构建范围表
		 * 以正确重建策略依赖。
		*
		 */

		/* 检查策略是否有 USING 表达式 */
		value_datum = heap_getattr(policy_tuple, Anum_pg_policy_polqual,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		if (!attr_isnull)
		{
			char	   *qual_value;
			ParseState *qual_pstate;

			/* parsestate 仅用于构建范围表 */
			qual_pstate = make_parsestate(NULL);

			qual_value = TextDatumGetCString(value_datum);
			qual = stringToNode(qual_value);

			/* 添加关系到 parsestate 范围表用于依赖 */
			(void) addRangeTableEntryForRelation(qual_pstate, target_table,
												 AccessShareLock,
												 NULL, false, false);

			qual_parse_rtable = qual_pstate->p_rtable;
			free_parsestate(qual_pstate);
		}
	}

	if (with_check_qual != NULL)
	{
		replaces[Anum_pg_policy_polwithcheck - 1] = true;
		values[Anum_pg_policy_polwithcheck - 1]
			= CStringGetTextDatum(nodeToString(with_check_qual));
	}
	else
	{
		Datum		value_datum;
		bool		attr_isnull;

		/*
		 * 我们需要从目录中的内容提取 WITH CHECK 表达式，并 
		 * 为该策略构建范围表，以便能正确地重建其依赖   
		 * 关系。                                 
		 */

		/* 检查策略是否有 WITH CHECK 表达式 */
		value_datum = heap_getattr(policy_tuple, Anum_pg_policy_polwithcheck,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		if (!attr_isnull)
		{
			char	   *with_check_value;
			ParseState *with_check_pstate;

			/* parsestate 仅用于构建范围表 */
			with_check_pstate = make_parsestate(NULL);

			with_check_value = TextDatumGetCString(value_datum);
			with_check_qual = stringToNode(with_check_value);

			/* 添加关系到 parsestate 范围表用于依赖 */
			(void) addRangeTableEntryForRelation(with_check_pstate,
												 target_table,
												 AccessShareLock,
												 NULL, false, false);

			with_check_parse_rtable = with_check_pstate->p_rtable;
			free_parsestate(with_check_pstate);
		}
	}

	new_tuple = heap_modify_tuple(policy_tuple,
								  RelationGetDescr(pg_policy_rel),
								  values, isnull, replaces);
	CatalogTupleUpdate(pg_policy_rel, &new_tuple->t_self, new_tuple);

	/* 更新依赖。 */
	deleteDependencyRecordsFor(PolicyRelationId, policy_id, false);

	/* 记录依赖 */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_parse_rtable, DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual, with_check_parse_rtable,
						   DEPENDENCY_NORMAL);

	/* 注册角色依赖 */
	deleteSharedDependencyRecordsFor(PolicyRelationId, policy_id, 0);
	target.classId = AuthIdRelationId;
	target.objectSubId = 0;
	for (i = 0; i < nitems; i++)
	{
		target.objectId = DatumGetObjectId(role_oids[i]);
		/* public 则无需依赖 */
		if (target.objectId != ACL_ID_PUBLIC)
			recordSharedDependencyOn(&myself, &target,
									 SHARED_DEPENDENCY_POLICY);
	}

	InvokeObjectPostAlterHook(PolicyRelationId, policy_id, 0);

	heap_freetuple(new_tuple);

	/* 使关系缓存失效 */
	CacheInvalidateRelcache(target_table);

	/* 清理。 */
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	table_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * rename_policy -
 *	 重命名关系上的策略
 */
ObjectAddress
rename_policy(RenameStmt *stmt)
{
	Relation	pg_policy_rel;
	Relation	target_table;
	Oid			table_id;
	Oid			opoloid;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	ObjectAddress address;

	/* 获取表 ID，同时处理权限检查。 */
	table_id = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										(void *) stmt);

	target_table = relation_open(table_id, NoLock);

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* 第一遍 -- 检查冲突 */

	/* 添加键 - 策略的关系 ID。 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* 添加键 - 策略名称。 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->newname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	if (HeapTupleIsValid(systable_getnext(sscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" already exists",
						stmt->newname, RelationGetRelationName(target_table))));

	systable_endscan(sscan);

	/* 第二遍 -- 查找现有策略并更新 */
	/* 添加键 - 策略的关系 ID。 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* 添加键 - 策略名称。 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->subname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* 未找到策略则报错 */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(target_table))));

	opoloid = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	policy_tuple = heap_copytuple(policy_tuple);

	namestrcpy(&((Form_pg_policy) GETSTRUCT(policy_tuple))->polname,
			   stmt->newname);

	CatalogTupleUpdate(pg_policy_rel, &policy_tuple->t_self, policy_tuple);

	InvokeObjectPostAlterHook(PolicyRelationId, opoloid, 0);

	ObjectAddressSet(address, PolicyRelationId, opoloid);

	/*
	 * 使关系 relcache 失效，向其他后端（及此后端！）发送 SI
	 * 消息重建 relcache 条目。（理想情况下应自动发生……）
	*
	 */
	CacheInvalidateRelcache(target_table);

	/* 清理。 */
	systable_endscan(sscan);
	table_close(pg_policy_rel, RowExclusiveLock);
	relation_close(target_table, NoLock);

	return address;
}

/*
 * get_relation_policy_oid - 按名称查找策略 OID
 *
 * missing_ok 为 false 时未找到则报错，为 true 只返回 InvalidOid。
*
 */
Oid
get_relation_policy_oid(Oid relid, const char *policy_name, bool missing_ok)
{
	Relation	pg_policy_rel;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	Oid			policy_oid;

	pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);

	/* 添加键 - 策略的关系 ID。 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	/* 添加键 - 策略名称。 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	if (!HeapTupleIsValid(policy_tuple))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("policy \"%s\" for table \"%s\" does not exist",
							policy_name, get_rel_name(relid))));

		policy_oid = InvalidOid;
	}
	else
		policy_oid = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	/* 清理。 */
	systable_endscan(sscan);
	table_close(pg_policy_rel, AccessShareLock);

	return policy_oid;
}

/*
 * relation_has_policies - 确定关系是否有策略
 */
bool
relation_has_policies(Relation rel)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	bool		ret = false;

	catalog = table_open(PolicyRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	sscan = systable_beginscan(catalog, PolicyPolrelidPolnameIndexId, true,
							   NULL, 1, &skey);
	policy_tuple = systable_getnext(sscan);
	if (HeapTupleIsValid(policy_tuple))
		ret = true;

	systable_endscan(sscan);
	table_close(catalog, AccessShareLock);

	return ret;
}
