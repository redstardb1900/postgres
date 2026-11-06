/*-------------------------------------------------------------------------
 *
 * parse_relation.c
 *	  处理关系相关的解析器支持例程    
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_relation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_enr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 支持对列的模糊匹配。       
 *
 * 用于构建诊断消息，其中涉及多个                        
 * 或非精确匹配的属性。      
 *
 * "distance"在rfirst非NULL时为当前最佳模糊匹配距离，        
 * 否则为可接受的最大距离加 1。              
 *
 * rfirst/first 记录迄今最接近的非精确匹配，distance     
 * 为其与目标名的距离。若已找到第二个非精确              
 * 且距离完全相同的匹配，rsecond/second 记录之（若     
 * 找到三个相同距离的匹配，就断定 "distance" 上限       
 * 不够紧，给不出有效提示，再次清除 rfirst               
 * 和 rsecond。稍后发现更近者才会重新填充 rfirst。） 
 *
 * rexact1/exact1 记录第一个精确匹配列的位置（若有），   
 * 若找到多个精确匹配，则 rexact2/exact2 记录另一处     
 * （不太在意是哪一个）。目前这些字段                
 * 的填充独立于模糊匹配字段。           
 */
typedef struct
{
	int			distance;		/* 当前或极限距离     */
	RangeTblEntry *rfirst;		/* 最近非精确匹配的 RTE 或 NULL   */
	AttrNumber	first;			/* rfirst 中的列号 */
	RangeTblEntry *rsecond;		/* 另一相同距离非精确匹配的 RTE   */
	AttrNumber	second;			/* rsecond 中的列号 */
	RangeTblEntry *rexact1;		/* 首个精确匹配的 RTE 或 NULL*/
	AttrNumber	exact1;			/* rexact1 中的列号 */
	RangeTblEntry *rexact2;		/* 第二精确匹配的 RTE 或 NULL */
	AttrNumber	exact2;			/* rexact2 中的列号 */
} FuzzyAttrMatchState;

#define MAX_FUZZY_DISTANCE				3


static ParseNamespaceItem *scanNameSpaceForRefname(ParseState *pstate,
												   const char *refname,
												   int location);
static ParseNamespaceItem *scanNameSpaceForRelid(ParseState *pstate, Oid relid,
												 int location);
static void check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
								 int location);
static int	scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
							 Alias *eref,
							 const char *colname, int location,
							 int fuzzy_rte_penalty,
							 FuzzyAttrMatchState *fuzzystate);
static void markRTEForSelectPriv(ParseState *pstate,
								 int rtindex, AttrNumber col);
static void expandRelation(Oid relid, Alias *eref,
						   int rtindex, int sublevels_up,
						   int location, bool include_dropped,
						   List **colnames, List **colvars);
static void expandTupleDesc(TupleDesc tupdesc, Alias *eref,
							int count, int offset,
							int rtindex, int sublevels_up,
							int location, bool include_dropped,
							List **colnames, List **colvars);
static int	specialAttNum(const char *attname);
static bool rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte);
static bool rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte);
static bool isQueryUsingTempRelation_walker(Node *node, void *context);


/*
 * refnameNamespaceItem
 *	  给定可能带限定的 refname，查看其是否匹配任何可见的  
 *	  命名空间条目。若匹配则返回 nsitem，否则返回 NULL。    
 *
 *	  可将 nsitem 嵌套深度（0=当前层）存入 *sublevels_up。     
 *	  若 sublevels_up 为 NULL，则只考虑当前嵌套               
 *	  层。
 *
 * 不带限定的 refname（schemaname == NULL）可匹配任何具有匹配  
 * 别名的条目；无别名的关系条目，则可匹配不带限定的 
 * relname。这样的 refname 可能在有匹配的最近嵌套           
 * 层中匹配多个条目；若如此我们会经 ereport() 报错。 
 *           
 *
 * 带限定的 refname（schemaname != NULL）只能匹配满足以下   
 * 条件的关系条目：(a) 无别名且 (b) 是由                 
 * schemaname.refname 标识的同一关系。此时我们会把       
 * 转为关系 OID，按 relid 搜索而非按别名。虽怪，       
 * 但 SQL 确实如此规定。          
 */
ParseNamespaceItem *
refnameNamespaceItem(ParseState *pstate,
					 const char *schemaname,
					 const char *refname,
					 int location,
					 int *sublevels_up)
{
	Oid			relId = InvalidOid;

	if (sublevels_up)
		*sublevels_up = 0;

	if (schemaname != NULL)
	{
		Oid			namespaceId;

		/*
		 * 此处可用 LookupNamespaceNoError()，因为我们只想  
		 * 查找已存在的 RTE。无需检查模式上的 USAGE 权限， 
		 * 因为创建 RTE 时已经检查过了。此外，若该名称    
		 * 恰好匹配用户无权访问的模式名，我们想报告   
		 * "RTE not found"，而不是 "no permissions for schema"。     
		 *                                                   
		 */
		namespaceId = LookupNamespaceNoError(schemaname);
		if (!OidIsValid(namespaceId))
			return NULL;
		relId = get_relname_relid(refname, namespaceId);
		if (!OidIsValid(relId))
			return NULL;
	}

	while (pstate != NULL)
	{
		ParseNamespaceItem *result;

		if (OidIsValid(relId))
			result = scanNameSpaceForRelid(pstate, relId, location);
		else
			result = scanNameSpaceForRefname(pstate, refname, location);

		if (result)
			return result;

		if (sublevels_up)
			(*sublevels_up)++;
		else
			break;

		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * 在查询的表命名空间中搜索与给定不带限定  
 * refname 匹配的条目。唯一匹配则返回 nsitem，无匹配      
 * 返回 NULL；多个匹配则报错。        
 *
 * 注：似乎我们本不必担心出现多个匹配的可能；          
 * 毕竟 SQL 标准禁止在同一 SELECT 层内出现重复的表          
 * 别名。但 Postgres 历来都比这更宽松。例如，               
 * 我们允许                                
 *		SELECT ... FROM tab1 x CROSS JOIN (tab2 x CROSS JOIN tab3 y) z
 * 理由是带别名的连接（z）会隐藏其内部别名，故两  
 * 个名为 "x" 的 RTE 之间并不冲突。但是，若 tab3 是        
 * LATERAL 子查询，那么从子查询内部两个 "x" 都可见。    
 * 我们不想拒绝以前可行的查询，故允许这种情形，     
 * 只有当确实存在对 "x" 的歧义引用时才会             
 * 报错。        
 */
static ParseNamespaceItem *
scanNameSpaceForRefname(ParseState *pstate, const char *refname, int location)
{
	ParseNamespaceItem *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

		/* 忽略仅列条目        */
		if (!nsitem->p_rel_visible)
			continue;
		/* 若不在 LATERAL 内，则忽略仅横向条目 */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		if (strcmp(nsitem->p_names->aliasname, refname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								refname),
						 parser_errposition(pstate, location)));
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
		}
	}
	return result;
}

/*
 * 在查询的表命名空间中搜索与给定关系 OID 匹配的  
 * 关系条目。唯一匹配则返回 nsitem，无匹配返回    
 * NULL；多个匹配则报错。               
 *
 * 为何如此行事，参见 refnameNamespaceItem                
 * 的注释。         
 */
static ParseNamespaceItem *
scanNameSpaceForRelid(ParseState *pstate, Oid relid, int location)
{
	ParseNamespaceItem *result = NULL;
	ListCell   *l;

	foreach(l, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
		RangeTblEntry *rte = nsitem->p_rte;

		/* 忽略仅列条目        */
		if (!nsitem->p_rel_visible)
			continue;
		/* 若不在 LATERAL 内，则忽略仅横向条目 */
		if (nsitem->p_lateral_only && !pstate->p_lateral_active)
			continue;

		/* 是的，这里应有 alias == NULL 测试...      */
		if (rte->rtekind == RTE_RELATION &&
			rte->relid == relid &&
			rte->alias == NULL)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference %u is ambiguous",
								relid),
						 parser_errposition(pstate, location)));
			check_lateral_ref_ok(pstate, nsitem, location);
			result = nsitem;
		}
	}
	return result;
}

/*
 * 在查询的 CTE 命名空间中搜索与给定不带限定 refname      
 * 匹配的 CTE。若匹配则返回该 CTE（及 levelsup 计数），
 * 无匹配返回 NULL。无需担心多个匹配，因为 parse_cte.c     
 * 会拒绝含重复 CTE 名称的 WITH 列表。   
 */
CommonTableExpr *
scanNameSpaceForCTE(ParseState *pstate, const char *refname,
					Index *ctelevelsup)
{
	Index		levelsup;

	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_ctenamespace)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
			{
				*ctelevelsup = levelsup;
				return cte;
			}
		}
	}
	return NULL;
}

/*
 * 搜索可能的"未来 CTE"，即按 WITH 作用域规则               
 * 尚不在作用域内的 CTE。这与合法 SQL 语义无关，        
 * 但对错误报告很重要。                                 
 */
static bool
isFutureCTE(ParseState *pstate, const char *refname)
{
	for (; pstate != NULL; pstate = pstate->parentParseState)
	{
		ListCell   *lc;

		foreach(lc, pstate->p_future_ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

			if (strcmp(cte->ctename, refname) == 0)
				return true;
		}
	}
	return false;
}

/*
 * 在查询的瞬态命名关系命名空间中搜索与给定        
 * 不带限定 refname 匹配的关系。
 */
bool
scanNameSpaceForENR(ParseState *pstate, const char *refname)
{
	return name_matches_visible_ENR(pstate, refname);
}

/*
 * searchRangeTableForRel
 *	  查看是否有 RangeTblEntry 可能匹配该 RangeVar。  
 *	  若有则返回该 RangeTblEntry 指针，否则 NULL。       
 *
 * 它与 refnameNamespaceItem 的不同在于：它会考虑            
 * ParseState 的所有范围表条目，而不只是当前在               
 * p_namespace 列表中可见的那些。按 SQL 标准这一              
 * 行为无效，且可能给出歧义结果（可能存在多个         
 * 同样有效的匹配，但只返回其一）。它只能用作       
 * 给出合适错误消息的启发式手段。参见 errorMissingRTE。 
 *
 * 注意我们会同时考虑对实际关系（或 CTE）名的匹配  
 * 及别名匹配。   
 */
static RangeTblEntry *
searchRangeTableForRel(ParseState *pstate, RangeVar *relation)
{
	const char *refname = relation->relname;
	Oid			relId = InvalidOid;
	CommonTableExpr *cte = NULL;
	bool		isenr = false;
	Index		ctelevelsup = 0;
	Index		levelsup;

	/*
	 * 若名称不带限定，则检查是否有可匹配的 CTE。CTE  
	 * 会遮蔽真实关系匹配。若无 CTE，则查找匹配的   
	 * 关系。
	 *
	 * 注意：面对并发 DDL，RangeVarGetRelid 在此返回的答案   
	 * 并不必须正确。若不正确，最坏情况也只是一条   
	 * 不够清晰的错误消息。而且查询涉及的表均已锁定，
	 * 这减少了出现意外行为的情况数量。因此我们进行
	 * 不加锁的名称查找。                                   
	 *          
	 */
	if (!relation->schemaname)
	{
		cte = scanNameSpaceForCTE(pstate, refname, &ctelevelsup);
		if (!cte)
			isenr = scanNameSpaceForENR(pstate, refname);
	}

	if (!cte && !isenr)
		relId = RangeVarGetRelid(relation, NoLock, true);

	/* 然后查找匹配关系/CTE/ENR 或别名的 RTE                   */
	for (levelsup = 0;
		 pstate != NULL;
		 pstate = pstate->parentParseState, levelsup++)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

			if (rte->rtekind == RTE_RELATION &&
				OidIsValid(relId) &&
				rte->relid == relId)
				return rte;
			if (rte->rtekind == RTE_CTE &&
				cte != NULL &&
				rte->ctelevelsup + levelsup == ctelevelsup &&
				strcmp(rte->ctename, refname) == 0)
				return rte;
			if (rte->rtekind == RTE_NAMEDTUPLESTORE &&
				isenr &&
				strcmp(rte->enrname, refname) == 0)
				return rte;
			if (strcmp(rte->eref->aliasname, refname) == 0)
				return rte;
		}
	}
	return NULL;
}

/*
 * 检查两个命名空间列表之间的关系名冲突。     
 * 若发现冲突则报错。    
 *
 * 注：我们假定每个给定参数自身不含冲突；          
 * 我们只想知道两者能否合并在一起。               
 *
 * 按 SQL 规则，两个无别名普通关系 RTE 即使            
 * eref->aliasname 相同（即关系名相同），只要二者的    
 * 关系 OID 不同（即处于不同模式），则互不冲突。        
 *
 * 我们忽略命名空间条目中的仅横向标志：两个列表     
 * 必须互不冲突，即使所有条目均视为可见。不过， 
 * 应忽略仅列条目。             
 */
void
checkNameSpaceConflicts(ParseState *pstate, List *namespace1,
						List *namespace2)
{
	ListCell   *l1;

	foreach(l1, namespace1)
	{
		ParseNamespaceItem *nsitem1 = (ParseNamespaceItem *) lfirst(l1);
		RangeTblEntry *rte1 = nsitem1->p_rte;
		const char *aliasname1 = nsitem1->p_names->aliasname;
		ListCell   *l2;

		if (!nsitem1->p_rel_visible)
			continue;

		foreach(l2, namespace2)
		{
			ParseNamespaceItem *nsitem2 = (ParseNamespaceItem *) lfirst(l2);
			RangeTblEntry *rte2 = nsitem2->p_rte;
			const char *aliasname2 = nsitem2->p_names->aliasname;

			if (!nsitem2->p_rel_visible)
				continue;
			if (strcmp(aliasname2, aliasname1) != 0)
				continue;		/* 肯定无冲突        */
			if (rte1->rtekind == RTE_RELATION && rte1->alias == NULL &&
				rte2->rtekind == RTE_RELATION && rte2->alias == NULL &&
				rte1->relid != rte2->relid)
				continue;		/* 按 SQL 规则无冲突  */
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_ALIAS),
					 errmsg("table name \"%s\" specified more than once",
							aliasname1)));
		}
	}
}

/*
 * 若命名空间条目当前不允许作为 LATERAL 引用则报错。       
 * 这既强制执行了 SQL:2008 对外连接错误一侧的 LATERAL        
 * 引用的颇为古怪的处理方式，也强制执行我们自己      
 * 的禁令：在 FROM/USING 子句中不得横向引用 UPDATE             
 * 或 DELETE 目标表。
 *
 * 注：pstate 应当是发现该 nsitem 时所在的查询层级。      
 *
 * 便捷子例程，用于避免重复书写相当丑陋的 ereport。     
 */
static void
check_lateral_ref_ok(ParseState *pstate, ParseNamespaceItem *nsitem,
					 int location)
{
	if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
	{
		/* SQL:2008 要求此为错误，而非不可见条目      */
		RangeTblEntry *rte = nsitem->p_rte;
		char	   *refname = nsitem->p_names->aliasname;

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						refname),
				 (pstate->p_target_nsitem != NULL &&
				  rte == pstate->p_target_nsitem->p_rte) ?
				 errhint("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						 refname) :
				 errdetail("The combining JOIN type must be INNER or LEFT for a LATERAL reference."),
				 parser_errposition(pstate, location)));
	}
}

/*
 * 给定 RT 索引和嵌套深度，找到对应的          
 * ParseNamespaceItem（必定存在）。
 */
ParseNamespaceItem *
GetNSItemByRangeTablePosn(ParseState *pstate,
						  int varno,
						  int sublevels_up)
{
	ListCell   *lc;

	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	foreach(lc, pstate->p_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rtindex == varno)
			return nsitem;
	}
	elog(ERROR, "nsitem not found (internal error)");
	return NULL;				/* 让编译器安静  */
}

/*
 * 给定 RT 索引和嵌套深度，找到对应的 RTE。        
 * （注意该 RTE 不必在查询命名空间中。）    
 */
RangeTblEntry *
GetRTEByRangeTablePosn(ParseState *pstate,
					   int varno,
					   int sublevels_up)
{
	while (sublevels_up-- > 0)
	{
		pstate = pstate->parentParseState;
		Assert(pstate != NULL);
	}
	Assert(varno > 0 && varno <= list_length(pstate->p_rtable));
	return rt_fetch(varno, pstate->p_rtable);
}

/*
 * 为 CTE 引用 RTE 取得其 CTE。   
 *
 * rtelevelsup 是该 RTE 相对给定 pstate 之上的查询               
 * 层数。     
 */
CommonTableExpr *
GetCTEForRTE(ParseState *pstate, RangeTblEntry *rte, int rtelevelsup)
{
	Index		levelsup;
	ListCell   *lc;

	Assert(rte->rtekind == RTE_CTE);
	levelsup = rte->ctelevelsup + rtelevelsup;
	while (levelsup-- > 0)
	{
		pstate = pstate->parentParseState;
		if (!pstate)			/* 不应发生     */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	foreach(lc, pstate->p_ctenamespace)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			return cte;
	}
	/* 不应发生     */
	elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	return NULL;				/* 让编译器安静  */
}

/*
 * updateFuzzyAttrMatchState
 *	  用 Levenshtein 距离判断列是否为最佳模糊匹配。      
 */
static void
updateFuzzyAttrMatchState(int fuzzy_rte_penalty,
						  FuzzyAttrMatchState *fuzzystate, RangeTblEntry *rte,
						  const char *actual, const char *match, int attnum)
{
	int			columndistance;
	int			matchlen;

	/* 若已无希望，则免算 Levenshtein 距离，直接退出。   */
	if (fuzzy_rte_penalty > fuzzystate->distance)
		return;

	/*
	 * 直接拒绝已删除列；此类列在此处表现为实际        
	 * 名称为空，理由见 scanRTEForColumn() 的说明。  
	 */
	if (actual[0] == '\0')
		return;

	/* 用 Levenshtein 计算匹配距离。      */
	matchlen = strlen(match);
	columndistance =
		varstr_levenshtein_less_equal(actual, strlen(actual), match, matchlen,
									  1, 1, 1,
									  fuzzystate->distance + 1
									  - fuzzy_rte_penalty,
									  true);

	/*
	 * 若超过一半的字符不同，就不要将其视为匹配，    
	 * 以免给出荒谬的建议。                
	 */
	if (columndistance > matchlen / 2)
		return;

	/*
	 * 从这里开始，我们可以忽略 RTE 名距离与列              
	 * 名距离之间的区别。           
	 */
	columndistance += fuzzy_rte_penalty;

	/*
	 * 若新距离小于等于迄今找到的最佳匹配的距离，    
	 * 则更新 fuzzystate。         
	 */
	if (columndistance < fuzzystate->distance)
	{
		/* 将新观测到的最低距离存为首/唯一匹配   */
		fuzzystate->distance = columndistance;
		fuzzystate->rfirst = rte;
		fuzzystate->first = attnum;
		fuzzystate->rsecond = NULL;
	}
	else if (columndistance == fuzzystate->distance)
	{
		/* 若已有该距离的匹配，则更新状态             */
		if (fuzzystate->rsecond != NULL)
		{
			/*
			 * 相同距离的匹配太多。这个 distance 值显然   
			 * 定得太低，因此丢弃这些条目，同时保留当前  
			 * distance 值，这样只有更小的距离才会被视为      
			 * 有意义的。只有找到距离更小的对象时，        
			 * 才会重新填充 rfirst（通过上面的代码段）。 
			 */
			fuzzystate->rfirst = NULL;
			fuzzystate->rsecond = NULL;
		}
		else if (fuzzystate->rfirst != NULL)
		{
			/* 记录为临时第二匹配        */
			fuzzystate->rsecond = rte;
			fuzzystate->second = attnum;
		}
		else
		{
			/*
			 * 什么也不做。当 rfirst 为 NULL 时，distance 已超出 
			 * 我们可接受的范围，故应忽略此匹配。         
			 */
		}
	}
}

/*
 * scanNSItemForColumn
 *	  在单个命名空间条目的列名中搜索给定名称。          
 *	  若找到则返回相应 Var 节点，否则 NULL。       
 *	  若该名称在此 nsitem 内有歧义，则报错。        
 *
 * 副作用：若找到匹配，则将相应的 RTE 标记为             
 * 需要该列的读权限。
 */
Node *
scanNSItemForColumn(ParseState *pstate, ParseNamespaceItem *nsitem,
					int sublevels_up, const char *colname, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	int			attnum;
	Var		   *var;

	/*
	 * 扫描 nsitem 列名（或别名）以寻找匹配，若多个匹配 
	 * 则报错。     
	 */
	attnum = scanRTEForColumn(pstate, rte, nsitem->p_names,
							  colname, location,
							  0, NULL);

	if (attnum == InvalidAttrNumber)
		return NULL;			/* 无匹配则返回 NULL */

	/* 在约束检查中，除 tableOid 外不允许任何系统列    */
	if (pstate->p_expr_kind == EXPR_KIND_CHECK_CONSTRAINT &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("system column \"%s\" reference in check constraint is invalid",
						colname),
				 parser_errposition(pstate, location)));

	/* 在生成列中，除 tableOid 外不允许任何系统列       */
	if (pstate->p_expr_kind == EXPR_KIND_GENERATED_COLUMN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in column generation expression",
						colname),
				 parser_errposition(pstate, location)));

	/*
	 * 在 MERGE WHEN 条件中，除 tableOid 外不允许系统列         
	 */
	if (pstate->p_expr_kind == EXPR_KIND_MERGE_WHEN &&
		attnum < InvalidAttrNumber && attnum != TableOidAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use system column \"%s\" in MERGE WHEN condition",
						colname),
				 parser_errposition(pstate, location)));

	/* 找到有效匹配，故构建 Var  */
	if (attnum > InvalidAttrNumber)
	{
		/* 从 ParseNamespaceColumn 数组获取属性数据      */
		ParseNamespaceColumn *nscol = &nsitem->p_nscolumns[attnum - 1];

		/* 已删除列则报错，见 scanRTEForColumn 说明。      */
		if (nscol->p_varno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname,
							nsitem->p_names->aliasname)));

		var = makeVar(nscol->p_varno,
					  nscol->p_varattno,
					  nscol->p_vartype,
					  nscol->p_vartypmod,
					  nscol->p_varcollid,
					  sublevels_up);
		/* makeVar 未提供这些参数，所以手动设置：            */
		var->varnosyn = nscol->p_varnosyn;
		var->varattnosyn = nscol->p_varattnosyn;
	}
	else
	{
		/* 系统列，因此使用预定的类型数据 */
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attnum);
		var = makeVar(nsitem->p_rtindex,
					  attnum,
					  sysatt->atttypid,
					  sysatt->atttypmod,
					  sysatt->attcollation,
					  sublevels_up);
	}
	var->location = location;

	/* 若 Var 被任何外连接置空则标记  */
	markNullableIfNeeded(pstate, var);

	/* 需要对该列的读权限       */
	markVarForSelectPriv(pstate, var);

	return (Node *) var;
}

/*
 * scanRTEForColumn
 *	  在单个 RTE 的列名中搜索给定名称。            
 *	  若找到则返回 attnum（系统列可能为负），否则返回  
 *	  InvalidAttrNumber。          
 *	  若名称在此 RTE 内有歧义，则报错。           
 *
 * 实际上，我们只搜索 "eref" 中列出的名称。它可以是    
 * rte->eref（此时我们确实在搜索所有列名），             
 * 对于连接也可以是 rte->join_using_alias，此时我们           
 * 只考虑公共列名（它们是连接的前 N 列，因此             
 * 一切正常）。         
 *
 * pstate 和 location 仅出于错误报告目的而传入。      
 *
 * 副作用：若 fuzzystate 非 NULL，则检查非系统列是  
 * 否有近似匹配，并相应更新 fuzzystate。         
 *
 * 注：此函数从 scanNSItemForColumn 中拆分出来，因为        
 * 错误消息的创建可能需要检查不在命名空间内的           
 * RTE。为支持这种用法，这里要尽量少做有效性检查。   
 * 不过，报告名称歧义的情况是可以的，因为若我们  
 * 正要报告某个无效名称，那已经排除了这种情况。       
 */
static int
scanRTEForColumn(ParseState *pstate, RangeTblEntry *rte,
				 Alias *eref,
				 const char *colname, int location,
				 int fuzzy_rte_penalty,
				 FuzzyAttrMatchState *fuzzystate)
{
	int			result = InvalidAttrNumber;
	int			attnum = 0;
	ListCell   *c;

	/*
	 * 扫描用户列名（或别名）寻找匹配；若多个匹配 
	 * 则报错。     
	 *
	 * 注：eref->colnames 可能包含已删除列的条目，但那些     
	 * 条目是空字符串，无法匹配任何合法 SQL 标识符，      
	 * 故此处不必为此专门检测。    
	 *
	 * 即便万一出现差错而去访问已删除列，                
	 * scanNSItemForColumn() 中的检查仍会将其捕获。              
	 * 不过，想要寻找最短距离匹配的调用者需要         
	 * 直接防范这种情况。          
	 */
	foreach(c, eref->colnames)
	{
		const char *attcolname = strVal(lfirst(c));

		attnum++;
		if (strcmp(attcolname, colname) == 0)
		{
			if (result)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous",
								colname),
						 parser_errposition(pstate, location)));
			result = attnum;
		}

		/* 若提供则更新模糊匹配状态   */
		if (fuzzystate != NULL)
			updateFuzzyAttrMatchState(fuzzy_rte_penalty, fuzzystate,
									  rte, attcolname, colname, attnum);
	}

	/*
	 * 若存在唯一匹配则返回之。注意这允许用户别名    
	 * 无错地覆盖系统列名（例如 OID）。                     
	 */
	if (result)
		return result;

	/*
	 * 若 RTE 代表真实关系，则考虑系统列名。               
	 * 复合类型仅用于 ON CONFLICT 的 excluded 这类伪        
	 * 关系。
	 */
	if (rte->rtekind == RTE_RELATION &&
		rte->relkind != RELKIND_COMPOSITE_TYPE)
	{
		/* 快速检查名称是否可能是系统列          */
		attnum = specialAttNum(colname);
		if (attnum != InvalidAttrNumber)
		{
			/* 然后检查该列是否确实已定义        */
			if (SearchSysCacheExists2(ATTNUM,
									  ObjectIdGetDatum(rte->relid),
									  Int16GetDatum(attnum)))
				result = attnum;
		}
	}

	return result;
}

/*
 * colNameToVar
 *	  搜索不带限定的列名。        
 *	  若找到则返回相应 Var 节点（或表达式）。  
 *	  若未找到则返回 NULL；若名称有歧义则报错。           
 *	  若 localonly 为真，则只考虑最内层查询中的名称。      
 */
Node *
colNameToVar(ParseState *pstate, const char *colname, bool localonly,
			 int location)
{
	Node	   *result = NULL;
	int			sublevels_up = 0;
	ParseState *orig_pstate = pstate;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);
			Node	   *newresult;

			/* 忽略仅表条目      */
			if (!nsitem->p_cols_visible)
				continue;
			/* 若不在 LATERAL 内，则忽略仅横向条目 */
			if (nsitem->p_lateral_only && !pstate->p_lateral_active)
				continue;

			/* 此处用 orig_pstate 以与其他调用者保持一致 */
			newresult = scanNSItemForColumn(orig_pstate, nsitem, sublevels_up,
											colname, location);

			if (newresult)
			{
				if (result)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("column reference \"%s\" is ambiguous",
									colname),
							 parser_errposition(pstate, location)));
				check_lateral_ref_ok(pstate, nsitem, location);
				result = newresult;
			}
		}

		if (result != NULL || localonly)
			break;				/* 已找到，或不想查看父查询   */

		pstate = pstate->parentParseState;
		sublevels_up++;
	}

	return result;
}

/*
 * searchRangeTableForCol
 *	  查看是否有 RangeTblEntry 可能提供给定的列名（或          
 *	  找到可用的最佳匹配）。返回带有相关细节的状态。
 *
 * 它与 colNameToVar 的不同在于：它会考虑                     
 * ParseState 中所有的范围表条目，而不只是当前在           
 * p_namespace 列表中可见的那些。按 SQL 标准这一            
 * 行为无效，且可能给出歧义结果（可能存在多个         
 * 同样有效的匹配）。它只能用作报错启发，参见          
 * errorMissingColumn。                   
 *
 * 此函数的另一不同之处是它会考虑近似匹配——     
 * 若用户输入的别名/列名组合与有效组合略有不同，      
 * 我们或许能推断出其想输入的内容并给出合理提示。  
 * 我们返回一个 FuzzyAttrMatchState 结构，提供              
 * 精确匹配与近似匹配两类信息。                            
 */
static FuzzyAttrMatchState *
searchRangeTableForCol(ParseState *pstate, const char *alias, const char *colname,
					   int location)
{
	ParseState *orig_pstate = pstate;
	FuzzyAttrMatchState *fuzzystate = palloc(sizeof(FuzzyAttrMatchState));

	fuzzystate->distance = MAX_FUZZY_DISTANCE + 1;
	fuzzystate->rfirst = NULL;
	fuzzystate->rsecond = NULL;
	fuzzystate->rexact1 = NULL;
	fuzzystate->rexact2 = NULL;

	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
			int			fuzzy_rte_penalty = 0;
			int			attnum;

			/*
			 * 通常，在连接 RTE 内部寻找匹配没什么用；   
			 * 就我们的目的而言，它们实际上复制了其他    
			 * RTE。而若从连接 RTE 中选定匹配，最终诊断       
			 * 消息中显示的别名无助于诊断。
			 */
			if (rte->rtekind == RTE_JOIN)
				continue;

			/*
			 * 若用户未指定别名，那么与哪个 RTE 匹配都     
			 * 一样。但若用户指定别名，则我们希望范围  
			 * 表条目至少是模糊匹配——最好是精确匹配。  
			 *                                   
			 */
			if (alias != NULL)
				fuzzy_rte_penalty =
					varstr_levenshtein_less_equal(alias, strlen(alias),
												  rte->eref->aliasname,
												  strlen(rte->eref->aliasname),
												  1, 1, 1,
												  MAX_FUZZY_DISTANCE + 1,
												  true);

			/*
			 * 扫描匹配的列并更新 fuzzystate。非精确匹配     
			 * 在 scanRTEForColumn 内部处理，但精确匹配      
			 * 则在此处处理。（同一个 RTE 内不会有超过      
			 * 一个精确匹配，否则我们早已报错。）         
			 */
			attnum = scanRTEForColumn(orig_pstate, rte, rte->eref,
									  colname, location,
									  fuzzy_rte_penalty, fuzzystate);
			if (attnum != InvalidAttrNumber && fuzzy_rte_penalty == 0)
			{
				if (fuzzystate->rexact1 == NULL)
				{
					fuzzystate->rexact1 = rte;
					fuzzystate->exact1 = attnum;
				}
				else
				{
					/* 无需担心覆盖之前的 rexact2              */
					fuzzystate->rexact2 = rte;
					fuzzystate->exact2 = attnum;
				}
			}
		}

		pstate = pstate->parentParseState;
	}

	return fuzzystate;
}

/*
 * markNullableIfNeeded
 *		若 Var 引用的 RTE 在查询中此点可被外连接      
 *		置空，则设置 var->varnullingrels 以表明这一点。     
 */
void
markNullableIfNeeded(ParseState *pstate, Var *var)
{
	int			rtindex = var->varno;
	Bitmapset  *relids;

	/* 找到合适的 pstate      */
	for (int lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;

	/* 找到与 Var 的关系当前相关的连接 relid    */
	if (rtindex > 0 && rtindex <= list_length(pstate->p_nullingrels))
		relids = (Bitmapset *) list_nth(pstate->p_nullingrels, rtindex - 1);
	else
		relids = NULL;

	/*
	 * 与任何已声明的置空关系合并。（通常不会有，      
	 * 但若有就把它做对。）               
	 */
	if (relids != NULL)
		var->varnullingrels = bms_union(var->varnullingrels, relids);
}

/*
 * markRTEForSelectPriv
 *	   将索引为 rtindex 的 RTE 的指定列标记为     
 *	   需要 SELECT 权限         
 *
 * col == InvalidAttrNumber 表示"整行"引用         
 */
static void
markRTEForSelectPriv(ParseState *pstate, int rtindex, AttrNumber col)
{
	RangeTblEntry *rte = rt_fetch(rtindex, pstate->p_rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		RTEPermissionInfo *perminfo;

		/* 确保该关系整体被标记为需要 SELECT 访问    */
		perminfo = getRTEPermissionInfo(pstate->p_rteperminfos, rte);
		perminfo->requiredPerms |= ACL_SELECT;
		/* 必须偏移 attnum 才能放进 bitmapset   */
		perminfo->selectedCols =
			bms_add_member(perminfo->selectedCols,
						   col - FirstLowInvalidHeapAttributeNumber);
	}
	else if (rte->rtekind == RTE_JOIN)
	{
		if (col == InvalidAttrNumber)
		{
			/*
			 * 对连接的整行引用必须被视为对两个输入的     
			 * 整行引用。              
			 */
			JoinExpr   *j;

			if (rtindex > 0 && rtindex <= list_length(pstate->p_joinexprs))
				j = list_nth_node(JoinExpr, pstate->p_joinexprs, rtindex - 1);
			else
				j = NULL;
			if (j == NULL)
				elog(ERROR, "could not find JoinExpr for whole-row reference");

			/* 注：这里看不到 FromExpr   */
			if (IsA(j->larg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->larg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->larg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->larg));
			if (IsA(j->rarg, RangeTblRef))
			{
				int			varno = ((RangeTblRef *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else if (IsA(j->rarg, JoinExpr))
			{
				int			varno = ((JoinExpr *) j->rarg)->rtindex;

				markRTEForSelectPriv(pstate, varno, InvalidAttrNumber);
			}
			else
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(j->rarg));
		}
		else
		{
			/*
			 * 普通列的连接别名 Var 必须引用已合并的 JOIN    
			 * USING 列。这里无需做任何处理，因为连接输入  
			 * 列也会出现在连接的 qual 子句中，并会在那里 
			 * 被标记需要 SELECT 特权。                       
			 */
		}
	}
	/* 其他 RTE 类型无需特权标记             */
}

/*
 * markVarForSelectPriv
 *	   将 Var 引用的 RTE 标记为需要其列上的 SELECT 权限 
 *	   （Var 也可能是整行 Var）                            
 */
void
markVarForSelectPriv(ParseState *pstate, Var *var)
{
	Index		lv;

	Assert(IsA(var, Var));
	/* 如果是上层 Var，找到合适的 pstate       */
	for (lv = 0; lv < var->varlevelsup; lv++)
		pstate = pstate->parentParseState;
	markRTEForSelectPriv(pstate, var->varno, var->varattno);
}

/*
 * buildRelationAliases
 *		为关系 RTE 构造 eref 列名列表。              
 *		此代码也用于函数 RTE。          
 *
 * tupdesc：物理列信息               
 * alias：用户提供的别名，无则为 NULL 
 * eref：存储列名的 eref Alias            
 *
 * 填充 eref->colnames。同时重建 alias->colnames，插入           
 * 空串以表示每个被删除的列，从而与物理列号             
 * 一一对应。         
 *
 * 若提供的别名多于所需数目，将报错。               
 */
static void
buildRelationAliases(TupleDesc tupdesc, Alias *alias, Alias *eref)
{
	int			maxattrs = tupdesc->natts;
	List	   *aliaslist;
	ListCell   *aliaslc;
	int			numaliases;
	int			varattno;
	int			numdropped = 0;

	Assert(eref->colnames == NIL);

	if (alias)
	{
		aliaslist = alias->colnames;
		aliaslc = list_head(aliaslist);
		numaliases = list_length(aliaslist);
		/* 我们将重建别名的列名列表 */
		alias->colnames = NIL;
	}
	else
	{
		aliaslist = NIL;
		aliaslc = NULL;
		numaliases = 0;
	}

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);
		String	   *attrname;

		if (attr->attisdropped)
		{
			/* 对被删除的列总是插入一个空串         */
			attrname = makeString(pstrdup(""));
			if (aliaslc)
				alias->colnames = lappend(alias->colnames, attrname);
			numdropped++;
		}
		else if (aliaslc)
		{
			/* 使用下一个用户别名      */
			attrname = lfirst_node(String, aliaslc);
			aliaslc = lnext(aliaslist, aliaslc);
			alias->colnames = lappend(alias->colnames, attrname);
		}
		else
		{
			attrname = makeString(pstrdup(NameStr(attr->attname)));
			/* 若有别名则到此用完      */
		}

		eref->colnames = lappend(eref->colnames, attrname);
	}

	/* 用户别名太多了？        */
	if (aliaslc)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, maxattrs - numdropped, numaliases)));
}

/*
 * chooseScalarFunctionAlias
 *		为函数 RTE 中的函数选择列别名，             
 *		当函数返回标量类型（非复合或 RECORD）时。        
 *
 * funcexpr：函数调用转换后的表达式树            
 * funcname：函数名（由 FigureColname 确定）      
 * alias：RTE 的用户别名，无则为 NULL               
 * nfuncs：函数 RTE 中出现的函数数量                 
 *
 * 注意：若用户给出的别名包含列别名，我们所选的名字 
 * 之后可能被覆盖，此处无需关心。                 
 */
static char *
chooseScalarFunctionAlias(Node *funcexpr, char *funcname,
						  Alias *alias, int nfuncs)
{
	char	   *pname;

	/*
	 * 若表达式为简单函数调用，且函数只有单个具名的 
	 * OUT 参数，则使用该参数名。                        
	 */
	if (funcexpr && IsA(funcexpr, FuncExpr))
	{
		pname = get_func_result_name(((FuncExpr *) funcexpr)->funcid);
		if (pname)
			return pname;
	}

	/*
	 * 若 RTE 中只有一个函数，且用户给出了 RTE 的             
	 * 别名，则用该名。（这使 FROM func() AS foo 的列名       
	 * 和表别名同为 "foo"。）          
	 */
	if (nfuncs == 1 && alias)
		return alias->aliasname;

	/*
	 * 否则使用函数名。        
	 */
	return funcname;
}

/*
 * buildNSItemFromTupleDesc
 *		根据描述各列的 tupdesc 构建 ParseNamespaceItem。         
 *
 * rte：该 rel 的新 RangeTblEntry    
 * rtindex：其在范围表中的下标    
 * perminfo：该 rel 的权限列表条目   
 * tupdesc：物理列信息               
 */
static ParseNamespaceItem *
buildNSItemFromTupleDesc(RangeTblEntry *rte, Index rtindex,
						 RTEPermissionInfo *perminfo,
						 TupleDesc tupdesc)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = tupdesc->natts;
	int			varattno;

	/* colnames 的条目数必须与 nsitem 一致                */
	Assert(maxattrs == list_length(rte->eref->colnames));

	/* 从 tupdesc 提取每列数据           */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	for (varattno = 0; varattno < maxattrs; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

		/* 对被删除的列，条目保持为全零           */
		if (attr->attisdropped)
			continue;

		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = attr->atttypid;
		nscolumns[varattno].p_vartypmod = attr->atttypmod;
		nscolumns[varattno].p_varcollid = attr->attcollation;
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
	}

	/* 随后构建 nsitem      */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = perminfo;
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认可见性标志，之后可能被修改   */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * buildNSItemFromLists
 *		根据列表形式的列类型信息构建 ParseNamespaceItem。   
 *
 * rte：该 rel 的新 RangeTblEntry    
 * rtindex：其在范围表中的下标    
 * coltypes：每列的类型 OID    
 * coltypmods：每列的类型修饰符
 * colcollation：各列排序规则 OID  
 */
static ParseNamespaceItem *
buildNSItemFromLists(RangeTblEntry *rte, Index rtindex,
					 List *coltypes, List *coltypmods, List *colcollations)
{
	ParseNamespaceItem *nsitem;
	ParseNamespaceColumn *nscolumns;
	int			maxattrs = list_length(coltypes);
	int			varattno;
	ListCell   *lct;
	ListCell   *lcm;
	ListCell   *lcc;

	/* colnames 的条目数必须与 nsitem 一致                */
	Assert(maxattrs == list_length(rte->eref->colnames));

	Assert(maxattrs == list_length(coltypmods));
	Assert(maxattrs == list_length(colcollations));

	/* 从这些列表提取每列数据      */
	nscolumns = (ParseNamespaceColumn *)
		palloc0(maxattrs * sizeof(ParseNamespaceColumn));

	varattno = 0;
	forthree(lct, coltypes,
			 lcm, coltypmods,
			 lcc, colcollations)
	{
		nscolumns[varattno].p_varno = rtindex;
		nscolumns[varattno].p_varattno = varattno + 1;
		nscolumns[varattno].p_vartype = lfirst_oid(lct);
		nscolumns[varattno].p_vartypmod = lfirst_int(lcm);
		nscolumns[varattno].p_varcollid = lfirst_oid(lcc);
		nscolumns[varattno].p_varnosyn = rtindex;
		nscolumns[varattno].p_varattnosyn = varattno + 1;
		varattno++;
	}

	/* 随后构建 nsitem      */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_rtindex = rtindex;
	nsitem->p_perminfo = NULL;
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认可见性标志，之后可能被修改   */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * 在解析分析期间打开表    
 *
 * 本质上与 table_openrv() 相同，但它额外满足了                
 * 解析器特有的一些错误报告需求，尤其是让结果        
 * 错误中包含 RangeVar 的解析位置。                     
 *
 * 注意：严格来说 lockmode 应声明为 LOCKMODE 而非           
 * int，但那需要把 storage/lock.h 引入 parse_relation.h。     
 * 由于 LOCKMODE 反正就是 int，那似乎小题大做。   
 */
Relation
parserOpenTable(ParseState *pstate, const RangeVar *relation, int lockmode)
{
	Relation	rel;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, pstate, relation->location);
	rel = table_openrv_extended(relation, lockmode, true);
	if (rel == NULL)
	{
		if (relation->schemaname)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
		{
			/*
			 * 不带限定名的引用可能原本是想指向某个     
			 * 尚未进入作用域的 CTE。仅有"不存在"报错信息 
			 * 对排查此类问题帮助甚微，因此我们费心提供   
			 * 专门的提示。                        
			 */
			if (isFutureCTE(pstate, relation->relname))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname),
						 errdetail("There is a WITH item named \"%s\", but it cannot be referenced from this part of the query.",
								   relation->relname),
						 errhint("Use WITH RECURSIVE, or re-order the WITH items to remove forward references.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" does not exist",
								relation->relname)));
		}
	}
	cancel_parser_errposition_callback(&pcbstate);
	return rel;
}

/*
 * 向 pstate 的范围表（p_rtable）加入关系的条目。      
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 此处不把 ParseNamespaceItem 链入 pstate；这是              
 * 调用者以适当方式完成的工作。     
 *
 * 注意：以前此处检查 refname 冲突，但那是错的。     
 * 调用者负责在适当的作用域内检查冲突。                    
 */
ParseNamespaceItem *
addRangeTableEntry(ParseState *pstate,
				   RangeVar *relation,
				   Alias *alias,
				   bool inh,
				   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : relation->relname;
	LOCKMODE	lockmode;
	Relation	rel;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;

	/*
	 * 确定该关系所需的锁类型。查询目标表的情况        
	 * 在别处处理，所以这里我们需要                       
	 * RowShareLock（若被 FOR UPDATE/SHARE 锁定），否则       
	 * 用 AccessShareLock。    
	 */
	lockmode = isLockedRefname(pstate, refname) ? RowShareLock : AccessShareLock;

	/*
	 * 获取该 rel 的 OID。这次访问也确保我们拥有最新的     
	 * rel 的 relcache 条目。由于这通常是语句中对             
	 * 关系的首次访问，必须以正确的 lockmode 打开它。       
	 */
	rel = parserOpenTable(pstate, relation, lockmode);
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * 使用用户提供的别名和/或实际列名，构建有效的    
	 * 列名列表。            
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * 设置标志并初始化访问权限。     
	 *
	 * 访问检查的初始默认总是检查是否可读，               
	 * 这对目标表以外的所有情况都正确。      
	 */
	rte->lateral = false;
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	nsitem = buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									  perminfo, rel->rd_att);

	/*
	 * 递减 rel 的引用计数，但把访问锁保留到事务结束，   
	 * 使表无法被删除或在背后被修改模式，           
	 *               
	 */
	table_close(rel, NoLock);

	return nsitem;
}

/*
 * 向 pstate 的范围表（p_rtable）加入关系的条目。      
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 这与 addRangeTableEntry() 基本相同，只是它用           
 * 已打开的关系而非 RangeVar 引用来构造 RTE。       
 *
 * lockmode 是查询执行所需的锁类型；它必须是             
 * AccessShareLock、RowShareLock 或 RowExclusiveLock                   
 * 之一，视 RTE 在查询中的角色。调用者须已持有   
 * 或更强。      
 *
 * 注意：严格来说 lockmode 应声明为 LOCKMODE 而非           
 * int，但那需要把 storage/lock.h 引入 parse_relation.h。     
 * 由于 LOCKMODE 反正就是 int，那似乎小题大做。   
 */
ParseNamespaceItem *
addRangeTableEntryForRelation(ParseState *pstate,
							  Relation rel,
							  int lockmode,
							  Alias *alias,
							  bool inh,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo;
	char	   *refname = alias ? alias->aliasname : RelationGetRelationName(rel);

	Assert(pstate != NULL);

	Assert(lockmode == AccessShareLock ||
		   lockmode == RowShareLock ||
		   lockmode == RowExclusiveLock);
	Assert(CheckRelationLockedByMe(rel, lockmode, true));

	rte->rtekind = RTE_RELATION;
	rte->alias = alias;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = lockmode;

	/*
	 * 使用用户提供的别名和/或实际列名，构建有效的    
	 * 列名列表。            
	 */
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(rel->rd_att, alias, rte->eref);

	/*
	 * 设置标志并初始化访问权限。     
	 *
	 * 访问检查的初始默认总是检查是否可读，               
	 * 这对目标表以外的所有情况都正确。      
	 */
	rte->lateral = false;
	rte->inh = inh;
	rte->inFromCl = inFromCl;

	perminfo = addRTEPermissionInfo(&pstate->p_rteperminfos, rte);
	perminfo->requiredPerms = ACL_SELECT;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable),
									perminfo, rel->rd_att);
}

/*
 * 向 pstate 的范围表（p_rtable）加入子查询的条目。   
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 这与 addRangeTableEntry() 很相似，只是构造子查询 RTE。       
 *
 * 若子查询没有别名，返回的 ParseNamespaceItem 中自动          
 * 生成的关系名将被标记为不可见，因此仅允许              
 * 对子查询列的不带限定引用，且该关系名不会           
 * 与 pstate 命名空间列表中的其他名字冲突。                   
 */
ParseNamespaceItem *
addRangeTableEntryForSubquery(ParseState *pstate,
							  Query *subquery,
							  Alias *alias,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	List	   *coltypes,
			   *coltypmods,
			   *colcollations;
	int			varattno;
	ListCell   *tlistitem;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = subquery;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_subquery", NIL);
	numaliases = list_length(eref->colnames);

	/* 填充所有未指定的别名列，并提取列类型信息        */
	coltypes = coltypmods = colcollations = NIL;
	varattno = 0;
	foreach(tlistitem, subquery->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

		if (te->resjunk)
			continue;
		varattno++;
		Assert(varattno == te->resno);
		if (varattno > numaliases)
		{
			char	   *attrname;

			attrname = pstrdup(te->resname);
			eref->colnames = lappend(eref->colnames, makeString(attrname));
		}
		coltypes = lappend_oid(coltypes,
							   exprType((Node *) te->expr));
		coltypmods = lappend_int(coltypmods,
								 exprTypmod((Node *) te->expr));
		colcollations = lappend_oid(colcollations,
									exprCollation((Node *) te->expr));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, varattno, numaliases)));

	rte->eref = eref;

	/*
	 * 设标志 
	 *
	 * 子查询从不检查访问权限，因此无需执行               
	 * addRTEPermissionInfo() 
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* 对子查询永不为真  */
	rte->inFromCl = inFromCl;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	nsitem = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								  coltypes, coltypmods, colcollations);

	/*
	 * 仅当它有用户手写的别名时，才标记为关系名可见。  
	 */
	nsitem->p_rel_visible = (alias != NULL);

	return nsitem;
}

/*
 * 向 pstate 的范围表（p_rtable）加入函数（一个或         
 * 多个）的条目。然后为新 RTE 构造并返回 ParseNamespaceItem。  
 *
 * 这与 addRangeTableEntry() 很相似，只是构造函数 RTE。          
 */
ParseNamespaceItem *
addRangeTableEntryForFunction(ParseState *pstate,
							  List *funcnames,
							  List *funcexprs,
							  List *coldeflists,
							  RangeFunction *rangefunc,
							  bool lateral,
							  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rangefunc->alias;
	Alias	   *eref;
	char	   *aliasname;
	int			nfuncs = list_length(funcexprs);
	TupleDesc  *functupdescs;
	TupleDesc	tupdesc;
	ListCell   *lc1,
			   *lc2,
			   *lc3;
	int			i;
	int			j;
	int			funcno;
	int			natts,
				totalatts;

	Assert(pstate != NULL);

	rte->rtekind = RTE_FUNCTION;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->functions = NIL;		/* 该列表稍后填充      */
	rte->funcordinality = rangefunc->ordinality;
	rte->alias = alias;

	/*
	 * 选择 RTE 别名。默认使用第一个函数的名字，         
	 * 即使函数不止一个；这或许有争议，但总好过           
	 * 用"table"这类固定名。          
	 */
	if (alias)
		aliasname = alias->aliasname;
	else
		aliasname = linitial(funcnames);

	eref = makeAlias(aliasname, NIL);
	rte->eref = eref;

	/* 逐个处理各函数     */
	functupdescs = (TupleDesc *) palloc(nfuncs * sizeof(TupleDesc));

	totalatts = 0;
	funcno = 0;
	forthree(lc1, funcexprs, lc2, funcnames, lc3, coldeflists)
	{
		Node	   *funcexpr = (Node *) lfirst(lc1);
		char	   *funcname = (char *) lfirst(lc2);
		List	   *coldeflist = (List *) lfirst(lc3);
		RangeTblFunction *rtfunc = makeNode(RangeTblFunction);
		TypeFuncClass functypclass;
		Oid			funcrettype;

		/* 初始化 RangeTblFunction       */
		rtfunc->funcexpr = funcexpr;
		rtfunc->funccolnames = NIL;
		rtfunc->funccoltypes = NIL;
		rtfunc->funccoltypmods = NIL;
		rtfunc->funccolcollations = NIL;
		rtfunc->funcparams = NULL;	/* 规划期才设置     */

		/*
		 * 现在确定函数返回的是简单类型还是复合类型。  
		 */
		functypclass = get_expr_result_type(funcexpr,
											&funcrettype,
											&tupdesc);

		/*
		 * 若函数返回 RECORD 且没有预定记录类型，则需要    
		 * coldeflist，否则禁止使用。这一点可能令人困惑，   
		 * 因此我们会在给出相关错误信息上花些功夫。    
		 *                        
		 */
		if (coldeflist != NIL)
		{
			switch (functypclass)
			{
				case TYPEFUNC_RECORD:
					/* 可*/
					break;
				case TYPEFUNC_COMPOSITE:
				case TYPEFUNC_COMPOSITE_DOMAIN:

					/*
					 * 若函数的原始结果类型为 RECORD，则必须
					 * 已用其 OUT 参数把它解析出来。否则，    
					 * 它必须有具名复合类型。   
					 */
					if (exprType(funcexpr) == RECORDOID)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function with OUT parameters"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					else
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("a column definition list is redundant for a function returning a named composite type"),
								 parser_errposition(pstate,
													exprLocation((Node *) coldeflist))));
					break;
				default:
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("a column definition list is only allowed for functions returning \"record\""),
							 parser_errposition(pstate,
												exprLocation((Node *) coldeflist))));
					break;
			}
		}
		else
		{
			if (functypclass == TYPEFUNC_RECORD)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("a column definition list is required for functions returning \"record\""),
						 parser_errposition(pstate, exprLocation(funcexpr))));
		}

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* 复合类型，例如表的行类型         */
			Assert(tupdesc);
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* 基础类型，即标量    */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   chooseScalarFunctionAlias(funcexpr, funcname,
														 alias, nfuncs),
							   funcrettype,
							   exprTypmod(funcexpr),
							   0);
			TupleDescInitEntryCollation(tupdesc,
										(AttrNumber) 1,
										exprCollation(funcexpr));
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			ListCell   *col;

			/*
			 * 用列定义列表构造 tupdesc，并填充                  
			 * RangeTblFunction 的各列表。列数上限为             
			 * MaxHeapAttributeNumber，CheckAttributeNamesTypes 亦然。   
			 */
			if (list_length(coldeflist) > MaxHeapAttributeNumber)
				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_COLUMNS),
						 errmsg("column definition lists can have at most %d entries",
								MaxHeapAttributeNumber),
						 parser_errposition(pstate,
											exprLocation((Node *) coldeflist))));
			tupdesc = CreateTemplateTupleDesc(list_length(coldeflist));
			i = 1;
			foreach(col, coldeflist)
			{
				ColumnDef  *n = (ColumnDef *) lfirst(col);
				char	   *attrname;
				Oid			attrtype;
				int32		attrtypmod;
				Oid			attrcollation;

				attrname = n->colname;
				if (n->typeName->setof)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("column \"%s\" cannot be declared SETOF",
									attrname),
							 parser_errposition(pstate, n->location)));
				typenameTypeIdAndMod(pstate, n->typeName,
									 &attrtype, &attrtypmod);
				attrcollation = GetColumnDefCollation(pstate, n, attrtype);
				TupleDescInitEntry(tupdesc,
								   (AttrNumber) i,
								   attrname,
								   attrtype,
								   attrtypmod,
								   0);
				TupleDescInitEntryCollation(tupdesc,
											(AttrNumber) i,
											attrcollation);
				rtfunc->funccolnames = lappend(rtfunc->funccolnames,
											   makeString(pstrdup(attrname)));
				rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes,
												   attrtype);
				rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods,
													 attrtypmod);
				rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
														attrcollation);

				i++;
			}

			/*
			 * 确保 coldeflist 定义出合法的名字集合（不得  
			 * 重复，但无需担心系统列名）以及                  
			 * 数据类型合法。虽然多数伪类型不可用，但  
			 * 允许 RECORD 与 RECORD[] 似乎安全，因为这些      
			 * 类型类别中的值在运行时可自我识别，而     
			 * coldeflist 并不代表任何其他会话可见的内容。  
			 *                
			 */
			CheckAttributeNamesTypes(tupdesc, RELKIND_COMPOSITE_TYPE,
									 CHKATYPE_ANYRECORD);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function \"%s\" in FROM has unsupported return type %s",
							funcname, format_type_be(funcrettype)),
					 parser_errposition(pstate, exprLocation(funcexpr))));

		/* 完成 RangeTblFunction 并把它加入 RTE 的列表        */
		rtfunc->funccolcount = tupdesc->natts;
		rte->functions = lappend(rte->functions, rtfunc);

		/* 保存 tupdesc 供下文使用 */
		functupdescs[funcno] = tupdesc;
		totalatts += tupdesc->natts;
		funcno++;
	}

	/*
	 * 若有多个函数，或者想要 ordinality（序号）列，        
	 * 就必须生成合并 tupdesc。 
	 */
	if (nfuncs > 1 || rangefunc->ordinality)
	{
		if (rangefunc->ordinality)
			totalatts++;

		/* 禁止列数超过元组可容纳的上限     */
		if (totalatts > MaxTupleAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("functions in FROM can return at most %d columns",
							MaxTupleAttributeNumber),
					 parser_errposition(pstate,
										exprLocation((Node *) funcexprs))));

		/* 把各函数的 tupdesc 合并为一个复合 tupdesc       */
		tupdesc = CreateTemplateTupleDesc(totalatts);
		natts = 0;
		for (i = 0; i < nfuncs; i++)
		{
			for (j = 1; j <= functupdescs[i]->natts; j++)
				TupleDescCopyEntry(tupdesc, ++natts, functupdescs[i], j);
		}

		/* 若需要则加入 ordinality 列   */
		if (rangefunc->ordinality)
		{
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) ++natts,
							   "ordinality",
							   INT8OID,
							   -1,
							   0);
			/* 无需设置排序规则 */
		}

		Assert(natts == totalatts);
	}
	else
	{
		/* 直接原样使用单个函数的 tupdesc           */
		tupdesc = functupdescs[0];
	}

	/* 为 RTE 分配列别名时使用该 tupdesc                */
	buildRelationAliases(tupdesc, alias, eref);

	/*
	 * 设置标志和访问权限。   
	 *
	 * 函数从不检查访问权限（至少 ExecCheckPermissions() 
	 * 不做此项检查），因此无需执行 addRTEPermissionInfo()。  
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* 对函数永不为真    */
	rte->inFromCl = inFromCl;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}

/*
 * 向 pstate 的范围表（p_rtable）加入表函数的条目。         
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 这与 addRangeTableEntry() 很相似，只是构造 tablefunc RTE。       
 */
ParseNamespaceItem *
addRangeTableEntryForTableFunc(ParseState *pstate,
							   TableFunc *tf,
							   Alias *alias,
							   bool lateral,
							   bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname;
	Alias	   *eref;
	int			numaliases;

	Assert(pstate != NULL);

	/* 禁止列数超过元组可容纳的上限     */
	if (list_length(tf->colnames) > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("functions in FROM can return at most %d columns",
						MaxTupleAttributeNumber),
				 parser_errposition(pstate,
									exprLocation((Node *) tf))));
	Assert(list_length(tf->coltypes) == list_length(tf->colnames));
	Assert(list_length(tf->coltypmods) == list_length(tf->colnames));
	Assert(list_length(tf->colcollations) == list_length(tf->colnames));

	refname = alias ? alias->aliasname : pstrdup("xmltable");

	rte->rtekind = RTE_TABLEFUNC;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->tablefunc = tf;
	rte->coltypes = tf->coltypes;
	rte->coltypmods = tf->coltypmods;
	rte->colcollations = tf->colcollations;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* 填充所有未指定的别名列     */
	if (numaliases < list_length(tf->colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(tf->colnames, numaliases));

	if (numaliases > list_length(tf->colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("%s function has %d columns available but %d columns specified",
						"XMLTABLE",
						list_length(tf->colnames), numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志和访问权限。   
	 *
	 * 表函数从不检查权限（至少 ExecCheckPermissions()     
	 * 不做此项检查），因此无需执行 addRTEPermissionInfo()。  
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* 对 tablefunc RTE 永不为真*/
	rte->inFromCl = inFromCl;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * 向 pstate 的范围表（p_rtable）加入 VALUES 列表的条目。 
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 这与 addRangeTableEntry() 很相似，只是构造 values RTE。       
 */
ParseNamespaceItem *
addRangeTableEntryForValues(ParseState *pstate,
							List *exprs,
							List *coltypes,
							List *coltypmods,
							List *colcollations,
							Alias *alias,
							bool lateral,
							bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	char	   *refname = alias ? alias->aliasname : pstrdup("*VALUES*");
	Alias	   *eref;
	int			numaliases;
	int			numcolumns;

	Assert(pstate != NULL);

	rte->rtekind = RTE_VALUES;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->values_lists = exprs;
	rte->coltypes = coltypes;
	rte->coltypmods = coltypmods;
	rte->colcollations = colcollations;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias(refname, NIL);

	/* 填充所有未指定的别名列     */
	numcolumns = list_length((List *) linitial(exprs));
	numaliases = list_length(eref->colnames);
	while (numaliases < numcolumns)
	{
		char		attrname[64];

		numaliases++;
		snprintf(attrname, sizeof(attrname), "column%d", numaliases);
		eref->colnames = lappend(eref->colnames,
								 makeString(pstrdup(attrname)));
	}
	if (numcolumns < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("VALUES lists \"%s\" have %d columns available but %d columns specified",
						refname, numcolumns, numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志和访问权限。   
	 *
	 * 子查询从不检查访问权限，因此无需执行               
	 * addRTEPermissionInfo() 
	 */
	rte->lateral = lateral;
	rte->inh = false;			/* 对 values RTE 永不为真*/
	rte->inFromCl = inFromCl;

	/*
	 * 把完成的 RTE 加入 pstate 的范围表列表，以便知道其
	 * 下标。若合适须由调用者加入连接列表，此处不加。  
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构建 ParseNamespaceItem，但不把它链入 pstate 的             
	 * 命名空间列表；由调用者完成。  
	 */
	return buildNSItemFromLists(rte, list_length(pstate->p_rtable),
								rte->coltypes, rte->coltypmods,
								rte->colcollations);
}

/*
 * 向 pstate 的范围表（p_rtable）加入连接的条目。  
 * 然后为新 RTE 构造并返回 ParseNamespaceItem。          
 *
 * 这与 addRangeTableEntry() 很相似，只是构造连接 RTE。      
 * 此外，由调用者构造 ParseNamespaceColumn 数组   
 * 更为方便，因此我们将其传入。     
 */
ParseNamespaceItem *
addRangeTableEntryForJoin(ParseState *pstate,
						  List *colnames,
						  ParseNamespaceColumn *nscolumns,
						  JoinType jointype,
						  int nummergedcols,
						  List *aliasvars,
						  List *leftcols,
						  List *rightcols,
						  Alias *join_using_alias,
						  Alias *alias,
						  bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *eref;
	int			numaliases;
	ParseNamespaceItem *nsitem;

	Assert(pstate != NULL);

	/*
	 * 若连接的列过多则报错 —— 必须能用 AttrNumber         
	 * 引用其中任意一列。       
	 */
	if (list_length(aliasvars) > MaxAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("joins can have at most %d columns",
						MaxAttrNumber)));

	rte->rtekind = RTE_JOIN;
	rte->relid = InvalidOid;
	rte->subquery = NULL;
	rte->jointype = jointype;
	rte->joinmergedcols = nummergedcols;
	rte->joinaliasvars = aliasvars;
	rte->joinleftcols = leftcols;
	rte->joinrightcols = rightcols;
	rte->join_using_alias = join_using_alias;
	rte->alias = alias;

	eref = alias ? copyObject(alias) : makeAlias("unnamed_join", NIL);
	numaliases = list_length(eref->colnames);

	/* 填充未指定的别名列           */
	if (numaliases < list_length(colnames))
		eref->colnames = list_concat(eref->colnames,
									 list_copy_tail(colnames, numaliases));

	if (numaliases > list_length(colnames))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("join expression \"%s\" has %d columns available but %d columns specified",
						eref->aliasname, list_length(colnames), numaliases)));

	rte->eref = eref;

	/*
	 * 设置标志与访问权限。   
	 *
	 * 连接从不检查访问权限，因此无需调用             
	 * addRTEPermissionInfo() 
	 */
	rte->lateral = false;
	rte->inh = false;			/* 连接永不为真   */
	rte->inFromCl = inFromCl;

	/*
	 * 将完成的 RTE 加入 pstate 的范围表列表，以得知其   
	 * 索引。但不加入连接列表——若合适由调用者添加     
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造 ParseNamespaceItem，但不把它加入 pstate 的命名空间 
	 * 列表——若合适则由调用者添加  
	 */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = rte->eref;
	nsitem->p_rte = rte;
	nsitem->p_perminfo = NULL;
	nsitem->p_rtindex = list_length(pstate->p_rtable);
	nsitem->p_nscolumns = nscolumns;
	/* 设置默认可见性标志，之后可能被改变   */
	nsitem->p_rel_visible = true;
	nsitem->p_cols_visible = true;
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	return nsitem;
}

/*
 * 在 pstate 的范围表 (p_rtable) 中为 CTE 引用添加条目。     
 * 然后为新 RTE 构造并返回一个 ParseNamespaceItem。    
 *
 * 与 addRangeTableEntry() 很相似，但生成的是 CTE RTE。       
 */
ParseNamespaceItem *
addRangeTableEntryForCTE(ParseState *pstate,
						 CommonTableExpr *cte,
						 Index levelsup,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : cte->ctename;
	Alias	   *eref;
	int			numaliases;
	int			varattno;
	ListCell   *lc;
	int			n_dontexpand_columns = 0;
	ParseNamespaceItem *psi;

	Assert(pstate != NULL);

	rte->rtekind = RTE_CTE;
	rte->ctename = cte->ctename;
	rte->ctelevelsup = levelsup;

	/* 当且仅当 CTE 尚未完成解析时为自引用                 */
	rte->self_reference = !IsA(cte->ctequery, Query);
	Assert(cte->cterecursive || !rte->self_reference);
	/* 若非自引用，则递增该 CTE 的引用计数     */
	if (!rte->self_reference)
		cte->cterefcount++;

	/*
	 * 若 CTE 是不带 RETURNING 的 INSERT/UPDATE/DELETE 则报错。   
	 * 自引用情形不做此检查，但无妨，因为              
	 * 数据修改型 CTE 本就不允许递归。                      
	 */
	if (IsA(cte->ctequery, Query))
	{
		Query	   *ctequery = (Query *) cte->ctequery;

		if (ctequery->commandType != CMD_SELECT &&
			ctequery->returningList == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH query \"%s\" does not have a RETURNING clause",
							cte->ctename),
					 parser_errposition(pstate, rv->location)));
	}

	rte->coltypes = list_copy(cte->ctecoltypes);
	rte->coltypmods = list_copy(cte->ctecoltypmods);
	rte->colcollations = list_copy(cte->ctecolcollations);

	rte->alias = alias;
	if (alias)
		eref = copyObject(alias);
	else
		eref = makeAlias(refname, NIL);
	numaliases = list_length(eref->colnames);

	/* 填充未指定的别名列           */
	varattno = 0;
	foreach(lc, cte->ctecolnames)
	{
		varattno++;
		if (varattno > numaliases)
			eref->colnames = lappend(eref->colnames, lfirst(lc));
	}
	if (varattno < numaliases)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("table \"%s\" has %d columns available but %d columns specified",
						refname, varattno, numaliases)));

	rte->eref = eref;

	if (cte->search_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->search_clause->search_seq_column));
		if (cte->search_clause->search_breadth_first)
			rte->coltypes = lappend_oid(rte->coltypes, RECORDOID);
		else
			rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 1;
	}

	if (cte->cycle_clause)
	{
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_mark_column));
		rte->coltypes = lappend_oid(rte->coltypes, cte->cycle_clause->cycle_mark_type);
		rte->coltypmods = lappend_int(rte->coltypmods, cte->cycle_clause->cycle_mark_typmod);
		rte->colcollations = lappend_oid(rte->colcollations, cte->cycle_clause->cycle_mark_collation);

		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cte->cycle_clause->cycle_path_column));
		rte->coltypes = lappend_oid(rte->coltypes, RECORDARRAYOID);
		rte->coltypmods = lappend_int(rte->coltypmods, -1);
		rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);

		n_dontexpand_columns += 2;
	}

	/*
	 * 设置标志与访问权限。   
	 *
	 * 子查询从不检查访问权限，因此无需调用               
	 * addRTEPermissionInfo() 
	 */
	rte->lateral = false;
	rte->inh = false;			/* 子查询永不为真     */
	rte->inFromCl = inFromCl;

	/*
	 * 将完成的 RTE 加入 pstate 的范围表列表，以得知其   
	 * 索引。但不加入连接列表——若合适由调用者添加     
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造 ParseNamespaceItem，但不把它加入 pstate 的命名空间 
	 * 列表——若合适则由调用者添加  
	 */
	psi = buildNSItemFromLists(rte, list_length(pstate->p_rtable),
							   rte->coltypes, rte->coltypmods,
							   rte->colcollations);

	/*
	 * search 与 cycle 子句添加的列不包含在该 CTE                
	 * 所包含查询的星号展开之中。   
	 */
	if (rte->ctelevelsup > 0)
		for (int i = 0; i < n_dontexpand_columns; i++)
			psi->p_nscolumns[list_length(psi->p_names->colnames) - 1 - i].p_dontexpand = true;

	return psi;
}

/*
 * 在 pstate 的范围表 (p_rtable) 中为临时命名关系引用     
 * 添加一个条目。  
 * 然后为新 RTE 构造并返回一个 ParseNamespaceItem。    
 *
 * 预期这个 RangeVar（此前仅知其为临时命名关系）            
 * 将结合 ParseState 中的 QueryEnvironment，依据 enrtype              
 * 为特定*种类*的临时命名关系创建相应的 RangeTblEntry。    
 *                                  
 *
 * 与 addRangeTableEntry() 很相似，但生成的 RTE 对应             
 * 临时命名关系。    
 */
ParseNamespaceItem *
addRangeTableEntryForENR(ParseState *pstate,
						 RangeVar *rv,
						 bool inFromCl)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	Alias	   *alias = rv->alias;
	char	   *refname = alias ? alias->aliasname : rv->relname;
	EphemeralNamedRelationMetadata enrmd;
	TupleDesc	tupdesc;
	int			attno;

	Assert(pstate != NULL);
	enrmd = get_visible_ENR(pstate, rv->relname);
	Assert(enrmd != NULL);

	switch (enrmd->enrtype)
	{
		case ENR_NAMED_TUPLESTORE:
			rte->rtekind = RTE_NAMEDTUPLESTORE;
			break;

		default:
			elog(ERROR, "unexpected enrtype: %d", enrmd->enrtype);
			return NULL;		/* 取悦编译器     */
	}

	/*
	 * 记录对某个关系的依赖。当计划访问的传递表         
	 * 所链接的表被改动时，这些计划即可失效。          
	 */
	rte->relid = enrmd->reliddesc;

	/*
	 * 以用户提供的别名和/或实际列名构建                   
	 * 有效列名的列表。   
	 */
	tupdesc = ENRMetadataGetTupDesc(enrmd);
	rte->eref = makeAlias(refname, NIL);
	buildRelationAliases(tupdesc, alias, rte->eref);

	/* 记录 ENR 的附加数据，包括列类型信息         */
	rte->enrname = enrmd->name;
	rte->enrtuples = enrmd->enrtuples;
	rte->coltypes = NIL;
	rte->coltypmods = NIL;
	rte->colcollations = NIL;
	for (attno = 1; attno <= tupdesc->natts; ++attno)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attno - 1);

		if (att->attisdropped)
		{
			/* 为已删除列记录零值        */
			rte->coltypes = lappend_oid(rte->coltypes, InvalidOid);
			rte->coltypmods = lappend_int(rte->coltypmods, 0);
			rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);
		}
		else
		{
			/* 确保我们能辨别此列未被删除             */
			if (att->atttypid == InvalidOid)
				elog(ERROR, "atttypid is invalid for non-dropped column in \"%s\"",
					 rv->relname);
			rte->coltypes = lappend_oid(rte->coltypes, att->atttypid);
			rte->coltypmods = lappend_int(rte->coltypmods, att->atttypmod);
			rte->colcollations = lappend_oid(rte->colcollations,
											 att->attcollation);
		}
	}

	/*
	 * 设置标志与访问权限。   
	 *
	 * 从不检查 ENR 的访问权限，因此无需调用          
	 * addRTEPermissionInfo() 
	 */
	rte->lateral = false;
	rte->inh = false;			/* ENR 永不为真    */
	rte->inFromCl = inFromCl;

	/*
	 * 将完成的 RTE 加入 pstate 的范围表列表，以得知其   
	 * 索引。但不加入连接列表——若合适由调用者添加     
	 *             
	 */
	pstate->p_rtable = lappend(pstate->p_rtable, rte);

	/*
	 * 构造 ParseNamespaceItem，但不把它加入 pstate 的命名空间 
	 * 列表——若合适则由调用者添加  
	 */
	return buildNSItemFromTupleDesc(rte, list_length(pstate->p_rtable), NULL,
									tupdesc);
}


/*
 * 指定的 refname 是否已被选定 FOR UPDATE/FOR SHARE？ 
 *
 * 用于尚未执行 transformLockingClause，但需要知道            
 * 初次打开关系时应取何种锁的场合。                     
 *
 * 注意 refname 可能为 NULL（对应无别名的子查询），此时  
 * 无法按名字锁定该关系，但若 locking 子句要求              
 * 锁定所有表，则它仍可能被锁定。       
 *
 * 注意：我们不去区分 FOR UPDATE 与 FOR SHARE，           
 * 因为两者的表级锁是相同的。           
 */
bool
isLockedRefname(ParseState *pstate, const char *refname)
{
	ListCell   *l;

	/*
	 * 若我们处于被上层指定为 FOR UPDATE/SHARE 锁定的     
	 * 子查询中，则视作此处存在通用 FOR UPDATE。           
	 */
	if (pstate->p_locked_from_parent)
		return true;

	foreach(l, pstate->p_locking_clause)
	{
		LockingClause *lc = (LockingClause *) lfirst(l);

		if (lc->lockedRels == NIL)
		{
			/* 查询用到的全部表 */
			return true;
		}
		else if (refname != NULL)
		{
			/* 仅指定的那些表 */
			ListCell   *l2;

			foreach(l2, lc->lockedRels)
			{
				RangeVar   *thisrel = (RangeVar *) lfirst(l2);

				if (strcmp(refname, thisrel->relname) == 0)
					return true;
			}
		}
	}
	return false;
}

/*
 * 把给定的 nsitem/RTE 作为顶层条目加入 pstate 的            
 * 连接列表和/或命名空间列表。（调用者应已   
 * 检查过命名空间冲突。）该 nsitem 总是标记为无条件 
 * 可见，即并非仅 LATERAL。   
 */
void
addNSItemToQuery(ParseState *pstate, ParseNamespaceItem *nsitem,
				 bool addToJoinList,
				 bool addToRelNameSpace, bool addToVarNameSpace)
{
	if (addToJoinList)
	{
		RangeTblRef *rtr = makeNode(RangeTblRef);

		rtr->rtindex = nsitem->p_rtindex;
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	}
	if (addToRelNameSpace || addToVarNameSpace)
	{
		/* 正确设置新 nsitem 的可见性标志       */
		nsitem->p_rel_visible = addToRelNameSpace;
		nsitem->p_cols_visible = addToVarNameSpace;
		nsitem->p_lateral_only = false;
		nsitem->p_lateral_ok = true;
		pstate->p_namespace = lappend(pstate->p_namespace, nsitem);
	}
}

/*
 * expandRTE —— 展开范围表条目的列         
 *
 * 创建 RTE 的列名列表（有别名则用别名，否则           
 * 用真实名）以及各列的 Var。只考虑用户列。              
 * 若 include_dropped 为假，则结果中省略已删除列。       
 * 若 include_dropped 为真，则已删除列对应空字符串与         
 * NULL 常量（而非 Var）。               
 *
 * rtindex、sublevels_up 与 location 是所建 Var 的 varno、              
 * varlevelsup 与 location 值。通常 rtindex 应与该 RTE 在        
 * 范围表中的实际位置一致。         
 *
 * 输出列表放入 *colnames 与 *colvars。    
 * 若只需要两种输出列表之一，则对不需要的那个         
 * 输出指针传 NULL。             
 */
void
expandRTE(RangeTblEntry *rte, int rtindex, int sublevels_up,
		  int location, bool include_dropped,
		  List **colnames, List **colvars)
{
	int			varattno;

	if (colnames)
		*colnames = NIL;
	if (colvars)
		*colvars = NIL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 普通关系 RTE      */
			expandRelation(rte->relid, rte->eref,
						   rtindex, sublevels_up, location,
						   include_dropped, colnames, colvars);
			break;
		case RTE_SUBQUERY:
			{
				/* 子查询 RTE*/
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *tlistitem;

				varattno = 0;
				foreach(tlistitem, rte->subquery->targetList)
				{
					TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

					if (te->resjunk)
						continue;
					varattno++;
					Assert(varattno == te->resno);

					/*
					 * 从前子查询目标列表的非垃圾条目可能    
					 * 多于 colnames 列表的数目（若此 RTE 展开  
					 * 自某个视图，而其列数多于当前查询    
					 * 被解析时该视图所具有的列数）。          
					 * 如今 ApplyRetrieveRule 会清理这类情形，  
					 * 不应再遇到该情形，但仍照例检查。 
					 */
					if (!aliasp_item)
						elog(ERROR, "too few column names for subquery %s",
							 rte->eref->aliasname);

					if (colnames)
					{
						char	   *label = strVal(lfirst(aliasp_item));

						*colnames = lappend(*colnames, makeString(pstrdup(label)));
					}

					if (colvars)
					{
						Var		   *varnode;

						varnode = makeVar(rtindex, varattno,
										  exprType((Node *) te->expr),
										  exprTypmod((Node *) te->expr),
										  exprCollation((Node *) te->expr),
										  sublevels_up);
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}

					aliasp_item = lnext(rte->eref->colnames, aliasp_item);
				}
			}
			break;
		case RTE_FUNCTION:
			{
				/* 函数 RTE   */
				int			atts_done = 0;
				ListCell   *lc;

				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
					TypeFuncClass functypclass;
					Oid			funcrettype = InvalidOid;
					TupleDesc	tupdesc = NULL;

					/* 若带 coldeflist，则返回 RECORD      */
					if (rtfunc->funccolnames != NIL)
						functypclass = TYPEFUNC_RECORD;
					else
						functypclass = get_expr_result_type(rtfunc->funcexpr,
															&funcrettype,
															&tupdesc);

					if (functypclass == TYPEFUNC_COMPOSITE ||
						functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
					{
						/* 复合数据类型，例如表的行类型   */
						Assert(tupdesc);
						expandTupleDesc(tupdesc, rte->eref,
										rtfunc->funccolcount, atts_done,
										rtindex, sublevels_up, location,
										include_dropped, colnames, colvars);
					}
					else if (functypclass == TYPEFUNC_SCALAR)
					{
						/* 基础类型，即标量    */
						if (colnames)
							*colnames = lappend(*colnames,
												list_nth(rte->eref->colnames,
														 atts_done));

						if (colvars)
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, atts_done + 1,
											  funcrettype,
											  exprTypmod(rtfunc->funcexpr),
											  exprCollation(rtfunc->funcexpr),
											  sublevels_up);
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
					}
					else if (functypclass == TYPEFUNC_RECORD)
					{
						if (colnames)
						{
							List	   *namelist;

							/* 提取列列表的相应子集            */
							namelist = list_copy_tail(rte->eref->colnames,
													  atts_done);
							namelist = list_truncate(namelist,
													 rtfunc->funccolcount);
							*colnames = list_concat(*colnames, namelist);
						}

						if (colvars)
						{
							ListCell   *l1;
							ListCell   *l2;
							ListCell   *l3;
							int			attnum = atts_done;

							forthree(l1, rtfunc->funccoltypes,
									 l2, rtfunc->funccoltypmods,
									 l3, rtfunc->funccolcollations)
							{
								Oid			attrtype = lfirst_oid(l1);
								int32		attrtypmod = lfirst_int(l2);
								Oid			attrcollation = lfirst_oid(l3);
								Var		   *varnode;

								attnum++;
								varnode = makeVar(rtindex,
												  attnum,
												  attrtype,
												  attrtypmod,
												  attrcollation,
												  sublevels_up);
								varnode->location = location;
								*colvars = lappend(*colvars, varnode);
							}
						}
					}
					else
					{
						/* addRangeTableEntryForFunction 本应捕获此情况 */
						elog(ERROR, "function in FROM has unsupported return type");
					}
					atts_done += rtfunc->funccolcount;
				}

				/* 如有序数列则附加之         */
				if (rte->funcordinality)
				{
					if (colnames)
						*colnames = lappend(*colnames,
											llast(rte->eref->colnames));

					if (colvars)
					{
						Var		   *varnode = makeVar(rtindex,
													  atts_done + 1,
													  INT8OID,
													  -1,
													  InvalidOid,
													  sublevels_up);

						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_JOIN:
			{
				/* 连接RTE*/
				ListCell   *colname;
				ListCell   *aliasvar;

				Assert(list_length(rte->eref->colnames) == list_length(rte->joinaliasvars));

				varattno = 0;
				forboth(colname, rte->eref->colnames, aliasvar, rte->joinaliasvars)
				{
					Node	   *avar = (Node *) lfirst(aliasvar);

					varattno++;

					/*
					 * 普通解析时，连接中绝不会有         
					 * 已删除列。此函数虽也供重写器          
					 * 和规划器用，但它们目前不会对任何  
					 * JOIN RTE 调用。故接下来这段是死代码， 
					 * 但稳妥起见仍正确处理该情形。           
					 *                           
					 */
					if (avar == NULL)
					{
						if (include_dropped)
						{
							if (colnames)
								*colnames = lappend(*colnames,
													makeString(pstrdup("")));
							if (colvars)
							{
								/*
								 * 此处无法使用连接列类型（可能 
								 * 已删除！）；不过 Const 自称何  
								 * 种类型都无所谓。         
								 */
								*colvars = lappend(*colvars,
												   makeNullConst(INT4OID, -1,
																 InvalidOid));
							}
						}
						continue;
					}

					if (colnames)
					{
						char	   *label = strVal(lfirst(colname));

						*colnames = lappend(*colnames,
											makeString(pstrdup(label)));
					}

					if (colvars)
					{
						Var		   *varnode;

						/*
						 * 若 joinaliasvars 条目为简单 Var，则     
						 * 原样复制它（调整 varlevelsup 和   
						 * location）；否则它是 JOIN USING 列，须   
						 * 生成连接别名 Var。这与若能访问该  
						 * 连接的 ParseNamespaceItem，那么    
						 * expandNSItemVars 展开"join.*"的结果       
						 * 就会一致。                               
						 */
						if (IsA(avar, Var))
						{
							varnode = copyObject((Var *) avar);
							varnode->varlevelsup = sublevels_up;
						}
						else
							varnode = makeVar(rtindex, varattno,
											  exprType(avar),
											  exprTypmod(avar),
											  exprCollation(avar),
											  sublevels_up);
						varnode->location = location;

						*colvars = lappend(*colvars, varnode);
					}
				}
			}
			break;
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
			{
				/* Tablefunc/Values/CTE/ENR 型 RTE   */
				ListCell   *aliasp_item = list_head(rte->eref->colnames);
				ListCell   *lct;
				ListCell   *lcm;
				ListCell   *lcc;

				varattno = 0;
				forthree(lct, rte->coltypes,
						 lcm, rte->coltypmods,
						 lcc, rte->colcollations)
				{
					Oid			coltype = lfirst_oid(lct);
					int32		coltypmod = lfirst_int(lcm);
					Oid			colcoll = lfirst_oid(lcc);

					varattno++;

					if (colnames)
					{
						/* 假定每个输出列对应一个别名     */
						if (OidIsValid(coltype))
						{
							char	   *label = strVal(lfirst(aliasp_item));

							*colnames = lappend(*colnames,
												makeString(pstrdup(label)));
						}
						else if (include_dropped)
							*colnames = lappend(*colnames,
												makeString(pstrdup("")));

						aliasp_item = lnext(rte->eref->colnames, aliasp_item);
					}

					if (colvars)
					{
						if (OidIsValid(coltype))
						{
							Var		   *varnode;

							varnode = makeVar(rtindex, varattno,
											  coltype, coltypmod, colcoll,
											  sublevels_up);
							varnode->location = location;

							*colvars = lappend(*colvars, varnode);
						}
						else if (include_dropped)
						{
							/*
							 * Const 声称的类型其实无关紧要。  
							 *              
							 */
							*colvars = lappend(*colvars,
											   makeNullConst(INT4OID, -1,
															 InvalidOid));
						}
					}
				}
			}
			break;
		case RTE_RESULT:
			/* 这些不暴露任何列，故无事可做*/
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
	}
}

/*
 * expandRelation：expandRTE子程序   
 */
static void
expandRelation(Oid relid, Alias *eref, int rtindex, int sublevels_up,
			   int location, bool include_dropped,
			   List **colnames, List **colvars)
{
	Relation	rel;

	/* 取得 tupledesc 并转交 expandTupleDesc 处理     */
	rel = relation_open(relid, AccessShareLock);
	expandTupleDesc(rel->rd_att, eref, rel->rd_att->natts, 0,
					rtindex, sublevels_up,
					location, include_dropped,
					colnames, colvars);
	relation_close(rel, AccessShareLock);
}

/*
 * expandTupleDesc：expandRTE子程序   
 *
 * 为 tupdesc 的前 count 个属性生成名称和/或 Var，               
 * 并把它们追加到 colnames/colvars。offset 会加到              
 * 各 Var 本来的 varattno 上，且跳过 eref->colnames              
 * 的前 offset 个条目。（这些设定使此代码也能用于       
 * RTE_FUNCTION RTE 中的单个复合返回函数。）              
 */
static void
expandTupleDesc(TupleDesc tupdesc, Alias *eref, int count, int offset,
				int rtindex, int sublevels_up,
				int location, bool include_dropped,
				List **colnames, List **colvars)
{
	ListCell   *aliascell;
	int			varattno;

	aliascell = (offset < list_length(eref->colnames)) ?
		list_nth_cell(eref->colnames, offset) : NULL;

	Assert(count <= tupdesc->natts);
	for (varattno = 0; varattno < count; varattno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

		if (attr->attisdropped)
		{
			if (include_dropped)
			{
				if (colnames)
					*colnames = lappend(*colnames, makeString(pstrdup("")));
				if (colvars)
				{
					/*
					 * 此处无法使用 atttypid，不过它声称什么  
					 * 类型都无关紧要。         
					 */
					*colvars = lappend(*colvars,
									   makeNullConst(INT4OID, -1, InvalidOid));
				}
			}
			if (aliascell)
				aliascell = lnext(eref->colnames, aliascell);
			continue;
		}

		if (colnames)
		{
			char	   *label;

			if (aliascell)
			{
				label = strVal(lfirst(aliascell));
				aliascell = lnext(eref->colnames, aliascell);
			}
			else
			{
				/* 若别名用尽，则使用底层名称           */
				label = NameStr(attr->attname);
			}
			*colnames = lappend(*colnames, makeString(pstrdup(label)));
		}

		if (colvars)
		{
			Var		   *varnode;

			varnode = makeVar(rtindex, varattno + offset + 1,
							  attr->atttypid, attr->atttypmod,
							  attr->attcollation,
							  sublevels_up);
			varnode->location = location;

			*colvars = lappend(*colvars, varnode);
		}
	}
}

/*
 * expandNSItemVars
 *	  生成 Var 列表，并可选地生成列名列表，         
 *	  对应 nsitem 的未删除列。          
 *
 * 生成的 Var 标以给定的 sublevels_up 与 location。           
 *
 * 若 colnames 非 NULL，则该列的 String 项列表会存于          
 * 其中；注意它只是 RTE eref 列表的子集，因而           
 * 列表元素不可修改。           
 */
List *
expandNSItemVars(ParseState *pstate, ParseNamespaceItem *nsitem,
				 int sublevels_up, int location,
				 List **colnames)
{
	List	   *result = NIL;
	int			colindex;
	ListCell   *lc;

	if (colnames)
		*colnames = NIL;
	colindex = 0;
	foreach(lc, nsitem->p_names->colnames)
	{
		String	   *colnameval = lfirst(lc);
		const char *colname = strVal(colnameval);
		ParseNamespaceColumn *nscol = nsitem->p_nscolumns + colindex;

		if (nscol->p_dontexpand)
		{
			/* 略  */
		}
		else if (colname[0])
		{
			Var		   *var;

			Assert(nscol->p_varno > 0);
			var = makeVar(nscol->p_varno,
						  nscol->p_varattno,
						  nscol->p_vartype,
						  nscol->p_vartypmod,
						  nscol->p_varcollid,
						  sublevels_up);
			/* makeVar 不为此提供参数，故手工设置：          */
			var->varnosyn = nscol->p_varnosyn;
			var->varattnosyn = nscol->p_varattnosyn;
			var->location = location;

			/* 并更新 varnullingrels      */
			markNullableIfNeeded(pstate, var);

			result = lappend(result, var);
			if (colnames)
				*colnames = lappend(*colnames, colnameval);
		}
		else
		{
			/* 已删除列，忽略  */
			Assert(nscol->p_varno == 0);
		}
		colindex++;
	}
	return result;
}

/*
 * expandNSItemAttrs -
 *	  星号展开的主力：为该 nsitem 的属性生成       
 *	  目标列表项列表。        
 *
 * pstate->p_next_resno 决定各 TLE 的 resno。                 
 * 若调用者要求，则把被引用列标记为需要          
 * SELECT 访问。     
 */
List *
expandNSItemAttrs(ParseState *pstate, ParseNamespaceItem *nsitem,
				  int sublevels_up, bool require_col_privs, int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	RTEPermissionInfo *perminfo = nsitem->p_perminfo;
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	vars = expandNSItemVars(pstate, nsitem, sublevels_up, location, &names);

	/*
	 * 需要对表的读权限。这通常与下面的 markVarForSelectPriv 
	 * 调用重复，但表没有任何列时并非如此。         
	 * 若 nsitem 属于连接则无需任何操作：其成员表       
	 * 在加入范围表时已被标记 ACL_SELECT。（此步              
	 * 仅对 UPDATE/DELETE 的目标关系产生影响，              
	 * 它不可能位于连接之下。）                     
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		Assert(perminfo != NULL);
		perminfo->requiredPerms |= ACL_SELECT;
	}

	forboth(name, names, var, vars)
	{
		char	   *label = strVal(lfirst(name));
		Var		   *varnode = (Var *) lfirst(var);
		TargetEntry *te;

		te = makeTargetEntry((Expr *) varnode,
							 (AttrNumber) pstate->p_next_resno++,
							 label,
							 false);
		te_list = lappend(te_list, te);

		if (require_col_privs)
		{
			/* 需要对每列的读权限        */
			markVarForSelectPriv(pstate, varnode);
		}
	}

	Assert(name == NULL && var == NULL);	/* 两个列表长度不同？*/

	return te_list;
}

/*
 * get_rte_attribute_name
 *		从 RangeTblEntry 取得属性名         
 *
 * 与 get_attname() 不同：这里尽可能使用别名。        
 * 尤其是它可用于子查询或连接的 RTE，而                  
 * get_attname() 仅用于真实关系。     
 *
 * 若给定 attnum 为 InvalidAttrNumber，则返回"*"                 
 * ——当 Var 代表整个元组时即属此情形。    
 *
 * 调用者须保证不对已删除属性调用本函数。             
 * （对这类情形你会得到某个答案，但未必合理。）      
 */
char *
get_rte_attribute_name(RangeTblEntry *rte, AttrNumber attnum)
{
	if (attnum == InvalidAttrNumber)
		return "*";

	/*
	 * 若有用户书写的列别名，则用之。   
	 */
	if (rte->alias &&
		attnum > 0 && attnum <= list_length(rte->alias->colnames))
		return strVal(list_nth(rte->alias->colnames, attnum - 1));

	/*
	 * 若 RTE 是关系，则查系统目录而非               
	 * eref->colnames 列表。稍慢，但在 eref 列表               
	 * 建成后列已被重命名的情况下能给出正确             
	 * 答案（对规则这很易发生）。   
	 */
	if (rte->rtekind == RTE_RELATION)
		return get_attname(rte->relid, attnum, false);

	/*
	 * 否则使用 eref 中的列名。那里应当总有名字。         
	 */
	if (attnum > 0 && attnum <= list_length(rte->eref->colnames))
		return strVal(list_nth(rte->eref->colnames, attnum - 1));

	/* 否则即调用者给了伪 attnum */
	elog(ERROR, "invalid attnum %d for rangetable entry %s",
		 attnum, rte->eref->aliasname);
	return NULL;				/* 让编译器安静  */
}

/*
 * get_rte_attribute_is_dropped
 *		检查所尝试的属性引用是否指向已删除列      
 */
bool
get_rte_attribute_is_dropped(RangeTblEntry *rte, AttrNumber attnum)
{
	bool		result;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				/*
				 * 普通关系 RTE —— 取得该属性的目录条目  
				 */
				HeapTuple	tp;
				Form_pg_attribute att_tup;

				tp = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(rte->relid),
									 Int16GetDatum(attnum));
				if (!HeapTupleIsValid(tp))	/* 不应发生     */
					elog(ERROR, "cache lookup failed for attribute %d of relation %u",
						 attnum, rte->relid);
				att_tup = (Form_pg_attribute) GETSTRUCT(tp);
				result = att_tup->attisdropped;
				ReleaseSysCache(tp);
			}
			break;
		case RTE_SUBQUERY:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:

			/*
			 * 子查询、表函数、Values、CTE 这些 RTE 绝不会有   
			 *        
			 */
			result = false;
			break;
		case RTE_NAMEDTUPLESTORE:
			{
				/* 借 coltype 是否有效判断列已删除      */
				if (attnum <= 0 ||
					attnum > list_length(rte->coltypes))
					elog(ERROR, "invalid varattno %d", attnum);
				result = !OidIsValid((list_nth_oid(rte->coltypes, attnum - 1)));
			}
			break;
		case RTE_JOIN:
			{
				/*
				 * 连接 RTE 构造时不会有已删除列，               
				 * 但存储规则中的连接可能含这类列：        
				 * 它们已从底层表中删除，前提是这些列    
				 * 在规则中没有任何显式引用。这种情形     
				 * 会以 joinaliasvars 列表的空指针向我们示警。  
				 */
				Var		   *aliasvar;

				if (attnum <= 0 ||
					attnum > list_length(rte->joinaliasvars))
					elog(ERROR, "invalid varattno %d", attnum);
				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);

				result = (aliasvar == NULL);
			}
			break;
		case RTE_FUNCTION:
			{
				/* 函数 RTE   */
				ListCell   *lc;
				int			atts_done = 0;

				/*
				 * 只有返回命名复合类型的函数才可能有     
				 * 已删除属性。此时必须查结果类型，        
				 * 看看它当前是否已删除此列。因此首先    
				 * 遍历各函数，直到找到覆盖所求列的那    
				 * 一个为止。                               
				 */
				foreach(lc, rte->functions)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

					if (attnum > atts_done &&
						attnum <= atts_done + rtfunc->funccolcount)
					{
						TupleDesc	tupdesc;

						/* 若带 coldeflist，则返回 RECORD      */
						if (rtfunc->funccolnames != NIL)
							return false;	/* 不可能有已删除列       */

						tupdesc = get_expr_result_tupdesc(rtfunc->funcexpr,
														  true);
						if (tupdesc)
						{
							/* 复合数据类型，例如表的行类型   */
							Form_pg_attribute att_tup;

							Assert(tupdesc);
							Assert(attnum - atts_done <= tupdesc->natts);
							att_tup = TupleDescAttr(tupdesc,
													attnum - atts_done - 1);
							return att_tup->attisdropped;
						}
						/* 否则不可能有任何已删除列         */
						return false;
					}
					atts_done += rtfunc->funccolcount;
				}

				/* 若走到这里，必然是在找序数列                */
				if (rte->funcordinality && attnum == atts_done + 1)
					return false;

				/* 这种情况多半不会发生 */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column %d of relation \"%s\" does not exist",
								attnum,
								rte->eref->aliasname)));
				result = false; /* 让编译器安静  */
			}
			break;
		case RTE_RESULT:
			/* 这种情况多半不会发生 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column %d of relation \"%s\" does not exist",
							attnum,
							rte->eref->aliasname)));
			result = false;		/* 让编译器安静  */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
			result = false;		/* 让编译器安静  */
	}

	return result;
}

/*
 * 给定目标列表和 resno，返回匹配的 TargetEntry      
 *
 * 若列表中无 resno 则返回 NULL。      
 *
 * 注意：须进行搜索，而非直接用 list_nth() 取下标， 
 * 因为 tlist 未必都按 resno 排序。  
 */
TargetEntry *
get_tle_by_resno(List *tlist, AttrNumber resno)
{
	ListCell   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resno == resno)
			return tle;
	}
	return NULL;
}

/*
 * 给定查询和范围表索引，若有则返回关系的 RowMarkClause   
 *
 * 若关系未被 FOR UPDATE/SHARE 选中则返回 NULL    
 */
RowMarkClause *
get_parse_rowmark(Query *qry, Index rtindex)
{
	ListCell   *l;

	foreach(l, qry->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (rc->rti == rtindex)
			return rc;
	}
	return NULL;
}

/*
 *	给定关系和属性名，返回该变量的 attnum  
 *
 *	若属性不存在（或已删除），返回 InvalidAttrNumber。  
 *
 *	须已用 table_open() 打开关系方可使用。 
 *	未打开的关系请用缓存版本 get_attnum()   
 *	来访问。                       
 */
int
attnameAttNum(Relation rd, const char *attname, bool sysColOK)
{
	int			i;

	for (i = 0; i < RelationGetNumberOfAttributes(rd); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rd->rd_att, i);

		if (namestrcmp(&(att->attname), attname) == 0 && !att->attisdropped)
			return i + 1;
	}

	if (sysColOK)
	{
		if ((i = specialAttNum(attname)) != InvalidAttrNumber)
			return i;
	}

	/* 失败时  */
	return InvalidAttrNumber;
}

/* specialAttNum()
 *
 * 检查属性名是否为特殊名称，如 "xmin"。        
 * - thomas 2000-02-07
 *
 * 注意：这只能判断该名称是否可能是系统属性。        
 * 调用者需确保它确实是该关系的属性。              
 */
static int
specialAttNum(const char *attname)
{
	const FormData_pg_attribute *sysatt;

	sysatt = SystemAttributeByName(attname);
	if (sysatt != NULL)
		return sysatt->attnum;
	return InvalidAttrNumber;
}


/*
 * 给定属性标识，返回该属性的名称    
 *
 *	须已用 table_open() 打开关系方可使用。 
 *	未打开的关系请用缓存版本 get_atttype()   
 *	来访问。                       
 */
const NameData *
attnumAttName(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return &sysatt->attname;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return &TupleDescAttr(rd->rd_att, attid - 1)->attname;
}

/*
 * 给定属性标识，返回该属性的类型    
 *
 *	须已用 table_open() 打开关系方可使用。 
 *	未打开的关系请用缓存版本 get_atttype()   
 *	来访问。                       
 */
Oid
attnumTypeId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		const FormData_pg_attribute *sysatt;

		sysatt = SystemAttributeDefinition(attid);
		return sysatt->atttypid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return TupleDescAttr(rd->rd_att, attid - 1)->atttypid;
}

/*
 * 给定属性标识，返回该属性的排序规则   
 *
 *	须已用 table_open() 打开关系方可使用。                  
 */
Oid
attnumCollationId(Relation rd, int attid)
{
	if (attid <= 0)
	{
		/* 所有系统属性均为不可排序类型。     */
		return InvalidOid;
	}
	if (attid > rd->rd_att->natts)
		elog(ERROR, "invalid attribute number %d", attid);
	return TupleDescAttr(rd->rd_att, attid - 1)->attcollation;
}

/*
 * 针对缺失的 RTE 生成适当的错误。  
 *
 * 由于这类错误十分常见，我们花了很大功夫        
 * 以给出有用提示。  
 */
void
errorMissingRTE(ParseState *pstate, RangeVar *relation)
{
	RangeTblEntry *rte;
	const char *badAlias = NULL;

	/*
	 * 检查查询的范围表中是否存在潜在匹配。        
	 * （注意：RangeVar 中模式名有误时会立即在此报错。   
	 * 这样做似乎没问题。）                     
	 */
	rte = searchRangeTableForRel(pstate, relation);

	/*
	 * 若找到的匹配带别名且该别名在命名空间中可见，   
	 * 则问题很可能出在使用了关系的真实名称而非别名，  
	 * 即 "SELECT foo.* FROM foo f"。这种错误很常见，           
	 * 值得为其给出专门的提示。     
	 *
	 * 若找到的匹配不满足上述条件，则可认为这是    
	 * 在关系作用域外非法使用该关系，例如 MySQL 写法 
	 * "SELECT ... FROM a, b LEFT JOIN c ON (a.x = c.y)"。        
	 */
	if (rte && rte->alias &&
		strcmp(rte->eref->aliasname, relation->relname) != 0)
	{
		ParseNamespaceItem *nsitem;
		int			sublevels_up;

		nsitem = refnameNamespaceItem(pstate, NULL, rte->eref->aliasname,
									  relation->location,
									  &sublevels_up);
		if (nsitem && nsitem->p_rte == rte)
			badAlias = rte->eref->aliasname;
	}

	/* 若看似用户忘了使用别名，则就此给出提示。      */
	if (badAlias)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errhint("Perhaps you meant to reference the table alias \"%s\".",
						 badAlias),
				 parser_errposition(pstate, relation->location)));
	/* 若找到（不可访问的）精确匹配，则给出提示 */
	else if (rte)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("invalid reference to FROM-clause entry for table \"%s\"",
						relation->relname),
				 errdetail("There is an entry for table \"%s\", but it cannot be referenced from this part of the query.",
						   rte->eref->aliasname),
				 rte_visible_if_lateral(pstate, rte) ?
				 errhint("To reference that table, you must mark this subquery with LATERAL.") : 0,
				 parser_errposition(pstate, relation->location)));
	/* 否则除了干巴巴的报错之外我们无可奉告。      */
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("missing FROM-clause entry for table \"%s\"",
						relation->relname),
				 parser_errposition(pstate, relation->location)));
}

/*
 * 针对缺失的列生成适当的错误。       
 *
 * 由于这类错误十分常见，我们花了很大功夫        
 * 以给出有用提示。  
 */
void
errorMissingColumn(ParseState *pstate,
				   const char *relname, const char *colname, int location)
{
	FuzzyAttrMatchState *state;

	/*
	 * 搜索整个范围表以寻找可能的匹配。若找到匹配，     
	 * 则给出提示。   
	 */
	state = searchRangeTableForCol(pstate, relname, colname, location);

	/*
	 * 若有精确匹配，则必定是因某种原因无法访问。 
	 *        
	 */
	if (state->rexact1)
	{
		/*
		 * 存在多个不可访问的精确匹配时不必费力，     
		 * 但至少要确保不会误导性地暗示只有一个。       
		 *                       
		 */
		if (state->rexact2)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 errdetail("There are columns named \"%s\", but they are in tables that cannot be referenced from this part of the query.",
							   colname),
					 !relname ? errhint("Try using a table-qualified name.") : 0,
					 parser_errposition(pstate, location)));
		/* 仅一个精确匹配，尝试查明其为何不可访问。   */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errdetail("There is a column named \"%s\" in table \"%s\", but it cannot be referenced from this part of the query.",
						   colname, state->rexact1->eref->aliasname),
				 rte_visible_if_lateral(pstate, state->rexact1) ?
				 errhint("To reference that column, you must mark this subquery with LATERAL.") :
				 (!relname && rte_visible_if_qualified(pstate, state->rexact1)) ?
				 errhint("To reference that column, you must use a table-qualified name.") : 0,
				 parser_errposition(pstate, location)));
	}

	if (!state->rsecond)
	{
		/* 若完全找不到匹配，可报告的内容甚少。*/
		if (!state->rfirst)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 relname ?
					 errmsg("column %s.%s does not exist", relname, colname) :
					 errmsg("column \"%s\" does not exist", colname),
					 parser_errposition(pstate, location)));
		/* 处理只有一个备选拼写可提供的情况。              */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1))),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* 处理存在两条同等有用的列提示的情况。      */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 relname ?
				 errmsg("column %s.%s does not exist", relname, colname) :
				 errmsg("column \"%s\" does not exist", colname),
				 errhint("Perhaps you meant to reference the column \"%s.%s\" or the column \"%s.%s\".",
						 state->rfirst->eref->aliasname,
						 strVal(list_nth(state->rfirst->eref->colnames,
										 state->first - 1)),
						 state->rsecond->eref->aliasname,
						 strVal(list_nth(state->rsecond->eref->colnames,
										 state->second - 1))),
				 parser_errposition(pstate, location)));
	}
}

/*
 * 若 RTE 可见，则查找其 ParseNamespaceItem。     
 * 我们假定一个 RTE 在命名空间列表中不会出现多次。   
 */
static ParseNamespaceItem *
findNSItemForRTE(ParseState *pstate, RangeTblEntry *rte)
{
	while (pstate != NULL)
	{
		ListCell   *l;

		foreach(l, pstate->p_namespace)
		{
			ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(l);

			if (nsitem->p_rte == rte)
				return nsitem;
		}
		pstate = pstate->parentParseState;
	}
	return NULL;
}

/*
 * 如果用户写了 LATERAL，这个 RTE 会可见吗？         
 *
 * 本辅助函数用于决定是否就 LATERAL 发出提示。        
 * 因此它无需做到百分之百准确；即使不十分准确，    
 * 提示也可能有用。因此我们不去细究找到的 nsitem     
 * 是否设置了 p_rel_visible 或 p_cols_visible 中的适当者。     
 *                    
 */
static bool
rte_visible_if_lateral(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem;

	/* 若 LATERAL 确实已启用，那显然是找错了方向。    */
	if (pstate->p_lateral_active)
		return false;
	nsitem = findNSItemForRTE(pstate, rte);
	if (nsitem)
	{
		/* 找到了，报告其是否仅限 LATERAL  */
		return nsitem->p_lateral_only && nsitem->p_lateral_ok;
	}
	return false;
}

/*
 * 若加以限定，该 RTE 中的列可见吗？   
 */
static bool
rte_visible_if_qualified(ParseState *pstate, RangeTblEntry *rte)
{
	ParseNamespaceItem *nsitem = findNSItemForRTE(pstate, rte);

	if (nsitem)
	{
		/* 找到了，报告其是否仅限关系。  */
		return nsitem->p_rel_visible && !nsitem->p_cols_visible;
	}
	return false;
}


/*
 * 检查一个已完成解析的查询，当且仅当查询底层          
 * 存在临时关系（表、视图或物化视图）时返回 true。  
 */
bool
isQueryUsingTempRelation(Query *query)
{
	return isQueryUsingTempRelation_walker((Node *) query, NULL);
}

static bool
isQueryUsingTempRelation_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);

			if (rte->rtekind == RTE_RELATION)
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);
				char		relpersistence = rel->rd_rel->relpersistence;

				table_close(rel, AccessShareLock);
				if (relpersistence == RELPERSISTENCE_TEMP)
					return true;
			}
		}

		return query_tree_walker(query,
								 isQueryUsingTempRelation_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  isQueryUsingTempRelation_walker,
								  context);
}

/*
 * addRTEPermissionInfo
 *		为给定 RTE 创建 RTEPermissionInfo 并加入列表。     
 *		              
 *
 * 返回 RTEPermissionInfo 并设置 rte->perminfoindex。  
 */
RTEPermissionInfo *
addRTEPermissionInfo(List **rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	Assert(OidIsValid(rte->relid));
	Assert(rte->perminfoindex == 0);

	/* 没有，新建一个加入列表。   */
	perminfo = makeNode(RTEPermissionInfo);
	perminfo->relid = rte->relid;
	perminfo->inh = rte->inh;
	/* 其他信息会在需要时通过获取相应节点来设置。    */

	*rteperminfos = lappend(*rteperminfos, perminfo);

	/* 记其索引，1 起！   */
	rte->perminfoindex = list_length(*rteperminfos);

	return perminfo;
}

/*
 * getRTEPermissionInfo
 *		在给定列表中查找给定关系的 RTEPermissionInfo。     
 *
 * 这只是简单的 list_nth() 操作，不过保留此函数利于 
 * 进行各种健全性检查。         
 */
RTEPermissionInfo *
getRTEPermissionInfo(List *rteperminfos, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	if (rte->perminfoindex == 0 ||
		rte->perminfoindex > list_length(rteperminfos))
		elog(ERROR, "invalid perminfoindex %u in RTE with relid %u",
			 rte->perminfoindex, rte->relid);
	perminfo = list_nth_node(RTEPermissionInfo, rteperminfos,
							 rte->perminfoindex - 1);
	if (perminfo->relid != rte->relid)
		elog(ERROR, "permission info at index %u (with relid=%u) does not match provided RTE (with relid=%u)",
			 rte->perminfoindex, perminfo->relid, rte->relid);

	return perminfo;
}
