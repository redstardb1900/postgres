/*-------------------------------------------------------------------------
 *
 * view.c
 *	  使用重写规则构造视图
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/view.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void checkViewTupleDesc(TupleDesc newdesc, TupleDesc olddesc);

/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * 创建视图关系，并使用规则系统存储视图的
 * 查询。
 *
 * 必须先调用 EventTriggerAlterTableStart。
 *---------------------------------------------------------------------
 */
static ObjectAddress
DefineVirtualRelation(RangeVar *relation, List *tlist, bool replace,
					  List *options, Query *viewParse)
{
	Oid			viewOid;
	LOCKMODE	lockmode;
	CreateStmt *createStmt = makeNode(CreateStmt);
	List	   *attrList;
	ListCell   *t;

	/*
	 * 根据视图 SELECT 列表中（非垃圾）targetlist 项的名称和
	 * 类型创建 ColumnDef 节点列表。
	 */
	attrList = NIL;
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *def = makeColumnDef(tle->resname,
											exprType((Node *) tle->expr),
											exprTypmod((Node *) tle->expr),
											exprCollation((Node *) tle->expr));

			/*
			 * 该列可能属于可排序类型，但排序规则
			 * 无法解析，所以要复查。
			 */
			if (type_is_collatable(exprType((Node *) tle->expr)))
			{
				if (!OidIsValid(def->collOid))
					ereport(ERROR,
							(errcode(ERRCODE_INDETERMINATE_COLLATION),
							 errmsg("could not determine which collation to use for view column \"%s\"",
									def->colname),
							 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			else
				Assert(!OidIsValid(def->collOid));

			attrList = lappend(attrList, def);
		}
	}

	/*
	 * 查找、检查权限并锁定创建命名空间；同时检查是否
	 * 已有同名视图。若选中的命名空间是临时的，
	 * 这还会把 relation->relpersistence 设为
	 * RELPERSISTENCE_TEMP。
	 */
	lockmode = replace ? AccessExclusiveLock : NoLock;
	(void) RangeVarGetAndCheckCreationNamespace(relation, lockmode, &viewOid);

	if (OidIsValid(viewOid) && replace)
	{
		Relation	rel;
		TupleDesc	descriptor;
		List	   *atcmds = NIL;
		AlterTableCmd *atcmd;
		ObjectAddress address;

		/* 关系已锁定，但我们必须构建 relcache 条目。 */
		rel = relation_open(viewOid, NoLock);

		/* 确保它*确实*是视图。 */
		if (rel->rd_rel->relkind != RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a view",
							RelationGetRelationName(rel))));

		/* 还要检查它没有正在被使用 */
		CheckTableNotInUse(rel, "CREATE OR REPLACE VIEW");

		/*
		 * 由于临时对象的命名空间可见性规则，我们最终
		 * 只会用一个临时视图替换另一个临时视图，
		 * 永久视图也类似。
		 */
		Assert(relation->relpersistence == rel->rd_rel->relpersistence);

		/*
		 * 创建元组描述符与现有视图比较，并验证
		 * 旧列列表是新列列表的
		 * 初始前缀。
		 */
		descriptor = BuildDescForRelation(attrList);
		checkViewTupleDesc(descriptor, rel->rd_att);

		/*
		 * 若添加了新属性，我们必须为它们添加 pg_attribute
		 * 条目。使用 ALTER TABLE ADD COLUMN 基础设施做这件事
		 * 很方便（虽然有些大材小用）。
		 *
		 * 注意我们必须在更新视图查询之前做这件事，
		 * 因为规则系统需要正确的视图列在定义
		 * 新规则时已经就位。
		 *
		 * 还要注意 ALTER TABLE 不对 AT_AddColumnToView 命令
		 * 运行解析变换。我们提供的 ColumnDef 必须
		 * 可直接执行。
		 */
		if (list_length(attrList) > rel->rd_att->natts)
		{
			ListCell   *c;
			int			skip = rel->rd_att->natts;

			foreach(c, attrList)
			{
				if (skip > 0)
				{
					skip--;
					continue;
				}
				atcmd = makeNode(AlterTableCmd);
				atcmd->subtype = AT_AddColumnToView;
				atcmd->def = (Node *) lfirst(c);
				atcmds = lappend(atcmds, atcmd);
			}

			/* EventTriggerAlterTableStart 由 ProcessUtilitySlow 调用 */
			AlterTableInternal(viewOid, atcmds, true);

			/* 使新的视图列可见 */
			CommandCounterIncrement();
		}

		/*
		 * 更新视图的查询。
		 *
		 * 注意我们必须在更新视图选项之前做这件事，
		 * 因为新选项可能与旧视图查询不兼容
		 * （例如若我们试图添加 WITH CHECK OPTION，
		 * 新视图就需要可自动更新，但旧视图
		 * 可能不可以）。
		 */
		StoreViewQuery(viewOid, viewParse, replace);

		/* 使新的视图查询可见 */
		CommandCounterIncrement();

		/*
		 * 更新视图的选项。
		 *
		 * 新的选项列表替换现有选项列表，
		 * 即使它为空。
		 */
		atcmd = makeNode(AlterTableCmd);
		atcmd->subtype = AT_ReplaceRelOptions;
		atcmd->def = (Node *) options;
		atcmds = list_make1(atcmd);

		/* EventTriggerAlterTableStart 由 ProcessUtilitySlow 调用 */
		AlterTableInternal(viewOid, atcmds, true);

		/*
		 * 这里要更新视图依赖的地方非常少。大多数
		 * 视图级依赖关系，比如与所有者、模式和关联
		 * 复合类型的关系，都不会改变。因为我们不允许
		 * 改变现有视图列的类型或排序规则，现有列
		 * 的那些依赖也不会改变，而 AT_AddColumnToView
		 * 机制已负责为新视图列添加此类依赖。视图
		 * 查询的依赖可能任意改变，但那已在
		 * StoreViewQuery 内部处理。剩下的只是
		 * 检查在创建扩展时是否允许
		 * 替换视图。
		 */
		ObjectAddressSet(address, RelationRelationId, viewOid);

		recordDependencyOnCurrentExtension(&address, true);

		/*
		 * 看来没问题，返回先前存在的视图的 OID。
		 */
		relation_close(rel, NoLock);	/* 保留锁！ */

		return address;
	}
	else
	{
		ObjectAddress address;

		/*
		 * 设置键/继承等的参数。这些对视图来说
		 * 都不重要……
		 */
		createStmt->relation = relation;
		createStmt->tableElts = attrList;
		createStmt->inhRelations = NIL;
		createStmt->constraints = NIL;
		createStmt->options = options;
		createStmt->oncommit = ONCOMMIT_NOOP;
		createStmt->tablespacename = NULL;
		createStmt->if_not_exists = false;

		/*
		 * 创建关系（若已存在视图会报错，所以若
		 * "replace" 为 false，我们不需要更多代码
		 * 来抱怨）。
		 */
		address = DefineRelation(createStmt, RELKIND_VIEW, InvalidOid, NULL,
								 NULL);
		Assert(address.objectId != InvalidOid);

		/* 使新的视图关系可见 */
		CommandCounterIncrement();

		/* 存储视图的查询 */
		StoreViewQuery(address.objectId, viewParse, replace);

		return address;
	}
}

/*
 * 验证与提议的新视图定义关联的 tupledesc 与旧视图的
 * tupledesc 匹配。这基本上是 equalTupleDescs() 的精简版，
 * 添加了生成具体抱怨的代码。另外，我们允许新
 * tupledesc 的列数多于旧的。
 */
static void
checkViewTupleDesc(TupleDesc newdesc, TupleDesc olddesc)
{
	int			i;

	if (newdesc->natts < olddesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop columns from view")));

	for (i = 0; i < olddesc->natts; i++)
	{
		Form_pg_attribute newattr = TupleDescAttr(newdesc, i);
		Form_pg_attribute oldattr = TupleDescAttr(olddesc, i);

		/* XXX 消息不对，但反正我们也不支持对视图 DROP COL */
		if (newattr->attisdropped != oldattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop columns from view")));

		if (strcmp(NameStr(newattr->attname), NameStr(oldattr->attname)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change name of view column \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							NameStr(newattr->attname)),
					 errhint("Use ALTER VIEW ... RENAME COLUMN ... to change name of view column instead.")));

		/*
		 * 类型、typmod 或排序规则不允许改变，因为这些
		 * 属性可能嵌入在引用本视图的其他视图/规则的
		 * Var 中。其他列属性可以忽略。
		 */
		if (newattr->atttypid != oldattr->atttypid ||
			newattr->atttypmod != oldattr->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change data type of view column \"%s\" from %s to %s",
							NameStr(oldattr->attname),
							format_type_with_typemod(oldattr->atttypid,
													 oldattr->atttypmod),
							format_type_with_typemod(newattr->atttypid,
													 newattr->atttypmod))));

		/*
		 * 此时，attcollations 应两者都有效或两者都无效，
		 * 所以无条件应用 get_collation_name 应该没问题。
		 */
		if (newattr->attcollation != oldattr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change collation of view column \"%s\" from \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							get_collation_name(oldattr->attcollation),
							get_collation_name(newattr->attcollation))));
	}

	/*
	 * 我们忽略约束字段。新视图 desc 不可能有任何约束，
	 * 旧视图上可能有的唯一约束是默认值，我们乐意
	 * 让它们保留原位。
	 */
}

static void
DefineViewRules(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * 设置 ON SELECT 规则。由于查询已经经过解析
	 * 分析，我们直接使用 DefineQueryRewrite()。
	 */
	DefineQueryRewrite(pstrdup(ViewSelectRuleName),
					   viewOid,
					   NULL,
					   CMD_SELECT,
					   true,
					   replace,
					   list_make1(viewParse));

	/*
	 * 有朝一日：自动 ON INSERT 等
	 */
}

/*
 * DefineView
 *		执行 CREATE VIEW 命令。
 */
ObjectAddress
DefineView(ViewStmt *stmt, const char *queryString,
		   int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	Query	   *viewParse;
	RangeVar   *view;
	ListCell   *cell;
	bool		check_option;
	ObjectAddress address;

	/*
	 * 运行解析分析，把原始解析树转换为 Query。注意这还会
	 * 在源 table(s) 上获取足够的锁。
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	viewParse = parse_analyze_fixedparams(rawstmt, queryString, NULL, 0, NULL);

	/*
	 * 语法应确保结果是单个 SELECT Query。
	 * 但它不禁止 SELECT INTO，所以我们必须检查这一点。
	 */
	if (!IsA(viewParse, Query))
		elog(ERROR, "unexpected parse analysis result");
	if (viewParse->utilityStmt != NULL &&
		IsA(viewParse->utilityStmt, CreateTableAsStmt))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain SELECT INTO")));
	if (viewParse->commandType != CMD_SELECT)
		elog(ERROR, "unexpected parse analysis result");

	/*
	 * 检查不支持的情形。这些测试与 DefineQueryRewrite() 中的测试
	 * 重复，但那个函数会抱怨伪造的 ON SELECT 规则，
	 * 而我们宁愿消息抱怨的是视图。
	 */
	if (viewParse->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain data-modifying statements in WITH")));

	/*
	 * 若用户指定了 WITH CHECK OPTION，就把它加入
	 * reloptions 列表。
	 */
	if (stmt->withCheckOption == LOCAL_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("local"), -1));
	else if (stmt->withCheckOption == CASCADED_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("cascaded"), -1));

	/*
	 * 若指定了 WITH CHECK OPTION，检查视图
	 * 可自动更新。
	 */
	check_option = false;

	foreach(cell, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(cell);

		if (strcmp(defel->defname, "check_option") == 0)
			check_option = true;
	}

	/*
	 * 若指定了 check option，查看视图实际上
	 * 是否可自动更新。
	 */
	if (check_option)
	{
		const char *view_updatable_error =
			view_query_is_auto_updatable(viewParse, true);

		if (view_updatable_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
					 errhint("%s", _(view_updatable_error))));
	}

	/*
	 * 若给出了列名列表，就遍历并把它们插入实际的
	 * 查询树。- thomas 2000-03-08
	 */
	if (stmt->aliases != NIL)
	{
		ListCell   *alist_item = list_head(stmt->aliases);
		ListCell   *targetList;

		foreach(targetList, viewParse->targetList)
		{
			TargetEntry *te = lfirst_node(TargetEntry, targetList);

			/* 垃圾列不获得别名 */
			if (te->resjunk)
				continue;
			te->resname = pstrdup(strVal(lfirst(alist_item)));
			alist_item = lnext(stmt->aliases, alist_item);
			if (alist_item == NULL)
				break;			/* 别名分配完毕 */
		}

		if (alist_item != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("CREATE VIEW specifies more column "
							"names than columns")));
	}

	/* 未记日志的视图没有意义。 */
	if (stmt->view->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("views cannot be unlogged because they do not have storage")));

	/*
	 * 若用户没有明确要求临时视图，检查我们是否
	 * 隐含需要一个。只要 CREATE 命令与之相符——没有
	 * 显式模式名——我们就允许自动
	 * 插入 TEMP。
	 */
	view = copyObject(stmt->view);	/* 不要破坏原始命令 */
	if (view->relpersistence == RELPERSISTENCE_PERMANENT
		&& isQueryUsingTempRelation(viewParse))
	{
		view->relpersistence = RELPERSISTENCE_TEMP;
		ereport(NOTICE,
				(errmsg("view \"%s\" will be a temporary view",
						view->relname)));
	}

	/*
	 * 创建视图关系
	 *
	 * 注意：若它已存在且 replace 为 false，
	 * 事务将被中止。
	 */
	address = DefineVirtualRelation(view, viewParse->targetList,
									stmt->replace, stmt->options, viewParse);

	return address;
}

/*
 * 使用规则系统存储视图的查询。
 */
void
StoreViewQuery(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * 现在创建与视图关联的规则。
	 */
	DefineViewRules(viewOid, viewParse, replace);
}
