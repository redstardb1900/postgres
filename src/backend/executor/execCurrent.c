/*-------------------------------------------------------------------------
 *
 * execCurrent.c
 *	  WHERE CURRENT OF 游标的执行器支持   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/executor/execCurrent.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/rel.h"


static char *fetch_cursor_param_value(ExprContext *econtext, int paramId);
static ScanState *search_plan_tree(PlanState *node, Oid table_oid,
								   bool *pending_rescan);


/*
 * execCurrentOf
 *
 * 给定一个 CURRENT OF 表达式和某个表的 OID，确定该表中   
 * 当前正被 CURRENT OF 所指游标扫描的是哪一行，              
 * 并把该行的 TID 存入 *current_tid。 
 *
 * 如果确定了某一行则返回 true。如果游标对该表有效，    
 * 但当前并未在该表上扫描某一行（在继承场景中这是    
 * 一种合法情况），则返回 false。若该游标不是指定表  
 * 的有效可更新扫描，则报错。     
 */
bool
execCurrentOf(CurrentOfExpr *cexpr,
			  ExprContext *econtext,
			  Oid table_oid,
			  ItemPointer current_tid)
{
	char	   *cursor_name;
	char	   *table_name;
	Portal		portal;
	QueryDesc  *queryDesc;

	/* 获取游标名 --- 可能需要查找参数引用                */
	if (cexpr->cursor_name)
		cursor_name = cexpr->cursor_name;
	else
		cursor_name = fetch_cursor_param_value(econtext, cexpr->cursor_param);

	/* 获取表名，以备错误消息中使用          */
	table_name = get_rel_name(table_oid);
	if (table_name == NULL)
		elog(ERROR, "cache lookup failed for relation %u", table_oid);

	/* 找到游标的 portal   */
	portal = GetPortalByName(cursor_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor_name)));

	/*
	 * 我们必须同时提防非 SELECT 查询以及已持有的游标，
	 * 二者的 queryDesc 都可能为空。
	 */
	if (portal->strategy != PORTAL_ONE_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a SELECT query",
						cursor_name)));
	queryDesc = portal->queryDesc;
	if (queryDesc == NULL || queryDesc->estate == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is held from a previous transaction",
						cursor_name)));

	/*
	 * 根据游标是否使用 FOR UPDATE/SHARE，我们有两种不同的 
	 * 策略。两种都支持的原因是：FOR UPDATE 代码在很多   
	 * 另一种代码无法处理的场景中也能识别目标表，而非 
	 * FOR UPDATE 的情况则允许在 insensitive 游标上使用           
	 * WHERE CURRENT OF。                   
	 */
	if (queryDesc->estate->es_rowmarks)
	{
		ExecRowMark *erm;
		Index		i;

		/*
		 * 这里，查询必须恰好有对目标表的 FOR UPDATE/SHARE     
		 * 引用，我们从其中挖出 ctid 信息。          
		 */
		erm = NULL;
		for (i = 0; i < queryDesc->estate->es_range_table_size; i++)
		{
			ExecRowMark *thiserm = queryDesc->estate->es_rowmarks[i];

			if (thiserm == NULL ||
				!RowMarkRequiresRowShareLock(thiserm->markType))
				continue;		/* 忽略非 FOR UPDATE/SHARE 项    */

			if (thiserm->relid == table_oid)
			{
				if (erm)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_CURSOR_STATE),
							 errmsg("cursor \"%s\" has multiple FOR UPDATE/SHARE references to table \"%s\"",
									cursor_name, table_name)));
				erm = thiserm;
			}
		}

		if (erm == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" does not have a FOR UPDATE/SHARE reference to table \"%s\"",
							cursor_name, table_name)));

		/*
		 * 游标必须有一个当前结果行：按照 SQL 标准，      
		 * 无则报错。 
		 */
		if (portal->atStart || portal->atEnd)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not positioned on a row",
							cursor_name)));

		/* 如果存在，则返回当前扫描的 TID       */
		if (ItemPointerIsValid(&(erm->curCtid)))
		{
			*current_tid = erm->curCtid;
			return true;
		}

		/*
		 * 该表未产生游标的当前行；必然是同一父表的  
		 * 某个继承子表产生了它。通知调用者对该表不做  
		 * 任何处理。          
		 */
		return false;
	}
	else
	{
		/*
		 * 没有 FOR UPDATE 时，我们遍历游标的计划来找出    
		 * 扫描节点。若不在，或被埋于聚合下，    
		 * 则报错。
		 */
		ScanState  *scanstate;
		bool		pending_rescan = false;

		scanstate = search_plan_tree(queryDesc->planstate, table_oid,
									 &pending_rescan);
		if (!scanstate)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
							cursor_name, table_name)));

		/*
		 * 游标必须有一个当前结果行：按照 SQL 标准，若无
		 * 则报错。我们在顶层而非扫描节点层级做此测试， 
		 * 因为在继承场景中，任何单个表扫描都很容易    
		 * 不在任何行上。如果传入的表 OID 对应的是某个  
		 * 不活跃的扫描，我们希望返回 false 而非报错。      
		 */
		if (portal->atStart || portal->atEnd)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not positioned on a row",
							cursor_name)));

		/*
		 * 若我们发现的是不活跃扫描，现在可以返回  
		 * false。不活跃的原因要么是未定位在行上，     
		 * 或是有待重扫。 
		 */
		if (TupIsNull(scanstate->ss_ScanTupleSlot) || pending_rescan)
			return false;

		/*
		 * 提取扫描的当前行的 TID。其机制原则上取决于    
		 * 扫描类型，但对大多数扫描类型而言，我们可以  
		 * 直接从物理扫描元组中挖出 TID。     
		 */
		if (IsA(scanstate, IndexOnlyScanState))
		{
			/*
			 * 对 IndexOnlyScan 而言，ss_ScanTupleSlot 中存储的元组
			 * 可能是不带 ctid 列的虚拟元组，因此我们必须  
			 * 从 xs_ctup.t_self 获取 TID。   
			 */
			IndexScanDesc scan = ((IndexOnlyScanState *) scanstate)->ioss_ScanDesc;

			*current_tid = scan->xs_heaptid;
		}
		else
		{
			/*
			 * 默认：尝试从扫描节点的当前元组获取 TID。 
			 * 作为额外交叉检查，验证当前元组的 tableoid。   
			 * 如果扫描没有提供物理元组，我们只能          
			 * 失败  
			 */
			Datum		ldatum;
			bool		lisnull;
			ItemPointer tuple_tid;

#ifdef USE_ASSERT_CHECKING
			ldatum = slot_getsysattr(scanstate->ss_ScanTupleSlot,
									 TableOidAttributeNumber,
									 &lisnull);
			if (lisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_CURSOR_STATE),
						 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
								cursor_name, table_name)));
			Assert(DatumGetObjectId(ldatum) == table_oid);
#endif

			ldatum = slot_getsysattr(scanstate->ss_ScanTupleSlot,
									 SelfItemPointerAttributeNumber,
									 &lisnull);
			if (lisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_CURSOR_STATE),
						 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
								cursor_name, table_name)));
			tuple_tid = (ItemPointer) DatumGetPointer(ldatum);

			*current_tid = *tuple_tid;
		}

		Assert(ItemPointerIsValid(current_tid));

		return true;
	}
}

/*
 * fetch_cursor_param_value
 *
 * 获取参数的字符串值，并验证其类型为 REFCURSOR。     
 */
static char *
fetch_cursor_param_value(ExprContext *econtext, int paramId)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;

	if (paramInfo &&
		paramId > 0 && paramId <= paramInfo->numParams)
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		/* 若参数是动态的，给钩子一个机会   */
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

		if (OidIsValid(prm->ptype) && !prm->isnull)
		{
			/* 以防钩子做了意外之事的安全检查      */
			if (prm->ptype != REFCURSOROID)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(REFCURSOROID))));

			/* refcursor 使用 text 的 I/O 例程            */
			return TextDatumGetCString(prm->value);
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
	return NULL;
}

/*
 * search_plan_tree
 *
 * 在 PlanState 树中搜索指定表上的扫描节点。               
 * 找不到或有多个候选则返回 NULL。    
 *
 * 注意：这个函数的职责并不只是找到某个候选扫描，   
 * 而是要确保该扫描产生的正是计划树当前的输出行。  
 * 这正是我们必须拒绝多候选情况的原因。      
 *
 * 如果找到了候选，并且该候选或其上方的任何节点    
 * 有待处理的重新扫描动作（即 chgParam != NULL），          
 * 则把 *pending_rescan 设为 true。这表明我们不应认为        
 * 该节点定位在一个有效元组上，即使它自身的状态    
 * 显示如此。（调用者必须把 *pending_rescan 初始化为       
 * false；多候选勿信。）     
 */
static ScanState *
search_plan_tree(PlanState *node, Oid table_oid,
				 bool *pending_rescan)
{
	ScanState  *result = NULL;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
			/*
			 * 关系扫描节点都可以同等对待：检查它们       
			 * 是否在扫描指定的表。        
			 *
			 * ForeignScan 和 CustomScan 可能没有 currentRelation，     
			 * 这种情况我们直接忽略它们。（不敢下潜到   
			 * 其子计划节点，因为我们不知道该节点的    
			 * 当前输出元组与子节点当前输出之间有何   
			 * 关系。）                
			 */
		case T_SeqScanState:
		case T_SampleScanState:
		case T_IndexScanState:
		case T_IndexOnlyScanState:
		case T_BitmapHeapScanState:
		case T_TidScanState:
		case T_TidRangeScanState:
		case T_ForeignScanState:
		case T_CustomScanState:
			{
				ScanState  *sstate = (ScanState *) node;

				if (sstate->ss_currentRelation &&
					RelationGetRelid(sstate->ss_currentRelation) == table_oid)
					result = sstate;
				break;
			}

			/*
			 * 对 Append，可检查每个输入节点。下潜       
			 * 到这些输入是安全的，因为只有产生 Append      
			 * 当前输出节点的那个输入才可能定位在元组上；
			 * 其他输入要么已到 EOF，要么尚未启动。因此若 
			 * 目标表由不活跃节点扫描，我们会 
			 * 找到该节点，但调用者会意识到它并没有        
			 * 发出我们所关心的那个                          
			 * 元组。
			 *
			 * 我们确实需要提防多重匹配（当 Append 来自 
			 * UNION ALL 而非继承树时可能出现）。             
			 *
			 * 注意：不能类似地下潜穿过 MergeAppend，因为    
			 * 它的输入可能全部活跃，且不知道是哪一个    
			 * 返回了当前输出元组。（若让这段代码更    
			 * 多地了解 MergeAppend 的内部状态，这一点或许     
			 * 可解决，但似乎不值得费功夫。用户不应期望  
			 * ORDER BY 查询的计划被视为简单可更新，因为    
			 * 若排序由 Sort 节点实现，它们就不会是    
			 * 简单可更新的。）    
			 */
		case T_AppendState:
			{
				AppendState *astate = (AppendState *) node;
				int			i;

				for (i = 0; i < astate->as_nplans; i++)
				{
					ScanState  *elem = search_plan_tree(astate->appendplans[i],
														table_oid,
														pending_rescan);

					if (!elem)
						continue;
					if (result)
						return NULL;	/* 多重匹配     */
					result = elem;
				}
				break;
			}

			/*
			 * Result 和 Limit 可以穿过（它们是安全的，    
			 * 因为它们总是返回其输入的当前行）     
			 */
		case T_ResultState:
		case T_LimitState:
			result = search_plan_tree(outerPlanState(node),
									  table_oid,
									  pending_rescan);
			break;

			/*
			 * SubqueryScan 也是如此，但它把子节点存放在别处
			 */
		case T_SubqueryScanState:
			result = search_plan_tree(((SubqueryScanState *) node)->subplan,
									  table_oid,
									  pending_rescan);
			break;

		default:
			/* 否则，假定我们无法下潜穿过它    */
			break;
	}

	/*
	 * 如果在这个节点处或其下方找到了候选，那么   
	 * 该节点的 chgParam 表示将影响该候选的重新扫描。   
	 */
	if (result && node->chgParam != NULL)
		*pending_rescan = true;

	return result;
}
