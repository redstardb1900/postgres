/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/tupdesc.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_publication.h"
#include "nodes/bitmapset.h"
#include "partitioning/partdefs.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"


/*
 * LockRelId 与 LockInfo 本属 lmgr.h，但在此声明更方便，
 * 以便 Relation 中可有 LockInfoData 字段。
 */

typedef struct LockRelId
{
	Oid			relId;			/*  关系标识符  */
	Oid			dbId;			/*  数据库标识符  */
} LockRelId;

typedef struct LockInfoData
{
	LockRelId	lockRelId;
} LockInfoData;

typedef LockInfoData *LockInfo;

/*
 * 以下是关系缓存项的内容。
 */

typedef struct RelationData
{
	RelFileLocator rd_locator;	/*  关系物理标识符  */
	SMgrRelation rd_smgr;		/* cached file handle, or NULL */
	int			rd_refcnt;		/*  引用计数  */
	BackendId	rd_backend;		/*  所属后端 id,若为临时关系  */
	bool		rd_islocaltemp; /*  本会话的临时关系  */
	bool		rd_isnailed;	/*  钉在缓存中  */
	bool		rd_isvalid;		/*  relcache 项有效  */
	bool		rd_indexvalid;	/*  rd_indexlist 有效?(还有 rd_pkindex 与
								 * rd_replidindex) */
	bool		rd_statvalid;	/*  rd_statlist 有效?  */

	/*----------
	 * rd_createSubid is the ID of the highest subtransaction the rel has
	 * survived into or zero if the rel or its storage was created before the
	 * current top transaction.  (IndexStmt.oldNumber leads to the case of a new
	 * rel with an old rd_locator.)  rd_firstRelfilelocatorSubid is the ID of the
	 * highest subtransaction an rd_locator change has survived into or zero if
	 * rd_locator matches the value it had at the start of the current top
	 * transaction.  (Rolling back the subtransaction that
	 * rd_firstRelfilelocatorSubid denotes would restore rd_locator to the value it
	 * had at the start of the current top transaction.  Rolling back any
	 * lower subtransaction would not.)  Their accuracy is critical to
	 * RelationNeedsWAL().
	 *
	 * rd_newRelfilelocatorSubid is the ID of the highest subtransaction the
	 * most-recent relfilenumber change has survived into or zero if not changed
	 * in the current transaction (or we have forgotten changing it).  This
	 * field is accurate when non-zero, but it can be zero when a relation has
	 * multiple new relfilenumbers within a single transaction, with one of them
	 * occurring in a subsequently aborted subtransaction, e.g.
	 *		BEGIN;
	 *		TRUNCATE t;
	 *		SAVEPOINT save;
	 *		TRUNCATE t;
	 *		ROLLBACK TO save;
	 *		-- rd_newRelfilelocatorSubid is now forgotten
	 *
	 * If every rd_*Subid field is zero, they are read-only outside
	 * relcache.c.  Files that trigger rd_locator changes by updating
	 * pg_class.reltablespace and/or pg_class.relfilenode call
	 * RelationAssumeNewRelfilelocator() to update rd_*Subid.
	 *
	 * rd_droppedSubid is the ID of the highest subtransaction that a drop of
	 * the rel has survived into.  In entries visible outside relcache.c, this
	 * is always zero.
	 */
	SubTransactionId rd_createSubid;	/*  关系在当前事务创建  */
	SubTransactionId rd_newRelfilelocatorSubid; /* highest subxact changing
												 * rd_locator to current value */
	SubTransactionId rd_firstRelfilelocatorSubid;	/* highest subxact
													 * changing rd_locator to
													 * any value */
	SubTransactionId rd_droppedSubid;	/* dropped with another Subid set */

	Form_pg_class rd_rel;		/* RELATION tuple */
	TupleDesc	rd_att;			/*  元组描述符 */
	Oid			rd_id;			/*  关系的对象 id  */
	LockInfoData rd_lockInfo;	/*  锁管理器锁定关系的信息  */
	RuleLock   *rd_rules;		/*  改写规则 */
	MemoryContext rd_rulescxt;	/*  rd_rules 私有内存上下文,若有  */
	TriggerDesc *trigdesc;		/*  触发器信息,无则 NULL  */
	/*  用 "struct" 以免需含 rowsecurity.h:  */
	struct RowSecurityDesc *rd_rsdesc;	/*  行安全策略,或 NULL  */

	/* data managed by RelationGetFKeyList: */
	List	   *rd_fkeylist;	/*  ForeignKeyCacheInfo 列表(见下)  */
	bool		rd_fkeyvalid;	/*  列表已计算则为真  */

	/* data managed by RelationGetPartitionKey: */
	PartitionKey rd_partkey;	/*  分区键,或 NULL  */
	MemoryContext rd_partkeycxt;	/*  rd_partkey 的私有上下文,若有  */

	/* data managed by RelationGetPartitionDesc: */
	PartitionDesc rd_partdesc;	/*  分区描述符,或 NULL  */
	MemoryContext rd_pdcxt;		/*  rd_partdesc 的私有上下文,若有  */

	/*  同上,但用于省略已分离分区的 partdesc  */
	PartitionDesc rd_partdesc_nodetached;	/* partdesc w/o detached parts */
	MemoryContext rd_pddcxt;	/*  供 rd_partdesc_nodetached 用  */

	/*
	 * pg_inherits.xmin of the partition that was excluded in
	 * rd_partdesc_nodetached.  This informs a future user of that partdesc:
	 * if this value is not in progress for the active snapshot, then the
	 * partdesc can be used, otherwise they have to build a new one.  (This
	 * matches what find_inheritance_children_extended would do).
	 */
	TransactionId rd_partdesc_nodetached_xmin;

	/* data managed by RelationGetPartitionQual: */
	List	   *rd_partcheck;	/*  分区 CHECK 限定  */
	bool		rd_partcheckvalid;	/*  列表已计算则为真  */
	MemoryContext rd_partcheckcxt;	/* private cxt for rd_partcheck, if any */

	/* data managed by RelationGetIndexList: */
	List	   *rd_indexlist;	/*  关系上索引的 OIDs 列表  */
	Oid			rd_pkindex;		/*  主键 OID,若有  */
	Oid			rd_replidindex; /*  副本标识索引 OID,若有  */

	/* data managed by RelationGetStatExtList: */
	List	   *rd_statlist;	/*  扩展统计 OIDs 列表  */

	/* data managed by RelationGetIndexAttrBitmap: */
	bool		rd_attrsvalid;	/*  属性位图有效?  */
	Bitmapset  *rd_keyattr;		/*  可被外键引用的列  */
	Bitmapset  *rd_pkattr;		/*  包含在主键中的列  */
	Bitmapset  *rd_idattr;		/*  包含在副本标识索引中  */
	Bitmapset  *rd_hotblockingattr; /*  阻塞 HOT 更新的列 */
	Bitmapset  *rd_summarizedattr;	/*  由汇总索引索引的列  */

	PublicationDesc *rd_pubdesc;	/*  发布描述符,或 NULL  */

	/*
	 * rd_options is set whenever rd_rel is loaded into the relcache entry.
	 * Note that you can NOT look into rd_rel for this data.  NULL means "use
	 * defaults".
	 */
	bytea	   *rd_options;		/* parsed pg_class.reloptions */

	/*
	 * Oid of the handler for this relation. For an index this is a function
	 * returning IndexAmRoutine, for table like relations a function returning
	 * TableAmRoutine.  This is stored separately from rd_indam, rd_tableam as
	 * its lookup requires syscache access, but during relcache bootstrap we
	 * need to be able to initialize rd_tableam without syscache lookups.
	 */
	Oid			rd_amhandler;	/*  索引 AM 处理函数的 OID  */

	/*
	 * 表访问方法。
	 */
	const struct TableAmRoutine *rd_tableam;

	/*  这些仅对索引关系非 NULL:  */
	Form_pg_index rd_index;		/*  描述本索引的 pg_index 元组  */
	/*  用 "struct" 以免需含 htup.h:  */
	struct HeapTupleData *rd_indextuple;	/* all of pg_index tuple */

	/*
	 * index access support info (used only for an index relation)
	 *
	 * Note: only default support procs for each opclass are cached, namely
	 * those with lefttype and righttype equal to the opclass's opcintype. The
	 * arrays are indexed by support function number, which is a sufficient
	 * identifier given that restriction.
	 */
	MemoryContext rd_indexcxt;	/* private memory cxt for this stuff */
	/*  用 "struct" 以免需含 amapi.h:  */
	struct IndexAmRoutine *rd_indam;	/* index AM's API struct */
	Oid		   *rd_opfamily;	/*  各索引列操作符族 OIDs  */
	Oid		   *rd_opcintype;	/*  opclass 声明输入类型 OIDs  */
	RegProcedure *rd_support;	/*  支持过程的 OIDs  */
	struct FmgrInfo *rd_supportinfo;	/*  支持过程的查找信息  */
	int16	   *rd_indoption;	/*  每列 AM 专用标志  */
	List	   *rd_indexprs;	/*  索引表达式树,若有  */
	List	   *rd_indpred;		/*  索引谓词树,若有  */
	Oid		   *rd_exclops;		/*  排他操作符 OIDs,若有  */
	Oid		   *rd_exclprocs;	/*  排他操作符过程 OIDs,若有  */
	uint16	   *rd_exclstrats;	/*  排他操作符策略号,若有  */
	Oid		   *rd_indcollation;	/* OIDs of index collations */
	bytea	  **rd_opcoptions;	/* parsed opclass-specific options */

	/*
	 * rd_amcache is available for index and table AMs to cache private data
	 * about the relation.  This must be just a cache since it may get reset
	 * at any time (in particular, it will get reset by a relcache inval
	 * message for the relation).  If used, it must point to a single memory
	 * chunk palloc'd in CacheMemoryContext, or in rd_indexcxt for an index
	 * relation.  A relcache reset will include freeing that chunk and setting
	 * rd_amcache = NULL.
	 */
	void	   *rd_amcache;		/*  供索引/表 AM 使用  */

	/*
	 * foreign-table support
	 *
	 * rd_fdwroutine must point to a single memory chunk palloc'd in
	 * CacheMemoryContext.  It will be freed and reset to NULL on a relcache
	 * reset.
	 */

	/*  用 "struct" 以免需含 fdwapi.h:  */
	struct FdwRoutine *rd_fdwroutine;	/*  缓存的函数指针,或 NULL  */

	/*
	 * Hack for CLUSTER, rewriting ALTER TABLE, etc: when writing a new
	 * version of a table, we need to make any toast pointers inserted into it
	 * have the existing toast table's OID, not the OID of the transient toast
	 * table.  If rd_toastoid isn't InvalidOid, it is the OID to place in
	 * toast pointers inserted into this rel.  (Note it's set on the new
	 * version of the main heap, not the toast table itself.)  This also
	 * causes toast_save_datum() to try to preserve toast value OIDs.
	 */
	Oid			rd_toastoid;	/*  真实 TOAST 表 OID,或 InvalidOid  */

	bool		pgstat_enabled; /*  是否应统计关系统计  */
	/*  用 "struct" 以免需含 pgstat.h:  */
	struct PgStat_TableStatus *pgstat_info; /*  统计收集区  */
} RelationData;


/*
 * ForeignKeyCacheInfo
 *		Information the relcache can cache about foreign key constraints
 *
 * This is basically just an image of relevant columns from pg_constraint.
 * We make it a subclass of Node so that copyObject() can be used on a list
 * of these, but we also ensure it is a "flat" object without substructure,
 * so that list_free_deep() is sufficient to free such a list.
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.
 *
 * Currently, we mostly cache fields of interest to the planner, but the set
 * of fields has already grown the constraint OID for other uses.
 */
typedef struct ForeignKeyCacheInfo
{
	pg_node_attr(no_equal, no_read, no_query_jumble)

	NodeTag		type;
	/*  约束自身的 oid  */
	Oid			conoid;
	/*  受外键约束的关系  */
	Oid			conrelid;
	/*  外键引用的关系  */
	Oid			confrelid;
	/*  外键中的列数  */
	int			nkeys;

	/*
	 * 这些数组各有 nkeys 个有效项:
	 */
	/*  引用表中的列  */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/*  被引用表中的列  */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* PK = FK operator OIDs */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
} ForeignKeyCacheInfo;


/*
 * StdRdOptions
 *		Standard contents of rd_options for heaps.
 *
 * RelationGetFillFactor() and RelationGetTargetPageFreeSpace() can only
 * be applied to relations that use this format or a superset for
 * private options data.
 */
 /* autovacuum-related reloptions. */
typedef struct AutoVacOpts
{
	bool		enabled;
	int			vacuum_threshold;
	int			vacuum_ins_threshold;
	int			analyze_threshold;
	int			vacuum_cost_limit;
	int			freeze_min_age;
	int			freeze_max_age;
	int			freeze_table_age;
	int			multixact_freeze_min_age;
	int			multixact_freeze_max_age;
	int			multixact_freeze_table_age;
	int			log_min_duration;
	float8		vacuum_cost_delay;
	float8		vacuum_scale_factor;
	float8		vacuum_ins_scale_factor;
	float8		analyze_scale_factor;
} AutoVacOpts;

/*  StdRdOptions->vacuum_index_cleanup 值  */
typedef enum StdRdOptIndexCleanup
{
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO = 0,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON
} StdRdOptIndexCleanup;

typedef struct StdRdOptions
{
	int32		vl_len_;		/*  varlena 头(勿直接触碰!)  */
	int			fillfactor;		/*  页填充因子百分比(0..100)  */
	int			toast_tuple_target; /*  元组 toast 目标  */
	AutoVacOpts autovacuum;		/*  autovacuum 相关选项  */
	bool		user_catalog_table; /*  用作附加目录关系  */
	int			parallel_workers;	/*  最大并行工作进程数  */
	StdRdOptIndexCleanup vacuum_index_cleanup;	/*  控制索引清理  */
	bool		vacuum_truncate;	/*  允许 vacuum 截断关系  */
} StdRdOptions;

#define HEAP_MIN_FILLFACTOR			10
#define HEAP_DEFAULT_FILLFACTOR		100

/*
 * RelationGetToastTupleTarget
 *		Returns the relation's toast_tuple_target.  Note multiple eval of argument!
 */
#define RelationGetToastTupleTarget(relation, defaulttarg) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->toast_tuple_target : (defaulttarg))

/*
 * RelationGetFillFactor
 *		Returns the relation's fillfactor.  Note multiple eval of argument!
 */
#define RelationGetFillFactor(relation, defaultff) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->fillfactor : (defaultff))

/*
 * RelationGetTargetPageUsage
 *		Returns the relation's desired space usage per page in bytes.
 */
#define RelationGetTargetPageUsage(relation, defaultff) \
	(BLCKSZ * RelationGetFillFactor(relation, defaultff) / 100)

/*
 * RelationGetTargetPageFreeSpace
 *		Returns the relation's desired freespace per page in bytes.
 */
#define RelationGetTargetPageFreeSpace(relation, defaultff) \
	(BLCKSZ * (100 - RelationGetFillFactor(relation, defaultff)) / 100)

/*
 * RelationIsUsedAsCatalogTable
 *		Returns whether the relation should be treated as a catalog table
 *		from the pov of logical decoding.  Note multiple eval of argument!
 */
#define RelationIsUsedAsCatalogTable(relation)	\
	((relation)->rd_options && \
	 ((relation)->rd_rel->relkind == RELKIND_RELATION || \
	  (relation)->rd_rel->relkind == RELKIND_MATVIEW) ? \
	 ((StdRdOptions *) (relation)->rd_options)->user_catalog_table : false)

/*
 * RelationGetParallelWorkers
 *		Returns the relation's parallel_workers reloption setting.
 *		Note multiple eval of argument!
 */
#define RelationGetParallelWorkers(relation, defaultpw) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->parallel_workers : (defaultpw))

/*  ViewOptions->check_option 值  */
typedef enum ViewOptCheckOption
{
	VIEW_OPTION_CHECK_OPTION_NOT_SET,
	VIEW_OPTION_CHECK_OPTION_LOCAL,
	VIEW_OPTION_CHECK_OPTION_CASCADED
} ViewOptCheckOption;

/*
 * ViewOptions
 *		Contents of rd_options for views
 */
typedef struct ViewOptions
{
	int32		vl_len_;		/*  varlena 头(勿直接触碰!)  */
	bool		security_barrier;
	bool		security_invoker;
	ViewOptCheckOption check_option;
} ViewOptions;

/*
 * RelationIsSecurityView
 *		Returns whether the relation is security view, or not.  Note multiple
 *		eval of argument!
 */
#define RelationIsSecurityView(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_barrier : false)

/*
 * RelationHasSecurityInvoker
 *		Returns true if the relation has the security_invoker property set.
 *		Note multiple eval of argument!
 */
#define RelationHasSecurityInvoker(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_invoker : false)

/*
 * RelationHasCheckOption
 *		Returns true if the relation is a view defined with either the local
 *		or the cascaded check option.  Note multiple eval of argument!
 */
#define RelationHasCheckOption(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option !=				\
	 VIEW_OPTION_CHECK_OPTION_NOT_SET)

/*
 * RelationHasLocalCheckOption
 *		Returns true if the relation is a view defined with the local check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasLocalCheckOption(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	 VIEW_OPTION_CHECK_OPTION_LOCAL)

/*
 * RelationHasCascadedCheckOption
 *		Returns true if the relation is a view defined with the cascaded check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasCascadedCheckOption(relation)							\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	  VIEW_OPTION_CHECK_OPTION_CASCADED)

/*
 * RelationIsValid
 *		True iff relation descriptor is valid.
 */
#define RelationIsValid(relation) PointerIsValid(relation)

#define InvalidRelation ((Relation) NULL)

/*
 * RelationHasReferenceCountZero
 *		True iff relation reference count is zero.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationGetForm
 *		Returns pg_class tuple for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetForm(relation) ((relation)->rd_rel)

/*
 * RelationGetRelid
 *		Returns the OID of the relation
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetNumberOfAttributes
 *		Returns the total number of attributes in a relation.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * IndexRelationGetNumberOfAttributes
 *		Returns the number of attributes in an index.
 */
#define IndexRelationGetNumberOfAttributes(relation) \
		((relation)->rd_index->indnatts)

/*
 * IndexRelationGetNumberOfKeyAttributes
 *		Returns the number of key attributes in an index.
 */
#define IndexRelationGetNumberOfKeyAttributes(relation) \
		((relation)->rd_index->indnkeyatts)

/*
 * RelationGetDescr
 *		Returns tuple descriptor for a relation.
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

/*
 * RelationGetRelationName
 *		Returns the rel's name.
 *
 * Note that the name is only unique within the containing namespace.
 */
#define RelationGetRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNamespace
 *		Returns the rel's namespace OID.
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

/*
 * RelationIsMapped
 *		True if the relation uses the relfilenumber map.  Note multiple eval
 *		of argument!
 */
#define RelationIsMapped(relation) \
	(RELKIND_HAS_STORAGE((relation)->rd_rel->relkind) && \
	 ((relation)->rd_rel->relfilenode == InvalidRelFileNumber))

#ifndef FRONTEND
/*
 * RelationGetSmgr
 *		Returns smgr file handle for a relation, opening it if needed.
 *
 * Very little code is authorized to touch rel->rd_smgr directly.  Instead
 * use this function to fetch its value.
 *
 * Note: since a relcache flush can cause the file handle to be closed again,
 * it's unwise to hold onto the pointer returned by this function for any
 * long period.  Recommended practice is to just re-execute RelationGetSmgr
 * each time you need to access the SMgrRelation.  It's quite cheap in
 * comparison to whatever an smgr function is going to do.
 */
static inline SMgrRelation
RelationGetSmgr(Relation rel)
{
	if (unlikely(rel->rd_smgr == NULL))
		smgrsetowner(&(rel->rd_smgr), smgropen(rel->rd_locator, rel->rd_backend));
	return rel->rd_smgr;
}

/*
 * RelationCloseSmgr
 *		Close the relation at the smgr level, if not already done.
 */
static inline void
RelationCloseSmgr(Relation relation)
{
	if (relation->rd_smgr != NULL)
		smgrclose(relation->rd_smgr);

	/*  smgrclose 应从所有者指针脱钩  */
	Assert(relation->rd_smgr == NULL);
}
#endif							/* !FRONTEND */

/*
 * RelationGetTargetBlock
 *		Fetch relation's current insertion target block.
 *
 * Returns InvalidBlockNumber if there is no current target block.  Note
 * that the target block status is discarded on any smgr-level invalidation,
 * so there's no need to re-open the smgr handle if it's not currently open.
 */
#define RelationGetTargetBlock(relation) \
	( (relation)->rd_smgr != NULL ? (relation)->rd_smgr->smgr_targblock : InvalidBlockNumber )

/*
 * RelationSetTargetBlock
 *		Set relation's current insertion target block.
 */
#define RelationSetTargetBlock(relation, targblock) \
	do { \
		RelationGetSmgr(relation)->smgr_targblock = (targblock); \
	} while (0)

/*
 * RelationIsPermanent
 *		True if relation is permanent.
 */
#define RelationIsPermanent(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT)

/*
 * RelationNeedsWAL
 *		True if relation needs WAL.
 *
 * Returns false if wal_level = minimal and this relation is created or
 * truncated in the current transaction.  See "Skipping WAL for New
 * RelFileLocator" in src/backend/access/transam/README.
 */
#define RelationNeedsWAL(relation)										\
	(RelationIsPermanent(relation) && (XLogIsNeeded() ||				\
	  (relation->rd_createSubid == InvalidSubTransactionId &&			\
	   relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)))

/*
 * RelationUsesLocalBuffers
 *		True if relation's pages are stored in local buffers.
 */
#define RelationUsesLocalBuffers(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP)

/*
 * RELATION_IS_LOCAL
 *		If a rel is either temp or newly created in the current transaction,
 *		it can be assumed to be accessible only to the current backend.
 *		This is typically used to decide that we can skip acquiring locks.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_LOCAL(relation) \
	((relation)->rd_islocaltemp || \
	 (relation)->rd_createSubid != InvalidSubTransactionId)

/*
 * RELATION_IS_OTHER_TEMP
 *		Test for a temporary relation that belongs to some other session.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_OTHER_TEMP(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP && \
	 !(relation)->rd_islocaltemp)


/*
 * RelationIsScannable
 *		Currently can only be false for a materialized view which has not been
 *		populated by its query.  This is likely to get more complicated later,
 *		so use a macro which looks like a function.
 */
#define RelationIsScannable(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsPopulated
 *		Currently, we don't physically distinguish the "populated" and
 *		"scannable" properties of matviews, but that may change later.
 *		Hence, use the appropriate one of these macros in code tests.
 */
#define RelationIsPopulated(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsAccessibleInLogicalDecoding
 *		True if we need to log enough information to have access via
 *		decoding snapshot.
 */
#define RelationIsAccessibleInLogicalDecoding(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (IsCatalogRelation(relation) || RelationIsUsedAsCatalogTable(relation)))

/*
 * RelationIsLogicallyLogged
 *		True if we need to log enough information to extract the data from the
 *		WAL stream.
 *
 * We don't log information for unlogged tables (since they don't WAL log
 * anyway), for foreign tables (since they don't WAL log, either),
 * and for system tables (their content is hard to make sense of, and
 * it would complicate decoding slightly for little gain). Note that we *do*
 * log information for user defined catalog tables since they presumably are
 * interesting to the user...
 */
#define RelationIsLogicallyLogged(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (relation)->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&	\
	 !IsCatalogRelation(relation))

/*  utils/cache/relcache.c 的例程  */
extern void RelationIncrementReferenceCount(Relation rel);
extern void RelationDecrementReferenceCount(Relation rel);

#endif							/* REL_H */
