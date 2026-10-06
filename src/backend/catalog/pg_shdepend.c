/*-------------------------------------------------------------------------
 *
 * pg_shdepend.c
 *	  支持操作 pg_shdepend 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_shdepend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
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
#include "commands/typecmds.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

typedef enum
{
	LOCAL_OBJECT,
	SHARED_OBJECT,
	REMOTE_OBJECT
} SharedDependencyObjectType;

typedef struct
{
	ObjectAddress object;
	char		deptype;
	SharedDependencyObjectType objtype;
} ShDependObjectInfo;

static void getOidListDiff(Oid *list1, int *nlist1, Oid *list2, int *nlist2);
static Oid	classIdGetDbId(Oid classId);
static void shdepChangeDep(Relation sdepRel,
						   Oid classid, Oid objid, int32 objsubid,
						   Oid refclassid, Oid refobjid,
						   SharedDependencyType deptype);
static void shdepAddDependency(Relation sdepRel,
							   Oid classId, Oid objectId, int32 objsubId,
							   Oid refclassId, Oid refobjId,
							   SharedDependencyType deptype);
static void shdepDropDependency(Relation sdepRel,
								Oid classId, Oid objectId, int32 objsubId,
								bool drop_subobjects,
								Oid refclassId, Oid refobjId,
								SharedDependencyType deptype);
static void storeObjectDescription(StringInfo descs,
								   SharedDependencyObjectType type,
								   ObjectAddress *object,
								   SharedDependencyType deptype,
								   int count);


/*
 * recordSharedDependencyOn
 *
 * 通过各自的 ObjectAddresses 记录两个对象之间的依赖。
 * 第一个参数是依赖方对象，第二个是它所
 * 引用的对象（必须是共享对象）。
 *
 * 这会锁住被引用对象并确认它仍然存在。
 * 然后在 pg_shdepend 中创建一条条目。锁会一直持有到
 * 事务结束。
 *
 * 对被钉住对象的依赖不予记录。
 */
void
recordSharedDependencyOn(ObjectAddress *depender,
						 ObjectAddress *referenced,
						 SharedDependencyType deptype)
{
	Relation	sdepRel;

	/*
	 * pg_shdepend 中的对象不能有 SubIds。
	 */
	Assert(depender->objectSubId == 0);
	Assert(referenced->objectSubId == 0);

	/*
	 * 引导期间什么也不做，因为 pg_shdepend 可能尚不存在。
	 * initdb 会在引导之后填入适当的 pg_shdepend 条目。
	 */
	if (IsBootstrapProcessingMode())
		return;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/* 若被引用对象被钉住，则什么也不做。 */
	if (!IsPinnedObject(referenced->classId, referenced->objectId))
	{
		shdepAddDependency(sdepRel, depender->classId, depender->objectId,
						   depender->objectSubId,
						   referenced->classId, referenced->objectId,
						   deptype);
	}

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * recordDependencyOnOwner
 *
 * recordSharedDependencyOn 的便捷包装 —— 将指定
 * 用户登记为给定对象的所有者。
 *
 * 注意：调用者有责任确保该对象尚不存在
 * 所有者条目。
 */
void
recordDependencyOnOwner(Oid classId, Oid objectId, Oid owner)
{
	ObjectAddress myself,
				referenced;

	myself.classId = classId;
	myself.objectId = objectId;
	myself.objectSubId = 0;

	referenced.classId = AuthIdRelationId;
	referenced.objectId = owner;
	referenced.objectSubId = 0;

	recordSharedDependencyOn(&myself, &referenced, SHARED_DEPENDENCY_OWNER);
}

/*
 * shdepChangeDep
 *
 * 更新共享依赖记录，以反映被引用对象
 * 的更新。这是更改对象所有者等操作
 * 的内部工作例程。
 *
 * 给定的依赖方对象和依赖类型最多只能有一条现有
 * 条目！因此实际上这只能用于更新
 * SHARED_DEPENDENCY_OWNER 和 SHARED_DEPENDENCY_TABLESPACE
 * 条目，它们应具备该性质。
 *
 * 若没有先前条目，我们假定它引用的是 PINned
 * 对象，因此创建一条新条目。若新的被引用对象被
 * PINned，则不创建条目（并删除旧条目，若有）。
 * （对于表空间，某些情况下我们不记录依赖，因此
 * 条目缺少还可能有其他原因。）
 *
 * sdepRel 必须是 pg_shdepend 关系，且已打开并适当
 * 加锁。
 */
static void
shdepChangeDep(Relation sdepRel,
			   Oid classid, Oid objid, int32 objsubid,
			   Oid refclassid, Oid refobjid,
			   SharedDependencyType deptype)
{
	Oid			dbid = classIdGetDbId(classid);
	HeapTuple	oldtup = NULL;
	HeapTuple	scantup;
	ScanKeyData key[4];
	SysScanDesc scan;

	/*
	 * 确保在我们记录依赖期间，新的被引用对象不会
	 * 消失。
	 */
	shdepLockAndCheckObject(refclassid, refobjid);

	/*
	 * 查找先前条目
	 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classid));
	ScanKeyInit(&key[2],
				Anum_pg_shdepend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objid));
	ScanKeyInit(&key[3],
				Anum_pg_shdepend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(objsubid));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, 4, key);

	while ((scantup = systable_getnext(scan)) != NULL)
	{
		/* 若不是目标依赖类型则忽略 */
		if (((Form_pg_shdepend) GETSTRUCT(scantup))->deptype != deptype)
			continue;
		/* 若有多条匹配则调用者出错 */
		if (oldtup)
			elog(ERROR,
				 "multiple pg_shdepend entries for object %u/%u/%d deptype %c",
				 classid, objid, objsubid, deptype);
		oldtup = heap_copytuple(scantup);
	}

	systable_endscan(scan);

	if (IsPinnedObject(refclassid, refobjid))
	{
		/* 无需新条目，因此仅删除现有条目（若有） */
		if (oldtup)
			CatalogTupleDelete(sdepRel, &oldtup->t_self);
	}
	else if (oldtup)
	{
		/* 需要更新现有条目 */
		Form_pg_shdepend shForm = (Form_pg_shdepend) GETSTRUCT(oldtup);

		/* 由于 oldtup 是副本，可以直接在内存中修改 */
		shForm->refclassid = refclassid;
		shForm->refobjid = refobjid;

		CatalogTupleUpdate(sdepRel, &oldtup->t_self, oldtup);
	}
	else
	{
		/* 需要插入新条目 */
		Datum		values[Natts_pg_shdepend];
		bool		nulls[Natts_pg_shdepend];

		memset(nulls, false, sizeof(nulls));

		values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(dbid);
		values[Anum_pg_shdepend_classid - 1] = ObjectIdGetDatum(classid);
		values[Anum_pg_shdepend_objid - 1] = ObjectIdGetDatum(objid);
		values[Anum_pg_shdepend_objsubid - 1] = Int32GetDatum(objsubid);

		values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassid);
		values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjid);
		values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

		/*
		 * 我们重用 oldtup 只是为了避免声明新变量，但它
		 * 肯定是一个新元组
		 */
		oldtup = heap_form_tuple(RelationGetDescr(sdepRel), values, nulls);
		CatalogTupleInsert(sdepRel, oldtup);
	}

	if (oldtup)
		heap_freetuple(oldtup);
}

/*
 * changeDependencyOnOwner
 *
 * 更新共享依赖以反映新所有者。
 *
 * 注意：我们不需要 objsubid 参数，因为只有完整对象
 * 才有所有者。
 */
void
changeDependencyOnOwner(Oid classId, Oid objectId, Oid newOwnerId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/* 调整 SHARED_DEPENDENCY_OWNER 条目 */
	shdepChangeDep(sdepRel,
				   classId, objectId, 0,
				   AuthIdRelationId, newOwnerId,
				   SHARED_DEPENDENCY_OWNER);

	/*----------
	 * 所有者不应有 SHARED_DEPENDENCY_ACL 条目，
	 * 因此若存在则去掉它。若新所有者先前
	 * 被授予对该对象的某些权限，就会出现这种情况。
	 *
	 * 此步骤类似于 aclnewowner 去除 ACL 中的重复
	 * 条目。我们必须这样做以处理以下场景：
	 *		A 把某对象的一些权限授予 B
	 *		ALTER OWNER 将该对象的所有者改为 B
	 *		ALTER OWNER 将该对象的所有者改为 C
	 * 第三步会从对象的 ACL 中去掉所有关于 B 的记载，
	 * 但若我们不这样做，仍会留下 B 的
	 * SHARED_DEPENDENCY_ACL。
	 *
	 * 禁止所有者拥有 SHARED_DEPENDENCY_ACL 条目的规则
	 * 使我们只需在这一处修复，而不必
	 * 让各个 ALTER OWNER 例程都了解此事。
	 *----------
	 */
	shdepDropDependency(sdepRel, classId, objectId, 0, true,
						AuthIdRelationId, newOwnerId,
						SHARED_DEPENDENCY_ACL);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * recordDependencyOnTablespace
 *
 * recordSharedDependencyOn 的便捷包装 —— 将指定
 * 表空间登记为给定对象的默认表空间。
 *
 * 注意：调用者有责任确保该对象尚不存在
 * 表空间条目。
 */
void
recordDependencyOnTablespace(Oid classId, Oid objectId, Oid tablespace)
{
	ObjectAddress myself,
				referenced;

	ObjectAddressSet(myself, classId, objectId);
	ObjectAddressSet(referenced, TableSpaceRelationId, tablespace);

	recordSharedDependencyOn(&myself, &referenced,
							 SHARED_DEPENDENCY_TABLESPACE);
}

/*
 * changeDependencyOnTablespace
 *
 * 更新共享依赖以反映新表空间。
 *
 * 注意：我们不需要 objsubid 参数，因为只有完整对象
 * 才有表空间。
 */
void
changeDependencyOnTablespace(Oid classId, Oid objectId, Oid newTablespaceId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	if (newTablespaceId != DEFAULTTABLESPACE_OID &&
		newTablespaceId != InvalidOid)
		shdepChangeDep(sdepRel,
					   classId, objectId, 0,
					   TableSpaceRelationId, newTablespaceId,
					   SHARED_DEPENDENCY_TABLESPACE);
	else
		shdepDropDependency(sdepRel,
							classId, objectId, 0, true,
							InvalidOid, InvalidOid,
							SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * getOidListDiff
 *		updateAclDependencies 的辅助函数。
 *
 * 接受两个 Oid 数组并去掉两边共有的元素，
 * 只留下仅出现在其中一个输入中的元素。
 * 我们假定两个数组都已排序并去重。
 */
static void
getOidListDiff(Oid *list1, int *nlist1, Oid *list2, int *nlist2)
{
	int			in1,
				in2,
				out1,
				out2;

	in1 = in2 = out1 = out2 = 0;
	while (in1 < *nlist1 && in2 < *nlist2)
	{
		if (list1[in1] == list2[in2])
		{
			/* 跳过重复项 */
			in1++;
			in2++;
		}
		else if (list1[in1] < list2[in2])
		{
			/* list1[in1] 不在 list2 中 */
			list1[out1++] = list1[in1++];
		}
		else
		{
			/* list2[in2] 不在 list1 中 */
			list2[out2++] = list2[in2++];
		}
	}

	/* list1 中剩余条目都不在 list2 中 */
	while (in1 < *nlist1)
	{
		list1[out1++] = list1[in1++];
	}

	/* list2 中剩余条目都不在 list1 中 */
	while (in2 < *nlist2)
	{
		list2[out2++] = list2[in2++];
	}

	*nlist1 = out1;
	*nlist2 = out2;
}

/*
 * updateAclDependencies
 *		在 GRANT/REVOKE 期间更新对象 ACL 的 pg_shdepend 信息。
 *
 * classId, objectId, objsubId：标识该 ACL 所属的对象
 * ownerId：拥有该对象的角色
 * noldmembers, oldmembers：旧 ACL 中出现的 roleid 数组
 * nnewmembers, newmembers：新 ACL 中出现的 roleid 数组
 *
 * 我们计算新旧角色列表之间的差异，
 * 然后酌情向 pg_shdepend 插入或从中删除。
 *
 * 注意，GRANT 时不能盲目插入所有被引用角色，
 * 否则会得到重复登记的依赖。我们可以
 * 在插入前检查元组是否存在，但那似乎比
 * 这里的做法更昂贵。同样，REVOKE 时也不能盲目
 * 删除，因为该用户可能仍拥有其他权限。
 * REVOKE 实际上也可能增加依赖，原因是
 * 实例化了先前隐式的默认 ACL（尽管目前
 * 所有这类依赖都应属于拥有者角色，我们在此忽略）。
 *
 * 注意：两个输入数组都必须已排序并去重。（通常它们
 * 由 aclmembers() 从 ACL 数组提取，该函数会同时满足
 * 这两项要求。）返回前会 pfree 这些数组。
 */
void
updateAclDependencies(Oid classId, Oid objectId, int32 objsubId,
					  Oid ownerId,
					  int noldmembers, Oid *oldmembers,
					  int nnewmembers, Oid *newmembers)
{
	Relation	sdepRel;
	int			i;

	/*
	 * 去掉两边共有的条目；它们表示已有的
	 * 依赖，无需更改。
	 *
	 * 可以覆盖输入，因为反正我们会 pfree 它们。
	 */
	getOidListDiff(oldmembers, &noldmembers, newmembers, &nnewmembers);

	if (noldmembers > 0 || nnewmembers > 0)
	{
		sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

		/* 添加原先不存在的新依赖 */
		for (i = 0; i < nnewmembers; i++)
		{
			Oid			roleid = newmembers[i];

			/*
			 * 跳过所有者：他改用 OWNER 共享依赖条目。（这
			 * 不只是节省空间；也使 ALTER OWNER 更容易。见
			 * changeDependencyOnOwner 中的说明。）
			 */
			if (roleid == ownerId)
				continue;

			/* 跳过被钉住的角色；它们不需要依赖条目 */
			if (IsPinnedObject(AuthIdRelationId, roleid))
				continue;

			shdepAddDependency(sdepRel, classId, objectId, objsubId,
							   AuthIdRelationId, roleid,
							   SHARED_DEPENDENCY_ACL);
		}

		/* 删除不再使用的旧依赖 */
		for (i = 0; i < noldmembers; i++)
		{
			Oid			roleid = oldmembers[i];

			/* 跳过所有者，同上 */
			if (roleid == ownerId)
				continue;

			/* 跳过被钉住的角色 */
			if (IsPinnedObject(AuthIdRelationId, roleid))
				continue;

			shdepDropDependency(sdepRel, classId, objectId, objsubId,
								false,	/* 精确匹配 objsubId */
								AuthIdRelationId, roleid,
								SHARED_DEPENDENCY_ACL);
		}

		table_close(sdepRel, RowExclusiveLock);
	}

	if (oldmembers)
		pfree(oldmembers);
	if (newmembers)
		pfree(newmembers);
}

/*
 * 用于跟踪在其他数据库中发现的依赖的结构。
 */
typedef struct
{
	Oid			dbOid;
	int			count;
} remoteDep;

/*
 * ShDependObjectInfo 项的 qsort 比较器
 */
static int
shared_dependency_comparator(const void *a, const void *b)
{
	const ShDependObjectInfo *obja = (const ShDependObjectInfo *) a;
	const ShDependObjectInfo *objb = (const ShDependObjectInfo *) b;

	/*
	 * 主排序键是 OID 升序。
	 */
	if (obja->object.objectId < objb->object.objectId)
		return -1;
	if (obja->object.objectId > objb->object.objectId)
		return 1;

	/*
	 * 其次按系统目录 ID 排序，以防相同 OIDs 出现在不同
	 * 系统目录中。此处排序方向相当随意。
	 */
	if (obja->object.classId < objb->object.classId)
		return -1;
	if (obja->object.classId > objb->object.classId)
		return 1;

	/*
	 * 按对象 subId 排序。
	 *
	 * 我们将 subId 当作无符号 int 排序，以便 0（整个对象）会
	 * 排在前面。
	 */
	if ((unsigned int) obja->object.objectSubId < (unsigned int) objb->object.objectSubId)
		return -1;
	if ((unsigned int) obja->object.objectSubId > (unsigned int) objb->object.objectSubId)
		return 1;

	/*
	 * 最后按 deptype 排序，以防同一对象有多种依赖
	 * 类型。（注意无需考虑 objtype，因为它由
	 * 系统目录 OID 决定。）
	 */
	if (obja->deptype < objb->deptype)
		return -1;
	if (obja->deptype > objb->deptype)
		return 1;

	return 0;
}

/*
 * checkSharedDependencies
 *
 * 检查给定共享对象是否存在共享依赖条目；
 * 若有则返回 true。
 *
 * 此外，返回一个字符串，其中包含依赖该共享对象的对象
 * 描述的换行分隔列表；若未找到则为 NULL。
 * 我们实际返回两个这样的字符串；"detail" 结果适合作为
 * errdetail() 字符串返回给客户端，且长度受限。
 * "detail_log" 字符串可能长得多，应仅输出
 * 到服务器日志。
 *
 * 我们可能发现三种不同的依赖：对当前数据库中对象的
 * 依赖；对共享对象的依赖；以及对
 * 其他数据库本地对象的依赖。对前两种对象我们可以
 * （也确实会）提供描述，但对"远程"
 * 对象无法这样做，因此只提供它们的计数。
 */
bool
checkSharedDependencies(Oid classId, Oid objectId,
						char **detail_msg, char **detail_log_msg)
{
	Relation	sdepRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	int			numReportedDeps = 0;
	int			numNotReportedDeps = 0;
	int			numNotReportedDbs = 0;
	List	   *remDeps = NIL;
	ListCell   *cell;
	ObjectAddress object;
	ShDependObjectInfo *objects;
	int			numobjects;
	int			allocedobjects;
	StringInfoData descs;
	StringInfoData alldescs;

	/* 这种情况可以迅速处理 */
	if (IsPinnedObject(classId, objectId))
	{
		object.classId = classId;
		object.objectId = objectId;
		object.objectSubId = 0;
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because it is required by the database system",
						getObjectDescription(&object, false))));
	}

	/*
	 * 向客户端报告的依赖数量限制为
	 * MAX_REPORTED_DEPS，因为客户端软件可能不好处理
	 * 过长的错误字符串。服务器日志则始终得到完整报告。
	 *
	 * 为使回归测试结果稳定，我们在报告前按 OID 对本地和共享
	 * 对象排序。不过我们不关心
	 * 其他数据库的报告顺序。
	 */
#define MAX_REPORTED_DEPS 100

	allocedobjects = 128;		/* 任意的初始数组大小 */
	objects = (ShDependObjectInfo *)
		palloc(allocedobjects * sizeof(ShDependObjectInfo));
	numobjects = 0;
	initStringInfo(&descs);
	initStringInfo(&alldescs);

	sdepRel = table_open(SharedDependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_shdepend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tup);

		object.classId = sdepForm->classid;
		object.objectId = sdepForm->objid;
		object.objectSubId = sdepForm->objsubid;

		/*
		 * 若它是本数据库本地的依赖，或是共享
		 * 对象，则加入 objects 数组。
		 *
		 * 若是远程依赖，则跟踪它以便稍后报告
		 * 其数量。
		 */
		if (sdepForm->dbid == MyDatabaseId ||
			sdepForm->dbid == InvalidOid)
		{
			if (numobjects >= allocedobjects)
			{
				allocedobjects *= 2;
				objects = (ShDependObjectInfo *)
					repalloc(objects,
							 allocedobjects * sizeof(ShDependObjectInfo));
			}
			objects[numobjects].object = object;
			objects[numobjects].deptype = sdepForm->deptype;
			objects[numobjects].objtype = (sdepForm->dbid == MyDatabaseId) ?
				LOCAL_OBJECT : SHARED_OBJECT;
			numobjects++;
		}
		else
		{
			/* 既非本地也非共享，因此必为远程。 */
			remoteDep  *dep;
			bool		stored = false;

			/*
			 * XXX 此信息保存在简单的 List 上。也许对
			 * 性能不太好，但使用哈希表似乎不必要地
			 * 复杂。反正预期的数据库数量不会很多，
			 * 我想。
			 */
			foreach(cell, remDeps)
			{
				dep = lfirst(cell);
				if (dep->dbOid == sdepForm->dbid)
				{
					dep->count++;
					stored = true;
					break;
				}
			}
			if (!stored)
			{
				dep = (remoteDep *) palloc(sizeof(remoteDep));
				dep->dbOid = sdepForm->dbid;
				dep->count = 1;
				remDeps = lappend(remDeps, dep);
			}
		}
	}

	systable_endscan(scan);

	table_close(sdepRel, AccessShareLock);

	/*
	 * 排序并报告本地和共享对象。
	 */
	if (numobjects > 1)
		qsort(objects, numobjects,
			  sizeof(ShDependObjectInfo), shared_dependency_comparator);

	for (int i = 0; i < numobjects; i++)
	{
		if (numReportedDeps < MAX_REPORTED_DEPS)
		{
			numReportedDeps++;
			storeObjectDescription(&descs,
								   objects[i].objtype,
								   &objects[i].object,
								   objects[i].deptype,
								   0);
		}
		else
			numNotReportedDeps++;
		storeObjectDescription(&alldescs,
							   objects[i].objtype,
							   &objects[i].object,
							   objects[i].deptype,
							   0);
	}

	/*
	 * 汇总远程数据库中的依赖。
	 */
	foreach(cell, remDeps)
	{
		remoteDep  *dep = lfirst(cell);

		object.classId = DatabaseRelationId;
		object.objectId = dep->dbOid;
		object.objectSubId = 0;

		if (numReportedDeps < MAX_REPORTED_DEPS)
		{
			numReportedDeps++;
			storeObjectDescription(&descs, REMOTE_OBJECT, &object,
								   SHARED_DEPENDENCY_INVALID, dep->count);
		}
		else
			numNotReportedDbs++;
		storeObjectDescription(&alldescs, REMOTE_OBJECT, &object,
							   SHARED_DEPENDENCY_INVALID, dep->count);
	}

	pfree(objects);
	list_free_deep(remDeps);

	if (descs.len == 0)
	{
		pfree(descs.data);
		pfree(alldescs.data);
		*detail_msg = *detail_log_msg = NULL;
		return false;
	}

	if (numNotReportedDeps > 0)
		appendStringInfo(&descs, ngettext("\nand %d other object "
										  "(see server log for list)",
										  "\nand %d other objects "
										  "(see server log for list)",
										  numNotReportedDeps),
						 numNotReportedDeps);
	if (numNotReportedDbs > 0)
		appendStringInfo(&descs, ngettext("\nand objects in %d other database "
										  "(see server log for list)",
										  "\nand objects in %d other databases "
										  "(see server log for list)",
										  numNotReportedDbs),
						 numNotReportedDbs);

	*detail_msg = descs.data;
	*detail_log_msg = alldescs.data;
	return true;
}


/*
 * copyTemplateDependencies
 *
 * 为新数据库创建初始共享依赖的例程。
 * 我们只是从模板数据库复制依赖。
 */
void
copyTemplateDependencies(Oid templateDbId, Oid newDbId)
{
	Relation	sdepRel;
	TupleDesc	sdepDesc;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	CatalogIndexState indstate;
	TupleTableSlot **slot;
	int			max_slots,
				slot_init_count,
				slot_stored_count;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);
	sdepDesc = RelationGetDescr(sdepRel);

	/*
	 * 分配将要用的槽，但推迟昂贵的初始化，直到
	 * 知道它们会被使用。
	 */
	max_slots = MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_shdepend);
	slot = palloc(sizeof(TupleTableSlot *) * max_slots);

	indstate = CatalogOpenIndexes(sdepRel);

	/* 扫描 dbid = templateDbId 的所有条目 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(templateDbId));

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, 1, key);

	/* 当前存放着元组的槽数 */
	slot_stored_count = 0;
	/* 当前已初始化的槽数 */
	slot_init_count = 0;

	/*
	 * 复制原数据库的条目，将数据库 Id 改为
	 * 新数据库的。注意，因为我们不复制
	 * dbId == 0 的行（即描述依赖方共享对象的行），所以不会
	 * 复制模板数据库自身的所有权依赖；这正是
	 * 我们想要的。
	 */
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_shdepend shdep;

		if (slot_init_count < max_slots)
		{
			slot[slot_stored_count] = MakeSingleTupleTableSlot(sdepDesc, &TTSOpsHeapTuple);
			slot_init_count++;
		}

		ExecClearTuple(slot[slot_stored_count]);

		memset(slot[slot_stored_count]->tts_isnull, false,
			   slot[slot_stored_count]->tts_tupleDescriptor->natts * sizeof(bool));

		shdep = (Form_pg_shdepend) GETSTRUCT(tup);

		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(newDbId);
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_classid - 1] = shdep->classid;
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_objid - 1] = shdep->objid;
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_objsubid - 1] = shdep->objsubid;
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_refclassid - 1] = shdep->refclassid;
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_refobjid - 1] = shdep->refobjid;
		slot[slot_stored_count]->tts_values[Anum_pg_shdepend_deptype - 1] = shdep->deptype;

		ExecStoreVirtualTuple(slot[slot_stored_count]);
		slot_stored_count++;

		/* 若槽已满，则插入一批元组 */
		if (slot_stored_count == max_slots)
		{
			CatalogTuplesMultiInsertWithInfo(sdepRel, slot, slot_stored_count, indstate);
			slot_stored_count = 0;
		}
	}

	/* 插入缓冲区中剩余的任何元组 */
	if (slot_stored_count > 0)
		CatalogTuplesMultiInsertWithInfo(sdepRel, slot, slot_stored_count, indstate);

	systable_endscan(scan);

	CatalogCloseIndexes(indstate);
	table_close(sdepRel, RowExclusiveLock);

	/* 只丢弃已使用的槽数 */
	for (int i = 0; i < slot_init_count; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/*
 * dropDatabaseDependencies
 *
 * 删除正被删除的数据库所对应的 pg_shdepend
 * 条目。
 */
void
dropDatabaseDependencies(Oid databaseId)
{
	Relation	sdepRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/*
	 * 首先，删除 dbid 字段中带有该数据库 Oid 的所有
	 * 条目。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(databaseId));
	/* 其余索引字段保持未指定 */

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(sdepRel, &tup->t_self);
	}

	systable_endscan(scan);

	/* 现在删除对应该数据库自身的所有条目 */
	shdepDropDependency(sdepRel, DatabaseRelationId, databaseId, 0, true,
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * deleteSharedDependencyRecordsFor
 *
 * 删除正被删除或修改的对象所对应的所有
 * pg_shdepend 条目。假定该对象要么是共享对象，
 * 要么是当前数据库本地的（由 classId 告诉我们是哪一种）。
 *
 * 若 objectSubId 为零，则我们在删除整个对象，因此也去掉
 * 子对象的 pg_shdepend 条目。
 */
void
deleteSharedDependencyRecordsFor(Oid classId, Oid objectId, int32 objectSubId)
{
	Relation	sdepRel;

	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	shdepDropDependency(sdepRel, classId, objectId, objectSubId,
						(objectSubId == 0),
						InvalidOid, InvalidOid,
						SHARED_DEPENDENCY_INVALID);

	table_close(sdepRel, RowExclusiveLock);
}

/*
 * shdepAddDependency
 *		向 pg_shdepend 插入的内部工作例程
 *
 * sdepRel 必须是 pg_shdepend 关系，且已打开并适当
 * 加锁。
 */
static void
shdepAddDependency(Relation sdepRel,
				   Oid classId, Oid objectId, int32 objsubId,
				   Oid refclassId, Oid refobjId,
				   SharedDependencyType deptype)
{
	HeapTuple	tup;
	Datum		values[Natts_pg_shdepend];
	bool		nulls[Natts_pg_shdepend];

	/*
	 * 确保在我们记录对它的依赖期间该对象不会
	 * 消失。DROP 例程应在检查共享依赖之前
	 * 以排他方式锁住该对象。
	 */
	shdepLockAndCheckObject(refclassId, refobjId);

	memset(nulls, false, sizeof(nulls));

	/*
	 * 构造新元组并登记该依赖。
	 */
	values[Anum_pg_shdepend_dbid - 1] = ObjectIdGetDatum(classIdGetDbId(classId));
	values[Anum_pg_shdepend_classid - 1] = ObjectIdGetDatum(classId);
	values[Anum_pg_shdepend_objid - 1] = ObjectIdGetDatum(objectId);
	values[Anum_pg_shdepend_objsubid - 1] = Int32GetDatum(objsubId);

	values[Anum_pg_shdepend_refclassid - 1] = ObjectIdGetDatum(refclassId);
	values[Anum_pg_shdepend_refobjid - 1] = ObjectIdGetDatum(refobjId);
	values[Anum_pg_shdepend_deptype - 1] = CharGetDatum(deptype);

	tup = heap_form_tuple(sdepRel->rd_att, values, nulls);

	CatalogTupleInsert(sdepRel, tup);

	/* 清理 */
	heap_freetuple(tup);
}

/*
 * shdepDropDependency
 *		从 pg_shdepend 删除条目的内部工作例程。
 *
 * 我们删除具有以下性质的条目：
 *	依赖方对象是 classId/objectId/objsubId 所标识的那个
 *	若 refclassId 不是 InvalidOid，则必须匹配条目的 refclassid
 *	若 refobjId 不是 InvalidOid，则必须匹配条目的 refobjid
 *	若 deptype 不是 SHARED_DEPENDENCY_INVALID，则必须匹配条目的 deptype
 *
 * 若 drop_subobjects 为 true，则忽略 objsubId 并考虑所有匹配
 * classId/objectId 的条目。
 *
 * sdepRel 必须是 pg_shdepend 关系，且已打开并适当
 * 加锁。
 */
static void
shdepDropDependency(Relation sdepRel,
					Oid classId, Oid objectId, int32 objsubId,
					bool drop_subobjects,
					Oid refclassId, Oid refobjId,
					SharedDependencyType deptype)
{
	ScanKeyData key[4];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;

	/* 扫描匹配依赖方对象的条目 */
	ScanKeyInit(&key[0],
				Anum_pg_shdepend_dbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classIdGetDbId(classId)));
	ScanKeyInit(&key[1],
				Anum_pg_shdepend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[2],
				Anum_pg_shdepend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));
	if (drop_subobjects)
		nkeys = 3;
	else
	{
		ScanKeyInit(&key[3],
					Anum_pg_shdepend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(objsubId));
		nkeys = 4;
	}

	scan = systable_beginscan(sdepRel, SharedDependDependerIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_shdepend shdepForm = (Form_pg_shdepend) GETSTRUCT(tup);

		/* 按附加参数过滤条目 */
		if (OidIsValid(refclassId) && shdepForm->refclassid != refclassId)
			continue;
		if (OidIsValid(refobjId) && shdepForm->refobjid != refobjId)
			continue;
		if (deptype != SHARED_DEPENDENCY_INVALID &&
			shdepForm->deptype != deptype)
			continue;

		/* 可以，删除它 */
		CatalogTupleDelete(sdepRel, &tup->t_self);
	}

	systable_endscan(scan);
}

/*
 * classIdGetDbId
 *
 * 给定包含该对象的系统目录的 OID，取得应在 pg_shdepend 中
 * 使用的数据库 Id。对于共享对象，为 0
 * （InvalidOid）；对于所有其他对象，为当前数据库 Id。
 */
static Oid
classIdGetDbId(Oid classId)
{
	Oid			dbId;

	if (IsSharedRelation(classId))
		dbId = InvalidOid;
	else
		dbId = MyDatabaseId;

	return dbId;
}

/*
 * shdepLockAndCheckObject
 *
 * 锁住我们即将记录依赖的对象。
 * 加锁之后，验证在我们没注意时它没有被
 * 删除。若对象已被删除，本函数
 * 不会返回！
 */
void
shdepLockAndCheckObject(Oid classId, Oid objectId)
{
	/* AccessShareLock 应该可以，因为我们不修改该对象 */
	LockSharedObject(classId, objectId, 0, AccessShareLock);

	switch (classId)
	{
		case AuthIdRelationId:
			if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(objectId)))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role %u was concurrently dropped",
								objectId)));
			break;

		case TableSpaceRelationId:
			{
				/* 由于 pg_tablespace 没有 syscache，这样做： */
				char	   *tablespace = get_tablespace_name(objectId);

				if (tablespace == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace %u was concurrently dropped",
									objectId)));
				pfree(tablespace);
				break;
			}

		case DatabaseRelationId:
			{
				/* 由于 pg_database 没有 syscache，这样做： */
				char	   *database = get_database_name(objectId);

				if (database == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("database %u was concurrently dropped",
									objectId)));
				pfree(database);
				break;
			}


		default:
			elog(ERROR, "unrecognized shared classId: %u", classId);
	}
}


/*
 * storeObjectDescription
 *		将依赖方对象的描述追加到 "descs"
 *
 * 在搜索共享对象的依赖时，我们把找到的
 * 依赖方对象的描述暂存在一个字符串中，稍后当
 * 有人试图删除被引用的共享对象时，作为 DETAIL
 * 字段传给 ereport()。
 *
 * 当 type 为 LOCAL_OBJECT 或 SHARED_OBJECT 时，我们期望 object 是
 * 依赖方对象，deptype 是依赖类型，且不使用 count。
 * 当 type 为 REMOTE_OBJECT 时，我们期望 object 是数据库对象，
 * 且 count 非零；此时不使用 deptype。
 */
static void
storeObjectDescription(StringInfo descs,
					   SharedDependencyObjectType type,
					   ObjectAddress *object,
					   SharedDependencyType deptype,
					   int count)
{
	char	   *objdesc = getObjectDescription(object, false);

	/*
	 * 正被并发删除的对象无需报告。
	 */
	if (objdesc == NULL)
		return;

	/* 用换行分隔各条目 */
	if (descs->len != 0)
		appendStringInfoChar(descs, '\n');

	switch (type)
	{
		case LOCAL_OBJECT:
		case SHARED_OBJECT:
			if (deptype == SHARED_DEPENDENCY_OWNER)
				appendStringInfo(descs, _("owner of %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_ACL)
				appendStringInfo(descs, _("privileges for %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_POLICY)
				appendStringInfo(descs, _("target of %s"), objdesc);
			else if (deptype == SHARED_DEPENDENCY_TABLESPACE)
				appendStringInfo(descs, _("tablespace for %s"), objdesc);
			else
				elog(ERROR, "unrecognized dependency type: %d",
					 (int) deptype);
			break;

		case REMOTE_OBJECT:
			/* translator: %s will always be "database %s" */
			appendStringInfo(descs, ngettext("%d object in %s",
											 "%d objects in %s",
											 count),
							 count, objdesc);
			break;

		default:
			elog(ERROR, "unrecognized object type: %d", type);
	}

	pfree(objdesc);
}


/*
 * shdepDropOwned
 *
 * 删除任一给定 RoleIds 所拥有的对象。若某角色对
 * 某对象有访问权限，则该授权也会被移除（但对象
 * 当然不会）。
 *
 * 扫描时可以立即收回授权，但删除会
 * 先攒起来，再用 performMultipleDeletions 一次性完成。这
 * 是必要的，以免因试图按错误顺序删除
 * 相互依赖的对象而失败。
 */
void
shdepDropOwned(List *roleids, DropBehavior behavior)
{
	Relation	sdepRel;
	ListCell   *cell;
	ObjectAddresses *deleteobjs;

	deleteobjs = new_object_addresses();

	/*
	 * 我们在此并不需要这么强的锁，但将调用的例程会
	 * 获取 RowExclusiveLock。最好现在就取，以免潜在的
	 * 死锁失败。
	 */
	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	/*
	 * 对每个角色，找出依赖对象并用
	 * 常规（非共享）依赖管理删除它们。
	 */
	foreach(cell, roleids)
	{
		Oid			roleid = lfirst_oid(cell);
		ScanKeyData key[2];
		SysScanDesc scan;
		HeapTuple	tuple;

		/* 对被钉住对象不起作用 */
		if (IsPinnedObject(AuthIdRelationId, roleid))
		{
			ObjectAddress obj;

			obj.classId = AuthIdRelationId;
			obj.objectId = roleid;
			obj.objectSubId = 0;

			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop objects owned by %s because they are "
							"required by the database system",
							getObjectDescription(&obj, false))));
		}

		ScanKeyInit(&key[0],
					Anum_pg_shdepend_refclassid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AuthIdRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_shdepend_refobjid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
								  NULL, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);
			ObjectAddress obj;

			/*
			 * 我们只操作共享对象和当前
			 * 数据库中的对象
			 */
			if (sdepForm->dbid != MyDatabaseId &&
				sdepForm->dbid != InvalidOid)
				continue;

			switch (sdepForm->deptype)
			{
					/* 不应发生 */
				case SHARED_DEPENDENCY_INVALID:
					elog(ERROR, "unexpected dependency type");
					break;
				case SHARED_DEPENDENCY_POLICY:

					/*
					 * 尝试从策略中移除角色；若无法做到，则删除
					 * 策略。
					 */
					if (!RemoveRoleFromObjectPolicy(roleid,
													sdepForm->classid,
													sdepForm->objid))
					{
						obj.classId = sdepForm->classid;
						obj.objectId = sdepForm->objid;
						obj.objectSubId = sdepForm->objsubid;

						/*
						 * 获取对象上的锁，然后验证此依赖
						 * 仍然相关。若否，则对象可能已
						 * 被删除或策略已修改。那种情况下忽略
						 * 该对象。
						 */
						AcquireDeletionLock(&obj, 0);
						if (!systable_recheck_tuple(scan, tuple))
						{
							ReleaseDeletionLock(&obj);
							break;
						}
						add_exact_object_address(&obj, deleteobjs);
					}
					break;
				case SHARED_DEPENDENCY_ACL:

					/*
					 * 对角色授予的依赖用
					 * SHARED_DEPENDENCY_ACL 记录，但与常规 ACL 列表不同，
					 * 后者把某对象的全部权限存在
					 * 单个 ACL 数组中，这里每条授予有单独的系统目录行 ——
					 * 因此移除授予就意味着删除
					 * 整行。
					 */
					if (sdepForm->classid != AuthMemRelationId)
					{
						RemoveRoleFromObjectACL(roleid,
												sdepForm->classid,
												sdepForm->objid);
						break;
					}
					/* FALLTHROUGH */

				case SHARED_DEPENDENCY_OWNER:

					/*
					 * 若是本地对象或角色授予，则保存下来供下方删除。
					 * 其他共享对象，例如数据库，
					 * 不应在此移除。
					 */
					if (sdepForm->dbid == MyDatabaseId ||
						sdepForm->classid == AuthMemRelationId)
					{
						obj.classId = sdepForm->classid;
						obj.objectId = sdepForm->objid;
						obj.objectSubId = sdepForm->objsubid;
						/* 同上 */
						AcquireDeletionLock(&obj, 0);
						if (!systable_recheck_tuple(scan, tuple))
						{
							ReleaseDeletionLock(&obj);
							break;
						}
						add_exact_object_address(&obj, deleteobjs);
					}
					break;
			}
		}

		systable_endscan(scan);
	}

	/*
	 * 为使删除报告顺序稳定，在删除前把对象排成
	 * 大致的反向创建顺序。（这或许也会
	 * 让删除稍快一些，因为因依赖而必须
	 * 重排对象的机会更少。）
	 */
	sort_object_addresses(deleteobjs);

	/* 由依赖机制完成实际工作 */
	performMultipleDeletions(deleteobjs, behavior, 0);

	table_close(sdepRel, RowExclusiveLock);

	free_object_addresses(deleteobjs);
}

/*
 * shdepReassignOwned
 *
 * 把 roleids 中任一角色所拥有对象的所有者改为
 * newrole。不触及授权。
 */
void
shdepReassignOwned(List *roleids, Oid newrole)
{
	Relation	sdepRel;
	ListCell   *cell;

	/*
	 * 我们在此并不需要这么强的锁，但将调用的例程会
	 * 获取 RowExclusiveLock。最好现在就取，以免潜在的
	 * 死锁问题。
	 */
	sdepRel = table_open(SharedDependRelationId, RowExclusiveLock);

	foreach(cell, roleids)
	{
		SysScanDesc scan;
		ScanKeyData key[2];
		HeapTuple	tuple;
		Oid			roleid = lfirst_oid(cell);

		/* 拒绝处理被钉住的角色 */
		if (IsPinnedObject(AuthIdRelationId, roleid))
		{
			ObjectAddress obj;

			obj.classId = AuthIdRelationId;
			obj.objectId = roleid;
			obj.objectSubId = 0;

			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot reassign ownership of objects owned by %s because they are required by the database system",
							getObjectDescription(&obj, false))));

			/*
			 * 没必要把全部实情说出来，那就是我们
			 * 根本没有跟踪这些依赖……
			 */
		}

		ScanKeyInit(&key[0],
					Anum_pg_shdepend_refclassid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AuthIdRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_shdepend_refobjid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		scan = systable_beginscan(sdepRel, SharedDependReferenceIndexId, true,
								  NULL, 2, key);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			Form_pg_shdepend sdepForm = (Form_pg_shdepend) GETSTRUCT(tuple);
			MemoryContext cxt,
						oldcxt;

			/*
			 * 我们只操作共享对象和当前
			 * 数据库中的对象
			 */
			if (sdepForm->dbid != MyDatabaseId &&
				sdepForm->dbid != InvalidOid)
				continue;

			/* 我们不动非所有者依赖 */
			if (sdepForm->deptype != SHARED_DEPENDENCY_OWNER)
				continue;

			/*
			 * 各种 ALTER OWNER 例程往往会在
			 * CurrentMemoryContext 中泄漏内存。若每个命令只
			 * 调用一次，这不是问题；但在这种可能
			 * 触及许多对象的用法中，会变成严重的内存泄漏。
			 * 把每次调用放在短生命周期的上下文中即可修复。
			 */
			cxt = AllocSetContextCreate(CurrentMemoryContext,
										"shdepReassignOwned",
										ALLOCSET_DEFAULT_SIZES);
			oldcxt = MemoryContextSwitchTo(cxt);

			/* 发出适当的 ALTER OWNER 调用 */
			switch (sdepForm->classid)
			{
				case TypeRelationId:
					AlterTypeOwner_oid(sdepForm->objid, newrole, true);
					break;

				case NamespaceRelationId:
					AlterSchemaOwner_oid(sdepForm->objid, newrole);
					break;

				case RelationRelationId:

					/*
					 * 传入 recursing = true，以免在碰巧先访问索引、
					 * 附属序列等、后访问其父表时
					 * 失败。
					 */
					ATExecChangeOwner(sdepForm->objid, newrole, true, AccessExclusiveLock);
					break;

				case DefaultAclRelationId:

					/*
					 * 忽略默认 ACLs；它们应由 DROP
					 * OWNED 处理，而不是 REASSIGN OWNED。
					 */
					break;

				case UserMappingRelationId:
					/* 同上 */
					break;

				case ForeignServerRelationId:
					AlterForeignServerOwner_oid(sdepForm->objid, newrole);
					break;

				case ForeignDataWrapperRelationId:
					AlterForeignDataWrapperOwner_oid(sdepForm->objid, newrole);
					break;

				case EventTriggerRelationId:
					AlterEventTriggerOwner_oid(sdepForm->objid, newrole);
					break;

				case PublicationRelationId:
					AlterPublicationOwner_oid(sdepForm->objid, newrole);
					break;

				case SubscriptionRelationId:
					AlterSubscriptionOwner_oid(sdepForm->objid, newrole);
					break;

					/* 通用更改所有者情形 */
				case CollationRelationId:
				case ConversionRelationId:
				case OperatorRelationId:
				case ProcedureRelationId:
				case LanguageRelationId:
				case LargeObjectRelationId:
				case OperatorFamilyRelationId:
				case OperatorClassRelationId:
				case ExtensionRelationId:
				case StatisticExtRelationId:
				case TableSpaceRelationId:
				case DatabaseRelationId:
				case TSConfigRelationId:
				case TSDictionaryRelationId:
					{
						Oid			classId = sdepForm->classid;
						Relation	catalog;

						/*
						 * 对于大对象，要修改的系统目录是
						 * pg_largeobject_metadata
						 */
						if (classId == LargeObjectRelationId)
							classId = LargeObjectMetadataRelationId;

						catalog = table_open(classId, RowExclusiveLock);

						AlterObjectOwner_internal(catalog, sdepForm->objid,
												  newrole);

						table_close(catalog, NoLock);
					}
					break;

				default:
					elog(ERROR, "unexpected classid %u", sdepForm->classid);
					break;
			}

			/* 清理 */
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(cxt);

			/* 确保下一次迭代能看到我的更改 */
			CommandCounterIncrement();
		}

		systable_endscan(scan);
	}

	table_close(sdepRel, RowExclusiveLock);
}
