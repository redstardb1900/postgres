/*-------------------------------------------------------------------------
 *
 * parse_param.c
 *	  在解析器中处理参数
 *
 * 此代码涵盖核心后端中使用的两种情形：           
 *		* 具有已知类型的固定参数列表    
 *		* 可扩展的参数列表，其类型可选地              
 *		  由上下文确定        
 * 两种情形下都只支持显式的 $n 引用（ParamRef 节点）。    
 *
 * 注意，还可以通过 ParseState 中定义的解析器钩子以    
 * 其他方式处理参数。 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_param.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_param.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


typedef struct FixedParamState
{
	const Oid  *paramTypes;		/* 参数类型 OID 数组      */
	int			numParams;		/* 数组条目数         */
} FixedParamState;

/*
 * 在 varparams 情形下，调用者提供的 OID 数组（若有） 
 * 可以在需要时重新 palloc 得更大。数组条目为零表示       
 * 该参数编号尚未出现过，而 UNKNOWNOID 表示该参数         
 * 已用但类型未知。  
 */
typedef struct VarParamState
{
	Oid		  **paramTypes;		/* 参数类型 OID 数组      */
	int		   *numParams;		/* 数组条目数         */
} VarParamState;

static Node *fixed_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *variable_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *variable_coerce_param_hook(ParseState *pstate, Param *param,
										Oid targetTypeId, int32 targetTypeMod,
										int location);
static bool check_parameter_resolution_walker(Node *node, ParseState *pstate);
static bool query_contains_extern_params_walker(Node *node, void *context);


/*
 * 为处理包含固定参数引用的查询做好准备              
 */
void
setup_parse_fixed_parameters(ParseState *pstate,
							 const Oid *paramTypes, int numParams)
{
	FixedParamState *parstate = palloc(sizeof(FixedParamState));

	parstate->paramTypes = paramTypes;
	parstate->numParams = numParams;
	pstate->p_ref_hook_state = (void *) parstate;
	pstate->p_paramref_hook = fixed_paramref_hook;
	/* 无需使用 p_coerce_param_hook   */
}

/*
 * 为处理包含可变参数引用的查询做好准备                 
 */
void
setup_parse_variable_parameters(ParseState *pstate,
								Oid **paramTypes, int *numParams)
{
	VarParamState *parstate = palloc(sizeof(VarParamState));

	parstate->paramTypes = paramTypes;
	parstate->numParams = numParams;
	pstate->p_ref_hook_state = (void *) parstate;
	pstate->p_paramref_hook = variable_paramref_hook;
	pstate->p_coerce_param_hook = variable_coerce_param_hook;
}

/*
 * 使用固定参数类型转换 ParamRef          
 */
static Node *
fixed_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	FixedParamState *parstate = (FixedParamState *) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Param	   *param;

	/* 检查参数编号是否有效  */
	if (paramno <= 0 || paramno > parstate->numParams ||
		!OidIsValid(parstate->paramTypes[paramno - 1]))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno),
				 parser_errposition(pstate, pref->location)));

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = parstate->paramTypes[paramno - 1];
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * 使用可变参数类型转换 ParamRef             
 *
 * 唯一的差别在于必须按需扩大参数类型                 
 * 数组。 
 */
static Node *
variable_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Oid		   *pptype;
	Param	   *param;

	/* 检查参数编号是否在界内  */
	if (paramno <= 0 || paramno > INT_MAX / sizeof(Oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno),
				 parser_errposition(pstate, pref->location)));
	if (paramno > *parstate->numParams)
	{
		/* 需要扩大参数数组    */
		if (*parstate->paramTypes)
			*parstate->paramTypes = repalloc0_array(*parstate->paramTypes, Oid,
													*parstate->numParams, paramno);
		else
			*parstate->paramTypes = palloc0_array(Oid, paramno);
		*parstate->numParams = paramno;
	}

	/* 在数组中找参数槽位  */
	pptype = &(*parstate->paramTypes)[paramno - 1];

	/* 若未见，则初始化为 UNKNOWN 类型     */
	if (*pptype == InvalidOid)
		*pptype = UNKNOWNOID;

	/*
	 * 如果参数的类型为 void 且这是过程调用，则将其      
	 * 解释为 unknown。这使 JDBC 驱动无需区分                 
	 * 函数调用与过程调用。本技巧的另一部分                
	 * 见 ParseFuncOrColumn()
	 */
	if (*pptype == VOIDOID && pstate->p_expr_kind == EXPR_KIND_CALL_ARGUMENT)
		*pptype = UNKNOWNOID;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = *pptype;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * varparams 情形下将 Param 强制转换为查询所需类型      
 */
static Node *
variable_coerce_param_hook(ParseState *pstate, Param *param,
						   Oid targetTypeId, int32 targetTypeMod,
						   int location)
{
	if (param->paramkind == PARAM_EXTERN && param->paramtype == UNKNOWNOID)
	{
		/*
		 * 输入是一个此前类型未定的 Param，我们             
		 * 需要更新该 Param 的类型认知。 
		 */
		VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
		Oid		   *paramTypes = *parstate->paramTypes;
		int			paramno = param->paramid;

		if (paramno <= 0 ||		/* 不应发生，但…… */
			paramno > *parstate->numParams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no parameter $%d", paramno),
					 parser_errposition(pstate, param->location)));

		if (paramTypes[paramno - 1] == UNKNOWNOID)
		{
			/* 我们已成功确定该类型       */
			paramTypes[paramno - 1] = targetTypeId;
		}
		else if (paramTypes[paramno - 1] == targetTypeId)
		{
			/* 此前已确定过类型，且两者一致      */
		}
		else
		{
			/* 糟  */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("inconsistent types deduced for parameter $%d",
							paramno),
					 errdetail("%s versus %s",
							   format_type_be(paramTypes[paramno - 1]),
							   format_type_be(targetTypeId)),
					 parser_errposition(pstate, param->location)));
		}

		param->paramtype = targetTypeId;

		/*
		 * 注意：这里很想把 Param 的 paramtypmod 设为      
		 * targetTypeMod，但这可能并不明智，因为我们      
		 * 没有任何基础设施来保证传给 Param 的值会匹配    
		 * 某个特定的 typmod。将其保留为 -1 可确保在       
		 * 需要时发生运行时长度检查/转换。      
		 */
		param->paramtypmod = -1;

		/*
		 * 本模块总是把 Param 的排序规则设为其数据类型    
		 * 的默认值。如果这不是你想要的，就应当改用       
		 * 更通用的解析器替换钩子。   
		 */
		param->paramcollid = get_typcollation(param->paramtype);

		/* 使用参数位置和强制转换位置中最左的      */
		if (location >= 0 &&
			(param->location < 0 || location < param->location))
			param->location = location;

		return (Node *) param;
	}

	/* 否则发出信号继续正常转换        */
	return NULL;
}

/*
 * 在用 parse_variable_parameters 完成解析之后，检查           
 * 可变参数的赋值是否一致。      
 *
 * 注意：此代码有意不检查所有参数位置是否                
 * 都被用到，也不检查它们是否都获得了非 UNKNOWN 类型。 
 * 解析器调用者应按需确保。  
 */
void
check_variable_parameters(ParseState *pstate, Query *query)
{
	VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;

	/* 若 numParams 为零则未生成任何 Param，故无需处理 */
	if (*parstate->numParams > 0)
		(void) query_tree_walker(query,
								 check_parameter_resolution_walker,
								 (void *) pstate, 0);
}

/*
 * 遍历一棵已完成分析的树来验证参数符号         
 * 与其类型是否一致。需要这一步是因为有些       
 * Param 可能仍为 UNKNOWN，因为没有东西迫使        
 * 其转换，而后来出现的实例可能已被转换。    
 */
static bool
check_parameter_resolution_walker(Node *node, ParseState *pstate)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
		{
			VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
			int			paramno = param->paramid;

			if (paramno <= 0 || /* 不应发生，但…… */
				paramno > *parstate->numParams)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_PARAMETER),
						 errmsg("there is no parameter $%d", paramno),
						 parser_errposition(pstate, param->location)));

			if (param->paramtype != (*parstate->paramTypes)[paramno - 1])
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
						 errmsg("could not determine data type of parameter $%d",
								paramno),
						 parser_errposition(pstate, param->location)));
		}
		return false;
	}
	if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或未规划的 sublink 子查询   */
		return query_tree_walker((Query *) node,
								 check_parameter_resolution_walker,
								 (void *) pstate, 0);
	}
	return expression_tree_walker(node, check_parameter_resolution_walker,
								  (void *) pstate);
}

/*
 * 检查完整解析后的查询树是否包含任何 PARAM_EXTERN Param。  
 */
bool
query_contains_extern_params(Query *query)
{
	return query_tree_walker(query,
							 query_contains_extern_params_walker,
							 NULL, 0);
}

static bool
query_contains_extern_params_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或未规划的 sublink 子查询   */
		return query_tree_walker((Query *) node,
								 query_contains_extern_params_walker,
								 context, 0);
	}
	return expression_tree_walker(node, query_contains_extern_params_walker,
								  context);
}
