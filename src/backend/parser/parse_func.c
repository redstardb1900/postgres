/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		解析器中处理函数调用 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_func.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* LookupFuncNameInternal 可能返回的错误码  */
typedef enum
{
	FUNCLOOKUP_NOSUCHFUNC,
	FUNCLOOKUP_AMBIGUOUS
} FuncLookupError;

static void unify_hypothetical_args(ParseState *pstate,
									List *fargs, int numAggregatedArgs,
									Oid *actual_arg_types, Oid *declared_arg_types);
static Oid	FuncNameAsType(List *funcname);
static Node *ParseComplexProjection(ParseState *pstate, const char *funcname,
									Node *first_arg, int location);
static Oid	LookupFuncNameInternal(ObjectType objtype, List *funcname,
								   int nargs, const Oid *argtypes,
								   bool include_out_arguments, bool missing_ok,
								   FuncLookupError *lookupError);


/*
 *	解析函数调用   
 *
 *	出于历史原因，Postgres 试图将 tab.col 与                  
 *	col(tab) 视为等价：若单参数函数调用的参数是         
 *	复合类型且（未限定的）函数名匹配该类型的        
 *	任一属性，即可解释为列投影；反之，接受单个复    
 *	合类型参数的函数也能写成列引用的样子，让函数    
 *	如同计算列一般发挥作用。                                     
 *
 *	若两种解释都可行，优先取符合句法形式的那个；  
 *	除此之外形式并不重要。                      
 *
 *	因此两种情况都会经过这里。若 fn 为空，处理的       
 *	是列语法而非函数语法。函数语法情形下，       
 *	FuncCall 结构体用于携带各种修饰信息，这些修饰       
 *	适用于聚合与窗口函数。 
 *
 *	另外，当 fn 为空时，失败时返回 NULL 而不是  
 *	报告函数不存在错误。     
 *
 *	参数表达式（fargs 内）必须已转换完毕。不过  
 *	*fn 中的内容均未经转换。                     
 *
 *	last_srf 应当是刚开始转换 fargs 之前 pstate->p_last_srf  
 *	的副本。若调用者确知 fargs 不可能包含 SRF 调用，  
 *	则 last_srf 可直接取 pstate->p_last_srf。                
 *
 *	若正在考虑 CALL 语句则 proc_call 为真，此时名称       
 *	必须解析为过程名而非其他。此标志还规定参数列  
 *	表可以包含任意 OUT 模式的参数。                          
 */
Node *
ParseFuncOrColumn(ParseState *pstate, List *funcname, List *fargs,
				  Node *last_srf, FuncCall *fn, bool proc_call, int location)
{
	bool		is_column = (fn == NULL);
	List	   *agg_order = (fn ? fn->agg_order : NIL);
	Expr	   *agg_filter = NULL;
	WindowDef  *over = (fn ? fn->over : NULL);
	bool		agg_within_group = (fn ? fn->agg_within_group : false);
	bool		agg_star = (fn ? fn->agg_star : false);
	bool		agg_distinct = (fn ? fn->agg_distinct : false);
	bool		func_variadic = (fn ? fn->func_variadic : false);
	CoercionForm funcformat = (fn ? fn->funcformat : COERCE_EXPLICIT_CALL);
	bool		could_be_projection;
	Oid			rettype;
	Oid			funcid;
	ListCell   *l;
	Node	   *first_arg = NULL;
	int			nargs;
	int			nargsplusdefs;
	Oid			actual_arg_types[FUNC_MAX_ARGS];
	Oid		   *declared_arg_types;
	List	   *argnames;
	List	   *argdefaults;
	Node	   *retval;
	bool		retset;
	int			nvargs;
	Oid			vatype;
	FuncDetailCode fdresult;
	char		aggkind = 0;
	ParseCallbackState pcbstate;

	/*
	 * 若存在聚合过滤器，则用 transformWhereClause 转换之       
	 */
	if (fn && fn->agg_filter != NULL)
		agg_filter = (Expr *) transformWhereClause(pstate, fn->agg_filter,
												   EXPR_KIND_FILTER,
												   "FILTER");

	/*
	 * 解析器的其余部分大多直接假定函数的参数不超过    
	 * FUNC_MAX_ARGS 个。我们必须在此测试以防数组越界      
	 * 等问题。当然这可能不是函数，但该测试无害。     
	 *                           
	 */
	if (list_length(fargs) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS),
				 parser_errposition(pstate, location)));

	/*
	 * 为函数查找而提取参数类型信息。            
	 *
	 * 若有参数是 VOID 类型的 Param 标记，则将其从参        
	 * 数列表中丢弃。这是个 hack，让 JDBC 驱动在解析         
	 * 函数调用构造时无需区分输入与输出参数符号。    
	 * 处理列语法时不要这样做；有 WITHIN GROUP 时也            
	 * 不要这样做，因为那种情况下参数个数必须保持       
	 * 不变。                     
	 */
	nargs = 0;
	foreach(l, fargs)
	{
		Node	   *arg = lfirst(l);
		Oid			argtype = exprType(arg);

		if (argtype == VOIDOID && IsA(arg, Param) &&
			!is_column && !agg_within_group)
		{
			fargs = foreach_delete_current(fargs, l);
			continue;
		}

		actual_arg_types[nargs++] = argtype;
	}

	/*
	 * 检查命名参数；若有则构建名字列表。                
	 *
	 * 我们允许混合记法（一部分命名一部分不命名），  
	 * 但仅当所有命名参数都位于未命名参数之后。      
	 * 因此名字列表对应最后 N 个实际参数，无需任何        
	 * 额外的簿记来匹配。    
	 */
	argnames = NIL;
	foreach(l, fargs)
	{
		Node	   *arg = lfirst(l);

		if (IsA(arg, NamedArgExpr))
		{
			NamedArgExpr *na = (NamedArgExpr *) arg;
			ListCell   *lc;

			/* 拒绝重复的参数名   */
			foreach(lc, argnames)
			{
				if (strcmp(na->name, (char *) lfirst(lc)) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("argument name \"%s\" used more than once",
									na->name),
							 parser_errposition(pstate, na->location)));
			}
			argnames = lappend(argnames, na->name);
		}
		else
		{
			if (argnames != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("positional argument cannot follow named argument"),
						 parser_errposition(pstate, exprLocation(arg))));
		}
	}

	if (fargs)
	{
		first_arg = linitial(fargs);
		Assert(first_arg != NULL);
	}

	/*
	 * 判断把该构造视为列投影是否合理。为此，必须存     
	 * 在单个复合类型参数，函数名不得带限定，且不得 
	 * 存在任何需要它必须是函数的句法修饰（例如聚合    
	 * 或 variadic 修饰，或命名参数）。                     
	 *                                                       
	 */
	could_be_projection = (nargs == 1 && !proc_call &&
						   agg_order == NIL && agg_filter == NULL &&
						   !agg_star && !agg_distinct && over == NULL &&
						   !func_variadic && argnames == NIL &&
						   list_length(funcname) == 1 &&
						   (actual_arg_types[0] == RECORDOID ||
							ISCOMPLEX(actual_arg_types[0])));

	/*
	 * 若是列语法，先检查列投影情形。                 
	 */
	if (could_be_projection && is_column)
	{
		retval = ParseComplexProjection(pstate,
										strVal(linitial(funcname)),
										first_arg,
										location);
		if (retval)
			return retval;

		/*
		 * 若 ParseComplexProjection 不认为它是投影，            
		 * 则继续。  
		 */
	}

	/*
	 * func_get_detail 在系统目录中查找函数，为多态 
	 * 函数做消歧，处理继承，并返回函数返回值的      
	 * funcid、类型及集合/单例状态。                      
	 * 它还返回该函数真正的参数类型。                       
	 *              
	 *
	 * 注：对于命名记法或 variadic 的函数调用，报告的  
	 * "真实"类型并非 pg_proc 中的类型：类型会被重排        
	 * 以匹配命名参数的给定顺序，且 variadic 参数会被    
	 * 合适数量的元素类型副本所替代。下面会修正      
	 * variadic 情形。我们还可能需要处理默认参数。          
	 *                        
	 */

	setup_parser_errposition_callback(&pcbstate, pstate, location);

	fdresult = func_get_detail(funcname, fargs, argnames, nargs,
							   actual_arg_types,
							   !func_variadic, true, proc_call,
							   &funcid, &rettype, &retset,
							   &nvargs, &vatype,
							   &declared_arg_types, &argdefaults);

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * 检查各种例程类别错误的情形。    
	 */

	/* 若这是 CALL，则拒绝非过程的情形            */
	if (proc_call &&
		(fdresult == FUNCDETAIL_NORMAL ||
		 fdresult == FUNCDETAIL_AGGREGATE ||
		 fdresult == FUNCDETAIL_WINDOWFUNC ||
		 fdresult == FUNCDETAIL_COERCION))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a procedure",
						func_signature_string(funcname, nargs,
											  argnames,
											  actual_arg_types)),
				 errhint("To call a function, use SELECT."),
				 parser_errposition(pstate, location)));
	/* 反之，若不是 CALL，则拒绝过程    */
	if (fdresult == FUNCDETAIL_PROCEDURE && !proc_call)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a procedure",
						func_signature_string(funcname, nargs,
											  argnames,
											  actual_arg_types)),
				 errhint("To call a procedure, use CALL."),
				 parser_errposition(pstate, location)));

	if (fdresult == FUNCDETAIL_NORMAL ||
		fdresult == FUNCDETAIL_PROCEDURE ||
		fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * 在这些情形下，若有任何表明它必须是聚合或窗  
		 * 口函数的迹象就报错。     
		 */
		if (agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) specified, but %s is not an aggregate function",
							NameListToString(funcname),
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_distinct)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("DISTINCT specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("WITHIN GROUP specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_order != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("ORDER BY specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (agg_filter)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("FILTER specified, but %s is not an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		if (over)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("OVER specified, but %s is not a window function nor an aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
	}

	/*
	 * 一切顺利，下面做 fdresult 类型特定处理。      
	 */
	if (fdresult == FUNCDETAIL_NORMAL || fdresult == FUNCDETAIL_PROCEDURE)
	{
		/* 这些情形无需特殊处理。      */
	}
	else if (fdresult == FUNCDETAIL_AGGREGATE)
	{
		/*
		 * 这是聚合；从 pg_aggregate 条目获取所需信息。      
		 */
		HeapTuple	tup;
		Form_pg_aggregate classForm;
		int			catDirectArgs;

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcid));
		if (!HeapTupleIsValid(tup)) /* 不应发生      */
			elog(ERROR, "cache lookup failed for aggregate %u", funcid);
		classForm = (Form_pg_aggregate) GETSTRUCT(tup);
		aggkind = classForm->aggkind;
		catDirectArgs = classForm->aggnumdirectargs;
		ReleaseSysCache(tup);

		/* 现在检查各种禁止情形。   */
		if (AGGKIND_IS_ORDERED_SET(aggkind))
		{
			int			numAggregatedArgs;
			int			numDirectArgs;

			if (!agg_within_group)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("WITHIN GROUP is required for ordered-set aggregate %s",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
			if (over)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("OVER is not supported for ordered-set aggregate %s",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
			/* gram.y 拒绝 DISTINCT + WITHIN GROUP  */
			Assert(!agg_distinct);
			/* gram.y 拒绝 VARIADIC + WITHIN GROUP  */
			Assert(!func_variadic);

			/*
			 * 由于 func_get_detail 处理的是未区分的参数列表， 
			 * 它可能选出并不真正匹配的聚合，因为它需要  
			 * 直接与聚合参数有不同的划分。需检查直接参   
			 * 数的个数是否合适；若否，则抛出"未定义函
			 * 数"错误，与在普通聚合调用中误用 ORDER BY 的    
			 * 情形类似。                                                
			 *                             
			 */
			numAggregatedArgs = list_length(agg_order);
			numDirectArgs = nargs - numAggregatedArgs;
			Assert(numDirectArgs >= 0);

			if (!OidIsValid(vatype))
			{
				/* 若聚合非 variadic，测试很简单    */
				if (numDirectArgs != catDirectArgs)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("function %s does not exist",
									func_signature_string(funcname, nargs,
														  argnames,
														  actual_arg_types)),
							 errhint_plural("There is an ordered-set aggregate %s, but it requires %d direct argument, not %d.",
											"There is an ordered-set aggregate %s, but it requires %d direct arguments, not %d.",
											catDirectArgs,
											NameListToString(funcname),
											catDirectArgs, numDirectArgs),
							 parser_errposition(pstate, location)));
			}
			else
			{
				/*
				 * 若为 variadic，有两种情形，视聚合是        
				 * "... ORDER BY VARIADIC" 还是 "..., VARIADIC              
				 * ORDER BY VARIADIC"。若 catDirectArgs 等于         
				 * pronargs 则是后者；为省一次系统目录查找，从 
				 * func_get_detail 信息反推 pronargs。           
				 */
				int			pronargs;

				pronargs = nargs;
				if (nvargs > 1)
					pronargs -= nvargs - 1;
				if (catDirectArgs < pronargs)
				{
					/* VARIADIC 不属于直接参数，故仍简单     */
					if (numDirectArgs != catDirectArgs)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_FUNCTION),
								 errmsg("function %s does not exist",
										func_signature_string(funcname, nargs,
															  argnames,
															  actual_arg_types)),
								 errhint_plural("There is an ordered-set aggregate %s, but it requires %d direct argument, not %d.",
												"There is an ordered-set aggregate %s, but it requires %d direct arguments, not %d.",
												catDirectArgs,
												NameListToString(funcname),
												catDirectArgs, numDirectArgs),
								 parser_errposition(pstate, location)));
				}
				else
				{
					/*
					 * 直接参数与聚合参数都声明为 variadic。    
					 * 对标准有序集聚合而言，只要                
					 * 直接参数不致过少即可。          
					 * 对假想集聚合，假定假设参数就是 
					 * 匹配 variadic 参数；其个数须正好等于
					 * 聚合参数的个数。                              
					 *                                   
					 */
					if (aggkind == AGGKIND_HYPOTHETICAL)
					{
						if (nvargs != 2 * numAggregatedArgs)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(funcname, nargs,
																  argnames,
																  actual_arg_types)),
									 errhint("To use the hypothetical-set aggregate %s, the number of hypothetical direct arguments (here %d) must match the number of ordering columns (here %d).",
											 NameListToString(funcname),
											 nvargs - numAggregatedArgs, numAggregatedArgs),
									 parser_errposition(pstate, location)));
					}
					else
					{
						if (nvargs <= numAggregatedArgs)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(funcname, nargs,
																  argnames,
																  actual_arg_types)),
									 errhint_plural("There is an ordered-set aggregate %s, but it requires at least %d direct argument.",
													"There is an ordered-set aggregate %s, but it requires at least %d direct arguments.",
													catDirectArgs,
													NameListToString(funcname),
													catDirectArgs),
									 parser_errposition(pstate, location)));
					}
				}
			}

			/* 检查假设参数的类型匹配             */
			if (aggkind == AGGKIND_HYPOTHETICAL)
				unify_hypothetical_args(pstate, fargs, numAggregatedArgs,
										actual_arg_types, declared_arg_types);
		}
		else
		{
			/* 普通聚合，因此不能有 WITHIN GROUP     */
			if (agg_within_group)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("%s is not an ordered-set aggregate, so it cannot have WITHIN GROUP",
								NameListToString(funcname)),
						 parser_errposition(pstate, location)));
		}
	}
	else if (fdresult == FUNCDETAIL_WINDOWFUNC)
	{
		/*
		 * 真正的窗口函数必须随窗口定义一起调用。     
		 */
		if (!over)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("window function %s requires an OVER clause",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
		/* 而且按规范 WITHIN GROUP 不被允许 */
		if (agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("window function %s cannot have WITHIN GROUP",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));
	}
	else if (fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * 我们将其解释为类型转换。coerce_type 能处理这类   
		 * 情形，何必重复代码。 
		 */
		return coerce_type(pstate, linitial(fargs),
						   actual_arg_types[0], rettype, -1,
						   COERCION_EXPLICIT, COERCE_EXPLICIT_CALL, location);
	}
	else if (fdresult == FUNCDETAIL_MULTIPLE)
	{
		/*
		 * 我们找到了多个可能的函数匹配。若处理的是属  
		 * 性记法，则返回失败，让调用者报告"无此列"       
		 * （我们已确定没有这样的列）。若处理的是函  
		 * 数记法，则报告"函数有歧义"，无论是否还有 
		 * 同名的列。                                          
		 */
		if (is_column)
			return NULL;

		if (proc_call)
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("procedure %s is not unique",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("Could not choose a best candidate procedure. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("function %s is not unique",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("Could not choose a best candidate function. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
	}
	else
	{
		/*
		 * 未找到函数。若是属性记法则返回失败，    
		 * 让调用者报告"无此列"（已确定没有这样       
		 * 的列）。                                     
		 */
		if (is_column)
			return NULL;

		/*
		 * 由于之前没检查过，现在检查列投影解释。          
		 */
		if (could_be_projection)
		{
			retval = ParseComplexProjection(pstate,
											strVal(linitial(funcname)),
											first_arg,
											location);
			if (retval)
				return retval;
		}

		/*
		 * 既无函数也无列。既然处理的是函数记法，   
		 * 报告"函数不存在"。                          
		 */
		if (list_length(agg_order) > 1 && !agg_within_group)
		{
			/* 这是 agg(x, ORDER BY y,z)，ORDER BY 或许错位      */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No aggregate function matches the given name and argument types. "
							 "Perhaps you misplaced ORDER BY; ORDER BY must appear "
							 "after all regular arguments of the aggregate."),
					 parser_errposition(pstate, location)));
		}
		else if (proc_call)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("procedure %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No procedure matches the given name and argument types. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(funcname, nargs, argnames,
												  actual_arg_types)),
					 errhint("No function matches the given name and argument types. "
							 "You might need to add explicit type casts."),
					 parser_errposition(pstate, location)));
	}

	/*
	 * 若有默认参数，为检查通用类型一致性，须把它  
	 * 们的类型计入 actual_arg_types。但我们绝不把它们        
	 * 放进生成的解析节点，因为在查询运行之前其实际
	 * 值可能改变。规划器必须在计划时插入最新值。 
	 *                                                          
	 */
	nargsplusdefs = nargs;
	foreach(l, argdefaults)
	{
		Node	   *expr = (Node *) lfirst(l);

		/* 或许不该发生……      */
		if (nargsplusdefs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("cannot pass more than %d argument to a function",
								   "cannot pass more than %d arguments to a function",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS),
					 parser_errposition(pstate, location)));

		actual_arg_types[nargsplusdefs++] = exprType(expr);
	}

	/*
	 * 强制与多态参数及返回类型保持一致，            
	 * 可能调整返回类型或 declared_arg_types（后者将被      
	 * make_fn_arguments 用作转换目标）           
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargsplusdefs,
											   rettype,
											   false);

	/* 对参数执行必要的类型转换           */
	make_fn_arguments(pstate, fargs, actual_arg_types, declared_arg_types);

	/*
	 * 若函数实际上并非 variadic，则忽略调用上的任何        
	 * VARIADIC 修饰。（或许应改为抛错，但历来      
	 * 我们都允许这么写。）                   
	 */
	if (!OidIsValid(vatype))
	{
		Assert(nvargs == 0);
		func_variadic = false;
	}

	/*
	 * 若是 variadic 函数调用，则把最后 nvargs 个参数         
	 * 转换成数组 —— 除非是"any"变参。  
	 */
	if (nvargs > 0 && vatype != ANYOID)
	{
		ArrayExpr  *newa = makeNode(ArrayExpr);
		int			non_var_args = nargs - nvargs;
		List	   *vargs;

		Assert(non_var_args >= 0);
		vargs = list_copy_tail(fargs, non_var_args);
		fargs = list_truncate(fargs, non_var_args);

		newa->elements = vargs;
		/* 假定所有 variadic 参数都已被转换为同一类型      */
		newa->element_typeid = exprType((Node *) linitial(vargs));
		newa->array_typeid = get_array_type(newa->element_typeid);
		if (!OidIsValid(newa->array_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(newa->element_typeid)),
					 parser_errposition(pstate, exprLocation((Node *) vargs))));
		/* array_collid 将由 parse_collate.c 设置  */
		newa->multidims = false;
		newa->location = exprLocation((Node *) vargs);

		fargs = lappend(fargs, newa);

		/* 此前不可能已有 VARIADIC 标记……       */
		Assert(!func_variadic);
		/* ……但现在是 VARIADIC 调用*/
		func_variadic = true;
	}

	/*
	 * 若"any"变参函数带着显式 VARIADIC 标记被调用，         
	 * 则坚持变参参数必须是数组类型。     
	 */
	if (nargs > 0 && vatype == ANYOID && func_variadic)
	{
		Oid			va_arr_typid = actual_arg_types[nargs - 1];

		if (!OidIsValid(get_base_element_type(va_arr_typid)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("VARIADIC argument must be an array"),
					 parser_errposition(pstate,
										exprLocation((Node *) llast(fargs)))));
	}

	/* 若返回集合，检查是否允许 */
	if (retset)
		check_srf_call_placement(pstate, last_srf, location);

	/* 构建适当的输出结构            */
	if (fdresult == FUNCDETAIL_NORMAL || fdresult == FUNCDETAIL_PROCEDURE)
	{
		FuncExpr   *funcexpr = makeNode(FuncExpr);

		funcexpr->funcid = funcid;
		funcexpr->funcresulttype = rettype;
		funcexpr->funcretset = retset;
		funcexpr->funcvariadic = func_variadic;
		funcexpr->funcformat = funcformat;
		/* funccollid 与 inputcollid 将由 parse_collate.c 设置  */
		funcexpr->args = fargs;
		funcexpr->location = location;

		retval = (Node *) funcexpr;
	}
	else if (fdresult == FUNCDETAIL_AGGREGATE && !over)
	{
		/* 聚合函数       */
		Aggref	   *aggref = makeNode(Aggref);

		aggref->aggfnoid = funcid;
		aggref->aggtype = rettype;
		/* aggcollid 与 inputcollid 将由 parse_collate.c 设置  */
		aggref->aggtranstype = InvalidOid;	/* 将由规划器设置  */
		/* aggargtypes 将由 transformAggregateCall 设置  */
		/* aggdirectargs 与 args 将由 transformAggregateCall 设置  */
		/* aggorder 与 aggdistinct 将由 transformAggregateCall 设置  */
		aggref->aggfilter = agg_filter;
		aggref->aggstar = agg_star;
		aggref->aggvariadic = func_variadic;
		aggref->aggkind = aggkind;
		aggref->aggpresorted = false;
		/* agglevelsup 将由 transformAggregateCall 设置  */
		aggref->aggsplit = AGGSPLIT_SIMPLE; /* 规划器可能改变它  */
		aggref->aggno = -1;		/* 规划器将设置 aggno/aggtransno   */
		aggref->aggtransno = -1;
		aggref->location = location;

		/*
		 * 拒绝不带 (*) 语法就调用无参数聚合的企图。 
		 * 这只是咬文嚼字，但有些人坚持要这样…… 
		 */
		if (fargs == NIL && !agg_star && !agg_within_group)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) must be used to call a parameterless aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregates cannot return sets"),
					 parser_errposition(pstate, location)));

		/*
		 * 或许以后会想支持命名参数，但目前不允许。       
		 * 我们需要确定解析表示（NamedArgExprs 应放在           
		 * TargetEntry 节点之上还是之下？）再教规划          
		 * 器正确重排该列表。或者也许可以让                  
		 * transformAggregateCall 来做？不过，若还想允许         
		 * 聚合使用默认参数，就需要在规划阶段做，以    
		 * 避免语义问题。               
		 */
		if (argnames != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregates cannot use named arguments"),
					 parser_errposition(pstate, location)));

		/* parse_agg.c 做额外的聚合特定处理                */
		transformAggregateCall(pstate, aggref, fargs, agg_order, agg_distinct);

		retval = (Node *) aggref;
	}
	else
	{
		/* 窗口函数    */
		WindowFunc *wfunc = makeNode(WindowFunc);

		Assert(over);			/* 上文已检查过此项缺失 */
		Assert(!agg_within_group);	/* 上文已检查    */

		wfunc->winfnoid = funcid;
		wfunc->wintype = rettype;
		/* wincollid 与 inputcollid 将由 parse_collate.c 设置  */
		wfunc->args = fargs;
		/* winref 将由 transformWindowFuncCall 设置  */
		wfunc->winstar = agg_star;
		wfunc->winagg = (fdresult == FUNCDETAIL_AGGREGATE);
		wfunc->aggfilter = agg_filter;
		wfunc->location = location;

		/*
		 * agg_star 允许用于聚合，distinct 则不行              
		 */
		if (agg_distinct)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DISTINCT is not implemented for window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * 拒绝不带 (*) 语法就调用无参数聚合的企图。 
		 * 这只是咬文嚼字，但有些人坚持要这样…… 
		 */
		if (wfunc->winagg && fargs == NIL && !agg_star)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s(*) must be used to call a parameterless aggregate function",
							NameListToString(funcname)),
					 parser_errposition(pstate, location)));

		/*
		 * 窗口中尚不允许有序聚合      
		 */
		if (agg_order != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregate ORDER BY is not implemented for window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * 真正的窗口函数尚不支持 FILTER              
		 */
		if (!wfunc->winagg && agg_filter)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("FILTER is not implemented for non-aggregate window functions"),
					 parser_errposition(pstate, location)));

		/*
		 * 窗口函数既不能接受也不能返回集合 
		 */
		if (pstate->p_last_srf != last_srf)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("window function calls cannot contain set-returning function calls"),
					 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
					 parser_errposition(pstate,
										exprLocation(pstate->p_last_srf))));

		if (retset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("window functions cannot return sets"),
					 parser_errposition(pstate, location)));

		/* parse_agg.c 做额外的窗口函数特定处理            */
		transformWindowFuncCall(pstate, wfunc, over);

		retval = (Node *) wfunc;
	}

	/* 若返回集合，记下它供上层错误检查                   */
	if (retset)
		pstate->p_last_srf = retval;

	return retval;
}


/* func_match_argtypes()
 *
 * 给定一份候选函数列表（名字与参数个数都对）      
 * 和一个输入数据类型 OID 数组，产出一个实际接           
 * 受这些输入类型的（或经强制转换后接受的）候          
 * 选函数短名单，并返回其数量。                
 *
 * 注意 can_coerce_type 会假定 UNKNOWN 输入可转换                  
 * 为任何类型，因此不会据此排除候选。          
 *
 * 注意：只要找到至少一个匹配，修改输入列表结     
 * 构没问题。若完全无匹配，列表须保持原样。    
 */
int
func_match_argtypes(int nargs,
					Oid *input_typeids,
					FuncCandidateList raw_candidates,
					FuncCandidateList *candidates)	/* 返回值    */
{
	FuncCandidateList current_candidate;
	FuncCandidateList next_candidate;
	int			ncandidates = 0;

	*candidates = NULL;

	for (current_candidate = raw_candidates;
		 current_candidate != NULL;
		 current_candidate = next_candidate)
	{
		next_candidate = current_candidate->next;
		if (can_coerce_type(nargs, input_typeids, current_candidate->args,
							COERCION_IMPLICIT))
		{
			current_candidate->next = *candidates;
			*candidates = current_candidate;
			ncandidates++;
		}
	}

	return ncandidates;
}								/* func_match_argtypes() */


/* func_select_candidate()
 *		给定输入参数类型数组与多个函数候选，   
 *		尝试解决冲突。                             
 *
 * 若冲突可以解决，则返回选出的候选，            
 * 否则返回 NULL。   
 *
 * 注意调用者已确定没有候选与输入参数类型完全       
 * 匹配，并已剪除任何实际上与输入类型不可强制          
 * 转换兼容的"候选"。                                    
 *
 * 它也用于消解有歧义的操作符引用。以前                  
 * parse_oper.c 为此目的有一份本质重复的代码。           
 * 下面的注释（原在 parse_oper.c 中）被保留下来，           
 * 记录这些启发式的历史。  
 *
 * OLD COMMENTS:
 *
 * 本例程是新代码，取代了源自 v4.2/v1.0.x 时代的     
 * binary_oper_select_candidate()。它竭尽全力去匹           
 * 配操作符与类型，必要时还允许类型强制转换。      
 * 重要的是代码要尽可能多做，                     
 * 但绝不做错事；所谓"错事"指的是在存在更佳       
 * 选择时仍返回某操作符，或返回非直观可能性的  
 * 操作符。                                                  
 * - thomas 1998-05-21
 *
 * 下面的注释来自 binary_oper_select_candidate()，         
 * 展示了可能出现的问题与选择：           
 * - thomas 1998-05-20
 *
 * 当时的普遍观点是：默认操作符应当是两个操作      
 * 数同类型的操作符（这样的操作符只有一个） 
 *          
 *
 * 7.27.93 —— 我决定不这样做；太难自圆其说，          
 * 显式类型转换足够容易 —— avi    
 * [本例程其余部分自那时起已注释掉 —— ay]   
 *
 * 6/23/95 —— 我不完全同意 avi。尤其对用户来说， 
 * 浮点转换是件麻烦事。不管不这样做的理由是什    
 * 么，我需要以下特例能够工作。         
 *
 * 在查询的 WHERE 子句中，若浮点数没加引号，我  
 * 们将其当作 float8。我加入了 float48* 操作符，     
 * 以便能对 float4 与 float8 运算。但现在若右参数      
 * 未知（如带引号的浮点数），匹配操作符就有
 * 不止一个。这破坏了回归测试中一些内容，那里
 * 有未正确转换的带引号浮点数。解决办法如下。    
 * 除了要求操作符对两个操作数用同一类型［如同    
 * Avi 最初注释掉的代码］，还要求它们在某种
 * 意义上等价（详见 equivalentOpersAfterPromotion）。       
 *                                                             
 * - ay 6/95
 */
FuncCandidateList
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates)
{
	FuncCandidateList current_candidate,
				first_candidate,
				last_candidate;
	Oid		   *current_typeids;
	Oid			current_type;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch,
				nunknowns;
	Oid			input_base_typeids[FUNC_MAX_ARGS];
	TYPCATEGORY slot_category[FUNC_MAX_ARGS],
				current_category;
	bool		current_is_preferred;
	bool		slot_has_preferred_type[FUNC_MAX_ARGS];
	bool		resolved_unknowns;

	/* 保护本地定长数组        */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/*
	 * 若任何输入类型是域，则将其约减为基类型。这      
	 * 保证精确匹配启发式中基类型上的函数算作"精确     
	 * 匹配"；也使类型类别启发式能发挥作用。注意         
	 * 这使得使用声明为接受域的函数变得困难，但并   
	 * 非不可能。这样的函数只有在所有参数位置都是     
	 * 精确匹配时才胜过基类型函数，调用者早已            
	 * 把它选了出来。                                          
	 *                                                             
	 *
	 * 趁此机会，顺便统计未知类型参数个数备用            
	 *       
	 */
	nunknowns = 0;
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] != UNKNOWNOID)
			input_base_typeids[i] = getBaseType(input_typeids[i]);
		else
		{
			/* 对 UNKNOWNOID 无需调用 getBaseType   */
			input_base_typeids[i] = UNKNOWNOID;
			nunknowns++;
		}
	}

	/*
	 * 遍历所有候选，保留精确类型匹配数最多的那些。
	 * 若无匹配则保留全部候选。           
	 */
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] != UNKNOWNOID &&
				current_typeids[i] == input_base_typeids[i])
				nmatch++;
		}

		/* 把它当作迄今最佳的选择？     */
		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		/* 不比上一个选择差，所以也保留这个？  */
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
		/* 否则就不必保留这个……           */
	}

	if (last_candidate)			/* 结束重建列表     */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates;

	/*
	 * 候选仍然过多？现在寻找那些在需要强制转换的     
	 * 参数上有精确或首选类型匹配的候选。（7.4     
	 * 新增限制：首选类型必须与输入类型同类；对跨类 
	 * 别转换为首选类型不优先。）若无匹配则保留
	 * 全部候选。                                                     
	 */
	for (i = 0; i < nargs; i++) /* 避免多次查找     */
		slot_category[i] = TypeCategory(input_base_typeids[i]);
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] != UNKNOWNOID)
			{
				if (current_typeids[i] == input_base_typeids[i] ||
					IsPreferredType(slot_category[i], current_typeids[i]))
					nmatch++;
			}
		}

		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
	}

	if (last_candidate)			/* 结束重建列表     */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates;

	/*
	 * 候选仍然过多？尝试为未知输入指定类型。              
	 *
	 * 若无未知输入，则已无适用的启发式，                   
	 * 必须失败  
	 */
	if (nunknowns == 0)
		return NULL;			/* 未能选出最佳候选          */

	/*
	 * 下一步检查每个未知参数位置，看能否确定它的       
	 * "类型类别"。若任何候选有 STRING 类别的输入        
	 * 数据类型，则使用 STRING 类别（偏向 STRING 是         
	 * 合适的，因为未知类型字面量看起来像字符串）。   
	 * 否则，若所有候选都认同该参数位置的类别，       
	 * 就使用该类别。否则失败，因为无法确定类别。  
	 *                             
	 *
	 * 若能确定类别，还要留意是否有候选在该类别           
	 * 中接受首选数据类型。                                
	 *
	 * 检查完毕后，删除在任何未知位置接受错误类别     
	 * 的候选。若至少有一个候选在某位置接受        
	 * 了首选类型，则删除接受非首选类型的候选。若     
	 * 只剩一个候选，则返回它。但如果这条规则拒绝        
	 * 了所有候选，则改为保留全部。                          
	 *                       
	 */
	resolved_unknowns = false;
	for (i = 0; i < nargs; i++)
	{
		bool		have_conflict;

		if (input_base_typeids[i] != UNKNOWNOID)
			continue;
		resolved_unknowns = true;	/* 假定能做到     */
		slot_category[i] = TYPCATEGORY_INVALID;
		slot_has_preferred_type[i] = false;
		have_conflict = false;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			current_typeids = current_candidate->args;
			current_type = current_typeids[i];
			get_type_category_preferred(current_type,
										&current_category,
										&current_is_preferred);
			if (slot_category[i] == TYPCATEGORY_INVALID)
			{
				/* 首个候选    */
				slot_category[i] = current_category;
				slot_has_preferred_type[i] = current_is_preferred;
			}
			else if (current_category == slot_category[i])
			{
				/* 同类别中更多候选         */
				slot_has_preferred_type[i] |= current_is_preferred;
			}
			else
			{
				/* 类别冲突！    */
				if (current_category == TYPCATEGORY_STRING)
				{
					/* STRING 可用时总胜出       */
					slot_category[i] = current_category;
					slot_has_preferred_type[i] = current_is_preferred;
				}
				else
				{
					/*
					 * 记住冲突，但继续（可能找到 STRING）    
					 */
					have_conflict = true;
				}
			}
		}
		if (have_conflict && slot_category[i] != TYPCATEGORY_STRING)
		{
			/* 未能在此位置解决类别冲突                 */
			resolved_unknowns = false;
			break;
		}
	}

	if (resolved_unknowns)
	{
		/* 剔除不匹配的候选      */
		ncandidates = 0;
		first_candidate = candidates;
		last_candidate = NULL;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			bool		keepit = true;

			current_typeids = current_candidate->args;
			for (i = 0; i < nargs; i++)
			{
				if (input_base_typeids[i] != UNKNOWNOID)
					continue;
				current_type = current_typeids[i];
				get_type_category_preferred(current_type,
											&current_category,
											&current_is_preferred);
				if (current_category != slot_category[i])
				{
					keepit = false;
					break;
				}
				if (slot_has_preferred_type[i] && !current_is_preferred)
				{
					keepit = false;
					break;
				}
			}
			if (keepit)
			{
				/* 保留此候选     */
				last_candidate = current_candidate;
				ncandidates++;
			}
			else
			{
				/* 丢弃此候选       */
				if (last_candidate)
					last_candidate->next = current_candidate->next;
				else
					first_candidate = current_candidate->next;
			}
		}

		/* 若已找到任何匹配，则只关注这些匹配      */
		if (last_candidate)
		{
			candidates = first_candidate;
			/* 结束重建列表     */
			last_candidate->next = NULL;
		}

		if (ncandidates == 1)
			return candidates;
	}

	/*
	 * 最后手段：若输入同时存在已知与未知类型，        
	 * 且所有已知类型相同，则假定未知输入也为该类型，
	 * 再看能否据此得到唯一匹配；若能，则采用该匹配。 
	 *
	 * 注：对于一端未知、另一端非未知的二元操作符，     
	 * binary_oper_exact() 中已试过此启发式；但该代码           
	 * 只找精确匹配，而这里还会处理涉及强制转换、        
	 * 多态类型解析等情况。                     
	 */
	if (nunknowns < nargs)
	{
		Oid			known_type = UNKNOWNOID;

		for (i = 0; i < nargs; i++)
		{
			if (input_base_typeids[i] == UNKNOWNOID)
				continue;
			if (known_type == UNKNOWNOID)	/* 首已知参数  */
				known_type = input_base_typeids[i];
			else if (known_type != input_base_typeids[i])
			{
				/* 并非全部匹配  */
				known_type = UNKNOWNOID;
				break;
			}
		}

		if (known_type != UNKNOWNOID)
		{
			/* 仅一种已知类型，应用该启发式     */
			for (i = 0; i < nargs; i++)
				input_base_typeids[i] = known_type;
			ncandidates = 0;
			last_candidate = NULL;
			for (current_candidate = candidates;
				 current_candidate != NULL;
				 current_candidate = current_candidate->next)
			{
				current_typeids = current_candidate->args;
				if (can_coerce_type(nargs, input_base_typeids, current_typeids,
									COERCION_IMPLICIT))
				{
					if (++ncandidates > 1)
						break;	/* 不唯一，放弃  */
					last_candidate = current_candidate;
				}
			}
			if (ncandidates == 1)
			{
				/* 成功找到唯一匹配               */
				last_candidate->next = NULL;
				return last_candidate;
			}
		}
	}

	return NULL;				/* 未能选出最佳候选          */
}								/* func_select_candidate() */


/* func_get_detail()
 *
 * 在系统目录中查找命名函数。        
 *
 * 尝试在系统目录中找到实参与指定完全一致的  
 * 函数，使常见情形（精确匹配）尽可能快。            
 *                         
 *
 * 若未找到精确匹配：   
 *	1) 检查是否可解释为类型强制转换请求            
 *	2) 应用歧义函数消解规则               
 *
 * *funcid 至 *true_typeids 各返回值接收该函数的信息。           
 * 若 argdefaults 非 NULL，*argdefaults 接收需要添加到         
 * 给定实参的默认参数表达式列表。                     
 *
 * 处理命名记法或混合记法调用时（即 fargnames 非 NIL），  
 * 返回的 true_typeids 与 argdefaults 按调用实参顺序排列： 
 * 先是位置实参，再是命名实参，然后是带默认值的实参
 * （若需要且 expand_defaults 允许）。                 
 * 若这些信息要与函数的 pg_proc 条目做比较，需加小心；    
 * 但实践中调用方通常只需按调用实参顺序处理。        
 *                                             
 *
 * 我们主要依据 fargnames/nargs/argtypes 来描述实参。            
 * 实际表达式列表经 fargs 传入，以便检查常量是否被    
 * 强制转换。部分调用方传入 fargs == NIL，表示不需要        
 * 做该项检查。另请注意，当 fargnames 非 NIL 时，             
 * 若调用方希望将实际实参位置信息回填到 NamedArgExpr       
 * 节点，就必须传入 fargs 列表。               
 */
FuncDetailCode
func_get_detail(List *funcname,
				List *fargs,
				List *fargnames,
				int nargs,
				Oid *argtypes,
				bool expand_variadic,
				bool expand_defaults,
				bool include_out_arguments,
				Oid *funcid,	/* 返回值    */
				Oid *rettype,	/* 返回值    */
				bool *retset,	/* 返回值    */
				int *nvargs,	/* 返回值    */
				Oid *vatype,	/* 返回值    */
				Oid **true_typeids, /* 返回值    */
				List **argdefaults) /* 可选返回值       */
{
	FuncCandidateList raw_candidates;
	FuncCandidateList best_candidate;

	/* 初始化输出参数，以免编译器警告            */
	*funcid = InvalidOid;
	*rettype = InvalidOid;
	*retset = false;
	*nvargs = 0;
	*vatype = InvalidOid;
	*true_typeids = NULL;
	if (argdefaults)
		*argdefaults = NIL;

	/* 从命名空间搜索获取候选列表               */
	raw_candidates = FuncnameGetCandidates(funcname, nargs, fargnames,
										   expand_variadic, expand_defaults,
										   include_out_arguments, false);

	/*
	 * 快速检查是否存在与输入数据类型精确匹配的函数    
	 * （仅一个） 
	 */
	for (best_candidate = raw_candidates;
		 best_candidate != NULL;
		 best_candidate = best_candidate->next)
	{
		/* 若 nargs==0，argtypes 可为 NULL；勿将其传给 memcmp */
		if (nargs == 0 ||
			memcmp(argtypes, best_candidate->args, nargs * sizeof(Oid)) == 0)
			break;
	}

	if (best_candidate == NULL)
	{
		/*
		 * 若未找到精确匹配，接下来考虑：这其实是一次
		 * 类型强制转换请求：单参数调用，函数名是     
		 * 一个类型名。如果强制转换路径为 RELABELTYPE 或    
		 * COERCEVIAIO，就将"函数调用"当作强制转换处理。    
		 *                                             
		 *
		 * 这种解释必须高于"先转换再调用"的解释，  
		 * 否则可能产生意外结果。                               
		 * 例如，希望 "text(varchar)" 被解释为简单强制转换， 
		 * 而不是 "text(name(varchar))"——此点之下的代码完全   
		 * 有能力选出该解释。                                      
		 *                      
		 *
		 * 先前未知类型字面常量到具体类型的转换，也 
		 * 按此方式处理。                
		 *
		 * 此处拒绝 COERCION_PATH_FUNC 的原因在于：我们预期    
		 * 转换实现函数会以目标类型命名，因此若合适，
		 * 普通的函数查找即可找到该函数。                   
		 *
		 * 拒绝 COERCION_PATH_ARRAYCOERCE 主要是因为无法把        
		 * "foo[] (something)" 写成函数调用。理论上或许有人
		 * 想以 "_foo (something)" 的形式调用它，但我们历来   
		 * 从未支持过这种写法，故可坚持要求人们改用普
		 * 通的强制转换语法。       
		 *
		 * 我们还拒绝复合类型源、字符串类别目标的      
		 * COERCEVIAIO 特定情形。find_coercion_pathway() 默认          
		 * 允许这种情况，但经验表明该情形被误用得太频繁 
		 * 了。因此同样地，若人们想这样做，就坚持要求其
		 * 改用强制转换语法。                    
		 *
		 * 注意：此代码的行为绝不能超出 coerce_type 所能做  
		 * 的范围，因为若我们返回 FUNCDETAIL_COERCION，调用
		 * 方会尝试应用 coerce_type。若对 coerce_type 无法处理   
		 * 的情形返回该结果，会在本模块与 coerce_type 之间
		 * 造成无限递归！       
		 */
		if (nargs == 1 && fargs != NIL && fargnames == NIL)
		{
			Oid			targetType = FuncNameAsType(funcname);

			if (OidIsValid(targetType))
			{
				Oid			sourceType = argtypes[0];
				Node	   *arg1 = linitial(fargs);
				bool		iscoercion;

				if (sourceType == UNKNOWNOID && IsA(arg1, Const))
				{
					/* 总把 typename('literal') 视为强制转换*/
					iscoercion = true;
				}
				else
				{
					CoercionPathType cpathtype;
					Oid			cfuncid;

					cpathtype = find_coercion_pathway(targetType, sourceType,
													  COERCION_EXPLICIT,
													  &cfuncid);
					switch (cpathtype)
					{
						case COERCION_PATH_RELABELTYPE:
							iscoercion = true;
							break;
						case COERCION_PATH_COERCEVIAIO:
							if ((sourceType == RECORDOID ||
								 ISCOMPLEX(sourceType)) &&
								TypeCategory(targetType) == TYPCATEGORY_STRING)
								iscoercion = false;
							else
								iscoercion = true;
							break;
						default:
							iscoercion = false;
							break;
					}
				}

				if (iscoercion)
				{
					/* 按类型强制转换处理 */
					*funcid = InvalidOid;
					*rettype = targetType;
					*retset = false;
					*nvargs = 0;
					*vatype = InvalidOid;
					*true_typeids = argtypes;
					return FUNCDETAIL_COERCION;
				}
			}
		}

		/*
		 * 未找到精确匹配，现在开始尝试匹配各候选…    
		 */
		if (raw_candidates != NULL)
		{
			FuncCandidateList current_candidates;
			int			ncandidates;

			ncandidates = func_match_argtypes(nargs,
											  argtypes,
											  raw_candidates,
											  &current_candidates);

			/* 仅一个匹配？那就采用它…*/
			if (ncandidates == 1)
				best_candidate = current_candidates;

			/*
			 * 多个候选？那最好做出决定，否则报错…      
			 */
			else if (ncandidates > 1)
			{
				best_candidate = func_select_candidate(nargs,
													   argtypes,
													   current_candidates);

				/*
				 * 若能选出最佳候选，就完成了。             
				 * 否则为歧义函数调用。     
				 */
				if (!best_candidate)
					return FUNCDETAIL_MULTIPLE;
			}
		}
	}

	if (best_candidate)
	{
		HeapTuple	ftup;
		Form_pg_proc pform;
		FuncDetailCode result;

		/*
		 * 若正在处理命名实参、展开可变参数或默认值， 
		 * "最佳候选"可能代表多个同等优良的函数；   
		 * 将此情形视为歧义。             
		 */
		if (!OidIsValid(best_candidate->oid))
			return FUNCDETAIL_MULTIPLE;

		/*
		 * 除非最后一个实参（附带 VARIADIC 的那个）确实匹  
		 * 配可变参数，否则不允许 VARIADIC 与命名实参。 
		 * 这纯属吹毛求疵，但当初有人坚持如此。             
		 */
		if (fargnames != NIL && !expand_variadic && nargs > 0 &&
			best_candidate->argnumbers[nargs - 1] != nargs - 1)
			return FUNCDETAIL_NOTFOUND;

		*funcid = best_candidate->oid;
		*nvargs = best_candidate->nvargs;
		*true_typeids = best_candidate->args;

		/*
		 * 若处理命名实参，则把实际实参位置回填到      
		 * fargs 列表的 NamedArgExpr 节点中。这做法有点丑，   
		 * 但换别的方式需要额外记法，不值得。  
		 */
		if (best_candidate->argnumbers != NULL)
		{
			int			i = 0;
			ListCell   *lc;

			foreach(lc, fargs)
			{
				NamedArgExpr *na = (NamedArgExpr *) lfirst(lc);

				if (IsA(na, NamedArgExpr))
					na->argnumber = best_candidate->argnumbers[i];
				i++;
			}
		}

		ftup = SearchSysCache1(PROCOID,
							   ObjectIdGetDatum(best_candidate->oid));
		if (!HeapTupleIsValid(ftup))	/* 不应发生      */
			elog(ERROR, "cache lookup failed for function %u",
				 best_candidate->oid);
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*rettype = pform->prorettype;
		*retset = pform->proretset;
		*vatype = pform->provariadic;
		/* 调用方需要时取回默认参数   */
		if (argdefaults && best_candidate->ndargs > 0)
		{
			Datum		proargdefaults;
			char	   *str;
			List	   *defaults;

			/* 不应发生：FuncnameGetCandidates 出了错    */
			if (best_candidate->ndargs > pform->pronargdefaults)
				elog(ERROR, "not enough default arguments");

			proargdefaults = SysCacheGetAttrNotNull(PROCOID, ftup,
													Anum_pg_proc_proargdefaults);
			str = TextDatumGetCString(proargdefaults);
			defaults = castNode(List, stringToNode(str));
			pfree(str);

			/* 从返回列表中删除未用到的默认参数  */
			if (best_candidate->argnumbers != NULL)
			{
				/*
				 * 命名记法下这有点棘手，因为给定实参可能 
				 * 替换默认参数中的任意子集。                
				 * 做法是先为默认参数的参数号建 bitmapset，  
				 * 然后扫描默认参数列表，选出所需的项。  
				 * （假定默认参数应按其位置顺序提供。）   
				 *                                               
				 */
				Bitmapset  *defargnumbers;
				int		   *firstdefarg;
				List	   *newdefaults;
				ListCell   *lc;
				int			i;

				defargnumbers = NULL;
				firstdefarg = &best_candidate->argnumbers[best_candidate->nargs - best_candidate->ndargs];
				for (i = 0; i < best_candidate->ndargs; i++)
					defargnumbers = bms_add_member(defargnumbers,
												   firstdefarg[i]);
				newdefaults = NIL;
				i = best_candidate->nominalnargs - pform->pronargdefaults;
				foreach(lc, defaults)
				{
					if (bms_is_member(i, defargnumbers))
						newdefaults = lappend(newdefaults, lfirst(lc));
					i++;
				}
				Assert(list_length(newdefaults) == best_candidate->ndargs);
				bms_free(defargnumbers);
				*argdefaults = newdefaults;
			}
			else
			{
				/*
				 * 位置记法下容易得多；只需删去前端的   
				 * 多余默认参数。                   
				 */
				int			ndelete;

				ndelete = list_length(defaults) - best_candidate->ndargs;
				if (ndelete > 0)
					defaults = list_delete_first_n(defaults, ndelete);
				*argdefaults = defaults;
			}
		}

		switch (pform->prokind)
		{
			case PROKIND_AGGREGATE:
				result = FUNCDETAIL_AGGREGATE;
				break;
			case PROKIND_FUNCTION:
				result = FUNCDETAIL_NORMAL;
				break;
			case PROKIND_PROCEDURE:
				result = FUNCDETAIL_PROCEDURE;
				break;
			case PROKIND_WINDOW:
				result = FUNCDETAIL_WINDOWFUNC;
				break;
			default:
				elog(ERROR, "unrecognized prokind: %c", pform->prokind);
				result = FUNCDETAIL_NORMAL; /* 让编译器安静  */
				break;
		}

		ReleaseSysCache(ftup);
		return result;
	}

	return FUNCDETAIL_NOTFOUND;
}


/*
 * unify_hypothetical_args()
 *
 * 确保假想集聚合的每个假设直接实参都与其对应    
 * 的聚合实参类型相同。若有必要，修改 fargs 列表中  
 * 的表达式，并更新 actual_arg_types[]。                    
 *                    
 *
 * 若聚合声明的实参类型非 ANY（哪怕是 ANYELEMENT），    
 * 则只需做一致性检查确认声明类型匹配即可；             
 * make_fn_arguments 会把实际实参强制转换为声明的类型。   
 * 但若声明是 ANY，make_fn_arguments 中不会发生任何转       
 * 换，任何不匹配均需在此修复。解析同 UNION 等。   
 */
static void
unify_hypothetical_args(ParseState *pstate,
						List *fargs,
						int numAggregatedArgs,
						Oid *actual_arg_types,
						Oid *declared_arg_types)
{
	int			numDirectArgs,
				numNonHypotheticalArgs;
	int			hargpos;

	numDirectArgs = list_length(fargs) - numAggregatedArgs;
	numNonHypotheticalArgs = numDirectArgs - numAggregatedArgs;
	/* 安全检查（应只在聚合声明有误时触发）    */
	if (numNonHypotheticalArgs < 0)
		elog(ERROR, "incorrect number of arguments to hypothetical-set aggregate");

	/* 检查每个假设实参及其对应的聚合实参          */
	for (hargpos = numNonHypotheticalArgs; hargpos < numDirectArgs; hargpos++)
	{
		int			aargpos = numDirectArgs + (hargpos - numNonHypotheticalArgs);
		ListCell   *harg = list_nth_cell(fargs, hargpos);
		ListCell   *aarg = list_nth_cell(fargs, aargpos);
		Oid			commontype;
		int32		commontypmod;

		/* 不匹配意味着 AggregateCreate 没有正确检查…   */
		if (declared_arg_types[hargpos] != declared_arg_types[aargpos])
			elog(ERROR, "hypothetical-set aggregate has inconsistent declared argument types");

		/* make_fn_arguments 会强制转换时无需统一  */
		if (declared_arg_types[hargpos] != ANYOID)
			continue;

		/*
		 * 选择公共类型，偏向聚合实参的类型（宁可对直   
		 * 接实参强制转换一次，也不愿强制转换全部聚合  
		 * 值）。              
		 */
		commontype = select_common_type(pstate,
										list_make2(lfirst(aarg), lfirst(harg)),
										"WITHIN GROUP",
										NULL);
		commontypmod = select_common_typmod(pstate,
											list_make2(lfirst(aarg), lfirst(harg)),
											commontype);

		/*
		 * 执行强制转换。无需担心 NamedArgExpr，因为聚合不  
		 * 支持它们。                                    
		 */
		lfirst(harg) = coerce_type(pstate,
								   (Node *) lfirst(harg),
								   actual_arg_types[hargpos],
								   commontype, commontypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
		actual_arg_types[hargpos] = commontype;
		lfirst(aarg) = coerce_type(pstate,
								   (Node *) lfirst(aarg),
								   actual_arg_types[aargpos],
								   commontype, commontypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
		actual_arg_types[aargpos] = commontype;
	}
}


/*
 * make_fn_arguments()
 *
 * 给定某函数的实际实参表达式，以及该函数期望的   
 * 输入类型，向表达式树添加任何必要的类型转换。
 * 调用方应已验证过允许进行转换。                        
 *         
 *
 * 注意：给定实参列表会被就地修改。  
 *
 * 与 coerce_type 一样，若无需特殊的未知 Param 处理，  
 * pstate 可为 NULL。
 */
void
make_fn_arguments(ParseState *pstate,
				  List *fargs,
				  Oid *actual_arg_types,
				  Oid *declared_arg_types)
{
	ListCell   *current_fargs;
	int			i = 0;

	foreach(current_fargs, fargs)
	{
		/* 类型不匹配？那就用函数调用强制转换…          */
		if (actual_arg_types[i] != declared_arg_types[i])
		{
			Node	   *node = (Node *) lfirst(current_fargs);

			/*
			 * 若 arg 是 NamedArgExpr，则改为强制转换其内层     
			 * 表达式，以让 NamedArgExpr 保持在列表的顶层。
			 */
			if (IsA(node, NamedArgExpr))
			{
				NamedArgExpr *na = (NamedArgExpr *) node;

				node = coerce_type(pstate,
								   (Node *) na->arg,
								   actual_arg_types[i],
								   declared_arg_types[i], -1,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
				na->arg = (Expr *) node;
			}
			else
			{
				node = coerce_type(pstate,
								   node,
								   actual_arg_types[i],
								   declared_arg_types[i], -1,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
				lfirst(current_fargs) = node;
			}
		}
		i++;
	}
}

/*
 * FuncNameAsType -
 *	  便捷例程：检查函数名是否与某类型名匹配        
 *
 * 返回匹配类型的 OID；无匹配则得 InvalidOid。我们忽略  
 * shell 类型与复合类型。
 */
static Oid
FuncNameAsType(List *funcname)
{
	Oid			result;
	Type		typtup;

	/*
	 * temp_ok=false 用于保护安全编写 SECURITY DEFINER 函数的     
	 * <refsect1 id="sql-createfunction-security"> 约定。  
	 */
	typtup = LookupTypeNameExtended(NULL, makeTypeNameFromNameList(funcname),
									NULL, false, false);
	if (typtup == NULL)
		return InvalidOid;

	if (((Form_pg_type) GETSTRUCT(typtup))->typisdefined &&
		!OidIsValid(typeTypeRelid(typtup)))
		result = typeTypeId(typtup);
	else
		result = InvalidOid;

	ReleaseSysCache(typtup);
	return result;
}

/*
 * ParseComplexProjection -
 *	  处理实参唯一且为复合类型的函数调用。                
 *	  若该函数调用实为列投影，则返回经过适当变换        
 *	  的表达式树；否则返回 NULL。            
 */
static Node *
ParseComplexProjection(ParseState *pstate, const char *funcname, Node *first_arg,
					   int location)
{
	TupleDesc	tupdesc;
	int			i;

	/*
	 * 对整行 Var 作特殊处理，使 (foo.*).bar 即便在 foo 是      
	 * 子选择、连接或 RECORD 函数的引用时也能解析。另  
	 * 一好处是省去多余的 FieldSelect；我们的结果可     
	 * 以省略整行 Var，仅用所选字段的一个 Var 表示。     
	 *       
	 *
	 * 此情形本可由 expandRecordVariable 处理，但可行时    
	 * 用这种做法效率更高。            
	 */
	if (IsA(first_arg, Var) &&
		((Var *) first_arg)->varattno == InvalidAttrNumber)
	{
		ParseNamespaceItem *nsitem;

		nsitem = GetNSItemByRangeTablePosn(pstate,
										   ((Var *) first_arg)->varno,
										   ((Var *) first_arg)->varlevelsup);
		/* funcname 匹配列名则返回 Var，否则为 NULL  */
		return scanNSItemForColumn(pstate, nsitem,
								   ((Var *) first_arg)->varlevelsup,
								   funcname, location);
	}

	/*
	 * 否则用 get_expr_result_tupdesc() 走复杂路线。 
	 *
	 * 若这是 RECORD 类型的 Var，则要更费功夫：必须找出   
	 * 该 Var 引用的对象，并把它传给 get_expr_result_tupdesc。  
	 * 该任务由 expandRecordVariable() 处理。  
	 */
	if (IsA(first_arg, Var) &&
		((Var *) first_arg)->vartype == RECORDOID)
		tupdesc = expandRecordVariable(pstate, (Var *) first_arg, 0);
	else
		tupdesc = get_expr_result_tupdesc(first_arg, true);
	if (!tupdesc)
		return NULL;			/* RECORD 类型无法解析*/

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (strcmp(funcname, NameStr(att->attname)) == 0 &&
			!att->attisdropped)
		{
			/* 成功，因此生成 FieldSelect 表达式   */
			FieldSelect *fselect = makeNode(FieldSelect);

			fselect->arg = (Expr *) first_arg;
			fselect->fieldnum = i + 1;
			fselect->resulttype = att->atttypid;
			fselect->resulttypmod = att->atttypmod;
			/* 为 parse_collate.c 保存属性排序规则   */
			fselect->resultcollid = att->attcollation;
			return (Node *) fselect;
		}
	}

	return NULL;				/* funcname 与任何列都不匹配  */
}

/*
 * funcname_signature_string
 *		构造表示函数名的字符串，其中包含参数类型。  
 *		结果形如 "foo(integer)"。              
 *
 * 若 argnames 非 NIL，则它是由 C 字符串组成的列表，       
 * 表示最后 N 个实参的实际名称。处理命名记法函数      
 * 调用时，也必须把它视为函数签名的一部分。            
 *
 * 这通常用于构造"找不到函数"的错误消息。              
 *          
 */
const char *
funcname_signature_string(const char *funcname, int nargs,
						  List *argnames, const Oid *argtypes)
{
	StringInfoData argbuf;
	int			numposargs;
	ListCell   *lc;
	int			i;

	initStringInfo(&argbuf);

	appendStringInfo(&argbuf, "%s(", funcname);

	numposargs = nargs - list_length(argnames);
	lc = list_head(argnames);

	for (i = 0; i < nargs; i++)
	{
		if (i)
			appendStringInfoString(&argbuf, ", ");
		if (i >= numposargs)
		{
			appendStringInfo(&argbuf, "%s => ", (char *) lfirst(lc));
			lc = lnext(argnames, lc);
		}
		appendStringInfoString(&argbuf, format_type_be(argtypes[i]));
	}

	appendStringInfoChar(&argbuf, ')');

	return argbuf.data;			/* 返回 palloc 缓冲区       */
}

/*
 * func_signature_string
 *		同上，但函数名以限定名列表的形式传入。      
 */
const char *
func_signature_string(List *funcname, int nargs,
					  List *argnames, const Oid *argtypes)
{
	return funcname_signature_string(NameListToString(funcname),
									 nargs, argnames, argtypes);
}

/*
 * LookupFuncNameInternal
 *		LookupFuncName/LookupFuncWithArgs 主力函数 
 *
 * 出错时（例如找不到函数），我们返回 InvalidOid，  
 * 并设置 *lookupError 指明出了什么问题。          
 *
 * 可能错误： 
 *	FUNCLOOKUP_NOSUCHFUNC：找不到该名称的函数。       
 *	FUNCLOOKUP_AMBIGUOUS：多个函数匹配。         
 */
static Oid
LookupFuncNameInternal(ObjectType objtype, List *funcname,
					   int nargs, const Oid *argtypes,
					   bool include_out_arguments, bool missing_ok,
					   FuncLookupError *lookupError)
{
	Oid			result = InvalidOid;
	FuncCandidateList clist;

	/* 仅零参函数允许 argtypes 为 NULL          */
	Assert(argtypes != NULL || nargs == 0);

	/* 总是设置 *lookupError，以免出现未初始化变量警告      */
	*lookupError = FUNCLOOKUP_NOSUCHFUNC;

	/* 获取候选对象列表      */
	clist = FuncnameGetCandidates(funcname, nargs, NIL, false, false,
								  include_out_arguments, missing_ok);

	/* 扫描列表，找与参数类型（如指定）及 objtype 匹配者 */
	for (; clist != NULL; clist = clist->next)
	{
		/* 若指定则检查参数类型     */
		if (nargs >= 0)
		{
			/* 若 nargs==0，argtypes 可为 NULL；勿将其传给 memcmp */
			if (nargs > 0 &&
				memcmp(argtypes, clist->args, nargs * sizeof(Oid)) != 0)
				continue;
		}

		/* 检查 FuncnameGetCandidates 报告的重复项        */
		if (!OidIsValid(clist->oid))
		{
			*lookupError = FUNCLOOKUP_AMBIGUOUS;
			return InvalidOid;
		}

		/* 若指定则检查 objtype 匹配 */
		switch (objtype)
		{
			case OBJECT_FUNCTION:
			case OBJECT_AGGREGATE:
				/* 忽略过程      */
				if (get_func_prokind(clist->oid) == PROKIND_PROCEDURE)
					continue;
				break;
			case OBJECT_PROCEDURE:
				/* 忽略非过程       */
				if (get_func_prokind(clist->oid) != PROKIND_PROCEDURE)
					continue;
				break;
			case OBJECT_ROUTINE:
				/* 无限制      */
				break;
			default:
				Assert(false);
		}

		/* 检查是否有多个匹配*/
		if (OidIsValid(result))
		{
			*lookupError = FUNCLOOKUP_AMBIGUOUS;
			return InvalidOid;
		}

		/* 找到了一个候选   */
		result = clist->oid;
	}

	return result;
}

/*
 * LookupFuncName
 *
 * 给定可能带限定的函数名以及可选的一组参数类型，    
 * 查找该函数。传 nargs == -1 表示实参的个数与类型          
 * 均未指定（这与指定"没有实参"不是一回事）。     
 *                                         
 *
 * 若函数名未带模式限定，则在当前的命名空间搜索       
 * 路径中查找。    
 *
 * 若找不到函数，missing_ok 为真时返回 InvalidOid，            
 * 否则报错。     
 *
 * 若 nargs == -1 且找到多个与该函数名匹配的函数，           
 * 无论 missing_ok 设为何值都会抛出歧义函数错误。            
 *        
 *
 * 只会找到函数；即使过程与名称和参数类型都匹      
 * 配也会被忽略。（不过此处不费心去拒绝聚合或         
 * 窗口函数。）                   
 */
Oid
LookupFuncName(List *funcname, int nargs, const Oid *argtypes, bool missing_ok)
{
	Oid			funcoid;
	FuncLookupError lookupError;

	funcoid = LookupFuncNameInternal(OBJECT_FUNCTION,
									 funcname, nargs, argtypes,
									 false, missing_ok,
									 &lookupError);

	if (OidIsValid(funcoid))
		return funcoid;

	switch (lookupError)
	{
		case FUNCLOOKUP_NOSUCHFUNC:
			/* missing_ok 为真时交由调用方处理           */
			if (missing_ok)
				return InvalidOid;

			if (nargs < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not find a function named \"%s\"",
								NameListToString(funcname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("function %s does not exist",
								func_signature_string(funcname, nargs,
													  NIL, argtypes))));
			break;

		case FUNCLOOKUP_AMBIGUOUS:
			/* 无论 missing_ok 为何都报错       */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("function name \"%s\" is not unique",
							NameListToString(funcname)),
					 errhint("Specify the argument list to select the function unambiguously.")));
			break;
	}

	return InvalidOid;			/* 让编译器安静  */
}

/*
 * LookupFuncWithArgs
 *
 * 与 LookupFuncName 类似，但参数类型由 ObjectWithArgs  
 * 节点指定。此外，本函数可基于 objtype 实参检查结果      
 * 是函数、过程还是聚合。传 OBJECT_ROUTINE 表示             
 * 接受其中任意一种。          
 *
 * 由于历史原因，查找函数时也接受聚合。              
 *          
 *
 * missing_ok 为真时，对缺少对象不产生任何错误并返          
 * 回 InvalidOid。其他类型的错误仍可能抛出，无论           
 * missing_ok 取值如何。 
 */
Oid
LookupFuncWithArgs(ObjectType objtype, ObjectWithArgs *func, bool missing_ok)
{
	Oid			argoids[FUNC_MAX_ARGS];
	int			argcount;
	int			nargs;
	int			i;
	ListCell   *args_item;
	Oid			oid;
	FuncLookupError lookupError;

	Assert(objtype == OBJECT_AGGREGATE ||
		   objtype == OBJECT_FUNCTION ||
		   objtype == OBJECT_PROCEDURE ||
		   objtype == OBJECT_ROUTINE);

	argcount = list_length(func->objargs);
	if (argcount > FUNC_MAX_ARGS)
	{
		if (objtype == OBJECT_PROCEDURE)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("procedures cannot have more than %d argument",
								   "procedures cannot have more than %d arguments",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg_plural("functions cannot have more than %d argument",
								   "functions cannot have more than %d arguments",
								   FUNC_MAX_ARGS,
								   FUNC_MAX_ARGS)));
	}

	/*
	 * 首先按传统 Postgres 规则，只考虑输入实参执           
	 * 行查找。    
	 */
	i = 0;
	foreach(args_item, func->objargs)
	{
		TypeName   *t = lfirst_node(TypeName, args_item);

		argoids[i] = LookupTypeNameOid(NULL, t, missing_ok);
		if (!OidIsValid(argoids[i]))
			return InvalidOid;	/* missing_ok 必为真    */
		i++;
	}

	/*
	 * 为 LookupFuncNameInternal 设置 nargs。-1 表示未指          
	 * 定实参。   
	 */
	nargs = func->args_unspecified ? -1 : argcount;

	/*
	 * 在 args_unspecified 模式下，还要让 LookupFuncNameInternal     
	 * 考虑对象类型，似乎没有理由不这样做。但若给了  
	 * 实参列表，则禁用 objtype 检查，因为我们宁可报      
	 * "对象类型不对"，也不愿报"对象不存在"。              
	 * （带实参时 FuncnameGetCandidates 已确保只有一个参数    
	 * 类型匹配，所以这一选择不会带来歧义失败的风      
	 * 险。）    
	 */
	oid = LookupFuncNameInternal(func->args_unspecified ? objtype : OBJECT_ROUTINE,
								 func->objname, nargs, argoids,
								 false, missing_ok,
								 &lookupError);

	/*
	 * 若指定了 PROCEDURE 或 ROUTINE，且我们手上有实参      
	 * 列表而其中不含参数模式标记，且尚未发现存在        
	 * 歧义，则再做一次考虑全部实参的查找。（注：对 
	 * 零参过程，或在 args_unspecified 模式下，普通查找       
	 * 已足够；因此需要 objfuncargs 非 NIL 才做这次查找）    
	 *                         
	 */
	if ((objtype == OBJECT_PROCEDURE || objtype == OBJECT_ROUTINE) &&
		func->objfuncargs != NIL &&
		lookupError != FUNCLOOKUP_AMBIGUOUS)
	{
		bool		have_param_mode = false;

		/*
		 * 检查非默认参数模式标记。若存在任一标记，则 
		 * 说明命令不符合 SQL 规范语法，因此可假定传统 
		 * 的 Postgres 查找方法——即只考虑输入参数——    
		 * 就已足够。（注意，因为规范中函数没有 OUT       
		 * 参数，所以在 FUNCTION 或 AGGREGATE                        
		 * 模式下无需这种技巧。）   
		 */
		foreach(args_item, func->objfuncargs)
		{
			FunctionParameter *fp = lfirst_node(FunctionParameter, args_item);

			if (fp->mode != FUNC_PARAM_DEFAULT)
			{
				have_param_mode = true;
				break;
			}
		}

		if (!have_param_mode)
		{
			Oid			poid;

			/* 无模式标记时，objargs 必然包含全部参数  */
			Assert(list_length(func->objfuncargs) == argcount);

			/* 对于 objtype == OBJECT_PROCEDURE，可忽略非过程       */
			poid = LookupFuncNameInternal(objtype, func->objname,
										  argcount, argoids,
										  true, missing_ok,
										  &lookupError);

			/* 合并结果，并处理歧义      */
			if (OidIsValid(poid))
			{
				if (OidIsValid(oid) && oid != poid)
				{
					/* 糟糕，两个方向都命中了不同的对象  */
					oid = InvalidOid;
					lookupError = FUNCLOOKUP_AMBIGUOUS;
				}
				else
					oid = poid;
			}
			else if (lookupError == FUNCLOOKUP_AMBIGUOUS)
				oid = InvalidOid;
		}
	}

	if (OidIsValid(oid))
	{
		/*
		 * 即使已经找到了函数，也要验证 objtype 是否         
		 * 与所找到函数的 prokind 相符。基于历史原因，      
		 * 我们允许 FUNCTION 的 objtype 涵盖聚合和                
		 * 窗口函数；但若对象是过程则不在此列。该功能    
		 * 较新，这一历史规则并不适用。                       
		 *
		 * （此项检查与 LookupFuncNameInternal 中的 objtype    
		 * 检查部分重复；但并不完全重复，因为我们常常    
		 * 不让 LookupFuncNameInternal 执行该检查。）
		 */
		switch (objtype)
		{
			case OBJECT_FUNCTION:
				/* 仅当它是过程时才报错。  */
				if (get_func_prokind(oid) == PROKIND_PROCEDURE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("%s is not a function",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			case OBJECT_PROCEDURE:
				/* 若找到的对象不是过程则拒绝。 */
				if (get_func_prokind(oid) != PROKIND_PROCEDURE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("%s is not a procedure",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			case OBJECT_AGGREGATE:
				/* 若找到对象非聚合函数则拒绝。  */
				if (get_func_prokind(oid) != PROKIND_AGGREGATE)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("function %s is not an aggregate",
									func_signature_string(func->objname, argcount,
														  NIL, argoids))));
				break;

			default:
				/* OBJECT_ROUTINE 均可接受。   */
				break;
		}

		return oid;				/* 无异常*/
	}
	else
	{
		/* 处理函数查找失败的情形       */
		switch (lookupError)
		{
			case FUNCLOOKUP_NOSUCHFUNC:
				/* missing_ok 为真时抑制找不到函数的错误   */
				if (missing_ok)
					break;

				switch (objtype)
				{
					case OBJECT_PROCEDURE:
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find a procedure named \"%s\"",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("procedure %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;

					case OBJECT_AGGREGATE:
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find an aggregate named \"%s\"",
											NameListToString(func->objname))));
						else if (argcount == 0)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("aggregate %s(*) does not exist",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("aggregate %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;

					default:
						/* FUNCTION 与 ROUTINE */
						if (func->args_unspecified)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("could not find a function named \"%s\"",
											NameListToString(func->objname))));
						else
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_FUNCTION),
									 errmsg("function %s does not exist",
											func_signature_string(func->objname, argcount,
																  NIL, argoids))));
						break;
				}
				break;

			case FUNCLOOKUP_AMBIGUOUS:
				switch (objtype)
				{
					case OBJECT_FUNCTION:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("function name \"%s\" is not unique",
										NameListToString(func->objname)),
								 func->args_unspecified ?
								 errhint("Specify the argument list to select the function unambiguously.") : 0));
						break;
					case OBJECT_PROCEDURE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("procedure name \"%s\" is not unique",
										NameListToString(func->objname)),
								 func->args_unspecified ?
								 errhint("Specify the argument list to select the procedure unambiguously.") : 0));
						break;
					case OBJECT_AGGREGATE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("aggregate name \"%s\" is not unique",
										NameListToString(func->objname)),
								 func->args_unspecified ?
								 errhint("Specify the argument list to select the aggregate unambiguously.") : 0));
						break;
					case OBJECT_ROUTINE:
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
								 errmsg("routine name \"%s\" is not unique",
										NameListToString(func->objname)),
								 func->args_unspecified ?
								 errhint("Specify the argument list to select the routine unambiguously.") : 0));
						break;

					default:
						Assert(false);	/* 上方 Assert 已排除    */
						break;
				}
				break;
		}

		return InvalidOid;
	}
}

/*
 * check_srf_call_placement
 *		验证集合返回函数是否在有效位置被调用，       
 *		否则给出友好错误。   
 *
 * 副作用是：适当时将 pstate->p_hasTargetSRFs 置为真。    
 *
 * last_srf 应是刚开始转换函数参数之前                   
 * pstate->p_last_srf 的副本。这可用于检测                    
 * SRF 的参数中是否包含 SRF。             
 */
void
check_srf_call_placement(ParseState *pstate, Node *last_srf, int location)
{
	const char *err;
	bool		errkind;

	/*
	 * 检查集合返回函数在查询中是否位于无效位置。  
	 * 基本上，除目标列表（含 GROUP BY/ORDER BY                 
	 * 表达式）、VALUES 以及 FROM 中的函数外，                  
	 * 不允许 SRF。      
	 *
	 * 为简洁起见，这里支持两种报错方案：把 "err"       
	 * 设为自定义消息；或者，若 ParseExprKindName 的返回值  
	 * 足以标识错误上下文，*且*其返回值只是一个            
	 * SQL 关键字，则设 "errkind" 为真。（否则应使用        
	 * 自定义消息，以免产生翻译问题。）
	 */
	err = NULL;
	errkind = false;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* 不会发生 */
			break;
		case EXPR_KIND_OTHER:
			/* 此处接受 SRF；若需报错由调用者负责  */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
			err = _("set-returning functions are not allowed in JOIN conditions");
			break;
		case EXPR_KIND_FROM_SUBSELECT:
			/* 不会到这里，但保险起见抛出错误    */
			errkind = true;
			break;
		case EXPR_KIND_FROM_FUNCTION:
			/* 可以，但此处不允许嵌套 SRF。  */
			/* errmsg 选择与 transformRangeFunction() 一致   */
			/* errposition 应指向内层 SRF。        */
			if (pstate->p_last_srf != last_srf)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("set-returning functions must appear at top level of FROM"),
						 parser_errposition(pstate,
											exprLocation(pstate->p_last_srf))));
			break;
		case EXPR_KIND_WHERE:
			errkind = true;
			break;
		case EXPR_KIND_POLICY:
			err = _("set-returning functions are not allowed in policy expressions");
			break;
		case EXPR_KIND_HAVING:
			errkind = true;
			break;
		case EXPR_KIND_FILTER:
			errkind = true;
			break;
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
			/* 可以，实际相当于 GROUP BY/ORDER BY    */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			err = _("set-returning functions are not allowed in window definitions");
			break;
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
			/* 可  */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			/* 禁止，因为会导致无法确定该如何处理 */
			errkind = true;
			break;
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
			/* 可  */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* 可  */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
			errkind = true;
			break;
		case EXPR_KIND_RETURNING:
			errkind = true;
			break;
		case EXPR_KIND_VALUES:
			/* nodeValuesscan.c 目前不支持 SRF。              */
			errkind = true;
			break;
		case EXPR_KIND_VALUES_SINGLE:
			/* 可以，按 SELECT 目标列表一样处理    */
			pstate->p_hasTargetSRFs = true;
			break;
		case EXPR_KIND_MERGE_WHEN:
			err = _("set-returning functions are not allowed in MERGE WHEN conditions");
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			err = _("set-returning functions are not allowed in check constraints");
			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			err = _("set-returning functions are not allowed in DEFAULT expressions");
			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			err = _("set-returning functions are not allowed in index expressions");
			break;
		case EXPR_KIND_INDEX_PREDICATE:
			err = _("set-returning functions are not allowed in index predicates");
			break;
		case EXPR_KIND_STATS_EXPRESSION:
			err = _("set-returning functions are not allowed in statistics expressions");
			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			err = _("set-returning functions are not allowed in transform expressions");
			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			err = _("set-returning functions are not allowed in EXECUTE parameters");
			break;
		case EXPR_KIND_TRIGGER_WHEN:
			err = _("set-returning functions are not allowed in trigger WHEN conditions");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("set-returning functions are not allowed in partition bound");
			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			err = _("set-returning functions are not allowed in partition key expressions");
			break;
		case EXPR_KIND_CALL_ARGUMENT:
			err = _("set-returning functions are not allowed in CALL arguments");
			break;
		case EXPR_KIND_COPY_WHERE:
			err = _("set-returning functions are not allowed in COPY FROM WHERE conditions");
			break;
		case EXPR_KIND_GENERATED_COLUMN:
			err = _("set-returning functions are not allowed in column generation expressions");
			break;
		case EXPR_KIND_CYCLE_MARK:
			errkind = true;
			break;

			/*
			 * 这里有意不设 default: 分支，这样在新增     
			 * ParseExprKind 而不扩展此 switch 时，编译器会 
			 * 发出警告。若运行时遇到无法识别的值，       
			 * 其行为与 EXPR_KIND_OTHER 相同，                        
			 * 反正也合理。   
			 */
	}
	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, location)));
	if (errkind)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						ParseExprKindName(pstate->p_expr_kind)),
				 parser_errposition(pstate, location)));
}
