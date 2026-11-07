/*-------------------------------------------------------------------------
 *
 * parse_node.c
 *	  为查询树构造节点的各种例程        
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_node.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/varbit.h"

static void pcb_error_callback(void *arg);


/*
 * make_parsestate
 *		分配并初始化新的 ParseState。   
 *
 * 调用者最终应通过 free_parsestate() 释放该 ParseState。    
 */
ParseState *
make_parsestate(ParseState *parentParseState)
{
	ParseState *pstate;

	pstate = palloc0(sizeof(ParseState));

	pstate->parentParseState = parentParseState;

	/* 填充初值不是 null/false/零的字段。      */
	pstate->p_next_resno = 1;
	pstate->p_resolve_unknowns = true;

	if (parentParseState)
	{
		pstate->p_sourcetext = parentParseState->p_sourcetext;
		/* 钩子全部从父级复制      */
		pstate->p_pre_columnref_hook = parentParseState->p_pre_columnref_hook;
		pstate->p_post_columnref_hook = parentParseState->p_post_columnref_hook;
		pstate->p_paramref_hook = parentParseState->p_paramref_hook;
		pstate->p_coerce_param_hook = parentParseState->p_coerce_param_hook;
		pstate->p_ref_hook_state = parentParseState->p_ref_hook_state;
		/* 查询环境在整个解析分析期间保留于上下文中。 */
		pstate->p_queryEnv = parentParseState->p_queryEnv;
	}

	return pstate;
}

/*
 * free_parsestate
 *		释放一个 ParseState 及其附属资源。     
 */
void
free_parsestate(ParseState *pstate)
{
	/*
	 * 检查我们没有生成过多的 resno；至少我们             
	 * 不允许超过 2^16，因为这会超出 AttrNumber 范围。     
	 * 最稳妥的做法是使用 MaxTupleAttributeNumber。     
	 */
	if (pstate->p_next_resno - 1 > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("target lists can have at most %d entries",
						MaxTupleAttributeNumber)));

	if (pstate->p_target_relation != NULL)
		table_close(pstate->p_target_relation, NoLock);

	pfree(pstate);
}


/*
 * parser_errposition
 *		如果可能，报告解析分析时的游标位置。    
 *
 * 该函数预期在 ereport() 调用内部使用。返回值            
 * 是虚拟值（总是 0）。  
 *
 * 原始解析树存储的位置是源字符串内的字节偏移，     
 * 报告给客户端时必须把它们转换为 1 起算的字符序号。   
 * （我们这样做是为了避免在正常的非出错情形下         
 * 产生不必要的开销：计算字符序号的开销要比存储   
 * 词元偏移高得多。）           
 */
int
parser_errposition(ParseState *pstate, int location)
{
	int			pos;

	/* 未提供位置则为空操作     */
	if (location < 0)
		return 0;
	/* 若源文本不可用则无法做任何处理     */
	if (pstate == NULL || pstate->p_sourcetext == NULL)
		return 0;
	/* 将偏移转换为字符序号     */
	pos = pg_mbstrlen_with_len(pstate->p_sourcetext, location) + 1;
	/* 传给 ereport 机制处理。       */
	return errposition(pos);
}


/*
 * setup_parser_errposition_callback
 *		让非解析器错误也能报告出错位置            
 *
 * 有时解析器会调用不属于解析器子系统的函数，    
 * 这些函数无法合理地传入 ParseState；但我们希望     
 * 其中抛出的任何错误都能带上解析错误位置标记。 
 * 用本函数建立错误上下文栈条目即可实现该目标。 
 * 用法模式如下：                           
 *
 *		声明局部变量 "ParseCallbackState pcbstate"      
 *		…
 *		setup_parser_errposition_callback(&pcbstate, pstate, location);
 *		调用可能抛出错误的函数； 
 *		cancel_parser_errposition_callback(&pcbstate);
 */
void
setup_parser_errposition_callback(ParseCallbackState *pcbstate,
								  ParseState *pstate, int location)
{
	/* 为 ereport() 设置错误回溯支持      */
	pcbstate->pstate = pstate;
	pcbstate->location = location;
	pcbstate->errcallback.callback = pcb_error_callback;
	pcbstate->errcallback.arg = (void *) pcbstate;
	pcbstate->errcallback.previous = error_context_stack;
	error_context_stack = &pcbstate->errcallback;
}

/*
 * 取消先前设置的 errposition 回调。     
 */
void
cancel_parser_errposition_callback(ParseCallbackState *pcbstate)
{
	/* 弹出错误上下文栈    */
	error_context_stack = pcbstate->errcallback.previous;
}

/*
 * 插入解析器错误位置用的错误上下文回调。  
 *
 * 注意回调安装期间发生的任何错误都会调用它，  
 * 对于查询取消错误我们会避免插入无关的错误位置，  
 * --- 除此之外还有其他重要情形吗？                         
 */
static void
pcb_error_callback(void *arg)
{
	ParseCallbackState *pcbstate = (ParseCallbackState *) arg;

	if (geterrcode() != ERRCODE_QUERY_CANCELED)
		(void) parser_errposition(pcbstate->pstate, pcbstate->location);
}


/*
 * transformContainerType()
 *		确定下标操作实际使用的容器类型。                
 *
 * 如有必要将修改 containerType/containerTypmod，以标识    
 * 出真正的容器类型和 typmod。这主要涉及把域还原为 
 * 其基类型，但还有一些特殊情形需要考虑。              
 * 注意调用者仍需检查结果类型是否为容器类型。         
 */
void
transformContainerType(Oid *containerType, int32 *containerTypmod)
{
	/*
	 * 若输入是域，则还原为基类型，并提取实际应用于  
	 * 基类型的 typmod。对域做下标操作本质上作用于     
	 * 基础容器类型而非域本身。                                
	 * （注意：我们没有提供任何方法，让容器类型之上的  
	 * 域的创建者能够隐藏其可下标性。）                       
	 */
	*containerType = getBaseTypeAndTypmod(*containerType, containerTypmod);

	/*
	 * 我们将 int2vector 和 oidvector 当作 int2[] 与              
	 * oid[] 之上的域。这样做是必要的，因为数组切片        
	 * 可能创建出不满足 xxxvector 类型维度约束的        
	 * 数组；所以我们希望切片操作的结果                
	 * 被视为更一般的类型。            
	 */
	if (*containerType == INT2VECTOROID)
		*containerType = INT2ARRAYOID;
	else if (*containerType == OIDVECTOROID)
		*containerType = OIDARRAYOID;
}

/*
 * transformContainerSubscripts()
 *		转换容器（数组等）的下标操作。这同时用于容器   
 *		读取和容器赋值两种场景。     
 *
 * 在容器读取中，我们会得到一个源容器值，并生成一个  
 * 表示提取单个容器元素或容器切片之结果的表达式。    
 *                              
 *
 * 容器赋值在这里的处理方式与容器读取基本相同。       
 * 调用者会修改结果节点，将待赋值的源值插入到         
 * 本应被读取操作取回的元素或切片中。                   
 * 执行结果将是一个新的容器值，其中源值被插入到 
 * 容器中的相应部位。                                    
 *
 * 对这两种情形，若源值属于域上的容器类型，结果都
 * 与它属于容器类型时相同；本质上，在应用下标之前   
 * 必须先把域折叠为它的基类型。                          
 * （注意 int2vector 和 oidvector 在这里被视为域。）   
 *
 * pstate		解析状态
 * containerBase	已对整个容器完成转换的表达式                 
 * containerType	容器数据类型的 OID（应与 containerBase 
 *					的类型一致，或是其域类型的基类型）  
 *					            
 * containerTypMod	容器的 typmod        
 * indirection		未转换的下标列表（不得为 NIL）       
 * isAssignment		若将成为容器赋值则为 true。          
 */
SubscriptingRef *
transformContainerSubscripts(ParseState *pstate,
							 Node *containerBase,
							 Oid containerType,
							 int32 containerTypMod,
							 List *indirection,
							 bool isAssignment)
{
	SubscriptingRef *sbsref;
	const SubscriptRoutines *sbsroutines;
	Oid			elementType;
	bool		isSlice = false;
	ListCell   *idx;

	/*
	 * 确定实际容器类型，还原其中的域。赋值情形下  
	 * 调用者已经做过这件事，因为它也需要知道          
	 * 实际的容器类型。       
	 */
	if (!isAssignment)
		transformContainerType(&containerType, &containerTypMod);

	/*
	 * 验证容器类型是否可下标，并取得其支持函数和     
	 * typelem。            
	 */
	sbsroutines = getSubscriptingRoutines(containerType, &elementType);
	if (!sbsroutines)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot subscript type %s because it does not support subscripting",
						format_type_be(containerType)),
				 parser_errposition(pstate, exprLocation(containerBase))));

	/*
	 * 检测 indirection 各项中是否存在切片指定符。       
	 *
	 * 只含简单下标的列表引用的是单个容器元素。         
	 * 如果其中某些项是切片指定符(lower:upper)，那么         
	 * 下标表达式就表示容器切片操作。              
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);

		if (ai->is_slice)
		{
			isSlice = true;
			break;
		}
	}

	/*
	 * 准备构建 SubscriptingRef 节点。  
	 */
	sbsref = makeNode(SubscriptingRef);

	sbsref->refcontainertype = containerType;
	sbsref->refelemtype = elementType;
	/* refrestype 留待容器专属逻辑来设置。     */
	sbsref->reftypmod = containerTypMod;
	/* refcollid 将由 parse_collate.c 设置  */
	/* refupperindexpr、reflowerindexpr 留待容器逻辑设置        */
	sbsref->refexpr = (Expr *) containerBase;
	sbsref->refassgnexpr = NULL;	/* 若是赋值则由调用者填写      */

	/*
	 * 调用容器类型专属逻辑来转换各下标，并确定          
	 * 下标操作的结果类型。         
	 */
	sbsroutines->transform(sbsref, indirection, pstate,
						   isSlice, isAssignment);

	/*
	 * 验证我们得到的是有效类型（例如可防止有人把       
	 * array_subscript_handler 用作 typsubscript 而不设置 typelem）。 
	 */
	if (!OidIsValid(sbsref->refrestype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot subscript type %s because it does not support subscripting",
						format_type_be(containerType))));

	return sbsref;
}

/*
 * make_const
 *
 *	把 A_Const 节点（文法返回的）转换成 Const 节点，    
 *	其类型为该常量 "自然" 的类型。注意本例程只     
 *	在常量没有显式转换时才会用到，因此我们必须 
 *	猜测想要的是什么类型。 
 *
 *	对字符串字面量，我们产生 UNKNOWN 类型的常量 ----   
 *	其表示形式与 cstring 相同，但它向后继的类型解析   
 *	表明我们尚不确定它应被视为何种类型。               
 *	显式的 "NULL" 常量的类型也是 UNKNOWN。   
 *
 *	对于整数和浮点数，根据数值的取值产生 int4、int8 
 *	或 numeric。XXX 我们也应产生 int2，                    
 *	但在那之前还需要额外的清理工作；如果现在就  
 *	尝试，失败的例子过多。     
 */
Const *
make_const(ParseState *pstate, A_Const *aconst)
{
	Const	   *con;
	Datum		val;
	Oid			typeid;
	int			typelen;
	bool		typebyval;
	ParseCallbackState pcbstate;

	if (aconst->isnull)
	{
		/* 返回 null 常量  */
		con = makeConst(UNKNOWNOID,
						-1,
						InvalidOid,
						-2,
						(Datum) 0,
						true,
						false);
		con->location = aconst->location;
		return con;
	}

	switch (nodeTag(&aconst->val))
	{
		case T_Integer:
			val = Int32GetDatum(intVal(&aconst->val));

			typeid = INT4OID;
			typelen = sizeof(int32);
			typebyval = true;
			break;

		case T_Float:
			{
				/* 既可能是浮点数，也可能是超大整数。 */

				ErrorSaveContext escontext = {T_ErrorSaveContext};
				int64		val64;

				val64 = pg_strtoint64_safe(aconst->val.fval.fval, (Node *) &escontext);
				if (!escontext.error_occurred)
				{
					/*
					 * 它实际可能放得进 int32。大概只有 INT_MIN
					 * 会出现，但为稳妥仍把测试写成通用的， 
					 *      
					 */
					int32		val32 = (int32) val64;

					if (val64 == (int64) val32)
					{
						val = Int32GetDatum(val32);

						typeid = INT4OID;
						typelen = sizeof(int32);
						typebyval = true;
					}
					else
					{
						val = Int64GetDatum(val64);

						typeid = INT8OID;
						typelen = sizeof(int64);
						typebyval = FLOAT8PASSBYVAL;	/* int8 和 float8 类似*/
					}
				}
				else
				{
					/* 若 numeric_in() 失败则安排报告位置。  */
					setup_parser_errposition_callback(&pcbstate, pstate, aconst->location);
					val = DirectFunctionCall3(numeric_in,
											  CStringGetDatum(aconst->val.fval.fval),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(-1));
					cancel_parser_errposition_callback(&pcbstate);

					typeid = NUMERICOID;
					typelen = -1;	/* 变长       */
					typebyval = false;
				}
				break;
			}

		case T_Boolean:
			val = BoolGetDatum(boolVal(&aconst->val));

			typeid = BOOLOID;
			typelen = 1;
			typebyval = true;
			break;

		case T_String:

			/*
			 * 我们这里假定 UNKNOWN 的内部表示与 CSTRING       
			 * 相同。      
			 */
			val = CStringGetDatum(strVal(&aconst->val));

			typeid = UNKNOWNOID;	/* 稍后强制转换    */
			typelen = -2;		/* cstring 风格变宽类型  */
			typebyval = false;
			break;

		case T_BitString:
			/* 若 bit_in() 失败则安排报告位置。  */
			setup_parser_errposition_callback(&pcbstate, pstate, aconst->location);
			val = DirectFunctionCall3(bit_in,
									  CStringGetDatum(aconst->val.bsval.bsval),
									  ObjectIdGetDatum(InvalidOid),
									  Int32GetDatum(-1));
			cancel_parser_errposition_callback(&pcbstate);
			typeid = BITOID;
			typelen = -1;
			typebyval = false;
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(&aconst->val));
			return NULL;		/* 让编译器安静  */
	}

	con = makeConst(typeid,
					-1,			/* typmod -1 各情形均合法  */
					InvalidOid, /* 各情形均为不可排序类型*/
					typelen,
					val,
					false,
					typebyval);
	con->location = aconst->location;

	return con;
}
