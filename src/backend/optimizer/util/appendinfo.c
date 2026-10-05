/*-------------------------------------------------------------------------
 *
 * appendinfo.c
 *	  Routines for mapping between append parent(s) and children
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/appendinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "foreign/fdwapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	PlannerInfo *root;
	int			nappinfos;
	AppendRelInfo **appinfos;
} adjust_appendrel_attrs_context;

static void make_inh_translation_list(Relation oldrelation,
									  Relation newrelation,
									  Index newvarno,
									  AppendRelInfo *appinfo);
static Node *adjust_appendrel_attrs_mutator(Node *node,
											adjust_appendrel_attrs_context *context);


/*
 * make_append_rel_info
 *	  为父子对建立一个 AppendRelInfo
 */
AppendRelInfo *
make_append_rel_info(Relation parentrel, Relation childrel,
					 Index parentRTindex, Index childRTindex)
{
	AppendRelInfo *appinfo = makeNode(AppendRelInfo);

	appinfo->parent_relid = parentRTindex;
	appinfo->child_relid = childRTindex;
	appinfo->parent_reltype = parentrel->rd_rel->reltype;
	appinfo->child_reltype = childrel->rd_rel->reltype;
	make_inh_translation_list(parentrel, childrel, childRTindex, appinfo);
	appinfo->parent_reloid = RelationGetRelid(parentrel);

	return appinfo;
}

/*
 * make_inh_translation_list
 *	  为继承子表建立从父 Var 到子 Var 的翻译列表，
 *	  以及反向翻译数组。
 *
 * 反向翻译数组对每个子关系
 * 列有一项，要么是对应父
 * 列的从 1 起的下标，要么在没有匹配时为 0（已删除的子列会这样，
 * 超出父表的子列也会，传统继承允许这种情况，
 * 分区则不允许）。
 *
 * 为保险起见，我们同时匹配类型/排序规则和属性名。
 */
static void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno,
						  AppendRelInfo *appinfo)
{
	List	   *vars = NIL;
	AttrNumber *pcolnos;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	Oid			new_relid = RelationGetRelid(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;
	int			new_attno = 0;

	/* 把反向翻译数组的全部项初始化为零 */
	appinfo->num_child_cols = newnatts;
	appinfo->parent_colnos = pcolnos =
		(AttrNumber *) palloc0(newnatts * sizeof(AttrNumber));

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;

		att = TupleDescAttr(old_tupdesc, old_attno);
		if (att->attisdropped)
		{
			/* 这一列表项就放 NULL */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * 当我们为继承集的父表生成翻译列表时，
		 * 不必搜索匹配。
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			pcolnos[old_attno] = old_attno + 1;
			continue;
		}

		/*
		 * 否则必须按名搜索匹配的列。
		 * 不保证列位置相同，因为
		 * 有 ALTER TABLE ADD COLUMN 和多重继承之类的情况。
		 * 不过在简单情况下，两边关系的列相对顺序大体
		 * 相同，因此先试 newrelation 中紧接在
		 * 刚找到的那列之后的列，若失败
		 * 就交给系统缓存处理。
		 */
		if (new_attno >= newnatts ||
			(att = TupleDescAttr(new_tupdesc, new_attno))->attisdropped ||
			strcmp(attname, NameStr(att->attname)) != 0)
		{
			HeapTuple	newtup;

			newtup = SearchSysCacheAttName(new_relid, attname);
			if (!HeapTupleIsValid(newtup))
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
			new_attno = ((Form_pg_attribute) GETSTRUCT(newtup))->attnum - 1;
			Assert(new_attno >= 0 && new_attno < newnatts);
			ReleaseSysCache(newtup);

			att = TupleDescAttr(new_tupdesc, new_attno);
		}

		/* 找到了，检查类型和排序规则是否匹配 */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
				 attname, RelationGetRelationName(newrelation));
		if (attcollation != att->attcollation)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
				 attname, RelationGetRelationName(newrelation));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
		pcolnos[new_attno] = old_attno + 1;
		new_attno++;
	}

	appinfo->translated_vars = vars;
}

/*
 * adjust_appendrel_attrs
 *	  复制指定的查询或表达式，把引用父关系的 Var 翻译成
 *	  改为引用对应的子关系。我们也
 *	  更新出现在 Var 之外的 rtindex，例如 resultRelation 和
 *	  连接树 relid。
 *
 * 注意：这只在子链接已转换成子计划之后应用，
 * 因此不必应付递归进入子查询。
 *
 * 注意：这与 pullup_replace_vars() 所做的差别不算很大；
 * 也许应当试试把两个例程折到一起。
 */
Node *
adjust_appendrel_attrs(PlannerInfo *root, Node *node, int nappinfos,
					   AppendRelInfo **appinfos)
{
	adjust_appendrel_attrs_context context;

	context.root = root;
	context.nappinfos = nappinfos;
	context.appinfos = appinfos;

	/* 若没有什么要调整，不要调用本函数。 */
	Assert(nappinfos >= 1 && appinfos != NULL);

	/* 绝不应当翻译一棵 Query 树。 */
	Assert(node == NULL || !IsA(node, Query));

	return adjust_appendrel_attrs_mutator(node, &context);
}

static Node *
adjust_appendrel_attrs_mutator(Node *node,
							   adjust_appendrel_attrs_context *context)
{
	AppendRelInfo **appinfos = context->appinfos;
	int			nappinfos = context->nappinfos;
	int			cnt;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) copyObject(node);
		AppendRelInfo *appinfo = NULL;

		if (var->varlevelsup != 0)
			return (Node *) var;	/* 不需要改动 */

		/*
		 * 你也许以为需要调整 var->varnullingrels，但那
		 * 不应当需要任何改动。它会含有外连接 relid，
		 * 而我们做的变换只影响基关系。
		 * 下面我们只是把 var->varnullingrels 传播进翻译后的
		 * Var。
		 *
		 * 若 var->varnullingrels 非空，且翻译结果不会是
		 * Var，就只好失败。可以想象把翻译后的
		 * 表达式包进 PlaceHolderVar，但那行不通，因为这通常
		 * 用在冻结占位符之后。幸好这个情况
		 * 目前看来不可达。这里可以看到非空的
		 * var->varnullingrels，但只在涉及按分区
		 * 连接的情形，而那些情形的翻译总是 Var。
		 * （非 Var 翻译只出现在把
		 * UNION ALL 子查询展平而成的 appendrel。）若将来需要让这工作，
		 * 一个可能的修法是强制 prepjointree.c 为这种子查询的
		 * 所有非 Var 输出创建 PHVs，然后我们可以在这里查找
		 * 预先存在的 PHV。或者一开始就把翻译
		 * 那样包起来？
		 */

		for (cnt = 0; cnt < nappinfos; cnt++)
		{
			if (var->varno == appinfos[cnt]->parent_relid)
			{
				appinfo = appinfos[cnt];
				break;
			}
		}

		if (appinfo)
		{
			var->varno = appinfo->child_relid;
			/* 它现在是生成出来的 Var，因此丢掉任何语法标注 */
			var->varnosyn = 0;
			var->varattnosyn = 0;
			if (var->varattno > 0)
			{
				Node	   *newnode;

				if (var->varattno > list_length(appinfo->translated_vars))
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				newnode = copyObject(list_nth(appinfo->translated_vars,
											  var->varattno - 1));
				if (newnode == NULL)
					elog(ERROR, "attribute %d of relation \"%s\" does not exist",
						 var->varattno, get_rel_name(appinfo->parent_reloid));
				if (IsA(newnode, Var))
					((Var *) newnode)->varnullingrels = var->varnullingrels;
				else if (var->varnullingrels != NULL)
					elog(ERROR, "failed to apply nullingrels to a non-Var");
				return newnode;
			}
			else if (var->varattno == 0)
			{
				/*
				 * 整行 Var：若处理的是具名行类型，可以
				 * 对子表使用整行 Var，再加一步强制转换，
				 * 把元组布局转换成父表的行类型。
				 * 否则必须生成一个 RowExpr。
				 */
				if (OidIsValid(appinfo->child_reltype))
				{
					Assert(var->vartype == appinfo->parent_reltype);
					if (appinfo->parent_reltype != appinfo->child_reltype)
					{
						ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

						r->arg = (Expr *) var;
						r->resulttype = appinfo->parent_reltype;
						r->convertformat = COERCE_IMPLICIT_CAST;
						r->location = -1;
						/* 也要确保 Var 节点的类型 ID 正确 */
						var->vartype = appinfo->child_reltype;
						return (Node *) r;
					}
				}
				else
				{
					/*
					 * 建立一个包含翻译后变量的 RowExpr。
					 *
					 * 实践中 var->vartype 在这里总是 RECORDOID，
					 * 因此需要想出一些合适的列名。
					 * 我们用父 RTE 的列名。
					 *
					 * 注意：继承情形到不了这里，因此
					 * 不必担心 translated_vars 可能含有
					 * 一些哑 NULLs。
					 */
					RowExpr    *rowexpr;
					List	   *fields;
					RangeTblEntry *rte;

					rte = rt_fetch(appinfo->parent_relid,
								   context->root->parse->rtable);
					fields = copyObject(appinfo->translated_vars);
					rowexpr = makeNode(RowExpr);
					rowexpr->args = fields;
					rowexpr->row_typeid = var->vartype;
					rowexpr->row_format = COERCE_IMPLICIT_CAST;
					rowexpr->colnames = copyObject(rte->eref->colnames);
					rowexpr->location = -1;

					if (var->varnullingrels != NULL)
						elog(ERROR, "failed to apply nullingrels to a non-Var");

					return (Node *) rowexpr;
				}
			}
			/* 系统属性不需要任何其他翻译 */
		}
		else if (var->varno == ROWID_VAR)
		{
			/*
			 * 若它是 ROWID_VAR 占位符，看是否已到达叶子
			 * 目标关系，那样可以把该 Var 翻译成具体的
			 * 实例。绝不应当被要求翻译成含有不止一个叶子目标关系的
			 * relid 集合，因此
			 * 答案是唯一的。若仍在考虑非叶子
			 * 继承层，就把 ROWID_VAR Var 原样返回。
			 */
			Relids		leaf_result_relids = context->root->leaf_result_relids;
			Index		leaf_relid = 0;

			for (cnt = 0; cnt < nappinfos; cnt++)
			{
				if (bms_is_member(appinfos[cnt]->child_relid,
								  leaf_result_relids))
				{
					if (leaf_relid)
						elog(ERROR, "cannot translate to multiple leaf relids");
					leaf_relid = appinfos[cnt]->child_relid;
				}
			}

			if (leaf_relid)
			{
				RowIdentityVarInfo *ridinfo = (RowIdentityVarInfo *)
					list_nth(context->root->row_identity_vars, var->varattno - 1);

				if (bms_is_member(leaf_relid, ridinfo->rowidrels))
				{
					/* 代入 RowIdentityVarInfo 中给出的 Var */
					var = copyObject(ridinfo->rowidvar);
					/* ... 但使用正确的 relid */
					var->varno = leaf_relid;
					/* 标识 var 不应当有置空关系 */
					Assert(var->varnullingrels == NULL);
					/* RowIdentityVarInfo 中的 varnosyn 多半是错的 */
					var->varnosyn = 0;
					var->varattnosyn = 0;
				}
				else
				{
					/*
					 * 这个叶子关系不能返回所需的值，因此
					 * 代入一个类型正确的 NULL。
					 */
					return (Node *) makeNullConst(var->vartype,
												  var->vartypmod,
												  var->varcollid);
				}
			}
		}
		return (Node *) var;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		for (cnt = 0; cnt < nappinfos; cnt++)
		{
			AppendRelInfo *appinfo = appinfos[cnt];

			if (cexpr->cvarno == appinfo->parent_relid)
			{
				cexpr->cvarno = appinfo->child_relid;
				break;
			}
		}
		return (Node *) cexpr;
	}
	if (IsA(node, PlaceHolderVar))
	{
		/* 复制 PlaceHolderVar 节点，并正确变更子节点 */
		PlaceHolderVar *phv;

		phv = (PlaceHolderVar *) expression_tree_mutator(node,
														 adjust_appendrel_attrs_mutator,
														 (void *) context);
		/* 现在修正 PlaceHolderVar 的 relid 集合 */
		if (phv->phlevelsup == 0)
		{
			phv->phrels = adjust_child_relids(phv->phrels,
											  nappinfos, appinfos);
			/* 同上，不必碰 phnullingrels */
		}
		return (Node *) phv;
	}
	/* 这里应当不必处理规划器辅助节点 */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	/*
	 * 必须特别处理 RestrictInfo 节点。（注意：虽然
	 * set_append_rel_pathlist 会向我们隐藏父表
	 * baserestrictinfo 列表中的 RestrictInfos，但不会隐藏 joininfo 里的。）
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *oldinfo = (RestrictInfo *) node;
		RestrictInfo *newinfo = makeNode(RestrictInfo);

		/* 复制所有可平铺复制的字段，尤其包括 rinfo_serial */
		memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

		/* 递归修正子句本身 */
		newinfo->clause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->clause, context);

		/* 若是 OR 子句，也修正修改后的版本 */
		newinfo->orclause = (Expr *)
			adjust_appendrel_attrs_mutator((Node *) oldinfo->orclause, context);

		/* 也调整 relid 集合 */
		newinfo->clause_relids = adjust_child_relids(oldinfo->clause_relids,
													 context->nappinfos,
													 context->appinfos);
		newinfo->required_relids = adjust_child_relids(oldinfo->required_relids,
													   context->nappinfos,
													   context->appinfos);
		newinfo->outer_relids = adjust_child_relids(oldinfo->outer_relids,
													context->nappinfos,
													context->appinfos);
		newinfo->left_relids = adjust_child_relids(oldinfo->left_relids,
												   context->nappinfos,
												   context->appinfos);
		newinfo->right_relids = adjust_child_relids(oldinfo->right_relids,
													context->nappinfos,
													context->appinfos);

		/*
		 * 重置缓存的派生字段，因为考虑子关系时这些可能需要
		 * 不同的值。注意我们
		 * 不重置 left_ec/right_ec：每个子变量都隐含
		 * 等价于其父变量，因此若有等价类，仍是同一个等价类的成员。
		 */
		newinfo->eval_cost.startup = -1;
		newinfo->norm_selec = -1;
		newinfo->outer_selec = -1;
		newinfo->left_em = NULL;
		newinfo->right_em = NULL;
		newinfo->scansel_cache = NIL;
		newinfo->left_bucketsize = -1;
		newinfo->right_bucketsize = -1;
		newinfo->left_mcvfreq = -1;
		newinfo->right_mcvfreq = -1;

		return (Node *) newinfo;
	}

	/*
	 * 注意：不必递归进入子链接，因为在我们看到它们之前
	 * 它们应当已经转换成子计划。
	 */
	Assert(!IsA(node, SubLink));
	Assert(!IsA(node, Query));
	/* 也不应当看到这些 Query 子结构。 */
	Assert(!IsA(node, RangeTblRef));
	Assert(!IsA(node, JoinExpr));

	return expression_tree_mutator(node, adjust_appendrel_attrs_mutator,
								   (void *) context);
}

/*
 * adjust_appendrel_attrs_multilevel
 *	  把 Var 翻译从 appendrel 父项一直应用到子项。
 *
 * 把 node 表达式中引用 parentrel 的 Var 替换为
 * childrel 的适当 Var。childrel 可以比 parentrel
 * 低不止一个继承层。
 */
Node *
adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  RelOptInfo *childrel,
								  RelOptInfo *parentrel)
{
	AppendRelInfo **appinfos;
	int			nappinfos;

	/* 若直接父项不是顶层父项，则递归。 */
	if (childrel->parent != parentrel)
	{
		if (childrel->parent)
			node = adjust_appendrel_attrs_multilevel(root, node,
													 childrel->parent,
													 parentrel);
		else
			elog(ERROR, "childrel is not a child of parentrel");
	}

	/* 现在为这个子项做翻译。 */
	appinfos = find_appinfos_by_relids(root, childrel->relids, &nappinfos);

	node = adjust_appendrel_attrs(root, node, nappinfos, appinfos);

	pfree(appinfos);

	return node;
}

/*
 * 在 Relid 集合中用子 relid 替换父 relid。appinfo 数组
 * 指定要做的替换。
 */
Relids
adjust_child_relids(Relids relids, int nappinfos, AppendRelInfo **appinfos)
{
	Bitmapset  *result = NULL;
	int			cnt;

	for (cnt = 0; cnt < nappinfos; cnt++)
	{
		AppendRelInfo *appinfo = appinfos[cnt];

		/* 去掉父项，加入子项 */
		if (bms_is_member(appinfo->parent_relid, relids))
		{
			/* 若正在改变该集合，则做一份副本。 */
			if (!result)
				result = bms_copy(relids);

			result = bms_del_member(result, appinfo->parent_relid);
			result = bms_add_member(result, appinfo->child_relid);
		}
	}

	/* 若做了任何改动，返回修改后的副本。 */
	if (result)
		return result;

	/* 否则返回未修改的原集合。 */
	return relids;
}

/*
 * 在 Relid 集合中用子项的 relid 替换父项的 relid。
 * 子关系可以在父关系之下多个继承层。
 */
Relids
adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
							   RelOptInfo *childrel,
							   RelOptInfo *parentrel)
{
	AppendRelInfo **appinfos;
	int			nappinfos;

	/*
	 * 若给定的 relid 集合不含任何父 relid，它
	 * 将保持不变。
	 */
	if (!bms_overlap(relids, parentrel->relids))
		return relids;

	/* 若直接父项不是顶层父项，则递归。 */
	if (childrel->parent != parentrel)
	{
		if (childrel->parent)
			relids = adjust_child_relids_multilevel(root, relids,
													childrel->parent,
													parentrel);
		else
			elog(ERROR, "childrel is not a child of parentrel");
	}

	/* 现在为这个子项做翻译。 */
	appinfos = find_appinfos_by_relids(root, childrel->relids, &nappinfos);

	relids = adjust_child_relids(relids, nappinfos, appinfos);

	pfree(appinfos);

	return relids;
}

/*
 * adjust_inherited_attnums
 *	  把属性号的整数列表从父项翻译到子项。
 */
List *
adjust_inherited_attnums(List *attnums, AppendRelInfo *context)
{
	List	   *result = NIL;
	ListCell   *lc;

	/* 这只应发生在继承情形，而不是 UNION ALL */
	Assert(OidIsValid(context->parent_reloid));

	/* 在 AppendRelInfo 的 translated_vars 列表中查找每个属性 */
	foreach(lc, attnums)
	{
		AttrNumber	parentattno = lfirst_int(lc);
		Var		   *childvar;

		/* 查找这一列的翻译：它必须是一个 Var */
		if (parentattno <= 0 ||
			parentattno > list_length(context->translated_vars))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));
		childvar = (Var *) list_nth(context->translated_vars, parentattno - 1);
		if (childvar == NULL || !IsA(childvar, Var))
			elog(ERROR, "attribute %d of relation \"%s\" does not exist",
				 parentattno, get_rel_name(context->parent_reloid));

		result = lappend_int(result, childvar->varattno);
	}
	return result;
}

/*
 * adjust_inherited_attnums_multilevel
 *	  同上，但按需要穿越多个继承层。
 */
List *
adjust_inherited_attnums_multilevel(PlannerInfo *root, List *attnums,
									Index child_relid, Index top_parent_relid)
{
	AppendRelInfo *appinfo = root->append_rel_array[child_relid];

	if (!appinfo)
		elog(ERROR, "child rel %d not found in append_rel_array", child_relid);

	/* 若直接父项不是顶层父项，则递归。 */
	if (appinfo->parent_relid != top_parent_relid)
		attnums = adjust_inherited_attnums_multilevel(root, attnums,
													  appinfo->parent_relid,
													  top_parent_relid);

	/* 现在为这个子项做翻译 */
	return adjust_inherited_attnums(attnums, appinfo);
}

/*
 * get_translated_update_targetlist
 *	  取得 UPDATE 查询的 processed_tlist，按需要翻译以
 *	  匹配子目标关系。
 *
 * 可选地也返回翻译到该目标关系的目标列号列表。
 * （processed_tlist 中的 resno 不得被
 * 用来做这件事。）
 */
void
get_translated_update_targetlist(PlannerInfo *root, Index relid,
								 List **processed_tlist, List **update_colnos)
{
	/* 对 UPDATE 以外的命令，这相当没有意义。 */
	Assert(root->parse->commandType == CMD_UPDATE);
	if (relid == root->parse->resultRelation)
	{
		/*
		 * 非继承情形，所以很简单。不过调用者也许预期
		 * 一棵可以就地修改的树，因此要复制。
		 */
		*processed_tlist = copyObject(root->processed_tlist);
		if (update_colnos)
			*update_colnos = copyObject(root->update_colnos);
	}
	else
	{
		Assert(bms_is_member(relid, root->all_result_relids));
		*processed_tlist = (List *)
			adjust_appendrel_attrs_multilevel(root,
											  (Node *) root->processed_tlist,
											  find_base_rel(root, relid),
											  find_base_rel(root, root->parse->resultRelation));
		if (update_colnos)
			*update_colnos =
				adjust_inherited_attnums_multilevel(root, root->update_colnos,
													relid,
													root->parse->resultRelation);
	}
}

/*
 * find_appinfos_by_relids
 * 		为 relids 中列出的基关系找出 AppendRelInfo 结构。
 *
 * relids 参数通常是连接关系的 relid，除基关系外还可以
 * 包含外连接的 RT 下标。我们默默
 * 忽略外连接。
 *
 * AppendRelInfos 放在一个数组里返回，调用者可以 pfree。
 * *nappinfos 设为数组中的项数。
 */
AppendRelInfo **
find_appinfos_by_relids(PlannerInfo *root, Relids relids, int *nappinfos)
{
	AppendRelInfo **appinfos;
	int			cnt = 0;
	int			i;

	/* 分配一个肯定够大的数组 */
	appinfos = (AppendRelInfo **)
		palloc(sizeof(AppendRelInfo *) * bms_num_members(relids));

	i = -1;
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		AppendRelInfo *appinfo = root->append_rel_array[i];

		if (!appinfo)
		{
			/* i 多半是外连接下标，但还是检查一下 */
			if (find_base_rel_ignore_join(root, i) == NULL)
				continue;
			/* 它是基关系，但我们缺少 append_rel_array 项 */
			elog(ERROR, "child rel %d not found in append_rel_array", i);
		}

		appinfos[cnt++] = appinfo;
	}
	*nappinfos = cnt;
	return appinfos;
}


/*****************************************************************************
 *
 *		IDENTITY VARIABLE MANAGEMENT（行标识变量管理）
 *
 * 这段代码也许缺少一个好的归属。我们选择放在这里，因为
 * adjust_appendrel_attrs_mutator() 是它的主要同谋。那个
 * 函数完成了把 ROWID_VAR Var 展开成
 * 正确内容所需的大部分工作。
 *
 *****************************************************************************/

/*
 * add_row_identity_var
 *	  登记 UPDATE/DELETE/MERGE 要用的行标识列。
 *
 * 该 Var 除 varno 外必须与任何其他同名 rowid_name 的行标识
 * 列 equal()。因此例如 wholerow
 * 行标识最好用 vartype == RECORDOID。
 *
 * rtindex 目前与 rowid_var->varno 冗余，但我们把它
 * 作为单独参数指定，以防将来推广以支持
 * 非 Var 表达式。（我们可以合理地处理指定 rtindex 的
 * Var 上的表达式，但眼下似乎不必要。）
 */
void
add_row_identity_var(PlannerInfo *root, Var *orig_var,
					 Index rtindex, const char *rowid_name)
{
	TargetEntry *tle;
	Var		   *rowid_var;
	RowIdentityVarInfo *ridinfo;
	ListCell   *lc;

	/* 眼下，参数必须只是给定 rtindex 的一个 Var */
	Assert(IsA(orig_var, Var));
	Assert(orig_var->varno == rtindex);
	Assert(orig_var->varlevelsup == 0);
	Assert(orig_var->varnullingrels == NULL);

	/*
	 * 若做的是非继承的 UPDATE/DELETE/MERGE，就不大需要
	 * ROWID_VAR 这套花样。把给出的 Var 塞进
	 * processed_tlist，就完成了。
	 */
	if (rtindex == root->parse->resultRelation)
	{
		tle = makeTargetEntry((Expr *) orig_var,
							  list_length(root->processed_tlist) + 1,
							  pstrdup(rowid_name),
							  true);
		root->processed_tlist = lappend(root->processed_tlist, tle);
		return;
	}

	/*
	 * 否则，rtindex 应当引用正在
	 * expand_inherited_rtentry() 期间加入查询的叶子目标关系。
	 */
	Assert(bms_is_member(rtindex, root->leaf_result_relids));
	Assert(root->append_rel_array[rtindex] != NULL);

	/*
	 * 必须找到匹配的 RowIdentityVarInfo，若没有就建一个。
	 * 为了能用 equal() 匹配这些 var，把 varno 改成
	 * ROWID_VAR，其余不动。
	 */
	rowid_var = copyObject(orig_var);
	/* 这最终可能变成 ChangeVarNodes() */
	rowid_var->varno = ROWID_VAR;

	/* 查找已有的同名行标识列 */
	foreach(lc, root->row_identity_vars)
	{
		ridinfo = (RowIdentityVarInfo *) lfirst(lc);
		if (strcmp(rowid_name, ridinfo->rowidname) != 0)
			continue;
		if (equal(rowid_var, ridinfo->rowidvar))
		{
			/* 找到匹配；只需记下这个 rtindex 也需要它 */
			ridinfo->rowidrels = bms_add_member(ridinfo->rowidrels, rtindex);
			return;
		}
		else
		{
			/* 哎呀，处理不了这个 */
			elog(ERROR, "conflicting uses of row-identity name \"%s\"",
				 rowid_name);
		}
	}

	/* 还没有请求，因此添加一个新的 RowIdentityVarInfo */
	ridinfo = makeNode(RowIdentityVarInfo);
	ridinfo->rowidvar = copyObject(rowid_var);
	/* 眼下只用数据类型信息估计宽度 */
	ridinfo->rowidwidth = get_typavgwidth(exprType((Node *) rowid_var),
										  exprTypmod((Node *) rowid_var));
	ridinfo->rowidname = pstrdup(rowid_name);
	ridinfo->rowidrels = bms_make_singleton(rtindex);

	root->row_identity_vars = lappend(root->row_identity_vars, ridinfo);

	/* 把 rowid_var 改成对这个 row_identity_vars 项的引用 */
	rowid_var->varattno = list_length(root->row_identity_vars);

	/* 把 ROWID_VAR 引用变量推进 processed_tlist */
	tle = makeTargetEntry((Expr *) rowid_var,
						  list_length(root->processed_tlist) + 1,
						  pstrdup(rowid_name),
						  true);
	root->processed_tlist = lappend(root->processed_tlist, tle);
}

/*
 * add_row_identity_columns
 *
 * 本函数添加核心代码所需的行标识列。
 * FDWs 可以自己调用 add_row_identity_var() 来添加非标准
 * 列。（重复请求没问题。）
 */
void
add_row_identity_columns(PlannerInfo *root, Index rtindex,
						 RangeTblEntry *target_rte,
						 Relation target_relation)
{
	CmdType		commandType = root->parse->commandType;
	char		relkind = target_relation->rd_rel->relkind;
	Var		   *var;

	Assert(commandType == CMD_UPDATE || commandType == CMD_DELETE || commandType == CMD_MERGE);

	if (commandType == CMD_MERGE ||
		relkind == RELKIND_RELATION ||
		relkind == RELKIND_MATVIEW ||
		relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * 发出 CTID，以便执行器能找到要合并、更新或
		 * 删除的行。
		 */
		var = makeVar(rtindex,
					  SelfItemPointerAttributeNumber,
					  TIDOID,
					  -1,
					  InvalidOid,
					  0);
		add_row_identity_var(root, var, rtindex, "ctid");
	}
	else if (relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * 让外部表的 FDW 添加它想要的任何废 TLEs。
		 */
		FdwRoutine *fdwroutine;

		fdwroutine = GetFdwRoutineForRelation(target_relation, false);

		if (fdwroutine->AddForeignUpdateTargets != NULL)
			fdwroutine->AddForeignUpdateTargets(root, rtindex,
												target_rte, target_relation);

		/*
		 * 对于 UPDATE，我们需要让 FDW 取出未改变的列，办法是
		 * 让它取一个整行 Var。这是因为顶层
		 * 目标列表只含已改变列的项，但
		 * ExecUpdate 需要构造完整的新元组。（实际上，
		 * 我们只在没有下推到
		 * 远端的 UPDATEs 中真正需要这个，但在
		 * 调用本函数时很难说会不会是那种情况。）
		 *
		 * 若有任何行触发器，我们也需要整行，以便
		 * 执行器有旧行可传给触发器。
		 * 可惜这漏掉了系统列。
		 */
		if (commandType == CMD_UPDATE ||
			(target_relation->trigdesc &&
			 (target_relation->trigdesc->trig_delete_after_row ||
			  target_relation->trigdesc->trig_delete_before_row)))
		{
			var = makeVar(rtindex,
						  InvalidAttrNumber,
						  RECORDOID,
						  -1,
						  InvalidOid,
						  0);
			add_row_identity_var(root, var, rtindex, "wholerow");
		}
	}
}

/*
 * distribute_row_identity_vars
 *
 * 在识别完继承的 UPDATE/DELETE/MERGE 查询所需的全部行标识列之后，
 * 确保
 * 所有这些列都会由全部目标关系生成。
 *
 * 这大体像 build_base_rel_tlists() 所做的，
 * 只是它不明白对 ROWID_VAR Var 该怎么办。
 * 因为那个函数在展开继承关系之前运行，
 * 它反正永远看不到这种 Var。
 */
void
distribute_row_identity_vars(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	RangeTblEntry *target_rte;
	RelOptInfo *target_rel;
	ListCell   *lc;

	/*
	 * 若这不是继承的 UPDATE/DELETE/MERGE，就没有什么要做。
	 */
	if (parse->commandType != CMD_UPDATE && parse->commandType != CMD_DELETE &&
		parse->commandType != CMD_MERGE)
	{
		Assert(root->row_identity_vars == NIL);
		return;
	}
	target_rte = rt_fetch(result_relation, parse->rtable);
	if (!target_rte->inh)
	{
		Assert(root->row_identity_vars == NIL);
		return;
	}

	/*
	 * 通常，我们预期叶子结果关系会给查询加上一些
	 * ROWID_VAR Var。不过约束
	 * 排除可能抑制了每一个叶子关系。若计划完全没有行标识列，
	 * 执行器会不高兴，即使它
	 * 肯定不会处理任何行。处理这个边角情况的办法是重新打开顶层
	 * 结果关系，并添加它本来会使用的行标识列，
	 * 就像 preprocess_targetlist() 在它没有被标成 inh 时会做的那样。
	 * 然后重新运行 build_base_rel_tlists()，确保加上的列
	 * 被传播到该关系的 reltarget。（这有点难看，但
	 * 把难看和额外周期限制在这个
	 * 不寻常的边角情况里似乎更好。）
	 */
	if (root->row_identity_vars == NIL)
	{
		Relation	target_relation;

		target_relation = table_open(target_rte->relid, NoLock);
		add_row_identity_columns(root, result_relation,
								 target_rte, target_relation);
		table_close(target_relation, NoLock);
		build_base_rel_tlists(root, root->processed_tlist);
		/* 这种情况下没有 ROWID_VAR Var，因此完成了。 */
		return;
	}

	/*
	 * 翻查 processed_tlist，找出 ROWID_VAR 引用 Var，
	 * 并强制把它们复制进最顶层目标
	 * 关系的 reltarget 列表。这就够了，因为它们以后会被复制到
	 * 各个叶子目标关系（并做适当翻译），
	 * 那发生在 appendrel 展开期间——见 set_append_rel_size()。
	 */
	target_rel = find_base_rel(root, result_relation);

	foreach(lc, root->processed_tlist)
	{
		TargetEntry *tle = lfirst(lc);
		Var		   *var = (Var *) tle->expr;

		if (var && IsA(var, Var) && var->varno == ROWID_VAR)
		{
			target_rel->reltarget->exprs =
				lappend(target_rel->reltarget->exprs, copyObject(var));
			/* reltarget 的代价和宽度以后再计算 */
		}
	}
}
