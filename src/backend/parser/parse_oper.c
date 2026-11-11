/*-------------------------------------------------------------------------
 *
 * parse_oper.c
 *		为解析器处理操作符      
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_oper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * 操作符旁路缓存哈希表的查找键。为保证哈希一致，     
 * 未用位必须为零 --- 特别是 oprname 必须补零，           
 * 并且 search_path 中未用的表项也必须为零。                
 *
 * search_path 保存导出该表项时的实际 search_path           
 * （若有临时命名空间则去除），否则为单独          
 * 指定的模式 OID（用于查找显式限定的操作符名）。   
 *
 * search_path 必须是定长的，因为哈希表代码要求键       
 * 具有固定尺寸。若搜索路径超过该长度，我们只能     
 * 放弃任何缓存。    
 */

/* 若你的 search_path 比这还长，那你可就惨了 ...  */
#define MAX_CACHED_PATH_LEN		16

typedef struct OprCacheKey
{
	char		oprname[NAMEDATALEN];
	Oid			left_arg;		/* 左输入 OID，前缀则为 0    */
	Oid			right_arg;		/* 右输入 OID   */
	Oid			search_path[MAX_CACHED_PATH_LEN];
} OprCacheKey;

typedef struct OprCacheEntry
{
	/* 哈希查找键必须放在首位 */
	OprCacheKey key;

	Oid			opr_oid;		/* 已解析操作符的 OID    */
} OprCacheEntry;


static Oid	binary_oper_exact(List *opname, Oid arg1, Oid arg2);
static FuncDetailCode oper_select_candidate(int nargs,
											Oid *input_typeids,
											FuncCandidateList candidates,
											Oid *operOid);
static const char *op_signature_string(List *op, char oprkind,
									   Oid arg1, Oid arg2);
static void op_error(ParseState *pstate, List *op, char oprkind,
					 Oid arg1, Oid arg2,
					 FuncDetailCode fdresult, int location);
static bool make_oper_cache_key(ParseState *pstate, OprCacheKey *key,
								List *opname, Oid ltypeId, Oid rtypeId,
								int location);
static Oid	find_oper_cache_entry(OprCacheKey *key);
static void make_oper_cache_entry(OprCacheKey *key, Oid opr_oid);
static void InvalidateOprCacheCallBack(Datum arg, int cacheid, uint32 hashvalue);


/*
 * LookupOperName
 *		给定可能限定的操作符名与确切输入数据类型，    
 *		查找操作符。   
 *
 * 前缀操作符传 oprleft=InvalidOid。  
 *
 * 若操作符名不带模式限定，则在当前的命名空间搜索    
 * 路径中查找。    
 *
 * 若未找到操作符，noError 为真时返回 InvalidOid，         
 * 否则抛出错误。pstate 和 location 仅用于报告错误       
 * 位置；不可用时传 NULL/-1。           
 */
Oid
LookupOperName(ParseState *pstate, List *opername, Oid oprleft, Oid oprright,
			   bool noError, int location)
{
	Oid			result;

	result = OpernameGetOprid(opername, oprleft, oprright);
	if (OidIsValid(result))
		return result;

	/* 这里不用 op_error，因为只要求精确匹配              */
	if (!noError)
	{
		char		oprkind;

		if (!OidIsValid(oprleft))
			oprkind = 'l';
		else if (OidIsValid(oprright))
			oprkind = 'b';
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("postfix operators are not supported"),
					 parser_errposition(pstate, location)));
			oprkind = 0;		/* 安抚编译器     */
		}

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s",
						op_signature_string(opername, oprkind,
											oprleft, oprright)),
				 parser_errposition(pstate, location)));
	}

	return InvalidOid;
}

/*
 * LookupOperWithArgs
 *		类似 LookupOperName，但参数类型由 ObjectWithArgs   
 *		节点给出。       
 */
Oid
LookupOperWithArgs(ObjectWithArgs *oper, bool noError)
{
	TypeName   *oprleft,
			   *oprright;
	Oid			leftoid,
				rightoid;

	Assert(list_length(oper->objargs) == 2);
	oprleft = linitial_node(TypeName, oper->objargs);
	oprright = lsecond_node(TypeName, oper->objargs);

	if (oprleft == NULL)
		leftoid = InvalidOid;
	else
		leftoid = LookupTypeNameOid(NULL, oprleft, noError);

	if (oprright == NULL)
		rightoid = InvalidOid;
	else
		rightoid = LookupTypeNameOid(NULL, oprright, noError);

	return LookupOperName(NULL, oper->objname, leftoid, rightoid,
						  noError, -1);
}

/*
 * get_sort_group_operators - 取该类型的默认排序/分组操作符    
 *
 * 一次取出 "<"、"="、">" 操作符以减少查找                
 * 开销（多数调用者至少会用到其中两个）。               
 * 但某数据类型可能只有 "=" 操作符，即它可哈希     
 * 而不可排序。（其他存在/缺少组合不应出现，         
 * 除非系统目录被弄乱了。）                                  
 *
 * 若某操作符缺少且对应 needXX 标志为真，则抛出      
 * 标准错误消息，否则返回 InvalidOid。        
 *
 * 除操作符 OID 本身外，本函数还能判断 "=" 操作          
 * 符是否可哈希。                
 *
 * 调用者对不需要的结果可传 NULL 指针。                   
 *
 * 注意：结果保证是精确匹配或二进制兼容匹配，因为     
 * 多数调用者没有准备应付额外的运行期类型转换步骤。 
 *                
 */
void
get_sort_group_operators(Oid argtype,
						 bool needLT, bool needEQ, bool needGT,
						 Oid *ltOpr, Oid *eqOpr, Oid *gtOpr,
						 bool *isHashable)
{
	TypeCacheEntry *typentry;
	int			cache_flags;
	Oid			lt_opr;
	Oid			eq_opr;
	Oid			gt_opr;
	bool		hashable;

	/*
	 * 通过类型缓存查找这些操作符。 
	 *
	 * 注意：typcache.c 所用的搜索算法保证结果是一致的，  
	 * 即全部来自匹配的操作符类。        
	 */
	if (isHashable != NULL)
		cache_flags = TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR | TYPECACHE_GT_OPR |
			TYPECACHE_HASH_PROC;
	else
		cache_flags = TYPECACHE_LT_OPR | TYPECACHE_EQ_OPR | TYPECACHE_GT_OPR;

	typentry = lookup_type_cache(argtype, cache_flags);
	lt_opr = typentry->lt_opr;
	eq_opr = typentry->eq_opr;
	gt_opr = typentry->gt_opr;
	hashable = OidIsValid(typentry->hash_proc);

	/* 需要时报错         */
	if ((needLT && !OidIsValid(lt_opr)) ||
		(needGT && !OidIsValid(gt_opr)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an ordering operator for type %s",
						format_type_be(argtype)),
				 errhint("Use an explicit ordering operator or modify the query.")));
	if (needEQ && !OidIsValid(eq_opr))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for type %s",
						format_type_be(argtype))));

	/* 按需返回结果       */
	if (ltOpr)
		*ltOpr = lt_opr;
	if (eqOpr)
		*eqOpr = eq_opr;
	if (gtOpr)
		*gtOpr = gt_opr;
	if (isHashable)
		*isHashable = hashable;
}


/* 给定操作符元组，返回其操作符 OID*/
Oid
oprid(Operator op)
{
	return ((Form_pg_operator) GETSTRUCT(op))->oid;
}

/* 给定操作符元组，返回底层函数的 OID          */
Oid
oprfuncid(Operator op)
{
	Form_pg_operator pgopform = (Form_pg_operator) GETSTRUCT(op);

	return pgopform->oprcode;
}


/* binary_oper_exact()
 * 检查与给定操作数类型是否"精确"匹配。     
 *
 * 若某操作数是未知类型字面量，则此处假定它与另一
 * 操作数同类型。此外要考虑另一操作数是域类型、  
 * 需要归约到其基类型才能找到"精确"匹配的情形。    
 *                                                      
 */
static Oid
binary_oper_exact(List *opname, Oid arg1, Oid arg2)
{
	Oid			result;
	bool		was_unknown = false;

	/* 某参数类型未指定？那就用另一个的类型        */
	if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
	{
		arg1 = arg2;
		was_unknown = true;
	}
	else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
	{
		arg2 = arg1;
		was_unknown = true;
	}

	result = OpernameGetOprid(opname, arg1, arg2);
	if (OidIsValid(result))
		return result;

	if (was_unknown)
	{
		/* 此处 arg1 与 arg2 相同，只需查看 arg1         */
		Oid			basetype = getBaseType(arg1);

		if (basetype != arg1)
		{
			result = OpernameGetOprid(opname, basetype, basetype);
			if (OidIsValid(result))
				return result;
		}
	}

	return InvalidOid;
}


/* oper_select_candidate()
 *		给定输入参数类型数组与若干候选，        
 *		尝试消解冲突。                             
 *
 * 返回 FUNCDETAIL_NOTFOUND、FUNCDETAIL_MULTIPLE 或                   
 * FUNCDETAIL_NORMAL。成功时最优候选的 OID 存入                 
 *
 * *operOid。注意调用者已经确定不存在与输入参数类     
 * 型完全匹配的候选。不过不兼容的候选尚未被剪除。      
 *                      
 */
static FuncDetailCode
oper_select_candidate(int nargs,
					  Oid *input_typeids,
					  FuncCandidateList candidates,
					  Oid *operOid) /* 输出参数    */
{
	int			ncandidates;

	/*
	 * 删除那些实际无法接受给定输入类型的候选，     
	 * 无论直接还是经转换接受。   
	 */
	ncandidates = func_match_argtypes(nargs, input_typeids,
									  candidates, &candidates);

	/* 若无候选或仅剩一个候选即告完成       */
	if (ncandidates == 0)
	{
		*operOid = InvalidOid;
		return FUNCDETAIL_NOTFOUND;
	}
	if (ncandidates == 1)
	{
		*operOid = candidates->oid;
		return FUNCDETAIL_NORMAL;
	}

	/*
	 * 采用与歧义函数相同的启发式规则来消解           
	 * 冲突。
	 */
	candidates = func_select_candidate(nargs, input_typeids, candidates);

	if (candidates)
	{
		*operOid = candidates->oid;
		return FUNCDETAIL_NORMAL;
	}

	*operOid = InvalidOid;
	return FUNCDETAIL_MULTIPLE; /* 未能选出最佳候选          */
}


/* oper() -- 查找二元操作符       
 * 给定操作符名与 arg1、arg2 类型，返回 oper结构。 
 *
 * 注意：返回的操作符（若有）仅保证与输入类型 
 * 可强制转换兼容。若需要精确匹配或二进制兼容  
 * 匹配，请勿使用本函数；参见 compatible_oper。         
 *
 * 若未找到匹配操作符，noError 为真则返回 NULL， 
 * 为假则抛出错误。pstate 与 location 仅用于报告错误          
 * 位置；不可用时传 NULL/-1。               
 *
 * 注意：成功时返回的对象是 syscache 表项。调用者      
 * 用完须对该表项调用 ReleaseSysCache()。   
 */
Operator
oper(ParseState *pstate, List *opname, Oid ltypeId, Oid rtypeId,
	 bool noError, int location)
{
	Oid			operOid;
	OprCacheKey key;
	bool		key_ok;
	FuncDetailCode fdresult = FUNCDETAIL_NOTFOUND;
	HeapTuple	tup = NULL;

	/*
	 * 尝试在旁路缓存中查找映射。        
	 */
	key_ok = make_oper_cache_key(pstate, &key, opname, ltypeId, rtypeId, location);

	if (key_ok)
	{
		operOid = find_oper_cache_entry(&key);
		if (OidIsValid(operOid))
		{
			tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
			if (HeapTupleIsValid(tup))
				return (Operator) tup;
		}
	}

	/*
	 * 先尝试"精确"匹配。     
	 */
	operOid = binary_oper_exact(opname, ltypeId, rtypeId);
	if (!OidIsValid(operOid))
	{
		/*
		 * 否则搜索最合适的候选。                 
		 */
		FuncCandidateList clist;

		/* 按名称获取二元操作符     */
		clist = OpernameGetCandidates(opname, 'b', false);

		/* 未找到操作符？失败...   */
		if (clist != NULL)
		{
			/*
			 * 某参数类型未指定？那就用另一个的类型       
			 * （XXX这可能是死代码？）
			 */
			Oid			inputOids[2];

			if (rtypeId == InvalidOid)
				rtypeId = ltypeId;
			else if (ltypeId == InvalidOid)
				ltypeId = rtypeId;
			inputOids[0] = ltypeId;
			inputOids[1] = rtypeId;
			fdresult = oper_select_candidate(2, inputOids, clist, &operOid);
		}
	}

	if (OidIsValid(operOid))
		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));

	if (HeapTupleIsValid(tup))
	{
		if (key_ok)
			make_oper_cache_entry(&key, operOid);
	}
	else if (!noError)
		op_error(pstate, opname, 'b', ltypeId, rtypeId, fdresult, location);

	return (Operator) tup;
}

/* compatible_oper()
 *	给定操作符名和输入数据类型，查找兼容二元操作符 
 *
 *	本函数比 oper() 更严格：不会返回需要转换输入数      
 *	据类型的操作符（但接受二进制兼容的操作符）。       
 *	除此之外语义相同。                           
 */
Operator
compatible_oper(ParseState *pstate, List *op, Oid arg1, Oid arg2,
				bool noError, int location)
{
	Operator	optup;
	Form_pg_operator opform;

	/* oper() 会找出最佳可用匹配        */
	optup = oper(pstate, op, arg1, arg2, noError, location);
	if (optup == (Operator) NULL)
		return (Operator) NULL; /* 必为noError情形  */

	/* 但它足够好吗？  */
	opform = (Form_pg_operator) GETSTRUCT(optup);
	if (IsBinaryCoercible(arg1, opform->oprleft) &&
		IsBinaryCoercible(arg2, opform->oprright))
		return optup;

	/* 不然  */
	ReleaseSysCache(optup);

	if (!noError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator requires run-time type coercion: %s",
						op_signature_string(op, 'b', arg1, arg2)),
				 parser_errposition(pstate, location)));

	return (Operator) NULL;
}

/* compatible_oper_opid() -- 获取二元操作符的 OID
 *
 * 这是一个便利例程，仅从 compatible_oper() 的结果    
 * 中提取操作符 OID。若查找失败且 noError 为真，       
 * 则返回 InvalidOid。          
 */
Oid
compatible_oper_opid(List *op, Oid arg1, Oid arg2, bool noError)
{
	Operator	optup;
	Oid			result;

	optup = compatible_oper(NULL, op, arg1, arg2, noError, -1);
	if (optup != NULL)
	{
		result = oprid(optup);
		ReleaseSysCache(optup);
		return result;
	}
	return InvalidOid;
}


/* left_oper() -- 查找一元左操作符（前缀操作符）     
 * 给定操作符名与参数类型，返回 oper结构。
 *
 * 注意：返回的操作符（若有）仅保证与输入类型 
 * 可强制转换兼容。若需要精确或二进制兼容       
 * 匹配，请勿使用本函数。             
 *
 * 若未找到匹配操作符，noError 为真则返回 NULL， 
 * 为假则抛出错误。pstate 与 location 仅用于报告错误          
 * 位置；不可用时传 NULL/-1。               
 *
 * 注意：成功时返回的对象是 syscache 表项。调用者      
 * 用完须对该表项调用 ReleaseSysCache()。   
 */
Operator
left_oper(ParseState *pstate, List *op, Oid arg, bool noError, int location)
{
	Oid			operOid;
	OprCacheKey key;
	bool		key_ok;
	FuncDetailCode fdresult = FUNCDETAIL_NOTFOUND;
	HeapTuple	tup = NULL;

	/*
	 * 尝试在旁路缓存中查找映射。        
	 */
	key_ok = make_oper_cache_key(pstate, &key, op, InvalidOid, arg, location);

	if (key_ok)
	{
		operOid = find_oper_cache_entry(&key);
		if (OidIsValid(operOid))
		{
			tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
			if (HeapTupleIsValid(tup))
				return (Operator) tup;
		}
	}

	/*
	 * 先尝试"精确"匹配。     
	 */
	operOid = OpernameGetOprid(op, InvalidOid, arg);
	if (!OidIsValid(operOid))
	{
		/*
		 * 否则搜索最合适的候选。                 
		 */
		FuncCandidateList clist;

		/* 按名称获取前缀操作符     */
		clist = OpernameGetCandidates(op, 'l', false);

		/* 未找到操作符？失败...   */
		if (clist != NULL)
		{
			/*
			 * 返回列表的 args 形如 (0, oprright)。为保持         
			 * oper_select_candidate 简单，把有用数据移入          
			 * args[0]。XXX 此处假定可随意改写该列表！     
			 */
			FuncCandidateList clisti;

			for (clisti = clist; clisti != NULL; clisti = clisti->next)
			{
				clisti->args[0] = clisti->args[1];
			}

			/*
			 * 即使只有一个候选也要运行 oper_select_candidate，
			 * 否则可能错误地返回一个类型不兼容的操作符。
			 */
			fdresult = oper_select_candidate(1, &arg, clist, &operOid);
		}
	}

	if (OidIsValid(operOid))
		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));

	if (HeapTupleIsValid(tup))
	{
		if (key_ok)
			make_oper_cache_entry(&key, operOid);
	}
	else if (!noError)
		op_error(pstate, op, 'l', InvalidOid, arg, fdresult, location);

	return (Operator) tup;
}

/*
 * op_signature_string
 *		构建表示操作符名的字符串，包含参数类型。        
 *		结果形如 "integer + integer"。              
 *
 * 这通常用于构造"未找到操作符"错误消息。              
 *          
 */
static const char *
op_signature_string(List *op, char oprkind, Oid arg1, Oid arg2)
{
	StringInfoData argbuf;

	initStringInfo(&argbuf);

	if (oprkind != 'l')
		appendStringInfo(&argbuf, "%s ", format_type_be(arg1));

	appendStringInfoString(&argbuf, NameListToString(op));

	appendStringInfo(&argbuf, " %s", format_type_be(arg2));

	return argbuf.data;			/* 返回 palloc 缓冲区       */
}

/*
 * op_error - 用于报告无法解析的操作符错误的工具例程 
 */
static void
op_error(ParseState *pstate, List *op, char oprkind,
		 Oid arg1, Oid arg2,
		 FuncDetailCode fdresult, int location)
{
	if (fdresult == FUNCDETAIL_MULTIPLE)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("operator is not unique: %s",
						op_signature_string(op, oprkind, arg1, arg2)),
				 errhint("Could not choose a best candidate operator. "
						 "You might need to add explicit type casts."),
				 parser_errposition(pstate, location)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator does not exist: %s",
						op_signature_string(op, oprkind, arg1, arg2)),
				 (!arg1 || !arg2) ?
				 errhint("No operator matches the given name and argument type. "
						 "You might need to add an explicit type cast.") :
				 errhint("No operator matches the given name and argument types. "
						 "You might need to add explicit type casts."),
				 parser_errposition(pstate, location)));
}

/*
 * make_op()
 *		构造操作符表达式。      
 *
 * 转换操作符表达式并确保类型兼容。          
 * 部分类型转换就在这里完成。    
 *
 * last_srf 应是刚开始转换操作符参数之前的             
 * pstate->p_last_srf 副本；用于嵌套 SRF 检测。若                 
 * 调用者反正会因集合返回表达式报错，那么作弊传      
 * pstate->p_last_srf 也没问题。                              
 */
Expr *
make_op(ParseState *pstate, List *opname, Node *ltree, Node *rtree,
		Node *last_srf, int location)
{
	Oid			ltypeId,
				rtypeId;
	Operator	tup;
	Form_pg_operator opform;
	Oid			actual_arg_types[2];
	Oid			declared_arg_types[2];
	int			nargs;
	List	   *args;
	Oid			rettype;
	OpExpr	   *result;

	/* 检查它不是后缀操作符    */
	if (rtree == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("postfix operators are not supported")));

	/* 选择操作符     */
	if (ltree == NULL)
	{
		/* 前缀操作符 */
		rtypeId = exprType(rtree);
		ltypeId = InvalidOid;
		tup = left_oper(pstate, opname, rtypeId, false, location);
	}
	else
	{
		/* 否则为二元操作符   */
		ltypeId = exprType(ltree);
		rtypeId = exprType(rtree);
		tup = oper(pstate, opname, ltypeId, rtypeId, false, location);
	}

	opform = (Form_pg_operator) GETSTRUCT(tup);

	/* 检查它不是壳     */
	if (!RegProcedureIsValid(opform->oprcode))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator is only a shell: %s",
						op_signature_string(opname,
											opform->oprkind,
											opform->oprleft,
											opform->oprright)),
				 parser_errposition(pstate, location)));

	/* 进行类型转换并构建表达式树      */
	if (ltree == NULL)
	{
		/* 前缀操作符 */
		args = list_make1(rtree);
		actual_arg_types[0] = rtypeId;
		declared_arg_types[0] = opform->oprright;
		nargs = 1;
	}
	else
	{
		/* 否则为二元操作符   */
		args = list_make2(ltree, rtree);
		actual_arg_types[0] = ltypeId;
		actual_arg_types[1] = rtypeId;
		declared_arg_types[0] = opform->oprleft;
		declared_arg_types[1] = opform->oprright;
		nargs = 2;
	}

	/*
	 * 确保多态参数与返回类型的一致性，可能调整   
	 * 返回类型或 declared_arg_types（后者将被                  
	 * make_fn_arguments 用作转换目标）           
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   nargs,
											   opform->oprresult,
											   false);

	/* 对参数执行必要的类型转换           */
	make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

	/* 并构建表达式节点      */
	result = makeNode(OpExpr);
	result->opno = oprid(tup);
	result->opfuncid = opform->oprcode;
	result->opresulttype = rettype;
	result->opretset = get_func_retset(opform->oprcode);
	/* opcollid 和 inputcollid 由 parse_collate.c 设置     */
	result->args = args;
	result->location = location;

	/* 若返回集合，检查是否允许 */
	if (result->opretset)
	{
		check_srf_call_placement(pstate, last_srf, location);
		/* ...并记住它，供上层做错误检查            */
		pstate->p_last_srf = (Node *) result;
	}

	ReleaseSysCache(tup);

	return (Expr *) result;
}

/*
 * make_scalar_array_op()
 *		为 "scalar op ANY/ALL (array)" 结构构建表达式树。     
 */
Expr *
make_scalar_array_op(ParseState *pstate, List *opname,
					 bool useOr,
					 Node *ltree, Node *rtree,
					 int location)
{
	Oid			ltypeId,
				rtypeId,
				atypeId,
				res_atypeId;
	Operator	tup;
	Form_pg_operator opform;
	Oid			actual_arg_types[2];
	Oid			declared_arg_types[2];
	List	   *args;
	Oid			rettype;
	ScalarArrayOpExpr *result;

	ltypeId = exprType(ltree);
	atypeId = exprType(rtree);

	/*
	 * 操作符右侧输入将是数组的元素类型。不过，若当  
	 * 前右侧只是未定型字面量，则保持原样，指望能     
	 * 解析出该操作符。                                   
	 */
	if (atypeId == UNKNOWNOID)
		rtypeId = UNKNOWNOID;
	else
	{
		rtypeId = get_base_element_type(atypeId);
		if (!OidIsValid(rtypeId))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("op ANY/ALL (array) requires array on right side"),
					 parser_errposition(pstate, location)));
	}

	/* 现在解析该操作符 */
	tup = oper(pstate, opname, ltypeId, rtypeId, false, location);
	opform = (Form_pg_operator) GETSTRUCT(tup);

	/* 检查它不是壳     */
	if (!RegProcedureIsValid(opform->oprcode))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator is only a shell: %s",
						op_signature_string(opname,
											opform->oprkind,
											opform->oprleft,
											opform->oprright)),
				 parser_errposition(pstate, location)));

	args = list_make2(ltree, rtree);
	actual_arg_types[0] = ltypeId;
	actual_arg_types[1] = rtypeId;
	declared_arg_types[0] = opform->oprleft;
	declared_arg_types[1] = opform->oprright;

	/*
	 * 确保多态参数与返回类型的一致性，可能调整   
	 * 返回类型或 declared_arg_types（后者将被                  
	 * make_fn_arguments 用作转换目标）           
	 */
	rettype = enforce_generic_type_consistency(actual_arg_types,
											   declared_arg_types,
											   2,
											   opform->oprresult,
											   false);

	/*
	 * 检查操作符结果为布尔类型 
	 */
	if (rettype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("op ANY/ALL (array) requires operator to yield boolean"),
				 parser_errposition(pstate, location)));
	if (get_func_retset(opform->oprcode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("op ANY/ALL (array) requires operator not to return a set"),
				 parser_errposition(pstate, location)));

	/*
	 * 现在换回右侧数组类型，安排施加所需转换。     
	 * 这里要当心多态操作符；                                
	 * enforce_generic_type_consistency 是否已把                  
	 * 多态类型换成真实类型？
	 */
	if (IsPolymorphicType(declared_arg_types[1]))
	{
		/* 假定实际数组类型没问题  */
		res_atypeId = atypeId;
	}
	else
	{
		res_atypeId = get_array_type(declared_arg_types[1]);
		if (!OidIsValid(res_atypeId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(declared_arg_types[1])),
					 parser_errposition(pstate, location)));
	}
	actual_arg_types[1] = atypeId;
	declared_arg_types[1] = res_atypeId;

	/* 对参数执行必要的类型转换           */
	make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

	/* 并构建表达式节点      */
	result = makeNode(ScalarArrayOpExpr);
	result->opno = oprid(tup);
	result->opfuncid = opform->oprcode;
	result->hashfuncid = InvalidOid;
	result->negfuncid = InvalidOid;
	result->useOr = useOr;
	/* inputcollid 由 parse_collate.c 设置     */
	result->args = args;
	result->location = location;

	ReleaseSysCache(tup);

	return (Expr *) result;
}


/*
 * 用于加速操作符查找的旁路缓存。也许它应该放到   
 * utils/cache/ 下的独立模块？    
 *
 * 这里的想法是：对给定 search_path（或单独指定的模式  
 *  OID）而言，只要 pg_operator 和 pg_cast 的内容不变，操作符
 * 名与给定参数类型到解析结果的映射就保持不变。而该  
 * 映射的计算代价相当高，尤其是有歧义的操作符；         
 * 这主要是因为 "=" 等常用操作符名的实例*非常*多，      
 * 必须逐一检查以确定哪个是最佳匹配。所以一旦     
 * 确定正确的映射，就把它存入缓存，                       
 * 该缓存只需在 pg_operator 或 pg_cast 变化时刷新。         
 * （pg_cast 也必须考虑，因为隐式转换集合的变化会       
 * 影响任一给定输入类型的可用操作符集合。）         
 *
 * XXX 原则上 ALTER TABLE ... INHERIT 也可能影响                
 * 该映射，但我们不予理会：既无便利办法感知它，     
 * 而且这看上去也是相当牵强的边角情况。         
 *
 * 注意：将来或许值得为函数查找做类似的缓存。不过   
 * 潜在收益要小得多，因为 (a) 函数名通常不像操作符  
 * 名那样重度重载，而且 (b) pg_proc 更新时也得刷新， 
 * 而这类更新很可能比 pg_operator 更新常见得多。       
 *                                           
 */

/* 操作符缓存哈希表     */
static HTAB *OprCacheHash = NULL;


/*
 * make_oper_cache_key
 *		根据操作符名和参数类型填充查找键结构。    
 *
 * 成功返回 true；若 search_path 溢出则返回 false      
 * （因此无法缓存）。    
 *
 * pstate/location 仅用于报告错误位置；不可用时传           
 * NULL/-1。       
 */
static bool
make_oper_cache_key(ParseState *pstate, OprCacheKey *key, List *opname,
					Oid ltypeId, Oid rtypeId, int location)
{
	char	   *schemaname;
	char	   *opername;

	/* 分解名称列表        */
	DeconstructQualifiedName(opname, &schemaname, &opername);

	/* 确保零填充以获得稳定哈希*/
	MemSet(key, 0, sizeof(OprCacheKey));

	/* 把操作符名和输入类型存入键     */
	strlcpy(key->oprname, opername, NAMEDATALEN);
	key->left_arg = ltypeId;
	key->right_arg = rtypeId;

	if (schemaname)
	{
		ParseCallbackState pcbstate;

		/* 只在给定的确切模式搜索 */
		setup_parser_errposition_callback(&pcbstate, pstate, location);
		key->search_path[0] = LookupExplicitNamespace(schemaname, false);
		cancel_parser_errposition_callback(&pcbstate);
	}
	else
	{
		/* 取得活跃搜索路径   */
		if (fetch_search_path_array(key->search_path,
									MAX_CACHED_PATH_LEN) > MAX_CACHED_PATH_LEN)
			return false;		/* 放不下了     */
	}

	return true;
}

/*
 * find_oper_cache_entry
 *
 * 查找与给定键匹配的缓存表项。若找到，返回其中  
 * 的操作符 OID，否则返回 InvalidOid。  
 */
static Oid
find_oper_cache_entry(OprCacheKey *key)
{
	OprCacheEntry *oprentry;

	if (OprCacheHash == NULL)
	{
		/* 首次执行时：初始化哈希表          */
		HASHCTL		ctl;

		ctl.keysize = sizeof(OprCacheKey);
		ctl.entrysize = sizeof(OprCacheEntry);
		OprCacheHash = hash_create("Operator lookup cache", 256,
								   &ctl, HASH_ELEM | HASH_BLOBS);

		/* 安排在 pg_operator 和 pg_cast 变化时刷新缓存   */
		CacheRegisterSyscacheCallback(OPERNAMENSP,
									  InvalidateOprCacheCallBack,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(CASTSOURCETARGET,
									  InvalidateOprCacheCallBack,
									  (Datum) 0);
	}

	/* 查找已有的表项      */
	oprentry = (OprCacheEntry *) hash_search(OprCacheHash,
											 key,
											 HASH_FIND, NULL);
	if (oprentry == NULL)
		return InvalidOid;

	return oprentry->opr_oid;
}

/*
 * make_oper_cache_entry
 *
 * 为给定键插入一个缓存表项。
 */
static void
make_oper_cache_entry(OprCacheKey *key, Oid opr_oid)
{
	OprCacheEntry *oprentry;

	Assert(OprCacheHash != NULL);

	oprentry = (OprCacheEntry *) hash_search(OprCacheHash,
											 key,
											 HASH_ENTER, NULL);
	oprentry->opr_oid = opr_oid;
}

/*
 * pg_operator 和 pg_cast 失效事件的回调    
 */
static void
InvalidateOprCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	OprCacheEntry *hentry;

	Assert(OprCacheHash != NULL);

	/* 目前只是刷新全部表项；想更精细并不容易...*/
	hash_seq_init(&status, OprCacheHash);

	while ((hentry = (OprCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(OprCacheHash,
						&hentry->key,
						HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}
