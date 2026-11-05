/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attmap.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/policy.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/usercontext.h"

/*
 * ON COMMIT action list
 */
typedef struct OnCommitItem
{
	Oid			relid;			/* 关系的 relid */
	OnCommitAction oncommit;	/* 事务结束时执行的操作 */

	/*
	 * If this entry was created 期间 the current 事务, creating_subid is the ID of
	 * the creating 子事务; if created in a prior 事务, creating_subid is zero. If
	 * deleted 期间 the current 事务, deleting_subid is the ID of the deleting 子事务;
	 * if no deletion request is pending, deleting_subid is zero.
	 *
	 */
	SubTransactionId creating_subid;
	SubTransactionId deleting_subid;
} OnCommitItem;

static List *on_commits = NIL;


/*
 * State information for ALTER TABLE The pending-work queue for an ALTER TABLE
 * is a List of AlteredTableInfo structs, one for each table modified by the
 * operation (the named table plus any child tables that are affected). We
 * save lists of 子命令 to apply to this table (possibly modified by 解析转换 steps);
 * these lists 将被 executed in Phase 2. If a Phase 3 step 是必需的, necessary
 * information is stored in the 约束 and newvals lists. Phase 2 is divided into
 * multiple passes; 子命令 are executed in a pass determined by 子命令 type.
 *
 *
 *
 *
 */

#define AT_PASS_UNSET			-1	/* UNSET 将导致 ERROR */
#define AT_PASS_DROP			0	/* DROP（所有形式） */
#define AT_PASS_ALTER_TYPE		1	/* ALTER COLUMN TYPE */
#define AT_PASS_OLD_INDEX		2	/* 重新添加已有索引 */
#define AT_PASS_OLD_CONSTR		3	/* 重新添加已有约束 */
/* 此处可支持 RENAME COLUMN 遍，但当前未使用 */
#define AT_PASS_ADD_COL			4	/* 添加列 */
#define AT_PASS_ADD_CONSTR		5	/* ADD 约束（初始检查） */
#define AT_PASS_COL_ATTRS		6	/* 设置列属性，如 NOT NULL */
#define AT_PASS_ADD_INDEXCONSTR	7	/* ADD 基于索引的约束 */
#define AT_PASS_ADD_INDEX		8	/* 添加索引 */
#define AT_PASS_ADD_OTHERCONSTR	9	/* ADD 其他约束、默认值 */
#define AT_PASS_MISC			10	/* 其他事项 */
#define AT_NUM_PASSES			11

typedef struct AlteredTableInfo
{
	/* 工作开始前保存的信息： */
	Oid			relid;			/* 要操作的关系 */
	char		relkind;		/* 其 relkind */
	TupleDesc	oldDesc;		/* 修改前的元组描述符 */

	/*
	 * Phase 2 期间临时设置，通常设置为 NULL。 
	 *
	 * ATRewriteCatalogs 在开始时设置它，在 ATExecCmd 返回时    
	 * 关闭。ATExecCmd 的子例程可利用它跨事务边界         
	 * 进行关闭/重新打开。               
	 */
	Relation	rel;

	/* 阶段 1 为阶段 2 保存的信息： */
	List	   *subcmds[AT_NUM_PASSES]; /* AlterTableCmd 列表 */
	/* 阶段 1/2 为阶段 3 保存的信息： */
	List	   *constraints;	/* NewConstraint 列表 */
	List	   *newvals;		/* NewColumnValue 列表 */
	List	   *afterStmts;		/* 工具命令解析树列表 */
	bool		verify_new_notnull; /* 若需重新检查 NOT NULL 则为真 */
	int			rewrite;		/* 强制重写的原因（如有） */
	Oid			newAccessMethod;	/* 新访问方法；0 表示无变化 */
	Oid			newTableSpace;	/* 新表空间；0 表示无变化 */
	bool		chgPersistence; /* 若使用了 SET LOGGED/UNLOGGED 则为真 */
	char		newrelpersistence;	/* 若上述为真 */
	Expr	   *partition_constraint;	/* 用于附加分区验证 */
	/* 如果因其他 attach/detach 而验证默认值，则为真 */
	bool		validate_default;
	/* 完成 ALTER TYPE 操作后需重建的对象 */
	List	   *changedConstraintOids;	/* 需重建约束的 OID */
	List	   *changedConstraintDefs;	/* 相同的字符串定义 */
	List	   *changedIndexOids;	/* 需重建索引的 OID */
	List	   *changedIndexDefs;	/* 相同的字符串定义 */
	char	   *replicaIdentityIndex;	/* 要重置为 REPLICA IDENTITY 的索引 */
	char	   *clusterOnIndex; /* 用于 CLUSTER 的索引 */
	List	   *changedStatisticsOids;	/* 需重建统计信息的 OID */
	List	   *changedStatisticsDefs;	/* 相同的字符串定义 */
} AlteredTableInfo;

/* 描述阶段 3 扫描中需检查的一个新约束的结构体 */
/* 注意：新的 NOT NULL 约束在其他地方处理 */
typedef struct NewConstraint
{
	char	   *name;			/* 约束名称，若无则为 NULL */
	ConstrType	contype;		/* CHECK 或 FOREIGN */
	Oid			refrelid;		/* 若是 FOREIGN 则为主键关系 */
	Oid			refindid;		/* 若是 FOREIGN 则为主键索引的 OID */
	Oid			conid;			/* 若是 FOREIGN 则为 pg_constraint 条目的 OID */
	Node	   *qual;			/* Check 表达式或 CONSTR_FOREIGN 约束 */
	ExprState  *qualstate;		/* CHECK 表达式的执行状态 */
} NewConstraint;

/*
 * Struct describing one new column value that 需要被 computed 期间 Phase 3 copy
 * (this 可能 either a new column with a non-null default, or a column that
 * we're changing the type of). Columns 没有 such an entry are just copied from
 * the old table 期间 ATRewriteTable. 请注意 the expr is an expression over *old*
 * table values, except when is_generated is true; then it is an expression
 * over columns of the *new* 元组.
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* 哪个列 */
	Expr	   *expr;			/* 要计算的表达式 */
	ExprState  *exprstate;		/* 执行状态 */
	bool		is_generated;	/* 是否为 GENERATED 表达式？ */
} NewColumnValue;

/*
 * Error-reporting support for RemoveRelations
 */
struct dropmsgstrings
{
	char		kind;
	int			nonexistent_code;
	const char *nonexistent_msg;
	const char *skipping_msg;
	const char *nota_msg;
	const char *drophint_msg;
};

static const struct dropmsgstrings dropmsgstringarray[] = {
	{RELKIND_RELATION,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a table"),
	gettext_noop("Use DROP TABLE to remove a table.")},
	{RELKIND_SEQUENCE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("sequence \"%s\" does not exist"),
		gettext_noop("sequence \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a sequence"),
	gettext_noop("Use DROP SEQUENCE to remove a sequence.")},
	{RELKIND_VIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("view \"%s\" does not exist"),
		gettext_noop("view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a view"),
	gettext_noop("Use DROP VIEW to remove a view.")},
	{RELKIND_MATVIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("materialized view \"%s\" does not exist"),
		gettext_noop("materialized view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a materialized view"),
	gettext_noop("Use DROP MATERIALIZED VIEW to remove a materialized view.")},
	{RELKIND_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{RELKIND_COMPOSITE_TYPE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("type \"%s\" does not exist"),
		gettext_noop("type \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a type"),
	gettext_noop("Use DROP TYPE to remove a type.")},
	{RELKIND_FOREIGN_TABLE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("foreign table \"%s\" does not exist"),
		gettext_noop("foreign table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a foreign table"),
	gettext_noop("Use DROP FOREIGN TABLE to remove a foreign table.")},
	{RELKIND_PARTITIONED_TABLE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a table"),
	gettext_noop("Use DROP TABLE to remove a table.")},
	{RELKIND_PARTITIONED_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{'\0', 0, NULL, NULL, NULL, NULL}
};

/* RemoveRelations 与 RangeVarCallbackForDropRelation 之间的通信 */
struct DropRelationCallbackState
{
	/* 以下字段由 RemoveRelations 设置： */
	char		expected_relkind;
	LOCKMODE	heap_lockmode;
	/* 以下字段是用于跟踪所持附属锁的状态： */
	Oid			heapOid;
	Oid			partParentOid;
	/* 以下字段由 RangeVarCallbackForDropRelation 回传： */
	char		actual_relkind;
	char		actual_relpersistence;
};

/* ATSimplePermissions 的 ALTER TABLE 目标类型标志 */
#define		ATT_TABLE				0x0001
#define		ATT_VIEW				0x0002
#define		ATT_MATVIEW				0x0004
#define		ATT_INDEX				0x0008
#define		ATT_COMPOSITE_TYPE		0x0010
#define		ATT_FOREIGN_TABLE		0x0020
#define		ATT_PARTITIONED_INDEX	0x0040
#define		ATT_SEQUENCE			0x0080

/*
 * ForeignTruncateInfo
 *
 * 与外部表截断相关的信息。用作哈希表的元素。       
 * 它以服务器 OID 作为查找键，并包含                    
 * 一个按服务器列出的、涉及本次截断的所有外部     
 * 表。     
 */
typedef struct ForeignTruncateInfo
{
	Oid			serverid;
	List	   *rels;
} ForeignTruncateInfo;

/* addFkConstraint() 中的部分或完整外键创建 */
typedef enum addFkConstraintSides
{
	addFkReferencedSide,
	addFkReferencingSide,
	addFkBothSides,
} addFkConstraintSides;

/*
 * Partition tables are expected to be dropped when the 父分区表 gets dropped.
 * Hence for partitioning we use AUTO 依赖. Otherwise, for regular 继承 use NORMAL
 * 依赖.
 */
#define child_dependency_type(child_is_partition)	\
	((child_is_partition) ? DEPENDENCY_AUTO : DEPENDENCY_NORMAL)

static void truncate_check_rel(Oid relid, Form_pg_class reltuple);
static void truncate_check_perms(Oid relid, Form_pg_class reltuple);
static void truncate_check_activity(Relation rel);
static void RangeVarCallbackForTruncate(const RangeVar *relation,
										Oid relId, Oid oldRelId, void *arg);
static List *MergeAttributes(List *schema, List *supers, char relpersistence,
							 bool is_partition, List **supconstr);
static bool MergeCheckConstraint(List *constraints, char *name, Node *expr);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void StoreCatalogInheritance(Oid relationId, List *supers,
									bool child_is_partition);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
									 int32 seqNumber, Relation inhRelation,
									 bool child_is_partition);
static int	findAttrByName(const char *attributeName, List *schema);
static void AlterIndexNamespaces(Relation classRel, Relation rel,
								 Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved);
static void AlterSeqNamespaces(Relation classRel, Relation rel,
							   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
							   LOCKMODE lockmode);
static ObjectAddress ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd,
										   bool recurse, bool recursing, LOCKMODE lockmode);
static bool ATExecAlterConstrRecurse(Constraint *cmdcon, Relation conrel, Relation tgrel,
									 Relation rel, HeapTuple contuple, List **otherrelids,
									 LOCKMODE lockmode);
static ObjectAddress ATExecValidateConstraint(List **wqueue,
											  Relation rel, char *constrName,
											  bool recurse, bool recursing, LOCKMODE lockmode);
static int	transformColumnNameList(Oid relId, List *colList,
									int16 *attnums, Oid *atttypids);
static int	transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
									   List **attnamelist,
									   int16 *attnums, Oid *atttypids,
									   Oid *opclasses);
static Oid	transformFkeyCheckAttrs(Relation pkrel,
									int numattrs, int16 *attnums,
									Oid *opclasses);
static void checkFkeyPermissions(Relation rel, int16 *attnums, int natts);
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId,
									 Oid *funcid);
static void validateForeignKeyConstraint(char *conname,
										 Relation rel, Relation pkrel,
										 Oid pkindOid, Oid constraintOid);
static void CheckAlterTableIsSafe(Relation rel);
static void ATController(AlterTableStmt *parsetree,
						 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
						 AlterTableUtilityContext *context);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  bool recurse, bool recursing, LOCKMODE lockmode,
					  AlterTableUtilityContext *context);
static void ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATExecCmd(List **wqueue, AlteredTableInfo *tab,
					  AlterTableCmd *cmd, LOCKMODE lockmode, int cur_pass,
					  AlterTableUtilityContext *context);
static AlterTableCmd *ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab,
										  Relation rel, AlterTableCmd *cmd,
										  bool recurse, LOCKMODE lockmode,
										  int cur_pass,
										  AlterTableUtilityContext *context);
static void ATRewriteTables(AlterTableStmt *parsetree,
							List **wqueue, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(AlterTableType cmdtype, Relation rel, int allowed_targets);
static void ATSimpleRecursion(List **wqueue, Relation rel,
							  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode);
static void ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
								  LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static List *find_typed_table_dependencies(Oid typeOid, const char *typeName,
										   DropBehavior behavior);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static ObjectAddress ATExecAddColumn(List **wqueue, AlteredTableInfo *tab,
									 Relation rel, AlterTableCmd **cmd,
									 bool recurse, bool recursing,
									 LOCKMODE lockmode, int cur_pass,
									 AlterTableUtilityContext *context);
static bool check_for_column_name_collision(Relation rel, const char *colname,
											bool if_not_exists);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid);
static void ATPrepDropNotNull(Relation rel, bool recurse, bool recursing);
static ObjectAddress ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode);
static void ATPrepSetNotNull(List **wqueue, Relation rel,
							 AlterTableCmd *cmd, bool recurse, bool recursing,
							 LOCKMODE lockmode,
							 AlterTableUtilityContext *context);
static ObjectAddress ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
									  const char *colName, LOCKMODE lockmode);
static void ATExecCheckNotNull(AlteredTableInfo *tab, Relation rel,
							   const char *colName, LOCKMODE lockmode);
static bool NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr);
static bool ConstraintImpliedByRelConstraint(Relation scanrel,
											 List *testConstraint, List *provenConstraint);
static ObjectAddress ATExecColumnDefault(Relation rel, const char *colName,
										 Node *newDefault, LOCKMODE lockmode);
static ObjectAddress ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
											   Node *newDefault);
static ObjectAddress ATExecAddIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode);
static ObjectAddress ATExecSetIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode);
static ObjectAddress ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode);
static void ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode);
static ObjectAddress ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode);
static ObjectAddress ATExecSetStatistics(Relation rel, const char *colName, int16 colNum,
										 Node *newValue, LOCKMODE lockmode);
static ObjectAddress ATExecSetOptions(Relation rel, const char *colName,
									  Node *options, bool isReset, LOCKMODE lockmode);
static ObjectAddress ATExecSetStorage(Relation rel, const char *colName,
									  Node *newValue, LOCKMODE lockmode);
static void ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							 AlterTableCmd *cmd, LOCKMODE lockmode,
							 AlterTableUtilityContext *context);
static ObjectAddress ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
									  DropBehavior behavior,
									  bool recurse, bool recursing,
									  bool missing_ok, LOCKMODE lockmode,
									  ObjectAddresses *addrs);
static ObjectAddress ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
									IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
										 CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddConstraint(List **wqueue,
										 AlteredTableInfo *tab, Relation rel,
										 Constraint *newConstraint, bool recurse, bool is_readd,
										 LOCKMODE lockmode);
static char *ChooseForeignKeyConstraintNameAddition(List *colnames);
static ObjectAddress ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
											  IndexStmt *stmt, LOCKMODE lockmode);
static ObjectAddress ATAddCheckConstraint(List **wqueue,
										  AlteredTableInfo *tab, Relation rel,
										  Constraint *constr,
										  bool recurse, bool recursing, bool is_readd,
										  LOCKMODE lockmode);
static ObjectAddress ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab,
											   Relation rel, Constraint *fkconstraint,
											   bool recurse, bool recursing,
											   LOCKMODE lockmode);
static int	validateFkOnDeleteSetColumns(int numfks, const int16 *fkattnums,
										 int numfksetcols, int16 *fksetcolsattnums,
										 List *fksetcols);
static ObjectAddress addFkConstraint(addFkConstraintSides fkside,
									 char *constraintname,
									 Constraint *fkconstraint, Relation rel,
									 Relation pkrel, Oid indexOid,
									 Oid parentConstr,
									 int numfks, int16 *pkattnum, int16 *fkattnum,
									 Oid *pfeqoperators, Oid *ppeqoperators,
									 Oid *ffeqoperators, int numfkdelsetcols,
									 int16 *fkdelsetcols, bool is_internal);
static void addFkRecurseReferenced(Constraint *fkconstraint,
								   Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
								   int numfks, int16 *pkattnum, int16 *fkattnum,
								   Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
								   int numfkdelsetcols, int16 *fkdelsetcols,
								   bool old_check_ok,
								   Oid parentDelTrigger, Oid parentUpdTrigger);
static void addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint,
									Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
									int numfks, int16 *pkattnum, int16 *fkattnum,
									Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
									int numfkdelsetcols, int16 *fkdelsetcols,
									bool old_check_ok, LOCKMODE lockmode,
									Oid parentInsTrigger, Oid parentUpdTrigger);
static void CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
									   Relation partitionRel);
static void CloneFkReferenced(Relation parentRel, Relation partitionRel);
static void CloneFkReferencing(List **wqueue, Relation parentRel,
							   Relation partRel);
static void createForeignKeyCheckTriggers(Oid myRelOid, Oid refRelOid,
										  Constraint *fkconstraint, Oid constraintOid,
										  Oid indexOid,
										  Oid parentInsTrigger, Oid parentUpdTrigger,
										  Oid *insertTrigOid, Oid *updateTrigOid);
static void createForeignKeyActionTriggers(Relation rel, Oid refRelOid,
										   Constraint *fkconstraint, Oid constraintOid,
										   Oid indexOid,
										   Oid parentDelTrigger, Oid parentUpdTrigger,
										   Oid *deleteTrigOid, Oid *updateTrigOid);
static bool tryAttachPartitionForeignKey(ForeignKeyCacheInfo *fk,
										 Oid partRelid,
										 Oid parentConstrOid, int numfks,
										 AttrNumber *mapped_conkey, AttrNumber *confkey,
										 Oid *conpfeqop,
										 Oid parentInsTrigger,
										 Oid parentUpdTrigger,
										 Relation trigrel);
static void GetForeignKeyActionTriggers(Relation trigrel,
										Oid conoid, Oid confrelid, Oid conrelid,
										Oid *deleteTriggerOid,
										Oid *updateTriggerOid);
static void GetForeignKeyCheckTriggers(Relation trigrel,
									   Oid conoid, Oid confrelid, Oid conrelid,
									   Oid *insertTriggerOid,
									   Oid *updateTriggerOid);
static void ATExecDropConstraint(Relation rel, const char *constrName,
								 DropBehavior behavior,
								 bool recurse, bool recursing,
								 bool missing_ok, LOCKMODE lockmode);
static void ATPrepAlterColumnType(List **wqueue,
								  AlteredTableInfo *tab, Relation rel,
								  bool recurse, bool recursing,
								  AlterTableCmd *cmd, LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static bool ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno);
static ObjectAddress ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
										   AlterTableCmd *cmd, LOCKMODE lockmode);
static void RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab);
static void RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab);
static void RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab,
								   LOCKMODE lockmode);
static void ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId,
								 char *cmd, List **wqueue, LOCKMODE lockmode,
								 bool rewrite);
static void RebuildConstraintComment(AlteredTableInfo *tab, int pass,
									 Oid objid, Relation rel, List *domname,
									 const char *conname);
static void TryReuseIndex(Oid oldId, IndexStmt *stmt);
static void TryReuseForeignKey(Oid oldId, Constraint *con);
static ObjectAddress ATExecAlterColumnGenericOptions(Relation rel, const char *colName,
													 List *options, LOCKMODE lockmode);
static void change_owner_fix_column_acls(Oid relationOid,
										 Oid oldOwnerId, Oid newOwnerId);
static void change_owner_recurse_to_sequences(Oid relationOid,
											  Oid newOwnerId, LOCKMODE lockmode);
static ObjectAddress ATExecClusterOn(Relation rel, const char *indexName,
									 LOCKMODE lockmode);
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode);
static void ATPrepSetAccessMethod(AlteredTableInfo *tab, Relation rel, const char *amname);
static bool ATPrepChangePersistence(Relation rel, bool toLogged);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
								const char *tablespacename, LOCKMODE lockmode);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode);
static void ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace);
static void ATExecSetRelOptions(Relation rel, List *defList,
								AlterTableType operation,
								LOCKMODE lockmode);
static void ATExecEnableDisableTrigger(Relation rel, const char *trigname,
									   char fires_when, bool skip_system, bool recurse,
									   LOCKMODE lockmode);
static void ATExecEnableDisableRule(Relation rel, const char *rulename,
									char fires_when, LOCKMODE lockmode);
static void ATPrepAddInherit(Relation child_rel);
static ObjectAddress ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode);
static ObjectAddress ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode);
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
								   DependencyType deptype);
static ObjectAddress ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode);
static void ATExecDropOf(Relation rel, LOCKMODE lockmode);
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode);
static void ATExecGenericOptions(Relation rel, List *options);
static void ATExecSetRowSecurity(Relation rel, bool rls);
static void ATExecForceNoForceRowSecurity(Relation rel, bool force_rls);
static ObjectAddress ATExecSetCompression(Relation rel,
										  const char *column, Node *newValue, LOCKMODE lockmode);

static void index_copy_data(Relation rel, RelFileLocator newrlocator);
static const char *storage_name(char c);

static void RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid,
											Oid oldRelOid, void *arg);
static void RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid,
											 Oid oldrelid, void *arg);
static PartitionSpec *transformPartitionSpec(Relation rel, PartitionSpec *partspec);
static void ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
								  List **partexprs, Oid *partopclass, Oid *partcollation,
								  PartitionStrategy strategy);
static void CreateInheritance(Relation child_rel, Relation parent_rel);
static void RemoveInheritance(Relation child_rel, Relation parent_rel,
							  bool expect_detached);
static ObjectAddress ATExecAttachPartition(List **wqueue, Relation rel,
										   PartitionCmd *cmd,
										   AlterTableUtilityContext *context);
static void AttachPartitionEnsureIndexes(Relation rel, Relation attachrel);
static void QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
											   List *partConstraint,
											   bool validate_default);
static void CloneRowTriggersToPartition(Relation parent, Relation partition);
static void DetachAddConstraintIfNeeded(List **wqueue, Relation partRel);
static void DropClonedTriggersFromPartition(Oid partitionId);
static ObjectAddress ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab,
										   Relation rel, RangeVar *name,
										   bool concurrent);
static void DetachPartitionFinalize(Relation rel, Relation partRel,
									bool concurrent, Oid defaultPartOid);
static ObjectAddress ATExecDetachPartitionFinalize(Relation rel, RangeVar *name);
static ObjectAddress ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx,
											  RangeVar *name);
static void validatePartitionedIndex(Relation partedIdx, Relation partedTbl);
static void refuseDupeIndexAttach(Relation parentIdx, Relation partIdx,
								  Relation partitionTbl);
static List *GetParentedForeignKeyRefs(Relation partition);
static void ATDetachCheckNoForeignKeyRefs(Relation partition);
static char GetAttributeCompression(Oid atttypid, char *compression);
static char GetAttributeStorage(Oid atttypid, const char *storagemode);


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * stmt carries parsetree information from an ordinary CREATE TABLE statement.
 * The other arguments are used to extend the behavior for other cases:
 * relkind: relkind to assign to the new relation
 * ownerId: if not InvalidOid, use this as the new relation's owner.
 * typaddress: if not null, it's set to the pg_type entry's address.
 * queryString: for error reporting
 *
 * Note that permissions checks are done against current user regardless of
 * ownerId.  A nonzero ownerId is used when someone is creating a relation
 * "on behalf of" someone else, so we still want to see that the current user
 * has permissions to do it.
 *
 * If successful, returns the address of the new relation.
 * ----------------------------------------------------------------
 */
ObjectAddress
DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId,
			   ObjectAddress *typaddress, const char *queryString)
{
	char		relname[NAMEDATALEN];
	Oid			namespaceId;
	Oid			relationId;
	Oid			tablespaceId;
	Relation	rel;
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	List	   *rawDefaults;
	List	   *cookedDefaults;
	Datum		reloptions;
	ListCell   *listptr;
	AttrNumber	attnum;
	bool		partitioned;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	Oid			ofTypeId;
	ObjectAddress address;
	LOCKMODE	parentLockmode;
	const char *accessMethod = NULL;
	Oid			accessMethodId = InvalidOid;

	/*
	 * Truncate relname to appropriate length (probably a waste of time, as 解析器
	 * should have done this 已经).
	 */
	strlcpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * Check consistency of arguments
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables")));

	if (stmt->partspec != NULL)
	{
		if (relkind != RELKIND_RELATION)
			elog(ERROR, "unexpected relkind: %d", (int) relkind);

		relkind = RELKIND_PARTITIONED_TABLE;
		partitioned = true;
	}
	else
		partitioned = false;

	/*
	 * Look up the 命名空间 in which we are supposed to create the relation, check we
	 * have 权限 to create there, 锁 it against 并发 drop, and mark stmt->relation as
	 * RELPERSISTENCE_TEMP if a temporary 命名空间 is selected.
	 *
	 */
	namespaceId =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, NULL);

	/*
	 * Security check: disallow creating temp tables from 安全受限 code. This 是必需的 因为
	 * calling code might not expect untrusted tables to appear in pg_temp at the
	 * front of its 搜索路径.
	 */
	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP
		&& InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create temporary table within security-restricted operation")));

	/*
	 * Determine the lockmode to use when scanning parents. A self-排他锁 是必需的 here.
	 * For regular 继承, if two 后端 attempt to add children to the same parent
	 * simultaneously, and that parent has no pre-existing children, then both
	 * will attempt to update the parent's relhassubclass field, leading to a "元组
	 * con当前 updated" error. Also, this interlocks against a 并发 ANALYZE on the
	 * 父表, which might 否则 be attempting to clear the parent's relhassubclass
	 * field, if its previous children were recently dropped. If the 子表 is a
	 * partition, then we 相反 grab an 排他锁 on the parent 因为 its 分区描述符 将被 changed by
	 * addition of the new partition.
	 *
	 *
	 *
	 *
	 *
	 */
	parentLockmode = (stmt->partbound != NULL ? AccessExclusiveLock :
					  ShareUpdateExclusiveLock);

	/* 确定父表的 OID 列表。 */
	inheritOids = NIL;
	foreach(listptr, stmt->inhRelations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(listptr);
		Oid			parentOid;

		parentOid = RangeVarGetRelid(rv, parentLockmode, false);

		/*
		 * Reject duplications in the list of parents.
		 */
		if (list_member_oid(inheritOids, parentOid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							get_rel_name(parentOid))));

		inheritOids = lappend_oid(inheritOids, parentOid);
	}

	/*
	 * Select 表空间 to use: an explicitly indicated one, or (in the case of a 分区表)
	 * the parent's, if it has one.
	 */
	if (stmt->tablespacename)
	{
		tablespaceId = get_tablespace_oid(stmt->tablespacename, false);

		if (partitioned && tablespaceId == MyDatabaseTableSpace)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
	}
	else if (stmt->partbound)
	{
		Assert(list_length(inheritOids) == 1);
		tablespaceId = get_rel_tablespace(linitial_oid(inheritOids));
	}
	else
		tablespaceId = InvalidOid;

	/* 仍然没有？使用默认值 */
	if (!OidIsValid(tablespaceId))
		tablespaceId = GetDefaultTablespace(stmt->relation->relpersistence,
											partitioned);

	/* 检查权限，除使用数据库默认表空间外 */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/* 在任何情况下都不允许将用户关系放入 pg_global */
	if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/* 确定将拥有该表的用户 ID */
	if (!OidIsValid(ownerId))
		ownerId = GetUserId();

	/*
	 * Parse and 验证 reloptions, 如有.
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options, NULL, validnsps,
									 true, false);

	switch (relkind)
	{
		case RELKIND_VIEW:
			(void) view_reloptions(reloptions, true);
			break;
		case RELKIND_PARTITIONED_TABLE:
			(void) partitioned_table_reloptions(reloptions, true);
			break;
		default:
			(void) heap_reloptions(relkind, reloptions, true);
	}

	if (stmt->ofTypename)
	{
		AclResult	aclresult;

		ofTypeId = typenameTypeId(NULL, stmt->ofTypename);

		aclresult = object_aclcheck(TypeRelationId, ofTypeId, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, ofTypeId);
	}
	else
		ofTypeId = InvalidOid;

	/*
	 * Look up 继承 ancestors and generate relation 模式, including 继承的 属性. (请注意
	 * stmt->tableElts is destructively modified by MergeAttributes.)
	 *
	 */
	stmt->tableElts =
		MergeAttributes(stmt->tableElts, inheritOids,
						stmt->relation->relpersistence,
						stmt->partbound != NULL,
						&old_constraints);

	/*
	 * Create a 元组描述符 from the relation 模式. 请注意 this deals with 列名, types, and
	 * NOT NULL 约束, but not 默认值 or CHECK 约束; we handle those 以下.
	 *
	 */
	descriptor = BuildDescForRelation(stmt->tableElts);

	/*
	 * Find columns with 默认值 and prepare for insertion of the defaults.
	 * Pre-cooked (that is, 继承的) defaults go into a list of CookedConstraint
	 * structs that we'll pass to heap_create_with_catalog, while raw defaults go
	 * into a list of RawColumnDefault structs that 将被 processed by
	 * AddRelationNewConstraints. (我们可以't deal with raw expressions until 我们可以 do
	 * transformExpr.) 我们可以 set the atthasdef flags now in the 元组描述符; this just
	 * saves StoreAttrDefault from having to do an immediate update of the
	 * pg_attribute rows.
	 *
	 *
	 */
	rawDefaults = NIL;
	cookedDefaults = NIL;
	attnum = 0;

	foreach(listptr, stmt->tableElts)
	{
		ColumnDef  *colDef = lfirst(listptr);
		Form_pg_attribute attr;

		attnum++;
		attr = TupleDescAttr(descriptor, attnum - 1);

		if (colDef->raw_default != NULL)
		{
			RawColumnDefault *rawEnt;

			Assert(colDef->cooked_default == NULL);

			rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
			rawEnt->attnum = attnum;
			rawEnt->raw_default = colDef->raw_default;
			rawEnt->missingMode = false;
			rawEnt->generated = colDef->generated;
			rawDefaults = lappend(rawDefaults, rawEnt);
			attr->atthasdef = true;
		}
		else if (colDef->cooked_default != NULL)
		{
			CookedConstraint *cooked;

			cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
			cooked->contype = CONSTR_DEFAULT;
			cooked->conoid = InvalidOid;	/* 直到创建 */
			cooked->name = NULL;
			cooked->attnum = attnum;
			cooked->expr = colDef->cooked_default;
			cooked->skip_validation = false;
			cooked->is_local = true;	/* 不用于默认值 */
			cooked->inhcount = 0;	/* 同上 */
			cooked->is_no_inherit = false;
			cookedDefaults = lappend(cookedDefaults, cooked);
			attr->atthasdef = true;
		}

		if (colDef->identity)
			attr->attidentity = colDef->identity;

		if (colDef->generated)
			attr->attgenerated = colDef->generated;

		if (colDef->compression)
			attr->attcompression = GetAttributeCompression(attr->atttypid,
														   colDef->compression);

		if (colDef->storage_name)
			attr->attstorage = GetAttributeStorage(attr->atttypid, colDef->storage_name);
	}

	/*
	 * If the statement hasn't specified an 访问方法, but we're defining a type of
	 * relation that needs one, use the default.
	 */
	if (stmt->accessMethod != NULL)
	{
		accessMethod = stmt->accessMethod;

		if (partitioned)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("specifying a table access method is not supported on a partitioned table")));
	}
	else if (RELKIND_HAS_TABLE_AM(relkind))
		accessMethod = default_table_access_method;

	/* 查找访问方法，验证其适用于表 */
	if (accessMethod != NULL)
		accessMethodId = get_table_am_oid(accessMethod, false);

	/*
	 * Create the relation. Inherited defaults and 约束 are passed in for immediate
	 * handling --- since they don't need 解析, they can be stored immediately.
	 *
	 */
	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  tablespaceId,
										  InvalidOid,
										  InvalidOid,
										  ofTypeId,
										  ownerId,
										  accessMethodId,
										  descriptor,
										  list_concat(cookedDefaults,
													  old_constraints),
										  relkind,
										  stmt->relation->relpersistence,
										  false,
										  false,
										  stmt->oncommit,
										  reloptions,
										  true,
										  allowSystemTableMods,
										  false,
										  InvalidOid,
										  typaddress);

	/*
	 * 我们必须 bump the 命令计数器 to make the newly-created relation 元组 visible for
	 * opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire 排他锁 on it. This isn't really necessary
	 * for locking out other 后端 (since they can't see the new rel anyway until we
	 * 提交), but it keeps the 锁 manager from complaining about 死锁 risks.
	 *
	 */
	rel = relation_open(relationId, AccessExclusiveLock);

	/*
	 * Now add any newly specified 列默认值 and generation expressions to the new
	 * relation. These are passed to us in the form of raw parsetrees; 我们需要 转换
	 * them to executable expression trees 之前 they can be added. The most
	 * convenient way to do that is to apply the 解析器's transformExpr routine, but
	 * transformExpr doesn't work unless we have a pre-existing relation. So, the
	 * 转换 必须是 postponed to this final step of CREATE TABLE. This 需要被 之前
	 * processing the partitioning clauses 因为 those could refer to 生成列.
	 *
	 *
	 *
	 */
	if (rawDefaults)
		AddRelationNewConstraints(rel, rawDefaults, NIL,
								  true, true, false, queryString);

	/*
	 * Make column generation expressions visible for use by partitioning.
	 */
	CommandCounterIncrement();

	/* 处理并存储分区边界（如有）。 */
	if (stmt->partbound)
	{
		PartitionBoundSpec *bound;
		ParseState *pstate;
		Oid			parentId = linitial_oid(inheritOids),
					defaultPartOid;
		Relation	parent,
					defaultRel = NULL;
		ParseNamespaceItem *nsitem;

		/* 已持有对父表的足够强的锁 */
		parent = table_open(parentId, NoLock);

		/*
		 * We are going to try to 验证 the 分区边界 specification against the 分区键 of
		 * parentRel, so it better have one.
		 */
		if (parent->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("\"%s\" is not partitioned",
							RelationGetRelationName(parent))));

		/*
		 * The partition 约束 of the 默认分区 depends on the partition bounds of every
		 * other partition. It is possible that another 后端 可能 about to 执行 a 查询 on
		 * the 默认分区 table, and that the 查询 relies on 先前 cached 默认分区 约束. 我们必须 因此 take
		 * a table 锁 strong enough to prevent all 查询 on the 默认分区 from proceeding
		 * until we 提交 and send out a shared-cache-inval notice that will make them
		 * update their 索引 lists. Order of locking: The relation being added won't
		 * be visible to other 后端 until it is committed, hence here in
		 * DefineRelation() the order of locking the 默认分区 and the relation being
		 * added does not matter. But at all other places 我们需要 锁 the default
		 * relation 之前 we 锁 the relation being added or removed i.e. we should take
		 * the 锁 in same order at all the places such that 锁 parent, 锁 默认分区 and then
		 * 锁 the partition so as to avoid a 死锁.
		 *
		 *
		 *
		 *
		 *
		 */
		defaultPartOid =
			get_default_oid_from_partdesc(RelationGetPartitionDesc(parent,
																   true));
		if (OidIsValid(defaultPartOid))
			defaultRel = table_open(defaultPartOid, AccessExclusiveLock);

		/* 转换边界值 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * Add an nsitem containing this relation, 从而 transformExpr called on 分区边界
		 * expressions is able to report errors using a proper context.
		 *
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
											   NULL, false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		bound = transformPartitionBound(pstate, parent, stmt->partbound);

		/*
		 * Check first that the new partition's bound is valid and does not
		 * overlap with any of existing partitions of the parent.
		 */
		check_new_partition_bound(relname, parent, bound, pstate);

		/*
		 * If the 默认分区 exists, its partition 约束 will change 之后 the addition of this
		 * new partition such that it won't allow any row that qualifies for this
		 * new partition. So, 检查 the existing data in the 默认分区 satisfies the 约束 as
		 * it will exist 之后 adding this partition.
		 *
		 */
		if (OidIsValid(defaultPartOid))
		{
			check_default_partition_contents(parent, defaultRel, bound);
			/* 保持锁直到提交。 */
			table_close(defaultRel, NoLock);
		}

		/* 更新 pg_class 条目。 */
		StorePartitionBound(rel, parent, bound);

		table_close(parent, NoLock);
	}

	/* 存储新关系的继承信息。 */
	StoreCatalogInheritance(relationId, inheritOids, stmt->partbound != NULL);

	/*
	 * Process the partitioning specification (如有) and store the 分区键 information
	 * into the 目录.
	 */
	if (partitioned)
	{
		ParseState *pstate;
		int			partnatts;
		AttrNumber	partattrs[PARTITION_MAX_KEYS];
		Oid			partopclass[PARTITION_MAX_KEYS];
		Oid			partcollation[PARTITION_MAX_KEYS];
		List	   *partexprs = NIL;

		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		partnatts = list_length(stmt->partspec->partParams);

		/* 在此处和执行器中保护固定大小数组 */
		if (partnatts > PARTITION_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot partition using more than %d columns",
							PARTITION_MAX_KEYS)));

		/*
		 * 我们需要 转换 the raw parsetrees corresponding to partition expressions into
		 * executable expression trees. Like column defaults and CHECK 约束, we could
		 * not have done the 转换 更早的.
		 *
		 */
		stmt->partspec = transformPartitionSpec(rel, stmt->partspec);

		ComputePartitionAttrs(pstate, rel, stmt->partspec->partParams,
							  partattrs, &partexprs, partopclass,
							  partcollation, stmt->partspec->strategy);

		StorePartitionKey(rel, stmt->partspec->strategy, partnatts, partattrs,
						  partexprs,
						  partopclass, partcollation);

		/* 使其全部可见 */
		CommandCounterIncrement();
	}

	/*
	 * 创建分区时，即创建父分区中定义的所有索引、     
	 * 触发器和外键。     
	 *
	 * 不能更早这样做，因为 DefineIndex 需要知道我们刚刚    
	 * 存储的分区键。    
	 */
	if (stmt->partbound)
	{
		Oid			parentId = linitial_oid(inheritOids);
		Relation	parent;
		List	   *idxlist;
		ListCell   *cell;

		/* 已持有对父表的足够强的锁 */
		parent = table_open(parentId, NoLock);
		idxlist = RelationGetIndexList(parent);

		/*
		 * For each 索引 in the 父表, create one in the partition
		 */
		foreach(cell, idxlist)
		{
			Relation	idxRel = index_open(lfirst_oid(cell), AccessShareLock);
			AttrMap    *attmap;
			IndexStmt  *idxstmt;
			Oid			constraintOid;

			if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			{
				if (idxRel->rd_index->indisunique)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot create foreign partition of partitioned table \"%s\"",
									RelationGetRelationName(parent)),
							 errdetail("Table \"%s\" contains indexes that are unique.",
									   RelationGetRelationName(parent))));
				else
				{
					index_close(idxRel, AccessShareLock);
					continue;
				}
			}

			attmap = build_attrmap_by_name(RelationGetDescr(rel),
										   RelationGetDescr(parent),
										   false);
			idxstmt =
				generateClonedIndexStmt(NULL, idxRel,
										attmap, &constraintOid);
			DefineIndex(RelationGetRelid(rel),
						idxstmt,
						InvalidOid,
						RelationGetRelid(idxRel),
						constraintOid,
						-1,
						false, false, false, false, false);

			index_close(idxRel, AccessShareLock);
		}

		list_free(idxlist);

		/*
		 * If there are any 行级触发器s, clone them to the new partition.
		 *
		 */
		if (parent->trigdesc != NULL)
			CloneRowTriggersToPartition(parent, rel);

		/*
		 * And foreign keys too. 请注意 因为 we're freshly creating the table, there is
		 * no need to verify these new 约束.
		 */
		CloneForeignKeyConstraints(NULL, parent, rel);

		table_close(parent, NoLock);
	}

	/*
	 * Now add any newly specified CHECK 约束 to the new relation. Same as for
	 * defaults 以上, but these need to come 之后 partitioning 被设置 up.
	 *
	 */
	if (stmt->constraints)
		AddRelationNewConstraints(rel, NIL, stmt->constraints,
								  true, true, false, queryString);

	ObjectAddressSet(address, RelationRelationId, relationId);

	/*
	 * Clean up. We keep 锁 on new relation (although it shouldn't be visible to
	 * anyone else anyway, until 提交).
	 */
	relation_close(rel, NoLock);

	return address;
}

/*
 * Emit the right error or warning message for a "DROP" command issued on a
 * non-existent relation
 */
static void
DropErrorMsgNonExistent(RangeVar *rel, char rightkind, bool missing_ok)
{
	const struct dropmsgstrings *rentry;

	if (rel->schemaname != NULL &&
		!OidIsValid(LookupNamespaceNoError(rel->schemaname)))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist", rel->schemaname)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("schema \"%s\" does not exist, skipping",
							rel->schemaname)));
		}
		return;
	}

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
	{
		if (rentry->kind == rightkind)
		{
			if (!missing_ok)
			{
				ereport(ERROR,
						(errcode(rentry->nonexistent_code),
						 errmsg(rentry->nonexistent_msg, rel->relname)));
			}
			else
			{
				ereport(NOTICE, (errmsg(rentry->skipping_msg, rel->relname)));
				break;
			}
		}
	}

	Assert(rentry->kind != '\0');	/* 不应可能出现 */
}

/*
 * Emit the right error message for a "DROP" command issued on a
 * relation of the wrong type
 */
static void
DropErrorMsgWrongType(const char *relname, char wrongkind, char rightkind)
{
	const struct dropmsgstrings *rentry;
	const struct dropmsgstrings *wentry;

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = dropmsgstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind 可能是我们的表中没有的内容... */

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(rentry->nota_msg, relname),
			 (wentry->kind != '\0') ? errhint("%s", _(wentry->drophint_msg)) : 0));
}

/*
 * RemoveRelations 实现 DROP TABLE、DROP INDEX、DROP SEQUENCE、DROP VIEW、 DROP
 * MATERIALIZED VIEW、DROP FOREIGN TABLE
 *
 */
void
RemoveRelations(DropStmt *drop)
{
	ObjectAddresses *objects;
	char		relkind;
	ListCell   *cell;
	int			flags = 0;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/* DROP CONCURRENTLY 使用较弱锁，且有一些限制 */
	if (drop->concurrent)
	{
		/*
		 * 请注意 for temporary relations this 锁 may get upgraded 稍后的 on, but as no
		 * other 会话 can access a 临时关系, this is actually fine.
		 *
		 */
		lockmode = ShareUpdateExclusiveLock;
		Assert(drop->removeType == OBJECT_INDEX);
		if (list_length(drop->objects) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
		if (drop->behavior == DROP_CASCADE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
	}

	/*
	 * First we identify all the relations, then we delete them in a single
	 * performMultipleDeletions() call.  This is to avoid unwanted DROP
	 * RESTRICT errors if one of the relations depends on another.
	 */

	/* 确定所需的 relkind */
	switch (drop->removeType)
	{
		case OBJECT_TABLE:
			relkind = RELKIND_RELATION;
			break;

		case OBJECT_INDEX:
			relkind = RELKIND_INDEX;
			break;

		case OBJECT_SEQUENCE:
			relkind = RELKIND_SEQUENCE;
			break;

		case OBJECT_VIEW:
			relkind = RELKIND_VIEW;
			break;

		case OBJECT_MATVIEW:
			relkind = RELKIND_MATVIEW;
			break;

		case OBJECT_FOREIGN_TABLE:
			relkind = RELKIND_FOREIGN_TABLE;
			break;

		default:
			elog(ERROR, "unrecognized drop object type: %d",
				 (int) drop->removeType);
			relkind = 0;		/* 避免编译器警告 */
			break;
	}

	/* 锁定并验证每个关系；构建对象地址列表 */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		RangeVar   *rel = makeRangeVarFromNameList((List *) lfirst(cell));
		Oid			relOid;
		ObjectAddress obj;
		struct DropRelationCallbackState state;

		/*
		 * These next few steps are a great deal like relation_openrv, but 我们不
		 * bother building a relcache 条目 since 我们不 need it. Check for
		 * shared-cache-inval messages 之前 trying to access the relation. This 是必需的
		 * to cover the case where the name identifies a rel that has been dropped
		 * and recreated since the start of our 事务: if 我们不 flush the old syscache
		 * entry, then we'll latch onto that entry and suffer an error 稍后的.
		 *
		 *
		 */
		AcceptInvalidationMessages();

		/* 使用命名空间搜索查找相应的关系。 */
		state.expected_relkind = relkind;
		state.heap_lockmode = drop->concurrent ?
			ShareUpdateExclusiveLock : AccessExclusiveLock;
		/* 我们必须初始化这些字段以表明未持有任何锁： */
		state.heapOid = InvalidOid;
		state.partParentOid = InvalidOid;

		relOid = RangeVarGetRelidExtended(rel, lockmode, RVR_MISSING_OK,
										  RangeVarCallbackForDropRelation,
										  (void *) &state);

		/* 不存在？ */
		if (!OidIsValid(relOid))
		{
			DropErrorMsgNonExistent(rel, relkind, drop->missing_ok);
			continue;
		}

		/*
		 * Decide if 并发 mode 需要被 used here 与否. The 回调 retrieved the rel's
		 * persistence for us.
		 */
		if (drop->concurrent &&
			state.actual_relpersistence != RELPERSISTENCE_TEMP)
		{
			Assert(list_length(drop->objects) == 1 &&
				   drop->removeType == OBJECT_INDEX);
			flags |= PERFORM_DELETION_CONCURRENTLY;
		}

		/*
		 * Concurrent 索引 drop 不能 used with partitioned 索引, either.
		 *
		 */
		if ((flags & PERFORM_DELETION_CONCURRENTLY) != 0 &&
			state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot drop partitioned index \"%s\" concurrently",
							rel->relname)));

		/*
		 * If we're told to drop a 分区索引, 我们必须 acquire 锁 on all the children of its
		 * 父分区表 之前 proceeding. Otherwise we'd try to 锁 the child 索引 partitions 之前
		 * their tables, leading to potential 死锁 against other 会话 that will 锁 those
		 * objects in the other order.
		 *
		 */
		if (state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			(void) find_all_inheritors(state.heapOid,
									   state.heap_lockmode,
									   NULL);

		/* 好的，准备删除此项 */
		obj.classId = RelationRelationId;
		obj.objectId = relOid;
		obj.objectSubId = 0;

		add_exact_object_address(&obj, objects);
	}

	performMultipleDeletions(objects, drop->behavior, flags);

	free_object_addresses(objects);
}

/*
 * Before acquiring a table 锁, check whether we have sufficient rights. In the
 * case of DROP INDEX, 也 try to 锁 the table 之前 the 索引. Also, if the table to
 * be dropped is a partition, we try to 锁 the parent first.
 *
 */
static void
RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid, Oid oldRelOid,
								void *arg)
{
	HeapTuple	tuple;
	struct DropRelationCallbackState *state;
	char		expected_relkind;
	bool		is_partition;
	Form_pg_class classform;
	LOCKMODE	heap_lockmode;
	bool		invalid_system_index = false;

	state = (struct DropRelationCallbackState *) arg;
	heap_lockmode = state->heap_lockmode;

	/*
	 * If we 先前 locked some other 索引's 堆, and the name we're looking up no longer
	 * refers to that relation, release the now-useless 锁.
	 *
	 */
	if (relOid != oldRelOid && OidIsValid(state->heapOid))
	{
		UnlockRelationOid(state->heapOid, heap_lockmode);
		state->heapOid = InvalidOid;
	}

	/*
	 * Similarly, if we 先前 locked some other partition's 堆, and the name we're
	 * looking up no longer refers to that relation, release the now-useless 锁.
	 *
	 */
	if (relOid != oldRelOid && OidIsValid(state->partParentOid))
	{
		UnlockRelationOid(state->partParentOid, AccessExclusiveLock);
		state->partParentOid = InvalidOid;
	}

	/* 未找到关系，因此无需锁定或权限检查。 */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除，因此无需操作 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	is_partition = classform->relispartition;

	/* 传回一些数据以节省 RemoveRelations 中的查找 */
	state->actual_relkind = classform->relkind;
	state->actual_relpersistence = classform->relpersistence;

	/*
	 * Both RELKIND_RELATION and RELKIND_PARTITIONED_TABLE are OBJECT_TABLE, but
	 * RemoveRelations() can 仅 pass one relkind for a given relation. It chooses
	 * RELKIND_RELATION for both regular and partitioned tables. That means 我们必须
	 * be careful 之前 giving the wrong type error when the relation is
	 * RELKIND_PARTITIONED_TABLE. An equivalent problem exists with 索引.
	 *
	 */
	if (classform->relkind == RELKIND_PARTITIONED_TABLE)
		expected_relkind = RELKIND_RELATION;
	else if (classform->relkind == RELKIND_PARTITIONED_INDEX)
		expected_relkind = RELKIND_INDEX;
	else
		expected_relkind = classform->relkind;

	if (state->expected_relkind != expected_relkind)
		DropErrorMsgWrongType(rel->relname, classform->relkind,
							  state->expected_relkind);

	/* 允许表属主或模式属主执行 DROP */
	if (!object_ownercheck(RelationRelationId, relOid, GetUserId()) &&
		!object_ownercheck(NamespaceRelationId, classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(classform->relkind),
					   rel->relname);

	/*
	 * Check the case of a system 索引 that might have been 已失效 by a failed 并发
	 * process and allow its drop. For the time being, this 仅 concerns 索引 of
	 * toast relations that became 无效 期间 a REINDEX CONCURRENTLY process.
	 *
	 */
	if (IsSystemClass(relOid, classform) && classform->relkind == RELKIND_INDEX)
	{
		HeapTuple	locTuple;
		Form_pg_index indexform;
		bool		indisvalid;

		locTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(relOid));
		if (!HeapTupleIsValid(locTuple))
		{
			ReleaseSysCache(tuple);
			return;
		}

		indexform = (Form_pg_index) GETSTRUCT(locTuple);
		indisvalid = indexform->indisvalid;
		ReleaseSysCache(locTuple);

		/* 将对象标记为系统目录的无效索引 */
		if (!indisvalid)
			invalid_system_index = true;
	}

	/* 对于无效索引，可以绕过此检查 */
	if (!invalid_system_index && !allowSystemTableMods && IsSystemClass(relOid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	/*
	 * In DROP INDEX, attempt to acquire 锁 on the 父表 之前 locking the 索引.
	 * index_drop() will need this anyway, and since regular 查询 锁 tables 之前 their
	 * 索引, we risk 死锁 if we do it the other way around. No error if 我们不 find a
	 * pg_index entry, though --- the relation may have been dropped. 请注意 this
	 * code will 执行 for either plain or partitioned 索引.
	 *
	 */
	if (expected_relkind == RELKIND_INDEX &&
		relOid != oldRelOid)
	{
		state->heapOid = IndexGetRelation(relOid, true);
		if (OidIsValid(state->heapOid))
			LockRelationOid(state->heapOid, heap_lockmode);
	}

	/*
	 * Similarly, if the relation is a partition, 我们必须 acquire 锁 on its parent 之前
	 * locking the partition. That's 因为 查询 锁 the parent 之前 its partitions, so we
	 * risk 死锁 if we do it the other way around.
	 *
	 */
	if (is_partition && relOid != oldRelOid)
	{
		state->partParentOid = get_partition_parent(relOid, true);
		if (OidIsValid(state->partParentOid))
			LockRelationOid(state->partParentOid, AccessExclusiveLock);
	}
}

/*
 * ExecuteTruncate 执行 TRUNCATE 命令。 This is a multi-relation truncate. We first
 * open and grab 排他锁 on all relations involved, checking 权限 and 否则 verifying
 * that the relation is OK for truncation. 请注意 if relations are foreign
 * tables, at this stage, we have not yet checked that their foreign data in
 * external data sources are OK for truncation. These are checked when foreign
 * data are actually truncated 稍后的. In CASCADE mode, relations having FK
 * references to the targeted relations are automatically added to the group;
 * in RESTRICT mode, we 检查 all FK references are internal to the group that's
 * being truncated. Finally all the relations are truncated and reindexed.
 *
 *
 *
 *
 */
void
ExecuteTruncate(TruncateStmt *stmt)
{
	List	   *rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *cell;

	/*
	 * Open, 排他锁, and check all the explicitly-specified relations
	 */
	foreach(cell, stmt->relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;
		bool		recurse = rv->inh;
		Oid			myrelid;
		LOCKMODE	lockmode = AccessExclusiveLock;

		myrelid = RangeVarGetRelidExtended(rv, lockmode,
										   0, RangeVarCallbackForTruncate,
										   NULL);

		/* 执行n't th行 错误 for "TRUNCATE foo, foo" */
		if (list_member_oid(relids, myrelid))
			continue;

		/* 打开 the 关系, we 已经 h旧 a 锁 on it */
		rel = table_open(myrelid, NoLock);

		/*
		 * RangeVarGetRelidExtended() has done most 检查 with its 回调, but other 检查
		 * with the now-opened Relation remain.
		 */
		truncate_check_activity(rel);

		rels = lappend(rels, rel);
		relids = lappend_oid(relids, myrelid);

		/* Log this 关系 仅 if 需要ed for logical decoding */
		if (RelationIsLogicallyLogged(rel))
			relids_logged = lappend_oid(relids_logged, myrelid);

		if (recurse)
		{
			ListCell   *child;
			List	   *children;

			children = find_all_inheritors(myrelid, lockmode, NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors 已获取锁 */
				rel = table_open(childrelid, NoLock);

				/*
				 * It is possible that the 父表 has children that are temp tables of other
				 * 后端. 我们可以not safely access such tables (因为 of buffering issues), and the
				 * best thing to do is to silently ignore them. 请注意 this check is the same
				 * as one of the 检查 done in truncate_check_activity() called 以下, still it
				 * is kept here for simplicity.
				 *
				 *
				 */
				if (RELATION_IS_OTHER_TEMP(rel))
				{
					table_close(rel, lockmode);
					continue;
				}

				/*
				 * Inherited TRUNCATE commands perform access 权限 检查 on the 父表 仅. So we
				 * skip checking the children's 权限 and don't call truncate_check_perms()
				 * here.
				 *
				 */
				truncate_check_rel(RelationGetRelid(rel), rel->rd_rel);
				truncate_check_activity(rel);

				rels = lappend(rels, rel);
				relids = lappend_oid(relids, childrelid);

				/* Log this 关系 仅 if 需要ed for logical decoding */
				if (RelationIsLogicallyLogged(rel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
		else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot truncate only a partitioned table"),
					 errhint("Do not specify the ONLY keyword, or use TRUNCATE ONLY on the partitions directly.")));
	}

	ExecuteTruncateGuts(rels, relids, relids_logged,
						stmt->behavior, stmt->restart_seqs, false);

	/* And 关闭 the rels */
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * ExecuteTruncateGuts
 *
 * TRUNCATE 的内部实现。由实际的 TRUNCATE 命令（见上文）     
 * 以及执行被复制 TRUNCATE 动作的复制订阅者所           
 * 调用。                  
 *
 * explicit_rels 是命令指定的待截断 Relation 列表。       
 * relids 是与 explicit_rels 对应的 Oid 列表。                   
 * relids_logged：需要记录 WAL 的 Oid（relids 的子集）。  
 * 这有点冗余，但现有的调用者已经可以方便地取得      
 * 这些信息。                     
 */
void
ExecuteTruncateGuts(List *explicit_rels,
					List *relids,
					List *relids_logged,
					DropBehavior behavior, bool restart_seqs,
					bool run_as_table_owner)
{
	List	   *rels;
	List	   *seq_relids = NIL;
	HTAB	   *ft_htab = NULL;
	EState	   *estate;
	ResultRelInfo *resultRelInfos;
	ResultRelInfo *resultRelInfo;
	SubTransactionId mySubid;
	ListCell   *cell;
	Oid		   *logrelids;

	/*
	 * Check the explicitly-specified relations. In CASCADE mode, suck in all
	 * referencing relations 同时. This 需要 multiple iterations to find
	 * indirectly-dependent relations. At each phase, 我们需要 排他锁 new rels 之前
	 * looking for their 依赖, else we might miss something. Also, we check each
	 * rel as soon as we open it, to avoid a faux pas 例如 holding 锁 for a long
	 * time on a rel we have no 权限 for.
	 *
	 *
	 */
	rels = list_copy(explicit_rels);
	if (behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List	   *newrelids;

			newrelids = heap_truncate_find_FKs(relids);
			if (newrelids == NIL)
				break;			/* nothing else to 添加 */

			foreach(cell, newrelids)
			{
				Oid			relid = lfirst_oid(cell);
				Relation	rel;

				rel = table_open(relid, AccessExclusiveLock);
				ereport(NOTICE,
						(errmsg("truncate cascades to table \"%s\"",
								RelationGetRelationName(rel))));
				truncate_check_rel(relid, rel->rd_rel);
				truncate_check_perms(relid, rel->rd_rel);
				truncate_check_activity(rel);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, relid);

				/* Log this 关系 仅 if 需要ed for logical decoding */
				if (RelationIsLogicallyLogged(rel))
					relids_logged = lappend_oid(relids_logged, relid);
			}
		}
	}

	/*
	 * Check 外键 references. In CASCADE mode, this 应是 unnecessary since we just
	 * pulled in all the references; but as a cross-check, do it anyway if in an
	 * Assert-enabled build.
	 */
#ifdef USE_ASSERT_CHECKING
	heap_truncate_check_FKs(rels, false);
#else
	if (behavior == DROP_RESTRICT)
		heap_truncate_check_FKs(rels, false);
#endif

	/*
	 * If we are asked to restart 序列, find all the 序列, 锁 them (we need
	 * AccessExclusiveLock for ResetSequence), and check 权限. We want to do this
	 * early since it's pointless to do all the truncation work 仅 to fail on 序列
	 * 权限.
	 */
	if (restart_seqs)
	{
		foreach(cell, rels)
		{
			Relation	rel = (Relation) lfirst(cell);
			List	   *seqlist = getOwnedSequences(RelationGetRelid(rel));
			ListCell   *seqcell;

			foreach(seqcell, seqlist)
			{
				Oid			seq_relid = lfirst_oid(seqcell);
				Relation	seq_rel;

				seq_rel = relation_open(seq_relid, AccessExclusiveLock);

				/* This 检查 must match AlterSequence! */
				if (!object_ownercheck(RelationRelationId, seq_relid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SEQUENCE,
								   RelationGetRelationName(seq_rel));

				seq_relids = lappend_oid(seq_relids, seq_relid);

				relation_close(seq_rel, NoLock);
			}
		}
	}

	/* Prepare to catch AFTER 触发器s. */
	AfterTriggerBeginQuery();

	/*
	 * To fire 触发器, we'll need an EState 同时 as a ResultRelInfo for each relation.
	 * 我们不 need to call ExecOpenIndices, though. We put the ResultRelInfos in the
	 * es_opened_result_relations list, even though 我们不 have a range table and
	 * don't populate the es_result_relations array. That's a bit bogus, but it's
	 * enough to make ExecGetTriggerResultRel() find them.
	 *
	 *
	 */
	estate = CreateExecutorState();
	resultRelInfos = (ResultRelInfo *)
		palloc(list_length(rels) * sizeof(ResultRelInfo));
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		InitResultRelInfo(resultRelInfo,
						  rel,
						  0,	/* 虚拟范围表索引 */
						  NULL,
						  0);
		estate->es_opened_result_relations =
			lappend(estate->es_opened_result_relations, resultRelInfo);
		resultRelInfo++;
	}

	/*
	 * Process all BEFORE STATEMENT TRUNCATE 触发器 之前 we begin truncating (this is
	 * 因为 one of them might throw an error). Also, if we were to allow them to
	 * prevent statement 执行, that would need to be handled here.
	 *
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		UserContext ucxt;

		if (run_as_table_owner)
			SwitchToUntrustedUser(resultRelInfo->ri_RelationDesc->rd_rel->relowner,
								  &ucxt);
		ExecBSTruncateTriggers(estate, resultRelInfo);
		if (run_as_table_owner)
			RestoreUserContext(&ucxt);
		resultRelInfo++;
	}

	/*
	 * OK, truncate each table.
	 */
	mySubid = GetCurrentSubTransactionId();

	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		/* 跳过分区表，因为无需做任何操作 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			continue;

		/*
		 * Build the lists of foreign tables belonging to each foreign server and
		 * pass each list to the foreign data wrapper's 回调 function, 从而 each server
		 * can truncate its all foreign tables in bulk. Each list is saved as a
		 * single entry in a 哈希 table that uses the server OID as lookup key.
		 *
		 */
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			Oid			serverid = GetForeignServerIdByRelId(RelationGetRelid(rel));
			bool		found;
			ForeignTruncateInfo *ft_info;

			/* First time through, initialize hash表 for foreign 表s */
			if (!ft_htab)
			{
				HASHCTL		hctl;

				memset(&hctl, 0, sizeof(HASHCTL));
				hctl.keysize = sizeof(Oid);
				hctl.entrysize = sizeof(ForeignTruncateInfo);
				hctl.hcxt = CurrentMemoryContext;

				ft_htab = hash_create("TRUNCATE for Foreign Tables",
									  32,	/* 从小开始，按需扩展 */
									  &hctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}

			/* 查找或创建外表的缓存条目 */
			ft_info = hash_search(ft_htab, &serverid, HASH_ENTER, &found);
			if (!found)
			{
				ft_info->serverid = serverid;
				ft_info->rels = NIL;
			}

			/*
			 * Save the 外表 in the entry of the server that the 外表 belongs to.
			 *
			 */
			ft_info->rels = lappend(ft_info->rels, rel);
			continue;
		}

		/*
		 * Normally, we need a 事务-safe truncation here. However, if the table was
		 * either created in the current (sub)事务 or has a new relfilenumber in the
		 * current (sub)事务, then 我们可以 just truncate it in-place, 因为 a 回滚 would cause
		 * the whole table or the current physical file to be thrown away anyway.
		 *
		 */
		if (rel->rd_createSubid == mySubid ||
			rel->rd_newRelfilelocatorSubid == mySubid)
		{
			/* 立即、不可回滚的截断是可以的 */
			heap_truncate_one_rel(rel);
		}
		else
		{
			Oid			heap_relid;
			Oid			toast_relid;
			ReindexParams reindex_params = {0};

			/*
			 * This effectively 删除 all rows in the table, and 可能 done in a 可序列化 事务. In
			 * that case 我们必须 record a rw-conflict in to this 事务 from each 事务 holding a
			 * predicate 锁 on the table.
			 *
			 */
			CheckTableForSerializableConflictIn(rel);

			/*
			 * 需要完整的事务安全操作。   
			 *
			 * 为该关系创建新的空存储文件，并将其指定为   
			 * relfilenumber 值。旧存储文件被安排                 
			 * 在提交时删除。  
			 */
			RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);

			heap_relid = RelationGetRelid(rel);

			/*
			 * The same for the TOAST 表, 如有.
			 */
			toast_relid = rel->rd_rel->reltoastrelid;
			if (OidIsValid(toast_relid))
			{
				Relation	toastrel = relation_open(toast_relid,
													 AccessExclusiveLock);

				RelationSetNewRelfilenumber(toastrel,
											toastrel->rd_rel->relpersistence);
				table_close(toastrel, NoLock);
			}

			/*
			 * Reconstruct the 索引 to match, and we're done.
			 */
			reindex_relation(heap_relid, REINDEX_REL_PROCESS_TOAST,
							 &reindex_params);
		}

		pgstat_count_truncate(rel);
	}

	/* Now go through the hash 表, and t运行cate foreign 表s */
	if (ft_htab)
	{
		ForeignTruncateInfo *ft_info;
		HASH_SEQ_STATUS seq;

		hash_seq_init(&seq, ft_htab);

		PG_TRY();
		{
			while ((ft_info = hash_seq_search(&seq)) != NULL)
			{
				FdwRoutine *routine = GetFdwRoutineByServerId(ft_info->serverid);

				/* truncate_check_rel() 已检查过 */
				Assert(routine->ExecForeignTruncate != NULL);

				routine->ExecForeignTruncate(ft_info->rels,
											 behavior,
											 restart_seqs);
			}
		}
		PG_FINALLY();
		{
			hash_destroy(ft_htab);
		}
		PG_END_TRY();
	}

	/*
	 * Restart owned 序列 if we were asked to.
	 */
	foreach(cell, seq_relids)
	{
		Oid			seq_relid = lfirst_oid(cell);

		ResetSequence(seq_relid);
	}

	/*
	 * 写一条 WAL 记录，使这组操作可被逻辑解码。    
	 *         
	 *
	 * 组装 relids 数组，这样我们可以为整个操作写入单个   
	 * WAL 记录。
	 */
	if (relids_logged != NIL)
	{
		xl_heap_truncate xlrec;
		int			i = 0;

		/* 仅在 wal_level >= logical 时才应到达此处 */
		Assert(XLogLogicalInfoActive());

		logrelids = palloc(list_length(relids_logged) * sizeof(Oid));
		foreach(cell, relids_logged)
			logrelids[i++] = lfirst_oid(cell);

		xlrec.dbId = MyDatabaseId;
		xlrec.nrelids = list_length(relids_logged);
		xlrec.flags = 0;
		if (behavior == DROP_CASCADE)
			xlrec.flags |= XLH_TRUNCATE_CASCADE;
		if (restart_seqs)
			xlrec.flags |= XLH_TRUNCATE_RESTART_SEQS;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapTruncate);
		XLogRegisterData((char *) logrelids, list_length(relids_logged) * sizeof(Oid));

		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		(void) XLogInsert(RM_HEAP_ID, XLOG_HEAP_TRUNCATE);
	}

	/*
	 * Process all AFTER STATEMENT TRUNCATE 触发器.
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		UserContext ucxt;

		if (run_as_table_owner)
			SwitchToUntrustedUser(resultRelInfo->ri_RelationDesc->rd_rel->relowner,
								  &ucxt);
		ExecASTruncateTriggers(estate, resultRelInfo);
		if (run_as_table_owner)
			RestoreUserContext(&ucxt);
		resultRelInfo++;
	}

	/* Handle 队列d AFTER 触发器s */
	AfterTriggerEndQuery(estate);

	/* We can clean up the EState 现在 */
	FreeExecutorState(estate);

	/*
	 * Close any rels opened by CASCADE (can't do this while EState still
	 * holds refs)
	 */
	rels = list_difference_ptr(rels, explicit_rels);
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * 检查 a given relation is safe to truncate. Subroutine for ExecuteTruncate()
 * and RangeVarCallbackForTruncate().
 */
static void
truncate_check_rel(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);

	/*
	 * Only allow truncate on regular tables, foreign tables using foreign data
	 * wrappers supporting TRUNCATE and partitioned tables (although, the latter
	 * are 仅 being included here for the 后面的 检查; no physical truncation will
	 * occur in their case.).
	 */
	if (reltuple->relkind == RELKIND_FOREIGN_TABLE)
	{
		Oid			serverid = GetForeignServerIdByRelId(relid);
		FdwRoutine *fdwroutine = GetFdwRoutineByServerId(serverid);

		if (!fdwroutine->ExecForeignTruncate)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot truncate foreign table \"%s\"",
							relname)));
	}
	else if (reltuple->relkind != RELKIND_RELATION &&
			 reltuple->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", relname)));

	/*
	 * Most system 目录 can't be truncated at all, or 至少 not unless
	 * allow_system_table_mods=on. As an exception, 然而, we allow pg_largeobject
	 * to be truncated as part of pg_upgrade, 因为 我们需要 change its relfilenode to
	 * match the old cluster, and allowing a TRUNCATE command to be executed is
	 * the easiest way of doing that.
	 */
	if (!allowSystemTableMods && IsSystemClass(relid, reltuple)
		&& (!IsBinaryUpgrade || relid != LargeObjectRelationId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						relname)));

	InvokeObjectTruncateHook(relid);
}

/*
 * 检查 current 用户 has the 权限 to truncate given relation.
 */
static void
truncate_check_perms(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);
	AclResult	aclresult;

	/* 权限检查 */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(reltuple->relkind),
					   relname);
}

/*
 * Set of extra sanity 检查 to check if a given relation is safe to truncate.
 * This is split with truncate_check_rel() as RangeVarCallbackForTruncate()
 * cannot open a Relation yet.
 */
static void
truncate_check_activity(Relation rel)
{
	/*
	 * Don't allow truncate on temp tables of other 后端 ... their local 缓冲区
	 * manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current 事务, including
	 * open 扫描 and pending AFTER 触发器 events.
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*
 * storage_name 返回 the name corresponding to a typstorage/attstorage enum
 * value
 */
static const char *
storage_name(char c)
{
	switch (c)
	{
		case TYPSTORAGE_PLAIN:
			return "PLAIN";
		case TYPSTORAGE_EXTERNAL:
			return "EXTERNAL";
		case TYPSTORAGE_EXTENDED:
			return "EXTENDED";
		case TYPSTORAGE_MAIN:
			return "MAIN";
		default:
			return "???";
	}
}

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of OIDs of parent relations, already locked by caller.
 * 'relpersistence' is the persistence type of the table.
 * 'is_partition' tells if the table is a partition.
 *
 * Output arguments:
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 *
 * Return value:
 * Completed schema list.
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 *	   Constraints (including NOT NULL constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *
 *		Note that the default-value infrastructure is used for generated
 *		columns' expressions too, so most of the preceding paragraph applies
 *		to generation expressions too.  We insist that a child column be
 *		generated if and only if its parent(s) are, but it need not have
 *		the same generation expression.
 *----------
 */
static List *
MergeAttributes(List *schema, List *supers, char relpersistence,
				bool is_partition, List **supconstr)
{
	List	   *inhSchema = NIL;
	List	   *constraints = NIL;
	bool		have_bogus_defaults = false;
	int			child_attno;
	static Node bogus_marker = {0}; /* 标记冲突的默认值 */
	List	   *saved_schema = NIL;
	ListCell   *entry;

	/*
	 * Check for and reject tables with too many columns. We perform this check
	 * relatively early for two reasons: (a) 我们不 run the risk of overflowing an
	 * AttrNumber in subsequent code (b) an O(n^2) algorithm is okay if we're
	 * processing <= 1600 columns, but could take minutes to 执行 if the 用户
	 * attempts to create a table with hundreds of thousands of columns. 请注意 we 也
	 * need to 检查 we do not exceed this figure 之后 including columns from 继承的
	 * relations.
	 *
	 *
	 */
	if (list_length(schema) > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * Check for duplicate names in the explicit list of 属性. Although we might
	 * consider merging such entries in the same way that we handle name
	 * conflicts for 继承的 属性, it seems to make more sense to assume such conflicts
	 * are errors. 我们不 use foreach() here 因为 we have two nested loops over the 模式
	 * list, with possible element deletions in the inner one. If we used
	 * foreach_delete_current() it could 仅 fix up the state of one of the loops,
	 * so it seems cleaner to use looping over list 索引 for both loops. 请注意 any
	 * deletion will happen beyond where the outer loop is, so its 索引 从不 needs
	 * adjustment.
	 *
	 *
	 *
	 */
	for (int coldefpos = 0; coldefpos < list_length(schema); coldefpos++)
	{
		ColumnDef  *coldef = list_nth_node(ColumnDef, schema, coldefpos);

		if (!is_partition && coldef->typeName == NULL)
		{
			/*
			 * Typed table column option that does not belong to a column from the
			 * type. This works 因为 the columns from the type come first in the list.
			 * (We omit this check for partition column lists; those are processed
			 * separately 以下.)
			 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							coldef->colname)));
		}

		/* restpos 扫描 coldef 之后的所有条目；增量在循环体中 */
		for (int restpos = coldefpos + 1; restpos < list_length(schema);)
		{
			ColumnDef  *restdef = list_nth_node(ColumnDef, schema, restpos);

			if (strcmp(coldef->colname, restdef->colname) == 0)
			{
				if (coldef->is_from_type)
				{
					/*
					 * merge the column options into the column from the type
					 */
					coldef->is_not_null = restdef->is_not_null;
					coldef->raw_default = restdef->raw_default;
					coldef->cooked_default = restdef->cooked_default;
					coldef->constraints = restdef->constraints;
					coldef->is_from_type = false;
					schema = list_delete_nth_cell(schema, restpos);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									coldef->colname)));
			}
			else
				restpos++;
		}
	}

	/*
	 * In case of a partition, there are no new 列定义, 仅 dummy ColumnDefs created
	 * for column 约束. Set them aside for now and process them at the end.
	 *
	 */
	if (is_partition)
	{
		saved_schema = schema;
		schema = NIL;
	}

	/*
	 * Scan the parents left-to-right, and merge their 属性 to form a list of 继承的
	 * 属性 (inhSchema). Also check to see if 我们需要 继承 an OID column.
	 *
	 */
	child_attno = 0;
	foreach(entry, supers)
	{
		Oid			parent = lfirst_oid(entry);
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrMap    *newattmap;
		List	   *inherited_defaults;
		List	   *cols_with_defaults;
		AttrNumber	parent_attno;
		ListCell   *lc1;
		ListCell   *lc2;

		/* c所有er 已经 got 锁 */
		relation = table_open(parent, NoLock);

		/*
		 * Check for active uses of the 父分区表 in the current 事务, 例如 being used in
		 * some manner by an enclosing command.
		 *
		 */
		if (is_partition)
			CheckTableNotInUse(relation, "CREATE TABLE .. PARTITION OF");

		/*
		 * 我们不 allow partitioned tables and partitions to participate in regular 继承.
		 *
		 */
		if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
			!is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from partitioned table \"%s\"",
							RelationGetRelationName(relation))));
		if (relation->rd_rel->relispartition && !is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from partition \"%s\"",
							RelationGetRelationName(relation))));

		if (relation->rd_rel->relkind != RELKIND_RELATION &&
			relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
			relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table or foreign table",
							RelationGetRelationName(relation))));

		/*
		 * If the parent is permanent, so 必须是 all of its partitions. 请注意 继承 允许 that
		 * case.
		 */
		if (is_partition &&
			relation->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
			relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create a temporary relation as partition of permanent relation \"%s\"",
							RelationGetRelationName(relation))));

		/* 永久关系不能从临时关系继承 */
		if (relpersistence != RELPERSISTENCE_TEMP &&
			relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation \"%s\""
							: "cannot create a permanent relation as partition of temporary relation \"%s\"",
							RelationGetRelationName(relation))));

		/* 如果现有关系是临时的，它必须属于此会话 */
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
			!relation->rd_islocaltemp)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation of another session"
							: "cannot create as partition of temporary relation of another session")));

		/*
		 * 我们应该 have an UNDER 权限 flag for this, but for now, demand that 创建者 of a 子表
		 * own the parent.
		 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(relation), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattmap->attnums[] will contain the child-table 属性 numbers for the 属性
		 * of this 父表. (They are not the same for parents 之后 the first one, nor if
		 * we have 已删除列.)
		 */
		newattmap = make_attrmap(tupleDesc->natts);

		/* 在 newattmap 完成之前无法处理继承的默认值。 */
		inherited_defaults = cols_with_defaults = NIL;

		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
														parent_attno - 1);
			char	   *attributeName = NameStr(attribute->attname);
			int			exist_attno;
			ColumnDef  *def;

			/*
			 * Ignore 已删除列 in the parent.
			 */
			if (attribute->attisdropped)
				continue;		/* 将 newattmap->attnums 条目保留为零 */

			/*
			 * Does it conflict with some 先前 继承的 column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				Oid			defTypeId;
				int32		deftypmod;
				Oid			defCollId;

				/*
				 * Yes, try to merge the two 列定义.
				 */
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);

				/*
				 * Must have the same type and typmod
				 */
				typenameTypeIdAndMod(NULL, def->typeName, &defTypeId, &deftypmod);
				if (defTypeId != attribute->atttypid ||
					deftypmod != attribute->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("inherited column \"%s\" has a type conflict",
									attributeName),
							 errdetail("%s versus %s",
									   format_type_with_typemod(defTypeId,
																deftypmod),
									   format_type_with_typemod(attribute->atttypid,
																attribute->atttypmod))));

				/*
				 * Must have the same 排序规则
				 */
				defCollId = GetColumnDefCollation(NULL, def, defTypeId);
				if (defCollId != attribute->attcollation)
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
							 errmsg("inherited column \"%s\" has a collation conflict",
									attributeName),
							 errdetail("\"%s\" versus \"%s\"",
									   get_collation_name(defCollId),
									   get_collation_name(attribute->attcollation))));

				/*
				 * Copy/check 存储 parameter
				 */
				if (def->storage == 0)
					def->storage = attribute->attstorage;
				else if (def->storage != attribute->attstorage)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("inherited column \"%s\" has a storage parameter conflict",
									attributeName),
							 errdetail("%s versus %s",
									   storage_name(def->storage),
									   storage_name(attribute->attstorage))));

				/*
				 * Copy/check 压缩 parameter
				 */
				if (CompressionMethodIsValid(attribute->attcompression))
				{
					const char *compression =
						GetCompressionMethodName(attribute->attcompression);

					if (def->compression == NULL)
						def->compression = pstrdup(compression);
					else if (strcmp(def->compression, compression) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("column \"%s\" has a compression method conflict",
										attributeName),
								 errdetail("%s versus %s", def->compression, compression)));
				}

				/*
				 * Merge of NOT NULL 约束 = OR 'em together
				 */
				def->is_not_null |= attribute->attnotnull;

				/*
				 * Check for GENERATED conflicts
				 */
				if (def->generated != attribute->attgenerated)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("inherited column \"%s\" has a generation conflict",
									attributeName)));

				/*
				 * Default and other 约束 are handled 以下
				 */

				def->inhcount++;
				if (def->inhcount < 0)
					ereport(ERROR,
							errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("too many inheritance parents"));

				newattmap->attnums[parent_attno - 1] = exist_attno;
			}
			else
			{
				/*
				 * No, create a new 继承的 column
				 */
				def = makeNode(ColumnDef);
				def->colname = pstrdup(attributeName);
				def->typeName = makeTypeNameFromOid(attribute->atttypid,
													attribute->atttypmod);
				def->inhcount = 1;
				def->is_local = false;
				def->is_not_null = attribute->attnotnull;
				def->is_from_type = false;
				def->storage = attribute->attstorage;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->generated = attribute->attgenerated;
				def->collClause = NULL;
				def->collOid = attribute->attcollation;
				def->constraints = NIL;
				def->location = -1;
				if (CompressionMethodIsValid(attribute->attcompression))
					def->compression =
						pstrdup(GetCompressionMethodName(attribute->attcompression));
				else
					def->compression = NULL;
				inhSchema = lappend(inhSchema, def);
				newattmap->attnums[parent_attno - 1] = ++child_attno;
			}

			/*
			 * Locate default/生成表达式 如有
			 */
			if (attribute->atthasdef)
			{
				Node	   *this_default = NULL;

				/* 在约束结构中查找默认值 */
				if (constr != NULL)
				{
					AttrDefault *attrdef = constr->defval;

					for (int i = 0; i < constr->num_defval; i++)
					{
						if (attrdef[i].adnum == parent_attno)
						{
							this_default = stringToNode(attrdef[i].adbin);
							break;
						}
					}
				}
				if (this_default == NULL)
					elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
						 parent_attno, RelationGetRelationName(relation));

				/*
				 * If it's a GENERATED default, it might contain Vars that need to be
				 * mapped to the 继承的 column(s)' new numbers. 我们可以't do that till newattmap
				 * is ready, so just remember all the 继承的 default expressions for the
				 * moment.
				 */
				inherited_defaults = lappend(inherited_defaults, this_default);
				cols_with_defaults = lappend(cols_with_defaults, def);
			}
		}

		/*
		 * Now process any 继承的 default expressions, adjusting attnos using the
		 * completed newattmap map.
		 */
		forboth(lc1, inherited_defaults, lc2, cols_with_defaults)
		{
			Node	   *this_default = (Node *) lfirst(lc1);
			ColumnDef  *def = (ColumnDef *) lfirst(lc2);
			bool		found_whole_row;

			/* Ad仅 Vars to match 新 表's 列 编号ing */
			this_default = map_variable_attnos(this_default,
											   1, 0,
											   newattmap,
											   InvalidOid, &found_whole_row);

			/*
			 * For the moment we have to reject 全行变量. We could convert them, if we knew
			 * the new table's rowtype OID, but that hasn't been assigned yet. (A
			 * variable could 仅 appear in a 生成表达式, so the error message is correct.)
			 *
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
								   def->colname,
								   RelationGetRelationName(relation))));

			/*
			 * If we 已经 had a default from some prior parent, check to see if they are
			 * the same. If so, no problem; 如果不是, mark the column as having a bogus
			 * default. Below, we will complain if the bogus default isn't overridden
			 * by the child 模式.
			 */
			Assert(def->raw_default == NULL);
			if (def->cooked_default == NULL)
				def->cooked_default = this_default;
			else if (!equal(def->cooked_default, this_default))
			{
				def->cooked_default = &bogus_marker;
				have_bogus_defaults = true;
			}
		}

		/*
		 * Now copy the CHECK 约束 of this parent, adjusting attnos using the
		 * completed newattmap map. Identically named 约束 are merged if possible,
		 * else we throw error.
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				char	   *name = check[i].ccname;
				Node	   *expr;
				bool		found_whole_row;

				/* 如果约束不可继承则忽略 */
				if (check[i].ccnoinherit)
					continue;

				/* Ad仅 Vars to match 新 表's 列 编号ing */
				expr = map_variable_attnos(stringToNode(check[i].ccbin),
										   1, 0,
										   newattmap,
										   InvalidOid, &found_whole_row);

				/*
				 * For the moment we have to reject 全行变量. We could convert them, if we
				 * knew the new table's rowtype OID, but that hasn't been assigned yet.
				 *
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
									   name,
									   RelationGetRelationName(relation))));

				/* 检查重复 */
				if (!MergeCheckConstraint(constraints, name, expr))
				{
					/* 不是，这是新的 */
					CookedConstraint *cooked;

					cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
					cooked->contype = CONSTR_CHECK;
					cooked->conoid = InvalidOid;	/* 直到创建 */
					cooked->name = pstrdup(name);
					cooked->attnum = 0; /* 不用于约束 */
					cooked->expr = expr;
					cooked->skip_validation = false;
					cooked->is_local = false;
					cooked->inhcount = 1;
					cooked->is_no_inherit = false;
					constraints = lappend(constraints, cooked);
				}
			}
		}

		free_attrmap(newattmap);

		/*
		 * Close the parent rel, but keep our 锁 on it until 事务 提交. That will prevent
		 * someone else from deleting or ALTERing the parent 之前 the child is
		 * committed.
		 */
		table_close(relation, NoLock);
	}

	/*
	 * If we had no 继承的 属性, the result 模式 is just the explicitly declared
	 * columns. Otherwise, 我们需要 merge the declared columns into the 继承的 模式 list.
	 * Although, we 从不 have any explicitly declared columns if the table is a
	 * partition.
	 */
	if (inhSchema != NIL)
	{
		int			schema_attno = 0;

		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			schema_attno++;

			/*
			 * Does it conflict with some 先前 继承的 column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				ColumnDef  *def;
				Oid			defTypeId,
							newTypeId;
				int32		deftypmod,
							newtypmod;
				Oid			defcollid,
							newcollid;

				/*
				 * Partitions have 仅 one parent and have no 列定义 of their own, so conflict
				 * should 从不 occur.
				 */
				Assert(!is_partition);

				/*
				 * Yes, try to merge the two 列定义.
				 */
				if (exist_attno == schema_attno)
					ereport(NOTICE,
							(errmsg("merging column \"%s\" with inherited definition",
									attributeName)));
				else
					ereport(NOTICE,
							(errmsg("moving and merging column \"%s\" with inherited definition", attributeName),
							 errdetail("User-specified column moved to the position of the inherited column.")));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);

				/*
				 * Must have the same type and typmod
				 */
				typenameTypeIdAndMod(NULL, def->typeName, &defTypeId, &deftypmod);
				typenameTypeIdAndMod(NULL, newdef->typeName, &newTypeId, &newtypmod);
				if (defTypeId != newTypeId || deftypmod != newtypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" has a type conflict",
									attributeName),
							 errdetail("%s versus %s",
									   format_type_with_typemod(defTypeId,
																deftypmod),
									   format_type_with_typemod(newTypeId,
																newtypmod))));

				/*
				 * Must have the same 排序规则
				 */
				defcollid = GetColumnDefCollation(NULL, def, defTypeId);
				newcollid = GetColumnDefCollation(NULL, newdef, newTypeId);
				if (defcollid != newcollid)
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
							 errmsg("column \"%s\" has a collation conflict",
									attributeName),
							 errdetail("\"%s\" versus \"%s\"",
									   get_collation_name(defcollid),
									   get_collation_name(newcollid))));

				/*
				 * Identity is 从不 继承的. The new column can have an identity definition, so
				 * we 始终 just take that one.
				 */
				def->identity = newdef->identity;

				/*
				 * Copy 存储 parameter
				 */
				if (def->storage == 0)
					def->storage = newdef->storage;
				else if (newdef->storage != 0 && def->storage != newdef->storage)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" has a storage parameter conflict",
									attributeName),
							 errdetail("%s versus %s",
									   storage_name(def->storage),
									   storage_name(newdef->storage))));

				/*
				 * Copy 压缩 parameter
				 */
				if (def->compression == NULL)
					def->compression = newdef->compression;
				else if (newdef->compression != NULL)
				{
					if (strcmp(def->compression, newdef->compression) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("column \"%s\" has a compression method conflict",
										attributeName),
								 errdetail("%s versus %s", def->compression, newdef->compression)));
				}

				/*
				 * Merge of NOT NULL 约束 = OR 'em together
				 */
				def->is_not_null |= newdef->is_not_null;

				/*
				 * Check for conflicts related to 生成列. If the parent column is generated,
				 * the child column 将被 made a 生成列 if it isn't 已经. If it is a 生成列, we'll
				 * take its 生成表达式 in preference to the parent's. 我们必须 检查 the child column
				 * doesn't specify a 默认值 or identity, which matches the rules for a single
				 * column in parse_utilcmd.c. Conversely, if the parent column is not
				 * generated, the child column can't be either. (We used to allow that,
				 * but it results in being able to override the 生成表达式 via UPDATEs through
				 * the parent.)
				 *
				 *
				 *
				 *
				 *
				 */
				if (def->generated)
				{
					if (newdef->raw_default && !newdef->generated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
								 errmsg("column \"%s\" inherits from generated column but specifies default",
										def->colname)));
					if (newdef->identity)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
								 errmsg("column \"%s\" inherits from generated column but specifies identity",
										def->colname)));
				}
				else
				{
					if (newdef->generated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
								 errmsg("child column \"%s\" specifies generation expression",
										def->colname),
								 errhint("A child table column cannot be generated unless its parent column is.")));
				}

				/*
				 * If new def has a default, override previous default
				 */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}

				/* 将列标记为本地定义 */
				def->is_local = true;
			}
			else
			{
				/*
				 * No, attach new column to result 模式
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;

		/*
		 * 检查 we haven't exceeded the legal # of columns 之后 merging in 继承的 columns.
		 *
		 */
		if (list_length(schema) > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	/*
	 * Now that we have the 列定义 list for a partition, 我们可以 check whether the
	 * columns referenced in the 列约束 specs actually exist. Also, we merge
	 * parent's NOT NULL 约束 and defaults into each corresponding 列定义.
	 *
	 */
	if (is_partition)
	{
		foreach(entry, saved_schema)
		{
			ColumnDef  *restdef = lfirst(entry);
			bool		found = false;
			ListCell   *l;

			foreach(l, schema)
			{
				ColumnDef  *coldef = lfirst(l);

				if (strcmp(coldef->colname, restdef->colname) == 0)
				{
					found = true;
					coldef->is_not_null |= restdef->is_not_null;

					/*
					 * 检查与生成列相关的冲突。             
					 *
					 * 规则同上：生成性必须与父分区匹配， 
					 * 但生成表达式的内容                          
					 * 可以不同。  
					 */
					if (coldef->generated)
					{
						if (restdef->raw_default && !restdef->generated)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("column \"%s\" inherits from generated column but specifies default",
											restdef->colname)));
						if (restdef->identity)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("column \"%s\" inherits from generated column but specifies identity",
											restdef->colname)));
					}
					else
					{
						if (restdef->generated)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("child column \"%s\" specifies generation expression",
											restdef->colname),
									 errhint("A child table column cannot be generated unless its parent column is.")));
					}

					/*
					 * Override the parent's 默认值 for this column (coldef->cooked_default)
					 * with the partition's local definition (restdef->raw_default), if
					 * there's one. It 应是 physically impossible to get a cooked default in
					 * the local definition or a raw default in the 继承的 definition, but 确保
					 * they're nulls, for future-proofing.
					 *
					 *
					 */
					Assert(restdef->cooked_default == NULL);
					Assert(coldef->raw_default == NULL);
					if (restdef->raw_default)
					{
						coldef->raw_default = restdef->raw_default;
						coldef->cooked_default = NULL;
					}
				}
			}

			/* 对不在父表中的列上的约束报错 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist",
								restdef->colname)));
		}
	}

	/*
	 * If we found any conflicting parent 默认值, check to 确保 they were overridden
	 * by the child.
	 */
	if (have_bogus_defaults)
	{
		foreach(entry, schema)
		{
			ColumnDef  *def = lfirst(entry);

			if (def->cooked_default == &bogus_marker)
			{
				if (def->generated)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
							 errmsg("column \"%s\" inherits conflicting generation expressions",
									def->colname),
							 errhint("To resolve the conflict, specify a generation expression explicitly.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
							 errmsg("column \"%s\" inherits conflicting default values",
									def->colname),
							 errhint("To resolve the conflict, specify a default explicitly.")));
			}
		}
	}

	*supconstr = constraints;
	return schema;
}


/*
 * MergeCheckConstraint
 *		尝试将继承的 CHECK 约束与之前的约束合并      
 *
 * 如果从多个父分区继承同名约束，则必须合并它们，     
 * 或者如果它们的定义不相同，则抛出错误。              
 *
 * constraints 是先前约束的 CookedConstraint 结构体列表。         
 *
 * 若已合并（约束为重复）则返回 true；若名称至今   
 * 唯一则返回 false；若冲突则抛出错误。   
 */
static bool
MergeCheckConstraint(List *constraints, char *name, Node *expr)
{
	ListCell   *lc;

	foreach(lc, constraints)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lc);

		Assert(ccon->contype == CONSTR_CHECK);

		/* 名称不匹配永不冲突 */
		if (strcmp(ccon->name, name) != 0)
			continue;

		if (equal(expr, ccon->expr))
		{
			/* 可以合并 */
			ccon->inhcount++;
			if (ccon->inhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			return true;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("check constraint name \"%s\" appears multiple times but with different expressions",
						name)));
	}

	return false;
}


/*
 * StoreCatalogInheritance Updates the system 目录 with proper 继承 information.
 * supers is a list of the OID of the new relation's direct ancestors.
 *
 *
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers,
						bool child_is_partition)
{
	Relation	relation;
	int32		seqNumber;
	ListCell   *entry;

	/*
	 * sanity 检查
	 */
	Assert(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Store INHERITS information in pg_inherits using direct ancestors 仅. Also
	 * enter 依赖 on the direct ancestors, and 确保 they are marked with
	 * relhassubclass = true. (Once upon a time, both direct and indirect
	 * ancestors were found here and then entered into pg_ipl. Since that 目录
	 * doesn't exist anymore, there's no need to look for indirect ancestors.)
	 *
	 *
	 */
	relation = table_open(InheritsRelationId, RowExclusiveLock);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			parentOid = lfirst_oid(entry);

		StoreCatalogInheritance1(relationId, parentOid, seqNumber, relation,
								 child_is_partition);
		seqNumber++;
	}

	table_close(relation, RowExclusiveLock);
}

/*
 * Make 目录 entries showing relationId as being an 继承 child of parentOid.
 * inhRelation is the 已经-opened pg_inherits 目录.
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int32 seqNumber, Relation inhRelation,
						 bool child_is_partition)
{
	ObjectAddress childobject,
				parentobject;

	/* 存储 pg_inherits 行 */
	StoreSingleInheritance(relationId, parentOid, seqNumber);

	/*
	 * Store a 依赖 too
	 */
	parentobject.classId = RelationRelationId;
	parentobject.objectId = parentOid;
	parentobject.objectSubId = 0;
	childobject.classId = RelationRelationId;
	childobject.objectId = relationId;
	childobject.objectSubId = 0;

	recordDependencyOn(&childobject, &parentobject,
					   child_dependency_type(child_is_partition));

	/*
	 * Post creation hook of this 继承. Since object_access_hook doesn't take
	 * multiple object identifiers, we relay oid of 父关系 using auxiliary_id
	 * argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 relationId, 0,
								 parentOid, false);

	/*
	 * Mark the parent as having subclasses.
	 */
	SetRelationHasSubclass(parentOid, true);
}

/*
 * 查找具有给定名称的现有模式项。         
 *
 * 若属性已存在于模式中，则返回其索引（从 1 开始），  
 * 否则为 0。  
 */
static int
findAttrByName(const char *attributeName, List *schema)
{
	ListCell   *s;
	int			i = 1;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		if (strcmp(attributeName, def->colname) == 0)
			return i;

		i++;
	}
	return 0;
}


/*
 * SetRelationHasSubclass Set the value of the relation's relhassubclass field
 * in pg_class. It's 始终 safe to set this field to true, 因为 all SQL commands
 * are ready to see true and then find no children. On the other hand,
 * commands generally assume zero children if this is false. Caller must hold
 * any self-排他锁 until end of 事务. If the new value is false, caller must have
 * acquired that 锁 之前 reading the evidence that justified the false value.
 * That way, it properly waits if another 后端 is simultaneously concluding no
 * need to change the 元组 (new and old values are true). NOTE: an important
 * side-effect of this operation is that an SI invalidation message is sent
 * out to all 后端 --- including me --- causing plans referencing the relation
 * to be 重建 with the new list of children. This must happen even if we find
 * that no change 是必需的 in the pg_class row.
 *
 *
 *
 *
 *
 *
 */
void
SetRelationHasSubclass(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classtuple;

	Assert(CheckRelationOidLockedByMe(relationId,
									  ShareUpdateExclusiveLock, false) ||
		   CheckRelationOidLockedByMe(relationId,
									  ShareRowExclusiveLock, true));

	/*
	 * Fetch a modifiable copy of the 元组, modify it, update pg_class.
	 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classtuple = (Form_pg_class) GETSTRUCT(tuple);

	if (classtuple->relhassubclass != relhassubclass)
	{
		classtuple->relhassubclass = relhassubclass;
		CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);
	}
	else
	{
		/* 无需更改元组，但强制重建 relcache */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * CheckRelationTableSpaceMove
 *		检查关系是否可以移动到新表空间。 
 *
 * 注意：调用者必须持有关系的 AccessExclusiveLock。 
 *
 * 如果该关系可以移动到新表空间则返回 true；如果无法
 * 执行移动则抛出错误；如果移动不会产生效果则         
 * 返回 false。      
 */
bool
CheckRelationTableSpaceMove(Relation rel, Oid newTableSpaceId)
{
	Oid			oldTableSpaceId;

	/*
	 * No work if no change in 表空间. 请注意 MyDatabaseTableSpace is stored as 0.
	 *
	 */
	oldTableSpaceId = rel->rd_rel->reltablespace;
	if (newTableSpaceId == oldTableSpaceId ||
		(newTableSpaceId == MyDatabaseTableSpace && oldTableSpaceId == 0))
		return false;

	/*
	 * 我们可以not support moving mapped relations into different 表空间. (In particular
	 * this eliminates all shared 目录.)
	 */
	if (RelationIsMapped(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel))));

	/* 无法将非共享关系移入 pg_global */
	if (newTableSpaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Do not allow moving temp tables of other 后端 ... their local 缓冲区 manager is
	 * not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions")));

	return true;
}

/*
 * SetRelationTableSpace
 *		设置 pg_class 条目的 reltablespace/relfilenumber     
 *
 * newTableSpaceId 是该关系的新表空间，而          
 * newRelFilenumber 是其新文件号。若 newRelFilenumber  
 * 为 InvalidRelFileNumber，此字段不更新。
 *
 * 注意：调用者必须持有关系的 AccessExclusiveLock。 
 *
 * 本例程的调用者最好先检查该关系能否移动到    
 * 此新表空间：先调用 CheckRelationTableSpaceMove()，         
 * 并且由调用者负责通过 CommandCounterIncrement() 使
 * 更改可见。           
 */
void
SetRelationTableSpace(Relation rel,
					  Oid newTableSpaceId,
					  RelFileNumber newRelFilenumber)
{
	Relation	pg_class;
	HeapTuple	tuple;
	ItemPointerData otid;
	Form_pg_class rd_rel;
	Oid			reloid = RelationGetRelid(rel);

	Assert(CheckRelationTableSpaceMove(rel, newTableSpaceId));

	/* Get a modifiable 复制 of the 关系's pg_class 行. */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", reloid);
	otid = tuple->t_self;
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* 更新 pg_class 行。 */
	rd_rel->reltablespace = (newTableSpaceId == MyDatabaseTableSpace) ?
		InvalidOid : newTableSpaceId;
	if (RelFileNumberIsValid(newRelFilenumber))
		rd_rel->relfilenode = newRelFilenumber;
	CatalogTupleUpdate(pg_class, &otid, tuple);
	UnlockTuple(pg_class, &otid, InplaceUpdateTupleLock);

	/*
	 * Record 依赖 on 表空间. This is 仅 required for relations that have no physical
	 * 存储.
	 */
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		changeDependencyOnTablespace(RelationRelationId, reloid,
									 rd_rel->reltablespace);

	heap_freetuple(tuple);
	table_close(pg_class, RowExclusiveLock);
}

/*
 * renameatt_check			- basic sanity 检查 之前 属性 rename
 */
static void
renameatt_check(Oid myrelid, Form_pg_class classform, bool recursing)
{
	char		relkind = classform->relkind;

	if (classform->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename column of typed table")));

	/*
	 * Renaming the columns of 序列 or toast tables doesn't actually break anything
	 * from the system's point of view, since internal references are by attnum.
	 * But it doesn't seem right to allow 用户 to change names that are hardcoded
	 * into the system, hence the 后面的 restriction.
	 *
	 */
	if (relkind != RELKIND_RELATION &&
		relkind != RELKIND_VIEW &&
		relkind != RELKIND_MATVIEW &&
		relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX &&
		relkind != RELKIND_FOREIGN_TABLE &&
		relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename columns of relation \"%s\"",
						NameStr(classform->relname)),
				 errdetail_relkind_not_supported(relkind)));

	/*
	 * 权限 checking.  仅 the 属主 of a class can change its 模式.
	 */
	if (!object_ownercheck(RelationRelationId, myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(myrelid)),
					   NameStr(classform->relname));
	if (!allowSystemTableMods && IsSystemClass(myrelid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						NameStr(classform->relname))));
}

/*
 * renameatt_internal - workhorse for renameatt Return value is the 属性 number
 * in the 'myrelid' relation.
 *
 */
static AttrNumber
renameatt_internal(Oid myrelid,
				   const char *oldattname,
				   const char *newattname,
				   bool recurse,
				   bool recursing,
				   int expected_parents,
				   DropBehavior behavior)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	AttrNumber	attnum;

	/*
	 * Grab an 排他锁 on the target table, which we will NOT release until end of
	 * 事务.
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);
	renameatt_check(myrelid, RelationGetForm(targetrelation), recursing);

	/*
	 * 如果设置了 'recurse' 标志，我们就需要在继承自  
	 * 'relname' 的所有类中重命名此属性 (同样在             
	 * 'relname') 
	 *
	 * 任何权限问题或重复属性的问题都将导致整个事务  
	 * 中止，这正是我们想要的 -- 要么全做，要么不做。 
	 */
	if (recurse)
	{
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		/*
		 * we need the number of parents for each child 从而 the recursive calls to
		 * renameatt() can determine whether there are any parents outside the 继承
		 * hierarchy being processed.
		 */
		child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the 继承 hierarchy, so all
		 * we have to do is process all of the relids in the list that it 返回.
		 *
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);

			if (childrelid == myrelid)
				continue;
			/* 注意我们无需再次递归 */
			renameatt_internal(childrelid, oldattname, newattname, false, true, numparents, behavior);
		}
	}
	else
	{
		/*
		 * If we are told not to 递归, there had better not be any child tables; else
		 * the rename would put them out of step. expected_parents will 仅 be 0 if we
		 * are not 已经 递归中.
		 *
		 */
		if (expected_parents == 0 &&
			find_inheritance_children(myrelid, NoLock) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	/* 重命名复合类型的类型化表中的属性 */
	if (targetrelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		List	   *child_oids;
		ListCell   *lo;

		child_oids = find_typed_table_dependencies(targetrelation->rd_rel->reltype,
												   RelationGetRelationName(targetrelation),
												   behavior);

		foreach(lo, child_oids)
			renameatt_internal(lfirst_oid(lo), oldattname, newattname, true, true, 0, behavior);
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCacheCopyAttName(myrelid, oldattname);
	if (!HeapTupleIsValid(atttup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist",
						oldattname)));
	attform = (Form_pg_attribute) GETSTRUCT(atttup);

	attnum = attform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot rename system column \"%s\"",
						oldattname)));

	/*
	 * if the 属性 is 继承的, forbid the renaming. if this is a top-level call to
	 * renameatt(), then expected_parents 将被 0, so the effect of this code 将被 to
	 * prohibit the renaming if the 属性 is 继承的 at all. if this is a recursive call
	 * to renameatt(), expected_parents 将被 the number of parents the current
	 * relation has 之内 the 继承 hierarchy being processed, so we'll prohibit the
	 * renaming 仅 if there are additional parents from elsewhere.
	 *
	 */
	if (attform->attinhcount > expected_parents)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* 新名称不应已存在 */
	(void) check_for_column_name_collision(targetrelation, newattname, false);

	/* 应用更新 */
	namestrcpy(&(attform->attname), newattname);

	CatalogTupleUpdate(attrelation, &atttup->t_self, atttup);

	InvokeObjectPostAlterHook(RelationRelationId, myrelid, attnum);

	heap_freetuple(atttup);

	table_close(attrelation, RowExclusiveLock);

	relation_close(targetrelation, NoLock); /* 关闭 rel but 保持 锁 */

	return attnum;
}

/*
 * Perform 权限 and integrity 检查 之前 acquiring a relation 锁.
 */
static void
RangeVarCallbackForRenameAttribute(const RangeVar *rv, Oid relid, Oid oldrelid,
								   void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除 */
	form = (Form_pg_class) GETSTRUCT(tuple);
	renameatt_check(relid, form, false);
	ReleaseSysCache(tuple);
}

/*
 * renameatt - changes the name of an 属性 in a relation The returned
 * ObjectAddress is that of the renamed column.
 *
 */
ObjectAddress
renameatt(RenameStmt *stmt)
{
	Oid			relid;
	AttrNumber	attnum;
	ObjectAddress address;

	/* 锁 级别 taken 此处 should match re名称att_内部 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackForRenameAttribute,
									 NULL);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	attnum =
		renameatt_internal(relid,
						   stmt->subname,	/* 旧 att 名称 */
						   stmt->newname,	/* 新 att 名称 */
						   stmt->relation->inh, /* recursive? */
						   false,	/* 递归中? */
						   0,	/* expected inhcount */
						   stmt->behavior);

	ObjectAddressSubSet(address, RelationRelationId, relid, attnum);

	return address;
}

/*
 * same logic as renameatt_internal
 */
static ObjectAddress
rename_constraint_internal(Oid myrelid,
						   Oid mytypid,
						   const char *oldconname,
						   const char *newconname,
						   bool recurse,
						   bool recursing,
						   int expected_parents)
{
	Relation	targetrelation = NULL;
	Oid			constraintOid;
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	Assert(!myrelid || !mytypid);

	if (mytypid)
	{
		constraintOid = get_domain_constraint_oid(mytypid, oldconname, false);
	}
	else
	{
		targetrelation = relation_open(myrelid, AccessExclusiveLock);

		/*
		 * don't tell it whether we're 递归中; we allow changing typed tables here
		 *
		 */
		renameatt_check(myrelid, RelationGetForm(targetrelation), false);

		constraintOid = get_relation_constraint_oid(myrelid, oldconname, false);
	}

	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u",
			 constraintOid);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	if (myrelid && con->contype == CONSTRAINT_CHECK && !con->connoinherit)
	{
		if (recurse)
		{
			List	   *child_oids,
					   *child_numparents;
			ListCell   *lo,
					   *li;

			child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
											 &child_numparents);

			forboth(lo, child_oids, li, child_numparents)
			{
				Oid			childrelid = lfirst_oid(lo);
				int			numparents = lfirst_int(li);

				if (childrelid == myrelid)
					continue;

				rename_constraint_internal(childrelid, InvalidOid, oldconname, newconname, false, true, numparents);
			}
		}
		else
		{
			if (expected_parents == 0 &&
				find_inheritance_children(myrelid, NoLock) != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("inherited constraint \"%s\" must be renamed in child tables too",
								oldconname)));
		}

		if (con->coninhcount > expected_parents)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot rename inherited constraint \"%s\"",
							oldconname)));
	}

	if (con->conindid
		&& (con->contype == CONSTRAINT_PRIMARY
			|| con->contype == CONSTRAINT_UNIQUE
			|| con->contype == CONSTRAINT_EXCLUSION))
		/* re名称 the 索引; this re名称s the 约束 也 */
		RenameRelationInternal(con->conindid, newconname, false, true);
	else
		RenameConstraintById(constraintOid, newconname);

	ObjectAddressSet(address, ConstraintRelationId, constraintOid);

	ReleaseSysCache(tuple);

	if (targetrelation)
	{
		/*
		 * Invalidate relcache so as others can see the new 约束 name.
		 */
		CacheInvalidateRelcache(targetrelation);

		relation_close(targetrelation, NoLock); /* 关闭 rel but 保持 锁 */
	}

	return address;
}

ObjectAddress
RenameConstraint(RenameStmt *stmt)
{
	Oid			relid = InvalidOid;
	Oid			typid = InvalidOid;

	if (stmt->renameType == OBJECT_DOMCONSTRAINT)
	{
		Relation	rel;
		HeapTuple	tup;

		typid = typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, stmt->object)));
		rel = table_open(TypeRelationId, RowExclusiveLock);
		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		checkDomainOwner(tup);
		ReleaseSysCache(tup);
		table_close(rel, NoLock);
	}
	else
	{
		/* 锁 级别 taken 此处 should match re名称_约束_内部 */
		relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										 stmt->missing_ok ? RVR_MISSING_OK : 0,
										 RangeVarCallbackForRenameAttribute,
										 NULL);
		if (!OidIsValid(relid))
		{
			ereport(NOTICE,
					(errmsg("relation \"%s\" does not exist, skipping",
							stmt->relation->relname)));
			return InvalidObjectAddress;
		}
	}

	return
		rename_constraint_internal(relid, typid,
								   stmt->subname,
								   stmt->newname,
								   (stmt->relation &&
									stmt->relation->inh),	/* recursive? */
								   false,	/* 递归中? */
								   0 /* expected inhcount */ );
}

/*
 * Execute ALTER TABLE/INDEX/SEQUENCE/VIEW/MATERIALIZED VIEW/FOREIGN TABLE
 * RENAME
 */
ObjectAddress
RenameRelation(RenameStmt *stmt)
{
	bool		is_index_stmt = stmt->renameType == OBJECT_INDEX;
	Oid			relid;
	ObjectAddress address;

	/*
	 * 获取目标表、索引、序列、视图、物化视图或外表
	 * 上的排他锁，直到事务结束之前我们都                 
	 * 不会释放它。 
	 *
	 * 此处使用的锁级别应与 RenameRelationInternal 匹配，避免  
	 * 锁升级。但由于 ALTER INDEX 可用于任何关系类型，       
	 * 我们不能不加验证地相信它。       
	 */
	for (;;)
	{
		LOCKMODE	lockmode;
		char		relkind;
		bool		obj_is_index;

		lockmode = is_index_stmt ? ShareUpdateExclusiveLock : AccessExclusiveLock;

		relid = RangeVarGetRelidExtended(stmt->relation, lockmode,
										 stmt->missing_ok ? RVR_MISSING_OK : 0,
										 RangeVarCallbackForAlterRelation,
										 (void *) stmt);

		if (!OidIsValid(relid))
		{
			ereport(NOTICE,
					(errmsg("relation \"%s\" does not exist, skipping",
							stmt->relation->relname)));
			return InvalidObjectAddress;
		}

		/*
		 * We allow mismatched statement and object types (e.g., ALTER INDEX to
		 * rename a table), but we might've used the wrong 锁 level. If that happens,
		 * retry with the correct 锁 level. 我们不 bother if we 已经 acquired
		 * AccessExclusiveLock with an 索引, 然而.
		 */
		relkind = get_rel_relkind(relid);
		obj_is_index = (relkind == RELKIND_INDEX ||
						relkind == RELKIND_PARTITIONED_INDEX);
		if (obj_is_index || is_index_stmt == obj_is_index)
			break;

		UnlockRelationOid(relid, lockmode);
		is_index_stmt = obj_is_index;
	}

	/* Do the 工作 */
	RenameRelationInternal(relid, stmt->newname, false, is_index_stmt);

	ObjectAddressSet(address, RelationRelationId, relid);

	return address;
}

/*
 *		RenameRelationInternal - change the name of a relation
 */
void
RenameRelationInternal(Oid myrelid, const char *newrelname, bool is_internal, bool is_index)
{
	Relation	targetrelation;
	Relation	relrelation;	/* 对于 RELATION 关系 */
	ItemPointerData otid;
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			namespaceId;

	/*
	 * Grab a 锁 on the target relation, which we will NOT release until end of
	 * 事务. We need 至少 a self-排他锁 从而 并发 DDL doesn't overwrite the rename if they
	 * start updating while still seeing the old version. The 锁 也 guards against
	 * triggering relcache reloads in 并发 会话, which might not handle this
	 * information changing under them. For 索引, 我们可以 use a reduced 锁 level 因为
	 * RelationReloadIndexInfo() 处理 索引 specially.
	 *
	 *
	 */
	targetrelation = relation_open(myrelid, is_index ? ShareUpdateExclusiveLock : AccessExclusiveLock);
	namespaceId = RelationGetNamespace(targetrelation);

	/*
	 * Find relation's pg_class 元组, and 确保 newrelname isn't in use.
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	otid = reltup->t_self;
	relform = (Form_pg_class) GETSTRUCT(reltup);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * RenameRelation is careful not to believe the caller's idea of the relation
	 * kind being handled. 我们不 have to worry about this, but let's not be totally
	 * oblivious to it. 我们可以 process an 索引 as not-an-索引, but not the other way
	 * around.
	 */
	Assert(!is_index ||
		   is_index == (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
						targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX));

	/*
	 * Update pg_class 元组 with new relname. (Scribbling on reltup is OK 因为 it's a
	 * copy...)
	 */
	namestrcpy(&(relform->relname), newrelname);

	CatalogTupleUpdate(relrelation, &otid, reltup);
	UnlockTuple(relrelation, &otid, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHookArg(RelationRelationId, myrelid, 0,
								 InvalidOid, is_internal);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, 如有.
	 */
	if (OidIsValid(targetrelation->rd_rel->reltype))
		RenameTypeInternal(targetrelation->rd_rel->reltype,
						   newrelname, namespaceId);

	/*
	 * Also rename the associated 约束, 如有.
	 */
	if (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
		targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		Oid			constraintId = get_index_constraint(myrelid);

		if (OidIsValid(constraintId))
			RenameConstraintById(constraintId, newrelname);
	}

	/*
	 * Close rel, but keep 锁!
	 */
	relation_close(targetrelation, NoLock);
}

/*
 *		ResetRelRewrite - reset relrewrite
 */
void
ResetRelRewrite(Oid myrelid)
{
	Relation	relrelation;	/* 对于 RELATION 关系 */
	HeapTuple	reltup;
	Form_pg_class relform;

	/*
	 * Find relation's pg_class 元组.
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	/*
	 * Update pg_class 元组.
	 */
	relform->relrewrite = InvalidOid;

	CatalogTupleUpdate(relrelation, &reltup->t_self, reltup);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);
}

/*
 * Disallow ALTER TABLE (and similar commands) when the current 后端 has any
 * open reference to the target table besides the one just acquired by the
 * calling command; this implies there's an open cursor or active 计划. We need
 * this check 因为 our 锁 doesn't protect us against stomping on our own foot, 仅
 * other people's feet! For ALTER TABLE, the 仅 case known to cause serious
 * trouble is ALTER COLUMN TYPE, and some changes are obviously pretty benign,
 * so this could possibly be relaxed to 仅 error out for certain types of
 * alterations. But the use-case for allowing any of these things is not
 * obvious, so we won't work hard at it for now. We 也 reject these commands if
 * there are any pending AFTER 触发器 events for the rel. This is certainly
 * necessary for the rewriting variants of ALTER TABLE, 因为 they don't preserve
 * 元组 TIDs and so the pending events would try to fetch the wrong 元组. It 可能
 * overly cautious in other cases, but again it seems better to err on the
 * side of paranoia. REINDEX calls this with "rel" referencing the 索引 to be
 * 重建; here we are worried about active index扫描 on the 索引. The 触发器-event check
 * can be skipped, since we are doing no damage to the 父表. The statement name
 * (eg, "ALTER TABLE") is passed for use in error messages.
 *
 *
 *
 *
 *
 *
 */
void
CheckTableNotInUse(Relation rel, const char *stmt)
{
	int			expected_refcnt;

	expected_refcnt = rel->rd_isnailed ? 2 : 1;
	if (rel->rd_refcnt != expected_refcnt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
		/* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because it is being used by active queries in this session",
						stmt, RelationGetRelationName(rel))));

	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX &&
		AfterTriggerPendingOnRel(RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
		/* translator: first %s is a SQL command, eg ALTER TABLE */
				 errmsg("cannot %s \"%s\" because it has pending trigger events",
						stmt, RelationGetRelationName(rel))));
}

/*
 * CheckAlterTableIsSafe Verify that it's safe to allow ALTER TABLE on this
 * relation. This consists of CheckTableNotInUse() plus a 检查 the relation
 * isn't another 会话's 临时表. 我们必须 split out the temp-table check 因为 there are
 * callers of CheckTableNotInUse() that don't want that, notably DROP TABLE.
 * (我们必须 allow DROP or we couldn't clean out an orphaned temp 模式.) Compare
 * truncate_check_activity().
 *
 *
 */
static void
CheckAlterTableIsSafe(Relation rel)
{
	/*
	 * Don't allow ALTER on temp tables of other 后端. Their local 缓冲区 manager is
	 * not going to cope if 我们需要 change the table's contents. Even if 我们不, there
	 * 可能 optimizations that assume temp tables aren't subject to such
	 * interference.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current 事务, including
	 * open 扫描 and pending AFTER 触发器 events.
	 */
	CheckTableNotInUse(rel, "ALTER TABLE");
}

/*
 * AlterTableLookupRelation
 *		查找并锁定由 alter table 语句指定的关系               
 *		OID。    
 */
Oid
AlterTableLookupRelation(AlterTableStmt *stmt, LOCKMODE lockmode)
{
	return RangeVarGetRelidExtended(stmt->relation, lockmode,
									stmt->missing_ok ? RVR_MISSING_OK : 0,
									RangeVarCallbackForAlterRelation,
									(void *) stmt);
}

/*
 * AlterTable
 *		执行 ALTER TABLE，它可以是子命令列表       
 *
 * ALTER TABLE 分三个阶段执行：     
 *		1. 检查子命令并执行预转换检查。                  
 *		2. 验证并转换子命令，并更新系统目录。            
 *		3. 扫描表以检查新约束，并可选地将数据重新    
 *		   复制到新表中。      
 * 除非一个或多个子命令需要，否则不执行阶段 3。      
 * 此设计的意图是允许对表模式进行多次独立的更   
 * 新，而只需对数据执行一遍扫描。                          
 *      
 *
 * ATPrepCmd 执行阶段 1。会为每个受影响的表创建    
 * 一个"工作队列"项（若命令遍历表继承层次结构，  
 * 则可能影响多个表）。我们还会对子命令做初步验证。 
 * 由于较早的子命令可能会改变稍后命令所看到的目录 
 * 状态，所以此阶段能做的事情存在限制。                
 * 通常，此阶段会获取表锁、检查权限与 relkind，并  
 * 递归找出子表。                                             
 *
 * ATRewriteCatalogs 为每个受影响的表执行阶段 2。
 * 某些子命令需要先于其他子命令执行，以避免   
 * 不必要的冲突；例如 DROP COLUMN 应在 ADD COLUMN 之前  
 * 执行。因此阶段 1 将子命令划分为多个列表，        
 * 对应阶段 2 的各次逻辑"遍"。    
 *
 * ATRewriteTables 为需要它的表执行阶段 3。            
 *
 * 对大多数子命令类型，阶段 2 和 3 不做显式递归， 
 * 因为阶段 1 已完成递归。但对于某些子命令类型，   
 * 要到阶段 2 才能确定递归方式；此时阶段 1 会设置  
 * cmd->recurse 标志。                          
 *
 * 多亏 MVCC 的魔力，途中任何地方发生错误都会回滚     
 * 整个操作；我们不需要做任何特殊的清理工作。       
 *
 * 调用者必须锁定关系，并使用适合所请求子命令  
 * 的锁级别，即使用 AlterTableGetLockLevel(stmt->cmds) 或        
 * 更高的级别。我们将锁级别  
 * 向下传递，以便递归地应用到继承表。请注意，我们 
 * 在递归时想要的锁级别很可能高于该特定子命令所     
 * 需要的级别。因此我们向下传递整体锁需求，而不是  
 * 在较低层级重新评估它。       
 *
 * 调用者还会提供一个"上下文"，在我们需要执行
 * 诸如 CREATE INDEX 之类的子命令时传回给 utility.c。     
 * 其中的某些字段（例如 relid）也会在这里使用。                  
 */
void
AlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
		   AlterTableUtilityContext *context)
{
	Relation	rel;

	/* C所有er 是必需的 to prov标识符e an adequate 锁. */
	rel = relation_open(context->relid, NoLock);

	CheckAlterTableIsSafe(rel);

	ATController(stmt, rel, stmt->cmds, stmt->relation->inh, lockmode, context);
}

/*
 * AlterTableInternal
 *
 * 以 OID 指定目标的 ALTER TABLE     
 *
 * 如果该关系已打开，我们并不拒绝，因为很可能     
 * 有一层或多层调用者已将其打开。这意味着            
 * 对于可能破坏现有查询计划的修改，使用此入口点
 * 并不安全。鉴于它不用于此类用途，我们也不必拒 
 * 绝待处理的 AFTER 触发器。                  
 *
 * 另外，由于我们没有 AlterTableUtilityContext，这不能用  
 * 于任何需要解析转换的子命令类型，或可能生成必
 * 须传递给 ProcessUtility 的子命令的类型。                
 */
void
AlterTableInternal(Oid relid, List *cmds, bool recurse)
{
	Relation	rel;
	LOCKMODE	lockmode = AlterTableGetLockLevel(cmds);

	rel = relation_open(relid, lockmode);

	EventTriggerAlterTableRelid(relid);

	ATController(NULL, rel, cmds, recurse, lockmode, NULL);
}

/*
 * AlterTableGetLockLevel Sets the overall 锁 level required for the supplied
 * list of 子命令. Policy for doing th被设置 according to needs of AlterTable(), see
 * comments there for overall explanation. Function 被调用 之前 and 之后 解析, so it
 * must give same answer each time it 被调用. Some 子命令 are 转换后的 into other 子命令
 * types, so the 转换 must 从不 be made to a lower 锁 level than 先前 assigned. All
 * 转换 are noted 以下. Since this 被调用 之前 we 锁 the table 我们可以not use table
 * metadata to influence the type of 锁 we acquire. There 应是 no lockmodes
 * hardcoded into the 子命令 functions. All lockmode decisions for ALTER TABLE
 * are made here 仅. The one exception is ALTER TABLE RENAME which is treated
 * as a different statement type T_RenameStmt and does not travel through this
 * section of code and 不能 combined with any of the 子命令 given here. 请注意 Hot
 * Standby 仅 knows about AccessExclusiveLocks on the primary so any changes
 * that might affect SELECTs running on standbys need to use
 * AccessExclusiveLocks even if you think a lesser 锁 would do, unless you have
 * a solution for that 也. Also 请注意 pg_dump uses 仅 an AccessShareLock, meaning
 * that anything that takes a 锁 less than AccessExclusiveLock can change
 * object definitions while pg_dump is running. Be careful to 检查 the
 * appropriate data is derived by pg_dump using an MVCC 快照, rather than
 * syscache lookups, 否则 we might end up with an inconsistent dump that can't
 * restore.
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 */
LOCKMODE
AlterTableGetLockLevel(List *cmds)
{
	/*
	 * This 仅 works if we read 目录 tables using MVCC snapshots.
	 */
	ListCell   *lcmd;
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		LOCKMODE	cmd_lockmode = AccessExclusiveLock; /* 默认值 for 编译器 */

		switch (cmd->subtype)
		{
				/*
				 * These 子命令 重写 the 堆, so require full 锁.
				 */
			case AT_AddColumn:	/* may rewrite heap, in some cases and visible
								 * to SELECT */
			case AT_SetAccessMethod:	/* 必须重写堆 */
			case AT_SetTableSpace:	/* 必须重写堆 */
			case AT_AlterColumnType:	/* 必须重写堆 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These 子命令 may require addition of toast tables. If we add a TOAST 表 to
				 * a table 当前 being scanned, we might miss data added to the new TOAST 表
				 * by 并发 insert 事务.
				 *
				 */
			case AT_SetStorage: /* may add toast tables, see
								 * ATRewriteCatalogs() */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Removing 约束 can affect SELECTs that have been optimized assuming the 约束
				 * holds true. See 也 CloneFkReferenced.
				 *
				 */
			case AT_DropConstraint: /* as DROP INDEX */
			case AT_DropNotNull:	/* may 变更 某些 SQL plans */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Subcommands that 可能 visible to 并发 SELECTs
				 */
			case AT_DropColumn: /* 变更 可见 to SELECT */
			case AT_AddColumnToView:	/* CREATE VIEW */
			case AT_DropOids:	/* 使用 to equiv to DropColumn */
			case AT_EnableAlwaysRule:	/* 可能会更改 SELECT 规则 */
			case AT_EnableReplicaRule:	/* 可能会更改 SELECT 规则 */
			case AT_EnableRule: /* 可能会更改 SELECT 规则 */
			case AT_DisableRule:	/* 可能会更改 SELECT 规则 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing 属主 may remove implicit SELECT 权限
				 */
			case AT_ChangeOwner:	/* 变更 可见 to SELECT */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing 外表 options may affect optimization.
				 */
			case AT_GenericOptions:
			case AT_AlterColumnGenericOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These 子命令 affect write operations 仅.
				 */
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
				cmd_lockmode = ShareRowExclusiveLock;
				break;

				/*
				 * These 子命令 affect write operations 仅. XXX Theoretically, these 可能
				 * ShareRowExclusiveLock.
				 */
			case AT_ColumnDefault:
			case AT_CookedColumnDefault:
			case AT_AlterConstraint:
			case AT_AddIndex:	/* from ADD CONSTRAINT */
			case AT_AddIndexConstraint:
			case AT_ReplicaIdentity:
			case AT_SetNotNull:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
			case AT_AddIdentity:
			case AT_DropIdentity:
			case AT_SetIdentity:
			case AT_DropExpression:
			case AT_SetCompression:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_AddConstraint:
			case AT_ReAddConstraint:	/* becomes AT_AddConstraint */
			case AT_ReAddDomainConstraint:	/* becomes AT_AddConstraint */
				if (IsA(cmd->def, Constraint))
				{
					Constraint *con = (Constraint *) cmd->def;

					switch (con->contype)
					{
						case CONSTR_EXCLUSION:
						case CONSTR_PRIMARY:
						case CONSTR_UNIQUE:

							/*
							 * 情况与 CREATE INDEX 基本相同。        
							 * 若能解决如何允许并发目录更新， 
							 * 可把锁强度降到 ShareLock。XXX 也许   
							 * 降为 ShareRowExclusiveLock，  
							 * 但还需要进一步的                  
							 * 分析。
							 */
							cmd_lockmode = AccessExclusiveLock;
							break;
						case CONSTR_FOREIGN:

							/*
							 * We add 触发器 to both tables when we add a Foreign Key, so the 锁 level
							 * 必须是 至少 as strong as CREATE TRIGGER.
							 *
							 */
							cmd_lockmode = ShareRowExclusiveLock;
							break;

						default:
							cmd_lockmode = AccessExclusiveLock;
					}
				}
				break;

				/*
				 * These 子命令 affect 继承 behaviour. Queries started 之前 us will continue to
				 * see the old 继承 behaviour, while 查询 started 之后 we 提交 will see new
				 * behaviour. No need to prevent 读取 or 写入 to the subtable while we hook it
				 * up though. Changing the TupDesc 可能 a problem, so keep highest 锁.
				 *
				 *
				 */
			case AT_AddInherit:
			case AT_DropInherit:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These 子命令 affect implicit 行类型 conversion. They have affects similar to
				 * CREATE/DROP CAST on 查询. don't provide for invalidating 解析 trees as a
				 * result of such changes, so we keep these at AccessExclusiveLock.
				 *
				 */
			case AT_AddOf:
			case AT_DropOf:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Only used by CREATE OR REPLACE VIEW which must conflict with an SELECTs
				 * 当前 using the view.
				 */
			case AT_ReplaceRelOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These 子命令 affect general strategies for performance and maintenance,
				 * though don't change the semantic results from normal data 读取 and 写入.
				 * Delaying an ALTER TABLE behind 当前 active 写入 仅 delays the point where
				 * the new strategy begins to take effect, so there is no benefit in
				 * waiting. In this case the minimum restriction applies: 我们不 当前 allow 并发
				 * 目录 更新.
				 *
				 *
				 */
			case AT_SetStatistics:	/* getTableAttrs() 使用 MVCC */
			case AT_ClusterOn:	/* 使用 MVCC in 获取Indexes() */
			case AT_DropCluster:	/* 使用 MVCC in 获取Indexes() */
			case AT_SetOptions: /* getTableAttrs() 使用 MVCC */
			case AT_ResetOptions:	/* getTableAttrs() 使用 MVCC */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_SetLogged:
			case AT_SetUnLogged:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_ValidateConstraint: /* 使用 MVCC in 获取Constraints() */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

				/*
				 * Rel options are more complex than first appears. Options 被设置 here for
				 * tables, views and 索引; for historical reasons these can all be used with
				 * ALTER TABLE, so 我们可以't decide 之间 them using the basic grammar.
				 *
				 */
			case AT_SetRelOptions:	/* Uses MVCC in getIndexes() and
									 * getTables() */
			case AT_ResetRelOptions:	/* Uses MVCC in getIndexes() and
										 * getTables() */
				cmd_lockmode = AlterTableGetRelOptionsLockLevel((List *) cmd->def);
				break;

			case AT_AttachPartition:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_DetachPartition:
				if (((PartitionCmd *) cmd->def)->concurrent)
					cmd_lockmode = ShareUpdateExclusiveLock;
				else
					cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_DetachPartitionFinalize:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_CheckNotNull:

				/*
				 * This 仅 examines the table's 模式; but 锁 必须是 strong enough to prevent 并发
				 * DROP NOT NULL.
				 */
				cmd_lockmode = AccessShareLock;
				break;

			default:			/* 出错 */
				elog(ERROR, "unrecognized alter table type: %d",
					 (int) cmd->subtype);
				break;
		}

		/*
		 * Take the greatest lockmode from any 子命令
		 */
		if (cmd_lockmode > lockmode)
			lockmode = cmd_lockmode;
	}

	return lockmode;
}

/*
 * ATController 对这些阶段提供顶层控制。       
 *
 * 传入 parsetree 是为了允许在请求时把它传递给事件
 * 触发器。   
 */
static void
ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
			 AlterTableUtilityContext *context)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* 阶段 1: preliminary examination of 命令s, 创建 工作 队列 */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode, context);
	}

	/* Close the 关系, but 保持 锁 直到 提交 */
	relation_close(rel, NoLock);

	/* 阶段 2: 更新 system 目录s */
	ATRewriteCatalogs(&wqueue, lockmode, context);

	/* 阶段 3: 扫描/重写 表s as 需要ed, and 运行 之后Stmts */
	ATRewriteTables(parsetree, &wqueue, lockmode, context);
}

/*
 * ATPrepCmd
 *
 * ALTER TABLE 阶段 1 操作的交通警察，包括简单递归 
 * 和权限检查。              
 *
 * 调用者必须已经获得该关系上适当类型的锁。        
 * 此锁应持有到提交为止。     
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode,
		  AlterTableUtilityContext *context)
{
	AlteredTableInfo *tab;
	int			pass = AT_PASS_UNSET;

	/* Find or 创建 工作 队列 条目 for this 表 */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * Disallow any ALTER TABLE other than ALTER TABLE DETACH FINALIZE on
	 * partitions that are pending detach.
	 */
	if (rel->rd_rel->relispartition &&
		cmd->subtype != AT_DetachPartitionFinalize &&
		PartitionHasPendingDetach(RelationGetRelid(rel)))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter partition \"%s\" with an incomplete detach",
					   RelationGetRelationName(rel)),
				errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

	/*
	 * Copy the original 子命令 for each table, so 我们可以 scribble on it. This avoids
	 * conflicts when different child tables need to make different 解析
	 * transformations (例如, the same column may have different 列号 in different
	 * children).
	 */
	cmd = copyObject(cmd);

	/*
	 * Do 权限 and relkind checking, 递归 to child tables if needed, and any
	 * additional phase-1 processing needed. (But beware of adding any processing
	 * that looks at table details that another 子命令 could change. In some cases
	 * we reject multiple 子命令 that could try to change the same state in contrary
	 * ways.)
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* 添加列 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, false, cmd,
							lockmode, context);
			/* 递归发生在执行阶段 */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_AddColumnToView:	/* 添加 列 via CREATE OR REPLACE VIEW */
			ATSimplePermissions(cmd->subtype, rel, ATT_VIEW);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, true, cmd,
							lockmode, context);
			/* 递归发生在执行阶段 */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */

			/*
			 * We allow defaults on views 从而 INSERT into a view can have default-ish
			 * behavior. This works 因为 the rewriter substitutes 默认值 into INSERTs 之前 it
			 * expands rules.
			 *
			 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备 */
			pass = cmd->def ? AT_PASS_ADD_OTHERCONSTR : AT_PASS_DROP;
			break;
		case AT_CookedColumnDefault:	/* 添加 a pre-cooked 默认值 */
			/* This is 当前ly 使用 仅 in CREATE TABLE */
			/* (so the 权限 检查 re所有y isn't necessary) */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_AddIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_SetIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			/* This should 运行 之后 AddIdentity, so 执行 it in MISC 遍 */
			pass = AT_PASS_MISC;
			break;
		case AT_DropIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			pass = AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATPrepDropNotNull(rel, recurse, recursing);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Need 命令-specific 递归 decision */
			ATPrepSetNotNull(wqueue, rel, cmd, recurse, recursing,
							 lockmode, context);
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_CheckNotNull:	/* 检查 列 is 已经 标记ed NOT NULL */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备 */
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_DropExpression: /* ALTER COLUMN DROP EXPRESSION */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			ATPrepDropExpression(rel, cmd, recurse, recursing, lockmode);
			pass = AT_PASS_DROP;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( 选项 ) */
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( 选项 ) */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetCompression: /* ALTER COLUMN SET COMPRESSION */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd,
							 lockmode, context);
			/* 递归发生在执行阶段 */
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 递归发生在执行阶段 */
			/* 除了保存递归标志外，无需特定命令的准备 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_ADD_INDEXCONSTR;
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
			/* Other 递归 occurs 期间 执行 阶段 */
			/* 除了保存递归标志外，无需特定命令的准备 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			/* See 注释s for ATPrepAlterColumnType */
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
			Assert(cmd != NULL);
			/* Performs own 递归 */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd,
								  lockmode, context);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_AlterColumnGenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			/* 这些命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetLogged:		/* SET LOGGED */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_SEQUENCE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			tab->chgPersistence = ATPrepChangePersistence(rel, true);
			/* force 重写 if necessary; see 注释 in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_SetUnLogged:	/* SET UNLOGGED */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_SEQUENCE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			tab->chgPersistence = ATPrepChangePersistence(rel, false);
			/* force 重写 if necessary; see 注释 in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_UNLOGGED;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			pass = AT_PASS_DROP;
			break;
		case AT_SetAccessMethod:	/* SET ACCESS METHOD */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);

			/* 分区ed 表s 执行n't have an access method */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot change access method of a partitioned table")));

			/* 检查 if another access method 变更 was 已经 requested */
			if (OidIsValid(tab->newAccessMethod))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot have multiple SET ACCESS METHOD subcommands")));

			ATPrepSetAccessMethod(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* 执行es not matter; no 工作 in 阶段 2 */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX |
								ATT_PARTITIONED_INDEX);
			/* 此命令不会递归 */
			ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
			pass = AT_PASS_MISC;	/* 执行esn't actu所有y matter */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* RESET (...) */
		case AT_ReplaceRelOptions:	/* re设置 them 所有, 然后 设置 仅 these */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* INHERIT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			ATPrepAddInherit(rel);
			pass = AT_PASS_MISC;
			break;
		case AT_DropInherit:	/* NO INHERIT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 递归发生在执行阶段 */
			pass = AT_PASS_MISC;
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 递归发生在执行阶段 */
			/* 除了保存递归标志外，无需特定命令的准备 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_ReplicaIdentity:	/* REPLICA IDENTITY ... */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			pass = AT_PASS_MISC;
			/* 此命令不会递归 */
			/* 无需特定命令的准备 */
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* Set up 递归 for 阶段 2; no other prep 需要ed */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_EnableRule:		/* ENABLE/DISABLE RULE variants */
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddOf:			/* OF */
		case AT_DropOf:			/* NOT OF */
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 这些命令不会递归 */
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_GenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_AttachPartition:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_PARTITIONED_INDEX);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartition:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartitionFinalize:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 无需特定命令的准备 */
			pass = AT_PASS_MISC;
			break;
		default:				/* 出错 */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = AT_PASS_UNSET;	/* 避免编译器警告 */
			break;
	}
	Assert(pass > AT_PASS_UNSET);

	/* Add the sub命令 to the appropriate 列表 for 阶段 2 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * ALTER TABLE 阶段 2 操作的交通警察。子命令按照    
 * "安全"的执行顺序分发（该顺序旨在避免不必要的
 * 冲突）      
 */
static void
ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	int			pass;
	ListCell   *ltab;

	/*
	 * We process all the tables "in parallel", one pass at a time. This 是必需的 因为
	 * we may have to propagate work from one table to another (specifically,
	 * ALTER TYPE on a 外键's PK has to dispatch the re-adding of the 外键约束 to the
	 * other table). Work can 仅 be propagated into 稍后的 passes, 然而.
	 *
	 */
	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through 每 表 that 需要s to be 处理ed */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * Open the relation and store it in tab. This 允许 subroutines close and
			 * reopen, if necessary. Appropriate 锁 was obtained by phase 1, needn't get
			 * it again.
			 */
			tab->rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(wqueue, tab,
						  lfirst_node(AlterTableCmd, lcmd),
						  lockmode, pass, context);

			/*
			 * After the ALTER TYPE pass, do cleanup work (this is not done in
			 * ATExecAlterColumnType since it 应是 done 仅 once if multiple columns of a
			 * table are altered).
			 */
			if (pass == AT_PASS_ALTER_TYPE)
				ATPostAlterTypeCleanup(wqueue, tab, lockmode);

			if (tab->rel)
			{
				relation_close(tab->rel, NoLock);
				tab->rel = NULL;
			}
		}
	}

	/* Check to see if a toast 表 必须是 添加ed. */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/*
		 * If the table is source table of ATTACH PARTITION command, we did
		 * not modify anything about it that will change its toasting
		 * requirement, so no need to check.
		 */
		if (((tab->relkind == RELKIND_RELATION ||
			  tab->relkind == RELKIND_PARTITIONED_TABLE) &&
			 tab->partition_constraint == NULL) ||
			tab->relkind == RELKIND_MATVIEW)
			AlterTableCreateToastTable(tab->relid, (Datum) 0, lockmode);
	}
}

/*
 * ATExecCmd: dispatch a 子命令 to appropriate 执行 routine
 */
static void
ATExecCmd(List **wqueue, AlteredTableInfo *tab,
		  AlterTableCmd *cmd, LOCKMODE lockmode, int cur_pass,
		  AlterTableUtilityContext *context)
{
	ObjectAddress address = InvalidObjectAddress;
	Relation	rel = tab->rel;

	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* 添加列 */
		case AT_AddColumnToView:	/* 添加 列 via CREATE OR REPLACE VIEW */
			address = ATExecAddColumn(wqueue, tab, rel, &cmd,
									  cmd->recurse, false,
									  lockmode, cur_pass, context);
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
			address = ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_CookedColumnDefault:	/* 添加 a pre-cooked 默认值 */
			address = ATExecCookedColumnDefault(rel, cmd->num, cmd->def);
			break;
		case AT_AddIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecAddIdentity(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecSetIdentity(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_DropIdentity:
			address = ATExecDropIdentity(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			address = ATExecDropNotNull(rel, cmd->name, lockmode);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			address = ATExecSetNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_CheckNotNull:	/* 检查 列 is 已经 标记ed NOT NULL */
			ATExecCheckNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_DropExpression:
			address = ATExecDropExpression(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			address = ATExecSetStatistics(rel, cmd->name, cmd->num, cmd->def, lockmode);
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( 选项 ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
			break;
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( 选项 ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			address = ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetCompression: /* ALTER COLUMN SET COMPRESSION */
			address = ATExecSetCompression(rel, cmd->name, cmd->def,
										   lockmode);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, cmd->recurse, false,
									   cmd->missing_ok, lockmode,
									   NULL);
			break;
		case AT_AddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
									 lockmode);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
									 lockmode);
			break;
		case AT_ReAddStatistics:	/* ADD STATISTICS */
			address = ATExecAddStatistics(tab, rel, (CreateStatsStmt *) cmd->def,
										  true, lockmode);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			/* Transform the 命令 仅 期间 initial examination */
			if (cur_pass == AT_PASS_ADD_CONSTR)
				cmd = ATParseTransformCmd(wqueue, tab, rel, cmd,
										  cmd->recurse, lockmode,
										  cur_pass, context);
			/* Depending on 约束 类型, 可能 no 更多 工作 to 执行 现在 */
			if (cmd != NULL)
				address =
					ATExecAddConstraint(wqueue, tab, rel,
										(Constraint *) cmd->def,
										cmd->recurse, false, lockmode);
			break;
		case AT_ReAddConstraint:	/* Re-添加 pre-existing 检查 约束 */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									true, true, lockmode);
			break;
		case AT_ReAddDomainConstraint:	/* Re-add pre-existing domain check
										 * constraint */
			address =
				AlterDomainAddConstraint(((AlterDomainStmt *) cmd->def)->typeName,
										 ((AlterDomainStmt *) cmd->def)->def,
										 NULL);
			break;
		case AT_ReAddComment:	/* Re-添加 existing 注释 */
			address = CommentObject((CommentStmt *) cmd->def);
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
			address = ATExecAddIndexConstraint(tab, rel, (IndexStmt *) cmd->def,
											   lockmode);
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
			address = ATExecAlterConstraint(rel, cmd, false, false, lockmode);
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
			address = ATExecValidateConstraint(wqueue, rel, cmd->name, cmd->recurse,
											   false, lockmode);
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 cmd->recurse, false,
								 cmd->missing_ok, lockmode);
			break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
			/* parse transformation was 执行ne earlier */
			address = ATExecAlterColumnType(tab, rel, cmd, lockmode);
			break;
		case AT_AlterColumnGenericOptions:	/* ALTER COLUMN OPTIONS */
			address =
				ATExecAlterColumnGenericOptions(rel, cmd->name,
												(List *) cmd->def, lockmode);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_rolespec_oid(cmd->newowner, false),
							  false, lockmode);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
			address = ATExecClusterOn(rel, cmd->name, lockmode);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATExecDropCluster(rel, lockmode);
			break;
		case AT_SetLogged:		/* SET LOGGED */
		case AT_SetUnLogged:	/* SET UNLOGGED */
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			/* nothing to 执行 此处, o标识符 列 执行n't exist 任何更多 */
			break;
		case AT_SetAccessMethod:	/* SET ACCESS METHOD */
			/* handled speci所有y in 阶段 3 */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */

			/*
			 * Only do this for partitioned tables and 索引, for which this is just a 目录
			 * change. Other relation types which have 存储 are handled by Phase 3.
			 *
			 */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
				rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
				ATExecSetTableSpaceNoStorage(rel, tab->newTableSpace);

			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* RESET (...) */
		case AT_ReplaceRelOptions:	/* replace entire 选项 列表 */
			ATExecSetRelOptions(rel, (List *) cmd->def, cmd->subtype, lockmode);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER 名称 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableAlwaysTrig:	/* ENABLE ALWAYS TRIGGER 名称 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableReplicaTrig:	/* ENABLE REPLICA TRIGGER 名称 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_REPLICA, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER 名称 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigAll: /* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigUser: /* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, true,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigUser:	/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, true,
									   cmd->recurse,
									   lockmode);
			break;

		case AT_EnableRule:		/* ENABLE RULE 名称 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN, lockmode);
			break;
		case AT_EnableAlwaysRule:	/* ENABLE ALWAYS RULE 名称 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS, lockmode);
			break;
		case AT_EnableReplicaRule:	/* ENABLE REPLICA RULE 名称 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA, lockmode);
			break;
		case AT_DisableRule:	/* DISABLE RULE 名称 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_DISABLED, lockmode);
			break;

		case AT_AddInherit:
			address = ATExecAddInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_DropInherit:
			address = ATExecDropInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_AddOf:
			address = ATExecAddOf(rel, (TypeName *) cmd->def, lockmode);
			break;
		case AT_DropOf:
			ATExecDropOf(rel, lockmode);
			break;
		case AT_ReplicaIdentity:
			ATExecReplicaIdentity(rel, (ReplicaIdentityStmt *) cmd->def, lockmode);
			break;
		case AT_EnableRowSecurity:
			ATExecSetRowSecurity(rel, true);
			break;
		case AT_DisableRowSecurity:
			ATExecSetRowSecurity(rel, false);
			break;
		case AT_ForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, true);
			break;
		case AT_NoForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, false);
			break;
		case AT_GenericOptions:
			ATExecGenericOptions(rel, (List *) cmd->def);
			break;
		case AT_AttachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				address = ATExecAttachPartition(wqueue, rel, (PartitionCmd *) cmd->def,
												context);
			else
				address = ATExecAttachPartitionIdx(wqueue, rel,
												   ((PartitionCmd *) cmd->def)->name);
			break;
		case AT_DetachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			/* ATPrepCmd ensures it 必须是 a 表 */
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			address = ATExecDetachPartition(wqueue, tab, rel,
											((PartitionCmd *) cmd->def)->name,
											((PartitionCmd *) cmd->def)->concurrent);
			break;
		case AT_DetachPartitionFinalize:
			address = ATExecDetachPartitionFinalize(rel, ((PartitionCmd *) cmd->def)->name);
			break;
		default:				/* 出错 */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Report the 子命令 to interested event 触发器.
	 */
	if (cmd)
		EventTriggerCollectAlterTableSubcmd((Node *) cmd, address);

	/*
	 * Bump the 命令计数器 to ensure the next 子命令 in the 序列 can see the changes so far
	 *
	 */
	CommandCounterIncrement();
}

/*
 * ATParseTransformCmd: perform 解析转换 for one 子命令 Returns the 转换后的 子命令 tree, if
 * there is one, else NULL. The 解析器 may hand back additional AlterTableCmd(s)
 * and/or other utility statements, either 之前 or 之后 the original 子命令. Other
 * AlterTableCmds are scheduled into the appropriate slot of the
 * AlteredTableInfo (they had better be for 稍后的 passes than the current one).
 * Utility statements that are supposed to happen 之前 the AlterTableCmd are
 * executed immediately. Those that are supposed to happen 之后wards are added
 * to the tab->之后Stmts list to be done at the very end.
 *
 *
 *
 */
static AlterTableCmd *
ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab, Relation rel,
					AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
					int cur_pass, AlterTableUtilityContext *context)
{
	AlterTableCmd *newcmd = NULL;
	AlterTableStmt *atstmt = makeNode(AlterTableStmt);
	List	   *beforeStmts;
	List	   *afterStmts;
	ListCell   *lc;

	/* Gin up an AlterTableStmt with 仅 this sub命令 and this 表 */
	atstmt->relation =
		makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
					 pstrdup(RelationGetRelationName(rel)),
					 -1);
	atstmt->relation->inh = recurse;
	atstmt->cmds = list_make1(cmd);
	atstmt->objtype = OBJECT_TABLE; /* 需要n't be picky 此处 */
	atstmt->missing_ok = false;

	/* Transform the AlterTableStmt */
	atstmt = transformAlterTableStmt(RelationGetRelid(rel),
									 atstmt,
									 context->queryString,
									 &beforeStmts,
									 &afterStmts);

	/* Execute 任何 状态ments that should happen 之前 these sub命令(s) */
	foreach(lc, beforeStmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		ProcessUtilityForAlterTable(stmt, context);
		CommandCounterIncrement();
	}

	/* Examine the transformed sub命令s and schedule them appropriately */
	foreach(lc, atstmt->cmds)
	{
		AlterTableCmd *cmd2 = lfirst_node(AlterTableCmd, lc);
		int			pass;

		/*
		 * This switch need 仅 cover the 子命令 types that can be added by
		 * parse_utilcmd.c; 否则, we'll use the default strategy of executing the 子命令
		 * immediately, as a substitute for the original 子命令. (Note, 然而, that this
		 * does cause AT_AddConstraint 子命令 to be rescheduled into 稍后的 passes, which
		 * is important for 索引 and 外键 约束.) We assume we needn't do any phase-1 检查
		 * for added 子命令.
		 *
		 *
		 */
		switch (cmd2->subtype)
		{
			case AT_SetNotNull:
				/* Need 命令-specific 递归 decision */
				ATPrepSetNotNull(wqueue, rel, cmd2,
								 recurse, false,
								 lockmode, context);
				pass = AT_PASS_COL_ATTRS;
				break;
			case AT_AddIndex:
				/* 此命令不会递归 */
				/* 无需特定命令的准备 */
				pass = AT_PASS_ADD_INDEX;
				break;
			case AT_AddIndexConstraint:
				/* 此命令不会递归 */
				/* 无需特定命令的准备 */
				pass = AT_PASS_ADD_INDEXCONSTR;
				break;
			case AT_AddConstraint:
				/* 递归发生在执行阶段 */
				if (recurse)
					cmd2->recurse = true;
				switch (castNode(Constraint, cmd2->def)->contype)
				{
					case CONSTR_PRIMARY:
					case CONSTR_UNIQUE:
					case CONSTR_EXCLUSION:
						pass = AT_PASS_ADD_INDEXCONSTR;
						break;
					default:
						pass = AT_PASS_ADD_OTHERCONSTR;
						break;
				}
				break;
			case AT_AlterColumnGenericOptions:
				/* 此命令不会递归 */
				/* 无需特定命令的准备 */
				pass = AT_PASS_MISC;
				break;
			default:
				pass = cur_pass;
				break;
		}

		if (pass < cur_pass)
		{
			/* Cannot schedule into a 遍 we 已经 finished */
			elog(ERROR, "ALTER TABLE scheduling failure: too late for pass %d",
				 pass);
		}
		else if (pass > cur_pass)
		{
			/* OK, 队列 it up for 稍后 */
			tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd2);
		}
		else
		{
			/*
			 * 我们应该 see 最多 one 子命令 for the current pass, which is the 转换后的 version of
			 * the original 子命令.
			 */
			if (newcmd == NULL && cmd->subtype == cmd2->subtype)
			{
				/* Found the transformed version of our sub命令 */
				newcmd = cmd2;
			}
			else
				elog(ERROR, "ALTER TABLE scheduling failure: bogus item for pass %d",
					 pass);
		}
	}

	/* Queue up 任何 之后-状态ments to happen at the end */
	tab->afterStmts = list_concat(tab->afterStmts, afterStmts);

	return newcmd;
}

/*
 * ATRewriteTables: ALTER TABLE phase 3
 */
static void
ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	ListCell   *ltab;

	/* Go through 每 表 that 需要s to be 检查ed or rewritten */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/* Relations without 存储 可能 ignored 此处 */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		/*
		 * If we change column data types, the operation 必须是 propagated to tables
		 * that use this table's rowtype as a 列类型. tab->newvals will 也 be non-NULL
		 * in the case where we're adding a column with a default. We choose to
		 * forbid that case 同时, since composite types might eventually support
		 * defaults. (Eventually we'll probably need to check for 复合类型 依赖 even when
		 * we're just scanning the table 没有 a 重写, but at the moment a 复合类型 does not
		 * enforce any 约束, so it's not necessary/appropriate to enforce them just 期间
		 * ALTER.)
		 *
		 *
		 *
		 */
		if (tab->newvals != NIL || tab->rewrite > 0)
		{
			Relation	rel;

			rel = table_open(tab->relid, NoLock);
			find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
			table_close(rel, NoLock);
		}

		/*
		 * We 仅 need to 重写 the table if 至少 one column 需要被 recomputed, or we are
		 * changing its persistence or 访问方法. There are two reasons for requiring a
		 * 重写 when changing persistence: on one hand, 我们需要 ensure that the 缓冲区
		 * belonging to each of the two relations are marked with or 没有 BM_PERMANENT
		 * properly. On the other hand, since rewriting 创建 and assigns a new
		 * relfilenumber, we automatically create or drop an init fork for the
		 * relation as appropriate.
		 *
		 *
		 */
		if (tab->rewrite > 0 && tab->relkind != RELKIND_SEQUENCE)
		{
			/* Build a temporary 关系 and 复制 数据 */
			Relation	OldHeap;
			Oid			OIDNewHeap;
			Oid			NewAccessMethod;
			Oid			NewTableSpace;
			char		persistence;

			OldHeap = table_open(tab->relid, NoLock);

			/*
			 * 我们不 support rewriting of system 目录; there are too many corner cases and
			 * too little benefit. In particular this is certainly not going to work
			 * for mapped 目录.
			 */
			if (IsSystemRelation(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(OldHeap))));

			if (RelationIsUsedAsCatalogTable(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite table \"%s\" used as a catalog table",
								RelationGetRelationName(OldHeap))));

			/*
			 * Don't allow 重写 on temp tables of other 后端 ... their local 缓冲区 manager is
			 * not going to cope. (This is redundant with the check in
			 * CheckAlterTableIsSafe, but for safety we'll check here too.)
			 *
			 */
			if (RELATION_IS_OTHER_TEMP(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite temporary tables of other sessions")));

			/*
			 * Select destination 表空间 (same as original unless 用户 requested a change)
			 *
			 */
			if (tab->newTableSpace)
				NewTableSpace = tab->newTableSpace;
			else
				NewTableSpace = OldHeap->rd_rel->reltablespace;

			/*
			 * Select destination 访问方法 (same as original unless 用户 requested a change)
			 *
			 */
			if (OidIsValid(tab->newAccessMethod))
				NewAccessMethod = tab->newAccessMethod;
			else
				NewAccessMethod = OldHeap->rd_rel->relam;

			/*
			 * Select persistence of transient table (same as original unless 用户
			 * requested a change)
			 */
			persistence = tab->chgPersistence ?
				tab->newrelpersistence : OldHeap->rd_rel->relpersistence;

			table_close(OldHeap, NoLock);

			/*
			 * 在实际重写该表之前，就先触发事件触发      
			 * 器。
			 *
			 * 我们不支持任何嵌套命令使用事件触发器，   
			 * 此处也不例外，且当来自 AlterTableInternal 时   
			 * parsetree 为 NULL 
			 *
			 * 且只触发一次。 
			 */
			if (parsetree)
				EventTriggerTableRewrite((Node *) parsetree,
										 tab->relid,
										 tab->rewrite);

			/*
			 * Create transient table that will receive the modified data. Ensure it is
			 * marked correctly as logged or unlogged. We have to do this here 从而 缓冲区
			 * for the new relfilenumber will have the right persistence set, and at
			 * the same time ensure that the original filenumbers's 缓冲区 will get read
			 * in with the correct setting (i.e. the original one). Otherwise a 回滚 之后
			 * the 重写 would possibly result with 缓冲区 for the original filenumbers
			 * having the wrong persistence setting. NB: This relies on
			 * swap_relation_files() 也 swapping the persistence. That wouldn't work for
			 * pg_class, but that can't be unlogged anyway.
			 *
			 *
			 *
			 *
			 *
			 */
			OIDNewHeap = make_new_heap(tab->relid, NewTableSpace, NewAccessMethod,
									   persistence, lockmode);

			/*
			 * Copy the 堆 data into the new table with the desired modifications, and
			 * test the current data 之内 the table against new 约束 generated by ALTER
			 * TABLE commands.
			 */
			ATRewriteTable(tab, OIDNewHeap, lockmode);

			/*
			 * Swap the physical files of the old and new heaps, then 重建 索引 and discard
			 * the old 堆. 我们可以 use RecentXmin for the table's new relfrozenxid 因为 we
			 * rewrote all the 元组 in ATRewriteTable, so no older Xid remains in the
			 * table. Also, we 从不 try to swap toast tables by content, since we have no
			 * interest in letting this code work on system 目录.
			 *
			 */
			finish_heap_swap(tab->relid, OIDNewHeap,
							 false, false, true,
							 !OidIsValid(tab->newTableSpace),
							 RecentXmin,
							 ReadNextMultiXactId(),
							 persistence);

			InvokeObjectPostAlterHook(RelationRelationId, tab->relid, 0);
		}
		else if (tab->rewrite > 0 && tab->relkind == RELKIND_SEQUENCE)
		{
			if (tab->chgPersistence)
				SequenceChangePersistence(tab->relid, tab->newrelpersistence);
		}
		else
		{
			/*
			 * If required, test the current data 之内 the table against new 约束 generated
			 * by ALTER TABLE commands, but don't 重建 data.
			 *
			 */
			if (tab->constraints != NIL || tab->verify_new_notnull ||
				tab->partition_constraint != NULL)
				ATRewriteTable(tab, InvalidOid, lockmode);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct 元组, just do a
			 * 块-by-块 copy.
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);
		}

		/*
		 * Also change persistence of owned 序列, 从而 it matches the table persistence.
		 *
		 */
		if (tab->chgPersistence)
		{
			List	   *seqlist = getOwnedSequences(tab->relid);
			ListCell   *lc;

			foreach(lc, seqlist)
			{
				Oid			seq_relid = lfirst_oid(lc);

				SequenceChangePersistence(seq_relid, tab->newrelpersistence);
			}
		}
	}

	/*
	 * Foreign key 约束 are checked in a final pass, since (a) it's generally best
	 * to examine each one separately, and (b) it's 至少 theoretically possible
	 * that we have changed both relations of the 外键, and we'd better have
	 * finished both re写入 之前 we try to read the tables.
	 *
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		/* Relations without 存储 可能 ignored 此处 too */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		foreach(lcon, tab->constraints)
		{
			NewConstraint *con = lfirst(lcon);

			if (con->contype == CONSTR_FOREIGN)
			{
				Constraint *fkconstraint = (Constraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since 锁ed, no 需要 for another */
					rel = table_open(tab->relid, NoLock);
				}

				refrel = table_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint->conname, rel, refrel,
											 con->refindid,
											 con->conid);

				/*
				 * No need to mark the 约束 row as 已验证, we did that when we inserted the row
				 * 更早的.
				 */

				table_close(refrel, NoLock);
			}
		}

		if (rel)
			table_close(rel, NoLock);
	}

	/* Fin所有y, 运行 任何 之后Stmts that were 队列d up */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		ListCell   *lc;

		foreach(lc, tab->afterStmts)
		{
			Node	   *stmt = (Node *) lfirst(lc);

			ProcessUtilityForAlterTable(stmt, context);
			CommandCounterIncrement();
		}
	}
}

/*
 * ATRewriteTable: 扫描 or 重写 one table OIDNewHeap is InvalidOid if 我们不 need to
 * 重写
 *
 */
static void
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode)
{
	Relation	oldrel;
	Relation	newrel;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	bool		needscan = false;
	List	   *notnull_attrs;
	int			i;
	ListCell   *l;
	EState	   *estate;
	CommandId	mycid;
	BulkInsertState bistate;
	int			ti_options;
	ExprState  *partqualstate = NULL;

	/*
	 * Open the relation(s). We have surely 已经 locked the existing table.
	 *
	 */
	oldrel = table_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);	/* includes 所有 mods */

	if (OidIsValid(OIDNewHeap))
		newrel = table_open(OIDNewHeap, lockmode);
	else
		newrel = NULL;

	/*
	 * Prepare a BulkInsertState and options for table_tuple_insert.  The FSM
	 * is empty, so don't bother using it.
	 */
	if (newrel)
	{
		mycid = GetCurrentCommandId(true);
		bistate = GetBulkInsertState();
		ti_options = TABLE_INSERT_SKIP_FSM;
	}
	else
	{
		/* 保持 编译器 quiet about 使用 these uninitialized */
		mycid = 0;
		bistate = NULL;
		ti_options = 0;
	}

	/*
	 * Generate the 约束 and default 执行 states
	 */

	estate = CreateExecutorState();

	/* Build the 需要ed 表达式 执行 状态s */
	foreach(l, tab->constraints)
	{
		NewConstraint *con = lfirst(l);

		switch (con->contype)
		{
			case CONSTR_CHECK:
				needscan = true;
				con->qualstate = ExecPrepareExpr((Expr *) con->qual, estate);
				break;
			case CONSTR_FOREIGN:
				/* Nothing to 执行 此处 */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	/* Build 表达式 执行 状态s for 分区 检查 quals */
	if (tab->partition_constraint)
	{
		needscan = true;
		partqualstate = ExecPrepareExpr(tab->partition_constraint, estate);
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		/* expr 已经 planned */
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
	}

	notnull_attrs = NIL;
	if (newrel || tab->verify_new_notnull)
	{
		/*
		 * If we are rebuilding the 元组 OR if we added any new but not verified NOT
		 * NULL 约束, check all NOT NULL 约束s. This is a bit of overkill but it
		 * minimizes risk of bugs, and heap_attisnull is a pretty cheap test anyway.
		 *
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(newTupDesc, i);

			if (attr->attnotnull && !attr->attisdropped)
				notnull_attrs = lappend_int(notnull_attrs, i);
		}
		if (notnull_attrs)
			needscan = true;
	}

	if (newrel || needscan)
	{
		ExprContext *econtext;
		TupleTableSlot *oldslot;
		TupleTableSlot *newslot;
		TableScanDesc scan;
		MemoryContext oldCxt;
		List	   *dropped_attrs = NIL;
		ListCell   *lc;
		Snapshot	snapshot;

		if (newrel)
			ereport(DEBUG1,
					(errmsg_internal("rewriting table \"%s\"",
									 RelationGetRelationName(oldrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("verifying table \"%s\"",
									 RelationGetRelationName(oldrel))));

		if (newrel)
		{
			/*
			 * All predicate 锁 on the 元组 or pages are about to be made 无效, 因为 we move
			 * 元组 around. Promote them to relation 锁.
			 *
			 */
			TransferPredicateLocksToHeapRelation(oldrel);
		}

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Create necessary 元组 slots. When rewriting, two slots 是必需的, 否则 one
		 * suffices. In the case where one slot suffices, 我们需要 use the new 元组描述符, 否则
		 * some 约束 can't be evaluated. 请注意 even when the 元组 layout is the same and
		 * no 重写 is required, the tupDescs might not be (consider ADD COLUMN 没有 a
		 * default).
		 *
		 */
		if (tab->rewrite)
		{
			Assert(newrel != NULL);
			oldslot = MakeSingleTupleTableSlot(oldTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(newrel));

			/*
			 * Set all columns in the new slot to NULL initially, to ensure columns
			 * added as part of the 重写 are initialized to NULL. That is necessary as
			 * tab->newvals will not contain an expression for columns with a NULL
			 * default, e.g. when adding a column 没有 a default together with a column
			 * with a default requiring an actual 重写.
			 *
			 */
			ExecStoreAllNullTuple(newslot);
		}
		else
		{
			oldslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = NULL;
		}

		/*
		 * Any 属性 that 被删除 according to the new 元组描述符 can be set to NULL. We
		 * precompute the list of dropped 属性 to avoid needing to do so in the per-元组
		 * loop.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (TupleDescAttr(newTupDesc, i)->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}

		/*
		 * Scan through the rows, generating a new row if needed and then checking
		 * all the 约束.
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(oldrel, snapshot, 0, NULL);

		/*
		 * Switch to per-元组 memory context and reset it for each 元组 produced, so 我们不
		 * leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, oldslot))
		{
			TupleTableSlot *insertslot;

			if (tab->rewrite > 0)
			{
				/* Extract 数据 from 旧 元组 */
				slot_getallattrs(oldslot);
				ExecClearTuple(newslot);

				/* 复制 属性s */
				memcpy(newslot->tts_values, oldslot->tts_values,
					   sizeof(Datum) * oldslot->tts_nvalid);
				memcpy(newslot->tts_isnull, oldslot->tts_isnull,
					   sizeof(bool) * oldslot->tts_nvalid);

				/* Set 删除ped 属性s to null in 新 元组 */
				foreach(lc, dropped_attrs)
					newslot->tts_isnull[lfirst_int(lc)] = true;

				/*
				 * Constraints and GENERATED expressions might reference the tableoid
				 * column, so fill tts_tableOid with the desired value. (我们必须 do this each
				 * time, 因为 it gets overwritten with newrel's OID 期间 storing.)
				 *
				 */
				newslot->tts_tableOid = RelationGetRelid(oldrel);

				/*
				 * 处理所提供的表达式，以替换选定的列。   
				 *
				 * 首先，求值其输入来自旧元组的表达          
				 * 式。
				 */
				econtext->ecxt_scantuple = oldslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				ExecStoreVirtualTuple(newslot);

				/*
				 * Now, evaluate any expressions whose inputs come from the new 元组. We
				 * assume these columns won't reference each other, 从而 there's no ordering
				 * 依赖.
				 */
				econtext->ecxt_scantuple = newslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (!ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				insertslot = newslot;
			}
			else
			{
				/*
				 * If there's no 重写, old and new table are guaranteed to have the same AM,
				 * so 我们可以 just use the old slot to verify new 约束 etc.
				 *
				 */
				insertslot = oldslot;
			}

			/* Now 检查 任何 约束s on the possibly-变更d 元组 */
			econtext->ecxt_scantuple = insertslot;

			foreach(l, notnull_attrs)
			{
				int			attn = lfirst_int(l);

				if (slot_attisnull(insertslot, attn + 1))
				{
					Form_pg_attribute attr = TupleDescAttr(newTupDesc, attn);

					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" of relation \"%s\" contains null values",
									NameStr(attr->attname),
									RelationGetRelationName(oldrel)),
							 errtablecol(oldrel, attn + 1)));
				}
			}

			foreach(l, tab->constraints)
			{
				NewConstraint *con = lfirst(l);

				switch (con->contype)
				{
					case CONSTR_CHECK:
						if (!ExecCheck(con->qualstate, econtext))
							ereport(ERROR,
									(errcode(ERRCODE_CHECK_VIOLATION),
									 errmsg("check constraint \"%s\" of relation \"%s\" is violated by some row",
											con->name,
											RelationGetRelationName(oldrel)),
									 errtableconstraint(oldrel, con->name)));
						break;
					case CONSTR_FOREIGN:
						/* Nothing to 执行 此处 */
						break;
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) con->contype);
				}
			}

			if (partqualstate && !ExecCheck(partqualstate, econtext))
			{
				if (tab->validate_default)
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("partition constraint of relation \"%s\" is violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
			}

			/* Write the 元组 out to the 新 关系 */
			if (newrel)
				table_tuple_insert(newrel, insertslot, mycid,
								   ti_options, bistate);

			ResetExprContext(econtext);

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		ExecDropSingleTupleTableSlot(oldslot);
		if (newslot)
			ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	table_close(oldrel, NoLock);
	if (newrel)
	{
		FreeBulkInsertState(bistate);

		table_finish_bulk_insert(newrel, ti_options);

		table_close(newrel, NoLock);
	}
}

/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 */
static AlteredTableInfo *
ATGetQueueEntry(List **wqueue, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	AlteredTableInfo *tab;
	ListCell   *ltab;

	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == relid)
			return tab;
	}

	/*
	 * Not there, so add it. 请注意 we make a copy of the relation's existing
	 * descriptor 之前 anything interesting can happen to it.
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->rel = NULL;			/* 设置 稍后 */
	tab->relkind = rel->rd_rel->relkind;
	tab->oldDesc = CreateTupleDescCopyConstr(RelationGetDescr(rel));
	tab->newAccessMethod = InvalidOid;
	tab->newTableSpace = InvalidOid;
	tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
	tab->chgPersistence = false;

	*wqueue = lappend(*wqueue, tab);

	return tab;
}

static const char *
alter_table_type_to_string(AlterTableType cmdtype)
{
	switch (cmdtype)
	{
		case AT_AddColumn:
		case AT_AddColumnToView:
			return "ADD COLUMN";
		case AT_ColumnDefault:
		case AT_CookedColumnDefault:
			return "ALTER COLUMN ... SET DEFAULT";
		case AT_DropNotNull:
			return "ALTER COLUMN ... DROP NOT NULL";
		case AT_SetNotNull:
			return "ALTER COLUMN ... SET NOT NULL";
		case AT_DropExpression:
			return "ALTER COLUMN ... DROP EXPRESSION";
		case AT_CheckNotNull:
			return NULL;		/* 非真实语法 */
		case AT_SetStatistics:
			return "ALTER COLUMN ... SET STATISTICS";
		case AT_SetOptions:
			return "ALTER COLUMN ... SET";
		case AT_ResetOptions:
			return "ALTER COLUMN ... RESET";
		case AT_SetStorage:
			return "ALTER COLUMN ... SET STORAGE";
		case AT_SetCompression:
			return "ALTER COLUMN ... SET COMPRESSION";
		case AT_DropColumn:
			return "DROP COLUMN";
		case AT_AddIndex:
		case AT_ReAddIndex:
			return NULL;		/* 非真实语法 */
		case AT_AddConstraint:
		case AT_ReAddConstraint:
		case AT_ReAddDomainConstraint:
		case AT_AddIndexConstraint:
			return "ADD CONSTRAINT";
		case AT_AlterConstraint:
			return "ALTER CONSTRAINT";
		case AT_ValidateConstraint:
			return "VALIDATE CONSTRAINT";
		case AT_DropConstraint:
			return "DROP CONSTRAINT";
		case AT_ReAddComment:
			return NULL;		/* 非真实语法 */
		case AT_AlterColumnType:
			return "ALTER COLUMN ... SET DATA TYPE";
		case AT_AlterColumnGenericOptions:
			return "ALTER COLUMN ... OPTIONS";
		case AT_ChangeOwner:
			return "OWNER TO";
		case AT_ClusterOn:
			return "CLUSTER ON";
		case AT_DropCluster:
			return "SET WITHOUT CLUSTER";
		case AT_SetAccessMethod:
			return "SET ACCESS METHOD";
		case AT_SetLogged:
			return "SET LOGGED";
		case AT_SetUnLogged:
			return "SET UNLOGGED";
		case AT_DropOids:
			return "SET WITHOUT OIDS";
		case AT_SetTableSpace:
			return "SET TABLESPACE";
		case AT_SetRelOptions:
			return "SET";
		case AT_ResetRelOptions:
			return "RESET";
		case AT_ReplaceRelOptions:
			return NULL;		/* 非真实语法 */
		case AT_EnableTrig:
			return "ENABLE TRIGGER";
		case AT_EnableAlwaysTrig:
			return "ENABLE ALWAYS TRIGGER";
		case AT_EnableReplicaTrig:
			return "ENABLE REPLICA TRIGGER";
		case AT_DisableTrig:
			return "DISABLE TRIGGER";
		case AT_EnableTrigAll:
			return "ENABLE TRIGGER ALL";
		case AT_DisableTrigAll:
			return "DISABLE TRIGGER ALL";
		case AT_EnableTrigUser:
			return "ENABLE TRIGGER USER";
		case AT_DisableTrigUser:
			return "DISABLE TRIGGER USER";
		case AT_EnableRule:
			return "ENABLE RULE";
		case AT_EnableAlwaysRule:
			return "ENABLE ALWAYS RULE";
		case AT_EnableReplicaRule:
			return "ENABLE REPLICA RULE";
		case AT_DisableRule:
			return "DISABLE RULE";
		case AT_AddInherit:
			return "INHERIT";
		case AT_DropInherit:
			return "NO INHERIT";
		case AT_AddOf:
			return "OF";
		case AT_DropOf:
			return "NOT OF";
		case AT_ReplicaIdentity:
			return "REPLICA IDENTITY";
		case AT_EnableRowSecurity:
			return "ENABLE ROW SECURITY";
		case AT_DisableRowSecurity:
			return "DISABLE ROW SECURITY";
		case AT_ForceRowSecurity:
			return "FORCE ROW SECURITY";
		case AT_NoForceRowSecurity:
			return "NO FORCE ROW SECURITY";
		case AT_GenericOptions:
			return "OPTIONS";
		case AT_AttachPartition:
			return "ATTACH PARTITION";
		case AT_DetachPartition:
			return "DETACH PARTITION";
		case AT_DetachPartitionFinalize:
			return "DETACH PARTITION ... FINALIZE";
		case AT_AddIdentity:
			return "ALTER COLUMN ... ADD IDENTITY";
		case AT_SetIdentity:
			return "ALTER COLUMN ... SET";
		case AT_DropIdentity:
			return "ALTER COLUMN ... DROP IDENTITY";
		case AT_ReAddStatistics:
			return NULL;		/* 非真实语法 */
	}

	return NULL;
}

/*
 * ATSimplePermissions
 *
 * - 确保它是关系（或者可能是视图）    
 * - 确保该用户是属主     
 * - 确保它不是系统表            
 */
static void
ATSimplePermissions(AlterTableType cmdtype, Relation rel, int allowed_targets)
{
	int			actual_target;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			actual_target = ATT_TABLE;
			break;
		case RELKIND_VIEW:
			actual_target = ATT_VIEW;
			break;
		case RELKIND_MATVIEW:
			actual_target = ATT_MATVIEW;
			break;
		case RELKIND_INDEX:
			actual_target = ATT_INDEX;
			break;
		case RELKIND_PARTITIONED_INDEX:
			actual_target = ATT_PARTITIONED_INDEX;
			break;
		case RELKIND_COMPOSITE_TYPE:
			actual_target = ATT_COMPOSITE_TYPE;
			break;
		case RELKIND_FOREIGN_TABLE:
			actual_target = ATT_FOREIGN_TABLE;
			break;
		case RELKIND_SEQUENCE:
			actual_target = ATT_SEQUENCE;
			break;
		default:
			actual_target = 0;
			break;
	}

	/* Wrong tar获取 类型? */
	if ((actual_target & allowed_targets) == 0)
	{
		const char *action_str = alter_table_type_to_string(cmdtype);

		if (action_str)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			/* translator: %s is a group of some SQL keywords */
					 errmsg("ALTER action %s cannot be performed on relation \"%s\"",
							action_str, RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
		else
			/* 内部 错误? */
			elog(ERROR, "invalid ALTER action attempted on relation \"%s\"",
				 RelationGetRelationName(rel));
	}

	/* 权限检查 */
	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}

/*
 * ATSimpleRecursion
 *
 * 适用于大多数 ALTER TABLE 操作的简单表递归。        
 * 所有直接和间接子表都会以未指定的顺序处理。        
 * 注意，若子表通过多个继承路径继承自原表，它将
 * 只会被访问一次。                        
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	/*
	 * Propagate to children, if desired and if there are (or 可能) any children.
	 *
	 */
	if (recurse && rel->rd_rel->relhassubclass)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		children = find_all_inheritors(relid, lockmode, NULL);

		/*
		 * find_all_inheritors does the recursive search of the 继承 hierarchy, so all
		 * we have to do is process all of the relids in the list that it 返回.
		 *
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			/* find_all_inheritors 已获取锁 */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * Obtain list of partitions of the given table, locking them all at the given
 * lockmode and ensuring that they all pass CheckAlterTableIsSafe. This
 * function is a no-op if the given relation is not a 分区表; in particular,
 * nothing is done if it's a legacy 继承 parent.
 *
 */
static void
ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode)
{
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *inh;
		ListCell   *cell;

		inh = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
		/* 第一 element is the 父 rel; must ignore it */
		for_each_from(cell, inh, 1)
		{
			Relation	childrel;

			/* find_all_inheritors 已获取锁 */
			childrel = table_open(lfirst_oid(cell), NoLock);
			CheckAlterTableIsSafe(childrel);
			table_close(childrel, NoLock);
		}
		list_free(inh);
	}
}

/*
 * ATTypedTableRecursion
 *
 * 将 ALTER TYPE 操作传播到该类型的类型化表。        
 * 并检查 RESTRICT/CASCADE 行为。给定 CASCADE 时，也允许   
 * 向类型化表的继承子表递归。               
 */
static void
ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	ListCell   *child;
	List	   *children;

	Assert(rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

	children = find_typed_table_dependencies(rel->rd_rel->reltype,
											 RelationGetRelationName(rel),
											 cmd->behavior);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, lockmode);
		CheckAlterTableIsSafe(childrel);
		ATPrepCmd(wqueue, childrel, cmd, true, true, lockmode, context);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_composite_type_dependencies Check to see if the type "typeOid" is
 * being used as a column in some table (possibly nested several levels deep
 * in composite types, arrays, etc!). Eventually, we'd like to propagate the
 * check or 重写 operation into such tables, but for now, just error out if we
 * find any. Caller should provide either the associated relation of a
 * rowtype, or a type name (not both) for use in the error message, 如有. 请注意
 * "typeOid" is not necessarily a 复合类型; it could 也 be another container type
 * 例如 an array or range, or a domain over one of these things. The name of
 * this function is 因此 somewhat historical, but it's not worth changing. We
 * assume that functions and views depending on the type are not reasons to
 * reject the ALTER. (How safe is this really?)
 *
 *
 *
 *
 *
 *
 */
void
find_composite_type_dependencies(Oid typeOid, Relation origRelation,
								 const char *origTypeName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	/* 由于此函数递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/*
	 * We 扫描 pg_depend to find those things that depend on the given type. (We
	 * assume 我们可以 ignore refobjsubid for a type.)
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		Relation	rel;
		TupleDesc	tupleDesc;
		Form_pg_attribute att;

		/* Check for directly dependent 类型s */
		if (pg_depend->classid == TypeRelationId)
		{
			/*
			 * This 必须是 an array, domain, or range containing the given type, so 递归地
			 * check for uses of this type. 请注意 any error message will mention the
			 * original type not the container; this is intentional.
			 *
			 */
			find_composite_type_dependencies(pg_depend->objid,
											 origRelation, origTypeName);
			continue;
		}

		/* Else, ignore dependees that aren't 关系s */
		if (pg_depend->classid != RelationRelationId)
			continue;

		rel = relation_open(pg_depend->objid, AccessShareLock);
		tupleDesc = RelationGetDescr(rel);

		/*
		 * If objsubid identifies a specific column, refer to that in error
		 * messages. Otherwise, search to see if there's a 用户 column of the type.
		 * (We assume 系统列 are 从不 of interesting types.) The search 是必需的 因为 an 索引
		 * containing an expression column of the target type will just be recorded
		 * as a whole-relation 依赖. If we do not find a column of the type, the 依赖
		 * must indicate that the type is transiently referenced in an 索引 expression
		 * but not stored on disk, which we assume is OK, just as we do for
		 * references in views. (It could 也 be that the target type is embedded in
		 * some container type that is stored in an 索引 column, but the previous 递归
		 * should catch such cases.)
		 *
		 */
		if (pg_depend->objsubid > 0 && pg_depend->objsubid <= tupleDesc->natts)
			att = TupleDescAttr(tupleDesc, pg_depend->objsubid - 1);
		else
		{
			att = NULL;
			for (int attno = 1; attno <= tupleDesc->natts; attno++)
			{
				att = TupleDescAttr(tupleDesc, attno - 1);
				if (att->atttypid == typeOid && !att->attisdropped)
					break;
				att = NULL;
			}
			if (att == NULL)
			{
				/* No such 列, so assume OK */
				relation_close(rel, AccessShareLock);
				continue;
			}
		}

		/*
		 * We definitely should reject if the relation has 存储. If it's partitioned,
		 * then perhaps 我们不 have to reject: if there are partitions then we'll fail
		 * when we find one, else there is no stored data to worry about. However,
		 * it's possible that the type change would affect conclusions about whether
		 * the type is sortable or hashable and thus (if it's a partitioning column)
		 * break the partitioning rule. For now, reject for partitioned rels too.
		 *
		 */
		if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind) ||
			RELKIND_HAS_PARTITIONS(rel->rd_rel->relkind))
		{
			if (origTypeName)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								origTypeName,
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter foreign table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
		}
		else if (OidIsValid(rel->rd_rel->reltype))
		{
			/*
			 * A view or 复合类型 itself isn't a problem, but 我们必须 递归地 check for indirect
			 * 依赖 via its rowtype.
			 */
			find_composite_type_dependencies(rel->rd_rel->reltype,
											 origRelation, origTypeName);
		}

		relation_close(rel, AccessShareLock);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);
}


/*
 * find_typed_table_dependencies
 *
 * 检查某个复合类型是否正被用作某个类型化      
 * 表的类型。若发现且行为是 RESTRICT，则中止。  
 * 否则返回该表列表。    
 */
static List *
find_typed_table_dependencies(Oid typeOid, const char *typeName, DropBehavior behavior)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_reloftype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

		if (behavior == DROP_RESTRICT)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot alter type \"%s\" because it is the type of a typed table",
							typeName),
					 errhint("Use ALTER ... CASCADE to alter the typed tables too.")));
		else
			result = lappend_oid(result, classform->oid);
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);

	return result;
}


/*
 * check_of_type Check whether a type is suitable for CREATE TABLE OF/ALTER
 * TABLE OF. If it isn't suitable, throw an error. Currently, we require that
 * the type originated with CREATE TYPE AS. We could support any 行类型, but
 * doing so would require handling a number of extra corner cases in the DDL
 * commands. (Also, allowing domain-over-composite would open up a can of
 * worms about whether and how the domain's 约束 should apply to derived
 * tables.)
 *
 */
void
check_of_type(HeapTuple typetuple)
{
	Form_pg_type typ = (Form_pg_type) GETSTRUCT(typetuple);
	bool		typeOk = false;

	if (typ->typtype == TYPTYPE_COMPOSITE)
	{
		Relation	typeRelation;

		Assert(OidIsValid(typ->typrelid));
		typeRelation = relation_open(typ->typrelid, AccessShareLock);
		typeOk = (typeRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until 事务 提交.
		 * That will prevent someone else from deleting or ALTERing the type 之前 the
		 * 类型化表 creation/conversion 提交.
		 */
		relation_close(typeRelation, NoLock);
	}
	if (!typeOk)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("type %s is not a composite type",
						format_type_be(typ->oid))));
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * 向一个关系添加附加属性，前提是 CHECK、NOT NULL 与   
 * FOREIGN KEY 约束会从 parse_utilcmd.c 中的 AT_AddColumn         
 * AlterTableCmd 中删除，并被添加为独立的                    
 * AlterTableCmd。
 *
 * ADD COLUMN 不能使用普通的 ALTER TABLE 递归机制，因为我们    
 * 必须在运行时决定是否递归，这取决于我们实际是添加   
 * 一个列还是仅仅与现有列合并。（我们无法在静态的预 
 * 检查中完成这一点，因为它不能正确处理多重继承的       
 * 情况。）          
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse && !is_view)
		cmd->recurse = true;
}

/*
 * 向表添加一个列。返回值是新列在父关             
 * 系中的地址。                
 *
 * cmd 以传引用方式传入，这样我们能用解析转换后的副 
 * 本替换它（但只在检查 IF NOT EXISTS 后发生）。  
 */
static ObjectAddress
ATExecAddColumn(List **wqueue, AlteredTableInfo *tab, Relation rel,
				AlterTableCmd **cmd,
				bool recurse, bool recursing,
				LOCKMODE lockmode, int cur_pass,
				AlterTableUtilityContext *context)
{
	Oid			myrelid = RelationGetRelid(rel);
	ColumnDef  *colDef = castNode(ColumnDef, (*cmd)->def);
	bool		if_not_exists = (*cmd)->missing_ok;
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	FormData_pg_attribute attribute;
	int			newattnum;
	char		relkind;
	HeapTuple	typeTuple;
	Oid			typeOid;
	int32		typmod;
	Oid			collOid;
	Form_pg_type tform;
	Expr	   *defval;
	List	   *children;
	ListCell   *child;
	AlterTableCmd *childcmd;
	AclResult	aclresult;
	ObjectAddress address;
	TupleDesc	tupdesc;
	FormData_pg_attribute *aattr[] = {&attribute};

	/* 由于此函数递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/* 在顶层，权限检查已在 ATPrepCmd 中完成，否则在此执行 */
	if (recursing)
		ATSimplePermissions((*cmd)->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to a partition")));

	attrdesc = table_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a 递归 child? If so, check whether to merge with
	 * an existing definition for the column. If we do merge, 我们必须 not 递归.
	 * Children will 已经 have the column, and 递归中 into them would mess up
	 * attinhcount.
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/* Does 子 已经 have a 列 by this 名称? */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);
			Oid			ctypeId;
			int32		ctypmod;
			Oid			ccollid;

			/* Child 列 must match on 类型, typmod, and 排序规则 */
			typenameTypeIdAndMod(NULL, colDef->typeName, &ctypeId, &ctypmod);
			if (ctypeId != childatt->atttypid ||
				ctypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname)));
			ccollid = GetColumnDefCollation(NULL, colDef, ctypeId);
			if (ccollid != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname),
						 errdetail("\"%s\" versus \"%s\"",
								   get_collation_name(ccollid),
								   get_collation_name(childatt->attcollation))));

			/* Bump the existing 子 att's inhcount */
			childatt->attinhcount++;
			if (childatt->attinhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			CatalogTupleUpdate(attrdesc, &tuple->t_self, tuple);

			heap_freetuple(tuple);

			/* Inform the 用户 about the merge */
			ereport(NOTICE,
					(errmsg("merging definition of column \"%s\" for child \"%s\"",
							colDef->colname, RelationGetRelationName(rel))));

			table_close(attrdesc, RowExclusiveLock);

			/* Make the 子 列 变更 可见 */
			CommandCounterIncrement();

			return InvalidObjectAddress;
		}
	}

	/* 跳过 if the 名称 已经 exists and if_not_exists is 真 */
	if (!check_for_column_name_collision(rel, colDef->colname, if_not_exists))
	{
		table_close(attrdesc, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/*
	 * Okay, 我们需要 add the column, so go ahead and do 解析转换. This can result in
	 * queueing up, or even immediately executing, subsidiary operations (例如
	 * creation of unique 索引); so 我们必须n't do it until we have made the
	 * if_not_exists check. When 递归中, the command was 已经 转换后的 and we needn't do
	 * so again. Also, if context isn't given 我们可以't 转换. (That 当前 happens 仅 for
	 * AT_AddColumnToView; we expect that view.c passed us a ColumnDef that
	 * doesn't need work.)
	 *
	 *
	 */
	if (context != NULL && !recursing)
	{
		*cmd = ATParseTransformCmd(wqueue, tab, rel, *cmd, recurse, lockmode,
								   cur_pass, context);
		Assert(*cmd != NULL);
		colDef = castNode(ColumnDef, (*cmd)->def);
	}

	/*
	 * Cannot add 标识列 if table has children, 因为 identity does not 继承. (Adding
	 * column and identity separately will work.)
	 */
	if (colDef->identity &&
		recurse &&
		find_inheritance_children(myrelid, NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot recursively add identity column to table that has child tables")));

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relkind = ((Form_pg_class) GETSTRUCT(reltup))->relkind;

	/* Determine the 新 属性's 编号 */
	newattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts + 1;
	if (newattnum > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	typeTuple = typenameType(NULL, colDef->typeName, &typmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	typeOid = tform->oid;

	aclresult = object_aclcheck(TypeRelationId, typeOid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, typeOid);

	collOid = GetColumnDefCollation(NULL, colDef, typeOid);

	/* 使 sure 数据类型 is legal for a 列 */
	CheckAttributeType(colDef->colname, typeOid, collOid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	/*
	 * Construct new 属性's pg_attribute entry. (Variable-length fields are handled
	 * by InsertPgAttributeTuples().)
	 */
	attribute.attrelid = myrelid;
	namestrcpy(&(attribute.attname), colDef->colname);
	attribute.atttypid = typeOid;
	attribute.attstattarget = (newattnum > 0) ? -1 : 0;
	attribute.attlen = tform->typlen;
	attribute.attnum = newattnum;
	if (list_length(colDef->typeName->arrayBounds) > PG_INT16_MAX)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many array dimensions"));
	attribute.attndims = list_length(colDef->typeName->arrayBounds);
	attribute.atttypmod = typmod;
	attribute.attbyval = tform->typbyval;
	attribute.attalign = tform->typalign;
	if (colDef->storage_name)
		attribute.attstorage = GetAttributeStorage(typeOid, colDef->storage_name);
	else
		attribute.attstorage = tform->typstorage;
	attribute.attcompression = GetAttributeCompression(typeOid,
													   colDef->compression);
	attribute.attnotnull = colDef->is_not_null;
	attribute.atthasdef = false;
	attribute.atthasmissing = false;
	attribute.attidentity = colDef->identity;
	attribute.attgenerated = colDef->generated;
	attribute.attisdropped = false;
	attribute.attislocal = colDef->is_local;
	attribute.attinhcount = colDef->inhcount;
	attribute.attcollation = collOid;

	ReleaseSysCache(typeTuple);

	tupdesc = CreateTupleDesc(lengthof(aattr), (FormData_pg_attribute **) &aattr);

	InsertPgAttributeTuples(attrdesc, tupdesc, myrelid, NULL, NULL);

	table_close(attrdesc, RowExclusiveLock);

	/*
	 * Update pg_class 元组 as appropriate
	 */
	((Form_pg_class) GETSTRUCT(reltup))->relnatts = newattnum;

	CatalogTupleUpdate(pgclass, &reltup->t_self, reltup);

	heap_freetuple(reltup);

	/* Post creation hook for 新 属性 */
	InvokeObjectPostCreateHook(RelationRelationId, myrelid, newattnum);

	table_close(pgclass, RowExclusiveLock);

	/* Make the 属性's 目录 条目 可见 */
	CommandCounterIncrement();

	/*
	 * Store the DEFAULT, 如有, in the 目录
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute.attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);
		rawEnt->missingMode = false;	/* XXX vestigial */
		rawEnt->generated = colDef->generated;

		/*
		 * This function is intended for CREATE TABLE, so it 处理 a _list_ of
		 * defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);

		/* Make the 添加itional 目录 变更s 可见 */
		CommandCounterIncrement();
	}

	/*
	 * Tell Phase 3 to fill in the 默认表达式, if there is one. If there is no
	 * default, Phase 3 doesn't have to do anything, 因为 that effectively means
	 * that the default is NULL. The 堆 元组 access routines 始终 check for attnum > #
	 * of 属性 in 元组, and return NULL 如果是这样, so 没有 any modification of the 元组 data
	 * we will get the effect of NULL values in the new column. An exception
	 * occurs when the new column is of a domain type: the domain might have a
	 * NOT NULL 约束, or a CHECK 约束 that indirectly rejects nulls. If there are any
	 * domain 约束 then we construct an explicit NULL 默认值 that 将被 passed through
	 * CoerceToDomain processing. (This is a tad inefficient, since it causes
	 * rewriting the table which we really wouldn't have to do; but we do it to
	 * preserve the historical behavior that such a failure 将被 raised 仅 if the
	 * table 当前 包含 some rows.) Note: we use build_column_default, and not just
	 * the cooked default returned by AddRelationNewConstraints, 从而 the right
	 * thing happens when a datatype's default applies. Note: it might seem that
	 * this should happen at the end of Phase 2, 从而 the effects of subsequent 子命令
	 * can be taken into account. It's intentional that we do it now, though. The
	 * new column 应是 filled according to what is said in the ADD COLUMN 子命令, 从而
	 * the effects are the same as if this 子命令 had been run by itself and the 稍后的
	 * 子命令 had been issued in new ALTER TABLE commands. 我们可以 skip this entirely
	 * for relations 没有 存储, since Phase 3 is certainly not going to touch them.
	 * System 属性 don't have interesting defaults, either.
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 */
	if (RELKIND_HAS_STORAGE(relkind) && attribute.attnum > 0)
	{
		bool		has_domain_constraints;
		bool		has_missing = false;

		/*
		 * For an 标识列, 我们可以't use build_column_default(), 因为 the 序列 ownership isn't
		 * set yet. So do it manually.
		 */
		if (colDef->identity)
		{
			NextValueExpr *nve = makeNode(NextValueExpr);

			nve->seqid = RangeVarGetRelid(colDef->identitySequence, NoLock, false);
			nve->typeId = typeOid;

			defval = (Expr *) nve;
		}
		else
			defval = (Expr *) build_column_default(rel, attribute.attnum);

		/* Build CoerceToDomain(NULL) 表达式 if 需要ed */
		has_domain_constraints = DomainHasConstraints(typeOid);
		if (!defval && has_domain_constraints)
		{
			Oid			baseTypeId;
			int32		baseTypeMod;
			Oid			baseTypeColl;

			baseTypeMod = typmod;
			baseTypeId = getBaseTypeAndTypmod(typeOid, &baseTypeMod);
			baseTypeColl = get_typcollation(baseTypeId);
			defval = (Expr *) makeNullConst(baseTypeId, baseTypeMod, baseTypeColl);
			defval = (Expr *) coerce_to_target_type(NULL,
													(Node *) defval,
													baseTypeId,
													typeOid,
													typmod,
													COERCION_ASSIGNMENT,
													COERCE_IMPLICIT_CAST,
													-1);
			if (defval == NULL) /* 不应发生 */
				elog(ERROR, "failed to coerce base type to domain");
		}

		if (defval)
		{
			NewColumnValue *newval;

			/* Prepare defval for 执行, either 此处 or in 阶段 3 */
			defval = expression_planner(defval);

			/* Add the 新 默认值 to the 新vals 列表 */
			newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
			newval->attnum = attribute.attnum;
			newval->expr = defval;
			newval->is_generated = (colDef->generated != '\0');

			tab->newvals = lappend(tab->newvals, newval);

			/*
			 * Attempt to skip a complete table 重写 by storing the specified DEFAULT
			 * value outside of the 堆. This is 仅 allowed for plain relations and
			 * non-生成列, and the 默认表达式 can't be volatile (stable is OK). 请注意
			 * contain_volatile_functions deems CoerceToDomain immutable, but here we
			 * consider that coercion to a domain with 约束 is volatile; else it might
			 * fail even when the table is empty.
			 *
			 */
			if (rel->rd_rel->relkind == RELKIND_RELATION &&
				!colDef->generated &&
				!has_domain_constraints &&
				!contain_volatile_functions((Node *) defval))
			{
				EState	   *estate;
				ExprState  *exprState;
				Datum		missingval;
				bool		missingIsNull;

				/* Evaluate the 默认值 表达式 */
				estate = CreateExecutorState();
				exprState = ExecPrepareExpr(defval, estate);
				missingval = ExecEvalExpr(exprState,
										  GetPerTupleExprContext(estate),
										  &missingIsNull);
				/* If it turns out NULL, nothing to 执行; else store it */
				if (!missingIsNull)
				{
					StoreAttrMissingVal(rel, attribute.attnum, missingval);
					/* Make the 添加itional 目录 变更 可见 */
					CommandCounterIncrement();
					has_missing = true;
				}
				FreeExecutorState(estate);
			}
			else
			{
				/*
				 * Failed to use missing mode. We have to do a table 重写 to install the
				 * value.
				 */
				tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
			}
		}

		if (!has_missing)
		{
			/*
			 * If the new column is NOT NULL, and there is no missing value,
			 * tell Phase 3 it needs to check for NULLs.
			 */
			tab->verify_new_notnull |= colDef->is_not_null;
		}
	}

	/*
	 * Add needed 依赖 entries for the new column.
	 */
	add_column_datatype_dependency(myrelid, newattnum, attribute.atttypid);
	add_column_collation_dependency(myrelid, newattnum, attribute.attcollation);

	/*
	 * Propagate to children as appropriate. Unlike most other ALTER routines, we
	 * have to do this one level of 递归 at a time; 我们可以't use find_all_inheritors
	 * to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * If we are told not to 递归, there had better not be any child tables; else
	 * the addition would put them out of step.
	 */
	if (children && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column must be added to child tables too")));

	/* Children should see 列 as singly 继承ed */
	if (!recursing)
	{
		childcmd = copyObject(*cmd);
		colDef = castNode(ColumnDef, childcmd->def);
		colDef->inhcount = 1;
		colDef->is_local = false;
	}
	else
		childcmd = *cmd;		/* no 需要 to 复制 again */

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children 已获取锁 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/* Find or 创建 工作 队列 条目 for this 表 */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to 子; return 值 is ignored */
		ATExecAddColumn(wqueue, childtab, childrel,
						&childcmd, recurse, true,
						lockmode, cur_pass, context);

		table_close(childrel, NoLock);
	}

	ObjectAddressSubSet(address, RelationRelationId, myrelid, newattnum);
	return address;
}

/*
 * If a new or renamed column will collide with the name of an existing
 * column and if_not_exists is false then error out, else do nothing.
 */
static bool
check_for_column_name_collision(Relation rel, const char *colname,
								bool if_not_exists)
{
	HeapTuple	attTuple;
	int			attnum;

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a 已删除列 name, it's gonna fail anyway.
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   PointerGetDatum(colname));
	if (!HeapTupleIsValid(attTuple))
		return true;

	attnum = ((Form_pg_attribute) GETSTRUCT(attTuple))->attnum;
	ReleaseSysCache(attTuple);

	/*
	 * We throw a different error message for conflicts with system 列名, since
	 * they are normally not shown and the 用户 might 否则 be confused about the
	 * reason for the conflict.
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column name \"%s\" conflicts with a system column name",
						colname)));
	else
	{
		if (if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" already exists, skipping",
							colname, RelationGetRelationName(rel))));
			return false;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						colname, RelationGetRelationName(rel))));
	}

	return true;
}

/*
 * Install a column's 依赖 on its datatype.
 */
static void
add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
	ObjectAddress myself,
				referenced;

	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = typid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * Install a column's 依赖 on its 排序规则.
 */
static void
add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
	ObjectAddress myself,
				referenced;

	/* We k现在 the 默认值 排序规则 is pinned, so 执行n't bother recording it */
	if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
	{
		myself.classId = RelationRelationId;
		myself.objectId = relid;
		myself.objectSubId = attnum;
		referenced.classId = CollationRelationId;
		referenced.objectId = collid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */

static void
ATPrepDropNotNull(Relation rel, bool recurse, bool recursing)
{
	/*
	 * If the parent is a 分区表, like check 约束, we do not support removing the NOT
	 * NULL while partitions exist.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);

		Assert(partdesc != NULL);
		if (partdesc->nparts > 0 && !recurse && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot remove constraint from only the partitioned table when partitions exist"),
					 errhint("Do not specify the ONLY keyword.")));
	}
}

/*
 * Return the address of the modified column. If the column was 已经 nullable,
 * InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	ObjectAddress address;

	/*
	 * lookup the 属性
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* 防止它们修改系统属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel))));

	/*
	 * 检查 the 属性 is not in a 主键 or in an 索引 used as a 副本标识. Note: we'll throw
	 * error even if the pkey 索引 is not valid.
	 *
	 *
	 */

	/* Loop over 所有 索引es on the 关系 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * If the 索引 is not a 主键 or an 索引 used as 副本标识, skip the check.
		 *
		 */
		if (indexStruct->indisprimary || indexStruct->indisreplident)
		{
			/*
			 * 遍历主键或用作副本标识的索引中的每个属    
			 * 性，看看它是否与被修改的属性相              
			 * 匹配。 
			 */
			for (i = 0; i < indexStruct->indnkeyatts; i++)
			{
				if (indexStruct->indkey.values[i] == attnum)
				{
					if (indexStruct->indisprimary)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("column \"%s\" is in a primary key",
										colName)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("column \"%s\" is in index used as replica identity",
										colName)));
				}
			}
		}

		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/* If rel is 分区, shouldn't 删除 NOT NULL if 父 has the 相同 */
	if (rel->rd_rel->relispartition)
	{
		Oid			parentId = get_partition_parent(RelationGetRelid(rel), false);
		Relation	parent = table_open(parentId, AccessShareLock);
		TupleDesc	tupDesc = RelationGetDescr(parent);
		AttrNumber	parent_attnum;

		parent_attnum = get_attnum(parentId, colName);
		if (TupleDescAttr(tupDesc, parent_attnum - 1)->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" is marked NOT NULL in parent table",
							colName)));
		table_close(parent, AccessShareLock);
	}

	/*
	 * Okay, actually perform the 目录 change ... if needed
	 */
	if (attTup->attnotnull)
	{
		attTup->attnotnull = false;

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	table_close(attr_rel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */

static void
ATPrepSetNotNull(List **wqueue, Relation rel,
				 AlterTableCmd *cmd, bool recurse, bool recursing,
				 LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	/*
	 * If we're 已经 递归中, there's nothing to do; the topmost invocation of
	 * ATSimpleRecursion 已经 visited all children.
	 */
	if (recursing)
		return;

	/*
	 * If the target column is 已经 marked NOT NULL, 我们可以 skip 递归中 to children, 因为
	 * their columns should 已经 be marked NOT NULL 同时. But there's no point in
	 * checking here unless the relation has some children; else 我们可以 just wait
	 * till 执行 to check. (If it does have children, 然而, this can save taking
	 * per-child 锁 unnecessarily. This greatly improves concurrency in some
	 * parallel restore scenarios.) Unfortunately, 我们可以 仅 apply this optimization
	 * to partitioned tables, 因为 traditional 继承 doesn't enforce that child
	 * columns be NOT NULL when their parent is. (That's a bug that should get
	 * fixed someday.)
	 *
	 *
	 *
	 */
	if (rel->rd_rel->relhassubclass &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		HeapTuple	tuple;
		bool		attnotnull;

		tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);

		/* Might 也 th行 the 错误 现在, if 名称 is bad */
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cmd->name, RelationGetRelationName(rel))));

		attnotnull = ((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull;
		ReleaseSysCache(tuple);
		if (attnotnull)
			return;
	}

	/*
	 * If we have ALTER TABLE ONLY ... SET NOT NULL on a 分区表, apply ALTER TABLE
	 * ... CHECK NOT NULL to every child. Otherwise, use normal 递归 logic.
	 *
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		!recurse)
	{
		AlterTableCmd *newcmd = makeNode(AlterTableCmd);

		newcmd->subtype = AT_CheckNotNull;
		newcmd->name = pstrdup(cmd->name);
		ATSimpleRecursion(wqueue, rel, newcmd, true, lockmode, context);
	}
	else
		ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
}

/*
 * Return the address of the modified column. If the column was 已经 NOT NULL,
 * InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	ObjectAddress address;

	/*
	 * lookup the 属性
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* 防止它们修改系统属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Okay, actually perform the 目录 change ... if needed
	 */
	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = true;

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		/*
		 * Ordinarily phase 3 must ensure that no NULLs exist in columns that 被设置
		 * NOT NULL; 然而, if 我们可以 find a 约束 which proves this then 我们可以 skip that. We
		 * needn't bother looking if we've 已经 found that 我们必须 verify some other NOT
		 * NULL 约束.
		 */
		if (!tab->verify_new_notnull &&
			!NotNullImpliedByRelConstraints(rel, (Form_pg_attribute) GETSTRUCT(tuple)))
		{
			/* Tell 阶段 3 it 需要s to test the 约束 */
			tab->verify_new_notnull = true;
		}

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	table_close(attr_rel, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN CHECK NOT NULL This doesn't exist in the grammar,
 * but we generate AT_CheckNotNull commands against the partitions of a 分区表 if
 * the 用户 写入 ALTER TABLE ONLY ... SET NOT NULL on the 分区表, or tries to create
 * a 主键 on it (which internally 创建 AT_SetNotNull on the 分区表). Such a command
 * doesn't allow us to actually modify any partition, but we want to let it go
 * through if the partitions are 已经 properly marked. In future, this might
 * need to adjust the 子表's state, likely by incrementing an 继承 count for the
 * attnotnull 约束. For now we need 仅 check for the presence of the flag.
 *
 *
 *
 *
 *
 */
static void
ATExecCheckNotNull(AlteredTableInfo *tab, Relation rel,
				   const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too"),
				 errdetail("Column \"%s\" of relation \"%s\" is not already NOT NULL.",
						   colName, RelationGetRelationName(rel)),
				 errhint("Do not specify the ONLY keyword.")));

	ReleaseSysCache(tuple);
}

/*
 * NotNullImpliedByRelConstraints Does rel's existing 约束 imply NOT NULL for
 * the given 属性?
 */
static bool
NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr)
{
	NullTest   *nnulltest = makeNode(NullTest);

	nnulltest->arg = (Expr *) makeVar(1,
									  attr->attnum,
									  attr->atttypid,
									  attr->atttypmod,
									  attr->attcollation,
									  0);
	nnulltest->nulltesttype = IS_NOT_NULL;

	/*
	 * argisrow = false is correct even for a composite column, 因为 attnotnull
	 * does not represent a SQL-spec IS NOT NULL test in such a case, just IS
	 * DISTINCT FROM NULL.
	 */
	nnulltest->argisrow = false;
	nnulltest->location = -1;

	if (ConstraintImpliedByRelConstraint(rel, list_make1(nnulltest), NIL))
	{
		ereport(DEBUG1,
				(errmsg_internal("existing constraints on column \"%s.%s\" are sufficient to prove that it does not contain nulls",
								 RelationGetRelationName(rel), NameStr(attr->attname))));
		return true;
	}

	return false;
}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * get the number of the 属性
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* 防止它们修改系统属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (TupleDescAttr(tupdesc, attnum - 1)->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel)),
		/* translator: %s is an SQL ALTER command */
				 newDefault ? 0 : errhint("Use %s instead.",
										  "ALTER TABLE ... ALTER COLUMN ... DROP IDENTITY")));

	if (TupleDescAttr(tupdesc, attnum - 1)->attgenerated)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is a generated column",
						colName, RelationGetRelationName(rel)),
				 newDefault || TupleDescAttr(tupdesc, attnum - 1)->attgenerated != ATTRIBUTE_GENERATED_STORED ? 0 :
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE ... ALTER COLUMN ... DROP EXPRESSION")));

	/*
	 * Remove any old default for the column. We use RESTRICT here for safety,
	 * but at present we do not expect anything to depend on the default. We
	 * treat removing the existing default as an internal operation when it is
	 * preparatory to adding a new default, but as a 用户-initiated operation when
	 * the 用户 asked for a drop.
	 *
	 *
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  newDefault != NULL);

	if (newDefault)
	{
		/* SET DEFAULT */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;
		rawEnt->missingMode = false;
		rawEnt->generated = '\0';

		/*
		 * This function is intended for CREATE TABLE, so it 处理 a _list_ of
		 * defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * Add a pre-cooked 默认表达式. Return the address of the affected column.
 *
 *
 */
static ObjectAddress
ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
						  Node *newDefault)
{
	ObjectAddress address;

	/* We assume no 检查ing 是必需的 */

	/*
	 * Remove any old default for the column. We use RESTRICT here for safety,
	 * but at present we do not expect anything to depend on the default. (In
	 * ordinary cases, there could not be a default in place anyway, but it's
	 * possible when combining LIKE with 继承.)
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  true);

	(void) StoreAttrDefault(rel, attnum, newDefault, true, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN ADD IDENTITY
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecAddIdentity(Relation rel, const char *colName,
				  Node *def, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	ObjectAddress address;
	ColumnDef  *cdef = castNode(ColumnDef, def);

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't 修改 a system 属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Creating a column as identity implies NOT NULL, so adding the identity to
	 * an existing column that is not NOT NULL would create a state that 不能
	 * reproduced 没有 contortions.
	 */
	if (!attTup->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" must be declared NOT NULL before identity can be added",
						colName, RelationGetRelationName(rel))));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is already an identity column",
						colName, RelationGetRelationName(rel))));

	if (attTup->atthasdef)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" already has a default value",
						colName, RelationGetRelationName(rel))));

	attTup->attidentity = cdef->identity;
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET { GENERATED or 序列 options } Return the address
 * of the affected column.
 *
 */
static ObjectAddress
ATExecSetIdentity(Relation rel, const char *colName, Node *def, LOCKMODE lockmode)
{
	ListCell   *option;
	DefElem    *generatedEl = NULL;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;

	foreach(option, castNode(List, def))
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "generated") == 0)
		{
			if (generatedEl)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			generatedEl = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * Even if there is nothing to change here, we run all the 检查. There 将被 a
	 * subsequent ALTER SEQUENCE that relies on everything being there.
	 *
	 */

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
						colName, RelationGetRelationName(rel))));

	if (generatedEl)
	{
		attTup->attidentity = defGetInt32(generatedEl);
		CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  attTup->attnum);
		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	heap_freetuple(tuple);
	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN DROP IDENTITY
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;
	Oid			seqid;
	ObjectAddress seqaddress;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not an identity column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	attTup->attidentity = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/* 删除 the 内部 序列 */
	seqid = getIdentitySequence(RelationGetRelid(rel), attnum, false);
	deleteDependencyRecordsForClass(RelationRelationId, seqid,
									RelationRelationId, DEPENDENCY_INTERNAL);
	CommandCounterIncrement();
	seqaddress.classId = RelationRelationId;
	seqaddress.objectId = seqid;
	seqaddress.objectSubId = 0;
	performDeletion(&seqaddress, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN DROP EXPRESSION
 */
static void
ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode)
{
	/*
	 * Reject ONLY if there are child tables. We could implement this, but it is
	 * a bit complicated. GENERATED clauses 必须是 attached to the 列定义 and 不能 added
	 * 稍后的 like DEFAULT, so if a 子表 has a 生成表达式 that the parent does not have,
	 * the child column will necessarily be an attislocal column. So to implement
	 * ONLY here, we'd need extra code to update attislocal of the direct child
	 * tables, somewhat similar to how DROP COLUMN does it, 从而 the resulting
	 * state can be properly dumped and restored.
	 *
	 */
	if (!recurse &&
		find_inheritance_children(RelationGetRelid(rel), lockmode))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / DROP EXPRESSION must be applied to child tables too")));

	/*
	 * Cannot drop 生成表达式 from 继承的 columns.
	 */
	if (!recursing)
	{
		HeapTuple	tuple;
		Form_pg_attribute attTup;

		tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), cmd->name);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cmd->name, RelationGetRelationName(rel))));

		attTup = (Form_pg_attribute) GETSTRUCT(tuple);

		if (attTup->attinhcount > 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop generation expression from inherited column")));
	}
}

/*
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	Oid			attrdefoid;
	ObjectAddress address;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (attTup->attgenerated != ATTRIBUTE_GENERATED_STORED)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not a stored generated column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not a stored generated column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	/*
	 * Mark the column as no longer generated.  (The atthasdef flag needs to
	 * get cleared too, but RemoveAttrDefault will handle that.)
	 */
	attTup->attgenerated = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/*
	 * Drop the 依赖 records of the GENERATED expression, in particular its
	 * INTERNAL 依赖 on the column, which would 否则 cause 依赖.c to refuse to perform
	 * the deletion.
	 */
	attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);
	if (!OidIsValid(attrdefoid))
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 RelationGetRelid(rel), attnum);
	(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);

	/* Make above 变更s 可见 */
	CommandCounterIncrement();

	/*
	 * Get rid of the GENERATED expression itself.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT,
					  false, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStatistics(Relation rel, const char *colName, int16 colNum, Node *newValue, LOCKMODE lockmode)
{
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * We allow referencing columns by numbers 仅 for 索引, since table 列号 could
	 * contain gaps if columns are 稍后的 dropped.
	 */
	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX &&
		!colName)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot refer to non-index column by number")));

	Assert(IsA(newValue, Integer));
	newtarget = intVal(newValue);

	/*
	 * Limit target to a sane range
	 */
	if (newtarget < -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("statistics target %d is too low",
						newtarget)));
	}
	else if (newtarget > 10000)
	{
		newtarget = 10000;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lowering statistics target to %d",
						newtarget)));
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	if (colName)
	{
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
	}
	else
	{
		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(rel), colNum);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column number %d of relation \"%s\" does not exist",
							colNum, RelationGetRelationName(rel))));
	}

	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		if (attnum > rel->rd_index->indnkeyatts)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on included column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel))));
		else if (rel->rd_index->indkey.values[attnum - 1] != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on non-expression column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel)),
					 errhint("Alter statistics on table column instead.")));
	}

	attrtuple->attstattarget = newtarget;

	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetOptions(Relation rel, const char *colName, Node *options,
				 bool isReset, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple,
				newtuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	Datum		datum,
				newOptions;
	bool		isnull;
	ObjectAddress address;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* Generate 新 proposed att选项 (text array) */
	datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							&isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 castNode(List, options), NULL, NULL,
									 false, isReset);
	/* Val标识符ate 新 选项 */
	(void) attribute_reloptions(newOptions, true);

	/* Build 新 元组. */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);

	/* Update system 目录. */
	CatalogTupleUpdate(attrelation, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * Helper function for ATExecSetStorage and ATExecSetCompression Set the
 * attstorage and/or attcompression fields for 索引 columns associated with the
 * specified table column.
 *
 */
static void
SetIndexStorageProperties(Relation rel, Relation attrelation,
						  AttrNumber attnum,
						  bool setstorage, char newstorage,
						  bool setcompression, char newcompression,
						  LOCKMODE lockmode)
{
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(rel))
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	indrel;
		AttrNumber	indattnum = 0;
		HeapTuple	tuple;

		indrel = index_open(indexoid, lockmode);

		for (int i = 0; i < indrel->rd_index->indnatts; i++)
		{
			if (indrel->rd_index->indkey.values[i] == attnum)
			{
				indattnum = i + 1;
				break;
			}
		}

		if (indattnum == 0)
		{
			index_close(indrel, lockmode);
			continue;
		}

		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(indrel), indattnum);

		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

			if (setstorage)
				attrtuple->attstorage = newstorage;

			if (setcompression)
				attrtuple->attcompression = newcompression;

			CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

			InvokeObjectPostAlterHook(RelationRelationId,
									  RelationGetRelid(rel),
									  attrtuple->attnum);

			heap_freetuple(tuple);
		}

		index_close(indrel, lockmode);
	}
}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStorage(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attrtuple->attstorage = GetAttributeStorage(attrtuple->atttypid, strVal(newValue));

	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);

	/*
	 * Apply the change to 索引 同时 (仅 for simple 索引 columns, matching behavior of
	 * 索引.c ConstructTupleDescriptor()).
	 */
	SetIndexStorageProperties(rel, attrelation, attnum,
							  true, attrtuple->attstorage,
							  false, 0,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN 不能使用普通的 ALTER TABLE 递归机制，     
 * 因为我们必须在运行时决定是否递归，这取决于         
 * attinhcount 是否减到零。（我们无法在静态预检查中    
 * 完成这一点，因为它不能正确处理多重继承的情        
 * 况。）  
 */
static void
ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				 AlterTableCmd *cmd, LOCKMODE lockmode,
				 AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop column from typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse)
		cmd->recurse = true;
}

/*
 * Drops column 'colName' from relation 'rel' and 返回 the address of the 已删除列.
 * The column is 也 dropped (or marked as no longer 继承的 from relation) from the
 * relation's 继承 children, 如有. In the recursive invocations for 继承 child
 * relations, 相反 of dropping the column directly (if to be dropped at all),
 * its 对象地址 is added to 'addrs', which 必须是 non-NULL in such invocations. All
 * columns 被删除 at the same time 之后 all the children have been checked 递归地.
 *
 *
 *
 */
static ObjectAddress
ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing,
				 bool missing_ok, LOCKMODE lockmode,
				 ObjectAddresses *addrs)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;
	bool		is_expr;

	/* 在顶层，权限检查已在 ATPrepCmd 中完成，否则在此执行 */
	if (recursing)
		ATSimplePermissions(AT_DropColumn, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/* Initialize 添加rs on the 第一 invocation */
	Assert(!recursing || addrs != NULL);

	/* 由于此函数递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (!recursing)
		addrs = new_object_addresses();

	/*
	 * get the number of the 属性
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
							colName, RelationGetRelationName(rel))));
			return InvalidObjectAddress;
		}
	}
	targetatt = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = targetatt->attnum;

	/* Can't 删除 a system 属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName)));

	/*
	 * Don't drop 继承的 columns, unless 递归中 (presumably from a drop of the parent
	 * column)
	 */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName)));

	/*
	 * Don't drop columns used in the 分区键, either. (If we let this go through,
	 * the key column's 依赖 would cause a cascaded drop of the whole table, which
	 * is surely not what the 用户 expected.)
	 */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	ReleaseSysCache(tuple);

	/*
	 * Propagate to children as appropriate. Unlike most other ALTER routines, we
	 * have to do this one level of 递归 at a time; 我们可以't use find_all_inheritors
	 * to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		/*
		 * In case of a 分区表, the column 必须是 dropped from the partitions 同时.
		 *
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE && !recurse)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop column from only the partitioned table when partitions exist"),
					 errhint("Do not specify the ONLY keyword.")));

		attr_rel = table_open(AttributeRelationId, RowExclusiveLock);
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;
			Form_pg_attribute childatt;

			/* find_inheritance_children 已获取锁 */
			childrel = table_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))	/* 不应发生 */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0) /* 不应发生 */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * 若子列还有其他定义来源，只需递减其   
				 * 继承计数；如果没有，则递归将它删除。    
				 *    
				 */
				if (childatt->attinhcount == 1 && !childatt->attislocal)
				{
					/* Time to 删除 this 子 列, too */
					ATExecDropColumn(wqueue, childrel, colName,
									 behavior, true, true,
									 false, lockmode, addrs);
				}
				else
				{
					/* Child 列 must survive my deletion */
					childatt->attinhcount--;

					CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

					/* 使更新可见 */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * If we were told to drop ONLY in this table (no 递归), 我们需要 mark the 继承者'
				 * 属性 as locally defined rather than 继承的.
				 *
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

				/* 使更新可见 */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			table_close(childrel, NoLock);
		}
		table_close(attr_rel, RowExclusiveLock);
	}

	/* Add 对象 to 删除 */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;
	add_exact_object_address(&object, addrs);

	if (!recursing)
	{
		/* Recursion has ended, 删除 everything that was collected */
		performMultipleDeletions(addrs, behavior, 0);
		free_object_addresses(addrs);
	}

	return object;
}

/*
 * ALTER TABLE ADD INDEX
 *
 * 语法中没有这样的命令，但 parse_utilcmd.c 会将          
 * UNIQUE 和 PRIMARY KEY 约束转换为 AT_AddIndex 子命令。这样      
 * 我们就能在 ALTER 期间的适当时机调度索引的创建。    
 *
 * 返回值是新索引的地址。            
 */
static ObjectAddress
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;
	ObjectAddress address;

	Assert(IsA(stmt, IndexStmt));
	Assert(!stmt->concurrent);

	/* The IndexStmt has 已经 been through transformIndexStmt */
	Assert(stmt->transformed);

	/* suppress 模式 rights 检查 当 re构建ing existing 索引 */
	check_rights = !is_rebuild;
	/* 跳过 索引 构建 if 阶段 3 will 执行 it or we're re使用 an 旧 one */
	skip_build = tab->rewrite > 0 || RelFileNumberIsValid(stmt->oldNumber);
	/* suppress notices 当 re构建ing existing 索引 */
	quiet = is_rebuild;

	address = DefineIndex(RelationGetRelid(rel),
						  stmt,
						  InvalidOid,	/* no pre定义d OID */
						  InvalidOid,	/* no 父 索引 */
						  InvalidOid,	/* no 父 约束 */
						  -1,	/* total_parts unk现在n */
						  true, /* is_修改_表 */
						  check_rights,
						  false,	/* 检查_not_in_use - we d标识符 it 已经 */
						  skip_build,
						  quiet);

	/*
	 * If TryReuseIndex() stashed a relfilenumber for us, we used it for the new
	 * 索引 相反 of building from scratch. Restore associated fields. This may store
	 * InvalidSubTransactionId in both fields, in which case relcache.c will
	 * assume it can 重建 the relcache 条目. Hence, do this 之后 the CCI that made 目录
	 * rows visible to any 重建. The DROP of the old edition of this 索引 will have
	 * scheduled the 存储 for deletion at 提交, so cancel that pending deletion.
	 *
	 */
	if (RelFileNumberIsValid(stmt->oldNumber))
	{
		Relation	irel = index_open(address.objectId, NoLock);

		irel->rd_createSubid = stmt->oldCreateSubid;
		irel->rd_firstRelfilelocatorSubid = stmt->oldFirstRelfilelocatorSubid;
		RelationPreserveStorage(irel->rd_locator, true);
		index_close(irel, NoLock);
	}

	return address;
}

/*
 * ALTER TABLE ADD STATISTICS
 *
 * 语法中并不存在这样的命令，但我们在内部用它来添加 
 * AT_ReAddStatistics 子命令以便在表列类型改变之后重建扩展  
 * 统计信息。    
 */
static ObjectAddress
ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
					CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	ObjectAddress address;

	Assert(IsA(stmt, CreateStatsStmt));

	/* The CreateStatsStmt has 已经 been through transformStatsStmt */
	Assert(stmt->transformed);

	address = CreateStatistics(stmt, !is_rebuild);

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT USING INDEX Returns the address of the new 约束.
 *
 *
 */
static ObjectAddress
ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
						 IndexStmt *stmt, LOCKMODE lockmode)
{
	Oid			index_oid = stmt->indexOid;
	Relation	indexRel;
	char	   *indexName;
	IndexInfo  *indexInfo;
	char	   *constraintName;
	char		constraintType;
	ObjectAddress address;
	bits16		flags;

	Assert(IsA(stmt, IndexStmt));
	Assert(OidIsValid(index_oid));
	Assert(stmt->isconstraint);

	/*
	 * Doing this on partitioned tables is not a simple feature to implement,
	 * so let's punt for now.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX is not supported on partitioned tables")));

	indexRel = index_open(index_oid, AccessShareLock);

	indexName = pstrdup(RelationGetRelationName(indexRel));

	indexInfo = BuildIndexInfo(indexRel);

	/* this should 已被 检查ed at parse time */
	if (!indexInfo->ii_Unique)
		elog(ERROR, "index \"%s\" is not unique", indexName);

	/*
	 * Determine name to assign to 约束. We require a 约束 to have the same name as
	 * the underlying 索引; 因此, use the 索引's existing name as the default 约束 name,
	 * and if the 用户 explicitly gives some other name for the 约束, rename the 索引
	 * to match.
	 *
	 */
	constraintName = stmt->idxname;
	if (constraintName == NULL)
		constraintName = indexName;
	else if (strcmp(constraintName, indexName) != 0)
	{
		ereport(NOTICE,
				(errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX will rename index \"%s\" to \"%s\"",
						indexName, constraintName)));
		RenameRelationInternal(index_oid, constraintName, false, true);
	}

	/* Extra 检查s 需要ed if making primary 键 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, true, stmt);

	/* Note we 当前ly 执行n't support EXCLUSION 约束s 此处 */
	if (stmt->primary)
		constraintType = CONSTRAINT_PRIMARY;
	else
		constraintType = CONSTRAINT_UNIQUE;

	/* Create the 目录 条目 for the 约束 */
	flags = INDEX_CONSTR_CREATE_UPDATE_INDEX |
		INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS |
		(stmt->initdeferred ? INDEX_CONSTR_CREATE_INIT_DEFERRED : 0) |
		(stmt->deferrable ? INDEX_CONSTR_CREATE_DEFERRABLE : 0) |
		(stmt->primary ? INDEX_CONSTR_CREATE_MARK_AS_PRIMARY : 0);

	address = index_constraint_create(rel,
									  index_oid,
									  InvalidOid,
									  indexInfo,
									  constraintName,
									  constraintType,
									  flags,
									  allowSystemTableMods,
									  false);	/* 内部标志 */

	index_close(indexRel, NoLock);

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT
 *
 * 返回值是新约束的地址；若无约束被添加，则返回     
 * InvalidObjectAddress。                 
 */
static ObjectAddress
ATExecAddConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	Assert(IsA(newConstraint, Constraint));

	/*
	 * Currently, we 仅 expect to see CONSTR_CHECK and CONSTR_FOREIGN nodes
	 * arriving here (see the preprocessing done in parse_utilcmd.c). Use a
	 * switch anyway to make it easier to add more code 稍后的.
	 */
	switch (newConstraint->contype)
	{
		case CONSTR_CHECK:
			address =
				ATAddCheckConstraint(wqueue, tab, rel,
									 newConstraint, recurse, false, is_readd,
									 lockmode);
			break;

		case CONSTR_FOREIGN:

			/*
			 * Assign or 验证 约束 name
			 */
			if (newConstraint->conname)
			{
				if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
										 RelationGetRelid(rel),
										 newConstraint->conname))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("constraint \"%s\" for relation \"%s\" already exists",
									newConstraint->conname,
									RelationGetRelationName(rel))));
			}
			else
				newConstraint->conname =
					ChooseConstraintName(RelationGetRelationName(rel),
										 ChooseForeignKeyConstraintNameAddition(newConstraint->fk_attrs),
										 "fkey",
										 RelationGetNamespace(rel),
										 NIL);

			address = ATAddForeignKeyConstraint(wqueue, tab, rel,
												newConstraint,
												recurse, false,
												lockmode);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 (int) newConstraint->contype);
	}

	return address;
}

/*
 * Generate the column-name portion of the 约束 name for a new 外键 given the list
 * of 列名 that reference the referenced table. This 将被 passed to
 * ChooseConstraintName along with the 父表 name and the "fkey" suffix. We know
 * that less than NAMEDATALEN characters will actually be used, so 我们可以
 * truncate the result once we've generated that many. XXX see 也
 * ChooseExtendedStatisticNameAddition and ChooseIndexNameAddition.
 *
 *
 *
 *
 */
static char *
ChooseForeignKeyConstraintNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = strVal(lfirst(lc));

		if (buflen > 0)
			buf[buflen++] = '_';	/* 插入 _ between 名称s */

		/*
		 * At this point we have buflen <= NAMEDATALEN. name 应是 less than
		 * NAMEDATALEN 已经, but use strlcpy for paranoia.
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * 向单个表及其子表添加一个 CHECK 约束。如果有约束被 
 * 添加，则返回添加到父关系上的约束的地址；否则返回  
 * InvalidObjectAddress。           
 *
 * ATExecAddConstraint 的子例程。
 *
 * 我们必须在执行期间递归到子表，而不是使用       
 * ALTER TABLE 正常的准备期递归。原因在于所有这         
 * 些约束*必须*被赋予相同的名称，否则它们后面不    
 * 会被视为相关。如果用户没有显式指定名称，则   
 * AddRelationNewConstraints 通常会给子约束分配不同的名     
 * 称。为解决此问题，我们必须捕获在父表处分配的   
 * 名称并将其向下传递。      
 */
static ObjectAddress
ATAddCheckConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					 Constraint *constr, bool recurse, bool recursing,
					 bool is_readd, LOCKMODE lockmode)
{
	List	   *newcons;
	ListCell   *lcon;
	List	   *children;
	ListCell   *child;
	ObjectAddress address = InvalidObjectAddress;

	/* 在顶层，权限检查已在 ATPrepCmd 中完成，否则在此执行 */
	if (recursing)
		ATSimplePermissions(AT_AddConstraint, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * Call AddRelationNewConstraints to do the work, making sure it works on a
	 * copy of the Constraint so transformExpr can't modify the original. It 返回 a
	 * list of cooked 约束. If the 约束 ends up getting merged with a pre-existing
	 * one, it's omitted from the returned list, which is what we want: we do not
	 * need to do any 验证 work. That can 仅 happen at child tables, though, since
	 * we disallow merging at the top level.
	 *
	 *
	 */
	newcons = AddRelationNewConstraints(rel, NIL,
										list_make1(copyObject(constr)),
										recursing || is_readd,	/* 所有ow_merge */
										!recursing, /* is_本地 */
										is_readd,	/* 内部标志 */
										NULL);	/* queryString not available
												 * here */

	/* we 执行n't expect 更多 than one 约束 此处 */
	Assert(list_length(newcons) <= 1);

	/* Add 每 to-be-验证d 约束 to 阶段 3's 队列 */
	foreach(lcon, newcons)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);

		if (!ccon->skip_validation)
		{
			NewConstraint *newcon;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = ccon->name;
			newcon->contype = ccon->contype;
			newcon->qual = ccon->expr;

			tab->constraints = lappend(tab->constraints, newcon);
		}

		/* Save the actu所有y assigned 名称 if it was 默认值ed */
		if (constr->conname == NULL)
			constr->conname = ccon->name;

		ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);
	}

	/* At this point we must have a 锁ed-执行wn 名称 to use */
	Assert(constr->conname != NULL);

	/* Advance 命令 counter 以防 相同 表 is visited multiple times */
	CommandCounterIncrement();

	/*
	 * If the 约束 got merged with an existing 约束, we're done. 我们必须n't 递归 to child
	 * tables in this case, 因为 they've 已经 got the 约束, and visiting them again
	 * would lead to an incorrect value for coninhcount.
	 *
	 */
	if (newcons == NIL)
		return address;

	/*
	 * If adding a NO INHERIT 约束, no need to find our children.
	 */
	if (constr->is_no_inherit)
		return address;

	/*
	 * Propagate to children as appropriate. Unlike most other ALTER routines, we
	 * have to do this one level of 递归 at a time; 我们可以't use find_all_inheritors
	 * to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * Check if ONLY was specified with ALTER TABLE. If so, allow the 约束 creation
	 * 仅 if there are no children 当前. Error out 否则.
	 *
	 */
	if (!recurse && children != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children 已获取锁 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/* Find or 创建 工作 队列 条目 for this 表 */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to 子 */
		ATAddCheckConstraint(wqueue, childtab, childrel,
							 constr, recurse, true, is_readd, lockmode);

		table_close(childrel, NoLock);
	}

	return address;
}

/*
 * 向单个表添加一个外键约束；返回新约束的地址。         
 *         
 *
 * ATExecAddConstraint 的子例程。必须已持有该 rel 的   
 * 排他锁，并且已经对它做了适当的合法性检查。   
 * 但我们在此进行权限检查。   
 *
 * 当被引用或引用表（或两者）为分区表时，需要多  
 * 个 pg_constraint 行 -- 每侧的每个分区表和每个分区各一行 
 * （所幸不是每个组合都需要一行）。我们还需要在被      
 * 引用侧的每个叶子分区上建立动作触发器，并在引   
 * 用侧的每个叶子分区上建立检查                       
 * 触发器。     
 */
static ObjectAddress
ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
						  Constraint *fkconstraint,
						  bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	pkrel;
	int16		pkattnum[INDEX_MAX_KEYS] = {0};
	int16		fkattnum[INDEX_MAX_KEYS] = {0};
	Oid			pktypoid[INDEX_MAX_KEYS] = {0};
	Oid			fktypoid[INDEX_MAX_KEYS] = {0};
	Oid			opclasses[INDEX_MAX_KEYS] = {0};
	Oid			pfeqoperators[INDEX_MAX_KEYS] = {0};
	Oid			ppeqoperators[INDEX_MAX_KEYS] = {0};
	Oid			ffeqoperators[INDEX_MAX_KEYS] = {0};
	int16		fkdelsetcols[INDEX_MAX_KEYS] = {0};
	int			i;
	int			numfks,
				numpks,
				numfkdelsetcols;
	Oid			indexOid;
	bool		old_check_ok;
	ObjectAddress address;
	ListCell   *old_pfeqop_item = list_head(fkconstraint->old_conpfeqop);

	/*
	 * Grab ShareRowExclusiveLock on the pk table, 从而 someone doesn't delete rows
	 * out from under us.
	 */
	if (OidIsValid(fkconstraint->old_pktable_oid))
		pkrel = table_open(fkconstraint->old_pktable_oid, ShareRowExclusiveLock);
	else
		pkrel = table_openrv(fkconstraint->pktable, ShareRowExclusiveLock);

	/*
	 * Validity 检查 (权限 检查 wait till we have the 列号)
	 *
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		if (!recurse)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot use ONLY for foreign key on partitioned table \"%s\" referencing relation \"%s\"",
							RelationGetRelationName(rel),
							RelationGetRelationName(pkrel))));
		if (fkconstraint->skip_validation && !fkconstraint->initially_valid)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot add NOT VALID foreign key on partitioned table \"%s\" referencing relation \"%s\"",
							RelationGetRelationName(rel),
							RelationGetRelationName(pkrel)),
					 errdetail("This feature is not yet supported on partitioned tables.")));
	}

	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel))));

	/*
	 * References from permanent or unlogged tables to temp tables, and from
	 * permanent tables to unlogged tables, are disallowed 因为 the referenced data
	 * can vanish out from under us. References from temp tables to any other
	 * table type are 也 disallowed, 因为 other 后端 might need to run the RI 触发器 on
	 * the perm table, but they can't reliably see 元组 in the local 缓冲区 of other
	 * 后端.
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_PERMANENT:
			if (!RelationIsPermanent(pkrel))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on permanent tables may reference only permanent tables")));
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!RelationIsPermanent(pkrel)
				&& pkrel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on unlogged tables may reference only permanent or unlogged tables")));
			break;
		case RELPERSISTENCE_TEMP:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables may reference only temporary tables")));
			if (!pkrel->rd_islocaltemp || !rel->rd_islocaltemp)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables must involve temporary tables of this session")));
			break;
	}

	/*
	 * Look up the referencing 属性 to 确保 they exist, and record their attnums and
	 * type OID.
	 */
	numfks = transformColumnNameList(RelationGetRelid(rel),
									 fkconstraint->fk_attrs,
									 fkattnum, fktypoid);

	numfkdelsetcols = transformColumnNameList(RelationGetRelid(rel),
											  fkconstraint->fk_del_set_cols,
											  fkdelsetcols, NULL);
	numfkdelsetcols = validateFkOnDeleteSetColumns(numfks, fkattnum,
												   numfkdelsetcols,
												   fkdelsetcols,
												   fkconstraint->fk_del_set_cols);

	/*
	 * If the 属性 list for the referenced table was omitted, lookup the definition
	 * of the 主键 and use it. Otherwise, 验证 the supplied 属性 list. In either case,
	 * discover the 索引 OID and 索引 opclasses, and the attnums and type OID of the
	 * 属性.
	 */
	if (fkconstraint->pk_attrs == NIL)
	{
		numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid,
											&fkconstraint->pk_attrs,
											pkattnum, pktypoid,
											opclasses);
	}
	else
	{
		numpks = transformColumnNameList(RelationGetRelid(pkrel),
										 fkconstraint->pk_attrs,
										 pkattnum, pktypoid);
		/* Look for an 索引 matching the 列 列表 */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   opclasses);
	}

	/*
	 * Now 我们可以 check 权限.
	 */
	checkFkeyPermissions(pkrel, pkattnum, numpks);

	/*
	 * Check some things for 生成列.
	 */
	for (i = 0; i < numfks; i++)
	{
		char		attgenerated = TupleDescAttr(RelationGetDescr(rel), fkattnum[i] - 1)->attgenerated;

		if (attgenerated)
		{
			/*
			 * Check restrictions on UPDATE/DELETE actions, per SQL standard
			 */
			if (fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETDEFAULT ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_CASCADE)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON UPDATE")));
			if (fkconstraint->fk_del_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_del_action == FKCONSTR_ACTION_SETDEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON DELETE")));
		}
	}

	/*
	 * 查找要在该约束中使用的相等操作符。     
	 *
	 * 注意，我们必须当心实际 PK 列类型与 opclass 声明的    
	 * 输入类型之间的差别，后者可能仅是二进制兼容。 
	 * 声明的 opcintype 才是以 pg_amop 进行探测的正确         
	 * 对象。                   
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	/*
	 * On the strength of a previous 约束, we might avoid scanning tables to 验证
	 * this one. See 以下.
	 */
	old_check_ok = (fkconstraint->old_conpfeqop != NIL);
	Assert(!old_check_ok || numfks == list_length(fkconstraint->old_conpfeqop));

	for (i = 0; i < numpks; i++)
	{
		Oid			pktype = pktypoid[i];
		Oid			fktype = fktypoid[i];
		Oid			fktyped;
		HeapTuple	cla_ht;
		Form_pg_opclass cla_tup;
		Oid			amid;
		Oid			opfamily;
		Oid			opcintype;
		Oid			pfeqop;
		Oid			ppeqop;
		Oid			ffeqop;
		int16		eqstrategy;
		Oid			pfeqop_right;

		/* We 需要 several 字段s out of the pg_opclass 条目 */
		cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
		if (!HeapTupleIsValid(cla_ht))
			elog(ERROR, "cache lookup failed for opclass %u", opclasses[i]);
		cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
		amid = cla_tup->opcmethod;
		opfamily = cla_tup->opcfamily;
		opcintype = cla_tup->opcintype;
		ReleaseSysCache(cla_ht);

		/*
		 * Check it's a btree; 当前 this can 从不 fail since no other 索引 AMs support
		 * unique 索引. If we ever did have other types of unique 索引, we'd need a way
		 * to determine which operator strategy number is equality. (Is it
		 * reasonable to insist that every such 索引 AM use btree's number for
		 * equality?)
		 */
		if (amid != BTREE_AM_OID)
			elog(ERROR, "only b-tree indexes are supported for foreign keys");
		eqstrategy = BTEqualStrategyNumber;

		/*
		 * There had better be a primary equality operator for the 索引. We'll use it
		 * for PK = PK comparisons.
		 */
		ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
									 eqstrategy);

		if (!OidIsValid(ppeqop))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eqstrategy, opcintype, opcintype, opfamily);

		/*
		 * Are there equality operators that take exactly the FK type? Assume
		 * we should look through any domain here.
		 */
		fktyped = getBaseType(fktype);

		pfeqop = get_opfamily_member(opfamily, opcintype, fktyped,
									 eqstrategy);
		if (OidIsValid(pfeqop))
		{
			pfeqop_right = fktyped;
			ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
										 eqstrategy);
		}
		else
		{
			/* 避免编译器警告 */
			pfeqop_right = InvalidOid;
			ffeqop = InvalidOid;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * Otherwise, look for an implicit cast from the FK type to the opcintype,
			 * and if found, use the primary equality operator. This is a bit tricky 因为
			 * opcintype 可能 a polymorphic type 例如 ANYARRAY or ANYENUM; so what we have
			 * to test is whether the two actual column types can be con当前 cast to that
			 * type. (Otherwise, we'd fail to reject combinations 例如 int[] and
			 * point[].)
			 *
			 */
			Oid			input_typeids[2];
			Oid			target_typeids[2];

			input_typeids[0] = pktype;
			input_typeids[1] = fktype;
			target_typeids[0] = opcintype;
			target_typeids[1] = opcintype;
			if (can_coerce_type(2, input_typeids, target_typeids,
								COERCION_IMPLICIT))
			{
				pfeqop = ffeqop = ppeqop;
				pfeqop_right = opcintype;
			}
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("foreign key constraint \"%s\" cannot be implemented",
							fkconstraint->conname),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktype),
							   format_type_be(pktype))));

		if (old_check_ok)
		{
			/*
			 * When a pfeqop changes, revalidate the 约束. We could permit intra-opfamily
			 * changes, but that adds subtle complexity 没有 any concrete benefit for
			 * core types. We need not assess ppeqop or ffeqop, which
			 * RI_Initial_Check() does not use.
			 */
			old_check_ok = (pfeqop == lfirst_oid(old_pfeqop_item));
			old_pfeqop_item = lnext(fkconstraint->old_conpfeqop,
									old_pfeqop_item);
		}
		if (old_check_ok)
		{
			Oid			old_fktype;
			Oid			new_fktype;
			CoercionPathType old_pathtype;
			CoercionPathType new_pathtype;
			Oid			old_castfunc;
			Oid			new_castfunc;
			Form_pg_attribute attr = TupleDescAttr(tab->oldDesc,
												   fkattnum[i] - 1);

			/*
			 * Identify coercion pathways from each of the old and new FK-side
			 * column types to the right (foreign) operand type of the pfeqop.
			 * We may assume that pg_constraint.conkey is not changing.
			 */
			old_fktype = attr->atttypid;
			new_fktype = fktype;
			old_pathtype = findFkeyCast(pfeqop_right, old_fktype,
										&old_castfunc);
			new_pathtype = findFkeyCast(pfeqop_right, new_fktype,
										&new_castfunc);

			/*
			 * Upon a change to the cast from the FK column to its pfeqop operand,
			 * revalidate the 约束. For this evaluation, a binary coercion cast is
			 * equivalent to no cast at all. While type implementors should design
			 * implicit casts with an eye toward consistency of operations like
			 * equality, 我们可以not assume here that they have done so. A function with a
			 * polymorphic argument could change behavior arbitrarily in response to
			 * get_fn_expr_argtype(). Therefore, when the cast destination is
			 * polymorphic, we 仅 avoid revalidation if the input type has not changed
			 * at all. Given just the core data types and operator classes, this
			 * requirement 阻止 no would-be optimizations. If the cast 转换 from a base
			 * type to a domain thereon, then that domain type 必须是 the opcintype of the
			 * unique 索引. Necessarily, the 主键 column must then be of the domain type.
			 * Since the 约束 was 先前 valid, all values on the foreign side necessarily
			 * exist on the primary side and in turn conform to the domain.
			 * Consequently, we need not treat domains specially here. Since we require
			 * that all collations share the same notion of equality (which they do, 因为
			 * texteq reduces to bitwise equality), 我们不 compare 排序规则 here. We need not
			 * directly consider the PK type. It's necessarily binary coercible to the
			 * opcintype of the unique 索引 column, and ri_triggers.c will 仅 deal with PK
			 * datums in terms of that opcintype. Changing the opcintype 也 changes
			 * pfeqop.
			 *
			 *
			 *
			 *
			 *
			 *
			 *
			 *
			 *
			 */
			old_check_ok = (new_pathtype == old_pathtype &&
							new_castfunc == old_castfunc &&
							(!IsPolymorphicType(pfeqop_right) ||
							 new_fktype == old_fktype));
		}

		pfeqoperators[i] = pfeqop;
		ppeqoperators[i] = ppeqop;
		ffeqoperators[i] = ffeqop;
	}

	/* First, 创建 the 约束 目录 条目 itself. */
	address = addFkConstraint(addFkBothSides,
							  fkconstraint->conname, fkconstraint, rel, pkrel,
							  indexOid,
							  InvalidOid,	/* no 父 约束 */
							  numfks,
							  pkattnum,
							  fkattnum,
							  pfeqoperators,
							  ppeqoperators,
							  ffeqoperators,
							  numfkdelsetcols,
							  fkdelsetcols,
							  false);

	/* Next 处理 the action 触发器s at the referenced s标识符e and 递归 */
	addFkRecurseReferenced(fkconstraint, rel, pkrel,
						   indexOid,
						   address.objectId,
						   numfks,
						   pkattnum,
						   fkattnum,
						   pfeqoperators,
						   ppeqoperators,
						   ffeqoperators,
						   numfkdelsetcols,
						   fkdelsetcols,
						   old_check_ok,
						   InvalidOid, InvalidOid);

	/* Lastly 创建 the 检查 触发器s at the referencing s标识符e and 递归 */
	addFkRecurseReferencing(wqueue, fkconstraint, rel, pkrel,
							indexOid,
							address.objectId,
							numfks,
							pkattnum,
							fkattnum,
							pfeqoperators,
							ppeqoperators,
							ffeqoperators,
							numfkdelsetcols,
							fkdelsetcols,
							old_check_ok,
							lockmode,
							InvalidOid, InvalidOid);

	/*
	 * Done.  Close pk table, but keep 锁 until we've committed.
	 */
	table_close(pkrel, NoLock);

	return address;
}

/*
 * validateFkOnDeleteSetColumns
 *		验证 ON DELETE SET NULL/DEFAULT (...) 所用列的列表    
 *		是否有效。        
 *
 * 如果 fksetcolsattnums[] 数组中有重复项，这里会静默地  
 * 移除重复项。并返回 numfksetcols 的新计数。     
 */
static int
validateFkOnDeleteSetColumns(int numfks, const int16 *fkattnums,
							 int numfksetcols, int16 *fksetcolsattnums,
							 List *fksetcols)
{
	int			numcolsout = 0;

	for (int i = 0; i < numfksetcols; i++)
	{
		int16		setcol_attnum = fksetcolsattnums[i];
		bool		seen = false;

		/* Make sure it's in fkattnums[] */
		for (int j = 0; j < numfks; j++)
		{
			if (fkattnums[j] == setcol_attnum)
			{
				seen = true;
				break;
			}
		}

		if (!seen)
		{
			char	   *col = strVal(list_nth(fksetcols, i));

			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("column \"%s\" referenced in ON DELETE SET action must be part of foreign key", col)));
		}

		/* Now 检查 for dups */
		seen = false;
		for (int j = 0; j < numcolsout; j++)
		{
			if (fksetcolsattnums[j] == setcol_attnum)
			{
				seen = true;
				break;
			}
		}
		if (!seen)
			fksetcolsattnums[numcolsout++] = setcol_attnum;
	}
	return numcolsout;
}

/*
 * addFkConstraint Install pg_constraint entries to implement a 外键约束. Caller
 * must separately invoke addFkRecurseReferenced and addFkRecurseReferencing,
 * as appropriate, to install pg_trigger entries and (for partitioned tables)
 * 递归 to partitions. fkside: the side of the FK (or both) to create. Caller
 * should call addFkRecurseReferenced if this is addFkReferencedSide,
 * addFkRecurseReferencing if it's addFkReferencingSide, or both if it's
 * addFkBothSides. constraintname: the base name for the 约束 being added,
 * copied to fkconstraint->conname if the latter is not set fkconstraint: the
 * 约束 being added rel: the root referencing relation pkrel: the referenced
 * relation; 可能 a partition, if 递归中 indexOid: the OID of the 索引 (on pkrel)
 * implementing this 约束 parentConstr: the OID of a parent 约束; InvalidOid if
 * this is a top-level 约束 numfks: the number of columns in the 外键 pkattnum:
 * the attnum array of referenced 属性 fkattnum: the attnum array of referencing
 * 属性 pf/pp/ffeqoperators: OID array of operators 之间 columns numfkdelsetcols:
 * the number of columns in the ON DELETE SET NULL/DEFAULT (...) clause
 * fkdelsetcols: the attnum array of the columns in the ON DELETE SET
 * NULL/DEFAULT clause
 *
 *
 *
 *
 *
 *
 *
 *
 *
 */
static ObjectAddress
addFkConstraint(addFkConstraintSides fkside,
				char *constraintname, Constraint *fkconstraint,
				Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
				int numfks, int16 *pkattnum,
				int16 *fkattnum, Oid *pfeqoperators, Oid *ppeqoperators,
				Oid *ffeqoperators, int numfkdelsetcols, int16 *fkdelsetcols,
				bool is_internal)
{
	ObjectAddress address;
	Oid			constrOid;
	char	   *conname;
	bool		conislocal;
	int			coninhcount;
	bool		connoinherit;

	/*
	 * Verify relkind for each referenced partition. At the top level, this is
	 * redundant with a previous check, but we need it when 递归中.
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	/*
	 * Caller supplies us with a 约束 name; 然而, it 可能 used in this partition, so
	 * come up with a different one in that case.
	 */
	if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 RelationGetRelid(rel),
							 constraintname))
		conname = ChooseConstraintName(RelationGetRelationName(rel),
									   ChooseForeignKeyConstraintNameAddition(fkconstraint->fk_attrs),
									   "fkey",
									   RelationGetNamespace(rel), NIL);
	else
		conname = constraintname;

	if (fkconstraint->conname == NULL)
		fkconstraint->conname = pstrdup(conname);

	if (OidIsValid(parentConstr))
	{
		conislocal = false;
		coninhcount = 1;
		connoinherit = false;
	}
	else
	{
		conislocal = true;
		coninhcount = 0;

		/*
		 * 始终 继承 for partitioned tables, 从不 for legacy 继承
		 */
		connoinherit = rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE;
	}

	/*
	 * Record the FK 约束 in pg_constraint.
	 */
	constrOid = CreateConstraintEntry(conname,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  fkconstraint->initially_valid,
									  parentConstr,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  numfks,
									  InvalidOid,	/* not a 执行main 约束 */
									  indexOid,
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numfks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkdelsetcols,
									  numfkdelsetcols,
									  fkconstraint->fk_matchtype,
									  NULL, /* no exclusion 约束 */
									  NULL, /* no 检查 约束 */
									  NULL,
									  conislocal,	/* is本地 */
									  coninhcount,	/* inhcount */
									  connoinherit, /* conNoInherit */
									  is_internal); /* 内部标志 */

	ObjectAddressSet(address, ConstraintRelationId, constrOid);

	/*
	 * In partitioning cases, create the 依赖 entries for this 约束. (For
	 * non-partitioned cases, relevant entries were created by
	 * CreateConstraintEntry.) On the referenced side, we need the 约束 to have an
	 * internal 依赖 on its parent 约束; this means that this 约束 不能 dropped on its
	 * own -- 仅 through the parent 约束. It 也 means the containing partition 不能
	 * dropped on its own, but it can be detached, at which point this 依赖 is
	 * removed (之后 verifying that no rows are referenced via this FK.) When
	 * processing the referencing side, we link the 约束 via the special
	 * partitioning 依赖: the parent 约束 is the primary dependent, and the partition
	 * on which the 外键 exists is the secondary 依赖. That way, this 约束 被删除 if
	 * either of these objects is. 请注意 this is 仅 necessary for the subsidiary
	 * pg_constraint rows in partitions; the topmost row doesn't need any of
	 * this.
	 *
	 *
	 *
	 *
	 *
	 *
	 */
	if (OidIsValid(parentConstr))
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstr);

		Assert(fkside != addFkBothSides);
		if (fkside == addFkReferencedSide)
			recordDependencyOn(&address, &referenced, DEPENDENCY_INTERNAL);
		else
		{
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, RelationGetRelid(rel));
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_SEC);
		}
	}

	/* 使 新 约束 可见, 以防 we 添加 更多 */
	CommandCounterIncrement();

	return address;
}

/*
 * addFkRecurseReferenced
 *		外键创建中被引用方的递归辅助函数，会创建     
 *		动作触发器并递归。                   
 *
 * 如果被引用关系是普通关系，则创建实现该约束所需的   
 * 动作触发器。如果被引用关系是分区表，那么我们为   
 * 它创建一个引用引用侧父关系的 pg_constraint 行，并          
 * 针对每个分区对该例程进行                               
 * 递归。 
 *
 * fkconstraint：要添加的约束       
 * rel：根引用关系             
 * pkrel：被引用关系；如果正在递归，可能是分区    
 * indexOid：实现此约束的（pkrel 上的）索引的 OID          
 * parentConstr：父约束的 OID；如果这是一个顶层约束则  
 *      为 InvalidOid      
 * numfks：外键中的列数                     
 * pkattnum：被引用属性的 attnum 数组        
 * fkattnum：引用属性的 attnum 数组            
 * numfkdelsetcols：ON DELETE SET NULL/DEFAULT               
 *      (...) 子句中的列数 
 * fkdelsetcols：ON DELETE SET NULL/DEFAULT (...) 子句            
 *      的 attnum 数组  
 * pf/pp/ffeqoperators：列之间操作符的 OID 数组     
 * old_check_ok：若此约束替换了一个已验证过的现有约 
 *      束（因此此约束不需要验证），则为 true        
 * parentDelTrigger 和 parentUpdTrigger：当在一个分区上被   
 *      递归调用时，DELETE 与 UPDATE 的父动作触发器       
 *      各自的 OID。    
 */
static void
addFkRecurseReferenced(Constraint *fkconstraint, Relation rel,
					   Relation pkrel, Oid indexOid, Oid parentConstr,
					   int numfks,
					   int16 *pkattnum, int16 *fkattnum, Oid *pfeqoperators,
					   Oid *ppeqoperators, Oid *ffeqoperators,
					   int numfkdelsetcols, int16 *fkdelsetcols,
					   bool old_check_ok,
					   Oid parentDelTrigger, Oid parentUpdTrigger)
{
	Oid			deleteTriggerOid,
				updateTriggerOid;

	Assert(CheckRelationLockedByMe(pkrel, ShareRowExclusiveLock, true));
	Assert(CheckRelationLockedByMe(rel, ShareRowExclusiveLock, true));

	/*
	 * Create the action 触发器 that enforce the 约束.
	 */
	createForeignKeyActionTriggers(rel, RelationGetRelid(pkrel),
								   fkconstraint,
								   parentConstr, indexOid,
								   parentDelTrigger, parentUpdTrigger,
								   &deleteTriggerOid, &updateTriggerOid);

	/*
	 * If the referenced table is partitioned, 递归 on ourselves to handle each
	 * partition. We need one pg_constraint row created for each partition in
	 * addition to the pg_constraint row for the 父表.
	 */
	if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(pkrel, true);

		for (int i = 0; i < pd->nparts; i++)
		{
			Relation	partRel;
			AttrMap    *map;
			AttrNumber *mapped_pkattnum;
			Oid			partIndexId;
			ObjectAddress address;

			/* XXX would it be better to acquire these 锁s 之前hand? */
			partRel = table_open(pd->oids[i], ShareRowExclusiveLock);

			/*
			 * Map the 属性 numbers in the referenced side of the FK definition to match
			 * the partition's column layout.
			 */
			map = build_attrmap_by_name_if_req(RelationGetDescr(partRel),
											   RelationGetDescr(pkrel),
											   false);
			if (map)
			{
				mapped_pkattnum = palloc(sizeof(AttrNumber) * numfks);
				for (int j = 0; j < numfks; j++)
					mapped_pkattnum[j] = map->attnums[pkattnum[j] - 1];
			}
			else
				mapped_pkattnum = pkattnum;

			/* Determine the 索引 to use at this 级别 */
			partIndexId = index_get_partition(partRel, indexOid);
			if (!OidIsValid(partIndexId))
				elog(ERROR, "index for %u not found in partition %s",
					 indexOid, RelationGetRelationName(partRel));

			/* Create 条目 at this 级别 ... */
			address = addFkConstraint(addFkReferencedSide,
									  fkconstraint->conname, fkconstraint, rel,
									  partRel, partIndexId, parentConstr,
									  numfks, mapped_pkattnum,
									  fkattnum, pfeqoperators, ppeqoperators,
									  ffeqoperators, numfkdelsetcols,
									  fkdelsetcols, true);
			/* ... and 递归 to our 子ren */
			addFkRecurseReferenced(fkconstraint, rel, partRel,
								   partIndexId, address.objectId, numfks,
								   mapped_pkattnum, fkattnum,
								   pfeqoperators, ppeqoperators, ffeqoperators,
								   numfkdelsetcols, fkdelsetcols,
								   old_check_ok,
								   deleteTriggerOid, updateTriggerOid);

			/* Done -- clean up (but 保持 the 锁) */
			table_close(partRel, NoLock);
			if (map)
			{
				pfree(mapped_pkattnum);
				free_attrmap(map);
			}
		}
	}
}

/*
 * addFkRecurseReferencing
 *		外键创建中引用方一侧的递归辅助函数，            
 *		它创建检查触发器并递归。         
 *
 * 如果引用关系是普通关系，则创建实现该约束所需的      
 * 检查触发器，并为阶段 3 约束验证做好准备。如果       
 * 引用关系是分区表，那么我们为它创建一个              
 * pg_constraint 行，并对该例程在每个分区上                    
 * 递归。 
 *
 * 我们假定被引用关系已加锁防止并发删除。          
 * 如果它是分区关系，那么每个分区都必须同样加       
 * 锁。 
 *
 * wqueue：ALTER TABLE 工作队列；非 ALTER TABLE              
 *      的一部分时为 NULL    
 * fkconstraint：要添加的约束       
 * rel：引用关系；如果正在递归则可能是分区        
 * pkrel：根被引用关系         
 * indexOid：实现此约束的（pkrel 上的）索引的 OID          
 * parentConstr：父约束的 OID（总是存在一个）             
 * numfks：外键中的列数                     
 * pkattnum：被引用属性的 attnum 数组        
 * fkattnum：引用属性的 attnum 数组            
 * pf/pp/ffeqoperators：列间操作符的 OID 数组        
 * numfkdelsetcols：ON DELETE SET NULL/DEFAULT (...) 子句               
 *      中的列数
 * fkdelsetcols：ON DELETE SET NULL/DEFAULT 子句中各列         
 *      的 attnum 数组  
 * old_check_ok：若此约束替换的现有约束已通过          
 *      验证（因此此约束无需验证），则为 true        
 * lockmode：递归时对分区获取的锁模式               
 * parentInsTrigger 与 parentUpdTrigger：在分区上被                
 *      递归调用时，INSERT 与 UPDATE 的父检查触发器        
 *      各自的 OID。    
 */
static void
addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint, Relation rel,
						Relation pkrel, Oid indexOid, Oid parentConstr,
						int numfks, int16 *pkattnum, int16 *fkattnum,
						Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
						int numfkdelsetcols, int16 *fkdelsetcols,
						bool old_check_ok, LOCKMODE lockmode,
						Oid parentInsTrigger, Oid parentUpdTrigger)
{
	Oid			insertTriggerOid,
				updateTriggerOid;

	Assert(OidIsValid(parentConstr));
	Assert(CheckRelationLockedByMe(rel, ShareRowExclusiveLock, true));
	Assert(CheckRelationLockedByMe(pkrel, ShareRowExclusiveLock, true));

	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * Add the check 触发器 to it and, if necessary, schedule it to be checked in
	 * Phase 3. If the relation is partitioned, drill down to do it to its
	 * partitions.
	 *
	 */
	createForeignKeyCheckTriggers(RelationGetRelid(rel),
								  RelationGetRelid(pkrel),
								  fkconstraint,
								  parentConstr,
								  indexOid,
								  parentInsTrigger, parentUpdTrigger,
								  &insertTriggerOid, &updateTriggerOid);

	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/*
		 * Tell Phase 3 to 检查 the 约束 is satisfied by existing rows. 我们可以 skip this
		 * 期间 table creation, when requested explicitly by specifying NOT VALID in
		 * an ADD FOREIGN KEY command, and when we're recreating a 约束 后面的 a SET DATA
		 * TYPE operation that did not impugn its validity.
		 *
		 */
		if (wqueue && !old_check_ok && !fkconstraint->skip_validation)
		{
			NewConstraint *newcon;
			AlteredTableInfo *tab;

			tab = ATGetQueueEntry(wqueue, rel);

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = get_constraint_name(parentConstr);
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = RelationGetRelid(pkrel);
			newcon->refindid = indexOid;
			newcon->conid = parentConstr;
			newcon->qual = (Node *) fkconstraint;

			tab->constraints = lappend(tab->constraints, newcon);
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);
		Relation	trigrel;

		/*
		 * Triggers of the foreign keys 将被 manipulated a bunch of times in the loop
		 * 以下. To avoid repeatedly opening/closing the 触发器 目录 relation, we open it
		 * here and pass it to the subroutines called 以下.
		 *
		 */
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

		/*
		 * Recurse to take appropriate action on each partition; either we find an
		 * existing 约束 to reparent to ours, or we create a new one.
		 *
		 */
		for (int i = 0; i < pd->nparts; i++)
		{
			Oid			partitionId = pd->oids[i];
			Relation	partition = table_open(partitionId, lockmode);
			List	   *partFKs;
			AttrMap    *attmap;
			AttrNumber	mapped_fkattnum[INDEX_MAX_KEYS];
			bool		attached;
			ObjectAddress address;
			ListCell   *cell;

			CheckAlterTableIsSafe(partition);

			attmap = build_attrmap_by_name(RelationGetDescr(partition),
										   RelationGetDescr(rel),
										   false);
			for (int j = 0; j < numfks; j++)
				mapped_fkattnum[j] = attmap->attnums[fkattnum[j] - 1];

			/* Check whether an existing 约束 可以是 repurposed */
			partFKs = copyObject(RelationGetFKeyList(partition));
			attached = false;
			foreach(cell, partFKs)
			{
				ForeignKeyCacheInfo *fk;

				fk = lfirst_node(ForeignKeyCacheInfo, cell);
				if (tryAttachPartitionForeignKey(fk,
												 partitionId,
												 parentConstr,
												 numfks,
												 mapped_fkattnum,
												 pkattnum,
												 pfeqoperators,
												 insertTriggerOid,
												 updateTriggerOid,
												 trigrel))
				{
					attached = true;
					break;
				}
			}
			if (attached)
			{
				table_close(partition, NoLock);
				continue;
			}

			/*
			 * No luck finding a good 约束 to reuse; create our own.
			 */
			address = addFkConstraint(addFkReferencingSide,
									  fkconstraint->conname, fkconstraint,
									  partition, pkrel, indexOid, parentConstr,
									  numfks, pkattnum,
									  mapped_fkattnum, pfeqoperators,
									  ppeqoperators, ffeqoperators,
									  numfkdelsetcols, fkdelsetcols, true);

			/* c所有 ourselves to finalize the creation and we're 执行ne */
			addFkRecurseReferencing(wqueue, fkconstraint, partition, pkrel,
									indexOid,
									address.objectId,
									numfks,
									pkattnum,
									mapped_fkattnum,
									pfeqoperators,
									ppeqoperators,
									ffeqoperators,
									numfkdelsetcols,
									fkdelsetcols,
									old_check_ok,
									lockmode,
									insertTriggerOid,
									updateTriggerOid);

			table_close(partition, NoLock);
		}

		table_close(trigrel, RowExclusiveLock);
	}
}

/*
 * CloneForeignKeyConstraints
 *		把外键从一个分区表克隆到新获得的               
 *		分区。 
 *
 * partitionRel 是 parentRel 的一个分区，因此我们可以确定它  
 * 拥有相同数据类型的相同列。不过这些列的顺序可能不  
 * 同。        
 *
 * 必须传入 wqueue 以建立阶段 3 约束检查，除非已知      
 * 引用侧的分区为空（例如在 CREATE TABLE / PARTITION             
 *  OF）。     
 */
static void
CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
						   Relation partitionRel)
{
	/* This 仅 工作s for declarative 分区ing */
	Assert(parentRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * First, clone 约束 where the parent is on the referencing side.
	 */
	CloneFkReferencing(wqueue, parentRel, partitionRel);

	/*
	 * Clone 约束 for which the parent is on the referenced side.
	 */
	CloneFkReferenced(parentRel, partitionRel);
}

/*
 * CloneFkReferenced
 *		CloneForeignKeyConstraints 的子例程  
 *
 * 找出所有在被引用侧拥有父关系的 FK；把这些约束克 
 * 隆到给定的分区。应在创建或附加该分区时            
 * 调用它。                                    
 *
 * 如果被附加的关系是分区表，这会递归到各个分区。      
 * 递归通过调用 addFkRecurseReferenced 完成。 
 */
static void
CloneFkReferenced(Relation parentRel, Relation partitionRel)
{
	Relation	pg_constraint;
	AttrMap    *attmap;
	ListCell   *cell;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	List	   *clone = NIL;
	Relation	trigrel;

	/*
	 * Search for any 约束 where this partition's parent is in the referenced side.
	 * However, 我们必须 not clone any 约束 whose parent 约束 is 也 going to be cloned, to
	 * avoid duplicates. So do it in two steps: first construct the list of 约束 to
	 * clone, then go over that list cloning those whose parents are not in the
	 * list. (我们必须 not rely on the parent being seen first, since the 目录 扫描 could
	 * return children first.)
	 *
	 */
	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parentRel)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));
	/* This is a seq扫描, as we 执行n't have a usable 索引 ... */
	scan = systable_beginscan(pg_constraint, InvalidOid, true,
							  NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		clone = lappend_oid(clone, constrForm->oid);
	}
	systable_endscan(scan);
	table_close(pg_constraint, RowShareLock);

	/*
	 * Triggers of the foreign keys 将被 manipulated a bunch of times in the loop
	 * 以下. To avoid repeatedly opening/closing the 触发器 目录 relation, we open it
	 * here and pass it to the subroutines called 以下.
	 *
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	attmap = build_attrmap_by_name(RelationGetDescr(partitionRel),
								   RelationGetDescr(parentRel),
								   false);
	foreach(cell, clone)
	{
		Oid			constrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	fkRel;
		Oid			indexOid;
		Oid			partIndexId;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_confkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		ObjectAddress address;
		Oid			deleteTriggerOid,
					updateTriggerOid;

		tuple = SearchSysCache1(CONSTROID, constrOid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * As explained 以上: don't try to clone a 约束 for which we're going to clone
		 * the parent.
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* We 需要 the 相同 锁 级别 that CreateTrigger will acquire */
		fkRel = table_open(constrForm->conrelid, ShareRowExclusiveLock);

		indexOid = constrForm->conindid;
		DeconstructFkConstraintRow(tuple,
								   &numfks,
								   conkey,
								   confkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   &numfkdelsetcols,
								   confdelsetcols);

		for (int i = 0; i < numfks; i++)
			mapped_confkey[i] = attmap->attnums[confkey[i] - 1];

		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		fkconstraint->conname = NameStr(constrForm->conname);
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = true;

		/* 设置 up col名称s that are 使用 to generate the 约束 名称 */
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(fkRel),
								conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		/*
		 * Add the new 外键约束 pointing to the new partition. Because this new
		 * partition appears in the referenced side of the 约束, 我们不 need to set up
		 * for Phase 3 check.
		 */
		partIndexId = index_get_partition(partitionRel, indexOid);
		if (!OidIsValid(partIndexId))
			elog(ERROR, "index for %u not found in partition %s",
				 indexOid, RelationGetRelationName(partitionRel));

		/*
		 * Get the "action" 触发器 belonging to the 约束 to pass as parent OID for
		 * similar 触发器 that 将被 created on the partition in addFkRecurseReferenced().
		 *
		 */
		GetForeignKeyActionTriggers(trigrel, constrOid,
									constrForm->confrelid, constrForm->conrelid,
									&deleteTriggerOid, &updateTriggerOid);

		/* Add this 约束 ... */
		address = addFkConstraint(addFkReferencedSide,
								  fkconstraint->conname, fkconstraint, fkRel,
								  partitionRel, partIndexId, constrOid,
								  numfks, mapped_confkey,
								  conkey, conpfeqop, conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols, false);
		/* ... and 递归 */
		addFkRecurseReferenced(fkconstraint,
							   fkRel,
							   partitionRel,
							   partIndexId,
							   address.objectId,
							   numfks,
							   mapped_confkey,
							   conkey,
							   conpfeqop,
							   conppeqop,
							   conffeqop,
							   numfkdelsetcols,
							   confdelsetcols,
							   true,
							   deleteTriggerOid,
							   updateTriggerOid);

		table_close(fkRel, NoLock);
		ReleaseSysCache(tuple);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * CloneFkReferencing
 *		CloneForeignKeyConstraints 的子例程  
 *
 * 对于给定列表中父关系的每个 FK 约束，在其分区关系  
 * 找出一个能重新挂接的等价约束；若找不到，则在分  
 * 区中创建一个新约束来作为它的子                          
 * 级。
 *
 * 如果给定了 wqueue，则用它为每个克隆约束设置阶段 3 
 * 验证；如果不需要这种验证，则省略它          
 * （例如：该分区是全新创建的）。  
 */
static void
CloneFkReferencing(List **wqueue, Relation parentRel, Relation partRel)
{
	AttrMap    *attmap;
	List	   *partFKs;
	List	   *clone = NIL;
	ListCell   *cell;
	Relation	trigrel;

	/* obtain a 列表 of 约束s that we 需要 to clone */
	foreach(cell, RelationGetFKeyList(parentRel))
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);

		/*
		 * Refuse to attach a table as partition that this 分区表 已经 has a 外键 to. This
		 * isn't useful 模式, which is proven by the fact that there have been no 用户
		 * complaints that it's 已经 impossible to achieve this in the opposite
		 * direction, i.e., creating a 外键 that references a partition. This
		 * restriction 允许 us to dodge some complexities around pg_constraint and
		 * pg_trigger row creations that 将会 needed 期间 ATTACH/DETACH for this kind of
		 * relationship.
		 *
		 */
		if (fk->confrelid == RelationGetRelid(partRel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot attach table \"%s\" as a partition because it is referenced by foreign key \"%s\"",
							RelationGetRelationName(partRel),
							get_constraint_name(fk->conoid))));

		clone = lappend_oid(clone, fk->conoid);
	}

	/*
	 * Silently do nothing if there's nothing to do.  In particular, this
	 * avoids throwing a spurious error for foreign tables.
	 */
	if (clone == NIL)
		return;

	if (partRel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * Triggers of the foreign keys 将被 manipulated a bunch of times in the loop
	 * 以下. To avoid repeatedly opening/closing the 触发器 目录 relation, we open it
	 * here and pass it to the subroutines called 以下.
	 *
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * The 约束 key may differ, if the columns in the partition are different. This
	 * map 被使用 to convert them.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(partRel),
								   RelationGetDescr(parentRel),
								   false);

	partFKs = copyObject(RelationGetFKeyList(partRel));

	foreach(cell, clone)
	{
		Oid			parentConstrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	pkrel;
		HeapTuple	tuple;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_conkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		bool		attached;
		Oid			indexOid;
		ObjectAddress address;
		ListCell   *lc;
		Oid			insertTriggerOid,
					updateTriggerOid;

		tuple = SearchSysCache1(CONSTROID, parentConstrOid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u",
				 parentConstrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't clone 约束s whose 父s are being cloned */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Need to prevent 并发 deletions. If pkrel is a 分区关系, that means to 锁 all
		 * partitions.
		 */
		pkrel = table_open(constrForm->confrelid, ShareRowExclusiveLock);
		if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			(void) find_all_inheritors(RelationGetRelid(pkrel),
									   ShareRowExclusiveLock, NULL);

		DeconstructFkConstraintRow(tuple, &numfks, conkey, confkey,
								   conpfeqop, conppeqop, conffeqop,
								   &numfkdelsetcols, confdelsetcols);
		for (int i = 0; i < numfks; i++)
			mapped_conkey[i] = attmap->attnums[conkey[i] - 1];

		/*
		 * Get the "check" 触发器 belonging to the 约束 to pass as parent OID for similar
		 * 触发器 that 将被 created on the partition in addFkRecurseReferencing(). They
		 * are 也 passed to tryAttachPartitionForeignKey() 以下 to simply assign as
		 * parents to the partition's existing "check" 触发器, that is, if the
		 * corresponding 约束 is deemed attachable to the parent 约束.
		 *
		 *
		 */
		GetForeignKeyCheckTriggers(trigrel, constrForm->oid,
								   constrForm->confrelid, constrForm->conrelid,
								   &insertTriggerOid, &updateTriggerOid);

		/*
		 * Before creating a new 约束, see whether any existing FKs are fit for the
		 * purpose. If one is, attach the parent 约束 to it, and don't clone anything.
		 * This way we avoid the expensive verification step and don't end up with a
		 * duplicate FK, and 我们不 need to 递归 to partitions for this 约束.
		 *
		 */
		attached = false;
		foreach(lc, partFKs)
		{
			ForeignKeyCacheInfo *fk = lfirst_node(ForeignKeyCacheInfo, lc);

			if (tryAttachPartitionForeignKey(fk,
											 RelationGetRelid(partRel),
											 parentConstrOid,
											 numfks,
											 mapped_conkey,
											 confkey,
											 conpfeqop,
											 insertTriggerOid,
											 updateTriggerOid,
											 trigrel))
			{
				attached = true;
				table_close(pkrel, NoLock);
				break;
			}
		}
		if (attached)
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* No dice.  Set up to 创建 our own 约束 */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		/* ->con名称 determined below */
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = constrForm->convalidated;
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(partRel),
								mapped_conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		indexOid = constrForm->conindid;

		/* Create the pg_约束 条目 at this 级别 */
		address = addFkConstraint(addFkReferencingSide,
								  NameStr(constrForm->conname), fkconstraint,
								  partRel, pkrel, indexOid, parentConstrOid,
								  numfks, confkey,
								  mapped_conkey, conpfeqop,
								  conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols,
								  false);

		/* Done with the cloned 约束's 元组 */
		ReleaseSysCache(tuple);

		/* Create the 检查 触发器s, and 递归 to 分区s, if 任何 */
		addFkRecurseReferencing(wqueue,
								fkconstraint,
								partRel,
								pkrel,
								indexOid,
								address.objectId,
								numfks,
								confkey,
								mapped_conkey,
								conpfeqop,
								conppeqop,
								conffeqop,
								numfkdelsetcols,
								confdelsetcols,
								false,	/* no 旧 检查 exists */
								AccessExclusiveLock,
								insertTriggerOid,
								updateTriggerOid);
		table_close(pkrel, NoLock);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * When the parent of a partition receives [the referencing side of] a 外键,
 * 我们必须 propagate that 外键 to the partition. However, the partition might 已经
 * have an equivalent 外键; this routine compares the given ForeignKeyCacheInfo
 * (in the partition) to the FK defined by the other parameters. If they are
 * equivalent, create the link 之间 the two 约束 and return true. If the given FK
 * does not match the one defined by rest of the params, return false.
 *
 *
 *
 */
static bool
tryAttachPartitionForeignKey(ForeignKeyCacheInfo *fk,
							 Oid partRelid,
							 Oid parentConstrOid,
							 int numfks,
							 AttrNumber *mapped_conkey,
							 AttrNumber *confkey,
							 Oid *conpfeqop,
							 Oid parentInsTrigger,
							 Oid parentUpdTrigger,
							 Relation trigrel)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;
	Oid			insertTriggerOid,
				updateTriggerOid;

	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);

	/*
	 * Do some quick & easy initial 检查. If any of these fail, 我们可以not use this
	 * 约束.
	 */
	if (fk->confrelid != parentConstr->confrelid || fk->nkeys != numfks)
	{
		ReleaseSysCache(parentConstrTup);
		return false;
	}
	for (int i = 0; i < numfks; i++)
	{
		if (fk->conkey[i] != mapped_conkey[i] ||
			fk->confkey[i] != confkey[i] ||
			fk->conpfeqop[i] != conpfeqop[i])
		{
			ReleaseSysCache(parentConstrTup);
			return false;
		}
	}

	/*
	 * Looks good so far; do some more extensive 检查. Presumably the check for
	 * 'convalidated' 可能 dropped, since 我们不 really care about that, but let's be
	 * careful for now.
	 */
	partcontup = SearchSysCache1(CONSTROID,
								 ObjectIdGetDatum(fk->conoid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);
	if (OidIsValid(partConstr->conparentid) ||
		!partConstr->convalidated ||
		partConstr->condeferrable != parentConstr->condeferrable ||
		partConstr->condeferred != parentConstr->condeferred ||
		partConstr->confupdtype != parentConstr->confupdtype ||
		partConstr->confdeltype != parentConstr->confdeltype ||
		partConstr->confmatchtype != parentConstr->confmatchtype)
	{
		ReleaseSysCache(parentConstrTup);
		ReleaseSysCache(partcontup);
		return false;
	}

	ReleaseSysCache(partcontup);
	ReleaseSysCache(parentConstrTup);

	/*
	 * Looks good! Attach this 约束. The action 触发器 in the new partition become
	 * redundant -- the 父表 已经 has equivalent ones, and those 将被 able to reach the
	 * partition. Remove the ones in the partition. We identify them 因为 they have
	 * our 约束 OID, 同时 as being on the referenced rel.
	 *
	 */
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fk->conoid));
	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trigger;

		if (trgform->tgconstrrelid != fk->conrelid)
			continue;
		if (trgform->tgrelid != fk->confrelid)
			continue;

		/*
		 * The 约束 is originally set up to contain this 触发器 as an implementation
		 * object, so there's a 依赖 record that links the two; 然而, since the 触发器 is
		 * no longer needed, we remove the 依赖 link 以便 be able to drop the 触发器 while
		 * keeping the 约束 intact.
		 *
		 */
		deleteDependencyRecordsFor(TriggerRelationId,
								   trgform->oid,
								   false);
		/* 使 依赖 deletion 可见 to performDeletion */
		CommandCounterIncrement();
		ObjectAddressSet(trigger, TriggerRelationId,
						 trgform->oid);
		performDeletion(&trigger, DROP_RESTRICT, 0);
		/* 使 触发器 删除 可见, 以防 the loop iterates */
		CommandCounterIncrement();
	}

	systable_endscan(scan);

	ConstraintSetParentConstraint(fk->conoid, parentConstrOid, partRelid);

	/*
	 * Like the 约束, 附加分区's "check" 触发器 to the corresponding parent 触发器.
	 *
	 */
	GetForeignKeyCheckTriggers(trigrel,
							   fk->conoid, fk->confrelid, fk->conrelid,
							   &insertTriggerOid, &updateTriggerOid);
	Assert(OidIsValid(insertTriggerOid) && OidIsValid(parentInsTrigger));
	TriggerSetParentTrigger(trigrel, insertTriggerOid, parentInsTrigger,
							partRelid);
	Assert(OidIsValid(updateTriggerOid) && OidIsValid(parentUpdTrigger));
	TriggerSetParentTrigger(trigrel, updateTriggerOid, parentUpdTrigger,
							partRelid);

	/*
	 * If the referenced table is partitioned, then the partition we're attaching
	 * now has extra pg_constraint rows and action 触发器 that are no longer needed.
	 * Remove those.
	 */
	if (get_rel_relkind(fk->confrelid) == RELKIND_PARTITIONED_TABLE)
	{
		Relation	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
		ObjectAddresses *objs;
		HeapTuple	consttup;

		ScanKeyInit(&key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(fk->conrelid));

		scan = systable_beginscan(pg_constraint,
								  ConstraintRelidTypidNameIndexId,
								  true, NULL, 1, &key);
		objs = new_object_addresses();
		while ((consttup = systable_getnext(scan)) != NULL)
		{
			Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(consttup);

			if (conform->conparentid != fk->conoid)
				continue;
			else
			{
				ObjectAddress addr;
				SysScanDesc scan2;
				ScanKeyData key2;
				int			n PG_USED_FOR_ASSERTS_ONLY;

				ObjectAddressSet(addr, ConstraintRelationId, conform->oid);
				add_exact_object_address(&addr, objs);

				/*
				 * First 我们必须 delete the 依赖 record that binds the 约束 records together.
				 *
				 */
				n = deleteDependencyRecordsForSpecific(ConstraintRelationId,
													   conform->oid,
													   DEPENDENCY_INTERNAL,
													   ConstraintRelationId,
													   fk->conoid);
				Assert(n == 1); /* actu所有y 仅 one is expected */

				/*
				 * Now search for the 触发器 for this 约束 and set them up for deletion too
				 *
				 */
				ScanKeyInit(&key2,
							Anum_pg_trigger_tgconstraint,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(conform->oid));
				scan2 = systable_beginscan(trigrel, TriggerConstraintIndexId,
										   true, NULL, 1, &key2);
				while ((trigtup = systable_getnext(scan2)) != NULL)
				{
					ObjectAddressSet(addr, TriggerRelationId,
									 ((Form_pg_trigger) GETSTRUCT(trigtup))->oid);
					add_exact_object_address(&addr, objs);
				}
				systable_endscan(scan2);
			}
		}
		/* 使 the 依赖 deletions 可见 */
		CommandCounterIncrement();
		performMultipleDeletions(objs, DROP_RESTRICT,
								 PERFORM_DELETION_INTERNAL);
		systable_endscan(scan);

		table_close(pg_constraint, RowShareLock);
	}

	CommandCounterIncrement();
	return true;
}

/*
 * GetForeignKeyActionTriggers Returns delete and update "action" 触发器 of the
 * given relation belonging to the given 约束
 *
 */
static void
GetForeignKeyActionTriggers(Relation trigrel,
							Oid conoid, Oid confrelid, Oid conrelid,
							Oid *deleteTriggerOid,
							Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*deleteTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != conrelid)
			continue;
		if (trgform->tgrelid != confrelid)
			continue;
		/* Only ever 查找 at "action" 触发器s on the PK s标识符e. */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_PK)
			continue;
		if (TRIGGER_FOR_DELETE(trgform->tgtype))
		{
			Assert(*deleteTriggerOid == InvalidOid);
			*deleteTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled 构建, continue 查找ing to 查找 duplicates */
		if (OidIsValid(*deleteTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*deleteTriggerOid))
		elog(ERROR, "could not find ON DELETE action trigger of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE action trigger of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * GetForeignKeyCheckTriggers Returns insert and update "check" 触发器 of the
 * given relation belonging to the given 约束
 *
 */
static void
GetForeignKeyCheckTriggers(Relation trigrel,
						   Oid conoid, Oid confrelid, Oid conrelid,
						   Oid *insertTriggerOid,
						   Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*insertTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != confrelid)
			continue;
		if (trgform->tgrelid != conrelid)
			continue;
		/* Only ever 查找 at "检查" 触发器s on the FK s标识符e. */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_FK)
			continue;
		if (TRIGGER_FOR_INSERT(trgform->tgtype))
		{
			Assert(*insertTriggerOid == InvalidOid);
			*insertTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled 构建, continue 查找ing to 查找 duplicates. */
		if (OidIsValid(*insertTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*insertTriggerOid))
		elog(ERROR, "could not find ON INSERT check triggers of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE check triggers of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * ALTER TABLE ALTER CONSTRAINT
 *
 * 更新约束的属性。              
 *
 * 当前仅适用于外键约束。                
 *
 * 如果该约束被修改，则返回其地址；否则返回         
 * InvalidObjectAddress 
 */
static ObjectAddress
ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd, bool recurse,
					  bool recursing, LOCKMODE lockmode)
{
	Constraint *cmdcon;
	Relation	conrel;
	Relation	tgrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	contuple;
	Form_pg_constraint currcon;
	ObjectAddress address;
	List	   *otherrelids = NIL;
	ListCell   *lc;

	cmdcon = castNode(Constraint, cmd->def);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find and check the target 约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(cmdcon->conname));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* 最多只有一行匹配 */
	if (!HeapTupleIsValid(contuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						cmdcon->conname, RelationGetRelationName(rel))));

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	if (currcon->contype != CONSTRAINT_FOREIGN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key constraint",
						cmdcon->conname, RelationGetRelationName(rel))));

	/*
	 * If it's not the topmost 约束, raise an error. Altering a non-topmost 约束
	 * leaves some 触发器 untouched, since they are not directly connected to this
	 * 约束; 也, pg_dump would ignore the deferrability status of the individual 约束,
	 * since it 仅 dumps topmost 约束. Avoid these problems by refusing this
	 * operation and telling the 用户 to alter the parent 约束 相反.
	 *
	 *
	 */
	if (OidIsValid(currcon->conparentid))
	{
		HeapTuple	tp;
		Oid			parent = currcon->conparentid;
		char	   *ancestorname = NULL;
		char	   *ancestortable = NULL;

		/* Loop to 查找 the topmost 约束 */
		while (HeapTupleIsValid(tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parent))))
		{
			Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);

			/* If no 父, this is the 约束 we want */
			if (!OidIsValid(contup->conparentid))
			{
				ancestorname = pstrdup(NameStr(contup->conname));
				ancestortable = get_rel_name(contup->conrelid);
				ReleaseSysCache(tp);
				break;
			}

			parent = contup->conparentid;
			ReleaseSysCache(tp);
		}

		ereport(ERROR,
				(errmsg("cannot alter constraint \"%s\" on relation \"%s\"",
						cmdcon->conname, RelationGetRelationName(rel)),
				 ancestorname && ancestortable ?
				 errdetail("Constraint \"%s\" is derived from constraint \"%s\" of relation \"%s\".",
						   cmdcon->conname, ancestorname, ancestortable) : 0,
				 errhint("You may alter the constraint it derives from instead.")));
	}

	/*
	 * Do the actual 目录 work. 我们可以 skip changing if 已经 in the desired state, but
	 * not if a 分区表: partitions need to be processed regardless, in case they had
	 * the 约束 locally changed.
	 */
	address = InvalidObjectAddress;
	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		if (ATExecAlterConstrRecurse(cmdcon, conrel, tgrel, rel, contuple,
									 &otherrelids, lockmode))
			ObjectAddressSet(address, ConstraintRelationId, currcon->oid);
	}

	/*
	 * ATExecAlterConstrRecurse 已经 已失效 relcache for the relations having the 约束
	 * itself; here we 也 失效 for relations that have any 触发器 that are part of the
	 * 约束.
	 */
	foreach(lc, otherrelids)
		CacheInvalidateRelcacheByRelid(lfirst_oid(lc));

	systable_endscan(scan);

	table_close(tgrel, RowExclusiveLock);
	table_close(conrel, RowExclusiveLock);

	return address;
}

/*
 * ATExecAlterConstraint 的递归子例程。若约束被修改，   
 * 则返回 true。     
 *
 * *otherrelids                                                            
 *
 * 注意，即使值正确也必须递归，以防间接后代       
 * 已在本地修改过它们的约束。                         
 * （若禁止修改分区约束，本可避免，但现有版本并不  
 * 这样做。）                      
 */
static bool
ATExecAlterConstrRecurse(Constraint *cmdcon, Relation conrel, Relation tgrel,
						 Relation rel, HeapTuple contuple, List **otherrelids,
						 LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	Oid			conoid;
	Oid			refrelid;
	bool		changed = false;

	/* 由于此函数递归，可能被驱动到栈溢出 */
	check_stack_depth();

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;
	refrelid = currcon->confrelid;

	/*
	 * 用 cmdcon 中的标志更新 pg_constraint。  
	 *
	 * 若被调用来修改已处于期望状态的约束，则静默地   
	 * 什么也不做。  
	 */
	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred)
	{
		HeapTuple	copyTuple;
		Form_pg_constraint copy_con;
		HeapTuple	tgtuple;
		ScanKeyData tgkey;
		SysScanDesc tgscan;

		copyTuple = heap_copytuple(contuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->condeferrable = cmdcon->deferrable;
		copy_con->condeferred = cmdcon->initdeferred;
		CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId,
								  conoid, 0);

		heap_freetuple(copyTuple);
		changed = true;

		/* Make 新 约束 标志s 可见 to others */
		CacheInvalidateRelcache(rel);

		/*
		 * Now 我们需要 update the multiple entries in pg_trigger that implement the 约束.
		 *
		 */
		ScanKeyInit(&tgkey,
					Anum_pg_trigger_tgconstraint,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(conoid));
		tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
									NULL, 1, &tgkey);
		while (HeapTupleIsValid(tgtuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tgtuple);
			Form_pg_trigger copy_tg;
			HeapTuple	tgCopyTuple;

			/*
			 * Remember OID of other relation(s) involved in FK 约束. (Note: it's likely
			 * that we could skip forcing a relcache inval for other rels that don't
			 * have a 触发器 whose properties change, but let's be conservative.)
			 *
			 */
			if (tgform->tgrelid != RelationGetRelid(rel))
				*otherrelids = list_append_unique_oid(*otherrelids,
													  tgform->tgrelid);

			/*
			 * Update deferrability of RI_FKey_noaction_del, RI_FKey_noaction_upd,
			 * RI_FKey_check_ins and RI_FKey_check_upd 触发器, but not others; see
			 * createForeignKeyActionTriggers and CreateFKCheckTrigger.
			 *
			 */
			if (tgform->tgfoid != F_RI_FKEY_NOACTION_DEL &&
				tgform->tgfoid != F_RI_FKEY_NOACTION_UPD &&
				tgform->tgfoid != F_RI_FKEY_CHECK_INS &&
				tgform->tgfoid != F_RI_FKEY_CHECK_UPD)
				continue;

			tgCopyTuple = heap_copytuple(tgtuple);
			copy_tg = (Form_pg_trigger) GETSTRUCT(tgCopyTuple);

			copy_tg->tgdeferrable = cmdcon->deferrable;
			copy_tg->tginitdeferred = cmdcon->initdeferred;
			CatalogTupleUpdate(tgrel, &tgCopyTuple->t_self, tgCopyTuple);

			InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

			heap_freetuple(tgCopyTuple);
		}

		systable_endscan(tgscan);
	}

	/*
	 * 如果约束任一端的表是分区表，我们就需要递归并     
	 * 处理作为此约束子约束的每一个约束。             
	 *
	 * （这里假定对分区表强制设置了递归标志，而对    
	 * 传统继承则未设置，尽管我们不在此处检查这一      
	 * 点。）  
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
		get_rel_relkind(refrelid) == RELKIND_PARTITIONED_TABLE)
	{
		ScanKeyData pkey;
		SysScanDesc pscan;
		HeapTuple	childtup;

		ScanKeyInit(&pkey,
					Anum_pg_constraint_conparentid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(conoid));

		pscan = systable_beginscan(conrel, ConstraintParentIndexId,
								   true, NULL, 1, &pkey);

		while (HeapTupleIsValid(childtup = systable_getnext(pscan)))
		{
			Form_pg_constraint childcon = (Form_pg_constraint) GETSTRUCT(childtup);
			Relation	childrel;

			childrel = table_open(childcon->conrelid, lockmode);
			ATExecAlterConstrRecurse(cmdcon, conrel, tgrel, childrel, childtup,
									 otherrelids, lockmode);
			table_close(childrel, NoLock);
		}

		systable_endscan(pscan);
	}

	return changed;
}

/*
 * ALTER TABLE VALIDATE CONSTRAINT
 *
 * XXX 我们在此而非在阶段 1 处理递归，原因是：在处理   
 * 外键时没有好的方法可以跳过递归：那种情况下不需要  
 * 锁定子表，但在那个层级我们将无法避免这样           
 * 做。                 
 *
 * 返回值是已验证约束的地址。若该约束已被验证过，      
 * 则返回 InvalidObjectAddress。                       
 */
static ObjectAddress
ATExecValidateConstraint(List **wqueue, Relation rel, char *constrName,
						 bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and check the target 约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* 最多只有一行匹配 */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						constrName, RelationGetRelationName(rel))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_FOREIGN &&
		con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key or check constraint",
						constrName, RelationGetRelationName(rel))));

	if (!con->convalidated)
	{
		AlteredTableInfo *tab;
		HeapTuple	copyTuple;
		Form_pg_constraint copy_con;

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			NewConstraint *newcon;
			Constraint *fkconstraint;

			/* Queue 验证 for 阶段 3 */
			fkconstraint = makeNode(Constraint);
			/* for 现在 this is 所有 we 需要 */
			fkconstraint->conname = constrName;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = constrName;
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = con->confrelid;
			newcon->refindid = con->conindid;
			newcon->conid = con->oid;
			newcon->qual = (Node *) fkconstraint;

			/* Find or 创建 工作 队列 条目 for this 表 */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * We disallow creating 无效 foreign keys to or from partitioned tables, so
			 * ignoring the 递归 bit is okay.
			 */
		}
		else if (con->contype == CONSTRAINT_CHECK)
		{
			List	   *children = NIL;
			ListCell   *child;
			NewConstraint *newcon;
			Datum		val;
			char	   *conbin;

			/*
			 * If we're 递归中, the parent has 已经 done this, so skip it. Also, if the 约束
			 * is a NO INHERIT 约束, we shouldn't try to look for it in the children.
			 *
			 */
			if (!recursing && !con->connoinherit)
				children = find_all_inheritors(RelationGetRelid(rel),
											   lockmode, NULL);

			/*
			 * 对 CHECK 约束，必须确保只有当子表上          
			 * 已验证时，才在父表上把它标记                     
			 * 为已验证。 
			 *
			 * 我们在父表上验证之前递归，以减少死锁       
			 * 风险。 
			 */
			foreach(child, children)
			{
				Oid			childoid = lfirst_oid(child);
				Relation	childrel;

				if (childoid == RelationGetRelid(rel))
					continue;

				/*
				 * If we are told not to 递归, there had better not be any child tables, 因为
				 * 我们可以't mark the 约束 on the parent valid unless it is valid for all child
				 * tables.
				 */
				if (!recurse)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("constraint must be validated on child tables too")));

				/* find_all_inheritors 已获取锁 */
				childrel = table_open(childoid, NoLock);

				ATExecValidateConstraint(wqueue, childrel, constrName, false,
										 true, lockmode);
				table_close(childrel, NoLock);
			}

			/* Queue 验证 for 阶段 3 */
			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = constrName;
			newcon->contype = CONSTR_CHECK;
			newcon->refrelid = InvalidOid;
			newcon->refindid = InvalidOid;
			newcon->conid = con->oid;

			val = SysCacheGetAttrNotNull(CONSTROID, tuple,
										 Anum_pg_constraint_conbin);
			conbin = TextDatumGetCString(val);
			newcon->qual = (Node *) stringToNode(conbin);

			/* Find or 创建 工作 队列 条目 for this 表 */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * Invalidate relcache 从而 others see the new 已验证 约束.
			 *
			 */
			CacheInvalidateRelcache(rel);
		}

		/*
		 * Now update the 目录, while we have the door open.
		 */
		copyTuple = heap_copytuple(tuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->convalidated = true;
		CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

		heap_freetuple(copyTuple);

		ObjectAddressSet(address, ConstraintRelationId, con->oid);
	}
	else
		address = InvalidObjectAddress; /* 已经 验证d */

	systable_endscan(scan);

	table_close(conrel, RowExclusiveLock);

	return address;
}


/*
 * transformColumnNameList - 转换列名列表            
 *
 * 查找每个名称，返回其 attnum 与可选的类型 OID    
 *
 * 注意：此函数的名字看似表明它是通用的，但实际 
 * 上它仅用于查找出现在外键子句中的名称。            
 * 错误消息需要改进才能在其他情况下使用它，          
 * 或许有效性检查也是如此。    
 */
static int
transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids)
{
	ListCell   *l;
	int			attnum;

	attnum = 0;
	foreach(l, colList)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;
		Form_pg_attribute attform;

		atttuple = SearchSysCacheAttName(relId, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in foreign key constraint does not exist",
							attname)));
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);
		if (attform->attnum < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("system columns cannot be used in foreign keys")));
		if (attnum >= INDEX_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in a foreign key",
							INDEX_MAX_KEYS)));
		attnums[attnum] = attform->attnum;
		if (atttypids != NULL)
			atttypids[attnum] = attform->atttypid;
		ReleaseSysCache(atttuple);
		attnum++;
	}

	return attnum;
}

/*
 * transformFkeyGetPrimaryKey -
 *
 *	查找 pkrel 的主键属性的名称、attnum 和类型           
 *	还要返回支持主键的索引的 OID 和索引                  
 *	操作符类。                  
 *
 *	除 pkrel 外，所有参数都是输出参数。另外，函数的返
 *	回值是主键中属性的数量。                        
 *
 *	在 REFERENCES 规范中的列列表被省略时使用。            
 */
static int
transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	HeapTuple	indexTuple = NULL;
	Form_pg_index indexStruct = NULL;
	Datum		indclassDatum;
	oidvector  *indclass;
	int			i;

	/*
	 * Get the list of 索引 OID for the table from the relcache, and look up each
	 * one in the pg_index syscache until we find one marked 主键 (hopefully there
	 * isn't more than one such). Insist it's valid, too.
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary && indexStruct->indisvalid)
		{
			/*
			 * Refuse to use a deferrable 主键. This is per SQL spec, and there 将会 a lot
			 * of interesting semantic problems if we tried to allow it.
			 *
			 */
			if (!indexStruct->indimmediate)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot use a deferrable primary key for referenced table \"%s\"",
								RelationGetRelationName(pkrel))));

			*indexOid = indexoid;
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * 检查 we found it
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	/* Must 获取 indclass the hard way */
	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Now build the list of PK 属性 from the indkey definition (we assume a 主键
	 * cannot have expressional elements)
	 */
	*attnamelist = NIL;
	for (i = 0; i < indexStruct->indnkeyatts; i++)
	{
		int			pkattno = indexStruct->indkey.values[i];

		attnums[i] = pkattno;
		atttypids[i] = attnumTypeId(pkrel, pkattno);
		opclasses[i] = indclass->values[i];
		*attnamelist = lappend(*attnamelist,
							   makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
	}

	ReleaseSysCache(indexTuple);

	return i;
}

/*
 * transformFkeyCheckAttrs -
 *
 *	验证 'pkrel' 关系中的 'attnums' 列作为外键约束的一部分 
 *	来引用是有效的。                      
 *
 *	返回支持该约束的唯一索引的 OID，并                
 *	用与索引列相关联的操作符类填充调用者提供的   
 *	'opclasses' 数组。             
 *
 *	验证失败时抛出 ERROR。        
 */
static Oid
transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses)
{
	Oid			indexoid = InvalidOid;
	bool		found = false;
	bool		found_deferrable = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i,
				j;

	/*
	 * Reject duplicate appearances of columns in the referenced-columns list.
	 * Such a case is forbidden by the SQL standard, and even if we thought it
	 * useful to allow it, there 将会 ambiguity about how to match the list to
	 * unique 索引 (in particular, it'd be unclear which 索引 opclass goes with which
	 * FK column).
	 */
	for (i = 0; i < numattrs; i++)
	{
		for (j = i + 1; j < numattrs; j++)
		{
			if (attnums[i] == attnums[j])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FOREIGN_KEY),
						 errmsg("foreign key referenced-columns list must not contain duplicates")));
		}
	}

	/*
	 * Get the list of 索引 OID for the table from the relcache, and look up each
	 * one in the pg_index syscache, and match unique 索引 to the list of attnums
	 * we are given.
	 */
	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; 必须是 unique and not a partial 索引;
		 * forget it if there are any expressions, too. Invalid 索引 are out 同时.
		 *
		 */
		if (indexStruct->indnkeyatts == numattrs &&
			indexStruct->indisunique &&
			indexStruct->indisvalid &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL))
		{
			Datum		indclassDatum;
			oidvector  *indclass;

			/* Must 获取 indclass the hard way */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the 索引 columns in any order. Check for a
			 * match, and extract the appropriate opclasses while we're at it. We know
			 * that attnums[] is duplicate-free per the test at the start of this
			 * function, and we checked 以上 that the number of 索引 columns agrees, so if
			 * we find a match for each attnums[] entry then 我们必须 have a one-to-one
			 * match in some order.
			 *
			 *
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						opclasses[i] = indclass->values[j];
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}

			/*
			 * Refuse to use a deferrable unique/主键. This is per SQL spec, and there 将会
			 * a lot of interesting semantic problems if we tried to allow it.
			 *
			 */
			if (found && !indexStruct->indimmediate)
			{
				/*
				 * Remember that we found an 否则 matching 索引, 从而 我们可以 generate a more
				 * appropriate error message.
				 */
				found_deferrable = true;
				found = false;
			}
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
	{
		if (found_deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a deferrable unique constraint for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
	}

	list_free(indexoidlist);

	return indexoid;
}

/*
 * findFkeyCast -
 *
 *	Wrapper around find_coercion_pathway() for ATAddForeignKeyConstraint().
 *	Caller has equal regard for binary coercibility and for an exact match.
*/
static CoercionPathType
findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid *funcid)
{
	CoercionPathType ret;

	if (targetTypeId == sourceTypeId)
	{
		ret = COERCION_PATH_RELABELTYPE;
		*funcid = InvalidOid;
	}
	else
	{
		ret = find_coercion_pathway(targetTypeId, sourceTypeId,
									COERCION_IMPLICIT, funcid);
		if (ret == COERCION_PATH_NONE)
			/* A 前一个ly-relied-upon cast is 现在 gone. */
			elog(ERROR, "could not find cast from %u to %u",
				 sourceTypeId, targetTypeId);
	}

	return ret;
}

/*
 * Permissions 检查 on the referenced table for ADD FOREIGN KEY Note: we have 已经
 * checked that the 用户 owns the referencing table, else we'd have failed much
 * 更早的; no additional 检查 是必需的 for it.
 *
 */
static void
checkFkeyPermissions(Relation rel, int16 *attnums, int natts)
{
	Oid			roleid = GetUserId();
	AclResult	aclresult;
	int			i;

	/* Okay if we have 关系-级别 REFERENCES 权限 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid,
								  ACL_REFERENCES);
	if (aclresult == ACLCHECK_OK)
		return;
	/* Else we must have REFERENCES on 每 列 */
	for (i = 0; i < natts; i++)
	{
		aclresult = pg_attribute_aclcheck(RelationGetRelid(rel), attnums[i],
										  roleid, ACL_REFERENCES);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));
	}
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK 约束.
 * Caller must have opened and locked both relations appropriately.
 *
 *
 */
static void
validateForeignKeyConstraint(char *conname,
							 Relation rel,
							 Relation pkrel,
							 Oid pkindOid,
							 Oid constraintOid)
{
	TupleTableSlot *slot;
	TableScanDesc scan;
	Trigger		trig = {0};
	Snapshot	snapshot;
	MemoryContext oldcxt;
	MemoryContext perTupCxt;

	ereport(DEBUG1,
			(errmsg_internal("validating foreign key constraint \"%s\"", conname)));

	/*
	 * Build a 触发器 call structure; we'll need it either way.
	 */
	trig.tgoid = InvalidOid;
	trig.tgname = conname;
	trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trig.tgisinternal = true;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgconstrindid = pkindOid;
	trig.tgconstraint = constraintOid;
	trig.tgdeferrable = false;
	trig.tginitdeferred = false;
	/* we 需要n't fill in remaining 字段s */

	/*
	 * See if 我们可以 do it with a single LEFT JOIN 查询. A false result indicates
	 * 我们必须 proceed with the fire-the-触发器 method.
	 */
	if (RI_Initial_Check(&trig, rel, pkrel))
		return;

	/*
	 * Scan through each 元组, calling RI_FKey_check_ins (insert 触发器) as if that 元组
	 * had just been inserted. If any of those fail, it should ereport(ERROR) and
	 * that's that.
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "validateForeignKeyConstraint",
									  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(perTupCxt);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		LOCAL_FCINFO(fcinfo, 0);
		TriggerData trigdata = {0};

		CHECK_FOR_INTERRUPTS();

		/*
		 * Make a call to the 触发器 function No parameters are passed, but we do set a
		 * context
		 *
		 */
		MemSet(fcinfo, 0, SizeForFunctionCallInfo(0));

		/*
		 * We assume RI_FKey_check_ins won't look at flinfo...
		 */
		trigdata.type = T_TriggerData;
		trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
		trigdata.tg_relation = rel;
		trigdata.tg_trigtuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		trigdata.tg_trigslot = slot;
		trigdata.tg_trigger = &trig;

		fcinfo->context = (Node *) &trigdata;

		RI_FKey_check_ins(fcinfo);

		MemoryContextReset(perTupCxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perTupCxt);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * CreateFKCheckTrigger
 *		创建实现给定外键的 insert (on_insert=true) 或 update    
 *		"check" 触发器。          
 *
 * 返回这样创建的触发器的 OID。  
 */
static Oid
CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
					 Oid constraintOid, Oid indexOid, Oid parentTrigOid,
					 bool on_insert)
{
	ObjectAddress trigAddress;
	CreateTrigStmt *fk_trigger;

	/*
	 * Note: for a self-referential FK (referencing and referenced tables are the
	 * same), it is important that the ON UPDATE action fires 之前 the CHECK
	 * action, since both 触发器 will fire on the same row 期间 an UPDATE event; 否则
	 * the CHECK 触发器 将被 checking a non-final state of the row. Triggers fire in
	 * name order, so we ensure this by using names like
	 * "RI_ConstraintTrigger_a_NNNN" for the action 触发器 and
	 * "RI_ConstraintTrigger_c_NNNN" for the check 触发器.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_c";
	fk_trigger->relation = NULL;

	/* Either ON INSERT or ON UPDATE */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->events = TRIGGER_TYPE_INSERT;
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->events = TRIGGER_TYPE_UPDATE;
	}

	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = NULL;

	trigAddress = CreateTrigger(fk_trigger, NULL, myRelOid, refRelOid,
								constraintOid, indexOid, InvalidOid,
								parentTrigOid, NULL, true, false);

	/* Make 变更s-so-far 可见 */
	CommandCounterIncrement();

	return trigAddress.objectId;
}

/*
 * createForeignKeyActionTriggers
 *		创建实现一个外键的被引用侧 "action" 触发器。        
 *		    
 *
 * 把这样创建的触发器的 OID 返回在 *deleteTrigOid 和  
 * *updateTrigOid 
 */
static void
createForeignKeyActionTriggers(Relation rel, Oid refRelOid, Constraint *fkconstraint,
							   Oid constraintOid, Oid indexOid,
							   Oid parentDelTrigger, Oid parentUpdTrigger,
							   Oid *deleteTrigOid, Oid *updateTrigOid)
{
	CreateTrigStmt *fk_trigger;
	ObjectAddress trigAddress;

	/*
	 * Build and 执行 a CREATE CONSTRAINT TRIGGER statement for the ON DELETE
	 * action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_DELETE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;
	switch (fkconstraint->fk_del_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_del_action);
			break;
	}

	trigAddress = CreateTrigger(fk_trigger, NULL, refRelOid,
								RelationGetRelid(rel),
								constraintOid, indexOid, InvalidOid,
								parentDelTrigger, NULL, true, false);
	if (deleteTrigOid)
		*deleteTrigOid = trigAddress.objectId;

	/* Make 变更s-so-far 可见 */
	CommandCounterIncrement();

	/*
	 * Build and 执行 a CREATE CONSTRAINT TRIGGER statement for the ON UPDATE
	 * action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_UPDATE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;
	switch (fkconstraint->fk_upd_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_upd_action);
			break;
	}

	trigAddress = CreateTrigger(fk_trigger, NULL, refRelOid,
								RelationGetRelid(rel),
								constraintOid, indexOid, InvalidOid,
								parentUpdTrigger, NULL, true, false);
	if (updateTrigOid)
		*updateTrigOid = trigAddress.objectId;
}

/*
 * createForeignKeyCheckTriggers
 *		创建实现一个外键的引用侧 "check" 触发器。            
 *		    
 *
 * 把这样创建的触发器的 OID 返回在 *insertTrigOid 和  
 * *updateTrigOid 
 */
static void
createForeignKeyCheckTriggers(Oid myRelOid, Oid refRelOid,
							  Constraint *fkconstraint, Oid constraintOid,
							  Oid indexOid,
							  Oid parentInsTrigger, Oid parentUpdTrigger,
							  Oid *insertTrigOid, Oid *updateTrigOid)
{
	*insertTrigOid = CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint,
										  constraintOid, indexOid,
										  parentInsTrigger, true);
	*updateTrigOid = CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint,
										  constraintOid, indexOid,
										  parentUpdTrigger, false);
}

/*
 * ALTER TABLE DROP CONSTRAINT Like DROP COLUMN, 我们可以't use the normal ALTER
 * TABLE 递归 mechanism.
 *
 */
static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior,
					 bool recurse, bool recursing,
					 bool missing_ok, LOCKMODE lockmode)
{
	List	   *children;
	ListCell   *child;
	Relation	conrel;
	Form_pg_constraint con;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
	bool		found = false;
	bool		is_no_inherit_constraint = false;
	char		contype;

	/* 由于此函数递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/* 在顶层，权限检查已在 ATPrepCmd 中完成，否则在此执行 */
	if (recursing)
		ATSimplePermissions(AT_DropConstraint, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and drop the target 约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* 最多只有一行匹配 */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress conobj;

		con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't 删除 继承ed 约束s */
		if (con->coninhcount > 0 && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
							constrName, RelationGetRelationName(rel))));

		is_no_inherit_constraint = con->connoinherit;
		contype = con->contype;

		/*
		 * If it's a foreign-key 约束, we'd better 锁 the referenced table and 检查
		 * that's not in use, just as we've 已经 done for the constrained table (else
		 * we might, eg, be dropping a 触发器 that has unfired events). But 我们可以/must
		 * skip that in the self-referential case.
		 *
		 */
		if (contype == CONSTRAINT_FOREIGN &&
			con->confrelid != RelationGetRelid(rel))
		{
			Relation	frel;

			/* Must match 锁 taken by RemoveTriggerById: */
			frel = table_open(con->confrelid, AccessExclusiveLock);
			CheckAlterTableIsSafe(frel);
			table_close(frel, NoLock);
		}

		/*
		 * Perform the actual 约束 deletion
		 */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = con->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);

		found = true;
	}

	systable_endscan(scan);

	if (!found)
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
							constrName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of relation \"%s\" does not exist, skipping",
							constrName, RelationGetRelationName(rel))));
			table_close(conrel, RowExclusiveLock);
			return;
		}
	}

	/*
	 * For partitioned tables, non-CHECK 继承的 约束 被删除 via the 依赖 mechanism, so
	 * we're done here.
	 */
	if (contype != CONSTRAINT_CHECK &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		table_close(conrel, RowExclusiveLock);
		return;
	}

	/*
	 * Propagate to children as appropriate. Unlike most other ALTER routines, we
	 * have to do this one level of 递归 at a time; 我们可以't use find_all_inheritors
	 * to do it in one pass.
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	/*
	 * For a 分区表, if partitions exist and we are told not to 递归, it's a 用户 error.
	 * It doesn't make sense to have a 约束 be defined 仅 on the parent, especially
	 * if it's a 分区表.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		children != NIL && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot remove constraint from only the partitioned table when partitions exist"),
				 errhint("Do not specify the ONLY keyword.")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		HeapTuple	copy_tuple;

		/* find_inheritance_children 已获取锁 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		ScanKeyInit(&skey[0],
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(childrelid));
		ScanKeyInit(&skey[1],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(InvalidOid));
		ScanKeyInit(&skey[2],
					Anum_pg_constraint_conname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(constrName));
		scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
								  true, NULL, 3, skey);

		/* 最多只有一行匹配 */
		if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
							constrName,
							RelationGetRelationName(childrel))));

		copy_tuple = heap_copytuple(tuple);

		systable_endscan(scan);

		con = (Form_pg_constraint) GETSTRUCT(copy_tuple);

		/* Right 现在 仅 CHECK 约束s 可以是 继承ed */
		if (con->contype != CONSTRAINT_CHECK)
			elog(ERROR, "inherited constraint is not a CHECK constraint");

		if (con->coninhcount <= 0)	/* 不应发生 */
			elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
				 childrelid, constrName);

		if (recurse)
		{
			/*
			 * If the child 约束 has other definition sources, just decrement its 继承
			 * count; 如果不是, 递归 to delete it.
			 */
			if (con->coninhcount == 1 && !con->conislocal)
			{
				/* Time to 删除 this 子 约束, too */
				ATExecDropConstraint(childrel, constrName, behavior,
									 true, true,
									 false, lockmode);
			}
			else
			{
				/* Child 约束 must survive my deletion */
				con->coninhcount--;
				CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

				/* 使更新可见 */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * If we were told to drop ONLY in this table (no 递归), 我们需要 mark the 继承者'
			 * 约束 as locally defined rather than 继承的.
			 *
			 */
			con->coninhcount--;
			con->conislocal = true;

			CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

			/* 使更新可见 */
			CommandCounterIncrement();
		}

		heap_freetuple(copy_tuple);

		table_close(childrel, NoLock);
	}

	table_close(conrel, RowExclusiveLock);
}

/*
 * ALTER COLUMN TYPE
 *
 * 与其他子命令类型不同，我们在阶段 1 期间为 ALTER COLUMN 
 * TYPE 做解析转换 --- 传入这里的 AlterTableCmd 已转换   
 * 过（必须如此，因为我们依赖某些已转换的字段）。 
 *
 * 关键点在于：一个表的所有 ALTER COLUMN TYPE 的执行将在 
 * 在阶段 3 "并行"完成，因此所有 USING 表达式都应  
 * 假定原始列类型来解析。此外，它允许 USING 表达式引 
 * 用一个将被删除的字段。                                       
 *
 * 为了安全地做到这一点，AT_PASS_DROP 和 AT_PASS_ALTER_TYPE  
 * 必须作为阶段 2 的头两个执行步骤；它们绝不能看到  
 * 其他子命令类型的效果，因为 USING 表达式针对未修  
 * 改的表状态解析的。          
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd, LOCKMODE lockmode,
					  AlterTableUtilityContext *context)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	Node	   *transform = def->cooked_default;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);
	AclResult	aclresult;
	bool		is_expr;

	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter column type of typed table")));

	/* 查找up the 属性 so we can 检查 继承ance status */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't 修改 a system 属性 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Cannot specify USING when altering type of a 生成列, 因为 that would violate
	 * the 生成表达式.
	 */
	if (attTup->attgenerated && def->cooked_default)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("cannot specify USING when altering type of generated column"),
				 errdetail("Column \"%s\" is a generated column.", colName)));

	/*
	 * Don't alter 继承的 columns. At outer level, there had better not be any 继承的
	 * definition; when 递归中, we assume this was checked at the parent level (see
	 * 以下).
	 */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"",
						colName)));

	/* Don't 修改 列 使用 in the 分区 键 */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	/* Look up the tar获取 类型 */
	typenameTypeIdAndMod(NULL, typeName, &targettype, &targettypmod);

	aclresult = object_aclcheck(TypeRelationId, targettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);

	/* And the 排序规则 */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/* 使 sure 数据类型 is legal for a 列 */
	CheckAttributeType(colName, targettype, targetcollid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	if (tab->relkind == RELKIND_RELATION ||
		tab->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Set up an expression to 转换 the old data value to the new type. If a USING
		 * option was given, use the expression as 转换后的 by transformAlterTableStmt,
		 * else just take the old value and try to coerce it. We do this first 从而
		 * type incompatibility can be detected 之前 we waste effort, and 因为 we need
		 * the expression to be parsed against the original table 行类型.
		 *
		 *
		 */
		if (!transform)
		{
			transform = (Node *) makeVar(1, attnum,
										 attTup->atttypid, attTup->atttypmod,
										 attTup->attcollation,
										 0);
		}

		transform = coerce_to_target_type(pstate,
										  transform, exprType(transform),
										  targettype, targettypmod,
										  COERCION_ASSIGNMENT,
										  COERCE_IMPLICIT_CAST,
										  -1);
		if (transform == NULL)
		{
			/* 错误 text depends on whether USING was specified or not */
			if (def->cooked_default != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("result of USING clause for column \"%s\""
								" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 errhint("You might need to add an explicit cast.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 !attTup->attgenerated ?
				/* translator: USING is SQL, don't translate it */
						 errhint("You might need to specify \"USING %s::%s\".",
								 quote_identifier(colName),
								 format_type_with_typemod(targettype,
														  targettypmod)) : 0));
		}

		/* Fix 排序规则s 之后 所有 else */
		assign_expr_collations(pstate, transform);

		/* Plan the expr 现在 so we can accurately assess the 需要 to 重写. */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * Add a work queue item to make ATRewriteTable update the column
		 * contents.
		 */
		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attnum;
		newval->expr = (Expr *) transform;
		newval->is_generated = false;

		tab->newvals = lappend(tab->newvals, newval);
		if (ATColumnChangeRequiresRewrite(transform, attnum))
			tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
	}
	else if (transform)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!RELKIND_HAS_STORAGE(tab->relkind))
	{
		/*
		 * For relations 没有 存储, do this check now. Regular tables will check it 稍后的
		 * when the table is being 重写.
		 */
		find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
	}

	ReleaseSysCache(tuple);

	/*
	 * 必要时，通过为每个子表排队一个新命令来手 
	 * 动递归。这里不能用 ATSimpleRecursion，因为需重        
	 * 映射 USING 表达式（如果有）中的属性号。 
	 *
	 * 若被告知不要递归，则最好没有任何子表；否则 
	 * 修改会使它们失去同步。                 
	 */
	if (recurse)
	{
		Oid			relid = RelationGetRelid(rel);
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		child_oids = find_all_inheritors(relid, lockmode,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the 继承 hierarchy, so all
		 * we have to do is process all of the relids in the list that it 返回.
		 *
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);
			Relation	childrel;
			HeapTuple	childtuple;
			Form_pg_attribute childattTup;

			if (childrelid == relid)
				continue;

			/* find_all_inheritors 已获取锁 */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			/*
			 * Verify that the child doesn't have any 继承的 definitions of this column
			 * that came from outside this 继承 hierarchy. (renameatt makes a similar
			 * test, though in a different way 因为 of its different 递归 mechanism.)
			 *
			 */
			childtuple = SearchSysCacheAttName(RelationGetRelid(childrel),
											   colName);
			if (!HeapTupleIsValid(childtuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								colName, RelationGetRelationName(childrel))));
			childattTup = (Form_pg_attribute) GETSTRUCT(childtuple);

			if (childattTup->attinhcount > numparents)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot alter inherited column \"%s\" of relation \"%s\"",
								colName, RelationGetRelationName(childrel))));

			ReleaseSysCache(childtuple);

			/*
			 * Remap the 属性 numbers. If no USING expression was specified, there is no
			 * need for this step.
			 */
			if (def->cooked_default)
			{
				AttrMap    *attmap;
				bool		found_whole_row;

				/* 创建 a 复制 to scribble on */
				cmd = copyObject(cmd);

				attmap = build_attrmap_by_name(RelationGetDescr(childrel),
											   RelationGetDescr(rel),
											   false);
				((ColumnDef *) cmd->def)->cooked_default =
					map_variable_attnos(def->cooked_default,
										1, 0,
										attmap,
										InvalidOid, &found_whole_row);
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("USING expression contains a whole-row table reference.")));
				pfree(attmap);
			}
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel), NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName)));

	if (tab->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);
}

/*
 * When the 数据类型 of a column is changed, a 重写 might not be required if the new
 * type is sufficiently identical to the old one, and the USING clause isn't
 * trying to insert some other value. It's safe to skip the 重写 in these cases:
 * - the old type is binary coercible to the new type - the new type is an
 * unconstrained domain over the old type - {NEW,OLD} or {OLD,NEW} is
 * {timestamptz,timestamp} and the timezone is UTC In the case of a
 * constrained domain, we could get by with scanning the table and checking
 * the 约束 rather than actually rewriting it, but 我们不 当前 try to do that.
 *
 *
 *
 *
 */
static bool
ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		/* 仅 one varno, so no 需要 to 检查 that */
		if (IsA(expr, Var) && ((Var *) expr)->varattno == varattno)
			return false;
		else if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
		{
			CoerceToDomain *d = (CoerceToDomain *) expr;

			if (DomainHasConstraints(d->resulttype))
				return true;
			expr = (Node *) d->arg;
		}
		else if (IsA(expr, FuncExpr))
		{
			FuncExpr   *f = (FuncExpr *) expr;

			switch (f->funcid)
			{
				case F_TIMESTAMPTZ_TIMESTAMP:
				case F_TIMESTAMP_TIMESTAMPTZ:
					if (TimestampTimestampTzRequiresRewrite())
						return true;
					else
						expr = linitial(f->args);
					break;
				default:
					return true;
			}
		}
		else
			return true;
	}
}

/*
 * ALTER COLUMN .. SET DATA TYPE
 *
 * Return the address of the modified column.
 */
static ObjectAddress
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	HeapTuple	heapTup;
	Form_pg_attribute attTup,
				attOldTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	ObjectAddress address;

	/*
	 * Clear all the missing values if we're rewriting the table, since this
	 * renders them pointless.
	 */
	if (tab->rewrite)
	{
		Relation	newrel;

		newrel = table_open(RelationGetRelid(rel), NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
		/* 使 sure we 执行n't conflict with 稍后 属性 modifications */
		CommandCounterIncrement();
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	/* Look up the tar获取 列 */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup)) /* 不应发生 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;
	attOldTup = TupleDescAttr(tab->oldDesc, attnum - 1);

	/* Check for multiple ALTER TYPE on 相同 列 --- can't cope */
	if (attTup->atttypid != attOldTup->atttypid ||
		attTup->atttypmod != attOldTup->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName)));

	/* Look up the tar获取 类型 (should not fail, since prep 找到 it) */
	typeTuple = typenameType(NULL, typeName, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = tform->oid;
	/* And the 排序规则 */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/*
	 * If there is a 默认表达式 for the column, get it and ensure 我们可以 coerce it to
	 * the new datatype. (我们必须 do this 之前 changing the 列类型, 因为
	 * build_column_default itself will try to coerce, and will not issue the
	 * error message we want if it fails.) We remove any implicit coercion steps
	 * at the top level of the old 默认表达式; this has been agreed to satisfy the
	 * principle of least surprise. (The conversion to the new 列类型 should act
	 * like it started from what the 用户 sees as the stored expression, and the
	 * implicit coercions aren't going to be shown.)
	 *
	 *
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,	/* no UNKNOWN params */
											defaultexpr, exprType(defaultexpr),
											targettype, targettypmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (defaultexpr == NULL)
		{
			if (attTup->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("generation expression for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("default for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
		}
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (约束, 索引, etc), and record
	 * enough information to let us recreate the objects. The actual recreation
	 * does not happen here, but 仅 之后 we have performed all the individual ALTER
	 * TYPE operations. We have to save the info 之前 executing ALTER TYPE, though,
	 * else the deparser will get confused.
	 *
	 *
	 */
	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		foundObject.classId = foundDep->classid;
		foundObject.objectId = foundDep->objid;
		foundObject.objectSubId = foundDep->objsubid;

		switch (getObjectClass(&foundObject))
		{
			case OCLASS_CLASS:
				{
					char		relKind = get_rel_relkind(foundObject.objectId);

					if (relKind == RELKIND_INDEX ||
						relKind == RELKIND_PARTITIONED_INDEX)
					{
						Assert(foundObject.objectSubId == 0);
						RememberIndexForRebuilding(foundObject.objectId, tab);
					}
					else if (relKind == RELKIND_SEQUENCE)
					{
						/*
						 * This 必须是 a SERIAL column's 序列. We need not do anything to it.
						 *
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else
					{
						/* Not expecting 任何 other direct 依赖... */
						elog(ERROR, "unexpected object depending on column: %s",
							 getObjectDescription(&foundObject, false));
					}
					break;
				}

			case OCLASS_CONSTRAINT:
				Assert(foundObject.objectSubId == 0);
				RememberConstraintForRebuilding(foundObject.objectId, tab);
				break;

			case OCLASS_PROC:

				/*
				 * A new-style SQL function can depend on a column, if that
				 * column is referenced in the parsed function body.  Ideally
				 * we'd automatically update the function by deparsing and
				 * reparsing it, but that's risky and might well fail anyhow.
				 * FIXME someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a function or procedure"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_REWRITE:

				/*
				 * View/rule bodies have pretty much the same issues as
				 * function bodies.  FIXME someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a view or rule"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_TRIGGER:

				/*
				 * A 触发器 can depend on a column 因为 the column is specified as an update
				 * target, or 因为 the column 被使用 in the 触发器's WHEN condition. The first
				 * case would not require any extra work, but the second case would
				 * require updating the WHEN expression, which has the same issues as 以上.
				 * Since 我们可以't easily tell which case applies, we punt for both. FIXME
				 * someday.
				 *
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a trigger definition"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_POLICY:

				/*
				 * A policy can depend on a column 因为 the column is specified in the
				 * policy's USING or WITH CHECK qual expressions. It 可能 possible to 重写 and
				 * recheck the policy expression, but punt for now. It's certainly easy
				 * enough to remove and recreate the policy; still, FIXME someday.
				 *
				 *
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a policy definition"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_DEFAULT:
				{
					ObjectAddress col = GetAttrDefaultColumnAddress(foundObject.objectId);

					if (col.objectId == RelationGetRelid(rel) &&
						col.objectSubId == attnum)
					{
						/*
						 * Ignore the column's own 默认表达式, which we will deal with 以下.
						 *
						 */
						Assert(defaultexpr);
					}
					else
					{
						/*
						 * This 必须是 a reference from the expression of a 生成列 elsewhere in the
						 * same table. Changing the type of a column that 被使用 by a 生成列 不被允许 by
						 * SQL standard, so just punt for now. It 可能 doable with some thinking
						 * and effort.
						 *
						 *
						 */
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot alter type of a column used by a generated column"),
								 errdetail("Column \"%s\" is used by generated column \"%s\".",
										   colName,
										   get_attname(col.objectId,
													   col.objectSubId,
													   false))));
					}
					break;
				}

			case OCLASS_STATISTIC_EXT:

				/*
				 * Give the extended-stats machinery a chance to fix anything that this
				 * 列类型 change would break.
				 */
				RememberStatisticsForRebuilding(foundObject.objectId, tab);
				break;

			case OCLASS_PUBLICATION_REL:

				/*
				 * Column reference in a PUBLICATION ... FOR TABLE ... WHERE clause. Same
				 * issues as 以上. FIXME someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a publication WHERE clause"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_TYPE:
			case OCLASS_CAST:
			case OCLASS_COLLATION:
			case OCLASS_CONVERSION:
			case OCLASS_LANGUAGE:
			case OCLASS_LARGEOBJECT:
			case OCLASS_OPERATOR:
			case OCLASS_OPCLASS:
			case OCLASS_OPFAMILY:
			case OCLASS_AM:
			case OCLASS_AMOP:
			case OCLASS_AMPROC:
			case OCLASS_SCHEMA:
			case OCLASS_TSPARSER:
			case OCLASS_TSDICT:
			case OCLASS_TSTEMPLATE:
			case OCLASS_TSCONFIG:
			case OCLASS_ROLE:
			case OCLASS_ROLE_MEMBERSHIP:
			case OCLASS_DATABASE:
			case OCLASS_TBLSPACE:
			case OCLASS_FDW:
			case OCLASS_FOREIGN_SERVER:
			case OCLASS_USER_MAPPING:
			case OCLASS_DEFACL:
			case OCLASS_EXTENSION:
			case OCLASS_EVENT_TRIGGER:
			case OCLASS_PARAMETER_ACL:
			case OCLASS_PUBLICATION:
			case OCLASS_PUBLICATION_NAMESPACE:
			case OCLASS_SUBSCRIPTION:
			case OCLASS_TRANSFORM:

				/*
				 * 我们不 expect any of these sorts of objects to depend on a column.
				 *
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject, false));
				break;

				/*
				 * There's intentionally no default: case here; we want the compiler to
				 * warn if a new OCLASS hasn't been handled 以上.
				 */
		}
	}

	systable_endscan(scan);

	/*
	 * Now 扫描 for 依赖 of this column on other things. The 仅 things we should find
	 * are the 依赖 on the column datatype and possibly a 排序规则 依赖. Those can be
	 * removed.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		foundObject.classId = foundDep->refclassid;
		foundObject.objectId = foundDep->refobjid;
		foundObject.objectSubId = foundDep->refobjsubid;

		if (foundDep->deptype != DEPENDENCY_NORMAL)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (!(foundDep->refclassid == TypeRelationId &&
			  foundDep->refobjid == attTup->atttypid) &&
			!(foundDep->refclassid == CollationRelationId &&
			  foundDep->refobjid == attTup->attcollation))
			elog(ERROR, "found unexpected dependency for column: %s",
				 getObjectDescription(&foundObject, false));

		CatalogTupleDelete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	/*
	 * Here we go --- change the recorded 列类型 and 排序规则. (Note heapTup is a copy
	 * of the syscache entry, so okay to scribble on.) First fix up the missing
	 * value 如有.
	 */
	if (attTup->atthasmissing)
	{
		Datum		missingval;
		bool		missingNull;

		/* if 重写 is 真 the missing 值 should 已经 be cleared */
		Assert(tab->rewrite == 0);

		/* Get the missing 值 datum */
		missingval = heap_getattr(heapTup,
								  Anum_pg_attribute_attmissingval,
								  attrelation->rd_att,
								  &missingNull);

		/* if it's a null array t此处 不是hing to 执行 */

		if (!missingNull)
		{
			/*
			 * Get the datum out of the array and repack it in a new array built with
			 * the new type data. We assume that since the table doesn't need
			 * rewriting, the actual Datum doesn't need to be changed, 仅 the array
			 * metadata.
			 */

			int			one = 1;
			bool		isNull;
			Datum		valuesAtt[Natts_pg_attribute] = {0};
			bool		nullsAtt[Natts_pg_attribute] = {0};
			bool		replacesAtt[Natts_pg_attribute] = {0};
			HeapTuple	newTup;

			missingval = array_get_element(missingval,
										   1,
										   &one,
										   0,
										   attTup->attlen,
										   attTup->attbyval,
										   attTup->attalign,
										   &isNull);
			missingval = PointerGetDatum(construct_array(&missingval,
														 1,
														 targettype,
														 tform->typlen,
														 tform->typbyval,
														 tform->typalign));

			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = false;

			newTup = heap_modify_tuple(heapTup, RelationGetDescr(attrelation),
									   valuesAtt, nullsAtt, replacesAtt);
			heap_freetuple(heapTup);
			heapTup = newTup;
			attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
		}
	}

	attTup->atttypid = targettype;
	attTup->atttypmod = targettypmod;
	attTup->attcollation = targetcollid;
	if (list_length(typeName->arrayBounds) > PG_INT16_MAX)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many array dimensions"));
	attTup->attndims = list_length(typeName->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;
	attTup->attcompression = InvalidCompressionMethod;

	ReleaseSysCache(typeTuple);

	CatalogTupleUpdate(attrelation, &heapTup->t_self, heapTup);

	table_close(attrelation, RowExclusiveLock);

	/* Inst所有 依赖 on 新 数据类型 and 排序规则 */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	/*
	 * Update the default, if present, by brute force --- remove and re-add the
	 * default. Probably unsafe to take shortcuts, since the new version may well
	 * have additional 依赖. (It's okay to do this now, rather than 之后 other ALTER
	 * TYPE commands, since the default won't depend on other column types.)
	 *
	 */
	if (defaultexpr)
	{
		/*
		 * If it's a GENERATED default, drop its 依赖 records, in particular its
		 * INTERNAL 依赖 on the column, which would 否则 cause 依赖.c to refuse to perform
		 * the deletion.
		 */
		if (attTup->attgenerated)
		{
			Oid			attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);

			if (!OidIsValid(attrdefoid))
				elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
					 RelationGetRelid(rel), attnum);
			(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);
		}

		/*
		 * Make 更新-so-far visible, particularly the new pg_attribute row which 将被
		 * updated again.
		 */
		CommandCounterIncrement();

		/*
		 * We use RESTRICT here for safety, but at present we do not expect
		 * anything to depend on the default.
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true,
						  true);

		StoreAttrDefault(rel, attnum, defaultexpr, true, false);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* Cleanup */
	heap_freetuple(heapTup);

	return address;
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a 副本标识 需要被 reset.
 *
 */
static void
RememberReplicaIdentityForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isreplident(indoid))
		return;

	if (tab->replicaIdentityIndex)
		elog(ERROR, "relation %u has multiple indexes marked as replica identity", tab->relid);

	tab->replicaIdentityIndex = get_rel_name(indoid);
}

/*
 * Subroutine for ATExecAlterColumnType: remember any clustered 索引.
 */
static void
RememberClusterOnForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isclustered(indoid))
		return;

	if (tab->clusterOnIndex)
		elog(ERROR, "relation %u has multiple clustered indexes", tab->relid);

	tab->clusterOnIndex = get_rel_name(indoid);
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a 约束 需要被 重建 (which we
 * might 已经 know).
 */
static void
RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: 我们必须n't
	 * try to recreate the same 约束 twice, and if a 约束 depends on more than one
	 * column whose type is to be altered, 我们必须 capture its definition string 之前
	 * applying any of the 列类型 changes. ruleutils.c will get confused if we ask
	 * again 稍后的.
	 */
	if (!list_member_oid(tab->changedConstraintOids, conoid))
	{
		/* OK, capture the 约束's existing definition string */
		char	   *defstring = pg_get_constraintdef_command(conoid);
		Oid			indoid;

		tab->changedConstraintOids = lappend_oid(tab->changedConstraintOids,
												 conoid);
		tab->changedConstraintDefs = lappend(tab->changedConstraintDefs,
											 defstring);

		/*
		 * For the 索引 of a 约束, 如有, remember if it 被使用 for the table's 副本标识 or if it
		 * is a clustered 索引, 从而 ATPostAlterTypeCleanup() can queue up commands
		 * necessary to restore those properties.
		 *
		 */
		indoid = get_constraint_index(conoid);
		if (OidIsValid(indoid))
		{
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}

/*
 * Subroutine for ATExecAlterColumnType: remember that an 索引 需要被 重建 (which we
 * might 已经 know).
 */
static void
RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: 我们必须n't
	 * try to recreate the same 索引 twice, and if an 索引 depends on more than one
	 * column whose type is to be altered, 我们必须 capture its definition string 之前
	 * applying any of the 列类型 changes. ruleutils.c will get confused if we ask
	 * again 稍后的.
	 */
	if (!list_member_oid(tab->changedIndexOids, indoid))
	{
		/*
		 * Before adding it as an 索引-to-重建, we'd better see if it belongs to a 约束,
		 * and 如果是这样 重建 the 约束 相反. Typically this check fails, 因为 约束 索引 normally
		 * have 仅 依赖 on their 约束. But it's possible for such an 索引 to 也 have direct
		 * 依赖 on table columns, 例如 with a partial 排他约束.
		 *
		 *
		 */
		Oid			conoid = get_index_constraint(indoid);

		if (OidIsValid(conoid))
		{
			RememberConstraintForRebuilding(conoid, tab);
		}
		else
		{
			/* OK, capture the 索引's existing definition string */
			char	   *defstring = pg_get_indexdef_string(indoid);

			tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
												indoid);
			tab->changedIndexDefs = lappend(tab->changedIndexDefs,
											defstring);

			/*
			 * Remember if this 索引 被使用 for the table's 副本标识 or if it is a clustered 索引,
			 * 从而 ATPostAlterTypeCleanup() can queue up commands necessary to restore
			 * those properties.
			 */
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a 统计信息 object 需要被 重建
 * (which we might 已经 know).
 */
static void
RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: 我们必须n't
	 * try to recreate the same 统计信息 object twice, and if the 统计信息 object depends
	 * on more than one column whose type is to be altered, 我们必须 capture its
	 * definition string 之前 applying any of the type changes. ruleutils.c will
	 * get confused if we ask again 稍后的.
	 */
	if (!list_member_oid(tab->changedStatisticsOids, stxoid))
	{
		/* OK, capture the 统计信息 对象's existing definition string */
		char	   *defstring = pg_get_statisticsobjdef_string(stxoid);

		tab->changedStatisticsOids = lappend_oid(tab->changedStatisticsOids,
												 stxoid);
		tab->changedStatisticsDefs = lappend(tab->changedStatisticsDefs,
											 defstring);
	}
}

/*
 * Cleanup 之后 we've finished all the ALTER TYPE operations for a particular
 * relation. We have to drop and recreate all the 索引 and 约束 that depend on the
 * altered columns. We do the actual dropping here, but re-creation is managed
 * by adding work queue entries to do those steps 稍后的.
 *
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab, LOCKMODE lockmode)
{
	ObjectAddress obj;
	ObjectAddresses *objects;
	ListCell   *def_item;
	ListCell   *oid_item;

	/*
	 * Collect all the 约束 and 索引 to drop so 我们可以 process them in a single call.
	 * That way 我们不 have to worry about 依赖 among them.
	 *
	 */
	objects = new_object_addresses();

	/*
	 * Re-解析 the 索引 and 约束 definitions, and attach them to the appropriate work
	 * queue entries. We do this 之前 dropping 因为 in the case of a 约束 on another
	 * table, we might not yet have 排他锁 on the table the 约束 is attached to, and
	 * 我们需要 get that 之前 reparsing/dropping. (That's possible 至少 for FOREIGN KEY,
	 * CHECK, and EXCLUSION 约束; in non-FK cases it 需要 a 依赖 on the target table's
	 * 复合类型 in the other table's 约束 expressions.) 我们可以't rely on the output of
	 * deparsing to tell us which relation to operate on, 因为 并发 activity might
	 * have made the name resolve differently. Instead, we've got to use the OID
	 * of the 约束 or 索引 we're processing to figure out which relation to operate
	 * on.
	 *
	 *
	 *
	 *
	 */
	forboth(oid_item, tab->changedConstraintOids,
			def_item, tab->changedConstraintDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		HeapTuple	tup;
		Form_pg_constraint con;
		Oid			relid;
		Oid			confrelid;
		bool		conislocal;

		tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
		if (!HeapTupleIsValid(tup)) /* 不应发生 */
			elog(ERROR, "cache lookup failed for constraint %u", oldId);
		con = (Form_pg_constraint) GETSTRUCT(tup);
		if (OidIsValid(con->conrelid))
			relid = con->conrelid;
		else
		{
			/* 必须是 a 执行main 约束 */
			relid = get_typ_typrelid(getBaseType(con->contypid));
			if (!OidIsValid(relid))
				elog(ERROR, "could not identify relation associated with constraint %u", oldId);
		}
		confrelid = con->confrelid;
		conislocal = con->conislocal;
		ReleaseSysCache(tup);

		ObjectAddressSet(obj, ConstraintRelationId, oldId);
		add_exact_object_address(&obj, objects);

		/*
		 * If the 约束 is 继承的 (仅), 我们不 want to inject a new definition here; it'll get
		 * recreated when ATAddCheckConstraint recurses from adding the 父表's 约束. But
		 * we had to carry the info this far 从而 我们可以 drop the 约束 以下.
		 *
		 */
		if (!conislocal)
			continue;

		/*
		 * When rebuilding another table's 约束 that references the table we're
		 * modifying, we might not yet have any 锁 on the other table, so get one
		 * now. We'll need AccessExclusiveLock for the DROP CONSTRAINT step, so
		 * there's no value in asking for anything weaker.
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, confrelid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);
	}
	forboth(oid_item, tab->changedIndexOids,
			def_item, tab->changedIndexDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = IndexGetRelation(oldId, false);

		/*
		 * As 以上, 确保 we have 锁 on the 索引's table if it's not the same table.
		 *
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, RelationRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/* 添加 依赖 for 新 统计信息 */
	forboth(oid_item, tab->changedStatisticsOids,
			def_item, tab->changedStatisticsDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = StatisticsGetRelation(oldId, false);

		/*
		 * 同上，确保在统计信息对象的表上持有锁，        
		 * 若它不是同一张表。但我们在此   
		 * 采用 ShareUpdateExclusiveLock，与 CreateStatistics、          
		 * RemoveStatisticsById 的锁级别一致。
		 *
		 * 注意：这应在所有获取                        
		 * AccessExclusiveLock 的情况后完成，否则可能因需要升
		 * 级表锁而死锁。     
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, ShareUpdateExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, StatisticExtRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/*
	 * Queue up command to restore 副本标识 索引 marking
	 */
	if (tab->replicaIdentityIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		ReplicaIdentityStmt *subcmd = makeNode(ReplicaIdentityStmt);

		subcmd->identity_type = REPLICA_IDENTITY_INDEX;
		subcmd->name = tab->replicaIdentityIndex;
		cmd->subtype = AT_ReplicaIdentity;
		cmd->def = (Node *) subcmd;

		/* 执行 it 之后 索引es and 约束s */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * Queue up command to restore marking of 索引 used for cluster.
	 */
	if (tab->clusterOnIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_ClusterOn;
		cmd->name = tab->clusterOnIndex;

		/* 执行 it 之后 索引es and 约束s */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * It 应是 okay to use DROP_RESTRICT here, since nothing else 应是 depending on
	 * these objects.
	 */
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	free_object_addresses(objects);

	/*
	 * The objects will get recreated 期间 subsequent passes over the work queue.
	 *
	 */
}

/*
 * 针对新建立的列类型，解析先前保存的约束、索引或  
 * 统计信息对象，并把生成的命令解析树排队等待         
 * 执行。                                               
 *
 * 这可能会失败，例如你的 WHERE 子句使用了新列类型  
 * 不可用的操作符。                              
 */
static void
ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId, char *cmd,
					 List **wqueue, LOCKMODE lockmode, bool rewrite)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;
	Relation	rel;

	/*
	 * We expect that we will get 仅 ALTER TABLE and CREATE INDEX statements.
	 * Hence, there is no need to pass them through parse_analyze_*() or the
	 * rewriter, but 相反 我们需要 pass them through parse_utilcmd.c to make them ready
	 * for 执行.
	 */
	raw_parsetree_list = raw_parser(cmd, RAW_PARSE_DEFAULT);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		RawStmt    *rs = lfirst_node(RawStmt, list_item);
		Node	   *stmt = rs->stmt;

		if (IsA(stmt, IndexStmt))
			querytree_list = lappend(querytree_list,
									 transformIndexStmt(oldRelId,
														(IndexStmt *) stmt,
														cmd));
		else if (IsA(stmt, AlterTableStmt))
		{
			List	   *beforeStmts;
			List	   *afterStmts;

			stmt = (Node *) transformAlterTableStmt(oldRelId,
													(AlterTableStmt *) stmt,
													cmd,
													&beforeStmts,
													&afterStmts);
			querytree_list = list_concat(querytree_list, beforeStmts);
			querytree_list = lappend(querytree_list, stmt);
			querytree_list = list_concat(querytree_list, afterStmts);
		}
		else if (IsA(stmt, CreateStatsStmt))
			querytree_list = lappend(querytree_list,
									 transformStatsStmt(oldRelId,
														(CreateStatsStmt *) stmt,
														cmd));
		else
			querytree_list = lappend(querytree_list, stmt);
	}

	/* C所有er should 已经 have acquired whatever 锁 we 需要. */
	rel = relation_open(oldRelId, NoLock);

	/*
	 * 把每个生成的命令附加到工作队列中合适的位置。  
	 * 注意，这可能导致创建全新的工作队列项。             
	 *
	 * 注意，我们还必须微调命令子类型，因为事实证明， 
	 * 索引和约束重建的行为必须与初始创建略有不    
	 * 同。                            
	 */
	foreach(list_item, querytree_list)
	{
		Node	   *stm = (Node *) lfirst(list_item);
		AlteredTableInfo *tab;

		tab = ATGetQueueEntry(wqueue, rel);

		if (IsA(stm, IndexStmt))
		{
			IndexStmt  *stmt = (IndexStmt *) stm;
			AlterTableCmd *newcmd;

			if (!rewrite)
				TryReuseIndex(oldId, stmt);
			stmt->reset_default_tblspc = true;
			/* 保持 the 索引's 注释 */
			stmt->idxcomment = GetComment(oldId, RelationRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddIndex;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_OLD_INDEX] =
				lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
		}
		else if (IsA(stm, AlterTableStmt))
		{
			AlterTableStmt *stmt = (AlterTableStmt *) stm;
			ListCell   *lcmd;

			foreach(lcmd, stmt->cmds)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lcmd);

				if (cmd->subtype == AT_AddIndex)
				{
					IndexStmt  *indstmt;
					Oid			indoid;

					indstmt = castNode(IndexStmt, cmd->def);
					indoid = get_constraint_index(oldId);

					if (!rewrite)
						TryReuseIndex(indoid, indstmt);
					/* 保持 任何 注释 on the 索引 */
					indstmt->idxcomment = GetComment(indoid,
													 RelationRelationId, 0);
					indstmt->reset_default_tblspc = true;

					cmd->subtype = AT_ReAddIndex;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);

					/* 重新创建约束上的所有注释 */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_INDEX,
											 oldId,
											 rel,
											 NIL,
											 indstmt->idxname);
				}
				else if (cmd->subtype == AT_AddConstraint)
				{
					Constraint *con = castNode(Constraint, cmd->def);

					con->old_pktable_oid = refRelId;
					/* rewriting neither s标识符e of a FK */
					if (con->contype == CONSTR_FOREIGN &&
						!rewrite && tab->rewrite == 0)
						TryReuseForeignKey(oldId, con);
					con->reset_default_tblspc = true;
					cmd->subtype = AT_ReAddConstraint;
					tab->subcmds[AT_PASS_OLD_CONSTR] =
						lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

					/* 重新创建约束上的所有注释 */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_CONSTR,
											 oldId,
											 rel,
											 NIL,
											 con->conname);
				}
				else if (cmd->subtype == AT_SetNotNull)
				{
					/*
					 * 解析器会为 PRIMARY KEY 索引/约束的列     
					 * 创建 AT_SetNotNull 子命令，但在这里无需    
					 * 对它们做任何处理，因为这些列的       
					 * NOT NULL 标记已经被传播到新的              
					 * 表定义中。          
					 */
				}
				else
					elog(ERROR, "unexpected statement subtype: %d",
						 (int) cmd->subtype);
			}
		}
		else if (IsA(stm, AlterDomainStmt))
		{
			AlterDomainStmt *stmt = (AlterDomainStmt *) stm;

			if (stmt->subtype == 'C')	/* ADD CONSTRAINT */
			{
				Constraint *con = castNode(Constraint, stmt->def);
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_ReAddDomainConstraint;
				cmd->def = (Node *) stmt;
				tab->subcmds[AT_PASS_OLD_CONSTR] =
					lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

				/* 重新创建约束上的所有注释 */
				RebuildConstraintComment(tab,
										 AT_PASS_OLD_CONSTR,
										 oldId,
										 NULL,
										 stmt->typeName,
										 con->conname);
			}
			else
				elog(ERROR, "unexpected statement subtype: %d",
					 (int) stmt->subtype);
		}
		else if (IsA(stm, CreateStatsStmt))
		{
			CreateStatsStmt *stmt = (CreateStatsStmt *) stm;
			AlterTableCmd *newcmd;

			/* 保持 the 统计信息 对象's 注释 */
			stmt->stxcomment = GetComment(oldId, StatisticExtRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddStatistics;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_MISC] =
				lappend(tab->subcmds[AT_PASS_MISC], newcmd);
		}
		else
			elog(ERROR, "unexpected statement type: %d",
				 (int) nodeTag(stm));
	}

	relation_close(rel, NoLock);
}

/*
 * ATPostAlterTypeParse() 的子例程，用于为正在重建的表     
 * 或域约束重建任何现有注释。                
 *
 * objid 是约束的 OID。          
 * 对于表约束传递 "rel"，或对于域约束传递 "domname"（域  
 * 的限定名，字符串列表形式）。
 * （我们本可以从 pg_constraint 条目中挖出这些信息以及    
 * conname；但调用者已有它们，所以不妨传它们。） 
 */
static void
RebuildConstraintComment(AlteredTableInfo *tab, int pass, Oid objid,
						 Relation rel, List *domname,
						 const char *conname)
{
	CommentStmt *cmd;
	char	   *comment_str;
	AlterTableCmd *newcmd;

	/* Look for 注释 for 对象 wanted, and leave if none */
	comment_str = GetComment(objid, ConstraintRelationId, 0);
	if (comment_str == NULL)
		return;

	/* Build CommentStmt node, 复制ing 所有 input 数据 for safety */
	cmd = makeNode(CommentStmt);
	if (rel)
	{
		cmd->objtype = OBJECT_TABCONSTRAINT;
		cmd->object = (Node *)
			list_make3(makeString(get_namespace_name(RelationGetNamespace(rel))),
					   makeString(pstrdup(RelationGetRelationName(rel))),
					   makeString(pstrdup(conname)));
	}
	else
	{
		cmd->objtype = OBJECT_DOMCONSTRAINT;
		cmd->object = (Node *)
			list_make2(makeTypeNameFromNameList(copyObject(domname)),
					   makeString(pstrdup(conname)));
	}
	cmd->comment = comment_str;

	/* Append it to 列表 of 命令s */
	newcmd = makeNode(AlterTableCmd);
	newcmd->subtype = AT_ReAddComment;
	newcmd->def = (Node *) cmd;
	tab->subcmds[pass] = lappend(tab->subcmds[pass], newcmd);
}

/*
 * Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 * for the real analysis, then mutates the IndexStmt based on that verdict.
 */
static void
TryReuseIndex(Oid oldId, IndexStmt *stmt)
{
	if (CheckIndexCompatible(oldId,
							 stmt->accessMethod,
							 stmt->indexParams,
							 stmt->excludeOpNames))
	{
		Relation	irel = index_open(oldId, NoLock);

		/* If it's a 分区ed 索引, t此处 is no 存储 to share. */
		if (irel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			stmt->oldNumber = irel->rd_locator.relNumber;
			stmt->oldCreateSubid = irel->rd_createSubid;
			stmt->oldFirstRelfilelocatorSubid = irel->rd_firstRelfilelocatorSubid;
		}
		index_close(irel, NoLock);
	}
}

/*
 * ATPostAlterTypeParse() 的子例程   
 *
 * 把旧的 P-F 相等操作符存入 Constraint 节点，供               
 * ATAddForeignKeyConstraint() 用于判断是否可以跳过此约         
 * 束的重新验证。          
 */
static void
TryReuseForeignKey(Oid oldId, Constraint *con)
{
	HeapTuple	tup;
	Datum		adatum;
	ArrayType  *arr;
	Oid		   *rawarr;
	int			numkeys;
	int			i;

	Assert(con->contype == CONSTR_FOREIGN);
	Assert(con->old_conpfeqop == NIL);	/* 已经 prepared this node */

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for constraint %u", oldId);

	adatum = SysCacheGetAttrNotNull(CONSTROID, tup,
									Anum_pg_constraint_conpfeqop);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	/* test follows the one in ri_FetchConstraintInfo() */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	rawarr = (Oid *) ARR_DATA_PTR(arr);

	/* stash a List of the operator O标识符 in our Constraint node */
	for (i = 0; i < numkeys; i++)
		con->old_conpfeqop = lappend_oid(con->old_conpfeqop, rawarr[i]);

	ReleaseSysCache(tup);
}

/*
 * ALTER COLUMN .. OPTIONS ( ... )
 *
 * Returns the address of the modified column
 */
static ObjectAddress
ATExecAlterColumnGenericOptions(Relation rel,
								const char *colName,
								List *options,
								LOCKMODE lockmode)
{
	Relation	ftrel;
	Relation	attrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Datum		datum;
	Form_pg_foreign_table fttableform;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	ObjectAddress address;

	if (options == NIL)
		return InvalidObjectAddress;

	/* First, determine FDW val标识符ator associated to the foreign 表. */
	ftrel = table_open(ForeignTableRelationId, AccessShareLock);
	tuple = SearchSysCache1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	fttableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(fttableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	table_close(ftrel, AccessShareLock);
	ReleaseSysCache(tuple);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* 防止它们修改系统属性 */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName)));


	/* Initialize buffers for 新 元组 值s */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the 当前 选项 */
	datum = SysCacheGetAttr(ATTNAME,
							tuple,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the 选项 */
	datum = transformGenericOptions(AttributeRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
	else
		repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;

	repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

	/* Everything 查找s good - 更新 the 元组 */

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrel),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(attrel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  atttableform->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	ReleaseSysCache(tuple);

	table_close(attrel, RowExclusiveLock);

	heap_freetuple(newtuple);

	return address;
}

/*
 * ALTER TABLE OWNER
 *
 * recursing 为 true 表示从表递归到其索引、序列或     
 * TOAST 表。不允许这些东西的所有权与父表分开改变。      
 * 此外，我们可以跳过权限检查（这不仅是优化，而且是  
 * 必需的），否则我们将无法正确处理                     
 * TOAST 表。                  
 *
 * recursing 在 ALTER TYPE OWNER 调我们去修复一个独立       
 * 复合类型时也为 true。
 */
void
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * Get 排他锁 till end of 事务 on the target table. Use relation_open 从而 我们可以 work
	 * on 索引 and 序列.
	 */
	target_rel = relation_open(relationOid, lockmode);

	/* Get its pg_class 元组, too */
	class_rel = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we 变更 the 属主ship of this 元组? */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
			/* ok to 变更 属主 */
			break;
		case RELKIND_INDEX:
			if (!recursing)
			{
				/*
				 * Because ALTER INDEX OWNER used to be allowed, and in fact is generated
				 * by old versions of pg_dump, we give a warning and do nothing rather
				 * than erroring out. Also, to avoid unnecessary chatter while restoring
				 * those old dumps, say nothing at all if the command 将会 a no-op anyway.
				 *
				 */
				if (tuple_class->relowner != newOwnerId)
					ereport(WARNING,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot change owner of index \"%s\"",
									NameStr(tuple_class->relname)),
							 errhint("Change the ownership of the index's table instead.")));
				/* quick hack to exit via the no-op path */
				newOwnerId = tuple_class->relowner;
			}
			break;
		case RELKIND_PARTITIONED_INDEX:
			if (recursing)
				break;
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change owner of index \"%s\"",
							NameStr(tuple_class->relname)),
					 errhint("Change the ownership of the index's table instead.")));
			break;
		case RELKIND_SEQUENCE:
			if (!recursing &&
				tuple_class->relowner != newOwnerId)
			{
				/* if it's an owned 序列, dis所有ow changing it by itself */
				Oid			tableId;
				int32		colId;

				if (sequenceIsOwned(relationOid, DEPENDENCY_AUTO, &tableId, &colId) ||
					sequenceIsOwned(relationOid, DEPENDENCY_INTERNAL, &tableId, &colId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot change owner of sequence \"%s\"",
									NameStr(tuple_class->relname)),
							 errdetail("Sequence \"%s\" is linked to table \"%s\".",
									   NameStr(tuple_class->relname),
									   get_rel_name(tableId))));
			}
			break;
		case RELKIND_COMPOSITE_TYPE:
			if (recursing)
				break;
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							NameStr(tuple_class->relname)),
			/* translator: %s is an SQL ALTER command */
					 errhint("Use %s instead.",
							 "ALTER TYPE")));
			break;
		case RELKIND_TOASTVALUE:
			if (recursing)
				break;
			/* FALL THRU */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change owner of relation \"%s\"",
							NameStr(tuple_class->relname)),
					 errdetail_relkind_not_supported(tuple_class->relkind)));
	}

	/*
	 * If the new 属主 is the same as the existing 属主, consider the command to have
	 * succeeded. This is for dump restoration purposes.
	 */
	if (tuple_class->relowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_class];
		bool		repl_null[Natts_pg_class];
		bool		repl_repl[Natts_pg_class];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* 跳过 权限 检查s 当 递归中 to 索引 or toast 表 */
		if (!recursing)
		{
			/* Super用户s can 始终 执行 it */
			if (!superuser())
			{
				Oid			namespaceOid = tuple_class->relnamespace;
				AclResult	aclresult;

				/* Otherwise, 必须是 属主 of the existing 对象 */
				if (!object_ownercheck(RelationRelationId, relationOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relationOid)),
								   RelationGetRelationName(target_rel));

				/* Must be able to become 新 属主 */
				check_can_set_role(GetUserId(), newOwnerId);

				/* New 属主 must have CREATE 权限 on 名称space */
				aclresult = object_aclcheck(NamespaceRelationId, namespaceOid, newOwnerId,
											ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_SCHEMA,
								   get_namespace_name(namespaceOid));
			}
		}

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_class_relowner - 1] = true;
		repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new 属主. This is 仅 necessary when the
		 * ACL is non-null.
		 */
		aclDatum = SysCacheGetAttr(RELOID, tuple,
								   Anum_pg_class_relacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 tuple_class->relowner, newOwnerId);
			repl_repl[Anum_pg_class_relacl - 1] = true;
			repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(class_rel, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);

		/*
		 * 我们必须 similarly update any per-column ACLs to reflect the new 属主; for
		 * neatness reasons that's split out as a subroutine.
		 */
		change_owner_fix_column_acls(relationOid,
									 tuple_class->relowner,
									 newOwnerId);

		/*
		 * Update 属主 依赖 reference, 如有. A 复合类型 has none, 因为 it's tracked for the
		 * pg_type entry 相反 of here; 索引 and TOAST tables don't have their own
		 * entries either.
		 */
		if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE &&
			tuple_class->relkind != RELKIND_INDEX &&
			tuple_class->relkind != RELKIND_PARTITIONED_INDEX &&
			tuple_class->relkind != RELKIND_TOASTVALUE)
			changeDependencyOnOwner(RelationRelationId, relationOid,
									newOwnerId);

		/*
		 * Also change the ownership of the table's 行类型, if it has one
		 */
		if (OidIsValid(tuple_class->reltype))
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId);

		/*
		 * If we are operating on a table or 物化视图, 也 change the ownership of any 索引
		 * and 序列 that belong to the relation, 同时 as its TOAST 表 (if it has one).
		 *
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_PARTITIONED_TABLE ||
			tuple_class->relkind == RELKIND_MATVIEW ||
			tuple_class->relkind == RELKIND_TOASTVALUE)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* Find 所有 the 索引es belonging to this 关系 */
			index_oid_list = RelationGetIndexList(target_rel);

			/* For 每 索引, recursively 变更 its 属主ship */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true, lockmode);

			list_free(index_oid_list);
		}

		/* If it has a toast 表, 递归 to 变更 its 属主ship */
		if (tuple_class->reltoastrelid != InvalidOid)
			ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
							  true, lockmode);

		/* If it has dependent 序列s, 递归 to 变更 them too */
		change_owner_recurse_to_sequences(relationOid, newOwnerId, lockmode);
	}

	InvokeObjectPostAlterHook(RelationRelationId, relationOid, 0);

	ReleaseSysCache(tuple);
	table_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);
}

/*
 * change_owner_fix_column_acls Helper function for ATExecChangeOwner. Scan
 * the columns of the table and fix any non-null column ACLs to reflect the
 * new 属主.
 *
 */
static void
change_owner_fix_column_acls(Oid relationOid, Oid oldOwnerId, Oid newOwnerId)
{
	Relation	attRelation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	attributeTuple;

	attRelation = table_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	scan = systable_beginscan(attRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);
		Datum		repl_val[Natts_pg_attribute];
		bool		repl_null[Natts_pg_attribute];
		bool		repl_repl[Natts_pg_attribute];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Ignore 删除ped 列 */
		if (att->attisdropped)
			continue;

		aclDatum = heap_getattr(attributeTuple,
								Anum_pg_attribute_attacl,
								RelationGetDescr(attRelation),
								&isNull);
		/* Null ACLs 执行 not require 变更s */
		if (isNull)
			continue;

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 oldOwnerId, newOwnerId);
		repl_repl[Anum_pg_attribute_attacl - 1] = true;
		repl_val[Anum_pg_attribute_attacl - 1] = PointerGetDatum(newAcl);

		newtuple = heap_modify_tuple(attributeTuple,
									 RelationGetDescr(attRelation),
									 repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(attRelation, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);
	}
	systable_endscan(scan);
	table_close(attRelation, RowExclusiveLock);
}

/*
 * change_owner_recurse_to_sequences
 *
 * ATExecChangeOwner 的辅助函数。它会检查 pg_depend 来      
 * 查找依赖于 serial 列的序列，并改变它们的             
 *           
 */
static void
change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId, LOCKMODE lockmode)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL 序列 are those having an auto 依赖 on one of the table's columns (我们不
	 * care *which* column, exactly).
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	/* we leave refobjsub标识符 unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* 跳过 依赖 other than auto 依赖 on 列 */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* Use 关系_打开 仅 以防 it's an 索引 */
		seqRel = relation_open(depForm->objid, lockmode);

		/* 跳过 non-序列 关系s */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No 需要 to 保持 the 锁 */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* We 执行n't 需要 to 关闭 the 序列 当 we 修改 it. */
		ATExecChangeOwner(depForm->objid, newOwnerId, true, lockmode);

		/* Now we can 关闭 it.  Keep the 锁 till end of 事务. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * 我们唯一要做的就是改变 indisclustered 位。           
 *
 * 返回新的集簇索引的地址。           
 */
static ObjectAddress
ATExecClusterOn(Relation rel, const char *indexName, LOCKMODE lockmode)
{
	Oid			indexOid;
	ObjectAddress address;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel))));

	/* Check 索引 is val标识符 to cluster on */
	check_index_is_clusterable(rel, indexOid, lockmode);

	/* And 执行 the 工作 */
	mark_index_clustered(rel, indexOid, false);

	ObjectAddressSet(address,
					 RelationRelationId, indexOid);

	return address;
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * 我们必须找出表上设置了 indisclustered 位的索引，并  
 * 将其关闭。     
 */
static void
ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
	mark_index_clustered(rel, InvalidOid, false);
}

/*
 * Preparation phase for SET ACCESS METHOD 检查 访问方法 exists. If it is the same
 * as the table's current 访问方法, it is a no-op. Otherwise, a table 重写 is
 * necessary.
 *
 */
static void
ATPrepSetAccessMethod(AlteredTableInfo *tab, Relation rel, const char *amname)
{
	Oid			amoid;

	/* Check that the 表 access method exists */
	amoid = get_table_am_oid(amname, false);

	if (rel->rd_rel->relam == amoid)
		return;

	/* Save info for 阶段 3 to 执行 the real 工作 */
	tab->rewrite |= AT_REWRITE_ACCESS_METHOD;
	tab->newAccessMethod = amoid;
}

/*
 * ALTER TABLE SET TABLESPACE
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, const char *tablespacename, LOCKMODE lockmode)
{
	Oid			tablespaceId;

	/* Check that the 表space exists */
	tablespaceId = get_tablespace_oid(tablespacename, false);

	/* Check 权限s except 当 moving to 数据base's 默认值 */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE, tablespacename);
	}

	/* Save info for 阶段 3 to 执行 the real 工作 */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));

	tab->newTableSpace = tablespaceId;
}

/*
 * Set, reset, or replace reloptions.
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, AlterTableType operation,
					LOCKMODE lockmode)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	bool		isnull;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

	if (defList == NIL && operation != AT_ReplaceRelOptions)
		return;					/* 无需操作 */

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	/* Fetch 堆 元组 */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCacheLocked1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	if (operation == AT_ReplaceRelOptions)
	{
		/*
		 * If we're supposed to replace the reloptions list, we just pretend there
		 * were none 之前.
		 */
		datum = (Datum) 0;
		isnull = true;
	}
	else
	{
		/* Get the 旧 rel选项 */
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								&isnull);
	}

	/* Generate 新 proposed rel选项 (text array) */
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, NULL, validnsps, false,
									 operation == AT_ResetRelOptions);

	/* Val标识符ate */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_PARTITIONED_TABLE:
			(void) partitioned_table_reloptions(newOptions, true);
			break;
		case RELKIND_VIEW:
			(void) view_reloptions(newOptions, true);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			(void) index_reloptions(rel->rd_indam->amoptions, newOptions, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot set options for relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
			break;
	}

	/* Special-case 验证 of 视图 选项 */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *view_query = get_view_query(rel);
		List	   *view_options = untransformRelOptions(newOptions);
		ListCell   *cell;
		bool		check_option = false;

		foreach(cell, view_options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (strcmp(defel->defname, "check_option") == 0)
				check_option = true;
		}

		/*
		 * If the check option is specified, look to see if the view is actually
		 * auto-updatable 与否.
		 */
		if (check_option)
		{
			const char *view_updatable_error =
				view_query_is_auto_updatable(view_query, true);

			if (view_updatable_error)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
						 errhint("%s", _(view_updatable_error))));
		}
	}

	/*
	 * All we need do here is update the pg_class row; the new options 将被
	 * propagated into relcaches 期间 post-提交 cache inval.
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;

	repl_repl[Anum_pg_class_reloptions - 1] = true;

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);
	UnlockTuple(pgclass, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	/* repeat the whole exercise for the toast 表, if t此处's one */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;
		Oid			toastid = rel->rd_rel->reltoastrelid;

		toastrel = table_open(toastid, lockmode);

		/* Fetch 堆 元组 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);

		if (operation == AT_ReplaceRelOptions)
		{
			/*
			 * If we're supposed to replace the reloptions list, we just pretend there
			 * were none 之前.
			 */
			datum = (Datum) 0;
			isnull = true;
		}
		else
		{
			/* Get the 旧 rel选项 */
			datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									&isnull);
		}

		newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
										 defList, "toast", validnsps, false,
										 operation == AT_ResetRelOptions);

		(void) heap_reloptions(RELKIND_TOASTVALUE, newOptions, true);

		memset(repl_val, 0, sizeof(repl_val));
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		if (newOptions != (Datum) 0)
			repl_val[Anum_pg_class_reloptions - 1] = newOptions;
		else
			repl_null[Anum_pg_class_reloptions - 1] = true;

		repl_repl[Anum_pg_class_reloptions - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
									 repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);

		InvokeObjectPostAlterHookArg(RelationRelationId,
									 RelationGetRelid(toastrel), 0,
									 InvalidOid, true);

		heap_freetuple(newtuple);

		ReleaseSysCache(tuple);

		table_close(toastrel, NoLock);
	}

	table_close(pgclass, RowExclusiveLock);
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no 元组 rewriting
 * to be done, so we just want to copy the data as fast as possible.
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode)
{
	Relation	rel;
	Oid			reltoastrelid;
	RelFileNumber newrelfilenumber;
	RelFileLocator newrlocator;
	List	   *reltoastidxids = NIL;
	ListCell   *lc;

	/*
	 * Need 锁 here in case we are 递归中 to TOAST 表 or 索引
	 */
	rel = relation_open(tableOid, lockmode);

	/* Check 第一 if 关系 可以是 moved to 新 表space */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel), 0);
		relation_close(rel, NoLock);
		return;
	}

	reltoastrelid = rel->rd_rel->reltoastrelid;
	/* Fetch the 列表 of 索引es on toast 关系 if necessary */
	if (OidIsValid(reltoastrelid))
	{
		Relation	toastRel = relation_open(reltoastrelid, lockmode);

		reltoastidxids = RelationGetIndexList(toastRel);
		relation_close(toastRel, lockmode);
	}

	/*
	 * Relfilenumbers are not unique in databases across 表空间, so 我们需要 allocate a
	 * new one in the new 表空间.
	 */
	newrelfilenumber = GetNewRelFileNumber(newTableSpace, NULL,
										   rel->rd_rel->relpersistence);

	/* Open 旧 and 新 关系 */
	newrlocator = rel->rd_locator;
	newrlocator.relNumber = newrelfilenumber;
	newrlocator.spcOid = newTableSpace;

	/* hand off to AM to actu所有y 创建 新 rel 存储 and 复制 the 数据 */
	if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		index_copy_data(rel, newrlocator);
	}
	else
	{
		Assert(RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind));
		table_relation_copy_data(rel, &newrlocator);
	}

	/*
	 * 更新 pg_class 行。  
	 *
	 * 注意：若允许 ATExecSetTableSpace() 在 pg_class 或其索   
	 * 引上执行（上面的副本将不含更新 pg_class 条目）， 
	 * 但被以下函数所禁止：                        
	 * CheckRelationTableSpaceMove() 
	 */
	SetRelationTableSpace(rel, newTableSpace, newrelfilenumber);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	RelationAssumeNewRelfilelocator(rel);

	relation_close(rel, NoLock);

	/* Make sure the rel表space 变更 is 可见 */
	CommandCounterIncrement();

	/* Move associated toast 关系 and/or 索引es, too */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
	foreach(lc, reltoastidxids)
		ATExecSetTableSpace(lfirst_oid(lc), newTableSpace, lockmode);

	/* Clean up */
	list_free(reltoastidxids);
}

/*
 * 对没有存储但希望保留表空间的关系，为 ALTER TABLE  
 * SET TABLESPACE 做特殊处理。                      
 *
 * 由于这些关系没有存储，表空间可以通过一个简单的  
 * 仅元数据操作来更新。                   
 */
static void
ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace)
{
	/*
	 * Shouldn't be called on relations having 存储; these are processed in phase
	 * 3.
	 */
	Assert(!RELKIND_HAS_STORAGE(rel->rd_rel->relkind));

	/* 检查 if 关系 可以是 moved to its 新 表space */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  0);
		return;
	}

	/* Update 可以是 执行ne, so 变更 rel表space */
	SetRelationTableSpace(rel, newTableSpace, InvalidOid);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	/* Make sure the rel表space 变更 is 可见 */
	CommandCounterIncrement();
}

/*
 * Alter Table ALL ... SET TABLESPACE Allows a 用户 to move all objects of some
 * type in a given 表空间 in the current database to another 表空间. Objects can be
 * chosen based on the 属主 of the object 也, to allow 用户 to move 仅 their
 * objects. The 用户 must have CREATE rights on the new 表空间, as usual. The main
 * 权限 handling is done by the lower-level table move function. All to-be-moved
 * objects are locked first. If NOWAIT is specified and the 锁 can't be
 * acquired then we ereport(ERROR).
 *
 *
 *
 */
Oid
AlterTableMoveAll(AlterTableMoveAllStmt *stmt)
{
	List	   *relations = NIL;
	ListCell   *l;
	ScanKeyData key[1];
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	Oid			orig_tablespaceoid;
	Oid			new_tablespaceoid;
	List	   *role_oids = roleSpecsToIds(stmt->roles);

	/* Ensure we were not asked to move 某些thing we can't */
	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_INDEX &&
		stmt->objtype != OBJECT_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only tables, indexes, and materialized views exist in tablespaces")));

	/* Get the orig and 新 表space OIDs */
	orig_tablespaceoid = get_tablespace_oid(stmt->orig_tablespacename, false);
	new_tablespaceoid = get_tablespace_oid(stmt->new_tablespacename, false);

	/* Can't move 共享 关系s in to or out of pg_全局 */
	/* This is 也 检查ed by ATExecSetTableSpace, but nice to stop earlier */
	if (orig_tablespaceoid == GLOBALTABLESPACE_OID ||
		new_tablespaceoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global tablespace")));

	/*
	 * Must have CREATE rights on the new 表空间, unless it is the database default
	 * 表空间 (which all 用户 implicitly have CREATE rights on).
	 *
	 */
	if (OidIsValid(new_tablespaceoid) && new_tablespaceoid != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, new_tablespaceoid, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(new_tablespaceoid));
	}

	/*
	 * Now that the 检查 are done, check if we should set either to InvalidOid 因为
	 * it is our database's default 表空间.
	 */
	if (orig_tablespaceoid == MyDatabaseTableSpace)
		orig_tablespaceoid = InvalidOid;

	if (new_tablespaceoid == MyDatabaseTableSpace)
		new_tablespaceoid = InvalidOid;

	/* no-op */
	if (orig_tablespaceoid == new_tablespaceoid)
		return new_tablespaceoid;

	/*
	 * Walk the list of objects in the 表空间 and move them. This will 仅 find
	 * objects in our database, of course.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_reltablespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(orig_tablespaceoid));

	rel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relOid = relForm->oid;

		/*
		 * 不要此时移动 pg_catalog 对象；若管理员想这     
		 * 样做，他们可以直接发出单独的 ALTER           
		 * 命令。         
		 *
		 * 另外，明确避开共享表、临时表或 TOAST（TOAST    
		 *  会随主表一起移动）。           
		 */
		if (IsCatalogNamespace(relForm->relnamespace) ||
			relForm->relisshared ||
			isAnyTempNamespace(relForm->relnamespace) ||
			IsToastNamespace(relForm->relnamespace))
			continue;

		/* Only move the 对象 类型 requested */
		if ((stmt->objtype == OBJECT_TABLE &&
			 relForm->relkind != RELKIND_RELATION &&
			 relForm->relkind != RELKIND_PARTITIONED_TABLE) ||
			(stmt->objtype == OBJECT_INDEX &&
			 relForm->relkind != RELKIND_INDEX &&
			 relForm->relkind != RELKIND_PARTITIONED_INDEX) ||
			(stmt->objtype == OBJECT_MATVIEW &&
			 relForm->relkind != RELKIND_MATVIEW))
			continue;

		/* Check if we are 仅 moving 对象s owned by certain roles */
		if (role_oids != NIL && !list_member_oid(role_oids, relForm->relowner))
			continue;

		/*
		 * Handle 权限-checking here since we are locking the tables and 也 to avoid
		 * doing a bunch of work 仅 to fail part-way. 请注意 权限 will 也 be checked by
		 * AlterTableInternal(). Caller 必须是 considered an 属主 on the table to move
		 * it.
		 *
		 */
		if (!object_ownercheck(RelationRelationId, relOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relOid)),
						   NameStr(relForm->relname));

		if (stmt->nowait &&
			!ConditionalLockRelationOid(relOid, AccessExclusiveLock))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("aborting because lock on relation \"%s.%s\" is not available",
							get_namespace_name(relForm->relnamespace),
							NameStr(relForm->relname))));
		else
			LockRelationOid(relOid, AccessExclusiveLock);

		/* Add to our 列表 of 对象s to move */
		relations = lappend_oid(relations, relOid);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	if (relations == NIL)
		ereport(NOTICE,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("no matching relations in tablespace \"%s\" found",
						orig_tablespaceoid == InvalidOid ? "(database default)" :
						get_tablespace_name(orig_tablespaceoid))));

	/* Everything is 锁ed, loop through and move 所有 of the 关系s. */
	foreach(l, relations)
	{
		List	   *cmds = NIL;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_SetTableSpace;
		cmd->name = stmt->new_tablespacename;

		cmds = lappend(cmds, cmd);

		EventTriggerAlterTableStart((Node *) stmt);
		/* OID is 设置 by AlterTableInternal */
		AlterTableInternal(lfirst_oid(l), cmds, false);
		EventTriggerAlterTableEnd();
	}

	return new_tablespaceoid;
}

static void
index_copy_data(Relation rel, RelFileLocator newrlocator)
{
	SMgrRelation dstrel;

	dstrel = smgropen(newrlocator, rel->rd_backend);

	/*
	 * Since we copy the file directly 没有 looking at the shared 缓冲区, we'd better
	 * first flush out any pages of the source relation that are in shared 缓冲区.
	 * We assume no new changes 将被 made while we are holding 排他锁 on the rel.
	 *
	 */
	FlushRelationBuffers(rel);

	/*
	 * 创建并复制关系的所有分叉，并安排解除旧物理     
	 * 文件的链接。 
	 *
	 * 注意：relfilenumber 值的任何冲突都会在         
	 * RelationCreateStorage() 
	 */
	RelationCreateStorage(newrlocator, rel->rd_rel->relpersistence, true);

	/* 复制 main fork */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* 复制 those 额外 forks that exist */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(&newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* 删除 旧 关系, and 关闭 新 one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER We just pass this off to 触发器.c.
 *
 *
 */
static void
ATExecEnableDisableTrigger(Relation rel, const char *trigname,
						   char fires_when, bool skip_system, bool recurse,
						   LOCKMODE lockmode)
{
	EnableDisableTrigger(rel, trigname, InvalidOid,
						 fires_when, skip_system, recurse,
						 lockmode);
}

/*
 * ALTER TABLE ENABLE/DISABLE RULE
 *
 * We just pass this off to rewriteDefine.c.
 */
static void
ATExecEnableDisableRule(Relation rel, const char *rulename,
						char fires_when, LOCKMODE lockmode)
{
	EnableDisableRule(rel, rulename, fires_when);
}

/*
 * ALTER TABLE INHERIT
 *
 * 把一个父表加到子表的父表列表中。这验证父表的所有   
 * 列和检查约束都出现在子表中，并且它们具有相同的数  
 * 据类型和表达式。        
 */
static void
ATPrepAddInherit(Relation child_rel)
{
	if (child_rel->rd_rel->reloftype)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of typed table")));

	if (child_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	if (child_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of partitioned table")));
}

/*
 * Return the address of the new 父关系.
 */
static ObjectAddress
ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel;
	List	   *children;
	ObjectAddress address;
	const char *trigger_name;

	/*
	 * A self-排他锁 是必需的 here. See the similar case in MergeAttributes() for a full
	 * explanation.
	 */
	parent_rel = table_openrv(parent, ShareUpdateExclusiveLock);

	/*
	 * Must be 属主 of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(AT_AddInherit, parent_rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/* 永久关系不能从临时关系继承 */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/* If 父 rel is temp, it 必须是long to this 会话 */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!parent_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation of another session")));

	/* Ditto for the 子 */
	if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!child_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit to temporary relation of another session")));

	/* Prevent 分区ed 表s from becoming 继承ance 父s */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from partitioned table \"%s\"",
						parent->relname)));

	/* Likewise for 分区s */
	if (parent_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from a partition")));

	/*
	 * Prevent circularity by seeing if proposed parent inherits from child. (In
	 * particular, this dis允许 making a rel 继承 from itself.) This is not
	 * completely bulletproof 因为 of race conditions: in multi-level 继承 trees,
	 * someone else could con当前 be making another 继承 link that 关闭 the loop but
	 * does not join either of the rels we have locked. Preventing that seems to
	 * require exclusive 锁 on the entire 继承 tree, which is a cure worse than the
	 * disease. find_all_inheritors() will cope with circularity anyway, so don't
	 * sweat it too much. We use weakest 锁 我们可以 on child's children, namely
	 * AccessShareLock.
	 *
	 *
	 *
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel),
								   AccessShareLock, NULL);

	if (list_member_oid(children, RelationGetRelid(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   parent->relname,
						   RelationGetRelationName(child_rel))));

	/*
	 * If child_rel has 行级触发器s with transition tables, we 当前 don't allow it to
	 * become an 继承 child. See 也 prohibitions in ATExecAttachPartition() and
	 * CreateTrigger().
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(child_rel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming an inheritance child",
						trigger_name, RelationGetRelationName(child_rel)),
				 errdetail("ROW triggers with transition tables are not supported in inheritance hierarchies.")));

	/* OK to 创建 继承ance */
	CreateInheritance(child_rel, parent_rel);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* 保持 our 锁 on the 父 关系 直到 提交 */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * CreateInheritance
 *		在子表与父表之间创建继承的目录操作                 
 *		部分。                
 *
 * ATExecAddInherit() 和 ATExecAttachPartition() 共用。 
 */
static void
CreateInheritance(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;

	/* Note: 获取 RowExclusiveLock 因为 we will write pg_继承s below. */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates in the list of parents, and determine the highest
	 * inhseqno 已经 present; we'll use the next one for the new parent. Also, if
	 * proposed child is a partition, it cannot 已经 be 继承. Note: we do not reject
	 * the case where the child 已经 inherits from the parent indirectly; CREATE
	 * TABLE doesn't reject comparable cases.
	 *
	 *
	 */
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	/* inhseqno 序列s start at 1 */
	inhseqno = 0;
	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inheritsTuple);

		if (inh->inhparent == RelationGetRelid(parent_rel))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							RelationGetRelationName(parent_rel))));

		if (inh->inhseqno > inhseqno)
			inhseqno = inh->inhseqno;
	}
	systable_endscan(scan);

	/* Match up the 列 and bump attinhcount as 需要ed */
	MergeAttributesIntoExisting(child_rel, parent_rel);

	/* Match up the 约束s and bump coninhcount as 需要ed */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.  Make the 目录 entries that show 继承.
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation,
							 parent_rel->rd_rel->relkind ==
							 RELKIND_PARTITIONED_TABLE);

	/* Now we're 执行ne with pg_继承s */
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 * Obtain the source-text form of the 约束 expression for a CHECK 约束, given its
 * pg_constraint 元组
 */
static char *
decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
	Form_pg_constraint con;
	bool		isnull;
	Datum		attr;
	Datum		expr;

	con = (Form_pg_constraint) GETSTRUCT(contup);
	attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u", con->oid);

	expr = DirectFunctionCall2(pg_get_expr, attr,
							   ObjectIdGetDatum(con->conrelid));
	return TextDatumGetCString(expr);
}

/*
 * Determine whether two check 约束 are functionally equivalent The test we
 * apply is to see whether they reverse-compile to the same source string.
 * This insulates us from issues like whether 属性 have the same physical 列号 in
 * parent and child relations.
 *
 */
static bool
constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc)
{
	Form_pg_constraint acon = (Form_pg_constraint) GETSTRUCT(a);
	Form_pg_constraint bcon = (Form_pg_constraint) GETSTRUCT(b);

	if (acon->condeferrable != bcon->condeferrable ||
		acon->condeferred != bcon->condeferred ||
		strcmp(decompile_conbin(a, tupleDesc),
			   decompile_conbin(b, tupleDesc)) != 0)
		return false;
	else
		return true;
}

/*
 * 检查子表中的各列与父表中的各列是否匹配，并递增      
 * attinhcount。    
 *
 * CreateInheritance 调用。
 *
 * 当前，父表的所有列都必须能在子表中找到。缺少列      
 * 是一种错误。也许有一天我们可以考虑像 CREATE TABLE     
 * 那样创建新列。不过这很不得人心 --- 在分区表这种  
 * 常见场景中是个 foot-gun。  
 *
 * 数据类型必须完全一致。若父列是 NOT NULL，则           
 * 子列也必须如此。但默认值不作比较。           
 */
static void
MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	attrrel;
	AttrNumber	parent_attno;
	int			parent_natts;
	TupleDesc	tupleDesc;
	HeapTuple	tuple;
	bool		child_is_partition = false;

	attrrel = table_open(AttributeRelationId, RowExclusiveLock);

	tupleDesc = RelationGetDescr(parent_rel);
	parent_natts = tupleDesc->natts;

	/* 如果 parent_rel 是分区表，child_rel 必须是分区 */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore 删除ped 列 in the 父. */
		if (attribute->attisdropped)
			continue;

		/* Find 相同 列 in 子 (matching on 列 名称). */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel),
										  attributeName);
		if (HeapTupleIsValid(tuple))
		{
			/* Check they are 相同 类型, typmod, and 排序规则 */
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (attribute->atttypid != childatt->atttypid ||
				attribute->atttypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			if (attribute->attcollation != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			/*
			 * Check child doesn't discard NOT NULL property. (Other 约束 are checked
			 * elsewhere.)
			 */
			if (attribute->attnotnull && !childatt->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be marked NOT NULL",
								attributeName)));

			/*
			 * Child column 必须是 generated if and 仅 if parent column is.
			 */
			if (attribute->attgenerated && !childatt->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be a generated column",
								attributeName)));
			if (childatt->attgenerated && !attribute->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must not be a generated column",
								attributeName)));

			/*
			 * OK, bump the child column's 继承 count. (If we fail 稍后的 on, this change
			 * will just roll back.)
			 */
			childatt->attinhcount++;
			if (childatt->attinhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * In case of partitions, 我们必须 enforce that value of attislocal is same in
			 * all partitions. (Note: there are 仅 继承的 属性 in partitions)
			 *
			 */
			if (child_is_partition)
			{
				Assert(childatt->attinhcount == 1);
				childatt->attislocal = false;
			}

			CatalogTupleUpdate(attrrel, &tuple->t_self, tuple);
			heap_freetuple(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing column \"%s\"",
							attributeName)));
		}
	}

	table_close(attrrel, RowExclusiveLock);
}

/*
 * 检查子表中的约束与父表中的约束是否匹配，并递增
 * 它们的 coninhcount。        
 *
 * 仅在父表中标记为 ONLY 的约束会被忽略。     
 *
 * CreateInheritance 调用。
 *
 * 当前，父表的所有约束都必须存在于子表中。也许有一    
 * 天我们会考虑像 CREATE TABLE 那样添加新约束。
 *
 * XXX 此算法是 O(N^2)，对拥有数百个约束的表而言       
 * 可能是个问题。但只要表的约束数量在 10 个左右，         
 * 即使 100 个约束也不至于世界末日。                          
 *
 * XXX 若修改此代码，请参见 MergeWithExistingConstraint  
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	catalog_relation;
	TupleDesc	tuple_desc;
	SysScanDesc parent_scan;
	ScanKeyData parent_key;
	HeapTuple	parent_tuple;
	bool		child_is_partition = false;

	catalog_relation = table_open(ConstraintRelationId, RowExclusiveLock);
	tuple_desc = RelationGetDescr(catalog_relation);

	/* 如果 parent_rel 是分区表，child_rel 必须是分区 */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	/* Outer loop 扫描s through the 父's 约束 definitions */
	ScanKeyInit(&parent_key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	parent_scan = systable_beginscan(catalog_relation, ConstraintRelidTypidNameIndexId,
									 true, NULL, 1, &parent_key);

	while (HeapTupleIsValid(parent_tuple = systable_getnext(parent_scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(parent_tuple);
		SysScanDesc child_scan;
		ScanKeyData child_key;
		HeapTuple	child_tuple;
		bool		found = false;

		if (parent_con->contype != CONSTRAINT_CHECK)
			continue;

		/* if the 父's 约束 is 标记ed NO INHERIT, it's not 继承ed */
		if (parent_con->connoinherit)
			continue;

		/* Search for a 子 约束 matching this one */
		ScanKeyInit(&child_key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(child_rel)));
		child_scan = systable_beginscan(catalog_relation, ConstraintRelidTypidNameIndexId,
										true, NULL, 1, &child_key);

		while (HeapTupleIsValid(child_tuple = systable_getnext(child_scan)))
		{
			Form_pg_constraint child_con = (Form_pg_constraint) GETSTRUCT(child_tuple);
			HeapTuple	child_copy;

			if (child_con->contype != CONSTRAINT_CHECK)
				continue;

			if (strcmp(NameStr(parent_con->conname),
					   NameStr(child_con->conname)) != 0)
				continue;

			if (!constraints_equivalent(parent_tuple, child_tuple, tuple_desc))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different definition for check constraint \"%s\"",
								RelationGetRelationName(child_rel),
								NameStr(parent_con->conname))));

			/* If the 子 约束 is "no 继承" 然后 不能 merge */
			if (child_con->connoinherit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * If the child 约束 is "not valid" then cannot merge with a valid parent 约束
			 *
			 */
			if (parent_con->convalidated && !child_con->convalidated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * OK, bump the child 约束's 继承 count. (If we fail 稍后的 on, this change will
			 * just roll back.)
			 */
			child_copy = heap_copytuple(child_tuple);
			child_con = (Form_pg_constraint) GETSTRUCT(child_copy);
			child_con->coninhcount++;
			if (child_con->coninhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * In case of partitions, an 继承的 约束 必须是 继承的 仅 once since it cannot have
			 * multiple parents and it is 从不 considered local.
			 *
			 */
			if (child_is_partition)
			{
				Assert(child_con->coninhcount == 1);
				child_con->conislocal = false;
			}

			CatalogTupleUpdate(catalog_relation, &child_copy->t_self, child_copy);
			heap_freetuple(child_copy);

			found = true;
			break;
		}

		systable_endscan(child_scan);

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname))));
	}

	systable_endscan(parent_scan);
	table_close(catalog_relation, RowExclusiveLock);
}

/*
 * ALTER TABLE NO INHERIT
 *
 * Return value is the address of the relation that is no longer parent.
 */
static ObjectAddress
ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode)
{
	ObjectAddress address;
	Relation	parent_rel;

	if (rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	/*
	 * AccessShareLock on the parent is probably enough, seeing that DROP TABLE
	 * doesn't 锁 parent tables at all. We need some 锁 since we'll be inspecting
	 * the parent's 模式.
	 */
	parent_rel = table_openrv(parent, AccessShareLock);

	/*
	 * 我们不 bother to check ownership of the 父表 --- ownership of the child is
	 * presumed enough rights.
	 */

	/* Off to RemoveInheritance() w此处 most of the 工作 happens */
	RemoveInheritance(rel, parent_rel, false);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* 保持 our 锁 on the 父 关系 直到 提交 */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * MarkInheritDetached
 *
 * 为一个分区设置 inhdetachpending，用于                
 * ATExecDetachPartition 的并发模式。同时验证没有其他   
 * 分区等待分离。  
 */
static void
MarkInheritDetached(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	bool		found = false;

	Assert(parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Find pg_inherits entries by inhparent. (我们需要 扫描 them all 以便 verify that no
	 * other partition is pending detach.)
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, InheritsParentIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inhForm;

		inhForm = (Form_pg_inherits) GETSTRUCT(inheritsTuple);
		if (inhForm->inhdetachpending)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("partition \"%s\" already pending detach in partitioned table \"%s.%s\"",
						   get_rel_name(inhForm->inhrelid),
						   get_namespace_name(parent_rel->rd_rel->relnamespace),
						   RelationGetRelationName(parent_rel)),
					errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

		if (inhForm->inhrelid == RelationGetRelid(child_rel))
		{
			HeapTuple	newtup;

			newtup = heap_copytuple(inheritsTuple);
			((Form_pg_inherits) GETSTRUCT(newtup))->inhdetachpending = true;

			CatalogTupleUpdate(catalogRelation,
							   &inheritsTuple->t_self,
							   newtup);
			found = true;
			heap_freetuple(newtup);
			/* 保持 查找ing, to ensure we catch others pending detach */
		}
	}

	/* Done */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel))));
}

/*
 * RemoveInheritance Drop a parent from the child's parents. This just adjusts
 * the attinhcount and attislocal of the columns and re移动 the pg_inherit and
 * pg_depend entries. expect_detached is passed down to DeleteInheritsTuple,
 * q.v.. If attinhcount goes to 0 then attislocal gets set to true. If it goes
 * back up attislocal stays true, which means if a child is ever removed from
 * a parent then its columns will 从不 be automatically dropped which may
 * surprise. But 至少 we'll 从不 surprise by dropping columns someone isn't
 * expecting to be dropped which would actually mean data loss. coninhcount
 * and conislocal for 继承的 约束 are adjusted in exactly the same way. Common to
 * ATExecDropInherit() and ATExecDetachPartition().
 *
 *
 *
 *
 *
 *
 */
static void
RemoveInheritance(Relation child_rel, Relation parent_rel, bool expect_detached)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	attributeTuple,
				constraintTuple;
	List	   *connames;
	bool		found;
	bool		child_is_partition = false;

	/* 如果 parent_rel 是分区表，child_rel 必须是分区 */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	found = DeleteInheritsTuple(RelationGetRelid(child_rel),
								RelationGetRelid(parent_rel),
								expect_detached,
								RelationGetRelationName(child_rel));
	if (!found)
	{
		if (child_is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
							RelationGetRelationName(child_rel),
							RelationGetRelationName(parent_rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a parent of relation \"%s\"",
							RelationGetRelationName(parent_rel),
							RelationGetRelationName(child_rel))));
	}

	/*
	 * Search through child columns looking for ones matching parent rel
	 */
	catalogRelation = table_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);

		/* Ignore if 删除ped or not 继承ed */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* Decrement inhcount and possibly 设置 is本地 to 真 */
			HeapTuple	copyTuple = heap_copytuple(attributeTuple);
			Form_pg_attribute copy_att = (Form_pg_attribute) GETSTRUCT(copyTuple);

			copy_att->attinhcount--;
			if (copy_att->attinhcount == 0)
				copy_att->attislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	/*
	 * Likewise, find 继承的 check 约束 and disinherit them. To do this, we first need
	 * a list of the names of the parent's check 约束. (We cheat a bit by 仅
	 * checking for name matches, assuming that the expressions will match.)
	 *
	 */
	catalogRelation = table_open(ConstraintRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	connames = NIL;

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->contype == CONSTRAINT_CHECK)
			connames = lappend(connames, pstrdup(NameStr(con->conname)));
	}

	systable_endscan(scan);

	/* Now 扫描 the 子's 约束s */
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		match;
		ListCell   *lc;

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		match = false;
		foreach(lc, connames)
		{
			if (strcmp(NameStr(con->conname), (char *) lfirst(lc)) == 0)
			{
				match = true;
				break;
			}
		}

		if (match)
		{
			/* Decrement inhcount and possibly 设置 is本地 to 真 */
			HeapTuple	copyTuple = heap_copytuple(constraintTuple);
			Form_pg_constraint copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

			if (copy_con->coninhcount <= 0) /* 不应发生 */
				elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
					 RelationGetRelid(child_rel), NameStr(copy_con->conname));

			copy_con->coninhcount--;
			if (copy_con->coninhcount == 0)
				copy_con->conislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	drop_parent_dependency(RelationGetRelid(child_rel),
						   RelationRelationId,
						   RelationGetRelid(parent_rel),
						   child_dependency_type(child_is_partition));

	/*
	 * Post alter hook of this inherits. Since object_access_hook doesn't take
	 * multiple object identifiers, we relay oid of 父关系 using auxiliary_id
	 * argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 RelationGetRelid(child_rel), 0,
								 RelationGetRelid(parent_rel), false);
}

/*
 * Drop the 依赖 created by StoreCatalogInheritance1 (CREATE TABLE
 * INHERITS/ALTER TABLE INHERIT -- refclassid 将被 RelationRelationId) or
 * heap_create_with_catalog (CREATE TABLE OF/ALTER TABLE OF -- refclassid 将被
 * TypeRelationId). There's no convenient way to do this, so go trawling
 * through pg_depend.
 */
static void
drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
					   DependencyType deptype)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	depTuple;

	catalogRelation = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == refclassid &&
			dep->refobjid == refobjid &&
			dep->refobjsubid == 0 &&
			dep->deptype == deptype)
			CatalogTupleDelete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 * ALTER TABLE OF Attach a table to a 复合类型, as though it had been created with
 * CREATE TABLE OF. All attname, atttypid, atttypmod and attcollation must
 * match. The subject table must not have 继承 parents. These restrictions
 * ensure that you cannot create a configuration impossible with CREATE TABLE
 * OF alone. The address of the type is returned.
 *
 *
 *
 */
static ObjectAddress
ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Type		typetuple;
	Form_pg_type typeform;
	Oid			typeid;
	Relation	inheritsRelation,
				relationRelation;
	SysScanDesc scan;
	ScanKeyData key;
	AttrNumber	table_attno,
				type_attno;
	TupleDesc	typeTupleDesc,
				tableTupleDesc;
	ObjectAddress tableobj,
				typeobj;
	HeapTuple	classtuple;

	/* Val标识符ate the 类型. */
	typetuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(typetuple);
	typeform = (Form_pg_type) GETSTRUCT(typetuple);
	typeid = typeform->oid;

	/* Fail if the 表 has 任何 继承ance 父s. */
	inheritsRelation = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inheritsRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("typed tables cannot inherit")));
	systable_endscan(scan);
	table_close(inheritsRelation, AccessShareLock);

	/*
	 * Check the 元组 descriptors for compatibility. Unlike 继承, we require that the
	 * order 也 match. However, attnotnull need not match.
	 */
	typeTupleDesc = lookup_rowtype_tupdesc(typeid, -1);
	tableTupleDesc = RelationGetDescr(rel);
	table_attno = 1;
	for (type_attno = 1; type_attno <= typeTupleDesc->natts; type_attno++)
	{
		Form_pg_attribute type_attr,
					table_attr;
		const char *type_attname,
				   *table_attname;

		/* Get the 下一个 non-删除ped 类型 属性. */
		type_attr = TupleDescAttr(typeTupleDesc, type_attno - 1);
		if (type_attr->attisdropped)
			continue;
		type_attname = NameStr(type_attr->attname);

		/* Get the 下一个 non-删除ped 表 属性. */
		do
		{
			if (table_attno > tableTupleDesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table is missing column \"%s\"",
								type_attname)));
			table_attr = TupleDescAttr(tableTupleDesc, table_attno - 1);
			table_attno++;
		} while (table_attr->attisdropped);
		table_attname = NameStr(table_attr->attname);

		/* Compare 名称. */
		if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has column \"%s\" where type requires \"%s\"",
							table_attname, type_attname)));

		/* Compare 类型. */
		if (table_attr->atttypid != type_attr->atttypid ||
			table_attr->atttypmod != type_attr->atttypmod ||
			table_attr->attcollation != type_attr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), type_attname)));
	}
	ReleaseTupleDesc(typeTupleDesc);

	/* Any remaining 列 at the end of the 表 had better be 删除ped. */
	for (; table_attno <= tableTupleDesc->natts; table_attno++)
	{
		Form_pg_attribute table_attr = TupleDescAttr(tableTupleDesc,
													 table_attno - 1);

		if (!table_attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has extra column \"%s\"",
							NameStr(table_attr->attname))));
	}

	/* If the 表 was 已经 类型d, 删除 the existing 依赖. */
	if (rel->rd_rel->reloftype)
		drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
							   DEPENDENCY_NORMAL);

	/* Record a 依赖 on the 新 类型. */
	tableobj.classId = RelationRelationId;
	tableobj.objectId = relid;
	tableobj.objectSubId = 0;
	typeobj.classId = TypeRelationId;
	typeobj.objectId = typeid;
	typeobj.objectSubId = 0;
	recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

	/* Update pg_class.relof类型 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	classtuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classtuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(classtuple))->reloftype = typeid;
	CatalogTupleUpdate(relationRelation, &classtuple->t_self, classtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(classtuple);
	table_close(relationRelation, RowExclusiveLock);

	ReleaseSysCache(typetuple);

	return typeobj;
}

/*
 * ALTER TABLE NOT OF
 *
 * 把类型化表与其源类型解除关联。只需清除 reloftype      
 * 并移除依赖项。 
 */
static void
ATExecDropOf(Relation rel, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Relation	relationRelation;
	HeapTuple	tuple;

	if (!OidIsValid(rel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a typed table",
						RelationGetRelationName(rel))));

	/*
	 * 我们不 bother to check ownership of the type --- ownership of the table is
	 * presumed enough rights. No 锁 required on the type, either.
	 */

	drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
						   DEPENDENCY_NORMAL);

	/* Clear pg_class.relof类型 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(tuple))->reloftype = InvalidOid;
	CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * relation_mark_replica_identity：更新表的副本标识        
 *
 * 仅当 ri_type = REPLICA_IDENTITY_INDEX 时，indexOid 必须是            
 * 适用索引 OID，否则 InvalidOid。 
 *
 * 调用者最好已在该关系上持有排他锁，因为并发运行两
 * 个此类操作的话，后果可不会太妙。        
 */
static void
relation_mark_replica_identity(Relation rel, char ri_type, Oid indexOid,
							   bool is_internal)
{
	Relation	pg_index;
	Relation	pg_class;
	HeapTuple	pg_class_tuple;
	HeapTuple	pg_index_tuple;
	Form_pg_class pg_class_form;
	Form_pg_index pg_index_form;
	ListCell   *index;

	/*
	 * Check whether relreplident has changed, and update it 如果是这样.
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	pg_class_tuple = SearchSysCacheCopy1(RELOID,
										 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "cache lookup failed for relation \"%s\"",
			 RelationGetRelationName(rel));
	pg_class_form = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	if (pg_class_form->relreplident != ri_type)
	{
		pg_class_form->relreplident = ri_type;
		CatalogTupleUpdate(pg_class, &pg_class_tuple->t_self, pg_class_tuple);
	}
	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(pg_class_tuple);

	/*
	 * Update the per-索引 indisreplident flags correctly.
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);
	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);
		bool		dirty = false;

		pg_index_tuple = SearchSysCacheCopy1(INDEXRELID,
											 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(pg_index_tuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		pg_index_form = (Form_pg_index) GETSTRUCT(pg_index_tuple);

		if (thisIndexOid == indexOid)
		{
			/* Set the bit 如果不是 已经 设置. */
			if (!pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = true;
			}
		}
		else
		{
			/* Un设置 the bit if 设置. */
			if (pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = false;
			}
		}

		if (dirty)
		{
			CatalogTupleUpdate(pg_index, &pg_index_tuple->t_self, pg_index_tuple);
			InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
										 InvalidOid, is_internal);

			/*
			 * Invalidate the relcache for the table, 从而 之后 we 提交 all 会话 will refresh
			 * the table's 副本标识 索引 之前 attempting any UPDATE or DELETE on the table. (If
			 * we changed the table's pg_class row 以上, then a relcache inval is 已经
			 * queued due to that; but we might not have.)
			 *
			 */
			CacheInvalidateRelcache(rel);
		}
		heap_freetuple(pg_index_tuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * ALTER TABLE <name> REPLICA IDENTITY ...
 */
static void
ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode)
{
	Oid			indexOid;
	Relation	indexRel;
	int			key;

	if (stmt->identity_type == REPLICA_IDENTITY_DEFAULT)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_FULL)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_NOTHING)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_INDEX)
	{
		 /* fallthrough */ ;
	}
	else
		elog(ERROR, "unexpected identity type %u", stmt->identity_type);

	/* Check that the 索引 exists */
	indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						stmt->name, RelationGetRelationName(rel))));

	indexRel = index_open(indexOid, ShareLock);

	/* Check that the 索引 is on the 关系 we're 修改ing. */
	if (indexRel->rd_index == NULL ||
		indexRel->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(indexRel),
						RelationGetRelationName(rel))));
	/* The AM must support uniqueness, and the 索引 must 实际上 be unique. */
	if (!indexRel->rd_indam->amcanunique ||
		!indexRel->rd_index->indisunique)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use non-unique index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Deferred 索引es 不是 guaranteed to be 始终 unique. */
	if (!indexRel->rd_index->indimmediate)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use non-immediate index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Expression 索引es aren't supported. */
	if (RelationGetIndexExpressions(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use expression index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Predicate 索引es aren't supported. */
	if (RelationGetIndexPredicate(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use partial index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));

	/* Check 索引 for nullable 列. */
	for (key = 0; key < IndexRelationGetNumberOfKeyAttributes(indexRel); key++)
	{
		int16		attno = indexRel->rd_index->indkey.values[key];
		Form_pg_attribute attr;

		/*
		 * Reject any other 系统列. (Going forward, we'll disallow 索引 containing such
		 * columns in the first place, but they might exist in older branches.)
		 *
		 */
		if (attno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index \"%s\" cannot be used as replica identity because column %d is a system column",
							RelationGetRelationName(indexRel), attno)));

		attr = TupleDescAttr(rel->rd_att, attno - 1);
		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" cannot be used as replica identity because column \"%s\" is nullable",
							RelationGetRelationName(indexRel),
							NameStr(attr->attname))));
	}

	/* This 索引 is sui表 for use as a replica 标识符entity. Mark it. */
	relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

	index_close(indexRel, NoLock);
}

/*
 * ALTER TABLE ENABLE/DISABLE ROW LEVEL SECURITY
 */
static void
ATExecSetRowSecurity(Relation rel, bool rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	/* Pull the record for this 关系 and 更新 it */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity = rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER TABLE FORCE/NO FORCE ROW LEVEL SECURITY
 */
static void
ATExecForceNoForceRowSecurity(Relation rel, bool force_rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relforcerowsecurity = force_rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER FOREIGN TABLE <name> OPTIONS (...)
 */
static void
ATExecGenericOptions(Relation rel, List *options)
{
	Relation	ftrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_foreign_table];
	bool		repl_null[Natts_pg_foreign_table];
	bool		repl_repl[Natts_pg_foreign_table];
	Datum		datum;
	Form_pg_foreign_table tableform;

	if (options == NIL)
		return;

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	tableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(tableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the 当前 选项 */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tuple,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the 选项 */
	datum = transformGenericOptions(ForeignTableRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
	else
		repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

	repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

	/* Everything 查找s good - 更新 the 元组 */

	tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel),
							  repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(ftrel, &tuple->t_self, tuple);

	/*
	 * Invalidate relcache 从而 all 会话 will refresh any cached plans that might
	 * depend on the old options.
	 */
	CacheInvalidateRelcache(rel);

	InvokeObjectPostAlterHook(ForeignTableRelationId,
							  RelationGetRelid(rel), 0);

	table_close(ftrel, RowExclusiveLock);

	heap_freetuple(tuple);
}

/*
 * ALTER TABLE ALTER COLUMN SET COMPRESSION
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetCompression(Relation rel,
					 const char *column,
					 Node *newValue,
					 LOCKMODE lockmode)
{
	Relation	attrel;
	HeapTuple	tuple;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	char	   *compression;
	char		cmethod;
	ObjectAddress address;

	compression = strVal(newValue);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 复制 the cache 条目 so we can scribble on it below */
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), column);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, RelationGetRelationName(rel))));

	/* prevent them from 修改ing a system 属性 */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", column)));

	/*
	 * 检查 列类型 is compressible, then get the 属性 压缩 method code
	 *
	 */
	cmethod = GetAttributeCompression(atttableform->atttypid, compression);

	/* 更新 pg_属性 条目 */
	atttableform->attcompression = cmethod;
	CatalogTupleUpdate(attrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);

	/*
	 * Apply the change to 索引 同时 (仅 for simple 索引 columns, matching behavior of
	 * 索引.c ConstructTupleDescriptor()).
	 */
	SetIndexStorageProperties(rel, attrel, attnum,
							  false, 0,
							  true, cmethod,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrel, RowExclusiveLock);

	/* 使 变更s 可见 */
	CommandCounterIncrement();

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * SET LOGGED/UNLOGGED 的准备阶段      
 *
 * 此阶段会验证我们并非试图更改临时表。另外，   
 * 还会检查现有外键约束，以避免最终出现永久表     
 * 引用不记录 WAL 的表的情况。       
 *
 * 若操作为无操作则返回 false（此时跳过检查），     
 * 否则返回 true。                
 */
static bool
ATPrepChangePersistence(Relation rel, bool toLogged)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/*
	 * Disallow changing status for a 临时表. Also verify whether 我们可以 get away with
	 * doing nothing; in such cases 我们不 need to run the 检查 以下, either.
	 *
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change logged status of table \"%s\" because it is temporary",
							RelationGetRelationName(rel)),
					 errtable(rel)));
			break;
		case RELPERSISTENCE_PERMANENT:
			if (toLogged)
				/* 无需操作 */
				return false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!toLogged)
				/* 无需操作 */
				return false;
			break;
	}

	/*
	 * 检查 the table is not part of any publication when changing to UNLOGGED, as
	 * UNLOGGED tables can't be published.
	 */
	if (!toLogged &&
		GetRelationPublications(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot change table \"%s\" to unlogged because it is part of a publication",
						RelationGetRelationName(rel)),
				 errdetail("Unlogged relations cannot be replicated.")));

	/*
	 * Check existing 外键 约束 to preserve the invariant that permanent tables
	 * cannot reference unlogged ones. Self-referencing foreign keys can safely
	 * be ignored.
	 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	/*
	 * Scan conrelid if changing to permanent, else confrelid. This 也 determines
	 * whether a useful 索引 exists.
	 */
	ScanKeyInit(&skey[0],
				toLogged ? Anum_pg_constraint_conrelid :
				Anum_pg_constraint_confrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(pg_constraint,
							  toLogged ? ConstraintRelidTypidNameIndexId : InvalidOid,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			Oid			foreignrelid;
			Relation	foreignrel;

			/* the opposite end of what we 使用 as 扫描键 */
			foreignrelid = toLogged ? con->confrelid : con->conrelid;

			/* ignore if self-referencing */
			if (RelationGetRelid(rel) == foreignrelid)
				continue;

			foreignrel = relation_open(foreignrelid, AccessShareLock);

			if (toLogged)
			{
				if (!RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to logged because it references unlogged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}
			else
			{
				if (RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to unlogged because it references logged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}

			relation_close(foreignrel, AccessShareLock);
		}
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	return true;
}

/*
 * Execute ALTER TABLE SET SCHEMA
 */
ObjectAddress
AlterTableNamespace(AlterObjectSchemaStmt *stmt, Oid *oldschema)
{
	Relation	rel;
	Oid			relid;
	Oid			oldNspOid;
	Oid			nspOid;
	RangeVar   *newrv;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackForAlterRelation,
									 (void *) stmt);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	rel = relation_open(relid, NoLock);

	oldNspOid = RelationGetNamespace(rel);

	/* If it's an owned 序列, dis所有ow moving it by itself. */
	if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
	{
		Oid			tableId;
		int32		colId;

		if (sequenceIsOwned(relid, DEPENDENCY_AUTO, &tableId, &colId) ||
			sequenceIsOwned(relid, DEPENDENCY_INTERNAL, &tableId, &colId))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot move an owned sequence into another schema"),
					 errdetail("Sequence \"%s\" is linked to table \"%s\".",
							   RelationGetRelationName(rel),
							   get_rel_name(tableId))));
	}

	/* Get and 锁 模式 OID and 检查 its 权限s. */
	newrv = makeRangeVar(stmt->newschema, RelationGetRelationName(rel), -1);
	nspOid = RangeVarGetAndCheckCreationNamespace(newrv, NoLock, NULL);

	/* common 检查s on switching 名称spaces */
	CheckSetNamespace(oldNspOid, nspOid);

	objsMoved = new_object_addresses();
	AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	ObjectAddressSet(myself, RelationRelationId, relid);

	if (oldschema)
		*oldschema = oldNspOid;

	/* 关闭 rel, but 保持 锁 直到 提交 */
	relation_close(rel, NoLock);

	return myself;
}

/*
 * The guts of relocating a table or 物化视图 to another 命名空间: besides moving the
 * relation itself, its dependent objects are relocated to the new 模式.
 *
 */
void
AlterTableNamespaceInternal(Relation rel, Oid oldNspOid, Oid nspOid,
							ObjectAddresses *objsMoved)
{
	Relation	classRel;

	Assert(objsMoved != NULL);

	/* OK, modify the pg_class 行 and pg_depend 条目 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, RelationGetRelid(rel), oldNspOid,
								   nspOid, true, objsMoved);

	/* Fix the 表's 行 类型 too, if it has one */
	if (OidIsValid(rel->rd_rel->reltype))
		AlterTypeNamespaceInternal(rel->rd_rel->reltype,
								   nspOid, false, false, objsMoved);

	/* Fix other dependent stuff */
	AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved);
	AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid,
					   objsMoved, AccessExclusiveLock);
	AlterConstraintNamespaces(RelationGetRelid(rel), oldNspOid, nspOid,
							  false, objsMoved);

	table_close(classRel, RowExclusiveLock);
}

/*
 * The guts of relocating a relation to another 命名空间: fix the pg_class entry,
 * and the pg_depend entry 如有. Caller must 已经 have opened and write-locked
 * pg_class.
 */
void
AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry,
							   ObjectAddresses *objsMoved)
{
	HeapTuple	classTup;
	Form_pg_class classForm;
	ObjectAddress thisobj;
	bool		already_done = false;

	/* no rel 锁 for relkind=c so use LOCKTAG_TUPLE */
	classTup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	thisobj.classId = RelationRelationId;
	thisobj.objectId = relOid;
	thisobj.objectSubId = 0;

	/*
	 * If the object has 已经 been moved, don't move it again. If it's 已经 in the
	 * right place, don't move it, but still fire the object access hook.
	 *
	 */
	already_done = object_address_present(&thisobj, objsMoved);
	if (!already_done && oldNspOid != newNspOid)
	{
		ItemPointerData otid = classTup->t_self;

		/* 检查 for duplicate 名称 (更多 friendly than unique-索引 failure) */
		if (get_relname_relid(NameStr(classForm->relname),
							  newNspOid) != InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists in schema \"%s\"",
							NameStr(classForm->relname),
							get_namespace_name(newNspOid))));

		/* classTup is a 复制, so OK to scribble on */
		classForm->relnamespace = newNspOid;

		CatalogTupleUpdate(classRel, &otid, classTup);
		UnlockTuple(classRel, &otid, InplaceUpdateTupleLock);


		/* Update 依赖 on 模式 if c所有er sa标识符 so */
		if (hasDependEntry &&
			changeDependencyFor(RelationRelationId,
								relOid,
								NamespaceRelationId,
								oldNspOid,
								newNspOid) != 1)
			elog(ERROR, "failed to change schema dependency for relation \"%s\"",
				 NameStr(classForm->relname));
	}
	else
		UnlockTuple(classRel, &classTup->t_self, InplaceUpdateTupleLock);
	if (!already_done)
	{
		add_exact_object_address(&thisobj, objsMoved);

		InvokeObjectPostAlterHook(RelationRelationId, relOid, 0);
	}

	heap_freetuple(classTup);
}

/*
 * Move all 索引 for the specified relation to another 命名空间. Note: we assume
 * adequate 权限 checking was done by the caller, and that the caller has a
 * suitable 锁 on the owning relation.
 *
 */
static void
AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved)
{
	List	   *indexList;
	ListCell   *l;

	indexList = RelationGetIndexList(rel);

	foreach(l, indexList)
	{
		Oid			indexOid = lfirst_oid(l);
		ObjectAddress thisobj;

		thisobj.classId = RelationRelationId;
		thisobj.objectId = indexOid;
		thisobj.objectSubId = 0;

		/*
		 * 注意：当前索引不会在命名空间上有自己的依赖，
		 * 因此无需执行 changeDependencyFor()。pg_type 中也没有    
		 * 对应的行类型。       
		 *
		 * XXX 这个 objsMoved 测试可能是多余的 -- 从关系到每个
		 * 索引应该只有一条依赖链接吧？    
		 */
		if (!object_address_present(&thisobj, objsMoved))
		{
			AlterRelationNamespaceInternal(classRel, indexOid,
										   oldNspOid, newNspOid,
										   false, objsMoved);
			add_exact_object_address(&thisobj, objsMoved);
		}
	}

	list_free(indexList);
}

/*
 * 把指定关系的所有标识列和 SERIAL 列的序列移动到另一个命名  
 * 空间。 
 *
 * 注意：我们假定调用者已完成充分的权限检查，     
 * 并且调用者已在该拥有关系上持有适当的锁。   
 */
static void
AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
				   LOCKMODE lockmode)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL 序列 are those having an auto 依赖 on one of the table's columns (我们不
	 * care *which* column, exactly).
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	/* we leave refobjsub标识符 unspecified */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* 跳过 依赖 other than auto 依赖 on 列 */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* Use 关系_打开 仅 以防 it's an 索引 */
		seqRel = relation_open(depForm->objid, lockmode);

		/* 跳过 non-序列 关系s */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No 需要 to 保持 the 锁 */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* Fix the pg_class and pg_depend 条目 */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true, objsMoved);

		/*
		 * Sequences used to have entries in pg_type, but no longer do. If we ever
		 * re-instate that, we'll need to move the pg_type entry to the new 命名空间,
		 * too (using AlterTypeNamespaceInternal).
		 */
		Assert(RelationGetForm(seqRel)->reltype == InvalidOid);

		/* Now we can 关闭 it.  Keep the 锁 till end of 事务. */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}


/*
 * 此代码支持   
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * 因为我们只对 TEMP 表支持此功能，把状态记录在一个后 
 * 端本地的数据结构中就足够了。  
 */

/*
 * Register a newly-created relation's ON COMMIT action.
 */
void
register_on_commit_action(Oid relid, OnCommitAction action)
{
	OnCommitItem *oc;
	MemoryContext oldcxt;

	/*
	 * We needn't bother registering the relation unless there is an ON COMMIT
	 * action 我们需要 take.
	 */
	if (action == ONCOMMIT_NOOP || action == ONCOMMIT_PRESERVE_ROWS)
		return;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	oc = (OnCommitItem *) palloc(sizeof(OnCommitItem));
	oc->relid = relid;
	oc->oncommit = action;
	oc->creating_subid = GetCurrentSubTransactionId();
	oc->deleting_subid = InvalidSubTransactionId;

	/*
	 * We use lcons() here 从而 ON COMMIT actions are processed in reverse order of
	 * registration. That might not be essential but it seems reasonable.
	 *
	 */
	on_commits = lcons(oc, on_commits);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Unregister any ON COMMIT action when a relation is deleted. Actually, we 仅
 * mark the OnCommitItem entry as to be deleted 之后 提交.
 *
 */
void
remove_on_commit_action(Oid relid)
{
	ListCell   *l;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		if (oc->relid == relid)
		{
			oc->deleting_subid = GetCurrentSubTransactionId();
			break;
		}
	}
}

/*
 * 执行 ON COMMIT 动作。
 *
 * 此例程在实际提交之前被调用，因为这一过程有可  
 * 能遇到错误。  
 */
void
PreCommit_on_commit_actions(void)
{
	ListCell   *l;
	List	   *oids_to_truncate = NIL;
	List	   *oids_to_drop = NIL;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		/* Ignore 条目 if 已经 删除ped in this xact */
		if (oc->deleting_subid != InvalidSubTransactionId)
			continue;

		switch (oc->oncommit)
		{
			case ONCOMMIT_NOOP:
			case ONCOMMIT_PRESERVE_ROWS:
				/* Do nothing (t此处 shouldn't be such 条目, actu所有y) */
				break;
			case ONCOMMIT_DELETE_ROWS:

				/*
				 * If this 事务 hasn't accessed any temporary relations, 我们可以 skip
				 * truncating ON COMMIT DELETE ROWS tables, as they must still be empty.
				 *
				 */
				if ((MyXactFlags & XACT_FLAGS_ACCESSEDTEMPNAMESPACE))
					oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
				break;
			case ONCOMMIT_DROP:
				oids_to_drop = lappend_oid(oids_to_drop, oc->relid);
				break;
		}
	}

	/*
	 * Truncate relations 之前 dropping 从而 all 依赖 之间 relations are removed 之后 they
	 * are worked on. Doing it like this 可能 a waste as it is possible that a
	 * relation being truncated 将被 dropped anyway due to its parent being
	 * dropped, but this makes the code more robust 因为 of not having to re-检查 the
	 * relation exists at truncation time.
	 *
	 */
	if (oids_to_truncate != NIL)
		heap_truncate(oids_to_truncate);

	if (oids_to_drop != NIL)
	{
		ObjectAddresses *targetObjects = new_object_addresses();

		foreach(l, oids_to_drop)
		{
			ObjectAddress object;

			object.classId = RelationRelationId;
			object.objectId = lfirst_oid(l);
			object.objectSubId = 0;

			Assert(!object_address_present(&object, targetObjects));

			add_exact_object_address(&object, targetObjects);
		}

		/*
		 * Object deletion might involve TOAST 表 access (to clean up toasted 目录
		 * entries), so ensure we have a valid 快照.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Since this is an automatic drop, rather than one directly initiated by
		 * the 用户, we pass the PERFORM_DELETION_INTERNAL flag.
		 */
		performMultipleDeletions(targetObjects, DROP_CASCADE,
								 PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);

		PopActiveSnapshot();

#ifdef USE_ASSERT_CHECKING

		/*
		 * 请注意 table deletion will call remove_on_commit_action, so the entry should
		 * get marked as deleted.
		 */
		foreach(l, on_commits)
		{
			OnCommitItem *oc = (OnCommitItem *) lfirst(l);

			if (oc->oncommit != ONCOMMIT_DROP)
				continue;

			Assert(oc->deleting_subid != InvalidSubTransactionId);
		}
#endif
	}
}

/*
 * ON COMMIT 管理的提交后或中止后清理工作。    
 *
 * 这里仅移除不再需要的 OnCommitItem 条目。          
 *
 * 提交时，移除在本事务中被删除的条目；                  
 * 中止时，移除在本事务中创建的条目。        
 */
void
AtEOXact_on_commit_actions(bool isCommit)
{
	ListCell   *cur_item;

	foreach(cur_item, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (isCommit ? oc->deleting_subid != InvalidSubTransactionId :
			oc->creating_subid != InvalidSubTransactionId)
		{
			/* cur_item 必须是 移除d */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item 必须是 preserved */
			oc->creating_subid = InvalidSubTransactionId;
			oc->deleting_subid = InvalidSubTransactionId;
		}
	}
}

/*
 * Post-subcommit or post-subabort cleanup for ON COMMIT management. During
 * subabort, 我们可以 immediately remove entries created 期间 this 子事务. During
 * subcommit, just relabel entries marked 期间 this 子事务 as being the parent's
 * responsibility.
 *
 */
void
AtEOSubXact_on_commit_actions(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid)
{
	ListCell   *cur_item;

	foreach(cur_item, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (!isCommit && oc->creating_subid == mySubid)
		{
			/* cur_item 必须是 移除d */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item 必须是 preserved */
			if (oc->creating_subid == mySubid)
				oc->creating_subid = parentSubid;
			if (oc->deleting_subid == mySubid)
				oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
		}
	}
}

/*
 * This is intended as a 回调 for RangeVarGetRelidExtended(). It 允许 the relation
 * to be locked 仅 if (1) it's a plain or 分区表, 物化视图, or TOAST table and (2) the
 * current 用户 is the 属主 (or the 超级用户). This meets the 权限-checking needs of
 * CLUSTER, REINDEX TABLE, and REFRESH MATERIALIZED VIEW; we expose it here 从而
 * it can be used by all.
 *
 */
void
RangeVarCallbackOwnsTable(const RangeVar *relation,
						  Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;

	/* 如果未找到关系，无需做任何操作。 */
	if (!OidIsValid(relId))
		return;

	/*
	 * If the relation does exist, check whether it's an 索引. But 请注意 the relation
	 * might have been dropped 之间 the time we did the name lookup and now. In
	 * that case, there's nothing to do.
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE &&
		relkind != RELKIND_MATVIEW && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or materialized view", relation->relname)));

	/* Check 权限s */
	if (!object_ownercheck(RelationRelationId, relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relId)), relation->relname);
}

/*
 * Callback to RangeVarGetRelidExtended() for TRUNCATE processing.
 */
static void
RangeVarCallbackForTruncate(const RangeVar *relation,
							Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/* 如果未找到关系，无需做任何操作。 */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	truncate_check_rel(relId, (Form_pg_class) GETSTRUCT(tuple));
	truncate_check_perms(relId, (Form_pg_class) GETSTRUCT(tuple));

	ReleaseSysCache(tuple);
}

/*
 * Callback for RangeVarGetRelidExtended(). Checks that the current 用户 is the
 * 属主 of the relation, or 超级用户.
 */
void
RangeVarCallbackOwnsRelation(const RangeVar *relation,
							 Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/* 如果未找到关系，无需做任何操作。 */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	if (!object_ownercheck(RelationRelationId, relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relId)),
					   relation->relname);

	if (!allowSystemTableMods &&
		IsSystemClass(relId, (Form_pg_class) GETSTRUCT(tuple)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						relation->relname)));

	ReleaseSysCache(tuple);
}

/*
 * Common RangeVarGetRelid 回调 for rename, set 模式, and alter table processing.
 *
 */
static void
RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	Node	   *stmt = (Node *) arg;
	ObjectType	reltype;
	HeapTuple	tuple;
	Form_pg_class classform;
	AclResult	aclresult;
	char		relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* Must own 关系. */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	/* No system 表 modifications un更少 explicitly 所有owed. */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/*
	 * Extract the specified relation type from the statement 解析树. Also, for
	 * ALTER .. RENAME, check 权限: the 用户 must (still) have CREATE rights on the
	 * containing 命名空间.
	 *
	 */
	if (IsA(stmt, RenameStmt))
	{
		aclresult = object_aclcheck(NamespaceRelationId, classform->relnamespace,
									GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(classform->relnamespace));
		reltype = ((RenameStmt *) stmt)->renameType;
	}
	else if (IsA(stmt, AlterObjectSchemaStmt))
		reltype = ((AlterObjectSchemaStmt *) stmt)->objectType;

	else if (IsA(stmt, AlterTableStmt))
		reltype = ((AlterTableStmt *) stmt)->objtype;
	else
	{
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
		reltype = OBJECT_TABLE; /* placate 编译器 */
	}

	/*
	 * For compatibility with prior releases, we allow ALTER TABLE to be used
	 * with most other types of relations (but not composite types). We allow
	 * similar flexibility for ALTER INDEX in the case of RENAME, but not 否则.
	 * Otherwise, the 用户 must select the correct form of the command for the
	 * relation at issue.
	 */
	if (reltype == OBJECT_SEQUENCE && relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence", rv->relname)));

	if (reltype == OBJECT_VIEW && relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a view", rv->relname)));

	if (reltype == OBJECT_MATVIEW && relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a materialized view", rv->relname)));

	if (reltype == OBJECT_FOREIGN_TABLE && relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a foreign table", rv->relname)));

	if (reltype == OBJECT_TYPE && relkind != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a composite type", rv->relname)));

	if (reltype == OBJECT_INDEX && relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX
		&& !IsA(stmt, RenameStmt))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", rv->relname)));

	/*
	 * Don't allow ALTER TABLE on composite types. We want people to use ALTER
	 * TYPE for that.
	 */
	if (reltype != OBJECT_TYPE && relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type", rv->relname),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TYPE")));

	/*
	 * Don't allow ALTER TABLE .. SET SCHEMA on relations that can't be moved to
	 * a different 模式, 例如 索引 and TOAST tables.
	 */
	if (IsA(stmt, AlterObjectSchemaStmt))
	{
		if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of index \"%s\"",
							rv->relname),
					 errhint("Change the schema of the table instead.")));
		else if (relkind == RELKIND_COMPOSITE_TYPE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of composite type \"%s\"",
							rv->relname),
			/* translator: %s is an SQL ALTER command */
					 errhint("Use %s instead.",
							 "ALTER TYPE")));
		else if (relkind == RELKIND_TOASTVALUE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of TOAST table \"%s\"",
							rv->relname),
					 errhint("Change the schema of the table instead.")));
	}

	ReleaseSysCache(tuple);
}

/*
 * Transform any expressions present in the 分区键 Returns a 转换后的 PartitionSpec.
 *
 *
 */
static PartitionSpec *
transformPartitionSpec(Relation rel, PartitionSpec *partspec)
{
	PartitionSpec *newspec;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;

	newspec = makeNode(PartitionSpec);

	newspec->strategy = partspec->strategy;
	newspec->partParams = NIL;
	newspec->location = partspec->location;

	/* Check val标识符 编号 of 列 for strategy */
	if (partspec->strategy == PARTITION_STRATEGY_LIST &&
		list_length(partspec->partParams) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot use \"list\" partition strategy with more than one column")));

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/* take care of 任何 分区 表达式s */
	foreach(l, partspec->partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, l);

		if (pelem->expr)
		{
			/* Copy, to avo标识符 scribbling on the input */
			pelem = copyObject(pelem);

			/* Now 执行 parse transformation of the 表达式 */
			pelem->expr = transformExpr(pstate, pelem->expr,
										EXPR_KIND_PARTITION_EXPRESSION);

			/* we have to fix its 排序规则s too */
			assign_expr_collations(pstate, pelem->expr);
		}

		newspec->partParams = lappend(newspec->partParams, pelem);
	}

	return newspec;
}

/*
 * Compute per-partition-column information from a list of PartitionElems.
 * Expressions in the PartitionElems 必须是 解析-analyzed 已经.
 */
static void
ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
					  List **partexprs, Oid *partopclass, Oid *partcollation,
					  PartitionStrategy strategy)
{
	int			attn;
	ListCell   *lc;
	Oid			am_oid;

	attn = 0;
	foreach(lc, partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, lc);
		Oid			atttype;
		Oid			attcollation;

		if (pelem->name != NULL)
		{
			/* Simple 属性 reference */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			atttuple = SearchSysCacheAttName(RelationGetRelid(rel),
											 pelem->name);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in partition key does not exist",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);

			if (attform->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use system column \"%s\" in partition key",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));

			/*
			 * Generated columns cannot work: They are computed 之后 BEFORE 触发器, but
			 * partition routing is done 之前 all 触发器.
			 */
			if (attform->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use generated column in partition key"),
						 errdetail("Column \"%s\" is a generated column.",
								   pelem->name),
						 parser_errposition(pstate, pelem->location)));

			partattrs[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Expression */
			Node	   *expr = pelem->expr;
			char		partattname[16];
			Bitmapset  *expr_attrs = NULL;
			int			i;

			Assert(expr != NULL);
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * The expression 必须是 of a storable type (e.g., not RECORD). The test is
			 * the same as for whether a table column is of a safe type (which is why
			 * we needn't check for the non-expression case).
			 *
			 */
			snprintf(partattname, sizeof(partattname), "%d", attn + 1);
			CheckAttributeType(partattname,
							   atttype, attcollation,
							   NIL, CHKATYPE_IS_PARTKEY);

			/*
			 * Strip any top-level COLLATE clause. This 确保 that we treat "x COLLATE y"
			 * and "(x COLLATE y)" alike.
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			/*
			 * Examine all the columns in the 分区键 expression. When the whole-row
			 * reference is present, examine all the columns of the 分区表.
			 *
			 */
			pull_varattnos(expr, 1, &expr_attrs);
			if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, expr_attrs))
			{
				expr_attrs = bms_add_range(expr_attrs,
										   1 - FirstLowInvalidHeapAttributeNumber,
										   RelationGetNumberOfAttributes(rel) - FirstLowInvalidHeapAttributeNumber);
				expr_attrs = bms_del_member(expr_attrs, 0 - FirstLowInvalidHeapAttributeNumber);
			}

			i = -1;
			while ((i = bms_next_member(expr_attrs, i)) >= 0)
			{
				AttrNumber	attno = i + FirstLowInvalidHeapAttributeNumber;

				Assert(attno != 0);

				/*
				 * Cannot allow system column references, since that would make partition
				 * routing impossible: their values won't be known yet when 我们需要 do that.
				 *
				 */
				if (attno < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("partition key expressions cannot contain system column references")));

				/*
				 * Generated columns cannot work: They are computed 之后 BEFORE 触发器, but
				 * partition routing is done 之前 all 触发器.
				 *
				 */
				if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use generated column in partition key"),
							 errdetail("Column \"%s\" is a generated column.",
									   get_attname(RelationGetRelid(rel), attno, false)),
							 parser_errposition(pstate, pelem->location)));
			}

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno > 0)
			{

				/*
				 * User wrote "(column)" or "(column COLLATE something)". Treat it like
				 * simple 属性 anyway.
				 */
				partattrs[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				partattrs[attn] = 0;	/* 标记s the 列 as 表达式 */
				*partexprs = lappend(*partexprs, expr);

				/*
				 * transformPartitionSpec() should have 已经 rejected subqueries,
				 * aggregates, window functions, and SRFs, based on the EXPR_KIND_ for
				 * partition expressions.
				 */

				/*
				 * Preprocess the expression 之前 checking for mutability. This is essential
				 * for the reasons described in contain_mutable_functions_之后_planning.
				 * However, we call expression_planner for ourselves rather than using
				 * that function, 因为 if constant-folding reduces the expression to a
				 * constant, we'd like to know that so 我们可以 complain 以下. Like
				 * contain_mutable_functions_之后_planning, assume that expression_planner
				 * won't scribble on its input, so this won't affect the partexprs entry
				 * we saved 以上.
				 *
				 *
				 *
				 */
				expr = (Node *) expression_planner((Expr *) expr);

				/*
				 * Partition expressions cannot contain mutable functions, 因为 a given row
				 * must 始终 map to the same partition as long as there is no change in the
				 * partition boundary structure.
				 *
				 */
				if (contain_mutable_functions(expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in partition key expression must be marked IMMUTABLE")));

				/*
				 * While it is not exactly *wrong* for a partition expression
				 * to be a constant, it seems better to reject such keys.
				 */
				if (IsA(expr, Const))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use constant expression as partition key")));
			}
		}

		/*
		 * Apply 排序规则 override 如有
		 */
		if (pelem->collation)
			attcollation = get_collation_oid(pelem->collation, false);

		/*
		 * Check we have a 排序规则 iff it's a collatable type. The 仅 expected failures
		 * here are (1) COLLATE applied to a noncollatable type, or (2) partition
		 * expression had an unresolved 排序规则. But we might 同时 code this to be a
		 * complete consistency check.
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for partition expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		partcollation[attn] = attcollation;

		/*
		 * Identify the appropriate operator class. For list and range partitioning,
		 * we use a btree operator class; 哈希 partitioning uses a 哈希 operator class.
		 *
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			am_oid = HASH_AM_OID;
		else
			am_oid = BTREE_AM_OID;

		if (!pelem->opclass)
		{
			partopclass[attn] = GetDefaultOpClass(atttype, am_oid);

			if (!OidIsValid(partopclass[attn]))
			{
				if (strategy == PARTITION_STRATEGY_HASH)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "hash"),
							 errhint("You must specify a hash operator class or define a default hash operator class for the data type.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "btree"),
							 errhint("You must specify a btree operator class or define a default btree operator class for the data type.")));
			}
		}
		else
			partopclass[attn] = ResolveOpClass(pelem->opclass,
											   atttype,
											   am_oid == HASH_AM_OID ? "hash" : "btree",
											   am_oid);

		attn++;
	}
}

/*
 * PartConstraintImpliedByRelConstraint
 *		scanrel 现有的约束是否蕴含该分区约束？            
 *
 * "现有约束"包括它的 CHECK 约束和列级 NOT NULL        
 * 约束。partConstraint 描述分区约束，其形式为                
 * 隐式 AND。            
 */
bool
PartConstraintImpliedByRelConstraint(Relation scanrel,
									 List *partConstraint)
{
	List	   *existConstraint = NIL;
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			i;

	if (constr && constr->has_not_null)
	{
		int			natts = scanrel->rd_att->natts;

		for (i = 1; i <= natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(scanrel->rd_att, i - 1);

			if (att->attnotnull && !att->attisdropped)
			{
				NullTest   *ntest = makeNode(NullTest);

				ntest->arg = (Expr *) makeVar(1,
											  i,
											  att->atttypid,
											  att->atttypmod,
											  att->attcollation,
											  0);
				ntest->nulltesttype = IS_NOT_NULL;

				/*
				 * argisrow=false is correct even for a composite column, 因为 attnotnull
				 * does not represent a SQL-spec IS NOT NULL test in such a case, just IS
				 * DISTINCT FROM NULL.
				 */
				ntest->argisrow = false;
				ntest->location = -1;
				existConstraint = lappend(existConstraint, ntest);
			}
		}
	}

	return ConstraintImpliedByRelConstraint(scanrel, partConstraint, existConstraint);
}

/*
 * ConstraintImpliedByRelConstraint
 *		scanrel 现有的约束是否蕴含给定的约束？        
 *
 * testConstraint 是待验证的约束。provenConstraint 是        
 * 调用者提供的条件列表，本函数可以假定其为     
 * 真。provenConstraint 与 testConstraint 都必须处于隐式
 * AND 形式，必须只含不可变子句，并且必须只含   
 * varno = 1 的 Var。             
 */
bool
ConstraintImpliedByRelConstraint(Relation scanrel, List *testConstraint, List *provenConstraint)
{
	List	   *existConstraint = list_copy(provenConstraint);
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			num_check,
				i;

	num_check = (constr != NULL) ? constr->num_check : 0;
	for (i = 0; i < num_check; i++)
	{
		Node	   *cexpr;

		/*
		 * If this 约束 hasn't been fully 已验证 yet, 我们必须 ignore it here.
		 *
		 */
		if (!constr->check[i].ccvalid)
			continue;

		cexpr = stringToNode(constr->check[i].ccbin);

		/*
		 * Run each expression through const-simplification and canonicalization. It
		 * is necessary, 因为 we 将被 comparing it to similarly-processed partition 约束
		 * expressions, and may fail to detect valid matches 没有 this.
		 *
		 */
		cexpr = eval_const_expressions(NULL, cexpr);
		cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

		existConstraint = list_concat(existConstraint,
									  make_ands_implicit((Expr *) cexpr));
	}

	/*
	 * Try to make the proof. Since we are comparing CHECK 约束, 我们需要 use weak
	 * implication, i.e., we assume existConstraint is not-false and try to prove
	 * the same for testConstraint. 请注意 predicate_implied_by assumes its first
	 * argument is known immutable. That should 始终 be true for both NOT NULL and
	 * partition 约束, so 我们不 test it here.
	 *
	 *
	 */
	return predicate_implied_by(testConstraint, existConstraint, true);
}

/*
 * QueuePartitionConstraintValidation
 *
 * 向 wqueue 添加一个条目，让阶段 3 对给定关系及其所有   
 * 子表验证给定的分区约束。                  
 *
 * 我们首先验证给定的约束是否已被既有关系约束所蕴  
 * 含；如果是，则无需扫描表来验证，                    
 * 所以这种情况下不排队。     
 */
static void
QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
								   List *partConstraint,
								   bool validate_default)
{
	/*
	 * Based on the table's existing 约束, determine whether 与否 we may skip
	 * scanning the table.
	 */
	if (PartConstraintImpliedByRelConstraint(scanrel, partConstraint))
	{
		if (!validate_default)
			ereport(DEBUG1,
					(errmsg_internal("partition constraint for table \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		return;
	}

	/*
	 * Constraints proved insufficient. For plain relations, queue a 验证 item now;
	 * for partitioned tables, 递归 to process each partition.
	 *
	 */
	if (scanrel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlteredTableInfo *tab;

		/* Grab a 工作 队列 条目. */
		tab = ATGetQueueEntry(wqueue, scanrel);
		Assert(tab->partition_constraint == NULL);
		tab->partition_constraint = (Expr *) linitial(partConstraint);
		tab->validate_default = validate_default;
	}
	else if (scanrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(scanrel, true);
		int			i;

		for (i = 0; i < partdesc->nparts; i++)
		{
			Relation	part_rel;
			List	   *thisPartConstraint;

			/*
			 * This is the minimum 锁 我们需要 prevent deadlocks.
			 */
			part_rel = table_open(partdesc->oids[i], AccessExclusiveLock);

			/*
			 * Adjust the 约束 for scanrel 从而 it matches this partition's 属性 numbers.
			 *
			 */
			thisPartConstraint =
				map_partition_varattnos(partConstraint, 1,
										part_rel, scanrel);

			QueuePartitionConstraintValidation(wqueue, part_rel,
											   thisPartConstraint,
											   validate_default);
			table_close(part_rel, NoLock);	/* 保持 锁 till 提交 */
		}
	}
}

/*
 * ALTER TABLE <name> ATTACH PARTITION <partition-name> FOR VALUES
 *
 * Return the address of the newly attached partition.
 */
static ObjectAddress
ATExecAttachPartition(List **wqueue, Relation rel, PartitionCmd *cmd,
					  AlterTableUtilityContext *context)
{
	Relation	attachrel,
				catalog;
	List	   *attachrel_children;
	List	   *partConstraint;
	SysScanDesc scan;
	ScanKeyData skey;
	AttrNumber	attno;
	int			natts;
	TupleDesc	tupleDesc;
	ObjectAddress address;
	const char *trigger_name;
	Oid			defaultPartOid;
	List	   *partBoundConstraint;
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_sourcetext = context->queryString;

	/*
	 * 我们必须 锁 the 默认分区 if one exists, 因为 attaching a new partition will change
	 * its partition 约束.
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
		LockRelationOid(defaultPartOid, AccessExclusiveLock);

	attachrel = table_openrv(cmd->name, AccessExclusiveLock);

	/*
	 * XXX I think it'd be a good idea to grab 锁 on all tables referenced by FKs
	 * at this point 也.
	 */

	/*
	 * Must be 属主 of both parent and source table -- parent was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(AT_AttachPartition, attachrel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/* A 分区 can 仅 have one 父 */
	if (attachrel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is already a partition",
						RelationGetRelationName(attachrel))));

	if (OidIsValid(attachrel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a typed table as partition")));

	/*
	 * Table being attached should not 已经 be part of 继承; either as a 子表...
	 *
	 */
	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance child as partition")));
	systable_endscan(scan);

	/* ...or as a 父 表 (except the case 当 it is 分区ed) */
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsParentIndexId, true, NULL,
							  1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)) &&
		attachrel->rd_rel->relkind == RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance parent as partition")));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	/*
	 * Prevent circularity by seeing if rel is a partition of attachrel. (In
	 * particular, this dis允许 making a rel a partition of itself.) We do that by
	 * checking if rel is a member of the list of attachrel's partitions provided
	 * the latter is partitioned at all. We want to avoid having to construct
	 * this list again, so we request the strongest 锁 on all partitions. We need
	 * the strongest 锁, 因为 we may decide to 扫描 them if we find out that the table
	 * being attached (or its leaf partitions) may contain rows that violate the
	 * partition 约束. If the table has a 约束 that would prevent such rows, which by
	 * definition is present in all the partitions, we need not 扫描 the table, nor
	 * its partitions. But 我们可以not risk a 死锁 by taking a weaker 锁 now and the
	 * stronger one 仅 when needed.
	 *
	 *
	 */
	attachrel_children = find_all_inheritors(RelationGetRelid(attachrel),
											 AccessExclusiveLock, NULL);
	if (list_member_oid(attachrel_children, RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   RelationGetRelationName(rel),
						   RelationGetRelationName(attachrel))));

	/* If the 父 is permanent, so 必须是 所有 of its 分区s. */
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a temporary relation as partition of permanent relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Temp 父 不能 have a 分区 that is itself not a temp */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a permanent relation as partition of temporary relation \"%s\"",
						RelationGetRelationName(rel))));

	/* If the 父 is temp, it 必须是long to this 会话 */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach as partition of temporary relation of another session")));

	/* Ditto for the 分区 */
	if (attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!attachrel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach temporary relation of another session as partition")));

	/* Check if t此处 are 任何 列 in attachrel that aren't in the 父 */
	tupleDesc = RelationGetDescr(attachrel);
	natts = tupleDesc->natts;
	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore 删除ped */
		if (attribute->attisdropped)
			continue;

		/* Try to 查找 the 列 in 父 (matching on 列 名称) */
		if (!SearchSysCacheExists2(ATTNAME,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   CStringGetDatum(attributeName)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" contains column \"%s\" not found in parent \"%s\"",
							RelationGetRelationName(attachrel), attributeName,
							RelationGetRelationName(rel)),
					 errdetail("The new partition may contain only the columns present in parent.")));
	}

	/*
	 * If child_rel has 行级触发器s with transition tables, we 当前 don't allow it to
	 * become a partition. See 也 prohibitions in ATExecAddInherit() and
	 * CreateTrigger().
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(attachrel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming a partition",
						trigger_name, RelationGetRelationName(attachrel)),
				 errdetail("ROW triggers with transition tables are not supported on partitions.")));

	/*
	 * 检查 the new partition's bound is valid and does not overlap any of existing
	 * partitions of the parent - 请注意 it does not return on error.
	 *
	 */
	check_new_partition_bound(RelationGetRelationName(attachrel), rel,
							  cmd->bound, pstate);

	/* OK to 创建 继承ance.  Rest of the 检查s performed t此处 */
	CreateInheritance(attachrel, rel);

	/* 更新 pg_class 条目。 */
	StorePartitionBound(attachrel, rel, cmd->bound);

	/* Ensure t此处 exists a correct 设置 of 索引es in the 分区. */
	AttachPartitionEnsureIndexes(rel, attachrel);

	/* and 触发器s */
	CloneRowTriggersToPartition(rel, attachrel);

	/*
	 * Clone 外键 约束. Callee is responsible for setting up for phase 3 约束
	 * verification.
	 */
	CloneForeignKeyConstraints(wqueue, rel, attachrel);

	/*
	 * Generate partition 约束 from the 分区边界 specification. If the parent itself is
	 * a partition, 确保 to include its 约束 同时.
	 *
	 */
	partBoundConstraint = get_qual_from_partbound(rel, cmd->bound);
	partConstraint = list_concat(partBoundConstraint,
								 RelationGetPartitionQual(rel));

	/* Skip 验证 if t此处 are no 约束s to 验证. */
	if (partConstraint)
	{
		/*
		 * Run the partition quals through const-simplification similar to check 约束.
		 * We skip canonicalize_qual, though, 因为 partition quals 应是 in canonical
		 * form 已经.
		 */
		partConstraint =
			(List *) eval_const_expressions(NULL,
											(Node *) partConstraint);

		/* XXX this sure 查找s wrong */
		partConstraint = list_make1(make_ands_explicit(partConstraint));

		/*
		 * Adjust the generated 约束 to match this partition's 属性 numbers.
		 *
		 */
		partConstraint = map_partition_varattnos(partConstraint, 1, attachrel,
												 rel);

		/* Val标识符ate 分区 约束s against the 表 being attached. */
		QueuePartitionConstraintValidation(wqueue, attachrel, partConstraint,
										   false);
	}

	/*
	 * If we're attaching a partition other than the 默认分区 and a default one
	 * exists, then that partition's partition 约束 changes, so add an entry to the
	 * work queue to 验证 it, too. (我们必须 not do this when the partition being
	 * attached is the default one; we 已经 did it 以上!)
	 *
	 */
	if (OidIsValid(defaultPartOid))
	{
		Relation	defaultrel;
		List	   *defPartConstraint;

		Assert(!cmd->bound->is_default);

		/* we 已经 h旧 a 锁 on the 默认值 分区 */
		defaultrel = table_open(defaultPartOid, NoLock);
		defPartConstraint =
			get_proposed_default_constraint(partBoundConstraint);

		/*
		 * Map the Vars in the 约束 expression from rel's attnos to defaultrel's.
		 *
		 */
		defPartConstraint =
			map_partition_varattnos(defPartConstraint,
									1, defaultrel, rel);
		QueuePartitionConstraintValidation(wqueue, defaultrel,
										   defPartConstraint, true);

		/* 保持 our 锁 直到 提交. */
		table_close(defaultrel, NoLock);
	}

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(attachrel));

	/*
	 * If the partition we just attached is partitioned itself, 失效 relcache for
	 * all descendent partitions too to ensure that their rd_partcheck expression
	 * trees are 重建; partitions 已经 locked at the beginning of this function.
	 *
	 */
	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell   *l;

		foreach(l, attachrel_children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(l));
		}
	}

	/* 保持 our 锁 直到 提交 */
	table_close(attachrel, NoLock);

	return address;
}

/*
 * AttachPartitionEnsureIndexes
 *		ATExecAttachPartition 子例程，用于创建/匹配索引 
 *
 * 在 ALTER TABLE / ATTACH PARTITION 期间，对分区表强                 
 * 制执行索引规则：每个分区都必须有一个索引，对应分   
 * 区表各索引。
 */
static void
AttachPartitionEnsureIndexes(Relation rel, Relation attachrel)
{
	List	   *idxes;
	List	   *attachRelIdxs;
	Relation   *attachrelIdxRels;
	IndexInfo **attachInfos;
	int			i;
	ListCell   *cell;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"AttachPartitionEnsureIndexes",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	idxes = RelationGetIndexList(rel);
	attachRelIdxs = RelationGetIndexList(attachrel);
	attachrelIdxRels = palloc(sizeof(Relation) * list_length(attachRelIdxs));
	attachInfos = palloc(sizeof(IndexInfo *) * list_length(attachRelIdxs));

	/* Build arrays of 所有 existing 索引es and their IndexInfos */
	i = 0;
	foreach(cell, attachRelIdxs)
	{
		Oid			cldIdxId = lfirst_oid(cell);

		attachrelIdxRels[i] = index_open(cldIdxId, AccessShareLock);
		attachInfos[i] = BuildIndexInfo(attachrelIdxRels[i]);
		i++;
	}

	/*
	 * If we're attaching a 外表, 我们必须 fail 如有 of the 索引 is a 约束 索引; 否则, there's
	 * nothing to do here. Do this 之前 starting work, to avoid wasting the effort
	 * of building a few non-unique 索引 之前 coming across a unique one.
	 *
	 */
	if (attachrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		foreach(cell, idxes)
		{
			Oid			idx = lfirst_oid(cell);
			Relation	idxRel = index_open(idx, AccessShareLock);

			if (idxRel->rd_index->indisunique ||
				idxRel->rd_index->indisprimary)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot attach foreign table \"%s\" as partition of partitioned table \"%s\"",
								RelationGetRelationName(attachrel),
								RelationGetRelationName(rel)),
						 errdetail("Partitioned table \"%s\" contains unique indexes.",
								   RelationGetRelationName(rel))));
			index_close(idxRel, AccessShareLock);
		}

		goto out;
	}

	/*
	 * For each 索引 on the 分区表, find a matching one in the partition-to-be; if one
	 * is not found, create one.
	 */
	foreach(cell, idxes)
	{
		Oid			idx = lfirst_oid(cell);
		Relation	idxRel = index_open(idx, AccessShareLock);
		IndexInfo  *info;
		AttrMap    *attmap;
		bool		found = false;
		Oid			constraintOid;

		/*
		 * Ignore 索引 in the 分区表 other than partitioned 索引.
		 *
		 */
		if (idxRel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			index_close(idxRel, AccessShareLock);
			continue;
		}

		/* construct an 索引info to compare existing 索引es against */
		info = BuildIndexInfo(idxRel);
		attmap = build_attrmap_by_name(RelationGetDescr(attachrel),
									   RelationGetDescr(rel),
									   false);
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(rel), idx);

		/*
		 * Scan the list of existing 索引 in the partition-to-be, and mark the first
		 * matching, valid, unattached one we find, 如有, as partition of the parent
		 * 索引. If we find one, we're done.
		 */
		for (i = 0; i < list_length(attachRelIdxs); i++)
		{
			Oid			cldIdxId = RelationGetRelid(attachrelIdxRels[i]);
			Oid			cldConstrOid = InvalidOid;

			/* 执行es this 索引 have a 父?  如果是, can't use it */
			if (attachrelIdxRels[i]->rd_rel->relispartition)
				continue;

			/* If this 索引 is 无效, can't use it */
			if (!attachrelIdxRels[i]->rd_index->indisvalid)
				continue;

			if (CompareIndexInfo(attachInfos[i], info,
								 attachrelIdxRels[i]->rd_indcollation,
								 idxRel->rd_indcollation,
								 attachrelIdxRels[i]->rd_opfamily,
								 idxRel->rd_opfamily,
								 attmap))
			{
				/*
				 * If this 索引 is being created in the parent 因为 of a 约束, then the child
				 * needs to have a 约束 也, so look for one. If there is no such 约束, this 索引
				 * is no good, so keep looking.
				 *
				 */
				if (OidIsValid(constraintOid))
				{
					cldConstrOid =
						get_relation_idx_constraint_oid(RelationGetRelid(attachrel),
														cldIdxId);
					/* no dice */
					if (!OidIsValid(cldConstrOid))
						continue;
				}

				/* bingo. */
				IndexSetParentIndex(attachrelIdxRels[i], idx);
				if (OidIsValid(constraintOid))
					ConstraintSetParentConstraint(cldConstrOid, constraintOid,
												  RelationGetRelid(attachrel));
				found = true;

				CommandCounterIncrement();
				break;
			}
		}

		/*
		 * If no suitable 索引 was found in the partition-to-be, create one now.
		 *
		 */
		if (!found)
		{
			IndexStmt  *stmt;
			Oid			conOid;

			stmt = generateClonedIndexStmt(NULL,
										   idxRel, attmap,
										   &conOid);
			DefineIndex(RelationGetRelid(attachrel), stmt, InvalidOid,
						RelationGetRelid(idxRel),
						conOid,
						-1,
						true, false, false, false, false);
		}

		index_close(idxRel, AccessShareLock);
	}

out:
	/* Clean up. */
	for (i = 0; i < list_length(attachRelIdxs); i++)
		index_close(attachrelIdxRels[i], AccessShareLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * CloneRowTriggersToPartition subroutine for
 * ATExecAttachPartition/DefineRelation to create row 触发器 on partitions
 *
 */
static void
CloneRowTriggersToPartition(Relation parent, Relation partition)
{
	Relation	pg_trigger;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	MemoryContext perTupCxt;

	ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parent)));
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndexId,
							  true, NULL, 1, &key);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "clone trig", ALLOCSET_SMALL_SIZES);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigForm = (Form_pg_trigger) GETSTRUCT(tuple);
		CreateTrigStmt *trigStmt;
		Node	   *qual = NULL;
		Datum		value;
		bool		isnull;
		List	   *cols = NIL;
		List	   *trigargs = NIL;
		MemoryContext oldcxt;

		/*
		 * Ignore 语句级触发器s; those are not cloned.
		 */
		if (!TRIGGER_FOR_ROW(trigForm->tgtype))
			continue;

		/*
		 * Don't clone internal 触发器, 因为 the 约束 cloning code will.
		 *
		 */
		if (trigForm->tgisinternal)
			continue;

		/*
		 * Complain if we find an unexpected 触发器 type.
		 */
		if (!TRIGGER_FOR_BEFORE(trigForm->tgtype) &&
			!TRIGGER_FOR_AFTER(trigForm->tgtype))
			elog(ERROR, "unexpected trigger \"%s\" found",
				 NameStr(trigForm->tgname));

		/* Use short-lived context for CREATE TRIGGER */
		oldcxt = MemoryContextSwitchTo(perTupCxt);

		/*
		 * If there is a WHEN clause, generate a 'cooked' version of it that's
		 * appropriate for the partition.
		 */
		value = heap_getattr(tuple, Anum_pg_trigger_tgqual,
							 RelationGetDescr(pg_trigger), &isnull);
		if (!isnull)
		{
			qual = stringToNode(TextDatumGetCString(value));
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
													partition, parent);
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
													partition, parent);
		}

		/*
		 * If there is a column list, 转换 it to a list of 列名. Note 我们不 need to map
		 * this list in any way ...
		 */
		if (trigForm->tgattr.dim1 > 0)
		{
			int			i;

			for (i = 0; i < trigForm->tgattr.dim1; i++)
			{
				Form_pg_attribute col;

				col = TupleDescAttr(parent->rd_att,
									trigForm->tgattr.values[i] - 1);
				cols = lappend(cols,
							   makeString(pstrdup(NameStr(col->attname))));
			}
		}

		/* Reconstruct 触发器 arguments 列表. */
		if (trigForm->tgnargs > 0)
		{
			char	   *p;

			value = heap_getattr(tuple, Anum_pg_trigger_tgargs,
								 RelationGetDescr(pg_trigger), &isnull);
			if (isnull)
				elog(ERROR, "tgargs is null for trigger \"%s\" in partition \"%s\"",
					 NameStr(trigForm->tgname), RelationGetRelationName(partition));

			p = (char *) VARDATA_ANY(DatumGetByteaPP(value));

			for (int i = 0; i < trigForm->tgnargs; i++)
			{
				trigargs = lappend(trigargs, makeString(pstrdup(p)));
				p += strlen(p) + 1;
			}
		}

		trigStmt = makeNode(CreateTrigStmt);
		trigStmt->replace = false;
		trigStmt->isconstraint = OidIsValid(trigForm->tgconstraint);
		trigStmt->trigname = NameStr(trigForm->tgname);
		trigStmt->relation = NULL;
		trigStmt->funcname = NULL;	/* 遍ed separately */
		trigStmt->args = trigargs;
		trigStmt->row = true;
		trigStmt->timing = trigForm->tgtype & TRIGGER_TYPE_TIMING_MASK;
		trigStmt->events = trigForm->tgtype & TRIGGER_TYPE_EVENT_MASK;
		trigStmt->columns = cols;
		trigStmt->whenClause = NULL;	/* 遍ed separately */
		trigStmt->transitionRels = NIL; /* not supported at present */
		trigStmt->deferrable = trigForm->tgdeferrable;
		trigStmt->initdeferred = trigForm->tginitdeferred;
		trigStmt->constrrel = NULL; /* 遍ed separately */

		CreateTriggerFiringOn(trigStmt, NULL, RelationGetRelid(partition),
							  trigForm->tgconstrrelid, InvalidOid, InvalidOid,
							  trigForm->tgfoid, trigForm->oid, qual,
							  false, true, trigForm->tgenabled);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(perTupCxt);
	}

	MemoryContextDelete(perTupCxt);

	systable_endscan(scan);
	table_close(pg_trigger, RowExclusiveLock);
}

/*
 * ALTER TABLE DETACH PARTITION
 *
 * 返回不再作为 rel 分区的那个关系的地址。                
 *
 * 若请求并发模式，我们将在两个事务中运行。副作用
 * 是该命令不能在多部分的 ALTER TABLE 中运行。         
 * 当前，这一点由语法强制保证   
 *
 * 并发策略是：首先修改分区的                        
 * pg_inherit 目录行，让所有人都能看到该分区已   
 * 被分离，锁定分区禁止写入，然后提交事务；        
 * 从那时起，任何请求分区描述符的人必须忽略这   
 * 样的分区。在第二个事务中，                        
 * 我们会等待所有可能已将该分区视为已附加的事务  
 * 全部结束，然后移除其余的分区元数据                 
 * （pg_inherits 与 pg_class.relpartbounds）。   
 */
static ObjectAddress
ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab, Relation rel,
					  RangeVar *name, bool concurrent)
{
	Relation	partRel;
	ObjectAddress address;
	Oid			defaultPartOid;
	PartitionDesc partdesc;

	/*
	 * 我们必须 锁 the 默认分区, 因为 detaching this partition will change its partition 约束.
	 *
	 */
	partdesc = RelationGetPartitionDesc(rel, true);
	defaultPartOid = get_default_oid_from_partdesc(partdesc);
	if (OidIsValid(defaultPartOid))
	{
		/*
		 * Concurrent detaching when a 默认分区 exists is not supported. The main
		 * problem is that the 默认分区 约束 would change. And there's a definitional
		 * problem: what should happen to the 元组 that are being inserted that belong
		 * to the partition being detached? Putting them on the partition being
		 * detached 将会 wrong, since they'd become "lost" 之后 the detaching completes
		 * but 我们可以not put them in the 默认分区 either until we alter its partition 约束.
		 * I think we could solve this problem if we effected the 约束 change 之前
		 * committing the first 事务. But the 锁 would have to remain AEL and it would
		 * cause 并发 查询 planning to be blocked, so changing it that way 将会 even
		 * worse.
		 *
		 *
		 *
		 */
		if (concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot detach partitions concurrently when a default partition exists")));
		LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	/*
	 * In 并发 mode, the partition is locked with share-update-exclusive in the
	 * first 事务. This 允许 并发 事务 to be doing DML to the partition.
	 *
	 */
	partRel = table_openrv(name, concurrent ? ShareUpdateExclusiveLock :
						   AccessExclusiveLock);

	/*
	 * Check 继承 conditions and either delete the pg_inherits row (in non-并发 mode)
	 * or just set the inhdetachpending flag.
	 */
	if (!concurrent)
		RemoveInheritance(partRel, rel, false);
	else
		MarkInheritDetached(partRel, rel);

	/*
	 * Ensure that foreign keys still hold 之后 this detach. This keeps 锁 on the
	 * referencing tables, which 阻止 并发 事务 from adding rows that we wouldn't see.
	 * For this to work in 并发 mode, it is critical that the partition appears as
	 * no longer attached for the RI 查询 as soon as the first 事务 提交.
	 *
	 */
	ATDetachCheckNoForeignKeyRefs(partRel);

	/*
	 * Concurrent mode has to work harder; first we add a new 约束 to the partition
	 * that matches the partition 约束. Then we close our existing 事务, and in a new
	 * one wait for all 处理 to catch up on the 目录 更新 we've done so far; at that
	 * point 我们可以 complete the operation.
	 *
	 */
	if (concurrent)
	{
		Oid			partrelid,
					parentrelid;
		LOCKTAG		tag;
		char	   *parentrelname;
		char	   *partrelname;

		/*
		 * For strategies other than 哈希, add a 约束 to the partition being detached
		 * which supplants the partition 约束. For 哈希 我们可以not do that, 因为 the 约束 would
		 * reference the 分区表 OID, possibly causing problems 稍后的.
		 *
		 */
		if (partdesc->boundinfo->strategy != PARTITION_STRATEGY_HASH)
			DetachAddConstraintIfNeeded(wqueue, partRel);

		/*
		 * We're almost done now; the 仅 traces that remain are the pg_inherits 元组
		 * and the partition's relpartbounds. Before 我们可以 remove those, 我们需要 wait
		 * until all 事务 that know that this is a partition are gone.
		 *
		 */

		/*
		 * Remember relation OID to re-acquire them 稍后的; and relation names too, for
		 * error messages 如果是这样mething 被删除 in 之间.
		 */
		partrelid = RelationGetRelid(partRel);
		parentrelid = RelationGetRelid(rel);
		parentrelname = MemoryContextStrdup(PortalContext,
											RelationGetRelationName(rel));
		partrelname = MemoryContextStrdup(PortalContext,
										  RelationGetRelationName(partRel));

		/* In验证 relcache 条目 for the 父 -- 必须是 之前 关闭 */
		CacheInvalidateRelcache(rel);

		table_close(partRel, NoLock);
		table_close(rel, NoLock);
		tab->rel = NULL;

		/* Make 更新d 目录 条目 可见 */
		PopActiveSnapshot();
		CommitTransactionCommand();

		StartTransactionCommand();

		/*
		 * Now wait. This 确保 that all 查询 that were planned including the partition
		 * are finished 之前 we remove the rest of 目录 entries. 我们不 need or indeed want
		 * to acquire this 锁, though -- that would 块 稍后的 查询. 我们不 need to concern
		 * ourselves with waiting for a 锁 on the partition itself, since we will
		 * acquire AccessExclusiveLock 以下.
		 *
		 *
		 */
		SET_LOCKTAG_RELATION(tag, MyDatabaseId, parentrelid);
		WaitForLockersMultiple(list_make1(&tag), AccessExclusiveLock, false);

		/*
		 * Now acquire 锁 in both relations again. Note they may have been removed in
		 * the meantime, so care is required.
		 */
		rel = try_relation_open(parentrelid, ShareUpdateExclusiveLock);
		partRel = try_relation_open(partrelid, AccessExclusiveLock);

		/* If the 关系s aren't t此处, 某些thing bad happened; bail out */
		if (rel == NULL)
		{
			if (partRel != NULL)	/* 不应发生 */
				elog(WARNING, "dangling partition \"%s\" remains, can't fix",
					 partrelname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partitioned table \"%s\" was removed concurrently",
							parentrelname)));
		}
		if (partRel == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partition \"%s\" was removed concurrently", partrelname)));

		tab->rel = rel;
	}

	/*
	 * Detaching the partition might involve TOAST table access, so ensure we
	 * have a valid 快照.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Do the final part of detaching */
	DetachPartitionFinalize(rel, partRel, concurrent, defaultPartOid);

	PopActiveSnapshot();

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	/* 保持 our 锁 直到 提交 */
	table_close(partRel, NoLock);

	return address;
}

/*
 * ALTER TABLE .. DETACH 第二部分。
 *
 * 单独列出它，是为了在并发算法的第二个事务失败  
 * （崩溃或中止）时可以独立运行。                  
 */
static void
DetachPartitionFinalize(Relation rel, Relation partRel, bool concurrent,
						Oid defaultPartOid)
{
	Relation	classRel;
	List	   *fks;
	ListCell   *cell;
	List	   *indexes;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	HeapTuple	tuple,
				newtuple;
	Relation	trigrel = NULL;
	List	   *fkoids = NIL;

	if (concurrent)
	{
		/*
		 * 我们可以 remove the pg_inherits row now. (In the non-并发 case, this was 已经
		 * done).
		 */
		RemoveInheritance(partRel, rel, true);
	}

	/* Drop 任何 触发器s that were cloned on creation/attach. */
	DropClonedTriggersFromPartition(RelationGetRelid(partRel));

	/*
	 * Detach any foreign keys that are 继承的. This includes creating additional
	 * action 触发器.
	 */
	fks = copyObject(RelationGetFKeyList(partRel));
	if (fks != NIL)
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * It's possible that the partition being detached has a 外键 that references a
	 * 分区表. When that happens, there are multiple pg_constraint rows for the
	 * partition: one points to the 分区表 itself, while the others point to each of
	 * its partitions. Only the topmost one is to be considered here; the child
	 * 约束 必须是 left alone, 因为 conceptually those aren't coming from our 父分区表, but
	 * from this partition itself. We implement this by collecting all the 约束 OID
	 * in a first 扫描 of the FK array, and skipping in the loop 以下 those 约束 whose
	 * parents are listed here.
	 *
	 *
	 *
	 */
	foreach(cell, fks)
	{
		ForeignKeyCacheInfo *fk = (ForeignKeyCacheInfo *) lfirst(cell);

		fkoids = lappend_oid(fkoids, fk->conoid);
	}

	foreach(cell, fks)
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);
		HeapTuple	contup;
		Form_pg_constraint conform;
		Oid			insertTriggerOid,
					updateTriggerOid;

		contup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
		if (!HeapTupleIsValid(contup))
			elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
		conform = (Form_pg_constraint) GETSTRUCT(contup);

		/*
		 * Consider 仅 继承的 foreign keys, and 仅 if their parents aren't in the list.
		 *
		 */
		if (conform->contype != CONSTRAINT_FOREIGN ||
			!OidIsValid(conform->conparentid) ||
			list_member_oid(fkoids, conform->conparentid))
		{
			ReleaseSysCache(contup);
			continue;
		}

		/*
		 * The 约束 on this table 必须是 marked no longer a child of the parent's 约束, as
		 * do its check 触发器.
		 */
		ConstraintSetParentConstraint(fk->conoid, InvalidOid, InvalidOid);

		/*
		 * Also, look up the partition's "check" 触发器 corresponding to the 约束 being
		 * detached and detach them from the parent 触发器.
		 */
		GetForeignKeyCheckTriggers(trigrel,
								   fk->conoid, fk->confrelid, fk->conrelid,
								   &insertTriggerOid, &updateTriggerOid);
		Assert(OidIsValid(insertTriggerOid));
		TriggerSetParentTrigger(trigrel, insertTriggerOid, InvalidOid,
								RelationGetRelid(partRel));
		Assert(OidIsValid(updateTriggerOid));
		TriggerSetParentTrigger(trigrel, updateTriggerOid, InvalidOid,
								RelationGetRelid(partRel));

		/*
		 * Lastly, create the action 触发器 on the referenced table, using
		 * addFkRecurseReferenced, which 需要 some elaborate setup (so put it in a
		 * separate 块). While at it, if the table is partitioned, that function will
		 * 递归 to create the pg_constraint rows and action 触发器 for each partition.
		 * Note there's no need to do addFkConstraint() here, 因为 the pg_constraint
		 * row 已经 exists.
		 *
		 *
		 */
		{
			Constraint *fkconstraint;
			int			numfks;
			AttrNumber	conkey[INDEX_MAX_KEYS];
			AttrNumber	confkey[INDEX_MAX_KEYS];
			Oid			conpfeqop[INDEX_MAX_KEYS];
			Oid			conppeqop[INDEX_MAX_KEYS];
			Oid			conffeqop[INDEX_MAX_KEYS];
			int			numfkdelsetcols;
			AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
			Relation	refdRel;

			DeconstructFkConstraintRow(contup,
									   &numfks,
									   conkey,
									   confkey,
									   conpfeqop,
									   conppeqop,
									   conffeqop,
									   &numfkdelsetcols,
									   confdelsetcols);

			/* Create a synthetic node we'll use throughout */
			fkconstraint = makeNode(Constraint);
			fkconstraint->contype = CONSTRAINT_FOREIGN;
			fkconstraint->conname = pstrdup(NameStr(conform->conname));
			fkconstraint->deferrable = conform->condeferrable;
			fkconstraint->initdeferred = conform->condeferred;
			fkconstraint->skip_validation = true;
			fkconstraint->initially_valid = true;
			/* a few irrelevant 字段s omitted 此处 */
			fkconstraint->pktable = NULL;
			fkconstraint->fk_attrs = NIL;
			fkconstraint->pk_attrs = NIL;
			fkconstraint->fk_matchtype = conform->confmatchtype;
			fkconstraint->fk_upd_action = conform->confupdtype;
			fkconstraint->fk_del_action = conform->confdeltype;
			fkconstraint->fk_del_set_cols = NIL;
			fkconstraint->old_conpfeqop = NIL;
			fkconstraint->old_pktable_oid = InvalidOid;
			fkconstraint->location = -1;

			/* 设置 up col名称s, 使用 to generate the 约束 名称 */
			for (int i = 0; i < numfks; i++)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(partRel),
									conkey[i] - 1);

				fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
												 makeString(NameStr(att->attname)));
			}

			refdRel = table_open(fk->confrelid, ShareRowExclusiveLock);

			addFkRecurseReferenced(fkconstraint, partRel,
								   refdRel,
								   conform->conindid,
								   fk->conoid,
								   numfks,
								   confkey,
								   conkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   numfkdelsetcols,
								   confdelsetcols,
								   true,
								   InvalidOid, InvalidOid);
			table_close(refdRel, NoLock);	/* 保持 锁 till end of xact */
		}

		ReleaseSysCache(contup);
	}
	list_free_deep(fks);
	if (trigrel)
		table_close(trigrel, RowExclusiveLock);

	/*
	 * Any sub-约束 that are in the referenced-side of a larger 约束 必须是 removed.
	 * This partition is no longer part of the key space of the 约束.
	 *
	 */
	foreach(cell, GetParentedForeignKeyRefs(partRel))
	{
		Oid			constrOid = lfirst_oid(cell);
		ObjectAddress constraint;

		ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);
		deleteDependencyRecordsForClass(ConstraintRelationId,
										constrOid,
										ConstraintRelationId,
										DEPENDENCY_INTERNAL);
		CommandCounterIncrement();

		ObjectAddressSet(constraint, ConstraintRelationId, constrOid);
		performDeletion(&constraint, DROP_RESTRICT, 0);
	}

	/* Now we can detach 索引es */
	indexes = RelationGetIndexList(partRel);
	foreach(cell, indexes)
	{
		Oid			idxid = lfirst_oid(cell);
		Oid			parentidx;
		Relation	idx;
		Oid			constrOid;
		Oid			parentConstrOid;

		if (!has_superclass(idxid))
			continue;

		parentidx = get_partition_parent(idxid, false);
		Assert((IndexGetRelation(parentidx, false) == RelationGetRelid(rel)));

		idx = index_open(idxid, AccessExclusiveLock);
		IndexSetParentIndex(idx, InvalidOid);

		/*
		 * If there's a 约束 associated with the 索引, detach it too. Careful: it is
		 * possible for a 约束 索引 in a partition to be the child of a non-约束 索引, so
		 * verify whether the parent 索引 does actually have a 约束.
		 *
		 */
		constrOid = get_relation_idx_constraint_oid(RelationGetRelid(partRel),
													idxid);
		parentConstrOid = get_relation_idx_constraint_oid(RelationGetRelid(rel),
														  parentidx);
		if (OidIsValid(parentConstrOid) && OidIsValid(constrOid))
			ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);

		index_close(idx, NoLock);
	}

	/* Update pg_class 元组 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(partRel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(partRel));
	Assert(((Form_pg_class) GETSTRUCT(tuple))->relispartition);

	/* Clear relpartbound and re设置 relis分区 */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = (Datum) 0;
	new_null[Anum_pg_class_relpartbound - 1] = true;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);

	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = false;
	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	if (OidIsValid(defaultPartOid))
	{
		/*
		 * 若被分离的关系正是默认分区本身，则把它从父
		 * 表的 pg_partitioned_table 条目中移除。         
		 *
		 * 否则，我们必须使默认分区的 relcache 条目失效， 
		 * 这与 StorePartitionBound 中一样：它的分区约束依赖  
		 * 于其他每个分区的分区约束。
		 */
		if (RelationGetRelid(partRel) == defaultPartOid)
			update_default_partition_oid(RelationGetRelid(rel), InvalidOid);
		else
			CacheInvalidateRelcacheByRelid(defaultPartOid);
	}

	/*
	 * Invalidate the parent's relcache 从而 the partition is no longer included in
	 * its 分区描述符.
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * If the partition we just detached is partitioned itself, 失效 relcache for
	 * all descendent partitions too to ensure that their rd_partcheck expression
	 * trees are 重建; must 锁 partitions 之前 doing so, using the same lockmode as
	 * what partRel has been locked with by the caller.
	 *
	 */
	if (partRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *children;

		children = find_all_inheritors(RelationGetRelid(partRel),
									   AccessExclusiveLock, NULL);
		foreach(cell, children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(cell));
		}
	}
}

/*
 * ALTER TABLE ... DETACH PARTITION ... FINALIZE To use when a DETACH
 * PARTITION command 先前 did not run to completion; this completes the
 * detaching process.
 *
 */
static ObjectAddress
ATExecDetachPartitionFinalize(Relation rel, RangeVar *name)
{
	Relation	partRel;
	ObjectAddress address;
	Snapshot	snap = GetActiveSnapshot();

	partRel = table_openrv(name, AccessExclusiveLock);

	/*
	 * Wait until existing snapshots are gone. This is important if the second 事务
	 * of DETACH PARTITION CONCURRENTLY is canceled: the 用户 could immediately run
	 * DETACH FINALIZE 没有 actually waiting for existing 事务. 我们必须 not complete the
	 * detach action until all such 查询 are complete (否则 we would present them
	 * with an inconsistent view of 目录).
	 *
	 */
	WaitForOlderSnapshots(snap->xmin, false);

	DetachPartitionFinalize(rel, partRel, true, InvalidOid);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	table_close(partRel, NoLock);

	return address;
}

/*
 * DetachAddConstraintIfNeeded
 *		ATExecDetachPartition 的子例程。创建一个              
 *		约束来代替分区约束；但如果已有蕴含所需      
 *		约束的约束存在，则避免重复                        
 *		创建。  
 */
static void
DetachAddConstraintIfNeeded(List **wqueue, Relation partRel)
{
	List	   *constraintExpr;

	constraintExpr = RelationGetPartitionQual(partRel);
	constraintExpr = (List *) eval_const_expressions(NULL, (Node *) constraintExpr);

	/*
	 * Avoid adding a new 约束 if the needed 约束 is implied by an existing 约束
	 *
	 */
	if (!PartConstraintImpliedByRelConstraint(partRel, constraintExpr))
	{
		AlteredTableInfo *tab;
		Constraint *n;

		tab = ATGetQueueEntry(wqueue, partRel);

		/* Add 约束 on 分区, equivalent to the 分区 约束 */
		n = makeNode(Constraint);
		n->contype = CONSTR_CHECK;
		n->conname = NULL;
		n->location = -1;
		n->is_no_inherit = false;
		n->raw_expr = NULL;
		n->cooked_expr = nodeToString(make_ands_explicit(constraintExpr));
		n->initially_valid = true;
		n->skip_validation = true;
		/* It's a re-添加, since it nomin所有y 已经 exists */
		ATAddCheckConstraint(wqueue, tab, partRel, n,
							 true, false, true, ShareUpdateExclusiveLock);
	}
}

/*
 * DropClonedTriggersFromPartition subroutine for ATExecDetachPartition to
 * remove any 触发器 that were cloned to the partition when it was
 * created-as-partition or attached. This undoes what
 * CloneRowTriggersToPartition did.
 */
static void
DropClonedTriggersFromPartition(Oid partitionId)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	trigtup;
	Relation	tgrel;
	ObjectAddresses *objects;

	objects = new_object_addresses();

	/*
	 * Scan pg_trigger to search for all 触发器 on this rel.
	 */
	ScanKeyInit(&skey, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(partitionId));
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
							  true, NULL, 1, &skey);
	while (HeapTupleIsValid(trigtup = systable_getnext(scan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trig;

		/* Ignore 触发器s that weren't cloned */
		if (!OidIsValid(pg_trigger->tgparentid))
			continue;

		/*
		 * Ignore internal 触发器 that are implementation objects of foreign keys, 因为
		 * these 将被 detached when the foreign keys themselves are.
		 *
		 */
		if (OidIsValid(pg_trigger->tgconstrrelid))
			continue;

		/*
		 * This is ugly, but necessary: remove the 依赖 markings on the 触发器 从而 it can
		 * be removed.
		 */
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);

		/* remember this 触发器 to 移除 it below */
		ObjectAddressSet(trig, TriggerRelationId, pg_trigger->oid);
		add_exact_object_address(&trig, objects);
	}

	/* 使 the 依赖 removal 可见 to the deletion below */
	CommandCounterIncrement();
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* 执行ne */
	free_object_addresses(objects);
	systable_endscan(scan);
	table_close(tgrel, RowExclusiveLock);
}

/*
 * Before acquiring 锁 on an 索引, acquire the same 锁 on the owning table.
 *
 */
struct AttachIndexCallbackState
{
	Oid			partitionOid;
	Oid			parentTblOid;
	bool		lockedParentTbl;
};

static void
RangeVarCallbackForAttachIndex(const RangeVar *rv, Oid relOid, Oid oldRelOid,
							   void *arg)
{
	struct AttachIndexCallbackState *state;
	Form_pg_class classform;
	HeapTuple	tuple;

	state = (struct AttachIndexCallbackState *) arg;

	if (!state->lockedParentTbl)
	{
		LockRelationOid(state->parentTblOid, AccessShareLock);
		state->lockedParentTbl = true;
	}

	/*
	 * If we 先前 locked some other 堆, and the name we're looking up no longer
	 * refers to an 索引 on that relation, release the now-useless 锁. XXX maybe we
	 * should do *之后* we verify whether the 索引 does not actually belong to the
	 * same relation ...
	 */
	if (relOid != oldRelOid && OidIsValid(state->partitionOid))
	{
		UnlockRelationOid(state->partitionOid, AccessShareLock);
		state->partitionOid = InvalidOid;
	}

	/* 未找到关系，因此无需锁定或权限检查。 */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除，因此无需操作 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (classform->relkind != RELKIND_PARTITIONED_INDEX &&
		classform->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("\"%s\" is not an index", rv->relname)));
	ReleaseSysCache(tuple);

	/*
	 * Since we need 仅 examine the 堆's tupledesc, an access share 锁 on it
	 * (preventing any DDL) is sufficient.
	 */
	state->partitionOid = IndexGetRelation(relOid, false);
	LockRelationOid(state->partitionOid, AccessShareLock);
}

/*
 * ALTER INDEX i1 ATTACH PARTITION i2
 */
static ObjectAddress
ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx, RangeVar *name)
{
	Relation	partIdx;
	Relation	partTbl;
	Relation	parentTbl;
	ObjectAddress address;
	Oid			partIdxId;
	Oid			currParent;
	struct AttachIndexCallbackState state;

	/*
	 * 我们需要 obtain 锁 on the 索引 'name' to modify it, but we 也 need to read its
	 * owning table's 元组描述符 -- so 我们需要 锁 both. To avoid deadlocks, obtain 锁 on
	 * the table 之前 doing so on the 索引. Furthermore, 我们需要 examine the 父表 of the
	 * partition, so 锁 that one too.
	 *
	 */
	state.partitionOid = InvalidOid;
	state.parentTblOid = parentIdx->rd_index->indrelid;
	state.lockedParentTbl = false;
	partIdxId =
		RangeVarGetRelidExtended(name, AccessExclusiveLock, 0,
								 RangeVarCallbackForAttachIndex,
								 (void *) &state);
	/* 不存在？ */
	if (!OidIsValid(partIdxId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", name->relname)));

	/* no dead锁 risk: RangeVarGetRel标识符Extended 已经 acquired the 锁 */
	partIdx = relation_open(partIdxId, AccessExclusiveLock);

	/* we 已经 h旧 锁s on both 表s, so this is safe: */
	parentTbl = relation_open(parentIdx->rd_index->indrelid, AccessShareLock);
	partTbl = relation_open(partIdx->rd_index->indrelid, NoLock);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partIdx));

	/* Silently 执行 nothing if 已经 in the right 状态 */
	currParent = partIdx->rd_rel->relispartition ?
		get_partition_parent(partIdxId, false) : InvalidOid;
	if (currParent != RelationGetRelid(parentIdx))
	{
		IndexInfo  *childInfo;
		IndexInfo  *parentInfo;
		AttrMap    *attmap;
		bool		found;
		int			i;
		PartitionDesc partDesc;
		Oid			constraintOid,
					cldConstrId = InvalidOid;

		/*
		 * If this partition 已经 has an 索引 attached, refuse the operation.
		 *
		 */
		refuseDupeIndexAttach(parentIdx, partIdx, partTbl);

		if (OidIsValid(currParent))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is already attached to another index.",
							   RelationGetRelationName(partIdx))));

		/* Make sure it 索引es a 分区 of the other 索引's 表 */
		partDesc = RelationGetPartitionDesc(parentTbl, true);
		found = false;
		for (i = 0; i < partDesc->nparts; i++)
		{
			if (partDesc->oids[i] == state.partitionOid)
			{
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is not an index on any partition of table \"%s\".",
							   RelationGetRelationName(partIdx),
							   RelationGetRelationName(parentTbl))));

		/* Ensure the 索引es are compatible */
		childInfo = BuildIndexInfo(partIdx);
		parentInfo = BuildIndexInfo(parentIdx);
		attmap = build_attrmap_by_name(RelationGetDescr(partTbl),
									   RelationGetDescr(parentTbl),
									   false);
		if (!CompareIndexInfo(childInfo, parentInfo,
							  partIdx->rd_indcollation,
							  parentIdx->rd_indcollation,
							  partIdx->rd_opfamily,
							  parentIdx->rd_opfamily,
							  attmap))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("The index definitions do not match.")));

		/*
		 * If there is a 约束 in the parent, 确保 there is one in the child too.
		 *
		 */
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(parentTbl),
														RelationGetRelid(parentIdx));

		if (OidIsValid(constraintOid))
		{
			cldConstrId = get_relation_idx_constraint_oid(RelationGetRelid(partTbl),
														  partIdxId);
			if (!OidIsValid(cldConstrId))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
								RelationGetRelationName(partIdx),
								RelationGetRelationName(parentIdx)),
						 errdetail("The index \"%s\" belongs to a constraint in table \"%s\" but no constraint exists for index \"%s\".",
								   RelationGetRelationName(parentIdx),
								   RelationGetRelationName(parentTbl),
								   RelationGetRelationName(partIdx))));
		}

		/* All good -- 执行 it */
		IndexSetParentIndex(partIdx, RelationGetRelid(parentIdx));
		if (OidIsValid(constraintOid))
			ConstraintSetParentConstraint(cldConstrId, constraintOid,
										  RelationGetRelid(partTbl));

		free_attrmap(attmap);

		validatePartitionedIndex(parentIdx, parentTbl);
	}

	relation_close(parentTbl, AccessShareLock);
	/* 保持 these 锁s till 提交 */
	relation_close(partTbl, NoLock);
	relation_close(partIdx, NoLock);

	return address;
}

/*
 * Verify whether the given partition 已经 包含 an 索引 attached to the given 分区索引.
 * If so, raise an error.
 */
static void
refuseDupeIndexAttach(Relation parentIdx, Relation partIdx, Relation partitionTbl)
{
	Oid			existingIdx;

	existingIdx = index_get_partition(partitionTbl,
									  RelationGetRelid(parentIdx));
	if (OidIsValid(existingIdx))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
						RelationGetRelationName(partIdx),
						RelationGetRelationName(parentIdx)),
				 errdetail("Another index is already attached for partition \"%s\".",
						   RelationGetRelationName(partitionTbl))));
}

/*
 * Verify whether the set of attached partition 索引 to a parent 索引 on a 分区表 is
 * complete. If it is, mark the parent 索引 valid. This 应是 called each time a
 * partition 索引 is attached.
 *
 */
static void
validatePartitionedIndex(Relation partedIdx, Relation partedTbl)
{
	Relation	inheritsRel;
	SysScanDesc scan;
	ScanKeyData key;
	int			tuples = 0;
	HeapTuple	inhTup;
	bool		updated = false;

	Assert(partedIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for this parent 索引. Count each valid 索引 we find
	 * (verifying the pg_index entry for each), and if we reach the total amount
	 * we expect, 我们可以 mark this parent 索引 as valid.
	 */
	inheritsRel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(partedIdx)));
	scan = systable_beginscan(inheritsRel, InheritsParentIndexId, true,
							  NULL, 1, &key);
	while ((inhTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(inhTup);
		HeapTuple	indTup;
		Form_pg_index indexForm;

		indTup = SearchSysCache1(INDEXRELID,
								 ObjectIdGetDatum(inhForm->inhrelid));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u", inhForm->inhrelid);
		indexForm = (Form_pg_index) GETSTRUCT(indTup);
		if (indexForm->indisvalid)
			tuples += 1;
		ReleaseSysCache(indTup);
	}

	/* Done with pg_继承s */
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	/*
	 * If we found as many 继承的 索引 as the 分区表 has partitions, we're good; update
	 * pg_index to set indisvalid.
	 */
	if (tuples == RelationGetPartitionDesc(partedTbl, true)->nparts)
	{
		Relation	idxRel;
		HeapTuple	indTup;
		Form_pg_index indexForm;

		idxRel = table_open(IndexRelationId, RowExclusiveLock);
		indTup = SearchSysCacheCopy1(INDEXRELID,
									 ObjectIdGetDatum(RelationGetRelid(partedIdx)));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u",
				 RelationGetRelid(partedIdx));
		indexForm = (Form_pg_index) GETSTRUCT(indTup);

		indexForm->indisvalid = true;
		updated = true;

		CatalogTupleUpdate(idxRel, &indTup->t_self, indTup);

		table_close(idxRel, RowExclusiveLock);
		heap_freetuple(indTup);
	}

	/*
	 * If this 索引 is in turn a partition of a larger 索引, validating it might
	 * cause the parent to become valid 也. Try that.
	 */
	if (updated && partedIdx->rd_rel->relispartition)
	{
		Oid			parentIdxId,
					parentTblId;
		Relation	parentIdx,
					parentTbl;

		/* 使 sure we see the 验证 we 仅 d标识符 */
		CommandCounterIncrement();

		parentIdxId = get_partition_parent(RelationGetRelid(partedIdx), false);
		parentTblId = get_partition_parent(RelationGetRelid(partedTbl), false);
		parentIdx = relation_open(parentIdxId, AccessExclusiveLock);
		parentTbl = relation_open(parentTblId, AccessExclusiveLock);
		Assert(!parentIdx->rd_index->indisvalid);

		validatePartitionedIndex(parentIdx, parentTbl);

		relation_close(parentIdx, AccessExclusiveLock);
		relation_close(parentTbl, AccessExclusiveLock);
	}
}

/*
 * Return an OID list of 约束 that reference the given relation that are marked
 * as having a parent 约束.
 */
static List *
GetParentedForeignKeyRefs(Relation partition)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData key[2];
	List	   *constraints = NIL;

	/*
	 * If no 索引, or no columns are referenceable by FKs, 我们可以 avoid the 扫描.
	 *
	 */
	if (RelationGetIndexList(partition) == NIL ||
		bms_is_empty(RelationGetIndexAttrBitmap(partition,
												INDEX_ATTR_BITMAP_KEY)))
		return NIL;

	/* Search for 约束s referencing this 表 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(partition)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));

	/* XXX This is a seq扫描, as we 执行n't have a usable 索引 */
	scan = systable_beginscan(pg_constraint, InvalidOid, true, NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * We 仅 need to process 约束 that are part of larger ones.
		 */
		if (!OidIsValid(constrForm->conparentid))
			continue;

		constraints = lappend_oid(constraints, constrForm->oid);
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return constraints;
}

/*
 * During DETACH PARTITION, verify that any foreign keys pointing to the 分区表
 * would not become 无效. An error is raised 如有 referenced values exist.
 *
 */
static void
ATDetachCheckNoForeignKeyRefs(Relation partition)
{
	List	   *constraints;
	ListCell   *cell;

	constraints = GetParentedForeignKeyRefs(partition);

	foreach(cell, constraints)
	{
		Oid			constrOid = lfirst_oid(cell);
		HeapTuple	tuple;
		Form_pg_constraint constrForm;
		Relation	rel;
		Trigger		trig = {0};

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		Assert(OidIsValid(constrForm->conparentid));
		Assert(constrForm->confrelid == RelationGetRelid(partition));

		/* prevent 数据 变更s into the referencing 表 直到 提交 */
		rel = table_open(constrForm->conrelid, ShareLock);

		trig.tgoid = InvalidOid;
		trig.tgname = NameStr(constrForm->conname);
		trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
		trig.tgisinternal = true;
		trig.tgconstrrelid = RelationGetRelid(partition);
		trig.tgconstrindid = constrForm->conindid;
		trig.tgconstraint = constrForm->oid;
		trig.tgdeferrable = false;
		trig.tginitdeferred = false;
		/* we 需要n't fill in remaining 字段s */

		RI_PartitionRemove_Check(&trig, rel, partition);

		ReleaseSysCache(tuple);

		table_close(rel, NoLock);
	}
}

/*
 * resolve column 压缩 specification to 压缩 method.
 */
static char
GetAttributeCompression(Oid atttypid, char *compression)
{
	char		cmethod;

	if (compression == NULL || strcmp(compression, "default") == 0)
		return InvalidCompressionMethod;

	/*
	 * To specify a nondefault method, the column 数据类型 必须是 toastable. Note this
	 * says nothing about whether the column's attstorage setting permits 压缩; we
	 * intentionally allow attstorage and attcompression to be independent. But
	 * with a non-toastable type, attstorage could not be set to a value that
	 * would permit 压缩. 我们不 actually need to enforce this, since nothing bad
	 * would happen if attcompression were non-default; it would 从不 be consulted.
	 * But it seems more 用户-friendly to complain about a certainly-useless
	 * attempt to set the property.
	 *
	 *
	 */
	if (!TypeIsToastable(atttypid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s does not support compression",
						format_type_be(atttypid))));

	cmethod = CompressionNameToMethod(compression);
	if (!CompressionMethodIsValid(cmethod))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid compression method \"%s\"", compression)));

	return cmethod;
}

/*
 * resolve column 存储 specification
 */
static char
GetAttributeStorage(Oid atttypid, const char *storagemode)
{
	char		cstorage = 0;

	if (pg_strcasecmp(storagemode, "plain") == 0)
		cstorage = TYPSTORAGE_PLAIN;
	else if (pg_strcasecmp(storagemode, "external") == 0)
		cstorage = TYPSTORAGE_EXTERNAL;
	else if (pg_strcasecmp(storagemode, "extended") == 0)
		cstorage = TYPSTORAGE_EXTENDED;
	else if (pg_strcasecmp(storagemode, "main") == 0)
		cstorage = TYPSTORAGE_MAIN;
	else if (pg_strcasecmp(storagemode, "default") == 0)
		cstorage = get_typstorage(atttypid);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid storage type \"%s\"",
						storagemode)));

	/*
	 * safety check: do not allow toasted 存储 modes unless column datatype is
	 * TOAST-aware.
	 */
	if (!(cstorage == TYPSTORAGE_PLAIN || TypeIsToastable(atttypid)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(atttypid))));

	return cstorage;
}
