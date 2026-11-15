/*-------------------------------------------------------------------------
 *
 * relcache.c
 *	  POSTGRES 关系描述符缓存代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/relcache.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程
 *		RelationCacheInitialize			- 初始化 relcache（为空）
 *		RelationCacheInitializePhase2	- 初始化共享目录的条目
 *		RelationCacheInitializePhase3	- 完成 relcache 的初始化
 *		RelationIdGetRelation		- 按 OID 取关系描述符
 *		RelationClose				- 关闭已打开关系
 *
 * 注
 *		以下代码包含许多未载入文档的手法，请务必
 *		小心。
 */
#include "postgres.h"

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/schemapg.h"
#include "catalog/storage.h"
#include "commands/policy.h"
#include "commands/publicationcmds.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rowsecurity.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"
#include "utils/resowner_private.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define RELCACHE_INIT_FILEMAGIC		0x573266	/* 版本 ID 值 */

/*
 * 是否需要检查关系缓存内存是否需要被急切释放。
 * 另参见 RelationBuildDesc() 与 pg_config_manual.h。
 */
#if defined(RECOVER_RELATION_BUILD_MEMORY) && (RECOVER_RELATION_BUILD_MEMORY != 0)
#define MAYBE_RECOVER_RELATION_BUILD_MEMORY 1
#else
#define RECOVER_RELATION_BUILD_MEMORY 0
#ifdef DISCARD_CACHES_ENABLED
#define MAYBE_RECOVER_RELATION_BUILD_MEMORY 1
#endif
#endif

/*
 *		硬编码的元组描述符，内容由 genbki.pl 生成
 */
static const FormData_pg_attribute Desc_pg_class[Natts_pg_class] = {Schema_pg_class};
static const FormData_pg_attribute Desc_pg_attribute[Natts_pg_attribute] = {Schema_pg_attribute};
static const FormData_pg_attribute Desc_pg_proc[Natts_pg_proc] = {Schema_pg_proc};
static const FormData_pg_attribute Desc_pg_type[Natts_pg_type] = {Schema_pg_type};
static const FormData_pg_attribute Desc_pg_database[Natts_pg_database] = {Schema_pg_database};
static const FormData_pg_attribute Desc_pg_authid[Natts_pg_authid] = {Schema_pg_authid};
static const FormData_pg_attribute Desc_pg_auth_members[Natts_pg_auth_members] = {Schema_pg_auth_members};
static const FormData_pg_attribute Desc_pg_index[Natts_pg_index] = {Schema_pg_index};
static const FormData_pg_attribute Desc_pg_shseclabel[Natts_pg_shseclabel] = {Schema_pg_shseclabel};
static const FormData_pg_attribute Desc_pg_subscription[Natts_pg_subscription] = {Schema_pg_subscription};

/*
 *		索引关系缓存的哈希表
 *
 *		过去我们同时按名称与 OID 索引缓存，但现在
 *		仅按 OID 索引。
 */
typedef struct relidcacheent
{
	Oid			reloid;
	Relation	reldesc;
} RelIdCacheEnt;

static HTAB *RelationIdCache;

/*
 * 本标志为假，直到我们备好 relcache 构建所读的表执行
 * 索引扫描所需的关键 relcache 条目。
 */
bool		criticalRelcachesBuilt = false;

/*
 * 本标志为假，直至为共享目录备好关键 relcache 条目
 * （这些表即登录所需的表）。
 */
bool		criticalSharedRelcachesBuilt = false;

/*
 * 此计数器统计自后端启动以来收到的 relcache 失效事件
 * （但仅限实际已入缓存的关系）。目前只用它检测
 * write_relcache_init_file() 即将写出的数据是否可能
 * 已经过时。
 */
static long relcacheInvalsReceived = 0L;

/*
 * in_progress_list 是一个由进行中的 RelationBuildDesc() 调用
 * 组成的栈。CREATE INDEX CONCURRENTLY 在
 * ShareUpdateExclusiveLock 下做目录变更。它极其依赖每个
 * 后端最迟在下个事务开始时吸收这些变更。因此，
 * RelationBuildDesc() 会循环直至完成，且期间未接受相关
 * 失效。
 */
typedef struct inprogressent
{
	Oid			reloid;			/* 所构建关系的 OID */
	bool		invalidated;	/* 是否已有其失效事件到达 */
} InProgressEnt;

static InProgressEnt *in_progress_list;
static int	in_progress_list_len;
static int	in_progress_list_maxlen;

/*
 * eoxact_list[] 存储那些（可能）需要 AtEOXact 清理工作的
 * 关系 OID。该列表有意限制大小；一旦溢出，
 * 退回扫描整个哈希表。列表过于庞大并无价值，因为
 * （1）在某些时候 hash_seq_search 扫描比逐个查找更快；且
 * （2）其价值在于减少 EOXact 工作量——
 * 反正短事务本来也脏不了那么多表。
 * EOXactListAdd() 不会防止列表条目重复，因此清理处理
 * 必须是幂等的。
 */
#define MAX_EOXACT_LIST 32
static Oid	eoxact_list[MAX_EOXACT_LIST];
static int	eoxact_list_len = 0;
static bool eoxact_list_overflowed = false;

#define EOXactListAdd(rel) \
	do { \
		if (eoxact_list_len < MAX_EOXACT_LIST) \
			eoxact_list[eoxact_list_len++] = (rel)->rd_id; \
		else \
			eoxact_list_overflowed = true; \
	} while (0)

/*
 * EOXactTupleDescArray 存储（可能）需要 AtEOXact
 * 清理工作的 TupleDesc。数组按需扩展；无哈希表，因
 * EOXact 之外无需访问单个元素。
 */
static TupleDesc *EOXactTupleDescArray;
static int	NextEOXactTupleDescNum = 0;
static int	EOXactTupleDescArrayLen = 0;

/*
 *		操纵查找哈希表的宏
 */
#define RelationCacheInsert(RELATION, replace_allowed)	\
do { \
	RelIdCacheEnt *hentry; bool found; \
	hentry = (RelIdCacheEnt *) hash_search(RelationIdCache, \
										   &((RELATION)->rd_id), \
										   HASH_ENTER, &found); \
	if (found) \
	{ \
		/* 参见 RelationBuildDesc 与 RelationBuildLocalRelation 注释 */ \
		Relation _old_rel = hentry->reldesc; \
		Assert(replace_allowed); \
		hentry->reldesc = (RELATION); \
		if (RelationHasReferenceCountZero(_old_rel)) \
			RelationDestroyRelation(_old_rel, false); \
		else if (!IsBootstrapProcessingMode()) \
			elog(WARNING, "leaking still-referenced relcache entry for \"%s\"", \
				 RelationGetRelationName(_old_rel)); \
	} \
	else \
		hentry->reldesc = (RELATION); \
} while(0)

#define RelationIdCacheLookup(ID, RELATION) \
do { \
	RelIdCacheEnt *hentry; \
	hentry = (RelIdCacheEnt *) hash_search(RelationIdCache, \
										   &(ID), \
										   HASH_FIND, NULL); \
	if (hentry) \
		RELATION = hentry->reldesc; \
	else \
		RELATION = NULL; \
} while(0)

#define RelationCacheDelete(RELATION) \
do { \
	RelIdCacheEnt *hentry; \
	hentry = (RelIdCacheEnt *) hash_search(RelationIdCache, \
										   &((RELATION)->rd_id), \
										   HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "failed to delete relcache entry for OID %u", \
			 (RELATION)->rd_id); \
} while(0)


/*
 * opclass 相关信息的专用缓存
 *
 * 注意：仅缓存默认支持过程，即那些
 * lefttype = righttype = opcintype.
 */
typedef struct opclasscacheent
{
	Oid			opclassoid;		/* 查找键：opclass OID */
	bool		valid;			/* 成功填充后设为真 */
	StrategyNumber numSupport;	/* 支持过程数上限（pg_am） */
	Oid			opcfamily;		/* opclass 家族的 OID */
	Oid			opcintype;		/* opclass 声明的输入类型 OID */
	RegProcedure *supportProcs; /* 支持过程的 OID */
} OpClassCacheEnt;

static HTAB *OpClassCache = NULL;


/* 非导出函数的原型 */

static void RelationDestroyRelation(Relation relation, bool remember_tupdesc);
static void RelationClearRelation(Relation relation, bool rebuild);

static void RelationReloadIndexInfo(Relation relation);
static void RelationReloadNailed(Relation relation);
static void RelationFlushRelation(Relation relation);
static void RememberToFreeTupleDescAtEOX(TupleDesc td);
#ifdef USE_ASSERT_CHECKING
static void AssertPendingSyncConsistency(Relation relation);
#endif
static void AtEOXact_cleanup(Relation relation, bool isCommit);
static void AtEOSubXact_cleanup(Relation relation, bool isCommit,
								SubTransactionId mySubid, SubTransactionId parentSubid);
static bool load_relcache_init_file(bool shared);
static void write_relcache_init_file(bool shared);
static void write_item(const void *data, Size len, FILE *fp);

static void formrdesc(const char *relationName, Oid relationReltype,
					  bool isshared, int natts, const FormData_pg_attribute *attrs);

static HeapTuple ScanPgRelation(Oid targetRelId, bool indexOK, bool force_non_historic);
static Relation AllocateRelationDesc(Form_pg_class relp);
static void RelationParseRelOptions(Relation relation, HeapTuple tuple);
static void RelationBuildTupleDesc(Relation relation);
static Relation RelationBuildDesc(Oid targetRelId, bool insertIt);
static void RelationInitPhysicalAddr(Relation relation);
static void load_critical_index(Oid indexoid, Oid heapoid);
static TupleDesc GetPgClassDescriptor(void);
static TupleDesc GetPgIndexDescriptor(void);
static void AttrDefaultFetch(Relation relation, int ndef);
static int	AttrDefaultCmp(const void *a, const void *b);
static void CheckConstraintFetch(Relation relation);
static int	CheckConstraintCmp(const void *a, const void *b);
static void InitIndexAmRoutine(Relation relation);
static void IndexSupportInitialize(oidvector *indclass,
								   RegProcedure *indexSupport,
								   Oid *opFamily,
								   Oid *opcInType,
								   StrategyNumber maxSupportNumber,
								   AttrNumber maxAttributeNumber);
static OpClassCacheEnt *LookupOpclassInfo(Oid operatorClassOid,
										  StrategyNumber numSupport);
static void RelationCacheInitFileRemoveInDir(const char *tblspcpath);
static void unlink_initfile(const char *initfilename, int elevel);


/*
 *		ScanPgRelation
 *
 *		RelationBuildDesc 用它查找匹配 targetRelId 的
 *		pg_class 元组。调用方至少须持有目标 relid 的
 *		AccessShareLock，以防并发更新场景；不保证构建
 *		relcache 条目的所有扫描都使用同一快照。
 *		若在扫描 pg_class 之后、扫描 pg_attribute 之前
 *		有属性被加入，
 *		relnatts 就会不匹配。
 *
 *		注意：返回元组已复制到 palloc 存储，最终须
 *		用 heap_freetuple 释放。
 */
static HeapTuple
ScanPgRelation(Oid targetRelId, bool indexOK, bool force_non_historic)
{
	HeapTuple	pg_class_tuple;
	Relation	pg_class_desc;
	SysScanDesc pg_class_scan;
	ScanKeyData key[1];
	Snapshot	snapshot = NULL;

	/*
	 * 若后端启动时出错，可能会在尚未选库前就试图读取
	 * pg_class——这行不通，带着有用的错误消息退出。
	 * 若发生这种情况，多半意味着某个该被钉住的 relcache
	 * 条目实际上并未被钉住。
	 */
	if (!OidIsValid(MyDatabaseId))
		elog(FATAL, "cannot read pg_class without having selected a database");

	/*
	 * 构成扫描键
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(targetRelId));

	/*
	 * 打开 pg_class 并取回元组。若尚未构建关键 relcache
	 * 条目（包括 initdb 以及缺少 pg_internal.init 文件的
	 * 启动）。调用方亦可设置 indexOK == false 来强制
	 * 进行堆扫描。
	 */
	pg_class_desc = table_open(RelationRelationId, AccessShareLock);

	/*
	 * 调用方可能需要比历史快照所见更新的元组；
	 * 唯一需要这么做的场景是解码期间查找未映射系统的
	 * relfilenumber。该快照在 relcache 构建期间不会改变，
	 * 因此无需
	 * 注册该快照。
	 */
	if (force_non_historic)
		snapshot = GetNonHistoricCatalogSnapshot(RelationRelationId);

	pg_class_scan = systable_beginscan(pg_class_desc, ClassOidIndexId,
									   indexOK && criticalRelcachesBuilt,
									   snapshot,
									   1, key);

	pg_class_tuple = systable_getnext(pg_class_scan);

	/*
	 * 释放缓冲区前必须复制元组。
	 */
	if (HeapTupleIsValid(pg_class_tuple))
		pg_class_tuple = heap_copytuple(pg_class_tuple);

	/* 完成 */
	systable_endscan(pg_class_scan);
	table_close(pg_class_desc, AccessShareLock);

	return pg_class_tuple;
}

/*
 *		AllocateRelationDesc
 *
 *		分配新关系描述符的内存，并从给定 pg_class
 *		元组初始化 rd_rel 字段。
 */
static Relation
AllocateRelationDesc(Form_pg_class relp)
{
	Relation	relation;
	MemoryContext oldcxt;
	Form_pg_class relationForm;

	/* relcache 条目必须位于 CacheMemoryContext */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 为新关系描述符分配空间并清零
	 */
	relation = (Relation) palloc0(sizeof(RelationData));

	/* 确保关系被标记为尚无已打开文件 */
	relation->rd_smgr = NULL;

	/*
	 * 复制关系元组形式
	 *
	 * 我们只为固定字段分配空间，即 CLASS_TUPLE_SIZE。变长
	 * 字段（relacl、reloptions）不会存储在 relcache 中——
	 * 意义不大，因为不复制空值位图，便无从知晓其值
	 * 是否有效。
	 * 结论就是：relacl 无法从 relcache 中获取。如需它，
	 * 请从 syscache 获取。reloptions 的原始形式同理，但
	 * 我们确实会把解析后的形式存入
	 * rd_options。
	 */
	relationForm = (Form_pg_class) palloc(CLASS_TUPLE_SIZE);

	memcpy(relationForm, relp, CLASS_TUPLE_SIZE);

	/* 初始化关系元组形式 */
	relation->rd_rel = relationForm;

	/* 并分配属性元组形式存储 */
	relation->rd_att = CreateTemplateTupleDesc(relationForm->relnatts);
	/* 并标记为引用计数的元组描述符 */
	relation->rd_att->tdrefcount = 1;

	MemoryContextSwitchTo(oldcxt);

	return relation;
}

/*
 * RelationParseRelOptions
 *		把 pg_class.reloptions 转为预解析的 rd_options
 *
 * tuple 是关系的真实 pg_class 元组（非 rd_rel！）
 *
 * 注意：rd_rel（索引则含 rd_indam）必须已经有效
 */
static void
RelationParseRelOptions(Relation relation, HeapTuple tuple)
{
	bytea	   *options;
	amoptions_function amoptsfn;

	relation->rd_options = NULL;

	/*
	 * 查找 AM 专属解析函数；若 relkind 不应有选项，
	 * 则退出。
	 */
	switch (relation->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_PARTITIONED_TABLE:
			amoptsfn = NULL;
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			amoptsfn = relation->rd_indam->amoptions;
			break;
		default:
			return;
	}

	/*
	 * 从元组取 reloptions；必须用硬编码描述符，因为此时
	 * 可能还没有 pg_class 可用的描述符（考虑对 pg_class
	 * 本身执行此代码）
	 */
	options = extractRelOptions(tuple, GetPgClassDescriptor(), amoptsfn);

	/*
	 * 将解析后的数据复制进 CacheMemoryContext。为防
	 * reloptions 代码可能泄漏，故在调用方内存上下文中
	 * 进行，然后再把结果复制进
	 * CacheMemoryContext。
	 */
	if (options)
	{
		relation->rd_options = MemoryContextAlloc(CacheMemoryContext,
												  VARSIZE(options));
		memcpy(relation->rd_options, options, VARSIZE(options));
		pfree(options);
	}
}

/*
 *		RelationBuildTupleDesc
 *
 *		依据 pg_attribute、pg_attrdef 和 pg_constraint
 *		系统目录的信息来构建关系的元组描述符。
 */
static void
RelationBuildTupleDesc(Relation relation)
{
	HeapTuple	pg_attribute_tuple;
	Relation	pg_attribute_desc;
	SysScanDesc pg_attribute_scan;
	ScanKeyData skey[2];
	int			need;
	TupleConstr *constr;
	AttrMissing *attrmiss = NULL;
	int			ndef = 0;

	/* 填充 rd_att 的类型 ID 字段（对比 AddNewRelationTuple） */
	relation->rd_att->tdtypeid =
		relation->rd_rel->reltype ? relation->rd_rel->reltype : RECORDOID;
	relation->rd_att->tdtypmod = -1;	/* 保险起见 */

	constr = (TupleConstr *) MemoryContextAllocZero(CacheMemoryContext,
													sizeof(TupleConstr));
	constr->has_not_null = false;
	constr->has_generated_stored = false;

	/*
	 * 构建仅选择用户属性的扫描键（attnum > 0）。
	 * （在索引层剔除系统属性行，远快于取回它们）
	 * 
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));
	ScanKeyInit(&skey[1],
				Anum_pg_attribute_attnum,
				BTGreaterStrategyNumber, F_INT2GT,
				Int16GetDatum(0));

	/*
	 * 打开 pg_attribute 并开始扫描。若尚未构建关键 relcache
	 * 条目（包括 initdb 以及缺少 pg_internal.init 文件的
	 * 启动）。
	 */
	pg_attribute_desc = table_open(AttributeRelationId, AccessShareLock);
	pg_attribute_scan = systable_beginscan(pg_attribute_desc,
										   AttributeRelidNumIndexId,
										   criticalRelcachesBuilt,
										   NULL,
										   2, skey);

	/*
	 * 属性数据添入 relation->rd_att
	 */
	need = RelationGetNumberOfAttributes(relation);

	while (HeapTupleIsValid(pg_attribute_tuple = systable_getnext(pg_attribute_scan)))
	{
		Form_pg_attribute attp;
		int			attnum;

		attp = (Form_pg_attribute) GETSTRUCT(pg_attribute_tuple);

		attnum = attp->attnum;
		if (attnum <= 0 || attnum > RelationGetNumberOfAttributes(relation))
			elog(ERROR, "invalid attribute number %d for relation \"%s\"",
				 attp->attnum, RelationGetRelationName(relation));

		memcpy(TupleDescAttr(relation->rd_att, attnum - 1),
			   attp,
			   ATTRIBUTE_FIXED_PART_SIZE);

		/* 更新约束/默认值信息 */
		if (attp->attnotnull)
			constr->has_not_null = true;
		if (attp->attgenerated == ATTRIBUTE_GENERATED_STORED)
			constr->has_generated_stored = true;
		if (attp->atthasdef)
			ndef++;

		/* 如果该列有"missing"值，则将其放入 attrmiss 数组 */
		if (attp->atthasmissing)
		{
			Datum		missingval;
			bool		missingNull;

			/* 有缺失值吗？ */
			missingval = heap_getattr(pg_attribute_tuple,
									  Anum_pg_attribute_attmissingval,
									  pg_attribute_desc->rd_att,
									  &missingNull);
			if (!missingNull)
			{
				/* 有，从数组中取值 */
				MemoryContext oldcxt;
				bool		is_null;
				int			one = 1;
				Datum		missval;

				if (attrmiss == NULL)
					attrmiss = (AttrMissing *)
						MemoryContextAllocZero(CacheMemoryContext,
											   relation->rd_rel->relnatts *
											   sizeof(AttrMissing));

				missval = array_get_element(missingval,
											1,
											&one,
											-1,
											attp->attlen,
											attp->attbyval,
											attp->attalign,
											&is_null);
				Assert(!is_null);
				if (attp->attbyval)
				{
					/* 按值复制时直接复制 datum 本身 */
					attrmiss[attnum - 1].am_value = missval;
				}
				else
				{
					/* 否则在正确的上下文中复制 */
					oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
					attrmiss[attnum - 1].am_value = datumCopy(missval,
															  attp->attbyval,
															  attp->attlen);
					MemoryContextSwitchTo(oldcxt);
				}
				attrmiss[attnum - 1].am_present = true;
			}
		}
		need--;
		if (need == 0)
			break;
	}

	/*
	 * 结束扫描并关闭属性关系
	 */
	systable_endscan(pg_attribute_scan);
	table_close(pg_attribute_desc, AccessShareLock);

	if (need != 0)
		elog(ERROR, "pg_attribute catalog is missing %d attribute(s) for relation OID %u",
			 need, RelationGetRelid(relation));

	/*
	 * 从 pg_attribute 读到的 attcacheoff 值应全为 -1（表示
	 * "未知"）。断言检查开启时验证此点。它们会在需
	 * 要的时候在元组访问期间计算。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < RelationGetNumberOfAttributes(relation); i++)
			Assert(TupleDescAttr(relation->rd_att, i)->attcacheoff == -1);
	}
#endif

	/*
	 * 不过，第一个属性的 attcacheoff 值很容易设置：
	 * 它必为零。这就免除了 fastgetattr() 与 index_getattr() 中
	 * 曾经存在的 attnum=1 特例。
	 */
	if (RelationGetNumberOfAttributes(relation) > 0)
		TupleDescAttr(relation->rd_att, 0)->attcacheoff = 0;

	/*
	 * 设置约束/默认值信息
	 */
	if (constr->has_not_null ||
		constr->has_generated_stored ||
		ndef > 0 ||
		attrmiss ||
		relation->rd_rel->relchecks > 0)
	{
		relation->rd_att->constr = constr;

		if (ndef > 0)			/* 默认 */
			AttrDefaultFetch(relation, ndef);
		else
			constr->num_defval = 0;

		constr->missing = attrmiss;

		if (relation->rd_rel->relchecks > 0)	/* 检查 */
			CheckConstraintFetch(relation);
		else
			constr->num_check = 0;
	}
	else
	{
		pfree(constr);
		relation->rd_att->constr = NULL;
	}
}

/*
 *		RelationBuildRuleLock
 *
 *		依据 pg_rewrite 系统目录中的信息，
 *		构建关系重写规则。
 *
 * 注意：规则的分析树是可能非常复杂的节点结构。
 * 为了让这些树在 relcache 条目被刷写时得以释放，
 * 我们为每个带规则的 relcache 条目建私有内存上下文
 * 来存放 RuleLock 信息。该上下文只用于规则信息，
 * 而不是 relcache 条目的任何其他附属数据，因为这样
 * 能让 RelationClearRelation() 中的更新逻辑保持可管理。
 * 其余附属数据结构足够简单，
 * 反正都容易显式释放。
 *
 * 注意：必须先提取该关系的 reloptions。
 */
static void
RelationBuildRuleLock(Relation relation)
{
	MemoryContext rulescxt;
	MemoryContext oldcxt;
	HeapTuple	rewrite_tuple;
	Relation	rewrite_desc;
	TupleDesc	rewrite_tupdesc;
	SysScanDesc rewrite_scan;
	ScanKeyData key;
	RuleLock   *rulelock;
	int			numlocks;
	RewriteRule **rules;
	int			maxlocks;

	/*
	 * 创建私有上下文。假定它不会包含太多数据。
	 */
	rulescxt = AllocSetContextCreate(CacheMemoryContext,
									 "relation rules",
									 ALLOCSET_SMALL_SIZES);
	relation->rd_rulescxt = rulescxt;
	MemoryContextCopyAndSetIdentifier(rulescxt,
									  RelationGetRelationName(relation));

	/*
	 * 分配一个数组来存放重写规则（必要时会扩展）。
	 * 
	 */
	maxlocks = 4;
	rules = (RewriteRule **)
		MemoryContextAlloc(rulescxt, sizeof(RewriteRule *) * maxlocks);
	numlocks = 0;

	/*
	 * 构成扫描键
	 */
	ScanKeyInit(&key,
				Anum_pg_rewrite_ev_class,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	/*
	 * 打开 pg_rewrite 开始扫描
	 *
	 * 注意：由于我们用 RewriteRelRulenameIndexId 扫描规则，
	 * 规则将按名称顺序读取，紧急恢复期间除外
	 * （即 IgnoreSystemIndexes）。这进而确保规则按名称顺
	 * 序触发。
	 */
	rewrite_desc = table_open(RewriteRelationId, AccessShareLock);
	rewrite_tupdesc = RelationGetDescr(rewrite_desc);
	rewrite_scan = systable_beginscan(rewrite_desc,
									  RewriteRelRulenameIndexId,
									  true, NULL,
									  1, &key);

	while (HeapTupleIsValid(rewrite_tuple = systable_getnext(rewrite_scan)))
	{
		Form_pg_rewrite rewrite_form = (Form_pg_rewrite) GETSTRUCT(rewrite_tuple);
		bool		isnull;
		Datum		rule_datum;
		char	   *rule_str;
		RewriteRule *rule;
		Oid			check_as_user;

		rule = (RewriteRule *) MemoryContextAlloc(rulescxt,
												  sizeof(RewriteRule));

		rule->ruleId = rewrite_form->oid;

		rule->event = rewrite_form->ev_type - '0';
		rule->enabled = rewrite_form->ev_enabled;
		rule->isInstead = rewrite_form->is_instead;

		/*
		 * 必须用 heap_getattr 来取 ev_action 与 ev_qual。此外，
		 * 规则串常大得被 TOAST；为免调用方内存泄
		 * 漏。在此处做解 TOAST，以便
		 * 能释放解出来的版本。
		 */
		rule_datum = heap_getattr(rewrite_tuple,
								  Anum_pg_rewrite_ev_action,
								  rewrite_tupdesc,
								  &isnull);
		Assert(!isnull);
		rule_str = TextDatumGetCString(rule_datum);
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->actions = (List *) stringToNode(rule_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(rule_str);

		rule_datum = heap_getattr(rewrite_tuple,
								  Anum_pg_rewrite_ev_qual,
								  rewrite_tupdesc,
								  &isnull);
		Assert(!isnull);
		rule_str = TextDatumGetCString(rule_datum);
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->qual = (Node *) stringToNode(rule_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(rule_str);

		/*
		 * 如果这是定义视图的 SELECT 规则，而该视图
		 * 设置了"security_invoker"，就必须以调用者身份对
		 * 规则所引用的所有关系做权限检查。
		 *
		 * 其余所有情况（包括 security invoker 视图上的非
		 * SELECT 规则），都以关系所有者身份检查权限。
		 */
		if (rule->event == CMD_SELECT &&
			relation->rd_rel->relkind == RELKIND_VIEW &&
			RelationHasSecurityInvoker(relation))
			check_as_user = InvalidOid;
		else
			check_as_user = relation->rd_rel->relowner;

		/*
		 * 扫描该规则的动作，为其 RTEPermissionInfo 设置
		 * checkAsUser 字段。还须查看 qual，以防其中包含
		 * 子链接。
		 *
		 * 之所以在加载时而非存储时做此事，是因为否则
		 * ALTER TABLE OWNER 就得遍历已存规则来更新
		 * checkAsUser 字段。加载时扫描规则树开销相对较
		 * 小（相比于最初构建），所以就在
		 * 这里执行。
		 */
		setRuleCheckAsUser((Node *) rule->actions, check_as_user);
		setRuleCheckAsUser(rule->qual, check_as_user);

		if (numlocks >= maxlocks)
		{
			maxlocks *= 2;
			rules = (RewriteRule **)
				repalloc(rules, sizeof(RewriteRule *) * maxlocks);
		}
		rules[numlocks++] = rule;
	}

	/*
	 * 结束扫描并关闭属性关系
	 */
	systable_endscan(rewrite_scan);
	table_close(rewrite_desc, AccessShareLock);

	/*
	 * 可能没有任何规则（如果 relhasrules 已过时）
	 */
	if (numlocks == 0)
	{
		relation->rd_rules = NULL;
		relation->rd_rulescxt = NULL;
		MemoryContextDelete(rulescxt);
		return;
	}

	/*
	 * 构成 RuleLock 并插入到关系中
	 */
	rulelock = (RuleLock *) MemoryContextAlloc(rulescxt, sizeof(RuleLock));
	rulelock->numLocks = numlocks;
	rulelock->rules = rules;

	relation->rd_rules = rulelock;
}

/*
 *		equalRuleLocks
 *
 *		判断两个 RuleLock 是否等价
 *
 *		或许本应放在规则代码的某处……
 */
static bool
equalRuleLocks(RuleLock *rlock1, RuleLock *rlock2)
{
	int			i;

	/*
	 * 从 7.3 起我们假定规则顺序是可重现的，因为
	 * RelationBuildRuleLock 应以一致的顺序读取它们。所以只
	 * 需比较对应的槽位。
	 */
	if (rlock1 != NULL)
	{
		if (rlock2 == NULL)
			return false;
		if (rlock1->numLocks != rlock2->numLocks)
			return false;
		for (i = 0; i < rlock1->numLocks; i++)
		{
			RewriteRule *rule1 = rlock1->rules[i];
			RewriteRule *rule2 = rlock2->rules[i];

			if (rule1->ruleId != rule2->ruleId)
				return false;
			if (rule1->event != rule2->event)
				return false;
			if (rule1->enabled != rule2->enabled)
				return false;
			if (rule1->isInstead != rule2->isInstead)
				return false;
			if (!equal(rule1->qual, rule2->qual))
				return false;
			if (!equal(rule1->actions, rule2->actions))
				return false;
		}
	}
	else if (rlock2 != NULL)
		return false;
	return true;
}

/*
 *		equalPolicy
 *
 *		判断两个策略是否等价
 */
static bool
equalPolicy(RowSecurityPolicy *policy1, RowSecurityPolicy *policy2)
{
	int			i;
	Oid		   *r1,
			   *r2;

	if (policy1 != NULL)
	{
		if (policy2 == NULL)
			return false;

		if (policy1->polcmd != policy2->polcmd)
			return false;
		if (policy1->hassublinks != policy2->hassublinks)
			return false;
		if (strcmp(policy1->policy_name, policy2->policy_name) != 0)
			return false;
		if (ARR_DIMS(policy1->roles)[0] != ARR_DIMS(policy2->roles)[0])
			return false;

		r1 = (Oid *) ARR_DATA_PTR(policy1->roles);
		r2 = (Oid *) ARR_DATA_PTR(policy2->roles);

		for (i = 0; i < ARR_DIMS(policy1->roles)[0]; i++)
		{
			if (r1[i] != r2[i])
				return false;
		}

		if (!equal(policy1->qual, policy2->qual))
			return false;
		if (!equal(policy1->with_check_qual, policy2->with_check_qual))
			return false;
	}
	else if (policy2 != NULL)
		return false;

	return true;
}

/*
 *		equalRSDesc
 *
 *		判断两个 RowSecurityDesc 是否等价
 */
static bool
equalRSDesc(RowSecurityDesc *rsdesc1, RowSecurityDesc *rsdesc2)
{
	ListCell   *lc,
			   *rc;

	if (rsdesc1 == NULL && rsdesc2 == NULL)
		return true;

	if ((rsdesc1 != NULL && rsdesc2 == NULL) ||
		(rsdesc1 == NULL && rsdesc2 != NULL))
		return false;

	if (list_length(rsdesc1->policies) != list_length(rsdesc2->policies))
		return false;

	/* RelationBuildRowSecurity 应有序构建策略 */
	forboth(lc, rsdesc1->policies, rc, rsdesc2->policies)
	{
		RowSecurityPolicy *l = (RowSecurityPolicy *) lfirst(lc);
		RowSecurityPolicy *r = (RowSecurityPolicy *) lfirst(rc);

		if (!equalPolicy(l, r))
			return false;
	}

	return true;
}

/*
 *		RelationBuildDesc
 *
 *		构建关系描述符。调用方至少须在目标 relid
 *		上持有 AccessShareLock。
 *
 *		若 insertIt 为真，新描述符会被插入哈希表。
 *
 *		如果找不到给定 relid 的 pg_class 行，则返回 NULL
 *		（暗示我们正试图访问刚删除的关系）。
 *		其他错误经由 elog 报告。
 */
static Relation
RelationBuildDesc(Oid targetRelId, bool insertIt)
{
	int			in_progress_offset;
	Relation	relation;
	Oid			relid;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;

	/*
	 * 本函数及其子程序可能在 CurrentMemoryContext 中分配大
	 * 量临时数据。传统做法是任其泄漏：调用方上下文
	 * 至多在事务作用域，且 relcache 加载不会频繁到必须
	 * 在语句/事务结束前回收临时数据。但当
	 * debug_discard_caches 启用时这一点肯定不成立，
	 * 在其他情况下或许也
	 * 不成立。
	 *
	 * 当 debug_discard_caches 启用，或被
	 * RECOVER_RELATION_BUILD_MEMORY=1 强制时，安排在一个临
	 * 时上下文中分配垃圾数据，返回前释放。将其设为调
	 * 用方上下文的子上下文，这样中途出错时它也能
	 * 得到恰当的清理。
	 */
#ifdef MAYBE_RECOVER_RELATION_BUILD_MEMORY
	MemoryContext tmpcxt = NULL;
	MemoryContext oldcxt = NULL;

	if (RECOVER_RELATION_BUILD_MEMORY || debug_discard_caches > 0)
	{
		tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
									   "RelationBuildDesc workspace",
									   ALLOCSET_DEFAULT_SIZES);
		oldcxt = MemoryContextSwitchTo(tmpcxt);
	}
#endif

	/* 注册以接收失效消息 */
	if (in_progress_list_len >= in_progress_list_maxlen)
	{
		int			allocsize;

		allocsize = in_progress_list_maxlen * 2;
		in_progress_list = repalloc(in_progress_list,
									allocsize * sizeof(*in_progress_list));
		in_progress_list_maxlen = allocsize;
	}
	in_progress_offset = in_progress_list_len++;
	in_progress_list[in_progress_offset].reloid = targetRelId;
retry:
	in_progress_list[in_progress_offset].invalidated = false;

	/*
	 * 在 pg_class 中查找给定关系 id 对应的元组
	 */
	pg_class_tuple = ScanPgRelation(targetRelId, true, false);

	/*
	 * 若无此元组则返回 NULL
	 */
	if (!HeapTupleIsValid(pg_class_tuple))
	{
#ifdef MAYBE_RECOVER_RELATION_BUILD_MEMORY
		if (tmpcxt)
		{
			/* 回到调用方的上下文，并清掉临时上下文 */
			MemoryContextSwitchTo(oldcxt);
			MemoryContextDelete(tmpcxt);
		}
#endif
		Assert(in_progress_offset + 1 == in_progress_list_len);
		in_progress_list_len--;
		return NULL;
	}

	/*
	 * 从 pg_class 元组中获取信息
	 */
	relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	relid = relp->oid;
	Assert(relid == targetRelId);

	/*
	 * 为关系描述符分配存储，并把 pg_class 元组复制到
	 * relation->rd_rel。
	 */
	relation = AllocateRelationDesc(relp);

	/*
	 * 初始化该关系的关系 id（relation->rd_id）
	 */
	RelationGetRelid(relation) = relid;

	/*
	 * 普通关系不会被钉入缓存。因为我们不刷写新关系，
	 * 所以不是新建的。但也可能是临时表。
	 */
	relation->rd_refcnt = 0;
	relation->rd_isnailed = false;
	relation->rd_createSubid = InvalidSubTransactionId;
	relation->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_droppedSubid = InvalidSubTransactionId;
	switch (relation->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_UNLOGGED:
		case RELPERSISTENCE_PERMANENT:
			relation->rd_backend = InvalidBackendId;
			relation->rd_islocaltemp = false;
			break;
		case RELPERSISTENCE_TEMP:
			if (isTempOrTempToastNamespace(relation->rd_rel->relnamespace))
			{
				relation->rd_backend = BackendIdForTempRelations();
				relation->rd_islocaltemp = true;
			}
			else
			{
				/*
				 * 若是别人的临时表，就得用缓慢粗陋的方法
				 * 来确定其所属后端。
				 *
				 * 注意：此处 rd_backend 可能被设为 MyBackendId，
				 * 即我们看到的 pg_class 条目出自一个恰好用了
				 * 同 BackendId 的已崩溃后端，不应把这种表
				 * 视为"我们的"；这需单独的 rd_islocaltemp
				 * 标志。该 pg_class 条目将在
				 * 我们清理对应的临时表命名空间时被刷写，
				 * 该清理是为
				 * 使用它而做的准备。
				 */
				relation->rd_backend =
					GetTempNamespaceBackendId(relation->rd_rel->relnamespace);
				Assert(relation->rd_backend != InvalidBackendId);
				relation->rd_islocaltemp = false;
			}
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c",
				 relation->rd_rel->relpersistence);
			break;
	}

	/*
	 * 初始化元组描述符（relation->rd_att）。
	 */
	RelationBuildTupleDesc(relation);

	/* 外键数据在被请求前不加载 */
	relation->rd_fkeylist = NIL;
	relation->rd_fkeyvalid = false;

	/* 分区数据在被请求前不加载 */
	relation->rd_partkey = NULL;
	relation->rd_partkeycxt = NULL;
	relation->rd_partdesc = NULL;
	relation->rd_partdesc_nodetached = NULL;
	relation->rd_partdesc_nodetached_xmin = InvalidTransactionId;
	relation->rd_pdcxt = NULL;
	relation->rd_pddcxt = NULL;
	relation->rd_partcheck = NIL;
	relation->rd_partcheckvalid = false;
	relation->rd_partcheckcxt = NULL;

	/*
	 * 初始化访问方法信息
	 */
	if (relation->rd_rel->relkind == RELKIND_INDEX ||
		relation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		RelationInitIndexAccessInfo(relation);
	else if (RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind) ||
			 relation->rd_rel->relkind == RELKIND_SEQUENCE)
		RelationInitTableAccessMethod(relation);
	else
		Assert(relation->rd_rel->relam == InvalidOid);

	/* 有 reloptions 则提取 */
	RelationParseRelOptions(relation, pg_class_tuple);

	/*
	 * 取回影响此关系的规则和触发器。
	 *
	 * 注意 RelationBuildRuleLock() 依赖此事在提取关系的
	 * reloptions 之后进行。
	 */
	if (relation->rd_rel->relhasrules)
		RelationBuildRuleLock(relation);
	else
	{
		relation->rd_rules = NULL;
		relation->rd_rulescxt = NULL;
	}

	if (relation->rd_rel->relhastriggers)
		RelationBuildTriggers(relation);
	else
		relation->trigdesc = NULL;

	if (relation->rd_rel->relrowsecurity)
		RelationBuildRowSecurity(relation);
	else
		relation->rd_rsdesc = NULL;

	/*
	 * 初始化关系锁管理器信息
	 */
	RelationInitLockInfo(relation); /* 见lmgr.c */

	/*
	 * 初始化该关系的物理寻址信息
	 */
	RelationInitPhysicalAddr(relation);

	/* 确保关系被标记为尚无已打开文件 */
	relation->rd_smgr = NULL;

	/*
	 * 现在可以释放 pg_class 元组的内存了
	 */
	heap_freetuple(pg_class_tuple);

	/*
	 * 若构建期间收到失效，则推倒重来。从这里直到本函
	 * 束处，不要加入会或很可能会读系统目录的代码。那
	 * 一段区间必须免于失效处理：对 !insertIt 情形是硬性
	 * 要求。对 insertIt 情形，RelationCacheInsert() 会把此
	 * 关系登记进常规 relcache 失效处理。
	 */
	if (in_progress_list[in_progress_offset].invalidated)
	{
		RelationDestroyRelation(relation, false);
		goto retry;
	}
	Assert(in_progress_offset + 1 == in_progress_list_len);
	in_progress_list_len--;

	/*
	 * 如果需要，把新建的关系插入 relcache 哈希表。
	 *
	 * 有一种场景会发现哈希表条目已存在，尽管调用方未
	 * 找到它：若该关系是 relcache 加载期间要用的系统目
	 * 录或索引，我们可能已在前几步中递归创建过同一
	 * relcache 条目。因此允许 RelationCacheInsert 删除已存
	 * 在的同 OID 的 relcache 条目。已存条目的引用计数应
	 * 为零（否则有人忘了关闭它）；若非如此，我们将
	 * elog 一条 WARNING 并泄漏这个已存在的
	 * 条目。
	 */
	if (insertIt)
		RelationCacheInsert(relation, true);

	/* 已完全有效 */
	relation->rd_isvalid = true;

#ifdef MAYBE_RECOVER_RELATION_BUILD_MEMORY
	if (tmpcxt)
	{
		/* 回到调用方的上下文，并清掉临时上下文 */
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(tmpcxt);
	}
#endif

	return relation;
}

/*
 * 为一个 relcache 条目初始化物理寻址信息（RelFileLocator）
 *
 * 注意：物理层面上，pg_global 表空间中的关系必须当
 * 作共享关系，即使 relisshared 未设置。这里我们不
 * 查看 relisshared。
 */
static void
RelationInitPhysicalAddr(Relation relation)
{
	RelFileNumber oldnumber = relation->rd_locator.relNumber;

	/* 这些关系种类从不拥有存储 */
	if (!RELKIND_HAS_STORAGE(relation->rd_rel->relkind))
		return;

	if (relation->rd_rel->reltablespace)
		relation->rd_locator.spcOid = relation->rd_rel->reltablespace;
	else
		relation->rd_locator.spcOid = MyDatabaseTableSpace;
	if (relation->rd_locator.spcOid == GLOBALTABLESPACE_OID)
		relation->rd_locator.dbOid = InvalidOid;
	else
		relation->rd_locator.dbOid = MyDatabaseId;

	if (relation->rd_rel->relfilenode)
	{
		/*
		 * 即使所用解码快照不代表目录当前状态，也须确保
		 * filenode 指向当前文件，因旧文件将消失（或被
		 * 截断）。新文件仍含旧行，故在其中查找照常
		 * 进行。如果允许重写以不兼容的方式修改模式，
		 * 这就行不通。不过无论目录表还是用户表，
		 * 都禁止以不兼容的方式修改模式——包括那些
		 * 被声明为附加
		 * 目录表的用户表。
		 */
		if (HistoricSnapshotActive()
			&& RelationIsAccessibleInLogicalDecoding(relation)
			&& IsTransactionState())
		{
			HeapTuple	phys_tuple;
			Form_pg_class physrel;

			phys_tuple = ScanPgRelation(RelationGetRelid(relation),
										RelationGetRelid(relation) != ClassOidIndexId,
										true);
			if (!HeapTupleIsValid(phys_tuple))
				elog(ERROR, "could not find pg_class entry for %u",
					 RelationGetRelid(relation));
			physrel = (Form_pg_class) GETSTRUCT(phys_tuple);

			relation->rd_rel->reltablespace = physrel->reltablespace;
			relation->rd_rel->relfilenode = physrel->relfilenode;
			heap_freetuple(phys_tuple);
		}

		relation->rd_locator.relNumber = relation->rd_rel->relfilenode;
	}
	else
	{
		/* 查询关系映射器 */
		relation->rd_locator.relNumber =
			RelationMapOidToFilenumber(relation->rd_id,
									   relation->rd_rel->relisshared);
		if (!RelFileNumberIsValid(relation->rd_locator.relNumber))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 RelationGetRelationName(relation), relation->rd_id);
	}

	/*
	 * 为了让 RelationNeedsWAL() 在并行 worker 上回答正确，
	 * 须恢复 rd_firstRelfilelocatorSubid。并行模式下没有
	 * 子事务会启停，故具体 SubTransactionId 无关紧要。
	 */
	if (IsParallelWorker() && oldnumber != relation->rd_locator.relNumber)
	{
		if (RelFileLocatorSkippingWAL(relation->rd_locator))
			relation->rd_firstRelfilelocatorSubid = TopSubTransactionId;
		else
			relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	}
}

/*
 * 为一个索引关系填入 IndexAmRoutine。
 *
 * relation 的 rd_amhandler 与 rd_indexcxt 必须已有效。
 */
static void
InitIndexAmRoutine(Relation relation)
{
	IndexAmRoutine *cached,
			   *tmp;

	/*
	 * 在当前短命的内存上下文中调用 amhandler，以防它泄漏
	 * 东西（大概率不会，但我们宁可信其有）。
	 */
	tmp = GetIndexAmRoutine(relation->rd_amhandler);

	/* 好，现在把数据转移到 rd_indexcxt 中。 */
	cached = (IndexAmRoutine *) MemoryContextAlloc(relation->rd_indexcxt,
												   sizeof(IndexAmRoutine));
	memcpy(cached, tmp, sizeof(IndexAmRoutine));
	relation->rd_indam = cached;

	pfree(tmp);
}

/*
 * 为一个索引关系初始化索引访问方法支持数据
 */
void
RelationInitIndexAccessInfo(Relation relation)
{
	HeapTuple	tuple;
	Form_pg_am	aform;
	Datum		indcollDatum;
	Datum		indclassDatum;
	Datum		indoptionDatum;
	bool		isnull;
	oidvector  *indcoll;
	oidvector  *indclass;
	int2vector *indoption;
	MemoryContext indexcxt;
	MemoryContext oldcontext;
	int			indnatts;
	int			indnkeyatts;
	uint16		amsupport;

	/*
	 * 复制该索引的 pg_index 条目。由于 pg_index 含
	 * 变长且可能为空的字段，必须老老实实复制，而非
	 * 只是把它当作 Form_pg_index 结构来对待。
	 */
	tuple = SearchSysCache1(INDEXRELID,
							ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u",
			 RelationGetRelid(relation));
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indextuple = heap_copytuple(tuple);
	relation->rd_index = (Form_pg_index) GETSTRUCT(relation->rd_indextuple);
	MemoryContextSwitchTo(oldcontext);
	ReleaseSysCache(tuple);

	/*
	 * 查找该索引的访问方法，保存其处理函数的 OID
	 */
	Assert(relation->rd_rel->relam != InvalidOid);
	tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(relation->rd_rel->relam));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for access method %u",
			 relation->rd_rel->relam);
	aform = (Form_pg_am) GETSTRUCT(tuple);
	relation->rd_amhandler = aform->amhandler;
	ReleaseSysCache(tuple);

	indnatts = RelationGetNumberOfAttributes(relation);
	if (indnatts != IndexRelationGetNumberOfAttributes(relation))
		elog(ERROR, "relnatts disagrees with indnatts for index %u",
			 RelationGetRelid(relation));
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(relation);

	/*
	 * 创建存放索引访问信息的私有上下文。我们需要上
	 * 下文而非仅几个 palloc，是为了不泄漏附于 fmgr 查
	 * 找记录上的任何附属信息。
	 */
	indexcxt = AllocSetContextCreate(CacheMemoryContext,
									 "index info",
									 ALLOCSET_SMALL_SIZES);
	relation->rd_indexcxt = indexcxt;
	MemoryContextCopyAndSetIdentifier(indexcxt,
									  RelationGetRelationName(relation));

	/*
	 * 现在可取索引 AM 的 API 结构
	 */
	InitIndexAmRoutine(relation);

	/*
	 * 分配数组存数据。包含列不用 opclass，故只需为
	 * indnkeyatts 分配它们。
	 */
	relation->rd_opfamily = (Oid *)
		MemoryContextAllocZero(indexcxt, indnkeyatts * sizeof(Oid));
	relation->rd_opcintype = (Oid *)
		MemoryContextAllocZero(indexcxt, indnkeyatts * sizeof(Oid));

	amsupport = relation->rd_indam->amsupport;
	if (amsupport > 0)
	{
		int			nsupport = indnatts * amsupport;

		relation->rd_support = (RegProcedure *)
			MemoryContextAllocZero(indexcxt, nsupport * sizeof(RegProcedure));
		relation->rd_supportinfo = (FmgrInfo *)
			MemoryContextAllocZero(indexcxt, nsupport * sizeof(FmgrInfo));
	}
	else
	{
		relation->rd_support = NULL;
		relation->rd_supportinfo = NULL;
	}

	relation->rd_indcollation = (Oid *)
		MemoryContextAllocZero(indexcxt, indnkeyatts * sizeof(Oid));

	relation->rd_indoption = (int16 *)
		MemoryContextAllocZero(indexcxt, indnkeyatts * sizeof(int16));

	/*
	 * indcollation 不能经 C 结构直接引用，因为它在变
	 * 宽 indkey 字段之后。必须用笨办法提取
	 * datum……
	 */
	indcollDatum = fastgetattr(relation->rd_indextuple,
							   Anum_pg_index_indcollation,
							   GetPgIndexDescriptor(),
							   &isnull);
	Assert(!isnull);
	indcoll = (oidvector *) DatumGetPointer(indcollDatum);
	memcpy(relation->rd_indcollation, indcoll->values, indnkeyatts * sizeof(Oid));

	/*
	 * indclass 无法通过 C 结构直接引用，因为它也位于变宽
	 * 的 indkey 字段之后。必须用笨办法提取
	 * datum……
	 */
	indclassDatum = fastgetattr(relation->rd_indextuple,
								Anum_pg_index_indclass,
								GetPgIndexDescriptor(),
								&isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * 填充支持过程 OID 数组及 opfamily、opclass 输入
	 * 类型的信息。（aminfo 与 supportinfo 留为零，使用时
	 * 即时填充）
	 */
	IndexSupportInitialize(indclass, relation->rd_support,
						   relation->rd_opfamily, relation->rd_opcintype,
						   amsupport, indnkeyatts);

	/*
	 * 同样提取 indoption 并复制到缓存条目
	 */
	indoptionDatum = fastgetattr(relation->rd_indextuple,
								 Anum_pg_index_indoption,
								 GetPgIndexDescriptor(),
								 &isnull);
	Assert(!isnull);
	indoption = (int2vector *) DatumGetPointer(indoptionDatum);
	memcpy(relation->rd_indoption, indoption->values, indnkeyatts * sizeof(int16));

	(void) RelationGetIndexAttOptions(relation, false);

	/*
	 * 表达式、谓词、排除缓存在稍后填充
	 */
	relation->rd_indexprs = NIL;
	relation->rd_indpred = NIL;
	relation->rd_exclops = NULL;
	relation->rd_exclprocs = NULL;
	relation->rd_exclstrats = NULL;
	relation->rd_amcache = NULL;
}

/*
 * IndexSupportInitialize
 *		初始化某索引缓存的 opclass 信息，
 *		以索引的 pg_index.indclass 为据。
 *
 * 数据返回到 *indexSupport、*opFamily 与 *opcInType 中，
 * 它们是由调用方分配的数组。
 *
 * 调用方还会传入 maxSupportNumber 与 maxAttributeNumber，
 * 因为它们指明已分配数组的大小——但实践中这些数字
 * 必须总是与从该索引及访问方法的系统目录条目所能得
 * 到的相匹配。
 */
static void
IndexSupportInitialize(oidvector *indclass,
					   RegProcedure *indexSupport,
					   Oid *opFamily,
					   Oid *opcInType,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber)
{
	int			attIndex;

	for (attIndex = 0; attIndex < maxAttributeNumber; attIndex++)
	{
		OpClassCacheEnt *opcentry;

		if (!OidIsValid(indclass->values[attIndex]))
			elog(ERROR, "bogus pg_index tuple");

		/* 借助缓存查找此 opclass 的信息 */
		opcentry = LookupOpclassInfo(indclass->values[attIndex],
									 maxSupportNumber);

		/* 缓存数据填入 relcache 条目 */
		opFamily[attIndex] = opcentry->opcfamily;
		opcInType[attIndex] = opcentry->opcintype;
		if (maxSupportNumber > 0)
			memcpy(&indexSupport[attIndex * maxSupportNumber],
				   opcentry->supportProcs,
				   maxSupportNumber * sizeof(RegProcedure));
	}
}

/*
 * LookupOpclassInfo
 *
 * 本例程维护每个 opclass 所需信息的缓存，供
 * IndexSupportInitialize() 使用。比依赖目录缓存更高效，
 * 因为我们能在对 pg_amproc 的一次索引扫描中加载某个
 * opclass 的全部信息。
 *
 * pg_am 中支持函数号预期范围的信息是传入而非查出
 * 的，主要因为
 * 调用方手上已有。
 *
 * 注意：无刷写缓存的机制。就目前而言这没问题，因
 * 为现有 opclass 的任何有趣属性都无法 ALTER——你能做
 * 的只有删除它——那只会在缓存中留下无用但无害
 * 条目。要支持改动 opclass 成员关系（不同于 opfamily
 * 成员关系！），就得能同时刷写此缓存，还
 * 要刷写 relcache 条目内容——
 * 含索引。
 */
static OpClassCacheEnt *
LookupOpclassInfo(Oid operatorClassOid,
				  StrategyNumber numSupport)
{
	OpClassCacheEnt *opcentry;
	bool		found;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	htup;
	bool		indexOK;

	if (OpClassCache == NULL)
	{
		/* 首次执行：初始化 opclass 缓存 */
		HASHCTL		ctl;

		/* 同时确保 CacheMemoryContext 存在 */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(OpClassCacheEnt);
		OpClassCache = hash_create("Operator class cache", 64,
								   &ctl, HASH_ELEM | HASH_BLOBS);
	}

	opcentry = (OpClassCacheEnt *) hash_search(OpClassCache,
											   &operatorClassOid,
											   HASH_ENTER, &found);

	if (!found)
	{
		/* 初始化新条目 */
		opcentry->valid = false;	/* 尚未验证 */
		opcentry->numSupport = numSupport;
		opcentry->supportProcs = NULL;	/* 待填充 */
	}
	else
	{
		Assert(numSupport == opcentry->numSupport);
	}

	/*
	 * 积极测试缓存刷写风险时，我们会禁用算子类缓存
	 * 并在每次调用时强制重载信息。这不模拟任何真实行
	 * 为，因为缓存条目不会因此失效。但它有助检测
	 * 缓存加载逻辑本身的缺陷，如依赖未钉住的索引。
	 * 考虑用例有限且开销大增，因此仅在
	 * debug_discard_caches 的高值才
	 * 启用它。
	 */
#ifdef DISCARD_CACHES_ENABLED
	if (debug_discard_caches > 2)
		opcentry->valid = false;
#endif

	if (opcentry->valid)
		return opcentry;

	/*
	 * 需要填充新条目。先分配空间，除非之前某次尝试
	 * 已这样做过。
	 */
	if (opcentry->supportProcs == NULL && numSupport > 0)
		opcentry->supportProcs = (RegProcedure *)
			MemoryContextAllocZero(CacheMemoryContext,
								   numSupport * sizeof(RegProcedure));

	/*
	 * 为免启动期间无限递归，若查找的是我们此处想
	 * 引用的索引所用 opclass 的信息，就强制堆扫描。
	 * 
	 */
	indexOK = criticalRelcachesBuilt ||
		(operatorClassOid != OID_BTREE_OPS_OID &&
		 operatorClassOid != INT2_BTREE_OPS_OID);

	/*
	 * 我们必须取回 pg_opclass 行来确定其 opfamily 与
	 * opcintype，查找相关算子与函数时需要它们。用 syscache
	 * 会很方便，但在引导期间多半
	 * 行不通。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_opclass_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(operatorClassOid));
	rel = table_open(OperatorClassRelationId, AccessShareLock);
	scan = systable_beginscan(rel, OpclassOidIndexId, indexOK,
							  NULL, 1, skey);

	if (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		Form_pg_opclass opclassform = (Form_pg_opclass) GETSTRUCT(htup);

		opcentry->opcfamily = opclassform->opcfamily;
		opcentry->opcintype = opclassform->opcintype;
	}
	else
		elog(ERROR, "could not find tuple for opclass %u", operatorClassOid);

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * 扫描 pg_amproc 以获取该 opclass 的支持过程。只取默认
	 * 的那些（即 lefttype = righttype = opcintype 的）。
	 */
	if (numSupport > 0)
	{
		ScanKeyInit(&skey[0],
					Anum_pg_amproc_amprocfamily,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(opcentry->opcfamily));
		ScanKeyInit(&skey[1],
					Anum_pg_amproc_amproclefttype,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(opcentry->opcintype));
		ScanKeyInit(&skey[2],
					Anum_pg_amproc_amprocrighttype,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(opcentry->opcintype));
		rel = table_open(AccessMethodProcedureRelationId, AccessShareLock);
		scan = systable_beginscan(rel, AccessMethodProcedureIndexId, indexOK,
								  NULL, 3, skey);

		while (HeapTupleIsValid(htup = systable_getnext(scan)))
		{
			Form_pg_amproc amprocform = (Form_pg_amproc) GETSTRUCT(htup);

			if (amprocform->amprocnum <= 0 ||
				(StrategyNumber) amprocform->amprocnum > numSupport)
				elog(ERROR, "invalid amproc number %d for opclass %u",
					 amprocform->amprocnum, operatorClassOid);

			opcentry->supportProcs[amprocform->amprocnum - 1] =
				amprocform->amproc;
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	opcentry->valid = true;
	return opcentry;
}

/*
 * 为一个关系填入 TableAmRoutine
 *
 * relation 的 rd_amhandler 必须已有效。
 */
static void
InitTableAmRoutine(Relation relation)
{
	relation->rd_tableam = GetTableAmRoutine(relation->rd_amhandler);
}

/*
 * 为类表关系初始化表访问方法支持
 */
void
RelationInitTableAccessMethod(Relation relation)
{
	HeapTuple	tuple;
	Form_pg_am	aform;

	if (relation->rd_rel->relkind == RELKIND_SEQUENCE)
	{
		/*
		 * 序列目前如堆表般被访问，但在目录中显示这点
		 * 似乎欠妥。所以只需在这里把它覆盖。
		 * 
		 */
		Assert(relation->rd_rel->relam == InvalidOid);
		relation->rd_amhandler = F_HEAP_TABLEAM_HANDLER;
	}
	else if (IsCatalogRelation(relation))
	{
		/*
		 * 对目录表避免 syscache 查找。
		 */
		Assert(relation->rd_rel->relam == HEAP_TABLE_AM_OID);
		relation->rd_amhandler = F_HEAP_TABLEAM_HANDLER;
	}
	else
	{
		/*
		 * 查找表访问方法，保存其处理函数
		 * OID。
		 */
		Assert(relation->rd_rel->relam != InvalidOid);
		tuple = SearchSysCache1(AMOID,
								ObjectIdGetDatum(relation->rd_rel->relam));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for access method %u",
				 relation->rd_rel->relam);
		aform = (Form_pg_am) GETSTRUCT(tuple);
		relation->rd_amhandler = aform->amhandler;
		ReleaseSysCache(tuple);
	}

	/*
	 * 现在可取表 AM 的 API 结构
	 */
	InitTableAmRoutine(relation);
}

/*
 *		formrdesc
 *
 *		这是 RelationBuildDesc() 的一个特殊精简版本，
 *		在初始化 relcache 时使用。
 *		关系描述符只用给定参数构建，并不真正查看任何
 *		系统表条目。此处作弊甚多，因为只须对少数
 *		基本系统目录有效即可。
 *		
 *
 * 这些目录不能有约束（attnotnull 除外）、默认值、规则
 * 或触发器，因为我们不处理其中任何一项。（当然，
 * 实际上，这只对引导期间或 RelationCacheInitializePhase3
 * 运行前保持有效的属性才有影响，而这些属性当时
 * 要紧……）
 *
 * 注意：我们假定已经切换到 CacheMemoryContext 中。
 */
static void
formrdesc(const char *relationName, Oid relationReltype,
		  bool isshared,
		  int natts, const FormData_pg_attribute *attrs)
{
	Relation	relation;
	int			i;
	bool		has_not_null;

	/*
	 * 分配新关系描述符，清零 reldesc 全部字段
	 */
	relation = (Relation) palloc0(sizeof(RelationData));

	/* 确保关系被标记为尚无已打开文件 */
	relation->rd_smgr = NULL;

	/*
	 * 初始化引用计数：设为 1，因其钉在缓存中
	 */
	relation->rd_refcnt = 1;

	/*
	 * 用本例程构建的条目全都钉在缓存中；无一属于
	 * 新/临时表。
	 */
	relation->rd_isnailed = true;
	relation->rd_createSubid = InvalidSubTransactionId;
	relation->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_droppedSubid = InvalidSubTransactionId;
	relation->rd_backend = InvalidBackendId;
	relation->rd_islocaltemp = false;

	/*
	 * 初始化关系元组形式
	 *
	 * 此处插入的数据相当不完整/虚假，但够我们启动
	 * RelationCacheInitializePhase3() 会从 pg_class 读取真实
	 * 数据并替换此处内容。特别注意 relowner 保持
	 * 为零；RelationCacheInitializePhase3 借此
	 * 得知真实数据尚未就位。
	 */
	relation->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&relation->rd_rel->relname, relationName);
	relation->rd_rel->relnamespace = PG_CATALOG_NAMESPACE;
	relation->rd_rel->reltype = relationReltype;

	/*
	 * 区分共享/非共享关系很重要，即便在引导阶段也
	 * 要确保我们知道它们存储在哪里。
	 */
	relation->rd_rel->relisshared = isshared;
	if (isshared)
		relation->rd_rel->reltablespace = GLOBALTABLESPACE_OID;

	/* formrdesc 只用于永久关系 */
	relation->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;

	/* ……而且它们总是已填充的 */
	relation->rd_rel->relispopulated = true;

	relation->rd_rel->relreplident = REPLICA_IDENTITY_NOTHING;
	relation->rd_rel->relpages = 0;
	relation->rd_rel->reltuples = -1;
	relation->rd_rel->relallvisible = 0;
	relation->rd_rel->relkind = RELKIND_RELATION;
	relation->rd_rel->relnatts = (int16) natts;
	relation->rd_rel->relam = HEAP_TABLE_AM_OID;

	/*
	 * 初始化属性元组形式
	 *
	 * 与关系元组不同，这份数据最好准确无误，因为它
	 * 不会被替换。数据源自 src/include/catalog/
	 * 头文件，由 genbki.pl 生成。
	 */
	relation->rd_att = CreateTemplateTupleDesc(natts);
	relation->rd_att->tdrefcount = 1;	/* 标为引用计数 */

	relation->rd_att->tdtypeid = relationReltype;
	relation->rd_att->tdtypmod = -1;	/* 保险起见 */

	/*
	 * 初始化描述符信息
	 */
	has_not_null = false;
	for (i = 0; i < natts; i++)
	{
		memcpy(TupleDescAttr(relation->rd_att, i),
			   &attrs[i],
			   ATTRIBUTE_FIXED_PART_SIZE);
		has_not_null |= attrs[i].attnotnull;
		/* 确保 attcacheoff 有效 */
		TupleDescAttr(relation->rd_att, i)->attcacheoff = -1;
	}

	/* 初始化首属性 attcacheoff，cf. RelationBuildTupleDesc */
	TupleDescAttr(relation->rd_att, 0)->attcacheoff = 0;

	/* 标记非空状态 */
	if (has_not_null)
	{
		TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

		constr->has_not_null = true;
		relation->rd_att->constr = constr;
	}

	/*
	 * 用 att 数组里的信息初始化关系 id（唉，真丑）
	 */
	RelationGetRelid(relation) = TupleDescAttr(relation->rd_att, 0)->attrelid;

	/*
	 * 用 formrdesc 创建的关系皆为映射关系。这必然如此，
	 * 因为没有任何其他办法能知道它们当前的 filenumber。
	 * 引导模式下，将它们加入初始关系映射器数据，并
	 * 指定初始 filenumber 与 OID 相同。
	 */
	relation->rd_rel->relfilenode = InvalidRelFileNumber;
	if (IsBootstrapProcessingMode())
		RelationMapUpdateMap(RelationGetRelid(relation),
							 RelationGetRelid(relation),
							 isshared, true);

	/*
	 * 初始化关系锁管理器信息
	 */
	RelationInitLockInfo(relation); /* 见lmgr.c */

	/*
	 * 初始化该关系的物理寻址信息
	 */
	RelationInitPhysicalAddr(relation);

	/*
	 * 初始化表 AM 处理函数
	 */
	relation->rd_rel->relam = HEAP_TABLE_AM_OID;
	relation->rd_tableam = GetHeapamTableAmRoutine();

	/*
	 * 利用硬编码知识初始化 rel-has-index 标志
	 */
	if (IsBootstrapProcessingMode())
	{
		/* 引导模式下，我们没有索引 */
		relation->rd_rel->relhasindex = false;
	}
	else
	{
		/* 否则，formrdesc 所用于的所有关系都有索引 */
		relation->rd_rel->relhasindex = true;
	}

	/*
	 * 新 reldesc 进 relcache
	 */
	RelationCacheInsert(relation, false);

	/* 已完全有效 */
	relation->rd_isvalid = true;
}


/* ----------------------------------------------------------------
 *				 关系描述符查找接口
 * ----------------------------------------------------------------
 */

/*
 *		RelationIdGetRelation
 *
 *		按 OID 查找 reldesc；若缓存中没有则新建。
 *
 *		如果找不到给定 relid 的 pg_class 行，则返回 NULL
 *		（暗示我们正试图访问刚删除的关系）。
 *		其他错误经由 elog 报告。
 *
 *		注意：调用方应已至少持有该关系 ID 上的
 *		AccessShareLock，否则就有恶性竞态了。
 *
 *		注意：引用计数递增；新条目则设为 1。
 *		调用方最终应递减该计数。（通常，
 *		经由调用 RelationClose() 完成。）
 */
Relation
RelationIdGetRelation(Oid relationId)
{
	Relation	rd;

	/* 确保我们处于事务中，即使最终只是缓存命中 */
	Assert(IsTransactionState());

	/*
	 * 先尝试在缓存中查找 reldesc
	 */
	RelationIdCacheLookup(relationId, rd);

	if (RelationIsValid(rd))
	{
		/* 对已删除关系返回 NULL */
		if (rd->rd_droppedSubid != InvalidSubTransactionId)
		{
			Assert(!rd->rd_isvalid);
			return NULL;
		}

		RelationIncrementReferenceCount(rd);
		/* 必要时重验缓存条目 */
		if (!rd->rd_isvalid)
		{
			/*
			 * 索引只可能有有限的模式变更，而我们不想使
			 * 用全套流程，因为重载自身依赖的索引会头
			 * 疼。
			 */
			if (rd->rd_rel->relkind == RELKIND_INDEX ||
				rd->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
				RelationReloadIndexInfo(rd);
			else
				RelationClearRelation(rd, true);

			/*
			 * 通常条目在此须有效，但 relcache 未初始化前，
			 * 基础设施不够，查不了 pg_class。此类条目
			 * 不会改变，但我们仍想更新 rd_rel 条目。
			 * 因此
			 * rd_isvalid = false 留在原地，留待后续查找。
			 */
			Assert(rd->rd_isvalid ||
				   (rd->rd_isnailed && !criticalRelcachesBuilt));
		}
		return rd;
	}

	/*
	 * 缓存中没有 reldesc，就让 RelationBuildDesc() 构建之。
	 * 
	 */
	rd = RelationBuildDesc(relationId, true);
	if (RelationIsValid(rd))
		RelationIncrementReferenceCount(rd);
	return rd;
}

/* ----------------------------------------------------------------
 *				缓存失效支持例程
 * ----------------------------------------------------------------
 */

/*
 * RelationIncrementReferenceCount
 *		递增关系引用计数。
 *
 * 注意：引导模式对引用计数行为自有怪异的想法；
 * 理应某天修掉，但眼下先在引导模式中禁用引用
 * 数所有权跟踪。
 */
void
RelationIncrementReferenceCount(Relation rel)
{
	ResourceOwnerEnlargeRelationRefs(CurrentResourceOwner);
	rel->rd_refcnt += 1;
	if (!IsBootstrapProcessingMode())
		ResourceOwnerRememberRelationRef(CurrentResourceOwner, rel);
}

/*
 * RelationDecrementReferenceCount
 *		递减关系引用计数。
 */
void
RelationDecrementReferenceCount(Relation rel)
{
	Assert(rel->rd_refcnt > 0);
	rel->rd_refcnt -= 1;
	if (!IsBootstrapProcessingMode())
		ResourceOwnerForgetRelationRef(CurrentResourceOwner, rel);
}

/*
 * RelationClose - 关闭打开的关系
 *
 *	实际上只是递减引用计数。
 *
 *	注意：若以 -DRELCACHE_FORCE_RELEASE 编译，relcache 条目
 *	会在引用计数降为零时立刻被释放。配合 aset.c 的
 *	CLOBBER_FREED_MEMORY 选项，提供了捕捉对已释放
 *	relcache 条目引用的好测试。不过它会让
 *	速度显著变慢。
 */
void
RelationClose(Relation relation)
{
	/* 注意：无需任何锁操作 */
	RelationDecrementReferenceCount(relation);

	/*
	 * 若该关系在本会话中已不再打开，便可清理其任何
	 * 过时分区描述符。这不太可能发生，所以在耗费一次
	 * mcxt.c 调用之前，先检查是否存在子上下文。
	 */
	if (RelationHasReferenceCountZero(relation))
	{
		if (relation->rd_pdcxt != NULL &&
			relation->rd_pdcxt->firstchild != NULL)
			MemoryContextDeleteChildren(relation->rd_pdcxt);

		if (relation->rd_pddcxt != NULL &&
			relation->rd_pddcxt->firstchild != NULL)
			MemoryContextDeleteChildren(relation->rd_pddcxt);
	}

#ifdef RELCACHE_FORCE_RELEASE
	if (RelationHasReferenceCountZero(relation) &&
		relation->rd_createSubid == InvalidSubTransactionId &&
		relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)
		RelationClearRelation(relation, false);
#endif
}

/*
 * RelationReloadIndexInfo - 为已打开索引重载最少信息
 *
 *	本函数只用于索引。索引的 relcache 失效可能意味着
 *	其 pg_class 或 pg_index 行已变。已有索引的模式
 *	只允许非常有限的变更，无需完整重建即可更新
 *	relcache 条目；这很幸运，因为我们无法重建被"钉住"
 *	和/或使用中的索引条目。我们支持完整替换 pg_class
 *	行——此外还更新 pg_index 行
 *	的少数简单字段。
 *
 *	不一定能马上重读目录行；收到 SI 通知时可能处于
 *	失败事务中。若是，RelationClearRelation 只会把条目
 *	标记为无效（置 rd_isvalid = false）。本例程
 *	会在条目下次被需要时被调用，
 *	以修复它。
 *
 *	我们假定被调用时至少已持有目标索引上的
 *	AccessShareLock。（注意：从 RelationClearRelation 发起
 *	的调用合法，因为已知该关系引用计数为正。）
 *
 *	若目标索引在 pg_class 或 pg_index 上，最好此前已
 *	曾对底层目录持有至少 AccessShareLock，否则
 *	先堆后索引加排他锁者死锁的风险。目前的用法
 *	通过只将它用于正打开或引用计数为正的索引来
 *	保证这一点。
 */
static void
RelationReloadIndexInfo(Relation relation)
{
	bool		indexOK;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;

	/* 只应针对已失效的存活索引调用 */
	Assert((relation->rd_rel->relkind == RELKIND_INDEX ||
			relation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX) &&
		   !relation->rd_isvalid &&
		   relation->rd_droppedSubid == InvalidSubTransactionId);

	/* 确保在 smgr 层面关闭 */
	RelationCloseSmgr(relation);

	/* relcache 刷写须释放任何 AM 缓存数据 */
	if (relation->rd_amcache)
		pfree(relation->rd_amcache);
	relation->rd_amcache = NULL;

	/*
	 * 若是共享索引，可能在本后端尚未完成选库时被调
	 * 用，此时我们还无法读取 pg_class。不过
	 * 共享索引绝不会有任何重大
	 * 的模式更新，忽略失效信号也没关系。只需
	 * 标记为有效后即返回，别无其他动作。
	 */
	if (relation->rd_rel->relisshared && !criticalRelcachesBuilt)
	{
		relation->rd_isvalid = true;
		return;
	}

	/*
	 * 读取 pg_class 行
	 *
	 * 不要试图用 pg_class_oid_index 上的索引扫描来重载
	 * pg_class_oid_index……
	 */
	indexOK = (RelationGetRelid(relation) != ClassOidIndexId);
	pg_class_tuple = ScanPgRelation(RelationGetRelid(relation), indexOK, false);
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "could not find pg_class tuple for index %u",
			 RelationGetRelid(relation));
	relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	memcpy(relation->rd_rel, relp, CLASS_TUPLE_SIZE);
	/* 重载 reloptions 以防其已变化 */
	if (relation->rd_options)
		pfree(relation->rd_options);
	RelationParseRelOptions(relation, pg_class_tuple);
	/* 用完 pg_class 元组 */
	heap_freetuple(pg_class_tuple);
	/* 物理地址可能已变，必须重新计算 */
	RelationInitPhysicalAddr(relation);

	/*
	 * 非系统索引的 pg_index 行中有允许变化的字段，因此
	 * 要重读该行并更新 relcache 条目。从 pg_index 派生的大
	 * 多数信息（如支持函数查找）不能变，本例程的
	 * 意义就在于更新 relcache 条目而不破坏
	 * 那些数据；所以整行替换
	 * 并不合适。
	 */
	if (!IsSystemRelation(relation))
	{
		HeapTuple	tuple;
		Form_pg_index index;

		tuple = SearchSysCache1(INDEXRELID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for index %u",
				 RelationGetRelid(relation));
		index = (Form_pg_index) GETSTRUCT(tuple);

		/*
		 * 基本做法是复制全部 bool 字段。其中有一两个
		 * 在当前代码中实际变不了，但精确追踪是哪些并
		 * 不值得。不过所有数组字段一律
		 * 都不允许变化。
		 */
		relation->rd_index->indisunique = index->indisunique;
		relation->rd_index->indnullsnotdistinct = index->indnullsnotdistinct;
		relation->rd_index->indisprimary = index->indisprimary;
		relation->rd_index->indisexclusion = index->indisexclusion;
		relation->rd_index->indimmediate = index->indimmediate;
		relation->rd_index->indisclustered = index->indisclustered;
		relation->rd_index->indisvalid = index->indisvalid;
		relation->rd_index->indcheckxmin = index->indcheckxmin;
		relation->rd_index->indisready = index->indisready;
		relation->rd_index->indislive = index->indislive;
		relation->rd_index->indisreplident = index->indisreplident;

		/* 同时复制 xmin，理解 indcheckxmin 需要它 */
		HeapTupleHeaderSetXmin(relation->rd_indextuple->t_data,
							   HeapTupleHeaderGetXmin(tuple->t_data));

		ReleaseSysCache(tuple);
	}

	/* 现在它又有效了 */
	relation->rd_isvalid = true;
}

/*
 * RelationReloadNailed - 为钉住的关系重载最少信息。
 *
 * 钉住关系的结构永不改变（这很好，因为我们正依赖知晓
 * 其结构才能读取目录内容）。但有些部分，例如
 * pg_class.relfrozenxid，其内容仍然需要保持准确。因此
 * 这些内容需要在收到失效
 * 后重载。
 */
static void
RelationReloadNailed(Relation relation)
{
	Assert(relation->rd_isnailed);

	/*
	 * 重做 RelationInitPhysicalAddr，以防这是映射已变动的
	 * 映射关系。
	 */
	RelationInitPhysicalAddr(relation);

	/* 标记为需要重新验证 */
	relation->rd_isvalid = false;

	/*
	 * 只有在事务内才能重读目录。若该关系当前打开（
	 * 钉住引用不计），立即重读；否则条目
	 * 已被标记为可能无效，
	 * 并会在下次打开时被修复。
	 */
	if (!IsTransactionState() || relation->rd_refcnt <= 1)
		return;

	if (relation->rd_rel->relkind == RELKIND_INDEX)
	{
		/*
		 * 若是钉住但未映射的索引，就须重读 pg_class 行
		 * 看看其 relfilenumber 是否变了。
		 */
		RelationReloadIndexInfo(relation);
	}
	else
	{
		/*
		 * 重载非索引条目。relcache 未建时做不了，不过
		 * 没关系，该阶段须保持最新的属性（如
		 * relfrozenxid）尚未被访问。为确保日后会重验，
		 * 我们让条目保持无效状态但允许使用（cf.
		 * RelationIdGetRelation）。
		 */
		if (criticalRelcachesBuilt)
		{
			HeapTuple	pg_class_tuple;
			Form_pg_class relp;

			/*
			 * 注意：扫描前先把条目标为有效，以免重建
			 * pg_class 时发生自递归。
			 */
			relation->rd_isvalid = true;

			pg_class_tuple = ScanPgRelation(RelationGetRelid(relation),
											true, false);
			relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
			memcpy(relation->rd_rel, relp, CLASS_TUPLE_SIZE);
			heap_freetuple(pg_class_tuple);

			/*
			 * 再次标记为有效，以防并发到达
			 * 的失效。
			 */
			relation->rd_isvalid = true;
		}
	}
}

/*
 * RelationDestroyRelation
 *
 *	物理删除一个关系缓存条目及其所有附属数据。
 *	调用方必须已把条目从哈希表摘除。
 */
static void
RelationDestroyRelation(Relation relation, bool remember_tupdesc)
{
	Assert(RelationHasReferenceCountZero(relation));

	/*
	 * 确保 smgr 与下层关闭关系文件——若尚未关闭
	 * 的话。（多半已由调用方做过，但再
	 * 确认一遍。）
	 */
	RelationCloseSmgr(relation);

	/* 解开与统计条目的互链 */
	pgstat_unlink_relation(relation);

	/*
	 * 释放 relcache 条目的全部附属数据结构，然后释放
	 * 本身。
	 */
	if (relation->rd_rel)
		pfree(relation->rd_rel);
	/* 不能用 DecrTupleDescRefCount */
	Assert(relation->rd_att->tdrefcount > 0);
	if (--relation->rd_att->tdrefcount == 0)
	{
		/*
		 * 若事务期间重建了 relcache 条目，原因可能是
		 * 某个低于 AccessExclusiveLock 的 ALTER TABLE 改变了
		 * TupDesc。可能有人复制过它，此时副本将指向
		 * 已释放的内存。所以重建条目时出于安全考虑
		 * 会保留
		 * TupDesc 直到事务结束。
		 */
		if (remember_tupdesc)
			RememberToFreeTupleDescAtEOX(relation->rd_att);
		else
			FreeTupleDesc(relation->rd_att);
	}
	FreeTriggerDesc(relation->trigdesc);
	list_free_deep(relation->rd_fkeylist);
	list_free(relation->rd_indexlist);
	list_free(relation->rd_statlist);
	bms_free(relation->rd_keyattr);
	bms_free(relation->rd_pkattr);
	bms_free(relation->rd_idattr);
	bms_free(relation->rd_hotblockingattr);
	bms_free(relation->rd_summarizedattr);
	if (relation->rd_pubdesc)
		pfree(relation->rd_pubdesc);
	if (relation->rd_options)
		pfree(relation->rd_options);
	if (relation->rd_indextuple)
		pfree(relation->rd_indextuple);
	if (relation->rd_amcache)
		pfree(relation->rd_amcache);
	if (relation->rd_fdwroutine)
		pfree(relation->rd_fdwroutine);
	if (relation->rd_indexcxt)
		MemoryContextDelete(relation->rd_indexcxt);
	if (relation->rd_rulescxt)
		MemoryContextDelete(relation->rd_rulescxt);
	if (relation->rd_rsdesc)
		MemoryContextDelete(relation->rd_rsdesc->rscxt);
	if (relation->rd_partkeycxt)
		MemoryContextDelete(relation->rd_partkeycxt);
	if (relation->rd_pdcxt)
		MemoryContextDelete(relation->rd_pdcxt);
	if (relation->rd_pddcxt)
		MemoryContextDelete(relation->rd_pddcxt);
	if (relation->rd_partcheckcxt)
		MemoryContextDelete(relation->rd_partcheckcxt);
	pfree(relation);
}

/*
 * RelationClearRelation
 *
 *	 物理清掉一个关系缓存条目，或重置它并从零重建
 *	 （即依据目录条目重建）。后者用于收到已打开关
 *	 系变更的通知时（即引用计数
 *	 > 0）。
 *
 *	 注意：重建时最好持有该关系的某种锁，否则要
 *	 读的目录数据可能正在脚下滑动。另，待重建关
 *	 系最好有 refcnt > 0。因为访问目录期间可能发
 *	 生 sinval 重置，而 RelationCacheInvalidate 会在
 *	 该关系引用计数为零时把它在我们脚下
 *	 清掉。
 *
 *	 "rebuild" 参数在当前用法中是冗余的，它必须匹配
 *	 关系的引用计数状态；保留它作交叉检查，确保我们
 *	 的行为符合调用方的预期。
 */
static void
RelationClearRelation(Relation relation, bool rebuild)
{
	/*
	 * 如前述，待重建的关系必须 refcnt > 0；当然，清掉
	 * 引用计数非零者同样是坏主意，因为那会让某处
	 * 留下悬空指针。所有调用方都应已验证
	 * 过这一点成立。
	 */
	Assert(rebuild ?
		   !RelationHasReferenceCountZero(relation) :
		   RelationHasReferenceCountZero(relation));

	/*
	 * 确保 smgr 与下层关闭关系文件——若尚未关闭
	 * 的话。若非删除该关系，下次 smgr 访问会自动重
	 * 开文件。这保证 vacuum 截断等情况后，底层文件
	 * 访问状态得到
	 * 更新。
	 */
	RelationCloseSmgr(relation);

	/* 有 AM 缓存则释放 */
	if (relation->rd_amcache)
		pfree(relation->rd_amcache);
	relation->rd_amcache = NULL;

	/*
	 * 钉死的系统关系须区别对待：必须始终可访问，
	 * 我们不能把它们清掉。
	 */
	if (relation->rd_isnailed)
	{
		RelationReloadNailed(relation);
		return;
	}

	/* 标记它无效，直到重建完成 */
	relation->rd_isvalid = false;

	/* RelationForgetRelation()。 */
	if (relation->rd_droppedSubid != InvalidSubTransactionId)
		return;

	/*
	 * 即便非系统索引，只要打开且带有效索引支持信息，
	 * 也不该清掉。这可避免索引支持信息被活跃使用时
	 * 出问题。与钉住索引一样，我们会重读 pg_class
	 * 行来应对其可能的物理迁移，同时检查 pg_index
	 * 有无更新。
	 */
	if ((relation->rd_rel->relkind == RELKIND_INDEX ||
		 relation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX) &&
		relation->rd_refcnt > 0 &&
		relation->rd_indexcxt != NULL)
	{
		if (IsTransactionState())
			RelationReloadIndexInfo(relation);
		return;
	}

	/*
	 * 若确实用完了 relcache 条目，就清掉它。但如果还
	 * 有人还在用，就原位重构整体而不移动 RelationData
	 * 记录（这样那个人的指针仍有效）。
	 * 
	 */
	if (!rebuild)
	{
		/* 把它从哈希表移除 */
		RelationCacheDelete(relation);

		/* 然后释放存储 */
		RelationDestroyRelation(relation, false);
	}
	else if (!IsTransactionState())
	{
		/*
		 * 若不在有效事务中，便无法访问目录，也就无法
		 * 重建。直接退出，留下 rd_isvalid = false，待下次
		 * 打开该条目时再
		 * 重建。
		 *
		 * 注意：可能在子事务回退期间到这里，要重建的
		 * 原因是该关系在外层事务中打开。此时不立即
		 * 重建似乎不安全，因为已打开该关系的代码会
		 * 用 relcache 条目。但此时外层事务应已持有足够
		 * 的锁来防止其模式发生任何重大变化，故现有条
		 * 目内容对其用途应已足够；最坏是统计更新
		 * 之类上落后。（另见 CheckTableNotInUse() 及其调用
		 * 方。）
		 * 这些同样的说明也适用于上文中
		 * 那些没有先执行
		 * RelationReloadIndexInfo() 便退出
		 * 的情形。
		 */
		return;
	}
	else
	{
		/*
		 * 重建打开 relcache 条目的策略：从零建新条目，
		 * 与旧条目交换内容，最后删掉新条目（连同换来
		 * 的基础设施）。这是为了防备中途因出错而失去
		 * 控制。旧条目仍标记为 !rd_isvalid，下次
		 * 问时会再试重建。它并不比之前更无效，因此
		 * 想继续访问它的代码不会因重建失败而受害
		 * （设想某个子事务对一张表执行 ALTER，随后
		 * 在缓存条目重建中途被取消。此时外层
		 * 事务仍应看到未修改的缓存条目仍然
		 * 有效。）出错
		 * 的最坏后果，就是泄漏那个必然
		 * 无人引用的新条目。而且这种事
		 * 也不该发生得如此频繁，以至于
		 * 成为大问题。
		 * 
		 *
		 * 重建打开的 relcache 条目须保留引用计数、rd_*Subid
		 * 与 rd_toastoid 状态。还应尽量原位保留 pg_class
		 * （rd_rel）、元组描述符、重写规则、分区键
		 * 与分区描述符等子结构，因多处代码假定
		 * 打开的 relcache 条目期间它们不动。（注意：
		 * 元组描述符的引用计数机制终或能免去
		 * tupledesc 的这一特技处理。）
		 * 
		 *
		 * 注意该过程不碰 CurrentResourceOwner；很好，因
		 * 条目有的引用计数未必属于
		 * 那个资源所有者。
		 */
		Relation	newrel;
		Oid			save_relid = RelationGetRelid(relation);
		bool		keep_tupdesc;
		bool		keep_rules;
		bool		keep_policies;
		bool		keep_partkey;

		/* 构建临时条目，但不把它链入哈希表 */
		newrel = RelationBuildDesc(save_relid, false);

		/*
		 * 从此处到交换完成间，不要加入会或可能读系统
		 * 目录的代码。该区间必须免于失效处理。参见
		 * RelationBuildDesc() 对 in_progress_list
		 * 的操作。
		 */

		if (newrel == NULL)
		{
			/*
			 * 若正用历史快照，某在解码外访问的关系仍不
			 * 可见，此时标关系无效并返回即可——它会
			 * 在解码结束的重置或下次访问中重载。
			 * 正常处理中我们不想忽略这种
			 * 情况，因为
			 * 它本不该在那里发生——
			 * 理由见下文的解释。
			 */
			if (HistoricSnapshotActive())
				return;

			/*
			 * 这不该发生，因为删除关系时若仍被引用本应
			 * 被阻止（cf. CheckTableNotInUse()）。但若还是到了
			 * 这里，也不能直接删 relcache 条目，因其日后
			 * 或许被访问（如错误可能被捕获并经
			 * 子事务回退而得到处理）。
			 */
			elog(ERROR, "relation %u deleted while still in use", save_relid);
		}

		/*
		 * 若再次遇到 relcache 条目的 relkind 变化情形，就须
		 * 确保 pgstats 不会因此被弄
		 * 乱。
		 */
		Assert(relation->rd_rel->relkind == newrel->rd_rel->relkind);

		keep_tupdesc = equalTupleDescs(relation->rd_att, newrel->rd_att);
		keep_rules = equalRuleLocks(relation->rd_rules, newrel->rd_rules);
		keep_policies = equalRSDesc(relation->rd_rsdesc, newrel->rd_rsdesc);
		/* partkey 一经设置即不可变，所以总能保留它 */
		keep_partkey = (relation->rd_partkey != NULL);

		/*
		 * 交换 relcache 条目内容。此过程旧条目会瞬时
		 * 无效，所以该序列中绝不能有 CHECK_FOR_INTERRUPTS 的
		 * 可能。为安全起见，用
		 * 全内联代码来实现。
		 *
		 * 由于绝大多数字段都要交换，做法是先交换整个
		 * 体，然后对不想交换的少数几个字段
		 * 再换回来。
		 */
#define SWAPFIELD(fldtype, fldname) \
		do { \
			fldtype _tmp = newrel->fldname; \
			newrel->fldname = relation->fldname; \
			relation->fldname = _tmp; \
		} while (0)

		/* 交换全部 Relation 字段 */
		{
			RelationData tmpstruct;

			memcpy(&tmpstruct, newrel, sizeof(RelationData));
			memcpy(newrel, relation, sizeof(RelationData));
			memcpy(relation, &tmpstruct, sizeof(RelationData));
		}

		/* rd_smgr 不能交换，因为 smgr 层有反向链接指向它 */
		SWAPFIELD(SMgrRelation, rd_smgr);
		/* rd_refcnt 必须保留 */
		SWAPFIELD(int, rd_refcnt);
		/* isnailed 不应改变 */
		Assert(newrel->rd_isnailed == relation->rd_isnailed);
		/* 创建子 XID 必须保留 */
		SWAPFIELD(SubTransactionId, rd_createSubid);
		SWAPFIELD(SubTransactionId, rd_newRelfilelocatorSubid);
		SWAPFIELD(SubTransactionId, rd_firstRelfilelocatorSubid);
		SWAPFIELD(SubTransactionId, rd_droppedSubid);
		/* rd_rel 指针不换，改换内容 */
		SWAPFIELD(Form_pg_class, rd_rel);
		/* ……但实际上我们不必更新 newrel->rd_rel */
		memcpy(relation->rd_rel, newrel->rd_rel, CLASS_TUPLE_SIZE);
		/* 无逻辑变化则保留旧 tupledesc、规则与策略 */
		if (keep_tupdesc)
			SWAPFIELD(TupleDesc, rd_att);
		if (keep_rules)
		{
			SWAPFIELD(RuleLock *, rd_rules);
			SWAPFIELD(MemoryContext, rd_rulescxt);
		}
		if (keep_policies)
			SWAPFIELD(RowSecurityDesc *, rd_rsdesc);
		/* toast OID 覆盖值必须保留 */
		SWAPFIELD(Oid, rd_toastoid);
		/* pgstat_info / enabled 必须保留 */
		SWAPFIELD(struct PgStat_TableStatus *, pgstat_info);
		SWAPFIELD(bool, pgstat_enabled);
		/* 如有分区键则保留旧的 */
		if (keep_partkey)
		{
			SWAPFIELD(PartitionKey, rd_partkey);
			SWAPFIELD(MemoryContext, rd_partkeycxt);
		}
		if (newrel->rd_pdcxt != NULL || newrel->rd_pddcxt != NULL)
		{
			/*
			 * 正重建引用计数非零的分区关系，须保留
			 * 旧分区描述符，以防 PartitionDirectory 持有
			 * 指向它的条目。旧 rd_pdcxt 暂不能释放。（这
			 * 是必需：RelationGetPartitionDesc 会直接交出指向
			 * relcache 数据结构指针，不同于惯常交副本之
			 * 法。至于 rd_partkey，
			 * 也有同样的问题——只不过一旦创建，
			 * 我们总会把它保留下来。）
			 * 
			 *
			 * 为确保不彻底泄漏，须重挂到新 reldesc 上，或
			 * 在极小概率已有之时，作为新 reldesc 的 rd_pdcxt
			 * 的子上下文。（对照 RelationBuildPartitionDesc 中
			 * 的手法。）RelationClose 会在引用计数归零后
			 * 清理所有这类上下文。
			 *
			 * 引用计数为零时不会走到此代码，这应当没问
			 * 题，因为那时不该有 PartitionDirectory 持有指向
			 * 旧条目的指针。
			 *
			 * 注意 newrel 与 relation 已交换，所以
			 * newrel 上挂的"旧"分区描述符才是实际的那一个
			 * 
			 */
			relation->rd_partdesc = NULL;	/* 确保 rd_partdesc 无效 */
			relation->rd_partdesc_nodetached = NULL;
			relation->rd_partdesc_nodetached_xmin = InvalidTransactionId;
			if (relation->rd_pdcxt != NULL) /* 多半永不发生 */
				MemoryContextSetParent(newrel->rd_pdcxt, relation->rd_pdcxt);
			else
				relation->rd_pdcxt = newrel->rd_pdcxt;
			if (relation->rd_pddcxt != NULL)
				MemoryContextSetParent(newrel->rd_pddcxt, relation->rd_pddcxt);
			else
				relation->rd_pddcxt = newrel->rd_pddcxt;
			/* 丢弃 newrel 的指针，免得下面把它销毁 */
			newrel->rd_partdesc = NULL;
			newrel->rd_partdesc_nodetached = NULL;
			newrel->rd_partdesc_nodetached_xmin = InvalidTransactionId;
			newrel->rd_pdcxt = NULL;
			newrel->rd_pddcxt = NULL;
		}

#undef SWAPFIELD

		/* 现在可以丢弃临时条目了 */
		RelationDestroyRelation(newrel, !keep_tupdesc);
	}
}

/*
 * RelationFlushRelation
 *
 *	 关系打开（refcount > 0）则重建，否则清掉。
 *	 它用于我们收到该关系的缓存失效事件时。
 */
static void
RelationFlushRelation(Relation relation)
{
	if (relation->rd_createSubid != InvalidSubTransactionId ||
		relation->rd_firstRelfilelocatorSubid != InvalidSubTransactionId)
	{
		/*
		 * 新建 relcache 条目总是重建而非刷写，否则会忘
		 * 掉"新建"状态。
		 * relfilenumber 亦然。
		 *
		 * 此处引用计数可能为零，故临时递增它以保证
		 * 重建是安全的。可假定当前事务已持有该关系
		 * 的某种锁。
		 */
		RelationIncrementReferenceCount(relation);
		RelationClearRelation(relation, true);
		RelationDecrementReferenceCount(relation);
	}
	else
	{
		/*
		 * 已存在的 rels 若未打开则可从 relcache 中删除。
		 */
		bool		rebuild = !RelationHasReferenceCountZero(relation);

		RelationClearRelation(relation, rebuild);
	}
}

/*
 * RelationForgetRelation - 调用方报告它已删除该关系
 */
void
RelationForgetRelation(Oid rid)
{
	Relation	relation;

	RelationIdCacheLookup(rid, relation);

	if (!PointerIsValid(relation))
		return;					/* 不在缓存，无事可做 */

	if (!RelationHasReferenceCountZero(relation))
		elog(ERROR, "relation %u is still open", rid);

	Assert(relation->rd_droppedSubid == InvalidSubTransactionId);
	if (relation->rd_createSubid != InvalidSubTransactionId ||
		relation->rd_firstRelfilelocatorSubid != InvalidSubTransactionId)
	{
		/*
		 * 若子事务回退，须记住 rd_*Subid。将条目标为
		 * "已删除"，令 RelationClearRelation() 失效之而非
		 * 毁。（若在顶层事务中，还可以选择
		 * 直接销毁该条目。）
		 */
		relation->rd_droppedSubid = GetCurrentSubTransactionId();
	}

	RelationClearRelation(relation, false);
}

/*
 *		RelationCacheInvalidateEntry
 *
 *		本例程因 SI 缓存刷写消息而被调用。
 *
 * 匹配该 relid 的所有 relcache 条目都必须刷写。（注意：
 * 调用方已确定该 relid 属于本库或为共享关系。）
 * 
 *
 * 过去我们跳过本地关系，理由是它们不会成为跨
 * 更新消息的目标；但处理它们似更稳妥，如此我
 * *自己*的 SI 更新消息在 CommandCounterIncrement 期间
 * 对本地关系与非本地关系都产生
 * 同样的效果。
 */
void
RelationCacheInvalidateEntry(Oid relationId)
{
	Relation	relation;

	RelationIdCacheLookup(relationId, relation);

	if (PointerIsValid(relation))
	{
		relcacheInvalsReceived++;
		RelationFlushRelation(relation);
	}
	else
	{
		int			i;

		for (i = 0; i < in_progress_list_len; i++)
			if (in_progress_list[i].reloid == relationId)
				in_progress_list[i].invalidated = true;
	}
}

/*
 * RelationCacheInvalidate
 *	 清掉引用计数为零的缓存描述符，重建引用计数
 *	 为正的那些。同时重置 smgr 缓存并重读关系映射数
 *	 据。
 *
 *	 除 debug_discard_caches 外，它目前只用于从 SI 消息缓
 *	 冲溢出，因此不碰带事务内新 relfilenumber 的关系
 *	 它们不可能是跨后端 SI 更新的目标（而自身
 *	 的更新现在走一条不受 SI 消息缓冲限制的独立链
 *	 表。）
 *
 *	 分两阶段做：第一遍删除可删项，第二遍重建可
 *	 重建项。这对安全至关重要，因为 hash_seq_search 只
 *	 能应对正访问元素被并发删除的情形。若走表期间
 *	 发生第二次 SI 溢出而递归进入本例程，就会因内层
 *	 调清掉外层扫描的下一访问目标而崩溃。但使用这
 *	 种做法可行：（a）第一遍不再处理任何 SI 消息，
 *	 hash_seq_search 会安全完成；（b）
 *	 第二遍期间我们只持有指向
 *	 不可删除条目的
 *	 指针。
 *
 *	 两阶段做法还便于我们在做任何事之前先更新映射
 *	 的 relfilenumber，并保证第二遍先处理钉住的条目
 *	 再处理其他不可删条目。这保证系统目录在我们
 *	 试图用它们来重载其他已打开关系的信息之前
 *	 已经是最新的。
 *
 *	 在这两阶段的工作即时生效后，通常会通知栈上
 *	 所有 RelationBuildDesc() 重新开始。但如果调用源于
 *	 debug_discard_caches 则不这么做。否则 RelationBuildDesc()
 *	 会变成无限循环。
 */
void
RelationCacheInvalidate(bool debug_discard)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	Relation	relation;
	List	   *rebuildFirstList = NIL;
	List	   *rebuildList = NIL;
	ListCell   *l;
	int			i;

	/*
	 * 在开始重建缓存前重载关系映射数据。
	 */
	RelationMapInvalidateAll();

	/* 第1步 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		relation = idhentry->reldesc;

		/* 必须关闭所有 smgr 引用，避免留下悬空指针 */
		RelationCloseSmgr(relation);

		/*
		 * 忽略新关系：提交前无其他后端会动它们。同样
		 * 在换掉关系的 relfilelocator 前，也已取得
		 * AccessExclusiveLock 并清空适用的
		 * 待处理失效。
		 */
		if (relation->rd_createSubid != InvalidSubTransactionId ||
			relation->rd_firstRelfilelocatorSubid != InvalidSubTransactionId)
			continue;

		relcacheInvalsReceived++;

		if (RelationHasReferenceCountZero(relation))
		{
			/* 立即删除此条目 */
			Assert(!relation->rd_isnailed);
			RelationClearRelation(relation, false);
		}
		else
		{
			/*
			 * 如果是映射关系，立即更新其 rd_locator，以防其
			 * relfilenumber 已变。必须在第1步期间做，以防重
			 * 建其他条目时在第2步中被查询。
			 * 这样做是安全的，因为查询映射
			 * 不涉及访问任何 relcache 条目。
			 */
			if (RelationIsMapped(relation))
				RelationInitPhysicalAddr(relation);

			/*
			 * 把此条目加入第二遍要重建的列表。
			 * pg_class 进 rebuildFirstList 队首，而
			 * pg_class_oid_index 入队尾，两者分列第一、第二
			 * 个处理。其他钉住关系进 rebuildList 队首，
			 * 以任意次序接着处理；其余的都进
			 * rebuildList 的队
			 * 尾。
			 */
			if (RelationGetRelid(relation) == RelationRelationId)
				rebuildFirstList = lcons(relation, rebuildFirstList);
			else if (RelationGetRelid(relation) == ClassOidIndexId)
				rebuildFirstList = lappend(rebuildFirstList, relation);
			else if (relation->rd_isnailed)
				rebuildList = lcons(relation, rebuildList);
			else
				rebuildList = lappend(rebuildList, relation);
		}
	}

	/*
	 * 现在清掉剩余的 smgr 缓存条目。这必须发生在开始
	 * 重建条目之前，因为那可能涉及目录读取并重新
	 * 打开目录文件。
	 */
	smgrcloseall();

	/* 第2步：重建第1步发现需重建的条目 */
	foreach(l, rebuildFirstList)
	{
		relation = (Relation) lfirst(l);
		RelationClearRelation(relation, true);
	}
	list_free(rebuildFirstList);
	foreach(l, rebuildList)
	{
		relation = (Relation) lfirst(l);
		RelationClearRelation(relation, true);
	}
	list_free(rebuildList);

	if (!debug_discard)
		/* 栈上的 RelationBuildDesc() 都须重新开始。 */
		for (i = 0; i < in_progress_list_len; i++)
			in_progress_list[i].invalidated = true;
}

/*
 * RelationCloseSmgrByOid - 关闭条目的 smgr 链接
 *
 * 需要在某些改变关系物理映射的情况下用到。链接
 * 会在下次使用时自动重新打开。
 */
void
RelationCloseSmgrByOid(Oid relationId)
{
	Relation	relation;

	RelationIdCacheLookup(relationId, relation);

	if (!PointerIsValid(relation))
		return;					/* 不在缓存，无事可做 */

	RelationCloseSmgr(relation);
}

static void
RememberToFreeTupleDescAtEOX(TupleDesc td)
{
	if (EOXactTupleDescArray == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		EOXactTupleDescArray = (TupleDesc *) palloc(16 * sizeof(TupleDesc));
		EOXactTupleDescArrayLen = 16;
		NextEOXactTupleDescNum = 0;
		MemoryContextSwitchTo(oldcxt);
	}
	else if (NextEOXactTupleDescNum >= EOXactTupleDescArrayLen)
	{
		int32		newlen = EOXactTupleDescArrayLen * 2;

		Assert(EOXactTupleDescArrayLen > 0);

		EOXactTupleDescArray = (TupleDesc *) repalloc(EOXactTupleDescArray,
													  newlen * sizeof(TupleDesc));
		EOXactTupleDescArrayLen = newlen;
	}

	EOXactTupleDescArray[NextEOXactTupleDescNum++] = td;
}

#ifdef USE_ASSERT_CHECKING
static void
AssertPendingSyncConsistency(Relation relation)
{
	bool		relcache_verdict =
		RelationIsPermanent(relation) &&
		((relation->rd_createSubid != InvalidSubTransactionId &&
		  RELKIND_HAS_STORAGE(relation->rd_rel->relkind)) ||
		 relation->rd_firstRelfilelocatorSubid != InvalidSubTransactionId);

	Assert(relcache_verdict == RelFileLocatorSkippingWAL(relation->rd_locator));

	if (relation->rd_droppedSubid != InvalidSubTransactionId)
		Assert(!relation->rd_isvalid &&
			   (relation->rd_createSubid != InvalidSubTransactionId ||
				relation->rd_firstRelfilelocatorSubid != InvalidSubTransactionId));
}

/*
 * AssertPendingSyncs_RelationCache
 *
 *	断言 relcache.c 与 storage.c 在是否跳过 WAL 上一致。
 */
void
AssertPendingSyncs_RelationCache(void)
{
	HASH_SEQ_STATUS status;
	LOCALLOCK  *locallock;
	Relation   *rels;
	int			maxrels;
	int			nrels;
	RelIdCacheEnt *idhentry;
	int			i;

	/*
	 * 打开本事务已加锁的所有关系。若对某关系而言，
	 * storage.c 正跳过 WAL 而 relcache.c 没有跳过，
	 * 通常一次 CommandCounterIncrement() 会产生一条摧毁该
	 * relcache 条目的本地失效消息。在此重建这类条目，
	 * 就能发现此问题。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());
	maxrels = 1;
	rels = palloc(maxrels * sizeof(*rels));
	nrels = 0;
	hash_seq_init(&status, GetLockMethodLocalHash());
	while ((locallock = (LOCALLOCK *) hash_seq_search(&status)) != NULL)
	{
		Oid			relid;
		Relation	r;

		if (locallock->nLocks <= 0)
			continue;
		if ((LockTagType) locallock->tag.lock.locktag_type !=
			LOCKTAG_RELATION)
			continue;
		relid = ObjectIdGetDatum(locallock->tag.lock.locktag_field2);
		r = RelationIdGetRelation(relid);
		if (!RelationIsValid(r))
			continue;
		if (nrels >= maxrels)
		{
			maxrels *= 2;
			rels = repalloc(rels, maxrels * sizeof(*rels));
		}
		rels[nrels++] = r;
	}

	hash_seq_init(&status, RelationIdCache);
	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
		AssertPendingSyncConsistency(idhentry->reldesc);

	for (i = 0; i < nrels; i++)
		RelationClose(rels[i]);
	PopActiveSnapshot();
}
#endif

/*
 * AtEOXact_RelationCache
 *
 *	在主事务提交或中止时清理 relcache。
 *
 * 注意：必须在处理失效消息*之前*调用它。
 * 中止时我们不想尝试重建任何已失效的缓存条
 * 目（因为无法安全访问数据库）。因此必须在处
 * 理待处理失效之前先重置引用计数。
 *
 * 从 PostgreSQL 8.1 起，relcache 引用计数应由
 * ResourceOwner 机制释放。它只做调试交叉检查，
 * 确认没有遗留的钉住。但当本事务创建过关系或使
 * 用过强制索引列表时，还需要做
 * 特殊清理。
 */
void
AtEOXact_RelationCache(bool isCommit)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	int			i;

	/*
	 * 忘掉 in_progress_list。当我们因 RelationBuildDesc() 中
	 * 途出错而中止时才有意义。
	 */
	Assert(in_progress_list_len == 0 || !isCommit);
	in_progress_list_len = 0;

	/*
	 * 除非 eoxact_list[] 已溢出，否则只需检查其中列出的关
	 * 系。否则退回用 hash_seq_search 扫描。
	 *
	 * 简单起见，eoxact_list[] 条目到顶层事务结束才删，
	 * 尽管有时本可在子事务末移除，或因他故删
	 * 除关系也能从列表移除。因此应当预期列表条目
	 * 在哈希表中找不到的情形；若找
	 * 不到，则对它们
	 * 无事可做。
	 */
	if (eoxact_list_overflowed)
	{
		hash_seq_init(&status, RelationIdCache);
		while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
		{
			AtEOXact_cleanup(idhentry->reldesc, isCommit);
		}
	}
	else
	{
		for (i = 0; i < eoxact_list_len; i++)
		{
			idhentry = (RelIdCacheEnt *) hash_search(RelationIdCache,
													 &eoxact_list[i],
													 HASH_FIND,
													 NULL);
			if (idhentry != NULL)
				AtEOXact_cleanup(idhentry->reldesc, isCommit);
		}
	}

	if (EOXactTupleDescArrayLen > 0)
	{
		Assert(EOXactTupleDescArray != NULL);
		for (i = 0; i < NextEOXactTupleDescNum; i++)
			FreeTupleDesc(EOXactTupleDescArray[i]);
		pfree(EOXactTupleDescArray);
		EOXactTupleDescArray = NULL;
	}

	/* 现在已出事务，可以清空这些列表 */
	eoxact_list_len = 0;
	eoxact_list_overflowed = false;
	NextEOXactTupleDescNum = 0;
	EOXactTupleDescArrayLen = 0;
}

/*
 * AtEOXact_cleanup
 *
 *	在主事务提交或中止时清理单个关系
 *
 * 注意：此处理必须幂等，因为 EOXactListAdd() 不去防止
 * eoxact_list[] 中出现重复条目。
 */
static void
AtEOXact_cleanup(Relation relation, bool isCommit)
{
	bool		clear_relcache = false;

	/*
	 * relcache 条目引用计数应回到事务外的常态：
	 * 0，除非它被钉在缓存中。
	 *
	 * 引导模式下并非如此，故不检查——预期关系
	 * 在 start/commit transaction 调用之间保持打开。
	 * （这看起来不对，但不值得修。）
	 *
	 * 注意：理想情况下此检查应施加于每个 relcache 条目
	 * 不只是有 eoxact 工作要做的条目。但不值得为此强
	 * 扫描整个 relcache。况且这意味着开断言的测试永
	 * 不走到上面的 hash_search 代码路径，这似乎是个
	 * 坏主意。
	 */
#ifdef USE_ASSERT_CHECKING
	if (!IsBootstrapProcessingMode())
	{
		int			expected_refcnt;

		expected_refcnt = relation->rd_isnailed ? 1 : 0;
		Assert(relation->rd_refcnt == expected_refcnt);
	}
#endif

	/*
	 * 本事务结束后该关系还存活吗？
	 *
	 * 提交时，若条目在关系删除后被保留，则清掉它
	 * 免其成为孤儿。回滚时，若关系创建于当前事务，
	 * 则清掉 relcache 条目，因为一旦出了该事务，
	 * 它就不再有任何
	 * 意义了。
	 */
	clear_relcache =
		(isCommit ?
		 relation->rd_droppedSubid != InvalidSubTransactionId :
		 relation->rd_createSubid != InvalidSubTransactionId);

	/*
	 * 既然现在已出事务，就把这些子 id 重置为零。这也让
	 * RelationClearRelation() 得以丢弃该 relcache 条目。
	 */
	relation->rd_createSubid = InvalidSubTransactionId;
	relation->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	relation->rd_droppedSubid = InvalidSubTransactionId;

	if (clear_relcache)
	{
		if (RelationHasReferenceCountZero(relation))
		{
			RelationClearRelation(relation, false);
			return;
		}
		else
		{
			/*
			 * 嗯，某处有该关系的（泄漏?）引用，不敢移
			 * 除，免日后解悬空指针。发牢骚并标
			 * 记它不属于当前事务。希望终会被清理。
			 * 这里必须只是 WARNING 级，以免
			 * 错误恢复期间再次出错形成
			 * 循环。
			 */
			elog(WARNING, "cannot remove relcache entry for \"%s\" because it has nonzero refcount",
				 RelationGetRelationName(relation));
		}
	}
}

/*
 * AtEOSubXact_RelationCache
 *
 *	在子事务提交或中止时清理 relcache。
 *
 * 注意：必须在处理失效消息*之前*调用它。
 */
void
AtEOSubXact_RelationCache(bool isCommit, SubTransactionId mySubid,
						  SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	int			i;

	/*
	 * 忘掉 in_progress_list。当我们因 RelationBuildDesc() 中
	 * 途出错而中止时才有意义。在 RelationBuildDesc()
	 * 里不会提交子事务。
	 */
	Assert(in_progress_list_len == 0 || !isCommit);
	in_progress_list_len = 0;

	/*
	 * 除非 eoxact_list[] 已溢出，否则只需检查其中列出的关
	 * 系。否则退回用 hash_seq_search 扫描。逻辑与
	 * AtEOXact_RelationCache 相同。
	 */
	if (eoxact_list_overflowed)
	{
		hash_seq_init(&status, RelationIdCache);
		while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
		{
			AtEOSubXact_cleanup(idhentry->reldesc, isCommit,
								mySubid, parentSubid);
		}
	}
	else
	{
		for (i = 0; i < eoxact_list_len; i++)
		{
			idhentry = (RelIdCacheEnt *) hash_search(RelationIdCache,
													 &eoxact_list[i],
													 HASH_FIND,
													 NULL);
			if (idhentry != NULL)
				AtEOSubXact_cleanup(idhentry->reldesc, isCommit,
									mySubid, parentSubid);
		}
	}

	/* 不要重置该列表；稍后还需要更多清理 */
}

/*
 * AtEOSubXact_cleanup
 *
 *	在子事务提交或中止时清理单个关系
 *
 * 注意：此处理必须幂等，因为 EOXactListAdd() 不去防止
 * eoxact_list[] 中出现重复条目。
 */
static void
AtEOSubXact_cleanup(Relation relation, bool isCommit,
					SubTransactionId mySubid, SubTransactionId parentSubid)
{
	/*
	 * 它是当前子事务中创建的关系吗？
	 *
	 * 子提交时，只要未删除，就把它标为属于父事务；
	 * 否则直接删除 relcache 条目——它已不再
	 * 有任何用处了。
	 */
	if (relation->rd_createSubid == mySubid)
	{
		/*
		 * 有效的 rd_droppedSubid 意味对应关系已删除，但
		 * relcache 条目为提交时待同步而被保留。须在此
		 * 显式删除它，免得条目成为孤儿。
		 */
		Assert(relation->rd_droppedSubid == mySubid ||
			   relation->rd_droppedSubid == InvalidSubTransactionId);
		if (isCommit && relation->rd_droppedSubid == InvalidSubTransactionId)
			relation->rd_createSubid = parentSubid;
		else if (RelationHasReferenceCountZero(relation))
		{
			/* 允许移除该条目 */
			relation->rd_createSubid = InvalidSubTransactionId;
			relation->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
			relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
			relation->rd_droppedSubid = InvalidSubTransactionId;
			RelationClearRelation(relation, false);
			return;
		}
		else
		{
			/*
			 * 嗯，某处有该关系的（泄漏?）引用，不敢移
			 * 除，免得日后解引用
			 * 悬空指针。发牢骚，并把它移交给父
			 * 子事务，以便日后再试。这必须只是一条
			 * WARNING，以免错误恢复中的错误循环。
			 */
			relation->rd_createSubid = parentSubid;
			elog(WARNING, "cannot remove relcache entry for \"%s\" because it has nonzero refcount",
				 RelationGetRelationName(relation));
		}
	}

	/*
	 * 同样，更新或删除任何子事务内新 relfilenumber 记录，
	 * 或删除之。
	 */
	if (relation->rd_newRelfilelocatorSubid == mySubid)
	{
		if (isCommit)
			relation->rd_newRelfilelocatorSubid = parentSubid;
		else
			relation->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	}

	if (relation->rd_firstRelfilelocatorSubid == mySubid)
	{
		if (isCommit)
			relation->rd_firstRelfilelocatorSubid = parentSubid;
		else
			relation->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	}

	if (relation->rd_droppedSubid == mySubid)
	{
		if (isCommit)
			relation->rd_droppedSubid = parentSubid;
		else
			relation->rd_droppedSubid = InvalidSubTransactionId;
	}
}


/*
 *		RelationBuildLocalRelation
 *			为即将创建的关系构建 relcache 条目，
 *			并登记进 relcache。
 */
Relation
RelationBuildLocalRelation(const char *relname,
						   Oid relnamespace,
						   TupleDesc tupDesc,
						   Oid relid,
						   Oid accessmtd,
						   RelFileNumber relfilenumber,
						   Oid reltablespace,
						   bool shared_relation,
						   bool mapped_relation,
						   char relpersistence,
						   char relkind)
{
	Relation	rel;
	MemoryContext oldcxt;
	int			natts = tupDesc->natts;
	int			i;
	bool		has_not_null;
	bool		nailit;

	Assert(natts >= 0);

	/*
	 * 检查是否创建了必须钉在缓存中的关系。
	 *
	 * XXX 本清单应与 RelationCacheInitializePhase2/3 特殊处理
	 * 的关系一致。
	 */
	switch (relid)
	{
		case DatabaseRelationId:
		case AuthIdRelationId:
		case AuthMemRelationId:
		case RelationRelationId:
		case AttributeRelationId:
		case ProcedureRelationId:
		case TypeRelationId:
			nailit = true;
			break;
		default:
			nailit = false;
			break;
	}

	/*
	 * 检查共享关系的硬编码清单与引导 .bki 文件中
	 * 的内容一致。若 initdb 期间此处失败，多半需要
	 * 修正 IsSharedRelation() 以匹配你对共享关系集合
	 * 所做的改动。
	 */
	if (shared_relation != IsSharedRelation(relid))
		elog(ERROR, "shared_relation flag for \"%s\" does not match IsSharedRelation(%u)",
			 relname, relid);

	/* 共享关系也必须已被映射 */
	Assert(mapped_relation || !shared_relation);

	/*
	 * 切换到缓存上下文以创建 relcache 条目。
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 分配一个新关系描述符并填入基本状态字段。
	 */
	rel = (Relation) palloc0(sizeof(RelationData));

	/* 确保关系被标记为尚无已打开文件 */
	rel->rd_smgr = NULL;

	/* 酌情将其标记为钉住 */
	rel->rd_isnailed = nailit;

	rel->rd_refcnt = nailit ? 1 : 0;

	/* 它正由本事务创建 */
	rel->rd_createSubid = GetCurrentSubTransactionId();
	rel->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	rel->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	rel->rd_droppedSubid = InvalidSubTransactionId;

	/*
	 * 用传入的描述符创建新的元组描述符。这样做，
	 * 一方面是为了把它复制进缓存上下文，另一方面
	 * 是因为新关系不可能已有默认值或约束；这些
	 * 内容必须留待后续步骤添加，因为需要对多个系
	 * 统目录增补。不过 attnotnull 约束可在此复制。
	 */
	rel->rd_att = CreateTupleDescCopy(tupDesc);
	rel->rd_att->tdrefcount = 1;	/* 引用计数标记 */
	has_not_null = false;
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute satt = TupleDescAttr(tupDesc, i);
		Form_pg_attribute datt = TupleDescAttr(rel->rd_att, i);

		datt->attidentity = satt->attidentity;
		datt->attgenerated = satt->attgenerated;
		datt->attnotnull = satt->attnotnull;
		has_not_null |= satt->attnotnull;
	}

	if (has_not_null)
	{
		TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

		constr->has_not_null = true;
		rel->rd_att->constr = constr;
	}

	/*
	 * 初始化关系元组形态（调用方稍后可增补或覆盖）
	 */
	rel->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&rel->rd_rel->relname, relname);
	rel->rd_rel->relnamespace = relnamespace;

	rel->rd_rel->relkind = relkind;
	rel->rd_rel->relnatts = natts;
	rel->rd_rel->reltype = InvalidOid;
	/* 引导时需要： */
	rel->rd_rel->relowner = BOOTSTRAP_SUPERUSERID;

	/* 建立依赖它的持久性与 relcache 字段 */
	rel->rd_rel->relpersistence = relpersistence;
	switch (relpersistence)
	{
		case RELPERSISTENCE_UNLOGGED:
		case RELPERSISTENCE_PERMANENT:
			rel->rd_backend = InvalidBackendId;
			rel->rd_islocaltemp = false;
			break;
		case RELPERSISTENCE_TEMP:
			Assert(isTempOrTempToastNamespace(relnamespace));
			rel->rd_backend = BackendIdForTempRelations();
			rel->rd_islocaltemp = true;
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relpersistence);
			break;
	}

	/* 若是物化视图，初始时未填充数据 */
	if (relkind == RELKIND_MATVIEW)
		rel->rd_rel->relispopulated = false;
	else
		rel->rd_rel->relispopulated = true;

	/* 设置副本标识 -- 系统目录与非表对象没有副本标识 */
	if (!IsCatalogNamespace(relnamespace) &&
		(relkind == RELKIND_RELATION ||
		 relkind == RELKIND_MATVIEW ||
		 relkind == RELKIND_PARTITIONED_TABLE))
		rel->rd_rel->relreplident = REPLICA_IDENTITY_DEFAULT;
	else
		rel->rd_rel->relreplident = REPLICA_IDENTITY_NOTHING;

	/*
	 * 将关系的物理与逻辑标识符（OID）插入正确的位置。
	 * 对于映射关系，我们把 relfilenumber 置零并依靠
	 * RelationInitPhysicalAddr 查询映射。
	 */
	rel->rd_rel->relisshared = shared_relation;

	RelationGetRelid(rel) = relid;

	for (i = 0; i < natts; i++)
		TupleDescAttr(rel->rd_att, i)->attrelid = relid;

	rel->rd_rel->reltablespace = reltablespace;

	if (mapped_relation)
	{
		rel->rd_rel->relfilenode = InvalidRelFileNumber;
		/* 将其加入活跃映射信息 */
		RelationMapUpdateMap(relid, relfilenumber, shared_relation, true);
	}
	else
		rel->rd_rel->relfilenode = relfilenumber;

	RelationInitLockInfo(rel);	/* 见lmgr.c */

	RelationInitPhysicalAddr(rel);

	rel->rd_rel->relam = accessmtd;

	/*
	 * RelationInitTableAccessMethod 会做 syscache 查找，
	 * 因此绝不能在 CacheMemoryContext 中运行它。好在
	 * 余下步骤无需长期上下文。
	 */
	MemoryContextSwitchTo(oldcxt);

	if (RELKIND_HAS_TABLE_AM(relkind) || relkind == RELKIND_SEQUENCE)
		RelationInitTableAccessMethod(rel);

	/*
	 * 可以插入 relcache 哈希表了。
	 *
	 * 通常同一 OID 绝不应已有现存哈希条目；但在引导
	 * 期间，当我们为某个引导关系创建"真正"的
	 * relcache 条目时，我们会覆盖 formrdesc 创建的
	 * 假条目。所以允许这种覆盖发生在钉住
	 * rels.
	 */
	RelationCacheInsert(rel, nailit);

	/*
	 * 标记关系待事务结束清理（清 rd_createSubid）。
	 * 但存入 relid 前不能这么做。
	 */
	EOXactListAdd(rel);

	/* 已完全有效 */
	rel->rd_isvalid = true;

	/*
	 * 调用方期望我们钉住返回条目。
	 */
	RelationIncrementReferenceCount(rel);

	return rel;
}


/*
 * RelationSetNewRelfilenumber
 *
 * 为关系指派新的 relfilenumber（物理文件名），
 * 或还指派新的持久性设置。
 *
 * 这让关系能以事务安全的方式完全重写（因为文件号
 * 的指派可以回滚）。但请注意，在当前事务的剩余
 * 部分里，没有什么简单办法可以访问该关系的旧
 * 数据。这把它限定在 TRUNCATE 或从头重建索引等
 * 场景中才有用。
 *
 * 调用方必须已持有该关系上的排他锁。
 */
void
RelationSetNewRelfilenumber(Relation relation, char persistence)
{
	RelFileNumber newrelfilenumber;
	Relation	pg_class;
	ItemPointerData otid;
	HeapTuple	tuple;
	Form_pg_class classform;
	MultiXactId minmulti = InvalidMultiXactId;
	TransactionId freezeXid = InvalidTransactionId;
	RelFileLocator newrlocator;

	if (!IsBinaryUpgrade)
	{
		/* 分配新的 relfilenumber */
		newrelfilenumber = GetNewRelFileNumber(relation->rd_rel->reltablespace,
											   NULL, persistence);
	}
	else if (relation->rd_rel->relkind == RELKIND_INDEX)
	{
		if (!OidIsValid(binary_upgrade_next_index_pg_class_relfilenumber))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("index relfilenumber value not set when in binary upgrade mode")));

		newrelfilenumber = binary_upgrade_next_index_pg_class_relfilenumber;
		binary_upgrade_next_index_pg_class_relfilenumber = InvalidOid;
	}
	else if (relation->rd_rel->relkind == RELKIND_RELATION)
	{
		if (!OidIsValid(binary_upgrade_next_heap_pg_class_relfilenumber))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("heap relfilenumber value not set when in binary upgrade mode")));

		newrelfilenumber = binary_upgrade_next_heap_pg_class_relfilenumber;
		binary_upgrade_next_heap_pg_class_relfilenumber = InvalidOid;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unexpected request for new relfilenumber in binary upgrade mode")));

	/*
	 * 为给定关系获取一份可写的 pg_class 元组副本。
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(RELOID,
									  ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u",
			 RelationGetRelid(relation));
	otid = tuple->t_self;
	classform = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * 安排在事务提交时 unlink 旧存储；但执行二进制
	 * 升级时必须立即执行。
	 */
	if (IsBinaryUpgrade)
	{
		SMgrRelation srel;

		/*
		 * 二进制升级期间，我们借这条代码路径确保
		 * pg_largeobject 及其索引与旧集簇中具有相同的
		 * relfilenumber。这是必需的，因为 pg_upgrade
		 * 把 pg_largeobject 当作用户表而非系统表对待。
		 * 不过，表或索引有可能需要在新集簇中保有其
		 * 旧集簇中的同一 relfilenumber。
		 * 因此，我们不能等到提交时才移除旧存储。
		 *
		 * 大体上，本函数需要具备事务性语义，
		 * 而提交前就移除旧存储显然不具备。
		 * 不过没关系，若二进制升级在此阶段失败，
		 * 新集簇反正都需要重
		 * 建。
		 */
		srel = smgropen(relation->rd_locator, relation->rd_backend);
		smgrdounlinkall(&srel, 1, false);
		smgrclose(srel);
	}
	else
	{
		/* 非二进制升级，故只安排延后处理。 */
		RelationDropStorage(relation);
	}

	/*
	 * 为新 relfilenumber 的主 fork 创建存储。若对象
	 * 类似表，则调用表 AM 来做，它还会按需创建该表
	 * 的 init fork。
	 *
	 * 注意：若对相关 AM 适用，relfilenumber 值的任何冲突
	 * 都会在此被捕获，倘若 GetNewRelFileNumber 失手。
	 */
	newrlocator = relation->rd_locator;
	newrlocator.relNumber = newrelfilenumber;

	if (RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind))
	{
		table_relation_set_new_filelocator(relation, &newrlocator,
										   persistence,
										   &freezeXid, &minmulti);
	}
	else if (RELKIND_HAS_STORAGE(relation->rd_rel->relkind))
	{
		/* 至少目前直接处理这些 */
		SMgrRelation srel;

		srel = RelationCreateStorage(newrlocator, persistence, true);
		smgrclose(srel);
	}
	else
	{
		/* 其余情况不该调用到我们 */
		elog(ERROR, "relation \"%s\" does not have storage",
			 RelationGetRelationName(relation));
	}

	/*
	 * 若处理的是映射索引，pg_class.relfilenode 不会
	 * 改变；此时须把更新发送给关系映射器。
	 *
	 * 对映射索引而言，我们实际完全不改动 pg_class
	 * 条目；这在重建 pg_class 自身索引时至关重要。
	 * 这会留下可能不准的 relpages 等值，稍后自会修
	 * 正。
	 */
	if (RelationIsMapped(relation))
	{
		/* 仅索引支持此情形 */
		Assert(relation->rd_rel->relkind == RELKIND_INDEX);

		/* 由于不更新 pg_class，这些值最好别变 */
		Assert(classform->relfrozenxid == freezeXid);
		Assert(classform->relminmxid == minmulti);
		Assert(classform->relpersistence == persistence);

		/*
		 * 某些代码路径下，此处本应做的元组更新
		 * 可能正是为当前事务指派 XID 的唯一契机。不过
		 * 删除文件必须有 XID，因此要确保
		 * 指派了一个。
		 */
		(void) GetCurrentTransactionId();

		/* 动手干 */
		RelationMapUpdateMap(RelationGetRelid(relation),
							 newrelfilenumber,
							 relation->rd_rel->relisshared,
							 false);

		/* 由于不更新 pg_class，须手动触发失效 */
		CacheInvalidateRelcache(relation);
	}
	else
	{
		/* 常规情形，更新 pg_class 条目 */
		classform->relfilenode = newrelfilenumber;

		/* 序列的 relpages 等从不变化 */
		if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
		{
			classform->relpages = 0;	/* 在另行通知之前它为空 */
			classform->reltuples = -1;
			classform->relallvisible = 0;
		}
		classform->relfrozenxid = freezeXid;
		classform->relminmxid = minmulti;
		classform->relpersistence = persistence;

		CatalogTupleUpdate(pg_class, &otid, tuple);
	}

	UnlockTuple(pg_class, &otid, InplaceUpdateTupleLock);
	heap_freetuple(tuple);

	table_close(pg_class, RowExclusiveLock);

	/*
	 * 使 pg_class 行变更或关系映射变更可见。这也会
	 * 使 relcache 条目得到更新。
	 */
	CommandCounterIncrement();

	RelationAssumeNewRelfilelocator(relation);
}

/*
 * RelationAssumeNewRelfilelocator
 *
 * 凡是修改 pg_class.reltablespace 或 pg_class.relfilenode 的代码
 * 都必须调用本函数。调用应先于任何可能插入 WAL
 * 记录、且重放会修改新 RelFileLocator 字节的代码，
 * 并后于修改先前 RelFileLocator 字节的任何 WAL。参见
 * struct RelationData。理想情况下，应尽量靠近使
 * pg_class 变更可见的 CommandCounterIncrement()
 * 调用它（之前或之后均可）；这能最大限度减少未来
 * 开发在两函数之间添加违禁 WAL 的几率。
 */
void
RelationAssumeNewRelfilelocator(Relation relation)
{
	relation->rd_newRelfilelocatorSubid = GetCurrentSubTransactionId();
	if (relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)
		relation->rd_firstRelfilelocatorSubid = relation->rd_newRelfilelocatorSubid;

	/* 标记关系需要事务结束清理（清这些字段） */
	EOXactListAdd(relation);
}


/*
 *		RelationCacheInitialize
 *
 *		本函数初始化关系描述符缓存。在被调用时，
 *		尚不能访问数据库（主要因为事务子系统尚未
 *		就绪）；我们只是在建一张空的缓存哈希表。
 *		这必须在初始化事务启动之前完成，否则
 *		一旦该事务中止，AtEOXact_RelationCache
 *		就会在 relcache 尚未建立的情况下
 *		发生崩溃。
 */

#define INITRELCACHESIZE		400

void
RelationCacheInitialize(void)
{
	HASHCTL		ctl;
	int			allocsize;

	/*
	 * 确保缓存内存上下文已存在
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/*
	 * 建立为 relcache 建索引的哈希表
	 */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelIdCacheEnt);
	RelationIdCache = hash_create("Relcache by OID", INITRELCACHESIZE,
								  &ctl, HASH_ELEM | HASH_BLOBS);

	/*
	 * 为多数情况预留 in_progress_list 槽位
	 */
	allocsize = 4;
	in_progress_list =
		MemoryContextAlloc(CacheMemoryContext,
						   allocsize * sizeof(*in_progress_list));
	in_progress_list_maxlen = allocsize;

	/*
	 * 关系映射器也需要先初始化
	 */
	RelationMapInitialize();
}

/*
 *		RelationCacheInitializePhase2
 *
 *		本函数为启动期间访问共享目录做准备。我们至少要
 *		为 pg_database、pg_authid、pg_auth_members 和
 *		pg_shseclabel 建立钉住的 reldesc。理想情况下还
 *		要为它们的索引建立 reldesc。我们尝试从共享
 *		relcache 初始化文件加载该信息。若缺失或损坏，
 *		就只为目录本身建假条目。
 *		RelationCacheInitializePhase3 会按需清理。
 */
void
RelationCacheInitializePhase2(void)
{
	MemoryContext oldcxt;

	/*
	 * 关系映射器也需要初始化
	 */
	RelationMapInitializePhase2();

	/*
	 * 引导模式下共享目录反正还不存在，不用管
	 * 它。
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 切换到缓存内存上下文
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 尝试加载共享 relcache 缓存文件。若不成功，则用
	 * 为关键共享目录预制的描述符来引导缓存。
	 */
	if (!load_relcache_init_file(true))
	{
		formrdesc("pg_database", DatabaseRelation_Rowtype_Id, true,
				  Natts_pg_database, Desc_pg_database);
		formrdesc("pg_authid", AuthIdRelation_Rowtype_Id, true,
				  Natts_pg_authid, Desc_pg_authid);
		formrdesc("pg_auth_members", AuthMemRelation_Rowtype_Id, true,
				  Natts_pg_auth_members, Desc_pg_auth_members);
		formrdesc("pg_shseclabel", SharedSecLabelRelation_Rowtype_Id, true,
				  Natts_pg_shseclabel, Desc_pg_shseclabel);
		formrdesc("pg_subscription", SubscriptionRelation_Rowtype_Id, true,
				  Natts_pg_subscription, Desc_pg_subscription);

#define NUM_CRITICAL_SHARED_RELS	5	/* 清单有变须同步修正 */
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 *		RelationCacheInitializePhase3
 *
 *		本函数在 catcache 与事务系统就绪并确定
 *		MyDatabaseId 后尽快调用。此时我们才真正能
 *		从数据库的系统目录读取数据。我们首先尝试
 *		从本地 relcache 初始化文件读取预计算好的
 *		relcache 条目。若缺失或损坏，就为最少的
 *		钉在缓存中的关系建假条目。然后（除非正在
 *		引导）确保我们握有关键系统索引的条目。
 *		全部完成后，我们就有足够的基础设施来打开
 *		任何系统目录或使用任何 catcache。最后一步是
 *		按需重写缓存文件。
 */
void
RelationCacheInitializePhase3(void)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	MemoryContext oldcxt;
	bool		needNewCacheFile = !criticalSharedRelcachesBuilt;

	/*
	 * 关系映射器也需要初始化
	 */
	RelationMapInitializePhase3();

	/*
	 * 切换到缓存内存上下文
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * 尝试加载本地 relcache 缓存文件。若不成功，则用
	 * 为关键"钉入"系统目录预制的描述符来引导
	 * 缓存。
	 */
	if (IsBootstrapProcessingMode() ||
		!load_relcache_init_file(false))
	{
		needNewCacheFile = true;

		formrdesc("pg_class", RelationRelation_Rowtype_Id, false,
				  Natts_pg_class, Desc_pg_class);
		formrdesc("pg_attribute", AttributeRelation_Rowtype_Id, false,
				  Natts_pg_attribute, Desc_pg_attribute);
		formrdesc("pg_proc", ProcedureRelation_Rowtype_Id, false,
				  Natts_pg_proc, Desc_pg_proc);
		formrdesc("pg_type", TypeRelation_Rowtype_Id, false,
				  Natts_pg_type, Desc_pg_type);

#define NUM_CRITICAL_LOCAL_RELS 4	/* 清单有变须同步修正 */
	}

	MemoryContextSwitchTo(oldcxt);

	/* 引导模式下伪造的 formrdesc 信息即全部家当 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 若关键系统索引尚未装入 relcache，现在就装。
	 * 这些索引之所以关键，是因为 relcache 加载期间
	 * 的取用要依赖 catcache 和/或 opclass 缓存。因此
	 * 我们面临无限递归问题。破解递归的办法是在
	 * 若干关键位置改用堆扫描而非索引扫描。为免拖
	 * 累性能，我们只想在关键索引装入 relcache 之前
	 * 这么做。因此用标志 criticalRelcachesBuilt 来
	 * 决定关键位置做堆扫描还是索引扫描，并在关键
	 * 索引装入后把它置为真。
	 *
	 * 关键索引被标为"钉在缓存中"，一方面方便
	 * load_relcache_init_file 对它们计数，但主要是
	 * 因为一旦把 criticalRelcachesBuilt 置为真，就
	 * 无法再刷掉并重建它们。（注意：也许能
	 * 将 criticalRelcachesBuilt 重新置假来重载它们。
	 * 眼下我们只钉死。）
	 *
	 * RewriteRelRulenameIndexId 与 TriggerRelidNameIndexId
	 * 并不像其他索引那样关键，因为关键目录（目前）
	 * 没有任何规则或触发器，这些索引可以重建而
	 * 不引发递归。不过当关系真有规则或触发器时，
	 * relcache 加载期间会用到它们，所以我们出于性
	 * 能原因也把它们钉住。
	 */
	if (!criticalRelcachesBuilt)
	{
		load_critical_index(ClassOidIndexId,
							RelationRelationId);
		load_critical_index(AttributeRelidNumIndexId,
							AttributeRelationId);
		load_critical_index(IndexRelidIndexId,
							IndexRelationId);
		load_critical_index(OpclassOidIndexId,
							OperatorClassRelationId);
		load_critical_index(AccessMethodProcedureIndexId,
							AccessMethodProcedureRelationId);
		load_critical_index(RewriteRelRulenameIndexId,
							RewriteRelationId);
		load_critical_index(TriggerRelidNameIndexId,
							TriggerRelationId);

#define NUM_CRITICAL_LOCAL_INDEXES	7	/* 清单有变须同步修正 */

		criticalRelcachesBuilt = true;
	}

	/*
	 * 同时处理关键共享索引。
	 *
	 * DatabaseNameIndexId 对 relcache 加载并不关键，
	 * 关键的是用它初查 MyDatabaseId；找不到它就
	 * 根本找不到任何非共享目录。autovacuum 带着
	 * 数据库 OID 调用 InitPostgres，故它转而依赖
	 * DatabaseOidIndexId。我们还需要为客户端认证
	 * 期间的使用而钉住 pg_authid 与 pg_auth_members 上
	 * 的一些索引。SharedSecLabelObjectIndexId 对核
	 * 心非必需。
	 */
	if (!criticalSharedRelcachesBuilt)
	{
		load_critical_index(DatabaseNameIndexId,
							DatabaseRelationId);
		load_critical_index(DatabaseOidIndexId,
							DatabaseRelationId);
		load_critical_index(AuthIdRolnameIndexId,
							AuthIdRelationId);
		load_critical_index(AuthIdOidIndexId,
							AuthIdRelationId);
		load_critical_index(AuthMemMemRoleIndexId,
							AuthMemRelationId);
		load_critical_index(SharedSecLabelObjectIndexId,
							SharedSecLabelRelationId);

#define NUM_CRITICAL_SHARED_INDEXES 6	/* 清单有变须同步修正 */

		criticalSharedRelcachesBuilt = true;
	}

	/*
	 * 现在扫描所有 relcache 条目，修正 formrdesc 或
	 * relcache 缓存文件结果中可能不对的地方。若我们
	 * 曾用 formrdesc 伪造 relcache 条目，就读取真实
	 * pg_class 行并以其替换假条目。此外若任何
	 * relcache 条目带有规则、触发器或安全策略，就
	 * 用费力的办法加载该信息，因为缓存文件
	 *
	 * 不记录。每当我们访问目录读取数据时，都有可能
	 * 发生共享失效缓存刷写，导致 relcache 条目被
	 * 移除。hash_seq_search 只保证在*当前*条目被
	 * 移除后仍能继续工作，因此其后继续哈希表扫描
	 * 并不安全。我们的对策是每次访问后都从头重启
	 * 扫描。理论上这是 O(N^2)，但实际需要修正的条
	 * 目数量少得足以忽略不计。
	 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;
		bool		restart = false;

		/*
		 * 确保处理期间*this*条目不被刷掉。
		 */
		RelationIncrementReferenceCount(relation);

		/*
		 * 若是伪造条目，就读取真实 pg_class 元组。
		 */
		if (relation->rd_rel->relowner == InvalidOid)
		{
			HeapTuple	htup;
			Form_pg_class relp;

			htup = SearchSysCache1(RELOID,
								   ObjectIdGetDatum(RelationGetRelid(relation)));
			if (!HeapTupleIsValid(htup))
				elog(FATAL, "cache lookup failed for relation %u",
					 RelationGetRelid(relation));
			relp = (Form_pg_class) GETSTRUCT(htup);

			/*
			 * 元组复制到 relation->rd_rel，参见
			 * AllocateRelationDesc()
			 */
			memcpy((char *) relation->rd_rel, (char *) relp, CLASS_TUPLE_SIZE);

			/* 趁手里还有元组时更新 rd_options */
			if (relation->rd_options)
				pfree(relation->rd_options);
			RelationParseRelOptions(relation, htup);

			/*
			 * 检查 rd_att 中的值是否设置正确。（我们现在
			 * 无法直接照搬：formrdesc 必须一开始就把
			 * rd_att 数据设置正确，因为它可能已经被复制
			 * 进一或多个 catcache 条目。）
			 */
			Assert(relation->rd_att->tdtypeid == relp->reltype);
			Assert(relation->rd_att->tdtypmod == -1);

			ReleaseSysCache(htup);

			/* 此时 relowner 最好已正常，否则会死循环 */
			if (relation->rd_rel->relowner == InvalidOid)
				elog(ERROR, "invalid relowner in pg_class entry for \"%s\"",
					 RelationGetRelationName(relation));

			restart = true;
		}

		/*
		 * 修正未存入 relcache 缓存文件的数据。
		 *
		 * relhasrules 或 relhastriggers 可能出错或过
		 * 期。若确实没找到任何规则或触发器，就清掉
		 * 标志的本地副本，以免在此陷入无限循环。不
		 * 过我们并不打算去修 pg_class 条目。
		 */
		if (relation->rd_rel->relhasrules && relation->rd_rules == NULL)
		{
			RelationBuildRuleLock(relation);
			if (relation->rd_rules == NULL)
				relation->rd_rel->relhasrules = false;
			restart = true;
		}
		if (relation->rd_rel->relhastriggers && relation->trigdesc == NULL)
		{
			RelationBuildTriggers(relation);
			if (relation->trigdesc == NULL)
				relation->rd_rel->relhastriggers = false;
			restart = true;
		}

		/*
		 * 若关系带有行安全策略则重新加载，因为缓存中
		 * 不保留它们。注意 relrowsecurity 为真时绝不
		 * 会没有策略，
		 * RelationBuildRowSecurity 会创建一个默认拒绝
		 * 策略（pg_policy 未定义策略时）。
		 */
		if (relation->rd_rel->relrowsecurity && relation->rd_rsdesc == NULL)
		{
			RelationBuildRowSecurity(relation);

			Assert(relation->rd_rsdesc != NULL);
			restart = true;
		}

		/* 按需重载 tableam 数据 */
		if (relation->rd_tableam == NULL &&
			(RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind) || relation->rd_rel->relkind == RELKIND_SEQUENCE))
		{
			RelationInitTableAccessMethod(relation);
			Assert(relation->rd_tableam != NULL);

			restart = true;
		}

		/* 释放对该关系的持有 */
		RelationDecrementReferenceCount(relation);

		/* 现在按需重启哈希表扫描 */
		if (restart)
		{
			hash_seq_term(&status);
			hash_seq_init(&status, RelationIdCache);
		}
	}

	/*
	 * 最后按需写出新的 relcache 缓存文件。我们不费心
	 * 区分两者中只有一个需要更新的情形。
	 */
	if (needNewCacheFile)
	{
		/*
		 * 强令所有 catcache 完成初始化，从而打开它们
		 * 所用到的目录与索引。这会为所有最重要的系统
		 * 目录和索引预装 relcache 条目，使初始化文件对
		 * 未来的后端最为有用。
		 */
		InitCatalogCachePhase2();

		/* 现在写出文件 */
		write_relcache_init_file(true);
		write_relcache_init_file(false);
	}
}

/*
 * 向 relcache 装入一个关键系统索引
 *
 * indexoid 是目标索引的 OID，heapoid 是其所属目录
 * 的 OID。
 */
static void
load_critical_index(Oid indexoid, Oid heapoid)
{
	Relation	ird;

	/*
	 * 为避免死锁，必须先锁底层目录再锁索引，因为
	 * RelationBuildDesc 很可能需要读取该目录，而若
	 * 别人正以排他锁锁着该目录和索引，他们必然按
	 * 同一顺序加锁。
	 */
	LockRelationOid(heapoid, AccessShareLock);
	LockRelationOid(indexoid, AccessShareLock);
	ird = RelationBuildDesc(indexoid, true);
	if (ird == NULL)
		elog(PANIC, "could not open critical system index %u", indexoid);
	ird->rd_isnailed = true;
	ird->rd_refcnt = 1;
	UnlockRelationOid(indexoid, AccessShareLock);
	UnlockRelationOid(heapoid, AccessShareLock);

	(void) RelationGetIndexAttOptions(ird, false);
}

/*
 * GetPgClassDescriptor -- 取 pg_class 预定义元组描述符
 * GetPgIndexDescriptor -- 取 pg_index 预定义的元组描述符
 *
 * 我们需要这个权宜之法，因为在标准目录缓存可用
 * 之前，就得能访问 pg_class 与 pg_index 的非定
 * 长字段。我们使用的预定义数据，其建立方式与
 * formrdesc() 所用的引导 reldesc 完全一致。得到的
 * tupdesc 并非 100% 正统：tdtypeid 中没有正确的
 * rowtype OID，也没有 TupleConstr 字段。但用于
 * 提取足够。
 */
static TupleDesc
BuildHardcodedDescriptor(int natts, const FormData_pg_attribute *attrs)
{
	TupleDesc	result;
	MemoryContext oldcxt;
	int			i;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	result = CreateTemplateTupleDesc(natts);
	result->tdtypeid = RECORDOID;	/* 不正确，但无所谓 */
	result->tdtypmod = -1;

	for (i = 0; i < natts; i++)
	{
		memcpy(TupleDescAttr(result, i), &attrs[i], ATTRIBUTE_FIXED_PART_SIZE);
		/* 确保 attcacheoff 有效 */
		TupleDescAttr(result, i)->attcacheoff = -1;
	}

	/* 初始化首属性 attcacheoff，cf. RelationBuildTupleDesc */
	TupleDescAttr(result, 0)->attcacheoff = 0;

	/* 注意：我们不费心设置 TupleConstr 条目 */

	MemoryContextSwitchTo(oldcxt);

	return result;
}

static TupleDesc
GetPgClassDescriptor(void)
{
	static TupleDesc pgclassdesc = NULL;

	/* 已做完？ */
	if (pgclassdesc == NULL)
		pgclassdesc = BuildHardcodedDescriptor(Natts_pg_class,
											   Desc_pg_class);

	return pgclassdesc;
}

static TupleDesc
GetPgIndexDescriptor(void)
{
	static TupleDesc pgindexdesc = NULL;

	/* 已做完？ */
	if (pgindexdesc == NULL)
		pgindexdesc = BuildHardcodedDescriptor(Natts_pg_index,
											   Desc_pg_index);

	return pgindexdesc;
}

/*
 * 为关系加载任意默认属性值定义。
 *
 * ndef 是被标为 atthasdef 的属性个数。
 *
 * 注意：缺少一些 pg_attrdef 记录并不构成硬错误。
 * 只要没有东西需要用默认值，我们就能凑合前行。
 * 找不到预期 AttrDefault 记录的代码应当报错。
 */
static void
AttrDefaultFetch(Relation relation, int ndef)
{
	AttrDefault *attrdef;
	Relation	adrel;
	SysScanDesc adscan;
	ScanKeyData skey;
	HeapTuple	htup;
	int			found = 0;

	/* 按预期条目数分配带足空间的数组 */
	attrdef = (AttrDefault *)
		MemoryContextAllocZero(CacheMemoryContext,
							   ndef * sizeof(AttrDefault));

	/* 在 pg_attrdef 中找相关条目 */
	ScanKeyInit(&skey,
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	adrel = table_open(AttrDefaultRelationId, AccessShareLock);
	adscan = systable_beginscan(adrel, AttrDefaultIndexId, true,
								NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(adscan)))
	{
		Form_pg_attrdef adform = (Form_pg_attrdef) GETSTRUCT(htup);
		Datum		val;
		bool		isnull;

		/* 保护容量有限的数组 */
		if (found >= ndef)
		{
			elog(WARNING, "unexpected pg_attrdef record found for attribute %d of relation \"%s\"",
				 adform->adnum, RelationGetRelationName(relation));
			break;
		}

		val = fastgetattr(htup,
						  Anum_pg_attrdef_adbin,
						  adrel->rd_att, &isnull);
		if (isnull)
			elog(WARNING, "null adbin for attribute %d of relation \"%s\"",
				 adform->adnum, RelationGetRelationName(relation));
		else
		{
			/* 在调用方上下文中解 toast 转成 cstring */
			char	   *s = TextDatumGetCString(val);

			attrdef[found].adnum = adform->adnum;
			attrdef[found].adbin = MemoryContextStrdup(CacheMemoryContext, s);
			pfree(s);
			found++;
		}
	}

	systable_endscan(adscan);
	table_close(adrel, AccessShareLock);

	if (found != ndef)
		elog(WARNING, "%d pg_attrdef record(s) missing for relation \"%s\"",
			 ndef - found, RelationGetRelationName(relation));

	/*
	 * 为方便 equalTupleDescs()，按 adnum 排序
	 * AttrDefault 条目。（通常它们本就有序，但若
	 * systable_getnext 未走索引就未必如此。）
	 */
	if (found > 1)
		qsort(attrdef, found, sizeof(AttrDefault), AttrDefaultCmp);

	/* 待数组完全有效后才安装 */
	relation->rd_att->constr->defval = attrdef;
	relation->rd_att->constr->num_defval = found;
}

/*
 * 按 adnum 排 AttrDefault 条目的 qsort 比较器
 */
static int
AttrDefaultCmp(const void *a, const void *b)
{
	const AttrDefault *ada = (const AttrDefault *) a;
	const AttrDefault *adb = (const AttrDefault *) b;

	return ada->adnum - adb->adnum;
}

/*
 * 为关系加载任意检查约束。
 *
 * 与默认值一样，若找不到预期数量，此处只发警告。
 * 若此时尝试 INSERT/UPDATE，执行器应当报错。
 */
static void
CheckConstraintFetch(Relation relation)
{
	ConstrCheck *check;
	int			ncheck = relation->rd_rel->relchecks;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[1];
	HeapTuple	htup;
	int			found = 0;

	/* 按预期条目数分配带足空间的数组 */
	check = (ConstrCheck *)
		MemoryContextAllocZero(CacheMemoryContext,
							   ncheck * sizeof(ConstrCheck));

	/* 在 pg_constraint 中找相关条目 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	conrel = table_open(ConstraintRelationId, AccessShareLock);
	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 1, skey);

	while (HeapTupleIsValid(htup = systable_getnext(conscan)))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(htup);
		Datum		val;
		bool		isnull;

		/* 我们只要检查约束 */
		if (conform->contype != CONSTRAINT_CHECK)
			continue;

		/* 保护容量有限的数组 */
		if (found >= ncheck)
		{
			elog(WARNING, "unexpected pg_constraint record found for relation \"%s\"",
				 RelationGetRelationName(relation));
			break;
		}

		check[found].ccvalid = conform->convalidated;
		check[found].ccnoinherit = conform->connoinherit;
		check[found].ccname = MemoryContextStrdup(CacheMemoryContext,
												  NameStr(conform->conname));

		/* 取出并测试 conbin 确已设置 */
		val = fastgetattr(htup,
						  Anum_pg_constraint_conbin,
						  conrel->rd_att, &isnull);
		if (isnull)
			elog(WARNING, "null conbin for relation \"%s\"",
				 RelationGetRelationName(relation));
		else
		{
			/* 在调用方上下文中解 toast 转成 cstring */
			char	   *s = TextDatumGetCString(val);

			check[found].ccbin = MemoryContextStrdup(CacheMemoryContext, s);
			pfree(s);
			found++;
		}
	}

	systable_endscan(conscan);
	table_close(conrel, AccessShareLock);

	if (found != ncheck)
		elog(WARNING, "%d pg_constraint record(s) missing for relation \"%s\"",
			 ncheck - found, RelationGetRelationName(relation));

	/*
	 * 按名称排序记录。这确保 CHECK 以确定性的顺序
	 * 施加，也让 equalTupleDescs() 更快。
	 */
	if (found > 1)
		qsort(check, found, sizeof(ConstrCheck), CheckConstraintCmp);

	/* 待数组完全有效后才安装 */
	relation->rd_att->constr->check = check;
	relation->rd_att->constr->num_check = found;
}

/*
 * 按名称排 ConstrCheck 的 qsort 比较器
 */
static int
CheckConstraintCmp(const void *a, const void *b)
{
	const ConstrCheck *ca = (const ConstrCheck *) a;
	const ConstrCheck *cb = (const ConstrCheck *) b;

	return strcmp(ca->ccname, cb->ccname);
}

/*
 * RelationGetFKeyList -- 取该关系的外键信息清单
 *
 * 返回 ForeignKeyCacheInfo 结构的清单，每个约束
 * 该给定关系的外键各对应一个。数据直接复制自
 * pg_constraint 的相关字段。清单项次序任意。
 *
 * 当心：返回的清单是 relcache 数据的一部分，可能
 * 在 relcache 条目重置时消失。调用方必须在任何
 * 可能触发缓存刷写之举（如访问目录）之前
 * 检视或复制它。需要时可用 copyObject()。
 * （这样定义是因为眼下调用方反正要过滤并
 * 修改清单项，复制反而浪费时间。）
 */
List *
RelationGetFKeyList(Relation relation)
{
	List	   *result;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *oldlist;
	MemoryContext oldcxt;

	/* 已计算过该清单则快速退出。 */
	if (relation->rd_fkeyvalid)
		return relation->rd_fkeylist;

	/* 快速路径：无触发器的非分区表不可能有 FK */
	if (!relation->rd_rel->relhastriggers &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return NIL;

	/*
	 * 我们边扫描边在调用方上下文中构建要返回的清单，
	 * 该扫描成功完成后，将清单复制入 relcache 条目。
	 * 这样即使中途遇到某种错误，也不会泄漏
	 * 缓存上下文中的内存。
	 */
	result = NIL;

	/* 准备扫描 pg_constraint 中 conrelid 等于本关系的条目。 */
	ScanKeyInit(&skey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	conrel = table_open(ConstraintRelationId, AccessShareLock);
	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(conscan)))
	{
		Form_pg_constraint constraint = (Form_pg_constraint) GETSTRUCT(htup);
		ForeignKeyCacheInfo *info;

		/* 只考虑外键 */
		if (constraint->contype != CONSTRAINT_FOREIGN)
			continue;

		info = makeNode(ForeignKeyCacheInfo);
		info->conoid = constraint->oid;
		info->conrelid = constraint->conrelid;
		info->confrelid = constraint->confrelid;

		DeconstructFkConstraintRow(htup, &info->nkeys,
								   info->conkey,
								   info->confkey,
								   info->conpfeqop,
								   NULL, NULL, NULL, NULL);

		/* 把 FK 节点加入结果清单 */
		result = lappend(result, info);
	}

	systable_endscan(conscan);
	table_close(conrel, AccessShareLock);

	/* 现在把已完成清单的副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	oldlist = relation->rd_fkeylist;
	relation->rd_fkeylist = copyObject(result);
	relation->rd_fkeyvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* 若存在旧清单，勿泄漏之 */
	list_free_deep(oldlist);

	return result;
}

/*
 * RelationGetIndexList -- 取本关系索引的 OID 清单
 *
 * 只有有人请求时才创建索引清单。我们扫描 pg_index
 * 找出相关索引，并把清单加进 relcache 条目，从而
 * 无需再次计算。注意共享缓存失效会使 relcache
 * 条目删除旧清单并把 rd_indexvalid 置假，因此下
 * 次请求时必须重算索引清单。这涵盖了索引的创建
 * 与删除。
 *
 * 被标记为非 indislive 的索引会从返回清单中省略。
 * 这类索引预计马上就会被删除，本函数的任何调用
 * 方都完全不应碰它们。
 *
 * 返回清单保证按 OID 顺序排好。执行器需要它，
 * 因为对我们更新索引时要取排他锁的索引类型，
 * 所有后端必须按相同顺序锁索引，否则会死锁（
 * 参见 ExecOpenIndices()）。任何一致顺序都行，
 * 但按 OID 排序最简单。
 *
 * 由于共享缓存失效会使 relcache 持有的清单副本
 * 消失，我们返回一份在调用方上下文 palloc 的清单
 * 副本。调用方扫描完后可以 list_free() 它。这很
 * 必要，因为调用方通常还要对相关索引做 syscache
 * 查找，而 syscache 查找可能促使 SI 消息被处理！
 *
 * 同样地，我们也会更新 rd_pkindex（主键索引的
 * OID，若无则 InvalidOid）与 rd_replidindex（作
 * 为关系副本标识索引的 pg_class OID，无此索引时
 * 则为 InvalidOid）。
 */
List *
RelationGetIndexList(Relation relation)
{
	Relation	indrel;
	SysScanDesc indscan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result;
	List	   *oldlist;
	char		replident = relation->rd_rel->relreplident;
	Oid			pkeyIndex = InvalidOid;
	Oid			candidateIndex = InvalidOid;
	MemoryContext oldcxt;

	/* 已计算过该清单则快速退出。 */
	if (relation->rd_indexvalid)
		return list_copy(relation->rd_indexlist);

	/*
	 * 我们边扫描边在调用方上下文中构建要返回的清单，
	 * 该扫描成功完成后，将清单复制入 relcache 条目。
	 * 这样即使中途遇到某种错误，也不会泄漏
	 * 缓存上下文中的内存。
	 */
	result = NIL;

	/* 准备扫描 pg_index 中 indrelid 等于本关系的条目。 */
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	indrel = table_open(IndexRelationId, AccessShareLock);
	indscan = systable_beginscan(indrel, IndexIndrelidIndexId, true,
								 NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(indscan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(htup);

		/*
		 * 忽略正在被删除的索引。这能防止它们被搜索、
		 * 被插入或被纳入 HOT 安全性考量。这类索引完全
		 * 碰不得，因为其目录条目随时
		 * 可能消失。
		 */
		if (!index->indislive)
			continue;

		/* 索引 OID 加入结果清单 */
		result = lappend_oid(result, index->indexrelid);

		/*
		 * 无效、非唯一、非立即或带谓词索引对 OID 索引
		 * 和副本标识索引都无用，
		 * 故不检查。
		 */
		if (!index->indisvalid || !index->indisunique ||
			!index->indimmediate ||
			!heap_attisnull(htup, Anum_pg_index_indpred, NULL))
			continue;

		/* 有主键索引则记住它 */
		if (index->indisprimary)
			pkeyIndex = index->indexrelid;

		/* 记住显式选定的副本索引 */
		if (index->indisreplident)
			candidateIndex = index->indexrelid;
	}

	systable_endscan(indscan);

	table_close(indrel, AccessShareLock);

	/* 按接口规范将结果清单排成 OID 序。 */
	list_sort(result, list_oid_cmp);

	/* 现在把已完成清单的副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	oldlist = relation->rd_indexlist;
	relation->rd_indexlist = list_copy(result);
	relation->rd_pkindex = pkeyIndex;
	if (replident == REPLICA_IDENTITY_DEFAULT && OidIsValid(pkeyIndex))
		relation->rd_replidindex = pkeyIndex;
	else if (replident == REPLICA_IDENTITY_INDEX && OidIsValid(candidateIndex))
		relation->rd_replidindex = candidateIndex;
	else
		relation->rd_replidindex = InvalidOid;
	relation->rd_indexvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* 若存在旧清单，勿泄漏之 */
	list_free(oldlist);

	return result;
}

/*
 * RelationGetStatExtList
 *		取本关系统计对象的 OID 清单
 *
 * 统计对象清单也是只有有人请求才创建，方式与
 * RelationGetIndexList() 类似。我们扫描
 * pg_statistic_ext 找相关统计，并把清单加进
 * relcache 条目，从而无需再算。注意共享缓存失效
 * 会使 relcache 条目删除旧清单并把 rd_statvalid
 * 置 0，因此下次请求必须重算统计清单。这涵盖统
 * 计对象的创建与删除。
 *
 * 返回清单同样保证按 OID 排序，尽管当前并不需要
 * 这一点。
 *
 * 由于共享缓存失效会使 relcache 持有的清单副本
 * 消失，我们返回一份在调用方上下文 palloc 的清单
 * 副本。调用方扫描完后可以 list_free() 它。这很
 * 必要，因为调用方通常还要对相关统计做 syscache
 * 查找，而 syscache 查找可能促使 SI 消息被处理！
 */
List *
RelationGetStatExtList(Relation relation)
{
	Relation	indrel;
	SysScanDesc indscan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result;
	List	   *oldlist;
	MemoryContext oldcxt;

	/* 已计算过该清单则快速退出。 */
	if (relation->rd_statvalid != 0)
		return list_copy(relation->rd_statlist);

	/*
	 * 我们边扫描边在调用方上下文中构建要返回的清单，
	 * 该扫描成功完成后，将清单复制入 relcache 条目。
	 * 这样即使中途遇到某种错误，也不会泄漏
	 * 缓存上下文中的内存。
	 */
	result = NIL;

	/*
	 * 准备扫描 pg_statistic_ext 中 stxrelid 等于本
	 * rel.
	 */
	ScanKeyInit(&skey,
				Anum_pg_statistic_ext_stxrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	indrel = table_open(StatisticExtRelationId, AccessShareLock);
	indscan = systable_beginscan(indrel, StatisticExtRelidIndexId, true,
								 NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(indscan)))
	{
		Oid			oid = ((Form_pg_statistic_ext) GETSTRUCT(htup))->oid;

		result = lappend_oid(result, oid);
	}

	systable_endscan(indscan);

	table_close(indrel, AccessShareLock);

	/* 按接口规范将结果清单排成 OID 序。 */
	list_sort(result, list_oid_cmp);

	/* 现在把已完成清单的副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	oldlist = relation->rd_statlist;
	relation->rd_statlist = list_copy(result);

	relation->rd_statvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* 若存在旧清单，勿泄漏之 */
	list_free(oldlist);

	return result;
}

/*
 * RelationGetPrimaryKeyIndex -- 取关系主键索引的 OID
 *
 * 无则返回 InvalidOid。
 */
Oid
RelationGetPrimaryKeyIndex(Relation relation)
{
	List	   *ilist;

	if (!relation->rd_indexvalid)
	{
		/* 重活由 RelationGetIndexList 来干。 */
		ilist = RelationGetIndexList(relation);
		list_free(ilist);
		Assert(relation->rd_indexvalid);
	}

	return relation->rd_pkindex;
}

/*
 * RelationGetReplicaIndex -- 取关系副本标识索引的 OID
 *
 * 无则返回 InvalidOid。
 */
Oid
RelationGetReplicaIndex(Relation relation)
{
	List	   *ilist;

	if (!relation->rd_indexvalid)
	{
		/* 重活由 RelationGetIndexList 来干。 */
		ilist = RelationGetIndexList(relation);
		list_free(ilist);
		Assert(relation->rd_indexvalid);
	}

	return relation->rd_replidindex;
}

/*
 * RelationGetIndexExpressions -- 取索引的索引表达式
 *
 * 我们把 pg_index.indexprs 变换成节点树的结果
 * 缓存起来。若该关系不是索引或无表达式列，则返
 * 回 NIL。否则，返回的树会复制进调用方的内存上
 * 下文。（我们不想返回指向 relcache 副本的指针，
 * 随 relcache 失效而消失。）
 */
List *
RelationGetIndexExpressions(Relation relation)
{
	List	   *result;
	Datum		exprsDatum;
	bool		isnull;
	char	   *exprsString;
	MemoryContext oldcxt;

	/* 已算出结果则快速退出。 */
	if (relation->rd_indexprs)
		return copyObject(relation->rd_indexprs);

	/* 无事可做则快速退出。 */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indexprs, NULL))
		return NIL;

	/*
	 * 我们会先在调用方上下文中构建打算返回的树。
	 * 工作成功完成后再把它复制进 relcache 条目。
	 * 这样即使中途遇到某种错误也不会出问题。
	 */
	exprsDatum = heap_getattr(relation->rd_indextuple,
							  Anum_pg_index_indexprs,
							  GetPgIndexDescriptor(),
							  &isnull);
	Assert(!isnull);
	exprsString = TextDatumGetCString(exprsDatum);
	result = (List *) stringToNode(exprsString);
	pfree(exprsString);

	/*
	 * 把表达式送过 eval_const_expressions 处理。这不
	 * 只是优化，而是必需：规划器会把它们与同样处理
	 * 过的 qual 子句比较，没有这一步可能检测不到有
	 * 效匹配。但我们绝不能用 canonicalize_qual，
	 * 因为这些不是 qual 表达式。
	 */
	result = (List *) eval_const_expressions(NULL, (Node *) result);

	/* 不妨也修正 opfuncids */
	fix_opfuncids((Node *) result);

	/* 现在把已完成树的副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_indexprs = copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * RelationGetDummyIndexExpressions -- 取索引的哑
 *
 * 表达式清单（只是 Const 节点），其类型/typmod/
 * 排序规则与索引真实表达式一致。在不想运行任何
 * 用户自定义代码的场合很有用。
 */
List *
RelationGetDummyIndexExpressions(Relation relation)
{
	List	   *result;
	Datum		exprsDatum;
	bool		isnull;
	char	   *exprsString;
	List	   *rawExprs;
	ListCell   *lc;

	/* 无事可做则快速退出。 */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indexprs, NULL))
		return NIL;

	/* 从索引元组提取原始节点树。 */
	exprsDatum = heap_getattr(relation->rd_indextuple,
							  Anum_pg_index_indexprs,
							  GetPgIndexDescriptor(),
							  &isnull);
	Assert(!isnull);
	exprsString = TextDatumGetCString(exprsDatum);
	rawExprs = (List *) stringToNode(exprsString);
	pfree(exprsString);

	/* 构造空 Const；typlen 与 typbyval 随意。 */
	result = NIL;
	foreach(lc, rawExprs)
	{
		Node	   *rawExpr = (Node *) lfirst(lc);

		result = lappend(result,
						 makeConst(exprType(rawExpr),
								   exprTypmod(rawExpr),
								   exprCollation(rawExpr),
								   1,
								   (Datum) 0,
								   true,
								   true));
	}

	return result;
}

/*
 * RelationGetIndexPredicate -- 取索引的索引谓词
 *
 * 我们把 pg_index.indpred 变换成隐式 AND 节点树
 * （供规划用）的结果被缓存。
 * 若该关系非索引或无谓词，则返回 NIL。否则，
 * 返回的树会复制进调用方的内存上下文。（我们不
 * 想返回指向 relcache 副本的指针，它可能随
 * relcache 失效而消失。）
 */
List *
RelationGetIndexPredicate(Relation relation)
{
	List	   *result;
	Datum		predDatum;
	bool		isnull;
	char	   *predString;
	MemoryContext oldcxt;

	/* 已算出结果则快速退出。 */
	if (relation->rd_indpred)
		return copyObject(relation->rd_indpred);

	/* 无事可做则快速退出。 */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indpred, NULL))
		return NIL;

	/*
	 * 我们会先在调用方上下文中构建打算返回的树。
	 * 工作成功完成后再把它复制进 relcache 条目。
	 * 这样即使中途遇到某种错误也不会出问题。
	 */
	predDatum = heap_getattr(relation->rd_indextuple,
							 Anum_pg_index_indpred,
							 GetPgIndexDescriptor(),
							 &isnull);
	Assert(!isnull);
	predString = TextDatumGetCString(predDatum);
	result = (List *) stringToNode(predString);
	pfree(predString);

	/*
	 * 把表达式送过常量简化与规范化处理。这不只是
	 * 优化，而是必需：规划器会把它与同样处理过的
	 * qual 子句比较，没有这一步可能检测不到有效
	 * 匹配。这必须与 preprocess_expression() 对
	 * qual 子句所做的处理一致！（不过索引谓词中
	 * 不允许出现子查询，因此子查询相关部分可
	 * 跳过。）
	 */
	result = (List *) eval_const_expressions(NULL, (Node *) result);

	result = (List *) canonicalize_qual((Expr *) result, false);

	/* 并转换成隐式 AND 格式 */
	result = make_ands_implicit((Expr *) result);

	/* 不妨也修正 opfuncids */
	fix_opfuncids((Node *) result);

	/* 现在把已完成树的副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_indpred = copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * RelationGetIndexAttrBitmap -- 取索引属性号位图
 *
 * 结果中，本关系全部索引定义里用到的每个属性
 * 都有一位置位。（这不仅涵盖简单索引键，
 * 还涵盖用在表达式与部分索引谓词中的属
 * 性。）
 *
 * 依据 attrKind，返回的位图会为特定列的 attnum
 * 置位：
 *	INDEX_ATTR_BITMAP_KEY		非部分唯一索引中不在表达式内
 *									的列（即可用于 FK）
 *	INDEX_ATTR_BITMAP_PRIMARY_KEY	表的主键中的列
 *	INDEX_ATTR_BITMAP_IDENTITY_KEY	表的副本标识
 *									索引，FULL 则空
 *	INDEX_ATTR_BITMAP_HOT_BLOCKING	阻止更新成为 HOT 更新的列
 *	INDEX_ATTR_BITMAP_SUMMARIZED	包含在汇总索引中的列
 *
 * 属性号偏移了 FirstLowInvalidHeapAttributeNumber，
 * 以便位图表示中能纳入系统属性（如 OID）。
 *
 * 调用方最好至少持有目标关系上的 RowExclusiveLock，
 * 以确保我们对其索引加锁是安全的（无死锁）。注意
 * 自 CREATE INDEX CONCURRENTLY 引入以来，该锁级
 * 并不保证索引集合稳定，所以若索引集合有变，我们
 * 必须准备好在此重试。
 *
 * 返回结果 palloc 于调用方的内存上下文，不再需要时
 * 应当 bms_free 释放。
 */
Bitmapset *
RelationGetIndexAttrBitmap(Relation relation, IndexAttrBitmapKind attrKind)
{
	Bitmapset  *uindexattrs;	/* 唯一索引中的列 */
	Bitmapset  *pkindexattrs;	/* 主索引中的列 */
	Bitmapset  *idindexattrs;	/* 副本标识中的列 */
	Bitmapset  *hotblockingattrs;	/* 阻碍 HOT 的索引所在列 */
	Bitmapset  *summarizedattrs;	/* 汇总索引中的列 */
	List	   *indexoidlist;
	List	   *newindexoidlist;
	Oid			relpkindex;
	Oid			relreplindex;
	ListCell   *l;
	MemoryContext oldcxt;

	/* 已算出结果则快速退出。 */
	if (relation->rd_attrsvalid)
	{
		switch (attrKind)
		{
			case INDEX_ATTR_BITMAP_KEY:
				return bms_copy(relation->rd_keyattr);
			case INDEX_ATTR_BITMAP_PRIMARY_KEY:
				return bms_copy(relation->rd_pkattr);
			case INDEX_ATTR_BITMAP_IDENTITY_KEY:
				return bms_copy(relation->rd_idattr);
			case INDEX_ATTR_BITMAP_HOT_BLOCKING:
				return bms_copy(relation->rd_hotblockingattr);
			case INDEX_ATTR_BITMAP_SUMMARIZED:
				return bms_copy(relation->rd_summarizedattr);
			default:
				elog(ERROR, "unknown attrKind %u", attrKind);
		}
	}

	/* 确定无索引时走快速路径 */
	if (!RelationGetForm(relation)->relhasindex)
		return NULL;

	/*
	 * 取索引 OID 缓存清单。若须重来，便在此处重来。
	 */
restart:
	indexoidlist = RelationGetIndexList(relation);

	/* 无索引则跳出（但 relhasindex 置位） */
	if (indexoidlist == NIL)
		return NULL;

	/*
	 * 先复制 RelationGetIndexList 算出的 rd_pkindex
	 * 与 rd_replidindex 值再继续。这样做是因为下面
	 * index_open 内部可能发生 relcache 刷写，重置
	 * RelationGetIndexList 管理的字段。我们需要用这
	 * 些字段的稳定值。
	 */
	relpkindex = relation->rd_pkindex;
	relreplindex = relation->rd_replidindex;

	/*
	 * 对每个索引，把其引用属性加入 indexattrs。
	 *
	 * 注意：我们考虑 RelationGetIndexList 返回的所有
	 * 索引，即使它们非 indisready 或 indisvalid。这
	 * 很重要，因为 CREATE INDEX CONCURRENTLY 刚启动
	 * 的索引必须纳入 HOT 安全性考量（见 README.HOT）。
	 * 若 DROP INDEX CONCURRENTLY 已进行到该忽略此索
	 * 引时，它根本不会被返回。
	 */
	uindexattrs = NULL;
	pkindexattrs = NULL;
	idindexattrs = NULL;
	hotblockingattrs = NULL;
	summarizedattrs = NULL;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		Datum		datum;
		bool		isnull;
		Node	   *indexExpressions;
		Node	   *indexPredicate;
		int			i;
		bool		isKey;		/* 候选键 */
		bool		isPK;		/* 主键 */
		bool		isIDKey;	/* 副本标识索引 */
		Bitmapset **attrs;

		indexDesc = index_open(indexOid, AccessShareLock);

		/*
		 * 提取索引表达式与索引谓词。注意：不要使用
		 * RelationGetIndexExpressions()/RelationGetIndexPredicate()，因为
		 * 它们可能运行常量表达式求值，那需要快照，
		 * 而我们此处未必有快照。（此外，先在任何可能
		 * 消除列的变换之前收集位图，多半也更稳妥，不
		 * 过这一点的实际影响有限。）
		 */

		datum = heap_getattr(indexDesc->rd_indextuple, Anum_pg_index_indexprs,
							 GetPgIndexDescriptor(), &isnull);
		if (!isnull)
			indexExpressions = stringToNode(TextDatumGetCString(datum));
		else
			indexExpressions = NULL;

		datum = heap_getattr(indexDesc->rd_indextuple, Anum_pg_index_indpred,
							 GetPgIndexDescriptor(), &isnull);
		if (!isnull)
			indexPredicate = stringToNode(TextDatumGetCString(datum));
		else
			indexPredicate = NULL;

		/* 此索引能否被外键引用？ */
		isKey = indexDesc->rd_index->indisunique &&
			indexExpressions == NULL &&
			indexPredicate == NULL;

		/* 这是主键吗？ */
		isPK = (indexOid == relpkindex);

		/* 此索引是不是配置的（或默认）副本标识？ */
		isIDKey = (indexOid == relreplindex);

		/*
		 * 若索引是汇总型的，它不阻碍 HOT 更新（但属性
		 * 有改动时仍需更新它）。所以要在下面的循环
		 * 中决定我们将更新哪个位图。
		 */
		if (indexDesc->rd_indam->amsummarizing)
			attrs = &summarizedattrs;
		else
			attrs = &hotblockingattrs;

		/* 收集简单属性引用 */
		for (i = 0; i < indexDesc->rd_index->indnatts; i++)
		{
			int			attrnum = indexDesc->rd_index->indkey.values[i];

			/*
			 * 由于我们拥有带非键列的覆盖索引，必须在此
			 * 准确处理它们。非键列必须加进 hotblockingattrs
			 * 或 summarizedattrs，因为它们在索引里，而且更
			 * 新不应漏掉它们。
			 *
			 * 汇总索引不阻碍 HOT，但列值变化时
			 * 确实需要更新，因而要有单独的属性
			 * bitmapset。
			 *
			 * 显然非键列不可能被外键或标识键引用，因此
			 * 我们不加进 uindexattrs、pkindexattrs 和
			 * idindexattrs 位图。
			 */
			if (attrnum != 0)
			{
				*attrs = bms_add_member(*attrs,
										attrnum - FirstLowInvalidHeapAttributeNumber);

				if (isKey && i < indexDesc->rd_index->indnkeyatts)
					uindexattrs = bms_add_member(uindexattrs,
												 attrnum - FirstLowInvalidHeapAttributeNumber);

				if (isPK && i < indexDesc->rd_index->indnkeyatts)
					pkindexattrs = bms_add_member(pkindexattrs,
												  attrnum - FirstLowInvalidHeapAttributeNumber);

				if (isIDKey && i < indexDesc->rd_index->indnkeyatts)
					idindexattrs = bms_add_member(idindexattrs,
												  attrnum - FirstLowInvalidHeapAttributeNumber);
			}
		}

		/* 也收集表达式中用到的全部属性 */
		pull_varattnos(indexExpressions, 1, attrs);

		/* 也收集索引谓词中的全部属性 */
		pull_varattnos(indexPredicate, 1, attrs);

		index_close(indexDesc, AccessShareLock);
	}

	/*
	 * 在上面循环的某次 index_open 期间，我们可能收到
	 * 针对本 relcache 条目的刷写事件，它可能正在通知
	 * 关系的索引清单有变。若然，我们最好重来一遍，
	 * 以确保交付的属性位图是最新的。
	 */
	newindexoidlist = RelationGetIndexList(relation);
	if (equal(indexoidlist, newindexoidlist) &&
		relpkindex == relation->rd_pkindex &&
		relreplindex == relation->rd_replidindex)
	{
		/* 索引集合未变，继续即可 */
		list_free(newindexoidlist);
		list_free(indexoidlist);
	}
	else
	{
		/* 得重干一场 …… 顺便别泄漏内存 */
		list_free(newindexoidlist);
		list_free(indexoidlist);
		bms_free(uindexattrs);
		bms_free(pkindexattrs);
		bms_free(idindexattrs);
		bms_free(hotblockingattrs);
		bms_free(summarizedattrs);

		goto restart;
	}

	/* 若有旧值，勿泄漏这些位图 */
	relation->rd_attrsvalid = false;
	bms_free(relation->rd_keyattr);
	relation->rd_keyattr = NULL;
	bms_free(relation->rd_pkattr);
	relation->rd_pkattr = NULL;
	bms_free(relation->rd_idattr);
	relation->rd_idattr = NULL;
	bms_free(relation->rd_hotblockingattr);
	relation->rd_hotblockingattr = NULL;
	bms_free(relation->rd_summarizedattr);
	relation->rd_summarizedattr = NULL;

	/*
	 * 现在把位图副本存入 relcache 条目。我们有意把
	 * rd_attrsvalid 放到最后设置，因为发出这些值有
	 * 效信号的是它；若在复制前内存耗尽，我们就不会
	 * 让条目看起来其他值有效却
	 * 空。
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_keyattr = bms_copy(uindexattrs);
	relation->rd_pkattr = bms_copy(pkindexattrs);
	relation->rd_idattr = bms_copy(idindexattrs);
	relation->rd_hotblockingattr = bms_copy(hotblockingattrs);
	relation->rd_summarizedattr = bms_copy(summarizedattrs);
	relation->rd_attrsvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* 把我们原本的工作副本交由调用方使用 */
	switch (attrKind)
	{
		case INDEX_ATTR_BITMAP_KEY:
			return uindexattrs;
		case INDEX_ATTR_BITMAP_PRIMARY_KEY:
			return pkindexattrs;
		case INDEX_ATTR_BITMAP_IDENTITY_KEY:
			return idindexattrs;
		case INDEX_ATTR_BITMAP_HOT_BLOCKING:
			return hotblockingattrs;
		case INDEX_ATTR_BITMAP_SUMMARIZED:
			return summarizedattrs;
		default:
			elog(ERROR, "unknown attrKind %u", attrKind);
			return NULL;
	}
}

/*
 * RelationGetIdentityKeyBitmap -- 取副本标识属性号
 * 位图
 *
 * 返回的是已配置副本标识索引的索引
 * 属性号位图。
 *
 * 参见 RelationGetIndexAttrBitmap() 的注释。
 *
 * 这是逻辑复制期间使用的专用函数。此处与
 * RelationGetIndexAttrBitmap() 不同，我们并不对所
 * 需索引加锁，因为我们用历史快照构建缓存条目，
 * 后续所有变更会在解码 WAL 时被吸收。正因如此，
 * 索引集合有变时无需在此重试。
 */
Bitmapset *
RelationGetIdentityKeyBitmap(Relation relation)
{
	Bitmapset  *idindexattrs = NULL;	/* 副本标识中的列 */
	Relation	indexDesc;
	int			i;
	Oid			replidindex;
	MemoryContext oldcxt;

	/* 已算出结果则快速退出 */
	if (relation->rd_idattr != NULL)
		return bms_copy(relation->rd_idattr);

	/* 确定无索引时走快速路径 */
	if (!RelationGetForm(relation)->relhasindex)
		return NULL;

	/* 必须已设置历史快照。 */
	Assert(HistoricSnapshotActive());

	replidindex = RelationGetReplicaIndex(relation);

	/* 无副本标识索引则跳出 */
	if (!OidIsValid(replidindex))
		return NULL;

	/* 查阅副本标识索引的描述符 */
	indexDesc = RelationIdGetRelation(replidindex);

	if (!RelationIsValid(indexDesc))
		elog(ERROR, "could not open relation with OID %u",
			 relation->rd_replidindex);

	/* 把引用的属性加入 idindexattrs */
	for (i = 0; i < indexDesc->rd_index->indnatts; i++)
	{
		int			attrnum = indexDesc->rd_index->indkey.values[i];

		/*
		 * 我们不给 idindexattrs 位图纳入非键列。参见
		 * RelationGetIndexAttrBitmap
		 */
		if (attrnum != 0)
		{
			if (i < indexDesc->rd_index->indnkeyatts)
				idindexattrs = bms_add_member(idindexattrs,
											  attrnum - FirstLowInvalidHeapAttributeNumber);
		}
	}

	RelationClose(indexDesc);

	/* 若有旧值，勿泄漏这些位图 */
	bms_free(relation->rd_idattr);
	relation->rd_idattr = NULL;

	/* 现在把位图副本存入 relcache 条目 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_idattr = bms_copy(idindexattrs);
	MemoryContextSwitchTo(oldcxt);

	/* 把我们原本的工作副本交由调用方使用 */
	return idindexattrs;
}

/*
 * RelationGetExclusionInfo -- 取索引排除约束的信息
 *
 * 只应对已知带有排除约束的索引调用本函数。它
 * 会返回若干数组（palloc 于调用方上下文）：排除
 * 算子 OID、底层函数 OID 及其在索引 opclass 中
 * 的策略号。这些信息获取工作量不小，因此我们
 * 将其全部缓存起来。
 */
void
RelationGetExclusionInfo(Relation indexRelation,
						 Oid **operators,
						 Oid **procs,
						 uint16 **strategies)
{
	int			indnkeyatts;
	Oid		   *ops;
	Oid		   *funcs;
	uint16	   *strats;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[1];
	HeapTuple	htup;
	bool		found;
	MemoryContext oldcxt;
	int			i;

	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(indexRelation);

	/* 调用方上下文中分配结果空间 */
	*operators = ops = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	*procs = funcs = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	*strategies = strats = (uint16 *) palloc(sizeof(uint16) * indnkeyatts);

	/* 若数据已缓存则快速退出 */
	if (indexRelation->rd_exclstrats != NULL)
	{
		memcpy(ops, indexRelation->rd_exclops, sizeof(Oid) * indnkeyatts);
		memcpy(funcs, indexRelation->rd_exclprocs, sizeof(Oid) * indnkeyatts);
		memcpy(strats, indexRelation->rd_exclstrats, sizeof(uint16) * indnkeyatts);
		return;
	}

	/*
	 * 在 pg_constraint 中搜索与该索引相关的约束。
	 * 为免太慢，我们用 conrelid 上的索引；其中存的是
	 * 父关系的 OID 而非索引自身的 OID。
	 *
	 * 注意：如果想依赖约束名与索引名一致这一点，就
	 * 可以直接用 pg_constraint 的唯一索引做查找。目
	 * 前还不值得强制要求这一点。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexRelation->rd_index->indrelid));

	conrel = table_open(ConstraintRelationId, AccessShareLock);
	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 1, skey);
	found = false;

	while (HeapTupleIsValid(htup = systable_getnext(conscan)))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(htup);
		Datum		val;
		bool		isnull;
		ArrayType  *arr;
		int			nelem;

		/* 我们要的是拥有该索引的排除约束 */
		if (conform->contype != CONSTRAINT_EXCLUSION ||
			conform->conindid != RelationGetRelid(indexRelation))
			continue;

		/* 应当只有一条 */
		if (found)
			elog(ERROR, "unexpected exclusion constraint record found for rel %s",
				 RelationGetRelationName(indexRelation));
		found = true;

		/* 从 conexclop 提取算子 OID */
		val = fastgetattr(htup,
						  Anum_pg_constraint_conexclop,
						  conrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "null conexclop for rel %s",
				 RelationGetRelationName(indexRelation));

		arr = DatumGetArrayTypeP(val);	/* 确保未经 toast */
		nelem = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			nelem != indnkeyatts ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conexclop is not a 1-D Oid array");

		memcpy(ops, ARR_DATA_PTR(arr), sizeof(Oid) * indnkeyatts);
	}

	systable_endscan(conscan);
	table_close(conrel, AccessShareLock);

	if (!found)
		elog(ERROR, "exclusion constraint record missing for rel %s",
			 RelationGetRelationName(indexRelation));

	/* 我们还需要函数 OID 与策略号 */
	for (i = 0; i < indnkeyatts; i++)
	{
		funcs[i] = get_opcode(ops[i]);
		strats[i] = get_op_opfamily_strategy(ops[i],
											 indexRelation->rd_opfamily[i]);
		/* 不应失败，建索引时已检查过 */
		if (strats[i] == InvalidStrategy)
			elog(ERROR, "could not find strategy for operator %u in family %u",
				 ops[i], indexRelation->rd_opfamily[i]);
	}

	/* 把结果副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(indexRelation->rd_indexcxt);
	indexRelation->rd_exclops = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	indexRelation->rd_exclprocs = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	indexRelation->rd_exclstrats = (uint16 *) palloc(sizeof(uint16) * indnkeyatts);
	memcpy(indexRelation->rd_exclops, ops, sizeof(Oid) * indnkeyatts);
	memcpy(indexRelation->rd_exclprocs, funcs, sizeof(Oid) * indnkeyatts);
	memcpy(indexRelation->rd_exclstrats, strats, sizeof(uint16) * indnkeyatts);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * 取给定关系的发布信息。
 *
 * 遍历该关系所在的所有发布，取其发布动作，并
 * 在有的情况下校验这些发布的行过滤器表达式。
 * 若表达式引用了任何不属于 REPLICA IDENTITY 的列，
 * 我们就视该行过滤器表达式为无效。
 *
 * 为免反复取发布信息，我们把发布动作与行过滤
 * 器校验信息缓存起来。
 */
void
RelationBuildPublicationDesc(Relation relation, PublicationDesc *pubdesc)
{
	List	   *puboids;
	ListCell   *lc;
	MemoryContext oldcxt;
	Oid			schemaid;
	List	   *ancestors = NIL;
	Oid			relid = RelationGetRelid(relation);

	/*
	 * 若不可发布，则不发布任何动作（pgoutput_change() 会
	 * 忽略）
	 */
	if (!is_publishable_relation(relation))
	{
		memset(pubdesc, 0, sizeof(PublicationDesc));
		pubdesc->rf_valid_for_update = true;
		pubdesc->rf_valid_for_delete = true;
		pubdesc->cols_valid_for_update = true;
		pubdesc->cols_valid_for_delete = true;
		return;
	}

	if (relation->rd_pubdesc)
	{
		memcpy(pubdesc, relation->rd_pubdesc, sizeof(PublicationDesc));
		return;
	}

	memset(pubdesc, 0, sizeof(PublicationDesc));
	pubdesc->rf_valid_for_update = true;
	pubdesc->rf_valid_for_delete = true;
	pubdesc->cols_valid_for_update = true;
	pubdesc->cols_valid_for_delete = true;

	/* 取发布成员关系信息。 */
	puboids = GetRelationPublications(relid);
	schemaid = RelationGetNamespace(relation);
	puboids = list_concat_unique_oid(puboids, GetSchemaPublications(schemaid));

	if (relation->rd_rel->relispartition)
	{
		/* 把祖先所在的发布也加进来。 */
		ancestors = get_partition_ancestors(relid);

		foreach(lc, ancestors)
		{
			Oid			ancestor = lfirst_oid(lc);

			puboids = list_concat_unique_oid(puboids,
											 GetRelationPublications(ancestor));
			schemaid = get_rel_namespace(ancestor);
			puboids = list_concat_unique_oid(puboids,
											 GetSchemaPublications(schemaid));
		}
	}
	puboids = list_concat_unique_oid(puboids, GetAllTablesPublications());

	foreach(lc, puboids)
	{
		Oid			pubid = lfirst_oid(lc);
		HeapTuple	tup;
		Form_pg_publication pubform;

		tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for publication %u", pubid);

		pubform = (Form_pg_publication) GETSTRUCT(tup);

		pubdesc->pubactions.pubinsert |= pubform->pubinsert;
		pubdesc->pubactions.pubupdate |= pubform->pubupdate;
		pubdesc->pubactions.pubdelete |= pubform->pubdelete;
		pubdesc->pubactions.pubtruncate |= pubform->pubtruncate;

		/*
		 * 检查过滤表达式引用的列是否都属于 REPLICA
		 *  IDENTITY 索引。
		 *
		 * 若发布是 FOR ALL TABLES，则意味着表没有行过
		 * 滤器，可跳过校验。
		 */
		if (!pubform->puballtables &&
			(pubform->pubupdate || pubform->pubdelete) &&
			pub_rf_contains_invalid_column(pubid, relation, ancestors,
										   pubform->pubviaroot))
		{
			if (pubform->pubupdate)
				pubdesc->rf_valid_for_update = false;
			if (pubform->pubdelete)
				pubdesc->rf_valid_for_delete = false;
		}

		/*
		 * 检查所有列是否都属于 REPLICA IDENTITY 索引。
		 *
		 * 若发布是 FOR ALL TABLES，则意味着表没有列清
		 * 单，可跳过校验。
		 */
		if (!pubform->puballtables &&
			(pubform->pubupdate || pubform->pubdelete) &&
			pub_collist_contains_invalid_column(pubid, relation, ancestors,
												pubform->pubviaroot))
		{
			if (pubform->pubupdate)
				pubdesc->cols_valid_for_update = false;
			if (pubform->pubdelete)
				pubdesc->cols_valid_for_delete = false;
		}

		ReleaseSysCache(tup);

		/*
		 * 若已知全部复制，且行过滤器对更新与删除
		 * 无效，再检查其他发布已无
		 * 意义。
		 */
		if (pubdesc->pubactions.pubinsert && pubdesc->pubactions.pubupdate &&
			pubdesc->pubactions.pubdelete && pubdesc->pubactions.pubtruncate &&
			!pubdesc->rf_valid_for_update && !pubdesc->rf_valid_for_delete)
			break;

		/*
		 * 若已知全部复制，且列清单对更新与删除无效，
		 * 再检查其他发布已无
		 * 意义。
		 */
		if (pubdesc->pubactions.pubinsert && pubdesc->pubactions.pubupdate &&
			pubdesc->pubactions.pubdelete && pubdesc->pubactions.pubtruncate &&
			!pubdesc->cols_valid_for_update && !pubdesc->cols_valid_for_delete)
			break;
	}

	if (relation->rd_pubdesc)
	{
		pfree(relation->rd_pubdesc);
		relation->rd_pubdesc = NULL;
	}

	/* 现在把描述符副本存入 relcache 条目。 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_pubdesc = palloc(sizeof(PublicationDesc));
	memcpy(relation->rd_pubdesc, pubdesc, sizeof(PublicationDesc));
	MemoryContextSwitchTo(oldcxt);
}

/*
 * RelationGetIndexRawAttOptions -- 取索引的 AM/opclass 专属选项
 */
Datum *
RelationGetIndexRawAttOptions(Relation indexrel)
{
	Oid			indexrelid = RelationGetRelid(indexrel);
	int16		natts = RelationGetNumberOfAttributes(indexrel);
	Datum	   *options = NULL;
	int16		attnum;

	for (attnum = 1; attnum <= natts; attnum++)
	{
		if (indexrel->rd_indam->amoptsprocnum == 0)
			continue;

		if (!OidIsValid(index_getprocid(indexrel, attnum,
										indexrel->rd_indam->amoptsprocnum)))
			continue;

		if (!options)
			options = palloc0(sizeof(Datum) * natts);

		options[attnum - 1] = get_attoptions(indexrelid, attnum);
	}

	return options;
}

static bytea **
CopyIndexAttOptions(bytea **srcopts, int natts)
{
	bytea	  **opts = palloc(sizeof(*opts) * natts);

	for (int i = 0; i < natts; i++)
	{
		bytea	   *opt = srcopts[i];

		opts[i] = !opt ? NULL : (bytea *)
			DatumGetPointer(datumCopy(PointerGetDatum(opt), false, -1));
	}

	return opts;
}

/*
 * RelationGetIndexAttOptions
 *		取索引的 AM/opclass 专属选项，解析成二进制形式
 */
bytea	  **
RelationGetIndexAttOptions(Relation relation, bool copy)
{
	MemoryContext oldcxt;
	bytea	  **opts = relation->rd_opcoptions;
	Oid			relid = RelationGetRelid(relation);
	int			natts = RelationGetNumberOfAttributes(relation);	/* XXX
																	 * IndexRelationGetNumberOfKeyAttributes */
	int			i;

	/* 尝试复制缓存选项。 */
	if (opts)
		return copy ? CopyIndexAttOptions(opts, natts) : opts;

	/* 取并解析 opclass 选项。 */
	opts = palloc0(sizeof(*opts) * natts);

	for (i = 0; i < natts; i++)
	{
		if (criticalRelcachesBuilt && relid != AttributeRelidNumIndexId)
		{
			Datum		attoptions = get_attoptions(relid, i + 1);

			opts[i] = index_opclass_options(relation, i + 1, attoptions, false);

			if (attoptions != (Datum) 0)
				pfree(DatumGetPointer(attoptions));
		}
	}

	/* 把解析结果复制进缓存。 */
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_opcoptions = CopyIndexAttOptions(opts, natts);
	MemoryContextSwitchTo(oldcxt);

	if (copy)
		return opts;

	for (i = 0; i < natts; i++)
	{
		if (opts[i])
			pfree(opts[i]);
	}

	pfree(opts);

	return relation->rd_opcoptions;
}

/*
 * 支撑 ereport() 关系相关报错的例程
 *
 * 这些本可放进 elog.c，但让 elog.c 调用 relcache
 * 或 syscache 的东西像是模块分层违规 -- 而且我们
 * 绝不想让 elog.h 包含 rel.h，故放在这里。
 */

/*
 * errtable --- 在当前 errordata 中记下某表的
 * schema_name 与 table_name。
 */
int
errtable(Relation rel)
{
	err_generic_string(PG_DIAG_SCHEMA_NAME,
					   get_namespace_name(RelationGetNamespace(rel)));
	err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(rel));

	return 0;					/* 返回值无关紧要 */
}

/*
 * errtablecol --- 在当前 errordata 中记下某表列的
 * schema_name、table_name 与 column_name。
 *
 * 列由属性号指定 -- 对多数调用方而言，这比自
 * 己去取列名更省事也更不易出错。
 */
int
errtablecol(Relation rel, int attnum)
{
	TupleDesc	reldesc = RelationGetDescr(rel);
	const char *colname;

	/* 若是用户属性则用 reldesc，否则查系统目录 */
	if (attnum > 0 && attnum <= reldesc->natts)
		colname = NameStr(TupleDescAttr(reldesc, attnum - 1)->attname);
	else
		colname = get_attname(RelationGetRelid(rel), attnum, false);

	return errtablecolname(rel, colname);
}

/*
 * errtablecolname --- 在当前 errordata 中记下某表
 * 列的 schema_name、table_name 与 column_name，
 * 其中列名是直接给出而非取自关系的目录数据。
 *
 * 除非 errtablecol() 有不便之处，否则不要直接
 * 使用本函数。比如在 ALTER TABLE 的中间状态
 * 时可能需要。
 */
int
errtablecolname(Relation rel, const char *colname)
{
	errtable(rel);
	err_generic_string(PG_DIAG_COLUMN_NAME, colname);

	return 0;					/* 返回值无关紧要 */
}

/*
 * errtableconstraint --- 在当前 errordata 中记下表相关约束的
 * schema_name、table_name 与 constraint_name。
 */
int
errtableconstraint(Relation rel, const char *conname)
{
	errtable(rel);
	err_generic_string(PG_DIAG_CONSTRAINT_NAME, conname);

	return 0;					/* 返回值无关紧要 */
}


/*
 *	load_relcache_init_file, write_relcache_init_file
 *
 *		1992 年末起，我们开始常规性地见到包含上千个类
 *		（classes）的数据库。随着这一数量，对系统目录做
 *		索引查找变得至关重要。
 *
 *		引导这些查找非常困难。比如想在 pg_attribute
 *		上建索引，但要做到这一点，必须先读出该索引
 *		涉及的属性的 pg_attribute，这意味着又需要用
 *		这个索引。
 *
 *		为绕开这个问题，我们采取如下做法：
 *
 *		   + 数据库系统初始化时（initdb 期间），我们不
 *			  使用索引，而是做顺序扫描。
 *
 *		   + 后端以正常模式启动时，我们从 data/base/...
 *			  目录下的初始化文件里，以内部格式加载相应
 *			  关系描述符的映像。
 *
 *		   + 若初始化文件不在，就用顺序扫描创建
 *			  关系描述符，并把它们写入
 *			  初始化文件供后续后端使用。
 *
 *		自 Postgres 9.0 起，每个数据库有一个本地初始
 *		化文件，外加供共享目录用的共享初始化文件。
 *
 *		我们本可弃用初始化文件，每次后端启动都用笨
 *		办法构建关键 reldesc，但这会明显
 *		拖慢后端启动速度。
 *
 *		我们其实可更进一步，保存比绝对关键条目更
 *		多的 relcache 条目；这可免去笨办法构建，从而
 *		加快后端启动速度。
 *		目前，凡被 catcache 引用的目录与索引条目都
 *		存进初始化文件。
 *
 *		检测 catcache 与 relcache 条目何时该失效（因
 *		目录更新）的机制，也会安排在内容可能过时时
 *		解除初始化文件的链接。之后文件会在下次后端
 *		启动期间重建。
 */

/*
 * load_relcache_init_file -- 尝试装入缓存：从共享
 * 或本地初始化文件
 *
 * 成功时返回真，并把 criticalRelcachesBuilt 或
 * criticalSharedRelcachesBuilt 置真
 * 不成功则返回假。
 *
 * 注意：我们假定已经切换到 CacheMemoryContext 中。
 */
static bool
load_relcache_init_file(bool shared)
{
	FILE	   *fp;
	char		initfilename[MAXPGPATH];
	Relation   *rels;
	int			relno,
				num_rels,
				max_rels,
				nailed_rels,
				nailed_indexes,
				magic;
	int			i;

	if (shared)
		snprintf(initfilename, sizeof(initfilename), "global/%s",
				 RELCACHE_INIT_FILENAME);
	else
		snprintf(initfilename, sizeof(initfilename), "%s/%s",
				 DatabasePath, RELCACHE_INIT_FILENAME);

	fp = AllocateFile(initfilename, PG_BINARY_R);
	if (fp == NULL)
		return false;

	/*
	 * 从文件读取索引 relcache 条目。注意若中途读取失败，
	 * 就不会把其中任何条目存入缓存；这有助于防范
	 * 损坏的初始化文件。
	 */
	max_rels = 100;
	rels = (Relation *) palloc(max_rels * sizeof(Relation));
	num_rels = 0;
	nailed_rels = nailed_indexes = 0;

	/* 检查魔数是否正确（版本兼容） */
	if (fread(&magic, 1, sizeof(magic), fp) != sizeof(magic))
		goto read_failed;
	if (magic != RELCACHE_INIT_FILEMAGIC)
		goto read_failed;

	for (relno = 0;; relno++)
	{
		Size		len;
		size_t		nread;
		Relation	rel;
		Form_pg_class relform;
		bool		has_not_null;

		/* 先读关系描述符长度 */
		nread = fread(&len, 1, sizeof(len), fp);
		if (nread != sizeof(len))
		{
			if (nread == 0)
				break;			/* 文件尾 */
			goto read_failed;
		}

		/* 对不兼容 relcache 布局做安全检查 */
		if (len != sizeof(RelationData))
			goto read_failed;

		/* 再分配一个 relcache 头部 */
		if (num_rels >= max_rels)
		{
			max_rels *= 2;
			rels = (Relation *) repalloc(rels, max_rels * sizeof(Relation));
		}

		rel = rels[num_rels++] = (Relation) palloc(len);

		/* 接着读取 Relation 结构 */
		if (fread(rel, 1, len, fp) != len)
			goto read_failed;

		/* 接着读关系元组形态 */
		if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
			goto read_failed;

		relform = (Form_pg_class) palloc(len);
		if (fread(relform, 1, len, fp) != len)
			goto read_failed;

		rel->rd_rel = relform;

		/* 初始化属性元组形态 */
		rel->rd_att = CreateTemplateTupleDesc(relform->relnatts);
		rel->rd_att->tdrefcount = 1;	/* 引用计数标记 */

		rel->rd_att->tdtypeid = relform->reltype ? relform->reltype : RECORDOID;
		rel->rd_att->tdtypmod = -1; /* 谨慎起见 */

		/* 接着读全部属性元组形态数据条目 */
		has_not_null = false;
		for (i = 0; i < relform->relnatts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(rel->rd_att, i);

			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;
			if (len != ATTRIBUTE_FIXED_PART_SIZE)
				goto read_failed;
			if (fread(attr, 1, len, fp) != len)
				goto read_failed;

			has_not_null |= attr->attnotnull;
		}

		/* 接着读访问方法专属字段 */
		if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
			goto read_failed;
		if (len > 0)
		{
			rel->rd_options = palloc(len);
			if (fread(rel->rd_options, 1, len, fp) != len)
				goto read_failed;
			if (len != VARSIZE(rel->rd_options))
				goto read_failed;	/* 健全检查 */
		}
		else
		{
			rel->rd_options = NULL;
		}

		/* 标记非空状态 */
		if (has_not_null)
		{
			TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

			constr->has_not_null = true;
			rel->rd_att->constr = constr;
		}

		/*
		 * 若是索引则还有事要做。注意此处显式忽略
		 * 分区索引。
		 */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			MemoryContext indexcxt;
			Oid		   *opfamily;
			Oid		   *opcintype;
			RegProcedure *support;
			int			nsupport;
			int16	   *indoption;
			Oid		   *indcollation;

			/* 统计钉住索引以确保一个不漏 */
			if (rel->rd_isnailed)
				nailed_indexes++;

			/* 读 pg_index 元组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			rel->rd_indextuple = (HeapTuple) palloc(len);
			if (fread(rel->rd_indextuple, 1, len, fp) != len)
				goto read_failed;

			/* 修正元组内部指针，参见 heap_copytuple */
			rel->rd_indextuple->t_data = (HeapTupleHeader) ((char *) rel->rd_indextuple + HEAPTUPLESIZE);
			rel->rd_index = (Form_pg_index) GETSTRUCT(rel->rd_indextuple);

			/*
			 * 准备索引信息上下文 --- 参数应一致，见
			 * RelationInitIndexAccessInfo
			 */
			indexcxt = AllocSetContextCreate(CacheMemoryContext,
											 "index info",
											 ALLOCSET_SMALL_SIZES);
			rel->rd_indexcxt = indexcxt;
			MemoryContextCopyAndSetIdentifier(indexcxt,
											  RelationGetRelationName(rel));

			/*
			 * 现在可以取索引 AM 的 API 结构了。（它不能存
			 * 入初始化文件，因为其中函数指针可能随服务器
			 * 运行而异。好在即使在引导索引期间调用
			 * amhandler 也是安全的。）
			 */
			InitIndexAmRoutine(rel);

			/* 读取 opfamily OID 数组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			opfamily = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(opfamily, 1, len, fp) != len)
				goto read_failed;

			rel->rd_opfamily = opfamily;

			/* 读取 opcintype OID 数组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			opcintype = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(opcintype, 1, len, fp) != len)
				goto read_failed;

			rel->rd_opcintype = opcintype;

			/* 读取支持过程 OID 数组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;
			support = (RegProcedure *) MemoryContextAlloc(indexcxt, len);
			if (fread(support, 1, len, fp) != len)
				goto read_failed;

			rel->rd_support = support;

			/* 读取排序规则 OID 数组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			indcollation = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(indcollation, 1, len, fp) != len)
				goto read_failed;

			rel->rd_indcollation = indcollation;

			/* 读取 indoption 值数组 */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			indoption = (int16 *) MemoryContextAlloc(indexcxt, len);
			if (fread(indoption, 1, len, fp) != len)
				goto read_failed;

			rel->rd_indoption = indoption;

			/* 读取 opcoptions 值数组 */
			rel->rd_opcoptions = (bytea **)
				MemoryContextAllocZero(indexcxt, sizeof(*rel->rd_opcoptions) * relform->relnatts);

			for (i = 0; i < relform->relnatts; i++)
			{
				if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
					goto read_failed;

				if (len > 0)
				{
					rel->rd_opcoptions[i] = (bytea *) MemoryContextAlloc(indexcxt, len);
					if (fread(rel->rd_opcoptions[i], 1, len, fp) != len)
						goto read_failed;
				}
			}

			/* 建立清零 fmgr-info 数组 */
			nsupport = relform->relnatts * rel->rd_indam->amsupport;
			rel->rd_supportinfo = (FmgrInfo *)
				MemoryContextAllocZero(indexcxt, nsupport * sizeof(FmgrInfo));
		}
		else
		{
			/* 统计钉住关系以确保一个不漏 */
			if (rel->rd_isnailed)
				nailed_rels++;

			/* 载表 AM 数据 */
			if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind) || rel->rd_rel->relkind == RELKIND_SEQUENCE)
				RelationInitTableAccessMethod(rel);

			Assert(rel->rd_index == NULL);
			Assert(rel->rd_indextuple == NULL);
			Assert(rel->rd_indexcxt == NULL);
			Assert(rel->rd_indam == NULL);
			Assert(rel->rd_opfamily == NULL);
			Assert(rel->rd_opcintype == NULL);
			Assert(rel->rd_support == NULL);
			Assert(rel->rd_supportinfo == NULL);
			Assert(rel->rd_indoption == NULL);
			Assert(rel->rd_indcollation == NULL);
			Assert(rel->rd_opcoptions == NULL);
		}

		/*
		 * 规则与触发器不保存（主要因为内部格式复杂
		 * 且可能变化）。如 RelationCacheInitializePhase3
		 * 需要就必须重建。这不预期成为大的性能瓶颈，
		 * 因为少有系统目录带这些。RLS 策略数据、分区
		 * 信息、索引表达式、谓词、排除约束信息与 FDW
		 * 信息亦然。
		 */
		rel->rd_rules = NULL;
		rel->rd_rulescxt = NULL;
		rel->trigdesc = NULL;
		rel->rd_rsdesc = NULL;
		rel->rd_partkey = NULL;
		rel->rd_partkeycxt = NULL;
		rel->rd_partdesc = NULL;
		rel->rd_partdesc_nodetached = NULL;
		rel->rd_partdesc_nodetached_xmin = InvalidTransactionId;
		rel->rd_pdcxt = NULL;
		rel->rd_pddcxt = NULL;
		rel->rd_partcheck = NIL;
		rel->rd_partcheckvalid = false;
		rel->rd_partcheckcxt = NULL;
		rel->rd_indexprs = NIL;
		rel->rd_indpred = NIL;
		rel->rd_exclops = NULL;
		rel->rd_exclprocs = NULL;
		rel->rd_exclstrats = NULL;
		rel->rd_fdwroutine = NULL;

		/*
		 * 重置 relcache 条目中的瞬态字段
		 */
		rel->rd_smgr = NULL;
		if (rel->rd_isnailed)
			rel->rd_refcnt = 1;
		else
			rel->rd_refcnt = 0;
		rel->rd_indexvalid = false;
		rel->rd_indexlist = NIL;
		rel->rd_pkindex = InvalidOid;
		rel->rd_replidindex = InvalidOid;
		rel->rd_attrsvalid = false;
		rel->rd_keyattr = NULL;
		rel->rd_pkattr = NULL;
		rel->rd_idattr = NULL;
		rel->rd_pubdesc = NULL;
		rel->rd_statvalid = false;
		rel->rd_statlist = NIL;
		rel->rd_fkeyvalid = false;
		rel->rd_fkeylist = NIL;
		rel->rd_createSubid = InvalidSubTransactionId;
		rel->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
		rel->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
		rel->rd_droppedSubid = InvalidSubTransactionId;
		rel->rd_amcache = NULL;
		rel->pgstat_info = NULL;

		/*
		 * 重算锁与物理寻址信息。这是需要的，以防
		 * pg_internal.init 文件经 CREATE DATABASE 从其他
		 * 库复制而来。
		 */
		RelationInitLockInfo(rel);
		RelationInitPhysicalAddr(rel);
	}

	/*
	 * 我们走到初始化文件末尾，未见明显问题。钉住
	 * 条目数对了吗？这是有用的交叉检查，以防关键
	 * 关系或索引集合发生变化。不过正常运行的系统不应
	 * 出现这种情况，所以真发生了就发牢骚提示。
	 *
	 * 对共享初始化文件而言，本函数在客户端认证完成前
	 * 被调用，这意味着 elog(WARNING) 只会进入 postmaster
	 * 日志，很容易被错过。为确保开发者注意到不当的
	 * NUM_CRITICAL_SHARED_RELS/NUM_CRITICAL_SHARED_INDEXES 值，我们
	 * 放了 Assert(false)。
	 */
	if (shared)
	{
		if (nailed_rels != NUM_CRITICAL_SHARED_RELS ||
			nailed_indexes != NUM_CRITICAL_SHARED_INDEXES)
		{
			elog(WARNING, "found %d nailed shared rels and %d nailed shared indexes in init file, but expected %d and %d respectively",
				 nailed_rels, nailed_indexes,
				 NUM_CRITICAL_SHARED_RELS, NUM_CRITICAL_SHARED_INDEXES);
			/* 确保开发者注意到此事 */
			Assert(false);
			/* 生产构建中靠引导 relcache 恢复 */
			goto read_failed;
		}
	}
	else
	{
		if (nailed_rels != NUM_CRITICAL_LOCAL_RELS ||
			nailed_indexes != NUM_CRITICAL_LOCAL_INDEXES)
		{
			elog(WARNING, "found %d nailed rels and %d nailed indexes in init file, but expected %d and %d respectively",
				 nailed_rels, nailed_indexes,
				 NUM_CRITICAL_LOCAL_RELS, NUM_CRITICAL_LOCAL_INDEXES);
			/* 此情形无需 Assert() */
			goto read_failed;
		}
	}

	/*
	 * 好，一切正常。
	 *
	 * 现在把所有新 relcache 条目插入缓存。
	 */
	for (relno = 0; relno < num_rels; relno++)
	{
		RelationCacheInsert(rels[relno], false);
	}

	pfree(rels);
	FreeFile(fp);

	if (shared)
		criticalSharedRelcachesBuilt = true;
	else
		criticalRelcachesBuilt = true;
	return true;

	/*
	 * 初始化文件已坏，只好走笨办法。无需费心释放刚
	 * 分配的杂物；它们不在 relcache 里，所以
	 * 无碍。
	 */
read_failed:
	pfree(rels);
	FreeFile(fp);

	return false;
}

/*
 * 以 relcache 当前内容写出新的初始化文件（按
 * 指示为共享关系或本地关系）。
 */
static void
write_relcache_init_file(bool shared)
{
	FILE	   *fp;
	char		tempfilename[MAXPGPATH];
	char		finalfilename[MAXPGPATH];
	int			magic;
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	int			i;

	/*
	 * 如果已经收到过任何 relcache 失效事件，就没有
	 * 成功的机会了，不妨干脆放弃整件事。
	 */
	if (relcacheInvalsReceived != 0L)
		return;

	/*
	 * 我们必须先写临时文件再改名到位。否则，几乎同
	 * 时启动的另一后端可能因读到未写完的文件而崩
	 * 溃。
	 */
	if (shared)
	{
		snprintf(tempfilename, sizeof(tempfilename), "global/%s.%d",
				 RELCACHE_INIT_FILENAME, MyProcPid);
		snprintf(finalfilename, sizeof(finalfilename), "global/%s",
				 RELCACHE_INIT_FILENAME);
	}
	else
	{
		snprintf(tempfilename, sizeof(tempfilename), "%s/%s.%d",
				 DatabasePath, RELCACHE_INIT_FILENAME, MyProcPid);
		snprintf(finalfilename, sizeof(finalfilename), "%s/%s",
				 DatabasePath, RELCACHE_INIT_FILENAME);
	}

	unlink(tempfilename);		/* 以防它已存在且权限不对 */

	fp = AllocateFile(tempfilename, PG_BINARY_W);
	if (fp == NULL)
	{
		/*
		 * 我们过去把这当作致命错误，但其实不妨继续
		 * 后端启动流程 ……
		 */
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not create relation-cache initialization file \"%s\": %m",
						tempfilename),
				 errdetail("Continuing anyway, but there's something wrong.")));
		return;
	}

	/*
	 * 写一个魔数作为文件版本标识。每当 relcache 布
	 * 局改变，魔数都可以改变。
	 */
	magic = RELCACHE_INIT_FILEMAGIC;
	if (fwrite(&magic, 1, sizeof(magic), fp) != sizeof(magic))
		elog(FATAL, "could not write init file");

	/*
	 * 写出所有相应的 reldesc（无特定顺序）。
	 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	rel = idhentry->reldesc;
		Form_pg_class relform = rel->rd_rel;

		/* 组不对则忽略 */
		if (relform->relisshared != shared)
			continue;

		/*
		 * 忽略不该进初始化文件的关系。任何已加载至今的
		 * 共享关系均可进共享初始化文件，但非共享关系
		 * 必须按 RelationIdIsInInitFile 属于本地文件。
		 * （注意：若想改关系保留在初始化文件的判据，
		 * 另见 inval.c。此处过滤的理由是确保不
		 * 把任何 relcache 失效不会导致其初始化
		 * 文件失效的东西放进本地初始化
		 * 文件。）
		 */
		if (!shared && !RelationIdIsInInitFile(RelationGetRelid(rel)))
		{
			/* 钉住的关系务必被存储。 */
			Assert(!rel->rd_isnailed);
			continue;
		}

		/* 先写 relcache 条目本体 */
		write_item(rel, sizeof(RelationData), fp);

		/* 接着写关系元组形态 */
		write_item(relform, CLASS_TUPLE_SIZE, fp);

		/* 接着处理全部属性元组形态数据条目 */
		for (i = 0; i < relform->relnatts; i++)
		{
			write_item(TupleDescAttr(rel->rd_att, i),
					   ATTRIBUTE_FIXED_PART_SIZE, fp);
		}

		/* 接着处理访问方法专属字段 */
		write_item(rel->rd_options,
				   (rel->rd_options ? VARSIZE(rel->rd_options) : 0),
				   fp);

		/*
		 * 若是索引则还有事要做。注意此处显式忽略
		 * 分区索引。
		 */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			/* 写 pg_index 元组 */
			/* 我们假定这出自 heap_copytuple！ */
			write_item(rel->rd_indextuple,
					   HEAPTUPLESIZE + rel->rd_indextuple->t_len,
					   fp);

			/* 写出 opfamily OID 数组 */
			write_item(rel->rd_opfamily,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* 写出 opcintype OID 数组 */
			write_item(rel->rd_opcintype,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* 写出支持过程 OID 数组 */
			write_item(rel->rd_support,
					   relform->relnatts * (rel->rd_indam->amsupport * sizeof(RegProcedure)),
					   fp);

			/* 写出排序规则 OID 数组 */
			write_item(rel->rd_indcollation,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* 写出 indoption 值数组 */
			write_item(rel->rd_indoption,
					   relform->relnatts * sizeof(int16),
					   fp);

			Assert(rel->rd_opcoptions);

			/* 写出 opcoptions 值数组 */
			for (i = 0; i < relform->relnatts; i++)
			{
				bytea	   *opt = rel->rd_opcoptions[i];

				write_item(opt, opt ? VARSIZE(opt) : 0, fp);
			}
		}
	}

	if (FreeFile(fp))
		elog(FATAL, "could not write init file");

	/*
	 * 现在必须检查辛苦积累的数据是否因他人刚
	 * 提交的目录变更而过时。若是，就删掉临时文件，
	 * 留给下一个后端再试。（我们自己的 relcache 条
	 * 目会由 SI 消息处理更新，但我们无法肯定所写
	 * 出的内容在当时是否为最
	 * 新。）
	 *
	 * 本函数绝不能与解除初始化文件链接的代码，
	 * 也不能与发送 SI 消息的代码并发，故须全程持锁。
	 */
	LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);

	/* 确保已收全全部传入 SI 消息 */
	AcceptInvalidationMessages();

	/*
	 * 若自后端启动以来收到过任何 SI relcache 失效，
	 * 则假定或已写出过时数据。
	 */
	if (relcacheInvalsReceived == 0L)
	{
		/*
		 * 好，把临时文件改名成最终名，删除既有
		 * 的旧初始化文件。
		 *
		 * 注意：Cygwin 下此处可能失败，若别的后端
		 * 正打开着已解除链接但尚未消失的初始化文件。
		 * 所以按非关键失败处理；失败时删掉无用的临时
		 * 文件即可。
		 */
		if (rename(tempfilename, finalfilename) < 0)
			unlink(tempfilename);
	}
	else
	{
		/* 删除已过时的临时文件 */
		unlink(tempfilename);
	}

	LWLockRelease(RelCacheInitLock);
}

/* 写一块数据，前面带其长度 */
static void
write_item(const void *data, Size len, FILE *fp)
{
	if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
		elog(FATAL, "could not write init file");
	if (len > 0 && fwrite(data, 1, len, fp) != len)
		elog(FATAL, "could not write init file");
}

/*
 * 判断给定关系（以 OID 标识）是否属于应存入
 * relcache 初始化文件的那一类。
 *
 * 所有钉住关系都必须缓存；为效率起见，每个支持
 * syscache 的关系也都应缓存。前者集合几乎是后者
 * 的子集，但非完全如此。特例关系是：
 * RelationCacheInitializePhase2/3 出于效率原因选择
 * 钉住却不支持 syscache。
 */
bool
RelationIdIsInInitFile(Oid relationId)
{
	if (relationId == SharedSecLabelRelationId ||
		relationId == TriggerRelidNameIndexId ||
		relationId == DatabaseNameIndexId ||
		relationId == SharedSecLabelObjectIndexId)
	{
		/*
		 * 若此 Assert 失败，即不再需要相应的特例处理
		 * 了。
		 */
		Assert(!RelationSupportsSysCache(relationId));
		return true;
	}
	return RelationSupportsSysCache(relationId);
}

/*
 * 在更改了一个或多个保留在本地初始化文件中的
 * 关系缓存条目的事务提交期间，失效（移除）初始
 * 化文件。
 *
 * 为对初始化文件的并发检视或重写保持安全，我们
 * 必须先取 RelCacheInitLock，再移除旧初始化文件，
 * 然后发送包含这些关系 relcache 失效的 SI 消息，
 * 最后释放 RelCacheInitLock。这使整件事相对于
 * write_relcache_init_file 串行化，从而能确信任何
 * 正在并发尝试创建新初始化文件的进程，都不会在
 * 我们解除链接之后把已然过时的版本挪到位。此外，
 * 因为我们在发送 SI 消息之前就解除链接，正在启动
 * 的后端无法读到已然过时的初始化文件、然后错过
 * 那些将强使其更新 relcache 条目的 SI 消息。（这
 * 之所以行得通，是因为后端启动序列在尝试加载初始文
 * 件）
 *
 * 加锁与解除链接在 RelationCacheInitFilePreInvalidate
 * 中完成，在 RelationCacheInitFilePostInvalidate 中
 * 释放锁。其间须发出待发 SI 消息。
 */
void
RelationCacheInitFilePreInvalidate(void)
{
	char		localinitfname[MAXPGPATH];
	char		sharedinitfname[MAXPGPATH];

	if (DatabasePath)
		snprintf(localinitfname, sizeof(localinitfname), "%s/%s",
				 DatabasePath, RELCACHE_INIT_FILENAME);
	snprintf(sharedinitfname, sizeof(sharedinitfname), "global/%s",
			 RELCACHE_INIT_FILENAME);

	LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);

	/*
	 * 自上次移除以来若无后端启动过，文件可能不在。
	 * 但除 ENOENT 外的失败要报 ERROR。幸运的是，若
	 * 我们无法清除将过时的初始化文件，现在中止事务
	 * 还来得及。
	 */
	if (DatabasePath)
		unlink_initfile(localinitfname, ERROR);
	unlink_initfile(sharedinitfname, ERROR);
}

void
RelationCacheInitFilePostInvalidate(void)
{
	LWLockRelease(RelCacheInitLock);
}

/*
 * postmaster 启动时移除初始化文件。
 *
 * 我们过去跨重启保留初始化文件，但那在 PITR 场
 * 景下不安全，即便在简单崩溃恢复情形中，初始化
 * 文件也存在与数据库失去同步的窗口期。所以现在
 * 我们只在启动期间移除它们，并期望首个后端启动
 * 时重建。当然，这必须对集簇每个数据库执行。
 */
void
RelationCacheInitFileRemove(void)
{
	const char *tblspcdir = "pg_tblspc";
	DIR		   *dir;
	struct dirent *de;
	char		path[MAXPGPATH + 10 + sizeof(TABLESPACE_VERSION_DIRECTORY)];

	snprintf(path, sizeof(path), "global/%s",
			 RELCACHE_INIT_FILENAME);
	unlink_initfile(path, LOG);

	/* 扫描默认表空间中的一切 */
	RelationCacheInitFileRemoveInDir("base");

	/* 扫描表空间链接目录以找出非默认表空间 */
	dir = AllocateDir(tblspcdir);

	while ((de = ReadDirExtended(dir, tblspcdir, LOG)) != NULL)
	{
		if (strspn(de->d_name, "0123456789") == strlen(de->d_name))
		{
			/* 扫描表空间目录找各数据库子目录 */
			snprintf(path, sizeof(path), "%s/%s/%s",
					 tblspcdir, de->d_name, TABLESPACE_VERSION_DIRECTORY);
			RelationCacheInitFileRemoveInDir(path);
		}
	}

	FreeDir(dir);
}

/* 为 RelationCacheInitFileRemove 处理单个表 */
static void
RelationCacheInitFileRemoveInDir(const char *tblspcpath)
{
	DIR		   *dir;
	struct dirent *de;
	char		initfilename[MAXPGPATH * 2];

	/* 扫描表空间目录以找出各数据库目录 */
	dir = AllocateDir(tblspcpath);

	while ((de = ReadDirExtended(dir, tblspcpath, LOG)) != NULL)
	{
		if (strspn(de->d_name, "0123456789") == strlen(de->d_name))
		{
			/* 试移除每个数据库的初始化文件 */
			snprintf(initfilename, sizeof(initfilename), "%s/%s/%s",
					 tblspcpath, de->d_name, RELCACHE_INIT_FILENAME);
			unlink_initfile(initfilename, LOG);
		}
	}

	FreeDir(dir);
}

static void
unlink_initfile(const char *initfilename, int elevel)
{
	if (unlink(initfilename) < 0)
	{
		/* 可能不在，但 ENOENT 以外的错误要记日志 */
		if (errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove cache file \"%s\": %m",
							initfilename)));
	}
}
