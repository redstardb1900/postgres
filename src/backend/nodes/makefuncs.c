/*-------------------------------------------------------------------------
 *
 * makefuncs.c
 *	  各种节点的构造函数。这里的函数针对
 *	  最常被创建的那些节点。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/makefuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"


/*
 * makeA_Expr -
 *		制作一个 A_Expr 节点
 */
A_Expr *
makeA_Expr(A_Expr_Kind kind, List *name,
		   Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = name;
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeSimpleA_Expr -
 *		同上，给定一个简单（未限定）的操作符名
 */
A_Expr *
makeSimpleA_Expr(A_Expr_Kind kind, char *name,
				 Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = list_make1(makeString((char *) name));
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeVar -
 *	  创建一个 Var 节点
 */
Var *
makeVar(int varno,
		AttrNumber varattno,
		Oid vartype,
		int32 vartypmod,
		Oid varcollid,
		Index varlevelsup)
{
	Var		   *var = makeNode(Var);

	var->varno = varno;
	var->varattno = varattno;
	var->vartype = vartype;
	var->vartypmod = vartypmod;
	var->varcollid = varcollid;
	var->varlevelsup = varlevelsup;

	/*
	 * 只有少数调用方需要制作 varnullingrels 非空的 Var 节点，
	 * 或 varnosyn/varattnosyn 与 varno/varattno 不同的节点。我们不
	 * 为它们提供单独的参数，只是把它们初始化为 NULL
	 * 以及给定的 varno/varattno。这减少了代码杂乱，也降低了
	 * 大多数调用方出错的可能。
	 */
	var->varnullingrels = NULL;
	var->varnosyn = (Index) varno;
	var->varattnosyn = varattno;

	/* 同样，这里只把 location 设为 "未知" */
	var->location = -1;

	return var;
}

/*
 * makeVarFromTargetEntry -
 *		便捷函数，从 TargetEntry 创建同层的
 *		Var 节点
 */
Var *
makeVarFromTargetEntry(int varno,
					   TargetEntry *tle)
{
	return makeVar(varno,
				   tle->resno,
				   exprType((Node *) tle->expr),
				   exprTypmod((Node *) tle->expr),
				   exprCollation((Node *) tle->expr),
				   0);
}

/*
 * makeWholeRowVar -
 *	  创建表示指定 RTE 整行的 Var 节点
 *
 * 整行引用是一个 Var，其 varno 设为正确的范围
 * 表项，且 varattno == 0 表示它引用整个
 * 元组。（这里用零并不干净，因为它很容易与
 * 错误情形混淆，但现在不值得改。）vartype 表示
 * 一种行类型；要么是命名的复合类型，要么是命名
 * 复合类型上的域（仅当 RTE 是返回该类型的函数时才可能），
 * 要么是 RECORD。本函数封装了确定
 * 应使用的正确行类型 OID 的逻辑。
 *
 * 若 allowScalar 为 true，则当 RTE 是返回非复合结果类型的
 * 单个函数时，我们生成一个直接引用
 * 该函数结果的普通 Var，而不是整行记法否则可能暗示的
 * 单列复合值。
 */
Var *
makeWholeRowVar(RangeTblEntry *rte,
				int varno,
				Index varlevelsup,
				bool allowScalar)
{
	Var		   *result;
	Oid			toid;
	Node	   *fexpr;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 关系：行类型是命名的复合类型 */
			toid = get_rel_type_id(rte->relid);
			if (!OidIsValid(toid))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("relation \"%s\" does not have a composite type",
								get_rel_name(rte->relid))));
			result = makeVar(varno,
							 InvalidAttrNumber,
							 toid,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;

		case RTE_SUBQUERY:

			/*
			 * 对于普通子查询，Var 的类型应为 RECORD。
			 * 不过，若我们看到的子查询是由
			 * 视图或 SRF 展开的（仅在规划期间可能），必须使用
			 * 适当的行类型，使得到的 Var 与
			 * 我们从原始 RTE 生成的类型相同。
			 */
			if (OidIsValid(rte->relid))
			{
				/* 子查询由视图展开而来 */
				toid = get_rel_type_id(rte->relid);
				if (!OidIsValid(toid))
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("relation \"%s\" does not have a composite type",
									get_rel_name(rte->relid))));
			}
			else if (rte->functions)
			{
				/*
				 * 子查询由集合返回函数展开而来。若
				 * 函数不止一个，
				 * 或请求了序号列，就不会发生这种情况。allowScalar 的情况也不必担心，
				 * 因为规划器不用它。
				 * 否则这里必须与下面的 RTE_FUNCTION 代码一致。
				 */
				Assert(!allowScalar);
				fexpr = ((RangeTblFunction *) linitial(rte->functions))->funcexpr;
				toid = exprType(fexpr);
				if (!type_is_rowtype(toid))
					toid = RECORDOID;
			}
			else
			{
				/* FROM 中的普通子查询 */
				toid = RECORDOID;
			}
			result = makeVar(varno,
							 InvalidAttrNumber,
							 toid,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;

		case RTE_FUNCTION:

			/*
			 * 若函数不止一个，或请求了序号列，
			 * 强制结果为 RECORD，因为涉及的列肯定不止一列，
			 * 而且无法是已知的命名类型。
			 */
			if (rte->funcordinality || list_length(rte->functions) != 1)
			{
				/* 总是产生匿名的 RECORD 结果 */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
				break;
			}

			fexpr = ((RangeTblFunction *) linitial(rte->functions))->funcexpr;
			toid = exprType(fexpr);
			if (type_is_rowtype(toid))
			{
				/* 函数返回复合类型；与关系的情形相同 */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 toid,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			else if (allowScalar)
			{
				/* 函数返回标量；直接原样返回其输出 */
				result = makeVar(varno,
								 1,
								 toid,
								 -1,
								 exprCollation(fexpr),
								 varlevelsup);
			}
			else
			{
				/* 函数返回标量，但我们想要复合结果 */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			break;

		default:

			/*
			 * RTE 是连接、表函数、VALUES、CTE 等。我们把这些
			 * 情形表示为 RECORD 类型的整行 Var。（注意在多数
			 * 情形下，该 Var 会在规划期间被展开为 RowExpr，
			 * 但那不是这里要关心的。）
			 */
			result = makeVar(varno,
							 InvalidAttrNumber,
							 RECORDOID,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;
	}

	return result;
}

/*
 * makeTargetEntry -
 *	  创建一个 TargetEntry 节点
 */
TargetEntry *
makeTargetEntry(Expr *expr,
				AttrNumber resno,
				char *resname,
				bool resjunk)
{
	TargetEntry *tle = makeNode(TargetEntry);

	tle->expr = expr;
	tle->resno = resno;
	tle->resname = resname;

	/*
	 * 我们总是把这些字段设为 0。若调用方想改它们，
	 * 必须显式去做。很少有调用方这么做，因此省掉这些
	 * 参数能降低出错的可能。
	 */
	tle->ressortgroupref = 0;
	tle->resorigtbl = InvalidOid;
	tle->resorigcol = 0;

	tle->resjunk = resjunk;

	return tle;
}

/*
 * flatCopyTargetEntry -
 *	  复制一个 TargetEntry，但不复制子结构
 *
 * 这常用于我们只想修改 resno 或替换
 * 一个新表达式时。
 */
TargetEntry *
flatCopyTargetEntry(TargetEntry *src_tle)
{
	TargetEntry *tle = makeNode(TargetEntry);

	Assert(IsA(src_tle, TargetEntry));
	memcpy(tle, src_tle, sizeof(TargetEntry));
	return tle;
}

/*
 * makeFromExpr -
 *	  创建一个 FromExpr 节点
 */
FromExpr *
makeFromExpr(List *fromlist, Node *quals)
{
	FromExpr   *f = makeNode(FromExpr);

	f->fromlist = fromlist;
	f->quals = quals;
	return f;
}

/*
 * makeConst -
 *	  创建一个 Const 节点
 */
Const *
makeConst(Oid consttype,
		  int32 consttypmod,
		  Oid constcollid,
		  int constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval)
{
	Const	   *cnst = makeNode(Const);

	/*
	 * 若它是 varlena 值，强制其为非展开（非 toast）
	 * 格式；这避免对外部值的任何可能依赖，并
	 * 改善表示的一致性，这对 equal() 很重要。
	 */
	if (!constisnull && constlen == -1)
		constvalue = PointerGetDatum(PG_DETOAST_DATUM(constvalue));

	cnst->consttype = consttype;
	cnst->consttypmod = consttypmod;
	cnst->constcollid = constcollid;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;
	cnst->location = -1;		/* "未知" */

	return cnst;
}

/*
 * makeNullConst -
 *	  创建一个表示指定 type/typmod 的 NULL 的 Const 节点
 *
 * 这是个便捷例程，只是省去了查找该类型
 * 存储属性的步骤。
 */
Const *
makeNullConst(Oid consttype, int32 consttypmod, Oid constcollid)
{
	int16		typLen;
	bool		typByVal;

	get_typlenbyval(consttype, &typLen, &typByVal);
	return makeConst(consttype,
					 consttypmod,
					 constcollid,
					 (int) typLen,
					 (Datum) 0,
					 true,
					 typByVal);
}

/*
 * makeBoolConst -
 *	  创建一个表示布尔值的 Const 节点（也可以是 NULL）
 */
Node *
makeBoolConst(bool value, bool isnull)
{
	/* 注意 pg_type.h 把 bool 的大小写死为 1……这里照抄 */
	return (Node *) makeConst(BOOLOID, -1, InvalidOid, 1,
							  BoolGetDatum(value), isnull, true);
}

/*
 * makeBoolExpr -
 *	  创建一个 BoolExpr 节点
 */
Expr *
makeBoolExpr(BoolExprType boolop, List *args, int location)
{
	BoolExpr   *b = makeNode(BoolExpr);

	b->boolop = boolop;
	b->args = args;
	b->location = location;

	return (Expr *) b;
}

/*
 * makeAlias -
 *	  创建一个 Alias 节点
 *
 * 注意：给定的名字会被复制，但 colnames 列表（若有）不会。
 */
Alias *
makeAlias(const char *aliasname, List *colnames)
{
	Alias	   *a = makeNode(Alias);

	a->aliasname = pstrdup(aliasname);
	a->colnames = colnames;

	return a;
}

/*
 * makeRelabelType -
 *	  创建一个 RelabelType 节点
 */
RelabelType *
makeRelabelType(Expr *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				CoercionForm rformat)
{
	RelabelType *r = makeNode(RelabelType);

	r->arg = arg;
	r->resulttype = rtype;
	r->resulttypmod = rtypmod;
	r->resultcollid = rcollid;
	r->relabelformat = rformat;
	r->location = -1;

	return r;
}

/*
 * makeRangeVar -
 *	  创建一个 RangeVar 节点（相当简化的情形）
 */
RangeVar *
makeRangeVar(char *schemaname, char *relname, int location)
{
	RangeVar   *r = makeNode(RangeVar);

	r->catalogname = NULL;
	r->schemaname = schemaname;
	r->relname = relname;
	r->inh = true;
	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->alias = NULL;
	r->location = location;

	return r;
}

/*
 * makeTypeName -
 *	为一个未限定的名字构建 TypeName 节点。
 *
 * typmod 取默认值，但调用方以后可以改。
 */
TypeName *
makeTypeName(char *typnam)
{
	return makeTypeNameFromNameList(list_make1(makeString(typnam)));
}

/*
 * makeTypeNameFromNameList -
 *	为一个表示限定名的 String 列表构建 TypeName 节点。
 *
 * typmod 取默认值，但调用方以后可以改。
 */
TypeName *
makeTypeNameFromNameList(List *names)
{
	TypeName   *n = makeNode(TypeName);

	n->names = names;
	n->typmods = NIL;
	n->typemod = -1;
	n->location = -1;
	return n;
}

/*
 * makeTypeNameFromOid -
 *	构建 TypeName 节点，表示一个已由 OID/typmod 知晓的类型。
 */
TypeName *
makeTypeNameFromOid(Oid typeOid, int32 typmod)
{
	TypeName   *n = makeNode(TypeName);

	n->typeOid = typeOid;
	n->typemod = typmod;
	n->location = -1;
	return n;
}

/*
 * makeColumnDef -
 *	构建表示简单列定义的 ColumnDef 节点。
 *
 * 类型和排序规则由 OID 指定。
 * 其他属性一开始都是基本值。
 */
ColumnDef *
makeColumnDef(const char *colname, Oid typeOid, int32 typmod, Oid collOid)
{
	ColumnDef  *n = makeNode(ColumnDef);

	n->colname = pstrdup(colname);
	n->typeName = makeTypeNameFromOid(typeOid, typmod);
	n->inhcount = 0;
	n->is_local = true;
	n->is_not_null = false;
	n->is_from_type = false;
	n->storage = 0;
	n->raw_default = NULL;
	n->cooked_default = NULL;
	n->collClause = NULL;
	n->collOid = collOid;
	n->constraints = NIL;
	n->fdwoptions = NIL;
	n->location = -1;

	return n;
}

/*
 * makeFuncExpr -
 *	构建表示函数调用的表达式树。
 *
 * 参数表达式必须已经变换过。
 */
FuncExpr *
makeFuncExpr(Oid funcid, Oid rettype, List *args,
			 Oid funccollid, Oid inputcollid, CoercionForm fformat)
{
	FuncExpr   *funcexpr;

	funcexpr = makeNode(FuncExpr);
	funcexpr->funcid = funcid;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcretset = false;	/* 这里只允许这种情况 */
	funcexpr->funcvariadic = false; /* 这里只允许这种情况 */
	funcexpr->funcformat = fformat;
	funcexpr->funccollid = funccollid;
	funcexpr->inputcollid = inputcollid;
	funcexpr->args = args;
	funcexpr->location = -1;

	return funcexpr;
}

/*
 * makeDefElem -
 *	构建一个 DefElem 节点
 *
 * 这对未限定选项名且没有特殊动作的 "典型" 情形
 * 已经够用。
 */
DefElem *
makeDefElem(char *name, Node *arg, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = NULL;
	res->defname = name;
	res->arg = arg;
	res->defaction = DEFELEM_UNSPEC;
	res->location = location;

	return res;
}

/*
 * makeDefElemExtended -
 *	构建一个所有字段都可指定的 DefElem 节点
 */
DefElem *
makeDefElemExtended(char *nameSpace, char *name, Node *arg,
					DefElemAction defaction, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = nameSpace;
	res->defname = name;
	res->arg = arg;
	res->defaction = defaction;
	res->location = location;

	return res;
}

/*
 * makeFuncCall -
 *
 * 用每个调用方都必须提供的信息初始化 FuncCall 结构体。
 * 任何非默认参数都得由调用方填入。
 */
FuncCall *
makeFuncCall(List *name, List *args, CoercionForm funcformat, int location)
{
	FuncCall   *n = makeNode(FuncCall);

	n->funcname = name;
	n->args = args;
	n->agg_order = NIL;
	n->agg_filter = NULL;
	n->over = NULL;
	n->agg_within_group = false;
	n->agg_star = false;
	n->agg_distinct = false;
	n->func_variadic = false;
	n->funcformat = funcformat;
	n->location = location;
	return n;
}

/*
 * make_opclause
 *	  根据操作符信息、左操作数
 *	  和右操作数（传入 NULL 以创建单操作数子句）
 *	  以及排序规则信息，创建操作符子句。
 */
Expr *
make_opclause(Oid opno, Oid opresulttype, bool opretset,
			  Expr *leftop, Expr *rightop,
			  Oid opcollid, Oid inputcollid)
{
	OpExpr	   *expr = makeNode(OpExpr);

	expr->opno = opno;
	expr->opfuncid = InvalidOid;
	expr->opresulttype = opresulttype;
	expr->opretset = opretset;
	expr->opcollid = opcollid;
	expr->inputcollid = inputcollid;
	if (rightop)
		expr->args = list_make2(leftop, rightop);
	else
		expr->args = list_make1(leftop);
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_andclause
 *
 * 根据子子句列表创建一个 'and' 子句。
 */
Expr *
make_andclause(List *andclauses)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = AND_EXPR;
	expr->args = andclauses;
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_orclause
 *
 * 根据子子句列表创建一个 'or' 子句。
 */
Expr *
make_orclause(List *orclauses)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = OR_EXPR;
	expr->args = orclauses;
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_notclause
 *
 * 根据要否定的表达式创建一个 'not' 子句。
 */
Expr *
make_notclause(Expr *notclause)
{
	BoolExpr   *expr = makeNode(BoolExpr);

	expr->boolop = NOT_EXPR;
	expr->args = list_make1(notclause);
	expr->location = -1;
	return (Expr *) expr;
}

/*
 * make_and_qual
 *
 * make_andclause 的变体，用于把两个限定条件 ANDing 在一起。
 * 限定条件有这样的性质：NULL 节点树被解释
 * 为 'true'。
 *
 * 注意：这里并不试图保持 AND/OR 的平坦性；因此不应当
 * 用于已经过 prepqual.c 处理的限定条件。
 */
Node *
make_and_qual(Node *qual1, Node *qual2)
{
	if (qual1 == NULL)
		return qual2;
	if (qual2 == NULL)
		return qual1;
	return (Node *) make_andclause(list_make2(qual1, qual2));
}

/*
 * 规划器和执行器通常把限定表达式
 * 表示为带隐式 AND 语义的布尔表达式列表。
 *
 * 这些函数在 AND 语义的表达式列表与
 * 布尔表达式的普通表示之间转换。
 *
 * 注意空列表被视为等价于 TRUE。
 */
Expr *
make_ands_explicit(List *andclauses)
{
	if (andclauses == NIL)
		return (Expr *) makeBoolConst(true, false);
	else if (list_length(andclauses) == 1)
		return (Expr *) linitial(andclauses);
	else
		return make_andclause(andclauses);
}

List *
make_ands_implicit(Expr *clause)
{
	/*
	 * 注意：因为解析器在没有 WHERE 子句的查询里把 qual 字段设为 NULL，
	 * 我们必须把 NULL 输入子句视为 TRUE，即便
	 * 有人更合理地认为它是 FALSE。
	 */
	if (clause == NULL)
		return NIL;				/* NULL -> NIL 列表 == TRUE */
	else if (is_andclause(clause))
		return ((BoolExpr *) clause)->args;
	else if (IsA(clause, Const) &&
			 !((Const *) clause)->constisnull &&
			 DatumGetBool(((Const *) clause)->constvalue))
		return NIL;				/* 常量 TRUE 输入 -> NIL 列表 */
	else
		return list_make1(clause);
}

/*
 * makeIndexInfo
 *	  创建一个 IndexInfo 节点
 */
IndexInfo *
makeIndexInfo(int numattrs, int numkeyattrs, Oid amoid, List *expressions,
			  List *predicates, bool unique, bool nulls_not_distinct,
			  bool isready, bool concurrent, bool summarizing)
{
	IndexInfo  *n = makeNode(IndexInfo);

	n->ii_NumIndexAttrs = numattrs;
	n->ii_NumIndexKeyAttrs = numkeyattrs;
	Assert(n->ii_NumIndexKeyAttrs != 0);
	Assert(n->ii_NumIndexKeyAttrs <= n->ii_NumIndexAttrs);
	n->ii_Unique = unique;
	n->ii_NullsNotDistinct = nulls_not_distinct;
	n->ii_ReadyForInserts = isready;
	n->ii_CheckedUnchanged = false;
	n->ii_IndexUnchanged = false;
	n->ii_Concurrent = concurrent;
	n->ii_Summarizing = summarizing;

	/* 汇总索引无法包含非键属性 */
	Assert(!summarizing || (numkeyattrs == numattrs));

	/* 表达式 */
	n->ii_Expressions = expressions;
	n->ii_ExpressionsState = NIL;

	/* 谓词  */
	n->ii_Predicate = predicates;
	n->ii_PredicateState = NULL;

	/* 排除约束 */
	n->ii_ExclusionOps = NULL;
	n->ii_ExclusionProcs = NULL;
	n->ii_ExclusionStrats = NULL;

	/* 操作符类选项 */
	n->ii_OpclassOptions = NULL;

	/* 推测性插入 */
	n->ii_UniqueOps = NULL;
	n->ii_UniqueProcs = NULL;
	n->ii_UniqueStrats = NULL;

	/* 把索引构建状态初始化为默认值 */
	n->ii_BrokenHotChain = false;
	n->ii_ParallelWorkers = 0;

	/* 为索引访问方法可能的使用做准备 */
	n->ii_Am = amoid;
	n->ii_AmCache = NULL;
	n->ii_Context = CurrentMemoryContext;

	return n;
}

/*
 * makeGroupingSet
 *
 */
GroupingSet *
makeGroupingSet(GroupingSetKind kind, List *content, int location)
{
	GroupingSet *n = makeNode(GroupingSet);

	n->kind = kind;
	n->content = content;
	n->location = location;
	return n;
}

/*
 * makeVacuumRelation -
 *	  创建一个 VacuumRelation 节点
 */
VacuumRelation *
makeVacuumRelation(RangeVar *relation, Oid oid, List *va_cols)
{
	VacuumRelation *v = makeNode(VacuumRelation);

	v->relation = relation;
	v->oid = oid;
	v->va_cols = va_cols;
	return v;
}

/*
 * makeJsonFormat -
 *	  创建一个 JsonFormat 节点
 */
JsonFormat *
makeJsonFormat(JsonFormatType type, JsonEncoding encoding, int location)
{
	JsonFormat *jf = makeNode(JsonFormat);

	jf->format_type = type;
	jf->encoding = encoding;
	jf->location = location;

	return jf;
}

/*
 * makeJsonValueExpr -
 *	  创建一个 JsonValueExpr 节点
 */
JsonValueExpr *
makeJsonValueExpr(Expr *raw_expr, Expr *formatted_expr,
				  JsonFormat *format)
{
	JsonValueExpr *jve = makeNode(JsonValueExpr);

	jve->raw_expr = raw_expr;
	jve->formatted_expr = formatted_expr;
	jve->format = format;

	return jve;
}

/*
 * makeJsonEncoding -
 *	  把 JSON 编码名转换为枚举 JsonEncoding
 */
JsonEncoding
makeJsonEncoding(char *name)
{
	if (!pg_strcasecmp(name, "utf8"))
		return JS_ENC_UTF8;
	if (!pg_strcasecmp(name, "utf16"))
		return JS_ENC_UTF16;
	if (!pg_strcasecmp(name, "utf32"))
		return JS_ENC_UTF32;

	ereport(ERROR,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("unrecognized JSON encoding: %s", name));

	return JS_ENC_DEFAULT;
}

/*
 * makeJsonKeyValue -
 *	  创建一个 JsonKeyValue 节点
 */
Node *
makeJsonKeyValue(Node *key, Node *value)
{
	JsonKeyValue *n = makeNode(JsonKeyValue);

	n->key = (Expr *) key;
	n->value = castNode(JsonValueExpr, value);

	return (Node *) n;
}

/*
 * makeJsonIsPredicate -
 *	  创建一个 JsonIsPredicate 节点
 */
Node *
makeJsonIsPredicate(Node *expr, JsonFormat *format, JsonValueType item_type,
					bool unique_keys, int location)
{
	JsonIsPredicate *n = makeNode(JsonIsPredicate);

	n->expr = expr;
	n->format = format;
	n->item_type = item_type;
	n->unique_keys = unique_keys;
	n->location = location;

	return (Node *) n;
}
