/*-------------------------------------------------------------------------
 *
 * execReplication.c
 *	  用于逻辑复制的各类执行器例程             
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execReplication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "replication/logicalrelation.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static bool tuples_equal(TupleTableSlot *slot1, TupleTableSlot *slot2,
						 TypeCacheEntry **eq);

/*
 * 为在关系 'rel' 中搜索元组 'key' 设置一个 ScanKey，         
 * 元组需匹配 'rel' *而非* idxrel！
 *
 * 返回索引扫描要使用的列数。            
 *
 * 这不是通用例程，idxrel 必须是 PK、RI 或可用于              
 * REPLICA IDENTITY FULL 表。详情参见 FindUsableIndexForReplicaIdentityFull() 
 * 了解详情
 *
 * 根据定义，关系的复制标识满足与之相关的所有限           
 * 制。注意，任何其他索引也可能满足这些限制。        
 */
static int
build_replindex_scan_key(ScanKey skey, Relation rel, Relation idxrel,
						 TupleTableSlot *searchslot)
{
	int			index_attoff;
	int			skey_attoff = 0;
	Datum		indclassDatum;
	oidvector  *opclass;
	int2vector *indkey = &idxrel->rd_index->indkey;

	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, idxrel->rd_indextuple,
										   Anum_pg_index_indclass);
	opclass = (oidvector *) DatumGetPointer(indclassDatum);

	/* 为索引中每个非表达式属性构建 scankey。          */
	for (index_attoff = 0; index_attoff < IndexRelationGetNumberOfKeyAttributes(idxrel);
		 index_attoff++)
	{
		Oid			operator;
		Oid			optype;
		Oid			opfamily;
		RegProcedure regop;
		int			table_attno = indkey->values[index_attoff];

		if (!AttributeNumberIsValid(table_attno))
		{
			/*
			 * XXX：目前我们不支持扫描键中的表达式，       
			 * 参见下文   
			 */
			continue;
		}

		/*
		 * 加载算子信息。我们需要它来获取扫描键的         
		 * 相等算子函数。     
		 */
		optype = get_opclass_input_type(opclass->values[index_attoff]);
		opfamily = get_opclass_family(opclass->values[index_attoff]);

		operator = get_opfamily_member(opfamily, optype,
									   optype,
									   BTEqualStrategyNumber);
		if (!OidIsValid(operator))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 BTEqualStrategyNumber, optype, optype, opfamily);

		regop = get_opcode(operator);

		/* 初始化 scankey。    */
		ScanKeyInit(&skey[skey_attoff],
					index_attoff + 1,
					BTEqualStrategyNumber,
					regop,
					searchslot->tts_values[table_attno - 1]);

		skey[skey_attoff].sk_collation = idxrel->rd_indcollation[index_attoff];

		/* 检查 NULL 值。    */
		if (searchslot->tts_isnull[table_attno - 1])
			skey[skey_attoff].sk_flags |= (SK_ISNULL | SK_SEARCHNULL);

		skey_attoff++;
	}

	/* 索引扫描必须始终至少有一个属性。                */
	Assert(skey_attoff > 0);

	return skey_attoff;
}

/*
 * 使用索引在关系 'rel' 中搜索元组。      
 *
 * 如果找到匹配的元组，则用 lockmode 锁定它，将其           
 * 内容填入槽，返回 true；否则为 false。 
 */
bool
RelationFindReplTupleByIndex(Relation rel, Oid idxoid,
							 LockTupleMode lockmode,
							 TupleTableSlot *searchslot,
							 TupleTableSlot *outslot)
{
	ScanKeyData skey[INDEX_MAX_KEYS];
	int			skey_attoff;
	IndexScanDesc scan;
	SnapshotData snap;
	TransactionId xwait;
	Relation	idxrel;
	bool		found;
	TypeCacheEntry **eq = NULL;
	bool		isIdxSafeToSkipDuplicates;

	/* 打开索引。 */
	idxrel = index_open(idxoid, RowExclusiveLock);

	isIdxSafeToSkipDuplicates = (GetRelationIdentityOrPK(rel) == idxoid);

	InitDirtySnapshot(snap);

	/* 构建扫描键 */
	skey_attoff = build_replindex_scan_key(skey, rel, idxrel, searchslot);

	/* 启动索引扫描。*/
	scan = index_beginscan(rel, idxrel, &snap, skey_attoff, 0);

retry:
	found = false;

	index_rescan(scan, skey, skey_attoff, NULL, 0);

	/* 尝试查找该元组 */
	while (index_getnext_slot(scan, ForwardScanDirection, outslot))
	{
		/*
		 * 如果索引是主键或复制标识索引，则避免       
		 * 昂贵的相等检查  
		 */
		if (!isIdxSafeToSkipDuplicates)
		{
			if (eq == NULL)
				eq = palloc0(sizeof(*eq) * outslot->tts_tupleDescriptor->natts);

			if (!tuples_equal(outslot, searchslot, eq))
				continue;
		}

		ExecMaterializeSlot(outslot);

		xwait = TransactionIdIsValid(snap.xmin) ?
			snap.xmin : snap.xmax;

		/*
		 * 如果元组已被锁定，则等待锁定事务结束后再      
		 * 重试
		 */
		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}

		/* 已找到元组且未被锁定      */
		found = true;
		break;
	}

	/* 找到元组，尝试以 lockmode 锁定。  */
	if (found)
	{
		TM_FailureData tmfd;
		TM_Result	res;

		PushActiveSnapshot(GetLatestSnapshot());

		res = table_tuple_lock(rel, &(outslot->tts_tid), GetActiveSnapshot(),
							   outslot,
							   GetCurrentCommandId(false),
							   lockmode,
							   LockWaitBlock,
							   0 /* 不跟随更新      */ ,
							   &tmfd);

		PopActiveSnapshot();

		switch (res)
		{
			case TM_Ok:
				break;
			case TM_Updated:
				/* XXX：改进这里的处理*/
				if (ItemPointerIndicatesMovedPartitions(&tmfd.ctid))
					ereport(LOG,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update, retrying")));
				else
					ereport(LOG,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("concurrent update, retrying")));
				goto retry;
			case TM_Deleted:
				/* XXX：改进这里的处理*/
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent delete, retrying")));
				goto retry;
			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;
			default:
				elog(ERROR, "unexpected table_tuple_lock status: %u", res);
				break;
		}
	}

	index_endscan(scan);

	/* 在提交之前不要释放锁。*/
	index_close(idxrel, NoLock);

	return found;
}

/*
 * 通过检查槽中的元组是否具有相等的值来比较它们。 
 */
static bool
tuples_equal(TupleTableSlot *slot1, TupleTableSlot *slot2,
			 TypeCacheEntry **eq)
{
	int			attrnum;

	Assert(slot1->tts_tupleDescriptor->natts ==
		   slot2->tts_tupleDescriptor->natts);

	slot_getallattrs(slot1);
	slot_getallattrs(slot2);

	/* 检查各属性是否相等。    */
	for (attrnum = 0; attrnum < slot1->tts_tupleDescriptor->natts; attrnum++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;

		att = TupleDescAttr(slot1->tts_tupleDescriptor, attrnum);

		/*
		 * 忽略删除列与生成列，因为发布者不会发送这些   
		 * 列  
		 */
		if (att->attisdropped || att->attgenerated)
			continue;

		/*
		 * 若一个值为 NULL 而另一个不是，则两者肯定不      
		 * 等  
		 */
		if (slot1->tts_isnull[attrnum] != slot2->tts_isnull[attrnum])
			return false;

		/*
		 * 两者均为 NULL 时可视为相等。        
		 */
		if (slot1->tts_isnull[attrnum] || slot2->tts_isnull[attrnum])
			continue;

		typentry = eq[attrnum];
		if (typentry == NULL)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an equality operator for type %s",
								format_type_be(att->atttypid))));
			eq[attrnum] = typentry;
		}

		if (!DatumGetBool(FunctionCall2Coll(&typentry->eq_opr_finfo,
											att->attcollation,
											slot1->tts_values[attrnum],
											slot2->tts_values[attrnum])))
			return false;
	}

	return true;
}

/*
 * 使用顺序扫描在关系 'rel' 中搜索元组。          
 *
 * 如果找到匹配的元组，则用 lockmode 锁定它，将其           
 * 内容填入槽，返回 true；否则为 false。 
 *
 * 注意它会在首个匹配元组处停止。    
 *
 * 显然，对于行数超过少数几行的表，这可能相当慢。   
 */
bool
RelationFindReplTupleSeq(Relation rel, LockTupleMode lockmode,
						 TupleTableSlot *searchslot, TupleTableSlot *outslot)
{
	TupleTableSlot *scanslot;
	TableScanDesc scan;
	SnapshotData snap;
	TypeCacheEntry **eq;
	TransactionId xwait;
	bool		found;
	TupleDesc	desc PG_USED_FOR_ASSERTS_ONLY = RelationGetDescr(rel);

	Assert(equalTupleDescs(desc, outslot->tts_tupleDescriptor));

	eq = palloc0(sizeof(*eq) * outslot->tts_tupleDescriptor->natts);

	/* 启动堆扫描。 */
	InitDirtySnapshot(snap);
	scan = table_beginscan(rel, &snap, 0, NULL);
	scanslot = table_slot_create(rel, NULL);

retry:
	found = false;

	table_rescan(scan, NULL);

	/* 尝试查找该元组 */
	while (table_scan_getnextslot(scan, ForwardScanDirection, scanslot))
	{
		if (!tuples_equal(scanslot, searchslot, eq))
			continue;

		found = true;
		ExecCopySlot(outslot, scanslot);

		xwait = TransactionIdIsValid(snap.xmin) ?
			snap.xmin : snap.xmax;

		/*
		 * 如果元组已被锁定，则等待锁定事务结束后再      
		 * 重试
		 */
		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}

		/* 已找到元组且未被锁定      */
		break;
	}

	/* 找到元组，尝试以 lockmode 锁定。  */
	if (found)
	{
		TM_FailureData tmfd;
		TM_Result	res;

		PushActiveSnapshot(GetLatestSnapshot());

		res = table_tuple_lock(rel, &(outslot->tts_tid), GetActiveSnapshot(),
							   outslot,
							   GetCurrentCommandId(false),
							   lockmode,
							   LockWaitBlock,
							   0 /* 不跟随更新      */ ,
							   &tmfd);

		PopActiveSnapshot();

		switch (res)
		{
			case TM_Ok:
				break;
			case TM_Updated:
				/* XXX：改进这里的处理*/
				if (ItemPointerIndicatesMovedPartitions(&tmfd.ctid))
					ereport(LOG,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update, retrying")));
				else
					ereport(LOG,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("concurrent update, retrying")));
				goto retry;
			case TM_Deleted:
				/* XXX：改进这里的处理*/
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent delete, retrying")));
				goto retry;
			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;
			default:
				elog(ERROR, "unexpected table_tuple_lock status: %u", res);
				break;
		}
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(scanslot);

	return found;
}

/*
 * 把槽中表示的元组插入到关系中，更新索引，             
 * 并执行任何约束和每行触发器。       
 *
 * 调用者负责打开索引。                
 */
void
ExecSimpleRelationInsert(ResultRelInfo *resultRelInfo,
						 EState *estate, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;

	/* 目前我们仅支持表。     */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	CheckCmdReplicaIdentity(rel, CMD_INSERT);

	/* BEFORE ROW INSERT 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		if (!ExecBRInsertTriggers(estate, resultRelInfo, slot))
			skip_tuple = true;	/* "do nothing" */
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;

		/* 计算存储的生成列         */
		if (rel->rd_att->constr &&
			rel->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_INSERT);

		/* 检查该元组的约束           */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);
		if (rel->rd_rel->relispartition)
			ExecPartitionCheck(resultRelInfo, slot, estate, true);

		/* 好，存储该元组并为它创建索引条目    */
		simple_table_tuple_insert(resultRelInfo->ri_RelationDesc, slot);

		if (resultRelInfo->ri_NumIndices > 0)
			recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
												   slot, estate, false, false,
												   NULL, NIL, false);

		/* AFTER ROW INSERT 触发器*/
		ExecARInsertTriggers(estate, resultRelInfo, slot,
							 recheckIndexes, NULL);

		/*
		 * XXX 理论上我们应向上面的调用传入一个               
		 * TransitionCaptureState 对象来捕获转换元组，但        
		 * 语句后触发器反正尚不会被复制触发  
		 */

		list_free(recheckIndexes);
	}
}

/*
 * 找到 searchslot 元组并用槽中的数据更新它，     
 * 更新索引，并执行任何约束和每行触发器。            
 *
 * 调用者负责打开索引。                
 */
void
ExecSimpleRelationUpdate(ResultRelInfo *resultRelInfo,
						 EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot, TupleTableSlot *slot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ItemPointer tid = &(searchslot->tts_tid);

	/*
	 * 我们仅支持非系统表，由      
	 * check_publication_add_relation() 负责。   
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);
	Assert(!IsCatalogRelation(rel));

	CheckCmdReplicaIdentity(rel, CMD_UPDATE);

	/* BEFORE ROW UPDATE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		if (!ExecBRUpdateTriggers(estate, epqstate, resultRelInfo,
								  tid, NULL, slot, NULL, NULL))
			skip_tuple = true;	/* "do nothing" */
	}

	if (!skip_tuple)
	{
		List	   *recheckIndexes = NIL;
		TU_UpdateIndexes update_indexes;

		/* 计算存储的生成列         */
		if (rel->rd_att->constr &&
			rel->rd_att->constr->has_generated_stored)
			ExecComputeStoredGenerated(resultRelInfo, estate, slot,
									   CMD_UPDATE);

		/* 检查该元组的约束           */
		if (rel->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);
		if (rel->rd_rel->relispartition)
			ExecPartitionCheck(resultRelInfo, slot, estate, true);

		simple_table_tuple_update(rel, tid, slot, estate->es_snapshot,
								  &update_indexes);

		if (resultRelInfo->ri_NumIndices > 0 && (update_indexes != TU_None))
			recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
												   slot, estate, true, false,
												   NULL, NIL,
												   (update_indexes == TU_Summarizing));

		/* AFTER ROW UPDATE 触发器*/
		ExecARUpdateTriggers(estate, resultRelInfo,
							 NULL, NULL,
							 tid, NULL, slot,
							 recheckIndexes, NULL, false);

		list_free(recheckIndexes);
	}
}

/*
 * 找到 searchslot 元组并将其删除，并执行任何约束     
 * 和每行触发器。
 *
 * 调用者负责打开索引。                
 */
void
ExecSimpleRelationDelete(ResultRelInfo *resultRelInfo,
						 EState *estate, EPQState *epqstate,
						 TupleTableSlot *searchslot)
{
	bool		skip_tuple = false;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	ItemPointer tid = &searchslot->tts_tid;

	CheckCmdReplicaIdentity(rel, CMD_DELETE);

	/* BEFORE ROW DELETE 触发器*/
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_before_row)
	{
		skip_tuple = !ExecBRDeleteTriggers(estate, epqstate, resultRelInfo,
										   tid, NULL, NULL, NULL, NULL);
	}

	if (!skip_tuple)
	{
		/* 好，删除该元组*/
		simple_table_tuple_delete(rel, tid, estate->es_snapshot);

		/* AFTER ROW DELETE 触发器*/
		ExecARDeleteTriggers(estate, resultRelInfo,
							 tid, NULL, NULL, false);
	}
}

/*
 * 检查命令是否可以用当前复制标识执行。         
 */
void
CheckCmdReplicaIdentity(Relation rel, CmdType cmd)
{
	PublicationDesc pubdesc;

	/*
	 * 对于分区表跳过复制标识检查，因为操作实际上是    
	 * 在叶子分区上执行的。                           
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* 只需对 UPDATE 和 DELETE 进行检查。      */
	if (cmd != CMD_UPDATE && cmd != CMD_DELETE)
		return;

	/*
	 * 只有当关系所在发布中的行过滤器引用的所有列都   
	 * 有效时，才能安全地执行 UPDATE/DELETE - 即当所       
	 * 有被引用的列都属于 REPLICA IDENTITY，                      
	 * 或者该表不发布 UPDATE 或 DELETE。       
	 *
	 * XXX 我们可以先检查是否有任何发布包含针对该
	 * 关系的行过滤器。如果没有且该关系具有复制标识，
	 * 则可以避免构建描述符，但由于这只发生一次，      
	 * 这似乎不值得为此增加额外的                        
	 * 复杂性  
	 */
	RelationBuildPublicationDesc(rel, &pubdesc);
	if (cmd == CMD_UPDATE && !pubdesc.rf_valid_for_update)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot update table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column used in the publication WHERE expression is not part of the replica identity.")));
	else if (cmd == CMD_UPDATE && !pubdesc.cols_valid_for_update)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot update table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column list used by the publication does not cover the replica identity.")));
	else if (cmd == CMD_DELETE && !pubdesc.rf_valid_for_delete)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot delete from table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column used in the publication WHERE expression is not part of the replica identity.")));
	else if (cmd == CMD_DELETE && !pubdesc.cols_valid_for_delete)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot delete from table \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Column list used by the publication does not cover the replica identity.")));

	/* 若关系有复制标识，则总是没有问题。  */
	if (OidIsValid(RelationGetReplicaIndex(rel)))
		return;

	/* REPLICA IDENTITY FULL 对 UPDATE/DELETE 也适用。  */
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return;

	/*
	 * 这是 UPDATE/DELETE 且没有复制标识。          
	 *
	 * 检查该表是否发布 UPDATE 或 DELETE。   
	 */
	if (cmd == CMD_UPDATE && pubdesc.pubactions.pubupdate)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot update table \"%s\" because it does not have a replica identity and publishes updates",
						RelationGetRelationName(rel)),
				 errhint("To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.")));
	else if (cmd == CMD_DELETE && pubdesc.pubactions.pubdelete)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot delete from table \"%s\" because it does not have a replica identity and publishes deletes",
						RelationGetRelationName(rel)),
				 errhint("To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.")));
}


/*
 * 检查我们是否支持写入特定的 relkind。
 *
 * nspname 和 relname 仅在报告错误时需要。          
 */
void
CheckSubscriptionRelkind(char relkind, const char *nspname,
						 const char *relname)
{
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use relation \"%s.%s\" as logical replication target",
						nspname, relname),
				 errdetail_relkind_not_supported(relkind)));
}
