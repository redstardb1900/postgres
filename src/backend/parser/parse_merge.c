/*-------------------------------------------------------------------------
 *
 * parse_merge.c
 *	  解析器处理 MERGE 语句    
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_merge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parsetree.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_merge.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/rel.h"
#include "utils/relcache.h"

static void setNamespaceForMergeWhen(ParseState *pstate,
									 MergeWhenClause *mergeWhenClause,
									 Index targetRTI,
									 Index sourceRTI);
static void setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
										 bool rel_visible,
										 bool cols_visible);

/*
 * 在转换各动作的限定条件和目标列表表达式时，需要  
 * 相应调整命名空间的可见性。特别地，INSERT 动作        
 * 只能看到源关系（因为 INSERT 动作只针对 NOT MATCHED          
 * 元组调用，因此没有目标元组需要处理）。另一方面， 
 * UPDATE 和 DELETE 动作可以同时看到源关系和                     
 * 目标关系。  
 *
 * 另外，由于内部连接节点会把源关系和目标关系  
 * 隐藏起来，我们必须显式地将相应关系设为可见，      
 * 以便这些关系的列可以被不带限定地引用。       
 */
static void
setNamespaceForMergeWhen(ParseState *pstate, MergeWhenClause *mergeWhenClause,
						 Index targetRTI, Index sourceRTI)
{
	RangeTblEntry *targetRelRTE,
			   *sourceRelRTE;

	targetRelRTE = rt_fetch(targetRTI, pstate->p_rtable);
	sourceRelRTE = rt_fetch(sourceRTI, pstate->p_rtable);

	if (mergeWhenClause->matched)
	{
		Assert(mergeWhenClause->commandType == CMD_UPDATE ||
			   mergeWhenClause->commandType == CMD_DELETE ||
			   mergeWhenClause->commandType == CMD_NOTHING);

		/* MATCHED 动作可同时看到目标与源关系。        */
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 targetRelRTE, true, true);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 sourceRelRTE, true, true);
	}
	else
	{
		/*
		 * NOT MATCHED 动作看不到目标关系，但可以看到      
		 * 源关系。    
		 */
		Assert(mergeWhenClause->commandType == CMD_INSERT ||
			   mergeWhenClause->commandType == CMD_NOTHING);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 targetRelRTE, false, false);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 sourceRelRTE, true, true);
	}
}

/*
 * transformMergeStmt -
 *	  转换 MERGE 语句         
 */
Query *
transformMergeStmt(ParseState *pstate, MergeStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ListCell   *l;
	AclMode		targetPerms = ACL_NO_RIGHTS;
	bool		is_terminal[2];
	Index		sourceRTI;
	List	   *mergeActionList;
	Node	   *joinExpr;
	ParseNamespaceItem *nsitem;

	/* 不需要担心任何外层的 WITH 子句   */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_MERGE;
	qry->hasRecursive = false;

	/* 独立于其他一切单独处理 WITH 子句     */
	if (stmt->withClause)
	{
		if (stmt->withClause->recursive)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH RECURSIVE is not supported for MERGE statement")));

		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * 检查 WHEN 子句的权限和合理性      
	 */
	is_terminal[0] = false;
	is_terminal[1] = false;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = (MergeWhenClause *) lfirst(l);
		int			when_type = (mergeWhenClause->matched ? 0 : 1);

		/*
		 * 根据动作类型收集需要检查的权限。对于 DO NOTHING  
		 * 我们需要 SELECT 权限，因为如果 DO NOTHING 是唯一的 
		 * 动作，目标关系却无权限被检查会不合常规。      
		 * 这样并无损害：有意义的 MERGE 命令反正都需要   
		 * 对表有至少某种访问。                                  
		 */
		switch (mergeWhenClause->commandType)
		{
			case CMD_INSERT:
				targetPerms |= ACL_INSERT;
				break;
			case CMD_UPDATE:
				targetPerms |= ACL_UPDATE;
				break;
			case CMD_DELETE:
				targetPerms |= ACL_DELETE;
				break;
			case CMD_NOTHING:
				targetPerms |= ACL_SELECT;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		/*
		 * 检查不可达的 WHEN 子句    
		 */
		if (is_terminal[when_type])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unreachable WHEN clause specified after unconditional WHEN clause")));
		if (mergeWhenClause->condition == NULL)
			is_terminal[when_type] = true;
	}

	/*
	 * 设置 MERGE 目标表。该表在下方加入命名空间，   
	 * 并由 transform_MERGE_to_join 加入连接列表，不在此处理。 
	 *         
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 false, targetPerms);

	/*
	 * 多种情形下不支持 MERGE       
	 */
	if (pstate->p_target_relation->rd_rel->relkind != RELKIND_RELATION &&
		pstate->p_target_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot execute MERGE on relation \"%s\"",
						RelationGetRelationName(pstate->p_target_relation)),
				 errdetail_relkind_not_supported(pstate->p_target_relation->rd_rel->relkind)));
	if (pstate->p_target_relation->rd_rules != NULL &&
		pstate->p_target_relation->rd_rules->numLocks > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot execute MERGE on relation \"%s\"",
						RelationGetRelationName(pstate->p_target_relation)),
				 errdetail("MERGE is not supported for relations with rules.")));

	/* 现在转换源关系以生成源 RTE。                     */
	transformFromClause(pstate,
						list_make1(stmt->sourceRelation));
	sourceRTI = list_length(pstate->p_rtable);
	nsitem = GetNSItemByRangeTablePosn(pstate, sourceRTI, 0);

	/*
	 * 检查目标表与源表是否冲突。这通常应当用          
	 * checkNameSpaceConflicts 来检查，但我们想要更               
	 * 具体的错误消息。    
	 */
	if (strcmp(pstate->p_target_nsitem->p_names->aliasname,
			   nsitem->p_names->aliasname) == 0)
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_ALIAS),
				errmsg("name \"%s\" specified more than once",
					   pstate->p_target_nsitem->p_names->aliasname),
				errdetail("The name is used both as MERGE target table and data source."));

	/*
	 * 此处无需目标列表；由 preprocess_targetlist     
	 * 稍后设置。             
	 */
	qry->targetList = NIL;
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;

	/*
	 * 转换连接条件。这包括对目标侧的引用，               
	 * 故需将其加入命名空间。  
	 */
	addNSItemToQuery(pstate, pstate->p_target_nsitem, false, true, true);
	joinExpr = transformExpr(pstate, stmt->joinCondition,
							 EXPR_KIND_JOIN_ON);

	/*
	 * 使用仅由源关系构建的连接列表来创建                    
	 * 临时查询的连接树；其中不包含目标关系。我们    
	 * 使用的限定条件是到合并目标的连接条件。连接    
	 * 将完全由 transform_MERGE_to_join 构建。       
	 */
	qry->jointree = makeFromExpr(pstate->p_joinlist, joinExpr);

	/*
	 * 现在查询形状已确定，接着查看 WHEN 条件与动作的    
	 * 目标列表。    
	 *
	 * 总体上 MERGE 查询目标列表为 NIL。 
	 *
	 * 每个动作都有各自的目标列表，需要单独           
	 * 进行转换。这些转换不会对整体目标列表           
	 * 产生任何影响，因为它只用于 resjunk 列。  
	 *
	 * Target 或 Source 中的任何列都可以引用，因为二者     
	 * 都已有各自的 RTE。这里没有 INSERT ON CONFLICT           
	 * 那样的 EXCLUDED 伪关系。        
	 */
	mergeActionList = NIL;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = lfirst_node(MergeWhenClause, l);
		MergeAction *action;

		action = makeNode(MergeAction);
		action->commandType = mergeWhenClause->commandType;
		action->matched = mergeWhenClause->matched;

		/* 若命令中存在任何 INSERT 动作，则使用外连接。*/
		if (action->commandType == CMD_INSERT)
			qry->mergeUseOuterJoin = true;

		/*
		 * 为特定动作设置命名空间。此步必须在            
		 * 分析 WHEN 条件与动作目标列表之前。   
		 */
		setNamespaceForMergeWhen(pstate, mergeWhenClause,
								 qry->resultRelation,
								 sourceRTI);

		/*
		 * 转换 WHEN 条件。        
		 *
		 * 注意这些限定条件不会加入连接条件；它们在       
		 * 执行期单独求值，以决定执行哪个                   
		 * WHEN MATCHED 或 WHEN NOT MATCHED 动作。         
		 */
		action->qual = transformWhereClause(pstate, mergeWhenClause->condition,
											EXPR_KIND_MERGE_WHEN, "WHEN");

		/*
		 * 为每个 INSERT 和 UPDATE 动作语句转换目标列表   
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					List	   *exprList = NIL;
					ListCell   *lc;
					RTEPermissionInfo *perminfo;
					ListCell   *icols;
					ListCell   *attnos;
					List	   *icolumns;
					List	   *attrnos;

					pstate->p_is_insert = true;

					icolumns = checkInsertTargets(pstate,
												  mergeWhenClause->targetList,
												  &attrnos);
					Assert(list_length(icolumns) == list_length(attrnos));

					action->override = mergeWhenClause->override;

					/*
					 * INSERT 的处理类似 transformInsertStmt    
					 */
					if (mergeWhenClause->values == NIL)
					{
						/*
						 * 我们有 INSERT ... DEFAULT VALUES。           
						 * 处理办法是发出空目标列表 ---         
						 * 规划器展开目标列表时，所有列        
						 * 取默认值。
						 */
						exprList = NIL;
					}
					else
					{
						/*
						 * 处理带单个 VALUES 子列表的           
						 * INSERT ... VALUES。为提高效率，     
						 * 这种情形被单独处理。子列表直接     
						 * 作为查询的目标列表来计算，没有     
						 * VALUES RTE。如同无 FROM 的 SELECT。    
						 */

						/*
						 * 基本表达式转换（同 ROW() 表达式        
						 * 相同，但顶层允许 SetToDefault）  
						 */
						exprList = transformExpressionList(pstate,
														   mergeWhenClause->values,
														   EXPR_KIND_VALUES_SINGLE,
														   true);

						/* 准备好行以便赋给目标表          */
						exprList = transformInsertRow(pstate, exprList,
													  mergeWhenClause->targetList,
													  icolumns, attrnos,
													  false);
					}

					/*
					 * 使用已算出的表达式列表生成动作的     
					 * 目标列表。同时把所有目标列标记为    
					 * 需要插入权限。      
					 */
					perminfo = pstate->p_target_nsitem->p_perminfo;
					forthree(lc, exprList, icols, icolumns, attnos, attrnos)
					{
						Expr	   *expr = (Expr *) lfirst(lc);
						ResTarget  *col = lfirst_node(ResTarget, icols);
						AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
						TargetEntry *tle;

						tle = makeTargetEntry(expr,
											  attr_num,
											  col->name,
											  false);
						action->targetList = lappend(action->targetList, tle);

						perminfo->insertedCols =
							bms_add_member(perminfo->insertedCols,
										   attr_num - FirstLowInvalidHeapAttributeNumber);
					}
				}
				break;
			case CMD_UPDATE:
				{
					pstate->p_is_insert = false;
					action->targetList =
						transformUpdateTargetList(pstate,
												  mergeWhenClause->targetList);
				}
				break;
			case CMD_DELETE:
				break;

			case CMD_NOTHING:
				action->targetList = NIL;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		mergeActionList = lappend(mergeActionList, action);
	}

	qry->mergeActionList = mergeActionList;

	/* 将来有可能加入 RETURNING，但 SQL 标准中没有              */
	qry->returningList = NULL;

	qry->hasTargetSRFs = false;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

static void
setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
							 bool rel_visible,
							 bool cols_visible)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rte == rte)
		{
			nsitem->p_rel_visible = rel_visible;
			nsitem->p_cols_visible = cols_visible;
			break;
		}
	}
}
