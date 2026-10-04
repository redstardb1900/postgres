/*-------------------------------------------------------------------------
 *
 * nodeFuncs.c
 *		Various general-purpose manipulations of Node trees
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/nodeFuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static bool expression_returns_set_walker(Node *node, void *context);
static int	leftmostLoc(int loc1, int loc2);
static bool fix_opfuncids_walker(Node *node, void *context);
static bool planstate_walk_subplans(List *plans,
									planstate_tree_walker_callback walker,
									void *context);
static bool planstate_walk_members(PlanState **planstates, int nplans,
								   planstate_tree_walker_callback walker,
								   void *context);


/*
 *	exprType -
 *	  返回表达式结果类型的 Oid。
 */
Oid
exprType(const Node *expr)
{
	Oid			type;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			type = ((const Var *) expr)->vartype;
			break;
		case T_Const:
			type = ((const Const *) expr)->consttype;
			break;
		case T_Param:
			type = ((const Param *) expr)->paramtype;
			break;
		case T_Aggref:
			type = ((const Aggref *) expr)->aggtype;
			break;
		case T_GroupingFunc:
			type = INT4OID;
			break;
		case T_WindowFunc:
			type = ((const WindowFunc *) expr)->wintype;
			break;
		case T_SubscriptingRef:
			type = ((const SubscriptingRef *) expr)->refrestype;
			break;
		case T_FuncExpr:
			type = ((const FuncExpr *) expr)->funcresulttype;
			break;
		case T_NamedArgExpr:
			type = exprType((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			type = ((const OpExpr *) expr)->opresulttype;
			break;
		case T_DistinctExpr:
			type = ((const DistinctExpr *) expr)->opresulttype;
			break;
		case T_NullIfExpr:
			type = ((const NullIfExpr *) expr)->opresulttype;
			break;
		case T_ScalarArrayOpExpr:
			type = BOOLOID;
			break;
		case T_BoolExpr:
			type = BOOLOID;
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的类型 */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					type = exprType((Node *) tent->expr);
					if (sublink->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(exprType((Node *) tent->expr)))));
					}
				}
				else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR 总是被视为返回 RECORD */
					type = RECORDOID;
				}
				else
				{
					/* 对所有其他子链接类型，结果是布尔值 */
					type = BOOLOID;
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的类型 */
					type = subplan->firstColType;
					if (subplan->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(subplan->firstColType))));
					}
				}
				else if (subplan->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR 总是被视为返回 RECORD */
					type = RECORDOID;
				}
				else
				{
					/* 对所有其他子计划类型，结果是布尔值 */
					type = BOOLOID;
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* 各子计划应当返回相同的东西 */
				type = exprType((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			type = ((const FieldSelect *) expr)->resulttype;
			break;
		case T_FieldStore:
			type = ((const FieldStore *) expr)->resulttype;
			break;
		case T_RelabelType:
			type = ((const RelabelType *) expr)->resulttype;
			break;
		case T_CoerceViaIO:
			type = ((const CoerceViaIO *) expr)->resulttype;
			break;
		case T_ArrayCoerceExpr:
			type = ((const ArrayCoerceExpr *) expr)->resulttype;
			break;
		case T_ConvertRowtypeExpr:
			type = ((const ConvertRowtypeExpr *) expr)->resulttype;
			break;
		case T_CollateExpr:
			type = exprType((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			type = ((const CaseExpr *) expr)->casetype;
			break;
		case T_CaseTestExpr:
			type = ((const CaseTestExpr *) expr)->typeId;
			break;
		case T_ArrayExpr:
			type = ((const ArrayExpr *) expr)->array_typeid;
			break;
		case T_RowExpr:
			type = ((const RowExpr *) expr)->row_typeid;
			break;
		case T_RowCompareExpr:
			type = BOOLOID;
			break;
		case T_CoalesceExpr:
			type = ((const CoalesceExpr *) expr)->coalescetype;
			break;
		case T_MinMaxExpr:
			type = ((const MinMaxExpr *) expr)->minmaxtype;
			break;
		case T_SQLValueFunction:
			type = ((const SQLValueFunction *) expr)->type;
			break;
		case T_XmlExpr:
			if (((const XmlExpr *) expr)->op == IS_DOCUMENT)
				type = BOOLOID;
			else if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				type = TEXTOID;
			else
				type = XMLOID;
			break;
		case T_JsonValueExpr:
			{
				const JsonValueExpr *jve = (const JsonValueExpr *) expr;

				type = exprType((Node *) jve->formatted_expr);
			}
			break;
		case T_JsonConstructorExpr:
			type = ((const JsonConstructorExpr *) expr)->returning->typid;
			break;
		case T_JsonIsPredicate:
			type = BOOLOID;
			break;
		case T_NullTest:
			type = BOOLOID;
			break;
		case T_BooleanTest:
			type = BOOLOID;
			break;
		case T_CoerceToDomain:
			type = ((const CoerceToDomain *) expr)->resulttype;
			break;
		case T_CoerceToDomainValue:
			type = ((const CoerceToDomainValue *) expr)->typeId;
			break;
		case T_SetToDefault:
			type = ((const SetToDefault *) expr)->typeId;
			break;
		case T_CurrentOfExpr:
			type = BOOLOID;
			break;
		case T_NextValueExpr:
			type = ((const NextValueExpr *) expr)->typeId;
			break;
		case T_InferenceElem:
			{
				const InferenceElem *n = (const InferenceElem *) expr;

				type = exprType((Node *) n->expr);
			}
			break;
		case T_PlaceHolderVar:
			type = exprType((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			type = InvalidOid;	/* 消除编译器告警 */
			break;
	}
	return type;
}

/*
 *	exprTypmod -
 *	  返回表达式结果类型的类型特定修饰符，
 *	  若能确定的话。很多情况下无法确定，我们就返回 -1。
 */
int32
exprTypmod(const Node *expr)
{
	if (!expr)
		return -1;

	switch (nodeTag(expr))
	{
		case T_Var:
			return ((const Var *) expr)->vartypmod;
		case T_Const:
			return ((const Const *) expr)->consttypmod;
		case T_Param:
			return ((const Param *) expr)->paramtypmod;
		case T_SubscriptingRef:
			return ((const SubscriptingRef *) expr)->reftypmod;
		case T_FuncExpr:
			{
				int32		coercedTypmod;

				/* 对长度强制转换函数要聪明一点…… */
				if (exprIsLengthCoercion(expr, &coercedTypmod))
					return coercedTypmod;
			}
			break;
		case T_NamedArgExpr:
			return exprTypmod((Node *) ((const NamedArgExpr *) expr)->arg);
		case T_NullIfExpr:
			{
				/*
				 * 结果要么是第一个参数，要么是 NULL，因此若已知，我们可以报告
				 * 第一个参数的 typmod。
				 */
				const NullIfExpr *nexpr = (const NullIfExpr *) expr;

				return exprTypmod((Node *) linitial(nexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的 typmod */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					return exprTypmod((Node *) tent->expr);
					/* 注意若它是数组，我们不必关心 */
				}
				/* 否则结果是 RECORD 或 BOOLEAN，typmod 为 -1 */
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的 typmod */
					/* 注意若它是数组，我们不必关心 */
					return subplan->firstColTypmod;
				}
				/* 否则结果是 RECORD 或 BOOLEAN，typmod 为 -1 */
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* 各子计划应当返回相同的东西 */
				return exprTypmod((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			return ((const FieldSelect *) expr)->resulttypmod;
		case T_RelabelType:
			return ((const RelabelType *) expr)->resulttypmod;
		case T_ArrayCoerceExpr:
			return ((const ArrayCoerceExpr *) expr)->resulttypmod;
		case T_CollateExpr:
			return exprTypmod((Node *) ((const CollateExpr *) expr)->arg);
		case T_CaseExpr:
			{
				/*
				 * 若所有备选在类型/typmod 上一致，就返回该
				 * typmod，否则用 -1
				 */
				const CaseExpr *cexpr = (const CaseExpr *) expr;
				Oid			casetype = cexpr->casetype;
				int32		typmod;
				ListCell   *arg;

				if (!cexpr->defresult)
					return -1;
				if (exprType((Node *) cexpr->defresult) != casetype)
					return -1;
				typmod = exprTypmod((Node *) cexpr->defresult);
				if (typmod < 0)
					return -1;	/* 没必要再费力 */
				foreach(arg, cexpr->args)
				{
					CaseWhen   *w = lfirst_node(CaseWhen, arg);

					if (exprType((Node *) w->result) != casetype)
						return -1;
					if (exprTypmod((Node *) w->result) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CaseTestExpr:
			return ((const CaseTestExpr *) expr)->typeMod;
		case T_ArrayExpr:
			{
				/*
				 * 若所有元素在类型/typmod 上一致，就返回该
				 * typmod，否则用 -1
				 */
				const ArrayExpr *arrayexpr = (const ArrayExpr *) expr;
				Oid			commontype;
				int32		typmod;
				ListCell   *elem;

				if (arrayexpr->elements == NIL)
					return -1;
				typmod = exprTypmod((Node *) linitial(arrayexpr->elements));
				if (typmod < 0)
					return -1;	/* 没必要再费力 */
				if (arrayexpr->multidims)
					commontype = arrayexpr->array_typeid;
				else
					commontype = arrayexpr->element_typeid;
				foreach(elem, arrayexpr->elements)
				{
					Node	   *e = (Node *) lfirst(elem);

					if (exprType(e) != commontype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CoalesceExpr:
			{
				/*
				 * 若所有备选在类型/typmod 上一致，就返回该
				 * typmod，否则用 -1
				 */
				const CoalesceExpr *cexpr = (const CoalesceExpr *) expr;
				Oid			coalescetype = cexpr->coalescetype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(cexpr->args)) != coalescetype)
					return -1;
				typmod = exprTypmod((Node *) linitial(cexpr->args));
				if (typmod < 0)
					return -1;	/* 没必要再费力 */
				for_each_from(arg, cexpr->args, 1)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != coalescetype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_MinMaxExpr:
			{
				/*
				 * 若所有备选在类型/typmod 上一致，就返回该
				 * typmod，否则用 -1
				 */
				const MinMaxExpr *mexpr = (const MinMaxExpr *) expr;
				Oid			minmaxtype = mexpr->minmaxtype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(mexpr->args)) != minmaxtype)
					return -1;
				typmod = exprTypmod((Node *) linitial(mexpr->args));
				if (typmod < 0)
					return -1;	/* 没必要再费力 */
				for_each_from(arg, mexpr->args, 1)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != minmaxtype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_SQLValueFunction:
			return ((const SQLValueFunction *) expr)->typmod;
		case T_JsonValueExpr:
			return exprTypmod((Node *) ((const JsonValueExpr *) expr)->formatted_expr);
		case T_JsonConstructorExpr:
			return ((const JsonConstructorExpr *) expr)->returning->typmod;
		case T_CoerceToDomain:
			return ((const CoerceToDomain *) expr)->resulttypmod;
		case T_CoerceToDomainValue:
			return ((const CoerceToDomainValue *) expr)->typeMod;
		case T_SetToDefault:
			return ((const SetToDefault *) expr)->typeMod;
		case T_PlaceHolderVar:
			return exprTypmod((Node *) ((const PlaceHolderVar *) expr)->phexpr);
		default:
			break;
	}
	return -1;
}

/*
 * exprIsLengthCoercion
 *		检测表达式树是否是某数据类型的
 *		typmod 强制转换函数的应用。可选地取出结果的 typmod。
 *
 * 若 coercedTypmod 不是 NULL，且表达式
 * 是长度强制转换函数，就把 typmod 存在那里，否则存 -1。
 *
 * 注意组合的类型加长度强制转换会被本例程
 * 当作长度强制转换。
 */
bool
exprIsLengthCoercion(const Node *expr, int32 *coercedTypmod)
{
	if (coercedTypmod != NULL)
		*coercedTypmod = -1;	/* 失败时的默认结果 */

	/*
	 * 标量类型的长度强制转换是 FuncExprs，数组类型的长度强制转换
	 * 是 ArrayCoerceExprs
	 */
	if (expr && IsA(expr, FuncExpr))
	{
		const FuncExpr *func = (const FuncExpr *) expr;
		int			nargs;
		Const	   *second_arg;

		/*
		 * 若它不是来自强制转换上下文，就拒绝。
		 */
		if (func->funcformat != COERCE_EXPLICIT_CAST &&
			func->funcformat != COERCE_IMPLICIT_CAST)
			return false;

		/*
		 * 若它不是二参数或三参数函数，且
		 * 第二个参数不是 int4 常量，它就无法由
		 * 长度强制转换创建（相反，它必须是类型强制转换）。
		 */
		nargs = list_length(func->args);
		if (nargs < 2 || nargs > 3)
			return false;

		second_arg = (Const *) lsecond(func->args);
		if (!IsA(second_arg, Const) ||
			second_arg->consttype != INT4OID ||
			second_arg->constisnull)
			return false;

		/*
		 * 好，它确实是长度强制转换函数。
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = DatumGetInt32(second_arg->constvalue);

		return true;
	}

	if (expr && IsA(expr, ArrayCoerceExpr))
	{
		const ArrayCoerceExpr *acoerce = (const ArrayCoerceExpr *) expr;

		/* 除非有非默认的 typmod，否则它不是长度强制转换 */
		if (acoerce->resulttypmod < 0)
			return false;

		/*
		 * 好，它确实是长度强制转换表达式。
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = acoerce->resulttypmod;

		return true;
	}

	return false;
}

/*
 * applyRelabelType
 *		必要时加上 RelabelType 节点，使表达式暴露
 *		指定的类型、typmod 和排序规则。
 *
 * 这主要用于规划期间。因此，它必须
 * 维持 eval_const_expressions 之后的不变量：没有相邻的
 * RelabelTypes，且树已完全常量折叠（因此，
 * 我们不得在 Const 之上返回 RelabelType）。若确实找到 Const，
 * 当 "overwrite_ok" 为 true 时就地修改它；仅当
 * 调用方知道该 Const 是新生成的时，才应传入 true。
 */
Node *
applyRelabelType(Node *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				 CoercionForm rformat, int rlocation, bool overwrite_ok)
{
	/*
	 * 若发现叠在一起的 RelabelTypes（例如 foo::int::oid），可以丢掉
	 * 除最上面一个以外的全部，而且必须这么做，以确保语义上
	 * 等价的表达式 equal()。
	 */
	while (arg && IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;

	if (arg && IsA(arg, Const))
	{
		/* 直接修改 Const，以保持常量平坦性。 */
		Const	   *con = (Const *) arg;

		if (!overwrite_ok)
			con = copyObject(con);
		con->consttype = rtype;
		con->consttypmod = rtypmod;
		con->constcollid = rcollid;
		/* 我们保留 Const 原来的位置。 */
		return (Node *) con;
	}
	else if (exprType(arg) == rtype &&
			 exprTypmod(arg) == rtypmod &&
			 exprCollation(arg) == rcollid)
	{
		/* 有时会发现一层层重标号互相抵消，净效果为零。 */
		return arg;
	}
	else
	{
		/* 不行，必须有一个 RelabelType。 */
		RelabelType *newrelabel = makeNode(RelabelType);

		newrelabel->arg = (Expr *) arg;
		newrelabel->resulttype = rtype;
		newrelabel->resulttypmod = rtypmod;
		newrelabel->resultcollid = rcollid;
		newrelabel->relabelformat = rformat;
		newrelabel->location = rlocation;
		return (Node *) newrelabel;
	}
}

/*
 * relabel_to_typmod
 *		加上一个只改变表达式 typmod 的 RelabelType 节点。
 *
 * 这是 applyRelabelType 常见用法的便捷函数。
 */
Node *
relabel_to_typmod(Node *expr, int32 typmod)
{
	return applyRelabelType(expr, exprType(expr), typmod, exprCollation(expr),
							COERCE_EXPLICIT_CAST, -1, false);
}

/*
 * strip_implicit_coercions：去掉树顶层的隐式强制转换
 *
 * 这不修改也不复制输入表达式树，只返回
 * 指向其中合适位置的指针。
 *
 * 注意：对 RowExpr 这里没有什么有用的事可做，因此
 * 即使它被标成隐式强制转换，也原样返回。
 */
Node *
strip_implicit_coercions(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;

		if (f->funcformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions(linitial(f->args));
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;

		if (r->relabelformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) r->arg);
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *c = (ArrayCoerceExpr *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *c = (ConvertRowtypeExpr *) node;

		if (c->convertformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *c = (CoerceToDomain *) node;

		if (c->coercionformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	return node;
}

/*
 * expression_returns_set
 *	  测试表达式是否返回集合结果。
 *
 * 因为我们使用 expression_tree_walker()，这也可以用于
 * 整个目标列表；若任一目标列表项
 * 返回集合，结果就是 true。
 */
bool
expression_returns_set(Node *clause)
{
	return expression_returns_set_walker(clause, NULL);
}

static bool
expression_returns_set_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (expr->funcretset)
			return true;
		/* else fall through to check args */
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		if (expr->opretset)
			return true;
		/* else fall through to check args */
	}

	/*
	 * 若你再增加任何返回集合的情形，也要修改
	 * clauses.c 中的 expression_returns_set_rows() 和 tlist.c 中的
	 * IS_SRF_CALL()。
	 */

	/* 对解析器已检查不会返回集合的某些情形，避免递归 */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, GroupingFunc))
		return false;
	if (IsA(node, WindowFunc))
		return false;

	return expression_tree_walker(node, expression_returns_set_walker,
								  context);
}


/*
 *	exprCollation -
 *	  返回表达式结果的排序规则 Oid。
 *
 * 注意：通常会调用函数的表达式节点都有
 * "inputcollid" 字段，函数应把它用作排序规则。
 * 那是该节点各输入已解析的公共排序规则。它常常
 * 与结果排序规则相同，但并不总是；特别是，若函数
 * 从可排序的输入产生不可排序的结果类型，
 * 或反过来，这两者就不同。
 */
Oid
exprCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			coll = ((const Var *) expr)->varcollid;
			break;
		case T_Const:
			coll = ((const Const *) expr)->constcollid;
			break;
		case T_Param:
			coll = ((const Param *) expr)->paramcollid;
			break;
		case T_Aggref:
			coll = ((const Aggref *) expr)->aggcollid;
			break;
		case T_GroupingFunc:
			coll = InvalidOid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->wincollid;
			break;
		case T_SubscriptingRef:
			coll = ((const SubscriptingRef *) expr)->refcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->funccollid;
			break;
		case T_NamedArgExpr:
			coll = exprCollation((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->opcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->opcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->opcollid;
			break;
		case T_ScalarArrayOpExpr:
			/* ScalarArrayOpExpr 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_BoolExpr:
			/* BoolExpr 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的排序规则 */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					coll = exprCollation((Node *) tent->expr);
					/* 若被转换成数组，排序规则不变 */
				}
				else
				{
					/* 否则 SubLink 的结果是 RECORD 或 BOOLEAN */
					coll = InvalidOid;	/* ... 因此它没有排序规则 */
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的排序规则 */
					coll = subplan->firstColCollation;
					/* 若被转换成数组，排序规则不变 */
				}
				else
				{
					/* 否则 SubPlan 的结果是 RECORD 或 BOOLEAN */
					coll = InvalidOid;	/* ... 因此它没有排序规则 */
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* 各子计划应当返回相同的东西 */
				coll = exprCollation((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			coll = ((const FieldSelect *) expr)->resultcollid;
			break;
		case T_FieldStore:
			/* FieldStore 的结果是复合类型…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_RelabelType:
			coll = ((const RelabelType *) expr)->resultcollid;
			break;
		case T_CoerceViaIO:
			coll = ((const CoerceViaIO *) expr)->resultcollid;
			break;
		case T_ArrayCoerceExpr:
			coll = ((const ArrayCoerceExpr *) expr)->resultcollid;
			break;
		case T_ConvertRowtypeExpr:
			/* ConvertRowtypeExpr 的结果是复合类型…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_CollateExpr:
			coll = ((const CollateExpr *) expr)->collOid;
			break;
		case T_CaseExpr:
			coll = ((const CaseExpr *) expr)->casecollid;
			break;
		case T_CaseTestExpr:
			coll = ((const CaseTestExpr *) expr)->collation;
			break;
		case T_ArrayExpr:
			coll = ((const ArrayExpr *) expr)->array_collid;
			break;
		case T_RowExpr:
			/* RowExpr 的结果是复合类型…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_RowCompareExpr:
			/* RowCompareExpr 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_CoalesceExpr:
			coll = ((const CoalesceExpr *) expr)->coalescecollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->minmaxcollid;
			break;
		case T_SQLValueFunction:
			/* 返回 NAME，或一种不可排序的类型 */
			if (((const SQLValueFunction *) expr)->type == NAMEOID)
				coll = C_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_XmlExpr:

			/*
			 * XMLSERIALIZE 从不可排序的输入返回 text，因此它的
			 * 排序规则总是默认值。其他情形返回布尔或
			 * XML，它们不可排序。
			 */
			if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				coll = DEFAULT_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_JsonValueExpr:
			coll = exprCollation((Node *) ((const JsonValueExpr *) expr)->formatted_expr);
			break;
		case T_JsonConstructorExpr:
			{
				const JsonConstructorExpr *ctor = (const JsonConstructorExpr *) expr;

				if (ctor->coercion)
					coll = exprCollation((Node *) ctor->coercion);
				else
					coll = InvalidOid;
			}
			break;
		case T_JsonIsPredicate:
			/* IS JSON 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_NullTest:
			/* NullTest 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_BooleanTest:
			/* BooleanTest 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_CoerceToDomain:
			coll = ((const CoerceToDomain *) expr)->resultcollid;
			break;
		case T_CoerceToDomainValue:
			coll = ((const CoerceToDomainValue *) expr)->collation;
			break;
		case T_SetToDefault:
			coll = ((const SetToDefault *) expr)->collation;
			break;
		case T_CurrentOfExpr:
			/* CurrentOfExpr 的结果是布尔值…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_NextValueExpr:
			/* NextValueExpr 的结果是整数类型…… */
			coll = InvalidOid;	/* ... 因此它没有排序规则 */
			break;
		case T_InferenceElem:
			coll = exprCollation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PlaceHolderVar:
			coll = exprCollation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			coll = InvalidOid;	/* 消除编译器告警 */
			break;
	}
	return coll;
}

/*
 *	exprInputCollation -
 *	  返回函数应当使用的排序规则 Oid（若有）。
 *
 * 若节点类型不存放该信息，结果是 InvalidOid。
 */
Oid
exprInputCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Aggref:
			coll = ((const Aggref *) expr)->inputcollid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->inputcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->inputcollid;
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->inputcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->inputcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->inputcollid;
			break;
		case T_ScalarArrayOpExpr:
			coll = ((const ScalarArrayOpExpr *) expr)->inputcollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->inputcollid;
			break;
		default:
			coll = InvalidOid;
			break;
	}
	return coll;
}

/*
 *	exprSetCollation -
 *	  给表达式树节点指定排序规则信息。
 *
 * 注意：这只在语法分析期间使用，因此不必
 * 担心子计划或 PlaceHolderVars。
 */
void
exprSetCollation(Node *expr, Oid collation)
{
	switch (nodeTag(expr))
	{
		case T_Var:
			((Var *) expr)->varcollid = collation;
			break;
		case T_Const:
			((Const *) expr)->constcollid = collation;
			break;
		case T_Param:
			((Param *) expr)->paramcollid = collation;
			break;
		case T_Aggref:
			((Aggref *) expr)->aggcollid = collation;
			break;
		case T_GroupingFunc:
			Assert(!OidIsValid(collation));
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->wincollid = collation;
			break;
		case T_SubscriptingRef:
			((SubscriptingRef *) expr)->refcollid = collation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->funccollid = collation;
			break;
		case T_NamedArgExpr:
			Assert(collation == exprCollation((Node *) ((NamedArgExpr *) expr)->arg));
			break;
		case T_OpExpr:
			((OpExpr *) expr)->opcollid = collation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->opcollid = collation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->opcollid = collation;
			break;
		case T_ScalarArrayOpExpr:
			/* ScalarArrayOpExpr 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_BoolExpr:
			/* BoolExpr 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_SubLink:
#ifdef USE_ASSERT_CHECKING
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* 取子查询第一个目标列的排序规则 */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot set collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					Assert(collation == exprCollation((Node *) tent->expr));
				}
				else
				{
					/* 否则结果是 RECORD 或 BOOLEAN */
					Assert(!OidIsValid(collation));
				}
			}
#endif							/* USE_ASSERT_CHECKING */
			break;
		case T_FieldSelect:
			((FieldSelect *) expr)->resultcollid = collation;
			break;
		case T_FieldStore:
			/* FieldStore 的结果是复合类型…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_RelabelType:
			((RelabelType *) expr)->resultcollid = collation;
			break;
		case T_CoerceViaIO:
			((CoerceViaIO *) expr)->resultcollid = collation;
			break;
		case T_ArrayCoerceExpr:
			((ArrayCoerceExpr *) expr)->resultcollid = collation;
			break;
		case T_ConvertRowtypeExpr:
			/* ConvertRowtypeExpr 的结果是复合类型…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_CaseExpr:
			((CaseExpr *) expr)->casecollid = collation;
			break;
		case T_ArrayExpr:
			((ArrayExpr *) expr)->array_collid = collation;
			break;
		case T_RowExpr:
			/* RowExpr 的结果是复合类型…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_RowCompareExpr:
			/* RowCompareExpr 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) expr)->coalescecollid = collation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->minmaxcollid = collation;
			break;
		case T_SQLValueFunction:
			Assert((((SQLValueFunction *) expr)->type == NAMEOID) ?
				   (collation == C_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_XmlExpr:
			Assert((((XmlExpr *) expr)->op == IS_XMLSERIALIZE) ?
				   (collation == DEFAULT_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_JsonValueExpr:
			exprSetCollation((Node *) ((JsonValueExpr *) expr)->formatted_expr,
							 collation);
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) expr;

				if (ctor->coercion)
					exprSetCollation((Node *) ctor->coercion, collation);
				else
					Assert(!OidIsValid(collation)); /* 结果总是一种
													 * json[b] 类型 */
			}
			break;
		case T_JsonIsPredicate:
			Assert(!OidIsValid(collation)); /* 结果总是布尔值 */
			break;
		case T_NullTest:
			/* NullTest 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_BooleanTest:
			/* BooleanTest 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_CoerceToDomain:
			((CoerceToDomain *) expr)->resultcollid = collation;
			break;
		case T_CoerceToDomainValue:
			((CoerceToDomainValue *) expr)->collation = collation;
			break;
		case T_SetToDefault:
			((SetToDefault *) expr)->collation = collation;
			break;
		case T_CurrentOfExpr:
			/* CurrentOfExpr 的结果是布尔值…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		case T_NextValueExpr:
			/* NextValueExpr 的结果是整数类型…… */
			Assert(!OidIsValid(collation)); /* ... 因此从不设置排序规则 */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			break;
	}
}

/*
 *	exprSetInputCollation -
 *	  给表达式树节点指定输入排序规则信息。
 *
 * 对不存放输入排序规则的节点类型，这是空操作。
 * 注意我们略过 RowCompareExpr，它需要特殊处理，因为它
 * 包含多个输入排序规则 OIDs。
 */
void
exprSetInputCollation(Node *expr, Oid inputcollation)
{
	switch (nodeTag(expr))
	{
		case T_Aggref:
			((Aggref *) expr)->inputcollid = inputcollation;
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->inputcollid = inputcollation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_OpExpr:
			((OpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_ScalarArrayOpExpr:
			((ScalarArrayOpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->inputcollid = inputcollation;
			break;
		default:
			break;
	}
}


/*
 *	exprLocation -
 *	  返回表达式树的解析位置，用于错误报告
 *
 * 若位置无法确定，则返回 -1。
 *
 * 对于大于单个词元的表达式，这里的意图是
 * 返回表达式最左词元的位置，不一定是
 * 最顶层 Node 的 location 字段。例如，OpExpr 的 location
 * 字段会指向操作符名，但若它不是前缀操作符，
 * 则应改为返回左操作数的位置。
 * 原因是我们想引用整个表达式，而不只是
 * 那个操作符，指向它的起点看来是最自然的方式。
 *
 * 这个位置并不完美——例如，语法并不
 * 在分析树里显式表示括号，对于写成 "(a + b) * c" 的内容，
 * 我们会指向 "a" 而不是 "("。
 * 但对错误报告来说应当足够好。
 *
 * 你也许觉得这段代码过于一般，比如为什么要检查
 * FuncExpr 节点的操作数，函数名不是应该
 * 在它们左边吗？有几个原因。语法
 * 有时构建的表达式并不完全是用户所写的；
 * 例如 x IS NOT BETWEEN ... 会变成一个 NOT 表达式，其关键字
 * 指针在最左参数的右边。另外，由语法分析
 * 隐式插入的节点（例如隐式强制转换的 FuncExprs）
 * location 为 -1，因此树中会出现已知位置与
 * 未知位置的奇怪组合。
 */
int
exprLocation(const Node *expr)
{
	int			loc;

	if (expr == NULL)
		return -1;
	switch (nodeTag(expr))
	{
		case T_RangeVar:
			loc = ((const RangeVar *) expr)->location;
			break;
		case T_TableFunc:
			loc = ((const TableFunc *) expr)->location;
			break;
		case T_Var:
			loc = ((const Var *) expr)->location;
			break;
		case T_Const:
			loc = ((const Const *) expr)->location;
			break;
		case T_Param:
			loc = ((const Param *) expr)->location;
			break;
		case T_Aggref:
			/* 函数名应当总是最先出现的东西 */
			loc = ((const Aggref *) expr)->location;
			break;
		case T_GroupingFunc:
			loc = ((const GroupingFunc *) expr)->location;
			break;
		case T_WindowFunc:
			/* 函数名应当总是最先出现的东西 */
			loc = ((const WindowFunc *) expr)->location;
			break;
		case T_SubscriptingRef:
			/* 只用容器参数的位置 */
			loc = exprLocation((Node *) ((const SubscriptingRef *) expr)->refexpr);
			break;
		case T_FuncExpr:
			{
				const FuncExpr *fexpr = (const FuncExpr *) expr;

				/* 同时考虑函数名和最左参数 */
				loc = leftmostLoc(fexpr->location,
								  exprLocation((Node *) fexpr->args));
			}
			break;
		case T_NamedArgExpr:
			{
				const NamedArgExpr *na = (const NamedArgExpr *) expr;

				/* 同时考虑参数名和值 */
				loc = leftmostLoc(na->location,
								  exprLocation((Node *) na->arg));
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* 结构上等价于 OpExpr */
		case T_NullIfExpr:		/* 结构上等价于 OpExpr */
			{
				const OpExpr *opexpr = (const OpExpr *) expr;

				/* 同时考虑操作符名和最左参数 */
				loc = leftmostLoc(opexpr->location,
								  exprLocation((Node *) opexpr->args));
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				const ScalarArrayOpExpr *saopexpr = (const ScalarArrayOpExpr *) expr;

				/* 同时考虑操作符名和最左参数 */
				loc = leftmostLoc(saopexpr->location,
								  exprLocation((Node *) saopexpr->args));
			}
			break;
		case T_BoolExpr:
			{
				const BoolExpr *bexpr = (const BoolExpr *) expr;

				/*
				 * 同上，用以处理 NOT 或 AND/OR。我们无法
				 * 把 NOT 特殊处理，因为
				 * IS NOT BETWEEN 之类的用法。
				 */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				/* 检查 testexpr（若有）以及操作符/关键字 */
				loc = leftmostLoc(exprLocation(sublink->testexpr),
								  sublink->location);
			}
			break;
		case T_FieldSelect:
			/* 只用参数的位置 */
			loc = exprLocation((Node *) ((const FieldSelect *) expr)->arg);
			break;
		case T_FieldStore:
			/* 只用参数的位置 */
			loc = exprLocation((Node *) ((const FieldStore *) expr)->arg);
			break;
		case T_RelabelType:
			{
				const RelabelType *rexpr = (const RelabelType *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(rexpr->location,
								  exprLocation((Node *) rexpr->arg));
			}
			break;
		case T_CoerceViaIO:
			{
				const CoerceViaIO *cexpr = (const CoerceViaIO *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ArrayCoerceExpr:
			{
				const ArrayCoerceExpr *cexpr = (const ArrayCoerceExpr *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				const ConvertRowtypeExpr *cexpr = (const ConvertRowtypeExpr *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CollateExpr:
			/* 只用参数的位置 */
			loc = exprLocation((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			/* CASE 关键字应当总是最先出现的东西 */
			loc = ((const CaseExpr *) expr)->location;
			break;
		case T_CaseWhen:
			/* WHEN 关键字应当总是最先出现的东西 */
			loc = ((const CaseWhen *) expr)->location;
			break;
		case T_ArrayExpr:
			/* 位置指向 ARRAY 或 [，它必须在最左边 */
			loc = ((const ArrayExpr *) expr)->location;
			break;
		case T_RowExpr:
			/* 位置指向 ROW 或 (，它必须在最左边 */
			loc = ((const RowExpr *) expr)->location;
			break;
		case T_RowCompareExpr:
			/* 只用最左参数的位置 */
			loc = exprLocation((Node *) ((const RowCompareExpr *) expr)->largs);
			break;
		case T_CoalesceExpr:
			/* COALESCE 关键字应当总是最先出现的东西 */
			loc = ((const CoalesceExpr *) expr)->location;
			break;
		case T_MinMaxExpr:
			/* GREATEST/LEAST 关键字应当总是最先出现的东西 */
			loc = ((const MinMaxExpr *) expr)->location;
			break;
		case T_SQLValueFunction:
			/* 函数关键字应当总是最先出现的东西 */
			loc = ((const SQLValueFunction *) expr)->location;
			break;
		case T_XmlExpr:
			{
				const XmlExpr *xexpr = (const XmlExpr *) expr;

				/* 同时考虑函数名和最左参数 */
				loc = leftmostLoc(xexpr->location,
								  exprLocation((Node *) xexpr->args));
			}
			break;
		case T_JsonFormat:
			loc = ((const JsonFormat *) expr)->location;
			break;
		case T_JsonValueExpr:
			loc = exprLocation((Node *) ((const JsonValueExpr *) expr)->raw_expr);
			break;
		case T_JsonConstructorExpr:
			loc = ((const JsonConstructorExpr *) expr)->location;
			break;
		case T_JsonIsPredicate:
			loc = ((const JsonIsPredicate *) expr)->location;
			break;
		case T_NullTest:
			{
				const NullTest *nexpr = (const NullTest *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(nexpr->location,
								  exprLocation((Node *) nexpr->arg));
			}
			break;
		case T_BooleanTest:
			{
				const BooleanTest *bexpr = (const BooleanTest *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->arg));
			}
			break;
		case T_CoerceToDomain:
			{
				const CoerceToDomain *cexpr = (const CoerceToDomain *) expr;

				/* 与上面大致相同 */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CoerceToDomainValue:
			loc = ((const CoerceToDomainValue *) expr)->location;
			break;
		case T_SetToDefault:
			loc = ((const SetToDefault *) expr)->location;
			break;
		case T_TargetEntry:
			/* 只用参数的位置 */
			loc = exprLocation((Node *) ((const TargetEntry *) expr)->expr);
			break;
		case T_IntoClause:
			/* 使用所含 RangeVar 的位置——足够接近 */
			loc = exprLocation((Node *) ((const IntoClause *) expr)->rel);
			break;
		case T_List:
			{
				/* 报告第一个带位置的列表成员的位置 */
				ListCell   *lc;

				loc = -1;		/* 只为消除编译器告警 */
				foreach(lc, (const List *) expr)
				{
					loc = exprLocation((Node *) lfirst(lc));
					if (loc >= 0)
						break;
				}
			}
			break;
		case T_A_Expr:
			{
				const A_Expr *aexpr = (const A_Expr *) expr;

				/* 使用操作符或左操作数（若有）中更靠左的那个 */
				/* 我们假定右操作数无法出现在操作符左边 */
				loc = leftmostLoc(aexpr->location,
								  exprLocation(aexpr->lexpr));
			}
			break;
		case T_ColumnRef:
			loc = ((const ColumnRef *) expr)->location;
			break;
		case T_ParamRef:
			loc = ((const ParamRef *) expr)->location;
			break;
		case T_A_Const:
			loc = ((const A_Const *) expr)->location;
			break;
		case T_FuncCall:
			{
				const FuncCall *fc = (const FuncCall *) expr;

				/* 同时考虑函数名和最左参数 */
				/* （我们假定任何 ORDER BY 节点都在名字右边） */
				loc = leftmostLoc(fc->location,
								  exprLocation((Node *) fc->args));
			}
			break;
		case T_A_ArrayExpr:
			/* 位置指向 ARRAY 或 [，它必须在最左边 */
			loc = ((const A_ArrayExpr *) expr)->location;
			break;
		case T_ResTarget:
			/* 我们不必检查所含的表达式（若有） */
			loc = ((const ResTarget *) expr)->location;
			break;
		case T_MultiAssignRef:
			loc = exprLocation(((const MultiAssignRef *) expr)->source);
			break;
		case T_TypeCast:
			{
				const TypeCast *tc = (const TypeCast *) expr;

				/*
				 * 这可以表示 CAST()、:: 或 TypeName 'literal'，因此
				 * 任一组成部分都可能在最左边。
				 */
				loc = exprLocation(tc->arg);
				loc = leftmostLoc(loc, tc->typeName->location);
				loc = leftmostLoc(loc, tc->location);
			}
			break;
		case T_CollateClause:
			/* 只用参数的位置 */
			loc = exprLocation(((const CollateClause *) expr)->arg);
			break;
		case T_SortBy:
			/* 只用参数的位置（忽略操作符，若有） */
			loc = exprLocation(((const SortBy *) expr)->node);
			break;
		case T_WindowDef:
			loc = ((const WindowDef *) expr)->location;
			break;
		case T_RangeTableSample:
			loc = ((const RangeTableSample *) expr)->location;
			break;
		case T_TypeName:
			loc = ((const TypeName *) expr)->location;
			break;
		case T_ColumnDef:
			loc = ((const ColumnDef *) expr)->location;
			break;
		case T_Constraint:
			loc = ((const Constraint *) expr)->location;
			break;
		case T_FunctionParameter:
			/* 只用类型名的位置 */
			loc = exprLocation((Node *) ((const FunctionParameter *) expr)->argType);
			break;
		case T_XmlSerialize:
			/* XMLSERIALIZE 关键字应当总是最先出现的东西 */
			loc = ((const XmlSerialize *) expr)->location;
			break;
		case T_GroupingSet:
			loc = ((const GroupingSet *) expr)->location;
			break;
		case T_WithClause:
			loc = ((const WithClause *) expr)->location;
			break;
		case T_InferClause:
			loc = ((const InferClause *) expr)->location;
			break;
		case T_OnConflictClause:
			loc = ((const OnConflictClause *) expr)->location;
			break;
		case T_CTESearchClause:
			loc = ((const CTESearchClause *) expr)->location;
			break;
		case T_CTECycleClause:
			loc = ((const CTECycleClause *) expr)->location;
			break;
		case T_CommonTableExpr:
			loc = ((const CommonTableExpr *) expr)->location;
			break;
		case T_JsonKeyValue:
			/* 只用键的位置 */
			loc = exprLocation((Node *) ((const JsonKeyValue *) expr)->key);
			break;
		case T_JsonObjectConstructor:
			loc = ((const JsonObjectConstructor *) expr)->location;
			break;
		case T_JsonArrayConstructor:
			loc = ((const JsonArrayConstructor *) expr)->location;
			break;
		case T_JsonArrayQueryConstructor:
			loc = ((const JsonArrayQueryConstructor *) expr)->location;
			break;
		case T_JsonAggConstructor:
			loc = ((const JsonAggConstructor *) expr)->location;
			break;
		case T_JsonObjectAgg:
			loc = exprLocation((Node *) ((const JsonObjectAgg *) expr)->constructor);
			break;
		case T_JsonArrayAgg:
			loc = exprLocation((Node *) ((const JsonArrayAgg *) expr)->constructor);
			break;
		case T_PlaceHolderVar:
			/* 只用参数的位置 */
			loc = exprLocation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		case T_InferenceElem:
			/* 只用嵌套表达式的位置 */
			loc = exprLocation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PartitionElem:
			loc = ((const PartitionElem *) expr)->location;
			break;
		case T_PartitionSpec:
			loc = ((const PartitionSpec *) expr)->location;
			break;
		case T_PartitionBoundSpec:
			loc = ((const PartitionBoundSpec *) expr)->location;
			break;
		case T_PartitionRangeDatum:
			loc = ((const PartitionRangeDatum *) expr)->location;
			break;
		default:
			/* 对任何其他节点类型，位置就是未知的…… */
			loc = -1;
			break;
	}
	return loc;
}

/*
 * leftmostLoc - 为 exprLocation 提供支持
 *
 * 取两个解析位置中较小的那个，但忽略未知值
 */
static int
leftmostLoc(int loc1, int loc2)
{
	if (loc1 < 0)
		return loc2;
	else if (loc2 < 0)
		return loc1;
	else
		return Min(loc1, loc2);
}


/*
 * fix_opfuncids
 *	  为给定树中每个 OpExpr 节点根据 opno 计算 opfuncid 字段。
 *	  给定的树可以是 expression_tree_walker 能处理的任何东西。
 *
 * 参数会被就地修改。（这没问题，因为即使由于
 * 共享结构而多次访问同一节点，我们对该节点
 * 也想要同样的修改。）
 */
void
fix_opfuncids(Node *node)
{
	/* 这次树遍历不需要特殊准备，那就开始吧…… */
	fix_opfuncids_walker(node, NULL);
}

static bool
fix_opfuncids_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* 依赖结构等价 */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* 依赖结构等价 */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	return expression_tree_walker(node, fix_opfuncids_walker, context);
}

/*
 * set_opfuncid
 *		设置 OpExpr 节点中的 opfuncid（过程 OID），
 *		若尚未设置的话。
 *
 * 由于结构等价，这也可用于
 * DistinctExpr 和 NullIfExpr 节点。
 */
void
set_opfuncid(OpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}

/*
 * set_sa_opfuncid
 *		同上，用于 ScalarArrayOpExpr 节点。
 */
void
set_sa_opfuncid(ScalarArrayOpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}


/*
 *	check_functions_in_node -
 *	  对给定表达式节点中包含的每个函数 OID 调用 checker()
 *
 * 若 checker() 返回 true 则返回 true；对表示不止
 * 一次函数调用的节点，若 checker() 对其中
 * 任一函数返回 true 则返回 true。若节点不调用任何
 * SQL 可见函数则返回 false。调用方不得传入 node == NULL。
 *
 * 本函数只检查给定节点；不递归进入任何
 * 子表达式。调用方通常希望自己控制递归，
 * 以防还要做额外检查，或因为他们对
 * 树的哪些部分需要访问有特殊规则。
 *
 * 注意：我们忽略 MinMaxExpr、SQLValueFunction、XmlExpr、CoerceToDomain
 * 和 NextValueExpr 节点，因为它们不含 SQL 函数 OIDs。
 * 不过它们可以调用 SQL 可见函数，因此调用方应当
 * 考虑如何对待它们。
 */
bool
check_functions_in_node(Node *node, check_function_callback checker,
						void *context)
{
	switch (nodeTag(node))
	{
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				if (checker(expr->aggfnoid, context))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				if (checker(expr->winfnoid, context))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (checker(expr->funcid, context))
					return true;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* 结构上等价于 OpExpr */
		case T_NullIfExpr:		/* 结构上等价于 OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				/* 若 opfuncid 尚未设置，就设置它 */
				set_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				set_sa_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *expr = (CoerceViaIO *) node;
				Oid			iofunc;
				Oid			typioparam;
				bool		typisvarlena;

				/* 检查结果类型的输入函数 */
				getTypeInputInfo(expr->resulttype,
								 &iofunc, &typioparam);
				if (checker(iofunc, context))
					return true;
				/* 检查输入类型的输出函数 */
				getTypeOutputInfo(exprType((Node *) expr->arg),
								  &iofunc, &typisvarlena);
				if (checker(iofunc, context))
					return true;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *opid;

				foreach(opid, rcexpr->opnos)
				{
					Oid			opfuncid = get_opcode(lfirst_oid(opid));

					if (checker(opfuncid, context))
						return true;
				}
			}
			break;
		default:
			break;
	}
	return false;
}


/*
 * 标准的表达式树遍历支持
 *
 * 以前许多不同例程里有近乎重复的代码，它们
 * 知道如何递归表达式节点树。那很难
 * 维护，而且我们经常出漏洞，因为某个
 * 例程漏掉了对某种节点类型的支持。多数情况下，
 * 这些例程其实只关心某些节点类型，对其他类型
 * 只是必须递归穿过
 * 非原始节点类型。因此我们现在提供通用的树遍历
 * 逻辑，把重复的样板代码收拢起来。有
 * 两个版本：expression_tree_walker() 和 expression_tree_mutator()。
 */

/*
 * expression_tree_walker() 用来支持以
 * 只读方式遍历树的例程（它也能用于
 * 就地修改节点、但从不增删或替换节点的例程）。
 * 遍历例程应当像这样：
 *
 * bool my_walker (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return false;
 *		// 检查需要特殊处理的节点，例如：
 *		if (IsA(node, Var))
 *		{
 *			... 对 Var 节点做特殊动作
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... 对其他节点类型做特殊动作
 *		}
 *		// 对未特殊处理的任何节点类型，执行：
 *		return expression_tree_walker(node, my_walker, (void *) context);
 * }
 *
 * "context" 参数指向一个结构体，其中存放遍历例程需要的
 * 任何上下文信息——它也可用来返回
 * 遍历例程收集的数据。该参数不会被
 * expression_tree_walker 触碰，但会传给递归的子调用
 * 中的 my_walker。树遍历由一个准备例程启动，它
 * 填好相应的上下文结构体，对顶层
 * 节点调用 my_walker，然后检查结果。
 *
 * 遍历例程应返回 "false" 以继续遍历，或
 * 返回 "true" 以中止遍历并立即向顶层
 * 调用方返回 "true"。若遍历例程已经
 * 找到了要找的东西，可用此来短路遍历。当且仅当
 * 没有任何一次遍历调用返回 "true" 时，才向顶层调用方返回 "false"。
 *
 * expression_tree_walker 处理的节点类型包括
 * 规划阶段目标列表和限定子句中通常出现的全部类型。
 * 特别是它处理 List 节点，因为合取范式化的限定子句
 * 在顶层会有 List 结构，并且它处理 TargetEntry 节点，
 * 因此扫描目标列表不需要额外代码。
 * 另外也处理 RangeTblRef、FromExpr、JoinExpr 和 SetOperationStmt 节点，
 * 因此查询连接树和 setOperation 树可以
 * 不写额外代码就处理。
 *
 * expression_tree_walker 会正常递归 SubLink 节点
 * 进入 "testexpr" 子树（属于外层
 * 计划的表达式）。它也会对子 Query 节点调用遍历例程；不过，当
 * expression_tree_walker 本身在 Query 节点上被调用时，它什么也不做
 * 并返回 "false"。净效果是，除非遍历例程在
 * Query 节点上做了特殊处理，否则表达式树遍历期间不会访问
 * 子查询。这正是许多情况下想要的行为
 * ——而对那些确实想递归进子查询的遍历例程，进入子查询时
 * 通常反正需要特殊行为（例如
 * 递增深度计数器）。想检查子查询的遍历例程
 * 应包含大致如下的代码：
 *
 *		if (IsA(node, Query))
 *		{
 *			为子查询调整 context；
 *			result = query_tree_walker((Query *) node, my_walker, context,
 *									   0); // 按需要调整标志
 *			必要时恢复 context；
 *			return result;
 *		}
 *
 * query_tree_walker 是一个便捷例程（见下文），它对给定 Query 节点的
 * 所有表达式子树调用遍历例程。
 *
 * expression_tree_walker 会正常递归 SubPlan 节点
 * 进入 "testexpr" 和 "args" 列表（属于
 * 外层计划的表达式）。但它不会触碰已完成的子计划。由于
 * 没有指向原始 Query 的链接，无法递归进入
 * 已规划表达式树的子查询。对目前的
 * 用途这没问题，但将来也许需要重新考虑。
 */

bool
expression_tree_walker_impl(Node *node,
							tree_walker_callback walker,
							void *context)
{
	ListCell   *temp;

	/*
	 * 遍历例程已经访问过当前节点，因此我们只需
	 * 递归进入它拥有的任何子节点。
	 *
	 * 我们假定遍历例程本身对 List 节点不感兴趣，因此
	 * 当预期是 List 时，就直接递归到自身，
	 * 而不必去调用遍历例程。
	 */
#define WALK(n) walker((Node *) (n), context)

#define LIST_WALK(l) expression_tree_walker_impl((Node *) (l), walker, context)

	if (node == NULL)
		return false;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_CTESearchClause:
			/* 没有表达式子节点的原始节点类型 */
			break;
		case T_WithCheckOption:
			return WALK(((WithCheckOption *) node)->qual);
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				/* 直接在 List 上递归 */
				if (LIST_WALK(expr->aggdirectargs))
					return true;
				if (LIST_WALK(expr->args))
					return true;
				if (LIST_WALK(expr->aggorder))
					return true;
				if (LIST_WALK(expr->aggdistinct))
					return true;
				if (WALK(expr->aggfilter))
					return true;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;

				if (LIST_WALK(grouping->args))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				/* 直接在 List 上递归 */
				if (LIST_WALK(expr->args))
					return true;
				if (WALK(expr->aggfilter))
					return true;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				/* 直接在上/下容器下标列表上递归 */
				if (LIST_WALK(sbsref->refupperindexpr))
					return true;
				if (LIST_WALK(sbsref->reflowerindexpr))
					return true;
				/* 不过遍历例程必须看到 refexpr 和 refassgnexpr */
				if (WALK(sbsref->refexpr))
					return true;

				if (WALK(sbsref->refassgnexpr))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_NamedArgExpr:
			return WALK(((NamedArgExpr *) node)->arg);
		case T_OpExpr:
		case T_DistinctExpr:	/* 结构上等价于 OpExpr */
		case T_NullIfExpr:		/* 结构上等价于 OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (WALK(sublink->testexpr))
					return true;

				/*
				 * 也对子链接的 Query 节点调用遍历例程，以便它
				 * 可以按意愿递归进子查询。
				 */
				return WALK(sublink->subselect);
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/* 递归进 testexpr，但不进入 Plan */
				if (WALK(subplan->testexpr))
					return true;
				/* 也检查 args 列表 */
				if (LIST_WALK(subplan->args))
					return true;
			}
			break;
		case T_AlternativeSubPlan:
			return LIST_WALK(((AlternativeSubPlan *) node)->subplans);
		case T_FieldSelect:
			return WALK(((FieldSelect *) node)->arg);
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				if (WALK(fstore->arg))
					return true;
				if (WALK(fstore->newvals))
					return true;
			}
			break;
		case T_RelabelType:
			return WALK(((RelabelType *) node)->arg);
		case T_CoerceViaIO:
			return WALK(((CoerceViaIO *) node)->arg);
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

				if (WALK(acoerce->arg))
					return true;
				if (WALK(acoerce->elemexpr))
					return true;
			}
			break;
		case T_ConvertRowtypeExpr:
			return WALK(((ConvertRowtypeExpr *) node)->arg);
		case T_CollateExpr:
			return WALK(((CollateExpr *) node)->arg);
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (WALK(caseexpr->arg))
					return true;
				/* 我们假定遍历例程也不关心 CaseWhens */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (WALK(when->expr))
						return true;
					if (WALK(when->result))
						return true;
				}
				if (WALK(caseexpr->defresult))
					return true;
			}
			break;
		case T_ArrayExpr:
			return WALK(((ArrayExpr *) node)->elements);
		case T_RowExpr:
			/* 假定 colnames 不值得关注 */
			return WALK(((RowExpr *) node)->args);
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				if (WALK(rcexpr->largs))
					return true;
				if (WALK(rcexpr->rargs))
					return true;
			}
			break;
		case T_CoalesceExpr:
			return WALK(((CoalesceExpr *) node)->args);
		case T_MinMaxExpr:
			return WALK(((MinMaxExpr *) node)->args);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (WALK(xexpr->named_args))
					return true;
				/* 我们假定遍历例程不关心 arg_names */
				if (WALK(xexpr->args))
					return true;
			}
			break;
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				if (WALK(jve->raw_expr))
					return true;
				if (WALK(jve->formatted_expr))
					return true;
			}
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;

				if (WALK(ctor->args))
					return true;
				if (WALK(ctor->func))
					return true;
				if (WALK(ctor->coercion))
					return true;
			}
			break;
		case T_JsonIsPredicate:
			return WALK(((JsonIsPredicate *) node)->expr);
		case T_NullTest:
			return WALK(((NullTest *) node)->arg);
		case T_BooleanTest:
			return WALK(((BooleanTest *) node)->arg);
		case T_CoerceToDomain:
			return WALK(((CoerceToDomain *) node)->arg);
		case T_TargetEntry:
			return WALK(((TargetEntry *) node)->expr);
		case T_Query:
			/* 对子 Query 什么也不做，见上面的讨论 */
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				if (WALK(wc->partitionClause))
					return true;
				if (WALK(wc->orderClause))
					return true;
				if (WALK(wc->startOffset))
					return true;
				if (WALK(wc->endOffset))
					return true;
				if (WALK(wc->runCondition))
					return true;
			}
			break;
		case T_CTECycleClause:
			{
				CTECycleClause *cc = (CTECycleClause *) node;

				if (WALK(cc->cycle_mark_value))
					return true;
				if (WALK(cc->cycle_mark_default))
					return true;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				/*
				 * 也对 CTE 的 Query 节点调用遍历例程，以便它
				 * 可以按意愿递归进子查询。
				 */
				if (WALK(cte->ctequery))
					return true;

				if (WALK(cte->search_clause))
					return true;
				if (WALK(cte->cycle_clause))
					return true;
			}
			break;
		case T_JsonKeyValue:
			{
				JsonKeyValue *kv = (JsonKeyValue *) node;

				if (WALK(kv->key))
					return true;
				if (WALK(kv->value))
					return true;
			}
			break;
		case T_JsonObjectConstructor:
			{
				JsonObjectConstructor *ctor = (JsonObjectConstructor *) node;

				if (LIST_WALK(ctor->exprs))
					return true;
			}
			break;
		case T_JsonArrayConstructor:
			{
				JsonArrayConstructor *ctor = (JsonArrayConstructor *) node;

				if (LIST_WALK(ctor->exprs))
					return true;
			}
			break;
		case T_JsonArrayQueryConstructor:
			{
				JsonArrayQueryConstructor *ctor = (JsonArrayQueryConstructor *) node;

				if (WALK(ctor->query))
					return true;
			}
			break;
		case T_JsonAggConstructor:
			{
				JsonAggConstructor *ctor = (JsonAggConstructor *) node;

				if (WALK(ctor->agg_filter))
					return true;
				if (WALK(ctor->agg_order))
					return true;
				if (WALK(ctor->over))
					return true;
			}
			break;
		case T_JsonObjectAgg:
			{
				JsonObjectAgg *ctor = (JsonObjectAgg *) node;

				if (WALK(ctor->constructor))
					return true;
				if (WALK(ctor->arg))
					return true;
			}
			break;
		case T_JsonArrayAgg:
			{
				JsonArrayAgg *ctor = (JsonArrayAgg *) node;

				if (WALK(ctor->constructor))
					return true;
				if (WALK(ctor->arg))
					return true;
			}
			break;

		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *pbs = (PartitionBoundSpec *) node;

				if (WALK(pbs->listdatums))
					return true;
				if (WALK(pbs->lowerdatums))
					return true;
				if (WALK(pbs->upperdatums))
					return true;
			}
			break;
		case T_PartitionRangeDatum:
			{
				PartitionRangeDatum *prd = (PartitionRangeDatum *) node;

				if (WALK(prd->value))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (WALK(lfirst(temp)))
					return true;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				if (LIST_WALK(from->fromlist))
					return true;
				if (WALK(from->quals))
					return true;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *onconflict = (OnConflictExpr *) node;

				if (WALK(onconflict->arbiterElems))
					return true;
				if (WALK(onconflict->arbiterWhere))
					return true;
				if (WALK(onconflict->onConflictSet))
					return true;
				if (WALK(onconflict->onConflictWhere))
					return true;
				if (WALK(onconflict->exclRelTlist))
					return true;
			}
			break;
		case T_MergeAction:
			{
				MergeAction *action = (MergeAction *) node;

				if (WALK(action->qual))
					return true;
				if (WALK(action->targetList))
					return true;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;

				if (WALK(opstep->exprs))
					return true;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* 没有表达式子节点 */
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (WALK(join->larg))
					return true;
				if (WALK(join->rarg))
					return true;
				if (WALK(join->quals))
					return true;

				/*
				 * alias 子句和 using 列表被视为不值得关注。
				 */
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				if (WALK(setop->larg))
					return true;
				if (WALK(setop->rarg))
					return true;

				/* groupClauses 被视为不值得关注 */
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;

				if (WALK(iclause->rinfo))
					return true;
				if (LIST_WALK(iclause->indexquals))
					return true;
			}
			break;
		case T_PlaceHolderVar:
			return WALK(((PlaceHolderVar *) node)->phexpr);
		case T_InferenceElem:
			return WALK(((InferenceElem *) node)->expr);
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;

				if (LIST_WALK(appinfo->translated_vars))
					return true;
			}
			break;
		case T_PlaceHolderInfo:
			return WALK(((PlaceHolderInfo *) node)->ph_var);
		case T_RangeTblFunction:
			return WALK(((RangeTblFunction *) node)->funcexpr);
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;

				if (LIST_WALK(tsc->args))
					return true;
				if (WALK(tsc->repeatable))
					return true;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;

				if (WALK(tf->ns_uris))
					return true;
				if (WALK(tf->docexpr))
					return true;
				if (WALK(tf->rowexpr))
					return true;
				if (WALK(tf->colexprs))
					return true;
				if (WALK(tf->coldefexprs))
					return true;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;

	/* 下面可以复用 WALK() 宏，但 LIST_WALK() 就不太行 */
#undef LIST_WALK
}

/*
 * query_tree_walker --- 开始遍历 Query 的表达式
 *
 * 本例程只是为了减少需要知道
 * Query 的全部表达式子树在哪里的地方。注意它可以用于
 * 从 Query 顶层开始遍历，无论遍历例程是否
 * 打算下降到子查询。它对在遍历例程内部
 * 下降到子查询也很有用。
 *
 * 有些调用方想抑制对子 Query 中某些项的访问，
 * 通常是因为他们需要特殊处理，或其实并不
 * 想递归进子查询。这由 flags 参数支持，
 * 它是要增加或抑制对
 * 所示项的访问的标志值的按位 OR。（以后可以按需增加更多标志位。）
 */
bool
query_tree_walker_impl(Query *query,
					   tree_walker_callback walker,
					   void *context,
					   int flags)
{
	Assert(query != NULL && IsA(query, Query));

	/*
	 * 这里我们不遍历任何 utilityStmt。不过，我们无法轻易断言
	 * 它不存在，因为至少有两条代码路径会让
	 * CREATE RULE 的动作语句到达这里，并且规则动作中允许
	 * NOTIFY。
	 */

	if (WALK(query->targetList))
		return true;
	if (WALK(query->withCheckOptions))
		return true;
	if (WALK(query->onConflict))
		return true;
	if (WALK(query->mergeActionList))
		return true;
	if (WALK(query->returningList))
		return true;
	if (WALK(query->jointree))
		return true;
	if (WALK(query->setOperations))
		return true;
	if (WALK(query->havingQual))
		return true;
	if (WALK(query->limitOffset))
		return true;
	if (WALK(query->limitCount))
		return true;

	/*
	 * 多数调用方对 SortGroupClause 节点不感兴趣，因为那些
	 * 并不包含实际表达式。不过它们确实包含 OIDs，
	 * 依赖遍历等可能会需要。
	 */
	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		if (WALK(query->groupClause))
			return true;
		if (WALK(query->windowClause))
			return true;
		if (WALK(query->sortClause))
			return true;
		if (WALK(query->distinctClause))
			return true;
	}
	else
	{
		/*
		 * 但即使我们对 SortGroupClause 节点不感兴趣，
		 * 也需要遍历 WindowClause 节点下的表达式。
		 */
		ListCell   *lc;

		foreach(lc, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, lc);

			if (WALK(wc->startOffset))
				return true;
			if (WALK(wc->endOffset))
				return true;
			if (WALK(wc->runCondition))
				return true;
		}
	}

	/*
	 * 不遍历 groupingSets 和 rowMarks：
	 *
	 * groupingSets 只包含 ressortgrouprefs（整数），若没有
	 * 对应的 groupClause 或 tlist 就没有意义。
	 * 因此，任何需要关心它们的遍历例程都得在自己的 Query
	 * 处理中自行处理。
	 *
	 * 不遍历 rowMarks，因为它只包含范围表下标（以及
	 * 标志等），因此同样应在 Query 层处理。
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
	{
		if (WALK(query->cteList))
			return true;
	}
	if (!(flags & QTW_IGNORE_RANGE_TABLE))
	{
		if (range_table_walker(query->rtable, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * range_table_walker 只是 query_tree_walker 中扫描
 * 查询范围表的那一部分。把它拆出来是因为它
 * 单独也有用。
 */
bool
range_table_walker_impl(List *rtable,
						tree_walker_callback walker,
						void *context,
						int flags)
{
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, rt);

		if (range_table_entry_walker(rte, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * 有些调用方甚至想扫描各个 RTEs 中的表达式。
 */
bool
range_table_entry_walker_impl(RangeTblEntry *rte,
							  tree_walker_callback walker,
							  void *context,
							  int flags)
{
	/*
	 * 遍历例程也许需要在访问 RTE 内容之前或
	 * 之后检查 RTE 节点本身（或者可以想见两者都要）。注意若你
	 * 两个标志都不指定，遍历例程根本不会在 RTE 上被调用。
	 */
	if (flags & QTW_EXAMINE_RTES_BEFORE)
		if (WALK(rte))
			return true;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			if (WALK(rte->tablesample))
				return true;
			break;
		case RTE_SUBQUERY:
			if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
				if (WALK(rte->subquery))
					return true;
			break;
		case RTE_JOIN:
			if (!(flags & QTW_IGNORE_JOINALIASES))
				if (WALK(rte->joinaliasvars))
					return true;
			break;
		case RTE_FUNCTION:
			if (WALK(rte->functions))
				return true;
			break;
		case RTE_TABLEFUNC:
			if (WALK(rte->tablefunc))
				return true;
			break;
		case RTE_VALUES:
			if (WALK(rte->values_lists))
				return true;
			break;
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* 无事可做 */
			break;
	}

	if (WALK(rte->securityQuals))
		return true;

	if (flags & QTW_EXAMINE_RTES_AFTER)
		if (WALK(rte))
			return true;

	return false;
}


/*
 * expression_tree_mutator() 用来支持这样的例程：它们制作
 * 表达式树的修改副本，其中一些节点被增加、
 * 删除或替换为新的子树。原来的树（通常）
 * 不会被改动。每一层递归负责返回它收到的
 * 子树的副本（或适当修改后的替代物）。
 * 变换例程应当像这样：
 *
 * Node * my_mutator (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return NULL;
 *		// 检查需要特殊处理的节点，例如：
 *		if (IsA(node, Var))
 *		{
 *			... 创建并返回 Var 节点的修改副本
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... 对其他节点类型做特殊变换
 *		}
 *		// 对未特殊处理的任何节点类型，执行：
 *		return expression_tree_mutator(node, my_mutator, (void *) context);
 * }
 *
 * "context" 参数指向一个结构体，其中存放变换例程需要的
 * 任何上下文信息——它也可用来返回变换器
 * 收集的额外数据。该参数不会被
 * expression_tree_mutator 触碰，但会传给递归的子调用
 * 中的 my_mutator。树遍历由一个准备例程启动，它
 * 填好相应的上下文结构体，对树的
 * 顶层节点调用 my_mutator，并做任何需要的后处理。
 *
 * 每一层递归都必须返回适当修改过的 Node。
 * 若调用了 expression_tree_mutator()，它会精确复制
 * 给定的 Node，但调用 my_mutator() 来复制该
 * Node 的子节点。这样 my_mutator() 完全控制
 * 复制过程，但不必直接处理它
 * 不感兴趣的表达式树。
 *
 * 正如 expression_tree_walker 那样，expression_tree_mutator 处理的
 * 节点类型包括规划阶段目标列表
 * 和限定子句中通常出现的全部类型。
 *
 * expression_tree_mutator 会正常递归 SubLink 节点
 * 进入 "testexpr" 子树（属于外层
 * 计划的表达式）。它也会对子 Query 节点调用变换器；不过，当
 * expression_tree_mutator 本身在 Query 节点上被调用时，它什么也不做
 * 并返回未修改的 Query 节点。净效果是，除非
 * 变换器在 Query 节点上做了特殊处理，子查询不会被
 * 访问或修改；原来的子查询会由新的
 * SubLink 节点链接到。想下降到子查询的变换器通常
 * 通过识别 Query 节点并调用 query_tree_mutator（见下）来做。
 *
 * expression_tree_mutator 会递归 SubPlan 节点进入
 * "testexpr" 和 "args" 列表（属于外层计划），但
 * 只会复制指向内层计划的链接，因为那通常正是
 * 表达式树变换器想要的。想修改子计划的变换器
 * 可以通过识别 SubPlan 表达式节点
 * 并做正确的事，来强制适当的行为。
 */

Node *
expression_tree_mutator_impl(Node *node,
							 tree_mutator_callback mutator,
							 void *context)
{
	/*
	 * 变换器已经决定不修改当前节点，但我们
	 * 必须对任何子节点调用变换器。
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), context) )

	if (node == NULL)
		return NULL;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * 没有表达式子节点的原始节点类型。Var 和
			 * Const 足够常见，值得特殊处理，其余的
			 * 我们就用 copyObject。
			 */
		case T_Var:
			{
				Var		   *var = (Var *) node;
				Var		   *newnode;

				FLATCOPY(newnode, var, Var);
				/* 假定不必复制 varnullingrels 位图集 */
				return (Node *) newnode;
			}
			break;
		case T_Const:
			{
				Const	   *oldnode = (Const *) node;
				Const	   *newnode;

				FLATCOPY(newnode, oldnode, Const);
				/* XXX 我们没有费心做 datumCopy；要不要做？ */
				return (Node *) newnode;
			}
			break;
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_JsonFormat:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_CTESearchClause:
			return (Node *) copyObject(node);
		case T_WithCheckOption:
			{
				WithCheckOption *wco = (WithCheckOption *) node;
				WithCheckOption *newnode;

				FLATCOPY(newnode, wco, WithCheckOption);
				MUTATE(newnode->qual, wco->qual, Node *);
				return (Node *) newnode;
			}
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				Aggref	   *newnode;

				FLATCOPY(newnode, aggref, Aggref);
				/* 假定变换不会改变参数的类型 */
				newnode->aggargtypes = list_copy(aggref->aggargtypes);
				MUTATE(newnode->aggdirectargs, aggref->aggdirectargs, List *);
				MUTATE(newnode->args, aggref->args, List *);
				MUTATE(newnode->aggorder, aggref->aggorder, List *);
				MUTATE(newnode->aggdistinct, aggref->aggdistinct, List *);
				MUTATE(newnode->aggfilter, aggref->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;
				GroupingFunc *newnode;

				FLATCOPY(newnode, grouping, GroupingFunc);
				MUTATE(newnode->args, grouping->args, List *);

				/*
				 * 这里我们假定变换参数不会改变
				 * 语义，即参数不会被变换成
				 * 与 GROUP BY 子句中先前匹配的表达式
				 * 在语义上不同的东西。
				 *
				 * 若某个变换器不知怎么想这么做，它必须
				 * 自行妥善处理 refs 和 cols 列表。
				 */
				newnode->refs = list_copy(grouping->refs);
				newnode->cols = list_copy(grouping->cols);

				return (Node *) newnode;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFunc *newnode;

				FLATCOPY(newnode, wfunc, WindowFunc);
				MUTATE(newnode->args, wfunc->args, List *);
				MUTATE(newnode->aggfilter, wfunc->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				SubscriptingRef *newnode;

				FLATCOPY(newnode, sbsref, SubscriptingRef);
				MUTATE(newnode->refupperindexpr, sbsref->refupperindexpr,
					   List *);
				MUTATE(newnode->reflowerindexpr, sbsref->reflowerindexpr,
					   List *);
				MUTATE(newnode->refexpr, sbsref->refexpr,
					   Expr *);
				MUTATE(newnode->refassgnexpr, sbsref->refassgnexpr,
					   Expr *);

				return (Node *) newnode;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				FuncExpr   *newnode;

				FLATCOPY(newnode, expr, FuncExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NamedArgExpr:
			{
				NamedArgExpr *nexpr = (NamedArgExpr *) node;
				NamedArgExpr *newnode;

				FLATCOPY(newnode, nexpr, NamedArgExpr);
				MUTATE(newnode->arg, nexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				OpExpr	   *newnode;

				FLATCOPY(newnode, expr, OpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				DistinctExpr *newnode;

				FLATCOPY(newnode, expr, DistinctExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *expr = (NullIfExpr *) node;
				NullIfExpr *newnode;

				FLATCOPY(newnode, expr, NullIfExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				ScalarArrayOpExpr *newnode;

				FLATCOPY(newnode, expr, ScalarArrayOpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				BoolExpr   *newnode;

				FLATCOPY(newnode, expr, BoolExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				SubLink    *newnode;

				FLATCOPY(newnode, sublink, SubLink);
				MUTATE(newnode->testexpr, sublink->testexpr, Node *);

				/*
				 * 也对子链接的 Query 节点调用变换器，以便它
				 * 可以按意愿递归进子查询。
				 */
				MUTATE(newnode->subselect, sublink->subselect, Node *);
				return (Node *) newnode;
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlan    *newnode;

				FLATCOPY(newnode, subplan, SubPlan);
				/* 变换 testexpr */
				MUTATE(newnode->testexpr, subplan->testexpr, Node *);
				/* 变换 args 列表（要传给子计划的参数） */
				MUTATE(newnode->args, subplan->args, List *);
				/* 但不变换子 Plan 本身，它按原样引用 */
				return (Node *) newnode;
			}
			break;
		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				AlternativeSubPlan *newnode;

				FLATCOPY(newnode, asplan, AlternativeSubPlan);
				MUTATE(newnode->subplans, asplan->subplans, List *);
				return (Node *) newnode;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newnode;

				FLATCOPY(newnode, fselect, FieldSelect);
				MUTATE(newnode->arg, fselect->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				FieldStore *newnode;

				FLATCOPY(newnode, fstore, FieldStore);
				MUTATE(newnode->arg, fstore->arg, Expr *);
				MUTATE(newnode->newvals, fstore->newvals, List *);
				newnode->fieldnums = list_copy(fstore->fieldnums);
				return (Node *) newnode;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				RelabelType *newnode;

				FLATCOPY(newnode, relabel, RelabelType);
				MUTATE(newnode->arg, relabel->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				CoerceViaIO *newnode;

				FLATCOPY(newnode, iocoerce, CoerceViaIO);
				MUTATE(newnode->arg, iocoerce->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				ArrayCoerceExpr *newnode;

				FLATCOPY(newnode, acoerce, ArrayCoerceExpr);
				MUTATE(newnode->arg, acoerce->arg, Expr *);
				MUTATE(newnode->elemexpr, acoerce->elemexpr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convexpr = (ConvertRowtypeExpr *) node;
				ConvertRowtypeExpr *newnode;

				FLATCOPY(newnode, convexpr, ConvertRowtypeExpr);
				MUTATE(newnode->arg, convexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CollateExpr:
			{
				CollateExpr *collate = (CollateExpr *) node;
				CollateExpr *newnode;

				FLATCOPY(newnode, collate, CollateExpr);
				MUTATE(newnode->arg, collate->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newnode;

				FLATCOPY(newnode, caseexpr, CaseExpr);
				MUTATE(newnode->arg, caseexpr->arg, Expr *);
				MUTATE(newnode->args, caseexpr->args, List *);
				MUTATE(newnode->defresult, caseexpr->defresult, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseWhen:
			{
				CaseWhen   *casewhen = (CaseWhen *) node;
				CaseWhen   *newnode;

				FLATCOPY(newnode, casewhen, CaseWhen);
				MUTATE(newnode->expr, casewhen->expr, Expr *);
				MUTATE(newnode->result, casewhen->result, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				ArrayExpr  *newnode;

				FLATCOPY(newnode, arrayexpr, ArrayExpr);
				MUTATE(newnode->elements, arrayexpr->elements, List *);
				return (Node *) newnode;
			}
			break;
		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				RowExpr    *newnode;

				FLATCOPY(newnode, rowexpr, RowExpr);
				MUTATE(newnode->args, rowexpr->args, List *);
				/* 假定 colnames 不必复制 */
				return (Node *) newnode;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				RowCompareExpr *newnode;

				FLATCOPY(newnode, rcexpr, RowCompareExpr);
				MUTATE(newnode->largs, rcexpr->largs, List *);
				MUTATE(newnode->rargs, rcexpr->rargs, List *);
				return (Node *) newnode;
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExpr *newnode;

				FLATCOPY(newnode, coalesceexpr, CoalesceExpr);
				MUTATE(newnode->args, coalesceexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				MinMaxExpr *newnode;

				FLATCOPY(newnode, minmaxexpr, MinMaxExpr);
				MUTATE(newnode->args, minmaxexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				XmlExpr    *newnode;

				FLATCOPY(newnode, xexpr, XmlExpr);
				MUTATE(newnode->named_args, xexpr->named_args, List *);
				/* 假定变换器不关心 arg_names */
				MUTATE(newnode->args, xexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_JsonReturning:
			{
				JsonReturning *jr = (JsonReturning *) node;
				JsonReturning *newnode;

				FLATCOPY(newnode, jr, JsonReturning);
				MUTATE(newnode->format, jr->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;
				JsonValueExpr *newnode;

				FLATCOPY(newnode, jve, JsonValueExpr);
				MUTATE(newnode->raw_expr, jve->raw_expr, Expr *);
				MUTATE(newnode->formatted_expr, jve->formatted_expr, Expr *);
				MUTATE(newnode->format, jve->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *jce = (JsonConstructorExpr *) node;
				JsonConstructorExpr *newnode;

				FLATCOPY(newnode, jce, JsonConstructorExpr);
				MUTATE(newnode->args, jce->args, List *);
				MUTATE(newnode->func, jce->func, Expr *);
				MUTATE(newnode->coercion, jce->coercion, Expr *);
				MUTATE(newnode->returning, jce->returning, JsonReturning *);

				return (Node *) newnode;
			}
		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;
				JsonIsPredicate *newnode;

				FLATCOPY(newnode, pred, JsonIsPredicate);
				MUTATE(newnode->expr, pred->expr, Node *);
				MUTATE(newnode->format, pred->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTest   *newnode;

				FLATCOPY(newnode, ntest, NullTest);
				MUTATE(newnode->arg, ntest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;
				BooleanTest *newnode;

				FLATCOPY(newnode, btest, BooleanTest);
				MUTATE(newnode->arg, btest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				CoerceToDomain *newnode;

				FLATCOPY(newnode, ctest, CoerceToDomain);
				MUTATE(newnode->arg, ctest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *targetentry = (TargetEntry *) node;
				TargetEntry *newnode;

				FLATCOPY(newnode, targetentry, TargetEntry);
				MUTATE(newnode->expr, targetentry->expr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_Query:
			/* 对子 Query 什么也不做，见上面的讨论 */
			return node;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;
				WindowClause *newnode;

				FLATCOPY(newnode, wc, WindowClause);
				MUTATE(newnode->partitionClause, wc->partitionClause, List *);
				MUTATE(newnode->orderClause, wc->orderClause, List *);
				MUTATE(newnode->startOffset, wc->startOffset, Node *);
				MUTATE(newnode->endOffset, wc->endOffset, Node *);
				MUTATE(newnode->runCondition, wc->runCondition, List *);
				return (Node *) newnode;
			}
			break;
		case T_CTECycleClause:
			{
				CTECycleClause *cc = (CTECycleClause *) node;
				CTECycleClause *newnode;

				FLATCOPY(newnode, cc, CTECycleClause);
				MUTATE(newnode->cycle_mark_value, cc->cycle_mark_value, Node *);
				MUTATE(newnode->cycle_mark_default, cc->cycle_mark_default, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;
				CommonTableExpr *newnode;

				FLATCOPY(newnode, cte, CommonTableExpr);

				/*
				 * 也对 CTE 的 Query 节点调用变换器，以便它
				 * 可以按意愿递归进子查询。
				 */
				MUTATE(newnode->ctequery, cte->ctequery, Node *);

				MUTATE(newnode->search_clause, cte->search_clause, CTESearchClause *);
				MUTATE(newnode->cycle_clause, cte->cycle_clause, CTECycleClause *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *pbs = (PartitionBoundSpec *) node;
				PartitionBoundSpec *newnode;

				FLATCOPY(newnode, pbs, PartitionBoundSpec);
				MUTATE(newnode->listdatums, pbs->listdatums, List *);
				MUTATE(newnode->lowerdatums, pbs->lowerdatums, List *);
				MUTATE(newnode->upperdatums, pbs->upperdatums, List *);
				return (Node *) newnode;
			}
			break;
		case T_PartitionRangeDatum:
			{
				PartitionRangeDatum *prd = (PartitionRangeDatum *) node;
				PartitionRangeDatum *newnode;

				FLATCOPY(newnode, prd, PartitionRangeDatum);
				MUTATE(newnode->value, prd->value, Node *);
				return (Node *) newnode;
			}
			break;
		case T_List:
			{
				/*
				 * 我们假定变换器本身对列表节点
				 * 不感兴趣，因此只对每个列表元素调用它。注意：这
				 * 在元素为整数的列表上会严重失败！
				 */
				List	   *resultlist;
				ListCell   *temp;

				resultlist = NIL;
				foreach(temp, (List *) node)
				{
					resultlist = lappend(resultlist,
										 mutator((Node *) lfirst(temp),
												 context));
				}
				return (Node *) resultlist;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;
				FromExpr   *newnode;

				FLATCOPY(newnode, from, FromExpr);
				MUTATE(newnode->fromlist, from->fromlist, List *);
				MUTATE(newnode->quals, from->quals, Node *);
				return (Node *) newnode;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *oc = (OnConflictExpr *) node;
				OnConflictExpr *newnode;

				FLATCOPY(newnode, oc, OnConflictExpr);
				MUTATE(newnode->arbiterElems, oc->arbiterElems, List *);
				MUTATE(newnode->arbiterWhere, oc->arbiterWhere, Node *);
				MUTATE(newnode->onConflictSet, oc->onConflictSet, List *);
				MUTATE(newnode->onConflictWhere, oc->onConflictWhere, Node *);
				MUTATE(newnode->exclRelTlist, oc->exclRelTlist, List *);

				return (Node *) newnode;
			}
			break;
		case T_MergeAction:
			{
				MergeAction *action = (MergeAction *) node;
				MergeAction *newnode;

				FLATCOPY(newnode, action, MergeAction);
				MUTATE(newnode->qual, action->qual, Node *);
				MUTATE(newnode->targetList, action->targetList, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;
				PartitionPruneStepOp *newnode;

				FLATCOPY(newnode, opstep, PartitionPruneStepOp);
				MUTATE(newnode->exprs, opstep->exprs, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* 没有表达式子节点 */
			return (Node *) copyObject(node);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;
				JoinExpr   *newnode;

				FLATCOPY(newnode, join, JoinExpr);
				MUTATE(newnode->larg, join->larg, Node *);
				MUTATE(newnode->rarg, join->rarg, Node *);
				MUTATE(newnode->quals, join->quals, Node *);
				/* 默认不变换 alias 或 using */
				return (Node *) newnode;
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;
				SetOperationStmt *newnode;

				FLATCOPY(newnode, setop, SetOperationStmt);
				MUTATE(newnode->larg, setop->larg, Node *);
				MUTATE(newnode->rarg, setop->rarg, Node *);
				/* 默认不变换 groupClauses */
				return (Node *) newnode;
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;
				IndexClause *newnode;

				FLATCOPY(newnode, iclause, IndexClause);
				MUTATE(newnode->rinfo, iclause->rinfo, RestrictInfo *);
				MUTATE(newnode->indexquals, iclause->indexquals, List *);
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderVar:
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderVar *newnode;

				FLATCOPY(newnode, phv, PlaceHolderVar);
				MUTATE(newnode->phexpr, phv->phexpr, Expr *);
				/* 假定不必复制 relids 位图集 */
				return (Node *) newnode;
			}
			break;
		case T_InferenceElem:
			{
				InferenceElem *inferenceelemdexpr = (InferenceElem *) node;
				InferenceElem *newnode;

				FLATCOPY(newnode, inferenceelemdexpr, InferenceElem);
				MUTATE(newnode->expr, newnode->expr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;
				AppendRelInfo *newnode;

				FLATCOPY(newnode, appinfo, AppendRelInfo);
				MUTATE(newnode->translated_vars, appinfo->translated_vars, List *);
				/* 假定不必对 parent_colnos[] 做任何事 */
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderInfo:
			{
				PlaceHolderInfo *phinfo = (PlaceHolderInfo *) node;
				PlaceHolderInfo *newnode;

				FLATCOPY(newnode, phinfo, PlaceHolderInfo);
				MUTATE(newnode->ph_var, phinfo->ph_var, PlaceHolderVar *);
				/* 假定不必复制 relids 位图集 */
				return (Node *) newnode;
			}
			break;
		case T_RangeTblFunction:
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) node;
				RangeTblFunction *newnode;

				FLATCOPY(newnode, rtfunc, RangeTblFunction);
				MUTATE(newnode->funcexpr, rtfunc->funcexpr, Node *);
				/* 假定不必复制 coldef 信息列表 */
				return (Node *) newnode;
			}
			break;
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;
				TableSampleClause *newnode;

				FLATCOPY(newnode, tsc, TableSampleClause);
				MUTATE(newnode->args, tsc->args, List *);
				MUTATE(newnode->repeatable, tsc->repeatable, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;
				TableFunc  *newnode;

				FLATCOPY(newnode, tf, TableFunc);
				MUTATE(newnode->ns_uris, tf->ns_uris, List *);
				MUTATE(newnode->docexpr, tf->docexpr, Node *);
				MUTATE(newnode->rowexpr, tf->rowexpr, Node *);
				MUTATE(newnode->colexprs, tf->colexprs, List *);
				MUTATE(newnode->coldefexprs, tf->coldefexprs, List *);
				return (Node *) newnode;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	/* 无法到达这里，但要消除编译器告警 */
	return NULL;
}


/*
 * query_tree_mutator --- 开始修改 Query 的表达式
 *
 * 本例程只是为了减少需要知道
 * Query 的全部表达式子树在哪里的地方。注意它可以用于
 * 从 Query 顶层开始遍历，无论变换器是否
 * 打算下降到子查询。它对在变换器内部
 * 下降到子查询也很有用。
 *
 * 有些调用方想抑制对 Query 中某些项的变换，
 * 通常是因为他们需要特殊处理，或其实并不
 * 想递归进子查询。这由 flags 参数支持，
 * 它是要抑制对
 * 所示项的变换的标志值的按位 OR。（以后可以按需增加更多标志位。）
 *
 * 通常顶层 Query 节点本身会被复制，但有些调用方希望
 * 就地修改它；他们必须在 flags 中传入 QTW_DONT_COPY_QUERY。
 * 无论如何，所有被修改的子结构都会被安全地复制。
 */
Query *
query_tree_mutator_impl(Query *query,
						tree_mutator_callback mutator,
						void *context,
						int flags)
{
	Assert(query != NULL && IsA(query, Query));

	if (!(flags & QTW_DONT_COPY_QUERY))
	{
		Query	   *newquery;

		FLATCOPY(newquery, query, Query);
		query = newquery;
	}

	MUTATE(query->targetList, query->targetList, List *);
	MUTATE(query->withCheckOptions, query->withCheckOptions, List *);
	MUTATE(query->onConflict, query->onConflict, OnConflictExpr *);
	MUTATE(query->mergeActionList, query->mergeActionList, List *);
	MUTATE(query->returningList, query->returningList, List *);
	MUTATE(query->jointree, query->jointree, FromExpr *);
	MUTATE(query->setOperations, query->setOperations, Node *);
	MUTATE(query->havingQual, query->havingQual, Node *);
	MUTATE(query->limitOffset, query->limitOffset, Node *);
	MUTATE(query->limitCount, query->limitCount, Node *);

	/*
	 * 多数调用方对 SortGroupClause 节点不感兴趣，因为那些
	 * 并不包含实际表达式。不过它们确实包含 OIDs，
	 * 有些变换器可能会感兴趣。
	 */

	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		MUTATE(query->groupClause, query->groupClause, List *);
		MUTATE(query->windowClause, query->windowClause, List *);
		MUTATE(query->sortClause, query->sortClause, List *);
		MUTATE(query->distinctClause, query->distinctClause, List *);
	}
	else
	{
		/*
		 * 但即使我们对 SortGroupClause 节点不感兴趣，
		 * 也需要变换 WindowClause 节点下的表达式。
		 */
		List	   *resultlist;
		ListCell   *temp;

		resultlist = NIL;
		foreach(temp, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, temp);
			WindowClause *newnode;

			FLATCOPY(newnode, wc, WindowClause);
			MUTATE(newnode->startOffset, wc->startOffset, Node *);
			MUTATE(newnode->endOffset, wc->endOffset, Node *);
			MUTATE(newnode->runCondition, wc->runCondition, List *);

			resultlist = lappend(resultlist, (Node *) newnode);
		}
		query->windowClause = resultlist;
	}

	/*
	 * 不变换 groupingSets 和 rowMarks：
	 *
	 * groupingSets 只包含 ressortgroup 引用（整数），若没有
	 * groupClause 或 tlist 就没有意义。因此，任何
	 * 需要关心它们的变换器都得在自己的 Query
	 * 处理中自行处理。
	 *
	 * rowMarks 只包含范围表下标（以及标志等），
	 * 因此同样应在 Query 层处理。
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
		MUTATE(query->cteList, query->cteList, List *);
	else						/* 否则按原样复制 CTE 列表 */
		query->cteList = copyObject(query->cteList);
	query->rtable = range_table_mutator(query->rtable,
										mutator, context, flags);
	return query;
}

/*
 * range_table_mutator 只是 query_tree_mutator 中处理
 * 查询范围表的那一部分。把它拆出来是因为它
 * 单独也有用。
 */
List *
range_table_mutator_impl(List *rtable,
						 tree_mutator_callback mutator,
						 void *context,
						 int flags)
{
	List	   *newrt = NIL;
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
		RangeTblEntry *newrte;

		FLATCOPY(newrte, rte, RangeTblEntry);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				MUTATE(newrte->tablesample, rte->tablesample,
					   TableSampleClause *);
				/* 我们没有费心复制 eref、别名等；可以吗？ */
				break;
			case RTE_SUBQUERY:
				if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
					MUTATE(newrte->subquery, rte->subquery, Query *);
				else
				{
					/* 否则按原样复制 RT 子查询 */
					newrte->subquery = copyObject(rte->subquery);
				}
				break;
			case RTE_JOIN:
				if (!(flags & QTW_IGNORE_JOINALIASES))
					MUTATE(newrte->joinaliasvars, rte->joinaliasvars, List *);
				else
				{
					/* 否则按原样复制连接别名 */
					newrte->joinaliasvars = copyObject(rte->joinaliasvars);
				}
				break;
			case RTE_FUNCTION:
				MUTATE(newrte->functions, rte->functions, List *);
				break;
			case RTE_TABLEFUNC:
				MUTATE(newrte->tablefunc, rte->tablefunc, TableFunc *);
				break;
			case RTE_VALUES:
				MUTATE(newrte->values_lists, rte->values_lists, List *);
				break;
			case RTE_CTE:
			case RTE_NAMEDTUPLESTORE:
			case RTE_RESULT:
				/* 无事可做 */
				break;
		}
		MUTATE(newrte->securityQuals, rte->securityQuals, List *);
		newrt = lappend(newrt, newrte);
	}
	return newrt;
}

/*
 * query_or_expression_tree_walker --- 混合形式
 *
 * 若在 Query 节点上调用，本例程会调用 query_tree_walker，
 * 否则直接调用遍历例程。当遍历例程通常的状态变化
 * 不适合最外层 Query 节点时，
 * 这是启动递归的有用方式。
 */
bool
query_or_expression_tree_walker_impl(Node *node,
									 tree_walker_callback walker,
									 void *context,
									 int flags)
{
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 walker,
								 context,
								 flags);
	else
		return WALK(node);
}

/*
 * query_or_expression_tree_mutator --- 混合形式
 *
 * 若在 Query 节点上调用，本例程会调用 query_tree_mutator，
 * 否则直接调用变换器。当变换器通常的状态变化
 * 不适合最外层 Query 节点时，
 * 这是启动递归的有用方式。
 */
Node *
query_or_expression_tree_mutator_impl(Node *node,
									  tree_mutator_callback mutator,
									  void *context,
									  int flags)
{
	if (node && IsA(node, Query))
		return (Node *) query_tree_mutator((Query *) node,
										   mutator,
										   context,
										   flags);
	else
		return mutator(node, context);
}


/*
 * raw_expression_tree_walker --- 遍历原始分析树
 *
 * 它的 API 与 expression_tree_walker 完全相同，但遍历的不是
 * 分析之后的分析树，而是知道如何遍历原始语法输出中的
 * 节点类型。（目前不需要合并的遍历器，
 * 因此为了效率把它们分开。）
 * 与 expression_tree_walker 不同，对查询
 * 边界没有特殊规则：凡是可能有意思的东西我们都下降进去。
 *
 * 目前这里覆盖的节点类型只到 DML 语句
 * （SELECT/INSERT/UPDATE/DELETE/MERGE）以及能出现在其中的节点，
 * 因为这主要用于分析 CTEs，而只有 DML
 * 语句能出现在 CTEs 中。
 */
bool
raw_expression_tree_walker_impl(Node *node,
								tree_walker_callback walker,
								void *context)
{
	ListCell   *temp;

	/*
	 * 遍历例程已经访问过当前节点，因此我们只需
	 * 递归进入它拥有的任何子节点。
	 */
	if (node == NULL)
		return false;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_JsonFormat:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_SQLValueFunction:
		case T_Integer:
		case T_Float:
		case T_Boolean:
		case T_String:
		case T_BitString:
		case T_ParamRef:
		case T_A_Const:
		case T_A_Star:
			/* 没有子节点的原始节点类型 */
			break;
		case T_Alias:
			/* 我们假定 colnames 列表不值得关注 */
			break;
		case T_RangeVar:
			return WALK(((RangeVar *) node)->alias);
		case T_GroupingFunc:
			return WALK(((GroupingFunc *) node)->args);
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (WALK(sublink->testexpr))
					return true;
				/* 我们假定 operName 不值得关注 */
				if (WALK(sublink->subselect))
					return true;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (WALK(caseexpr->arg))
					return true;
				/* 我们假定遍历例程也不关心 CaseWhens */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (WALK(when->expr))
						return true;
					if (WALK(when->result))
						return true;
				}
				if (WALK(caseexpr->defresult))
					return true;
			}
			break;
		case T_RowExpr:
			/* 假定 colnames 不值得关注 */
			return WALK(((RowExpr *) node)->args);
		case T_CoalesceExpr:
			return WALK(((CoalesceExpr *) node)->args);
		case T_MinMaxExpr:
			return WALK(((MinMaxExpr *) node)->args);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (WALK(xexpr->named_args))
					return true;
				/* 我们假定遍历例程不关心 arg_names */
				if (WALK(xexpr->args))
					return true;
			}
			break;
		case T_JsonReturning:
			return WALK(((JsonReturning *) node)->format);
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				if (WALK(jve->raw_expr))
					return true;
				if (WALK(jve->formatted_expr))
					return true;
				if (WALK(jve->format))
					return true;
			}
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;

				if (WALK(ctor->args))
					return true;
				if (WALK(ctor->func))
					return true;
				if (WALK(ctor->coercion))
					return true;
				if (WALK(ctor->returning))
					return true;
			}
			break;
		case T_JsonIsPredicate:
			return WALK(((JsonIsPredicate *) node)->expr);
		case T_NullTest:
			return WALK(((NullTest *) node)->arg);
		case T_BooleanTest:
			return WALK(((BooleanTest *) node)->arg);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (WALK(join->larg))
					return true;
				if (WALK(join->rarg))
					return true;
				if (WALK(join->quals))
					return true;
				if (WALK(join->alias))
					return true;
				/* using 列表被视为不值得关注 */
			}
			break;
		case T_IntoClause:
			{
				IntoClause *into = (IntoClause *) node;

				if (WALK(into->rel))
					return true;
				/* colNames、options 被视为不值得关注 */
				/* 在原始分析树中 viewQuery 应当为 null，但仍检查它 */
				if (WALK(into->viewQuery))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (WALK((Node *) lfirst(temp)))
					return true;
			}
			break;
		case T_InsertStmt:
			{
				InsertStmt *stmt = (InsertStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->cols))
					return true;
				if (WALK(stmt->selectStmt))
					return true;
				if (WALK(stmt->onConflictClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_DeleteStmt:
			{
				DeleteStmt *stmt = (DeleteStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->usingClause))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_UpdateStmt:
			{
				UpdateStmt *stmt = (UpdateStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->fromClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_MergeStmt:
			{
				MergeStmt  *stmt = (MergeStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->sourceRelation))
					return true;
				if (WALK(stmt->joinCondition))
					return true;
				if (WALK(stmt->mergeWhenClauses))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_MergeWhenClause:
			{
				MergeWhenClause *mergeWhenClause = (MergeWhenClause *) node;

				if (WALK(mergeWhenClause->condition))
					return true;
				if (WALK(mergeWhenClause->targetList))
					return true;
				if (WALK(mergeWhenClause->values))
					return true;
			}
			break;
		case T_SelectStmt:
			{
				SelectStmt *stmt = (SelectStmt *) node;

				if (WALK(stmt->distinctClause))
					return true;
				if (WALK(stmt->intoClause))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->fromClause))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->groupClause))
					return true;
				if (WALK(stmt->havingClause))
					return true;
				if (WALK(stmt->windowClause))
					return true;
				if (WALK(stmt->valuesLists))
					return true;
				if (WALK(stmt->sortClause))
					return true;
				if (WALK(stmt->limitOffset))
					return true;
				if (WALK(stmt->limitCount))
					return true;
				if (WALK(stmt->lockingClause))
					return true;
				if (WALK(stmt->withClause))
					return true;
				if (WALK(stmt->larg))
					return true;
				if (WALK(stmt->rarg))
					return true;
			}
			break;
		case T_PLAssignStmt:
			{
				PLAssignStmt *stmt = (PLAssignStmt *) node;

				if (WALK(stmt->indirection))
					return true;
				if (WALK(stmt->val))
					return true;
			}
			break;
		case T_A_Expr:
			{
				A_Expr	   *expr = (A_Expr *) node;

				if (WALK(expr->lexpr))
					return true;
				if (WALK(expr->rexpr))
					return true;
				/* 操作符名被视为不值得关注 */
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (WALK(expr->args))
					return true;
			}
			break;
		case T_ColumnRef:
			/* 我们假定这些字段不含值得关注的内容 */
			break;
		case T_FuncCall:
			{
				FuncCall   *fcall = (FuncCall *) node;

				if (WALK(fcall->args))
					return true;
				if (WALK(fcall->agg_order))
					return true;
				if (WALK(fcall->agg_filter))
					return true;
				if (WALK(fcall->over))
					return true;
				/* 函数名被视为不值得关注 */
			}
			break;
		case T_NamedArgExpr:
			return WALK(((NamedArgExpr *) node)->arg);
		case T_A_Indices:
			{
				A_Indices  *indices = (A_Indices *) node;

				if (WALK(indices->lidx))
					return true;
				if (WALK(indices->uidx))
					return true;
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *indir = (A_Indirection *) node;

				if (WALK(indir->arg))
					return true;
				if (WALK(indir->indirection))
					return true;
			}
			break;
		case T_A_ArrayExpr:
			return WALK(((A_ArrayExpr *) node)->elements);
		case T_ResTarget:
			{
				ResTarget  *rt = (ResTarget *) node;

				if (WALK(rt->indirection))
					return true;
				if (WALK(rt->val))
					return true;
			}
			break;
		case T_MultiAssignRef:
			return WALK(((MultiAssignRef *) node)->source);
		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) node;

				if (WALK(tc->arg))
					return true;
				if (WALK(tc->typeName))
					return true;
			}
			break;
		case T_CollateClause:
			return WALK(((CollateClause *) node)->arg);
		case T_SortBy:
			return WALK(((SortBy *) node)->node);
		case T_WindowDef:
			{
				WindowDef  *wd = (WindowDef *) node;

				if (WALK(wd->partitionClause))
					return true;
				if (WALK(wd->orderClause))
					return true;
				if (WALK(wd->startOffset))
					return true;
				if (WALK(wd->endOffset))
					return true;
			}
			break;
		case T_RangeSubselect:
			{
				RangeSubselect *rs = (RangeSubselect *) node;

				if (WALK(rs->subquery))
					return true;
				if (WALK(rs->alias))
					return true;
			}
			break;
		case T_RangeFunction:
			{
				RangeFunction *rf = (RangeFunction *) node;

				if (WALK(rf->functions))
					return true;
				if (WALK(rf->alias))
					return true;
				if (WALK(rf->coldeflist))
					return true;
			}
			break;
		case T_RangeTableSample:
			{
				RangeTableSample *rts = (RangeTableSample *) node;

				if (WALK(rts->relation))
					return true;
				/* 方法名被视为不值得关注 */
				if (WALK(rts->args))
					return true;
				if (WALK(rts->repeatable))
					return true;
			}
			break;
		case T_RangeTableFunc:
			{
				RangeTableFunc *rtf = (RangeTableFunc *) node;

				if (WALK(rtf->docexpr))
					return true;
				if (WALK(rtf->rowexpr))
					return true;
				if (WALK(rtf->namespaces))
					return true;
				if (WALK(rtf->columns))
					return true;
				if (WALK(rtf->alias))
					return true;
			}
			break;
		case T_RangeTableFuncCol:
			{
				RangeTableFuncCol *rtfc = (RangeTableFuncCol *) node;

				if (WALK(rtfc->colexpr))
					return true;
				if (WALK(rtfc->coldefexpr))
					return true;
			}
			break;
		case T_TypeName:
			{
				TypeName   *tn = (TypeName *) node;

				if (WALK(tn->typmods))
					return true;
				if (WALK(tn->arrayBounds))
					return true;
				/* 类型名本身被视为不值得关注 */
			}
			break;
		case T_ColumnDef:
			{
				ColumnDef  *coldef = (ColumnDef *) node;

				if (WALK(coldef->typeName))
					return true;
				if (WALK(coldef->raw_default))
					return true;
				if (WALK(coldef->collClause))
					return true;
				/* 目前忽略约束 */
			}
			break;
		case T_IndexElem:
			{
				IndexElem  *indelem = (IndexElem *) node;

				if (WALK(indelem->expr))
					return true;
				/* 排序规则名和操作符类名被视为不值得关注 */
			}
			break;
		case T_GroupingSet:
			return WALK(((GroupingSet *) node)->content);
		case T_LockingClause:
			return WALK(((LockingClause *) node)->lockedRels);
		case T_XmlSerialize:
			{
				XmlSerialize *xs = (XmlSerialize *) node;

				if (WALK(xs->expr))
					return true;
				if (WALK(xs->typeName))
					return true;
			}
			break;
		case T_WithClause:
			return WALK(((WithClause *) node)->ctes);
		case T_InferClause:
			{
				InferClause *stmt = (InferClause *) node;

				if (WALK(stmt->indexElems))
					return true;
				if (WALK(stmt->whereClause))
					return true;
			}
			break;
		case T_OnConflictClause:
			{
				OnConflictClause *stmt = (OnConflictClause *) node;

				if (WALK(stmt->infer))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->whereClause))
					return true;
			}
			break;
		case T_CommonTableExpr:
			/* search_clause 和 cycle_clause 在这里不值得关注 */
			return WALK(((CommonTableExpr *) node)->ctequery);
		case T_JsonOutput:
			{
				JsonOutput *out = (JsonOutput *) node;

				if (WALK(out->typeName))
					return true;
				if (WALK(out->returning))
					return true;
			}
			break;
		case T_JsonKeyValue:
			{
				JsonKeyValue *jkv = (JsonKeyValue *) node;

				if (WALK(jkv->key))
					return true;
				if (WALK(jkv->value))
					return true;
			}
			break;
		case T_JsonObjectConstructor:
			{
				JsonObjectConstructor *joc = (JsonObjectConstructor *) node;

				if (WALK(joc->output))
					return true;
				if (WALK(joc->exprs))
					return true;
			}
			break;
		case T_JsonArrayConstructor:
			{
				JsonArrayConstructor *jac = (JsonArrayConstructor *) node;

				if (WALK(jac->output))
					return true;
				if (WALK(jac->exprs))
					return true;
			}
			break;
		case T_JsonAggConstructor:
			{
				JsonAggConstructor *ctor = (JsonAggConstructor *) node;

				if (WALK(ctor->output))
					return true;
				if (WALK(ctor->agg_order))
					return true;
				if (WALK(ctor->agg_filter))
					return true;
				if (WALK(ctor->over))
					return true;
			}
			break;
		case T_JsonObjectAgg:
			{
				JsonObjectAgg *joa = (JsonObjectAgg *) node;

				if (WALK(joa->constructor))
					return true;
				if (WALK(joa->arg))
					return true;
			}
			break;
		case T_JsonArrayAgg:
			{
				JsonArrayAgg *jaa = (JsonArrayAgg *) node;

				if (WALK(jaa->constructor))
					return true;
				if (WALK(jaa->arg))
					return true;
			}
			break;
		case T_JsonArrayQueryConstructor:
			{
				JsonArrayQueryConstructor *jaqc = (JsonArrayQueryConstructor *) node;

				if (WALK(jaqc->output))
					return true;
				if (WALK(jaqc->query))
					return true;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
}

/*
 * planstate_tree_walker --- 遍历计划状态树
 *
 * 遍历例程已经访问过当前节点，因此我们只需
 * 递归进入它拥有的任何子节点。
 */
bool
planstate_tree_walker_impl(PlanState *planstate,
						   planstate_tree_walker_callback walker,
						   void *context)
{
	Plan	   *plan = planstate->plan;
	ListCell   *lc;

	/* 这里不需要到 Node 的隐式强制转换 */
#define PSWALK(n) walker(n, context)

	/* 防止过于复杂的计划树导致栈溢出 */
	check_stack_depth();

	/* initPlan 们 */
	if (planstate_walk_subplans(planstate->initPlan, walker, context))
		return true;

	/* lefttree */
	if (outerPlanState(planstate))
	{
		if (PSWALK(outerPlanState(planstate)))
			return true;
	}

	/* righttree */
	if (innerPlanState(planstate))
	{
		if (PSWALK(innerPlanState(planstate)))
			return true;
	}

	/* 特殊的子计划 */
	switch (nodeTag(plan))
	{
		case T_Append:
			if (planstate_walk_members(((AppendState *) planstate)->appendplans,
									   ((AppendState *) planstate)->as_nplans,
									   walker, context))
				return true;
			break;
		case T_MergeAppend:
			if (planstate_walk_members(((MergeAppendState *) planstate)->mergeplans,
									   ((MergeAppendState *) planstate)->ms_nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapAnd:
			if (planstate_walk_members(((BitmapAndState *) planstate)->bitmapplans,
									   ((BitmapAndState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapOr:
			if (planstate_walk_members(((BitmapOrState *) planstate)->bitmapplans,
									   ((BitmapOrState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_SubqueryScan:
			if (PSWALK(((SubqueryScanState *) planstate)->subplan))
				return true;
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScanState *) planstate)->custom_ps)
			{
				if (PSWALK(lfirst(lc)))
					return true;
			}
			break;
		default:
			break;
	}

	/* subPlan 们 */
	if (planstate_walk_subplans(planstate->subPlan, walker, context))
		return true;

	return false;
}

/*
 * 遍历 SubPlans 列表（或同样使用 SubPlan 节点的 initPlans）。
 */
static bool
planstate_walk_subplans(List *plans,
						planstate_tree_walker_callback walker,
						void *context)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		if (PSWALK(sps->planstate))
			return true;
	}

	return false;
}

/*
 * 遍历 ModifyTable、Append、MergeAppend、
 * BitmapAnd 或 BitmapOr 节点的组成计划。
 */
static bool
planstate_walk_members(PlanState **planstates, int nplans,
					   planstate_tree_walker_callback walker,
					   void *context)
{
	int			j;

	for (j = 0; j < nplans; j++)
	{
		if (PSWALK(planstates[j]))
			return true;
	}

	return false;
}
