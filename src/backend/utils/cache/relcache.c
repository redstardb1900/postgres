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
			/* allow the entry to be removed */
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
			 * Hmm, somewhere there's a (leaked?) reference to the relation.
			 * We daren't remove the entry for fear of dereferencing a
			 * dangling pointer later.  Bleat, and transfer it to the parent
			 * subtransaction so we can try again later.  This must be just a
			 * WARNING to avoid error-during-error-recovery loops.
			 */
			relation->rd_createSubid = parentSubid;
			elog(WARNING, "cannot remove relcache entry for \"%s\" because it has nonzero refcount",
				 RelationGetRelationName(relation));
		}
	}

	/*
	 * Likewise, update or drop any new-relfilenumber-in-subtransaction record
	 * or drop record.
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
 *			Build a relcache entry for an about-to-be-created relation,
 *			and enter it into the relcache.
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
	 * check for creation of a rel that must be nailed in cache.
	 *
	 * XXX this list had better match the relations specially handled in
	 * RelationCacheInitializePhase2/3.
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
	 * check that hardwired list of shared rels matches what's in the
	 * bootstrap .bki file.  If you get a failure here during initdb, you
	 * probably need to fix IsSharedRelation() to match whatever you've done
	 * to the set of shared relations.
	 */
	if (shared_relation != IsSharedRelation(relid))
		elog(ERROR, "shared_relation flag for \"%s\" does not match IsSharedRelation(%u)",
			 relname, relid);

	/* Shared relations had better be mapped, too */
	Assert(mapped_relation || !shared_relation);

	/*
	 * switch to the cache context to create the relcache entry.
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * allocate a new relation descriptor and fill in basic state fields.
	 */
	rel = (Relation) palloc0(sizeof(RelationData));

	/* make sure relation is marked as having no open file yet */
	rel->rd_smgr = NULL;

	/* mark it nailed if appropriate */
	rel->rd_isnailed = nailit;

	rel->rd_refcnt = nailit ? 1 : 0;

	/* it's being created in this transaction */
	rel->rd_createSubid = GetCurrentSubTransactionId();
	rel->rd_newRelfilelocatorSubid = InvalidSubTransactionId;
	rel->rd_firstRelfilelocatorSubid = InvalidSubTransactionId;
	rel->rd_droppedSubid = InvalidSubTransactionId;

	/*
	 * create a new tuple descriptor from the one passed in.  We do this
	 * partly to copy it into the cache context, and partly because the new
	 * relation can't have any defaults or constraints yet; they have to be
	 * added in later steps, because they require additions to multiple system
	 * catalogs.  We can copy attnotnull constraints here, however.
	 */
	rel->rd_att = CreateTupleDescCopy(tupDesc);
	rel->rd_att->tdrefcount = 1;	/* mark as refcounted */
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
	 * initialize relation tuple form (caller may add/override data later)
	 */
	rel->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&rel->rd_rel->relname, relname);
	rel->rd_rel->relnamespace = relnamespace;

	rel->rd_rel->relkind = relkind;
	rel->rd_rel->relnatts = natts;
	rel->rd_rel->reltype = InvalidOid;
	/* needed when bootstrapping: */
	rel->rd_rel->relowner = BOOTSTRAP_SUPERUSERID;

	/* set up persistence and relcache fields dependent on it */
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

	/* if it's a materialized view, it's not populated initially */
	if (relkind == RELKIND_MATVIEW)
		rel->rd_rel->relispopulated = false;
	else
		rel->rd_rel->relispopulated = true;

	/* set replica identity -- system catalogs and non-tables don't have one */
	if (!IsCatalogNamespace(relnamespace) &&
		(relkind == RELKIND_RELATION ||
		 relkind == RELKIND_MATVIEW ||
		 relkind == RELKIND_PARTITIONED_TABLE))
		rel->rd_rel->relreplident = REPLICA_IDENTITY_DEFAULT;
	else
		rel->rd_rel->relreplident = REPLICA_IDENTITY_NOTHING;

	/*
	 * Insert relation physical and logical identifiers (OIDs) into the right
	 * places.  For a mapped relation, we set relfilenumber to zero and rely
	 * on RelationInitPhysicalAddr to consult the map.
	 */
	rel->rd_rel->relisshared = shared_relation;

	RelationGetRelid(rel) = relid;

	for (i = 0; i < natts; i++)
		TupleDescAttr(rel->rd_att, i)->attrelid = relid;

	rel->rd_rel->reltablespace = reltablespace;

	if (mapped_relation)
	{
		rel->rd_rel->relfilenode = InvalidRelFileNumber;
		/* Add it to the active mapping information */
		RelationMapUpdateMap(relid, relfilenumber, shared_relation, true);
	}
	else
		rel->rd_rel->relfilenode = relfilenumber;

	RelationInitLockInfo(rel);	/* see lmgr.c */

	RelationInitPhysicalAddr(rel);

	rel->rd_rel->relam = accessmtd;

	/*
	 * RelationInitTableAccessMethod will do syscache lookups, so we mustn't
	 * run it in CacheMemoryContext.  Fortunately, the remaining steps don't
	 * require a long-lived current context.
	 */
	MemoryContextSwitchTo(oldcxt);

	if (RELKIND_HAS_TABLE_AM(relkind) || relkind == RELKIND_SEQUENCE)
		RelationInitTableAccessMethod(rel);

	/*
	 * Okay to insert into the relcache hash table.
	 *
	 * Ordinarily, there should certainly not be an existing hash entry for
	 * the same OID; but during bootstrap, when we create a "real" relcache
	 * entry for one of the bootstrap relations, we'll be overwriting the
	 * phony one created with formrdesc.  So allow that to happen for nailed
	 * rels.
	 */
	RelationCacheInsert(rel, nailit);

	/*
	 * Flag relation as needing eoxact cleanup (to clear rd_createSubid). We
	 * can't do this before storing relid in it.
	 */
	EOXactListAdd(rel);

	/* It's fully valid */
	rel->rd_isvalid = true;

	/*
	 * Caller expects us to pin the returned entry.
	 */
	RelationIncrementReferenceCount(rel);

	return rel;
}


/*
 * RelationSetNewRelfilenumber
 *
 * Assign a new relfilenumber (physical file name), and possibly a new
 * persistence setting, to the relation.
 *
 * This allows a full rewrite of the relation to be done with transactional
 * safety (since the filenumber assignment can be rolled back).  Note however
 * that there is no simple way to access the relation's old data for the
 * remainder of the current transaction.  This limits the usefulness to cases
 * such as TRUNCATE or rebuilding an index from scratch.
 *
 * Caller must already hold exclusive lock on the relation.
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
		/* Allocate a new relfilenumber */
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
	 * Get a writable copy of the pg_class tuple for the given relation.
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
	 * Schedule unlinking of the old storage at transaction commit, except
	 * when performing a binary upgrade, when we must do it immediately.
	 */
	if (IsBinaryUpgrade)
	{
		SMgrRelation srel;

		/*
		 * During a binary upgrade, we use this code path to ensure that
		 * pg_largeobject and its index have the same relfilenumbers as in the
		 * old cluster. This is necessary because pg_upgrade treats
		 * pg_largeobject like a user table, not a system table. It is however
		 * possible that a table or index may need to end up with the same
		 * relfilenumber in the new cluster as what it had in the old cluster.
		 * Hence, we can't wait until commit time to remove the old storage.
		 *
		 * In general, this function needs to have transactional semantics,
		 * and removing the old storage before commit time surely isn't.
		 * However, it doesn't really matter, because if a binary upgrade
		 * fails at this stage, the new cluster will need to be recreated
		 * anyway.
		 */
		srel = smgropen(relation->rd_locator, relation->rd_backend);
		smgrdounlinkall(&srel, 1, false);
		smgrclose(srel);
	}
	else
	{
		/* Not a binary upgrade, so just schedule it to happen later. */
		RelationDropStorage(relation);
	}

	/*
	 * Create storage for the main fork of the new relfilenumber.  If it's a
	 * table-like object, call into the table AM to do so, which'll also
	 * create the table's init fork if needed.
	 *
	 * NOTE: If relevant for the AM, any conflict in relfilenumber value will
	 * be caught here, if GetNewRelFileNumber messes up for any reason.
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
		/* handle these directly, at least for now */
		SMgrRelation srel;

		srel = RelationCreateStorage(newrlocator, persistence, true);
		smgrclose(srel);
	}
	else
	{
		/* we shouldn't be called for anything else */
		elog(ERROR, "relation \"%s\" does not have storage",
			 RelationGetRelationName(relation));
	}

	/*
	 * If we're dealing with a mapped index, pg_class.relfilenode doesn't
	 * change; instead we have to send the update to the relation mapper.
	 *
	 * For mapped indexes, we don't actually change the pg_class entry at all;
	 * this is essential when reindexing pg_class itself.  That leaves us with
	 * possibly-inaccurate values of relpages etc, but those will be fixed up
	 * later.
	 */
	if (RelationIsMapped(relation))
	{
		/* This case is only supported for indexes */
		Assert(relation->rd_rel->relkind == RELKIND_INDEX);

		/* Since we're not updating pg_class, these had better not change */
		Assert(classform->relfrozenxid == freezeXid);
		Assert(classform->relminmxid == minmulti);
		Assert(classform->relpersistence == persistence);

		/*
		 * In some code paths it's possible that the tuple update we'd
		 * otherwise do here is the only thing that would assign an XID for
		 * the current transaction.  However, we must have an XID to delete
		 * files, so make sure one is assigned.
		 */
		(void) GetCurrentTransactionId();

		/* Do the deed */
		RelationMapUpdateMap(RelationGetRelid(relation),
							 newrelfilenumber,
							 relation->rd_rel->relisshared,
							 false);

		/* Since we're not updating pg_class, must trigger inval manually */
		CacheInvalidateRelcache(relation);
	}
	else
	{
		/* Normal case, update the pg_class entry */
		classform->relfilenode = newrelfilenumber;

		/* relpages etc. never change for sequences */
		if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
		{
			classform->relpages = 0;	/* it's empty until further notice */
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
	 * Make the pg_class row change or relation map change visible.  This will
	 * cause the relcache entry to get updated, too.
	 */
	CommandCounterIncrement();

	RelationAssumeNewRelfilelocator(relation);
}

/*
 * RelationAssumeNewRelfilelocator
 *
 * Code that modifies pg_class.reltablespace or pg_class.relfilenode must call
 * this.  The call shall precede any code that might insert WAL records whose
 * replay would modify bytes in the new RelFileLocator, and the call shall follow
 * any WAL modifying bytes in the prior RelFileLocator.  See struct RelationData.
 * Ideally, call this as near as possible to the CommandCounterIncrement()
 * that makes the pg_class change visible (before it or after it); that
 * minimizes the chance of future development adding a forbidden WAL insertion
 * between RelationAssumeNewRelfilelocator() and CommandCounterIncrement().
 */
void
RelationAssumeNewRelfilelocator(Relation relation)
{
	relation->rd_newRelfilelocatorSubid = GetCurrentSubTransactionId();
	if (relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)
		relation->rd_firstRelfilelocatorSubid = relation->rd_newRelfilelocatorSubid;

	/* Flag relation as needing eoxact cleanup (to clear these fields) */
	EOXactListAdd(relation);
}


/*
 *		RelationCacheInitialize
 *
 *		This initializes the relation descriptor cache.  At the time
 *		that this is invoked, we can't do database access yet (mainly
 *		because the transaction subsystem is not up); all we are doing
 *		is making an empty cache hashtable.  This must be done before
 *		starting the initialization transaction, because otherwise
 *		AtEOXact_RelationCache would crash if that transaction aborts
 *		before we can get the relcache set up.
 */

#define INITRELCACHESIZE		400

void
RelationCacheInitialize(void)
{
	HASHCTL		ctl;
	int			allocsize;

	/*
	 * make sure cache memory context exists
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/*
	 * create hashtable that indexes the relcache
	 */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelIdCacheEnt);
	RelationIdCache = hash_create("Relcache by OID", INITRELCACHESIZE,
								  &ctl, HASH_ELEM | HASH_BLOBS);

	/*
	 * reserve enough in_progress_list slots for many cases
	 */
	allocsize = 4;
	in_progress_list =
		MemoryContextAlloc(CacheMemoryContext,
						   allocsize * sizeof(*in_progress_list));
	in_progress_list_maxlen = allocsize;

	/*
	 * relation mapper needs to be initialized too
	 */
	RelationMapInitialize();
}

/*
 *		RelationCacheInitializePhase2
 *
 *		This is called to prepare for access to shared catalogs during startup.
 *		We must at least set up nailed reldescs for pg_database, pg_authid,
 *		pg_auth_members, and pg_shseclabel. Ideally we'd like to have reldescs
 *		for their indexes, too.  We attempt to load this information from the
 *		shared relcache init file.  If that's missing or broken, just make
 *		phony entries for the catalogs themselves.
 *		RelationCacheInitializePhase3 will clean up as needed.
 */
void
RelationCacheInitializePhase2(void)
{
	MemoryContext oldcxt;

	/*
	 * relation mapper needs initialized too
	 */
	RelationMapInitializePhase2();

	/*
	 * In bootstrap mode, the shared catalogs aren't there yet anyway, so do
	 * nothing.
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * switch to cache memory context
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * Try to load the shared relcache cache file.  If unsuccessful, bootstrap
	 * the cache with pre-made descriptors for the critical shared catalogs.
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

#define NUM_CRITICAL_SHARED_RELS	5	/* fix if you change list above */
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 *		RelationCacheInitializePhase3
 *
 *		This is called as soon as the catcache and transaction system
 *		are functional and we have determined MyDatabaseId.  At this point
 *		we can actually read data from the database's system catalogs.
 *		We first try to read pre-computed relcache entries from the local
 *		relcache init file.  If that's missing or broken, make phony entries
 *		for the minimum set of nailed-in-cache relations.  Then (unless
 *		bootstrapping) make sure we have entries for the critical system
 *		indexes.  Once we've done all this, we have enough infrastructure to
 *		open any system catalog or use any catcache.  The last step is to
 *		rewrite the cache files if needed.
 */
void
RelationCacheInitializePhase3(void)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	MemoryContext oldcxt;
	bool		needNewCacheFile = !criticalSharedRelcachesBuilt;

	/*
	 * relation mapper needs initialized too
	 */
	RelationMapInitializePhase3();

	/*
	 * switch to cache memory context
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * Try to load the local relcache cache file.  If unsuccessful, bootstrap
	 * the cache with pre-made descriptors for the critical "nailed-in" system
	 * catalogs.
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

#define NUM_CRITICAL_LOCAL_RELS 4	/* fix if you change list above */
	}

	MemoryContextSwitchTo(oldcxt);

	/* In bootstrap mode, the faked-up formrdesc info is all we'll have */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * If we didn't get the critical system indexes loaded into relcache, do
	 * so now.  These are critical because the catcache and/or opclass cache
	 * depend on them for fetches done during relcache load.  Thus, we have an
	 * infinite-recursion problem.  We can break the recursion by doing
	 * heapscans instead of indexscans at certain key spots. To avoid hobbling
	 * performance, we only want to do that until we have the critical indexes
	 * loaded into relcache.  Thus, the flag criticalRelcachesBuilt is used to
	 * decide whether to do heapscan or indexscan at the key spots, and we set
	 * it true after we've loaded the critical indexes.
	 *
	 * The critical indexes are marked as "nailed in cache", partly to make it
	 * easy for load_relcache_init_file to count them, but mainly because we
	 * cannot flush and rebuild them once we've set criticalRelcachesBuilt to
	 * true.  (NOTE: perhaps it would be possible to reload them by
	 * temporarily setting criticalRelcachesBuilt to false again.  For now,
	 * though, we just nail 'em in.)
	 *
	 * RewriteRelRulenameIndexId and TriggerRelidNameIndexId are not critical
	 * in the same way as the others, because the critical catalogs don't
	 * (currently) have any rules or triggers, and so these indexes can be
	 * rebuilt without inducing recursion.  However they are used during
	 * relcache load when a rel does have rules or triggers, so we choose to
	 * nail them for performance reasons.
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

#define NUM_CRITICAL_LOCAL_INDEXES	7	/* fix if you change list above */

		criticalRelcachesBuilt = true;
	}

	/*
	 * Process critical shared indexes too.
	 *
	 * DatabaseNameIndexId isn't critical for relcache loading, but rather for
	 * initial lookup of MyDatabaseId, without which we'll never find any
	 * non-shared catalogs at all.  Autovacuum calls InitPostgres with a
	 * database OID, so it instead depends on DatabaseOidIndexId.  We also
	 * need to nail up some indexes on pg_authid and pg_auth_members for use
	 * during client authentication.  SharedSecLabelObjectIndexId isn't
	 * critical for the core system, but authentication hooks might be
	 * interested in it.
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

#define NUM_CRITICAL_SHARED_INDEXES 6	/* fix if you change list above */

		criticalSharedRelcachesBuilt = true;
	}

	/*
	 * Now, scan all the relcache entries and update anything that might be
	 * wrong in the results from formrdesc or the relcache cache file. If we
	 * faked up relcache entries using formrdesc, then read the real pg_class
	 * rows and replace the fake entries with them. Also, if any of the
	 * relcache entries have rules, triggers, or security policies, load that
	 * info the hard way since it isn't recorded in the cache file.
	 *
	 * Whenever we access the catalogs to read data, there is a possibility of
	 * a shared-inval cache flush causing relcache entries to be removed.
	 * Since hash_seq_search only guarantees to still work after the *current*
	 * entry is removed, it's unsafe to continue the hashtable scan afterward.
	 * We handle this by restarting the scan from scratch after each access.
	 * This is theoretically O(N^2), but the number of entries that actually
	 * need to be fixed is small enough that it doesn't matter.
	 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;
		bool		restart = false;

		/*
		 * Make sure *this* entry doesn't get flushed while we work with it.
		 */
		RelationIncrementReferenceCount(relation);

		/*
		 * If it's a faked-up entry, read the real pg_class tuple.
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
			 * Copy tuple to relation->rd_rel. (See notes in
			 * AllocateRelationDesc())
			 */
			memcpy((char *) relation->rd_rel, (char *) relp, CLASS_TUPLE_SIZE);

			/* Update rd_options while we have the tuple */
			if (relation->rd_options)
				pfree(relation->rd_options);
			RelationParseRelOptions(relation, htup);

			/*
			 * Check the values in rd_att were set up correctly.  (We cannot
			 * just copy them over now: formrdesc must have set up the rd_att
			 * data correctly to start with, because it may already have been
			 * copied into one or more catcache entries.)
			 */
			Assert(relation->rd_att->tdtypeid == relp->reltype);
			Assert(relation->rd_att->tdtypmod == -1);

			ReleaseSysCache(htup);

			/* relowner had better be OK now, else we'll loop forever */
			if (relation->rd_rel->relowner == InvalidOid)
				elog(ERROR, "invalid relowner in pg_class entry for \"%s\"",
					 RelationGetRelationName(relation));

			restart = true;
		}

		/*
		 * Fix data that isn't saved in relcache cache file.
		 *
		 * relhasrules or relhastriggers could possibly be wrong or out of
		 * date.  If we don't actually find any rules or triggers, clear the
		 * local copy of the flag so that we don't get into an infinite loop
		 * here.  We don't make any attempt to fix the pg_class entry, though.
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
		 * Re-load the row security policies if the relation has them, since
		 * they are not preserved in the cache.  Note that we can never NOT
		 * have a policy while relrowsecurity is true,
		 * RelationBuildRowSecurity will create a single default-deny policy
		 * if there is no policy defined in pg_policy.
		 */
		if (relation->rd_rel->relrowsecurity && relation->rd_rsdesc == NULL)
		{
			RelationBuildRowSecurity(relation);

			Assert(relation->rd_rsdesc != NULL);
			restart = true;
		}

		/* Reload tableam data if needed */
		if (relation->rd_tableam == NULL &&
			(RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind) || relation->rd_rel->relkind == RELKIND_SEQUENCE))
		{
			RelationInitTableAccessMethod(relation);
			Assert(relation->rd_tableam != NULL);

			restart = true;
		}

		/* Release hold on the relation */
		RelationDecrementReferenceCount(relation);

		/* Now, restart the hashtable scan if needed */
		if (restart)
		{
			hash_seq_term(&status);
			hash_seq_init(&status, RelationIdCache);
		}
	}

	/*
	 * Lastly, write out new relcache cache files if needed.  We don't bother
	 * to distinguish cases where only one of the two needs an update.
	 */
	if (needNewCacheFile)
	{
		/*
		 * Force all the catcaches to finish initializing and thereby open the
		 * catalogs and indexes they use.  This will preload the relcache with
		 * entries for all the most important system catalogs and indexes, so
		 * that the init files will be most useful for future backends.
		 */
		InitCatalogCachePhase2();

		/* now write the files */
		write_relcache_init_file(true);
		write_relcache_init_file(false);
	}
}

/*
 * Load one critical system index into the relcache
 *
 * indexoid is the OID of the target index, heapoid is the OID of the catalog
 * it belongs to.
 */
static void
load_critical_index(Oid indexoid, Oid heapoid)
{
	Relation	ird;

	/*
	 * We must lock the underlying catalog before locking the index to avoid
	 * deadlock, since RelationBuildDesc might well need to read the catalog,
	 * and if anyone else is exclusive-locking this catalog and index they'll
	 * be doing it in that order.
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
 * GetPgClassDescriptor -- get a predefined tuple descriptor for pg_class
 * GetPgIndexDescriptor -- get a predefined tuple descriptor for pg_index
 *
 * We need this kluge because we have to be able to access non-fixed-width
 * fields of pg_class and pg_index before we have the standard catalog caches
 * available.  We use predefined data that's set up in just the same way as
 * the bootstrapped reldescs used by formrdesc().  The resulting tupdesc is
 * not 100% kosher: it does not have the correct rowtype OID in tdtypeid, nor
 * does it have a TupleConstr field.  But it's good enough for the purpose of
 * extracting fields.
 */
static TupleDesc
BuildHardcodedDescriptor(int natts, const FormData_pg_attribute *attrs)
{
	TupleDesc	result;
	MemoryContext oldcxt;
	int			i;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	result = CreateTemplateTupleDesc(natts);
	result->tdtypeid = RECORDOID;	/* not right, but we don't care */
	result->tdtypmod = -1;

	for (i = 0; i < natts; i++)
	{
		memcpy(TupleDescAttr(result, i), &attrs[i], ATTRIBUTE_FIXED_PART_SIZE);
		/* make sure attcacheoff is valid */
		TupleDescAttr(result, i)->attcacheoff = -1;
	}

	/* initialize first attribute's attcacheoff, cf RelationBuildTupleDesc */
	TupleDescAttr(result, 0)->attcacheoff = 0;

	/* Note: we don't bother to set up a TupleConstr entry */

	MemoryContextSwitchTo(oldcxt);

	return result;
}

static TupleDesc
GetPgClassDescriptor(void)
{
	static TupleDesc pgclassdesc = NULL;

	/* Already done? */
	if (pgclassdesc == NULL)
		pgclassdesc = BuildHardcodedDescriptor(Natts_pg_class,
											   Desc_pg_class);

	return pgclassdesc;
}

static TupleDesc
GetPgIndexDescriptor(void)
{
	static TupleDesc pgindexdesc = NULL;

	/* Already done? */
	if (pgindexdesc == NULL)
		pgindexdesc = BuildHardcodedDescriptor(Natts_pg_index,
											   Desc_pg_index);

	return pgindexdesc;
}

/*
 * Load any default attribute value definitions for the relation.
 *
 * ndef is the number of attributes that were marked atthasdef.
 *
 * Note: we don't make it a hard error to be missing some pg_attrdef records.
 * We can limp along as long as nothing needs to use the default value.  Code
 * that fails to find an expected AttrDefault record should throw an error.
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

	/* Allocate array with room for as many entries as expected */
	attrdef = (AttrDefault *)
		MemoryContextAllocZero(CacheMemoryContext,
							   ndef * sizeof(AttrDefault));

	/* Search pg_attrdef for relevant entries */
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

		/* protect limited size of array */
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
			/* detoast and convert to cstring in caller's context */
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
	 * Sort the AttrDefault entries by adnum, for the convenience of
	 * equalTupleDescs().  (Usually, they already will be in order, but this
	 * might not be so if systable_getnext isn't using an index.)
	 */
	if (found > 1)
		qsort(attrdef, found, sizeof(AttrDefault), AttrDefaultCmp);

	/* Install array only after it's fully valid */
	relation->rd_att->constr->defval = attrdef;
	relation->rd_att->constr->num_defval = found;
}

/*
 * qsort comparator to sort AttrDefault entries by adnum
 */
static int
AttrDefaultCmp(const void *a, const void *b)
{
	const AttrDefault *ada = (const AttrDefault *) a;
	const AttrDefault *adb = (const AttrDefault *) b;

	return ada->adnum - adb->adnum;
}

/*
 * Load any check constraints for the relation.
 *
 * As with defaults, if we don't find the expected number of them, just warn
 * here.  The executor should throw an error if an INSERT/UPDATE is attempted.
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

	/* Allocate array with room for as many entries as expected */
	check = (ConstrCheck *)
		MemoryContextAllocZero(CacheMemoryContext,
							   ncheck * sizeof(ConstrCheck));

	/* Search pg_constraint for relevant entries */
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

		/* We want check constraints only */
		if (conform->contype != CONSTRAINT_CHECK)
			continue;

		/* protect limited size of array */
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

		/* Grab and test conbin is actually set */
		val = fastgetattr(htup,
						  Anum_pg_constraint_conbin,
						  conrel->rd_att, &isnull);
		if (isnull)
			elog(WARNING, "null conbin for relation \"%s\"",
				 RelationGetRelationName(relation));
		else
		{
			/* detoast and convert to cstring in caller's context */
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
	 * Sort the records by name.  This ensures that CHECKs are applied in a
	 * deterministic order, and it also makes equalTupleDescs() faster.
	 */
	if (found > 1)
		qsort(check, found, sizeof(ConstrCheck), CheckConstraintCmp);

	/* Install array only after it's fully valid */
	relation->rd_att->constr->check = check;
	relation->rd_att->constr->num_check = found;
}

/*
 * qsort comparator to sort ConstrCheck entries by name
 */
static int
CheckConstraintCmp(const void *a, const void *b)
{
	const ConstrCheck *ca = (const ConstrCheck *) a;
	const ConstrCheck *cb = (const ConstrCheck *) b;

	return strcmp(ca->ccname, cb->ccname);
}

/*
 * RelationGetFKeyList -- get a list of foreign key info for the relation
 *
 * Returns a list of ForeignKeyCacheInfo structs, one per FK constraining
 * the given relation.  This data is a direct copy of relevant fields from
 * pg_constraint.  The list items are in no particular order.
 *
 * CAUTION: the returned list is part of the relcache's data, and could
 * vanish in a relcache entry reset.  Callers must inspect or copy it
 * before doing anything that might trigger a cache flush, such as
 * system catalog accesses.  copyObject() can be used if desired.
 * (We define it this way because current callers want to filter and
 * modify the list entries anyway, so copying would be a waste of time.)
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

	/* Quick exit if we already computed the list. */
	if (relation->rd_fkeyvalid)
		return relation->rd_fkeylist;

	/* Fast path: non-partitioned tables without triggers can't have FKs */
	if (!relation->rd_rel->relhastriggers &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return NIL;

	/*
	 * We build the list we intend to return (in the caller's context) while
	 * doing the scan.  After successfully completing the scan, we copy that
	 * list into the relcache entry.  This avoids cache-context memory leakage
	 * if we get some sort of error partway through.
	 */
	result = NIL;

	/* Prepare to scan pg_constraint for entries having conrelid = this rel. */
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

		/* consider only foreign keys */
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

		/* Add FK's node to the result list */
		result = lappend(result, info);
	}

	systable_endscan(conscan);
	table_close(conrel, AccessShareLock);

	/* Now save a copy of the completed list in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	oldlist = relation->rd_fkeylist;
	relation->rd_fkeylist = copyObject(result);
	relation->rd_fkeyvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* Don't leak the old list, if there is one */
	list_free_deep(oldlist);

	return result;
}

/*
 * RelationGetIndexList -- get a list of OIDs of indexes on this relation
 *
 * The index list is created only if someone requests it.  We scan pg_index
 * to find relevant indexes, and add the list to the relcache entry so that
 * we won't have to compute it again.  Note that shared cache inval of a
 * relcache entry will delete the old list and set rd_indexvalid to false,
 * so that we must recompute the index list on next request.  This handles
 * creation or deletion of an index.
 *
 * Indexes that are marked not indislive are omitted from the returned list.
 * Such indexes are expected to be dropped momentarily, and should not be
 * touched at all by any caller of this function.
 *
 * The returned list is guaranteed to be sorted in order by OID.  This is
 * needed by the executor, since for index types that we obtain exclusive
 * locks on when updating the index, all backends must lock the indexes in
 * the same order or we will get deadlocks (see ExecOpenIndices()).  Any
 * consistent ordering would do, but ordering by OID is easy.
 *
 * Since shared cache inval causes the relcache's copy of the list to go away,
 * we return a copy of the list palloc'd in the caller's context.  The caller
 * may list_free() the returned list after scanning it. This is necessary
 * since the caller will typically be doing syscache lookups on the relevant
 * indexes, and syscache lookup could cause SI messages to be processed!
 *
 * In exactly the same way, we update rd_pkindex, which is the OID of the
 * relation's primary key index if any, else InvalidOid; and rd_replidindex,
 * which is the pg_class OID of an index to be used as the relation's
 * replication identity index, or InvalidOid if there is no such index.
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

	/* Quick exit if we already computed the list. */
	if (relation->rd_indexvalid)
		return list_copy(relation->rd_indexlist);

	/*
	 * We build the list we intend to return (in the caller's context) while
	 * doing the scan.  After successfully completing the scan, we copy that
	 * list into the relcache entry.  This avoids cache-context memory leakage
	 * if we get some sort of error partway through.
	 */
	result = NIL;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
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
		 * Ignore any indexes that are currently being dropped.  This will
		 * prevent them from being searched, inserted into, or considered in
		 * HOT-safety decisions.  It's unsafe to touch such an index at all
		 * since its catalog entries could disappear at any instant.
		 */
		if (!index->indislive)
			continue;

		/* add index's OID to result list */
		result = lappend_oid(result, index->indexrelid);

		/*
		 * Invalid, non-unique, non-immediate or predicate indexes aren't
		 * interesting for either oid indexes or replication identity indexes,
		 * so don't check them.
		 */
		if (!index->indisvalid || !index->indisunique ||
			!index->indimmediate ||
			!heap_attisnull(htup, Anum_pg_index_indpred, NULL))
			continue;

		/* remember primary key index if any */
		if (index->indisprimary)
			pkeyIndex = index->indexrelid;

		/* remember explicitly chosen replica index */
		if (index->indisreplident)
			candidateIndex = index->indexrelid;
	}

	systable_endscan(indscan);

	table_close(indrel, AccessShareLock);

	/* Sort the result list into OID order, per API spec. */
	list_sort(result, list_oid_cmp);

	/* Now save a copy of the completed list in the relcache entry. */
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

	/* Don't leak the old list, if there is one */
	list_free(oldlist);

	return result;
}

/*
 * RelationGetStatExtList
 *		get a list of OIDs of statistics objects on this relation
 *
 * The statistics list is created only if someone requests it, in a way
 * similar to RelationGetIndexList().  We scan pg_statistic_ext to find
 * relevant statistics, and add the list to the relcache entry so that we
 * won't have to compute it again.  Note that shared cache inval of a
 * relcache entry will delete the old list and set rd_statvalid to 0,
 * so that we must recompute the statistics list on next request.  This
 * handles creation or deletion of a statistics object.
 *
 * The returned list is guaranteed to be sorted in order by OID, although
 * this is not currently needed.
 *
 * Since shared cache inval causes the relcache's copy of the list to go away,
 * we return a copy of the list palloc'd in the caller's context.  The caller
 * may list_free() the returned list after scanning it. This is necessary
 * since the caller will typically be doing syscache lookups on the relevant
 * statistics, and syscache lookup could cause SI messages to be processed!
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

	/* Quick exit if we already computed the list. */
	if (relation->rd_statvalid != 0)
		return list_copy(relation->rd_statlist);

	/*
	 * We build the list we intend to return (in the caller's context) while
	 * doing the scan.  After successfully completing the scan, we copy that
	 * list into the relcache entry.  This avoids cache-context memory leakage
	 * if we get some sort of error partway through.
	 */
	result = NIL;

	/*
	 * Prepare to scan pg_statistic_ext for entries having stxrelid = this
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

	/* Sort the result list into OID order, per API spec. */
	list_sort(result, list_oid_cmp);

	/* Now save a copy of the completed list in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	oldlist = relation->rd_statlist;
	relation->rd_statlist = list_copy(result);

	relation->rd_statvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* Don't leak the old list, if there is one */
	list_free(oldlist);

	return result;
}

/*
 * RelationGetPrimaryKeyIndex -- get OID of the relation's primary key index
 *
 * Returns InvalidOid if there is no such index.
 */
Oid
RelationGetPrimaryKeyIndex(Relation relation)
{
	List	   *ilist;

	if (!relation->rd_indexvalid)
	{
		/* RelationGetIndexList does the heavy lifting. */
		ilist = RelationGetIndexList(relation);
		list_free(ilist);
		Assert(relation->rd_indexvalid);
	}

	return relation->rd_pkindex;
}

/*
 * RelationGetReplicaIndex -- get OID of the relation's replica identity index
 *
 * Returns InvalidOid if there is no such index.
 */
Oid
RelationGetReplicaIndex(Relation relation)
{
	List	   *ilist;

	if (!relation->rd_indexvalid)
	{
		/* RelationGetIndexList does the heavy lifting. */
		ilist = RelationGetIndexList(relation);
		list_free(ilist);
		Assert(relation->rd_indexvalid);
	}

	return relation->rd_replidindex;
}

/*
 * RelationGetIndexExpressions -- get the index expressions for an index
 *
 * We cache the result of transforming pg_index.indexprs into a node tree.
 * If the rel is not an index or has no expressional columns, we return NIL.
 * Otherwise, the returned tree is copied into the caller's memory context.
 * (We don't want to return a pointer to the relcache copy, since it could
 * disappear due to relcache invalidation.)
 */
List *
RelationGetIndexExpressions(Relation relation)
{
	List	   *result;
	Datum		exprsDatum;
	bool		isnull;
	char	   *exprsString;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result. */
	if (relation->rd_indexprs)
		return copyObject(relation->rd_indexprs);

	/* Quick exit if there is nothing to do. */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indexprs, NULL))
		return NIL;

	/*
	 * We build the tree we intend to return in the caller's context. After
	 * successfully completing the work, we copy it into the relcache entry.
	 * This avoids problems if we get some sort of error partway through.
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
	 * Run the expressions through eval_const_expressions. This is not just an
	 * optimization, but is necessary, because the planner will be comparing
	 * them to similarly-processed qual clauses, and may fail to detect valid
	 * matches without this.  We must not use canonicalize_qual, however,
	 * since these aren't qual expressions.
	 */
	result = (List *) eval_const_expressions(NULL, (Node *) result);

	/* May as well fix opfuncids too */
	fix_opfuncids((Node *) result);

	/* Now save a copy of the completed tree in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_indexprs = copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * RelationGetDummyIndexExpressions -- get dummy expressions for an index
 *
 * Return a list of dummy expressions (just Const nodes) with the same
 * types/typmods/collations as the index's real expressions.  This is
 * useful in situations where we don't want to run any user-defined code.
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

	/* Quick exit if there is nothing to do. */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indexprs, NULL))
		return NIL;

	/* Extract raw node tree(s) from index tuple. */
	exprsDatum = heap_getattr(relation->rd_indextuple,
							  Anum_pg_index_indexprs,
							  GetPgIndexDescriptor(),
							  &isnull);
	Assert(!isnull);
	exprsString = TextDatumGetCString(exprsDatum);
	rawExprs = (List *) stringToNode(exprsString);
	pfree(exprsString);

	/* Construct null Consts; the typlen and typbyval are arbitrary. */
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
 * RelationGetIndexPredicate -- get the index predicate for an index
 *
 * We cache the result of transforming pg_index.indpred into an implicit-AND
 * node tree (suitable for use in planning).
 * If the rel is not an index or has no predicate, we return NIL.
 * Otherwise, the returned tree is copied into the caller's memory context.
 * (We don't want to return a pointer to the relcache copy, since it could
 * disappear due to relcache invalidation.)
 */
List *
RelationGetIndexPredicate(Relation relation)
{
	List	   *result;
	Datum		predDatum;
	bool		isnull;
	char	   *predString;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result. */
	if (relation->rd_indpred)
		return copyObject(relation->rd_indpred);

	/* Quick exit if there is nothing to do. */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indpred, NULL))
		return NIL;

	/*
	 * We build the tree we intend to return in the caller's context. After
	 * successfully completing the work, we copy it into the relcache entry.
	 * This avoids problems if we get some sort of error partway through.
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
	 * Run the expression through const-simplification and canonicalization.
	 * This is not just an optimization, but is necessary, because the planner
	 * will be comparing it to similarly-processed qual clauses, and may fail
	 * to detect valid matches without this.  This must match the processing
	 * done to qual clauses in preprocess_expression()!  (We can skip the
	 * stuff involving subqueries, however, since we don't allow any in index
	 * predicates.)
	 */
	result = (List *) eval_const_expressions(NULL, (Node *) result);

	result = (List *) canonicalize_qual((Expr *) result, false);

	/* Also convert to implicit-AND format */
	result = make_ands_implicit((Expr *) result);

	/* May as well fix opfuncids too */
	fix_opfuncids((Node *) result);

	/* Now save a copy of the completed tree in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(relation->rd_indexcxt);
	relation->rd_indpred = copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * RelationGetIndexAttrBitmap -- get a bitmap of index attribute numbers
 *
 * The result has a bit set for each attribute used anywhere in the index
 * definitions of all the indexes on this relation.  (This includes not only
 * simple index keys, but attributes used in expressions and partial-index
 * predicates.)
 *
 * Depending on attrKind, a bitmap covering attnums for certain columns is
 * returned:
 *	INDEX_ATTR_BITMAP_KEY			Columns in non-partial unique indexes not
 *									in expressions (i.e., usable for FKs)
 *	INDEX_ATTR_BITMAP_PRIMARY_KEY	Columns in the table's primary key
 *	INDEX_ATTR_BITMAP_IDENTITY_KEY	Columns in the table's replica identity
 *									index (empty if FULL)
 *	INDEX_ATTR_BITMAP_HOT_BLOCKING	Columns that block updates from being HOT
 *	INDEX_ATTR_BITMAP_SUMMARIZED	Columns included in summarizing indexes
 *
 * Attribute numbers are offset by FirstLowInvalidHeapAttributeNumber so that
 * we can include system attributes (e.g., OID) in the bitmap representation.
 *
 * Caller had better hold at least RowExclusiveLock on the target relation
 * to ensure it is safe (deadlock-free) for us to take locks on the relation's
 * indexes.  Note that since the introduction of CREATE INDEX CONCURRENTLY,
 * that lock level doesn't guarantee a stable set of indexes, so we have to
 * be prepared to retry here in case of a change in the set of indexes.
 *
 * The returned result is palloc'd in the caller's memory context and should
 * be bms_free'd when not needed anymore.
 */
Bitmapset *
RelationGetIndexAttrBitmap(Relation relation, IndexAttrBitmapKind attrKind)
{
	Bitmapset  *uindexattrs;	/* columns in unique indexes */
	Bitmapset  *pkindexattrs;	/* columns in the primary index */
	Bitmapset  *idindexattrs;	/* columns in the replica identity */
	Bitmapset  *hotblockingattrs;	/* columns with HOT blocking indexes */
	Bitmapset  *summarizedattrs;	/* columns with summarizing indexes */
	List	   *indexoidlist;
	List	   *newindexoidlist;
	Oid			relpkindex;
	Oid			relreplindex;
	ListCell   *l;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result. */
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

	/* Fast path if definitely no indexes */
	if (!RelationGetForm(relation)->relhasindex)
		return NULL;

	/*
	 * Get cached list of index OIDs. If we have to start over, we do so here.
	 */
restart:
	indexoidlist = RelationGetIndexList(relation);

	/* Fall out if no indexes (but relhasindex was set) */
	if (indexoidlist == NIL)
		return NULL;

	/*
	 * Copy the rd_pkindex and rd_replidindex values computed by
	 * RelationGetIndexList before proceeding.  This is needed because a
	 * relcache flush could occur inside index_open below, resetting the
	 * fields managed by RelationGetIndexList.  We need to do the work with
	 * stable values of these fields.
	 */
	relpkindex = relation->rd_pkindex;
	relreplindex = relation->rd_replidindex;

	/*
	 * For each index, add referenced attributes to indexattrs.
	 *
	 * Note: we consider all indexes returned by RelationGetIndexList, even if
	 * they are not indisready or indisvalid.  This is important because an
	 * index for which CREATE INDEX CONCURRENTLY has just started must be
	 * included in HOT-safety decisions (see README.HOT).  If a DROP INDEX
	 * CONCURRENTLY is far enough along that we should ignore the index, it
	 * won't be returned at all by RelationGetIndexList.
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
		bool		isKey;		/* candidate key */
		bool		isPK;		/* primary key */
		bool		isIDKey;	/* replica identity index */
		Bitmapset **attrs;

		indexDesc = index_open(indexOid, AccessShareLock);

		/*
		 * Extract index expressions and index predicate.  Note: Don't use
		 * RelationGetIndexExpressions()/RelationGetIndexPredicate(), because
		 * those might run constant expressions evaluation, which needs a
		 * snapshot, which we might not have here.  (Also, it's probably more
		 * sound to collect the bitmaps before any transformations that might
		 * eliminate columns, but the practical impact of this is limited.)
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

		/* Can this index be referenced by a foreign key? */
		isKey = indexDesc->rd_index->indisunique &&
			indexExpressions == NULL &&
			indexPredicate == NULL;

		/* Is this a primary key? */
		isPK = (indexOid == relpkindex);

		/* Is this index the configured (or default) replica identity? */
		isIDKey = (indexOid == relreplindex);

		/*
		 * If the index is summarizing, it doesn't block HOT updates, but we
		 * may still need to update it (if the attributes were modified). So
		 * decide which bitmap we'll update in the following loop.
		 */
		if (indexDesc->rd_indam->amsummarizing)
			attrs = &summarizedattrs;
		else
			attrs = &hotblockingattrs;

		/* Collect simple attribute references */
		for (i = 0; i < indexDesc->rd_index->indnatts; i++)
		{
			int			attrnum = indexDesc->rd_index->indkey.values[i];

			/*
			 * Since we have covering indexes with non-key columns, we must
			 * handle them accurately here. non-key columns must be added into
			 * hotblockingattrs or summarizedattrs, since they are in index,
			 * and update shouldn't miss them.
			 *
			 * Summarizing indexes do not block HOT, but do need to be updated
			 * when the column value changes, thus require a separate
			 * attribute bitmapset.
			 *
			 * Obviously, non-key columns couldn't be referenced by foreign
			 * key or identity key. Hence we do not include them into
			 * uindexattrs, pkindexattrs and idindexattrs bitmaps.
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

		/* Collect all attributes used in expressions, too */
		pull_varattnos(indexExpressions, 1, attrs);

		/* Collect all attributes in the index predicate, too */
		pull_varattnos(indexPredicate, 1, attrs);

		index_close(indexDesc, AccessShareLock);
	}

	/*
	 * During one of the index_opens in the above loop, we might have received
	 * a relcache flush event on this relcache entry, which might have been
	 * signaling a change in the rel's index list.  If so, we'd better start
	 * over to ensure we deliver up-to-date attribute bitmaps.
	 */
	newindexoidlist = RelationGetIndexList(relation);
	if (equal(indexoidlist, newindexoidlist) &&
		relpkindex == relation->rd_pkindex &&
		relreplindex == relation->rd_replidindex)
	{
		/* Still the same index set, so proceed */
		list_free(newindexoidlist);
		list_free(indexoidlist);
	}
	else
	{
		/* Gotta do it over ... might as well not leak memory */
		list_free(newindexoidlist);
		list_free(indexoidlist);
		bms_free(uindexattrs);
		bms_free(pkindexattrs);
		bms_free(idindexattrs);
		bms_free(hotblockingattrs);
		bms_free(summarizedattrs);

		goto restart;
	}

	/* Don't leak the old values of these bitmaps, if any */
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
	 * Now save copies of the bitmaps in the relcache entry.  We intentionally
	 * set rd_attrsvalid last, because that's the one that signals validity of
	 * the values; if we run out of memory before making that copy, we won't
	 * leave the relcache entry looking like the other ones are valid but
	 * empty.
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_keyattr = bms_copy(uindexattrs);
	relation->rd_pkattr = bms_copy(pkindexattrs);
	relation->rd_idattr = bms_copy(idindexattrs);
	relation->rd_hotblockingattr = bms_copy(hotblockingattrs);
	relation->rd_summarizedattr = bms_copy(summarizedattrs);
	relation->rd_attrsvalid = true;
	MemoryContextSwitchTo(oldcxt);

	/* We return our original working copy for caller to play with */
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
 * RelationGetIdentityKeyBitmap -- get a bitmap of replica identity attribute
 * numbers
 *
 * A bitmap of index attribute numbers for the configured replica identity
 * index is returned.
 *
 * See also comments of RelationGetIndexAttrBitmap().
 *
 * This is a special purpose function used during logical replication. Here,
 * unlike RelationGetIndexAttrBitmap(), we don't acquire a lock on the required
 * index as we build the cache entry using a historic snapshot and all the
 * later changes are absorbed while decoding WAL. Due to this reason, we don't
 * need to retry here in case of a change in the set of indexes.
 */
Bitmapset *
RelationGetIdentityKeyBitmap(Relation relation)
{
	Bitmapset  *idindexattrs = NULL;	/* columns in the replica identity */
	Relation	indexDesc;
	int			i;
	Oid			replidindex;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result */
	if (relation->rd_idattr != NULL)
		return bms_copy(relation->rd_idattr);

	/* Fast path if definitely no indexes */
	if (!RelationGetForm(relation)->relhasindex)
		return NULL;

	/* Historic snapshot must be set. */
	Assert(HistoricSnapshotActive());

	replidindex = RelationGetReplicaIndex(relation);

	/* Fall out if there is no replica identity index */
	if (!OidIsValid(replidindex))
		return NULL;

	/* Look up the description for the replica identity index */
	indexDesc = RelationIdGetRelation(replidindex);

	if (!RelationIsValid(indexDesc))
		elog(ERROR, "could not open relation with OID %u",
			 relation->rd_replidindex);

	/* Add referenced attributes to idindexattrs */
	for (i = 0; i < indexDesc->rd_index->indnatts; i++)
	{
		int			attrnum = indexDesc->rd_index->indkey.values[i];

		/*
		 * We don't include non-key columns into idindexattrs bitmaps. See
		 * RelationGetIndexAttrBitmap.
		 */
		if (attrnum != 0)
		{
			if (i < indexDesc->rd_index->indnkeyatts)
				idindexattrs = bms_add_member(idindexattrs,
											  attrnum - FirstLowInvalidHeapAttributeNumber);
		}
	}

	RelationClose(indexDesc);

	/* Don't leak the old values of these bitmaps, if any */
	bms_free(relation->rd_idattr);
	relation->rd_idattr = NULL;

	/* Now save copy of the bitmap in the relcache entry */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_idattr = bms_copy(idindexattrs);
	MemoryContextSwitchTo(oldcxt);

	/* We return our original working copy for caller to play with */
	return idindexattrs;
}

/*
 * RelationGetExclusionInfo -- get info about index's exclusion constraint
 *
 * This should be called only for an index that is known to have an
 * associated exclusion constraint.  It returns arrays (palloc'd in caller's
 * context) of the exclusion operator OIDs, their underlying functions'
 * OIDs, and their strategy numbers in the index's opclasses.  We cache
 * all this information since it requires a fair amount of work to get.
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

	/* Allocate result space in caller context */
	*operators = ops = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	*procs = funcs = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	*strategies = strats = (uint16 *) palloc(sizeof(uint16) * indnkeyatts);

	/* Quick exit if we have the data cached already */
	if (indexRelation->rd_exclstrats != NULL)
	{
		memcpy(ops, indexRelation->rd_exclops, sizeof(Oid) * indnkeyatts);
		memcpy(funcs, indexRelation->rd_exclprocs, sizeof(Oid) * indnkeyatts);
		memcpy(strats, indexRelation->rd_exclstrats, sizeof(uint16) * indnkeyatts);
		return;
	}

	/*
	 * Search pg_constraint for the constraint associated with the index. To
	 * make this not too painfully slow, we use the index on conrelid; that
	 * will hold the parent relation's OID not the index's own OID.
	 *
	 * Note: if we wanted to rely on the constraint name matching the index's
	 * name, we could just do a direct lookup using pg_constraint's unique
	 * index.  For the moment it doesn't seem worth requiring that.
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

		/* We want the exclusion constraint owning the index */
		if (conform->contype != CONSTRAINT_EXCLUSION ||
			conform->conindid != RelationGetRelid(indexRelation))
			continue;

		/* There should be only one */
		if (found)
			elog(ERROR, "unexpected exclusion constraint record found for rel %s",
				 RelationGetRelationName(indexRelation));
		found = true;

		/* Extract the operator OIDS from conexclop */
		val = fastgetattr(htup,
						  Anum_pg_constraint_conexclop,
						  conrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "null conexclop for rel %s",
				 RelationGetRelationName(indexRelation));

		arr = DatumGetArrayTypeP(val);	/* ensure not toasted */
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

	/* We need the func OIDs and strategy numbers too */
	for (i = 0; i < indnkeyatts; i++)
	{
		funcs[i] = get_opcode(ops[i]);
		strats[i] = get_op_opfamily_strategy(ops[i],
											 indexRelation->rd_opfamily[i]);
		/* shouldn't fail, since it was checked at index creation */
		if (strats[i] == InvalidStrategy)
			elog(ERROR, "could not find strategy for operator %u in family %u",
				 ops[i], indexRelation->rd_opfamily[i]);
	}

	/* Save a copy of the results in the relcache entry. */
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
 * Get the publication information for the given relation.
 *
 * Traverse all the publications which the relation is in to get the
 * publication actions and validate the row filter expressions for such
 * publications if any. We consider the row filter expression as invalid if it
 * references any column which is not part of REPLICA IDENTITY.
 *
 * To avoid fetching the publication information repeatedly, we cache the
 * publication actions and row filter validation information.
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
	 * If not publishable, it publishes no actions.  (pgoutput_change() will
	 * ignore it.)
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

	/* Fetch the publication membership info. */
	puboids = GetRelationPublications(relid);
	schemaid = RelationGetNamespace(relation);
	puboids = list_concat_unique_oid(puboids, GetSchemaPublications(schemaid));

	if (relation->rd_rel->relispartition)
	{
		/* Add publications that the ancestors are in too. */
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
		 * Check if all columns referenced in the filter expression are part
		 * of the REPLICA IDENTITY index or not.
		 *
		 * If the publication is FOR ALL TABLES then it means the table has no
		 * row filters and we can skip the validation.
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
		 * Check if all columns are part of the REPLICA IDENTITY index or not.
		 *
		 * If the publication is FOR ALL TABLES then it means the table has no
		 * column list and we can skip the validation.
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
		 * If we know everything is replicated and the row filter is invalid
		 * for update and delete, there is no point to check for other
		 * publications.
		 */
		if (pubdesc->pubactions.pubinsert && pubdesc->pubactions.pubupdate &&
			pubdesc->pubactions.pubdelete && pubdesc->pubactions.pubtruncate &&
			!pubdesc->rf_valid_for_update && !pubdesc->rf_valid_for_delete)
			break;

		/*
		 * If we know everything is replicated and the column list is invalid
		 * for update and delete, there is no point to check for other
		 * publications.
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

	/* Now save copy of the descriptor in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_pubdesc = palloc(sizeof(PublicationDesc));
	memcpy(relation->rd_pubdesc, pubdesc, sizeof(PublicationDesc));
	MemoryContextSwitchTo(oldcxt);
}

/*
 * RelationGetIndexRawAttOptions -- get AM/opclass-specific options for the index
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
 *		get AM/opclass-specific options for an index parsed into a binary form
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

	/* Try to copy cached options. */
	if (opts)
		return copy ? CopyIndexAttOptions(opts, natts) : opts;

	/* Get and parse opclass options. */
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

	/* Copy parsed options to the cache. */
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
 * Routines to support ereport() reports of relation-related errors
 *
 * These could have been put into elog.c, but it seems like a module layering
 * violation to have elog.c calling relcache or syscache stuff --- and we
 * definitely don't want elog.h including rel.h.  So we put them here.
 */

/*
 * errtable --- stores schema_name and table_name of a table
 * within the current errordata.
 */
int
errtable(Relation rel)
{
	err_generic_string(PG_DIAG_SCHEMA_NAME,
					   get_namespace_name(RelationGetNamespace(rel)));
	err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(rel));

	return 0;					/* return value does not matter */
}

/*
 * errtablecol --- stores schema_name, table_name and column_name
 * of a table column within the current errordata.
 *
 * The column is specified by attribute number --- for most callers, this is
 * easier and less error-prone than getting the column name for themselves.
 */
int
errtablecol(Relation rel, int attnum)
{
	TupleDesc	reldesc = RelationGetDescr(rel);
	const char *colname;

	/* Use reldesc if it's a user attribute, else consult the catalogs */
	if (attnum > 0 && attnum <= reldesc->natts)
		colname = NameStr(TupleDescAttr(reldesc, attnum - 1)->attname);
	else
		colname = get_attname(RelationGetRelid(rel), attnum, false);

	return errtablecolname(rel, colname);
}

/*
 * errtablecolname --- stores schema_name, table_name and column_name
 * of a table column within the current errordata, where the column name is
 * given directly rather than extracted from the relation's catalog data.
 *
 * Don't use this directly unless errtablecol() is inconvenient for some
 * reason.  This might possibly be needed during intermediate states in ALTER
 * TABLE, for instance.
 */
int
errtablecolname(Relation rel, const char *colname)
{
	errtable(rel);
	err_generic_string(PG_DIAG_COLUMN_NAME, colname);

	return 0;					/* return value does not matter */
}

/*
 * errtableconstraint --- stores schema_name, table_name and constraint_name
 * of a table-related constraint within the current errordata.
 */
int
errtableconstraint(Relation rel, const char *conname)
{
	errtable(rel);
	err_generic_string(PG_DIAG_CONSTRAINT_NAME, conname);

	return 0;					/* return value does not matter */
}


/*
 *	load_relcache_init_file, write_relcache_init_file
 *
 *		In late 1992, we started regularly having databases with more than
 *		a thousand classes in them.  With this number of classes, it became
 *		critical to do indexed lookups on the system catalogs.
 *
 *		Bootstrapping these lookups is very hard.  We want to be able to
 *		use an index on pg_attribute, for example, but in order to do so,
 *		we must have read pg_attribute for the attributes in the index,
 *		which implies that we need to use the index.
 *
 *		In order to get around the problem, we do the following:
 *
 *		   +  When the database system is initialized (at initdb time), we
 *			  don't use indexes.  We do sequential scans.
 *
 *		   +  When the backend is started up in normal mode, we load an image
 *			  of the appropriate relation descriptors, in internal format,
 *			  from an initialization file in the data/base/... directory.
 *
 *		   +  If the initialization file isn't there, then we create the
 *			  relation descriptors using sequential scans and write 'em to
 *			  the initialization file for use by subsequent backends.
 *
 *		As of Postgres 9.0, there is one local initialization file in each
 *		database, plus one shared initialization file for shared catalogs.
 *
 *		We could dispense with the initialization files and just build the
 *		critical reldescs the hard way on every backend startup, but that
 *		slows down backend startup noticeably.
 *
 *		We can in fact go further, and save more relcache entries than
 *		just the ones that are absolutely critical; this allows us to speed
 *		up backend startup by not having to build such entries the hard way.
 *		Presently, all the catalog and index entries that are referred to
 *		by catcaches are stored in the initialization files.
 *
 *		The same mechanism that detects when catcache and relcache entries
 *		need to be invalidated (due to catalog updates) also arranges to
 *		unlink the initialization files when the contents may be out of date.
 *		The files will then be rebuilt during the next backend startup.
 */

/*
 * load_relcache_init_file -- attempt to load cache from the shared
 * or local cache init file
 *
 * If successful, return true and set criticalRelcachesBuilt or
 * criticalSharedRelcachesBuilt to true.
 * If not successful, return false.
 *
 * NOTE: we assume we are already switched into CacheMemoryContext.
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
	 * Read the index relcache entries from the file.  Note we will not enter
	 * any of them into the cache if the read fails partway through; this
	 * helps to guard against broken init files.
	 */
	max_rels = 100;
	rels = (Relation *) palloc(max_rels * sizeof(Relation));
	num_rels = 0;
	nailed_rels = nailed_indexes = 0;

	/* check for correct magic number (compatible version) */
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

		/* first read the relation descriptor length */
		nread = fread(&len, 1, sizeof(len), fp);
		if (nread != sizeof(len))
		{
			if (nread == 0)
				break;			/* end of file */
			goto read_failed;
		}

		/* safety check for incompatible relcache layout */
		if (len != sizeof(RelationData))
			goto read_failed;

		/* allocate another relcache header */
		if (num_rels >= max_rels)
		{
			max_rels *= 2;
			rels = (Relation *) repalloc(rels, max_rels * sizeof(Relation));
		}

		rel = rels[num_rels++] = (Relation) palloc(len);

		/* then, read the Relation structure */
		if (fread(rel, 1, len, fp) != len)
			goto read_failed;

		/* next read the relation tuple form */
		if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
			goto read_failed;

		relform = (Form_pg_class) palloc(len);
		if (fread(relform, 1, len, fp) != len)
			goto read_failed;

		rel->rd_rel = relform;

		/* initialize attribute tuple forms */
		rel->rd_att = CreateTemplateTupleDesc(relform->relnatts);
		rel->rd_att->tdrefcount = 1;	/* mark as refcounted */

		rel->rd_att->tdtypeid = relform->reltype ? relform->reltype : RECORDOID;
		rel->rd_att->tdtypmod = -1; /* just to be sure */

		/* next read all the attribute tuple form data entries */
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

		/* next read the access method specific field */
		if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
			goto read_failed;
		if (len > 0)
		{
			rel->rd_options = palloc(len);
			if (fread(rel->rd_options, 1, len, fp) != len)
				goto read_failed;
			if (len != VARSIZE(rel->rd_options))
				goto read_failed;	/* sanity check */
		}
		else
		{
			rel->rd_options = NULL;
		}

		/* mark not-null status */
		if (has_not_null)
		{
			TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

			constr->has_not_null = true;
			rel->rd_att->constr = constr;
		}

		/*
		 * If it's an index, there's more to do.  Note we explicitly ignore
		 * partitioned indexes here.
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

			/* Count nailed indexes to ensure we have 'em all */
			if (rel->rd_isnailed)
				nailed_indexes++;

			/* read the pg_index tuple */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			rel->rd_indextuple = (HeapTuple) palloc(len);
			if (fread(rel->rd_indextuple, 1, len, fp) != len)
				goto read_failed;

			/* Fix up internal pointers in the tuple -- see heap_copytuple */
			rel->rd_indextuple->t_data = (HeapTupleHeader) ((char *) rel->rd_indextuple + HEAPTUPLESIZE);
			rel->rd_index = (Form_pg_index) GETSTRUCT(rel->rd_indextuple);

			/*
			 * prepare index info context --- parameters should match
			 * RelationInitIndexAccessInfo
			 */
			indexcxt = AllocSetContextCreate(CacheMemoryContext,
											 "index info",
											 ALLOCSET_SMALL_SIZES);
			rel->rd_indexcxt = indexcxt;
			MemoryContextCopyAndSetIdentifier(indexcxt,
											  RelationGetRelationName(rel));

			/*
			 * Now we can fetch the index AM's API struct.  (We can't store
			 * that in the init file, since it contains function pointers that
			 * might vary across server executions.  Fortunately, it should be
			 * safe to call the amhandler even while bootstrapping indexes.)
			 */
			InitIndexAmRoutine(rel);

			/* read the vector of opfamily OIDs */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			opfamily = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(opfamily, 1, len, fp) != len)
				goto read_failed;

			rel->rd_opfamily = opfamily;

			/* read the vector of opcintype OIDs */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			opcintype = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(opcintype, 1, len, fp) != len)
				goto read_failed;

			rel->rd_opcintype = opcintype;

			/* read the vector of support procedure OIDs */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;
			support = (RegProcedure *) MemoryContextAlloc(indexcxt, len);
			if (fread(support, 1, len, fp) != len)
				goto read_failed;

			rel->rd_support = support;

			/* read the vector of collation OIDs */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			indcollation = (Oid *) MemoryContextAlloc(indexcxt, len);
			if (fread(indcollation, 1, len, fp) != len)
				goto read_failed;

			rel->rd_indcollation = indcollation;

			/* read the vector of indoption values */
			if (fread(&len, 1, sizeof(len), fp) != sizeof(len))
				goto read_failed;

			indoption = (int16 *) MemoryContextAlloc(indexcxt, len);
			if (fread(indoption, 1, len, fp) != len)
				goto read_failed;

			rel->rd_indoption = indoption;

			/* read the vector of opcoptions values */
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

			/* set up zeroed fmgr-info vector */
			nsupport = relform->relnatts * rel->rd_indam->amsupport;
			rel->rd_supportinfo = (FmgrInfo *)
				MemoryContextAllocZero(indexcxt, nsupport * sizeof(FmgrInfo));
		}
		else
		{
			/* Count nailed rels to ensure we have 'em all */
			if (rel->rd_isnailed)
				nailed_rels++;

			/* Load table AM data */
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
		 * Rules and triggers are not saved (mainly because the internal
		 * format is complex and subject to change).  They must be rebuilt if
		 * needed by RelationCacheInitializePhase3.  This is not expected to
		 * be a big performance hit since few system catalogs have such. Ditto
		 * for RLS policy data, partition info, index expressions, predicates,
		 * exclusion info, and FDW info.
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
		 * Reset transient-state fields in the relcache entry
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
		 * Recompute lock and physical addressing info.  This is needed in
		 * case the pg_internal.init file was copied from some other database
		 * by CREATE DATABASE.
		 */
		RelationInitLockInfo(rel);
		RelationInitPhysicalAddr(rel);
	}

	/*
	 * We reached the end of the init file without apparent problem.  Did we
	 * get the right number of nailed items?  This is a useful crosscheck in
	 * case the set of critical rels or indexes changes.  However, that should
	 * not happen in a normally-running system, so let's bleat if it does.
	 *
	 * For the shared init file, we're called before client authentication is
	 * done, which means that elog(WARNING) will go only to the postmaster
	 * log, where it's easily missed.  To ensure that developers notice bad
	 * values of NUM_CRITICAL_SHARED_RELS/NUM_CRITICAL_SHARED_INDEXES, we put
	 * an Assert(false) there.
	 */
	if (shared)
	{
		if (nailed_rels != NUM_CRITICAL_SHARED_RELS ||
			nailed_indexes != NUM_CRITICAL_SHARED_INDEXES)
		{
			elog(WARNING, "found %d nailed shared rels and %d nailed shared indexes in init file, but expected %d and %d respectively",
				 nailed_rels, nailed_indexes,
				 NUM_CRITICAL_SHARED_RELS, NUM_CRITICAL_SHARED_INDEXES);
			/* Make sure we get developers' attention about this */
			Assert(false);
			/* In production builds, recover by bootstrapping the relcache */
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
			/* We don't need an Assert() in this case */
			goto read_failed;
		}
	}

	/*
	 * OK, all appears well.
	 *
	 * Now insert all the new relcache entries into the cache.
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
	 * init file is broken, so do it the hard way.  We don't bother trying to
	 * free the clutter we just allocated; it's not in the relcache so it
	 * won't hurt.
	 */
read_failed:
	pfree(rels);
	FreeFile(fp);

	return false;
}

/*
 * Write out a new initialization file with the current contents
 * of the relcache (either shared rels or local rels, as indicated).
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
	 * If we have already received any relcache inval events, there's no
	 * chance of succeeding so we may as well skip the whole thing.
	 */
	if (relcacheInvalsReceived != 0L)
		return;

	/*
	 * We must write a temporary file and rename it into place. Otherwise,
	 * another backend starting at about the same time might crash trying to
	 * read the partially-complete file.
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

	unlink(tempfilename);		/* in case it exists w/wrong permissions */

	fp = AllocateFile(tempfilename, PG_BINARY_W);
	if (fp == NULL)
	{
		/*
		 * We used to consider this a fatal error, but we might as well
		 * continue with backend startup ...
		 */
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not create relation-cache initialization file \"%s\": %m",
						tempfilename),
				 errdetail("Continuing anyway, but there's something wrong.")));
		return;
	}

	/*
	 * Write a magic number to serve as a file version identifier.  We can
	 * change the magic number whenever the relcache layout changes.
	 */
	magic = RELCACHE_INIT_FILEMAGIC;
	if (fwrite(&magic, 1, sizeof(magic), fp) != sizeof(magic))
		elog(FATAL, "could not write init file");

	/*
	 * Write all the appropriate reldescs (in no particular order).
	 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	rel = idhentry->reldesc;
		Form_pg_class relform = rel->rd_rel;

		/* ignore if not correct group */
		if (relform->relisshared != shared)
			continue;

		/*
		 * Ignore if not supposed to be in init file.  We can allow any shared
		 * relation that's been loaded so far to be in the shared init file,
		 * but unshared relations must be ones that should be in the local
		 * file per RelationIdIsInInitFile.  (Note: if you want to change the
		 * criterion for rels to be kept in the init file, see also inval.c.
		 * The reason for filtering here is to be sure that we don't put
		 * anything into the local init file for which a relcache inval would
		 * not cause invalidation of that init file.)
		 */
		if (!shared && !RelationIdIsInInitFile(RelationGetRelid(rel)))
		{
			/* Nailed rels had better get stored. */
			Assert(!rel->rd_isnailed);
			continue;
		}

		/* first write the relcache entry proper */
		write_item(rel, sizeof(RelationData), fp);

		/* next write the relation tuple form */
		write_item(relform, CLASS_TUPLE_SIZE, fp);

		/* next, do all the attribute tuple form data entries */
		for (i = 0; i < relform->relnatts; i++)
		{
			write_item(TupleDescAttr(rel->rd_att, i),
					   ATTRIBUTE_FIXED_PART_SIZE, fp);
		}

		/* next, do the access method specific field */
		write_item(rel->rd_options,
				   (rel->rd_options ? VARSIZE(rel->rd_options) : 0),
				   fp);

		/*
		 * If it's an index, there's more to do. Note we explicitly ignore
		 * partitioned indexes here.
		 */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			/* write the pg_index tuple */
			/* we assume this was created by heap_copytuple! */
			write_item(rel->rd_indextuple,
					   HEAPTUPLESIZE + rel->rd_indextuple->t_len,
					   fp);

			/* write the vector of opfamily OIDs */
			write_item(rel->rd_opfamily,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* write the vector of opcintype OIDs */
			write_item(rel->rd_opcintype,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* write the vector of support procedure OIDs */
			write_item(rel->rd_support,
					   relform->relnatts * (rel->rd_indam->amsupport * sizeof(RegProcedure)),
					   fp);

			/* write the vector of collation OIDs */
			write_item(rel->rd_indcollation,
					   relform->relnatts * sizeof(Oid),
					   fp);

			/* write the vector of indoption values */
			write_item(rel->rd_indoption,
					   relform->relnatts * sizeof(int16),
					   fp);

			Assert(rel->rd_opcoptions);

			/* write the vector of opcoptions values */
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
	 * Now we have to check whether the data we've so painstakingly
	 * accumulated is already obsolete due to someone else's just-committed
	 * catalog changes.  If so, we just delete the temp file and leave it to
	 * the next backend to try again.  (Our own relcache entries will be
	 * updated by SI message processing, but we can't be sure whether what we
	 * wrote out was up-to-date.)
	 *
	 * This mustn't run concurrently with the code that unlinks an init file
	 * and sends SI messages, so grab a serialization lock for the duration.
	 */
	LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);

	/* Make sure we have seen all incoming SI messages */
	AcceptInvalidationMessages();

	/*
	 * If we have received any SI relcache invals since backend start, assume
	 * we may have written out-of-date data.
	 */
	if (relcacheInvalsReceived == 0L)
	{
		/*
		 * OK, rename the temp file to its final name, deleting any
		 * previously-existing init file.
		 *
		 * Note: a failure here is possible under Cygwin, if some other
		 * backend is holding open an unlinked-but-not-yet-gone init file. So
		 * treat this as a noncritical failure; just remove the useless temp
		 * file on failure.
		 */
		if (rename(tempfilename, finalfilename) < 0)
			unlink(tempfilename);
	}
	else
	{
		/* Delete the already-obsolete temp file */
		unlink(tempfilename);
	}

	LWLockRelease(RelCacheInitLock);
}

/* write a chunk of data preceded by its length */
static void
write_item(const void *data, Size len, FILE *fp)
{
	if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
		elog(FATAL, "could not write init file");
	if (len > 0 && fwrite(data, 1, len, fp) != len)
		elog(FATAL, "could not write init file");
}

/*
 * Determine whether a given relation (identified by OID) is one of the ones
 * we should store in a relcache init file.
 *
 * We must cache all nailed rels, and for efficiency we should cache every rel
 * that supports a syscache.  The former set is almost but not quite a subset
 * of the latter. The special cases are relations where
 * RelationCacheInitializePhase2/3 chooses to nail for efficiency reasons, but
 * which do not support any syscache.
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
		 * If this Assert fails, we don't need the applicable special case
		 * anymore.
		 */
		Assert(!RelationSupportsSysCache(relationId));
		return true;
	}
	return RelationSupportsSysCache(relationId);
}

/*
 * Invalidate (remove) the init file during commit of a transaction that
 * changed one or more of the relation cache entries that are kept in the
 * local init file.
 *
 * To be safe against concurrent inspection or rewriting of the init file,
 * we must take RelCacheInitLock, then remove the old init file, then send
 * the SI messages that include relcache inval for such relations, and then
 * release RelCacheInitLock.  This serializes the whole affair against
 * write_relcache_init_file, so that we can be sure that any other process
 * that's concurrently trying to create a new init file won't move an
 * already-stale version into place after we unlink.  Also, because we unlink
 * before sending the SI messages, a backend that's currently starting cannot
 * read the now-obsolete init file and then miss the SI messages that will
 * force it to update its relcache entries.  (This works because the backend
 * startup sequence gets into the sinval array before trying to load the init
 * file.)
 *
 * We take the lock and do the unlink in RelationCacheInitFilePreInvalidate,
 * then release the lock in RelationCacheInitFilePostInvalidate.  Caller must
 * send any pending SI messages between those calls.
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
	 * The files might not be there if no backend has been started since the
	 * last removal.  But complain about failures other than ENOENT with
	 * ERROR.  Fortunately, it's not too late to abort the transaction if we
	 * can't get rid of the would-be-obsolete init file.
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
 * Remove the init files during postmaster startup.
 *
 * We used to keep the init files across restarts, but that is unsafe in PITR
 * scenarios, and even in simple crash-recovery cases there are windows for
 * the init files to become out-of-sync with the database.  So now we just
 * remove them during startup and expect the first backend launch to rebuild
 * them.  Of course, this has to happen in each database of the cluster.
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

	/* Scan everything in the default tablespace */
	RelationCacheInitFileRemoveInDir("base");

	/* Scan the tablespace link directory to find non-default tablespaces */
	dir = AllocateDir(tblspcdir);

	while ((de = ReadDirExtended(dir, tblspcdir, LOG)) != NULL)
	{
		if (strspn(de->d_name, "0123456789") == strlen(de->d_name))
		{
			/* Scan the tablespace dir for per-database dirs */
			snprintf(path, sizeof(path), "%s/%s/%s",
					 tblspcdir, de->d_name, TABLESPACE_VERSION_DIRECTORY);
			RelationCacheInitFileRemoveInDir(path);
		}
	}

	FreeDir(dir);
}

/* Process one per-tablespace directory for RelationCacheInitFileRemove */
static void
RelationCacheInitFileRemoveInDir(const char *tblspcpath)
{
	DIR		   *dir;
	struct dirent *de;
	char		initfilename[MAXPGPATH * 2];

	/* Scan the tablespace directory to find per-database directories */
	dir = AllocateDir(tblspcpath);

	while ((de = ReadDirExtended(dir, tblspcpath, LOG)) != NULL)
	{
		if (strspn(de->d_name, "0123456789") == strlen(de->d_name))
		{
			/* Try to remove the init file in each database */
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
		/* It might not be there, but log any error other than ENOENT */
		if (errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove cache file \"%s\": %m",
							initfilename)));
	}
}
