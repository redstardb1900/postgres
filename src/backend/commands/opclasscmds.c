/*-------------------------------------------------------------------------
 *
 * opclasscmds.c
 *
 *	  Routines for opclass (and opfamily) manipulation commands
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/opclasscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void AlterOpFamilyAdd(AlterOpFamilyStmt *stmt,
							 Oid amoid, Oid opfamilyoid,
							 int maxOpNumber, int maxProcNumber,
							 int optsProcNumber, List *items);
static void AlterOpFamilyDrop(AlterOpFamilyStmt *stmt,
							  Oid amoid, Oid opfamilyoid,
							  int maxOpNumber, int maxProcNumber,
							  List *items);
static void processTypesSpec(List *args, Oid *lefttype, Oid *righttype);
static void assignOperTypes(OpFamilyMember *member, Oid amoid, Oid typeoid);
static void assignProcTypes(OpFamilyMember *member, Oid amoid, Oid typeoid,
							int opclassOptsProcNum);
static void addFamilyMember(List **list, OpFamilyMember *member);
static void storeOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						   List *operators, bool isAdd);
static void storeProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
							List *procedures, bool isAdd);
static bool typeDepNeeded(Oid typid, OpFamilyMember *member);
static void dropOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						  List *operators);
static void dropProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
						   List *procedures);

/*
 * OpFamilyCacheLookup
 *   通过名称查找现有的操作符族。
 *
 * 返回系统缓存元组引用，如果未找到则返回 NULL。
 */
static HeapTuple
OpFamilyCacheLookup(Oid amID, List *opfamilyname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opfname;
	HeapTuple	htup;

	/* 解析名称列表        */
	DeconstructQualifiedName(opfamilyname, &schemaname, &opfname);

	if (schemaname)
	{
		/* 仅在特定模式中查找  */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(OPFAMILYAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opfname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 非限定操作符族名称，因此搜索搜索路径 */
		Oid			opfID = OpfamilynameGetOpfid(amID, opfname);

		if (!OidIsValid(opfID))
			htup = NULL;
		else
			htup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator family \"%s\" does not exist for access method \"%s\"",
						NameListToString(opfamilyname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opfamily_oid
 *   通过可能限定的名称查找操作符族 OID。
 *
 * 如果未找到，当 missing_ok 时返回 InvalidOid，否则抛出错误。
 */
Oid
get_opfamily_oid(Oid amID, List *opfamilyname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opfamily opfamform;
	Oid			opfID;

	htup = OpFamilyCacheLookup(amID, opfamilyname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opfamform = (Form_pg_opfamily) GETSTRUCT(htup);
	opfID = opfamform->oid;
	ReleaseSysCache(htup);

	return opfID;
}

/*
 * OpClassCacheLookup
 *   通过名称查找现有的操作符类。
 *
 * 返回系统缓存元组引用，如果未找到则返回 NULL。
 */
static HeapTuple
OpClassCacheLookup(Oid amID, List *opclassname, bool missing_ok)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	htup;

	/* 解析名称列表        */
	DeconstructQualifiedName(opclassname, &schemaname, &opcname);

	if (schemaname)
	{
		/* 仅在特定模式中查找  */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			htup = NULL;
		else
			htup = SearchSysCache3(CLAAMNAMENSP,
								   ObjectIdGetDatum(amID),
								   PointerGetDatum(opcname),
								   ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 非限定操作符类名称，因此搜索搜索路径 */
		Oid			opcID = OpclassnameGetOpcid(amID, opcname);

		if (!OidIsValid(opcID))
			htup = NULL;
		else
			htup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcID));
	}

	if (!HeapTupleIsValid(htup) && !missing_ok)
	{
		HeapTuple	amtup;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(amID));
		if (!HeapTupleIsValid(amtup))
			elog(ERROR, "cache lookup failed for access method %u", amID);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclassname),
						NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname))));
	}

	return htup;
}

/*
 * get_opclass_oid
 *   通过可能限定的名称查找操作符类 OID。
 *
 * 如果未找到，当 missing_ok 时返回 InvalidOid，否则抛出错误。
 */
Oid
get_opclass_oid(Oid amID, List *opclassname, bool missing_ok)
{
	HeapTuple	htup;
	Form_pg_opclass opcform;
	Oid			opcID;

	htup = OpClassCacheLookup(amID, opclassname, missing_ok);
	if (!HeapTupleIsValid(htup))
		return InvalidOid;
	opcform = (Form_pg_opclass) GETSTRUCT(htup);
	opcID = opcform->oid;
	ReleaseSysCache(htup);

	return opcID;
}

/*
 * CreateOpFamily
 *   为新操作符族创建目录条目的内部例程。
 *
 * 调用者必须已经完成权限检查等操作。
                  */
static ObjectAddress
CreateOpFamily(CreateOpFamilyStmt *stmt, const char *opfname,
			   Oid namespaceoid, Oid amoid)
{
	Oid			opfamilyoid;
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_opfamily];
	bool		nulls[Natts_pg_opfamily];
	NameData	opfName;
	ObjectAddress myself,
				referenced;

	rel = table_open(OperatorFamilyRelationId, RowExclusiveLock);

	/*
		 * 确保没有同名的现有操作符族（这仅是为了提供比
		 * "重复键"更友好的错误消息）。
		             */
	if (SearchSysCacheExists3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  CStringGetDatum(opfname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator family \"%s\" for access method \"%s\" already exists",
						opfname, stmt->amname)));

	/*
		 * 好的，让我们创建 pg_opfamily 条目。
		 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	opfamilyoid = GetNewOidWithIndex(rel, OpfamilyOidIndexId,
									 Anum_pg_opfamily_oid);
	values[Anum_pg_opfamily_oid - 1] = ObjectIdGetDatum(opfamilyoid);
	values[Anum_pg_opfamily_opfmethod - 1] = ObjectIdGetDatum(amoid);
	namestrcpy(&opfName, opfname);
	values[Anum_pg_opfamily_opfname - 1] = NameGetDatum(&opfName);
	values[Anum_pg_opfamily_opfnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_opfamily_opfowner - 1] = ObjectIdGetDatum(GetUserId());

	tup = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/*
		 * 为操作符族本身创建依赖关系。
		 */
	myself.classId = OperatorFamilyRelationId;
	myself.objectId = opfamilyoid;
	myself.objectSubId = 0;

	/* 依赖于访问方法       */
	referenced.classId = AccessMethodRelationId;
	referenced.objectId = amoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 依赖于命名空间   */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = namespaceoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 依赖于所有者  */
	recordDependencyOnOwner(OperatorFamilyRelationId, opfamilyoid, GetUserId());

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 将新的操作符族报告给可能感兴趣的事件触发器      */
	EventTriggerCollectSimpleCommand(myself, InvalidObjectAddress,
									 (Node *) stmt);

	/* 新操作符族的创建后钩子          */
	InvokeObjectPostCreateHook(OperatorFamilyRelationId, opfamilyoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * DefineOpClass
 *   定义一个新的索引操作符类。
 */
ObjectAddress
DefineOpClass(CreateOpClassStmt *stmt)
{
	char	   *opcname;		/* 我们正在创建的操作符类的名称 */
	Oid			amoid,			/* 我们的访问方法的 OID */
				typeoid,		/* 可索引数据类型的 OID */
				storageoid,		/* 存储数据类型的 OID（如果有） */
				namespaceoid,	/* 创建操作符类的命名空间 */
				opfamilyoid,	/* 包含的操作符族的 OID */
				opclassoid;		/* 我们创建的操作符类的 OID */
	int			maxOpNumber,	/* amstrategies 值   */
				optsProcNumber, /* amoptsprocnum 值   */
				maxProcNumber;	/* amsupport 值   */
	bool		amstorage;		/* amstorage 标志 */
	List	   *operators;		/* 操作符的 OpFamilyMember 列表 */
	List	   *procedures;		/* 支持过程的 OpFamilyMember 列表 */
	ListCell   *l;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_am	amform;
	IndexAmRoutine *amroutine;
	Datum		values[Natts_pg_opclass];
	bool		nulls[Natts_pg_opclass];
	AclResult	aclresult;
	NameData	opcName;
	ObjectAddress myself,
				referenced;

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(stmt->opclassname,
													 &opcname);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/* 获取有关访问方法的必要信息 */
	tup = SearchSysCache1(AMNAME, CStringGetDatum(stmt->amname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	amform = (Form_pg_am) GETSTRUCT(tup);
	amoid = amform->oid;
	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	ReleaseSysCache(tup);

	maxOpNumber = amroutine->amstrategies;
	/* 如果 amstrategies 为零，仅强制操作符号适配 int16     */
	if (maxOpNumber <= 0)
		maxOpNumber = SHRT_MAX;
	maxProcNumber = amroutine->amsupport;
	optsProcNumber = amroutine->amoptsprocnum;
	amstorage = amroutine->amstorage;

	/* XXX 我们是否应该对访问方法进行任何权限检查？ */

	/*
	 * CREATE OPERATOR CLASS 的适当权限问题很有趣。创建操作符类
	 * 等同于授予对所涉及函数的公共执行权限，因为索引机制
	 * 通常在使用函数之前不检查访问权限。
	 * 因此，最低期望是调用者具有带授权选项的执行权限。
	 * 由于我们没有办法在授权选项被撤销时使操作符类消失，
	 * 我们选择要求拥有函数的所有权。此外，关于数据类型
	 * 需要什么权限也不完全清楚，但所有权似乎是一个安全的选择。
	 *
	 * 目前，我们要求超级用户权限来创建操作符类。这似乎是必要的，
	 * 因为我们无法验证所提供的操作符和函数集合是否与访问方法
	 * 的期望一致。如果能在不解决停机问题的情况下提供这样的检查，
	 * 那就太好了 :(
	 *
	 * XXX 如果你移除此测试，请重新启用下面的 NOT_USED 代码段。
 *
 *
 *
 *
	                                                                                                                                                                                */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an operator class")));

	/* 查找数据类型   */
	typeoid = typenameTypeId(NULL, stmt->datatype);

#ifdef NOT_USED
	/* XXX 鉴于上面的超级用户检查，这是不必要的 */
	/* 检查我们是否拥有该数据类型 */
	if (!object_ownercheck(TypeRelationId, typeoid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeoid);
#endif

	/*
	 * 查找包含的操作符族，或者如果省略了 FAMILY 选项
	 * 且尚未存在匹配，则创建一个。
	        */
	if (stmt->opfamilyname)
	{
		opfamilyoid = get_opfamily_oid(amoid, stmt->opfamilyname, false);
	}
	else
	{
		/* 查找同名的现有族和命名空间           */
		tup = SearchSysCache3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  PointerGetDatum(opcname),
							  ObjectIdGetDatum(namespaceoid));
		if (HeapTupleIsValid(tup))
		{
			opfamilyoid = ((Form_pg_opfamily) GETSTRUCT(tup))->oid;

			/*
			 * XXX 鉴于上面的超级用户检查，
			 * 这里不需要所有权检查
			          */
			ReleaseSysCache(tup);
		}
		else
		{
			CreateOpFamilyStmt *opfstmt;
			ObjectAddress tmpAddr;

			opfstmt = makeNode(CreateOpFamilyStmt);
			opfstmt->opfamilyname = stmt->opclassname;
			opfstmt->amname = stmt->amname;

			/*
			 * 创建它...再次不需要更多权限...
			           */
			tmpAddr = CreateOpFamily(opfstmt, opcname, namespaceoid, amoid);
			opfamilyoid = tmpAddr.objectId;
		}
	}

	operators = NIL;
	procedures = NIL;

	/* 存储数据类型是可选的 */
	storageoid = InvalidOid;

	/*
	 * 扫描 "items" 列表以获取额外信息。
	    */
	foreach(l, stmt->items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			operOid;
		Oid			funcOid;
		Oid			sortfamilyOid;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				if (item->name->objargs != NIL)
					operOid = LookupOperWithArgs(item->name, false);
				else
				{
					/* 默认使用输入数据类型上的二元操作符 */
					operOid = LookupOperName(NULL, item->name->objname,
											 typeoid, typeoid,
											 false, -1);
				}

				if (item->order_family)
					sortfamilyOid = get_opfamily_oid(BTREE_AM_OID,
													 item->order_family,
													 false);
				else
					sortfamilyOid = InvalidOid;

#ifdef NOT_USED
				/* XXX 鉴于上面的超级用户检查，这是不必要的 */
				/* 调用者必须拥有操作符及其底层函数     */
				if (!object_ownercheck(OperatorRelationId, operOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
								   get_opname(operOid));
				funcOid = get_opcode(operOid);
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->object = operOid;
				member->number = item->number;
				member->sortfamily = sortfamilyOid;
				assignOperTypes(member, amoid, typeoid);
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				funcOid = LookupFuncWithArgs(OBJECT_FUNCTION, item->name, false);
#ifdef NOT_USED
				/* XXX 鉴于上面的超级用户检查，这是不必要的 */
				/* 调用者必须拥有函数 */
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif
				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->object = funcOid;
				member->number = item->number;

				/* 允许覆盖函数的实际参数类型             */
				if (item->class_args)
					processTypesSpec(item->class_args,
									 &member->lefttype, &member->righttype);

				assignProcTypes(member, amoid, typeoid, optsProcNumber);
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				if (OidIsValid(storageoid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("storage type specified more than once")));
				storageoid = typenameTypeId(NULL, item->storedtype);

#ifdef NOT_USED
				/* XXX 鉴于上面的超级用户检查，这是不必要的 */
				/* 检查我们是否拥有该数据类型 */
				if (!object_ownercheck(TypeRelationId, storageoid, GetUserId()))
					aclcheck_error_type(ACLCHECK_NOT_OWNER, storageoid);
#endif
				break;
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * 如果指定了存储类型，确保它是合法的。
	 */
	if (OidIsValid(storageoid))
	{
		/* 如果与列数据类型相同，则丢弃该规范 */
		if (storageoid == typeoid)
			storageoid = InvalidOid;
		else if (!amstorage)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("storage type cannot be different from data type for access method \"%s\"",
							stmt->amname)));
	}

	rel = table_open(OperatorClassRelationId, RowExclusiveLock);

	/*
	 * 确保没有同名的现有操作符类（这仅是为了提供比
	 * "重复键"更友好的错误消息）。
	               */
	if (SearchSysCacheExists3(CLAAMNAMENSP,
							  ObjectIdGetDatum(amoid),
							  CStringGetDatum(opcname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists",
						opcname, stmt->amname)));

	/*
	 * 如果我们正在创建一个默认操作符类，请检查是否已经有一个。
	 * （注意我们不将此测试限制为可见的操作符类；这确保 typcache.c
	 * 可以为其查询找到唯一解。）
	 */
	if (stmt->isDefault)
	{
		ScanKeyData skey[1];
		SysScanDesc scan;

		ScanKeyInit(&skey[0],
					Anum_pg_opclass_opcmethod,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(amoid));

		scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
								  NULL, 1, skey);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

			if (opclass->opcintype == typeoid && opclass->opcdefault)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("could not make operator class \"%s\" be default for type %s",
								opcname,
								TypeNameToString(stmt->datatype)),
						 errdetail("Operator class \"%s\" already is the default.",
								   NameStr(opclass->opcname))));
		}

		systable_endscan(scan);
	}

	/*
	 * 好的，让我们创建 pg_opclass 条目。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	opclassoid = GetNewOidWithIndex(rel, OpclassOidIndexId,
									Anum_pg_opclass_oid);
	values[Anum_pg_opclass_oid - 1] = ObjectIdGetDatum(opclassoid);
	values[Anum_pg_opclass_opcmethod - 1] = ObjectIdGetDatum(amoid);
	namestrcpy(&opcName, opcname);
	values[Anum_pg_opclass_opcname - 1] = NameGetDatum(&opcName);
	values[Anum_pg_opclass_opcnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_opclass_opcowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_opclass_opcfamily - 1] = ObjectIdGetDatum(opfamilyoid);
	values[Anum_pg_opclass_opcintype - 1] = ObjectIdGetDatum(typeoid);
	values[Anum_pg_opclass_opcdefault - 1] = BoolGetDatum(stmt->isDefault);
	values[Anum_pg_opclass_opckeytype - 1] = ObjectIdGetDatum(storageoid);

	tup = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/*
	 * 现在我们有了操作符类 OID，为 pg_amop 和 pg_amproc 条目设置
	 * 默认依赖信息。历史上，CREATE OPERATOR CLASS 已创建了对
	 * 操作符类的硬依赖，所以我们就使用这个。
	      */
	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);

		op->ref_is_hard = true;
		op->ref_is_family = false;
		op->refobjid = opclassoid;
	}
	foreach(l, procedures)
	{
		OpFamilyMember *proc = (OpFamilyMember *) lfirst(l);

		proc->ref_is_hard = true;
		proc->ref_is_family = false;
		proc->refobjid = opclassoid;
	}

	/*
	 * 让索引访问方法对依赖选择进行编辑。如果它喜欢，也可以
	 * 对操作符和函数进行进一步验证。
	               */
	if (amroutine->amadjustmembers)
		amroutine->amadjustmembers(opfamilyoid,
								   opclassoid,
								   operators,
								   procedures);

	/*
	 * 现在添加元组到 pg_amop 和 pg_amproc，绑定操作符和函数。
	 * 同时插入对它们的依赖关系。
	      */
	storeOperators(stmt->opfamilyname, amoid, opfamilyoid,
				   operators, false);
	storeProcedures(stmt->opfamilyname, amoid, opfamilyoid,
					procedures, false);

	/* 让事件触发器知道发生了什么 */
	EventTriggerCollectCreateOpClass(stmt, opclassoid, operators, procedures);

	/*
	 * 为操作符类本身创建依赖关系。注意：我们不需要到访问方法的
	 * 依赖链接，因为该链接通过操作符族存在。
	 */
	myself.classId = OperatorClassRelationId;
	myself.objectId = opclassoid;
	myself.objectSubId = 0;

	/* 依赖于命名空间   */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = namespaceoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 依赖于操作符族  */
	referenced.classId = OperatorFamilyRelationId;
	referenced.objectId = opfamilyoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* 依赖于被索引的数据类型 */
	referenced.classId = TypeRelationId;
	referenced.objectId = typeoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 依赖于存储数据类型    */
	if (OidIsValid(storageoid))
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = storageoid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* 依赖于所有者  */
	recordDependencyOnOwner(OperatorClassRelationId, opclassoid, GetUserId());

	/* 依赖于扩展         */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新操作符类的创建后钩子         */
	InvokeObjectPostCreateHook(OperatorClassRelationId, opclassoid, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * DefineOpFamily
 *   定义一个新的索引操作符族。
 */
ObjectAddress
DefineOpFamily(CreateOpFamilyStmt *stmt)
{
	char	   *opfname;		/* 我们正在创建的操作符族的名称 */
	Oid			amoid,			/* 我们的访问方法的 OID */
				namespaceoid;	/* 创建操作符族的命名空间 */
	AclResult	aclresult;

	/* 将名称列表转换为名称和命名空间 */
	namespaceoid = QualifiedNameGetCreationNamespace(stmt->opfamilyname,
													 &opfname);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	/* 获取访问方法 OID，如果不存在则抛出错误。    */
	amoid = get_index_am_oid(stmt->amname, false);

	/* XXX 我们是否应该对访问方法进行任何权限检查？ */

	/*
	 * 目前，我们要求超级用户权限来创建操作符族。请参阅
	 * DefineOpClass 中的注释。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create an operator family")));

	/* 插入 pg_opfamily 目录条目  */
	return CreateOpFamily(stmt, opfname, namespaceoid, amoid);
}


/*
 * AlterOpFamily
 *   在现有操作符族中添加或删除操作符/过程。
 *
 * 注意：这里只实现 ALTER OPERATOR FAMILY ... ADD/DROP。其他的
 * ALTER OPERATOR FAMILY 命令存在，但通过不同的代码路径处理。
 *
                       */
Oid
AlterOpFamily(AlterOpFamilyStmt *stmt)
{
	Oid			amoid,			/* 我们的访问方法的 OID */
				opfamilyoid;	/* 操作符族的 OID */
	int			maxOpNumber,	/* amstrategies 值   */
				optsProcNumber, /* amoptsprocnum 值   */
				maxProcNumber;	/* amsupport 值   */
	HeapTuple	tup;
	Form_pg_am	amform;
	IndexAmRoutine *amroutine;

	/* 获取有关访问方法的必要信息 */
	tup = SearchSysCache1(AMNAME, CStringGetDatum(stmt->amname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						stmt->amname)));

	amform = (Form_pg_am) GETSTRUCT(tup);
	amoid = amform->oid;
	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	ReleaseSysCache(tup);

	maxOpNumber = amroutine->amstrategies;
	/* 如果 amstrategies 为零，仅强制操作符号适配 int16     */
	if (maxOpNumber <= 0)
		maxOpNumber = SHRT_MAX;
	maxProcNumber = amroutine->amsupport;
	optsProcNumber = amroutine->amoptsprocnum;

	/* XXX 我们是否应该对访问方法进行任何权限检查？ */

	/* 查找操作符族   */
	opfamilyoid = get_opfamily_oid(amoid, stmt->opfamilyname, false);

	/*
	 * 目前，我们要求超级用户权限来修改操作符族。
	 *
	 * XXX 如果你移除此测试，请重新启用下面的 NOT_USED 代码段。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter an operator family")));

	/*
	 * ADD 和 DROP 情况从此处开始需要不同的代码。
	 */
	if (stmt->isDrop)
		AlterOpFamilyDrop(stmt, amoid, opfamilyoid,
						  maxOpNumber, maxProcNumber, stmt->items);
	else
		AlterOpFamilyAdd(stmt, amoid, opfamilyoid,
						 maxOpNumber, maxProcNumber, optsProcNumber,
						 stmt->items);

	return opfamilyoid;
}

/*
 * ALTER OP FAMILY 的 ADD 部分
 */
static void
AlterOpFamilyAdd(AlterOpFamilyStmt *stmt, Oid amoid, Oid opfamilyoid,
				 int maxOpNumber, int maxProcNumber, int optsProcNumber,
				 List *items)
{
	IndexAmRoutine *amroutine = GetIndexAmRoutineByAmId(amoid, false);
	List	   *operators;		/* 操作符的 OpFamilyMember 列表 */
	List	   *procedures;		/* 支持过程的 OpFamilyMember 列表 */
	ListCell   *l;

	operators = NIL;
	procedures = NIL;

	/*
	 * 扫描 "items" 列表以获取额外信息。
	    */
	foreach(l, items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			operOid;
		Oid			funcOid;
		Oid			sortfamilyOid;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				if (item->name->objargs != NIL)
					operOid = LookupOperWithArgs(item->name, false);
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("operator argument types must be specified in ALTER OPERATOR FAMILY")));
					operOid = InvalidOid;	/* 让编译器安静  */
				}

				if (item->order_family)
					sortfamilyOid = get_opfamily_oid(BTREE_AM_OID,
													 item->order_family,
													 false);
				else
					sortfamilyOid = InvalidOid;

#ifdef NOT_USED
				/* XXX 鉴于上面的超级用户检查，这是不必要的 */
				/* 调用者必须拥有操作符及其底层函数     */
				if (!object_ownercheck(OperatorRelationId, operOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
								   get_opname(operOid));
				funcOid = get_opcode(operOid);
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->object = operOid;
				member->number = item->number;
				member->sortfamily = sortfamilyOid;
				/* 我们可以立即设置依赖字段        */
				/* 历史上，ALTER ADD 创建的是软依赖           */
				member->ref_is_hard = false;
				member->ref_is_family = true;
				member->refobjid = opfamilyoid;
				assignOperTypes(member, amoid, InvalidOid);
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				funcOid = LookupFuncWithArgs(OBJECT_FUNCTION, item->name, false);
#ifdef NOT_USED
				/* XXX 鉴于上面的超级用户检查，这是不必要的 */
				/* 调用者必须拥有函数 */
				if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
								   get_func_name(funcOid));
#endif

				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->object = funcOid;
				member->number = item->number;
				/* 我们可以立即设置依赖字段        */
				/* 历史上，ALTER ADD 创建的是软依赖           */
				member->ref_is_hard = false;
				member->ref_is_family = true;
				member->refobjid = opfamilyoid;

				/* 允许覆盖函数的实际参数类型             */
				if (item->class_args)
					processTypesSpec(item->class_args,
									 &member->lefttype, &member->righttype);

				assignProcTypes(member, amoid, InvalidOid, optsProcNumber);
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("STORAGE cannot be specified in ALTER OPERATOR FAMILY")));
				break;
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * 让索引访问方法对依赖选择进行编辑。如果它喜欢，也可以
	 * 对操作符和函数进行进一步验证。
	               */
	if (amroutine->amadjustmembers)
		amroutine->amadjustmembers(opfamilyoid,
								   InvalidOid,	/*无特定操作符类*/
								   operators,
								   procedures);

	/*
	 * 添加元组到 pg_amop 和 pg_amproc，绑定操作符和函数。
	 * 同时插入对它们的依赖关系。
	        */
	storeOperators(stmt->opfamilyname, amoid, opfamilyoid,
				   operators, true);
	storeProcedures(stmt->opfamilyname, amoid, opfamilyoid,
					procedures, true);

	/* 使信息对事件触发器可用            */
	EventTriggerCollectAlterOpFam(stmt, opfamilyoid,
								  operators, procedures);
}

/*
 * ALTER OP FAMILY 的 DROP 部分
 */
static void
AlterOpFamilyDrop(AlterOpFamilyStmt *stmt, Oid amoid, Oid opfamilyoid,
				  int maxOpNumber, int maxProcNumber, List *items)
{
	List	   *operators;		/* 操作符的 OpFamilyMember 列表 */
	List	   *procedures;		/* 支持过程的 OpFamilyMember 列表 */
	ListCell   *l;

	operators = NIL;
	procedures = NIL;

	/*
	 * 扫描 "items" 列表以获取额外信息。
	    */
	foreach(l, items)
	{
		CreateOpClassItem *item = lfirst_node(CreateOpClassItem, l);
		Oid			lefttype,
					righttype;
		OpFamilyMember *member;

		switch (item->itemtype)
		{
			case OPCLASS_ITEM_OPERATOR:
				if (item->number <= 0 || item->number > maxOpNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid operator number %d,"
									" must be between 1 and %d",
									item->number, maxOpNumber)));
				processTypesSpec(item->class_args, &lefttype, &righttype);
				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = false;
				member->number = item->number;
				member->lefttype = lefttype;
				member->righttype = righttype;
				addFamilyMember(&operators, member);
				break;
			case OPCLASS_ITEM_FUNCTION:
				if (item->number <= 0 || item->number > maxProcNumber)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("invalid function number %d,"
									" must be between 1 and %d",
									item->number, maxProcNumber)));
				processTypesSpec(item->class_args, &lefttype, &righttype);
				/* 保存信息  */
				member = (OpFamilyMember *) palloc0(sizeof(OpFamilyMember));
				member->is_func = true;
				member->number = item->number;
				member->lefttype = lefttype;
				member->righttype = righttype;
				addFamilyMember(&procedures, member);
				break;
			case OPCLASS_ITEM_STORAGETYPE:
				/* 语法阻止此情况出现          */
			default:
				elog(ERROR, "unrecognized item type: %d", item->itemtype);
				break;
		}
	}

	/*
	 * 从 pg_amop 和 pg_amproc 中删除元组。
	 */
	dropOperators(stmt->opfamilyname, amoid, opfamilyoid, operators);
	dropProcedures(stmt->opfamilyname, amoid, opfamilyoid, procedures);

	/* 使信息对事件触发器可用            */
	EventTriggerCollectAlterOpFam(stmt, opfamilyoid,
								  operators, procedures);
}


/*
 * 处理 ALTER ADD/DROP 中使用的显式参数类型
 */
static void
processTypesSpec(List *args, Oid *lefttype, Oid *righttype)
{
	TypeName   *typeName;

	Assert(args != NIL);

	typeName = (TypeName *) linitial(args);
	*lefttype = typenameTypeId(NULL, typeName);

	if (list_length(args) > 1)
	{
		typeName = (TypeName *) lsecond(args);
		*righttype = typenameTypeId(NULL, typeName);
	}
	else
		*righttype = *lefttype;

	if (list_length(args) > 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("one or two argument types must be specified")));
}


/*
 * 确定要分配给操作符的 lefttype/righttype，
 * 并执行我们能做的任何有效性检查。
  */
static void
assignOperTypes(OpFamilyMember *member, Oid amoid, Oid typeoid)
{
	Operator	optup;
	Form_pg_operator opform;

	/* 获取操作符定义         */
	optup = SearchSysCache1(OPEROID, ObjectIdGetDatum(member->object));
	if (!HeapTupleIsValid(optup))
		elog(ERROR, "cache lookup failed for operator %u", member->object);
	opform = (Form_pg_operator) GETSTRUCT(optup);

	/*
	 * 操作符族操作符必须是二元的。
	 */
	if (opform->oprkind != 'b')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("index operators must be binary")));

	if (OidIsValid(member->sortfamily))
	{
		/*
		 * 排序操作符，检查索引是否支持。（我们或许也可以检查
		 * 操作符是否返回排序族支持的类型，但这在这里似乎比
		 * 其价值更麻烦。如果不支持，操作符将永远无法与任何
		 * ORDER BY 子句匹配，但不会产生更糟的后果。此外，
		 * 尝试检查将在转储/重新加载期间创建排序风险：
		 * 族可能已经创建但尚未填充所需的操作符。）
 *
 *
		                                                                        */
		IndexAmRoutine *amroutine = GetIndexAmRoutineByAmId(amoid, false);

		if (!amroutine->amcanorderbyop)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("access method \"%s\" does not support ordering operators",
							get_am_name(amoid))));
	}
	else
	{
		/*
		 * 搜索操作符必须返回布尔值。
		 */
		if (opform->oprresult != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index search operators must return boolean")));
	}

	/*
	 * 如果未指定 lefttype/righttype，则使用操作符的输入类型
	 */
	if (!OidIsValid(member->lefttype))
		member->lefttype = opform->oprleft;
	if (!OidIsValid(member->righttype))
		member->righttype = opform->oprright;

	ReleaseSysCache(optup);
}

/*
 * 确定要分配给支持过程的 lefttype/righttype，
 * 并执行我们能做的任何有效性检查。
       */
static void
assignProcTypes(OpFamilyMember *member, Oid amoid, Oid typeoid,
				int opclassOptsProcNum)
{
	HeapTuple	proctup;
	Form_pg_proc procform;

	/* 获取过程定义             */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(member->object));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", member->object);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	/* 检查操作符类选项解析函数的签名               */
	if (member->number == opclassOptsProcNum)
	{
		if (OidIsValid(typeoid))
		{
			if ((OidIsValid(member->lefttype) && member->lefttype != typeoid) ||
				(OidIsValid(member->righttype) && member->righttype != typeoid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("associated data types for operator class options parsing functions must match opclass input type")));
		}
		else
		{
			if (member->lefttype != member->righttype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("left and right associated data types for operator class options parsing functions must match")));
		}

		if (procform->prorettype != VOIDOID ||
			procform->pronargs != 1 ||
			procform->proargtypes.values[0] != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("invalid operator class options parsing function"),
					 errhint("Valid signature of operator class options parsing function is %s.",
							 "(internal) RETURNS void")));
	}

	/*
	 * btree 比较过程必须是返回 int4 的二元过程。btree
	 * sortsupport 过程必须接受 internal 并返回 void。btree in_range
	 * 过程必须是返回 bool 的五元过程。btree equalimage 过程必须
	 * 接受 1 个参数并返回 bool。hash 支持过程 1 必须是返回 int4
	 * 的一元过程，而过程 2 必须是返回 int8 的二元过程。
	 * 否则我们不知道。
	 */
	else if (amoid == BTREE_AM_OID)
	{
		if (member->number == BTORDER_PROC)
		{
			if (procform->pronargs != 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree comparison functions must have two arguments")));
			if (procform->prorettype != INT4OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree comparison functions must return integer")));

			/*
			 * 如果未指定 lefttype/righttype，
			 * 则使用过程的输入类型
			 */
			if (!OidIsValid(member->lefttype))
				member->lefttype = procform->proargtypes.values[0];
			if (!OidIsValid(member->righttype))
				member->righttype = procform->proargtypes.values[1];
		}
		else if (member->number == BTSORTSUPPORT_PROC)
		{
			if (procform->pronargs != 1 ||
				procform->proargtypes.values[0] != INTERNALOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree sort support functions must accept type \"internal\"")));
			if (procform->prorettype != VOIDOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree sort support functions must return void")));

			/*
			 * 无法从过程推断 lefttype/righttype，因此使用默认规则
			 */
		}
		else if (member->number == BTINRANGE_PROC)
		{
			if (procform->pronargs != 5)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree in_range functions must have five arguments")));
			if (procform->prorettype != BOOLOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree in_range functions must return boolean")));

			/*
			 * 如果未指定 lefttype/righttype，则使用过程的输入类型
			 * （我们查看测试值和偏移参数）
			     */
			if (!OidIsValid(member->lefttype))
				member->lefttype = procform->proargtypes.values[0];
			if (!OidIsValid(member->righttype))
				member->righttype = procform->proargtypes.values[2];
		}
		else if (member->number == BTEQUALIMAGE_PROC)
		{
			if (procform->pronargs != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree equal image functions must have one argument")));
			if (procform->prorettype != BOOLOID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree equal image functions must return boolean")));

			/*
			 * pg_amproc 函数按 (lefttype, righttype) 索引，但
			 * equalimage 函数只能在 CREATE INDEX 时调用。
			 * 相同的操作符类 opcintype OID 始终用于 lefttype 和
			 * righttype。提供跨类型例程不合理。
			 * 在此拒绝跨类型的 ALTER OPERATOR FAMILY ... ADD FUNCTION 4
			 * 语句。
			                                  */
			if (member->lefttype != member->righttype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree equal image functions must not be cross-type")));
		}
	}
	else if (amoid == HASH_AM_OID)
	{
		if (member->number == HASHSTANDARD_PROC)
		{
			if (procform->pronargs != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 1 must have one argument")));
			if (procform->prorettype != INT4OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 1 must return integer")));
		}
		else if (member->number == HASHEXTENDED_PROC)
		{
			if (procform->pronargs != 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 2 must have two arguments")));
			if (procform->prorettype != INT8OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash function 2 must return bigint")));
		}

		/*
		 * 如果未指定 lefttype/righttype，则使用过程的输入类型
		 */
		if (!OidIsValid(member->lefttype))
			member->lefttype = procform->proargtypes.values[0];
		if (!OidIsValid(member->righttype))
			member->righttype = procform->proargtypes.values[0];
	}

	/*
	 * CREATE OPERATOR CLASS 中的默认行为是使用类的 opcintype 作为
	 * lefttype 和 righttype。在 CREATE 或 ALTER OPERATOR FAMILY 中，
	 * opcintype 不可用，因此要求用户指定类型。
	 */
	if (!OidIsValid(member->lefttype))
		member->lefttype = typeoid;
	if (!OidIsValid(member->righttype))
		member->righttype = typeoid;

	if (!OidIsValid(member->lefttype) || !OidIsValid(member->righttype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("associated data types must be specified for index support function")));

	ReleaseSysCache(proctup);
}

/*
 * 在检查重复的策略号或过程号之后，将新的族成员添加到
 * 适当的列表中。
       */
static void
addFamilyMember(List **list, OpFamilyMember *member)
{
	ListCell   *l;

	foreach(l, *list)
	{
		OpFamilyMember *old = (OpFamilyMember *) lfirst(l);

		if (old->number == member->number &&
			old->lefttype == member->lefttype &&
			old->righttype == member->righttype)
		{
			if (member->is_func)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("function number %d for (%s,%s) appears more than once",
								member->number,
								format_type_be(member->lefttype),
								format_type_be(member->righttype))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator number %d for (%s,%s) appears more than once",
								member->number,
								format_type_be(member->lefttype),
								format_type_be(member->righttype))));
		}
	}
	*list = lappend(*list, member);
}

/*
 * 将操作符转储到 pg_amop
 *
 * 我们还在 pg_depend 中为 pg_amop 条目创建依赖项。
        */
static void
storeOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			   List *operators, bool isAdd)
{
	Relation	rel;
	Datum		values[Natts_pg_amop];
	bool		nulls[Natts_pg_amop];
	HeapTuple	tup;
	Oid			entryoid;
	ObjectAddress myself,
				referenced;
	ListCell   *l;

	rel = table_open(AccessMethodOperatorRelationId, RowExclusiveLock);

	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		char		oppurpose;

		/*
		 * 如果添加到现有族，检查与现有 pg_amop 条目的冲突
		 * （仅为了提供更好的错误消息）
		           */
		if (isAdd &&
			SearchSysCacheExists4(AMOPSTRATEGY,
								  ObjectIdGetDatum(opfamilyoid),
								  ObjectIdGetDatum(op->lefttype),
								  ObjectIdGetDatum(op->righttype),
								  Int16GetDatum(op->number)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("operator %d(%s,%s) already exists in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		oppurpose = OidIsValid(op->sortfamily) ? AMOP_ORDER : AMOP_SEARCH;

		/* 创建 pg_amop 条目    */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		entryoid = GetNewOidWithIndex(rel, AccessMethodOperatorOidIndexId,
									  Anum_pg_amop_oid);
		values[Anum_pg_amop_oid - 1] = ObjectIdGetDatum(entryoid);
		values[Anum_pg_amop_amopfamily - 1] = ObjectIdGetDatum(opfamilyoid);
		values[Anum_pg_amop_amoplefttype - 1] = ObjectIdGetDatum(op->lefttype);
		values[Anum_pg_amop_amoprighttype - 1] = ObjectIdGetDatum(op->righttype);
		values[Anum_pg_amop_amopstrategy - 1] = Int16GetDatum(op->number);
		values[Anum_pg_amop_amoppurpose - 1] = CharGetDatum(oppurpose);
		values[Anum_pg_amop_amopopr - 1] = ObjectIdGetDatum(op->object);
		values[Anum_pg_amop_amopmethod - 1] = ObjectIdGetDatum(amoid);
		values[Anum_pg_amop_amopsortfamily - 1] = ObjectIdGetDatum(op->sortfamily);

		tup = heap_form_tuple(rel->rd_att, values, nulls);

		CatalogTupleInsert(rel, tup);

		heap_freetuple(tup);

		/* 创建其依赖关系 */
		myself.classId = AccessMethodOperatorRelationId;
		myself.objectId = entryoid;
		myself.objectSubId = 0;

		referenced.classId = OperatorRelationId;
		referenced.objectId = op->object;
		referenced.objectSubId = 0;

		/* 有关依赖强度，请参阅 amapi.h 中的注释 */
		recordDependencyOn(&myself, &referenced,
						   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);

		referenced.classId = op->ref_is_family ? OperatorFamilyRelationId :
			OperatorClassRelationId;
		referenced.objectId = op->refobjid;
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced,
						   op->ref_is_hard ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

		if (typeDepNeeded(op->lefttype, op))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = op->lefttype;
			referenced.objectSubId = 0;

			/* 有关依赖强度，请参阅 amapi.h 中的注释 */
			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		if (op->lefttype != op->righttype &&
			typeDepNeeded(op->righttype, op))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = op->righttype;
			referenced.objectSubId = 0;

			/* 有关依赖强度，请参阅 amapi.h 中的注释 */
			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* 搜索操作符还需要依赖于引用的操作符族        */
		if (OidIsValid(op->sortfamily))
		{
			referenced.classId = OperatorFamilyRelationId;
			referenced.objectId = op->sortfamily;
			referenced.objectSubId = 0;

			recordDependencyOn(&myself, &referenced,
							   op->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* 此访问方法操作符的创建后钩子      */
		InvokeObjectPostCreateHook(AccessMethodOperatorRelationId,
								   entryoid, 0);
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * 将过程（支持例程）转储到 pg_amproc
 *
 * 我们还在 pg_depend 中为 pg_amproc 条目创建依赖项。
             */
static void
storeProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
				List *procedures, bool isAdd)
{
	Relation	rel;
	Datum		values[Natts_pg_amproc];
	bool		nulls[Natts_pg_amproc];
	HeapTuple	tup;
	Oid			entryoid;
	ObjectAddress myself,
				referenced;
	ListCell   *l;

	rel = table_open(AccessMethodProcedureRelationId, RowExclusiveLock);

	foreach(l, procedures)
	{
		OpFamilyMember *proc = (OpFamilyMember *) lfirst(l);

		/*
		 * 如果添加到现有族，检查与现有 pg_amproc 条目的冲突
		 * （仅为了提供更好的错误消息）
		           */
		if (isAdd &&
			SearchSysCacheExists4(AMPROCNUM,
								  ObjectIdGetDatum(opfamilyoid),
								  ObjectIdGetDatum(proc->lefttype),
								  ObjectIdGetDatum(proc->righttype),
								  Int16GetDatum(proc->number)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("function %d(%s,%s) already exists in operator family \"%s\"",
							proc->number,
							format_type_be(proc->lefttype),
							format_type_be(proc->righttype),
							NameListToString(opfamilyname))));

		/* 创建 pg_amproc 条目    */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		entryoid = GetNewOidWithIndex(rel, AccessMethodProcedureOidIndexId,
									  Anum_pg_amproc_oid);
		values[Anum_pg_amproc_oid - 1] = ObjectIdGetDatum(entryoid);
		values[Anum_pg_amproc_amprocfamily - 1] = ObjectIdGetDatum(opfamilyoid);
		values[Anum_pg_amproc_amproclefttype - 1] = ObjectIdGetDatum(proc->lefttype);
		values[Anum_pg_amproc_amprocrighttype - 1] = ObjectIdGetDatum(proc->righttype);
		values[Anum_pg_amproc_amprocnum - 1] = Int16GetDatum(proc->number);
		values[Anum_pg_amproc_amproc - 1] = ObjectIdGetDatum(proc->object);

		tup = heap_form_tuple(rel->rd_att, values, nulls);

		CatalogTupleInsert(rel, tup);

		heap_freetuple(tup);

		/* 创建其依赖关系 */
		myself.classId = AccessMethodProcedureRelationId;
		myself.objectId = entryoid;
		myself.objectSubId = 0;

		referenced.classId = ProcedureRelationId;
		referenced.objectId = proc->object;
		referenced.objectSubId = 0;

		/* 有关依赖强度，请参阅 amapi.h 中的注释 */
		recordDependencyOn(&myself, &referenced,
						   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);

		referenced.classId = proc->ref_is_family ? OperatorFamilyRelationId :
			OperatorClassRelationId;
		referenced.objectId = proc->refobjid;
		referenced.objectSubId = 0;

		recordDependencyOn(&myself, &referenced,
						   proc->ref_is_hard ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

		if (typeDepNeeded(proc->lefttype, proc))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = proc->lefttype;
			referenced.objectSubId = 0;

			/* 有关依赖强度，请参阅 amapi.h 中的注释 */
			recordDependencyOn(&myself, &referenced,
							   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		if (proc->lefttype != proc->righttype &&
			typeDepNeeded(proc->righttype, proc))
		{
			referenced.classId = TypeRelationId;
			referenced.objectId = proc->righttype;
			referenced.objectSubId = 0;

			/* 有关依赖强度，请参阅 amapi.h 中的注释 */
			recordDependencyOn(&myself, &referenced,
							   proc->ref_is_hard ? DEPENDENCY_NORMAL : DEPENDENCY_AUTO);
		}

		/* 访问方法过程的创建后钩子        */
		InvokeObjectPostCreateHook(AccessMethodProcedureRelationId,
								   entryoid, 0);
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * 检测 pg_amop 或 pg_amproc 条目是否需要对其 lefttype 或
 * righttype 的显式依赖。
 *
 * 我们创建这样的依赖，除非该条目通过其引用的操作符或函数
 * 有间接依赖。这对于操作符几乎总是成立，但对于支持函数
 * 可能不是。
                                  */
static bool
typeDepNeeded(Oid typid, OpFamilyMember *member)
{
	bool		result = true;

	/*
	 * 如果类型是固定的，我们不需要依赖。这或许有点违反分层
	 * （recordDependencyOn 无论如何都会忽略请求），但这是一个
	 * 廉价的测试，并且通常会在此处省去系统缓存查找。
 *
	     */
	if (IsPinnedObject(TypeRelationId, typid))
		return false;

	/* 不，所以检查函数或操作符的输入类型。      */
	if (member->is_func)
	{
		Oid		   *argtypes;
		int			nargs;

		(void) get_func_signature(member->object, &argtypes, &nargs);
		for (int i = 0; i < nargs; i++)
		{
			if (typid == argtypes[i])
			{
				result = false; /* 匹配，无需依赖      */
				break;
			}
		}
		pfree(argtypes);
	}
	else
	{
		Oid			lefttype,
					righttype;

		op_input_types(member->object, &lefttype, &righttype);
		if (typid == lefttype || typid == righttype)
			result = false;		/* 匹配，不需要依赖    */
	}
	return result;
}


/*
 * 从操作符族中删除操作符条目。
 *
 * 注意：这仅允许用于操作符族的"松散"成员，因此行为始终是 RESTRICT。
 *
    */
static void
dropOperators(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			  List *operators)
{
	ListCell   *l;

	foreach(l, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		Oid			amopid;
		ObjectAddress object;

		amopid = GetSysCacheOid4(AMOPSTRATEGY, Anum_pg_amop_oid,
								 ObjectIdGetDatum(opfamilyoid),
								 ObjectIdGetDatum(op->lefttype),
								 ObjectIdGetDatum(op->righttype),
								 Int16GetDatum(op->number));
		if (!OidIsValid(amopid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator %d(%s,%s) does not exist in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		object.classId = AccessMethodOperatorRelationId;
		object.objectId = amopid;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT, 0);
	}
}

/*
 * 从操作符族中删除过程条目。
 *
 * 注意：这仅允许用于操作符族的"松散"成员，因此行为始终是 RESTRICT。
 *
        */
static void
dropProcedures(List *opfamilyname, Oid amoid, Oid opfamilyoid,
			   List *procedures)
{
	ListCell   *l;

	foreach(l, procedures)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(l);
		Oid			amprocid;
		ObjectAddress object;

		amprocid = GetSysCacheOid4(AMPROCNUM, Anum_pg_amproc_oid,
								   ObjectIdGetDatum(opfamilyoid),
								   ObjectIdGetDatum(op->lefttype),
								   ObjectIdGetDatum(op->righttype),
								   Int16GetDatum(op->number));
		if (!OidIsValid(amprocid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("function %d(%s,%s) does not exist in operator family \"%s\"",
							op->number,
							format_type_be(op->lefttype),
							format_type_be(op->righttype),
							NameListToString(opfamilyname))));

		object.classId = AccessMethodProcedureRelationId;
		object.objectId = amprocid;
		object.objectSubId = 0;

		performDeletion(&object, DROP_RESTRICT, 0);
	}
}

/*
 * ALTER OPERATOR CLASS SET SCHEMA/RENAME 的子例程
 *
 * 给定命名空间中是否已存在具有给定名称和签名的
 * 操作符类？如果存在，则引发适当的错误消息。
        */
void
IsThereOpClassInNamespace(const char *opcname, Oid opcmethod,
						  Oid opcnamespace)
{
	/* 确保新名称不存在             */
	if (SearchSysCacheExists3(CLAAMNAMENSP,
							  ObjectIdGetDatum(opcmethod),
							  CStringGetDatum(opcname),
							  ObjectIdGetDatum(opcnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator class \"%s\" for access method \"%s\" already exists in schema \"%s\"",
						opcname,
						get_am_name(opcmethod),
						get_namespace_name(opcnamespace))));
}

/*
 * ALTER OPERATOR FAMILY SET SCHEMA/RENAME 的子例程
 *
 * 给定命名空间中是否已存在具有给定名称和签名的
 * 操作符族？如果存在，则引发适当的错误消息。
         */
void
IsThereOpFamilyInNamespace(const char *opfname, Oid opfmethod,
						   Oid opfnamespace)
{
	/* 确保新名称不存在             */
	if (SearchSysCacheExists3(OPFAMILYAMNAMENSP,
							  ObjectIdGetDatum(opfmethod),
							  CStringGetDatum(opfname),
							  ObjectIdGetDatum(opfnamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("operator family \"%s\" for access method \"%s\" already exists in schema \"%s\"",
						opfname,
						get_am_name(opfmethod),
						get_namespace_name(opfnamespace))));
}
