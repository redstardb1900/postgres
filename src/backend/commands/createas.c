/*-------------------------------------------------------------------------
 *
 * createas.c
 *	  CREATE TABLE ... AS（又称 SELECT INTO）的执行。
 *	  由于 CREATE MATERIALIZED VIEW 与它共享语法和大部分行为，
 *	  我们在这里一并实现。
 *
 * 我们把查询的正常输出转移到一种
 * 专门的 DestReceiver 类型来实现这一点。
 *
 * 以前，CTAS 被实现为 SELECT 的变体，这导致了
 * 我们仍试图保留的各种遗留行为，特别是
 * 我们必须在 QueryCompletion 中返回已处理元组数。（不过，
 * 对于 CTAS ... WITH NO DATA 我们已不再这样做。）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/createas.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/createas.h"
#include "commands/matview.h"
#include "commands/prepare.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteHandler.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"

typedef struct
{
	DestReceiver pub;			/* 公开的函数指针 */
	IntoClause *into;			/* 目标关系规格 */
	/* 这些字段由 intorel_startup 填充： */
	Relation	rel;			/* 要写入的关系 */
	ObjectAddress reladdr;		/* rel 的地址，供 ExecCreateTableAs 使用 */
	CommandId	output_cid;		/* 插入输出元组的 cmin */
	int			ti_options;		/* table_tuple_insert 的性能选项 */
	BulkInsertState bistate;	/* 批量插入状态 */
} DR_intorel;

/* 用于 CTAS 定义创建的实用函数 */
static ObjectAddress create_ctas_internal(List *attrList, IntoClause *into);
static ObjectAddress create_ctas_nodata(List *tlist, IntoClause *into);

/* 用于收集数据的 DestReceiver 例程 */
static void intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool intorel_receive(TupleTableSlot *slot, DestReceiver *self);
static void intorel_shutdown(DestReceiver *self);
static void intorel_destroy(DestReceiver *self);


/*
 * create_ctas_internal
 *
 * 用于为经由 CREATE TABLE AS 或物化视图创建的关系
 * 创建定义的内部实用函数。调用方需要
 * 提供属性列表（ColumnDef 节点）。
 */
static ObjectAddress
create_ctas_internal(List *attrList, IntoClause *into)
{
	CreateStmt *create = makeNode(CreateStmt);
	bool		is_matview;
	char		relkind;
	Datum		toast_options;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	ObjectAddress intoRelationAddr;

	/* 此代码同时支持 CREATE TABLE AS 和 CREATE MATERIALIZED VIEW */
	is_matview = (into->viewQuery != NULL);
	relkind = is_matview ? RELKIND_MATVIEW : RELKIND_RELATION;

	/*
	 * 通过伪造一棵 CREATE TABLE 解析树并把它传给
	 * DefineRelation 来创建目标关系。
	 */
	create->relation = into->rel;
	create->tableElts = attrList;
	create->inhRelations = NIL;
	create->ofTypename = NULL;
	create->constraints = NIL;
	create->options = into->options;
	create->oncommit = into->onCommit;
	create->tablespacename = into->tableSpaceName;
	create->if_not_exists = false;
	create->accessMethod = into->accessMethod;

	/*
	 * 创建关系。（若已存在视图会报错，所以若
	 * "replace" 为 false，我们不需要更多代码来抱怨。）
	 */
	intoRelationAddr = DefineRelation(create, relkind, InvalidOid, NULL, NULL);

	/*
	 * 如有必要，为目标表创建 TOAST 表。注意
	 * NewRelationCreateToastTable 以 CommandCounterIncrement() 结束，
	 * 因此 TOAST 表将对插入可见。
	 */
	CommandCounterIncrement();

	/* 解析并校验 toast 表的 reloptions */
	toast_options = transformRelOptions((Datum) 0,
										create->options,
										"toast",
										validnsps,
										true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	NewRelationCreateToastTable(intoRelationAddr.objectId, toast_options);

	/* 创建物化视图的 "view" 部分。 */
	if (is_matview)
	{
		/* StoreViewQuery 会涂改树，因此先复制一份 */
		Query	   *query = (Query *) copyObject(into->viewQuery);

		StoreViewQuery(intoRelationAddr.objectId, query, false);
		CommandCounterIncrement();
	}

	return intoRelationAddr;
}


/*
 * create_ctas_nodata
 *
 * 使用 WITH NO DATA 时，从 SELECT 或视图定义的
 * targetlist 出发，创建 CTAS 或物化视图。
 */
static ObjectAddress
create_ctas_nodata(List *tlist, IntoClause *into)
{
	List	   *attrList;
	ListCell   *t,
			   *lc;

	/*
	 * 从 tlist 的非垃圾元素构建 ColumnDefs 列表。
	 * 若在 CREATE TABLE AS 中指定了列名列表，就覆盖查询中的
	 * 列名。（列名过少没问题，过多不行。）
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *col;
			char	   *colname;

			if (lc)
			{
				colname = strVal(lfirst(lc));
				lc = lnext(into->colNames, lc);
			}
			else
				colname = tle->resname;

			col = makeColumnDef(colname,
								exprType((Node *) tle->expr),
								exprTypmod((Node *) tle->expr),
								exprCollation((Node *) tle->expr));

			/*
			 * 该列可能属于可排序类型，但排序规则无法
			 * 解析，所以要复查。（我们必须在这里检查，因为
			 * DefineRelation 会采用该类型的默认排序规则
			 * 而不是报错。）
			 */
			if (!OidIsValid(col->collOid) &&
				type_is_collatable(col->typeName->typeOid))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("no collation was derived for column \"%s\" with collatable type %s",
								col->colname,
								format_type_be(col->typeName->typeOid)),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));

			attrList = lappend(attrList, col);
		}
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/* 使用 ColumnDef 列表创建关系定义 */
	return create_ctas_internal(attrList, into);
}


/*
 * ExecCreateTableAs -- 执行 CREATE TABLE AS 命令
 */
ObjectAddress
ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
				  ParamListInfo params, QueryEnvironment *queryEnv,
				  QueryCompletion *qc)
{
	Query	   *query = castNode(Query, stmt->query);
	IntoClause *into = stmt->into;
	bool		is_matview = (into->viewQuery != NULL);
	DestReceiver *dest;
	Oid			save_userid = InvalidOid;
	int			save_sec_context = 0;
	int			save_nestlevel = 0;
	ObjectAddress address;
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;

	/* 检查关系是否存在 */
	if (CreateTableAsRelExists(stmt))
		return InvalidObjectAddress;

	/*
	 * 创建元组接收器对象并插入它需要的信息
	 */
	dest = CreateIntoRelDestReceiver(into);

	/*
	 * 内含的 Query 可能是 SELECT，也可能是 EXECUTE 工具命令。
	 * 若是后者，我们直接把它转交给 ExecuteQuery。
	 */
	if (query->commandType == CMD_UTILITY &&
		IsA(query->utilityStmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = castNode(ExecuteStmt, query->utilityStmt);

		Assert(!is_matview);	/* 语法已排除 */
		ExecuteQuery(pstate, estmt, into, params, dest, qc);

		/* 获取 intorel_startup 为我们保存的对象地址 */
		address = ((DR_intorel *) dest)->reladdr;

		return address;
	}
	Assert(query->commandType == CMD_SELECT);

	/*
	 * 对于物化视图，锁定安全受限的操作，
	 * 并安排让 GUC 变量更改仅在本命令内
	 * 生效。这对安全并非必要，但能让行为
	 * 与 REFRESH MATERIALIZED VIEW 类似。否则，
	 * 有人可能创建出无法刷新的物化视图。
	 */
	if (is_matview)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(save_userid,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}

	if (into->skipData)
	{
		/*
		 * 若指定了 WITH NO DATA，就不经过重写器、
		 * 规划器和执行器。直接用类似于 CREATE VIEW 的
		 * 代码路径定义关系。这避免了在所有依赖建立
		 * 之前运行规划器而导致的转储/恢复问题。
		 */
		address = create_ctas_nodata(query->targetList, into);
	}
	else
	{
		/*
		 * 解析分析已完成，但我们仍须运行规则重写器。
		 * 我们不调用 AcquireRewriteLocks：假定查询要么直接
		 * 来自解析器，要么已由 plancache.c 获取了
		 * 合适的锁。
		 */
		rewritten = QueryRewrite(query);

		/* SELECT 重写后应该不多不少恰为一个 SELECT 查询 */
		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result for %s",
				 is_matview ? "CREATE MATERIALIZED VIEW" :
				 "CREATE TABLE AS SELECT");
		query = linitial_node(Query, rewritten);
		Assert(query->commandType == CMD_SELECT);

		/* 规划查询 */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, params);

		/*
		 * 使用带有更新后的命令 ID 的快照，确保本查询
		 * 能看到任何先前执行的查询的结果。（只有当
		 * 规划器执行了某个号称稳定的函数、而它改变了
		 * 数据库内容时这才重要，但为了与 EXPLAIN 代码
		 * 路径保持一致，我们还是照做。）
		 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* 创建 QueryDesc，把输出重定向到我们的元组接收器 */
		queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
									GetActiveSnapshot(), InvalidSnapshot,
									dest, params, queryEnv, 0);

		/* 调用 ExecutorStart 为执行准备计划 */
		ExecutorStart(queryDesc, GetIntoRelEFlags(into));

		/* 把计划执行到底 */
		ExecutorRun(queryDesc, ForwardScanDirection, 0, true);

		/* 若给了我们要填充的 qc，就保存行数 */
		if (qc)
			SetQueryCompletion(qc, CMDTAG_SELECT, queryDesc->estate->es_processed);

		/* 获取 intorel_startup 为我们保存的对象地址 */
		address = ((DR_intorel *) dest)->reladdr;

		/* 然后清理 */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);

		FreeQueryDesc(queryDesc);

		PopActiveSnapshot();
	}

	if (is_matview)
	{
		/* 回滚任何 GUC 更改 */
		AtEOXact_GUC(false, save_nestlevel);

		/* 恢复 userid 和安全上下文 */
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}

	return address;
}

/*
 * GetIntoRelEFlags --- 计算 CREATE TABLE AS 所需的执行器标志
 *
 * 此函数被导出，因为 EXPLAIN 和 PREPARE 也需要它。
 * （注意：那些调用方仍需显式处理 skipData 标志；
 * 由于它们用不同的方法抑制执行，似乎不值得
 * 尝试把那部分封装起来。）
 */
int
GetIntoRelEFlags(IntoClause *intoClause)
{
	int			flags = 0;

	if (intoClause->skipData)
		flags |= EXEC_FLAG_WITH_NO_DATA;

	return flags;
}

/*
 * CreateTableAsRelExists --- 为 CreateTableAsStmt 检查关系是否存在
 *
 * 实用包装函数：检查本 CreateTableAsStmt 查询中
 * 待创建的关系是否已存在。若关系存在返回
 * true，否则返回 false。
 */
bool
CreateTableAsRelExists(CreateTableAsStmt *ctas)
{
	Oid			nspid;
	Oid			oldrelid;
	ObjectAddress address;
	IntoClause *into = ctas->into;

	nspid = RangeVarGetCreationNamespace(into->rel);

	oldrelid = get_relname_relid(into->rel->relname, nspid);
	if (OidIsValid(oldrelid))
	{
		if (!ctas->if_not_exists)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists",
							into->rel->relname)));

		/*
		 * 关系已存在，且指定了 IF NOT EXISTS。
		 *
		 * 若我们处于扩展脚本中，坚持要求先前存在的
		 * 对象是该扩展的成员，以避免安全风险。
		 */
		ObjectAddressSet(address, RelationRelationId, oldrelid);
		checkMembershipInCurrentExtension(&address);

		/* 可以跳过 */
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						into->rel->relname)));
		return true;
	}

	/* 关系不存在，可以创建 */
	return false;
}

/*
 * CreateIntoRelDestReceiver -- 创建合适的 DestReceiver 对象
 *
 * 若从 CreateDestReceiver() 调用，intoClause 将为 NULL，
 * 此时它必须稍后再提供。不过，允许其他调用方
 * 立即填好 self->into 也很方便。
 */
DestReceiver *
CreateIntoRelDestReceiver(IntoClause *intoClause)
{
	DR_intorel *self = (DR_intorel *) palloc0(sizeof(DR_intorel));

	self->pub.receiveSlot = intorel_receive;
	self->pub.rStartup = intorel_startup;
	self->pub.rShutdown = intorel_shutdown;
	self->pub.rDestroy = intorel_destroy;
	self->pub.mydest = DestIntoRel;
	self->into = intoClause;
	/* 其他私有字段将在 intorel_startup 期间设置 */

	return (DestReceiver *) self;
}

/*
 * intorel_startup --- 执行器启动
 */
static void
intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;
	bool		is_matview;
	List	   *attrList;
	ObjectAddress intoRelationAddr;
	Relation	intoRelationDesc;
	ListCell   *lc;
	int			attnum;

	Assert(into != NULL);		/* 否则就是有人忘了设置它 */

	/* 此代码同时支持 CREATE TABLE AS 和 CREATE MATERIALIZED VIEW */
	is_matview = (into->viewQuery != NULL);

	/*
	 * 使用"预制"的类型和排序规则信息构建列定义。
	 * 若在 CREATE TABLE AS 中指定了列名列表，就覆盖
	 * 从查询推导出的列名。（列名过少没问题，
	 * 过多不行。）
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	for (attnum = 0; attnum < typeinfo->natts; attnum++)
	{
		Form_pg_attribute attribute = TupleDescAttr(typeinfo, attnum);
		ColumnDef  *col;
		char	   *colname;

		if (lc)
		{
			colname = strVal(lfirst(lc));
			lc = lnext(into->colNames, lc);
		}
		else
			colname = NameStr(attribute->attname);

		col = makeColumnDef(colname,
							attribute->atttypid,
							attribute->atttypmod,
							attribute->attcollation);

		/*
		 * 该列可能属于可排序类型，但排序规则无法
		 * 解析，所以要复查。（我们必须在这里检查，因为
		 * DefineRelation 会采用该类型的默认排序规则
		 * 而不是报错。）
		 */
		if (!OidIsValid(col->collOid) &&
			type_is_collatable(col->typeName->typeOid))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_COLLATION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							col->colname,
							format_type_be(col->typeName->typeOid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));

		attrList = lappend(attrList, col);
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/*
	 * 实际创建目标表
	 */
	intoRelationAddr = create_ctas_internal(attrList, into);

	/*
	 * 最后我们可以打开目标表
	 */
	intoRelationDesc = table_open(intoRelationAddr.objectId, AccessExclusiveLock);

	/*
	 * 确保构造出来的表没有启用 RLS。
	 *
	 * 若用户请求了无效的东西，check_enable_rls() 会自己
	 * ereport(ERROR)，否则若应在此启用 RLS 就返回
	 * RLS_ENABLED。我们目前其实不支持这一点，
	 * 所以若发生那种情况就抛自己的 ereport(ERROR)。
	 */
	if (check_enable_rls(intoRelationAddr.objectId, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("policies not yet implemented for this command")));

	/*
	 * 若是物化视图且我们将填充它，就暂时把目标标记
	 * 为已填充；否则无需更改。
	 */
	if (is_matview && !into->skipData)
		SetMatViewPopulatedState(intoRelationDesc, true);

	/*
	 * 填充 myState 的私有字段，供后续例程使用
	 */
	myState->rel = intoRelationDesc;
	myState->reladdr = intoRelationAddr;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM;

	/*
	 * 若指定了 WITH NO DATA，就没有必要为批量插入
	 * 设置状态，因为没有元组要插入。
	 */
	if (!into->skipData)
		myState->bistate = GetBulkInsertState();
	else
		myState->bistate = NULL;

	/*
	 * 有效的 smgr_targblock 意味着已有东西写过该关系。
	 * 这也许无害，但本函数并未为此做准备。
	 */
	Assert(RelationGetTargetBlock(intoRelationDesc) == InvalidBlockNumber);
}

/*
 * intorel_receive --- 接收一个元组
 */
static bool
intorel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;

	/* 若指定了 WITH NO DATA，就没有要插入的内容。 */
	if (!myState->into->skipData)
	{
		/*
		 * 注意，输入槽的类型可能不同于目标关系的类型。
		 * table_tuple_insert() 支持这一点，但比用正确的槽
		 * 插入效率略低——不过另一种做法是复制进类型正确
		 * 的槽，那也不便宜。这同样不允许访问每个 AM 的
		 * 数据（比如元组的 xmin），但既然我们在这里
		 * 不那样做……
		 */
		table_tuple_insert(myState->rel,
						   slot,
						   myState->output_cid,
						   myState->ti_options,
						   myState->bistate);
	}

	/* 我们知道这是新建的关系，因此没有索引 */

	return true;
}

/*
 * intorel_shutdown --- 执行器结束
 */
static void
intorel_shutdown(DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;

	if (!into->skipData)
	{
		FreeBulkInsertState(myState->bistate);
		table_finish_bulk_insert(myState->rel, myState->ti_options);
	}

	/* 关闭 rel，但把锁保留到提交 */
	table_close(myState->rel, NoLock);
	myState->rel = NULL;
}

/*
 * intorel_destroy --- 释放 DestReceiver 对象
 */
static void
intorel_destroy(DestReceiver *self)
{
	pfree(self);
}
