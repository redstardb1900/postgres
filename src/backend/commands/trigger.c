/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"


/* GUC 变量 */
int			SessionReplicationRole = SESSION_REPLICATION_ROLE_ORIGIN;

/* 我们已深入触发器执行多少层？ */
static int	MyTriggerDepth = 0;

/* 局部函数原型 */
static void renametrig_internal(Relation tgrel, Relation targetrel,
								HeapTuple trigtup, const char *newname,
								const char *expected_name);
static void renametrig_partition(Relation tgrel, Oid partitionId,
								 Oid parentTriggerOid, const char *newname,
								 const char *expected_name);
static void SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger);
static bool GetTupleForTrigger(EState *estate,
							   EPQState *epqstate,
							   ResultRelInfo *relinfo,
							   ItemPointer tid,
							   LockTupleMode lockmode,
							   TupleTableSlot *oldslot,
							   TupleTableSlot **epqslot,
							   TM_Result *tmresultp,
							   TM_FailureData *tmfdp);
static bool TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
						   Trigger *trigger, TriggerEvent event,
						   Bitmapset *modifiedCols,
						   TupleTableSlot *oldslot, TupleTableSlot *newslot);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
									 int tgindx,
									 FmgrInfo *finfo,
									 Instrumentation *instr,
									 MemoryContext per_tuple_context);
static void AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
								  ResultRelInfo *src_partinfo,
								  ResultRelInfo *dst_partinfo,
								  int event, bool row_trigger,
								  TupleTableSlot *oldslot, TupleTableSlot *newslot,
								  List *recheckIndexes, Bitmapset *modifiedCols,
								  TransitionCaptureState *transition_capture,
								  bool is_crosspart_update);
static void AfterTriggerEnlargeQueryState(void);
static bool before_stmt_triggers_fired(Oid relid, CmdType cmdType);


/*
 * 创建触发器。返回创建的触发器的地址。        
 *
 * queryString 是 CREATE TRIGGER 命令的源文本。         
 * 若指定了 whenClause，则必须提供，否则可为 NULL。        
 *
 * relOid 如果非零，是应在上面创建触发器的关系。  
 * 如果为零，将查找语句中提供的名称。                     
 *
 * refRelOid 如果非零，是约束触发器所引用的关系。      
 * 如果为零，将根据需要查找语句中提供的                  
 * 约束关系名称。       
 *
 * constraintOid 非零时，表示此触发器正被内部创建    
 * 以实现该约束。会创建合适的 pg_depend 条目来链接        
 * 触发器与该约束。执行用户输入的 CREATE TRIGGER               
 * 命令时，constraintOid 为零。（对于 CREATE CONSTRAINT          
 * TRIGGER，在内部构建 pg_constraint 条目。）
 *
 * indexOid 如果不为零，是与该约束关联的索引的 OID。         
 * 除了把它存储到 pg_trigger.tgconstrindid 中之外，我们     
 * 对它不做任何事；但为分区表上的可推迟唯一约束 
 * 创建触发器时，会查找其子表。注意在这种情况下     
 * 不处理无效索引。     
 *
 * funcoid 如果不为零，则是要调用的函数的 OID。            
 * 提供时忽略 stmt->funcname。
 *
 * parentTriggerOid 如果不为零，则是生成此触发器的          
 * 父触发器；这样当它被删除时，此触发器也应被删      
 * 除。传递非零值的情况有两种：1）此函数递归以          
 * 在分区上创建触发器，2）创建子外键触发器时；    
 * 参见 CreateFKCheckTrigger() 和 createForeignKeyActionTriggers()。     
 *
 * 如果传递了 whenClause，它就是已经转换好的 WHEN       
 * 表达式；此时我们忽略传入的 stmt->whenClause。          
 *
 * 若 isInternal 为真，则这是内部生成的触发器。       
 * 此参数设置 pg_trigger 条目的 tgisinternal 字段，           
 * 若为真，还会使我们修改给定的触发器名以确保唯一。
 *
 * isInternal 不为真时，要求对关系具有 ACL_TRIGGER          
 * 权限，并对触发函数具有 ACL_EXECUTE 权限。对于          
 * 内部触发器由调用者自行应用所需权限检查。  
 *
 * 在分区表上调用时，此函数会递归以在所有分区上     
 * 创建触发器，除非 isInternal 为真，此时                  
 * 调用者应自行执行递归。in_partition 表示此类            
 * 递归调用；外部调用者应传递 "false"（但请参见      
 * CloneRowTriggersToPartition）。     
 */
ObjectAddress
CreateTrigger(CreateTrigStmt *stmt, const char *queryString,
			  Oid relOid, Oid refRelOid, Oid constraintOid, Oid indexOid,
			  Oid funcoid, Oid parentTriggerOid, Node *whenClause,
			  bool isInternal, bool in_partition)
{
	return
		CreateTriggerFiringOn(stmt, queryString, relOid, refRelOid,
							  constraintOid, indexOid, funcoid,
							  parentTriggerOid, whenClause, isInternal,
							  in_partition, TRIGGER_FIRES_ON_ORIGIN);
}

/*
 * 与上述类似；另外可以指定触发条件 
 * （always/origin/replica/disabled）。           
 */
ObjectAddress
CreateTriggerFiringOn(CreateTrigStmt *stmt, const char *queryString,
					  Oid relOid, Oid refRelOid, Oid constraintOid,
					  Oid indexOid, Oid funcoid, Oid parentTriggerOid,
					  Node *whenClause, bool isInternal, bool in_partition,
					  char trigger_fires_when)
{
	int16		tgtype;
	int			ncolumns;
	int16	   *columns;
	int2vector *tgattr;
	List	   *whenRtable;
	char	   *qual;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	Relation	pgrel;
	HeapTuple	tuple = NULL;
	Oid			funcrettype;
	Oid			trigoid = InvalidOid;
	char		internaltrigname[NAMEDATALEN];
	char	   *trigname;
	Oid			constrrelid = InvalidOid;
	ObjectAddress myself,
				referenced;
	char	   *oldtablename = NULL;
	char	   *newtablename = NULL;
	bool		partition_recurse;
	bool		trigger_exists = false;
	Oid			existing_constraint_oid = InvalidOid;
	bool		existing_isInternal = false;
	bool		existing_isClone = false;

	if (OidIsValid(relOid))
		rel = table_open(relOid, ShareRowExclusiveLock);
	else
		rel = table_openrv(stmt->relation, ShareRowExclusiveLock);

	/*
	 * 触发器必须在表或视图上，并且还有额外的    
	 * 关系类型相关的限制。      
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/* 表不能具有 INSTEAD OF 触发器 */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* 分区表不能具有 INSTEAD OF 触发器 */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));

		/*
		 * FOR EACH ROW 触发器有额外的限制       
		 */
		if (stmt->row)
		{
			/*
			 * 禁止使用转换表。          
			 *
			 * 注意，对分区中                                        
			 * 的转换表还有另一项限制；'has_superclass'   
			 * 的解释见下方。这里的检查只是为                
			 * 了防止如下情形：若在此处允许使用转换表，
			 * 那么对于没有分区的分区表，创建会成功；但 
			 * 在创建第一个分区时会被另一项限制阻止，  
			 * 这是非常不友好的行为。 
			 */
			if (stmt->transitionRels != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" is a partitioned table",
								RelationGetRelationName(rel)),
						 errdetail("ROW triggers with transition tables are not supported on partitioned tables.")));
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		/*
		 * 视图可有 INSTEAD OF 触发器（下面检查其         
		 * 为行级），或语句级 BEFORE/AFTER 触发器。
		 */
		if (stmt->timing != TRIGGER_TYPE_INSTEAD && stmt->row)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have row-level BEFORE or AFTER triggers.")));
		/* 禁止在视图上使用 TRUNCATE 触发器 */
		if (TRIGGER_FOR_TRUNCATE(stmt->events))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have TRUNCATE triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have INSTEAD OF triggers.")));

		/*
		 * 我们禁止约束触发器，以维护 FK 触                
		 * 发器不可延迟的假设。参见下方 AfterTriggers 数据 
		 * 结构处的注释。  
		 */
		if (stmt->isconstraint)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have constraint triggers.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	if (stmt->isconstraint)
	{
		/*
		 * 我们必须在目标关系上加锁以防被并发删除。 
		 * AccessShareLock 是否足够强并不清楚，但我们肯     
		 * 定至少需要这个程度的锁……否则，我们最终可  
		 * 能创建出一个引用不存在的表的 pg_constraint 
		 * 条目。         
		 */
		if (OidIsValid(refRelOid))
		{
			LockRelationOid(refRelOid, AccessShareLock);
			constrrelid = refRelOid;
		}
		else if (stmt->constrrel != NULL)
			constrrelid = RangeVarGetRelid(stmt->constrrel, AccessShareLock,
										   false);
	}

	/* 权限检查 */
	if (!isInternal)
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_TRIGGER);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		if (OidIsValid(constrrelid))
		{
			aclresult = pg_class_aclcheck(constrrelid, GetUserId(),
										  ACL_TRIGGER);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(constrrelid)),
							   get_rel_name(constrrelid));
		}
	}

	/*
	 * 当在分区表上调用、要创建非内部的 FOR EACH ROW      
	 * 触发器时，我们也会为每个分区创建一个触发器。 
	 *
	 * 为此，最好提前在所有分区上持有锁。          
	 */
	partition_recurse = !isInternal && stmt->row &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partition_recurse)
		list_free(find_all_inheritors(RelationGetRelid(rel),
									  ShareRowExclusiveLock, NULL));

	/* 计算 tgtype */
	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);
	tgtype |= stmt->timing;
	tgtype |= stmt->events;

	/* 禁止行级 TRUNCATE 触发器 */
	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_TRUNCATE(tgtype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("TRUNCATE FOR EACH ROW triggers are not supported")));

	/* INSTEAD 触发器必须是行级的，且不能有 WHEN 或列 */
	if (TRIGGER_FOR_INSTEAD(tgtype))
	{
		if (!TRIGGER_FOR_ROW(tgtype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers must be FOR EACH ROW")));
		if (stmt->whenClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have WHEN conditions")));
		if (stmt->columns != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have column lists")));
	}

	/*
	 * 我们尚不支持 ROW 转换变量命名，但解析器能识别   
	 * 该语法，以便我们在此给出更友好的消息。 
	 *
	 * 按照标准，REFERENCING TABLE 名称仅允许用            
	 * 于 AFTER 触发器。按照标准，REFERENCING ROW 名称不允许与
	 * FOR EACH STATEMENT 同用。按照标准，OLD/NEW、ROW/TABLE 的   
	 * 每种组合只允许出现一次。按照标准，创建仅用
	 * 于 INSERT 的触发器时不能指定 OLD，创建仅用于 DELETE  
	 * 的触发器时不能指定 NEW。 
	 *
	 * 请注意，标准允许 AFTER ... FOR EACH ROW 触发              
	 * 器同时引用 ROW 和 TABLE 转换数据。
	 */
	if (stmt->transitionRels != NIL)
	{
		List	   *varList = stmt->transitionRels;
		ListCell   *lc;

		foreach(lc, varList)
		{
			TriggerTransition *tt = lfirst_node(TriggerTransition, lc);

			if (!(tt->isTable))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ROW variable naming in the REFERENCING clause is not supported"),
						 errhint("Use OLD TABLE or NEW TABLE for naming transition tables.")));

			/*
			 * 由于上面已做过测试，下面省略进一步的        
			 * ROW 相关测试。如果以后允许为 OLD 和 NEW ROW   
			 * 变量命名，则需作调整。
			 */

			if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a foreign table",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on foreign tables cannot have transition tables.")));

			if (rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a view",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on views cannot have transition tables.")));

			/*
			 * 我们目前不允                                         
			 * 许分区或继承子表上带有转换表的行级触发  
			 * 器。这类触发器需要以某种方式看到被转换为所
			 * 属表格式的元组，且不清楚每个子表应看到元 
			 * 组的哪个子集。另请参见 ATExecAttachPartition()  
			 * 和 ATExecAddInherit() 中的禁止规定。   
			 */
			if (TRIGGER_FOR_ROW(tgtype) && has_superclass(rel->rd_id))
			{
				/* 使用适当的错误消息。 */
				if (rel->rd_rel->relispartition)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on partitions")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on inheritance children")));
			}

			if (stmt->timing != TRIGGER_TYPE_AFTER)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("transition table name can only be specified for an AFTER trigger")));

			if (TRIGGER_FOR_TRUNCATE(tgtype))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("TRUNCATE triggers with transition tables are not supported")));

			/*
			 * 我们目前不允许带转换表的多事件            
			 * 触发器（"INSERT OR UPDATE"），因为不清楚如何处  
			 * 理可能同时触发 INSERT 和 UPDATE 触发器的 INSERT 
			 * ... ON CONFLICT 语句。把插入的元组展示给 INSERT
			 * 触发器，将更新的元组展示给 UPDATE 触发器，   
			 * 但尚不清楚 INSERT OR UPDATE 触发器应看到什么。
			 * 如果我们能在以后的版本中确定正确的语义，  
			 * 则可解除此限制。     
			 */
			if (((TRIGGER_FOR_INSERT(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_UPDATE(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_DELETE(tgtype) ? 1 : 0)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with more than one event")));

			/*
			 * 我们目前不允                                    
			 * 许带转换表的特定列触发器。按规范，
			 * 这似乎需要为列的每种组合分别累积独立的转   
			 * 换表，对于相当边缘的特性来说工作量太大。  
			 */
			if (stmt->columns != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with column lists")));

			/*
			 * 我们禁止使用带转换表的约束触发器，       
			 * 以保护此类触发器不能延迟的假设。            
			 * 参见下面 AfterTriggers 数据结构的注释。 
			 *
			 * 目前这由语法强制执行，所以这里只是 Assert。  
			 */
			Assert(!stmt->isconstraint);

			if (tt->isNew)
			{
				if (!(TRIGGER_FOR_INSERT(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE can only be specified for an INSERT or UPDATE trigger")));

				if (newtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE cannot be specified multiple times")));

				newtablename = tt->name;
			}
			else
			{
				if (!(TRIGGER_FOR_DELETE(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE can only be specified for a DELETE or UPDATE trigger")));

				if (oldtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE cannot be specified multiple times")));

				oldtablename = tt->name;
			}
		}

		if (newtablename != NULL && oldtablename != NULL &&
			strcmp(newtablename, oldtablename) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("OLD TABLE name and NEW TABLE name cannot be the same")));
	}

	/*
	 * 解析 WHEN 子句（若存在且未传入已转换           
	 * 者）。       
	 *
	 * 注意，一个副作用是解析时会填充 whenRtable。如果      
	 * 传入的是已解析的子句，则不会填充，这正是我们     
	 * 想要的，下方加冗余依赖无意义。   
	 */
	if (!whenClause && stmt->whenClause)
	{
		ParseState *pstate;
		ParseNamespaceItem *nsitem;
		List	   *varList;
		ListCell   *lc;

		/* 设置用于解析的 pstate */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * 为 OLD 和 NEW 引用设置 nsitems。   
		 *
		 * 'OLD' 的 varno 必须始终等于 1，'NEW' 等于 2。     
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("old", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("new", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		/* 转换表达式。复制以确保不会修改原始表达式 */
		whenClause = transformWhereClause(pstate,
										  copyObject(stmt->whenClause),
										  EXPR_KIND_TRIGGER_WHEN,
										  "WHEN");
		/* 我们还必须修正其排序规则 */
		assign_expr_collations(pstate, whenClause);

		/*
		 * 检查不合法的 OLD/NEW 引用。       
		 *
		 * 注意：能用 pull_var_clause 仅仅因为我们不允许 
		 * WHEN 子句包含子选择；否则它将无法检查子选择   
		 * 的内容。  
		 */
		varList = pull_var_clause(whenClause, 0);
		foreach(lc, varList)
		{
			Var		   *var = (Var *) lfirst(lc);

			switch (var->varno)
			{
				case PRS2_OLD_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_INSERT(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("INSERT trigger's WHEN condition cannot reference OLD values"),
								 parser_errposition(pstate, var->location)));
					/* 系统列在这里是允许的 */
					break;
				case PRS2_NEW_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_DELETE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("DELETE trigger's WHEN condition cannot reference NEW values"),
								 parser_errposition(pstate, var->location)));
					if (var->varattno < 0 && TRIGGER_FOR_BEFORE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW system columns"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno == 0 &&
						RelationGetDescr(rel)->constr &&
						RelationGetDescr(rel)->constr->has_generated_stored)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("A whole-row reference is used and the table contains generated columns."),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno > 0 &&
						TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attgenerated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("Column \"%s\" is a generated column.",
										   NameStr(TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attname)),
								 parser_errposition(pstate, var->location)));
					break;
				default:
					/* 没有 add_missing_from 不会发生，所以只做 elog */
					elog(ERROR, "trigger WHEN condition cannot contain references to other relations");
					break;
			}
		}

		/* 我们需要 rtable 用于 recordDependencyOnExpr */
		whenRtable = pstate->p_rtable;

		qual = nodeToString(whenClause);

		free_parsestate(pstate);
	}
	else if (!whenClause)
	{
		whenClause = NULL;
		whenRtable = NIL;
		qual = NULL;
	}
	else
	{
		qual = nodeToString(whenClause);
		whenRtable = NIL;
	}

	/*
	 * 查找并验证触发器函数。      
	 */
	if (!OidIsValid(funcoid))
		funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	if (!isInternal)
	{
		aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION,
						   NameListToString(stmt->funcname));
	}
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "trigger")));

	/*
	 * 扫描 pg_trigger，查看是否已存在同名触发器。           
	 * 对内部生成的触发器跳过此检查，因为稍后我们会 
	 * 把名称改得唯一。
	 *
	 * 注意：这之所以安全，只因为我们持有该关系的     
	 * ShareRowExclusiveLock，触发器集合不会在期间变化。  
	 */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	if (!isInternal)
	{
		ScanKeyData skeys[2];
		SysScanDesc tgscan;

		ScanKeyInit(&skeys[0],
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));

		ScanKeyInit(&skeys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(stmt->trigname));

		tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
									NULL, 2, skeys);

		/* 最多应有一个匹配的元组 */
		if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger oldtrigger = (Form_pg_trigger) GETSTRUCT(tuple);

			trigoid = oldtrigger->oid;
			existing_constraint_oid = oldtrigger->tgconstraint;
			existing_isInternal = oldtrigger->tgisinternal;
			existing_isClone = OidIsValid(oldtrigger->tgparentid);
			trigger_exists = true;
			/* 复制元组以在 CatalogTupleUpdate() 中使用 */
			tuple = heap_copytuple(tuple);
		}
		systable_endscan(tgscan);
	}

	if (!trigger_exists)
	{
		/* 生成新触发器的 OID。 */
		trigoid = GetNewOidWithIndex(tgrel, TriggerOidIndexId,
									 Anum_pg_trigger_oid);
	}
	else
	{
		/*
		 * 如果指定了 OR REPLACE，我们将替换旧触发器；
		 * 否则报告重复名称。                 
		 */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" already exists",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * 内部触发器或子触发器（isClone）不                     
		 * 能被用户定义的触发器替换。但 in_partition 
		 * 时会跳过此测试，因为那时我们正从分区表          
		 * 递归而下，检查已在父级完成。 
		 */
		if ((existing_isInternal || existing_isClone) &&
			!isInternal && !in_partition)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is an internal or a child trigger",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * 不允许替换为约束触发器；                          
		 * gram.y 本应已强制这一点。
		 */
		Assert(!stmt->isconstraint);

		/*
		 * 也不允许                                                
		 * 替换现有的约束触发器。（这些限制的部分原因在 
		 * 于：在此类情况下处理待决触发器事件似乎很困 
		 * 难；另一部分原因是该命令可能还意味着要更 
		 * 改约束的属性，这似乎不太好。）
		 */
		if (OidIsValid(existing_constraint_oid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is a constraint trigger",
							stmt->trigname, RelationGetRelationName(rel))));
	}

	/*
	 * 若是用户输入的 CREATE CONSTRAINT TRIGGER 命令，则创 
	 * 建相应 pg_constraint 条目。 
	 */
	if (stmt->isconstraint && !OidIsValid(constraintOid))
	{
		/* 内部调用者应该已经创建了自己的约束 */
		Assert(!isInternal);
		constraintOid = CreateConstraintEntry(stmt->trigname,
											  RelationGetNamespace(rel),
											  CONSTRAINT_TRIGGER,
											  stmt->deferrable,
											  stmt->initdeferred,
											  true,
											  InvalidOid,	/* no parent */
											  RelationGetRelid(rel),
											  NULL, /* no conkey */
											  0,
											  0,
											  InvalidOid,	/* no domain */
											  InvalidOid,	/* no index */
											  InvalidOid,	/* no foreign key */
											  NULL,
											  NULL,
											  NULL,
											  NULL,
											  0,
											  ' ',
											  ' ',
											  NULL,
											  0,
											  ' ',
											  NULL, /* no exclusion */
											  NULL, /* 没有检查约束 */
											  NULL,
											  true, /* islocal */
											  0,	/* inhcount */
											  true, /* noinherit */
											  isInternal);	/* is_internal */
	}

	/*
	 * 如果触发器是内部生成的，则通过追加触发器 OID       
	 * 来修改提供的触发器名称，以确保唯一。（这些情况 
	 * 下调用者通常提供简单常量触发器名。）
	 */
	if (isInternal)
	{
		snprintf(internaltrigname, sizeof(internaltrigname),
				 "%s_%u", stmt->trigname, trigoid);
		trigname = internaltrigname;
	}
	else
	{
		/* 用户定义的触发器；直接使用指定的触发器名称 */
		trigname = stmt->trigname;
	}

	/*
	 * 构建新 pg_trigger 元组。 
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgparentid - 1] = ObjectIdGetDatum(parentTriggerOid);
	values[Anum_pg_trigger_tgname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
	values[Anum_pg_trigger_tgenabled - 1] = trigger_fires_when;
	values[Anum_pg_trigger_tgisinternal - 1] = BoolGetDatum(isInternal);
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(constrrelid);
	values[Anum_pg_trigger_tgconstrindid - 1] = ObjectIdGetDatum(indexOid);
	values[Anum_pg_trigger_tgconstraint - 1] = ObjectIdGetDatum(constraintOid);
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(stmt->deferrable);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(stmt->initdeferred);

	if (stmt->args)
	{
		ListCell   *le;
		char	   *args;
		int16		nargs = list_length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = strVal(lfirst(le));

			len += strlen(ar) + 4;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = '\0';
		foreach(le, stmt->args)
		{
			char	   *s = strVal(lfirst(le));
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			strcpy(d, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(""));
	}

	/* 如果是列特定触发器，构建列号数组 */
	ncolumns = list_length(stmt->columns);
	if (ncolumns == 0)
		columns = NULL;
	else
	{
		ListCell   *cell;
		int			i = 0;

		columns = (int16 *) palloc(ncolumns * sizeof(int16));
		foreach(cell, stmt->columns)
		{
			char	   *name = strVal(lfirst(cell));
			int16		attnum;
			int			j;

			/* 查找列名。不允许系统列 */
			attnum = attnameAttNum(rel, name, false);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								name, RelationGetRelationName(rel))));

			/* 检查重复 */
			for (j = i - 1; j >= 0; j--)
			{
				if (columns[j] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name)));
			}

			columns[i++] = attnum;
		}
	}
	tgattr = buildint2vector(columns, ncolumns);
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	/* 如果触发器有 WHEN 子句，设置 tgqual */
	if (qual)
		values[Anum_pg_trigger_tgqual - 1] = CStringGetTextDatum(qual);
	else
		nulls[Anum_pg_trigger_tgqual - 1] = true;

	if (oldtablename)
		values[Anum_pg_trigger_tgoldtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(oldtablename));
	else
		nulls[Anum_pg_trigger_tgoldtable - 1] = true;
	if (newtablename)
		values[Anum_pg_trigger_tgnewtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(newtablename));
	else
		nulls[Anum_pg_trigger_tgnewtable - 1] = true;

	/*
	 * 插入或替换 pg_trigger 元组。  
	 */
	if (!trigger_exists)
	{
		tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleInsert(tgrel, tuple);
	}
	else
	{
		HeapTuple	newtup;

		newtup = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleUpdate(tgrel, &tuple->t_self, newtup);
		heap_freetuple(newtup);
	}

	heap_freetuple(tuple);		/* 释放原始元组或新元组 */
	table_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgattr - 1]));
	if (oldtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgoldtable - 1]));
	if (newtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgnewtable - 1]));

	/*
	 * 更新关系的 pg_class 条目（如需要）；如无需要，发   
	 * 送 SI 消息使其他后端（及本后端）重建 relcache 条目。
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	if (!((Form_pg_class) GETSTRUCT(tuple))->relhastriggers)
	{
		((Form_pg_class) GETSTRUCT(tuple))->relhastriggers = true;

		CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

		CommandCounterIncrement();
	}
	else
		CacheInvalidateRelcacheByTuple(tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * 若正在替换触发器，则在记录新依赖之前先刷新    
	 * 所有旧依赖。 
	 */
	if (trigger_exists)
		deleteDependencyRecordsFor(TriggerRelationId, trigoid, true);

	/*
	 * 为触发器记录依赖。总是在函数上放置普               
	 * 通依赖。 
	 */
	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (isInternal && OidIsValid(constraintOid))
	{
		/*
		 * 这是为约束内部生成的触                           
		 * 发器，因此将其设为该约束的内部依赖。可以跳 
		 * 过对各关系的依赖，因为经由约束会存在间接  
		 * 依赖。  
		 */
		referenced.classId = ConstraintRelationId;
		referenced.objectId = constraintOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
	else
	{
		/*
		 * 用户 CREATE TRIGGER，因此放置依赖。我们使触      
		 * 发器在其关系被删除、或 FK 关系被删除时自动删
		 * 除。（自动删除与我们 7.3 之前的行为兼容。）
		 */
		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		if (OidIsValid(constrrelid))
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = constrrelid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		}
		/* 在这种情况下不可能有索引依赖关系 */
		Assert(!OidIsValid(indexOid));

		/*
		 * 如果是用户指定的约束触发器，使约束             
		 * 内部依赖触发器，而不是反过来。             
		 */
		if (OidIsValid(constraintOid))
		{
			referenced.classId = ConstraintRelationId;
			referenced.objectId = constraintOid;
			referenced.objectSubId = 0;
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
		}

		/*
		 * 如果是分区触发器，创建分区依赖关系。         
		 */
		if (OidIsValid(parentTriggerOid))
		{
			ObjectAddressSet(referenced, TriggerRelationId, parentTriggerOid);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, RelationGetRelid(rel));
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
		}
	}

	/* 如果是列特定触发器，在列上添加普通依赖关系 */
	if (columns != NULL)
	{
		int			i;

		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		for (i = 0; i < ncolumns; i++)
		{
			referenced.objectSubId = columns[i];
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * 如果触发器带 WHEN 子句，则为表达式里提到            
	 * 的对象添加依赖（例如函数及所用列）。  
	 */
	if (whenRtable != NIL)
		recordDependencyOnExpr(&myself, whenClause, whenRtable,
							   DEPENDENCY_NORMAL);

	/* 新触发器的创建后钩子 */
	InvokeObjectPostCreateHookArg(TriggerRelationId, trigoid, 0,
								  isInternal);

	/*
	 * 最后，如果需要，在子关系上创建触发器。
	 */
	if (partition_recurse)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
		int			i;
		MemoryContext oldcxt,
					perChildCxt;

		perChildCxt = AllocSetContextCreate(CurrentMemoryContext,
											"part trig clone",
											ALLOCSET_SMALL_SIZES);

		/*
		 * 我们目前不期望在带有有效 indexOid 的情况下       
		 * 被调用。若将来有变化，则需在此编写代码来查 
		 * 找对应的子索引。  
		 */
		Assert(!OidIsValid(indexOid));

		oldcxt = MemoryContextSwitchTo(perChildCxt);

		/* 迭代以在每个现有分区上创建触发器 */
		for (i = 0; i < partdesc->nparts; i++)
		{
			CreateTrigStmt *childStmt;
			Relation	childTbl;
			Node	   *qual;

			childTbl = table_open(partdesc->oids[i], ShareRowExclusiveLock);

			/*
			 * 通过复制原始解析节点来初始化伪造的解析   
			 * 节点，然后重置我们单独传递的字段。
			 */
			childStmt = (CreateTrigStmt *) copyObject(stmt);
			childStmt->funcname = NIL;
			childStmt->whenClause = NULL;

			/* 如果有 WHEN 子句，创建其修改后的副本 */
			qual = copyObject(whenClause);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
										childTbl, rel);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
										childTbl, rel);

			CreateTriggerFiringOn(childStmt, queryString,
								  partdesc->oids[i], refRelOid,
								  InvalidOid, InvalidOid,
								  funcoid, trigoid, qual,
								  isInternal, true, trigger_fires_when);

			table_close(childTbl, NoLock);

			MemoryContextReset(perChildCxt);
		}

		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(perChildCxt);
	}

	/* 保持对目标关系的锁直到事务结束 */
	table_close(rel, NoLock);

	return myself;
}

/*
 * TriggerSetParentTrigger
 *		将分区触发器设为父触发器的子触发器，若
 *		parentTrigId 为 InvalidOid 则移除该链接。   
 *
 * 这会更新约束的 pg_trigger 行以将其显                         
 * 示为继承而来，并添加 PARTITION 依赖，防止触发器被 
 * 单独删除。或者，反向执行。  
 */
void
TriggerSetParentTrigger(Relation trigRel,
						Oid childTrigId,
						Oid parentTrigId,
						Oid childTableId)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	Form_pg_trigger trigForm;
	HeapTuple	tuple,
				newtup;
	ObjectAddress depender;
	ObjectAddress referenced;

	/*
	 * 查找待删触发器。   
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(childTrigId));

	tgscan = systable_beginscan(trigRel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tuple = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for trigger %u", childTrigId);
	newtup = heap_copytuple(tuple);
	trigForm = (Form_pg_trigger) GETSTRUCT(newtup);
	if (OidIsValid(parentTrigId))
	{
		/* 不允许为已有父约束的约束设置父约束 */
		if (OidIsValid(trigForm->tgparentid))
			elog(ERROR, "trigger %u already has a parent trigger",
				 childTrigId);

		trigForm->tgparentid = parentTrigId;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		ObjectAddressSet(depender, TriggerRelationId, childTrigId);

		ObjectAddressSet(referenced, TriggerRelationId, parentTrigId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_PRI);

		ObjectAddressSet(referenced, RelationRelationId, childTableId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_SEC);
	}
	else
	{
		trigForm->tgparentid = InvalidOid;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);
	}

	heap_freetuple(newtup);
	systable_endscan(tgscan);
}


/*
 * 触发器删除要点。 
 */
void
RemoveTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			relid;
	Relation	rel;

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * 查找待删触发器。   
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for trigger %u", trigOid);

	/*
	 * 打开并排他锁定触发器所属的关系。            
	 */
	relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

	rel = table_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * 删除 pg_trigger 元组。 
	 */
	CatalogTupleDelete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	table_close(tgrel, RowExclusiveLock);

	/*
	 * 我们不去费心判断是否还有其他触发器存在，           
	 * 而这一判断本是决定是否可安全清除关系 relhastriggers  
	 * 标志所必需的。（无论如何，都可能有并发进程正在
	 * 添加新触发器。）因此，只需强制一次 relcache 失效，
	 * 让其他后端（以及本后端！）重建其 relcache 条目。即 
	 * 使已没有触发器残留，让 relhastriggers 保持为真也没有
	 * 太大危害。  
	 */
	CacheInvalidateRelcache(rel);

	/* 保持对触发器关系的锁直到事务结束 */
	table_close(rel, NoLock);
}

/*
 * get_trigger_oid - 按名称查找触发器得到其 OID。  
 *
 * missing_ok 为假时，未找到触发器则抛出错误；为  
 * 真则只返回 InvalidOid。
 */
Oid
get_trigger_oid(Oid relid, const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * 查找触发器，验证权限，设置对象地址        
	 */
	tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("trigger \"%s\" for table \"%s\" does not exist",
							trigname, get_rel_name(relid))));
		oid = InvalidOid;
	}
	else
	{
		oid = ((Form_pg_trigger) GETSTRUCT(tup))->oid;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);
	return oid;
}

/*
 * 在获取关系锁之前执行权限和完整性检查。                 
 */
static void
RangeVarCallbackForRenameTrigger(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 并发删除 */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* 只有表和视图可以有触发器 */
	if (form->relkind != RELKIND_RELATION && form->relkind != RELKIND_VIEW &&
		form->relkind != RELKIND_FOREIGN_TABLE &&
		form->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						rv->relname),
				 errdetail_relkind_not_supported(form->relkind)));

	/* 必须拥有表才能重命名其触发器之一 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 *		renametrig		- 更改关系上某触发器的名称       
 *
 *		触发器名在触发器目录中更改。 
 *		不保留先前名称的记录。      
 *
 *		从关系目录获取正确的关系（若无参数）   
 *		查触发器目录  
 *				查重名（关系内）      
 *				查原触发器（若无参数）
 *		改触发器元组的 tgname  
 *		更新目录中的行
 */
ObjectAddress
renametrig(RenameStmt *stmt)
{
	Oid			tgoid;
	Relation	targetrel;
	Relation	tgrel;
	HeapTuple	tuple;
	SysScanDesc tgscan;
	ScanKeyData key[2];
	Oid			relid;
	ObjectAddress address;

	/*
	 * 查找名称，检查权限并获取锁（在事务结              
	 * 束前我们不会释放它）。 
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 0,
									 RangeVarCallbackForRenameTrigger,
									 NULL);

	/* 已持有锁，因此只需要构建 relcache 条目。 */
	targetrel = relation_open(relid, NoLock);

	/*
	 * 在分区表上，此操作递归到分区。提前锁定所有        
	 * 表。         
	 */
	if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		(void) find_all_inheritors(relid, AccessExclusiveLock, NULL);

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * 搜索要修改的触发器。   
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(stmt->subname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger trigform;

		trigform = (Form_pg_trigger) GETSTRUCT(tuple);
		tgoid = trigform->oid;

		/*
		 * 如果触发器源                                            
		 * 自父分区表上的触发器，则拒绝该重命名。我们不
		 * 允许分区中的触发器与其父触发器名称不同：那 
		 * 会造成 pg_dump 无法重现的不一致。  
		 */
		if (OidIsValid(trigform->tgparentid))
			ereport(ERROR,
					errmsg("cannot rename trigger \"%s\" on table \"%s\"",
						   stmt->subname, RelationGetRelationName(targetrel)),
					errhint("Rename the trigger on the partitioned table \"%s\" instead.",
							get_rel_name(get_partition_parent(relid, false))));


		/* 重命名此关系上的触发器 … */
		renametrig_internal(tgrel, targetrel, tuple, stmt->newname,
							stmt->subname);

		/* … 如果它是分区表，递归到其分区 */
		if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(targetrel, true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partitionId = partdesc->oids[i];

				renametrig_partition(tgrel, partitionId, trigform->oid,
									 stmt->newname, stmt->subname);
			}
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(targetrel))));
	}

	ObjectAddressSet(address, TriggerRelationId, tgoid);

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	/*
	 * 关闭关系，保持排他锁！  
	 */
	relation_close(targetrel, NoLock);

	return address;
}

/*
 * renametrig 的子例程，执行在表上重命名一个触发      
 * 器的实际工作。
 *
 * 如果触发器的名称与预期的不同，则就此发出      
 * 一条 NOTICE。
 */
static void
renametrig_internal(Relation tgrel, Relation targetrel, HeapTuple trigtup,
					const char *newname, const char *expected_name)
{
	HeapTuple	tuple;
	Form_pg_trigger tgform;
	ScanKeyData key[2];
	SysScanDesc tgscan;

	/* 如果触发器已经有新名称，则无需操作。 */
	tgform = (Form_pg_trigger) GETSTRUCT(trigtup);
	if (strcmp(NameStr(tgform->tgname), newname) == 0)
		return;

	/*
	 * 在真正尝试重命名之前，先搜索同名触发器。        
	 * 那种情况下更新会失败并给出难看的消息，因此最好 
	 * 抛出一个更友好的错误。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(targetrel)));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for relation \"%s\" already exists",
						newname, RelationGetRelationName(targetrel))));
	systable_endscan(tgscan);

	/*
	 * 目标名称可用；用它更新现有的 pg_trigger 元组。       
	 */
	tuple = heap_copytuple(trigtup);	/* 需要可修改的副本 */
	tgform = (Form_pg_trigger) GETSTRUCT(tuple);

	/*
	 * 如果触发器的名称与我们预期的不同，让用户知晓。  
	 * （我们仍可继续，因为必然是经由 tgparentid 链接到     
	 * 达这里的。） 
	 */
	if (strcmp(NameStr(tgform->tgname), expected_name) != 0)
		ereport(NOTICE,
				errmsg("renamed trigger \"%s\" on relation \"%s\"",
					   NameStr(tgform->tgname),
					   RelationGetRelationName(targetrel)));

	namestrcpy(&tgform->tgname, newname);

	CatalogTupleUpdate(tgrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

	/*
	 * 使关系的 relcache 条目失效，以便向其他后端（以     
	 * 及本后端！）发送 SI 消息，使其重建 relcache 条目。
	 * （理想情况下这应自动发生……）
	 */
	CacheInvalidateRelcache(targetrel);
}

/*
 * renametrig 的子例程——当在分区表上重命名触发器时
 * 递归到分区的辅助函数。        
 */
static void
renametrig_partition(Relation tgrel, Oid partitionId, Oid parentTriggerOid,
					 const char *newname, const char *expected_name)
{
	SysScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tuple;

	/*
	 * 给定一个关系以及父关系上某触发器的 OID，在子关系
	 * 中找到对应的那个触发器，并将它重命名为给定的名  
	 * 称  
	 */
	ScanKeyInit(&key,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partitionId));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tuple);
		Relation	partitionRel;

		if (tgform->tgparentid != parentTriggerOid)
			continue;			/* not our trigger */

		partitionRel = table_open(partitionId, NoLock);

		/* 重命名此分区上的触发器 */
		renametrig_internal(tgrel, partitionRel, tuple, newname, expected_name);

		/* 如果此关系是分区表，递归到其分区 */
		if (partitionRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(partitionRel,
															  true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partoid = partdesc->oids[i];

				renametrig_partition(tgrel, partoid, tgform->oid, newname,
									 NameStr(tgform->tgname));
			}
		}
		table_close(partitionRel, NoLock);

		/* 最多应有一个匹配的元组 */
		break;
	}
	systable_endscan(tgscan);
}

/*
 * EnableDisableTrigger()
 *
 *	由 ALTER TABLE ENABLE/DISABLE [ REPLICA | ALWAYS ] TRIGGER      
 *	调用，来更改指定触发器的 'tgenabled' 字段 
 *
 * rel: 要处理的关系（调用者必须持有合适的锁） 
 * tgname: 要处理的触发器名，NULL 扫描全部触发器   
 * tgparent: 若非零，只处理 tgparentid 为该值的触发器 
 * fires_when: tgenabled 字段的新值。                         
 *			   除通用的启用/禁用之外，它还定义在会话复
 *			   制角色下该触发器应于何时触发。
 * skip_system: 若为真，跳过"系统"（约束）触发器      
 * recurse: 若为真，递归到分区   
 *
 * 调用者应已检查过表的权限；这                        
 * 里我们还强制要求：更改系统触发器的状态需要超
 * 级用户权限
 */
void
EnableDisableTrigger(Relation rel, const char *tgname, Oid tgparent,
					 char fires_when, bool skip_system, bool recurse,
					 LOCKMODE lockmode)
{
	Relation	tgrel;
	int			nkeys;
	ScanKeyData keys[2];
	SysScanDesc tgscan;
	HeapTuple	tuple;
	bool		found;
	bool		changed;

	/* 扫描 pg_triggers 中的相关条目 */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&keys[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	if (tgname)
	{
		ScanKeyInit(&keys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tgname));
		nkeys = 2;
	}
	else
		nkeys = 1;

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, nkeys, keys);

	found = changed = false;

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger oldtrig = (Form_pg_trigger) GETSTRUCT(tuple);

		if (OidIsValid(tgparent) && tgparent != oldtrig->tgparentid)
			continue;

		if (oldtrig->tgisinternal)
		{
			/* 系统触发器 … 可以处理吗？ */
			if (skip_system)
				continue;
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied: \"%s\" is a system trigger",
								NameStr(oldtrig->tgname))));
		}

		found = true;

		if (oldtrig->tgenabled != fires_when)
		{
			/* 需要更改这个 … 复制一份以便修改 */
			HeapTuple	newtup = heap_copytuple(tuple);
			Form_pg_trigger newtrig = (Form_pg_trigger) GETSTRUCT(newtup);

			newtrig->tgenabled = fires_when;

			CatalogTupleUpdate(tgrel, &newtup->t_self, newtup);

			heap_freetuple(newtup);

			changed = true;
		}

		/*
		 * 在分区表上更改 FOR EACH ROW 触发器时，除非          
		 * 指定了 ONLY，否则对分区也做同样的更改。 
		 *
		 * 注意，即使上面没有更改该触发器，                
		 * 我们也会递归，因为分区上该触发器的副本可能 
		 * 具有与父触发器不同的 tgenabled 值，因此可能需要  
		 * 更改。  
		 */
		if (recurse &&
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
			(TRIGGER_FOR_ROW(oldtrig->tgtype)))
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
			int			i;

			for (i = 0; i < partdesc->nparts; i++)
			{
				Relation	part;

				part = relation_open(partdesc->oids[i], lockmode);
				/* 匹配子触发器的 tgparentid，而不是其名称 */
				EnableDisableTrigger(part, NULL, oldtrig->oid,
									 fires_when, skip_system, recurse,
									 lockmode);
				table_close(part, NoLock);	/* 保持锁直到提交 */
			}
		}

		InvokeObjectPostAlterHook(TriggerRelationId,
								  oldtrig->oid, 0);
	}

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	if (tgname && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						tgname, RelationGetRelationName(rel))));

	/*
	 * 如果更改了任何内容，广播 SI 失效消息，强制     
	 * 每个后端（包括我们自己！）重建关系的 relcache   
	 * 条目。否则它们将无法及时应用该更改。
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * 为给定 relcache 条目构建触发器数据。        
 *
 * 注意，附加到 relcache 条目的触发器数据必须存储在  
 * CacheMemoryContext 中，以确保其与 relcache 条目存活同样长
 * 的时间。但我们此时应运行在一个存活期较短的工作上
 * 下文中。为避免例程中途失败时泄漏缓存内存，我们 
 * 在工作内存中构建临时 TriggerDesc，并将完成的结构 
 * 复制到缓存内存中。 
 */
void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc;
	int			numtrigs;
	int			maxtrigs;
	Trigger    *triggers;
	Relation	tgrel;
	ScanKeyData skey;
	SysScanDesc tgscan;
	HeapTuple	htup;
	MemoryContext oldContext;
	int			i;

	/*
	 * 分配一个工作数组来容纳触发器（必要时会              
	 * 扩展） 
	 */
	maxtrigs = 16;
	triggers = (Trigger *) palloc(maxtrigs * sizeof(Trigger));
	numtrigs = 0;

	/*
	 * 注意：由于我们使用 TriggerRelidNameIndexId 扫                
	 * 描触发器，将按名称顺序读取触发器，除非正 
	 * 处于紧急恢复操作期间（即 IgnoreSystemIndexes）。这又 
	 * 进而确保触发器会按名称顺序触发。  
	 */
	ScanKeyInit(&skey,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = table_open(TriggerRelationId, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
		Trigger    *build;
		Datum		datum;
		bool		isnull;

		if (numtrigs >= maxtrigs)
		{
			maxtrigs *= 2;
			triggers = (Trigger *) repalloc(triggers, maxtrigs * sizeof(Trigger));
		}
		build = &(triggers[numtrigs]);

		build->tgoid = pg_trigger->oid;
		build->tgname = DatumGetCString(DirectFunctionCall1(nameout,
															NameGetDatum(&pg_trigger->tgname)));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisinternal = pg_trigger->tgisinternal;
		build->tgisclone = OidIsValid(pg_trigger->tgparentid);
		build->tgconstrrelid = pg_trigger->tgconstrrelid;
		build->tgconstrindid = pg_trigger->tgconstrindid;
		build->tgconstraint = pg_trigger->tgconstraint;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		/* tgattr 是第一个变宽字段，因此可以直接访问 */
		build->tgnattr = pg_trigger->tgattr.dim1;
		if (build->tgnattr > 0)
		{
			build->tgattr = (int16 *) palloc(build->tgnattr * sizeof(int16));
			memcpy(build->tgattr, &(pg_trigger->tgattr.values),
				   build->tgnattr * sizeof(int16));
		}
		else
			build->tgattr = NULL;
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			char	   *p;

			val = DatumGetByteaPP(fastgetattr(htup,
											  Anum_pg_trigger_tgargs,
											  tgrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "tgargs is null in trigger for relation \"%s\"",
					 RelationGetRelationName(relation));
			p = (char *) VARDATA_ANY(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = pstrdup(p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgoldtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgoldtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgoldtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgnewtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgnewtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgnewtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgqual,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgqual = TextDatumGetCString(datum);
		else
			build->tgqual = NULL;

		numtrigs++;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);

	/* 可能没有任何触发器 */
	if (numtrigs == 0)
	{
		pfree(triggers);
		return;
	}

	/* 构建 trigdesc */
	trigdesc = (TriggerDesc *) palloc0(sizeof(TriggerDesc));
	trigdesc->triggers = triggers;
	trigdesc->numtriggers = numtrigs;
	for (i = 0; i < numtrigs; i++)
		SetTriggerFlags(trigdesc, &(triggers[i]));

	/* 将完成的 trigdesc 复制到缓存存储中 */
	oldContext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->trigdesc = CopyTriggerDesc(trigdesc);
	MemoryContextSwitchTo(oldContext);

	/* 释放工作内存 */
	FreeTriggerDesc(trigdesc);
}

/*
 * 更新 TriggerDesc 的提示标志以包含指定的触发器       
 */
static void
SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger)
{
	int16		tgtype = trigger->tgtype;

	trigdesc->trig_insert_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_update_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_delete_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	/* 没有行级截断触发器 */
	trigdesc->trig_truncate_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_TRUNCATE);
	trigdesc->trig_truncate_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_TRUNCATE);

	trigdesc->trig_insert_new_table |=
		(TRIGGER_FOR_INSERT(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_update_old_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
	trigdesc->trig_update_new_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_delete_old_table |=
		(TRIGGER_FOR_DELETE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
}

/*
 * 复制 TriggerDesc 数据结构。
 *
 * 副本分配在当前内存上下文中。          
 */
TriggerDesc *
CopyTriggerDesc(TriggerDesc *trigdesc)
{
	TriggerDesc *newdesc;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL || trigdesc->numtriggers <= 0)
		return NULL;

	newdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	memcpy(newdesc, trigdesc, sizeof(TriggerDesc));

	trigger = (Trigger *) palloc(trigdesc->numtriggers * sizeof(Trigger));
	memcpy(trigger, trigdesc->triggers,
		   trigdesc->numtriggers * sizeof(Trigger));
	newdesc->triggers = trigger;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		trigger->tgname = pstrdup(trigger->tgname);
		if (trigger->tgnattr > 0)
		{
			int16	   *newattr;

			newattr = (int16 *) palloc(trigger->tgnattr * sizeof(int16));
			memcpy(newattr, trigger->tgattr,
				   trigger->tgnattr * sizeof(int16));
			trigger->tgattr = newattr;
		}
		if (trigger->tgnargs > 0)
		{
			char	  **newargs;
			int16		j;

			newargs = (char **) palloc(trigger->tgnargs * sizeof(char *));
			for (j = 0; j < trigger->tgnargs; j++)
				newargs[j] = pstrdup(trigger->tgargs[j]);
			trigger->tgargs = newargs;
		}
		if (trigger->tgqual)
			trigger->tgqual = pstrdup(trigger->tgqual);
		if (trigger->tgoldtable)
			trigger->tgoldtable = pstrdup(trigger->tgoldtable);
		if (trigger->tgnewtable)
			trigger->tgnewtable = pstrdup(trigger->tgnewtable);
		trigger++;
	}

	return newdesc;
}

/*
 * 释放 TriggerDesc 数据结构。
 */
void
FreeTriggerDesc(TriggerDesc *trigdesc)
{
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	trigger = trigdesc->triggers;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		pfree(trigger->tgname);
		if (trigger->tgnattr > 0)
			pfree(trigger->tgattr);
		if (trigger->tgnargs > 0)
		{
			while (--(trigger->tgnargs) >= 0)
				pfree(trigger->tgargs[trigger->tgnargs]);
			pfree(trigger->tgargs);
		}
		if (trigger->tgqual)
			pfree(trigger->tgqual);
		if (trigger->tgoldtable)
			pfree(trigger->tgoldtable);
		if (trigger->tgnewtable)
			pfree(trigger->tgnewtable);
		trigger++;
	}
	pfree(trigdesc->triggers);
	pfree(trigdesc);
}

/*
 * 比较两个 TriggerDesc 结构的逻辑相等性。    
 */
#ifdef NOT_USED
bool
equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2)
{
	int			i,
				j;

	/*
	 * 我们无需检查提示标志，只需检查触发器数组本      
	 * 身；如果触发器相同且类型相同，标志应当也匹配。 
	 *
	 * 自 7.3 起我们假定触发器集合的顺序在比较      
	 * 中有意义；因此只需比较两个集合中对应的槽位。 
	 *
	 * 注意：比较 WHEN                                                 
	 * 子句的 stringToNode 形式意味着解析列位置会影响结果。
	 * 只要本函数仅用于检测严格相等（例如检查缓存条     
	 * 目是否过时），这就没有问题。 
	 */
	if (trigdesc1 != NULL)
	{
		if (trigdesc2 == NULL)
			return false;
		if (trigdesc1->numtriggers != trigdesc2->numtriggers)
			return false;
		for (i = 0; i < trigdesc1->numtriggers; i++)
		{
			Trigger    *trig1 = trigdesc1->triggers + i;
			Trigger    *trig2 = trigdesc2->triggers + i;

			if (trig1->tgoid != trig2->tgoid)
				return false;
			if (strcmp(trig1->tgname, trig2->tgname) != 0)
				return false;
			if (trig1->tgfoid != trig2->tgfoid)
				return false;
			if (trig1->tgtype != trig2->tgtype)
				return false;
			if (trig1->tgenabled != trig2->tgenabled)
				return false;
			if (trig1->tgisinternal != trig2->tgisinternal)
				return false;
			if (trig1->tgisclone != trig2->tgisclone)
				return false;
			if (trig1->tgconstrrelid != trig2->tgconstrrelid)
				return false;
			if (trig1->tgconstrindid != trig2->tgconstrindid)
				return false;
			if (trig1->tgconstraint != trig2->tgconstraint)
				return false;
			if (trig1->tgdeferrable != trig2->tgdeferrable)
				return false;
			if (trig1->tginitdeferred != trig2->tginitdeferred)
				return false;
			if (trig1->tgnargs != trig2->tgnargs)
				return false;
			if (trig1->tgnattr != trig2->tgnattr)
				return false;
			if (trig1->tgnattr > 0 &&
				memcmp(trig1->tgattr, trig2->tgattr,
					   trig1->tgnattr * sizeof(int16)) != 0)
				return false;
			for (j = 0; j < trig1->tgnargs; j++)
				if (strcmp(trig1->tgargs[j], trig2->tgargs[j]) != 0)
					return false;
			if (trig1->tgqual == NULL && trig2->tgqual == NULL)
				 /* 确定 */ ;
			else if (trig1->tgqual == NULL || trig2->tgqual == NULL)
				return false;
			else if (strcmp(trig1->tgqual, trig2->tgqual) != 0)
				return false;
			if (trig1->tgoldtable == NULL && trig2->tgoldtable == NULL)
				 /* 确定 */ ;
			else if (trig1->tgoldtable == NULL || trig2->tgoldtable == NULL)
				return false;
			else if (strcmp(trig1->tgoldtable, trig2->tgoldtable) != 0)
				return false;
			if (trig1->tgnewtable == NULL && trig2->tgnewtable == NULL)
				 /* 确定 */ ;
			else if (trig1->tgnewtable == NULL || trig2->tgnewtable == NULL)
				return false;
			else if (strcmp(trig1->tgnewtable, trig2->tgnewtable) != 0)
				return false;
		}
	}
	else if (trigdesc2 != NULL)
		return false;
	return true;
}
#endif							/* NOT_USED */

/*
 * 检查是否存在带转换表、                                         
 * 且会阻止表成为继承子表或分区的行级触发器。返回第 
 * 一个此类不兼容触发器的名称，若无则返回 NULL。
 */
const char *
FindTriggerIncompatibleWithInheritance(TriggerDesc *trigdesc)
{
	if (trigdesc != NULL)
	{
		int			i;

		for (i = 0; i < trigdesc->numtriggers; ++i)
		{
			Trigger    *trigger = &trigdesc->triggers[i];

			if (!TRIGGER_FOR_ROW(trigger->tgtype))
				continue;
			if (trigger->tgoldtable != NULL || trigger->tgnewtable != NULL)
				return trigger->tgname;
		}
	}

	return NULL;
}

/*
 * 调用触发器函数。
 *
 *		trigdata: 触发器描述符 
 *		tgindx: 在 finfo 和 instr 数组中的下标。 
 *		finfo: 缓存的触发器函数调用信息数组。     
 *		instr: 可选的 EXPLAIN ANALYZE 插桩状态数组。         
 *		per_tuple_context: 执行该函数用的内存上下文。   
 *
 * 返回该函数所返回的元组（或 NULL）。      
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	LOCAL_FCINFO(fcinfo, 0);
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	MemoryContext oldContext;

	/*
	 * 防范那些可能未能对转换表信息完成初始化的代码路  
	 * 径  
	 */
	Assert(((TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) &&
			TRIGGER_FIRED_AFTER(trigdata->tg_event) &&
			!(trigdata->tg_event & AFTER_TRIGGER_DEFERRABLE) &&
			!(trigdata->tg_event & AFTER_TRIGGER_INITDEFERRED)) ||
		   (trigdata->tg_oldtable == NULL && trigdata->tg_newtable == NULL));

	finfo += tgindx;

	/*
	 * 我们缓存 fmgr 查找信息，以避免每次调用都重复查 
	 * 找  
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * 若在 EXPLAIN ANALYZE 下，开始为此触发器计时。   
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * 在每元组内存上下文中进行                               
	 * 函数求值，这样泄漏的内存每元组回收一次。特别要  
	 * 注意，触发器函数创建的任何新元组都会存活到元组  
	 * 周期结束。 
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * 调用函数，不传递参数但设置上下文。           
	 */
	InitFunctionCallInfoData(*fcinfo, finfo, 0,
							 InvalidOid, (Node *) trigdata, NULL);

	pgstat_init_function_usage(fcinfo, &fcusage);

	MyTriggerDepth++;
	PG_TRY();
	{
		result = FunctionCallInvoke(fcinfo);
	}
	PG_FINALLY();
	{
		MyTriggerDepth--;
	}
	PG_END_TRY();

	pgstat_end_function_usage(&fcusage, true);

	MemoryContextSwitchTo(oldContext);

	/*
	 * 触发器协议允许函数返回空指针，但绝不允许设      
	 * 置 isnull 结果标志。 
	 */
	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo->flinfo->fn_oid)));

	/*
	 * 若在 EXPLAIN ANALYZE 下，停止为此触发器计时，并         
	 * 计一次"返回元组"（实为触发次数）。  
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);

	return (HeapTuple) DatumGetPointer(result);
}

void
ExecBSInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_insert_before_statement)
		return;

	/* 如果我们已在此上下文中触发了 BS 触发器，则为空操作 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_INSERT))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_insert_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

bool
ExecBRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, slot, false);

			/*
			 * 分区中的元组经过触发器后，用户                
			 * 可能已经更改了分区键，使得元组不再          
			 * 适合该分区。验证这一点。    
			 */
			if (trigger->tgisclone &&
				!ExecPartitionCheck(relinfo, slot, estate, false))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("moving row to another partition during a BEFORE FOR EACH ROW trigger is not supported"),
						 errdetail("Before executing trigger \"%s\", the row was to be in partition \"%s.%s\".",
								   trigger->tgname,
								   get_namespace_name(RelationGetNamespace(relinfo->ri_RelationDesc)),
								   RelationGetRelationName(relinfo->ri_RelationDesc))));

			if (should_free)
				heap_freetuple(oldtuple);

			/* 标记元组如果使用应重新获取 */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecARInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot, List *recheckIndexes,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		transition_capture->tcs_insert_new_table)
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_insert_after_row) ||
		(transition_capture && transition_capture->tcs_insert_new_table))
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  true, NULL, slot,
							  recheckIndexes, NULL,
							  transition_capture,
							  false);
}

bool
ExecIRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, slot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* 标记元组如果使用应重新获取 */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecBSDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_delete_before_statement)
		return;

	/* 如果我们已在此上下文中触发了 BS 触发器，则为空操作 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_DELETE))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_delete_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

/*
 * 执行 BEFORE ROW DELETE 触发器 
 *
 * 返回真表示调用者可以继续执行删除。返回假              
 * 表示调用者需要抑制删除，且如果被要求，我们还需传  
 * 回并发更新的元组（如果有）。 
 */
bool
ExecBRDeleteTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot **epqslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd)
{
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	bool		result = true;
	TriggerData LocTriggerData = {0};
	HeapTuple	trigtuple;
	bool		should_free = false;
	int			i;

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								LockTupleExclusive, slot, &epqslot_candidate,
								tmresult, tmfd))
			return false;

		/*
		 * 如果元组被并发更新，并且此函数的调用者   
		 * 请求了更新后的元组，则跳过触发器          
		 * 执行。 
		 */
		if (epqslot_candidate != NULL && epqslot != NULL)
		{
			*epqslot = epqslot_candidate;
			return false;
		}

		trigtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
	}
	else
	{
		trigtuple = fdw_trigtuple;
		ExecForceStoreHeapTuple(trigtuple, slot, false);
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	newtuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			result = false;		/* 告诉调用者抑制删除 */
			break;
		}
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	if (should_free)
		heap_freetuple(trigtuple);

	return result;
}

/*
 * 注意：如果 DELETE 是作为跨分区更新的一部分执行的，
 * is_crosspart_update 必须为真。 
 */
void
ExecARDeleteTriggers(EState *estate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		transition_capture->tcs_delete_old_table)
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_delete_after_row) ||
		(transition_capture && transition_capture->tcs_delete_old_table))
	{
		TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);

		Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
		if (fdw_trigtuple == NULL)
			GetTupleForTrigger(estate,
							   NULL,
							   relinfo,
							   tupleid,
							   LockTupleExclusive,
							   slot,
							   NULL,
							   NULL,
							   NULL);
		else
			ExecForceStoreHeapTuple(fdw_trigtuple, slot, false);

		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  true, slot, NULL, NIL, NULL,
							  transition_capture,
							  is_crosspart_update);
	}
}

bool
ExecIRDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, slot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	rettuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		rettuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (rettuple == NULL)
			return false;		/* 删除已被抑制 */
		if (rettuple != trigtuple)
			heap_freetuple(rettuple);
	}
	return true;
}

void
ExecBSUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};
	Bitmapset  *updatedCols;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_update_before_statement)
		return;

	/* 如果我们已在此上下文中触发了 BS 触发器，则为空操作 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_UPDATE))
		return;

	/* 语句级触发器操作在父表上 */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	/* 语句级触发器操作在父表上 */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	if (trigdesc && trigdesc->trig_update_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_UPDATE,
							  false, NULL, NULL, NIL,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  false);
}

bool
ExecBRUpdateTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	HeapTuple	trigtuple;
	bool		should_free_trig = false;
	bool		should_free_new = false;
	TriggerData LocTriggerData = {0};
	int			i;
	Bitmapset  *updatedCols;
	LockTupleMode lockmode;

	/* 确定要使用的锁模式 */
	lockmode = ExecUpdateLockMode(estate, relinfo);

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		/* 获取我们计划更新的磁盘上元组的副本 */
		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								lockmode, oldslot, &epqslot_candidate,
								tmresult, tmfd))
			return false;		/* 取消更新操作 */

		/*
		 * 在 READ COMMITTED 隔离级别下，目标                      
		 * 元组可能已因并发更新而被改变。这种情况下，  
		 * epqslot_candidate 中是原始的子计划输出元组，需     
		 * 要用 ExecGetUpdateNewTuple 构造一个新的可插入元组， 
		 * 来替换我们在 newslot 中收到的那个。我们和调用  
		 * 者都不再关心传入的元组，因此用较新的数据覆盖
		 * newslot 即可。   
		 */
		if (epqslot_candidate != NULL)
		{
			TupleTableSlot *epqslot_clean;

			epqslot_clean = ExecGetUpdateNewTuple(relinfo, epqslot_candidate,
												  oldslot);

			/*
			 * 通常，调用者的 newslot 也是由              
			 * ExecGetUpdateNewTuple 生成的，因此 epqslot_clean 就   
			 * 是同一个槽位，无需复制。若不是，则正确处 
			 * 理。
			 */
			if (unlikely(newslot != epqslot_clean))
				ExecCopySlot(newslot, epqslot_clean);

			/*
			 * 此时 newslot 含虚拟元组，可能                
			 * 引用 oldslot 元组在磁盘缓冲区中的一些字段。 
			 * 如果该元组与原目标元组不在同一页上，那    
			 * 么我们对该缓冲区唯一的 pin 来自 oldslot，而我  
			 * 们即将释放它。因此最好对 newslot 进行物化，  
			 * 确保它不含指向未 pin 缓冲区的引用。（下     
			 * 面也会将其物化，但那时才做就不安全了。）
			 */
			ExecMaterializeSlot(newslot);
		}

		/*
		 * 此处将 oldslot 转换为持有 trigtuple 的物化槽位。   
		 * 传给触发器的两个槽位均不持有缓冲区 pin。   
		 */
		trigtuple = ExecFetchSlotHeapTuple(oldslot, true, &should_free_trig);
	}
	else
	{
		/* 将 FDW 提供的元组放入 oldslot 以统一情况 */
		ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		trigtuple = fdw_trigtuple;
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free_new);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple == NULL)
		{
			if (should_free_trig)
				heap_freetuple(trigtuple);
			if (should_free_new)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, newslot, false);

			/*
			 * 如果触发器返回的/即将存储的元组                 
			 * 是旧行版本，且传给触发器的堆元组是在本
			 * 地分配的，则物化该槽位。否则我们可能在  
			 * 它仍被槽位引用时就将其释放。 
			 */
			if (should_free_trig && newtuple == trigtuple)
				ExecMaterializeSlot(newslot);

			if (should_free_new)
				heap_freetuple(oldtuple);

			/* 标记元组如果使用应重新获取 */
			newtuple = NULL;
		}
	}
	if (should_free_trig)
		heap_freetuple(trigtuple);

	return true;
}

/*
 * 注                                                                        
 * 意：'src_partinfo' 与 'dst_partinfo'                                 
 * 在非 NULL 时，分别指查询中 'relinfo' 所给根分区表一  
 * 次跨分区更新的源分区和目标分区。此时 'tupleid' 指源分
 * 区中"旧"元组的 ctid，'newslot' 含目标分区中的"新"元 
 * 组。此接口用于满足 ExecCrossPartitionUpdateForeignKey()
 * 的要求；那种情况下 is_crosspart_update 必须                   
 * 为真。 
 */
void
ExecARUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ResultRelInfo *src_partinfo,
					 ResultRelInfo *dst_partinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 List *recheckIndexes,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		(transition_capture->tcs_update_old_table ||
		 transition_capture->tcs_update_new_table))
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_update_after_row) ||
		(transition_capture &&
		 (transition_capture->tcs_update_old_table ||
		  transition_capture->tcs_update_new_table)))
	{
		/*
		 * 注意：如果 UPDATE 在更新分                             
		 * 区键操作中被转换为 DELETE+INSERT，那么该函数也会
		 * 分别为 DELETE 和 INSERT 被调用，以捕获转换表行。  
		 * 这种情况下旧元组或新元组之一可为 NULL。
		 */
		TupleTableSlot *oldslot;
		ResultRelInfo *tupsrc;

		Assert((src_partinfo != NULL && dst_partinfo != NULL) ||
			   !is_crosspart_update);

		tupsrc = src_partinfo ? src_partinfo : relinfo;
		oldslot = ExecGetTriggerOldSlot(estate, tupsrc);

		if (fdw_trigtuple == NULL && ItemPointerIsValid(tupleid))
			GetTupleForTrigger(estate,
							   NULL,
							   tupsrc,
							   tupleid,
							   LockTupleExclusive,
							   oldslot,
							   NULL,
							   NULL,
							   NULL);
		else if (fdw_trigtuple != NULL)
			ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		else
			ExecClearTuple(oldslot);

		AfterTriggerSaveEvent(estate, relinfo,
							  src_partinfo, dst_partinfo,
							  TRIGGER_EVENT_UPDATE,
							  true,
							  oldslot, newslot, recheckIndexes,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  is_crosspart_update);
	}
}

bool
ExecIRUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple, TupleTableSlot *newslot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, oldslot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			return false;		/* "do nothing" */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, newslot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* 标记元组如果使用应重新获取 */
			newtuple = NULL;
		}
	}

	return true;
}

void
ExecBSTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_truncate_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_TRUNCATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_TRUNCATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_truncate_after_statement)
		AfterTriggerSaveEvent(estate, relinfo,
							  NULL, NULL,
							  TRIGGER_EVENT_TRUNCATE,
							  false, NULL, NULL, NIL, NULL, NULL,
							  false);
}


/*
 * 将元组获取到 "oldslot" 中，必要时处理锁定和 EPQ      
 */
static bool
GetTupleForTrigger(EState *estate,
				   EPQState *epqstate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   LockTupleMode lockmode,
				   TupleTableSlot *oldslot,
				   TupleTableSlot **epqslot,
				   TM_Result *tmresultp,
				   TM_FailureData *tmfdp)
{
	Relation	relation = relinfo->ri_RelationDesc;

	if (epqslot != NULL)
	{
		TM_Result	test;
		TM_FailureData tmfd;
		int			lockflags = 0;

		*epqslot = NULL;

		/* 如果 EvalPlanQual 可能发生，调用者必须传递 epqstate */
		Assert(epqstate != NULL);

		/*
		 * 锁定元组以更新
		 */
		if (!IsolationUsesXactSnapshot())
			lockflags |= TUPLE_LOCK_FLAG_FIND_LAST_VERSION;
		test = table_tuple_lock(relation, tid, estate->es_snapshot, oldslot,
								estate->es_output_cid,
								lockmode, LockWaitBlock,
								lockflags,
								&tmfd);

		/* 让调用者知道此操作的状态 */
		if (tmresultp)
			*tmresultp = test;
		if (tmfdp)
			*tmfdp = tmfd;

		switch (test)
		{
			case TM_SelfModified:

				/*
				 * 目标元组已被当前命                           
				 * 令、或当前事务中更靠后的命令更新或  
				 * 删除。前一种情况我们忽略该元组，后一种
				 * 情况则抛出错误，理由与 nodeModifyTable.c 
				 * 中 ExecUpdate 和 ExecDelete 所         
				 * 列举的相同。
				 */
				if (tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* 将其视为已删除；不处理 */
				return false;

			case TM_Ok:
				if (tmfd.traversed)
				{
					/*
					 * 使用 EPQ 重新检查元组。对 MERGE，我们留
					 * 给调用者（它必须做额外的重新检查， 
					 * 且可能最终执行完全不同的动作）。    
					 */
					if (estate->es_plannedstmt->commandType == CMD_MERGE)
					{
						if (tmresultp)
							*tmresultp = TM_Updated;
						return false;
					}

					*epqslot = EvalPlanQual(epqstate,
											relation,
											relinfo->ri_RangeTableIndex,
											oldslot);

					/*
					 * 若 PlanQual 对更新元组失败——绝不能 
					 * 处理该元组！ 
					 */
					if (TupIsNull(*epqslot))
					{
						*epqslot = NULL;
						return false;
					}
				}
				break;

			case TM_Updated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				elog(ERROR, "unexpected table_tuple_lock status: %u", test);
				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				/* 元组已被删除 */
				return false;

			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;

			default:
				elog(ERROR, "unrecognized table_tuple_lock status: %u", test);
				return false;	/* 让编译器安静 */
		}
	}
	else
	{
		/*
		 * 我们预期元组存在，因此，用非常简单的错误处理
		 * 即可。
		 */
		if (!table_tuple_fetch_row_version(relation, tid, SnapshotAny,
										   oldslot))
			elog(ERROR, "failed to fetch tuple for trigger");
	}

	return true;
}

/*
 * 触发器是否启用？   
 */
static bool
TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
			   Trigger *trigger, TriggerEvent event,
			   Bitmapset *modifiedCols,
			   TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	/* 检查复制角色相关的启用状态 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}
	else						/* ORIGIN 或 LOCAL 角色 */
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_REPLICA ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}

	/*
	 * 检查列特定触发器（仅对 UPDATE 可能，实际上我们  
	 * 必须忽略其他事件类型的 tgattr）        
	 */
	if (trigger->tgnattr > 0 && TRIGGER_FIRED_BY_UPDATE(event))
	{
		int			i;
		bool		modified;

		modified = false;
		for (i = 0; i < trigger->tgnattr; i++)
		{
			if (bms_is_member(trigger->tgattr[i] - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols))
			{
				modified = true;
				break;
			}
		}
		if (!modified)
			return false;
	}

	/* 检查 WHEN 子句 */
	if (trigger->tgqual)
	{
		ExprState **predicate;
		ExprContext *econtext;
		MemoryContext oldContext;
		int			i;

		Assert(estate != NULL);

		/*
		 * trigger 属于 relinfo->ri_TrigDesc->triggers[]；找出           
		 * relinfo->ri_TrigWhenExprs[] 中匹配的元素 
		 */
		i = trigger - relinfo->ri_TrigDesc->triggers;
		predicate = &relinfo->ri_TrigWhenExprs[i];

		/*
		 * 如果首次执行该 WHEN 表达式，则为其构建表达    
		 * 式节点树。并将其保存在每查询内存上下文中，
		 * 使其在整个查询期间存活。 
		 */
		if (*predicate == NULL)
		{
			Node	   *tgqual;

			oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
			tgqual = stringToNode(trigger->tgqual);
			/* 将对 OLD 和 NEW 的引用更改为 INNER_VAR 和 OUTER_VAR */
			ChangeVarNodes(tgqual, PRS2_OLD_VARNO, INNER_VAR, 0);
			ChangeVarNodes(tgqual, PRS2_NEW_VARNO, OUTER_VAR, 0);
			/* ExecPrepareQual 需要隐式 AND 形式 */
			tgqual = (Node *) make_ands_implicit((Expr *) tgqual);
			*predicate = ExecPrepareQual((List *) tgqual, estate);
			MemoryContextSwitchTo(oldContext);
		}

		/*
		 * 我们将使用 EState 的每个元组上下文来求值 WHEN 
		 * 表达式（如果尚未存在则创建它）。    
		 */
		econtext = GetPerTupleExprContext(estate);

		/*
		 * 最后求值表达式，将旧元组和/或新元组             
		 * 作为 INNER_VAR/OUTER_VAR 分别可用。    
		 */
		econtext->ecxt_innertuple = oldslot;
		econtext->ecxt_outertuple = newslot;
		if (!ExecQual(*predicate, econtext))
			return false;
	}

	return true;
}


/* ----------
 * After 触发器    
 *
 * AfterTriggersData 结构保存当前事务树中待处理                 
 * AFTER 触发器事件的数据。（BEFORE 触发器会立即   
 * 触发，所以我们无需保存关于它们的任何持久状           
 * 态。）该结构及其大部分附属数据保存在                    
 *  TopTransactionContext 中；不过一些可以更早丢弃                
 * 的数据出现在相关子事务的 CurTransactionContext             
 * 中。此外，各个事件记录保存在 TopTransactionContext      
 *  的一个单独子上下文中。这样做主要是为了能从转储    
 * 看出事件占用多少空间。
 *
 * 由于待处理事件列表可能变得很大，我们花了    
 * 相当大的功夫来最小化每个事件的内存消耗。事件      
 * 记录被分组为块，同一块中相似事件的公共数据          
 * 只存储一次。             
 *
 * XXX 若增长过大，需能将每个事件数据保存到文件           
 * 中。
 * ----------
 */

/* 每个触发器的 SET CONSTRAINT 状态 */
typedef struct SetConstraintTriggerData
{
	Oid			sct_tgoid;
	bool		sct_tgisdeferred;
} SetConstraintTriggerData;

typedef struct SetConstraintTriggerData *SetConstraintTrigger;

/*
 * SET CONSTRAINT 事务内状态。       
 *
 * 此对象为单个 palloc 分配，以便轻松复制和释放。          
 *
 * all_isset 和 all_isdeferred 用于跟踪          
 * SET CONSTRAINTS ALL {DEFERRED, IMMEDIATE}。 
 *
 * trigstates[] 存各触发器 tgisdeferred 设置。   
 */
typedef struct SetConstraintStateData
{
	bool		all_isset;
	bool		all_isdeferred;
	int			numstates;		/* 正在使用的 trigstates[] 条目数 */
	int			numalloc;		/* trigstates[] 的分配大小 */
	SetConstraintTriggerData trigstates[FLEXIBLE_ARRAY_MEMBER];
} SetConstraintStateData;

typedef SetConstraintStateData *SetConstraintState;


/*
 * 触发器事件数据 
 *
 * 实际的每个事件数据 AfterTriggerEventData 包括 DONE/IN_PROGRESS  
 * 状态位、最多两个元组 CTID，以及可选的两个分区 OID。  
 * 每个事件记录还有一个关联的 AfterTriggerSharedData，它   
 * 在"块"内的所有相似事件实例之间共享。          
 *
 * 对于行级触发器，我们安排不在不需要的 ctid 字段      
 * 上浪费存储空间。常规表的更新使用两个；常规表        
 * 的插入和删除使用一个；外部表总是使用零个，并       
 * 把元组保存到 tuplestore 中。AFTER_TRIGGER_FDW_FETCH          
 *  指示 AfterTriggerExecute() 从该 tuplestore 取回新           
 * 元组或元组对，而 AFTER_TRIGGER_FDW_REUSE 则指示           
 * 它使用最近取回的元组。这样无论外部表上有           
 * 多少个触发器都只存一次。  
 *
 * 当分区表上的更新导致行在分区之间移动时，两个       
 * 分区的 OID 也会被保存，以便能够取回这些元组；    
 * 此类条目标记为 AFTER_TRIGGER_CP_UPDATE（即"跨              
 * 分区更新"）。
 *
 * 注意，我们需要外部表上的触发器严格按照其入队     
 * 顺序被触发，这样元组才能以正确的顺序从               
 *  tuplestore 中取出。为确保这一点，我们禁止外部表 
 * 上有可延迟的（约束）触发器。这也确保这类触发      
 * 器不会被延迟到外层触发器查询级别，这意味着在     
 * 查询级别结束时销毁 tuplestore 即可。     
 *
 * 语句级触发器总是带有 AFTER_TRIGGER_1CTID，尽管          
 * 它们并不需要 ctid 字段。我们没有足够的标志位空         
 * 间来优雅表示，这似乎不值得费太多周折。     
 *
 * 注意：ats_firing_id 初始为零，当设置                        
 *  AFTER_TRIGGER_IN_PROGRESS 时会被设为其他值。它           
 * 表明该触发器将在哪个触发周期中被触发（如果设   
 * 置了 DONE 则是已被触发）。虽然这是可变状态，         
 * 我们仍然可以把它保存在 AfterTriggerSharedData 中，           
 * 因为给定事件列表中同一类型事件的所有实例          
 * 都将在同一触发周期之间入队的情况下同时被触发。    
 * 故只需保证向现有记录附加新事件时为零。
 */
typedef uint32 TriggerFlags;

#define AFTER_TRIGGER_OFFSET			0x07FFFFFF	/* 必须是低位 */
#define AFTER_TRIGGER_DONE				0x80000000
#define AFTER_TRIGGER_IN_PROGRESS		0x40000000
/* 描述此事件大小和元组来源的位 */
#define AFTER_TRIGGER_FDW_REUSE			0x00000000
#define AFTER_TRIGGER_FDW_FETCH			0x20000000
#define AFTER_TRIGGER_1CTID				0x10000000
#define AFTER_TRIGGER_2CTID				0x30000000
#define AFTER_TRIGGER_CP_UPDATE			0x08000000
#define AFTER_TRIGGER_TUP_BITS			0x38000000
typedef struct AfterTriggerSharedData *AfterTriggerShared;

typedef struct AfterTriggerSharedData
{
	TriggerEvent ats_event;		/* 事件类型指示器，参见 trigger.h */
	Oid			ats_tgoid;		/* the trigger's ID */
	Oid			ats_relid;		/* 它所在的关系 */
	CommandId	ats_firing_id;	/* 触发周期的 ID */
	struct AfterTriggersTableData *ats_table;	/* 转换表访问 */
	Bitmapset  *ats_modifiedcols;	/* modified columns */
} AfterTriggerSharedData;

typedef struct AfterTriggerEventData *AfterTriggerEvent;

typedef struct AfterTriggerEventData
{
	TriggerFlags ate_flags;		/* 状态位和到共享数据的偏移量 */
	ItemPointerData ate_ctid1;	/* 插入、删除或旧更新后的元组 */
	ItemPointerData ate_ctid2;	/* new updated tuple */

	/*
	 * 在分区表的跨分区更新期间，我们还存储               
	 * 从中获取旧元组（ctid1）和新元组（ctid2）的            
	 * 源分区和目标分区的 OID。                           
	 */
	Oid			ate_src_part;
	Oid			ate_dst_part;
} AfterTriggerEventData;

/* AfterTriggerEventData，减去 ate_src_part, ate_dst_part */
typedef struct AfterTriggerEventDataNoOids
{
	TriggerFlags ate_flags;
	ItemPointerData ate_ctid1;
	ItemPointerData ate_ctid2;
}			AfterTriggerEventDataNoOids;

/* AfterTriggerEventData，减去 ate_*_part 和 ate_ctid2 */
typedef struct AfterTriggerEventDataOneCtid
{
	TriggerFlags ate_flags;		/* 状态位和到共享数据的偏移量 */
	ItemPointerData ate_ctid1;	/* 插入、删除或旧更新后的元组 */
}			AfterTriggerEventDataOneCtid;

/* AfterTriggerEventData，减去 ate_*_part, ate_ctid1 和 ate_ctid2 */
typedef struct AfterTriggerEventDataZeroCtids
{
	TriggerFlags ate_flags;		/* 状态位和到共享数据的偏移量 */
}			AfterTriggerEventDataZeroCtids;

#define SizeofTriggerEvent(evt) \
	(((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_CP_UPDATE ? \
	 sizeof(AfterTriggerEventData) : \
	 (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ? \
	  sizeof(AfterTriggerEventDataNoOids) : \
	  (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_1CTID ? \
	   sizeof(AfterTriggerEventDataOneCtid) : \
	   sizeof(AfterTriggerEventDataZeroCtids))))

#define GetTriggerSharedData(evt) \
	((AfterTriggerShared) ((char *) (evt) + ((evt)->ate_flags & AFTER_TRIGGER_OFFSET)))

/*
 * 为了避免 palloc 开销，我们将触发器事件保存在连续更大 
 * 块的数组中（一种稍微更复杂版本的可扩展数组）。
 * CHUNK_DATA_START 和 freeptr 之间的空间由 AfterTriggerEventData 
 * 记录占用；endfree 和 endptr 之间的空间是空闲的。      
 *                                            
 */
typedef struct AfterTriggerEventChunk
{
	struct AfterTriggerEventChunk *next;	/* list link */
	char	   *freeptr;		/* 块中空闲空间的起始 */
	char	   *endfree;		/* 块中空闲空间的结束 */
	char	   *endptr;			/* end of chunk */
	/* 事件数据在此 */
} AfterTriggerEventChunk;

#define CHUNK_DATA_START(cptr) ((char *) (cptr) + MAXALIGN(sizeof(AfterTriggerEventChunk)))

/* 事件列表 */
typedef struct AfterTriggerEventList
{
	AfterTriggerEventChunk *head;
	AfterTriggerEventChunk *tail;
	char	   *tailfree;		/* 尾部块的 freeptr */
} AfterTriggerEventList;

/* 辅助遍历事件列表的宏 */
#define for_each_chunk(cptr, evtlist) \
	for (cptr = (evtlist).head; cptr != NULL; cptr = cptr->next)
#define for_each_event(eptr, cptr) \
	for (eptr = (AfterTriggerEvent) CHUNK_DATA_START(cptr); \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))
/* 如果不需要每个块的特定处理，使用此宏 */
#define for_each_event_chunk(eptr, cptr, evtlist) \
	for_each_chunk(cptr, evtlist) for_each_event(eptr, cptr)

/* 用于从可能不是列表起始点开始遍历的宏 */
#define for_each_chunk_from(cptr) \
	for (; cptr != NULL; cptr = cptr->next)
#define for_each_event_from(eptr, cptr) \
	for (; \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))


/*
 * AFTER TRIGGERS 模块的所有每事务数据。       
 *
 * AfterTriggersData 具有以下字段：    
 *
 * firing_counter 在每次调用 afterTriggerInvokeEvents 时递增。    
 * 我们用当前触发周期的 ID 标记可触发事件，以便知道  
 * 要处理哪些事件。若触发器函数选择执行              
 *  SET CONSTRAINTS，行为就是合理的：内部的                  
 *  SET CONSTRAINTS 只会触发尚未被调度触发的事件。   
 *
 * state 跟踪 SET CONSTRAINTS 的事务局部效果。                 
 * 这些数据会跨失败的子事务保存和恢复。   
 *
 * events 是当前延迟事件的列表。它在当前事务的        
 * 所有子事务中都是全局的。在子事务中止时，我们  
 * 知道该子事务添加的事件位于列表末尾，因此丢弃   
 * 它们相对容易。事件列表块本身则存储在               
 *  event_cxt 中。                              
 *
 * query_depth 是嵌套 AfterTriggerBeginQuery 调用的当前           
 * 深度（空栈时 -1）。  
 *
 * query_stack[query_depth] 是每个查询级别的数据，字段如下：    
 *
 * events 是由当前查询排队的 AFTER 触发器事件列表。    
 * 在匹配的 AfterTriggerEndQuery 调用发生之前，这些       
 * 事件都无效。到那时我们会触发立即模式的触发器， 
 * 并把延迟事件追加到主列表。 
 *
 * fdw_tuplestore 是一个 tuplestore，包含当前查询排队    
 * 的事件所需的外部表元组。（注意：即使可能涉及   
 * 多个外部表，我们也只使用一个 tuplestore。这是       
 * 可行的，因为 tuplestore 并不真正关心其存储的元组     
 * 内容；但将来某天这可能会坏掉。）       
 *
 * tables 是当前查询目标表的 AfterTriggersTableData           
 * 结构列表（见下文）。   
 *
 * maxquerydepth 只是 query_stack 已分配的长度。    
 *
 * trans_stack 保存每个子事务的数据，包括这些字段：
 *
 * state 是 NULL 或指向保存的 SET CONSTRAINTS 状态         
 * 数据。修改该状态的每个子事务层级都会               
 * 先保存一份副本，中止时我们用它恢复状态。
 *
 * events 是 events 头/尾指针的副本，       
 * 我们在子事务中止时用它恢复那些值。              
 *
 * query_depth 是子事务开始时的 query_depth 值，           
 * 我们同样用它来在子事务中止时进行清理。  
 *
 * firing_counter 是子事务开始时的 firing_counter 值。           
 * 我们用它来识别哪些延迟触发器是在被中止的子事    
 * 务中触发（或标记触发）的。      
 *
 * 我们用 GetCurrentTransactionNestLevel() 来确定                  
 *  trans_stack 中的正确数组下标。maxtransdepth 是                 
 *  trans_stack 中已分配条目的数量。（不维护我们自         
 * 己的栈指针，可避免子事务中止期间的错误导致在   
 * 同一深度多次调用 AfterTriggerEndSubXact()。）
 *
 * 我们为当前查询的每个目标表和每种操作模式           
 * （INSERT/UPDATE/DELETE）创建一个 AfterTriggersTableData          
 *  结构，只要它拥有转换表或语句级触发器。它用于   
 * 保存相关的转换表，以及跟踪我们是否已经将语句   
 * 触发器入队的信息。（我们用该信息来防止同一条     
 * 语句的语句级触发器被触发多次，或者更准确地说        
 * 在每个转换表集合中被触发多次。）这些结构与转       
 * 换表 tuplestore 一起生活在（子）事务的                     
 *  CurTransactionContext 中。这样的寿命足够了，因为           
 * 我们不允许可延迟触发器使用转换表，所以只需
 * 存活到查询结束
 */
typedef struct AfterTriggersQueryData AfterTriggersQueryData;
typedef struct AfterTriggersTransData AfterTriggersTransData;
typedef struct AfterTriggersTableData AfterTriggersTableData;

typedef struct AfterTriggersData
{
	CommandId	firing_counter; /* 下一个要分配的触发 ID */
	SetConstraintState state;	/* 活动的 S C 状态 */
	AfterTriggerEventList events;	/* 延迟事件列表 */
	MemoryContext event_cxt;	/* 事件的内存上下文（如果有） */

	/* 每个查询级别的数据： */
	AfterTriggersQueryData *query_stack;	/* 下面显示的结构数组 */
	int			query_depth;	/* 上述数组中的当前索引 */
	int			maxquerydepth;	/* 上述数组的分配长度 */

	/* 每个子事务级别的数据： */
	AfterTriggersTransData *trans_stack;	/* 下面显示的结构数组 */
	int			maxtransdepth;	/* 上述数组的分配长度 */
} AfterTriggersData;

struct AfterTriggersQueryData
{
	AfterTriggerEventList events;	/* 此查询的待处理事件 */
	Tuplestorestate *fdw_tuplestore;	/* 所述事件的外来元组 */
	List	   *tables;			/* AfterTriggersTableData 列表，参见下文 */
};

struct AfterTriggersTransData
{
	/* 这些字段仅用于在子事务中止时重置： */
	SetConstraintState state;	/* 保存的 S C 状态，如果尚未保存则为 NULL */
	AfterTriggerEventList events;	/* saved list pointer */
	int			query_depth;	/* saved query_depth */
	CommandId	firing_counter; /* 保存的 firing_counter */
};

struct AfterTriggersTableData
{
	/* relid + cmdType 构成这些结构的查找键： */
	Oid			relid;			/* target table's OID */
	CmdType		cmdType;		/* event type, CMD_INSERT/UPDATE/DELETE */
	bool		closed;			/* true when no longer OK to add tuples */
	bool		before_trig_done;	/* did we already queue BS triggers? */
	bool		after_trig_done;	/* did we already queue AS triggers? */
	AfterTriggerEventList after_trig_events;	/* if so, saved list pointer */

	/*
	 * 我们为 UPDATE/INSERT/DELETE 维护单独的转换表，因为     
	 * MERGE 可以在单个语句中运行所有三种操作。注意 UPDATE 
	 * 需要新旧两个转换表，而 INSERT 只需要新表，            
	 * DELETE 只需要旧表。 
	 */

	/* UPDATE 的"旧"转换表（如果有） */
	Tuplestorestate *old_upd_tuplestore;
	/* UPDATE 的"新"转换表（如果有） */
	Tuplestorestate *new_upd_tuplestore;
	/* DELETE 的"旧"转换表（如果有） */
	Tuplestorestate *old_del_tuplestore;
	/* INSERT 的"新"转换表（如果有） */
	Tuplestorestate *new_ins_tuplestore;

	TupleTableSlot *storeslot;	/* for converting to tuplestore's format */
};

static AfterTriggersData afterTriggers;

static void AfterTriggerExecute(EState *estate,
								AfterTriggerEvent event,
								ResultRelInfo *relInfo,
								ResultRelInfo *src_relInfo,
								ResultRelInfo *dst_relInfo,
								TriggerDesc *trigdesc,
								FmgrInfo *finfo,
								Instrumentation *instr,
								MemoryContext per_tuple_context,
								TupleTableSlot *trig_tuple_slot1,
								TupleTableSlot *trig_tuple_slot2);
static AfterTriggersTableData *GetAfterTriggersTableData(Oid relid,
														 CmdType cmdType);
static TupleTableSlot *GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
												 TupleDesc tupdesc);
static Tuplestorestate *GetAfterTriggersTransitionTable(int event,
														TupleTableSlot *oldslot,
														TupleTableSlot *newslot,
														TransitionCaptureState *transition_capture);
static void TransitionTableAddTuple(EState *estate,
									TransitionCaptureState *transition_capture,
									ResultRelInfo *relinfo,
									TupleTableSlot *slot,
									TupleTableSlot *original_insert_tuple,
									Tuplestorestate *tuplestore);
static void AfterTriggerFreeQuery(AfterTriggersQueryData *qs);
static SetConstraintState SetConstraintStateCreate(int numalloc);
static SetConstraintState SetConstraintStateCopy(SetConstraintState origstate);
static SetConstraintState SetConstraintStateAddItem(SetConstraintState state,
													Oid tgoid, bool tgisdeferred);
static void cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent);


/*
 * 获取当前触发器查询层级的 FDW 元组集，如需要则      
 * 创建之。 
 */
static Tuplestorestate *
GetCurrentFDWTuplestore(void)
{
	Tuplestorestate *ret;

	ret = afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore;
	if (ret == NULL)
	{
		MemoryContext oldcxt;
		ResourceOwner saveResourceOwner;

		/*
		 * 使元组存储在子事务结束前保持有效。实际上     
		 * 只需到 AfterTriggerEndQuery() 为止。
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		saveResourceOwner = CurrentResourceOwner;
		CurrentResourceOwner = CurTransactionResourceOwner;

		ret = tuplestore_begin_heap(false, false, work_mem);

		CurrentResourceOwner = saveResourceOwner;
		MemoryContextSwitchTo(oldcxt);

		afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore = ret;
	}

	return ret;
}

/* * ----------
 * afterTriggerCheckState()
 *
 *	如果触发器事件实际处于 DEFERRED 状态，则返回真。
 * ----------
 */
static bool
afterTriggerCheckState(AfterTriggerShared evtshared)
{
	Oid			tgoid = evtshared->ats_tgoid;
	SetConstraintState state = afterTriggers.state;
	int			i;

	/*
	 * 对于不可延迟的触发器（即普通 AFTER ROW           
	 * 触发器和声明为 NOT DEFERRABLE 的约束），恒为假。
	 */
	if ((evtshared->ats_event & AFTER_TRIGGER_DEFERRABLE) == 0)
		return false;

	/*
	 * 如果约束状态存在，SET CONSTRAINTS 可能已为此触发器 
	 * 或所有触发器执行过。              
	 */
	if (state != NULL)
	{
		/* 检查此特定触发器的 SET CONSTRAINTS。 */
		for (i = 0; i < state->numstates; i++)
		{
			if (state->trigstates[i].sct_tgoid == tgoid)
				return state->trigstates[i].sct_tgisdeferred;
		}

		/* 检查 SET CONSTRAINTS ALL。 */
		if (state->all_isset)
			return state->all_isdeferred;
	}

	/*
	 * 否则返回触发器的默认状态。            
	 */
	return ((evtshared->ats_event & AFTER_TRIGGER_INITDEFERRED) != 0);
}

/* * ----------
 * afterTriggerCopyBitmap()
 *
 * 将位图复制到 AfterTriggerEvents 内存上下文中，
 * 这是 after trigger 事件保留的地方。
 * ----------
 */
static Bitmapset *
afterTriggerCopyBitmap(Bitmapset *src)
{
	Bitmapset	   *dst;
	MemoryContext	oldcxt;

	if (src == NULL)
		return NULL;

	oldcxt = MemoryContextSwitchTo(afterTriggers.event_cxt);

	dst = bms_copy(src);

	MemoryContextSwitchTo(oldcxt);

	return dst;
}

/* * ----------
 * afterTriggerAddEvent()
 *
 *	向指定队列添加新的触发器事件。
 *	传入的事件数据被复制。
 * ----------
 */
static void
afterTriggerAddEvent(AfterTriggerEventList *events,
					 AfterTriggerEvent event, AfterTriggerShared evtshared)
{
	Size		eventsize = SizeofTriggerEvent(event);
	Size		needed = eventsize + sizeof(AfterTriggerSharedData);
	AfterTriggerEventChunk *chunk;
	AfterTriggerShared newshared;
	AfterTriggerEvent newevent;

	/*
	 * 如果列表为空或尾部块空间不足，则新建一块。      
	 * 我们假设这里总是需要一条新的共享记录。     
	 */
	chunk = events->tail;
	if (chunk == NULL ||
		chunk->endfree - chunk->freeptr < needed)
	{
		Size		chunksize;

		/* 如果尚未创建事件上下文，则创建 */
		if (afterTriggers.event_cxt == NULL)
			afterTriggers.event_cxt =
				AllocSetContextCreate(TopTransactionContext,
									  "AfterTriggerEvents",
									  ALLOCSET_DEFAULT_SIZES);

		/*
		 * 块大小从 1KB 开始，允许增大到 1MB。              
		 * 这些数字相当随意，不过在 AFTER_TRIGGER_OFFSET          
		 * 处有硬限制；否则无法利用 ate_flags 的可用空间    
		 * 把事件记录链接到其共享记录。另一约束是，   
		 * 若块大小变得过大，在（并不太常见）的块内       
		 * 含许多不同事件类型的使用模式下，下面的搜索 
		 * 循环会变慢。因此，只有当前一块中共享记录   
		 * 不太多时才将前块大小加倍；否则减半。这使我   
		 * 们有一定能力适应查询的实际使用模式，同时      
		 * 在典型使用中仍保持较大的块大小。所有用到     
		 * 的块大小都应为 MAXALIGN 的倍数，从而确保          
		 * 共享记录都能安全地                                       
		 * 对齐。                      
		 */
#define MIN_CHUNK_SIZE 1024
#define MAX_CHUNK_SIZE (1024*1024)

#if MAX_CHUNK_SIZE > (AFTER_TRIGGER_OFFSET+1)
#error MAX_CHUNK_SIZE must not exceed AFTER_TRIGGER_OFFSET
#endif

		if (chunk == NULL)
			chunksize = MIN_CHUNK_SIZE;
		else
		{
			/* 前一个块的大小… */
			chunksize = chunk->endptr - (char *) chunk;
			/* 检查前一个块中的共享记录数 */
			if ((chunk->endptr - chunk->endfree) <=
				(100 * sizeof(AfterTriggerSharedData)))
				chunksize *= 2; /* okay, double it */
			else
				chunksize /= 2; /* too many shared records */
			chunksize = Min(chunksize, MAX_CHUNK_SIZE);
		}
		chunk = MemoryContextAlloc(afterTriggers.event_cxt, chunksize);
		chunk->next = NULL;
		chunk->freeptr = CHUNK_DATA_START(chunk);
		chunk->endptr = chunk->endfree = (char *) chunk + chunksize;
		Assert(chunk->endfree - chunk->freeptr >= needed);

		if (events->head == NULL)
			events->head = chunk;
		else
			events->tail->next = chunk;
		events->tail = chunk;
		/* events->tailfree 现在不同步，但我们会在下面修复它 */
	}

	/*
	 * 尝试在块中定位匹配的共享数据记录。如果           
	 * 没有就新建。   
	 */
	for (newshared = ((AfterTriggerShared) chunk->endptr) - 1;
		 (char *) newshared >= chunk->endfree;
		 newshared--)
	{
		/* 按不同的概率大致比较字段 */
		if (newshared->ats_tgoid == evtshared->ats_tgoid &&
			newshared->ats_event == evtshared->ats_event &&
			newshared->ats_firing_id == 0 &&
			newshared->ats_table == evtshared->ats_table &&
			newshared->ats_relid == evtshared->ats_relid &&
			bms_equal(newshared->ats_modifiedcols,
					  evtshared->ats_modifiedcols))
			break;
	}
	if ((char *) newshared < chunk->endfree)
	{
		*newshared = *evtshared;
		/* 现在我们必须创建位图的适当长寿副本 */
		newshared->ats_modifiedcols = afterTriggerCopyBitmap(evtshared->ats_modifiedcols);
		newshared->ats_firing_id = 0;	/* just to be sure */
		chunk->endfree = (char *) newshared;
	}

	/* 插入数据 */
	newevent = (AfterTriggerEvent) chunk->freeptr;
	memcpy(newevent, event, eventsize);
	/* … 并将新事件链接到其共享记录 */
	newevent->ate_flags &= ~AFTER_TRIGGER_OFFSET;
	newevent->ate_flags |= (char *) newshared - (char *) newevent;

	chunk->freeptr += eventsize;
	events->tailfree = chunk->freeptr;
}

/* * ----------
 * afterTriggerFreeEventList()
 *
 *	释放给定列表中所有事件的存储。
 * ----------
 */
static void
afterTriggerFreeEventList(AfterTriggerEventList *events)
{
	AfterTriggerEventChunk *chunk;

	while ((chunk = events->head) != NULL)
	{
		events->head = chunk->next;
		pfree(chunk);
	}
	events->tail = NULL;
	events->tailfree = NULL;
}

/* * ----------
 * afterTriggerRestoreEventList()
 *
 *	将事件列表恢复到其先前的长度，删除自其具有
 *	值 old_events 以来添加的所有事件。
 * ----------
 */
static void
afterTriggerRestoreEventList(AfterTriggerEventList *events,
							 const AfterTriggerEventList *old_events)
{
	AfterTriggerEventChunk *chunk;
	AfterTriggerEventChunk *next_chunk;

	if (old_events->tail == NULL)
	{
		/* 恢复到完全空状态，因此释放所有内容 */
		afterTriggerFreeEventList(events);
	}
	else
	{
		*events = *old_events;
		/* 释放我们要保留的最后一个块之后的所有块 */
		for (chunk = events->tail->next; chunk != NULL; chunk = next_chunk)
		{
			next_chunk = chunk->next;
			pfree(chunk);
		}
		/* 并清理尾部块为正确的长度 */
		events->tail->next = NULL;
		events->tail->freeptr = events->tailfree;

		/*
		 * 我们不费心移除现在未使用的共享数据记录。      
		 * 它们可能仍然有用。        
		 */
	}
}

/* * ----------
 * afterTriggerDeleteHeadEventChunk()
 *
 *	从查询级别的事件列表中删除第一个事件块。
 *	保持查询级别数据结构中其他事件列表指针
 *	同步。
 * ----------
 */
static void
afterTriggerDeleteHeadEventChunk(AfterTriggersQueryData *qs)
{
	AfterTriggerEventChunk *target = qs->events.head;
	ListCell   *lc;

	Assert(target && target->next);

	/*
	 * 首先，更新每个表数据中的任何指针，以免它们        
	 * 悬空。将过时的指针重置为 NULL 将使         
	 * cancel_prior_stmt_triggers 从列表头开始，这是可以的。 
	 */
	foreach(lc, qs->tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		if (table->after_trig_done &&
			table->after_trig_events.tail == target)
		{
			table->after_trig_events.head = NULL;
			table->after_trig_events.tail = NULL;
			table->after_trig_events.tailfree = NULL;
		}
	}

	/* 现在我们可以刷新头部块 */
	qs->events.head = target->next;
	pfree(target);
}


/* ----------
 * AfterTriggerExecute()
 *
 *	从堆中取回所需元组，并触发单个触发器   
 *	函数。               
 *
 *	通常，同一关系的触发器会连续触发多次。因      
 *	此我们在调用者层级缓存打开的关系，并提供         
 *	 fmgr 查找缓存空间。（对于在查询结束时触发的     
 *	触发器，我们甚至可以搭执行器状态的便车。）    
 *
 *	当为分区表的跨分区更新触发时，旧元组用              
 *	 'src_relInfo'（源叶子分区）取回，而新元组用         
 *	 'dst_relInfo'（目标叶子分区）取回，不过两者都会         
 *	在传递给触发器函数之前被转换成根分区表的格           
 *	式。                  
 *
 *	event: 当前正触发的事件。 
 *	relInfo: 事件的结果关系。  
 *	src_relInfo: 跨分区更新的源分区                 
 *	dst_relInfo: 其目标分区          
 *	trigdesc: rel 的触发器信息副本。    
 *	finfo: fmgr 查找缓存条目数组（trigdesc 中每个触发         
 *	instr: EXPLAIN ANALYZE 插桩节点数组（每触发                   
 *		器一个），无插桩时为 NULL。  
 *	per_tuple_context: 调用触发器函数的内存上下文    
 *	trig_tuple_slot1: tg_trigtuple 的暂存槽（仅外部表）        
 *	trig_tuple_slot2: tg_newtuple 的暂存槽（仅外部表）        
 * ----------
 */
static void
AfterTriggerExecute(EState *estate,
					AfterTriggerEvent event,
					ResultRelInfo *relInfo,
					ResultRelInfo *src_relInfo,
					ResultRelInfo *dst_relInfo,
					TriggerDesc *trigdesc,
					FmgrInfo *finfo, Instrumentation *instr,
					MemoryContext per_tuple_context,
					TupleTableSlot *trig_tuple_slot1,
					TupleTableSlot *trig_tuple_slot2)
{
	Relation	rel = relInfo->ri_RelationDesc;
	Relation	src_rel = src_relInfo->ri_RelationDesc;
	Relation	dst_rel = dst_relInfo->ri_RelationDesc;
	AfterTriggerShared evtshared = GetTriggerSharedData(event);
	Oid			tgoid = evtshared->ats_tgoid;
	TriggerData LocTriggerData = {0};
	HeapTuple	rettuple;
	int			tgindx;
	bool		should_free_trig = false;
	bool		should_free_new = false;

	/*
	 * 在 trigdesc 中定位触发器。它可能不存在，事实上     
	 * 如果触发器在事件入队后已被删除，则 trigdesc          
	 * 可能为 NULL。此时静默不做事。  
	 */
	if (trigdesc == NULL)
		return;
	for (tgindx = 0; tgindx < trigdesc->numtriggers; tgindx++)
	{
		if (trigdesc->triggers[tgindx].tgoid == tgoid)
		{
			LocTriggerData.tg_trigger = &(trigdesc->triggers[tgindx]);
			break;
		}
	}
	if (LocTriggerData.tg_trigger == NULL)
		return;

	/*
	 * 如果正在执行 EXPLAIN ANALYZE，开始为此触发器计时。   
	 * 重新获取元组的时间也要计入触发器成本。    
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * 获取所需的元组。    
	 */
	switch (event->ate_flags & AFTER_TRIGGER_TUP_BITS)
	{
		case AFTER_TRIGGER_FDW_FETCH:
			{
				Tuplestorestate *fdw_tuplestore = GetCurrentFDWTuplestore();

				if (!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot1))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
					TRIGGER_EVENT_UPDATE &&
					!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot2))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");
			}
			/* fall through */
		case AFTER_TRIGGER_FDW_REUSE:

			/*
			 * 将元组存入槽位，使 tg_trigtuple 不引用元组存储 
			 * 内存。（形式上，触发器函数有可能            
			 * 向同一元组存储添加事件，把其他元组  
			 * 挤出内存。）这种区分只是理论上的，因为   
			 * 槽位中一开始放入的就是最小元组，它作为     
			 * 堆元组形式存放，并在不同的内存上下文         
			 * 中构造。                
			 */
			LocTriggerData.tg_trigslot = trig_tuple_slot1;
			LocTriggerData.tg_trigtuple =
				ExecFetchSlotHeapTuple(trig_tuple_slot1, true, &should_free_trig);

			if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
				TRIGGER_EVENT_UPDATE)
			{
				LocTriggerData.tg_newslot = trig_tuple_slot2;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(trig_tuple_slot2, true, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
			break;

		default:
			if (ItemPointerIsValid(&(event->ate_ctid1)))
			{
				TupleTableSlot *src_slot = ExecGetTriggerOldSlot(estate,
																 src_relInfo);

				if (!table_tuple_fetch_row_version(src_rel,
												   &(event->ate_ctid1),
												   SnapshotAny,
												   src_slot))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				/*
				 * 将从源分区获取的元组存储到目标             
				 * （根分区）表槽位中，必要时进行转换。     
				 */
				if (src_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(src_relInfo);

					LocTriggerData.tg_trigslot = ExecGetTriggerOldSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  src_slot,
											  LocTriggerData.tg_trigslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_trigslot, src_slot);
				}
				else
					LocTriggerData.tg_trigslot = src_slot;
				LocTriggerData.tg_trigtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_trigslot, false, &should_free_trig);
			}
			else
			{
				LocTriggerData.tg_trigtuple = NULL;
			}

			/* 如果不存在 ctid2，不要触碰它 */
			if (((event->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ||
				 (event->ate_flags & AFTER_TRIGGER_CP_UPDATE)) &&
				ItemPointerIsValid(&(event->ate_ctid2)))
			{
				TupleTableSlot *dst_slot = ExecGetTriggerNewSlot(estate,
																 dst_relInfo);

				if (!table_tuple_fetch_row_version(dst_rel,
												   &(event->ate_ctid2),
												   SnapshotAny,
												   dst_slot))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");

				/*
				 * 把从目标分区取得的元组存入目标（根分     
				 * 区）表的槽位中，必要时进行转换。       
				 *        
				 */
				if (dst_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(dst_relInfo);

					LocTriggerData.tg_newslot = ExecGetTriggerNewSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  dst_slot,
											  LocTriggerData.tg_newslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_newslot, dst_slot);
				}
				else
					LocTriggerData.tg_newslot = dst_slot;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_newslot, false, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
	}

	/*
	 * 设置元组存储信息，让触发器可以访问                
	 * 转换表。当我们首次使转换表对某个触发器可用时， 
	 * 将其标记为"已关闭"，使其不能再更改。如果任何    
	 * 相同类型的额外事件在当前触发器队列中排队，     
	 * 排队器将负责打开新的转换表集合。   
	 */
	LocTriggerData.tg_oldtable = LocTriggerData.tg_newtable = NULL;
	if (evtshared->ats_table)
	{
		if (LocTriggerData.tg_trigger->tgoldtable)
		{
			if (TRIGGER_FIRED_BY_UPDATE(evtshared->ats_event))
				LocTriggerData.tg_oldtable = evtshared->ats_table->old_upd_tuplestore;
			else
				LocTriggerData.tg_oldtable = evtshared->ats_table->old_del_tuplestore;
			evtshared->ats_table->closed = true;
		}

		if (LocTriggerData.tg_trigger->tgnewtable)
		{
			if (TRIGGER_FIRED_BY_INSERT(evtshared->ats_event))
				LocTriggerData.tg_newtable = evtshared->ats_table->new_ins_tuplestore;
			else
				LocTriggerData.tg_newtable = evtshared->ats_table->new_upd_tuplestore;
			evtshared->ats_table->closed = true;
		}
	}

	/*
	 * 设置剩余的触发器信息         
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event =
		evtshared->ats_event & (TRIGGER_EVENT_OPMASK | TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;
	if (TRIGGER_FOR_UPDATE(LocTriggerData.tg_trigger->tgtype))
		LocTriggerData.tg_updatedcols = evtshared->ats_modifiedcols;

	MemoryContextReset(per_tuple_context);

	/*
	 * 调用触发器并丢弃任何可能返回的更新元组。        
	 * （勿让ExecCallTriggerFunc测EXPLAIN时间。）   
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   tgindx,
								   finfo,
								   NULL,
								   per_tuple_context);
	if (rettuple != NULL &&
		rettuple != LocTriggerData.tg_trigtuple &&
		rettuple != LocTriggerData.tg_newtuple)
		heap_freetuple(rettuple);

	/*
	 * 释放资源     
	 */
	if (should_free_trig)
		heap_freetuple(LocTriggerData.tg_trigtuple);
	if (should_free_new)
		heap_freetuple(LocTriggerData.tg_newtuple);

	/* 如果是外表，不要清除槽位内容 */
	if (trig_tuple_slot1 == NULL)
	{
		if (LocTriggerData.tg_trigslot)
			ExecClearTuple(LocTriggerData.tg_trigslot);
		if (LocTriggerData.tg_newslot)
			ExecClearTuple(LocTriggerData.tg_newslot);
	}

	/*
	 * 如果正在执行 EXPLAIN ANALYZE，停止为此触发器计时，并 
	 * "返回元组"计一次（实为触发次数）。  
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);
}


/*
 * afterTriggerMarkEvents()
 *
 *	扫描给定事件列表中尚未被调用的事件，并标记     
 *	那些现在能用当前触发ID调用的事件。 
 *
 *	如果 move_list 不为 NULL，那些现在不调用的事件将  
 *	被转移到 move_list。
 *
 *	当 immediate_only 为真时，不调用现被延迟的触发器。    
 *	（只有在主事务退出时它才为假。）   
 *
 *	若找到任何可调用事件则返回真。   
 */
static bool
afterTriggerMarkEvents(AfterTriggerEventList *events,
					   AfterTriggerEventList *move_list,
					   bool immediate_only)
{
	bool		found = false;
	bool		deferred_found = false;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;

	for_each_event_chunk(event, chunk, *events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);
		bool		defer_it = false;

		if (!(event->ate_flags &
			  (AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS)))
		{
			/*
			 * 此触发器尚未被调用或调度。检查现在是否    
			 * 该调用它。    
			 */
			if (immediate_only && afterTriggerCheckState(evtshared))
			{
				defer_it = true;
			}
			else
			{
				/*
				 * 将其标记为在本触发周期触发。  
				 */
				evtshared->ats_firing_id = afterTriggers.firing_counter;
				event->ate_flags |= AFTER_TRIGGER_IN_PROGRESS;
				found = true;
			}
		}

		/*
		 * 若是延迟的，按请求将其移入 move_list。 
		 */
		if (defer_it && move_list != NULL)
		{
			deferred_found = true;
			/* 将其添加到 move_list */
			afterTriggerAddEvent(move_list, event, evtshared);
			/* 将原始副本标记为"已完成"，以免再次处理它 */
			event->ate_flags |= AFTER_TRIGGER_DONE;
		}
	}

	/*
	 * 我们本可允许延迟触发器，只要在安全受限 
	 * 操作结束之前验证一条 SET CONSTRAINTS ... IMMEDIATE 已       
	 * 触发了所有这类触发器。目前暂不费此心。         
	 */
	if (deferred_found && InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot fire deferred trigger within security-restricted operation")));

	return found;
}

/*
 * afterTriggerInvokeEvents()
 *
 *	扫描给定事件列表，找出标记为在当前触发周期    
 *	中触发的事件，并触发它们。    
 *
 *	如果 estate 不为 NULL，我们使用其结果关系信息来       
 *	避免反复打开和关闭触发器目标关系。如果为        
 *	NULL，就在本地创建缓存，以免同一关系有多个        
 *	事件。      
 *
 *	当 delete_ok 为 true 时，删除已完全处理的事件是     
 *	安全的。（对此我们并不苛求：只要某个块的所有    
 *	事件都已触发，我们就简单地把该块重置为空。这       
 *	里的目的只是避免在触发器于事务结束时排队新事      
 *	件时无用地重复扫描事件，仅有部分事件被         
 *	触发也无妨）     
 *
 *	如果列表中不再有未触发的事件则返回 true，这样   
 *	以免重复 afterTriggerMarkEvents）。  
 */
static bool
afterTriggerInvokeEvents(AfterTriggerEventList *events,
						 CommandId firing_id,
						 EState *estate,
						 bool delete_ok)
{
	bool		all_fired = true;
	AfterTriggerEventChunk *chunk;
	MemoryContext per_tuple_context;
	bool		local_estate = false;
	ResultRelInfo *rInfo = NULL;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;
	Instrumentation *instr = NULL;
	TupleTableSlot *slot1 = NULL,
			   *slot2 = NULL;

	/* 如果需要，创建本地 EState */
	if (estate == NULL)
	{
		estate = CreateExecutorState();
		local_estate = true;
	}

	/* 为触发器函数调用创建每个元组的内存上下文 */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerTupleContext",
							  ALLOCSET_DEFAULT_SIZES);

	for_each_chunk(chunk, *events)
	{
		AfterTriggerEvent event;
		bool		all_fired_in_chunk = true;

		for_each_event(event, chunk)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			/*
			 * 是我要触发的吗？ 
			 */
			if ((event->ate_flags & AFTER_TRIGGER_IN_PROGRESS) &&
				evtshared->ats_firing_id == firing_id)
			{
				ResultRelInfo *src_rInfo,
						   *dst_rInfo;

				/*
				 * 那就触发它……但首先，若本次与之前不     
				 * 是同一关系，先找正确关系。 
				 */
				if (rel == NULL || RelationGetRelid(rel) != evtshared->ats_relid)
				{
					rInfo = ExecGetTriggerResultRel(estate, evtshared->ats_relid,
													NULL);
					rel = rInfo->ri_RelationDesc;
					/* 捕获 relcache 引用计数不足的调用 */
					Assert(!RelationHasReferenceCountZero(rel));
					trigdesc = rInfo->ri_TrigDesc;
					/* 注意：trigdesc 可能在此处为 NULL */
					finfo = rInfo->ri_TrigFunctions;
					instr = rInfo->ri_TrigInstrument;
					if (slot1 != NULL)
					{
						ExecDropSingleTupleTableSlot(slot1);
						ExecDropSingleTupleTableSlot(slot2);
						slot1 = slot2 = NULL;
					}
					if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
					{
						slot1 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
						slot2 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
					}
				}

				/*
				 * 查找跨分区更新事件的源和目标               
				 * 分区结果关系。        
				 */
				if ((event->ate_flags & AFTER_TRIGGER_TUP_BITS) ==
					AFTER_TRIGGER_CP_UPDATE)
				{
					Assert(OidIsValid(event->ate_src_part) &&
						   OidIsValid(event->ate_dst_part));
					src_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_src_part,
														rInfo);
					dst_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_dst_part,
														rInfo);
				}
				else
					src_rInfo = dst_rInfo = rInfo;

				/*
				 * 触发它。注意 AFTER_TRIGGER_IN_PROGRESS 标志      
				 * 仍被置位，因此递归检查事件列表时不会
				 * 试图重新触发它。
				 */
				AfterTriggerExecute(estate, event, rInfo,
									src_rInfo, dst_rInfo,
									trigdesc, finfo, instr,
									per_tuple_context, slot1, slot2);

				/*
				 * 标记事件完成。  
				 */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			else if (!(event->ate_flags & AFTER_TRIGGER_DONE))
			{
				/* 还有事情需要做 */
				all_fired = all_fired_in_chunk = false;
			}
		}

		/* 如果 delete_ok 且没有感兴趣的内容，清除块 */
		if (delete_ok && all_fired_in_chunk)
		{
			chunk->freeptr = CHUNK_DATA_START(chunk);
			chunk->endfree = chunk->endptr;

			/*
			 * 如果是最后一个块，还必须同步事件列表的     
			 * tailfree。注意，如果可能还有                     
			 * 别的 AfterTriggerEventList 值指向此列表，则不得把
			 * delete_ok 传为真：tailfree 副本无法修复。  
			 */
			if (chunk == events->tail)
				events->tailfree = chunk->freeptr;
		}
	}
	if (slot1 != NULL)
	{
		ExecDropSingleTupleTableSlot(slot1);
		ExecDropSingleTupleTableSlot(slot2);
	}

	/* 释放工作资源 */
	MemoryContextDelete(per_tuple_context);

	if (local_estate)
	{
		ExecCloseResultRelations(estate);
		ExecResetTupleTable(estate->es_tupleTable, false);
		FreeExecutorState(estate);
	}

	return all_fired;
}


/*
 * GetAfterTriggersTableData
 *
 * 为指定的触发器事件（关系 + 操作类型）查找或  
 * 创建一个 AfterTriggersTableData 结构。忽略标记为       
 * "closed" 的现有结构；我们不想向它们添加更多          
 * 元组或改其 stmt 已触发状态。    
 *
 * 注意：AfterTriggersTableData 列表是在当前               
 * （子）事务的 CurTransactionContext 中，这没问题  
 * 因为它无需存活过 AfterTriggerEndQuery。   
 */
static AfterTriggersTableData *
GetAfterTriggersTableData(Oid relid, CmdType cmdType)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs;
	MemoryContext oldcxt;
	ListCell   *lc;

	/* 调用者应确保 query_depth 没问题。 */
	Assert(afterTriggers.query_depth >= 0 &&
		   afterTriggers.query_depth < afterTriggers.maxquerydepth);
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	foreach(lc, qs->tables)
	{
		table = (AfterTriggersTableData *) lfirst(lc);
		if (table->relid == relid && table->cmdType == cmdType &&
			!table->closed)
			return table;
	}

	oldcxt = MemoryContextSwitchTo(CurTransactionContext);

	table = (AfterTriggersTableData *) palloc0(sizeof(AfterTriggersTableData));
	table->relid = relid;
	table->cmdType = cmdType;
	qs->tables = lappend(qs->tables, table);

	MemoryContextSwitchTo(oldcxt);

	return table;
}

/*
 * 返回一个适合保存要放入 AfterTriggersTableData          
 * 转换表元组存储中的元组的 TupleTableSlot。     
 */
static TupleTableSlot *
GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
						  TupleDesc tupdesc)
{
	/* 如果尚未创建，则创建它。 */
	if (!table->storeslot)
	{
		MemoryContext oldcxt;

		/*
		 * 我们只需此槽位到 AfterTriggerEndQuery，但存活到    
		 * 子事务结束也足够了。它将由                    
		 * AfterTriggerFreeQuery() 释放。不过，传入的 tupdesc 可能 
		 * 有不同的生命周期，因此最好复制一份。   
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		tupdesc = CreateTupleDescCopy(tupdesc);
		table->storeslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		MemoryContextSwitchTo(oldcxt);
	}

	return table->storeslot;
}

/*
 * MakeTransitionCaptureState
 *
 * 为给定的 TriggerDesc、目标关系和操作类型创建          
 * TransitionCaptureState 对象。TCS 对象保存决定是否            
 * 在转换表中捕获元组所需的所有状态。      
 *
 * 如果 'trigdesc' 中没有触发器要求相关的转换表，        
 * 则返回 NULL。        
 *
 * 结果对象可以传递给 ExecAR* 函数。在处理子表时，
 * 调用者可以设置 tcs_original_insert_tuple，以避免            
 * 以根表的格式重建原始元组。                              
 *        
 *
 * 注意，我们把父表的标志复制到此结构中（而不是      
 * 随后直接使用关系的 TriggerDesc），这样我们就可以         
 * 用它来控制从子表收集转换元组。                       
 *
 * 按照 SQL 规范，同一表上同类操作（INSERT/UPDATE/       
 * DELETE）在同一次查询中应共享一个转换表。            
 * 因此，Tuplestore 由 AfterTriggersTableData 结构                   
 * 拥有，通过表 OID + CmdType 查找，并且只被我们         
 * 交付给调用者的 TransitionCaptureState 引用。    
 */
TransitionCaptureState *
MakeTransitionCaptureState(TriggerDesc *trigdesc, Oid relid, CmdType cmdType)
{
	TransitionCaptureState *state;
	bool		need_old_upd,
				need_new_upd,
				need_old_del,
				need_new_ins;
	AfterTriggersTableData *table;
	MemoryContext oldcxt;
	ResourceOwner saveResourceOwner;

	if (trigdesc == NULL)
		return NULL;

	/* 检测我们需要哪些表。 */
	switch (cmdType)
	{
		case CMD_INSERT:
			need_old_upd = need_old_del = need_new_upd = false;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		case CMD_UPDATE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = need_new_ins = false;
			break;
		case CMD_DELETE:
			need_old_del = trigdesc->trig_delete_old_table;
			need_old_upd = need_new_upd = need_new_ins = false;
			break;
		case CMD_MERGE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = trigdesc->trig_delete_old_table;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		default:
			elog(ERROR, "unexpected CmdType: %d", (int) cmdType);
			/* 让编译器安静 */
			need_old_upd = need_new_upd = need_old_del = need_new_ins = false;
			break;
	}
	if (!need_old_upd && !need_new_upd && !need_new_ins && !need_old_del)
		return NULL;

	/* 检查状态，类似于 AfterTriggerSaveEvent。 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "MakeTransitionCaptureState() called outside of query");

	/* 确保我们有足够的空间在此查询深度记录事件。 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * 查找或创建 AfterTriggersTableData 结构来保存     
	 * 元组存储。若有匹配的结构但标记为已关闭，        
	 * 则忽略它，需要新的。 
	 *
	 * 注意：AfterTriggersTableData 列表以及元组存储都是        
	 * 在当前（子）事务的 CurTransactionContext 中分配的，     
	 * 而元组存储由（子）事务的资源所有者管理。         
	 * 这样的生命周期就足够了，因为我们不允许使      
	 * 用转换表的触发器为可延迟的；它们将在       
	 * AfterTriggerEndQuery 期间触发，之后即可删除数据。
	 */
	table = GetAfterTriggersTableData(relid, cmdType);

	/* 现在创建所需的元组存储（如果还没有的话）。 */
	oldcxt = MemoryContextSwitchTo(CurTransactionContext);
	saveResourceOwner = CurrentResourceOwner;
	CurrentResourceOwner = CurTransactionResourceOwner;

	if (need_old_upd && table->old_upd_tuplestore == NULL)
		table->old_upd_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_upd && table->new_upd_tuplestore == NULL)
		table->new_upd_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_old_del && table->old_del_tuplestore == NULL)
		table->old_del_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_ins && table->new_ins_tuplestore == NULL)
		table->new_ins_tuplestore = tuplestore_begin_heap(false, false, work_mem);

	CurrentResourceOwner = saveResourceOwner;
	MemoryContextSwitchTo(oldcxt);

	/* 现在在调用者的上下文中构建 TransitionCaptureState 结构 */
	state = (TransitionCaptureState *) palloc0(sizeof(TransitionCaptureState));
	state->tcs_delete_old_table = need_old_del;
	state->tcs_update_old_table = need_old_upd;
	state->tcs_update_new_table = need_new_upd;
	state->tcs_insert_new_table = need_new_ins;
	state->tcs_private = table;

	return state;
}


/* * ----------
 * AfterTriggerBeginXact()
 *
 *	在事务开始时调用（BEGIN 或事务块外
 *	单个语句的隐式开始）。
 * ----------
 */
void
AfterTriggerBeginXact(void)
{
	/*
	 * 将 after-trigger 状态结构初始化为空    
	 */
	afterTriggers.firing_counter = (CommandId) 1;	/* mustn't be 0 */
	afterTriggers.query_depth = -1;

	/*
	 * 验证没有剩余状态。如果这些断言触发，                
	 * 意味着 AfterTriggerEndXact 未被调用或未正确               
	 * 清理。   
	 */
	Assert(afterTriggers.state == NULL);
	Assert(afterTriggers.query_stack == NULL);
	Assert(afterTriggers.maxquerydepth == 0);
	Assert(afterTriggers.event_cxt == NULL);
	Assert(afterTriggers.events.head == NULL);
	Assert(afterTriggers.trans_stack == NULL);
	Assert(afterTriggers.maxtransdepth == 0);
}


/* * ----------
 * AfterTriggerBeginQuery()
 *
 *	在开始处理事务（或子事务）中的单个查询
 *	之前调用。大部分实际工作被推迟到
 *	实际有人尝试排队触发器事件时。
 * ----------
 */
void
AfterTriggerBeginQuery(void)
{
	/* 增加查询堆栈深度 */
	afterTriggers.query_depth++;
}


/* * ----------
 * AfterTriggerEndQuery()
 *
 *	在一个查询完全处理完毕后调用。此时
 *	我们调用由该查询排队的所有 AFTER IMMEDIATE 触发器事件，
 *	并将延迟的触发器事件转移到全局延迟触发器列表。
 *
 *	注意：这必须在完成触发器执行后
 *	关闭打开的查询之前调用。
 * ----------
 *
 */
void
AfterTriggerEndQuery(EState *estate)
{
	AfterTriggersQueryData *qs;

	/* 也必须在查询内部 */
	Assert(afterTriggers.query_depth >= 0);

	/*
	 * 如果我们甚至尚未初始化事件堆栈，那               
	 * 么肯定不会有事件，快速退出。     
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
	{
		afterTriggers.query_depth--;
		return;
	}

	/*
	 * 处理由该查询排队的所有立即模式触发器，并把延   
	 * 迟的触发器移到主延迟事件列表中。  
	 *
	 * 注意，我们在实际触发任何东西之前就决定哪些     
	 * 将被触发，并把延迟的触发器放到主列表上。这        
	 * 确保了当触发器函数执行 SET CONSTRAINTS ...                  
	 *  IMMEDIATE 时行为合理：已决定延迟的所有事件都已      
	 * 就绪  
	 *
	 * 我们会循环，以防触发器在同一查询级别排队更      
	 * 多事件。包括所有 PL/pgSQL 触发器函数在内的普通     
	 * 触发器函数会改为在专用的查询级别中触发触发器。  
	 * 外键强制触发器确实会添加到当前查询级别，因为     
	 * 它们向 SPI_execute_snapshot() 传递了 fire_triggers       
	 *  = false。其他 C 触发器亦然。
	 *
	 * 若没有可触发的事件，我们无需递增        
	 *  firing_counter
	 */
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	for (;;)
	{
		if (afterTriggerMarkEvents(&qs->events, &afterTriggers.events, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;
			AfterTriggerEventChunk *oldtail = qs->events.tail;

			if (afterTriggerInvokeEvents(&qs->events, firing_id, estate, false))
				break;			/* all fired */

			/*
			 * 触发一个触发器可能导致 query_stack 被 repalloc， 
			 * 因此每次调用 afterTriggerInvokeEvents 后都必须重  
			 * 新计算 qs。此外，此处传 delete_ok = true 是不安  
			 * 全的，因为这会让 afterTriggerInvokeEvents 试图   
			 * 在堆栈被 repalloc 之后访问 qs->events。      
			 */
			qs = &afterTriggers.query_stack[afterTriggers.query_depth];

			/*
			 * 我们需要再次扫描事件列表。为降低开销，    
			 * 要清除完全触发过的块。我们知道，在           
			 * afterTriggerMarkEvents 结束时，所有事件都已被标    
			 * 记为 IN_PROGRESS 或 DONE，故任何仍需关注的      
			 * 事件必定是在那之后添加的，因此必定位于   
			 * 当时的尾部块或更后面的块里。所以要清除
			 * oldtail 之前的所有块。这与传 delete_ok = true 所  
			 * 要清除的事件集合大体相同。                        
			 */
			Assert(oldtail != NULL);
			while (qs->events.head != oldtail)
				afterTriggerDeleteHeadEventChunk(qs);
		}
		else
			break;
	}

	/* 释放查询级别的本地存储，包括元组存储（如果有） */
	AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);

	afterTriggers.query_depth--;
}


/*
 * AfterTriggerFreeQuery
 *	释放触发器查询层级的附属存储。        
 *	这包括关闭元组存储。         
 *	注意：重要的是本函数若被错误中断后再次为       
 *	同一查询层级调用时仍是安全的。  
 */
static void
AfterTriggerFreeQuery(AfterTriggersQueryData *qs)
{
	Tuplestorestate *ts;
	List	   *tables;
	ListCell   *lc;

	/* 丢弃触发器事件 */
	afterTriggerFreeEventList(&qs->events);

	/* 丢弃 FDW 元组存储（如果有） */
	ts = qs->fdw_tuplestore;
	qs->fdw_tuplestore = NULL;
	if (ts)
		tuplestore_end(ts);

	/* 释放每个表的辅助存储 */
	tables = qs->tables;
	foreach(lc, tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		ts = table->old_upd_tuplestore;
		table->old_upd_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->new_upd_tuplestore;
		table->new_upd_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->old_del_tuplestore;
		table->old_del_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->new_ins_tuplestore;
		table->new_ins_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		if (table->storeslot)
		{
			TupleTableSlot *slot = table->storeslot;

			table->storeslot = NULL;
			ExecDropSingleTupleTableSlot(slot);
		}
	}

	/*
	 * 现在释放 AfterTriggersTableData 结构和列表单元。先重置  
	 * 列表指针；若 list_free_deep 以某种方式出错，宁可泄漏
	 * 存储，也不要陷入无限循环。 
	 */
	qs->tables = NIL;
	list_free_deep(tables);
}


/* * ----------
 * AfterTriggerFireDeferred()
 *
 *	在当前事务提交之前调用。此时
 *	我们调用所有待处理的 DEFERRED 触发器。
 *
 *	其他模块可能在预提交处理期间排队额外的延迟触发器；
 *	因此 xact.c 可能需要多次调用此函数。
 * ----------
 *
 */
void
AfterTriggerFireDeferred(void)
{
	AfterTriggerEventList *events;
	bool		snap_pushed = false;

	/* 不得在查询内部 */
	Assert(afterTriggers.query_depth == -1);

	/*
	 * 如果有任何要触发的触发器，确保已设置它们要用的  
	 * 快照。（由于 PortalRunUtility 不为 COMMIT 设置快照，      
	 * 不能假定进入时ActiveSnapshot有效。）
	 */
	events = &afterTriggers.events;
	if (events->head != NULL)
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snap_pushed = true;
	}

	/*
	 * 运行所有剩余的触发器。循环直到它们全部结束，    
	 * 以防触发器又排队了更多。  
	 */
	while (afterTriggerMarkEvents(events, NULL, false))
	{
		CommandId	firing_id = afterTriggers.firing_counter++;

		if (afterTriggerInvokeEvents(events, firing_id, NULL, true))
			break;				/* all fired */
	}

	/*
	 * 我们不必费心释放事件列表，因为它反正会在        
	 * AfterTriggerEndXact 中消失（比 pfree 更高效）。    
	 */

	if (snap_pushed)
		PopActiveSnapshot();
}


/* * ----------
 * AfterTriggerEndXact()
 *
 *	当前事务正在结束。
 *
 *	任何未触发的触发器被取消，因此我们简单地
 *	丢弃我们知道的任何东西。
 *
 *	注意：在事务中止期间，如果发生错误，
 *	这可能被重复调用；因此，如果找到
 *	未触发的触发器，不要抱怨。
 * ----------
 */
void
AfterTriggerEndXact(bool isCommit)
{
	/*
	 * 忘记待处理事件列表。 
	 *
	 * 由于所有信息都在 TopTransactionContext 或其子上下文     
	 * 中，其实并不需要做任何事来回收内存。不过，  
	 * 待处理事件列表可能很大，所以尽快将其丢弃是        
	 * 有益的——尤其是当我们因列表内存耗尽而            
	 * 中止时！           
	 */
	if (afterTriggers.event_cxt)
	{
		MemoryContextDelete(afterTriggers.event_cxt);
		afterTriggers.event_cxt = NULL;
		afterTriggers.events.head = NULL;
		afterTriggers.events.tail = NULL;
		afterTriggers.events.tailfree = NULL;
	}

	/*
	 * 也忘记子事务状态。由于这不可能很大，            
	 * 我们让 TopTransactionContext 最终重置时释放内存，    
	 * 而不是在这里做。        
	 */
	afterTriggers.trans_stack = NULL;
	afterTriggers.maxtransdepth = 0;


	/*
	 * 忘记查询堆栈和约束相关的状态信息。与子事务     
	 * 状态信息一样，我们不费心在这里释放                   
	 * 内存。   
	 */
	afterTriggers.query_stack = NULL;
	afterTriggers.maxquerydepth = 0;
	afterTriggers.state = NULL;

	/* 在下一次事务开始之前不再进行 afterTriggers 操作。 */
	afterTriggers.query_depth = -1;
}

/*
 * AfterTriggerBeginSubXact()
 *
 *	启动子事务。     
 */
void
AfterTriggerBeginSubXact(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * 如有需要，在 trans_stack 中分配更多空间。（注意，  
	 * 由于子事务的最小嵌套级别为 2，我们会浪费数组     
	 * 的前几个条目；不值得费记法功夫去避免它。       
	 */
	while (my_level >= afterTriggers.maxtransdepth)
	{
		if (afterTriggers.maxtransdepth == 0)
		{
			/* 任意初始化为最多 8 个子事务级别 */
			afterTriggers.trans_stack = (AfterTriggersTransData *)
				MemoryContextAlloc(TopTransactionContext,
								   8 * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = 8;
		}
		else
		{
			/* repalloc 将保持堆栈在同一上下文中 */
			int			new_alloc = afterTriggers.maxtransdepth * 2;

			afterTriggers.trans_stack = (AfterTriggersTransData *)
				repalloc(afterTriggers.trans_stack,
						 new_alloc * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = new_alloc;
		}
	}

	/*
	 * 将当前信息推入堆栈。SET CONSTRAINTS 状态                   
	 * 直到/除非更改才保存。同样，我们直到需要   
	 * 才创建每个子事务的事件上下文。 
	 */
	afterTriggers.trans_stack[my_level].state = NULL;
	afterTriggers.trans_stack[my_level].events = afterTriggers.events;
	afterTriggers.trans_stack[my_level].query_depth = afterTriggers.query_depth;
	afterTriggers.trans_stack[my_level].firing_counter = afterTriggers.firing_counter;
}

/*
 * AfterTriggerEndSubXact()
 *
 *	当前子事务正在结束。       
 */
void
AfterTriggerEndSubXact(bool isCommit)
{
	int			my_level = GetCurrentTransactionNestLevel();
	SetConstraintState state;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	CommandId	subxact_firing_id;

	/*
	 * 必要时弹出先前状态。
	 */
	if (isCommit)
	{
		Assert(my_level < afterTriggers.maxtransdepth);
		/* 如果我们保存了先前状态，不再需要它 */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
			pfree(state);
		/* 这避免了稍后错误时的双重 pfree： */
		afterTriggers.trans_stack[my_level].state = NULL;
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);
	}
	else
	{
		/*
		 * 正在中止。子事务的启动有可能在调用          
		 * AfterTriggerBeginSubXact 之前失败，此时绝不能去碰   
		 * 并不存在的 trans_stack 级别。
		 */
		if (my_level >= afterTriggers.maxtransdepth)
			return;

		/*
		 * 释放被中止查询的查询层级存储，并将 query_depth   
		 * 恢复为子事务前的值。这假设子事务不会向 
		 * 在更早事务状态中启动的查询层级添加            
		 * 事件。                 
		 */
		while (afterTriggers.query_depth > afterTriggers.trans_stack[my_level].query_depth)
		{
			if (afterTriggers.query_depth < afterTriggers.maxquerydepth)
				AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);
			afterTriggers.query_depth--;
		}
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);

		/*
		 * 将全局延迟事件列表恢复到其先前的长度，   
		 * 丢弃子事务排队的任何事件。     
		 */
		afterTriggerRestoreEventList(&afterTriggers.events,
									 &afterTriggers.trans_stack[my_level].events);

		/*
		 * 恢复触发器状态。如果保存的状态为 NULL，则此   
		 * 子事务未保存它，因此不需要恢复。     
		 */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
		{
			pfree(afterTriggers.state);
			afterTriggers.state = state;
		}
		/* 这避免了稍后错误时的双重 pfree： */
		afterTriggers.trans_stack[my_level].state = NULL;

		/*
		 * 扫描那些被本子事务或其子子事务标记为 DONE 或   
		 * IN PROGRESS 的其余延迟事件，并取消其标记。    
		 * 我们可以这样识别：它们的触发 ID 大于或等于      
		 * 我们在子事务开始时保存的 firing_counter 值。（这   
		 * 实质上假设当前子事务包含其后启动的全部子事
		 * 务。）                  
		 */
		subxact_firing_id = afterTriggers.trans_stack[my_level].firing_counter;
		for_each_event_chunk(event, chunk, afterTriggers.events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags &
				(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS))
			{
				if (evtshared->ats_firing_id >= subxact_firing_id)
					event->ate_flags &=
						~(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS);
			}
		}
	}
}

/*
 * 获取给定事件的转换表，取决于我们正在处理                
 * 旧元组还是新元组。         
 */
static Tuplestorestate *
GetAfterTriggersTransitionTable(int event,
								TupleTableSlot *oldslot,
								TupleTableSlot *newslot,
								TransitionCaptureState *transition_capture)
{
	Tuplestorestate *tuplestore = NULL;
	bool		delete_old_table = transition_capture->tcs_delete_old_table;
	bool		update_old_table = transition_capture->tcs_update_old_table;
	bool		update_new_table = transition_capture->tcs_update_new_table;
	bool		insert_new_table = transition_capture->tcs_insert_new_table;

	/*
	 * 对于 INSERT 事件，NEW 应为非 NULL；对于 DELETE 事件，   
	 * OLD 应为非 NULL；而对于 UPDATE 事件，通常 OLD 和 NEW   
	 * 都非 NULL。但对为捕获转换元组而在 UPDATE 分区键      
	 * 行移动期间触发的 UPDATE 事件，当事件针对被插入      
	 * 的行时 OLD 为 NULL，而当事件针对被删除的行时 NEW    
	 * 为 NULL。       
	 */
	Assert(!(event == TRIGGER_EVENT_DELETE && delete_old_table &&
			 TupIsNull(oldslot)));
	Assert(!(event == TRIGGER_EVENT_INSERT && insert_new_table &&
			 TupIsNull(newslot)));

	if (!TupIsNull(oldslot))
	{
		Assert(TupIsNull(newslot));
		if (event == TRIGGER_EVENT_DELETE && delete_old_table)
			tuplestore = transition_capture->tcs_private->old_del_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_old_table)
			tuplestore = transition_capture->tcs_private->old_upd_tuplestore;
	}
	else if (!TupIsNull(newslot))
	{
		Assert(TupIsNull(oldslot));
		if (event == TRIGGER_EVENT_INSERT && insert_new_table)
			tuplestore = transition_capture->tcs_private->new_ins_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_new_table)
			tuplestore = transition_capture->tcs_private->new_upd_tuplestore;
	}

	return tuplestore;
}

/*
 * 将给定的堆元组添加到给定的元组存储，必要时施加    
 * 转换映射。  
 *
 * 如果传入 original_insert_tuple，我们能不加转换添加该元组。
 */
static void
TransitionTableAddTuple(EState *estate,
						TransitionCaptureState *transition_capture,
						ResultRelInfo *relinfo,
						TupleTableSlot *slot,
						TupleTableSlot *original_insert_tuple,
						Tuplestorestate *tuplestore)
{
	TupleConversionMap *map;

	/*
	 * 如果没有元组存储，则无需做任何事。    
	 */
	if (tuplestore == NULL)
		return;

	if (original_insert_tuple)
		tuplestore_puttupleslot(tuplestore, original_insert_tuple);
	else if ((map = ExecGetChildToRootMap(relinfo)) != NULL)
	{
		AfterTriggersTableData *table = transition_capture->tcs_private;
		TupleTableSlot *storeslot;

		storeslot = GetAfterTriggersStoreSlot(table, map->outdesc);
		execute_attr_map_slot(map->attrMap, slot, storeslot);
		tuplestore_puttupleslot(tuplestore, storeslot);
	}
	else
		tuplestore_puttupleslot(tuplestore, slot);
}

/* * ----------
 * AfterTriggerEnlargeQueryState()
 *
 *	准备必要的状态，以便我们可以记录由查询排队的
 *	AFTER 触发器事件。允许在（子）事务中嵌套查询，
 *	因此我们需要为每个查询嵌套级别设置单独的状态。
 * ----------
 *
 */
static void
AfterTriggerEnlargeQueryState(void)
{
	int			init_depth = afterTriggers.maxquerydepth;

	Assert(afterTriggers.query_depth >= afterTriggers.maxquerydepth);

	if (afterTriggers.maxquerydepth == 0)
	{
		int			new_alloc = Max(afterTriggers.query_depth + 1, 8);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			MemoryContextAlloc(TopTransactionContext,
							   new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}
	else
	{
		/* repalloc 将保持堆栈在同一上下文中 */
		int			old_alloc = afterTriggers.maxquerydepth;
		int			new_alloc = Max(afterTriggers.query_depth + 1,
									old_alloc * 2);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			repalloc(afterTriggers.query_stack,
					 new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}

	/* 将新数组条目初始化为空 */
	while (init_depth < afterTriggers.maxquerydepth)
	{
		AfterTriggersQueryData *qs = &afterTriggers.query_stack[init_depth];

		qs->events.head = NULL;
		qs->events.tail = NULL;
		qs->events.tailfree = NULL;
		qs->fdw_tuplestore = NULL;
		qs->tables = NIL;

		++init_depth;
	}
}

/*
 * 创建具有 numalloc 个 trigstates 空间的空 SetConstraintState
 */
static SetConstraintState
SetConstraintStateCreate(int numalloc)
{
	SetConstraintState state;

	/* 当 numalloc == 0 时，行为正常 */
	if (numalloc <= 0)
		numalloc = 1;

	/*
	 * 我们假设清零将正确初始化状态值。                  
	 */
	state = (SetConstraintState)
		MemoryContextAllocZero(TopTransactionContext,
							   offsetof(SetConstraintStateData, trigstates) +
							   numalloc * sizeof(SetConstraintTriggerData));

	state->numalloc = numalloc;

	return state;
}

/*
 * 复制 SetConstraintState
 */
static SetConstraintState
SetConstraintStateCopy(SetConstraintState origstate)
{
	SetConstraintState state;

	state = SetConstraintStateCreate(origstate->numstates);

	state->all_isset = origstate->all_isset;
	state->all_isdeferred = origstate->all_isdeferred;
	state->numstates = origstate->numstates;
	memcpy(state->trigstates, origstate->trigstates,
		   origstate->numstates * sizeof(SetConstraintTriggerData));

	return state;
}

/*
 * 向 SetConstraintState 添加一个每触发器条目。返回可能已  
 * 更改的状态对象指针（若必须 repalloc 则会更改）。 
 */
static SetConstraintState
SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred)
{
	if (state->numstates >= state->numalloc)
	{
		int			newalloc = state->numalloc * 2;

		newalloc = Max(newalloc, 8);	/* in case original has size 0 */
		state = (SetConstraintState)
			repalloc(state,
					 offsetof(SetConstraintStateData, trigstates) +
					 newalloc * sizeof(SetConstraintTriggerData));
		state->numalloc = newalloc;
		Assert(state->numstates < state->numalloc);
	}

	state->trigstates[state->numstates].sct_tgoid = tgoid;
	state->trigstates[state->numstates].sct_tgisdeferred = tgisdeferred;
	state->numstates++;

	return state;
}

/* * ----------
 * AfterTriggerSetState()
 *
 *	执行 SET CONSTRAINTS ... 实用命令。
 * ----------
 */
void
AfterTriggerSetState(ConstraintsSetStmt *stmt)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* 如果尚未初始化状态，则初始化。 */
	if (afterTriggers.state == NULL)
		afterTriggers.state = SetConstraintStateCreate(8);

	/*
	 * 如果在子事务中，且我们尚未保存当前状态，         
	 * 则保存它，以便在子事务中止时可以恢复。  
	 */
	if (my_level > 1 &&
		afterTriggers.trans_stack[my_level].state == NULL)
	{
		afterTriggers.trans_stack[my_level].state =
			SetConstraintStateCopy(afterTriggers.state);
	}

	/*
	 * 处理 SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * 忘记此事务中任何先前的 SET CONSTRAINTS 命令。      
		 */
		afterTriggers.state->numstates = 0;

		/*
		 * 将每事务的 ALL 状态设为已知。  
		 */
		afterTriggers.state->all_isset = true;
		afterTriggers.state->all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	conrel;
		Relation	tgrel;
		List	   *conoidlist = NIL;
		List	   *tgoidlist = NIL;
		ListCell   *lc;

		/*
		 * 处理 SET CONSTRAINTS 约束名 [, ...]      
		 *
		 * 首先，识别所有指名的约束并建立其 OID 的列表。 
		 * 由于与 SQL 规范不同，我们允许同一模式内有多个  
		 * 同名约束，所以这些指称不一定唯一。      
		 * 策略是：瞄准首个有匹配的搜索路径模式内  
		 * 所有匹配约束，但无视首个匹配后的其他模   
		 * 式中的匹配。                                               
		 * （这有点怪，但这是历史行为。）        
		 *
		 * 分区表中的约束可能在分区里有对应约束。 
		 * 把这些约束也一并获取。              
		 */
		conrel = table_open(ConstraintRelationId, AccessShareLock);

		foreach(lc, stmt->constraints)
		{
			RangeVar   *constraint = lfirst(lc);
			bool		found;
			List	   *namespacelist;
			ListCell   *nslc;

			if (constraint->catalogname)
			{
				if (strcmp(constraint->catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
									constraint->catalogname, constraint->schemaname,
									constraint->relname)));
			}

			/*
			 * 如果给出了约束的模式名称，只在该             
			 * 模式中查找。如果给出了裸约束名称，使用
			 * 搜索路径查找第一个匹配的约束。     
			 */
			if (constraint->schemaname)
			{
				Oid			namespaceId = LookupExplicitNamespace(constraint->schemaname,
																  false);

				namespacelist = list_make1_oid(namespaceId);
			}
			else
			{
				namespacelist = fetch_search_path(true);
			}

			found = false;
			foreach(nslc, namespacelist)
			{
				Oid			namespaceId = lfirst_oid(nslc);
				SysScanDesc conscan;
				ScanKeyData skey[2];
				HeapTuple	tup;

				ScanKeyInit(&skey[0],
							Anum_pg_constraint_conname,
							BTEqualStrategyNumber, F_NAMEEQ,
							CStringGetDatum(constraint->relname));
				ScanKeyInit(&skey[1],
							Anum_pg_constraint_connamespace,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(namespaceId));

				conscan = systable_beginscan(conrel, ConstraintNameNspIndexId,
											 true, NULL, 2, skey);

				while (HeapTupleIsValid(tup = systable_getnext(conscan)))
				{
					Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

					if (con->condeferrable)
						conoidlist = lappend_oid(conoidlist, con->oid);
					else if (stmt->deferred)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("constraint \"%s\" is not deferrable",
										constraint->relname)));
					found = true;
				}

				systable_endscan(conscan);

				/*
				 * 一旦找到匹配的约束，我们就不搜索       
				 * 搜索路径的后续部分。 
				 */
				if (found)
					break;
			}

			list_free(namespacelist);

			/*
			 * 未找到? 
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" does not exist",
								constraint->relname)));
		}

		/*
		 * 扫描这些约束的任何可能的后代。我们把找到的 
		 * 东西都追加到我们正在扫描的同一个列表中；这样 
		 * 也会为它们建立新的扫描，因此如果还有更深的
		 * 后代，我们也能捕获到。          
		 */
		foreach(lc, conoidlist)
		{
			Oid			parent = lfirst_oid(lc);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key,
						Anum_pg_constraint_conparentid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(parent));

			scan = systable_beginscan(conrel, ConstraintParentIndexId, true, NULL, 1, &key);

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

				conoidlist = lappend_oid(conoidlist, con->oid);
			}

			systable_endscan(scan);
		}

		table_close(conrel, AccessShareLock);

		/*
		 * 现在，定位实现每个约束的触发器，                  
		 * 并列出它们的 OID。     
		 */
		tgrel = table_open(TriggerRelationId, AccessShareLock);

		foreach(lc, conoidlist)
		{
			Oid			conoid = lfirst_oid(lc);
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;

			ScanKeyInit(&skey,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conoid));

			tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
										NULL, 1, &skey);

			while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
			{
				Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);

				/*
				 * 静默跳过在 pg_trigger 中被标记为不可延迟     
				 * 的触发器。这不算错误，因为可延迟 RI 
				 * 约束可能带有一些不可延迟的动作。     
				 *         
				 */
				if (pg_trigger->tgdeferrable)
					tgoidlist = lappend_oid(tgoidlist, pg_trigger->oid);
			}

			systable_endscan(tgscan);
		}

		table_close(tgrel, AccessShareLock);

		/*
		 * 现在我们可以为此次事务设置各触发器的状态。  
		 *      
		 */
		foreach(lc, tgoidlist)
		{
			Oid			tgoid = lfirst_oid(lc);
			SetConstraintState state = afterTriggers.state;
			bool		found = false;
			int			i;

			for (i = 0; i < state->numstates; i++)
			{
				if (state->trigstates[i].sct_tgoid == tgoid)
				{
					state->trigstates[i].sct_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				afterTriggers.state =
					SetConstraintStateAddItem(state, tgoid, stmt->deferred);
			}
		}
	}

	/*
	 * SQL99 要求，当一个约束被设置为 IMMEDIATE 时，任何      
	 * 针对该约束的延迟检查都必须在 SET CONSTRAINTS          
	 * 命令执行时进行——即 SET CONSTRAINTS 命令的              
	 * 效果是追溯适用的。我们已经更新了约束状态，       
	 * 因此扫描先前延迟的事件列表，触发已变为立       
	 * 即的。 
	 *
	 * 显然，若这是 SET ... DEFERRED，它就不可能已把任何   
	 * 未触发的事件转换为立即事件，故此时无需做事。  
	 */
	if (!stmt->deferred)
	{
		AfterTriggerEventList *events = &afterTriggers.events;
		bool		snapshot_set = false;

		while (afterTriggerMarkEvents(events, NULL, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;

			/*
			 * 确保已建立快照，以防触发器函数需要。   
			 * 注意，若没有找到至少一个现在必须触发的    
			 * 触发器，就避免设置快照。这是为了让 BEGIN;  
			 *  SET CONSTRAINTS ...; SET TRANSACTION                      
			 *  ISOLATION LEVEL SERIALIZABLE; ... 能正常运行。        
			 * （若正处于事务开始处，则任何触发器事件都   
			 * 尚不可能入队。） 
			 */
			if (!snapshot_set)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				snapshot_set = true;
			}

			/*
			 * 我们可以在顶层事务时删除已触发的事件，     
			 * 但如果在子事务内部则最好不要，因为      
			 * 子事务可能稍后回滚。             
			 */
			if (afterTriggerInvokeEvents(events, firing_id, NULL,
										 !IsSubTransaction()))
				break;			/* all fired */
		}

		if (snapshot_set)
			PopActiveSnapshot();
	}
}

/* * ----------
 * AfterTriggerPendingOnRel()
 *		测试是否有任何待处理的 after-trigger 事件针对 rel。
 *
 * 这被 TRUNCATE、CLUSTER、ALTER TABLE 等使用，以检测
 * 对关系执行重大手术是否不安全。注意只检查
 * 本地待处理事件。我们假设具有全局事务 ID 的事件
 * 由其他后端处理。
 * ----------
 *
 *
 *
 *
 *
 */
bool
AfterTriggerPendingOnRel(Oid relid)
{
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	int			depth;

	/* 扫描排队的事件 */
	for_each_event_chunk(event, chunk, afterTriggers.events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);

		/*
		 * 我们可以忽略已完成的事件。（即使 DONE 标志   
		 * 被子事务中止回滚，也没关系，因为 TRUNCATE 等     
		 * 其他操作效果也须回滚。）  
		 */
		if (event->ate_flags & AFTER_TRIGGER_DONE)
			continue;

		if (evtshared->ats_relid == relid)
			return true;
	}

	/*
	 * 也扫描由未完成的查询排队的事件。只有当             
	 * TRUNCATE 等操作由同一关系上更新查询内的函数或        
	 * 触发器执行时才有影响，这相当反常，但仍需检查。 
	 */
	for (depth = 0; depth <= afterTriggers.query_depth && depth < afterTriggers.maxquerydepth; depth++)
	{
		for_each_event_chunk(event, chunk, afterTriggers.query_stack[depth].events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags & AFTER_TRIGGER_DONE)
				continue;

			if (evtshared->ats_relid == relid)
				return true;
		}
	}

	return false;
}

/* ----------
 * AfterTriggerSaveEvent()
 *
 *	由 ExecA[RS]...Triggers() 调用，用来为事件排队应当      
 *	触发的触发器。 
 *
 *	注意：只要事件关联了任何触发器就会调用本函数  
 *	（即使它们已被禁用）。由本函数决定哪些触发器 
 *	真正需要入队。每一行之后也会调用它，即使该事     
 *	件没有触发器——只要该语句存在使用转换表的     
 *	 AFTER STATEMENT 触发器时，以便能构建转换 tuplestore。      
 *	 此外，如果转换捕获针对的是因分区键变更而要移     
 *	动到其他分区的被更新行，那么本函数会被调用两        
 *	次：行被删除时一次（用于捕获 OLD 行），行被插入      
 *	到另一个分区时一次（用于捕获 NEW 行）。这样做         
 *	是分开进行的，因为 DELETE 和 INSERT 发生在不同的             
 *	表上。                                    
 *
 *	转换 tuplestore 是现在构建，而不是在事件从队列         
 *	中取出时构建，因为 AFTER ROW 触发器被允许从该            
 *	语句的转换表中选取元组。
 *
 *	本函数包含特殊支持，用于为分区表经历跨分区更         
 *	新且可能有外键指向它的情况排队更新事件。通常      
 *	情况下，分区表的行触发器不会被触发，因为因该      
 *	表上的操作而被修改的叶子分区包含相同的触发器       
 *	，实际触发的是后者。但这种通用方案在跨分区           
 *	更新期间会给外键触发器带来问题行为，跨分区更        
 *	新被实现为先在源分区上 DELETE 再插入到目标分区。      
 *	具体来说，触发 DELETE 触发器会导致强制执行错误        
 *	的外键动作，因为原始命令是 UPDATE；这种情况下，     
 *	本函数被调用时 relinfo 是该分区表，src_partinfo 和          
 *	 dst_partinfo 分别指向源叶子分区                        
 *	和目标叶子分区。                                             
 *	                                                
 *
 *	is_crosspart_update 为 true 的情况是：在源分区上           
 *	触发了 DELETE 事件（将被忽略），或者在根分区表       
 *	上触发 UPDATE 事件。 
 * ----------
 */
static void
AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
					  ResultRelInfo *src_partinfo,
					  ResultRelInfo *dst_partinfo,
					  int event, bool row_trigger,
					  TupleTableSlot *oldslot, TupleTableSlot *newslot,
					  List *recheckIndexes, Bitmapset *modifiedCols,
					  TransitionCaptureState *transition_capture,
					  bool is_crosspart_update)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	AfterTriggerEventData new_event;
	AfterTriggerSharedData new_shared;
	char		relkind = rel->rd_rel->relkind;
	int			tgtype_event;
	int			tgtype_level;
	int			i;
	Tuplestorestate *fdw_tuplestore = NULL;

	/*
	 * 检查状态。我们使用普通测试而不是 Assert，因为       
	 * 在 RI 触发器配置不当的情况下可能以错误状态到 
	 * 这里，尤其延迟级联触发器时。    
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "AfterTriggerSaveEvent() called outside of query");

	/* 确保我们有足够的空间在此查询深度记录事件。 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * 如果直接命名的关系有任何带有转换表的触发器，     
	 * 那么我们需要捕获转换元组。   
	 */
	if (row_trigger && transition_capture != NULL)
	{
		TupleTableSlot *original_insert_tuple = transition_capture->tcs_original_insert_tuple;

		/*
		 * 根据事件，在适当的转换表中捕获旧元组。         
		 *           
		 */
		if (!TupIsNull(oldslot))
		{
			Tuplestorestate *old_tuplestore;

			old_tuplestore = GetAfterTriggersTransitionTable(event,
															 oldslot,
															 NULL,
															 transition_capture);
			TransitionTableAddTuple(estate, transition_capture, relinfo,
									oldslot, NULL, old_tuplestore);
		}

		/*
		 * 根据事件，在适当的转换表中捕获新元组。         
		 *           
		 */
		if (!TupIsNull(newslot))
		{
			Tuplestorestate *new_tuplestore;

			new_tuplestore = GetAfterTriggersTransitionTable(event,
															 NULL,
															 newslot,
															 transition_capture);
			TransitionTableAddTuple(estate, transition_capture, relinfo,
									newslot, original_insert_tuple, new_tuplestore);
		}

		/*
		 * 如果转换表是来此处的唯一原因，则返回。如   
		 * 前所述，在存在转换表的情况下，更新元组路由期 
		 * 间也可能到这里，此时本函数会分别为 OLD 和  
		 * NEW 各调用一次，因此我们期望两者中恰好有一个为
		 * NULL。    
		 */
		if (trigdesc == NULL ||
			(event == TRIGGER_EVENT_DELETE && !trigdesc->trig_delete_after_row) ||
			(event == TRIGGER_EVENT_INSERT && !trigdesc->trig_insert_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && !trigdesc->trig_update_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && (TupIsNull(oldslot) ^ TupIsNull(newslot))))
			return;
	}

	/*
	 * 对于行级触发器，我们在这里通常看不到分区表，  
	 * 除非是跨分区更新的特殊情况。在这种情况下，       
	 * nodeModifyTable.c:ExecCrossPartitionUpdateForeignKey() 会在       
	 * 这里调用，以便在根目标分区表上排队一个更新 
	 * 事件，同时传入源分区和目标分区及其元组。   
	 */
	Assert(!row_trigger ||
		   rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE ||
		   (is_crosspart_update &&
			TRIGGER_FIRED_BY_UPDATE(event) &&
			src_partinfo != NULL && dst_partinfo != NULL));

	/*
	 * 校验事件代码并收集关联的元组 CTID。             
	 *
	 * 事件代码将既用作位掩码又用作数组偏移量，因此   
	 * 校验很重要，可确保我们不会越过数组的边界。       
	 *        
	 *
	 * 此外，如果我们正在考虑语句级触发器，则检查是   
	 * 否已经为此事件排队了一组这样的触发器，如果是   
	 * 则取消先前那组。这保留了语句级触发器每条语       
	 * 句只触发一次且在行级触发器之后的行为。 
	 */
	switch (event)
	{
		case TRIGGER_EVENT_INSERT:
			tgtype_event = TRIGGER_TYPE_INSERT;
			if (row_trigger)
			{
				Assert(oldslot == NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_INSERT, event);
			}
			break;
		case TRIGGER_EVENT_DELETE:
			tgtype_event = TRIGGER_TYPE_DELETE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot == NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_DELETE, event);
			}
			break;
		case TRIGGER_EVENT_UPDATE:
			tgtype_event = TRIGGER_TYPE_UPDATE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid2));

				/*
				 * 同时记住分区 OID，稍后由 AfterTriggerExecute()  
				 * 取出这些元组。                 
				 */
				if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				{
					Assert(src_partinfo != NULL && dst_partinfo != NULL);
					new_event.ate_src_part =
						RelationGetRelid(src_partinfo->ri_RelationDesc);
					new_event.ate_dst_part =
						RelationGetRelid(dst_partinfo->ri_RelationDesc);
				}
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_UPDATE, event);
			}
			break;
		case TRIGGER_EVENT_TRUNCATE:
			tgtype_event = TRIGGER_TYPE_TRUNCATE;
			Assert(oldslot == NULL);
			Assert(newslot == NULL);
			ItemPointerSetInvalid(&(new_event.ate_ctid1));
			ItemPointerSetInvalid(&(new_event.ate_ctid2));
			break;
		default:
			elog(ERROR, "invalid after-trigger event code: %d", event);
			tgtype_event = 0;	/* keep compiler quiet */
			break;
	}

	/* 确定标志 */
	if (!(relkind == RELKIND_FOREIGN_TABLE && row_trigger))
	{
		if (row_trigger && event == TRIGGER_EVENT_UPDATE)
		{
			if (relkind == RELKIND_PARTITIONED_TABLE)
				new_event.ate_flags = AFTER_TRIGGER_CP_UPDATE;
			else
				new_event.ate_flags = AFTER_TRIGGER_2CTID;
		}
		else
			new_event.ate_flags = AFTER_TRIGGER_1CTID;
	}

	/* 否则，我们将为每个触发器初始化 ate_flags */

	tgtype_level = (row_trigger ? TRIGGER_TYPE_ROW : TRIGGER_TYPE_STATEMENT);

	/*
	 * 必须将源分区和目标分区元组转换/复制到               
	 * 根分区表的格式/槽位中，因为下面的循环            
	 * 期望 oldslot 和 newslot 元组都是该形式。                   
	 */
	if (row_trigger && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		TupleTableSlot *rootslot;
		TupleConversionMap *map;

		rootslot = ExecGetTriggerOldSlot(estate, relinfo);
		map = ExecGetChildToRootMap(src_partinfo);
		if (map)
			oldslot = execute_attr_map_slot(map->attrMap,
											oldslot,
											rootslot);
		else
			oldslot = ExecCopySlot(rootslot, oldslot);

		rootslot = ExecGetTriggerNewSlot(estate, relinfo);
		map = ExecGetChildToRootMap(dst_partinfo);
		if (map)
			newslot = execute_attr_map_slot(map->attrMap,
											newslot,
											rootslot);
		else
			newslot = ExecCopySlot(rootslot, newslot);
	}

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  tgtype_level,
								  TRIGGER_TYPE_AFTER,
								  tgtype_event))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, event,
							modifiedCols, oldslot, newslot))
			continue;

		if (relkind == RELKIND_FOREIGN_TABLE && row_trigger)
		{
			if (fdw_tuplestore == NULL)
			{
				fdw_tuplestore = GetCurrentFDWTuplestore();
				new_event.ate_flags = AFTER_TRIGGER_FDW_FETCH;
			}
			else
				/* 同一元组的后续事件 */
				new_event.ate_flags = AFTER_TRIGGER_FDW_REUSE;
		}

		/*
		 * 如果该触发器是外键强制触发器，那么在某些  
		 * 情况下我们可以跳过事件排队，因为通过检查就  
		 * 能判断 FK 约束仍会通过。在分区表的跨分区更新
		 * 期间也存在一些可以跳过事件排队的                   
		 * 情况。                                    
		 */
		if (TRIGGER_FIRED_BY_UPDATE(event) || TRIGGER_FIRED_BY_DELETE(event))
		{
			switch (RI_FKey_trigger_type(trigger->tgfoid))
			{
				case RI_TRIGGER_PK:

					/*
					 * 对于分区 PK 表的跨分区更新，应跳过由  
					 * 源叶分区上的组件删除所触发的事件，
					 * 除非该约束源自分区本身（!tgisclone），  
					 * 因为在根（分区）目标表上将触发的 
					 * 更新事件会被用来执行必要的    
					 * 外键强制实施                                    
					 * 操作。                                
					 */
					if (is_crosspart_update &&
						TRIGGER_FIRED_BY_DELETE(event) &&
						trigger->tgisclone)
						continue;

					/* 触发器的 PK 表上的更新或删除 */
					if (!RI_FKey_pk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* 跳过排队此事件 */
						continue;
					}
					break;

				case RI_TRIGGER_FK:

					/*
					 * 触发器的 FK 表上的更新。可跳过在该表 
					 * 跨分区更新期间在分区表上触    
					 * 发的更新事件，因为目标叶子分区上触发 
					 * 的插入事件就足以执行必要的外键检查  
					 * 此外，RI_FKey_fk_upd_check_required() 期望    
					 * 被传入包含系统属性的元组，而分区表的 
					 * 虚拟槽中大多并不存在                         
					 * 这些系统属性。                                 
					 *                     
					 */
					if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
						!RI_FKey_fk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* 跳过排队此事件 */
						continue;
					}
					break;

				case RI_TRIGGER_NONE:

					/*
					 * 不是 FK 触发器。分区表跨分区更新期间 
					 * 触发的更新事件不必排队，因为  
					 * 相同的行触发器必存在于受此更新影响的 
					 * 叶分区中，而在这些分区上触发的事件  
					 * 会被排队                                        
					 * 代替。      
					 */
					if (row_trigger &&
						rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
						continue;
					break;
			}
		}

		/*
		 * 如果该触发器是延迟的唯一约束检查触发器，只有
		 * 在唯一约束可能被违反时才将其排队，这一点     
		 * 在索引插入时便可知道。 
		 */
		if (trigger->tgfoid == F_UNIQUE_KEY_RECHECK)
		{
			if (!list_member_oid(recheckIndexes, trigger->tgconstrindid))
				continue;		/* 唯一性确实未被违反 */
		}

		/*
		 * 填充事件结构并将其加入当前查询的队列。注意 
		 * 只要此触发器不使用转换表，我们就把 ats_table  
		 * 设为 NULL，以提高共享事件数据的可共享性。        
		 */
		new_shared.ats_event =
			(event & TRIGGER_EVENT_OPMASK) |
			(row_trigger ? TRIGGER_EVENT_ROW : 0) |
			(trigger->tgdeferrable ? AFTER_TRIGGER_DEFERRABLE : 0) |
			(trigger->tginitdeferred ? AFTER_TRIGGER_INITDEFERRED : 0);
		new_shared.ats_tgoid = trigger->tgoid;
		new_shared.ats_relid = RelationGetRelid(rel);
		new_shared.ats_firing_id = 0;
		if ((trigger->tgoldtable || trigger->tgnewtable) &&
			transition_capture != NULL)
			new_shared.ats_table = transition_capture->tcs_private;
		else
			new_shared.ats_table = NULL;
		new_shared.ats_modifiedcols = modifiedCols;

		afterTriggerAddEvent(&afterTriggers.query_stack[afterTriggers.query_depth].events,
							 &new_event, &new_shared);
	}

	/*
	 * 最后，spool 任何外来元组。元组存储会把它们压缩为
	 * 最小元组，因此这会丢失任何系统列。执行器在我  
	 * 们之前就因无关原因丢失了这些列，所以没问题。
	 */
	if (fdw_tuplestore)
	{
		if (oldslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, oldslot);
		if (newslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, newslot);
	}
}

/*
 * 检测我们是否已经为给定的关系 + 操作排队了 BEFORE     
 *  STATEMENT 触发器，并设置标志使下次调用报告 "true"。      
 */
static bool
before_stmt_triggers_fired(Oid relid, CmdType cmdType)
{
	bool		result;
	AfterTriggersTableData *table;

	/* 检查状态，类似于 AfterTriggerSaveEvent。 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "before_stmt_triggers_fired() called outside of query");

	/* 确保我们有足够的空间在此查询深度记录事件。 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * 我们将此状态保存在同时保存关系 + 操作的转换 
	 * 表的 AfterTriggersTableData 中。这样，如果我们被迫创建  
	 * 新的转换表集合（因为在已触发触发器后有更多      
	 * 元组被输入），我们就允许新的一组语句触发器       
	 * 被排队。                     
	 */
	table = GetAfterTriggersTableData(relid, cmdType);
	result = table->before_trig_done;
	table->before_trig_done = true;
	return result;
}

/*
 * 如果我们之前为给定关系 + 操作排队了一组              
 *  AFTER STATEMENT 触发器，而它们尚未被触发，则取         
 * 消它们。调用者将排队一个全新的集合，排在当前        
 * 子语句可能已排队的任何行级触发器之后，尽可能   
 * 地保留 AFTER ROW 触发器先于 AFTER STATEMENT 触发                
 * 器触发、且后者只触发一次的属性。这处理了多个
 * 外键强制触发器把同一表的触发器顺序排队到同一         
 * 触发器查询级别的情况。但我们无法完全防止古      
 * 怪行为：如果存在使用转换表的 AFTER ROW 触发器，  
 * 我们不希望在这类触发器中的第一个见过转换表          
 * 之后再改变它们。这种情况下，任何额外的事件都      
 * 会导致创建新的转换表，并允许语句级触发器再次       
 * 触发。          
 *
 * 这还会保存当前事件列表的位置，使得以后再次调        
 * 用本函数时能廉价地找到我们即将排队的触发器并     
 * 取消它们
 */
static void
cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	/*
	 * 我们将此状态保存在同时保存关系 + 操作的转换 
	 * 表的 AfterTriggersTableData 中。这样，如果我们被迫创建  
	 * 新的转换表集合（因为在已触发触发器后有更多      
	 * 元组被输入），我们就允许新的一组语句触发器       
	 * 被排队，而不取消旧的那一组。                      
	 */
	table = GetAfterTriggersTableData(relid, cmdType);

	if (table->after_trig_done)
	{
		/*
		 * 我们要从插入任何语句触发器之前存在的尾部位   
		 * 置开始扫描。但事件列表在那时可能完全为空，
		 * 在这种情况下就从当前的头部                       
		 * 扫描。    
		 */
		AfterTriggerEvent event;
		AfterTriggerEventChunk *chunk;

		if (table->after_trig_events.tail)
		{
			chunk = table->after_trig_events.tail;
			event = (AfterTriggerEvent) table->after_trig_events.tailfree;
		}
		else
		{
			chunk = qs->events.head;
			event = NULL;
		}

		for_each_chunk_from(chunk)
		{
			if (event == NULL)
				event = (AfterTriggerEvent) CHUNK_DATA_START(chunk);
			for_each_event_from(event, chunk)
			{
				AfterTriggerShared evtshared = GetTriggerSharedData(event);

				/*
				 * 到达不是目标关系 AS 触发器的事件时，退出
				 * 循环。           
				 */
				if (evtshared->ats_relid != relid)
					goto done;
				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) != tgevent)
					goto done;
				if (!TRIGGER_FIRED_FOR_STATEMENT(evtshared->ats_event))
					goto done;
				if (!TRIGGER_FIRED_AFTER(evtshared->ats_event))
					goto done;
				/* 好的，将其标记为 DONE */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			/* 标记我们必须为下一个块重新初始化事件指针 */
			event = NULL;
		}
	}
done:

	/* 无论如何，保存当前插入点以备下次使用 */
	table->after_trig_done = true;
	table->after_trig_events = qs->events;
}

/*
 * session_replication_role 的 GUC assign_hook
 */
void
assign_session_replication_role(int newval, void *extra)
{
	/*
	 * 更改复制角色时必须刷新计划缓存；但无必要时    
	 * 不刷新。        
	 */
	if (SessionReplicationRole != newval)
		ResetPlanCache();
}

/*
 * SQL 函数 pg_trigger_depth()  
 */
Datum
pg_trigger_depth(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyTriggerDepth);
}
