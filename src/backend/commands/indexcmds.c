/*-------------------------------------------------------------------------
 *
 * indexcmds.c
 *	  POSTGRES 索引定义/移除代码。 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/indexcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/pg_rusage.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* 非导出函数原型 */
static bool CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts);
static void CheckPredicate(Expr *predicate);
static void ComputeIndexAttrs(IndexInfo *indexInfo,
							  Oid *typeOidP,
							  Oid *collationOidP,
							  Oid *classOidP,
							  int16 *colOptionP,
							  List *attList,
							  List *exclusionOpNames,
							  Oid relId,
							  const char *accessMethodName, Oid accessMethodId,
							  bool amcanorder,
							  bool isconstraint,
							  Oid ddl_userid,
							  int ddl_sec_context,
							  int *ddl_save_nestlevel);
static char *ChooseIndexName(const char *tabname, Oid namespaceId,
							 List *colnames, List *exclusionOpNames,
							 bool primary, bool isconstraint);
static char *ChooseIndexNameAddition(List *colnames);
static List *ChooseIndexColumnNames(List *indexElems);
static void ReindexIndex(RangeVar *indexRelation, ReindexParams *params,
						 bool isTopLevel);
static void RangeVarCallbackForReindexIndex(const RangeVar *relation,
											Oid relId, Oid oldRelId, void *arg);
static Oid	ReindexTable(RangeVar *relation, ReindexParams *params,
						 bool isTopLevel);
static void ReindexMultipleTables(const char *objectName,
								  ReindexObjectType objectKind, ReindexParams *params);
static void reindex_error_callback(void *arg);
static void ReindexPartitions(Oid relid, ReindexParams *params,
							  bool isTopLevel);
static void ReindexMultipleInternal(List *relids,
									ReindexParams *params);
static bool ReindexRelationConcurrently(Oid relationOid,
										ReindexParams *params);
static void update_relispartition(Oid relationId, bool newval);
static inline void set_indexsafe_procflags(void);

/*
 * RangeVarCallbackForReindexIndex() 的回调参数类型     
 */
struct ReindexIndexCallbackState
{
	ReindexParams params;		/* options from statement */
	Oid			locked_table_oid;	/* tracks previously locked table */
};

/*
 * reindex_error_callback() 的回调参数       
 */
typedef struct ReindexErrorInfo
{
	char	   *relname;
	char	   *relnamespace;
	char		relkind;
} ReindexErrorInfo;

/*
 * CheckIndexCompatible
 *		判断现有索引定义与预期索引定义是否兼容，使    
 *		现有索引的存储可以成为新索引的存储，从而避免
 *		重建。                                                     
 *
 * 'oldId': 现有索引的 OID          
 * 'accessMethodName': 所用 AM 的名称   
 * 'attributeList': IndexElem 列表，指定要建立索引的列与     
 *		表达式。
 * 'exclusionOpNames': 排除约束运算符名称的列表，         
 *		若不是排除约束则为 NIL。    
 *
 * 这是为 ALTER TABLE ALTER TYPE 的需要而定制的，它            
 * 会从 pg_get_indexdef 或 pg_get_constraintdef 定义重建             
 * 任何依赖于变更列的索引。我们省略了 DefineIndex           
 * 的某些健全性检查。我们假定旧索引和新索引具有       
 * 相同的列数，并且如果一个有表达式列或谓词，则        
 * 两者都有。属性列表的错误仍然适用。
 *
 * 大多数可以跳过表重写的列类型变更并不会使索引      
 * 失效。当所有运算符类、排序规则和排除运算符都     
 * 匹配时，我们认可这一点。虽然我们可以进一步允许    
 * btree 和 hash 索引的 opfamily 内变更，但这会带来          
 * 微妙的复杂性，对核心类型并无实际好处。注意，
 * 本函数不检查 INCLUDE 列；对它们来说，只需跳过表  
 * 重写  
 *
 * 当比较或排除运算符具有多态输入类型时，实际的       
 * 输入类型也必须匹配。这可以防范运算符响应             
 *  get_fn_expr_argtype() 而改变行为的可能性。目前，          
 * 这一风险只是理论上的：check_exclusion_constraint()           
 * 以及所有核心索引访问方法都不会为此类调用设置  
 *
 * 我们尚未实现验证表达式列或谓词兼容性的测试，  
 * 因此假定任何此类索引都不兼容。                   
 */
bool
CheckIndexCompatible(Oid oldId,
					 const char *accessMethodName,
					 List *attributeList,
					 List *exclusionOpNames)
{
	bool		isconstraint;
	Oid		   *typeObjectId;
	Oid		   *collationObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			relationId;
	HeapTuple	tuple;
	Form_pg_index indexForm;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	bool		amsummarizing;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	int			numberOfAttributes;
	int			old_natts;
	bool		ret = true;
	oidvector  *old_indclass;
	oidvector  *old_indcollation;
	Relation	irel;
	int			i;
	Datum		d;

	/* 调用者应该已经以某种方式锁定了关系。 */
	relationId = IndexGetRelation(oldId, false);

	/*
	 * 我们可以无条件地假装 isconstraint = false。它只用于     
	 * 决定对我们来说不应该发生的错误消息的文本。     
	 */
	isconstraint = false;

	numberOfAttributes = list_length(attributeList);
	Assert(numberOfAttributes > 0);
	Assert(numberOfAttributes <= INDEX_MAX_KEYS);

	/* 查找访问方法 */
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist",
						accessMethodName)));
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);
	ReleaseSysCache(tuple);

	amcanorder = amRoutine->amcanorder;
	amsummarizing = amRoutine->amsummarizing;

	/*
	 * 计算新索引的操作符类、排序规则和排除运算符，   
	 * 以便我们能够测试它与现有索引是否兼容。注意        
	 * ComputeIndexAttrs 可能在此处失败，但没关系：        
	 * DefineIndex 稍后也会失败。我们的 attributeList 仅包含   
	 * 键属性，因此我们为 ii_NumIndexAttrs 和       
	 * ii_NumIndexKeyAttrs 填相同值。 
	 */
	indexInfo = makeIndexInfo(numberOfAttributes, numberOfAttributes,
							  accessMethodId, NIL, NIL, false, false,
							  false, false, amsummarizing);
	typeObjectId = palloc_array(Oid, numberOfAttributes);
	collationObjectId = palloc_array(Oid, numberOfAttributes);
	classObjectId = palloc_array(Oid, numberOfAttributes);
	coloptions = palloc_array(int16, numberOfAttributes);
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, collationObjectId, classObjectId,
					  coloptions, attributeList,
					  exclusionOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, isconstraint, InvalidOid, 0, NULL);


	/* 获取即将过时的 pg_index 元组。 */
	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", oldId);
	indexForm = (Form_pg_index) GETSTRUCT(tuple);

	/*
	 * 我们不评估表达式或谓词；假设不兼容。此外，   
	 * 如果索引因任何原因无效，也将其视为不兼容。        
	 */
	if (!(heap_attisnull(tuple, Anum_pg_index_indpred, NULL) &&
		  heap_attisnull(tuple, Anum_pg_index_indexprs, NULL) &&
		  indexForm->indisvalid))
	{
		ReleaseSysCache(tuple);
		return false;
	}

	/* 操作符类或排序规则的任何更改都会破坏兼容性。 */
	old_natts = indexForm->indnkeyatts;
	Assert(old_natts == numberOfAttributes);

	d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indcollation);
	old_indcollation = (oidvector *) DatumGetPointer(d);

	d = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indclass);
	old_indclass = (oidvector *) DatumGetPointer(d);

	ret = (memcmp(old_indclass->values, classObjectId,
				  old_natts * sizeof(Oid)) == 0 &&
		   memcmp(old_indcollation->values, collationObjectId,
				  old_natts * sizeof(Oid)) == 0);

	ReleaseSysCache(tuple);

	if (!ret)
		return false;

	/* 对于多态 opcintype，列类型更改会破坏兼容性。 */
	irel = index_open(oldId, AccessShareLock);	/* caller probably has a lock */
	for (i = 0; i < old_natts; i++)
	{
		if (IsPolymorphicType(get_opclass_input_type(classObjectId[i])) &&
			TupleDescAttr(irel->rd_att, i)->atttypid != typeObjectId[i])
		{
			ret = false;
			break;
		}
	}

	/* opclass 选项的任何更改都会破坏兼容性。 */
	if (ret)
	{
		Datum	   *opclassOptions = RelationGetIndexRawAttOptions(irel);

		ret = CompareOpclassOptions(opclassOptions,
									indexInfo->ii_OpclassOptions, old_natts);

		if (opclassOptions)
			pfree(opclassOptions);
	}

	/* 排除运算符选择的任何更改都会破坏兼容性。 */
	if (ret && indexInfo->ii_ExclusionOps != NULL)
	{
		Oid		   *old_operators,
				   *old_procs;
		uint16	   *old_strats;

		RelationGetExclusionInfo(irel, &old_operators, &old_procs, &old_strats);
		ret = memcmp(old_operators, indexInfo->ii_ExclusionOps,
					 old_natts * sizeof(Oid)) == 0;

		/* 对于多态运算符，要求输入类型完全匹配。 */
		if (ret)
		{
			for (i = 0; i < old_natts && ret; i++)
			{
				Oid			left,
							right;

				op_input_types(indexInfo->ii_ExclusionOps[i], &left, &right);
				if ((IsPolymorphicType(left) || IsPolymorphicType(right)) &&
					TupleDescAttr(irel->rd_att, i)->atttypid != typeObjectId[i])
				{
					ret = false;
					break;
				}
			}
		}
	}

	index_close(irel, NoLock);
	return ret;
}

/*
 * CompareOpclassOptions
 *
 * 比较由 text[] datum 数组表示的每列 opclass 选项。              
 * 数组元素和数组本身都可以为 NULL。                   
 */
static bool
CompareOpclassOptions(Datum *opts1, Datum *opts2, int natts)
{
	int			i;
	FmgrInfo	fm;

	if (!opts1 && !opts2)
		return true;

	fmgr_info(F_ARRAY_EQ, &fm);
	for (i = 0; i < natts; i++)
	{
		Datum		opt1 = opts1 ? opts1[i] : (Datum) 0;
		Datum		opt2 = opts2 ? opts2[i] : (Datum) 0;

		if (opt1 == (Datum) 0)
		{
			if (opt2 == (Datum) 0)
				continue;
			else
				return false;
		}
		else if (opt2 == (Datum) 0)
			return false;

		/*
		 * 比较非 NULL 的 text[] datum。使用 C 排序规则来强制  
		 * 文本的二进制等价性，因为我们不了解           
		 * opclass 选项的语义。   
		 */
		if (!DatumGetBool(FunctionCall2Coll(&fm, C_COLLATION_OID, opt1, opt2)))
			return false;
	}

	return true;
}

/*
 * WaitForOlderSnapshots
 *
 * 等待那些可能拥有比给定 xmin 限制更旧的快照的事务，   
 * 因为它获取的快照可能不包含恰在获取之前删除的   
 * 元组。获取这类事务的 VXID 列表，并逐个等待它们。     
 * 本函数在并发构建索引时使用。                     
 *
 * 我们可以排除任何 xmin 大于给定 xmin 的运行中事务；   
 * 其最旧快照必然比我们的 xmin 限制新。     
 * 我们还可以排除任何 xmin 为零的事务，因为它们显然 
 * 完全没有活动快照（而且它们可能正在获取的任何  
 * 快照显然都比我们的新）。其他数据库中的事务也     
 * 可以忽略，因为它们根本无法看到正在被处理的    
 * 那个索引。       
 *
 * 我们还可以排除 autovacuum 进程以及运行手工            
 * 惰性 VACUUM 的进程，因为缺失索引条目也不会       
 * 困扰它们。（但手工 ANALYZE 不能排除在外，因为   
 * 它们可能处于稍后要执行任意操作的事务中。）       
 * 运行 CREATE INDEX CONCURRENTLY 或 REINDEX CONCURRENTLY 的进程，若   
 * 索引既非表达式索引也非部分索引，同样可以安全   
 * 忽略，因为我们知道这些进程不会检查所           
 * 索引表之外的数据。        
 *
 * 此外，GetCurrentVirtualXIDs 从不报告我们自己的 vxid，    
 * 无需检查。
 *
 * 如果某个进程带着为零的 xmin 进入空闲事务状态，     
 * 按上述论证我们无需再等待它。                      
 * 若发生这种情况，我们还没有停止等待的基础设施，  
 * 但我们至少可以避免在它当时已经空闲时开始等待的
 * 愚蠢做法。我们为此反复重新检查                    
 * GetCurrentVirtualXIDs 的输出。如果在任何一轮迭代中   
 * 某个 vxid 未出现在输出中，就可以忘掉它。     
 */
void
WaitForOlderSnapshots(TransactionId limitXmin, bool progress)
{
	int			n_old_snapshots;
	int			i;
	VirtualTransactionId *old_snapshots;

	old_snapshots = GetCurrentVirtualXIDs(limitXmin, true, false,
										  PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
										  | PROC_IN_SAFE_IC,
										  &n_old_snapshots);
	if (progress)
		pgstat_progress_update_param(PROGRESS_WAITFOR_TOTAL, n_old_snapshots);

	for (i = 0; i < n_old_snapshots; i++)
	{
		if (!VirtualTransactionIdIsValid(old_snapshots[i]))
			continue;			/* found uninteresting in previous cycle */

		if (i > 0)
		{
			/* 看看是否有任何变化… */
			VirtualTransactionId *newer_snapshots;
			int			n_newer_snapshots;
			int			j;
			int			k;

			newer_snapshots = GetCurrentVirtualXIDs(limitXmin,
													true, false,
													PROC_IS_AUTOVACUUM | PROC_IN_VACUUM
													| PROC_IN_SAFE_IC,
													&n_newer_snapshots);
			for (j = i; j < n_old_snapshots; j++)
			{
				if (!VirtualTransactionIdIsValid(old_snapshots[j]))
					continue;	/* found uninteresting in previous cycle */
				for (k = 0; k < n_newer_snapshots; k++)
				{
					if (VirtualTransactionIdEquals(old_snapshots[j],
												   newer_snapshots[k]))
						break;
				}
				if (k >= n_newer_snapshots) /* not there anymore */
					SetInvalidVirtualTransactionId(old_snapshots[j]);
			}
			pfree(newer_snapshots);
		}

		if (VirtualTransactionIdIsValid(old_snapshots[i]))
		{
			/* 如果请求，发布我们将要等待的后端。 */
			if (progress)
			{
				PGPROC	   *holder = BackendIdGetProc(old_snapshots[i].backendId);

				if (holder)
					pgstat_progress_update_param(PROGRESS_WAITFOR_CURRENT_PID,
												 holder->pid);
			}
			VirtualXactLock(old_snapshots[i], true);
		}

		if (progress)
			pgstat_progress_update_param(PROGRESS_WAITFOR_DONE, i + 1);
	}
}


/*
 * DefineIndex
 *		创建新索引。  
 *
 * 此函数根据 pg_dump 的需求管理当前 userid。                    
 * 在新数据库中重新创建旧数据库的目录条目是可以的，   
 * 无论现在哪些用户有权重新创建这些条目。这只是         
 * 状态保留。以对象所有者以外的任何身份运行不透明  
 * 表达式——比如调用目录条目中命名的函数，或求值目   
 * 录条目中的 pg_node_tree——都是不允许的。为了遵守     
 * 这些原则并保持故障安全，大多数 ACL 检查使用表       
 * 所有者的 userid。不经过不透明表达式就能到达的 ACL    
 * 检查则使用原始 userid。（pg_dump 可以从目录中预知       
 * 这类 ACL 检查。）总之这很混乱。未来的 DDL 开发应
 * 考虑提供一个用于目录设置的 DDL 命令，以及单独一  
 * 个执行不透明表达式的 DDL 命令。  
 *
 * 'relationId': 要在其上创建该索引的堆关系的              
 *		OID。 
 * 'stmt': 描述新索引属性的 IndexStmt。                
 * 'indexRelationId': 通常为 InvalidOid，但在引导期间可以 
 *		为非零值，以指定索引的预选 OID。     
 * 'parentIndexId': 父索引的 OID；如果不是分区索引的子       
 *		则为 InvalidOid。   
 * 'parentConstraintId': 父约束的 OID；如果不是约束的子项     
 *		则为 InvalidOid（仅在递归时使用）        
 * 'total_parts': 关系的直接和间接分区的总数；如果未知或  
 *		关系未分区，则传入 -1。              
 * 'is_alter_table': 这是由 ALTER 而非 CREATE 操作引起的。       
 * 'check_rights': 检查命名空间和表空间中的 CREATE 权限。      
 *		（除 ALTER 正在删除/重建索引外，此值应为真）    
 * 'check_not_in_use': 检查表是否尚未在当前会话中使用。      
 *		除非调用者持有该表打开，那样调用者最好            
 *		已提前检查过。                              
 * 'skip_build': 创建目录条目但不创建索引文件               
 * 'quiet': 抑制通常为约束提供的 NOTICE 消息。                 
 *
 * 返回所创建索引的对象地址。         
 */
ObjectAddress
DefineIndex(Oid relationId,
			IndexStmt *stmt,
			Oid indexRelationId,
			Oid parentIndexId,
			Oid parentConstraintId,
			int total_parts,
			bool is_alter_table,
			bool check_rights,
			bool check_not_in_use,
			bool skip_build,
			bool quiet)
{
	bool		concurrent;
	char	   *indexRelationName;
	char	   *accessMethodName;
	Oid		   *typeObjectId;
	Oid		   *collationObjectId;
	Oid		   *classObjectId;
	Oid			accessMethodId;
	Oid			namespaceId;
	Oid			tablespaceId;
	Oid			createdConstraintId = InvalidOid;
	List	   *indexColNames;
	List	   *allIndexParams;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_am	accessMethodForm;
	IndexAmRoutine *amRoutine;
	bool		amcanorder;
	bool		amissummarizing;
	amoptions_function amoptions;
	bool		partitioned;
	bool		safe_index;
	Datum		reloptions;
	int16	   *coloptions;
	IndexInfo  *indexInfo;
	bits16		flags;
	bits16		constr_flags;
	int			numberOfAttributes;
	int			numberOfKeyAttributes;
	TransactionId limitXmin;
	ObjectAddress address;
	LockRelId	heaprelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;
	Snapshot	snapshot;
	Oid			root_save_userid;
	int			root_save_sec_context;
	int			root_save_nestlevel;

	root_save_nestlevel = NewGUCNestLevel();

	/*
	 * 某些调用者需要我们以空的 default_tablespace 运行；这是
	 * 一个必要的 hack，以便在重写表的 ALTER TABLE 之后     
	 * 重新创建索引时准确重现目录状态。     
	 */
	if (stmt->reset_default_tblspc)
		(void) set_config_option("default_tablespace", "",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * 在临时关系上强制非并发构建，即使请求了 CONCURRENTLY 
	 * 其他后端无法访问临时关系，因此拿更强的锁没有  
	 * 危害，而且非并发 DROP 更高效。在对并发选项做任何 
	 * 使用之前先执行此操作                                        
	 *      
	 */
	if (stmt->concurrent && get_rel_persistence(relationId) != RELPERSISTENCE_TEMP)
		concurrent = true;
	else
		concurrent = false;

	/*
	 * 开始进度报告。如果我们正在构建分区，这已经完成  
	 *      
	 */
	if (!OidIsValid(parentIndexId))
	{
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  relationId);
		pgstat_progress_update_param(PROGRESS_CREATEIDX_COMMAND,
									 concurrent ?
									 PROGRESS_CREATEIDX_COMMAND_CREATE_CONCURRENTLY :
									 PROGRESS_CREATEIDX_COMMAND_CREATE);
	}

	/*
	 * 尚无索引 OID 可报告
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_INDEX_OID,
								 InvalidOid);

	/*
	 * 计算索引的键属性数  
	 */
	numberOfKeyAttributes = list_length(stmt->indexParams);

	/*
	 * 计算包含键列和 INCLUDE 列在内的索引列新列表。       
	 * 稍后我们可以通过检查列表位置来确定其中哪些
	 * 是键列，哪些只是 INCLUDE 列表的组成部分。           
	 * 位置小于 ii_NumIndexKeyAttrs 的列表项属于键列，           
	 * 等于及以上的项则属于                                       
	 * INCLUDE 列。      
	 */
	allIndexParams = list_concat_copy(stmt->indexParams,
									  stmt->indexIncludingParams);
	numberOfAttributes = list_length(allIndexParams);

	if (numberOfKeyAttributes <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("must specify at least one column")));
	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot use more than %d columns in an index",
						INDEX_MAX_KEYS)));

	/*
	 * 在进行标准索引构建时，只允许 SELECT ... FOR          
	 *  UPDATE/SHARE；但并发构建允许 INSERT/UPDATE/DELETE          
	 * （除 VACUUM） 
	 *
	 * 注意：调用者负责确保 relationId 引用的是应当在其     
	 * 上构建索引的关系；除了引导模式外，这通常要求    
	 * 调用者已锁定该关系。为避免锁升级风险，这里  
	 * 持有的锁至少应当与我们此处获取的锁                   
	 * 一样强。                      
	 *
	 * 注意：若此处的锁强度改变，由某些特定 ambuild 
	 *  函数控制下运行的并行 worker 所执行的代码也需 
	 * 要更新。                           
	 */
	lockmode = concurrent ? ShareUpdateExclusiveLock : ShareLock;
	rel = table_open(relationId, lockmode);

	/*
	 * 切换到表所有者的 userid，以便任何索引函数都以该    
	 * 用户身份运行。还要锁定安全受限的操作，并     
	 * 安排好使 GUC 变量更改仅限于此命令。                  
	 */
	GetUserIdAndSecContext(&root_save_userid, &root_save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   root_save_sec_context | SECURITY_RESTRICTED_OPERATION);

	namespaceId = RelationGetNamespace(rel);

	/* 确保对此类型的关系创建索引是有意义的 */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_PARTITIONED_TABLE:
			/* OK */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create index on relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
			break;
	}

	/*
	 * 为分区表建立行为，并验证参数                     
	 * 合理。  
	 *
	 * 在这种情况下我们不构建实际索引；只创建一些   
	 * 目录条目。实际索引通过为每个分区递归而构        
	 * 建。    
	 */
	partitioned = rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partitioned)
	{
		/*
		 * 注意：我们检查 'stmt->concurrent' 而非 'concurrent'，    
		 * 临时表也会抛出该错误。即使我们本可以在临时表
		 * 这样做（因为我们实际上并非并发），保持一致  
		 * 似乎更好。                          
		 */
		if (stmt->concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create index on partitioned table \"%s\" concurrently",
							RelationGetRelationName(rel))));
		if (stmt->excludeOpNames)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create exclusion constraints on partitioned table \"%s\"",
							RelationGetRelationName(rel))));
	}

	/*
	 * 不要尝试在其他后端的临时表上 CREATE INDEX。 
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create indexes on temporary tables of other sessions")));

	/*
	 * 除非调用者保证已经检查过这一点，否则坚持要求该 
	 * 表也未被我们自己的会话使用。否则我们可能无法在  
	 * 新索引中创建条目（例如，如果 INSERT 或 UPDATE 正在 
	 * 进行中并且已经创建了它的目标索引列表）。           
	 */
	if (check_not_in_use)
		CheckTableNotInUse(rel, "CREATE INDEX");

	/*
	 * 验证我们（仍）有关系命名空间的 CREATE 权限 
	 * （推测创建该关系时我们有，但现在也许没有。）  
	 * 调用者不需要则跳过。如果正在引导，      
	 * 也跳过检查，因为权限机制可能尚未工作。         
	 */
	if (check_rights && !IsBootstrapProcessingMode())
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(NamespaceRelationId, namespaceId, root_save_userid,
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
	}

	/*
	 * 选择要使用的表空间。如果未指定，则使用默认    
	 * 表空间（可能默认为数据库默认）。  
	 */
	if (stmt->tableSpace)
	{
		tablespaceId = get_tablespace_oid(stmt->tableSpace, false);
		if (partitioned && tablespaceId == MyDatabaseTableSpace)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
	}
	else
	{
		tablespaceId = GetDefaultTablespace(rel->rd_rel->relpersistence,
											partitioned);
		/* 注意 InvalidOid 在这种情况下是可以的 */
	}

	/* 检查表空间权限 */
	if (check_rights &&
		OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, root_save_userid,
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/*
	 * 强制共享索引进入 pg_global 表空间。这有点像 hack，    
	 * 但似乎比在 BKI 命令中标记它们更简单。另一方面， 
	 * 如果不是共享的，则不允许将其放置在那里。      
	 */
	if (rel->rd_rel->relisshared)
		tablespaceId = GLOBALTABLESPACE_OID;
	else if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * 选择索引列名称。      
	 */
	indexColNames = ChooseIndexColumnNames(allIndexParams);

	/*
	 * 调用者未指定时为索引选择名称    
	 */
	indexRelationName = stmt->idxname;
	if (indexRelationName == NULL)
		indexRelationName = ChooseIndexName(RelationGetRelationName(rel),
											namespaceId,
											indexColNames,
											stmt->excludeOpNames,
											stmt->primary,
											stmt->isconstraint);

	/*
	 * 查找访问方法，验证它是否能处理请求的功能          
	 */
	accessMethodName = stmt->accessMethod;
	tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * 提供将旧 RTREE 索引或多或少透明更新为 GiST 的 
		 * Hack：若请求 RTREE 但未找到，则用 GIST。           
		 */
		if (strcmp(accessMethodName, "rtree") == 0)
		{
			ereport(NOTICE,
					(errmsg("substituting access method \"gist\" for obsolete method \"rtree\"")));
			accessMethodName = "gist";
			tuple = SearchSysCache1(AMNAME, PointerGetDatum(accessMethodName));
		}

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("access method \"%s\" does not exist",
							accessMethodName)));
	}
	accessMethodForm = (Form_pg_am) GETSTRUCT(tuple);
	accessMethodId = accessMethodForm->oid;
	amRoutine = GetIndexAmRoutine(accessMethodForm->amhandler);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
								 accessMethodId);

	if (stmt->unique && !amRoutine->amcanunique)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support unique indexes",
						accessMethodName)));
	if (stmt->indexIncludingParams != NIL && !amRoutine->amcaninclude)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support included columns",
						accessMethodName)));
	if (numberOfKeyAttributes > 1 && !amRoutine->amcanmulticol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support multicolumn indexes",
						accessMethodName)));
	if (stmt->excludeOpNames && amRoutine->amgettuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support exclusion constraints",
						accessMethodName)));

	amcanorder = amRoutine->amcanorder;
	amoptions = amRoutine->amoptions;
	amissummarizing = amRoutine->amsummarizing;

	pfree(amRoutine);
	ReleaseSysCache(tuple);

	/*
	 * 验证谓词（若给出） 
	 */
	if (stmt->whereClause)
		CheckPredicate((Expr *) stmt->whereClause);

	/*
	 * 解析 AM 特定选项，转换为 text 数组形式并验证。
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options,
									 NULL, NULL, false, false);

	(void) index_reloptions(amoptions, reloptions, true);

	/*
	 * 为 index_create 准备参数，主要是 IndexInfo 结构。        
	 * 注意谓词必须采用隐式 AND 格式。在并发构建中，    
	 * 将其标记为未准备好插入。 
	 */
	indexInfo = makeIndexInfo(numberOfAttributes,
							  numberOfKeyAttributes,
							  accessMethodId,
							  NIL,	/* expressions, NIL for now */
							  make_ands_implicit((Expr *) stmt->whereClause),
							  stmt->unique,
							  stmt->nulls_not_distinct,
							  !concurrent,
							  concurrent,
							  amissummarizing);

	typeObjectId = palloc_array(Oid, numberOfAttributes);
	collationObjectId = palloc_array(Oid, numberOfAttributes);
	classObjectId = palloc_array(Oid, numberOfAttributes);
	coloptions = palloc_array(int16, numberOfAttributes);
	ComputeIndexAttrs(indexInfo,
					  typeObjectId, collationObjectId, classObjectId,
					  coloptions, allIndexParams,
					  stmt->excludeOpNames, relationId,
					  accessMethodName, accessMethodId,
					  amcanorder, stmt->isconstraint, root_save_userid,
					  root_save_sec_context, &root_save_nestlevel);

	/*
	 * 创建 PRIMARY KEY 索引时的额外检查。 
	 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, is_alter_table, stmt);

	/*
	 * 如果此表是分区的，并且我们正在创建唯一索引或 
	 * 主键，请确保分区键是索引列的子集。             
	 * 否则，可以通过将本应是唯一的值放入不同的分区    
	 * 来违反唯一性。                                             
	 *
	 * 我们目前只能对简单列引用执行此操作。                
	 *                                                             
	 */
	if (partitioned && (stmt->unique || stmt->primary))
	{
		PartitionKey key = RelationGetPartitionKey(rel);
		const char *constraint_type;
		int			i;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames != NIL)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
		}

		/*
		 * 验证分区键中的所有列都出现在                    
		 * 唯一键定义中，具有相同的相等性概念。  
		 */
		for (i = 0; i < key->partnatts; i++)
		{
			bool		found = false;
			int			eq_strategy;
			Oid			ptkey_eqop;
			int			j;

			/*
			 * 识别与此 partkey 列关联的相等运算符。        
			 * 对于列表和范围分区，partkeys 使用 btree         
			 * 运算符类；哈希分区使用哈希运算符类。         
			 * （保持与 ComputePartitionAttrs 同步！）
			 */
			if (key->strategy == PARTITION_STRATEGY_HASH)
				eq_strategy = HTEqualStrategyNumber;
			else
				eq_strategy = BTEqualStrategyNumber;

			ptkey_eqop = get_opfamily_member(key->partopfamily[i],
											 key->partopcintype[i],
											 key->partopcintype[i],
											 eq_strategy);
			if (!OidIsValid(ptkey_eqop))
				elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
					 eq_strategy, key->partopcintype[i], key->partopcintype[i],
					 key->partopfamily[i]);

			/*
			 * 我们还需要能够识别与索引列关联的        
			 * 相等运算符。btree opclasses 我们知道如何处理  
			 * ；若将来出现其他支持唯一索引的类型，       
			 * 此逻辑就需要扩展。                            
			 */
			if (accessMethodId == BTREE_AM_OID)
				eq_strategy = BTEqualStrategyNumber;
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot match partition key to an index using access method \"%s\"",
								accessMethodName)));

			/*
			 * 分区键为表达式时或许也能支持 UNIQUE 约束，    
			 * 但这值得吗？现在先放弃。                       
			 */
			if (key->partattrs[i] == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unsupported %s constraint with partition key definition",
								constraint_type),
						 errdetail("%s constraints cannot be used when partition keys include expressions.",
								   constraint_type)));

			/* 搜索索引列以查找匹配项 */
			for (j = 0; j < indexInfo->ii_NumIndexKeyAttrs; j++)
			{
				if (key->partattrs[i] == indexInfo->ii_IndexAttrNumbers[j])
				{
					/* 匹配了列，现在排序规则和相等运算符呢？ */
					Oid			idx_opfamily;
					Oid			idx_opcintype;

					if (key->partcollation[i] != collationObjectId[j])
						continue;

					if (get_opclass_opfamily_and_input_type(classObjectId[j],
															&idx_opfamily,
															&idx_opcintype))
					{
						Oid			idx_eqop;

						idx_eqop = get_opfamily_member(idx_opfamily,
													   idx_opcintype,
													   idx_opcintype,
													   eq_strategy);
						if (ptkey_eqop == idx_eqop)
						{
							found = true;
							break;
						}
					}
				}
			}

			if (!found)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(rel),
									key->partattrs[i] - 1);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraint on partitioned table must include all partitioning columns"),
						 errdetail("%s constraint on table \"%s\" lacks column \"%s\" which is part of the partition key.",
								   constraint_type, RelationGetRelationName(rel),
								   NameStr(att->attname))));
			}
		}
	}


	/*
	 * 我们禁止在系统列上创建索引。它们不一定能正确    
	 * 更新，而且似乎也没什么用。              
	 */
	for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		AttrNumber	attno = indexInfo->ii_IndexAttrNumbers[i];

		if (attno < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index creation on system columns is not supported")));
	}

	/*
	 * 还要检查表达式或谓词中使用的系统列。          
	 */
	if (indexInfo->ii_Expressions || indexInfo->ii_Predicate)
	{
		Bitmapset  *indexattrs = NULL;

		pull_varattnos((Node *) indexInfo->ii_Expressions, 1, &indexattrs);
		pull_varattnos((Node *) indexInfo->ii_Predicate, 1, &indexattrs);

		for (int i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
							  indexattrs))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("index creation on system columns is not supported")));
		}
	}

	/* 索引对其他来说忽略是否安全？参见 set_indexsafe_procflags() */
	safe_index = indexInfo->ii_Expressions == NIL &&
		indexInfo->ii_Predicate == NIL;

	/*
	 * 如果合适则报告索引创建（将此延迟到大部分错误     
	 * 检查后） 
	 */
	if (stmt->isconstraint && !quiet)
	{
		const char *constraint_type;

		if (stmt->primary)
			constraint_type = "PRIMARY KEY";
		else if (stmt->unique)
			constraint_type = "UNIQUE";
		else if (stmt->excludeOpNames != NIL)
			constraint_type = "EXCLUDE";
		else
		{
			elog(ERROR, "unknown constraint type");
			constraint_type = NULL; /* keep compiler quiet */
		}

		ereport(DEBUG1,
				(errmsg_internal("%s %s will create implicit index \"%s\" for table \"%s\"",
								 is_alter_table ? "ALTER TABLE / ADD" : "CREATE TABLE /",
								 constraint_type,
								 indexRelationName, RelationGetRelationName(rel))));
	}

	/*
	 * 有效的 stmt->oldNumber 意味着我们已拥有该索引的      
	 * 已构建形式，调用者也应拒绝任何索引构建  
	 */
	Assert(!RelFileNumberIsValid(stmt->oldNumber) || (skip_build && !concurrent));

	/*
	 * 为索引创建目录条目，包括约束。此步骤             
	 * 还实际构建索引，除非调用者请求不构建或处于       
	 * 并发模式（在这种情况下稍后构建），或正在创建  
	 * 分区索引（因为这些没有存储）。        
	 */
	flags = constr_flags = 0;
	if (stmt->isconstraint)
		flags |= INDEX_CREATE_ADD_CONSTRAINT;
	if (skip_build || concurrent || partitioned)
		flags |= INDEX_CREATE_SKIP_BUILD;
	if (stmt->if_not_exists)
		flags |= INDEX_CREATE_IF_NOT_EXISTS;
	if (concurrent)
		flags |= INDEX_CREATE_CONCURRENT;
	if (partitioned)
		flags |= INDEX_CREATE_PARTITIONED;
	if (stmt->primary)
		flags |= INDEX_CREATE_IS_PRIMARY;

	/*
	 * 如果表是分区的，并且递归被拒绝但分区存在，       
	 * 则将索引标记为无效。   
	 */
	if (partitioned && stmt->relation && !stmt->relation->inh)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);

		if (pd->nparts != 0)
			flags |= INDEX_CREATE_INVALID;
	}

	if (stmt->deferrable)
		constr_flags |= INDEX_CONSTR_CREATE_DEFERRABLE;
	if (stmt->initdeferred)
		constr_flags |= INDEX_CONSTR_CREATE_INIT_DEFERRED;

	indexRelationId =
		index_create(rel, indexRelationName, indexRelationId, parentIndexId,
					 parentConstraintId,
					 stmt->oldNumber, indexInfo, indexColNames,
					 accessMethodId, tablespaceId,
					 collationObjectId, classObjectId,
					 coloptions, reloptions,
					 flags, constr_flags,
					 allowSystemTableMods, !check_rights,
					 &createdConstraintId);

	ObjectAddressSet(address, RelationRelationId, indexRelationId);

	if (!OidIsValid(indexRelationId))
	{
		/*
		 * 回滚索引函数执行的任何 GUC 更改。若我们在上面  
		 * 更改过，也恢复为原始 default_tablespace。  
		 */
		AtEOXact_GUC(false, root_save_nestlevel);

		/* 恢复 userid 和安全上下文 */
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

		table_close(rel, NoLock);

		/* 如果这是顶层索引，我们完成了 */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();

		return address;
	}

	/*
	 * 回滚索引函数执行的任何 GUC 更改，并让后续    
	 * 更改仅限于此命令。如果某个索引函数更改了影响  
	 * 行为的 GUC（例如 search_path），这至关重要。        
	 */
	AtEOXact_GUC(false, root_save_nestlevel);
	root_save_nestlevel = NewGUCNestLevel();

	/* 添加任何请求的注释 */
	if (stmt->idxcomment != NULL)
		CreateComments(indexRelationId, RelationRelationId, 0,
					   stmt->idxcomment);

	if (partitioned)
	{
		PartitionDesc partdesc;

		/*
		 * 除非调用者要求跳过此步骤（通过 ONLY），处理每 
		 * 个分区，以确保它们都包含相应的索引。        
		 *
		 * 如果是内部调用（无 stmt->relation），则总是递归
		 */
		partdesc = RelationGetPartitionDesc(rel, true);
		if ((!stmt->relation || stmt->relation->inh) && partdesc->nparts > 0)
		{
			int			nparts = partdesc->nparts;
			Oid		   *part_oids = palloc_array(Oid, nparts);
			bool		invalidate_parent = false;
			Relation	parentIndex;
			TupleDesc	parentDesc;

			/*
			 * 在命令开始时报告分区总数；被递归调用时
			 * 则不要更新它。                                  
			 */
			if (!OidIsValid(parentIndexId))
			{
				/*
				 * 当由 ProcessUtilitySlow 调用时，分区数作为      
				 * 一种优化传入；但其他调用者传递 -1，因为 
				 * 它们手头没有该值。它计数的分区方式应当
				 * 与 find_all_inheritors 报告的关系数一致，即 
				 * 少一。                             
				 *
				 * 我们假定无需要求 find_all_inheritors 获取锁，  
				 * 因为对所有调用者而言这应当已经发生过。 
				 * 即使没有，只要不在此处触碰分区，这也是  
				 * 安全的；最坏后果只是一个错误的进度报告  
				 * 总计值。                   
				 */
				if (total_parts < 0)
				{
					List	   *children = find_all_inheritors(relationId,
															   NoLock, NULL);

					total_parts = list_length(children) - 1;
					list_free(children);
				}

				pgstat_progress_update_param(PROGRESS_CREATEIDX_PARTITIONS_TOTAL,
											 total_parts);
			}

			/* 为安全起见，制作 partdesc->oids[] 的本地副本 */
			memcpy(part_oids, partdesc->oids, sizeof(Oid) * nparts);

			/*
			 * 我们需要一个描述父索引的 IndexInfo。上面      
			 * 那个几乎足够好，但不完全，因为（例如）其  
			 * 谓词表达式（若有）尚未经过表达式预处理。
			 * 要获得能与子索引匹配的 IndexInfo，最可靠的
			 * 办法就是按同样的方式构建它，即使用          
			 * BuildIndexInfo()。                     
			 */
			parentIndex = index_open(indexRelationId, lockmode);
			indexInfo = BuildIndexInfo(parentIndex);

			parentDesc = RelationGetDescr(rel);

			/*
			 * 对每个分区扫描所有现有索引；如果有一个与 
			 * 我们的索引定义匹配且未附加到其他父索引，  
			 * 则将它附加到我们刚创建的那个索引。
			 *
			 * 如无匹配，则以相同选项递归调用自身    
			 * 来构建新索引（索引名称除外）。                 
			 */
			for (int i = 0; i < nparts; i++)
			{
				Oid			childRelid = part_oids[i];
				Relation	childrel;
				Oid			child_save_userid;
				int			child_save_sec_context;
				int			child_save_nestlevel;
				List	   *childidxs;
				ListCell   *cell;
				AttrMap    *attmap;
				bool		found = false;

				childrel = table_open(childRelid, lockmode);

				GetUserIdAndSecContext(&child_save_userid,
									   &child_save_sec_context);
				SetUserIdAndSecContext(childrel->rd_rel->relowner,
									   child_save_sec_context | SECURITY_RESTRICTED_OPERATION);
				child_save_nestlevel = NewGUCNestLevel();

				/*
				 * 但不要尝试在外部表上创建索引。若为常规  
				 * 索引则跳过；若尝试创建约束索引则报错 
				 * 报错。        
				 */
				if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				{
					if (stmt->unique || stmt->primary)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("cannot create unique index on partitioned table \"%s\"",
										RelationGetRelationName(rel)),
								 errdetail("Table \"%s\" contains partitions that are foreign tables.",
										   RelationGetRelationName(rel))));

					AtEOXact_GUC(false, child_save_nestlevel);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);
					table_close(childrel, lockmode);
					continue;
				}

				childidxs = RelationGetIndexList(childrel);
				attmap =
					build_attrmap_by_name(RelationGetDescr(childrel),
										  parentDesc,
										  false);

				foreach(cell, childidxs)
				{
					Oid			cldidxid = lfirst_oid(cell);
					Relation	cldidx;
					IndexInfo  *cldIdxInfo;

					/* 此索引已经是另一个索引的分区 */
					if (has_superclass(cldidxid))
						continue;

					cldidx = index_open(cldidxid, lockmode);
					cldIdxInfo = BuildIndexInfo(cldidx);
					if (CompareIndexInfo(cldIdxInfo, indexInfo,
										 cldidx->rd_indcollation,
										 parentIndex->rd_indcollation,
										 cldidx->rd_opfamily,
										 parentIndex->rd_opfamily,
										 attmap))
					{
						Oid			cldConstrOid = InvalidOid;

						/*
						 * 找到匹配  
						 *
						 * 如果此索引是因为约束而在父索  
						 * 引中创建的，那么子索引也需要      
						 * 有一个约束，因此查找一个。如果     
						 * 没有该约束，此索引就不合格，继续
						 * 查找。    
						 */
						if (createdConstraintId != InvalidOid)
						{
							cldConstrOid =
								get_relation_idx_constraint_oid(childRelid,
																cldidxid);
							if (cldConstrOid == InvalidOid)
							{
								index_close(cldidx, lockmode);
								continue;
							}
						}

						/* 将索引附加到父索引，我们完成了。 */
						IndexSetParentIndex(cldidx, indexRelationId);
						if (createdConstraintId != InvalidOid)
							ConstraintSetParentConstraint(cldConstrOid,
														  createdConstraintId,
														  childRelid);

						if (!cldidx->rd_index->indisvalid)
							invalidate_parent = true;

						found = true;

						/*
						 * 将此分区报告为已处理。注意如果    
						 * 该分区尚有子分区时，理想情况下  
						 * 我们会统计子分区并更新所有子分区 
						 * 的进度报告；但这似乎代价过高。    
						 * 取而代之的是，进度报告将表现得好像
						 * 所有这类间接子分区都在命令结束时
						 * 零耗时处理完。  
						 */
						pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);

						/* 保持锁直到提交 */
						index_close(cldidx, NoLock);
						break;
					}

					index_close(cldidx, lockmode);
				}

				list_free(childidxs);
				AtEOXact_GUC(false, child_save_nestlevel);
				SetUserIdAndSecContext(child_save_userid,
									   child_save_sec_context);
				table_close(childrel, NoLock);

				/*
				 * 若未找到匹配的索引，则自行创建  
				 */
				if (!found)
				{
					IndexStmt  *childStmt = copyObject(stmt);
					bool		found_whole_row;
					ListCell   *lc;
					ObjectAddress childAddr;

					/*
					 * 不能为子索引使用相同名称，因此清除  
					 * idxname 让递归调用选择新名称。同样，     
					 * 目标关系字段也是错的；如果设置了   
					 * indexOid 或 oldNumber，它们也不能应用于子 
					 * 索引。                                   
					 */
					childStmt->idxname = NULL;
					childStmt->relation = NULL;
					childStmt->indexOid = InvalidOid;
					childStmt->oldNumber = InvalidRelFileNumber;
					childStmt->oldCreateSubid = InvalidSubTransactionId;
					childStmt->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;

					/*
					 * 调整任何 Vars（表达式和索引 WHERE 子句中 
					 * 的）以匹配分区的列编号，以防其与父表 
					 * 的不同。                             
					 */
					foreach(lc, childStmt->indexParams)
					{
						IndexElem  *ielem = lfirst(lc);

						/*
						 * 如果索引参数是表达式，则必须使其
						 * 包含子 Vars。                  
						 */
						if (ielem->expr)
						{
							ielem->expr =
								map_variable_attnos((Node *) ielem->expr,
													1, 0, attmap,
													InvalidOid,
													&found_whole_row);
							if (found_whole_row)
								elog(ERROR, "cannot convert whole-row table reference");
						}
					}
					childStmt->whereClause =
						map_variable_attnos(stmt->whereClause, 1, 0,
											attmap,
											InvalidOid, &found_whole_row);
					if (found_whole_row)
						elog(ERROR, "cannot convert whole-row table reference");

					/*
					 * 以起始用户 ID 递归。被调用者会用它来  
					 * 进行权限检查，然后再切换。  
					 */
					Assert(GetUserId() == child_save_userid);
					SetUserIdAndSecContext(root_save_userid,
										   root_save_sec_context);
					childAddr =
						DefineIndex(childRelid, childStmt,
									InvalidOid, /* no predefined OID */
									indexRelationId,	/* this is our child */
									createdConstraintId,
									-1,
									is_alter_table, check_rights,
									check_not_in_use,
									skip_build, quiet);
					SetUserIdAndSecContext(child_save_userid,
										   child_save_sec_context);

					/*
					 * 检查刚创建的索引是否有效，因为在跨   
					 * 分区级别递归时，它可能已被切换为无效
					 * 无效。                                       
					 */
					if (!get_index_isvalid(childAddr.objectId))
						invalidate_parent = true;
				}

				free_attrmap(attmap);
			}

			index_close(parentIndex, lockmode);

			/*
			 * 我们为此索引插入的 pg_index 行被标记为  
			 * indisvalid=true。但如果附加的现有索引无效，     
			 * 那么就不对了，因此也将我们的行改为无效。 
			 */
			if (invalidate_parent)
			{
				Relation	pg_index = table_open(IndexRelationId, RowExclusiveLock);
				HeapTuple	tup,
							newtup;

				tup = SearchSysCache1(INDEXRELID,
									  ObjectIdGetDatum(indexRelationId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for index %u",
						 indexRelationId);
				newtup = heap_copytuple(tup);
				((Form_pg_index) GETSTRUCT(newtup))->indisvalid = false;
				CatalogTupleUpdate(pg_index, &tup->t_self, newtup);
				ReleaseSysCache(tup);
				table_close(pg_index, RowExclusiveLock);
				heap_freetuple(newtup);

				/*
				 * 此处 CCI 以使此更新可见，以防此操作跨多个
				 * 分区级别递归。            
				 */
				CommandCounterIncrement();
			}
		}

		/*
		 * 分区表上的索引本身不会被构建，因此我们在此 
		 * 完成。 
		 */
		AtEOXact_GUC(false, root_save_nestlevel);
		SetUserIdAndSecContext(root_save_userid, root_save_sec_context);
		table_close(rel, NoLock);
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();
		else
		{
			/* 更新中间分区索引本身的进度 */
			pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);
		}

		return address;
	}

	AtEOXact_GUC(false, root_save_nestlevel);
	SetUserIdAndSecContext(root_save_userid, root_save_sec_context);

	if (!concurrent)
	{
		/* 关闭堆，在非并发情况下我们完成了 */
		table_close(rel, NoLock);

		/*
		 * 如果这是顶层索引，命令整体完成；            
		 * 否则，增加进度以报告一个子索引已完成。       
		 */
		if (!OidIsValid(parentIndexId))
			pgstat_progress_end_command();
		else
			pgstat_progress_incr_param(PROGRESS_CREATEIDX_PARTITIONS_DONE, 1);

		return address;
	}

	/* 为下面的操作保存 lockrelid 和 locktag，然后关闭 rel */
	heaprelid = rel->rd_lockInfo.lockRelId;
	SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
	table_close(rel, NoLock);

	/*
	 * 对于并发构建，在开始构建索引之前让目录条目   
	 * 对其他事务可见非常重要。这将防止它们进行          
	 * 不兼容的 HOT 更新。新索引将被标记为既不是           
	 * indisready 也不是 indisvalid，这样其他任何人都           
	 * 不会尝试向它插入或将其用于查询。     
	 *
	 * 我们必须提交当前事务，使索引变得可见；然后 
	 * 再启动一个新事务。注意，我们刚刚构建的所有        
	 * 数据结构都会在提交时丢失。此处之后保留的只有    
	 * 关系 OID。
	 *
	 * 在提交之前，先获取表上的会话级锁，以确保表    
	 * 和索引都不会在我们完成之前被删除。即使有       
	 * 其他人正在等待访问，这也不会阻塞，因为我们     
	 * 在事务内已持有相同的锁。              
	 *
	 * 注意：我们目前不为索引获取会话锁，因为在     
	 * 我们持有父表锁期间，不存在会改变其状态的        
	 * 操作。这一点将来可能需要改变。                   
	 */
	LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/* 告诉并发索引构建忽略我们，如果索引符合条件 */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * 索引现在可见，因此我们可以报告 OID。同时包括 
	 * 阶段 2 开始的报告。                     
	 */
	{
		const int	progress_cols[] = {
			PROGRESS_CREATEIDX_INDEX_OID,
			PROGRESS_CREATEIDX_PHASE
		};
		const int64 progress_vals[] = {
			indexRelationId,
			PROGRESS_CREATEIDX_PHASE_WAIT_1
		};

		pgstat_progress_update_multi_param(2, progress_cols, progress_vals);
	}

	/*
	 * 并发索引构建的阶段 2（工作原理概览参见            
	 * validate_index() 的注释）     
	 *
	 * 现在我们必须等待，直到没有运行中的事务仍以        
	 * 旧的索引列表打开该表。使用 ShareLock 来涵盖     
	 * 持有允许写入该表的锁的运行中事务。注意，           
	 * 我们无需担心在此点之后才以写方式打开表的          
	 * 事务；它们打开表时就会看到新索引。       
	 *
	 * 注意：我们在此使用真正的锁获取，而不是仅仅       
	 * 检查 ProcArray 然后休眠，是因为如果相关事务中      
	 * 有某个事务正阻塞在试图获取我们表的排他锁上， 
	 * 就可能发生死锁。锁代码会检测死锁并正确地        
	 * 报错退出。    
	 */
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * 此时我们可以确定，没有哪个以写方式打开表的   
	 * 事务的索引列表中缺少这个新索引。我们已经        
	 * 等待完了所有现存事务，而任何新事务的列表中       
	 * 都会有新索引，但该索引仍被标记为"不可插入"         
	 * 状态。不过在判断 HOT 安全性时会查阅该索引。  
	 * 这种安排确保不会创建新的 HOT 链，使得链中的       
	 * 新元组和旧元组具有不同的索引键。                  
	 *                                 
	 *
	 * 我们现在获取一个新快照，并使用该快照中可见      
	 * 的所有元组来构建索引。可以确信，对这些元组      
	 * 的任何 HOT 更新都会与该索引兼容，因为不知道        
	 * 该索引的事务所做的更新现在都已提交或回滚。      
	 * 因此，每个可见元组要么是其 HOT 链的末端，    
	 * 要么其链的延伸对该索引是 HOT 安全的。              
	 */

	/* 设置 ActiveSnapshot，因为索引中的函数可能需要它 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* 执行索引的并发构建 */
	index_concurrently_build(relationId, indexRelationId);

	/* 我们可以放弃我们的快照 */
	PopActiveSnapshot();

	/*
	 * 提交此事务以使 indisready 更新可见。              
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* 告诉并发索引构建忽略我们，如果索引符合条件 */
	if (safe_index)
		set_indexsafe_procflags();

	/*
	 * 并发索引构建的阶段 3    
	 *
	 * 我们再次等待，直到没有事务可以使用标记为        
	 * 对更新只读的索引打开表。      
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockers(heaplocktag, ShareLock, true);

	/*
	 * 现在获取将由 validate_index() 用来过滤候选元组的        
	 * "引用快照"。当心！可能仍然存在一些使用中的         
	 * 快照，它们会将我们的引用快照视为已提交的某  
	 * 个事务视为仍在进行中。如果这样一个刚提交的        
	 * 事务删除了表中的元组，我们不会把它们纳入           
	 * 索引；然而，视该删除事务为仍在进行的那些           
	 * 事务仍期望这些元组在索引标记为有效时存在。
	 *
	 * 我们的解决方法是在把索引标记为有效之前，           
	 * 先等危险事务退出。
	 *
	 * 我们还把 ActiveSnapshot 设置为该快照，因为索引中       
	 * 要用快照。 
	 */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	PushActiveSnapshot(snapshot);

	/*
	 * 扫描索引和堆，插入任何缺失的索引条目。     
	 */
	validate_index(relationId, indexRelationId, snapshot);

	/*
	 * 丢弃引用快照。我们必须在等待其他快照持有者之前 
	 * 完成此事，否则我们将与其他同样在执行 CREATE INDEX 
	 * CONCURRENTLY 的进程死锁，它们会把我们的快照当作    
	 * 其必须等待的对象。但首先保存快照的 xmin 用作   
	 * GetCurrentVirtualXIDs() 的 limitXmin 
	 */
	limitXmin = snapshot->xmin;

	PopActiveSnapshot();
	UnregisterSnapshot(snapshot);

	/*
	 * 快照子系统仍可能包含已注册的快照，这些快照     
	 * 阻碍了我们进程公告的 xmin；特别是如果            
	 * default_transaction_isolation = serializable，则存在一个      
	 * 仍然活动的事务快照。CatalogSnapshot 同样是一个    
	 * 危险。                                                            
	 *                                                                       
	 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* 告诉并发索引构建忽略我们，如果索引符合条件 */
	if (safe_index)
		set_indexsafe_procflags();

	/* 我们现在绝对不应该公告任何 xmin。 */
	Assert(MyProc->xmin == InvalidTransactionId);

	/*
	 * 索引现在在这个意义上是有效的：它包含所有当前
	 * 感兴趣的元组。但由于它可能不包含刚好在引用快照  
	 * 获取之前删除的元组，我们必须等待可能有   
	 * 更旧快照的事务。                     
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_3);
	WaitForOlderSnapshots(limitXmin, true);

	/*
	 * 索引现可标记为有效 -- 更新其 pg_index 条目  
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_VALID);

	/*
	 * pg_index 更新将导致后端（包括此后端）更新索引本身 
	 * 的 relcache 条目，但我们也应该发送父表的 relcache
	 * 无效化以强制重新规划缓存的计划。否则现有会话     
	 * 可能无法在需要的地方使用新索引。                    
	 *                                                                        
	 *                                                                  
	 */
	CacheInvalidateRelcacheByRelid(heaprelid.relId);

	/*
	 * 最后要做的是释放父表上的会话级锁。                    
	 */
	UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);

	pgstat_progress_end_command();

	return address;
}


/*
 * CheckPredicate
 *		检查给定的部分索引谓词是否有效。       
 *
 * 这曾经还将谓词的形式限制为 indxpath.c 能处理的    
 * 形式。然而，这似乎过于受限。部分索引的一个
 * 有用应用是对表的子集应用 UNIQUE 约束，在这种     
 * 场景下任何可求值的谓词都能行得通。所以这里     
 * 接受任何谓词（需要计划才能执行的除外），让 
 * indxpath.c 自行应对。                                         
 */
static void
CheckPredicate(Expr *predicate)
{
	/*
	 * transformExpr() 应该已经根据谓词的 EXPR_KIND_              
	 * 拒绝了子查询、聚合和窗口函数。                 
	 */

	/*
	 * 使用可变函数的谓词可能是错误的，原因与          
	 * 我们不允许索引表达式使用可变函数相同。  
	 */
	if (contain_mutable_functions_after_planning(predicate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("functions in index predicate must be marked IMMUTABLE")));
}

/*
 * 计算每个索引列的信息，包括被索引的列号或索引    
 * 表达式、opclass 及其选项。注意，所有输出向量都应       
 * 为所有列分配空间，包括 "including" 列。            
 *
 * 如果调用者已切换为表所有者，ddl_userid 就是              
 * 不经过不透明表达式即可到达的 ACL 检查所用角色。  
 * 否则为 InvalidOid，其余 ddl_* 参数未定义 
 */
static void
ComputeIndexAttrs(IndexInfo *indexInfo,
				  Oid *typeOidP,
				  Oid *collationOidP,
				  Oid *classOidP,
				  int16 *colOptionP,
				  List *attList,	/* list of IndexElem's */
				  List *exclusionOpNames,
				  Oid relId,
				  const char *accessMethodName,
				  Oid accessMethodId,
				  bool amcanorder,
				  bool isconstraint,
				  Oid ddl_userid,
				  int ddl_sec_context,
				  int *ddl_save_nestlevel)
{
	ListCell   *nextExclOp;
	ListCell   *lc;
	int			attn;
	int			nkeycols = indexInfo->ii_NumIndexKeyAttrs;
	Oid			save_userid;
	int			save_sec_context;

	/* 如果需要，为排除运算符信息分配空间 */
	if (exclusionOpNames)
	{
		Assert(list_length(exclusionOpNames) == nkeycols);
		indexInfo->ii_ExclusionOps = palloc_array(Oid, nkeycols);
		indexInfo->ii_ExclusionProcs = palloc_array(Oid, nkeycols);
		indexInfo->ii_ExclusionStrats = palloc_array(uint16, nkeycols);
		nextExclOp = list_head(exclusionOpNames);
	}
	else
		nextExclOp = NULL;

	if (OidIsValid(ddl_userid))
		GetUserIdAndSecContext(&save_userid, &save_sec_context);

	/*
	 * 处理 attributeList 
	 */
	attn = 0;
	foreach(lc, attList)
	{
		IndexElem  *attribute = (IndexElem *) lfirst(lc);
		Oid			atttype;
		Oid			attcollation;

		/*
		 * 处理要索引的列或表达式。           
		 */
		if (attribute->name != NULL)
		{
			/* 简单索引属性 */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			Assert(attribute->expr == NULL);
			atttuple = SearchSysCacheAttName(relId, attribute->name);
			if (!HeapTupleIsValid(atttuple))
			{
				/* 错误消息拼写的差异是历史原因 */
				if (isconstraint)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" named in key does not exist",
									attribute->name)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									attribute->name)));
			}
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);
			indexInfo->ii_IndexAttrNumbers[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* 索引表达式 */
			Node	   *expr = attribute->expr;

			Assert(expr != NULL);

			if (attn >= nkeycols)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("expressions are not supported in included columns")));
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * 剥离任何顶层 COLLATE 子句，确保我们同样对待   
			 * "x COLLATE y" 和 "(x COLLATE y)"。    
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno != InvalidAttrNumber)
			{
				/*
				 * 用户写 "(column)" 或 "(column COLLATE something)" 
				 * 仍将其视为简单属性。        
				 */
				indexInfo->ii_IndexAttrNumbers[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				indexInfo->ii_IndexAttrNumbers[attn] = 0;	/* marks expression */
				indexInfo->ii_Expressions = lappend(indexInfo->ii_Expressions,
													expr);

				/*
				 * transformExpr() 应该已经根据索引表达式的    
				 * EXPR_KIND_ 拒绝了子查询、聚合和                
				 * 窗口函数。         
				 */

				/*
				 * 使用易变函数的表达式多半是错的，若     
				 * 相同数据每次得不到相同结果，索引条目  
				 * 究竟意味着什么就完全不清楚了                 
				 *             
				 */
				if (contain_mutable_functions_after_planning((Expr *) expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in index expression must be marked IMMUTABLE")));
			}
		}

		typeOidP[attn] = atttype;

		/*
		 * 包含列无排序规则、无 opclass，也无排序选项   
		 *         
		 */
		if (attn >= nkeycols)
		{
			if (attribute->collation)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support a collation")));
			if (attribute->opclass)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support an operator class")));
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support ASC/DESC options")));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("including column does not support NULLS FIRST/LAST options")));

			classOidP[attn] = InvalidOid;
			colOptionP[attn] = 0;
			collationOidP[attn] = InvalidOid;
			attn++;

			continue;
		}

		/*
		 * 若有，应用排序规则覆盖。使用 ddl_userid 是必    
		 * 要的，因为其中含 ACL 检查；由于排序规则不含   
		 * 不透明表达式（或非不透明的）就安全。 
		 */
		if (attribute->collation)
		{
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			attcollation = get_collation_oid(attribute->collation, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
			}
		}

		/*
		 * 检查当且仅当类型可排序时我们才有排序规则。
		 * 这里唯一预期的失败是 (1) 将 COLLATE 应用于不可   
		 * 排序的类型，或 (2) 索引表达式具有未解析的排序 
		 * 规则。但也不妨将其编成完整的一致性检查  
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for index expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		collationOidP[attn] = attcollation;

		/*
		 * 确定要使用的 opclass。由于其中有 ACL 检查，因此   
		 * 使用 ddl_userid 是必要的。尽管 opclass 含不透明  
		 * 表达式（具体指函数），但只有超级用户可 
		 * 定义 opclass，故安全。   
		 */
		if (OidIsValid(ddl_userid))
		{
			AtEOXact_GUC(false, *ddl_save_nestlevel);
			SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
		}
		classOidP[attn] = ResolveOpClass(attribute->opclass,
										 atttype,
										 accessMethodName,
										 accessMethodId);
		if (OidIsValid(ddl_userid))
		{
			SetUserIdAndSecContext(save_userid, save_sec_context);
			*ddl_save_nestlevel = NewGUCNestLevel();
		}

		/*
		 * 识别排除运算符（如果有）。 
		 */
		if (nextExclOp)
		{
			List	   *opname = (List *) lfirst(nextExclOp);
			Oid			opid;
			Oid			opfamily;
			int			strat;

			/*
			 * 查找运算符——它必须无需运行时强制     
			 * 即可接受列数据类型（但二进制兼容是可以 
			 * 的）。运算符包含不透明表达式（具体来说是   
			 * 函数）。compatible_oper_opid() 即 oper()  
			 * 和 IsBinaryCoercible()。如果 oper() 开始调用不      
			 * 透明表达式，PostgreSQL 就会有安全问题。  
			 */
			if (OidIsValid(ddl_userid))
			{
				AtEOXact_GUC(false, *ddl_save_nestlevel);
				SetUserIdAndSecContext(ddl_userid, ddl_sec_context);
			}
			opid = compatible_oper_opid(opname, atttype, atttype, false);
			if (OidIsValid(ddl_userid))
			{
				SetUserIdAndSecContext(save_userid, save_sec_context);
				*ddl_save_nestlevel = NewGUCNestLevel();
			}

			/*
			 * 只允许在排除约束中使用交换运算符。     
			 * 如果 X 与 Y 冲突，但 Y 不与 X 冲突，           
			 * 会发生坏事。             
			 */
			if (get_commutator(opid) != opid)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not commutative",
								format_operator(opid)),
						 errdetail("Only commutative operators can be used in exclusion constraints.")));

			/*
			 * 运算符也必须是正确的 opfamily 的成员   
			 */
			opfamily = get_opclass_family(classOidP[attn]);
			strat = get_op_opfamily_strategy(opid, opfamily);
			if (strat == 0)
			{
				HeapTuple	opftuple;
				Form_pg_opfamily opfform;

				/*
				 * attribute->opclass 可能没有显式命名 opfamily，   
				 * 因此获取所选 opfamily 名称，用于错误消息  
				 * 消息。     
				 */
				opftuple = SearchSysCache1(OPFAMILYOID,
										   ObjectIdGetDatum(opfamily));
				if (!HeapTupleIsValid(opftuple))
					elog(ERROR, "cache lookup failed for opfamily %u",
						 opfamily);
				opfform = (Form_pg_opfamily) GETSTRUCT(opftuple);

				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("operator %s is not a member of operator family \"%s\"",
								format_operator(opid),
								NameStr(opfform->opfname)),
						 errdetail("The exclusion operator must be related to the index operator class for the constraint.")));
			}

			indexInfo->ii_ExclusionOps[attn] = opid;
			indexInfo->ii_ExclusionProcs[attn] = get_opcode(opid);
			indexInfo->ii_ExclusionStrats[attn] = strat;
			nextExclOp = lnext(exclusionOpNames, nextExclOp);
		}

		/*
		 * 设置每列的选项（indoption 字段）。目前，对任何  
		 * 无序索引该值为零，有序索引则带 DESC 和 NULLS      
		 * FIRST/LAST 选项。     
		 */
		colOptionP[attn] = 0;
		if (amcanorder)
		{
			/* 默认排序是 ASC */
			if (attribute->ordering == SORTBY_DESC)
				colOptionP[attn] |= INDOPTION_DESC;
			/* 默认 null 排序：ASC 为 LAST，DESC 为 FIRST */
			if (attribute->nulls_ordering == SORTBY_NULLS_DEFAULT)
			{
				if (attribute->ordering == SORTBY_DESC)
					colOptionP[attn] |= INDOPTION_NULLS_FIRST;
			}
			else if (attribute->nulls_ordering == SORTBY_NULLS_FIRST)
				colOptionP[attn] |= INDOPTION_NULLS_FIRST;
		}
		else
		{
			/* 索引 AM 不支持排序 */
			if (attribute->ordering != SORTBY_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support ASC/DESC options",
								accessMethodName)));
			if (attribute->nulls_ordering != SORTBY_NULLS_DEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("access method \"%s\" does not support NULLS FIRST/LAST options",
								accessMethodName)));
		}

		/* 设置每列的 opclass 选项（attoptions 字段）。 */
		if (attribute->opclassopts)
		{
			Assert(attn < nkeycols);

			if (!indexInfo->ii_OpclassOptions)
				indexInfo->ii_OpclassOptions =
					palloc0_array(Datum, indexInfo->ii_NumIndexAttrs);

			indexInfo->ii_OpclassOptions[attn] =
				transformRelOptions((Datum) 0, attribute->opclassopts,
									NULL, NULL, false, false);
		}

		attn++;
	}
}

/*
 * 解析可能默认的操作符类规范                
 *
 * 注意：这用于解析索引和分区键定义中的                  
 * 操作符类规范。     
 */
Oid
ResolveOpClass(List *opclass, Oid attrType,
			   const char *accessMethodName, Oid accessMethodId)
{
	char	   *schemaname;
	char	   *opcname;
	HeapTuple	tuple;
	Form_pg_opclass opform;
	Oid			opClassId,
				opInputType;

	if (opclass == NIL)
	{
		/* 未指定操作符类，因此查找默认值 */
		opClassId = GetDefaultOpClass(attrType, accessMethodId);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(attrType), accessMethodName),
					 errhint("You must specify an operator class for the index or define a default operator class for the data type.")));
		return opClassId;
	}

	/*
	 * 给出特定 opclass 名时，就查找该 opclass  
	 */

	/* 解构名称列表 */
	DeconstructQualifiedName(opclass, &schemaname, &opcname);

	if (schemaname)
	{
		/* 仅在特定模式中查找 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, false);
		tuple = SearchSysCache3(CLAAMNAMENSP,
								ObjectIdGetDatum(accessMethodId),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 未限定的 opclass 名称，因此搜索搜索路径 */
		opClassId = OpclassnameGetOpcid(accessMethodId, opcname);
		if (!OidIsValid(opClassId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
							opcname, accessMethodName)));
		tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opClassId));
	}

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
						NameListToString(opclass), accessMethodName)));

	/*
	 * 验证索引操作符类接受此数据类型。注意我们将     
	 * 接受二进制兼容。         
	 */
	opform = (Form_pg_opclass) GETSTRUCT(tuple);
	opClassId = opform->oid;
	opInputType = opform->opcintype;

	if (!IsBinaryCoercible(attrType, opInputType))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("operator class \"%s\" does not accept data type %s",
						NameListToString(opclass), format_type_be(attrType))));

	ReleaseSysCache(tuple);

	return opClassId;
}

/*
 * GetDefaultOpClass
 *
 * 给定数据类型和访问方法的 OID，查找默认操作符类
 * （若有）。如果没有，则返回 InvalidOid。        
 */
Oid
GetDefaultOpClass(Oid type_id, Oid am_id)
{
	Oid			result = InvalidOid;
	int			nexact = 0;
	int			ncompatible = 0;
	int			ncompatiblepreferred = 0;
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TYPCATEGORY tcategory;

	/* 如果是域，则查找基类型 */
	type_id = getBaseType(type_id);

	tcategory = TypeCategory(type_id);

	/*
	 * 我们遍历该访问方法所有可用的 opclass，寻找一    
	 * 个被标记为默认且与目标类型匹配的 opclass          
	 * （精确或二进制兼容均可，但优先精确匹配）。  
	 *
	 * 我们可能找到多个二进制兼容的匹配。如果其中     
	 * 只有一个属于首选类型，就使用它；否则我们报错     
	 * 失败，迫使用户指明想要的那个。（首选类型的      
	 * 特例是为 varchar 而设的权宜之计：它与 text 和         
	 *  bpchar 都二进制兼容，需要裁决。）如果发现多个 
	 * 精确匹配，即 pg_opclass 有错。 
	 */
	rel = table_open(OperatorClassRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_opclass_opcmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(am_id));

	scan = systable_beginscan(rel, OpclassAmNameNspIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_opclass opclass = (Form_pg_opclass) GETSTRUCT(tup);

		/* 如果不是默认 opclass，则完全忽略 */
		if (!opclass->opcdefault)
			continue;
		if (opclass->opcintype == type_id)
		{
			nexact++;
			result = opclass->oid;
		}
		else if (nexact == 0 &&
				 IsBinaryCoercible(type_id, opclass->opcintype))
		{
			if (IsPreferredType(tcategory, opclass->opcintype))
			{
				ncompatiblepreferred++;
				result = opclass->oid;
			}
			else if (ncompatiblepreferred == 0)
			{
				ncompatible++;
				result = opclass->oid;
			}
		}
	}

	systable_endscan(scan);

	table_close(rel, AccessShareLock);

	/* 如果 pg_opclass 包含不一致的数据，则引发错误 */
	if (nexact > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("there are multiple default operator classes for data type %s",
						format_type_be(type_id))));

	if (nexact == 1 ||
		ncompatiblepreferred == 1 ||
		(ncompatiblepreferred == 0 && ncompatible == 1))
		return result;

	return InvalidOid;
}

/*
 *	makeObjectName()
 *
 *	为隐式创建的索引、序列、约束、扩展统计等创建  
 *	一个名称。          
 *
 *	参数通常为：原表名、原字段名和一个"类型"字符        
 *	串（例如 "seq" 或 "pkey"）。若不相关，字段名和/       
 *	或类型可以为 NULL。              
 *
 *	结果为 palloc 的字符串。
 *
 *	我们想要的基本结果是 "name1_name2_label"，当参数       
 *	为 NULL 时省略 "_name2" 或 "_label"。但是，我们生成   
 *	的名称必须少于 NAMEDATALEN 个字符！因此，如有        
 *	必要我们会截断一个或两个名称，以得到足够短的    
 *	字符串。标签绝不截断（故最好足够短）。 
 *
 *	调用者负责检查生成名称的唯一性并按需重试；   
 *	重试将通过修改 "label" 字符串来完成（这正是我们
 *	从不截断那部分的原因）。                      
 */
char *
makeObjectName(const char *name1, const char *name2, const char *label)
{
	char	   *name;
	int			overhead = 0;	/* chars needed for label and underscores */
	int			availchars;		/* chars available for name(s) */
	int			name1chars;		/* chars allocated to name1 */
	int			name2chars;		/* chars allocated to name2 */
	int			ndx;

	name1chars = strlen(name1);
	if (name2)
	{
		name2chars = strlen(name2);
		overhead++;				/* allow for separating underscore */
	}
	else
		name2chars = 0;
	if (label)
		overhead += strlen(label) + 1;

	availchars = NAMEDATALEN - 1 - overhead;
	Assert(availchars > 0);		/* else caller chose a bad label */

	/*
	 * 如果必须截断，则优先截断较长的名称。这一逻辑
	 * 本可不带循环地表达，但作为循环既简单明了           
	 *        
	 */
	while (name1chars + name2chars > availchars)
	{
		if (name1chars > name2chars)
			name1chars--;
		else
			name2chars--;
	}

	name1chars = pg_mbcliplen(name1, name1chars, name1chars);
	if (name2)
		name2chars = pg_mbcliplen(name2, name2chars, name2chars);

	/* 现在使用选择的长度构造字符串 */
	name = palloc(name1chars + name2chars + overhead + 1);
	memcpy(name, name1, name1chars);
	ndx = name1chars;
	if (name2)
	{
		name[ndx++] = '_';
		memcpy(name + ndx, name2, name2chars);
		ndx += name2chars;
	}
	if (label)
	{
		name[ndx++] = '_';
		strcpy(name + ndx, label);
	}
	else
		name[ndx] = '\0';

	return name;
}

/*
 * 为一个新关系选择一个不冲突的名称。这通常用于  
 * 选择索引名称（这正是它放在此处的原因），但也 
 * 可以用于序列或任何自动生成的关系种类。 
 *
 * name1、name2 和 label 的用法与 makeObjectName()                 
 * 相同，只是 label 不能为 NULL；必要时会在 label 后         
 * 追加数字，以创建在指定命名空间内唯一的名称。       
 *
 * 如果 isconstraint 为 true，我们还要避免选择与同一   
 * 命名空间中任何现有约束匹配的名称。（这比           
 * Postgres 自身要求的更严格，但 SQL 标准说约束名称        
 * 在模式内应当唯一，因此对于自动生成的约束名称  
 * 我们遵循它）
 *
 * 注意：如果有人并发地选择了相同的名称，理论上      
 * 仍然可能发生冲突。我们通过使用 SnapshotDirty 检查     
 * 冲突关系来缩短竞态条件的窗口，但这并不能完全   
 * 消除该窗口。这在实践中相当不太可能成为问题，    
 * 尤其是当持有 name1 所标识关系上的排他锁时。           
 * 但是，如果要在单条命令中选择多个名称，最好在    
 * 选择下一个名称之前创建新对象并执行               
 *  CommandCounterIncrement！                          
 *
 * 返回一个 palloc 串。
 */
char *
ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid,
				   bool isconstraint)
{
	int			pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];
	SnapshotData SnapshotDirty;
	Relation	pgclassrel;

	/* 准备使用脏快照搜索 pg_class */
	InitDirtySnapshot(SnapshotDirty);
	pgclassrel = table_open(RelationRelationId, AccessShareLock);

	/* 先尝试未修改的标签 */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		ScanKeyData key[2];
		SysScanDesc scan;
		bool		collides;

		relname = makeObjectName(name1, name2, modlabel);

		/* 是否有冲突的关系名称？ */
		ScanKeyInit(&key[0],
					Anum_pg_class_relname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(relname));
		ScanKeyInit(&key[1],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(namespaceid));

		scan = systable_beginscan(pgclassrel, ClassNameNspIndexId,
								  true /* indexOK */ ,
								  &SnapshotDirty,
								  2, key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		/* 如果没有冲突，跳出循环 */
		if (!collides)
		{
			if (!isconstraint ||
				!ConstraintNameExists(relname, namespaceid))
				break;
		}

		/* 发现冲突，因此尝试新的名称组件 */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(pgclassrel, AccessShareLock);

	return relname;
}

/*
 * 选择用于索引的名称。          
 *
 * 参数列表非常临时 :-(          
 */
static char *
ChooseIndexName(const char *tabname, Oid namespaceId,
				List *colnames, List *exclusionOpNames,
				bool primary, bool isconstraint)
{
	char	   *indexname;

	if (primary)
	{
		/* 主键的名称不依赖于特定列 */
		indexname = ChooseRelationName(tabname,
									   NULL,
									   "pkey",
									   namespaceId,
									   true);
	}
	else if (exclusionOpNames != NIL)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "excl",
									   namespaceId,
									   true);
	}
	else if (isconstraint)
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "key",
									   namespaceId,
									   true);
	}
	else
	{
		indexname = ChooseRelationName(tabname,
									   ChooseIndexNameAddition(colnames),
									   "idx",
									   namespaceId,
									   false);
	}

	return indexname;
}

/*
 * 为新索引根据其列名列表生成 "name2"                       
 * （由 ChooseIndexColumnNames 产生）。它将与父表名    
 * 一起传递给 ChooseRelationName，并附带一个合适的标签。   
 *
 * 我们知道实际使用的字符数会少于 NAMEDATALEN，        
 * 因此一旦生成了这么多字符就可以截断结果。 
 *
 * XXX 另见 ChooseForeignKeyConstraintNameAddition 和  
 * ChooseExtendedStatisticNameAddition.
 */
static char *
ChooseIndexNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = (const char *) lfirst(lc);

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */

		/*
		 * 此时我们有 buflen <= NAMEDATALEN。name 应该已经小于  
		 * NAMEDATALEN，但为了偏执使用 strlcpy。         
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * 为索引的列选择实际要使用的名称，给定这些列的       
 * IndexElems 列表。这主要是为了确保名称唯一，           
 * 以免得到属性名称冲突的错误。                           
 *
 * 返回纯字符串 List（char *，非 String 节点）。 
 */
static List *
ChooseIndexColumnNames(List *indexElems)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		const char *origname;
		const char *curname;
		int			i;
		char		buf[NAMEDATALEN];

		/* 从 IndexElem 获取初步名称 */
		if (ielem->indexcolname)
			origname = ielem->indexcolname; /* caller-specified name */
		else if (ielem->name)
			origname = ielem->name; /* simple column reference */
		else
			origname = "expr";	/* default name for expression */

		/* 如果它与任何先前的列冲突，调整它 */
		curname = origname;
		for (i = 1;; i++)
		{
			ListCell   *lc2;
			char		nbuf[32];
			int			nlen;

			foreach(lc2, result)
			{
				if (strcmp(curname, (char *) lfirst(lc2)) == 0)
					break;
			}
			if (lc2 == NULL)
				break;			/* found nonconflicting name */

			sprintf(nbuf, "%d", i);

			/* 确保生成的名称短于 NAMEDATALEN */
			nlen = pg_mbcliplen(origname, strlen(origname),
								NAMEDATALEN - 1 - strlen(nbuf));
			memcpy(buf, origname, nlen);
			strcpy(buf + nlen, nbuf);
			curname = buf;
		}

		/* 并附加到结果列表 */
		result = lappend(result, pstrdup(curname));
	}
	return result;
}

/*
 * ExecReindex
 *
 * 手动 REINDEX 命令的主要入口点。这主要是为         
 * REINDEX 的每个子例程中发生的实际操作准备的    
 * 包装器。               
 */
void
ExecReindex(ParseState *pstate, ReindexStmt *stmt, bool isTopLevel)
{
	ReindexParams params = {0};
	ListCell   *lc;
	bool		concurrently = false;
	bool		verbose = false;
	char	   *tablespacename = NULL;

	/* 解析选项列表 */
	foreach(lc, stmt->params)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "concurrently") == 0)
			concurrently = defGetBoolean(opt);
		else if (strcmp(opt->defname, "tablespace") == 0)
			tablespacename = defGetString(opt);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized REINDEX option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	if (concurrently)
		PreventInTransactionBlock(isTopLevel,
								  "REINDEX CONCURRENTLY");

	params.options =
		(verbose ? REINDEXOPT_VERBOSE : 0) |
		(concurrently ? REINDEXOPT_CONCURRENTLY : 0);

	/*
	 * 分配索引目标表空间的 OID；若为 InvalidOid 则         
	 * 跳过  
	 */
	if (tablespacename != NULL)
	{
		params.tablespaceOid = get_tablespace_oid(tablespacename, false);

		/* 检查权限，除非移动到数据库的默认值 */
		if (OidIsValid(params.tablespaceOid) &&
			params.tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(TableSpaceRelationId, params.tablespaceOid,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params.tablespaceOid));
		}
	}
	else
		params.tablespaceOid = InvalidOid;

	switch (stmt->kind)
	{
		case REINDEX_OBJECT_INDEX:
			ReindexIndex(stmt->relation, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_TABLE:
			ReindexTable(stmt->relation, &params, isTopLevel);
			break;
		case REINDEX_OBJECT_SCHEMA:
		case REINDEX_OBJECT_SYSTEM:
		case REINDEX_OBJECT_DATABASE:

			/*
			 * 它不能在用户事务块内运行；如果我们处于  
			 * 事务中，那么其提交与启动事务
			 * 命令的调用将不会产生预期的效                 
			 * 果！ 
			 */
			PreventInTransactionBlock(isTopLevel,
									  (stmt->kind == REINDEX_OBJECT_SCHEMA) ? "REINDEX SCHEMA" :
									  (stmt->kind == REINDEX_OBJECT_SYSTEM) ? "REINDEX SYSTEM" :
									  "REINDEX DATABASE");
			ReindexMultipleTables(stmt->name, stmt->kind, &params);
			break;
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) stmt->kind);
			break;
	}
}

/*
 * ReindexIndex
 *		重建特定索引。     
 */
static void
ReindexIndex(RangeVar *indexRelation, ReindexParams *params, bool isTopLevel)
{
	struct ReindexIndexCallbackState state;
	Oid			indOid;
	char		persistence;
	char		relkind;

	/*
	 * 查找并锁定索引，并检查表的权限；先通过回调     
	 * 获取表上的锁，以避免死锁风险。这里使用的锁      
	 * 级别必须与 reindex_index() 中获取的索引锁一致。  
	 *
	 * 如果是临时索引，即使请求了 CONCURRENTLY，我们也    
	 * 将执行非并发重建。在这种情况下，reindex_index()        
	 * 会升级锁，但没关系，因为其他会话无法持有      
	 * 我们临时表上的锁。  
	 */
	state.params = *params;
	state.locked_table_oid = InvalidOid;
	indOid = RangeVarGetRelidExtended(indexRelation,
									  (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									  ShareUpdateExclusiveLock : AccessExclusiveLock,
									  0,
									  RangeVarCallbackForReindexIndex,
									  &state);

	/*
	 * 获取现有索引的当前持久性和类型。我们已经      
	 * 持有索引的锁。            
	 */
	persistence = get_rel_persistence(indOid);
	relkind = get_rel_relkind(indOid);

	if (relkind == RELKIND_PARTITIONED_INDEX)
		ReindexPartitions(indOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 persistence != RELPERSISTENCE_TEMP)
		ReindexRelationConcurrently(indOid, params);
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		reindex_index(indOid, false, persistence, &newparams);
	}
}

/*
 * 在获取关系锁之前检查表的权限；还要在              
 * RangeVarGetRelidExtended 获取索引锁之前锁定堆，以避免        
 * 死锁。 
 */
static void
RangeVarCallbackForReindexIndex(const RangeVar *relation,
								Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;
	struct ReindexIndexCallbackState *state = arg;
	LOCKMODE	table_lockmode;

	/*
	 * 这里的锁级别在非并发情况下应与 reindex_index() 
	 * 中的表锁一致；并发时应与 index_concurrently_*() 所用    
	 * 表锁一致。 
	 */
	table_lockmode = (state->params.options & REINDEXOPT_CONCURRENTLY) != 0 ?
		ShareUpdateExclusiveLock : ShareLock;

	/*
	 * 如果我们之前锁定了某个其他索引的堆，而当前    
	 * 查找的名称已不再指向该关系，则释放现已无用的   
	 * 锁  
	 */
	if (relId != oldRelId && OidIsValid(oldRelId))
	{
		UnlockRelationOid(state->locked_table_oid, table_lockmode);
		state->locked_table_oid = InvalidOid;
	}

	/* 如果关系不存在，就没有什么可做的。 */
	if (!OidIsValid(relId))
		return;

	/*
	 * 如果关系确实存在，检查它是否是索引。但请注意，  
	 * 关系可能在我们进行名称查找和现在之间被删除。   
	 * 在这种情况下，没有什么可做的。        
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", relation->relname)));

	/* 检查权限 */
	if (!object_ownercheck(RelationRelationId, relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relation->relname);

	/* 在索引之前锁定堆以避免死锁。 */
	if (relId != oldRelId)
	{
		Oid			table_oid = IndexGetRelation(relId, true);

		/*
		 * 如果 OID 无效，说明索引已被并发删除，这对
		 * 我们来说并不是问题；只需正常返回。          
		 */
		if (OidIsValid(table_oid))
		{
			LockRelationOid(table_oid, table_lockmode);
			state->locked_table_oid = table_oid;
		}
	}
}

/*
 * ReindexTable
 *		重建表的所有索引（及其 toast 表，如果有）      
 */
static Oid
ReindexTable(RangeVar *relation, ReindexParams *params, bool isTopLevel)
{
	Oid			heapOid;
	bool		result;

	/*
	 * 这里使用的锁级别应匹配 reindex_relation()。  
	 *
	 * 如果是临时表，我们将执行非并发重建，即使        
	 * 请求了 CONCURRENTLY。在这种情况下，reindex_relation()     
	 * 会升级锁，但这是可以的，因为其他会话不能持有     
	 * 临时表的锁。           
	 */
	heapOid = RangeVarGetRelidExtended(relation,
									   (params->options & REINDEXOPT_CONCURRENTLY) != 0 ?
									   ShareUpdateExclusiveLock : ShareLock,
									   0,
									   RangeVarCallbackOwnsTable, NULL);

	if (get_rel_relkind(heapOid) == RELKIND_PARTITIONED_TABLE)
		ReindexPartitions(heapOid, params, isTopLevel);
	else if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			 get_rel_persistence(heapOid) != RELPERSISTENCE_TEMP)
	{
		result = ReindexRelationConcurrently(heapOid, params);

		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes that can be reindexed concurrently",
							relation->relname)));
	}
	else
	{
		ReindexParams newparams = *params;

		newparams.options |= REINDEXOPT_REPORT_PROGRESS;
		result = reindex_relation(heapOid,
								  REINDEX_REL_PROCESS_TOAST |
								  REINDEX_REL_CHECK_CONSTRAINTS,
								  &newparams);
		if (!result)
			ereport(NOTICE,
					(errmsg("table \"%s\" has no indexes to reindex",
							relation->relname)));
	}

	return heapOid;
}

/*
 * ReindexMultipleTables
 *		重建由 objectName/objectKind 选中的表的索引。     
 *
 * 为了降低死锁的概率，每个表都在单独的事务中     
 * 重建，这样我们就能立即释放其上的锁。这意味   
 * 着它绝不能在用户事务块内被调用！                   
 */
static void
ReindexMultipleTables(const char *objectName, ReindexObjectType objectKind,
					  ReindexParams *params)
{
	Oid			objectOid;
	Relation	relationRelation;
	TableScanDesc scan;
	ScanKeyData scan_keys[1];
	HeapTuple	tuple;
	MemoryContext private_context;
	MemoryContext old;
	List	   *relids = NIL;
	int			num_keys;
	bool		concurrent_warning = false;
	bool		tablespace_warning = false;

	Assert(objectKind == REINDEX_OBJECT_SCHEMA ||
		   objectKind == REINDEX_OBJECT_SYSTEM ||
		   objectKind == REINDEX_OBJECT_DATABASE);

	/*
	 * 这匹配语法强制执行的选项，其中对象名对于           
	 * DATABASE 和 SYSTEM 是可选的。 
	 */
	Assert(objectName || objectKind != REINDEX_OBJECT_SCHEMA);

	if (objectKind == REINDEX_OBJECT_SYSTEM &&
		(params->options & REINDEXOPT_CONCURRENTLY) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex system catalogs concurrently")));

	/*
	 * 获取待重建索引对象的 OID：数据库情形是会话当前  
	 * 使用的数据库；系统目录亦然；模式则是调用者定义  
	 * 的模式。同时执行权限检查，其处理方式因           
	 * 对象类型而异。                   
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		objectOid = get_namespace_oid(objectName, false);

		if (!object_ownercheck(NamespaceRelationId, objectOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA,
						   objectName);
	}
	else
	{
		objectOid = MyDatabaseId;

		if (objectName && strcmp(objectName, get_database_name(objectOid)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("can only reindex the currently open database")));
		if (!object_ownercheck(DatabaseRelationId, objectOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   get_database_name(objectOid));
	}

	/*
	 * 创建一个内存上下文，使其能在我们下面强制提交     
	 * 事务后依然存活。由于它是 PortalContext 的子上下 
	 * 文，即使遭遇错误它最终也会消失；无需特殊的   
	 * 中止清理逻辑  
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexMultipleTables",
											ALLOCSET_SMALL_SIZES);

	/*
	 * 定义搜索键以查找要重建索引的对象。对于模式，     
	 * 我们使用 relnamespace 选择目标关系，这对于数据库级   
	 * 操作不是必需的。  
	 */
	if (objectKind == REINDEX_OBJECT_SCHEMA)
	{
		num_keys = 1;
		ScanKeyInit(&scan_keys[0],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectOid));
	}
	else
		num_keys = 0;

	/*
	 * 扫描 pg_class 以构建需要重建索引的关系列表。     
	 *
	 * 我们只考虑普通关系和物化视图（toast rels 将由     
	 * reindex_relation 间接处理）。                    
	 */
	relationRelation = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(relationRelation, num_keys, scan_keys);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classtuple = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = classtuple->oid;

		/*
		 * 只有常规表和物化视图可以有索引，所以忽略任 
		 * 何其他关系。     
		 *
		 * 分区表/索引会被跳过，但匹配的叶子分区会被处   
		 * 理。        
		 */
		if (classtuple->relkind != RELKIND_RELATION &&
			classtuple->relkind != RELKIND_MATVIEW)
			continue;

		/* 跳过其他后端的临时表；我们根本无法重建它们的索引 */
		if (classtuple->relpersistence == RELPERSISTENCE_TEMP &&
			!isTempNamespace(classtuple->relnamespace))
			continue;

		/*
		 * 检查用户/系统分类。SYSTEM 处理所有目录，   
		 * DATABASE 处理所有非目录的内容。                       
		 */
		if (objectKind == REINDEX_OBJECT_SYSTEM &&
			!IsCatalogRelationOid(relid))
			continue;
		else if (objectKind == REINDEX_OBJECT_DATABASE &&
				 IsCatalogRelationOid(relid))
			continue;

		/*
		 * 若用户为超级用户、表所有者或数据库/模式所 
		 * 有者，则该表可以被重建（但后一种情况仅限于  
		 * 它不是共享关系）。object_ownercheck 包含超级      
		 * 用户情形，而根据 objectKind 我们已经知道，用户 
		 * 具有在此数据库或模式上运行 REINDEX 的权限（     
		 * 依据本例程开头的权限检查）。                 
		 */
		if (classtuple->relisshared &&
			!object_ownercheck(RelationRelationId, relid, GetUserId()))
			continue;

		/*
		 * 跳过系统表，因为 index_create() 会拒绝以并发方式   
		 * 对它们建索引（尝试时很可能失败）。 
		 */
		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			IsCatalogRelationOid(relid))
		{
			if (!concurrent_warning)
				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot reindex system catalogs concurrently, skipping all")));
			concurrent_warning = true;
			continue;
		}

		/*
		 * 如果设置了新的表空间，检查此关系是否需要
		 * 跳过  
		 */
		if (OidIsValid(params->tablespaceOid))
		{
			bool		skip_rel = false;

			/*
			 * 映射关系不能移动到不同的表空间（特别是    
			 * 这消除了所有共享目录）。             
			 */
			if (RELKIND_HAS_STORAGE(classtuple->relkind) &&
				!RelFileNumberIsValid(classtuple->relfilenode))
				skip_rel = true;

			/*
			 * 系统关系始终被跳过，即使启用了 
			 * allow_system_table_mods。      
			 */
			if (IsSystemClass(relid, classtuple))
				skip_rel = true;

			if (skip_rel)
			{
				if (!tablespace_warning)
					ereport(WARNING,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("cannot move system relations, skipping all")));
				tablespace_warning = true;
				continue;
			}
		}

		/* 将关系 OID 列表保存在私有上下文中 */
		old = MemoryContextSwitchTo(private_context);

		/*
		 * 若 pg_class 被选中重建，我们总希望最先重建它  
		 * 的索引。这样可以确保如果 pg_class 的索引有 
		 * 任何损坏，也会在我们处理其他任何表之前先行  
		 * 修复。这很关键，因为重建索引本身就会尝试   
		 * 更新 pg_class.
		 */
		if (relid == RelationRelationId)
			relids = lcons_oid(relid, relids);
		else
			relids = lappend_oid(relids, relid);

		MemoryContextSwitchTo(old);
	}
	table_endscan(scan);
	table_close(relationRelation, AccessShareLock);

	/*
	 * 在单独的事务中处理列出的每个关系。注意这会        
	 * 提交然后立即启动新事务。                  
	 */
	ReindexMultipleInternal(relids, params);

	MemoryContextDelete(private_context);
}

/*
 * ReindexPartitions() 特定的错误回调。   
 */
static void
reindex_error_callback(void *arg)
{
	ReindexErrorInfo *errinfo = (ReindexErrorInfo *) arg;

	Assert(RELKIND_HAS_PARTITIONS(errinfo->relkind));

	if (errinfo->relkind == RELKIND_PARTITIONED_TABLE)
		errcontext("while reindexing partitioned table \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
	else if (errinfo->relkind == RELKIND_PARTITIONED_INDEX)
		errcontext("while reindexing partitioned index \"%s.%s\"",
				   errinfo->relnamespace, errinfo->relname);
}

/*
 * ReindexPartitions
 *
 * 根据调用者给定的分区索引或表，重建一组分区      
 * 的索引。  
 */
static void
ReindexPartitions(Oid relid, ReindexParams *params, bool isTopLevel)
{
	List	   *partitions = NIL;
	char		relkind = get_rel_relkind(relid);
	char	   *relname = get_rel_name(relid);
	char	   *relnamespace = get_namespace_name(get_rel_namespace(relid));
	MemoryContext reindex_context;
	List	   *inhoids;
	ListCell   *lc;
	ErrorContextCallback errcallback;
	ReindexErrorInfo errinfo;

	Assert(RELKIND_HAS_PARTITIONS(relkind));

	/*
	 * 使用错误回调检查是否在事务块中运行，以提供     
	 * 问题发生所在的更多上下文。            
	 */
	errinfo.relname = pstrdup(relname);
	errinfo.relnamespace = pstrdup(relnamespace);
	errinfo.relkind = relkind;
	errcallback.callback = reindex_error_callback;
	errcallback.arg = (void *) &errinfo;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	PreventInTransactionBlock(isTopLevel,
							  relkind == RELKIND_PARTITIONED_TABLE ?
							  "REINDEX TABLE" : "REINDEX INDEX");

	/* 弹出错误上下文堆栈 */
	error_context_stack = errcallback.previous;

	/*
	 * 为跨事务存储创建一个特殊的内存上下文。   
	 *
	 * 由于它是 PortalContext 的子上下文，即使我们遭遇错   
	 * 误最终它也会消失，因此无需特殊的中止清理       
	 * 逻辑
	 */
	reindex_context = AllocSetContextCreate(PortalContext, "Reindex",
											ALLOCSET_DEFAULT_SIZES);

	/* ShareLock 足以防止模式修改 */
	inhoids = find_all_inheritors(relid, ShareLock, NULL);

	/*
	 * 要重建索引的关系列表是树的物理分区，             
	 * 因此丢弃任何分区表或索引。        
	 */
	foreach(lc, inhoids)
	{
		Oid			partoid = lfirst_oid(lc);
		char		partkind = get_rel_relkind(partoid);
		MemoryContext old_context;

		/*
		 * 这会丢弃分区表、分区索引和外部表。              
		 *        
		 */
		if (!RELKIND_HAS_STORAGE(partkind))
			continue;

		Assert(partkind == RELKIND_INDEX ||
			   partkind == RELKIND_RELATION);

		/* 保存分区 OID */
		old_context = MemoryContextSwitchTo(reindex_context);
		partitions = lappend_oid(partitions, partoid);
		MemoryContextSwitchTo(old_context);
	}

	/*
	 * 在单独的事务中处理列出的每个分区。注意这会    
	 * 提交然后立即启动新事务。                       
	 */
	ReindexMultipleInternal(partitions, params);

	/*
	 * 清理工作存储——注意必须在                
	 * StartTransactionCommand 之后操作，否则可能误删活动上下
	 * 文！  
	 */
	MemoryContextDelete(reindex_context);
}

/*
 * ReindexMultipleInternal
 *
 * 重建一组关系的索引，每个关系都在各自的       
 * 事务中处理。此函数会立即提交现有事务，并在 
 * 完成时启动新的事务。             
 */
static void
ReindexMultipleInternal(List *relids, ReindexParams *params)
{
	ListCell   *l;

	PopActiveSnapshot();
	CommitTransactionCommand();

	foreach(l, relids)
	{
		Oid			relid = lfirst_oid(l);
		char		relkind;
		char		relpersistence;

		StartTransactionCommand();

		/* 索引中的函数可能需要设置快照 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* 检查关系是否仍然存在 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		{
			PopActiveSnapshot();
			CommitTransactionCommand();
			continue;
		}

		/*
		 * 检查权限，除非选择了新表空间时移动到数据库的 
		 * 默认表空间。注意 ExecReindex() 中也做检查，  
		 * 但我们在此多做一次检查，因为此操作会跨越    
		 * 多个事务。       
		 */
		if (OidIsValid(params->tablespaceOid) &&
			params->tablespaceOid != MyDatabaseTableSpace)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(TableSpaceRelationId, params->tablespaceOid,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLESPACE,
							   get_tablespace_name(params->tablespaceOid));
		}

		relkind = get_rel_relkind(relid);
		relpersistence = get_rel_persistence(relid);

		/*
		 * 分区表和索引永远不能直接处理，应首先构建       
		 * 其叶子列表。                           
		 */
		Assert(!RELKIND_HAS_PARTITIONS(relkind));

		if ((params->options & REINDEXOPT_CONCURRENTLY) != 0 &&
			relpersistence != RELPERSISTENCE_TEMP)
		{
			ReindexParams newparams = *params;

			newparams.options |= REINDEXOPT_MISSING_OK;
			(void) ReindexRelationConcurrently(relid, &newparams);
			/* ReindexRelationConcurrently() 执行详细输出 */
		}
		else if (relkind == RELKIND_INDEX)
		{
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			reindex_index(relid, false, relpersistence, &newparams);
			PopActiveSnapshot();
			/* reindex_index() 执行详细输出 */
		}
		else
		{
			bool		result;
			ReindexParams newparams = *params;

			newparams.options |=
				REINDEXOPT_REPORT_PROGRESS | REINDEXOPT_MISSING_OK;
			result = reindex_relation(relid,
									  REINDEX_REL_PROCESS_TOAST |
									  REINDEX_REL_CHECK_CONSTRAINTS,
									  &newparams);

			if (result && (params->options & REINDEXOPT_VERBOSE) != 0)
				ereport(INFO,
						(errmsg("table \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(relid)),
								get_rel_name(relid))));

			PopActiveSnapshot();
		}

		CommitTransactionCommand();
	}

	StartTransactionCommand();
}


/*
 * ReindexRelationConcurrently - 处理给定关系 OID 的 REINDEX    
 * CONCURRENTLY
 *
 * 'relationOid' 可以属于索引、表或物化视图。对于表     
 * 和物化视图，其所有索引都将被重建，但不包括无        
 * 效索引以及任何用于排除约束的索引，但包括其关      
 * 联的 toast 表索引。对于索引而言，该索引本身会被      
 * 重建。              
 *
 * 在父表和相关索引上获取的锁会一直保持到事务提      
 * 交为止，届时将在每个关系上获取一个会话锁。这      
 * 两者都能防止并发的模式变更。                         
 *
 * 如果重建了任何索引（相关的 toast 表索引用到时      
 * 已包括），返回 true，否则为 false。  
 *
 * 注意：这不能用于临时关系。并发构建会与该并发         
 * 构建自身的事务所触发的 ON COMMIT 操作产生问题。       
 * 临时关系本来就不受并发问题影响，因此无需更复  
 * 杂的并发构建，无论如何，                                   
 * 非并发的 reindex 效率更高。                   
 */
static bool
ReindexRelationConcurrently(Oid relationOid, ReindexParams *params)
{
	typedef struct ReindexIndexInfo
	{
		Oid			indexId;
		Oid			tableId;
		Oid			amId;
		bool		safe;		/* for set_indexsafe_procflags */
	} ReindexIndexInfo;
	List	   *heapRelationIds = NIL;
	List	   *indexIds = NIL;
	List	   *newIndexIds = NIL;
	List	   *relationLocks = NIL;
	List	   *lockTags = NIL;
	ListCell   *lc,
			   *lc2;
	MemoryContext private_context;
	MemoryContext oldcontext;
	char		relkind;
	char	   *relationName = NULL;
	char	   *relationNamespace = NULL;
	PGRUsage	ru0;
	const int	progress_index[] = {
		PROGRESS_CREATEIDX_COMMAND,
		PROGRESS_CREATEIDX_PHASE,
		PROGRESS_CREATEIDX_INDEX_OID,
		PROGRESS_CREATEIDX_ACCESS_METHOD_OID
	};
	int64		progress_vals[4];

	/*
	 * 创建一个内存上下文，使其能在我们下面强制提交     
	 * 事务后依然存活。由于它是 PortalContext 的子上下 
	 * 文，即使遭遇错误它最终也会消失；无需特殊的   
	 * 中止清理逻辑  
	 */
	private_context = AllocSetContextCreate(PortalContext,
											"ReindexConcurrent",
											ALLOCSET_SMALL_SIZES);

	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		/* 将 REINDEX VERBOSE 所需的数据保存在私有上下文中 */
		oldcontext = MemoryContextSwitchTo(private_context);

		relationName = get_rel_name(relationOid);
		relationNamespace = get_namespace_name(get_rel_namespace(relationOid));

		pg_rusage_init(&ru0);

		MemoryContextSwitchTo(oldcontext);
	}

	relkind = get_rel_relkind(relationOid);

	/*
	 * 根据调用者给出的关系 Oid 提取要重建的索引          
	 * 列表。                    
	 */
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
		case RELKIND_TOASTVALUE:
			{
				/*
				 * 对于关系，查找其所有索引，包括 toast      
				 * 索引。     
				 */
				Relation	heapRelation;

				/* 将关系 OID 列表保存在私有上下文中 */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* 跟踪此关系以进行会话锁 */
				heapRelationIds = lappend_oid(heapRelationIds, relationOid);

				MemoryContextSwitchTo(oldcontext);

				if (IsCatalogRelationOid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/* 打开关系以获取其索引 */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(relationOid,
												  ShareUpdateExclusiveLock);
					/* 如果关系不存在，离开 */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(relationOid,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									RelationGetRelationName(heapRelation))));

				/* 将关系的所有有效索引添加到列表中 */
				foreach(lc, RelationGetIndexList(heapRelation))
				{
					Oid			cellOid = lfirst_oid(lc);
					Relation	indexRelation = index_open(cellOid,
														   ShareUpdateExclusiveLock);

					if (!indexRelation->rd_index->indisvalid)
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot reindex invalid index \"%s.%s\" concurrently, skipping",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid))));
					else if (indexRelation->rd_index->indisexclusion)
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot reindex exclusion constraint index \"%s.%s\" concurrently, skipping",
										get_namespace_name(get_rel_namespace(cellOid)),
										get_rel_name(cellOid))));
					else
					{
						ReindexIndexInfo *idx;

						/* 将关系 OID 列表保存在私有上下文中 */
						oldcontext = MemoryContextSwitchTo(private_context);

						idx = palloc_object(ReindexIndexInfo);
						idx->indexId = cellOid;
						/* 稍后设置其他字段 */

						indexIds = lappend(indexIds, idx);

						MemoryContextSwitchTo(oldcontext);
					}

					index_close(indexRelation, NoLock);
				}

				/* 也添加 toast 索引 */
				if (OidIsValid(heapRelation->rd_rel->reltoastrelid))
				{
					Oid			toastOid = heapRelation->rd_rel->reltoastrelid;
					Relation	toastRelation = table_open(toastOid,
														   ShareUpdateExclusiveLock);

					/* 将关系 OID 列表保存在私有上下文中 */
					oldcontext = MemoryContextSwitchTo(private_context);

					/* 跟踪此关系以进行会话锁 */
					heapRelationIds = lappend_oid(heapRelationIds, toastOid);

					MemoryContextSwitchTo(oldcontext);

					foreach(lc2, RelationGetIndexList(toastRelation))
					{
						Oid			cellOid = lfirst_oid(lc2);
						Relation	indexRelation = index_open(cellOid,
															   ShareUpdateExclusiveLock);

						if (!indexRelation->rd_index->indisvalid)
							ereport(WARNING,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("cannot reindex invalid index \"%s.%s\" concurrently, skipping",
											get_namespace_name(get_rel_namespace(cellOid)),
											get_rel_name(cellOid))));
						else
						{
							ReindexIndexInfo *idx;

							/*
							 * 在私有上下文中保存关系 OID    
							 * 列表 
							 */
							oldcontext = MemoryContextSwitchTo(private_context);

							idx = palloc_object(ReindexIndexInfo);
							idx->indexId = cellOid;
							indexIds = lappend(indexIds, idx);
							/* 稍后设置其他字段 */

							MemoryContextSwitchTo(oldcontext);
						}

						index_close(indexRelation, NoLock);
					}

					table_close(toastRelation, NoLock);
				}

				table_close(heapRelation, NoLock);
				break;
			}
		case RELKIND_INDEX:
			{
				Oid			heapId = IndexGetRelation(relationOid,
													  (params->options & REINDEXOPT_MISSING_OK) != 0);
				Relation	heapRelation;
				ReindexIndexInfo *idx;

				/* 如果关系缺失，离开 */
				if (!OidIsValid(heapId))
					break;

				if (IsCatalogRelationOid(heapId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex system catalogs concurrently")));

				/*
				 * 不允许对 TOAST 表上的无效索引执行 reindex，  
				 * 因为若重建则无法删除。错误消息需与   
				 * reindex_index() 保持一致。  
				 */
				if (IsToastNamespace(get_rel_namespace(relationOid)) &&
					!get_index_isvalid(relationOid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot reindex invalid index on TOAST table")));

				/*
				 * 检查父关系能否被锁定以及是否存在，     
				 * 此项工作必须在此阶段完成，因为待重建的 
				 * 索引列表尚未完成，且获取全部会话锁之   
				 * 后就不应再使用 REINDEXOPT_MISSING_OK。          
				 */
				if ((params->options & REINDEXOPT_MISSING_OK) != 0)
				{
					heapRelation = try_table_open(heapId,
												  ShareUpdateExclusiveLock);
					/* 如果关系不存在，离开 */
					if (!heapRelation)
						break;
				}
				else
					heapRelation = table_open(heapId,
											  ShareUpdateExclusiveLock);

				if (OidIsValid(params->tablespaceOid) &&
					IsSystemRelation(heapRelation))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot move system relation \"%s\"",
									get_rel_name(relationOid))));

				table_close(heapRelation, NoLock);

				/* 将关系 OID 列表保存在私有上下文中 */
				oldcontext = MemoryContextSwitchTo(private_context);

				/* 跟踪此索引的堆关系以进行会话锁 */
				heapRelationIds = list_make1_oid(heapId);

				/*
				 * 将关系 OID 列表保存在私有上下文中。注意
				 * 这里允许无效索引。           
				 */
				idx = palloc_object(ReindexIndexInfo);
				idx->indexId = relationOid;
				indexIds = lappend(indexIds, idx);
				/* 稍后设置其他字段 */

				MemoryContextSwitchTo(oldcontext);
				break;
			}

		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_PARTITIONED_INDEX:
		default:
			/* 如果关系类型不受支持，返回错误 */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot reindex this type of relation concurrently")));
			break;
	}

	/*
	 * 肯定没有索引，所以离开。任何基于     
	 * REINDEXOPT_MISSING_OK 的检查应只在构建待处理索引         
	 * 列表期间进行，因为在本事务提交前获取的会话锁 
	 * 将确保在操作完成之前，它们不会被并发会话      
	 * 所删除。                           
	 */
	if (indexIds == NIL)
	{
		PopActiveSnapshot();
		return false;
	}

	/* 它不是共享目录，因此拒绝将其移动到共享表空间 */
	if (params->tablespaceOid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move non-shared relation to tablespace \"%s\"",
						get_tablespace_name(params->tablespaceOid))));

	Assert(heapRelationIds != NIL);

	/* *
	 * 现在我们拥有要在 indexIds 中处理的所有索引。
	 *
	 * 现在的阶段是：
	 *
	 * 1. 在目录中创建新索引
	 * 2. 构建新索引
	 * 3. 让新索引赶上在此期间插入的元组
	 * 4. 交换索引名称
	 * 5. 将旧索引标记为已死
	 * 6. 删除旧索引
	 *
	 *
	 *
	 */

	/*
	 * REINDEX CONCURRENTLY 阶段 1  
	 *
	 * 创建一个与旧索引属性相同的新索引，但它只在      
	 * 目录中注册，稍后才构建。然后获取所有相关表       
	 * 上的会话锁。更详细的说明请参见 DefineIndex() 中       
	 * 的类似代码。     
	 */

	foreach(lc, indexIds)
	{
		char	   *concurrentName;
		ReindexIndexInfo *idx = lfirst(lc);
		ReindexIndexInfo *newidx;
		Oid			newIndexId;
		Relation	indexRel;
		Relation	heapRel;
		Oid			save_userid;
		int			save_sec_context;
		int			save_nestlevel;
		Relation	newIndexRel;
		LockRelId  *lockrelid;
		Oid			tablespaceid;

		indexRel = index_open(idx->indexId, ShareUpdateExclusiveLock);
		heapRel = table_open(indexRel->rd_index->indrelid,
							 ShareUpdateExclusiveLock);

		/*
		 * 切换到表所有者的 userid，让任何索引函数都以      
		 * 该用户身份运行。还要锁定安全受限的操作，并 
		 * 使 GUC 变量的更改仅限于此命令。                   
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heapRel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();

		/* 确定此索引对 set_indexsafe_procflags 的安全性 */
		idx->safe = (RelationGetIndexExpressions(indexRel) == NIL &&
					 RelationGetIndexPredicate(indexRel) == NIL);
		idx->tableId = RelationGetRelid(heapRel);
		idx->amId = indexRel->rd_rel->relam;

		/* 此函数不应为临时关系调用。 */
		if (indexRel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			elog(ERROR, "cannot reindex a temporary table concurrently");

		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  idx->tableId);

		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = 0;	/* initializing */
		progress_vals[2] = idx->indexId;
		progress_vals[3] = idx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* 为新索引选择临时关系名称 */
		concurrentName = ChooseRelationName(get_rel_name(idx->indexId),
											NULL,
											"ccnew",
											get_rel_namespace(indexRel->rd_index->indrelid),
											false);

		/* 选择新表空间，toast 表的索引不被移动 */
		if (OidIsValid(params->tablespaceOid) &&
			heapRel->rd_rel->relkind != RELKIND_TOASTVALUE)
			tablespaceid = params->tablespaceOid;
		else
			tablespaceid = indexRel->rd_rel->reltablespace;

		/* 基于给定索引创建新索引定义 */
		newIndexId = index_concurrently_create_copy(heapRel,
													idx->indexId,
													tablespaceid,
													concurrentName);

		/*
		 * 现在打开新索引的关系，也需要其上的会话级   
		 * 锁。            
		 */
		newIndexRel = index_open(newIndexId, ShareUpdateExclusiveLock);

		/*
		 * 将 OID 和锁的列表保存在私有上下文中
		 */
		oldcontext = MemoryContextSwitchTo(private_context);

		newidx = palloc_object(ReindexIndexInfo);
		newidx->indexId = newIndexId;
		newidx->safe = idx->safe;
		newidx->tableId = idx->tableId;
		newidx->amId = idx->amId;

		newIndexIds = lappend(newIndexIds, newidx);

		/*
		 * 保存 lockrelid 以防每个关系被删除，然后关闭 
		 * 这些关系。父关系上的 lockrelid 并不在此获取，  
		 * 以避免在同一关系上获取多个锁，而是依赖于之前 
		 * 构建的 parentRelationIds。  
		 */
		lockrelid = palloc_object(LockRelId);
		*lockrelid = indexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);
		lockrelid = palloc_object(LockRelId);
		*lockrelid = newIndexRel->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		MemoryContextSwitchTo(oldcontext);

		index_close(indexRel, NoLock);
		index_close(newIndexRel, NoLock);

		/* 回滚由索引函数执行的任何 GUC 更改 */
		AtEOXact_GUC(false, save_nestlevel);

		/* 恢复 userid 和安全上下文 */
		SetUserIdAndSecContext(save_userid, save_sec_context);

		table_close(heapRel, NoLock);
	}

	/*
	 * 保存堆锁，以备后续与其他后端进行可见性检查时    
	 * 可能与会话冲突。         
	 */
	foreach(lc, heapRelationIds)
	{
		Relation	heapRelation = table_open(lfirst_oid(lc), ShareUpdateExclusiveLock);
		LockRelId  *lockrelid;
		LOCKTAG    *heaplocktag;

		/* 将锁列表保存在私有上下文中 */
		oldcontext = MemoryContextSwitchTo(private_context);

		/* 将堆关系的 lockrelid 添加到锁定关系列表中 */
		lockrelid = palloc_object(LockRelId);
		*lockrelid = heapRelation->rd_lockInfo.lockRelId;
		relationLocks = lappend(relationLocks, lockrelid);

		heaplocktag = palloc_object(LOCKTAG);

		/* 为此父关系保存 LOCKTAG 以用于等待阶段 */
		SET_LOCKTAG_RELATION(*heaplocktag, lockrelid->dbId, lockrelid->relId);
		lockTags = lappend(lockTags, heaplocktag);

		MemoryContextSwitchTo(oldcontext);

		/* 关闭堆关系 */
		table_close(heapRelation, NoLock);
	}

	/* 在每个表上获取会话级锁。 */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		LockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * 因为我们在此事务中不获取快照，所以不需要在此处
	 * 设置 PROC_IN_SAFE_IC 标志。     
	 */

	/*
	 * REINDEX CONCURRENTLY 阶段 2  
	 *
	 * 在单独的事务中构建每个新索引，以免事务不必要地  
	 * 间保持打开。但在此之前，需等待到没有正在运   
	 * 行的事务可能以旧的索引列表打开该索引的表。更    
	 * 多详情请参见 DefineIndex() 中的                       
	 * "phase 2"。                   
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_1);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);

		/* 为此索引的并发构建启动新事务 */
		StartTransactionCommand();

		/*
		 * 检查是否用户请求中止。此检查处于事务内，这样 
		 * xact.c 不会发出无用 WARNING，并确保会话级   
		 * 的锁会在中止时被清理。           
		 */
		CHECK_FOR_INTERRUPTS();

		/* 告诉并发索引构建忽略我们，如果索引符合条件 */
		if (newidx->safe)
			set_indexsafe_procflags();

		/* 设置 ActiveSnapshot，因为索引中的函数可能需要它 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * 为要构建的索引更新进度，涉及正确的父表。   
		 *                
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_BUILD;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		/* 执行新索引的并发构建 */
		index_concurrently_build(newidx->tableId, newidx->indexId);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	StartTransactionCommand();

	/*
	 * 因为我们在此事务中不获取快照或 Xid，所以不需要    
	 * 在此处设置 PROC_IN_SAFE_IC 标志。 
	 */

	/*
	 * REINDEX CONCURRENTLY 阶段 3  
	 *
	 * 在此阶段中，旧索引要追上前一阶段期间创建的任 
	 * 何新元组。更多详情请参见 DefineIndex() 中的              
	 * "phase 3"。     
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_2);
	WaitForLockersMultiple(lockTags, ShareLock, true);
	CommitTransactionCommand();

	foreach(lc, newIndexIds)
	{
		ReindexIndexInfo *newidx = lfirst(lc);
		TransactionId limitXmin;
		Snapshot	snapshot;

		StartTransactionCommand();

		/*
		 * 检查是否用户请求中止。此检查处于事务内，这样 
		 * xact.c 不会发出无用 WARNING，并确保会话级   
		 * 的锁会在中止时被清理。           
		 */
		CHECK_FOR_INTERRUPTS();

		/* 告诉并发索引构建忽略我们，如果索引符合条件 */
		if (newidx->safe)
			set_indexsafe_procflags();

		/*
		 * 获取将由 validate_index() 用于过滤候选元组的          
		 * "引用快照"。          
		 */
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		PushActiveSnapshot(snapshot);

		/*
		 * 为要构建的索引更新进度，涉及正确的父表。   
		 *                
		 */
		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  newidx->tableId);
		progress_vals[0] = PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY;
		progress_vals[1] = PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN;
		progress_vals[2] = newidx->indexId;
		progress_vals[3] = newidx->amId;
		pgstat_progress_update_multi_param(4, progress_index, progress_vals);

		validate_index(newidx->tableId, newidx->indexId, snapshot);

		/*
		 * 我们现在可以放弃活动的快照，我们仍然需要保存
		 * xmin 限制以等待更旧的快照。     
		 */
		limitXmin = snapshot->xmin;

		PopActiveSnapshot();
		UnregisterSnapshot(snapshot);

		/*
		 * 为确保不死锁，我们必须提交并再启动一个   
		 * 事务，并在此事务获取任何快照之前执行等         
		 * 待
		 */
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * 该索引现在可以认为是有效的：它包含所有当前   
		 * 感兴趣的元组。但由于它可能不含恰在参考快照   
		 * 获取之前删除的元组，必须等待任何可能持有更  
		 * 旧快照的事务结束。                  
		 *
		 * 因为此事务中不获取快照或 Xid，无需在此处  
		 * 设置 PROC_IN_SAFE_IC 标志。                     
		 */
		pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
									 PROGRESS_CREATEIDX_PHASE_WAIT_3);
		WaitForOlderSnapshots(limitXmin, true);

		CommitTransactionCommand();
	}

	/*
	 * REINDEX CONCURRENTLY 阶段 4  
	 *
	 * 现在新索引已经得到验证，将每个新索引与其对应    
	 * 的旧索引交换。       
	 *
	 * 我们将同时把新索引标记为有效、把旧索引标记为  
	 * 无效，以确保我们只会从名称正确的索引收到约束   
	 * 冲突。                      
	 */

	StartTransactionCommand();

	/*
	 * 因为此事务只进行目录操作而不进行任何索引操作，  
	 * 我们可以在此处无条件设置 PROC_IN_SAFE_IC 标志。
	 *                 
	 */
	set_indexsafe_procflags();

	forboth(lc, indexIds, lc2, newIndexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);
		ReindexIndexInfo *newidx = lfirst(lc2);
		char	   *oldName;

		/*
		 * 检查是否用户请求中止。此检查处于事务内，这样 
		 * xact.c 不会发出无用 WARNING，并确保会话级   
		 * 的锁会在中止时被清理。           
		 */
		CHECK_FOR_INTERRUPTS();

		/* 为旧索引选择关系名称 */
		oldName = ChooseRelationName(get_rel_name(oldidx->indexId),
									 NULL,
									 "ccold",
									 get_rel_namespace(oldidx->tableId),
									 false);

		/*
		 * 交换索引可能涉及对 TOAST 表的访问，因此请确保   
		 * 拥有有效快照。 
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * 将旧索引与新索引交换，同时把新索引标记为有 
		 * 效，旧索引标记为无效。  
		 */
		index_concurrently_swap(newidx->indexId, oldidx->indexId, oldName);

		PopActiveSnapshot();

		/*
		 * 使表的 relcache 无效，以便在此提交后所有会话   
		 * 都会刷新可能引用该索引的任何缓存计划。          
		 *       
		 */
		CacheInvalidateRelcacheByRelid(oldidx->tableId);

		/*
		 * 此处 CCI，让后续迭代能在目录中看到 oldName， 
		 * 并能为其自己的 oldName 选择不冲突的名称。      
		 * 否则，如果某个表有两个索引，其名称在前         
		 * NAMEDATALEN 减去几字符内相等，就可能引起     
		 * 冲突。  
		 */
		CommandCounterIncrement();
	}

	/* 提交此事务并使索引交换可见 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * 虽然如果所有索引都合格，我们本可以设置              
	 * 但其实并无真正的必要，因为我们只在等待结束     
	 * 后才获取 Xid，且持续时间极短。  
	 */

	/*
	 * REINDEX CONCURRENTLY 阶段 5  
	 *
	 * 把旧索引标记为已死。首先必须等待，直到没有   
	 * 任何运行中事务可能正在使用该索引查询。  
	 * 另请参见 index_drop()。  
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_4);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	foreach(lc, indexIds)
	{
		ReindexIndexInfo *oldidx = lfirst(lc);

		/*
		 * 检查是否用户请求中止。此检查处于事务内，这样 
		 * xact.c 不会发出无用 WARNING，并确保会话级   
		 * 的锁会在中止时被清理。           
		 */
		CHECK_FOR_INTERRUPTS();

		index_concurrently_set_dead(oldidx->tableId, oldidx->indexId);
	}

	/* 提交此事务以使更新可见。 */
	CommitTransactionCommand();
	StartTransactionCommand();

	/*
	 * 虽然如果所有索引都合格，我们本可以设置              
	 * 但其实并无真正的必要，因为我们只在等待结束     
	 * 后才获取 Xid，且持续时间极短。  
	 */

	/*
	 * REINDEX CONCURRENTLY 阶段 6  
	 *
	 * 删除旧的索引。
	 */

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_WAIT_5);
	WaitForLockersMultiple(lockTags, AccessExclusiveLock, true);

	PushActiveSnapshot(GetTransactionSnapshot());

	{
		ObjectAddresses *objects = new_object_addresses();

		foreach(lc, indexIds)
		{
			ReindexIndexInfo *idx = lfirst(lc);
			ObjectAddress object;

			object.classId = RelationRelationId;
			object.objectId = idx->indexId;
			object.objectSubId = 0;

			add_exact_object_address(&object, objects);
		}

		/*
		 * 使用 PERFORM_DELETION_CONCURRENT_LOCK 以便 index_drop() 用对
		 * 锁级别。     
		 */
		performMultipleDeletions(objects, DROP_RESTRICT,
								 PERFORM_DELETION_CONCURRENT_LOCK | PERFORM_DELETION_INTERNAL);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * 最后，释放表上的会话级锁。              
	 */
	foreach(lc, relationLocks)
	{
		LockRelId  *lockrelid = (LockRelId *) lfirst(lc);

		UnlockRelationIdForSession(lockrelid, ShareUpdateExclusiveLock);
	}

	/* 启动新事务以正确完成过程 */
	StartTransactionCommand();

	/* 记录我们所做的 */
	if ((params->options & REINDEXOPT_VERBOSE) != 0)
	{
		if (relkind == RELKIND_INDEX)
			ereport(INFO,
					(errmsg("index \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		else
		{
			foreach(lc, newIndexIds)
			{
				ReindexIndexInfo *idx = lfirst(lc);
				Oid			indOid = idx->indexId;

				ereport(INFO,
						(errmsg("index \"%s.%s\" was reindexed",
								get_namespace_name(get_rel_namespace(indOid)),
								get_rel_name(indOid))));
				/* 此处不显示 rusage，因为它不是按索引计算的。 */
			}

			ereport(INFO,
					(errmsg("table \"%s.%s\" was reindexed",
							relationNamespace, relationName),
					 errdetail("%s.",
							   pg_rusage_show(&ru0))));
		}
	}

	MemoryContextDelete(private_context);

	pgstat_progress_end_command();

	return true;
}

/*
 * 插入或删除适当的 pg_inherits 元组，使给定的索引成      
 * 为所指父索引的一个分区。         
 *
 * 这同时也会修正受影响索引的 pg_depend 信息。         
 */
void
IndexSetParentIndex(Relation partitionIdx, Oid parentOid)
{
	Relation	pg_inherits;
	ScanKeyData key[2];
	SysScanDesc scan;
	Oid			partRelid = RelationGetRelid(partitionIdx);
	HeapTuple	tuple;
	bool		fix_dependencies;

	/* 确保这是一个索引 */
	Assert(partitionIdx->rd_rel->relkind == RELKIND_INDEX ||
		   partitionIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * 扫描 pg_inherits，查找链接本索引到父的行。  
	 */
	pg_inherits = relation_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partRelid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));
	scan = systable_beginscan(pg_inherits, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);
	tuple = systable_getnext(scan);

	if (!HeapTupleIsValid(tuple))
	{
		if (parentOid == InvalidOid)
		{
			/*
			 * 没有 pg_inherits 行，也不需要父索引：此时无     
			 *      
			 */
			fix_dependencies = false;
		}
		else
		{
			StoreSingleInheritance(partRelid, parentOid, 1);
			fix_dependencies = true;
		}
	}
	else
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(tuple);

		if (parentOid == InvalidOid)
		{
			/*
			 * 存在一个我们想要清除的 pg_inherits 行；执行。 
			 */
			CatalogTupleDelete(pg_inherits, &tuple->t_self);
			fix_dependencies = true;
		}
		else
		{
			/*
			 * 存在 pg_inherits 行。若它正是想要的那个，就没  
			 * 问题；若不同，则等同于目录损坏，不应    
			 * 发生。         
			 */
			if (inhForm->inhparent != parentOid)
			{
				/* 意外：我们不应该在这种情况下被调用 */
				elog(ERROR, "bogus pg_inherit row: inhrelid %u inhparent %u",
					 inhForm->inhrelid, inhForm->inhparent);
			}

			/* 已经处于正确的状态 */
			fix_dependencies = false;
		}
	}

	/* 完成 pg_inherits 的操作 */
	systable_endscan(scan);
	relation_close(pg_inherits, RowExclusiveLock);

	/* 如果索引分区已添加到父索引，则设置 relhassubclass */
	if (OidIsValid(parentOid))
	{
		LockRelationOid(parentOid, ShareUpdateExclusiveLock);
		SetRelationHasSubclass(parentOid, true);
	}

	/* 在分区上正确设置 relispartition */
	update_relispartition(partRelid, OidIsValid(parentOid));

	if (fix_dependencies)
	{
		/*
		 * 插入/删除 pg_depend 行。若设置父索引，在父索引  
		 * 和表上添加 PARTITION 依赖；若移除父索引则删  
		 * 除 PARTITION 依赖。               
		 */
		if (OidIsValid(parentOid))
		{
			ObjectAddress partIdx;
			ObjectAddress parentIdx;
			ObjectAddress partitionTbl;

			ObjectAddressSet(partIdx, RelationRelationId, partRelid);
			ObjectAddressSet(parentIdx, RelationRelationId, parentOid);
			ObjectAddressSet(partitionTbl, RelationRelationId,
							 partitionIdx->rd_index->indrelid);
			recordDependencyOn(&partIdx, &parentIdx,
							   DEPENDENCY_PARTITION_PRI);
			recordDependencyOn(&partIdx, &partitionTbl,
							   DEPENDENCY_PARTITION_SEC);
		}
		else
		{
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_PRI);
			deleteDependencyRecordsForClass(RelationRelationId, partRelid,
											RelationRelationId,
											DEPENDENCY_PARTITION_SEC);
		}

		/* 使我们的更新可见 */
		CommandCounterIncrement();
	}
}

/*
 * IndexSetParentIndex 的子例程，把给定索引的 relispartition      
 * 标志更新为给定值。    
 */
static void
update_relispartition(Oid relationId, bool newval)
{
	HeapTuple	tup;
	Relation	classRel;
	ItemPointerData otid;

	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	otid = tup->t_self;
	Assert(((Form_pg_class) GETSTRUCT(tup))->relispartition != newval);
	((Form_pg_class) GETSTRUCT(tup))->relispartition = newval;
	CatalogTupleUpdate(classRel, &otid, tup);
	UnlockTuple(classRel, &otid, InplaceUpdateTupleLock);
	heap_freetuple(tup);
	table_close(classRel, RowExclusiveLock);
}

/*
 * 在 MyProc->statusFlags 中设置 PROC_IN_SAFE_IC。
 *
 * 并发构建索引时，可设置此标志告知其他  
 * 运行 CREATE INDEX CONCURRENTLY 或 REINDEX       
 *  CONCURRENTLY 的进程，让它们在等待并发快照时 
 * 忽略我们。一方面避免了无谓等待不相干     
 * 的进程；但更重要的是它在某些情况下避免了   
 * 死锁。                                                      
 *
 * 但这只能对不执行任何可能访问其他表的表达式
 * 的索引安全地完成，因此索引不能是表达式索引 
 * 也不能是部分索引。调用者负责确保只在上述     
 * 假设成立时才调用此例程。         
 *
 * （该标志会在事务结束时自动重置，因此每个      
 * 事务都得重设它）  
 */
static inline void
set_indexsafe_procflags(void)
{
	/*
	 * 基于当前的 isvalid 状态设置并行安全属性。          
	 * 只有有效索引可以被标记为并行安全。                    
	 */
	Assert(MyProc->xid == InvalidTransactionId &&
		   MyProc->xmin == InvalidTransactionId);

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags |= PROC_IN_SAFE_IC;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}
