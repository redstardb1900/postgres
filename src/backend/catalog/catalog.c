/*-------------------------------------------------------------------------
 *
 * catalog.c
 *		与目录命名约定及其他硬编码知识
 *		相关的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/catalog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 决定 GetNewOidWithIndex() 何时发出日志消息的
 * 参数
 */
#define GETNEWOID_LOG_THRESHOLD 1000000
#define GETNEWOID_LOG_MAX_INTERVAL 128000000

/*
 * IsSystemRelation
 *		当且仅当关系是系统目录或 toast 表时为真。
 *		系统目录的精确定义见 IsCatalogRelation。
 *
 *		出于保护目的，我们把用户关系的 toast 表也当作
 *		"系统关系"，例如未获特殊权限不能改其模式。
 *		因此本函数的多数用法是在检查
 *		allow_system_table_mods 限制是否适用。
 *		若用于其他目的，请考虑是否应改用
 *		IsCatalogRelation。
 *
 *		本函数不做任何目录访问。
 *		有些调用方依赖这一点！
 */
bool
IsSystemRelation(Relation relation)
{
	return IsSystemClass(RelationGetRelid(relation), relation->rd_rel);
}

/*
 * IsSystemClass
 *		同上，但以 Form_pg_class 为参数。
 *		用于不想打开关系、而必须直接
 *		搜索 pg_class 的场合。
 */
bool
IsSystemClass(Oid relid, Form_pg_class reltuple)
{
	/* IsCatalogRelationOid 更快一些，因此先测它 */
	return (IsCatalogRelationOid(relid) || IsToastClass(reltuple));
}

/*
 * IsCatalogRelation
 *		当且仅当关系是系统目录时为真。
 *
 *		所谓系统目录，指在 initdb 引导阶段创建的
 *		那些关系。不仅包括目录本身，还包括
 *		其索引，以及若有的 TOAST 表与索引。
 *
 *		本函数不做任何目录访问。
 *		有些调用方依赖这一点！
 */
bool
IsCatalogRelation(Relation relation)
{
	return IsCatalogRelationOid(RelationGetRelid(relation));
}

/*
 * IsCatalogRelationOid
 *		当且仅当此 OID 标识的关系是系统目录时为真。
 *
 *		所谓系统目录，指在 initdb 引导阶段创建的
 *		那些关系。不仅包括目录本身，还包括
 *		其索引，以及若有的 TOAST 表与索引。
 *
 *		本函数不做任何目录访问。
 *		有些调用方依赖这一点！
 */
bool
IsCatalogRelationOid(Oid relid)
{
	/*
	 * 若关系拥有钉住的 OID，我们就视其为系统目录。
	 * 这涵盖所有已定义的目录、其索引，以及它们的 TOAST
	 * 表与索引。
	 *
	 * 该规则排除 information_schema 中的关系，它们并非
	 * 系统不可或缺，可与用户关系同等对待。
	 * （既然可以合法地删除并重建 information_schema，任何
	 * 不如此处理的规则都会是错的。）
	 *
	 * 该测试是可靠的，因为 OID 回绕会跳过这一范围的
	 * OIDs；见 GetNewObjectId()。
	 */
	return (relid < (Oid) FirstUnpinnedObjectId);
}

/*
 * IsInplaceUpdateRelation
 *		当且仅当核心代码对该关系做原地更新时为真。
 *
 *		用于断言，并让执行器遵循 README.tuplock 中
 *		"Locking to write inplace-updated tables"一节所述的
 *		加锁协议。扩展可能原地更新其他堆表，但
 *		对同一表的并发 SQL UPDATE 可能覆盖那些
 *		修改。
 *
 *		执行器可以假定这些不是分区、也不是已分区表，
 *		且没有触发器。
 */
bool
IsInplaceUpdateRelation(Relation relation)
{
	return IsInplaceUpdateOid(RelationGetRelid(relation));
}

/*
 * IsInplaceUpdateOid
 *		同上，但以 OID 为参数。
 */
bool
IsInplaceUpdateOid(Oid relid)
{
	return (relid == RelationRelationId ||
			relid == DatabaseRelationId);
}

/*
 * IsToastRelation
 *		当且仅当关系是 TOAST 支撑关系（或索引）时为真。
 *
 *		不做任何目录访问。
 */
bool
IsToastRelation(Relation relation)
{
	/*
	 * 实际检查的是关系是否属于某个 pg_toast
	 * 命名空间。这应当等价，因为别处已限制不得在
	 * pg_toast 命名空间中创建用户关系，或把关系移入/
	 * 移出该命名空间。另请注意：对属于其他会话临时
	 * 表的 toast 表，本函数不会返回"真"；我们期望
	 * 其他机制会阻止访问那些对象。
	 */
	return IsToastNamespace(RelationGetNamespace(relation));
}

/*
 * IsToastClass
 *		同上，但以 Form_pg_class 为参数。
 *		用于不想打开关系、而必须直接
 *		搜索 pg_class 的场合。
 */
bool
IsToastClass(Form_pg_class reltuple)
{
	Oid			relnamespace = reltuple->relnamespace;

	return IsToastNamespace(relnamespace);
}

/*
 * IsCatalogNamespace
 *		当且仅当命名空间是 pg_catalog 时为真。
 *
 *		不做任何目录访问。
 *
 * NOTE: 不做成宏的原因是避免在许多地方
 * 包含 catalog/pg_namespace.h。
 */
bool
IsCatalogNamespace(Oid namespaceId)
{
	return namespaceId == PG_CATALOG_NAMESPACE;
}

/*
 * IsToastNamespace
 *		当且仅当命名空间是 pg_toast 或我的临时 toast 表命名空间时为真。
 *
 *		不做任何目录访问。
 *
 * Note: 对属于其他后端的临时 toast 表命名空间，本函数返回
 * false。它们与其他后端的普通临时表命名空间同等对待，
 * 并在适当处阻止访问。
 * 若需要检查那些命名空间，或许可用 isAnyTempNamespace，
 * 但请注意那会涉及目录访问。
 */
bool
IsToastNamespace(Oid namespaceId)
{
	return (namespaceId == PG_TOAST_NAMESPACE) ||
		isTempToastNamespace(namespaceId);
}


/*
 * IsReservedName
 *		当且仅当名称以 pg_ 前缀开头时为真。
 *
 *		对某些对象类别，前缀 pg_ 保留给
 *		系统对象专用。截至 8.0，这仅适用于
 *		模式名与表空间名。自 9.6 起，角色名
 *		亦如此。
 */
bool
IsReservedName(const char *name)
{
	/* 为速度而采用的难看写法 */
	return (name[0] == 'p' &&
			name[1] == 'g' &&
			name[2] == '_');
}


/*
 * IsSharedRelation
 *		给定关系的 OID，判断它是否应在整个
 *		数据库集簇中共享。
 *
 * 在较旧的发行版中，这必须硬编码，以便我们能在检查
 * 其目录条目之前计算关系的 locktag 并加锁。
 * 现在已有 MVCC 目录访问，使那成为硬性要求的竞态
 * 条件已不存在，因此可考虑放宽这一限制。
 * 然而，若我们先扫描 pg_class 条目以查找 relisshared，再
 * 锁定关系，期间 pg_class 可能被更新，
 * 迫使我们再次扫描该关系，这肯定很复杂，
 * 也可能带来不良的性能后果。幸好，共享
 * 关系集合相当稳定，因此手工维护一份
 * OIDs 列表并非完全不切实际。
 */
bool
IsSharedRelation(Oid relationId)
{
	/* 这些是共享目录（查找 BKI_SHARED_RELATION） */
	if (relationId == AuthIdRelationId ||
		relationId == AuthMemRelationId ||
		relationId == DatabaseRelationId ||
		relationId == DbRoleSettingRelationId ||
		relationId == ParameterAclRelationId ||
		relationId == ReplicationOriginRelationId ||
		relationId == SharedDependRelationId ||
		relationId == SharedDescriptionRelationId ||
		relationId == SharedSecLabelRelationId ||
		relationId == SubscriptionRelationId ||
		relationId == TableSpaceRelationId)
		return true;
	/* 这些是它们的索引 */
	if (relationId == AuthIdOidIndexId ||
		relationId == AuthIdRolnameIndexId ||
		relationId == AuthMemMemRoleIndexId ||
		relationId == AuthMemRoleMemIndexId ||
		relationId == AuthMemOidIndexId ||
		relationId == AuthMemGrantorIndexId ||
		relationId == DatabaseNameIndexId ||
		relationId == DatabaseOidIndexId ||
		relationId == DbRoleSettingDatidRolidIndexId ||
		relationId == ParameterAclOidIndexId ||
		relationId == ParameterAclParnameIndexId ||
		relationId == ReplicationOriginIdentIndex ||
		relationId == ReplicationOriginNameIndex ||
		relationId == SharedDependDependerIndexId ||
		relationId == SharedDependReferenceIndexId ||
		relationId == SharedDescriptionObjIndexId ||
		relationId == SharedSecLabelObjectIndexId ||
		relationId == SubscriptionNameIndexId ||
		relationId == SubscriptionObjectIndexId ||
		relationId == TablespaceNameIndexId ||
		relationId == TablespaceOidIndexId)
		return true;
	/* 这些是它们的 toast 表与 toast 索引 */
	if (relationId == PgAuthidToastTable ||
		relationId == PgAuthidToastIndex ||
		relationId == PgDatabaseToastTable ||
		relationId == PgDatabaseToastIndex ||
		relationId == PgDbRoleSettingToastTable ||
		relationId == PgDbRoleSettingToastIndex ||
		relationId == PgParameterAclToastTable ||
		relationId == PgParameterAclToastIndex ||
		relationId == PgReplicationOriginToastTable ||
		relationId == PgReplicationOriginToastIndex ||
		relationId == PgShdescriptionToastTable ||
		relationId == PgShdescriptionToastIndex ||
		relationId == PgShseclabelToastTable ||
		relationId == PgShseclabelToastIndex ||
		relationId == PgSubscriptionToastTable ||
		relationId == PgSubscriptionToastIndex ||
		relationId == PgTablespaceToastTable ||
		relationId == PgTablespaceToastIndex)
		return true;
	return false;
}

/*
 * IsPinnedObject
 *		给定数据库对象的 class + OID 身份，报告它是否
 *		被"钉住"，即因系统需要而不可删除。
 *
 * 我们曾在 pg_depend 中显式表示这一点，但那被证明是
 * 不可取的开销，因此现在依赖 OID 范围测试。
 */
bool
IsPinnedObject(Oid classId, Oid objectId)
{
	/*
	 * OIDs 高于 FirstUnpinnedObjectId 的对象从不会被钉住。由于
	 * OID 生成器在回绕时会跳过该范围，此检查
	 * 保证用户定义对象绝不会被视为钉住。
	 */
	if (objectId >= FirstUnpinnedObjectId)
		return false;

	/*
	 * 大对象从不会被钉住。需要这一特例是因为
	 * 它们的 OIDs 可由用户指定。
	 */
	if (classId == LargeObjectRelationId)
		return false;

	/*
	 * 目录 .dat 文件中定义的少数对象，作为策略
	 * 我们宁愿不把它们当作钉住。我们曾通过把它们
	 * 排除在 pg_depend 外来处理，但在这里硬编码其
	 * OIDs 同样容易。（若用户确实删除并重建它们，
	 * 它们会得到新的、肯定未钉住的 OIDs，因此没问题。）
	 *
	 * 同时检查 classId 与 objectId 其实过头，因为低于
	 * FirstGenbkiObjectId 的 OIDs 应全局唯一，但为健壮性
	 * 仍这样做。
	 */

	/* public 命名空间未被钉住 */
	if (classId == NamespaceRelationId &&
		objectId == PG_PUBLIC_NAMESPACE)
		return false;

	/*
	 * 数据库从不会被钉住。或许钉住至少 template0 显得稳妥；
	 * 但我们有意如此，以便 template0 与
	 * template1 可以互相重建，从而互为
	 * 备份（只要你没改过 template1）。
	 */
	if (classId == DatabaseRelationId)
		return false;

	/*
	 * 所有其他由 initdb 创建的对象都被钉住。这有点过头（系统
	 * 并非真的依赖每一个古怪的数据类型都存在，例如）
	 * 但只生成所需的最小依赖集似乎很难，而强制维护一份
	 * 精确列表会比这里用的简单范围测试
	 * 昂贵得多。
	 */
	return true;
}


/*
 * GetNewOidWithIndex
 *		生成在该系统关系内唯一的新 OID。
 *
 * 由于 OID 不会立即插入表中，这里存在
 * 竞态；但只有当别人设法绕完 2^32 个
 * OIDs 并在我们插入行之前生成相同 OID 时才会
 * 出问题。这似乎不太可能成为问题。注意：若必须
 * *提交* 该行才能结束竞态，风险会高得多；
 * 因此测试中使用 SnapshotAny，以便能看到
 * 未提交的行。（我们曾用 SnapshotDirty，但它
 * 的缺点是忽略最近删除的行，从而在我们自己的
 * MVCC 快照仍认为最近删除的行存活期间造成
 * 瞬时冲突风险。在选择 TOAST OIDs 时风险更高，
 * 因为 SnapshotToast 会无限期把死行视为活动。）
 *
 * 注意我们实际上假定该表条目相对较少
 * （远小于 2^32），且不存在很长的连续
 * 已有 OIDs 段。对系统目录而言，这一假定
 * 大多合理。
 *
 * 调用方必须对该关系持有合适的锁。
 */
Oid
GetNewOidWithIndex(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	Oid			newOid;
	SysScanDesc scan;
	ScanKeyData key;
	bool		collides;
	uint64		retries = 0;
	uint64		retries_before_log = GETNEWOID_LOG_THRESHOLD;

	/* 仅支持系统关系 */
	Assert(IsSystemRelation(relation));

	/* 引导模式下没有可用的索引 */
	if (IsBootstrapProcessingMode())
		return GetNewObjectId();

	/*
	 * 在 pg_upgrade 期间绝不应被要求生成新的 pg_type OID；
	 * 否则会有与它想分配的 OIDs 冲突的风险。
	 * 触发此断言意味着某条路径未能确保类型 OID
	 * 由转储脚本中的命令决定。
	 */
	Assert(!IsBinaryUpgrade || RelationGetRelid(relation) != TypeRelationId);

	/* 不断生成新 OIDs，直到找到表中未使用的 */
	do
	{
		CHECK_FOR_INTERRUPTS();

		newOid = GetNewObjectId();

		ScanKeyInit(&key,
					oidcolumn,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(newOid));

		/* 见上文关于使用 SnapshotAny 的说明 */
		scan = systable_beginscan(relation, indexId, true,
								  SnapshotAny, 1, &key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		/*
		 * 若已迭代超过 GETNEWOID_LOG_THRESHOLD 仍未
		 * 找到关系中未用的 OID，则记日志。然后以
		 * 指数增大的间隔重复记日志，直到迭代超过
		 * GETNEWOID_LOG_MAX_INTERVAL。最后除非找到未用
		 * OID，否则每隔 GETNEWOID_LOG_MAX_INTERVAL 再记一次。
		 * 此逻辑是为了避免服务器日志被类似消息
		 * 填满。
		 */
		if (retries >= retries_before_log)
		{
			ereport(LOG,
					(errmsg("still searching for an unused OID in relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail_plural("OID candidates have been checked %llu time, but no unused OID has been found yet.",
									  "OID candidates have been checked %llu times, but no unused OID has been found yet.",
									  retries,
									  (unsigned long long) retries)));

			/*
			 * 将下次记日志前的重试次数加倍，直到达到
			 * GETNEWOID_LOG_MAX_INTERVAL。
			 */
			if (retries_before_log * 2 <= GETNEWOID_LOG_MAX_INTERVAL)
				retries_before_log *= 2;
			else
				retries_before_log += GETNEWOID_LOG_MAX_INTERVAL;
		}

		retries++;
	} while (collides);

	/*
	 * 若至少发出过一条日志消息，也记录 OID 分配
	 * 完成。
	 */
	if (retries > GETNEWOID_LOG_THRESHOLD)
	{
		ereport(LOG,
				(errmsg_plural("new OID has been assigned in relation \"%s\" after %llu retry",
							   "new OID has been assigned in relation \"%s\" after %llu retries",
							   retries,
							   RelationGetRelationName(relation), (unsigned long long) retries)));
	}

	return newOid;
}

/*
 * GetNewRelFileNumber
 *		生成在给定表空间所属数据库内唯一的
 *		新 relfilenumber。
 *
 * 若该 relfilenumber 也将用作关系的 OID，则传入已打开的
 * pg_class 目录，本例程将保证结果在 pg_class 中
 * 也是未使用的 OID。若结果仅用作已有关系的
 * relfilenumber，则对 pg_class 传 NULL。
 *
 * 与 GetNewOidWithIndex() 一样，理论上存在竞态
 * 风险，但似乎不值得担心。
 *
 * Note: 我们不支持在引导模式下使用本函数。引导创建的
 * 所有关系都有预分配的 OIDs，因此无需。
 */
RelFileNumber
GetNewRelFileNumber(Oid reltablespace, Relation pg_class, char relpersistence)
{
	RelFileLocatorBackend rlocator;
	char	   *rpath;
	bool		collides;
	BackendId	backend;

	/*
	 * 若在 pg_upgrade 期间走到这里，说明出了问题；二进制升级
	 * 运行中的所有 relfilenumber 分配都应由转储脚本中的
	 * 命令决定。
	 */
	Assert(!IsBinaryUpgrade);

	switch (relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			backend = BackendIdForTempRelations();
			break;
		case RELPERSISTENCE_UNLOGGED:
		case RELPERSISTENCE_PERMANENT:
			backend = InvalidBackendId;
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relpersistence);
			return InvalidRelFileNumber;	/* 安抚编译器 */
	}

	/* 此逻辑应与 RelationInitPhysicalAddr 匹配 */
	rlocator.locator.spcOid = reltablespace ? reltablespace : MyDatabaseTableSpace;
	rlocator.locator.dbOid =
		(rlocator.locator.spcOid == GLOBALTABLESPACE_OID) ?
		InvalidOid : MyDatabaseId;

	/*
	 * relpath 会随后端 ID 而变，因此必须在此正确
	 * 初始化它，以确保基于文件名的任何冲突
	 * 都能被正确检测到。
	 */
	rlocator.backend = backend;

	do
	{
		CHECK_FOR_INTERRUPTS();

		/* 生成 OID */
		if (pg_class)
			rlocator.locator.relNumber = GetNewOidWithIndex(pg_class, ClassOidIndexId,
															Anum_pg_class_oid);
		else
			rlocator.locator.relNumber = GetNewObjectId();

		/* 检查是否已有同名文件 */
		rpath = relpath(rlocator, MAIN_FORKNUM);

		if (access(rpath, F_OK) == 0)
		{
			/* 确定发生冲突 */
			collides = true;
		}
		else
		{
			/*
			 * 这里有点两难：若 errno 不是 ENOENT，是否应
			 * 宣告冲突并循环？实践中似乎最好无论
			 * errno 为何都继续。若确有冲突文件，我们在
			 * 尝试创建新关系文件时会得到
			 * smgr 失败。
			 */
			collides = false;
		}

		pfree(rpath);
	} while (collides);

	return rlocator.locator.relNumber;
}

/*
 * GetNewOidWithIndex() 的 SQL 可调用接口。除 initdb 直接
 * 插入目录表，以及从损坏中恢复外，
 * 很少需要。
 *
 * 有意不在面向用户的文档中记载本函数。
 */
Datum
pg_nextoid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	Name		attname = PG_GETARG_NAME(1);
	Oid			idxoid = PG_GETARG_OID(2);
	Relation	rel;
	Relation	idx;
	HeapTuple	atttuple;
	Form_pg_attribute attform;
	AttrNumber	attno;
	Oid			newoid;

	/*
	 * 由于本函数不打算在正常运行中使用，且仅支持
	 * 系统目录（修改它们需要超级用户权限），
	 * 只检查超级用户应不至于妨碍合法
	 * 用例。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call %s()",
						"pg_nextoid")));

	rel = table_open(reloid, RowExclusiveLock);
	idx = index_open(idxoid, RowExclusiveLock);

	if (!IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_nextoid() can only be used on system catalogs")));

	if (idx->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("index \"%s\" does not belong to table \"%s\"",
						RelationGetRelationName(idx),
						RelationGetRelationName(rel))));

	atttuple = SearchSysCacheAttName(reloid, NameStr(*attname));
	if (!HeapTupleIsValid(atttuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						NameStr(*attname), RelationGetRelationName(rel))));

	attform = ((Form_pg_attribute) GETSTRUCT(atttuple));
	attno = attform->attnum;

	if (attform->atttypid != OIDOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column \"%s\" is not of type oid",
						NameStr(*attname))));

	if (IndexRelationGetNumberOfKeyAttributes(idx) != 1 ||
		idx->rd_index->indkey.values[0] != attno)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("index \"%s\" is not the index for column \"%s\"",
						RelationGetRelationName(idx),
						NameStr(*attname))));

	newoid = GetNewOidWithIndex(rel, idxoid, attno);

	ReleaseSysCache(atttuple);
	table_close(rel, RowExclusiveLock);
	index_close(idx, RowExclusiveLock);

	PG_RETURN_OID(newoid);
}

/*
 * StopGeneratingPinnedObjectIds() 的 SQL 可调用接口。
 *
 * 仅供 initdb 使用，因此有意不在面向用户的
 * 文档中记载。
 */
Datum
pg_stop_making_pinned_objects(PG_FUNCTION_ARGS)
{
	/*
	 * 双保险检查，因为 StopGeneratingPinnedObjectIds 在
	 * 非单用户模式下本来就会失败。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call %s()",
						"pg_stop_making_pinned_objects")));

	StopGeneratingPinnedObjectIds();

	PG_RETURN_VOID();
}
