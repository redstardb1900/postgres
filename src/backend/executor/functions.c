/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  SQL 语言函数的执行。       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/functions.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "rewrite/rewriteHandler.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * 专用的 DestReceiver，用于收集 SQL 函数的查询输出。    
 */
typedef struct
{
	DestReceiver pub;			/* 对外可见的函数指针      */
	Tuplestorestate *tstore;	/* 结果元组的存放位置*/
	MemoryContext cxt;			/* 包含 tstore 的上下文*/
	JunkFilter *filter;			/* 元组类型转换过滤器  */
} DR_sqlfunction;

/*
 * 函数里的每个查询都有一个 execution_state 记录。       
 * 每条记录都包含其查询的 plantree。若查询当前处于     
 * F_EXEC_RUN 状态，则还有 QueryDesc。    
 *
 * 这些 "next" 字段把由同一个原始 parsetree 生成的             
 * 所有 execution_state 记录链接在一起。（只有在规则      
 * 展开原始 parsetree 时才会有多条。）   
 */
typedef enum
{
	F_EXEC_START, F_EXEC_RUN, F_EXEC_DONE
} ExecStatus;

typedef struct execution_state
{
	struct execution_state *next;
	ExecStatus	status;
	bool		setsResult;		/* true 表示此查询产生函数结果    */
	bool		lazyEval;		/* true 表示应一次只取一行       */
	PlannedStmt *stmt;			/* 此查询的计划  */
	QueryDesc  *qd;				/* status 非 RUN 时为 null*/
} execution_state;


/*
 * SQLFunctionCache 记录在首次调用时构建，        
 * 并从 FmgrInfo 结构的 fn_extra 字段链接过来。     
 *
 * 注意：目前它的生存期只有调用查询那么长。        
 * 将来应该重写这段代码，改用 plancache.c 以便把解析/     
 * 规划结果存得更久。  
 *
 * 不过从物理上说，这些数据的生存期与调用函数所用的 
 * FmgrInfo 相同，而且有些场合（尤其是索引方面）      
 * FmgrInfo 可能跨越事务存活。我们不能假定                 
 * 解析/规划树的有效期长于执行解析的那个（子）          
 * 事务，所以必须用创建时的 LXID/subxid 标记该记录，       
 * 一旦其过时就重新生成全部数据。为避免重新生成        
 * 时的内存泄漏，所有数据都保存在 FmgrInfo 的               
 * fn_mcxt 的一个子上下文中。        
 */
typedef struct
{
	char	   *fname;			/* 函数名（用于报错）    */
	char	   *src;			/* 函数体文本（用于报错）   */

	SQLFunctionParseInfoPtr pinfo;	/* 解析器回调钩子的数据 */

	Oid			rettype;		/* 实际返回类型 */
	int16		typlen;			/* 返回类型的长度     */
	bool		typbyval;		/* true 表示返回类型按值传递  */
	bool		returnsSet;		/* true 表示返回多行         */
	bool		returnsTuple;	/* true 表示返回整个元组结果  */
	bool		shutdown_reg;	/* true 表示已注册关闭回调     */
	bool		readonly_func;	/* true 则以"只读"模式运行 */
	bool		lazyEval;		/* true 表示结果查询使用 lazyEval  */

	ParamListInfo paramLI;		/* 代表当前参数的 Param 列表   */

	Tuplestorestate *tstore;	/* 累积结果元组的位置       */

	JunkFilter *junkFilter;		/* 若函数返回 VOID 则为 NULL      */

	/*
	 * func_state 是一个 execution_state 记录的 List，其中每条   
	 * 都是其原始 parsetree 的第一条，其余记录通过 "next"   
	 * 字段链接到它。之所以需要这种子列表结构，是       
	 * 为了追踪原始查询边界的位置。       
	 */
	List	   *func_state;

	MemoryContext fcontext;		/* memory context holding this struct and all
								 * subsidiary data */

	LocalTransactionId lxid;	/* 创建缓存时的 lxid      */
	SubTransactionId subxid;	/* 创建缓存时的 subxid      */
} SQLFunctionCache;

typedef SQLFunctionCache *SQLFunctionCachePtr;


/* 非导出函数的原型       */
static Node *sql_fn_param_ref(ParseState *pstate, ParamRef *pref);
static Node *sql_fn_post_column_ref(ParseState *pstate,
									ColumnRef *cref, Node *var);
static Node *sql_fn_make_param(SQLFunctionParseInfoPtr pinfo,
							   int paramno, int location);
static Node *sql_fn_resolve_param_name(SQLFunctionParseInfoPtr pinfo,
									   const char *paramname, int location);
static List *init_execution_state(List *queryTree_list,
								  SQLFunctionCachePtr fcache,
								  bool lazyEvalOK);
static void init_sql_fcache(FunctionCallInfo fcinfo, Oid collation, bool lazyEvalOK);
static void postquel_start(execution_state *es, SQLFunctionCachePtr fcache);
static bool postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache);
static void postquel_end(execution_state *es);
static void postquel_sub_params(SQLFunctionCachePtr fcache,
								FunctionCallInfo fcinfo);
static Datum postquel_get_single_result(TupleTableSlot *slot,
										FunctionCallInfo fcinfo,
										SQLFunctionCachePtr fcache,
										MemoryContext resultcontext);
static void sql_exec_error_callback(void *arg);
static void ShutdownSQLFunction(Datum arg);
static bool coerce_fn_result_column(TargetEntry *src_tle,
									Oid res_type, int32 res_typmod,
									bool tlist_is_modifiable,
									List **upper_tlist,
									bool *upper_tlist_nontrivial);
static void sqlfunction_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self);
static void sqlfunction_shutdown(DestReceiver *self);
static void sqlfunction_destroy(DestReceiver *self);


/*
 * 构建 SQLFunctionParseInfo 结构，用于解析 SQL 函数体。     
 *
 * 这包括解析多态参数的实际类型。                 
 *
 * call_expr 可以传 NULL，但只要存在多态参数，              
 * 就会失败。       
 */
SQLFunctionParseInfoPtr
prepare_sql_fn_parse_info(HeapTuple procedureTuple,
						  Node *call_expr,
						  Oid inputCollation)
{
	SQLFunctionParseInfoPtr pinfo;
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	int			nargs;

	pinfo = (SQLFunctionParseInfoPtr) palloc0(sizeof(SQLFunctionParseInfo));

	/* 只有函数名可用于限定参数名                      */
	pinfo->fname = pstrdup(NameStr(procedureStruct->proname));

	/* 保存函数的输入排序规则   */
	pinfo->collation = inputCollation;

	/*
	 * 从 pg_proc 条目复制输入参数类型，然后解决一切   
	 * 多态类型。   
	 */
	pinfo->nargs = nargs = procedureStruct->pronargs;
	if (nargs > 0)
	{
		Oid		   *argOidVect;
		int			argnum;

		argOidVect = (Oid *) palloc(nargs * sizeof(Oid));
		memcpy(argOidVect,
			   procedureStruct->proargtypes.values,
			   nargs * sizeof(Oid));

		for (argnum = 0; argnum < nargs; argnum++)
		{
			Oid			argtype = argOidVect[argnum];

			if (IsPolymorphicType(argtype))
			{
				argtype = get_call_expr_argtype(call_expr, argnum);
				if (argtype == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine actual type of argument declared %s",
									format_type_be(argOidVect[argnum]))));
				argOidVect[argnum] = argtype;
			}
		}

		pinfo->argtypes = argOidVect;
	}

	/*
	 * 若存在参数名，也一并收集   
	 */
	if (nargs > 0)
	{
		Datum		proargnames;
		Datum		proargmodes;
		int			n_arg_names;
		bool		isNull;

		proargnames = SysCacheGetAttr(PROCNAMEARGSNSP, procedureTuple,
									  Anum_pg_proc_proargnames,
									  &isNull);
		if (isNull)
			proargnames = PointerGetDatum(NULL);	/* 以确保无误 */

		proargmodes = SysCacheGetAttr(PROCNAMEARGSNSP, procedureTuple,
									  Anum_pg_proc_proargmodes,
									  &isNull);
		if (isNull)
			proargmodes = PointerGetDatum(NULL);	/* 以确保无误 */

		n_arg_names = get_func_input_arg_names(proargnames, proargmodes,
											   &pinfo->argnames);

		/* 偏执检查：数组条目太少时忽略结果     */
		if (n_arg_names < nargs)
			pinfo->argnames = NULL;
	}
	else
		pinfo->argnames = NULL;

	return pinfo;
}

/*
 * 解析 SQL 函数体的解析器设置钩子。   
 */
void
sql_fn_parser_setup(struct ParseState *pstate, SQLFunctionParseInfoPtr pinfo)
{
	pstate->p_pre_columnref_hook = NULL;
	pstate->p_post_columnref_hook = sql_fn_post_column_ref;
	pstate->p_paramref_hook = sql_fn_param_ref;
	/* 无需使用 p_coerce_param_hook   */
	pstate->p_ref_hook_state = (void *) pinfo;
}

/*
 * sql_fn_post_column_ref		ColumnRef 的解析器回调  
 */
static Node *
sql_fn_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var)
{
	SQLFunctionParseInfoPtr pinfo = (SQLFunctionParseInfoPtr) pstate->p_ref_hook_state;
	int			nnames;
	Node	   *field1;
	Node	   *subfield = NULL;
	const char *name1;
	const char *name2 = NULL;
	Node	   *param;

	/*
	 * 绝不覆盖对表列的引用。这样做相当于          
	 * 把参数名视为在各条 SQL 命令外层的作用域中，  
	 * 而这正是我们想要的效果。           
	 */
	if (var != NULL)
		return NULL;

	/*----------
	 * 允许的语法形式： 
	 *
	 * A		A = 参数名     
	 * A.B		A = 函数名，B = 参数名        
	 *			或：A = 记录类型参数名，B = 字段名    
	 *			（第一种可能优先）             
	 * A.B.C	A = 函数名，B = 记录类型参数名，       
	 *			C = 字段名 
	 * A.*		对复合参数 A 的整行引用。         
	 * A.B.*	同上，但 A = 函数名，B = 参数名      
	 *
	 * 对于最后两种情况，忽略 "*" 即可，因为主解析器会 
	 * 负责展开整行引用。                                     
	 *----------
	 */
	nnames = list_length(cref->fields);

	if (nnames > 3)
		return NULL;

	if (IsA(llast(cref->fields), A_Star))
		nnames--;

	field1 = (Node *) linitial(cref->fields);
	name1 = strVal(field1);
	if (nnames > 1)
	{
		subfield = (Node *) lsecond(cref->fields);
		name2 = strVal(subfield);
	}

	if (nnames == 3)
	{
		/*
		 * 三段式名称：若第一段与函数名不匹配，             
		 * 就可以立即失败。否则查找第二段，                
		 * 并把第三段当作字段引用。        
		 */
		if (strcmp(name1, pinfo->fname) != 0)
			return NULL;

		param = sql_fn_resolve_param_name(pinfo, name2, cref->location);

		subfield = (Node *) lthird(cref->fields);
		Assert(IsA(subfield, String));
	}
	else if (nnames == 2 && strcmp(name1, pinfo->fname) == 0)
	{
		/*
		 * 两段式名称且第一段与函数名匹配：先看第二段   
		 * 是否匹配某个参数名。         
		 */
		param = sql_fn_resolve_param_name(pinfo, name2, cref->location);

		if (param)
		{
			/* 是，所以这是参数引用，无子字段      */
			subfield = NULL;
		}
		else
		{
			/* 否，尝试按参数名+子字段匹配           */
			param = sql_fn_resolve_param_name(pinfo, name1, cref->location);
		}
	}
	else
	{
		/* 单一名称，或参数名后跟子字段          */
		param = sql_fn_resolve_param_name(pinfo, name1, cref->location);
	}

	if (!param)
		return NULL;			/* 无匹配*/

	if (subfield)
	{
		/*
		 * 必然是对某个复合参数某字段的引用；否则         
		 * ParseFuncOrColumn 会返回 NULL，报错发生在调用      
		 * 方。 
		 */
		param = ParseFuncOrColumn(pstate,
								  list_make1(subfield),
								  list_make1(param),
								  pstate->p_last_srf,
								  NULL,
								  false,
								  cref->location);
	}

	return param;
}

/*
 * sql_fn_param_ref		ParamRefs（$n 符号）解析器回调   
 */
static Node *
sql_fn_param_ref(ParseState *pstate, ParamRef *pref)
{
	SQLFunctionParseInfoPtr pinfo = (SQLFunctionParseInfoPtr) pstate->p_ref_hook_state;
	int			paramno = pref->number;

	/* 检查参数编号是否有效  */
	if (paramno <= 0 || paramno > pinfo->nargs)
		return NULL;			/* 参数编号未知       */

	return sql_fn_make_param(pinfo, paramno, pref->location);
}

/*
 * sql_fn_make_param		为给定 paramno 构造 Param 节点       
 */
static Node *
sql_fn_make_param(SQLFunctionParseInfoPtr pinfo,
				  int paramno, int location)
{
	Param	   *param;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = pinfo->argtypes[paramno - 1];
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = location;

	/*
	 * 若有函数输入排序规则，就允许它覆盖            
	 * 参数符号从类型派生的排序规则。（XXX 或许若类型    
	 * 排序规则并非默认，就不该如此？） 
	 */
	if (OidIsValid(pinfo->collation) && OidIsValid(param->paramcollid))
		param->paramcollid = pinfo->collation;

	return (Node *) param;
}

/*
 * 按给定名字查找函数参数；若存在，                   
 * 就为它构造并返回 Param 节点。否则返回 NULL。   
 * sql_fn_post_column_ref 的辅助函数。  
 */
static Node *
sql_fn_resolve_param_name(SQLFunctionParseInfoPtr pinfo,
						  const char *paramname, int location)
{
	int			i;

	if (pinfo->argnames == NULL)
		return NULL;

	for (i = 0; i < pinfo->nargs; i++)
	{
		if (pinfo->argnames[i] && strcmp(pinfo->argnames[i], paramname) == 0)
			return sql_fn_make_param(pinfo, i + 1, location);
	}

	return NULL;
}

/*
 * 为一个 SQL 函数设置各查询的 execution_state 记录。
 *
 * 输入是 List 的 List，各元素是已解析并重写、           
 * 但尚未规划的查询树。子列表结构标示原始查询边界。 
 */
static List *
init_execution_state(List *queryTree_list,
					 SQLFunctionCachePtr fcache,
					 bool lazyEvalOK)
{
	List	   *eslist = NIL;
	execution_state *lasttages = NULL;
	ListCell   *lc1;

	foreach(lc1, queryTree_list)
	{
		List	   *qtlist = lfirst_node(List, lc1);
		execution_state *firstes = NULL;
		execution_state *preves = NULL;
		ListCell   *lc2;

		foreach(lc2, qtlist)
		{
			Query	   *queryTree = lfirst_node(Query, lc2);
			PlannedStmt *stmt;
			execution_state *newes;

			/* 必要时规划查询    */
			if (queryTree->commandType == CMD_UTILITY)
			{
				/* 实用命令无需规划。           */
				stmt = makeNode(PlannedStmt);
				stmt->commandType = CMD_UTILITY;
				stmt->canSetTag = queryTree->canSetTag;
				stmt->utilityStmt = queryTree->utilityStmt;
				stmt->stmt_location = queryTree->stmt_location;
				stmt->stmt_len = queryTree->stmt_len;
				stmt->queryId = queryTree->queryId;
			}
			else
				stmt = pg_plan_query(queryTree,
									 fcache->src,
									 CURSOR_OPT_PARALLEL_OK,
									 NULL);

			/*
			 * 预检查函数中各命令是否有效。一般应当与     
			 * spi.c 施加的限制保持一致。           
			 */
			if (stmt->commandType == CMD_UTILITY)
			{
				if (IsA(stmt->utilityStmt, CopyStmt) &&
					((CopyStmt *) stmt->utilityStmt)->filename == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot COPY to/from client in an SQL function")));

				if (IsA(stmt->utilityStmt, TransactionStmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/* translator: %s is a SQL statement name */
							 errmsg("%s is not allowed in an SQL function",
									CreateCommandName(stmt->utilityStmt))));
			}

			if (fcache->readonly_func && !CommandIsReadOnly(stmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
						 errmsg("%s is not allowed in a non-volatile function",
								CreateCommandName((Node *) stmt))));

			/* 好，为此查询构建 execution_state     */
			newes = (execution_state *) palloc(sizeof(execution_state));
			if (preves)
				preves->next = newes;
			else
				firstes = newes;

			newes->next = NULL;
			newes->status = F_EXEC_START;
			newes->setsResult = false;	/* 下文可能修改 */
			newes->lazyEval = false;	/* 下文可能修改 */
			newes->stmt = stmt;
			newes->qd = NULL;

			if (queryTree->canSetTag)
				lasttages = newes;

			preves = newes;
		}

		eslist = lappend(eslist, firstes);
	}

	/*
	 * 把最后一个 canSetTag 查询标记为交付函数结果；        
	 * 若它是简单 SELECT，则标记为惰性求值。若不是      
	 * SELECT，就必须始终运行到结束。 
	 *
	 * 注意：将来可能会增加更多判据，来决定是否           
	 * 采用惰性求值。不过，只要函数不返回集合，         
	 * 就应优先采用它，因为此时取出多行毫无用处。        
	 *      
	 *
	 * 注意：若函数返回 VOID，则不要设置 setsResult，        
	 * 依据是未创建 junkfilter。这保证我们会丢弃这类     
	 * 函数中最后一条语句的所有输出。     
	 */
	if (lasttages && fcache->junkFilter)
	{
		lasttages->setsResult = true;
		if (lazyEvalOK &&
			lasttages->stmt->commandType == CMD_SELECT &&
			!lasttages->stmt->hasModifyingCTE)
			fcache->lazyEval = lasttages->lazyEval = true;
	}

	return eslist;
}

/*
 * 为一个 SQL 函数初始化 SQLFunctionCache    
 */
static void
init_sql_fcache(FunctionCallInfo fcinfo, Oid collation, bool lazyEvalOK)
{
	FmgrInfo   *finfo = fcinfo->flinfo;
	Oid			foid = finfo->fn_oid;
	MemoryContext fcontext;
	MemoryContext oldcontext;
	Oid			rettype;
	TupleDesc	rettupdesc;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	SQLFunctionCachePtr fcache;
	List	   *queryTree_list;
	List	   *resulttlist;
	ListCell   *lc;
	Datum		tmp;
	bool		isNull;

	/*
	 * 创建容纳全部 SQLFunctionCache 数据的内存上下文。它 
	 * 必须位于持有 FmgrInfo 的上下文之下。      
	 */
	fcontext = AllocSetContextCreate(finfo->fn_mcxt,
									 "SQL function",
									 ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(fcontext);

	/*
	 * 创建结构本身，把它链接到 fcontext 与 fn_extra。一旦   
	 * 完成这一步，即使 FmgrInfo 长期存活，失败后也          
	 * 能回收其内存。  
	 */
	fcache = (SQLFunctionCachePtr) palloc0(sizeof(SQLFunctionCache));
	fcache->fcontext = fcontext;
	finfo->fn_extra = (void *) fcache;

	/*
	 * 取得与给定函数 Oid 对应的过程元组                
	 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(foid));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", foid);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/*
	 * 立刻复制函数名，供错误报告回调使用，同时也        
	 * 作为内存上下文标识符。   
	 */
	fcache->fname = pstrdup(NameStr(procedureStruct->proname));
	MemoryContextSetIdentifier(fcontext, fcache->fname);

	/*
	 * 解决一切多态性，得到实际返回类型，以及对应    
	 * 的 tupdesc（若为行类型）。     
	 */
	(void) get_call_result_type(fcinfo, &rettype, &rettupdesc);

	fcache->rettype = rettype;

	/* 获取结果类型的 typlen 与 byval 信息       */
	get_typlenbyval(rettype, &fcache->typlen, &fcache->typbyval);

	/* 记住我们是否正在返回 setof 结果      */
	fcache->returnsSet = procedureStruct->proretset;

	/* 记住函数是否 STABLE/IMMUTABLE      */
	fcache->readonly_func =
		(procedureStruct->provolatile != PROVOLATILE_VOLATILE);

	/*
	 * 我们需要把实际参数类型传给解析器。另外还要    
	 * 确保参数符号被视为具有该函数已确定的输入     
	 * 排序规则。          
	 */
	fcache->pinfo = prepare_sql_fn_parse_info(procedureTuple,
											  finfo->fn_expr,
											  collation);

	/*
	 * 当然我们还需要函数体文本。      
	 */
	tmp = SysCacheGetAttrNotNull(PROCOID, procedureTuple, Anum_pg_proc_prosrc);
	fcache->src = TextDatumGetCString(tmp);

	/* 若有 prosqlbody，则关注它而非 prosrc。         */
	tmp = SysCacheGetAttr(PROCOID,
						  procedureTuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);

	/*
	 * 解析并重写函数文本中的查询。使用子列表来        
	 * 记录原始查询的边界。              
	 *
	 * 注意：解析和规划都在 fcontext 中进行，故会产生      
	 * 大量与 fcache 存活期一样长的垃圾。这很烦人，         
	 * 但在该模块被重写为使用 plancache.c 之前，我们    
	 * 不管它。
	 */
	queryTree_list = NIL;
	if (!isNull)
	{
		Node	   *n;
		List	   *stored_query_list;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			stored_query_list = linitial_node(List, castNode(List, n));
		else
			stored_query_list = list_make1(n);

		foreach(lc, stored_query_list)
		{
			Query	   *parsetree = lfirst_node(Query, lc);
			List	   *queryTree_sublist;

			AcquireRewriteLocks(parsetree, true, false);
			queryTree_sublist = pg_rewrite_query(parsetree);
			queryTree_list = lappend(queryTree_list, queryTree_sublist);
		}
	}
	else
	{
		List	   *raw_parsetree_list;

		raw_parsetree_list = pg_parse_query(fcache->src);

		foreach(lc, raw_parsetree_list)
		{
			RawStmt    *parsetree = lfirst_node(RawStmt, lc);
			List	   *queryTree_sublist;

			queryTree_sublist = pg_analyze_and_rewrite_withcb(parsetree,
															  fcache->src,
															  (ParserSetupHook) sql_fn_parser_setup,
															  fcache->pinfo,
															  NULL);
			queryTree_list = lappend(queryTree_list, queryTree_sublist);
		}
	}

	/*
	 * 检查是否存在不允许出现的语句。             
	 */
	check_sql_fn_statements(queryTree_list);

	/*
	 * 检查函数返回的类型确实就是其声称的类型。虽然 
	 * 简单情形下这已在函数定义时做过，但我们必须重  
	 * 新检查，因为函数查询中用到的数据库对象可能        
	 * 已改变类型。若带多态参数反正也得重新检查。      
	 * 此外，check_sql_fn_retval 会负责注入所需的列           
	 * 转换。但我们并不要求它为被删除的列插入           
	 * 空值；那由 junkfilter 处理                                 
	 * 了。
	 *
	 * 注意：我们根据返回的是整个元组结果还是单列，     
	 * 来设置 fcache->returnsTuple。对后一种情形我们会清除    
	 * returnsTuple，因为无需在标量结果情形之外表现不同  
	 * 行为，即使它是行类型列。（不过该情形下必须强     
	 * 制惰性求值模式；否则需额外代码把行类型列展开   
	 * 成多列，因为我们没有办法通知调用者自行        
	 * 展开。）                              
	 */
	fcache->returnsTuple = check_sql_fn_retval_ext(queryTree_list,
												   rettype,
												   rettupdesc,
												   procedureStruct->prokind,
												   false,
												   &resulttlist);

	/*
	 * 构造一个 JunkFilter，用来把返回的行类型强转为         
	 * 所需形式；若结果类型是 VOID，则没有可强转        
	 * 的目标。（XXX 很多时候 JunkFilter 并没做              
	 * 什么要紧事，但本模块的许多部分仍然期待它在    
	 * 那里。）  
	 */
	if (rettype != VOIDOID)
	{
		TupleTableSlot *slot = MakeSingleTupleTableSlot(NULL,
														&TTSOpsMinimalTuple);

		/*
		 * 若结果为复合类型*且*我们返回整个元组结果，    
		 * 就需要为已删除的列插入空值。在单列结果       
		 * 情形中，复合列值内部可能也有已删除的列，但 
		 * 不是我们此处要关心的问题。另外 resulttlist        
		 * 中不应有 resjunk 项，所以在第二种情形下            
		 * JunkFilter 必然是空操作。    
		 */
		if (rettupdesc && fcache->returnsTuple)
			fcache->junkFilter = ExecInitJunkFilterConversion(resulttlist,
															  rettupdesc,
															  slot);
		else
			fcache->junkFilter = ExecInitJunkFilter(resulttlist, slot);
	}

	if (fcache->returnsTuple)
	{
		/* 确保输出行类型被正确祝福         */
		BlessTupleDesc(fcache->junkFilter->jf_resultSlot->tts_tupleDescriptor);
	}
	else if (fcache->returnsSet && type_is_rowtype(fcache->rettype))
	{
		/*
		 * 把行类型当作标量返回 --- 物化模式行不通。       
		 * 目前覆盖调用者对物化模式的偏好即可，但要在
		 * init_execution_state 中为此加入更多智能，就多半    
		 * 需要用三态标志替代 bool。                             
		 */
		lazyEvalOK = true;
	}

	/* 最后规划这些查询  */
	fcache->func_state = init_execution_state(queryTree_list,
											  fcache,
											  lazyEvalOK);

	/* 在 fcache 上标记创建时间以表明其有效   */
	fcache->lxid = MyProc->lxid;
	fcache->subxid = GetCurrentSubTransactionId();

	ReleaseSysCache(procedureTuple);

	MemoryContextSwitchTo(oldcontext);
}

/* 启动一个 execution_state 节点的执行   */
static void
postquel_start(execution_state *es, SQLFunctionCachePtr fcache)
{
	DestReceiver *dest;

	Assert(es->qd == NULL);

	/* 调用者应已确保有合适的快照处于活动状态*/
	Assert(ActiveSnapshotSet());

	/*
	 * 若此查询产生函数结果，就把其输出发送到         
	 * tuplestore，否则丢弃输出。  
	 */
	if (es->setsResult)
	{
		DR_sqlfunction *myState;

		dest = CreateDestReceiver(DestSQLFunction);
		/* 把所需信息向下传递给目标接收器例程     */
		myState = (DR_sqlfunction *) dest;
		Assert(myState->pub.mydest == DestSQLFunction);
		myState->tstore = fcache->tstore;
		myState->cxt = CurrentMemoryContext;
		myState->filter = fcache->junkFilter;
	}
	else
		dest = None_Receiver;

	es->qd = CreateQueryDesc(es->stmt,
							 fcache->src,
							 GetActiveSnapshot(),
							 InvalidSnapshot,
							 dest,
							 fcache->paramLI,
							 es->qd ? es->qd->queryEnv : NULL,
							 0);

	/* 实用命令不需要 Executor。     */
	if (es->qd->operation != CMD_UTILITY)
	{
		/*
		 * 在 lazyEval 模式下，不让执行器建立 AfterTrigger     
		 * 上下文。这不仅是优化，而且是必要的，因为    
		 * 绝不能在 AfterTrigger 层活跃时退出执行。  
		 * 对任何可能排队触发器的语句都小心地避开     
		 * lazyEval 模式。                                                 
		 */
		int			eflags;

		if (es->lazyEval)
			eflags = EXEC_FLAG_SKIP_TRIGGERS;
		else
			eflags = 0;			/* 默认的运行到完成标志  */
		ExecutorStart(es->qd, eflags);
	}

	es->status = F_EXEC_RUN;
}

/* 运行一个 execution_state：跑完或到首个结果行为止     */
/* 若已运行到完成则返回真    */
static bool
postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache)
{
	bool		result;

	if (es->qd->operation == CMD_UTILITY)
	{
		ProcessUtility(es->qd->plannedstmt,
					   fcache->src,
					   true,	/* 保护函数缓存的解析树     */
					   PROCESS_UTILITY_QUERY,
					   es->qd->params,
					   es->qd->queryEnv,
					   es->qd->dest,
					   NULL);
		result = true;			/* 从不过早停止*/
	}
	else
	{
		/* 除非 lazyEval，否则常规命令运行到完成*/
		uint64		count = (es->lazyEval) ? 1 : 0;

		ExecutorRun(es->qd, ForwardScanDirection, count, !fcache->returnsSet || !es->lazyEval);

		/*
		 * 若我们请求了运行到完成，或没有返回元组，     
		 * 命令必定已完成。 
		 */
		result = (count == 0 || es->qd->estate->es_processed == 0);
	}

	return result;
}

/* 关闭一个 execution_state 节点的执行    */
static void
postquel_end(execution_state *es)
{
	/* 标记状态为完成，以免 ExecutorEnd 执行两次  */
	es->status = F_EXEC_DONE;

	/* 实用命令不需要 Executor。     */
	if (es->qd->operation != CMD_UTILITY)
	{
		ExecutorFinish(es->qd);
		ExecutorEnd(es->qd);
	}

	es->qd->dest->rDestroy(es->qd->dest);

	FreeQueryDesc(es->qd);
	es->qd = NULL;
}

/* 构建表示当前参数的 ParamListInfo 数组         */
static void
postquel_sub_params(SQLFunctionCachePtr fcache,
					FunctionCallInfo fcinfo)
{
	int			nargs = fcinfo->nargs;

	if (nargs > 0)
	{
		ParamListInfo paramLI;
		Oid		   *argtypes = fcache->pinfo->argtypes;

		if (fcache->paramLI == NULL)
		{
			paramLI = makeParamList(nargs);
			fcache->paramLI = paramLI;
		}
		else
		{
			paramLI = fcache->paramLI;
			Assert(paramLI->numParams == nargs);
		}

		for (int i = 0; i < nargs; i++)
		{
			ParamExternData *prm = &paramLI->params[i];

			/*
			 * 如果传入参数值是 R/W 展开型 datum，我们会  
			 * 强制它变为 R/O。我们完全有权涂写它，但        
			 * 问题在于函数中参数被多次引用时，前面的  
			 * 引用可能改变后面引用所见值，这完全不行。  
			 * 我们能确定函数计划中 Param 节点的数目，   
			 * 本可以做得更好；但我们可能尚未规划所有 
			 * 语句，也没有遍历计划树的设施。（只看      
			 * 解析树不足以判断，因为规划时可能发生 
			 * 函数内联                                                   
			 * 所致。）                                           
			 */
			prm->isnull = fcinfo->args[i].isnull;
			prm->value = MakeExpandedObjectReadOnly(fcinfo->args[i].value,
													prm->isnull,
													get_typlen(argtypes[i]));
			prm->pflags = 0;
			prm->ptype = argtypes[i];
		}
	}
	else
		fcache->paramLI = NULL;
}

/*
 * 从单个结果行提取 SQL 函数的值。这既用于标量（非集 
 * 合）函数，也用于惰性求值集合的每一行结              
 * 果。 
 */
static Datum
postquel_get_single_result(TupleTableSlot *slot,
						   FunctionCallInfo fcinfo,
						   SQLFunctionCachePtr fcache,
						   MemoryContext resultcontext)
{
	Datum		value;
	MemoryContext oldcontext;

	/*
	 * 准备返回函数值。对于引用传递的数据类型，务必把 
	 * 结果分配到 resultcontext 中，而不是当前内存上下文     
	 * （其生命周期为查询期）。我们不能把数据留在    
	 * TupleTableSlot 中，因为我们打算在返回前清空该槽。  
	 */
	oldcontext = MemoryContextSwitchTo(resultcontext);

	if (fcache->returnsTuple)
	{
		/* 必须以 Datum 返回整个元组。      */
		fcinfo->isnull = false;
		value = ExecFetchSlotHeapTupleDatum(slot);
	}
	else
	{
		/*
		 * 返回标量：需要从 SELECT 结果的第一列中提取它， 
		 * 然后视需要复制到结果上下文中。                     
		 */
		value = slot_getattr(slot, 1, &(fcinfo->isnull));

		if (!fcinfo->isnull)
			value = datumCopy(value, fcache->typbyval, fcache->typlen);
	}

	MemoryContextSwitchTo(oldcontext);

	return value;
}

/*
 * fmgr_sql：SQL 函数的函数调用管理器    
 */
Datum
fmgr_sql(PG_FUNCTION_ARGS)
{
	SQLFunctionCachePtr fcache;
	ErrorContextCallback sqlerrcontext;
	MemoryContext oldcontext;
	bool		randomAccess;
	bool		lazyEvalOK;
	bool		is_first;
	bool		pushed_snapshot;
	execution_state *es;
	TupleTableSlot *slot;
	Datum		result;
	List	   *eslist;
	ListCell   *eslc;

	/*
	 * 为 ereport() 设置错误回溯支持     
	 */
	sqlerrcontext.callback = sql_exec_error_callback;
	sqlerrcontext.arg = fcinfo->flinfo;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* 查调用上下文 */
	if (fcinfo->flinfo->fn_retset)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		/*
		 * 为简单起见，要求调用者支持两种集合求值模式。
		 * 有的情形必须用这种，有的必须用那种，而要等 
		 * 确认之后再检查并不值得。但请注意我们并不要求
		 * 调用者提供 expectedDesc。                          
		 */
		if (!rsi || !IsA(rsi, ReturnSetInfo) ||
			(rsi->allowedModes & SFRM_ValuePerCall) == 0 ||
			(rsi->allowedModes & SFRM_Materialize) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		randomAccess = rsi->allowedModes & SFRM_Materialize_Random;
		lazyEvalOK = !(rsi->allowedModes & SFRM_Materialize_Preferred);
	}
	else
	{
		randomAccess = false;
		lazyEvalOK = true;
	}

	/*
	 * 若是首次执行则初始化 fcache（构建计划）；若缓存    
	 * 已过期则重建。 
	 */
	fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;

	if (fcache != NULL)
	{
		if (fcache->lxid != MyProc->lxid ||
			!SubTransactionIsActive(fcache->subxid))
		{
			/* 过期；解除链接并删除*/
			fcinfo->flinfo->fn_extra = NULL;
			MemoryContextDelete(fcache->fcontext);
			fcache = NULL;
		}
	}

	if (fcache == NULL)
	{
		init_sql_fcache(fcinfo, PG_GET_COLLATION(), lazyEvalOK);
		fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	}

	/*
	 * 切换到 fcache 所在的上下文。这确保我们的           
	 * tuplestore 等拥有足够的生存期。子执行器负责删除 
	 * 每元组信息。（XXX 对于长生命周期的 FmgrInfo，这一   
	 * 策略意味着更多的内存泄漏，但并不完全清楚应把  
	 * 这些东西改存何处。                          
	 */
	oldcontext = MemoryContextSwitchTo(fcache->fcontext);

	/*
	 * 找出函数中第一个未完成的查询，并注明它是否   
	 * 为首个。
	 */
	eslist = fcache->func_state;
	es = NULL;
	is_first = true;
	foreach(eslc, eslist)
	{
		es = (execution_state *) lfirst(eslc);

		while (es && es->status == F_EXEC_DONE)
		{
			is_first = false;
			es = es->next;
		}

		if (es)
			break;
	}

	/*
	 * 如果开始全新执行，则把参数转换为适当格式。（若  
	 * 继续执行，则可复用先前的参数。）  
	 */
	if (is_first && es && es->status == F_EXEC_START)
		postquel_sub_params(fcache, fcinfo);

	/*
	 * 如果尚无 tuplestore，则构建一个来存放结果。注意   
	 * 位于查询生存期上下文。  
	 */
	if (!fcache->tstore)
		fcache->tstore = tuplestore_begin_heap(randomAccess, false, work_mem);

	/*
	 * 依次执行函数中的每条命令，直到命令用尽，或者从 
	 * 惰性求值的 SELECT 中得到一个结果行为止。               
	 *
	 * 关于快照管理的说明：  
	 *
	 * 在只读函数中，我们直接使用外围查询的快照。       
	 *
	 * 在非只读函数中，我们依赖这一事实：绝不会     
	 * 在函数的查询之间暂停执行：在完成之前暂停执行   
	 * 的唯一原因是从一个惰性求值的 SELECT 返回一行。      
	 * 因此首次进入该循环时，我们要么启动新查询（并
	 * 压入新快照），要么从现有查询描述符重新建立活  
	 * 动快照。如果在后续循环执行中需要启动新查询，    
	 * 那么要么需要新快照（且 pushed_snapshot 为假），         
	 * 要么现有快照位于活动栈上，我们只需提升它的
	 * 命令 ID。                                                        
	 */
	pushed_snapshot = false;
	while (es)
	{
		bool		completed;

		if (es->status == F_EXEC_START)
		{
			/*
			 * 若非只读，务必为每条命令递增命令计数器，
			 * 使本事务中到目前为止的所有工作都可见。    
			 * 如果还没有快照，则取一个新快照；       
			 * 否则只需提升现有快照中的命令 ID。            
			 */
			if (!fcache->readonly_func)
			{
				CommandCounterIncrement();
				if (!pushed_snapshot)
				{
					PushActiveSnapshot(GetTransactionSnapshot());
					pushed_snapshot = true;
				}
				else
					UpdateActiveSnapshotCommandId();
			}

			postquel_start(es, fcache);
		}
		else if (!fcache->readonly_func && !pushed_snapshot)
		{
			/* 重新进入函数时重建活动快照                */
			PushActiveSnapshot(es->qd->snapshot);
			pushed_snapshot = true;
		}

		completed = postquel_getnext(es, fcache);

		/*
		 * 如果命令已运行到完成，现在就可以关闭它。     
		 * 需要返回的各行都已安全存入 tuplestore，而我们    
		 * 希望确保例如 AFTER 触发器在我们返回任何结果 
		 * 之前已被触发。另外若函数不返回集合，我们    
		 * 也照样可以将其关闭，因为它必定是 SELECT，而且  
		 * 我们不关心获取更多结果行。        
		 */
		if (completed || !fcache->returnsSet)
			postquel_end(es);

		/*
		 * 若没有关闭则跳出循环（这意味着得到了   
		 * 一个惰性求值的行）。否则我们会继续推进直到
		 * 整个函数跑完，依赖 tuplestore 保管最终要         
		 * 返回的数据。这是必要的，因为设置结果的  
		 * INSERT/UPDATE/DELETE RETURNING 之后可能还              
		 * 有规则插入的额外命令，我们希望在返回任何 
		 * 结果之前把所有这些命令都执行完毕。       
		 */
		if (es->status != F_EXEC_DONE)
			break;

		/*
		 * 前进到下一 execution_state（可能在下个列表）       
		 */
		es = es->next;
		while (!es)
		{
			eslc = lnext(eslist, eslc);
			if (!eslc)
				break;			/* 函数结束    */

			es = (execution_state *) lfirst(eslc);

			/*
			 * 冲刷当前快照，以便为新查询列表取新快照。 
			 * 这确保在原始查询边界处取新快照，与交互式 
			 * 执行的行为保持                                          
			 * 一致。 
			 */
			if (pushed_snapshot)
			{
				PopActiveSnapshot();
				pushed_snapshot = false;
			}
		}
	}

	/*
	 * tuplestore 现在包含着我们应返回的所有行。              
	 */
	if (fcache->returnsSet)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (es)
		{
			/*
			 * 若未等完成就停下，手里必有一个惰性求值  
			 * 行 
			 */
			Assert(es->lazyEval);
			/* 复用 junkfilter 的输出槽来取回元组               */
			Assert(fcache->junkFilter);
			slot = fcache->junkFilter->jf_resultSlot;
			if (!tuplestore_gettupleslot(fcache->tstore, true, false, slot))
				elog(ERROR, "failed to fetch lazy-eval tuple");
			/* 提取结果为 datum，并从槽复制出来             */
			result = postquel_get_single_result(slot, fcinfo,
												fcache, oldcontext);
			/* 清空 tuplestore，但保留它供下次使用 */
			/* 注意：这可能删除槽的内容，但我们不在乎   */
			tuplestore_clear(fcache->tstore);

			/*
			 * 让调用者知道还未完成。  
			 */
			rsi->isDone = ExprMultipleResult;

			/*
			 * 确保即使 exprcontext 未跑完也能被干净地          
			 * 关闭。         
			 */
			if (!fcache->shutdown_reg)
			{
				RegisterExprContextCallback(rsi->econtext,
											ShutdownSQLFunction,
											PointerGetDatum(fcache));
				fcache->shutdown_reg = true;
			}
		}
		else if (fcache->lazyEval)
		{
			/*
			 * 我们已完成惰性求值。请清理。    
			 */
			tuplestore_clear(fcache->tstore);

			/*
			 * 让调用者知道已完成。 
			 */
			rsi->isDone = ExprEndResult;

			fcinfo->isnull = true;
			result = (Datum) 0;

			/* 若注册过关闭回调，则将其注销   */
			if (fcache->shutdown_reg)
			{
				UnregisterExprContextCallback(rsi->econtext,
											  ShutdownSQLFunction,
											  PointerGetDatum(fcache));
				fcache->shutdown_reg = false;
			}
		}
		else
		{
			/*
			 * 我们完成了一次非惰性求值。返回 tuplestore 中  
			 * 的全部内容。（现在释放 tuplestore 是调用者      
			 * 的责任。）       
			 */
			rsi->returnMode = SFRM_Materialize;
			rsi->setResult = fcache->tstore;
			fcache->tstore = NULL;
			/* 必须复制 desc（execSRF.c 会释放它） */
			if (fcache->junkFilter)
				rsi->setDesc = CreateTupleDescCopy(fcache->junkFilter->jf_cleanTupType);

			fcinfo->isnull = true;
			result = (Datum) 0;

			/* 若注册过关闭回调，则将其注销   */
			if (fcache->shutdown_reg)
			{
				UnregisterExprContextCallback(rsi->econtext,
											  ShutdownSQLFunction,
											  PointerGetDatum(fcache));
				fcache->shutdown_reg = false;
			}
		}
	}
	else
	{
		/*
		 * 非集合函数。若有行则返回它；否则返回 NULL。  
		 */
		if (fcache->junkFilter)
		{
			/* 复用 junkfilter 的输出槽来取回元组               */
			slot = fcache->junkFilter->jf_resultSlot;
			if (tuplestore_gettupleslot(fcache->tstore, true, false, slot))
				result = postquel_get_single_result(slot, fcinfo,
													fcache, oldcontext);
			else
			{
				fcinfo->isnull = true;
				result = (Datum) 0;
			}
		}
		else
		{
			/* 只有 VOID 函数和过程才会走到这里          */
			Assert(fcache->rettype == VOIDOID);
			fcinfo->isnull = true;
			result = (Datum) 0;
		}

		/* 清空 tuplestore，但保留它供下次使用 */
		tuplestore_clear(fcache->tstore);
	}

	/* 若已压入快照则弹出        */
	if (pushed_snapshot)
		PopActiveSnapshot();

	/*
	 * 若已执行完函数中的每条命令，那么就完成了。重置  
	 * 各执行状态，以便下次调用时从头再来。
	 */
	if (es == NULL)
	{
		foreach(eslc, fcache->func_state)
		{
			es = (execution_state *) lfirst(eslc);
			while (es)
			{
				es->status = F_EXEC_START;
				es = es->next;
			}
		}
	}

	error_context_stack = sqlerrcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	return result;
}


/*
 * 错误上下文回调，让我们能够提供调用栈回溯  
 */
static void
sql_exec_error_callback(void *arg)
{
	FmgrInfo   *flinfo = (FmgrInfo *) arg;
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) flinfo->fn_extra;
	int			syntaxerrposition;

	/*
	 * 如果 init_sql_fcache() 尚未进展到保存函数名，        
	 * 我们便无能为力。
	 */
	if (fcache == NULL || fcache->fname == NULL)
		return;

	/*
	 * 若存在语法错误位置信息，则转换为内部语法错误。
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0 && fcache->src != NULL)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(fcache->src);
	}

	/*
	 * 尝试确定我们在函数中何处失败。若存在 QueryDesc      
	 * 非空的查询，就指向它。（我们检查这一点而非寻找  
	 * F_EXEC_RUN 状态，这样 ExecutorStart 或 ExecutorEnd    
	 * 期间的错误能归咎于相应的查询；参见 postquel_start 和 
	 * postquel_end.)
	 */
	if (fcache->func_state)
	{
		execution_state *es;
		int			query_num;
		ListCell   *lc;

		es = NULL;
		query_num = 1;
		foreach(lc, fcache->func_state)
		{
			es = (execution_state *) lfirst(lc);
			while (es)
			{
				if (es->qd)
				{
					errcontext("SQL function \"%s\" statement %d",
							   fcache->fname, query_num);
					break;
				}
				es = es->next;
			}
			if (es)
				break;
			query_num++;
		}
		if (es == NULL)
		{
			/*
			 * 无法识别运行中的查询；可能是函数入口、  
			 * 函数退出，或查询之间。 
			 */
			errcontext("SQL function \"%s\"", fcache->fname);
		}
	}
	else
	{
		/*
		 * 假定我们在 init_sql_fcache() 期间失败。（也有可能   
		 * 该函数其实函数体为空，但那样的话我们不妨   
		 * 把所有错误都报告为"during startup"。)   
		 */
		errcontext("SQL function \"%s\" during startup", fcache->fname);
	}
}


/*
 * 回调函数，在一个集合返回函数需要在其运行完毕之前
 * 被关闭时调用                  
 */
static void
ShutdownSQLFunction(Datum arg)
{
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) DatumGetPointer(arg);
	execution_state *es;
	ListCell   *lc;

	foreach(lc, fcache->func_state)
	{
		es = (execution_state *) lfirst(lc);
		while (es)
		{
			/* 关闭仍在运行的一切      */
			if (es->status == F_EXEC_RUN)
			{
				/* 为所有被调用的函数重建活动快照         */
				if (!fcache->readonly_func)
					PushActiveSnapshot(es->qd->snapshot);

				postquel_end(es);

				if (!fcache->readonly_func)
					PopActiveSnapshot();
			}

			/* 重置状态为 START，以防再次被调用    */
			es->status = F_EXEC_START;
			es = es->next;
		}
	}

	/* 若持有 tuplestore 则释放之 */
	if (fcache->tstore)
		tuplestore_end(fcache->tstore);
	fcache->tstore = NULL;

	/* execUtils 会自行注销该回调…     */
	fcache->shutdown_reg = false;
}

/*
 * check_sql_fn_statements
 *
 * 检查 SQL 函数中的语句。若存在任何不可接受的           
 * 东西即报错   
 */
void
check_sql_fn_statements(List *queryTreeLists)
{
	ListCell   *lc;

	/* 输入为查询子列表的列表          */
	foreach(lc, queryTreeLists)
	{
		List	   *sublist = lfirst_node(List, lc);
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Query	   *query = lfirst_node(Query, lc2);

			/*
			 * 禁止调用带输出参数的过程。当前的实现只会把
			 * 输出值丢掉，除非它是最后一条语句。按照     
			 * SQL 标准，本应按名称赋输出值。在此处禁止 
			 * 这种做法为我们保留了未来改进的                  
			 * 机会。                                      
			 */
			if (query->commandType == CMD_UTILITY &&
				IsA(query->utilityStmt, CallStmt))
			{
				CallStmt   *stmt = (CallStmt *) query->utilityStmt;

				if (stmt->outargs != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("calling procedures with output arguments is not supported in SQL functions")));
			}
		}
	}
}

/*
 * check_sql_fn_retval()
 *		检查一个 sql 解析树列表的列表的返回值。 
 *
 * SQL 函数的返回值是函数中最后一条 canSetTag            
 * 查询所返回的值。我们在此做一些临时性的类型      
 * 检查与强制转换，以确保函数返回预期的结果。         
 * 我们确实可能会修改最后一个查询来使其匹配！  
 *
 * 若 SQL 函数返回其最后一条语句的整个元组结果，      
 * 此函数返回 true；若只返回该语句第一列的结果，          
 * 则返回 false。若最后一条语句返回的类型完全不对，       
 * 则抛出错误。           
 *
 * 注意：因为我们允许"SELECT rowtype_expression"，即便声明    
 * 的返回类型是行类型，结果也可能为 false。      
 *
 * 对于多态函数，传入的 rettype 必须是已解析出的实际    
 * 输出类型。（这意味着多态函数在定义期间无法         
 * 检查其类型。）若确实看到多态的 rettype，                   
 * 我们将抛出错误，说明它不是受支持的 rettype。     
 *
 * 若函数返回复合类型，传入的 rettupdesc 应描述预期      
 * 的输出。若 rettupdesc 为 NULL，我们就无法验证输出    
 * 是否匹配；这只应发生在 fmgr_sql_validator() 中，或        
 * 当函数返回 RECORD 且调用方实际上并不关心它是        
 * 哪种复合类型。
 *
 * （通常，rettype 和 rettupdesc 由 get_call_result_type            
 * 或同级函数。）  
 *
 * 除了对各个输出列做强制转换，我们还能修改输出  
 * 以便为 rettupdesc 里出现的已删除列加入哑 NULL 列。    
 * 只有调用方要求时才会这么做。                  
 *
 * 若 resultTargetList 非 NULL，则 *resultTargetList 会被        
 * 设为定义最后一条语句结果的目标列表。例外：若      
 * 函数被定义为返回 VOID，则 *resultTargetList 为 NIL。        
 */
bool
check_sql_fn_retval(List *queryTreeLists,
					Oid rettype, TupleDesc rettupdesc,
					bool insertDroppedCols,
					List **resultTargetList)
{
	/* 包装函数，用于在已发布分支中保持 ABI 兼容性      */
	return check_sql_fn_retval_ext(queryTreeLists,
								   rettype, rettupdesc,
								   PROKIND_FUNCTION,
								   insertDroppedCols,
								   resultTargetList);
}

bool
check_sql_fn_retval_ext(List *queryTreeLists,
						Oid rettype, TupleDesc rettupdesc,
						char prokind,
						bool insertDroppedCols,
						List **resultTargetList)
{
	bool		is_tuple_result = false;
	Query	   *parse;
	ListCell   *parse_cell;
	List	   *tlist;
	int			tlistlen;
	bool		tlist_is_modifiable;
	char		fn_typtype;
	List	   *upper_tlist = NIL;
	bool		upper_tlist_nontrivial = false;
	ListCell   *lc;

	if (resultTargetList)
		*resultTargetList = NIL;	/* 初始化，以防结果是 VOID  */

	/*
	 * 若函数被声明为返回 VOID，我们并不关心函数里        
	 * 有什么。（这同时也照顾到无输出参数的过程。） 
	 */
	if (rettype == VOIDOID)
		return false;

	/*
	 * 在函数体中找出最后一条 canSetTag 查询（函数体以     
	 * Query 节点子列表列表的形式交给我们）。这未必是  
	 * 最后一个解析树，因为规则重写会在用户所写内容 
	 * 之后插入查询。注意它甚至可能不在最后的子列 
	 * 表中，比如最后一条查询被重写为 DO INSTEAD NOTHING。   
	 * （在这种情况下抛出错误也并非不合理，但这是    
	 * 历史行为，而且似乎不值得改变。）                    
	 */
	parse = NULL;
	parse_cell = NULL;
	foreach(lc, queryTreeLists)
	{
		List	   *sublist = lfirst_node(List, lc);
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Query	   *q = lfirst_node(Query, lc2);

			if (q->canSetTag)
			{
				parse = q;
				parse_cell = lc2;
			}
		}
	}

	/*
	 * 若是普通 SELECT，它返回目标列表所给出的东西。
	 * 否则，若是带 RETURNING 的 INSERT/UPDATE/DELETE，它返回 
	 * 相应结果。否则返回类型必须是 VOID。     
	 *
	 * 注意：最终可用 QueryReturnsTuples 取代此测试？          
	 * 不过需要更通用的方法来确定输出类型。此外，把     
	 * FETCH 或 EXECUTE 视为返回可确定行类型，似乎过于
	 * 危险，因为它们依赖于寿命相对较短的              
	 * 实体。
	 */
	if (parse &&
		parse->commandType == CMD_SELECT)
	{
		tlist = parse->targetList;
		/* 除 setop 查询中的哑条目外，tlist 均可修改  */
		tlist_is_modifiable = (parse->setOperations == NULL);
	}
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE) &&
			 parse->returningList)
	{
		tlist = parse->returningList;
		/* returningList 总是可以被修改  */
		tlist_is_modifiable = true;
	}
	else
	{
		/* 函数体为空，或者最后一条语句是实用命令   */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type mismatch in function declared to return %s",
						format_type_be(rettype)),
				 errdetail("Function's final statement must be SELECT or INSERT/UPDATE/DELETE RETURNING.")));
		return false;			/* 让编译器安静  */
	}

	/*
	 * 好，现在检查目标列表返回的东西是否与声明的类型
	 * 匹配，必要时修改它。如果可能，我们将任何强制转 
	 * 换步骤直接插入最后一条语句的目标列表。然而，这  
	 * 可能冒险改变语句语义——例如，我们无法安全地  
	 * 改变一个分组列的输出类型。这类情况下，我们通过
	 * 插入一个额外层次的 Query 来处理强制转换，该层次   
	 * 实际只做投影。  
	 */

	/*
	 * 统计结果目标列表中非垃圾项的数量。 
	 */
	tlistlen = ExecCleanTargetListLength(tlist);

	fn_typtype = get_typtype(rettype);

	if (fn_typtype == TYPTYPE_BASE ||
		fn_typtype == TYPTYPE_DOMAIN ||
		fn_typtype == TYPTYPE_ENUM ||
		fn_typtype == TYPTYPE_RANGE ||
		fn_typtype == TYPTYPE_MULTIRANGE)
	{
		/*
		 * 标量类型返回时，目标列表必须恰好有一个     
		 * 非垃圾项，且类型能强制转换为 rettype。     
		 */
		TargetEntry *tle;

		if (tlistlen != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Final statement must return exactly one column.")));

		/* 我们在此假定非垃圾 TLE 必须排在 tlist 的前面*/
		tle = (TargetEntry *) linitial(tlist);
		Assert(!tle->resjunk);

		if (!coerce_fn_result_column(tle, rettype, -1,
									 tlist_is_modifiable,
									 &upper_tlist,
									 &upper_tlist_nontrivial))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Actual return type is %s.",
							   format_type_be(exprType((Node *) tle->expr)))));
	}
	else if (fn_typtype == TYPTYPE_COMPOSITE || rettype == RECORDOID)
	{
		/*
		 * 返回行类型。
		 *
		 * 注意：复合类型上的域不算"行类型"返回类型；
		 * 它会走上面的标量分支。这是因为我们只提供逐列 
		 * 的隐式转换，不会转换完整的记录结果。因此，  
		 * 得到复合域结果的唯一办法是把它算成一个    
		 * 显式单列结果。紧随其下的单复合列代码       
		 * 路径本可以处理这类情形，                            
		 * 但它并不会被走到。                                      
		 */
		int			tupnatts;	/* 元组中的物理列数            */
		int			tuplogcols; /* 元组中未删除列的数量   */
		int			colindex;	/* 物理列索引       */

		/*
		 * 若目标列表只有一个非垃圾项，且该表达式具有   
		 * 声明返回类型或可强制转换为它，就把它当作 
		 * 结果。例如这允许 'SELECT func2()'，其中 func2 与    
		 * 调用它的函数具有相同的复合返回类型。           
		 * 这条规定带来了一些含混——也许该表达式本意是 
		 * 复合结果里唯一的字段——但只要我们不过分     
		 * 热衷于发明从标量到复合类型的强制转换，它就 
		 * 足够好用。                                    
		 *
		 * XXX 注意：若 rettype 为 RECORD 而表达式属于某个命    
		 * 名的复合类型，或反之，此强制转换都会成功，无
		 * 论记录类型是否真正匹配。目前我们依靠运行时  
		 * 类型检查来捕获差异，但最好能在解析阶段做得    
		 * 更好。               
		 *
		 * 但对过程*绝不能*这么做。带输出参数的过程     
		 * 其 rettype 为 RECORD，而 CALL 代码预期得到的结果     
		 * 对应于输出参数列表，即使只有一个参数且它是    
		 * 复合类型的。                               
		 */
		if (tlistlen == 1 && prokind != PROKIND_PROCEDURE)
		{
			TargetEntry *tle = (TargetEntry *) linitial(tlist);

			Assert(!tle->resjunk);
			if (coerce_fn_result_column(tle, rettype, -1,
										tlist_is_modifiable,
										&upper_tlist,
										&upper_tlist_nontrivial))
			{
				/* 注意我们并未设置 is_tuple_result    */
				goto tlist_coercion_finished;
			}
		}

		/*
		 * 若调用方未提供预期 tupdesc，我们就无法再继续   
		 * 检查。假定我们返回的是整个元组。          
		 */
		if (rettupdesc == NULL)
		{
			/* 若需要则返回 tlist  */
			if (resultTargetList)
				*resultTargetList = tlist;
			return true;
		}

		/*
		 * 验证目标列表与返回元组类型匹配。我们扫描所有
		 * 非 resjunk 列，必要时强制转换它们，使其匹配未删
		 * 除属性的数据类型。对已删除的属性，若调用方  
		 * 要求，则插入 NULL 结果列。                    
		 */
		tupnatts = rettupdesc->natts;
		tuplogcols = 0;			/* 边处理边统计未删除列数    */
		colindex = 0;

		foreach(lc, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Form_pg_attribute attr;

			/* resjunk 列可直接忽略            */
			if (tle->resjunk)
				continue;

			do
			{
				colindex++;
				if (colindex > tupnatts)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("return type mismatch in function declared to return %s",
									format_type_be(rettype)),
							 errdetail("Final statement returns too many columns.")));
				attr = TupleDescAttr(rettupdesc, colindex - 1);
				if (attr->attisdropped && insertDroppedCols)
				{
					Expr	   *null_expr;

					/* 我们插入的 NULL 值其类型并不重要  */
					null_expr = (Expr *) makeConst(INT4OID,
												   -1,
												   InvalidOid,
												   sizeof(int32),
												   (Datum) 0,
												   true,	/* isnull */
												   true /* byval */ );
					upper_tlist = lappend(upper_tlist,
										  makeTargetEntry(null_expr,
														  list_length(upper_tlist) + 1,
														  NULL,
														  false));
					upper_tlist_nontrivial = true;
				}
			} while (attr->attisdropped);
			tuplogcols++;

			if (!coerce_fn_result_column(tle,
										 attr->atttypid, attr->atttypmod,
										 tlist_is_modifiable,
										 &upper_tlist,
										 &upper_tlist_nontrivial))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final statement returns %s instead of %s at column %d.",
								   format_type_be(exprType((Node *) tle->expr)),
								   format_type_be(attr->atttypid),
								   tuplogcols)));
		}

		/* rettupdesc 中剩余的列最好都是已删除的        */
		for (colindex++; colindex <= tupnatts; colindex++)
		{
			if (!TupleDescAttr(rettupdesc, colindex - 1)->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final statement returns too few columns.")));
			if (insertDroppedCols)
			{
				Expr	   *null_expr;

				/* 我们插入的 NULL 值其类型并不重要  */
				null_expr = (Expr *) makeConst(INT4OID,
											   -1,
											   InvalidOid,
											   sizeof(int32),
											   (Datum) 0,
											   true,	/* isnull */
											   true /* byval */ );
				upper_tlist = lappend(upper_tlist,
									  makeTargetEntry(null_expr,
													  list_length(upper_tlist) + 1,
													  NULL,
													  false));
				upper_tlist_nontrivial = true;
			}
		}

		/* 报告我们正在返回整个元组结果       */
		is_tuple_result = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type %s is not supported for SQL functions",
						format_type_be(rettype))));

tlist_coercion_finished:

	/*
	 * 必要时，通过注入一个额外 Query 层次来修改最后的   
	 * Query，它只做一次投影。（对非 SELECT 查询这样做  
	 * 是可疑的，但我们决不必这么做；RETURNING 列表总是  
	 * 可原地修改） 
	 */
	if (upper_tlist_nontrivial)
	{
		Query	   *newquery;
		List	   *colnames;
		RangeTblEntry *rte;
		RangeTblRef *rtr;

		Assert(parse->commandType == CMD_SELECT);

		/* 上层 Query 结构大多数字段可保持为零或空值 */
		newquery = makeNode(Query);
		newquery->commandType = CMD_SELECT;
		newquery->querySource = parse->querySource;
		newquery->canSetTag = true;
		newquery->targetList = upper_tlist;

		/* 需要为子查询 RTE 提供一个适度真实的列名列表    */
		colnames = NIL;
		foreach(lc, parse->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (tle->resjunk)
				continue;
			colnames = lappend(colnames,
							   makeString(tle->resname ? tle->resname : ""));
		}

		/* 为子查询构建一个合适的 RTE */
		rte = makeNode(RangeTblEntry);
		rte->rtekind = RTE_SUBQUERY;
		rte->subquery = parse;
		rte->eref = rte->alias = makeAlias("*SELECT*", colnames);
		rte->lateral = false;
		rte->inh = false;
		rte->inFromCl = true;
		newquery->rtable = list_make1(rte);

		rtr = makeNode(RangeTblRef);
		rtr->rtindex = 1;
		newquery->jointree = makeFromExpr(list_make1(rtr), NULL);

		/*
		 * 若原查询有行安全，确保新查询也标记为具有   
		 * 行安全。      
		 */
		newquery->hasRowSecurity = parse->hasRowSecurity;

		/* 在查询列表中正确的元素处替换原查询             */
		lfirst(parse_cell) = newquery;
	}

	/* 若需要则返回 tlist（可能已修改） */
	if (resultTargetList)
		*resultTargetList = upper_tlist;

	return is_tuple_result;
}

/*
 * 为 check_sql_fn_retval 处理一个函数结果列       
 *
 * 把输出值强转为所需的类型/typmod，并将一列加入     
 * *upper_tlist 中。若加入的上层列表项不只是个 Var， 
 * 则把 *upper_tlist_nontrivial 设为 true。 
 *
 * 成功返回 true；无法强制转换成所需类型则返回
 * false（此时未做任何修改）       
 */
static bool
coerce_fn_result_column(TargetEntry *src_tle,
						Oid res_type,
						int32 res_typmod,
						bool tlist_is_modifiable,
						List **upper_tlist,
						bool *upper_tlist_nontrivial)
{
	TargetEntry *new_tle;
	Expr	   *new_tle_expr;
	Node	   *cast_result;

	/*
	 * 若 TLE 带 sortgroupref 标记，不要改动它，因为它很      
	 * 可能被 ORDER BY、DISTINCT 等引用，而改变其类型会      
	 * 破坏查询语义。否则，原地修改是安全的，除非       
	 * 整个查询对此有问题。            
	 */
	if (tlist_is_modifiable && src_tle->ressortgroupref == 0)
	{
		/* 必要时可原地修改 src_tle            */
		cast_result = coerce_to_target_type(NULL,
											(Node *) src_tle->expr,
											exprType((Node *) src_tle->expr),
											res_type, res_typmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (cast_result == NULL)
			return false;
		assign_expr_collations(NULL, cast_result);
		src_tle->expr = (Expr *) cast_result;
		/* 构造一个引用可能已修改 TLE 的 Var    */
		new_tle_expr = (Expr *) makeVarFromTargetEntry(1, src_tle);
	}
	else
	{
		/* 所有转换必须发生在上层 tlist    */
		Var		   *var = makeVarFromTargetEntry(1, src_tle);

		cast_result = coerce_to_target_type(NULL,
											(Node *) var,
											var->vartype,
											res_type, res_typmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (cast_result == NULL)
			return false;
		assign_expr_collations(NULL, cast_result);
		/* 强制转换实际起作用了吗？   */
		if (cast_result != (Node *) var)
			*upper_tlist_nontrivial = true;
		new_tle_expr = (Expr *) cast_result;
	}
	new_tle = makeTargetEntry(new_tle_expr,
							  list_length(*upper_tlist) + 1,
							  src_tle->resname, false);
	*upper_tlist = lappend(*upper_tlist, new_tle);
	return true;
}


/*
 * CreateSQLFunctionDestReceiver — 创建合适的 DestReceiver 对象 
 */
DestReceiver *
CreateSQLFunctionDestReceiver(void)
{
	DR_sqlfunction *self = (DR_sqlfunction *) palloc0(sizeof(DR_sqlfunction));

	self->pub.receiveSlot = sqlfunction_receive;
	self->pub.rStartup = sqlfunction_startup;
	self->pub.rShutdown = sqlfunction_shutdown;
	self->pub.rDestroy = sqlfunction_destroy;
	self->pub.mydest = DestSQLFunction;

	/* 私有字段将由 postquel_start 设置     */

	return (DestReceiver *) self;
}

/*
 * sqlfunction_startup — 执行器启动 
 */
static void
sqlfunction_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* no-op */
}

/*
 * sqlfunction_receive —接收一个元组
 */
static bool
sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_sqlfunction *myState = (DR_sqlfunction *) self;

	/* 按需过滤元组     */
	slot = ExecFilterJunk(myState->filter, slot);

	/* 把过滤后的元组存入 tuplestore       */
	tuplestore_puttupleslot(myState->tstore, slot);

	return true;
}

/*
 * sqlfunction_shutdown — 结束      
 */
static void
sqlfunction_shutdown(DestReceiver *self)
{
	/* no-op */
}

/*
 * sqlfunction_destroy—释放 DestReceiver 对象   
 */
static void
sqlfunction_destroy(DestReceiver *self)
{
	pfree(self);
}
