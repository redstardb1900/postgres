/*-------------------------------------------------------------------------
 *
 * preptlist.c
 *	  Routines to preprocess the parse tree target list
 *
 * For an INSERT, the targetlist must contain an entry for each attribute of
 * the target relation in the correct order.
 *
 * For an UPDATE, the targetlist just contains the expressions for the new
 * column values.
 *
 * For UPDATE and DELETE queries, the targetlist must also contain "junk"
 * tlist entries needed to allow the executor to identify the rows to be
 * updated or deleted; for example, the ctid of a heap row.  (The planner
 * adds these; they're not in what we receive from the parser/rewriter.)
 *
 * For all query types, there can be additional junk tlist entries, such as
 * sort keys, Vars needed for a RETURNING list, and row ID information needed
 * for SELECT FOR UPDATE locking and/or EvalPlanQual checking.
 *
 * The query rewrite phase also does preprocessing of the targetlist (see
 * rewriteTargetListIU).  The division of labor between here and there is
 * partially historical, but it's not entirely arbitrary.  The stuff done
 * here is closely connected to physical access to tables, whereas the
 * rewriter's work is more concerned with SQL semantics.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/preptlist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "nodes/makefuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

static List *expand_insert_targetlist(PlannerInfo *root, List *tlist,
									  Relation rel);


/*
 * preprocess_targetlist
 *	    预处理解析树目标列表的驱动函数。
 *
 * 预处理后的目标列表放在 root->processed_tlist 里返回。
 * 另外，若这是 UPDATE，我们在 root->update_colnos 里返回一份目标列号
 * 列表。（processed_tlist 里的 resno 将是连续的，
 * 因此不要靠那个来判断哪些列是目标！）
 */
void
preprocess_targetlist(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	List	   *range_table = parse->rtable;
	CmdType		command_type = parse->commandType;
	RangeTblEntry *target_rte = NULL;
	Relation	target_relation = NULL;
	List	   *tlist;
	ListCell   *lc;

	/*
	 * 若有结果关系，就打开它，以便寻找缺少的
	 * 列等等。我们假定前面的代码已经至少取得了该关系上的
	 * AccessShareLock，因此这里不需要锁。
	 */
	if (result_relation)
	{
		target_rte = rt_fetch(result_relation, range_table);

		/*
		 * 健全性检查：它最好是真正的关系，而不是比如说子查询。
		 * 否则就是解析器或重写器弄乱了。
		 */
		if (target_rte->rtekind != RTE_RELATION)
			elog(ERROR, "result relation must be a regular relation");

		target_relation = table_open(target_rte->relid, NoLock);
	}
	else
		Assert(command_type == CMD_SELECT);

	/*
	 * 在 INSERT 里，执行器预期目标列表精确匹配
	 * 目标表属性的顺序，包括源查询里没提到的
	 * 属性的项。
	 *
	 * 在 UPDATE 里，我们不重排 tlist 顺序，但需要按 tlist 顺序做一份
	 * 目标属性号的单独列表，然后
	 * 把 processed_tlist 项重新编号成连续的。
	 */
	tlist = parse->targetList;
	if (command_type == CMD_INSERT)
		tlist = expand_insert_targetlist(root, tlist, target_relation);
	else if (command_type == CMD_UPDATE)
		root->update_colnos = extract_update_targetlist_colnos(tlist);

	/*
	 * 对非继承的 UPDATE/DELETE/MERGE，登记执行器识别要更新或
	 * 删除的行所需的任何杂项列。
	 * 在继承情形下，现在什么都不做，留给
	 * expand_inherited_rtentry() 建立叶子目标
	 * 关系时处理。（但也许没有任何叶子目标关系，那种
	 * 情况下必须在 distribute_row_identity_vars() 里做这件事。）
	 */
	if ((command_type == CMD_UPDATE || command_type == CMD_DELETE ||
		 command_type == CMD_MERGE) &&
		!target_rte->inh)
	{
		/* 行身份逻辑预期会往 processed_tlist 里加东西 */
		root->processed_tlist = tlist;
		add_row_identity_columns(root, result_relation,
								 target_rte, target_relation);
		tlist = root->processed_tlist;
	}

	/*
	 * 对 MERGE，也需要分别处理每个 INSERT 和
	 * UPDATE 动作的目标列表。另外，我们检查每个
	 * 动作的限定条件，并把那里的任何 Var（目标关系的那些除外）加进
	 * 子计划目标列表。
	 */
	if (command_type == CMD_MERGE)
	{
		ListCell   *l;

		/*
		 * 对 MERGE，分别处理每个 MergeAction 的目标列表。对
		 * MergeAction->targetList 给予与普通
		 * INSERT 相同的处理。对 UPDATE，收集
		 * 正在被修改的列号。
		 */
		foreach(l, parse->mergeActionList)
		{
			MergeAction *action = (MergeAction *) lfirst(l);
			List	   *vars;
			ListCell   *l2;

			if (action->commandType == CMD_INSERT)
				action->targetList = expand_insert_targetlist(root,
															  action->targetList,
															  target_relation);
			else if (action->commandType == CMD_UPDATE)
				action->updateColnos =
					extract_update_targetlist_colnos(action->targetList);

			/*
			 * 为每个动作的目标列表和 WHEN 条件里属于
			 * 目标以外关系的任何 Var 和 PlaceHolderVars 加上 resjunk 项。
			 * 我们不预期在这里看到任何
			 * 聚合或窗口函数。
			 */
			vars = pull_var_clause((Node *)
								   list_concat_copy((List *) action->qual,
													action->targetList),
								   PVC_INCLUDE_PLACEHOLDERS);
			foreach(l2, vars)
			{
				Var		   *var = (Var *) lfirst(l2);
				TargetEntry *tle;

				if (IsA(var, Var) && var->varno == result_relation)
					continue;	/* 不需要它 */

				if (tlist_member((Expr *) var, tlist))
					continue;	/* 已经有了 */

				tle = makeTargetEntry((Expr *) var,
									  list_length(tlist) + 1,
									  NULL, true);
				tlist = lappend(tlist, tle);
			}
			list_free(vars);
		}
	}

	/*
	 * 为做了行标记的关系加上必要的杂项列。
	 * 锁定被选来做 FOR UPDATE/SHARE 的关系，以及做 EvalPlanQual
	 * 复查，都需要这些值。见 plannodes.h 里 PlanRowMark 的注释。若你
	 * 改这一段，也要看 expand_inherited_rtentry()，它必须
	 * 能加上与这些等价的杂项列。
	 *
	 * （也许有一天把这些 resjunk 列折进行身份列
	 * 管理会有用，那是给 UPDATE/DELETE 用的。不过今天
	 * 还不是那天。一个显著问题是，这里做的整行 Var
	 * 使用真实表行类型而不是 RECORD，这似乎很重要，
	 * 这样才会发生与子关系行类型之间的转换。另外，
	 * 由于这些项不会因越来越多的子
	 * 关系而膨胀，列共享其实没有多大必要。）
	 */
	foreach(lc, root->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(lc);
		Var		   *var;
		char		resname[32];
		TargetEntry *tle;

		/* 子关系使用与其父项相同的杂项属性 */
		if (rc->rti != rc->prti)
			continue;

		if (rc->allMarkTypes & ~(1 << ROW_MARK_COPY))
		{
			/* 需要取出 TID */
			var = makeVar(rc->rti,
						  SelfItemPointerAttributeNumber,
						  TIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "ctid%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}
		if (rc->allMarkTypes & (1 << ROW_MARK_COPY))
		{
			/* 需要把整行作为杂项 var */
			var = makeWholeRowVar(rt_fetch(rc->rti, range_table),
								  rc->rti,
								  0,
								  false);
			snprintf(resname, sizeof(resname), "wholerow%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}

		/* 若是继承树的父项，也总是取出 tableoid。 */
		if (rc->isParent)
		{
			var = makeVar(rc->rti,
						  TableOidAttributeNumber,
						  OIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "tableoid%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}
	}

	/*
	 * 若查询有 RETURNING 列表，就为 RETURNING 里属于其他关系的任何 Var
	 * 加上 resjunk 项。需要这么做，
	 * 以使这些 Var 可用于 RETURNING 计算。属于
	 * 结果关系的 Var 不必加，因为它们会被
	 * 改成引用实际的堆元组。
	 */
	if (parse->returningList && list_length(parse->rtable) > 1)
	{
		List	   *vars;
		ListCell   *l;

		vars = pull_var_clause((Node *) parse->returningList,
							   PVC_RECURSE_AGGREGATES |
							   PVC_RECURSE_WINDOWFUNCS |
							   PVC_INCLUDE_PLACEHOLDERS);
		foreach(l, vars)
		{
			Var		   *var = (Var *) lfirst(l);
			TargetEntry *tle;

			if (IsA(var, Var) &&
				var->varno == result_relation)
				continue;		/* 不需要它 */

			if (tlist_member((Expr *) var, tlist))
				continue;		/* 已经有了 */

			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  NULL,
								  true);

			tlist = lappend(tlist, tle);
		}
		list_free(vars);
	}

	root->processed_tlist = tlist;

	if (target_relation)
		table_close(target_relation, NoLock);
}

/*
 * extract_update_targetlist_colnos
 * 				抽出 UPDATE 的目标列表想要赋值的目标表列号
 * 				列表，然后重新编号。
 *
 * 解析器和重写器的约定是，UPDATE 的非 resjunk TLE 项里的 resno
 * 就是要赋值的目标列号。
 * 这里我们把该信息抽成一份单独的列表，然后
 * 把 tlist 转换成所有其他查询类型都使用的
 * 顺序编号约定。
 *
 * 这也应用到与 INSERT ... ON CONFLICT
 * ... UPDATE 关联的 tlist，尽管要到规划后期才做。
 */
List *
extract_update_targetlist_colnos(List *tlist)
{
	List	   *update_colnos = NIL;
	AttrNumber	nextresno = 1;
	ListCell   *lc;

	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (!tle->resjunk)
			update_colnos = lappend_int(update_colnos, tle->resno);
		tle->resno = nextresno++;
	}
	return update_colnos;
}


/*****************************************************************************
 *
 *			TARGETLIST EXPANSION
 *
 *****************************************************************************/

/*
 * expand_insert_targetlist
 *	    给定解析器生成的目标列表和结果关系，
 *	    为任何缺少的属性加上目标列表项，并确保
 *	    非杂项属性以正确的字段顺序出现。
 *
 * 从前我们也对 UPDATE 目标列表或多或少这么做，
 * 但现在这段代码只用于 INSERT 目标列表。
 */
static List *
expand_insert_targetlist(PlannerInfo *root, List *tlist, Relation rel)
{
	List	   *new_tlist = NIL;
	ListCell   *tlist_item;
	int			attrno,
				numattrs;

	tlist_item = list_head(tlist);

	/*
	 * 重写器应当已经确保 TLEs 顺序正确；
	 * 但我们必须为任何缺少的属性插入 TLE。
	 *
	 * 扫描该关系 relcache 项里的元组描述，以
	 * 确保我们有全部用户属性且顺序正确。
	 */
	numattrs = RelationGetNumberOfAttributes(rel);

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = TupleDescAttr(rel->rd_att, attrno - 1);
		TargetEntry *new_tle = NULL;

		if (tlist_item != NULL)
		{
			TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

			if (!old_tle->resjunk && old_tle->resno == attrno)
			{
				new_tle = old_tle;
				tlist_item = lnext(tlist, tlist_item);
			}
		}

		if (new_tle == NULL)
		{
			/*
			 * 没找到匹配的 tlist 项，因此做一个。
			 *
			 * 这种情况下 INSERTs 应当插入 NULL。（我们假定
			 * 重写器会已经插入任何可用的非 NULL 默认
			 * 值。）另外，通常必须应用任何也许存在的域约束
			 * ——这是为了抓住域的 NOT NULL。
			 *
			 * 为已删除列生成 NULL 常量时，我们把它标成
			 * INT4（任何其他保证存在的数据类型也
			 * 行）。不能用已删除列的数据类型来标它，
			 * 因为那个也许已经不存在了。我们声称的类型是什么
			 * 其实无关紧要，因为 NULL 就是 NULL——它的
			 * 表示与数据类型无关。不过这也许会
			 * 搞混把完成的计划与目标
			 * 关系作比较的代码。
			 *
			 * 另一个例外是，若该列是生成列，我们在这里产生的值
			 * 会被忽略，而且我们不想冒
			 * 抛错的风险。因此那种情况下我们*不*想应用
			 * 域约束，所以必须产生基类型的 NULL。
			 * 同样，把完成的计划与目标关系作比较的代码
			 * 必须把这一点算进去。
			 */
			Node	   *new_expr;

			if (att_tup->attisdropped)
			{
				/* 为已删除列插入 NULL */
				new_expr = (Node *) makeConst(INT4OID,
											  -1,
											  InvalidOid,
											  sizeof(int32),
											  (Datum) 0,
											  true, /* isnull（为空） */
											  true /* byval（传值） */ );
			}
			else if (att_tup->attgenerated)
			{
				/* 生成列，插入基类型的 NULL */
				Oid			baseTypeId = att_tup->atttypid;
				int32		baseTypeMod = att_tup->atttypmod;

				baseTypeId = getBaseTypeAndTypmod(baseTypeId, &baseTypeMod);
				new_expr = (Node *) makeConst(baseTypeId,
											  baseTypeMod,
											  att_tup->attcollation,
											  att_tup->attlen,
											  (Datum) 0,
											  true, /* isnull（为空） */
											  att_tup->attbyval);
			}
			else
			{
				/* 普通列，插入该列数据类型的 NULL */
				new_expr = coerce_null_to_domain(att_tup->atttypid,
												 att_tup->atttypmod,
												 att_tup->attcollation,
												 att_tup->attlen,
												 att_tup->attbyval);
				/* 必须对任何非 const 节点跑表达式预处理 */
				if (!IsA(new_expr, Const))
					new_expr = eval_const_expressions(root, new_expr);
			}

			new_tle = makeTargetEntry((Expr *) new_expr,
									  attrno,
									  pstrdup(NameStr(att_tup->attname)),
									  false);
		}

		new_tlist = lappend(new_tlist, new_tle);
	}

	/*
	 * 其余 tlist 项应当是 resjunk；把它们全部追加到
	 * 新 tlist 的末尾，确保它们的 resno 高于最后一个
	 * 真实属性。（注意：虽然重写器已经做过这样的
	 * 重新编号，若上面加了 NULL 项，
	 * 这里必须再做一次。）
	 */
	while (tlist_item)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

		if (!old_tle->resjunk)
			elog(ERROR, "targetlist is not sorted correctly");
		/* 把 resno 弄对，但不要不必要地复制 */
		if (old_tle->resno != attrno)
		{
			old_tle = flatCopyTargetEntry(old_tle);
			old_tle->resno = attrno;
		}
		new_tlist = lappend(new_tlist, old_tle);
		attrno++;
		tlist_item = lnext(tlist, tlist_item);
	}

	return new_tlist;
}


/*
 * 找出给定 RT 下标的 PlanRowMark，若没有则返回 NULL
 *
 * 这大概应当放在别处，但没有很好的地方
 */
PlanRowMark *
get_plan_rowmark(List *rowmarks, Index rtindex)
{
	ListCell   *l;

	foreach(l, rowmarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(l);

		if (rc->rti == rtindex)
			return rc;
	}
	return NULL;
}
