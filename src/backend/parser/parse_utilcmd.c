/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.c
 *	  为各种实用命令执行解析分析工作           
 *
 * 此前这项工作由 analyze.c 的 parse_analyze_*() 完成，          
 * 但在有查询树缓存时这样做相当不安全，因为         
 * 依赖的数据库状态到实用命令执行时可能已过时；     
 * 而实用命令没有用于持锁或重新检查计划有效性的        
 * 基础设施。                                                      
 * 因此这些函数现在在各实用命令开始执行时调用。     
 *                             
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* transformCreateStmt 及其子程序共享的状态      */
typedef struct
{
	ParseState *pstate;			/* 整体解析器状态*/
	const char *stmtType;		/* CREATE [FOREIGN] TABLE 或 ALTER TABLE    */
	RangeVar   *relation;		/* 要创建的关系 */
	Relation	rel;			/* ALTER 时锁定的关系    */
	List	   *inhRelations;	/* 要继承的关系        */
	bool		isforeign;		/* 若为 CREATE/ALTER FOREIGN TABLE  */
	bool		isalter;		/* 若在修改既有表则为真  */
	List	   *columns;		/* ColumnDef 项   */
	List	   *ckconstraints;	/* CHECK 约束      */
	List	   *fkconstraints;	/* FOREIGN KEY 约束      */
	List	   *ixconstraints;	/* 会创建索引的约束   */
	List	   *likeclauses;	/* 需要后处理的 LIKE 子句         */
	List	   *blist;			/* "before list"，即建表前要做  
								 * 的事             */
	List	   *alist;			/* "after list"，即建表后要做          
								 * 的事    */
	IndexStmt  *pkey;			/* PRIMARY KEY 索引(若有)*/
	bool		ispartitioned;	/* 若表为分区表则为真  */
	PartitionBoundSpec *partbound;	/* 已转换的 FOR VALUES*/
	bool		ofType;			/* 若语句含 OF typename 则为真     */
} CreateStmtContext;

/* transformCreateSchemaStmtElements 及其子程序共享的状态      */
typedef struct
{
	const char *schemaname;		/* 模式名      */
	List	   *sequences;		/* CREATE SEQUENCE 项   */
	List	   *tables;			/* CREATE TABLE 项   */
	List	   *views;			/* CREATE VIEW 项   */
	List	   *indexes;		/* CREATE INDEX 项   */
	List	   *triggers;		/* CREATE TRIGGER 项   */
	List	   *grants;			/* GRANT 项   */
} CreateSchemaStmtContext;


static void transformColumnDefinition(CreateStmtContext *cxt,
									  ColumnDef *column);
static void transformTableConstraint(CreateStmtContext *cxt,
									 Constraint *constraint);
static void transformTableLikeClause(CreateStmtContext *cxt,
									 TableLikeClause *table_like_clause);
static void transformOfType(CreateStmtContext *cxt,
							TypeName *ofTypename);
static CreateStatsStmt *generateClonedExtStatsStmt(RangeVar *heapRel,
												   Oid heapRelid,
												   Oid source_statsid,
												   const AttrMap *attmap);
static List *get_collation(Oid collation, Oid actual_datatype);
static List *get_opclass(Oid opclass, Oid actual_datatype);
static void transformIndexConstraints(CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
										   CreateStmtContext *cxt);
static void transformFKConstraints(CreateStmtContext *cxt,
								   bool skipValidation,
								   bool isAddConstraint);
static void transformCheckConstraints(CreateStmtContext *cxt,
									  bool skipValidation);
static void transformConstraintAttrs(CreateStmtContext *cxt,
									 List *constraintList);
static void transformColumnType(CreateStmtContext *cxt, ColumnDef *column);
static void setSchemaName(const char *context_schema, char **stmt_schema_name);
static void transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd);
static List *transformPartitionRangeBounds(ParseState *pstate, List *blist,
										   Relation parent);
static void validateInfiniteBounds(ParseState *pstate, List *blist);
static Const *transformPartitionBoundValue(ParseState *pstate, Node *val,
										   const char *colName, Oid colType, int32 colTypmod,
										   Oid partCollation);


/*
 * transformCreateStmt -
 *	  CREATE TABLE 的解析分析   
 *
 * 返回一个需按顺序执行的实用命令列表。其中之一      
 * 是转换后的 CreateStmt，但也可能有额外动作               
 * 需要在实际的 DefineRelation() 调用前后完成。    
 * 除 AlterTableStmt、IndexStmt 等常规实用命令之外，    
 * 结果列表还可能包含 TableLikeClause，表示                   
 * 需要在 DefineRelation() 之后执行额外解析分析            
 *
 * SQL 允许约束散布在各处，因此需要遍历             
 * 各列并把所有约束收集到一处。             
 * 如果存在隐式索引（如 UNIQUE 或 PRIMARY KEY），  
 * 则将其展开成多个 IndexStmt 块。        
 *	  - thomas 1997-12-02
 */
List *
transformCreateStmt(CreateStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *elements;
	Oid			namespaceid;
	Oid			existing_relid;
	ParseCallbackState pcbstate;

	/* 设置 pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * 查找创建命名空间。这同时检查目标命名空间上的  
	 * 权限，锁定它防止并发删除，检查是否已有同名  
	 * 的既有关系，若选定的是临时命名空间还会更新       
	 * stmt->relation->relpersistence。                                     
	 */
	setup_parser_errposition_callback(&pcbstate, pstate,
									  stmt->relation->location);
	namespaceid =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock,
											 &existing_relid);
	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * 如果关系已存在且用户指定了 "IF NOT EXISTS"，            
	 * 以 NOTICE 退出。   
	 */
	if (stmt->if_not_exists && OidIsValid(existing_relid))
	{
		/*
		 * 如果处于扩展脚本中，坚持要求既存对象必须  
		 * 属于该扩展，以规避安全风险。                   
		 */
		ObjectAddress address;

		ObjectAddressSet(address, RelationRelationId, existing_relid);
		checkMembershipInCurrentExtension(&address);

		/* 可跳过  */
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						stmt->relation->relname)));
		return NIL;
	}

	/*
	 * 如果目标关系名未带模式限定，则补上。这防止      
	 * 一些边角情况：附加进来的重写命令可能误以为      
	 * 自己应该应用于同名且在搜索路径中更靠前的          
	 * 其他关系。但本地临时表实际已指定属于 pg_temp，     
	 * 因此这种情况无需任何额外处理。                          
	 */
	if (stmt->relation->schemaname == NULL
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		stmt->relation->schemaname = get_namespace_name(namespaceid);

	/* 设置 CreateStmtContext */
	cxt.pstate = pstate;
	if (IsA(stmt, CreateForeignTableStmt))
	{
		cxt.stmtType = "CREATE FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "CREATE TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = NULL;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = stmt->partspec != NULL;
	cxt.partbound = stmt->partbound;
	cxt.ofType = (stmt->ofTypename != NULL);

	Assert(!stmt->ofTypename || !stmt->inhRelations);	/* 由语法强制  */

	if (stmt->ofTypename)
		transformOfType(&cxt, stmt->ofTypename);

	if (stmt->partspec)
	{
		if (stmt->inhRelations && !stmt->partbound)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot create partitioned table as inheritance child")));
	}

	/*
	 * 遍历建表子句中的每个主要元素。把列定义与约束     
	 * 分开，并做初步分析。                            
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(&cxt, (ColumnDef *) element);
				break;

			case T_Constraint:
				transformTableConstraint(&cxt, (Constraint *) element);
				break;

			case T_TableLikeClause:
				transformTableLikeClause(&cxt, (TableLikeClause *) element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * 把 cxt.alist 中已有的内容转移到 save_alist，使其          
	 * 与 transformIndexConstraints 的输出分开。（这              
	 * 可能已无必要，但为了保持 alist 命令的历史执行 
	 * 顺序，仍会继续这样做。）                 
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * 后处理会引出索引定义的约束。                  
	 */
	transformIndexConstraints(&cxt);

	/*
	 * LIKE 子句的重新考虑应发生在索引创建之后、     
	 * 外键创建之前。这一顺序至关重要，因为 LIKE          
	 * 子句可能试图创建主键。如果主 CREATE TABLE 列表中     
	 * 还有 pkey，那么其创建在运行时不会检查重复（因为
	 * index_check_primary_key() 期望我们已在此拒绝重复      
	 * ）。LIKE 生成的 pkey 创建行为如同 ALTER TABLE ADD，     
	 * 所以它会检查，但显然仅在其后进行才有效。           
	 * 另一方面，我们希望先于外键约束创建主键，以防  
	 * 用户试图创建自引用外键。                                  
	 *                     
	 */
	cxt.alist = list_concat(cxt.alist, cxt.likeclauses);

	/*
	 * 后处理外键约束。            
	 */
	transformFKConstraints(&cxt, true, false);

	/*
	 * 后处理 CHECK 约束。     
	 *
	 * 对普通表而言，所有约束都可立即标记为有效，    
	 * 因为新表必然为空。外部表则不然。                    
	 */
	transformCheckConstraints(&cxt, !cxt.isforeign);

	/*
	 * 输出结果。
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

/*
 * generateSerialExtraStmts
 *		生成 CREATE SEQUENCE 和 ALTER SEQUENCE ... OWNED BY 语句      
 *		用于为 serial 或标识列创建其序列。        
 *
 * 这包括确定序列将使用的名称。调用者可以通过传入 
 * 非空指针来取回名称组件。                                
 *  snamespace_p 与 sname_p。 
 */
static void
generateSerialExtraStmts(CreateStmtContext *cxt, ColumnDef *column,
						 Oid seqtypid, List *seqoptions,
						 bool for_identity, bool col_exists,
						 char **snamespace_p, char **sname_p)
{
	ListCell   *option;
	DefElem    *nameEl = NULL;
	DefElem    *loggedEl = NULL;
	Oid			snamespaceid;
	char	   *snamespace;
	char	   *sname;
	char		seqpersistence;
	CreateSeqStmt *seqstmt;
	AlterSeqStmt *altseqstmt;
	List	   *attnamelist;

	/* 复制一份，因为下面的代码可能会修改它              */
	seqoptions = list_copy(seqoptions);

	/*
	 * 检查非 SQL 标准选项（CREATE SEQUENCE 不支持，      
	 * 因为它们是冗余的），并从 seqoptions 列表中移除 
	 *                          
	 */
	foreach(option, seqoptions)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "sequence_name") == 0)
		{
			if (nameEl)
				errorConflictingDefElem(defel, cxt->pstate);
			nameEl = defel;
			seqoptions = foreach_delete_current(seqoptions, option);
		}
		else if (strcmp(defel->defname, "logged") == 0 ||
				 strcmp(defel->defname, "unlogged") == 0)
		{
			if (loggedEl)
				errorConflictingDefElem(defel, cxt->pstate);
			loggedEl = defel;
			seqoptions = foreach_delete_current(seqoptions, option);
		}
	}

	/*
	 * 确定序列要使用的命名空间和名称。     
	 */
	if (nameEl)
	{
		/* 使用指定名称 */
		RangeVar   *rv = makeRangeVarFromNameList(castNode(List, nameEl->arg));

		snamespace = rv->schemaname;
		if (!snamespace)
		{
			/* 若 SEQUENCE NAME 未限定，选择命名空间  */
			if (cxt->rel)
				snamespaceid = RelationGetNamespace(cxt->rel);
			else
				snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			snamespace = get_namespace_name(snamespaceid);
		}
		sname = rv->relname;
	}
	else
	{
		/*
		 * 生成名称。 
		 *
		 * 虽然我们使用了 ChooseRelationName，但不保证所选   
		 * 的序列名不会冲突；在字段名足够长时，        
		 * 同一表中两个不同 serial 列可能得到相同的序列   
		 * 名，且因尚未创建序列而不会察觉。实践中这    
		 * 看起来很难成为问题，尤其因为很少有人在      
		 * 一个表中需要两个 serial 列。                           
		 *                                      
		 */
		if (cxt->rel)
			snamespaceid = RelationGetNamespace(cxt->rel);
		else
		{
			snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			RangeVarAdjustRelationPersistence(cxt->relation, snamespaceid);
		}
		snamespace = get_namespace_name(snamespaceid);
		sname = ChooseRelationName(cxt->relation->relname,
								   column->colname,
								   "seq",
								   snamespaceid,
								   false);
	}

	ereport(DEBUG1,
			(errmsg_internal("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
							 cxt->stmtType, sname,
							 cxt->relation->relname, column->colname)));

	/*
	 * 确定序列的持久性。默认复制该表的持久性，若   
	 * 指定了 LOGGED 或 UNLOGGED 则使用它（前提是表             
	 * 不是 TEMP）。                      
	 *
	 * 对 CREATE TABLE，我们从 cxt->relation 取得持久性，     
	 * 它来自正在处理的 CreateStmt。对 ALTER TABLE，解析器  
	 * 不会设置 cxt->relation->relpersistence，但有 cxt->rel        
	 * 作为既存表，所以从那里复制持久性。   
	 */
	seqpersistence = cxt->rel ? cxt->rel->rd_rel->relpersistence : cxt->relation->relpersistence;
	if (loggedEl)
	{
		if (seqpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot set logged status of a temporary sequence"),
					 parser_errposition(cxt->pstate, loggedEl->location)));
		else if (strcmp(loggedEl->defname, "logged") == 0)
			seqpersistence = RELPERSISTENCE_PERMANENT;
		else
			seqpersistence = RELPERSISTENCE_UNLOGGED;
	}

	/*
	 * 构建一条 CREATE SEQUENCE 命令来创建该序列对象，        
	 * 并将其加入此 CREATE/ALTER TABLE 之前待办事项列表。  
	 */
	seqstmt = makeNode(CreateSeqStmt);
	seqstmt->for_identity = for_identity;
	seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	seqstmt->sequence->relpersistence = seqpersistence;
	seqstmt->options = seqoptions;

	/*
	 * 如果指定了序列数据类型，则将其加入选项。前置    
	 * 到列表头部而非追加；这样若用户提供了自己的     
	 *  AS 子句，"redundant options" 错误将指向其出现位置，   
	 * 而非合成项。    
	 */
	if (seqtypid)
		seqstmt->options = lcons(makeDefElem("as",
											 (Node *) makeTypeNameFromOid(seqtypid, -1),
											 -1),
								 seqstmt->options);

	/*
	 * 如果这是 ALTER ADD COLUMN，则确保序列将由表            
	 * 的所有者。当前用户可能是别人（比如超级用户，   
	 * 或仅是拥有角色成员的人），但 SEQUENCE OWNED BY          
	 * 机制会抱怨，除非表和序列拥有完全相同的             
	 * 拥有角色。              
	 */
	if (cxt->rel)
		seqstmt->ownerId = cxt->rel->rd_rel->relowner;
	else
		seqstmt->ownerId = InvalidOid;

	cxt->blist = lappend(cxt->blist, seqstmt);

	/*
	 * 存储我们确定的标识序列名。ALTER TABLE ... ADD           
	 * COLUMN ... IDENTITY 需要它来用序列值填充新列，          
	 * 而序列与表的关联要到 ALTER TABLE 之后才建立。       
	 *                                                       
	 */
	column->identitySequence = seqstmt->sequence;

	/*
	 * 构建一条 ALTER SEQUENCE ... OWNED BY 命令，把序列         
	 * 标记为归此列所有，并将其加到与该 CREATE/ALTER        
	 *  TABLE 一起执行的待办事项列表。在 CREATE 或            
	 * ALTER ADD COLUMN 中必须在该语句之后执行，因为          
	 * 我们还不知道列的 attnum。但如果我们确实有 attnum      
	 * （AT_AddIdentity），则可立即标记，改善某些行为    
	 *           
	 */
	altseqstmt = makeNode(AlterSeqStmt);
	altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	attnamelist = list_make3(makeString(snamespace),
							 makeString(cxt->relation->relname),
							 makeString(column->colname));
	altseqstmt->options = list_make1(makeDefElem("owned_by",
												 (Node *) attnamelist, -1));
	altseqstmt->for_identity = for_identity;

	if (col_exists)
		cxt->blist = lappend(cxt->blist, altseqstmt);
	else
		cxt->alist = lappend(cxt->alist, altseqstmt);

	if (snamespace_p)
		*snamespace_p = snamespace;
	if (sname_p)
		*sname_p = sname;
}

/*
 * transformColumnDefinition -
 *		转换 CREATE TABLE 中的单个 ColumnDef      
 *		也用于 ALTER TABLE ADD COLUMN   
 */
static void
transformColumnDefinition(CreateStmtContext *cxt, ColumnDef *column)
{
	bool		is_serial;
	bool		saw_nullable;
	bool		saw_default;
	bool		saw_identity;
	bool		saw_generated;
	ListCell   *clist;

	cxt->columns = lappend(cxt->columns, column);

	/* 检查 SERIAL 伪类型       */
	is_serial = false;
	if (column->typeName
		&& list_length(column->typeName->names) == 1
		&& !column->typeName->pct_type)
	{
		char	   *typname = strVal(linitial(column->typeName->names));

		if (strcmp(typname, "smallserial") == 0 ||
			strcmp(typname, "serial2") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT2OID;
		}
		else if (strcmp(typname, "serial") == 0 ||
				 strcmp(typname, "serial4") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT4OID;
		}
		else if (strcmp(typname, "bigserial") == 0 ||
				 strcmp(typname, "serial8") == 0)
		{
			is_serial = true;
			column->typeName->names = NIL;
			column->typeName->typeOid = INT8OID;
		}

		/*
		 * 我们必须显式拒绝 "serial[]"，因为一旦设置了    
		 * typeid，LookupTypeName 不会注意到 arrayBounds，             
		 * 但 serial(typmod) 无需特殊处理。 
		 */
		if (is_serial && column->typeName->arrayBounds != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array of serial is not implemented"),
					 parser_errposition(cxt->pstate,
										column->typeName->location)));
	}

	/* 对列类型声明做必要的工作             */
	if (column->typeName)
		transformColumnType(cxt, column);

	/* SERIAL 伪类型的特殊动作         */
	if (is_serial)
	{
		char	   *snamespace;
		char	   *sname;
		char	   *qstring;
		A_Const    *snamenode;
		TypeCast   *castnode;
		FuncCall   *funccallnode;
		Constraint *constraint;

		generateSerialExtraStmts(cxt, column,
								 column->typeName->typeOid, NIL,
								 false, false,
								 &snamespace, &sname);

		/*
		 * 为 SERIAL 创建合适的约束。需完整执行，          
		 * 而不是走捷径，这样我们就能检测到用户写的    
		 * 任何冲突约束（如不同的 DEFAULT）。       
		 *
		 * 创建一个表达式树来表示函数调用           
		 * nextval('sequencename')。我们无法把原始树化简为      
		 * 成品形式；待序列创建之后，但我们无需这样做。 
		 *    
		 */
		qstring = quote_qualified_identifier(snamespace, sname);
		snamenode = makeNode(A_Const);
		snamenode->val.node.type = T_String;
		snamenode->val.sval.sval = qstring;
		snamenode->location = -1;
		castnode = makeNode(TypeCast);
		castnode->typeName = SystemTypeName("regclass");
		castnode->arg = (Node *) snamenode;
		castnode->location = -1;
		funccallnode = makeFuncCall(SystemFuncName("nextval"),
									list_make1(castnode),
									COERCE_EXPLICIT_CALL,
									-1);
		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_DEFAULT;
		constraint->location = -1;
		constraint->raw_expr = (Node *) funccallnode;
		constraint->cooked_expr = NULL;
		column->constraints = lappend(column->constraints, constraint);

		constraint = makeNode(Constraint);
		constraint->contype = CONSTR_NOTNULL;
		constraint->location = -1;
		column->constraints = lappend(column->constraints, constraint);
	}

	/* 处理列约束（若有）……     */
	transformConstraintAttrs(cxt, column->constraints);

	saw_nullable = false;
	saw_default = false;
	saw_identity = false;
	saw_generated = false;

	foreach(clist, column->constraints)
	{
		Constraint *constraint = lfirst_node(Constraint, clist);

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				if (saw_nullable && column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = false;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				if (saw_nullable && !column->is_not_null)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->is_not_null = true;
				saw_nullable = true;
				break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_IDENTITY:
				{
					Type		ctype;
					Oid			typeOid;

					if (cxt->ofType)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on typed tables")));
					if (cxt->partbound)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on partitions")));

					ctype = typenameType(cxt->pstate, column->typeName, NULL);
					typeOid = ((Form_pg_type) GETSTRUCT(ctype))->oid;
					ReleaseSysCache(ctype);

					if (saw_identity)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple identity specifications for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));

					generateSerialExtraStmts(cxt, column,
											 typeOid, constraint->options,
											 true, false,
											 NULL, NULL);

					column->identity = constraint->generated_when;
					saw_identity = true;

					/* 标识列隐含 NOT NULL                  */
					if (saw_nullable && !column->is_not_null)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));
					column->is_not_null = true;
					saw_nullable = true;
					break;
				}

			case CONSTR_GENERATED:
				if (cxt->ofType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on typed tables")));
				if (saw_generated)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple generation clauses specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->generated = ATTRIBUTE_GENERATED_STORED;
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_generated = true;
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("primary key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				/* FALL THRU */

			case CONSTR_UNIQUE:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unique constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				if (constraint->keys == NIL)
					constraint->keys = list_make1(makeString(column->colname));
				cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
				break;

			case CONSTR_EXCLUSION:
				/* 语法不允许 EXCLUDE 作为列约束               */
				elog(ERROR, "column exclusion constraints are not supported");
				break;

			case CONSTR_FOREIGN:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				/*
				 * 填入当前属性名，并将其放入待稍后处理的 
				 * 外键约束列表。                        
				 */
				constraint->fk_attrs = list_make1(makeString(column->colname));
				cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs 已处理这些    */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}

		if (saw_default && saw_identity)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and identity specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_default && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_identity && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both identity and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));
	}

	/*
	 * 如有需要，生成 ALTER FOREIGN TABLE ALTER COLUMN               
	 * 语句，在创建完成后为该列添加每列 FDW 选项。        
	 */
	if (column->fdwoptions != NIL)
	{
		AlterTableStmt *stmt;
		AlterTableCmd *cmd;

		cmd = makeNode(AlterTableCmd);
		cmd->subtype = AT_AlterColumnGenericOptions;
		cmd->name = column->colname;
		cmd->def = (Node *) column->fdwoptions;
		cmd->behavior = DROP_RESTRICT;
		cmd->missing_ok = false;

		stmt = makeNode(AlterTableStmt);
		stmt->relation = cxt->relation;
		stmt->cmds = NIL;
		stmt->objtype = OBJECT_FOREIGN_TABLE;
		stmt->cmds = lappend(stmt->cmds, cmd);

		cxt->alist = lappend(cxt->alist, stmt);
	}
}

/*
 * transformTableConstraint
 *		转换 CREATE TABLE 或 ALTER TABLE 内的 Constraint 节点  
 */
static void
transformTableConstraint(CreateStmtContext *cxt, Constraint *constraint)
{
	switch (constraint->contype)
	{
		case CONSTR_PRIMARY:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_UNIQUE:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_EXCLUSION:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			if (cxt->ispartitioned)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on partitioned tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_FOREIGN:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("foreign key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "invalid context for constraint type %d",
				 constraint->contype);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 constraint->contype);
			break;
	}
}

/*
 * transformTableLikeClause
 *
 * 把 CREATE TABLE 语句中的 LIKE <srctable> 部分变成         
 * 列定义，重现 <srctable> 中用户定义的列部分。        
 * 此外，如果有此刻无法完全处理的 LIKE 选项，        
 * 则把该 TableLikeClause 加入 cxt->likeclauses，由此               
 * 让 utility.c 在新表创建后调用 expandTableLikeClause()。 
 *                        
 */
static void
transformTableLikeClause(CreateStmtContext *cxt, TableLikeClause *table_like_clause)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	AclResult	aclresult;
	char	   *comment;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, cxt->pstate,
									  table_like_clause->relation->location);

	/* 很多情况下我们本可支持 LIKE，但改日再操心此事   */
	if (cxt->isforeign)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE is not supported for creating foreign tables")));

	/* 打开 LIKE 子句引用的关系               */
	relation = relation_openrv(table_like_clause->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_VIEW &&
		relation->rd_rel->relkind != RELKIND_MATVIEW &&
		relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is invalid in LIKE clause",
						RelationGetRelationName(relation)),
				 errdetail_relkind_not_supported(relation->rd_rel->relkind)));

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * 检查权限        
	 */
	if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		aclresult = object_aclcheck(TypeRelationId, relation->rd_rel->reltype, GetUserId(),
									ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TYPE,
						   RelationGetRelationName(relation));
	}
	else
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));
	}

	tupleDesc = RelationGetDescr(relation);

	/*
	 * 把复制的属性插入 cxt 以构成新表的定义。               
	 * 必须现在就插入，使其在表中出现的位置与           
	 * LIKE 子句所在位置一致，SQL99 有此要求。   
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);
		ColumnDef  *def;

		/*
		 * 忽略父表中被删除的列。    
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * 创建一个新列，标记为非继承。            
		 *
		 * 在约束方面，依据 SQL99 只有 NOT NULL 约束会被      
		 * 新列定义继承。           
		 */
		def = makeNode(ColumnDef);
		def->colname = pstrdup(attributeName);
		def->typeName = makeTypeNameFromOid(attribute->atttypid,
											attribute->atttypmod);
		def->inhcount = 0;
		def->is_local = true;
		def->is_not_null = attribute->attnotnull;
		def->is_from_type = false;
		def->storage = 0;
		def->raw_default = NULL;
		def->cooked_default = NULL;
		def->collClause = NULL;
		def->collOid = attribute->attcollation;
		def->constraints = NIL;
		def->location = -1;

		/*
		 * 加入列列表   
		 */
		cxt->columns = lappend(cxt->columns, def);

		/*
		 * 虽然不转移该列的默认值/生成表达式，但若
		 * 合适仍需将其标记为 GENERATED。                    
		 */
		if (attribute->atthasdef && attribute->attgenerated &&
			(table_like_clause->options & CREATE_TABLE_LIKE_GENERATED))
			def->generated = attribute->attgenerated;

		/*
		 * 按需复制标识属性  
		 */
		if (attribute->attidentity &&
			(table_like_clause->options & CREATE_TABLE_LIKE_IDENTITY))
		{
			Oid			seq_relid;
			List	   *seq_options;

			/*
			 * 找到旧列拥有的序列；提取序列参数；            
			 * 构建新的创建序列命令   
			 */
			seq_relid = getIdentitySequence(RelationGetRelid(relation), attribute->attnum, false);
			seq_options = sequence_options(seq_relid);
			generateSerialExtraStmts(cxt, def,
									 InvalidOid, seq_options,
									 true, false,
									 NULL, NULL);
			def->identity = attribute->attidentity;
		}

		/* 同样，按需复制存储         */
		if (table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
			def->storage = attribute->attstorage;
		else
			def->storage = 0;

		/* 同样，按需复制压缩             */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMPRESSION) != 0
			&& CompressionMethodIsValid(attribute->attcompression))
			def->compression =
				pstrdup(GetCompressionMethodName(attribute->attcompression));
		else
			def->compression = NULL;

		/* 同样，按需复制注释         */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
			(comment = GetComment(attribute->attrelid,
								  RelationRelationId,
								  attribute->attnum)) != NULL)
		{
			CommentStmt *stmt = makeNode(CommentStmt);

			stmt->objtype = OBJECT_COLUMN;
			stmt->object = (Node *) list_make3(makeString(cxt->relation->schemaname),
											   makeString(cxt->relation->relname),
											   makeString(def->colname));
			stmt->comment = comment;

			cxt->alist = lappend(cxt->alist, stmt);
		}
	}

	/*
	 * 我们还无法处理默认值、CHECK 约束、索引或统计 
	 * 信息，因为尚不知被复制列在最终表中的列号。   
	 * 如果指定了其中任何选项，则把该 LIKE 子句加入      
	 * cxt->likeclauses，以便在确切知道之后再调用   
	 * expandTableLikeClause。此外，记住关系 OID，以确保     
	 * expandTableLikeClause 一定会打开同一张表。                 
	 *                     
	 */
	if (table_like_clause->options &
		(CREATE_TABLE_LIKE_DEFAULTS |
		 CREATE_TABLE_LIKE_GENERATED |
		 CREATE_TABLE_LIKE_CONSTRAINTS |
		 CREATE_TABLE_LIKE_INDEXES |
		 CREATE_TABLE_LIKE_STATISTICS))
	{
		table_like_clause->relationOid = RelationGetRelid(relation);
		cxt->likeclauses = lappend(cxt->likeclauses, table_like_clause);
	}

	/*
	 * 关闭父关系，但在事务提交前保留其上的             
	 * AccessShareLock。这可防止父表在能运行                     
	 * expandTableLikeClause 前被删除或改动。 
	 */
	table_close(relation, NoLock);
}

/*
 * expandTableLikeClause
 *
 * 处理那些需要知道分配给新表各列的最终列号的   
 * LIKE 选项。它在新表运行 DefineRelation 之后执行。  
 * 它返回一组为生成索引等而需执行的实用命令。    
 *                                                     
 */
List *
expandTableLikeClause(RangeVar *heapRel, TableLikeClause *table_like_clause)
{
	List	   *result = NIL;
	List	   *atsubcmds = NIL;
	AttrNumber	parent_attno;
	Relation	relation;
	Relation	childrel;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrMap    *attmap;
	char	   *comment;

	/*
	 * 打开该 LIKE 子句引用的关系。此时应仍持有             
	 * transformTableLikeClause 获取的表锁（如果没有则会         
	 * 触发断言失败）。因此无需重新检查权限等。我们 
	 * 必须按 OID 而非名称打开关系，确保拿到同一张表     
	 *       
	 */
	if (!OidIsValid(table_like_clause->relationOid))
		elog(ERROR, "expandTableLikeClause called on untransformed LIKE clause");

	relation = relation_open(table_like_clause->relationOid, NoLock);

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	/*
	 * 打开新建的子关系；我们也已持有它的锁。       
	 */
	childrel = relation_openrv(heapRel, NoLock);

	/*
	 * 构造从 LIKE 关系属性号到子关系属性号的映射。    
	 * 它重新检查类型匹配等，虽然照理不应失败，       
	 * 因为两个表都已加锁。              
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(childrel),
								   tupleDesc,
								   false);

	/*
	 * 按需处理默认值。      
	 */
	if ((table_like_clause->options &
		 (CREATE_TABLE_LIKE_DEFAULTS | CREATE_TABLE_LIKE_GENERATED)) &&
		constr != NULL)
	{
		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
														parent_attno - 1);

			/*
			 * 忽略父关系中已删除的列。 
			 */
			if (attribute->attisdropped)
				continue;

			/*
			 * 默认值存在且应复制时才复制。我们为普通 
			 * 默认表达式与 GENERATED 默认值各有独立选项。 
			 *          
			 */
			if (attribute->atthasdef &&
				(attribute->attgenerated ?
				 (table_like_clause->options & CREATE_TABLE_LIKE_GENERATED) :
				 (table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS)))
			{
				Node	   *this_default = NULL;
				AttrDefault *attrdef = constr->defval;
				AlterTableCmd *atsubcmd;
				bool		found_whole_row;

				/* 在约束结构中查找默认值    */
				for (int i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						this_default = stringToNode(attrdef[i].adbin);
						break;
					}
				}
				if (this_default == NULL)
					elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
						 parent_attno, RelationGetRelationName(relation));

				atsubcmd = makeNode(AlterTableCmd);
				atsubcmd->subtype = AT_CookedColumnDefault;
				atsubcmd->num = attmap->attnums[parent_attno - 1];
				atsubcmd->def = map_variable_attnos(this_default,
													1, 0,
													attmap,
													InvalidOid,
													&found_whole_row);

				/*
				 * 出于与下文约束相同的理由予以禁止。       
				 * 注意默认值不含任何变量，故错误信息        
				 * 提及生成列是可以的。                
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
									   NameStr(attribute->attname),
									   RelationGetRelationName(relation))));

				atsubcmds = lappend(atsubcmds, atsubcmd);
			}
		}
	}

	/*
	 * 如果要求则复制 CHECK 约束，注意要调整属性号，      
	 * 使之与子表匹配。        
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) &&
		constr != NULL)
	{
		int			ccnum;

		for (ccnum = 0; ccnum < constr->num_check; ccnum++)
		{
			char	   *ccname = constr->check[ccnum].ccname;
			char	   *ccbin = constr->check[ccnum].ccbin;
			bool		ccnoinherit = constr->check[ccnum].ccnoinherit;
			Node	   *ccbin_node;
			bool		found_whole_row;
			Constraint *n;
			AlterTableCmd *atsubcmd;

			ccbin_node = map_variable_attnos(stringToNode(ccbin),
											 1, 0,
											 attmap,
											 InvalidOid, &found_whole_row);

			/*
			 * 我们拒绝整行变量，因为 LIKE 的意义就在于    
			 * 新表行类型日后可能与父表的不同。            
			 * 所以，虽然目前或许还能做转换，                
			 * 却无法保证它将来也能继续有效。                
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
								   ccname,
								   RelationGetRelationName(relation))));

			n = makeNode(Constraint);
			n->contype = CONSTR_CHECK;
			n->conname = pstrdup(ccname);
			n->location = -1;
			n->is_no_inherit = ccnoinherit;
			n->raw_expr = NULL;
			n->cooked_expr = nodeToString(ccbin_node);

			/* 新表应当为空，因此可以跳过验证。             */
			n->skip_validation = true;
			n->initially_valid = true;

			atsubcmd = makeNode(AlterTableCmd);
			atsubcmd->subtype = AT_AddConstraint;
			atsubcmd->def = (Node *) n;
			atsubcmds = lappend(atsubcmds, atsubcmd);

			/* 复制约束的注释      */
			if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
				(comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation),
																  n->conname, false),
									  ConstraintRelationId,
									  0)) != NULL)
			{
				CommentStmt *stmt = makeNode(CommentStmt);

				stmt->objtype = OBJECT_TABCONSTRAINT;
				stmt->object = (Node *) list_make3(makeString(heapRel->schemaname),
												   makeString(heapRel->relname),
												   makeString(n->conname));
				stmt->comment = comment;

				result = lappend(result, stmt);
			}
		}
	}

	/*
	 * 如果上文生成了 ALTER TABLE 动作，就把它们包成一条   
	 * ALTER TABLE 命令，放在结果最前面，让它先于上文      
	 * 生成的 CommentStmt 之前执行。 
	 */
	if (atsubcmds)
	{
		AlterTableStmt *atcmd = makeNode(AlterTableStmt);

		atcmd->relation = copyObject(heapRel);
		atcmd->cmds = atsubcmds;
		atcmd->objtype = OBJECT_TABLE;
		atcmd->missing_ok = false;
		result = lcons(atcmd, result);
	}

	/*
	 * 按需处理索引。       
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) &&
		relation->rd_rel->relhasindex)
	{
		List	   *parent_indexes;
		ListCell   *l;

		parent_indexes = RelationGetIndexList(relation);

		foreach(l, parent_indexes)
		{
			Oid			parent_index_oid = lfirst_oid(l);
			Relation	parent_index;
			IndexStmt  *index_stmt;

			parent_index = index_open(parent_index_oid, AccessShareLock);

			/* 构建 CREATE INDEX 语句以重建 parent_index          */
			index_stmt = generateClonedIndexStmt(heapRel,
												 parent_index,
												 attmap,
												 NULL);

			/* 如要求则复制索引的注释   */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_index_oid, RelationRelationId, 0);

				/*
				 * 我们利用 IndexStmt 的 idxcomment 选项，无需     
				 * 现在就知道索引将叫什么名字。     
				 */
				index_stmt->idxcomment = comment;
			}

			result = lappend(result, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

	/*
	 * 按需处理扩展统计。             
	 */
	if (table_like_clause->options & CREATE_TABLE_LIKE_STATISTICS)
	{
		List	   *parent_extstats;
		ListCell   *l;

		parent_extstats = RelationGetStatExtList(relation);

		foreach(l, parent_extstats)
		{
			Oid			parent_stat_oid = lfirst_oid(l);
			CreateStatsStmt *stats_stmt;

			stats_stmt = generateClonedExtStatsStmt(heapRel,
													RelationGetRelid(childrel),
													parent_stat_oid,
													attmap);

			/* 如要求则复制统计对象的注释         */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_stat_oid, StatisticExtRelationId, 0);

				/*
				 * 我们利用 CreateStatsStmt 的 stxcomment 选项，    
				 * 无需现在就知道统计对象将叫什么名字。     
				 */
				stats_stmt->stxcomment = comment;
			}

			result = lappend(result, stats_stmt);
		}

		list_free(parent_extstats);
	}

	/* 子关系用毕     */
	table_close(childrel, NoLock);

	/*
	 * 关闭父关系，但保持其上的 AccessShareLock 直到事务  
	 * 提交。这样可防止他人在子关系提交之前删除或      
	 * ALTER 父关系。                   
	 */
	table_close(relation, NoLock);

	return result;
}

static void
transformOfType(CreateStmtContext *cxt, TypeName *ofTypename)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			i;
	Oid			ofTypeId;

	Assert(ofTypename);

	tuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(tuple);
	ofTypeId = ((Form_pg_type) GETSTRUCT(tuple))->oid;
	ofTypename->typeOid = ofTypeId; /* 缓存供后用  */

	tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		ColumnDef  *n;

		if (attr->attisdropped)
			continue;

		n = makeNode(ColumnDef);
		n->colname = pstrdup(NameStr(attr->attname));
		n->typeName = makeTypeNameFromOid(attr->atttypid, attr->atttypmod);
		n->inhcount = 0;
		n->is_local = true;
		n->is_not_null = false;
		n->is_from_type = true;
		n->storage = 0;
		n->raw_default = NULL;
		n->cooked_default = NULL;
		n->collClause = NULL;
		n->collOid = attr->attcollation;
		n->constraints = NIL;
		n->location = -1;
		cxt->columns = lappend(cxt->columns, n);
	}
	ReleaseTupleDesc(tupdesc);

	ReleaseSysCache(tuple);
}

/*
 * 根据已存在索引 "source_idx" 的信息生成 IndexStmt 节点。     
 *              
 *
 * heapRel 会被存入 IndexStmt 的 relation 字段，但除此之外      
 * 并不使用它；不需要它有效时调用者可传 NULL。       
 * （目标关系可能尚不存在，所以绝不能尝试访问它。）  
 *
 * 表达式 Var 中的属性号会按 attmap 进行调整。            
 *
 * 若 constraintOid 不是 NULL，则把与该索引相关联的约束的   
 * OID 存到这里。  
 *
 * 与 transformIndexConstraint 不同，我们不打算强制把主键      
 * 列设为 NOT NULL。作为其组成部分的更大克隆流程应当  
 * 已经单独克隆过它们的 NOT NULL 状态（若未做到，         
 * DefineIndex 会报错）。       
 */
IndexStmt *
generateClonedIndexStmt(RangeVar *heapRel, Relation source_idx,
						const AttrMap *attmap,
						Oid *constraintOid)
{
	Oid			source_relid = RelationGetRelid(source_idx);
	HeapTuple	ht_idxrel;
	HeapTuple	ht_idx;
	HeapTuple	ht_am;
	Form_pg_class idxrelrec;
	Form_pg_index idxrec;
	Form_pg_am	amrec;
	oidvector  *indcollation;
	oidvector  *indclass;
	IndexStmt  *index;
	List	   *indexprs;
	ListCell   *indexpr_item;
	Oid			indrelid;
	int			keyno;
	Oid			keycoltype;
	Datum		datum;
	bool		isnull;

	if (constraintOid)
		*constraintOid = InvalidOid;

	/*
	 * 取源索引的 pg_class 元组。我们不能使用 relcache      
	 * 条目里的副本，因为它不含可选字段。       
	 */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(source_relid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", source_relid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* 从 relcache 条目取源索引的 pg_index 元组        */
	ht_idx = source_idx->rd_indextuple;
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);
	indrelid = idxrec->indrelid;

	/* 取该索引访问方法的 pg_am 元组          */
	ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/* 从 pg_index 元组提取 indcollation       */
	datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
								   Anum_pg_index_indcollation);
	indcollation = (oidvector *) DatumGetPointer(datum);

	/* 从 pg_index 元组提取 indclass       */
	datum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx, Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* 开始构建 IndexStmt       */
	index = makeNode(IndexStmt);
	index->relation = heapRel;
	index->accessMethod = pstrdup(NameStr(amrec->amname));
	if (OidIsValid(idxrelrec->reltablespace))
		index->tableSpace = get_tablespace_name(idxrelrec->reltablespace);
	else
		index->tableSpace = NULL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNumber = InvalidRelFileNumber;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
	index->unique = idxrec->indisunique;
	index->nulls_not_distinct = idxrec->indnullsnotdistinct;
	index->primary = idxrec->indisprimary;
	index->transformed = true;	/* 无需 transformIndexStmt     */
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = false;

	/*
	 * 我们不去保留源索引的名字；而是直接让              
	 * DefineIndex() 选一个合理的名字。（若试图保留          
	 * 名字，那么除非源表位于不同的模式中，否则会      
	 * 出现重名失败。）         
	 */
	index->idxname = NULL;

	/*
	 * 若索引被标记为 PRIMARY 或带有排他条件，那它      
	 * 肯定来自约束；否则，若它未标记为 UNIQUE，      
	 * 就肯定不是。如果它确定或可能来自约束，           
	 * 须取 pg_constraint 记录。 
	 */
	if (index->primary || index->unique || idxrec->indisexclusion)
	{
		Oid			constraintId = get_index_constraint(source_relid);

		if (OidIsValid(constraintId))
		{
			HeapTuple	ht_constr;
			Form_pg_constraint conrec;

			if (constraintOid)
				*constraintOid = constraintId;

			ht_constr = SearchSysCache1(CONSTROID,
										ObjectIdGetDatum(constraintId));
			if (!HeapTupleIsValid(ht_constr))
				elog(ERROR, "cache lookup failed for constraint %u",
					 constraintId);
			conrec = (Form_pg_constraint) GETSTRUCT(ht_constr);

			index->isconstraint = true;
			index->deferrable = conrec->condeferrable;
			index->initdeferred = conrec->condeferred;

			/* 如果是排他约束，就需要操作符的名字         */
			if (idxrec->indisexclusion)
			{
				Datum	   *elems;
				int			nElems;
				int			i;

				Assert(conrec->contype == CONSTRAINT_EXCLUSION);
				/* 从 pg_constraint 元组提取操作符 OID        */
				datum = SysCacheGetAttrNotNull(CONSTROID, ht_constr,
											   Anum_pg_constraint_conexclop);
				deconstruct_array_builtin(DatumGetArrayTypeP(datum), OIDOID, &elems, NULL, &nElems);

				for (i = 0; i < nElems; i++)
				{
					Oid			operid = DatumGetObjectId(elems[i]);
					HeapTuple	opertup;
					Form_pg_operator operform;
					char	   *oprname;
					char	   *nspname;
					List	   *namelist;

					opertup = SearchSysCache1(OPEROID,
											  ObjectIdGetDatum(operid));
					if (!HeapTupleIsValid(opertup))
						elog(ERROR, "cache lookup failed for operator %u",
							 operid);
					operform = (Form_pg_operator) GETSTRUCT(opertup);
					oprname = pstrdup(NameStr(operform->oprname));
					/* 为简单起见，操作符名总是加模式限定 */
					nspname = get_namespace_name(operform->oprnamespace);
					namelist = list_make2(makeString(nspname),
										  makeString(oprname));
					index->excludeOpNames = lappend(index->excludeOpNames,
													namelist);
					ReleaseSysCache(opertup);
				}
			}

			ReleaseSysCache(ht_constr);
		}
		else
			index->isconstraint = false;
	}
	else
		index->isconstraint = false;

	/* 取索引表达式（若有）    */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indexprs, &isnull);
	if (!isnull)
	{
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		indexprs = (List *) stringToNode(exprsString);
	}
	else
		indexprs = NIL;

	/* 构建 IndexElem 列表     */
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;

	indexpr_item = list_head(indexprs);
	for (keyno = 0; keyno < idxrec->indnkeyatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);
		int16		opt = source_idx->rd_indoption[keyno];

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* 普通索引列     */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);
			keycoltype = get_atttype(indrelid, attnum);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
		{
			/* 表达式索引    */
			Node	   *indexkey;
			bool		found_whole_row;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexprs, indexpr_item);

			/* 调整 Var 使之匹配新表的列编号         */
			indexkey = map_variable_attnos(indexkey,
										   1, 0,
										   attmap,
										   InvalidOid, &found_whole_row);

			/* 与 expandTableLikeClause 一样，拒绝整行变量   */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Index \"%s\" contains a whole-row table reference.",
								   RelationGetRelationName(source_idx))));

			iparam->name = NULL;
			iparam->expr = indexkey;

			keycoltype = exprType(indexkey);
		}

		/* 复制原索引列名               */
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		/* 若非默认则加上排序规则名   */
		iparam->collation = get_collation(indcollation->values[keyno], keycoltype);

		/* 若非默认则加上操作符类名        */
		iparam->opclass = get_opclass(indclass->values[keyno], keycoltype);
		iparam->opclassopts =
			untransformRelOptions(get_attoptions(source_relid, keyno + 1));

		iparam->ordering = SORTBY_DEFAULT;
		iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;

		/* 必要时调整选项       */
		if (source_idx->rd_indam->amcanorder)
		{
			/*
			 * 若它支持排序顺序，则复制 DESC 与 NULLS 选项。
			 * 但不要不必要地设置非默认值，以便        
			 * 提高识别出与约束索引等价的机会。           
			 *         
			 */
			if (opt & INDOPTION_DESC)
			{
				iparam->ordering = SORTBY_DESC;
				if ((opt & INDOPTION_NULLS_FIRST) == 0)
					iparam->nulls_ordering = SORTBY_NULLS_LAST;
			}
			else
			{
				if (opt & INDOPTION_NULLS_FIRST)
					iparam->nulls_ordering = SORTBY_NULLS_FIRST;
			}
		}

		index->indexParams = lappend(index->indexParams, iparam);
	}

	/* 单独处理包含列              */
	for (keyno = idxrec->indnkeyatts; keyno < idxrec->indnatts; keyno++)
	{
		IndexElem  *iparam;
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(source_idx),
											   keyno);

		iparam = makeNode(IndexElem);

		if (AttributeNumberIsValid(attnum))
		{
			/* 普通索引列     */
			char	   *attname;

			attname = get_attname(indrelid, attnum, false);

			iparam->name = attname;
			iparam->expr = NULL;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("expressions are not supported in included columns")));

		/* 复制原索引列名               */
		iparam->indexcolname = pstrdup(NameStr(attr->attname));

		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}
	/* 有则复制 reloptions*/
	datum = SysCacheGetAttr(RELOID, ht_idxrel,
							Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		index->options = untransformRelOptions(datum);

	/* 如果是部分索引，则反编译并追加其谓词。   */
	datum = SysCacheGetAttr(INDEXRELID, ht_idx,
							Anum_pg_index_indpred, &isnull);
	if (!isnull)
	{
		char	   *pred_str;
		Node	   *pred_tree;
		bool		found_whole_row;

		/* 把文本串转成节点树      */
		pred_str = TextDatumGetCString(datum);
		pred_tree = (Node *) stringToNode(pred_str);

		/* 调整 Var 使之匹配新表的列编号         */
		pred_tree = map_variable_attnos(pred_tree,
										1, 0,
										attmap,
										InvalidOid, &found_whole_row);

		/* 与 expandTableLikeClause 一样，拒绝整行变量   */
		if (found_whole_row)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert whole-row table reference"),
					 errdetail("Index \"%s\" contains a whole-row table reference.",
							   RelationGetRelationName(source_idx))));

		index->whereClause = pred_tree;
	}

	/* 清理   */
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_am);

	return index;
}

/*
 * 根据已存在的扩展统计 "source_statsid" 的信息，为           
 * heapRel 与 heapRelid 所标识的关系生成 CreateStatsStmt            
 *           
 *
 * 表达式 Var 中的属性号会按 attmap 进行调整。            
 */
static CreateStatsStmt *
generateClonedExtStatsStmt(RangeVar *heapRel, Oid heapRelid,
						   Oid source_statsid, const AttrMap *attmap)
{
	HeapTuple	ht_stats;
	Form_pg_statistic_ext statsrec;
	CreateStatsStmt *stats;
	List	   *stat_types = NIL;
	List	   *def_names = NIL;
	bool		isnull;
	Datum		datum;
	ArrayType  *arr;
	char	   *enabled;
	int			i;

	Assert(OidIsValid(heapRelid));
	Assert(heapRel != NULL);

	/*
	 * 取源统计对象的 pg_statistic_ext 元组。         
	 */
	ht_stats = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(source_statsid));
	if (!HeapTupleIsValid(ht_stats))
		elog(ERROR, "cache lookup failed for statistics object %u", source_statsid);
	statsrec = (Form_pg_statistic_ext) GETSTRUCT(ht_stats);

	/* 确定存在哪些统计类型         */
	datum = SysCacheGetAttrNotNull(STATEXTOID, ht_stats,
								   Anum_pg_statistic_ext_stxkind);
	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a 1-D char array");
	enabled = (char *) ARR_DATA_PTR(arr);
	for (i = 0; i < ARR_DIMS(arr)[0]; i++)
	{
		if (enabled[i] == STATS_EXT_NDISTINCT)
			stat_types = lappend(stat_types, makeString("ndistinct"));
		else if (enabled[i] == STATS_EXT_DEPENDENCIES)
			stat_types = lappend(stat_types, makeString("dependencies"));
		else if (enabled[i] == STATS_EXT_MCV)
			stat_types = lappend(stat_types, makeString("mcv"));
		else if (enabled[i] == STATS_EXT_EXPRESSIONS)
			/* 表达式统计不对用户暴露         */
			continue;
		else
			elog(ERROR, "unrecognized statistics kind %c", enabled[i]);
	}

	/* 确定统计位于哪些列上                */
	for (i = 0; i < statsrec->stxkeys.dim1; i++)
	{
		StatsElem  *selem = makeNode(StatsElem);
		AttrNumber	attnum = statsrec->stxkeys.values[i];

		selem->name = get_attname(heapRelid, attnum, false);
		selem->expr = NULL;

		def_names = lappend(def_names, selem);
	}

	/*
	 * 接下来处理表达式（如果有）。相对于普通属性     
	 * 的顺序对扩展统计其实无关紧要，因此我们           
	 * 只是把它们追加在普通列引用之后。  
	 *
	 * XXX 在构建/估算期间，有些地方把表达式当作         
	 * 位于属性之前，但对 CREATE 命令来说这完全         
	 * 无关。  
	 */
	datum = SysCacheGetAttr(STATEXTOID, ht_stats,
							Anum_pg_statistic_ext_stxexprs, &isnull);

	if (!isnull)
	{
		ListCell   *lc;
		List	   *exprs = NIL;
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		exprs = (List *) stringToNode(exprsString);

		foreach(lc, exprs)
		{
			Node	   *expr = (Node *) lfirst(lc);
			StatsElem  *selem = makeNode(StatsElem);
			bool		found_whole_row;

			/* 调整 Var 使之匹配新表的列编号         */
			expr = map_variable_attnos(expr,
									   1, 0,
									   attmap,
									   InvalidOid,
									   &found_whole_row);

			selem->name = NULL;
			selem->expr = expr;

			def_names = lappend(def_names, selem);
		}

		pfree(exprsString);
	}

	/* 最后构建输出节点       */
	stats = makeNode(CreateStatsStmt);
	stats->defnames = NULL;
	stats->stat_types = stat_types;
	stats->exprs = def_names;
	stats->relations = list_make1(heapRel);
	stats->stxcomment = NULL;
	stats->transformed = true;	/* 无需再次 transformStatsStmt     */
	stats->if_not_exists = false;

	/* 清理   */
	ReleaseSysCache(ht_stats);

	return stats;
}

/*
 * get_collation		- 获取排序规则的限定名     
 *
 * 若 collation 为 InvalidOid 或等于给定 actual_datatype               
 * 的默认值则返回 NIL。 
 */
static List *
get_collation(Oid collation, Oid actual_datatype)
{
	List	   *result;
	HeapTuple	ht_coll;
	Form_pg_collation coll_rec;
	char	   *nsp_name;
	char	   *coll_name;

	if (!OidIsValid(collation))
		return NIL;				/* 简单    */
	if (collation == get_typcollation(actual_datatype))
		return NIL;				/* 让它取默认值  */

	ht_coll = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(ht_coll))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	coll_rec = (Form_pg_collation) GETSTRUCT(ht_coll);

	/* 为简单起见，名字总是加模式限定     */
	nsp_name = get_namespace_name(coll_rec->collnamespace);
	coll_name = pstrdup(NameStr(coll_rec->collname));
	result = list_make2(makeString(nsp_name), makeString(coll_name));

	ReleaseSysCache(ht_coll);
	return result;
}

/*
 * get_opclass		- 获取索引操作符类的限定名            
 *
 * 若操作符类是给定 actual_datatype 的默认类，         
 * 则返回 NIL。        
 */
static List *
get_opclass(Oid opclass, Oid actual_datatype)
{
	List	   *result = NIL;
	HeapTuple	ht_opc;
	Form_pg_opclass opc_rec;

	ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opc_rec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (GetDefaultOpClass(actual_datatype, opc_rec->opcmethod) != opclass)
	{
		/* 为简单起见，名字总是加模式限定     */
		char	   *nsp_name = get_namespace_name(opc_rec->opcnamespace);
		char	   *opc_name = pstrdup(NameStr(opc_rec->opcname));

		result = list_make2(makeString(nsp_name), makeString(opc_name));
	}

	ReleaseSysCache(ht_opc);
	return result;
}


/*
 * transformIndexConstraints
 *		处理会创建索引的 UNIQUE、PRIMARY KEY、EXCLUDE 约束。     
 *		还会合并进 LIKE ... INCLUDING INDEXES 所     
 *		产生的索引定义。   
 */
static void
transformIndexConstraints(CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	List	   *finalindexlist = NIL;
	ListCell   *lc;

	/*
	 * 遍历那些需要生成索引的约束。对 PRIMARY KEY，           
	 * 把每一列标记为 NOT NULL 并创建索引。对 UNIQUE 或     
	 * EXCLUDE，则像 PRIMARY KEY 那样创建索引，但不强求 NOT   
	 * NULL 
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = lfirst_node(Constraint, lc);

		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE ||
			   constraint->contype == CONSTR_EXCLUSION);

		index = transformIndexConstraint(constraint, cxt);

		indexlist = lappend(indexlist, index);
	}

	/*
	 * 扫描索引列表并移除任何冗余的索引声明。这种        
	 * 情况可能出现在例如用户写了 UNIQUE PRIMARY KEY 时。 
	 * 严格按 SQL 来讲应该改为抛出一个错误，但我觉得     
	 * 那太过迂腐刻板。 - tgl 2001-02-14         
	 *
	 * XXX 在 ALTER TABLE 情形下，若还能查找重复的   
	 * 既有索引就更好。  
	 */
	if (cxt->pkey != NULL)
	{
		/* 务必优先保留 PKEY 索引而非其他索引……      */
		finalindexlist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(lc);

		/* 如果是 pkey，它已在 finalindexlist 中*/
		if (index == cxt->pkey)
			continue;

		foreach(k, finalindexlist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->indexIncludingParams, priorindex->indexIncludingParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				equal(index->excludeOpNames, priorindex->excludeOpNames) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
				index->nulls_not_distinct == priorindex->nulls_not_distinct &&
				index->deferrable == priorindex->deferrable &&
				index->initdeferred == priorindex->initdeferred)
			{
				priorindex->unique |= index->unique;

				/*
				 * 若先前索引尚未命名，而这个已命名，  
				 * 则把名字转移给先前的索引。这样         
				 * 以确保同时有命名与未命名约束时，      
				 * 索引将（至少部分）使用这些名字。    
				 */
				if (priorindex->idxname == NULL)
					priorindex->idxname = index->idxname;
				keep = false;
				break;
			}
		}

		if (keep)
			finalindexlist = lappend(finalindexlist, index);
	}

	/*
	 * 现在把所有 IndexStmt 追加到 cxt->alist。                     
	 * 若为支持主键生成 ALTER TABLE SET NOT NULL 语句，它已在  
	 * cxt->alist 
	 */
	cxt->alist = list_concat(cxt->alist, finalindexlist);
}

/*
 * transformIndexConstraint
 *		为 transformIndexConstraints 转换 UNIQUE、PRIMARY KEY   
 *		或 EXCLUDE 约束。     
 *
 * 我们返回一个 IndexStmt。对于 PRIMARY KEY 约束，我们     
 * 还会额外产生 NOT NULL 约束，或是把 cxt->columns 中的        
 * ColumnDef 标记为 is_not_null，或加入 ALTER TABLE SET NOT   
 * NULL 命令
 */
static IndexStmt *
transformIndexConstraint(Constraint *constraint, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *notnullcmds = NIL;
	ListCell   *lc;

	index = makeNode(IndexStmt);

	index->unique = (constraint->contype != CONSTR_EXCLUSION);
	index->primary = (constraint->contype == CONSTR_PRIMARY);
	if (index->primary)
	{
		if (cxt->pkey != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("multiple primary keys for table \"%s\" are not allowed",
							cxt->relation->relname),
					 parser_errposition(cxt->pstate, constraint->location)));
		cxt->pkey = index;

		/*
		 * 在 ALTER TABLE 情形下，主索引可能已经存在，但
		 * DefineIndex 会检查。      
		 */
	}
	index->nulls_not_distinct = constraint->nulls_not_distinct;
	index->isconstraint = true;
	index->deferrable = constraint->deferrable;
	index->initdeferred = constraint->initdeferred;

	if (constraint->conname != NULL)
		index->idxname = pstrdup(constraint->conname);
	else
		index->idxname = NULL;	/* 名字由 DefineIndex 选定 */

	index->relation = cxt->relation;
	index->accessMethod = constraint->access_method ? constraint->access_method : DEFAULT_INDEX_TYPE;
	index->options = constraint->options;
	index->tableSpace = constraint->indexspace;
	index->whereClause = constraint->where_clause;
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNumber = InvalidRelFileNumber;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;
	index->transformed = false;
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = constraint->reset_default_tblspc;

	/*
	 * 如果是 ALTER TABLE ADD CONSTRAINT USING INDEX，则查找         
	 * 该索引并验证可用，再提取隐含列名列表。（我们 
	 * 运行时实际上并不需要这份列名列表，但现在需要    
	 * 用它检查下方是否有重复的列项。） 
	 */
	if (constraint->indexname != NULL)
	{
		char	   *index_name = constraint->indexname;
		Relation	heap_rel = cxt->rel;
		Oid			index_oid;
		Relation	index_rel;
		Form_pg_index index_form;
		oidvector  *indclass;
		Datum		indclassDatum;
		int			i;

		/* 文法不应允许在显式给出列清单时这样做  */
		Assert(constraint->keys == NIL);

		/* 文法只应允许 PRIMARY 与 UNIQUE 约束             */
		Assert(constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE);

		/* 必须是 ALTER 而非 CREATE，文法并未强制这点    */
		if (!cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot use an existing index in CREATE TABLE"),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* 在与表相同的模式中查找该索引         */
		index_oid = get_relname_relid(index_name, RelationGetNamespace(heap_rel));

		if (!OidIsValid(index_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" does not exist", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* 打开该索引（若它不是索引将抛出错误）          */
		index_rel = index_open(index_oid, AccessShareLock);
		index_form = index_rel->rd_index;

		/* 检查它是否已经存在关联的约束                   */
		if (OidIsValid(get_index_constraint(index_oid)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is already associated with a constraint",
							index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* 对该索引执行有效性检查    */
		if (index_form->indrelid != RelationGetRelid(heap_rel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" does not belong to table \"%s\"",
							index_name, RelationGetRelationName(heap_rel)),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!index_form->indisvalid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is not valid", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (!index_form->indisunique)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a unique index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexExpressions(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" contains expressions", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		if (RelationGetIndexPredicate(index_rel) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a partial index", index_name),
					 errdetail("Cannot create a primary key or unique constraint using such an index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * 把延迟索引改成非延迟很可能不安全（非约束索引 
		 * 本来就无法延迟，故这种情况不应出现；        
		 * 无需多虑，但仍要检查一下）                   
		 */
		if (!index_form->indimmediate && !constraint->deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a deferrable index", index_name),
					 errdetail("Cannot create a non-deferrable constraint using a deferrable index."),
					 parser_errposition(cxt->pstate, constraint->location)));

		/*
		 * 坚持要求该索引为 btree。目前也只有 btree         
		 * 支持唯一性；但我们必须有一个与普通            
		 * ADD CONSTRAINT 语法所得索引完全一致的                
		 * 索引，否则 dump/reload 会得出不同索引（尤其   
		 * 破坏 pg_upgrade）。   
		 */
		if (index_rel->rd_rel->relam != get_index_am_oid(DEFAULT_INDEX_TYPE, false))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" is not a btree", index_name),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* indclass 只能费力获取    */
		indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
											   index_rel->rd_indextuple,
											   Anum_pg_index_indclass);
		indclass = (oidvector *) DatumGetPointer(indclassDatum);

		for (i = 0; i < index_form->indnatts; i++)
		{
			int16		attnum = index_form->indkey.values[i];
			const FormData_pg_attribute *attform;
			char	   *attname;
			Oid			defopclass;

			/*
			 * 此处不应出现 attnum == 0，表达式索引已被      
			 * 拒绝；若真的出现，SystemAttributeDefinition         
			 * 将报错。   
			 */
			if (attnum > 0)
			{
				Assert(attnum <= heap_rel->rd_att->natts);
				attform = TupleDescAttr(heap_rel->rd_att, attnum - 1);
			}
			else
				attform = SystemAttributeDefinition(attnum);
			attname = pstrdup(NameStr(attform->attname));

			if (i < index_form->indnkeyatts)
			{
				/*
				 * 坚持默认 opclass、collation 和排序选项。    
				 * 索引用非默认设置仍可作为约束，        
				 * 但其唯一性语义未必与正常创建的约束        
				 * 完全相同；另外还有上面提到的 dump/reload   
				 * 问题。                                           
				 *                 
				 */
				Datum		attoptions =
					get_attoptions(RelationGetRelid(index_rel), i + 1);

				defopclass = GetDefaultOpClass(attform->atttypid,
											   index_rel->rd_rel->relam);
				if (indclass->values[i] != defopclass ||
					attform->attcollation != index_rel->rd_indcollation[i] ||
					attoptions != (Datum) 0 ||
					index_rel->rd_indoption[i] != 0)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("index \"%s\" column number %d does not have default sorting behavior", index_name, i + 1),
							 errdetail("Cannot create a primary key or unique constraint using such an index."),
							 parser_errposition(cxt->pstate, constraint->location)));

				constraint->keys = lappend(constraint->keys, makeString(attname));
			}
			else
				constraint->including = lappend(constraint->including, makeString(attname));
		}

		/* 关闭索引关系但保留锁             */
		relation_close(index_rel, NoLock);

		index->indexOid = index_oid;
	}

	/*
	 * 若是 EXCLUDE 约束，语法分析返回的是 IndexElem           
	 * 与操作符名成对的列表，我们必须拆成独立的    
	 * 列表。      
	 */
	if (constraint->contype == CONSTR_EXCLUSION)
	{
		foreach(lc, constraint->exclusions)
		{
			List	   *pair = (List *) lfirst(lc);
			IndexElem  *elem;
			List	   *opname;

			Assert(list_length(pair) == 2);
			elem = linitial_node(IndexElem, pair);
			opname = lsecond_node(List, pair);

			index->indexParams = lappend(index->indexParams, elem);
			index->excludeOpNames = lappend(index->excludeOpNames, opname);
		}
	}

	/*
	 * 对于 UNIQUE 和 PRIMARY KEY，我们只有列名的列表。  
	 *
	 * 确保被引用的键存在。若创建的是 PRIMARY KEY 索引，    
	 * 并确保它们是 NOT NULL。   
	 */
	else
	{
		foreach(lc, constraint->keys)
		{
			char	   *key = strVal(lfirst(lc));
			bool		found = false;
			bool		forced_not_null = false;
			ColumnDef  *column = NULL;
			ListCell   *columns;
			IndexElem  *iparam;

			/* 确保被引用的列存在。      */
			foreach(columns, cxt->columns)
			{
				column = lfirst_node(ColumnDef, columns);
				if (strcmp(column->colname, key) == 0)
				{
					found = true;
					break;
				}
			}
			if (found)
			{
				/*
				 * 该列定义于新表。对 PRIMARY KEY，可在此处  
				 * 以低成本施加 NOT NULL 约束……除非           
				 * 该列标记了 is_from_type，此时在此处标记将    
				 * 不会生效（见 MergeAttributes）。        
				 */
				if (constraint->contype == CONSTR_PRIMARY &&
					!column->is_from_type)
				{
					column->is_not_null = true;
					forced_not_null = true;
				}
			}
			else if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * 该列在新表中将是系统列，因此接受它。    
				 * 系统列永远不可能为空，故无需担心          
				 * PRIMARY/NOT NULL 约束。        
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* 尝试继承表      */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* 检查用户是否从有效的 relkind 继承       */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;

							/*
							 * 如果父列已是 NOT NULL，很想设置 
							 * forced_not_null，但这似乎不安全， 
							 * 因为该列的 NOT NULL 标记           
							 * 可能在执行前消失。            
							 * 为安全起见，请做运行时检查。  
							 */
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}

			/*
			 * 在 ALTER TABLE 场景下，不要抱怨命令中未        
			 * 创建的索引键；它们很可能已经存在。 
			 * 若不存在，DefineIndex 会报错。     
			 */
			if (!found && !cxt->isalter)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in key does not exist", key),
						 parser_errposition(cxt->pstate, constraint->location)));

			/* 检查 PRIMARY KEY(foo, foo)    */
			foreach(columns, index->indexParams)
			{
				iparam = (IndexElem *) lfirst(columns);
				if (iparam->name && strcmp(key, iparam->name) == 0)
				{
					if (index->primary)
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column \"%s\" appears twice in primary key constraint",
										key),
								 parser_errposition(cxt->pstate, constraint->location)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column \"%s\" appears twice in unique constraint",
										key),
								 parser_errposition(cxt->pstate, constraint->location)));
				}
			}

			/* 好，将它加入索引定义     */
			iparam = makeNode(IndexElem);
			iparam->name = pstrdup(key);
			iparam->expr = NULL;
			iparam->indexcolname = NULL;
			iparam->collation = NIL;
			iparam->opclass = NIL;
			iparam->opclassopts = NIL;
			iparam->ordering = SORTBY_DEFAULT;
			iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
			index->indexParams = lappend(index->indexParams, iparam);

			/*
			 * 对主键列，若上面未能用 is_not_null 确保，      
			 * 则还要为 ALTER TABLE SET NOT NULL 创建一项。       
			 */
			if (constraint->contype == CONSTR_PRIMARY && !forced_not_null)
			{
				AlterTableCmd *notnullcmd = makeNode(AlterTableCmd);

				notnullcmd->subtype = AT_SetNotNull;
				notnullcmd->name = pstrdup(key);
				notnullcmds = lappend(notnullcmds, notnullcmd);
			}
		}
	}

	/*
	 * 把包含列加入索引定义。这与上面处理简单列名 
	 * 列表的代码非常相似，只是我们不关心 NOT NULL        
	 * 标记；主键中的包含列不应被强制为 NOT NULL。     
	 * 对重复列也不报错，不过也许应该报错？              
	 *                        
	 */
	foreach(lc, constraint->including)
	{
		char	   *key = strVal(lfirst(lc));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;
		IndexElem  *iparam;

		foreach(columns, cxt->columns)
		{
			column = lfirst_node(ColumnDef, columns);
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * 该列在新表中将是系统列，因此接受它。    
				 *    
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* 尝试继承表      */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* 检查用户是否从有效的 relkind 继承       */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}
		}

		/*
		 * 在 ALTER TABLE 场景下，不要抱怨命令中未        
		 * 创建的索引键；它们很可能已经存在。DefineIndex  
		 * 会报错。                    
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist", key),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* 好，将它加入索引定义     */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->indexcolname = NULL;
		iparam->collation = NIL;
		iparam->opclass = NIL;
		iparam->opclassopts = NIL;
		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}

	/*
	 * 如果发现了任何需要运行时 SET NOT NULL 的项，就构建  
	 * 完整的 ALTER TABLE 命令并加入 cxt->alist。   
	 */
	if (notnullcmds)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = copyObject(cxt->relation);
		alterstmt->cmds = notnullcmds;
		alterstmt->objtype = OBJECT_TABLE;
		alterstmt->missing_ok = false;

		cxt->alist = lappend(cxt->alist, alterstmt);
	}

	return index;
}

/*
 * transformCheckConstraints
 *		处理 CHECK 约束     
 *
 * 目前从 ALTER TABLE 调用到这里时无事可做，            
 * 但其他约束转换函数在 CREATE TABLE 与 ALTER TABLE         
 * 两条路径中都会被调用，所以这里也照此办理，      
 * 若无权限跳过校验，就不做任何事。             
 */
static void
transformCheckConstraints(CreateStmtContext *cxt, bool skipValidation)
{
	ListCell   *ckclist;

	if (cxt->ckconstraints == NIL)
		return;

	/*
	 * 创建新表（但非外部表）时，可安全跳过               
	 * 对 CHECK 约束的校验，并且仍将它们标记为有效。       
	 * （将覆盖用户给的 NOT VALID 标志。） 
	 */
	if (skipValidation)
	{
		foreach(ckclist, cxt->ckconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(ckclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}
}

/*
 * transformFKConstraints
 *		处理 FOREIGN KEY 约束     
 */
static void
transformFKConstraints(CreateStmtContext *cxt,
					   bool skipValidation, bool isAddConstraint)
{
	ListCell   *fkclist;

	if (cxt->fkconstraints == NIL)
		return;

	/*
	 * 如果是 CREATE TABLE 或者添加带 NULL 默认值的列，     
	 * 就可以安全跳过 FK 约束校验，同时仍把它们标记   
	 * 为有效。（覆盖用户的 NOT VALID 标志。） 
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}

	/*
	 * 对于 CREATE TABLE 或 ALTER TABLE ADD COLUMN，需要生成        
	 * 一条 ALTER TABLE ADD CONSTRAINT 命令，待基础命令完成后   
	 * 执行。（若由 ADD CONSTRAINT 调用，例程会把 FK 约束      
	 * 并入子命令列表） 
	 *
	 * 注意：ADD CONSTRAINT 命令还须在任何创建索引命令   
	 * 之后执行。因此，它应运行在        
	 * transformIndexConstraints 之后，这样 CREATE INDEX 命令   
	 * 已经在 cxt->alist 中。另见 cxt->likeclauses 的处理。   
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->objtype = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_AddConstraint;
			altercmd->name = NULL;
			altercmd->def = (Node *) constraint;
			alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
		}

		cxt->alist = lappend(cxt->alist, alterstmt);
	}
}

/*
 * transformIndexStmt - CREATE INDEX 与 ALTER TABLE 的解析分析   
 *
 * 注意：对既不用索引表达式也不用谓词的索引，         
 * 本函数是空操作。有若干代码路径创建索引时             
 * 并未调用它，因为它们知道自己没有这类表达式      
 * 需要处理。               
 *
 * 为避免竞态条件，本函数确定目标关系时只能依赖      
 * 传入的 relid（而不能依赖 stmt->relation）。                 
 *          
 */
IndexStmt *
transformIndexStmt(Oid relid, IndexStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;
	Relation	rel;

	/* 若语句已转换过，则无事可做。      */
	if (stmt->transformed)
		return stmt;

	/* 设置 pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * 把父表放入 rtable，使表达式无需限定即可引用         
	 * 其字段。调用方负责锁定关系，但我们仍需打开它。  
	 *                                        
	 */
	rel = relation_open(relid, NoLock);
	nsitem = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   NULL, false, true);

	/* 进命名空间不进连接列表  */
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* 处理 WHERE 子句           */
	if (stmt->whereClause)
	{
		stmt->whereClause = transformWhereClause(pstate,
												 stmt->whereClause,
												 EXPR_KIND_INDEX_PREDICATE,
												 "WHERE");
		/* 还需要修正它的排序规则 */
		assign_expr_collations(pstate, stmt->whereClause);
	}

	/* 处理所有索引表达式        */
	foreach(l, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);

		if (ielem->expr)
		{
			/* 在转换表达式之前先提取初步的索引列名      */
			if (ielem->indexcolname == NULL)
				ielem->indexcolname = FigureIndexColname(ielem->expr);

			/* 现在对表达式做解析转换             */
			ielem->expr = transformExpr(pstate, ielem->expr,
										EXPR_KIND_INDEX_EXPRESSION);

			/* 还需要修正它的排序规则 */
			assign_expr_collations(pstate, ielem->expr);

			/*
			 * transformExpr() 应已按索引表达式的 EXPR_KIND_  
			 * 拒绝了子查询、聚集、窗口函数和 SRF。           
			 *                         
			 *
			 * DefineIndex() 会做更多检查。 
			 */
		}
	}

	/*
	 * 检查是否只提到了基础关系。（既然 add_missing_from    
	 * 已成历史，应为死代码。）  
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("index expressions and predicates can refer only to the table being indexed")));

	free_parsestate(pstate);

	/* 关闭关系   */
	table_close(rel, NoLock);

	/* 把语句标记为已成功转换          */
	stmt->transformed = true;

	return stmt;
}

/*
 * transformStatsStmt - CREATE STATISTICS 的解析分析   
 *
 * 为避免竞态条件，本函数确定目标关系时只能依赖传入  
 * 的 relid（而不能依赖 stmt->relation）。                       
 *          
 */
CreateStatsStmt *
transformStatsStmt(Oid relid, CreateStatsStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;
	Relation	rel;

	/* 若语句已转换过，则无事可做。      */
	if (stmt->transformed)
		return stmt;

	/* 设置 pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * 把父表放入 rtable，使表达式无需限定即可引用         
	 * 其字段。调用方负责锁定关系，但我们仍需打开它。  
	 *                                        
	 */
	rel = relation_open(relid, NoLock);
	nsitem = addRangeTableEntryForRelation(pstate, rel,
										   AccessShareLock,
										   NULL, false, true);

	/* 进命名空间不进连接列表  */
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* 处理所有表达式        */
	foreach(l, stmt->exprs)
	{
		StatsElem  *selem = (StatsElem *) lfirst(l);

		if (selem->expr)
		{
			/* 现在对表达式做解析转换             */
			selem->expr = transformExpr(pstate, selem->expr,
										EXPR_KIND_STATS_EXPRESSION);

			/* 还需要修正它的排序规则 */
			assign_expr_collations(pstate, selem->expr);
		}
	}

	/*
	 * 检查是否只提到了基础关系。（既然 add_missing_from    
	 * 已成历史，应为死代码。）  
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("statistics expressions can refer only to the table being referenced")));

	free_parsestate(pstate);

	/* 关闭关系   */
	table_close(rel, NoLock);

	/* 把语句标记为已成功转换          */
	stmt->transformed = true;

	return stmt;
}


/*
 * transformRuleStmt -
 *	  转换 CREATE RULE 语句。其动作是解析树列表，会被
 *	  转换为查询树列表，另外我们还要转换               
 *	  WHERE 子句（若有）。       
 *
 * actions 和 whereClause 是输出参数，用于接收转换  
 * 后的结果。     
 */
void
transformRuleStmt(RuleStmt *stmt, const char *queryString,
				  List **actions, Node **whereClause)
{
	Relation	rel;
	ParseState *pstate;
	ParseNamespaceItem *oldnsitem;
	ParseNamespaceItem *newnsitem;

	/*
	 * 为免死锁，第一件事必须在目标关系上获取 
	 * AccessExclusiveLock。DefineQueryRewrite() 会需要它，         
	 * 我们不想事先获取更弱的锁。                      
	 *            
	 */
	rel = table_openrv(stmt->relation, AccessExclusiveLock);

	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rules on materialized views are not supported")));

	/* 设置 pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * 注意：'OLD' 的 varno 必须总为 1，'NEW' 的必须总为 2。 
	 * 请在主 pstate 中设置它们的 ParseNamespaceItems，供解析   
	 * 规则限定用。     
	 */
	oldnsitem = addRangeTableEntryForRelation(pstate, rel,
											  AccessShareLock,
											  makeAlias("old", NIL),
											  false, false);
	newnsitem = addRangeTableEntryForRelation(pstate, rel,
											  AccessShareLock,
											  makeAlias("new", NIL),
											  false, false);

	/*
	 * 为便于查找，它们也必须位于命名空间中，但只添加  
	 * 与当前规则种类相关者。UPDATE 规则中，限定条件    
	 * 必须引用 OLD.field 或 NEW.field 才无歧义，但 INSERT 与     
	 * DELETE 则无需如此挑剔。我们不会将其加入连接列表。 
	 *                 
	 */
	switch (stmt->event)
	{
		case CMD_SELECT:
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			break;
		case CMD_UPDATE:
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			addNSItemToQuery(pstate, newnsitem, false, true, true);
			break;
		case CMD_INSERT:
			addNSItemToQuery(pstate, newnsitem, false, true, true);
			break;
		case CMD_DELETE:
			addNSItemToQuery(pstate, oldnsitem, false, true, true);
			break;
		default:
			elog(ERROR, "unrecognized event type: %d",
				 (int) stmt->event);
			break;
	}

	/* 处理 WHERE 子句           */
	*whereClause = transformWhereClause(pstate,
										stmt->whereClause,
										EXPR_KIND_WHERE,
										"WHERE");
	/* 还需要修正它的排序规则 */
	assign_expr_collations(pstate, *whereClause);

	/* 没有 add_missing_from 时这多半是死代码：  */
	if (list_length(pstate->p_rtable) != 2) /* 太调皮了……  */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("rule WHERE condition cannot contain references to other relations")));

	/*
	 * 有限定条件的 'instead nothing' 规则要有查询范围表，    
	 * 以便重写处理器把取反的规则限定加到原查询     
	 * 中。此处创建一个命令类型为 CMD_NOTHING 的新查询，    
	 * 重写系统会特殊处理它。                    
	 */
	if (stmt->actions == NIL)
	{
		Query	   *nothing_qry = makeNode(Query);

		nothing_qry->commandType = CMD_NOTHING;
		nothing_qry->rtable = pstate->p_rtable;
		nothing_qry->rteperminfos = pstate->p_rteperminfos;
		nothing_qry->jointree = makeFromExpr(NIL, NULL);	/* 无需连接   */

		*actions = list_make1(nothing_qry);
	}
	else
	{
		ListCell   *l;
		List	   *newactions = NIL;

		/*
		 * 像 parse_sub_analyze() 那样转换每条语句  
		 */
		foreach(l, stmt->actions)
		{
			Node	   *action = (Node *) lfirst(l);
			ParseState *sub_pstate = make_parsestate(NULL);
			Query	   *sub_qry,
					   *top_subqry;
			bool		has_old,
						has_new;

			/*
			 * 由于外层 ParseState 不是内层父节点，必须手工   
			 * 传下查询文本。  
			 */
			sub_pstate->p_sourcetext = queryString;

			/*
			 * 在本语句的 rtable 中设置 OLD/NEW。这些条目      
			 * 只加入 relnamespace，而不加入 varnamespace，因为 
			 * 不想让它们在规则动作中被非限定字段名或   
			 * "*" 所引用。是否加入连接列表稍后决定。     
			 *                      
			 */
			oldnsitem = addRangeTableEntryForRelation(sub_pstate, rel,
													  AccessShareLock,
													  makeAlias("old", NIL),
													  false, false);
			newnsitem = addRangeTableEntryForRelation(sub_pstate, rel,
													  AccessShareLock,
													  makeAlias("new", NIL),
													  false, false);
			addNSItemToQuery(sub_pstate, oldnsitem, false, true, false);
			addNSItemToQuery(sub_pstate, newnsitem, false, true, false);

			/* 转换规则动作语句            */
			top_subqry = transformStmt(sub_pstate, action);

			/*
			 * 规则 WHERE 条件非空，无法支持工具语句动作  
			 * （如 NOTIFY），因为没办法让工具动作有条件地 
			 * 执行。                                
			 */
			if (top_subqry->commandType == CMD_UTILITY &&
				*whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("rules with WHERE conditions can only have SELECT, INSERT, UPDATE, or DELETE actions")));

			/*
			 * 如果动作是 INSERT...SELECT，OLD/NEW 已被下推到      
			 * SELECT 中，那是要检查的对象。（丑陋的      
			 * 权宜之计……重新设计查询树时再修）    
			 */
			sub_qry = getInsertSelectQuery(top_subqry, NULL);

			/*
			 * 若 sub_qry 是集合操作，无法附加任何限定条件  
			 * 因为规划器注意不到它们。这也许将来能   
			 * 放宽，但目前不妨立即拒绝这种规则。           
			 *                         
			 */
			if (sub_qry->setOperations != NULL && *whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			/*
			 * 检查动作中 OLD/NEW 和 qual 的使用
			 */
			has_old =
				rangeTableEntry_used((Node *) sub_qry, PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_OLD_VARNO, 0);
			has_new =
				rangeTableEntry_used((Node *) sub_qry, PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used(*whereClause, PRS2_NEW_VARNO, 0);

			switch (stmt->event)
			{
				case CMD_SELECT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use OLD")));
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON SELECT rule cannot use NEW")));
					break;
				case CMD_UPDATE:
					/* 两者皆可*/
					break;
				case CMD_INSERT:
					if (has_old)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON INSERT rule cannot use OLD")));
					break;
				case CMD_DELETE:
					if (has_new)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("ON DELETE rule cannot use NEW")));
					break;
				default:
					elog(ERROR, "unrecognized event type: %d",
						 (int) stmt->event);
					break;
			}

			/*
			 * WITH 查询中不允许出现 OLD/NEW，因为对 WITH 而 
			 * 言属于 WITH 的外层引用，这是被禁止的        
			 * 但分析查询时它们已在外层范围表中，所以 
			 * 必须进行检查。                   
			 *
			 * 注意在 INSERT...SELECT 情形下，需要检查           
			 * top_subqry 与 sub_qry 的 CTE 列表。 
			 *
			 * 注意我们并不深入查询体去寻找嵌套子           
			 * SELECT 中的 WITH。位于深处的 WITH 可以       
			 * 合法地引用 OLD/NEW，因为那将是        
			 * 间接关联的外层引用。      
			 */
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_OLD_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_OLD_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to OLD within WITH query")));
			if (rangeTableEntry_used((Node *) top_subqry->cteList,
									 PRS2_NEW_VARNO, 0) ||
				rangeTableEntry_used((Node *) sub_qry->cteList,
									 PRS2_NEW_VARNO, 0))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot refer to NEW within WITH query")));

			/*
			 * 为效率起见，只有 OLD 真的被语句或限定条件 
			 * 引用时，才把 OLD 加入规则动作的连接树。    
			 *
			 * 对 INSERT 而言，NEW 并不是真关系（只是对将    
			 * 插入的元组的引用），绝不应加入连接树。 
			 *          
			 *
			 * 对 UPDATE，我们把 NEW 视为对 OLD 的另一种         
			 * 引用，因为它代表对现有关系*变换后*元组的  
			 * 引用。把 NEW 单独登记进连接树是错误的， 
			 * 因为那会导致被更新关系被连接两次。        
			 * 如果只提到 NEW 而没提到 OLD，连接树中却不     
			 * 建立条目也是错误的。                        
			 */
			if (has_old || (has_new && stmt->event == CMD_UPDATE))
			{
				RangeTblRef *rtr;

				/*
				 * 若 sub_qry 是集合操作，摆弄它的连接树毫无  
				 * 用处，因为连接树是假的。（此前的           
				 * 检查本应排除这种情况。）        
				 */
				if (sub_qry->setOperations != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
				/* 硬把 OLD 塞进已构建好的 FROM 子句       */
				rtr = makeNode(RangeTblRef);
				rtr->rtindex = oldnsitem->p_rtindex;
				sub_qry->jointree->fromlist =
					lappend(sub_qry->jointree->fromlist, rtr);
			}

			newactions = lappend(newactions, top_subqry);

			free_parsestate(sub_pstate);
		}

		*actions = newactions;
	}

	free_parsestate(pstate);

	/* 关闭关系，但保留排他锁           */
	table_close(rel, NoLock);
}


/*
 * transformAlterTableStmt -
 *		ALTER TABLE 的解析分析   
 *
 * 返回转换后的 AlterTableStmt。在转换后的语句之前和      
 * 之后还可能有需要执行的其他动作，它们以工具命令    
 * 解析树列表的形式在 *beforeStmts 和 *afterStmts 中返回。  
 *
 * 为避免竞态条件，本函数确定目标关系时只能依赖传入
 * 的 relid（而不能依赖 stmt->relation）。                       
 *          
 */
AlterTableStmt *
transformAlterTableStmt(Oid relid, AlterTableStmt *stmt,
						const char *queryString,
						List **beforeStmts, List **afterStmts)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;
	ParseNamespaceItem *nsitem;

	/* 调用方负责锁定该关系                 */
	rel = relation_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	/* 设置 pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* 设置 CreateStmtContext */
	cxt.pstate = pstate;
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		cxt.stmtType = "ALTER FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "ALTER TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = rel;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.columns = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	cxt.partbound = NULL;
	cxt.ofType = false;

	/*
	 * 转换需要处理的 ALTER 子命令（多数不需要）。它们  
	 * 多半复用 CREATE TABLE     
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);

					transformColumnDefinition(&cxt, def);

					/*
					 * 如果该列有非空默认值，就无法跳过   
					 * 外键校验。            
					 */
					if (def->raw_default != NULL)
						skipValidation = false;

					/*
					 * 所有约束均以其他方式处理。删除原列表 
					 *              
					 */
					def->constraints = NIL;

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddConstraint:

				/*
				 * 原始 AddConstraint 命令节点不会进入 newcmds    
				 */
				if (IsA(cmd->def, Constraint))
				{
					transformTableConstraint(&cxt, (Constraint *) cmd->def);
					if (((Constraint *) cmd->def)->contype == CONSTR_FOREIGN)
						skipValidation = false;
				}
				else
					elog(ERROR, "unrecognized node type: %d",
						 (int) nodeTag(cmd->def));
				break;

			case AT_AlterColumnType:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);
					AttrNumber	attnum;

					/*
					 * 若为 ALTER COLUMN TYPE 指定了 USING            
					 * 子句则转换。
					 */
					if (def->raw_default)
					{
						def->cooked_default =
							transformExpr(pstate, def->raw_default,
										  EXPR_KIND_ALTER_COL_TRANSFORM);
					}

					/*
					 * 对于标识列，创建 ALTER SEQUENCE 命令       
					 * 以修改该序列的数据类型。 
					 */
					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					if (attnum > 0 &&
						TupleDescAttr(tupdesc, attnum - 1)->attidentity)
					{
						Oid			seq_relid = getIdentitySequence(relid, attnum, false);
						Oid			typeOid = typenameTypeId(pstate, def->typeName);
						AlterSeqStmt *altseqstmt = makeNode(AlterSeqStmt);

						altseqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
															get_rel_name(seq_relid),
															-1);
						altseqstmt->options = list_make1(makeDefElem("as", (Node *) makeTypeNameFromOid(typeOid, -1), -1));
						altseqstmt->for_identity = true;
						cxt.blist = lappend(cxt.blist, altseqstmt);
					}

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddIdentity:
				{
					Constraint *def = castNode(Constraint, cmd->def);
					ColumnDef  *newdef = makeNode(ColumnDef);
					AttrNumber	attnum;

					newdef->colname = cmd->name;
					newdef->identity = def->generated_when;
					cmd->def = (Node *) newdef;

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					generateSerialExtraStmts(&cxt, newdef,
											 get_atttype(relid, attnum),
											 def->options, true, true,
											 NULL, NULL);

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_SetIdentity:
				{
					/*
					 * 为标识列的内部序列创建一条            
					 * ALTER SEQUENCE 语句。        
					 */
					ListCell   *lc;
					List	   *newseqopts = NIL;
					List	   *newdef = NIL;
					AttrNumber	attnum;
					Oid			seq_relid;

					/*
					 * 把选项拆分为由 ALTER SEQUENCE 处理的        
					 * 与 ALTER TABLE 本身的。 
					 */
					foreach(lc, castNode(List, cmd->def))
					{
						DefElem    *def = lfirst_node(DefElem, lc);

						if (strcmp(def->defname, "generated") == 0)
							newdef = lappend(newdef, def);
						else
							newseqopts = lappend(newseqopts, def);
					}

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					seq_relid = getIdentitySequence(relid, attnum, true);

					if (seq_relid)
					{
						AlterSeqStmt *seqstmt;

						seqstmt = makeNode(AlterSeqStmt);
						seqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
														 get_rel_name(seq_relid), -1);
						seqstmt->options = newseqopts;
						seqstmt->for_identity = true;
						seqstmt->missing_ok = false;

						cxt.blist = lappend(cxt.blist, seqstmt);
					}

					/*
					 * 若该列不是标识列，就放任                 
					 * 后面的 ALTER TABLE 命令随后报错。（有些  
					 * 情况未覆盖，但我们需要重构           
					 * 序列依赖链接的创建位置，             
					 * 之后才能修复。）      
					 */

					cmd->def = (Node *) newdef;
					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AttachPartition:
			case AT_DetachPartition:
				{
					PartitionCmd *partcmd = (PartitionCmd *) cmd->def;

					transformPartitionCmd(&cxt, partcmd);
					/* 赋予分区边界的转换结果。            */
					partcmd->bound = cxt.partbound;
				}

				newcmds = lappend(newcmds, cmd);
				break;

			default:

				/*
				 * 目前，对于无需转换的子命令类型，        
				 * 我们本不该走到这里；但若真到了，           
				 * 照原样输出。  
				 */
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * 把 cxt.alist 中已有的内容转移到 save_alist，                
	 * 使其与 transformIndexConstraints 的输出分开。   
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* 对约束进行后处理*/
	transformIndexConstraints(&cxt);
	transformFKConstraints(&cxt, skipValidation, true);
	transformCheckConstraints(&cxt, false);

	/*
	 * 把所有创建索引的命令都推入 ALTER 中，                
	 * 以便 tablecmds.c 能妥善安排它们。注意 tablecmds.c       
	 * 假定附在 AT_AddIndex 或 AT_AddIndexConstraint 子命令上  
	 * 的 IndexStmt 已经过 transformIndexStmt 处理。   
	 */
	foreach(l, cxt.alist)
	{
		Node	   *istmt = (Node *) lfirst(l);

		/*
		 * 我们假定 cxt.alist 中只包含 IndexStmts，                  
		 * 以及可能由主键约束生成的                          
		 * ALTER TABLE SET NOT NULL 语句；其子命令直接吸收。  
		 */
		if (IsA(istmt, IndexStmt))
		{
			IndexStmt  *idxstmt = (IndexStmt *) istmt;

			idxstmt = transformIndexStmt(relid, idxstmt, queryString);
			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
			newcmd->def = (Node *) idxstmt;
			newcmds = lappend(newcmds, newcmd);
		}
		else if (IsA(istmt, AlterTableStmt))
		{
			AlterTableStmt *alterstmt = (AlterTableStmt *) istmt;

			newcmds = list_concat(newcmds, alterstmt->cmds);
		}
		else
			elog(ERROR, "unexpected stmt type %d", (int) nodeTag(istmt));
	}
	cxt.alist = NIL;

	/* 把 CHECK 或 FK 约束追加到命令列表。         */
	foreach(l, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}
	foreach(l, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}

	/* 关闭 rel*/
	relation_close(rel, NoLock);

	/*
	 * 输出结果。
	 */
	stmt->cmds = newcmds;

	*beforeStmts = cxt.blist;
	*afterStmts = list_concat(cxt.alist, save_alist);

	return stmt;
}


/*
 * 预处理列约束子句列表，             
 * 把约束属性附加到其主约束节点上，                 
 * 并检测不一致或放错位置的约束属性。     
 *
 * 注意：目前只有 FOREIGN KEY、UNIQUE、                          
 * EXCLUSION 和 PRIMARY KEY 约束支持属性，                     
 * 但其他类型将来也应支持。 
 */
static void
transformConstraintAttrs(CreateStmtContext *cxt, List *constraintList)
{
	Constraint *lastprimarycon = NULL;
	bool		saw_deferrability = false;
	bool		saw_initially = false;
	ListCell   *clist;

#define SUPPORTS_ATTRS(node)				\
	((node) != NULL &&						\
	 ((node)->contype == CONSTR_PRIMARY ||	\
	  (node)->contype == CONSTR_UNIQUE ||	\
	  (node)->contype == CONSTR_EXCLUSION || \
	  (node)->contype == CONSTR_FOREIGN))

	foreach(clist, constraintList)
	{
		Constraint *con = (Constraint *) lfirst(clist);

		if (!IsA(con, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(con));
		switch (con->contype)
		{
			case CONSTR_ATTR_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = true;
				break;

			case CONSTR_ATTR_NOT_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced NOT DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = false;
				if (saw_initially &&
					lastprimarycon->initdeferred)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_DEFERRED:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY DEFERRED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = true;

				/*
				 * 若只出现 INITIALLY DEFERRED，视为 DEFERRABLE  
				 */
				if (!saw_deferrability)
					lastprimarycon->deferrable = true;
				else if (!lastprimarycon->deferrable)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_IMMEDIATE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY IMMEDIATE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = false;
				break;

			default:
				/* 否则它就不是属性。     */
				lastprimarycon = con;
				/* 为新主节点重置标志位。*/
				saw_deferrability = false;
				saw_initially = false;
				break;
		}
	}
}

/*
 * 对列的类型定义做特殊处理。         
 */
static void
transformColumnType(CreateStmtContext *cxt, ColumnDef *column)
{
	/*
	 * 这里我们真正要做的只是验证类型有效，         
	 * 包括可能存在的任何排序规则说明。   
	 */
	Type		ctype = typenameType(cxt->pstate, column->typeName, NULL);

	if (column->collClause)
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(ctype);

		LookupCollation(cxt->pstate,
						column->collClause->collname,
						column->collClause->location);
		/* 若 COLLATE 应用于不可排序的类型则报错。 */
		if (!OidIsValid(typtup->typcollation))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("collations are not supported by type %s",
							format_type_be(typtup->oid)),
					 parser_errposition(cxt->pstate,
										column->collClause->location)));
	}

	ReleaseSysCache(ctype);
}


/*
 * transformCreateSchemaStmtElements -
 *	  分析 CREATE SCHEMA 语句的各个元素。     
 *
 * 把 CREATE SCHEMA 语句中的模式元素列表拆分成        
 * 各条命令，并按无前向引用的顺序放入              
 * 结果列表（例如对列表中后面创建的表             
 * 发出 GRANT）。注意我们判定前向引用所用的             
 * 逻辑目前还相当不完整。                
 *
 * "schemaName" 是创建所列各对象时                                 
 * 所用模式的名称，该名称可由语句中定义的               
 * 模式名或角色说明组合而成。  
 *
 * SQL 也允许约束形成前向引用，因此需要遍历              
 * 各表列，把前向引用挪到其后的 alter-table                  
 * 命令  
 *
 * 结果是一个仍需分析的解析节点列表，                   
 * 但在执行完前面的命令之前无法分析                        
 * 后面的命令，对象间可能相互引用。  
 *
 * 注意：这样做稍微破坏了规矩，因为要修改传入         
 * 结构中的模式名字段。不过就算重来一遍                  
 * 转换结果也一样，所以不妨对输入小幅涂改。            
 *        
 */
List *
transformCreateSchemaStmtElements(List *schemaElts, const char *schemaName)
{
	CreateSchemaStmtContext cxt;
	List	   *result;
	ListCell   *elements;

	cxt.schemaname = schemaName;
	cxt.sequences = NIL;
	cxt.tables = NIL;
	cxt.views = NIL;
	cxt.indexes = NIL;
	cxt.triggers = NIL;
	cxt.grants = NIL;

	/*
	 * 遍历模式元素列表中的每个模式元素。                 
	 * 按类型区分语句并做初步分析。      
	 */
	foreach(elements, schemaElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_CreateSeqStmt:
				{
					CreateSeqStmt *elp = (CreateSeqStmt *) element;

					setSchemaName(cxt.schemaname, &elp->sequence->schemaname);
					cxt.sequences = lappend(cxt.sequences, element);
				}
				break;

			case T_CreateStmt:
				{
					CreateStmt *elp = (CreateStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);

					/*
					 * XXX 待办：处理约束      
					 */
					cxt.tables = lappend(cxt.tables, element);
				}
				break;

			case T_ViewStmt:
				{
					ViewStmt   *elp = (ViewStmt *) element;

					setSchemaName(cxt.schemaname, &elp->view->schemaname);

					/*
					 * XXX 待办：处理视图间的引用       
					 */
					cxt.views = lappend(cxt.views, element);
				}
				break;

			case T_IndexStmt:
				{
					IndexStmt  *elp = (IndexStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.indexes = lappend(cxt.indexes, element);
				}
				break;

			case T_CreateTrigStmt:
				{
					CreateTrigStmt *elp = (CreateTrigStmt *) element;

					setSchemaName(cxt.schemaname, &elp->relation->schemaname);
					cxt.triggers = lappend(cxt.triggers, element);
				}
				break;

			case T_GrantStmt:
				cxt.grants = lappend(cxt.grants, element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
		}
	}

	result = NIL;
	result = list_concat(result, cxt.sequences);
	result = list_concat(result, cxt.tables);
	result = list_concat(result, cxt.views);
	result = list_concat(result, cxt.indexes);
	result = list_concat(result, cxt.triggers);
	result = list_concat(result, cxt.grants);

	return result;
}

/*
 * setSchemaName
 *		设置或检查 CREATE SCHEMA 命令元素中的模式名。     
 */
static void
setSchemaName(const char *context_schema, char **stmt_schema_name)
{
	if (*stmt_schema_name == NULL)
		*stmt_schema_name = unconstify(char *, context_schema);
	else if (strcmp(context_schema, *stmt_schema_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_DEFINITION),
				 errmsg("CREATE specifies a schema (%s) "
						"different from the one being created (%s)",
						*stmt_schema_name, context_schema)));
}

/*
 * transformPartitionCmd
 *		分析 ATTACH/DETACH PARTITION 命令      
 *
 * 对于 ATTACH PARTITION 命令，cxt->partbound                      
 * 为 cmd->bound 的转换值。  
 */
static void
transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd)
{
	Relation	parentRel = cxt->rel;

	switch (parentRel->rd_rel->relkind)
	{
		case RELKIND_PARTITIONED_TABLE:
			/* 转换分区边界（如有）。     */
			Assert(RelationGetPartitionKey(parentRel) != NULL);
			if (cmd->bound != NULL)
				cxt->partbound = transformPartitionBound(cxt->pstate, parentRel,
														 cmd->bound);
			break;
		case RELKIND_PARTITIONED_INDEX:

			/*
			 * 分区索引无法设置分区边界。                      
			 * ALTER INDEX 靠语法阻止，ALTER TABLE 则不行。    
			 */
			if (cmd->bound != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("\"%s\" is not a partitioned table",
								RelationGetRelationName(parentRel))));
			break;
		case RELKIND_RELATION:
			/* 该表必须是分区表。   */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("table \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		case RELKIND_INDEX:
			/* 索引必须是分区索引。*/
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		default:
			/* 解析器不应让这种情况通过。*/
			elog(ERROR, "\"%s\" is not a partitioned table or index",
				 RelationGetRelationName(parentRel));
			break;
	}
}

/*
 * transformPartitionBound
 *
 * 转换一个分区边界规格说明。  
 */
PartitionBoundSpec *
transformPartitionBound(ParseState *pstate, Relation parent,
						PartitionBoundSpec *spec)
{
	PartitionBoundSpec *result_spec;
	PartitionKey key = RelationGetPartitionKey(parent);
	char		strategy = get_partition_strategy(key);
	int			partnatts = get_partition_natts(key);
	List	   *partexprs = get_partition_exprs(key);

	/* 避免涂改输入。     */
	result_spec = copyObject(spec);

	if (spec->is_default)
	{
		/*
		 * 哈希分区不支持默认分区；                              
		 * 它没有用例（因为要创建的分区集合                   
		 * 是完全确定的），而且如果用户不小心             
		 * 陷入就很难退出。    
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("a hash-partitioned table may not have a default partition")));

		/*
		 * 对于默认分区，解析器无从识别分区策略，          
		 * 因此把父分区的策略赋予默认分区边界规格。   
		 *                      
		 */
		result_spec->strategy = strategy;

		return result_spec;
	}

	if (strategy == PARTITION_STRATEGY_HASH)
	{
		if (spec->strategy != PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a hash partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (spec->modulus <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("modulus for hash partition must be an integer value greater than zero")));

		Assert(spec->remainder >= 0);

		if (spec->remainder >= spec->modulus)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("remainder for hash partition must be less than modulus")));
	}
	else if (strategy == PARTITION_STRATEGY_LIST)
	{
		ListCell   *cell;
		char	   *colname;
		Oid			coltype;
		int32		coltypmod;
		Oid			partcollation;

		if (spec->strategy != PARTITION_STRATEGY_LIST)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a list partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		/* 取唯一列的名称，以备需要输出错误。           */
		if (key->partattrs[0] != 0)
			colname = get_attname(RelationGetRelid(parent),
								  key->partattrs[0], false);
		else
			colname = deparse_expression((Node *) linitial(partexprs),
										 deparse_context_for(RelationGetRelationName(parent),
															 RelationGetRelid(parent)),
										 false, false);
		/* 还需类型数据     */
		coltype = get_partition_col_typid(key, 0);
		coltypmod = get_partition_col_typmod(key, 0);
		partcollation = get_partition_col_collation(key, 0);

		result_spec->listdatums = NIL;
		foreach(cell, spec->listdatums)
		{
			Node	   *expr = lfirst(cell);
			Const	   *value;
			ListCell   *cell2;
			bool		duplicate;

			value = transformPartitionBoundValue(pstate, expr,
												 colname, coltype, coltypmod,
												 partcollation);

			/* 若值重复则不加入结果。                   */
			duplicate = false;
			foreach(cell2, result_spec->listdatums)
			{
				Const	   *value2 = lfirst_node(Const, cell2);

				if (equal(value, value2))
				{
					duplicate = true;
					break;
				}
			}
			if (duplicate)
				continue;

			result_spec->listdatums = lappend(result_spec->listdatums,
											  value);
		}
	}
	else if (strategy == PARTITION_STRATEGY_RANGE)
	{
		if (spec->strategy != PARTITION_STRATEGY_RANGE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("invalid bound specification for a range partition"),
					 parser_errposition(pstate, exprLocation((Node *) spec))));

		if (list_length(spec->lowerdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("FROM must specify exactly one value per partitioning column")));
		if (list_length(spec->upperdatums) != partnatts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("TO must specify exactly one value per partitioning column")));

		/*
		 * 把原始解析节点转换为 PartitionRangeDatum 节点，      
		 * 并做必要的验证。 
		 */
		result_spec->lowerdatums =
			transformPartitionRangeBounds(pstate, spec->lowerdatums,
										  parent);
		result_spec->upperdatums =
			transformPartitionRangeBounds(pstate, spec->upperdatums,
										  parent);
	}
	else
		elog(ERROR, "unexpected partition strategy: %d", (int) strategy);

	return result_spec;
}

/*
 * transformPartitionRangeBounds
 *		此函数把范围分区边界的表达式从原始的               
 *		语法表示转换为 PartitionRangeDatum 结构。  
 */
static List *
transformPartitionRangeBounds(ParseState *pstate, List *blist,
							  Relation parent)
{
	List	   *result = NIL;
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *partexprs = get_partition_exprs(key);
	ListCell   *lc;
	int			i,
				j;

	i = j = 0;
	foreach(lc, blist)
	{
		Node	   *expr = lfirst(lc);
		PartitionRangeDatum *prd = NULL;

		/*
		 * 无限范围边界（"minvalue" 与 "maxvalue"）传入时         
		 * 为 ColumnRefs
		 */
		if (IsA(expr, ColumnRef))
		{
			ColumnRef  *cref = (ColumnRef *) expr;
			char	   *cname = NULL;

			/*
			 * 字段名应当是 "minvalue" 或 "maxvalue"             
			 * 之一。  
			 */
			if (list_length(cref->fields) == 1 &&
				IsA(linitial(cref->fields), String))
				cname = strVal(linitial(cref->fields));

			if (cname == NULL)
			{
				/*
				 * ColumnRef 不是想要的单字段名形式。             
				 * 为了让所有分区策略保持一致，           
				 * 任何错误都交给表达式转换报告，          
				 * 而非自己做。 
				 */
			}
			else if (strcmp("minvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MINVALUE;
				prd->value = NULL;
			}
			else if (strcmp("maxvalue", cname) == 0)
			{
				prd = makeNode(PartitionRangeDatum);
				prd->kind = PARTITION_RANGE_DATUM_MAXVALUE;
				prd->value = NULL;
			}
		}

		if (prd == NULL)
		{
			char	   *colname;
			Oid			coltype;
			int32		coltypmod;
			Oid			partcollation;
			Const	   *value;

			/* 取该列的名称，以备需要输出错误。         */
			if (key->partattrs[i] != 0)
				colname = get_attname(RelationGetRelid(parent),
									  key->partattrs[i], false);
			else
			{
				colname = deparse_expression((Node *) list_nth(partexprs, j),
											 deparse_context_for(RelationGetRelationName(parent),
																 RelationGetRelid(parent)),
											 false, false);
				++j;
			}

			/* 还需类型数据     */
			coltype = get_partition_col_typid(key, i);
			coltypmod = get_partition_col_typmod(key, i);
			partcollation = get_partition_col_collation(key, i);

			value = transformPartitionBoundValue(pstate, expr,
												 colname,
												 coltype, coltypmod,
												 partcollation);
			if (value->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot specify NULL in range bound")));
			prd = makeNode(PartitionRangeDatum);
			prd->kind = PARTITION_RANGE_DATUM_VALUE;
			prd->value = (Node *) value;
			++i;
		}

		prd->location = exprLocation(expr);

		result = lappend(result, prd);
	}

	/*
	 * 一旦某列出现 MINVALUE 或 MAXVALUE，其余列                  
	 * 必须相同。  
	 */
	validateInfiniteBounds(pstate, result);

	return result;
}

/*
 * validateInfiniteBounds
 *
 * 检查分区边界中的 MAXVALUE 或 MINVALUE 规格说明            
 * 其后只能跟同类说明。    
 */
static void
validateInfiniteBounds(ParseState *pstate, List *blist)
{
	ListCell   *lc;
	PartitionRangeDatumKind kind = PARTITION_RANGE_DATUM_VALUE;

	foreach(lc, blist)
	{
		PartitionRangeDatum *prd = lfirst_node(PartitionRangeDatum, lc);

		if (kind == prd->kind)
			continue;

		switch (kind)
		{
			case PARTITION_RANGE_DATUM_VALUE:
				kind = prd->kind;
				break;

			case PARTITION_RANGE_DATUM_MAXVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MAXVALUE must also be MAXVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;

			case PARTITION_RANGE_DATUM_MINVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MINVALUE must also be MINVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
				break;
		}
	}
}

/*
 * 转换分区边界规格中的一个条目，产出一个常量。  
 */
static Const *
transformPartitionBoundValue(ParseState *pstate, Node *val,
							 const char *colName, Oid colType, int32 colTypmod,
							 Oid partCollation)
{
	Node	   *value;

	/* 转换原始解析树。*/
	value = transformExpr(pstate, val, EXPR_KIND_PARTITION_BOUND);

	/*
	 * transformExpr() 本应已根据分区边界表达式的        
	 * EXPR_KIND_ 拒绝列引用、子查询、聚合、              
	 * 窗口函数和 SRF。                     
	 */
	Assert(!contain_var_clause(value));

	/*
	 * 强制转换到正确类型。这可能会在表达式                 
	 * 之上再叠加一个显式转换步骤，该步骤必须           
	 * 求值后再返回给调用者。  
	 */
	value = coerce_to_target_type(pstate,
								  value, exprType(value),
								  colType,
								  colTypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 parser_errposition(pstate, exprLocation(val))));

	/*
	 * 如需要则对表达式求值，并把分区键的数据类型       
	 * 和排序规则赋给所得 Const 节点。    
	 */
	if (!IsA(value, Const))
	{
		assign_expr_collations(pstate, value);
		value = (Node *) expression_planner((Expr *) value);
		value = (Node *) evaluate_expr((Expr *) value, colType, colTypmod,
									   partCollation);
		if (!IsA(value, Const))
			elog(ERROR, "could not evaluate partition bound expression");
	}
	else
	{
		/*
		 * 如果表达式已经是 Const（这很常见），               
		 * 就可以跳过上面相当昂贵的步骤。但仍须插入       
		 * 正确排序规则，因 coerce_to_target_type 不管此事。 
		 *      
		 */
		((Const *) value)->constcollid = partCollation;
	}

	/*
	 * 把原始表达式的解析位置附加到 Const 上，          
	 * 这样之后与此分区边界相关的任何错误              
	 * 都报出它。 
	 */
	((Const *) value)->location = exprLocation(val);

	return (Const *) value;
}
