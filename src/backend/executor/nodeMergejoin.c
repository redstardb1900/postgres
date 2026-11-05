/*-------------------------------------------------------------------------
 *
 * nodeMergejoin.c
 *	  支持合并连接的例程    
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergejoin.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程      
 *		ExecMergeJoin			合并连接 outer/inner 关系。  
 *		ExecInitMergeJoin		创建并初始化运行时状态      
 *		ExecEndMergeJoin		清理该节点。 
 *
 * NOTES
 *
 *		合并连接通过把满足 ((= outerKey innerKey) ...) 形式的  
 *		连接子句的内、外侧元组连接而成。     
 *		连接子句列表由查询规划器提供，且可能包含多个   
 *		(= outerKey innerKey) 子句（用于复合排序键）。          
 *
 *		不过，查询执行器需要知道某个外侧元组    
 *		相对内侧元组是"更大"还是"更小"，从而可以   
 *		"同步"这两个关系。例如，考虑以下                     
 *		关系： 
 *
 *				outer: (0 ^1 1 2 5 5 5 6 6 7)	current tuple: 1
 *				inner: (1 ^3 5 5 5 5 6)			current tuple: 3
 *
 *		为继续合并连接，执行器需要扫描内外侧关系，  
 *		直到匹配元组 5 为止。须知当前内侧元组 3        
 *		比外侧元组 1 大，因此应先扫描外侧关系以寻找 
 *		第一个匹配元组，如此反复                       
 *		进行下去。          
 *
 *		因此我们不是直接执行合并连接子句，而是分别  
 *		计算左右键表达式，然后每次比较其中一列         
 *		（见 MJCompare）。规划器会向我们传入足够的     
 *		信息，来说明输入的排序顺序，使我们能够确定   
 *		如何比较。我们可以用合适的 btree 比较函数，       
 *		因为 Postgres 排序的唯一概念由 btree 操作符族所    
 *		定义。                                    
 *
 *
 *		考虑上述关系，假设执行器刚把第一个外侧 "5" 
 *		与最后一个内侧 "5" 连接。接下来是第二个    
 *		外侧 "5" 与所有内侧 "5" 连接。这需要把内侧  
 *		"游标"重新定位到第一个内侧 "5"。这通过"标记" 
 *		第一个内侧 5，从而在与第二个外侧 5 连接前   
 *		可以把"游标"恢复到它那里。访问方法接口提供了
 *		针对元组执行标记与恢复操作的                   
 *		例程。                               
 *
 *
 *		合并连接算法的基本操作流程如下：              
 *
 *		Join {
 *			get initial outer and inner tuples				INITIALIZE
 *			do forever {
 *				while (outer != inner) {					SKIP_TEST
 *					if (outer < inner)
 *						advance outer						SKIPOUTER_ADVANCE
 *					else
 *						advance inner						SKIPINNER_ADVANCE
 *				}
 *				mark inner position							SKIP_TEST
 *				do forever {
 *					while (outer == inner) {
 *						join tuples							JOINTUPLES
 *						advance inner position				NEXTINNER
 *					}
 *					advance outer position					NEXTOUTER
 *					if (outer == mark)						TESTOUTER
 *						restore inner position to mark		TESTOUTER
 *					else
 *						break	// return to top of outer loop
 *				}
 *			}
 *		}
 *
 *		合并连接操作以状态机的方式编码。
 *		在每个状态中，我们先做某件事，然后转入   
 *		另一个状态。该状态保存在节点的执行状态    
 *		信息里，并在多次调用之间保持不变。         
 *		ExecMergeJoin. -cim 10/31/89
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "executor/execdebug.h"
#include "executor/nodeMergejoin.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


/*
 * ExecMergeJoin 状态机的各个状态   
 */
#define EXEC_MJ_INITIALIZE_OUTER		1
#define EXEC_MJ_INITIALIZE_INNER		2
#define EXEC_MJ_JOINTUPLES				3
#define EXEC_MJ_NEXTOUTER				4
#define EXEC_MJ_TESTOUTER				5
#define EXEC_MJ_NEXTINNER				6
#define EXEC_MJ_SKIP_TEST				7
#define EXEC_MJ_SKIPOUTER_ADVANCE		8
#define EXEC_MJ_SKIPINNER_ADVANCE		9
#define EXEC_MJ_ENDOUTER				10
#define EXEC_MJ_ENDINNER				11

/*
 * 各合并子句的运行时数据     
 */
typedef struct MergeJoinClauseData
{
	/* 可执行表达式树       */
	ExprState  *lexpr;			/* 左侧（外侧）输入表达式  */
	ExprState  *rexpr;			/* 右侧（内侧）输入表达式   */

	/*
	 * 如果我们有当前的外侧或内侧输入元组，           
	 * 表达式的值会装入这些字段：  
	 */
	Datum		ldatum;			/* 当前左侧值         */
	Datum		rdatum;			/* 当前右侧值          */
	bool		lisnull;		/* 及其 isnull 标志   */
	bool		risnull;

	/*
	 * 比较左右两侧值所需的一切信息都保存               
	 * 在此。   
	 */
	SortSupportData ssup;
}			MergeJoinClauseData;

/* MJEvalOuterValues 和 MJEvalInnerValues 的结果类型 */
typedef enum
{
	MJEVAL_MATCHABLE,			/* 普通元组，可能参与匹配   */
	MJEVAL_NONMATCHABLE,		/* 含 NULL，无法连接的元组        */
	MJEVAL_ENDOFJOIN			/* 输入结束（物理或实际）    */
} MJEvalResult;


#define MarkInnerTuple(innerTupleSlot, mergestate) \
	ExecCopySlot((mergestate)->mj_MarkedTupleSlot, (innerTupleSlot))


/*
 * MJExamineQuals
 *
 * 本函数解构可合并连接表达式列表，该列表由规划器  
 * 以 "leftexpr = rightexpr" 表达式树的形式交给我们，     
 * 其顺序与输入的排序列相匹配。我们会构建一个       
 * MergeJoinClause 结构体数组，其中包含运行时所需的       
 * 信息。每个结构体实质上告诉我们该如何比较             
 * 原始子句中的两个表达式。         
 *
 * 除了表达式本身，规划器还会传入用于标识每个        
 * 合并键的 btree 操作符族 OID、排序规则 OID、btree 策略号   
 * （BTLessStrategyNumber 或 BTGreaterStrategyNumber）以及             
 * 空值置前标志。可合并连接的操作符是操作符族    
 * 中的一个等值操作符，这两个输入被保证按升序           
 * 或降序（各对应）排列，依据是操作符族与排序规则， 
 * 空值位于范围中指示的一端。这让我们能够从操作符族    
 * 中获得所需的比较函数。                                         
 */
static MergeJoinClause
MJExamineQuals(List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   PlanState *parent)
{
	MergeJoinClause clauses;
	int			nClauses = list_length(mergeclauses);
	int			iClause;
	ListCell   *cl;

	clauses = (MergeJoinClause) palloc0(nClauses * sizeof(MergeJoinClauseData));

	iClause = 0;
	foreach(cl, mergeclauses)
	{
		OpExpr	   *qual = (OpExpr *) lfirst(cl);
		MergeJoinClause clause = &clauses[iClause];
		Oid			opfamily = mergefamilies[iClause];
		Oid			collation = mergecollations[iClause];
		StrategyNumber opstrategy = mergestrategies[iClause];
		bool		nulls_first = mergenullsfirst[iClause];
		int			op_strategy;
		Oid			op_lefttype;
		Oid			op_righttype;
		Oid			sortfunc;

		if (!IsA(qual, OpExpr))
			elog(ERROR, "mergejoin clause is not an OpExpr");

		/*
		 * 为执行准备输入表达式。           
		 */
		clause->lexpr = ExecInitExpr((Expr *) linitial(qual->args), parent);
		clause->rexpr = ExecInitExpr((Expr *) lsecond(qual->args), parent);

		/* 设置排序支持数据 */
		clause->ssup.ssup_cxt = CurrentMemoryContext;
		clause->ssup.ssup_collation = collation;
		if (opstrategy == BTLessStrategyNumber)
			clause->ssup.ssup_reverse = false;
		else if (opstrategy == BTGreaterStrategyNumber)
			clause->ssup.ssup_reverse = true;
		else					/* 规划器出错了 */
			elog(ERROR, "unsupported mergejoin strategy %d", opstrategy);
		clause->ssup.ssup_nulls_first = nulls_first;

		/* 提取该操作符声明的左右数据类型        */
		get_op_opfamily_properties(qual->opno, opfamily, false,
								   &op_strategy,
								   &op_lefttype,
								   &op_righttype);
		if (op_strategy != BTEqualStrategyNumber)	/* 不应发生      */
			elog(ERROR, "cannot merge using non-equality operator %u",
				 qual->opno);

		/*
		 * sortsupport 例程必须知道缩写优化在原则上       
		 * 是否适用。对合并连接而言它绝不适用，因为    
		 * 没有合适的机会把值转换为                    
		 * 替代表示。            
		 */
		clause->ssup.abbreviate = false;

		/* 然后获取匹配支持函数或比较函数       */
		Assert(clause->ssup.comparator == NULL);
		sortfunc = get_opfamily_proc(opfamily,
									 op_lefttype,
									 op_righttype,
									 BTSORTSUPPORT_PROC);
		if (OidIsValid(sortfunc))
		{
			/* 排序支持函数可以提供一个比较器      */
			OidFunctionCall1(sortfunc, PointerGetDatum(&clause->ssup));
		}
		if (clause->ssup.comparator == NULL)
		{
			/* 无支持函数，取得比较函数       */
			sortfunc = get_opfamily_proc(opfamily,
										 op_lefttype,
										 op_righttype,
										 BTORDER_PROC);
			if (!OidIsValid(sortfunc))	/* 不应发生      */
				elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
					 BTORDER_PROC, op_lefttype, op_righttype, opfamily);
			/* 我们将用垫片来调用旧式 btree 比较器       */
			PrepareSortSupportComparisonShim(sortfunc, &clause->ssup);
		}

		iClause++;
	}

	return clauses;
}

/*
 * MJEvalOuterValues
 *
 * 为当前外侧元组求合并连接表达式的值。我们还  
 * 会检测当前外侧元组是否不可能与任何元组匹配     
 * --- 若它产出 NULL 输入即如此，我们假设合并连接  
 * 操作符是严格的。如果该 NULL 在第一个连接列，    
 * 且该列将空值排在末尾，那么我们可以进一步断定 
 * 后续任何元组也不可能匹配，因为它们的第一列   
 * 必然都是空值。不过，只有当不在 FillOuter 模式时    
 * 这一情形才值得关心，否则无论如何我们都得访问
 * 全部这些元组。                  
 *
 * 为方便调用者，我们还让本例程负责检测输入结束   
 * （外侧元组为 NULL），并在看到该情况时返回   
 * MJEVAL_ENDOFJOIN。这样同一段代码便可同时用于              
 * 真正的输入结束，以及由首列 NULL 所代表的               
 * 有效结束。     
 *
 * 我们在 OuterEContext 中求值，每次移到新元组时    
 * 都可以重置。          
 */
static MJEvalResult
MJEvalOuterValues(MergeJoinState *mergestate)
{
	ExprContext *econtext = mergestate->mj_OuterEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* 外侧子计划是否已结束 */
	if (TupIsNull(mergestate->mj_OuterTupleSlot))
		return MJEVAL_ENDOFJOIN;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_outertuple = mergestate->mj_OuterTupleSlot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->ldatum = ExecEvalExpr(clause->lexpr, econtext,
									  &clause->lisnull);
		if (clause->lisnull)
		{
			/* 匹配不可能；能否提前结束连接？   */
			if (i == 0 && !clause->ssup.ssup_nulls_first &&
				!mergestate->mj_FillOuter)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJEvalInnerValues
 *
 * 与上文相同，但针对内侧元组。这里我们必须准备好
 * 从真正的当前内侧元组或标记元组装载数据，         
 * 调用者须告知从哪个 slot 装载。     
 */
static MJEvalResult
MJEvalInnerValues(MergeJoinState *mergestate, TupleTableSlot *innerslot)
{
	ExprContext *econtext = mergestate->mj_InnerEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* 内侧子计划是否已结束 */
	if (TupIsNull(innerslot))
		return MJEVAL_ENDOFJOIN;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_innertuple = innerslot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->rdatum = ExecEvalExpr(clause->rexpr, econtext,
									  &clause->risnull);
		if (clause->risnull)
		{
			/* 匹配不可能；能否提前结束连接？   */
			if (i == 0 && !clause->ssup.ssup_nulls_first &&
				!mergestate->mj_FillInner)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJCompare
 *
 * 对当前两个输入元组比较可合并连接的值，相等 
 * 则返回 0（各合并连接等式都成功），外侧大于  
 * 内侧时返回 >0，外侧小时返回 <0。      
 *
 * 必须已对当前外侧元组和内侧元组分别调用过         
 * MJEvalOuterValues 和 MJEvalInnerValues。           
 */
static int
MJCompare(MergeJoinState *mergestate)
{
	int			result = 0;
	bool		nulleqnull = false;
	ExprContext *econtext = mergestate->js.ps.ps_ExprContext;
	int			i;
	MemoryContext oldContext;

	/*
	 * 在短寿命的上下文中调用比较函数，以防它们泄漏     
	 * 内存 
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		/*
		 * NULL 对 NULL 时做特殊处理，否则用标准比较。 
		 */
		if (clause->lisnull && clause->risnull)
		{
			nulleqnull = true;	/* NULL "=" NULL */
			continue;
		}

		result = ApplySortComparator(clause->ldatum, clause->lisnull,
									 clause->rdatum, clause->risnull,
									 &clause->ssup);

		if (result != 0)
			break;
	}

	/*
	 * 如果出现过 NULL 对 NULL 输入，我们就不希望报告     
	 * 元组相等。相反，若结果仍为 0，就把它改为 +1。这   
	 * 将导致连接的内侧被推进。                
	 *
	 * 同样，如果存在常为假的 joinqual，也不要报告   
	 * 相等。必须把它作为 mergequals 的一部分来检查，     
	 * 否则重扫逻辑会出错。       
	 */
	if (result == 0 &&
		(nulleqnull || mergestate->mj_ConstFalseJoin))
		result = 1;

	MemoryContextSwitchTo(oldContext);

	return result;
}


/*
 * 生成用空值代替内侧元组的伪连接元组，    
 * 如果它通过非连接条件则返回它。 
 */
static TupleTableSlot *
MJFillOuter(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	ExprState  *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_OuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_NullInnerTupleSlot;

	if (ExecQual(otherqual, econtext))
	{
		/*
		 * 限定通过。现在构造所需的投影元组，并返回       
		 * 包含它的那个 slot。    
		 */
		MJ_printf("ExecMergeJoin: returning outer fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered2(node, 1);

	return NULL;
}

/*
 * 生成用空值代替外侧元组的伪连接元组，    
 * 如果它通过非连接条件则返回它。 
 */
static TupleTableSlot *
MJFillInner(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	ExprState  *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_NullOuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_InnerTupleSlot;

	if (ExecQual(otherqual, econtext))
	{
		/*
		 * 限定通过。现在构造所需的投影元组，并返回       
		 * 包含它的那个 slot。    
		 */
		MJ_printf("ExecMergeJoin: returning inner fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered2(node, 1);

	return NULL;
}


/*
 * 检查一个限定条件是常真还是常假。               
 * 若为常假（或空值），把 *is_const_false 置为真。 
 *
 * 常真通常以 NIL 列表来表示，但我们也允许一个实际      
 * 的 bool Const。我们确实预期规划器已经丢弃了所有         
 * 与常假做过 AND 的非常量项。                                 
 */
static bool
check_constant_qual(List *qual, bool *is_const_false)
{
	ListCell   *lc;

	foreach(lc, qual)
	{
		Const	   *con = (Const *) lfirst(lc);

		if (!con || !IsA(con, Const))
			return false;
		if (con->constisnull || !DatumGetBool(con->constvalue))
			*is_const_false = true;
	}
	return true;
}


/* ----------------------------------------------------------------
 *		ExecMergeTupleDump
 *
 *		本函数通过 MJ_dump() 宏来调用，          
 *		定义 EXEC_MERGEJOINDEBUG 时     
 * ----------------------------------------------------------------
 */
#ifdef EXEC_MERGEJOINDEBUG

static void
ExecMergeTupleDumpOuter(MergeJoinState *mergestate)
{
	TupleTableSlot *outerSlot = mergestate->mj_OuterTupleSlot;

	printf("==== outer tuple ====\n");
	if (TupIsNull(outerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(outerSlot);
}

static void
ExecMergeTupleDumpInner(MergeJoinState *mergestate)
{
	TupleTableSlot *innerSlot = mergestate->mj_InnerTupleSlot;

	printf("==== inner tuple ====\n");
	if (TupIsNull(innerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(innerSlot);
}

static void
ExecMergeTupleDumpMarked(MergeJoinState *mergestate)
{
	TupleTableSlot *markedSlot = mergestate->mj_MarkedTupleSlot;

	printf("==== marked tuple ====\n");
	if (TupIsNull(markedSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(markedSlot);
}

static void
ExecMergeTupleDump(MergeJoinState *mergestate)
{
	printf("******** ExecMergeTupleDump ********\n");

	ExecMergeTupleDumpOuter(mergestate);
	ExecMergeTupleDumpInner(mergestate);
	ExecMergeTupleDumpMarked(mergestate);

	printf("********\n");
}
#endif

/* ----------------------------------------------------------------
 *		ExecMergeJoin
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecMergeJoin(PlanState *pstate)
{
	MergeJoinState *node = castNode(MergeJoinState, pstate);
	ExprState  *joinqual;
	ExprState  *otherqual;
	bool		qualResult;
	int			compareResult;
	PlanState  *innerPlan;
	TupleTableSlot *innerTupleSlot;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	ExprContext *econtext;
	bool		doFillOuter;
	bool		doFillInner;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 从节点取得信息    
	 */
	innerPlan = innerPlanState(node);
	outerPlan = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	doFillOuter = node->mj_FillOuter;
	doFillInner = node->mj_FillInner;

	/*
	 * 重置每元组内存上下文，释放上一个元组周期里 
	 * 分配的表达式求值存储。             
	 */
	ResetExprContext(econtext);

	/*
	 * 好了，一切就绪……开始干活吧
	 */
	for (;;)
	{
		MJ_dump(node);

		/*
		 * 取得连接的当前状态，据此采取相应行动。   
		 */
		switch (node->mj_JoinState)
		{
				/*
				 * EXEC_MJ_INITIALIZE_OUTER 表示这是头一次调用      
				 * ExecMergeJoin()，故必须为外侧和内侧子计划各  
				 * 取首个可匹配元组。先用 INITIALIZE_OUTER 状态  
				 * 处理外侧，再前进到 INITIALIZE_INNER 状态      
				 * 来处理内侧子计划。                     
				 */
			case EXEC_MJ_INITIALIZE_OUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_OUTER\n");

				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* 可以去取首个内侧元组了  */
						node->mj_JoinState = EXEC_MJ_INITIALIZE_INNER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* 保持同一状态以取下一个外侧元组*/
						if (doFillOuter)
						{
							/*
							 * 生成用空值代替内侧元组的伪连接
							 * 元组，如果它通过非连接条件    
							 * 则返回它。
							 */
							TupleTableSlot *result;

							result = MJFillOuter(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* 外侧元组已用尽*/
						MJ_printf("ExecMergeJoin: nothing in outer subplan\n");
						if (doFillInner)
						{
							/*
							 * 须为剩余内侧元组发右连接元组  
							 * 并设 MatchedInner = true，使 ENDOUTER  
							 * 状态前进内侧。                     
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							node->mj_MatchedInner = true;
							break;
						}
						/* 否则就完成了。 */
						return NULL;
				}
				break;

			case EXEC_MJ_INITIALIZE_INNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_INNER\n");

				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:

						/*
						 * 好，初始元组已拿到，先跳过           
						 * 不匹配元组。  
						 */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* 如果需要，先标记再前进*/
						if (node->mj_ExtraMarks)
							ExecMarkPos(innerPlan);
						/* 保持同一状态以取下一个内侧元组*/
						if (doFillInner)
						{
							/*
							 * 生成用空值代替外侧元组的伪连接
							 * 元组，如果它通过非连接条件    
							 * 则返回它。
							 */
							TupleTableSlot *result;

							result = MJFillInner(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* 内侧元组已用尽*/
						MJ_printf("ExecMergeJoin: nothing in inner subplan\n");
						if (doFillOuter)
						{
							/*
							 * 需要为所有外侧元组发出左连    
							 * 接元组，包括取到的那个。我们设 
							 * MatchedOuter = false，迫使 ENDINNER 状态 
							 * 先发出第一个元组，然后推进   
							 * 外侧
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							node->mj_MatchedOuter = false;
							break;
						}
						/* 否则就完成了。 */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_JOINTUPLES 表示我们有两个满足合并       
				 * 子句的元组，故我们连接它们，再继续取  
				 * 下个内侧元组（EXEC_MJ_NEXTINNER）
				 */
			case EXEC_MJ_JOINTUPLES:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTUPLES\n");

				/*
				 * 设置状态机的下一状态。不论我们是返回  
				 * 该连接元组，还是落到下文继续执行     
				 * 状态机，结果都会是正确的。         
				 */
				node->mj_JoinState = EXEC_MJ_NEXTINNER;

				/*
				 * 检查附加限定条件，看我们是否真想返回    
				 * 这个连接元组。若不想，可以继续合并。     
				 * 必须区分附加连接条件（须通过，才能把  
				 * 元组视为外连接逻辑中的"已匹配"）            
				 * 与其它限定条件（其必须在真正返回之前 
				 * 通过）。      
				 *
				 * 这里无需 ResetExprContext，我们假定        
				 * 在检查合并限定条件时刚做过一次。每个  
				 * 元组一次应该就够了。但我们确实必须设置
				 * 指向元组的 econtext 链接，供 ExecQual 使     
				 * 用 
				 */
				outerTupleSlot = node->mj_OuterTupleSlot;
				econtext->ecxt_outertuple = outerTupleSlot;
				innerTupleSlot = node->mj_InnerTupleSlot;
				econtext->ecxt_innertuple = innerTupleSlot;

				qualResult = (joinqual == NULL ||
							  ExecQual(joinqual, econtext));
				MJ_DEBUG_QUAL(joinqual, qualResult);

				if (qualResult)
				{
					node->mj_MatchedOuter = true;
					node->mj_MatchedInner = true;

					/* 在反连接中从不返回已匹配元组      */
					if (node->js.jointype == JOIN_ANTI)
					{
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					}

					/*
					 * 若只需考虑第一个匹配的内侧元组，    
					 * 那么处理完这个之后，就前进到下一个
					 * 外侧元组。    
					 */
					if (node->js.single_match)
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;

					/*
					 * 右反连接中从不返回已匹配的元组。     
					 * 若非 inner_unique 连接，则需停留在当前  
					 * 外侧元组上，继续扫描内侧以寻            
					 * 找匹配。     
					 */
					if (node->js.jointype == JOIN_RIGHT_ANTI)
						break;

					qualResult = (otherqual == NULL ||
								  ExecQual(otherqual, econtext));
					MJ_DEBUG_QUAL(otherqual, qualResult);

					if (qualResult)
					{
						/*
						 * 限定通过。现在构造所需的投影    
						 * 元组并返回包含它的那个 slot。          
						 */
						MJ_printf("ExecMergeJoin: returning tuple\n");

						return ExecProject(node->js.ps.ps_ProjInfo);
					}
					else
						InstrCountFiltered2(node, 1);
				}
				else
					InstrCountFiltered1(node, 1);
				break;

				/*
				 * EXEC_MJ_NEXTINNER 表示把内侧扫描推进到下一    
				 * 个元组。若该元组不是 nil，我们接着把它与
				 * 连接限定条件比较。    
				 *
				 * 前进前，检查是否必须为该内侧元组    
				 * 发出外连接填充元组。             
				 */
			case EXEC_MJ_NEXTINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTINNER\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * 生成用空值代替外侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* 只做一次    */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * 现在我们取得下一个内侧元组，如果没有， 
				 * 就前进到下一外侧元组（它可能能与之前   
				 * 标记元组连接）。  
				 *
				 * 注意：此处绝不能做 "extraMarks"，因为之后
				 * 还需回到已标记的元组。  
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:

						/*
						 * 测试新的内侧元组看它是否匹配   
						 * 外侧
						 *
						 * 如果两者匹配，则连接它们并继续到  
						 * 下个内侧元组（EXEC_MJ_JOINTUPLES）
						 *
						 * 若两者不匹配，则前进到下一外侧  
						 * 元组
						 */
						compareResult = MJCompare(node);
						MJ_DEBUG_COMPARE(compareResult);

						if (compareResult == 0)
							node->mj_JoinState = EXEC_MJ_JOINTUPLES;
						else if (compareResult < 0)
							node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						else	/* compareResult > 0 不应发生      */
							elog(ERROR, "mergejoin input data is out of order");
						break;
					case MJEVAL_NONMATCHABLE:

						/*
						 * 它含有一个 NULL，因此无法匹配任何   
						 * 外侧元组，故可跳过比较，并假定新的
						 * 元组大于当前外侧元组。       
						 */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:

						/*
						 * 内侧元组已用尽。这可能只是有效的  
						 * 结束而非内计划的物理末尾，所以要
						 * 把 mj_InnerTupleSlot 置为 null，以确保  
						 * 不取更多内侧元组。（我们需要这一  
						 * 取巧手法，因为我们没有进入假定内  
						 * 侧计划已耗尽的状态。）      
						 */
						node->mj_InnerTupleSlot = NULL;
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
				}
				break;

				/*-------------------------------------------
				 * EXEC_MJ_NEXTOUTER 指  
				 *
				 *				outer inner
				 * outer tuple -  5		5  - marked tuple
				 *				  5		5
				 *				  6		6  - inner tuple
				 *				  7		7
				 *
				 * 我们知道我们刚碰上    
				 * 首个大于当前外侧元组的内侧元组（或 
				 * 是内侧流的结束）    
				 * 于是取新外侧元组，然后
				 * 接着把它与标记元组进行比较    
				 * (EXEC_MJ_TESTOUTER)
				 *
				 * 前进前，检查是否必须为该外侧元组    
				 * 发出外连接填充元组。             
				 *------------------------------------------------
				 */
			case EXEC_MJ_NEXTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTOUTER\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * 生成用空值代替内侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* 只做一次    */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * 现在取下一个外侧元组，若有
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* 去把新元组与标记元组做比较        */
						node->mj_JoinState = EXEC_MJ_TESTOUTER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* 无法匹配，取下一外侧元组   */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:
						/* 外侧元组已用尽*/
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * 需要为剩余内侧元组发出右连接  
							 * 元组。    
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/* 否则就完成了。 */
						return NULL;
				}
				break;

				/*--------------------------------------------------------
				 * EXEC_MJ_TESTOUTER 若新的外侧元组与标记元组 
				 * 满足合并子句，则外侧扫描存在重复，
				 * 需把内侧扫描恢复到标记元组处，然后   
				 * 让新的外侧元组与这些内侧元组            
				 * 逐个连接。                       
				 *
				 * 此时情形如下：
				 *						  outer inner
				 *							4	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	5	  5
				 *							6	  8  - inner tuple
				 *							7	 12
				 *
				 *				新外侧元组 == 标记元组
				 *
				 * 若外侧元组未通过测试，标记元组便已
				 * 处理完毕，之后必须寻找与当前内侧 
				 * 元组的匹配。此后跳过外侧直至   
				 * 外侧 >= 内侧                                 
				 * (EXEC_MJ_SKIP_TEST).
				 *
				 *		此时情形如下：
				 *
				 *						  outer inner
				 *							5	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	6	  8  - inner tuple
				 *							7	 12
				 *
				 *				新外侧元组>标记元组  
				 *
				 *---------------------------------------------------------
				 */
			case EXEC_MJ_TESTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_TESTOUTER\n");

				/*
				 * 这里必须把外侧元组与被标记的内侧元组    
				 * 比较。（可以忽略 MJEvalInnerValues 的结果，
				 * 因为标记的内侧元组肯定可以匹配。）  
				 */
				innerTupleSlot = node->mj_MarkedTupleSlot;
				(void) MJEvalInnerValues(node, innerTupleSlot);

				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					/*
					 * 合并子句已匹配，现在将内侧扫描位置 
					 * 恢复到第一个标记处，并让该元组（以及 
					 * 后续元组）与新外侧元组连接。
					 *
					 * 若能确定无需标记和恢复，不必回退；  
					 * 当前内侧元组就已经是首个              
					 * 可能匹配。                           
					 *
					 * 注意：重扫内侧元组时我们无需操心    
					 * MatchedInner 状态。我们知道它们全部都会 
					 * 与新外侧元组匹配，故不会作为填充  
					 * 元组发出。这*仅仅*是因为我们要求    
					 * 附加连接条件须为常量：右连接、右反  
					 * 连接或全连接时 --- 否则部分重扫   
					 * 元组或许过不了附加连接条件。常真     
					 * 附加条件显然不会如此，而常假的      
					 * 情形则用强迫合并子句永不匹配处理，   
					 * 所以我们根本就                               
					 * 不会走到这里。       
					 */
					if (!node->mj_SkipMarkRestore)
					{
						ExecRestrPos(innerPlan);

						/*
						 * ExecRestrPos 理应返回一个新的 slot，  
						 * 但它没有，所以使用被标记的 slot。 
						 * （先前返回的 mj_InnerTupleSlot 不能      
						 * 被假定仍持有所需元组。）   
						 */
						node->mj_InnerTupleSlot = innerTupleSlot;
						/* 无需再调用 MJEvalInnerValues      */
					}

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else if (compareResult > 0)
				{
					/* ----------------
					 *	若新外侧元组未匹配标记的内侧元组， 
					 *	那么情形就如下所示： 
					 *
					 *			 outer inner
					 *			   4	 4	- marked tuple
					 * new outer - 5	 4
					 *			   6	 5	- inner tuple
					 *			   7
					 *
					 *	这意味着之后所有外侧元组都会大于    
					 *	标记的内侧元组。所以无需重访任何    
					 *	被标记的元组，而可以直接去找与当前
					 *	内侧元组匹配的对象。如果没有更多  
					 *	内侧元组，也就不可能再有匹配。
					 * ----------------
					 */
					innerTupleSlot = node->mj_InnerTupleSlot;

					/* 为当前内侧重载比较数据        */
					switch (MJEvalInnerValues(node, innerTupleSlot))
					{
						case MJEVAL_MATCHABLE:
							/* 接着把它与当前外侧元组比较    */
							node->mj_JoinState = EXEC_MJ_SKIP_TEST;
							break;
						case MJEVAL_NONMATCHABLE:

							/*
							 * 当前内侧元组无法匹配任何外侧；
							 * 与其推进外侧，不如推进内侧  
							 * 扫描
							 */
							node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
							break;
						case MJEVAL_ENDOFJOIN:
							/* 内侧元组已用尽*/
							if (doFillOuter)
							{
								/*
								 * 需要为剩余外侧元组发出左连接 
								 * 元组。    
								 */
								node->mj_JoinState = EXEC_MJ_ENDINNER;
								break;
							}
							/* 否则就完成了。 */
							return NULL;
					}
				}
				else			/* compareResult < 0 不应发生      */
					elog(ERROR, "mergejoin input data is out of order");
				break;

				/*----------------------------------------------------------
				 * EXEC_MJ_SKIP_TEST 表示比较两个元组，如果不   
				 * 匹配，就跳过较小者。  
				 *
				 * 例如：   
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple -  6		8  - inner tuple
				 *				  7    12
				 *				  8    14
				 *
				 * 我们必须前进外侧扫描，
				 * 直到找到外侧 8。   
				 *
				 * 另一方面：   
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple - 12		8  - inner tuple
				 *				 14    10
				 *				 17    12
				 *
				 * 我们必须前进内侧扫描，
				 * 直到找到内侧 12。   
				 *----------------------------------------------------------
				 */
			case EXEC_MJ_SKIP_TEST:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIP_TEST\n");

				/*
				 * 前进前，先确认当前元组不满足合并      
				 * 子句。如果满足，那么我们就更新标记元组
				 * 的位置并去连接它们。         
				 */
				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					if (!node->mj_SkipMarkRestore)
						ExecMarkPos(innerPlan);

					MarkInnerTuple(node->mj_InnerTupleSlot, node);

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else if (compareResult < 0)
					node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
				else
					/* compareResult > 0 */
					node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
				break;

				/*
				 * EXEC_MJ_SKIPOUTER_ADVANCE：跳过已知不会与          
				 * 内侧元组连接的外侧元组。    
				 *
				 * 前进前，检查是否必须为该外侧元组    
				 * 发出外连接填充元组。             
				 */
			case EXEC_MJ_SKIPOUTER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPOUTER_ADVANCE\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * 生成用空值代替内侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* 只做一次    */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * 现在取下一个外侧元组，若有
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* 去把新元组与当前内侧元组比较      */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* 无法匹配，取下一外侧元组   */
						node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* 外侧元组已用尽*/
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * 需要为剩余内侧元组发出右连接  
							 * 元组。    
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/* 否则就完成了。 */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_SKIPINNER_ADVANCE：跳过已知不会与          
				 * 外侧元组连接的内侧元组。    
				 *
				 * 前进前，检查是否必须为该内侧元组    
				 * 发出外连接填充元组。             
				 */
			case EXEC_MJ_SKIPINNER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPINNER_ADVANCE\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * 生成用空值代替外侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* 只做一次    */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* 如果需要，先标记再前进*/
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * 现在取下一个内侧元组，若有
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* 计算连接值并检查不可匹配性          */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:
						/* 接着把它与当前外侧元组比较    */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:

						/*
						 * 当前内侧元组无法匹配任何外侧；
						 * 与其推进外侧，不如推进内侧扫描。
						 */
						node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* 内侧元组已用尽*/
						MJ_printf("ExecMergeJoin: end of inner subplan\n");
						outerTupleSlot = node->mj_OuterTupleSlot;
						if (doFillOuter && !TupIsNull(outerTupleSlot))
						{
							/*
							 * 需要为剩余外侧元组发出左连接 
							 * 元组。    
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							break;
						}
						/* 否则就完成了。 */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_ENDOUTER 表示外侧元组已经用尽，但我们
				 * 正在做右/右反/全连接，所以必须把剩余    
				 * 的未匹配内侧元组填上空值。        
				 */
			case EXEC_MJ_ENDOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDOUTER\n");

				Assert(doFillInner);

				if (!node->mj_MatchedInner)
				{
					/*
					 * 生成用空值代替外侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* 只做一次    */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* 如果需要，先标记再前进*/
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * 现在取下一个内侧元组，若有
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of inner subplan\n");
					return NULL;
				}

				/* 否则留在 ENDOUTER 状态并处理下一元组。  */
				break;

				/*
				 * EXEC_MJ_ENDINNER 表示内侧元组已经用尽，但我们
				 * 正在做左/全连接，因此必须把剩余的       
				 * 未匹配外侧元组填上空值。 
				 */
			case EXEC_MJ_ENDINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDINNER\n");

				Assert(doFillOuter);

				if (!node->mj_MatchedOuter)
				{
					/*
					 * 生成用空值代替内侧元组的伪连接元   
					 * 组，如果它通过非连接条件则返回它。  
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* 只做一次    */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * 现在取下一个外侧元组，若有
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				if (TupIsNull(outerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of outer subplan\n");
					return NULL;
				}

				/* 否则留在 ENDINNER 状态并处理下一元组。  */
				break;

				/*
				 * 状态值出错？ 
				 */
			default:
				elog(ERROR, "unrecognized mergejoin state: %d",
					 (int) node->mj_JoinState);
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitMergeJoin
 * ----------------------------------------------------------------
 */
MergeJoinState *
ExecInitMergeJoin(MergeJoin *node, EState *estate, int eflags)
{
	MergeJoinState *mergestate;
	TupleDesc	outerDesc,
				innerDesc;
	const TupleTableSlotOps *innerOps;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "initializing node");

	/*
	 * 创建状态结构    
	 */
	mergestate = makeNode(MergeJoinState);
	mergestate->js.ps.plan = (Plan *) node;
	mergestate->js.ps.state = estate;
	mergestate->js.ps.ExecProcNode = ExecMergeJoin;
	mergestate->js.jointype = node->join.jointype;
	mergestate->mj_ConstFalseJoin = false;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &mergestate->js.ps);

	/*
	 * 我们还需要两个额外 econtext，用来从左右输入     
	 * 元组中计算连接表达式。节点常规的 econtext            
	 * 不行，因为它被重置得太频繁。        
	 */
	mergestate->mj_OuterEContext = CreateExprContext(estate);
	mergestate->mj_InnerEContext = CreateExprContext(estate);

	/*
	 * 初始化子节点    
	 *
	 * 内侧子节点必须支持 MARK/RESTORE，除非已检测            
	 * 出我们并不需要它。注意：若存在非合并子句      
	 * 连接条件，不得设置 skip_mark_restore，否则行不通。 
	 */
	Assert(node->join.joinqual == NIL || !node->skip_mark_restore);
	mergestate->mj_SkipMarkRestore = node->skip_mark_restore;

	outerPlanState(mergestate) = ExecInitNode(outerPlan(node), estate, eflags);
	outerDesc = ExecGetResultType(outerPlanState(mergestate));
	innerPlanState(mergestate) = ExecInitNode(innerPlan(node), estate,
											  mergestate->mj_SkipMarkRestore ?
											  eflags :
											  (eflags | EXEC_FLAG_MARK));
	innerDesc = ExecGetResultType(innerPlanState(mergestate));

	/*
	 * 对于某些类型的内侧子节点，每次越过我们          
	 * 绝不会返回的内侧元组时就发 MARK 是有利的；           
	 * 而对于另一些类型，对我们无法返回的元组         
	 * 发 MARK 是浪费。判断哪种情形；若想发出 "unnecessary"  
	 * MARK 就设置 mj_ExtraMarks。
	 *
	 * 目前只有 Material 需要这些额外 MARK，而且只有          
	 * eflags 未含 REWIND 时才有用。  
	 *
	 * 注意：对 IndexScan 与 IndexOnlyScan，我们                   
	 * 就*绝不*能设置 mj_ExtraMarks；否则可能试图在            
	 * 首个内侧元组前设标记，其不支持。 
	 */
	if (IsA(innerPlan(node), Material) &&
		(eflags & EXEC_FLAG_REWIND) == 0 &&
		!mergestate->mj_SkipMarkRestore)
		mergestate->mj_ExtraMarks = true;
	else
		mergestate->mj_ExtraMarks = false;

	/*
	 * 初始化结果 slot、类型与投影。   
	 */
	ExecInitResultTupleSlotTL(&mergestate->js.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&mergestate->js.ps, NULL);

	/*
	 * 元组表初始化        
	 */
	innerOps = ExecGetResultSlotOps(innerPlanState(mergestate), NULL);
	mergestate->mj_MarkedTupleSlot = ExecInitExtraTupleSlot(estate, innerDesc,
															innerOps);

	/*
	 * 初始化子表达式       
	 */
	mergestate->js.ps.qual =
		ExecInitQual(node->join.plan.qual, (PlanState *) mergestate);
	mergestate->js.joinqual =
		ExecInitQual(node->join.joinqual, (PlanState *) mergestate);
	/* mergeclauses 将在后面处理*/

	/*
	 * 检测是否只需考虑第一个匹配的内侧元组             
	 */
	mergestate->js.single_match = (node->join.inner_unique ||
								   node->join.jointype == JOIN_SEMI);

	/* 如果需要，为外连接设置空元组    */
	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = false;
			break;
		case JOIN_LEFT:
		case JOIN_ANTI:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = false;
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);
			break;
		case JOIN_RIGHT:
		case JOIN_RIGHT_ANTI:
			mergestate->mj_FillOuter = false;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);

			/*
			 * 无法用非常量的附加连接子句处理右连接、    
			 * 右反连接或全连接。本应由规划器查出。      
			 */
			if (!check_constant_qual(node->join.joinqual,
									 &mergestate->mj_ConstFalseJoin))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RIGHT JOIN is only supported with merge-joinable join conditions")));
			break;
		case JOIN_FULL:
			mergestate->mj_FillOuter = true;
			mergestate->mj_FillInner = true;
			mergestate->mj_NullOuterTupleSlot =
				ExecInitNullTupleSlot(estate, outerDesc, &TTSOpsVirtual);
			mergestate->mj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate, innerDesc, &TTSOpsVirtual);

			/*
			 * 无法用非常量的附加连接子句处理右连接、    
			 * 右反连接或全连接。本应由规划器查出。      
			 */
			if (!check_constant_qual(node->join.joinqual,
									 &mergestate->mj_ConstFalseJoin))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN is only supported with merge-joinable join conditions")));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * 预处理合并子句       
	 */
	mergestate->mj_NumClauses = list_length(node->mergeclauses);
	mergestate->mj_Clauses = MJExamineQuals(node->mergeclauses,
											node->mergeFamilies,
											node->mergeCollations,
											node->mergeStrategies,
											node->mergeNullsFirst,
											(PlanState *) mergestate);

	/*
	 * 初始化连接状态
	 */
	mergestate->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	mergestate->mj_MatchedOuter = false;
	mergestate->mj_MatchedInner = false;
	mergestate->mj_OuterTupleSlot = NULL;
	mergestate->mj_InnerTupleSlot = NULL;

	/*
	 * 初始化成功          
	 */
	MJ1_printf("ExecInitMergeJoin: %s\n",
			   "node initialized");

	return mergestate;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeJoin
 *
 * 旧注释   
 *		负责释放经 C 例程分配的存储。 
 * ----------------------------------------------------------------
 */
void
ExecEndMergeJoin(MergeJoinState *node)
{
	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "ending node processing");

	/*
	 * 释放 exprcontext  
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * 清空元组表          
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->mj_MarkedTupleSlot);

	/*
	 * 关闭子计划       
	 */
	ExecEndNode(innerPlanState(node));
	ExecEndNode(outerPlanState(node));

	MJ1_printf("ExecEndMergeJoin: %s\n",
			   "node processing ended");
}

void
ExecReScanMergeJoin(MergeJoinState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);

	ExecClearTuple(node->mj_MarkedTupleSlot);

	node->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
	node->mj_MatchedOuter = false;
	node->mj_MatchedInner = false;
	node->mj_OuterTupleSlot = NULL;
	node->mj_InnerTupleSlot = NULL;

	/*
	 * 如果子节点的 chgParam 非空，那么计划将由首个       
	 * ExecProcNode 重扫
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
	if (innerPlan->chgParam == NULL)
		ExecReScan(innerPlan);
}
