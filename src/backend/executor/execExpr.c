/*-------------------------------------------------------------------------
 *
 * execExpr.c
 *	  表达式求值基础设施。       
 *
 *	执行器启动时，我们会把每棵表达式树（先前已由 
 *	解析器和规划器处理过）编译成 ExprState，                
 *	借助 ExecInitExpr() 等函数。这会把该树转换成一个     
 *	扁平的 ExprEvalStep 数组，可将其视为程序中的指令。   
 *	运行时，我们会从第一步开始执行这些步骤，直到遇到
 *	EEOP_DONE 为止。 
 *
 *	本文件包含"编译"逻辑，它与我们采用的                
 *	具体执行技术（switch 语句、computed goto、JIT              
 *	编译等）无关。 
 *
 *	背景知识参见 src/backend/executor/README，特别是           
 *	"Expression Trees and ExprState nodes"、"Expression Initialization"
 *	与 "Expression Evaluation" 各节。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExpr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


typedef struct ExprSetupInfo
{
	/* 从 inner/outer/scan 元组槽取的最高属性号：               */
	AttrNumber	last_inner;
	AttrNumber	last_outer;
	AttrNumber	last_scan;
	/* 表达式中出现的 MULTIEXPR SubPlan 节点：    */
	List	   *multiexpr_subplans;
} ExprSetupInfo;

static void ExecReadyExpr(ExprState *state);
static void ExecInitExprRec(Expr *node, ExprState *state,
							Datum *resv, bool *resnull);
static void ExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args,
						 Oid funcid, Oid inputcollid,
						 ExprState *state);
static void ExecCreateExprSetupSteps(ExprState *state, Node *node);
static void ExecPushExprSetupSteps(ExprState *state, ExprSetupInfo *info);
static bool expr_setup_walker(Node *node, ExprSetupInfo *info);
static bool ExecComputeSlotInfo(ExprState *state, ExprEvalStep *op);
static void ExecInitWholeRowVar(ExprEvalStep *scratch, Var *variable,
								ExprState *state);
static void ExecInitSubscriptingRef(ExprEvalStep *scratch,
									SubscriptingRef *sbsref,
									ExprState *state,
									Datum *resv, bool *resnull);
static bool isAssignmentIndirectionExpr(Expr *expr);
static void ExecInitCoerceToDomain(ExprEvalStep *scratch, CoerceToDomain *ctest,
								   ExprState *state,
								   Datum *resv, bool *resnull);
static void ExecBuildAggTransCall(ExprState *state, AggState *aggstate,
								  ExprEvalStep *scratch,
								  FunctionCallInfo fcinfo, AggStatePerTrans pertrans,
								  int transno, int setno, int setoff, bool ishash,
								  bool nullcheck);


/*
 * ExecInitExpr：为执行准备一棵表达式树      
 *
 * 本函数构建并返回一个 ExprState，用以实现给定的    
 * Expr 节点树。返回的 ExprState 随后可交给 ExecEvalExpr       
 * 执行。由于就 Expr 树本身而言，ExecInitExpr 和         
 * ExecEvalExpr 均视其为只读，因此同一棵计划树的多个      
 * 不同执行可以并发进行。（但请注意 ExprState 在运行      
 * 时会发生变动，故不能并发地重复使用。）    
 *
 * 必须在寿命足以覆盖表达式重复执行需求的内存上下文  
 * 中调用本函数。通常，该上下文会与相关 ExprContext 的 
 * 每查询上下文相同。                                     
 *
 * 树中发现的任何 Aggref、WindowFunc 或 SubPlan 节点都会     
 * 加入父 PlanState 所持有的此类节点列表。
 *
 * 注意：没有 ExecEndExpr 函数；我们假定所需的资源   
 * 清理只需释放构建状态树的内存上下文即可完成。 
 * 需要额外清理工作的函数可以在 ExprContext 中注册    
 * 关闭回调。                                                  
 *
 *	'node' 是待编译表达式树的根节点。       
 *	'parent' 是拥有该表达式的 PlanState 节点。   
 *
 * 如果准备的表达式不关联计划树，'parent' 可以为   
 * NULL。（这样它就不能包含聚合或子计划。）            
 * 这类情形通常应当经由 ExecPrepareExpr，而非直接到这里。 
 *
 * 另外，若 'node' 为 NULL，我们就直接返回 NULL。这对某些  
 * 调用方很方便：它们可能有、也可能没有需要编译的    
 * 表达式。注意：NULL ExprState 指针*不能*交给              
 * ExecEvalExpr，但 ExecQual/ExecCheck 会接受（视为"真"）。      
 */
ExprState *
ExecInitExpr(Expr *node, PlanState *parent)
{
	ExprState  *state;
	ExprEvalStep scratch = {0};

	/* 特殊情形：NULL 表达式产生 NULL ExprState 指针       */
	if (node == NULL)
		return NULL;

	/* 用空步骤列表初始化 ExprState     */
	state = makeNode(ExprState);
	state->expr = node;
	state->parent = parent;
	state->ext_params = NULL;

	/* 按需插入设置步骤     */
	ExecCreateExprSetupSteps(state, (Node *) node);

	/* 编译表达式主体         */
	ExecInitExprRec(node, state, &state->resvalue, &state->resnull);

	/* 最后追加 DONE 步骤    */
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * ExecInitExprWithParams：为执行准备独立的表达式树             
 *
 * 与 ExecInitExpr 相同，只是没有父 PlanState，                     
 * 但可能会有描述 PARAM_EXTERN 参数的 ParamListInfo。          
 */
ExprState *
ExecInitExprWithParams(Expr *node, ParamListInfo ext_params)
{
	ExprState  *state;
	ExprEvalStep scratch = {0};

	/* 特殊情形：NULL 表达式产生 NULL ExprState 指针       */
	if (node == NULL)
		return NULL;

	/* 用空步骤列表初始化 ExprState     */
	state = makeNode(ExprState);
	state->expr = node;
	state->parent = NULL;
	state->ext_params = ext_params;

	/* 按需插入设置步骤     */
	ExecCreateExprSetupSteps(state, (Node *) node);

	/* 编译表达式主体         */
	ExecInitExprRec(node, state, &state->resvalue, &state->resnull);

	/* 最后追加 DONE 步骤    */
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * ExecInitQual：准备一个待 ExecQual 执行的 qual
 *
 * 准备对合取布尔表达式（即具有隐式 AND 语义的 qual       
 * 列表）求值，若无任何子表达式为 false 则返回
 * true。                  
 *
 * 列表为空时必须返回 true。由于这是极常见的情形，       
 * 我们进一步将其优化为转换成 NULL ExprState 指针，       
 * 而不是构造一个计算常量 TRUE 的 ExprState。（某些       
 * 特别热点 ExecQual 调用方会检测到这一点，从而干脆  
 * 避开它。）  
 *
 * 若任一子表达式得出 NULL，则整个合取的结果为 false。    
 * 这使得 ExecQual 主要适合用来求值 WHERE 子句，         
 * 因为 SQL 规定 WHERE 结果为 null 的元组不会被              
 * 选中。    
 */
ExprState *
ExecInitQual(List *qual, PlanState *parent)
{
	ExprState  *state;
	ExprEvalStep scratch = {0};
	List	   *adjust_jumps = NIL;
	ListCell   *lc;

	/* 限制列表为空时短路（此处与 ExecQual 中均是）   */
	if (qual == NIL)
		return NULL;

	Assert(IsA(qual, List));

	state = makeNode(ExprState);
	state->expr = (Expr *) qual;
	state->parent = parent;
	state->ext_params = NULL;

	/* 标记表达式将配合 ExecQual() 使用    */
	state->flags = EEO_FLAG_IS_QUAL;

	/* 按需插入设置步骤     */
	ExecCreateExprSetupSteps(state, (Node *) qual);

	/*
	 * 对于返回 NULL 的表达式，ExecQual() 需要返回 false。      
	 * 这让我们在首次遇到 NULL 时即可短路求值。qual     
	 * 求值是热路径，故值得为 qual 求值用专门操作码，  
	 * 该操作码比 BOOL_AND 更简单（后者的 NULL                   
	 * 处理更复杂）。           
	 */
	scratch.opcode = EEOP_QUAL;

	/*
	 * 各 qual 表达式的目标可用 ExprState 的 resvalue/resnull。  
	 */
	scratch.resvalue = &state->resvalue;
	scratch.resnull = &state->resnull;

	foreach(lc, qual)
	{
		Expr	   *node = (Expr *) lfirst(lc);

		/* 先对表达式求值     */
		ExecInitExprRec(node, state, &state->resvalue, &state->resnull);

		/* 再生成 EEOP_QUAL 检测是否 false（或 null）  */
		scratch.d.qualexpr.jumpdone = -1;
		ExprEvalPushStep(state, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	/* 调整跳转目标  */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = state->steps_len;
	}

	/*
	 * 最后无需再做任何事。最后一个 qual 表达式必然        
	 * 已得出 TRUE，而其结果就存放在所需的输出位置中，   
	 * 因此即告完成。
	 */
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * ExecInitCheck：准备一个待 ExecCheck 执行的检查约束     
 *
 * 与 ExecInitQual/ExecQual 非常相似，不同之处在于合取      
 * 得出 null 时视为 TRUE。该行为很适合评估                
 * 检查约束，因为 SQL 规定 NULL 约束条件                    
 * 不算失败。             
 *
 * 注意与 ExecInitQual 一样，本函数要求输入为隐式 AND     
 * 格式。持有常规显式 AND 格式表达式的 ExecCheck 用户   
 * 只需应用 ExecInitExpr 就能产生适合 ExecCheck 的输入。 
 */
ExprState *
ExecInitCheck(List *qual, PlanState *parent)
{
	/* 限制列表为空时短路（此处与 ExecCheck 中均是）   */
	if (qual == NIL)
		return NULL;

	Assert(IsA(qual, List));

	/*
	 * 只需把隐式 AND 列表转换为显式 AND（若多于             
	 * 一个）并正常编译。与 ExecQual 不同，不能          
	 * 对 NULL 结果短路，因此需要常规的 AND 行为。          
	 */
	return ExecInitExpr(make_ands_explicit(qual), parent);
}

/*
 * 对一个表达式列表调用 ExecInitExpr()，返回 ExprState 列表。
 */
List *
ExecInitExprList(List *nodes, PlanState *parent)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, nodes)
	{
		Expr	   *e = lfirst(lc);

		result = lappend(result, ExecInitExpr(e, parent));
	}

	return result;
}

/*
 *		ExecBuildProjectionInfo
 *
 * 构建一个 ProjectionInfo 节点，用于在给定的 econtext 中   
 * 对给定 tlist 求值，并把结果存入元组槽（调用方必须  
 * 已确保该元组槽具有与 tlist 匹配的描述符！）
 *
 * inputDesc 可为 NULL；若非 NULL，我们会检查 tlist 中        
 * 的简单 Var 是否匹配描述符。为关系扫描计划节点  
 * 提供 inputDesc 很重要，可作为交叉检查，确认该关         
 * 系自计划生成以来未被更改过。在计划的较高层级，    
 * 无需再复查。          
 *
 * 其实现方式是在内部构建一个 ExprState，一次性完成      
 * 整个投影。            
 *
 * 注意：在 PG v10 之前，targetList 是 ExprState 列表；        
 * 现在它应当是规划器创建的 targetlist——编译在此进行。 
 */
ProjectionInfo *
ExecBuildProjectionInfo(List *targetList,
						ExprContext *econtext,
						TupleTableSlot *slot,
						PlanState *parent,
						TupleDesc inputDesc)
{
	ProjectionInfo *projInfo = makeNode(ProjectionInfo);
	ExprState  *state;
	ExprEvalStep scratch = {0};
	ListCell   *lc;

	projInfo->pi_exprContext = econtext;
	/* 将 ExprState 内嵌进 ProjectionInfo，而不是额外 palloc      */
	projInfo->pi_state.type = T_ExprState;
	state = &projInfo->pi_state;
	state->expr = (Expr *) targetList;
	state->parent = parent;
	state->ext_params = NULL;

	state->resultslot = slot;

	/* 按需插入设置步骤     */
	ExecCreateExprSetupSteps(state, (Node *) targetList);

	/* 现在编译每个 tlist 列  */
	foreach(lc, targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *variable = NULL;
		AttrNumber	attnum = 0;
		bool		isSafeVar = false;

		/*
		 * 若 tlist 表达式是安全的非系统 Var，用快速路径 
		 * 的 ASSIGN_*_VAR 操作码。"安全"即无需在计划启动  
		 * 期间应用 CheckVarSlotCompatibility()。若提供了源槽，  
		 * 我们就在此处做等价的测试；若未提供槽，         
		 * 则假定无需检查，因为我们处理的是                 
		 * 非关系扫描层级的表达式。      
		 */
		if (tle->expr != NULL &&
			IsA(tle->expr, Var) &&
			((Var *) tle->expr)->varattno > 0)
		{
			/* 非系统 Var，但有多安全？  */
			variable = (Var *) tle->expr;
			attnum = variable->varattno;

			if (inputDesc == NULL)
				isSafeVar = true;	/* 无法检查，只假定 OK */
			else if (attnum <= inputDesc->natts)
			{
				Form_pg_attribute attr = TupleDescAttr(inputDesc, attnum - 1);

				/*
				 * 若用户属性已删除或类型不匹配，不要使用 
				 * ASSIGN_*_VAR，而是交给常规表达式机制来 
				 * 处理（它可能会报错）。                 
				 */
				if (!attr->attisdropped && variable->vartype == attr->atttypid)
				{
					isSafeVar = true;
				}
			}
		}

		if (isSafeVar)
		{
			/* 快速路径：只生成 EEOP_ASSIGN_*_VAR 步骤  */
			switch (variable->varno)
			{
				case INNER_VAR:
					/* 从内节点获取元组          */
					scratch.opcode = EEOP_ASSIGN_INNER_VAR;
					break;

				case OUTER_VAR:
					/* 从外节点获取元组          */
					scratch.opcode = EEOP_ASSIGN_OUTER_VAR;
					break;

					/* INDEX_VAR 由默认分支处理      */

				default:
					/* 从被扫描的关系获取元组             */
					scratch.opcode = EEOP_ASSIGN_SCAN_VAR;
					break;
			}

			scratch.d.assign_var.attnum = attnum - 1;
			scratch.d.assign_var.resultnum = tle->resno - 1;
			ExprEvalPushStep(state, &scratch);
		}
		else
		{
			/*
			 * 否则，按常规编译该列表达式。        
			 *
			 * 不能让表达式直接求值到结果槽中，因为    
			 * 结果槽（exprstate 亦然）在执行间会变化。    
			 * 先求值到 ExprState 的 resvalue/resnull，             
			 * 再移动。                                        
			 */
			ExecInitExprRec(tle->expr, state,
							&state->resvalue, &state->resnull);

			/*
			 * 该列可能在上层节点中被引用多次，因此      
			 * 需强制其为 R/O，但仅当它可能是展开 datum 时。 
			 */
			if (get_typlen(exprType((Node *) tle->expr)) == -1)
				scratch.opcode = EEOP_ASSIGN_TMP_MAKE_RO;
			else
				scratch.opcode = EEOP_ASSIGN_TMP;
			scratch.d.assign_tmp.resultnum = tle->resno - 1;
			ExprEvalPushStep(state, &scratch);
		}
	}

	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return projInfo;
}

/*
 *		ExecBuildUpdateProjection
 *
 * 构建一个 ProjectionInfo 节点，用于在 UPDATE 期间构造     
 * 新元组。该投影会在给定的 econtext 中执行，结果将      
 * 被存入给定的元组槽。（调用方必须已确保该元组槽     
 * 具有与目标 rel 匹配的描述符！）   
 *
 * 当 evalTargetList 为 false 时，targetList 包含的是已由    
 * 子计划节点计算出的 UPDATE ... SET 表达式；                  
 * 假定来自该 tlist 的值可在"outer"元组槽中获得。        
 * 当 evalTargetList 为 true 时，targetList 包含必须计算    
 * 的 UPDATE ... SET 表达式（其中可能引用                   
 * 外层、内层或扫描元组槽）。
 *
 * 无论哪种情形，targetColnos 都包含一个列表，列出与      
 * targetList 中非 resjunk 条目对应的目标列号。tlist 的        
 * 值将被赋给结果元组槽的这些列。未列入                   
 * targetColnos 的目标列将用 UPDATE 的旧元组填充，            
 * 其假定可在"scan"元组槽中获得。          
 *
 * targetList 中也可以有 resjunk 列。若 evalTargetList          
 * 为 true 则必须求值，但其值会被丢弃。       
 *
 * relDesc 必须描述我们打算更新的关系。     
 *
 * 这基本上是 ExecBuildProjectionInfo 的一个特化变体。   
 * 但它还会执行与 ExecCheckPlanOutput 等价的健全性检查。     
 * 由于从不构造与整个待赋值元组等价的常规 
 * tlist，因此没有便捷的途径来应用             
 * ExecCheckPlanOutput，必须在此处做安全检查。   
 */
ProjectionInfo *
ExecBuildUpdateProjection(List *targetList,
						  bool evalTargetList,
						  List *targetColnos,
						  TupleDesc relDesc,
						  ExprContext *econtext,
						  TupleTableSlot *slot,
						  PlanState *parent)
{
	ProjectionInfo *projInfo = makeNode(ProjectionInfo);
	ExprState  *state;
	int			nAssignableCols;
	bool		sawJunk;
	Bitmapset  *assignedCols;
	ExprSetupInfo deform = {0, 0, 0, NIL};
	ExprEvalStep scratch = {0};
	int			outerattnum;
	ListCell   *lc,
			   *lc2;

	projInfo->pi_exprContext = econtext;
	/* 将 ExprState 内嵌进 ProjectionInfo，而不是额外 palloc      */
	projInfo->pi_state.type = T_ExprState;
	state = &projInfo->pi_state;
	if (evalTargetList)
		state->expr = (Expr *) targetList;
	else
		state->expr = NULL;		/* 未使用*/
	state->parent = parent;
	state->ext_params = NULL;

	state->resultslot = slot;

	/*
	 * 检查 targetList，看其中有多少个非垃圾列，并验证非  
	 * 垃圾列是否排在垃圾列之前。                       
	 */
	nAssignableCols = 0;
	sawJunk = false;
	foreach(lc, targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk)
			sawJunk = true;
		else
		{
			if (sawJunk)
				elog(ERROR, "subplan target list is out of order");
			nAssignableCols++;
		}
	}

	/* 每个非垃圾列应有一个 targetColnos 条目        */
	if (nAssignableCols != list_length(targetColnos))
		elog(ERROR, "targetColnos does not match subplan target list");

	/*
	 * 为 targetColnos 中的列构建一个 bitmapset。（其实可以   
	 * 直接用 list_member_int() 测试，但列多时有 O(N^2) 之  
	 * 患。）
	 */
	assignedCols = NULL;
	foreach(lc, targetColnos)
	{
		AttrNumber	targetattnum = lfirst_int(lc);

		assignedCols = bms_add_member(assignedCols, targetattnum);
	}

	/*
	 * 我们需要插入 EEOP_*_FETCHSOME 步骤，以确保输入元组     
	 * 已被充分解构。扫描元组必须至少解构到所需的     
	 * 最后一个旧列。                       
	 */
	for (int attnum = relDesc->natts; attnum > 0; attnum--)
	{
		Form_pg_attribute attr = TupleDescAttr(relDesc, attnum - 1);

		if (attr->attisdropped)
			continue;
		if (bms_is_member(attnum, assignedCols))
			continue;
		deform.last_scan = attnum;
		break;
	}

	/*
	 * 若确实要求值 tlist，则将其输入需求也算入； 
	 * 否则只需获取                                                   
	 * "outer"元组的相应列数即可。  
	 */
	if (evalTargetList)
		expr_setup_walker((Node *) targetList, &deform);
	else
		deform.last_outer = nAssignableCols;

	ExecPushExprSetupSteps(state, &deform);

	/*
	 * 现在生成代码来求值 tlist 中可赋值的表达式，或从
	 * 外层元组中取它们，顺带验证它们将具有正确的      
	 * 数据类型。上面的检查确保 forboth() 恰好会遍历全部  
	 * 非垃圾列。注意我们不必费心求值剩余的             
	 * resjunk 列。                                  
	 */
	outerattnum = 0;
	forboth(lc, targetList, lc2, targetColnos)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		AttrNumber	targetattnum = lfirst_int(lc2);
		Form_pg_attribute attr;

		Assert(!tle->resjunk);

		/*
		 * 做与 ExecCheckPlanOutput() 相当的健全性检查。
		 */
		if (targetattnum <= 0 || targetattnum > relDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Query has too many columns.")));
		attr = TupleDescAttr(relDesc, targetattnum - 1);

		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Query provides a value for a dropped column at ordinal position %d.",
							   targetattnum)));
		if (exprType((Node *) tle->expr) != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
							   format_type_be(attr->atttypid),
							   targetattnum,
							   format_type_be(exprType((Node *) tle->expr)))));

		/* 好，生成执行赋值的代码。         */
		if (evalTargetList)
		{
			/*
			 * 我们必须求值该 TLE 的表达式并赋值。我们不必 
			 * 为"safe" Var 而像下述那样大费周折：  
			 * ExecBuildProjectionInfo 那样；这条路径相对少用，
			 * 为此而耗费代码似乎不值得。                
			 */
			ExecInitExprRec(tle->expr, state,
							&state->resvalue, &state->resnull);
			/* 此处也无需担心只读性问题。          */
			scratch.opcode = EEOP_ASSIGN_TMP;
			scratch.d.assign_tmp.resultnum = targetattnum - 1;
			ExprEvalPushStep(state, &scratch);
		}
		else
		{
			/* 直接从外层元组赋值。    */
			scratch.opcode = EEOP_ASSIGN_OUTER_VAR;
			scratch.d.assign_var.attnum = outerattnum;
			scratch.d.assign_var.resultnum = targetattnum - 1;
			ExprEvalPushStep(state, &scratch);
		}
		outerattnum++;
	}

	/*
	 * 现在生成代码，复制所有未被赋值的旧列，并确保已
	 * 删除的列被置为 NULL。                          
	 */
	for (int attnum = 1; attnum <= relDesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(relDesc, attnum - 1);

		if (attr->attisdropped)
		{
			/* 把 null 放入 ExprState 的 resvalue/resnull ...   */
			scratch.opcode = EEOP_CONST;
			scratch.resvalue = &state->resvalue;
			scratch.resnull = &state->resnull;
			scratch.d.constval.value = (Datum) 0;
			scratch.d.constval.isnull = true;
			ExprEvalPushStep(state, &scratch);
			/* ...然后把它赋给结果槽        */
			scratch.opcode = EEOP_ASSIGN_TMP;
			scratch.d.assign_tmp.resultnum = attnum - 1;
			ExprEvalPushStep(state, &scratch);
		}
		else if (!bms_is_member(attnum, assignedCols))
		{
			/* 类型必然正确，因此无需检查    */
			scratch.opcode = EEOP_ASSIGN_SCAN_VAR;
			scratch.d.assign_var.attnum = attnum - 1;
			scratch.d.assign_var.resultnum = attnum - 1;
			ExprEvalPushStep(state, &scratch);
		}
	}

	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return projInfo;
}

/*
 * ExecPrepareExpr --- 在正常 Plan 树上下文之外初始化          
 * 表达式执行。
 *
 * 它与 ExecInitExpr 的不同在于：我们不假定调用方已经
 * 运行在 EState 的每查询上下文中。此外，我们将传入
 * 的表达式树经过 expression_planner() 处理，为执行做     
 * 准备。（在常规 Plan 树中，计划流程已对表达式           
 * 做适当的变换，但对独立表达式而言                       
 * 这不会发生。）                 
 */
ExprState *
ExecPrepareExpr(Expr *node, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	node = expression_planner(node);

	result = ExecInitExpr(node, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecPrepareQual --- 于正常 Plan 树上下文外初始          
 * 化 qual 执行。
 *
 * 它与 ExecInitQual 的不同在于：我们不假定调用方已经
 * 运行在 EState 的每查询上下文中。此外，我们将传入
 * 的表达式树经过 expression_planner() 处理，为执行做     
 * 准备。（在常规 Plan 树中，计划流程已对表达式           
 * 做适当的变换，但对独立表达式而言                       
 * 这不会发生。）                 
 */
ExprState *
ExecPrepareQual(List *qual, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	qual = (List *) expression_planner((Expr *) qual);

	result = ExecInitQual(qual, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecPrepareCheck -- 在正常 Plan 树上下文之外初始化         
 * 检查约束供执行。 
 *
 * 详情参见 ExecPrepareExpr() 与 ExecInitCheck()。 
 */
ExprState *
ExecPrepareCheck(List *qual, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	qual = (List *) expression_planner((Expr *) qual);

	result = ExecInitCheck(qual, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * 对 Expr 列表的每个成员调用 ExecPrepareExpr()，并返回  
 * ExprState 列表。  
 *
 * 详情参见 ExecPrepareExpr()。 
 */
List *
ExecPrepareExprList(List *nodes, EState *estate)
{
	List	   *result = NIL;
	MemoryContext oldcontext;
	ListCell   *lc;

	/* 确保 list cell 节点也处于正确的上下文中        */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	foreach(lc, nodes)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		result = lappend(result, ExecPrepareExpr(e, estate));
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecCheck - 求值一个检查约束   
 *
 * 对于检查约束，null 结果被视为 TRUE，即该约束           
 * 通过 
 *
 * 该检查约束可能经 ExecInitCheck 准备（或经        
 * ExecPrepareCheck）；若调用方持有的格式是隐式          
 * AND 格式，但用 ExecInitExpr 或 ExecPrepareExpr 准备的        
 * 常规表达式亦可。  
 */
bool
ExecCheck(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	/* 限制列表为空时短路（此处与 ExecInitCheck 中均是）   */
	if (state == NULL)
		return true;

	/* 验证表达式并非用 ExecInitQual 编译               */
	Assert(!(state->flags & EEO_FLAG_IS_QUAL));

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	if (isnull)
		return true;

	return DatumGetBool(ret);
}

/*
 * 为执行准备一个已编译的表达式。每个 ExprState 在可以
 * 执行之前都必须调用本函数。   
 *
 * 注意：虽然目前只调用 ExecReadyInterpretedExpr()，   
 * 但将来很可能扩展到更多表达式求值方法。因此应     
 * 使用本函数，而不是直接调用                  
 * ExecReadyInterpretedExpr().
 */
static void
ExecReadyExpr(ExprState *state)
{
	if (jit_compile_expr(state))
		return;

	ExecReadyInterpretedExpr(state);
}

/*
 * 把求值 node 所需的步骤追加到 ExprState->steps，               
 * 并可能递归进入 node 的子表达式。   
 *
 * node - 待求值的表达式 
 * state - 要把必要操作追加到其 ->steps 的 ExprState         
 * resv / resnull - node 结果的存放位置                
 */
static void
ExecInitExprRec(Expr *node, ExprState *state,
				Datum *resv, bool *resnull)
{
	ExprEvalStep scratch = {0};

	/* 防止因表达式过于复杂而导致栈溢出               */
	check_stack_depth();

	/* 步骤的输出位置始终是调用方给我们的位置*/
	Assert(resv != NULL && resnull != NULL);
	scratch.resvalue = resv;
	scratch.resnull = resnull;

	/* 各情形应按 enum NodeTag 中的顺序排列     */
	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *variable = (Var *) node;

				if (variable->varattno == InvalidAttrNumber)
				{
					/* 整行 Var    */
					ExecInitWholeRowVar(&scratch, variable, state);
				}
				else if (variable->varattno <= 0)
				{
					/* 系统列     */
					scratch.d.var.attnum = variable->varattno;
					scratch.d.var.vartype = variable->vartype;
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_SYSVAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_SYSVAR;
							break;

							/* INDEX_VAR 由默认分支处理      */

						default:
							scratch.opcode = EEOP_SCAN_SYSVAR;
							break;
					}
				}
				else
				{
					/* 常规用户列     */
					scratch.d.var.attnum = variable->varattno - 1;
					scratch.d.var.vartype = variable->vartype;
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_VAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_VAR;
							break;

							/* INDEX_VAR 由默认分支处理      */

						default:
							scratch.opcode = EEOP_SCAN_VAR;
							break;
					}
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_Const:
			{
				Const	   *con = (Const *) node;

				scratch.opcode = EEOP_CONST;
				scratch.d.constval.value = con->constvalue;
				scratch.d.constval.isnull = con->constisnull;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_Param:
			{
				Param	   *param = (Param *) node;
				ParamListInfo params;

				switch (param->paramkind)
				{
					case PARAM_EXEC:
						scratch.opcode = EEOP_PARAM_EXEC;
						scratch.d.param.paramid = param->paramid;
						scratch.d.param.paramtype = param->paramtype;
						ExprEvalPushStep(state, &scratch);
						break;
					case PARAM_EXTERN:

						/*
						 * 若有相关 ParamCompileHook，就用它；   
						 * 否则编译一个标准的 EEOP_PARAM_EXTERN 
						 * 步骤。ext_params 若有提供则优先于    
						 * 父节点 EState 信息（若存在）。        
						 */
						if (state->ext_params)
							params = state->ext_params;
						else if (state->parent &&
								 state->parent->state)
							params = state->parent->state->es_param_list_info;
						else
							params = NULL;
						if (params && params->paramCompile)
						{
							params->paramCompile(params, param, state,
												 resv, resnull);
						}
						else
						{
							scratch.opcode = EEOP_PARAM_EXTERN;
							scratch.d.param.paramid = param->paramid;
							scratch.d.param.paramtype = param->paramtype;
							ExprEvalPushStep(state, &scratch);
						}
						break;
					default:
						elog(ERROR, "unrecognized paramkind: %d",
							 (int) param->paramkind);
						break;
				}
				break;
			}

		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				scratch.opcode = EEOP_AGGREF;
				scratch.d.aggref.aggno = aggref->aggno;

				if (state->parent && IsA(state->parent, AggState))
				{
					AggState   *aggstate = (AggState *) state->parent;

					aggstate->aggs = lappend(aggstate->aggs, aggref);
				}
				else
				{
					/* 规划器搞砸了*/
					elog(ERROR, "Aggref found in non-Agg plan node");
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_GroupingFunc:
			{
				GroupingFunc *grp_node = (GroupingFunc *) node;
				Agg		   *agg;

				if (!state->parent || !IsA(state->parent, AggState) ||
					!IsA(state->parent->plan, Agg))
					elog(ERROR, "GroupingFunc found in non-Agg plan node");

				scratch.opcode = EEOP_GROUPING_FUNC;

				agg = (Agg *) (state->parent->plan);

				if (agg->groupingSets)
					scratch.d.grouping_func.clauses = grp_node->cols;
				else
					scratch.d.grouping_func.clauses = NIL;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFuncExprState *wfstate = makeNode(WindowFuncExprState);

				wfstate->wfunc = wfunc;

				if (state->parent && IsA(state->parent, WindowAggState))
				{
					WindowAggState *winstate = (WindowAggState *) state->parent;
					int			nfuncs;

					winstate->funcs = lappend(winstate->funcs, wfstate);
					nfuncs = ++winstate->numfuncs;
					if (wfunc->winagg)
						winstate->numaggs++;

					/* 目前先用旧式表达式来初始化聚合      */
					wfstate->args = ExecInitExprList(wfunc->args,
													 state->parent);
					wfstate->aggfilter = ExecInitExpr(wfunc->aggfilter,
													  state->parent);

					/*
					 * 若窗口函数参数含窗口函数则报错；  
					 * 嵌套的窗口函数在语义上不通。           
					 * （这种情况本应更早被发现，             
					 * 但我们仍在此处防御它。）  
					 */
					if (nfuncs != winstate->numfuncs)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("window function calls cannot be nested")));
				}
				else
				{
					/* 规划器搞砸了*/
					elog(ERROR, "WindowFunc found in non-WindowAgg plan node");
				}

				scratch.opcode = EEOP_WINDOW_FUNC;
				scratch.d.window_func.wfstate = wfstate;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				ExecInitSubscriptingRef(&scratch, sbsref, state, resv, resnull);
				break;
			}

		case T_FuncExpr:
			{
				FuncExpr   *func = (FuncExpr *) node;

				ExecInitFunc(&scratch, node,
							 func->args, func->funcid, func->inputcollid,
							 state);
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 state);
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_DistinctExpr:
			{
				DistinctExpr *op = (DistinctExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 state);

				/*
				 * 把调用指令的操作码改为 EEOP_DISTINCT。 
				 *
				 * XXX：历史上我们从未调用函数用量统计  
				 * pgstat 基础设施；鉴于我们对普通函数*和*    
				 * 操作符求值都会调用它，这显得不一致。若  
				 * 决定在此也这样做，可能需要单独的       
				 * 操作码区分 FUSAGE。 
				 */
				scratch.opcode = EEOP_DISTINCT;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_NullIfExpr:
			{
				NullIfExpr *op = (NullIfExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 state);

				/*
				 * 若第一个参数为 varlena 类型，我们需要确保 
				 * 传给比较函数的值是                          
				 * 只读指针。   
				 */
				scratch.d.func.make_ro =
					(get_typlen(exprType((Node *) linitial(op->args))) == -1);

				/*
				 * 把调用指令的操作码改为 EEOP_NULLIF。 
				 *
				 * XXX：历史上我们从未调用函数用量统计  
				 * pgstat 基础设施；鉴于我们对普通函数*和*    
				 * 操作符求值都会调用它，这显得不一致。若  
				 * 决定在此也这样做，可能需要单独的       
				 * 操作码区分 FUSAGE。 
				 */
				scratch.opcode = EEOP_NULLIF;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;
				Expr	   *scalararg;
				Expr	   *arrayarg;
				FmgrInfo   *finfo;
				FunctionCallInfo fcinfo;
				AclResult	aclresult;
				Oid			cmpfuncid;

				/*
				 * 选择正确的比较函数。当我们处理哈希       
				 * NOT IN 子句时，opfuncid 将是不等           
				 * 比较函数，而 negfuncid 被设为相等。           
				 * 哈希探测时需要使用相等函数。           
				 */
				if (OidIsValid(opexpr->negfuncid))
				{
					Assert(OidIsValid(opexpr->hashfuncid));
					cmpfuncid = opexpr->negfuncid;
				}
				else
					cmpfuncid = opexpr->opfuncid;

				Assert(list_length(opexpr->args) == 2);
				scalararg = (Expr *) linitial(opexpr->args);
				arrayarg = (Expr *) lsecond(opexpr->args);

				/* 检查调用函数的权限       */
				aclresult = object_aclcheck(ProcedureRelationId, cmpfuncid,
											GetUserId(),
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(cmpfuncid));
				InvokeFunctionExecuteHook(cmpfuncid);

				if (OidIsValid(opexpr->hashfuncid))
				{
					aclresult = object_aclcheck(ProcedureRelationId, opexpr->hashfuncid,
												GetUserId(),
												ACL_EXECUTE);
					if (aclresult != ACLCHECK_OK)
						aclcheck_error(aclresult, OBJECT_FUNCTION,
									   get_func_name(opexpr->hashfuncid));
					InvokeFunctionExecuteHook(opexpr->hashfuncid);
				}

				/* 设置主 fmgr 查找信息                */
				finfo = palloc0(sizeof(FmgrInfo));
				fcinfo = palloc0(SizeForFunctionCallInfo(2));
				fmgr_info(cmpfuncid, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 opexpr->inputcollid, NULL, NULL);

				/*
				 * 若已设置 hashfuncid，我们会创建                  
				 * EEOP_HASHED_SCALARARRAYOP 步骤而非                   
				 * EEOP_SCALARARRAYOP。这样当数组元素             
				 * 数量不算极小时，查找性能远超线性搜索。 
				 *       
				 */
				if (OidIsValid(opexpr->hashfuncid))
				{
					/* 把标量直接求值到左侧函数参数中        */
					ExecInitExprRec(scalararg, state,
									&fcinfo->args[0].value, &fcinfo->args[0].isnull);

					/*
					 * 把数组参数求值到我们的返回值中。       
					 * 没有危险，因为返回值必定会被    
					 * EEOP_HASHED_SCALARARRAYOP      
					 * 所覆盖；不会传给                            
					 * 其他表达式。   
					 */
					ExecInitExprRec(arrayarg, state, resv, resnull);

					/* 然后执行该操作     */
					scratch.opcode = EEOP_HASHED_SCALARARRAYOP;
					scratch.d.hashedscalararrayop.inclause = opexpr->useOr;
					scratch.d.hashedscalararrayop.finfo = finfo;
					scratch.d.hashedscalararrayop.fcinfo_data = fcinfo;
					scratch.d.hashedscalararrayop.saop = opexpr;


					ExprEvalPushStep(state, &scratch);
				}
				else
				{
					/* 把标量直接求值到左侧函数参数中        */
					ExecInitExprRec(scalararg, state,
									&fcinfo->args[0].value,
									&fcinfo->args[0].isnull);

					/*
					 * 把数组参数求值到我们的返回值中。       
					 * 没有危险，因为返回值必定会被    
					 * EEOP_SCALARARRAYOP 覆盖，且                        
					 * 不会传给任何其他表达式。       
					 */
					ExecInitExprRec(arrayarg, state, resv, resnull);

					/* 然后执行该操作     */
					scratch.opcode = EEOP_SCALARARRAYOP;
					scratch.d.scalararrayop.element_type = InvalidOid;
					scratch.d.scalararrayop.useOr = opexpr->useOr;
					scratch.d.scalararrayop.finfo = finfo;
					scratch.d.scalararrayop.fcinfo_data = fcinfo;
					scratch.d.scalararrayop.fn_addr = finfo->fn_addr;
					ExprEvalPushStep(state, &scratch);
				}
				break;
			}

		case T_BoolExpr:
			{
				BoolExpr   *boolexpr = (BoolExpr *) node;
				int			nargs = list_length(boolexpr->args);
				List	   *adjust_jumps = NIL;
				int			off;
				ListCell   *lc;

				/* 分配 AND/OR 所有步骤共用的临时内存     */
				if (boolexpr->boolop != NOT_EXPR)
					scratch.d.boolexpr.anynull = (bool *) palloc(sizeof(bool));

				/*
				 * 对每个参数先求值其本身，再执行       
				 * 布尔操作相应的处理。                    
				 *
				 * 可以把每个参数求值到结果区域中，因为   
				 * 短路逻辑意味着我们只需记住                  
				 * 先前的 NULL 值。
				 *
				 * AND/OR 被拆分为 STEP_FIRST（一个）/ STEP（零     
				 * 个或多个）/ STEP_LAST（一个）步骤，因为    
				 * 各自要做的工作不同。FIRST/LAST 拆分是    
				 * 有效，因 AND/OR 至少两参数。     
				 */
				off = 0;
				foreach(lc, boolexpr->args)
				{
					Expr	   *arg = (Expr *) lfirst(lc);

					/* 把参数求值到我们的输出变量中 */
					ExecInitExprRec(arg, state, resv, resnull);

					/* 执行相应的步骤类型       */
					switch (boolexpr->boolop)
					{
						case AND_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_AND_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_AND_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_AND_STEP;
							break;
						case OR_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_OR_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_OR_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_OR_STEP;
							break;
						case NOT_EXPR:
							Assert(nargs == 1);

							scratch.opcode = EEOP_BOOL_NOT_STEP;
							break;
						default:
							elog(ERROR, "unrecognized boolop: %d",
								 (int) boolexpr->boolop);
							break;
					}

					scratch.d.boolexpr.jumpdone = -1;
					ExprEvalPushStep(state, &scratch);
					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);
					off++;
				}

				/* 调整跳转目标  */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->d.boolexpr.jumpdone == -1);
					as->d.boolexpr.jumpdone = state->steps_len;
				}

				break;
			}

		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlanState *sstate;

				/*
				 * MULTIEXPR SubPlan 的实际执行早已完成。      
				 * 这里要做的只是返回一个哑 NULL record 值，    
				 * 以防该 targetlist 元素被赋值到          
				 * 某处。 
				 */
				if (subplan->subLinkType == MULTIEXPR_SUBLINK)
				{
					scratch.opcode = EEOP_CONST;
					scratch.d.constval.value = (Datum) 0;
					scratch.d.constval.isnull = true;
					ExprEvalPushStep(state, &scratch);
					break;
				}

				if (!state->parent)
					elog(ERROR, "SubPlan found with no parent plan");

				sstate = ExecInitSubPlan(subplan, state->parent);

				/* 将 SubPlanState 加进 state->parent->subPlan   */
				state->parent->subPlan = lappend(state->parent->subPlan,
												 sstate);

				scratch.opcode = EEOP_SUBPLAN;
				scratch.d.subplan.sstate = sstate;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;

				/* 把行/record 参数求值到结果区域中  */
				ExecInitExprRec(fselect->arg, state, resv, resnull);

				/* 然后提取字段*/
				scratch.opcode = EEOP_FIELDSELECT;
				scratch.d.fieldselect.fieldnum = fselect->fieldnum;
				scratch.d.fieldselect.resulttype = fselect->resulttype;
				scratch.d.fieldselect.rowcache.cacheptr = NULL;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				TupleDesc	tupDesc;
				ExprEvalRowtypeCache *rowcachep;
				Datum	   *values;
				bool	   *nulls;
				int			ncolumns;
				ListCell   *l1,
						   *l2;

				/* 查出复合类型的列数                          */
				tupDesc = lookup_rowtype_tupdesc(fstore->resulttype, -1);
				ncolumns = tupDesc->natts;
				ReleaseTupleDesc(tupDesc);

				/* 为列值创建工作区           */
				values = (Datum *) palloc(sizeof(Datum) * ncolumns);
				nulls = (bool *) palloc(sizeof(bool) * ncolumns);

				/* 创建共享的复合类型查找缓存结构    */
				rowcachep = palloc(sizeof(ExprEvalRowtypeCache));
				rowcachep->cacheptr = NULL;

				/* 生成求值复合输入值的代码            */
				ExecInitExprRec(fstore->arg, state, resv, resnull);

				/* 接着把输入元组解构到工作区中      */
				scratch.opcode = EEOP_FIELDSTORE_DEFORM;
				scratch.d.fieldstore.fstore = fstore;
				scratch.d.fieldstore.rowcache = rowcachep;
				scratch.d.fieldstore.values = values;
				scratch.d.fieldstore.nulls = nulls;
				scratch.d.fieldstore.ncolumns = ncolumns;
				ExprEvalPushStep(state, &scratch);

				/* 求值新的字段值，存入工作区列中         */
				forboth(l1, fstore->newvals, l2, fstore->fieldnums)
				{
					Expr	   *e = (Expr *) lfirst(l1);
					AttrNumber	fieldnum = lfirst_int(l2);
					Datum	   *save_innermost_caseval;
					bool	   *save_innermost_casenull;

					if (fieldnum <= 0 || fieldnum > ncolumns)
						elog(ERROR, "field number %d is out of range in FieldStore",
							 fieldnum);

					/*
					 * 借助 CaseTestExpr 传下被替换字段旧值    
					 * ；当新值本身是 FieldStore 或                
					 * SubscriptingRef 时就需用它获取并 
					 * 修改旧值。复用 CASE 机制是安全的，     
					 * 因为此处与需要值的位置之间不可能     
					 * 存在 CASE，且字段赋值也不可能位于 CASE   
					 * 之内。（因此保存并恢复                      
					 * innermost_caseval 只是出于谨慎， 
					 * 但我们还是这么做。）                     
					 *         
					 *
					 * 另一个不明显要点：字段的 values[]/nulls[] 
					 * 条目既用作该子表达式的 caseval 源、    
					 * 又用作其结果地址是安全的。              
					 * 没问题，是因为 (1) FieldStore 和        
					 * SubscriptingRef 都会先求值其 arg 或 refexpr  
					 * 输入，(2) 任何这类 CaseTestExpr 都直接是 
					 * arg/refexpr 输入，故对 caseval 的任何读取   
					 * 都发生在覆盖它之前。另外，              
					 * 若 newvals/fieldnums 有多个条目             
					 * 指向同一字段，实际上会按从左到右     
					 * 顺序生效，正是所需。      
					 */
					save_innermost_caseval = state->innermost_caseval;
					save_innermost_casenull = state->innermost_casenull;
					state->innermost_caseval = &values[fieldnum - 1];
					state->innermost_casenull = &nulls[fieldnum - 1];

					ExecInitExprRec(e, state,
									&values[fieldnum - 1],
									&nulls[fieldnum - 1]);

					state->innermost_caseval = save_innermost_caseval;
					state->innermost_casenull = save_innermost_casenull;
				}

				/* 最后组成结果元组   */
				scratch.opcode = EEOP_FIELDSTORE_FORM;
				scratch.d.fieldstore.fstore = fstore;
				scratch.d.fieldstore.rowcache = rowcachep;
				scratch.d.fieldstore.values = values;
				scratch.d.fieldstore.nulls = nulls;
				scratch.d.fieldstore.ncolumns = ncolumns;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_RelabelType:
			{
				/* relabel 在运行时无需做任何事         */
				RelabelType *relabel = (RelabelType *) node;

				ExecInitExprRec(relabel->arg, state, resv, resnull);
				break;
			}

		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				Oid			iofunc;
				bool		typisvarlena;
				Oid			typioparam;
				FunctionCallInfo fcinfo_in;

				/* 把参数求值到步骤的结果区域中*/
				ExecInitExprRec(iocoerce->arg, state, resv, resnull);

				/*
				 * 同时准备输出与输入函数调用，以便   
				 * 在单个求值步骤内完成求值以提速——       
				 * 这是极常见的操作。    
				 *
				 * 此处不检查权限，因为类型的输入/输出    
				 * 函数被假定人人可执行。                 
				 */
				scratch.opcode = EEOP_IOCOERCE;

				/* 查找源类型的输出函数           */
				scratch.d.iocoerce.finfo_out = palloc0(sizeof(FmgrInfo));
				scratch.d.iocoerce.fcinfo_data_out = palloc0(SizeForFunctionCallInfo(1));

				getTypeOutputInfo(exprType((Node *) iocoerce->arg),
								  &iofunc, &typisvarlena);
				fmgr_info(iofunc, scratch.d.iocoerce.finfo_out);
				fmgr_info_set_expr((Node *) node, scratch.d.iocoerce.finfo_out);
				InitFunctionCallInfoData(*scratch.d.iocoerce.fcinfo_data_out,
										 scratch.d.iocoerce.finfo_out,
										 1, InvalidOid, NULL, NULL);

				/* 查找结果类型的输入函数       */
				scratch.d.iocoerce.finfo_in = palloc0(sizeof(FmgrInfo));
				scratch.d.iocoerce.fcinfo_data_in = palloc0(SizeForFunctionCallInfo(3));

				getTypeInputInfo(iocoerce->resulttype,
								 &iofunc, &typioparam);
				fmgr_info(iofunc, scratch.d.iocoerce.finfo_in);
				fmgr_info_set_expr((Node *) node, scratch.d.iocoerce.finfo_in);
				InitFunctionCallInfoData(*scratch.d.iocoerce.fcinfo_data_in,
										 scratch.d.iocoerce.finfo_in,
										 3, InvalidOid, NULL, NULL);

				/*
				 * 输入函数的第二、第三个参数可以预加载，  
				 * 因为它们是常量。          
				 */
				fcinfo_in = scratch.d.iocoerce.fcinfo_data_in;
				fcinfo_in->args[1].value = ObjectIdGetDatum(typioparam);
				fcinfo_in->args[1].isnull = false;
				fcinfo_in->args[2].value = Int32GetDatum(-1);
				fcinfo_in->args[2].isnull = false;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				Oid			resultelemtype;
				ExprState  *elemstate;

				/* 把参数求值到步骤的结果区域中*/
				ExecInitExprRec(acoerce->arg, state, resv, resnull);

				resultelemtype = get_element_type(acoerce->resulttype);
				if (!OidIsValid(resultelemtype))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("target type is not an array")));

				/*
				 * 为逐元素表达式构造一个子表达式；          
				 * 但在检查其平凡性之后再 ready 它。            
				 * 我们假定它不含 Var 引用，但含有一个     
				 * 代表源数组元素值的 CaseTestExpr。               
				 */
				elemstate = makeNode(ExprState);
				elemstate->expr = acoerce->elemexpr;
				elemstate->parent = state->parent;
				elemstate->ext_params = state->ext_params;

				elemstate->innermost_caseval = (Datum *) palloc(sizeof(Datum));
				elemstate->innermost_casenull = (bool *) palloc(sizeof(bool));

				ExecInitExprRec(acoerce->elemexpr, elemstate,
								&elemstate->resvalue, &elemstate->resnull);

				if (elemstate->steps_len == 1 &&
					elemstate->steps[0].opcode == EEOP_CASE_TESTVAL)
				{
					/* 平凡，故运行时无需任何逐元素处理   */
					elemstate = NULL;
				}
				else
				{
					/* 非平凡，故追加 DONE 步骤  */
					scratch.opcode = EEOP_DONE;
					ExprEvalPushStep(elemstate, &scratch);
					/* 然后 ready 子表达式   */
					ExecReadyExpr(elemstate);
				}

				scratch.opcode = EEOP_ARRAYCOERCE;
				scratch.d.arraycoerce.elemexprstate = elemstate;
				scratch.d.arraycoerce.resultelemtype = resultelemtype;

				if (elemstate)
				{
					/* 为 array_map 设置工作区  */
					scratch.d.arraycoerce.amstate =
						(ArrayMapState *) palloc0(sizeof(ArrayMapState));
				}
				else
				{
					/* 若无子表达式则不需要工作区          */
					scratch.d.arraycoerce.amstate = NULL;
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;
				ExprEvalRowtypeCache *rowcachep;

				/* 出于空间考虑，缓存结构必须放在行外 */
				rowcachep = palloc(2 * sizeof(ExprEvalRowtypeCache));
				rowcachep[0].cacheptr = NULL;
				rowcachep[1].cacheptr = NULL;

				/* 把参数求值到步骤的结果区域中*/
				ExecInitExprRec(convert->arg, state, resv, resnull);

				/* 然后压入转换步骤 */
				scratch.opcode = EEOP_CONVERT_ROWTYPE;
				scratch.d.convert_rowtype.inputtype =
					exprType((Node *) convert->arg);
				scratch.d.convert_rowtype.outputtype = convert->resulttype;
				scratch.d.convert_rowtype.incache = &rowcachep[0];
				scratch.d.convert_rowtype.outcache = &rowcachep[1];
				scratch.d.convert_rowtype.map = NULL;

				ExprEvalPushStep(state, &scratch);
				break;
			}

			/* 注意 CaseWhen 表达式在本块内处理                  */
		case T_CaseExpr:
			{
				CaseExpr   *caseExpr = (CaseExpr *) node;
				List	   *adjust_jumps = NIL;
				Datum	   *caseval = NULL;
				bool	   *casenull = NULL;
				ListCell   *lc;

				/*
				 * 若存在测试表达式，须求值它，并把值     
				 * 存进 CaseTestExpr 占位符可找到的位置            
				 *    
				 */
				if (caseExpr->arg != NULL)
				{
					/* 把 testexpr 求值到 caseval/casenull 工作区 */
					caseval = palloc(sizeof(Datum));
					casenull = palloc(sizeof(bool));

					ExecInitExprRec(caseExpr->arg, state,
									caseval, casenull);

					/*
					 * 由于该值可能被读取多次，须强制为 R/O  
					 * ——但仅当可能是展开 datum 时。 
					 */
					if (get_typlen(exprType((Node *) caseExpr->arg)) == -1)
					{
						/* 就地修改 caseval    */
						scratch.opcode = EEOP_MAKE_READONLY;
						scratch.resvalue = caseval;
						scratch.resnull = casenull;
						scratch.d.make_readonly.value = caseval;
						scratch.d.make_readonly.isnull = casenull;
						ExprEvalPushStep(state, &scratch);
						/* 恢复临时字段的常规设置         */
						scratch.resvalue = resv;
						scratch.resnull = resnull;
					}
				}

				/*
				 * 准备依次求值各个 WHEN 子句；一旦某个     
				 * 条件为真，就返回相应 THEN 子句    
				 * 的值。若都不为真，则返回 ELSE 子句           
				 * 的值，若没有 ELSE 则返回 NULL。              
				 */
				foreach(lc, caseExpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(lc);
					Datum	   *save_innermost_caseval;
					bool	   *save_innermost_casenull;
					int			whenstep;

					/*
					 * 使 testexpr 的结果可为条件内的            
					 * CaseTestExpr 节点使用。须保存并恢复       
					 * innermost_caseval 先前的设置，以防本节点   
					 * 位于更大的 CASE 之内。 
					 *
					 * 若没有测试表达式，实际上并不需要     
					 * 保存和恢复这些字段；但无条件地          
					 * 这样做更省代码。   
					 */
					save_innermost_caseval = state->innermost_caseval;
					save_innermost_casenull = state->innermost_casenull;
					state->innermost_caseval = caseval;
					state->innermost_casenull = casenull;

					/* 把条件求值到 CASE 的结果变量中      */
					ExecInitExprRec(when->expr, state, resv, resnull);

					state->innermost_caseval = save_innermost_caseval;
					state->innermost_casenull = save_innermost_casenull;

					/* 若 WHEN 结果非真，跳到下一 CASE 分支 */
					scratch.opcode = EEOP_JUMP_IF_NOT_TRUE;
					scratch.d.jump.jumpdone = -1;	/* 稍后计算   */
					ExprEvalPushStep(state, &scratch);
					whenstep = state->steps_len - 1;

					/*
					 * 若 WHEN 结果为真，则求值 THEN 结果，     
					 * 存入 CASE 的结果变量中。   
					 */
					ExecInitExprRec(when->result, state, resv, resnull);

					/* 生成 JUMP 步骤以跳到 CASE 代码末尾*/
					scratch.opcode = EEOP_JUMP;
					scratch.d.jump.jumpdone = -1;	/* 稍后计算   */
					ExprEvalPushStep(state, &scratch);

					/*
					 * 还不知道该跳转的地址，待整个 CASE       
					 * 表达式建成后再计算。 
					 */
					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);

					/*
					 * 但 WHEN 测试的跳转目标现在就能设置，   
					 * 跳到下一个 WHEN 子表达式或 ELSE。    
					 */
					state->steps[whenstep].d.jump.jumpdone = state->steps_len;
				}

				/* transformCaseExpr 总会添加默认值 */
				Assert(caseExpr->defresult);

				/* 把 ELSE 表达式求值到 CASE 结果变量中*/
				ExecInitExprRec(caseExpr->defresult, state,
								resv, resnull);

				/* 调整跳转目标  */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->opcode == EEOP_JUMP);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = state->steps_len;
				}

				break;
			}

		case T_CaseTestExpr:
			{
				/*
				 * 从 innermost_caseval 标识的位置读取。注意，  
				 * 若本节点实际上不在 CaseExpr、ArrayCoerceExpr  
				 * 之类的结构之内，innermost_caseval 可能为 NULL。
				 * 之所以可能，是因为系统某些部分会      
				 * 滥用 CaseTestExpr 来读取外部经                     
				 * econtext->caseValue_datum 提供的值。             
				 * 运行时处理。  
				 */
				scratch.opcode = EEOP_CASE_TESTVAL;
				scratch.d.casetest.value = state->innermost_caseval;
				scratch.d.casetest.isnull = state->innermost_casenull;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				int			nelems = list_length(arrayexpr->elements);
				ListCell   *lc;
				int			elemoff;

				/*
				 * 求值方式是先计算每个元素，然后           
				 * 组成数组。各元素被计算到             
				 * ARRAYEXPR 步骤临时数组里。 
				 */
				scratch.opcode = EEOP_ARRAYEXPR;
				scratch.d.arrayexpr.elemvalues =
					(Datum *) palloc(sizeof(Datum) * nelems);
				scratch.d.arrayexpr.elemnulls =
					(bool *) palloc(sizeof(bool) * nelems);
				scratch.d.arrayexpr.nelems = nelems;

				/* 填充 step 的其余字段   */
				scratch.d.arrayexpr.multidims = arrayexpr->multidims;
				scratch.d.arrayexpr.elemtype = arrayexpr->element_typeid;

				/* 对类型信息做一次性查找        */
				get_typlenbyvalalign(arrayexpr->element_typeid,
									 &scratch.d.arrayexpr.elemlength,
									 &scratch.d.arrayexpr.elembyval,
									 &scratch.d.arrayexpr.elemalign);

				/* 准备求值所有参数          */
				elemoff = 0;
				foreach(lc, arrayexpr->elements)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					ExecInitExprRec(e, state,
									&scratch.d.arrayexpr.elemvalues[elemoff],
									&scratch.d.arrayexpr.elemnulls[elemoff]);
					elemoff++;
				}

				/* 然后把它们都收集进数组  */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				int			nelems = list_length(rowexpr->args);
				TupleDesc	tupdesc;
				int			i;
				ListCell   *l;

				/* 构建描述结果元组的 tupdesc     */
				if (rowexpr->row_typeid == RECORDOID)
				{
					/* 通用 record：使用给定表达式的类型 */
					tupdesc = ExecTypeFromExprList(rowexpr->args);
					/* ...但采用 RowExpr 的列别名      */
					ExecTypeSetColNames(tupdesc, rowexpr->colnames);
					/* Bless 该 tupdesc，以便日后可查         */
					BlessTupleDesc(tupdesc);
				}
				else
				{
					/* 已被 cast 为具名类型，使用它  */
					tupdesc = lookup_rowtype_tupdesc_copy(rowexpr->row_typeid, -1);
				}

				/*
				 * 在具名类型的情形下，tupdesc 的列数可能多于
				 * args 列表中的列数，因为该类型自 ROW() 被  
				 * 解析以来可能加过列。希望这些额外列     
				 * 取值 NULL，因此确保工作区数组足够大， 
				 * 然后把各额外列初始化，                        
				 * 使其读取 NULL。           
				 */
				Assert(nelems <= tupdesc->natts);
				nelems = Max(nelems, tupdesc->natts);

				/*
				 * 求值方式是先为每个字段构建 datum，然后    
				 * 由最后一步组成复合 datum。     
				 */
				scratch.opcode = EEOP_ROW;
				scratch.d.row.tupdesc = tupdesc;

				/* 供各字段 datum 使用的空间    */
				scratch.d.row.elemvalues =
					(Datum *) palloc(sizeof(Datum) * nelems);
				scratch.d.row.elemnulls =
					(bool *) palloc(sizeof(bool) * nelems);
				/* 如上所述，确保任何额外的列均为 NULL       */
				memset(scratch.d.row.elemnulls, true, sizeof(bool) * nelems);

				/* 设置求值，跳过任何已删除的列      */
				i = 0;
				foreach(l, rowexpr->args)
				{
					Form_pg_attribute att = TupleDescAttr(tupdesc, i);
					Expr	   *e = (Expr *) lfirst(l);

					if (!att->attisdropped)
					{
						/*
						 * 防范自 RowExpr 创建以来 rowtype 上发生
						 * ALTER COLUMN TYPE。XXX 是否也应检查   
						 * typmod？不确定能否保证它相同。      
						 *      
						 */
						if (exprType((Node *) e) != att->atttypid)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("ROW() column has type %s instead of type %s",
											format_type_be(exprType((Node *) e)),
											format_type_be(att->atttypid))));
					}
					else
					{
						/*
						 * 忽略原表达式并插入一个 NULL。我们 
						 * 并不在意 NULL 是什么类型，所以   
						 * 总是生成 int4 NULL。
						 */
						e = (Expr *) makeNullConst(INT4OID, -1, InvalidOid);
					}

					/* 把列表达式求值到相应的工作区槽中     */
					ExecInitExprRec(e, state,
									&scratch.d.row.elemvalues[i],
									&scratch.d.row.elemnulls[i]);
					i++;
				}

				/* 最后再构建行值           */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				int			nopers = list_length(rcexpr->opnos);
				List	   *adjust_jumps = NIL;
				ListCell   *l_left_expr,
						   *l_right_expr,
						   *l_opno,
						   *l_opfamily,
						   *l_inputcollid;
				ListCell   *lc;

				/*
				 * 遍历每个字段，准备比较操作。为处理     
				 * NULL 结果，准备好跳转到表达式之后。        
				 * 若比较结果 != 0，则跳转到最后一步。      
				 */
				Assert(list_length(rcexpr->largs) == nopers);
				Assert(list_length(rcexpr->rargs) == nopers);
				Assert(list_length(rcexpr->opfamilies) == nopers);
				Assert(list_length(rcexpr->inputcollids) == nopers);

				forfive(l_left_expr, rcexpr->largs,
						l_right_expr, rcexpr->rargs,
						l_opno, rcexpr->opnos,
						l_opfamily, rcexpr->opfamilies,
						l_inputcollid, rcexpr->inputcollids)
				{
					Expr	   *left_expr = (Expr *) lfirst(l_left_expr);
					Expr	   *right_expr = (Expr *) lfirst(l_right_expr);
					Oid			opno = lfirst_oid(l_opno);
					Oid			opfamily = lfirst_oid(l_opfamily);
					Oid			inputcollid = lfirst_oid(l_inputcollid);
					int			strategy;
					Oid			lefttype;
					Oid			righttype;
					Oid			proc;
					FmgrInfo   *finfo;
					FunctionCallInfo fcinfo;

					get_op_opfamily_properties(opno, opfamily, false,
											   &strategy,
											   &lefttype,
											   &righttype);
					proc = get_opfamily_proc(opfamily,
											 lefttype,
											 righttype,
											 BTORDER_PROC);
					if (!OidIsValid(proc))
						elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
							 BTORDER_PROC, lefttype, righttype, opfamily);

					/* 设置主要的 fmgr 查找信息          */
					finfo = palloc0(sizeof(FmgrInfo));
					fcinfo = palloc0(SizeForFunctionCallInfo(2));
					fmgr_info(proc, finfo);
					fmgr_info_set_expr((Node *) node, finfo);
					InitFunctionCallInfoData(*fcinfo, finfo, 2,
											 inputcollid, NULL, NULL);

					/*
					 * 如果我们对索引支持函数强制执行     
					 * 权限检查，就得在此处做检查。但      
					 * 索引支持机制并不这么做，因此       
					 * 本代码亦然。     
					 */

					/* 把左右两个参数直接求值进 fcinfo       */
					ExecInitExprRec(left_expr, state,
									&fcinfo->args[0].value, &fcinfo->args[0].isnull);
					ExecInitExprRec(right_expr, state,
									&fcinfo->args[1].value, &fcinfo->args[1].isnull);

					scratch.opcode = EEOP_ROWCOMPARE_STEP;
					scratch.d.rowcompare_step.finfo = finfo;
					scratch.d.rowcompare_step.fcinfo_data = fcinfo;
					scratch.d.rowcompare_step.fn_addr = finfo->fn_addr;
					/* 跳转目标稍后填充  */
					scratch.d.rowcompare_step.jumpnull = -1;
					scratch.d.rowcompare_step.jumpdone = -1;

					ExprEvalPushStep(state, &scratch);
					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);
				}

				/*
				 * 可能存在零列的行类型，此时两行              
				 * 必然彼此相等。     
				 */
				if (nopers == 0)
				{
					scratch.opcode = EEOP_CONST;
					scratch.d.constval.value = Int32GetDatum(0);
					scratch.d.constval.isnull = false;
					ExprEvalPushStep(state, &scratch);
				}

				/* 最后检查最后一次比较的结果     */
				scratch.opcode = EEOP_ROWCOMPARE_FINAL;
				scratch.d.rowcompare_final.rctype = rcexpr->rctype;
				ExprEvalPushStep(state, &scratch);

				/* 调整跳转目标  */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->opcode == EEOP_ROWCOMPARE_STEP);
					Assert(as->d.rowcompare_step.jumpdone == -1);
					Assert(as->d.rowcompare_step.jumpnull == -1);

					/* 跳转到比较求值处      */
					as->d.rowcompare_step.jumpdone = state->steps_len - 1;
					/* 跳转到后面的表达式      */
					as->d.rowcompare_step.jumpnull = state->steps_len;
				}

				break;
			}

		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesce = (CoalesceExpr *) node;
				List	   *adjust_jumps = NIL;
				ListCell   *lc;

				/* 我们假定至少有一个参数  */
				Assert(coalesce->args != NIL);

				/*
				 * 准备好对所有合并参数的求值，每处理完   
				 * 一个就压入非 NULL 即短路的步骤。   
				 */
				foreach(lc, coalesce->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					/* 把参数直接求值到结果 datum 中      */
					ExecInitExprRec(e, state, resv, resnull);

					/* 若非 NULL，跳到 COALESCE 表达式末尾  */
					scratch.opcode = EEOP_JUMP_IF_NOT_NULL;
					scratch.d.jump.jumpdone = -1;	/* 稍后调整 */
					ExprEvalPushStep(state, &scratch);

					adjust_jumps = lappend_int(adjust_jumps,
											   state->steps_len - 1);
				}

				/*
				 * 无需添加返回常量 NULL 的步骤——只有当    
				 * 已经在返回 NULL 时，才可能到达表达式 
				 * 末尾。
				 */

				/* 调整跳转目标  */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = &state->steps[lfirst_int(lc)];

					Assert(as->opcode == EEOP_JUMP_IF_NOT_NULL);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = state->steps_len;
				}

				break;
			}

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				int			nelems = list_length(minmaxexpr->args);
				TypeCacheEntry *typentry;
				FmgrInfo   *finfo;
				FunctionCallInfo fcinfo;
				ListCell   *lc;
				int			off;

				/* 查找该数据类型对应的 btree 比较函数      */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (!OidIsValid(typentry->cmp_proc))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("could not identify a comparison function for type %s",
									format_type_be(minmaxexpr->minmaxtype))));

				/*
				 * 如果我们对索引支持函数强制执行     
				 * 权限检查，就得在此处做检查。但索引      
				 * 支持机制并不这么做，因此本代码           
				 * 亦然。 
				 */

				/* 执行函数查找      */
				finfo = palloc0(sizeof(FmgrInfo));
				fcinfo = palloc0(SizeForFunctionCallInfo(2));
				fmgr_info(typentry->cmp_proc, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 minmaxexpr->inputcollid, NULL, NULL);

				scratch.opcode = EEOP_MINMAX;
				/* 分配存储参数的空间       */
				scratch.d.minmax.values =
					(Datum *) palloc(sizeof(Datum) * nelems);
				scratch.d.minmax.nulls =
					(bool *) palloc(sizeof(bool) * nelems);
				scratch.d.minmax.nelems = nelems;

				scratch.d.minmax.op = minmaxexpr->op;
				scratch.d.minmax.finfo = finfo;
				scratch.d.minmax.fcinfo_data = fcinfo;

				/* 把表达式求值到 minmax->values/nulls 中 */
				off = 0;
				foreach(lc, minmaxexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					ExecInitExprRec(e, state,
									&scratch.d.minmax.values[off],
									&scratch.d.minmax.nulls[off]);
					off++;
				}

				/* 再压入最后的比较      */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				scratch.opcode = EEOP_SQLVALUEFUNCTION;
				scratch.d.sqlvaluefunction.svf = svf;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				int			nnamed = list_length(xexpr->named_args);
				int			nargs = list_length(xexpr->args);
				int			off;
				ListCell   *arg;

				scratch.opcode = EEOP_XMLEXPR;
				scratch.d.xmlexpr.xexpr = xexpr;

				/* 分配存储所有参数的空间            */
				if (nnamed)
				{
					scratch.d.xmlexpr.named_argvalue =
						(Datum *) palloc(sizeof(Datum) * nnamed);
					scratch.d.xmlexpr.named_argnull =
						(bool *) palloc(sizeof(bool) * nnamed);
				}
				else
				{
					scratch.d.xmlexpr.named_argvalue = NULL;
					scratch.d.xmlexpr.named_argnull = NULL;
				}

				if (nargs)
				{
					scratch.d.xmlexpr.argvalue =
						(Datum *) palloc(sizeof(Datum) * nargs);
					scratch.d.xmlexpr.argnull =
						(bool *) palloc(sizeof(bool) * nargs);
				}
				else
				{
					scratch.d.xmlexpr.argvalue = NULL;
					scratch.d.xmlexpr.argnull = NULL;
				}

				/* 准备参数的执行      */
				off = 0;
				foreach(arg, xexpr->named_args)
				{
					Expr	   *e = (Expr *) lfirst(arg);

					ExecInitExprRec(e, state,
									&scratch.d.xmlexpr.named_argvalue[off],
									&scratch.d.xmlexpr.named_argnull[off]);
					off++;
				}

				off = 0;
				foreach(arg, xexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(arg);

					ExecInitExprRec(e, state,
									&scratch.d.xmlexpr.argvalue[off],
									&scratch.d.xmlexpr.argnull[off]);
					off++;
				}

				/* 再求值实际的 XML 表达式       */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				Assert(jve->raw_expr != NULL);
				ExecInitExprRec(jve->raw_expr, state, resv, resnull);
				Assert(jve->formatted_expr != NULL);
				ExecInitExprRec(jve->formatted_expr, state, resv, resnull);
				break;
			}

		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;
				List	   *args = ctor->args;
				ListCell   *lc;
				int			nargs = list_length(args);
				int			argno = 0;

				if (ctor->func)
				{
					ExecInitExprRec(ctor->func, state, resv, resnull);
				}
				else
				{
					JsonConstructorExprState *jcstate;

					jcstate = palloc0(sizeof(JsonConstructorExprState));

					scratch.opcode = EEOP_JSON_CONSTRUCTOR;
					scratch.d.json_constructor.jcstate = jcstate;

					jcstate->constructor = ctor;
					jcstate->arg_values = (Datum *) palloc(sizeof(Datum) * nargs);
					jcstate->arg_nulls = (bool *) palloc(sizeof(bool) * nargs);
					jcstate->arg_types = (Oid *) palloc(sizeof(Oid) * nargs);
					jcstate->nargs = nargs;

					foreach(lc, args)
					{
						Expr	   *arg = (Expr *) lfirst(lc);

						jcstate->arg_types[argno] = exprType((Node *) arg);

						if (IsA(arg, Const))
						{
							/* 不要每轮都对常量参数求值       */
							Const	   *con = (Const *) arg;

							jcstate->arg_values[argno] = con->constvalue;
							jcstate->arg_nulls[argno] = con->constisnull;
						}
						else
						{
							ExecInitExprRec(arg, state,
											&jcstate->arg_values[argno],
											&jcstate->arg_nulls[argno]);
						}
						argno++;
					}

					ExprEvalPushStep(state, &scratch);
				}

				if (ctor->coercion)
				{
					Datum	   *innermost_caseval = state->innermost_caseval;
					bool	   *innermost_isnull = state->innermost_casenull;

					state->innermost_caseval = resv;
					state->innermost_casenull = resnull;

					ExecInitExprRec(ctor->coercion, state, resv, resnull);

					state->innermost_caseval = innermost_caseval;
					state->innermost_casenull = innermost_isnull;
				}
			}
			break;

		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;

				ExecInitExprRec((Expr *) pred->expr, state, resv, resnull);

				scratch.opcode = EEOP_IS_JSON;
				scratch.d.is_json.pred = pred;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (ntest->nulltesttype == IS_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNULL;
				}
				else if (ntest->nulltesttype == IS_NOT_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNOTNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
				}
				else
				{
					elog(ERROR, "unrecognized nulltesttype: %d",
						 (int) ntest->nulltesttype);
				}
				/* 以防是行测试，先初始化缓存  */
				scratch.d.nulltest_row.rowcache.cacheptr = NULL;

				/* 先把参数求值到结果变量中         */
				ExecInitExprRec(ntest->arg, state,
								resv, resnull);

				/* 再压入对该参数的测试      */
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				/*
				 * 把参数直接求值进结果 datum，这样做没问题 
				 * 因为 resv/resnull 绝不会在别处使用，而且     
				 * 会被下面的 EEOP_BOOLTEST_IS_* 步骤覆盖。      
				 *      
				 */
				ExecInitExprRec(btest->arg, state, resv, resnull);

				switch (btest->booltesttype)
				{
					case IS_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_TRUE;
						break;
					case IS_NOT_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_TRUE;
						break;
					case IS_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_FALSE;
						break;
					case IS_NOT_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_FALSE;
						break;
					case IS_UNKNOWN:
						/* 同标量 IS NULL 测试    */
						scratch.opcode = EEOP_NULLTEST_ISNULL;
						break;
					case IS_NOT_UNKNOWN:
						/* 同标量 IS NOT NULL 测试    */
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;

				ExecInitCoerceToDomain(&scratch, ctest, state,
									   resv, resnull);
				break;
			}

		case T_CoerceToDomainValue:
			{
				/*
				 * 从 innermost_domainval 标识的位置读取。注意，  
				 * 若编译的是独立的域检查而非嵌入更大       
				 * 表达式中的检查，则 innermost_domainval        
				 * 可能为 NULL。那种情况下就必须从      
				 * econtext->domainValue_datum 读取。我们会在    
				 * 运行时处理。  
				 */
				scratch.opcode = EEOP_DOMAIN_TESTVAL;
				/* 与 case 测试值共用指令联合体变体        */
				scratch.d.casetest.value = state->innermost_domainval;
				scratch.d.casetest.isnull = state->innermost_domainnull;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_CurrentOfExpr:
			{
				scratch.opcode = EEOP_CURRENTOFEXPR;
				ExprEvalPushStep(state, &scratch);
				break;
			}

		case T_NextValueExpr:
			{
				NextValueExpr *nve = (NextValueExpr *) node;

				scratch.opcode = EEOP_NEXTVALUEEXPR;
				scratch.d.nextvalueexpr.seqid = nve->seqid;
				scratch.d.nextvalueexpr.seqtypid = nve->typeId;

				ExprEvalPushStep(state, &scratch);
				break;
			}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * 向 ExprState->steps 添加一个表达式求值步骤。  
 *
 * 注意这可能会重新分配 es->steps，因此只要表达式       
 * 还在构建之中，就不得使用指向该数组的任何指针。 
 */
void
ExprEvalPushStep(ExprState *es, const ExprEvalStep *s)
{
	if (es->steps_alloc == 0)
	{
		es->steps_alloc = 16;
		es->steps = palloc(sizeof(ExprEvalStep) * es->steps_alloc);
	}
	else if (es->steps_alloc == es->steps_len)
	{
		es->steps_alloc *= 2;
		es->steps = repalloc(es->steps,
							 sizeof(ExprEvalStep) * es->steps_alloc);
	}

	memcpy(&es->steps[es->steps_len++], s, sizeof(ExprEvalStep));
}

/*
 * 为函数类表达式的求值做必要的准备工作，把参数求值 
 * 步骤附加到 *state 里的步骤列表，并把 *scratch         
 * 设置妥当，使其随时可以压入。      
 *
 * *scratch 不在此处压入，调用者可借此改写操作码，   
 * 这对 DISTINCT 之类的函数式场景很有用。  
 */
static void
ExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args, Oid funcid,
			 Oid inputcollid, ExprState *state)
{
	int			nargs = list_length(args);
	AclResult	aclresult;
	FmgrInfo   *flinfo;
	FunctionCallInfo fcinfo;
	int			argno;
	ListCell   *lc;

	/* 检查调用函数的权限       */
	aclresult = object_aclcheck(ProcedureRelationId, funcid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);

	/*
	 * 对 nargs 的安全性检查。正常情况下它绝不应失败， 
	 * 因为解析器应更早检查。但如果服务器编译时的    
	 * FUNC_MAX_ARGS 小于 pg_proc 中某些函数声明的值，            
	 * 还会失败吗？  
	 */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* 为此次调用分配函数查找数据与参数工作区           */
	scratch->d.func.finfo = palloc0(sizeof(FmgrInfo));
	scratch->d.func.fcinfo_data = palloc0(SizeForFunctionCallInfo(nargs));
	flinfo = scratch->d.func.finfo;
	fcinfo = scratch->d.func.fcinfo_data;

	/* 设置主要的 fmgr 查找信息          */
	fmgr_info(funcid, flinfo);
	fmgr_info_set_expr((Node *) node, flinfo);

	/* 同时初始化函数调用参数结构          */
	InitFunctionCallInfoData(*fcinfo, flinfo,
							 nargs, inputcollid, NULL, NULL);

	/* 多保留一份该信息，省去运行时的一次间接寻址  */
	scratch->d.func.fn_addr = flinfo->fn_addr;
	scratch->d.func.nargs = nargs;

	/* 此处只支持非集合返回函数   */
	if (flinfo->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 state->parent ?
				 executor_errposition(state->parent->state,
									  exprLocation((Node *) node)) : 0));

	/* 构建把参数直接求值到 fcinfo 结构中的代码         */
	argno = 0;
	foreach(lc, args)
	{
		Expr	   *arg = (Expr *) lfirst(lc);

		if (IsA(arg, Const))
		{
			/*
			 * 不要每一轮都对常量参数重新求值；这对
			 * 比较中的常量尤其有意义。     
			 */
			Const	   *con = (Const *) arg;

			fcinfo->args[argno].value = con->constvalue;
			fcinfo->args[argno].isnull = con->constisnull;
		}
		else
		{
			ExecInitExprRec(arg, state,
							&fcinfo->args[argno].value,
							&fcinfo->args[argno].isnull);
		}
		argno++;
	}

	/* 根据严格性与统计级别插入相应的操作码            */
	if (pgstat_track_functions <= flinfo->fn_stats)
	{
		if (flinfo->fn_strict && nargs > 0)
			scratch->opcode = EEOP_FUNCEXPR_STRICT;
		else
			scratch->opcode = EEOP_FUNCEXPR;
	}
	else
	{
		if (flinfo->fn_strict && nargs > 0)
			scratch->opcode = EEOP_FUNCEXPR_STRICT_FUSAGE;
		else
			scratch->opcode = EEOP_FUNCEXPR_FUSAGE;
	}
}

/*
 * 添加若干表达式步骤，完成表达式主体开始执行      
 * 之前所需的准备工作。   
 */
static void
ExecCreateExprSetupSteps(ExprState *state, Node *node)
{
	ExprSetupInfo info = {0, 0, 0, NIL};

	/* 预扫描，弄清需要什么。 */
	expr_setup_walker(node, &info);

	/* 再生成这些步骤。  */
	ExecPushExprSetupSteps(state, &info);
}

/*
 * 按 "info" 指示添加表达式准备工作的步骤。     
 * 在构建覆盖多个表达式的 ExprState 时，这一功能很有用。  
 */
static void
ExecPushExprSetupSteps(ExprState *state, ExprSetupInfo *info)
{
	ExprEvalStep scratch = {0};
	ListCell   *lc;

	scratch.resvalue = NULL;
	scratch.resnull = NULL;

	/*
	 * 添加步骤，把 ExprState 的 inner/outer/scan 槽解构          
	 * 到表达式中各 Var 所需的程度。        
	 */
	if (info->last_inner > 0)
	{
		scratch.opcode = EEOP_INNER_FETCHSOME;
		scratch.d.fetch.last_var = info->last_inner;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(state, &scratch))
			ExprEvalPushStep(state, &scratch);
	}
	if (info->last_outer > 0)
	{
		scratch.opcode = EEOP_OUTER_FETCHSOME;
		scratch.d.fetch.last_var = info->last_outer;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(state, &scratch))
			ExprEvalPushStep(state, &scratch);
	}
	if (info->last_scan > 0)
	{
		scratch.opcode = EEOP_SCAN_FETCHSOME;
		scratch.d.fetch.last_var = info->last_scan;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(state, &scratch))
			ExprEvalPushStep(state, &scratch);
	}

	/*
	 * 添加步骤，执行表达式中出现的所有            
	 * MULTIEXPR SubPlan。必须在引用其输出的                
	 * 任何 Param 被使用之前对其求值，但要在为其          
	 * 可能包含的 Var 引用做好准备之后。（                 
	 * MULTIEXPR SubPlan 间无交叉引用，无需担心顺序。）    
	 */
	foreach(lc, info->multiexpr_subplans)
	{
		SubPlan    *subplan = (SubPlan *) lfirst(lc);
		SubPlanState *sstate;

		Assert(subplan->subLinkType == MULTIEXPR_SUBLINK);

		/* 应与 ExecInitExprRec 对其他 SubPlan 的做法一致：     */

		if (!state->parent)
			elog(ERROR, "SubPlan found with no parent plan");

		sstate = ExecInitSubPlan(subplan, state->parent);

		/* SubPlanState 节点加入 state->parent->subPlan */
		state->parent->subPlan = lappend(state->parent->subPlan,
										 sstate);

		scratch.opcode = EEOP_SUBPLAN;
		scratch.d.subplan.sstate = sstate;

		/* 结果可忽略，但我们最好还是把它放在某处 */
		scratch.resvalue = &state->resvalue;
		scratch.resnull = &state->resnull;

		ExprEvalPushStep(state, &scratch);
	}
}

/*
 * expr_setup_walker: ExecCreateExprSetupSteps 的表达式遍历器
 */
static bool
expr_setup_walker(Node *node, ExprSetupInfo *info)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *variable = (Var *) node;
		AttrNumber	attnum = variable->varattno;

		switch (variable->varno)
		{
			case INNER_VAR:
				info->last_inner = Max(info->last_inner, attnum);
				break;

			case OUTER_VAR:
				info->last_outer = Max(info->last_outer, attnum);
				break;

				/* INDEX_VAR 由 default 分支处理   */

			default:
				info->last_scan = Max(info->last_scan, attnum);
				break;
		}
		return false;
	}

	/* 同时收集所有 MULTIEXPR SubPlan*/
	if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;

		if (subplan->subLinkType == MULTIEXPR_SUBLINK)
			info->multiexpr_subplans = lappend(info->multiexpr_subplans,
											   subplan);
	}

	/*
	 * 不要检查 Aggrefs 或 WindowFuncs 的参数或过滤器，    
	 * 因为它们并不代表要在调用表达式的                     
	 * econtext 中求值的表达式。GroupingFunc 的参数         
	 * 从不求值。  
	 */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, WindowFunc))
		return false;
	if (IsA(node, GroupingFunc))
		return false;
	return expression_tree_walker(node, expr_setup_walker,
								  (void *) info);
}

/*
 * 为 EEOP_*_FETCHSOME 操作计算附加信息。        
 *
 * 其目的是判断槽是否"固定"，也就是说，每一次    
 * 表达式求值遇到的槽都是同一类型，且带有等价的   
 * 描述符。          
 *
 * 若需要解构步骤则返回 true，否则返回 false。     
 */
static bool
ExecComputeSlotInfo(ExprState *state, ExprEvalStep *op)
{
	PlanState  *parent = state->parent;
	TupleDesc	desc = NULL;
	const TupleTableSlotOps *tts_ops = NULL;
	bool		isfixed = false;
	ExprEvalOp	opcode = op->opcode;

	Assert(opcode == EEOP_INNER_FETCHSOME ||
		   opcode == EEOP_OUTER_FETCHSOME ||
		   opcode == EEOP_SCAN_FETCHSOME);

	if (op->d.fetch.known_desc != NULL)
	{
		desc = op->d.fetch.known_desc;
		tts_ops = op->d.fetch.kind;
		isfixed = op->d.fetch.kind != NULL;
	}
	else if (!parent)
	{
		isfixed = false;
	}
	else if (opcode == EEOP_INNER_FETCHSOME)
	{
		PlanState  *is = innerPlanState(parent);

		if (parent->inneropsset && !parent->inneropsfixed)
		{
			isfixed = false;
		}
		else if (parent->inneropsset && parent->innerops)
		{
			isfixed = true;
			tts_ops = parent->innerops;
			desc = ExecGetResultType(is);
		}
		else if (is)
		{
			tts_ops = ExecGetResultSlotOps(is, &isfixed);
			desc = ExecGetResultType(is);
		}
	}
	else if (opcode == EEOP_OUTER_FETCHSOME)
	{
		PlanState  *os = outerPlanState(parent);

		if (parent->outeropsset && !parent->outeropsfixed)
		{
			isfixed = false;
		}
		else if (parent->outeropsset && parent->outerops)
		{
			isfixed = true;
			tts_ops = parent->outerops;
			desc = ExecGetResultType(os);
		}
		else if (os)
		{
			tts_ops = ExecGetResultSlotOps(os, &isfixed);
			desc = ExecGetResultType(os);
		}
	}
	else if (opcode == EEOP_SCAN_FETCHSOME)
	{
		desc = parent->scandesc;

		if (parent->scanops)
			tts_ops = parent->scanops;

		if (parent->scanopsset)
			isfixed = parent->scanopsfixed;
	}

	if (isfixed && desc != NULL && tts_ops != NULL)
	{
		op->d.fetch.fixed = true;
		op->d.fetch.kind = tts_ops;
		op->d.fetch.known_desc = desc;
	}
	else
	{
		op->d.fetch.fixed = false;
		op->d.fetch.kind = NULL;
		op->d.fetch.known_desc = NULL;
	}

	/* 若已知槽始终是虚拟槽，则永远无需解构         */
	if (op->d.fetch.fixed && op->d.fetch.kind == &TTSOpsVirtual)
		return false;

	return true;
}

/*
 * 准备好整行变量求值所需的步骤。           
 * 调用者仍需压入该步骤。     
 */
static void
ExecInitWholeRowVar(ExprEvalStep *scratch, Var *variable, ExprState *state)
{
	PlanState  *parent = state->parent;

	/* 除目标外全部填好   */
	scratch->opcode = EEOP_WHOLEROW;
	scratch->d.wholerow.var = variable;
	scratch->d.wholerow.first = true;
	scratch->d.wholerow.slow = false;
	scratch->d.wholerow.tupdesc = NULL; /* 运行时填充   */
	scratch->d.wholerow.junkFilter = NULL;

	/*
	 * 若输入元组来自子查询，其中可能含有 "resjunk"      
	 * 列（例如 GROUP BY 或 ORDER BY 列），我们不想把这类     
	 * 列保留在整行结果中。让元组通过一个 JunkFilter  
	 * 即可去掉这类列——但要构造过滤器，就必须先拿    
	 * 到子查询的目标列表。幸运的是，会发生这种情况的  
	 * 场景并不多，而且只需检查父 PlanState 就能把这些     
	 * 情形都识别出来。我们假定，在没有父计划的独立   
	 * 表达式中这不成问题。（整行 Var 也可能出现在这类 
	 * 表达式中，但它们引用的必然是表的                  
	 * 行。）   
	 */
	if (parent)
	{
		PlanState  *subplan = NULL;

		switch (nodeTag(parent))
		{
			case T_SubqueryScanState:
				subplan = ((SubqueryScanState *) parent)->subplan;
				break;
			case T_CteScanState:
				subplan = ((CteScanState *) parent)->cteplanstate;
				break;
			default:
				break;
		}

		if (subplan)
		{
			bool		junk_filter_needed = false;
			ListCell   *tlist;

			/* 检查子计划的 tlist 是否真含有 junk 列          */
			foreach(tlist, subplan->plan->targetlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlist);

				if (tle->resjunk)
				{
					junk_filter_needed = true;
					break;
				}
			}

			/* 若有，现在构建 junkfilter*/
			if (junk_filter_needed)
			{
				scratch->d.wholerow.junkFilter =
					ExecInitJunkFilter(subplan->plan->targetlist,
									   ExecInitExtraTupleSlot(parent->state, NULL,
															  &TTSOpsVirtual));
			}
		}
	}
}

/*
 * 准备好 SubscriptingRef 表达式的求值。    
 */
static void
ExecInitSubscriptingRef(ExprEvalStep *scratch, SubscriptingRef *sbsref,
						ExprState *state, Datum *resv, bool *resnull)
{
	bool		isAssignment = (sbsref->refassgnexpr != NULL);
	int			nupper = list_length(sbsref->refupperindexpr);
	int			nlower = list_length(sbsref->reflowerindexpr);
	const SubscriptRoutines *sbsroutines;
	SubscriptingRefState *sbsrefstate;
	SubscriptExecSteps methods;
	char	   *ptr;
	List	   *adjust_jumps = NIL;
	ListCell   *lc;
	int			i;

	/* 查找下标支持方法                 */
	sbsroutines = getSubscriptingRoutines(sbsref->refcontainertype, NULL);
	if (!sbsroutines)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot subscript type %s because it does not support subscripting",
						format_type_be(sbsref->refcontainertype)),
				 state->parent ?
				 executor_errposition(state->parent->state,
									  exprLocation((Node *) sbsref)) : 0));

	/* 分配 sbsrefstate，并为各下标数组预留足够空间         */
	sbsrefstate = palloc0(MAXALIGN(sizeof(SubscriptingRefState)) +
						  (nupper + nlower) * (sizeof(Datum) +
											   2 * sizeof(bool)));

	/* 填写 SubscriptingRefState 常量字段     */
	sbsrefstate->isassignment = isAssignment;
	sbsrefstate->numupper = nupper;
	sbsrefstate->numlower = nlower;
	/* 设置各下标数组       */
	ptr = ((char *) sbsrefstate) + MAXALIGN(sizeof(SubscriptingRefState));
	sbsrefstate->upperindex = (Datum *) ptr;
	ptr += nupper * sizeof(Datum);
	sbsrefstate->lowerindex = (Datum *) ptr;
	ptr += nlower * sizeof(Datum);
	sbsrefstate->upperprovided = (bool *) ptr;
	ptr += nupper * sizeof(bool);
	sbsrefstate->lowerprovided = (bool *) ptr;
	ptr += nlower * sizeof(bool);
	sbsrefstate->upperindexnull = (bool *) ptr;
	ptr += nupper * sizeof(bool);
	sbsrefstate->lowerindexnull = (bool *) ptr;
	/* ptr += nlower * sizeof(bool); */

	/*
	 * 让容器类型专属的代码参与进来。它必须在 "methods"  
	 * 结构中填入我们在下面执行步骤里                    
	 * 可能用到的函数指针；还可以选择性地设置            
	 * workspace 所指数据。 
	 */
	memset(&methods, 0, sizeof(methods));
	sbsroutines->exec_setup(sbsref, sbsrefstate, &methods);

	/*
	 * 对数组输入求值。将其求值到 resv/resnull 是安全的，   
	 * 因为我们不会把它用作其他任何子表达式的目标，     
	 * 而且它会被最后压入的 EEOP_SBSREF_FETCH/ASSIGN 步骤     
	 * 覆盖。   
	 */
	ExecInitExprRec(sbsref->refexpr, state, resv, resnull);

	/*
	 * 若 refexpr 产生 NULL，且操作应当是严格的，则结果      
	 * 为 NULL。只需 JUMP_IF_NULL 就能实现，因为我们      
	 * 已经把数组求值到了期望的目标位置。  
	 */
	if (!isAssignment && sbsroutines->fetch_strict)
	{
		scratch->opcode = EEOP_JUMP_IF_NULL;
		scratch->d.jump.jumpdone = -1;	/* 稍后调整 */
		ExprEvalPushStep(state, scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	/* 求值上界下标        */
	i = 0;
	foreach(lc, sbsref->refupperindexpr)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		/* 切片时，各个下标边界都可以省略            */
		if (!e)
		{
			sbsrefstate->upperprovided[i] = false;
			sbsrefstate->upperindexnull[i] = true;
		}
		else
		{
			sbsrefstate->upperprovided[i] = true;
			/* 每个下标都求值到相应的数组条目中         */
			ExecInitExprRec(e, state,
							&sbsrefstate->upperindex[i],
							&sbsrefstate->upperindexnull[i]);
		}
		i++;
	}

	/* 类似地求值下界下标         */
	i = 0;
	foreach(lc, sbsref->reflowerindexpr)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		/* 切片时，各个下标边界都可以省略            */
		if (!e)
		{
			sbsrefstate->lowerprovided[i] = false;
			sbsrefstate->lowerindexnull[i] = true;
		}
		else
		{
			sbsrefstate->lowerprovided[i] = true;
			/* 每个下标都会被求值到对应的数组条目中   */
			ExecInitExprRec(e, state,
							&sbsrefstate->lowerindex[i],
							&sbsrefstate->lowerindexnull[i]);
		}
		i++;
	}

	/* SBSREF_SUBSCRIPTS 一次性检查并转换所有下标           */
	if (methods.sbs_check_subscripts)
	{
		scratch->opcode = EEOP_SBSREF_SUBSCRIPTS;
		scratch->d.sbsref_subscript.subscriptfunc = methods.sbs_check_subscripts;
		scratch->d.sbsref_subscript.state = sbsrefstate;
		scratch->d.sbsref_subscript.jumpdone = -1;	/* 稍后调整 */
		ExprEvalPushStep(state, scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	if (isAssignment)
	{
		Datum	   *save_innermost_caseval;
		bool	   *save_innermost_casenull;

		/* 检查未实现的方法        */
		if (!methods.sbs_assign)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("type %s does not support subscripted assignment",
							format_type_be(sbsref->refcontainertype))));

		/*
		 * 我们可能碰到嵌套赋值的情形，其中的      
		 * refassgnexpr 本身是 FieldStore 或 SubscriptingRef，需要   
		 * 获取并修改被替换的数组元素或切片的原有值。
		 * 若是如此，就必须从数组中提取该值，经          
		 * CaseTestExpr 机制向下传递。可安全复用 CASE          
		 * 机制，因为从这点到用到该值处之间不会有      
		 * CASE，且数组赋值也不可能处于 CASE 之中。       
		 * （所以保存并恢复 innermost_caseval 只是保险起见，   
		 * 但我们还是会这么做。）                           
		 *
		 * 由于取出旧元素可能开销不小，因此只在参数确实 
		 * 需要该值时才执行。            
		 */
		if (isAssignmentIndirectionExpr(sbsref->refassgnexpr))
		{
			if (!methods.sbs_fetch_old)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("type %s does not support subscripted assignment",
								format_type_be(sbsref->refcontainertype))));
			scratch->opcode = EEOP_SBSREF_OLD;
			scratch->d.sbsref.subscriptfunc = methods.sbs_fetch_old;
			scratch->d.sbsref.state = sbsrefstate;
			ExprEvalPushStep(state, scratch);
		}

		/* SBSREF_OLD 把提取出的值放入 prevvalue/prevnull  */
		save_innermost_caseval = state->innermost_caseval;
		save_innermost_casenull = state->innermost_casenull;
		state->innermost_caseval = &sbsrefstate->prevvalue;
		state->innermost_casenull = &sbsrefstate->prevnull;

		/* 把替换值求值到 replacevalue/replacenull 中       */
		ExecInitExprRec(sbsref->refassgnexpr, state,
						&sbsrefstate->replacevalue, &sbsrefstate->replacenull);

		state->innermost_caseval = save_innermost_caseval;
		state->innermost_casenull = save_innermost_casenull;

		/* 然后执行赋值         */
		scratch->opcode = EEOP_SBSREF_ASSIGN;
		scratch->d.sbsref.subscriptfunc = methods.sbs_assign;
		scratch->d.sbsref.state = sbsrefstate;
		ExprEvalPushStep(state, scratch);
	}
	else
	{
		/* 数组读取则简单得多 */
		scratch->opcode = EEOP_SBSREF_FETCH;
		scratch->d.sbsref.subscriptfunc = methods.sbs_fetch;
		scratch->d.sbsref.state = sbsrefstate;
		ExprEvalPushStep(state, scratch);
	}

	/* 调整跳转目标  */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		if (as->opcode == EEOP_SBSREF_SUBSCRIPTS)
		{
			Assert(as->d.sbsref_subscript.jumpdone == -1);
			as->d.sbsref_subscript.jumpdone = state->steps_len;
		}
		else
		{
			Assert(as->opcode == EEOP_JUMP_IF_NULL);
			Assert(as->d.jump.jumpdone == -1);
			as->d.jump.jumpdone = state->steps_len;
		}
	}
}

/*
 * 辅助函数：为求值准备 SubscriptingRef 表达式；expr 是     
 * 一个嵌套的 FieldStore 或 SubscriptingRef，需要传递旧       
 * 元素值？
 *
 * （FieldStore 中本也可使用它，但那种情况下传递旧值  
 * 极为廉价，无此必要。）  
 *
 * 注意：看似这里需要递归，但多数情况下并不需要；    
 * 即便有 CaseTestExpr，它也会直接作为顶层节点的 arg 或    
 * refexpr。嵌套赋值情形产生的表达式树中，每层赋值  
 * 都有属于自己的 CaseTestExpr，而递归结构则出现在         
 * newvals 或 refassgnexpr 字段之中。                             
 * 但有一个例外：如果该数组是域上的数组，那么我们会  
 * 得到 CoerceToDomain 或 RelabelType 作为 refassgnexpr，此时      
 * 必须能够看穿它。     
 */
static bool
isAssignmentIndirectionExpr(Expr *expr)
{
	if (expr == NULL)
		return false;			/* 纯属谨慎  */
	if (IsA(expr, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) expr;

		if (fstore->arg && IsA(fstore->arg, CaseTestExpr))
			return true;
	}
	else if (IsA(expr, SubscriptingRef))
	{
		SubscriptingRef *sbsRef = (SubscriptingRef *) expr;

		if (sbsRef->refexpr && IsA(sbsRef->refexpr, CaseTestExpr))
			return true;
	}
	else if (IsA(expr, CoerceToDomain))
	{
		CoerceToDomain *cd = (CoerceToDomain *) expr;

		return isAssignmentIndirectionExpr(cd->arg);
	}
	else if (IsA(expr, RelabelType))
	{
		RelabelType *r = (RelabelType *) expr;

		return isAssignmentIndirectionExpr(r->arg);
	}
	return false;
}

/*
 * 准备对 CoerceToDomain 表达式求值。       
 */
static void
ExecInitCoerceToDomain(ExprEvalStep *scratch, CoerceToDomain *ctest,
					   ExprState *state, Datum *resv, bool *resnull)
{
	DomainConstraintRef *constraint_ref;
	Datum	   *domainval = NULL;
	bool	   *domainnull = NULL;
	ListCell   *l;

	scratch->d.domaincheck.resulttype = ctest->resulttype;
	/* 只有在需要时才分配工作区    */
	scratch->d.domaincheck.checkvalue = NULL;
	scratch->d.domaincheck.checknull = NULL;

	/*
	 * 对参数求值——直接把它存入 resv/resnull 没有问题， 
	 * 因为若有约束失败就会报错，否则它就是                
	 * 需要返回的值。
	 */
	ExecInitExprRec(ctest->arg, state, resv, resnull);

	/*
	 * 注意：若参数为 varlena 类型，它可能是读写的展开  
	 * 对象。我们希望返回 R/W 指针作为最终结果，但传给  
	 * 检查表达式中各函数的待测值却必须是 R/O 指针。    
	 * 不过，只有确实存在至少一个检查表达式时，我们 
	 * 才生成 MAKE_READONLY 步骤。而在做此检验之前，         
	 * domainval/domainnull 均为 NULL。              
	 */

	/*
	 * 收集与该域相关联的各条约束。         
	 *
	 * 注意：PG v10 之前，每次对表达式求值都要重新检查
	 * 约束集。现在我们会在执行器初始化时把它们固化
	 * 进 ExprState。这意味着我们不再需要 typcache.c 提供       
	 * 编译表达式。     
	 */
	constraint_ref = (DomainConstraintRef *)
		palloc(sizeof(DomainConstraintRef));
	InitDomainConstraintRef(ctest->resulttype,
							constraint_ref,
							CurrentMemoryContext,
							false);

	/*
	 * 生成检查每条域约束的代码。NOTNULL 约束只须作用     
	 * 于 resv/resnull 值即可，但对 CHECK 约束则需                 
	 * 要多费周折。
	 */
	foreach(l, constraint_ref->constraints)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);
		Datum	   *save_innermost_domainval;
		bool	   *save_innermost_domainnull;

		scratch->d.domaincheck.constraintname = con->name;

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				scratch->opcode = EEOP_DOMAIN_NOTNULL;
				ExprEvalPushStep(state, scratch);
				break;
			case DOM_CONSTRAINT_CHECK:
				/* 若尚未分配，则为 CHECK 输出分配工作区 */
				if (scratch->d.domaincheck.checkvalue == NULL)
				{
					scratch->d.domaincheck.checkvalue =
						(Datum *) palloc(sizeof(Datum));
					scratch->d.domaincheck.checknull =
						(bool *) palloc(sizeof(bool));
				}

				/*
				 * 首次经过时，确定 CoerceToDomainValue 节点       
				 * 应从何处读值。  
				 */
				if (domainval == NULL)
				{
					/*
					 * 因 value 可能被多次读取，强制转为 R/O， 
					 * 但仅当可能是展开 datum 时。       
					 */
					if (get_typlen(ctest->resulttype) == -1)
					{
						ExprEvalStep scratch2 = {0};

						/* 是，为 MAKE_READONLY 生成输出工作区   */
						domainval = (Datum *) palloc(sizeof(Datum));
						domainnull = (bool *) palloc(sizeof(bool));

						/* 生成MAKE_READONLY*/
						scratch2.opcode = EEOP_MAKE_READONLY;
						scratch2.resvalue = domainval;
						scratch2.resnull = domainnull;
						scratch2.d.make_readonly.value = resv;
						scratch2.d.make_readonly.isnull = resnull;
						ExprEvalPushStep(state, &scratch2);
					}
					else
					{
						/* 否，从 resv/resnull 读取即可        */
						domainval = resv;
						domainnull = resnull;
					}
				}

				/*
				 * 设置 CoerceToDomainValue 节点要返回的值。      
				 * 必须保存并恢复 innermost_domainval/null           
				 * 字段，以防本节点自身正处于另一域的检查
				 * 表达式中。
				 */
				save_innermost_domainval = state->innermost_domainval;
				save_innermost_domainnull = state->innermost_domainnull;
				state->innermost_domainval = domainval;
				state->innermost_domainnull = domainnull;

				/* 对检查表达式的值求值  */
				ExecInitExprRec(con->check_expr, state,
								scratch->d.domaincheck.checkvalue,
								scratch->d.domaincheck.checknull);

				state->innermost_domainval = save_innermost_domainval;
				state->innermost_domainnull = save_innermost_domainnull;

				/* 再测试结果 */
				scratch->opcode = EEOP_DOMAIN_CHECK;
				ExprEvalPushStep(state, scratch);

				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->constrainttype);
				break;
		}
	}
}

/*
 * 为分组集阶段中所有聚合构建转移/组合函数调用。       
 * 它必须调用阶段中所有基于排序的转移（doSort            
 * 为真时）、所有基于哈希的转移（doHash 为真时），或    
 * 两者都调用（两者都为真）。                    
 *
 * 所得表达式对每组转移值会先检查过滤器，再           
 * 求值聚合输入，并在转移函数为严格时检查该输入是否 
 * 为 NULL，最后为每个并发计算的分组集                
 * 调用转移。                                                
 *
 * 若 nullcheck 为真，生成的代码会检查指向 AggStatePerGroup   
 * 数组是否为 NULL 指针，若是则跳过求值。   
 */
ExprState *
ExecBuildAggTrans(AggState *aggstate, AggStatePerPhase phase,
				  bool doSort, bool doHash, bool nullcheck)
{
	ExprState  *state = makeNode(ExprState);
	PlanState  *parent = &aggstate->ss.ps;
	ExprEvalStep scratch = {0};
	bool		isCombine = DO_AGGSPLIT_COMBINE(aggstate->aggsplit);
	ExprSetupInfo deform = {0, 0, 0, NIL};

	state->expr = (Expr *) aggstate;
	state->parent = parent;

	scratch.resvalue = &state->resvalue;
	scratch.resnull = &state->resnull;

	/*
	 * 先弄清我们需要哪些 slot，以及每个 slot 各需要     
	 * 多少列。  
	 */
	for (int transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];

		expr_setup_walker((Node *) pertrans->aggref->aggdirectargs,
						  &deform);
		expr_setup_walker((Node *) pertrans->aggref->args,
						  &deform);
		expr_setup_walker((Node *) pertrans->aggref->aggorder,
						  &deform);
		expr_setup_walker((Node *) pertrans->aggref->aggdistinct,
						  &deform);
		expr_setup_walker((Node *) pertrans->aggref->aggfilter,
						  &deform);
	}
	ExecPushExprSetupSteps(state, &deform);

	/*
	 * 为每个转移值/分组集组合生成指令。                      
	 */
	for (int transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];
		FunctionCallInfo trans_fcinfo = pertrans->transfn_fcinfo;
		List	   *adjust_bailout = NIL;
		NullableDatum *strictargs = NULL;
		bool	   *strictnulls = NULL;
		int			argno;
		ListCell   *bail;

		/*
		 * 若有过滤器则生成。应在求值输入前做，以免  
		 * 产生可能多余的计算，或更糟的意外副作用。在   
		 * 组合时，所有必要的过滤都已经                    
		 * 完成了。      
		 */
		if (pertrans->aggref->aggfilter && !isCombine)
		{
			/* 对过滤表达式求值   */
			ExecInitExprRec(pertrans->aggref->aggfilter, state,
							&state->resvalue, &state->resnull);
			/* 若为假则跳出    */
			scratch.opcode = EEOP_JUMP_IF_NOT_TRUE;
			scratch.d.jump.jumpdone = -1;	/* 稍后调整 */
			ExprEvalPushStep(state, &scratch);
			adjust_bailout = lappend_int(adjust_bailout,
										 state->steps_len - 1);
		}

		/*
		 * 对聚合/组合函数的参数求值。         
		 */
		argno = 0;
		if (isCombine)
		{
			/*
			 * 组合两个聚合转移值。输入并非直接来自元组  
			 * 而是可能经过反序列化的一个                      
			 * 转移值。     
			 */
			TargetEntry *source_tle;

			Assert(pertrans->numSortCols == 0);
			Assert(list_length(pertrans->aggref->args) == 1);

			strictargs = trans_fcinfo->args + 1;
			source_tle = (TargetEntry *) linitial(pertrans->aggref->args);

			/*
			 * 若须在调用组合函数前反序列化输入状态，  
			 * 则会设置 deserialfn_oid。            
			 */
			if (!OidIsValid(pertrans->deserialfn_oid))
			{
				/*
				 * 从 1 开始，因为第 0 个参数将是转移      
				 * 值  
				 */
				ExecInitExprRec(source_tle->expr, state,
								&trans_fcinfo->args[argno + 1].value,
								&trans_fcinfo->args[argno + 1].isnull);
			}
			else
			{
				FunctionCallInfo ds_fcinfo = pertrans->deserialfn_fcinfo;

				/* 对参数求值   */
				ExecInitExprRec(source_tle->expr, state,
								&ds_fcinfo->args[0].value,
								&ds_fcinfo->args[0].isnull);

				/* 出于类型安全考虑的哑第二参数    */
				ds_fcinfo->args[1].value = PointerGetDatum(NULL);
				ds_fcinfo->args[1].isnull = false;

				/*
				 * 不要以 NULL 输入调用严格的反序列化函   
				 * 数  
				 */
				if (pertrans->deserialfn.fn_strict)
					scratch.opcode = EEOP_AGG_STRICT_DESERIALIZE;
				else
					scratch.opcode = EEOP_AGG_DESERIALIZE;

				scratch.d.agg_deserialize.fcinfo_data = ds_fcinfo;
				scratch.d.agg_deserialize.jumpnull = -1;	/* 稍后调整 */
				scratch.resvalue = &trans_fcinfo->args[argno + 1].value;
				scratch.resnull = &trans_fcinfo->args[argno + 1].isnull;

				ExprEvalPushStep(state, &scratch);
				/* 除非函数严格，否则不要添加调整         */
				if (pertrans->deserialfn.fn_strict)
					adjust_bailout = lappend_int(adjust_bailout,
												 state->steps_len - 1);

				/* 恢复 scratch 字段的正常设置      */
				scratch.resvalue = &state->resvalue;
				scratch.resnull = &state->resnull;
			}
			argno++;

			Assert(pertrans->numInputs == argno);
		}
		else if (!pertrans->aggsortrequired)
		{
			ListCell   *arg;

			/*
			 * 无 ORDER BY / DISTINCT 的普通转移函数；或有        
			 * ORDER BY / DISTINCT 但规划器已给了我们预排序   
			 * 输入
			 */
			strictargs = trans_fcinfo->args + 1;

			foreach(arg, pertrans->aggref->args)
			{
				TargetEntry *source_tle = (TargetEntry *) lfirst(arg);

				/*
				 * 不要为可能存在于预排序聚合中的 ORDER BY  
				 * 子句初始化参数。       
				 */
				if (argno == pertrans->numTransInputs)
					break;

				/*
				 * 从 1 开始，因为第 0 个参数将是转移      
				 * 值  
				 */
				ExecInitExprRec(source_tle->expr, state,
								&trans_fcinfo->args[argno + 1].value,
								&trans_fcinfo->args[argno + 1].isnull);
				argno++;
			}
			Assert(pertrans->numTransInputs == argno);
		}
		else if (pertrans->numInputs == 1)
		{
			/*
			 * 非预排序的 DISTINCT 和/或 ORDER BY 情形，且按 
			 * 单列排序。  
			 */
			TargetEntry *source_tle =
				(TargetEntry *) linitial(pertrans->aggref->args);

			Assert(list_length(pertrans->aggref->args) == 1);

			ExecInitExprRec(source_tle->expr, state,
							&state->resvalue,
							&state->resnull);
			strictnulls = &state->resnull;
			argno++;

			Assert(pertrans->numInputs == argno);
		}
		else
		{
			/*
			 * 非预排序的 DISTINCT 和/或 ORDER BY 情形，且    
			 * 按多列排序。
			 */
			Datum	   *values = pertrans->sortslot->tts_values;
			bool	   *nulls = pertrans->sortslot->tts_isnull;
			ListCell   *arg;

			strictnulls = nulls;

			foreach(arg, pertrans->aggref->args)
			{
				TargetEntry *source_tle = (TargetEntry *) lfirst(arg);

				ExecInitExprRec(source_tle->expr, state,
								&values[argno], &nulls[argno]);
				argno++;
			}
			Assert(pertrans->numInputs == argno);
		}

		/*
		 * 对严格 transfn，NULL 输入时什么都不会发生；         
		 * 我们只是沿用原先的 transValue。这对普通聚合和 
		 * 排序/DISTINCT 聚合。  
		 */
		if (trans_fcinfo->flinfo->fn_strict && pertrans->numTransInputs > 0)
		{
			if (strictnulls)
				scratch.opcode = EEOP_AGG_STRICT_INPUT_CHECK_NULLS;
			else
				scratch.opcode = EEOP_AGG_STRICT_INPUT_CHECK_ARGS;
			scratch.d.agg_strict_input_check.nulls = strictnulls;
			scratch.d.agg_strict_input_check.args = strictargs;
			scratch.d.agg_strict_input_check.jumpnull = -1; /* 稍后调整 */
			scratch.d.agg_strict_input_check.nargs = pertrans->numTransInputs;
			ExprEvalPushStep(state, &scratch);
			adjust_bailout = lappend_int(adjust_bailout,
										 state->steps_len - 1);
		}

		/* 处理带预排序输入的 DISTINCT 聚合            */
		if (pertrans->numDistinctCols > 0 && !pertrans->aggsortrequired)
		{
			if (pertrans->numDistinctCols > 1)
				scratch.opcode = EEOP_AGG_PRESORTED_DISTINCT_MULTI;
			else
				scratch.opcode = EEOP_AGG_PRESORTED_DISTINCT_SINGLE;

			scratch.d.agg_presorted_distinctcheck.pertrans = pertrans;
			scratch.d.agg_presorted_distinctcheck.jumpdistinct = -1;	/* 稍后调整 */
			ExprEvalPushStep(state, &scratch);
			adjust_bailout = lappend_int(adjust_bailout,
										 state->steps_len - 1);
		}

		/*
		 * 调用转移函数（每个并发求值分组集一次）。  
		 * 排序和哈希两种计算，只要适用都会这么            
		 * 做。     
		 */
		if (doSort)
		{
			int			processGroupingSets = Max(phase->numsets, 1);
			int			setoff = 0;

			for (int setno = 0; setno < processGroupingSets; setno++)
			{
				ExecBuildAggTransCall(state, aggstate, &scratch, trans_fcinfo,
									  pertrans, transno, setno, setoff, false,
									  nullcheck);
				setoff++;
			}
		}

		if (doHash)
		{
			int			numHashes = aggstate->num_hashes;
			int			setoff;

			/* 在 MIXED 模式下，会有前序的转移值存在   */
			if (aggstate->aggstrategy != AGG_HASHED)
				setoff = aggstate->maxsets;
			else
				setoff = 0;

			for (int setno = 0; setno < numHashes; setno++)
			{
				ExecBuildAggTransCall(state, aggstate, &scratch, trans_fcinfo,
									  pertrans, transno, setno, setoff, true,
									  nullcheck);
				setoff++;
			}
		}

		/* 调整提前退出的跳转目标    */
		foreach(bail, adjust_bailout)
		{
			ExprEvalStep *as = &state->steps[lfirst_int(bail)];

			if (as->opcode == EEOP_JUMP_IF_NOT_TRUE)
			{
				Assert(as->d.jump.jumpdone == -1);
				as->d.jump.jumpdone = state->steps_len;
			}
			else if (as->opcode == EEOP_AGG_STRICT_INPUT_CHECK_ARGS ||
					 as->opcode == EEOP_AGG_STRICT_INPUT_CHECK_NULLS)
			{
				Assert(as->d.agg_strict_input_check.jumpnull == -1);
				as->d.agg_strict_input_check.jumpnull = state->steps_len;
			}
			else if (as->opcode == EEOP_AGG_STRICT_DESERIALIZE)
			{
				Assert(as->d.agg_deserialize.jumpnull == -1);
				as->d.agg_deserialize.jumpnull = state->steps_len;
			}
			else if (as->opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE ||
					 as->opcode == EEOP_AGG_PRESORTED_DISTINCT_MULTI)
			{
				Assert(as->d.agg_presorted_distinctcheck.jumpdistinct == -1);
				as->d.agg_presorted_distinctcheck.jumpdistinct = state->steps_len;
			}
			else
				Assert(false);
		}
	}

	scratch.resvalue = NULL;
	scratch.resnull = NULL;
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * 为单个转移值构建状态转移/组合函数调用。          
 * 之所以从 ExecBuildAggTrans() 拆出，是因为有多个       
 * 调用点（某些分组集情形下用哈希和排序）。  
 */
static void
ExecBuildAggTransCall(ExprState *state, AggState *aggstate,
					  ExprEvalStep *scratch,
					  FunctionCallInfo fcinfo, AggStatePerTrans pertrans,
					  int transno, int setno, int setoff, bool ishash,
					  bool nullcheck)
{
	ExprContext *aggcontext;
	int			adjust_jumpnull = -1;

	if (ishash)
		aggcontext = aggstate->hashcontext;
	else
		aggcontext = aggstate->aggcontexts[setno];

	/* 添加 NULL 指针检查？ */
	if (nullcheck)
	{
		scratch->opcode = EEOP_AGG_PLAIN_PERGROUP_NULLCHECK;
		scratch->d.agg_plain_pergroup_nullcheck.setoff = setoff;
		/* 稍后调整 */
		scratch->d.agg_plain_pergroup_nullcheck.jumpnull = -1;
		ExprEvalPushStep(state, scratch);
		adjust_jumpnull = state->steps_len - 1;
	}

	/*
	 * 确定合适的转移实现方式。            
	 *
	 * 对于非有序聚合，以及带预排序输入的 ORDER BY      
	 * /DISTINCT 聚合
	 *
	 * 如果转移状态的初值在 pg_aggregate 表中不存在，    
	 * 那么我们就让外层 procNode 返回的第一个非 NULL 值     
	 * 成为初值。（这对 max()/min() 之类聚合很有用。）     
	 * noTransValue 标志用来表示需要这样做。若为真，则       
	 * 会生成一个                   
	 * EEOP_AGG_INIT_STRICT_PLAIN_TRANS{,_BYVAL} 步骤，它                 
	 * 还需完成下述工作：
	 *
	 * 如果函数是严格的，但确实有初值，则选择        
	 * EEOP_AGG_STRICT_PLAIN_TRANS{,_BYVAL}，它会在转移值       
	 * 变为 NULL 时跳过转移函数（因为之前某个转移函数  
	 * 返回了 NULL）。该步骤也需完成下述                        
	 * 工作：      
	 *
	 * 否则我们调用 EEOP_AGG_PLAIN_TRANS{,_BYVAL}，它无须           
	 * 执行上述任何一种检查。  
	 *
	 * 让步骤职责重叠并不理想，但聚合对性能极为   
	 * 敏感，这样做是值得的。                                   
	 *
	 * 对于有序聚合：  
	 *
	 * 只需在单有序列的较快路径与多列路径之间做出 
	 * 选择。严格性等检查会推迟到聚合定稿时进行。      
	 * 参见 process_ordered_aggregate_         
	 * {single, multi} 与                          
	 * advance_transition_function 
	 */
	if (!pertrans->aggsortrequired)
	{
		if (pertrans->transtypeByVal)
		{
			if (fcinfo->flinfo->fn_strict &&
				pertrans->initValueIsNull)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL;
			else if (fcinfo->flinfo->fn_strict)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL;
			else
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_BYVAL;
		}
		else
		{
			if (fcinfo->flinfo->fn_strict &&
				pertrans->initValueIsNull)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF;
			else if (fcinfo->flinfo->fn_strict)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_STRICT_BYREF;
			else
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_BYREF;
		}
	}
	else if (pertrans->numInputs == 1)
		scratch->opcode = EEOP_AGG_ORDERED_TRANS_DATUM;
	else
		scratch->opcode = EEOP_AGG_ORDERED_TRANS_TUPLE;

	scratch->d.agg_trans.pertrans = pertrans;
	scratch->d.agg_trans.setno = setno;
	scratch->d.agg_trans.setoff = setoff;
	scratch->d.agg_trans.transno = transno;
	scratch->d.agg_trans.aggcontext = aggcontext;
	ExprEvalPushStep(state, scratch);

	/* 修正 jumpnull */
	if (adjust_jumpnull != -1)
	{
		ExprEvalStep *as = &state->steps[adjust_jumpnull];

		Assert(as->opcode == EEOP_AGG_PLAIN_PERGROUP_NULLCHECK);
		Assert(as->d.agg_plain_pergroup_nullcheck.jumpnull == -1);
		as->d.agg_plain_pergroup_nullcheck.jumpnull = state->steps_len;
	}
}

/*
 * 构建能用 ExecQual() 求值的相等表达式，当表达式上下文的
 * 内层/外层元组 NOT DISTINCT 时返回真。也就是说：         
 * 两个 NULL 匹配，NULL 与非 NULL 不匹配。 
 *
 * desc: 待比较元组的元组描述符            
 * numCols: 要检查的属性数量               
 * keyColIdx: 属性列号组成的数组      
 * eqFunctions: 待使用的相等函数的函数 OID 数组           
 * parent: 父执行器节点  
 */
ExprState *
ExecBuildGroupingEqual(TupleDesc ldesc, TupleDesc rdesc,
					   const TupleTableSlotOps *lops, const TupleTableSlotOps *rops,
					   int numCols,
					   const AttrNumber *keyColIdx,
					   const Oid *eqfunctions,
					   const Oid *collations,
					   PlanState *parent)
{
	ExprState  *state = makeNode(ExprState);
	ExprEvalStep scratch = {0};
	int			maxatt = -1;
	List	   *adjust_jumps = NIL;
	ListCell   *lc;

	/*
	 * 当实际上没有比较任何列时，结果总为真。参见     
	 * ExecQual() 的特例。    
	 */
	if (numCols == 0)
		return NULL;

	state->expr = NULL;
	state->flags = EEO_FLAG_IS_QUAL;
	state->parent = parent;

	scratch.resvalue = &state->resvalue;
	scratch.resnull = &state->resnull;

	/* 计算所需最大属性号  */
	for (int natt = 0; natt < numCols; natt++)
	{
		int			attno = keyColIdx[natt];

		if (attno > maxatt)
			maxatt = attno;
	}
	Assert(maxatt >= 0);

	/* 压入解构步骤*/
	scratch.opcode = EEOP_INNER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = ldesc;
	scratch.d.fetch.kind = lops;
	if (ExecComputeSlotInfo(state, &scratch))
		ExprEvalPushStep(state, &scratch);

	scratch.opcode = EEOP_OUTER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = rdesc;
	scratch.d.fetch.kind = rops;
	if (ExecComputeSlotInfo(state, &scratch))
		ExprEvalPushStep(state, &scratch);

	/*
	 * 从最后一个字段（最不重要的排序键）开始比较。    
	 * 对于已排序的输入，它是最可能不同的字段。        
	 */
	for (int natt = numCols; --natt >= 0;)
	{
		int			attno = keyColIdx[natt];
		Form_pg_attribute latt = TupleDescAttr(ldesc, attno - 1);
		Form_pg_attribute ratt = TupleDescAttr(rdesc, attno - 1);
		Oid			foid = eqfunctions[natt];
		Oid			collid = collations[natt];
		FmgrInfo   *finfo;
		FunctionCallInfo fcinfo;
		AclResult	aclresult;

		/* 检查调用该函数的权限    */
		aclresult = object_aclcheck(ProcedureRelationId, foid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(foid));

		InvokeFunctionExecuteHook(foid);

		/* 设置主 fmgr 查找信息                */
		finfo = palloc0(sizeof(FmgrInfo));
		fcinfo = palloc0(SizeForFunctionCallInfo(2));
		fmgr_info(foid, finfo);
		fmgr_info_set_expr(NULL, finfo);
		InitFunctionCallInfoData(*fcinfo, finfo, 2,
								 collid, NULL, NULL);

		/* 左参数*/
		scratch.opcode = EEOP_INNER_VAR;
		scratch.d.var.attnum = attno - 1;
		scratch.d.var.vartype = latt->atttypid;
		scratch.resvalue = &fcinfo->args[0].value;
		scratch.resnull = &fcinfo->args[0].isnull;
		ExprEvalPushStep(state, &scratch);

		/* 右参数 */
		scratch.opcode = EEOP_OUTER_VAR;
		scratch.d.var.attnum = attno - 1;
		scratch.d.var.vartype = ratt->atttypid;
		scratch.resvalue = &fcinfo->args[1].value;
		scratch.resnull = &fcinfo->args[1].isnull;
		ExprEvalPushStep(state, &scratch);

		/* 对去重判定求值 */
		scratch.opcode = EEOP_NOT_DISTINCT;
		scratch.d.func.finfo = finfo;
		scratch.d.func.fcinfo_data = fcinfo;
		scratch.d.func.fn_addr = finfo->fn_addr;
		scratch.d.func.nargs = 2;
		scratch.resvalue = &state->resvalue;
		scratch.resnull = &state->resnull;
		ExprEvalPushStep(state, &scratch);

		/* 再生成 EEOP_QUAL 检测结果是否为假（或 NULL） */
		scratch.opcode = EEOP_QUAL;
		scratch.d.qualexpr.jumpdone = -1;
		scratch.resvalue = &state->resvalue;
		scratch.resnull = &state->resnull;
		ExprEvalPushStep(state, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	/* 调整跳转目标  */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = state->steps_len;
	}

	scratch.resvalue = NULL;
	scratch.resnull = NULL;
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}

/*
 * 构建能用 ExecQual() 求值的相等表达式，当表达式上下文的
 * 内层/外层元组相等时返回真。内层/外层 slot 中的        
 * 各 datum 被假定与 'eqfunctions' 参数具有相同的顺序         
 * 和数量。NULL 被视为相等。                      
 *
 * desc: 待比较元组的元组描述符            
 * lops: 内层元组 slot 的 slot 操作符  
 * rops: 外层元组 slot 的 slot 操作符  
 * eqFunctions: 待使用的相等函数的函数 OID 数组           
 * 其长度必须与 'param_exprs' 列表相同。       
 * collations: 用于相等比较的排序规则 OID。长度必须      
 * 与 'param_exprs' 列表相同。     
 * parent: 父执行器节点  
 */
ExprState *
ExecBuildParamSetEqual(TupleDesc desc,
					   const TupleTableSlotOps *lops,
					   const TupleTableSlotOps *rops,
					   const Oid *eqfunctions,
					   const Oid *collations,
					   const List *param_exprs,
					   PlanState *parent)
{
	ExprState  *state = makeNode(ExprState);
	ExprEvalStep scratch = {0};
	int			maxatt = list_length(param_exprs);
	List	   *adjust_jumps = NIL;
	ListCell   *lc;

	state->expr = NULL;
	state->flags = EEO_FLAG_IS_QUAL;
	state->parent = parent;

	scratch.resvalue = &state->resvalue;
	scratch.resnull = &state->resnull;

	/* 压入解构步骤*/
	scratch.opcode = EEOP_INNER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = desc;
	scratch.d.fetch.kind = lops;
	if (ExecComputeSlotInfo(state, &scratch))
		ExprEvalPushStep(state, &scratch);

	scratch.opcode = EEOP_OUTER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = desc;
	scratch.d.fetch.kind = rops;
	if (ExecComputeSlotInfo(state, &scratch))
		ExprEvalPushStep(state, &scratch);

	for (int attno = 0; attno < maxatt; attno++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attno);
		Oid			foid = eqfunctions[attno];
		Oid			collid = collations[attno];
		FmgrInfo   *finfo;
		FunctionCallInfo fcinfo;
		AclResult	aclresult;

		/* 检查调用该函数的权限    */
		aclresult = object_aclcheck(ProcedureRelationId, foid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(foid));

		InvokeFunctionExecuteHook(foid);

		/* 设置主 fmgr 查找信息                */
		finfo = palloc0(sizeof(FmgrInfo));
		fcinfo = palloc0(SizeForFunctionCallInfo(2));
		fmgr_info(foid, finfo);
		fmgr_info_set_expr(NULL, finfo);
		InitFunctionCallInfoData(*fcinfo, finfo, 2,
								 collid, NULL, NULL);

		/* 左参数*/
		scratch.opcode = EEOP_INNER_VAR;
		scratch.d.var.attnum = attno;
		scratch.d.var.vartype = att->atttypid;
		scratch.resvalue = &fcinfo->args[0].value;
		scratch.resnull = &fcinfo->args[0].isnull;
		ExprEvalPushStep(state, &scratch);

		/* 右参数 */
		scratch.opcode = EEOP_OUTER_VAR;
		scratch.d.var.attnum = attno;
		scratch.d.var.vartype = att->atttypid;
		scratch.resvalue = &fcinfo->args[1].value;
		scratch.resnull = &fcinfo->args[1].isnull;
		ExprEvalPushStep(state, &scratch);

		/* 对去重判定求值 */
		scratch.opcode = EEOP_NOT_DISTINCT;
		scratch.d.func.finfo = finfo;
		scratch.d.func.fcinfo_data = fcinfo;
		scratch.d.func.fn_addr = finfo->fn_addr;
		scratch.d.func.nargs = 2;
		scratch.resvalue = &state->resvalue;
		scratch.resnull = &state->resnull;
		ExprEvalPushStep(state, &scratch);

		/* 再生成 EEOP_QUAL 检测结果是否为假（或 NULL） */
		scratch.opcode = EEOP_QUAL;
		scratch.d.qualexpr.jumpdone = -1;
		scratch.resvalue = &state->resvalue;
		scratch.resnull = &state->resnull;
		ExprEvalPushStep(state, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   state->steps_len - 1);
	}

	/* 调整跳转目标  */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = &state->steps[lfirst_int(lc)];

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = state->steps_len;
	}

	scratch.resvalue = NULL;
	scratch.resnull = NULL;
	scratch.opcode = EEOP_DONE;
	ExprEvalPushStep(state, &scratch);

	ExecReadyExpr(state);

	return state;
}
