/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c
 *	  routines for defining a rewrite rule
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteDefine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_rewrite.h"
#include "catalog/storage.h"
#include "commands/policy.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_utilcmd.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


static void checkRuleResultList(List *targetList, TupleDesc resultDesc,
								bool isSelect, bool requireColumnNameMatch);
static bool setRuleCheckAsUser_walker(Node *node, Oid *context);
static void setRuleCheckAsUser_Query(Query *qry, Oid userid);


/*
 * InsertRule -
 *	  把这些参数作为一行插入系统关系
 *	  "pg_rewrite"。
 */
static Oid
InsertRule(const char *rulname,
		   int evtype,
		   Oid eventrel_oid,
		   bool evinstead,
		   Node *event_qual,
		   List *action,
		   bool replace)
{
	char	   *evqual = nodeToString(event_qual);
	char	   *actiontree = nodeToString((Node *) action);
	Datum		values[Natts_pg_rewrite];
	bool		nulls[Natts_pg_rewrite] = {0};
	NameData	rname;
	Relation	pg_rewrite_desc;
	HeapTuple	tup,
				oldtup;
	Oid			rewriteObjectId;
	ObjectAddress myself,
				referenced;
	bool		is_update = false;

	/*
	 * 设置 *nulls 和 *values 数组
	 */
	namestrcpy(&rname, rulname);
	values[Anum_pg_rewrite_rulename - 1] = NameGetDatum(&rname);
	values[Anum_pg_rewrite_ev_class - 1] = ObjectIdGetDatum(eventrel_oid);
	values[Anum_pg_rewrite_ev_type - 1] = CharGetDatum(evtype + '0');
	values[Anum_pg_rewrite_ev_enabled - 1] = CharGetDatum(RULE_FIRES_ON_ORIGIN);
	values[Anum_pg_rewrite_is_instead - 1] = BoolGetDatum(evinstead);
	values[Anum_pg_rewrite_ev_qual - 1] = CStringGetTextDatum(evqual);
	values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(actiontree);

	/*
	 * 准备存放新的 pg_rewrite 元组
	 */
	pg_rewrite_desc = table_open(RewriteRelationId, RowExclusiveLock);

	/*
	 * 检查是否在替换一条已有元组
	 */
	oldtup = SearchSysCache2(RULERELNAME,
							 ObjectIdGetDatum(eventrel_oid),
							 PointerGetDatum(rulname));

	if (HeapTupleIsValid(oldtup))
	{
		bool		replaces[Natts_pg_rewrite] = {0};

		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("rule \"%s\" for relation \"%s\" already exists",
							rulname, get_rel_name(eventrel_oid))));

		/*
		 * 替换时，不必替换每一个属性
		 */
		replaces[Anum_pg_rewrite_ev_type - 1] = true;
		replaces[Anum_pg_rewrite_is_instead - 1] = true;
		replaces[Anum_pg_rewrite_ev_qual - 1] = true;
		replaces[Anum_pg_rewrite_ev_action - 1] = true;

		tup = heap_modify_tuple(oldtup, RelationGetDescr(pg_rewrite_desc),
								values, nulls, replaces);

		CatalogTupleUpdate(pg_rewrite_desc, &tup->t_self, tup);

		ReleaseSysCache(oldtup);

		rewriteObjectId = ((Form_pg_rewrite) GETSTRUCT(tup))->oid;
		is_update = true;
	}
	else
	{
		rewriteObjectId = GetNewOidWithIndex(pg_rewrite_desc,
											 RewriteOidIndexId,
											 Anum_pg_rewrite_oid);
		values[Anum_pg_rewrite_oid - 1] = ObjectIdGetDatum(rewriteObjectId);

		tup = heap_form_tuple(pg_rewrite_desc->rd_att, values, nulls);

		CatalogTupleInsert(pg_rewrite_desc, tup);
	}


	heap_freetuple(tup);

	/* 若在替换，先去掉旧依赖，再建立新依赖 */
	if (is_update)
		deleteDependencyRecordsFor(RewriteRelationId, rewriteObjectId, false);

	/*
	 * 为规则所在关系安装依赖，确保关系删除时
	 * 规则随之消失。若规则是 ON SELECT，则把该依赖
	 * 做成隐式的，以免删除视图的 SELECT 规则。其他种类
	 * 的规则可以是 AUTO。
	 */
	myself.classId = RewriteRelationId;
	myself.objectId = rewriteObjectId;
	myself.objectSubId = 0;

	referenced.classId = RelationRelationId;
	referenced.objectId = eventrel_oid;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced,
					   (evtype == CMD_SELECT) ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

	/*
	 * 同时为动作和限定条件里引用的对象安装依赖。
	 */
	recordDependencyOnExpr(&myself, (Node *) action, NIL,
						   DEPENDENCY_NORMAL);

	if (event_qual != NULL)
	{
		/* 查找含 OLD/NEW 范围表项的查询 */
		Query	   *qry = linitial_node(Query, action);

		qry = getInsertSelectQuery(qry, NULL);
		recordDependencyOnExpr(&myself, event_qual, qry->rtable,
							   DEPENDENCY_NORMAL);
	}

	/* 新规则的创建后钩子 */
	InvokeObjectPostCreateHook(RewriteRelationId, rewriteObjectId, 0);

	table_close(pg_rewrite_desc, RowExclusiveLock);

	return rewriteObjectId;
}

/*
 * DefineRule
 *		执行一条 CREATE RULE 命令。
 */
ObjectAddress
DefineRule(RuleStmt *stmt, const char *queryString)
{
	List	   *actions;
	Node	   *whereClause;
	Oid			relId;

	/* 解析分析。 */
	transformRuleStmt(stmt, queryString, &actions, &whereClause);

	/*
	 * 查找并锁定该关系。锁级别应当与
	 * DefineQueryRewrite 一致。
	 */
	relId = RangeVarGetRelid(stmt->relation, AccessExclusiveLock, false);

	/* ……然后执行 */
	return DefineQueryRewrite(stmt->rulename,
							  relId,
							  whereClause,
							  stmt->event,
							  stmt->instead,
							  stmt->replace,
							  actions);
}


/*
 * DefineQueryRewrite
 *		创建一条规则。
 *
 * 这本质上与 DefineRule() 相同，只是规则的
 * 动作和限定条件已经过了解析分析。
 */
ObjectAddress
DefineQueryRewrite(const char *rulename,
				   Oid event_relid,
				   Node *event_qual,
				   CmdType event_type,
				   bool is_instead,
				   bool replace,
				   List *action)
{
	Relation	event_relation;
	ListCell   *l;
	Query	   *query;
	Oid			ruleId = InvalidOid;
	ObjectAddress address;

	/*
	 * 若正在安装一条 ON SELECT 规则，最好取得
	 * AccessExclusiveLock，以确保当前没有 SELECTs 正在该
	 * 事件关系上运行。对其他类型的规则，取得
	 * ShareRowExclusiveLock 就足以锁住插入、更新和删除动作，并
	 * 确保锁住当前的 CREATE RULE 语句；但由于
	 * 访问系统目录项时存在竞争条件，目前还没法那样做。
	 *
	 * 注意：该锁级别应当与 DefineRule 中使用的一致。
	 */
	event_relation = table_open(event_relid, AccessExclusiveLock);

	/*
	 * 验证该关系的类型适合应用规则。
	 * 内部调用者可以针对物化视图，但 transformRuleStmt()
	 * 会拦住用户。错误信息里不要提到它们。
	 */
	if (event_relation->rd_rel->relkind != RELKIND_RELATION &&
		event_relation->rd_rel->relkind != RELKIND_MATVIEW &&
		event_relation->rd_rel->relkind != RELKIND_VIEW &&
		event_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have rules",
						RelationGetRelationName(event_relation)),
				 errdetail_relkind_not_supported(event_relation->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(event_relation))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(event_relation))));

	/*
	 * 检查用户是否有权对这个关系应用规则。
	 */
	if (!object_ownercheck(RelationRelationId, event_relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(event_relation->rd_rel->relkind),
					   RelationGetRelationName(event_relation));

	/*
	 * 没有会修改 OLD 或 NEW 的规则动作
	 */
	foreach(l, action)
	{
		query = lfirst_node(Query, l);
		if (query->resultRelation == 0)
			continue;
		/* 不要被 INSERT/SELECT 迷惑 */
		if (query != getInsertSelectQuery(query, NULL))
			continue;
		if (query->resultRelation == PRS2_OLD_VARNO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule actions on OLD are not implemented"),
					 errhint("Use views or triggers instead.")));
		if (query->resultRelation == PRS2_NEW_VARNO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule actions on NEW are not implemented"),
					 errhint("Use triggers instead.")));
	}

	if (event_type == CMD_SELECT)
	{
		/*
		 * ON SELECT 规则仅限于视图定义。
		 *
		 * 因此这最好是一个视图，……
		 */
		if (event_relation->rd_rel->relkind != RELKIND_VIEW &&
			event_relation->rd_rel->relkind != RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("relation \"%s\" cannot have ON SELECT rules",
							RelationGetRelationName(event_relation)),
					 errdetail_relkind_not_supported(event_relation->rd_rel->relkind)));

		/*
		 * ……这里无法有 INSTEAD NOTHING，……
		 */
		if (action == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD NOTHING rules on SELECT are not implemented"),
					 errhint("Use views instead.")));

		/*
		 * ……这里无法有多个动作，……
		 */
		if (list_length(action) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multiple actions for rules on SELECT are not implemented")));

		/*
		 * ……那一个动作必须是 SELECT，……
		 */
		query = linitial_node(Query, action);
		if (!is_instead ||
			query->commandType != CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rules on SELECT must have action INSTEAD SELECT")));

		/*
		 * ……它无法包含会修改数据的 WITH……
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rules on SELECT must not contain data-modifying statements in WITH")));

		/*
		 * ……不能有规则限定条件，……
		 */
		if (event_qual != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("event qualifications are not implemented for rules on SELECT")));

		/*
		 * ……SELECT 动作的目标列表必须与
		 * 事件关系完全匹配，……
		 */
		checkRuleResultList(query->targetList,
							RelationGetDescr(event_relation),
							true,
							event_relation->rd_rel->relkind !=
							RELKIND_MATVIEW);

		/*
		 * ……不得已经存在另一条 ON SELECT 规则……
		 */
		if (!replace && event_relation->rd_rules != NULL)
		{
			int			i;

			for (i = 0; i < event_relation->rd_rules->numLocks; i++)
			{
				RewriteRule *rule;

				rule = event_relation->rd_rules->rules[i];
				if (rule->event == CMD_SELECT)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("\"%s\" is already a view",
									RelationGetRelationName(event_relation))));
			}
		}

		/*
		 * ……最后，该规则必须命名为 _RETURN。
		 */
		if (strcmp(rulename, ViewSelectRuleName) != 0)
		{
			/*
			 * 在 7.3 之前的版本里，预期名称是 _RETviewname。为了
			 * 与旧的 pg_dump 输出向后兼容，接受该名称
			 * 并静默地把它改成 _RETURN。由于这只是一个临时的
			 * 向后兼容权宜做法，把检查的字符数限制在比
			 * NAMEDATALEN 少几个；这样就不必
			 * 担心多字节字符可能在何处被
			 * 截断。
			 */
			if (strncmp(rulename, "_RET", 4) != 0 ||
				strncmp(rulename + 4, RelationGetRelationName(event_relation),
						NAMEDATALEN - 4 - 4) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("view rule for \"%s\" must be named \"%s\"",
								RelationGetRelationName(event_relation),
								ViewSelectRuleName)));
			rulename = pstrdup(ViewSelectRuleName);
		}
	}
	else
	{
		/*
		 * 对非 SELECT 规则，RETURNING 列表最多只能出现在其中一个
		 * 动作里……而且在条件规则或非 INSTEAD 规则里根本不能有
		 * 任何 RETURNING 列表。（实际上，同一事件上的所有规则
		 * 加起来最多只能有一个 RETURNING 列表，但似乎最好在
		 * 规则展开时再强制这一点。）若存在
		 * RETURNING 列表，它必须匹配事件关系。
		 */
		bool		haveReturning = false;

		foreach(l, action)
		{
			query = lfirst_node(Query, l);

			if (!query->returningList)
				continue;
			if (haveReturning)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot have multiple RETURNING lists in a rule")));
			haveReturning = true;
			if (event_qual != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RETURNING lists are not supported in conditional rules")));
			if (!is_instead)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("RETURNING lists are not supported in non-INSTEAD rules")));
			checkRuleResultList(query->returningList,
								RelationGetDescr(event_relation),
								false, false);
		}

		/*
		 * 最后，若它不是 ON SELECT 规则，则它不得
		 * 被命名为 _RETURN。这可以防止意外或恶意地用
		 * 其他种类的规则替换视图的 ON SELECT 规则。
		 */
		if (strcmp(rulename, ViewSelectRuleName) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("non-view rule for \"%s\" must not be named \"%s\"",
							RelationGetRelationName(event_relation),
							ViewSelectRuleName)));
	}

	/*
	 * 这条规则是允许的：准备安装它。
	 */

	/* 若它是空动作且不是 INSTEAD，就丢弃该规则；它是空操作 */
	if (action != NIL || is_instead)
	{
		ruleId = InsertRule(rulename,
							event_type,
							event_relid,
							is_instead,
							event_qual,
							action,
							replace);

		/*
		 * 把事件关系的 pg_class relhasrules 成员设为真。
		 *
		 * 重要的副作用：会广播一条 SI 通知，迫使所有
		 * 后端（包括我自己！）用这条新规则更新
		 * relcache 项。
		 */
		SetRelationRuleStatus(event_relid, true);
	}

	ObjectAddressSet(address, RewriteRelationId, ruleId);

	/* 关闭关系，但把锁保持到提交…… */
	table_close(event_relation, NoLock);

	return address;
}

/*
 * checkRuleResultList
 *		验证 targetList 产生的输出与 tupledesc 兼容。
 *
 * targetList 可能是 SELECT 目标列表，也可能是 RETURNING 列表；
 * isSelect 表明是哪一种。这用来选择错误信息。
 *
 * SELECT 目标列表可以可选地需要列名匹配。
 */
static void
checkRuleResultList(List *targetList, TupleDesc resultDesc, bool isSelect,
					bool requireColumnNameMatch)
{
	ListCell   *tllist;
	int			i;

	/* 只有 SELECT 才可能需要列名匹配。 */
	Assert(isSelect || !requireColumnNameMatch);

	i = 0;
	foreach(tllist, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tllist);
		Oid			tletypid;
		int32		tletypmod;
		Form_pg_attribute attr;
		char	   *attname;

		/* resjunk 项可以忽略 */
		if (tle->resjunk)
			continue;
		i++;
		if (i > resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target list has too many entries") :
					 errmsg("RETURNING list has too many entries")));

		attr = TupleDescAttr(resultDesc, i - 1);
		attname = NameStr(attr->attname);

		/*
		 * 不允许关系里存在已删除的列。创建 ON SELECT
		 * 规则时，这种情况其实不在预期之中。若有人试图把含
		 * 已删除列的关系转换成视图，就可能出现，但我们
		 * 关心支持的表到视图转换情形只针对 pg_dump，而
		 * pg_dump 不会那样做。
		 *
		 * 不幸的是，给普通表添加带 RETURNING 的规则时，
		 * 也可能出现这种情况，而拒绝那种情况
		 * 要麻烦得多。原则上我们可以支持它：修改目标列表，
		 * 纳入与 tupdesc 中已删除列对应的
		 * 哑 NULL 列。然而，ruleutils.c 之类的地方
		 * 必须改成不去处理这些项，而这会带来不确定、
		 * 可能相当大的工作量。（注意，无法靠把这些哑
		 * 列标成 resjunk 来回避，因为预期要对应表列的
		 * 恰恰是那些非 resjunk 的目标列表
		 * 列。）
		 */
		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 isSelect ?
					 errmsg("cannot convert relation containing dropped columns to view") :
					 errmsg("cannot create a RETURNING list for a relation containing dropped columns")));

		/* 若有要求则检查名称匹配；这里不需要两段错误文本 */
		if (requireColumnNameMatch && strcmp(tle->resname, attname) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("SELECT rule's target entry %d has different column name from column \"%s\"",
							i, attname),
					 errdetail("SELECT target entry is named \"%s\".",
							   tle->resname)));

		/* 检查类型匹配。 */
		tletypid = exprType((Node *) tle->expr);
		if (attr->atttypid != tletypid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target entry %d has different type from column \"%s\"",
							i, attname) :
					 errmsg("RETURNING list's entry %d has different type from column \"%s\"",
							i, attname),
					 isSelect ?
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_be(tletypid),
							   format_type_be(attr->atttypid)) :
					 errdetail("RETURNING list entry has type %s, but column has type %s.",
							   format_type_be(tletypid),
							   format_type_be(attr->atttypid))));

		/*
		 * 仅当其中一个 typmod 为 -1（即
		 * "未指定"）时，才允许 typmod 不同。这对 "numeric"
		 * 这类情况是必要的：表会有填好的默认长度，而 select
		 * 规则的表达式的 typmod 多半是 -1。
		 */
		tletypmod = exprTypmod((Node *) tle->expr);
		if (attr->atttypmod != tletypmod &&
			attr->atttypmod != -1 && tletypmod != -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 isSelect ?
					 errmsg("SELECT rule's target entry %d has different size from column \"%s\"",
							i, attname) :
					 errmsg("RETURNING list's entry %d has different size from column \"%s\"",
							i, attname),
					 isSelect ?
					 errdetail("SELECT target entry has type %s, but column has type %s.",
							   format_type_with_typemod(tletypid, tletypmod),
							   format_type_with_typemod(attr->atttypid,
														attr->atttypmod)) :
					 errdetail("RETURNING list entry has type %s, but column has type %s.",
							   format_type_with_typemod(tletypid, tletypmod),
							   format_type_with_typemod(attr->atttypid,
														attr->atttypmod))));
	}

	if (i != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 isSelect ?
				 errmsg("SELECT rule's target list has too few entries") :
				 errmsg("RETURNING list has too few entries")));
}

/*
 * setRuleCheckAsUser
 *		递归扫描查询或表达式树，把该查询所有
 *		RTEPermissionInfos 里的 checkAsUser 成员设为给定的用户 ID。
 */
void
setRuleCheckAsUser(Node *node, Oid userid)
{
	(void) setRuleCheckAsUser_walker(node, &userid);
}

static bool
setRuleCheckAsUser_walker(Node *node, Oid *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		setRuleCheckAsUser_Query((Query *) node, *context);
		return false;
	}
	return expression_tree_walker(node, setRuleCheckAsUser_walker,
								  (void *) context);
}

static void
setRuleCheckAsUser_Query(Query *qry, Oid userid)
{
	ListCell   *l;

	/* 为该查询的所有 RTEPermissionInfos 设置。 */
	foreach(l, qry->rteperminfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, l);

		perminfo->checkAsUser = userid;
	}

	/* 现在递归到任何子查询 RTEs */
	foreach(l, qry->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_SUBQUERY)
			setRuleCheckAsUser_Query(rte->subquery, userid);
	}

	/* 递归进入 WITH 中的子查询 */
	foreach(l, qry->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		setRuleCheckAsUser_Query(castNode(Query, cte->ctequery), userid);
	}

	/* 若有子链接，就搜索它们并处理其 RTEs */
	if (qry->hasSubLinks)
		query_tree_walker(qry, setRuleCheckAsUser_walker, (void *) &userid,
						  QTW_IGNORE_RC_SUBQUERIES);
}


/*
 * 改变一条已有规则的触发语义。
 */
void
EnableDisableRule(Relation rel, const char *rulename,
				  char fires_when)
{
	Relation	pg_rewrite_desc;
	Oid			owningRel = RelationGetRelid(rel);
	Oid			eventRelationOid;
	HeapTuple	ruletup;
	Form_pg_rewrite ruleform;
	bool		changed = false;

	/*
	 * 查找要修改的规则元组。
	 */
	pg_rewrite_desc = table_open(RewriteRelationId, RowExclusiveLock);
	ruletup = SearchSysCacheCopy2(RULERELNAME,
								  ObjectIdGetDatum(owningRel),
								  PointerGetDatum(rulename));
	if (!HeapTupleIsValid(ruletup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						rulename, get_rel_name(owningRel))));

	ruleform = (Form_pg_rewrite) GETSTRUCT(ruletup);

	/*
	 * 验证用户拥有适当的权限。
	 */
	eventRelationOid = ruleform->ev_class;
	Assert(eventRelationOid == owningRel);
	if (!object_ownercheck(RelationRelationId, eventRelationOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(eventRelationOid)),
					   get_rel_name(eventRelationOid));

	/*
	 * 若 ev_enabled 与想要的新状态不同，就修改它。
	 */
	if (DatumGetChar(ruleform->ev_enabled) !=
		fires_when)
	{
		ruleform->ev_enabled = CharGetDatum(fires_when);
		CatalogTupleUpdate(pg_rewrite_desc, &ruletup->t_self, ruletup);

		changed = true;
	}

	InvokeObjectPostAlterHook(RewriteRelationId, ruleform->oid, 0);

	heap_freetuple(ruletup);
	table_close(pg_rewrite_desc, RowExclusiveLock);

	/*
	 * 若有改动，就广播一条 SI 失效消息，迫使每个
	 * 后端（包括我们自己！）重建该关系的 relcache 项。
	 * 否则它们没法及时应用这项改动。
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * 在获取关系锁之前执行权限和完整性检查。
 */
static void
RangeVarCallbackForRenameRule(const RangeVar *rv, Oid relid, Oid oldrelid,
							  void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除 */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* 只有表和视图才能有规则 */
	if (form->relkind != RELKIND_RELATION &&
		form->relkind != RELKIND_VIEW &&
		form->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have rules", rv->relname),
				 errdetail_relkind_not_supported(form->relkind)));

	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/* 要重命名它的一条规则，你必须拥有该表 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	ReleaseSysCache(tuple);
}

/*
 * 重命名一条已有的重写规则。
 */
ObjectAddress
RenameRewriteRule(RangeVar *relation, const char *oldName,
				  const char *newName)
{
	Oid			relid;
	Relation	targetrel;
	Relation	pg_rewrite_desc;
	HeapTuple	ruletup;
	Form_pg_rewrite ruleform;
	Oid			ruleOid;
	ObjectAddress address;

	/*
	 * 查找名称、检查权限并获取锁（我们不会
	 * 在事务结束前释放它）。
	 */
	relid = RangeVarGetRelidExtended(relation, AccessExclusiveLock,
									 0,
									 RangeVarCallbackForRenameRule,
									 NULL);

	/* 已经持有锁，因此只需建立 relcache 项。 */
	targetrel = relation_open(relid, NoLock);

	/* 准备修改 pg_rewrite */
	pg_rewrite_desc = table_open(RewriteRelationId, RowExclusiveLock);

	/* 取出该规则的项（它应当存在） */
	ruletup = SearchSysCacheCopy2(RULERELNAME,
								  ObjectIdGetDatum(relid),
								  PointerGetDatum(oldName));
	if (!HeapTupleIsValid(ruletup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" does not exist",
						oldName, RelationGetRelationName(targetrel))));
	ruleform = (Form_pg_rewrite) GETSTRUCT(ruletup);
	ruleOid = ruleform->oid;

	/* 新名称的规则不应当已经存在 */
	if (IsDefinedRewriteRule(relid, newName))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("rule \"%s\" for relation \"%s\" already exists",
						newName, RelationGetRelationName(targetrel))));

	/*
	 * 不允许重命名 ON SELECT 规则，因为它们应当始终
	 * 被命名为 "_RETURN"。
	 */
	if (ruleform->ev_type == CMD_SELECT + '0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("renaming an ON SELECT rule is not allowed")));

	/* 可以了，执行更新 */
	namestrcpy(&(ruleform->rulename), newName);

	CatalogTupleUpdate(pg_rewrite_desc, &ruletup->t_self, ruletup);

	InvokeObjectPostAlterHook(RewriteRelationId, ruleOid, 0);

	heap_freetuple(ruletup);
	table_close(pg_rewrite_desc, RowExclusiveLock);

	/*
	 * 使该关系的 relcache 项失效，以便其他后端（以及
	 * 本后端！）收到 SI 消息，从而重建 relcache 项。
	 * （理想情况下这应当自动发生……）
	 */
	CacheInvalidateRelcache(targetrel);

	ObjectAddressSet(address, RewriteRelationId, ruleOid);

	/*
	 * 关闭关系，但保持排他锁！
	 */
	relation_close(targetrel, NoLock);

	return address;
}
