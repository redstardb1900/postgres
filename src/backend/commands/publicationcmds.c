/*-------------------------------------------------------------------------
 *
 * publicationcmds.c
 *		发布操作            
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/commands/publicationcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/publicationcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_relation.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
  * 用于验证行过滤器表达式中列的信息。
  * 详见 contain_invalid_rfcolumn_walker。
 */
typedef struct rf_context
{
	Bitmapset  *bms_replident;	/* 复制标识列的位集 */
	bool		pubviaroot;		/* 验证父关系的行过滤器
								  * 则为 true */
	Oid			relid;			/* 关系的 relid */
	Oid			parentid;		/* 父关系的 relid */
} rf_context;

static List *OpenTableList(List *tables);
static void CloseTableList(List *rels);
static void LockSchemaList(List *schemalist);
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
								 AlterPublicationStmt *stmt);
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok);
static void PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
								  AlterPublicationStmt *stmt);
static void PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok);


static void
parse_publication_options(ParseState *pstate,
						  List *options,
						  bool *publish_given,
						  PublicationActions *pubactions,
						  bool *publish_via_partition_root_given,
						  bool *publish_via_partition_root)
{
	ListCell   *lc;

	*publish_given = false;
	*publish_via_partition_root_given = false;

	/* 默认值 */
	pubactions->pubinsert = true;
	pubactions->pubupdate = true;
	pubactions->pubdelete = true;
	pubactions->pubtruncate = true;
	*publish_via_partition_root = false;

	/* 解析选项 */
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "publish") == 0)
		{
			char	   *publish;
			List	   *publish_list;
			ListCell   *lc2;

			if (*publish_given)
				errorConflictingDefElem(defel, pstate);

			/*
			  * 若给出 publish 选项，仅发布显式列出的操作。
			  *
			 */
			pubactions->pubinsert = false;
			pubactions->pubupdate = false;
			pubactions->pubdelete = false;
			pubactions->pubtruncate = false;

			*publish_given = true;
			publish = defGetString(defel);

			if (!SplitIdentifierString(publish, ',', &publish_list))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid list syntax in parameter \"%s\"",
								"publish")));

			/* 处理选项列表。    */
			foreach(lc2, publish_list)
			{
				char	   *publish_opt = (char *) lfirst(lc2);

				if (strcmp(publish_opt, "insert") == 0)
					pubactions->pubinsert = true;
				else if (strcmp(publish_opt, "update") == 0)
					pubactions->pubupdate = true;
				else if (strcmp(publish_opt, "delete") == 0)
					pubactions->pubdelete = true;
				else if (strcmp(publish_opt, "truncate") == 0)
					pubactions->pubtruncate = true;
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("unrecognized value for publication option \"%s\": \"%s\"",
									"publish", publish_opt)));
			}
		}
		else if (strcmp(defel->defname, "publish_via_partition_root") == 0)
		{
			if (*publish_via_partition_root_given)
				errorConflictingDefElem(defel, pstate);
			*publish_via_partition_root_given = true;
			*publish_via_partition_root = defGetBoolean(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized publication parameter: \"%s\"", defel->defname)));
	}
}

/*
  * 将 PublicationObjSpecType 列表转换为模式 OID 列表和
  * PublicationTable 列表。
 */
static void
ObjectsInPublicationToOids(List *pubobjspec_list, ParseState *pstate,
						   List **rels, List **schemas)
{
	ListCell   *cell;
	PublicationObjSpec *pubobj;

	if (!pubobjspec_list)
		return;

	foreach(cell, pubobjspec_list)
	{
		Oid			schemaid;
		List	   *search_path;

		pubobj = (PublicationObjSpec *) lfirst(cell);

		switch (pubobj->pubobjtype)
		{
			case PUBLICATIONOBJ_TABLE:
				*rels = lappend(*rels, pubobj->pubtable);
				break;
			case PUBLICATIONOBJ_TABLES_IN_SCHEMA:
				schemaid = get_namespace_oid(pubobj->name, false);

				/* 若用户指定 "sch1, sch1" 则过滤掉重复项   */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			case PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA:
				search_path = fetch_search_path(false);
				if (search_path == NIL) /* nothing valid in search_path? */
					ereport(ERROR,
							errcode(ERRCODE_UNDEFINED_SCHEMA),
							errmsg("no schema has been selected for CURRENT_SCHEMA"));

				schemaid = linitial_oid(search_path);
				list_free(search_path);

				/* 若用户指定 "sch1, sch1" 则过滤掉重复项   */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			default:
				/* shouldn't happen */
				elog(ERROR, "invalid publication object type %d", pubobj->pubobjtype);
				break;
		}
	}
}

/*
  * 若行过滤器 WHERE 表达式中使用的任何列不属于
  * REPLICA IDENTITY 则返回 true，否则 false。
 */
static bool
contain_invalid_rfcolumn_walker(Node *node, rf_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		AttrNumber	attnum = var->varattno;

		/*
		  * 若 pubviaroot 为 true，在验证父表的行过滤器，
		  * 但位图含子表的复制标识信息。
		  * 需获取子表列号，
		  * 因父表和子表列顺序可能不同。
		 */
		if (context->pubviaroot)
		{
			char	   *colname = get_attname(context->parentid, attnum, false);

			attnum = get_attnum(context->relid, colname);
		}

		if (!bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber,
						   context->bms_replident))
			return true;
	}

	return expression_tree_walker(node, contain_invalid_rfcolumn_walker,
								  (void *) context);
}

/*
  * 检查过滤器表达式引用的所有列是否在
  * REPLICA IDENTITY 索引中。
  *
  * 若发现无效列则返回 true。
 */
bool
pub_rf_contains_invalid_column(Oid pubid, Relation relation, List *ancestors,
							   bool pubviaroot)
{
	HeapTuple	rftuple;
	Oid			relid = RelationGetRelid(relation);
	Oid			publish_as_relid = RelationGetRelid(relation);
	bool		result = false;
	Datum		rfdatum;
	bool		rfisnull;

	/*
	  * FULL 表示所有列均在 REPLICA IDENTITY 中，因此所有列
	  * 在行过滤器中均允许，可跳过验证。
	 */
	if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return false;

	/*
	  * 对于分区，若 pubviaroot 为 true，找到通过此发布
	  * 发布的最顶层祖先，因需用其行过滤器表达式
	  * 过滤分区更改。
	  *
	  * 注意即使用祖先的行过滤器，
	  * REPLICA IDENTITY 使用的是实际子表的。
	 */
	if (pubviaroot && relation->rd_rel->relispartition)
	{
		publish_as_relid
			= GetTopMostAncestorInPublication(pubid, ancestors, NULL);

		if (!OidIsValid(publish_as_relid))
			publish_as_relid = relid;
	}

	rftuple = SearchSysCache2(PUBLICATIONRELMAP,
							  ObjectIdGetDatum(publish_as_relid),
							  ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(rftuple))
		return false;

	rfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
							  Anum_pg_publication_rel_prqual,
							  &rfisnull);

	if (!rfisnull)
	{
		rf_context	context = {0};
		Node	   *rfnode;
		Bitmapset  *bms = NULL;

		context.pubviaroot = pubviaroot;
		context.parentid = publish_as_relid;
		context.relid = relid;

		/* 记住属于 REPLICA IDENTITY 的列                   */
		bms = RelationGetIndexAttrBitmap(relation,
										 INDEX_ATTR_BITMAP_IDENTITY_KEY);

		context.bms_replident = bms;
		rfnode = stringToNode(TextDatumGetCString(rfdatum));
		result = contain_invalid_rfcolumn_walker(rfnode, &context);
	}

	ReleaseSysCache(rftuple);

	return result;
}

/*
  * 检查 REPLICA IDENTITY 中引用的所有列是否被
  * 列列表覆盖。
  *
  * 若有复制标识列未被列列表覆盖则返回 true。
 */
bool
pub_collist_contains_invalid_column(Oid pubid, Relation relation, List *ancestors,
									bool pubviaroot)
{
	HeapTuple	tuple;
	Oid			relid = RelationGetRelid(relation);
	Oid			publish_as_relid = RelationGetRelid(relation);
	bool		result = false;
	Datum		datum;
	bool		isnull;

	/*
	 * 对于分区，若 pubviaroot 为 true，则找到通过此发布发  
	 * 布的最顶层祖先，因为我们需要使用它的列列表来处  
	 * 理更改。
	 *
	 * 注意，即使所使用的列列表是祖先的，所使用的   
	 *  REPLICA IDENTITY 却是实际子表的。               
	 */
	if (pubviaroot && relation->rd_rel->relispartition)
	{
		publish_as_relid = GetTopMostAncestorInPublication(pubid, ancestors, NULL);

		if (!OidIsValid(publish_as_relid))
			publish_as_relid = relid;
	}

	tuple = SearchSysCache2(PUBLICATIONRELMAP,
							ObjectIdGetDatum(publish_as_relid),
							ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tuple))
		return false;

	datum = SysCacheGetAttr(PUBLICATIONRELMAP, tuple,
							Anum_pg_publication_rel_prattrs,
							&isnull);

	if (!isnull)
	{
		int			x;
		Bitmapset  *idattrs;
		Bitmapset  *columns = NULL;

		/* REPLICA IDENTITY FULL 下不允许列列表。         */
		if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			result = true;

		/* 把列列表 datum 转换为位图集。        */
		columns = pub_collist_to_bitmapset(NULL, datum, NULL);

		/* 记住属于 REPLICA IDENTITY 的列                   */
		idattrs = RelationGetIndexAttrBitmap(relation,
											 INDEX_ATTR_BITMAP_IDENTITY_KEY);

		/*
		  * RelationGetIndexAttrBitmap 返回的位图 Attnum 是偏移的
		  * （以通常方式处理系统列），而列列表不使用偏移，
		  * 因此不能做 bms_is_subset()。
		  * 必须遍历 idattrs 检查所有是否在列表中。
		  *
		 */
		x = -1;
		while ((x = bms_next_member(idattrs, x)) >= 0)
		{
			AttrNumber	attnum = (x + FirstLowInvalidHeapAttributeNumber);

			/*
			  * 若 pubviaroot 为 true，在验证父表的列列表，
			  * 但位图含子表的复制标识信息。
			  * 父/子 attnum 可能不匹配，需转换到父表——
			  * 从子表获取 attname，在父表查找。
			  *
			 */
			if (pubviaroot)
			{
				/* 子表中的属性名             */
				char	   *colname = get_attname(relid, attnum, false);

				/*
				  * 确定属性名在父表中的 attnum（使用
				  * 父表上定义的列列表）。
				 */
				attnum = get_attnum(publish_as_relid, colname);
			}

			/* 复制标识列，未被列列表覆盖                 */
			if (!bms_is_member(attnum, columns))
			{
				result = true;
				break;
			}
		}

		bms_free(idattrs);
		bms_free(columns);
	}

	ReleaseSysCache(tuple);

	return result;
}

/* check_functions_in_node callback */
static bool
contain_mutable_or_user_functions_checker(Oid func_id, void *context)
{
	return (func_volatile(func_id) != PROVOLATILE_IMMUTABLE ||
			func_id >= FirstNormalObjectId);
}

/*
 * 行过滤器遍历器会检查行过滤器表达式是否为简单    
 * 表达式。
 *
 * 它只允许简单或复合表达式，例如：      
 * - (Var Op Const)
 * - (Var Op Var)
 * - (Var Op Const) AND/OR (Var Op Const)
 * - 等
 * （其中 Var 是该过滤器所属表的列）            
 *
 * 简单表达式有以下一些限制：              
 * - 不允许用户自定义操作符；   
 * - 不允许用户自定义函数；      
 * - 不允许用户自定义类型；  
 * - 不允许用户自定义排序规则； 
 * - 不允许非 IMMUTABLE 的内置函数；        
 * - 不允许系统列。          
 *
 * 注  
 *
 * 不允许用户自定义函数/操作符/类型/排序规则，因为   
 * (a) 用户删除了行过滤表达式中使用的用户自定义对象，   
 * 或在使用它时发生任何其他错误，逻辑解码       
 * 基础设施将无法从此类错误中恢复，即使                
 * 该对象被重新创建也一样，因为历史快照被用于求值    
 * 行过滤器；
 * (b) 用户自定义函数可被用于访问某些表，而这样可能  
 * 带来不良结果，因为使用了历史快照。这正是为何在  
 * 行过滤表达式中只允许不可变内置函数。             
 *
 * 我们不允许系统列，因为目前我们还没有             
 * 下游所传递元组中的该信息。另外，由于我们并不把     
 * 它们复制给订阅者，所以似乎没必要过滤这些               
 * 列。  
 *
 * 经更多分析与测试后，可允许其他节点类型。  
 */
static bool
check_simple_rowfilter_expr_walker(Node *node, ParseState *pstate)
{
	char	   *errdetail_msg = NULL;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			/* 不允许系统列。           */
			if (((Var *) node)->varattno < InvalidAttrNumber)
				errdetail_msg = _("System columns are not allowed.");
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			/* 可以，但用户自定义操作符不允许。   */
			if (((OpExpr *) node)->opno >= FirstNormalObjectId)
				errdetail_msg = _("User-defined operators are not allowed.");
			break;
		case T_ScalarArrayOpExpr:
			/* 可以，但用户自定义操作符不允许。   */
			if (((ScalarArrayOpExpr *) node)->opno >= FirstNormalObjectId)
				errdetail_msg = _("User-defined operators are not allowed.");

			/*
			  * 不需检查 ScalarArrayOpExpr 的 hashfuncid 和
			  * negfuncid，因这些函数仅为子查询构建。
			  *
			 */
			break;
		case T_RowCompareExpr:
			{
				ListCell   *opid;

				/* 可以，但用户自定义操作符不允许。   */
				foreach(opid, ((RowCompareExpr *) node)->opnos)
				{
					if (lfirst_oid(opid) >= FirstNormalObjectId)
					{
						errdetail_msg = _("User-defined operators are not allowed.");
						break;
					}
				}
			}
			break;
		case T_Const:
		case T_FuncExpr:
		case T_BoolExpr:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CaseTestExpr:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_XmlExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_List:
			/* OK, supported */
			break;
		default:
			errdetail_msg = _("Only columns, constants, built-in operators, built-in data types, built-in collations, and immutable built-in functions are allowed.");
			break;
	}

	/*
	  * 对所有支持的节点，若尚未发现问题，
	  * 检查其中使用的类型、函数和排序规则。
	  * 通过遍历每个元素检查 List。
	 */
	if (!errdetail_msg && !IsA(node, List))
	{
		if (exprType(node) >= FirstNormalObjectId)
			errdetail_msg = _("User-defined types are not allowed.");
		else if (check_functions_in_node(node, contain_mutable_or_user_functions_checker,
										 (void *) pstate))
			errdetail_msg = _("User-defined or built-in mutable functions are not allowed.");
		else if (exprCollation(node) >= FirstNormalObjectId ||
				 exprInputCollation(node) >= FirstNormalObjectId)
			errdetail_msg = _("User-defined collations are not allowed.");
	}

	/*
	  * 若此节点发现任何问题，立即报错。否则继续。
	  *
	 */
	if (errdetail_msg)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid publication WHERE expression"),
				 errdetail_internal("%s", errdetail_msg),
				 parser_errposition(pstate, exprLocation(node))));

	return expression_tree_walker(node, check_simple_rowfilter_expr_walker,
								  (void *) pstate);
}

/*
  * 检查行过滤器表达式是否为"简单表达式"。
  *
  * 详见 check_simple_rowfilter_expr_walker。
 */
static bool
check_simple_rowfilter_expr(Node *node, ParseState *pstate)
{
	return check_simple_rowfilter_expr_walker(node, pstate);
}

/*
  * 转换列表中所有关系的发布 WHERE 表达式，
  * 确保强制转为布尔，按需添加排序规则信息，
  * 并为关联关系在 ParseState 命名空间列表中添加
  * 新的 nsitem/RTE。
  *
  * 同时检查发布行过滤器表达式，遇到不允许或意料之外的
  * 内容则报错。
 */
static void
TransformPubWhereClauses(List *tables, const char *queryString,
						 bool pubviaroot)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		ParseNamespaceItem *nsitem;
		Node	   *whereclause = NULL;
		ParseState *pstate;
		PublicationRelInfo *pri = (PublicationRelInfo *) lfirst(lc);

		if (pri->whereClause == NULL)
			continue;

		/*
		  * 若发布不通过根分区表发布更改，
		  * 将用分区的行过滤器。故不允许
		  * 对分区表使用 WHERE 子句。
		 */
		if (!pubviaroot &&
			pri->relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use publication WHERE clause for relation \"%s\"",
							RelationGetRelationName(pri->relation)),
					 errdetail("WHERE clause cannot be used for a partitioned table when %s is false.",
							   "publish_via_partition_root")));

		/*
		  * 需新 pstate，使其范围表中仅有"此"表
		  *
		 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;
		nsitem = addRangeTableEntryForRelation(pstate, pri->relation,
											   AccessShareLock, NULL,
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		whereclause = transformWhereClause(pstate,
										   copyObject(pri->whereClause),
										   EXPR_KIND_WHERE,
										   "PUBLICATION WHERE");

		/* 修正排序规则信息     */
		assign_expr_collations(pstate, whereclause);

		/*
		  * 行过滤器中只允许简单表达式。参见
		  * check_simple_rowfilter_expr_walker。
		 */
		check_simple_rowfilter_expr(whereclause, pstate);

		free_parsestate(pstate);

		pri->whereClause = whereclause;
	}
}


/*
  * 给定要添加到发布的表列表，
  * 验证它们满足必要前置条件，即：若有模式被发布则表
  * 不能有列列表；若 publish_via_partition_root 未设置则
  * 分区表不能有列列表。
  *
  * 'publish_schema' 指示发布是否包含 TABLES IN SCHEMA 元素
  * （新增或已有）。'pubviaroot' 为 publish_via_partition_root
  * 的值。
 */
static void
CheckPubRelationColumnList(char *pubname, List *tables,
						   bool publish_schema, bool pubviaroot)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		PublicationRelInfo *pri = (PublicationRelInfo *) lfirst(lc);

		if (pri->columns == NIL)
			continue;

		/*
		 * 如果发布中包含任何模式，则不允许指定 
		 * 列列表。
		 *
		 * XXX 我们可以改为直接禁止这种情形：发布试图 
		 * 用列列表加上该表的模式来发布该表。但如果那    
		 * 样做，我们就需要在 ALTER TABLE ... SET SCHEMA         
		 * 期间加以限制，而这似乎不是个好主意。         
		 *                        
		 */
		if (publish_schema)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("cannot use column list for relation \"%s.%s\" in publication \"%s\"",
						   get_namespace_name(RelationGetNamespace(pri->relation)),
						   RelationGetRelationName(pri->relation), pubname),
					errdetail("Column lists cannot be specified in publications containing FOR TABLES IN SCHEMA elements."));

		/*
		  * 若发布不通过根分区表发布更改，
		  * 将用分区的列列表。故不允许
		  * 对分区表使用列列表。
		 */
		if (!pubviaroot &&
			pri->relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use column list for relation \"%s.%s\" in publication \"%s\"",
							get_namespace_name(RelationGetNamespace(pri->relation)),
							RelationGetRelationName(pri->relation), pubname),
					 errdetail("Column lists cannot be specified for partitioned tables when %s is false.",
							   "publish_via_partition_root")));
	}
}

/*
  * 创建新发布。
 */
ObjectAddress
CreatePublication(ParseState *pstate, CreatePublicationStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			puboid;
	bool		nulls[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	HeapTuple	tup;
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	AclResult	aclresult;
	List	   *relations = NIL;
	List	   *schemaidlist = NIL;

	/* 必须对数据库有 CREATE 权限 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	/* FOR ALL TABLES 需超级用户 */
	if (stmt->for_all_tables && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create FOR ALL TABLES publication")));

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	/* 检查名称是否已使用 */
	puboid = GetSysCacheOid1(PUBLICATIONNAME, Anum_pg_publication_oid,
							 CStringGetDatum(stmt->pubname));
	if (OidIsValid(puboid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("publication \"%s\" already exists",
						stmt->pubname)));

	/* 构造元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_publication_pubname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->pubname));
	values[Anum_pg_publication_pubowner - 1] = ObjectIdGetDatum(GetUserId());

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root);

	puboid = GetNewOidWithIndex(rel, PublicationObjectIndexId,
								Anum_pg_publication_oid);
	values[Anum_pg_publication_oid - 1] = ObjectIdGetDatum(puboid);
	values[Anum_pg_publication_puballtables - 1] =
		BoolGetDatum(stmt->for_all_tables);
	values[Anum_pg_publication_pubinsert - 1] =
		BoolGetDatum(pubactions.pubinsert);
	values[Anum_pg_publication_pubupdate - 1] =
		BoolGetDatum(pubactions.pubupdate);
	values[Anum_pg_publication_pubdelete - 1] =
		BoolGetDatum(pubactions.pubdelete);
	values[Anum_pg_publication_pubtruncate - 1] =
		BoolGetDatum(pubactions.pubtruncate);
	values[Anum_pg_publication_pubviaroot - 1] =
		BoolGetDatum(publish_via_partition_root);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* 将元组插入目录。 */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(PublicationRelationId, puboid, GetUserId());

	ObjectAddressSet(myself, PublicationRelationId, puboid);

	/* 使更改可见。 */
	CommandCounterIncrement();

	/* 将对象与发布关联。 */
	if (stmt->for_all_tables)
	{
		/* 使 relcache 失效，以便重建发布信息。        */
		CacheInvalidateRelcacheAll();
	}
	else
	{
		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		/* FOR TABLES IN SCHEMA requires superuser */
		if (schemaidlist != NIL && !superuser())
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("must be superuser to create FOR TABLES IN SCHEMA publication"));

		if (relations != NIL)
		{
			List	   *rels;

			rels = OpenTableList(relations);
			TransformPubWhereClauses(rels, pstate->p_sourcetext,
									 publish_via_partition_root);

			CheckPubRelationColumnList(stmt->pubname, rels,
									   schemaidlist != NIL,
									   publish_via_partition_root);

			PublicationAddTables(puboid, rels, true, NULL);
			CloseTableList(rels);
		}

		if (schemaidlist != NIL)
		{
			/*
			  * 模式锁保持至发布创建完，防止并发模式删除。
			  *
			 */
			LockSchemaList(schemaidlist);
			PublicationAddSchemas(puboid, schemaidlist, true, NULL);
		}
	}

	table_close(rel, RowExclusiveLock);

	InvokeObjectPostCreateHook(PublicationRelationId, puboid, 0);

	if (wal_level != WAL_LEVEL_LOGICAL)
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("wal_level is insufficient to publish logical changes"),
				 errhint("Set wal_level to \"logical\" before creating subscriptions.")));

	return myself;
}

/*
  * 更改发布选项。
 */
static void
AlterPublicationOptions(ParseState *pstate, AlterPublicationStmt *stmt,
						Relation rel, HeapTuple tup)
{
	bool		nulls[Natts_pg_publication];
	bool		replaces[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	ObjectAddress obj;
	Form_pg_publication pubform;
	List	   *root_relids = NIL;
	ListCell   *lc;

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/*
	  * 若发布不通过根分区表发布更改，
	  * 将用分区的行过滤器和列列表。故不允许
	  * 对分区表使用 WHERE 子句和列列表。
	  *
	 */
	if (!pubform->puballtables && publish_via_partition_root_given &&
		!publish_via_partition_root)
	{
		/*
		  * 锁定发布以防他人操作。防止并发 alter 添加
		  * 带 WHERE 子句和/或列列表的分区表，
		  * 不通过根发布时不允许。
		  *
		 */
		LockDatabaseObject(PublicationRelationId, pubform->oid, 0,
						   AccessShareLock);

		root_relids = GetPublicationRelations(pubform->oid,
											  PUBLICATION_PART_ROOT);

		foreach(lc, root_relids)
		{
			Oid			relid = lfirst_oid(lc);
			HeapTuple	rftuple;
			char		relkind;
			char	   *relname;
			bool		has_rowfilter;
			bool		has_collist;

			/*
			  * 注意：未对关系加锁，故对缓存查找返回 NULL
			  * 静默处理。
			 */

			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(relid),
									  ObjectIdGetDatum(pubform->oid));
			if (!HeapTupleIsValid(rftuple))
				continue;
			has_rowfilter = !heap_attisnull(rftuple, Anum_pg_publication_rel_prqual, NULL);
			has_collist = !heap_attisnull(rftuple, Anum_pg_publication_rel_prattrs, NULL);
			if (!has_rowfilter && !has_collist)
			{
				ReleaseSysCache(rftuple);
				continue;
			}

			relkind = get_rel_relkind(relid);
			if (relkind != RELKIND_PARTITIONED_TABLE)
			{
				ReleaseSysCache(rftuple);
				continue;
			}
			relname = get_rel_name(relid);
			if (relname == NULL)	/* table concurrently dropped */
			{
				ReleaseSysCache(rftuple);
				continue;
			}

			if (has_rowfilter)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cannot set parameter \"%s\" to false for publication \"%s\"",
								"publish_via_partition_root",
								stmt->pubname),
						 errdetail("The publication contains a WHERE clause for partitioned table \"%s\", which is not allowed when \"%s\" is false.",
								   relname, "publish_via_partition_root")));
			Assert(has_collist);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot set parameter \"%s\" to false for publication \"%s\"",
							"publish_via_partition_root",
							stmt->pubname),
					 errdetail("The publication contains a column list for partitioned table \"%s\", which is not allowed when \"%s\" is false.",
							   relname, "publish_via_partition_root")));
		}
	}

	/* 一切正常，构造新元组。 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (publish_given)
	{
		values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(pubactions.pubinsert);
		replaces[Anum_pg_publication_pubinsert - 1] = true;

		values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(pubactions.pubupdate);
		replaces[Anum_pg_publication_pubupdate - 1] = true;

		values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(pubactions.pubdelete);
		replaces[Anum_pg_publication_pubdelete - 1] = true;

		values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(pubactions.pubtruncate);
		replaces[Anum_pg_publication_pubtruncate - 1] = true;
	}

	if (publish_via_partition_root_given)
	{
		values[Anum_pg_publication_pubviaroot - 1] = BoolGetDatum(publish_via_partition_root);
		replaces[Anum_pg_publication_pubviaroot - 1] = true;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* 更新目录。 */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	CommandCounterIncrement();

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* 使 relcache 失效。 */
	if (pubform->puballtables)
	{
		CacheInvalidateRelcacheAll();
	}
	else
	{
		List	   *relids = NIL;
		List	   *schemarelids = NIL;

		/*
		  * 对于发布中包含的任何分区表，须失效相应分区树中的
		  * 所有分区，不仅发布中显式提及的。
		  *
		 */
		if (root_relids == NIL)
			relids = GetPublicationRelations(pubform->oid,
											 PUBLICATION_PART_ALL);
		else
		{
			/*
			  * 已获取发布中显式提及的表。
			  * 现在获取列表中分区表的所有分区。
			 */
			foreach(lc, root_relids)
				relids = GetPubPartitionOptionRelations(relids,
														PUBLICATION_PART_ALL,
														lfirst_oid(lc));
		}

		schemarelids = GetAllSchemaPublicationRelations(pubform->oid,
														PUBLICATION_PART_ALL);
		relids = list_concat_unique_oid(relids, schemarelids);

		InvalidatePublicationRels(relids);
	}

	ObjectAddressSet(obj, PublicationRelationId, pubform->oid);
	EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
									 (Node *) stmt);

	InvokeObjectPostAlterHook(PublicationRelationId, pubform->oid, 0);
}

/*
  * 使关系失效。
 */
void
InvalidatePublicationRels(List *relids)
{
	/*
	  * 不想发太多单独消息，某种程度
	  * 直接重置整个 relcache 更便宜。
	 */
	if (list_length(relids) < MAX_RELCACHE_INVAL_MSGS)
	{
		ListCell   *lc;

		foreach(lc, relids)
			CacheInvalidateRelcacheByRelid(lfirst_oid(lc));
	}
	else
		CacheInvalidateRelcacheAll();
}

/*
  * 向发布中增删表。
 */
static void
AlterPublicationTables(AlterPublicationStmt *stmt, HeapTuple tup,
					   List *tables, const char *queryString,
					   bool publish_schema)
{
	List	   *rels = NIL;
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);
	Oid			pubid = pubform->oid;

	/*
	  * 若无对象则无事可做，SET 除外：此时用户可能
	  * 未指定任何表，需删除所有现有表。
	  *
	 */
	if (!tables && stmt->action != AP_SetObjects)
		return;

	rels = OpenTableList(tables);

	if (stmt->action == AP_AddObjects)
	{
		TransformPubWhereClauses(rels, queryString, pubform->pubviaroot);

		publish_schema |= is_schema_publication(pubid);

		CheckPubRelationColumnList(stmt->pubname, rels, publish_schema,
								   pubform->pubviaroot);

		PublicationAddTables(pubid, rels, false, stmt);
	}
	else if (stmt->action == AP_DropObjects)
		PublicationDropTables(pubid, rels, false);
	else						/* AP_SetObjects */
	{
		List	   *oldrelids = GetPublicationRelations(pubid,
														PUBLICATION_PART_ROOT);
		List	   *delrels = NIL;
		ListCell   *oldlc;

		TransformPubWhereClauses(rels, queryString, pubform->pubviaroot);

		CheckPubRelationColumnList(stmt->pubname, rels, publish_schema,
								   pubform->pubviaroot);

		/*
		  * 重建发布的关系列表，查找不需删除的现有关系。
		  *
		 */
		foreach(oldlc, oldrelids)
		{
			Oid			oldrelid = lfirst_oid(oldlc);
			ListCell   *newlc;
			PublicationRelInfo *oldrel;
			bool		found = false;
			HeapTuple	rftuple;
			Node	   *oldrelwhereclause = NULL;
			Bitmapset  *oldcolumns = NULL;

			/* 在缓存中查找旧 relmap 条目  */
			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(oldrelid),
									  ObjectIdGetDatum(pubid));

			/*
			  * 查看现有关系当前是否有 WHERE 子句或
			  * 列列表。也需要比较这些。
			 */
			if (HeapTupleIsValid(rftuple))
			{
				bool		isnull = true;
				Datum		whereClauseDatum;
				Datum		columnListDatum;

				/* 加载此表的 WHERE 子句。       */
				whereClauseDatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
												   Anum_pg_publication_rel_prqual,
												   &isnull);
				if (!isnull)
					oldrelwhereclause = stringToNode(TextDatumGetCString(whereClauseDatum));

				/* 把 int2vector 列列表转换为位图。        */
				columnListDatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
												  Anum_pg_publication_rel_prattrs,
												  &isnull);

				if (!isnull)
					oldcolumns = pub_collist_to_bitmapset(NULL, columnListDatum, NULL);

				ReleaseSysCache(rftuple);
			}

			foreach(newlc, rels)
			{
				PublicationRelInfo *newpubrel;
				Oid			newrelid;
				Bitmapset  *newcolumns = NULL;

				newpubrel = (PublicationRelInfo *) lfirst(newlc);
				newrelid = RelationGetRelid(newpubrel->relation);

				/*
				  * 若新发布有列列表，同样转为位图。
				  *
				 */
				if (newpubrel->columns)
				{
					ListCell   *lc;

					foreach(lc, newpubrel->columns)
					{
						char	   *colname = strVal(lfirst(lc));
						AttrNumber	attnum = get_attnum(newrelid, colname);

						newcolumns = bms_add_member(newcolumns, attnum);
					}
				}

				/*
				 * 检查新关系集合中是否有与发布中现有      
				 * 关系匹配的。此外，如果该关系带有           
				 * 关联的 WHERE 子句，也要检查 WHERE 表达式匹
				 * 配。列列表也一样。其余的都删除             
				 * 。  
				 */
				if (RelationGetRelid(newpubrel->relation) == oldrelid)
				{
					if (equal(oldrelwhereclause, newpubrel->whereClause) &&
						bms_equal(oldcolumns, newcolumns))
					{
						found = true;
						break;
					}
				}
			}

			/*
			  * 将不匹配的关系加入列表以便删除。
			  *
			 */
			if (!found)
			{
				oldrel = palloc(sizeof(PublicationRelInfo));
				oldrel->whereClause = NULL;
				oldrel->columns = NIL;
				oldrel->relation = table_open(oldrelid,
											  ShareUpdateExclusiveLock);
				delrels = lappend(delrels, oldrel);
			}
		}

		/* And drop them. */
		PublicationDropTables(pubid, delrels, true);

		/*
		  * 不需计算差异来添加，目录更新时会捕获
		  * 并跳过已有项。
		 */
		PublicationAddTables(pubid, rels, true, stmt);

		CloseTableList(delrels);
	}

	CloseTableList(rels);
}

/*
  * 更改发布模式。
  *
  * 向发布中增删模式。
 */
static void
AlterPublicationSchemas(AlterPublicationStmt *stmt,
						HeapTuple tup, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	/*
	  * 若无对象则无事可做，SET 除外：此时用户可能
	  * 未指定模式，需删除所有现有模式。
	  *
	 */
	if (!schemaidlist && stmt->action != AP_SetObjects)
		return;

	/*
	  * 模式锁保持至发布更改完，防止并发模式删除。
	  *
	 */
	LockSchemaList(schemaidlist);
	if (stmt->action == AP_AddObjects)
	{
		ListCell   *lc;
		List	   *reloids;

		reloids = GetPublicationRelations(pubform->oid, PUBLICATION_PART_ROOT);

		foreach(lc, reloids)
		{
			HeapTuple	coltuple;

			coltuple = SearchSysCache2(PUBLICATIONRELMAP,
									   ObjectIdGetDatum(lfirst_oid(lc)),
									   ObjectIdGetDatum(pubform->oid));

			if (!HeapTupleIsValid(coltuple))
				continue;

			/*
			  * 若发布中已有列列表，禁止添加模式。
			  * 参见 CheckPubRelationColumnList。
			 */
			if (!heap_attisnull(coltuple, Anum_pg_publication_rel_prattrs, NULL))
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot add schema to publication \"%s\"",
							   stmt->pubname),
						errdetail("Schemas cannot be added if any tables that specify a column list are already part of the publication."));

			ReleaseSysCache(coltuple);
		}

		PublicationAddSchemas(pubform->oid, schemaidlist, false, stmt);
	}
	else if (stmt->action == AP_DropObjects)
		PublicationDropSchemas(pubform->oid, schemaidlist, false);
	else						/* AP_SetObjects */
	{
		List	   *oldschemaids = GetPublicationSchemas(pubform->oid);
		List	   *delschemas = NIL;

		/* 确定应删除哪些模式              */
		delschemas = list_difference_oid(oldschemaids, schemaidlist);

		/*
		  * 模式锁保持至发布更改完，防止并发模式删除。
		  *
		 */
		LockSchemaList(delschemas);

		/* And drop them */
		PublicationDropSchemas(pubform->oid, delschemas, true);

		/*
		  * 不需计算差异来添加，目录更新时会捕获
		  * 并跳过已有项。
		 */
		PublicationAddSchemas(pubform->oid, schemaidlist, true, stmt);
	}
}

/*
  * 检查关系和模式是否可放入指定发布，
  * 若不可则报错。
 */
static void
CheckAlterPublication(AlterPublicationStmt *stmt, HeapTuple tup,
					  List *tables, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	if ((stmt->action == AP_AddObjects || stmt->action == AP_SetObjects) &&
		schemaidlist && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to add or set schemas")));

	/*
	  * 检查用户是否有权操作发布中的模式内表
	  *
	 */
	if (schemaidlist && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Schemas cannot be added to or dropped from FOR ALL TABLES publications.")));

	/* 检查用户是否有权操作发布表。 */
	if (tables && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Tables cannot be added to or dropped from FOR ALL TABLES publications.")));
}

/*
  * 更改现有发布。
  *
  * 这是 AlterPublicationOptions、AlterPublicationSchemas
  * 和 AlterPublicationTables 的分发函数。
 */
void
AlterPublication(ParseState *pstate, AlterPublicationStmt *stmt)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME,
							  CStringGetDatum(stmt->pubname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist",
						stmt->pubname)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* 必须是所有者 */
	if (!object_ownercheck(PublicationRelationId, pubform->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
					   stmt->pubname);

	if (stmt->options)
		AlterPublicationOptions(pstate, stmt, rel, tup);
	else
	{
		List	   *relations = NIL;
		List	   *schemaidlist = NIL;
		Oid			pubid = pubform->oid;

		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		CheckAlterPublication(stmt, tup, relations, schemaidlist);

		heap_freetuple(tup);

		/* 锁定发布，使其他人无法对其做任何操作。    */
		LockDatabaseObject(PublicationRelationId, pubid, 0,
						   AccessExclusiveLock);

		/*
		  * 可能在我们获取发布锁时，并发 DDL 已将其删除。
		  * 可通过检查发布存在性来验证。重新获取元组
		  * 以避免发布选项被更改的风险。
		  *
		 */
		tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("publication \"%s\" does not exist",
						   stmt->pubname));

		AlterPublicationTables(stmt, tup, relations, pstate->p_sourcetext,
							   schemaidlist != NIL);
		AlterPublicationSchemas(stmt, tup, schemaidlist);
	}

	/* 清理。 */
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

/*
  * 按映射 OID 从发布中移除关系。
 */
void
RemovePublicationRelById(Oid proid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication_rel pubrel;
	List	   *relids = NIL;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONREL, ObjectIdGetDatum(proid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication table %u",
			 proid);

	pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

	/*
	  * 使 relcache 失效以重建发布信息。
	  *
	  * 对于分区表，须失效相应分区层次结构中的所有分区，
	  * 不仅发布中显式提及的。因为当父表发布时
	  * 隐式地发布了子表。
	  *
	 */
	relids = GetPubPartitionOptionRelations(relids, PUBLICATION_PART_ALL,
											pubrel->prrelid);

	InvalidatePublicationRels(relids);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
  * 按映射 OID 删除发布。
 */
void
RemovePublicationById(Oid pubid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* 使 relcache 失效以重建发布信息。 */
	if (pubform->puballtables)
		CacheInvalidateRelcacheAll();

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
  * 按映射 OID 从发布中移除模式。
 */
void
RemovePublicationSchemaById(Oid psoid)
{
	Relation	rel;
	HeapTuple	tup;
	List	   *schemaRels = NIL;
	Form_pg_publication_namespace pubsch;

	rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONNAMESPACE, ObjectIdGetDatum(psoid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication schema %u", psoid);

	pubsch = (Form_pg_publication_namespace) GETSTRUCT(tup);

	/*
	  * 使 relcache 失效以重建发布信息。关于为何需考虑
	  * 所有分区，见 RemovePublicationRelById。
	  *
	 */
	schemaRels = GetSchemaPublicationRelations(pubsch->pnnspid,
											   PUBLICATION_PART_ALL);
	InvalidatePublicationRels(schemaRels);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
  * 打开 PublicationTable 列表指定的关系。
  * 返回的表在 ShareUpdateExclusiveLock 模式下锁定以便
  * 添加到发布。
 */
static List *
OpenTableList(List *tables)
{
	List	   *relids = NIL;
	List	   *rels = NIL;
	ListCell   *lc;
	List	   *relids_with_rf = NIL;
	List	   *relids_with_collist = NIL;

	/*
	  * 打开、共享锁定并检查所有显式指定的关系
	 */
	foreach(lc, tables)
	{
		PublicationTable *t = lfirst_node(PublicationTable, lc);
		bool		recurse = t->relation->inh;
		Relation	rel;
		Oid			myrelid;
		PublicationRelInfo *pub_rel;

		/* 允许查询取消，以防此操作耗时过长  */
		CHECK_FOR_INTERRUPTS();

		rel = table_openrv(t->relation, ShareUpdateExclusiveLock);
		myrelid = RelationGetRelid(rel);

		/*
		  * 若用户指定重复项（如"foo, foo"）则过滤。
		  *
		  * 此算法已知效率不高（O(N^2)），
		  * 但仅处理用户给定的表列表，可接受。
		  *
		 */
		if (list_member_oid(relids, myrelid))
		{
			/* 若存在带行过滤器的重复表，则不允许。       */
			if (t->whereClause || list_member_oid(relids_with_rf, myrelid))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("conflicting or redundant WHERE clauses for table \"%s\"",
								RelationGetRelationName(rel))));

			/* 若存在带列列表的重复表，则不允许。           */
			if (t->columns || list_member_oid(relids_with_collist, myrelid))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("conflicting or redundant column lists for table \"%s\"",
								RelationGetRelationName(rel))));

			table_close(rel, ShareUpdateExclusiveLock);
			continue;
		}

		pub_rel = palloc(sizeof(PublicationRelInfo));
		pub_rel->relation = rel;
		pub_rel->whereClause = t->whereClause;
		pub_rel->columns = t->columns;
		rels = lappend(rels, pub_rel);
		relids = lappend_oid(relids, myrelid);

		if (t->whereClause)
			relids_with_rf = lappend_oid(relids_with_rf, myrelid);

		if (t->columns)
			relids_with_collist = lappend_oid(relids_with_collist, myrelid);

		/*
		  * 若要求，添加此关系的子项，令其也加入
		  * 发布。分区表除其分区外不能有继承子项，
		  * 且分区无需显式添加到发布。
		  *
		 */
		if (recurse && rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			List	   *children;
			ListCell   *child;

			children = find_all_inheritors(myrelid, ShareUpdateExclusiveLock,
										   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				/* 允许查询取消，以防此操作耗时过长  */
				CHECK_FOR_INTERRUPTS();

				/*
				  * 若用户同时指定了父表和子表，跳过重复。
				  *
				 */
				if (list_member_oid(relids, childrelid))
				{
					/*
					 * 不允许同时为父子表都指定行过滤器， 
					 * 因为不太清楚应优先采用哪一个。     
					 *                                            
					 */
					if (childrelid != myrelid &&
						(t->whereClause || list_member_oid(relids_with_rf, childrelid)))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("conflicting or redundant WHERE clauses for table \"%s\"",
										RelationGetRelationName(rel))));

					/*
					 * 不允许同时为父子表都指定列列表，     
					 * 因为不太清楚应优先采用哪一个。     
					 *                                            
					 */
					if (childrelid != myrelid &&
						(t->columns || list_member_oid(relids_with_collist, childrelid)))
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("conflicting or redundant column lists for table \"%s\"",
										RelationGetRelationName(rel))));

					continue;
				}

				/* find_all_inheritors already got lock */
				rel = table_open(childrelid, NoLock);
				pub_rel = palloc(sizeof(PublicationRelInfo));
				pub_rel->relation = rel;
				/* 子表继承父表的 WHERE 子句      */
				pub_rel->whereClause = t->whereClause;

				/* 子表继承父表的列列表         */
				pub_rel->columns = t->columns;
				rels = lappend(rels, pub_rel);
				relids = lappend_oid(relids, childrelid);

				if (t->whereClause)
					relids_with_rf = lappend_oid(relids_with_rf, childrelid);

				if (t->columns)
					relids_with_collist = lappend_oid(relids_with_collist, childrelid);
			}
		}
	}

	list_free(relids);
	list_free(relids_with_rf);

	return rels;
}

/*
  * 关闭列表中的所有关系。
 */
static void
CloseTableList(List *rels)
{
	ListCell   *lc;

	foreach(lc, rels)
	{
		PublicationRelInfo *pub_rel;

		pub_rel = (PublicationRelInfo *) lfirst(lc);
		table_close(pub_rel->relation, NoLock);
	}

	list_free_deep(rels);
}

/*
  * 以 AccessShareLock 模式锁定模式列表中指定的模式，
  * 防止并发模式删除。
 */
static void
LockSchemaList(List *schemalist)
{
	ListCell   *lc;

	foreach(lc, schemalist)
	{
		Oid			schemaid = lfirst_oid(lc);

		/* 允许查询取消，以防此操作耗时过长  */
		CHECK_FOR_INTERRUPTS();
		LockDatabaseObject(NamespaceRelationId, schemaid, 0, AccessShareLock);

		/*
		  * 可能获取模式锁时，并发 DDL 已将其删除。
		  * 通过检查模式存在性验证。
		  *
		 */
		if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaid)))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_SCHEMA),
					errmsg("schema with OID %u does not exist", schemaid));
	}
}

/*
  * 将列出的表添加到发布。
 */
static void
PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
					 AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, rels)
	{
		PublicationRelInfo *pub_rel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pub_rel->relation;
		ObjectAddress obj;

		/* 必须是表所有者或超级用户。  */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		obj = publication_add_relation(pubid, pub_rel, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationRelRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
  * 从发布中删除列出的表。
 */
static void
PublicationDropTables(Oid pubid, List *rels, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			prid;

	foreach(lc, rels)
	{
		PublicationRelInfo *pubrel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pubrel->relation;
		Oid			relid = RelationGetRelid(rel);

		if (pubrel->columns)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("column list must not be specified in ALTER PUBLICATION ... DROP"));

		prid = GetSysCacheOid2(PUBLICATIONRELMAP, Anum_pg_publication_rel_oid,
							   ObjectIdGetDatum(relid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(prid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("relation \"%s\" is not part of the publication",
							RelationGetRelationName(rel))));
		}

		if (pubrel->whereClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot use a WHERE clause when removing a table from a publication")));

		ObjectAddressSet(obj, PublicationRelRelationId, prid);
		performDeletion(&obj, DROP_CASCADE, 0);
	}
}

/*
  * 将列出的模式添加到发布。
 */
static void
PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
					  AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);
		ObjectAddress obj;

		obj = publication_add_schema(pubid, schemaid, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationNamespaceRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
  * 从发布中删除列出的模式。
 */
static void
PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			psid;

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);

		psid = GetSysCacheOid2(PUBLICATIONNAMESPACEMAP,
							   Anum_pg_publication_namespace_oid,
							   ObjectIdGetDatum(schemaid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(psid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tables from schema \"%s\" are not part of the publication",
							get_namespace_name(schemaid))));
		}

		ObjectAddressSet(obj, PublicationNamespaceRelationId, psid);
		performDeletion(&obj, DROP_CASCADE, 0);
	}
}

/*
  * 更改发布所有者的内部核心函数
 */
static void
AlterPublicationOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_publication form;

	form = (Form_pg_publication) GETSTRUCT(tup);

	if (form->pubowner == newOwnerId)
		return;

	if (!superuser())
	{
		AclResult	aclresult;

		/* Must be owner */
		if (!object_ownercheck(PublicationRelationId, form->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
						   NameStr(form->pubname));

		/* 必须能够成为新所有者   */
		check_can_set_role(GetUserId(), newOwnerId);

		/* 新所有者须有数据库的 CREATE 权限     */
		aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, newOwnerId, ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));

		if (form->puballtables && !superuser_arg(newOwnerId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR ALL TABLES publication must be a superuser.")));

		if (!superuser_arg(newOwnerId) && is_schema_publication(form->oid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR TABLES IN SCHEMA publication must be a superuser.")));
	}

	form->pubowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* 更新所有者依赖引用 */
	changeDependencyOnOwner(PublicationRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(PublicationRelationId,
							  form->oid, 0);
}

/*
  * 按名称更改发布所有者
 */
ObjectAddress
AlterPublicationOwner(const char *name, Oid newOwnerId)
{
	Oid			subid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", name)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	subid = pubform->oid;

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, PublicationRelationId, subid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
  * 按 OID 更改发布所有者
 */
void
AlterPublicationOwner_oid(Oid subid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication with OID %u does not exist", subid)));

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}
