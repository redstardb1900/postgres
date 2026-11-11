/*-------------------------------------------------------------------------
 *
 * execPartition.c
 *	  分区支持的支撑例程。    
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execPartition.c
 *
 *---------------------------------------------------------------------    
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"


/*-----------------------
 * PartitionTupleRouting 封装把插入分区表的               
 * 元组路由到其某个叶子分区所需的全部               
 * 信息。  
 *
 * partition_root
 *		该命令的目标分区表。                         
 *
 * partition_dispatch_info
 *		'max_dispatch' 个元素的数组，包含指向          
 *		元组路由所触及的每个分区表的                           
 *		PartitionDispatch 对象的指针。目标                      
 *		分区表的表项总是存在于该数组的             
 *		第 0 个元素。索引详见 PartitionDispatchData->indexes  
 *		注释  
 *
 * nonleaf_partitions
 *		'max_dispatch' 个元素的数组，存放指向            
 *		非叶子分区的伪 ResultRelInfo 对象指针，              
 *		便于检查分区约束 
 *
 * num_dispatch
 *		'partition_dispatch_info' 数组当前存储的项数。           
 *		同时也用作下一个空闲数组元素的                      
 *		索引，供新的 PartitionDispatch 使用。       
 *
 * max_dispatch
 *		'partition_dispatch_info' 数组当前分配的大小。          
 *
 * partitions
 *		'max_partitions' 个元素的数组，存放指向          
 *		元组路由所触及的每个叶子分区的                   
 *		ResultRelInfo。其中一些指针从所属                           
 *		ModifyTableState 节点借入，其余的                       
 *		则为元组路由而专建。该数组如何 
 *		索引，参见 PartitionDispatchData->indexes                 
 *		注释  
 *
 * is_borrowed_rel
 *		'max_partitions' 个布尔值的数组，记录                   
 *		'partitions' 中表项是否为借自所属                        
 *		ModifyTableState 而非在此构建。              
 *
 * num_partitions
 *		'partitions' 数组当前存储的项数，也是                  
 *		下一个空闲数组元素的索引，供新的          
 *		ResultRelInfo 使用。                      
 *
 * max_partitions
 *		'partitions' 数组当前分配的大小。          
 *
 * memcxt
 *		用于分配附属结构的内存上下文。      
 *-----------------------
 */
struct PartitionTupleRouting
{
	Relation	partition_root;
	PartitionDispatch *partition_dispatch_info;
	ResultRelInfo **nonleaf_partitions;
	int			num_dispatch;
	int			max_dispatch;
	ResultRelInfo **partitions;
	bool	   *is_borrowed_rel;
	int			num_partitions;
	int			max_partitions;
	MemoryContext memcxt;
};

/*-----------------------
 * PartitionDispatch - 分区层级中一个分区表的信息，            
 * 用于把元组路由到其任意分区。                      
 * PartitionDispatch 总是封装在                                      
 * PartitionTupleRouting 之内。                              
 *
 * reldesc
 *		该表的关系描述符        
 *
 * key
 *		该表的分区键信息              
 *
 * keystate
 *		分区键中表达式所需的执行状态                   
 *
 * partdesc
 *		该表的分区描述符         
 *
 * tupslot
 *		以该表元组描述符初始化的独立                     
 *		TupleTableSlot；与父表间无需转换时                    
 *		为 NULL 
 *
 * tupmap
 *		TupleConversionMap，用于把父表的行类型转换为该表的     
 *		行类型（在路由前提取元组的分区键时）。         
 *		若无需元组转换则存入                                    
 *		 NULL。               
 *
 * indexes
 *		含 partdesc->nparts 个元素的数组。                        
 *		对叶子分区，该索引对应其在封装的                 
 *		PartitionTupleRouting 的 partitions                                  
 *		数组中的 ResultRelInfo；对分区化的                
 *		分区，则对应其 partition_dispatch_info                       
 *		数组中的表项。-1 表示尚未分配。       
 *-----------------------
 */
typedef struct PartitionDispatchData
{
	Relation	reldesc;
	PartitionKey key;
	List	   *keystate;		/* ExprState 列表  */
	PartitionDesc partdesc;
	TupleTableSlot *tupslot;
	AttrMap    *tupmap;
	int			indexes[FLEXIBLE_ARRAY_MEMBER];
}			PartitionDispatchData;


static ResultRelInfo *ExecInitPartitionInfo(ModifyTableState *mtstate,
											EState *estate, PartitionTupleRouting *proute,
											PartitionDispatch dispatch,
											ResultRelInfo *rootResultRelInfo,
											int partidx);
static void ExecInitRoutingInfo(ModifyTableState *mtstate,
								EState *estate,
								PartitionTupleRouting *proute,
								PartitionDispatch dispatch,
								ResultRelInfo *partRelInfo,
								int partidx,
								bool is_borrowed_rel);
static PartitionDispatch ExecInitPartitionDispatchInfo(EState *estate,
													   PartitionTupleRouting *proute,
													   Oid partoid, PartitionDispatch parent_pd,
													   int partidx, ResultRelInfo *rootResultRelInfo);
static void FormPartitionKeyDatum(PartitionDispatch pd,
								  TupleTableSlot *slot,
								  EState *estate,
								  Datum *values,
								  bool *isnull);
static int	get_partition_for_tuple(PartitionDispatch pd, Datum *values,
									bool *isnull);
static char *ExecBuildSlotPartitionKeyDescription(Relation rel,
												  Datum *values,
												  bool *isnull,
												  int maxfieldlen);
static List *adjust_partition_colnos(List *colnos, ResultRelInfo *leaf_part_rri);
static List *adjust_partition_colnos_using_map(List *colnos, AttrMap *attrMap);
static PartitionPruneState *CreatePartitionPruneState(PlanState *planstate,
													  PartitionPruneInfo *pruneinfo);
static void InitPartitionPruneContext(PartitionPruneContext *context,
									  List *pruning_steps,
									  PartitionDesc partdesc,
									  PartitionKey partkey,
									  PlanState *planstate,
									  ExprContext *econtext);
static void PartitionPruneFixSubPlanMap(PartitionPruneState *prunestate,
										Bitmapset *initially_valid_subplans,
										int n_total_subplans);
static void find_matching_subplans_recurse(PartitionPruningData *prunedata,
										   PartitionedRelPruningData *pprune,
										   bool initial_prune,
										   Bitmapset **validsubplans);


/*
 * ExecSetupPartitionTupleRouting - 为分区表的                  
 * 元组路由准备所需的信息，将其封装为     
 * PartitionTupleRouting 并返回。    
 *
 * 调用者在对 ExecFindPartition() 的调用                      
 * 期间必须使用返回的 PartitionTupleRouting。                  
 * 分区的 ResultRelInfo 仅初次找到时分配。      
 *
 * 当前内存上下文用于分配此结构及后续               
 * 从中分配的所有附属结构。通常应为                      
 * estate->es_query_cxt。           
 */
PartitionTupleRouting *
ExecSetupPartitionTupleRouting(EState *estate, Relation rel)
{
	PartitionTupleRouting *proute;

	/*
	 * 此处我们尽量以最小代价来建立                          
	 * PartitionTupleRouting。各分区的                                  
	 * ResultRelInfo 均按需构建，仅当确                             
	 * 实需要路由元组到该分区时才做。                      
	 * 原因在于 INSERT 常只插入一个元组，必须快。 
	 */
	proute = (PartitionTupleRouting *) palloc0(sizeof(PartitionTupleRouting));
	proute->partition_root = rel;
	proute->memcxt = CurrentMemoryContext;
	/* 其余成员通过清零来初始化   */

	/*
	 * 初始化此表的 PartitionDispatch 对象。此处                  
	 * 将父级传为 NULL，因为我们无需                             
	 * 关心其父表。
	 */
	ExecInitPartitionDispatchInfo(estate, proute, RelationGetRelid(rel),
								  NULL, 0, NULL);

	return proute;
}

/*
 * ExecFindPartition -- 返回 *slot 中元组所属                        
 * 的叶子分区的 ResultRelInfo。           
 *
 * 若该分区的 ResultRelInfo 尚不存在于                              
 * 'proute' 中，我们就建立一个，或复用                       
 * mtstate 的 resultRelInfo 数组中的既有项；此时我们会    
 * 验证其为有效 INSERT 目标并做初始化。          
 *
 * rootResultRelInfo 是查询命名的关系。        
 *
 * estate 必须非 NULL；需要用它来计算分区                     
 * 键中的表达式；其 per-tuple 上下文则                     
 * 用作暂存  
 *
 * 若未找到叶子分区，本例程会以适当的                        
 * 错误消息报错。另外，若所找到的目                         
 * 标分区无效也可能报错。   
 */
ResultRelInfo *
ExecFindPartition(ModifyTableState *mtstate,
				  ResultRelInfo *rootResultRelInfo,
				  PartitionTupleRouting *proute,
				  TupleTableSlot *slot, EState *estate)
{
	PartitionDispatch *pd = proute->partition_dispatch_info;
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS];
	Relation	rel;
	PartitionDispatch dispatch;
	PartitionDesc partdesc;
	ExprContext *ecxt = GetPerTupleExprContext(estate);
	TupleTableSlot *ecxt_scantuple_saved = ecxt->ecxt_scantuple;
	TupleTableSlot *rootslot = slot;
	TupleTableSlot *myslot = NULL;
	MemoryContext oldcxt;
	ResultRelInfo *rri = NULL;

	/* 此处使用 per-tuple 上下文以免泄漏内存 */
	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	/*
	 * 首先检查根表的分区约束（若有）。若                    
	 * 元组本不属于根表，路由它毫无意义。             
	 */
	if (rootResultRelInfo->ri_RelationDesc->rd_rel->relispartition)
		ExecPartitionCheck(rootResultRelInfo, slot, estate, true);

	/* 从根分区表开始                 */
	dispatch = pd[0];
	while (dispatch != NULL)
	{
		int			partidx = -1;
		bool		is_leaf;

		CHECK_FOR_INTERRUPTS();

		rel = dispatch->reldesc;
		partdesc = dispatch->partdesc;

		/*
		 * 从元组中提取分区键。FormPartitionKeyDatum()            
		 * 所调用的表达式求值机制要求                       
		 * ecxt_scantuple 指向正确的元组槽。                        
		 * 若当前分区层的表元组描述符有别                 
		 * 于父表，槽可能已改变，故需相应                     
		 * 更新 ecxt_scantuple。             
		 */
		ecxt->ecxt_scantuple = slot;
		FormPartitionKeyDatum(dispatch, slot, estate, values, isnull);

		/*
		 * 若此分区表没有分区，或这些值没有               
		 * 分区，则报错。   
		 */
		if (partdesc->nparts == 0 ||
			(partidx = get_partition_for_tuple(dispatch, values, isnull)) < 0)
		{
			char	   *val_desc;

			val_desc = ExecBuildSlotPartitionKeyDescription(rel,
															values, isnull, 64);
			Assert(OidIsValid(RelationGetRelid(rel)));
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("no partition of relation \"%s\" found for row",
							RelationGetRelationName(rel)),
					 val_desc ?
					 errdetail("Partition key of the failing row contains %s.",
							   val_desc) : 0,
					 errtable(rel)));
		}

		is_leaf = partdesc->is_leaf[partidx];
		if (is_leaf)
		{
			/*
			 * 已到达叶子分区——好极了。看看                
			 * 是否已有该分区的 ResultRelInfo。            
			 */
			if (likely(dispatch->indexes[partidx] >= 0))
			{
				/* ResultRelInfo 已构建     */
				Assert(dispatch->indexes[partidx] < proute->num_partitions);
				rri = proute->partitions[dispatch->indexes[partidx]];
			}
			else
			{
				/*
				 * 若该分区在所属 ModifyTableState 节点           
				 * 中已知，我们可以复用该 ResultRelInfo，        
				 * 免去 ExecInitPartitionInfo() 新建  
				 */
				rri = ExecLookupResultRelByOid(mtstate,
											   partdesc->oids[partidx],
											   true, false);
				if (rri)
				{
					ModifyTable *node = (ModifyTable *) mtstate->ps.plan;

					/* 验证该 ResultRelInfo 允许 INSERT    */
					CheckValidResultRelNew(rri, CMD_INSERT,
										   node ? node->onConflictAction : ONCONFLICT_NONE,
										   NIL);

					/*
					 * 初始化向此分区插入本元组及后续   
					 * 元组所需的信息。                   
					 */
					ExecInitRoutingInfo(mtstate, estate, proute, dispatch,
										rri, partidx, true);
				}
				else
				{
					/* 这里需要新建一个。  */
					rri = ExecInitPartitionInfo(mtstate, estate, proute,
												dispatch,
												rootResultRelInfo, partidx);
				}
			}
			Assert(rri != NULL);

			/* 发出终止循环的信号  */
			dispatch = NULL;
		}
		else
		{
			/*
			 * 该分区是子分区表；获取其 PartitionDispatch         
			 */
			if (likely(dispatch->indexes[partidx] >= 0))
			{
				/* 已构建。   */
				Assert(dispatch->indexes[partidx] < proute->num_dispatch);

				rri = proute->nonleaf_partitions[dispatch->indexes[partidx]];

				/*
				 * 下移到下一个分区层级并继续搜索，      
				 * 直到找到匹配此元组的叶子分区            
				 */
				dispatch = pd[dispatch->indexes[partidx]];
			}
			else
			{
				/* 尚未构建，现在建。 */
				PartitionDispatch subdispatch;

				/*
				 * 创建新的 PartitionDispatch。我们把当前的       
				 * 当作父 PartitionDispatch。    
				 */
				subdispatch = ExecInitPartitionDispatchInfo(estate,
															proute,
															partdesc->oids[partidx],
															dispatch, partidx,
															mtstate->rootResultRelInfo);
				Assert(dispatch->indexes[partidx] >= 0 &&
					   dispatch->indexes[partidx] < proute->num_dispatch);

				rri = proute->nonleaf_partitions[dispatch->indexes[partidx]];
				dispatch = subdispatch;
			}

			/*
			 * 若与先前的父级布局不同，则将元                  
			 * 组转换过去。  
			 */
			if (dispatch->tupslot)
			{
				AttrMap    *map = dispatch->tupmap;
				TupleTableSlot *tempslot = myslot;

				myslot = dispatch->tupslot;
				slot = execute_attr_map_slot(map, slot, myslot);

				if (tempslot != NULL)
					ExecClearTuple(tempslot);
			}
		}

		/*
		 * 若此分区是默认分区，我们必须现在                 
		 * 检查其分区约束，该约束可能已因             
		 * 并发添加分区而改变。       
		 *
		 * （我们在此处执行，而不指望 ExecInsert               
		 * 代劳，因为不想遗漏非叶子分区。）        
		 */
		if (partidx == partdesc->boundinfo->default_index)
		{
			/*
			 * 元组必须匹配该分区的布局，约束表达式        
			 * 才能成功求值。若该分区是子                      
			 * 分区化的，上面代码已保证这一点；               
			 * 但对叶子分区，元组仍匹配父级的              
			 * 布局。       
			 *
			 * 注意我们虽有从根分区到当前分区          
			 * 的映射，却没有从直接父级出发的                 
			 * 映射。若需要转换，就从根槽转换；               
			 * 否则原样用它  
			 */
			if (is_leaf)
			{
				TupleConversionMap *map = ExecGetRootToChildMap(rri, estate);

				if (map)
					slot = execute_attr_map_slot(map->attrMap, rootslot,
												 rri->ri_PartitionTupleSlot);
				else
					slot = rootslot;
			}

			ExecPartitionCheck(rri, slot, estate, true);
		}
	}

	/* 释放在最低层父级专用槽中的元组。         */
	if (myslot != NULL)
		ExecClearTuple(myslot);
	/* 并恢复 ecxt 的 scantuple */
	ecxt->ecxt_scantuple = ecxt_scantuple_saved;
	MemoryContextSwitchTo(oldcxt);

	return rri;
}

/*
 * ExecInitPartitionInfo
 *		锁定分区并初始化 ResultRelInfo。同时为该             
 *		分区建立其他信息，存入 proute->                          
 *		partitions 数组。         
 *
 * 返回 ResultRelInfo     
 */
static ResultRelInfo *
ExecInitPartitionInfo(ModifyTableState *mtstate, EState *estate,
					  PartitionTupleRouting *proute,
					  PartitionDispatch dispatch,
					  ResultRelInfo *rootResultRelInfo,
					  int partidx)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	Oid			partOid = dispatch->partdesc->oids[partidx];
	Relation	partrel;
	int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;
	Relation	firstResultRel = mtstate->resultRelInfo[0].ri_RelationDesc;
	ResultRelInfo *leaf_part_rri;
	MemoryContext oldcxt;
	AttrMap    *part_attmap = NULL;
	bool		found_whole_row;

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	partrel = table_open(partOid, RowExclusiveLock);

	leaf_part_rri = makeNode(ResultRelInfo);
	InitResultRelInfo(leaf_part_rri,
					  partrel,
					  0,
					  rootResultRelInfo,
					  estate->es_instrument);

	/*
	 * 验证结果关系是 INSERT 的有效目标。对分区键            
	 * 的 UPDATE 会成为 DELETE+INSERT 操作，因此                     
	 * 操作为 CMD_UPDATE 时仍需此检查。
	 */
	CheckValidResultRelNew(leaf_part_rri, CMD_INSERT,
						   node ? node->onConflictAction : ONCONFLICT_NONE,
						   NIL);

	/*
	 * 打开分区的索引。用户可能要求检查                       
	 * 此叶子分区内的冲突并以 "nothing"                       
	 * 代替报错。为此需预先初始化好                            
	 * ExecInsert() 推测插入所需的索引信息。        
	 */
	if (partrel->rd_rel->relhasindex &&
		leaf_part_rri->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(leaf_part_rri,
						(node != NULL &&
						 node->onConflictAction != ONCONFLICT_NONE));

	/*
	 * 为该分区构建 WITH CHECK OPTION 约束。                      
	 * 注意我们没有在规划器中为分区构建                       
	 * withCheckOptionList，但简单转换                                  
	 * varattno 即可。这只出现在 INSERT，                          
	 * 或 UPDATE/MERGE 路由无 rel。 
	 */
	if (node && node->withCheckOptionLists != NIL)
	{
		List	   *wcoList;
		List	   *wcoExprs = NIL;
		ListCell   *ll;

		/*
		 * 对分区表上的 INSERT，只存在一个计划。           
		 * 同样只有一个 WCO 列表，而非每分区一个。           
		 * 对 UPDATE/MERGE，WCO 列表与计划一样多。               
		 */
		Assert((node->operation == CMD_INSERT &&
				list_length(node->withCheckOptionLists) == 1 &&
				list_length(node->resultRelations) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->withCheckOptionLists) ==
				list_length(node->resultRelations)) ||
			   (node->operation == CMD_MERGE &&
				list_length(node->withCheckOptionLists) ==
				list_length(node->resultRelations)));

		/*
		 * 以第一个计划的 WCO 列表为参照，                  
		 * 来计算该分区 WCO 列表的属性号。INSERT              
		 * 情形下它指根分区表，而在 UPDATE 元组              
		 * 路由情形下它指 mtstate->resultRelInfo 数组          
		 * 中的第一个分区。无论如何，该关系与               
		 * 此分区应有相同的列，因此我们应当                 
		 * 能成功映射属性。       
		 */
		wcoList = linitial(node->withCheckOptionLists);

		/*
		 * 将其中的 Var 转换为该分区的属性号。               
		 */
		part_attmap =
			build_attrmap_by_name(RelationGetDescr(partrel),
								  RelationGetDescr(firstResultRel),
								  false);
		wcoList = (List *)
			map_variable_attnos((Node *) wcoList,
								firstVarno, 0,
								part_attmap,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);
		/* 忽略 found_whole_row 的值。        */

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = lfirst_node(WithCheckOption, ll);
			ExprState  *wcoExpr = ExecInitQual(castNode(List, wco->qual),
											   &mtstate->ps);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		leaf_part_rri->ri_WithCheckOptions = wcoList;
		leaf_part_rri->ri_WithCheckOptionExprs = wcoExprs;
	}

	/*
	 * 为该分区构建 RETURNING 投影。注意我们                    
	 * 没有在规划器中为分区构建                                 
	 * returningList，但简单转换 varattno 即                           
	 * 可。这只发生在 INSERT 情形，或未找                   
	 * 到可复用 rel。 
	 */
	if (node && node->returningLists != NIL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;
		List	   *returningList;

		/* 关于 WCO 列表见上文说明。  */
		/* (MERGE 则尚不支持 RETURNING)           */
		Assert((node->operation == CMD_INSERT &&
				list_length(node->returningLists) == 1 &&
				list_length(node->resultRelations) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->returningLists) ==
				list_length(node->resultRelations)));

		/*
		 * 以第一个计划的 RETURNING 列表为参照，        
		 * 来计算该分区 RETURNING 列表的属性号。              
		 * 为何可行，详见上文 WCO 列表的注                   
		 * 释  
		 */
		returningList = linitial(node->returningLists);

		/*
		 * 将其中的 Var 转换为该分区的属性号。               
		 */
		if (part_attmap == NULL)
			part_attmap =
				build_attrmap_by_name(RelationGetDescr(partrel),
									  RelationGetDescr(firstResultRel),
									  false);
		returningList = (List *)
			map_variable_attnos((Node *) returningList,
								firstVarno, 0,
								part_attmap,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);
		/* 忽略 found_whole_row 的值。        */

		leaf_part_rri->ri_returningList = returningList;

		/*
		 * 初始化投影本身。         
		 *
		 * 使用本会为 ExecInitModifyTable() 里的投影                 
		 * 输出而设的槽位与表达式上下文。    
		 */
		Assert(mtstate->ps.ps_ResultTupleSlot != NULL);
		slot = mtstate->ps.ps_ResultTupleSlot;
		Assert(mtstate->ps.ps_ExprContext != NULL);
		econtext = mtstate->ps.ps_ExprContext;
		leaf_part_rri->ri_projectReturning =
			ExecBuildProjectionInfo(returningList, econtext, slot,
									&mtstate->ps, RelationGetDescr(partrel));
	}

	/* 设置把元组路由到该分区所需的信息。            */
	ExecInitRoutingInfo(mtstate, estate, proute, dispatch,
						leaf_part_rri, partidx, false);

	/*
	 * 若存在 ON CONFLICT 子句，为其初始化状态。    
	 */
	if (node && node->onConflictAction != ONCONFLICT_NONE)
	{
		TupleDesc	partrelDesc = RelationGetDescr(partrel);
		ExprContext *econtext = mtstate->ps.ps_ExprContext;
		ListCell   *lc;
		List	   *arbiterIndexes = NIL;

		/*
		 * 若存在仲裁索引列表，将其映射为该                  
		 * 分区中的索引列表。做法是扫描该分               
		 * 区的索引列表，并为祖先表内每个索引               
		 * 寻祖系。   
		 */
		if (rootResultRelInfo->ri_onConflictArbiterIndexes != NIL)
		{
			List	   *childIdxs;

			childIdxs = RelationGetIndexList(leaf_part_rri->ri_RelationDesc);

			foreach(lc, childIdxs)
			{
				Oid			childIdx = lfirst_oid(lc);
				List	   *ancestors;
				ListCell   *lc2;

				ancestors = get_partition_ancestors(childIdx);
				foreach(lc2, rootResultRelInfo->ri_onConflictArbiterIndexes)
				{
					if (list_member_oid(ancestors, lfirst_oid(lc2)))
						arbiterIndexes = lappend_oid(arbiterIndexes, childIdx);
				}
				list_free(ancestors);
			}
		}

		/*
		 * 若所得两个列表长度不等，则出了问题（           
		 * 这不应发生，因为仲裁索引                            
		 * 不会选坏索引。）  
		 */
		if (list_length(rootResultRelInfo->ri_onConflictArbiterIndexes) !=
			list_length(arbiterIndexes))
			elog(ERROR, "invalid arbiter index list");
		leaf_part_rri->ri_onConflictArbiterIndexes = arbiterIndexes;

		/*
		 * 在 DO UPDATE 情形下，还需初始化更多状态。     
		 */
		if (node->onConflictAction == ONCONFLICT_UPDATE)
		{
			OnConflictSetState *onconfl = makeNode(OnConflictSetState);
			TupleConversionMap *map;

			map = ExecGetRootToChildMap(leaf_part_rri, estate);

			Assert(node->onConflictSet != NIL);
			Assert(rootResultRelInfo->ri_onConflict != NULL);

			leaf_part_rri->ri_onConflict = onconfl;

			/*
			 * 每个分区需要一个单独的既有槽位，        
			 * 因为即便元组描述符一致，该分区          
			 * 或属其他 AM。
			 */
			onconfl->oc_Existing =
				table_slot_create(leaf_part_rri->ri_RelationDesc,
								  &mtstate->ps.state->es_tupleTable);

			/*
			 * 若该分区的元组描述符与根父级完               
			 * 全一致（常见情形），我们可以复用父            
			 * 级的大部分 ON CONFLICT SET 状态，省去             
			 * 大量工作；否则需建分区专属状态。
			 */
			if (map == NULL)
			{
				/*
				 * 从分区根复用这些是安全的，因为我们    
				 * 一次只处理一个元组（因此不会          
				 * 覆盖槽位中尚需的数据），且投影的   
				 * 结果独立于底层存储。投影与 where         
				 * 子句本身不存储状态，且独立于底             
				 * 层存储。                                
				 */
				onconfl->oc_ProjSlot =
					rootResultRelInfo->ri_onConflict->oc_ProjSlot;
				onconfl->oc_ProjInfo =
					rootResultRelInfo->ri_onConflict->oc_ProjInfo;
				onconfl->oc_WhereClause =
					rootResultRelInfo->ri_onConflict->oc_WhereClause;
			}
			else
			{
				List	   *onconflset;
				List	   *onconflcols;

				/*
				 * 转换 onConflictSet 中的表达式，以适配     
				 * 不同的属性号。为此把分区 varattno        
				 * 映射两次：第一次捕捉 EXCLUDED     
				 * 伪关系（INNER_VAR），第二次处理主             
				 * 目标关系(firstVarno)。  
				 */
				onconflset = copyObject(node->onConflictSet);
				if (part_attmap == NULL)
					part_attmap =
						build_attrmap_by_name(RelationGetDescr(partrel),
											  RelationGetDescr(firstResultRel),
											  false);
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										INNER_VAR, 0,
										part_attmap,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);
				/* 忽略 found_whole_row 的值。        */
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										firstVarno, 0,
										part_attmap,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);
				/* 忽略 found_whole_row 的值。        */

				/* 最后，调整目标列号以匹配该分区。          */
				onconflcols = adjust_partition_colnos(node->onConflictCols,
													  leaf_part_rri);

				/* 为 UPDATE SET 投影创建元组槽                */
				onconfl->oc_ProjSlot =
					table_slot_create(partrel,
									  &mtstate->ps.state->es_tupleTable);

				/* 构建 UPDATE SET 投影状态    */
				onconfl->oc_ProjInfo =
					ExecBuildUpdateProjection(onconflset,
											  true,
											  onconflcols,
											  partrelDesc,
											  econtext,
											  onconfl->oc_ProjSlot,
											  &mtstate->ps);

				/*
				 * 若有 WHERE 子句，则在其求值处初始化状态，
				 * 并恰当地映射属性编号。                         
				 * 如同 onConflictSet 一样，需要把分区 varattnos  
				 * 映射到分区 tupdesc。 
				 */
				if (node->onConflictWhere)
				{
					List	   *clause;

					clause = copyObject((List *) node->onConflictWhere);
					clause = (List *)
						map_variable_attnos((Node *) clause,
											INNER_VAR, 0,
											part_attmap,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					/* 忽略 found_whole_row 的值。        */
					clause = (List *)
						map_variable_attnos((Node *) clause,
											firstVarno, 0,
											part_attmap,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					/* 忽略 found_whole_row 的值。        */
					onconfl->oc_WhereClause =
						ExecInitQual((List *) clause, &mtstate->ps);
				}
			}
		}
	}

	/*
	 * 由于我们刚初始化了 ResultRelInfo，它还未加入任何    
	 * 附加到 estate 的列表。请将其加入，以便日后查找。  
	 *
	 * 注意，列表中的条目没有预定顺序，因为分区结果  
	 * 关系只会在需要时才被初始化。                       
	 *        
	 */
	MemoryContextSwitchTo(estate->es_query_cxt);
	estate->es_tuple_routing_result_relations =
		lappend(estate->es_tuple_routing_result_relations,
				leaf_part_rri);

	/*
	 * 初始化处理 MERGE 所需的该分区信息。我们以首个   
	 * 结果关系 mergeActionList 为参照，为本关系制作     
	 * 副本，并转换其中引用的属性编号以匹配          
	 * 本关系的编号。                                 
	 *
	 * 此处重复了 ExecInitMerge() 的许多逻辑，若那边有变， 
	 * 请同时关注此处。     
	 */
	if (node && node->operation == CMD_MERGE)
	{
		List	   *firstMergeActionList = linitial(node->mergeActionLists);
		ListCell   *lc;
		ExprContext *econtext = mtstate->ps.ps_ExprContext;

		if (part_attmap == NULL)
			part_attmap =
				build_attrmap_by_name(RelationGetDescr(partrel),
									  RelationGetDescr(firstResultRel),
									  false);

		if (unlikely(!leaf_part_rri->ri_projectNewInfoValid))
			ExecInitMergeTupleSlots(mtstate, leaf_part_rri);

		foreach(lc, firstMergeActionList)
		{
			/* 为本关系制作副本以确保安全。 */
			MergeAction *action = copyObject(lfirst(lc));
			MergeActionState *action_state;
			List	  **list;

			/* 为本关系生成该动作的状态          */
			action_state = makeNode(MergeActionState);
			action_state->mas_action = action;

			/* 并把该动作放入相应的列表       */
			if (action->matched)
				list = &leaf_part_rri->ri_matchedMergeAction;
			else
				list = &leaf_part_rri->ri_notMatchedMergeAction;
			*list = lappend(*list, action_state);

			switch (action->commandType)
			{
				case CMD_INSERT:

					/*
					 * ExecCheckPlanOutput() 在首个结果关系初始化
					 * 时已对目标列表执行，且对所有结果关系 
					 * 均相同。             
					 */
					action_state->mas_proj =
						ExecBuildProjectionInfo(action->targetList, econtext,
												leaf_part_rri->ri_newTupleSlot,
												&mtstate->ps,
												RelationGetDescr(partrel));
					break;
				case CMD_UPDATE:

					/*
					 * "first" 结果关系 updateColnos 属性编号   
					 * 转换为本结果关系的编号。   
					 */
					if (part_attmap)
						action->updateColnos =
							adjust_partition_colnos_using_map(action->updateColnos,
															  part_attmap);
					action_state->mas_proj =
						ExecBuildUpdateProjection(action->targetList,
												  true,
												  action->updateColnos,
												  RelationGetDescr(leaf_part_rri->ri_RelationDesc),
												  econtext,
												  leaf_part_rri->ri_newTupleSlot,
												  NULL);
					break;
				case CMD_DELETE:
				case CMD_NOTHING:
					/* 无需处理  */
					break;

				default:
					elog(ERROR, "unknown action in MERGE WHEN clause");
			}

			/* 有意忽略 found_whole_row。        */
			action->qual =
				map_variable_attnos(action->qual,
									firstVarno, 0,
									part_attmap,
									RelationGetForm(partrel)->reltype,
									&found_whole_row);
			action_state->mas_whenqual =
				ExecInitQual((List *) action->qual, &mtstate->ps);
		}
	}
	MemoryContextSwitchTo(oldcxt);

	return leaf_part_rri;
}

/*
 * ExecInitRoutingInfo
 *		设置在根分区表格式与分区格式之间转换元组 
 *		所需的信息，并将其记录在                               
 *		PartitionTupleRouting。 
 */
static void
ExecInitRoutingInfo(ModifyTableState *mtstate,
					EState *estate,
					PartitionTupleRouting *proute,
					PartitionDispatch dispatch,
					ResultRelInfo *partRelInfo,
					int partidx,
					bool is_borrowed_rel)
{
	MemoryContext oldcxt;
	int			rri_index;

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	/*
	 * 若根父表与分区的行类型不同，则在两者间建立     
	 * 元组转换。若确需转换，还初始化一个专用于        
	 * 存放该分区已转换元组的槽。路由之后对元组     
	 * 施加的各种操作（例如检查约束）都将引用            
	 * 此槽。                                             
	 */
	if (ExecGetRootToChildMap(partRelInfo, estate) != NULL)
	{
		Relation	partrel = partRelInfo->ri_RelationDesc;

		/*
		 * 这会固定该分区的 TupleDesc，它将在命令结束时     
		 * 被释放。       
		 */
		partRelInfo->ri_PartitionTupleSlot =
			table_slot_create(partrel, &estate->es_tupleTable);
	}
	else
		partRelInfo->ri_PartitionTupleSlot = NULL;

	/*
	 * 若该分区是外部表，则让 FDW 自行初始化以向其  
	 * 路由元组。                 
	 */
	if (partRelInfo->ri_FdwRoutine != NULL &&
		partRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		partRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate, partRelInfo);

	/*
	 * 判断 FDW 是否支持批量插入，并确定批量大小            
	 * （FDW 或许支持批处理，但可能对服务器/表或
	 * 本查询禁用）。                      
	 *
	 * 若 FDW 不支持批处理，我们将批量大小设为 1。    
	 */
	if (partRelInfo->ri_FdwRoutine != NULL &&
		partRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
		partRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		partRelInfo->ri_BatchSize =
			partRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(partRelInfo);
	else
		partRelInfo->ri_BatchSize = 1;

	Assert(partRelInfo->ri_BatchSize >= 1);

	partRelInfo->ri_CopyMultiInsertBuffer = NULL;

	/*
	 * 将其记录在 PartitionTupleRouting->partitions 数组中。  
	 */
	Assert(dispatch->indexes[partidx] == -1);

	rri_index = proute->num_partitions++;

	/* 按需分配或扩大数组              */
	if (proute->num_partitions >= proute->max_partitions)
	{
		if (proute->max_partitions == 0)
		{
			proute->max_partitions = 8;
			proute->partitions = (ResultRelInfo **)
				palloc(sizeof(ResultRelInfo *) * proute->max_partitions);
			proute->is_borrowed_rel = (bool *)
				palloc(sizeof(bool) * proute->max_partitions);
		}
		else
		{
			proute->max_partitions *= 2;
			proute->partitions = (ResultRelInfo **)
				repalloc(proute->partitions, sizeof(ResultRelInfo *) *
						 proute->max_partitions);
			proute->is_borrowed_rel = (bool *)
				repalloc(proute->is_borrowed_rel, sizeof(bool) *
						 proute->max_partitions);
		}
	}

	proute->partitions[rri_index] = partRelInfo;
	proute->is_borrowed_rel[rri_index] = is_borrowed_rel;
	dispatch->indexes[partidx] = rri_index;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * ExecInitPartitionDispatchInfo
 *		锁定分区表（若尚未锁定）并初始化                 
 *		该分区表的 PartitionDispatch，并将它存放到            
 *		proute->partition_dispatch_info 数组的下一个可用           
 *		槽位中。同时在 parent_pd->indexes[] 数组的                 
 *		partidx 元素中记录该下标，以便日后正确取回新建的  
 *		PartitionDispatch。    
 */
static PartitionDispatch
ExecInitPartitionDispatchInfo(EState *estate,
							  PartitionTupleRouting *proute, Oid partoid,
							  PartitionDispatch parent_pd, int partidx,
							  ResultRelInfo *rootResultRelInfo)
{
	Relation	rel;
	PartitionDesc partdesc;
	PartitionDispatch pd;
	int			dispatchidx;
	MemoryContext oldcxt;

	/*
	 * 对于数据修改，最好让执行器不包含正在被分离的
	 * 分区，除非在快照隔离模式下运行。这意味着，读  
	 * 已提交事务在元组被插入到正被并发分离的分区       
	 * 时会立即收到 "no partition for tuple" 错误，而可   
	 * 重复读模式下的事务则仍可继续使用这样            
	 * 的分区。                                        
	 */
	if (estate->es_partition_directory == NULL)
		estate->es_partition_directory =
			CreatePartitionDirectory(estate->es_query_cxt,
									 !IsolationUsesXactSnapshot());

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	/*
	 * 只有子分区表需要在此处加锁。根分区表       
	 * 已被加过锁，因为它被引用在查询的                     
	 * rtable 中。      
	 */
	if (partoid != RelationGetRelid(proute->partition_root))
		rel = table_open(partoid, RowExclusiveLock);
	else
		rel = proute->partition_root;
	partdesc = PartitionDirectoryLookup(estate->es_partition_directory, rel);

	pd = (PartitionDispatch) palloc(offsetof(PartitionDispatchData, indexes) +
									partdesc->nparts * sizeof(int));
	pd->reldesc = rel;
	pd->key = RelationGetPartitionKey(rel);
	pd->keystate = NIL;
	pd->partdesc = partdesc;
	if (parent_pd != NULL)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);

		/*
		 * 对于列顺序与其直接父分区表不同的子分区表，   
		 * 我们必须存储一个用其元组描述符初始化的元组  
		 * 槽，以及一个元组转换映射，用来把元组从其父表 
		 * 的行类型转换为其自身的行类型。这样做是为了  
		 * 确保在为元组路由计算分区键时，以正确的         
		 * 元组描述符查看正确的行。                       
		 *         
		 */
		pd->tupmap = build_attrmap_by_name_if_req(RelationGetDescr(parent_pd->reldesc),
												  tupdesc,
												  false);
		pd->tupslot = pd->tupmap ?
			MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual) : NULL;
	}
	else
	{
		/* 根分区表无需此操作                 */
		pd->tupmap = NULL;
		pd->tupslot = NULL;
	}

	/*
	 * 初始化为 -1，表示相应分区的 ResultRelInfo 或       
	 * PartitionDispatch 尚未创建。                           
	 */
	memset(pd->indexes, -1, sizeof(int) * partdesc->nparts);

	/* 记录到 PartitionTupleRouting 以备后用 */
	dispatchidx = proute->num_dispatch++;

	/* 按需分配或扩大数组              */
	if (proute->num_dispatch >= proute->max_dispatch)
	{
		if (proute->max_dispatch == 0)
		{
			proute->max_dispatch = 4;
			proute->partition_dispatch_info = (PartitionDispatch *)
				palloc(sizeof(PartitionDispatch) * proute->max_dispatch);
			proute->nonleaf_partitions = (ResultRelInfo **)
				palloc(sizeof(ResultRelInfo *) * proute->max_dispatch);
		}
		else
		{
			proute->max_dispatch *= 2;
			proute->partition_dispatch_info = (PartitionDispatch *)
				repalloc(proute->partition_dispatch_info,
						 sizeof(PartitionDispatch) * proute->max_dispatch);
			proute->nonleaf_partitions = (ResultRelInfo **)
				repalloc(proute->nonleaf_partitions,
						 sizeof(ResultRelInfo *) * proute->max_dispatch);
		}
	}
	proute->partition_dispatch_info[dispatchidx] = pd;

	/*
	 * 若为子分区表建立 PartitionDispatch，则可能还需         
	 * 要一个最低限度有效的 ResultRelInfo，以便日后         
	 * 检查分区约束。             
	 */
	if (parent_pd)
	{
		ResultRelInfo *rri = makeNode(ResultRelInfo);

		InitResultRelInfo(rri, rel, 0, rootResultRelInfo, 0);
		proute->nonleaf_partitions[dispatchidx] = rri;
	}
	else
		proute->nonleaf_partitions[dispatchidx] = NULL;

	/*
	 * 最后，若为子分区表建立 PartitionDispatch，则还要        
	 * 在父表中安装 downlink，以便快速下行。     
	 */
	if (parent_pd)
	{
		Assert(parent_pd->indexes[partidx] == -1);
		parent_pd->indexes[partidx] = dispatchidx;
	}

	MemoryContextSwitchTo(oldcxt);

	return pd;
}

/*
 * ExecCleanupTupleRouting -- 清理为分区元组路由分配的          
 * 对象  
 *
 * 关闭所有分区表、叶分区及其索引。                     
 */
void
ExecCleanupTupleRouting(ModifyTableState *mtstate,
						PartitionTupleRouting *proute)
{
	int			i;

	/*
	 * 记住，proute->partition_dispatch_info[0] 对应根分区         
	 * 表，绝不能尝试将其关闭，因为它是查询的主目标  
	 * 表，将由 ExecEndPlan() 或 DoCopy() 等调用者关闭。        
	 * 另外，根分区表的 tupslot                             
	 * 为 NULL。       
	 */
	for (i = 1; i < proute->num_dispatch; i++)
	{
		PartitionDispatch pd = proute->partition_dispatch_info[i];

		table_close(pd->reldesc, NoLock);

		if (pd->tupslot)
			ExecDropSingleTupleTableSlot(pd->tupslot);
	}

	for (i = 0; i < proute->num_partitions; i++)
	{
		ResultRelInfo *resultRelInfo = proute->partitions[i];

		/* 允许各 FDW 关闭        */
		if (resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->EndForeignInsert != NULL)
			resultRelInfo->ri_FdwRoutine->EndForeignInsert(mtstate->ps.state,
														   resultRelInfo);

		/*
		 * 若它不是从所属 ModifyTableState 借来的结果关系，   
		 * 则关闭它；那些关系将由 ExecEndPlan() 关闭。      
		 */
		if (proute->is_borrowed_rel[i])
			continue;

		ExecCloseIndices(resultRelInfo);
		table_close(resultRelInfo->ri_RelationDesc, NoLock);
	}
}

/* ----------------
 *		FormPartitionKeyDatum
 *			为元组的分区键构造 values[] 与 isnull[]           
 *			数组。  
 *
 *	pd				分区表的 PartitionDispatch 对象             
 *	slot			从中提取分区键的堆元组             
 *	estate			对分区键表达式求值的执行器状态  
 *					（必须非 NULL）。       
 *	values			分区键 Datum 数组（输出区）      
 *	isnull			空值指示数组（输出区）        
 *
 * estate 的每元组表达式上下文的 ecxt_scantuple 槽必须指向 
 * 传入的堆元组。    
 * ----------------
 */
static void
FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull)
{
	ListCell   *partexpr_item;
	int			i;

	if (pd->key->partexprs != NIL && pd->keystate == NIL)
	{
		/* 检查调用者已正确设置上下文   */
		Assert(estate != NULL &&
			   GetPerTupleExprContext(estate)->ecxt_scantuple == slot);

		/* 首次执行时，建立表达式求值状态          */
		pd->keystate = ExecPrepareExprList(pd->key->partexprs, estate);
	}

	partexpr_item = list_head(pd->keystate);
	for (i = 0; i < pd->key->partnatts; i++)
	{
		AttrNumber	keycol = pd->key->partattrs[i];
		Datum		datum;
		bool		isNull;

		if (keycol != 0)
		{
			/* 普通列：直接从堆元组中取值                  */
			datum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/* 表达式：需要求值        */
			if (partexpr_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			datum = ExecEvalExprSwitchContext((ExprState *) lfirst(partexpr_item),
											  GetPerTupleExprContext(estate),
											  &isNull);
			partexpr_item = lnext(pd->keystate, partexpr_item);
		}
		values[i] = datum;
		isnull[i] = isNull;
	}

	if (partexpr_item != NULL)
		elog(ERROR, "wrong number of partition key expressions");
}

/*
 * 连续找到同一分区的次数阈值，超过该次数后，我们  
 * 就不再对给定值做二分查找，而是只检查这些值是否   
 * 属于最后找到的分区。该值必须大于 0。            
 */
#define PARTITION_CACHED_FIND_THRESHOLD			16

/*
 * get_partition_for_tuple
 *		找到关系中接受 values 与 isnull 所指定分区键的那个  
 *		分区。            
 *
 * 当 LIST 与 RANGE 分区表有很多分区时，调用此函数
 * 的代价可能相当高昂，原因是其中为找到正确分区而     
 * 进行的二分查找。LIST 与 RANGE 分区表的许多用例           
 * 都使得同一分区很可能在后续的 ExecFindPartition()        
 * 调用中再次被找到。对于像基于 TIMESTAMP 列、分区          
 * 键为当前时间的 RANGE 分区表这类场景而言尤其如此。
 * 当被要求为 RANGE 或 LIST 分区表查找分区时，我们会     
 * 把已找到的分区下标以及给定 values 在 PartitionDesc        
 * （存储于 relcache 中）中找到的 datum 偏移量记录下来， 
 * 如果我们连续多次找到同一分区，达到           
 * PARTITION_CACHED_FIND_THRESHOLD 次，便会启用缓存                 
 * 逻辑，此后不再执行二分查找来找到正确分区，而 
 * 是只复查 values 是否仍属于最后找到的分区；若是，     
 * 就返回该分区下标，省去二分查找。若复查时未能 
 * 匹配最后那个分区，则回退为执行二分查找。          
 * 在这种情况下，除非发现 values 属于 DEFAULT 分区，          
 * 否则我们都会重置命中同一分区的次数计数，这样一 
 * 来在我们再次连续找到该分区至少达到                    
 * PARTITION_CACHED_FIND_THRESHOLD 次之前，就不会再                
 * 尝试使用缓存了。                             
 *
 * 对于每次查找时分区都变化的情形，额外工作只是 
 * 相当于记录最后找到的分区及其边界偏移，然后重置      
 * 找到计数而已。这开销很小，对此类情形不会造成        
 * 任何明显的减速。                                    
 *
 * 当最后找到的分区是 DEFAULT 或 NULL 分区时，我们不   
 * 对分区做缓存。就 DEFAULT 分区而言，没有存储匹配      
 * datum 的边界偏移量，因此无法确认下标是否匹配。     
 * 至于 NULL 分区，其开销本就极低，                            
 * 不必缓存。  
 *
 * 返回值是分区的下标（>= 0 且 < partdesc->nparts），若         
 * 找到；否则为 -1。  
 */
static int
get_partition_for_tuple(PartitionDispatch pd, Datum *values, bool *isnull)
{
	int			bound_offset = -1;
	int			part_index = -1;
	PartitionKey key = pd->key;
	PartitionDesc partdesc = pd->partdesc;
	PartitionBoundInfo boundinfo = partdesc->boundinfo;

	/*
	 * 在下面的 switch 语句中，当我们对 RANGE 与 LIST 分区
	 * 表执行缓存查找时，如果发现最后找到的分区与  
	 * values 匹配，就会立即返回该分区下标。我们这样做
	 * 而不是跳出 switch，因为我们不想执行有关 DEFAULT    
	 * 分区的代码，也不想去更新任何与缓存相关的字段， 
	 * 因为那样做是徒劳无功的：我们已经知道它并不是   
	 * DEFAULT 分区，而且无需将同一分区的命中次数增加    
	 * 到超过                                                           
	 * PARTITION_CACHED_FIND_THRESHOLD。   
	 */

	/* 根据分区策略选择相应的路由方式。     */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				uint64		rowHash;

				/* 哈希分区代价极低，无需缓存          */
				rowHash = compute_partition_hash_value(key->partnatts,
													   key->partsupfunc,
													   key->partcollation,
													   values, isnull);

				/*
				 * HASH 分区不能拥有 DEFAULT 分区，而且我们不   
				 * 为它们做缓存工作，直接返回分区下标即可  
				 */
				return boundinfo->indexes[rowHash % boundinfo->nindexes];
			}

		case PARTITION_STRATEGY_LIST:
			if (isnull[0])
			{
				/* 它同样代价极低，不值得做任何缓存  */
				if (partition_bound_accepts_nulls(boundinfo))
				{
					/*
					 * 有 NULL 分区时，我们直接返回它。     
					 * 由于没有 bound_offset，落入 switch 之后的
					 * 检查并更新缓存字段的代码不合适。  
					 * 或许应该让最后缓存分区的详情失效，    
					 * 但并无实际必要。让这些缓存       
					 * 字段保持设置，反而给下次查找提供了  
					 * 匹配到缓存分区的机会，所以无需额外
					 * 处理。                    
					 */
					return boundinfo->null_index;
				}
			}
			else
			{
				bool		equal;

				if (partdesc->last_found_count >= PARTITION_CACHED_FIND_THRESHOLD)
				{
					int			last_datum_offset = partdesc->last_found_datum_index;
					Datum		lastDatum = boundinfo->datums[last_datum_offset][0];
					int32		cmpval;

					/* 上次 datum 下标与此 datum 匹配吗？      */
					cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
															 key->partcollation[0],
															 lastDatum,
															 values[0]));

					if (cmpval == 0)
						return boundinfo->indexes[last_datum_offset];

					/* fall-through and do a manual lookup */
				}

				bound_offset = partition_list_bsearch(key->partsupfunc,
													  key->partcollation,
													  boundinfo,
													  values[0], &equal);
				if (bound_offset >= 0 && equal)
					part_index = boundinfo->indexes[bound_offset];
			}
			break;

		case PARTITION_STRATEGY_RANGE:
			{
				bool		equal = false,
							range_partkey_has_null = false;
				int			i;

				/*
				 * 没有范围包含 NULL，因此它会被默认分区  
				 * 接受（若存在），否则被拒绝。                
				 */
				for (i = 0; i < key->partnatts; i++)
				{
					if (isnull[i])
					{
						range_partkey_has_null = true;
						break;
					}
				}

				/* NULL 应归入 DEFAULT 分区         */
				if (range_partkey_has_null)
					break;

				if (partdesc->last_found_count >= PARTITION_CACHED_FIND_THRESHOLD)
				{
					int			last_datum_offset = partdesc->last_found_datum_index;
					Datum	   *lastDatums = boundinfo->datums[last_datum_offset];
					PartitionRangeDatumKind *kind = boundinfo->kind[last_datum_offset];
					int32		cmpval;

					/* 检查该值是否 >= 下界                */
					cmpval = partition_rbound_datum_cmp(key->partsupfunc,
														key->partcollation,
														lastDatums,
														kind,
														values,
														key->partnatts);

					/*
					 * 若该值等于下界，则无需检查               
					 * 上界。       
					 */
					if (cmpval == 0)
						return boundinfo->indexes[last_datum_offset + 1];

					if (cmpval < 0 && last_datum_offset + 1 < boundinfo->ndatums)
					{
						/* 检查该值是否低于上界              */
						lastDatums = boundinfo->datums[last_datum_offset + 1];
						kind = boundinfo->kind[last_datum_offset + 1];
						cmpval = partition_rbound_datum_cmp(key->partsupfunc,
															key->partcollation,
															lastDatums,
															kind,
															values,
															key->partnatts);

						if (cmpval > 0)
							return boundinfo->indexes[last_datum_offset + 1];
					}
					/* fall-through and do a manual lookup */
				}

				bound_offset = partition_range_datum_bsearch(key->partsupfunc,
															 key->partcollation,
															 boundinfo,
															 key->partnatts,
															 values,
															 &equal);

				/*
				 * bound_offset 处的边界不高于元组值，因此  
				 * offset+1 处的边界就是要找分区的上界，        
				 * 前提是该分区确实存在。                        
				 *     
				 */
				part_index = boundinfo->indexes[bound_offset + 1];
			}
			break;

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	/*
	 * part_index < 0 表示我们未能找到此父表的分区。此时    
	 * 则使用默认分区（若存在）。
	 */
	if (part_index < 0)
	{
		/*
		 * 此处无需重置缓存字段。下一组 values 仍可能    
		 * 属于缓存的分区，因此保留缓存可以提高下次  
		 * 查找时命中缓存的机会。                                  
		 */
		return boundinfo->default_index;
	}

	/* 只有上方代码设置了 bound_offset 时才会到此处      */
	Assert(bound_offset >= 0);

	/*
	 * 处理缓存字段。若 bound_offset 与上次缓存的边界偏
	 * 移量一致，那么我们找到的分区与上次相同，把计   
	 * 数加一。如果一切顺利，最终会达到                   
	 * PARTITION_CACHED_FIND_THRESHOLD，下次走缓存路径。      
	 * 否则，把缓存计数重置回 1，表示这是第一次找到     
	 * 该分区。                                  
	 */
	if (bound_offset == partdesc->last_found_datum_index)
		partdesc->last_found_count++;
	else
	{
		partdesc->last_found_count = 1;
		partdesc->last_found_part_index = part_index;
		partdesc->last_found_datum_index = bound_offset;
	}

	return part_index;
}

/*
 * ExecBuildSlotPartitionKeyDescription
 *
 * 其工作方式与 BuildIndexValueDescription() 非常相似，目前  
 * 用于在 ExecFindPartition() 未能为某行找到分区时构建     
 * 错误消息。     
 */
static char *
ExecBuildSlotPartitionKeyDescription(Relation rel,
									 Datum *values,
									 bool *isnull,
									 int maxfieldlen)
{
	StringInfoData buf;
	PartitionKey key = RelationGetPartitionKey(rel);
	int			partnatts = get_partition_natts(key);
	int			i;
	Oid			relid = RelationGetRelid(rel);
	AclResult	aclresult;

	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	/* 若用户拥有表级权限，就直接去构建描述信息。    */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/*
		 * 逐一遍历分区键的各个列，确保用户对它们      
		 * 全部拥有 SELECT 权限。         
		 */
		for (i = 0; i < partnatts; i++)
		{
			AttrNumber	attnum = get_partition_col_attnum(key, i);

			/*
			 * 若此分区键列为表达式，就不返回细节，     
			 * 而不是去分析表达式包含哪些列、          
			 * 以及用户是否拥有其 SELECT 权限。                  
			 */
			if (attnum == InvalidAttrNumber ||
				pg_attribute_aclcheck(relid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
				return NULL;
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s) = (",
					 pg_get_partkeydef_columns(relid, true));

	for (i = 0; i < partnatts; i++)
	{
		char	   *val;
		int			vallen;

		if (isnull[i])
			val = "null";
		else
		{
			Oid			foutoid;
			bool		typisvarlena;

			getTypeOutputInfo(get_partition_col_typid(key, i),
							  &foutoid, &typisvarlena);
			val = OidOutputFunctionCall(foutoid, values[i]);
		}

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		/* 需要时截断    */
		vallen = strlen(val);
		if (vallen <= maxfieldlen)
			appendBinaryStringInfo(&buf, val, vallen);
		else
		{
			vallen = pg_mbcliplen(val, vallen, maxfieldlen);
			appendBinaryStringInfo(&buf, val, vallen);
			appendStringInfoString(&buf, "...");
		}
	}

	appendStringInfoChar(&buf, ')');

	return buf.data;
}

/*
 * adjust_partition_colnos
 *		调整 UPDATE 目标列号列表，以顾及父表与分区   
 *		之间的属性差异。                                   
 *
 * 注意：无需调整时绝不可调用。           
 */
static List *
adjust_partition_colnos(List *colnos, ResultRelInfo *leaf_part_rri)
{
	TupleConversionMap *map = ExecGetChildToRootMap(leaf_part_rri);

	Assert(map != NULL);

	return adjust_partition_colnos_using_map(colnos, map->attrMap);
}

/*
 * adjust_partition_colnos_using_map
 *		类似 adjust_partition_colnos，但使用调用者提供的        
 *		映射，而非假定源自 "root" 结果关系。 
 *
 * 注意：无需调整时绝不可调用。           
 */
static List *
adjust_partition_colnos_using_map(List *colnos, AttrMap *attrMap)
{
	List	   *new_colnos = NIL;
	ListCell   *lc;

	Assert(attrMap != NULL);	/* 否则不应走到这里  */

	foreach(lc, colnos)
	{
		AttrNumber	parentattrno = lfirst_int(lc);

		if (parentattrno <= 0 ||
			parentattrno > attrMap->maplen ||
			attrMap->attnums[parentattrno - 1] == 0)
			elog(ERROR, "unexpected attno %d in target column list",
				 parentattrno);
		new_colnos = lappend_int(new_colnos,
								 attrMap->attnums[parentattrno - 1]);
	}

	return new_colnos;
}

/*-------------------------------------------------------------------------
 * 运行时分区剪枝支持。     
 *
 * 以下一系列函数用于支持移除分区表查询中不需要的     
 * 子计划。这里的支撑函数设计为可与任何支持任意        
 * 数量子计划的计划类型协同工作。                           
 * 例如 Append、MergeAppend。               
 *
 * 剪枝若涉及分区键与常量的比较，则由规划器完成；  
 * 但若比较对象是非常量而又非易变的表达式，            
 * 执行器便有机会在运行时进行剪枝，从而允许          
 * 动态地跳过无关的                                             
 * 分区。   
 *
 * 我们必须区分包含 PARAM_EXEC 参数的表达式与不     
 * 包含它们的表达式。尽管 PARAM_EXEC 参数被视为稳定      
 * 表达式，但其值可能随查询执行期间计划节点           
 * 扫描的推进而改变。不涉及此类参数的稳定比较 
 * 表达式允许分区剪枝在执行器启动时一次性完成。      
 * 而涉及此类参数的表达式，则要求我们针对父计划节点   
 * 的每次扫描分别进行剪枝。                                 
 *
 * 注意，启动时剪除不需要的子计划还带来额外好处，   
 * 即根本无需初始化那些不需要的子计划。                 
 *
 *
 * 函数： 
 *
 * ExecInitPartitionPruning:
 *		创建 ExecFindMatchingSubPlans 所需的 PartitionPruneState。     
 *		存储的细节包括如何把剪枝代码返回的分区索引映射
 *		为子计划索引。同时确定考虑到初始剪枝结果后      
 *		需要初始化的子计划集合。PartitionPruneState 中的        
 *		映射则会被更新，以顾及初始剪枝                    
 *		可能已经剔除了部分子计划。                           
 *		         
 *
 * ExecFindMatchingSubPlans:
 *		在求值给定位置可安全求值的表达式之后，返回匹配
 *		子计划的索引。本函数首先在                            
 *		ExecInitPartitionPruning() 期间被调用，基于初始     
 *		剪枝步骤的结果找出初始匹配的子计划，之后每当 
 *		PartitionPruneState 的 'execparamids' 中列出的某            
 *		个 Param 变化时须再次调用。        
 *-------------------------------------------------------------------------
 */

/*
 * ExecInitPartitionPruning
 *		初始化运行时分区剪枝所需的数据结构，             
 *		需要时执行初始剪枝 
 *
 * 返回时，*initially_valid_subplans 被赋予必须连同父计划  
 * 节点一起初始化的子计划索引集合。初始剪枝在需要时
 * 会在此处执行，此时只有存活子计划的                  
 * 索引被加入。                    
 *
 * 如果子计划确实被剪除，返回的 PartitionPruneState 中的      
 * subplan_map 数组会被重新编号以不计入它们，不过仅当      
 * 后续执行期剪枝还需要这些映射时才会如此。
 */
PartitionPruneState *
ExecInitPartitionPruning(PlanState *planstate,
						 int n_total_subplans,
						 PartitionPruneInfo *pruneinfo,
						 Bitmapset **initially_valid_subplans)
{
	PartitionPruneState *prunestate;
	EState	   *estate = planstate->state;

	/* 可能需要表达式上下文来求值分区表达式        */
	ExecAssignExprContext(estate, planstate);

	/* 为剪枝创建工作数据结构             */
	prunestate = CreatePartitionPruneState(planstate, pruneinfo);

	/*
	 * 如果需要，执行一次初始分区剪枝。     
	 */
	if (prunestate->do_initial_prune)
		*initially_valid_subplans = ExecFindMatchingSubPlans(prunestate, true);
	else
	{
		/* 无需剪枝，因此要初始化所有子计划     */
		Assert(n_total_subplans > 0);
		*initially_valid_subplans = bms_add_range(NULL, 0,
												  n_total_subplans - 1);
	}

	/*
	 * 重新编号 prunestate 中的子计划索引，以顾及上面因    
	 * 初始剪枝而移除的那些。若无步骤被移除，则无需    
	 * 这样做。          
	 */
	if (bms_num_members(*initially_valid_subplans) < n_total_subplans)
	{
		/*
		 * !do_exec_prune 时可安全跳过这一步，尽管那会在 
		 * prunestate 中留下无效数据，因为这些数据不再  
		 * 查阅（见 ExecFindMatchingSubPlans 开头的 Assert）。    
		 */
		if (prunestate->do_exec_prune)
			PartitionPruneFixSubPlanMap(prunestate,
										*initially_valid_subplans,
										n_total_subplans);
	}

	return prunestate;
}

/*
 * CreatePartitionPruneState
 *		构建调用 ExecFindMatchingSubPlans 所需的数据结构           
 *
 * 'planstate' 是父计划节点的执行状态。      
 *
 * 'pruneinfo' 是一个 PartitionPruneInfo，由     
 * make_partition_pruneinfo 生成。这里构建 PartitionPruneState，      
 * 其中为每个分区层级（即 pruneinfo->prune_infos 的               
 * 各子列表）包含一个 PartitionPruningData，而其中的            
 * 每个 PartitionPruningData 又为该子列表中出现的每个           
 * PartitionedRelPruneInfo 各包含一个 PartitionedRelPruningData。    
 * 若 UNION ALL 以多个分区表为子节点，就需要这套两级 
 * 结构来避免混淆不同的层级。存储在每个              
 * PartitionedRelPruningData 里的数据，可在每次判定哪些    
 * 分区匹配时复用。
 */
static PartitionPruneState *
CreatePartitionPruneState(PlanState *planstate, PartitionPruneInfo *pruneinfo)
{
	EState	   *estate = planstate->state;
	PartitionPruneState *prunestate;
	int			n_part_hierarchies;
	ListCell   *lc;
	int			i;
	ExprContext *econtext = planstate->ps_ExprContext;

	/* 数据读取时，执行器总是包含已分离的分区      */
	if (estate->es_partition_directory == NULL)
		estate->es_partition_directory =
			CreatePartitionDirectory(estate->es_query_cxt, false);

	n_part_hierarchies = list_length(pruneinfo->prune_infos);
	Assert(n_part_hierarchies > 0);

	/*
	 * 分配该数据结构      
	 */
	prunestate = (PartitionPruneState *)
		palloc(offsetof(PartitionPruneState, partprunedata) +
			   sizeof(PartitionPruningData *) * n_part_hierarchies);

	prunestate->execparamids = NULL;
	/* other_subplans 会在运行时改变，需自留一份副本  */
	prunestate->other_subplans = bms_copy(pruneinfo->other_subplans);
	prunestate->do_initial_prune = false;	/* 下面会设置  */
	prunestate->do_exec_prune = false;	/* 下面会设置  */
	prunestate->num_partprunedata = n_part_hierarchies;

	/*
	 * 创建一个短期内存上下文，供我们调用分区剪枝        
	 * 函数时使用。这可避免内存泄漏风险，因为剪枝     
	 * 函数会调用一些我们无法控制的比较函数，为           
	 * 此而设。
	 */
	prunestate->prune_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "Partition Prune",
							  ALLOCSET_DEFAULT_SIZES);

	i = 0;
	foreach(lc, pruneinfo->prune_infos)
	{
		List	   *partrelpruneinfos = lfirst_node(List, lc);
		int			npartrelpruneinfos = list_length(partrelpruneinfos);
		PartitionPruningData *prunedata;
		ListCell   *lc2;
		int			j;

		prunedata = (PartitionPruningData *)
			palloc(offsetof(PartitionPruningData, partrelprunedata) +
				   npartrelpruneinfos * sizeof(PartitionedRelPruningData));
		prunestate->partprunedata[i] = prunedata;
		prunedata->num_partrelprunedata = npartrelpruneinfos;

		j = 0;
		foreach(lc2, partrelpruneinfos)
		{
			PartitionedRelPruneInfo *pinfo = lfirst_node(PartitionedRelPruneInfo, lc2);
			PartitionedRelPruningData *pprune = &prunedata->partrelprunedata[j];
			Relation	partrel;
			PartitionDesc partdesc;
			PartitionKey partkey;

			/*
			 * 可以信赖分区表 relcache 条目中的分区键与分区 
			 * 描述符副本，因为在该次执行器运行期间，该 
			 * 条目会被一直保持打开并加锁，直至本次 
			 * 执行器运行结束。      
			 */
			partrel = ExecGetRangeTableRelation(estate, pinfo->rtindex);
			partkey = RelationGetPartitionKey(partrel);
			partdesc = PartitionDirectoryLookup(estate->es_partition_directory,
												partrel);

			/*
			 * 初始化 subplan_map 与 subpart_map。   
			 *
			 * 现在存在的分区集合可能与制定计划时所存在的
			 * 并不相同。常规情况是相同的；此时通过快速比
			 * 较来优化该情况，只需复制 subplan_map 并让      
			 * subpart_map 指向 PruneInfo 中的那一               
			 * 份。    
			 *
			 * 若两者不同，任一侧的分区都可能更多；甚至
			 * 两侧数量完全相同但 OID 集合不完全匹配。     
			 * 对此的应对办法，是新建 subplan_map 与            
			 * subpart_map 数组，使其对应 PruneInfo 中那些与    
			 * 新分区描述符 OID 匹配的条目；不匹配者 
			 * 则可设为 -1，如同已被剪除。按照构造，两个   
			 * 数组都按分区的边界顺序排列，这是构造所
			 * 保证的。                         
			 */
			pprune->nparts = partdesc->nparts;
			pprune->subplan_map = palloc(sizeof(int) * partdesc->nparts);

			if (partdesc->nparts == pinfo->nparts &&
				memcmp(partdesc->oids, pinfo->relid_map,
					   sizeof(int) * partdesc->nparts) == 0)
			{
				pprune->subpart_map = pinfo->subpart_map;
				memcpy(pprune->subplan_map, pinfo->subplan_map,
					   sizeof(int) * pinfo->nparts);
			}
			else
			{
				int			pd_idx = 0;
				int			pp_idx;

				/*
				 * 分区数组并不相同时，可能出现新的分区，  
				 * 但也很可能有一个分区被移除；为此我们通 
				 * 过遍历数组的办法来兼顾这两种情形。   
				 * 即丢弃其中不匹配的项。 
				 *
				 * 若两侧分区数量相同，仍可能有一个分区被  
				 * 分离而另一个被挂载。可创建一个映射，   
				 * 让它跳过其中任何不匹配的                      
				 * 之处。  
				 */
				pprune->subpart_map = palloc(sizeof(int) * partdesc->nparts);

				for (pp_idx = 0; pp_idx < partdesc->nparts; pp_idx++)
				{
					/* 跳过 InvalidOid 的 relid_map 条目*/
					while (pd_idx < pinfo->nparts &&
						   !OidIsValid(pinfo->relid_map[pd_idx]))
						pd_idx++;

			recheck:
					if (pd_idx < pinfo->nparts &&
						pinfo->relid_map[pd_idx] == partdesc->oids[pp_idx])
					{
						/* 匹配…*/
						pprune->subplan_map[pp_idx] =
							pinfo->subplan_map[pd_idx];
						pprune->subpart_map[pp_idx] =
							pinfo->subpart_map[pd_idx];
						pd_idx++;
						continue;
					}

					/*
					 * 两个数组对应位置并无完全匹配。  
					 * 向前翻查 pinfo->relid_map，看     
					 * partdesc 中当前分区有无匹配项。        
					 * 正常情况下，若有匹配，通常就在前方
					 * 仅一个元素的位置；这说明规划器当时 
					 * 多看了一个分区，如今我们已看不到它  
					 * 了（其并发分离恰在此间完成）；于是  
					 * 我们把 pd_idx 改到新位置并跳           
					 * 回上方来跳过它。这样，跳过无匹配的 
					 * OID 之后，我们仍然可以继续去匹配其余 
					 * 的元素；但不会为被跳过项尝试任何  
					 * 后续匹配，因为我们已经知道这两          
					 * 数组顺序一致。
					 *
					 * 若在 pinfo->relid_map 其余部分中            
					 * 都找不到匹配，说明现在看到的某个    
					 * 元素是规划器没见过的，标为已剪枝。 
					 * 随后继续。    
					 */
					for (int pd_idx2 = pd_idx + 1; pd_idx2 < pinfo->nparts; pd_idx2++)
					{
						if (pd_idx2 >= pinfo->nparts)
							break;
						if (pinfo->relid_map[pd_idx2] == partdesc->oids[pp_idx])
						{
							pd_idx = pd_idx2;
							goto recheck;
						}
					}

					pprune->subpart_map[pp_idx] = -1;
					pprune->subplan_map[pp_idx] = -1;
				}
			}

			/* present_parts 之后也可能被进一步修改     */
			pprune->present_parts = bms_copy(pinfo->present_parts);

			/*
			 * 按需初始化剪枝上下文。注意，在 EXPLAIN         
			 *  (GENERIC_PLAN) 中必须跳过执行期分区剪枝，    
			 * 因为参数值可能缺少。        
			 */
			pprune->initial_pruning_steps = pinfo->initial_pruning_steps;
			if (pinfo->initial_pruning_steps &&
				!(econtext->ecxt_estate->es_top_eflags & EXEC_FLAG_EXPLAIN_GENERIC))
			{
				InitPartitionPruneContext(&pprune->initial_context,
										  pinfo->initial_pruning_steps,
										  partdesc, partkey, planstate,
										  econtext);
				/* 记录任何层级是否需要初始剪枝            */
				prunestate->do_initial_prune = true;
			}
			pprune->exec_pruning_steps = pinfo->exec_pruning_steps;
			if (pinfo->exec_pruning_steps &&
				!(econtext->ecxt_estate->es_top_eflags & EXEC_FLAG_EXPLAIN_GENERIC))
			{
				InitPartitionPruneContext(&pprune->exec_context,
										  pinfo->exec_pruning_steps,
										  partdesc, partkey, planstate,
										  econtext);
				/* 记录任何层级是否需要执行期剪枝      */
				prunestate->do_exec_prune = true;
			}

			/*
			 * 把影响本计划节点分区决策的全部 PARAM_EXEC 
			 *  参数的 ID 都累加起来。         
			 */
			prunestate->execparamids = bms_add_members(prunestate->execparamids,
													   pinfo->execparamids);

			j++;
		}
		i++;
	}

	return prunestate;
}

/*
 * 为给定的剪枝步骤列表初始化一个 PartitionPruneContext。 
 */
static void
InitPartitionPruneContext(PartitionPruneContext *context,
						  List *pruning_steps,
						  PartitionDesc partdesc,
						  PartitionKey partkey,
						  PlanState *planstate,
						  ExprContext *econtext)
{
	int			n_steps;
	int			partnatts;
	ListCell   *lc;

	n_steps = list_length(pruning_steps);

	context->strategy = partkey->strategy;
	context->partnatts = partnatts = partkey->partnatts;
	context->nparts = partdesc->nparts;
	context->boundinfo = partdesc->boundinfo;
	context->partcollation = partkey->partcollation;
	context->partsupfunc = partkey->partsupfunc;

	/* 我们会按需查找特定类型的支持函数        */
	context->stepcmpfuncs = (FmgrInfo *)
		palloc0(sizeof(FmgrInfo) * n_steps * partnatts);

	context->ppccontext = CurrentMemoryContext;
	context->planstate = planstate;
	context->exprcontext = econtext;

	/* 为每个需要的表达式初始化表达式状态     */
	context->exprstates = (ExprState **)
		palloc0(sizeof(ExprState *) * n_steps * partnatts);
	foreach(lc, pruning_steps)
	{
		PartitionPruneStepOp *step = (PartitionPruneStepOp *) lfirst(lc);
		ListCell   *lc2 = list_head(step->exprs);
		int			keyno;

		/* 其他步骤种类不需要     */
		if (!IsA(step, PartitionPruneStepOp))
			continue;

		Assert(list_length(step->exprs) <= partnatts);

		for (keyno = 0; keyno < partnatts; keyno++)
		{
			if (bms_is_member(keyno, step->nullkeys))
				continue;

			if (lc2 != NULL)
			{
				Expr	   *expr = lfirst(lc2);

				/* 对 Consts 不需要  */
				if (!IsA(expr, Const))
				{
					int			stateidx = PruneCxtStateIdx(partnatts,
															step->step.step_id,
															keyno);

					/*
					 * 当 planstate 为 NULL 时，pruning_steps 已知    
					 * 不含依赖父计划的表达式。此时所有可用 
					 * 的 EXTERN 参数信息须显式传入，调用者应 
					 * 已经通过 econtext 使其                         
					 * 可用。                        
					 */
					if (planstate == NULL)
						context->exprstates[stateidx] =
							ExecInitExprWithParams(expr,
												   econtext->ecxt_param_list_info);
					else
						context->exprstates[stateidx] =
							ExecInitExpr(expr, context->planstate);
				}
				lc2 = lnext(step->exprs, lc2);
			}
		}
	}
}

/*
 * PartitionPruneFixSubPlanMap
 *		修正 prunestate 中的分区索引到子计划索引映       
 *		射，依据是初始剪枝后存活下来的子计划的       
 *		新列表。   
 *
 * PartitionPruneState 中现存索引的值统计的是初始               
 * 剪枝尚未进行时会存在的全部子计划。如果初               
 * 始剪枝除掉了部分子计划，那么后续任何剪枝               
 * 轮次所面对的可选目标子计划集合，都会与初              
 * 始剪枝前的集合不同，因此 PartitionPruneState 中      
 * 包含这些索引的映射必须更新，以反映初始剪          
 * 枝之后子计划集合的新索引。  
 */
static void
PartitionPruneFixSubPlanMap(PartitionPruneState *prunestate,
							Bitmapset *initially_valid_subplans,
							int n_total_subplans)
{
	int		   *new_subplan_indexes;
	Bitmapset  *new_other_subplans;
	int			i;
	int			newidx;

	/*
	 * 首先必须构建一个临时数组，把旧的子计划索引映射  
	 * 为新索引。为便于初始化，数组内采用 1-based 索引，  
	 * 被剪除的项则留为 0。          
	 */
	new_subplan_indexes = (int *) palloc0(sizeof(int) * n_total_subplans);
	newidx = 1;
	i = -1;
	while ((i = bms_next_member(initially_valid_subplans, i)) >= 0)
	{
		Assert(i < n_total_subplans);
		new_subplan_indexes[i] = newidx++;
	}

	/*
	 * 现在可用新的子计划索引更新每个 PartitionedRelPruneInfo
	 *  的 subplan_map，还须重算它的 present_parts 位图。     
	 */
	for (i = 0; i < prunestate->num_partprunedata; i++)
	{
		PartitionPruningData *prunedata = prunestate->partprunedata[i];
		int			j;

		/*
		 * 在每个层级内部，我们按从后往前的顺序执行此   
		 * 循环，从而先为最低层的分区表确定                   
		 * present_parts。这样我们就能判断某个子分区表的
		 * 分区是否已被完全剪除，从而可以把它从当前层  
		 * 级的 present_parts 中排除。 
		 */
		for (j = prunedata->num_partrelprunedata - 1; j >= 0; j--)
		{
			PartitionedRelPruningData *pprune = &prunedata->partrelprunedata[j];
			int			nparts = pprune->nparts;
			int			k;

			/* 我们直接从头重建 present_parts     */
			bms_free(pprune->present_parts);
			pprune->present_parts = NULL;

			for (k = 0; k < nparts; k++)
			{
				int			oldidx = pprune->subplan_map[k];
				int			subidx;

				/*
				 * 若此分区作为子计划存在，就把旧的索引改 
				 * 成新的子计划索引。新索引可能变成 -1，这 
				 * 是因为分区在上面已被剪除，也可能只是因  
				 * 为列表前部的一些子计划先被移除，使其在  
				 * 子计划列表位置提前。若为子分区，除非它
				 * 被完全剪除，否则加入 present_parts。 
				 */
				if (oldidx >= 0)
				{
					Assert(oldidx < n_total_subplans);
					pprune->subplan_map[k] = new_subplan_indexes[oldidx] - 1;

					if (new_subplan_indexes[oldidx] > 0)
						pprune->present_parts =
							bms_add_member(pprune->present_parts, k);
				}
				else if ((subidx = pprune->subpart_map[k]) >= 0)
				{
					PartitionedRelPruningData *subprune;

					subprune = &prunedata->partrelprunedata[subidx];

					if (!bms_is_empty(subprune->present_parts))
						pprune->present_parts =
							bms_add_member(pprune->present_parts, k);
				}
			}
		}
	}

	/*
	 * 还须重算 other_subplans 集合，因其中的索引可能改      
	 * 变。 
	 */
	new_other_subplans = NULL;
	i = -1;
	while ((i = bms_next_member(prunestate->other_subplans, i)) >= 0)
		new_other_subplans = bms_add_member(new_other_subplans,
											new_subplan_indexes[i] - 1);

	bms_free(prunestate->other_subplans);
	prunestate->other_subplans = new_other_subplans;

	pfree(new_subplan_indexes);
}

/*
 * ExecFindMatchingSubPlans
 *		针对当前比较表达式的取值，确定哪些子计划
 *		与 'prunestate' 中详述的剪枝步骤相符。        
 *
 * 若 PARAM_EXEC 参数尚不能求值，则传入 initial_prune。     
 * 这可区分执行器启动时的初始剪枝与后续运行    
 * 期剪枝。    
 */
Bitmapset *
ExecFindMatchingSubPlans(PartitionPruneState *prunestate,
						 bool initial_prune)
{
	Bitmapset  *result = NULL;
	MemoryContext oldcontext;
	int			i;

	/*
	 * 要么是在剪枝初始化时做初始剪枝，要么    
	 * 所在的点是 PARAM_EXEC 参数可以求值 *并且* 存在     
	 * 可执行这些求值的步骤。                 
	 */
	Assert(initial_prune || prunestate->do_exec_prune);

	/*
	 * 切换到临时上下文，以免内存泄漏到执行器的      
	 * 查询期内存上下文中。
	 */
	oldcontext = MemoryContextSwitchTo(prunestate->prune_context);

	/*
	 * 对每个层级做剪枝测试，并把不可删除子计划  
	 * 的索引加入 "result"。   
	 */
	for (i = 0; i < prunestate->num_partprunedata; i++)
	{
		PartitionPruningData *prunedata = prunestate->partprunedata[i];
		PartitionedRelPruningData *pprune;

		/*
		 * 我们传入第零项，它属于层级的根表，而     
		 * find_matching_subplans_recurse() 会负责按需递归到    
		 * 其他（更低层的）父节点。               
		 */
		pprune = &prunedata->partrelprunedata[0];
		find_matching_subplans_recurse(prunedata, pprune, initial_prune,
									   &result);

		/* 表达式求值可能也占用了 ExprContext 空间   */
		if (pprune->exec_pruning_steps)
			ResetExprContext(pprune->exec_context.exprcontext);
	}

	/* 把分区剪枝未顾及的所有子计划也加入进来     */
	result = bms_add_members(result, prunestate->other_subplans);

	MemoryContextSwitchTo(oldcontext);

	/* 重置临时上下文前，先把结果复制出来    */
	result = bms_copy(result);

	MemoryContextReset(prunestate->prune_context);

	return result;
}

/*
 * find_matching_subplans_recurse
 *		ExecFindMatchingSubPlans 的递归工作函数        
 *
 * 把不可剪除的子计划 ID 加入 *validsubplans   
 */
static void
find_matching_subplans_recurse(PartitionPruningData *prunedata,
							   PartitionedRelPruningData *pprune,
							   bool initial_prune,
							   Bitmapset **validsubplans)
{
	Bitmapset  *partset;
	int			i;

	/* 防止分区层级过深导致栈溢出。                           */
	check_stack_depth();

	/*
	 * 如果我们有匹配当前执行上下文的剪枝步骤，就    
	 * 酌情剪枝；否则就把此层的全部分区都纳入其     
	 * 中。
	 */
	if (initial_prune && pprune->initial_pruning_steps)
		partset = get_matching_partitions(&pprune->initial_context,
										  pprune->initial_pruning_steps);
	else if (!initial_prune && pprune->exec_pruning_steps)
		partset = get_matching_partitions(&pprune->exec_context,
										  pprune->exec_pruning_steps);
	else
		partset = pprune->present_parts;

	/* 把 partset 换算成子计划索引   */
	i = -1;
	while ((i = bms_next_member(partset, i)) >= 0)
	{
		if (pprune->subplan_map[i] >= 0)
			*validsubplans = bms_add_member(*validsubplans,
											pprune->subplan_map[i]);
		else
		{
			int			partidx = pprune->subpart_map[i];

			if (partidx >= 0)
				find_matching_subplans_recurse(prunedata,
											   &prunedata->partrelprunedata[partidx],
											   initial_prune, validsubplans);
			else
			{
				/*
				 * 若规划器已剪除了它的全部子分区，就会
				 * 到这里。此时静默忽略该分区。最终    
				 * 结果相同：本来也会剪除全部分区，只是 
				 * 我们没有任何剪枝步骤可执行来验证      
				 * 这一点。                                           
				 */
			}
		}
	}
}
