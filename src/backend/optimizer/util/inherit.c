/*-------------------------------------------------------------------------
 *
 * inherit.c
 *	  Routines to process child relations in inheritance trees
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/inherit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "utils/rel.h"


static void expand_partitioned_rtentry(PlannerInfo *root, RelOptInfo *relinfo,
									   RangeTblEntry *parentrte,
									   Index parentRTindex, Relation parentrel,
									   Bitmapset *parent_updatedCols,
									   PlanRowMark *top_parentrc, LOCKMODE lockmode);
static void expand_single_inheritance_child(PlannerInfo *root,
											RangeTblEntry *parentrte,
											Index parentRTindex, Relation parentrel,
											PlanRowMark *top_parentrc, Relation childrel,
											RangeTblEntry **childrte_p,
											Index *childRTindex_p);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
									  List *translated_vars);
static Bitmapset *translate_col_privs_multilevel(PlannerInfo *root,
												 RelOptInfo *rel,
												 RelOptInfo *parent_rel,
												 Bitmapset *parent_cols);
static void expand_appendrel_subquery(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte, Index rti);


/*
 * expand_inherited_rtentry
 *		展开设置了 inh 位的范围表项。
 *
 * inh 只允许两种情形：RELATION 和 SUBQUERY RTEs。
 *
 * 普通 RELATION RTE 上的 inh 表示它是分区表，或
 * 传统继承集的父表。这时必须把
 * 所有有关的子表项加进查询的范围表，并为它们建立
 * 额外的规划器数据结构，包括 RelOptInfos、
 * AppendRelInfos，以及可能的 PlanRowMarks。
 *
 * 注意原来的 RTE 被认为代表整个继承
 * 集。传统继承时，生成的第一个
 * RTE 是同一张表的 RTE，但 inh = false，以代表
 * 父表作为继承集普通成员的角色。对于
 * 分区，不需要第二个 RTE，因为分区表
 * 本身没有数据，不必扫描。
 *
 * SUBQUERY RTE 上的 inh 表示它是 UNION ALL 组的父项，
 * 与继承情形类似，被当作 appendrel；不过
 * 我们已经为子查询建了 RTE 和 AppendRelInfos。只需
 * 为它们建立 RelOptInfos，这由 expand_appendrel_subquery 完成。
 */
void
expand_inherited_rtentry(PlannerInfo *root, RelOptInfo *rel,
						 RangeTblEntry *rte, Index rti)
{
	Oid			parentOID;
	Relation	oldrelation;
	LOCKMODE	lockmode;
	PlanRowMark *oldrc;
	bool		old_isParent = false;
	int			old_allMarkTypes = 0;

	Assert(rte->inh);			/* 否则是调用者错误 */

	if (rte->rtekind == RTE_SUBQUERY)
	{
		expand_appendrel_subquery(root, rel, rte, rti);
		return;
	}

	Assert(rte->rtekind == RTE_RELATION);

	parentOID = rte->relid;

	/*
	 * 我们以前在这里检查 has_subclass()，但不再
	 * 需要，因为 subquery_planner 已经做过。
	 */

	/*
	 * 重写器应当已经对查询中点名的每个
	 * 关系取得了适当的锁，因此可以不加锁地打开父关系。
	 * 不过，对加进查询的每个子关系，我们
	 * 必须取得适当的锁，因为这将是这些关系在解析/重写/规划流水线中的
	 * 第一次使用。子关系应当
	 * 使用与父关系相同的锁模式。
	 */
	oldrelation = table_open(parentOID, NoLock);
	lockmode = rte->rellockmode;

	/*
	 * 若父关系被选为 FOR UPDATE/SHARE，需要把它的
	 * PlanRowMark 标成 isParent = true，并为每个
	 * 子项生成一个新的 PlanRowMark。
	 */
	oldrc = get_plan_rowmark(root->rowMarks, rti);
	if (oldrc)
	{
		old_isParent = oldrc->isParent;
		oldrc->isParent = true;
		/* 在子项往里加之前，保存 allMarkTypes 的初值 */
		old_allMarkTypes = oldrc->allMarkTypes;
	}

	/* 扫描继承集并展开它 */
	if (oldrelation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		RTEPermissionInfo *perminfo;

		perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);

		/*
		 * 是分区表，因此为分区做准备。
		 */
		Assert(rte->relkind == RELKIND_PARTITIONED_TABLE);

		/*
		 * 递归展开并锁定各个分区。同时
		 * 抽出所有分区表的分区键列。
		 */
		expand_partitioned_rtentry(root, rel, rte, rti,
								   oldrelation,
								   perminfo->updatedCols,
								   oldrc, lockmode);
	}
	else
	{
		/*
		 * 普通表，因此处理传统继承的子表。（注意
		 * 分区表不允许有继承
		 * 子表，因此两种情形不可能同时适用。）
		 */
		List	   *inhOIDs;
		ListCell   *l;

		/* 扫描继承集的全部成员，取得所需的锁 */
		inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

		/*
		 * 我们以前对表不再有
		 * 任何子表的情况做特殊处理：清除 rte->inh 并退出。那不再
		 * 可行，因为本函数要到依赖 rte->inh 的决定
		 * 已经做出之后才运行。我们必须把这类
		 * 情况当作正常继承。不过表本身应当总是
		 * 已经被找到。
		 */
		Assert(inhOIDs != NIL);
		Assert(linitial_oid(inhOIDs) == parentOID);

		/* 扩大 simple_rel_array 及其相关结构以容纳子对象。 */
		expand_planner_arrays(root, list_length(inhOIDs));

		/*
		 * 按 find_all_inheritors 返回 OIDs 的顺序
		 * 展开继承子表。
		 */
		foreach(l, inhOIDs)
		{
			Oid			childOID = lfirst_oid(l);
			Relation	newrelation;
			RangeTblEntry *childrte;
			Index		childRTindex;

			/* 若需要则打开关系；所需的锁已经有了 */
			if (childOID != parentOID)
				newrelation = table_open(childOID, NoLock);
			else
				newrelation = oldrelation;

			/*
			 * 父表可能有属于其他后端的临时
			 * 子表。我们无法安全访问这种表
			 * （因为缓冲问题），最好的办法
			 * 似乎是默默忽略它们。
			 */
			if (childOID != parentOID && RELATION_IS_OTHER_TEMP(newrelation))
			{
				table_close(newrelation, lockmode);
				continue;
			}

			/* 创建 RTE 和 AppendRelInfo，若需要再加 PlanRowMark。 */
			expand_single_inheritance_child(root, rte, rti, oldrelation,
											oldrc, newrelation,
											&childrte, &childRTindex);

			/* 也创建 otherrel 的 RelOptInfo。 */
			(void) build_simple_rel(root, childRTindex, rel);

			/* 关闭子关系，但保留锁 */
			if (childOID != parentOID)
				table_close(newrelation, NoLock);
		}
	}

	/*
	 * 有些子表可能需要不同的标记类型，那会已经
	 * 报告进 oldrc。若如此，把相关项加到顶层
	 * 目标列表并更新父关系的 reltarget。这应当与
	 * preprocess_targetlist() 在这些标记类型
	 * 一开始就被请求时会添加的内容一致。
	 *
	 * （将来也许有用的是把这些 resjunk 列并入
	 * 用于 UPDATE/DELETE 的行标识列管理。今天还
	 * 不是那天。）
	 */
	if (oldrc)
	{
		int			new_allMarkTypes = oldrc->allMarkTypes;
		Var		   *var;
		TargetEntry *tle;
		char		resname[32];
		List	   *newvars = NIL;

		/* 若需要则添加 TID 废 Var，除非已经有了 */
		if (new_allMarkTypes & ~(1 << ROW_MARK_COPY) &&
			!(old_allMarkTypes & ~(1 << ROW_MARK_COPY)))
		{
			/* 需要取出 TID */
			var = makeVar(oldrc->rti,
						  SelfItemPointerAttributeNumber,
						  TIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "ctid%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/* 若需要则添加整行废 Var，除非已经有了 */
		if ((new_allMarkTypes & (1 << ROW_MARK_COPY)) &&
			!(old_allMarkTypes & (1 << ROW_MARK_COPY)))
		{
			var = makeWholeRowVar(planner_rt_fetch(oldrc->rti, root),
								  oldrc->rti,
								  0,
								  false);
			snprintf(resname, sizeof(resname), "wholerow%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/* 添加 tableoid 废 Var，除非已经有了 */
		if (!old_isParent)
		{
			var = makeVar(oldrc->rti,
						  TableOidAttributeNumber,
						  OIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "tableoid%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/*
		 * 把新加的 Var 加到父关系的 reltarget。不必担心
		 * 子关系的 reltarget，它们以后会建立。
		 */
		add_vars_to_targetlist(root, newvars, bms_make_singleton(0));
	}

	table_close(oldrelation, NoLock);
}

/*
 * expand_partitioned_rtentry
 *		递归展开分区表的 RTE。
 */
static void
expand_partitioned_rtentry(PlannerInfo *root, RelOptInfo *relinfo,
						   RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   Bitmapset *parent_updatedCols,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode)
{
	PartitionDesc partdesc;
	int			num_live_parts;
	int			i;

	check_stack_depth();

	Assert(parentrte->inh);

	partdesc = PartitionDirectoryLookup(root->glob->partition_directory,
										parentrel);

	/* 分区表应当总有分区描述符。 */
	Assert(partdesc);

	/*
	 * 记下是否有分区键列正在被更新。虽然我们关心的是
	 * 根分区表的 updatedCols，
	 * 调用者提供的 parent_updatedCols 含有根 partrel 的
	 * updatedCols，已翻译成与 parentrel 的属性顺序一致。
	 */
	if (!root->partColsUpdated)
		root->partColsUpdated =
			has_partition_attrs(parentrel, parent_updatedCols, NULL);

	/* 若没有分区，这里没有进一步的事。 */
	if (partdesc->nparts == 0)
		return;

	/*
	 * 用赋给父关系的限制子句做分区裁剪。
	 * live_parts 将含有经裁剪仍保留的分区在 PartitionDesc 中的下标。
	 * 下面我们将为
	 * 保留下来的分区初始化子对象。
	 */
	relinfo->live_parts = prune_append_rel_partitions(relinfo);

	/* 扩大 simple_rel_array 及其相关结构以容纳子对象。 */
	num_live_parts = bms_num_members(relinfo->live_parts);
	if (num_live_parts > 0)
		expand_planner_arrays(root, num_live_parts);

	/*
	 * 我们也把分区的 RelOptInfo 指针存在父关系里。
	 * 因为我们用 palloc0，被裁掉的分区对应的槽
	 * 将含有 NULL。
	 */
	Assert(relinfo->part_rels == NULL);
	relinfo->part_rels = (RelOptInfo **)
		palloc0(relinfo->nparts * sizeof(RelOptInfo *));

	/*
	 * 为每个仍然存活的分区创建一个子 RTE。注意与
	 * 传统继承不同，分区表本身不需要子 RTE，
	 * 因为它不会被扫描。
	 */
	i = -1;
	while ((i = bms_next_member(relinfo->live_parts, i)) >= 0)
	{
		Oid			childOID = partdesc->oids[i];
		Relation	childrel;
		RangeTblEntry *childrte;
		Index		childRTindex;
		RelOptInfo *childrelinfo;

		/*
		 * 打开关系并取得所需的锁。若某个分区最近
		 * 被分离并随后被删除，打开它会失败。在
		 * 这种情况下，表现得好像该分区已被裁掉。
		 */
		childrel = try_table_open(childOID, lockmode);
		if (childrel == NULL)
		{
			relinfo->live_parts = bms_del_member(relinfo->live_parts, i);
			continue;
		}

		/*
		 * 属于其他会话的临时分区在定义时就应当被
		 * 禁止，但为保险起见，再
		 * 检查一次。
		 */
		if (RELATION_IS_OTHER_TEMP(childrel))
			elog(ERROR, "temporary relation from another session found as partition");

		/* 创建 RTE 和 AppendRelInfo，若需要再加 PlanRowMark。 */
		expand_single_inheritance_child(root, parentrte, parentRTindex,
										parentrel, top_parentrc, childrel,
										&childrte, &childRTindex);

		/* 也创建 otherrel 的 RelOptInfo。 */
		childrelinfo = build_simple_rel(root, childRTindex, relinfo);
		relinfo->part_rels[i] = childrelinfo;
		relinfo->all_partrels = bms_add_members(relinfo->all_partrels,
												childrelinfo->relids);

		/* 若这个子项自身也是分区的，则递归 */
		if (childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			AppendRelInfo *appinfo = root->append_rel_array[childRTindex];
			Bitmapset  *child_updatedCols;

			child_updatedCols = translate_col_privs(parent_updatedCols,
													appinfo->translated_vars);

			expand_partitioned_rtentry(root, childrelinfo,
									   childrte, childRTindex,
									   childrel,
									   child_updatedCols,
									   top_parentrc, lockmode);
		}

		/* 关闭子关系，但保留锁 */
		table_close(childrel, NoLock);
	}
}

/*
 * expand_single_inheritance_child
 *		建立一个 RangeTblEntry 和一个 AppendRelInfo，也许再加一个 PlanRowMark。
 *
 * 我们现在一层一层地展开分区层次，建立
 * 对应的 AppendRelInfos 和 RelOptInfos 层次，其中每个
 * 分区后代充当其直接分区的父项。
 * （这与较旧版本的 PostgreSQL 所做的不同，也与
 * 未分区表的表继承仍然采用的做法不同，
 * 那种情况下层次在 RTE 展开时被展平。）
 *
 * PlanRowMarks 仍然携带顶层父项的 RTI，顶层父项的
 * allMarkTypes 字段仍然累积所有后代的值。
 *
 * parentrte 和 parentRTindex 是直接父项的 RTE 和
 * RTI。top_parentrc 是顶层父项的 PlanRowMark。
 *
 * 子 RangeTblEntry 及其 RTI 分别返回在 childrte_p 和
 * childRTindex_p 中。
 */
static void
expand_single_inheritance_child(PlannerInfo *root, RangeTblEntry *parentrte,
								Index parentRTindex, Relation parentrel,
								PlanRowMark *top_parentrc, Relation childrel,
								RangeTblEntry **childrte_p,
								Index *childRTindex_p)
{
	Query	   *parse = root->parse;
	Oid			parentOID PG_USED_FOR_ASSERTS_ONLY =
		RelationGetRelid(parentrel);
	Oid			childOID = RelationGetRelid(childrel);
	RangeTblEntry *childrte;
	Index		childRTindex;
	AppendRelInfo *appinfo;
	TupleDesc	child_tupdesc;
	List	   *parent_colnames;
	List	   *child_colnames;

	/*
	 * 为子项建立一个 RTE，并挂到查询的范围表列表。我们
	 * 复制父 RTE 的大部分标量成员，但替换子项的关系 OID、
	 * relkind 和 inh。把子项的 securityQuals 设为
	 * 空，因为我们只想应用父项的 RLS 条件，
	 * 不论各个子项自己有什么 RLS 属性。（这
	 * 是有意的选择，使继承的 RLS 像常规
	 * 权限检查那样工作。）父项的 securityQuals 会连同其他基限制子句
	 * 一起传播到子项，因此这里不必
	 * 做这件事。父 RTE 的其他基础设施必须
	 * 翻译成与子表列顺序一致，这在
	 * 下面做，因此一开始做一份平铺副本就够了。
	 */
	childrte = makeNode(RangeTblEntry);
	memcpy(childrte, parentrte, sizeof(RangeTblEntry));
	Assert(parentrte->rtekind == RTE_RELATION); /* 否则这值得怀疑 */
	childrte->relid = childOID;
	childrte->relkind = childrel->rd_rel->relkind;
	/* 分区的子项还需要进一步展开。 */
	if (childrte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		Assert(childOID != parentOID);
		childrte->inh = true;
	}
	else
		childrte->inh = false;
	childrte->securityQuals = NIL;

	/* 对子 RTEs 不做权限检查。 */
	childrte->perminfoindex = 0;

	/* 把尚未完全填好的子 RTE 链进数据结构 */
	parse->rtable = lappend(parse->rtable, childrte);
	childRTindex = list_length(parse->rtable);
	*childrte_p = childrte;
	*childRTindex_p = childRTindex;

	/*
	 * 为每一对父子建立一个 AppendRelInfo 结构。
	 */
	appinfo = make_append_rel_info(parentrel, childrel,
								   parentRTindex, childRTindex);
	root->append_rel_list = lappend(root->append_rel_list, appinfo);

	/* tablesample 多半是空的，但还是复制它 */
	childrte->tablesample = copyObject(parentrte->tablesample);

	/*
	 * 为子项构造一个别名子句，也可以当作 eref 用。
	 * 这很重要，这样 EXPLAIN 会为子表列打印正确的列别名。
	 * （因为 ruleutils.c 没有简便办法
	 * 把父列和子列重新关联起来，我们必须一开始就把子列
	 * 别名弄对。注意设置 childrte->alias 会迫使
	 * ruleutils.c 使用这些列名，否则它不会用。）
	 */
	child_tupdesc = RelationGetDescr(childrel);
	parent_colnames = parentrte->eref->colnames;
	child_colnames = NIL;
	for (int cattno = 0; cattno < child_tupdesc->natts; cattno++)
	{
		Form_pg_attribute att = TupleDescAttr(child_tupdesc, cattno);
		const char *attname;

		if (att->attisdropped)
		{
			/* 对已删除的列总是插入空字符串 */
			attname = "";
		}
		else if (appinfo->parent_colnos[cattno] > 0 &&
				 appinfo->parent_colnos[cattno] <= list_length(parent_colnames))
		{
			/* 复制查询赋给父列的名字 */
			attname = strVal(list_nth(parent_colnames,
									  appinfo->parent_colnos[cattno] - 1));
		}
		else
		{
			/* 新列，就用它的真名 */
			attname = NameStr(att->attname);
		}
		child_colnames = lappend(child_colnames, makeString(pstrdup(attname)));
	}

	/*
	 * 我们只是为每个子项复制父表的表别名。若
	 * 计划被打印，ruleutils.c 必须理出要使用的唯一表别名，
	 * 它能处理。
	 */
	childrte->alias = childrte->eref = makeAlias(parentrte->eref->aliasname,
												 child_colnames);

	/*
	 * 把 RTE 和 appinfo 存进各自的 PlannerInfo 数组，
	 * 调用者必须已经为它们分配了空间。
	 */
	Assert(childRTindex < root->simple_rel_array_size);
	Assert(root->simple_rte_array[childRTindex] == NULL);
	root->simple_rte_array[childRTindex] = childrte;
	Assert(root->append_rel_array[childRTindex] == NULL);
	root->append_rel_array[childRTindex] = appinfo;

	/*
	 * 若父项被标为 FOR UPDATE/SHARE，则建立一个 PlanRowMark。
	 */
	if (top_parentrc)
	{
		PlanRowMark *childrc = makeNode(PlanRowMark);

		childrc->rti = childRTindex;
		childrc->prti = top_parentrc->rti;
		childrc->rowmarkId = top_parentrc->rowmarkId;
		/* 重新选择行标记类型，因为 relkind 可能与父项不匹配 */
		childrc->markType = select_rowmark_type(childrte,
												top_parentrc->strength);
		childrc->allMarkTypes = (1 << childrc->markType);
		childrc->strength = top_parentrc->strength;
		childrc->waitPolicy = top_parentrc->waitPolicy;

		/*
		 * 我们把分区子表的 RowMarks 标成父 RowMarks，这样
		 * 执行器会忽略它们（只是它们的存在意味着
		 * 子表会用适当的模式被锁定）。
		 */
		childrc->isParent = (childrte->relkind == RELKIND_PARTITIONED_TABLE);

		/* 把子项的行标记类型纳入顶层父项的 allMarkTypes */
		top_parentrc->allMarkTypes |= childrc->allMarkTypes;

		root->rowMarks = lappend(root->rowMarks, childrc);
	}

	/*
	 * 若正在创建查询目标关系的子项（只可能
	 * 在 UPDATE/DELETE/MERGE 中），把它加入 all_result_relids，并在
	 * 适当时加入 leaf_result_relids，并确保我们生成
	 * 所需的行标识数据。
	 */
	if (bms_is_member(parentRTindex, root->all_result_relids))
	{
		/* 好，也把这个子项登记为结果关系。 */
		root->all_result_relids = bms_add_member(root->all_result_relids,
												 childRTindex);

		/* 非叶子分区不需要任何行标识信息。 */
		if (childrte->relkind != RELKIND_PARTITIONED_TABLE)
		{
			Var		   *rrvar;

			root->leaf_result_relids = bms_add_member(root->leaf_result_relids,
													  childRTindex);

			/*
			 * 若有任何子目标关系，假定它们都需要
			 * 生成一个废 tableoid 列。（若裁剪后只剩一个子项，
			 * 其实不需要这个，但不值得
			 * 为避开它折腾。）
			 */
			rrvar = makeVar(childRTindex,
							TableOidAttributeNumber,
							OIDOID,
							-1,
							InvalidOid,
							0);
			add_row_identity_var(root, rrvar, childRTindex, "tableoid");

			/* 登记这个子项所需的任何行标识列。 */
			add_row_identity_columns(root, childRTindex,
									 childrte, childrel);
		}
	}
}

/*
 * get_rel_all_updated_cols
 * 		返回给定简单关系中被本查询
 * 		更新的列集合。
 */
Bitmapset *
get_rel_all_updated_cols(PlannerInfo *root, RelOptInfo *rel)
{
	Index		relid;
	RangeTblEntry *rte;
	RTEPermissionInfo *perminfo;
	Bitmapset  *updatedCols,
			   *extraUpdatedCols;

	Assert(root->parse->commandType == CMD_UPDATE);
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * 我们取得查询结果关系的 updatedCols。然后若
	 * 有必要，把它映射到被请求的那个关系的列号。
	 * 按此映射。
	 */
	relid = root->parse->resultRelation;
	rte = planner_rt_fetch(relid, root);
	perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);

	updatedCols = perminfo->updatedCols;

	if (rel->relid != relid)
	{
		RelOptInfo *top_parent_rel = find_base_rel(root, relid);

		Assert(IS_OTHER_REL(rel));

		updatedCols = translate_col_privs_multilevel(root, rel, top_parent_rel,
													 updatedCols);
	}

	/*
	 * 现在必须检查是否有任何依赖于
	 * updatedCols 的生成列，并把它们加入结果。
	 */
	extraUpdatedCols = get_dependent_generated_columns(root, rel->relid,
													   updatedCols);

	return bms_union(updatedCols, extraUpdatedCols);
}

/*
 * translate_col_privs
 *	  把表示按列权限的位图集从
 *	  父关系的属性编号翻译到子关系。
 *
 * 这里唯一令人意外的是，我们不把父项的整行
 * 引用翻译成子项的整行引用。那将意味着需要
 * 对所有子列的权限，过于严格，因为
 * 查询实际上只会引用继承来的列。我们改为
 * 为所有继承来的列设置按列的位。
 */
static Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars)
{
	Bitmapset  *child_privs = NULL;
	bool		whole_row;
	int			attno;
	ListCell   *lc;

	/* 系统属性在所有表中编号相同 */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
										 attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* 检查父项是否有整行引用 */
	whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
							  parent_privs);

	/* 然后用 vars 列表翻译普通用户属性 */
	attno = InvalidAttrNumber;
	foreach(lc, translated_vars)
	{
		Var		   *var = lfirst_node(Var, lc);

		attno++;
		if (var == NULL)		/* 忽略已删除的列 */
			continue;
		if (whole_row ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
										 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return child_privs;
}

/*
 * translate_col_privs_multilevel
 *		把 parent_cols 中包含的列号递归翻译
 *		成由 rel 给出的后代关系的列号
 *
 * 注意因为这基于 translate_col_privs，它会把
 * 整行引用展开成所有继承来的列。对当前的用法这不是问题，
 * 但要当心。
 */
static Bitmapset *
translate_col_privs_multilevel(PlannerInfo *root, RelOptInfo *rel,
							   RelOptInfo *parent_rel,
							   Bitmapset *parent_cols)
{
	AppendRelInfo *appinfo;

	/* 简单情形的快速路径。 */
	if (parent_cols == NULL)
		return NULL;

	/* 若直接父项不是顶层父项，则递归。 */
	if (rel->parent != parent_rel)
	{
		if (rel->parent)
			parent_cols = translate_col_privs_multilevel(root, rel->parent,
														 parent_rel,
														 parent_cols);
		else
			elog(ERROR, "rel with relid %u is not a child rel", rel->relid);
	}

	/* 现在为这个子项做翻译。 */
	Assert(root->append_rel_array != NULL);
	appinfo = root->append_rel_array[rel->relid];
	Assert(appinfo != NULL);

	return translate_col_privs(parent_cols, appinfo->translated_vars);
}

/*
 * expand_appendrel_subquery
 *		为 appendrel 基关系的子项添加 other rel 的 RelOptInfos
 *
 * rel 是设置了 rte->inh 标志的子查询关系，表示它
 * 是已被展平成 appendrel 的 UNION ALL 子查询，
 * 子查询列在 root->append_rel_list 中。我们需要为
 * 每个子关系建立一个 RelOptInfo，以便规划对它们的扫描。
 */
static void
expand_appendrel_subquery(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte, Index rti)
{
	ListCell   *l;

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		Index		childRTindex = appinfo->child_relid;
		RangeTblEntry *childrte;
		RelOptInfo *childrel;

		/* append_rel_list 含有全部 append 关系；忽略其他的 */
		if (appinfo->parent_relid != rti)
			continue;

		/* 找到应当已经存在的子 RTE */
		Assert(childRTindex < root->simple_rel_array_size);
		childrte = root->simple_rte_array[childRTindex];
		Assert(childrte != NULL);

		/* 建立子 RelOptInfo。 */
		childrel = build_simple_rel(root, childRTindex, rel);

		/* 子项自身也可能是继承关系，表或子查询皆可。 */
		if (childrte->inh)
			expand_inherited_rtentry(root, childrel, childrte, childRTindex);
	}
}


/*
 * apply_child_basequals
 *		用父关系的限定条件填充子关系的基限制限定条件，
 *		并用 appinfo 翻译它们。
 *
 * 若任何结果子句求值为常量假或 NULL，我们
 * 返回假并且不应用任何限定条件。调用者这时应把该关系标成
 * 哑关系，因为它不需要被扫描。
 */
bool
apply_child_basequals(PlannerInfo *root, RelOptInfo *parentrel,
					  RelOptInfo *childrel, RangeTblEntry *childRTE,
					  AppendRelInfo *appinfo)
{
	List	   *childquals;
	Index		cq_min_security;
	ListCell   *lc;

	/*
	 * 子关系的目标列表可能含有非 Var 表达式，这
	 * 意味着代入限定条件后可能出现常量简化的机会，
	 * 甚至伪常量限定条件。因此，
	 * 分别变换每个 RestrictInfo，看它是否归约为
	 * 常量或伪常量。（必须分别处理，以
	 * 跟踪每条限定条件的安全级别。）
	 */
	childquals = NIL;
	cq_min_security = UINT_MAX;
	foreach(lc, parentrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *childqual;
		ListCell   *lc2;

		Assert(IsA(rinfo, RestrictInfo));
		childqual = adjust_appendrel_attrs(root,
										   (Node *) rinfo->clause,
										   1, &appinfo);
		childqual = eval_const_expressions(root, childqual);
		/* 检查是否为彻头彻尾的常量 */
		if (childqual && IsA(childqual, Const))
		{
			if (((Const *) childqual)->constisnull ||
				!DatumGetBool(((Const *) childqual)->constvalue))
			{
				/* 限制归约为常量 FALSE 或 NULL */
				return false;
			}
			/* 限制归约为常量 TRUE，因此丢掉它 */
			continue;
		}
		/* 也许得到了一个 AND 子句，若是则展平它 */
		foreach(lc2, make_ands_implicit((Expr *) childqual))
		{
			Node	   *onecq = (Node *) lfirst(lc2);
			bool		pseudoconstant;

			/* 检查是否为伪常量（没有 Var 或易变函数） */
			pseudoconstant =
				!contain_vars_of_level(onecq, 0) &&
				!contain_volatile_functions(onecq);
			if (pseudoconstant)
			{
				/* 告诉 createplan.c 检查门控限定条件 */
				root->hasPseudoConstantQuals = true;
			}
			/* 用适当的属性重新组成 RestrictInfo */
			childquals = lappend(childquals,
								 make_restrictinfo(root,
												   (Expr *) onecq,
												   rinfo->is_pushed_down,
												   rinfo->has_clone,
												   rinfo->is_clone,
												   pseudoconstant,
												   rinfo->security_level,
												   NULL, NULL, NULL));
			/* 跟踪子限定条件中的最低安全级别 */
			cq_min_security = Min(cq_min_security, rinfo->security_level);
		}
	}

	/*
	 * 除了从父项继承的限定条件，我们可能有
	 * 与这个特定子节点关联的 securityQuals。（目前
	 * 这只可能发生在源自 UNION ALL 的 appendrel 中；
	 * 继承子表没有自己的 securityQuals，见
	 * expand_single_inheritance_child()。）把任何这种 securityQuals 拉
	 * 进该子项的 baserestrictinfo。这类似于对父关系做的
	 * process_security_barrier_quals()，只是我们
	 * 不能从这种限定条件做一般推论，因为它们对
	 * 整个 appendrel 并不成立。
	 */
	if (childRTE->securityQuals)
	{
		Index		security_level = 0;

		foreach(lc, childRTE->securityQuals)
		{
			List	   *qualset = (List *) lfirst(lc);
			ListCell   *lc2;

			foreach(lc2, qualset)
			{
				Expr	   *qual = (Expr *) lfirst(lc2);

				/* 这里不太可能看到常量，因此不检查 */
				childquals = lappend(childquals,
									 make_restrictinfo(root, qual,
													   true,
													   false, false,
													   false,
													   security_level,
													   NULL, NULL, NULL));
				cq_min_security = Min(cq_min_security, security_level);
			}
			security_level++;
		}
		Assert(security_level <= root->qual_security_level);
	}

	/*
	 * 好，已经得到这个子项的全部 baserestrictinfo 限定条件。
	 */
	childrel->baserestrictinfo = childquals;
	childrel->baserestrict_min_security = cq_min_security;

	return true;
}
