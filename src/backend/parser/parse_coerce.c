/*-------------------------------------------------------------------------
 *
 * parse_coerce.c
 *		处理解析器的类型强制转换/转换 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_coerce.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"		/* datumIsEqual() 所需     */
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Node *coerce_type_typmod(Node *node,
								Oid targetTypeId, int32 targetTypMod,
								CoercionContext ccontext, CoercionForm cformat,
								int location,
								bool hideInputCoercion);
static void hide_coercion_node(Node *node);
static Node *build_coercion_expression(Node *node,
									   CoercionPathType pathtype,
									   Oid funcId,
									   Oid targetTypeId, int32 targetTypMod,
									   CoercionContext ccontext, CoercionForm cformat,
									   int location);
static Node *coerce_record_to_complex(ParseState *pstate, Node *node,
									  Oid targetTypeId,
									  CoercionContext ccontext,
									  CoercionForm cformat,
									  int location);
static bool is_complex_array(Oid typid);
static bool typeIsOfTypedTable(Oid reltypeId, Oid reloftypeId);


/*
 * coerce_to_target_type()
 *		把表达式转换为目标类型与 typmod。    
 *
 * 这是面向任意类型强制转换操作的通用入口。       
 * 各组件操作 can_coerce_type、coerce_type 与                   
 * coerce_type_typmod 只宜用于特殊情况                        
 * （例如预期转换必能成功时）。             
 *
 * 返回（可能已变换的）表达式树；若类型转换         
 * 无法进行，则返回 NULL。（我们这样做而非直接 ereport，  
 * 以便调用者能生成带上下文的定制错误消息。）        
 *
 * pstate - 解析状态（可空，见 coerce_type）
 * expr - 输入表达式树（已由 transformExpr 转换）         
 * exprtype - expr 结果类型  
 * targettype - 目标结果类型 
 * targettypmod - 期望结果 typmod  
 * ccontext、cformat - 控制强制转换的上下文标记  
 * location - 强制转换请求的解析位置；未知/隐式时为 -1       
 */
Node *
coerce_to_target_type(ParseState *pstate, Node *expr, Oid exprtype,
					  Oid targettype, int32 targettypmod,
					  CoercionContext ccontext,
					  CoercionForm cformat,
					  int location)
{
	Node	   *result;
	Node	   *origexpr;

	if (!can_coerce_type(1, &exprtype, &targettype, ccontext))
		return NULL;

	/*
	 * 若输入顶层是 CollateExpr，则先剥掉它，                 
	 * 完成强制转换后再装回一个新的。这很烦人，   
	 * 因为它重复了 coerce_type 中的逻辑；但若不这样，      
	 * 就很难判断 coerce_type 是否真的有改动，而且我们  
	 * *必须*知道这一点，从而避免对并非 coerce_type     
	 * 生成的东西调用 hide_coercion_node。注意若堆叠了          
	 * 多个 CollateExpr，我们只保留最顶层的，其余丢弃。 
	 * 此外，若目标类型不可排序，我们丢弃 CollateExpr。    
	 */
	origexpr = expr;
	while (expr && IsA(expr, CollateExpr))
		expr = (Node *) ((CollateExpr *) expr)->arg;

	result = coerce_type(pstate, expr, exprtype,
						 targettype, targettypmod,
						 ccontext, cformat, location);

	/*
	 * 若目标是定长类型，可能除了类型强制转换外          
	 * 还需要长度强制转换。若发现两者都要加，则         
	 * 内层节点取隐式显示形式。         
	 */
	result = coerce_type_typmod(result,
								targettype, targettypmod,
								ccontext, cformat, location,
								(result != expr && !IsA(result, Const)));

	if (expr != origexpr && type_is_collatable(targettype))
	{
		/* 重装顶层 CollateExpr  */
		CollateExpr *coll = (CollateExpr *) origexpr;
		CollateExpr *newcoll = makeNode(CollateExpr);

		newcoll->arg = (Expr *) result;
		newcoll->collOid = coll->collOid;
		newcoll->location = coll->location;
		result = (Node *) newcoll;
	}

	return result;
}


/*
 * coerce_type()
 *		把表达式转换为另一种类型。   
 *
 * 调用者应已确认该强制转换可行；参见                     
 * can_coerce_type。  
 *
 * 通常，这里不会做任何到 typmod（长度）的强制转换。   
 * 调用者若需要 typmod 约束，还须调用 coerce_type_typmod。  
 * （但若目标类型是域，内部可能含 typmod 约束，  
 * 该约束将在 coerce_to_domain 内部实施。）               
 * 某些情况下，pg_cast 指定的类型强制转换函数还    
 * 会做长度转换，且仅在这些情况下，结果才会       
 * 已完成到指定 typmod 的强制转换。        
 *
 * pstate 仅在我们能解析出先前 UNKNOWN Param 的                
 * 类型时才会用到。若调用者不想更新 Param 的          
 * 类型信息，传 pstate = NULL 是可以的。         
 *
 * 注意：本函数绝不可修改给定的表达式树，只能在     
 * 其上添加装饰。例如参见 transformSetOperationTree。       
 */
Node *
coerce_type(ParseState *pstate, Node *node,
			Oid inputTypeId, Oid targetTypeId, int32 targetTypeMod,
			CoercionContext ccontext, CoercionForm cformat, int location)
{
	Node	   *result;
	CoercionPathType pathtype;
	Oid			funcId;

	if (targetTypeId == inputTypeId ||
		node == NULL)
	{
		/* 无需转换         */
		return node;
	}
	if (targetTypeId == ANYOID ||
		targetTypeId == ANYELEMENTOID ||
		targetTypeId == ANYNONARRAYOID ||
		targetTypeId == ANYCOMPATIBLEOID ||
		targetTypeId == ANYCOMPATIBLENONARRAYOID)
	{
		/*
		 * 假定 can_coerce_type 已验证隐式强制转换可行。    
		 *
		 * 注意：在这里返回未修改的节点，等于表示      
		 * UNKNOWN 常量可视为接受此类伪类型函数的      
		 * 合法输入。这应当没有问题，                         
		 * 毕竟 UNKNOWN 值仍是完全合法的 Datum。               
		 *
		 * 注意：这里绝不能加 RelabelType：函数参数        
		 * 暴露类型必须是实际类型，而非多态伪类型。  
		 *            
		 */
		return node;
	}
	if (targetTypeId == ANYARRAYOID ||
		targetTypeId == ANYENUMOID ||
		targetTypeId == ANYRANGEOID ||
		targetTypeId == ANYMULTIRANGEOID ||
		targetTypeId == ANYCOMPATIBLEARRAYOID ||
		targetTypeId == ANYCOMPATIBLERANGEOID ||
		targetTypeId == ANYCOMPATIBLEMULTIRANGEOID)
	{
		/*
		 * 假定 can_coerce_type 已验证隐式强制转换可行。    
		 *
		 * 这些情形不同于上文，因为参数暴露在外的类型  
		 * 必须是实际的数组、枚举、范围或多范围类型。 
		 * 尤其参数*绝不能*是 UNKNOWN 常量。              
		 * 若是，就直接落到下文；稍后会调用伪类型的    
		 * 输入函数，它将报错。另外，若得到的是             
		 * 数组、枚举、范围或多范围上的域，则须重标记为 
		 * 其基类型。                     
		 *
		 * 注意：目前在此处实际见不到枚举上的域，      
		 * 因为本文件的其他函数不会把这类参数匹配到
		 * ANYENUM。但这终究应当改变。                         
		 */
		if (inputTypeId != UNKNOWNOID)
		{
			Oid			baseTypeId = getBaseType(inputTypeId);

			if (baseTypeId != inputTypeId)
			{
				RelabelType *r = makeRelabelType((Expr *) node,
												 baseTypeId, -1,
												 InvalidOid,
												 cformat);

				r->location = location;
				return (Node *) r;
			}
			/* 非域类型，故原样返回        */
			return node;
		}
	}
	if (inputTypeId == UNKNOWNOID && IsA(node, Const))
	{
		/*
		 * 输入是此前类型未定的字符串常量。对其应用       
		 * 目标类型的 typinput 函数，以产出目标类型常量。  
		 *                 
		 *
		 * 注意：由于 typinput 行为未必与                
		 * 类型转换函数一致，本情形无法与另一      
		 * 常量输入情形合并。例如 int4 的 typinput           
		 * 函数会拒绝 "1.2"，而浮点转整数会舍入。      
		 * 为整数。                                       
		 *
		 * XXX：若 typinput 函数并非不可变，我们确实          
		 * 应当把该函数调用的求值推迟到运行时。但        
		 * 无法把 typinput 函数调用表示为表达式树，         
		 * 因为 C 字符串值不是 Datum。（XXX 自 7.3 起      
		 * 这*确实*已可行，要做吗？）   
		 */
		Const	   *con = (Const *) node;
		Const	   *newcon = makeNode(Const);
		Oid			baseTypeId;
		int32		baseTypeMod;
		int32		inputTypeMod;
		Type		baseType;
		ParseCallbackState pcbstate;

		/*
		 * 若目标类型是域，我们要调用其基础类型的      
		 * 输入例程而非 domain_in()。这是为了避免在域          
		 * 施加 typmod 时过早失败：现有输入例程对长度     
		 * 检查遵循隐式强制转换语义，这并不总是我们      
		 * 此处想要的。所需检查将由 coerce_to_domain()      
		 * 正确实施。           
		 */
		baseTypeMod = targetTypeMod;
		baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);

		/*
		 * 对多数类型，我们向输入例程传 typmod -1，因为 
		 * 现有输入例程对长度检查遵循隐式强制转换     
		 * 语义，这并不总是此处想要的。任何长度约束     
		 * 将由调用者稍后实施。例外是 INTERVAL 类型，   
		 * 因为我们*必须*为它传 typmod，否则它无法遵从     
		 * 古怪的 SQL 规范输入规则。（丑陋之极，              
		 * 不过规范这部分同样如此……） 
		 */
		if (baseTypeId == INTERVALOID)
			inputTypeMod = baseTypeMod;
		else
			inputTypeMod = -1;

		baseType = typeidType(baseTypeId);

		newcon->consttype = baseTypeId;
		newcon->consttypmod = inputTypeMod;
		newcon->constcollid = typeTypeCollation(baseType);
		newcon->constlen = typeLen(baseType);
		newcon->constbyval = typeByVal(baseType);
		newcon->constisnull = con->constisnull;

		/*
		 * 无论强制转换位于何处，我们都用原始字面值     
		 * 的位置。这与 9.2 之前的行为不同，旨在为          
		 * pg_stat_statements 提供便利。   
		 */
		newcon->location = con->location;

		/*
		 * 设置好位置，若输入例程报错则指向常量文本，   
		 *          
		 */
		setup_parser_errposition_callback(&pcbstate, pstate, con->location);

		/*
		 * 这里我们假定 UNKNOWN 的内部表示与 CSTRING            
		 * 相同。  
		 */
		if (!con->constisnull)
			newcon->constvalue = stringTypeDatum(baseType,
												 DatumGetCString(con->constvalue),
												 inputTypeMod);
		else
			newcon->constvalue = stringTypeDatum(baseType,
												 NULL,
												 inputTypeMod);

		/*
		 * 若为 varlena 值，则强制其为非展开          
		 * （非 TOAST）格式；这避免任何对外部值         
		 * 可能依赖，并改进表示的一致性。              
		 */
		if (!con->constisnull && newcon->constlen == -1)
			newcon->constvalue =
				PointerGetDatum(PG_DETOAST_DATUM(newcon->constvalue));

#ifdef RANDOMIZE_ALLOCATED_MEMORY

		/*
		 * 对引用传递的数据类型，重复该转换以查看        
		 * 输入函数是否在结果中留下未初始化的字节。       
		 * 只有启用 RANDOMIZE_ALLOCATED_MEMORY 才能可靠          
		 * 地检测到这一点，否则就不费心测试。我们不       
		 * 希望输入函数有任何不确定性的原因是，Const     
		 * 节点间的比较依赖于对 Datum 的逐字节比较，若     
		 * 输入函数留下垃圾数据，那么本应相同的子表达 
		 * 式可能无法被识别为相同。参见 pgsql-hackers    
		 * 2008-04-04 的讨论。  
		 */
		if (!con->constisnull && !newcon->constbyval)
		{
			Datum		val2;

			val2 = stringTypeDatum(baseType,
								   DatumGetCString(con->constvalue),
								   inputTypeMod);
			if (newcon->constlen == -1)
				val2 = PointerGetDatum(PG_DETOAST_DATUM(val2));
			if (!datumIsEqual(newcon->constvalue, val2, false, newcon->constlen))
				elog(WARNING, "type %s has unstable input conversion for \"%s\"",
					 typeTypeName(baseType), DatumGetCString(con->constvalue));
		}
#endif

		cancel_parser_errposition_callback(&pcbstate);

		result = (Node *) newcon;

		/* 若目标是域，则施加约束。      */
		if (baseTypeId != targetTypeId)
			result = coerce_to_domain(result,
									  baseTypeId, baseTypeMod,
									  targetTypeId,
									  ccontext, cformat, location,
									  false);

		ReleaseSysCache(baseType);

		return result;
	}
	if (IsA(node, Param) &&
		pstate != NULL && pstate->p_coerce_param_hook != NULL)
	{
		/*
		 * 让 CoerceParamHook 决定如何处理。它可以返回          
		 * 转换后的节点（多半是原 Param 节点），或        
		 * 返回 NULL 表示应继续正常强制转换。        
		 */
		result = pstate->p_coerce_param_hook(pstate,
											 (Param *) node,
											 targetTypeId,
											 targetTypeMod,
											 location);
		if (result)
			return result;
	}
	if (IsA(node, CollateExpr))
	{
		/*
		 * 若有 COLLATE 子句，就必须把强制转换压到   
		 * COLLATE 之下；若目标类型不可排序，则丢弃         
		 * COLLATE。这实在丑陋，但几乎没有别的选择，        
		 * 否则上述对 Const 和 Param 的这些特殊处理       
		 * 就不会发生。此杂凑手段后果见 coerce_to_target_type.
		 */
		CollateExpr *coll = (CollateExpr *) node;

		result = coerce_type(pstate, (Node *) coll->arg,
							 inputTypeId, targetTypeId, targetTypeMod,
							 ccontext, cformat, location);
		if (type_is_collatable(targetTypeId))
		{
			CollateExpr *newcoll = makeNode(CollateExpr);

			newcoll->arg = (Expr *) result;
			newcoll->collOid = coll->collOid;
			newcoll->location = coll->location;
			result = (Node *) newcoll;
		}
		return result;
	}
	pathtype = find_coercion_pathway(targetTypeId, inputTypeId, ccontext,
									 &funcId);
	if (pathtype != COERCION_PATH_NONE)
	{
		if (pathtype != COERCION_PATH_RELABELTYPE)
		{
			/*
			 * 生成一棵表示运行时应用转换函数的表达式    
			 * 树。若目标是域类型，转换函数将产出基础   
			 * 类型，于是我们需要从域的 typtypmod 中            
			 * 提取要用的正确 typmod。                          
			 *                    
			 */
			Oid			baseTypeId;
			int32		baseTypeMod;

			baseTypeMod = targetTypeMod;
			baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);

			result = build_coercion_expression(node, pathtype, funcId,
											   baseTypeId, baseTypeMod,
											   ccontext, cformat, location);

			/*
			 * 若是域，则强制转换到域类型，并以域类型   
			 * ID 重新标注，隐藏原转换节点。 
			 */
			if (targetTypeId != baseTypeId)
				result = coerce_to_domain(result, baseTypeId, baseTypeMod,
										  targetTypeId,
										  ccontext, cformat, location,
										  true);
		}
		else
		{
			/*
			 * 我们无需做物理转换，但确实需要附加一个   
			 * RelabelType 节点，使表达式在被上层代码          
			 * 检查时能看出具有预期类型。                       
			 *
			 * 此外，域可能有基础类型之外的取值限制，     
			 * 必须将其考虑在内。若目的地是域，           
			 * 则无需 RelabelType 节点。       
			 */
			result = coerce_to_domain(node, InvalidOid, -1, targetTypeId,
									  ccontext, cformat, location,
									  false);
			if (result == node)
			{
				/*
				 * XXX：能否改用 exprTypmod(node) 来标注结果，    
				 * 而非默认的 -1 typmod，以省去日后可能的 
				 * 长度强制转换？前提是两类型对 typmod 的      
				 * 解释相同；可能但不确定。    
				 */
				RelabelType *r = makeRelabelType((Expr *) result,
												 targetTypeId, -1,
												 InvalidOid,
												 cformat);

				r->location = location;
				result = (Node *) r;
			}
		}
		return result;
	}
	if (inputTypeId == RECORDOID &&
		ISCOMPLEX(targetTypeId))
	{
		/* 将 RECORD 转换为特定复合类型     */
		return coerce_record_to_complex(pstate, node, targetTypeId,
										ccontext, cformat, location);
	}
	if (targetTypeId == RECORDOID &&
		ISCOMPLEX(inputTypeId))
	{
		/* 把特定复合类型转成 RECORD       */
		/* 注意：此处不能加 RelabelType  */
		return node;
	}
#ifdef NOT_USED
	if (inputTypeId == RECORDARRAYOID &&
		is_complex_array(targetTypeId))
	{
		/* 将 record[] 转换为特定复合数组类型   */
		/* 尚未实现……      */
	}
#endif
	if (targetTypeId == RECORDARRAYOID &&
		is_complex_array(inputTypeId))
	{
		/* 将特定复合数组类型转换为 record[]    */
		/* 注意：此处不能加 RelabelType  */
		return node;
	}
	if (typeInheritsFrom(inputTypeId, targetTypeId)
		|| typeIsOfTypedTable(inputTypeId, targetTypeId))
	{
		/*
		 * 输入类类型是目标的子类，故生成一个     
		 * 合适的运行时转换（去掉不需要的列，并       
		 * 可能重排需要的列）。                 
		 *
		 * 当输入是目标类型某个子类之上的域时，也会       
		 * 走到这里。为让执行器处理简单，我们把           
		 * ConvertRowtypeExpr 定义为只在常规复合类型               
		 * 之间工作；因此这类情形下插入 RelabelType，       
		 * 将输入压回基础类型。   
		 */
		Oid			baseTypeId = getBaseType(inputTypeId);
		ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

		if (baseTypeId != inputTypeId)
		{
			RelabelType *rt = makeRelabelType((Expr *) node,
											  baseTypeId, -1,
											  InvalidOid,
											  COERCE_IMPLICIT_CAST);

			rt->location = location;
			node = (Node *) rt;
		}
		r->arg = (Expr *) node;
		r->resulttype = targetTypeId;
		r->convertformat = cformat;
		r->location = location;
		return (Node *) r;
	}
	/* 到此处说明调用者错了 */
	elog(ERROR, "failed to find conversion function from %s to %s",
		 format_type_be(inputTypeId), format_type_be(targetTypeId));
	return NULL;				/* 让编译器安静  */
}


/*
 * can_coerce_type()
 *		input_typeids 可否转成 target_typeids？   
 *
 * 必须被告知上下文（CAST 构造、赋值、隐式强制转换）， 
 * 因为这决定了可用转换的集合。    
 */
bool
can_coerce_type(int nargs, const Oid *input_typeids, const Oid *target_typeids,
				CoercionContext ccontext)
{
	bool		have_generics = false;
	int			i;

	/* 遍历参数列表……     */
	for (i = 0; i < nargs; i++)
	{
		Oid			inputTypeId = input_typeids[i];
		Oid			targetTypeId = target_typeids[i];
		CoercionPathType pathtype;
		Oid			funcId;

		/* 类型相同则无问题*/
		if (inputTypeId == targetTypeId)
			continue;

		/* 目标是 ANY 则接受 */
		if (targetTypeId == ANYOID)
			continue;

		/* 目标为多态类型则暂时接受     */
		if (IsPolymorphicType(targetTypeId))
		{
			have_generics = true;	/* 稍后做更多检查  */
			continue;
		}

		/*
		 * 输入未定类型字符串常量时，假定可转任意类型。 
		 *          
		 */
		if (inputTypeId == UNKNOWNOID)
			continue;

		/*
		 * 若 pg_cast 表明可以强制转换，则接受。该测试      
		 * 涵盖二进制兼容与转换函数两类。      
		 */
		pathtype = find_coercion_pathway(targetTypeId, inputTypeId, ccontext,
										 &funcId);
		if (pathtype != COERCION_PATH_NONE)
			continue;

		/*
		 * 若输入是 RECORD 且目标是复合类型，假定能        
		 * 转换（此处检查或需更严）   
		 */
		if (inputTypeId == RECORDOID &&
			ISCOMPLEX(targetTypeId))
			continue;

		/*
		 * 若输入是复合类型且目标是 RECORD，则接受  
		 */
		if (targetTypeId == RECORDOID &&
			ISCOMPLEX(inputTypeId))
			continue;

#ifdef NOT_USED					/* 尚未实现        */

		/*
		 * 若输入是 record[] 且目标是复合数组类型，假定    
		 * 能转换（此处检查或需更严）       
		 */
		if (inputTypeId == RECORDARRAYOID &&
			is_complex_array(targetTypeId))
			continue;
#endif

		/*
		 * 若输入是复合数组类型且目标是 record[]，接受     
		 */
		if (targetTypeId == RECORDARRAYOID &&
			is_complex_array(inputTypeId))
			continue;

		/*
		 * 若输入是继承自目标的类类型，接受          
		 */
		if (typeInheritsFrom(inputTypeId, targetTypeId)
			|| typeIsOfTypedTable(inputTypeId, targetTypeId))
			continue;

		/*
		 * 否则该参数位置无法强制转换      
		 */
		return false;
	}

	/* 若发现任何泛型参数类型，交叉检查它们   */
	if (have_generics)
	{
		if (!check_generic_type_consistency(input_typeids, target_typeids,
											nargs))
			return false;
	}

	return true;
}


/*
 * 创建一棵表示强制转换到域类型的表达式树。     
 *
 * 'arg'：输入表达式
 * 'baseTypeId'：域的基础类型，若已知（调用者未查       
 *		过则传 InvalidOid）          
 * 'baseTypeMod'：域的基础类型 typmod，若已知（调用        
 *		者未查过则传 -1）         
 * 'typeId'：转换目标类型     
 * 'ccontext'：控制强制转换的上下文标记 
 * 'cformat'：转换显示格式    
 * 'location'：强制转换请求位置
 * 'hideInputCoercion'：若为真，则把输入转换隐藏在本     
 *
 * 若目标类型不是域，则给定的 'arg' 原样返回。        
 */
Node *
coerce_to_domain(Node *arg, Oid baseTypeId, int32 baseTypeMod, Oid typeId,
				 CoercionContext ccontext, CoercionForm cformat, int location,
				 bool hideInputCoercion)
{
	CoerceToDomain *result;

	/* 若未提供基础类型则获取之         */
	if (baseTypeId == InvalidOid)
		baseTypeId = getBaseTypeAndTypmod(typeId, &baseTypeMod);

	/* 若不是域，则按传入原样返回该节点          */
	if (baseTypeId == typeId)
		return arg;

	/* 抑制嵌套强制转换步骤的显示   */
	if (hideInputCoercion)
		hide_coercion_node(arg);

	/*
	 * 若域对其基础类型施加 typmod，则构建相应的            
	 * 强制转换步骤。出于显示目的将其标为隐式，           
	 * 因为我们不想让 ruleutils.c 单独显示它；但传给          
	 * 转换函数的 isExplicit 标志取决于域强制转换的        
	 * 调用方式，隐式与显式强制转换语义由此             
	 * 有所差异。（这真是想要的行为吗？）     
	 *
	 * 注意：因为这是作为固定表达式结构的一部分          
	 * 施加的，ALTER DOMAIN 无法再改 typtypmod。但               
	 * 即便要改，在缺乏对基础类型如何理解 typmod          
	 * 的大量知识，未必安全。 
	 */
	arg = coerce_type_typmod(arg, baseTypeId, baseTypeMod,
							 ccontext, COERCE_IMPLICIT_CAST, location,
							 false);

	/*
	 * 现在构建域强制转换节点。它表示在运行时             
	 * 检查该域当前附带的所有约束。这还确保                 
	 * 表达式被正确标注结果类型。                   
	 */
	result = makeNode(CoerceToDomain);
	result->arg = (Expr *) arg;
	result->resulttype = typeId;
	result->resulttypmod = -1;	/* 当前对域而言总是 -1      */
	/* resultcollid 将由 parse_collate.c 设置  */
	result->coercionformat = cformat;
	result->location = location;

	return (Node *) result;
}


/*
 * coerce_type_typmod()
 *		若有意义且可行，则把值强制到特定 typmod。       
 *
 * 它作用于将要存入关系（列有 atttypmod）的值，      
 * 也作用于被显式 CAST 的值（typmod 来自目标            
 * 类型规格）。                                                   
 *
 * 调用者必须已确保值的类型正确，通常是通过         
 * 应用 coerce_type。                   
 *
 * ccontext 可能影响语义，取决于长度强制转换              
 * 函数是否留意传入的 isExplicit 标志。           
 *
 * cformat 决定所生成节点（若有）的显示属性。              
 *
 * 若 hideInputCoercion 为真*且*我们生成了节点，               
 * 输入节点被强制为 IMPLICIT 显示形式，这样显示表达式     
 * 时只有 typmod 转换节点可见。    
 *
 * 注意：这无需适用于域类型，因为域的任何           
 * typmod 强制转换都被视为生成域值本身所需的         
 * 类型强制转换的一部分。所以不做 getBaseType。               
 */
static Node *
coerce_type_typmod(Node *node, Oid targetTypeId, int32 targetTypMod,
				   CoercionContext ccontext, CoercionForm cformat,
				   int location,
				   bool hideInputCoercion)
{
	CoercionPathType pathtype;
	Oid			funcId;

	/* 若已转换则跳过         */
	if (targetTypMod == exprTypmod(node))
		return node;

	/* 抑制嵌套强制转换步骤的显示   */
	if (hideInputCoercion)
		hide_coercion_node(node);

	/*
	 * typmod 为负意味着无需实际转换，但我们仍然             
	 * 想要 RelabelType，确保表达式暴露预期的 typmod。        
	 *        
	 */
	if (targetTypMod < 0)
		pathtype = COERCION_PATH_NONE;
	else
		pathtype = find_typmod_coercion_function(targetTypeId, &funcId);

	if (pathtype != COERCION_PATH_NONE)
	{
		node = build_coercion_expression(node, pathtype, funcId,
										 targetTypeId, targetTypMod,
										 ccontext, cformat, location);
	}
	else
	{
		/*
		 * 我们无需执行任何实际转换步骤，但应附加       
		 * 一个 RelabelType，确保表达式暴露预期的          
		 * typmod。       
		 */
		node = applyRelabelType(node, targetTypeId, targetTypMod,
								exprCollation(node),
								cformat, location, false);
	}

	return node;
}

/*
 * 把强制转换节点标为 IMPLICIT，使 ruleutils.c 永远    
 * 不会显示它。当我们生成一组嵌套的强制转换       
 * 节点来实现逻辑上的单一转换时使用；内层         
 * 节点被强制为 IMPLICIT_CAST 格式。这不改变其语义，    
 * 只改显示行为。 
 *
 * 对没有 CoercionForm 字段的东西调用它属于           
 *                    
 */
static void
hide_coercion_node(Node *node)
{
	if (IsA(node, FuncExpr))
		((FuncExpr *) node)->funcformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, RelabelType))
		((RelabelType *) node)->relabelformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, CoerceViaIO))
		((CoerceViaIO *) node)->coerceformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, ArrayCoerceExpr))
		((ArrayCoerceExpr *) node)->coerceformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, ConvertRowtypeExpr))
		((ConvertRowtypeExpr *) node)->convertformat = COERCE_IMPLICIT_CAST;
	else if (IsA(node, RowExpr))
		((RowExpr *) node)->row_format = COERCE_IMPLICIT_CAST;
	else if (IsA(node, CoerceToDomain))
		((CoerceToDomain *) node)->coercionformat = COERCE_IMPLICIT_CAST;
	else
		elog(ERROR, "unsupported node type: %d", (int) nodeTag(node));
}

/*
 * build_coercion_expression()
 *		构建应用 pg_cast 条目的表达式树。             
 *
 * 类型强制转换与长度强制转换操作都会用到它，    
 * 因为两者在调用约定上没有差别。                   
 */
static Node *
build_coercion_expression(Node *node,
						  CoercionPathType pathtype,
						  Oid funcId,
						  Oid targetTypeId, int32 targetTypMod,
						  CoercionContext ccontext, CoercionForm cformat,
						  int location)
{
	int			nargs = 0;

	if (OidIsValid(funcId))
	{
		HeapTuple	tp;
		Form_pg_proc procstruct;

		tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for function %u", funcId);
		procstruct = (Form_pg_proc) GETSTRUCT(tp);

		/*
		 * 这些 Assert 实质上是在检查 function 是合法的        
		 * 强制转换函数。我们无法对 prorettype 和               
		 * proargtypes[0] 做看似显然的测试，即便 COERCION_PATH_FUNC
		 * 下也不行：二进制兼容。  
		 */
		/* Assert(targetTypeId == procstruct->prorettype); */
		Assert(!procstruct->proretset);
		Assert(procstruct->prokind == PROKIND_FUNCTION);
		nargs = procstruct->pronargs;
		Assert(nargs >= 1 && nargs <= 3);
		/* Assert(procstruct->proargtypes.values[0] == exprType(node)); */
		Assert(nargs < 2 || procstruct->proargtypes.values[1] == INT4OID);
		Assert(nargs < 3 || procstruct->proargtypes.values[2] == BOOLOID);

		ReleaseSysCache(tp);
	}

	if (pathtype == COERCION_PATH_FUNC)
	{
		/* 我们构建带特殊参数的普通 FuncExpr        */
		FuncExpr   *fexpr;
		List	   *args;
		Const	   *cons;

		Assert(OidIsValid(funcId));

		args = list_make1(node);

		if (nargs >= 2)
		{
			/* 以 int4 常量传入目标 typmod     */
			cons = makeConst(INT4OID,
							 -1,
							 InvalidOid,
							 sizeof(int32),
							 Int32GetDatum(targetTypMod),
							 false,
							 true);

			args = lappend(args, cons);
		}

		if (nargs == 3)
		{
			/* 同时传入布尔 isExplicit 参数        */
			cons = makeConst(BOOLOID,
							 -1,
							 InvalidOid,
							 sizeof(bool),
							 BoolGetDatum(ccontext == COERCION_EXPLICIT),
							 false,
							 true);

			args = lappend(args, cons);
		}

		fexpr = makeFuncExpr(funcId, targetTypeId, args,
							 InvalidOid, InvalidOid, cformat);
		fexpr->location = location;
		return (Node *) fexpr;
	}
	else if (pathtype == COERCION_PATH_ARRAYCOERCE)
	{
		/* 我们需要构建 ArrayCoerceExpr  */
		ArrayCoerceExpr *acoerce = makeNode(ArrayCoerceExpr);
		CaseTestExpr *ctest = makeNode(CaseTestExpr);
		Oid			sourceBaseTypeId;
		int32		sourceBaseTypeMod;
		Oid			targetElementType;
		Node	   *elemexpr;

		/*
		 * 穿透源数组类型之上的任何域。注意我们不         
		 * 预期目标类型是域；它必须是纯数组。               
		 * （要到域目标类型，稍后会做 coerce_to_domain。）    
		 */
		sourceBaseTypeMod = exprTypmod(node);
		sourceBaseTypeId = getBaseTypeAndTypmod(exprType(node),
												&sourceBaseTypeMod);

		/*
		 * 设置一个 CaseTestExpr 表示源数组的单个元素。        
		 * 这是对 CaseTestExpr 的滥用，但只要最终的         
		 * elemexpr 中不可能有 CaseExpr 或 ArrayCoerceExpr，     
		 *          
		 */
		ctest->typeId = get_element_type(sourceBaseTypeId);
		Assert(OidIsValid(ctest->typeId));
		ctest->typeMod = sourceBaseTypeMod;
		ctest->collation = InvalidOid;	/* 假定强制转换不关心 */

		/* 并将其转换为目标元素类型     */
		targetElementType = get_element_type(targetTypeId);
		Assert(OidIsValid(targetElementType));

		elemexpr = coerce_to_target_type(NULL,
										 (Node *) ctest,
										 ctest->typeId,
										 targetElementType,
										 targetTypMod,
										 ccontext,
										 cformat,
										 location);
		if (elemexpr == NULL)	/* 不应发生     */
			elog(ERROR, "failed to coerce array element type as expected");

		acoerce->arg = (Expr *) node;
		acoerce->elemexpr = (Expr *) elemexpr;
		acoerce->resulttype = targetTypeId;

		/*
		 * 只有当我们最终得到的逐元素表达式被如此        
		 * 标注时，才把输出标为具有特定元素 typmod。      
		 */
		acoerce->resulttypmod = exprTypmod(elemexpr);
		/* resultcollid 将由 parse_collate.c 设置  */
		acoerce->coerceformat = cformat;
		acoerce->location = location;

		return (Node *) acoerce;
	}
	else if (pathtype == COERCION_PATH_COERCEVIAIO)
	{
		/* 需要构建 CoerceViaIO 节点     */
		CoerceViaIO *iocoerce = makeNode(CoerceViaIO);

		Assert(!OidIsValid(funcId));

		iocoerce->arg = (Expr *) node;
		iocoerce->resulttype = targetTypeId;
		/* resultcollid 将由 parse_collate.c 设置  */
		iocoerce->coerceformat = cformat;
		iocoerce->location = location;

		return (Node *) iocoerce;
	}
	else
	{
		elog(ERROR, "unsupported pathtype %d in build_coercion_expression",
			 (int) pathtype);
		return NULL;			/* 让编译器安静  */
	}
}


/*
 * coerce_record_to_complex
 *		把 RECORD 强制转换为复合类型。    
 *
 * 目前仅支持输入为 RowExpr 或整行 Var 的情况。             
 *      
 */
static Node *
coerce_record_to_complex(ParseState *pstate, Node *node,
						 Oid targetTypeId,
						 CoercionContext ccontext,
						 CoercionForm cformat,
						 int location)
{
	RowExpr    *rowexpr;
	Oid			baseTypeId;
	int32		baseTypeMod = -1;
	TupleDesc	tupdesc;
	List	   *args = NIL;
	List	   *newargs;
	int			i;
	int			ucolno;
	ListCell   *arg;

	if (node && IsA(node, RowExpr))
	{
		/*
		 * 既然 RowExpr 必为 RECORD 类型，无需担心其中           
		 * 含有被删除的列。       
		 */
		args = ((RowExpr *) node)->args;
	}
	else if (node && IsA(node, Var) &&
			 ((Var *) node)->varattno == InvalidAttrNumber)
	{
		int			rtindex = ((Var *) node)->varno;
		int			sublevels_up = ((Var *) node)->varlevelsup;
		int			vlocation = ((Var *) node)->location;
		ParseNamespaceItem *nsitem;

		nsitem = GetNSItemByRangeTablePosn(pstate, rtindex, sublevels_up);
		args = expandNSItemVars(pstate, nsitem, sublevels_up, vlocation, NULL);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(RECORDOID),
						format_type_be(targetTypeId)),
				 parser_coercion_errposition(pstate, location, node)));

	/*
	 * 查找复合类型，并考虑给定的可能是复合                 
	 * 类型之上的域。            
	 */
	baseTypeId = getBaseTypeAndTypmod(targetTypeId, &baseTypeMod);
	tupdesc = lookup_rowtype_tupdesc(baseTypeId, baseTypeMod);

	/* 处理各字段    */
	newargs = NIL;
	ucolno = 1;
	arg = list_head(args);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Node	   *expr;
		Node	   *cexpr;
		Oid			exprtype;
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		/* 为 rowtype 中被删除的列填入 NULL    */
		if (attr->attisdropped)
		{
			/*
			 * 这里无法使用 atttypid，不过 Const 声称的类型     
			 * 其实无关紧要。  
			 */
			newargs = lappend(newargs,
							  makeNullConst(INT4OID, -1, InvalidOid));
			continue;
		}

		if (arg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(RECORDOID),
							format_type_be(targetTypeId)),
					 errdetail("Input has too few columns."),
					 parser_coercion_errposition(pstate, location, node)));
		expr = (Node *) lfirst(arg);
		exprtype = exprType(expr);

		cexpr = coerce_to_target_type(pstate,
									  expr, exprtype,
									  attr->atttypid,
									  attr->atttypmod,
									  ccontext,
									  COERCE_IMPLICIT_CAST,
									  -1);
		if (cexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CANNOT_COERCE),
					 errmsg("cannot cast type %s to %s",
							format_type_be(RECORDOID),
							format_type_be(targetTypeId)),
					 errdetail("Cannot cast type %s to %s in column %d.",
							   format_type_be(exprtype),
							   format_type_be(attr->atttypid),
							   ucolno),
					 parser_coercion_errposition(pstate, location, expr)));
		newargs = lappend(newargs, cexpr);
		ucolno++;
		arg = lnext(args, arg);
	}
	if (arg != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(RECORDOID),
						format_type_be(targetTypeId)),
				 errdetail("Input has too many columns."),
				 parser_coercion_errposition(pstate, location, node)));

	ReleaseTupleDesc(tupdesc);

	rowexpr = makeNode(RowExpr);
	rowexpr->args = newargs;
	rowexpr->row_typeid = baseTypeId;
	rowexpr->row_format = cformat;
	rowexpr->colnames = NIL;	/* 命名目标类型不需要      */
	rowexpr->location = location;

	/* 若目标是域，则施加约束        */
	if (baseTypeId != targetTypeId)
	{
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		return coerce_to_domain((Node *) rowexpr,
								baseTypeId, baseTypeMod,
								targetTypeId,
								ccontext, cformat, location,
								false);
	}

	return (Node *) rowexpr;
}

/*
 * coerce_to_boolean()
 *		把需要布尔输入的构造（AND、OR、NOT 等）的    
 *		参数强制转换，并检查输入不是集合。      
 *
 * 返回（可能已变换的）节点树。 
 *
 * 与 coerce_type 一样，若不需要对未知 Param 的           
 * 处理，传 NULL。 
 */
Node *
coerce_to_boolean(ParseState *pstate, Node *node,
				  const char *constructName)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId != BOOLOID)
	{
		Node	   *newnode;

		newnode = coerce_to_target_type(pstate, node, inputTypeId,
										BOOLOID, -1,
										COERCION_ASSIGNMENT,
										COERCE_IMPLICIT_CAST,
										-1);
		if (newnode == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/* translator: first %s is name of a SQL construct, eg WHERE */
					 errmsg("argument of %s must be type %s, not type %s",
							constructName, "boolean",
							format_type_be(inputTypeId)),
					 parser_errposition(pstate, exprLocation(node))));
		node = newnode;
	}

	if (expression_returns_set(node))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg WHERE */
				 errmsg("argument of %s must not return a set",
						constructName),
				 parser_errposition(pstate, exprLocation(node))));

	return node;
}

/*
 * coerce_to_specific_type_typmod()
 *		把需要特定数据类型、特定 typmod 的构造的参          
 *		数强制转换，并检查输入不是集合。            
 *
 * 返回（可能已变换的）节点树。 
 *
 * 与 coerce_type 一样，若不需要对未知 Param 的           
 * 处理，传 NULL。 
 */
Node *
coerce_to_specific_type_typmod(ParseState *pstate, Node *node,
							   Oid targetTypeId, int32 targetTypmod,
							   const char *constructName)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId != targetTypeId)
	{
		Node	   *newnode;

		newnode = coerce_to_target_type(pstate, node, inputTypeId,
										targetTypeId, targetTypmod,
										COERCION_ASSIGNMENT,
										COERCE_IMPLICIT_CAST,
										-1);
		if (newnode == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/* translator: first %s is name of a SQL construct, eg LIMIT */
					 errmsg("argument of %s must be type %s, not type %s",
							constructName,
							format_type_be(targetTypeId),
							format_type_be(inputTypeId)),
					 parser_errposition(pstate, exprLocation(node))));
		node = newnode;
	}

	if (expression_returns_set(node))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not return a set",
						constructName),
				 parser_errposition(pstate, exprLocation(node))));

	return node;
}

/*
 * coerce_to_specific_type()
 *		对需要特定数据类型的构造，强制转换其参数。      
 *		同时检查输入不是集合。  
 *
 * 返回可能已被变换的节点树。    
 *
 * 同 coerce_type，若无需特殊 unknown-Param 处理，则       
 * pstate 可为 NULL。
 */
Node *
coerce_to_specific_type(ParseState *pstate, Node *node,
						Oid targetTypeId,
						const char *constructName)
{
	return coerce_to_specific_type_typmod(pstate, node,
										  targetTypeId, -1,
										  constructName);
}

/*
 * coerce_null_to_domain()
 *		构建 NULL 常量，目标类型为域时包进     
 *		CoerceToDomain。这样任何 NOT NULL 域约束      
 *		都能在运行时得到强制执行。              
 */
Node *
coerce_null_to_domain(Oid typid, int32 typmod, Oid collation,
					  int typlen, bool typbyval)
{
	Node	   *result;
	Oid			baseTypeId;
	int32		baseTypeMod = typmod;

	/*
	 * 该常量必须呈现为域的基本类型/typmod，否则          
	 * coerce_to_domain() 将施加无用的长度强制转换。       
	 */
	baseTypeId = getBaseTypeAndTypmod(typid, &baseTypeMod);
	result = (Node *) makeConst(baseTypeId,
								baseTypeMod,
								collation,
								typlen,
								(Datum) 0,
								true,	/* isnull */
								typbyval);
	if (typid != baseTypeId)
		result = coerce_to_domain(result,
								  baseTypeId, baseTypeMod,
								  typid,
								  COERCION_IMPLICIT,
								  COERCE_IMPLICIT_CAST,
								  -1,
								  false);
	return result;
}

/*
 * parser_coercion_errposition - 尽量报告类型强制转换错误位置 
 *
 * 若可能，我们倾向指向强制转换请求（CAST、:: 等）； 
 * 但隐式强制转换的情形下可能没有这样的位置。       
 * 此时就指向输入表达式。          
 *
 * XXX 也许它的用途比强制转换错误报告更广；      
 * 若如此，宜改名并靠近 parser_errposition。   
 */
int
parser_coercion_errposition(ParseState *pstate,
							int coerce_location,
							Node *input_expr)
{
	if (coerce_location >= 0)
		return parser_errposition(pstate, coerce_location);
	else
		return parser_errposition(pstate, exprLocation(input_expr));
}


/*
 * select_common_type()
 *		确定一组输入表达式的公共超类型。              
 *		它用于确定 CASE、UNION 等构造的输出类型，    
 *		以及类似构造。  
 *
 * 'exprs' 是表达式组成的*非空*列表。注意：列表中     
 * 靠前的项在存疑时优先选用。         
 * 'context' 是选取不到可用类型时错误消息采用的措辞。
 * 传入 NULL 可让例程返回 InvalidOid，                   
 * 而非在失败时抛出错误。        
 * 'which_expr'：若非 NULL，会接收指向实际选定输入       
 * 表达式的指针，即结果类型来源。   
 *
 * 注意："失败"仅表示存在属于不同类型类别的输入，  
 * 并非保证所有输入能强制转换到所选类型；调用者必须  
 * 自行检查这一点（见 verify_common_type）。           
 */
Oid
select_common_type(ParseState *pstate, List *exprs, const char *context,
				   Node **which_expr)
{
	Node	   *pexpr;
	Oid			ptype;
	TYPCATEGORY pcategory;
	bool		pispreferred;
	ListCell   *lc;

	Assert(exprs != NIL);
	pexpr = (Node *) linitial(exprs);
	lc = list_second_cell(exprs);
	ptype = exprType(pexpr);

	/*
	 * 若所有输入类型均有效且完全相同，就直接选它。     
	 * 只有这条路能把结果定为域类型；其他情况下域都会 
	 * 被打回基本类型再参与比较。                                
	 */
	if (ptype != UNKNOWNOID)
	{
		for_each_cell(lc, exprs, lc)
		{
			Node	   *nexpr = (Node *) lfirst(lc);
			Oid			ntype = exprType(nexpr);

			if (ntype != ptype)
				break;
		}
		if (lc == NULL)			/* 已走到列表末尾？    */
		{
			if (which_expr)
				*which_expr = pexpr;
			return ptype;
		}
	}

	/*
	 * 并非如此，故设置完整算法。注意此时 lc 指向       
	 * 第一个类型与 pexpr 不同的列表项；此前循环扫过的项 
	 * 无需重新检查。                                    
	 */
	ptype = getBaseType(ptype);
	get_type_category_preferred(ptype, &pcategory, &pispreferred);

	for_each_cell(lc, exprs, lc)
	{
		Node	   *nexpr = (Node *) lfirst(lc);
		Oid			ntype = getBaseType(exprType(nexpr));

		/* 没有新信息就继续看下一项……   */
		if (ntype != UNKNOWNOID && ntype != ptype)
		{
			TYPCATEGORY ncategory;
			bool		nispreferred;

			get_type_category_preferred(ntype, &ncategory, &nispreferred);
			if (ptype == UNKNOWNOID)
			{
				/* 至今全是未知，随便接受……   */
				pexpr = nexpr;
				ptype = ntype;
				pcategory = ncategory;
				pispreferred = nispreferred;
			}
			else if (ncategory != pcategory)
			{
				/*
				 * 两个类型分属不同类别？那希望不大……   
				 */
				if (context == NULL)
					return InvalidOid;
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
				/*------
				  translator: first %s is name of a SQL construct, eg CASE */
						 errmsg("%s types %s and %s cannot be matched",
								context,
								format_type_be(ptype),
								format_type_be(ntype)),
						 parser_errposition(pstate, exprLocation(nexpr))));
			}
			else if (!pispreferred &&
					 can_coerce_type(1, &ptype, &ntype, COERCION_IMPLICIT) &&
					 !can_coerce_type(1, &ntype, &ptype, COERCION_IMPLICIT))
			{
				/*
				 * 若能隐式转到新类型而反向不行，则采用  
				 * 但若已有优先类型，则坚持用。             
				 */
				pexpr = nexpr;
				ptype = ntype;
				pcategory = ncategory;
				pispreferred = nispreferred;
			}
		}
	}

	/*
	 * 若所有输入都是 UNKNOWN 类型 --- 即未知类型字面量 ---
	 * 那就定为 TEXT 类型。以下构造会出现这种情况：    
	 * 例如 SELECT (CASE WHEN foo THEN 'bar' ELSE 'baz' END)、         
	 * SELECT 'foo' UNION SELECT 'bar'；似乎让该构造的输出       
	 * 类型保持 UNKNOWN 似可取，但实际行不通，因为我们
	 * 多半需要从 UNKNOWN 到其他类型的运行时强制转换，  
	 * 而我们通常没有，必须趁未知字面量仍是字面量时
	 * 对它强制转换，所以现在就必须做出决定。           
	 *     
	 */
	if (ptype == UNKNOWNOID)
		ptype = TEXTOID;

	if (which_expr)
		*which_expr = pexpr;
	return ptype;
}

/*
 * select_common_type_from_oids()
 *		确定一个类型 OID 数组的公共超类型。      
 *
 * 逻辑与 select_common_type() 相同，但输入是类型      
 * OID 数组而非表达式列表。与该函数一样，数组中      
 * 靠前的项比靠后的项略受优先。                        
 * 失败时，若 noerror 为真则返回 InvalidOid，否则报错。  
 *
 * 注意："失败"仅表示存在属于不同类型类别的输入，  
 * 并非保证所有输入能强制转换到所选类型；调用者必须  
 * 自行检查这一点（见 verify_common_type_from_oids）。           
 *
 * 注意：两个调用者都不会传入 UNKNOWNOID 项，因此    
 * 本函数中的相关测试是死代码。不过它们开销不大，   
 * 而且让这段逻辑尽量贴近 select_common_type() 似乎          
 * 更好。   
 */
static Oid
select_common_type_from_oids(int nargs, const Oid *typeids, bool noerror)
{
	Oid			ptype;
	TYPCATEGORY pcategory;
	bool		pispreferred;
	int			i = 1;

	Assert(nargs > 0);
	ptype = typeids[0];

	/* 若所有输入类型均有效且完全相同，就选该类型。 */
	if (ptype != UNKNOWNOID)
	{
		for (; i < nargs; i++)
		{
			if (typeids[i] != ptype)
				break;
		}
		if (i == nargs)
			return ptype;
	}

	/*
	 * 并非如此，故设置完整算法。注意此时我们可以     
	 * 跳过 "i" 之前的数组项；它们全都等于 ptype。    
	 */
	ptype = getBaseType(ptype);
	get_type_category_preferred(ptype, &pcategory, &pispreferred);

	for (; i < nargs; i++)
	{
		Oid			ntype = getBaseType(typeids[i]);

		/* 没有新信息就继续看下一项……   */
		if (ntype != UNKNOWNOID && ntype != ptype)
		{
			TYPCATEGORY ncategory;
			bool		nispreferred;

			get_type_category_preferred(ntype, &ncategory, &nispreferred);
			if (ptype == UNKNOWNOID)
			{
				/* 至今全是未知，随便接受……   */
				ptype = ntype;
				pcategory = ncategory;
				pispreferred = nispreferred;
			}
			else if (ncategory != pcategory)
			{
				/*
				 * 两个类型分属不同类别？那希望不大……   
				 */
				if (noerror)
					return InvalidOid;
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument types %s and %s cannot be matched",
								format_type_be(ptype),
								format_type_be(ntype))));
			}
			else if (!pispreferred &&
					 can_coerce_type(1, &ptype, &ntype, COERCION_IMPLICIT) &&
					 !can_coerce_type(1, &ntype, &ptype, COERCION_IMPLICIT))
			{
				/*
				 * 若能隐式转到新类型而反向不行，则采用  
				 * 但若已有优先类型，则坚持用。             
				 */
				ptype = ntype;
				pcategory = ncategory;
				pispreferred = nispreferred;
			}
		}
	}

	/* 同 select_common_type()，输入全为 UNKNOWN 时选 TEXT       */
	if (ptype == UNKNOWNOID)
		ptype = TEXTOID;

	return ptype;
}

/*
 * coerce_to_common_type()
 *		将表达式转为给定类型。      
 *
 * 它在 select_common_type() 之后使用，把各表达式          
 * 逐个强制转换到所需类型。'context' 是无法完成        
 * 失败时错误消息的措辞。  
 *
 * 同 coerce_type，若无需特殊 unknown-Param 处理，          
 * pstate 可为 NULL。
 */
Node *
coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId, const char *context)
{
	Oid			inputTypeId = exprType(node);

	if (inputTypeId == targetTypeId)
		return node;			/* 无事  */
	if (can_coerce_type(1, &inputTypeId, &targetTypeId, COERCION_IMPLICIT))
		node = coerce_type(pstate, node, inputTypeId, targetTypeId, -1,
						   COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	else
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
		/* translator: first %s is name of a SQL construct, eg CASE */
				 errmsg("%s could not convert type %s to %s",
						context,
						format_type_be(inputTypeId),
						format_type_be(targetTypeId)),
				 parser_errposition(pstate, exprLocation(node))));
	return node;
}

/*
 * verify_common_type()
 *		验证所有输入类型都能强制转换到提议的公共类型。
 *		若如此返回真；若有转换做不到则返回假。  
 *
 * select_common_type() 的大多数调用者无需显式               
 * 做这件事，因为在尝试把输入表达式转换成正确类型时
 * （例如在 coerce_to_common_type() 中）自然完成。但若需要     
 * 单独的检查步骤来验证公共类型的适用性，就调用本函数
 *      
 */
bool
verify_common_type(Oid common_type, List *exprs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *nexpr = (Node *) lfirst(lc);
		Oid			ntype = exprType(nexpr);

		if (!can_coerce_type(1, &ntype, &common_type, COERCION_IMPLICIT))
			return false;
	}
	return true;
}

/*
 * verify_common_type_from_oids()
 *		同上，但输入为类型 OID 数组。     
 */
static bool
verify_common_type_from_oids(Oid common_type, int nargs, const Oid *typeids)
{
	for (int i = 0; i < nargs; i++)
	{
		if (!can_coerce_type(1, &typeids[i], &common_type, COERCION_IMPLICIT))
			return false;
	}
	return true;
}

/*
 * select_common_typmod()
 *		确定一组输入表达式的公共 typmod。             
 *
 * common_type 是这些表达式选定的公共类型，通常         
 * 由 select_common_type() 算出。  
 */
int32
select_common_typmod(ParseState *pstate, List *exprs, Oid common_type)
{
	ListCell   *lc;
	bool		first = true;
	int32		result = -1;

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		/* 类型须一致  */
		if (exprType(expr) != common_type)
			return -1;
		else if (first)
		{
			result = exprTypmod(expr);
			first = false;
		}
		else
		{
			/* 一看到不匹配的 typmod，就退回 -1              */
			if (result != exprTypmod(expr))
				return -1;
		}
	}

	return result;
}

/*
 * check_generic_type_consistency()
 *		实际参数是否有可能与                        
 *		多态函数兼容？
 *
 * 参数一致性规则如下：     
 *
 * 1) 声明为 ANYELEMENT 的所有参数必须同类型。        
 * 2) 声明为 ANYARRAY 的所有参数必须同类型，且     
 *	  须为 varlena 数组类型。     
 * 3) 声明为 ANYRANGE 的参数须是同一范围类型。     
 *	  同样，声明为 ANYMULTIRANGE 的所有参数必须是同    
 *	  一多范围类型；若两者都出现，则 ANYRANGE 类型  
 *	  必须是 ANYMULTIRANGE 类型的元素类型。   
 * 4) 若存在多个上述多态类型的参数，则数组元素类型 
 *	  和/或范围子类型必须彼此相同，且与 ANYELEMENT 类型 
 *	  相同。                                 
 * 5) ANYENUM 的处理与 ANYELEMENT 相同，但若使用它           
 *	  （单独或与普通 ANYELEMENT 组合），我们会附加     
 *	  条件：ANYELEMENT 类型必须是枚举。       
 * 6) ANYNONARRAY 的处理与 ANYELEMENT 相同，但若使用它，         
 *	  我们会附加条件：ANYELEMENT 类型不能是数组。              
 *	  （与 ANYARRAY 或 ANYENUM 组合使用时这是空操作，        
 *	  否则是额外限制。）     
 * 7) 声明为 ANYCOMPATIBLE 的所有参数必须可隐式          
 *	  转换到公共超类型（按 select_common_type 的规则）。
 *	  ANYCOMPATIBLENONARRAY 与 ANYCOMPATIBLE 类似，但还             
 *	  需要公共超类型不是数组。若存在 ANYCOMPATIBLEARRAY     
 *	  或 ANYCOMPATIBLERANGE 或 ANYCOMPATIBLEMULTIRANGE 参数，             
 *	  则它们的元素类型或子类型也会纳入公共超类型的选取。
 * 8) ANYCOMPATIBLEARRAY 参数最终确定的类型将是               
 *	  以公共超类型为元素类型的数组类型（可能不同于    
 *	  任一原始数组）。       
 * 9) 所有 ANYCOMPATIBLERANGE 参数必须是完全相同的          
 *	  范围类型（展平域后），因为我们没有优先规则可在 
 *	  其中做出取舍。此外，该范围类型的子类型必须   
 *	  与规则 7 选出的公共超类型完全一致。      
 * 10) 所有 ANYCOMPATIBLEMULTIRANGE 参数必须是完全相同             
 *	  的多范围类型（展平域后），因为我们没有优先规则
 *	  可在其中做出取舍。此外，若还出现了 ANYCOMPATIBLERANGE，
 *	  则该范围类型必须是多范围类型的元素类型；        
 *	  否则，多范围类型所属范围类型的子类型必须      
 *	  同规则 7 超类型一致。    
 *
 * 数组上的域匹配 ANYARRAY，并立即被展平为基本类型。     
 * （例如，若一个 ANYARRAY 参数是 int4[] 上的域，而另         
 * 一个参数只是 int4[]，我们会认为匹配。）另外注意，  
 * 这样的域*不能*匹配 ANYNONARRAY。ANYCOMPATIBLEARRAY           
 * 与 ANYCOMPATIBLENONARRAY 同理。              
 *
 * 同样，范围上的域匹配 ANYRANGE 或 ANYCOMPATIBLERANGE，   
 * 并展平为基本类型。同理，多范围上的域匹配        
 * ANYMULTIRANGE 或 ANYCOMPATIBLEMULTIRANGE，并展平为               
 * 基本类型。                          
 *
 * 注意：目前认为域不能匹配 ANYENUM，即使其基本 
 * 类型本可匹配。               
 *
 * 若任何多态参数的输入是 UNKNOWN（即未定型字面量） 
 * 就假定它没问题。    
 *
 * 这里不做 ereport，若某条规则被违反就只返回假。   
 */
bool
check_generic_type_consistency(const Oid *actual_arg_types,
							   const Oid *declared_arg_types,
							   int nargs)
{
	Oid			elem_typeid = InvalidOid;
	Oid			array_typeid = InvalidOid;
	Oid			range_typeid = InvalidOid;
	Oid			multirange_typeid = InvalidOid;
	Oid			anycompatible_range_typeid = InvalidOid;
	Oid			anycompatible_range_typelem = InvalidOid;
	Oid			anycompatible_multirange_typeid = InvalidOid;
	Oid			anycompatible_multirange_typelem = InvalidOid;
	Oid			range_typelem = InvalidOid;
	bool		have_anynonarray = false;
	bool		have_anyenum = false;
	bool		have_anycompatible_nonarray = false;
	int			n_anycompatible_args = 0;
	Oid			anycompatible_actual_types[FUNC_MAX_ARGS];

	/*
	 * 遍历各参数，看是否存在多态参数。                      
	 * 若有，则需要实际类型保持一致。    
	 */
	Assert(nargs <= FUNC_MAX_ARGS);
	for (int j = 0; j < nargs; j++)
	{
		Oid			decl_type = declared_arg_types[j];
		Oid			actual_type = actual_arg_types[j];

		if (decl_type == ANYELEMENTOID ||
			decl_type == ANYNONARRAYOID ||
			decl_type == ANYENUMOID)
		{
			if (decl_type == ANYNONARRAYOID)
				have_anynonarray = true;
			else if (decl_type == ANYENUMOID)
				have_anyenum = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (OidIsValid(elem_typeid) && actual_type != elem_typeid)
				return false;
			elem_typeid = actual_type;
		}
		else if (decl_type == ANYARRAYOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(array_typeid) && actual_type != array_typeid)
				return false;
			array_typeid = actual_type;
		}
		else if (decl_type == ANYRANGEOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(range_typeid) && actual_type != range_typeid)
				return false;
			range_typeid = actual_type;
		}
		else if (decl_type == ANYMULTIRANGEOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(multirange_typeid) && actual_type != multirange_typeid)
				return false;
			multirange_typeid = actual_type;
		}
		else if (decl_type == ANYCOMPATIBLEOID ||
				 decl_type == ANYCOMPATIBLENONARRAYOID)
		{
			if (decl_type == ANYCOMPATIBLENONARRAYOID)
				have_anycompatible_nonarray = true;
			if (actual_type == UNKNOWNOID)
				continue;
			/* 收集非未知 COMPATIBLE 参数的实际类型        */
			anycompatible_actual_types[n_anycompatible_args++] = actual_type;
		}
		else if (decl_type == ANYCOMPATIBLEARRAYOID)
		{
			Oid			elem_type;

			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			elem_type = get_element_type(actual_type);
			if (!OidIsValid(elem_type))
				return false;	/* 不是数组 */
			/* 收集元素类型供公共超类型选取           */
			anycompatible_actual_types[n_anycompatible_args++] = elem_type;
		}
		else if (decl_type == ANYCOMPATIBLERANGEOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(anycompatible_range_typeid))
			{
				/* 所有 ANYCOMPATIBLERANGE 参数须同类型           */
				if (anycompatible_range_typeid != actual_type)
					return false;
			}
			else
			{
				anycompatible_range_typeid = actual_type;
				anycompatible_range_typelem = get_range_subtype(actual_type);
				if (!OidIsValid(anycompatible_range_typelem))
					return false;	/* 非范围类型  */
				/* 收集子类型供公共超类型选取         */
				anycompatible_actual_types[n_anycompatible_args++] = anycompatible_range_typelem;
			}
		}
		else if (decl_type == ANYCOMPATIBLEMULTIRANGEOID)
		{
			if (actual_type == UNKNOWNOID)
				continue;
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(anycompatible_multirange_typeid))
			{
				/* 所有 ANYCOMPATIBLEMULTIRANGE 参数须同类型           */
				if (anycompatible_multirange_typeid != actual_type)
					return false;
			}
			else
			{
				anycompatible_multirange_typeid = actual_type;
				anycompatible_multirange_typelem = get_multirange_range(actual_type);
				if (!OidIsValid(anycompatible_multirange_typelem))
					return false;	/* 不是多范围类型 */
				/* 下面再考虑其子类型      */
			}
		}
	}

	/* 若已有数组类型，则据其获取元素类型          */
	if (OidIsValid(array_typeid))
	{
		if (array_typeid == ANYARRAYOID)
		{
			/*
			 * 把 ANYARRAY 输入匹配到 ANYARRAY 参数的特殊  
			 * 情况：目前允许。enforce_generic_type_consistency()     
			 * 稍后可能报警，视其他多态参数/结果而定 
			 * 但报出的错误不会像"函数不存在"那样费解    
			 *                                                         
			 *
			 * （若你想改动这一点，注意 can_coerce_type 会    
			 * 把这种情况视为匹配，故我们可能走不到这里）
			 *           
			 */
		}
		else
		{
			Oid			array_typelem;

			array_typelem = get_element_type(array_typeid);
			if (!OidIsValid(array_typelem))
				return false;	/* 应是数组，实则不是   */

			if (!OidIsValid(elem_typeid))
			{
				/*
				 * 若尚无元素类型，就使用刚获取的那个      
				 *    
				 */
				elem_typeid = array_typelem;
			}
			else if (array_typelem != elem_typeid)
			{
				/* 否则两者最好匹配     */
				return false;
			}
		}
	}

	/* 从多范围类型推知范围类型，或检查两者一致     */
	if (OidIsValid(multirange_typeid))
	{
		Oid			multirange_typelem;

		multirange_typelem = get_multirange_range(multirange_typeid);
		if (!OidIsValid(multirange_typelem))
			return false;		/* 应是多范围，实则不是    */

		if (!OidIsValid(range_typeid))
		{
			/* 若还没有范围类型，就用刚取到的那个        */
			range_typeid = multirange_typelem;
			range_typelem = get_range_subtype(multirange_typelem);
			if (!OidIsValid(range_typelem))
				return false;	/* 应是范围，实则不是  */
		}
		else if (multirange_typelem != range_typeid)
		{
			/* 否则两者最好匹配     */
			return false;
		}
	}

	/* 若已有范围类型，则据其获取元素类型          */
	if (OidIsValid(range_typeid))
	{
		range_typelem = get_range_subtype(range_typeid);
		if (!OidIsValid(range_typelem))
			return false;		/* 应是范围，实则不是  */

		if (!OidIsValid(elem_typeid))
		{
			/*
			 * 若还没有元素类型，就用刚取到的那个          
			 */
			elem_typeid = range_typelem;
		}
		else if (range_typelem != elem_typeid)
		{
			/* 否则两者最好匹配     */
			return false;
		}
	}

	if (have_anynonarray)
	{
		/* 需要元素类型不是数组或数组上的域                 */
		if (type_is_array_domain(elem_typeid))
			return false;
	}

	if (have_anyenum)
	{
		/* 需要元素类型是枚举            */
		if (!type_is_enum(elem_typeid))
			return false;
	}

	/* 从多范围类型推知范围类型，或检查两者一致     */
	if (OidIsValid(anycompatible_multirange_typeid))
	{
		if (OidIsValid(anycompatible_range_typeid))
		{
			if (anycompatible_multirange_typelem !=
				anycompatible_range_typeid)
				return false;
		}
		else
		{
			anycompatible_range_typeid = anycompatible_multirange_typelem;
			anycompatible_range_typelem = get_range_subtype(anycompatible_range_typeid);
			if (!OidIsValid(anycompatible_range_typelem))
				return false;	/* 非范围类型  */
			/* 收集子类型供公共超类型选取         */
			anycompatible_actual_types[n_anycompatible_args++] =
				anycompatible_range_typelem;
		}
	}

	/* 若有 ANYCOMPATIBLE 族参数则检查其匹配         */
	if (n_anycompatible_args > 0)
	{
		Oid			anycompatible_typeid;

		anycompatible_typeid =
			select_common_type_from_oids(n_anycompatible_args,
										 anycompatible_actual_types,
										 true);

		if (!OidIsValid(anycompatible_typeid))
			return false;		/* 肯定没有公共超类型            */

		/* 必须验证所选类型确实可行                    */
		if (!verify_common_type_from_oids(anycompatible_typeid,
										  n_anycompatible_args,
										  anycompatible_actual_types))
			return false;

		if (have_anycompatible_nonarray)
		{
			/*
			 * 需要 anycompatible 类型不是数组或其上的域     
			 *           
			 */
			if (type_is_array_domain(anycompatible_typeid))
				return false;
		}

		/*
		 * anycompatible 类型必须与范围元素类型完全一致，   
		 * 若我们能确定出该类型的话。这也一并检查了  
		 * anycompatiblemultirange，因为它亦设置    
		 * anycompatible_range_typelem。    
		 */
		if (OidIsValid(anycompatible_range_typelem) &&
			anycompatible_range_typelem != anycompatible_typeid)
			return false;
	}

	/* 看来有效*/
	return true;
}

/*
 * enforce_generic_type_consistency()
 *		确保多态函数可被合法调用，并推知实际   
 *		参数与结果类型。                
 *
 * 若函数参数或返回类型中使用任何多态伪类型，   
 * 我们就确保实际数据类型彼此一致。参数一致性    
 * 规则已在上方 check_generic_type_consistency() 处        
 * 给出。                        
 *
 * 若任何多态参数的输入是 UNKNOWN（即未定型字面量） 
 * 我们会尝试推知它应有的实际类型。若推知成功，
 * 我们就修改 declared_arg_types[] 的相应位置，使        
 * make_fn_arguments 能把字面量强制转换到位。       
 *
 * 若存在 ANYCOMPATIBLE 族的多态参数，我们同样会 
 * 修改 declared_arg_types[] 的相应项来体现解析出的       
 * 公共超类型，使 make_fn_arguments 能把实际参数        
 * 转换为正确类型。     
 *
 * 规则也会作用于函数的返回类型（可能修改它），    
 * 前提是它声明为多态类型，且至少有一个多态     
 * 参数类型：           
 *
 * 1) 若返回类型是 ANYELEMENT 且任一参数是 ANYELEMENT，       
 *	  则以该参数实际类型为函数返回类型。  
 * 2) 若返回类型是 ANYARRAY 且任一参数是 ANYARRAY，       
 *	  则以该参数实际类型为函数返回类型。  
 * 3) 类似地，若返回类型是 ANYRANGE 或 ANYMULTIRANGE，   
 *	  且任一参数是 ANYRANGE 或 ANYMULTIRANGE，则以该参数       
 *	  实际类型（或相应范围/多范围类型）作为返回类型。  
 *	       
 * 4) 否则，若返回类型是 ANYELEMENT 或 ANYARRAY，且有     
 *	  至少一个 ANYELEMENT、ANYARRAY、ANYRANGE 或                   
 *	  ANYMULTIRANGE 输入，就从这些输入推知返回类型，       
 * 做不到就报错。5) 否则，若返回类型为 ANYRANGE           
 *	  或 ANYMULTIRANGE，我们无从选定具体的范围                 
 *	  类型。）                       
 * 6) ANYENUM 的处理与 ANYELEMENT 相同，但若使用它           
 *	  （单独或与普通 ANYELEMENT 组合），我们会附加     
 *	  条件：ANYELEMENT 类型必须是枚举。       
 * 7) ANYNONARRAY 的处理与 ANYELEMENT 相同，但若使用它，         
 *	  我们会附加条件：ANYELEMENT 类型不能是数组。              
 *	  （与 ANYARRAY 或 ANYENUM 组合使用时这是空操作，        
 *	  否则是额外限制。）     
 * 8) ANYCOMPATIBLE、ANYCOMPATIBLEARRAY 与                                  
 *	  ANYCOMPATIBLENONARRAY 的处理方式是：解析这些参数         
 *	  （数组输入则取其元素类型）的公共超类型，再把    
 *	  所有这些参数强制转换过去；ANYCOMPATIBLEARRAY 则用 
 *	  超类型数组。 
 * 9) 对 ANYCOMPATIBLERANGE 与 ANYCOMPATIBLEMULTIRANGE，               
 *	  必须至少有一个非 UNKNOWN 输入与之匹配，且所有   
 *	  这类输入必须是同一范围类型（或按需为其     
 *	  多范围），因为无法从非范围类型推知范围类型。     
 *	  此外，选取 ANYCOMPATIBLE 等的公共超类型时会把        
 *	  范围类型的子类型纳入，且它必须与该公共超类型 
 *	  完全一致。       
 *
 * 数组上的域或范围上的域分别匹配 ANYARRAY 或         
 * ANYRANGE 参数，并展平为基本类型。（特别地，若     
 * 返回类型也是 ANYARRAY 或 ANYRANGE，我们会把它设         
 * 为基本类型而非域类型。）同理，ANYMULTIRANGE、  
 * ANYCOMPATIBLEARRAY、ANYCOMPATIBLERANGE 与               
 * ANYCOMPATIBLEMULTIRANGE 
 *
 * allow_poly 为假时，我们不预期 actual_arg_types 中               
 * 出现多态类型，也不应返回多态结果类型。            
 * allow_poly 为真时，允许多态的"实际"参数类型，            
 * 也可以返回匹配的多态类型作为结果。                    
 * （此情形目前只用于检查聚集的声明与其基础转换        
 * 函数 transfn 是否兼容。）        
 *
 * 特殊情形：即使 allow_poly 为假，actual_arg_types             
 * 中也可能出现 ANYARRAY（这仅因 pg_statistic 的某些          
 * 列在目录中显示为 anyarray）。我们允许它匹配         
 * 声明的 ANYARRAY 参数，但前提是没有其他需要与        
 * 之匹配的多态参数，也无需再确定元素类型来          
 * 推断结果类型。注意这意味着接受 ANYARRAY 的函数          
 * 在作用于 pg_statistic 列时最好行为正常；它们            
 * 无法径直假定相继的输入具有相同的实际元素          
 * 类型。ANYCOMPATIBLEARRAY 没有类似逻辑；                      
 * 因为不存在该类型的目录列，所以也无需这类逻辑，     
 * 不会看到它作为输入。我们可以考虑把实际 ANYARRAY         
 * 输入匹配到 ANYCOMPATIBLEARRAY 参数，但目前看来               
 * 并无用处，因为使用 ANYCOMPATIBLEARRAY 本身没有               
 * 价值，除非另有 ANYCOMPATIBLE 族参数或结果。  
 *
 * 另外，若没有任何参数声明为多态类型，即使返回  
 * 类型本身是多态的，我们也会原样返回 rettype。            
 * 用户声明的函数绝不会出现这种情况，因为 CREATE         
 * FUNCTION 会阻止。但 array_in() 等内建函数确实会这样。   
 */
Oid
enforce_generic_type_consistency(const Oid *actual_arg_types,
								 Oid *declared_arg_types,
								 int nargs,
								 Oid rettype,
								 bool allow_poly)
{
	bool		have_poly_anycompatible = false;
	bool		have_poly_unknowns = false;
	Oid			elem_typeid = InvalidOid;
	Oid			array_typeid = InvalidOid;
	Oid			range_typeid = InvalidOid;
	Oid			multirange_typeid = InvalidOid;
	Oid			anycompatible_typeid = InvalidOid;
	Oid			anycompatible_array_typeid = InvalidOid;
	Oid			anycompatible_range_typeid = InvalidOid;
	Oid			anycompatible_range_typelem = InvalidOid;
	Oid			anycompatible_multirange_typeid = InvalidOid;
	Oid			anycompatible_multirange_typelem = InvalidOid;
	bool		have_anynonarray = (rettype == ANYNONARRAYOID);
	bool		have_anyenum = (rettype == ANYENUMOID);
	bool		have_anymultirange = (rettype == ANYMULTIRANGEOID);
	bool		have_anycompatible_nonarray = (rettype == ANYCOMPATIBLENONARRAYOID);
	bool		have_anycompatible_array = (rettype == ANYCOMPATIBLEARRAYOID);
	bool		have_anycompatible_range = (rettype == ANYCOMPATIBLERANGEOID);
	bool		have_anycompatible_multirange = (rettype == ANYCOMPATIBLEMULTIRANGEOID);
	int			n_poly_args = 0;	/* 统计全部第 1 族参数        */
	int			n_anycompatible_args = 0;	/* 只统计非未知项         */
	Oid			anycompatible_actual_types[FUNC_MAX_ARGS];

	/*
	 * 遍历各参数，看是否存在多态参数。                      
	 * 若有，则需要实际类型保持一致。    
	 */
	Assert(nargs <= FUNC_MAX_ARGS);
	for (int j = 0; j < nargs; j++)
	{
		Oid			decl_type = declared_arg_types[j];
		Oid			actual_type = actual_arg_types[j];

		if (decl_type == ANYELEMENTOID ||
			decl_type == ANYNONARRAYOID ||
			decl_type == ANYENUMOID)
		{
			n_poly_args++;
			if (decl_type == ANYNONARRAYOID)
				have_anynonarray = true;
			else if (decl_type == ANYENUMOID)
				have_anyenum = true;
			if (actual_type == UNKNOWNOID)
			{
				have_poly_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			if (OidIsValid(elem_typeid) && actual_type != elem_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("arguments declared \"%s\" are not all alike", "anyelement"),
						 errdetail("%s versus %s",
								   format_type_be(elem_typeid),
								   format_type_be(actual_type))));
			elem_typeid = actual_type;
		}
		else if (decl_type == ANYARRAYOID)
		{
			n_poly_args++;
			if (actual_type == UNKNOWNOID)
			{
				have_poly_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(array_typeid) && actual_type != array_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("arguments declared \"%s\" are not all alike", "anyarray"),
						 errdetail("%s versus %s",
								   format_type_be(array_typeid),
								   format_type_be(actual_type))));
			array_typeid = actual_type;
		}
		else if (decl_type == ANYRANGEOID)
		{
			n_poly_args++;
			if (actual_type == UNKNOWNOID)
			{
				have_poly_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(range_typeid) && actual_type != range_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("arguments declared \"%s\" are not all alike", "anyrange"),
						 errdetail("%s versus %s",
								   format_type_be(range_typeid),
								   format_type_be(actual_type))));
			range_typeid = actual_type;
		}
		else if (decl_type == ANYMULTIRANGEOID)
		{
			n_poly_args++;
			have_anymultirange = true;
			if (actual_type == UNKNOWNOID)
			{
				have_poly_unknowns = true;
				continue;
			}
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(multirange_typeid) && actual_type != multirange_typeid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("arguments declared \"%s\" are not all alike", "anymultirange"),
						 errdetail("%s versus %s",
								   format_type_be(multirange_typeid),
								   format_type_be(actual_type))));
			multirange_typeid = actual_type;
		}
		else if (decl_type == ANYCOMPATIBLEOID ||
				 decl_type == ANYCOMPATIBLENONARRAYOID)
		{
			have_poly_anycompatible = true;
			if (decl_type == ANYCOMPATIBLENONARRAYOID)
				have_anycompatible_nonarray = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			/* 收集非未知 COMPATIBLE 参数的实际类型        */
			anycompatible_actual_types[n_anycompatible_args++] = actual_type;
		}
		else if (decl_type == ANYCOMPATIBLEARRAYOID)
		{
			Oid			anycompatible_elem_type;

			have_poly_anycompatible = true;
			have_anycompatible_array = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			anycompatible_elem_type = get_element_type(actual_type);
			if (!OidIsValid(anycompatible_elem_type))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not an array but type %s",
								"anycompatiblearray",
								format_type_be(actual_type))));
			/* 收集元素类型供公共超类型选取           */
			anycompatible_actual_types[n_anycompatible_args++] = anycompatible_elem_type;
		}
		else if (decl_type == ANYCOMPATIBLERANGEOID)
		{
			have_poly_anycompatible = true;
			have_anycompatible_range = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(anycompatible_range_typeid))
			{
				/* 所有 ANYCOMPATIBLERANGE 参数须同类型           */
				if (anycompatible_range_typeid != actual_type)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("arguments declared \"%s\" are not all alike", "anycompatiblerange"),
							 errdetail("%s versus %s",
									   format_type_be(anycompatible_range_typeid),
									   format_type_be(actual_type))));
			}
			else
			{
				anycompatible_range_typeid = actual_type;
				anycompatible_range_typelem = get_range_subtype(actual_type);
				if (!OidIsValid(anycompatible_range_typelem))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("argument declared %s is not a range type but type %s",
									"anycompatiblerange",
									format_type_be(actual_type))));
				/* 收集子类型供公共超类型选取         */
				anycompatible_actual_types[n_anycompatible_args++] = anycompatible_range_typelem;
			}
		}
		else if (decl_type == ANYCOMPATIBLEMULTIRANGEOID)
		{
			have_poly_anycompatible = true;
			have_anycompatible_multirange = true;
			if (actual_type == UNKNOWNOID)
				continue;
			if (allow_poly && decl_type == actual_type)
				continue;		/* 此处无新信息      */
			actual_type = getBaseType(actual_type); /* 展平域       */
			if (OidIsValid(anycompatible_multirange_typeid))
			{
				/* 所有 ANYCOMPATIBLEMULTIRANGE 参数须同类型           */
				if (anycompatible_multirange_typeid != actual_type)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("arguments declared \"%s\" are not all alike", "anycompatiblemultirange"),
							 errdetail("%s versus %s",
									   format_type_be(anycompatible_multirange_typeid),
									   format_type_be(actual_type))));
			}
			else
			{
				anycompatible_multirange_typeid = actual_type;
				anycompatible_multirange_typelem = get_multirange_range(actual_type);
				if (!OidIsValid(anycompatible_multirange_typelem))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("argument declared %s is not a multirange type but type %s",
									"anycompatiblemultirange",
									format_type_be(actual_type))));
				/* 下面再考虑其子类型      */
			}
		}
	}

	/*
	 * 快速通道：若无参数是多态类型，则原样返回    
	 * rettype。若它为多态类型，解析它不归我们管。      
	 */
	if (n_poly_args == 0 && !have_poly_anycompatible)
		return rettype;

	/* 检查（若有）family-1 多态参数的匹配情况   */
	if (n_poly_args)
	{
		/* 若已有数组类型，则据其获取元素类型          */
		if (OidIsValid(array_typeid))
		{
			Oid			array_typelem;

			if (array_typeid == ANYARRAYOID)
			{
				/*
				 * 特例：仅当无其他 family-1 多态参数，允许
				 * 把 ANYARRAY 输入匹配到 ANYARRAY 参数（否则 
				 * 无法确定数组元素类型是否匹配得上）    
				 * 且结果类型不需要我们                            
				 * 推断具体元素类型。                 
				 */
				if (n_poly_args != 1 ||
					(rettype != ANYARRAYOID &&
					 IsPolymorphicTypeFamily1(rettype)))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("cannot determine element type of \"anyarray\" argument")));
				array_typelem = ANYELEMENTOID;
			}
			else
			{
				array_typelem = get_element_type(array_typeid);
				if (!OidIsValid(array_typelem))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("argument declared %s is not an array but type %s",
									"anyarray", format_type_be(array_typeid))));
			}

			if (!OidIsValid(elem_typeid))
			{
				/*
				 * 若尚无元素类型，就使用刚获取的那个      
				 *    
				 */
				elem_typeid = array_typelem;
			}
			else if (array_typelem != elem_typeid)
			{
				/* 否则两者必须匹配     */
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not consistent with argument declared %s",
								"anyarray", "anyelement"),
						 errdetail("%s versus %s",
								   format_type_be(array_typeid),
								   format_type_be(elem_typeid))));
			}
		}

		/* 从多范围类型推得范围类型，或反之      */
		if (OidIsValid(multirange_typeid))
		{
			Oid			multirange_typelem;

			multirange_typelem = get_multirange_range(multirange_typeid);
			if (!OidIsValid(multirange_typelem))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not a multirange type but type %s",
								"anymultirange",
								format_type_be(multirange_typeid))));

			if (!OidIsValid(range_typeid))
			{
				/* 若还没有范围类型，就用刚取到的那个        */
				range_typeid = multirange_typelem;
			}
			else if (multirange_typelem != range_typeid)
			{
				/* 否则两者必须匹配     */
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not consistent with argument declared %s",
								"anymultirange", "anyrange"),
						 errdetail("%s versus %s",
								   format_type_be(multirange_typeid),
								   format_type_be(range_typeid))));
			}
		}
		else if (have_anymultirange && OidIsValid(range_typeid))
		{
			multirange_typeid = get_range_multirange(range_typeid);
			/* 若那样没成功，后面会报错     */
		}

		/* 若已有范围类型，则据其获取元素类型          */
		if (OidIsValid(range_typeid))
		{
			Oid			range_typelem;

			range_typelem = get_range_subtype(range_typeid);
			if (!OidIsValid(range_typelem))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not a range type but type %s",
								"anyrange",
								format_type_be(range_typeid))));

			if (!OidIsValid(elem_typeid))
			{
				/*
				 * 若尚无元素类型，就使用刚获取的那个      
				 *    
				 */
				elem_typeid = range_typelem;
			}
			else if (range_typelem != elem_typeid)
			{
				/* 否则两者必须匹配     */
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("argument declared %s is not consistent with argument declared %s",
								"anyrange", "anyelement"),
						 errdetail("%s versus %s",
								   format_type_be(range_typeid),
								   format_type_be(elem_typeid))));
			}
		}

		if (!OidIsValid(elem_typeid))
		{
			if (allow_poly)
			{
				elem_typeid = ANYELEMENTOID;
				array_typeid = ANYARRAYOID;
				range_typeid = ANYRANGEOID;
				multirange_typeid = ANYMULTIRANGEOID;
			}
			else
			{
				/*
				 * 到达此处的唯一途径是所有 family-1 多态参
				 * 数都带有 UNKNOWN 输入。
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("could not determine polymorphic type because input has type %s",
								"unknown")));
			}
		}

		if (have_anynonarray && elem_typeid != ANYELEMENTOID)
		{
			/*
			 * 需要元素类型不能是数组或数组上的域       
			 *      
			 */
			if (type_is_array_domain(elem_typeid))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type matched to anynonarray is an array type: %s",
								format_type_be(elem_typeid))));
		}

		if (have_anyenum && elem_typeid != ANYELEMENTOID)
		{
			/* 需要元素类型是枚举            */
			if (!type_is_enum(elem_typeid))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type matched to anyenum is not an enum type: %s",
								format_type_be(elem_typeid))));
		}
	}

	/* 检查（若有）family-2 多态参数的匹配情况   */
	if (have_poly_anycompatible)
	{
		/* 从多范围类型推得范围类型，或反之      */
		if (OidIsValid(anycompatible_multirange_typeid))
		{
			if (OidIsValid(anycompatible_range_typeid))
			{
				if (anycompatible_multirange_typelem !=
					anycompatible_range_typeid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("argument declared %s is not consistent with argument declared %s",
									"anycompatiblemultirange",
									"anycompatiblerange"),
							 errdetail("%s versus %s",
									   format_type_be(anycompatible_multirange_typeid),
									   format_type_be(anycompatible_range_typeid))));
			}
			else
			{
				anycompatible_range_typeid = anycompatible_multirange_typelem;
				anycompatible_range_typelem = get_range_subtype(anycompatible_range_typeid);
				if (!OidIsValid(anycompatible_range_typelem))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("argument declared %s is not a multirange type but type %s",
									"anycompatiblemultirange",
									format_type_be(anycompatible_multirange_typeid))));
				/* 这会启用后面的元素类型匹配检查  */
				have_anycompatible_range = true;
				/* 收集子类型供公共超类型选取         */
				anycompatible_actual_types[n_anycompatible_args++] =
					anycompatible_range_typelem;
			}
		}
		else if (have_anycompatible_multirange &&
				 OidIsValid(anycompatible_range_typeid))
		{
			anycompatible_multirange_typeid = get_range_multirange(anycompatible_range_typeid);
			/* 若那样没成功，后面会报错     */
		}

		if (n_anycompatible_args > 0)
		{
			anycompatible_typeid =
				select_common_type_from_oids(n_anycompatible_args,
											 anycompatible_actual_types,
											 false);

			/* 必须验证所选类型确实可行                    */
			if (!verify_common_type_from_oids(anycompatible_typeid,
											  n_anycompatible_args,
											  anycompatible_actual_types))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("arguments of anycompatible family cannot be cast to a common type")));

			if (have_anycompatible_array)
			{
				anycompatible_array_typeid = get_array_type(anycompatible_typeid);
				if (!OidIsValid(anycompatible_array_typeid))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("could not find array type for data type %s",
									format_type_be(anycompatible_typeid))));
			}

			if (have_anycompatible_range)
			{
				/* 无法从其他类型推断范围类型     */
				if (!OidIsValid(anycompatible_range_typeid))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anycompatiblerange", "unknown")));

				/*
				 * anycompatible 类型必须与范围元素类型完全一致
				 *     
				 */
				if (anycompatible_range_typelem != anycompatible_typeid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("anycompatiblerange type %s does not match anycompatible type %s",
									format_type_be(anycompatible_range_typeid),
									format_type_be(anycompatible_typeid))));
			}

			if (have_anycompatible_multirange)
			{
				/* 无法从其他类型推断多范围类型       */
				if (!OidIsValid(anycompatible_multirange_typeid))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anycompatiblemultirange", "unknown")));

				/*
				 * anycompatible 类型必须与多范围元素类型      
				 * 完全一致
				 */
				if (anycompatible_range_typelem != anycompatible_typeid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("anycompatiblemultirange type %s does not match anycompatible type %s",
									format_type_be(anycompatible_multirange_typeid),
									format_type_be(anycompatible_typeid))));
			}

			if (have_anycompatible_nonarray)
			{
				/*
				 * 需要元素类型不能是数组或数组上的域       
				 *      
				 */
				if (type_is_array_domain(anycompatible_typeid))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("type matched to anycompatiblenonarray is an array type: %s",
									format_type_be(anycompatible_typeid))));
			}
		}
		else
		{
			if (allow_poly)
			{
				anycompatible_typeid = ANYCOMPATIBLEOID;
				anycompatible_array_typeid = ANYCOMPATIBLEARRAYOID;
				anycompatible_range_typeid = ANYCOMPATIBLERANGEOID;
				anycompatible_multirange_typeid = ANYCOMPATIBLEMULTIRANGEOID;
			}
			else
			{
				/*
				 * 到达此处的唯一途径是所有 family-2 多态参
				 * 数都为 UNKNOWN 输入，处理方式与        
				 * select_common_type() 一致。但这并不允许我们使 
				 * 用 TEXTRANGE 或 TEXTMULTIRANGE。     
				 */
				anycompatible_typeid = TEXTOID;
				anycompatible_array_typeid = TEXTARRAYOID;
				if (have_anycompatible_range)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anycompatiblerange", "unknown")));
				if (have_anycompatible_multirange)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anycompatiblemultirange", "unknown")));
			}
		}

		/* 用选定的类型替换 family-2 多态类型       */
		for (int j = 0; j < nargs; j++)
		{
			Oid			decl_type = declared_arg_types[j];

			if (decl_type == ANYCOMPATIBLEOID ||
				decl_type == ANYCOMPATIBLENONARRAYOID)
				declared_arg_types[j] = anycompatible_typeid;
			else if (decl_type == ANYCOMPATIBLEARRAYOID)
				declared_arg_types[j] = anycompatible_array_typeid;
			else if (decl_type == ANYCOMPATIBLERANGEOID)
				declared_arg_types[j] = anycompatible_range_typeid;
			else if (decl_type == ANYCOMPATIBLEMULTIRANGEOID)
				declared_arg_types[j] = anycompatible_multirange_typeid;
		}
	}

	/*
	 * 若 family-1 多态参数曾带有 UNKNOWN 输入，则需重   
	 * 新扫描，为其指派正确类型。 
	 *
	 * 注意：无需考虑那些匹配到 family-2 多态参数的      
	 * unknown 输入，因为我们刚强制更新了它们的         
	 * declared_arg_types[] 位置。            
	 */
	if (have_poly_unknowns)
	{
		for (int j = 0; j < nargs; j++)
		{
			Oid			decl_type = declared_arg_types[j];
			Oid			actual_type = actual_arg_types[j];

			if (actual_type != UNKNOWNOID)
				continue;

			if (decl_type == ANYELEMENTOID ||
				decl_type == ANYNONARRAYOID ||
				decl_type == ANYENUMOID)
				declared_arg_types[j] = elem_typeid;
			else if (decl_type == ANYARRAYOID)
			{
				if (!OidIsValid(array_typeid))
				{
					array_typeid = get_array_type(elem_typeid);
					if (!OidIsValid(array_typeid))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("could not find array type for data type %s",
										format_type_be(elem_typeid))));
				}
				declared_arg_types[j] = array_typeid;
			}
			else if (decl_type == ANYRANGEOID)
			{
				if (!OidIsValid(range_typeid))
				{
					/* 无法从其他类型推断范围类型     */
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anyrange", "unknown")));
				}
				declared_arg_types[j] = range_typeid;
			}
			else if (decl_type == ANYMULTIRANGEOID)
			{
				if (!OidIsValid(multirange_typeid))
				{
					/* 无法从其他类型推断多范围类型       */
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine polymorphic type %s because input has type %s",
									"anymultirange", "unknown")));
				}
				declared_arg_types[j] = multirange_typeid;
			}
		}
	}

	/* 若返回 ANYELEMENT，则使用适当的参数类型     */
	if (rettype == ANYELEMENTOID ||
		rettype == ANYNONARRAYOID ||
		rettype == ANYENUMOID)
		return elem_typeid;

	/* 若返回 ANYARRAY，则使用适当的参数类型     */
	if (rettype == ANYARRAYOID)
	{
		if (!OidIsValid(array_typeid))
		{
			array_typeid = get_array_type(elem_typeid);
			if (!OidIsValid(array_typeid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(elem_typeid))));
		}
		return array_typeid;
	}

	/* 若返回 ANYRANGE，则使用适当的参数类型     */
	if (rettype == ANYRANGEOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(range_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not determine polymorphic type %s because input has type %s",
									 "anyrange", "unknown")));
		return range_typeid;
	}

	/* 若返回 ANYMULTIRANGE，则使用适当的参数类型     */
	if (rettype == ANYMULTIRANGEOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not determine polymorphic type %s because input has type %s",
									 "anymultirange", "unknown")));
		return multirange_typeid;
	}

	/* 若返回 ANYCOMPATIBLE，则使用适当的类型  */
	if (rettype == ANYCOMPATIBLEOID ||
		rettype == ANYCOMPATIBLENONARRAYOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(anycompatible_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not identify anycompatible type")));
		return anycompatible_typeid;
	}

	/* 若返回 ANYCOMPATIBLEARRAY，则使用适当的类型  */
	if (rettype == ANYCOMPATIBLEARRAYOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(anycompatible_array_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not identify anycompatiblearray type")));
		return anycompatible_array_typeid;
	}

	/* 若返回 ANYCOMPATIBLERANGE，则使用适当的参数类型     */
	if (rettype == ANYCOMPATIBLERANGEOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(anycompatible_range_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not identify anycompatiblerange type")));
		return anycompatible_range_typeid;
	}

	/* 若返回 ANYCOMPATIBLEMULTIRANGE，则使用适当的参数类型     */
	if (rettype == ANYCOMPATIBLEMULTIRANGEOID)
	{
		/* 若函数签名有效，此错误在逻辑上不可达：     */
		if (!OidIsValid(anycompatible_multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("could not identify anycompatiblemultirange type")));
		return anycompatible_multirange_typeid;
	}

	/* 我们不会返回通用类型；将原始的返回类型送回    */
	return rettype;
}

/*
 * check_valid_polymorphic_signature()
 *		提议的函数签名按多态规则是否有效？           
 *
 * 若签名有效则返回 NULL（即 ret_type 并非多态类型，         
 * 或者它能从给定的被声明参数类型推导得出）。否则， 
 * 返回 palloc 的、已翻译的 errdetail 字符串说明原因。    
 */
char *
check_valid_polymorphic_signature(Oid ret_type,
								  const Oid *declared_arg_types,
								  int nargs)
{
	if (ret_type == ANYRANGEOID || ret_type == ANYMULTIRANGEOID)
	{
		/*
		 * ANYRANGE 和 ANYMULTIRANGE 需要有 ANYRANGE 或              
		 * ANYMULTIRANGE 输入，否则无法分辨元素类型相同的 
		 * 范围该用哪个。    
		 */
		for (int i = 0; i < nargs; i++)
		{
			if (declared_arg_types[i] == ANYRANGEOID ||
				declared_arg_types[i] == ANYMULTIRANGEOID)
				return NULL;	/* 可*/
		}
		return psprintf(_("A result of type %s requires at least one input of type anyrange or anymultirange."),
						format_type_be(ret_type));
	}
	else if (ret_type == ANYCOMPATIBLERANGEOID || ret_type == ANYCOMPATIBLEMULTIRANGEOID)
	{
		/*
		 * ANYCOMPATIBLERANGE 和 ANYCOMPATIBLEMULTIRANGE 需要    
		 * 有 ANYCOMPATIBLERANGE 或 ANYCOMPATIBLEMULTIRANGE 输入，      
		 * 否则无法分辨元素类型相同的范围该用哪个。   
		 *     
		 */
		for (int i = 0; i < nargs; i++)
		{
			if (declared_arg_types[i] == ANYCOMPATIBLERANGEOID ||
				declared_arg_types[i] == ANYCOMPATIBLEMULTIRANGEOID)
				return NULL;	/* 可*/
		}
		return psprintf(_("A result of type %s requires at least one input of type anycompatiblerange or anycompatiblemultirange."),
						format_type_be(ret_type));
	}
	else if (IsPolymorphicTypeFamily1(ret_type))
	{
		/* 否则，任一 family-1 类型都可由其他推出       */
		for (int i = 0; i < nargs; i++)
		{
			if (IsPolymorphicTypeFamily1(declared_arg_types[i]))
				return NULL;	/* 可*/
		}
		/* 此列表须与 IsPolymorphicTypeFamily1 同步！    */
		return psprintf(_("A result of type %s requires at least one input of type anyelement, anyarray, anynonarray, anyenum, anyrange, or anymultirange."),
						format_type_be(ret_type));
	}
	else if (IsPolymorphicTypeFamily2(ret_type))
	{
		/* 否则，任一 family-2 类型都可由其他推出       */
		for (int i = 0; i < nargs; i++)
		{
			if (IsPolymorphicTypeFamily2(declared_arg_types[i]))
				return NULL;	/* 可*/
		}
		/* 此列表须与 IsPolymorphicTypeFamily2 同步！    */
		return psprintf(_("A result of type %s requires at least one input of type anycompatible, anycompatiblearray, anycompatiblenonarray, anycompatiblerange, or anycompatiblemultirange."),
						format_type_be(ret_type));
	}
	else
		return NULL;			/* 可：ret_type 非多态类型  */
}

/*
 * check_valid_internal_signature()
 *		提议的函数签名按 INTERNAL 安全规则是否有效？    
 *
 * 若没问题则返回 NULL；若 ret_type 为 INTERNAL，但被声         
 * 明的参数类型均不是，则返回合适的错误消息。            
 * （创建这样的函数不安全，因为它会让人直接从 SQL     
 * 调用使用 INTERNAL 参数的函数。）返回错误详情有点      
 * 小题大做，因为只有一种可能；但为了让此 API 与         
 * check_valid_polymorphic_signature() 
 */
char *
check_valid_internal_signature(Oid ret_type,
							   const Oid *declared_arg_types,
							   int nargs)
{
	if (ret_type == INTERNALOID)
	{
		for (int i = 0; i < nargs; i++)
		{
			if (declared_arg_types[i] == ret_type)
				return NULL;	/* 可*/
		}
		return pstrdup(_("A result of type internal requires at least one input of type internal."));
	}
	else
		return NULL;			/* 可：ret_type 非 INTERNAL  */
}


/* TypeCategory()
 *		为指定的类型 OID 分配一个类别。
 *
 * 注意：不得返回 TYPCATEGORY_INVALID。 
 */
TYPCATEGORY
TypeCategory(Oid type)
{
	char		typcategory;
	bool		typispreferred;

	get_type_category_preferred(type, &typcategory, &typispreferred);
	Assert(typcategory != TYPCATEGORY_INVALID);
	return (TYPCATEGORY) typcategory;
}


/* IsPreferredType()
 *		检查此类型对给定类别而言是否为首选类型。  
 *
 * 若 category 是 TYPCATEGORY_INVALID，则对任意类别的首选      
 * 类型返回 true；否则仅对该类别的首选类型。       
 *          
 */
bool
IsPreferredType(TYPCATEGORY category, Oid type)
{
	char		typcategory;
	bool		typispreferred;

	get_type_category_preferred(type, &typcategory, &typispreferred);
	if (category == typcategory || category == TYPCATEGORY_INVALID)
		return typispreferred;
	else
		return false;
}


/* IsBinaryCoercible()
 *		检查 srctype 能否二进制转换到 targettype 
 *
 * 这一概念允许我们取巧，直接交换值而无需费事调 
 * 用转换函数。注意，一般而言这只应作为实现上的    
 * 捷径。在 7.4 之前，它还被用作解析重载函数与操作    
 * 符的启发式方法，但那基本是个坏主意。                  
 *                                            
 *
 * 从 7.3 起，二进制可转换性不再硬编码于代码中。    
 * 若存在可隐式调用、无需函数的 pg_cast 条目，    
 * 我们就认为两个类型是二进制可转换的。另外，域    
 * 对其基类型总是二进制可转换，但反之则*不然*（       
 * 方向上，必须先应用域约束检查才能把值当作合        
 * 法的值接受）。另需特判各种多态类                       
 * 型。
 *
 * 此函数取代了 IsBinaryCompatible()，后者本质上是一     
 * 个对称测试。由于 pg_cast 条目未必是对称的，操作数   
 * 的顺序如今便有了意义。            
 */
bool
IsBinaryCoercible(Oid srctype, Oid targettype)
{
	Oid			castoid;

	return IsBinaryCoercibleWithCast(srctype, targettype, &castoid);
}

/* IsBinaryCoercibleWithCast()
 *		检查 srctype 能否二进制转换到 targettype 
 *
 * 此变体还会在涉及 pg_cast 条目时返回其 OID。                
 * *若不存在二进制可转换的 cast，或存在的是硬编        
 * 码规则而非 pg_cast 条目，则 castoid 为 InvalidOid。
 */
bool
IsBinaryCoercibleWithCast(Oid srctype, Oid targettype,
						  Oid *castoid)
{
	HeapTuple	tuple;
	Form_pg_cast castForm;
	bool		result;

	*castoid = InvalidOid;

	/* 相同类型走捷径  */
	if (srctype == targettype)
		return true;

	/* 任何类型都可转换为 ANY/ANYELEMENT/ANYCOMPATIBLE    */
	if (targettype == ANYOID || targettype == ANYELEMENTOID ||
		targettype == ANYCOMPATIBLEOID)
		return true;

	/* 若 srctype 是域，则化归为其基类型   */
	if (OidIsValid(srctype))
		srctype = getBaseType(srctype);

	/* 域到基类型情形的次快速路径         */
	if (srctype == targettype)
		return true;

	/* 也接受任何数组类型可转换为 ANY[COMPATIBLE]ARRAY    */
	if (targettype == ANYARRAYOID || targettype == ANYCOMPATIBLEARRAYOID)
		if (type_is_array(srctype))
			return true;

	/* 也接受任何非数组类型可转换为 ANY[COMPATIBLE]NONARRAY     */
	if (targettype == ANYNONARRAYOID || targettype == ANYCOMPATIBLENONARRAYOID)
		if (!type_is_array(srctype))
			return true;

	/* 也接受任何枚举类型可转换为 ANYENUM   */
	if (targettype == ANYENUMOID)
		if (type_is_enum(srctype))
			return true;

	/* 也接受任何范围类型可转换为 ANY[COMPATIBLE]RANGE    */
	if (targettype == ANYRANGEOID || targettype == ANYCOMPATIBLERANGEOID)
		if (type_is_range(srctype))
			return true;

	/* 任何多范围类型也可转换为 ANY[COMPATIBLE]MULTIRANGE      */
	if (targettype == ANYMULTIRANGEOID || targettype == ANYCOMPATIBLEMULTIRANGEOID)
		if (type_is_multirange(srctype))
			return true;

	/* 也接受任何复合类型可转换为 RECORD        */
	if (targettype == RECORDOID)
		if (ISCOMPLEX(srctype))
			return true;

	/* 也接受任何复合类型数组可转换为 RECORD[]        */
	if (targettype == RECORDARRAYOID)
		if (is_complex_array(srctype))
			return true;

	/* 否则查 pg_cast    */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(srctype),
							ObjectIdGetDatum(targettype));
	if (!HeapTupleIsValid(tuple))
		return false;			/* 无 cast*/
	castForm = (Form_pg_cast) GETSTRUCT(tuple);

	result = (castForm->castmethod == COERCION_METHOD_BINARY &&
			  castForm->castcontext == COERCION_CODE_IMPLICIT);

	if (result)
		*castoid = castForm->oid;

	ReleaseSysCache(tuple);

	return result;
}


/*
 * find_coercion_pathway
 *		寻找两类型间的强制转换路径。    
 *
 * 目前它只处理标量类型的情形；不考虑多态类型，     
 * 也不考虑复合类型间的 cast。（也许有朝一日会     
 * 纳入？）      
 *
 * ccontext 决定可用的 cast 集合。        
 *
 * 可能的结果码有：      
 *	COERCION_PATH_NONE：找不到任何强制转换路径 
 *				*funcid 被置为 InvalidOid
 *	COERCION_PATH_FUNC：应用 *funcid 返回的强制转换函数    
 *	COERCION_PATH_RELABELTYPE：二进制兼容 cast，无需函数      
 *				*funcid 被置为 InvalidOid
 *	COERCION_PATH_ARRAYCOERCE：需 ArrayCoerceExpr 节点 
 *				*funcid 被置为 InvalidOid
 *	COERCION_PATH_COERCEVIAIO：需 CoerceViaIO 节点
 *				*funcid 被置为 InvalidOid
 *
 * 注意：COERCION_PATH_RELABELTYPE 不一定意味着转换无需        
 * 做任何工作；若目标是域，则可能需要应用域约束检   
 * 查。若想检查是否零开销转换，请用 IsBinaryCoercible()。  
 *                                         
 */
CoercionPathType
find_coercion_pathway(Oid targetTypeId, Oid sourceTypeId,
					  CoercionContext ccontext,
					  Oid *funcid)
{
	CoercionPathType result = COERCION_PATH_NONE;
	HeapTuple	tuple;

	*funcid = InvalidOid;

	/* 或许两者都是域；若是，则考察它们的基类型   */
	if (OidIsValid(sourceTypeId))
		sourceTypeId = getBaseType(sourceTypeId);
	if (OidIsValid(targetTypeId))
		targetTypeId = getBaseType(targetTypeId);

	/* 域与其基类型之间总是可互相转换的         */
	if (sourceTypeId == targetTypeId)
		return COERCION_PATH_RELABELTYPE;

	/* 查 pg_cast     */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(sourceTypeId),
							ObjectIdGetDatum(targetTypeId));

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tuple);
		CoercionContext castcontext;

		/* castcontext 的 char 值转换为 CoercionContext 枚举   */
		switch (castForm->castcontext)
		{
			case COERCION_CODE_IMPLICIT:
				castcontext = COERCION_IMPLICIT;
				break;
			case COERCION_CODE_ASSIGNMENT:
				castcontext = COERCION_ASSIGNMENT;
				break;
			case COERCION_CODE_EXPLICIT:
				castcontext = COERCION_EXPLICIT;
				break;
			default:
				elog(ERROR, "unrecognized castcontext: %d",
					 (int) castForm->castcontext);
				castcontext = 0;	/* 让编译器安静  */
				break;
		}

		/* 此处依赖枚举的次序来获得正确行为   */
		if (ccontext >= castcontext)
		{
			switch (castForm->castmethod)
			{
				case COERCION_METHOD_FUNCTION:
					result = COERCION_PATH_FUNC;
					*funcid = castForm->castfunc;
					break;
				case COERCION_METHOD_INOUT:
					result = COERCION_PATH_COERCEVIAIO;
					break;
				case COERCION_METHOD_BINARY:
					result = COERCION_PATH_RELABELTYPE;
					break;
				default:
					elog(ERROR, "unrecognized castmethod: %d",
						 (int) castForm->castmethod);
					break;
			}
		}

		ReleaseSysCache(tuple);
	}
	else
	{
		/*
		 * 若无 pg_cast 条目，也许我们面对的是一对数组      
		 * 类型。若如此，且两元素类型间存在转换路        
		 * 径，则报告可用 ArrayCoerceExpr 完成转换。      
		 *
		 * Hack：不允许转换到 oidvector 与 int2vector，      
		 * 否则它们往往会捕获本应去向"真实"数组类型的转 
		 * 换。我们希望在很多场景下这些类型被视为"真实"  
		 * 数组，但不包括这一场景。（另外，         
		 * ArrayCoerceExpr 不保证输出符合这些数据类型的约 
		 * 束，例如一维性。）                   
		 */
		if (targetTypeId != OIDVECTOROID && targetTypeId != INT2VECTOROID)
		{
			Oid			targetElem;
			Oid			sourceElem;

			if ((targetElem = get_element_type(targetTypeId)) != InvalidOid &&
				(sourceElem = get_element_type(sourceTypeId)) != InvalidOid)
			{
				CoercionPathType elempathtype;
				Oid			elemfuncid;

				elempathtype = find_coercion_pathway(targetElem,
													 sourceElem,
													 ccontext,
													 &elemfuncid);
				if (elempathtype != COERCION_PATH_NONE)
				{
					result = COERCION_PATH_ARRAYCOERCE;
				}
			}
		}

		/*
		 * 若仍未找到可行途径，考虑用 I/O 函数自动转换。  
		 * 我们允许赋值 cast 到字符串类型，以及从字符串类 
		 * 型出发的显式 cast 以此处理。（CoerceViaIO 机制 
		 * 比这通用得多，但在没有 pg_cast 条目时我们只想   
		 * 允许这么多。）若两个方向都坚持显式 cast，也许
		 * 会更好，但为保留 8.3 前的行为，这是一种妥协。 
		 * 此前许多类型曾带有到 text 隐式（哇！）cast。
		 *                                                              
		 */
		if (result == COERCION_PATH_NONE)
		{
			if (ccontext >= COERCION_ASSIGNMENT &&
				TypeCategory(targetTypeId) == TYPCATEGORY_STRING)
				result = COERCION_PATH_COERCEVIAIO;
			else if (ccontext >= COERCION_EXPLICIT &&
					 TypeCategory(sourceTypeId) == TYPCATEGORY_STRING)
				result = COERCION_PATH_COERCEVIAIO;
		}
	}

	/*
	 * 解析 PL/pgSQL 赋值时，若无常规强制转换可用，  
	 * 则允许使用 I/O 转换。            
	 */
	if (result == COERCION_PATH_NONE &&
		ccontext == COERCION_PLPGSQL)
		result = COERCION_PATH_COERCEVIAIO;

	return result;
}


/*
 * find_typmod_coercion_function -- 该类型是否需要长度强制转换  
 *
 * 若目标类型拥有一个从自身到自身的 pg_cast 函数，    
 * 则必须做长度强制。  
 *
 * "bpchar"（即 char(N)）与 "numeric" 即属此类。          
 *
 * 若给定类型是 varlena 数组类型，我们不找与该数组类   
 * 型直接关联的强制转换函数，而是寻找与元素类型    
 * 关联的那一个。必须使用 ArrayCoerceExpr 节点来应用     
 * 这类函数。（注意：目前此情形下返回 funcid 并     
 * 无意义，因为它随后还会在递归构建 ArrayCoerceExpr      
 * 的 elemexpr 时被再次查找。）                            
 *
 * 我们使用与 find_coercion_pathway 相同的结果枚举，唯一       
 * 结果码是：  
 *	COERCION_PATH_NONE：无需长度强制转换
 *	COERCION_PATH_FUNC：应用 *funcid 返回的函数       
 *	COERCION_PATH_ARRAYCOERCE：用 ArrayCoerceExpr 应用函数       
 */
CoercionPathType
find_typmod_coercion_function(Oid typeId,
							  Oid *funcid)
{
	CoercionPathType result;
	Type		targetType;
	Form_pg_type typeForm;
	HeapTuple	tuple;

	*funcid = InvalidOid;
	result = COERCION_PATH_FUNC;

	targetType = typeidType(typeId);
	typeForm = (Form_pg_type) GETSTRUCT(targetType);

	/* 检查是否"真"数组类型 */
	if (IsTrueArrayType(typeForm))
	{
		/* 是，把注意力转到元素类型上       */
		typeId = typeForm->typelem;
		result = COERCION_PATH_ARRAYCOERCE;
	}
	ReleaseSysCache(targetType);

	/* 查 pg_cast     */
	tuple = SearchSysCache2(CASTSOURCETARGET,
							ObjectIdGetDatum(typeId),
							ObjectIdGetDatum(typeId));

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tuple);

		*funcid = castForm->castfunc;
		ReleaseSysCache(tuple);
	}

	if (!OidIsValid(*funcid))
		result = COERCION_PATH_NONE;

	return result;
}

/*
 * is_complex_array
 *		此类型是否为复合数组？  
 *
 * 注意：对 record[] 不会返回 true；需要时请另行检查    
 * RECORDARRAYOID。    
 */
static bool
is_complex_array(Oid typid)
{
	Oid			elemtype = get_element_type(typid);

	return (OidIsValid(elemtype) && ISCOMPLEX(elemtype));
}


/*
 * 检查 reltypeId 是否为类型为 reloftypeId 的类型化表 
 * 的行类型，或者是否为该行类型上的域。（这在概念   
 * 上类似于 typeInheritsFrom() 所检查的子类型关系。）  
 */
static bool
typeIsOfTypedTable(Oid reltypeId, Oid reloftypeId)
{
	Oid			relid = typeOrDomainTypeRelid(reltypeId);
	bool		result = false;

	if (relid)
	{
		HeapTuple	tp;
		Form_pg_class reltup;

		tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for relation %u", relid);

		reltup = (Form_pg_class) GETSTRUCT(tp);
		if (reltup->reloftype == reloftypeId)
			result = true;

		ReleaseSysCache(tp);
	}

	return result;
}
