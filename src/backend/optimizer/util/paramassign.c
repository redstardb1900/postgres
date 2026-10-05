/*-------------------------------------------------------------------------
 *
 * paramassign.c
 *		规划期间分配 PARAM_EXEC 槽的函数。
 *
 * 本模块负责管理规划器的三个数据结构：
 *
 * root->glob->paramExecTypes：记录 PARAM_EXEC 槽的实际分配。
 * 第 i 个列表元素保存第 i 个参数槽的数据类型 OID。
 * （若元素为 InvalidOid，表示该槽只为
 * chgParam 发信号所需，运行时永远不会持有值。）该列表对
 * 整个计划是全局的，因为执行器只有一个 PARAM_EXEC 数组。
 * 分配对计划是永久的：一旦加入就不再移除。
 *
 * root->plan_params：PlannerParamItem 节点列表，记录 Var 与
 * 根查询层需要提供给下层子查询的 PlaceHolderVars，
 * 以及每个这种值要使用的 PARAM_EXEC 编号。
 * 规划子查询时把元素加入此列表，每个子查询
 * 完成后把列表重置为空。
 *
 * root->curOuterParams：NestLoopParam 节点列表，记录某些外层嵌套循环
 * 需要下传到其右侧下层计划节点的 Var 与 PlaceHolderVars。
 * createplan.c 创建需要这种 Param 的下层 Plan 节点时，把元素加入此
 * 列表，并在它创建将提供这些
 * 值的 NestLoop Plan 节点时
 * 移除这些元素。
 *
 * 后两个数据结构用来防止创建多个
 * PARAM_EXEC 槽（每个都要费工夫去填充）：同一个上层
 * SubPlan 或 NestLoop 提供的值，若在其子计划节点里
 * 被引用了不止一处，就不要各建一个槽。不过，当同一个 Var 必须
 * 由不同的 SubPlan 或 NestLoop 父节点提供给不同的子计划树时，
 * 我们不认定它们有共性；会新建一条 plan_params 或
 * curOuterParams 项（因为处理完先前的 SubPlan 或 NestLoop 时，
 * 旧项已被移除），并分配一个新的
 * PARAM_EXEC 编号。我们曾经试图在这种情况下避免
 * 分配重复的 PARAM_EXEC 编号，但要避开因 Param 生存期重叠而出的错，
 * 比看起来更难，因此我们不再冒这个险。
 * 尽量减少 PARAM_EXEC 槽的数目，
 * 反正也省不了多少执行器的工作。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/paramassign.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/paramassign.h"
#include "optimizer/placeholder.h"
#include "rewrite/rewriteManip.h"


/*
 * 选择一个 PARAM_EXEC 编号，把给定 Var 标识为
 * 当前子查询的参数。（它也许已经有一个。）
 * 在合适的上层 root->plan_params 中登记对该 Var 的需求。
 */
static int
assign_param_for_var(PlannerInfo *root, Var *var)
{
	ListCell   *ppl;
	PlannerParamItem *pitem;
	Index		levelsup;

	/* 找出该 Var 所属的查询层 */
	for (levelsup = var->varlevelsup; levelsup > 0; levelsup--)
		root = root->parent_root;

	/* 若那里已有匹配的 PlannerParamItem，直接用它 */
	foreach(ppl, root->plan_params)
	{
		pitem = (PlannerParamItem *) lfirst(ppl);
		if (IsA(pitem->item, Var))
		{
			Var		   *pvar = (Var *) pitem->item;

			/*
			 * 这次比较必须与 _equalVar() 一致，只是忽略
			 * varlevelsup。注意 _equalVar() 忽略 varnosyn、
			 * varattnosyn 和 location，因此这里也忽略。
			 */
			if (pvar->varno == var->varno &&
				pvar->varattno == var->varattno &&
				pvar->vartype == var->vartype &&
				pvar->vartypmod == var->vartypmod &&
				pvar->varcollid == var->varcollid &&
				bms_equal(pvar->varnullingrels, var->varnullingrels))
				return pitem->paramId;
		}
	}

	/* 没有，因此新建一个 */
	var = copyObject(var);
	var->varlevelsup = 0;

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) var;
	pitem->paramId = list_length(root->glob->paramExecTypes);
	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 var->vartype);

	root->plan_params = lappend(root->plan_params, pitem);

	return pitem->paramId;
}

/*
 * 生成一个 Param 节点来替换给定的 Var，
 * 该 Var 预期 varlevelsup > 0（即它不是局部的）。
 * 在合适的上层 root->plan_params 中登记对该 Var 的需求。
 */
Param *
replace_outer_var(PlannerInfo *root, Var *var)
{
	Param	   *retval;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < root->query_level);

	/* 在适当的 plan_params 中找到该 Var，若没有则加入 */
	i = assign_param_for_var(root, var);

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = var->vartype;
	retval->paramtypmod = var->vartypmod;
	retval->paramcollid = var->varcollid;
	retval->location = var->location;

	return retval;
}

/*
 * 选择一个 PARAM_EXEC 编号，把给定 PlaceHolderVar 标识为
 * 当前子查询的参数。（它也许已经有一个。）
 * 在合适的上层 root->plan_params 中登记对该 PHV 的需求。
 *
 * 这与 assign_param_for_var 一样，只是针对 PlaceHolderVars。
 */
static int
assign_param_for_placeholdervar(PlannerInfo *root, PlaceHolderVar *phv)
{
	ListCell   *ppl;
	PlannerParamItem *pitem;
	Index		levelsup;

	/* 找出该 PHV 所属的查询层 */
	for (levelsup = phv->phlevelsup; levelsup > 0; levelsup--)
		root = root->parent_root;

	/* 若那里已有匹配的 PlannerParamItem，直接用它 */
	foreach(ppl, root->plan_params)
	{
		pitem = (PlannerParamItem *) lfirst(ppl);
		if (IsA(pitem->item, PlaceHolderVar))
		{
			PlaceHolderVar *pphv = (PlaceHolderVar *) pitem->item;

			/* 我们假定比较 PHIDs 就足够了 */
			if (pphv->phid == phv->phid)
				return pitem->paramId;
		}
	}

	/* 没有，因此新建一个 */
	phv = copyObject(phv);
	IncrementVarSublevelsUp((Node *) phv, -((int) phv->phlevelsup), 0);
	Assert(phv->phlevelsup == 0);

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) phv;
	pitem->paramId = list_length(root->glob->paramExecTypes);
	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 exprType((Node *) phv->phexpr));

	root->plan_params = lappend(root->plan_params, pitem);

	return pitem->paramId;
}

/*
 * 生成一个 Param 节点来替换给定的 PlaceHolderVar，
 * 该 PlaceHolderVar 预期 phlevelsup > 0（即它不是局部的）。
 * 在合适的上层 root->plan_params 中登记对该 PHV 的需求。
 *
 * 这与 replace_outer_var 一样，只是针对 PlaceHolderVars。
 */
Param *
replace_outer_placeholdervar(PlannerInfo *root, PlaceHolderVar *phv)
{
	Param	   *retval;
	int			i;

	Assert(phv->phlevelsup > 0 && phv->phlevelsup < root->query_level);

	/* 在适当的 plan_params 中找到该 PHV，若没有则加入 */
	i = assign_param_for_placeholdervar(root, phv);

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = exprType((Node *) phv->phexpr);
	retval->paramtypmod = exprTypmod((Node *) phv->phexpr);
	retval->paramcollid = exprCollation((Node *) phv->phexpr);
	retval->location = -1;

	return retval;
}

/*
 * 生成一个 Param 节点来替换给定的 Aggref，
 * 该 Aggref 预期 agglevelsup > 0（即它不是局部的）。
 * 在合适的上层 root->plan_params 中登记对该 Aggref 的需求。
 */
Param *
replace_outer_agg(PlannerInfo *root, Aggref *agg)
{
	Param	   *retval;
	PlannerParamItem *pitem;
	Index		levelsup;

	Assert(agg->agglevelsup > 0 && agg->agglevelsup < root->query_level);

	/* 找出该 Aggref 所属的查询层 */
	for (levelsup = agg->agglevelsup; levelsup > 0; levelsup--)
		root = root->parent_root;

	/*
	 * 试图对外部聚集的引用去重似乎不值得。
	 * 每次都新开一个槽。
	 */
	agg = copyObject(agg);
	IncrementVarSublevelsUp((Node *) agg, -((int) agg->agglevelsup), 0);
	Assert(agg->agglevelsup == 0);

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) agg;
	pitem->paramId = list_length(root->glob->paramExecTypes);
	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 agg->aggtype);

	root->plan_params = lappend(root->plan_params, pitem);

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = pitem->paramId;
	retval->paramtype = agg->aggtype;
	retval->paramtypmod = -1;
	retval->paramcollid = agg->aggcollid;
	retval->location = agg->location;

	return retval;
}

/*
 * 生成一个 Param 节点来替换给定的 GroupingFunc 表达式，该表达式
 * 预期 agglevelsup > 0（即它不是局部的）。
 * 在合适的上层 root->plan_params 中登记对该 GroupingFunc 的
 * 需求。
 */
Param *
replace_outer_grouping(PlannerInfo *root, GroupingFunc *grp)
{
	Param	   *retval;
	PlannerParamItem *pitem;
	Index		levelsup;
	Oid			ptype = exprType((Node *) grp);

	Assert(grp->agglevelsup > 0 && grp->agglevelsup < root->query_level);

	/* 找出该 GroupingFunc 所属的查询层 */
	for (levelsup = grp->agglevelsup; levelsup > 0; levelsup--)
		root = root->parent_root;

	/*
	 * 试图对外部聚集的引用去重似乎不值得。
	 * 每次都新开一个槽。
	 */
	grp = copyObject(grp);
	IncrementVarSublevelsUp((Node *) grp, -((int) grp->agglevelsup), 0);
	Assert(grp->agglevelsup == 0);

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) grp;
	pitem->paramId = list_length(root->glob->paramExecTypes);
	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 ptype);

	root->plan_params = lappend(root->plan_params, pitem);

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = pitem->paramId;
	retval->paramtype = ptype;
	retval->paramtypmod = -1;
	retval->paramcollid = InvalidOid;
	retval->location = grp->location;

	return retval;
}

/*
 * 生成一个 Param 节点来替换给定的 Var，
 * 该 Var 预期来自某个上层 NestLoop 计划节点。
 * 在 root->curOuterParams 中登记对该 Var 的需求。
 */
Param *
replace_nestloop_param_var(PlannerInfo *root, Var *var)
{
	Param	   *param;
	NestLoopParam *nlp;
	ListCell   *lc;

	/* 这个 Var 是否已列在 root->curOuterParams 中？ */
	foreach(lc, root->curOuterParams)
	{
		nlp = (NestLoopParam *) lfirst(lc);
		if (equal(var, nlp->paramval))
		{
			/* 是，因此只做一个引用该 NLP 槽的 Param */
			param = makeNode(Param);
			param->paramkind = PARAM_EXEC;
			param->paramid = nlp->paramno;
			param->paramtype = var->vartype;
			param->paramtypmod = var->vartypmod;
			param->paramcollid = var->varcollid;
			param->location = var->location;
			return param;
		}
	}

	/* 否，因此为新的 NLP 分配一个 PARAM_EXEC 槽 */
	param = generate_new_exec_param(root,
									var->vartype,
									var->vartypmod,
									var->varcollid);
	param->location = var->location;

	/* 把它加入所需 NLPs 的列表 */
	nlp = makeNode(NestLoopParam);
	nlp->paramno = param->paramid;
	nlp->paramval = copyObject(var);
	root->curOuterParams = lappend(root->curOuterParams, nlp);

	/* 并返回替换用的 Param */
	return param;
}

/*
 * 生成一个 Param 节点来替换给定的 PlaceHolderVar，
 * 该 PlaceHolderVar 预期来自某个上层 NestLoop 计划节点。
 * 在 root->curOuterParams 中登记对该 PHV 的需求。
 *
 * 这与 replace_nestloop_param_var 一样，只是针对 PlaceHolderVars。
 */
Param *
replace_nestloop_param_placeholdervar(PlannerInfo *root, PlaceHolderVar *phv)
{
	Param	   *param;
	NestLoopParam *nlp;
	ListCell   *lc;

	/* 这个 PHV 是否已列在 root->curOuterParams 中？ */
	foreach(lc, root->curOuterParams)
	{
		nlp = (NestLoopParam *) lfirst(lc);
		if (equal(phv, nlp->paramval))
		{
			/* 是，因此只做一个引用该 NLP 槽的 Param */
			param = makeNode(Param);
			param->paramkind = PARAM_EXEC;
			param->paramid = nlp->paramno;
			param->paramtype = exprType((Node *) phv->phexpr);
			param->paramtypmod = exprTypmod((Node *) phv->phexpr);
			param->paramcollid = exprCollation((Node *) phv->phexpr);
			param->location = -1;
			return param;
		}
	}

	/* 否，因此为新的 NLP 分配一个 PARAM_EXEC 槽 */
	param = generate_new_exec_param(root,
									exprType((Node *) phv->phexpr),
									exprTypmod((Node *) phv->phexpr),
									exprCollation((Node *) phv->phexpr));

	/* 把它加入所需 NLPs 的列表 */
	nlp = makeNode(NestLoopParam);
	nlp->paramno = param->paramid;
	nlp->paramval = (Var *) copyObject(phv);
	root->curOuterParams = lappend(root->curOuterParams, nlp);

	/* 并返回替换用的 Param */
	return param;
}

/*
 * process_subquery_nestloop_params
 *	  处理参数化子查询中需要由外层嵌套循环提供的参数。
 *	  目前，那就是 FROM 中的子查询向当前查询层索取的全部参数，
 *
 * 因为它们必须是 LATERAL 引用。
 * subplan_params 是我们打算传给
 *
 * FROM 中子查询的 PlannerParamItems 列表。（它是在规划该子查询时
 * 在 root->plan_params 里建的，但调用到这里时已经不在那里了。）
 * 子计划对外部变量的引用已经表示成
 *
 * PARAM_EXEC Param，因为上面的例程在规划该子查询时
 * 已经做了转换。因此不必修改子计划或这里的
 * PlannerParamItems。需要做的是向
 * root->curOuterParams 添加项，通知父嵌套循环计划节点必须
 * 提供这些值。这与 replace_nestloop_param_var 的不同
 * 在于要用的 PARAM_EXEC 槽已经确定。
 * 注意我们也把 root->curOuterRels
 *
 * 当作隐式参数，
 * 用于健全性检查。
 */
void
process_subquery_nestloop_params(PlannerInfo *root, List *subplan_params)
{
	ListCell   *lc;

	foreach(lc, subplan_params)
	{
		PlannerParamItem *pitem = lfirst_node(PlannerParamItem, lc);

		if (IsA(pitem->item, Var))
		{
			Var		   *var = (Var *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc2;

			/* 若不是来自嵌套循环的外侧关系，则报错 */
			if (!bms_is_member(var->varno, root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");

			/* 这个参数是否已列在 root->curOuterParams 中？ */
			foreach(lc2, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc2);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(var, nlp->paramval));
					/* 已经存在，因此无需处理 */
					break;
				}
			}
			if (lc2 == NULL)
			{
				/* 否，因此加入 */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = copyObject(var);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else if (IsA(pitem->item, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc2;

			/* 若不是来自嵌套循环的外侧关系，则报错 */
			if (!bms_is_subset(find_placeholder_info(root, phv)->ph_eval_at,
							   root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");

			/* 这个参数是否已列在 root->curOuterParams 中？ */
			foreach(lc2, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc2);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(phv, nlp->paramval));
					/* 已经存在，因此无需处理 */
					break;
				}
			}
			if (lc2 == NULL)
			{
				/* 否，因此加入 */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = (Var *) copyObject(phv);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else
			elog(ERROR, "unexpected type of subquery parameter");
	}
}

/*
 * 识别应由指定左侧关系的 NestLoop 计划
 * 节点提供的任何 NestLoopParams。把它们从活动的
 * root->curOuterParams 列表中去掉，并作为结果列表返回。
 *
 * XXX 这里我们还改写返回的 Var 和 PHVs，使它们不含
 * 超出外侧所能提供的 nullingrel 集合。
 * 若我们应用了外连接恒等式 3，就需要这样做：
 *		(A leftjoin B on (Pab)) leftjoin C on (Pb*c)
 *		= A leftjoin (B leftjoin C on (Pbc)) on (Pab)
 * 并且 C 含有对 B 的 LATERAL 引用。应用该
 * 恒等式仍然安全，但解析器会把那些引用建成
 * "b*" 的形式（即 varnullingrels 列出 A/B 连接），而嵌套循环外侧
 * 实际能提供的只是 "b"。我们在这里把 nullingrels 削到
 * 按 leftrelids 从外侧所能提供的
 * 范围。
 *
 * 这修好了正向应用恒等式 3 的情形。
 * 若恒等式是反向应用的，参数 Var 含有的 nullingrel 位
 * 会过少而不是过多。
 * 目前这不会出问题，因为 setrefs.c 对 NestLoopParams 中的
 * nullingrels 只做子集检查，但若以后想收紧该检查，就得更费力。
 * 这相当烦人，
 * 因为它大大削弱了 setrefs.c 的交叉检查，但替代方案
 * 似乎是为每个带 LATERAL 参数化的
 * 子查询生成多个版本，那会过于昂贵。
 */
List *
identify_current_nestloop_params(PlannerInfo *root, Relids leftrelids)
{
	List	   *result;
	ListCell   *cell;

	result = NIL;
	foreach(cell, root->curOuterParams)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(cell);

		/*
		 * 我们要找可以由左侧关系提供的 Var 和 PHVs。
		 * 找到一个时，可以就地修改它，
		 * 因为上面所有例程都会做一份新副本放进
		 * curOuterParams。
		 */
		if (IsA(nlp->paramval, Var) &&
			bms_is_member(nlp->paramval->varno, leftrelids))
		{
			Var		   *var = (Var *) nlp->paramval;

			root->curOuterParams = foreach_delete_current(root->curOuterParams,
														  cell);
			var->varnullingrels = bms_intersect(var->varnullingrels,
												leftrelids);
			result = lappend(result, nlp);
		}
		else if (IsA(nlp->paramval, PlaceHolderVar) &&
				 bms_is_subset(find_placeholder_info(root,
													 (PlaceHolderVar *) nlp->paramval)->ph_eval_at,
							   leftrelids))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) nlp->paramval;

			root->curOuterParams = foreach_delete_current(root->curOuterParams,
														  cell);
			phv->phnullingrels = bms_intersect(phv->phnullingrels,
											   leftrelids);
			result = lappend(result, nlp);
		}
	}
	return result;
}

/*
 * 生成一个不会与任何其他 Param 冲突的新 Param 节点。
 *
 * 这用来创建表示子计划输出或
 * NestLoop 参数的 Param。
 *
 * 这种 Param 不必建立 PlannerParamItem，但必须
 * 确保在 paramExecTypes 中记下类型（否则
 * 不会为它分配槽）。
 */
Param *
generate_new_exec_param(PlannerInfo *root, Oid paramtype, int32 paramtypmod,
						Oid paramcollation)
{
	Param	   *retval;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = list_length(root->glob->paramExecTypes);
	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 paramtype);
	retval->paramtype = paramtype;
	retval->paramtypmod = paramtypmod;
	retval->paramcollid = paramcollation;
	retval->location = -1;

	return retval;
}

/*
 * 为一个特殊参数分配（非负的）PARAM_EXEC ID（这种参数
 * 实际上不在运行时携带值）。这类参数用于
 * 特殊的运行时信号，例如把递归并集节点连到它的工作表扫描节点，
 * 或在 EvalPlanQual 机制内强制重新
 * 对计划求值。不过并不存在
 * 带这个 ID 的实际 Param 节点。
 */
int
assign_special_exec_param(PlannerInfo *root)
{
	int			paramId = list_length(root->glob->paramExecTypes);

	root->glob->paramExecTypes = lappend_oid(root->glob->paramExecTypes,
											 InvalidOid);
	return paramId;
}
