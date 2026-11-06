/*-------------------------------------------------------------------------
 *
 * execSRF.c
 *	  实现集合返回函数 API 的例程。                
 *
 * 本文件服务于 nodeFunctionscan.c 与 nodeProjectSet.c，      
 * 按照 ReturnSetInfo API 为调用集合返回函数提供      
 * 公共代码。   
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execSRF.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "executor/execdebug.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


/* 静态函数声明    */
static void init_sexpr(Oid foid, Oid input_collation, Expr *node,
					   SetExprState *sexpr, PlanState *parent,
					   MemoryContext sexprCxt, bool allowSRF, bool needDescForSRF);
static void ShutdownSetExpr(Datum arg);
static void ExecEvalFuncArgs(FunctionCallInfo fcinfo,
							 List *argList, ExprContext *econtext);
static void ExecPrepareTuplestoreResult(SetExprState *sexpr,
										ExprContext *econtext,
										Tuplestorestate *resultStore,
										TupleDesc resultDesc);
static void tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);


/*
 * 为执行准备 FROM (ROWS FROM) 中的函数调用。  
 *
 * 由 nodeFunctionscan.c 使用。   
 */
SetExprState *
ExecInitTableFunctionResult(Expr *expr,
							ExprContext *econtext, PlanState *parent)
{
	SetExprState *state = makeNode(SetExprState);

	state->funcReturnsSet = false;
	state->expr = expr;
	state->func.fn_oid = InvalidOid;

	/*
	 * 传入的表达式树通常是 FuncExpr，因为语法           
	 * 只允许在表函数引用的顶层出现函数调用。      
	 * 不过，若函数不返回集合，规划器可能已通过         
	 * 常量折叠或内联替换了函数调用。因此若我们看到    
	 * 任何其他种类的表达式节点，就通过通用的            
	 * ExecEvalExpr() 代码来执行它。但该代码路径          
	 * 不支持深藏在表达式中的集合返回函数。           
	 */
	if (IsA(expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) expr;

		state->funcReturnsSet = func->funcretset;
		state->args = ExecInitExprList(func->args, parent);

		init_sexpr(func->funcid, func->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, func->funcretset, false);
	}
	else
	{
		state->elidedFuncState = ExecInitExpr(expr, parent);
	}

	return state;
}

/*
 *		ExecMakeTableFunctionResult
 *
 * 对一个表函数求值，把结果物化在 Tuplestore 对象中。     
 *        
 *
 * 由 nodeFunctionscan.c 使用。   
 */
Tuplestorestate *
ExecMakeTableFunctionResult(SetExprState *setexpr,
							ExprContext *econtext,
							MemoryContext argContext,
							TupleDesc expectedDesc,
							bool randomAccess)
{
	Tuplestorestate *tupstore = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			funcrettype;
	bool		returnsTuple;
	bool		returnsSet = false;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;
	HeapTupleData tmptup;
	MemoryContext callerContext;
	bool		first_time = true;

	/*
	 * 在合适上下文中执行各表函数的动作。  
	 *
	 * FunctionCallInfo 需要在对一个 ValuePerCall            
	 * 函数的全部调用期间存活，因此不能分配在       
	 * 每元组上下文中。类似地，函数参数也需要在比     
	 * 每元组上下文存活期更长的上下文中求值：否则      
	 * 当在内循环中重置该上下文时，参数值就会消失。
	 * 由于调用者的 CurrentMemoryContext 通常是               
	 * 查询生命周期的上下文，我们不希望在那里泄漏        
	 * 内存。我们要求调用者传入一个可为此用途使用的     
	 * 独立内存上下文，每轮重置防膨胀。  
	 */
	MemoryContextReset(argContext);
	callerContext = MemoryContextSwitchTo(argContext);

	funcrettype = exprType((Node *) setexpr->expr);

	returnsTuple = type_is_rowtype(funcrettype);

	/*
	 * 准备一个用于通信的 resultinfo 节点。即使并不期待     
	 * 集合结果也总是这样做，以便能传递 expectedDesc。     
	 * 在通用表达式情形下，表达式实际上看不到该           
	 * resultinfo，但仍要设置它，因为其中一些字段           
	 * 要用作状态变量。
	 */
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = expectedDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize | SFRM_Materialize_Preferred);
	if (randomAccess)
		rsinfo.allowedModes |= (int) SFRM_Materialize_Random;
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone 在下方填充 */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	fcinfo = palloc(SizeForFunctionCallInfo(list_length(setexpr->args)));

	/*
	 * 传入的表达式树通常是 SetExprState，因为语法           
	 * 只允许在表函数引用的顶层出现函数调用。      
	 * 不过，若函数不返回集合，规划器可能已通过         
	 * 常量折叠或内联替换了该函数调用。因此若我们看到 
	 * 任何其他种类的表达式节点，就通过通用的            
	 * ExecEvalExpr() 代码执行它；唯一区别是我们                
	 * 没有机会向深藏于表达式中的函数传递特殊的       
	 * ReturnSetInfo。         
	 */
	if (!setexpr->elidedFuncState)
	{
		/*
		 * 此路径与 ExecMakeFunctionResultSet 类似。  
		 */
		returnsSet = setexpr->funcReturnsSet;
		InitFunctionCallInfoData(*fcinfo, &(setexpr->func),
								 list_length(setexpr->args),
								 setexpr->fcinfo->fncollation,
								 NULL, (Node *) &rsinfo);
		/* 对函数的参数列表求值        */
		Assert(CurrentMemoryContext == argContext);
		ExecEvalFuncArgs(fcinfo, setexpr->args, econtext);

		/*
		 * 若函数是 strict 的且有 NULL 参数，则跳过        
		 * 调用该函数，表现得如同它返回了 NULL（若       
		 * 返回集合则为空集）。
		 */
		if (setexpr->func.fn_strict)
		{
			int			i;

			for (i = 0; i < fcinfo->nargs; i++)
			{
				if (fcinfo->args[i].isnull)
					goto no_function_result;
			}
		}
	}
	else
	{
		/* 把 setexpr 视为通用表达式     */
		InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
	}

	/*
	 * 切换到短命上下文来调用该函数或表达式。            
	 */
	MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * 循环处理 ValuePerCall 协议（这也是通用              
	 * ExecEvalExpr 路径所需的行为）。          
	 */
	for (;;)
	{
		Datum		result;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 在每次调用函数或表达式之前重置每元组内存      
		 * 上下文。这会清理函数调用时可能泄漏的局部     
		 * 内存。   
		 */
		ResetExprContext(econtext);

		/* 调用一次函数或表达式           */
		if (!setexpr->elidedFuncState)
		{
			pgstat_init_function_usage(fcinfo, &fcusage);

			fcinfo->isnull = false;
			rsinfo.isDone = ExprSingleResult;
			result = FunctionCallInvoke(fcinfo);

			pgstat_end_function_usage(&fcusage,
									  rsinfo.isDone != ExprMultipleResult);
		}
		else
		{
			result =
				ExecEvalExpr(setexpr->elidedFuncState, econtext, &fcinfo->isnull);
			rsinfo.isDone = ExprSingleResult;
		}

		/* 函数想使用哪种协议？            */
		if (rsinfo.returnMode == SFRM_ValuePerCall)
		{
			/*
			 * 检查结果集的末尾。 
			 */
			if (rsinfo.isDone == ExprEndResult)
				break;

			/*
			 * 若是第一轮，为结果构建 tuplestore。对标量  
			 * 函数结果类型，还要做一个合适的 tupdesc。  
			 */
			if (first_time)
			{
				MemoryContext oldcontext =
					MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

				tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
				rsinfo.setResult = tupstore;
				if (!returnsTuple)
				{
					tupdesc = CreateTemplateTupleDesc(1);
					TupleDescInitEntry(tupdesc,
									   (AttrNumber) 1,
									   "column",
									   funcrettype,
									   -1,
									   0);
					rsinfo.setDesc = tupdesc;
				}
				MemoryContextSwitchTo(oldcontext);
			}

			/*
			 * 保存当前结果集条目  
			 */
			if (returnsTuple)
			{
				if (!fcinfo->isnull)
				{
					HeapTupleHeader td = DatumGetHeapTupleHeader(result);

					if (tupdesc == NULL)
					{
						MemoryContext oldcontext =
							MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

						/*
						 * 这是该函数首个非 NULL 的结果，
						 * 用行类型 Datum 内嵌的类型信息来 
						 * 查找所需的 tupdesc。并为该查询         
						 * 制作一份副本。
						 */
						tupdesc = lookup_rowtype_tupdesc_copy(HeapTupleHeaderGetTypeId(td),
															  HeapTupleHeaderGetTypMod(td));
						rsinfo.setDesc = tupdesc;
						MemoryContextSwitchTo(oldcontext);
					}
					else
					{
						/*
						 * 验证后续返回的行都有相同子类型； 
						 * 类型为 RECORD 时必须如此。  
						 */
						if (HeapTupleHeaderGetTypeId(td) != tupdesc->tdtypeid ||
							HeapTupleHeaderGetTypMod(td) != tupdesc->tdtypmod)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("rows returned by function are not all of the same row type")));
					}

					/*
					 * tuplestore_puttuple 需要 HeapTuple 而非     
					 * 光秃的 HeapTupleHeader，无须全部字段。   
					 */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					tuplestore_puttuple(tupstore, &tmptup);
				}
				else
				{
					/*
					 * 返回元组的函数给出 NULL 结果；将其扩   
					 * 展成一行全空值。我们依靠 expectedDesc 来 
					 * 构造这些行。（注意：若                   
					 * tuplestore_putvalues 保存了所提供描述符的  
					 * tdtypeid/tdtypmod，便有问题：可能与函数 
					 * 本身给出的不一致。但实际并非如此。） 
					 */
					int			natts = expectedDesc->natts;
					bool	   *nullflags;

					nullflags = (bool *) palloc(natts * sizeof(bool));
					memset(nullflags, true, natts * sizeof(bool));
					tuplestore_putvalues(tupstore, expectedDesc, NULL, nullflags);
				}
			}
			else
			{
				/* 标量类型情形：直接存储函数结果    */
				tuplestore_putvalues(tupstore, tupdesc, &result, &fcinfo->isnull);
			}

			/*
			 * 完成了？
			 */
			if (rsinfo.isDone != ExprMultipleResult)
				break;

			/*
			 * 检查集合返回函数是否被正确地声明。       
			 * （注意：出于历史原因，非 SRF 返回              
			 * ExprEndResult 时不报错；视为返回 NULL。）     
			 */
			if (!returnsSet)
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
						 errmsg("table-function protocol for value-per-call mode was not followed")));
		}
		else if (rsinfo.returnMode == SFRM_Materialize)
		{
			/* 检查我们与函数作者的假设是否一致    */
			if (!first_time || rsinfo.isDone != ExprSingleResult || !returnsSet)
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
						 errmsg("table-function protocol for materialize mode was not followed")));
			/* 集合结果求值完毕       */
			break;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("unrecognized table-function returnMode: %d",
							(int) rsinfo.returnMode)));

		first_time = false;
	}

no_function_result:

	/*
	 * 如果没从函数得到任何结果（即空集或 NULL 结果）， 
	 * 就必须创建要返回的 tuplestore，而如果这是 
	 * 非集合返回函数，则插入一行全空值。同上面      
	 * 一样，我们依靠 expectedDesc 来构造该占位行。       
	 */
	if (rsinfo.setResult == NULL)
	{
		MemoryContext oldcontext =
			MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
		rsinfo.setResult = tupstore;
		MemoryContextSwitchTo(oldcontext);

		if (!returnsSet)
		{
			int			natts = expectedDesc->natts;
			bool	   *nullflags;

			nullflags = (bool *) palloc(natts * sizeof(bool));
			memset(nullflags, true, natts * sizeof(bool));
			tuplestore_putvalues(tupstore, expectedDesc, NULL, nullflags);
		}
	}

	/*
	 * 如果函数提供了 tupdesc，就交叉检查它。其实只有      
	 * 返回 RECORD 的函数才需要这样做，但总是做也无妨。   
	 */
	if (rsinfo.setDesc)
	{
		tupledesc_match(expectedDesc, rsinfo.setDesc);

		/*
		 * 如果它是动态分配的 TupleDesc，就释放它：它
		 * 通常分配在每查询上下文中，因此必须避免   
		 * 在多次使用间泄漏它。    
		 */
		if (rsinfo.setDesc->tdrefcount == -1)
			FreeTupleDesc(rsinfo.setDesc);
	}

	MemoryContextSwitchTo(callerContext);

	/* 全部完成，交回 tuplestore   */
	return rsinfo.setResult;
}


/*
 * 为执行准备目标列表 SRF 函数调用。    
 *
 * 由 nodeProjectSet.c 使用。   
 */
SetExprState *
ExecInitFunctionResultSet(Expr *expr,
						  ExprContext *econtext, PlanState *parent)
{
	SetExprState *state = makeNode(SetExprState);

	state->funcReturnsSet = true;
	state->expr = expr;
	state->func.fn_oid = InvalidOid;

	/*
	 * 初始化元数据。表达式节点可能是 FuncExpr 或             
	 * OpExpr。 
	 */
	if (IsA(expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) expr;

		state->args = ExecInitExprList(func->args, parent);
		init_sexpr(func->funcid, func->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, true, true);
	}
	else if (IsA(expr, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) expr;

		state->args = ExecInitExprList(op->args, parent);
		init_sexpr(op->opfuncid, op->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, true, true);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(expr));

	/* 除非所选函数返回集合，否则不应走到这里   */
	Assert(state->func.fn_retset);

	return state;
}

/*
 *		ExecMakeFunctionResultSet
 *
 * 对一个集合返回函数的参数求值，然后调用该        
 * 函数本身。参数表达式不能包含集合返回函数            
 * （规划器应当已把那些函数的求值分离出来）。            
 *
 * 本函数应在短命（每元组）上下文中调用，而          
 * argContext 须活到各行返回完（*isDone 设为 ExprEndResult  
 * 或 ExprSingleResult）。         
 *
 * 由 nodeProjectSet.c 使用。   
 */
Datum
ExecMakeFunctionResultSet(SetExprState *fcache,
						  ExprContext *econtext,
						  MemoryContext argContext,
						  bool *isNull,
						  ExprDoneCond *isDone)
{
	List	   *arguments;
	Datum		result;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;
	bool		callit;
	int			i;

restart:

	/* 防止因表达式过于复杂而导致栈溢出               */
	check_stack_depth();

	/*
	 * 如果该函数的前一次调用返回了以 tuplestore 形式        
	 * 存在的集合结果，则从该 tuplestore 读取直到取空   
	 *       
	 */
	if (fcache->funcResultStore)
	{
		TupleTableSlot *slot = fcache->funcResultSlot;
		MemoryContext oldContext;
		bool		foundTup;

		/*
		 * 必须确保槽中的元组存活得足够久，否则      
		 * 清除槽时可能最终试图释放已被释放的东西。   
		 *       
		 */
		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		foundTup = tuplestore_gettupleslot(fcache->funcResultStore, true, false,
										   fcache->funcResultSlot);
		MemoryContextSwitchTo(oldContext);

		if (foundTup)
		{
			*isDone = ExprMultipleResult;
			if (fcache->funcReturnsTuple)
			{
				/* 必须把整个元组作为 Datum 返回。*/
				*isNull = false;
				return ExecFetchSlotHeapTupleDatum(fcache->funcResultSlot);
			}
			else
			{
				/* 抽取第一列并将其作为标量返回。       */
				return slot_getattr(fcache->funcResultSlot, 1, isNull);
			}
		}
		/* tuplestore 已取尽，故清理      */
		tuplestore_end(fcache->funcResultStore);
		fcache->funcResultStore = NULL;
		*isDone = ExprEndResult;
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * arguments 是表达式列表，须先求值再传给                
	 * 函数管理器。若求值已在前一次调用中完成，           
	 * （即我们正在继续对一个集合值函数的求值），    
	 * 就跳过它。否则把当前参数值收集到 fcinfo 中。         
	 *
	 * 这些参数必须存放在一个上下文中，其寿命            
	 * 至少要延续到该 SRF 所有行都返回之后，否则       
	 * ValuePerCall SRF 首行后即引用已释放的内存。  
	 */
	fcinfo = fcache->fcinfo;
	arguments = fcache->args;
	if (!fcache->setArgsValid)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(argContext);

		ExecEvalFuncArgs(fcinfo, arguments, econtext);
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		/* 重置标志（下面可能再设置）*/
		fcache->setArgsValid = false;
	}

	/*
	 * 现在调用该函数，传入已求值的参数值。        
	 */

	/* 准备用于通信的 resultinfo 节点。   */
	fcinfo->resultinfo = (Node *) &rsinfo;
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = fcache->funcResultDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	/* 注意：不设置 SFRM_Materialize_Random/_Preferred    */
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone 在下方填充 */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	/*
	 * 若函数是 strict 的且有任一 NULL 参数，就跳过调用。 
	 *              
	 */
	callit = true;
	if (fcache->func.fn_strict)
	{
		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		pgstat_init_function_usage(fcinfo, &fcusage);

		fcinfo->isnull = false;
		rsinfo.isDone = ExprSingleResult;
		result = FunctionCallInvoke(fcinfo);
		*isNull = fcinfo->isnull;
		*isDone = rsinfo.isDone;

		pgstat_end_function_usage(&fcusage,
								  rsinfo.isDone != ExprMultipleResult);
	}
	else
	{
		/* 对 strict SRF 而言，NULL 的结果为空集   */
		result = (Datum) 0;
		*isNull = true;
		*isDone = ExprEndResult;
	}

	/* 函数想使用哪种协议？            */
	if (rsinfo.returnMode == SFRM_ValuePerCall)
	{
		if (*isDone != ExprEndResult)
		{
			/*
			 * 保存当前参数值以供下次调用时复用。         
			 */
			if (*isDone == ExprMultipleResult)
			{
				fcache->setArgsValid = true;
				/* 若尚未注册则注册清理回调           */
				if (!fcache->shutdown_reg)
				{
					RegisterExprContextCallback(econtext,
												ShutdownSetExpr,
												PointerGetDatum(fcache));
					fcache->shutdown_reg = true;
				}
			}
		}
	}
	else if (rsinfo.returnMode == SFRM_Materialize)
	{
		/* 检查我们与函数作者的假设是否一致    */
		if (rsinfo.isDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("table-function protocol for materialize mode was not followed")));
		if (rsinfo.setResult != NULL)
		{
			/* 准备从 tuplestore 返回值               */
			ExecPrepareTuplestoreResult(fcache, econtext,
										rsinfo.setResult,
										rsinfo.setDesc);
			/* 回跳到顶部，开始从 tuplestore 返回       */
			goto restart;
		}
		/* 如果 setResult 保持为 NULL，则视作空集 */
		*isDone = ExprEndResult;
		*isNull = true;
		result = (Datum) 0;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("unrecognized table-function returnMode: %d",
						(int) rsinfo.returnMode)));

	return result;
}


/*
 * init_sexpr - 在首次使用时初始化 SetExprState 节点
 */
static void
init_sexpr(Oid foid, Oid input_collation, Expr *node,
		   SetExprState *sexpr, PlanState *parent,
		   MemoryContext sexprCxt, bool allowSRF, bool needDescForSRF)
{
	AclResult	aclresult;
	size_t		numargs = list_length(sexpr->args);

	/* 检查调用函数的权限       */
	aclresult = object_aclcheck(ProcedureRelationId, foid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(foid));
	InvokeFunctionExecuteHook(foid);

	/*
	 * 对 nargs 的安全检查。正常情况下它绝不应             
	 * 失败，因为解析器应当更早检查。但若编译          
	 * 服务器时所用 FUNC_MAX_ARGS 小于 pg_proc 中                    
	 * 声明的函数？  
	 */
	if (list_length(sexpr->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* 建立主要的 fmgr 查找信息          */
	fmgr_info_cxt(foid, &(sexpr->func), sexprCxt);
	fmgr_info_set_expr((Node *) sexpr->expr, &(sexpr->func));

	/* 同时初始化函数调用参数结构体            */
	sexpr->fcinfo =
		(FunctionCallInfo) palloc(SizeForFunctionCallInfo(numargs));
	InitFunctionCallInfoData(*sexpr->fcinfo, &(sexpr->func),
							 numargs,
							 input_collation, NULL, NULL);

	/* 若函数返回集合，检查调用者是否允许如此  */
	if (sexpr->func.fn_retset && !allowSRF)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 parent ? executor_errposition(parent->state,
											   exprLocation((Node *) node)) : 0));

	/* 否则，调用者应当已正确地标记该 sexpr      */
	Assert(sexpr->func.fn_retset == sexpr->funcReturnsSet);

	/* 如果函数返回集合，准备期望的元组描述符  */
	if (sexpr->func.fn_retset && needDescForSRF)
	{
		TypeFuncClass functypclass;
		Oid			funcrettype;
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		functypclass = get_expr_result_type(sexpr->func.fn_expr,
											&funcrettype,
											&tupdesc);

		/* 须将 tupdesc 存入 sexpr 上下文*/
		oldcontext = MemoryContextSwitchTo(sexprCxt);

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* 复合数据类型，例如某表的行类型*/
			Assert(tupdesc);
			/* 安全起见须从 typcache 复制出来*/
			sexpr->funcResultDesc = CreateTupleDescCopy(tupdesc);
			sexpr->funcReturnsTuple = true;
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* 基础类型，即标量    */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   NULL,
							   funcrettype,
							   -1,
							   0);
			sexpr->funcResultDesc = tupdesc;
			sexpr->funcReturnsTuple = false;
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			/* 如果函数不需要 expectedDesc，这样可行       */
			sexpr->funcResultDesc = NULL;
			sexpr->funcReturnsTuple = true;
		}
		else
		{
			/* 否则，若函数需要 expectedDesc 就会失败   */
			sexpr->funcResultDesc = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}
	else
		sexpr->funcResultDesc = NULL;

	/* 初始化附加状态       */
	sexpr->funcResultStore = NULL;
	sexpr->funcResultSlot = NULL;
	sexpr->shutdown_reg = false;
}

/*
 * 当某个 SetExprState 需要在运行完毕之前被关闭             
 * 时所用的回调函数  
 */
static void
ShutdownSetExpr(Datum arg)
{
	SetExprState *sexpr = castNode(SetExprState, DatumGetPointer(arg));

	/* 如果我们有槽，确保它已释放任何 tuplestore 指针    */
	if (sexpr->funcResultSlot)
		ExecClearTuple(sexpr->funcResultSlot);

	/* 释放打开的 tuplestore  */
	if (sexpr->funcResultStore)
		tuplestore_end(sexpr->funcResultStore);
	sexpr->funcResultStore = NULL;

	/* 清除所有活跃集合参数状态*/
	sexpr->setArgsValid = false;

	/* execUtils 会注销该回调……        */
	sexpr->shutdown_reg = false;
}

/*
 * 对函数的参数求值。       
 */
static void
ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList,
				 ExprContext *econtext)
{
	int			i;
	ListCell   *arg;

	i = 0;
	foreach(arg, argList)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->args[i].value = ExecEvalExpr(argstate,
											 econtext,
											 &fcinfo->args[i].isnull);
		i++;
	}

	Assert(i == fcinfo->nargs);
}

/*
 *		ExecPrepareTuplestoreResult
 *
 * ExecMakeFunctionResultSet 的子例程：准备从                      
 * tuplestore 函数结果中抽取行。我们必须建立            
 * funcResultSlot（除非前轮调用已建立过），并验证     
 * 函数是否返回期望的描述符。
 */
static void
ExecPrepareTuplestoreResult(SetExprState *sexpr,
							ExprContext *econtext,
							Tuplestorestate *resultStore,
							TupleDesc resultDesc)
{
	sexpr->funcResultStore = resultStore;

	if (sexpr->funcResultSlot == NULL)
	{
		/* 创建一个槽以便从 tuplestore 中读出数据     */
		TupleDesc	slotDesc;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(sexpr->func.fn_mcxt);

		/*
		 * 如果无法从上下文确定结果的行类型，              
		 * 且函数也没有返回 tupdesc，那就必须失败。  
		 */
		if (sexpr->funcResultDesc)
			slotDesc = sexpr->funcResultDesc;
		else if (resultDesc)
		{
			/* 不要假定 resultDesc 是长寿的  */
			slotDesc = CreateTupleDescCopy(resultDesc);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning setof record called in "
							"context that cannot accept type record")));
			slotDesc = NULL;	/* 让编译器安静  */
		}

		sexpr->funcResultSlot = MakeSingleTupleTableSlot(slotDesc,
														 &TTSOpsMinimalTuple);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * 如果函数提供了 tupdesc，就交叉检查它。其实只有      
	 * 返回 RECORD 的函数才需要这样做，但总是做也无妨。   
	 */
	if (resultDesc)
	{
		if (sexpr->funcResultDesc)
			tupledesc_match(sexpr->funcResultDesc, resultDesc);

		/*
		 * 如果它是动态分配的 TupleDesc，就释放它：它
		 * 通常分配在每查询上下文中，因此必须避免   
		 * 在多次使用间泄漏它。    
		 */
		if (resultDesc->tdrefcount == -1)
			FreeTupleDesc(resultDesc);
	}

	/* 若尚未注册则注册清理回调           */
	if (!sexpr->shutdown_reg)
	{
		RegisterExprContextCallback(econtext,
									ShutdownSetExpr,
									PointerGetDatum(sexpr));
		sexpr->shutdown_reg = true;
	}
}

/*
 * 检查函数结果元组类型 (src_tupdesc) 是否匹配         
 * 或可视为匹配查询期望的类型 (dst_tupdesc)。若    
 * 不匹配则 ereport。   
 *
 * 我们真正在意的只是属性数量和数据类型。    
 * 此外，对于目标类型中已被删除的列，我们可以     
 * 忽略其类型不匹配，只要物理存储方式一致。这    
 * 在某些涉及过期缓存计划的情形下很有用。
 */
static void
tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc)
{
	int			i;

	if (dst_tupdesc->natts != src_tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function return row and query-specified return row do not match"),
				 errdetail_plural("Returned row contains %d attribute, but query expects %d.",
								  "Returned row contains %d attributes, but query expects %d.",
								  src_tupdesc->natts,
								  src_tupdesc->natts, dst_tupdesc->natts)));

	for (i = 0; i < dst_tupdesc->natts; i++)
	{
		Form_pg_attribute dattr = TupleDescAttr(dst_tupdesc, i);
		Form_pg_attribute sattr = TupleDescAttr(src_tupdesc, i);

		if (IsBinaryCoercible(sattr->atttypid, dattr->atttypid))
			continue;			/* 无妨     */
		if (!dattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Returned type %s at ordinal position %d, but query expects %s.",
							   format_type_be(sattr->atttypid),
							   i + 1,
							   format_type_be(dattr->atttypid))));

		if (dattr->attlen != sattr->attlen ||
			dattr->attalign != sattr->attalign)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
							   i + 1)));
	}
}
